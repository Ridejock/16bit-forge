// SPDX-License-Identifier: MIT
#include "frontend/input.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace bitforge::frontend {

namespace {

constexpr double kStickMax = 32767.0;
constexpr double kSectorEdge = 0.38268343236;  // sin(22.5 deg): diagonals get the middle 45 degrees

struct ButtonLabel {
    SDL_GamepadButton button;
    std::string_view label;
};
constexpr std::array<ButtonLabel, 15> kButtonLabels = {{
    {SDL_GAMEPAD_BUTTON_SOUTH, "South (A)"},
    {SDL_GAMEPAD_BUTTON_EAST, "East (B)"},
    {SDL_GAMEPAD_BUTTON_WEST, "West (X)"},
    {SDL_GAMEPAD_BUTTON_NORTH, "North (Y)"},
    {SDL_GAMEPAD_BUTTON_BACK, "Back"},
    {SDL_GAMEPAD_BUTTON_GUIDE, "Guide"},
    {SDL_GAMEPAD_BUTTON_START, "Start"},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, "Left stick"},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "Right stick"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LB"},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RB"},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, "D-pad Up"},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "D-pad Down"},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "D-pad Left"},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "D-pad Right"},
}};

constexpr int16_t kTriggerPress = 16384;  // half travel; released below a quarter
constexpr int16_t kTriggerRelease = 8192;

auto bit(Hotkey h) -> uint32_t { return 1u << static_cast<unsigned>(h); }

} // namespace

auto default_hotkeys() -> HotkeyMap {
    HotkeyMap map;
    for (size_t i = 0; i < kHotkeys.size(); i++) {
        map.keys[i] = kHotkeys[i].default_key;
        map.pad[i] = kHotkeys[i].default_pad;
    }
    return map;
}

auto pad_input_name(int input) -> std::string {
    if (input == kPadTriggerLeft) return "LT";
    if (input == kPadTriggerRight) return "RT";
    if (input < 0 || input >= SDL_GAMEPAD_BUTTON_COUNT) return "(none)";
    return std::string(button_display_name(static_cast<SDL_GamepadButton>(input)));
}

auto pad_input_id(int input) -> std::string {
    if (input == kPadTriggerLeft) return "lefttrigger";
    if (input == kPadTriggerRight) return "righttrigger";
    const char* name = input >= 0 && input < SDL_GAMEPAD_BUTTON_COUNT ? SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(input)) : nullptr;
    return name ? name : "none";
}

auto parse_pad_input(std::string_view id) -> std::optional<int> {
    if (id == "none") return kPadNone;
    if (id == "lefttrigger") return kPadTriggerLeft;
    if (id == "righttrigger") return kPadTriggerRight;
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(std::string(id).c_str());
    if (b == SDL_GAMEPAD_BUTTON_INVALID) return std::nullopt;
    return static_cast<int>(b);
}

auto default_key_map() -> KeyMap {
    KeyMap map{};
    for (size_t i = 0; i < kPadButtons.size(); i++) map[i] = kPadButtons[i].default_key;
    return map;
}

auto default_button_map() -> ButtonMap {
    ButtonMap map{};
    for (size_t i = 0; i < kPadButtons.size(); i++) map[i] = kPadButtons[i].default_button;
    return map;
}

auto guid_string(SDL_GUID guid) -> std::string {
    char text[64];
    SDL_GUIDToString(guid, text, static_cast<int>(sizeof(text)));
    return text;
}

auto button_display_name(SDL_GamepadButton button) -> std::string_view {
    for (const auto& b : kButtonLabels) {
        if (b.button == button) return b.label;
    }
    const char* name = SDL_GetGamepadStringForButton(button);
    return name ? name : "(none)";
}

auto stick_to_dpad(int x, int y, int deadzone_percent) -> uint16_t {
    const double fx = std::clamp(x / kStickMax, -1.0, 1.0);
    const double fy = std::clamp(y / kStickMax, -1.0, 1.0);
    const double magnitude = std::sqrt(fx * fx + fy * fy);
    if (magnitude <= std::clamp(deadzone_percent, 0, 95) / 100.0) return 0;
    uint16_t dirs = 0;
    const double edge = magnitude * kSectorEdge;
    if (fx > edge) dirs |= kRight;
    if (fx < -edge) dirs |= kLeft;
    if (fy > edge) dirs |= kDown;  // SDL: positive Y is down
    if (fy < -edge) dirs |= kUp;
    return dirs;
}

