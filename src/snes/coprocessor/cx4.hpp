// SPDX-License-Identifier: MIT
#pragma once
// Cx4 (Mega Man X2, Mega Man X3): a Hitachi HG51B169 DSP at 20 MHz, low-level emulated.
//
// The HG51B is a 24-bit RISC with 16-bit instructions. It can't run from ROM directly: programs
// are copied 512 bytes at a time into one of two 256-instruction cache pages ("program RAM"),
// addressed by a 15-bit program bank (PB) and 8-bit PC. It has a 24-bit accumulator, sixteen
// 24-bit GPRs, a 24x24 -> 48-bit signed multiplier, an 8-deep call stack, 3 KB of data RAM
// shared with the S-CPU, a 1024 x 24-bit data ROM (generated math tables by default, or an
// optional cx4.data.rom: sine, reciprocal and square-root tables), a DMA engine and an
// external bus port (MAR/MDR) with programmable ROM/RAM wait states.
//
// S-CPU map (LoROM board, banks $00-$3F/$80-$BF):
//   $6000-$6BFF, $7000-$7BFF  data RAM (3 KB, mirrored at $7000)
//   $6C00-$6FFF, $7C00-$7FFF  I/O, decoded as $7Cxx:
//     $7F40-42 DMA source  $7F43-44 DMA length  $7F45-47 DMA target (write $7F47 starts it)
//     $7F48 cache page (write: preload PB into that page)  $7F49-4B cache base  $7F4C page locks
//     $7F4D-4E program bank  $7F4F PC (write: start executing)  $7F50 wait states (RAM lo, ROM hi)
//     $7F51 IRQ disable/ack  $7F52 ROM config  $7F53 stop  $7F55-5C suspend  $7F5D resume
//     $7F5E clear IRQ flag  $7F5F status (bit 7 busy, bit 6 running, bit 1 IRQ, bit 0 suspended)
//     $7F60-7F7F vectors (seen by the S-CPU at $00:FFE0-FFFF while the chip owns the bus)
//     $7F80-7FAF, $7FC0-7FEF  GPR 0-15, three bytes each
// ROM ($00-$3F/$80-$BF:8000-FFFF, $C0-$FF) and RAM ($70-$77:0000-7FFF) are the cartridge's,
// which the HG51B reaches over its own bus for cache fills, DMA and MAR/MDR transfers.
#include "snes/coprocessor/coprocessor.hpp"

#include <array>
#include <vector>

namespace bitforge::coprocessor {

class Cx4 final : public Coprocessor {
public:
  static constexpr uint32_t kClockHz = 20'000'000;

  // `data_rom` is the 3072-byte image (1024 little-endian 24-bit words), usually from
  // generate_cx4_data_rom() or a user-supplied firmware file.
  Cx4(const std::vector<Byte>& rom, std::vector<Byte>& ram, std::span<const Byte> data_rom);

  auto chip() const -> Chip override { return Chip::Cx4; }
  auto reset() -> void override;
  auto read(Address address) -> std::optional<Byte> override;
  auto write(Address address, Byte value) -> bool override;
  auto step(uint32_t master_clocks) -> void override;
  auto irq() const -> bool override { return irq_line_; }
  auto serialize(Archive& ar) -> void override;

  // ---- introspection for tests ----
  auto running() const -> bool { return busy() || !halted_; }
  auto busy() const -> bool { return load_pending_ || copy_.active || ext_.wait != 0; }
  auto accumulator() const -> uint32_t { return a_; }
  auto gpr(int n) const -> uint32_t { return gpr_[static_cast<size_t>(n & 15)]; }
  auto multiplier() const -> uint64_t { return mul_; }
  auto flags() const -> unsigned { return (n_ ? 8u : 0u) | (z_ ? 4u : 0u) | (c_ ? 2u : 0u) | (v_ ? 1u : 0u); }
  auto data_ram() -> std::array<Byte, 3072>& { return dram_; }
  auto clocks_run() const -> uint64_t { return clocks_; }
  // Runs one scheduling unit (instruction, cache fill, DMA or idle clock) and returns its clocks.
  auto run_one() -> uint32_t;

private:
  const std::vector<Byte>& rom_;
  std::vector<Byte>& ram_;
  std::array<uint32_t, 1024> data_rom_{};

