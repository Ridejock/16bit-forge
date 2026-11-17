// SPDX-License-Identifier: MIT
#include "frontend/wav_writer.hpp"

#include <algorithm>

namespace bitforge::frontend {

namespace {

constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * 2;
constexpr uint32_t kHeaderBytes = 44;
constexpr uint64_t kMaxDataBytes = 0xFFFFFFFFull - kHeaderBytes;  // RIFF sizes are 32-bit

auto put_le(std::FILE* f, uint32_t v, int bytes) -> void {
    for (int i = 0; i < bytes; i++) std::fputc(static_cast<int>((v >> (8 * i)) & 0xFF), f);
}

} // namespace

auto WavWriter::open(const std::string& path, int sample_rate) -> bool {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;
    path_ = path;
    rate_ = sample_rate;
    frames_ = 0;
    std::fwrite("RIFF", 1, 4, file_);
    put_le(file_, kHeaderBytes - 8, 4);
    std::fwrite("WAVEfmt ", 1, 8, file_);
    put_le(file_, 16, 4);                                            // fmt chunk size
    put_le(file_, 1, 2);                                             // PCM
    put_le(file_, kChannels, 2);
    put_le(file_, static_cast<uint32_t>(sample_rate), 4);
    put_le(file_, static_cast<uint32_t>(sample_rate * kBytesPerFrame), 4);
    put_le(file_, kBytesPerFrame, 2);
    put_le(file_, 16, 2);                                            // bits per sample
    std::fwrite("data", 1, 4, file_);
    put_le(file_, 0, 4);
    return true;
}

auto WavWriter::write(std::span<const int16_t> stereo) -> void {
    if (!file_ || stereo.empty()) return;
    if ((frames_ + stereo.size() / kChannels) * kBytesPerFrame > kMaxDataBytes) return;  // ~6.7 h at 44.1 kHz
    for (const int16_t s : stereo) put_le(file_, static_cast<uint16_t>(s), 2);  // little-endian on any host
    frames_ += stereo.size() / kChannels;
}

auto WavWriter::patch_sizes() -> void {
    const auto data_bytes = static_cast<uint32_t>(frames_ * kBytesPerFrame);
    const long end = std::ftell(file_);
    std::fseek(file_, 4, SEEK_SET);
    put_le(file_, kHeaderBytes - 8 + data_bytes, 4);
    std::fseek(file_, 40, SEEK_SET);
    put_le(file_, data_bytes, 4);
    std::fseek(file_, end, SEEK_SET);
}

auto WavWriter::flush() -> void {
    if (!file_) return;
    patch_sizes();
    std::fflush(file_);
}

auto WavWriter::close() -> void {
    if (!file_) return;
    patch_sizes();
    std::fclose(file_);
    file_ = nullptr;
}

} // namespace bitforge::frontend
