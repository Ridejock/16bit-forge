// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <memory>
#include <utility>
#include <span>
#include <vector>
#include "snes/memory/bus.hpp"
#include "snes/cpu/cpu65c816.hpp"
#include "snes/ppu/ppu.hpp"
#include "snes/apu/apu.hpp"
#include "snes/cheats.hpp"
#include "snes/msu1.hpp"

namespace bitforge {

class System {
public:
    System();
    ~System();
    
    auto load_rom(const std::string& path) -> bool;
    // Where load_rom() looks for coprocessor firmware (dsp1b.rom, ...). Set before loading.
    auto set_firmware_dir(const std::string& dir) -> void { bus.cart().set_firmware_dir(dir); }
    // Why load_rom() failed (e.g. "DSP-1B firmware not found: put dsp1b.rom (8192 bytes) in ..."),
    // and a note when the game loaded but its chip isn't emulated.
    auto load_error() const -> const std::string& { return bus.cart().load_error(); }
    auto load_warning() const -> const std::string& { return bus.cart().load_warning(); }
    auto run_frame() -> void;
    auto reset() -> void;
    // The last run_frame() was a lag frame: the game read no controller in it ($4016/$4017 or
    // the auto-read $4218-$421F), so that frame's input went unseen.
    auto lagged() const -> bool { return lagged_; }
    // Master clocks since reset, for the timing checks.
    auto master_clocks() const -> uint64_t { return total_clocks; }
    
    // XRGB8888, packed rows. The size can change from frame to frame: 256 or 512 wide (hi-res),
    // 224 or 239 lines (overscan), doubled with interlace.
    auto get_framebuffer() const -> const uint32_t*;
    auto get_framebuffer_size() const -> std::pair<int, int>;
    
    // Controller 1 latch at $4218. Bits match the word the game reads.
    auto set_joypad_state(uint16_t state) -> void { set_joypad_state(0, state); }
    // Controller `port` (0 or 1): $4218/$421A after auto-read, $4016/$4017 serially.
    auto set_joypad_state(int port, uint16_t state) -> void;
    // Whether a pad is plugged into `port`. Pad 1 is always there; pad 2 starts empty.
    auto set_port_connected(int port, bool connected) -> void { bus.set_port_connected(port, connected); }
    // Other devices: multitap pads (slot 0-3), the SNES Mouse, the Super Scope (port 2).
    auto set_port_device(int port, Device device) -> void { bus.set_port_device(port, device); }
    auto controller(int port) const -> const ControllerPort& { return bus.controller(port); }
    auto set_pad(int port, int slot, uint16_t state) -> void { bus.set_pad(port, slot, state); }
    auto add_mouse(int port, int dx, int dy, bool left, bool right) -> void { bus.controller(port).add_mouse(dx, dy, left, right); }
    auto set_scope(int port, const ScopeInput& input) -> void { bus.controller(port).set_scope(input); }

    // ---- Save states and battery RAM --------------------------------------------------------
    // The whole machine (CPU, WRAM, PPU, DMA/HDMA, timers/IRQ, APU, cartridge SRAM, clocks) as
    // a component-by-component archive. Valid between run_frame() calls. load_state() checks
    // that the data is complete and leaves the machine untouched if it isn't.
    auto save_state(uint32_t version = 0) -> std::vector<uint8_t>;  // 0: the current format
    // `version`: the save-state file version the data came from (0: current), for components
    // whose layout grew since (Archive::at_least).
    auto load_state(std::span<const uint8_t> data, uint32_t version = 0) -> bool;
    auto rom_crc32() const -> uint32_t;
    auto sram() -> std::span<uint8_t>;
    auto sram_dirty() const -> bool;
    auto clear_sram_dirty() -> void;
    // The cartridge's real-time clock file (<rom>.rtc): empty when it has none. Unix time.
    auto rtc_save(int64_t now) -> std::vector<uint8_t>;
    auto rtc_load(std::span<const uint8_t> data, int64_t now) -> bool;

    // For tests and tools: WRAM without side effects, and the cartridge (coprocessor, board).
    auto peek_wram(uint32_t offset) const -> uint8_t { return bus.peek_wram(offset); }
    auto cartridge() -> memory::Cartridge& { return bus.cart(); }

    // Stereo int16 frames (L, R, ...) at apu::APU::kSampleRate produced by the last run_frame().
    auto audio_samples() const -> const std::vector<int16_t>& { return apu.samples(); }
    // MSU-1 (when <rom>.msu sits beside the ROM): its audio for the same frame, the same number
    // of samples at the same rate, for the frontend to mix in. Empty without an MSU-1.
    auto msu1() -> Msu1* { return msu1_.get(); }
    // Cheats (configuration, not machine state): the enabled entries apply from the next read.
    auto set_cheats(std::span<const CheatEntry> entries) -> void { cheats_.set(entries); }
    auto cheats() const -> const CheatEngine& { return cheats_; }

    // The 128KB of WRAM, for a memory watch to read and freeze in place.
    auto wram() -> std::span<uint8_t>;
    // A watch wrote SRAM through wram()'s sibling span. The battery file follows.
    auto mark_sram_dirty() -> void;

    auto msu1_samples() const -> const std::vector<int16_t>& { return msu1_samples_; }

private:
    memory::Bus bus;
    cpu::CPU65c816 cpu;
    ppu::PPU ppu;
    apu::APU apu;
    std::unique_ptr<Msu1> msu1_;
    CheatEngine cheats_;
    std::vector<int16_t> msu1_samples_;
    
    uint64_t total_clocks = 0;  // master clocks since reset
    uint64_t frame_count = 0;   // frames since power-on (drives the periodic debug dumps)
    bool vram_dumped = false;
    bool lagged_ = false;

    auto serialize(Archive& ar) -> void;
};

} // namespace bitforge