  // Program memory: two 256-instruction pages, each loaded from a 512-byte ROM bank.
  struct ProgramPage {
    std::array<uint16_t, 256> code{};
    uint32_t origin = 0;  // bus address it was loaded from
    bool locked = false;  // $7F4C: never replaced
  };
  std::array<ProgramPage, 2> pages_{};
  uint8_t page_ = 0;           // page being executed
  bool load_pending_ = false;  // a page load waits to run
  bool load_forced_ = false;   // ...into page_, without looking for a hit ($7F48)
  uint32_t code_base_ = 0;     // $7F49-$7F4B: bank n lives at base + n * 512
  uint16_t start_bank_ = 0;    // $7F4D-$7F4E
  uint8_t start_pc_ = 0;       // $7F4F

  // Processor registers
  std::array<Byte, 3072> dram_{};
  uint16_t pb_ = 0;   // 15-bit program bank
  uint8_t pc_ = 0;
  bool n_ = false, z_ = false, c_ = false, v_ = false, i_ = false;
  uint32_t a_ = 0;    // 24-bit
  uint16_t p_ = 0;    // 15-bit page register
  uint64_t mul_ = 0;  // 48-bit
  uint32_t mdr_ = 0, rom_buffer_ = 0, ram_buffer_ = 0, mar_ = 0, dpr_ = 0;
  std::array<uint32_t, 16> gpr_{};
  std::array<uint32_t, 8> stack_{};

  // Control
  bool locked_up_ = false;  // after an impossible DMA, until $7F53
  bool halted_ = true;
  bool irq_disable_ = false;
  bool irq_line_ = false;
  uint8_t rom_config_ = 1;
  std::array<Byte, 32> vector_{};
  uint8_t rom_wait_ = 3, ram_wait_ = 3;
  bool sleeping_ = false;     // $7F55-$7F5C suspend
  uint8_t sleep_clocks_ = 0;  // 0 = until $7F5D
  struct BlockCopy { bool active = false; uint32_t from = 0, to = 0; uint16_t count = 0; } copy_;
  struct External {  // a single MAR/MDR byte access in flight
    enum Kind : uint8_t { Idle, Read, Write };
    Kind kind = Idle;
    uint8_t wait = 0;
    uint32_t address = 0;
  } ext_;

  int64_t clock_budget_ = 0;     // master clocks x kClockHz x 11 units
  uint32_t unit_clocks_ = 0;     // clocks charged by the current unit of work
  uint64_t clocks_ = 0;

  // HG51B bus
  auto rom_linear(uint32_t address) const -> std::optional<uint32_t>;
  auto ram_linear(uint32_t address) const -> std::optional<uint32_t>;
  auto bus_read(uint32_t address) -> Byte;
  auto bus_write(uint32_t address, Byte value) -> void;
  auto access_clocks(uint32_t address) const -> uint32_t;
  auto elapse(uint32_t clocks) -> void;
  auto begin_external(External::Kind kind, unsigned reg) -> void;
  auto halt() -> void;
  auto next_pc() -> void;
  auto code_origin() const -> uint32_t;
  auto find_page(uint32_t origin) const -> std::optional<uint8_t>;
  auto load_page(uint8_t index, uint32_t origin) -> void;
  auto ensure_page() -> bool;
  auto block_copy() -> void;
  auto execute() -> void;
  auto status() const -> Byte;
  auto io_read(Word address) -> Byte;
  auto io_write(Word address, Byte value) -> void;
  auto reg_read(unsigned r) -> uint32_t;
  auto reg_write(unsigned r, uint32_t value) -> void;
  auto push() -> void;
  auto pull() -> void;
  auto set_nz(uint32_t value) -> uint32_t;
  auto add(uint32_t x, uint32_t y) -> uint32_t;
  auto sub(uint32_t x, uint32_t y) -> uint32_t;
  auto dram_index(uint32_t address) const -> uint32_t;
};

} // namespace bitforge::coprocessor
