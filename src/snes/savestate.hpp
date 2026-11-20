// SPDX-License-Identifier: MIT
#pragma once
// Save-state files: a small fixed header, a thumbnail, then System::save_state()'s archive.
//
//   magic "SNESSST\0" | version u32 | ROM CRC-32 u32 | frame u64 | unix time i64 |
//   thumbnail w u16, h u16, w*h XRGB8888 | state size u32 | state bytes
//
// Bump kVersion whenever any component's serialize() changes. Files from kMinVersion on still
// load: the component reads its new fields only `if (ar.at_least(N))` (see Archive). Older files
// are refused with a clear message instead of being misread.
#include "snes/types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bitforge {

class System;

namespace savestate {

// The magic predates the 16bit Forge name and stays as is, so existing .sst files keep loading.
inline constexpr std::array<char, 8> kMagic = {'S', 'N', 'E', 'S', 'S', 'S', 'T', '\0'};
inline constexpr uint32_t kVersion = 6;  // 2: cartridge coprocessor state, 3: Cx4, 4: APU port timing, 5: controller devices,
                                         // 6: interrupt edge timing
inline constexpr uint32_t kMinVersion = 4;  // the oldest file version that still loads

struct Info {
    uint32_t version = 0;
    uint32_t rom_crc = 0;
    uint64_t frame = 0;      // frames run since boot when saved
    int64_t timestamp = 0;   // seconds since 1970 (UTC)
    int thumb_w = 0;
    int thumb_h = 0;
    std::vector<uint32_t> thumbnail;  // XRGB8888, thumb_w x thumb_h
};

enum class Result { Ok, NotAState, WrongVersion, WrongRom, Corrupt };
auto describe(Result result) -> const char*;

// Half-size (quarter for 512-wide / interlaced frames) box-filtered copy of a frame.
auto make_thumbnail(const uint32_t* pixels, int width, int height, Info& info) -> void;

// `version` below kVersion writes that older layout (for testing the older-file paths).
auto encode(System& system, uint64_t frame, int64_t timestamp, const uint32_t* pixels, int width, int height,
            uint32_t version = kVersion) -> std::vector<uint8_t>;
// A state file from an already-serialized machine (`state` is System::save_state()'s bytes) and a
// thumbnail. `thumb` may be null. Used to save a rewind moment without loading it.
auto encode_from_state(uint32_t rom_crc, uint64_t frame, int64_t timestamp, const uint32_t* thumb, int thumb_w,
                       int thumb_h, std::span<const uint8_t> state) -> std::vector<uint8_t>;
// Header and thumbnail only (for the slot menu). Checks magic and version (kMinVersion..kVersion).
auto read_info(std::span<const uint8_t> file, Info& info) -> Result;
// Checks magic, version and ROM, then restores the machine (untouched on any failure).
auto decode(System& system, std::span<const uint8_t> file, Info& info) -> Result;

} // namespace savestate
} // namespace bitforge
