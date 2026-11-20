// SPDX-License-Identifier: MIT
// The emulation thread (frontend/emu_thread.hpp) and its building blocks:
//  - TripleBuffer (the frame mailbox): latest-value semantics, and a two-thread stress run in
//    which the reader must never see a torn or older frame;
//  - CommandQueue: order, closed-queue fallback, several producers against a draining worker;
//  - EmuThread::call/post: inline before start, between frames while running, nested calls;
//  - a real-time harness: the real EmuThread, core (the built-in test ROM, tests/test_rom.hpp) and AudioOutput against a
//    sound-card thread pulling 10 ms periods (WASAPI-like), while the UI thread stalls for
//    170 ms every half second (a Windows modal resize, a swapchain recreation). With emulation on
//    its own thread that must give 0 underruns.
// Build with -DBITFORGE_TSAN=ON to run all of it under ThreadSanitizer (timing checks are skipped
// there: instrumented emulation is slower than real time).
#include "test_rom.hpp"
#include "frontend/emu_thread.hpp"
#include "frontend/thread_sync.hpp"
#include "speedrun/autosplit.hpp"
#include "speedrun/race.hpp"
#include "utils/logging.hpp"
#include "utils/sha1.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define BITFORGE_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define BITFORGE_UNDER_TSAN 1
#endif
#endif
#ifndef BITFORGE_UNDER_TSAN
#define BITFORGE_UNDER_TSAN 0
#endif

using namespace bitforge::frontend;

namespace {

int checks_failed = 0;

// Audio silence a UI-stall harness run may have. None locally; CI runners (GitHub sets CI)
// are shared, noisy VMs where a busy neighbour can starve the queue for a period or two.
const double kAllowedSilenceMs = std::getenv("CI") ? 100.0 : 0.0;

// Fast-forward speed script. After audio grace, a normal-speed baseline and a fast-forward
// run of the same wall time, a short settle, a counted recovery window, then rewind.
constexpr double kFastForwardMinRatio = 1.2;  // fast-forward must beat normal speed by at least this
constexpr double kSpeedWarmupS = 2.8;
constexpr double kSpeedPhaseS = 1.5;
constexpr double kSpeedSettleS = static_cast<double>(EmuThread::kUnderrunLogSettleNs) / 1e9;
constexpr double kSpeedRecoverS = 1.0;
constexpr double kSpeedRewindS = 1.0;
constexpr double kSpeedScriptS =
    kSpeedWarmupS + kSpeedPhaseS + kSpeedPhaseS + kSpeedSettleS + kSpeedRecoverS + kSpeedRewindS;

auto count_underrun_warnings() -> int {
    int n = 0;
    const int lines = bitforge::utils::Logger::line_count();
    for (int i = 0; i < lines; i++) {
        const auto line = bitforge::utils::Logger::line(i);
        if (line.level == bitforge::utils::LogLevel::Warning &&
            line.text.find("Audio underrun:") != std::string::npos) {
            n++;
        }
    }
    return n;
}

// Temporarily show Warning lines in the System ring so tests can see underrun reports.
struct UnderrunLogCapture {
    UnderrunLogCapture() {
        bitforge::utils::Logger::set_level(bitforge::utils::LogLevel::Warning);
        bitforge::utils::Logger::clear();
    }
    ~UnderrunLogCapture() { bitforge::utils::Logger::set_level(bitforge::utils::LogLevel::Error); }
};

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

// ---- TripleBuffer --------------------------------------------------------------------------

auto test_mailbox_latest() -> void {
    TripleBuffer<int> box;
    CHECK(!box.acquire());  // nothing published yet
    box.write_buffer() = 1;
    box.publish();
    CHECK(box.acquire());
    CHECK(box.read_buffer() == 1);
    CHECK(!box.acquire());  // already taken
    CHECK(box.read_buffer() == 1);
    box.write_buffer() = 2;
    box.publish();
    box.write_buffer() = 3;
    box.publish();          // the reader only ever gets the newest
    CHECK(box.acquire());
    CHECK(box.read_buffer() == 3);
    // The writer can keep going while the reader holds its buffer.
    for (int i = 4; i < 10; i++) {
        box.write_buffer() = i;
        box.publish();
        CHECK(box.read_buffer() == 3);
    }
    CHECK(box.acquire() && box.read_buffer() == 9);
}

struct Stamped {
    uint64_t seq = 0;
    std::vector<uint64_t> payload;  // every element == seq when consistent
};

auto test_mailbox_threads() -> void {
    TripleBuffer<Stamped> box;
    constexpr uint64_t kFrames = 100'000;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint64_t s = 1; s <= kFrames; s++) {
            Stamped& f = box.write_buffer();
            f.seq = s;
            f.payload.assign(64 + s % 64, s);  // sizes change too (like 256 <-> 512 wide frames)
            box.publish();
        }
        done.store(true, std::memory_order_release);
    });
    uint64_t last = 0, taken = 0;
    bool torn = false, backwards = false;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        if (box.acquire()) {
            const Stamped& f = box.read_buffer();
            taken++;
            if (f.seq < last) backwards = true;
            last = f.seq;
            for (uint64_t v : f.payload) {
                if (v != f.seq) torn = true;
            }
        }
        if (finished && !box.acquire()) break;
    }
    writer.join();
    if (box.acquire()) last = box.read_buffer().seq;
    CHECK(!torn);
    CHECK(!backwards);
    CHECK(last == kFrames);  // the final publish is always seen
    CHECK(taken > 0);
}

