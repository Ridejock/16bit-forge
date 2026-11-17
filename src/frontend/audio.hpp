// SPDX-License-Identifier: MIT
#pragma once
#include "frontend/wav_writer.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace bitforge::frontend {

// Steers the playback queue toward a target depth by nudging the SDL stream's frequency ratio
// (dynamic rate control). A PI controller on the smoothed queue depth: the proportional term
// reacts to a queue that is off target, the integral term learns the steady mismatch between
// the emulator's pacing and the sound card's clock (and any shortfall when frames run late), so
// the queue settles on the target instead of drifting. +/-1.5% is still inaudible as pitch.
class RateControl {
public:
    static constexpr double kTargetMs = 64.0;        // default queue depth to steer toward
    static constexpr double kMaxRateAdjust = 0.015;  // +/-1.5%
    static constexpr double kLevelSmoothing = 0.1;   // per push (~10 pushes, ~170 ms)
    static constexpr double kProportional = 0.01;    // ratio offset at 100% queue error
    static constexpr double kIntegral = 0.00005;     // per push at 100% error (1% learned in ~3 s at full error)

    auto restart() -> void;
    // Raised when the device pulls big chunks, so the low point of the queue stays above zero.
    auto set_target(double ms) -> void { target_ms_ = ms; }
    auto target_ms() const -> double { return target_ms_; }
    // Takes the queue depth just before a push; returns the stream frequency ratio to use.
    auto update(double queued_ms) -> double;
    auto level_ms() const -> double { return level_ms_; }
    auto ratio() const -> double { return ratio_; }
    auto learned() const -> double { return integral_; }  // the steady part of the adjustment

private:
    double target_ms_ = kTargetMs;
    double level_ms_ = kTargetMs;
    double integral_ = 0.0;
    double ratio_ = 1.0;
};

// The mixer stage: `apu` at `volume` percent plus `msu1` (same length, or empty) at
// `msu1_volume` percent of that, saturated to int16.
auto mix_sources(std::span<const int16_t> apu, std::span<const int16_t> msu1, int volume, int msu1_volume,
                 std::vector<int16_t>& out) -> void;

// Sound output on an SDL3 audio stream (no SDL_mixer). The core's 32040 Hz stereo int16 goes
// through a small mixer stage (volume, mute, saturation) into our own FIFO. SDL pulls from the
// FIFO through the stream's get-callback whenever the device needs data and resamples to the
// device rate, so this class sees every request: an empty FIFO is a counted underrun that
// fades out over a couple of milliseconds (no hard click), waits until the FIFO has refilled,
// then fades back in. The emulation thread keeps pushing one emulated frame of audio per video frame.
//
// Diagnostics: two optional WAV recorders. "Core" is the raw 32040 Hz DSP output before the
// mixer. "Device" is the final mix SDL hands the sound card (after resampling and rate control,
// underrun silence included), at the device rate. If the core file is clean and the device file
// crackles, the problem is sync, not emulation.
class AudioOutput {
public:
    static constexpr double kTargetMs = RateControl::kTargetMs;
    static constexpr double kMaxQueueMs = 200.0;     // beyond this the oldest audio is dropped (crossfaded)
    static constexpr double kFrameMs = 1000.0 / 60.0988;  // one push
    static constexpr int kFadeFrames = 64;           // 2 ms fades at underrun, resume and drops
    // Startup is measured in emulated frames (pushes), not wall-clock time or device pulls, so a
    // stalled emulation thread (window/GPU creation, the first presents, a ROM load) can't use it up.
    static constexpr int kWarmupFrames = 8;          // frames pushed after a (re)start before the device may start
    // Frames pushed after the device starts in which underruns aren't counted (~2 s of play: a
    // fresh SDL_GPU window on Windows can stall the loop for most of a second after it appears).
    static constexpr int kGraceFrames = 120;

