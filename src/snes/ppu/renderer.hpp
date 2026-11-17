// SPDX-License-Identifier: MIT
#pragma once
// Data tables for the PPU scanline renderer: BG depths per mode, sprite sizes, layer priority
// orders and window logic. PPU::render_line (renderer.cpp) only walks these tables.
#include <array>
#include <cstdint>

namespace bitforge::ppu::tables {

// Layer ids used across the renderer. BG1-BG4 are 0-3, matching the TM/TS/TMW/TSW bit numbers.
enum Layer : uint8_t { BG1 = 0, BG2 = 1, BG3 = 2, BG4 = 3, OBJ = 4, BACKDROP = 5, COLOR_WINDOW = 5 };
constexpr int kLayerCount = 5;   // layers that hold pixels (BG1-4, OBJ)
constexpr int kWindowCount = 6;  // window masks: BG1-4, OBJ and the color window

// Bits per pixel of BG1-BG4 in each BG mode; 0 = layer absent. Mode 7 has its own renderer.
constexpr std::array<std::array<uint8_t, 4>, 8> kModeBpp = {{
    {2, 2, 2, 2},  // 0
    {4, 4, 2, 0},  // 1
    {4, 4, 0, 0},  // 2
    {8, 4, 0, 0},  // 3
    {8, 2, 0, 0},  // 4
    {4, 2, 0, 0},  // 5 (hi-res: 512 pixels per line)
    {4, 0, 0, 0},  // 6 (hi-res)
    {0, 0, 0, 0},  // 7 (drawn by render_mode7)
}};

// BG mode traits beyond the depths.
constexpr auto is_hires_mode(int mode) -> bool { return mode == 5 || mode == 6; }
// Offset-per-tile: BG3's tilemap holds per-column scroll values for BG1/BG2 (modes 2, 4, 6).
constexpr auto has_offset_per_tile(int mode) -> bool { return mode == 2 || mode == 4 || mode == 6; }
// Direct color (CGWSEL bit 0) applies to BG1 in the 256-color modes.
constexpr auto has_direct_color(int mode) -> bool { return mode == 3 || mode == 4 || mode == 7; }

// Direct color: an 8-bit pixel BBGGGRRR plus the tile's palette bits bgr make a BGR555 color
// 0BBb00GG Gg0RRRr0.
constexpr auto direct_color(int pixel, int group) -> uint16_t {
    return static_cast<uint16_t>(((pixel << 7) & 0x6000) | ((group << 10) & 0x1000) | ((pixel << 4) & 0x0380) |
                                 ((group << 5) & 0x0040) | ((pixel << 2) & 0x001C) | ((group << 1) & 0x0002));
}

// INIDISP brightness: an 8-bit output level for each 5-bit channel value and brightness 0-15.
// The scale is (brightness + 1) / 16, with brightness 0 at 1/64 rather than black, on the
// channel widened to 16 bits by bit replication and then cut to 8 bits. This is the curve the
// PeterLemon RedSpace9BitHDMA reference screenshot matches.
constexpr auto make_brightness_table() {
    std::array<std::array<uint8_t, 32>, 16> table{};
    for (uint32_t b = 0; b < 16; b++) {
        const uint32_t scale = b ? (b + 1) * 4 : 1;  // in 64ths
        for (uint32_t c = 0; c < 32; c++) {
            const uint32_t wide = (c << 11) | (c << 6) | (c << 1) | (c >> 4);
            table[b][c] = static_cast<uint8_t>(wide * scale / 64 >> 8);
        }
    }
    return table;
}
constexpr auto kBrightness = make_brightness_table();

// Mode 0 gives each BG its own 32-color slice of CGRAM.
constexpr int kMode0PaletteStride = 32;
constexpr int kObjPaletteBase = 128;  // sprites use CGRAM 128-255, eight 16-color palettes

// OBSEL bits 7-5: {small, large} sprite size in pixels as {width, height}.
struct ObjSize { uint8_t w, h; };
constexpr std::array<std::array<ObjSize, 2>, 8> kObjSizes = {{
    {{{8, 8}, {16, 16}}},
    {{{8, 8}, {32, 32}}},
    {{{8, 8}, {64, 64}}},
    {{{16, 16}, {32, 32}}},
    {{{16, 16}, {64, 64}}},
    {{{32, 32}, {64, 64}}},
    {{{16, 32}, {32, 64}}},
    {{{16, 32}, {32, 32}}},
}};
constexpr int kMaxObjsPerLine = 32;   // range-over (STAT77 bit 6) past this
constexpr int kMaxObjTilesPerLine = 34;  // time-over (STAT77 bit 7) past this

// One step of a priority order: a layer and the priority it must have. BGs use the tile's
// priority bit (0/1); OBJ uses its 0-3 priority. Orders run front to back.
struct Slot { uint8_t layer, priority; };
using Order = std::array<Slot, 12>;
struct PriorityOrder { uint8_t count; Order slots; };

constexpr PriorityOrder kOrderMode0{12, {{
    {OBJ, 3}, {BG1, 1}, {BG2, 1}, {OBJ, 2}, {BG1, 0}, {BG2, 0},
    {OBJ, 1}, {BG3, 1}, {BG4, 1}, {OBJ, 0}, {BG3, 0}, {BG4, 0},
}}};
constexpr PriorityOrder kOrderMode1{10, {{
    {OBJ, 3}, {BG1, 1}, {BG2, 1}, {OBJ, 2}, {BG1, 0}, {BG2, 0},
    {OBJ, 1}, {BG3, 1}, {OBJ, 0}, {BG3, 0},
}}};
// Mode 1 with BGMODE bit 3 set: BG3 high-priority tiles jump to the very front.
constexpr PriorityOrder kOrderMode1Bg3Front{10, {{
    {BG3, 1}, {OBJ, 3}, {BG1, 1}, {BG2, 1}, {OBJ, 2}, {BG1, 0},
    {BG2, 0}, {OBJ, 1}, {OBJ, 0}, {BG3, 0},
}}};
// Modes 2-6 (mode 6 simply has no BG2).
constexpr PriorityOrder kOrderMode2to6{8, {{
    {OBJ, 3}, {BG1, 1}, {OBJ, 2}, {BG2, 1}, {OBJ, 1}, {BG1, 0}, {OBJ, 0}, {BG2, 0},
}}};
// Mode 7 (BG2 is the EXTBG layer).
constexpr PriorityOrder kOrderMode7{7, {{
    {OBJ, 3}, {OBJ, 2}, {BG2, 1}, {OBJ, 1}, {BG1, 0}, {OBJ, 0}, {BG2, 0},
}}};

constexpr auto priority_order(int mode, bool bg3_front) -> const PriorityOrder& {
    switch (mode) {
        case 0: return kOrderMode0;
        case 1: return bg3_front ? kOrderMode1Bg3Front : kOrderMode1;
        case 7: return kOrderMode7;
        default: return kOrderMode2to6;
    }
}

// WBGLOG/WOBJLOG: how W1 and W2 combine when both are enabled for a layer.
constexpr auto window_logic(int logic, bool w1, bool w2) -> bool {
    switch (logic & 3) {
        case 0: return w1 || w2;
        case 1: return w1 && w2;
        case 2: return w1 != w2;
        default: return w1 == w2;
    }
}

// CGWSEL clip/prevent regions: never, outside the color window, inside it, always.
constexpr auto color_window_region(int sel, bool inside) -> bool {
    switch (sel & 3) {
        case 0: return false;
        case 1: return !inside;
        case 2: return inside;
        default: return true;
    }
}

} // namespace bitforge::ppu::tables
