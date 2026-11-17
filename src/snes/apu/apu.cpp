// SPDX-License-Identifier: MIT
#include "snes/apu/apu.hpp"
#include "utils/logging.hpp"
#include "snes/apu/ipl_rom.hpp"

#include <algorithm>
#include <cstring>

namespace bitforge::apu {

namespace {

// Clock ratio. NTSC master clock = 236.25 MHz / 11 (21.477 MHz); SMP = kClockHz / 24.
// One master clock buys kSmpPerMaster / kMasterUnit SMP cycles, kept as an exact fraction.
constexpr int64_t kSmpPerMaster = static_cast<int64_t>(APU::kClockHz / 24) * 11;
constexpr int64_t kMasterUnit = 236'250'000;

// $F0-$FF
enum Io : Word {
  TEST = 0xF0, CONTROL = 0xF1, DSPADDR = 0xF2, DSPDATA = 0xF3, CPUIO0 = 0xF4, CPUIO3 = 0xF7,
  AUXIO4 = 0xF8, AUXIO5 = 0xF9, T0TARGET = 0xFA, T2TARGET = 0xFC, T0OUT = 0xFD, T2OUT = 0xFF,
};
constexpr Word kIplBase = 0xFFC0;
constexpr int64_t kPast = INT64_MIN / 4;  // a port write the CPU can already see
constexpr std::array<int, 3> kTimerDividers = {128, 128, 16};  // 8 kHz, 8 kHz, 64 kHz

// .spc snapshot layout
constexpr char kSpcSignature[] = "SNES-SPC700 Sound File Data";
constexpr size_t kSpcPc = 0x25, kSpcA = 0x27, kSpcX = 0x28, kSpcY = 0x29, kSpcPsw = 0x2A, kSpcSp = 0x2B;
constexpr size_t kSpcRam = 0x100, kSpcDsp = 0x10100, kSpcIplRam = 0x101C0;
constexpr Byte kDspKon = 0x4C, kDspFlg = 0x6C;

} // namespace

APU::APU() : smp_(*this), dsp_(ram) { reset(); }

auto APU::reset() -> void {
  ram.fill(0);
  dsp_.reset();
  for (size_t i = 0; i < timers.size(); i++) timers[i] = Timer{kTimerDividers[i]};
  from_cpu.fill(0);
  to_cpu.fill(0);
  ipl_enabled = true;
  timers_running = true;
  ram_writable = true;
  dsp_addr = 0;
  clock_budget = 0;
  smp_clock_ = instr_start_ = 0;
  instr_cycles_ = 0;
  in_instruction_ = false;
  credited_ = 0;
  pending_count_ = 0;
  to_cpu_prev_.fill(0);
  to_cpu_at_.fill(kPast);
  dsp_phase = 0;
  samples_.clear();
  smp_.reset(static_cast<Word>(kIplRom[62] | (kIplRom[63] << 8)));
}

// ---- S-CPU side -------------------------------------------------------------------------------

auto APU::read_register(Byte port) -> Byte { return to_cpu[port & 3]; }

auto APU::write_register(Byte port, Byte value) -> void {
  APU_TRACE("CPU -> APU port %d = %02X", port & 3, value);
  deliver_cpu_writes(INT64_MAX);  // keep the order with any timed writes
  from_cpu[port & 3] = value;
}

auto APU::credit(uint64_t elapsed) -> void {
  if (elapsed <= credited_) return;
  clock_budget += static_cast<int64_t>(elapsed - credited_) * kSmpPerMaster;
  credited_ = elapsed;
}

auto APU::cpu_read(Byte port, uint64_t elapsed) -> Byte {
  credit(elapsed);
  while (clock_budget > 0) run_instruction();
  const size_t p = port & 3;
  return to_cpu_at_[p] > cpu_now() ? to_cpu_prev_[p] : to_cpu[p];
}

auto APU::cpu_write(Byte port, Byte value, uint64_t elapsed) -> void {
  APU_TRACE("CPU -> APU port %d = %02X (+%llu)", port & 3, value, static_cast<unsigned long long>(elapsed));
  credit(elapsed);
  const int64_t at = cpu_now();
  if (at <= smp_clock_ && pending_count_ == 0) {  // the SMP is already past this moment
    from_cpu[port & 3] = value;
    return;
  }
  if (pending_count_ == pending_.size()) deliver_cpu_writes(pending_[0].at);  // never in practice
  pending_[pending_count_++] = {at, static_cast<Byte>(port & 3), value};
}

// Applies, in order, the CPU writes made at or before `until`.
auto APU::deliver_cpu_writes(int64_t until) -> void {
  size_t done = 0;
  while (done < pending_count_ && pending_[done].at <= until) {
    from_cpu[pending_[done].port] = pending_[done].value;
    done++;
  }
  if (done == 0) return;
  std::copy(pending_.begin() + static_cast<std::ptrdiff_t>(done), pending_.begin() + static_cast<std::ptrdiff_t>(pending_count_),
            pending_.begin());
  pending_count_ -= done;
}

// The time of the bus access in progress: the end of the running instruction's current cycle.
auto APU::access_time() const -> int64_t {
  if (!in_instruction_) return smp_clock_;
  return instr_start_ + static_cast<int64_t>(instr_cycles_) * kMasterUnit;
}

// Every SMP cycle advances the timers and the DSP before its access lands, so a timer read or a
// DSP register write sees exactly the cycles before it.
auto APU::cycle() -> void {
  instr_cycles_++;
  tick(1);
}

auto APU::run_instruction() -> int {
  instr_start_ = smp_clock_;
  instr_cycles_ = 0;
  in_instruction_ = true;
  const int cycles = smp_.step();  // ticks per cycle through read()/write()/idle()
  in_instruction_ = false;
  smp_clock_ += static_cast<int64_t>(cycles) * kMasterUnit;
  clock_budget -= static_cast<int64_t>(cycles) * kMasterUnit;
  deliver_cpu_writes(smp_clock_);
  return cycles;
}

// ---- timing -----------------------------------------------------------------------------------

auto APU::step(uint32_t master_clocks) -> void {
  // Lockstep at instruction granularity: the SMP runs whole instructions until it has caught up
  // with the S-CPU (it may finish up to one instruction ahead; the overshoot carries over).
  // Port accesses inside the CPU instruction may already have run part of the way (credit()).
  credit(master_clocks);
  credited_ = 0;
  while (clock_budget > 0) run_instruction();
  deliver_cpu_writes(INT64_MAX);  // all made before the CPU's time, which the SMP has reached
  // Re-base the clocks on the SMP's position so they stay small.
  for (auto& at : to_cpu_at_) at = at > cpu_now() ? at - smp_clock_ : kPast;
  smp_clock_ = 0;
}

auto APU::run_smp_cycles(int cycles) -> void {
  while (cycles > 0) {
    const int used = run_instruction();
    clock_budget += static_cast<int64_t>(used) * kMasterUnit;  // outside the master-clock budget
    cycles -= used;
  }
  for (auto& at : to_cpu_at_) at = kPast;
  smp_clock_ = 0;
}

auto APU::tick(int cycles) -> void {
  if (timers_running) {
    for (auto& t : timers) t.step(cycles);
  }
  dsp_phase += cycles;
  while (dsp_phase >= kSmpCyclesPerSample) {
    dsp_phase -= kSmpCyclesPerSample;
    dsp_.run_sample(samples_);
  }
}

auto APU::Timer::step(int cycles) -> void {
  phase += cycles;
  while (phase >= divider) {
    phase -= divider;
    if (enabled && ++count == target) {  // Byte wrap makes target 0 mean 256
      count = 0;
      out = static_cast<Byte>((out + 1) & 0x0F);
    }
  }
}

// ---- SMP memory map ---------------------------------------------------------------------------

auto APU::read(Word addr) -> Byte {
  cycle();
  return peek(addr);
}

auto APU::write(Word addr, Byte value) -> void {
  cycle();
  poke(addr, value);
}

auto APU::idle() -> void { cycle(); }

auto APU::peek(Word addr) -> Byte {
  if ((addr & 0xFFF0) == 0x00F0) return read_io(addr);
  if (addr >= kIplBase && ipl_enabled) return kIplRom[addr - kIplBase];
  return ram[addr];
}

auto APU::poke(Word addr, Byte value) -> void {
  // Writes always land in RAM, even under the IPL ROM and the I/O registers.
  if (ram_writable) ram[addr] = value;
  if ((addr & 0xFFF0) == 0x00F0) write_io(addr, value);
}

auto APU::read_io(Word addr) -> Byte {
  switch (addr) {
    case DSPADDR: return dsp_addr;
    case DSPDATA: return dsp_.read(dsp_addr);  // $80-$FF read back $00-$7F
    case AUXIO4: case AUXIO5: return ram[addr];
    default: break;
  }
  if (addr >= CPUIO0 && addr <= CPUIO3) {
    if (pending_count_) deliver_cpu_writes(access_time());
    return from_cpu[addr - CPUIO0];
  }
  if (addr >= T0OUT && addr <= T2OUT) {
    Timer& t = timers[addr - T0OUT];
    const Byte value = t.out;
    t.out = 0;
    return value;
  }
  return 0x00;  // TEST, CONTROL and the timer targets are write-only
}

auto APU::write_io(Word addr, Byte value) -> void {
  switch (addr) {
    case TEST:
      if (smp_.registers().psw & SPC700::P) break;  // only takes effect with P clear
      timers_running = (value & 0x08) && !(value & 0x01);
      ram_writable = (value & 0x02) != 0;
      return;
    case CONTROL: write_control(value); return;
    case DSPADDR: dsp_addr = value; return;
    case DSPDATA:
      if (!(dsp_addr & 0x80)) dsp_.write(dsp_addr, value);
      return;
    default: break;
  }
  if (addr >= CPUIO0 && addr <= CPUIO3) {
    const size_t p = addr - CPUIO0;
    // Visible to the CPU from the end of the write cycle.
    to_cpu_prev_[p] = to_cpu_at_[p] > cpu_now() ? to_cpu_prev_[p] : to_cpu[p];
    to_cpu_at_[p] = access_time();
    to_cpu[p] = value;
  }
  if (addr >= T0TARGET && addr <= T2TARGET) timers[addr - T0TARGET].target = value;
}

auto APU::write_control(Byte value) -> void {
  for (size_t i = 0; i < timers.size(); i++) {
    const bool enable = (value >> i) & 1;
    if (enable && !timers[i].enabled) {  // a 0->1 edge restarts the timer
      timers[i].count = 0;
      timers[i].out = 0;
    }
    timers[i].enabled = enable;
  }
  if (value & 0x10) from_cpu[0] = from_cpu[1] = 0;  // clear input ports 0/1
  if (value & 0x20) from_cpu[2] = from_cpu[3] = 0;  // clear input ports 2/3
  ipl_enabled = (value & 0x80) != 0;
}

// ---- .spc snapshots ---------------------------------------------------------------------------

auto APU::load_spc(const Byte* data, size_t size) -> bool {
  if (size < kSpcDsp + 128 || std::memcmp(data, kSpcSignature, sizeof(kSpcSignature) - 1) != 0) return false;
  reset();
  std::copy_n(data + kSpcRam, ram.size(), ram.begin());
  // The 64 bytes of RAM hidden under the IPL ROM are stored separately.
  if (size >= kSpcIplRam + 64) std::copy_n(data + kSpcIplRam, 64, ram.begin() + kIplBase);

  for (int r = 0; r < 128; r++) {
    if (r != kDspKon && r != kDspFlg) dsp_.write(static_cast<Byte>(r), data[kSpcDsp + static_cast<size_t>(r)]);
  }
  dsp_.write(kDspFlg, data[kSpcDsp + kDspFlg]);
  dsp_.write(kDspKon, data[kSpcDsp + kDspKon]);

  // I/O registers as the snapshot left them in RAM.
  for (size_t i = 0; i < timers.size(); i++) timers[i].target = ram[T0TARGET + i];
  write_control(ram[CONTROL] & 0x87);  // timers and IPL; don't clear ports
  for (size_t i = 0; i < timers.size(); i++) timers[i].out = ram[T0OUT + i] & 0x0F;
  dsp_addr = ram[DSPADDR];
  for (int p = 0; p < 4; p++) from_cpu[static_cast<size_t>(p)] = ram[CPUIO0 + static_cast<Word>(p)];

  SmpRegisters r;
  r.pc = static_cast<Word>(data[kSpcPc] | (data[kSpcPc + 1] << 8));
  r.a = data[kSpcA];
  r.x = data[kSpcX];
  r.y = data[kSpcY];
  r.psw = data[kSpcPsw];
  r.sp = data[kSpcSp];
  smp_.set_registers(r);
  return true;
}

} // namespace bitforge::apu
