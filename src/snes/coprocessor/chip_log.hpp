// SPDX-License-Identifier: MIT
#pragma once
// Rate limits for chip diagnostics: each event kind logs only its first few occurrences, so a
// log from a game run shows how the game drives the chip without flooding. Not emulation state
// (it isn't saved and never changes behaviour).
#include <array>
#include <cstddef>
#include <cstdint>

namespace snes::coprocessor {

template <std::size_t N>
class LogLimiter {
public:
  // True for the first `limit` calls with this key; one extra call returns false and sets
  // `last` so the caller can say that later ones are suppressed.
  auto allow(std::size_t key, uint16_t limit, bool* last = nullptr) -> bool {
    uint16_t& n = counts_[key % N];
    if (n > limit) return false;
    n++;
    if (last) *last = n == limit;
    return n <= limit;
  }

private:
  std::array<uint16_t, N> counts_{};
};

} // namespace snes::coprocessor
