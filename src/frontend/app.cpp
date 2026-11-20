// SPDX-License-Identifier: MIT
#include "frontend/app.hpp"
#include "frontend/app_icon.hpp"
#include "snes/savestate.hpp"
#include "speedrun/race.hpp"
#include "utils/logging.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace bitforge::frontend {

namespace {

// Name, version and settings file come from CMake's project() (CMakeLists.txt).
constexpr const char* kMainTitle = BITFORGE_TITLE;
constexpr const char* kSettingsFile = BITFORGE_EXE_NAME ".ini";
constexpr const char* kOldSettingsFile = "snes_emulator.ini";  // name before the 16bit Forge rename
constexpr const char* kRomsFolder = "roms";
constexpr const char* kPresetsFile = "shaders/presets.ini";
// Log files next to the executable, one per sink. Trace output can run to hundreds of MB a
// minute, so each file is capped: at the cap it becomes <name>.1.log and a new one starts.
struct LogFile {
    const char* path;
    size_t max_bytes;
};
constexpr std::array<LogFile, utils::kLogSinkCount> kLogFiles = {{
    {"logs/system.log", 4u << 20},
    {"logs/emu_trace.log", 64u << 20},
}};
constexpr uint64_t kStatusNs = 4'000'000'000ULL;  // how long a status-bar message stays
constexpr float kThumbHeight = 56.f;              // slot thumbnails in the state menus (before DPI scale)
constexpr int kMainWidth = 960;
constexpr int kMainHeight = 640;
constexpr int kMainMinWidth = 480;
constexpr int kMainMinHeight = 320;
constexpr int kWindowGap = 16;           // space between the main and game windows
constexpr float kLibraryShare = 0.45f;   // library height share when a debug panel is open
constexpr double kFrameBudgetMs = 1000.0 / Emulator::kNtscFrameRate;
constexpr float kClearGray = 24.f / 255.f;
constexpr const char* kRacePromptOn = "Turn on race mode?";
constexpr const char* kRacePromptOff = "Turn off race mode?";
constexpr float kRacePromptWidthEm = 30.f;
constexpr const char* kRaceTimelineHint = "Off in race mode: no snapshots are kept until you turn it off (Speedrun > Race mode).";

constexpr SDL_DialogFileFilter kRomFilters[] = {
    {"Game ROMs", "sfc;smc"},
    {"All files", "*"},
};

auto display_scale() -> float {
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    return scale < 1.f ? 1.f : scale;
}

auto is_rom_file(const fs::path& p) -> bool {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".sfc" || ext == ".smc";
}

auto local_time(int64_t seconds, const char* format) -> std::string {
    const auto t = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[64];
    std::strftime(text, sizeof(text), format, &local);
    return text;
}

auto state_name(bool loaded, bool paused) -> const char* {
    if (!loaded) return "Stopped";
    return paused ? "Paused" : "Running";
}

} // namespace

App::~App() { shutdown(); }

auto App::init() -> bool {
    // Pads keep working while the main window has focus (e.g. with the Controls tab open).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        utils::Logger::error("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    const char* base = SDL_GetBasePath();
    base_dir_ = base ? base : "";
    settings_path_ = (fs::path(base_dir_) / kSettingsFile).string();
    roms_dir_ = (fs::path(base_dir_) / kRomsFolder).string();
    // One-time carry-over: until the new file has been written, read the pre-rename one.
    const fs::path old_settings = fs::path(base_dir_) / kOldSettingsFile;
    std::error_code ec;
    const bool use_old = !fs::exists(settings_path_, ec) && fs::exists(old_settings, ec);
    settings_.load(use_old ? old_settings.string() : settings_path_);
    apply_log_settings();
    utils::Logger::info("%s %s - %s", BITFORGE_TITLE, BITFORGE_VERSION, BITFORGE_TAGLINE);
    if (use_old) utils::Logger::info("Settings read from %s; they are saved to %s from now on", kOldSettingsFile, kSettingsFile);
    // The emulation thread isn't running yet: set up what it will own directly.
    EmuThread::Core& core = emu_thread_.core();
    core.emu.set_data_dir(base_dir_);

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        utils::Logger::warning(utils::LogChannel::Input, "Gamepads unavailable: %s", SDL_GetError());
    }
    input_.set_key_map(settings_.keys);
    input_.set_hotkeys(settings_.hotkeys);
    core.speed = settings_.speed;
    core.apply_speed();
    core.splits.timer.set_method(settings_.timer.primary);
    core.verify.emulator = running_version();
    core.verify.write = settings_.race.write_verification;
    core.set_race(settings_.race.enabled, EmuThread::timer_clock());  // no game yet: nothing restarts
    core.movie_end = settings_.movie.at_end;
    core.attempt_movies = settings_.movie.attempt_movies;
    core.emu.set_movie_read_only(settings_.movie.read_only);
    core.emu.set_movie_flush_frames(static_cast<uint32_t>(settings_.movie.flush_frames));
    find_unfinished_movies();
    for (int p = 0; p < kPadCount; p++) input_.set_port(p, settings_.ports[static_cast<size_t>(p)]);
    input_.set_pointer_config(settings_.pointer);
    input_.open_connected();
    filters_ = video::load_presets((fs::path(base_dir_) / kPresetsFile).string());
    core.audio.open(Emulator::audio_sample_rate());  // optional: runs silent without a device
    core.audio.set_volume(settings_.volume);
    core.audio.set_msu1_volume(settings_.msu1_volume);
    core.audio.set_muted(settings_.muted);
    core.audio.set_rate_control(!settings_.audio_sync);
    core.audio_sync = settings_.audio_sync;
#ifndef NDEBUG
    utils::Logger::warning("This is a Debug build: emulation may not keep up with 60 fps, and audio will crackle when it "
                           "doesn't. Build Release or RelWithDebInfo to play.");
