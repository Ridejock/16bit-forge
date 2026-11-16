// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include "snes/coprocessor/board.hpp"
#include "snes/coprocessor/coprocessor.hpp"
#include "snes/serialize.hpp"
#include "snes/types.hpp"

namespace snes::memory {

struct CartridgeHeader {
  char name[21];
  Byte rom_layout;
  Byte cartridge_type;
  Byte rom_size;
  Byte sram_size;
  Byte country;
  Byte license;
  Byte version;
};

enum class MapMode { LoROM, HiROM };

class Cartridge {
public:
  Cartridge();
  ~Cartridge();

  auto load(const std::string& path) -> bool;
  auto load_raw(const std::vector<Byte>& data, MapMode mode = MapMode::LoROM, std::size_t sram_bytes = 0) -> void;
  auto is_loaded() const -> bool { return loaded; }
  auto map_mode() const -> MapMode { return mode; }
  auto sram_size() const -> std::size_t { return sram.size(); }
  // CRC-32 of the ROM image (copier header stripped): identifies the game in save states.
  auto rom_crc32() const -> uint32_t { return rom_crc; }

  // Battery-backed SRAM. `sram_dirty` is set by any write that changes a byte. On coprocessor
  // boards the chip writes its RAM itself (GSU plots, SA-1 BW-RAM), so there the battery RAM is
  // compared with a copy taken at the last clear instead; RAM without a battery isn't saved.
  auto sram_data() -> std::span<Byte>;
  auto sram_dirty() const -> bool;
  auto clear_sram_dirty() -> void;

  // ---- Enhancement chips ----------------------------------------------------------------------
  // Folder holding user-supplied coprocessor firmware (dsp1b.rom, st010.rom, ...).
  auto set_firmware_dir(std::string dir) -> void { firmware_dir = std::move(dir); }
  // Why the last load() failed (missing firmware, ...), and what works only partly.
  auto load_error() const -> const std::string& { return error; }
  auto load_warning() const -> const std::string& { return warning; }
  auto board() const -> const coprocessor::BoardInfo& { return board_info; }
  auto coprocessor() -> coprocessor::Coprocessor* { return chip.get(); }
  // Called after every S-CPU instruction with its master clocks and its last bus address.
  auto step(uint32_t master_clocks, Address cpu_address) -> void {
    if (!chip) return;
    chip->set_cpu_address(cpu_address);
    chip->step(master_clocks);
  }
  auto irq() const -> bool { return chip && chip->irq(); }
  auto reset() -> void { if (chip) chip->reset(); }

  // Save states carry SRAM (the ROM itself is identified by its CRC, not stored).
  auto serialize(Archive& ar) -> void;

  // Cartridge-space reads return nothing when the address maps to neither ROM nor SRAM (open bus).
  // A coprocessor sees the access first (reads can have side effects, hence non-const).
  auto read(Address address) -> std::optional<Byte>;
  auto write(Address address, Byte value) -> void;

private:
  bool loaded = false;
  MapMode mode = MapMode::LoROM;
  std::vector<Byte> rom;
  std::vector<Byte> sram;
  CartridgeHeader header{};
  uint32_t rom_crc = 0;
  bool dirty = false;
  std::size_t header_offset = 0x7FC0;

  coprocessor::BoardInfo board_info;
  std::unique_ptr<coprocessor::Coprocessor> chip;
  std::string firmware_dir = "firmware";
  std::string error;
  std::string warning;
  std::vector<Byte> battery_copy;  // coprocessor boards: battery RAM at the last clear

  auto parse_header() -> void;
  auto attach_coprocessor() -> bool;
  auto battery_span() const -> std::span<const Byte>;
  auto rom_offset(Address address) const -> std::optional<Address>;
  auto sram_offset(Address address) const -> std::optional<Address>;
};

} // namespace snes::memory
