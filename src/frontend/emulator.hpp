// SPDX-License-Identifier: MIT
#pragma once
#include "debug/memory_watch.hpp"
#include "snes/cheats.hpp"
#include "snes/controllers.hpp"
#include "speedrun/movie.hpp"
#include "speedrun/movie_journal.hpp"
#include "speedrun/run_timer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bitforge {
class System;
namespace savestate {
struct Info;
}
}

namespace bitforge::frontend {

enum class MovieMode : uint8_t { Off, Recording, Playing };

// A movie as the UI shows it.
struct MovieStatus {
    MovieMode mode = MovieMode::Off;
    uint64_t frame = 0;   // frames recorded or played so far
    uint64_t frames = 0;  // the movie's length
    uint32_t rerecords = 0;
    bool read_only = true;
    bool race = false;    // recorded in race mode from its first frame
    speedrun::MovieStart start = speedrun::MovieStart::PowerOn;
    bool journal = false; // recorded since it opened: Stop writes the file
    std::string path;
};

// The only surface the UI sees of the core. No SDL or ImGui types cross this line.
class Emulator {
public:
    // NTSC master clock 21.477272 MHz / (262 lines * 1364 clocks).
    static constexpr double kNtscFrameRate = 60.0988;

    // Save-state slots 1..kStateSlots (F1-F10). SRAM is flushed this often while it changes.
    static constexpr int kStateSlots = 10;
    static constexpr uint64_t kSramFlushFrames = 300;  // ~5 s

    Emulator();
    ~Emulator();  // writes battery RAM

    // Where states/ and saves/ go (next to the executable). Empty = current directory.
    auto set_data_dir(const std::string& dir) -> void { data_dir_ = dir; }
    auto data_dir() const -> const std::string& { return data_dir_; }

    // Coprocessor firmware is looked up in <data dir>/firmware. After a failed load,
    // last_error() says why (e.g. which firmware file is missing and its size); after a
    // successful one, last_warning() notes a chip that isn't emulated.
    // Loading a game ends a movie.
    auto load(const std::string& path) -> bool;
    auto last_error() const -> const std::string& { return last_error_; }
    auto last_warning() const -> const std::string& { return last_warning_; }
    auto unload() -> void;
    auto reset() -> void;
    // Power off and on: the ROM boots again from its file (fresh memory, the battery save as on
    // disk, watches unfrozen, no hand edits). False if it can't be loaded any more.
    auto power_cycle() -> bool;

    // ---- Race mode (speedrun/race.hpp) ----------------------------------------------------
    // Turning it on power-cycles the game. While on, cheats aren't applied (the list and its
    // file stay), freezes are dropped, and memory edits, state loads and saves, timeline saves
    // and snapshot loads (rewind) are refused, whatever asks. Turning it off applies the
    // cheats again.
    auto set_race(bool on) -> void;
    auto race() const -> bool { return race_; }
    auto run_frame() -> void;

    auto loaded() const -> bool { return system_ != nullptr; }
    auto rom_path() const -> const std::string& { return rom_path_; }
    auto rom_name() const -> const std::string& { return rom_name_; }
    auto rom_stem() const -> std::string;  // the ROM's file name without its extension
    auto rom_crc32() const -> uint32_t;    // of the ROM as loaded (no copier header); 0 without one
    auto rom_sha1() const -> const std::array<uint8_t, 20>& { return rom_sha1_; }  // of the ROM file
    auto rom_sha1_hex() const -> std::string;
    auto frame_count() const -> uint64_t { return frames_; }
    // What last moved frame_count() other than a frame (a reset, a state load, a rewind or
    // timeline jump), cleared by the call: the run timer keeps game time and auto-splits right.
    auto take_jump() -> speedrun::Jump { return std::exchange(jump_, speedrun::Jump::None); }
    // The frame rate game time counts at. The core runs NTSC timing (262 lines) for every
    // cartridge, so this is NTSC until PAL timing is emulated.
    auto video_standard() const -> speedrun::VideoStandard { return speedrun::VideoStandard::Ntsc; }

