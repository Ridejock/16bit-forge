// SPDX-License-Identifier: MIT
#include "snes/ppu/ppu.hpp"
#include <cstdint>
#include "utils/logging.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace bitforge::ppu {

namespace {

constexpr Byte kPpu1Version = 0x01;  // STAT77 bits 3-0
constexpr Byte kPpu2Version = 0x03;  // STAT78 bits 3-0
constexpr uint16_t kOamHighTable = 0x200;
constexpr uint16_t kOamAddrMask = 0x3FF;

// Mode 7 scroll and center registers are signed 13-bit.
constexpr auto sign13(int value) -> int16_t {
    return static_cast<int16_t>((value & 0x1FFF) - ((value & 0x1000) << 1));
}

} // namespace

PPU::PPU() : framebuffer(static_cast<size_t>(kMaxWidth * kMaxHeight), 0xFF000000) {
  reset();
}

auto PPU::reset() -> void {
    std::fill(framebuffer.begin(), framebuffer.end(), 0xFF000000);
    frame_w_ = WIDTH;
    frame_h_ = HEIGHT;
    field_ = frame_interlace_ = frame_overscan_ = false;
    frame_had_hires_ = last_frame_hires_ = false;
    mosaic_vindex = mosaic_hcounter = mosaic_hstart = 0;
    mosaic_vblock = 1;
    mosaic_restart = false;
    vram.fill(0x00);
    cgram.fill(0x00);
    oam.fill(0x00);
    
    line_clock = 0;
    current_scanline = 0;
    line_x = WIDTH;
    vblank = false;
    
    inidisp = 0x80;
    obsel = 0x00;
    bgmode = 0x00;
    mosaic = 0x00;
    bgsc.fill(0);
    bg12nba = bg34nba = 0x00;
    bg_hofs.fill(0);
    bg_vofs.fill(0);
    scroll_latch = 0;
    hofs_latch = 0;
    w12sel = w34sel = wobjsel = 0x00;
    wh.fill(0);
    wbglog = wobjlog = 0x00;
    tm = ts = tmw = tsw = 0x00;
    cgwsel = cgadsub = 0x00;
    fixed_color = 0;
    setini = 0x00;
    m7sel = 0;
    m7matrix.fill(0);
    m7x = m7y = m7hofs = m7vofs = 0;
    m7_latch = 0;
    
    vmain = 0x00;
    vram_addr = 0;
    vram_prefetch = 0;
    cgram_addr = 0;
    oam_reload = 0;
    oam_addr = 0;
    oam_latch = 0;
    oam_priority_rotate = false;
    range_over = time_over = false;
    h_latch = v_latch = 0;
    ophct_high = opvct_high = counters_latched = false;
    open_bus = 0x00;
}

auto PPU::read_register(Byte addr) -> Byte {
    Byte result = open_bus;
    
    switch (addr) {
        case 0x34: case 0x35: case 0x36: {  // MPYL/M/H: signed M7A * (M7B >> 8)
            const int32_t product = static_cast<int32_t>(m7matrix[0]) * static_cast<int8_t>(m7matrix[1] >> 8);
            result = static_cast<Byte>(product >> ((addr - 0x34) * 8));
            break;
        }

        case 0x37:  // SLHV: latch the H/V counters
            latch_counters(line_clock / 4, current_scanline);
            break;
            
        case 0x38:  // RDOAM
            result = oam_read();
            break;

        case 0x39:  // VMDATAL
        case 0x3A:  // VMDATAH
            if (vram_accessible()) {
                bool high = (addr == 0x3A);
                result = high ? static_cast<Byte>(vram_prefetch >> 8)
                              : static_cast<Byte>(vram_prefetch & 0xFF);
                // Bit 7 of VMAIN selects which byte triggers the increment. The latch reloads
                // from the address *before* it steps, as on hardware (so after setting VMADD
                // the first word comes back twice; games do a dummy read to skip it).
                if (high == ((vmain & 0x80) != 0)) {
                    prefetch_vram();
                    vram_addr = static_cast<uint16_t>(vram_addr + vram_increment());
                }
            }
            break;
            
        case 0x3B:  // RDCGRAM
            if (cgram_addr < 512) {
                result = cgram[cgram_addr];
            }
            cgram_addr++;
            cgram_addr &= 0x01FF;
            break;
            
        case 0x3C:  // OPHCT: low byte, then bit 8
            result = static_cast<Byte>(ophct_high ? (h_latch >> 8) & 1 : h_latch & 0xFF);
            ophct_high = !ophct_high;
            break;

        case 0x3D:  // OPVCT
            result = static_cast<Byte>(opvct_high ? (v_latch >> 8) & 1 : v_latch & 0xFF);
            opvct_high = !opvct_high;
            break;
            
        case 0x3E:  // STAT77
            result = static_cast<Byte>((time_over ? 0x80 : 0) | (range_over ? 0x40 : 0) | kPpu1Version);
            break;
            
        case 0x3F:  // STAT78: reading resets the OPHCT/OPVCT flip-flops and the latch flag
            result = static_cast<Byte>((field_ ? 0x80 : 0) | (counters_latched ? 0x40 : 0) | kPpu2Version);
            counters_latched = false;
            ophct_high = opvct_high = false;
            break;
            
        default:
            PPU_TRACE("PPU read from register 0x%02X", addr);
            break;
    }
    
    open_bus = result;
    return result;
}