#endif

    const float scale = display_scale();
    main_window_ = SDL_CreateWindow(kMainTitle, static_cast<int>(kMainWidth * scale), static_cast<int>(kMainHeight * scale),
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!main_window_) {
        utils::Logger::error("Main window creation failed: %s", SDL_GetError());
        return false;
    }
    set_app_icon(main_window_);
    SDL_SetWindowMinimumSize(main_window_, kMainMinWidth, kMainMinHeight);

    main_renderer_ = SDL_CreateRenderer(main_window_, nullptr);
    if (!main_renderer_) {
        utils::Logger::error("Main renderer creation failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(main_renderer_, 0);  // App::run paces both windows (by the emulated frames).

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;

    if (!ImGui_ImplSDL3_InitForSDLRenderer(main_window_, main_renderer_)) return false;
    if (!ImGui_ImplSDLRenderer3_Init(main_renderer_)) {
        ImGui_ImplSDL3_Shutdown();
        return false;
    }
    imgui_ready_ = true;

    scan_roms();
    utils::Logger::info("Ready. ROM folder: %s", roms_dir_.c_str());
    return true;
}

auto App::shutdown() -> void {
    // The emulation thread goes first (it finishes queued commands), before any SDL teardown;
    // after the join its core is ours again: the battery save is written here.
    emu_thread_.stop();
    EmuThread::Core& core = emu_thread_.core();
    core.save_splits();
    core.audio.end_session();
    core.emu.unload();  // writes battery RAM
    input_.close_all();
    for (auto& slot : slots_) {
        if (slot.texture) SDL_DestroyTexture(slot.texture);
        slot = SlotView{};
    }
    clear_timeline();
    game_.close();
    set_core_recording(false);  // logs where the files went
    set_device_recording(false);
    core.audio.close();
    if (imgui_ready_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        imgui_ready_ = false;
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (main_renderer_) SDL_DestroyRenderer(main_renderer_);
    if (main_window_) SDL_DestroyWindow(main_window_);
    main_renderer_ = nullptr;
    main_window_ = nullptr;
    SDL_Quit();
}

auto App::run(const LaunchOptions& options) -> int {
    if (options.renderer) settings_.renderer = *options.renderer;
    if (!options.filter.empty()) settings_.filter = options.filter;
    if (!options.rom.empty()) boot(options.rom);
    if (!options.record_audio.empty()) {
        const fs::path p(options.record_audio);
        const fs::path stem = p.parent_path() / p.stem();
        set_core_recording(true, stem.string() + "_core.wav");
        set_device_recording(true, stem.string() + "_device.wav");
    }
    screenshot_path_ = options.screenshot;
    emu_thread_.set_exit_after_frames(options.exit_after_frames);
    emu_thread_.start();

    while (!quit_) {
        // Wake for each emulated frame (or after a frame period when none comes: no ROM, paused),
        // so the game window shows every frame as it's made.
        emu_thread_.wait_frame(kUiWaitNs);
        ui_frame();
    }
    emu_thread_.stop();

    save_settings();
    return 0;
}

// One pass of the UI loop: events (and input to the emulation thread), pending boot/stop, the
// game window (the newest emulated frame) and the ImGui main window. Inside a Windows modal
// move/size loop this is blocked in SDL_PollEvent: the windows stop updating meanwhile, but
// emulation and audio carry on in the emulation thread.
auto App::ui_frame() -> void {
    SDL_Event event;
    while (SDL_PollEvent(&event)) handle_event(event);
    send_input();
    // Race mode: the emulation thread ignores rewind and fast-forward too; this says why.
    const bool rewind = input_.hotkey_held(Hotkey::Rewind) && !race_blocks(speedrun::Guard::Rewind);
    const bool fast = input_.hotkey_held(Hotkey::FastForward) && !race_blocks(speedrun::Guard::FastForward);
    emu_thread_.set_speed_input(rewind, fast);
    if (input_.take_hotkey_press(Hotkey::FastForwardToggle) && !race_blocks(speedrun::Guard::FastForward)) {
        emu_thread_.post([](EmuThread::Core& c) { c.fast_forward_toggled = !c.fast_forward_toggled; });
    }
    if (input_.take_hotkey_press(Hotkey::MemoryWatch)) toggle_memory_panel();
    if (input_.take_hotkey_press(Hotkey::RaceMode)) request_race(!status_.race);
    for (const auto& [hotkey, command] : kTimerHotkeyCommands) {
        uint64_t when = 0;
        if (!input_.take_hotkey_press(hotkey, &when)) continue;
        if (command == TimerCommand::Pause && race_blocks(speedrun::Guard::TimerPause)) continue;
        timer_panel_.command(command, when);
    }
    {
        std::unique_lock lock(dialog_mutex_);
        if (dialog_result_) {
            std::string path = std::move(*dialog_result_);
            dialog_result_.reset();
            lock.unlock();
            boot(path);
        }
    }
    take_movie_dialog();
    if (stop_requested_) {
        stop_requested_ = false;
        stop();
    }
    status_ = emu_thread_.status();
    timer_panel_.update(status_, [this](std::string text) { notify(std::move(text)); });
    watch_movie();

    if (game_.is_open()) {
        emu_thread_.take_frame();
        present_game(emu_thread_.frame());
        if (shown_pattern_title_ != status_.test_pattern) update_game_title();
    }
    draw_ui();

    if (status_.exit_reached && !quit_) finish_smoke_run();
}

// --exit-after-frames: the emulation thread holds at that frame; report and (optionally) save a
// screenshot of it.
auto App::finish_smoke_run() -> void {
    utils::Logger::info("Exit after %llu frames (fps %.2f, test pattern: %s, video: %s, filter: %s)",
                        static_cast<unsigned long long>(status_.frame_count), status_.fps, status_.test_pattern ? "yes" : "no",
                        game_.backend_description().c_str(), current_filter().name.c_str());
    const AudioOutput::Stats& a = status_.audio;
    utils::Logger::info(utils::LogChannel::Audio, "Audio: %s, queue avg %.1f ms (target %.0f), rate %+.3f%%, underruns %llu (%.0f ms silence), "
                        "overflows %llu, emu frame avg %.2f ms",
                        a.open ? "open" : "off", a.level_ms, a.target_ms, (a.ratio - 1.0) * 100.0,
                        static_cast<unsigned long long>(a.underruns), a.silence_ms,
                        static_cast<unsigned long long>(a.overflows), status_.frame_ms_avg);
    if (!screenshot_path_.empty() && game_.is_open()) {
        emu_thread_.take_frame();  // the held frame
        const FrameImage& image = emu_thread_.frame();
        game_.request_screenshot(screenshot_path_);
        present_game(image);
    }
    quit_ = true;
}

// ---- lifecycle -------------------------------------------------------------

auto App::boot(const std::string& path) -> void {
    // The load runs on the emulation thread with the output paused first (a game that's still
    // running stops queueing audio, so nothing drains into an underrun meanwhile). The new game
    // isn't started until its window exists.
    struct Loaded {
        bool ok;
        std::string message;  // the error, or a warning (a chip that isn't emulated)
    };
    const Loaded loaded = emu_thread_.call([&path](EmuThread::Core& c) -> Loaded {
        c.audio.end_session();
        if (!c.emu.load(path)) return {false, c.emu.last_error()};
        c.state = RunState::Stopped;  // loaded, not running until the game window is up
        c.republish = true;
        return {true, c.emu.last_warning()};
    });
    if (!loaded.ok) {
        // Missing coprocessor firmware and the like: say which file goes where. A game that was
        // running carries on (the emulation thread resumes its output).
        set_status(loaded.message);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Can't start the game", loaded.message.c_str(),
                                 game_.is_open() ? game_.window() : nullptr);
        return;
    }
    if (!loaded.message.empty()) set_status(loaded.message);

    const bool reused = game_.is_open();
    if (!reused && !game_.open(kMainTitle, view_options(), settings_.renderer, current_filter())) {
        utils::Logger::error(utils::LogChannel::Video, "Game window creation failed: %s", SDL_GetError());
        emu_thread_.call([](EmuThread::Core& c) { c.emu.unload(); });
        status_ = emu_thread_.status();
        return;
    }
    if (!reused) place_game_window();
    input_.release_all();
    emu_thread_.call([](EmuThread::Core& c) {
        c.state = RunState::Running;
        c.advance_one = false;
        c.audio.begin_session();  // per-game underrun counters; the device starts once frames flow
    });
    status_ = emu_thread_.status();
    update_game_title();
    SDL_RaiseWindow(game_.window());

    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    settings_.add_recent(ec ? path : absolute.lexically_normal().string());
    save_settings();
}

auto App::stop() -> void {
    if (!status_.loaded && !game_.is_open()) return;
    emu_thread_.call([](EmuThread::Core& c) {
        c.audio.end_session();  // nothing plays or counts through the battery save
        c.emu.unload();
        c.state = RunState::Stopped;
    });
    status_ = emu_thread_.status();
    game_.close();
    input_.release_all();
    utils::Logger::info("Emulation stopped");
}

// ---- events ----------------------------------------------------------------

// Handled after the event loop (the game window must not go away while its events are handled).
auto App::request_stop() -> void { stop_requested_ = true; }

auto App::request_quit() -> void { quit_ = true; }

auto App::handle_event(const SDL_Event& event) -> void {
    if (event.type == SDL_EVENT_QUIT) {
        request_quit();
        return;
    }

    // Gamepads aren't tied to a window. ImGui sees them too (it tracks connected pads).
    bool bound = false;
    if (input_.handle_event(event, &bound)) {
        if (bound) sync_input_settings();
        ImGui_ImplSDL3_ProcessEvent(&event);
        return;
    }

    SDL_Window* target = SDL_GetWindowFromEvent(&event);
    if (target && game_.is_open() && target == game_.window()) {
        handle_game_event(event);
        return;
    }

    // Everything else belongs to the main window (and its single ImGui context).
    if (binding_button_ >= 0 && event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key != SDLK_ESCAPE) {
            const auto index = static_cast<size_t>(binding_button_);
            if (index < kPadButtons.size()) {
                settings_.keys[index] = event.key.key;
                input_.set_key_map(settings_.keys);
            } else {
                settings_.hotkeys.keys[index - kPadButtons.size()] = event.key.key;
                input_.set_hotkeys(settings_.hotkeys);
            }
            save_settings();
        }
        binding_button_ = -1;
        return;
    }
    if (input_.binding_active() && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        input_.cancel_binding();
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && state_hotkey(event.key.key, event.key.mod)) return;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !ImGui::GetIO().WantTextInput &&
        timer_hotkey(event.key.key, event.key.timestamp)) {
        return;
    }
    // The memory watch and race mode hotkeys work here too, unless a text field is taking the key.
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !ImGui::GetIO().WantTextInput && event.key.key != SDLK_UNKNOWN) {
        if (event.key.key == settings_.hotkeys.keys[static_cast<size_t>(Hotkey::MemoryWatch)]) {
            toggle_memory_panel();
            return;
        }
        if (event.key.key == settings_.hotkeys.keys[static_cast<size_t>(Hotkey::RaceMode)]) {
            request_race(!status_.race);
            return;
        }
    }
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && target == main_window_) request_quit();
}

auto App::handle_game_event(const SDL_Event& event) -> void {
    switch (event.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        request_stop();  // closing the game window stops emulation; the main window stays
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_.release_keys();  // gamepads keep working in the background
        set_mouse_captured(false);
        break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        pointer_inside_ = event.type == SDL_EVENT_WINDOW_MOUSE_ENTER;
        if (uses(bitforge::Device::SuperScope) && pointer_inside_) SDL_HideCursor();
        else if (!mouse_captured_) SDL_ShowCursor();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        pointer_inside_ = true;
        pointer_x_ = event.motion.x;
        pointer_y_ = event.motion.y;
        if (mouse_captured_) input_.on_mouse_motion(event.motion.xrel, event.motion.yrel);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        // The first click on the picture captures the mouse for a SNES Mouse (Esc releases it).
        if (down && uses(bitforge::Device::Mouse) && !mouse_captured_) {
            set_mouse_captured(true);
            break;
        }
        input_.on_mouse_button(event.button.button, down);
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) break;
        if (event.key.key == SDLK_ESCAPE && mouse_captured_) {
            set_mouse_captured(false);
        } else if (event.key.key == SDLK_F11) {
            game_.toggle_fullscreen();
        } else if (event.key.key == SDLK_ESCAPE && game_.is_fullscreen()) {
            game_.toggle_fullscreen();
        } else if (!state_hotkey(event.key.key, event.key.mod)) {
            input_.on_key(event.key.key, true, event.key.timestamp);
        }
        break;
    case SDL_EVENT_KEY_UP:
        input_.on_key(event.key.key, false, event.key.timestamp);
        break;
    default:
        break;
    }
}

// The timer's keys also work while the main window has focus (e.g. with the Timer tab open).
auto App::timer_hotkey(SDL_Keycode key, uint64_t when_ns) -> bool {
    if (key == SDLK_UNKNOWN) return false;
    for (const auto& [hotkey, command] : kTimerHotkeyCommands) {
        if (settings_.hotkeys.keys[static_cast<size_t>(hotkey)] != key) continue;
        if (command != TimerCommand::Pause || !race_blocks(speedrun::Guard::TimerPause)) timer_panel_.command(command, when_ns);
        return true;
    }
    return false;
}

auto App::uses(bitforge::Device device) const -> bool {
    return std::find(settings_.port_devices.begin(), settings_.port_devices.end(), device) != settings_.port_devices.end();
}

auto App::set_mouse_captured(bool on) -> void {
    if (!game_.is_open() || on == mouse_captured_) return;
    SDL_SetWindowRelativeMouseMode(game_.window(), on);
    mouse_captured_ = on;
    input_.take_mouse_motion();  // drop motion from before the capture
    if (on) set_status("Mouse captured for the Mouse controller: Esc releases it");
}

