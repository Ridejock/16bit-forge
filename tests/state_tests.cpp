// SPDX-License-Identifier: MIT
// Save states, battery RAM and the controller ports.
//
//   state_tests [extra.sfc ...]
//
// Determinism: for the built-in test ROMs (tests/test_rom.hpp), every ROM in SNES_TEST_ROMS_DIR
// and any given on the command line, run a while with
// scripted input on both pads, save a state, run N frames hashing the picture and the audio,
// load the state, run the same N frames again: both hashes must match, and saving again right
// after the load must give the same bytes.
#include "frontend/emulator.hpp"
#include "frontend/rewind.hpp"
#include "snes/memory/bus.hpp"
#include "test_rom.hpp"
#include "snes/savestate.hpp"
#include "snes/serialize.hpp"
#include "snes/system.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

constexpr int kWarmupFrames = 150;
constexpr int kRunFrames = 120;

struct Hash {
    uint64_t value = 0xCBF29CE484222325ull;  // FNV-1a
    auto add(const void* data, size_t size) -> void {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) value = (value ^ p[i]) * 0x100000001B3ull;
    }
};

// Pads change every few frames so input-driven ROMs actually move.
auto scripted_pad(int port, int frame) -> uint16_t {
    const uint32_t x = static_cast<uint32_t>(frame / 8 + port * 7) * 2654435761u;
    return static_cast<uint16_t>((x >> 12) & 0xFFF0);
}

struct RunResult {
    uint64_t video = 0;
    uint64_t audio = 0;
    size_t nonzero_audio = 0;
};

auto run(snes::System& sys, int first_frame, int frames) -> RunResult {
    Hash video;
    Hash audio;
    RunResult r;
    for (int f = 0; f < frames; f++) {
        sys.set_port_connected(1, true);
        sys.set_joypad_state(0, scripted_pad(0, first_frame + f));
        sys.set_joypad_state(1, scripted_pad(1, first_frame + f));
        sys.run_frame();
        const auto [w, h] = sys.get_framebuffer_size();
        video.add(&w, sizeof(w));
        video.add(&h, sizeof(h));
        video.add(sys.get_framebuffer(), static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint32_t));
        const auto& samples = sys.audio_samples();
        audio.add(samples.data(), samples.size() * sizeof(int16_t));
        r.nonzero_audio += static_cast<size_t>(std::count_if(samples.begin(), samples.end(), [](int16_t s) { return s != 0; }));
    }
    r.video = video.value;
    r.audio = audio.value;
    return r;
}

auto test_archive() -> void {
    struct Inner {
        int16_t a = -2;
        bool b = true;
        auto serialize(snes::Archive& ar) -> void { ar(a, b); }
    };
    enum class Color : uint8_t { Red, Green };
    uint32_t u = 0xDEADBEEF;
    int8_t c_array[3] = {1, -2, 3};
    std::array<uint8_t, 4> bytes = {9, 8, 7, 6};
    std::array<Inner, 2> inner{};
    inner[1].a = 1234;
    Color color = Color::Green;
    uint64_t big = 0x0123456789ABCDEFull;

    snes::Archive out = snes::Archive::saver();
    out(u, c_array, bytes, inner, color, big);
    CHECK(out.data().size() == 4 + 3 + 4 + 2 * 3 + 1 + 8);

    uint32_t u2 = 0;
    int8_t c2[3] = {};
    std::array<uint8_t, 4> bytes2{};
    std::array<Inner, 2> inner2{};
    Color color2 = Color::Red;
    uint64_t big2 = 0;
    snes::Archive in = snes::Archive::loader(out.data());
    in(u2, c2, bytes2, inner2, color2, big2);
    CHECK(in.ok() && in.remaining() == 0);
    CHECK(u2 == u && c2[1] == -2 && bytes2 == bytes && inner2[1].a == 1234 && inner2[0].b && color2 == Color::Green && big2 == big);

    // Reading past the end fails and doesn't touch the value.
    std::vector<uint8_t> short_data(out.data().begin(), out.data().begin() + 2);
    snes::Archive truncated = snes::Archive::loader(short_data);
    uint32_t untouched = 7;
    truncated(untouched);
    CHECK(!truncated.ok() && untouched == 7);

    const char* digits = "123456789";
    CHECK(snes::crc32({reinterpret_cast<const uint8_t*>(digits), 9}) == 0xCBF43926u);
}

