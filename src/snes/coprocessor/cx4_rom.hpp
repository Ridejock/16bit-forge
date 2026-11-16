// SPDX-License-Identifier: MIT
#pragma once
// Cx4 data ROM: 1024 little-endian 24-bit math table entries (3072 bytes), generated
// at startup so no firmware file is required. The formulas match the cartridge dump
// (SHA-256 ae8d4d1961b93421ff00b3caa1d0f0ce7783e749772a3369c36b3dbf0d37ef18).
//
//   inverse[0] = 0xFFFFFF; inverse[i] = floor(0x800000 / i)     i = 1..255
//   sqrt[i]    = floor(0x1000000 * sqrt(i / 256))               i = 0..255
//   sin[i]     = floor(0x1000000 * sin(i * pi / 256))           i = 0..127
//   asin[i]    = floor(0x1000000 / pi * asin(i / 128))          i = 0..127
//   tan[i]     = floor(0x10000 * tan(i * pi / 256))             i = 0..127
//                tan[64] is 0x10000 (tan(pi/4) = 1; libm's tan is a hair under 1)
//   cos[0]     = 0xFFFFFF (the 0x1000000 value truncated to 24 bits)
//   cos[i]     = floor(0x1000000 * cos(i * pi / 256))           i = 1..127
//
// Inverse and sqrt are integer-exact (integer division and integer square root of
// i << 40). The trig tables use double. The only entry within 1e-6 of an integer
// boundary is tan(pi/4); every other value is at least 6.8e-4 away, so a small
// snap-to-nearest plus the documented tan[64] adjust keeps MSVC and GCC identical.
// A unit test checks the generated image's SHA-256 against the known dump.
#include "snes/types.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace snes::coprocessor {

inline constexpr char kCx4DataRomSha256[] =
    "ae8d4d1961b93421ff00b3caa1d0f0ce7783e749772a3369c36b3dbf0d37ef18";
inline constexpr size_t kCx4DataRomSize = 3072;

enum class Cx4TableFn { Inverse, Sqrt, Sin, Asin, Tan, Cos };

struct Cx4TableSpec {
  int count;
  Cx4TableFn fn;
};

inline constexpr Cx4TableSpec kCx4Tables[] = {
    {256, Cx4TableFn::Inverse},
    {256, Cx4TableFn::Sqrt},
    {128, Cx4TableFn::Sin},
    {128, Cx4TableFn::Asin},
    {128, Cx4TableFn::Tan},
    {128, Cx4TableFn::Cos},
};

// floor(2^24 * sqrt(i/256)) = floor(sqrt(i * 2^40)). i << 40 fits in uint64 and is
// exact in double (at most 2^48), then the walk corrects any one-ULP sqrt rounding.
inline auto isqrt_u64(uint64_t n) -> uint64_t {
  uint64_t r = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));
  while (r < (1ull << 32) && (r + 1) * (r + 1) <= n) ++r;
  while (r > 0 && r * r > n) --r;
  return r;
}

// Floor, but snap to the nearest integer when the double is within 1e-6 of one.
// The only table entry that close is tan(pi/4)*65536, whose exact value is 65536.
inline auto floor_or_snap(double x) -> uint32_t {
  const double nearest = std::nearbyint(x);
  if (std::fabs(x - nearest) < 1e-6) return static_cast<uint32_t>(nearest);
  return static_cast<uint32_t>(std::floor(x));
}

inline auto cx4_table_entry(Cx4TableFn fn, int i) -> uint32_t {
  const double pi = std::numbers::pi;
  switch (fn) {
    case Cx4TableFn::Inverse:
      return i == 0 ? 0xFFFFFFu : (0x800000u / static_cast<uint32_t>(i));
    case Cx4TableFn::Sqrt:
      return static_cast<uint32_t>(isqrt_u64(static_cast<uint64_t>(i) << 40));
    case Cx4TableFn::Sin:
      return floor_or_snap(0x1000000 * std::sin(static_cast<double>(i) * pi / 256.0));
    case Cx4TableFn::Asin:
      return floor_or_snap(0x1000000 / pi * std::asin(static_cast<double>(i) / 128.0));
    case Cx4TableFn::Tan: {
      uint32_t t = floor_or_snap(0x10000 * std::tan(static_cast<double>(i) * pi / 256.0));
      if (i == 64 && t == 65535) t += 1;  // dump / Python generator: tan(pi/4) is 1
      return t;
    }
    case Cx4TableFn::Cos:
      return i == 0 ? 0xFFFFFFu
                    : floor_or_snap(0x1000000 * std::cos(static_cast<double>(i) * pi / 256.0));
  }
  return 0;
}

inline auto generate_cx4_data_rom() -> std::array<Byte, kCx4DataRomSize> {
  std::array<Byte, kCx4DataRomSize> out{};
  size_t w = 0;
  for (const Cx4TableSpec& table : kCx4Tables) {
    for (int i = 0; i < table.count; i++) {
      const uint32_t word = cx4_table_entry(table.fn, i) & 0xFFFFFFu;
      out[w++] = static_cast<Byte>(word);
      out[w++] = static_cast<Byte>(word >> 8);
      out[w++] = static_cast<Byte>(word >> 16);
    }
  }
  return out;
}

}  // namespace snes::coprocessor