// ---- GamepadMapper -----------------------------------------------------------------------------

auto GamepadMapper::on_button(SDL_GamepadButton button, bool down) -> void {
    if (button < 0 || button >= 32) return;
    const uint32_t bit = 1u << static_cast<unsigned>(button);
    held_ = down ? (held_ | bit) : (held_ & ~bit);
}

auto GamepadMapper::on_axis(SDL_GamepadAxis axis, int16_t value) -> void {
    if (axis == SDL_GAMEPAD_AXIS_LEFTX) stick_x_ = value;
    if (axis == SDL_GAMEPAD_AXIS_LEFTY) stick_y_ = value;
}

auto GamepadMapper::release_all() -> void {
    held_ = 0;
    stick_x_ = stick_y_ = 0;
}

auto GamepadMapper::state() const -> uint16_t {
    uint16_t word = 0;
    for (size_t i = 0; i < kPadButtons.size(); i++) {
        const SDL_GamepadButton b = config_.buttons[i];
        if (b >= 0 && b < 32 && (held_ & (1u << static_cast<unsigned>(b)))) word |= kPadButtons[i].mask;
    }
    if (config_.stick_as_dpad) word |= stick_to_dpad(stick_x_, stick_y_, config_.deadzone);
    // A D-pad can't press opposite directions; some games misbehave if it does.
    if ((word & (kUp | kDown)) == (kUp | kDown)) word &= static_cast<uint16_t>(~(kUp | kDown));
    if ((word & (kLeft | kRight)) == (kLeft | kRight)) word &= static_cast<uint16_t>(~(kLeft | kRight));
    return word;
}

// ---- InputManager ------------------------------------------------------------------------------

auto InputManager::open_connected() -> void {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; i++) add_device(ids[i]);
    SDL_free(ids);
}

auto InputManager::close_all() -> void {
    for (auto& d : devices_) {
        if (d.pad) SDL_CloseGamepad(d.pad);
    }
    devices_.clear();
    assigned_.fill(0);
    for (auto& m : mappers_) m.release_all();
}

auto InputManager::on_mouse_motion(float xrel, float yrel) -> void {
    const float scale = static_cast<float>(pointer_config_.mouse_speed) / 100.f;
    mouse_x_ += xrel * scale;
    mouse_y_ += yrel * scale;
}

auto InputManager::on_mouse_button(uint8_t button, bool down) -> void {
    uint32_t mask = 0;
    if (button == SDL_BUTTON_LEFT) mask = 1;
    if (button == SDL_BUTTON_RIGHT) mask = 2;
    if (button == SDL_BUTTON_MIDDLE) mask = 4;
    if (button == SDL_BUTTON_X1 && down) scope_turbo_ = !scope_turbo_;
    mouse_buttons_ = down ? (mouse_buttons_ | mask) : (mouse_buttons_ & ~mask);
}

auto InputManager::take_mouse_motion() -> std::array<int, 2> {
    const auto dx = static_cast<int>(mouse_x_);  // toward zero; the rest carries over
    const auto dy = static_cast<int>(mouse_y_);
    mouse_x_ -= static_cast<float>(dx);
    mouse_y_ -= static_cast<float>(dy);
    return {dx, dy};
}

auto InputManager::set_port(int port, const PortConfig& config) -> void {
    if (port < 0 || port >= kPadCount) return;
    ports_[static_cast<size_t>(port)] = config;
    mappers_[static_cast<size_t>(port)].set_config(config);
    reassign();
}

