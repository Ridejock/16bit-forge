// SPDX-License-Identifier: MIT
// Scanline renderer: BG layers, sprites, windows, priority compositing of the main and sub
// screens, color math and master brightness. The priority orders and size tables live in
// renderer.hpp.
#include "snes/ppu/ppu.hpp"
#include <algorithm>

namespace bitforge::ppu {

using namespace tables;

namespace {

constexpr uint16_t kChannelMask = 0x1F;

// Per-channel add or subtract of two BGR555 colors, optionally halved, clamped to 0-31.
auto blend(uint16_t lhs, uint16_t rhs, bool subtract, bool halve) -> uint16_t {
    uint16_t out = 0;
    for (int shift = 0; shift < 15; shift += 5) {
        const int a = (lhs >> shift) & kChannelMask;
        const int b = (rhs >> shift) & kChannelMask;
        int c = subtract ? std::max(0, a - b) : a + b;
        if (halve) c >>= 1;
        out = static_cast<uint16_t>(out | (std::min(c, 31) << shift));
    }
    return out;
}

auto to_xrgb(uint16_t color, uint32_t brightness) -> uint32_t {
    const auto& level = kBrightness[brightness & 0x0F];
    const uint32_t r = level[color & kChannelMask];
    const uint32_t g = level[(color >> 5) & kChannelMask];
    const uint32_t b = level[(color >> 10) & kChannelMask];
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

constexpr uint32_t kForceBlank = 0xFF000000;

} // namespace

// The frame's picture size is fixed when its first visible line starts: SETINI interlace and
// overscan are latched there (games set them in V-blank). The width starts at whatever the
// last frame needed and widens if a hi-res line turns up.
auto PPU::begin_frame() -> void {
    frame_interlace_ = (setini & 0x01) != 0;
    frame_overscan_ = (setini & 0x04) != 0;
    last_frame_hires_ = frame_had_hires_;
    frame_had_hires_ = false;
    const int lines = frame_overscan_ ? kOverscanHeight : HEIGHT;
    set_frame_layout(last_frame_hires_ ? kMaxWidth : WIDTH, frame_interlace_ ? lines * 2 : lines);
}

// Changes the picture size, rescaling what is already there (nearest) so the other interlace
// field and the lines already drawn this frame stay in place.
auto PPU::set_frame_layout(int width, int height) -> void {
    if (width == frame_w_ && height == frame_h_) return;
    const std::vector<uint32_t> old(framebuffer.begin(), framebuffer.begin() + frame_w_ * frame_h_);
    for (int y = 0; y < height; y++) {
        const int sy = y * frame_h_ / height;
        for (int x = 0; x < width; x++) {
            framebuffer[static_cast<size_t>(y * width + x)] = old[static_cast<size_t>(sy * frame_w_ + x * frame_w_ / width)];
        }
    }
    frame_w_ = width;
    frame_h_ = height;
}

auto PPU::output_row(int v) const -> uint32_t* {
    const int line = v - 1;
    const int row = frame_interlace_ ? line * 2 + (field_ ? 1 : 0) : line;
    if (line < 0 || row >= frame_h_) return nullptr;
    return const_cast<uint32_t*>(framebuffer.data()) + static_cast<size_t>(row) * static_cast<size_t>(frame_w_);
}

// Sprites for the line are evaluated once, when it starts (after the previous H-blank's HDMA).
auto PPU::begin_line(int v) -> void {
    if (v == 1) begin_frame();
    line_x = 0;
    carry = MathCarry{cgram15(0), false, true, false, false};  // the first hi-res pixel shows the sub screen as is

    if (v == 1 || mosaic_restart || ++mosaic_vindex >= mosaic_vblock) {
        mosaic_vindex = 0;
        mosaic_vblock = mosaic_size();
        mosaic_restart = false;
    }
    mosaic_hcounter = mosaic_size();
    mosaic_hstart = 0;

    if (inidisp & 0x80) {
        layers[OBJ].fill(LinePixel{});  // force blank skips sprite evaluation
    } else {
        render_objects(v - 1, layers[OBJ]);  // runs (and sets STAT77) even with OBJ off
    }
}

// Draws pixels [line_x, x_end) of line v with the registers as they are now. A register write
// in the middle of a line draws up to the beam first, so raster effects split where they should.
auto PPU::flush_line(int x_end) -> void {
    x_end = std::min(x_end, WIDTH);
    if (line_x >= x_end) return;
    render_segment(current_scanline, line_x, x_end);
    line_x = x_end;
}

// Horizontal mosaic: each dot maps to the first dot of its block. Blocks restart at dot 0 and a
// new size applies from the next block.
auto PPU::update_mosaic(int x0, int x1) -> void {
    for (int x = x0; x < x1; x++) {
        if (x == 0 || --mosaic_hcounter == 0) {
            mosaic_hcounter = mosaic_size();
            mosaic_hstart = x;
        }
        mosaic_src[static_cast<size_t>(x)] = static_cast<uint8_t>(mosaic_hstart);
    }
}

auto PPU::render_segment(int v, int x0, int x1) -> void {
    const int mode = bgmode & 0x07;
    const bool hires_bg = is_hires_mode(mode);
    const bool hires = hires_bg || (setini & 0x08) != 0;  // two output pixels per dot
    if (hires && !(inidisp & 0x80)) {
        frame_had_hires_ = true;
        if (frame_w_ != kMaxWidth) set_frame_layout(kMaxWidth, frame_h_);
    }
    uint32_t* line = output_row(v);
    if (!line) return;
    const int scale = frame_w_ / WIDTH;  // output pixels per dot

    if (inidisp & 0x80) {  // force blank shows black
        std::fill(line + x0 * scale, line + x1 * scale, kForceBlank);
        return;
    }

    update_mosaic(x0, x1);
    const auto& bpp = kModeBpp[static_cast<size_t>(mode)];
    const uint8_t shown = tm | ts;
    if (mode == 7) {
        render_mode7(v, layers[BG1], layers[BG2], x0, x1);
        for (size_t bg = BG3; bg <= BG4; bg++) std::fill(layers[bg].begin() + x0, layers[bg].begin() + x1, LinePixel{});
    }
    const int bg_scale = hires_bg ? 2 : 1;  // BG line buffer entries per dot
    for (int bg = 0; bg < 4 && mode != 7; bg++) {
        auto& buffer = layers[static_cast<size_t>(bg)];
        if (bpp[static_cast<size_t>(bg)] != 0 && (shown & (1 << bg))) {
            render_background(bg, v, bpp[static_cast<size_t>(bg)], buffer, x0, x1);
        } else {
            std::fill(buffer.begin() + x0 * bg_scale, buffer.begin() + x1 * bg_scale, LinePixel{});
        }
    }
    build_windows(x0, x1);

    const bool sub_operand = (cgwsel & 0x02) != 0;
    const bool subtract = (cgadsub & 0x80) != 0;
    const bool half = (cgadsub & 0x40) != 0;
    const uint32_t brightness = inidisp & 0x0F;

    for (int x = x0; x < x1; x++) {
        const int main_x = hires_bg ? x * 2 + 1 : x;  // BG entries for the main and sub screens
        const int sub_x = hires_bg ? x * 2 : x;

        int sub_layer = BACKDROP;
        const uint16_t sub = (sub_operand || hires) ? compose_pixel(x, sub_x, false, ts, tsw, sub_layer) : fixed_color;
        const bool sub_transparent = sub_layer == BACKDROP;

        // Hi-res: the left pixel of the dot is the sub screen (its backdrop is CGRAM color 0),
        // under the previous main pixel's color math.
        if (hires) {
            const uint16_t shown_sub = sub_transparent ? cgram15(0) : sub;
            uint16_t left = carry.unclipped ? shown_sub : 0;
            if (carry.math) left = blend(left, carry.add_sub ? carry.main_color : fixed_color, subtract, carry.halve);
            line[x * 2] = to_xrgb(left, brightness);
        }

        int main_layer = BACKDROP;
        const uint16_t main = compose_pixel(x, main_x, true, tm, tmw, main_layer);
        const bool inside = window_masks[COLOR_WINDOW][static_cast<size_t>(x)];
        const bool clip = color_window_region(cgwsel >> 6, inside);
        const bool prevent = color_window_region(cgwsel >> 4, inside);

        bool math = (cgadsub & (1 << main_layer)) != 0 && !prevent;
        if (main_layer == OBJ) math = math && layers[OBJ][static_cast<size_t>(x)].math;
        // A transparent sub screen gives way to the fixed color, without halving.
        const bool add_sub = sub_operand && !sub_transparent;
        carry = MathCarry{main, math, !clip, add_sub, half && !clip && !(sub_operand && sub_transparent)};

        uint16_t color = clip ? 0 : main;
        if (math) color = blend(color, add_sub ? sub : fixed_color, subtract, carry.halve);
        const uint32_t out = to_xrgb(color, brightness);
        if (hires) {
            line[x * 2 + 1] = out;
        } else if (scale == 2) {
            line[x * 2] = line[x * 2 + 1] = out;
        } else {
            line[x] = out;
        }
    }
}

// Front-most opaque pixel among the enabled, unmasked layers, walking the mode's priority order.
// `bg_x` indexes the BG line buffers (it differs from the dot `x` in hi-res modes).
auto PPU::compose_pixel(int x, int bg_x, bool main, uint8_t enabled, uint8_t windowed, int& layer) const -> uint16_t {
    const auto& order = priority_order(bgmode & 0x07, (bgmode & 0x08) != 0);
    for (int i = 0; i < order.count; i++) {
        const Slot slot = order.slots[static_cast<size_t>(i)];
        const int bit = 1 << slot.layer;
        if (!(enabled & bit)) continue;
        if ((windowed & bit) && window_masks[slot.layer][static_cast<size_t>(x)]) continue;
        const LinePixel& px = layers[slot.layer][static_cast<size_t>(slot.layer == OBJ ? x : bg_x)];
        if (px.opaque && px.priority == slot.priority) {
            layer = slot.layer;
            return px.color;
        }
    }
    layer = BACKDROP;
    return main ? cgram15(0) : fixed_color;  // the sub screen's backdrop is the fixed color
}

auto PPU::build_windows(int x0, int x1) -> void {
    // Each window-able layer has a 4-bit field: W1 invert, W1 enable, W2 invert, W2 enable.
    const std::array<Byte, kWindowCount> select = {
        static_cast<Byte>(w12sel & 0x0F), static_cast<Byte>(w12sel >> 4),
        static_cast<Byte>(w34sel & 0x0F), static_cast<Byte>(w34sel >> 4),
        static_cast<Byte>(wobjsel & 0x0F), static_cast<Byte>(wobjsel >> 4),
    };
    const std::array<int, kWindowCount> logic = {
        wbglog & 3, (wbglog >> 2) & 3, (wbglog >> 4) & 3, (wbglog >> 6) & 3, wobjlog & 3, (wobjlog >> 2) & 3,
    };

    for (size_t w = 0; w < kWindowCount; w++) {
        const Byte sel = select[w];
        const bool en1 = (sel & 0x02) != 0;
        const bool en2 = (sel & 0x08) != 0;
        auto& mask = window_masks[w];
        for (int x = x0; x < x1; x++) {
            const bool in1 = (x >= wh[0] && x <= wh[1]) != ((sel & 0x01) != 0);
            const bool in2 = (x >= wh[2] && x <= wh[3]) != ((sel & 0x04) != 0);
            mask[static_cast<size_t>(x)] = en1 && en2 ? window_logic(logic[w], in1, in2)
                                         : en1        ? in1
                                         : en2        ? in2
                                                      : false;
        }
    }
}

// Offset-per-tile (modes 2, 4, 6): the scroll values for tile column `column` (1 and up; the
// first column is never affected) come from BG3's tilemap, the row at BG3's scroll for H and the
// row below it for V. Mode 4 has only the first, with bit 15 choosing H or V.
auto PPU::offset_per_tile(int column, uint16_t& h, uint16_t& v) const -> void {
    const Byte sc_reg = bgsc[BG3];
    const uint32_t tilemap_base = static_cast<uint32_t>(((sc_reg & 0xFC) << 8) & 0x7FFF) * 2u;
    const int tile_shift = (bgmode & 0x40) ? 4 : 3;  // BG3 16x16 tiles
    const int htile_shift = is_hires_mode(bgmode & 0x07) ? 3 : tile_shift;
    auto entry = [&](int y) -> uint16_t {
        const int hoffset = (column - 1) * 8 + (bg_hofs[BG3] & ~7);
        const int voffset = y + bg_vofs[BG3];
        const int htile = hoffset >> htile_shift;
        const int vtile = voffset >> tile_shift;
        uint32_t index = static_cast<uint32_t>((htile & 31) | ((vtile & 31) << 5));
        if ((htile & 32) && (sc_reg & 0x01)) index += 0x400;
        if ((vtile & 32) && (sc_reg & 0x02)) index += (sc_reg & 0x01) ? 0x800 : 0x400;
        const uint32_t addr = tilemap_base + index * 2u;
        return static_cast<uint16_t>(read_vram(static_cast<uint16_t>(addr)) | (read_vram(static_cast<uint16_t>(addr + 1)) << 8));
    };
    h = entry(0);
    v = (bgmode & 0x07) == 4 ? 0 : entry(8);
}

// One BG for dots [x0, x1). In modes 5/6 every dot is two pixels (16-pixel-wide tiles, the H
// scroll counts in hi-res pixels) and `out` holds 2 entries per dot; with interlace those modes
// also draw 448 distinct lines, one field at a time.
auto PPU::render_background(int bg, int v, int bpp, LineBuffer& out, int x0, int x1) const -> void {
  const Byte sc_reg = bgsc[static_cast<size_t>(bg)];
  const int mode = bgmode & 0x07;
  const bool hires = is_hires_mode(mode);
  const bool interlace = hires && frame_interlace_;
  const bool mosaic_enabled = mosaic_on(bg);

  // BGxSC and BGxxNBA are word addresses. The port stores each word at byte word*2.
  const uint32_t tilemap_base = static_cast<uint32_t>(((sc_reg & 0xFC) << 8) & 0x7FFF) * 2u;
  const bool map_wide = (sc_reg & 0x01) != 0;   // 64 tiles across
  const bool map_tall = (sc_reg & 0x02) != 0;   // 64 tiles down
  const Byte name_base = (bg <= 1) ? bg12nba : bg34nba;
  const int nibble_shift = (bg & 1) ? 4 : 0;
  const uint32_t tile_base = static_cast<uint32_t>((((name_base >> nibble_shift) & 0x0F) << 12) & 0x7FFF) * 2u;

  const bool big_tiles = (bgmode & (0x10 << bg)) != 0;  // BGMODE bits 4-7: 16x16 tiles
  const int vtile_shift = big_tiles ? 4 : 3;
  const int htile_shift = hires ? 4 : vtile_shift;      // hi-res tiles are always 16 wide
  const int tile_bytes = 8 * bpp;
  const int colors = 1 << bpp;
  const int palette_base = mode == 0 ? bg * kMode0PaletteStride : 0;
  const bool direct = bpp == 8 && bg == BG1 && (cgwsel & 0x01) && has_direct_color(mode);

  const int unit = hires ? 2 : 1;                       // pixels per dot
  const int hscroll = bg_hofs[static_cast<size_t>(bg)] * unit;
  const int hmask = ((WIDTH * unit) << (big_tiles ? 1 : 0) << (map_wide ? 1 : 0)) - 1;
  int vpixel = v;
  if (interlace) vpixel = v * 2 + ((field_ && !mosaic_enabled) ? 1 : 0);
  if (mosaic_enabled) vpixel -= mosaic_vindex * (interlace ? 2 : 1);

  // Offset-per-tile: columns are 8 dots wide, aligned to this BG's fine scroll.
  const bool opt = has_offset_per_tile(mode) && bg <= BG2;
  const int column_width = 8 * unit;
  const int fine = hscroll & (column_width - 1);
  const uint16_t valid_bit = static_cast<uint16_t>(0x2000 << bg);
  int opt_column = -1;
  uint16_t opt_h = 0, opt_v = 0;

  for (int px_i = x0 * unit; px_i < x1 * unit; px_i++) {
      LinePixel& px = out[static_cast<size_t>(px_i)];
      if (mosaic_enabled) {
          const int src = mosaic_src[static_cast<size_t>(px_i / unit)] * unit;  // left pixel of the block
          if (src != px_i) {
              px = out[static_cast<size_t>(src)];
              continue;
          }
      }
      px.opaque = false;

      int hoffset = px_i + hscroll;
      int voffset = vpixel + bg_vofs[static_cast<size_t>(bg)];
      if (opt) {
          const int column = (px_i + fine) / column_width;
          if (column > 0) {
              if (column != opt_column) {
                  offset_per_tile(column, opt_h, opt_v);
                  opt_column = column;
              }
              const int in_column = (px_i + fine) % column_width;
              if (mode == 4) {
                  if (opt_h & valid_bit) {
                      if (opt_h & 0x8000) voffset = vpixel + opt_h;
                      else hoffset = column * column_width + (opt_h & ~7) + in_column;
                  }
              } else {
                  if (opt_h & valid_bit) hoffset = column * column_width + (opt_h & ~7) + in_column;
                  if (opt_v & valid_bit) voffset = vpixel + opt_v;
              }
          }
      }
      const int map_x = hoffset & hmask;
      const int map_y = voffset & 0x3FF;
      const int tile_col = map_x >> htile_shift;
      const int tile_row = map_y >> vtile_shift;

      // 32x32 screens; the second screen across follows the first, the second down follows those.
      uint32_t entry_index = static_cast<uint32_t>((tile_row & 31) * 32 + (tile_col & 31));
      if (map_wide && (tile_col & 32)) entry_index += 0x400;
      if (map_tall && (tile_row & 32)) entry_index += map_wide ? 0x800 : 0x400;
      const uint32_t entry_addr = tilemap_base + entry_index * 2u;
      const uint16_t tile_entry = static_cast<uint16_t>(
          read_vram(static_cast<uint16_t>(entry_addr)) |
          (read_vram(static_cast<uint16_t>(entry_addr + 1)) << 8));

      const int palette = (tile_entry >> 10) & 0x07;
      const bool h_flip = (tile_entry >> 14) & 1;
      const bool v_flip = (tile_entry >> 15) & 1;
      int tile_index = tile_entry & 0x03FF;
      // A 16-wide or 16-tall tile is 8x8 characters n, n+1 (right), n+16 (below), n+17.
      const int sub_x = htile_shift == 4 ? ((map_x >> 3) & 1) ^ (h_flip ? 1 : 0) : 0;
      const int sub_y = vtile_shift == 4 ? ((map_y >> 3) & 1) ^ (v_flip ? 1 : 0) : 0;
      tile_index = (tile_index + sub_x + sub_y * 16) & 0x3FF;  // the character number wraps at 10 bits

      const int y_in_tile = v_flip ? (7 - (map_y & 7)) : (map_y & 7);
      const int bit = h_flip ? (map_x & 7) : 7 - (map_x & 7);
      const uint32_t row_addr = tile_base + static_cast<uint32_t>(tile_index * tile_bytes + y_in_tile * 2);

      // Bitplanes come in pairs: planes 2n and 2n+1 live 16n bytes into the tile.
      int color_index = 0;
      for (int plane = 0; plane < bpp; plane++) {
          const uint32_t at = row_addr + static_cast<uint32_t>((plane / 2) * 16 + (plane & 1));
          color_index |= ((read_vram(static_cast<uint16_t>(at)) >> bit) & 1) << plane;
      }
      if (color_index == 0) continue;

      if (direct) {
          px.color = direct_color(color_index, palette);
      } else {
          px.color = cgram15(bpp == 8 ? color_index : palette_base + palette * colors + color_index);
      }
      px.priority = static_cast<uint8_t>((tile_entry >> 13) & 1);
      px.opaque = true;
  }
}

// Mode 7: one 1024x1024 8bpp plane (128x128 tile map in the even VRAM bytes, 256 tiles of 8x8
// in the odd bytes) through the A-D matrix around the center X/Y. EXTBG (SETINI bit 6) shows the
// same pixels on BG2 with bit 7 as their priority. Mosaic: each layer uses its own H flag; both
// use BG1's V flag.
auto PPU::render_mode7(int v, LineBuffer& bg1, LineBuffer& bg2, int x0, int x1) const -> void {
    const auto clip = [](int n) { return (n & 0x2000) ? (n | ~0x3FF) : (n & 0x3FF); };
    const int a = m7matrix[0], b = m7matrix[1], c = m7matrix[2], d = m7matrix[3];
    const int line = mosaic_on(BG1) ? v - mosaic_vindex : v;
    const int y = (m7sel & 0x02) ? 255 - (line & 0xFF) : (line & 0xFF);
    const int ox = clip(m7hofs - m7x);
    const int oy = clip(m7vofs - m7y);
    const int start_x = ((a * ox) & ~63) + ((b * oy) & ~63) + ((b * y) & ~63) + (m7x * 256);
    const int start_y = ((c * ox) & ~63) + ((d * oy) & ~63) + ((d * y) & ~63) + (m7y * 256);
    const int repeat = m7sel >> 6;  // 0/1 wrap, 2 transparent outside, 3 tile 0 outside
    const bool extbg = (setini & 0x40) != 0;
    const bool direct = (cgwsel & 0x01) != 0;

    // The 8-bit pixel at a dot (0 = transparent).
    const auto sample = [&](int screen_x) -> int {
        const int x = (m7sel & 0x01) ? 255 - screen_x : screen_x;
        const int px = (start_x + a * x) >> 8;
        const int py = (start_y + c * x) >> 8;
        const bool outside = ((px | py) & ~0x3FF) != 0;
        if (outside && repeat == 2) return 0;
        int tile = 0;
        if (!(outside && repeat == 3)) {
            tile = read_vram(static_cast<uint16_t>((((py >> 3) & 127) * 128 + ((px >> 3) & 127)) * 2));
        }
        return read_vram(static_cast<uint16_t>((tile * 64 + (py & 7) * 8 + (px & 7)) * 2 + 1));
    };

    for (int screen_x = x0; screen_x < x1; screen_x++) {
        const auto i = static_cast<size_t>(screen_x);
        const int src = mosaic_src[i];
        const int color1 = sample(mosaic_on(BG1) ? src : screen_x);
        bg1[i] = color1 ? LinePixel{direct ? direct_color(color1, 0) : cgram15(color1), 0, true, true} : LinePixel{};
        bg2[i] = LinePixel{};
        if (!extbg) continue;
        const int color2 = mosaic_on(BG2) == mosaic_on(BG1) ? color1 : sample(mosaic_on(BG2) ? src : screen_x);
        if ((color2 & 0x7F) != 0) bg2[i] = LinePixel{cgram15(color2 & 0x7F), static_cast<uint8_t>(color2 >> 7), true, true};
    }
}

// Sprite evaluation and drawing for one framebuffer row (sprite Y = 0 shows on row 0).
// Range: the first 32 sprites on the line, starting at sprite 0 or the OAMADD sprite when
// priority rotation is on. Time: up to 34 8-pixel slivers, fetched from the last in-range sprite
// backwards, so the dropped slivers belong to the highest-priority sprites.
// OBJ interlace (SETINI bit 1): sprites cover half as many lines and draw every other row of
// their tiles, the field picking even or odd rows.
auto PPU::render_objects(int row, LineBuffer& out) -> void {
    std::fill(out.begin(), out.begin() + WIDTH, LinePixel{});
    const bool obj_interlace = (setini & 0x02) != 0;

    const int first = oam_priority_rotate ? (oam_reload >> 1) & 0x7F : 0;
    std::array<int, kMaxObjsPerLine> in_range{};
    int count = 0;
    for (int i = 0; i < 128; i++) {
        const int index = (first + i) & 0x7F;
        const Sprite s = sprite(index);
        const ObjSize size = sprite_size(s.large);
        if (((row - s.y) & 0xFF) >= (obj_interlace ? size.h / 2 : size.h)) continue;
        const int x9 = s.x & 0x1FF;
        if (x9 > 256 && x9 + size.w - 1 < 512) continue;  // fully off the left edge (x = -256 counts)
        if (count == kMaxObjsPerLine) {
            range_over = true;
            break;
        }
        in_range[static_cast<size_t>(count++)] = index;
    }

    const uint32_t name_base = static_cast<uint32_t>(obsel & 0x07) << 13;                 // words
    const uint32_t name_gap = (static_cast<uint32_t>((obsel >> 3) & 0x03) + 1) << 12;   // words

    int slivers = 0;
    for (int k = count - 1; k >= 0; k--) {
        const Sprite s = sprite(in_range[static_cast<size_t>(k)]);
        const ObjSize size = sprite_size(s.large);
        int sprite_row = (row - s.y) & 0xFF;
        if (obj_interlace) sprite_row *= 2;
        if (s.vflip) {
            // Tall sprites (16x32, 32x64) flip each square half on its own.
            if (size.w == size.h || sprite_row < size.w) sprite_row = size.w - 1 - sprite_row;
            else sprite_row = size.w + (size.w - 1) - (sprite_row - size.w);
        }
        if (obj_interlace) sprite_row = s.vflip ? sprite_row - (field_ ? 1 : 0) : sprite_row + (field_ ? 1 : 0);
        sprite_row &= 0xFF;
        const int columns = size.w / 8;
        const bool math = s.palette >= 4;
        const int palette_base = kObjPaletteBase + s.palette * 16;

        for (int col = 0; col < columns; col++) {
            const int tile_x = s.x + col * 8;
            if (tile_x <= -8 || tile_x >= WIDTH) continue;
            if (slivers == kMaxObjTilesPerLine) {
                time_over = true;
                return;
            }
            slivers++;

            // Columns and rows wrap inside the 16x16-tile name table page.
            const int tile_col = s.hflip ? columns - 1 - col : col;
            const int tile = (s.tile & 0x100) |
                             ((((s.tile >> 4) + (sprite_row >> 3)) & 0x0F) << 4) |
                             (((s.tile & 0x0F) + tile_col) & 0x0F);
            uint32_t word = name_base + static_cast<uint32_t>(tile & 0xFF) * 16 + ((tile & 0x100) ? name_gap : 0);
            word += static_cast<uint32_t>(sprite_row & 7);
            const auto addr = static_cast<uint16_t>((word & 0x7FFF) * 2);
            const Byte p0 = read_vram(addr);
            const Byte p1 = read_vram(static_cast<uint16_t>(addr + 1));
            const Byte p2 = read_vram(static_cast<uint16_t>(addr + 16));
            const Byte p3 = read_vram(static_cast<uint16_t>(addr + 17));

            for (int i = 0; i < 8; i++) {
                const int sx = tile_x + i;
                if (sx < 0 || sx >= WIDTH) continue;
                const int bit = s.hflip ? i : 7 - i;
                const int c = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) |
                              (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
                if (c == 0) continue;
                // Drawn last-to-first, so the lower-numbered sprite ends up on top.
                out[static_cast<size_t>(sx)] = LinePixel{cgram15(palette_base + c),
                                                         static_cast<uint8_t>(s.priority), true, math};
            }
        }
    }
}

} // namespace bitforge::ppu
