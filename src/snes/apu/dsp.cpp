// SPDX-License-Identifier: MIT
#include "snes/apu/dsp.hpp"
#include "snes/apu/dsp_tables.hpp"

#include <algorithm>

namespace bitforge::apu {

namespace {

constexpr auto clamp16(int v) -> int { return std::clamp(v, -32768, 32767); }
constexpr auto wrap16(int v) -> int { return static_cast<int16_t>(v); }

// Register map (fullsnes, "SNES APU DSP ..." chapters). Voice registers are $v0-$v9; FIR
// coefficients are $vF; the rest are globals in columns $xC / $xD.
namespace reg {
constexpr Byte kVolL = 0x0, kVolR = 0x1, kPitchL = 0x2, kPitchH = 0x3, kSrcn = 0x4, kAdsr1 = 0x5,
               kAdsr2 = 0x6, kGain = 0x7, kEnvx = 0x8, kOutx = 0x9, kFir = 0xF;
constexpr Byte kMvolL = 0x0C, kMvolR = 0x1C, kEvolL = 0x2C, kEvolR = 0x3C, kKon = 0x4C, kKoff = 0x5C,
               kFlg = 0x6C, kEndx = 0x7C, kEfb = 0x0D, kPmon = 0x2D, kNon = 0x3D, kEon = 0x4D,
               kDir = 0x5D, kEsa = 0x6D, kEdl = 0x7D;
}  // namespace reg

constexpr int kKeyOnDelay = 5;       // samples between KON and the voice starting
constexpr int kBlockBytes = 9;       // BRR block: header + 8 data bytes
constexpr int kEchoUnitBytes = 2048; // EDL is in 2 KB steps

// fullsnes, "BRR Samples": nibble -> 15-bit sample through shift and prediction filter.
// `old_s`/`older_s` are the previous two samples in their stored 16-bit form (value * 2).
// The result is clamped to 16 bits, then stored doubled: that drops bit 15, which is the
// hardware's 15-bit wrap ("lost sign" glitch).
auto brr_decode(int nibble, int shift, int filter, int old_s, int older_s) -> int16_t {
  if (shift > 12) {  // reserved shifts behave like 12 with the nibble pre-shifted by 3
    shift = 12;
    nibble >>= 3;
  }
  int value = (nibble << shift) >> 1;
  const int old = old_s >> 1, older = older_s >> 1;
  switch (filter) {
    case 1: value += old + ((-old) >> 4); break;
    case 2: value += 2 * old + ((-old * 3) >> 5) - older + (older >> 4); break;
    case 3: value += 2 * old + ((-old * 13) >> 6) - older + ((older * 3) >> 4); break;
    default: break;
  }
  return static_cast<int16_t>(clamp16(value) * 2);
}

constexpr auto signed_nibble(unsigned n) -> int { return static_cast<int>(n & 7) - static_cast<int>(n & 8); }

}  // namespace

// ---- timeline ----------------------------------------------------------------------------------
// One row per DSP clock (32 per sample). Voice n's steps trail voice n-1's by three clocks;
// voice 0 wraps around the echo and housekeeping clocks at the end.
//
//   ReadSrcn     form the directory entry address from the SRCN read for this voice earlier,
//                and read SRCN ahead
//   ReadDir      read the directory entry (start or loop address), ADSR1, PITCHL
//   PitchHigh    PITCHH
//   ReadBrr      BRR header and first data byte
//   Sample       pitch modulation, key-on delay, interpolation/noise, envelope
//   Advance      decode the next 4 BRR samples if due, step the pitch counter, mix left
//   MixRight     mix right, update the voice's ENDX bit
//   OutxLatch / EndxEnvx / OutxWrite / EnvxWrite   OUTX/ENDX/ENVX register traffic
enum class DSP::Op : Byte {
  Next,
  ReadSrcn, ReadDir, PitchHigh, ReadBrr, Sample, Advance, MixRight, OutxLatch, EndxEnvx, OutxWrite, EnvxWrite,
  EchoReadL, EchoReadR, Fir, Feedback, Output, EchoOffLatch, EchoWriteL, EchoWriteR,
  PmonLatch, NonEonDirLatch, KonPoll, RateTick,
};

struct DSP::Slot { Op op; Byte voice; };

struct DspTimeline {
  using Op = DSP::Op;
  static constexpr DSP::Slot kSlots[] = {
    /* 0 */ {Op::MixRight, 0}, {Op::ReadDir, 1}, {Op::Next, 0},
    /* 1 */ {Op::OutxLatch, 0}, {Op::PitchHigh, 1}, {Op::ReadBrr, 1}, {Op::Sample, 1}, {Op::Next, 0},
    /* 2 */ {Op::EndxEnvx, 0}, {Op::Advance, 1}, {Op::ReadSrcn, 3}, {Op::Next, 0},
    /* 3 */ {Op::OutxWrite, 0}, {Op::MixRight, 1}, {Op::ReadDir, 2}, {Op::Next, 0},
    /* 4 */ {Op::EnvxWrite, 0}, {Op::OutxLatch, 1}, {Op::PitchHigh, 2}, {Op::ReadBrr, 2}, {Op::Sample, 2}, {Op::Next, 0},
    /* 5 */ {Op::EndxEnvx, 1}, {Op::Advance, 2}, {Op::ReadSrcn, 4}, {Op::Next, 0},
    /* 6 */ {Op::OutxWrite, 1}, {Op::MixRight, 2}, {Op::ReadDir, 3}, {Op::Next, 0},
    /* 7 */ {Op::EnvxWrite, 1}, {Op::OutxLatch, 2}, {Op::PitchHigh, 3}, {Op::ReadBrr, 3}, {Op::Sample, 3}, {Op::Next, 0},
    /* 8 */ {Op::EndxEnvx, 2}, {Op::Advance, 3}, {Op::ReadSrcn, 5}, {Op::Next, 0},
    /* 9 */ {Op::OutxWrite, 2}, {Op::MixRight, 3}, {Op::ReadDir, 4}, {Op::Next, 0},
    /*10 */ {Op::EnvxWrite, 2}, {Op::OutxLatch, 3}, {Op::PitchHigh, 4}, {Op::ReadBrr, 4}, {Op::Sample, 4}, {Op::Next, 0},
    /*11 */ {Op::EndxEnvx, 3}, {Op::Advance, 4}, {Op::ReadSrcn, 6}, {Op::Next, 0},
    /*12 */ {Op::OutxWrite, 3}, {Op::MixRight, 4}, {Op::ReadDir, 5}, {Op::Next, 0},
    /*13 */ {Op::EnvxWrite, 3}, {Op::OutxLatch, 4}, {Op::PitchHigh, 5}, {Op::ReadBrr, 5}, {Op::Sample, 5}, {Op::Next, 0},
    /*14 */ {Op::EndxEnvx, 4}, {Op::Advance, 5}, {Op::ReadSrcn, 7}, {Op::Next, 0},
    /*15 */ {Op::OutxWrite, 4}, {Op::MixRight, 5}, {Op::ReadDir, 6}, {Op::Next, 0},
    /*16 */ {Op::EnvxWrite, 4}, {Op::OutxLatch, 5}, {Op::PitchHigh, 6}, {Op::ReadBrr, 6}, {Op::Sample, 6}, {Op::Next, 0},
    /*17 */ {Op::ReadSrcn, 0}, {Op::EndxEnvx, 5}, {Op::Advance, 6}, {Op::Next, 0},
    /*18 */ {Op::OutxWrite, 5}, {Op::MixRight, 6}, {Op::ReadDir, 7}, {Op::Next, 0},
    /*19 */ {Op::EnvxWrite, 5}, {Op::OutxLatch, 6}, {Op::PitchHigh, 7}, {Op::ReadBrr, 7}, {Op::Sample, 7}, {Op::Next, 0},
    /*20 */ {Op::ReadSrcn, 1}, {Op::EndxEnvx, 6}, {Op::Advance, 7}, {Op::Next, 0},
    /*21 */ {Op::OutxWrite, 6}, {Op::MixRight, 7}, {Op::ReadDir, 0}, {Op::Next, 0},
    /*22 */ {Op::PitchHigh, 0}, {Op::EnvxWrite, 6}, {Op::OutxLatch, 7}, {Op::EchoReadL, 0}, {Op::Next, 0},
    /*23 */ {Op::EndxEnvx, 7}, {Op::EchoReadR, 0}, {Op::Next, 0},
    /*24 */ {Op::OutxWrite, 7}, {Op::Next, 0},
    /*25 */ {Op::ReadBrr, 0}, {Op::EnvxWrite, 7}, {Op::Fir, 0}, {Op::Next, 0},
    /*26 */ {Op::Feedback, 0}, {Op::Next, 0},
    /*27 */ {Op::PmonLatch, 0}, {Op::Output, 0}, {Op::Next, 0},
    /*28 */ {Op::NonEonDirLatch, 0}, {Op::EchoOffLatch, 0}, {Op::Next, 0},
    /*29 */ {Op::KonPoll, 0}, {Op::EchoWriteL, 0}, {Op::Next, 0},
    /*30 */ {Op::RateTick, 0}, {Op::Sample, 0}, {Op::EchoWriteR, 0}, {Op::Next, 0},
    /*31 */ {Op::Advance, 0}, {Op::ReadSrcn, 2}, {Op::Next, 0},
  };
};

DSP::DSP(std::array<Byte, 0x10000>& aram) : ram_(aram) { reset(); }

auto DSP::reset() -> void {
  regs_.fill(0);
  voices_ = {};
  rate_clock_ = 0;
  kon_poll_ = true;
  mvol_[0] = mvol_[1] = 0;
  mix_[0] = mix_[1] = 0;
  noise_rate_ = 0;
  noise_ = 0x4000;
  dir_ = 0;
  evol_[0] = evol_[1] = 0;
  efb_ = 0;
  fir_.fill(0);
  for (auto& channel : fir_buf_) std::fill(std::begin(channel), std::end(channel), 0);
  fir_pos_ = 0;
  esa_ = edl_ = esa_now_ = 0;
  fir_out_[0] = fir_out_[1] = echo_mix_[0] = echo_mix_[1] = 0;
  echo_ptr_ = 0;
  echo_index_ = echo_size_ = 0;
  bus_ = {};
  write(reg::kFlg, 0xE0);  // power-on FLG: soft reset, mute, echo writes off
  echo_off_now_ = true;
}

auto DSP::write(Byte addr, Byte value) -> void {
  addr &= 0x7F;
  regs_[addr] = value;
  const auto s8 = static_cast<int8_t>(value);
  auto per_voice = [&](bool Voice::*field, Byte mask) {
    for (int n = 0; n < 8; n++) voices_[static_cast<size_t>(n)].*field = ((value & mask) >> n) & 1;
  };

  switch (addr) {
    case reg::kMvolL: mvol_[0] = s8; break;
    case reg::kMvolR: mvol_[1] = s8; break;
    case reg::kEvolL: evol_[0] = s8; break;
    case reg::kEvolR: evol_[1] = s8; break;
    case reg::kKon: per_voice(&Voice::kon, 0xFF); per_voice(&Voice::kon_pending, 0xFF); break;
    case reg::kKoff: per_voice(&Voice::koff, 0xFF); break;
    case reg::kFlg:
      flg_reset_ = (value & 0x80) != 0;
      flg_mute_ = (value & 0x40) != 0;
      flg_echo_off_ = (value & 0x20) != 0;
      noise_rate_ = value & 0x1F;
      break;
    case reg::kEndx:  // writing anything acknowledges all end flags
      for (auto& v : voices_) v.ended = false;
      regs_[reg::kEndx] = 0;
      break;
    case reg::kEfb: efb_ = s8; break;
    case reg::kPmon: per_voice(&Voice::pmon, 0xFE); break;  // voice 0 has nothing to follow
    case reg::kNon: per_voice(&Voice::non, 0xFF); break;
    case reg::kEon: per_voice(&Voice::eon, 0xFF); break;
    case reg::kDir: dir_ = value; break;
    case reg::kEsa: esa_ = value; break;
    case reg::kEdl: edl_ = value & 0x0F; break;
    default: break;
  }

  Voice& v = voices_[addr >> 4];
  switch (addr & 0x0F) {
    case reg::kVolL: v.volume[0] = s8; break;
    case reg::kVolR: v.volume[1] = s8; break;
    case reg::kPitchL: v.pitch = static_cast<uint16_t>((v.pitch & 0x3F00) | value); break;
    case reg::kPitchH: v.pitch = static_cast<uint16_t>(((value & 0x3F) << 8) | (v.pitch & 0x00FF)); break;
    case reg::kSrcn: v.srcn = value; break;
    case reg::kAdsr1: v.adsr1 = value; break;
    case reg::kAdsr2: v.adsr2 = value; break;
    case reg::kGain: v.gain = value; break;
    case reg::kEnvx: bus_.envx = value; break;  // the next ENVX/OUTX store overwrites these
    case reg::kOutx: bus_.outx = value; break;
    case reg::kFir: fir_[addr >> 4] = s8; break;
    default: break;
  }
}

auto DSP::run_sample(std::vector<int16_t>& out) -> void {
  out_ = &out;
  for (const Slot& slot : DspTimeline::kSlots) {
    if (slot.op != Op::Next) run(slot.op, voices_[slot.voice], slot.voice);
  }
  out_ = nullptr;
}

auto DSP::run(Op op, Voice& v, int n) -> void {
  switch (op) {
    case Op::Next: break;
    case Op::ReadSrcn: read_srcn(v); break;
    case Op::ReadDir: read_directory(v); break;
    case Op::PitchHigh: bus_.pitch |= v.pitch & 0x3F00; break;
    case Op::ReadBrr: read_brr_header(v); break;
    case Op::Sample: produce_sample(v); break;
    case Op::Advance: advance_counter(v); break;
    case Op::MixRight: mix_voice(v, 1); publish_endx(v); break;
    case Op::OutxLatch: bus_.outx = static_cast<Byte>(bus_.sample >> 8); break;
    case Op::EndxEnvx: {
      Byte endx = 0;
      for (int i = 7; i >= 0; i--) endx = static_cast<Byte>((endx << 1) | (voices_[static_cast<size_t>(i)].ended ? 1 : 0));
      regs_[reg::kEndx] = endx;
      bus_.envx = v.envx;
      break;
    }
    case Op::OutxWrite: regs_[static_cast<size_t>((n << 4) | reg::kOutx)] = bus_.outx; break;
    case Op::EnvxWrite: regs_[static_cast<size_t>((n << 4) | reg::kEnvx)] = bus_.envx; break;
    case Op::EchoReadL:
      fir_pos_ = (fir_pos_ + 1) & 7;
      echo_ptr_ = static_cast<Word>((esa_now_ << 8) + echo_index_);
      read_echo(0);
      break;
    case Op::EchoReadR: read_echo(1); break;
    case Op::Fir: run_fir(); break;
    case Op::Feedback: echo_feedback(); break;
    case Op::Output: emit_frame(); break;
    case Op::EchoOffLatch: echo_off_now_ = flg_echo_off_; break;
    case Op::EchoWriteL:
      esa_now_ = esa_;
      step_echo_index();
      write_echo(0);
      echo_off_now_ = flg_echo_off_;
      break;
    case Op::EchoWriteR: write_echo(1); break;
    case Op::PmonLatch:
      for (auto& voice : voices_) voice.pmon_now = voice.pmon;
      break;
    case Op::NonEonDirLatch:
      for (auto& voice : voices_) {
        voice.non_now = voice.non;
        voice.eon_now = voice.eon;
      }
      bus_.dir = dir_;
      break;
    case Op::KonPoll:
      kon_poll_ = !kon_poll_;
      if (kon_poll_) {  // a KON that has been acted on is cleared (63 clocks after the poll)
        for (auto& voice : voices_) {
          if (voice.kon_now) voice.kon_pending = false;
        }
      }
      break;
    case Op::RateTick: tick_rate_clock(); break;
  }
}

auto DSP::read16(Word addr) const -> Word {
  return static_cast<Word>((ram_[static_cast<Word>(addr + 1)] << 8) | ram_[addr]);
}

// ---- building blocks ---------------------------------------------------------------------------

// Rates (fullsnes "ADSR/Gain (and Noise) Rates"; phase offsets from anomie's S-DSP notes).
auto DSP::counter_fires(uint16_t value, int rate) -> bool {
  if (rate <= 0) return false;
  const auto r = static_cast<size_t>(rate);
  return (value + kCounterOffset[r]) % kCounterRate[r] == 0;
}

// fullsnes "4-Point Gaussian Interpolation", on samples in their doubled 16-bit form (so the
// weights shift by 11 instead of 10). The first three products are summed with 16-bit wrap,
// the newest one with saturation; bit 0 of the result is always clear.
auto DSP::gaussian(int phase, int s0, int s1, int s2, int s3) -> int {
  const auto g = [](int index) { return static_cast<int>(kGaussTable[static_cast<size_t>(index)]); };
  const int i = phase & 0xFF;
  int sum = (g(0x0FF - i) * s0) >> 11;
  sum += (g(0x1FF - i) * s1) >> 11;
  sum += (g(0x100 + i) * s2) >> 11;
  sum = wrap16(sum) + ((g(i) * s3) >> 11);
  return clamp16(sum) & ~1;
}

auto DSP::decode_brr_block(const Byte* block, std::array<int16_t, 16>& out, int& prev1, int& prev2) -> void {
  const int shift = block[0] >> 4;
  const int filter = (block[0] >> 2) & 3;
  for (int i = 0; i < 16; i++) {
    const Byte pair = block[1 + i / 2];
    const int nibble = signed_nibble((i % 2 == 0) ? pair >> 4 : pair);
    const int16_t sample = brr_decode(nibble, shift, filter, prev1, prev2);
    out[static_cast<size_t>(i)] = sample;
    prev2 = prev1;
    prev1 = sample;
  }
}

// Four samples from the two data bytes at `data_offset` (the first was fetched by ReadBrr).
auto DSP::decode_four(Voice& v) -> void {
  const Byte second = ram_[static_cast<Word>(v.block + v.data_offset + 1)];
  const unsigned nibbles = (static_cast<unsigned>(bus_.data) << 8) | second;
  const int shift = bus_.header >> 4;
  const int filter = (bus_.header >> 2) & 3;
  for (int k = 3; k >= 0; k--) {
    const int old_s = v.ring[static_cast<size_t>((v.ring_pos + 11) % 12)];
    const int older_s = v.ring[static_cast<size_t>((v.ring_pos + 10) % 12)];
    v.ring[static_cast<size_t>(v.ring_pos)] = brr_decode(signed_nibble(nibbles >> (4 * k)), shift, filter, old_s, older_s);
    v.ring_pos = (v.ring_pos + 1) % 12;
  }
}

// fullsnes "ADSR/Gain Envelope". The level is computed every sample; it is only stored when
// the rate counter fires.
auto DSP::update_envelope(Voice& v) -> void {
  if (v.env_mode == EnvMode::Release) {  // rate 31, step -8
    v.envelope = std::max(v.envelope - 8, 0);
    return;
  }
  const auto exp_decrease = [](int level) { return level - (((level - 1) >> 8) + 1); };
  int level = v.envelope;
  int rate = 0;
  Byte boundary = 0;  // sustain level comes from bits 7-5 of ADSR2, or of GAIN in GAIN mode
  if (bus_.adsr1 & 0x80) {
    boundary = v.adsr2;
    switch (v.env_mode) {
      case EnvMode::Attack:
        rate = (bus_.adsr1 & 0x0F) * 2 + 1;
        level += (rate == 31) ? 0x400 : 0x20;
        break;
      case EnvMode::Decay:
        rate = ((bus_.adsr1 >> 4) & 0x07) * 2 + 16;
        level = exp_decrease(level);
        break;
      default:  // Sustain
        rate = v.adsr2 & 0x1F;
        level = exp_decrease(level);
        break;
    }
  } else {
    boundary = v.gain;
    if ((v.gain & 0x80) == 0) {  // direct
      level = (v.gain & 0x7F) * 16;
      rate = 31;
    } else {
      rate = v.gain & 0x1F;
      switch ((v.gain >> 5) & 3) {
        case 0: level -= 0x20; break;                     // linear decrease
        case 1: level = exp_decrease(level); break;        // exponential decrease
        case 2: level += 0x20; break;                     // linear increase
        case 3:                                           // bent line
          level += static_cast<unsigned>(v.raw_level) < 0x600 ? 0x20 : 0x08;
          break;
      }
    }
  }

  if (v.env_mode == EnvMode::Decay && (level >> 8) == (boundary >> 5)) v.env_mode = EnvMode::Sustain;
  v.raw_level = level;
  if (level < 0 || level > 0x7FF) {
    level = level < 0 ? 0 : 0x7FF;
    if (v.env_mode == EnvMode::Attack) v.env_mode = EnvMode::Decay;
  }
  if (counter_fires(rate_clock_, rate)) v.envelope = level;
}

// ---- voice steps -------------------------------------------------------------------------------

auto DSP::mix_voice(Voice& v, int channel) -> void {
  const int amplitude = (bus_.sample * v.volume[channel]) >> 7;
  mix_[channel] = clamp16(mix_[channel] + amplitude);
  if (v.eon_now) echo_mix_[channel] = clamp16(echo_mix_[channel] + amplitude);
}

auto DSP::read_srcn(Voice& v) -> void {
  bus_.entry = static_cast<Word>(bus_.dir * 0x100 + bus_.srcn * 4);
  bus_.srcn = v.srcn;
}

// Directory entry: bytes 0-1 start address (used while keying on), bytes 2-3 loop address.
auto DSP::read_directory(Voice& v) -> void {
  bus_.next_block = read16(static_cast<Word>(bus_.entry + (v.start_delay != 0 ? 0 : 2)));
  bus_.adsr1 = v.adsr1;
  bus_.pitch = v.pitch & 0x00FF;
}

auto DSP::read_brr_header(Voice& v) -> void {
  bus_.header = ram_[v.block];
  bus_.data = ram_[static_cast<Word>(v.block + v.data_offset)];
}

auto DSP::produce_sample(Voice& v) -> void {
  // Pitch modulation by the previous voice's output (fullsnes "BRR Pitch"):
  // step = step * ((prev >> 4) + 0x400) >> 10, with prev the 15-bit sample.
  if (v.pmon_now) bus_.pitch = (bus_.pitch * (((bus_.sample >> 1) >> 4) + 0x400)) >> 10;

  if (v.start_delay != 0) {
    if (v.start_delay == kKeyOnDelay) {  // jump to the sample start; this sample ignores the header
      v.block = bus_.next_block;
      v.data_offset = 1;
      v.ring_pos = 0;
      bus_.header = 0;
    }
    v.envelope = 0;
    v.raw_level = 0;
    v.start_delay--;
    // Decoding is held off for the first two delay samples, then fills the ring for three.
    v.counter = (v.start_delay % 4 != 0) ? 0x4000 : 0;
    bus_.pitch = 0;
  }

  int sample = 0;
  if (v.non_now) {
    sample = wrap16(noise_ << 1);
  } else {
    int at = (v.ring_pos + (v.counter >> 12)) % 12;
    int taps[4];
    for (int& tap : taps) {
      tap = v.ring[static_cast<size_t>(at)];
      at = (at + 1) % 12;
    }
    sample = gaussian((v.counter >> 4) & 0xFF, taps[0], taps[1], taps[2], taps[3]);
  }
  bus_.sample = ((sample * v.envelope) >> 11) & ~1;
  v.envx = static_cast<Byte>(v.envelope >> 4);

  // Soft reset, or a block with end code 1 (end without loop), silences the voice at once.
  if (flg_reset_ || (bus_.header & 0x03) == 0x01) {
    v.env_mode = EnvMode::Release;
    v.envelope = 0;
  }
  if (kon_poll_) {
    if (v.koff_now) v.env_mode = EnvMode::Release;
    if (v.kon_now) {
      v.start_delay = kKeyOnDelay;
      v.env_mode = EnvMode::Attack;
    }
  }
  if (v.start_delay == 0) update_envelope(v);
}

// Every 0x4000 of pitch counter consumes four samples: decode the next data byte pair, and at
// the end of the block continue with the next one (or the loop address for end codes 1/3).
auto DSP::advance_counter(Voice& v) -> void {
  v.wrapped = false;
  if (v.counter >= 0x4000) {
    decode_four(v);
    v.data_offset += 2;
    if (v.data_offset >= kBlockBytes) {
      v.data_offset = 1;
      if (bus_.header & 0x01) {
        v.block = bus_.next_block;
        v.wrapped = true;
      } else {
        v.block = static_cast<Word>(v.block + kBlockBytes);
      }
    }
  }
  v.counter = std::min((v.counter & 0x3FFF) + bus_.pitch, 0x7FFF);
  mix_voice(v, 0);
}

auto DSP::publish_endx(Voice& v) -> void {
  if (v.wrapped) v.ended = true;
  if (v.start_delay == kKeyOnDelay) v.ended = false;  // keying on clears the voice's ENDX bit
}

// ---- echo --------------------------------------------------------------------------------------
// fullsnes "Echo Registers": 15-bit samples go through an 8-tap FIR (oldest tap = FIR0); the
// output is mixed into the speakers with EVOL and fed back into the echo buffer with EFB.

auto DSP::fir_tap(int channel, int tap) const -> int {
  return (fir_buf_[channel][(fir_pos_ + tap + 1) & 7] * fir_[static_cast<size_t>(tap)]) >> 6;
}

auto DSP::output_level(int channel) const -> int {
  const int dry = wrap16((mix_[channel] * mvol_[channel]) >> 7);
  const int wet = wrap16((fir_out_[channel] * evol_[channel]) >> 7);
  return clamp16(dry + wet);
}

auto DSP::read_echo(int channel) -> void {
  const auto stored = static_cast<int16_t>(read16(static_cast<Word>(echo_ptr_ + 2 * channel)));
  fir_buf_[channel][fir_pos_] = stored >> 1;
}

auto DSP::write_echo(int channel) -> void {
  if (!echo_off_now_) {
    const Word at = static_cast<Word>(echo_ptr_ + 2 * channel);
    ram_[at] = static_cast<Byte>(echo_mix_[channel] & 0xFF);
    ram_[static_cast<Word>(at + 1)] = static_cast<Byte>((echo_mix_[channel] >> 8) & 0xFF);
  }
  echo_mix_[channel] = 0;
}

// The first seven taps add with 16-bit wrap, the newest saturates.
auto DSP::run_fir() -> void {
  for (int c = 0; c < 2; c++) {
    int sum = 0;
    for (int tap = 0; tap < 7; tap++) sum += fir_tap(c, tap);
    sum = wrap16(sum) + wrap16(fir_tap(c, 7));
    fir_out_[c] = clamp16(sum) & ~1;
  }
}

auto DSP::echo_feedback() -> void {
  mix_[0] = output_level(0);  // the left output is formed a clock before the right one
  for (int c = 0; c < 2; c++) {
    const int fed = echo_mix_[c] + wrap16((fir_out_[c] * efb_) >> 7);
    echo_mix_[c] = clamp16(fed) & ~1;
  }
}

auto DSP::emit_frame() -> void {
  const int left = flg_mute_ ? 0 : mix_[0];
  const int right = flg_mute_ ? 0 : output_level(1);
  mix_[0] = 0;
  mix_[1] = 0;
  if (out_ != nullptr) {
    out_->push_back(static_cast<int16_t>(left));
    out_->push_back(static_cast<int16_t>(right));
  }
}

// The buffer length is reloaded from EDL only when the index wraps to 0 (fullsnes "Echo
// Buffer Notes"); EDL = 0 still uses one 4-byte entry.
auto DSP::step_echo_index() -> void {
  if (echo_index_ == 0) echo_size_ = edl_ * kEchoUnitBytes;
  echo_index_ += 4;
  if (echo_index_ >= echo_size_) echo_index_ = 0;
}

// Housekeeping at the end of the sample: latch KON/KOFF on polling samples, step the shared
// rate counter and the noise generator (fullsnes "NON": bit 14 = bit 0 xor bit 1).
auto DSP::tick_rate_clock() -> void {
  if (kon_poll_) {
    for (auto& v : voices_) {
      v.kon_now = v.kon_pending;
      v.koff_now = v.koff;
    }
  }
  rate_clock_ = static_cast<uint16_t>((rate_clock_ == 0 ? kCounterRange : rate_clock_) - 1);
  if (counter_fires(rate_clock_, noise_rate_)) {
    const int feedback = (noise_ ^ (noise_ >> 1)) & 1;
    noise_ = (feedback << 14) | (noise_ >> 1);
  }
}

} // namespace bitforge::apu
