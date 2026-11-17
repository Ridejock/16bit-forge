// SPDX-License-Identifier: MIT
#include "snes/coprocessor/sa1.hpp"

#include <algorithm>
#include <bit>

namespace bitforge::coprocessor {

namespace {

constexpr uint32_t kSa1CycleClocks = 2;   // 10.74 MHz: one SA-1 cycle is two master clocks
constexpr uint32_t kBwramClocks = 4;      // BW-RAM runs at half speed
constexpr Word kLineClocks = 1364;
constexpr Word kFrameLines = 262;

constexpr auto pow2_mask(size_t size) -> uint32_t {
  return (size != 0 && std::has_single_bit(size)) ? static_cast<uint32_t>(size - 1) : 0;
}

// Byte offset of bitplane n inside an 8x8 tile row: 0, 1, 16, 17, 32, 33, 48, 49.
constexpr auto plane_offset(unsigned n) -> unsigned { return ((n & 6) << 3) + (n & 1); }

} // namespace

SA1::SA1(const std::vector<Byte>& rom, std::vector<Byte>& bwram)
    : rom_(rom), bwram_(bwram), rom_mask_(pow2_mask(rom.size())), bwram_mask_(pow2_mask(bwram.size())) {
  reset();
}

auto SA1::reset() -> void {
  r_ = Regs{};
  iram_.fill(0);
  hcounter_ = vcounter_ = 0;
  timer_phase_ = 0;
  nmi_pending_ = false;
  cc1_active_ = false;
  cc2_line_ = 0;
  sa1_mdr_ = 0;
  clock_budget_ = 0;
  access_clocks_ = 0;
  accesses_ = 0;
  cpu_.reset();  // held in reset (CCNT bit 5) until the S-CPU releases it
}

// ---- memory ------------------------------------------------------------------------------------

auto SA1::rom_at(uint32_t linear) const -> Byte {
  if (rom_.empty()) return 0;
  return rom_[rom_mask_ ? (linear & rom_mask_) : (linear % rom_.size())];
}

// Super MMC. Banks $00-$3F/$80-$BF:$8000-$FFFF are LoROM and banks $C0-$FF are linear; both are
// split into four 1 MB segments (C, D, E, F) whose ROM bank comes from CXB-FXB. In the LoROM
// area a segment with its bit 7 clear ignores the register and maps its own MB.
auto SA1::rom_mmc(Address address) const -> Byte {
  uint32_t linear = 0;
  bool lorom = false;
  if ((address & 0x408000) == 0x008000) {
    linear = ((address & 0x800000) >> 2) | ((address & 0x3F0000) >> 1) | (address & 0x7FFF);
    lorom = true;
  } else if ((address & 0xC00000) == 0xC00000) {
    linear = address & 0x3FFFFF;
  } else {
    return 0;
  }
  const unsigned segment = (linear >> 20) & 3;
  if (lorom && !r_.bmode[segment]) return rom_at(linear);
  return rom_at((static_cast<uint32_t>(r_.bank[segment]) << 20) | (linear & 0xFFFFF));
}

auto SA1::bwram_at(uint32_t linear) -> Byte& {
  static Byte dummy = 0;
  if (bwram_.empty()) return dummy = 0;
  return bwram_[bwram_mask_ ? (linear & bwram_mask_) : (linear % bwram_.size())];
}

// BWPA protects the first 256 << n bytes, but only while neither CPU has writes enabled.
auto SA1::bwram_protected(uint32_t linear) const -> bool {
  return !r_.swen && !r_.cwen && (linear & 0x3FFFF) < (0x100u << r_.bwp);
}

auto SA1::bwram_write(uint32_t linear, Byte value) -> void {
  if (bwram_.empty() || bwram_protected(linear)) return;
  bwram_at(linear) = value;
}

// Banks $60-$6F: one byte per pixel, packed 2 (4bpp) or 4 (2bpp) pixels per BW-RAM byte.
auto SA1::bitmap_read(uint32_t pixel) -> Byte {
  pixel &= 0xFFFFF;
  if (!r_.bbf) return static_cast<Byte>((bwram_at(pixel >> 1) >> ((pixel & 1) * 4)) & 0x0F);
  return static_cast<Byte>((bwram_at(pixel >> 2) >> ((pixel & 3) * 2)) & 0x03);
}

auto SA1::bitmap_write(uint32_t pixel, Byte value) -> void {
  if (bwram_.empty()) return;
  pixel &= 0xFFFFF;
  if (!r_.bbf) {
    Byte& b = bwram_at(pixel >> 1);
    const unsigned shift = (pixel & 1) * 4;
    b = static_cast<Byte>((b & ~(0x0F << shift)) | ((value & 0x0F) << shift));
  } else {
    Byte& b = bwram_at(pixel >> 2);
    const unsigned shift = (pixel & 3) * 2;
    b = static_cast<Byte>((b & ~(0x03 << shift)) | ((value & 0x03) << shift));
  }
}

// ---- S-CPU side --------------------------------------------------------------------------------

auto SA1::read(Address address) -> std::optional<Byte> {
  const Byte bank = static_cast<Byte>(address >> 16);
  const Word offset = static_cast<Word>(address);
  if ((bank & 0x40) == 0) {  // $00-$3F, $80-$BF
    if (offset >= 0x8000) {
      // The SA-1 can redirect the S-CPU's NMI and IRQ vectors (SCNT NVSW/IVSW).
      if (bank == 0x00 || bank == 0x80) {
        if (r_.cpu_nvsw && (offset == 0xFFEA || offset == 0xFFEB)) return static_cast<Byte>(r_.snv >> ((offset & 1) * 8));
        if (r_.cpu_ivsw && (offset == 0xFFEE || offset == 0xFFEF)) return static_cast<Byte>(r_.siv >> ((offset & 1) * 8));
      }
      return rom_mmc(address);
    }
    if (offset >= 0x2200 && offset < 0x2400) return read_io_cpu(offset);
    if (offset >= 0x3000 && offset < 0x3800) return iram_[offset & 0x7FF];
    if (offset >= 0x6000) {
      if (bwram_.empty()) return std::nullopt;
      const uint32_t linear = static_cast<uint32_t>(r_.sbm & 0x1F) * 0x2000 + (offset & 0x1FFF);
      return cc1_active_ ? dma_cc1_read(linear) : bwram_at(linear);
    }
    return std::nullopt;
  }
  if ((bank & 0xC0) == 0xC0) return rom_mmc(address);
  if ((bank & 0xF0) == 0x40) {  // BW-RAM, linear
    if (bwram_.empty()) return std::nullopt;
    const uint32_t linear = address & 0x0FFFFF;
    return cc1_active_ ? dma_cc1_read(linear) : bwram_at(linear);
  }
  return std::nullopt;
}

auto SA1::write(Address address, Byte value) -> bool {
  const Byte bank = static_cast<Byte>(address >> 16);
  const Word offset = static_cast<Word>(address);
  if ((bank & 0x40) == 0) {
    if (offset >= 0x8000) return true;  // ROM
    if (offset >= 0x2200 && offset < 0x2400) {
      write_io_cpu(offset, value);
      return true;
    }
    if (offset >= 0x3000 && offset < 0x3800) {
      if (r_.siwp & (1u << ((offset >> 8) & 7))) iram_[offset & 0x7FF] = value;
      return true;
    }
    if (offset >= 0x6000) {
      bwram_write(static_cast<uint32_t>(r_.sbm & 0x1F) * 0x2000 + (offset & 0x1FFF), value);
      return true;
    }
    return false;
  }
  if ((bank & 0xF0) == 0x40) {
    bwram_write(address & 0x0FFFFF, value);
    return true;
  }
  return (bank & 0xC0) == 0xC0;
}

auto SA1::irq() const -> bool {
  return (r_.cpu_irqen && r_.cpu_irqfl) || (r_.chdma_irqen && r_.chdma_irqfl);
}

// ---- SA-1 side ---------------------------------------------------------------------------------

auto SA1::sa1_bus_read(Address address) -> Byte {
  address &= 0xFFFFFF;
  if ((address & 0x40FE00) == 0x002200) {  // $2200-$23FF
    charge(kSa1CycleClocks);
    return sa1_mdr_ = read_io_sa1(static_cast<Word>(address));
  }
  if ((address & 0x408000) == 0x008000 || (address & 0xC00000) == 0xC00000) {
    charge(rom_conflict() ? 2 * kSa1CycleClocks : kSa1CycleClocks);
    return sa1_mdr_ = rom_mmc(address);
  }
  if ((address & 0x40E000) == 0x006000) {  // $6000-$7FFF window
    charge(bwram_conflict() ? 2 * kBwramClocks : kBwramClocks);
    if (bwram_.empty()) return sa1_mdr_;
    if (!r_.sw46) return sa1_mdr_ = bwram_at(static_cast<uint32_t>(r_.cbm & 0x1F) * 0x2000 + (address & 0x1FFF));
    return sa1_mdr_ = bitmap_read(static_cast<uint32_t>(r_.cbm) * 0x2000 + (address & 0x1FFF));
  }
  if ((address & 0xE00000) == 0x400000) {  // $40-$5F
    charge(bwram_conflict() ? 2 * kBwramClocks : kBwramClocks);
    if (bwram_.empty()) return sa1_mdr_;
    return sa1_mdr_ = bwram_at(address & 0x1FFFFF);
  }
  if ((address & 0xF00000) == 0x600000) {  // $60-$6F bitmap
    charge(bwram_conflict() ? 2 * kBwramClocks : kBwramClocks);
    if (bwram_.empty()) return sa1_mdr_;
    return sa1_mdr_ = bitmap_read(address);
  }
  if ((address & 0x40F800) == 0x000000 || (address & 0x40F800) == 0x003000) {
    charge(iram_conflict() ? 3 * kSa1CycleClocks : kSa1CycleClocks);
    return sa1_mdr_ = iram_[address & 0x7FF];
  }
  charge(kSa1CycleClocks);
  return sa1_mdr_;  // open bus: the last value on the SA-1 bus
}

auto SA1::sa1_bus_write(Address address, Byte value) -> void {
  address &= 0xFFFFFF;
  sa1_mdr_ = value;
  if ((address & 0x40FE00) == 0x002200) {
    charge(kSa1CycleClocks);
    write_io_sa1(static_cast<Word>(address), value);
    return;
  }
  if ((address & 0x408000) == 0x008000 || (address & 0xC00000) == 0xC00000) {
    charge(rom_conflict() ? 2 * kSa1CycleClocks : kSa1CycleClocks);
    return;
  }
  if ((address & 0x40E000) == 0x006000) {
    charge(bwram_conflict() ? 2 * kBwramClocks : kBwramClocks);
    if (!r_.sw46) bwram_write(static_cast<uint32_t>(r_.cbm & 0x1F) * 0x2000 + (address & 0x1FFF), value);
    else bitmap_write(static_cast<uint32_t>(r_.cbm) * 0x2000 + (address & 0x1FFF), value);
    return;
  }
  if ((address & 0xE00000) == 0x400000) {
    charge(bwram_conflict() ? 2 * kBwramClocks : kBwramClocks);
    bwram_write(address & 0x1FFFFF, value);
    return;
  }
  if ((address & 0xF00000) == 0x600000) {
    charge(bwram_conflict() ? 2 * kBwramClocks : kBwramClocks);
    bitmap_write(address, value);
    return;
  }
  if ((address & 0x40F800) == 0x000000 || (address & 0x40F800) == 0x003000) {
    charge(iram_conflict() ? 3 * kSa1CycleClocks : kSa1CycleClocks);
    if (r_.ciwp & (1u << ((address >> 8) & 7))) iram_[address & 0x7FF] = value;
    return;
  }
  charge(kSa1CycleClocks);
}

auto SA1::vbr_read(uint32_t address) -> Byte {
  address &= 0xFFFFFF;
  if ((address & 0x408000) == 0x008000 || (address & 0xC00000) == 0xC00000) return rom_mmc(address);
  if ((address & 0x40E000) == 0x006000 || (address & 0xF00000) == 0x400000) return bwram_at(address & 0x0FFFFF);
  if ((address & 0x40F800) == 0x000000 || (address & 0x40F800) == 0x003000) return iram_[address & 0x7FF];
  return 0xFF;
}

auto SA1::poll_nmi() -> bool {
  if (!nmi_pending_) return false;
  nmi_pending_ = false;
  return true;
}

auto SA1::irq_pending() const -> bool {
  return (r_.sa1_irqen && r_.sa1_irqfl) || (r_.timer_irqen && r_.timer_irqfl) || (r_.dma_irqen && r_.dma_irqfl);
}

// The SA-1 takes its reset, NMI and IRQ vectors from CRV/CNV/CIV, not from ROM.
auto SA1::read_vector(Word address) -> Word {
  switch (address) {
    case 0xFFFC: return r_.crv;
    case 0xFFEA: case 0xFFFA: return r_.cnv;
    case 0xFFEE: case 0xFFFE: return r_.civ;
    default: {
      const Byte low = sa1_bus_read(address);
      return static_cast<Word>(low | (sa1_bus_read(static_cast<Word>(address + 1)) << 8));
    }
  }
}

// ---- timing ------------------------------------------------------------------------------------

auto SA1::step(uint32_t master_clocks) -> void {
  clock_budget_ += master_clocks;
  while (clock_budget_ > 0) {
    // Asleep (held in reset or told to wait), or waiting for an interrupt with none coming:
    // only the timer runs.
    const bool idle = !running() || cpu_.stopped() ||
                      (cpu_.waiting() && !nmi_pending_ && !irq_pending());
    if (idle) {
      advance_timer(static_cast<uint32_t>(clock_budget_));
      clock_budget_ = 0;
      break;
    }
    access_clocks_ = 0;
    accesses_ = 0;
    const int cycles = cpu_.step();
    const uint32_t clocks = access_clocks_ + static_cast<uint32_t>(std::max(0, cycles - accesses_)) * kSa1CycleClocks;
    clock_budget_ -= clocks;
    advance_timer(clocks);
  }
}

auto SA1::timer_tick() -> void {
  if (!r_.hvselb) {  // H/V counters, like the PPU's
    hcounter_ = static_cast<Word>(hcounter_ + 2);
    if (hcounter_ >= kLineClocks) {
      hcounter_ = 0;
      if (++vcounter_ >= kFrameLines) vcounter_ = 0;
    }
  } else {  // one 18-bit linear counter
    hcounter_ = static_cast<Word>(hcounter_ + 2);
    vcounter_ = static_cast<Word>((vcounter_ + (hcounter_ >> 11)) & 0x1FF);
    hcounter_ &= 0x07FF;
  }
  bool fire = false;
  switch ((r_.ven ? 2 : 0) | (r_.hen ? 1 : 0)) {
    case 1: fire = hcounter_ == (r_.hcnt << 2); break;
    case 2: fire = vcounter_ == r_.vcnt && hcounter_ == 0; break;
    case 3: fire = vcounter_ == r_.vcnt && hcounter_ == (r_.hcnt << 2); break;
    default: break;
  }
  if (fire) r_.timer_irqfl = true;
}

auto SA1::advance_timer(uint32_t clocks) -> void {
  timer_phase_ += clocks;
  uint32_t ticks = timer_phase_ / 2;
  timer_phase_ &= 1;
  if (r_.hen || r_.ven) {
    while (ticks--) timer_tick();
    return;
  }
  // No timer IRQ armed: jump straight there.
  if (!r_.hvselb) {
    const uint64_t total = static_cast<uint64_t>(vcounter_) * kLineClocks + hcounter_ + 2ull * ticks;
    hcounter_ = static_cast<Word>(total % kLineClocks);
    vcounter_ = static_cast<Word>((total / kLineClocks) % kFrameLines);
  } else {
    const uint32_t total = ((static_cast<uint32_t>(vcounter_) << 11) | hcounter_) + 2 * ticks;
    hcounter_ = static_cast<Word>(total & 0x7FF);
    vcounter_ = static_cast<Word>((total >> 11) & 0x1FF);
  }
}

// ---- registers ---------------------------------------------------------------------------------

auto SA1::read_io_cpu(Word address) -> std::optional<Byte> {
  if ((0x2200 | (address & 0x1FF)) == 0x2300) {  // SFR
    return static_cast<Byte>(r_.cpu_irqfl << 7 | r_.cpu_ivsw << 6 | r_.chdma_irqfl << 5 | r_.cpu_nvsw << 4 | r_.cmeg);
  }
  return std::nullopt;  // write-only or SA-1-only: open bus
}

auto SA1::read_io_sa1(Word address) -> Byte {
  auto vbr_window = [this] {
    const uint32_t data = vbr_read(r_.va) | (vbr_read(r_.va + 1) << 8) | (vbr_read(r_.va + 2) << 16);
    return data >> r_.vbit;
  };
  switch (0x2200 | (address & 0x1FF)) {
    case 0x2301:  // CFR
      return static_cast<Byte>(r_.sa1_irqfl << 7 | r_.timer_irqfl << 6 | r_.dma_irqfl << 5 | r_.sa1_nmifl << 4 | r_.smeg);
    case 0x2302:  // HCR: reading the low byte latches both counters
      r_.hcr = static_cast<Word>(hcounter_ >> 2);
      r_.vcr = vcounter_;
      return static_cast<Byte>(r_.hcr);
    case 0x2303: return static_cast<Byte>(r_.hcr >> 8);
    case 0x2304: return static_cast<Byte>(r_.vcr);
    case 0x2305: return static_cast<Byte>(r_.vcr >> 8);
    case 0x2306: return static_cast<Byte>(r_.mr);
    case 0x2307: return static_cast<Byte>(r_.mr >> 8);
    case 0x2308: return static_cast<Byte>(r_.mr >> 16);
    case 0x2309: return static_cast<Byte>(r_.mr >> 24);
    case 0x230A: return static_cast<Byte>(r_.mr >> 32);
    case 0x230B: return static_cast<Byte>(r_.overflow << 7);
    case 0x230C: return static_cast<Byte>(vbr_window());
    case 0x230D: {
      const Byte value = static_cast<Byte>(vbr_window() >> 8);
      if (r_.hl) {  // auto-increment: reading the high byte advances by VB bits
        r_.vbit = static_cast<Byte>(r_.vbit + r_.vb);
        r_.va += r_.vbit >> 3;
        r_.vbit &= 7;
      }
      return value;
    }
    default: return sa1_mdr_;
  }
}

auto SA1::write_io_cpu(Word address, Byte value) -> void {
  switch (0x2200 | (address & 0x1FF)) {
    case 0x2200: {  // CCNT
      const bool releasing = r_.sa1_resb && !(value & 0x20);
      r_.sa1_irq = value & 0x80;
      r_.sa1_rdyb = value & 0x40;
      r_.sa1_resb = value & 0x20;
      r_.sa1_nmi = value & 0x10;
      r_.smeg = value & 0x0F;
      if (releasing) {
        cpu_.reset_signal();  // PC from CRV
        r_.ciwp = 0;
        clock_budget_ = 0;
      }
      if (r_.sa1_irq) r_.sa1_irqfl = true;
      if (r_.sa1_nmi) {
        r_.sa1_nmifl = true;
        if (r_.sa1_nmien) nmi_pending_ = true;
      }
      break;
    }
    case 0x2201:  // SIE
      r_.cpu_irqen = value & 0x80;
      r_.chdma_irqen = value & 0x20;
      break;
    case 0x2202:  // SIC
      if (value & 0x80) r_.cpu_irqfl = false;
      if (value & 0x20) r_.chdma_irqfl = false;
      break;
    case 0x2203: r_.crv = static_cast<Word>((r_.crv & 0xFF00) | value); break;
    case 0x2204: r_.crv = static_cast<Word>((value << 8) | (r_.crv & 0xFF)); break;
    case 0x2205: r_.cnv = static_cast<Word>((r_.cnv & 0xFF00) | value); break;
    case 0x2206: r_.cnv = static_cast<Word>((value << 8) | (r_.cnv & 0xFF)); break;
    case 0x2207: r_.civ = static_cast<Word>((r_.civ & 0xFF00) | value); break;
    case 0x2208: r_.civ = static_cast<Word>((value << 8) | (r_.civ & 0xFF)); break;
    case 0x2220: case 0x2221: case 0x2222: case 0x2223: {  // CXB-FXB
      const size_t segment = (address & 0x1FF) - 0x20;
      r_.bmode[segment] = value & 0x80;
      r_.bank[segment] = value & 0x07;
      break;
    }
    case 0x2224: r_.sbm = value & 0x1F; break;
    case 0x2226: r_.swen = value & 0x80; break;
    case 0x2228: r_.bwp = value & 0x0F; break;
    case 0x2229: r_.siwp = value; break;
    case 0x2231: case 0x2232: case 0x2233: case 0x2234: case 0x2235: case 0x2236: case 0x2237:
      write_io_shared(address, value);
      break;
    default: break;
  }
}

auto SA1::write_io_sa1(Word address, Byte value) -> void {
  switch (0x2200 | (address & 0x1FF)) {
    case 0x2209:  // SCNT
      r_.cpu_irq = value & 0x80;
      r_.cpu_ivsw = value & 0x40;
      r_.cpu_nvsw = value & 0x10;
      r_.cmeg = value & 0x0F;
      if (r_.cpu_irq) r_.cpu_irqfl = true;
      break;
    case 0x220A: {  // CIE
      const bool nmi_was_off = !r_.sa1_nmien;
      r_.sa1_irqen = value & 0x80;
      r_.timer_irqen = value & 0x40;
      r_.dma_irqen = value & 0x20;
      r_.sa1_nmien = value & 0x10;
      if (nmi_was_off && r_.sa1_nmien && r_.sa1_nmifl) nmi_pending_ = true;
      break;
    }
    case 0x220B:  // CIC
      if (value & 0x80) r_.sa1_irqfl = false;
      if (value & 0x40) r_.timer_irqfl = false;
      if (value & 0x20) r_.dma_irqfl = false;
      if (value & 0x10) r_.sa1_nmifl = false;
      break;
    case 0x220C: r_.snv = static_cast<Word>((r_.snv & 0xFF00) | value); break;
    case 0x220D: r_.snv = static_cast<Word>((value << 8) | (r_.snv & 0xFF)); break;
    case 0x220E: r_.siv = static_cast<Word>((r_.siv & 0xFF00) | value); break;
    case 0x220F: r_.siv = static_cast<Word>((value << 8) | (r_.siv & 0xFF)); break;
    case 0x2210:  // TMC
      r_.hvselb = value & 0x80;
      r_.ven = value & 0x02;
      r_.hen = value & 0x01;
      break;
    case 0x2211: hcounter_ = vcounter_ = 0; break;  // CTR: restart the timer
    case 0x2212: r_.hcnt = static_cast<Word>((r_.hcnt & 0xFF00) | value); break;
    case 0x2213: r_.hcnt = static_cast<Word>((r_.hcnt & 0x00FF) | (value << 8)); break;
    case 0x2214: r_.vcnt = static_cast<Word>((r_.vcnt & 0xFF00) | value); break;
    case 0x2215: r_.vcnt = static_cast<Word>((r_.vcnt & 0x00FF) | (value << 8)); break;
    case 0x2225: r_.sw46 = value & 0x80; r_.cbm = value & 0x7F; break;  // BMAP
    case 0x2227: r_.cwen = value & 0x80; break;
    case 0x222A: r_.ciwp = value; break;
    case 0x2230:  // DCNT
      r_.dmaen = value & 0x80;
      r_.dprio = value & 0x40;
      r_.cden = value & 0x20;
      r_.cdsel = value & 0x10;
      r_.dd = value & 0x04;
      r_.sd = value & 0x03;
      if (!r_.dmaen) cc2_line_ = 0;
      break;
    case 0x2231: case 0x2232: case 0x2233: case 0x2234: case 0x2235: case 0x2236: case 0x2237:
      write_io_shared(address, value);
      break;
    case 0x2238: r_.dtc = static_cast<Word>((r_.dtc & 0xFF00) | value); break;
    case 0x2239: r_.dtc = static_cast<Word>((r_.dtc & 0x00FF) | (value << 8)); break;
    case 0x223F: r_.bbf = value & 0x80; break;
    case 0x2250:  // MCNT
      r_.acm = value & 0x02;
      r_.md = value & 0x01;
      if (r_.acm) r_.mr = 0;
      break;
    case 0x2251: r_.ma = static_cast<Word>((r_.ma & 0xFF00) | value); break;
    case 0x2252: r_.ma = static_cast<Word>((r_.ma & 0x00FF) | (value << 8)); break;
    case 0x2253: r_.mb = static_cast<Word>((r_.mb & 0xFF00) | value); break;
    case 0x2254:
      r_.mb = static_cast<Word>((r_.mb & 0x00FF) | (value << 8));
      arithmetic();
      break;
    case 0x2258:  // VBD
      r_.hl = value & 0x80;
      r_.vb = value & 0x0F;
      if (r_.vb == 0) r_.vb = 16;
      if (!r_.hl) {  // fixed mode: writing VBD advances
        r_.vbit = static_cast<Byte>(r_.vbit + r_.vb);
        r_.va += r_.vbit >> 3;
        r_.vbit &= 7;
      }
      break;
    case 0x2259: r_.va = (r_.va & 0xFFFF00) | value; break;
    case 0x225A: r_.va = (r_.va & 0xFF00FF) | (static_cast<uint32_t>(value) << 8); break;
    case 0x225B: r_.va = (r_.va & 0x00FFFF) | (static_cast<uint32_t>(value) << 16); r_.vbit = 0; break;
    default:
      if ((address & 0x1FF) >= 0x40 && (address & 0x1FF) < 0x50) {  // BRF: type-2 conversion
        const size_t index = (address & 0x1FF) - 0x40;
        r_.brf[index] = value;
        if ((index & 7) == 7 && r_.dmaen && r_.cden && !r_.cdsel) dma_cc2();
      }
      break;
  }
}

// Writable by both CPUs: the DMA parameters. Writing the destination starts the transfer.
auto SA1::write_io_shared(Word address, Byte value) -> void {
  switch (0x2200 | (address & 0x1FF)) {
    case 0x2231:  // CDMA
      r_.chdend = value & 0x80;
      r_.dmasize = std::min<Byte>(static_cast<Byte>((value >> 2) & 7), 5);
      r_.dmacb = std::min<Byte>(static_cast<Byte>(value & 3), 2);
      if (r_.chdend) cc1_active_ = false;
      break;
    case 0x2232: r_.dsa = (r_.dsa & 0xFFFF00) | value; break;
    case 0x2233: r_.dsa = (r_.dsa & 0xFF00FF) | (static_cast<uint32_t>(value) << 8); break;
    case 0x2234: r_.dsa = (r_.dsa & 0x00FFFF) | (static_cast<uint32_t>(value) << 16); break;
    case 0x2235: r_.dda = (r_.dda & 0xFFFF00) | value; break;
    case 0x2236:
      r_.dda = (r_.dda & 0xFF00FF) | (static_cast<uint32_t>(value) << 8);
      if (r_.dmaen) {
        if (!r_.cden && !r_.dd) dma_normal();          // to I-RAM
        else if (r_.cden && r_.cdsel) dma_cc1_start();  // type-1 character conversion
      }
      break;
    case 0x2237:
      r_.dda = (r_.dda & 0x00FFFF) | (static_cast<uint32_t>(value) << 16);
      if (r_.dmaen && !r_.cden && r_.dd) dma_normal();  // to BW-RAM
      break;
    default: break;
  }
}

// ---- arithmetic --------------------------------------------------------------------------------

auto SA1::arithmetic() -> void {
  const auto ma = static_cast<int16_t>(r_.ma);
  if (r_.acm) {  // cumulative sum: MR += MA * MB (40 bits), OF on carry out
    r_.mr += static_cast<uint64_t>(static_cast<int64_t>(ma) * static_cast<int16_t>(r_.mb));
    r_.overflow = ((r_.mr >> 40) & 1) != 0;
    r_.mr &= 0xFF'FFFF'FFFFull;
    r_.mb = 0;
  } else if (!r_.md) {  // signed 16x16 -> 32
    r_.mr = static_cast<uint32_t>(static_cast<int32_t>(ma) * static_cast<int16_t>(r_.mb));
    r_.mb = 0;
  } else {
    // Signed dividend / unsigned divisor. Measured on hardware (sneslab, sa1-multest): the
    // magnitude is divided, the quotient takes the dividend's sign and the remainder stays
    // positive. Dividing by zero gives a quotient of -1 (+1 for negative dividends) and |MA|.
    const Word divisor = r_.mb;
    const uint32_t magnitude = static_cast<uint32_t>(ma < 0 ? -static_cast<int32_t>(ma) : ma);
    Word quotient = 0, remainder = 0;
    if (divisor == 0) {
      quotient = ma < 0 ? 0x0001 : 0xFFFF;
      remainder = static_cast<Word>(magnitude);
    } else {
      const auto q = static_cast<int32_t>(magnitude / divisor);
      quotient = static_cast<Word>(ma < 0 ? -q : q);
      remainder = static_cast<Word>(magnitude % divisor);
    }
    r_.mr = (static_cast<uint32_t>(remainder) << 16) | quotient;
    r_.ma = 0;
    r_.mb = 0;
  }
}

// ---- DMA ---------------------------------------------------------------------------------------

auto SA1::dma_normal() -> void {
  enum Source : Byte { kRom = 0, kBwram = 1, kIram = 2 };
  while (r_.dtc != 0) {
    r_.dtc--;
    const uint32_t source = r_.dsa & 0xFFFFFF;
    const uint32_t target = r_.dda & 0xFFFFFF;
    r_.dsa = (r_.dsa + 1) & 0xFFFFFF;
    r_.dda = (r_.dda + 1) & 0xFFFFFF;
    Byte data = sa1_mdr_;
    switch (r_.sd) {
      case kRom: data = rom_mmc(source); break;
      case kBwram: if (!r_.dd) data = bwram_at(source & 0x0FFFFF); break;  // BW-RAM -> I-RAM only
      case kIram: if (r_.dd) data = iram_[source & 0x7FF]; break;          // I-RAM -> BW-RAM only
      default: break;
    }
    if (r_.dd) {
      if (!bwram_.empty()) bwram_at(target & 0x0FFFFF) = data;
    } else {
      iram_[target & 0x7FF] = data;
    }
  }
  r_.dma_irqfl = true;
}

auto SA1::dma_cc1_start() -> void {
  cc1_active_ = true;
  r_.chdma_irqfl = true;  // tells the S-CPU to start its DMA from BW-RAM
}

// Character conversion writes one 8-pixel tile row into I-RAM as SNES bitplanes: plane p of the
// row goes to row_base + plane_offset(p) (planes interleave in pairs, 16 bytes per pair).
auto SA1::store_tile_row(const std::array<Byte, 8>& pixels, unsigned bpp, uint32_t row_base) -> void {
  for (unsigned plane = 0; plane < bpp; plane++) {
    unsigned bits = 0;
    for (const Byte pixel : pixels) bits = (bits << 1) | ((pixel >> plane) & 1u);  // leftmost pixel in bit 7
    iram_[(row_base + plane_offset(plane)) & 0x7FF] = static_cast<Byte>(bits);
  }
}

// Type 1: the S-CPU DMAs "tiles" out of BW-RAM. Whenever it reaches the start of a tile, the
// SA-1 takes the matching 8x8 block of the packed bitmap at DSA (a bitmap 8 << DMASIZE pixels
// wide), converts it to bitplanes in I-RAM at DDA, and the S-CPU is fed those bytes instead.
auto SA1::dma_cc1_read(uint32_t address) -> Byte {
  const unsigned bpp = 8u >> r_.dmacb;          // 8, 4 or 2 bits per pixel
  const uint32_t tile_bytes = 8u * bpp;         // 64, 32 or 16
  if (address % tile_bytes == 0) {
    const uint32_t tiles_per_line = 1u << r_.dmasize;
    const uint32_t row_bytes = tiles_per_line * bpp;  // one pixel row of the whole bitmap
    const uint32_t bw_mask = bwram_mask_ ? bwram_mask_ : static_cast<uint32_t>(bwram_.size() - 1);
    const uint32_t tile = ((address - r_.dsa) & bw_mask) / tile_bytes;
    const uint32_t column = tile % tiles_per_line;
    const uint32_t line = tile / tiles_per_line;
    const uint32_t first = r_.dsa + line * 8 * row_bytes + column * bpp;
    for (unsigned y = 0; y < 8; y++) {
      uint64_t packed = 0;  // bpp bytes, pixel x in bits [x * bpp, x * bpp + bpp)
      for (unsigned i = 0; i < bpp; i++) {
        packed |= static_cast<uint64_t>(bwram_at((first + y * row_bytes + i) & bw_mask)) << (8 * i);
      }
      std::array<Byte, 8> pixels{};
      for (unsigned x = 0; x < 8; x++) pixels[x] = static_cast<Byte>((packed >> (x * bpp)) & ((1u << bpp) - 1));
      store_tile_row(pixels, bpp, r_.dda + y * 2);
    }
  }
  return iram_[(r_.dda + address % tile_bytes) & 0x7FF];
}

// Type 2: the SA-1 writes one row of 8 pixels (one byte each) to BRF; every completed row is
// converted into I-RAM at once. BRF has two 8-byte halves used alternately, and rows fill two
// tiles at DDA (16 rows) before wrapping.
auto SA1::dma_cc2() -> void {
  const unsigned bpp = 8u >> r_.dmacb;
  const uint32_t tile_bytes = 8u * bpp;
  std::array<Byte, 8> pixels{};
  std::copy_n(r_.brf.begin() + (cc2_line_ & 1) * 8, 8, pixels.begin());
  const uint32_t pair_base = (r_.dda & 0x7FF) & ~(2 * tile_bytes - 1);
  const uint32_t tile = (cc2_line_ >> 3) & 1;
  store_tile_row(pixels, bpp, pair_base + tile * tile_bytes + (cc2_line_ & 7) * 2);
  cc2_line_ = static_cast<Byte>((cc2_line_ + 1) & 15);
}

// ---- save state --------------------------------------------------------------------------------

auto SA1::Regs::serialize(Archive& ar) -> void {
  ar(sa1_irq, sa1_rdyb, sa1_resb, sa1_nmi, smeg, cpu_irqen, chdma_irqen, crv, cnv, civ);
  ar(cpu_irq, cpu_ivsw, cpu_nvsw, cmeg, sa1_irqen, timer_irqen, dma_irqen, sa1_nmien, snv, siv);
  ar(hvselb, ven, hen, hcnt, vcnt, bmode, bank, sbm, sw46, cbm, swen, cwen, bwp, siwp, ciwp);
  ar(dmaen, dprio, cden, cdsel, dd, sd, chdend, dmasize, dmacb, dsa, dda, dtc, bbf, brf);
  ar(acm, md, ma, mb, hl, vb, va, vbit, cpu_irqfl, chdma_irqfl, sa1_irqfl, timer_irqfl, dma_irqfl, sa1_nmifl);
  ar(hcr, vcr, mr, overflow);
}

auto SA1::serialize(Archive& ar) -> void {
  r_.serialize(ar);
  ar(iram_, hcounter_, vcounter_, timer_phase_, nmi_pending_, cc1_active_, cc2_line_, sa1_mdr_, clock_budget_);
  cpu_.serialize(ar);
}

} // namespace bitforge::coprocessor
