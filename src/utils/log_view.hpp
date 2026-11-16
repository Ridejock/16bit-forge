// SPDX-License-Identifier: MIT
#pragma once
#include "utils/logging.hpp"

#include "imgui.h"

namespace snes::utils {

// One log panel tab: the ring of one sink (System or Emulation) with its toolbar (level,
// channel checkboxes, pause, clear, search, copy, log file) inside the current ImGui window.
class LogView {
public:
    explicit LogView(LogSink sink) : sink_(sink) {}

    // Call between ImGui::NewFrame and ImGui::Render. Returns true when a persistent setting
    // (level, channel, file toggle) changed, so the owner can save them.
    auto draw() -> bool;

    // Whether this sink writes its log file; the owner opens or closes it.
    bool file_enabled = false;

private:
    LogSink sink_;
    ImGuiTextFilter filter_;
    bool follow_ = true;
};

} // namespace snes::utils
