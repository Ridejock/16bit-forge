// SPDX-License-Identifier: MIT
#pragma once
// The run timer's UI: the Timer tab (both times, the splits with deltas and golds, the sum of
// best, attempts, the controls and the splits file), the splits editor, the "save new times?"
// prompt and the image of the game window's timer overlay. The timer itself runs on the
// emulation thread (EmuThread::Core::splits); this side shows the copy in EmuStatus and posts
// commands, each stamped with the time of the key press that asked for it.
#include "frontend/emu_thread.hpp"
#include "frontend/game_window.hpp"
#include "frontend/settings.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bitforge::frontend {

// The timer's hotkeys and buttons.
enum class TimerCommand : uint8_t { Split, Undo, Skip, Reset, Pause };
struct TimerHotkey {
    Hotkey hotkey;
    TimerCommand command;
};
inline constexpr std::array<TimerHotkey, 5> kTimerHotkeyCommands = {{
    {Hotkey::TimerSplit, TimerCommand::Split},
    {Hotkey::TimerUndo, TimerCommand::Undo},
    {Hotkey::TimerSkip, TimerCommand::Skip},
    {Hotkey::TimerReset, TimerCommand::Reset},
    {Hotkey::TimerPause, TimerCommand::Pause},
}};

// The overlay's lines for `timer` at `now`: the segment, the big timer, the delta, the other
// method's time and the race badge (a race run) or the "modified" tag. Empty while there are
// no splits.
auto timer_overlay_lines(const speedrun::RunTimer& timer, speedrun::Nanos now) -> std::vector<TextLine>;

class TimerPanel {
public:
    using Notify = std::function<void(std::string)>;

    TimerPanel(EmuThread& emu, TimerConfig& config) : emu_(emu), config_(config) {}

    // Every UI frame, with the emulation thread's latest status: reports new timer events
    // (`notify`), opens the save prompt, applies a chosen file.
    auto update(const EmuStatus& status, const Notify& notify) -> void;
    // The Timer tab's contents. True when a setting changed (to be saved).
    auto draw(const EmuStatus& status, SDL_Window* window) -> bool;
    // The splits editor window and the save prompt: call once per frame, outside any tab.
    auto draw_windows(const EmuStatus& status) -> void;
    // A hotkey or button; `when_ns` is the press's time (SDL_GetTicksNS clock, 0 = now).
    auto command(TimerCommand command, uint64_t when_ns = 0) -> void;
    // The game window's overlay image for now (empty when it's off or there is no run).
    auto overlay_image(const EmuStatus& status, int& width, int& height) const -> std::vector<uint32_t>;

private:
    enum class Dialog : uint8_t { Open, New, Export };
    struct RuleEdit {
        int watch = -1;  // index into watches_, -1 = no rule
        int test = 0;    // index into debug::kTests
        std::array<char, 32> value{};
    };
    struct Editor {
        bool open = false;
        speedrun::Run run;  // as it was when the editor opened; segments follow the edits
        std::array<char, 128> game{};
        std::array<char, 128> category{};
        std::vector<std::array<char, 64>> names;
        RuleEdit start;
        RuleEdit reset;
        std::vector<RuleEdit> splits;
        std::string error;
    };

    auto sync_watches(const EmuStatus& status) -> void;
    auto draw_times(const speedrun::RunTimer& timer, speedrun::Nanos now) -> void;
    auto draw_splits(const speedrun::RunTimer& timer, speedrun::Nanos now) -> void;
    auto draw_controls(const EmuStatus& status) -> bool;
    auto draw_file(const EmuStatus& status, SDL_Window* window) -> void;
    auto draw_editor() -> void;
    auto draw_rule(const char* id, RuleEdit& rule) -> void;
    auto draw_prompt(const EmuStatus& status) -> void;
    auto open_editor(const speedrun::Run& run) -> void;
    auto rule_edit(const std::optional<speedrun::Rule>& rule) const -> RuleEdit;
    auto make_rule(const RuleEdit& edit, std::string& error) const -> std::optional<speedrun::Rule>;
    auto apply_editor() -> bool;
    auto show_dialog(Dialog dialog, SDL_Window* window, const std::string& start) -> void;
    static void SDLCALL on_dialog(void* userdata, const char* const* files, int filter);

    EmuThread& emu_;
    TimerConfig& config_;
    std::string rom_;  // the game watches_ belong to
    uint64_t watches_revision_ = 0;
    std::vector<debug::Watch> watches_;
    uint64_t seen_serial_ = 0;   // the last timer event reported
    std::string last_auto_;      // what the last auto rule did, for the tab
    bool prompt_open_ = false;
    bool prompt_answered_ = false;  // posted; waiting for the timer to clear pending()
    Editor editor_;

    // SDL may call the file dialog's callback on another thread.
    std::mutex dialog_mutex_;
    std::optional<Dialog> dialog_open_;  // waiting for this one
    std::optional<std::string> dialog_path_;
};

} // namespace bitforge::frontend
