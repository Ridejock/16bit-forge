// SPDX-License-Identifier: MIT
#pragma once
#include "frontend/audio.hpp"
#include "frontend/emu_thread.hpp"
#include "frontend/emulator.hpp"
#include "frontend/game_window.hpp"
#include "frontend/input.hpp"
#include "frontend/settings.hpp"
#include "utils/log_view.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bitforge::frontend {

struct LaunchOptions {
    std::string rom;               // boot this ROM on start, if set
    uint64_t exit_after_frames = 0;  // 0 = run until the user quits (used by headless smoke tests)
    std::optional<video::BackendKind> renderer;  // overrides (and saves) View > Renderer
    std::string filter;                          // overrides (and saves) View > Filter
    std::string screenshot;                      // with exit_after_frames: save the game window here (BMP)
    std::string record_audio;                    // record core + device audio to <stem>_core.wav / <stem>_device.wav
};

// PCSX2-style shell: an ImGui main window (menus, library, debug panels, status bar)
// plus a separate game window that only shows the emulated picture. This is the UI thread
// (SDL events, both windows, ImGui); emulation and audio run on the EmuThread.
class App {
public:
    App() = default;
    App(const App&) = delete;
    auto operator=(const App&) -> App& = delete;
    ~App();

    auto init() -> bool;
    auto run(const LaunchOptions& options) -> int;

private:
    // The UI loop waits at most this long for the next emulated frame before drawing anyway.
    static constexpr uint64_t kUiWaitNs = EmuThread::kFrameNs;

    auto ui_frame() -> void;
    auto finish_smoke_run() -> void;

    // Lifecycle
    auto boot(const std::string& path) -> void;
    auto stop() -> void;
    auto shutdown() -> void;

    // Events, routed by SDL window id
    auto handle_event(const SDL_Event& event) -> void;
    auto handle_game_event(const SDL_Event& event) -> void;

    // Main-window UI
    auto draw_ui() -> void;
    auto draw_menu_bar() -> void;
    auto draw_library() -> void;
    auto draw_controls() -> void;
    auto draw_port_controls(int port) -> void;
    auto draw_console_ports() -> void;
    auto send_input() -> void;                       // pads, mouse and scope to the emulation thread
    auto present_game(const FrameImage& image) -> void;  // with the Super Scope crosshair
    auto set_mouse_captured(bool on) -> void;
    auto uses(bitforge::Device device) const -> bool;
    auto draw_log_panel() -> void;
    auto draw_state_menu(bool save) -> void;
    auto toggle_pause() -> void;
    auto refresh_slots() -> void;
    auto save_slot(int slot) -> void;
    auto load_slot(int slot) -> void;
    auto state_hotkey(SDL_Keycode key, SDL_Keymod mod) -> bool;  // F1-F10 load, Shift+F1-F10 save
    auto set_status(std::string text) -> void;
    // set_status, plus an on-screen message in the game window (if View > On-screen messages).
    auto notify(std::string text) -> void;
    auto apply_log_settings() -> void;
    auto sync_input_settings() -> void;
    auto draw_status_bar() -> void;
    auto timeline_hint_height() -> float;
    auto timeline_block_height() -> float;
    auto sync_timeline() -> void;
    auto draw_timeline() -> void;
    auto clear_timeline() -> void;
    auto play_from(uint64_t id) -> void;
    auto save_moment(uint64_t id, int slot) -> void;
    auto draw_video_menus() -> void;
    auto draw_audio_menu() -> void;
    auto draw_audio_panel() -> void;
    auto draw_speed_panel() -> void;
    auto draw_cheats_panel() -> void;
    auto apply_cheats() -> void;
    auto apply_speed_settings() -> void;
    auto draw_hotkey_rows() -> void;
    auto set_core_recording(bool on, const std::string& path = {}) -> void;
    auto set_device_recording(bool on, const std::string& path = {}) -> void;
    auto recording_path(const char* kind) const -> std::string;
    auto request_stop() -> void;
    auto request_quit() -> void;

    auto open_rom_dialog() -> void;
    auto scan_roms() -> void;
    auto place_game_window() -> void;
    auto view_options() const -> ViewOptions;
    auto current_filter() const -> const video::FilterPreset&;
    auto apply_view() -> void;
    auto update_game_title() -> void;
    auto save_settings() -> void;

    static void SDLCALL on_file_chosen(void* userdata, const char* const* files, int filter);

    SDL_Window* main_window_ = nullptr;
    SDL_Renderer* main_renderer_ = nullptr;
    bool imgui_ready_ = false;

    GameWindow game_;
    EmuThread emu_thread_;  // owns the Emulator and drives the AudioOutput
    EmuStatus status_;      // its last published status, refreshed every UI frame
    InputManager input_;
    Settings settings_;
    std::vector<video::FilterPreset> filters_;  // from shaders/presets.ini
    utils::LogView system_log_{utils::LogSink::System};
    utils::LogView emu_log_{utils::LogSink::Emulation};

    // File > Save/Load State: one thumbnail texture (on the main renderer) per slot, reloaded
    // when the slot file changes.
    struct SlotView {
        SDL_Texture* texture = nullptr;
        std::string path;
        std::filesystem::file_time_type mtime{};
        std::string when;  // local time it was saved, empty if the slot is empty
        int64_t timestamp = 0;
        uint64_t frame = 0;
        uint32_t version = 0;
        int thumb_w = 0;
        int thumb_h = 0;
    };
    std::array<SlotView, Emulator::kStateSlots> slots_{};
    // Rewind timeline: one texture per snapshot still in the history, oldest first.
    // Ids come from the rewind buffer and are never reused, so a texture stays with its moment.
    struct TimelineView {
        uint64_t id = 0;
        uint64_t frame = 0;
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
    };
    std::vector<TimelineView> timeline_{};
    std::string status_text_;  // shown in the status bar for a few seconds
    uint64_t status_until_ns_ = 0;

    std::string base_dir_;
    std::string settings_path_;
    std::string roms_dir_;
    std::vector<std::string> library_;
    int library_selected_ = -1;

    bool quit_ = false;
    bool stop_requested_ = false;
    bool show_controls_ = false;
    bool show_audio_ = false;
    bool show_speed_ = false;
    bool show_cheats_ = false;
    // The Cheats tab's copy of the running game's list (the emulation thread owns the real one).
    std::vector<bitforge::CheatEntry> cheats_;
    std::string cheats_rom_;
    std::array<char, 128> cheat_code_{};
    std::array<char, 128> cheat_description_{};
    std::string cheat_error_;
    // While waiting for a key: index into kPadButtons, or kPadButtons.size() + hotkey; else -1
    // (gamepad binds live in input_).
    int binding_button_ = -1;
    bool shown_pattern_title_ = false;
    // Host mouse over the game window, for the SNES Mouse (captured, relative) and the Super
    // Scope (absolute position, in window pixels).
    bool mouse_captured_ = false;
    bool pointer_inside_ = false;
    float pointer_x_ = 0.f;
    float pointer_y_ = 0.f;
    bitforge::ScopeInput scope_aim_;
    std::vector<uint32_t> overlay_;  // the frame with the crosshair drawn in
    std::string screenshot_path_;  // LaunchOptions: saved when the exit-after-frames count is reached

    // SDL may call the file-dialog callback on another thread.
    std::mutex dialog_mutex_;
    std::optional<std::string> dialog_result_;
};

} // namespace bitforge::frontend
