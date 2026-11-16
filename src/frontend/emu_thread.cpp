// SPDX-License-Identifier: MIT
#include "frontend/emu_thread.hpp"
#include "frontend/frame_pacer.hpp"
#include "utils/logging.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>

namespace snes::frontend {

namespace {

constexpr uint64_t kNsPerSecond = 1'000'000'000ULL;
constexpr double kFrameBudgetMs = 1000.0 / Emulator::kNtscFrameRate;
constexpr uint64_t kAudioWaitStepNs = 250'000;      // poll interval when pacing by the audio queue
constexpr uint64_t kLogIntervalNs = kNsPerSecond;   // rate limit for the underrun warning
constexpr uint64_t kSlowLogIntervalNs = 10 * kNsPerSecond;
constexpr uint32_t kConnected = 1u << 16;
constexpr uint32_t kRewindHeld = 1u << 0;
constexpr uint32_t kFastHeld = 1u << 1;
constexpr uint64_t kUncappedBudgetNs = 14'000'000;  // per displayed frame when fast-forward is uncapped
constexpr int kMaxFastFrames = 60;                  // per displayed frame, whatever the speed
constexpr size_t kSilentFrameSamples = 2 * 533;     // one frame of stereo at 32040 Hz

// ScopeInput in one word: x (bits 0-8), y (9-17), offscreen, fire, cursor, turbo, pause (18-22).
auto pack_scope(const snes::ScopeInput& s) -> uint32_t {
    return static_cast<uint32_t>(s.x & 0x1FF) | static_cast<uint32_t>(s.y & 0x1FF) << 9 | (s.offscreen ? 1u << 18 : 0u) |
           (s.fire ? 1u << 19 : 0u) | (s.cursor ? 1u << 20 : 0u) | (s.turbo ? 1u << 21 : 0u) | (s.pause ? 1u << 22 : 0u);
}

auto unpack_scope(uint32_t v) -> snes::ScopeInput {
    snes::ScopeInput s;
    s.x = static_cast<int>(v & 0x1FF);
    s.y = static_cast<int>((v >> 9) & 0x1FF);
    s.offscreen = (v >> 18) & 1;
    s.fire = (v >> 19) & 1;
    s.cursor = (v >> 20) & 1;
    s.turbo = (v >> 21) & 1;
    s.pause = (v >> 22) & 1;
    return s;
}

} // namespace

auto EmuThread::start() -> void {
    if (running()) return;
    input_[0].fetch_or(kConnected, std::memory_order_relaxed);  // port 1 also takes the keyboard: always plugged in
    quit_.store(false, std::memory_order_release);
    commands_.open();
    thread_ = std::jthread([this] {
        thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
        // Frames are due every 16.6 ms whatever the UI does: ask the OS to schedule us promptly.
        if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH)) {
            utils::Logger::debug("Emulation thread: could not raise its priority: %s", SDL_GetError());
        }
        run();
    });
}

auto EmuThread::stop() -> void {
    if (!running()) return;
    quit_.store(true, std::memory_order_release);
    thread_.join();
    thread_ = std::jthread();
    thread_id_.store(std::thread::id{}, std::memory_order_release);
}

auto EmuThread::post(Command command) -> void {
    if (!commands_.post(command)) commands_.run_inline(command, core_);
}

auto EmuThread::set_input(int pad, uint16_t buttons, bool connected) -> void {
    if (pad < 0 || pad >= kPadCount) return;
    input_[static_cast<size_t>(pad)].store(buttons | (connected ? kConnected : 0u), std::memory_order_relaxed);
}

auto EmuThread::set_port_devices(const PortDevices& devices) -> void {
    for (size_t p = 0; p < devices.size(); p++) devices_[p].store(devices[p], std::memory_order_relaxed);
}

