// SPDX-License-Identifier: MIT
#pragma once
// Test ROMs built in code, so the suites need no ROM files. make_test_rom() assembles a 32 KB
// LoROM program: it turns the screen on and, every frame (NMI), changes the backdrop colour from
// a frame counter mixed with the pad 1 state, and writes to the APU ports. That gives the save
// state / rewind / threading tests a picture that changes with time and input.
//
// test_roms_dir(): optional folder of extra test ROMs (.sfc/.spc), from the BITFORGE_TEST_ROMS_DIR
// environment variable, else the BITFORGE_TEST_ROMS_DIR CMake option (empty = none).
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef BITFORGE_TEST_ROMS_DIR
#define BITFORGE_TEST_ROMS_DIR ""
#endif

namespace test_rom {

inline auto test_roms_dir() -> std::string {
    if (const char* env = std::getenv("BITFORGE_TEST_ROMS_DIR"); env != nullptr && *env != '\0') return env;
    return BITFORGE_TEST_ROMS_DIR;
}

// `variant` changes the title and the colour formula, giving distinct ROMs.
inline auto make_test_rom(uint8_t variant = 0) -> std::vector<uint8_t> {
    std::vector<uint8_t> rom(0x8000, 0xFF);
    const std::vector<uint8_t> reset = {
        0x78,                    // sei
        0x18, 0xFB,              // clc / xce: native mode
        0xE2, 0x30,              // sep #$30: 8-bit A, X, Y
        0xA9, 0x8F, 0x8D, 0x00, 0x21,  // lda #$8F / sta $2100: forced blank
        0x9C, 0x00, 0x00,        // stz $0000: frame counter
        0x9C, 0x21, 0x21,        // stz $2121
        0x9C, 0x22, 0x21,        // stz $2122
        0x9C, 0x22, 0x21,        // stz $2122
        0xA9, 0x0F, 0x8D, 0x00, 0x21,  // lda #$0F / sta $2100: screen on, full brightness
        0xA9, 0x81, 0x8D, 0x00, 0x42,  // lda #$81 / sta $4200: NMI + auto joypad read
        0xCB,                    // wai
        0x80, 0xFD,              // bra wai
    };
    const std::vector<uint8_t> nmi = {
        0x48,                    // pha
        0xAD, 0x10, 0x42,        // lda $4210: acknowledge NMI
        0xEE, 0x00, 0x00,        // inc $0000
        0x9C, 0x21, 0x21,        // stz $2121
        0xAD, 0x00, 0x00,        // lda $0000
        0x49, variant,           // eor #variant
        0x8D, 0x22, 0x21,        // sta $2122: colour low
        0x4D, 0x19, 0x42,        // eor $4219: pad 1 high byte (B Y Select Start Up Down Left Right)
        0x29, 0x7F,              // and #$7F
        0x8D, 0x22, 0x21,        // sta $2122: colour high
        0x8D, 0x40, 0x21,        // sta $2140: APU port 0
        0x68,                    // pla
        0x40,                    // rti
    };
    constexpr size_t kReset = 0x0000, kNmi = 0x0100;
    std::copy(reset.begin(), reset.end(), rom.begin() + kReset);
    std::copy(nmi.begin(), nmi.end(), rom.begin() + kNmi);

    // Header at $7FC0 (LoROM).
    std::string title = "TEST ROM " + std::to_string(variant);
    title.resize(21, ' ');
    std::copy(title.begin(), title.end(), rom.begin() + 0x7FC0);
    rom[0x7FD5] = 0x20;  // LoROM, SlowROM
    rom[0x7FD6] = 0x00;  // ROM only
    rom[0x7FD7] = 0x05;  // 32 KB
    rom[0x7FD8] = 0x00;  // no SRAM
    rom[0x7FD9] = 0x01;  // North America
    rom[0x7FDA] = 0x00;
    rom[0x7FDB] = 0x00;
    auto put16 = [&](size_t at, uint16_t v) {
        rom[at] = static_cast<uint8_t>(v);
        rom[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    const uint16_t reset_at = 0x8000 + kReset, nmi_at = 0x8000 + kNmi;
    put16(0x7FEA, nmi_at);    // native NMI
    put16(0x7FFA, nmi_at);    // emulation NMI
    put16(0x7FFC, reset_at);  // reset
    // Checksum over the image with the checksum pair preset to $FFFF/$0000.
    put16(0x7FDC, 0xFFFF);
    put16(0x7FDE, 0x0000);
    uint16_t sum = 0;
    for (uint8_t b : rom) sum = static_cast<uint16_t>(sum + b);
    put16(0x7FDC, static_cast<uint16_t>(~sum));
    put16(0x7FDE, sum);
    return rom;
}

// Writes make_test_rom(variant) to the temp folder and returns its path.
inline auto write_test_rom(uint8_t variant = 0) -> std::string {
    const auto path = std::filesystem::temp_directory_path() / ("snes_test_rom_" + std::to_string(variant) + ".sfc");
    const auto rom = make_test_rom(variant);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    return path.string();
}

} // namespace test_rom
