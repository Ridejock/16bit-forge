// SPDX-License-Identifier: MIT
// PPU, timing/IRQ and DMA/HDMA tests: sprites and their limits, the OAM tables, layer
// priorities, windows and color math, H/V IRQs and the master-clock model, DMA modes and HDMA.
#include "snes/cpu/cpu65c816.hpp"
#include "snes/memory/bus.hpp"
#include "snes/ppu/ppu.hpp"
#include "utils/logging.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <vector>

using bitforge::Address;
using bitforge::Byte;
using bitforge::Word;
using bitforge::ppu::PPU;

namespace {

constexpr int kLine = PPU::kClocksPerLine;
constexpr uint32_t kBlack = 0xFF000000;

// Expected framebuffer pixel for a BGR555 color at full brightness (5-bit channels widened
// by bit replication: 31 -> 255).
constexpr auto rgb(Word c, int brightness = 15) -> uint32_t {
    const auto& level = bitforge::ppu::tables::kBrightness[static_cast<size_t>(brightness)];
    return 0xFF000000u | (static_cast<uint32_t>(level[c & 0x1F]) << 16) |
           (static_cast<uint32_t>(level[(c >> 5) & 0x1F]) << 8) | level[(c >> 10) & 0x1F];
}
constexpr Word kRed = 0x001F, kGreen = 0x03E0, kBlue = 0x7C00, kGray = 0x2108;

int failures_in_test = 0;
auto check(bool ok, const char* what) -> void {
    if (!ok) {
        if (failures_in_test == 0) printf("\n");
        printf("      FAIL: %s\n", what);
        failures_in_test++;
    }
}
auto check_eq(unsigned got, unsigned want, const char* what) -> void {
    if (got != want) {
        if (failures_in_test == 0) printf("\n");
        printf("      FAIL: %s: got 0x%X, want 0x%X\n", what, got, want);
        failures_in_test++;
    }
}

// A bus + PPU (+ CPU) with helpers that poke registers the way a game would.
struct Rig {
    bitforge::memory::Bus bus;
    PPU ppu;
    bitforge::cpu::CPU65c816 cpu{bus};
    uint32_t clocks_run = 0;

    Rig() {
        bus.connect_ppu(&ppu);
        bus.connect_cpu(&cpu);
        ppu.connect_bus(&bus);
        bus.load_test_rom(std::vector<Byte>(0x8000, 0xEA));
        bus.reset();
    }
    auto reg(Word addr, Byte value) -> void { bus.write(addr, value); }
    auto reg16(Word addr, Word value) -> void {
        bus.write(addr, static_cast<Byte>(value));
        bus.write(static_cast<Word>(addr + 1), static_cast<Byte>(value >> 8));
    }
    auto read(Address addr) -> Byte { return bus.read(addr); }
    auto color(int index, Word value) -> void {
        reg(0x2121, static_cast<Byte>(index));
        reg(0x2122, static_cast<Byte>(value));
        reg(0x2122, static_cast<Byte>(value >> 8));
    }
    auto vram_word(Word word_addr, Word value) -> void {
        reg(0x2115, 0x80);
        reg16(0x2116, word_addr);
        reg(0x2118, static_cast<Byte>(value));
        reg(0x2119, static_cast<Byte>(value >> 8));
    }
    auto vram_read(Word word_addr) -> Word {
        reg(0x2115, 0x80);
        reg16(0x2116, word_addr);
        const Byte low = read(0x2139);
        return static_cast<Word>(low | (read(0x213A) << 8));
    }
    // 4bpp 8x8 tile at a VRAM word address. `rows[y]` is the plane-0 mask; every set pixel gets
    // color `c`.
    auto tile4(Word word_addr, int c, std::array<Byte, 8> rows) -> void {
        for (int y = 0; y < 8; y++) {
            const Byte m = rows[static_cast<size_t>(y)];
            vram_word(static_cast<Word>(word_addr + y), static_cast<Word>(((c & 1) ? m : 0) | (((c & 2) ? m : 0) << 8)));
            vram_word(static_cast<Word>(word_addr + 8 + y), static_cast<Word>(((c & 4) ? m : 0) | (((c & 8) ? m : 0) << 8)));
        }
    }
    auto solid4(Word word_addr, int c) -> void { tile4(word_addr, c, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}); }
    // 2bpp solid tile
    auto solid2(Word word_addr, int c) -> void {
        for (int y = 0; y < 8; y++) {
            vram_word(static_cast<Word>(word_addr + y), static_cast<Word>(((c & 1) ? 0xFF : 0) | (((c & 2) ? 0xFF : 0) << 8)));
        }
    }
    // Fill a whole 32x32 BG map with one entry.
    auto fill_map(Word map_word, Word entry) -> void {
        for (int i = 0; i < 1024; i++) vram_word(static_cast<Word>(map_word + i), entry);
    }
    // One sprite through $2102/$2104, low and high table.
    auto sprite(int index, int x, int y, int tile, Byte attr, bool large = false) -> void {
        reg16(0x2102, static_cast<Word>(index * 2));
        reg(0x2104, static_cast<Byte>(x));
        reg(0x2104, static_cast<Byte>(y));
        reg(0x2104, static_cast<Byte>(tile));
        reg(0x2104, attr);
        // High table: read-modify-write this sprite's 2 bits. OAMADD is a word address, so
        // read and write the byte pair holding them.
        const int byte = index / 4;
        const int shift = (index & 3) * 2;
        const Word word = static_cast<Word>(0x100 + byte / 2);
        reg16(0x2102, word);
        std::array<Byte, 2> pair = {read(0x2138), read(0x2138)};
        const Byte bits = static_cast<Byte>(((x >> 8) & 1) | (large ? 2 : 0));
        Byte& b = pair[static_cast<size_t>(byte & 1)];
        b = static_cast<Byte>((b & ~(3 << shift)) | (bits << shift));
        reg16(0x2102, word);
        reg(0x2104, pair[0]);
        reg(0x2104, pair[1]);
    }
    // Park every sprite off screen (y = 240).
    auto hide_sprites() -> void {
        reg16(0x2102, 0);
        for (int i = 0; i < 128; i++) {
            reg(0x2104, 0); reg(0x2104, 240); reg(0x2104, 0); reg(0x2104, 0);
        }
        for (int i = 0; i < 32; i++) reg(0x2104, 0);
    }
    // 4bpp 8x8 tile from a function of (x, y) giving the color index 0-15 of each pixel.
    template <typename F> auto tile4_fn(Word word_addr, F color_at) -> void {
        for (int y = 0; y < 8; y++) {
            std::array<Word, 2> planes{};  // planes 0/1 and 2/3
            for (int x = 0; x < 8; x++) {
                const int c = color_at(x, y);
                for (int p = 0; p < 4; p++) {
                    if (c & (1 << p)) planes[static_cast<size_t>(p / 2)] |= static_cast<Word>((0x80 >> x) << ((p & 1) * 8));
                }
            }
            vram_word(static_cast<Word>(word_addr + y), planes[0]);
            vram_word(static_cast<Word>(word_addr + 8 + y), planes[1]);
        }
    }
    // Solid 8bpp tile of color index `c`.
    auto solid8(Word word_addr, int c) -> void {
        for (int y = 0; y < 8; y++) {
            for (int pair = 0; pair < 4; pair++) {
                const Byte lo = (c >> (pair * 2)) & 1 ? 0xFF : 0;
                const Byte hi = (c >> (pair * 2 + 1)) & 1 ? 0xFF : 0;
                vram_word(static_cast<Word>(word_addr + pair * 8 + y), static_cast<Word>(lo | (hi << 8)));
            }
        }
    }
    auto step(uint32_t clocks) -> void { ppu.step(clocks); clocks_run += clocks; }
    // Runs to `clock` master clocks into line `line` of the current frame.
    auto run_to(int line, int clock) -> void {
        const int now = ppu.vcounter() * kLine + ppu.hclock();
        step(static_cast<uint32_t>(line * kLine + clock - now));
    }
    // Just after line `line` was drawn and its H-blank HDMA ran: where raster writes land.
    auto run_to_hblank(int line) -> void { run_to(line, PPU::kHblankClock + 16); }
    // From power-on (line 0), run until every visible line has been drawn (start of V-blank).
    auto render(Byte inidisp = 0x0F) -> void {
        reg(0x2100, inidisp);
        step(225u * kLine);
    }
    // Runs to the start of the next frame (line 0) and returns how many lines this one had.
    auto finish_frame() -> int {
        int lines = ppu.vcounter();
        do { step(kLine); lines++; } while (ppu.vcounter() != 0);
        return lines;
    }
    auto px(int x, int y) const -> uint32_t { return ppu.get_framebuffer()[y * ppu.frame_width() + x]; }
};

// ---- OAM ------------------------------------------------------------------------------------

auto test_oam_tables() -> void {
    Rig r;
    // Low table writes are latched in pairs: a lone even byte doesn't land.
    r.reg16(0x2102, 0);
    r.reg(0x2104, 0x12);
    check_eq(r.ppu.oam_byte(0), 0x00, "even byte waits for its pair");
    r.reg(0x2104, 0x34);
    check_eq(r.ppu.oam_byte(0), 0x12, "pair committed (low)");
    check_eq(r.ppu.oam_byte(1), 0x34, "pair committed (high)");

    // High table: word address $100 = byte $200, written byte by byte, mirrored over $200-$3FF.
    r.reg16(0x2102, 0x100);
    r.reg(0x2104, 0xA5);
    check_eq(r.ppu.oam_byte(0x200), 0xA5, "high table byte written directly");
    r.reg16(0x2102, 0x110);           // byte $220 mirrors $200
    r.reg(0x2104, 0x5A);
    check_eq(r.ppu.oam_byte(0x200), 0x5A, "high table mirrors every 32 bytes");

    // Decoding: x bit 8 and the size bit come from the high table.
    r.sprite(5, 0x1F0, 40, 0x23, 0xF7, true);  // x = -16, vflip+hflip, prio 3, pal 3, name bit
    const auto s = r.ppu.sprite(5);
    check(s.x == -16, "9-bit X sign");
    check(s.y == 40 && s.tile == 0x123 && s.palette == 3 && s.priority == 3, "attributes");
    check(s.hflip && s.vflip && s.large, "flips and size");

    // RDOAM walks the same address.
    r.reg16(0x2102, 0);
    check_eq(r.read(0x2138), 0x12, "RDOAM byte 0");
    check_eq(r.read(0x2138), 0x34, "RDOAM byte 1");
}

auto test_oam_reload_at_vblank() -> void {
    Rig r;
    r.reg(0x2100, 0x0F);              // not force blank: the reload happens
    r.reg16(0x2102, 0x0002);          // reload word 2 = byte 4
    r.read(0x2138); r.read(0x2138); r.read(0x2138);  // walk the address away
    r.step(225u * kLine);             // enter V-blank
    r.bus.write(0x2104, 0x77);        // byte 4 (even): latched
    r.bus.write(0x2104, 0x88);
    check_eq(r.ppu.oam_byte(4), 0x77, "address back at OAMADD*2 after V-blank");
    check_eq(r.ppu.oam_byte(5), 0x88, "second byte");
}

