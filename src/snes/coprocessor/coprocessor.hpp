// SPDX-License-Identifier: MIT
#pragma once
// Cartridge coprocessors (enhancement chips).
//
// A Coprocessor plugs into the cartridge slot: the Cartridge hands it every S-CPU access to
// cartridge space (and to the $2000-$5FFF expansion area of the system banks), and the System
// calls step() after every S-CPU instruction with the master clocks that instruction took, so
// the chip runs in lockstep with the S-CPU at instruction granularity. Each chip keeps its own
// clock budget in master-clock units and runs whole instructions until it has caught up.
#include "snes/serialize.hpp"
#include "snes/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bitforge::coprocessor {

enum class Chip : uint8_t {
  None,
  DSP1, DSP1B, DSP2, DSP3, DSP4,  // NEC uPD77C25 (uPD7725) with firmware
  ST010, ST011,                   // NEC uPD96050 with firmware
  SuperFX,                        // GSU-1/GSU-2 (MARIO chip)
  SA1,
  OBC1,
  Cx4,                            // Hitachi HG51B169 with data ROM
  SDD1,                           // S-DD1 decompressor + MMC
  SPC7110,                        // SPC7110 decompressor/ALU/MMC (+ RTC-4513)
  // Detected but not emulated: the cartridge runs as plain ROM and logs a warning.
  ST018, SRTC, SuperGameBoy, Unknown,
};

auto chip_name(Chip chip) -> const char*;

class Coprocessor {
public:
  virtual ~Coprocessor() = default;

  virtual auto chip() const -> Chip = 0;
  virtual auto reset() -> void = 0;

  // S-CPU access to cartridge space. A chip with a full memory map (owns_map() true: Super FX,
  // SA-1) answers every cartridge address itself and returns nullopt for open bus. Other chips
  // return nullopt for addresses they don't decode, and the cartridge's ROM/SRAM map answers.
  virtual auto owns_map() const -> bool { return false; }
  virtual auto read(Address address) -> std::optional<Byte> = 0;
  // Returns true when the write was decoded by the chip.
  virtual auto write(Address address, Byte value) -> bool = 0;

  // The S-CPU's (or DMA's) last bus address, given before each step(): chips that share memory
  // with the S-CPU use it to model bus contention.
  virtual auto set_cpu_address(Address) -> void {}

  // S-CPU writes to the DMA channel registers ($43n0-$43nF; reg = n's register 0-15). The
  // S-DD1 snoops them to learn which source address and size a transfer will use.
  virtual auto dma_register_write(Byte /*channel*/, Byte /*reg*/, Byte /*value*/) -> void {}

  // Run until the chip has caught up with `master_clocks` more S-CPU time.
  virtual auto step(uint32_t master_clocks) -> void = 0;

  // The chip's line into the S-CPU's IRQ input.
  virtual auto irq() const -> bool { return false; }

  // Battery-backed memory the chip keeps itself (ST010/ST011 data RAM). Empty when the
  // cartridge's own RAM is the battery RAM.
  virtual auto battery_ram() -> std::span<Byte> { return {}; }

  // A battery-backed real-time clock's file (<rom>.rtc); empty without one. `now` is Unix time.
  virtual auto rtc_save(int64_t /*now*/) -> std::vector<Byte> { return {}; }
  virtual auto rtc_load(std::span<const Byte> /*data*/, int64_t /*now*/) -> bool { return false; }

  // Save states: everything except ROM and firmware (those are identified by the ROM CRC).
  virtual auto serialize(Archive& ar) -> void = 0;
};

} // namespace bitforge::coprocessor
