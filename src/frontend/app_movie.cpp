// SPDX-License-Identifier: MIT
// The app's movie side (speedrun/movie.hpp): the Movie menu, its file dialogs, the prompt for a
// movie made with another ROM, the status bar entry and the game window's REC / PLAY badge.
// Recording and playback themselves run on the emulation thread (Emulator, EmuThread::Core).
#include "frontend/app.hpp"
#include "speedrun/race.hpp"
#include "utils/logging.hpp"

#include "imgui.h"

#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace bitforge::frontend {

namespace {

constexpr SDL_DialogFileFilter kMovieFilters[] = {
    {"16bit Forge movies", "bfm"},
    {"All files", "*"},
};
constexpr const char* kMoviesFolder = "movies";
constexpr const char* kMoviePromptId = "###movie_prompt";  // one popup, its title per mismatch
constexpr float kMoviePromptWidthEm = 40.f;

// How each mode looks: its badge (status bar, game window) and colour.
struct MovieLook {
    MovieMode mode;
    const char* tag;
    uint32_t argb;
};
constexpr std::array<MovieLook, 3> kMovieLooks = {{
    {MovieMode::Off, "", 0},
    {MovieMode::Recording, "REC", 0xFFFF5A5A},
    {MovieMode::Playing, "PLAY", 0xFF6EB4FF},
}};
constexpr uint32_t kMovieEndColor = 0xFFB4B4B4;
constexpr const char* kMovieEndTag = "END";

auto look(MovieMode mode) -> const MovieLook& { return kMovieLooks[static_cast<size_t>(mode)]; }

auto imgui_color(uint32_t argb) -> ImVec4 {
    return ImVec4(static_cast<float>((argb >> 16) & 0xFF) / 255.f, static_cast<float>((argb >> 8) & 0xFF) / 255.f,
                  static_cast<float>(argb & 0xFF) / 255.f, 1.f);
}

auto hex(const std::array<uint8_t, 20>& bytes) -> std::string {
    std::string out;
    char two[3];
    for (const uint8_t b : bytes) {
        std::snprintf(two, sizeof(two), "%02x", b);
        out += two;
    }
    return out;
}

// "REC 1234", "PLAY 1234 / 5000", "END 5000 / 5000"; empty without a movie.
auto movie_tag(const EmuStatus& s) -> std::pair<std::string, uint32_t> {
    if (s.movie.mode == MovieMode::Off) return {};
    const bool playing = s.movie.mode == MovieMode::Playing;
    const MovieLook& l = look(s.movie.mode);
    const char* tag = s.movie_at_end ? kMovieEndTag : l.tag;
    return {std::string(tag) + " " + speedrun::movie_counter(s.movie.frame, s.movie.frames, playing), s.movie_at_end ? kMovieEndColor : l.argb};
}

}  // namespace

auto App::running_version() -> std::string { return std::string(BITFORGE_TITLE) + " " + BITFORGE_VERSION; }

auto App::apply_movie_settings() -> void {
    emu_thread_.post([m = settings_.movie](EmuThread::Core& c) {
        c.movie_end = m.at_end;
        c.attempt_movies = m.attempt_movies;
        c.emu.set_movie_read_only(m.read_only);
    });
}

