// SPDX-License-Identifier: MIT
#pragma once
#include "frontend/audio.hpp"
#include "frontend/emulator.hpp"
#include "frontend/input.hpp"
#include "frontend/rewind.hpp"
#include "frontend/thread_sync.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace snes::frontend {

enum class RunState : uint8_t { Stopped, Running, Paused };

// One emulated picture, as published to the UI thread.
struct FrameImage {
    std::vector<uint32_t> pixels;  // XRGB8888, width * height
    int width = 0;
    int height = 0;
    uint64_t frame = 0;
    bool test_pattern = false;
};

// What the UI shows of the emulation, copied out by the emulation thread after every frame and
// every command batch. The UI never reads the core or the audio output directly.
struct EmuStatus {
    bool loaded = false;
    RunState state = RunState::Stopped;
    std::string rom_name;
    std::string rom_path;
    bool test_pattern = false;
    uint64_t frame_count = 0;   // the game's (a state load or reset moves it)
    uint64_t frames_run = 0;    // frames this thread has emulated, all games
    double fps = 0.0;
    double frame_ms_avg = 0.0;  // emulation time per frame
    double frame_ms_max = 0.0;  // worst in the last second
    bool exit_reached = false;  // set_exit_after_frames() reached: emulation holds there
    AudioOutput::Stats audio;
    bool core_recording = false;
    std::string core_recording_path;
    double core_recording_seconds = 0.0;
    int core_recording_rate = 0;
    bool device_recording = false;
    std::string device_recording_path;
    double device_recording_seconds = 0.0;
    bool msu1 = false;              // the game has an MSU-1 (<rom>.msu)
    // Rewind and fast-forward.
    bool rewinding = false;
    bool fast_forward = false;
    int fast_forward_speed = 0;     // the setting: 0 = uncapped
    double fast_forward_rate = 0.0; // emulated frames per displayed frame, last second
    size_t rewind_entries = 0;
    size_t rewind_bytes = 0;
    double rewind_seconds = 0.0;    // how far back the history reaches
};