auto EmuThread::add_mouse_motion(int dx, int dy, bool left, bool right) -> void {
    mouse_dx_.fetch_add(dx, std::memory_order_relaxed);
    mouse_dy_.fetch_add(dy, std::memory_order_relaxed);
    mouse_buttons_.store((left ? 1u : 0u) | (right ? 2u : 0u), std::memory_order_relaxed);
}

auto EmuThread::set_scope(const snes::ScopeInput& input) -> void { scope_.store(pack_scope(input), std::memory_order_relaxed); }

auto EmuThread::set_speed_input(bool rewind, bool fast_forward) -> void {
    speed_input_.store((rewind ? kRewindHeld : 0u) | (fast_forward ? kFastHeld : 0u), std::memory_order_relaxed);
}

auto EmuThread::wait_frame(uint64_t timeout_ns) -> void {
    std::unique_lock lock(frame_mutex_);
    frame_cv_.wait_for(lock, std::chrono::nanoseconds(timeout_ns), [this] { return frames_published_ != frames_seen_; });
    frames_seen_ = frames_published_;
}

auto EmuThread::status() const -> EmuStatus {
    std::lock_guard lock(status_mutex_);
    return status_;
}

// ---- emulation thread ------------------------------------------------------------------------

auto EmuThread::run() -> void {
    utils::Logger::info("Emulation thread started");
    FramePacer pacer(kFrameNs, kMaxLagFrames);
    pacer.reset(SDL_GetTicksNS());
    fps_window_start_ns_ = pacer.next_ns();
    while (!quit_.load(std::memory_order_acquire)) {
        const size_t ran = commands_.drain(core_);
        if (core_.republish) {
            core_.republish = false;
            publish_frame();
        }
        if (ran > 0) publish_status();

        const bool stepped = step();
        publish_status();

        const uint64_t sleep_ns = pacer.advance(SDL_GetTicksNS());
        if (stepped && core_.audio_sync && wait_for_audio(pacer.next_ns() + kFrameNs)) {
            pacer.sync(SDL_GetTicksNS());  // the sound card's clock paced this frame
            continue;
        }
        if (sleep_ns > 0) SDL_DelayPrecise(sleep_ns);
    }
    commands_.close_and_drain(core_);  // a call() posted just before stop() still gets its answer
    core_.audio.end_session();         // nothing plays or counts through the teardown that follows
    publish_status();
    utils::Logger::info("Emulation thread stopped");
}