auto test_obj_sizes() -> void {
    Rig r;
    constexpr std::array<std::array<int, 4>, 8> expected = {{
        {8, 8, 16, 16}, {8, 8, 32, 32}, {8, 8, 64, 64}, {16, 16, 32, 32},
        {16, 16, 64, 64}, {32, 32, 64, 64}, {16, 32, 32, 64}, {16, 32, 32, 32},
    }};
    for (int sel = 0; sel < 8; sel++) {
        r.reg(0x2101, static_cast<Byte>(sel << 5));
        const auto small = r.ppu.sprite_size(false);
        const auto large = r.ppu.sprite_size(true);
        const auto& e = expected[static_cast<size_t>(sel)];
        check(small.w == e[0] && small.h == e[1] && large.w == e[2] && large.h == e[3], "OBSEL size pair");
    }
}

// ---- Sprites --------------------------------------------------------------------------------

// Common sprite scene: OBJ only on the main screen, backdrop gray, OBJ palette 0 color 1 red.
auto sprite_rig(Rig& r) -> void {
    r.hide_sprites();
    r.color(0, kGray);
    r.color(129, kRed);               // OBJ palette 0, color 1
    r.color(129 + 16, kGreen);        // OBJ palette 1, color 1
    r.color(129 + 16 * 4, kBlue);     // OBJ palette 4, color 1
    r.reg(0x2101, 0x00);              // 8x8/16x16, name base $0000
    r.reg(0x212C, 0x10);
}

auto test_sprite_position_and_wrap() -> void {
    Rig r;
    sprite_rig(r);
    r.solid4(0x0010, 1);              // tile 1
    r.sprite(0, 100, 50, 1, 0x00);    // shows on rows 50-57 (sprite Y = 0 is the first row)
    r.sprite(1, 0x1FC, 80, 1, 0x00);  // X = -4: four columns peek in at the left edge
    r.sprite(2, 20, 252, 1, 0x00);    // Y = 252 wraps: rows 0-3 of the screen
    r.sprite(3, 252, 120, 1, 0x00);   // runs off the right edge
    r.render();
    check_eq(r.px(100, 50), rgb(kRed), "top-left");
    check_eq(r.px(107, 57), rgb(kRed), "bottom-right");
    check_eq(r.px(100, 49), rgb(kGray), "row above");
    check_eq(r.px(108, 50), rgb(kGray), "column right");
    check_eq(r.px(0, 80), rgb(kRed), "X=-4 shows at 0");
    check_eq(r.px(3, 80), rgb(kRed), "X=-4 shows at 3");
    check_eq(r.px(4, 80), rgb(kGray), "X=-4 ends at 3");
    check_eq(r.px(20, 0), rgb(kRed), "Y wrap row 0");
    check_eq(r.px(20, 3), rgb(kRed), "Y wrap row 3");
    check_eq(r.px(20, 4), rgb(kGray), "Y wrap ends");
    check_eq(r.px(255, 120), rgb(kRed), "right edge clipped, not wrapped");
    check_eq(r.px(0, 120), rgb(kGray), "no wrap to the left edge");
}

auto test_sprite_flip_and_large() -> void {
    Rig r;
    sprite_rig(r);
    // 16x16 sprite from tiles 0x20,0x21 / 0x30,0x31 (rows wrap in a 16-tile page). Only tile
    // 0x20 is drawn, with just its top-left pixel set.
    r.tile4(0x0200, 1, {0x80, 0, 0, 0, 0, 0, 0, 0});
    r.sprite(0, 40, 40, 0x20, 0x00, true);
    r.sprite(1, 80, 40, 0x20, 0x40, true);   // h-flip: pixel lands top-right of the 16x16
    r.sprite(2, 120, 40, 0x20, 0x80, true);  // v-flip: bottom-left
    r.sprite(3, 160, 40, 0x20, 0xC0, true);  // both: bottom-right
    r.render();
    check_eq(r.px(40, 40), rgb(kRed), "no flip");
    check_eq(r.px(95, 40), rgb(kRed), "h-flip over the whole sprite");
    check_eq(r.px(80, 40), rgb(kGray), "h-flip moved the pixel");
    check_eq(r.px(120, 55), rgb(kRed), "v-flip over the whole sprite");
    check_eq(r.px(175, 55), rgb(kRed), "h+v flip");
}

auto test_sprite_palettes_and_name_gap() -> void {
    Rig r;
    sprite_rig(r);
    r.reg(0x2101, 0x0A);  // name base word $4000, gap 1: the second table is at $4000+$2000
    r.solid4(0x4010, 1);                      // tile $001
    r.solid4(0x6010, 1);                      // tile $101
    r.sprite(0, 10, 10, 0x01, 0x00);          // palette 0
    r.sprite(1, 30, 10, 0x01, 0x02);          // palette 1
    r.sprite(2, 50, 10, 0x01, 0x01);          // name bit -> second table
    r.sprite(3, 70, 10, 0x01, 0x08);          // palette 4
    r.render();
    check_eq(r.px(10, 10), rgb(kRed), "palette 0 = CGRAM 128+");
    check_eq(r.px(30, 10), rgb(kGreen), "palette 1 = CGRAM 144+");
    check_eq(r.px(50, 10), rgb(kRed), "tile $101 from base + gap");
    check_eq(r.px(70, 10), rgb(kBlue), "palette 4 = CGRAM 192+");
}

auto test_sprite_order_and_rotation() -> void {
    Rig r;
    sprite_rig(r);
    r.solid4(0x0010, 1);
    r.sprite(0, 60, 60, 1, 0x00);   // palette 0 (red), priority 0
    r.sprite(5, 64, 60, 1, 0x32);   // palette 1 (green), priority 3: still under sprite 0
    r.render();
    check_eq(r.px(64, 60), rgb(kRed), "lower OAM index wins regardless of priority");
    check_eq(r.px(70, 60), rgb(kGreen), "sprite 5 visible past sprite 0");

    Rig r2;
    sprite_rig(r2);
    r2.solid4(0x0010, 1);
    r2.sprite(0, 60, 60, 1, 0x00);
    r2.sprite(5, 64, 60, 1, 0x32);
    r2.reg(0x2102, 5 * 2);          // OAMADD = sprite 5 ...
    r2.reg(0x2103, 0x80);           // ... with priority rotation
    r2.render();
    check_eq(r2.px(64, 60), rgb(kGreen), "rotation puts sprite 5 first");
}

auto test_sprite_range_limit() -> void {
    Rig r;
    sprite_rig(r);
    r.solid4(0x0010, 1);
    // 33 sprites on row 100: 0-31 packed at x = 0..124, sprite 32 alone at x = 200 is dropped.
    for (int i = 0; i < 33; i++) r.sprite(i, i < 32 ? i * 4 : 200, 100, 1, 0x00);
    r.render();
    check(r.read(0x213E) & 0x40, "range over flag (33 sprites)");
    check_eq(r.px(200, 100), rgb(kGray), "sprite 32 dropped");
    check_eq(r.px(130, 100), rgb(kRed), "sprite 31 drawn");

    Rig ok;
    sprite_rig(ok);
    ok.solid4(0x0010, 1);
    for (int i = 0; i < 32; i++) ok.sprite(i, i * 8, 100, 1, 0x00);
    ok.render();
    check((ok.read(0x213E) & 0xC0) == 0, "32 sprites: no flags");
    check_eq(ok.read(0x213E) & 0x0F, 1, "PPU1 version");
}

auto test_sprite_time_limit() -> void {
    Rig r;
    sprite_rig(r);
    r.solid4(0x0010, 1); r.solid4(0x0020, 1);  // 16x16 sprite uses tiles 1,2 and 0x11,0x12
    r.solid4(0x0110, 1); r.solid4(0x0120, 1);
    // 18 large sprites = 36 slivers on row 30. Slivers are fetched from the last sprite back, so
    // the two dropped ones are sprite 0's.
    for (int i = 0; i < 18; i++) r.sprite(i, i * 14, 30, 1, i == 0 ? 0x02 : 0x00, true);
    r.render();
    check(r.read(0x213E) & 0x80, "time over flag (36 slivers)");
    check((r.read(0x213E) & 0x40) == 0, "no range over");
    check_eq(r.px(2, 30), rgb(kGray), "sprite 0 lost its slivers");
    check_eq(r.px(20, 30), rgb(kRed), "sprite 1 drawn");

    // Slivers fully off screen don't count: 17 visible large sprites (34) + one at X=-16.
    Rig ok;
    sprite_rig(ok);
    ok.solid4(0x0010, 1); ok.solid4(0x0020, 1);
    for (int i = 0; i < 17; i++) ok.sprite(i, i * 14, 30, 1, 0x00, true);
    ok.sprite(17, 0x1F8, 30, 1, 0x00, true);    // x=-8: one sliver on screen -> 35 total
    ok.sprite(18, 0x100, 60, 1, 0x00, true);    // x=-256 on another row, no slivers
    ok.render();
    check(ok.read(0x213E) & 0x80, "35 slivers is over");
    Rig ok2;
    sprite_rig(ok2);
    ok2.solid4(0x0010, 1); ok2.solid4(0x0020, 1);
    for (int i = 0; i < 17; i++) ok2.sprite(i, i * 14, 30, 1, 0x00, true);
    ok2.sprite(17, 0x1F0, 30, 1, 0x00, true);   // x=-16: nothing on screen
    ok2.render();
    check((ok2.read(0x213E) & 0x80) == 0, "34 visible slivers is fine");
}

// ---- Priorities and screens -----------------------------------------------------------------

// Mode 1 scene: BG1 tile (4bpp) red, BG3 tile (2bpp) blue, a green sprite; each layer is a
// single solid tile filling its map. `bg1_entry`/`bg3_entry` carry the priority bit (0x2000).
auto mode1_scene(Rig& r, Word bg1_entry, Word bg3_entry, Byte obj_attr, Byte bgmode) -> void {
    r.hide_sprites();
    r.color(0, kGray);
    r.color(1, kRed);                 // BG1 palette 0 color 1
    r.color(1 + 4, kBlue);            // BG3 palette 1 color 1 (2bpp palettes are 4 colors)
    r.color(129, kGreen);
    r.reg(0x2105, bgmode);
    r.reg(0x2107, 0x10);              // BG1 map word $1000
    r.reg(0x2109, 0x14);              // BG3 map word $1400
    r.reg(0x210B, 0x00);              // BG1 chars $0000
    r.reg(0x210C, 0x02);              // BG3 chars $2000
    r.solid4(0x0010, 1);              // BG1 tile 1
    r.solid2(0x2008, 1);              // BG3 tile 1
    r.fill_map(0x1000, static_cast<Word>(0x0001 | bg1_entry));
    r.fill_map(0x1400, static_cast<Word>(0x0001 | 0x0400 | bg3_entry));
    r.solid4(0x6010, 1);
    r.reg(0x2101, 0x03);              // sprites at word $6000
    r.sprite(0, 0, 0, 1, obj_attr);
    r.reg(0x212C, 0x15);              // BG1, BG3, OBJ
}

