// SPDX-License-Identifier: MIT
#pragma once
#include "snes/serialize.hpp"
#include "snes/types.hpp"

#include <array>
#include <string_view>

namespace bitforge {

// What is plugged into a controller port.
enum class Device : uint8_t { None, Gamepad, Multitap, Mouse, SuperScope };

struct DeviceInfo {
    Device device;
    std::string_view id;    // settings value
    std::string_view name;  // UI label
};
inline constexpr std::array<DeviceInfo, 5> kDevices = {{
    {Device::None, "none", "Nothing"},
    {Device::Gamepad, "gamepad", "Gamepad"},
    {Device::Multitap, "multitap", "Multitap (4 pads)"},
    {Device::Mouse, "mouse", "SNES Mouse"},
    {Device::SuperScope, "superscope", "Super Scope"},
}};
auto device_info(Device device) -> const DeviceInfo&;

inline constexpr int kMultitapSlots = 4;

// Super Scope input. `x`/`y` are picture pixels (0-255, 0-238); `turbo` is the switch position.
struct ScopeInput {
    int x = 0;
    int y = 0;
    bool offscreen = true;
    bool fire = false;
    bool cursor = false;
    bool turbo = false;
    bool pause = false;
};

// The Super Scope sees the beam pass its aim point and pulls the port's I/O line, which latches
// the PPU's H/V counters ($213C/$213D, STAT78 bit 6) when $4201 bit 7 allows it. This is the
// latched H position: picture pixel x plus the dots before the first visible pixel.
inline constexpr int kScopeDotOffset = 22;

// One controller port and the device in it, seen from the serial side. $4016 bit 0 drives the
// latch line of both ports; each $4016/$4017 read clocks the port once and returns data lines
// D0 (bit 0) and D1 (bit 1). Reports, most significant bit first:
//   Gamepad     B Y Sel St U D L R A X L R 0 0 0 0, then 1s. D1 = 0.
//   Multitap    with the port's I/O bit high, pads 1 and 2 on D0 and D1; low, pads 3 and 4.
//               While latched, D1 reads 1 (how games detect it).
//   Mouse       0x00, R L s1 s0 0 0 0 1, then Y and X as sign + 7-bit magnitude (sign 1 =
//               up/left), then 1s. Clocking it while latched steps the sensitivity 0-1-2.
//   Super Scope Fire Cursor Turbo Pause 0 0 Offscreen 0, then 1s. Without turbo, Fire and Pause
//               report once per press.
//   None        0 on both lines (no trailing 1s, so games see an empty port).
class ControllerPort {
public:
    auto device() const -> Device { return device_; }
    auto set_device(Device device) -> void;  // a different device starts from a clean protocol state

    // Input from the host. A gamepad uses slot 0; the multitap slots 0-3.
    auto set_pad(int slot, Word buttons) -> void;
    auto pad(int slot) const -> Word;
    // The pad report left to read (for the version-4 save-state layout).
    auto shift(int slot) const -> Word { return shift_[static_cast<size_t>(slot & 3)]; }
    auto set_shift(int slot, Word value) -> void { shift_[static_cast<size_t>(slot & 3)] = value; }
    // Mouse motion accumulates until the next latch; buttons are the current state.
    auto add_mouse(int dx, int dy, bool left, bool right) -> void;
    auto mouse_speed() const -> int { return speed_; }
    auto set_scope(const ScopeInput& input) -> void { scope_ = input; }
    auto scope() const -> const ScopeInput& { return scope_; }

    auto latch(bool high) -> void;
    auto clock(bool iobit) -> Byte;
    // Auto-joypad read: a latch pulse and 16 clocks; returns the D0 and D1 words.
    auto auto_read(bool iobit, bool latch_after) -> std::array<Word, 2>;

    auto serialize(Archive& ar) -> void;

private:
    auto mouse_bit(int index) const -> bool;

    Device device_ = Device::Gamepad;
    bool latched_ = false;
    std::array<Word, kMultitapSlots> pads_{};
    std::array<Word, kMultitapSlots> shift_{};
    int counter_ = 0;  // mouse / scope report position
    // Mouse
    int mouse_dx_ = 0, mouse_dy_ = 0;   // since the last latch
    int report_dx_ = 0, report_dy_ = 0; // latched for the report
    bool left_ = false, right_ = false;
    Byte speed_ = 0;
    // Super Scope
    ScopeInput scope_;
    Byte scope_report_ = 0;
    bool fire_held_ = false, pause_held_ = false;
};

} // namespace bitforge
