// SPDX-License-Identifier: MIT
#include "debug/memory_value.hpp"

#include <cctype>
#include <charconv>
#include <cstdio>

namespace bitforge::debug {

namespace {

auto trim(std::string_view s) -> std::string_view {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

}  // namespace

auto read_value(std::span<const uint8_t> memory, uint32_t offset, ValueSize size) -> uint32_t {
  if (!fits(memory.size(), offset, size)) return 0;
  uint32_t value = 0;
  for (uint32_t i = 0; i < bytes_of(size); i++) value |= static_cast<uint32_t>(memory[offset + i]) << (8 * i);
  return value;
}

auto write_value(std::span<uint8_t> memory, uint32_t offset, ValueSize size, uint32_t value) -> bool {
  if (!fits(memory.size(), offset, size)) return false;
  value &= mask_of(size);
  for (uint32_t i = 0; i < bytes_of(size); i++) memory[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  return true;
}

auto parse_value(std::string_view text, ValueType type) -> std::optional<uint32_t> {
  text = trim(text);
  if (text.empty()) return std::nullopt;
  const uint32_t mask = mask_of(type.size);
  if (type.format == Format::Hex) {
    if (text.front() == '$') text.remove_prefix(1);
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
    if (text.empty()) return std::nullopt;
    uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || end != text.data() + text.size() || value != (value & mask)) return std::nullopt;
    return value;
  }

  const bool negative = text.front() == '-';
  if (negative || text.front() == '+') text.remove_prefix(1);
  if (text.empty() || (negative && type.format != Format::Signed)) return std::nullopt;
  uint32_t magnitude = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), magnitude, 10);
  if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  if (type.format == Format::Unsigned) {
    if (negative || magnitude != (magnitude & mask)) return std::nullopt;
    return magnitude;
  }
  const int bits = info(type.size).bits;
  const int64_t limit = static_cast<int64_t>(1) << (bits - 1);
  const int64_t number = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
  if (number < -limit || number >= limit) return std::nullopt;
  return static_cast<uint32_t>(number) & mask;
}

auto format_value(uint32_t raw, ValueType type) -> std::string {
  raw &= mask_of(type.size);
  char buf[32];
  if (type.format == Format::Hex) {
    std::snprintf(buf, sizeof(buf), "$%0*X", static_cast<int>(bytes_of(type.size) * 2), raw);
    return buf;
  }
  if (type.format == Format::Signed) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(to_number(raw, type)));
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "%u", raw);
  return buf;
}

auto format_address(Region region, uint32_t offset) -> std::string {
  char buf[32];
  if (region == Region::Wram) {
    std::snprintf(buf, sizeof(buf), "$%06X", info(region).bus_base + offset);
  } else {
    std::snprintf(buf, sizeof(buf), "SRAM:$%X", offset);
  }
  return buf;
}

auto parse_address(Region region, std::string_view text, size_t region_bytes) -> std::optional<uint32_t> {
  text = trim(text);
  if (!text.empty() && text.front() == '$') text.remove_prefix(1);
  const auto value = parse_value(text, {ValueSize::Bits24, Format::Hex});
  if (!value) return std::nullopt;
  uint32_t offset = *value;
  if (region == Region::Wram && offset >= info(Region::Wram).bus_base) {
    const uint32_t bus = offset;
    if (bus >= info(Region::Wram).bus_base + kWramBytes) return std::nullopt;
    offset = bus - info(Region::Wram).bus_base;
  }
  if (offset >= region_bytes) return std::nullopt;
  return offset;
}

}  // namespace bitforge::debug