auto InputManager::on_key(SDL_Keycode key, bool down, uint64_t when_ns) -> bool {
    bool matched = false;
    const uint32_t before = held_hotkeys();
    for (size_t i = 0; i < kHotkeys.size(); i++) {
        if (hotkeys_.keys[i] != key) continue;
        key_hotkeys_ = down ? (key_hotkeys_ | 1u << i) : (key_hotkeys_ & ~(1u << i));
        matched = true;
    }
    note_presses(before, when_ns);
    for (size_t i = 0; i < kPadButtons.size(); i++) {
        if (keys_[i] != key) continue;
        keyboard_ = down ? static_cast<uint16_t>(keyboard_ | kPadButtons[i].mask)
                         : static_cast<uint16_t>(keyboard_ & ~kPadButtons[i].mask);
        matched = true;
    }
    return matched;
}

auto InputManager::held_hotkeys() const -> uint32_t {
    uint32_t bits = 0;
    for (size_t i = 0; i < kHotkeys.size(); i++) {
        if (hotkey_held(static_cast<Hotkey>(i))) bits |= 1u << i;
    }
    return bits;
}

auto InputManager::hotkey_held(Hotkey h) const -> bool {
    if (key_hotkeys_ & bit(h)) return true;
    const int input = hotkeys_.pad[static_cast<size_t>(h)];
    if (input < 0 || input > kPadTriggerRight) return false;
    return std::any_of(pad_inputs_.begin(), pad_inputs_.end(), [input](const PadInputs& p) { return (p.held >> input) & 1; });
}

auto InputManager::take_hotkey_press(Hotkey h, uint64_t* when_ns) -> bool {
    const bool pressed = (hotkey_presses_ & bit(h)) != 0;
    hotkey_presses_ &= ~bit(h);
    if (when_ns) *when_ns = pressed ? press_ns_[static_cast<size_t>(h)] : 0;
    return pressed;
}

// Hotkeys held now that weren't before are new presses, at `when_ns`.
auto InputManager::note_presses(uint32_t before, uint64_t when_ns) -> void {
    const uint32_t pressed = held_hotkeys() & ~before;
    for (size_t i = 0; i < kHotkeys.size(); i++) {
        if (pressed & (1u << i)) press_ns_[i] = when_ns;
    }
    hotkey_presses_ |= pressed;
}

// A gamepad button or trigger from any pad: the hotkeys' view, and hotkey binding. Returns true
// when the press was taken as a binding.
auto InputManager::on_pad_input(SDL_JoystickID id, int input, bool down, bool* bound, uint64_t when_ns) -> bool {
    if (down && hotkey_binding_) {
        hotkeys_.pad[static_cast<size_t>(*hotkey_binding_)] = input;
        hotkey_binding_.reset();
        if (bound) *bound = true;
        return true;
    }
    auto it = std::find_if(pad_inputs_.begin(), pad_inputs_.end(), [id](const PadInputs& p) { return p.id == id; });
    if (it == pad_inputs_.end()) it = pad_inputs_.insert(pad_inputs_.end(), PadInputs{id, 0});
    const uint64_t b = uint64_t{1} << input;
    if (((it->held & b) != 0) == down) return false;
    const uint32_t before = held_hotkeys();
    it->held = down ? (it->held | b) : (it->held & ~b);
    note_presses(before, when_ns);
    return false;
}

auto InputManager::release_all() -> void {
    keyboard_ = 0;
    mouse_buttons_ = 0;
    mouse_x_ = mouse_y_ = 0.f;
    key_hotkeys_ = 0;
    pad_inputs_.clear();
    hotkey_presses_ = 0;
    for (auto& m : mappers_) m.release_all();
}

auto InputManager::state(int port) const -> uint16_t {
    if (port < 0 || port >= kPadCount) return 0;
    uint16_t word = mappers_[static_cast<size_t>(port)].state();
    if (port == 0) word |= keyboard_;
    return word;
}

auto InputManager::assigned(int port) const -> const Device* {
    if (port < 0 || port >= kPadCount) return nullptr;
    const SDL_JoystickID id = assigned_[static_cast<size_t>(port)];
    for (const auto& d : devices_) {
        if (d.id == id && id != 0) return &d;
    }
    return nullptr;
}

auto InputManager::port_of(SDL_JoystickID id) const -> int {
    for (int p = 0; p < kPadCount; p++) {
        if (assigned_[static_cast<size_t>(p)] == id) return p;
    }
    return -1;
}

