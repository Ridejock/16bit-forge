// SPDX-License-Identifier: MIT
// Controller input: the stick/button -> SNES pad mapping, and InputManager with real SDL
// gamepad events from SDL virtual joysticks (hotplug, port assignment, press-to-bind), plus the
// ini round trip of the pad settings. Headless: no window or physical pad needed.
#include "frontend/input.hpp"
#include "frontend/settings.hpp"
#include "utils/logging.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace bitforge::frontend;

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

constexpr uint16_t kB = 0x8000, kY = 0x4000, kSelect = 0x2000, kStart = 0x1000, kA = 0x0080, kX = 0x0040,
                   kL = 0x0020, kR = 0x0010;

auto test_stick() -> void {
    CHECK(stick_to_dpad(0, 0, 25) == 0);
    CHECK(stick_to_dpad(8000, 0, 25) == 0);               // ~24%: inside the deadzone
    CHECK(stick_to_dpad(9000, 0, 25) == kRight);          // ~27%: out
    CHECK(stick_to_dpad(-32768, 0, 25) == kLeft);
    CHECK(stick_to_dpad(0, -32768, 25) == kUp);           // SDL: negative Y is up
    CHECK(stick_to_dpad(0, 32767, 25) == kDown);
    CHECK(stick_to_dpad(23170, 23170, 25) == (kRight | kDown));  // 45 degrees
    CHECK(stick_to_dpad(30000, 9000, 25) == kRight);      // ~17 degrees: still a cardinal
    CHECK(stick_to_dpad(30000, 14000, 25) == (kRight | kDown));  // ~25 degrees: diagonal
    CHECK(stick_to_dpad(-16000, -16000, 25) == (kLeft | kUp));   // radial: 69% along the diagonal
    CHECK(stick_to_dpad(5500, 5500, 25) == 0);            // 24% radially: inside
    CHECK(stick_to_dpad(6500, 6500, 25) == (kRight | kDown));  // 28% radially: out
}

auto test_default_layout() -> void {
    GamepadMapper m;
    struct Case {
        SDL_GamepadButton button;
        uint16_t mask;
    };
    // SNES positional layout on an Xbox-style pad (F310 in XInput mode).
    const Case cases[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, kB},  {SDL_GAMEPAD_BUTTON_EAST, kA},
        {SDL_GAMEPAD_BUTTON_WEST, kY},   {SDL_GAMEPAD_BUTTON_NORTH, kX},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
        {SDL_GAMEPAD_BUTTON_START, kStart}, {SDL_GAMEPAD_BUTTON_BACK, kSelect},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
    };
    for (const auto& c : cases) {
        m.on_button(c.button, true);
        CHECK(m.state() == c.mask);
        m.on_button(c.button, false);
        CHECK(m.state() == 0);
    }
    m.on_button(SDL_GAMEPAD_BUTTON_GUIDE, true);  // unmapped
    CHECK(m.state() == 0);
    m.release_all();

    // Stick and D-pad combine; opposite directions cancel.
    m.on_axis(SDL_GAMEPAD_AXIS_LEFTX, 32767);
    CHECK(m.state() == kRight);
    m.on_button(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    CHECK(m.state() == (kRight | kUp));
    m.on_button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    CHECK(m.state() == kUp);
    m.release_all();
    CHECK(m.state() == 0);

    // Config: rebinding, deadzone, stick off.
    PortConfig config;
    config.buttons[0] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;  // B on RB
    config.deadzone = 60;
    m.set_config(config);
    m.on_button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    CHECK(m.state() == (kB | kR));
    m.release_all();
    m.on_axis(SDL_GAMEPAD_AXIS_LEFTY, 16000);  // 49%: inside 60%
    CHECK(m.state() == 0);
    m.on_axis(SDL_GAMEPAD_AXIS_LEFTY, 30000);
    CHECK(m.state() == kDown);
    config.stick_as_dpad = false;
    m.set_config(config);
    CHECK(m.state() == 0);
}

// ---- SDL virtual gamepads -------------------------------------------------------------------

auto attach_pad(uint16_t product) -> SDL_JoystickID {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x046D;  // Logitech
    desc.product_id = product;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = product == 0xC21D ? "Virtual F310" : "Virtual pad";
    return SDL_AttachVirtualJoystick(&desc);
}

// Pumps SDL and feeds every event to the manager, like the app's loop.
auto pump(InputManager& input, bool* bound = nullptr) -> void {
    SDL_UpdateJoysticks();
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e)) input.handle_event(e, bound);
}

