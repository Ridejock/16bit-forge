// SPDX-License-Identifier: MIT
#include "frontend/settings.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace bitforge::frontend {

namespace {

constexpr std::string_view kRecentPrefix = "rom";

auto trim(std::string_view s) -> std::string_view {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

auto parse_bool(std::string_view v) -> bool { return v == "1" || v == "true"; }

auto parse_int(const std::string& v, int lo, int hi, int& out) -> void {
    try {
        out = std::clamp(std::stoi(v), lo, hi);
    } catch (...) {
    }
}

auto parse_level(std::string_view v) -> std::optional<utils::LogLevel> {
    for (size_t i = 0; i < utils::kLogLevelNames.size(); i++) {
        if (utils::kLogLevelNames[i] == v) return static_cast<utils::LogLevel>(i);
    }
    return std::nullopt;
}

// "pad1" -> 0
auto port_section(std::string_view section) -> int {
    for (int p = 0; p < kPadCount; p++) {
        if (section == "pad" + std::to_string(p + 1)) return p;
    }
    return -1;
}

auto load_port(PortConfig& port, std::string_view key, const std::string& value) -> void {
    if (key == "device") {
        if (!value.empty()) port.device = value;
    } else if (key == "deadzone") {
        try {
            port.deadzone = std::clamp(std::stoi(value), 0, 95);
        } catch (...) {
        }
    } else if (key == "stick_as_dpad") {
        port.stick_as_dpad = parse_bool(value);
    } else {
        for (size_t i = 0; i < kPadButtons.size(); i++) {
            if (key != kPadButtons[i].name) continue;
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(value.c_str());
            if (b != SDL_GAMEPAD_BUTTON_INVALID) port.buttons[i] = b;
        }
    }
}

} // namespace

auto Settings::add_recent(const std::string& path) -> void {
    std::erase(recent_roms, path);
    recent_roms.insert(recent_roms.begin(), path);
    if (recent_roms.size() > kMaxRecent) recent_roms.resize(kMaxRecent);
}

auto Settings::load(const std::string& path) -> void {
    std::ifstream in(path);
    if (!in) return;

    std::vector<std::string> recent;
    std::string section;
    std::string raw;
    while (std::getline(in, raw)) {
        std::string_view line = trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = std::string(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, eq));
        const std::string value(trim(line.substr(eq + 1)));

        if (section == "view") {
            if (key == "scale") {
                try {
                    scale = std::clamp(std::stoi(value), kMinScale, kMaxScale);
                } catch (...) {
                }
            } else if (key == "integer_scaling") {
                integer_scaling = parse_bool(value);
            } else if (key == "aspect_8_7") {
                aspect_8_7 = parse_bool(value);
            } else if (key == "show_log") {
                show_log = parse_bool(value);
            } else if (key == "show_messages") {
                show_messages = parse_bool(value);
            }
        } else if (section == "video") {
            if (key == "renderer") {
                if (const auto kind = video::parse_backend_kind(value)) renderer = *kind;
            } else if (key == "filter" && !value.empty()) {
                filter = value;
            }
        } else if (section == "audio") {
            if (key == "volume") {
                try {
                    volume = std::clamp(std::stoi(value), 0, 100);
                } catch (...) {
                }
            } else if (key == "msu1_volume") {
                parse_int(value, 0, 100, msu1_volume);
            } else if (key == "muted") {
                muted = parse_bool(value);
            } else if (key == "sync_to_audio") {
                audio_sync = parse_bool(value);
            }
        } else if (section == "recent") {
            if (key.starts_with(kRecentPrefix) && !value.empty() && recent.size() < kMaxRecent) {
                recent.push_back(value);
            }
        } else if (const int port = port_section(section); port >= 0) {
            load_port(ports[static_cast<size_t>(port)], key, value);
        } else if (section == "log") {
            for (size_t i = 0; i < utils::kLogSinkCount; i++) {
                const std::string name(utils::kLogSinks[i].name);
                if (key == name + "_level") {
                    if (const auto level = parse_level(value)) log.level[i] = *level;
                } else if (key == name + "_file") {
                    log.file[i] = parse_bool(value);
                }
            }
            for (size_t i = 0; i < utils::kLogChannelCount; i++) {
                if (key == "channel." + std::string(utils::kLogChannels[i].name)) log.channel[i] = parse_bool(value);
            }
        } else if (section == "speed") {
            if (key == "rewind") {
                speed.rewind = parse_bool(value);
            } else if (key == "rewind_budget_mb") {
                parse_int(value, SpeedConfig::kMinBudgetMb, SpeedConfig::kMaxBudgetMb, speed.rewind_budget_mb);
            } else if (key == "rewind_interval") {
                parse_int(value, 1, SpeedConfig::kMaxInterval, speed.rewind_interval);
            } else if (key == "rewind_audio") {
                speed.rewind_reverse_audio = value == "reverse";
            } else if (key == "fast_forward_speed") {
                parse_int(value, 0, SpeedConfig::kMaxSpeed, speed.fast_forward_speed);
                if (speed.fast_forward_speed == 1) speed.fast_forward_speed = 2;
            }
        } else if (section == "ports") {
            for (int p = 0; p < kPortCount; p++) {
                if (key != "port" + std::to_string(p + 1)) continue;
                const auto device = parse_device(value);
                if (device && device_allowed(p, *device)) port_devices[static_cast<size_t>(p)] = *device;
            }
            if (key == "mouse_speed") parse_int(value, 10, 400, pointer.mouse_speed);
            if (key == "crosshair") pointer.crosshair = parse_bool(value);
        } else if (section == "hotkeys") {
            for (size_t i = 0; i < kHotkeys.size(); i++) {
                if (key == kHotkeys[i].name) {
                    const SDL_Keycode code = value == "none" ? SDLK_UNKNOWN : SDL_GetKeyFromName(value.c_str());
                    if (code != SDLK_UNKNOWN || value == "none") hotkeys.keys[i] = code;
                } else if (key == std::string(kHotkeys[i].name) + ".pad") {
                    if (const auto input = parse_pad_input(value)) hotkeys.pad[i] = *input;
                }
            }
        } else if (section == "timer") {
            if (key == "primary") {
                for (const auto& m : speedrun::kMethods) {
                    if (value == m.key) timer.primary = m.id;
                }
            } else if (key == "overlay") {
                timer.overlay = parse_bool(value);
            } else if (key == "corner") {
                for (size_t i = 0; i < kCorners.size(); i++) {
                    if (value == kCorners[i].id) timer.corner = static_cast<Corner>(i);
                }
            } else if (key == "show") {
                show_timer = parse_bool(value);
            }
        } else if (section == "keys") {
            for (size_t i = 0; i < kPadButtons.size(); i++) {
                if (key != kPadButtons[i].name) continue;
                SDL_Keycode code = SDL_GetKeyFromName(value.c_str());
                if (code != SDLK_UNKNOWN) keys[i] = code;
            }
        }
    }
    recent_roms = std::move(recent);
}

