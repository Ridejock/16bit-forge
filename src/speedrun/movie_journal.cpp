// SPDX-License-Identifier: MIT
#include "speedrun/movie_journal.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace bitforge::speedrun {

namespace {

constexpr size_t kFileHeader = 4 + 2 + 8;     // magic, version, id
constexpr size_t kRecordHeader = 1 + 4;       // kind, length
constexpr size_t kRecordCrc = 4;

auto record(JournalRecord kind, std::span<const uint8_t> payload) -> std::vector<uint8_t> {
  Archive ar = Archive::saver();
  uint32_t length = static_cast<uint32_t>(payload.size());
  ar(kind, length);
  std::vector<uint8_t> out = ar.data();
  out.insert(out.end(), payload.begin(), payload.end());
  const uint32_t crc = crc32(out);
  for (size_t i = 0; i < sizeof(crc); i++) out.push_back(static_cast<uint8_t>(crc >> (8 * i)));
  return out;
}

auto get_u32(std::span<const uint8_t> data, size_t at) -> uint32_t {
  return static_cast<uint32_t>(data[at]) | static_cast<uint32_t>(data[at + 1]) << 8 | static_cast<uint32_t>(data[at + 2]) << 16 |
         static_cast<uint32_t>(data[at + 3]) << 24;
}

// One input record into the journal's movie: false if it doesn't make sense.
auto apply_input(std::span<const uint8_t> payload, Movie& movie) -> bool {
  Archive ar = Archive::loader(payload);
  uint32_t start = 0;
  uint32_t rerecords = 0;
  bool race = false;
  ar(start, rerecords, race);
  if (!ar.ok() || start > movie.frames.size()) return false;
  std::vector<FrameInput> added;
  if (!read_runs(ar, added, kMaxMovieFrames - start) || ar.remaining() != 0) return false;
  movie.frames.resize(start);
  movie.frames.insert(movie.frames.end(), added.begin(), added.end());
  movie.rerecords = rerecords;
  movie.race = race;
  return true;
}

}  // namespace

auto journal_path(std::string_view movie_path) -> std::string { return std::string(movie_path) + std::string(kJournalSuffix); }

auto journal_movie_path(std::string_view path) -> std::string {
  return path.ends_with(kJournalSuffix) ? std::string(path.substr(0, path.size() - kJournalSuffix.size())) : std::string(path);
}

auto encode_journal_start(uint64_t id, const Movie& movie) -> std::vector<uint8_t> {
  Archive ar = Archive::saver();
  std::array<char, 4> magic = kJournalMagic;
  uint16_t version = kJournalVersion;
  ar(magic, version, id);
  std::vector<uint8_t> out = ar.data();
  const std::vector<uint8_t> header = record(JournalRecord::Header, encode_movie(movie, 0));
  const std::vector<uint8_t> input = encode_journal_input(movie, 0);
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), input.begin(), input.end());
  return out;
}

auto encode_journal_input(const Movie& movie, size_t from) -> std::vector<uint8_t> {
  from = std::min(from, movie.frames.size());
  Archive ar = Archive::saver();
  uint32_t start = static_cast<uint32_t>(from);
  uint32_t rerecords = movie.rerecords;
  bool race = movie.race;
  ar(start, rerecords, race);
  write_runs(ar, std::span(movie.frames).subspan(from));
  return record(JournalRecord::Input, ar.data());
}

