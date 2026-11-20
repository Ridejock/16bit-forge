// SPDX-License-Identifier: MIT
#include "speedrun/movie.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>

namespace bitforge::speedrun {

namespace {

constexpr const char* kMoviesFolder = "movies";
// A day of frames: more than any run, and a cap on what a damaged file can make us allocate.
constexpr uint64_t kMaxFrames = 24ull * 3600 * 61;
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
  // Runs of identical frames.
  std::vector<std::pair<uint32_t, size_t>> runs;  // length, first frame
  for (size_t i = 0; i < count; i++) {
    if (!runs.empty() && m.frames[i] == m.frames[runs.back().second]) {
      runs.back().first++;
    } else {
      runs.push_back({1, i});
    }
  }
  uint32_t run_count = static_cast<uint32_t>(runs.size());
  ar(run_count);
  for (auto& [length, first] : runs) ar(length, m.frames[first]);
  return ar.data();
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
  uint32_t run_count = 0;
  ar(run_count);
  if (!ar.ok() || frames > kMaxFrames || run_count > frames) return MovieResult::Corrupt;
  m.frames.reserve(frames);
  for (uint32_t r = 0; r < run_count; r++) {
    uint32_t length = 0;
    FrameInput in;
    ar(length, in);
    if (!ar.ok() || length == 0 || length > frames - m.frames.size()) return MovieResult::Corrupt;
    for (PortInput& p : in.ports) {
      if (!known_device(p.device)) return MovieResult::Corrupt;
      std::fill(p.words.begin() + static_cast<std::ptrdiff_t>(device_words(p.device)), p.words.end(), uint16_t{0});
    }
    m.frames.insert(m.frames.end(), length, in);
  }
  if (m.frames.size() != frames || ar.remaining() != 0) return MovieResult::Corrupt;
  if (m.start == MovieStart::SaveState && m.state.empty()) return MovieResult::Corrupt;
  movie = std::move(m);
  return MovieResult::Ok;
}

auto read_movie(const std::string& path, Movie& movie) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "can't open " + std::filesystem::path(path).filename().string();
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const MovieResult r = decode_movie(data, movie);
  return r == MovieResult::Ok ? std::string() : std::filesystem::path(path).filename().string() + ": " + describe(r);
}

// Written to <path>.tmp and renamed, so a crash mid-write never leaves half a movie.
auto write_movie(const std::string& path, const Movie& movie, size_t frames) -> bool {
  const std::vector<uint8_t> data = encode_movie(movie, frames);
  std::error_code ec;
  if (const auto dir = std::filesystem::path(path).parent_path(); !dir.empty()) std::filesystem::create_directories(dir, ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) return false;
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) std::filesystem::remove(tmp, ec);
  return !ec;
}

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
