// SPDX-License-Identifier: MIT
#pragma once
// Rewind history: save states captured every few frames, kept as a chain of compressed deltas.
//
// The newest state is kept whole; every older one is stored as the XOR against the state after
// it, run-length coded (consecutive states differ in few bytes, so the deltas are small). Going
// back one step decodes the newest delta onto the current state. Each entry also keeps a
// thumbnail, so the timeline can show the moment and a save slot can be written from it. The
// oldest entries are dropped to stay inside the memory budget (state bytes and thumbnails
// together). Pure data structure, no SDL, no threads.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace bitforge::frontend {

// a XOR b (missing bytes of the shorter one count as zero), coded as varint pairs (run of zero
// bytes, count of literal bytes) followed by the literals.
auto encode_delta(std::span<const uint8_t> a, std::span<const uint8_t> b) -> std::vector<uint8_t>;
// state ^= decode(delta) over `size` bytes (state is zero-extended first), then resized to `size`.
auto apply_delta(std::vector<uint8_t>& state, std::span<const uint8_t> delta, size_t size) -> bool;

// Squeezes `factor` frames' worth of interleaved stereo into one frame's worth by averaging each
// group of `factor` sample frames (fast-forward: the pitch rises, the queue stays level).
auto squeeze_audio(std::span<const int16_t> stereo, int factor, std::vector<int16_t>& out) -> void;

// One rewind snapshot's picture. `id` is assigned by RewindBuffer::push and is never reused.
// An empty picture (w == 0) is allowed; the state is still kept.
struct RewindThumb {
    uint64_t id = 0;
    uint64_t frame = 0;  // the game's frame counter when the snapshot was taken
    int w = 0;
    int h = 0;
    std::vector<uint32_t> pixels;  // XRGB8888, w * h
};

class RewindBuffer {
public:
    static constexpr size_t kDefaultBudgetMb = 64;
    static constexpr int kDefaultInterval = 4;  // frames between captures

    auto configure(size_t budget_bytes, int interval_frames) -> void;
    auto clear() -> void;

    // Adds the newest state (the caller captures one every interval() frames).
    // `thumb` is that moment's picture; its id is assigned here.
    auto push(std::vector<uint8_t> state, RewindThumb thumb = {}) -> void;
    // The newest state, then the history steps back one entry. Holding at the oldest entry
    // returns it again. Empty when nothing was captured.
    auto pop() -> std::optional<std::vector<uint8_t>>;
    // The state at `id`, without changing the history. Empty if that moment is gone.
    auto state_at(uint64_t id) const -> std::optional<std::vector<uint8_t>>;
    // Drops every moment newer than `id` and returns the state now at the end (still stored).
    // Empty if that moment is gone.
    auto seek(uint64_t id) -> std::optional<std::vector<uint8_t>>;
    auto thumb_at(uint64_t id) const -> const RewindThumb*;
    // Copies of the pictures whose ids are in [first, last], oldest first.
    auto thumbs(uint64_t first, uint64_t last) const -> std::vector<RewindThumb>;

    auto interval() const -> int { return interval_; }
    auto budget() const -> size_t { return budget_; }
    auto entries() const -> size_t { return newest_.empty() ? 0 : deltas_.size() + 1; }
    auto bytes() const -> size_t { return delta_bytes_ + newest_.size() + thumb_bytes_; }
    auto oldest_id() const -> uint64_t { return thumbs_.empty() ? 0 : thumbs_.front().id; }
    auto newest_id() const -> uint64_t { return thumbs_.empty() ? 0 : thumbs_.back().id; }

private:
    struct Delta {
        std::vector<uint8_t> data;
        size_t size = 0;  // size of the older state
    };
    auto trim() -> void;
    auto index_of(uint64_t id) const -> std::optional<size_t>;
    auto drop_oldest_thumb() -> void;
    auto drop_newest_thumb() -> void;
    // `steps` deltas back from the newest state, on a copy. Empty if a delta won't apply.
    auto rewind_copy(size_t steps) const -> std::optional<std::vector<uint8_t>>;

    std::vector<uint8_t> newest_;
    std::deque<Delta> deltas_;  // front = oldest
    std::deque<RewindThumb> thumbs_;  // one per entry, front = oldest; parallel to the states
    size_t delta_bytes_ = 0;
    size_t thumb_bytes_ = 0;
    uint64_t next_id_ = 1;  // not reset by clear(), so a UI texture never matches a new moment
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

} // namespace bitforge::frontend
