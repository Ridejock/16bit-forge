// SPDX-License-Identifier: MIT
#pragma once
// Address mirroring for ROM/RAM sizes that are not a power of two.
//
// A cartridge memory of `size` bytes behaves like a stack of power-of-two chips: the largest
// chip answers the lower half of the decoded address space and the remainder (itself mirrored
// the same way) answers the upper half. Address lines above that space are not connected.
#include <bit>
#include <cstdint>

namespace bitforge {

constexpr auto rom_mirror(uint32_t address, uint32_t size) -> uint32_t {
  if (size == 0) return 0;
  const uint32_t half = std::bit_floor(size);
  if (half == size) return address & (size - 1);
  address &= 2 * half - 1;
  if (address < half) return address;
  return half + rom_mirror(address - half, size - half);
}

} // namespace bitforge
