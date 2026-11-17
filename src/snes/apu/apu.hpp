// SPDX-License-Identifier: MIT
#pragma once
#include "snes/types.hpp"
#include "snes/apu/dsp.hpp"
#include "snes/apu/spc700.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace bitforge::apu {

// The sound module: S-SMP (SPC700 + 64 KB ARAM + timers + ports) and S-DSP.
// The S-CPU talks to it through $2140-$2143 (mirrored to $217F); the system hands it master
// clocks after every CPU instruction and it catches up deterministically from those.
//
// Port timing: within a CPU instruction the bus calls cpu_read()/cpu_write() with the master
// clocks elapsed so far. A CPU read first runs the SMP up to that moment; a CPU write is stamped
// with it and the SMP sees the new value only from the cycle it reads the port at or after that
// time. A port the SMP writes in an instruction that ends past the CPU's time keeps showing the
// old value to the CPU until the CPU reaches the write. Streaming sound drivers that count cycles
// instead of handshaking (Star Ocean, Tales of Phantasia) depend on this.
#include <cstdint>
class APU : private SPC700::Bus {
public:
  // APU crystal. Nominally 24.576 MHz; real consoles run at about 24.607 MHz, which gives the
  // 32040 Hz output rate emulators use. SPC700 = crystal / 24, DSP sample = crystal / 768.
  static constexpr uint64_t kClockHz = 24'606'720;
  static constexpr int kSampleRate = static_cast<int>(kClockHz / 768);  // 32040
  static constexpr int kSmpCyclesPerSample = 32;

  APU();
  auto reset() -> void;

  // S-CPU side: port 0-3 (address & 3). These two act immediately (tests, debugger, HDMA).
  auto read_register(Byte port) -> Byte;
  auto write_register(Byte port, Byte value) -> void;
  // The same, `elapsed` master clocks into the CPU instruction that step() will be given next.
  auto cpu_read(Byte port, uint64_t elapsed) -> Byte;
  auto cpu_write(Byte port, Byte value, uint64_t elapsed) -> void;

  // Runs the APU for this many S-CPU master clocks (21.477 MHz).
  auto step(uint32_t master_clocks) -> void;
  // Runs whole SPC700 instructions until at least this many SMP cycles have passed.
  auto run_smp_cycles(int cycles) -> void;

  // Stereo int16 frames (L, R, L, R, ...) produced since the last clear_samples().
  auto samples() const -> const std::vector<int16_t>& { return samples_; }
  auto clear_samples() -> void { samples_.clear(); }

  // Loads an .spc snapshot (RAM, DSP and SMP registers). Returns false if it isn't one.
  auto load_spc(const Byte* data, size_t size) -> bool;

  // SMP, DSP, ARAM, timers, ports and the clock budget. Pending output samples are dropped.
  auto serialize(Archive& ar) -> void {
    ar(ram, smp_, dsp_, timers, from_cpu, to_cpu, ipl_enabled, timers_running, ram_writable, dsp_addr, clock_budget,
       dsp_phase, to_cpu_prev_, to_cpu_at_);  // CPU writes are always delivered by the end of step()
    if (ar.loading()) {
      smp_clock_ = 0;
      credited_ = 0;
      pending_count_ = 0;
    }
    if (ar.loading()) samples_.clear();
  }

  // Debug and test access
  auto smp() -> SPC700& { return smp_; }
  auto dsp() -> DSP& { return dsp_; }
  auto aram() -> std::array<Byte, 0x10000>& { return ram; }
  auto smp_read(Word addr) -> Byte { return peek(addr); }  // no cycle spent
  auto smp_write(Word addr, Byte value) -> void { poke(addr, value); }
  auto port_out(int port) const -> Byte { return to_cpu[static_cast<size_t>(port)]; }

private:
  struct Timer {
    int divider = 128;  // SMP cycles per tick: 128 (8 kHz) or 16 (64 kHz)
    int phase = 0;
    Byte target = 0;    // 0 = 256
    Byte count = 0;
    Byte out = 0;       // 4-bit, cleared on read
    bool enabled = false;

    auto step(int cycles) -> void;
    auto serialize(Archive& ar) -> void { ar(divider, phase, target, count, out, enabled); }
  };

  auto read(Word addr) -> Byte override;
  auto write(Word addr, Byte value) -> void override;
  auto idle() -> void override;
  auto cycle() -> void;
  auto peek(Word addr) -> Byte;
  auto poke(Word addr, Byte value) -> void;
  auto read_io(Word addr) -> Byte;
  auto write_io(Word addr, Byte value) -> void;
  auto write_control(Byte value) -> void;
  auto tick(int cycles) -> void;
  auto run_instruction() -> int;            // one SMP instruction; advances smp_clock_
  auto credit(uint64_t elapsed) -> void;    // CPU time inside the current step
  auto deliver_cpu_writes(int64_t until) -> void;
  auto cpu_now() const -> int64_t { return smp_clock_ + clock_budget; }
  auto access_time() const -> int64_t;

  // A CPU port write the SMP hasn't reached yet (at most a few per CPU instruction).
  struct PendingWrite {
    int64_t at;
    Byte port;
    Byte value;
  };

  std::array<Byte, 0x10000> ram{};
  SPC700 smp_;
  DSP dsp_;
  std::array<Timer, 3> timers{};
  std::array<Byte, 4> from_cpu{};  // written at $2140-3, read at $F4-7
  std::array<Byte, 4> to_cpu{};    // written at $F4-7, read at $2140-3
  bool ipl_enabled = true;
  bool timers_running = true;  // TEST: timers enabled and not halted
  bool ram_writable = true;    // TEST bit 1
  Byte dsp_addr = 0;

  int64_t clock_budget = 0;  // master clocks owed to the SMP, in SMP-cycle units (see step())
  // Times in the same units, counted from the start of the current step() (SMP side).
  int64_t smp_clock_ = 0;      // the SMP's position
  int64_t instr_start_ = 0;    // where the running SMP instruction started
  int instr_cycles_ = 0;       // its cycles so far (the current one included)
  bool in_instruction_ = false;
  uint64_t credited_ = 0;      // master clocks of the current CPU instruction already credited
  std::array<PendingWrite, 16> pending_{};
  size_t pending_count_ = 0;
  std::array<Byte, 4> to_cpu_prev_{};     // what the CPU sees until it reaches to_cpu_at_
  std::array<int64_t, 4> to_cpu_at_{};
  int dsp_phase = 0;         // SMP cycles toward the next DSP sample
  std::vector<int16_t> samples_;
};

} // namespace bitforge::apu
