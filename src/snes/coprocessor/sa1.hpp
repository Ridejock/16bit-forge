// SPDX-License-Identifier: MIT
#pragma once
// SA-1: a second 65C816 at 10.74 MHz (the existing CPU core, on its own bus) with 2 KB of I-RAM,
// the cartridge's BW-RAM (battery-backed), the Super MMC ROM banking, normal and character
// conversion DMA, an arithmetic unit, a variable-length bit reader, an H/V timer, and IRQ/NMI
// lines in both directions.
//
// S-CPU map: $2200-$23FF registers, $3000-$37FF I-RAM, $6000-$7FFF BW-RAM window (BMAPS),
// $8000-$FFFF and banks $C0-$FF ROM through the MMC, banks $40-$4F BW-RAM.
// SA-1 map: the same plus I-RAM at $0000-$07FF, BW-RAM in banks $40-$5F and a 2/4bpp "bitmap"
// view of BW-RAM in banks $60-$6F. The SA-1 cannot see WRAM or the PPU.
#include "snes/coprocessor/coprocessor.hpp"
#include "snes/cpu/cpu65c816.hpp"
#include "snes/cpu/cpu_bus.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace bitforge::coprocessor {

class SA1 final : public Coprocessor {
public:
  // `rom` and `bwram` belong to the cartridge and outlive the chip.
  SA1(const std::vector<Byte>& rom, std::vector<Byte>& bwram);

  auto chip() const -> Chip override { return Chip::SA1; }
  auto reset() -> void override;
  auto owns_map() const -> bool override { return true; }
  auto read(Address address) -> std::optional<Byte> override;
  auto write(Address address, Byte value) -> bool override;
  auto step(uint32_t master_clocks) -> void override;
  auto irq() const -> bool override;
  auto serialize(Archive& ar) -> void override;
  auto set_cpu_address(Address address) -> void override { scpu_mar_ = address; }

  // ---- for tests ------------------------------------------------------------------------------
  auto sa1_cpu() -> cpu::CPU65c816& { return cpu_; }
  auto iram() -> std::array<Byte, 2048>& { return iram_; }
  auto sa1_read(Address address) -> Byte { return sa1_bus_read(address); }
  auto sa1_write(Address address, Byte value) -> void { sa1_bus_write(address, value); }
  auto running() const -> bool { return !r_.sa1_rdyb && !r_.sa1_resb; }

  // Registers ($2200-$230E), by name.
  struct Regs {
    // $2200 CCNT (S-CPU): SA-1 IRQ, ready (wait), reset, NMI, message to the SA-1
    bool sa1_irq = false, sa1_rdyb = false, sa1_resb = true, sa1_nmi = false;
    Byte smeg = 0;
    bool cpu_irqen = false, chdma_irqen = false;  // $2201 SIE
    Word crv = 0, cnv = 0, civ = 0;               // $2203-$2208 SA-1 reset/NMI/IRQ vectors
    // $2209 SCNT (SA-1): IRQ to the S-CPU, S-CPU vector switches, message to the S-CPU
    bool cpu_irq = false, cpu_ivsw = false, cpu_nvsw = false;
    Byte cmeg = 0;
    bool sa1_irqen = false, timer_irqen = false, dma_irqen = false, sa1_nmien = false;  // $220A CIE
    Word snv = 0, siv = 0;                        // $220C-$220F S-CPU NMI/IRQ vectors
    bool hvselb = false, ven = false, hen = false;  // $2210 TMC
    Word hcnt = 0, vcnt = 0;                      // $2212-$2215
    std::array<bool, 4> bmode{};                  // $2220-$2223 CXB..FXB bit 7
    std::array<Byte, 4> bank{0, 1, 2, 3};         // $2220-$2223 1 MB ROM bank per segment
    Byte sbm = 0;                                 // $2224 BMAPS
    bool sw46 = false;                            // $2225 BMAP bit 7: bitmap projection
    Byte cbm = 0;
    bool swen = false, cwen = false;              // $2226 SBWE, $2227 CBWE
    Byte bwp = 0x0F;                              // $2228 BWPA
    Byte siwp = 0, ciwp = 0;                      // $2229 SIWP, $222A CIWP
    // $2230 DCNT
    bool dmaen = false, dprio = false, cden = false, cdsel = false, dd = false;
    Byte sd = 0;
    bool chdend = false;                          // $2231 CDMA
    Byte dmasize = 0, dmacb = 0;
    uint32_t dsa = 0, dda = 0;                    // $2232-$2237
    Word dtc = 0;                                 // $2238-$2239
    bool bbf = false;                             // $223F: 2bpp bitmap (else 4bpp)
    std::array<Byte, 16> brf{};                   // $2240-$224F
    bool acm = false, md = false;                 // $2250 MCNT
    Word ma = 0, mb = 0;                          // $2251-$2254
    bool hl = false;                              // $2258 VBD
    Byte vb = 16;
    uint32_t va = 0;                              // $2259-$225B
    Byte vbit = 0;
    // Interrupt flags ($2300 SFR / $2301 CFR)
    bool cpu_irqfl = false, chdma_irqfl = false;
    bool sa1_irqfl = false, timer_irqfl = false, dma_irqfl = false, sa1_nmifl = false;
    Word hcr = 0, vcr = 0;                        // $2302-$2305 latched timer
    uint64_t mr = 0;                              // $2306-$230A 40-bit result
    bool overflow = false;                        // $230B