// ---- CommandQueue --------------------------------------------------------------------------

auto test_queue_basics() -> void {
    CommandQueue<std::vector<int>> q;
    std::vector<int> log;
    CHECK(!q.post([](std::vector<int>& v) { v.push_back(0); }));  // closed until open()
    q.open();
    CHECK(q.post([](std::vector<int>& v) { v.push_back(1); }));
    CHECK(q.post([](std::vector<int>& v) { v.push_back(2); }));
    CHECK(q.post([&q](std::vector<int>& v) {
        v.push_back(3);
        q.post([](std::vector<int>& w) { w.push_back(5); });  // posted from a command: next drain
    }));
    CHECK(q.drain(log) == 3);
    CHECK((log == std::vector<int>{1, 2, 3}));
    CHECK(q.post([](std::vector<int>& v) { v.push_back(4); }));
    CHECK(q.close_and_drain(log) == 2);
    CHECK((log == std::vector<int>{1, 2, 3, 5, 4}));
    CHECK(!q.post([](std::vector<int>& v) { v.push_back(6); }));  // closed: caller runs it
    q.run_inline([](std::vector<int>& v) { v.push_back(6); }, log);
    CHECK(log.back() == 6);
    CHECK(q.drain(log) == 0);
}

auto test_queue_threads() -> void {
    struct Ctx {
        std::vector<int> seen[2];
    };
    CommandQueue<Ctx> q;
    Ctx ctx;
    q.open();
    constexpr int kEach = 20'000;
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        while (!stop.load(std::memory_order_acquire)) q.drain(ctx);
        q.close_and_drain(ctx);
    });
    std::thread producers[2];
    for (int p = 0; p < 2; p++) {
        producers[p] = std::thread([&, p] {
            for (int i = 0; i < kEach; i++) {
                q.post([p, i](Ctx& c) { c.seen[p].push_back(i); });
            }
        });
    }
    for (auto& t : producers) t.join();
    stop.store(true, std::memory_order_release);
    worker.join();
    for (int p = 0; p < 2; p++) {
        CHECK(static_cast<int>(ctx.seen[p].size()) == kEach);
        bool ordered = true;
        for (int i = 0; i < static_cast<int>(ctx.seen[p].size()); i++) ordered = ordered && ctx.seen[p][static_cast<size_t>(i)] == i;
        CHECK(ordered);  // each producer's commands run once, in its order
    }
}

// ---- EmuThread commands --------------------------------------------------------------------

auto test_emu_calls() -> void {
    EmuThread emu;
    // Not running: call() runs right here.
    const auto here = std::this_thread::get_id();
    CHECK(emu.call([&](EmuThread::Core&) { return std::this_thread::get_id(); }) == here);
    emu.start();
    const auto there = emu.call([](EmuThread::Core&) { return std::this_thread::get_id(); });
    CHECK(there != here);
    // Nested call from a command runs inline (no deadlock), on the emulation thread.
    const int nested = emu.call([&emu](EmuThread::Core& c) {
        c.advance_one = false;
        return emu.call([](EmuThread::Core&) { return 42; });
    });
    CHECK(nested == 42);
    // Posts run in order before a later call returns.
    std::vector<int> order;
    for (int i = 0; i < 5; i++) emu.post([&order, i](EmuThread::Core&) { order.push_back(i); });
    emu.call([](EmuThread::Core&) {});
    CHECK((order == std::vector<int>{0, 1, 2, 3, 4}));
    emu.call([](EmuThread::Core& c) { c.audio_sync = true; });
    CHECK(emu.status().state == RunState::Stopped);
    emu.stop();
    emu.stop();  // twice is fine
    CHECK(emu.call([&](EmuThread::Core&) { return std::this_thread::get_id(); }) == here);
    CHECK(emu.core().audio_sync);
    // Start again after a stop.
    emu.start();
    CHECK(emu.call([](EmuThread::Core&) { return std::this_thread::get_id(); }) != here);
    emu.stop();
}