// Pads to their console ports, then the mouse and the scope.
auto App::send_input() -> void {
    emu_thread_.set_port_devices(settings_.port_devices);
    for (int p = 0; p < kPadCount; p++) {
        // Pad 1 also takes the keyboard, so it's always plugged in.
        emu_thread_.set_input(p, input_.state(p), p == 0 || input_.assigned(p) != nullptr);
    }
    if (uses(bitforge::Device::Mouse)) {
        const auto [dx, dy] = mouse_captured_ ? input_.take_mouse_motion() : std::array<int, 2>{};
        emu_thread_.add_mouse_motion(dx, dy, mouse_captured_ && input_.mouse_left(), mouse_captured_ && input_.mouse_right());
    } else if (mouse_captured_) {
        set_mouse_captured(false);  // the port changed away from the mouse
    }
    if (!uses(bitforge::Device::SuperScope) && !mouse_captured_ && !SDL_CursorVisible()) SDL_ShowCursor();
    if (uses(bitforge::Device::SuperScope) && game_.is_open()) {
        const FrameImage& image = emu_thread_.frame();
        int out_w = 0, out_h = 0;
        SDL_GetWindowSizeInPixels(game_.window(), &out_w, &out_h);
        const int lines = image.height > 0 ? picture_lines(image.height) : GameWindow::kBaseHeight;
        const video::Viewport vp = fit_viewport(out_w, out_h, view_options(), lines);
        const float density = SDL_GetWindowPixelDensity(game_.window());
        scope_aim_ = scope_aim(pointer_x_ * density, pointer_y_ * density, static_cast<float>(vp.x), static_cast<float>(vp.y),
                               static_cast<float>(vp.w), static_cast<float>(vp.h), lines);
        if (!pointer_inside_) scope_aim_.offscreen = true;
        scope_aim_.fire = input_.mouse_left();
        scope_aim_.cursor = input_.mouse_right();
        scope_aim_.pause = input_.mouse_middle();
        scope_aim_.turbo = input_.scope_turbo();
        emu_thread_.set_scope(scope_aim_);
    }
}

auto App::present_game(const FrameImage& image) -> void {
    const uint32_t* pixels = image.pixels.empty() ? nullptr : image.pixels.data();
    if (pixels && uses(bitforge::Device::SuperScope) && settings_.pointer.crosshair && !scope_aim_.offscreen) {
        overlay_ = image.pixels;
        draw_crosshair(overlay_.data(), image.width, image.height, picture_lines(image.height), scope_aim_.x, scope_aim_.y);
        pixels = overlay_.data();
    }
    int timer_w = 0;
    int timer_h = 0;
    std::vector<uint32_t> timer = timer_panel_.overlay_image(status_, timer_w, timer_h);
    game_.show_timer(std::move(timer), timer_w, timer_h, settings_.timer.corner);
    int badge_w = 0;
    int badge_h = 0;
    Corner badge_corner = settings_.timer.corner;
    std::vector<uint32_t> badge = movie_badge(badge_w, badge_h, badge_corner);
    game_.show_badge(std::move(badge), badge_w, badge_h, badge_corner);
    game_.present(pixels, image.width, image.height, view_options());
}

// ---- main-window UI --------------------------------------------------------