auto PPU::write_register(Byte addr, Byte value) -> void {
  open_bus = value;
  if (line_x < WIDTH) flush_line(line_clock / 4 - kFirstPixelDot);  // draw up to the beam first
  write_register_byte(addr, value);
}

auto PPU::write_register_byte(Byte addr, Byte value) -> void {
  switch (addr) {
      case 0x00: inidisp = value; break;
      case 0x01: obsel = value; break;
      case 0x02:
          oam_reload = static_cast<uint16_t>((oam_reload & 0x100) | value);
          oam_addr = static_cast<uint16_t>(oam_reload << 1);
          break;
      case 0x03:
          oam_reload = static_cast<uint16_t>((oam_reload & 0xFF) | ((value & 0x01) << 8));
          oam_priority_rotate = (value & 0x80) != 0;
          oam_addr = static_cast<uint16_t>(oam_reload << 1);
          break;
      case 0x04: oam_write(value); break;
      case 0x05: bgmode = value; break;
      case 0x06: {
          const bool was_on = (mosaic & 0x0F) != 0;
          mosaic = value;
          if (!was_on && (mosaic & 0x0F)) {  // switched on: a new block starts with the next line
              mosaic_restart = true;
              mosaic_vindex = 0;
          }
          break;
      }
      case 0x07: case 0x08: case 0x09: case 0x0A: bgsc[static_cast<size_t>(addr - 0x07)] = value; break;
      case 0x0B: bg12nba = value; break;
      case 0x0C: bg34nba = value; break;
      case 0x0D: case 0x0F: case 0x11: case 0x13: {
          // Write-twice: new byte is the high bits, the low 3 bits come from the last HOFS write.
          auto& hofs = bg_hofs[static_cast<size_t>((addr - 0x0D) / 2)];
          hofs = static_cast<uint16_t>(((value << 8) | (scroll_latch & ~7) | (hofs_latch & 7)) & 0x3FF);
          scroll_latch = value;
          hofs_latch = value;
          if (addr == 0x0D) {  // BG1HOFS doubles as M7HOFS, written through the mode 7 latch
              m7hofs = sign13((value << 8) | m7_latch);
              m7_latch = value;
          }
          break;
      }
      case 0x0E: case 0x10: case 0x12: case 0x14:
          bg_vofs[static_cast<size_t>((addr - 0x0E) / 2)] = static_cast<uint16_t>(((value << 8) | scroll_latch) & 0x3FF);
          scroll_latch = value;
          if (addr == 0x0E) {
              m7vofs = sign13((value << 8) | m7_latch);
              m7_latch = value;
          }
          break;
      case 0x15: vmain = value; break;
      case 0x16:
          vram_addr = static_cast<uint16_t>((vram_addr & 0xFF00) | value);
          prefetch_vram();
          break;
      case 0x17:
          vram_addr = static_cast<uint16_t>((vram_addr & 0x00FF) | (static_cast<uint16_t>(value) << 8));
          prefetch_vram();
          break;
      case 0x18:
          vram_port_write(false, value);
          break;
      case 0x19:
          vram_port_write(true, value);
          break;
      case 0x1A: m7sel = value; break;
      case 0x1B: case 0x1C: case 0x1D: case 0x1E:  // M7A-D, write twice (low then high)
          m7matrix[static_cast<size_t>(addr - 0x1B)] = static_cast<int16_t>((value << 8) | m7_latch);
          m7_latch = value;
          break;
      case 0x1F: m7x = sign13((value << 8) | m7_latch); m7_latch = value; break;
      case 0x20: m7y = sign13((value << 8) | m7_latch); m7_latch = value; break;
      case 0x21: 
        cgram_addr = static_cast<uint16_t>(value * 2);
        break;
      case 0x22:
          if (cgram_addr < 512) {
              cgram[cgram_addr] = value;
          }
          cgram_addr++;
          cgram_addr &= 0x01FF;
          break;
      case 0x23: w12sel = value; break;
      case 0x24: w34sel = value; break;
      case 0x25: wobjsel = value; break;
      case 0x26: case 0x27: case 0x28: case 0x29: wh[static_cast<size_t>(addr - 0x26)] = value; break;
      case 0x2A: wbglog = value; break;
      case 0x2B: wobjlog = value; break;
      case 0x2C: tm = value; break;
      case 0x2D: ts = value; break;
      case 0x2E: tmw = value; break;
      case 0x2F: tsw = value; break;
      case 0x30: cgwsel = value; break;
      case 0x31: cgadsub = value; break;
      case 0x32: {  // COLDATA: bits 5/6/7 pick which of R/G/B take the 5-bit intensity
          const uint16_t level = value & 0x1F;
          for (int channel = 0; channel < 3; channel++) {
              if (value & (0x20 << channel)) {
                  fixed_color = static_cast<uint16_t>((fixed_color & ~(0x1F << (channel * 5))) | (level << (channel * 5)));
              }
          }
          break;
      }
      case 0x33: setini = value; break;
      
      default:
          PPU_TRACE("PPU write to register 0x%02X = 0x%02X", addr, value);
          break;
  }
}

