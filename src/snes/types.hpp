// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace snes {

// Unsigned types
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Signed types
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// Common aliases
using Byte = u8;
using Word = u16;
using Address = u32;  // SNES uses 24-bit addresses

} // namespace snes