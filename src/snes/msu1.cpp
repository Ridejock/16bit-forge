// SPDX-License-Identifier: MIT
#include "snes/msu1.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <filesystem>

namespace snes {

namespace {
constexpr char kId[] = "S-MSU1";
constexpr uint32_t kHeaderSize = 8;  // "MSU1" + loop point
}  // namespace

auto Msu1::detect(const std::string& rom_path) -> std::unique_ptr<Msu1> {
  namespace fs = std::filesystem;
  fs::path base = fs::path(rom_path);
  base.replace_extension();
  std::error_code ec;
  if (!fs::is_regular_file(base.string() + ".msu", ec)) return nullptr;
  utils::Logger::info(utils::LogChannel::Rom, "MSU-1: %s.msu", base.filename().string().c_str());
  return std::make_unique<Msu1>(base.string());
}

Msu1::Msu1(std::string base) : base_(std::move(base)) {
  data_.open(data_path(), std::ios::binary);
  std::error_code ec;
  data_size_ = data_ ? std::filesystem::file_size(data_path(), ec) : 0;
  if (ec) data_size_ = 0;
  reset();
}

auto Msu1::reset() -> void {
  data_offset_ = seek_latch_ = 0;
  track_ = track_latch_ = 0;
  volume_ = 0xFF;
  playing_ = repeat_ = missing_ = false;
  audio_offset_ = audio_size_ = 0;
  loop_offset_ = kHeaderSize;
  prev_[0] = prev_[1] = next_[0] = next_[1] = 0;
  phase_ = 0;
}

auto Msu1::status() const -> Byte {
  return static_cast<Byte>((missing_ ? kTrackMissing : 0) | (playing_ ? kPlaying : 0) | (repeat_ ? kRepeating : 0) | kRevision);
}

auto Msu1::read(Word offset) -> Byte {
  switch (offset & 7) {
    case 0: return status();
    case 1: {
      if (data_offset_ >= data_size_) {
        data_offset_++;
        return 0x00;
      }
      if (data_pos_ != data_offset_) {
        data_.clear();
        data_.seekg(data_offset_);
      }
      const int c = data_.get();
      data_offset_++;
      data_pos_ = data_ ? data_offset_ : UINT64_MAX;
      return c == std::char_traits<char>::eof() ? 0x00 : static_cast<Byte>(c);
    }
    default: return static_cast<Byte>(kId[(offset & 7) - 2]);
  }
}

auto Msu1::write(Word offset, Byte value) -> void {
  switch (offset & 7) {
    case 0: case 1: case 2: case 3: {
      const int shift = (offset & 3) * 8;
      seek_latch_ = (seek_latch_ & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
      if ((offset & 3) == 3) data_offset_ = seek_latch_;
      break;
    }
    case 4: track_latch_ = static_cast<uint16_t>((track_latch_ & 0xFF00) | value); break;
    case 5:
      track_latch_ = static_cast<uint16_t>((track_latch_ & 0x00FF) | (value << 8));
      track_ = track_latch_;
      load_track();
      break;
    case 6: volume_ = value; break;
    case 7:
      if (missing_) break;
      playing_ = (value & 0x01) != 0;
      repeat_ = (value & 0x02) != 0;
      break;
  }
}

// A track select stops playback and rewinds to the first sample; a missing or malformed file
// sets the track-missing bit.
auto Msu1::load_track() -> void {
  playing_ = repeat_ = false;
  audio_offset_ = kHeaderSize;
  audio_size_ = 0;
  loop_offset_ = kHeaderSize;
  open_track_ = -1;
  missing_ = !open_audio();
  if (missing_) {
    utils::Logger::warning(utils::LogChannel::Rom, "MSU-1: track %u not found (%s)", track_, track_path(track_).c_str());
    return;
  }
  audio_.seekg(4);
  uint8_t loop[4] = {};
  audio_.read(reinterpret_cast<char*>(loop), 4);
  const uint32_t samples = loop[0] | loop[1] << 8 | loop[2] << 16 | static_cast<uint32_t>(loop[3]) << 24;
  loop_offset_ = kHeaderSize + samples * 4;
  if (samples >= (audio_size_ - kHeaderSize) / 4) loop_offset_ = kHeaderSize;
  audio_.seekg(audio_offset_);
}

auto Msu1::open_audio() -> bool {
  if (open_track_ == track_) return true;
  audio_.close();
  audio_.clear();
  open_track_ = -1;
  const std::string path = track_path(track_);
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size < kHeaderSize || size > UINT32_MAX) return false;
  audio_.open(path, std::ios::binary);
  char magic[4] = {};
  if (!audio_.read(magic, 4) || std::string_view(magic, 4) != "MSU1") {
    audio_.close();
    return false;
  }
  audio_size_ = static_cast<uint32_t>(size);
  open_track_ = track_;
  audio_.seekg(audio_offset_);
  return true;
}

auto Msu1::next_frame() -> void {
  prev_[0] = next_[0];
  prev_[1] = next_[1];
  next_[0] = next_[1] = 0;
  if (!playing_ || missing_) return;
  if (audio_offset_ + 4 > audio_size_) {  // past the end: loop or stop
    if (repeat_) {
      audio_offset_ = loop_offset_;
      audio_.clear();
      audio_.seekg(audio_offset_);
    } else {
      playing_ = false;
      audio_offset_ = kHeaderSize;
      audio_.clear();
      audio_.seekg(audio_offset_);
      return;
    }
    if (audio_offset_ + 4 > audio_size_) return;  // an empty track
  }
  uint8_t s[4] = {};
  audio_.read(reinterpret_cast<char*>(s), 4);
  audio_offset_ += 4;
  next_[0] = static_cast<int16_t>(s[0] | s[1] << 8);
  next_[1] = static_cast<int16_t>(s[2] | s[3] << 8);
}

auto Msu1::render(std::span<int16_t> stereo, int rate) -> void {
  if (playing_ && !missing_ && !open_audio()) missing_ = true;  // e.g. after a state load
  for (size_t i = 0; i + 1 < stereo.size(); i += 2) {
    for (int c = 0; c < 2; c++) {
      const int32_t v = prev_[c] + static_cast<int32_t>(static_cast<int64_t>(next_[c] - prev_[c]) * phase_ / static_cast<uint32_t>(rate));
      stereo[i + static_cast<size_t>(c)] = static_cast<int16_t>(v * volume_ / 255);
    }
    phase_ += kSampleRate;
    while (phase_ >= static_cast<uint32_t>(rate)) {
      phase_ -= static_cast<uint32_t>(rate);
      next_frame();
    }
  }
}

auto Msu1::serialize(Archive& ar) -> void {
  ar(data_offset_, seek_latch_, track_, track_latch_, volume_, playing_, repeat_, missing_, audio_offset_, audio_size_,
     loop_offset_, prev_, next_, phase_);
  if (ar.loading()) {
    open_track_ = -1;      // reopened (and seeked) at the next render
    data_pos_ = UINT64_MAX;
  }
}

} // namespace snes