auto PPU::oam_write(Byte value) -> void {
    if (oam_addr & kOamHighTable) {
        oam[kOamHighTable | (oam_addr & 0x1F)] = value;
    } else if ((oam_addr & 1) == 0) {
        oam_latch = value;              // low table: the even byte waits for its partner
    } else {
        oam[static_cast<size_t>(oam_addr - 1)] = oam_latch;
        oam[oam_addr] = value;
    }
    oam_addr = static_cast<uint16_t>((oam_addr + 1) & kOamAddrMask);
}

auto PPU::oam_read() -> Byte {
    const Byte value = (oam_addr & kOamHighTable) ? oam[kOamHighTable | (oam_addr & 0x1F)] : oam[oam_addr];
    oam_addr = static_cast<uint16_t>((oam_addr + 1) & kOamAddrMask);
    return value;
}

auto PPU::sprite(int index) const -> Sprite {
    const auto base = static_cast<size_t>(index & 127) * 4;
    const Byte high = static_cast<Byte>(oam[kOamHighTable + static_cast<size_t>((index & 127) >> 2)] >> (((index & 3)) * 2));
    const Byte attr = oam[base + 3];
    int x = oam[base] | ((high & 1) << 8);
    if (x >= 256) x -= 512;
    return Sprite{
        x,
        oam[base + 1],
        oam[base + 2] | ((attr & 1) << 8),
        (attr >> 1) & 7,
        (attr >> 4) & 3,
        (attr & 0x40) != 0,
        (attr & 0x80) != 0,
        (high & 2) != 0,
    };
}

auto PPU::sprite_size(bool large) const -> tables::ObjSize {
    return tables::kObjSizes[static_cast<size_t>(obsel >> 5)][large ? 1 : 0];
}

auto PPU::step(uint32_t master_clocks) -> bool {
    bool frame_done = false;

    while (master_clocks > 0) {
        // Run to the next event on this line: HDMA init (line 0), H-blank, HDMA, or the line's end.
        int event = line_clocks(current_scanline);
        if (line_clock < kHdmaClock) event = kHdmaClock;
        if (line_clock < kHblankClock) event = kHblankClock;
        if (current_scanline == 0 && line_clock < kHdmaInitClock) event = kHdmaInitClock;

        const auto run = std::min<uint32_t>(master_clocks, static_cast<uint32_t>(event - line_clock));
        if (bus) bus->on_clock(current_scanline, line_clock, line_clock + static_cast<int>(run));
        line_clock += static_cast<int>(run);
        master_clocks -= run;

        if (line_clock != event) continue;
        if (event == kHdmaInitClock) {
            if (bus) bus->hdma_init();
        } else if (event == kHblankClock) {
            flush_line(WIDTH);
        } else if (event == kHdmaClock) {
            if (bus && current_scanline < vblank_line()) bus->hdma_run_line();
        } else {
            line_clock = 0;
            frame_done |= next_line();
        }
    }

    return frame_done;
}

auto PPU::line_clocks(int v, bool next) const -> int {
    const bool field = next ? !field_ : field_;
    return (v == timing::kShortLine && field && !frame_interlace_) ? timing::kShortLineClocks : kClocksPerLine;
}