    struct Stats {
        bool open = false;
        bool paused = true;
        bool rate_control = true;
        const char* driver = "";
        int source_rate = 0;
        int device_rate = 0;
        int device_channels = 0;
        int device_buffer_frames = 0;  // SDL's device buffer (one hardware period)
        int last_pull_frames = 0;      // source frames SDL asked for in the last callback
        double target_ms = 0.0;
        double queued_ms = 0.0;        // FIFO + SDL stream, right now
        double level_ms = 0.0;         // smoothed depth just before each push (what the control steers)
        double ratio = 1.0;
        double learned = 0.0;
        uint64_t underruns = 0;
        double silence_ms = 0.0;       // inserted by underruns and refills
        uint64_t overflows = 0;
        double dropped_ms = 0.0;
        bool priming = false;          // resumed, device held until warm-up frames are in and the queue reaches the target
        bool grace = false;            // in the post-start window where underruns aren't counted
        bool device_running = false;   // the device has been started and is pulling
        // At the last counted underrun: how much audio the device had pulled since the emulation thread
        // last queued a frame. About one frame (17 ms) plus one pull when frames keep coming;
        // much more means the emulation thread stalled and starved it.
        double starved_ms = 0.0;
    };

    AudioOutput() = default;
    AudioOutput(const AudioOutput&) = delete;
    auto operator=(const AudioOutput&) -> AudioOutput& = delete;
    ~AudioOutput() { close(); }

    // Opens the default playback device.
    auto open(int source_rate) -> bool;
    // Test hook: a stream that isn't bound to a device; the caller pulls with SDL_GetAudioStreamData.
    auto open_unbound(int source_rate, int device_rate) -> bool;
    auto close() -> void;
    auto is_open() const -> bool { return stream_ != nullptr; }
    auto stream() const -> SDL_AudioStream* { return stream_; }

    // Paused output stops the device, plays nothing, counts nothing and drops pushes. Resuming
    // keeps the device stopped until kWarmupFrames have been pushed and the queue holds the target
    // (so playback starts with a full buffer once frames are really flowing), then starts it with
    // a grace period of kGraceFrames pushed frames in which underruns aren't counted.
    auto set_paused(bool paused) -> void;
    // A new game session (boot, after the game window exists): zero the underrun/overflow
    // counters; if the output is running, hold the device and warm up again.
    auto begin_session() -> void;
    // Call before anything that tears down or blocks emulation (stop, loading another ROM), and
    // as soon as a stop/close is requested: the device is paused first, so the teardown can't
    // drain the queue into an underrun, and anything counted since the last pushed frame (a
    // stall that ended in the stop request, e.g. holding a window's close button) is discarded.
    auto end_session() -> void { set_paused(true); }
    // Before a jump in emulation that may hitch (reset, renderer switch): start a grace period;
    // counters are kept. A stall right after this isn't counted, however long it is.
    auto start_grace() -> void;
    // Interleaved stereo frames from the emulator.
    // One emulated frame: the APU's samples, and the MSU-1's for the same frame (same count,
    // or empty) mixed in.
    auto push(std::span<const int16_t> stereo, std::span<const int16_t> msu1 = {}) -> void;
    // For a jump in the emulated audio (save state loaded): queued audio fades to silence over
    // kFadeFrames and the rest of the queue becomes silence (so its depth, and rate control,
    // stay as they were), and the next push fades in. No click, no underrun. Starts a grace period.
    auto flush() -> void;
    // Off: the frequency ratio stays at 1.0 (used when the emulation thread is paced by the audio queue).
    auto set_rate_control(bool on) -> void;
    auto queued_ms() const -> double;

    auto set_volume(int percent) -> void { volume_ = percent; }
    auto set_msu1_volume(int percent) -> void { msu1_volume_ = percent; }  // relative to the master volume
    auto set_muted(bool muted) -> void { muted_ = muted; }
    auto stats() const -> Stats;

