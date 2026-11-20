// SPDX-License-Identifier: MIT
#pragma once
// How a memory watch names a place and a number: WRAM or SRAM, 8/16/24-bit little-endian,
// shown as decimal, signed decimal, or hex. RAM search and the watch file both speak this.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bitforge::debug {

constexpr uint32_t kWramBytes = 128 * 1024;

enum class Region : uint8_t { Wram, Sram };

struct RegionInfo {
  Region id;
  std::string_view key;
  std::string_view label;
  bool cheatable;      // a Pro Action Replay cheat can hold this region
  uint32_t bus_base;   // added to the offset in a cheat code (WRAM $7E0000)
};

inline constexpr std::array kRegions = {
    RegionInfo{Region::Wram, "wram", "WRAM", true, 0x7E0000},
    RegionInfo{Region::Sram, "sram", "SRAM", false, 0},
};

enum class ValueSize : uint8_t { Bits8, Bits16, Bits24 };

struct SizeInfo {
  ValueSize id;
  std::string_view key;    // in a watch file
  std::string_view label;
  int bits;
};

inline constexpr std::array kSizes = {
    SizeInfo{ValueSize::Bits8, "8", "8-bit", 8},
    SizeInfo{ValueSize::Bits16, "16", "16-bit", 16},
    SizeInfo{ValueSize::Bits24, "24", "24-bit", 24},
};

enum class Format : uint8_t { Unsigned, Signed, Hex };

struct FormatInfo {
  Format id;
  std::string_view key;  // dec / signed / hex
  std::string_view label;
  bool is_signed;
};

inline constexpr std::array kFormats = {
    FormatInfo{Format::Unsigned, "dec", "decimal", false},
    FormatInfo{Format::Signed, "signed", "signed", true},
    FormatInfo{Format::Hex, "hex", "hex", false},
};

struct ValueType {
  ValueSize size = ValueSize::Bits8;
  Format format = Format::Unsigned;
  auto operator==(const ValueType&) const -> bool = default;
};

enum class Operand : uint8_t { None, Value, Step };

enum class Compare : uint8_t { Equal, NotEqual, Less, Greater, Changed, Unchanged, Increased, Decreased };

struct CompareInfo {
  Compare id;
  std::string_view key;
  std::string_view label;
  Operand operand;
};

inline constexpr std::array kCompares = {
    CompareInfo{Compare::Equal, "eq", "equal to", Operand::Value},
    CompareInfo{Compare::NotEqual, "ne", "not equal to", Operand::Value},
    CompareInfo{Compare::Less, "lt", "less than", Operand::Value},
    CompareInfo{Compare::Greater, "gt", "greater than", Operand::Value},
    CompareInfo{Compare::Changed, "changed", "changed", Operand::None},
    CompareInfo{Compare::Unchanged, "unchanged", "unchanged", Operand::None},
    CompareInfo{Compare::Increased, "increased", "increased by", Operand::Step},
    CompareInfo{Compare::Decreased, "decreased", "decreased by", Operand::Step},
};

constexpr auto info(Region region) -> const RegionInfo& { return kRegions[static_cast<size_t>(region)]; }
constexpr auto info(ValueSize size) -> const SizeInfo& { return kSizes[static_cast<size_t>(size)]; }
constexpr auto info(Format format) -> const FormatInfo& { return kFormats[static_cast<size_t>(format)]; }
constexpr auto info(Compare compare) -> const CompareInfo& { return kCompares[static_cast<size_t>(compare)]; }

constexpr auto bytes_of(ValueSize size) -> uint32_t { return static_cast<uint32_t>(info(size).bits / 8); }

constexpr auto mask_of(ValueSize size) -> uint32_t {
  const int bits = info(size).bits;
  return bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
}

// True when `size` bytes at `offset` sit inside a region of `region_bytes`.
constexpr auto fits(size_t region_bytes, uint32_t offset, ValueSize size) -> bool {
  const uint32_t width = bytes_of(size);
  return static_cast<uint64_t>(offset) + width <= region_bytes;
}

// The number a condition compares: signed formats sign-extend, the others stay unsigned.
inline auto to_number(uint32_t raw, ValueType type) -> int64_t {
  raw &= mask_of(type.size);
  if (!info(type.format).is_signed) return raw;
  const int bits = info(type.size).bits;
  const uint32_t sign = 1u << (bits - 1);
  if ((raw & sign) == 0) return raw;
  return static_cast<int64_t>(raw) - (static_cast<int64_t>(1) << bits);
}

// Case-insensitive match of an info table's `key`. The id type is whatever the table stores.
template <typename Info, size_t N>
auto find_key(const std::array<Info, N>& table, std::string_view key) -> std::optional<decltype(Info::id)> {
  auto same = [](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
      const auto ca = static_cast<unsigned char>(a[i]);
      const auto cb = static_cast<unsigned char>(b[i]);
      if (ca == cb) continue;
      if ((ca ^ cb) != 0x20) return false;
      if (ca < 'A' || (ca > 'Z' && ca < 'a') || ca > 'z') return false;
    }
    return true;
  };
  for (const Info& item : table) {
    if (same(item.key, key)) return item.id;
  }
  return std::nullopt;
}

auto read_value(std::span<const uint8_t> memory, uint32_t offset, ValueSize size) -> uint32_t;
auto write_value(std::span<uint8_t> memory, uint32_t offset, ValueSize size, uint32_t value) -> bool;
auto parse_value(std::string_view text, ValueType type) -> std::optional<uint32_t>;
auto format_value(uint32_t raw, ValueType type) -> std::string;
auto format_address(Region region, uint32_t offset) -> std::string;
// `$7E1FE8`, `7E1FE8`, or a bare hex offset. Nullopt when it falls outside the region.
auto parse_address(Region region, std::string_view text, size_t region_bytes) -> std::optional<uint32_t>;

}  // namespace bitforge::debug