auto test_mode1_priorities() -> void {
    struct Case { Word bg1; Word bg3; Byte obj; Byte mode; Word want; const char* what; };
    const Case cases[] = {
        {0x0000, 0x0000, 0x00, 0x01, kRed, "BG1 low over OBJ 0 and BG3 low"},
        {0x0000, 0x0000, 0x20, 0x01, kGreen, "OBJ 2 over BG1 low"},
        {0x2000, 0x0000, 0x20, 0x01, kRed, "BG1 high over OBJ 2"},
        {0x2000, 0x0000, 0x30, 0x01, kGreen, "OBJ 3 over everything"},
        {0x2000, 0x2000, 0x30, 0x01, kGreen, "BG3 high stays behind without BGMODE bit 3"},
        {0x2000, 0x2000, 0x30, 0x09, kBlue, "BG3 high to the front with BGMODE bit 3"},
        {0x2000, 0x0000, 0x30, 0x09, kGreen, "BG3 low stays at the back with bit 3"},
    };
    for (const auto& c : cases) {
        Rig r;
        mode1_scene(r, c.bg1, c.bg3, c.obj, c.mode);
        r.render();
        check_eq(r.px(2, 2), rgb(c.want), c.what);
    }
    // With BG1 off and no bit 3: BG3 high sits between OBJ 1 and OBJ 0.
    Rig r;
    mode1_scene(r, 0, 0x2000, 0x00, 0x01);
    r.reg(0x212C, 0x14);
    r.render();
    check_eq(r.px(2, 2), rgb(kBlue), "BG3 high over OBJ 0");
    Rig r2;
    mode1_scene(r2, 0, 0x2000, 0x10, 0x01);
    r2.reg(0x212C, 0x14);
    r2.render();
    check_eq(r2.px(2, 2), rgb(kGreen), "OBJ 1 over BG3 high");
    Rig r3;
    mode1_scene(r3, 0, 0x0000, 0x00, 0x01);
    r3.reg(0x212C, 0x14);
    r3.render();
    check_eq(r3.px(2, 2), rgb(kGreen), "OBJ 0 over BG3 low");
}

auto test_mode0_priorities() -> void {
    // Mode 0: BG2 high is above OBJ 1 and BG1 low; BG4 high sits above OBJ 0.
    Rig r;
    r.hide_sprites();
    r.color(0, kGray);
    r.color(32 + 1, kRed);            // BG2 palette 0 color 1 (mode 0 BG2 slice at 32)
    r.color(96 + 1, kBlue);           // BG4 palette 0 color 1
    r.color(129, kGreen);
    r.reg(0x2105, 0x00);
    r.reg(0x2108, 0x10);              // BG2 map $1000
    r.reg(0x210A, 0x14);              // BG4 map $1400
    r.reg(0x210B, 0x00);
    r.reg(0x210C, 0x00);
    r.solid2(0x0008, 1);
    r.fill_map(0x1000, 0x2001);       // BG2 high
    r.fill_map(0x1400, 0x2001);       // BG4 high
    r.reg(0x2101, 0x03);
    r.solid4(0x6010, 1);
    r.sprite(0, 0, 0, 1, 0x10);       // OBJ priority 1
    r.reg(0x212C, 0x1A);              // BG2, BG4, OBJ
    r.render();
    check_eq(r.px(2, 2), rgb(kRed), "BG2 high over OBJ 1");
    check_eq(r.px(20, 20), rgb(kRed), "BG2 high over BG4 high");
    r.reg(0x2100, 0x80);
    Rig r2;
    r2.hide_sprites();
    r2.color(0, kGray);
    r2.color(96 + 1, kBlue);
    r2.color(129, kGreen);
    r2.reg(0x210A, 0x14);
    r2.solid2(0x0008, 1);
    r2.fill_map(0x1400, 0x2001);
    r2.reg(0x2101, 0x03);
    r2.solid4(0x6010, 1);
    r2.sprite(0, 0, 0, 1, 0x00);      // OBJ 0 under BG4 high
    r2.reg(0x212C, 0x18);
    r2.render();
    check_eq(r2.px(2, 2), rgb(kBlue), "BG4 high over OBJ 0");
}

// ---- VRAM port: VMAIN increment, remapping, read latch ---------------------------------------

auto test_vmain_increment() -> void {
    // VMAIN bits 1-0 pick the step (1, 32, 128, 128); bit 7 picks the byte that steps.
    struct Case { Byte vmain; Word step; };
    for (const Case c : {Case{0x80, 1}, Case{0x81, 32}, Case{0x82, 128}, Case{0x83, 128}}) {
        Rig r;
        r.reg(0x2115, c.vmain);
        r.reg16(0x2116, 0x1000);
        for (int i = 0; i < 4; i++) {
            r.reg(0x2118, static_cast<Byte>(0x10 + i));
            r.reg(0x2119, static_cast<Byte>(0xA0 + i));
        }
        char what[64];
        for (int i = 0; i < 4; i++) {
            snprintf(what, sizeof(what), "VMAIN $%02X word %d", c.vmain, i);
            check_eq(r.vram_read(static_cast<Word>(0x1000 + i * c.step)), static_cast<Word>(((0xA0 + i) << 8) | (0x10 + i)), what);
        }
    }
    // Step after the low byte: the high byte lands in the next word.
    Rig lo;
    lo.reg(0x2115, 0x00);
    lo.reg16(0x2116, 0x0200);
    lo.reg(0x2118, 0x11);
    lo.reg(0x2119, 0x22);
    check_eq(lo.vram_read(0x0200), 0x0011, "VMAIN $00: low byte, then step");
    check_eq(lo.vram_read(0x0201), 0x2200, "VMAIN $00: high byte in the next word");
}

auto test_vmain_remap() -> void {
    // VMAIN bits 3-2 rotate the low 8/9/10 bits of the address: the CPU writes rows of a
    // bitplane tile linearly and they land interleaved. Expected targets from the hardware rule.
    struct Case { Byte vmain; Word from; Word to; };
    const Case cases[] = {
        {0x84, 0x0001, 0x0008}, {0x84, 0x0020, 0x0001}, {0x84, 0x0021, 0x0009}, {0x84, 0x12FF, 0x12FF},
        {0x88, 0x0001, 0x0008}, {0x88, 0x0002, 0x0010}, {0x88, 0x0040, 0x0001}, {0x88, 0x01C0, 0x0007},
        {0x8C, 0x0001, 0x0008}, {0x8C, 0x0080, 0x0001}, {0x8C, 0x0380, 0x0007}, {0x8C, 0x447F, 0x47F8},
    };
    char what[64];
    for (const auto& c : cases) {
        Rig r;
        r.reg(0x2115, c.vmain);
        r.reg16(0x2116, c.from);
        r.reg(0x2118, 0x34);
        r.reg(0x2119, 0x12);
        snprintf(what, sizeof(what), "VMAIN $%02X: $%04X -> $%04X", c.vmain, c.from, c.to);
        check_eq(r.vram_read(c.to), 0x1234, what);
    }
    // Remap and a 32-word step together: still the translation of the stepped address.
    Rig r;
    r.reg(0x2115, 0x85);
    r.reg16(0x2116, 0x0000);
    for (int i = 0; i < 3; i++) { r.reg(0x2118, static_cast<Byte>(i + 1)); r.reg(0x2119, 0); }
    check_eq(r.vram_read(0x0000), 1, "remap + step 32: word 0");
    check_eq(r.vram_read(0x0001), 2, "remap + step 32: $0020 -> $0001");
    check_eq(r.vram_read(0x0002), 3, "remap + step 32: $0040 -> $0002");
}

auto test_vram_read_latch() -> void {
    // Reads return the latch, which reloads from the address before it steps: after setting
    // VMADD the first word comes back twice (the dummy-read quirk games work around).
    Rig r;
    r.vram_word(0x0300, 0x1111);
    r.vram_word(0x0301, 0x2222);
    r.vram_word(0x0302, 0x3333);
    r.reg(0x2115, 0x80);
    r.reg16(0x2116, 0x0300);
    Word got[3];
    for (auto& w : got) { const Byte low = r.read(0x2139); w = static_cast<Word>(low | (r.read(0x213A) << 8)); }
    check_eq(got[0], 0x1111, "first read: latched at VMADD write");
    check_eq(got[1], 0x1111, "second read: reloaded before the step");
    check_eq(got[2], 0x2222, "third read: next word");
    // Writes step the address without touching the latch.
    r.reg16(0x2116, 0x0300);
    r.reg(0x2118, 0x99);
    r.reg(0x2119, 0x99);
    check_eq(static_cast<Word>(r.read(0x2139) | (r.read(0x213A) << 8)), 0x1111, "latch survives port writes");
}

// SMW-style column upload: DMA mode 1 into $2118/$2119 with VMAIN $81 fills one tilemap column.
auto test_tilemap_column_dma() -> void {
    Rig r;
    for (int i = 0; i < 32; i++) {
        r.bus.write(0x7E0400 + static_cast<Address>(i * 2), static_cast<Byte>(i));
        r.bus.write(0x7E0401 + static_cast<Address>(i * 2), 0x20);
    }
    r.reg(0x2115, 0x81);
    r.reg16(0x2116, 0x1005);  // map at $1000, column 5
    r.reg(0x4300, 0x01);
    r.reg(0x4301, 0x18);
    r.reg16(0x4302, 0x0400);
    r.reg(0x4304, 0x7E);
    r.reg16(0x4305, 64);
    r.reg(0x420B, 0x01);
    char what[64];
    for (int row : {0, 1, 17, 31}) {
        snprintf(what, sizeof(what), "column 5, row %d", row);
        check_eq(r.vram_read(static_cast<Word>(0x1005 + row * 32)), static_cast<Word>(0x2000 | row), what);
    }
    check_eq(r.vram_read(0x1006), 0, "neighbouring column untouched");
}

// ---- BG maps: 64-wide/tall screens, 16x16 tiles ----------------------------------------------

// Mode 1 BG1 with a map at word $1000. Screen n (32x32 entries, 0x400 words each) is filled with
// tile n+1, a solid tile in color n+1.
auto screens_scene(Rig& r, Byte sc_size, int screens) -> void {
    r.hide_sprites();
    r.color(0, kGray);
    const Word colors[] = {kRed, kGreen, kBlue, 0x7FFF};
    for (int n = 0; n < 4; n++) {
        r.color(n + 1, colors[n]);
        r.solid4(static_cast<Word>((n + 1) * 16), n + 1);
    }
    r.reg(0x2105, 0x01);
    r.reg(0x2107, static_cast<Byte>(0x10 | sc_size));
    for (int n = 0; n < screens; n++) r.fill_map(static_cast<Word>(0x1000 + n * 0x400), static_cast<Word>(n + 1));
    r.reg(0x212C, 0x01);
}

auto bg1_scroll(Rig& r, int x, int y) -> void {
    r.reg(0x210D, static_cast<Byte>(x)); r.reg(0x210D, static_cast<Byte>(x >> 8));
    r.reg(0x210E, static_cast<Byte>(y)); r.reg(0x210E, static_cast<Byte>(y >> 8));
}