    // Recorders. Call service() once per emulation-thread iteration: it writes what the audio thread
    // captured since the last call.
    auto start_core_recording(const std::string& path) -> bool;
    auto stop_core_recording() -> void;
    auto core_recording() const -> const WavWriter& { return core_rec_; }
    auto start_device_recording(const std::string& path) -> bool;
    auto stop_device_recording() -> void;
    auto device_recording() const -> bool { return device_rec_on_; }
    auto device_recording_seconds() const -> double { return device_rec_.seconds(); }
    auto device_recording_path() const -> const std::string& { return device_rec_path_; }
    auto service() -> void;

private:
    enum class Feed { Refill, Playing };

    auto open_stream(SDL_AudioStream* stream, int source_rate, int device_rate) -> void;
    auto fifo_frames() const -> size_t;  // lock held
    auto fifo_write(const int16_t* stereo, size_t frames) -> void;  // lock held
    auto fifo_read(int16_t* stereo, size_t frames) -> void;         // lock held
    auto fifo_drop_oldest(size_t frames) -> void;                   // lock held
    auto feed(SDL_AudioStream* stream, int additional_bytes) -> void;
    auto target_ms_for(int pull_frames) const -> double;
    auto resume_frames(double target_ms) const -> size_t;
    auto hold() -> void;  // stop the device, keep the output resumed, and prime again
    static void SDLCALL on_stream_get(void* userdata, SDL_AudioStream* stream, int additional, int total);
    static void SDLCALL on_postmix(void* userdata, const SDL_AudioSpec* spec, float* buffer, int buflen);

    SDL_AudioStream* stream_ = nullptr;
    bool owns_subsystem_ = false;
    SDL_AudioDeviceID device_ = 0;
    int source_rate_ = 0;
    int device_rate_ = 0;
    int device_channels_ = 0;
    int device_buffer_frames_ = 0;
    int volume_ = 100;
    int msu1_volume_ = 100;
    bool muted_ = false;
    bool paused_ = true;
    bool rate_control_ = true;
    RateControl rate_;
    std::vector<int16_t> mix_;
    bool fade_in_next_ = false;  // emulation thread only

    // Shared with the audio thread (under fifo_mutex_).
    mutable std::mutex fifo_mutex_;
    std::vector<int16_t> fifo_;  // ring of stereo frames
    size_t fifo_head_ = 0;       // frame index of the oldest sample
    size_t fifo_count_ = 0;      // frames stored
    Feed feed_state_ = Feed::Refill;
    std::vector<int16_t> feed_buf_;
    uint64_t underruns_ = 0;
    uint64_t silence_frames_ = 0;
    // Counted by the audio thread but not yet confirmed: they become underruns_/silence_frames_
    // when the emulation thread pushes its next frame (emulation carried on through the stall). A pause
    // or end_session() first discards them: that stall belonged to the stop, not to play.
    uint64_t pending_underruns_ = 0;
    uint64_t pending_silence_frames_ = 0;
    size_t pulled_since_push_ = 0;      // device time since the last push, in source frames
    size_t pending_starved_frames_ = 0; // pulled_since_push_ when the pending counts were made
    size_t starved_frames_ = 0;         // ... for the last committed underrun
    uint64_t overflows_ = 0;
    uint64_t dropped_frames_ = 0;
    int last_pull_frames_ = 0;
    int max_pull_frames_ = 0;    // largest request seen: sets the queue target
    bool priming_ = false;       // resumed; the device starts after warm-up, once the FIFO reaches the target
    bool running_ = false;       // device started and output not paused; nothing is counted otherwise
    int grace_frames_ = 0;       // pushed frames left in which underruns aren't counted
    int warmup_frames_ = 0;      // pushed frames left before the device may start

    // Recorders
    WavWriter core_rec_;
    WavWriter device_rec_;
    bool device_rec_on_ = false;
    std::string device_rec_path_;
    std::mutex capture_mutex_;
    std::vector<int16_t> capture_;  // device output waiting for service()
    int capture_rate_ = 0;
};

} // namespace bitforge::frontend
