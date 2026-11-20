// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <memory>
#include <vector>
#include <array>
#include <cstdint>
#include <span>
#include "snes/controllers.hpp"
#include "snes/types.hpp"
#include "snes/cpu/cpu_bus.hpp"
#include "snes/memory/cartridge.hpp"
#include "snes/memory/wram.hpp"
#include "snes/serialize.hpp"

namespace bitforge::ppu { class PPU; }
namespace bitforge::apu { class APU; }
namespace bitforge::cpu { class CPU65c816; }
namespace bitforge { class Msu1; class CheatEngine; struct Cheat; }

namespace bitforge::memory {

class WRAM;

// One of the eight DMA/HDMA channels, $43n0-$43nB.
struct DMAChannel {
    Byte dmap = 0xFF;   // $43n0: direction, HDMA indirect, A-bus step, transfer mode
    Byte bbad = 0xFF;   // $43n1: B-bus address ($21xx)
    Word a1t = 0xFFFF;  // $43n2-3: A-bus address / HDMA table start
    Byte a1b = 0xFF;    // $43n4: A-bus bank / HDMA table bank
    Word das = 0xFFFF;  // $43n5-6: byte count / HDMA indirect address
    Byte dasb = 0xFF;   // $43n7: HDMA indirect bank
    Word a2a = 0xFFFF;  // $43n8-9: HDMA table pointer
    Byte ntrl = 0xFF;   // $43nA: HDMA line counter (bit 7 = repeat)
    Byte unused = 0xFF; // $43nB / $43nF
    bool hdma_do_transfer = false;
    bool hdma_terminated = true;

    auto serialize(Archive& ar) -> void {
        ar(dmap, bbad, a1t, a1b, das, dasb, a2a, ntrl, unused, hdma_do_transfer, hdma_terminated);
    }
};

// DRAM refresh: once per line, at H = 538 master clocks (dot ~134), the S-CPU and DMA halt for
// 40 master clocks while WRAM refreshes; the PPU, APU and coprocessors keep running.
inline constexpr int kDramRefreshClock = 538;
inline constexpr int kDramRefreshClocks = 40;
inline constexpr int kLineClocks = 1364;
// Stall clocks added to a CPU step of `clocks` master clocks starting at line position `hclock`.
auto dram_refresh_stall(int hclock, uint64_t clocks) -> uint64_t;

class Bus final : public cpu::CpuBus {
public:
  Bus();
  ~Bus();
  
  auto connect_ppu(ppu::PPU* ppu_device) -> void { ppu = ppu_device; }
  auto connect_apu(apu::APU* apu_device) -> void { apu = apu_device; }
  auto connect_cpu(cpu::CPU65c816* cpu_device) -> void { cpu = cpu_device; }
  auto connect_msu1(Msu1* msu1_device) -> void { msu1 = msu1_device; }  // $2000-$2007, or null
  // Read overrides on every S-CPU/DMA read, and WRAM writes by apply_ram_cheats(); or null.
  auto connect_cheats(const CheatEngine* engine) -> void { cheats = engine; }
  auto apply_ram_cheats() -> void;  // Pro Action Replay WRAM codes, once a frame
  
  auto read(Address address) -> Byte override;
  auto write(Address address, Byte value) -> void override;
  
  auto read16(Address address) -> Word;
  auto write16(Address address, Word value) -> void;
  
  auto load_rom(const std::string& path) -> bool;
  auto is_rom_loaded() const -> bool;
  
  void dump_memory(Address start, Address length);

  auto load_test_rom(const std::vector<Byte>& data, MapMode mode = MapMode::LoROM,
                     std::size_t sram_bytes = 0) -> void;

  auto write_dma_register(Byte channel, Byte reg, Byte value) -> void;
  auto read_dma_register(Byte channel, Byte reg) const -> Byte;
  auto trigger_dma(Byte mdmaen_value) -> void;
  auto dma_channel(int index) const -> const DMAChannel& { return dma_channels[static_cast<size_t>(index)]; }

  auto reset() -> void;

