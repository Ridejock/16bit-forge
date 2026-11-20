// SPDX-License-Identifier: MIT
#pragma once
#include "snes/controllers.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bitforge::frontend {

// One entry per SNES pad button. `mask` is the bit in the $4218/$4219 word. The gamepad
// defaults put the face buttons where the SNES has them (south = B, east = A, west = Y,
// north = X), which on an Xbox-layout pad such as the Logitech F310 is A->B, B->A, X->Y, Y->X.
struct PadButton {
    std::string_view name;
    uint16_t mask;
    SDL_Keycode default_key;
    SDL_GamepadButton default_button;
};

inline constexpr std::array<PadButton, 12> kPadButtons = {{
    {"B",      0x8000, SDLK_Z,      SDL_GAMEPAD_BUTTON_SOUTH},
    {"Y",      0x4000, SDLK_A,      SDL_GAMEPAD_BUTTON_WEST},
    {"Select", 0x2000, SDLK_RSHIFT, SDL_GAMEPAD_BUTTON_BACK},
    {"Start",  0x1000, SDLK_RETURN, SDL_GAMEPAD_BUTTON_START},
    {"Up",     0x0800, SDLK_UP,     SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"Down",   0x0400, SDLK_DOWN,   SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"Left",   0x0200, SDLK_LEFT,   SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"Right",  0x0100, SDLK_RIGHT,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"A",      0x0080, SDLK_X,      SDL_GAMEPAD_BUTTON_EAST},
    {"X",      0x0040, SDLK_S,      SDL_GAMEPAD_BUTTON_NORTH},
    {"L",      0x0020, SDLK_Q,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"R",      0x0010, SDLK_W,      SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
}};
inline constexpr uint16_t kUp = 0x0800, kDown = 0x0400, kLeft = 0x0200, kRight = 0x0100;

using KeyMap = std::array<SDL_Keycode, kPadButtons.size()>;
using ButtonMap = std::array<SDL_GamepadButton, kPadButtons.size()>;

auto default_key_map() -> KeyMap;
auto default_button_map() -> ButtonMap;

inline constexpr int kPortCount = 2;  // console controller ports
// Pads (players). Each console port takes pads in turn: a gamepad one, a multitap four, a mouse or
// Super Scope none. So port 1 gamepad + port 2 multitap gives pads 1-5.
inline constexpr int kPadCount = 5;

using PortDevices = std::array<bitforge::Device, kPortCount>;
inline constexpr PortDevices kDefaultPortDevices = {bitforge::Device::Gamepad, bitforge::Device::Gamepad};
// Whether `device` can go in `port` (the Super Scope needs port 2's light-gun latch).
auto device_allowed(int port, bitforge::Device device) -> bool;
auto parse_device(std::string_view id) -> std::optional<bitforge::Device>;

struct PadSlot {
    int port = -1;  // -1: this pad isn't plugged in anywhere
    int slot = 0;   // multitap position 0-3
};
auto pad_slots(const PortDevices& devices) -> std::array<PadSlot, kPadCount>;
// "port 2" or "port 2, multitap 3": for the UI.
auto pad_slot_label(const PadSlot& slot, bitforge::Device device) -> std::string;

// Mouse and Super Scope from the host mouse. Mouse buttons: left/right. Scope: left = Fire,
// right = Cursor, middle = Pause, side button (X1) flips the Turbo switch.
struct PointerConfig {
    static constexpr int kDefaultMouseSpeed = 100;
    int mouse_speed = kDefaultMouseSpeed;  // percent: host pixels -> SNES mouse counts
    bool crosshair = true;                 // draw the Super Scope's aim on the picture
};

// The scope's aim from a pointer at (px, py) in window pixels over a picture drawn at
// (vx, vy, vw, vh) with `lines` base lines. Off the picture = offscreen.
auto scope_aim(float px, float py, float vx, float vy, float vw, float vh, int lines) -> bitforge::ScopeInput;
// Draws the aim marker into a frame of `width` x `height` pixels showing `lines` base lines.
auto draw_crosshair(uint32_t* pixels, int width, int height, int lines, int x, int y) -> void;inline constexpr std::string_view kAutoDevice = "auto";  // next free gamepad, in connection order
inline constexpr std::string_view kNoDevice = "none";    // keyboard only (port 1) / unplugged (port 2)

// What one controller port uses. `device` is "auto", "none" or a gamepad GUID string.
struct PortConfig {
    static constexpr int kDefaultDeadzone = 25;  // percent of full stick travel
    std::string device = std::string(kAutoDevice);
    ButtonMap buttons = default_button_map();
    int deadzone = kDefaultDeadzone;
    bool stick_as_dpad = true;  // left stick also drives the D-pad
};

// Emulator hotkeys, on the keyboard (game window) and on any gamepad. A pad binding is an
// SDL_GamepadButton, one of the two triggers (axes on XInput pads such as the F310), or none.
// The run timer's keys default to LiveSplit's numpad layout, which no other key here uses.
enum class Hotkey : uint8_t {
    Rewind,
    FastForward,
    FastForwardToggle,
    MemoryWatch,
    TimerSplit,
    TimerUndo,
    TimerSkip,
    TimerReset,
    TimerPause,
};
inline constexpr int kPadNone = -1;
inline constexpr int kPadTriggerLeft = SDL_GAMEPAD_BUTTON_COUNT;
inline constexpr int kPadTriggerRight = SDL_GAMEPAD_BUTTON_COUNT + 1;
struct HotkeyDef {
    std::string_view name;   // also the settings key
    std::string_view label;  // in the Controls tab
    SDL_Keycode default_key;
    int default_pad;
};
inline constexpr std::array<HotkeyDef, 9> kHotkeys = {{
    {"Rewind", "Rewind (hold)", SDLK_BACKSPACE, kPadTriggerLeft},
    {"FastForward", "Fast-forward (hold)", SDLK_TAB, kPadTriggerRight},
    {"FastForwardToggle", "Fast-forward (toggle)", SDLK_GRAVE, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"MemoryWatch", "Memory watch (show/hide)", SDLK_M, kPadNone},
    {"TimerSplit", "Timer: start / split", SDLK_KP_1, kPadNone},
    {"TimerUndo", "Timer: undo split", SDLK_KP_8, kPadNone},
    {"TimerSkip", "Timer: skip split", SDLK_KP_2, kPadNone},
    {"TimerReset", "Timer: reset", SDLK_KP_3, kPadNone},
    {"TimerPause", "Timer: pause", SDLK_KP_5, kPadNone},
}};
struct HotkeyMap {
    std::array<SDL_Keycode, kHotkeys.size()> keys{};
    std::array<int, kHotkeys.size()> pad{};
};
auto default_hotkeys() -> HotkeyMap;
// "LT", "South (A)", "(none)": for the UI. Settings use pad_input_id()/parse_pad_input().
auto pad_input_name(int input) -> std::string;
auto pad_input_id(int input) -> std::string;            // "lefttrigger", "a", "none", ...
auto parse_pad_input(std::string_view id) -> std::optional<int>;

// The left stick as a D-pad: nothing inside a round deadzone, then eight equal 45-degree
// sectors (an axis counts once it is past sin(22.5 deg) of the stick's deflection).
auto stick_to_dpad(int x, int y, int deadzone_percent) -> uint16_t;

// Gamepad button/axis state -> SNES pad word for one port. Pure logic, no SDL device calls.
class GamepadMapper {
public:
    auto set_config(const PortConfig& config) -> void { config_ = config; }
    auto on_button(SDL_GamepadButton button, bool down) -> void;
    auto on_axis(SDL_GamepadAxis axis, int16_t value) -> void;
    auto release_all() -> void;
    auto state() const -> uint16_t;

private:
    PortConfig config_;
    uint32_t held_ = 0;  // bit per SDL_GamepadButton
    int16_t stick_x_ = 0;
    int16_t stick_y_ = 0;
};

// Keyboard (port 1) plus SDL gamepads with hotplug. Ports set to "auto" take connected
// gamepads in the order they were plugged in and keep them until unplugged; a port set to a
// GUID takes the first free pad with that GUID (two identical pads: set both ports to it).
class InputManager {
public:
    struct Device {
        SDL_JoystickID id = 0;
        SDL_Gamepad* pad = nullptr;
        std::string name;
        std::string guid;
    };

    InputManager() = default;
    InputManager(const InputManager&) = delete;
    auto operator=(const InputManager&) -> InputManager& = delete;
    ~InputManager() { close_all(); }

    // Opens gamepads that are already connected (SDL also sends ADDED events for them; those are ignored).
    auto open_connected() -> void;
    auto close_all() -> void;

    auto set_key_map(const KeyMap& map) -> void { keys_ = map; }
    auto set_hotkeys(const HotkeyMap& map) -> void { hotkeys_ = map; }
    auto hotkeys() const -> const HotkeyMap& { return hotkeys_; }
    // Held on the keyboard or on any gamepad.
    auto hotkey_held(Hotkey h) const -> bool;
    // A press since the last call (for toggles). `when_ns`, if given, gets the press's event
    // time (SDL_GetTicksNS() clock; 0 if the caller passed none).
    auto take_hotkey_press(Hotkey h, uint64_t* when_ns = nullptr) -> bool;
    // Press-to-bind a hotkey's pad input: the next button or trigger from any pad.
    auto start_hotkey_binding(Hotkey h) -> void { binding_.reset(); hotkey_binding_ = h; }
    auto hotkey_binding(Hotkey h) const -> bool { return hotkey_binding_ == h; }
    auto key_map() const -> const KeyMap& { return keys_; }
    // `port` here (and in state/assigned/binding) is the pad number 0..kPadCount-1.
    auto set_port(int port, const PortConfig& config) -> void;
    auto port(int port) const -> const PortConfig& { return ports_[static_cast<size_t>(port)]; }

    // Host mouse, for a SNES Mouse or Super Scope port.
    auto set_pointer_config(const PointerConfig& config) -> void { pointer_config_ = config; }
    auto pointer_config() const -> const PointerConfig& { return pointer_config_; }
    auto on_mouse_motion(float xrel, float yrel) -> void;  // relative motion while captured
    auto on_mouse_button(uint8_t button, bool down) -> void;
    auto take_mouse_motion() -> std::array<int, 2>;  // whole SNES counts since the last call
    auto mouse_left() const -> bool { return (mouse_buttons_ & 1u) != 0; }
    auto mouse_right() const -> bool { return (mouse_buttons_ & 2u) != 0; }
    auto mouse_middle() const -> bool { return (mouse_buttons_ & 4u) != 0; }
    auto scope_turbo() const -> bool { return scope_turbo_; }
    auto set_scope_turbo(bool on) -> void { scope_turbo_ = on; }

    // Game-window keys. Returns true if the key belongs to pad 1 or is a hotkey. `when_ns`: the
    // event's time, for the run timer.
    auto on_key(SDL_Keycode key, bool down, uint64_t when_ns = 0) -> bool;
    // Gamepad add/remove/button/axis events. Returns true if the event was a gamepad event.
    // While binding, the next button press from the port's pad (or any pad) is taken as the
    // new binding instead; `bound` is then set.
    auto handle_event(const SDL_Event& event, bool* bound = nullptr) -> bool;
    auto release_all() -> void;
    auto release_keys() -> void { keyboard_ = 0; }

    // The word for $4218 (port 0) / $421A (port 1).
    auto state(int port) const -> uint16_t;

    auto devices() const -> const std::vector<Device>& { return devices_; }
    auto assigned(int port) const -> const Device*;  // null when the port has no gamepad

    // Press-to-bind: the next gamepad button sets kPadButtons[button] on `port`.
    auto start_binding(int port, int button) -> void { hotkey_binding_.reset(); binding_ = Binding{port, button}; }
    auto cancel_binding() -> void { binding_.reset(); hotkey_binding_.reset(); }
    auto binding(int port, int button) const -> bool {
        return binding_ && binding_->port == port && binding_->button == button;
    }
    auto binding_active() const -> bool { return binding_.has_value() || hotkey_binding_.has_value(); }

private:
    struct Binding {
        int port;
        int button;
    };

    auto add_device(SDL_JoystickID id) -> void;
    auto remove_device(SDL_JoystickID id) -> void;
    auto reassign() -> void;
    auto port_of(SDL_JoystickID id) const -> int;  // -1 if unassigned
    auto on_pad_input(SDL_JoystickID id, int input, bool down, bool* bound, uint64_t when_ns) -> bool;  // hotkeys + binding
    auto note_presses(uint32_t before, uint64_t when_ns) -> void;
    auto held_hotkeys() const -> uint32_t;  // bit per Hotkey

    KeyMap keys_ = default_key_map();
    uint16_t keyboard_ = 0;
    std::array<PortConfig, kPadCount> ports_{};
    std::array<GamepadMapper, kPadCount> mappers_{};
    std::array<SDL_JoystickID, kPadCount> assigned_{};  // 0 = none
    PointerConfig pointer_config_;
    float mouse_x_ = 0.f, mouse_y_ = 0.f;  // motion not yet taken, in SNES counts
    uint32_t mouse_buttons_ = 0;           // bit 0 left, 1 right, 2 middle
    bool scope_turbo_ = false;
    std::vector<Device> devices_;  // connection order
    std::optional<Binding> binding_;
    HotkeyMap hotkeys_ = default_hotkeys();
    uint32_t key_hotkeys_ = 0;  // bit per Hotkey
    struct PadInputs {
        SDL_JoystickID id;
        uint64_t held;  // bit per pad input (buttons, then the two triggers)
    };
    std::vector<PadInputs> pad_inputs_;
    uint32_t hotkey_presses_ = 0;
    std::array<uint64_t, kHotkeys.size()> press_ns_{};  // when each pending press happened
    std::optional<Hotkey> hotkey_binding_;
};

auto guid_string(SDL_GUID guid) -> std::string;
// "South (A)", "LB", "D-pad Up"...: Xbox-style labels, which is what an F310 in XInput mode shows.
auto button_display_name(SDL_GamepadButton button) -> std::string_view;

} // namespace bitforge::frontend