auto test_bg_screen_layouts() -> void {
    // Which screen shows at scroll (x, y) for each BGnSC size: 32x32, 64x32, 32x64, 64x64.
    struct Case { Byte size; int screens; int x, y; Word want; const char* what; };
    const Case cases[] = {
        {0, 1, 256, 256, kRed, "32x32 wraps both ways"},
        {1, 2, 0, 0, kRed, "64x32 left"},
        {1, 2, 256, 0, kGreen, "64x32 right = second screen"},
        {1, 2, 256, 256, kGreen, "64x32 wraps vertically"},
        {2, 2, 0, 256, kGreen, "32x64 bottom = second screen"},
        {2, 2, 256, 0, kRed, "32x64 wraps horizontally"},
        {3, 4, 256, 0, kGreen, "64x64 top right = screen 1"},
        {3, 4, 0, 256, kBlue, "64x64 bottom left = screen 2"},
        {3, 4, 256, 256, 0x7FFF, "64x64 bottom right = screen 3"},
        {3, 4, 512, 512, kRed, "64x64 wraps at 512"},
    };
    for (const auto& c : cases) {
        Rig r;
        screens_scene(r, c.size, c.screens);
        bg1_scroll(r, c.x, c.y);
        r.render();
        check_eq(r.px(100, 100), rgb(c.want), c.what);
    }
    // A boundary inside the view: 64x32 scrolled by 200 shows screen 0 then screen 1 at x=56.
    Rig r;
    screens_scene(r, 1, 2);
    bg1_scroll(r, 200, 0);
    r.render();
    check_eq(r.px(55, 10), rgb(kRed), "64x32: last column of screen 0");
    check_eq(r.px(56, 10), rgb(kGreen), "64x32: first column of screen 1");
}

auto test_bg_16x16_tiles() -> void {
    // BGMODE bit 4: BG1 tiles are 16x16, made of tiles n, n+1 (right), n+16 (below), n+17.
    auto scene = [](Rig& r, Word entry, Word base_tile) {
        r.hide_sprites();
        r.color(0, kGray);
        const Word colors[] = {kRed, kGreen, kBlue, 0x7FFF};
        const Word quads[] = {base_tile, static_cast<Word>((base_tile + 1) & 0x3FF), static_cast<Word>((base_tile + 16) & 0x3FF),
                              static_cast<Word>((base_tile + 17) & 0x3FF)};
        for (int q = 0; q < 4; q++) {
            r.color(q + 1, colors[q]);
            r.solid4(static_cast<Word>(quads[q] * 16), q + 1);
        }
        r.reg(0x2105, 0x11);
        r.reg(0x2107, 0x7C);  // map at $7C00, clear of the tiles
        r.fill_map(0x7C00, entry);
        r.reg(0x212C, 0x01);
        r.render();
    };
    Rig r;
    scene(r, 0x0020, 0x0020);
    check_eq(r.px(3, 3), rgb(kRed), "16x16: top left = n");
    check_eq(r.px(12, 3), rgb(kGreen), "16x16: top right = n+1");
    check_eq(r.px(3, 12), rgb(kBlue), "16x16: bottom left = n+16");
    check_eq(r.px(12, 12), rgb(0x7FFF), "16x16: bottom right = n+17");
    check_eq(r.px(19, 19), rgb(kRed), "16x16: the next map entry starts at 16 px");
    Rig f;
    scene(f, 0xC020, 0x0020);  // H and V flip swap the quarters too
    check_eq(f.px(3, 3), rgb(0x7FFF), "16x16 flipped: top left = n+17");
    check_eq(f.px(12, 12), rgb(kRed), "16x16 flipped: bottom right = n");
    Rig w;
    scene(w, 0x03FF, 0x03FF);  // n+1, n+16, n+17 wrap inside the 1024 characters
    check_eq(w.px(12, 3), rgb(kGreen), "16x16: n+1 wraps to tile 0");
    check_eq(w.px(12, 12), rgb(0x7FFF), "16x16: n+17 wraps to tile 16");
}

// ---- Windows and color math -----------------------------------------------------------------

// BG1 (mode 1) solid red over a gray backdrop.
auto bg1_scene(Rig& r) -> void {
    r.hide_sprites();
    r.color(0, kGray);
    r.color(1, kRed);
    r.reg(0x2105, 0x01);
    r.reg(0x2107, 0x10);
    r.solid4(0x0010, 1);
    r.fill_map(0x1000, 0x0001);
    r.reg(0x212C, 0x01);
}

auto test_windows() -> void {
    struct Case { Byte w12sel; Byte logic; int x; bool masked; const char* what; };
    // W1 = [40, 80], W2 = [60, 100]
    const Case cases[] = {
        {0x02, 0, 50, true, "W1 inside masks"},
        {0x02, 0, 30, false, "W1 outside shows"},
        {0x03, 0, 30, true, "W1 inverted masks outside"},
        {0x0A, 0, 90, true, "W1|W2 (OR)"},
        {0x0A, 1, 50, false, "W1&W2 (AND) only overlap"},
        {0x0A, 1, 70, true, "AND overlap"},
        {0x0A, 2, 70, false, "XOR overlap clear"},
        {0x0A, 2, 90, true, "XOR one window"},
        {0x0A, 3, 70, true, "XNOR overlap"},
        {0x0A, 3, 120, true, "XNOR neither"},
        {0x0A, 3, 50, false, "XNOR one window"},
    };
    for (const auto& c : cases) {
        Rig r;
        bg1_scene(r);
        r.reg(0x2126, 40); r.reg(0x2127, 80); r.reg(0x2128, 60); r.reg(0x2129, 100);
        r.reg(0x2123, c.w12sel);
        r.reg(0x212A, c.logic);
        r.reg(0x212E, 0x01);          // TMW: window masks BG1 on the main screen
        r.render();
        check_eq(r.px(c.x, 10), rgb(c.masked ? kGray : kRed), c.what);
    }
    // Without TMW the window does nothing.
    Rig r;
    bg1_scene(r);
    r.reg(0x2126, 0); r.reg(0x2127, 255); r.reg(0x2123, 0x02);
    r.render();
    check_eq(r.px(10, 10), rgb(kRed), "window ignored without TMW");
}

auto test_color_math() -> void {
    struct Case { Byte cgwsel; Byte cgadsub; Word fixed_r; Word want; const char* what; };
    // BG1 red (31,0,0) on main; sub screen = BG2 green where enabled.
    const Case cases[] = {
        {0x00, 0x01, 0, 0x001F, "fixed color 0 add = unchanged"},
        {0x00, 0x01, 0x10, 0x001F, "add clamps at 31"},
        {0x00, 0x81, 0x10, 0x000F, "subtract fixed 16"},
        {0x00, 0xC1, 0x10, 0x0007, "subtract then half"},
        {0x02, 0x01, 0, 0x03FF, "add sub screen (green)"},
        {0x02, 0x41, 0, 0x01EF, "add sub screen, half"},
        {0x00, 0x00, 0x10, 0x001F, "BG1 not enabled for math"},
        {0x30, 0x01, 0x10, 0x001F, "math prevented everywhere"},
        {0xC0, 0x00, 0, 0x0000, "clip main to black"},
        {0xC2, 0x41, 0, 0x03E0, "clipped main + sub, no half"},
    };
    for (const auto& c : cases) {
        Rig r;
        bg1_scene(r);
        r.color(16 + 1, kGreen);      // BG2 palette 1
        r.reg(0x2108, 0x14);
        r.reg(0x210B, 0x00);
        r.fill_map(0x1400, 0x0401);
        r.reg(0x212D, 0x02);          // TS: BG2
        r.reg(0x2132, static_cast<Byte>(0x20 | c.fixed_r));  // COLDATA red
        r.reg(0x2130, c.cgwsel);
        r.reg(0x2131, c.cgadsub);
        r.render();
        check_eq(r.px(10, 10), rgb(c.want), c.what);
    }

    // Transparent sub screen: the fixed color stands in and halving is skipped.
    Rig r;
    bg1_scene(r);
    r.reg(0x2132, 0x40 | 0x1F);       // fixed green 31
    r.reg(0x2130, 0x02);
    r.reg(0x2131, 0x41);
    r.render();
    check_eq(r.px(10, 10), rgb(0x03FF), "sub backdrop = fixed color, no half");

    // OBJ palettes 0-3 never blend, 4-7 do.
    Rig o;
    sprite_rig(o);
    o.solid4(0x0010, 1);
    o.sprite(0, 10, 10, 1, 0x00);     // palette 0 red
    o.sprite(1, 30, 10, 1, 0x08);     // palette 4 blue
    o.reg(0x2132, 0x20 | 0x1F);       // fixed red 31
    o.reg(0x2131, 0x10);              // OBJ math, add
    o.render();
    check_eq(o.px(10, 10), rgb(kRed), "OBJ palette 0: no math");
    check_eq(o.px(30, 10), rgb(kBlue | kRed), "OBJ palette 4: math");

    // Color window drives the clip region.
    Rig w;
    bg1_scene(w);
    w.reg(0x2126, 0); w.reg(0x2127, 31);
    w.reg(0x2125, 0x20);              // color window = W1
    w.reg(0x2130, 0x80);              // clip to black inside the color window
    w.render();
    check_eq(w.px(10, 10), kBlack, "clipped inside the color window");
    check_eq(w.px(40, 10), rgb(kRed), "untouched outside");
}

auto test_mid_line_write() -> void {
    // A CGRAM write mid-line only changes the pixels after the beam.
    Rig r;
    bg1_scene(r);
    r.reg(0x2100, 0x0F);
    r.step(50u * kLine + 4 * (22 + 128));  // line 50, beam at pixel 128
    r.color(1, kBlue);
    r.step(5u * kLine);
    check_eq(r.px(100, 49), rgb(kRed), "left of the beam keeps the old color");
    check_eq(r.px(200, 49), rgb(kBlue), "right of the beam gets the new one");
    check_eq(r.px(100, 50), rgb(kBlue), "next line all new");
}

// ---- Timing, IRQs, joypad -------------------------------------------------------------------

auto test_h_irq() -> void {
    Rig r;
    r.reg16(0x4207, 100);             // HTIME = dot 100: fires at H=103.5, 14 + 400 clocks (Anomie)
    r.reg(0x4200, 0x10);
    r.step(5u * kLine + 300);
    check(r.bus.irq_pending(), "fired on the earlier lines");
    r.read(0x4211);
    r.step(113);
    check(!r.bus.irq_pending(), "not before HTIME + 3.5 dots");
    r.step(1);
    check(r.bus.irq_pending(), "fires at HTIME + 3.5 dots");
    check_eq(r.read(0x4211), 0x80, "TIMEUP set");
    check_eq(r.read(0x4211), 0x00, "TIMEUP cleared by reading");
    check(!r.bus.irq_pending(), "line released");
    r.step(kLine);
    check(r.bus.irq_pending(), "again next line");
    r.reg(0x4200, 0x00);
    check(!r.bus.irq_pending(), "disabling IRQs drops it");

    // HTIME 339: 14 + 1356 + 4 (two long dots) = 1374 clocks, 10 into the next line.
    Rig end;
    end.reg16(0x4207, 339);
    end.reg(0x4200, 0x10);
    end.step(6u * kLine);
    end.read(0x4211);
    end.step(9);
    check(!end.bus.irq_pending(), "HTIME 339: not at the end of the line");
    end.step(1);
    check(end.bus.irq_pending(), "HTIME 339: 10 clocks into the next line");
}

