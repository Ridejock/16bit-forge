// SPDX-License-Identifier: MIT
#pragma once
#include "types.hpp"

namespace bitforge::utils {

// Extract bits from value
template<typename T>
constexpr T get_bits(T value, u8 offset, u8 count) {
  return (value >> offset) & ((T(1) << count) - 1);
}

// Check if specific bit is set
template<typename T>
constexpr bool bit_test(T value, u8 bit) {
  return (value >> bit) & 1;
}

// Set/clear specific bit
template<typename T>
constexpr T bit_set(T value, u8 bit) {
  return value | (T(1) << bit);
}

template<typename T>
constexpr T bit_clear(T value, u8 bit) {
  return value & ~(T(1) << bit);
}

// Sign extend
constexpr uint8_t sign_extend_8(s8 value) {
  return static_cast<uint8_t>(value);
}

constexpr uint16_t sign_extend_16(s16 value) {
  return static_cast<uint16_t>(value);
}

// Convert between little/big endian (SNES is little-endian like x86)
constexpr uint16_t to_le16(const u8* ptr) {
  return static_cast<uint16_t>(ptr[0]) | (static_cast<uint16_t>(ptr[1]) << 8);
}

} // namespace utils