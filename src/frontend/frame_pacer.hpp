// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace bitforge::frontend {

// Frame timing (the emulation thread's schedule, and the UI loop's): one frame per frame_ns,
// never fast-forwarding more than max_lag_frames behind (past that, resync). Times are passed in
// (SDL_GetTicksNS() in the app), so the logic is testable without a clock.
class FramePacer {
public:
    FramePacer(uint64_t frame_ns, uint64_t max_lag_frames) : frame_ns_(frame_ns), max_lag_ns_(max_lag_frames * frame_ns) {}

    auto reset(uint64_t now) -> void { next_ns_ = now; }
    // A frame is due at or after next_ns().
    auto due(uint64_t now) const -> bool { return now >= next_ns_; }
    auto next_ns() const -> uint64_t { return next_ns_; }
    // After a frame: schedule the next one and return how long to sleep until it (0 when late).
    // A loop that has fallen more than max_lag_frames behind resyncs instead of catching up.
    auto advance(uint64_t now) -> uint64_t {
        next_ns_ += frame_ns_;
        if (now < next_ns_) return next_ns_ - now;
        if (now - next_ns_ > max_lag_ns_) next_ns_ = now;
        return 0;
    }
    // Paced by something else this frame (the audio queue): the next frame is one period from now.
    auto sync(uint64_t now) -> void { next_ns_ = now; }

private:
    uint64_t frame_ns_;
    uint64_t max_lag_ns_;
    uint64_t next_ns_ = 0;
};

} // namespace bitforge::frontend
