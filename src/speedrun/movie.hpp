// SPDX-License-Identifier: MIT
#pragma once
// Movies: a game's controller input, frame by frame, from power-on (or from a save state kept in
// the file), so a run can be played back exactly. 16bit Forge's own format, .bfm:
//
//   magic "BFM\x1A" | version u16 | flags u16 (MovieFlag) | standard u8 | ports u8 |
//   movie id u64 | frames u32 | rerecords u32 | ROM CRC-32 u32 | ROM SHA-1 20 bytes |
//   recorded (Unix seconds) i64 | emulator, ROM file, author, notes (u16 length + UTF-8 each) |
//   start state (u32 length + a save-state file, empty for power-on) |
//   input: u32 run count, then per run u32 frames + one FrameInput
//
// Everything is little-endian. A FrameInput is the frame's events (MovieEvent bits) and, per
// controller port, the device and four words (kDeviceWords says which a device uses). Runs of
// identical frames are stored once, so held buttons and idle stretches cost a few bytes.
// No SDL or ImGui: the emulator records and plays, the frontend shows it, the tests drive it.
#include "snes/controllers.hpp"
#include "snes/serialize.hpp"
#include "speedrun/splits.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bitforge::speedrun {

inline constexpr std::string_view kMovieExtension = ".bfm";
inline constexpr std::array<char, 4> kMovieMagic = {'B', 'F', 'M', '\x1A'};
inline constexpr uint16_t kMovieVersion = 1;
inline constexpr size_t kMoviePorts = 2;
inline constexpr size_t kPortWords = 4;

// Things that happen to the console before a frame's input is read.
enum MovieEvent : uint8_t {
  kEventReset = 1u << 0,  // the reset button
  kEventPower = 1u << 1,  // off and on again
};

// One controller port for one frame. Gamepad: words[0] is the pad ($4218 bit order); multitap:
// words[0..3] the four pads; mouse: words[0], [1] the motion (int16 x, y), words[2] the buttons
// (bit 0 left, bit 1 right); Super Scope: words[0], [1] the packed aim and buttons (pack_scope).
struct PortInput {
  Device device = Device::Gamepad;
  std::array<uint16_t, kPortWords> words{};
  auto operator==(const PortInput&) const -> bool = default;
  auto serialize(Archive& ar) -> void { ar(device, words); }
};
struct DeviceWords {
  Device device;
  size_t words;  // how many of PortInput::words it uses (the rest stay 0)
};
inline constexpr std::array<DeviceWords, 5> kDeviceWords = {{
    {Device::None, 0}, {Device::Gamepad, 1}, {Device::Multitap, kMultitapSlots}, {Device::Mouse, 3}, {Device::SuperScope, 2},
}};
auto device_words(Device device) -> size_t;

struct FrameInput {
  uint8_t events = 0;  // MovieEvent bits
  std::array<PortInput, kMoviePorts> ports{};
  auto operator==(const FrameInput&) const -> bool = default;
  auto serialize(Archive& ar) -> void { ar(events, ports); }
};

// Mouse and Super Scope input in a port's words, and back.
auto mouse_input(int dx, int dy, bool left, bool right) -> PortInput;
auto scope_input(const ScopeInput& scope) -> PortInput;
auto scope_from(const PortInput& port) -> ScopeInput;

// How a movie starts.
enum class MovieStart : uint8_t { PowerOn, SaveState };
struct MovieStartInfo {
  MovieStart id;
  std::string_view key;    // the verification summary's "start"
  std::string_view label;  // UI
};
inline constexpr std::array<MovieStartInfo, 2> kMovieStarts = {{
    {MovieStart::PowerOn, "power_on", "from power-on"},
    {MovieStart::SaveState, "save_state", "from a save state"},
}};
inline auto info(MovieStart s) -> const MovieStartInfo& { return kMovieStarts[static_cast<size_t>(s)]; }

enum MovieFlag : uint16_t {
  kFlagSaveState = 1u << 0,  // starts from the embedded state
  kFlagRace = 1u << 1,       // every frame was recorded in race mode
};

// What happens when playback reaches the end of the movie.
enum class MovieEnd : uint8_t { Pause, Stop, Live };
struct MovieEndInfo {
  MovieEnd id;
  std::string_view key;    // settings value
  std::string_view label;  // UI
};
inline constexpr std::array<MovieEndInfo, 3> kMovieEnds = {{
    {MovieEnd::Pause, "pause", "Pause (the movie stays open)"},
    {MovieEnd::Stop, "stop", "Close the movie and pause"},
    {MovieEnd::Live, "live", "Close the movie and keep playing"},
}};
inline auto info(MovieEnd e) -> const MovieEndInfo& { return kMovieEnds[static_cast<size_t>(e)]; }

struct Movie {
  uint64_t id = 0;  // tells this movie's save states from another's
  MovieStart start = MovieStart::PowerOn;
  bool race = false;  // recorded in race mode from its first frame
  VideoStandard standard = VideoStandard::Ntsc;
  uint32_t rerecords = 0;
  uint32_t rom_crc32 = 0;  // as the core loaded the ROM (no copier header)
  std::array<uint8_t, 20> rom_sha1{};  // of the ROM file
  int64_t recorded = 0;    // Unix seconds, when recording started
  std::string emulator;    // "16bit Forge <version>"
  std::string rom_file;
  std::string author;
  std::string notes;
  std::vector<uint8_t> state;  // a save-state file (start == SaveState)
  std::vector<FrameInput> frames;

  // A movie that can stand behind a race verified run: race mode from power-on, no rerecords.
  auto verifiable() const -> bool { return start == MovieStart::PowerOn && race && rerecords == 0; }
};
auto new_movie_id() -> uint64_t;

enum class MovieResult : uint8_t { Ok, NotAMovie, WrongVersion, Corrupt };
auto describe(MovieResult result) -> const char*;

// The file's bytes, with only the first `frames` frames (all by default).
auto encode_movie(const Movie& movie, size_t frames = SIZE_MAX) -> std::vector<uint8_t>;
auto decode_movie(std::span<const uint8_t> data, Movie& movie) -> MovieResult;
// Whole files. read_movie: "" or why it failed; write_movie: false if it can't be written.
auto read_movie(const std::string& path, Movie& movie) -> std::string;
auto write_movie(const std::string& path, const Movie& movie, size_t frames = SIZE_MAX) -> bool;

// What differs between a movie and what is running, warned about before playback: a bit set.
enum MovieMismatch : uint8_t { kMismatchRom = 1, kMismatchEmulator = 2 };
auto movie_mismatch(const Movie& movie, uint32_t rom_crc32, const std::array<uint8_t, 20>& rom_sha1, std::string_view emulator)
    -> uint8_t;
// The warning for a mismatch (1..3): one dialog, whatever differs.
struct MismatchPrompt {
  const char* title;
  const char* text;
};
auto mismatch_prompt(uint8_t mismatch) -> const MismatchPrompt&;

// True when `part`'s frames are the start of `whole`'s.
auto is_prefix(std::span<const FrameInput> part, std::span<const FrameInput> whole) -> bool;
// Where a save state's movie goes: "states/Game.3.sst" -> "states/Game.3.bfm".
auto state_movie_path(std::string_view state_path) -> std::string;
// A race verified attempt's movie: <data dir>/movies/<rom>/attempt-<n>.bfm
auto attempt_movie_path(std::string_view data_dir, std::string_view rom_stem, int attempt) -> std::string;
// "1234" while recording, "1234 / 5000" while playing: the frame counter text.
auto movie_counter(uint64_t frame, uint64_t frames, bool playing) -> std::string;

}  // namespace bitforge::speedrun
