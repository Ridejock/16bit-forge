// SPDX-License-Identifier: MIT
#include "snes/memory/bus.hpp"
#include "snes/cheats.hpp"
#include "snes/msu1.hpp"
#include "snes/memory/wram.hpp"
#include "snes/memory/cartridge.hpp"
#include "snes/ppu/ppu.hpp"
#include "snes/apu/apu.hpp"
#include "utils/logging.hpp"
#include <algorithm>
#include <cstdio>
#include <memory>

namespace bitforge::memory {

namespace {

// Access speed of the system area ($00-$3F and $80-$BF below $8000), by offset range.
struct SpeedRange { Word end; Byte clocks; };
constexpr std::array<SpeedRange, 5> kSystemAreaSpeeds = {{
    {0x2000, 8},   // WRAM mirror
    {0x4000, 6},   // B-bus: PPU, APU, WRAM port
    {0x4200, 12},  // joypad serial ports
    {0x6000, 6},   // CPU I/O and DMA registers
    {0x8000, 8},   // expansion
}};
constexpr int kFastClocks = 6;
constexpr int kSlowClocks = 8;
constexpr int kInternalCycleClocks = 6;  // a CPU cycle with no bus access
constexpr int kLastDot = 339;            // H counter runs 0-339

// Auto-joypad read starts about 33 dots into the first V-blank line and takes 4224 clocks.
constexpr int kAutoJoypadStart = 130;
constexpr int kAutoJoypadClocks = 4224;

} // namespace

Bus::Bus() 
  : wram(std::make_unique<WRAM>())
  , cartridge(std::make_unique<Cartridge>()) {
}

Bus::~Bus() = default;

auto Bus::read(Address address) -> Byte {
  mar = address;
  if (!dma_active) {
    pending_clocks += static_cast<uint64_t>(access_clocks(address));
    cpu_accesses++;
  }

  if (cheats && cheats->active()) return open_bus = cheats->read(address, read_mapped(address));
  return read_mapped(address);
}

// The byte the bus carries for `address` (no cheats), updating open bus.
auto Bus::read_mapped(Address address) -> Byte {
  const Word offset = address & 0xFFFF;
  switch (decode_address(address)) {
    case MemoryRegion::WRAM:
      return open_bus = wram->read(wram_offset(address));

    case MemoryRegion::WRAM_PORT:
      if (offset == 0x2180) {
        open_bus = wram->read(wmadd);
        wmadd = (wmadd + 1) & 0x1FFFF;
      }
      return open_bus;

    case MemoryRegion::CARTRIDGE:
      if (auto value = cartridge->read(address)) open_bus = *value;
      return open_bus;

    case MemoryRegion::PPU_REGISTERS:
      if (ppu) open_bus = ppu->read_register(static_cast<Byte>(address & 0x3F));
      return open_bus;

    case MemoryRegion::CPU_IO:
      return open_bus = read_cpu_io(offset);

    case MemoryRegion::APU_REGISTERS:
      // Inside a CPU instruction the SMP is synced to this access's master clock (see APU).
      if (apu) open_bus = in_cpu_step ? apu->cpu_read(address & 0x3, pending_clocks) : apu->read_register(address & 0x3);
      return open_bus;

    case MemoryRegion::JOYPAD:
      return open_bus = static_cast<Byte>((open_bus & 0xFC) | read_joypad_port(offset));

    case MemoryRegion::DMA:
      return open_bus = read_dma_register(((offset - 0x4300) >> 4) & 0x07, offset & 0x0F);

    case MemoryRegion::UNMAPPED:
      if (msu1 && (offset & 0xFFF8) == 0x2000) return open_bus = msu1->read(offset);
      // $2000-$5FFF gaps: coprocessor registers (SA-1 $2200, Super FX $3000) when there is one.
      if (auto value = cartridge->read(address)) open_bus = *value;
      break;
  }
  return open_bus;
}

auto Bus::write(Address address, Byte value) -> void {
  const Word offset = address & 0xFFFF;
  mar = address;
  if (!dma_active) {
    pending_clocks += static_cast<uint64_t>(access_clocks(address));
    cpu_accesses++;
  }

  switch (decode_address(address)) {
    case MemoryRegion::WRAM:
      wram->write(wram_offset(address), value);
      break;

    case MemoryRegion::WRAM_PORT:
      switch (offset) {
        case 0x2180:
          wram->write(wmadd, value);
          wmadd = (wmadd + 1) & 0x1FFFF;
          break;
        case 0x2181: wmadd = (wmadd & 0x1FF00) | value; break;
        case 0x2182: wmadd = (wmadd & 0x100FF) | (static_cast<Address>(value) << 8); break;
        case 0x2183: wmadd = (wmadd & 0x0FFFF) | (static_cast<Address>(value & 0x01) << 16); break;
        default: break;
      }
      break;

    case MemoryRegion::CARTRIDGE:
      cartridge->write(address, value);
      break;

    case MemoryRegion::PPU_REGISTERS:
      if (ppu) ppu->write_register(static_cast<Byte>(address & 0x3F), value);
      break;

    case MemoryRegion::CPU_IO:
      write_cpu_io(offset, value);
      break;

    case MemoryRegion::APU_REGISTERS:
      if (apu) {
        if (in_cpu_step) apu->cpu_write(address & 0x3, value, pending_clocks);
        else apu->write_register(address & 0x3, value);
      }
      break;

    case MemoryRegion::JOYPAD:
      // $4016 bit 0 is the latch line to both ports: while high the pads reload their shift
      // registers continuously. $4017 writes go nowhere.
      if (offset == 0x4016) {
        joypad_strobe = (value & 0x01) != 0;
        for (auto& port : ports) port.latch(joypad_strobe);
      }
      break;

    case MemoryRegion::DMA:
      write_dma_register(((offset - 0x4300) >> 4) & 0x07, offset & 0x0F, value);
      if (auto* chip = cartridge->coprocessor()) {
        chip->dma_register_write(static_cast<Byte>(((offset - 0x4300) >> 4) & 0x07), static_cast<Byte>(offset & 0x0F), value);
      }
      break;

    case MemoryRegion::UNMAPPED:
      if (msu1 && (offset & 0xFFF8) == 0x2000) {
        msu1->write(offset, value);
        break;
      }
      cartridge->write(address, value);
      break;
  }
}

auto Bus::peek_wram(Address offset) const -> Byte { return wram->read(offset & 0x1FFFF); }
auto Bus::poke_wram(Address offset, Byte value) -> void { wram->write(offset & 0x1FFFF, value); }

auto Bus::apply_ram_cheats() -> void {
  if (!cheats) return;
  for (const Cheat& c : cheats->ram_writes()) {
    const Byte bank = static_cast<Byte>(c.address >> 16);
    poke_wram((bank == 0x7E || bank == 0x7F) ? (c.address & 0x1FFFF) : (c.address & 0x1FFF), c.value);
  }
}

auto Bus::read16(Address address) -> Word {
  Byte low = read(address);
  Byte high = read(address + 1);
  return (static_cast<Word>(high) << 8) | low;
}

auto Bus::write16(Address address, Word value) -> void {
  write(address, static_cast<Byte>(value & 0xFF));
  write(address + 1, static_cast<Byte>((value >> 8) & 0xFF));
}

auto Bus::load_rom(const std::string& path) -> bool {
  return cartridge->load(path);
}

auto Bus::is_rom_loaded() const -> bool {
  return cartridge->is_loaded();
}

// Banks $00-$3F and $80-$BF hold the system area below $8000; everything else is WRAM or cartridge.
auto Bus::decode_address(Address address) const -> MemoryRegion {
  const Byte bank = (address >> 16) & 0xFF;
  const Word offset = address & 0xFFFF;

  if (bank == 0x7E || bank == 0x7F) return MemoryRegion::WRAM;

  const bool system_bank = (bank & 0x7F) < 0x40;
  if (!system_bank || offset >= 0x6000) return MemoryRegion::CARTRIDGE;

  if (offset < 0x2000) return MemoryRegion::WRAM;                     // mirror of $7E:0000-1FFF
  if (offset >= 0x2100 && offset <= 0x213F) return MemoryRegion::PPU_REGISTERS;
  if (offset >= 0x2140 && offset <= 0x217F) return MemoryRegion::APU_REGISTERS;  // $2140-3 mirrored
  if (offset >= 0x2180 && offset <= 0x2183) return MemoryRegion::WRAM_PORT;
  if (offset == 0x4016 || offset == 0x4017) return MemoryRegion::JOYPAD;
  if (offset >= 0x4200 && offset <= 0x421F) return MemoryRegion::CPU_IO;
  if (offset >= 0x4300 && offset <= 0x437F) return MemoryRegion::DMA;
  return MemoryRegion::UNMAPPED;
}

// Offset into the 128KB WRAM: banks $7E-$7F directly, the low 8KB mirror maps to $7E:0000.
auto Bus::wram_offset(Address address) -> Address {
  const Byte bank = (address >> 16) & 0xFF;
  const Address offset = address & 0xFFFF;
  if (bank == 0x7E || bank == 0x7F) return (static_cast<Address>(bank - 0x7E) << 16) | offset;
  return offset & 0x1FFF;
}

auto Bus::reset() -> void {
  nmitimen = 0;
  wrio = 0;
  wrmpya = 0;
  wrmpyb = 0;
  dividend = 0;
  divisor = 0;
  htime = 0x1FF;
  vtime = 0x1FF;
  hdmaen = 0;
  memsel = 0;
  rdnmi = 0;
  timeup = 0;
  rddiv = 0;
  rdmpy = 0;
  joy1 = 0;
  joy2 = 0;
  joy3 = 0;
  joy4 = 0;
  for (auto& port : ports) port.latch(false);
  joypad_strobe = false;
  nmi_pending = false;
  vblank_active = false;
  wmadd = 0;
  open_bus = 0;
  pending_clocks = 0;
  cpu_accesses = 0;
  dma_active = false;
  in_cpu_step = false;
  nmi_fresh = nmi_blocked = false;
  rdnmi_read_early = false;
  timeup_read_early = false;
  for (auto& ch : dma_channels) ch.hdma_terminated = true;
  cartridge->reset();
}

auto Bus::access_clocks(Address address) const -> int {
  const Byte bank = (address >> 16) & 0xFF;
  const Word offset = address & 0xFFFF;
  const bool fast_rom = (bank & 0x80) && (memsel & 0x01);
  if ((bank & 0x40) == 0) {  // $00-$3F, $80-$BF
    for (const auto& range : kSystemAreaSpeeds) {
      if (offset < range.end) return range.clocks;
    }
  }
  return fast_rom ? kFastClocks : kSlowClocks;
}

auto Bus::end_cpu_step(int cpu_cycles) -> uint64_t {
  in_cpu_step = false;
  nmi_blocked = false;
  pending_clocks += static_cast<uint64_t>(std::max(0, cpu_cycles - cpu_accesses) * kInternalCycleClocks);
  cpu_accesses = 0;
  const uint64_t clocks = pending_clocks + refresh_stall(pending_clocks);
  pending_clocks = 0;
  return clocks;
}

auto Bus::refresh_stall(uint64_t clocks) const -> uint64_t {
  return ppu ? dram_refresh_stall(ppu->hclock(), clocks) : 0;
}

// Each refresh the step reaches (from H = `hclock`) pushes the rest of it 40 clocks later. A step
// ending exactly at H = 538 takes that line's refresh; one starting there has already had it.
auto dram_refresh_stall(int hclock, uint64_t clocks) -> uint64_t {
  int64_t until = kDramRefreshClock - hclock;  // clocks until the next refresh starts
  if (until <= 0) until += kLineClocks;
  uint64_t stall = 0;
  while (static_cast<int64_t>(clocks + stall) >= until) {
    stall += kDramRefreshClocks;
    until += kLineClocks;
  }
  return stall;
}

auto Bus::timer_hits(int v, int from, int to) const -> bool {
  // NMITIMEN bit 4 = H-IRQ, bit 5 = V-IRQ. H alone fires every line at HTIME, V alone at the
  // start of line VTIME, both at HTIME on line VTIME.
  const int mode = (nmitimen >> 4) & 0x03;
  if (mode == 0) return false;
  if ((mode & 0x01) && htime > kLastDot) return false;  // the H counter never gets there
  if ((mode & 0x02) && v != vtime) return false;
  const int at = (mode & 0x01) ? htime * 4 : 0;
  return at >= from && at < to;
}

auto Bus::on_clock(int v, int from, int to) -> void {
  scope_light(v, from, to);
  if (!timer_hits(v, from, to)) return;
  if (timeup_read_early) {
    timeup_read_early = false;  // this instruction's $4211 read already took it
    return;
  }
  timeup |= 0x80;
}

auto Bus::timer_fires_within(uint64_t clocks) const -> bool {
  if (!ppu || clocks == 0) return false;
  int v = ppu->vcounter();
  int c = ppu->hclock();
  const int lines = ppu->frame_lines();
  while (clocks > 0) {
    const auto run = static_cast<int>(std::min<uint64_t>(clocks, static_cast<uint64_t>(ppu::PPU::kClocksPerLine - c)));
    if (timer_hits(v, c, c + run)) return true;
    c += run;
    clocks -= static_cast<uint64_t>(run);
    if (c == ppu::PPU::kClocksPerLine) {
      c = 0;
      v = (v + 1) % lines;
    }
  }
  return false;
}

auto Bus::auto_joypad_busy() const -> bool {
  if (!(nmitimen & 0x01) || !ppu || !vblank_active) return false;
  const int since = (ppu->vcounter() - ppu->vblank_line()) * ppu::PPU::kClocksPerLine + ppu->hclock();
  return since >= kAutoJoypadStart && since < kAutoJoypadStart + kAutoJoypadClocks;
}

// $4016 reads port 1 and $4017 port 2, one bit per read (B first), on data line 0. Line 1
// (multitap pads 3/4) reads 0, and $4017 bits 2-4 are tied high. While the latch is high the
// port keeps returning the live B button.
auto Bus::read_joypad_port(Word offset) -> Byte {
  const int port = offset == 0x4017 ? 1 : 0;
  const Byte fixed = port == 1 ? 0x1C : 0x00;
  return static_cast<Byte>(fixed | ports[static_cast<size_t>(port)].clock(port_iobit(port)));
}

auto Bus::make_ports() -> std::array<ControllerPort, kPorts> {
  std::array<ControllerPort, kPorts> made{};
  made[1].set_device(Device::None);
  return made;
}

// The Super Scope in port 2 latches the counters as the beam passes its aim point.
auto Bus::scope_light(int v, int from, int to) -> void {
  const ControllerPort& port = ports[1];
  if (port.device() != Device::SuperScope || !ppu || !port_iobit(1)) return;
  const ScopeInput& aim = port.scope();
  if (aim.offscreen || v != aim.y + 1) return;
  const int dot = aim.x + kScopeDotOffset;
  if (dot * 4 >= from && dot * 4 < to) ppu->latch_counters(dot, v);
}

auto Bus::on_vblank_start() -> void {
  vblank_active = true;
  if (!rdnmi_read_early) rdnmi |= 0x80;  // else a $4210 read in this instruction already took it
  rdnmi_read_early = false;
  if (nmitimen & 0x01) {
    // Auto-joypad read strobes both ports and clocks 16 bits out of each: D0 into $4218/$421A,
    // D1 into $421C/$421E. The reports are left read out, so a manual $4016/$4017 read
    // afterwards continues past bit 16 (1s for a pad) until the game strobes again.
    const auto p1 = ports[0].auto_read(port_iobit(0), joypad_strobe);
    const auto p2 = ports[1].auto_read(port_iobit(1), joypad_strobe);
    joy1 = p1[0];
    joy3 = p1[1];
    joy2 = p2[0];
    joy4 = p2[1];
  }
  // NMI fires on this edge; enabling it later fires only while RDNMI is unread (see $4200).
  if (nmitimen & 0x80) {
    nmi_pending = true;
    nmi_fresh = true;
  }
}

auto Bus::on_vblank_end() -> void {
  vblank_active = false;
  rdnmi &= 0x7F;
}

auto Bus::poll_nmi() -> bool {
  if (!nmi_pending || nmi_blocked) {
    return false;
  }
  nmi_pending = false;
  return true;
}

auto Bus::set_pad(int port, int slot, Word state) -> void {
  if (port < 0 || port >= kPorts) return;
  ControllerPort& p = ports[static_cast<size_t>(port)];
  p.set_pad(slot, state);
  // Without auto-read the $4218 latches would go stale; show the live pad so polling still works.
  if (!(nmitimen & 0x01) && slot == 0 && p.device() == Device::Gamepad) (port == 0 ? joy1 : joy2) = state;
}

auto Bus::set_port_connected(int port, bool connected) -> void {
  if (port < 0 || port >= kPorts) return;
  ControllerPort& p = ports[static_cast<size_t>(port)];
  if (!connected) {
    p.set_device(Device::None);
    p.set_pad(0, 0);
  } else if (p.device() == Device::None) {
    p.set_device(Device::Gamepad);
  }
}

auto Bus::set_port_device(int port, Device device) -> void {
  if (port >= 0 && port < kPorts) ports[static_cast<size_t>(port)].set_device(device);
}

auto Bus::serialize(Archive& ar) -> void {
  wram->serialize(ar);
  cartridge->serialize(ar);
  ar(dma_channels, nmitimen, wrio, wrmpya, wrmpyb, dividend, divisor, htime, vtime, hdmaen, memsel, rdnmi, timeup);
  if (ar.at_least(5)) {
    ar(rddiv, rdmpy, joy1, joy2, joy3, joy4, ports, joypad_strobe, nmi_pending, vblank_active, wmadd, open_bus);
  } else {  // version 4: two gamepad ports (written too, for the older-file tests)
    std::array<Word, kPorts> pads{}, shift{};
    std::array<bool, kPorts> connected{};
    for (size_t p = 0; p < ports.size() && !ar.loading(); p++) {
      pads[p] = ports[p].pad(0);
      shift[p] = ports[p].shift(0);
      connected[p] = ports[p].device() != Device::None;
    }
    ar(rddiv, rdmpy, joy1, joy2, joy3, joy4, pads, shift, connected, joypad_strobe, nmi_pending, vblank_active, wmadd, open_bus);
    for (size_t p = 0; p < ports.size() && ar.loading(); p++) {
      ControllerPort& port = ports[p];
      port = ControllerPort{};
      port.set_device(connected[p] ? Device::Gamepad : Device::None);
      port.set_pad(0, pads[p]);
      port.latch(joypad_strobe);
      port.set_shift(0, shift[p]);
    }
  }
  ar(pending_clocks, cpu_accesses, dma_active, mar);
  if (ar.loading()) in_cpu_step = rdnmi_read_early = timeup_read_early = nmi_fresh = nmi_blocked = false;
}

auto Bus::read_cpu_io(Word offset) -> Byte {
  switch (offset) {
    case 0x4210: {
      // Bits 3-0 are the 5A22 version. Bits 6-4 are open bus; we drive them as 0.
      Byte flag = rdnmi & 0x80;
      // The PPU hasn't caught up with this instruction yet. If V-blank starts before this read,
      // the read sees (and clears) the new flag; the NMI still fires after the instruction.
      // Games poll $4210 with NMI on and an NMI handler that also reads it (Final Fantasy III's
      // "play song" event: LDA $4210 / BPL at $C0:B7A0): only these reads can ever see the flag.
      if (const uint64_t ahead = clocks_ahead(); ahead > 0 && ppu) {
        if (ppu->clocks_until_line(ppu->vblank_line()) <= ahead) {
          flag = 0x80;
          rdnmi_read_early = true;
        } else if (ppu->clocks_until_line(0) <= ahead) {
          flag = 0;  // V-blank ended before the read
        }
      }
      rdnmi &= 0x7F;
      return static_cast<Byte>(flag | 0x02);
    }
    case 0x4211: {
      Byte value = timeup & 0x80;
      timeup &= 0x7F;
      if (timer_fires_within(clocks_ahead())) {  // the H/V timer fires before this read
        value = 0x80;
        timeup_read_early = true;
      }
      return value;
    }
    case 0x4212: {
      Byte value = 0;
      if (vblank_active) value |= 0x80;
      if (ppu && (ppu->hclock() >= ppu::PPU::kHblankClock || ppu->hclock() < 4)) value |= 0x40;
      if (auto_joypad_busy()) value |= 0x01;
      return value;
    }
    case 0x4213:
      return wrio;
    case 0x4214: return static_cast<Byte>(rddiv & 0xFF);
    case 0x4215: return static_cast<Byte>((rddiv >> 8) & 0xFF);
    case 0x4216: return static_cast<Byte>(rdmpy & 0xFF);
    case 0x4217: return static_cast<Byte>((rdmpy >> 8) & 0xFF);
    case 0x4218: return static_cast<Byte>(joy1 & 0xFF);
    case 0x4219: return static_cast<Byte>((joy1 >> 8) & 0xFF);
    case 0x421A: return static_cast<Byte>(joy2 & 0xFF);
    case 0x421B: return static_cast<Byte>((joy2 >> 8) & 0xFF);
    case 0x421C: return static_cast<Byte>(joy3 & 0xFF);
    case 0x421D: return static_cast<Byte>((joy3 >> 8) & 0xFF);
    case 0x421E: return static_cast<Byte>(joy4 & 0xFF);
    case 0x421F: return static_cast<Byte>((joy4 >> 8) & 0xFF);
    default:
      return 0x00;
  }
}

auto Bus::write_cpu_io(Word offset, Byte value) -> void {
  switch (offset) {
    case 0x4200:
      // Turning NMI on while the V-blank flag is still unread fires it straight away.
      if ((value & 0x80) && !(nmitimen & 0x80) && (rdnmi & 0x80)) nmi_pending = true;
      if ((value & 0x30) == 0) timeup &= 0x7F;  // IRQs off: drop the pending one
      nmitimen = value;
      break;
    case 0x4201:
      wrio = value;
      break;
    case 0x4202:
      wrmpya = value;
      break;
    case 0x4203:
      wrmpyb = value;
      rdmpy = static_cast<Word>(wrmpya) * static_cast<Word>(wrmpyb);
      break;
    case 0x4204:
      dividend = static_cast<Word>((dividend & 0xFF00) | value);
      break;
    case 0x4205:
      dividend = static_cast<Word>((dividend & 0x00FF) | (value << 8));
      break;
    case 0x4206:
      divisor = value;
      if (divisor == 0) {
        rddiv = 0xFFFF;
        rdmpy = dividend;
      } else {
        rddiv = static_cast<Word>(dividend / divisor);
        rdmpy = static_cast<Word>(dividend % divisor);
      }
      break;
    case 0x4207:
      htime = static_cast<Word>((htime & 0x0100) | value);
      break;
    case 0x4208:
      htime = static_cast<Word>((htime & 0x00FF) | ((value & 0x01) << 8));
      break;
    case 0x4209:
      vtime = static_cast<Word>((vtime & 0x0100) | value);
      break;
    case 0x420A:
      vtime = static_cast<Word>((vtime & 0x00FF) | ((value & 0x01) << 8));
      break;
    case 0x420B:
      trigger_dma(value);
      break;
    case 0x420C:
      hdmaen = value;
      break;
    case 0x420D:
      memsel = value & 0x01;
      break;
    default:
      break;
  }
}

void Bus::dump_memory(Address start, Address length) {
  //printf("Memory dump from 0x%06X:\n", start);
  for (Address i = 0; i < length; i += 16) {
    //printf("  %06X: ", start + i);
    for (Address j = 0; j < 16 && (i + j) < length; j++) {
      //printf("%02X ", read(start + i + j));
    }
    //printf("\n");
  }
}

auto Bus::load_test_rom(const std::vector<Byte>& data, MapMode mode, std::size_t sram_bytes) -> void {
  cartridge->load_raw(data, mode, sram_bytes);
}

} // namespace bitforge::memory