// One pass: the pause state, one emulated frame with its audio (if running), the recorders and
// the audio watch. Returns whether a frame was emulated.
auto EmuThread::step() -> bool {
    Core& c = core_;
    const bool held = exit_after_frames_ != 0 && c.emu.loaded() && c.emu.frame_count() >= exit_after_frames_;
    exit_reached_ = held;
    // The device only runs while emulation does (no ROM, stopped or paused: paused device,
    // nothing counted). Set before the step so the first frame after a (re)start is queued.
    c.audio.set_paused(!c.emu.loaded() || c.state != RunState::Running || held);

    const bool stepping = c.emu.loaded() && !held && (c.state == RunState::Running || c.advance_one);
    if (c.emu.rom_path() != c.rewind_rom) {  // another game (or none): its history goes
        c.rewind.clear();
        c.rewind_rom = c.emu.rom_path();
        c.frames_since_snapshot = 0;
        c.fast_forward_toggled = false;
    }
    rewinding_ = fast_forward_ = false;
    if (stepping) {
        const uint32_t speed = speed_input_.load(std::memory_order_relaxed);
        const bool running = c.state == RunState::Running;
        rewinding_ = running && c.speed.rewind && (speed & kRewindHeld) != 0;
        fast_forward_ = running && !rewinding_ && ((speed & kFastHeld) != 0 || c.fast_forward_toggled);
        read_input();
        const uint64_t t0 = SDL_GetTicksNS();
        int ran = 1;
        if (rewinding_) {
            rewind_frame();
        } else {
            ran = run_frames(fast_forward_);
        }
        fast_frames_window_ += static_cast<uint64_t>(ran);
        const double ms = static_cast<double>(SDL_GetTicksNS() - t0) / 1e6 / ran;
        frame_ms_avg_ += (ms - frame_ms_avg_) * 0.05;
        frame_ms_max_window_ = std::max(frame_ms_max_window_, ms);
        // Time between queued frames, measured at the push.
        const uint64_t push_ns = SDL_GetTicksNS();
        if (last_push_ns_ != 0) frame_gap_ms_max_ = std::max(frame_gap_ms_max_, static_cast<double>(push_ns - last_push_ns_) / 1e6);
        last_push_ns_ = push_ns;
        c.audio.push(step_audio_, step_msu_);
        fps_frames_++;
        frames_run_ += static_cast<uint64_t>(ran);
        c.advance_one = false;
        publish_frame();
    } else {
        last_push_ns_ = 0;  // paused or stopped: the next frame starts a new measurement
    }
    c.audio.service();

    const uint64_t now = SDL_GetTicksNS();
    watch_audio(now);
    if (now - fps_window_start_ns_ >= kNsPerSecond) {
        fps_ = static_cast<double>(fps_frames_) * static_cast<double>(kNsPerSecond) / static_cast<double>(now - fps_window_start_ns_);
        fast_rate_ = fps_frames_ ? static_cast<double>(fast_frames_window_) / static_cast<double>(fps_frames_) : 0.0;
        fps_frames_ = 0;
        fast_frames_window_ = 0;
        fps_window_start_ns_ = now;
        frame_ms_max_ = frame_ms_max_window_;
        frame_ms_max_window_ = 0.0;
    }
    return stepping;
}

auto EmuThread::read_input() -> void {
    PortDevices devices{};
    for (size_t p = 0; p < devices.size(); p++) devices[p] = devices_[p].load(std::memory_order_relaxed);
    const auto slots = pad_slots(devices);
    // A gamepad port with no controller behind its pad reads as empty; a multitap is always there.
    for (int p = 0; p < kPortCount; p++) {
        snes::Device device = devices[static_cast<size_t>(p)];
        if (device == snes::Device::Gamepad) {
            const auto pad = std::find_if(slots.begin(), slots.end(), [p](const PadSlot& s) { return s.port == p; });
            const bool connected = pad != slots.end() && (input_[static_cast<size_t>(pad - slots.begin())].load(std::memory_order_relaxed) & kConnected);
            if (!connected) device = snes::Device::None;
        }
        core_.emu.set_port_device(p, device);
    }
    for (size_t i = 0; i < slots.size(); i++) {
        if (slots[i].port < 0) continue;
        const uint32_t in = input_[i].load(std::memory_order_relaxed);
        core_.emu.set_pad(slots[i].port, slots[i].slot, (in & kConnected) ? static_cast<uint16_t>(in & 0xFFFF) : 0);
    }
    const int dx = mouse_dx_.exchange(0, std::memory_order_relaxed);
    const int dy = mouse_dy_.exchange(0, std::memory_order_relaxed);
    const uint32_t buttons = mouse_buttons_.load(std::memory_order_relaxed);
    for (int p = 0; p < kPortCount; p++) {
        if (devices[static_cast<size_t>(p)] == snes::Device::Mouse) core_.emu.add_mouse(p, dx, dy, buttons & 1, buttons & 2);
        if (devices[static_cast<size_t>(p)] == snes::Device::SuperScope) core_.emu.set_scope(p, unpack_scope(scope_.load(std::memory_order_relaxed)));
    }
}

// A rewind snapshot every `interval` emulated frames while rewind is on.
auto EmuThread::snapshot_if_due() -> void {
    Core& c = core_;
    if (!c.speed.rewind) return;
    if (++c.frames_since_snapshot < c.rewind.interval()) return;
    c.frames_since_snapshot = 0;
    c.rewind.push(c.emu.save_snapshot());
}

