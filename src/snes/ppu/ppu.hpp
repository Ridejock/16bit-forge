// SPDX-License-Identifier: MIT
#pragma once
#include "snes/types.hpp"
#include "snes/memory/bus.hpp"
#include "snes/serialize.hpp"
#include "snes/ppu/renderer.hpp"
#include <array>
#include <vector>

namespace snes::ppu {

class PPU {
public:
    PPU();
    
    auto reset() -> void;
    auto read_register(Byte addr) -> Byte;
    auto write_register(Byte addr, Byte value) -> void;
    
    auto connect_bus(memory::Bus* bus_ptr) -> void { bus = bus_ptr; }
    
    // Advance the video clock by `master_clocks` (21.477 MHz). Lines are 1364 clocks, frames
    // 262 lines. A visible line is drawn as the beam passes: up to the beam on each mid-line
    // register write and the rest at H-blank, after which HDMA runs.
    // Returns true when line 261 wraps to 0.
    auto step(uint32_t master_clocks) -> bool;
    auto in_vblank() const -> bool { return vblank; }
    auto vcounter() const -> int { return current_scanline; }
    auto hclock() const -> int { return line_clock; }  // master clocks into the line, 0-1363
    // $213C/$213D read these back and STAT78 bit 6 flags them: SLHV, or a light gun via $4201.
    auto latch_counters(int dot, int line) -> void {
        h_latch = static_cast<uint16_t>(dot);
        v_latch = static_cast<uint16_t>(line);
        counters_latched = true;
    }
    auto vblank_line() const -> int { return (setini & 0x04) ? 240 : 225; }
    auto frame_lines() const -> int { return lines_this_frame(); }  // 262, or 263 on an interlaced odd frame
    // Master clocks from the current position to the start of `line` (always > 0: a line that has
    // just started is a whole frame away).
    auto clocks_until_line(int line) const -> uint64_t;
    auto field() const -> bool { return field_; }  // interlace field: 1 = odd output rows
    // Master clocks from `ahead` clocks past the beam to the next HDMA table reload (line 0) and
    // to the next HDMA line transfer (H-blank of a visible line). GP-DMA, which the bus runs in
    // one go, uses this to stop HDMA channels' transfers where HDMA would cut in.
    struct HdmaTiming { uint64_t init; uint64_t run; };
    auto hdma_timing(uint64_t ahead) const -> HdmaTiming;

    // The picture, XRGB8888, frame_width() x frame_height(), rows packed. Its size is set per
    // frame: 256 or 512 wide (512 once any line of the frame is hi-res: modes 5/6 or SETINI
    // pseudo hi-res; 256-wide lines are then doubled), and 224 or 239 lines (SETINI overscan),
    // doubled to 448/478 with SETINI interlace, where each field fills every other row (weave).
    auto get_framebuffer() const -> const uint32_t*;
    auto frame_width() const -> int { return frame_w_; }
    auto frame_height() const -> int { return frame_h_; }

    void dump_vram();

    // Registers, latches, memories and the picture. Per-line scratch (layer buffers, window
    // masks) is rebuilt before use, so states are taken between frames.
    auto serialize(Archive& ar) -> void;
    
    // Sprite attributes decoded from OAM; public for tests and debug views.
    struct Sprite {
        int x;          // -256..255
        int y;          // 0..255
        int tile;       // 9-bit name
        int palette;    // 0-7
        int priority;   // 0-3
        bool hflip, vflip, large;
    };
    auto sprite(int index) const -> Sprite;
    auto sprite_size(bool large) const -> tables::ObjSize;
    auto oam_byte(int index) const -> Byte { return oam[static_cast<size_t>(index)]; }

    static constexpr int WIDTH = 256;              // dots per line
    static constexpr int HEIGHT = 224;             // visible lines without overscan
    static constexpr int kOverscanHeight = 239;    // SETINI bit 2
    static constexpr int kMaxWidth = WIDTH * 2;    // hi-res
    static constexpr int kMaxHeight = kOverscanHeight * 2;  // interlace + overscan
    static constexpr int kClocksPerLine = 1364;
    static constexpr int kLinesPerFrame = 262;     // +1 on interlaced even fields
    static constexpr int kHblankClock = 274 * 4;     // dot 274: H-blank starts, line is drawn, HDMA
    static constexpr int kHdmaInitClock = 6 * 4;     // dot 6 of line 0: HDMA tables reload
    static constexpr int kFirstPixelDot = 22;        // dot that outputs pixel 0

private:
    memory::Bus* bus = nullptr;
    
