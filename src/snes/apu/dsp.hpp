// SPDX-License-Identifier: MIT
#pragma once
#include "snes/serialize.hpp"
#include "snes/types.hpp"

#include <array>
#include <vector>

namespace bitforge::apu {

// S-DSP: 8 BRR voices with Gaussian interpolation, ADSR/GAIN envelopes, noise, pitch modulation
// and an 8-tap FIR echo, mixed to 32 kHz stereo.
//
// One output sample is 32 DSP clocks. run_sample() walks a 32-slot timeline (the table in
// dsp.cpp, laid out like the "Register and RAM Access Timing Chart" in fullsnes, "SNES APU Low
// Level Timings"), so register reads and ARAM accesses happen in hardware order at sample
// granularity. Formulas follow fullsnes ("SNES APU DSP ..." chapters) and anomie's S-DSP notes.
class DSP {
public:
  explicit DSP(std::array<Byte, 0x10000>& aram);

  auto reset() -> void;
  auto read(Byte addr) const -> Byte { return regs_[addr & 0x7F]; }
  auto write(Byte addr, Byte value) -> void;
  // Appends one stereo frame (left, right) to `out`.
  auto run_sample(std::vector<int16_t>& out) -> void;

  // Building blocks, public so the unit tests can drive them in isolation. Samples are kept in
  // the 16-bit form the voice uses internally: the 15-bit BRR value shifted left by one.
  static auto decode_brr_block(const Byte* block, std::array<int16_t, 16>& out, int& prev1, int& prev2) -> void;
  static auto gaussian(int phase, int s0, int s1, int s2, int s3) -> int;  // phase 0-255, s0 oldest
  static auto counter_fires(uint16_t counter, int rate) -> bool;

  enum class EnvMode : Byte { Release, Attack, Decay, Sustain };
  struct Voice {
    // Register copies
    int8_t volume[2] = {};
    uint16_t pitch = 0;  // 14 bits
    Byte srcn = 0, adsr1 = 0, adsr2 = 0, gain = 0;
    bool kon = false, koff = false, pmon = false, non = false, eon = false;

    // BRR decoder and pitch counter
    std::array<int16_t, 12> ring{};  // decoded samples, oldest overwritten first
    int ring_pos = 0;                // next slot to write
    int counter = 0;                 // pitch counter: bits 14+ = samples to decode, 11-4 Gauss index
    Word block = 0;                  // address of the current 9-byte BRR block
    int data_offset = 1;             // 1, 3, 5, 7: data byte pair decoded next
    int start_delay = 0;             // samples left of the 5-sample key-on delay

    // Envelope
    EnvMode env_mode = EnvMode::Release;
    int envelope = 0;   // 0-0x7FF
    int raw_level = 0;  // last computed level before clamping (GAIN bent-line compares it)
    Byte envx = 0;

    // Flags sampled from the registers at their timeline slots
    bool kon_pending = false, kon_now = false, koff_now = false, pmon_now = false, non_now = false,
         eon_now = false, ended = false, wrapped = false;

    // Field order is part of the savestate format.
    auto serialize(Archive& ar) -> void {
      ar(volume, pitch, srcn, adsr1, adsr2, gain, kon, koff, pmon, non, eon, ring, ring_pos, counter, block,
         data_offset, start_delay, env_mode, envelope, raw_level, envx, kon_pending, kon_now, koff_now, pmon_now,
         non_now, eon_now, ended, wrapped);
    }
  };
  auto voice(int n) const -> const Voice& { return voices_[static_cast<size_t>(n)]; }

  // Everything but ARAM (the APU owns it). Field order is part of the savestate format.
  auto serialize(Archive& ar) -> void {
    ar(regs_, voices_, rate_clock_, kon_poll_, mvol_, mix_, flg_reset_, flg_mute_, noise_rate_, noise_, dir_);
    ar(evol_, efb_, fir_, fir_buf_, fir_pos_, esa_, edl_, flg_echo_off_, fir_out_, echo_mix_, esa_now_, echo_off_now_,
       echo_ptr_, echo_index_, echo_size_);
    ar(bus_.dir, bus_.srcn, bus_.header, bus_.data, bus_.adsr1, bus_.envx, bus_.outx, bus_.entry, bus_.next_block,
       bus_.pitch, bus_.sample);
  }

private:
  enum class Op : Byte;
  struct Slot;
  friend struct DspTimeline;  // the slot table in dsp.cpp

  std::array<Byte, 0x10000>& ram_;
  std::array<Byte, 128> regs_{};
  std::array<Voice, 8> voices_{};
  std::vector<int16_t>* out_ = nullptr;

  // Global state
  uint16_t rate_clock_ = 0;  // shared envelope/noise rate counter
  bool kon_poll_ = true;     // KON/KOFF are polled every other sample
  int8_t mvol_[2] = {};
  int mix_[2] = {};          // main mix, left/right
  bool flg_reset_ = true, flg_mute_ = true;
  int noise_rate_ = 0;
  int noise_ = 0x4000;       // 15-bit noise generator
  Byte dir_ = 0;

  // Echo
  int8_t evol_[2] = {}, efb_ = 0;
  std::array<int8_t, 8> fir_{};
  int fir_buf_[2][8] = {};   // last eight echo samples per channel (15-bit)
  int fir_pos_ = 0;
  Byte esa_ = 0, edl_ = 0;
  bool flg_echo_off_ = true;
  int fir_out_[2] = {}, echo_mix_[2] = {};
  Byte esa_now_ = 0;
  bool echo_off_now_ = true;
  Word echo_ptr_ = 0;
  int echo_index_ = 0, echo_size_ = 0;

  // Values the timeline carries from one slot to a later one (shared by all voices).
  struct Bus {
    Byte dir = 0, srcn = 0, header = 0, data = 0, adsr1 = 0, envx = 0, outx = 0;
    Word entry = 0, next_block = 0;
    int pitch = 0, sample = 0;
  } bus_;

  auto run(Op op, Voice& v, int n) -> void;
  auto read16(Word addr) const -> Word;
  auto mix_voice(Voice& v, int channel) -> void;
  auto decode_four(Voice& v) -> void;
  auto update_envelope(Voice& v) -> void;

  // Voice timeline steps (see the table in dsp.cpp)
  auto read_srcn(Voice& v) -> void;
  auto read_directory(Voice& v) -> void;
  auto read_brr_header(Voice& v) -> void;
  auto produce_sample(Voice& v) -> void;
  auto advance_counter(Voice& v) -> void;
  auto publish_endx(Voice& v) -> void;

  // Echo timeline steps
  auto fir_tap(int channel, int tap) const -> int;
  auto output_level(int channel) const -> int;
  auto read_echo(int channel) -> void;
  auto write_echo(int channel) -> void;
  auto run_fir() -> void;
  auto echo_feedback() -> void;
  auto emit_frame() -> void;
  auto step_echo_index() -> void;
  auto tick_rate_clock() -> void;
};

} // namespace bitforge::apu