auto test_v_and_hv_irq() -> void {
    Rig r;
    r.reg16(0x4209, 10);
    r.reg(0x4200, 0x20);              // V-IRQ: start of line 10
    r.step(10u * kLine + 9);
    check(!r.bus.irq_pending(), "not before line 10, H=2.5");
    r.step(1);
    check(r.bus.irq_pending(), "V-IRQ at line 10, 1374 clocks after line 9 starts");
    check_eq(r.ppu.vcounter(), 10u, "on line 10");

    Rig hv;
    hv.reg16(0x4209, 20);
    hv.reg16(0x4207, 200);
    hv.reg(0x4200, 0x30);             // HV: line 20 at dot 200
    hv.step(20u * kLine + 813);
    check(!hv.bus.irq_pending(), "not before dot 203.5");
    hv.step(1);
    check(hv.bus.irq_pending(), "HV-IRQ at line 20 dot 203.5");
    hv.read(0x4211);
    hv.step(kLine);
    check(!hv.bus.irq_pending(), "only on VTIME");

    Rig far;
    far.reg16(0x4207, 340);           // past the last dot: never
    far.reg(0x4200, 0x10);
    far.step(3u * kLine);
    check(!far.bus.irq_pending(), "HTIME 340 never fires");
}

auto test_hvbjoy_and_nmi_flag() -> void {
    Rig r;
    r.step(10u * kLine + 100);
    check_eq(r.read(0x4212) & 0xC0, 0x00, "active display");
    r.step(PPU::kHblankClock);
    check_eq(r.read(0x4212) & 0xC0, 0x40, "H-blank flag");
    r.step(215u * kLine);
    check(r.read(0x4212) & 0x80, "V-blank flag");
    check_eq(r.read(0x4210), 0x82, "RDNMI set + version 2");
    check_eq(r.read(0x4210), 0x02, "RDNMI cleared by read");

    // Enabling NMI while the flag is still set fires it.
    Rig n;
    n.step(226u * kLine);
    check(!n.bus.poll_nmi(), "no NMI while disabled");
    n.reg(0x4200, 0x80);
    check(n.bus.poll_nmi(), "late enable with RDNMI set fires");
}

auto test_auto_joypad() -> void {
    Rig r;
    r.reg(0x4200, 0x01);              // auto-read on
    r.bus.set_joypad1(0x8040);        // B + X
    check_eq(r.read(0x4219), 0x00, "latch waits for auto-read");
    r.step(225u * kLine + 200);       // into the read window
    check(r.read(0x4212) & 0x01, "busy during auto-read");
    check_eq(r.read(0x4218), 0x40, "$4218 low");
    check_eq(r.read(0x4219), 0x80, "$4219 high");
    r.step(4224);
    check((r.read(0x4212) & 0x01) == 0, "busy clears after 4224 clocks");

    // Manual serial read through $4016.
    r.reg(0x4016, 1);
    r.reg(0x4016, 0);
    check_eq(r.read(0x4016) & 1, 1, "bit 15 (B) first");
    for (int i = 0; i < 8; i++) r.read(0x4016);
    check_eq(r.read(0x4016) & 1, 1, "bit 6 (X) tenth");
}

auto test_master_clocks() -> void {
    Rig r;
    struct Case { Address addr; int clocks; };
    const Case cases[] = {
        {0x000000, 8}, {0x7E1234, 8}, {0x002100, 6}, {0x004016, 12}, {0x004200, 6},
        {0x004300, 6}, {0x006000, 8}, {0x008000, 8}, {0x808000, 8}, {0xC00000, 8}, {0x400000, 8},
    };
    for (const auto& c : cases) check_eq(static_cast<unsigned>(r.bus.access_clocks(c.addr)), static_cast<unsigned>(c.clocks), "access speed");
    r.reg(0x420D, 0x01);              // FastROM
    check_eq(static_cast<unsigned>(r.bus.access_clocks(0x808000)), 6, "FastROM $80:8000");
    check_eq(static_cast<unsigned>(r.bus.access_clocks(0xC00000)), 6, "FastROM $C0:0000");
    check_eq(static_cast<unsigned>(r.bus.access_clocks(0x008000)), 8, "bank $00 stays slow");
    check_eq(static_cast<unsigned>(r.bus.access_clocks(0x400000)), 8, "bank $40 stays slow");

    // NOP: opcode fetch + one internal cycle. 8+6 from slow ROM, 6+6 from FastROM.
    r.reg(0x420D, 0x00);
    r.cpu.reset();
    r.bus.end_cpu_step(0);
    r.bus.begin_cpu_step();
    int cycles = r.cpu.step();
    check_eq(static_cast<unsigned>(r.bus.end_cpu_step(cycles)), 14, "NOP from SlowROM");
    r.reg(0x420D, 0x01);
    auto regs = r.cpu.registers();
    regs.pbr = 0x80;
    r.cpu.set_registers(regs);
    r.bus.end_cpu_step(0);
    r.bus.begin_cpu_step();
    cycles = r.cpu.step();
    check_eq(static_cast<unsigned>(r.bus.end_cpu_step(cycles)), 12, "NOP from FastROM");
}

// CPU program in emulation mode: CLI, then spin. IRQ handler stores $5A to $0200 and acks.
auto test_cpu_takes_irq() -> void {
    std::vector<Byte> rom(0x8000, 0xEA);
    const Byte main_code[] = {0x58, 0x4C, 0x01, 0x80};                    // CLI; JMP $8001
    const Byte handler[] = {0xA9, 0x5A, 0x8D, 0x00, 0x02, 0xAD, 0x11, 0x42, 0x40};  // LDA/STA/LDA $4211/RTI
    std::copy(std::begin(main_code), std::end(main_code), rom.begin());
    std::copy(std::begin(handler), std::end(handler), rom.begin() + 0x100);
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;  // reset
    rom[0x7FFE] = 0x00; rom[0x7FFF] = 0x81;  // IRQ/BRK (emulation)
    Rig r;
    r.bus.load_test_rom(rom);
    r.cpu.reset();
    r.reg16(0x4209, 3);
    r.reg(0x4200, 0x20);              // V-IRQ at line 3
    for (int i = 0; i < 4000 && r.read(0x7E0200) != 0x5A; i++) {
        r.bus.begin_cpu_step();
        const int c = r.cpu.step();
        r.ppu.step(static_cast<uint32_t>(r.bus.end_cpu_step(c)));
    }
    check_eq(r.read(0x7E0200), 0x5A, "IRQ handler ran");
    check_eq(static_cast<unsigned>(r.ppu.vcounter()), 3, "during line 3");

    // WAI with I set wakes on the IRQ without taking it.
    std::vector<Byte> rom2(0x8000, 0xEA);
    const Byte wai_code[] = {0x78, 0xCB, 0xA9, 0x77, 0x8D, 0x00, 0x02, 0xDB};  // SEI; WAI; LDA #$77; STA; STP
    std::copy(std::begin(wai_code), std::end(wai_code), rom2.begin());
    rom2[0x7FFC] = 0x00; rom2[0x7FFD] = 0x80;
    Rig w;
    w.bus.load_test_rom(rom2);
    w.cpu.reset();
    w.reg16(0x4209, 2);
    w.reg(0x4200, 0x20);
    for (int i = 0; i < 4000 && !w.cpu.stopped(); i++) {
        w.bus.begin_cpu_step();
        const int c = w.cpu.step();
        w.ppu.step(static_cast<uint32_t>(w.bus.end_cpu_step(c)));
    }
    check_eq(w.read(0x7E0200), 0x77, "WAI resumed after a masked IRQ");
    check_eq(static_cast<unsigned>(w.ppu.vcounter()), 2, "woke on line 2");
}

// A loop polling $4210 (or $4211) with the interrupt enabled whose handler also reads the flag:
// Final Fantasy III's "play song" event spins on LDA $4210 / BPL at $C0:B7A0 with NMI on, and its
// NMI handler reads $4210. On hardware the poll sees the flag whenever V-blank starts during the
// LDA (before its read cycle); the NMI follows the LDA, and RTI restores N=1. The PPU here runs
// after each instruction, so without a look-ahead the NMI always came first and the loop spun
// forever (black screen before the intro). Returns {loop exits, handler runs} over `frames`.
struct PollResult { unsigned exits; unsigned handled; };
auto run_poll_loop(Word flag_reg, Byte enable, int frames) -> PollResult {
    std::vector<Byte> rom(0x8000, 0xEA);
    const Byte lo = static_cast<Byte>(flag_reg), hi = static_cast<Byte>(flag_reg >> 8);
    const Byte main_code[] = {
        0x18, 0xFB, 0xE2, 0x20, 0xC2, 0x10,  // CLC; XCE; SEP #$20; REP #$10 (native, M=1, X=0)
        0xA9, enable, 0x8D, 0x00, 0x42,      // LDA #enable; STA $4200
        0x58,                                // CLI
        0xAD, lo, hi, 0x10, 0xFB,            // loop: LDA flag; BPL loop
        0xEE, 0x00, 0x02, 0x80, 0xF6,        // INC $0200; BRA loop
    };
    const Byte handler[] = {0x48, 0xAD, lo, hi, 0xEE, 0x01, 0x02, 0x68, 0x40};  // PHA; LDA flag; INC $0201; PLA; RTI
    std::copy(std::begin(main_code), std::end(main_code), rom.begin());
    std::copy(std::begin(handler), std::end(handler), rom.begin() + 0x100);
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;  // reset
    rom[0x7FEA] = 0x00; rom[0x7FEB] = 0x81;  // native NMI
    rom[0x7FEE] = 0x00; rom[0x7FEF] = 0x81;  // native IRQ
    Rig r;
    r.bus.load_test_rom(rom);
    r.cpu.reset();
    r.reg16(0x4209, 100);  // VTIME for the V-IRQ variant
    r.bus.write(0x7E0200, 0);  // WRAM powers up as $55
    r.bus.write(0x7E0201, 0);
    int done = 0;
    while (done < frames) {
        r.bus.begin_cpu_step();
        const int c = r.cpu.step();
        if (r.ppu.step(static_cast<uint32_t>(r.bus.end_cpu_step(c)))) done++;
    }
    return {r.read(0x7E0200), r.read(0x7E0201)};
}

auto test_flag_poll_with_handler() -> void {
    const PollResult nmi = run_poll_loop(0x4210, 0x80, 30);
    check(nmi.handled >= 29 && nmi.handled <= 31, "an NMI every frame");
    check(nmi.exits >= 5, "the $4210 poll sees V-blank despite the NMI handler (FF3 $C0:B7A0)");
    check(nmi.exits <= nmi.handled, "at most one poll exit per V-blank");

    const PollResult irq = run_poll_loop(0x4211, 0x20, 30);
    // Unlike the NMI edge, a $4211 read acknowledges the IRQ: a poll that sees it takes it.
    check(irq.handled + irq.exits >= 29 && irq.handled + irq.exits <= 31, "each V-IRQ goes to the handler or the poll");
    check(irq.exits >= 5, "the $4211 poll sees TIMEUP despite the IRQ handler");
    check(irq.handled >= 5, "the IRQ is still taken when the poll misses it");

    // The look-ahead is per instruction: outside a CPU step a read sees the PPU as it is.
    Rig r;
    r.step(225u * kLine - 4);
    check_eq(r.read(0x4210), 0x02, "not yet in V-blank");
    r.step(8);
    check_eq(r.read(0x4210), 0x82, "V-blank flag after the PPU gets there");
}