// ---- real-time harness ---------------------------------------------------------------------

struct HarnessResult {
    uint64_t frames = 0;
    double elapsed_s = 0.0;
    uint64_t underruns = 0;
    double silence_ms = 0.0;
    uint64_t frames_seen = 0;
    bool frames_ok = true;
    double longest_stall_ms = 0.0;
    std::string state_error;
    // speed_script runs only:
    double fast_rate = 0.0;          // best emulated/displayed frame ratio seen while fast-forwarding
    uint64_t fast_frames = 0;        // frames run during the fast-forward phase
    uint64_t normal_frames = 0;      // frames run during the same-length normal-speed baseline
    uint64_t recover_underruns = 0;  // underruns in the post-settle normal-speed window
    double recover_silence_ms = 0.0;
    bool recover_seen = false;
    uint64_t rewound = 0;            // frames gone back while rewinding
    bool saw_rewind_status = false;
};

// The App's boot and stop sequence, as calls.
auto boot(EmuThread& emu, const std::string& rom) -> bool {
    const bool ok = emu.call([&rom](EmuThread::Core& c) {
        c.audio.end_session();
        if (!c.emu.load(rom)) return false;
        c.republish = true;
        return true;
    });
    if (!ok) return false;
    emu.call([](EmuThread::Core& c) {
        c.state = RunState::Running;
        c.audio.begin_session();
    });
    return true;
}

// The memory image: nothing is published until the UI asks, then one per frame (and after a
// command batch, so an edit shows while paused); writes and freezes go through the command
// queue while the UI thread reads every byte of each image (ThreadSanitizer checks the handoff).
volatile uint32_t g_memory_sink = 0;

auto test_memory_view() -> void {
    using bitforge::debug::Region;
    using bitforge::debug::ValueSize;
    constexpr uint32_t kEdit = 0x1F000;
    constexpr uint32_t kFrozen = 0x1F010;
    constexpr uint64_t kTimeoutNs = 5'000'000'000ULL;
    EmuThread emu;
    emu.core().emu.set_data_dir((std::filesystem::temp_directory_path() / "snes_thread_tests_memory").string());
    const std::string rom = test_rom::write_test_rom(0);
    emu.start();
    CHECK(boot(emu, rom));
    // Off: no images.
    emu.call([](EmuThread::Core&) {});
    SDL_DelayNS(50'000'000);
    CHECK(!emu.take_memory());

    emu.set_memory_view(true);
    auto wait_for = [&emu](auto&& done) {
        const uint64_t until = SDL_GetTicksNS() + kTimeoutNs;
        uint64_t images = 0;
        while (SDL_GetTicksNS() < until) {
            if (emu.take_memory()) {
                images++;
                const MemoryImage& m = emu.memory();
                uint32_t sum = 0;
                for (const uint8_t b : m.region(Region::Wram)) sum += b;  // read every byte
                g_memory_sink = sum;
                if (done(m)) return images;
            }
            SDL_DelayNS(1'000'000);
        }
        return uint64_t{0};
    };
    uint64_t first_frame = 0;
    CHECK(wait_for([&](const MemoryImage& m) {
        first_frame = m.frame;
        return m.rom_path == rom && m.region(Region::Wram).size() == bitforge::debug::kWramBytes && m.frame > 10;
    }) > 0);

    // A one-shot write and a freeze while running.
    emu.post([](EmuThread::Core& c) { c.emu.write_memory(Region::Wram, kEdit, ValueSize::Bits8, 0xA5); });
    emu.post([](EmuThread::Core& c) {
        c.emu.set_watches({{"HP", Region::Wram, kFrozen, {ValueSize::Bits16, bitforge::debug::Format::Hex}, true, 0xBEEF}});
    });
    uint64_t later_frame = 0;
    CHECK(wait_for([&](const MemoryImage& m) {
        later_frame = m.frame;
        const auto w = m.region(Region::Wram);
        return w[kEdit] == 0xA5 && bitforge::debug::read_value(w, kFrozen, ValueSize::Bits16) == 0xBEEF;
    }) > 0);
    CHECK(later_frame > first_frame);

    // Paused: an edit still shows (the command batch publishes).
    emu.call([](EmuThread::Core& c) { c.state = RunState::Paused; });
    emu.post([](EmuThread::Core& c) { c.emu.write_memory(Region::Wram, kEdit, ValueSize::Bits8, 0x5A); });
    CHECK(wait_for([](const MemoryImage& m) { return m.region(Region::Wram)[kEdit] == 0x5A; }) > 0);

    // Off again: publishing stops.
    emu.call([](EmuThread::Core& c) { c.state = RunState::Running; });
    emu.set_memory_view(false);
    emu.call([](EmuThread::Core&) {});
    emu.call([](EmuThread::Core&) {});
    emu.take_memory();
    SDL_DelayNS(100'000'000);
    CHECK(!emu.take_memory());
    emu.stop();
}