    auto serialize(Archive& ar) -> void;
  };
  auto regs() const -> const Regs& { return r_; }

private:
  const std::vector<Byte>& rom_;
  std::vector<Byte>& bwram_;
  uint32_t rom_mask_ = 0, bwram_mask_ = 0;  // size - 1 for power-of-two sizes, else 0
  std::array<Byte, 2048> iram_{};
  Regs r_;

  // H/V timer, in master clocks within the line / lines within the frame.
  Word hcounter_ = 0, vcounter_ = 0;
  uint32_t timer_phase_ = 0;  // odd master clock carried to the next advance
  bool nmi_pending_ = false;
  bool cc1_active_ = false;   // type-1 character conversion: S-CPU BW-RAM reads convert
  Byte cc2_line_ = 0;         // type-2: row counter
  Byte sa1_mdr_ = 0;          // SA-1 data bus (open bus value)
  // The S-CPU's last bus address. When both CPUs want the same memory the SA-1 waits: ROM and
  // BW-RAM accesses take twice as long, I-RAM ones three times (as measured by SA-1 timing test ROMs).
  Address scpu_mar_ = 0;
  auto rom_conflict() const -> bool {
    return (scpu_mar_ & 0x408000) == 0x008000 || (scpu_mar_ & 0xC00000) == 0xC00000;
  }
  auto bwram_conflict() const -> bool {
    return (scpu_mar_ & 0x40E000) == 0x006000 || (scpu_mar_ & 0xF00000) == 0x400000;
  }
  auto iram_conflict() const -> bool { return (scpu_mar_ & 0x40F800) == 0x003000; }
  int64_t clock_budget_ = 0;  // master clocks the SA-1 may still run
  uint32_t access_clocks_ = 0;
  int accesses_ = 0;

  // The SA-1 CPU's view of the machine.
  class Bus final : public cpu::CpuBus {
  public:
    explicit Bus(SA1& sa1) : sa1_(sa1) {}
    auto read(Address address) -> Byte override { return sa1_.sa1_bus_read(address); }
    auto write(Address address, Byte value) -> void override { sa1_.sa1_bus_write(address, value); }
    auto poll_nmi() -> bool override { return sa1_.poll_nmi(); }
    auto irq_pending() const -> bool override { return sa1_.irq_pending(); }
    auto read_vector(Word address) -> Word override { return sa1_.read_vector(address); }
  private:
    SA1& sa1_;
  };
  Bus bus_{*this};
  cpu::CPU65c816 cpu_{bus_};  // last: its constructor reads the reset vector through bus_

  auto poll_nmi() -> bool;
  auto irq_pending() const -> bool;
  auto read_vector(Word address) -> Word;

  auto sa1_bus_read(Address address) -> Byte;
  auto sa1_bus_write(Address address, Byte value) -> void;

  // Memory
  auto rom_at(uint32_t linear) const -> Byte;
  auto rom_mmc(Address address) const -> Byte;  // ROM through the Super MMC, either CPU
  auto bwram_at(uint32_t linear) -> Byte&;
  auto bwram_protected(uint32_t linear) const -> bool;
  auto bwram_write(uint32_t linear, Byte value) -> void;  // with write protection
  auto bitmap_read(uint32_t pixel) -> Byte;
  auto bitmap_write(uint32_t pixel, Byte value) -> void;
  auto vbr_read(uint32_t address) -> Byte;  // the bit reader's bus (no I/O)

  // Registers
  auto read_io_cpu(Word address) -> std::optional<Byte>;
  auto read_io_sa1(Word address) -> Byte;
  auto write_io_cpu(Word address, Byte value) -> void;
  auto write_io_sa1(Word address, Byte value) -> void;
  auto write_io_shared(Word address, Byte value) -> void;

  // DMA
  auto dma_normal() -> void;
  auto dma_cc1_start() -> void;
  auto dma_cc1_read(uint32_t address) -> Byte;
  auto dma_cc2() -> void;
  auto store_tile_row(const std::array<Byte, 8>& pixels, unsigned bpp, uint32_t row_base) -> void;

  // Arithmetic unit, run when MB's high byte is written
  auto arithmetic() -> void;

  auto advance_timer(uint32_t clocks) -> void;
  auto timer_tick() -> void;
  auto charge(uint32_t clocks) -> void { access_clocks_ += clocks; accesses_++; }
};

} // namespace bitforge::coprocessor