auto PPU::clocks_until_line(int line) const -> uint64_t {
    const int lines = lines_this_frame();
    int whole = ((line - current_scanline) % lines + lines) % lines;
    int64_t clocks = static_cast<int64_t>(whole) * kClocksPerLine - line_clock;
    if (clocks <= 0) {
        clocks += static_cast<int64_t>(lines) * kClocksPerLine;
        whole += lines;
    }
    // Less the short line, in this frame or the next, where the way there runs through it.
    const int end = current_scanline + whole;  // the lines run: current_scanline .. end - 1
    constexpr int s = timing::kShortLine;
    if (current_scanline <= s && s < end) clocks -= kClocksPerLine - line_clocks(s);
    if (s < end - lines) clocks -= kClocksPerLine - line_clocks(s, true);
    return static_cast<uint64_t>(clocks);
}

auto PPU::hdma_timing(uint64_t ahead) const -> HdmaTiming {
    constexpr uint64_t kNever = UINT64_MAX;
    const int lines = lines_this_frame();
    int line = current_scanline;
    bool next = false;  // `line` is in the next frame
    auto advance = [&] {
        if (++line < lines) return;
        line = 0;
        next = !next;
    };
    uint64_t clock = static_cast<uint64_t>(line_clock) + ahead;
    while (clock >= static_cast<uint64_t>(line_clocks(line, next))) {
        clock -= static_cast<uint64_t>(line_clocks(line, next));
        advance();
    }
    HdmaTiming timing{kNever, kNever};
    uint64_t distance = 0;
    for (int n = 0; n <= 2 * lines && (timing.init == kNever || timing.run == kNever); n++) {
        if (line == 0 && clock < kHdmaInitClock && timing.init == kNever) {
            timing.init = distance + (kHdmaInitClock - clock);
        }
        if (line < vblank_line() && clock < kHdmaClock && timing.run == kNever) {
            timing.run = distance + (kHdmaClock - clock);
        }
        distance += static_cast<uint64_t>(line_clocks(line, next)) - clock;
        clock = 0;
        advance();
    }
    return timing;
}

auto PPU::next_line() -> bool {
    current_scanline++;
    if (current_scanline >= 1 && current_scanline < vblank_line()) begin_line(current_scanline);

    if (current_scanline == vblank_line()) {
        vblank = true;
        // Outside force blank, the OAM address goes back to the OAMADD value every V-blank.
        if ((inidisp & 0x80) == 0) oam_addr = static_cast<uint16_t>(oam_reload << 1);
        if (bus) bus->on_vblank_start();
    }

    if (current_scanline >= lines_this_frame()) {
        current_scanline = 0;
        field_ = !field_;
        vblank = false;
        range_over = time_over = false;
        if (bus) bus->on_vblank_end();
        return true;
    }
    return false;
}

auto PPU::cgram15(int index) const -> uint16_t {
    const auto addr = static_cast<size_t>(index & 0xFF) * 2;
    return static_cast<uint16_t>((cgram[addr] | (cgram[addr + 1] << 8)) & 0x7FFF);
}

