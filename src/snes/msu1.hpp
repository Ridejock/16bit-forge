// SPDX-License-Identifier: MIT
#pragma once
// MSU-1: streamed data and CD-quality audio for ROM hacks and homebrew, following the
// public MSU-1 specification (file names, ports $2000-$2007, PCM format). It's enabled when <rom>.msu sits beside the ROM:
//
//   <rom>.msu      data file, read sequentially through $2001
//   <rom>-N.pcm    audio track N: "MSU1", a uint32 loop point (in samples), then 44.1 kHz
//                  16-bit little-endian stereo
//
// Registers (banks $00-$3F/$80-$BF):
//   read  $2000 status   $80 data busy, $40 audio busy, $20 repeating, $10 playing,
//                        $08 track missing, $07 revision (2)
//         $2001 data     the next data byte
//         $2002-$2007    "S-MSU1"
//   write $2000-$2003    data seek offset (little-endian, seeks on the $2003 write)
//         $2004-$2005    track number (loads on the $2005 write; stops playback)
//         $2006          volume (0-255)
//         $2007          control: bit 0 play, bit 1 repeat (ignored while the track is missing)
//
// Files open instantly here, so the busy bits always read 0. Audio is rendered once per frame
// at the APU's sample rate (linear interpolation from 44.1 kHz) so it lines up sample for sample
// with the DSP output and the frontend mixes the two.
#include "snes/serialize.hpp"
#include "snes/types.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string>

namespace bitforge {

class Msu1 {
public:
  static constexpr int kSampleRate = 44100;
  static constexpr Byte kRevision = 0x02;
  enum Status : Byte { kTrackMissing = 0x08, kPlaying = 0x10, kRepeating = 0x20, kAudioBusy = 0x40, kDataBusy = 0x80 };

  // An MSU-1 when <rom>.msu exists next to `rom_path`, else null.
  static auto detect(const std::string& rom_path) -> std::unique_ptr<Msu1>;
  // `base` is the path without extension: base + ".msu", base + "-N.pcm".
  explicit Msu1(std::string base);

  auto reset() -> void;
  auto read(Word offset) -> Byte;               // $2000-$2007
  auto write(Word offset, Byte value) -> void;  // $2000-$2007

  // Fills `stereo` (L, R, ...) with the next stereo.size() / 2 frames at `rate` Hz.
  auto render(std::span<int16_t> stereo, int rate) -> void;

  auto status() const -> Byte;
  auto track() const -> uint16_t { return track_; }
  auto data_path() const -> std::string { return base_ + ".msu"; }
  auto track_path(uint16_t track) const -> std::string { return base_ + "-" + std::to_string(track) + ".pcm"; }

  auto serialize(Archive& ar) -> void;

private:
  auto load_track() -> void;
  auto open_audio() -> bool;    // (re)opens the current track's file at audio_offset_
  auto next_frame() -> void;    // shifts the interpolation window by one 44.1 kHz frame

  std::string base_;
  std::ifstream data_;
  std::ifstream audio_;
  uint64_t data_size_ = 0;
  uint64_t data_pos_ = UINT64_MAX;  // where the data stream is (UINT64_MAX: unknown)
  int open_track_ = -1;             // the track audio_ has open (-1: none)

  // State.
  uint32_t data_offset_ = 0;
  uint32_t seek_latch_ = 0;
  uint16_t track_ = 0;
  uint16_t track_latch_ = 0;
  Byte volume_ = 0xFF;
  bool playing_ = false;
  bool repeat_ = false;
  bool missing_ = false;
  uint32_t audio_offset_ = 0;  // byte offset of the next frame in the track file
  uint32_t audio_size_ = 0;
  uint32_t loop_offset_ = 8;
  int16_t prev_[2] = {0, 0};   // the 44.1 kHz frames either side of the output position
  int16_t next_[2] = {0, 0};
  uint32_t phase_ = 0;         // output position between them, in 1/rate steps
};

} // namespace bitforge