    // XRGB8888, row-major, width() * height() pixels. Always valid while loaded. The size can
    // change between frames (256/512 x 224/239/448/478); the video backends rebuild on a change.
    auto framebuffer() const -> const uint32_t*;
    auto width() const -> int { return width_; }
    auto height() const -> int { return height_; }

    // True until the PPU has produced a non-black pixel; framebuffer() is a test pattern then.
    auto showing_test_pattern() const -> bool { return !seen_output_; }

    // Input goes to the ports at the start of the next frame (unless a movie is playing).
    // Controller `port` (0 = pad 1, 1 = pad 2) in $4218/$4219 bit order.
    auto set_joypad(int port, uint16_t state) -> void;
    // Pad 2 only reads as plugged in while a gamepad is assigned to it.
    auto set_port_connected(int port, bool connected) -> void;
    // Multitap, SNES Mouse and Super Scope (see bitforge::ControllerPort). No-ops without a game.
    auto set_port_device(int port, bitforge::Device device) -> void;
    auto set_pad(int port, int slot, uint16_t state) -> void;
    auto add_mouse(int port, int dx, int dy, bool left, bool right) -> void;
    auto set_scope(int port, const bitforge::ScopeInput& input) -> void;

    // ---- Movies (speedrun/movie.hpp) ------------------------------------------------------
    // While one records, every frame's input (and a reset or power cycle before it) is added to
    // it; while one plays, its input drives the game and what the frontend sets is ignored (a
    // reset too). Starting one power-cycles the game, or loads the movie's state. From then
    // until another game is loaded the battery save is neither read nor written, so a movie
    // always starts from the same blank save and never overwrites the real one.
    //
    // A recording is journaled to <path>.part every flush interval (speedrun/movie_journal.hpp),
    // so a crash loses at most that much; Stop writes the movie and deletes the journal.
    //
    // Save states made meanwhile get a reference to the movie up to that frame
    // (states/<rom>.<slot>.bfs). Loading one, or rewinding, goes back in the movie: in read-only
    // mode playback carries on from there (the state must be on the take playing); otherwise the
    // movie is cut there and recording carries on (a rerecord), also from a take cut earlier in
    // this recording (the journal still has it).
    struct MovieMeta {
        std::string emulator;  // "16bit Forge <version>"
        std::string author;
    };
    // `path` is where stop_movie() writes it. "" or why it can't start.
    auto record_movie(speedrun::MovieStart start, const std::string& path, const MovieMeta& meta) -> std::string;
    auto play_movie(speedrun::Movie movie, const std::string& path) -> std::string;
    // Ends it; a recording is written to its file first. "" or why the file couldn't be written.
    auto stop_movie() -> std::string;
    auto movie_mode() const -> MovieMode { return movie_mode_; }
    auto movie() const -> const speedrun::Movie& { return movie_; }
    auto movie_frame() const -> uint64_t { return movie_cursor_; }
    auto movie_status() const -> MovieStatus;
    // Playing, and every frame of it has been played.
    auto movie_at_end() const -> bool { return movie_mode_ == MovieMode::Playing && movie_cursor_ >= movie_.frames.size(); }
    auto set_movie_read_only(bool on) -> void { movie_read_only_ = on; }
    // How often new input goes to the journal, in frames (60: about a second).
    auto set_movie_flush_frames(uint32_t frames) -> void { flush_frames_ = std::clamp<uint32_t>(frames, 1, speedrun::kMaxFlushFrames); }
    // Hands the input recorded since the last time to the journal (run_frame() does, every flush
    // interval); `wait`: and waits until it is on disk.
    auto flush_movie(bool wait = false) -> void;
    // A rewind or timeline jump while recording cut the movie: counts once per jump.
    auto add_rerecord() -> void;