auto press(SDL_Joystick* joy, SDL_GamepadButton button, bool down, InputManager& input, bool* bound = nullptr) -> void {
    SDL_SetJoystickVirtualButton(joy, button, down);
    pump(input, bound);
}

auto test_virtual_pads() -> bool {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::printf("(SDL gamepad init failed: %s) ", SDL_GetError());
        return false;
    }
    InputManager input;
    input.open_connected();
    pump(input);
    CHECK(input.devices().empty());

    // Two pads plugged in: pad 1 then pad 2, by connection order.
    const SDL_JoystickID id_a = attach_pad(0xC21D);
    pump(input);
    const SDL_JoystickID id_b = attach_pad(0xC21E);
    pump(input);
    CHECK(input.devices().size() == 2);
    CHECK(input.assigned(0) && input.assigned(0)->id == id_a);
    CHECK(input.assigned(1) && input.assigned(1)->id == id_b);
    SDL_Joystick* joy_a = SDL_OpenJoystick(id_a);
    SDL_Joystick* joy_b = SDL_OpenJoystick(id_b);
    CHECK(joy_a && joy_b);

    press(joy_a, SDL_GAMEPAD_BUTTON_SOUTH, true, input);
    CHECK(input.state(0) == kB && input.state(1) == 0);
    press(joy_b, SDL_GAMEPAD_BUTTON_EAST, true, input);
    CHECK(input.state(0) == kB && input.state(1) == kA);
    SDL_SetJoystickVirtualAxis(joy_b, SDL_GAMEPAD_AXIS_LEFTX, -32768);
    pump(input);
    CHECK(input.state(1) == (kA | kLeft));
    SDL_SetJoystickVirtualAxis(joy_b, SDL_GAMEPAD_AXIS_LEFTX, 0);
    press(joy_a, SDL_GAMEPAD_BUTTON_SOUTH, false, input);
    press(joy_b, SDL_GAMEPAD_BUTTON_EAST, false, input);
    CHECK(input.state(0) == 0 && input.state(1) == 0);

    // The keyboard still drives pad 1 alongside the gamepad.
    input.on_key(SDLK_X, true);
    press(joy_a, SDL_GAMEPAD_BUTTON_NORTH, true, input);
    CHECK(input.state(0) == (kA | kX));
    input.on_key(SDLK_X, false);
    press(joy_a, SDL_GAMEPAD_BUTTON_NORTH, false, input);

    // Unplug pad 1 while its button is held: the port releases, pad 2 stays on port 2.
    press(joy_a, SDL_GAMEPAD_BUTTON_START, true, input);
    CHECK(input.state(0) == kStart);
    SDL_CloseJoystick(joy_a);
    SDL_DetachVirtualJoystick(id_a);
    pump(input);
    CHECK(input.devices().size() == 1);
    CHECK(input.assigned(0) == nullptr);
    CHECK(input.assigned(1) && input.assigned(1)->id == id_b);
    CHECK(input.state(0) == 0);

    // Plug it back in: it fills the free port 1.
    const SDL_JoystickID id_c = attach_pad(0xC21D);
    pump(input);
    CHECK(input.assigned(0) && input.assigned(0)->id == id_c);
    SDL_Joystick* joy_c = SDL_OpenJoystick(id_c);

    // Press-to-bind on port 2: the next press on pad 2 becomes its "A"; pad 1 presses don't count.
    input.start_binding(1, 8);  // kPadButtons[8] = A
    bool bound = false;
    press(joy_c, SDL_GAMEPAD_BUTTON_NORTH, true, input, &bound);
    CHECK(!bound && input.binding_active());
    press(joy_c, SDL_GAMEPAD_BUTTON_NORTH, false, input);
    press(joy_b, SDL_GAMEPAD_BUTTON_LEFT_STICK, true, input, &bound);
    CHECK(bound && !input.binding_active());
    CHECK(input.port(1).buttons[8] == SDL_GAMEPAD_BUTTON_LEFT_STICK);
    CHECK(input.state(1) == 0);  // the binding press isn't passed to the game
    press(joy_b, SDL_GAMEPAD_BUTTON_LEFT_STICK, false, input);
    press(joy_b, SDL_GAMEPAD_BUTTON_LEFT_STICK, true, input);
    CHECK(input.state(1) == kA);
    press(joy_b, SDL_GAMEPAD_BUTTON_LEFT_STICK, false, input);

    // A port pinned to a GUID takes that pad even if it was plugged in second.
    const std::string guid_b = input.assigned(1)->guid;
    CHECK(guid_b != input.assigned(0)->guid);
    PortConfig pinned = input.port(0);
    pinned.device = guid_b;
    input.set_port(0, pinned);
    CHECK(input.assigned(0) && input.assigned(0)->id == id_b);
    CHECK(input.assigned(1) && input.assigned(1)->id == id_c);  // auto port 2 gets the other one
    PortConfig none = input.port(1);
    none.device = std::string(kNoDevice);
    input.set_port(1, none);
    CHECK(input.assigned(1) == nullptr);

    SDL_CloseJoystick(joy_b);
    SDL_CloseJoystick(joy_c);
    input.close_all();
    SDL_DetachVirtualJoystick(id_b);
    SDL_DetachVirtualJoystick(id_c);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return true;
}