  // ---- Timing -------------------------------------------------------------------------------
  // Master clocks (21.477 MHz) per CPU access at this address: 6 (fast), 8 (slow) or 12 (joypad
  // serial ports). Banks $80-$FF ROM is fast when MEMSEL ($420D) bit 0 is set.
  auto access_clocks(Address address) const -> int;
  // The System wraps each CPU instruction in these. end_cpu_step() returns every master clock
  // spent since the last call: the instruction's memory accesses at their speeds, its internal
  // cycles at 6 clocks, and any DMA/HDMA time stolen in between.
  auto begin_cpu_step() -> void {
    cpu_accesses = 0;
    in_cpu_step = true;
    rdnmi_read_early = timeup_read_early = false;  // the PPU has run past the last instruction
    nmi_blocked = nmi_fresh;  // a V-blank NMI raised during the last instruction waits one more
    nmi_fresh = false;
  }
  auto end_cpu_step(int cpu_cycles) -> uint64_t;

  // ---- PPU callbacks ------------------------------------------------------------------------
  auto on_vblank_start() -> void;
  auto on_vblank_end() -> void;
  // The PPU advanced line `v` from master clock `from` to `to` (to <= 1364): H/V IRQ checks.
  auto on_clock(int v, int from, int to) -> void;
  auto hdma_init() -> void;       // line 0: reload every enabled channel's table
  auto hdma_run_line() -> void;   // H-blank of lines 0..224 (or 239)

  // Edge-latched NMI. True once, when the CPU should take the interrupt.
  auto poll_nmi() -> bool override;
  // Level IRQ line: held while TIMEUP ($4211) bit 7 is set, or by a cartridge coprocessor
  // (Super FX STOP, SA-1 messages and DMA).
  auto irq_pending() const -> bool override { return (timeup & 0x80) != 0 || cartridge->irq(); }

  // Controller ports 0 and 1 (the console's 1 and 2). A pad word is what auto-read leaves in
  // $4218/$4219 for a gamepad: B Y Sel Start U D L R A X L R 0 0 0 0. Port 1 starts with a
  // gamepad, port 2 empty.
  static constexpr int kPorts = 2;
  auto set_joypad(int port, Word state) -> void { set_pad(port, 0, state); }
  auto set_joypad1(Word state) -> void { set_joypad(0, state); }
  auto set_pad(int port, int slot, Word state) -> void;  // slot: multitap pad 0-3
  // Gamepad or nothing, for callers that only know pads. An empty port reads 0 on every bit (no
  // trailing 1s), which is how games tell a pad is plugged in.
  auto set_port_connected(int port, bool connected) -> void;
  auto set_port_device(int port, Device device) -> void;
  auto controller(int port) -> ControllerPort& { return ports[static_cast<size_t>(port & 1)]; }
  auto controller(int port) const -> const ControllerPort& { return ports[static_cast<size_t>(port & 1)]; }

  // WRAM byte at `offset` (0-$1FFFF) without touching the bus: for tests and tools.
  auto peek_wram(Address offset) const -> Byte;
  auto poke_wram(Address offset, Byte value) -> void;
  auto wram_bytes() -> std::span<Byte> { return wram->bytes(); }

  // The last address the S-CPU or DMA put on the bus.
  auto last_address() const -> Address { return mar; }

  auto cart() -> Cartridge& { return *cartridge; }
  auto cart() const -> const Cartridge& { return *cartridge; }
  auto serialize(Archive& ar) -> void;

private:
  auto read_mapped(Address address) -> Byte;
  auto refresh_stall(uint64_t clocks) const -> uint64_t;
  Msu1* msu1 = nullptr;
  const CheatEngine* cheats = nullptr;
  std::unique_ptr<WRAM> wram;
  std::unique_ptr<Cartridge> cartridge;
  std::array<DMAChannel, 8> dma_channels;
  
  ppu::PPU* ppu = nullptr;
  apu::APU* apu = nullptr;
  cpu::CPU65c816* cpu = nullptr;