auto Settings::save(const std::string& path) const -> bool {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "[view]\n"
        << "scale=" << scale << "\n"
        << "integer_scaling=" << (integer_scaling ? 1 : 0) << "\n"
        << "aspect_8_7=" << (aspect_8_7 ? 1 : 0) << "\n"
        << "show_log=" << (show_log ? 1 : 0) << "\n"
        << "show_messages=" << (show_messages ? 1 : 0) << "\n\n";

    out << "[video]\n"
        << "renderer=" << video::backend_kind_name(renderer) << "\n"
        << "filter=" << filter << "\n\n";

    out << "[audio]\n"
        << "volume=" << volume << "\n"
        << "msu1_volume=" << msu1_volume << "\n"
        << "muted=" << (muted ? 1 : 0) << "\n"
        << "sync_to_audio=" << (audio_sync ? 1 : 0) << "\n\n";

    out << "[speed]\n"
        << "rewind=" << (speed.rewind ? 1 : 0) << "\n"
        << "rewind_budget_mb=" << speed.rewind_budget_mb << "\n"
        << "rewind_interval=" << speed.rewind_interval << "\n"
        << "rewind_audio=" << (speed.rewind_reverse_audio ? "reverse" : "mute") << "\n"
        << "fast_forward_speed=" << speed.fast_forward_speed << "\n\n";

    out << "[ports]\n";
    for (size_t p = 0; p < port_devices.size(); p++) out << "port" << (p + 1) << "=" << bitforge::device_info(port_devices[p]).id << "\n";
    out << "mouse_speed=" << pointer.mouse_speed << "\n"
        << "crosshair=" << (pointer.crosshair ? 1 : 0) << "\n\n";

    out << "[hotkeys]\n";
    for (size_t i = 0; i < kHotkeys.size(); i++) {
        out << kHotkeys[i].name << "=" << (hotkeys.keys[i] == SDLK_UNKNOWN ? "none" : SDL_GetKeyName(hotkeys.keys[i])) << "\n"
            << kHotkeys[i].name << ".pad=" << pad_input_id(hotkeys.pad[i]) << "\n";
    }
    out << "\n";

    out << "[timer]\n"
        << "primary=" << speedrun::info(timer.primary).key << "\n"
        << "overlay=" << (timer.overlay ? 1 : 0) << "\n"
        << "corner=" << info(timer.corner).id << "\n"
        << "show=" << (show_timer ? 1 : 0) << "\n\n";

    out << "[recent]\n";
    for (size_t i = 0; i < recent_roms.size(); i++) {
        out << kRecentPrefix << i << "=" << recent_roms[i] << "\n";
    }

    out << "\n[keys]\n";
    for (size_t i = 0; i < kPadButtons.size(); i++) {
        out << kPadButtons[i].name << "=" << SDL_GetKeyName(keys[i]) << "\n";
    }

    for (size_t p = 0; p < ports.size(); p++) {
        const PortConfig& port = ports[p];
        out << "\n[pad" << (p + 1) << "]\n"
            << "device=" << port.device << "\n"
            << "deadzone=" << port.deadzone << "\n"
            << "stick_as_dpad=" << (port.stick_as_dpad ? 1 : 0) << "\n";
        for (size_t i = 0; i < kPadButtons.size(); i++) {
            const char* name = SDL_GetGamepadStringForButton(port.buttons[i]);
            out << kPadButtons[i].name << "=" << (name ? name : "") << "\n";
        }
    }

    out << "\n[log]\n";
    for (size_t i = 0; i < utils::kLogSinkCount; i++) {
        out << utils::kLogSinks[i].name << "_level=" << utils::kLogLevelNames[static_cast<size_t>(log.level[i])] << "\n"
            << utils::kLogSinks[i].name << "_file=" << (log.file[i] ? 1 : 0) << "\n";
    }
    for (size_t i = 0; i < utils::kLogChannelCount; i++) {
        out << "channel." << utils::kLogChannels[i].name << "=" << (log.channel[i] ? 1 : 0) << "\n";
    }
    return static_cast<bool>(out);
}

} // namespace bitforge::frontend
