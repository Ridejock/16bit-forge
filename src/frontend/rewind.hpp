// SPDX-License-Identifier: MIT
#pragma once
// Rewind history: save states captured every few frames, kept as a chain of compressed deltas.
//
// The newest state is kept whole; every older one is stored as the XOR against the state after
// it, run-length coded (consecutive states differ in few bytes, so the deltas are small). Going
// back one step decodes the newest delta onto the current state. The oldest entries are dropped
// to stay inside the memory budget. Pure data structure, no SDL, no threads.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace snes::frontend {

// a XOR b (missing bytes of the shorter one count as zero), coded as varint pairs (run of zero
// bytes, count of literal bytes) followed by the literals.
auto encode_delta(std::span<const uint8_t> a, std::span<const uint8_t> b) -> std::vector<uint8_t>;
// state ^= decode(delta) over `size` bytes (state is zero-extended first), then resized to `size`.
auto apply_delta(std::vector<uint8_t>& state, std::span<const uint8_t> delta, size_t size) -> bool;

// Squeezes `factor` frames' worth of interleaved stereo into one frame's worth by averaging each
// group of `factor` sample frames (fast-forward: the pitch rises, the queue stays level).
auto squeeze_audio(std::span<const int16_t> stereo, int factor, std::vector<int16_t>& out) -> void;

class RewindBuffer {
public:
    static constexpr size_t kDefaultBudgetMb = 64;
    static constexpr int kDefaultInterval = 4;  // frames between captures

    auto configure(size_t budget_bytes, int interval_frames) -> void;
    auto clear() -> void;

    // Adds the newest state (the caller captures one every interval() frames).
    auto push(std::vector<uint8_t> state) -> void;
    // The newest state, then the history steps back one entry. Holding at the oldest entry
    // returns it again. Empty when nothing was captured.
    auto pop() -> std::optional<std::vector<uint8_t>>;

    auto interval() const -> int { return interval_; }
    auto budget() const -> size_t { return budget_; }
    auto entries() const -> size_t { return newest_.empty() ? 0 : deltas_.size() + 1; }
    auto bytes() const -> size_t { return delta_bytes_ + newest_.size(); }

private:
    struct Delta {
        std::vector<uint8_t> data;
        size_t size = 0;  // size of the older state
    };
    auto trim() -> void;

    std::vector<uint8_t> newest_;
    std::deque<Delta> deltas_;  // front = oldest
    size_t delta_bytes_ = 0;
    size_t budget_ = kDefaultBudgetMb << 20;
    int interval_ = kDefaultInterval;
};

// Rewind and fast-forward settings (the frontend's [speed] section).
struct SpeedConfig {
    static constexpr int kMinBudgetMb = 8, kMaxBudgetMb = 2048;
    static constexpr int kMaxInterval = 60;
    static constexpr int kMaxSpeed = 16;
    bool rewind = true;
    int rewind_budget_mb = static_cast<int>(RewindBuffer::kDefaultBudgetMb);
    int rewind_interval = RewindBuffer::kDefaultInterval;  // frames between snapshots
    bool rewind_reverse_audio = false;                      // false: silent while rewinding
    int fast_forward_speed = 4;                             // x real time; 0 = as fast as possible
};

} // namespace snes::frontend
