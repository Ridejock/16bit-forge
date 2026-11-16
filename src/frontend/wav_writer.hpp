// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace snes::frontend {

// Streams 16-bit stereo PCM to a .wav file. The RIFF sizes are patched on every flush and on
// close, so a recording cut short by a crash or a debugger stop still opens in an audio editor.
class WavWriter {
public:
    WavWriter() = default;
    WavWriter(const WavWriter&) = delete;
    auto operator=(const WavWriter&) -> WavWriter& = delete;
    ~WavWriter() { close(); }

    auto open(const std::string& path, int sample_rate) -> bool;
    auto close() -> void;
    auto is_open() const -> bool { return file_ != nullptr; }
    // Interleaved L, R frames.
    auto write(std::span<const int16_t> stereo) -> void;
    auto flush() -> void;

    auto path() const -> const std::string& { return path_; }
    auto sample_rate() const -> int { return rate_; }
    auto seconds() const -> double { return rate_ > 0 ? static_cast<double>(frames_) / rate_ : 0.0; }

private:
    auto patch_sizes() -> void;

    std::FILE* file_ = nullptr;
    std::string path_;
    int rate_ = 0;
    uint64_t frames_ = 0;
};

} // namespace snes::frontend