// $4016/$4017 serial reads and the auto-joypad registers, on the bus alone.
auto test_joypad_ports() -> void {
    snes::memory::Bus bus;
    bus.load_test_rom(std::vector<snes::Byte>(0x8000, 0xEA));
    bus.reset();
    const uint16_t pad1 = 0x8000 | 0x0800 | 0x0080;  // B, Up, A
    const uint16_t pad2 = 0x4000 | 0x0100 | 0x0010;  // Y, Right, R
    bus.write(0x004200, 0x00);  // no auto-read yet

    // Pad 2 starts unplugged: every $4017 bit reads 0 (no trailing 1s) and $421A stays 0.
    bus.set_joypad(1, pad2);
    bus.write(0x004016, 1);
    bus.write(0x004016, 0);
    for (int i = 0; i < 20; i++) CHECK(bus.read(0x004017) == 0x1C);
    CHECK(bus.read(0x00421A) == 0 && bus.read(0x00421B) == 0);
    bus.set_port_connected(1, true);

    bus.set_joypad(0, pad1);
    bus.set_joypad(1, pad2);

    // Strobe, then 16 bits per port, B first, then 1s. $4017 has bits 2-4 high.
    bus.write(0x004016, 1);
    CHECK((bus.read(0x004016) & 1) == 1);  // latch high: live B
    bus.write(0x004016, 0);
    uint16_t got1 = 0, got2 = 0;
    for (int i = 0; i < 16; i++) {
        const auto b1 = bus.read(0x004016);
        const auto b2 = bus.read(0x004017);
        CHECK((b2 & 0x1C) == 0x1C);
        CHECK((b1 & 0x02) == 0 && (b2 & 0x02) == 0);  // no multitap
        got1 = static_cast<uint16_t>(got1 << 1 | (b1 & 1));
        got2 = static_cast<uint16_t>(got2 << 1 | (b2 & 1));
    }
    CHECK(got1 == pad1);
    CHECK(got2 == pad2);
    CHECK((bus.read(0x004016) & 1) == 1 && (bus.read(0x004017) & 1) == 1);

    // Without auto-read, the $4218 latches follow the live pads.
    CHECK(bus.read(0x004218) == (pad1 & 0xFF) && bus.read(0x004219) == (pad1 >> 8));
    CHECK(bus.read(0x00421A) == (pad2 & 0xFF) && bus.read(0x00421B) == (pad2 >> 8));

    // Auto-read at V-blank latches both pads; pads 3/4 read 0; later changes wait for the next V-blank.
    bus.write(0x004200, 0x01);
    bus.on_vblank_start();
    bus.set_joypad(0, 0);
    bus.set_joypad(1, 0x2000);
    CHECK(bus.read(0x004218) == (pad1 & 0xFF) && bus.read(0x004219) == (pad1 >> 8));
    CHECK(bus.read(0x00421A) == (pad2 & 0xFF) && bus.read(0x00421B) == (pad2 >> 8));
    CHECK(bus.read(0x00421C) == 0 && bus.read(0x00421D) == 0 && bus.read(0x00421E) == 0 && bus.read(0x00421F) == 0);
    // The serial registers were clocked empty by the auto-read.
    CHECK((bus.read(0x004016) & 1) == 1 && (bus.read(0x004017) & 1) == 1);
    bus.on_vblank_end();
    bus.on_vblank_start();
    CHECK(bus.read(0x00421B) == 0x20 && bus.read(0x004219) == 0);
}

// SRAM writes set the dirty flag; save states carry SRAM and refuse a size mismatch.
auto test_sram_dirty_and_state() -> void {
    snes::memory::Cartridge cart;
    cart.load_raw(std::vector<snes::Byte>(0x8000, 0), snes::memory::MapMode::LoROM, 0x2000);
    CHECK(!cart.sram_dirty());
    cart.write(0x700010, 0x00);  // same value: not dirty
    CHECK(!cart.sram_dirty());
    cart.write(0x700010, 0x5A);
    CHECK(cart.sram_dirty() && cart.sram_data()[0x10] == 0x5A);

    snes::Archive out = snes::Archive::saver();
    cart.serialize(out);
    snes::memory::Cartridge other;
    other.load_raw(std::vector<snes::Byte>(0x8000, 0), snes::memory::MapMode::LoROM, 0x800);
    snes::Archive in = snes::Archive::loader(out.data());
    other.serialize(in);
    CHECK(!in.ok());
}

// Power on with the ROM (as the frontend does: load, then reset to fetch the vectors).
auto boot(snes::System& sys, const std::string& path) -> bool {
    if (!sys.load_rom(path)) return false;
    sys.reset();
    return true;
}