// Normal speed: one frame, its audio as is. Fast-forward: `speed` frames (or as many as fit in
// the frame budget when uncapped), their audio squeezed into one frame's worth so the queue stays
// level and nothing underruns or overflows.
auto EmuThread::run_frames(bool fast) -> int {
    Core& c = core_;
    const int target = fast ? c.speed.fast_forward_speed : 1;
    const uint64_t start = SDL_GetTicksNS();
    fast_audio_.clear();
    fast_msu_.clear();
    int ran = 0;
    do {
        c.emu.run_frame();
        snapshot_if_due();
        ran++;
        const auto apu = c.emu.audio_samples();
        const auto msu = c.emu.msu1_samples();
        fast_audio_.insert(fast_audio_.end(), apu.begin(), apu.end());
        fast_msu_.insert(fast_msu_.end(), msu.begin(), msu.end());
        if (fast && target == 0 && SDL_GetTicksNS() - start >= kUncappedBudgetNs) break;
    } while (fast && ran < (target == 0 ? kMaxFastFrames : std::min(target, kMaxFastFrames)));
    if (ran == 1) {
        step_audio_.swap(fast_audio_);
        step_msu_.swap(fast_msu_);
    } else {
        squeeze_audio(fast_audio_, ran, step_audio_);
        squeeze_audio(fast_msu_, ran, step_msu_);
    }
    return ran;
}

// One step back: the newest snapshot is loaded and one frame run from it for the picture. Its
// audio plays reversed, or silence takes its place (the queue keeps its level either way).
auto EmuThread::rewind_frame() -> void {
    Core& c = core_;
    c.frames_since_snapshot = 0;
    step_audio_.assign(kSilentFrameSamples, 0);
    step_msu_.clear();
    auto state = c.rewind.pop();
    if (!state || !c.emu.load_snapshot(*state)) return;  // nothing (left) to go back to: hold
    c.emu.run_frame();
    if (!c.speed.rewind_reverse_audio) return;
    auto reverse = [](std::span<const int16_t> samples, std::vector<int16_t>& out) {
        out.resize(samples.size());
        for (size_t f = 0; f + 1 < samples.size(); f += 2) {  // by stereo frame
            out[samples.size() - 2 - f] = samples[f];
            out[samples.size() - 1 - f] = samples[f + 1];
        }
    };
    reverse(c.emu.audio_samples(), step_audio_);
    reverse(c.emu.msu1_samples(), step_msu_);
}

auto EmuThread::publish_frame() -> void {
    FrameImage& image = frames_.write_buffer();
    const uint32_t* pixels = core_.emu.framebuffer();
    if (pixels) {
        image.width = core_.emu.width();
        image.height = core_.emu.height();
        image.pixels.assign(pixels, pixels + static_cast<size_t>(image.width) * static_cast<size_t>(image.height));
    } else {
        image.width = image.height = 0;
        image.pixels.clear();
    }
    image.frame = core_.emu.frame_count();
    image.test_pattern = core_.emu.loaded() && core_.emu.showing_test_pattern();
    frames_.publish();
    {
        std::lock_guard lock(frame_mutex_);
        frames_published_++;
    }
    frame_cv_.notify_one();
}

