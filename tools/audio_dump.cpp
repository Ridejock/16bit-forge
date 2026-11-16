// SPDX-License-Identifier: MIT
// Headless audio render: runs a ROM (.sfc/.smc) or an SPC snapshot (.spc) for a few seconds and
// writes the APU output to a 16-bit stereo WAV at the native 32040 Hz, then prints simple
// checks (peak, RMS, clipping, largest sample-to-sample jump, samples per video frame).
//   audio_dump <rom.sfc|song.spc> <out.wav> [seconds=8] [skip_seconds=0]
#include "snes/apu/apu.hpp"
#include "snes/system.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int kRate = snes::apu::APU::kSampleRate;
constexpr double kFrameRate = 60.0988;

auto put32(std::ofstream& out, uint32_t v) -> void {
  const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
  out.write(b, 4);
}
auto put16(std::ofstream& out, uint16_t v) -> void {
  const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
  out.write(b, 2);
}

auto write_wav(const std::string& path, const std::vector<int16_t>& pcm) -> bool {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  const auto bytes = static_cast<uint32_t>(pcm.size() * 2);
  out.write("RIFF", 4);
  put32(out, 36 + bytes);
  out.write("WAVEfmt ", 8);
  put32(out, 16);
  put16(out, 1);  // PCM
  put16(out, 2);  // stereo
  put32(out, kRate);
  put32(out, kRate * 4);
  put16(out, 4);
  put16(out, 16);
  out.write("data", 4);
  put32(out, bytes);
  for (int16_t s : pcm) put16(out, static_cast<uint16_t>(s));
  return static_cast<bool>(out);
}

auto has_ext(const std::string& path, const char* ext) -> bool {
  const std::string e(ext);
  if (path.size() < e.size()) return false;
  std::string tail = path.substr(path.size() - e.size());
  std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return tail == e;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <rom.sfc|song.spc> <out.wav> [seconds=8] [skip_seconds=0]\n", argv[0]);
    return 2;
  }
  const double seconds = argc > 3 ? std::atof(argv[3]) : 8.0;
  const double skip = argc > 4 ? std::atof(argv[4]) : 0.0;
  snes::utils::Logger::set_level(snes::utils::LogLevel::Warning);

  std::vector<int16_t> pcm;
  int min_per_frame = 1 << 30, max_per_frame = 0;
  const auto keep_from = static_cast<size_t>(skip * kRate) * 2;
  const auto total = static_cast<size_t>((skip + seconds) * kRate) * 2;

  if (has_ext(argv[1], ".spc")) {
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<snes::Byte> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    snes::apu::APU apu;
    if (!apu.load_spc(data.data(), data.size())) {
      std::fprintf(stderr, "not an SPC file: %s\n", argv[1]);
      return 1;
    }
    while (pcm.size() < total) {
      apu.clear_samples();
      apu.run_smp_cycles(snes::apu::APU::kSmpCyclesPerSample * 512);
      pcm.insert(pcm.end(), apu.samples().begin(), apu.samples().end());
    }
  } else {
    snes::System sys;
    if (!sys.load_rom(argv[1])) return 1;
    sys.reset();
    while (pcm.size() < total) {
      sys.run_frame();
      const auto& s = sys.audio_samples();
      const int frames = static_cast<int>(s.size() / 2);
      min_per_frame = std::min(min_per_frame, frames);
      max_per_frame = std::max(max_per_frame, frames);
      pcm.insert(pcm.end(), s.begin(), s.end());
    }
  }
  pcm.resize(total);
  pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(keep_from));

  // Checks
  int peak = 0;
  size_t clipped = 0, nonzero = 0;
  int max_jump = 0;
  double sum_sq = 0;
  for (size_t i = 0; i < pcm.size(); i++) {
    const int s = pcm[i];
    peak = std::max(peak, std::abs(s));
    if (s >= 32767 || s <= -32768) clipped++;
    if (s != 0) nonzero++;
    sum_sq += static_cast<double>(s) * s;
    if (i >= 2) max_jump = std::max(max_jump, std::abs(s - pcm[i - 2]));  // same channel
  }
  const double rms = std::sqrt(sum_sq / static_cast<double>(std::max<size_t>(1, pcm.size())));
  std::printf("frames=%zu (%.2f s @ %d Hz) peak=%d (%.1f dBFS) rms=%.1f nonzero=%.1f%% clipped=%zu max_jump=%d",
              pcm.size() / 2, static_cast<double>(pcm.size() / 2) / kRate, kRate, peak,
              20.0 * std::log10(std::max(1, peak) / 32768.0), rms, 100.0 * static_cast<double>(nonzero) / static_cast<double>(std::max<size_t>(1, pcm.size())),
              clipped, max_jump);
  if (max_per_frame > 0) {
    std::printf(" samples/frame=%d..%d (expect %.1f)", min_per_frame, max_per_frame, kRate / kFrameRate);
  }
  std::printf("\n");
  if (!write_wav(argv[2], pcm)) {
    std::fprintf(stderr, "could not write %s\n", argv[2]);
    return 1;
  }
  return 0;
}