// The same poll with FF3's interrupt layout: a V-IRQ at line 0 whose handler (FieldIRQ) turns the
// IRQ off, and an NMI handler (FieldNMI) that reads $4210 and turns it back on. The handlers start
// at instruction boundaries, so the loop's phase at V-blank repeats from frame to frame; padding
// them by `nmi_pad` NOPs (+ an optional WRAM read) and `irq_pad` NOPs walks through every phase.
// Returns the frames until the loop exits, or 0 if it doesn't within `frames`.
auto run_locked_poll(int nmi_pad, int irq_pad, bool wram_read, int frames) -> int {
    std::vector<Byte> rom(0x8000, 0xEA);
    const Byte main_code[] = {
        0x18, 0xFB, 0xE2, 0x20, 0xC2, 0x10,  // CLC; XCE; SEP #$20; REP #$10
        0xA9, 0xA1, 0x8D, 0x00, 0x42,        // LDA #$A1; STA $4200 (NMI + V-IRQ, VTIME = 0)
        0x58,                                // CLI
        0xAD, 0x10, 0x42, 0x10, 0xFB,        // loop: LDA $4210; BPL loop
        0xDB,                                // STP
    };
    std::vector<Byte> nmi = {0x08, 0xC2, 0x30, 0x48, 0xE2, 0x20, 0xAD, 0x10, 0x42, 0xA9, 0xA1, 0x8D, 0x00, 0x42};
    for (int i = 0; i < nmi_pad; i++) nmi.push_back(0xEA);
    if (wram_read) { nmi.push_back(0xA5); nmi.push_back(0x00); }
    for (const Byte b : {0xC2, 0x30, 0x68, 0x28, 0x40}) nmi.push_back(b);  // REP #$30; PLA; PLP; RTI
    std::vector<Byte> irq = {0x08, 0xC2, 0x30, 0x48, 0xE2, 0x20, 0xAD, 0x11, 0x42, 0xA9, 0x81, 0x8D, 0x00, 0x42};
    for (int i = 0; i < irq_pad; i++) irq.push_back(0xEA);
    for (const Byte b : {0xC2, 0x30, 0x68, 0x28, 0x40}) irq.push_back(b);
    std::copy(std::begin(main_code), std::end(main_code), rom.begin());
    std::copy(nmi.begin(), nmi.end(), rom.begin() + 0x100);
    std::copy(irq.begin(), irq.end(), rom.begin() + 0x200);
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
    rom[0x7FEA] = 0x00; rom[0x7FEB] = 0x81;  // native NMI
    rom[0x7FEE] = 0x00; rom[0x7FEF] = 0x82;  // native IRQ
    Rig r;
    r.bus.load_test_rom(rom);
    r.cpu.reset();
    r.reg16(0x4209, 0);
    for (int done = 0; done < frames;) {
        if (r.cpu.stopped()) return done + 1;
        r.bus.begin_cpu_step();
        const int c = r.cpu.step();
        if (r.ppu.step(static_cast<uint32_t>(r.bus.end_cpu_step(c)))) done++;
    }
    return 0;
}

auto test_flag_poll_every_phase() -> void {
    // Every handler timing leaves the loop at the first V-blank. Before the slot 4 fix, 8 of these
    // 130 timings spun forever (the slot 4 hang).
    int stuck = 0, slowest = 0;
    for (int nmi_pad = 0; nmi_pad < 13; nmi_pad++) {
        for (int irq_pad = 0; irq_pad < 5; irq_pad++) {
            for (const bool wram : {false, true}) {
                const int frames = run_locked_poll(nmi_pad, irq_pad, wram, 20);
                if (frames == 0) stuck++;
                slowest = std::max(slowest, frames);
            }
        }
    }
    check_eq(static_cast<unsigned>(stuck), 0, "no handler timing locks the $4210 poll out");
    check(slowest <= 2, "the poll exits at the first V-blank");

    // The hold delays the NMI by one instruction, not more: it still fires every frame.
    const PollResult nmi = run_poll_loop(0x4210, 0x80, 30);
    check(nmi.handled >= 29 && nmi.handled <= 31, "one NMI per frame with the hold");
    check(nmi.exits >= 28, "and the poll sees every V-blank");
}

// ---- DMA and HDMA ---------------------------------------------------------------------------

auto test_dma_modes() -> void {
    // Eight source bytes 1..8 in WRAM through each mode into $2118 (VMAIN increments on $2119).
    struct Case { Byte mode; std::array<Word, 4> words; };
    const Case cases[] = {
        {0, {0x0008, 0, 0, 0}},
        {1, {0x0201, 0x0403, 0x0605, 0x0807}},
        {2, {0x0008, 0, 0, 0}},
        {3, {0x0302, 0x0400, 0x0706, 0x0800}},
        {4, {0x0201, 0x0605, 0, 0}},
        {5, {0x0201, 0x0403, 0x0605, 0x0807}},
        {6, {0x0008, 0, 0, 0}},
        {7, {0x0302, 0x0400, 0x0706, 0x0800}},
    };
    for (const auto& c : cases) {
        Rig r;
        for (int i = 0; i < 8; i++) r.bus.write(0x7E0100 + static_cast<Address>(i), static_cast<Byte>(i + 1));
        r.reg(0x2115, 0x80);
        r.reg16(0x2116, 0x0000);
        r.reg(0x4300, c.mode);
        r.reg(0x4301, 0x18);
        r.reg16(0x4302, 0x0100);
        r.reg(0x4304, 0x7E);
        r.reg16(0x4305, 8);
        r.reg(0x420B, 0x01);
        char what[64];
        for (int w = 0; w < 4; w++) {
            snprintf(what, sizeof(what), "mode %d word %d", c.mode, w);
            check_eq(r.vram_read(static_cast<Word>(w)), c.words[static_cast<size_t>(w)], what);
        }
        check_eq(r.read(0x4305) | (r.read(0x4306) << 8), 0, "DAS counts to 0");
        check_eq(r.read(0x4302) | (r.read(0x4303) << 8), 0x0108, "A1T advanced");
        if (c.mode == 4) {             // the 3rd/4th bytes went to $211A/$211B (M7A = $0804)
            r.reg(0x211C, 0x00);
            r.reg(0x211C, 0x01);
            check_eq(r.read(0x2134) | (r.read(0x2135) << 8), 0x0804, "mode 4 reached $211B");
        }
    }

    // Fixed and decrementing A-bus addresses, and the CPU time the transfer costs.
    Rig r;
    r.bus.write(0x7E0200, 0x11);
    r.bus.write(0x7E01FF, 0x22);
    r.reg(0x2121, 0);
    r.reg(0x4300, 0x18);              // fixed source, mode 0, into CGDATA
    r.reg(0x4301, 0x22);
    r.reg16(0x4302, 0x0200);
    r.reg(0x4304, 0x7E);
    r.reg16(0x4305, 4);
    r.bus.end_cpu_step(0);
    r.reg(0x420B, 0x01);
    const auto stolen = r.bus.end_cpu_step(0);
    check(stolen >= 8 + 8 + 4 * 8, "DMA time is stolen from the CPU");
    r.reg(0x2121, 0);
    check_eq(r.read(0x213B), 0x11, "fixed source repeats");
    check_eq(r.read(0x213B), 0x11, "fixed source repeats (2)");
    check_eq(r.read(0x4302) | (r.read(0x4303) << 8), 0x0200, "fixed A1T unchanged");
    r.reg(0x2121, 0);
    r.reg(0x4300, 0x10);              // decrement
    r.reg16(0x4305, 2);
    r.reg(0x420B, 0x01);
    r.reg(0x2121, 0);
    check_eq(r.read(0x213B), 0x11, "decrement: first byte");
    check_eq(r.read(0x213B), 0x22, "decrement: second byte from $01FF");

    // B-bus to A-bus: read VRAM back through $2139/$213A with mode 1.
    Rig b;
    b.vram_word(0x0040, 0xBEEF);
    b.reg(0x2115, 0x80);
    b.reg16(0x2116, 0x0040);
    b.read(0x2139);                   // the prefetch was loaded by the address write
    b.reg16(0x2116, 0x0040);
    b.reg(0x4300, 0x81);
    b.reg(0x4301, 0x39);
    b.reg16(0x4302, 0x0300);
    b.reg(0x4304, 0x7E);
    b.reg16(0x4305, 2);
    b.reg(0x420B, 0x01);
    check_eq(b.read(0x7E0300) | (b.read(0x7E0301) << 8), 0xBEEF, "B->A DMA");
}

// HDMA into $2180 (WRAM data port): every byte HDMA writes lands in WRAM in order, so the
// sequence shows exactly which lines transferred.
auto hdma_rig(Rig& r, Byte dmap, const std::vector<Byte>& table) -> void {
    for (size_t i = 0; i < table.size(); i++) r.bus.write(0x7E1000 + static_cast<Address>(i), table[i]);
    r.reg(0x2181, 0x00); r.reg(0x2182, 0x20); r.reg(0x2183, 0x00);  // WRAM port at $7E2000
    r.reg(0x4300, dmap);
    r.reg(0x4301, 0x80);
    r.reg16(0x4302, 0x1000);
    r.reg(0x4304, 0x7E);
    r.reg(0x4307, 0x7E);
    r.reg(0x420C, 0x01);
}

auto test_hdma_direct() -> void {
    Rig r;
    // 3 lines, write once; 2 lines repeat (a byte each line); 1 line; end.
    hdma_rig(r, 0x00, {0x03, 0x11, 0x82, 0x21, 0x22, 0x01, 0x31, 0x00});
    for (int i = 0; i < 8; i++) r.bus.write(0x7E2000 + static_cast<Address>(i), 0xEE);
    r.step(1u * kLine);               // line 0 (init + first transfer)
    check_eq(r.read(0x7E2000), 0x11, "line 0 transfers");
    check_eq(r.read(0x7E2001), 0xEE, "nothing else yet");
    r.step(3u * kLine);               // lines 1-3
    check_eq(r.read(0x7E2001), 0x21, "repeat entry starts on line 3");
    r.step(1u * kLine);
    check_eq(r.read(0x7E2002), 0x22, "repeat writes again on line 4");
    r.step(1u * kLine);
    check_eq(r.read(0x7E2003), 0x31, "next entry on line 5");
    r.step(50u * kLine);
    check_eq(r.read(0x7E2004), 0xEE, "terminated after the 0 entry");
    check(r.bus.dma_channel(0).hdma_terminated, "channel terminated");

    // Next frame starts the table over.
    r.reg(0x2181, 0x00); r.reg(0x2182, 0x30);
    r.step(262u * kLine - 56u * kLine + kLine);
    check_eq(r.read(0x7E3000), 0x11, "reloaded at the next frame");
}