    // ---- Save states: states/<rom>.<slot>.sst ---------------------------------------------
    struct SlotInfo {
        int64_t timestamp = 0;  // seconds since 1970
        uint64_t frame = 0;     // frames run since boot when saved
        uint32_t version = 0;   // save-state file version
        int thumb_w = 0;
        int thumb_h = 0;
        std::vector<uint32_t> thumbnail;  // XRGB8888
    };
    auto state_path(int slot) const -> std::string;
    // The same without an Emulator (the UI thread lists slots from the published ROM path).
    static auto state_path_for(const std::string& data_dir, const std::string& rom_path, int slot) -> std::string;
    // Returns an empty string on success, else why it failed (for the UI).
    auto save_state(int slot) -> std::string;
    auto load_state(int slot) -> std::string;
    // Header and thumbnail of a slot's file, if it exists and belongs to this emulator version.
    auto slot_info(int slot) const -> std::optional<SlotInfo>;
    // In-memory snapshot for rewind: the frame counter, the movie frame and the machine (no
    // header or thumbnail).
    auto save_snapshot() -> std::vector<uint8_t>;
    auto load_snapshot(std::span<const uint8_t> data) -> bool;
    // Writes slot `slot` from a rewind snapshot (frame counter, then the machine) and that
    // moment's picture, without loading it. The running game stays where it is.
    auto save_snapshot_to_slot(int slot, std::span<const uint8_t> snapshot, const uint32_t* thumb, int thumb_w,
                               int thumb_h) -> std::string;
    static auto read_slot_info(const std::string& path) -> std::optional<SlotInfo>;
    // For the slot menu: "just now", "5 min ago", "3 h ago", "2 days ago" (`now` and `saved` in
    // Unix seconds), and "1:02:03" for a frame count at the NTSC rate.
    static auto age_text(int64_t saved, int64_t now) -> std::string;
    static auto play_time_text(uint64_t frames) -> std::string;

    // ---- Battery RAM: saves/<rom>.srm, loaded at boot, written when dirty --------------------
    auto sram_path() const -> std::string;
    auto flush_sram() -> void;  // writes the file if SRAM changed since the last write, and the .rtc clock file
    auto rtc_path() const -> std::string;

    // ---- Cheats: cheats/<rom>.cht, loaded at boot -----------------------------------------
    auto cheats_path() const -> std::string;
    auto cheats() const -> const std::vector<CheatEntry>& { return cheats_; }
    // Applies the list (enabled entries, from the next bus read) and writes the file.
    auto set_cheats(std::vector<CheatEntry> entries) -> bool;

    // ---- Memory watches: watches/<rom>.wch, loaded at boot ---------------------------------
    // Frozen watches are written at the start of every frame (as Pro Action Replay codes are),
    // so they hold through rewind, timeline jumps and state loads. Freezes last until the game
    // is closed: the file keeps the watches, and they load unfrozen.
    auto watches_path() const -> std::string;
    auto watches() const -> const std::vector<debug::Watch>& { return watches_; }
    // Counts changes to the list (a load, set_watches), so the UI's copies know to refetch it.
    auto watches_revision() const -> uint64_t { return watches_revision_; }
    // Applies the list (frozen values are written right away) and writes the file.
    auto set_watches(std::vector<debug::Watch> entries) -> bool;
    // A region's memory (WRAM, or the cartridge's SRAM; empty when there is none or no game).
    auto memory(debug::Region region) -> std::span<uint8_t>;
    // One write of `value` at `offset` (a watch edit). False if it doesn't fit the region.
    auto write_memory(debug::Region region, uint32_t offset, debug::ValueSize size, uint32_t value) -> bool;
    // Every watch read once at the end of each frame, on this (the emulation) thread: the hook
    // for a run timer or auto-splitter. A state load, rewind or reset starts the readings over
    // (before == now), so a jump never looks like a change.
    auto watch_readings() const -> std::span<const debug::WatchReading> { return readings_; }
    auto evaluate(const debug::Condition& condition) const -> bool;
    // debug::TamperBits: a frozen watch, an enabled cheat, a hand edit since the game loaded.
    auto memory_tampered() const -> uint32_t;
    // The run timer's per-frame hook: call after every run_frame(). Game time and the
    // auto-split rules see this frame's readings and whatever jump came before it.
    auto step_timer(speedrun::RunTimer& timer, const speedrun::Clock& now) -> speedrun::Event;