// Rewind / fast-forward hotkeys: keyboard keys, F310 triggers (as pseudo-buttons with
// hysteresis) and R3 from any pad, press edges for the toggle, press-to-bind a pad input.
auto test_hotkeys() -> bool {
    CHECK(pad_input_id(kPadTriggerLeft) == "lefttrigger" && parse_pad_input("lefttrigger") == kPadTriggerLeft);
    CHECK(parse_pad_input("rightstick") == SDL_GAMEPAD_BUTTON_RIGHT_STICK && parse_pad_input("none") == kPadNone);
    CHECK(!parse_pad_input("bogus"));
    InputManager input;
    CHECK(input.on_key(SDLK_BACKSPACE, true));
    CHECK(input.hotkey_held(Hotkey::Rewind) && !input.hotkey_held(Hotkey::FastForward));
    CHECK(input.state(0) == 0);
    input.on_key(SDLK_BACKSPACE, false);
    CHECK(!input.hotkey_held(Hotkey::Rewind));
    input.on_key(SDLK_GRAVE, true);
    input.on_key(SDLK_GRAVE, false);
    CHECK(input.take_hotkey_press(Hotkey::FastForwardToggle) && !input.take_hotkey_press(Hotkey::FastForwardToggle));

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
    input.open_connected();
    const SDL_JoystickID id = attach_pad(0xC21D);
    pump(input);
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    CHECK(joy != nullptr);
    // Virtual trigger axes are raw -32768..32767; the gamepad reports them as 0..32767.
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 20000);
    pump(input);
    CHECK(input.hotkey_held(Hotkey::FastForward) && input.state(0) == 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -8000);  // ~12400: between, still held
    pump(input);
    CHECK(input.hotkey_held(Hotkey::FastForward));
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    pump(input);
    CHECK(!input.hotkey_held(Hotkey::FastForward));
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    pump(input);
    CHECK(input.hotkey_held(Hotkey::Rewind));
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    pump(input);
    press(joy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true, input);
    press(joy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false, input);
    CHECK(input.take_hotkey_press(Hotkey::FastForwardToggle));

    // Bind rewind to Select: the binding press is swallowed; afterwards Select is rewind, not SNES Select.
    input.start_hotkey_binding(Hotkey::Rewind);
    CHECK(input.binding_active() && input.hotkey_binding(Hotkey::Rewind));
    bool bound = false;
    press(joy, SDL_GAMEPAD_BUTTON_BACK, true, input, &bound);
    CHECK(bound && !input.binding_active() && input.hotkeys().pad[0] == SDL_GAMEPAD_BUTTON_BACK);
    CHECK(!input.hotkey_held(Hotkey::Rewind) && input.state(0) == 0);
    press(joy, SDL_GAMEPAD_BUTTON_BACK, false, input);
    press(joy, SDL_GAMEPAD_BUTTON_BACK, true, input);
    CHECK(input.hotkey_held(Hotkey::Rewind));
    // Unplugging drops whatever that pad held.
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    pump(input);
    CHECK(!input.hotkey_held(Hotkey::Rewind));
    input.close_all();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return true;
}