auto decode_journal(std::span<const uint8_t> data, Journal& journal) -> MovieResult {
  Archive ar = Archive::loader(data);
  std::array<char, 4> magic{};
  uint16_t version = 0;
  Journal j;
  ar(magic, version, j.id);
  if (!ar.ok() || magic != kJournalMagic) return MovieResult::NotAMovie;
  if (version > kJournalVersion) return MovieResult::WrongVersion;
  bool header = false;
  size_t at = kFileHeader;
  while (at < data.size()) {
    if (data.size() - at < kRecordHeader + kRecordCrc) break;
    const auto kind = static_cast<JournalRecord>(data[at]);
    const uint32_t length = get_u32(data, at + 1);
    if (length > data.size() - at - kRecordHeader - kRecordCrc) break;
    const size_t end = at + kRecordHeader + length;
    if (crc32(data.subspan(at, end - at)) != get_u32(data, end)) break;
    const std::span<const uint8_t> payload = data.subspan(at + kRecordHeader, length);
    if (kind == JournalRecord::Header && !header) {
      if (decode_movie(payload, j.movie) != MovieResult::Ok) break;
      header = true;
    } else if (kind != JournalRecord::Input || !header || !apply_input(payload, j.movie)) {
      break;
    }
    at = end + kRecordCrc;
  }
  if (!header) return MovieResult::Corrupt;
  j.bytes = at;
  j.torn = at != data.size();
  journal = std::move(j);
  return MovieResult::Ok;
}

auto read_journal(const std::string& path, Journal& journal, uint64_t length) -> std::string {
  const std::string name = fs::path(path).filename().string();
  std::ifstream in(path, std::ios::binary);
  if (!in) return "can't open " + name;
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() > length) data.resize(static_cast<size_t>(length));
  const MovieResult r = decode_journal(data, journal);
  return r == MovieResult::Ok ? std::string() : name + ": " + describe(r);
}

auto recover_journal(const std::string& path) -> std::string {
  Journal journal;
  if (std::string why = read_journal(path, journal); !why.empty()) return why;
  const std::string movie = journal_movie_path(path);
  if (!write_movie(movie, journal.movie)) return "could not write " + fs::path(movie).filename().string();
  std::error_code ec;
  fs::remove(path, ec);
  return {};
}

auto find_journals(const std::string& folder) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::error_code ec;
  const std::string tail = std::string(kMovieExtension) + std::string(kJournalSuffix);
  for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->is_regular_file(ec) && name.size() > tail.size() && name.ends_with(tail)) out.push_back(it->path().string());
  }
  return out;
}

// ---- The writer ---------------------------------------------------------------------------------

JournalWriter::~JournalWriter() { close(false); }

auto JournalWriter::open(const std::string& movie_path, const Movie& movie) -> void {
  close(false);
  path_ = journal_path(movie_path);
  id_ = new_movie_id();
  size_ = 0;
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    written_ = 0;
    failed_ = false;
    stop_ = false;
  }
  worker_ = std::thread([this] { run(); });
  append(encode_journal_start(id_, movie));
}

auto JournalWriter::append(std::vector<uint8_t> bytes) -> void {
  if (!is_open() || bytes.empty()) return;
  size_ += bytes.size();
  {
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(bytes));
  }
  wake_.notify_all();
}

auto JournalWriter::sync() -> bool {
  if (!is_open()) return false;
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [this] { return written_ >= size_; });
  return !failed_;
}

auto JournalWriter::close(bool remove) -> void {
  if (!is_open()) return;
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  worker_.join();
  std::error_code ec;
  if (remove) fs::remove(path_, ec);
  path_.clear();
  id_ = 0;
  size_ = 0;
}

// Appends what is queued and flushes it to the OS, until close() with nothing left.
auto JournalWriter::run() -> void {
  std::error_code ec;
  if (const auto dir = fs::path(path_).parent_path(); !dir.empty()) fs::create_directories(dir, ec);
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  if (!out) utils::Logger::error(utils::LogChannel::Rom, "Movie: can't write %s; a crash would lose this recording", path_.c_str());
  std::vector<std::vector<uint8_t>> batch;
  while (true) {
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stopping, all written
      batch.swap(queue_);
    }
    uint64_t done = 0;
    for (const std::vector<uint8_t>& bytes : batch) {
      out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      done += bytes.size();
    }
    out.flush();
    batch.clear();
    {
      std::lock_guard lock(mutex_);
      written_ += done;
      failed_ = failed_ || !out;
    }
    wake_.notify_all();
  }
}

}  // namespace bitforge::speedrun