// Save, run, load, run: same picture and sound. Returns false if the ROM can't be loaded.
auto test_determinism(const std::string& path) -> bool {
    snes::System sys;
    if (!boot(sys, path)) return false;
    run(sys, 0, kWarmupFrames);
    const auto& fb_pixels = sys.get_framebuffer();
    const auto [w, h] = sys.get_framebuffer_size();
    const std::vector<uint8_t> file = snes::savestate::encode(sys, kWarmupFrames, 0, fb_pixels, w, h);

    const RunResult first = run(sys, kWarmupFrames, kRunFrames);
    snes::savestate::Info info;
    CHECK(snes::savestate::decode(sys, file, info) == snes::savestate::Result::Ok);
    CHECK(info.frame == kWarmupFrames && info.rom_crc == sys.rom_crc32() && info.thumb_w > 0);
    const std::vector<uint8_t> resaved = snes::savestate::encode(sys, kWarmupFrames, 0, fb_pixels, w, h);
    CHECK(resaved == file);
    const RunResult second = run(sys, kWarmupFrames, kRunFrames);
    CHECK(first.video == second.video);
    CHECK(first.audio == second.audio);
    std::printf("[video %016llx audio %016llx, %zu non-zero samples, %zu-byte state] ",
                static_cast<unsigned long long>(first.video), static_cast<unsigned long long>(first.audio),
                first.nonzero_audio, file.size());
    return true;
}

// A state from another ROM, another version or a cut-off file is refused and changes nothing.
auto test_rejects(const std::string& rom_a, const std::string& rom_b) -> void {
    snes::System a;
    snes::System b;
    if (!boot(a, rom_a) || !boot(b, rom_b)) {
        CHECK(false);
        return;
    }
    run(a, 0, 30);
    run(b, 0, 30);
    const auto [w, h] = a.get_framebuffer_size();
    std::vector<uint8_t> file = snes::savestate::encode(a, 30, 1700000000, a.get_framebuffer(), w, h);
    const std::vector<uint8_t> before = b.save_state();
    snes::savestate::Info info;

    CHECK(snes::savestate::decode(b, file, info) == snes::savestate::Result::WrongRom);
    CHECK(b.save_state() == before);

    std::vector<uint8_t> bumped = file;
    bumped[8] = static_cast<uint8_t>(bumped[8] + 1);  // version field
    CHECK(snes::savestate::decode(a, bumped, info) == snes::savestate::Result::WrongVersion);

    std::vector<uint8_t> cut(file.begin(), file.end() - 100);
    const std::vector<uint8_t> a_before = a.save_state();
    CHECK(snes::savestate::decode(a, cut, info) == snes::savestate::Result::Corrupt);
    CHECK(a.save_state() == a_before);

    const std::vector<uint8_t> junk(64, 0x42);
    CHECK(snes::savestate::decode(a, junk, info) == snes::savestate::Result::NotAState);

    CHECK(snes::savestate::read_info(file, info) == snes::savestate::Result::Ok);
    CHECK(info.timestamp == 1700000000 && info.frame == 30 && info.thumb_w == w / 2 && info.thumb_h == h / 2);
    CHECK(info.thumbnail.size() == static_cast<size_t>(info.thumb_w * info.thumb_h));
}

// A LoROM cartridge with 2 KB battery RAM whose program increments $70:0000 once per boot.
auto write_counter_rom(const std::string& path) -> void {
    std::vector<uint8_t> rom(0x8000, 0x00);
    const uint8_t program[] = {
        0x78,                    // SEI
        0xAF, 0x00, 0x00, 0x70,  // LDA $700000
        0x1A,                    // INC A
        0x8F, 0x00, 0x00, 0x70,  // STA $700000
        0x80, 0xFE,              // BRA *
    };
    std::copy(std::begin(program), std::end(program), rom.begin());
    const char title[] = "SRAM COUNTER TEST    ";
    std::copy(title, title + 21, rom.begin() + 0x7FC0);
    rom[0x7FD5] = 0x20;  // LoROM
    rom[0x7FD6] = 0x02;  // ROM + RAM + battery
    rom[0x7FD7] = 0x05;  // 32 KB
    rom[0x7FD8] = 0x01;  // 2 KB SRAM
    rom[0x7FFC] = 0x00;  // reset vector $8000
    rom[0x7FFD] = 0x80;
    rom[0x7FDC] = 0xFF;  // checksum complement / checksum: any pair that XORs to $FFFF
    rom[0x7FDD] = 0xFF;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
}

