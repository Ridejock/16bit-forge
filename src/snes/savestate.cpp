// SPDX-License-Identifier: MIT
#include "snes/savestate.hpp"
#include "snes/serialize.hpp"
#include "snes/system.hpp"

namespace snes::savestate {

namespace {

constexpr int kMaxThumbSide = 512;

// The fixed part of the file, shared by save and load.
auto header(Archive& ar, Info& info) -> bool {
    std::array<char, 8> magic = kMagic;
    ar(magic);
    if (!ar.ok() || magic != kMagic) return false;
    ar(info.version);
    if (!ar.ok() || info.version < kMinVersion || info.version > kVersion) return ar.ok();  // caller reports the version
    uint16_t w = static_cast<uint16_t>(info.thumb_w);
    uint16_t h = static_cast<uint16_t>(info.thumb_h);
    ar(info.rom_crc, info.frame, info.timestamp, w, h);
    if (!ar.ok() || w > kMaxThumbSide || h > kMaxThumbSide) return false;
    info.thumb_w = w;
    info.thumb_h = h;
    info.thumbnail.resize(static_cast<size_t>(w) * h);
    ar.bytes(info.thumbnail.data(), info.thumbnail.size() * sizeof(uint32_t));
    return ar.ok();
}

auto read_header(Archive& ar, Info& info) -> Result {
    if (!header(ar, info)) return Result::NotAState;
    if (info.version < kMinVersion || info.version > kVersion) return Result::WrongVersion;
    return Result::Ok;
}

} // namespace

auto describe(Result result) -> const char* {
    switch (result) {
    case Result::Ok: return "ok";
    case Result::NotAState: return "not a save state file (or it is damaged)";
    case Result::WrongVersion: return "it was made by a different version of the emulator";
    case Result::WrongRom: return "it was saved from a different ROM";
    case Result::Corrupt: return "the file is truncated or damaged";
    }
    return "unknown error";
}

auto make_thumbnail(const uint32_t* pixels, int width, int height, Info& info) -> void {
    const int sx = width > 256 ? 4 : 2;
    const int sy = height > 239 ? 4 : 2;
    info.thumb_w = width / sx;
    info.thumb_h = height / sy;
    info.thumbnail.assign(static_cast<size_t>(info.thumb_w) * static_cast<size_t>(info.thumb_h), 0);
    for (int y = 0; y < info.thumb_h; y++) {
        for (int x = 0; x < info.thumb_w; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int dy = 0; dy < sy; dy++) {
                for (int dx = 0; dx < sx; dx++) {
                    const uint32_t p = pixels[static_cast<size_t>((y * sy + dy) * width + x * sx + dx)];
                    r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF;
                    b += p & 0xFF;
                }
            }
            const auto n = static_cast<uint32_t>(sx * sy);
            info.thumbnail[static_cast<size_t>(y * info.thumb_w + x)] = 0xFF000000 | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
}

auto encode(System& system, uint64_t frame, int64_t timestamp, const uint32_t* pixels, int width, int height,
            uint32_t version) -> std::vector<uint8_t> {
    Info info;
    info.version = version;
    info.rom_crc = system.rom_crc32();
    info.frame = frame;
    info.timestamp = timestamp;
    if (pixels && width > 0 && height > 0) make_thumbnail(pixels, width, height, info);

    Archive ar = Archive::saver();
    header(ar, info);
    std::vector<uint8_t> state = system.save_state(version == kVersion ? 0 : version);
    auto size = static_cast<uint32_t>(state.size());
    ar(size);
    ar.bytes(state.data(), state.size());
    return ar.data();
}

auto read_info(std::span<const uint8_t> file, Info& info) -> Result {
    Archive ar = Archive::loader(file);
    return read_header(ar, info);
}

auto decode(System& system, std::span<const uint8_t> file, Info& info) -> Result {
    Archive ar = Archive::loader(file);
    if (const Result r = read_header(ar, info); r != Result::Ok) return r;
    if (info.rom_crc != system.rom_crc32()) return Result::WrongRom;
    uint32_t size = 0;
    ar(size);
    if (!ar.ok() || size != ar.remaining()) return Result::Corrupt;
    return system.load_state(file.subspan(file.size() - size), info.version) ? Result::Ok : Result::Corrupt;
}

} // namespace snes::savestate