auto App::draw_movie_menu() -> void {
    if (!ImGui::BeginMenu("Movie")) return;
    const bool loaded = status_.loaded;
    const MovieMode mode = status_.movie.mode;
    if (ImGui::MenuItem("Record from power-on...", nullptr, false, loaded)) show_movie_dialog(MovieDialog::RecordPowerOn);
    ImGui::SetItemTooltip("Restarts the game from power-on with a blank battery save and records every frame's input. "
                          "Recorded in race mode from the start, a race verified run keeps its movie.");
    const bool can_snapshot = speedrun::race_allows(status_.race, speedrun::Guard::SaveState);
    if (ImGui::MenuItem("Record from here...", nullptr, false, loaded && can_snapshot)) show_movie_dialog(MovieDialog::RecordState);
    if (can_snapshot) {
        ImGui::SetItemTooltip("Records from the game as it is now: the movie starts with a save state of this moment, so it "
                              "can't back a race verified run.");
    }
    race_tooltip(speedrun::Guard::SaveState);
    if (ImGui::MenuItem("Play...", nullptr, false, loaded)) show_movie_dialog(MovieDialog::Play);
    ImGui::SetItemTooltip("Plays a movie: your input is ignored until it ends. A run it plays counts as a replay.");
    const char* stop = mode == MovieMode::Recording ? "Stop recording" : "Stop playback";
    if (ImGui::MenuItem(mode == MovieMode::Off ? "Stop" : stop, nullptr, false, mode != MovieMode::Off)) stop_movie();
    ImGui::Separator();
    if (ImGui::MenuItem("Read-only", nullptr, &settings_.movie.read_only)) {
        apply_movie_settings();
        save_settings();
    }
    ImGui::SetItemTooltip("On: loading a state (or rewinding) during a movie keeps playing it from there. Off: the movie is cut "
                          "there and recording carries on (a rerecord).");
    if (ImGui::BeginMenu("At the end of playback")) {
        for (const speedrun::MovieEndInfo& e : speedrun::kMovieEnds) {
            if (ImGui::MenuItem(e.label.data(), nullptr, settings_.movie.at_end == e.id)) {
                settings_.movie.at_end = e.id;
                apply_movie_settings();
                save_settings();
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Keep the movie of a race verified run", nullptr, &settings_.movie.attempt_movies)) {
        apply_movie_settings();
        save_settings();
    }
    ImGui::SetItemTooltip("movies/<rom>/attempt-<n>.bfm, named with its SHA-1 in the run's verification file.");
    if (ImGui::MenuItem("Badge in the game window", nullptr, &settings_.movie.overlay)) save_settings();
    ImGui::EndMenu();
}

// ---- File dialogs (SDL may call back on another thread) -----------------------------------------

auto App::show_movie_dialog(MovieDialog kind) -> void {
    {
        std::lock_guard lock(dialog_mutex_);
        if (movie_dialog_ != MovieDialog::None) return;  // one at a time
        movie_dialog_ = kind;
    }
    std::error_code ec;
    const fs::path folder = fs::path(base_dir_) / kMoviesFolder / fs::path(status_.rom_path).stem();
    fs::create_directories(folder, ec);
    const std::string location = folder.string();
    const int filters = static_cast<int>(std::size(kMovieFilters));
    if (kind == MovieDialog::Play) {
        SDL_ShowOpenFileDialog(&App::on_movie_chosen, this, main_window_, kMovieFilters, filters, location.c_str(), false);
    } else {
        SDL_ShowSaveFileDialog(&App::on_movie_chosen, this, main_window_, kMovieFilters, filters, location.c_str());
    }
}

void SDLCALL App::on_movie_chosen(void* userdata, const char* const* files, int) {
    auto* app = static_cast<App*>(userdata);
    std::lock_guard lock(app->dialog_mutex_);
    if (!files || !files[0]) {  // cancelled or failed
        if (!files) utils::Logger::error("Movie dialog failed: %s", SDL_GetError());
        app->movie_dialog_ = MovieDialog::None;
        return;
    }
    std::string path = files[0];
    if (app->movie_dialog_ != MovieDialog::Play && fs::path(path).extension().empty()) path += speedrun::kMovieExtension;
    app->movie_dialog_path_ = std::move(path);
}

auto App::take_movie_dialog() -> void {
    MovieDialog kind;
    std::string path;
    {
        std::lock_guard lock(dialog_mutex_);
        if (!movie_dialog_path_) return;
        kind = std::exchange(movie_dialog_, MovieDialog::None);
        path = std::move(*movie_dialog_path_);
        movie_dialog_path_.reset();
    }
    if (kind == MovieDialog::Play) open_movie(path);
    else record_movie(kind == MovieDialog::RecordPowerOn ? speedrun::MovieStart::PowerOn : speedrun::MovieStart::SaveState, path);
}

// ---- Commands -----------------------------------------------------------------------------------

auto App::record_movie(speedrun::MovieStart start, const std::string& path) -> void {
    const std::string author = settings_.movie.author;
    const std::string why = emu_thread_.call([start, path, author](EmuThread::Core& c) {
        return c.record_movie(start, path, author, EmuThread::timer_clock());
    });
    status_ = emu_thread_.status();
    if (status_.movie.mode != MovieMode::Recording) {
        notify("Can't record: " + why);
        return;
    }
    if (!why.empty()) utils::Logger::error("Movie: %s", why.c_str());  // the one before it
    notify("Recording a movie " + std::string(speedrun::info(start).label) + ": " + fs::path(path).filename().string());
}

// Reads the file; a movie made with another ROM or emulator version asks first.
auto App::open_movie(const std::string& path) -> void {
    speedrun::Movie movie;
    if (const std::string why = speedrun::read_movie(path, movie); !why.empty()) {
        notify("Can't play " + why);
        return;
    }
    if (const uint8_t mismatch = speedrun::movie_mismatch(movie, status_.rom_crc32, status_.rom_sha1, running_version()); mismatch != 0) {
        movie_prompt_ = PendingMovie{std::move(movie), path, mismatch};
        return;
    }
    play_movie(std::move(movie), path);
}

auto App::play_movie(speedrun::Movie movie, const std::string& path) -> void {
    const std::string why = emu_thread_.call([&movie, &path](EmuThread::Core& c) {
        return c.play_movie(std::move(movie), path, EmuThread::timer_clock());
    });
    status_ = emu_thread_.status();
    if (status_.movie.mode != MovieMode::Playing) {
        notify("Can't play the movie: " + why);
        return;
    }
    notify("Playing " + fs::path(path).filename().string() + " (" + std::to_string(status_.movie.frames) + " frames)");
}

auto App::stop_movie() -> void {
    const MovieStatus before = status_.movie;
    const std::string why = emu_thread_.call([](EmuThread::Core& c) { return c.emu.stop_movie(); });
    status_ = emu_thread_.status();
    if (!why.empty()) notify("Movie: " + why);
    else if (before.mode == MovieMode::Recording) notify("Movie saved: " + fs::path(before.path).filename().string());
    else notify("Playback stopped");
}

// Says when playback reaches the end of a movie.
auto App::watch_movie() -> void {
    if (status_.movie_ends > movie_ends_seen_) notify("Movie: the end (" + std::string(speedrun::info(settings_.movie.at_end).label) + ")");
    movie_ends_seen_ = status_.movie_ends;
}

auto App::draw_movie_prompt() -> void {
    if (!movie_prompt_) return;
    const speedrun::MismatchPrompt& prompt = speedrun::mismatch_prompt(movie_prompt_->mismatch);
    const std::string title = std::string(prompt.title) + kMoviePromptId;
    if (!ImGui::IsPopupOpen(kMoviePromptId)) ImGui::OpenPopup(title.c_str());
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * kMoviePromptWidthEm, 0.f));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        movie_prompt_.reset();
        return;
    }
    const speedrun::Movie& m = movie_prompt_->movie;
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(prompt.text);
    ImGui::Spacing();
    if (movie_prompt_->mismatch & speedrun::kMismatchRom) {
        ImGui::Text("Movie ROM:     %s", m.rom_file.c_str());
        ImGui::Text("               CRC32 %08X", m.rom_crc32);
        ImGui::Text("               SHA-1 %s", hex(m.rom_sha1).c_str());
        ImGui::Text("Running ROM:   %s", status_.rom_name.c_str());
        ImGui::Text("               CRC32 %08X", status_.rom_crc32);
        ImGui::Text("               SHA-1 %s", hex(status_.rom_sha1).c_str());
    }
    if (movie_prompt_->mismatch & speedrun::kMismatchEmulator) {
        ImGui::Text("Recorded with: %s", m.emulator.empty() ? "(unknown)" : m.emulator.c_str());
        ImGui::Text("Running:       %s", running_version().c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    bool close = false;
    if (ImGui::Button("Play anyway")) {
        PendingMovie pending = std::move(*movie_prompt_);
        play_movie(std::move(pending.movie), pending.path);
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    if (close) {
        movie_prompt_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- Indicators ---------------------------------------------------------------------------------

auto App::draw_movie_status() -> void {
    const auto [tag, argb] = movie_tag(status_);
    if (tag.empty()) return;
    const MovieStatus& m = status_.movie;
    ImGui::SameLine();
    ImGui::TextColored(imgui_color(argb), "|  %s", tag.c_str());
    std::string help = fs::path(m.path).filename().string() + ", " + std::string(speedrun::info(m.start).label) + ", " +
                       std::to_string(m.rerecords) + " rerecords" + (m.read_only ? ", read-only" : ", read+write");
    if (m.mode == MovieMode::Recording) help += m.race ? ". Race mode throughout so far." : ".";
    ImGui::SetItemTooltip("%s", help.c_str());
}

// The badge goes in the corner below (or above) the timer overlay.
auto App::movie_badge(int& width, int& height, Corner& corner) const -> std::vector<uint32_t> {
    width = height = 0;
    const CornerInfo& timer = info(settings_.timer.corner);
    for (size_t i = 0; i < kCorners.size(); i++) {
        if (kCorners[i].right == timer.right && kCorners[i].bottom != timer.bottom) corner = static_cast<Corner>(i);
    }
    const auto [tag, argb] = movie_tag(status_);
    if (!settings_.movie.overlay || tag.empty()) return {};
    const TextLine line{tag, argb, 1};
    return render_text_block(std::span(&line, 1), info(corner).right, 0, width, height);
}

}  // namespace bitforge::frontend