auto App::draw_ui() -> void {
    if (SDL_GetWindowFlags(main_window_) & SDL_WINDOW_MINIMIZED) return;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    draw_menu_bar();
    draw_race_prompt();
    draw_movie_prompt();
    draw_movie_recovery();

    // Reserve the status bar, and the timeline strip while a game is loaded.
    const float status_h = ImGui::GetFrameHeightWithSpacing();
    const float strip_h = status_.loaded ? timeline_block_height() : 0.f;
    const float body_h = std::max(1.f, ImGui::GetContentRegionAvail().y - status_h - strip_h);
    const bool panels =
        settings_.show_log || show_controls_ || show_audio_ || show_speed_ || show_cheats_ || show_memory_ || settings_.show_timer;
    const float library_h = panels ? body_h * kLibraryShare : body_h;

    ImGui::BeginChild("library", ImVec2(0.f, library_h), ImGuiChildFlags_Borders);
    draw_library();
    ImGui::EndChild();

    bool memory_shown = false;
    if (panels) {
        ImGui::BeginChild("panels", ImVec2(0.f, body_h - library_h - ImGui::GetStyle().ItemSpacing.y));
        if (ImGui::BeginTabBar("debug_tabs")) {
            if (settings_.show_log && ImGui::BeginTabItem("Log", &settings_.show_log)) {
                draw_log_panel();
                ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags timer_flags = select_timer_ ? ImGuiTabItemFlags_SetSelected : 0;
            select_timer_ = false;
            if (settings_.show_timer && ImGui::BeginTabItem("Timer", &settings_.show_timer, timer_flags)) {
                if (timer_panel_.draw(status_, main_window_)) save_settings();
                ImGui::EndTabItem();
            }
            if (show_controls_ && ImGui::BeginTabItem("Controls", &show_controls_)) {
                draw_controls();
                ImGui::EndTabItem();
            }
            if (show_audio_ && ImGui::BeginTabItem("Audio", &show_audio_)) {
                draw_audio_panel();
                ImGui::EndTabItem();
            }
            if (show_speed_ && ImGui::BeginTabItem("Rewind / Fast-forward", &show_speed_)) {
                draw_speed_panel();
                ImGui::EndTabItem();
            }
            if (show_cheats_ && ImGui::BeginTabItem("Cheats", &show_cheats_)) {
                draw_cheats_panel();
                ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags memory_flags = select_memory_ ? ImGuiTabItemFlags_SetSelected : 0;
            select_memory_ = false;
            if (show_memory_ && ImGui::BeginTabItem("Memory", &show_memory_, memory_flags)) {
                memory_panel_.draw(status_, [this](bitforge::CheatEntry e) { add_cheat(std::move(e)); },
                                   [this](std::string text) { notify(std::move(text)); });
                memory_shown = true;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
    }
    emu_thread_.set_memory_view(memory_shown);  // memory is only copied out while the tab shows

    if (status_.loaded) draw_timeline();
    draw_status_bar();
    ImGui::End();
    timer_panel_.draw_windows(status_);

    ImGui::Render();
    SDL_SetRenderScale(main_renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(main_renderer_, kClearGray, kClearGray, kClearGray, 1.f);
    SDL_RenderClear(main_renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), main_renderer_);
    SDL_RenderPresent(main_renderer_);
}

auto App::draw_menu_bar() -> void {
    if (!ImGui::BeginMenuBar()) return;
    const bool loaded = status_.loaded;
    const bool running = status_.state == RunState::Running;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open ROM...")) open_rom_dialog();
        if (ImGui::BeginMenu("Recent ROMs", !settings_.recent_roms.empty())) {
            std::string chosen;
            for (const auto& path : settings_.recent_roms) {
                if (ImGui::MenuItem(path.c_str())) chosen = path;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear list")) {
                settings_.recent_roms.clear();
                save_settings();
            }
            ImGui::EndMenu();
            if (!chosen.empty()) boot(chosen);
        }
        if (ImGui::MenuItem("Refresh library")) scan_roms();
        ImGui::Separator();
        using speedrun::Guard;
        if (ImGui::BeginMenu("Save State", loaded && speedrun::race_allows(status_.race, Guard::SaveState))) {
            draw_state_menu(true);
            ImGui::EndMenu();
        }
        race_tooltip(Guard::SaveState);
        if (ImGui::BeginMenu("Load State", loaded && speedrun::race_allows(status_.race, Guard::LoadState))) {
            draw_state_menu(false);
            ImGui::EndMenu();
        }
        race_tooltip(Guard::LoadState);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) request_quit();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Emulation")) {
        if (ImGui::MenuItem(running ? "Pause" : "Run", "Space", false, loaded)) toggle_pause();
        const bool playing = status_.movie.mode == MovieMode::Playing;
        if (ImGui::MenuItem("Reset", nullptr, false, loaded && !playing)) {
            emu_thread_.post([](EmuThread::Core& c) {
                c.emu.reset();
                c.audio.start_grace();
            });
        }
        if (playing) ImGui::SetItemTooltip("A movie is playing: its own resets are in it.");
        if (ImGui::MenuItem("Stop", nullptr, false, loaded)) request_stop();
        const bool can_advance = speedrun::race_allows(status_.race, speedrun::Guard::FrameAdvance);
        if (ImGui::MenuItem("Frame advance", nullptr, false, loaded && status_.state == RunState::Paused && can_advance)) {
            emu_thread_.post([](EmuThread::Core& c) { c.advance_one = true; });
        }
        race_tooltip(speedrun::Guard::FrameAdvance);
        ImGui::Separator();
        ImGui::MenuItem("Controls", nullptr, &show_controls_);
        ImGui::MenuItem("Rewind / Fast-forward", nullptr, &show_speed_);
        ImGui::MenuItem("Cheats", nullptr, &show_cheats_);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Window scale")) {
            for (int s = Settings::kMinScale; s <= Settings::kMaxScale; s++) {
                char label[8];
                SDL_snprintf(label, sizeof(label), "%dx", s);
                if (ImGui::MenuItem(label, nullptr, settings_.scale == s)) {
                    settings_.scale = s;
                    apply_view();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Integer scaling", nullptr, &settings_.integer_scaling)) save_settings();
        if (ImGui::MenuItem("8:7 pixel aspect", nullptr, &settings_.aspect_8_7)) apply_view();
        if (ImGui::MenuItem("On-screen messages", nullptr, &settings_.show_messages)) {
            if (!settings_.show_messages) game_.show_message({});
            save_settings();
        }
        if (ImGui::MenuItem("Fullscreen game window", "F11", game_.is_fullscreen(), game_.is_open())) {
            game_.toggle_fullscreen();
        }
        ImGui::Separator();
        draw_video_menus();
        ImGui::EndMenu();
    }

    draw_audio_menu();

    if (ImGui::BeginMenu("Speedrun")) {
        const SDL_Keycode race_key = settings_.hotkeys.keys[static_cast<size_t>(Hotkey::RaceMode)];
        if (ImGui::MenuItem("Race mode", race_key == SDLK_UNKNOWN ? nullptr : SDL_GetKeyName(race_key), status_.race)) {
            request_race(!status_.race);
        }
        ImGui::SetItemTooltip("Restarts the game from power-on and turns off everything that changes the game or its timing "
                              "(cheats, memory freezes and edits, rewind, the timeline, save states, fast-forward, frame "
                              "advance, timer pause). A run finished this way is marked race verified.");
        if (ImGui::MenuItem("Write a verification file for verified runs", nullptr, &settings_.race.write_verification)) {
            emu_thread_.post([on = settings_.race.write_verification](EmuThread::Core& c) { c.verify.write = on; });
            save_settings();
        }
        ImGui::SetItemTooltip("<splits>.attempt-<n>.verified.json beside the splits file: ROM hashes, emulator version, "
                              "timing and the final times, for a leaderboard submission.");
        ImGui::Separator();
        if (ImGui::MenuItem("Run timer", nullptr, settings_.show_timer)) {
            settings_.show_timer = !settings_.show_timer;
            select_timer_ = settings_.show_timer;
            save_settings();
        }
        if (ImGui::MenuItem("Timer overlay in the game window", nullptr, &settings_.timer.overlay)) save_settings();
        ImGui::EndMenu();
    }

    draw_movie_menu();

    if (ImGui::BeginMenu("Debug")) {
        if (ImGui::MenuItem("Log", nullptr, &settings_.show_log)) save_settings();
        const SDL_Keycode memory_key = settings_.hotkeys.keys[static_cast<size_t>(Hotkey::MemoryWatch)];
        if (ImGui::MenuItem("Memory watch / RAM search", memory_key == SDLK_UNKNOWN ? nullptr : SDL_GetKeyName(memory_key),
                            show_memory_)) {
            toggle_memory_panel();
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();

    // Space toggles pause while the main window has focus and no text field is active.
    if (loaded && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggle_pause();
}

auto App::toggle_pause() -> void {
    emu_thread_.post([](EmuThread::Core& c) {
        if (!c.emu.loaded()) return;
        c.state = c.state == RunState::Running ? RunState::Paused : RunState::Running;
    });
}

auto App::draw_video_menus() -> void {
    using video::BackendKind;
    constexpr struct {
        BackendKind kind;
        const char* label;
    } kRenderers[] = {
        {BackendKind::Gpu, "SDL_GPU (Vulkan / D3D12 / Metal)"},
        {BackendKind::Renderer, "SDL_Renderer (nearest/linear only)"},
    };
    if (ImGui::BeginMenu("Renderer")) {
        for (const auto& r : kRenderers) {
            if (ImGui::MenuItem(r.label, nullptr, settings_.renderer == r.kind)) {
                settings_.renderer = r.kind;
                game_.set_backend(r.kind, current_filter());  // stalls only the UI thread
                save_settings();
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Filter")) {
        for (const auto& preset : filters_) {
            if (ImGui::MenuItem(preset.name.c_str(), nullptr, preset.name == current_filter().name)) {
                settings_.filter = preset.name;
                game_.set_filter(preset);
                save_settings();
            }
        }
        ImGui::EndMenu();
    }
}

auto App::draw_audio_menu() -> void {
    if (!ImGui::BeginMenu("Audio")) return;
    const bool open = status_.audio.open;
    if (ImGui::MenuItem("Mute", nullptr, &settings_.muted)) {
        emu_thread_.post([muted = settings_.muted](EmuThread::Core& c) { c.audio.set_muted(muted); });
        save_settings();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
    if (ImGui::SliderInt("Volume", &settings_.volume, 0, 100, "%d%%")) {
        emu_thread_.post([volume = settings_.volume](EmuThread::Core& c) { c.audio.set_volume(volume); });
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
    if (ImGui::SliderInt("MSU-1 volume", &settings_.msu1_volume, 0, 100, "%d%%")) {
        emu_thread_.post([volume = settings_.msu1_volume](EmuThread::Core& c) { c.audio.set_msu1_volume(volume); });
    }
    ImGui::SetItemTooltip("CD-quality audio of MSU-1 games (a .msu file beside the ROM), relative to the volume.");
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
    ImGui::Separator();
    if (ImGui::MenuItem("Sync emulation to audio", nullptr, &settings_.audio_sync, open)) {
        emu_thread_.post([sync = settings_.audio_sync](EmuThread::Core& c) {
            c.audio_sync = sync;
            c.audio.set_rate_control(!sync);
        });
        save_settings();
    }
    ImGui::SetItemTooltip("Pace frames by the sound card's clock instead of the timer.\n"
                          "Exact pitch and no rate control, but frame pacing follows the device's buffer.");
    ImGui::Separator();
    const bool core_on = status_.core_recording;
    if (ImGui::MenuItem("Record core audio (32040 Hz)", nullptr, core_on, open)) set_core_recording(!core_on);
    const bool device_on = status_.device_recording;
    if (ImGui::MenuItem("Record device output", nullptr, device_on, open)) set_device_recording(!device_on);
    ImGui::Separator();
    ImGui::MenuItem("Audio panel", nullptr, &show_audio_);
    ImGui::EndMenu();
}

auto App::draw_audio_panel() -> void {
    const AudioOutput::Stats& s = status_.audio;
    if (!s.open) {
        ImGui::TextDisabled("No audio device: emulation runs silent.");
        return;
    }
    const double device_ms = s.device_rate > 0 ? 1000.0 * s.device_buffer_frames / s.device_rate : 0.0;
    const double pull_ms = s.source_rate > 0 ? 1000.0 * s.last_pull_frames / s.source_rate : 0.0;
    ImGui::Text("APU %d Hz stereo -> %s, %d Hz, %d ch  |  device buffer %d frames (%.1f ms)  |  pull %.1f ms", s.source_rate,
                s.driver, s.device_rate, s.device_channels, s.device_buffer_frames, device_ms, pull_ms);
    ImGui::Text("Queue");
    ImGui::SameLine();
    char overlay[64];
    SDL_snprintf(overlay, sizeof(overlay), "%.1f ms (avg %.1f, target %.0f ms)", s.queued_ms, s.level_ms, s.target_ms);
    ImGui::ProgressBar(static_cast<float>(s.queued_ms / (2.0 * s.target_ms)), ImVec2(-1.f, 0.f), overlay);
    if (s.rate_control) {
        ImGui::Text("Rate adjust %+.3f%% (learned %+.3f%%, limit +/-%.1f%%)", (s.ratio - 1.0) * 100.0, s.learned * 100.0,
                    RateControl::kMaxRateAdjust * 100.0);
    } else {
        ImGui::Text("Rate adjust off: emulation is paced by the audio queue");
    }
    const ImVec4 bad(1.f, 0.45f, 0.35f, 1.f);
    const bool underrunning = s.underruns > 0;
    if (underrunning) ImGui::PushStyleColor(ImGuiCol_Text, bad);
    ImGui::Text("Underruns %llu (%.0f ms of silence)", static_cast<unsigned long long>(s.underruns), s.silence_ms);
    if (underrunning) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("|  overflows %llu (%.0f ms dropped)  |  %s", static_cast<unsigned long long>(s.overflows), s.dropped_ms,
                s.paused ? "paused" : (settings_.muted ? "muted" : "playing"));

    const bool slow = status_.frame_ms_avg > kFrameBudgetMs * 0.95;
    if (slow) ImGui::PushStyleColor(ImGuiCol_Text, bad);
    ImGui::Text("Emulation %.2f ms per frame (worst %.2f ms in the last second) of a %.2f ms budget", status_.frame_ms_avg,
                status_.frame_ms_max, kFrameBudgetMs);
    if (slow) {
        ImGui::TextWrapped("Emulation is not keeping up with real time, so audio will run dry whatever the buffer does. "
                           "Is this a Debug build?");
        ImGui::PopStyleColor();
    }
#ifndef NDEBUG
    ImGui::TextDisabled("Debug build: expect it to run slower than Release.");
#endif
    if (status_.core_recording) {
        ImGui::Text("Recording core audio: %s (%.1f s)", status_.core_recording_path.c_str(), status_.core_recording_seconds);
    }
    if (status_.device_recording) {
        ImGui::Text("Recording device output: %s (%.1f s)", status_.device_recording_path.c_str(), status_.device_recording_seconds);
    }
}

auto App::recording_path(const char* kind) const -> std::string {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "%Y%m%d_%H%M%S", &local);
    return (fs::path(base_dir_) / (std::string("snes_audio_") + kind + "_" + name + ".wav")).string();
}

// The recorders belong to the audio output, which the emulation thread drives: start and stop
// them there (or directly, before the thread starts and after it stops).
auto App::set_core_recording(bool on, const std::string& path) -> void {
    const std::string file = on ? (path.empty() ? recording_path("core") : path) : std::string();
    emu_thread_.call([on, &file](EmuThread::Core& c) {
        if (!on) {
            const auto& rec = c.audio.core_recording();
            if (!rec.is_open()) return;
            utils::Logger::info(utils::LogChannel::Audio, "Core audio recording saved: %s (%.1f s at %d Hz)", rec.path().c_str(), rec.seconds(),
                                rec.sample_rate());
            c.audio.stop_core_recording();
            return;
        }
        if (c.audio.start_core_recording(file)) {
            utils::Logger::info(utils::LogChannel::Audio, "Recording core audio (32040 Hz, before the mixer) to %s", file.c_str());
        } else {
            utils::Logger::warning(utils::LogChannel::Audio, "Could not record core audio to %s", file.c_str());
        }
    });
}

auto App::set_device_recording(bool on, const std::string& path) -> void {
    const std::string file = on ? (path.empty() ? recording_path("device") : path) : std::string();
    emu_thread_.call([on, &file](EmuThread::Core& c) {
        if (!on) {
            if (!c.audio.device_recording()) return;
            const std::string saved = c.audio.device_recording_path();
            c.audio.stop_device_recording();
            utils::Logger::info(utils::LogChannel::Audio, "Device output recording saved: %s (%.1f s)", saved.c_str(),
                                c.audio.device_recording_seconds());
            return;
        }
        if (c.audio.start_device_recording(file)) {
            utils::Logger::info(utils::LogChannel::Audio, "Recording device output (what the sound card plays) to %s", file.c_str());
        } else {
            utils::Logger::warning(utils::LogChannel::Audio, "Could not record device output to %s", file.c_str());
        }
    });
}

auto App::draw_library() -> void {
    ImGui::Text("ROM library  (%s)", roms_dir_.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) scan_roms();
    ImGui::Separator();

    if (library_.empty()) {
        ImGui::TextDisabled("No .sfc/.smc files found. Use File > Open ROM, or put ROMs in the roms folder.");
        return;
    }

    std::string to_boot;
    for (int i = 0; i < static_cast<int>(library_.size()); i++) {
        const std::string name = fs::path(library_[static_cast<size_t>(i)]).filename().string();
        if (ImGui::Selectable(name.c_str(), library_selected_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            library_selected_ = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) to_boot = library_[static_cast<size_t>(i)];
        }
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) to_boot = library_[static_cast<size_t>(i)];
    }
    if (!to_boot.empty()) boot(to_boot);
}

auto App::draw_controls() -> void {
    // Connected pads, in connection order (the order "Auto" ports take them).
    const auto& devices = input_.devices();
    if (devices.empty()) {
        ImGui::TextDisabled("No gamepads connected. Plug one in (XInput pads such as the F310 work as is).");
    } else {
        ImGui::Text("Gamepads:");
        for (const auto& d : devices) {
            int port = -1;
            for (int p = 0; p < kPadCount; p++) {
                if (input_.assigned(p) == &d) port = p;
            }
            ImGui::SameLine();
            if (port >= 0) {
                ImGui::Text("[%s -> pad %d]", d.name.c_str(), port + 1);
            } else {
                ImGui::TextDisabled("[%s, unused]", d.name.c_str());
            }
        }
    }
    draw_console_ports();
    const auto slots = pad_slots(settings_.port_devices);
    int pads = 0;  // pads plugged in somewhere (they come first)
    while (pads < kPadCount && slots[static_cast<size_t>(pads)].port >= 0) pads++;
    for (int p = 0; p < pads; p++) draw_port_controls(p);

    ImGui::TextDisabled("Click a cell, then press the key or pad button (Esc cancels). Keys act on the game window.");
    const int columns = std::max(pads, 2);  // the hotkey rows use two
    if (ImGui::BeginTable("bindings", 2 + columns,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Button");
        ImGui::TableSetupColumn("Key (pad 1)");
        for (int p = 0; p < columns; p++) ImGui::TableSetupColumn(("Pad " + std::to_string(p + 1)).c_str());
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < kPadButtons.size(); i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kPadButtons[i].name.data(), kPadButtons[i].name.data() + kPadButtons[i].name.size());
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextColumn();
            const bool waiting = binding_button_ == static_cast<int>(i);
            if (ImGui::Button(waiting ? "press a key..." : SDL_GetKeyName(settings_.keys[i]))) {
                input_.cancel_binding();
                binding_button_ = static_cast<int>(i);
            }
            for (int p = 0; p < columns; p++) {
                ImGui::TableNextColumn();
                if (p >= pads) continue;
                ImGui::PushID(p);
                const bool pad_waiting = input_.binding(p, static_cast<int>(i));
                const std::string label = pad_waiting ? "press a button..."
                                                      : std::string(button_display_name(input_.port(p).buttons[i]));
                if (ImGui::Button(label.c_str())) {
                    binding_button_ = -1;
                    if (pad_waiting) {
                        input_.cancel_binding();
                    } else {
                        input_.start_binding(p, static_cast<int>(i));
                    }
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        draw_hotkey_rows();
        ImGui::EndTable();
    }
    if (ImGui::Button("Restore default buttons")) {
        settings_.keys = default_key_map();
        input_.set_key_map(settings_.keys);
        settings_.hotkeys = default_hotkeys();
        input_.set_hotkeys(settings_.hotkeys);
        for (int p = 0; p < kPadCount; p++) {
            PortConfig config = input_.port(p);
            config.buttons = default_button_map();
            config.deadzone = PortConfig::kDefaultDeadzone;
            config.stick_as_dpad = true;
            input_.set_port(p, config);
        }
        sync_input_settings();
    }
}

// Device, deadzone and stick option for one port, on one line.
auto App::draw_port_controls(int port) -> void {
    ImGui::PushID(port);
    PortConfig config = input_.port(port);
    bool changed = false;

    std::string preview;
    if (config.device == kAutoDevice) {
        preview = "Auto (connection order)";
    } else if (config.device == kNoDevice) {
        preview = port == 0 ? "Keyboard only" : "Unplugged";
    } else {
        preview = "(not connected)";
        for (const auto& d : input_.devices()) {
            if (d.guid == config.device) preview = d.name;
        }
    }
    if (const auto* d = input_.assigned(port)) preview += "  [" + d->name + "]";

    ImGui::AlignTextToFramePadding();
    const PadSlot slot = pad_slots(settings_.port_devices)[static_cast<size_t>(port)];
    const std::string where = pad_slot_label(slot, settings_.port_devices[static_cast<size_t>(std::max(slot.port, 0))]);
    ImGui::Text("Pad %d", port + 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", where.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.f);
    if (ImGui::BeginCombo("##device", preview.c_str())) {
        auto option = [&](const std::string& value, const char* label) {
            if (ImGui::Selectable(label, config.device == value)) {
                config.device = value;
                changed = true;
            }
        };
        option(std::string(kAutoDevice), "Auto (connection order)");
        option(std::string(kNoDevice), port == 0 ? "Keyboard only" : "Unplugged");
        for (const auto& d : input_.devices()) {
            ImGui::PushID(&d);
            option(d.guid, d.name.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.f);
    ImGui::SliderInt("Deadzone", &config.deadzone, 0, 95, "%d%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    if (ImGui::IsItemEdited()) input_.set_port(port, config);  // live while dragging
    ImGui::SameLine();
    if (ImGui::Checkbox("Left stick as D-pad", &config.stick_as_dpad)) changed = true;

    if (changed) {
        input_.set_port(port, config);
        sync_input_settings();
    }
    ImGui::PopID();
}

// What's plugged into console ports 1 and 2, and the host-mouse options for a SNES Mouse or
// Super Scope.
auto App::draw_console_ports() -> void {
    bool changed = false;
    for (int p = 0; p < kPortCount; p++) {
        ImGui::PushID(1000 + p);
        bitforge::Device& device = settings_.port_devices[static_cast<size_t>(p)];
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Port %d", p + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.f);
        if (ImGui::BeginCombo("##port", bitforge::device_info(device).name.data())) {
            for (const auto& d : bitforge::kDevices) {
                if (!device_allowed(p, d.device)) continue;
                if (ImGui::Selectable(d.name.data(), device == d.device)) {
                    device = d.device;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (p == 0) ImGui::SameLine();
        ImGui::PopID();
    }
    if (uses(bitforge::Device::Mouse) || uses(bitforge::Device::SuperScope)) {
        PointerConfig& pointer = settings_.pointer;
        if (uses(bitforge::Device::Mouse)) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.f);
            ImGui::SliderInt("Mouse speed", &pointer.mouse_speed, 10, 400, "%d%%");
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            ImGui::SameLine();
            ImGui::TextDisabled("Click the game window to capture the mouse, Esc to release.");
        }
        if (uses(bitforge::Device::SuperScope)) {
            if (ImGui::Checkbox("Crosshair", &pointer.crosshair)) changed = true;
            ImGui::SameLine();
            bool turbo = input_.scope_turbo();
            if (ImGui::Checkbox("Turbo switch", &turbo)) input_.set_scope_turbo(turbo);
            ImGui::SameLine();
            ImGui::TextDisabled("Aim with the mouse. Left = Fire, right = Cursor, middle = Pause, side button = Turbo.");
        }
        input_.set_pointer_config(pointer);
    }
    if (changed) save_settings();
}

// Hotkey rows under the pad buttons: the key, and one pad input that works on any gamepad.
auto App::draw_hotkey_rows() -> void {
    for (size_t h = 0; h < kHotkeys.size(); h++) {
        const auto hotkey = static_cast<Hotkey>(h);
        const int index = static_cast<int>(kPadButtons.size() + h);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kHotkeys[h].label.data(), kHotkeys[h].label.data() + kHotkeys[h].label.size());
        ImGui::PushID(index);
        ImGui::TableNextColumn();
        const SDL_Keycode key = settings_.hotkeys.keys[h];
        if (ImGui::Button(binding_button_ == index ? "press a key..." : key == SDLK_UNKNOWN ? "(none)" : SDL_GetKeyName(key))) {
            input_.cancel_binding();
            binding_button_ = index;
        }
        ImGui::TableNextColumn();
        const bool waiting = input_.hotkey_binding(hotkey);
        const std::string label = waiting ? "press a button or trigger..." : pad_input_name(input_.hotkeys().pad[h]) + "  (any pad)";
        if (ImGui::Button(label.c_str())) {
            binding_button_ = -1;
            if (waiting) {
                input_.cancel_binding();
            } else {
                input_.start_hotkey_binding(hotkey);
            }
        }
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Clear pad")) {
            HotkeyMap map = input_.hotkeys();
            map.pad[h] = kPadNone;
            input_.set_hotkeys(map);
            sync_input_settings();
        }
        ImGui::PopID();
    }
}

auto App::apply_speed_settings() -> void {
    emu_thread_.post([speed = settings_.speed](EmuThread::Core& c) {
        c.speed = speed;
        c.apply_speed();
    });
    save_settings();
}

auto App::draw_speed_panel() -> void {
    SpeedConfig& s = settings_.speed;
    bool changed = false;
    if (status_.race) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.f), "Race mode is on: rewind and fast-forward are off until you turn it off "
                                                             "(Speedrun > Race mode). These settings still apply after that.");
    }
    ImGui::SeparatorText("Rewind");
    changed |= ImGui::Checkbox("Enable rewind", &s.rewind);
    ImGui::SetItemTooltip("Keeps compressed snapshots in memory while the game runs. The timeline shows them; hold the rewind hotkey to step back.");
    ImGui::BeginDisabled(!s.rewind);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
    ImGui::SliderInt("Memory budget", &s.rewind_budget_mb, SpeedConfig::kMinBudgetMb, SpeedConfig::kMaxBudgetMb, "%d MB",
                     ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
    ImGui::SliderInt("Snapshot every", &s.rewind_interval, 1, SpeedConfig::kMaxInterval, "%d frames");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Rewinding steps back one snapshot per displayed frame, so this is also the rewind speed.");
    int audio = s.rewind_reverse_audio ? 1 : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
    if (ImGui::Combo("Audio while rewinding", &audio, "Muted\0Reversed\0")) {
        s.rewind_reverse_audio = audio == 1;
        changed = true;
    }
    if (status_.loaded) {
        ImGui::Text("History: %zu snapshots, %.1f s, %.1f MB", status_.rewind_entries, status_.rewind_seconds,
                    static_cast<double>(status_.rewind_bytes) / (1 << 20));
        ImGui::TextDisabled("The timeline under the library shows those snapshots.");
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Fast-forward");
    static constexpr int kSpeeds[] = {2, 3, 4, 5, 6, 8, 10, 16, 0};
    std::string preview = s.fast_forward_speed == 0 ? "Uncapped" : std::to_string(s.fast_forward_speed) + "x";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
    if (ImGui::BeginCombo("Speed", preview.c_str())) {
        for (const int speed : kSpeeds) {
            const std::string label = speed == 0 ? "Uncapped" : std::to_string(speed) + "x";
            if (ImGui::Selectable(label.c_str(), s.fast_forward_speed == speed)) {
                s.fast_forward_speed = speed;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Audio plays sped up (higher pitch) while fast-forwarding. Hotkeys are under Controls.");
    if (changed) apply_speed_settings();
}

auto App::sync_cheats() -> void {
    if (cheats_rom_ == status_.rom_path) return;
    cheats_ = emu_thread_.call([](EmuThread::Core& c) { return c.emu.cheats(); });
    cheats_rom_ = status_.rom_path;
    cheat_error_.clear();
}

auto App::add_cheat(bitforge::CheatEntry entry) -> void {
    sync_cheats();
    cheats_.push_back(std::move(entry));
    apply_cheats();
}

auto App::toggle_memory_panel() -> void {
    show_memory_ = !show_memory_;
    select_memory_ = show_memory_;
}

auto App::apply_cheats() -> void {
    emu_thread_.post([list = cheats_](EmuThread::Core& c) {
        if (!c.emu.set_cheats(list)) utils::Logger::error(utils::LogChannel::Rom, "Could not write %s", c.emu.cheats_path().c_str());
    });
}

// What a code does, for the tooltip: "$C4A704 = $C9 (ROM)", one line per part.
static auto describe_cheat(const std::string& code) -> std::string {
    const auto cheats = bitforge::decode_cheat(code);
    if (!cheats) return "not a valid code";
    std::string out;
    for (const auto& c : *cheats) {
        char line[96];
        if (c.compare) {
            std::snprintf(line, sizeof(line), "$%06X = $%02X if $%02X (read)\n", c.address, c.value, *c.compare);
        } else {
            std::snprintf(line, sizeof(line), "$%06X = $%02X (%s)\n", c.address, c.value,
                          c.kind == bitforge::Cheat::Kind::RamWrite ? "WRAM, written every frame" : "read");
        }
        out += line;
    }
    out.pop_back();
    return out;
}

auto App::draw_cheats_panel() -> void {
    if (!status_.loaded) {
        ImGui::TextDisabled("Load a game to edit its cheats.");
        return;
    }
    sync_cheats();
    bool changed = false;
    int remove = -1;
    const bool cheats_allowed = speedrun::race_allows(status_.race, speedrun::Guard::Cheats);
    if (!cheats_allowed) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.f), "Race mode is on: no cheat is applied, whatever is ticked here.");
        race_tooltip(speedrun::Guard::Cheats);
    }
    ImGui::BeginDisabled(!cheats_allowed);
    if (ImGui::BeginTable("cheats", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing() * 2.6f))) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12.f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < cheats_.size(); i++) {
            auto& e = cheats_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            changed |= ImGui::Checkbox("##on", &e.enabled);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.code.c_str());
            ImGui::SetItemTooltip("%s", describe_cheat(e.code).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.description.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Delete")) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        cheats_.erase(cheats_.begin() + remove);
        changed = true;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.f);
    ImGui::InputTextWithHint("##code", "DDAA-AAAA / 7E0DBF09", cheat_code_.data(), cheat_code_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.f);
    ImGui::InputTextWithHint("##description", "description", cheat_description_.data(), cheat_description_.size());
    ImGui::SameLine();
    if (ImGui::Button("Add")) {
        const std::string code = cheat_code_.data();
        if (bitforge::decode_cheat(code)) {
            cheats_.push_back({true, code, cheat_description_.data()});
            cheat_code_.fill(0);
            cheat_description_.fill(0);
            cheat_error_.clear();
            changed = true;
        } else {
            cheat_error_ = "\"" + code + "\" is not a Game Genie (DDAA-AAAA), Pro Action Replay (AAAAAAVV) or AAAAAA:VV code";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("All off")) {
        for (auto& e : cheats_) e.enabled = false;
        changed = true;
    }
    ImGui::EndDisabled();
    if (!cheat_error_.empty()) {
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", cheat_error_.c_str());
    } else {
        ImGui::TextDisabled("Saved to cheats/<rom>.cht. Join several codes with +. Codes take effect immediately.");
    }
    if (changed) apply_cheats();
}

auto App::sync_input_settings() -> void {
    settings_.hotkeys = input_.hotkeys();
    for (int p = 0; p < kPadCount; p++) settings_.ports[static_cast<size_t>(p)] = input_.port(p);
    save_settings();
}

// ---- log ---------------------------------------------------------------------------------------

auto App::draw_log_panel() -> void {
    bool changed = false;
    if (ImGui::BeginTabBar("log_tabs")) {
        if (ImGui::BeginTabItem("System")) {
            changed |= system_log_.draw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Emulation")) {
            changed |= emu_log_.draw();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!changed) return;
    for (size_t i = 0; i < utils::kLogSinkCount; i++) settings_.log.level[i] = utils::Logger::level(static_cast<utils::LogSink>(i));
    for (size_t i = 0; i < utils::kLogChannelCount; i++) {
        settings_.log.channel[i] = utils::Logger::channel_enabled(static_cast<utils::LogChannel>(i));
    }
    settings_.log.file = {system_log_.file_enabled, emu_log_.file_enabled};
    apply_log_settings();
    save_settings();
}

auto App::apply_log_settings() -> void {
    using utils::Logger;
    for (size_t i = 0; i < utils::kLogSinkCount; i++) {
        const auto sink = static_cast<utils::LogSink>(i);
        Logger::set_level(sink, settings_.log.level[i]);
        if (!settings_.log.file[i]) {
            Logger::set_file(sink, {}, 0);
        } else if (Logger::file_path(sink).empty()) {
            const std::string path = (fs::path(base_dir_) / kLogFiles[i].path).string();
            if (!Logger::set_file(sink, path, kLogFiles[i].max_bytes)) utils::Logger::warning("Could not open %s", path.c_str());
        }
    }
    for (size_t i = 0; i < utils::kLogChannelCount; i++) {
        Logger::set_channel_enabled(static_cast<utils::LogChannel>(i), settings_.log.channel[i]);
    }
    system_log_.file_enabled = settings_.log.file[0];
    emu_log_.file_enabled = settings_.log.file[1];
}

// ---- save states ---------------------------------------------------------------------------------

auto App::state_hotkey(SDL_Keycode key, SDL_Keymod mod) -> bool {
    if (key < SDLK_F1 || key > SDLK_F10 || !status_.loaded) return false;
    const int slot = static_cast<int>(key - SDLK_F1) + 1;
    const bool save = (mod & SDL_KMOD_SHIFT) != 0;
    if (race_blocks(save ? speedrun::Guard::SaveState : speedrun::Guard::LoadState)) return true;  // the key is still ours
    if (save) {
        save_slot(slot);
    } else {
        load_slot(slot);
    }
    return true;
}

auto App::save_slot(int slot) -> void {
    const std::string error = emu_thread_.call([slot](EmuThread::Core& c) { return c.emu.save_state(slot); });
    notify(error.empty() ? "Saved state " + std::to_string(slot) : "Save failed: " + error);
}

auto App::load_slot(int slot) -> void {
    const std::string error = emu_thread_.call([slot](EmuThread::Core& c) {
        std::string why = c.emu.load_state(slot);
        if (why.empty()) {
            c.audio.flush();     // the sound jumps: fade the queue out and the new audio in
            c.republish = true;  // show the loaded picture even while paused
        }
        return why;
    });
    notify(error.empty() ? "Loaded state " + std::to_string(slot) : "Load failed: " + error);
}

auto App::set_status(std::string text) -> void {
    status_text_ = std::move(text);
    status_until_ns_ = SDL_GetTicksNS() + kStatusNs;
}

auto App::notify(std::string text) -> void {
    if (settings_.show_messages) game_.show_message(text);
    set_status(std::move(text));
}

// Reloads a slot's thumbnail when its file appears, changes or goes away.
auto App::refresh_slots() -> void {
    for (int i = 0; i < Emulator::kStateSlots; i++) {
        SlotView& view = slots_[static_cast<size_t>(i)];
        const std::string path = Emulator::state_path_for(base_dir_, status_.rom_path, i + 1);
        std::error_code ec;
        const auto mtime = fs::last_write_time(path, ec);
        if (ec) {
            if (view.texture) SDL_DestroyTexture(view.texture);
            view = SlotView{};
            continue;
        }
        if (view.path == path && view.mtime == mtime) continue;
        if (view.texture) SDL_DestroyTexture(view.texture);
        view = SlotView{};
        view.path = path;
        view.mtime = mtime;
        const auto info = Emulator::read_slot_info(path);
        if (!info) {
            view.when = "(unreadable or older version)";
            continue;
        }
        view.when = local_time(info->timestamp, "%Y-%m-%d %H:%M:%S");
        view.timestamp = info->timestamp;
        view.frame = info->frame;
        view.version = info->version;
        view.thumb_w = info->thumb_w;
        view.thumb_h = info->thumb_h;
        if (info->thumb_w <= 0 || info->thumb_h <= 0) continue;
        view.texture = SDL_CreateTexture(main_renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, info->thumb_w,
                                         info->thumb_h);
        if (view.texture) {
            SDL_UpdateTexture(view.texture, nullptr, info->thumbnail.data(), info->thumb_w * 4);
            SDL_SetTextureScaleMode(view.texture, SDL_SCALEMODE_LINEAR);
        }
    }
}

auto App::draw_state_menu(bool save) -> void {
    refresh_slots();
    const float thumb_h = kThumbHeight * display_scale();
    const float thumb_w = thumb_h * 8.f / 7.f;
    for (int i = 0; i < Emulator::kStateSlots; i++) {
        const SlotView& view = slots_[static_cast<size_t>(i)];
        const bool exists = !view.when.empty();
        ImGui::PushID(i);
        if (view.texture) {
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(view.texture)), ImVec2(thumb_w, thumb_h));
        } else {
            ImGui::Dummy(ImVec2(thumb_w, thumb_h));
        }
        ImGui::SameLine();
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const std::string age = view.timestamp ? Emulator::age_text(view.timestamp, now) : "";
        char label[128];
        if (!exists) {
            SDL_snprintf(label, sizeof(label), "Slot %d   (empty)", i + 1);
        } else if (age.empty()) {
            SDL_snprintf(label, sizeof(label), "Slot %d   %s", i + 1, view.when.c_str());
        } else {
            SDL_snprintf(label, sizeof(label), "Slot %d   %s  (%s)", i + 1, view.when.c_str(), age.c_str());
        }
        char shortcut[16];
        SDL_snprintf(shortcut, sizeof(shortcut), save ? "Shift+F%d" : "F%d", i + 1);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (thumb_h - ImGui::GetFrameHeight()) * 0.5f);
        if (ImGui::MenuItem(label, shortcut, false, save || exists)) {
            if (save) {
                save_slot(i + 1);
            } else {
                load_slot(i + 1);
            }
        }
        // Hover: the thumbnail at twice the menu size, with the details.
        if (view.timestamp && ImGui::BeginItemTooltip()) {
            if (view.texture) {
                const float big_h = thumb_h * 2.f;
                const float big_w = big_h * 8.f / 7.f;
                ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(view.texture)), ImVec2(big_w, big_h));
            }
            ImGui::Text("Slot %d, saved %s (%s)", i + 1, view.when.c_str(), age.c_str());
            ImGui::Text("Play time %s since boot (frame %llu)", Emulator::play_time_text(view.frame).c_str(),
                        static_cast<unsigned long long>(view.frame));
            if (view.version != bitforge::savestate::kVersion) ImGui::TextDisabled("Saved by an older version (format %u)", view.version);
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
}

auto App::timeline_hint_height() -> float {
    // The longest line, wrapped to this window, so a narrow window doesn't clip the instruction
    // and the strip below it doesn't jump when the message changes.
    const char* hints[] = {
        "Click a picture to play from there. Right-click it to save that moment to a slot.",
        "Rewind is off. Enable it under Emulation > Rewind / Fast-forward.",
        "It fills in as the game runs.",
        kRaceTimelineHint,
    };
    const float label_w = ImGui::CalcTextSize("Timeline ").x;
    const float wrap = std::max(1.f, ImGui::GetContentRegionAvail().x - label_w);
    float height = ImGui::GetTextLineHeight();
    for (const char* hint : hints) {
        height = std::max(height, ImGui::CalcTextSize(hint, nullptr, false, wrap).y);
    }
    return height + ImGui::GetStyle().ItemSpacing.y;
}

auto App::timeline_block_height() -> float {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float thumb = kThumbHeight * display_scale();
    // ImageButton adds frame padding around the picture. The scrollbar is always reserved so the
    // strip doesn't jump when the history grows past the window.
    const float item = thumb + style.FramePadding.y * 2.f;
    const float child = item + style.WindowPadding.y * 2.f + style.ScrollbarSize + style.WindowBorderSize * 2.f;
    return timeline_hint_height() + child + style.ItemSpacing.y;
}

auto App::clear_timeline() -> void {
    for (TimelineView& view : timeline_) {
        if (view.texture) SDL_DestroyTexture(view.texture);
    }
    timeline_.clear();
}

// Pulls pictures for ids the strip doesn't have yet, and drops textures for moments that have
// fallen out of the history. One new snapshot arrives every few frames, so this copies one
// thumbnail at a time rather than the whole strip.
auto App::sync_timeline() -> void {
    const uint64_t lo = status_.timeline_oldest_id;
    const uint64_t hi = status_.timeline_newest_id;
    if (!status_.loaded || lo == 0 || hi == 0) {
        clear_timeline();
        return;
    }
    std::erase_if(timeline_, [&](TimelineView& view) {
        if (view.id >= lo && view.id <= hi) return false;
        if (view.texture) SDL_DestroyTexture(view.texture);
        view.texture = nullptr;
        return true;
    });
    const uint64_t have = timeline_.empty() ? 0 : timeline_.back().id;
    const uint64_t from = timeline_.empty() ? lo : have + 1;
    if (from > hi) return;
    const std::vector<RewindThumb> fresh = emu_thread_.call([from, hi](EmuThread::Core& c) { return c.rewind.thumbs(from, hi); });
    for (const RewindThumb& thumb : fresh) {
        if (!timeline_.empty() && thumb.id <= timeline_.back().id) continue;
        TimelineView view;
        view.id = thumb.id;
        view.frame = thumb.frame;
        view.w = thumb.w;
        view.h = thumb.h;
        if (thumb.w > 0 && thumb.h > 0 && !thumb.pixels.empty() && main_renderer_) {
            view.texture = SDL_CreateTexture(main_renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, thumb.w, thumb.h);
            if (view.texture) {
                SDL_UpdateTexture(view.texture, nullptr, thumb.pixels.data(), thumb.w * 4);
                SDL_SetTextureScaleMode(view.texture, SDL_SCALEMODE_LINEAR);
            }
        }
        timeline_.push_back(view);
    }
}

auto App::play_from(uint64_t id) -> void {
    uint64_t frame = 0;
    for (const TimelineView& view : timeline_) {
        if (view.id == id) frame = view.frame;
    }
    const bool ok = emu_thread_.call([id](EmuThread::Core& c) {
        const auto state = c.rewind.seek(id);
        if (!state || !c.emu.load_snapshot(*state)) return false;
        c.emu.add_rerecord();  // a recording movie was cut there
        c.audio.flush();  // the sound jumps: fade the queue out and the new audio in
        c.frames_since_snapshot = 0;
        c.fast_forward_toggled = false;  // land at normal speed; the fast-forward key still works
        c.state = RunState::Running;
        c.republish = true;
        return true;
    });
    notify(ok ? "Playing from " + Emulator::play_time_text(frame) : "That moment is no longer in the timeline");
}

auto App::save_moment(uint64_t id, int slot) -> void {
    const std::string error = emu_thread_.call([id, slot](EmuThread::Core& c) {
        const auto state = c.rewind.state_at(id);
        const RewindThumb* thumb = c.rewind.thumb_at(id);
        if (!state || !thumb) return std::string("that moment is no longer in the timeline");
        const uint32_t* pixels = thumb->pixels.empty() ? nullptr : thumb->pixels.data();
        return c.emu.save_snapshot_to_slot(slot, *state, pixels, thumb->w, thumb->h);
    });
    notify(error.empty() ? "Saved that moment to slot " + std::to_string(slot) : "Save failed: " + error);
}

auto App::draw_timeline() -> void {
    sync_timeline();
    const char* hint = "Click a picture to play from there. Right-click it to save that moment to a slot.";
    if (!speedrun::race_allows(status_.race, speedrun::Guard::Timeline)) hint = kRaceTimelineHint;
    else if (!status_.rewind_enabled) hint = "Rewind is off. Enable it under Emulation > Rewind / Fast-forward.";
    else if (timeline_.empty()) hint = "It fills in as the game runs.";
    ImGui::TextUnformatted("Timeline");
    ImGui::SameLine();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled("%s", hint);
    ImGui::PopTextWrapPos();

    const float thumb_h = kThumbHeight * display_scale();
    const float child_h = timeline_block_height() - timeline_hint_height() - ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("timeline", ImVec2(0.f, child_h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar);
    if (timeline_.empty()) {
        ImGui::TextDisabled("No moments yet.");
    }
    // Stay pinned to the newest moment unless the strip has been scrolled back.
    const bool follow = ImGui::GetScrollX() >= ImGui::GetScrollMaxX() - 2.f;
    for (size_t i = 0; i < timeline_.size(); i++) {
        const TimelineView& view = timeline_[i];
        if (i > 0) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(view.id));
        const float w = view.h > 0 ? thumb_h * static_cast<float>(view.w) / static_cast<float>(view.h) : thumb_h * 8.f / 7.f;
        bool clicked = false;
        if (view.texture) {
            clicked = ImGui::ImageButton("shot", static_cast<ImTextureID>(reinterpret_cast<intptr_t>(view.texture)), ImVec2(w, thumb_h),
                                         ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), ImVec4(0.f, 0.f, 0.f, 1.f));
        } else {
            clicked = ImGui::Button("shot", ImVec2(w, thumb_h));
        }
        if (view.id == status_.timeline_newest_id) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(242, 209, 71, 255), 0.f, 0, 2.f);
        }
        if (clicked && !race_blocks(speedrun::Guard::Timeline)) play_from(view.id);
        // The strip is on the bottom edge. A menu that opens downward is clipped before the last
        // slot, so anchor the popup's bottom-left to the thumbnail's top-left.
        ImGui::SetNextWindowPos(ImGui::GetItemRectMin(), ImGuiCond_Appearing, ImVec2(0.f, 1.f));
        if (speedrun::race_allows(status_.race, speedrun::Guard::Timeline) && ImGui::BeginPopupContextItem()) {
            refresh_slots();
            ImGui::TextDisabled("Save this moment");
            ImGui::Separator();
            for (int slot = 0; slot < Emulator::kStateSlots; slot++) {
                const SlotView& saved = slots_[static_cast<size_t>(slot)];
                char label[128];
                if (saved.when.empty()) SDL_snprintf(label, sizeof(label), "Slot %d   (empty)", slot + 1);
                else SDL_snprintf(label, sizeof(label), "Slot %d   %s", slot + 1, saved.when.c_str());
                if (ImGui::MenuItem(label)) save_moment(view.id, slot + 1);
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginItemTooltip()) {
            if (view.texture) {
                const float big_h = thumb_h * 2.f;
                const float big_w = view.h > 0 ? big_h * static_cast<float>(view.w) / static_cast<float>(view.h) : big_h * 8.f / 7.f;
                ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(view.texture)), ImVec2(big_w, big_h));
            }
            ImGui::Text("Play time %s (frame %llu)", Emulator::play_time_text(view.frame).c_str(),
                        static_cast<unsigned long long>(view.frame));
            if (status_.frame_count >= view.frame) {
                const double ago = static_cast<double>(status_.frame_count - view.frame) / Emulator::kNtscFrameRate;
                ImGui::Text("%.1f s ago", ago);
            }
            ImGui::TextDisabled("Click to play from here. Right-click to save to a slot.");
            ImGui::EndTooltip();
        }
        if (follow && i + 1 == timeline_.size()) ImGui::SetScrollHereX(1.f);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

auto App::draw_status_bar() -> void {
    ImGui::Separator();
    const bool loaded = status_.loaded;
    ImGui::Text("%s  |  %.1f FPS  |  %s", loaded ? status_.rom_name.c_str() : "No ROM", loaded ? status_.fps : 0.0,
                state_name(loaded, status_.state == RunState::Paused));
    if (game_.is_open()) {
        ImGui::SameLine();
        ImGui::Text("|  %s  |  %s", game_.backend_description().c_str(), current_filter().name.c_str());
    }
    if (loaded) {
        ImGui::SameLine();
        ImGui::Text("|  Lag %llu", static_cast<unsigned long long>(status_.lag_frames));
        ImGui::SetItemTooltip("Lag frames since power-on or the last reset: frames in which the game\n"
                              "didn't read the controller, so that frame's input went unseen.");
    }
    if (loaded && status_.test_pattern) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  PPU has not drawn yet: test pattern");
    }
    if (loaded && status_.msu1) {
        ImGui::SameLine();
        ImGui::TextUnformatted("|  MSU-1");
    }
    if (status_.race) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.f), "|  RACE MODE");
        ImGui::SetItemTooltip("Race mode is on (Speedrun > Race mode).");
    }
    draw_movie_status();
    if (status_.rewinding) {
        ImGui::SameLine();
        ImGui::Text("|  << Rewinding (%.1f s left)", status_.rewind_seconds);
    } else if (status_.fast_forward) {
        ImGui::SameLine();
        ImGui::Text("|  >> Fast-forward %.1fx", status_.fast_forward_rate);
    }
    if (!status_text_.empty() && SDL_GetTicksNS() < status_until_ns_) {
        ImGui::SameLine();
        ImGui::Text("|  %s", status_text_.c_str());
    }
}

// ---- race mode -------------------------------------------------------------

auto App::request_race(bool on) -> void {
    const bool race_run = status_.timer.race_status() == speedrun::RaceStatus::Clean;
    if (on && status_.loaded) race_prompt_ = RacePrompt::TurnOn;   // the game restarts: ask
    else if (!on && race_run) race_prompt_ = RacePrompt::TurnOff;  // the run stops counting: ask
    else set_race(on);
    if (race_prompt_ != RacePrompt::None) SDL_RaiseWindow(main_window_);  // the question is in the main window
}

auto App::set_race(bool on) -> void {
    emu_thread_.call([on](EmuThread::Core& c) { c.set_race(on, EmuThread::timer_clock()); });
    status_ = emu_thread_.status();
    race_told_ = 0;
    settings_.race.enabled = on;
    save_settings();
    if (!on) notify("Race mode off");
    else notify(status_.loaded ? "Race mode on: the game restarted from power-on" : "Race mode on: the next game starts clean");
}

auto App::draw_race_prompt() -> void {
    if (race_prompt_ == RacePrompt::None) return;
    const bool turn_on = race_prompt_ == RacePrompt::TurnOn;
    const char* title = turn_on ? kRacePromptOn : kRacePromptOff;
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * kRacePromptWidthEm, 0.f));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        race_prompt_ = RacePrompt::None;
        return;
    }
    ImGui::PushTextWrapPos(0.f);
    if (turn_on) {
        ImGui::TextUnformatted("The game restarts from power-on (a hard reset), so the run starts clean.");
        if (status_.timer.state() != speedrun::TimerState::Ready) ImGui::TextUnformatted("The timer's current attempt is reset.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Until you turn race mode off, these are off:");
        for (const speedrun::GuardInfo& g : speedrun::kGuards) {
            if (!speedrun::race_allows(true, g.id)) ImGui::BulletText("%.*s", static_cast<int>(g.label.size()), g.label.data());
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("A run started now and finished in race mode is marked race verified.");
    } else {
        ImGui::TextUnformatted("The run in progress started in race mode. Turning race mode off now means it no longer counts as "
                               "race verified.");
    }
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    bool close = false;
    if (ImGui::Button(turn_on ? "Restart in race mode" : "Turn off race mode")) {
        set_race(turn_on);
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    if (close) {
        race_prompt_ = RacePrompt::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

auto App::race_blocks(speedrun::Guard guard) -> bool {
    if (speedrun::race_allows(status_.race, guard)) return false;
    if ((race_told_ & speedrun::guard_bit(guard)) == 0) {
        race_told_ |= speedrun::guard_bit(guard);
        utils::Logger::info("%s", speedrun::blocked_message(guard).c_str());
        notify(speedrun::blocked_message(guard));
    }
    return true;
}

auto App::race_tooltip(speedrun::Guard guard) const -> void {
    if (!speedrun::race_allows(status_.race, guard)) ImGui::SetItemTooltip("%s", speedrun::blocked_tooltip(guard).c_str());
}

// ---- helpers ---------------------------------------------------------------

void SDLCALL App::on_file_chosen(void* userdata, const char* const* files, int) {
    auto* app = static_cast<App*>(userdata);
    if (!files) {
        utils::Logger::error("Open ROM dialog failed: %s", SDL_GetError());
        return;
    }
    if (!files[0]) return;  // cancelled
    std::lock_guard lock(app->dialog_mutex_);
    app->dialog_result_ = files[0];
}

auto App::open_rom_dialog() -> void {
    const char* start = fs::is_directory(roms_dir_) ? roms_dir_.c_str() : nullptr;
    SDL_ShowOpenFileDialog(&App::on_file_chosen, this, main_window_, kRomFilters,
                           static_cast<int>(SDL_arraysize(kRomFilters)), start, false);
}

auto App::scan_roms() -> void {
    library_.clear();
    library_selected_ = -1;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(roms_dir_, ec)) {
        if (entry.is_regular_file(ec) && is_rom_file(entry.path())) library_.push_back(entry.path().string());
    }
    std::sort(library_.begin(), library_.end());
}

auto App::place_game_window() -> void {
    int x = 0;
    int y = 0;
    int w = 0;
    SDL_GetWindowPosition(main_window_, &x, &y);
    int game_w = 0;
    SDL_GetWindowSize(main_window_, &w, nullptr);
    SDL_GetWindowSize(game_.window(), &game_w, nullptr);

    // Side by side on the main window's display; slide the main window left if the pair would spill off.
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(main_window_), &usable)) {
        const int right = usable.x + usable.w;
        const int needed = w + kWindowGap + game_w;
        if (x + needed > right) {
            x = std::max(usable.x, right - needed);
            SDL_SetWindowPosition(main_window_, x, y);
        }
    }
    SDL_SetWindowPosition(game_.window(), x + w + kWindowGap, y);
}

auto App::view_options() const -> ViewOptions {
    return {settings_.scale, settings_.integer_scaling, settings_.aspect_8_7};
}

auto App::current_filter() const -> const video::FilterPreset& {
    return video::find_preset(filters_, settings_.filter);
}

auto App::apply_view() -> void {
    game_.apply_scale(view_options());
    save_settings();
}

auto App::update_game_title() -> void {
    shown_pattern_title_ = status_.test_pattern;
    std::string title = std::string(kMainTitle) + " - " + status_.rom_name;
    if (shown_pattern_title_) title += " (test pattern)";
    game_.set_title(title.c_str());
}

auto App::save_settings() -> void {
    if (!settings_.save(settings_path_)) utils::Logger::warning("Could not write %s", settings_path_.c_str());
}

} // namespace bitforge::frontend