auto EmuThread::publish_status() -> void {
    EmuStatus s;
    const Core& c = core_;
    s.loaded = c.emu.loaded();
    s.state = c.state;
    s.rom_name = c.emu.rom_name();
    s.rom_path = c.emu.rom_path();
    s.test_pattern = s.loaded && c.emu.showing_test_pattern();
    s.frame_count = c.emu.frame_count();
    s.frames_run = frames_run_;
    s.fps = fps_;
    s.frame_ms_avg = frame_ms_avg_;
    s.frame_ms_max = std::max(frame_ms_max_, frame_ms_max_window_);
    s.exit_reached = exit_reached_;
    s.audio = c.audio.stats();
    const WavWriter& core_rec = c.audio.core_recording();
    s.core_recording = core_rec.is_open();
    if (s.core_recording) {
        s.core_recording_path = core_rec.path();
        s.core_recording_seconds = core_rec.seconds();
        s.core_recording_rate = core_rec.sample_rate();
    }
    s.msu1 = c.emu.has_msu1();
    s.rewinding = rewinding_;
    s.fast_forward = fast_forward_;
    s.fast_forward_speed = c.speed.fast_forward_speed;
    s.fast_forward_rate = fast_rate_;
    s.rewind_entries = c.rewind.entries();
    s.rewind_bytes = c.rewind.bytes();
    s.rewind_seconds = static_cast<double>(c.rewind.entries()) * c.rewind.interval() / Emulator::kNtscFrameRate;
    s.device_recording = c.audio.device_recording();
    if (s.device_recording) {
        s.device_recording_path = c.audio.device_recording_path();
        s.device_recording_seconds = c.audio.device_recording_seconds();
    }
    std::lock_guard lock(status_mutex_);
    status_ = std::move(s);
}

// Logs underruns (at most once a second) and sustained slow emulation (every 10 s), with the
// numbers that tell sync problems from speed problems.
auto EmuThread::watch_audio(uint64_t now) -> void {
    const AudioOutput::Stats s = core_.audio.stats();
    if (s.underruns < logged_underruns_) logged_underruns_ = s.underruns;  // a new session reset the counters
    if (s.underruns > logged_underruns_ && now - last_underrun_log_ns_ >= kLogIntervalNs) {
        utils::Logger::warning(utils::LogChannel::Audio, "Audio underrun: %llu total (+%llu), %.0f ms of silence so far | queue avg %.1f ms of %.0f, "
                               "rate %+.3f%%, pull %.1f ms | device played %.0f ms past the last queued frame | emulation %.2f ms/frame "
                               "(worst %.2f), %.1f fps, longest gap between frames %.0f ms (emulation thread)",
                               static_cast<unsigned long long>(s.underruns),
                               static_cast<unsigned long long>(s.underruns - logged_underruns_), s.silence_ms, s.level_ms,
                               s.target_ms, (s.ratio - 1.0) * 100.0,
                               s.source_rate > 0 ? 1000.0 * s.last_pull_frames / s.source_rate : 0.0, s.starved_ms, frame_ms_avg_,
                               std::max(frame_ms_max_, frame_ms_max_window_), fps_, frame_gap_ms_max_);
        frame_gap_ms_max_ = 0.0;
        logged_underruns_ = s.underruns;
        last_underrun_log_ns_ = now;
    }
    if (core_.emu.loaded() && core_.state == RunState::Running && frame_ms_avg_ > kFrameBudgetMs &&
        now - last_slow_log_ns_ >= kSlowLogIntervalNs) {
        utils::Logger::warning(utils::LogChannel::Audio, "Emulation is slower than real time: %.2f ms per frame (budget %.2f ms). Audio will "
                               "underrun; a Release build should fix it.", frame_ms_avg_, kFrameBudgetMs);
        last_slow_log_ns_ = now;
    }
}

// Audio as the timing master: wait until the queue has room for another frame. Returns false
// (use the timer) when there's no running audio output.
auto EmuThread::wait_for_audio(uint64_t deadline_ns) -> bool {
    if (!core_.audio.is_open() || core_.state != RunState::Running) return false;
    const double target = core_.audio.stats().target_ms;
    while (core_.audio.queued_ms() > target) {
        if (SDL_GetTicksNS() >= deadline_ns) break;  // never hold a frame back by more than ~2 frames
        if (quit_.load(std::memory_order_acquire)) break;
        SDL_DelayPrecise(kAudioWaitStepNs);
    }
    return true;
}

} // namespace snes::frontend