auto test_hdma_indirect() -> void {
    Rig r;
    for (int i = 0; i < 4; i++) r.bus.write(0x7E1100 + static_cast<Address>(i), static_cast<Byte>(0xA0 + i));
    for (int i = 0; i < 4; i++) r.bus.write(0x7E1200 + static_cast<Address>(i), static_cast<Byte>(0xB0 + i));
    // 2 lines once from $1100; 3 lines repeating from $1200; end. Mode 2 = 2 bytes per transfer,
    // both to $2180.
    hdma_rig(r, 0x42, {0x02, 0x00, 0x11, 0x83, 0x00, 0x12, 0x00});
    r.step(6u * kLine);
    const Byte want[] = {0xA0, 0xA1, 0xB0, 0xB1, 0xB2, 0xB3};
    for (int i = 0; i < 6; i++) check_eq(r.read(0x7E2000 + static_cast<Address>(i)), want[i], "indirect byte");
    const auto& ch = r.bus.dma_channel(0);
    check(ch.hdma_terminated, "indirect channel ended");
    check_eq(ch.a2a, 0x1007, "table pointer past the 0 entry");
}

auto test_hdma_time() -> void {
    Rig r;
    hdma_rig(r, 0x00, {0x81, 0x11, 0x00});
    r.bus.end_cpu_step(0);
    r.step(1u * kLine);
    check(r.bus.end_cpu_step(0) >= 18 + 8 + 8, "HDMA steals time");
}


// ---- PPU 3: brightness, mosaic, hi-res, interlace, offset-per-tile, direct color, overscan --

auto test_brightness_curve() -> void {
    // (brightness + 1) / 16 of the bit-replicated channel; brightness 0 is 1/64, not black.
    const int levels[] = {15, 7, 0};
    const uint32_t want[] = {0xFFFF0000, 0xFF7F0000, 0xFF030000};
    for (int i = 0; i < 3; i++) {
        Rig r;
        bg1_scene(r);
        r.render(static_cast<Byte>(levels[i]));
        check_eq(r.px(10, 10), want[i], "red 31 at brightness 15 / 7 / 0");
        check_eq(r.px(10, 10), rgb(kRed, levels[i]), "matches the table");
    }
    check_eq(bitforge::ppu::tables::kBrightness[15][16], 132, "16 -> 132 at full brightness");
    check_eq(bitforge::ppu::tables::kBrightness[0][31], 3, "31 at brightness 0");
}

// Mode 1 BG1 over a gray backdrop, every map entry tile 1. Tile 1's pixels come from `pixel`;
// CGRAM colors 1-15 are distinct.
template <typename F> auto pattern_scene(Rig& r, F pixel, Byte bgmode = 0x01) -> void {
    r.hide_sprites();
    r.color(0, kGray);
    for (int c = 1; c < 16; c++) r.color(c, static_cast<Word>(c * 0x0421 + 0x0400 * (c & 1)));
    r.reg(0x2105, bgmode);
    r.reg(0x2107, 0x7C);  // map at word $7C00
    r.tile4_fn(0x0010, pixel);
    r.fill_map(0x7C00, 0x0001);
    r.reg(0x212C, 0x01);
}
auto pal(Rig& r, int c) -> uint32_t {
    (void)r;
    return rgb(static_cast<Word>(c * 0x0421 + 0x0400 * (c & 1)));
}

auto test_mosaic_horizontal() -> void {
    Rig r;
    pattern_scene(r, [](int x, int) { return x + 1; });
    r.reg(0x2106, 0x31);  // 4x4 blocks on BG1
    r.render();
    check_eq(r.px(0, 5), pal(r, 1), "x 0 = block start");
    check_eq(r.px(3, 5), pal(r, 1), "x 3 copies x 0");
    check_eq(r.px(4, 5), pal(r, 5), "x 4 starts the next block");
    check_eq(r.px(7, 5), pal(r, 5), "x 7 copies x 4");
    check_eq(r.px(9, 5), pal(r, 1), "x 9 copies x 8");
    Rig off;
    pattern_scene(off, [](int x, int) { return x + 1; });
    off.reg(0x2106, 0x32);  // mosaic on BG2 only: BG1 untouched
    off.render();
    check_eq(off.px(3, 5), pal(off, 4), "other BG's mosaic bit leaves BG1 alone");
}

auto test_mosaic_vertical() -> void {
    // Row y of the tile is color y + 1; screen row 0 is line 1, which shows BG row 1.
    auto rows = [](int, int y) { return y + 1; };
    Rig r;
    pattern_scene(r, rows);
    r.reg(0x2106, 0x31);
    r.render();
    check_eq(r.px(5, 0), pal(r, 2), "rows 0-3 show BG row 1");
    check_eq(r.px(5, 3), pal(r, 2), "row 3 still BG row 1");
    check_eq(r.px(5, 4), pal(r, 6), "row 4 starts a block at BG row 5");
    check_eq(r.px(5, 7), pal(r, 6), "row 7 still BG row 5");

    // Switched on mid-frame: blocks start on the next line.
    Rig m;
    pattern_scene(m, rows);
    m.reg(0x2100, 0x0F);
    m.run_to_hblank(10);
    m.reg(0x2106, 0x31);
    m.run_to(225, 0);
    check_eq(m.px(5, 9), pal(m, 3), "line 10 drawn before the write");
    check_eq(m.px(5, 10), pal(m, 4), "line 11 starts a block (BG row 11)");
    check_eq(m.px(5, 13), pal(m, 4), "line 14 copies line 11");
    check_eq(m.px(5, 14), pal(m, 8), "line 15 starts the next block");

    // A new size waits for the current block to finish.
    Rig z;
    pattern_scene(z, rows);
    z.reg(0x2106, 0x31);
    z.reg(0x2100, 0x0F);
    z.run_to_hblank(2);
    z.reg(0x2106, 0x71);  // 8x8 from the next block
    z.run_to(225, 0);
    check_eq(z.px(5, 2), pal(z, 2), "line 3 finishes the 4-line block");
    check_eq(z.px(5, 4), pal(z, 6), "line 5 starts an 8-line block");
    check_eq(z.px(5, 11), pal(z, 6), "line 12 still in it");
}

auto test_hires_mode5() -> void {
    // Mode 5: BG1 tiles are 16 pixels wide (characters n and n+1) on a 512-pixel line; even
    // pixels come from the sub screen, odd ones from the main screen.
    auto scene = [](Rig& r, Byte tm, Byte ts) {
        pattern_scene(r, [](int x, int) { return x + 1; }, 0x05);
        r.tile4_fn(0x0020, [](int x, int) { return 15 - x; });  // character 2 = n + 1
        r.reg(0x212C, tm);
        r.reg(0x212D, ts);
    };
    Rig r;
    scene(r, 0x01, 0x01);
    r.render();
    check_eq(static_cast<unsigned>(r.ppu.frame_width()), 512, "hi-res frame is 512 wide");
    check_eq(r.px(0, 5), pal(r, 1), "hi-res pixel 0");
    check_eq(r.px(7, 5), pal(r, 8), "hi-res pixel 7");
    check_eq(r.px(8, 5), pal(r, 15), "pixel 8 is character n + 1");
    check_eq(r.px(16, 5), pal(r, 1), "the next tile starts at 16");

    Rig m;
    scene(m, 0x01, 0x00);  // BG1 on the main screen only
    m.render();
    check_eq(m.px(4, 5), rgb(kGray), "even pixel: empty sub screen shows the backdrop");
    check_eq(m.px(5, 5), pal(m, 6), "odd pixel: main screen");

    Rig s;
    scene(s, 0x01, 0x01);
    s.reg(0x210D, 1); s.reg(0x210D, 0);  // H scroll 1 moves 2 hi-res pixels
    s.render();
    check_eq(s.px(0, 5), pal(s, 3), "scroll counts in hi-res pixels x2");

    Rig mo;
    scene(mo, 0x01, 0x01);
    mo.reg(0x2106, 0x11);  // 2-dot blocks = 4 hi-res pixels
    mo.render();
    check_eq(mo.px(1, 5), pal(mo, 1), "mosaic: odd pixel takes the block's first pixel");
    check_eq(mo.px(3, 5), pal(mo, 1), "mosaic block is 4 hi-res pixels");
    check_eq(mo.px(4, 5), pal(mo, 5), "next block");
}

auto test_pseudo_hires() -> void {
    // SETINI bit 3 in mode 1: even pixels show the sub screen (BG2 green), odd the main (BG1 red).
    Rig r;
    bg1_scene(r);
    r.color(16 + 1, kGreen);
    r.reg(0x2108, 0x14);
    r.fill_map(0x1400, 0x0401);
    r.reg(0x212D, 0x02);
    r.reg(0x2133, 0x08);
    r.render();
    check_eq(static_cast<unsigned>(r.ppu.frame_width()), 512, "pseudo hi-res frame is 512 wide");
    check_eq(r.px(20, 5), rgb(kGreen), "even = sub screen");
    check_eq(r.px(21, 5), rgb(kRed), "odd = main screen");

    // Back to 256: the next frame stays 512 (lines doubled), the one after is 256 again.
    r.reg(0x2133, 0x00);
    r.finish_frame();
    r.render();
    check_eq(static_cast<unsigned>(r.ppu.frame_width()), 512, "one frame keeps the width");
    check_eq(r.px(20, 5), rgb(kRed), "lo-res line doubled (left)");
    check_eq(r.px(21, 5), rgb(kRed), "lo-res line doubled (right)");
    r.finish_frame();
    r.render();
    check_eq(static_cast<unsigned>(r.ppu.frame_width()), 256, "then 256 again");
}

auto test_hires_mid_frame() -> void {
    // Mode 1 at the top, mode 5 from line 101: the frame widens and the lines already drawn
    // are doubled in place.
    Rig r;
    bg1_scene(r);
    r.reg(0x2100, 0x0F);
    r.run_to_hblank(100);
    r.reg(0x2105, 0x05);
    r.run_to(225, 0);
    check_eq(static_cast<unsigned>(r.ppu.frame_width()), 512, "widened");
    check_eq(r.px(40, 50), rgb(kRed), "earlier line kept (left)");
    check_eq(r.px(41, 50), rgb(kRed), "earlier line kept (right)");
    check_eq(r.px(33, 150), rgb(kRed), "hi-res line, character n");
    check_eq(r.px(41, 150), rgb(kGray), "hi-res line, empty character n + 1");
}

auto test_interlace() -> void {
    // SETINI bit 0: 448 rows, one field per frame; the even field (0) has 263 lines.
    auto rows = [](int, int y) { return y + 1; };
    Rig r;
    pattern_scene(r, rows);
    r.reg(0x2133, 0x01);
    r.render();
    check_eq(static_cast<unsigned>(r.ppu.frame_height()), 448, "interlaced frame is 448 rows");
    check((r.read(0x213F) & 0x80) == 0, "field 0 first");
    check_eq(static_cast<unsigned>(r.finish_frame()), 263, "field 0 has 263 lines");
    r.render();
    check((r.read(0x213F) & 0x80) != 0, "then field 1");
    check_eq(static_cast<unsigned>(r.finish_frame()), 262, "field 1 has 262 lines");
    // Outside modes 5/6 both fields draw the same picture.
    check_eq(r.px(5, 0), pal(r, 2), "row 0 (field 0, line 1)");
    check_eq(r.px(5, 1), pal(r, 2), "row 1 (field 1, line 1)");
    check_eq(r.px(5, 2), pal(r, 3), "row 2 (line 2)");

    // Mode 5 + interlace: field f of line v shows BG row 2v + f.
    Rig h;
    pattern_scene(h, rows, 0x05);
    h.reg(0x212D, 0x01);
    h.reg(0x2133, 0x01);
    h.render();
    h.finish_frame();
    h.render();
    check_eq(h.px(4, 0), pal(h, 3), "row 0 = BG row 2");
    check_eq(h.px(4, 1), pal(h, 4), "row 1 = BG row 3");
    check_eq(h.px(4, 2), pal(h, 5), "row 2 = BG row 4");

    // Overscan: 239 lines (478 interlaced), V-blank at 240.
    Rig o;
    bg1_scene(o);
    o.reg(0x2133, 0x04);
    o.reg(0x2100, 0x0F);
    o.run_to(240, 0);
    check_eq(static_cast<unsigned>(o.ppu.frame_height()), 239, "overscan frame is 239 rows");
    check_eq(o.px(10, 238), rgb(kRed), "line 239 drawn");
    check(o.ppu.in_vblank(), "V-blank at line 240");
}