    std::vector<uint32_t> framebuffer;  // kMaxWidth * kMaxHeight, the first frame_w_ * frame_h_ used
    int frame_w_ = WIDTH;
    int frame_h_ = HEIGHT;
    bool field_ = false;            // toggles every frame
    bool frame_interlace_ = false;  // SETINI bits 0 and 2 as latched at the first visible line
    bool frame_overscan_ = false;
    bool frame_had_hires_ = false;  // a hi-res line was drawn this frame
    bool last_frame_hires_ = false;
    int line_clock = 0;
    int current_scanline = 0;
    int line_x = WIDTH;        // pixels of the current line drawn so far (WIDTH = line finished)
    bool vblank = false;
    
    // PPU Registers
    Byte inidisp = 0x80;      // $2100: Screen display
    Byte obsel = 0x00;        // $2101: sprite size, name base and name gap
    Byte bgmode = 0x00;       // $2105: BG mode
    Byte mosaic = 0x00;       // $2106: bits 0-3 BG1-4 enable, bits 7-4 size - 1
    std::array<Byte, 4> bgsc{};         // $2107-$210A: BG tilemap address and size
    Byte bg12nba = 0x00;      // $210B: BG1/2 character data
    Byte bg34nba = 0x00;      // $210C: BG3/4 character data
    std::array<uint16_t, 4> bg_hofs{};  // $210D/$210F/$2111/$2113: BG horizontal scroll (10-bit)
    std::array<uint16_t, 4> bg_vofs{};  // $210E/$2110/$2112/$2114: BG vertical scroll (10-bit)
    Byte scroll_latch = 0x00;           // previous byte written to any BGnxOFS register
    Byte hofs_latch = 0x00;             // previous byte written to any BGnHOFS register
    Byte vmain = 0x00;        // $2115: VRAM address increment
    Byte w12sel = 0x00;       // $2123: Window enable/invert for BG1/BG2
    Byte w34sel = 0x00;       // $2124: BG3/BG4
    Byte wobjsel = 0x00;      // $2125: OBJ and the color window
    std::array<Byte, 4> wh{};  // $2126-$2129: W1 left, W1 right, W2 left, W2 right
    Byte wbglog = 0x00;       // $212A: W1/W2 logic for BG1-BG4
    Byte wobjlog = 0x00;      // $212B: W1/W2 logic for OBJ and color
    Byte tm = 0x00;           // $212C: Main screen designation
    Byte ts = 0x00;           // $212D: Sub screen designation
    Byte tmw = 0x00;          // $212E: Window mask for main screen
    Byte tsw = 0x00;          // $212F: Window mask for sub screen
    Byte cgwsel = 0x00;       // $2130: Color math clip/prevent regions, sub screen vs fixed color
    Byte cgadsub = 0x00;      // $2131: Color math add/sub, half, per-layer enable
    uint16_t fixed_color = 0; // $2132: COLDATA as a BGR555 color
    Byte setini = 0x00;       // $2133: Screen mode/video select
    Byte m7sel = 0;           // $211A: mode 7 repeat/fill and flips
    std::array<int16_t, 4> m7matrix{};  // $211B-$211E: A, B, C, D (signed 8.8); A*B>>8 is also the multiplier
    int16_t m7x = 0, m7y = 0;           // $211F/$2120: center (signed 13-bit)
    int16_t m7hofs = 0, m7vofs = 0;     // $210D/$210E second meaning: mode 7 scroll (signed 13-bit)
    Byte m7_latch = 0;
    
    // PPU state
    uint16_t vram_addr = 0;       // $2116/$2117 word address, before translation
    uint16_t vram_prefetch = 0;   // Word returned by $2139/$213A
    uint16_t cgram_addr = 0;     // byte address into CGRAM (0-511); $2121 sets it to word * 2

    // OAM: $2102/$2103 set the reload word address; the internal byte address walks 0-$3FF,
    // with $200-$3FF all landing on the 32-byte high table.
    uint16_t oam_reload = 0;      // 9-bit word address from OAMADDL/H
    uint16_t oam_addr = 0;        // 10-bit byte address
    Byte oam_latch = 0;           // low-table writes go in pairs
    bool oam_priority_rotate = false;  // OAMADDH bit 7
    bool range_over = false;      // STAT77 bit 6
    bool time_over = false;       // STAT77 bit 7