// The run timer on the emulation thread: an edited run is saved with its rules (their watch
// joins the game's list), auto-start and auto-splits fire from the frame hook, holding the rewind
// hotkey back past a split undoes it and playing on splits it once (game time exact), and a
// reset with a first PB waits for the answer, then lands in the .lss. The UI side only reads
// status copies, so ThreadSanitizer checks the handoff of the timer and its shared run.
auto test_run_timer() -> void {
    namespace sr = bitforge::speedrun;
    using bitforge::debug::Region;
    constexpr uint32_t kCounter = 0x0000;  // the test ROM's 8-bit frame counter
    constexpr uint32_t kStartAt = 20, kFirstSplit = 80, kSecondSplit = 140;
    constexpr uint64_t kTimeoutNs = 20'000'000'000ULL;  // generous: ThreadSanitizer is slow
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "snes_thread_tests_timer";
    std::filesystem::remove_all(dir);
    EmuThread emu;
    emu.core().emu.set_data_dir(dir.string());
    emu.core().speed.rewind = true;
    emu.core().speed.rewind_interval = 1;
    emu.core().apply_speed();
    const std::string rom = test_rom::write_test_rom(0);
    emu.start();
    CHECK(boot(emu, rom));
    auto wait_for = [&emu](auto&& done) {
        const uint64_t until = SDL_GetTicksNS() + kTimeoutNs;
        while (SDL_GetTicksNS() < until) {
            const EmuStatus st = emu.status();
            if (done(st)) return true;
            SDL_DelayNS(1'000'000);
        }
        return false;
    };
    CHECK(wait_for([](const EmuStatus& st) { return !st.splits_path.empty(); }));
    const EmuStatus booted = emu.status();
    // Pause while the run is set, so the counter can't pass the start before the rules exist.
    emu.call([](EmuThread::Core& c) { c.state = RunState::Paused; });
    const bitforge::debug::Watch counter{"Counter", Region::Wram, kCounter, {}, false, 0, 0};
    sr::Run run = sr::make_run("Test", "Any%", 2);
    run.auto_start = sr::Rule{{1, bitforge::debug::Test::ChangedTo, kStartAt}, counter};
    run.segments[0].auto_split = sr::Rule{{1, bitforge::debug::Test::ChangedTo, kFirstSplit}, counter};
    run.segments[1].auto_split = sr::Rule{{1, bitforge::debug::Test::ChangedTo, kSecondSplit}, counter};
    emu.call([&run](EmuThread::Core& c) {
        c.emu.write_memory(Region::Wram, kCounter, bitforge::debug::ValueSize::Bits8, 0);
        c.set_run(run);
        c.state = RunState::Running;
    });
    const EmuStatus set = emu.status();
    CHECK(set.watches_revision > booted.watches_revision);
    CHECK(std::filesystem::exists(set.splits_path) && std::filesystem::exists(sr::autosplit_path(set.splits_path)));

    // Auto-start, then the first split.
    CHECK(wait_for([](const EmuStatus& st) { return st.timer.current() == 1; }));
    // Hold rewind until the split is undone, then play on: it splits again, once.
    emu.set_speed_input(true, false);
    CHECK(wait_for([](const EmuStatus& st) { return st.timer.current() == 0 && st.timer.state() == sr::TimerState::Running; }));
    emu.set_speed_input(false, false);
    CHECK(wait_for([](const EmuStatus& st) { return st.timer.state() == sr::TimerState::Ended; }));
    const EmuStatus ended = emu.status();
    CHECK(ended.timer.current() == 2);
    // Game time is the frames between the triggers, whatever the rewind did.
    CHECK(ended.timer.splits()[0].game == sr::frames_to_nanos(kFirstSplit - kStartAt, sr::VideoStandard::Ntsc));
    CHECK(ended.timer.splits()[1].game == sr::frames_to_nanos(kSecondSplit - kStartAt, sr::VideoStandard::Ntsc));

    // Reset: a first finish is a PB, kept only once the prompt says so.
    emu.post([](EmuThread::Core& c) { c.splits.timer.reset(EmuThread::timer_clock()); });
    CHECK(wait_for([](const EmuStatus& st) { return st.timer.pending(); }));
    emu.post([](EmuThread::Core& c) { c.splits.timer.resolve_pending(true); });
    CHECK(wait_for([](const EmuStatus& st) { return !st.timer.pending(); }));
    emu.stop();
    const auto saved = sr::load_splits(ended.splits_path);
    CHECK(saved.has_value());
    if (saved) {
        CHECK(saved->run.attempt_count == 1 && sr::completed_runs(saved->run) == 1);
        // (the file keeps 100 ns ticks)
        CHECK(sr::format_timespan(saved->run.segments[1].pb.game.value_or(0)) == sr::format_timespan(ended.timer.splits()[1].game.value_or(-1)));
        CHECK(saved->run.segments[0].auto_split.has_value() && saved->run.auto_start.has_value());
    }
    std::filesystem::remove_all(dir);
}

