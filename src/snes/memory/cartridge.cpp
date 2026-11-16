// SPDX-License-Identifier: MIT
#include "snes/memory/cartridge.hpp"
#include "utils/logging.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>

namespace snes::memory {

namespace {

// Where each layout keeps its 64-byte header block ($xxC0-$xxFF of the bank holding the vectors).
struct Layout {
  MapMode mode;
  Address header;
};
constexpr std::array<Layout, 2> kLayouts = {{
  {MapMode::LoROM, 0x7FC0},
  {MapMode::HiROM, 0xFFC0},
}};

constexpr std::size_t kCopierHeader = 512;
constexpr std::size_t kMaxSramBytes = 128 * 1024;

// Header fields, relative to the header block.
constexpr Address kMapByte = 0x15;
constexpr Address kComplement = 0x1C;
constexpr Address kChecksum = 0x1E;
constexpr Address kResetVector = 0x3C;

auto read16(const std::vector<Byte>& rom, Address at) -> Word {
  return static_cast<Word>(rom[at] | (rom[at + 1] << 8));
}

// Map byte ($xxD5) low nibble -> the layout it promises: SA-1 ($23) and S-DD1 ($22) boards are
// LoROM-style, SPC7110 ($2A) and ExHiROM ($25) HiROM-style.
constexpr std::array<int, 16> kMapNibbleHiRom = {0, 1, 0, 0, -1, 1, -1, -1, -1, -1, 1, -1, -1, -1, -1, -1};

// Higher is more likely. Checks the checksum pair, the map byte and the reset vector.
auto score_layout(const std::vector<Byte>& rom, const Layout& layout) -> int {
  if (rom.size() < layout.header + 0x40) return -1;
  int score = 0;
  const Word checksum = read16(rom, layout.header + kChecksum);
  const Word complement = read16(rom, layout.header + kComplement);
  if (static_cast<Word>(checksum ^ complement) == 0xFFFF) score += 4;
  const Byte map = rom[layout.header + kMapByte];
  const int hi = kMapNibbleHiRom[map & 0x0F];
  if ((map & 0xE0) == 0x20 && hi >= 0 && (hi == 1) == (layout.mode == MapMode::HiROM)) score += 2;
  if (read16(rom, layout.header + kResetVector) >= 0x8000) score += 1;
  return score;
}

} // namespace

Cartridge::Cartridge() = default;
Cartridge::~Cartridge() = default;

auto Cartridge::load(const std::string& path) -> bool {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    utils::Logger::error(utils::LogChannel::Rom, "Failed to open ROM: %s", path.c_str());
    return false;
  }

  const auto size = static_cast<std::size_t>(file.tellg());
  file.seekg(0, std::ios::beg);
  rom.resize(size);
  file.read(reinterpret_cast<char*>(rom.data()), static_cast<std::streamsize>(size));

  // Strip a copier header if present.
  if (rom.size() % 1024 == kCopierHeader) {
    rom.erase(rom.begin(), rom.begin() + static_cast<std::ptrdiff_t>(kCopierHeader));
  }
  if (rom.size() < 0x8000) {
    utils::Logger::error(utils::LogChannel::Rom, "ROM too small: %zu bytes", rom.size());
    return false;
  }

  parse_header();
  rom_crc = crc32(rom);
  dirty = false;
  if (!attach_coprocessor()) {
    utils::Logger::error(utils::LogChannel::Rom, "%s", error.c_str());
    return false;
  }
  loaded = true;

  utils::Logger::info(utils::LogChannel::Rom, "Loaded ROM: %.21s (%zu bytes, %s, SRAM %zu bytes)", header.name, rom.size(),
                      mode == MapMode::HiROM ? "HiROM" : "LoROM", sram.size());
  return true;
}

auto Cartridge::load_raw(const std::vector<Byte>& data, MapMode map, std::size_t sram_bytes) -> void {
  rom = data;
  mode = map;
  sram.assign(sram_bytes, 0x00);
  chip.reset();
  board_info = {};
  rom_crc = crc32(rom);
  dirty = false;
  loaded = true;
  utils::Logger::info(utils::LogChannel::Rom, "Loaded raw ROM: %zu bytes, first byte: 0x%02X",
                      rom.size(), rom.empty() ? 0 : rom[0]);
}

auto Cartridge::parse_header() -> void {
  const Layout* best = &kLayouts[0];
  int best_score = -1;
  for (const auto& layout : kLayouts) {
    const int score = score_layout(rom, layout);
    if (score > best_score) {
      best = &layout;
      best_score = score;
    }
  }
  mode = best->mode;
  header_offset = best->header;
  std::memcpy(&header, &rom[best->header], sizeof(CartridgeHeader));

  const std::size_t sram_bytes = header.sram_size == 0 || header.sram_size > 7
                                     ? 0
                                     : std::size_t{1024} << header.sram_size;
  sram.assign(std::min(sram_bytes, kMaxSramBytes), 0x00);
}