// The emulation thread. It owns the core (Emulator), the audio output's producer side and the
// frame schedule: every 1/60.0988 s it reads the joypads, runs one frame, queues its audio and
// publishes its picture. Nothing the UI thread does (window messages, modal move/size loops,
// swapchain recreation, ImGui, file dialogs) can starve the audio queue any more.
//
// The UI thread talks to it only through:
//  - commands (post: queued, run between frames; call: run between frames and wait for the
//    result), which are the only code that touches the Core while the thread runs,
//  - set_input() (atomics), take_frame()/frame() (triple buffer), status() (a copy under a mutex).
// Before start() and after stop() there is no thread: core() is then the UI's to use directly,
// and post()/call() run the command on the calling thread.
class EmuThread {
public:
    static constexpr uint64_t kFrameNs = static_cast<uint64_t>(1'000'000'000.0 / Emulator::kNtscFrameRate);
    static constexpr uint64_t kMaxLagFrames = 4;  // past this, resync instead of fast-forwarding
    static constexpr uint64_t kUnderrunLogSettleNs = 250'000'000ULL;  // after fast-forward or rewind ends

    // Everything the emulation thread owns.
    struct Core {
        Emulator emu;
        AudioOutput audio;
        RunState state = RunState::Stopped;
        bool advance_one = false;   // run one frame while paused
        bool audio_sync = false;    // pace frames by the audio queue instead of the timer
        bool republish = false;     // a command changed the picture (boot, state load): publish it
        SpeedConfig speed;
        bool fast_forward_toggled = false;
        RewindBuffer rewind;
        std::string rewind_rom;      // the ROM the history belongs to
        int frames_since_snapshot = 0;
        // Applies `speed` to the rewind buffer (call after changing it).
        auto apply_speed() -> void {
            rewind.configure(static_cast<size_t>(speed.rewind_budget_mb) << 20, speed.rewind_interval);
            if (!speed.rewind) rewind.clear();
        }
    };
    using Command = std::function<void(Core&)>;

    EmuThread() = default;
    EmuThread(const EmuThread&) = delete;
    auto operator=(const EmuThread&) -> EmuThread& = delete;
    ~EmuThread() { stop(); }

    // Only while the thread isn't running (setup before start(), teardown after stop()).
    auto core() -> Core& { return core_; }
    // 0: run until stopped. Otherwise emulation holds once this many frames have run (smoke tests).
    auto set_exit_after_frames(uint64_t frames) -> void { exit_after_frames_ = frames; }

    auto start() -> void;
    // Finishes queued commands, then joins. Safe to call twice.
    auto stop() -> void;
    auto running() const -> bool { return thread_.joinable(); }

    auto post(Command command) -> void;
    // Runs fn(Core&) on the emulation thread between frames and returns its result.
    template <typename F>
    auto call(F&& fn) -> std::invoke_result_t<F&, Core&>;

    // UI thread: pad `pad`'s state (SNES bit order) and whether it has a controller. Pads go to
    // console ports as pad_slots() says for the devices set here.
    auto set_input(int pad, uint16_t buttons, bool connected) -> void;
    auto set_port_devices(const PortDevices& devices) -> void;
    // UI thread: host mouse motion (SNES counts, accumulated) and buttons, for a SNES Mouse.
    auto add_mouse_motion(int dx, int dy, bool left, bool right) -> void;
    // UI thread: the Super Scope's aim and buttons.
    auto set_scope(const snes::ScopeInput& input) -> void;
    // UI thread: the rewind and fast-forward hotkeys as held right now.
    auto set_speed_input(bool rewind, bool fast_forward) -> void;

    // UI thread: wait up to timeout_ns for a frame newer than the last take_frame(), then take
    // the newest one, if any (true). frame() is the last one taken.
    auto wait_frame(uint64_t timeout_ns) -> void;
    auto take_frame() -> bool { return frames_.acquire(); }
    auto frame() const -> const FrameImage& { return frames_.read_buffer(); }

    auto status() const -> EmuStatus;

private:
    auto run() -> void;
    auto on_thread() const -> bool { return thread_id_.load(std::memory_order_acquire) == std::this_thread::get_id(); }
    auto step() -> bool;              // one loop pass: pause state, maybe a frame, audio service
    auto read_input() -> void;
    auto run_frames(bool fast) -> int;   // one frame, or a fast-forward batch; returns frames run
    auto rewind_frame() -> void;         // one step back through the history
    auto snapshot_if_due() -> void;
    auto publish_frame() -> void;
    auto publish_status() -> void;
    auto watch_audio(uint64_t now) -> void;
    auto wait_for_audio(uint64_t deadline_ns) -> bool;

    Core core_;
    CommandQueue<Core> commands_;
    std::jthread thread_;
    std::atomic<std::thread::id> thread_id_{};
    std::atomic<bool> quit_{false};
    uint64_t exit_after_frames_ = 0;

    std::array<std::atomic<uint32_t>, kPadCount> input_{};  // buttons | connected << 16
    std::array<std::atomic<snes::Device>, kPortCount> devices_{snes::Device::Gamepad, snes::Device::Gamepad};
    std::atomic<int32_t> mouse_dx_{0};
    std::atomic<int32_t> mouse_dy_{0};
    std::atomic<uint32_t> mouse_buttons_{0};  // bit 0 left, bit 1 right
    std::atomic<uint32_t> scope_{0};          // packed ScopeInput (see pack_scope)
    std::atomic<uint32_t> speed_input_{0};               // bit 0 rewind, bit 1 fast-forward
    std::vector<int16_t> fast_audio_;                    // a fast-forward batch's samples
    std::vector<int16_t> fast_msu_;
    std::vector<int16_t> step_audio_;                    // what one display frame queues: APU
    std::vector<int16_t> step_msu_;                      // and MSU-1 (empty without one)
    bool rewinding_ = false;
    bool fast_forward_ = false;
    uint64_t fast_frames_window_ = 0;   // emulated frames in this second (fps_frames_ counts displayed ones)
    double fast_rate_ = 0.0;

    TripleBuffer<FrameImage> frames_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    uint64_t frames_published_ = 0;  // under frame_mutex_
    uint64_t frames_seen_ = 0;       // UI thread

    mutable std::mutex status_mutex_;
    EmuStatus status_;

    // Emulation-thread statistics.
    bool exit_reached_ = false;
    double frame_ms_avg_ = 0.0;
    double frame_ms_max_ = 0.0;
    double frame_ms_max_window_ = 0.0;
    double fps_ = 0.0;
    uint64_t fps_frames_ = 0;
    uint64_t frames_run_ = 0;
    uint64_t fps_window_start_ns_ = 0;
    uint64_t last_push_ns_ = 0;      // when the last frame was queued (0: not running)
    double frame_gap_ms_max_ = 0.0;  // longest time between queued frames since the last underrun warning
    uint64_t logged_underruns_ = 0;
    uint64_t last_underrun_log_ns_ = 0;
    uint64_t last_slow_log_ns_ = 0;
    uint64_t speed_settle_until_ns_ = 0;  // underrun warnings stay quiet until this time
};

template <typename F>
auto EmuThread::call(F&& fn) -> std::invoke_result_t<F&, Core&> {
    using R = std::invoke_result_t<F&, Core&>;
    if (on_thread()) return fn(core_);
    std::promise<R> done;
    auto result = done.get_future();
    const Command task = [&fn, &done, this](Core& core) {
        if constexpr (std::is_void_v<R>) {
            fn(core);
            publish_status();  // so the UI sees the outcome as soon as call() returns
            done.set_value();
        } else {
            R value = fn(core);
            publish_status();
            done.set_value(std::move(value));
        }
    };
    if (!commands_.post(task)) commands_.run_inline(task, core_);
    return result.get();
}

} // namespace snes::frontend