// stall_emulation: the stalls happen on the emulation thread instead (a command that blocks):
// what the single-threaded loop did. That must underrun (control: the harness can see it).
// speed_script: after audio grace, a normal-speed baseline and a fast-forward run of the same
// length, a short settle, a counted recovery window, then rewind.
auto run_harness(double seconds, uint64_t stall_ns, uint64_t stall_every_ns, bool audio_sync, bool stall_emulation = false,
                 bool speed_script = false) -> HarnessResult {
    HarnessResult r;
    EmuThread emu;
    const std::string data_dir = (std::filesystem::temp_directory_path() / "snes_thread_tests").string();
    emu.core().emu.set_data_dir(data_dir);
    if (!emu.core().audio.open_unbound(Emulator::audio_sample_rate(), Emulator::audio_sample_rate())) {
        r.frames_ok = false;
        return r;
    }
    emu.core().audio_sync = audio_sync;
    emu.core().audio.set_rate_control(!audio_sync);
    SDL_AudioStream* stream = emu.core().audio.stream();
    emu.start();
    if (!boot(emu, test_rom::write_test_rom(0))) {
        emu.stop();
        r.frames_ok = false;
        return r;
    }

    // The sound card: 321 frames (10 ms at 32040 Hz) every 10 ms, on its own clock.
    std::atomic<bool> device_stop{false};
    std::thread device([&] {
        std::vector<int16_t> buf(321 * 2);
        uint64_t next = SDL_GetTicksNS();
        while (!device_stop.load(std::memory_order_acquire)) {
            next += 10'000'000ULL;
            const uint64_t now = SDL_GetTicksNS();
            if (next > now) SDL_DelayPrecise(next - now);
            SDL_GetAudioStreamData(stream, buf.data(), static_cast<int>(buf.size() * sizeof(int16_t)));
        }
    });

    // The UI thread: take each frame, write input, and stall now and then.
    const uint64_t start = SDL_GetTicksNS();
    const uint64_t end = start + static_cast<uint64_t>(seconds * 1e9);
    uint64_t next_stall = start + stall_every_ns;
    uint64_t last_frame = 0;
    uint64_t ff_start = 0, rewind_start = 0, normal_start = 0;
    uint64_t recover_underruns0 = 0;
    double recover_silence0 = 0.0;
    const double normal0 = kSpeedWarmupS;
    const double fast0 = normal0 + kSpeedPhaseS;
    const double settle0 = fast0 + kSpeedPhaseS;
    const double recover0 = settle0 + kSpeedSettleS;
    const double rewind0 = recover0 + kSpeedRecoverS;
    const double end_t = rewind0 + kSpeedRewindS;
    bool saved = speed_script, loaded = speed_script, checked_load = speed_script;
    int buttons = 0;
    while (SDL_GetTicksNS() < end) {
        emu.wait_frame(EmuThread::kFrameNs);
        if (emu.take_frame()) {
            const FrameImage& f = emu.frame();
            r.frames_seen++;
            if (f.frame < last_frame && !(loaded && !checked_load) && !speed_script) r.frames_ok = false;  // only a state load goes back
            if (loaded && f.frame < last_frame) checked_load = true;
            if (f.width <= 0 || f.height <= 0 || f.pixels.size() != static_cast<size_t>(f.width) * static_cast<size_t>(f.height)) {
                r.frames_ok = false;
            }
            last_frame = f.frame;
        }
        emu.set_input(0, static_cast<uint16_t>(buttons++ & 0x0F00), true);
        const EmuStatus st = emu.status();
        const uint64_t now = SDL_GetTicksNS();
        if (speed_script) {
            const double t = static_cast<double>(now - start) / 1e9;
            const bool in_normal = t >= normal0 && t < fast0;
            const bool in_ff = t >= fast0 && t < settle0;
            const bool in_recover = t >= recover0 && t < rewind0;
            const bool in_rewind = t >= rewind0 && t < end_t;
            emu.set_speed_input(in_rewind, in_ff);
            if (in_normal && !normal_start) normal_start = st.frames_run;
            if (!in_normal && normal_start && !r.normal_frames) r.normal_frames = st.frames_run - normal_start;
            if (in_ff && !ff_start) ff_start = st.frames_run;
            if (!in_ff && ff_start && !r.fast_frames) r.fast_frames = st.frames_run - ff_start;
            if (in_ff && st.fast_forward) r.fast_rate = std::max(r.fast_rate, st.fast_forward_rate);
            if (in_recover && !r.recover_seen) {
                recover_underruns0 = st.audio.underruns;
                recover_silence0 = st.audio.silence_ms;
                r.recover_seen = true;
            }
            if (in_recover) {
                r.recover_underruns = st.audio.underruns - recover_underruns0;
                r.recover_silence_ms = st.audio.silence_ms - recover_silence0;
            }
            if (in_rewind && !rewind_start) rewind_start = last_frame;
            if (in_rewind && st.rewinding) r.saw_rewind_status = true;
            if (in_rewind && last_frame < rewind_start) r.rewound = std::max(r.rewound, rewind_start - last_frame);
        }
        // F-key save and load early on (a load restarts the audio grace period, so they come
        // first: the rest of the run is counted).
        if (!saved && now > start + 300'000'000ULL) {
            r.state_error = emu.call([](EmuThread::Core& c) { return c.emu.save_state(9); });
            saved = true;
        } else if (saved && !loaded && now > start + 600'000'000ULL) {
            const std::string err = emu.call([](EmuThread::Core& c) {
                std::string why = c.emu.load_state(9);
                if (why.empty()) {
                    c.audio.flush();
                    c.republish = true;
                }
                return why;
            });
            if (!err.empty()) r.state_error = err;
            loaded = true;
        }
        if (stall_ns > 0 && now >= next_stall) {
            const uint64_t t0 = SDL_GetTicksNS();
            if (stall_emulation) {
                emu.call([stall_ns](EmuThread::Core&) { SDL_DelayPrecise(stall_ns); });
            } else {
                SDL_DelayPrecise(stall_ns);  // window resize, swapchain recreation, ...
            }
            r.longest_stall_ms = std::max(r.longest_stall_ms, static_cast<double>(SDL_GetTicksNS() - t0) / 1e6);
            next_stall += stall_every_ns;
        }
    }
    const EmuStatus st = emu.status();
    r.frames = st.frames_run;
    r.elapsed_s = static_cast<double>(SDL_GetTicksNS() - start) / 1e9;
    r.underruns = st.audio.underruns;
    r.silence_ms = st.audio.silence_ms;
    // Stop as the App does: a call, then join.
    emu.call([](EmuThread::Core& c) {
        c.audio.end_session();
        c.emu.unload();
        c.state = RunState::Stopped;
    });
    emu.stop();
    device_stop.store(true, std::memory_order_release);
    device.join();
    emu.core().audio.close();
    std::error_code ec;
    std::filesystem::remove_all(data_dir, ec);
    return r;
}

