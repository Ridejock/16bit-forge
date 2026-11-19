// SPDX-License-Identifier: MIT
#pragma once
#include "snes/cheats.hpp"
#include "snes/controllers.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bitforge {
class System;
namespace savestate {
struct Info;
}
}

namespace bitforge::frontend {

// The only surface the UI sees of the core. No SDL or ImGui types cross this line.
class Emulator {
public:
    // NTSC master clock 21.477272 MHz / (262 lines * 1364 clocks).
    static constexpr double kNtscFrameRate = 60.0988;

    // Save-state slots 1..kStateSlots (F1-F10). SRAM is flushed this often while it changes.
    static constexpr int kStateSlots = 10;
    static constexpr uint64_t kSramFlushFrames = 300;  // ~5 s

    Emulator();
    ~Emulator();  // writes battery RAM

    // Where states/ and saves/ go (next to the executable). Empty = current directory.
    auto set_data_dir(const std::string& dir) -> void { data_dir_ = dir; }
    auto data_dir() const -> const std::string& { return data_dir_; }

    // Coprocessor firmware is looked up in <data dir>/firmware. After a failed load,
    // last_error() says why (e.g. which firmware file is missing and its size); after a
    // successful one, last_warning() notes a chip that isn't emulated.
    auto load(const std::string& path) -> bool;
    auto last_error() const -> const std::string& { return last_error_; }
    auto last_warning() const -> const std::string& { return last_warning_; }
    auto unload() -> void;
    auto reset() -> void;
    auto run_frame() -> void;

    auto loaded() const -> bool { return system_ != nullptr; }
    auto rom_path() const -> const std::string& { return rom_path_; }
    auto rom_name() const -> const std::string& { return rom_name_; }
    auto frame_count() const -> uint64_t { return frames_; }

    // XRGB8888, row-major, width() * height() pixels. Always valid while loaded. The size can
    // change between frames (256/512 x 224/239/448/478); the video backends rebuild on a change.
    auto framebuffer() const -> const uint32_t*;
    auto width() const -> int { return width_; }
    auto height() const -> int { return height_; }

    // True until the PPU has produced a non-black pixel; framebuffer() is a test pattern then.
    auto showing_test_pattern() const -> bool { return !seen_output_; }

    // Controller `port` (0 = pad 1, 1 = pad 2) in $4218/$4219 bit order.
    auto set_joypad(int port, uint16_t state) -> void;
    // Pad 2 only reads as plugged in while a gamepad is assigned to it.
    auto set_port_connected(int port, bool connected) -> void;
    // Multitap, SNES Mouse and Super Scope (see bitforge::ControllerPort). No-ops without a game.
    auto set_port_device(int port, bitforge::Device device) -> void;
    auto set_pad(int port, int slot, uint16_t state) -> void;
    auto add_mouse(int port, int dx, int dy, bool left, bool right) -> void;
    auto set_scope(int port, const bitforge::ScopeInput& input) -> void;

    // ---- Save states: states/<rom>.<slot>.sst ---------------------------------------------
    struct SlotInfo {
        int64_t timestamp = 0;  // seconds since 1970
        uint64_t frame = 0;     // frames run since boot when saved
        uint32_t version = 0;   // save-state file version
        int thumb_w = 0;
        int thumb_h = 0;
        std::vector<uint32_t> thumbnail;  // XRGB8888
    };
    auto state_path(int slot) const -> std::string;
    // The same without an Emulator (the UI thread lists slots from the published ROM path).
    static auto state_path_for(const std::string& data_dir, const std::string& rom_path, int slot) -> std::string;
    // Returns an empty string on success, else why it failed (for the UI).
    auto save_state(int slot) -> std::string;
    auto load_state(int slot) -> std::string;
    // Header and thumbnail of a slot's file, if it exists and belongs to this emulator version.
    auto slot_info(int slot) const -> std::optional<SlotInfo>;
    // In-memory snapshot for rewind: the frame counter and the machine (no header or thumbnail).
    auto save_snapshot() -> std::vector<uint8_t>;
    auto load_snapshot(std::span<const uint8_t> data) -> bool;
    // Writes slot `slot` from a rewind snapshot (frame counter, then the machine) and that
    // moment's picture, without loading it. The running game stays where it is.
    auto save_snapshot_to_slot(int slot, std::span<const uint8_t> snapshot, const uint32_t* thumb, int thumb_w,
                               int thumb_h) -> std::string;
    static auto read_slot_info(const std::string& path) -> std::optional<SlotInfo>;
    // For the slot menu: "just now", "5 min ago", "3 h ago", "2 days ago" (`now` and `saved` in
    // Unix seconds), and "1:02:03" for a frame count at the NTSC rate.
    static auto age_text(int64_t saved, int64_t now) -> std::string;
    static auto play_time_text(uint64_t frames) -> std::string;

    // ---- Battery RAM: saves/<rom>.srm, loaded at boot, written when dirty --------------------
    auto sram_path() const -> std::string;
    auto flush_sram() -> void;  // writes the file if SRAM changed since the last write, and the .rtc clock file
    auto rtc_path() const -> std::string;

    // ---- Cheats: cheats/<rom>.cht, loaded at boot -----------------------------------------
    auto cheats_path() const -> std::string;
    auto cheats() const -> const std::vector<CheatEntry>& { return cheats_; }
    // Applies the list (enabled entries, from the next bus read) and writes the file.
    auto set_cheats(std::vector<CheatEntry> entries) -> bool;

    // Stereo int16 frames (L, R, L, R, ...) the last run_frame() produced, at audio_sample_rate().
    auto audio_samples() const -> std::span<const int16_t>;
    static auto audio_sample_rate() -> int;  // 32040 Hz
    // The MSU-1's audio for the same frame (as many samples, same rate); empty without one.
    auto msu1_samples() const -> std::span<const int16_t>;
    auto has_msu1() const -> bool;

private:
    auto update_test_pattern() -> void;
    auto load_sram() -> void;
    auto load_cheats() -> void;
    auto rom_stem() const -> std::string;

    std::unique_ptr<System> system_;
    std::vector<CheatEntry> cheats_;
    std::string data_dir_;
    std::string last_error_;
    std::string last_warning_;
    std::string rom_path_;
    std::string rom_name_;
    std::vector<uint32_t> test_pattern_;
    int width_ = 0;
    int height_ = 0;
    uint64_t frames_ = 0;
    bool seen_output_ = false;
};

} // namespace bitforge::frontend
