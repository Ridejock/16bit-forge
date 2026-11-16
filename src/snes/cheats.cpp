// SPDX-License-Identifier: MIT
#include "snes/cheats.hpp"

#include <algorithm>
#include <cctype>

namespace snes {

namespace {

constexpr std::string_view kGenieDigits = "DF4709156BC8A23E";  // GG digit -> hex value (by index)

// ijklqrst opabcduv wxefghmn (code bit order) -> abcdefgh ijklmnop qrstuvwx (address):
// for each code bit position (23 = MSB), which address bit it holds.
constexpr int kGenieBits[24] = {
  // i  j  k  l  q  r  s  t
  15, 14, 13, 12, 7, 6, 5, 4,
  // o  p  a  b  c  d  u  v
  9, 8, 23, 22, 21, 20, 3, 2,
  // w  x  e  f  g  h  m  n
  1, 0, 19, 18, 17, 16, 11, 10,
};

auto trim(std::string_view s) -> std::string_view {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

auto hex_value(char c) -> int {
  if (c >= '0' && c <= '9') return c - '0';
  c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

auto parse_hex(std::string_view s, size_t digits) -> std::optional<uint32_t> {
  if (s.size() != digits) return std::nullopt;
  uint32_t v = 0;
  for (const char c : s) {
    const int d = hex_value(c);
    if (d < 0) return std::nullopt;
    v = v << 4 | static_cast<uint32_t>(d);
  }
  return v;
}

auto is_wram(Address address) -> bool {
  const Byte bank = static_cast<Byte>(address >> 16);
  return bank == 0x7E || bank == 0x7F || ((bank & 0x40) == 0 && (address & 0xFFFF) < 0x2000);
}

}  // namespace

auto decode_game_genie(std::string_view code) -> std::optional<Cheat> {
  code = trim(code);
  if (code.size() != 9 || code[4] != '-') return std::nullopt;
  uint32_t raw = 0;
  for (size_t i = 0; i < 9; i++) {
    if (i == 4) continue;
    const auto at = kGenieDigits.find(static_cast<char>(std::toupper(static_cast<unsigned char>(code[i]))));
    if (at == std::string_view::npos) return std::nullopt;
    raw = raw << 4 | static_cast<uint32_t>(at);
  }
  Address address = 0;
  for (int bit = 0; bit < 24; bit++) {
    if (raw & (1u << (23 - bit))) address |= 1u << kGenieBits[bit];
  }
  return Cheat{Cheat::Kind::ReadOverride, address, static_cast<Byte>(raw >> 24), std::nullopt};
}

auto encode_game_genie(Address address, Byte value) -> std::string {
  uint32_t raw = static_cast<uint32_t>(value) << 24;
  for (int bit = 0; bit < 24; bit++) {
    if (address & (1u << kGenieBits[bit])) raw |= 1u << (23 - bit);
  }
  std::string out;
  for (int i = 7; i >= 0; i--) {
    out += kGenieDigits[(raw >> (4 * i)) & 0xF];
    if (i == 4) out += '-';
  }
  return out;
}

auto decode_pro_action_replay(std::string_view code) -> std::optional<Cheat> {
  const auto v = parse_hex(trim(code), 8);
  if (!v) return std::nullopt;
  const Address address = *v >> 8;
  return Cheat{is_wram(address) ? Cheat::Kind::RamWrite : Cheat::Kind::ReadOverride, address, static_cast<Byte>(*v), std::nullopt};
}

auto decode_cheat(std::string_view code) -> std::optional<std::vector<Cheat>> {
  std::vector<Cheat> out;
  code = trim(code);
  if (code.empty()) return std::nullopt;
  for (bool more = true; more;) {
    const size_t plus = code.find('+');
    const std::string_view part = trim(code.substr(0, plus));
    more = plus != std::string_view::npos;
    if (more) code = code.substr(plus + 1);
    if (const auto gg = decode_game_genie(part)) {
      out.push_back(*gg);
    } else if (const auto par = decode_pro_action_replay(part)) {
      out.push_back(*par);
    } else if (part.size() >= 9 && part[6] == ':') {  // AAAAAA:VV or AAAAAA:CC:VV
      const auto address = parse_hex(part.substr(0, 6), 6);
      std::optional<uint32_t> compare, value;
      if (part.size() == 9) {
        value = parse_hex(part.substr(7), 2);
      } else if (part.size() == 12 && part[9] == ':') {
        compare = parse_hex(part.substr(7, 2), 2);
        value = parse_hex(part.substr(10), 2);
        if (!compare) return std::nullopt;
      }
      if (!address || !value) return std::nullopt;
      Cheat c{Cheat::Kind::ReadOverride, *address, static_cast<Byte>(*value), std::nullopt};
      if (compare) c.compare = static_cast<Byte>(*compare);
      out.push_back(c);
    } else {
      return std::nullopt;
    }
  }
  return out;
}

auto parse_cheat_file(std::string_view text) -> std::vector<CheatEntry> {
  std::vector<CheatEntry> out;
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    std::string_view line = trim(text.substr(0, nl));
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (line.empty() || line.front() == '#') continue;
    auto token = [&line]() {
      line = trim(line);
      const size_t end = std::min(line.find_first_of(" \t"), line.size());
      const std::string_view t = line.substr(0, end);
      line.remove_prefix(end);
      return t;
    };
    const std::string_view state = token();
    const std::string_view code = token();
    if ((state != "on" && state != "off") || !decode_cheat(code)) continue;
    out.push_back({state == "on", std::string(code), std::string(trim(line))});
  }
  return out;
}

auto format_cheat_file(std::span<const CheatEntry> entries) -> std::string {
  std::string out = "# SNES cheats: on|off, the code (Game Genie DDAA-AAAA, Pro Action Replay AAAAAAVV,\n"
                    "# or AAAAAA:VV / AAAAAA:CC:VV; join several with +), then a description.\n";
  for (const auto& e : entries) {
    out += e.enabled ? "on  " : "off ";
    out += e.code;
    if (!e.description.empty()) out += "  " + e.description;
    out += '\n';
  }
  return out;
}

auto CheatEngine::normalize(Address address) -> Address {
  address &= 0xFFFFFF;
  const Byte bank = static_cast<Byte>(address >> 16);
  if ((bank & 0x7E) == 0x7E) return address;  // $7E-$7F WRAM, $FE-$FF ROM: no mirror between them
  return address & 0x7FFFFF;
}

auto CheatEngine::set(std::span<const CheatEntry> entries) -> void {
  reads_.clear();
  writes_.clear();
  std::fill(std::begin(banks_), std::end(banks_), false);
  for (const auto& e : entries) {
    if (!e.enabled) continue;
    const auto cheats = decode_cheat(e.code);
    if (!cheats) continue;
    for (Cheat c : *cheats) {
      if (c.kind == Cheat::Kind::RamWrite) {
        writes_.push_back(c);
        continue;
      }
      c.address = normalize(c.address);
      banks_[(c.address >> 16) & 0xFF] = true;
      reads_.push_back(c);
    }
  }
  std::stable_sort(reads_.begin(), reads_.end(), [](const Cheat& a, const Cheat& b) { return a.address < b.address; });
}

auto CheatEngine::lookup(Address address, Byte real) const -> Byte {
  const Address key = normalize(address);
  auto it = std::lower_bound(reads_.begin(), reads_.end(), key, [](const Cheat& c, Address a) { return c.address < a; });
  for (; it != reads_.end() && it->address == key; ++it) {
    if (!it->compare || *it->compare == real) return it->value;
  }
  return real;
}

} // namespace snes