auto InputManager::handle_event(const SDL_Event& event, bool* bound) -> bool {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        add_device(event.gdevice.which);
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        remove_device(event.gdevice.which);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const auto button = static_cast<SDL_GamepadButton>(event.gbutton.button);
        const int port = port_of(event.gbutton.which);
        if (on_pad_input(event.gbutton.which, static_cast<int>(button), down, bound, event.common.timestamp)) return true;
        if (down && binding_) {
            // Take the press from the port's own pad, or from any pad if the port has none.
            const int target = binding_->port;
            if (port == target || assigned_[static_cast<size_t>(target)] == 0) {
                PortConfig config = ports_[static_cast<size_t>(target)];
                config.buttons[static_cast<size_t>(binding_->button)] = button;
                binding_.reset();
                set_port(target, config);
                if (bound) *bound = true;
                return true;
            }
        }
        if (port >= 0) mappers_[static_cast<size_t>(port)].on_button(button, down);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const int port = port_of(event.gaxis.which);
        const auto axis = static_cast<SDL_GamepadAxis>(event.gaxis.axis);
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            const int input = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? kPadTriggerLeft : kPadTriggerRight;
            if (event.gaxis.value >= kTriggerPress) on_pad_input(event.gaxis.which, input, true, bound, event.common.timestamp);
            if (event.gaxis.value < kTriggerRelease) on_pad_input(event.gaxis.which, input, false, bound, event.common.timestamp);
            return true;
        }
        if (port >= 0) mappers_[static_cast<size_t>(port)].on_axis(static_cast<SDL_GamepadAxis>(event.gaxis.axis), event.gaxis.value);
        return true;
    }
    default:
        return false;
    }
}

auto InputManager::add_device(SDL_JoystickID id) -> void {
    if (std::any_of(devices_.begin(), devices_.end(), [id](const Device& d) { return d.id == id; })) return;
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        utils::Logger::warning(utils::LogChannel::Input, "Could not open gamepad %u: %s", static_cast<unsigned>(id), SDL_GetError());
        return;
    }
    Device d;
    d.id = id;
    d.pad = pad;
    const char* name = SDL_GetGamepadName(pad);
    d.name = name ? name : "Gamepad";
    d.guid = guid_string(SDL_GetGamepadGUIDForID(id));
    devices_.push_back(std::move(d));
    reassign();
    const int port = port_of(id);
    if (port >= 0) {
        utils::Logger::info(utils::LogChannel::Input, "Gamepad connected: %s -> pad %d", devices_.back().name.c_str(), port + 1);
    } else {
        utils::Logger::info(utils::LogChannel::Input, "Gamepad connected: %s (not assigned)", devices_.back().name.c_str());
    }
}

auto InputManager::remove_device(SDL_JoystickID id) -> void {
    const auto it = std::find_if(devices_.begin(), devices_.end(), [id](const Device& d) { return d.id == id; });
    if (it == devices_.end()) return;
    utils::Logger::info(utils::LogChannel::Input, "Gamepad disconnected: %s", it->name.c_str());
    std::erase_if(pad_inputs_, [id](const PadInputs& p) { return p.id == id; });
    if (it->pad) SDL_CloseGamepad(it->pad);
    devices_.erase(it);
    reassign();
}

// Explicit GUIDs first (each takes the first free pad with that GUID), then "auto" ports take
// the remaining pads in connection order. A port whose pad changes releases its buttons.
auto InputManager::reassign() -> void {
    std::array<SDL_JoystickID, kPadCount> next{};
    std::vector<bool> taken(devices_.size(), false);
    auto take = [&](auto&& match) -> SDL_JoystickID {
        for (size_t i = 0; i < devices_.size(); i++) {
            if (!taken[i] && match(devices_[i])) {
                taken[i] = true;
                return devices_[i].id;
            }
        }
        return 0;
    };
    for (size_t p = 0; p < kPadCount; p++) {
        const std::string& want = ports_[p].device;
        if (want != kAutoDevice && want != kNoDevice) next[p] = take([&](const Device& d) { return d.guid == want; });
    }
    // An auto port keeps the pad it has (unplugging pad 1 doesn't move pad 2 over), then empty
    // auto ports take the remaining pads in connection order.
    for (size_t p = 0; p < kPadCount; p++) {
        const SDL_JoystickID had = assigned_[p];
        if (ports_[p].device == kAutoDevice && had != 0) next[p] = take([had](const Device& d) { return d.id == had; });
    }
    for (size_t p = 0; p < kPadCount; p++) {
        if (ports_[p].device == kAutoDevice && next[p] == 0) next[p] = take([](const Device&) { return true; });
    }
    for (size_t p = 0; p < kPadCount; p++) {
        if (next[p] != assigned_[p]) mappers_[p].release_all();
    }
    assigned_ = next;
}

