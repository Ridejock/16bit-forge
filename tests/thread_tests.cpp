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
// Build with -DSNES_TSAN=ON to run all of it under ThreadSanitizer (timing checks are skipped
// there: instrumented emulation is slower than real time).
#include "test_rom.hpp"
#include "frontend/emu_thread.hpp"
#include "frontend/thread_sync.hpp"
#include "utils/logging.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define SNES_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SNES_UNDER_TSAN 1
#endif
#endif
#ifndef SNES_UNDER_TSAN
#define SNES_UNDER_TSAN 0
#endif

using namespace snes::frontend;

namespace {

int checks_failed = 0;

// Audio silence a harness run may have. None locally; CI runners (GitHub sets CI) are shared,
// noisy VMs where a busy neighbour can starve the queue for a period or two.
const double kAllowedSilenceMs = std::getenv("CI") ? 100.0 : 0.0;

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

// stall_emulation: the stalls happen on the emulation thread instead (a command that blocks):
// what the single-threaded loop did. That must underrun (control: the harness can see it).
// speed_script: hold fast-forward for 1.5 s, then rewind for 1 s (after the audio grace period).
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
    uint64_t ff_start = 0, rewind_start = 0;
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
            const bool ff = t >= 2.8 && t < 4.3, rw = t >= 4.6 && t < 5.6;
            emu.set_speed_input(rw, ff);
            if (ff && !ff_start) ff_start = st.frames_run;
            if (!ff && ff_start && !r.fast_frames) r.fast_frames = st.frames_run - ff_start;
            if (ff && st.fast_forward) r.fast_rate = std::max(r.fast_rate, st.fast_forward_rate);
            if (rw && !rewind_start) rewind_start = last_frame;
            if (rw && st.rewinding) r.saw_rewind_status = true;
            if (rw && last_frame < rewind_start) r.rewound = std::max(r.rewound, rewind_start - last_frame);
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
#if !SNES_UNDER_TSAN
    const double fps = static_cast<double>(r.frames) / r.elapsed_s;
    CHECK(fps >= 57.0 && fps <= 63.0);  // ~60 fps throughout, whatever the UI does
    CHECK(r.silence_ms <= kAllowedSilenceMs);  // an underrun always adds silence
#endif
}

auto test_harness_timer() -> void { test_harness(false); }

auto test_harness_control() -> void {
    const HarnessResult r = run_harness(6.0, 170'000'000ULL, 500'000'000ULL, false, true);
    std::printf("\n    170 ms stalls on the emulation thread: underruns %llu (%.0f ms silence)",
                static_cast<unsigned long long>(r.underruns), r.silence_ms);
    CHECK(r.frames_ok);
#if !SNES_UNDER_TSAN
    CHECK(r.underruns >= 3);  // one per stall after the start-up grace
#endif
}
auto test_harness_audio_sync() -> void { test_harness(true); }

// Fast-forward (4x) then rewind through the thread, with the sound card running: the game goes
// faster, then back in time, and the audio queue never underruns.
auto test_harness_speed() -> void {
    const HarnessResult r = run_harness(6.2, 0, 1'000'000'000ULL, false, false, true);
    std::printf("\n    fast-forward: %llu frames in 1.5 s (%.1fx), rewound %llu frames, underruns %llu (%.0f ms silence)",
                static_cast<unsigned long long>(r.fast_frames), r.fast_rate, static_cast<unsigned long long>(r.rewound),
                static_cast<unsigned long long>(r.underruns), r.silence_ms);
    CHECK(r.frames_ok);
    CHECK(r.saw_rewind_status);
    CHECK(r.rewound >= 60);  // 1 s back at least (4 frames per step)
#if !SNES_UNDER_TSAN
    CHECK(r.fast_frames >= 3 * 90);  // ~4x for 1.5 s
    CHECK(r.fast_rate >= 3.0);
    CHECK(r.silence_ms <= kAllowedSilenceMs);  // an underrun always adds silence
#endif
}

}  // namespace

int main() {
    snes::utils::Logger::set_level(snes::utils::LogLevel::Error);
    SDL_Init(0);
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"mailbox: newest frame only, never waits", test_mailbox_latest},
        {"mailbox: two threads, no torn or old frames", test_mailbox_threads},
        {"command queue: order, closed fallback", test_queue_basics},
        {"command queue: two producers, one worker", test_queue_threads},
        {"emu thread: call/post, inline, nested, restart", test_emu_calls},
        {"harness: UI stalls 170 ms, timer pacing", test_harness_timer},
        {"harness: UI stalls 170 ms, audio-sync pacing", test_harness_audio_sync},
        {"harness: same stalls on the emu thread (control)", test_harness_control},
        {"harness: fast-forward 4x, then rewind", test_harness_speed},
    };
    std::printf("=== Emulation thread tests%s ===\n", SNES_UNDER_TSAN ? " (ThreadSanitizer: timing checks off)" : "");
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