auto test_harness(bool audio_sync) -> void {
    const double seconds = 7.0;  // grace ends ~2.6 s in: 4+ s of counted play with 9 stalls
    const HarnessResult r = run_harness(seconds, 170'000'000ULL, 500'000'000ULL, audio_sync);
    std::printf("\n    %llu frames in %.2f s (%llu shown), UI stalls of %.0f ms every 0.5 s, underruns %llu (%.0f ms silence)",
                static_cast<unsigned long long>(r.frames), r.elapsed_s, static_cast<unsigned long long>(r.frames_seen),
                r.longest_stall_ms, static_cast<unsigned long long>(r.underruns), r.silence_ms);
    CHECK(r.frames_ok);
    CHECK(r.state_error.empty());
    CHECK(r.longest_stall_ms >= 165.0);
    CHECK(r.frames_seen > 0);
#if !BITFORGE_UNDER_TSAN
    const double fps = static_cast<double>(r.frames) / r.elapsed_s;
    CHECK(fps >= 57.0 && fps <= 63.0);  // ~60 fps throughout, whatever the UI does
    CHECK(r.silence_ms <= kAllowedSilenceMs);  // an underrun always adds silence
#endif
}

auto test_harness_timer() -> void { test_harness(false); }

auto test_harness_control() -> void {
    const UnderrunLogCapture logs;
    const HarnessResult r = run_harness(6.0, 170'000'000ULL, 500'000'000ULL, false, true);
    std::printf("\n    170 ms stalls on the emulation thread: underruns %llu (%.0f ms silence)",
                static_cast<unsigned long long>(r.underruns), r.silence_ms);
    CHECK(r.frames_ok);
#if !BITFORGE_UNDER_TSAN
    CHECK(r.underruns >= 3);  // one per stall after the start-up grace
    CHECK(count_underrun_warnings() >= 1);  // normal-speed underruns still warn
#endif
}
auto test_harness_audio_sync() -> void { test_harness(true); }