auto PPU::cgram_color(int index) const -> uint32_t {
    if (index >= 256) return 0xFF000000;
    const uint16_t color = cgram15(index);
    const uint32_t r = (color & 0x1F) << 3;
    const uint32_t g = ((color >> 5) & 0x1F) << 3;
    const uint32_t b = ((color >> 10) & 0x1F) << 3;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

auto PPU::read_vram(uint16_t addr) const -> Byte {
  return vram[addr];
}

auto PPU::write_vram(uint16_t addr, Byte value) -> void {
  vram[addr] = value;
}

auto PPU::vram_accessible() const -> bool {
  // Force-blank (INIDISP bit 7) or vblank. Active display ignores the data port.
  return (inidisp & 0x80) != 0 || vblank;
}

auto PPU::vram_increment() const -> uint16_t {
  // VMAIN bits 1-0: step 1, 32, 128, 128 words.
  switch (vmain & 0x03) {
    case 0: return 1;
    case 1: return 32;
    default: return 128;
  }
}

auto PPU::translate_vram_addr(uint16_t addr) const -> uint16_t {
  // VMAIN bits 3-2 rotate a 3-bit group down to the bottom of the word address.
  switch ((vmain >> 2) & 0x03) {
    case 0:
      return addr & 0x7FFF;
    case 1:  // aaaaaaaa bbbccccc -> aaaaaaaa cccccbbb
      return static_cast<uint16_t>(
          ((addr & 0xFF00) | ((addr << 3) & 0x00F8) | ((addr >> 5) & 0x07)) & 0x7FFF);
    case 2:  // aaaaaaa bbbcccccc -> aaaaaaa ccccccbbb
      return static_cast<uint16_t>(
          ((addr & 0xFE00) | ((addr << 3) & 0x01F8) | ((addr >> 6) & 0x07)) & 0x7FFF);
    default: // aaaaaa bbbccccccc -> aaaaaa cccccccbbb
      return static_cast<uint16_t>(
          ((addr & 0xFC00) | ((addr << 3) & 0x03F8) | ((addr >> 7) & 0x07)) & 0x7FFF);
  }
}

auto PPU::prefetch_vram() -> void {
  if (!vram_accessible()) return;
  uint16_t word = translate_vram_addr(vram_addr);
  uint16_t byte_addr = static_cast<uint16_t>(static_cast<uint32_t>(word) * 2u);
  vram_prefetch = static_cast<uint16_t>(read_vram(byte_addr) |
      (static_cast<uint16_t>(read_vram(static_cast<uint16_t>(byte_addr + 1))) << 8));
}

auto PPU::vram_port_write(bool high, Byte value) -> void {
  if (!vram_accessible()) return;
  uint16_t word = translate_vram_addr(vram_addr);
  uint16_t byte_addr = static_cast<uint16_t>(static_cast<uint32_t>(word) * 2u + (high ? 1u : 0u));
  write_vram(byte_addr, value);
  // Writes step the address but leave the read latch alone (it only reloads on VMADD writes
  // and on reads).
  if (high == ((vmain & 0x80) != 0)) vram_addr = static_cast<uint16_t>(vram_addr + vram_increment());
}

auto PPU::get_framebuffer() const -> const uint32_t* {
  return framebuffer.data();
}

void PPU::dump_vram() {
    auto log_row = [this](int base) {
        char bytes[16 * 3 + 1];
        int used = 0;
        bytes[0] = '\0';
        for (int j = 0; j < 16; j++) {
            if (used < 0 || static_cast<size_t>(used) >= sizeof(bytes)) break;
            int wrote = snprintf(bytes + used, sizeof(bytes) - static_cast<size_t>(used),
                                 "%02X%s", vram[static_cast<size_t>(base + j)], j == 15 ? "" : " ");
            if (wrote < 0) return;
            used += wrote;
        }
        utils::Logger::info(utils::LogChannel::Ppu, "%04X: %s", base, bytes);
    };

    utils::Logger::info(utils::LogChannel::Ppu, "VRAM at $0000 (tile data)");
    for (int i = 0; i < 64; i += 16) log_row(i);
    utils::Logger::info(utils::LogChannel::Ppu, "VRAM at $4000 (tilemap)");
    for (int i = 0; i < 64; i += 16) log_row(0x4000 + i);
}

} // namespace bitforge::ppu

namespace bitforge::ppu {

auto PPU::serialize(Archive& ar) -> void {
    ar(field_, frame_interlace_, frame_overscan_, frame_had_hires_, last_frame_hires_, line_clock, current_scanline, line_x,
       vblank);
    ar(inidisp, obsel, bgmode, mosaic, bgsc, bg12nba, bg34nba, bg_hofs, bg_vofs, scroll_latch, hofs_latch, vmain);
    ar(w12sel, w34sel, wobjsel, wh, wbglog, wobjlog, tm, ts, tmw, tsw, cgwsel, cgadsub, fixed_color, setini);
    ar(m7sel, m7matrix, m7x, m7y, m7hofs, m7vofs, m7_latch);
    ar(vram_addr, vram_prefetch, cgram_addr, oam_reload, oam_addr, oam_latch, oam_priority_rotate, range_over, time_over);
    ar(h_latch, v_latch, ophct_high, opvct_high, counters_latched, open_bus);
    ar(vram, cgram, oam);
    ar(mosaic_vindex, mosaic_vblock, mosaic_restart, mosaic_hcounter, mosaic_hstart);
    ar(carry.main_color, carry.math, carry.unclipped, carry.add_sub, carry.halve);

    // The picture: interlaced frames keep the other field's rows, so it is part of the state.
    int w = frame_w_;
    int h = frame_h_;
    ar(w, h);
    if ((w != WIDTH && w != kMaxWidth) || h <= 0 || h > kMaxHeight) {
        ar.fail();
        return;
    }
    frame_w_ = w;
    frame_h_ = h;
    ar.bytes(framebuffer.data(), static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint32_t));
}

} // namespace bitforge::ppu