    // Stereo int16 frames (L, R, L, R, ...) the last run_frame() produced, at audio_sample_rate().
    auto audio_samples() const -> std::span<const int16_t>;
    static auto audio_sample_rate() -> int;  // 32040 Hz
    // The MSU-1's audio for the same frame (as many samples, same rate); empty without one.
    auto msu1_samples() const -> std::span<const int16_t>;
    auto has_msu1() const -> bool;

private:
    auto boot(const std::string& path, bool battery) -> bool;
    auto close_game() -> void;     // unload() without ending the movie
    auto reset_machine() -> void;  // reset() without recording it
    auto update_test_pattern() -> void;
    auto restore(std::span<const uint8_t> file, std::string& why) -> bool;  // a save-state file
    auto next_input() -> speedrun::FrameInput;  // this frame's, from the movie or the frontend
    auto apply_input(const speedrun::FrameInput& in) -> void;
    auto capture_input() -> void;  // input_ = what the ports hold (after a boot)
    auto rewind_movie(uint64_t frame) -> void;  // a snapshot load moved back to `frame`
    auto cut_movie(size_t frame) -> void;       // recording from `frame`: the rest goes
    auto start_journal() -> void;               // recording: journal it, if not yet
    // The take a reference is on, from an old sidecar or this recording's journal: false if it is gone.
    auto take_of(const speedrun::StateRef& ref, std::vector<speedrun::FrameInput>& frames) -> bool;
    auto state_ref(size_t frame) -> speedrun::StateRef;  // to this movie's first `frame` frames
    auto load_sram() -> void;
    auto load_cheats() -> void;
    auto load_watches() -> void;
    auto apply_cheats() -> void;  // the enabled entries to the core, unless race mode is on
    auto apply_freezes() -> void;
    auto region_memory() -> debug::RegionMemory;
    auto sample_watches() -> void;

    std::unique_ptr<System> system_;
    std::vector<CheatEntry> cheats_;
    std::vector<debug::Watch> watches_;
    std::vector<debug::WatchReading> readings_;       // this frame's
    std::vector<debug::WatchReading> last_readings_;  // last frame's (kept to reuse its storage)
    bool poked_ = false;                              // a hand edit since the game loaded
    bool race_ = false;
    bool battery_off_ = false;                        // a movie: no battery file read or written
    std::array<uint8_t, 20> rom_sha1_{};
    speedrun::FrameInput input_;                      // the frontend's, applied at the next frame
    uint8_t pending_events_ = 0;                      // a reset or power cycle to record
    speedrun::Movie movie_;
    MovieMode movie_mode_ = MovieMode::Off;
    size_t movie_cursor_ = 0;                         // the next frame to record or play
    std::string movie_path_;
    bool movie_read_only_ = true;
    speedrun::JournalWriter journal_;                 // while (and since) the movie records
    size_t journal_frames_ = 0;                       // the journal has the movie's frames up to here
    uint32_t journal_rerecords_ = 0;                  // and these rerecords
    bool journal_race_ = false;                       // and this race flag
    uint32_t flush_frames_ = speedrun::kDefaultFlushFrames;
    uint32_t unflushed_ = 0;                          // frames since the last flush
    uint64_t watches_revision_ = 0;
    speedrun::Jump jump_ = speedrun::Jump::None;
    std::string data_dir_;
    std::string last_error_;
    std::string last_warning_;
    std::string rom_path_;
    std::string rom_name_;
    std::vector<uint32_t> test_pattern_;
    int width_ = 0;
    int height_ = 0;
    uint64_t frames_ = 0;
    bool seen_output_ = false;
};

} // namespace bitforge::frontend
