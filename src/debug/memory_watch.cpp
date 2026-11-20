// SPDX-License-Identifier: MIT
#include "debug/memory_watch.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cinttypes>
#include <cstdio>

namespace bitforge::debug {

namespace {

constexpr std::string_view kHeader =
    "# 16bit Forge memory watches: id, region (wram/sram), offset (hex), size (8/16/24),\n"
    "# format (dec/signed/hex), then the label.\n";

auto trim(std::string_view s) -> std::string_view {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// The next whitespace-separated field of `line`, removed from it.
auto take_field(std::string_view& line) -> std::string_view {
  line = trim(line);
  size_t end = 0;
  while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) end++;
  const std::string_view field = line.substr(0, end);
  line.remove_prefix(end);
  return field;
}

// Bare hex digits (the file never writes a prefix or a sign).
auto parse_offset(std::string_view s) -> std::optional<uint32_t> {
  if (s.empty() || !std::isxdigit(static_cast<unsigned char>(s.front()))) return std::nullopt;
  return parse_value(s, {ValueSize::Bits24, Format::Hex});
}

auto parse_id(std::string_view s) -> std::optional<WatchId> {
  if (s.empty() || !std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
    return std::nullopt;
  WatchId id = kNoWatch;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), id);
  if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
  return id;
}

auto parse_line(std::string_view line) -> std::optional<Watch> {
  Watch w;
  std::string_view first = take_field(line);
  if (const auto id = parse_id(first)) {
    w.id = *id;
    first = take_field(line);
  }
  const auto region = find_key(kRegions, first);
  const auto offset = parse_offset(take_field(line));
  const auto size = find_key(kSizes, take_field(line));
  const auto format = find_key(kFormats, take_field(line));
  if (!region || !offset || !size || !format) return std::nullopt;
  w.region = *region;
  w.offset = *offset;
  w.type = {*size, *format};
  w.label = std::string(trim(line));
  return w;
}

auto as_number(const Watch& w, uint32_t raw) -> int64_t { return to_number(raw & mask_of(w.type.size), w.type); }

}  // namespace

auto assign_watch_ids(std::vector<Watch>& watches) -> void {
  WatchId next = kNoWatch;
  for (const Watch& w : watches) next = std::max(next, w.id);
  for (size_t i = 0; i < watches.size(); i++) {
    const WatchId id = watches[i].id;
    const bool taken = std::any_of(watches.begin(), watches.begin() + static_cast<std::ptrdiff_t>(i),
                                   [id](const Watch& earlier) { return earlier.id == id; });
    if (id == kNoWatch || taken) watches[i].id = ++next;
  }
}

auto find_watch(std::span<const Watch> watches, WatchId id) -> const Watch* {
  const auto it = std::find_if(watches.begin(), watches.end(), [id](const Watch& w) { return w.id == id; });
  return it == watches.end() ? nullptr : &*it;
}

auto any_frozen(std::span<const Watch> watches) -> bool {
  return std::any_of(watches.begin(), watches.end(), [](const Watch& w) { return w.frozen; });
}

auto read_watch(const Watch& watch, const RegionMemory& memory) -> std::optional<uint32_t> {
  const std::span<uint8_t> region = memory[static_cast<size_t>(watch.region)];
  if (!fits(region.size(), watch.offset, watch.type.size)) return std::nullopt;
  return read_value(region, watch.offset, watch.type.size);
}

auto sample_watches(std::span<const Watch> watches, const RegionMemory& memory, std::span<const WatchReading> last,
                    std::vector<WatchReading>& out) -> void {
  out.resize(watches.size());
  for (size_t i = 0; i < watches.size(); i++) {
    const Watch& w = watches[i];
    const auto now = read_watch(w, memory);
    // Usually the list is unchanged since last frame, so the same index has the same id.
    const WatchReading* prev = i < last.size() && last[i].id == w.id ? &last[i] : nullptr;
    if (!prev) {
      const auto it = std::find_if(last.begin(), last.end(), [&w](const WatchReading& r) { return r.id == w.id; });
      if (it != last.end()) prev = &*it;
    }
    const uint32_t value = now.value_or(0);
    out[i] = {w.id, value, prev && prev->valid ? prev->now : value, now.has_value()};
  }
}

auto evaluate(const Condition& condition, std::span<const Watch> watches, std::span<const WatchReading> readings)
    -> bool {
  const Watch* w = find_watch(watches, condition.watch);
  if (!w) return false;
  const auto it = std::find_if(readings.begin(), readings.end(),
                               [&condition](const WatchReading& r) { return r.id == condition.watch; });
  if (it == readings.end() || !it->valid) return false;
  return info(condition.test).test(as_number(*w, it->before), as_number(*w, it->now), as_number(*w, condition.value));
}

auto apply_freezes(std::span<const Watch> watches, const RegionMemory& memory) -> uint32_t {
  uint32_t changed = 0;
  for (const Watch& w : watches) {
    if (!w.frozen) continue;
    const std::span<uint8_t> region = memory[static_cast<size_t>(w.region)];
    if (!fits(region.size(), w.offset, w.type.size)) continue;
    const uint32_t value = w.value & mask_of(w.type.size);
    if (read_value(region, w.offset, w.type.size) == value) continue;
    write_value(region, w.offset, w.type.size, value);
    changed |= 1u << static_cast<uint32_t>(w.region);
  }
  return changed;
}

auto parse_watch_file(std::string_view text) -> std::vector<Watch> {
  std::vector<Watch> out;
  while (!text.empty()) {
    const size_t eol = text.find('\n');
    const std::string_view line = trim(text.substr(0, eol));
    text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
    if (line.empty() || line.front() == '#') continue;
    if (auto w = parse_line(line)) out.push_back(std::move(*w));
  }
  assign_watch_ids(out);
  return out;
}

auto parse_watch_line(std::string_view line) -> std::optional<Watch> { return parse_line(trim(line)); }

auto format_watch_line(const Watch& w) -> std::string {
  char line[64];
  const SizeInfo& size = info(w.type.size);
  std::snprintf(line, sizeof(line), "%" PRIu32 " %.*s %06" PRIX32 " %.*s %.*s", w.id, static_cast<int>(info(w.region).key.size()),
                info(w.region).key.data(), w.offset, static_cast<int>(size.key.size()), size.key.data(),
                static_cast<int>(info(w.type.format).key.size()), info(w.type.format).key.data());
  std::string out = line;
  if (!w.label.empty()) out += " " + w.label;
  return out;
}

auto format_watch_file(std::span<const Watch> watches) -> std::string {
  std::string out(kHeader);
  for (const Watch& w : watches) out += format_watch_line(w) + '\n';
  return out;
}

auto watch_to_cheat(const Watch& watch, uint32_t value) -> std::optional<CheatEntry> {
  const RegionInfo& region = info(watch.region);
  if (!region.cheatable || !fits(kWramBytes, watch.offset, watch.type.size)) return std::nullopt;
  CheatEntry entry;
  for (uint32_t i = 0; i < bytes_of(watch.type.size); i++) {
    char code[16];
    std::snprintf(code, sizeof(code), "%s%06" PRIX32 "%02" PRIX32, i ? "+" : "", region.bus_base + watch.offset + i,
                  (value >> (8 * i)) & 0xFF);
    entry.code += code;
  }
  entry.description = watch.label.empty() ? format_address(watch.region, watch.offset) : watch.label;
  return entry;
}

}  // namespace bitforge::debug