// Pads to console ports, four virtual pads on a multitap, the host mouse, and the scope's aim.
auto test_controller_devices() -> bool {
    using bitforge::Device;
    auto slots = pad_slots(kDefaultPortDevices);
    CHECK(slots[0].port == 0 && slots[1].port == 1 && slots[2].port < 0);
    slots = pad_slots({Device::Gamepad, Device::Multitap});
    CHECK(slots[0].port == 0 && slots[1].port == 1 && slots[1].slot == 0 && slots[4].port == 1 && slots[4].slot == 3);
    slots = pad_slots({Device::Multitap, Device::Gamepad});
    CHECK(slots[3].port == 0 && slots[3].slot == 3 && slots[4].port == 1 && slots[4].slot == 0);
    slots = pad_slots({Device::Mouse, Device::Gamepad});
    CHECK(slots[0].port == 1 && slots[1].port < 0);  // pad 1 (and the keyboard) moves to port 2
    slots = pad_slots({Device::Gamepad, Device::SuperScope});
    CHECK(slots[0].port == 0 && slots[1].port < 0);
    CHECK(pad_slot_label({1, 2}, Device::Multitap) == "port 2, multitap 3");
    CHECK(device_allowed(1, Device::SuperScope) && !device_allowed(0, Device::SuperScope));
    CHECK(parse_device("multitap") == Device::Multitap && !parse_device("zapper"));

    // Scope aim: a 3x picture at (64, 0) in a 896x672 window; 239 lines when overscanned.
    auto aim = scope_aim(64 + 3 * 100 + 1, 3 * 50 + 1, 64, 0, 768, 672, 224);
    CHECK(!aim.offscreen && aim.x == 100 && aim.y == 50);
    aim = scope_aim(10, 300, 64, 0, 768, 672, 224);
    CHECK(aim.offscreen);
    aim = scope_aim(64 + 767, 671, 64, 0, 768, 672, 239);
    CHECK(!aim.offscreen && aim.x == 255 && aim.y == 238);

    // Crosshair: centered on the aim, scaled for hi-res, clipped at the edges.
    std::vector<uint32_t> px(256 * 224, 0x123456);
    draw_crosshair(px.data(), 256, 224, 224, 100, 50);
    CHECK(px[50 * 256 + 100] == 0xFFFFFF && px[50 * 256 + 104] == 0xFFFFFF && px[51 * 256 + 103] == 0);
    CHECK(px[50 * 256 + 101] == 0x123456);  // the gap around the center
    std::vector<uint32_t> hires(512 * 224, 0x123456);
    draw_crosshair(hires.data(), 512, 224, 224, 100, 50);
    CHECK(hires[50 * 512 + 200] == 0xFFFFFF && hires[50 * 512 + 201] == 0xFFFFFF);
    draw_crosshair(px.data(), 256, 224, 224, 0, 0);  // no out-of-bounds writes
    CHECK(px[0] == 0xFFFFFF);

    // Host mouse: speed scaling, fractions carried over, buttons, the turbo switch.
    InputManager input;
    PointerConfig pointer;
    pointer.mouse_speed = 50;
    input.set_pointer_config(pointer);
    input.on_mouse_motion(3.f, -5.f);
    auto motion = input.take_mouse_motion();
    CHECK(motion[0] == 1 && motion[1] == -2);
    input.on_mouse_motion(1.f, -1.f);
    motion = input.take_mouse_motion();
    CHECK(motion[0] == 1 && motion[1] == -1);  // 0.5 + 0.5, -0.5 - 0.5
    input.on_mouse_button(SDL_BUTTON_LEFT, true);
    input.on_mouse_button(SDL_BUTTON_MIDDLE, true);
    CHECK(input.mouse_left() && !input.mouse_right() && input.mouse_middle());
    input.on_mouse_button(SDL_BUTTON_X1, true);
    input.on_mouse_button(SDL_BUTTON_X1, false);
    CHECK(input.scope_turbo());
    input.release_all();
    CHECK(!input.mouse_left() && input.take_mouse_motion()[0] == 0);

    // Four gamepads: pads 1-4 in connection order, each its own state (multitap players).
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
    InputManager pads;
    pads.open_connected();
    std::array<SDL_JoystickID, 4> ids{};
    for (size_t i = 0; i < ids.size(); i++) {
        ids[i] = attach_pad(static_cast<uint16_t>(0xC300 + i));
        pump(pads);
    }
    for (int p = 0; p < 4; p++) CHECK(pads.assigned(p) && pads.assigned(p)->id == ids[static_cast<size_t>(p)]);
    CHECK(pads.assigned(4) == nullptr);
    SDL_Joystick* fourth = SDL_OpenJoystick(ids[3]);
    press(fourth, SDL_GAMEPAD_BUTTON_START, true, pads);
    CHECK(pads.state(3) == kStart && pads.state(0) == 0 && pads.state(2) == 0);
    press(fourth, SDL_GAMEPAD_BUTTON_START, false, pads);
    SDL_CloseJoystick(fourth);
    pads.close_all();
    for (const auto id : ids) SDL_DetachVirtualJoystick(id);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return true;
}