auto test_obj_interlace() -> void {
    // SETINI bit 1: an 8x8 sprite covers 4 lines, showing tile rows 2n + field.
    Rig r;
    sprite_rig(r);
    for (int c = 1; c < 9; c++) r.color(128 + c, static_cast<Word>(c * 0x0421));
    r.tile4_fn(0x0010, [](int, int y) { return y + 1; });
    r.sprite(0, 40, 10, 1, 0x00);
    r.reg(0x2133, 0x03);
    r.render();                                   // field 0: even rows
    r.finish_frame();
    r.render();                                   // field 1: odd rows
    auto obj = [](int c) { return rgb(static_cast<Word>(c * 0x0421)); };
    check_eq(r.px(42, 20), obj(1), "field 0, first line: tile row 0");
    check_eq(r.px(42, 21), obj(2), "field 1, first line: tile row 1");
    check_eq(r.px(42, 22), obj(3), "field 0, second line: tile row 2");
    check_eq(r.px(42, 27), obj(8), "field 1, fourth line: tile row 7");
    check_eq(r.px(42, 28), rgb(kGray), "4 lines tall");
}

auto test_tall_sprite_vflip() -> void {
    // 16x32 sprites flip each 16x16 half on its own (OBSEL size 6).
    Rig r;
    sprite_rig(r);
    r.reg(0x2101, 0xC0);
    for (int c = 1; c < 5; c++) r.color(128 + c, static_cast<Word>(c * 0x0421));
    // Character rows: tiles 0x01/0x11 (top half) color 1/2, 0x21/0x31 (bottom half) color 3/4.
    const int colors[] = {1, 2, 3, 4};
    for (int t = 0; t < 4; t++) {
        const int c = colors[t];
        r.tile4_fn(static_cast<Word>((0x01 + 0x10 * t) * 16), [c](int, int) { return c; });
        r.tile4_fn(static_cast<Word>((0x02 + 0x10 * t) * 16), [c](int, int) { return c; });
    }
    r.sprite(0, 40, 20, 1, 0x80, false);  // small = 16x32, V flip
    r.render();
    auto obj = [](int c) { return rgb(static_cast<Word>(c * 0x0421)); };
    check_eq(r.px(42, 20), obj(2), "top half flipped in place: tile row 1 first");
    check_eq(r.px(42, 28), obj(1), "then tile row 0");
    check_eq(r.px(42, 36), obj(4), "bottom half: tile row 3");
    check_eq(r.px(42, 44), obj(3), "then tile row 2");
}

auto test_offset_per_tile() -> void {
    // Mode 2: BG1 map entry (column c, row r) is tile 1 + c % 4 + 4 * (r % 2), solid color =
    // tile number. BG3's map (word $1400) holds the per-column offsets: entry (k - 1, 0) is the
    // H value for column k, entry (k - 1, 1) the V value; bit 13 = applies to BG1.
    auto scene = [](Rig& r, Byte mode, bool eight_bpp) {
        r.hide_sprites();
        r.color(0, kGray);
        for (int c = 1; c < 9; c++) r.color(c, static_cast<Word>(c * 0x0421));
        r.reg(0x2105, mode);
        r.reg(0x2107, 0x7C);
        r.reg(0x2109, 0x14);
        for (int t = 1; t < 9; t++) {
            if (eight_bpp) r.solid8(static_cast<Word>(t * 32), t);
            else r.solid4(static_cast<Word>(t * 16), t);
        }
        for (int row = 0; row < 32; row++) {
            for (int col = 0; col < 32; col++) {
                r.vram_word(static_cast<Word>(0x7C00 + row * 32 + col), static_cast<Word>(1 + col % 4 + 4 * (row % 2)));
            }
        }
        r.fill_map(0x1400, 0);
        r.reg(0x212C, 0x01);
    };
    auto c = [](int n) { return rgb(static_cast<Word>(n * 0x0421)); };
    Rig r;
    scene(r, 0x02, false);
    r.vram_word(0x1400 + 0, 0x2000 | 16);        // column 1: H = 16 -> map column 3
    r.vram_word(0x1400 + 32 + 2, 0x2000 | 8);    // column 3: V = 8 -> map row 1
    r.vram_word(0x1400 + 3, 0x4000 | 16);        // column 4: BG2 only, BG1 untouched
    r.render();
    check_eq(r.px(2, 0), c(1), "column 0 is never affected");
    check_eq(r.px(10, 0), c(4), "column 1 takes BG3's H offset");
    check_eq(r.px(18, 0), c(3), "column 2 without an entry");
    check_eq(r.px(26, 0), c(8), "column 3 takes BG3's V offset");
    check_eq(r.px(34, 0), c(1), "BG2-only entry leaves BG1 alone");

    // Fine H scroll stays; columns follow it.
    Rig f;
    scene(f, 0x02, false);
    f.vram_word(0x1400 + 0, 0x2000 | 16);
    f.reg(0x210D, 3); f.reg(0x210D, 0);
    f.render();
    check_eq(f.px(4, 0), c(1), "column 0 now covers x 0-4");
    check_eq(f.px(5, 0), c(4), "column 1 starts at x 5");

    // Mode 4: one entry per column, bit 15 = vertical.
    Rig m;
    scene(m, 0x04, true);
    m.vram_word(0x1400 + 0, 0x8000 | 0x2000 | 8);  // column 1: V = 8
    m.vram_word(0x1400 + 1, 0x2000 | 16);          // column 2: H = 16
    m.render();
    check_eq(m.px(10, 0), c(6), "mode 4 column 1: V offset");
    check_eq(m.px(18, 0), c(1), "mode 4 column 2: H offset (map column 4)");
}

auto test_direct_color() -> void {
    // CGWSEL bit 0 in mode 3: BG1's 8-bit pixel BBGGGRRR + palette bits bgr is the color.
    auto scene = [](Rig& r, Byte cgwsel, Word entry) {
        r.hide_sprites();
        r.color(0, kGray);
        r.color(0xC7, kBlue);
        r.reg(0x2105, 0x03);
        r.reg(0x2107, 0x7C);
        r.solid8(0x0020, 0xC7);  // tile 1: pixel BB=11, GGG=000, RRR=111
        r.fill_map(0x7C00, entry);
        r.reg(0x2130, cgwsel);
        r.reg(0x212C, 0x01);
        r.render();
    };
    Rig r;
    scene(r, 0x01, 0x0001);
    check_eq(r.px(5, 5), rgb(bitforge::ppu::tables::direct_color(0xC7, 0)), "direct color");
    check_eq(bitforge::ppu::tables::direct_color(0xC7, 0), 0x601C, "BBGGGRRR -> 0BB000000RRR00");
    check_eq(bitforge::ppu::tables::direct_color(0xFF, 7), 0x73DE, "all bits: each channel's low bit stays 0");
    Rig g;
    scene(g, 0x01, 0x1C01);  // palette bits 111 add the low bit of each channel
    check_eq(g.px(5, 5), rgb(0x705E), "palette bits");
    Rig off;
    scene(off, 0x00, 0x0001);
    check_eq(off.px(5, 5), rgb(kBlue), "bit clear: CGRAM");
}

} // namespace

int main() {
    bitforge::utils::Logger::set_level(bitforge::utils::LogLevel::Error);
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"OAM low/high tables", test_oam_tables},
        {"OAM reload at V-blank", test_oam_reload_at_vblank},
        {"OBSEL size pairs", test_obj_sizes},
        {"Sprite position, X/Y wrap", test_sprite_position_and_wrap},
        {"Sprite flips, 16x16", test_sprite_flip_and_large},
        {"Sprite palettes, name gap", test_sprite_palettes_and_name_gap},
        {"Sprite order, priority rotation", test_sprite_order_and_rotation},
        {"32 sprites per line (range over)", test_sprite_range_limit},
        {"34 slivers per line (time over)", test_sprite_time_limit},
        {"Mode 1 priorities + BG3 bit", test_mode1_priorities},
        {"Mode 0 priorities", test_mode0_priorities},
        {"VMAIN increment 1/32/128 + low/high", test_vmain_increment},
        {"VMAIN address remapping", test_vmain_remap},
        {"VRAM read latch", test_vram_read_latch},
        {"Tilemap column DMA (VMAIN $81)", test_tilemap_column_dma},
        {"BG screens 32x32/64x32/32x64/64x64", test_bg_screen_layouts},
        {"BG 16x16 tiles", test_bg_16x16_tiles},
        {"Windows W1/W2 + logic", test_windows},
        {"Color math", test_color_math},
        {"Mid-line register write", test_mid_line_write},
        {"H-IRQ + TIMEUP", test_h_irq},
        {"V-IRQ / HV-IRQ", test_v_and_hv_irq},
        {"HVBJOY + RDNMI", test_hvbjoy_and_nmi_flag},
        {"Auto-joypad + $4016", test_auto_joypad},
        {"Master clocks, FastROM", test_master_clocks},
        {"CPU IRQ + WAI", test_cpu_takes_irq},
        {"$4210/$4211 poll with NMI/IRQ handler", test_flag_poll_with_handler},
        {"$4210 poll, every handler phase (FF3 slot 4)", test_flag_poll_every_phase},
        {"DMA modes 0-7, B->A, time", test_dma_modes},
        {"HDMA direct + repeat", test_hdma_direct},
        {"HDMA indirect", test_hdma_indirect},
        {"HDMA time", test_hdma_time},
        {"Brightness curve", test_brightness_curve},
        {"Mosaic horizontal", test_mosaic_horizontal},
        {"Mosaic vertical counter", test_mosaic_vertical},
        {"Mode 5 hi-res (+ scroll, mosaic)", test_hires_mode5},
        {"Pseudo hi-res + frame width", test_pseudo_hires},
        {"Hi-res from mid-frame", test_hires_mid_frame},
        {"Interlace, fields, overscan", test_interlace},
        {"OBJ interlace", test_obj_interlace},
        {"Tall sprite V flip", test_tall_sprite_vflip},
        {"Offset-per-tile (modes 2/4)", test_offset_per_tile},
        {"Direct color", test_direct_color},
    };
    printf("=== SNES PPU / timing / DMA tests ===\n\n");
    int passed = 0, failed = 0;
    for (const auto& t : tests) {
        printf("  %s... ", t.name);
        failures_in_test = 0;
        t.fn();
        if (failures_in_test == 0) { printf("PASS\n"); passed++; } else { failed++; }
    }
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