  Byte nmitimen = 0;    // $4200
  Byte wrio = 0;        // $4201
  Byte wrmpya = 0;      // $4202
  Byte wrmpyb = 0;      // $4203
  Word dividend = 0;    // $4204-$4205
  Byte divisor = 0;     // $4206
  Word htime = 0x1FF;   // $4207-$4208
  Word vtime = 0x1FF;   // $4209-$420A
  Byte hdmaen = 0;      // $420C
  Byte memsel = 0;      // $420D
  Byte rdnmi = 0;       // $4210 bit 7, cleared on read and at the end of vblank
  Byte timeup = 0;      // $4211 bit 7, cleared on read
  Word rddiv = 0;       // $4214-$4215
  Word rdmpy = 0;       // $4216-$4217 (also the divide remainder)
  Word joy1 = 0;        // $4218-$4219, filled by auto-joypad read
  Word joy2 = 0;
  Word joy3 = 0;
  Word joy4 = 0;
  std::array<ControllerPort, kPorts> ports = make_ports();
  bool joypad_strobe = false;  // $4016 bit 0: the latch line to both ports
  bool nmi_pending = false;
  bool vblank_active = false;
  Address wmadd = 0;    // $2181-$2183 WRAM port address (17-bit)
  Byte open_bus = 0;    // last value seen on the data bus
  Address mar = 0;      // last address on the bus

  uint64_t pending_clocks = 0;  // master clocks not yet handed to the PPU
  int cpu_accesses = 0;         // CPU bus accesses in the current instruction
  bool dma_active = false;      // DMA accesses are timed per byte, not per access
  bool in_cpu_step = false;     // between begin_cpu_step() and end_cpu_step()
  // A $4210/$4211 read already returned (and so acknowledged) a flag the PPU raises later in the
  // same instruction: the PPU's event then fires the NMI edge but leaves the flag clear. Only
  // live inside one instruction, so not part of save states.
  bool rdnmi_read_early = false;
  bool timeup_read_early = false;
  // The V-blank NMI is not taken at the boundary right after the instruction in which the flag
  // rose: the next instruction runs first, so a $4210 poll always gets one look at the flag
  // before an NMI handler that also reads it (Final Fantasy III's song wait at $C0:B7A0).
  // Without this the loop misses whenever V-blank starts during its BPL; with our
  // approximate CPU timing that phase can repeat every frame and the loop never exits.
  bool nmi_fresh = false;    // raised since the last begin_cpu_step()
  bool nmi_blocked = false;  // held for the current step
  
  enum class MemoryRegion {
    WRAM,
    WRAM_PORT,
    CARTRIDGE,
    PPU_REGISTERS,
    APU_REGISTERS,
    JOYPAD,
    CPU_IO,
    DMA,
    UNMAPPED
  };
  
  auto decode_address(Address address) const -> MemoryRegion;
  static auto wram_offset(Address address) -> Address;
  auto read_cpu_io(Word offset) -> Byte;
  auto write_cpu_io(Word offset, Byte value) -> void;
  auto read_joypad_port(Word offset) -> Byte;
  auto port_iobit(int port) const -> bool { return (wrio >> (6 + port)) & 1; }  // $4201 bits 6/7
  auto scope_light(int v, int from, int to) -> void;
  static auto make_ports() -> std::array<ControllerPort, kPorts>;
  auto auto_joypad_busy() const -> bool;
  // The PPU runs after each instruction, so a $4210/$4211 read sees the flags as they were when
  // the instruction began. These look ahead over the clocks the instruction has used so far.
  auto clocks_ahead() const -> uint64_t { return in_cpu_step ? pending_clocks : 0; }
  auto timer_hits(int v, int from, int to) const -> bool;     // H/V timer fires in [from, to) of line v
  auto timer_fires_within(uint64_t clocks) const -> bool;
  auto hdma_reload(DMAChannel& ch) -> void;
  auto dma_transfer_unit(DMAChannel& ch, Address a_addr, int index) -> void;
};

} // namespace bitforge::memory