// Fast-forward then rewind through the thread, with the sound card running. Fast-forward only
// has to beat normal speed over the same wall time (not a fixed multiple). Audio may drop
// while it is on; after a short settle, a following normal-speed window must stay clean.
auto test_harness_speed() -> void {
    const UnderrunLogCapture logs;
    const HarnessResult r = run_harness(kSpeedScriptS, 0, 1'000'000'000ULL, false, false, true);
    const double ratio = r.normal_frames > 0
        ? static_cast<double>(r.fast_frames) / static_cast<double>(r.normal_frames)
        : 0.0;
    std::printf("\n    fast-forward: %llu frames vs %llu at normal speed in %.1f s (%.2fx, min %.1fx), "
                "rewound %llu frames, after settle: underruns %llu (%.0f ms silence)",
                static_cast<unsigned long long>(r.fast_frames),
                static_cast<unsigned long long>(r.normal_frames), kSpeedPhaseS, ratio,
                kFastForwardMinRatio, static_cast<unsigned long long>(r.rewound),
                static_cast<unsigned long long>(r.recover_underruns), r.recover_silence_ms);
    CHECK(r.frames_ok);
    CHECK(r.saw_rewind_status);
    CHECK(r.rewound >= 60);  // 1 s back at least (4 frames per step)
#if !BITFORGE_UNDER_TSAN
    CHECK(r.normal_frames > 0);
    CHECK(static_cast<double>(r.fast_frames) > kFastForwardMinRatio * static_cast<double>(r.normal_frames));
    CHECK(r.recover_seen);
    CHECK(r.recover_underruns == 0);
    CHECK(r.recover_silence_ms <= 0.0);
#endif
    CHECK(count_underrun_warnings() == 0);  // none while fast-forward/rewind is on, or in the settle after
}

// Race mode on the emulation thread: turning it on mid-run resets the attempt and power-cycles
// the game; the rewind and fast-forward hotkeys (held), the fast-forward toggle and frame
// advance do nothing and no rewind snapshots are kept; a run then split to the end in race mode
// writes its verification file (with the ROM's SHA-1) and is saved as race verified. Turning it
// off brings fast-forward back. ThreadSanitizer checks the status handoff of the race flag.
auto test_race_mode() -> void {
    namespace sr = bitforge::speedrun;
    constexpr uint64_t kTimeoutNs = 20'000'000'000ULL;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bitforge_thread_tests_race";
    std::filesystem::remove_all(dir);
    EmuThread emu;
    emu.core().emu.set_data_dir(dir.string());
    emu.core().speed.rewind = true;
    emu.core().speed.rewind_interval = 1;
    emu.core().speed.fast_forward_speed = 4;
    emu.core().apply_speed();
    emu.core().verify.emulator = "16bit Forge test";
    const std::string rom = test_rom::write_test_rom(3);
    emu.start();
    CHECK(boot(emu, rom));
    auto wait_for = [&emu](auto&& done) {
        const uint64_t until = SDL_GetTicksNS() + kTimeoutNs;
        while (SDL_GetTicksNS() < until) {
            if (done(emu.status())) return true;
            SDL_DelayNS(1'000'000);
        }
        return false;
    };
    CHECK(wait_for([](const EmuStatus& st) { return !st.splits_path.empty() && st.rewind_entries > 10; }));
    emu.call([](EmuThread::Core& c) {
        c.set_run(sr::make_run("Race", "Any%", 2));
        c.splits.timer.start(EmuThread::timer_clock());
    });
    CHECK(emu.status().timer.state() == sr::TimerState::Running && emu.status().timer.race_status() == sr::RaceStatus::Off);

    // On: the attempt is reset, the game restarts, the history is gone.
    emu.call([](EmuThread::Core& c) { c.set_race(true, EmuThread::timer_clock()); });
    const EmuStatus on = emu.status();
    CHECK(on.race && on.timer.state() == sr::TimerState::Ready && on.timer.run().attempts.size() == 1);
    CHECK(on.rewind_entries == 0 && on.frame_count < 30 && on.state == RunState::Running);

    // Held rewind / fast-forward and the toggle: ignored. Nothing goes into the history.
    emu.set_speed_input(true, true);
    emu.post([](EmuThread::Core& c) { c.fast_forward_toggled = true; });
    const uint64_t from = on.frame_count;
    CHECK(wait_for([from](const EmuStatus& st) { return st.frame_count > from + 60; }));
    const EmuStatus held = emu.status();
    CHECK(!held.rewinding && !held.fast_forward && held.rewind_entries == 0);
    emu.set_speed_input(false, false);
    // Frame advance while paused: ignored.
    emu.call([](EmuThread::Core& c) {
        c.state = RunState::Paused;
        c.advance_one = true;
    });
    const uint64_t paused_at = emu.status().frame_count;
    SDL_DelayNS(100'000'000);
    CHECK(emu.status().frame_count == paused_at);
    emu.call([](EmuThread::Core& c) { c.state = RunState::Running; });

    // A race run to the end: verified, its file written, saved as such.
    emu.call([](EmuThread::Core& c) { c.splits.timer.start(EmuThread::timer_clock()); });
    CHECK(wait_for([](const EmuStatus& st) { return st.timer.race_status() == sr::RaceStatus::Clean && st.timer.game_frames() > 10; }));
    emu.call([](EmuThread::Core& c) { c.splits.timer.split(EmuThread::timer_clock()); });
    CHECK(wait_for([](const EmuStatus& st) { return st.timer.game_frames() > 20; }));
    emu.call([](EmuThread::Core& c) { c.splits.timer.split(EmuThread::timer_clock()); });
    CHECK(wait_for([](const EmuStatus& st) { return !st.verification_path.empty(); }));  // written after the command batch
    const EmuStatus done = emu.status();
    CHECK(done.timer.race_status() == sr::RaceStatus::Verified);
    CHECK(done.verification_path == sr::verification_path(done.splits_path, 2));
    {
        std::ifstream in(done.verification_path, std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(json.find("\"race_verified\": true") != std::string::npos);
        CHECK(json.find(bitforge::utils::sha1_file(rom).value_or("?")) != std::string::npos);
        CHECK(json.find("\"emulator\": \"16bit Forge test\"") != std::string::npos);
    }
    emu.call([](EmuThread::Core& c) {
        c.splits.timer.reset(EmuThread::timer_clock());
        c.splits.timer.resolve_pending(true);
    });

    // Off: fast-forward works again.
    emu.call([](EmuThread::Core& c) { c.set_race(false, EmuThread::timer_clock()); });
    emu.set_speed_input(false, true);
    CHECK(wait_for([](const EmuStatus& st) { return !st.race && st.fast_forward; }));
    emu.set_speed_input(false, false);
    const std::string splits = emu.status().splits_path;
    emu.stop();
    const auto saved = sr::load_splits(splits);
    CHECK(saved && saved->run.attempts.size() == 2 && saved->run.attempts[1].race_verified && !saved->run.attempts[0].race_verified);
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    bitforge::utils::Logger::set_level(bitforge::utils::LogLevel::Error);
    SDL_Init(0);
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"mailbox: newest frame only, never waits", test_mailbox_latest},
        {"mailbox: two threads, no torn or old frames", test_mailbox_threads},
        {"command queue: order, closed fallback", test_queue_basics},
        {"command queue: two producers, one worker", test_queue_threads},
        {"emu thread: call/post, inline, nested, restart", test_emu_calls},
        {"emu thread: memory image, writes, freezes", test_memory_view},
        {"emu thread: run timer, auto-splits, rewind, splits file", test_run_timer},
        {"emu thread: race mode gating, verified run, file", test_race_mode},
        {"harness: UI stalls 170 ms, timer pacing", test_harness_timer},
        {"harness: UI stalls 170 ms, audio-sync pacing", test_harness_audio_sync},
        {"harness: same stalls on the emu thread (control)", test_harness_control},
        {"harness: fast-forward then rewind", test_harness_speed},
    };
    std::printf("=== Emulation thread tests%s ===\n", BITFORGE_UNDER_TSAN ? " (ThreadSanitizer: timing checks off)" : "");
    int passed = 0, failed = 0;
    for (const auto& t : tests) {
        const int before = checks_failed;
        std::printf("  %-48s", t.name);
        std::fflush(stdout);
        t.fn();
        if (checks_failed == before) { std::printf(" PASS\n"); passed++; } else { std::printf("\n  FAIL\n"); failed++; }
    }
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    SDL_Quit();
    return failed == 0 ? 0 : 1;
}
