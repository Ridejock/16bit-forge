// SPDX-License-Identifier: MIT
#pragma once
// Cheat codes: Game Genie and Pro Action Replay decoding, and the engine that applies them.
//
// Codes (several can be joined with '+'):
//   Game Genie   DDAA-AAAA  (the GG alphabet and address scramble; always a ROM read override)
//   Pro Action   AAAAAAVV   (8 hex digits; a WRAM address is written every frame, anything
//   Replay                   else is a read override)
//   raw          AAAAAA:VV or AAAAAA:CC:VV (read override; with CC only while the real byte is CC)
//
// Read overrides answer S-CPU and DMA reads of the address (and its FastROM mirror: bank bit 7
// is ignored outside WRAM) with the code's value. Per-frame writes go to WRAM at the start of
// each frame (vblank, when the real PAR writes on NMI). Cheats are configuration: they're not
// part of save states.
#include "snes/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bitforge {

struct Cheat {
  enum class Kind : uint8_t { ReadOverride, RamWrite };
  Kind kind = Kind::ReadOverride;
  Address address = 0;
  Byte value = 0;
  std::optional<Byte> compare;
  auto operator==(const Cheat&) const -> bool = default;
};

// One code (or '+'-joined codes) to cheats; nullopt if any part isn't a valid code.
auto decode_cheat(std::string_view code) -> std::optional<std::vector<Cheat>>;
auto decode_game_genie(std::string_view code) -> std::optional<Cheat>;
auto decode_pro_action_replay(std::string_view code) -> std::optional<Cheat>;
// The Game Genie code for a ROM address and value (for tests and the UI).
auto encode_game_genie(Address address, Byte value) -> std::string;

// A cheats-file entry: whether it's on, the code text, and what it does.
struct CheatEntry {
  bool enabled = true;
  std::string code;
  std::string description;
  auto operator==(const CheatEntry&) const -> bool = default;
};

// The per-ROM text file, one entry per line: "on" or "off", the code, then the description.
// Blank lines and lines starting with '#' are skipped, and so are lines that don't parse.
auto parse_cheat_file(std::string_view text) -> std::vector<CheatEntry>;
auto format_cheat_file(std::span<const CheatEntry> entries) -> std::string;

class CheatEngine {
public:
  // The enabled entries' codes; invalid ones are skipped.
  auto set(std::span<const CheatEntry> entries) -> void;
  auto clear() -> void { set({}); }
  auto active() const -> bool { return !reads_.empty() || !writes_.empty(); }

  // The bus asks on every read: the byte to return instead of `real`.
  auto read(Address address, Byte real) const -> Byte {
    if (reads_.empty() || !banks_[bank_of(address)]) return real;
    return lookup(address, real);
  }
  auto ram_writes() const -> std::span<const Cheat> { return writes_; }

private:
  static auto normalize(Address address) -> Address;
  static auto bank_of(Address address) -> Byte { return static_cast<Byte>((normalize(address) >> 16) & 0xFF); }
  auto lookup(Address address, Byte real) const -> Byte;

  std::vector<Cheat> reads_;   // sorted by normalized address
  std::vector<Cheat> writes_;
  bool banks_[256] = {};       // normalized banks with a read override
};

} // namespace bitforge