auto read_file(const std::string& path) -> std::vector<uint8_t> {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// saves/<rom>.srm: loaded at boot, written every few seconds while dirty and on unload.
auto test_battery_saves() -> void {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "snes_state_tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string rom = (dir / "Counter.sfc").string();
    write_counter_rom(rom);
    const std::string srm = (dir / "saves" / "Counter.srm").string();

    {
        snes::frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom));
        CHECK(emu.sram_path() == srm);
        for (int i = 0; i < 10; i++) emu.run_frame();
        CHECK(!fs::exists(srm));  // not yet: flushed every kSramFlushFrames
        for (uint64_t i = 10; i < snes::frontend::Emulator::kSramFlushFrames; i++) emu.run_frame();
        const auto file = read_file(srm);
        CHECK(file.size() == 2048 && file[0] == 1);
    }
    {
        snes::frontend::Emulator emu;  // second boot: the counter continues from the file
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom));
        for (int i = 0; i < 5; i++) emu.run_frame();
        emu.unload();  // stop writes the file
        const auto file = read_file(srm);
        CHECK(file.size() == 2048 && file[0] == 2);

        // A save state carries SRAM, and the state's SRAM wins after a load.
        CHECK(emu.load(rom));  // counter -> 3
        emu.run_frame();
        CHECK(emu.save_state(4).empty());
        CHECK(fs::exists(dir / "states" / "Counter.4.sst"));
        emu.reset();  // counter -> 4
        emu.run_frame();
        CHECK(emu.load_state(4).empty());
        CHECK(emu.frame_count() == 1);
        CHECK(!emu.load_state(5).empty());  // empty slot: a message, no crash
        const auto info = emu.slot_info(4);
        CHECK(info && info->thumb_w == 128 && info->thumb_h == 112);
        emu.unload();
        CHECK(read_file(srm)[0] == 3);
    }
    fs::remove_all(dir);
}

// saves/<rom>.rtc beside the .srm on a clock cartridge (SPC7110 + RTC-4513): read at boot and
// advanced by the real time since it was written, written back with the battery save.
auto test_rtc_file() -> void {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "snes_state_tests_rtc";
    fs::remove_all(dir);
    fs::create_directories(dir / "saves");
    std::vector<uint8_t> rom(0x200000, 0x00);  // 1 MB program + 1 MB data, HiROM header
    const size_t h = 0xFFC0;
    std::memset(&rom[h], ' ', 21);
    std::memcpy(&rom[h], "RTC TEST", 8);
    rom[h + 0x15] = 0x3A;
    rom[h + 0x16] = 0xF9;  // SPC7110 + RTC
    rom[h + 0x17] = 0x0B;
    rom[h + 0x18] = 0x03;
    rom[h + 0x1A] = 0x33;
    rom[h + 0x1C] = 0x34; rom[h + 0x1D] = 0x12; rom[h + 0x1E] = 0xCB; rom[h + 0x1F] = 0xED;
    rom[0xFFFC] = 0x00; rom[0xFFFD] = 0x80;
    rom[0x8000] = 0x80; rom[0x8001] = 0xFE;  // bra $
    const std::string path = (dir / "Clock.sfc").string();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    const std::string rtc = (dir / "saves" / "Clock.rtc").string();
    // 1999-12-31 23:59:58 Friday, 24 h, calendar on; saved 3 seconds ago.
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<uint8_t> file = {0x58, 0x59, 0x23, 0x31, 0x12, 0x99, 0x25, 0x40};
    for (int i = 0; i < 8; i++) file.push_back(static_cast<uint8_t>(static_cast<uint64_t>(now - 3) >> (8 * i)));
    std::ofstream(rtc, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    {
        snes::frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(path));
        CHECK(emu.rtc_path() == rtc);
        emu.unload();
    }
    const auto saved = read_file(rtc);
    CHECK(saved.size() == 16);
    if (saved.size() == 16) {
        // 2000-01-01 00:00:01 (+ a second or two of test time), Saturday.
        CHECK(saved[0] >= 0x01 && saved[0] <= 0x03);
        CHECK(saved[1] == 0x00 && saved[2] == 0x00 && saved[3] == 0x01 && saved[4] == 0x01 && saved[5] == 0x00);
        CHECK((saved[6] & 0x0F) == 6 && saved[7] == 0x40);
        uint64_t stamp = 0;
        for (int i = 0; i < 8; i++) stamp |= static_cast<uint64_t>(saved[8 + static_cast<size_t>(i)]) << (8 * i);
        CHECK(static_cast<int64_t>(stamp) >= now && static_cast<int64_t>(stamp) <= now + 60);
    }
    {  // a cartridge without a clock writes no .rtc
        snes::frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        const std::string plain = (dir / "Counter.sfc").string();
        write_counter_rom(plain);
        CHECK(emu.load(plain));
        emu.unload();
        CHECK(!fs::exists(dir / "saves" / "Counter.rtc"));
    }
    fs::remove_all(dir);
}