auto test_settings_round_trip() -> void {
    const std::string path = (std::filesystem::temp_directory_path() / "input_tests.ini").string();
    Settings out;
    out.ports[0].device = "03000000c82d00000000000000000000";
    out.ports[0].buttons[0] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    out.ports[0].deadzone = 40;
    out.ports[1].device = std::string(kNoDevice);
    out.ports[1].stick_as_dpad = false;
    out.log.level[1] = bitforge::utils::LogLevel::Trace;
    out.log.file[1] = true;
    out.log.channel[static_cast<size_t>(bitforge::utils::LogChannel::Ppu)] = false;
    out.hotkeys.keys[0] = SDLK_R;
    out.hotkeys.pad[0] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    out.hotkeys.pad[2] = kPadNone;
    out.speed = {false, 256, 10, true, 0};
    out.port_devices = {bitforge::Device::Mouse, bitforge::Device::SuperScope};
    out.pointer.mouse_speed = 150;
    out.pointer.crosshair = false;
    out.ports[4].device = std::string(kNoDevice);
    CHECK(out.save(path));
    Settings in;
    in.load(path);
    std::filesystem::remove(path);
    CHECK(in.port_devices == out.port_devices);
    CHECK(in.pointer.mouse_speed == 150 && !in.pointer.crosshair);
    CHECK(in.ports[4].device == kNoDevice && in.ports[3].device == kAutoDevice);
    CHECK(in.ports[0].device == out.ports[0].device);
    CHECK(in.ports[0].buttons == out.ports[0].buttons);
    CHECK(in.ports[0].deadzone == 40 && in.ports[0].stick_as_dpad);
    CHECK(in.ports[1].device == kNoDevice && !in.ports[1].stick_as_dpad);
    CHECK(in.ports[1].buttons == default_button_map());
    CHECK(in.log.level[1] == bitforge::utils::LogLevel::Trace && in.log.level[0] == bitforge::utils::LogLevel::Info);
    CHECK(in.log.file[1] && in.log.file[0]);
    CHECK(!in.log.channel[static_cast<size_t>(bitforge::utils::LogChannel::Ppu)]);
    CHECK(in.log.channel[static_cast<size_t>(bitforge::utils::LogChannel::Cpu)]);
    CHECK(in.keys == default_key_map());
    CHECK(in.hotkeys.keys == out.hotkeys.keys && in.hotkeys.pad == out.hotkeys.pad);
    CHECK(in.hotkeys.keys[1] == SDLK_TAB && in.hotkeys.pad[1] == kPadTriggerRight);
    CHECK(!in.speed.rewind && in.speed.rewind_budget_mb == 256 && in.speed.rewind_interval == 10);
    CHECK(in.speed.rewind_reverse_audio && in.speed.fast_forward_speed == 0);
    Settings defaults;
    CHECK(defaults.speed.rewind && defaults.speed.fast_forward_speed == 4 && !defaults.speed.rewind_reverse_audio);
}

} // namespace

int main() {
    bitforge::utils::Logger::set_level(bitforge::utils::LogLevel::Error);
    SDL_Init(0);
    std::printf("=== Input tests ===\n\n");
    int passed = 0;
    int failed = 0;
    auto run_test = [&](const char* name, auto&& fn) {
        std::printf("  %s... ", name);
        const int before = checks_failed;
        const bool ran = fn();
        if (ran && checks_failed == before) {
            std::printf("PASSED\n");
            passed++;
        } else {
            std::printf("\n  FAILED\n");
            failed++;
        }
    };
    run_test("Left stick -> D-pad (deadzone, 8 sectors)", [] { test_stick(); return true; });
    run_test("Default SNES layout, combine, rebind", [] { test_default_layout(); return true; });
    run_test("Virtual gamepads: hotplug, ports, bind, GUID", [] { return test_virtual_pads(); });
    run_test("Rewind / fast-forward hotkeys: keys, triggers, bind", [] { return test_hotkeys(); });
    run_test("Console port devices, multitap pads, mouse, scope aim + crosshair", [] { return test_controller_devices(); });
    run_test("Pad and log settings ini round trip", [] { test_settings_round_trip(); return true; });
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    SDL_Quit();
    return failed == 0 ? 0 : 1;
}