// ---- console ports ---------------------------------------------------------------------------

auto device_allowed(int port, bitforge::Device device) -> bool { return device != bitforge::Device::SuperScope || port == 1; }

auto parse_device(std::string_view id) -> std::optional<bitforge::Device> {
    for (const auto& d : bitforge::kDevices) {
        if (d.id == id) return d.device;
    }
    return std::nullopt;
}

auto pad_slots(const PortDevices& devices) -> std::array<PadSlot, kPadCount> {
    std::array<PadSlot, kPadCount> slots{};
    size_t next = 0;
    for (int port = 0; port < kPortCount; port++) {
        const bitforge::Device d = devices[static_cast<size_t>(port)];
        const int count = d == bitforge::Device::Gamepad ? 1 : d == bitforge::Device::Multitap ? bitforge::kMultitapSlots : 0;
        for (int s = 0; s < count && next < slots.size(); s++) slots[next++] = {port, s};
    }
    return slots;
}

auto pad_slot_label(const PadSlot& slot, bitforge::Device device) -> std::string {
    if (slot.port < 0) return "not plugged in";
    std::string label = "port " + std::to_string(slot.port + 1);
    if (device == bitforge::Device::Multitap) label += ", multitap " + std::to_string(slot.slot + 1);
    return label;
}

auto scope_aim(float px, float py, float vx, float vy, float vw, float vh, int lines) -> bitforge::ScopeInput {
    bitforge::ScopeInput aim;
    if (vw <= 0.f || vh <= 0.f || lines <= 0) return aim;
    const float x = (px - vx) * 256.f / vw;
    const float y = (py - vy) * static_cast<float>(lines) / vh;
    aim.offscreen = x < 0.f || y < 0.f || x >= 256.f || y >= static_cast<float>(lines);
    aim.x = std::clamp(static_cast<int>(x), 0, 255);
    aim.y = std::clamp(static_cast<int>(y), 0, lines - 1);
    return aim;
}

// A ring with a gap and a center dot, black-outlined white, 7 base pixels across.
auto draw_crosshair(uint32_t* pixels, int width, int height, int lines, int x, int y) -> void {
    if (!pixels || width <= 0 || height <= 0 || lines <= 0) return;
    const int sx = width / 256;               // 1, or 2 for hi-res
    const int sy = std::max(1, height / lines);  // 1, or 2 for interlace
    constexpr uint32_t kWhite = 0xFFFFFF, kBlack = 0x000000;
    auto plot = [&](int bx, int by, uint32_t color) {
        if (bx < 0 || bx >= 256 || by < 0 || by >= lines) return;
        for (int dy = 0; dy < sy; dy++) {
            for (int dx = 0; dx < sx; dx++) {
                const int px = bx * sx + dx, py = by * sy + dy;
                if (py < height) pixels[static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)] = color;
            }
        }
    };
    for (int d = 2; d <= 4; d++) {  // four arms, outlined
        for (const auto& [ax, ay] : {std::pair{d, 0}, std::pair{-d, 0}, std::pair{0, d}, std::pair{0, -d}}) {
            plot(x + ax + (ay != 0), y + ay + (ax != 0), kBlack);
            plot(x + ax - (ay != 0), y + ay - (ax != 0), kBlack);
        }
    }
    for (int d = 2; d <= 4; d++) {
        plot(x + d, y, kWhite);
        plot(x - d, y, kWhite);
        plot(x, y + d, kWhite);
        plot(x, y - d, kWhite);
    }
    plot(x, y, kWhite);
}

} // namespace bitforge::frontend
