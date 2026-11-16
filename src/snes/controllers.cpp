// SPDX-License-Identifier: MIT
#include "snes/controllers.hpp"

#include <algorithm>
#include <cstdlib>

namespace snes {

auto device_info(Device device) -> const DeviceInfo& {
    for (const auto& d : kDevices) {
        if (d.device == device) return d;
    }
    return kDevices[0];
}

auto ControllerPort::set_device(Device device) -> void {
    if (device == device_) return;
    const auto pads = pads_;
    *this = ControllerPort{};
    device_ = device;
    pads_ = pads;
}

auto ControllerPort::set_pad(int slot, Word buttons) -> void {
    if (slot >= 0 && slot < kMultitapSlots) pads_[static_cast<size_t>(slot)] = buttons;
}

auto ControllerPort::pad(int slot) const -> Word {
    return slot >= 0 && slot < kMultitapSlots ? pads_[static_cast<size_t>(slot)] : 0;
}

auto ControllerPort::add_mouse(int dx, int dy, bool left, bool right) -> void {
    constexpr int kLimit = 1 << 16;  // stop runaway accumulation while the game doesn't poll
    mouse_dx_ = std::clamp(mouse_dx_ + dx, -kLimit, kLimit);
    mouse_dy_ = std::clamp(mouse_dy_ + dy, -kLimit, kLimit);
    left_ = left;
    right_ = right;
}

auto ControllerPort::latch(bool high) -> void {
    switch (device_) {
    case Device::Gamepad:
    case Device::Multitap:
        if (high) shift_ = pads_;  // reloads continuously while high
        break;
    case Device::Mouse:
        if (high && !latched_) {
            // Sensitivity: low, medium, high scale the motion by 1, 1.5 and 2.
            static constexpr std::array<int, 3> kScale = {2, 3, 4};
            report_dx_ = std::clamp(mouse_dx_ * kScale[speed_] / 2, -127, 127);
            report_dy_ = std::clamp(mouse_dy_ * kScale[speed_] / 2, -127, 127);
            mouse_dx_ = mouse_dy_ = 0;
        }
        if (high != latched_) counter_ = 0;
        break;
    case Device::SuperScope:
        if (high && !latched_) {
            const bool fire = scope_.fire && (scope_.turbo || !fire_held_);
            const bool pause = scope_.pause && !pause_held_;
            fire_held_ = scope_.fire;
            pause_held_ = scope_.pause;
            scope_report_ = static_cast<Byte>((fire ? 0x80 : 0) | (scope_.cursor ? 0x40 : 0) | (scope_.turbo ? 0x20 : 0) |
                                              (pause ? 0x10 : 0) | (scope_.offscreen ? 0x02 : 0));
        }
        if (high != latched_) counter_ = 0;
        break;
    case Device::None:
        break;
    }
    latched_ = high;
}

auto ControllerPort::mouse_bit(int index) const -> bool {
    const auto magnitude = [](int v) { return std::abs(v); };
    if (index < 8) return false;
    if (index < 16) {
        switch (index) {
        case 8: return right_;
        case 9: return left_;
        case 10: return (speed_ >> 1) & 1;
        case 11: return speed_ & 1;
        case 15: return true;  // signature 0001
        default: return false;
        }
    }
    if (index == 16) return report_dy_ < 0;
    if (index < 24) return (magnitude(report_dy_) >> (23 - index)) & 1;
    if (index == 24) return report_dx_ < 0;
    if (index < 32) return (magnitude(report_dx_) >> (31 - index)) & 1;
    return true;
}

auto ControllerPort::clock(bool iobit) -> Byte {
    const auto shift_out = [this](size_t slot) -> Byte {
        if (latched_) return static_cast<Byte>(pads_[slot] >> 15);  // the live B button
        const auto bit = static_cast<Byte>(shift_[slot] >> 15);
        shift_[slot] = static_cast<Word>((shift_[slot] << 1) | 1);  // 1s after the report
        return bit;
    };
    switch (device_) {
    case Device::None: return 0;
    case Device::Gamepad: return shift_out(0);
    case Device::Multitap: {
        if (latched_) return 0x02;
        const size_t first = iobit ? 0 : 2;
        const Byte d0 = shift_out(first);
        return static_cast<Byte>(d0 | (shift_out(first + 1) << 1));
    }
    case Device::Mouse:
        if (latched_) {
            speed_ = static_cast<Byte>((speed_ + 1) % 3);
            return 0;
        }
        return mouse_bit(counter_ < 32 ? counter_++ : 32) ? 1 : 0;
    case Device::SuperScope:
        if (latched_) return 0;
        if (counter_ >= 8) return 1;
        return (scope_report_ >> (7 - counter_++)) & 1;
    }
    return 0;
}

auto ControllerPort::auto_read(bool iobit, bool latch_after) -> std::array<Word, 2> {
    latch(true);
    latch(false);
    std::array<Word, 2> words{};
    for (int i = 0; i < 16; i++) {
        const Byte bits = clock(iobit);
        words[0] = static_cast<Word>((words[0] << 1) | (bits & 1));
        words[1] = static_cast<Word>((words[1] << 1) | ((bits >> 1) & 1));
    }
    if (latch_after) latch(true);
    return words;
}

auto ControllerPort::serialize(Archive& ar) -> void {
    ar(device_, latched_, pads_, shift_, counter_, mouse_dx_, mouse_dy_, report_dx_, report_dy_, left_, right_, speed_);
    ar(scope_.x, scope_.y, scope_.offscreen, scope_.fire, scope_.cursor, scope_.turbo, scope_.pause, scope_report_,
       fire_held_, pause_held_);
}

} // namespace snes
