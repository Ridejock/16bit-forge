// SPDX-License-Identifier: MIT
#pragma once
#include "frontend/input.hpp"
#include "frontend/rewind.hpp"
#include "utils/logging.hpp"
#include "video/video_backend.hpp"
#include <array>
#include <string>
#include <vector>

namespace bitforge::frontend {

struct Settings {
    static constexpr int kMinScale = 1;
    static constexpr int kMaxScale = 4;
    static constexpr int kDefaultScale = 3;
    static constexpr size_t kMaxRecent = 8;

    int scale = kDefaultScale;
    bool integer_scaling = true;
    bool aspect_8_7 = false;
    bool show_log = true;
    bool show_messages = true;  // on-screen messages in the game window (save/load state)
    video::BackendKind renderer = video::BackendKind::Gpu;  // falls back to SDL_Renderer if GPU fails
    std::string filter = "nearest";                         // a preset name from shaders/presets.ini
    int volume = 100;                                       // percent, 0-100
    int msu1_volume = 100;                                  // MSU-1 audio, percent of the volume
    bool muted = false;
    bool audio_sync = false;  // pace emulation by the audio queue instead of the timer
    std::vector<std::string> recent_roms;  // newest first
    KeyMap keys = default_key_map();                // keyboard, pad 1
    HotkeyMap hotkeys = default_hotkeys();          // [hotkeys]: keyboard and any pad
    SpeedConfig speed;                              // [speed]: rewind, fast-forward
    std::array<PortConfig, kPadCount> ports{};      // [pad1]..[pad5]
    PortDevices port_devices = kDefaultPortDevices; // [ports]: what's in console ports 1 and 2
    PointerConfig pointer;                          // [ports]: mouse speed, scope crosshair

    // [log]: per-sink level and log file, per-channel enable. Applied to utils::Logger.
    struct Log {
        std::array<utils::LogLevel, utils::kLogSinkCount> level{utils::LogLevel::Info, utils::LogLevel::Info};
        std::array<bool, utils::kLogSinkCount> file{true, false};  // logs/system.log on, logs/emu_trace.log off
        std::array<bool, utils::kLogChannelCount> channel = [] {
            std::array<bool, utils::kLogChannelCount> on{};
            on.fill(true);
            return on;
        }();
    };
    Log log;

    auto add_recent(const std::string& path) -> void;

    // Plain INI next to the executable. Missing or unknown keys keep their defaults.
    auto load(const std::string& path) -> void;
    auto save(const std::string& path) const -> bool;
};

} // namespace bitforge::frontend