    // $2137 counter latch, read through $213C/$213D
    uint16_t h_latch = 0;
    uint16_t v_latch = 0;
    bool ophct_high = false;
    bool opvct_high = false;
    bool counters_latched = false;
    
    // VRAM (64KB)
    std::array<Byte, 64 * 1024> vram{};
    
    // CGRAM (512 bytes = 256 colors × 2 bytes each)
    std::array<Byte, 512> cgram{};
    
    // OAM: 512-byte low table (4 bytes per sprite) + 32-byte high table (2 bits per sprite)
    std::array<Byte, 544> oam{};
    
    // Open bus for registers
    Byte open_bus = 0x00;
    
    // Mosaic. Vertically, blocks start at the first visible line (or the line after mosaic is
    // switched on from all-off); a new size applies from the next block, the current one
    // finishes with the size it started with. Horizontally, blocks restart at dot 0 every line.
    int mosaic_vindex = 0;          // line within the current vertical block
    int mosaic_vblock = 1;          // that block's size
    bool mosaic_restart = false;    // start a new block on the next line
    int mosaic_hcounter = 0;
    int mosaic_hstart = 0;
    std::array<uint8_t, WIDTH> mosaic_src{};  // dot -> first dot of its mosaic block

    // One layer's pixels for the line being drawn.
    struct LinePixel {
        uint16_t color = 0;   // BGR555
        uint8_t priority = 0;
        bool opaque = false;
        bool math = true;     // OBJ palettes 0-3 never take part in color math
    };
    // BGs are drawn at 512 per line in modes 5/6 (even = sub screen, odd = main screen); OBJ
    // and every other mode use the first 256 entries.
    using LineBuffer = std::array<LinePixel, kMaxWidth>;
    std::array<LineBuffer, tables::kLayerCount> layers{};
    std::array<std::array<bool, WIDTH>, tables::kWindowCount> window_masks{};

    // Hi-res output draws the sub-screen pixel of each dot with the color math state the
    // main-screen pixel of the dot before left behind, so carry it along.
    struct MathCarry {
        uint16_t main_color = 0;  // main-screen color before math
        bool math = false;        // color math on for that pixel
        bool unclipped = false;   // outside the clip-to-black region
        bool add_sub = false;     // the operand was the sub screen
        bool halve = false;
    };
    MathCarry carry{};

    auto write_register_byte(Byte addr, Byte value) -> void;
    auto next_line() -> bool;
    auto lines_this_frame() const -> int { return kLinesPerFrame + ((frame_interlace_ && !field_) ? 1 : 0); }

    // Rendering (renderer.cpp)
    auto begin_frame() -> void;
    auto begin_line(int v) -> void;
    auto set_frame_layout(int width, int height) -> void;
    auto output_row(int v) const -> uint32_t*;  // null if line v is outside the picture
    auto flush_line(int x_end) -> void;
    auto render_segment(int v, int x0, int x1) -> void;
    auto update_mosaic(int x0, int x1) -> void;
    auto mosaic_on(int bg) const -> bool { return (mosaic & (1 << bg)) != 0; }
    auto mosaic_size() const -> int { return (mosaic >> 4) + 1; }
    auto render_background(int bg, int v, int bpp, LineBuffer& out, int x0, int x1) const -> void;
    auto offset_per_tile(int column, uint16_t& h, uint16_t& v) const -> void;
    auto render_objects(int row, LineBuffer& out) -> void;
    auto render_mode7(int v, LineBuffer& bg1, LineBuffer& bg2, int x0, int x1) const -> void;
    auto build_windows(int x0, int x1) -> void;
    auto compose_pixel(int x, int bg_x, bool main, uint8_t enabled, uint8_t windowed, int& layer) const -> uint16_t;
    auto cgram15(int index) const -> uint16_t;
    
    // Color conversion
    auto cgram_color(int index) const -> uint32_t;
    
    // VRAM access. `read_vram`/`write_vram` use a byte address into the 64KB array.
    auto read_vram(uint16_t addr) const -> Byte;
    auto write_vram(uint16_t addr, Byte value) -> void;
    auto vram_accessible() const -> bool;
    auto vram_increment() const -> uint16_t;
    auto translate_vram_addr(uint16_t addr) const -> uint16_t;
    auto prefetch_vram() -> void;
    auto vram_port_write(bool high, Byte value) -> void;
    auto oam_write(Byte value) -> void;
    auto oam_read() -> Byte;
};

} // namespace snes::ppu