// Save-state thumbnails: the box filter at every frame size, the round trip through the file
// header, the slot info the menu reads (date, frame, version), the menu's text helpers, and the
// version range that keeps older files loading.
auto test_thumbnails() -> void {
    namespace fs = std::filesystem;
    using snes::savestate::Info;
    auto frame = [](int w, int h) {
        std::vector<uint32_t> px(static_cast<size_t>(w) * static_cast<size_t>(h));
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                px[static_cast<size_t>(y * w + x)] = static_cast<uint32_t>((x & 0xFF) << 16 | (y & 0xFF) << 8 | ((x * 3 + y * 5) & 0xFF));
            }
        }
        return px;
    };
    struct Size { int w, h, tw, th; };
    for (const Size s : {Size{256, 224, 128, 112}, Size{512, 448, 128, 112}, Size{256, 239, 128, 119}, Size{512, 239, 128, 119},
                         Size{256, 478, 128, 119}}) {
        const auto px = frame(s.w, s.h);
        Info info;
        snes::savestate::make_thumbnail(px.data(), s.w, s.h, info);
        CHECK(info.thumb_w == s.tw && info.thumb_h == s.th && info.thumbnail.size() == static_cast<size_t>(s.tw * s.th));
        const int sx = s.w / s.tw, sy = s.h / s.th;
        bool exact = true;
        for (int y = 0; y < info.thumb_h && exact; y++) {
            for (int x = 0; x < info.thumb_w; x++) {
                uint32_t sum[3] = {};
                for (int dy = 0; dy < sy; dy++) {
                    for (int dx = 0; dx < sx; dx++) {
                        const uint32_t p = px[static_cast<size_t>((y * sy + dy) * s.w + x * sx + dx)];
                        sum[0] += p >> 16 & 0xFF; sum[1] += p >> 8 & 0xFF; sum[2] += p & 0xFF;
                    }
                }
                const auto n = static_cast<uint32_t>(sx * sy);
                const uint32_t want = 0xFF000000 | (sum[0] / n) << 16 | (sum[1] / n) << 8 | sum[2] / n;
                if (info.thumbnail[static_cast<size_t>(y * info.thumb_w + x)] != want) exact = false;
            }
        }
        CHECK(exact);
    }

    // Through the file: header and thumbnail back bit for bit, and the state still loads.
    const fs::path dir = fs::temp_directory_path() / "snes_state_tests_thumbs";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string rom = (dir / "Counter.sfc").string();
    write_counter_rom(rom);
    snes::System sys;
    CHECK(boot(sys, rom));
    sys.run_frame();
    const auto px = frame(256, 224);
    const auto file = snes::savestate::encode(sys, 1234, 1'700'000'000, px.data(), 256, 224);
    Info made;
    snes::savestate::make_thumbnail(px.data(), 256, 224, made);
    Info read;
    CHECK(snes::savestate::read_info(file, read) == snes::savestate::Result::Ok);
    CHECK(read.version == snes::savestate::kVersion && read.frame == 1234 && read.timestamp == 1'700'000'000);
    CHECK(read.thumb_w == 128 && read.thumb_h == 112 && read.thumbnail == made.thumbnail);
    CHECK(snes::savestate::decode(sys, file, read) == snes::savestate::Result::Ok);

    // Versions: kMinVersion..kVersion load; older and newer are refused (version at offset 8).
    for (const uint32_t v : {snes::savestate::kMinVersion - 1, snes::savestate::kVersion + 1}) {
        auto other = file;
        std::memcpy(&other[8], &v, 4);
        CHECK(snes::savestate::read_info(other, read) == snes::savestate::Result::WrongVersion);
        CHECK(snes::savestate::decode(sys, other, read) == snes::savestate::Result::WrongVersion);
    }
    // Every accepted version loads a file written in its own layout (older ones by encode's
    // version argument), with the controller ports intact.
    sys.set_port_connected(1, true);
    sys.set_pad(1, 0, 0x8421);
    for (uint32_t v = snes::savestate::kMinVersion; v <= snes::savestate::kVersion; v++) {
        const auto older = snes::savestate::encode(sys, 1234, 1'700'000'000, px.data(), 256, 224, v);
        CHECK(snes::savestate::read_info(older, read) == snes::savestate::Result::Ok && read.version == v);
        snes::System fresh;
        CHECK(boot(fresh, rom));
        CHECK(snes::savestate::decode(fresh, older, read) == snes::savestate::Result::Ok);
        CHECK(fresh.controller(1).device() == snes::Device::Gamepad && fresh.controller(1).pad(0) == 0x8421);
        CHECK(fresh.save_state() == sys.save_state());
    }
    // A multitap needs the current format; the older one keeps what it can (a gamepad).
    sys.set_port_device(1, snes::Device::Multitap);
    sys.set_pad(1, 3, 0x0F00);
    {
        snes::System fresh;
        CHECK(boot(fresh, rom));
        CHECK(snes::savestate::decode(fresh, snes::savestate::encode(sys, 1, 1, nullptr, 0, 0), read) == snes::savestate::Result::Ok);
        CHECK(fresh.controller(1).device() == snes::Device::Multitap && fresh.controller(1).pad(3) == 0x0F00);
        CHECK(snes::savestate::decode(fresh, snes::savestate::encode(sys, 1, 1, nullptr, 0, 0, 4), read) == snes::savestate::Result::Ok);
        CHECK(fresh.controller(1).device() == snes::Device::Gamepad);
    }
    // How a component keeps older files loading: a field added in version 5 is skipped for 4.
    struct Grown {
        uint8_t a = 0, b = 0;
        auto serialize(snes::Archive& ar) -> void {
            ar(a);
            if (ar.at_least(5)) ar(b);
        }
    };
    Grown saved{7, 9};
    snes::Archive out = snes::Archive::saver();
    out(saved);
    CHECK(out.data().size() == 2);
    const std::vector<uint8_t> v4_data = {7};
    Grown from4{0, 42};
    snes::Archive in4 = snes::Archive::loader(v4_data, 4);
    in4(from4);
    CHECK(in4.ok() && in4.remaining() == 0 && from4.a == 7 && from4.b == 42);
    Grown from5;
    snes::Archive in5 = snes::Archive::loader(out.data(), 5);
    in5(from5);
    CHECK(in5.ok() && in5.remaining() == 0 && from5.a == 7 && from5.b == 9);

    // What the slot menu reads after a real save.
    snes::frontend::Emulator emu;
    emu.set_data_dir(dir.string());
    CHECK(emu.load(rom));
    for (int i = 0; i < 30; i++) emu.run_frame();
    const int64_t before = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(emu.save_state(3).empty());
    const auto slot = emu.slot_info(3);
    CHECK(slot.has_value());
    if (slot) {
        CHECK(slot->timestamp >= before && slot->timestamp <= before + 5);
        CHECK(slot->frame == 30 && slot->version == snes::savestate::kVersion);
        CHECK(slot->thumb_w == emu.width() / 2 && slot->thumb_h == emu.height() / 2);
        CHECK(slot->thumbnail.size() == static_cast<size_t>(slot->thumb_w * slot->thumb_h));
    }
    CHECK(!emu.slot_info(4).has_value());
    CHECK(emu.load_state(3).empty());
    emu.unload();

    using E = snes::frontend::Emulator;
    CHECK(E::age_text(1000, 1030) == "just now" && E::age_text(1000, 1000 + 125) == "2 min ago");
    CHECK(E::age_text(0, 3 * 3600 + 5) == "3 h ago" && E::age_text(0, 86400 + 5) == "yesterday");
    CHECK(E::age_text(0, 5 * 86400) == "5 days ago" && E::age_text(10, 5) == "in the future");
    CHECK(E::play_time_text(0) == "0:00" && E::play_time_text(3606) == "1:00");
    CHECK(E::play_time_text(static_cast<uint64_t>(3723 * E::kNtscFrameRate + 1)) == "1:02:03");
    fs::remove_all(dir);
}

// cheats/<rom>.cht: loaded at boot and applied, rewritten by set_cheats.
auto test_cheats_file() -> void {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "snes_state_tests_cheats";
    fs::remove_all(dir);
    fs::create_directories(dir / "cheats");
    const std::string rom = (dir / "Counter.sfc").string();
    write_counter_rom(rom);
    {
        std::ofstream(dir / "cheats" / "Counter.cht") << "# test\non 7E0040AB  Fill\noff 7E0041CD\n";
    }
    snes::frontend::Emulator emu;
    emu.set_data_dir(dir.string());
    CHECK(emu.load(rom));
    CHECK(emu.cheats().size() == 2 && emu.cheats()[0].enabled && !emu.cheats()[1].enabled);
    emu.run_frame();
    auto list = emu.cheats();
    list[1].enabled = true;
    list.push_back({false, "DF47-E76D", "GG"});
    CHECK(emu.set_cheats(list));
    const auto text = read_file((dir / "cheats" / "Counter.cht").string());
    const auto parsed = snes::parse_cheat_file(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
    CHECK(parsed == list);
    emu.unload();
    // A game without a file and no cheats writes nothing.
    snes::frontend::Emulator other;
    other.set_data_dir((dir / "empty").string());
    CHECK(other.load(rom));
    CHECK(other.cheats().empty() && other.set_cheats({}));
    CHECK(!fs::exists(dir / "empty" / "cheats"));
    other.unload();
    fs::remove_all(dir);
}

// The delta coder and the history ring: exact round trips, size changes, the oldest entry held,
// the budget trimming the oldest entries, the fast-forward audio squeeze.
auto test_rewind_buffer() -> void {
    using namespace snes::frontend;
    std::vector<uint8_t> a(5000), b;
    for (size_t i = 0; i < a.size(); i++) a[i] = static_cast<uint8_t>(i * 7);
    b = a;
    b[0] ^= 1; b[1] ^= 2; b[2000] = 0; b[4999] ^= 0x80;
    b.resize(5003, 9);  // grows
    for (const auto& [from, to] : {std::pair{a, b}, std::pair{b, a}, std::pair{a, a}, std::pair{std::vector<uint8_t>{}, a}}) {
        const auto delta = encode_delta(from, to);
        std::vector<uint8_t> s = to;
        CHECK(apply_delta(s, delta, from.size()) && s == from);
    }
    CHECK(encode_delta(a, a).size() < 8);
    CHECK(encode_delta(a, b).size() < 32);
    std::vector<uint8_t> s = a;
    CHECK(!apply_delta(s, std::vector<uint8_t>{0x05, 0xFF}, a.size()));  // literals past the end

    RewindBuffer r;
    CHECK(!r.pop() && r.entries() == 0);
    r.configure(1 << 20, 4);
    std::vector<std::vector<uint8_t>> states;
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> st = a;
        st[static_cast<size_t>(i) * 100] = static_cast<uint8_t>(0xF0 + i);
        st.resize(a.size() + static_cast<size_t>(i % 3));
        states.push_back(st);
        r.push(st);
    }
    CHECK(r.entries() == 10 && r.bytes() < a.size() + 10 * 64);
    for (int i = 9; i >= 0; i--) {
        const auto got = r.pop();
        CHECK(got && *got == states[static_cast<size_t>(i)]);
    }
    const auto held = r.pop();  // at the oldest: it stays
    CHECK(held && *held == states[0] && r.entries() == 1);

    r.configure(64 * 1024, 2);  // tiny budget: incompressible states, only a few fit
    std::vector<uint8_t> noise(16 * 1024);
    uint32_t x = 1;
    for (int i = 0; i < 20; i++) {
        for (auto& byte : noise) byte = static_cast<uint8_t>((x = x * 1664525u + 1013904223u) >> 24);
        r.push(noise);
    }
    CHECK(r.bytes() <= 64 * 1024 && r.entries() >= 2 && r.entries() < 6 && r.interval() == 2);
    CHECK(*r.pop() == noise);
    r.clear();
    CHECK(r.entries() == 0 && r.bytes() == 0);

    std::vector<int16_t> out;
    squeeze_audio(std::vector<int16_t>{100, -100, 300, -300, 5, 6, 7, 8}, 2, out);
    CHECK((out == std::vector<int16_t>{200, -200, 6, 7}));
    squeeze_audio(std::vector<int16_t>{1, 2, 3, 4}, 1, out);
    CHECK((out == std::vector<int16_t>{1, 2, 3, 4}));
}

// Rewind is deterministic: capture every 4 frames while running with scripted input, step back
// through the history, and from each restored point the same input replays the same frames
// (picture and sound) bit for bit.
auto test_rewind_determinism(const std::string& rom) -> bool {
    using namespace snes::frontend;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "snes_state_tests_rewind";
    fs::remove_all(dir);
    Emulator emu;
    emu.set_data_dir(dir.string());
    if (!emu.load(rom)) return false;
    constexpr int kFrames = 240, kInterval = 4;
    auto step = [&](int f) -> uint64_t {
        emu.set_port_connected(1, true);
        emu.set_joypad(0, scripted_pad(0, f));
        emu.set_joypad(1, scripted_pad(1, f));
        emu.run_frame();
        Hash h;
        h.add(emu.framebuffer(), static_cast<size_t>(emu.width()) * static_cast<size_t>(emu.height()) * 4);
        const auto audio = emu.audio_samples();
        h.add(audio.data(), audio.size_bytes());
        return h.value;
    };
    while (emu.showing_test_pattern()) emu.run_frame();  // compare real pictures only
    const int start = static_cast<int>(emu.frame_count());
    RewindBuffer r;
    r.configure(64u << 20, kInterval);
    std::vector<uint64_t> hashes(static_cast<size_t>(start));
    size_t whole = 0;
    for (int f = start; f < kFrames; f++) {
        hashes.push_back(step(f));
        if ((f + 1) % kInterval == 0) {
            auto snap = emu.save_snapshot();
            whole += snap.size();
            r.push(std::move(snap));
        }
    }
    CHECK(r.entries() == static_cast<size_t>(kFrames / kInterval - start / kInterval));
    CHECK(r.bytes() < whole / 2);  // the deltas compress (the picture in the state changes most)
    int checked = 0;
    for (int back = 0; back < kFrames / kInterval; back += 7) {
        std::optional<std::vector<uint8_t>> snap;
        for (int k = 0; k <= 6 && r.entries() > 0; k++) snap = r.pop();  // step back up to 7 entries
        if (!snap) break;
        CHECK(emu.load_snapshot(*snap));
        const int from = static_cast<int>(emu.frame_count());
        CHECK(from % kInterval == 0 && from > start && from <= kFrames);
        for (int f = from; f < std::min(from + 12, kFrames); f++) CHECK(step(f) == hashes[static_cast<size_t>(f)]);
        checked++;
        if (r.entries() == 1) break;
    }
    CHECK(checked >= 5);
    CHECK(!emu.load_snapshot(std::vector<uint8_t>{1, 2, 3}));
    std::printf("[%zu-byte history for %zu bytes of states] ", r.bytes(), whole);
    emu.unload();
    fs::remove_all(dir);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    snes::utils::Logger::set_level(snes::utils::LogLevel::Error);
    std::printf("=== Save state / input port tests ===\n\n");
    int passed = 0;
    int failed = 0;
    auto run_test = [&](const std::string& name, auto&& fn) {
        std::printf("  %s... ", name.c_str());
        const int before = checks_failed;
        const bool ran = fn();
        if (ran && checks_failed == before) {
            std::printf("PASSED\n");
            passed++;
        } else {
            std::printf(ran ? "\n  FAILED\n" : "cannot open\n");
            failed++;
        }
    };

    run_test("Archive round trip, CRC-32", [] { test_archive(); return true; });
    run_test("$4016/$4017 serial + $4218-$421F auto-read, pad 2", [] { test_joypad_ports(); return true; });
    run_test("SRAM dirty flag, SRAM size check", [] { test_sram_dirty_and_state(); return true; });
    run_test("Battery saves (saves/<rom>.srm) and state slots", [] { test_battery_saves(); return true; });
    run_test("Clock file (saves/<rom>.rtc), real time since saving", [] { test_rtc_file(); return true; });
    run_test("Cheats file (cheats/<rom>.cht)", [] { test_cheats_file(); return true; });
    run_test("Thumbnails: box filter, file round trip, slot info, versions", [] { test_thumbnails(); return true; });
    run_test("Rewind history: delta coding, budget, audio squeeze", [] { test_rewind_buffer(); return true; });
    const std::string built_rom = test_rom::write_test_rom(0);
    run_test("Rewind determinism (built-in test ROM)", [&] { return test_rewind_determinism(built_rom); });

    // The built-in test ROMs, then any in SNES_TEST_ROMS_DIR, then the command line.
    std::vector<std::string> roms = {built_rom, test_rom::write_test_rom(1)};
    std::vector<std::string> extra;
    std::error_code ec;
    if (const std::string dir = test_rom::test_roms_dir(); !dir.empty()) {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".sfc" || e.path().extension() == ".smc") extra.push_back(e.path().string());
        }
    }
    std::sort(extra.begin(), extra.end());
    roms.insert(roms.end(), extra.begin(), extra.end());
    for (int i = 1; i < argc; i++) roms.emplace_back(argv[i]);

    if (roms.size() >= 2) {
        run_test("Refuses other ROM / version / truncated", [&] { test_rejects(roms[0], roms[1]); return true; });
    }
    for (const auto& path : roms) {
        run_test("Determinism " + std::filesystem::path(path).filename().string(), [&] { return test_determinism(path); });
    }

    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
