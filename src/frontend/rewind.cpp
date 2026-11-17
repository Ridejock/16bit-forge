// SPDX-License-Identifier: MIT
#include "frontend/rewind.hpp"

#include <algorithm>

namespace bitforge::frontend {

namespace {

auto put_varint(std::vector<uint8_t>& out, size_t value) -> void {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

auto get_varint(std::span<const uint8_t> in, size_t& pos, size_t& value) -> bool {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= in.size()) return false;
        const uint8_t b = in[pos++];
        value |= static_cast<size_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

} // namespace

auto encode_delta(std::span<const uint8_t> a, std::span<const uint8_t> b) -> std::vector<uint8_t> {
    const size_t n = std::max(a.size(), b.size());
    auto at = [&](size_t i) -> uint8_t {
        return static_cast<uint8_t>((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    };
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < n) {
        size_t zeros = 0;
        while (i + zeros < n && at(i + zeros) == 0) zeros++;
        i += zeros;
        // Literals run until the next stretch of 4+ zero bytes (shorter zero runs aren't worth a pair).
        size_t end = i;
        while (end < n) {
            if (at(end) == 0) {
                size_t z = 0;
                while (end + z < n && z < 4 && at(end + z) == 0) z++;
                if (z >= 4 || end + z == n) break;
                end += z;
            } else {
                end++;
            }
        }
        put_varint(out, zeros);
        put_varint(out, end - i);
        for (; i < end; i++) out.push_back(at(i));
    }
    return out;
}

auto apply_delta(std::vector<uint8_t>& state, std::span<const uint8_t> delta, size_t size) -> bool {
    if (state.size() < size) state.resize(size, 0);
    size_t pos = 0, i = 0;
    while (pos < delta.size()) {
        size_t zeros = 0, literals = 0;
        if (!get_varint(delta, pos, zeros) || !get_varint(delta, pos, literals)) return false;
        i += zeros;
        if (literals > delta.size() - pos || i + literals > state.size()) return false;
        for (size_t k = 0; k < literals; k++) state[i++] ^= delta[pos++];
    }
    state.resize(size);
    return true;
}

auto squeeze_audio(std::span<const int16_t> stereo, int factor, std::vector<int16_t>& out) -> void {
    out.clear();
    factor = std::max(factor, 1);
    const size_t frames = stereo.size() / 2;
    for (size_t f = 0; f + static_cast<size_t>(factor) <= frames; f += static_cast<size_t>(factor)) {
        int32_t l = 0, r = 0;
        for (int k = 0; k < factor; k++) {
            l += stereo[2 * (f + static_cast<size_t>(k))];
            r += stereo[2 * (f + static_cast<size_t>(k)) + 1];
        }
        out.push_back(static_cast<int16_t>(l / factor));
        out.push_back(static_cast<int16_t>(r / factor));
    }
}

auto RewindBuffer::configure(size_t budget_bytes, int interval_frames) -> void {
    budget_ = std::max<size_t>(budget_bytes, 1);
    interval_ = std::clamp(interval_frames, 1, 600);
    trim();
}

auto RewindBuffer::clear() -> void {
    newest_.clear();
    deltas_.clear();
    delta_bytes_ = 0;
}

auto RewindBuffer::push(std::vector<uint8_t> state) -> void {
    if (!newest_.empty()) {
        Delta d{encode_delta(newest_, state), newest_.size()};
        delta_bytes_ += d.data.size();
        deltas_.push_back(std::move(d));
    }
    newest_ = std::move(state);
    trim();
}

auto RewindBuffer::pop() -> std::optional<std::vector<uint8_t>> {
    if (newest_.empty()) return std::nullopt;
    std::vector<uint8_t> current = newest_;
    if (!deltas_.empty()) {
        Delta& d = deltas_.back();
        if (!apply_delta(newest_, d.data, d.size)) {  // can't happen with our own data; drop the history
            deltas_.clear();
            delta_bytes_ = 0;
            return current;
        }
        delta_bytes_ -= d.data.size();
        deltas_.pop_back();
    }
    return current;
}

auto RewindBuffer::trim() -> void {
    while (!deltas_.empty() && bytes() > budget_) {
        delta_bytes_ -= deltas_.front().data.size();
        deltas_.pop_front();
    }
}

} // namespace bitforge::frontend
