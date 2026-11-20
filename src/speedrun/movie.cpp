// SPDX-License-Identifier: MIT
#include "speedrun/movie.hpp"
#include "utils/sha1.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>

namespace bitforge::speedrun {

namespace {

constexpr const char* kMoviesFolder = "movies";
constexpr uint32_t kScopeHighShift = 16;

// Strings and blocks: a length, then the bytes.
template <class Length, class Bytes>
auto io_block(Archive& ar, Bytes& bytes) -> void {
  Length n = static_cast<Length>(std::min<size_t>(bytes.size(), std::numeric_limits<Length>::max()));
  ar(n);
  if (ar.loading()) {
    if (!ar.ok() || n > ar.remaining()) return ar.fail();
    bytes.resize(n);
  }
  ar.bytes(bytes.data(), n);
}

auto known_device(Device d) -> bool {
  return std::any_of(kDeviceWords.begin(), kDeviceWords.end(), [d](const DeviceWords& w) { return w.device == d; });
}

// The fixed part and the strings, in file order; the same code saves and loads.
auto io_header(Archive& ar, Movie& m, uint32_t& frames) -> void {
  uint8_t ports = kMoviePorts;
  ar(m.standard, ports, m.id, frames, m.rerecords, m.rom_crc32, m.rom_sha1, m.recorded);
  if (ports != kMoviePorts) ar.fail();
  for (std::string* s : {&m.emulator, &m.rom_file, &m.author, &m.notes}) io_block<uint16_t>(ar, *s);
  io_block<uint32_t>(ar, m.state);
}

auto read_file(const std::string& path) -> std::optional<std::vector<uint8_t>> {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Written to <path>.tmp and renamed, so a crash mid-write never leaves half a file.
auto write_file(const std::string& path, std::span<const uint8_t> data) -> bool {
  std::error_code ec;
  if (const auto dir = std::filesystem::path(path).parent_path(); !dir.empty()) std::filesystem::create_directories(dir, ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) return false;
  }
  std::filesystem::rename(tmp, path, ec);
  if (!ec) return true;
  std::filesystem::remove(tmp, ec);
  return false;
}

}  // namespace

auto device_words(Device device) -> size_t {
  for (const DeviceWords& w : kDeviceWords) {
    if (w.device == device) return w.words;
  }
  return 0;
}

auto mouse_input(int dx, int dy, bool left, bool right) -> PortInput {
  auto clamp16 = [](int v) { return static_cast<uint16_t>(static_cast<int16_t>(std::clamp(v, -32768, 32767))); };
  return {Device::Mouse, {clamp16(dx), clamp16(dy), static_cast<uint16_t>((left ? 1u : 0u) | (right ? 2u : 0u)), 0}};
}

// The aim and buttons in 23 bits: x (0-8), y (9-17), offscreen, fire, cursor, turbo, pause.
auto scope_input(const ScopeInput& s) -> PortInput {
  const uint32_t v = static_cast<uint32_t>(s.x & 0x1FF) | static_cast<uint32_t>(s.y & 0x1FF) << 9 | (s.offscreen ? 1u << 18 : 0u) |
                     (s.fire ? 1u << 19 : 0u) | (s.cursor ? 1u << 20 : 0u) | (s.turbo ? 1u << 21 : 0u) | (s.pause ? 1u << 22 : 0u);
  return {Device::SuperScope, {static_cast<uint16_t>(v), static_cast<uint16_t>(v >> kScopeHighShift), 0, 0}};
}

auto scope_from(const PortInput& port) -> ScopeInput {
  const uint32_t v = port.words[0] | static_cast<uint32_t>(port.words[1]) << kScopeHighShift;
  ScopeInput s;
  s.x = static_cast<int>(v & 0x1FF);
  s.y = static_cast<int>((v >> 9) & 0x1FF);
  s.offscreen = (v >> 18) & 1;
  s.fire = (v >> 19) & 1;
  s.cursor = (v >> 20) & 1;
  s.turbo = (v >> 21) & 1;
  s.pause = (v >> 22) & 1;
  return s;
}

auto new_movie_id() -> uint64_t {
  std::random_device rd;
  const uint64_t now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  return (static_cast<uint64_t>(rd()) << 32 ^ rd() ^ now) | 1;  // never 0
}

auto describe(MovieResult result) -> const char* {
  switch (result) {
  case MovieResult::Ok: return "ok";
  case MovieResult::NotAMovie: return "not a 16bit Forge movie";
  case MovieResult::WrongVersion: return "made by a newer version";
  case MovieResult::Corrupt: return "the file is damaged";
  }
  return "?";
}

auto encode_movie(const Movie& movie, size_t frames) -> std::vector<uint8_t> {
  Movie m = movie;  // io_header takes it by reference; nothing is changed when saving
  const size_t count = std::min(frames, m.frames.size());
  Archive ar = Archive::saver();
  std::array<char, 4> magic = kMovieMagic;
  uint16_t version = kMovieVersion;
  uint16_t flags = static_cast<uint16_t>((m.start == MovieStart::SaveState ? kFlagSaveState : 0) | (m.race ? kFlagRace : 0));
  uint32_t n = static_cast<uint32_t>(count);
  ar(magic, version, flags);
  io_header(ar, m, n);
  write_runs(ar, std::span(m.frames).first(count));
  return ar.data();
}

// Runs of identical frames.
auto write_runs(Archive& ar, std::span<const FrameInput> frames) -> void {
  std::vector<std::pair<uint32_t, size_t>> runs;  // length, first frame
  for (size_t i = 0; i < frames.size(); i++) {
    if (!runs.empty() && frames[i] == frames[runs.back().second]) {
      runs.back().first++;
    } else {
      runs.push_back({1, i});
    }
  }
  uint32_t run_count = static_cast<uint32_t>(runs.size());
  ar(run_count);
  for (auto& [length, first] : runs) {
    FrameInput in = frames[first];
    ar(length, in);
  }
}

auto read_runs(Archive& ar, std::vector<FrameInput>& frames, uint64_t limit) -> bool {
  uint32_t run_count = 0;
  ar(run_count);
  if (!ar.ok() || frames.size() > limit || run_count > limit - frames.size()) return false;
  for (uint32_t r = 0; r < run_count; r++) {
    uint32_t length = 0;
    FrameInput in;
    ar(length, in);
    if (!ar.ok() || length == 0 || length > limit - frames.size()) return false;
    for (PortInput& p : in.ports) {
      if (!known_device(p.device)) return false;
      std::fill(p.words.begin() + static_cast<std::ptrdiff_t>(device_words(p.device)), p.words.end(), uint16_t{0});
    }
    frames.insert(frames.end(), length, in);
  }
  return true;
}

auto input_hash(std::span<const FrameInput> frames) -> std::array<uint8_t, 20> {
  Archive ar = Archive::saver();
  write_runs(ar, frames);
  return utils::sha1(ar.data());
}

auto decode_movie(std::span<const uint8_t> data, Movie& movie) -> MovieResult {
  Archive ar = Archive::loader(data);
  std::array<char, 4> magic{};
  uint16_t version = 0;
  uint16_t flags = 0;
  ar(magic, version);
  if (!ar.ok() || magic != kMovieMagic) return MovieResult::NotAMovie;
  if (version > kMovieVersion) return MovieResult::WrongVersion;
  Movie m;
  uint32_t frames = 0;
  ar(flags);
  io_header(ar, m, frames);
  m.start = (flags & kFlagSaveState) ? MovieStart::SaveState : MovieStart::PowerOn;
  m.race = (flags & kFlagRace) != 0;
  if (!ar.ok() || frames > kMaxMovieFrames) return MovieResult::Corrupt;
  m.frames.reserve(frames);
  if (!read_runs(ar, m.frames, frames)) return MovieResult::Corrupt;
  if (m.frames.size() != frames || ar.remaining() != 0) return MovieResult::Corrupt;
  if (m.start == MovieStart::SaveState && m.state.empty()) return MovieResult::Corrupt;
  movie = std::move(m);
  return MovieResult::Ok;
}

auto read_movie(const std::string& path, Movie& movie) -> std::string {
  const auto data = read_file(path);
  if (!data) return "can't open " + std::filesystem::path(path).filename().string();
  const MovieResult r = decode_movie(*data, movie);
  return r == MovieResult::Ok ? std::string() : std::filesystem::path(path).filename().string() + ": " + describe(r);
}

auto write_movie(const std::string& path, const Movie& movie, size_t frames) -> bool { return write_file(path, encode_movie(movie, frames)); }

// ---- State references ---------------------------------------------------------------------------

auto make_state_ref(const Movie& movie, size_t frame, uint64_t journal_id, uint64_t journal_size) -> StateRef {
  StateRef ref;
  ref.movie_id = movie.id;
  ref.frame = static_cast<uint32_t>(std::min(frame, movie.frames.size()));
  ref.input = input_hash(std::span(movie.frames).first(ref.frame));
  ref.journal_id = journal_id;
  ref.journal_size = journal_size;
  return ref;
}

auto encode_state_ref(const StateRef& ref) -> std::vector<uint8_t> {
  StateRef r = ref;
  Archive ar = Archive::saver();
  std::array<char, 4> magic = kStateRefMagic;
  uint16_t version = kStateRefVersion;
  ar(magic, version, r.movie_id, r.frame, r.input, r.journal_id, r.journal_size);
  return ar.data();
}

auto decode_state_ref(std::span<const uint8_t> data, StateRef& ref) -> MovieResult {
  Archive ar = Archive::loader(data);
  std::array<char, 4> magic{};
  uint16_t version = 0;
  ar(magic, version);
  if (ar.ok() && magic == kMovieMagic) {  // an old sidecar: the movie up to the state
    Movie m;
    if (const MovieResult r = decode_movie(data, m); r != MovieResult::Ok) return r;
    StateRef r = make_state_ref(m, m.frames.size(), 0, 0);
    r.frames = std::move(m.frames);
    ref = std::move(r);
    return MovieResult::Ok;
  }
  if (!ar.ok() || magic != kStateRefMagic) return MovieResult::NotAMovie;
  if (version > kStateRefVersion) return MovieResult::WrongVersion;
  StateRef r;
  ar(r.movie_id, r.frame, r.input, r.journal_id, r.journal_size);
  if (!ar.ok() || ar.remaining() != 0 || r.frame > kMaxMovieFrames) return MovieResult::Corrupt;
  ref = std::move(r);
  return MovieResult::Ok;
}

auto on_take(const StateRef& ref, std::span<const FrameInput> frames) -> bool {
  return ref.frame <= frames.size() && input_hash(frames.first(ref.frame)) == ref.input;
}

auto state_ref_path(std::string_view state_path) -> std::string {
  return std::filesystem::path(state_path).replace_extension(kStateRefExtension).string();
}

auto read_state_ref(const std::string& state_path, StateRef& ref) -> bool {
  for (const std::string& path : {state_ref_path(state_path), state_movie_path(state_path)}) {
    if (const auto data = read_file(path)) return decode_state_ref(*data, ref) == MovieResult::Ok;
  }
  return false;
}

auto write_state_ref(const std::string& state_path, const StateRef* ref) -> bool {
  std::error_code ec;
  std::filesystem::remove(state_movie_path(state_path), ec);  // an old sidecar is stale either way
  if (ref) return write_file(state_ref_path(state_path), encode_state_ref(*ref));
  std::filesystem::remove(state_ref_path(state_path), ec);
  return true;
}

namespace {
constexpr std::array<MismatchPrompt, 4> kMismatchPrompts = {{
    {"", ""},
    {"Play a movie made with another ROM?", "This movie was recorded with a different ROM. It will most likely go out of sync."},
    {"Play a movie made with another version?",
     "This movie was recorded with a different version of the emulator. It may go out of sync."},
    {"Play a movie made with another ROM and version?",
     "This movie was recorded with a different ROM and a different version of the emulator. It will most likely go "
     "out of sync."},
}};
}  // namespace

auto movie_mismatch(const Movie& movie, uint32_t rom_crc32, const std::array<uint8_t, 20>& rom_sha1, std::string_view emulator)
    -> uint8_t {
  uint8_t out = 0;
  if (movie.rom_crc32 != rom_crc32 || movie.rom_sha1 != rom_sha1) out |= kMismatchRom;
  if (movie.emulator != emulator) out |= kMismatchEmulator;
  return out;
}

auto mismatch_prompt(uint8_t mismatch) -> const MismatchPrompt& { return kMismatchPrompts[mismatch & 3u]; }

auto is_prefix(std::span<const FrameInput> part, std::span<const FrameInput> whole) -> bool {
  return part.size() <= whole.size() && std::equal(part.begin(), part.end(), whole.begin());
}

auto state_movie_path(std::string_view state_path) -> std::string {
  return std::filesystem::path(state_path).replace_extension(kMovieExtension).string();
}

auto attempt_movie_path(std::string_view data_dir, std::string_view rom_stem, int attempt) -> std::string {
  const std::string name = "attempt-" + std::to_string(attempt) + std::string(kMovieExtension);
  return (std::filesystem::path(data_dir) / kMoviesFolder / rom_stem / name).string();
}

auto movie_counter(uint64_t frame, uint64_t frames, bool playing) -> std::string {
  return playing ? std::to_string(frame) + " / " + std::to_string(frames) : std::to_string(frame);
}

}  // namespace bitforge::speedrun