// Builds the board's coprocessor, if it has one. False (with `error` set) when the chip needs
// firmware that isn't there.
auto Cartridge::attach_coprocessor() -> bool {
  error.clear();
  warning.clear();
  chip.reset();
  board_info = coprocessor::detect_board(rom, header_offset, mode == MapMode::HiROM);
  if (board_info.chip == coprocessor::Chip::None) return true;

  // Super FX work RAM and SA-1 BW-RAM sizes come from the (extended) header.
  if (board_info.chip == coprocessor::Chip::SuperFX || board_info.chip == coprocessor::Chip::SA1) {
    sram.assign(board_info.ram_bytes, 0x00);
  }
  chip = coprocessor::create_coprocessor(board_info, rom, sram, firmware_dir, error);
  if (!error.empty()) return false;
  if (!chip) {
    warning = std::string(coprocessor::chip_name(board_info.chip)) + " isn't emulated yet: the game won't run properly";
  } else {
    utils::Logger::info(utils::LogChannel::Rom, "Coprocessor: %s, RAM %zu bytes%s", coprocessor::chip_name(chip->chip()),
                        sram.size(), board_info.battery ? " (battery)" : "");
  }
  const auto battery = battery_span();
  battery_copy.assign(battery.begin(), battery.end());
  return true;
}

auto Cartridge::battery_span() const -> std::span<const Byte> {
  if (chip) {
    if (auto own = chip->battery_ram(); !own.empty()) return own;
    if (!board_info.battery) return {};
  }
  return sram;
}

auto Cartridge::sram_data() -> std::span<Byte> {
  if (chip) {
    if (auto own = chip->battery_ram(); !own.empty()) return own;
    if (!board_info.battery) return {};
  }
  return sram;
}

auto Cartridge::sram_dirty() const -> bool {
  if (dirty) return true;
  if (!chip) return false;
  const auto battery = battery_span();
  return battery.size() != battery_copy.size() || !std::equal(battery.begin(), battery.end(), battery_copy.begin());
}

auto Cartridge::clear_sram_dirty() -> void {
  dirty = false;
  if (!chip) return;
  const auto battery = battery_span();
  battery_copy.assign(battery.begin(), battery.end());
}

// ROM is mirrored to fill whatever the layout addresses.
auto Cartridge::rom_offset(Address address) const -> std::optional<Address> {
  const Byte bank = static_cast<Byte>(address >> 16);
  const Word offset = static_cast<Word>(address);
  Address linear = 0;

  if (mode == MapMode::LoROM) {
    // 32KB per bank in $8000-$FFFF; banks $40-$7D/$C0-$FF also mirror it in the lower half.
    const bool upper_half = offset >= 0x8000;
    const bool lower_rom_bank = (bank & 0x7F) >= 0x40 && (bank & 0x7F) < 0x70;
    if (!upper_half && !lower_rom_bank) return std::nullopt;
    linear = (static_cast<Address>(bank & 0x7F) << 15) | (offset & 0x7FFF);
  } else {
    // 64KB per bank at $40-$7D/$C0-$FF; banks $00-$3F/$80-$BF mirror the upper halves.
    const bool full_bank = (bank & 0x7F) >= 0x40;
    if (!full_bank && offset < 0x8000) return std::nullopt;
    linear = (static_cast<Address>(bank & 0x3F) << 16) | offset;
  }
  if (rom.empty()) return std::nullopt;
  return static_cast<Address>(linear % rom.size());
}

auto Cartridge::sram_offset(Address address) const -> std::optional<Address> {
  if (sram.empty()) return std::nullopt;
  const Byte bank = static_cast<Byte>(address >> 16);
  const Word offset = static_cast<Word>(address);

  Address linear = 0;
  if (mode == MapMode::LoROM) {
    // Banks $70-$7D and $F0-$FF, $0000-$7FFF.
    if ((bank & 0x7F) < 0x70 || offset >= 0x8000) return std::nullopt;
    linear = (static_cast<Address>(bank & 0x0F) << 15) | offset;
  } else {
    // Banks $20-$3F and $A0-$BF, $6000-$7FFF (8KB windows).
    if ((bank & 0x7F) < 0x20 || (bank & 0x7F) >= 0x40 || offset < 0x6000 || offset >= 0x8000) {
      return std::nullopt;
    }
    linear = (static_cast<Address>(bank & 0x1F) << 13) | (offset - 0x6000);
  }
  return static_cast<Address>(linear % sram.size());
}

auto Cartridge::read(Address address) -> std::optional<Byte> {
  if (!loaded) return std::nullopt;
  if (chip) {
    auto value = chip->read(address);
    if (value || chip->owns_map()) return value;
  }
  if (auto at = sram_offset(address)) return sram[*at];
  if (auto at = rom_offset(address)) return rom[*at];
  return std::nullopt;
}

auto Cartridge::write(Address address, Byte value) -> void {
  if (!loaded) return;
  if (chip && (chip->write(address, value) || chip->owns_map())) return;
  if (auto at = sram_offset(address)) {
    if (sram[*at] != value) dirty = true;
    sram[*at] = value;
  }
}

auto Cartridge::serialize(Archive& ar) -> void {
  auto size = static_cast<uint32_t>(sram.size());
  ar(size);
  if (size != sram.size()) {  // a state from a cartridge with a different SRAM size
    ar.fail();
    return;
  }
  ar.bytes(sram.data(), sram.size());
  if (ar.loading()) dirty = true;  // the battery file should follow the loaded state

  // Version 2: the coprocessor (its id first, so a state can't land on a different chip).
  auto id = static_cast<uint8_t>(chip ? chip->chip() : coprocessor::Chip::None);
  const uint8_t expected = id;
  ar(id);
  if (id != expected) {
    ar.fail();
    return;
  }
  if (chip) chip->serialize(ar);
}

} // namespace snes::memory
