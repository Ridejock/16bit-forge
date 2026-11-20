// SPDX-License-Identifier: MIT
#pragma once
// A recording's journal, <movie>.bfm.part beside where the movie goes, so a crash keeps the take.
// While a movie records, its new input is added to the journal every so many frames; Stop writes
// the .bfm (temp file + rename) and deletes the journal. A journal found later is a recording
// that never stopped: recover_journal() makes it a movie, up to the last input that reached it.
//
//   magic "BFJ\x1A" | version u16 | journal id u64 | records, only ever appended:
//   kind u8 (JournalRecord) | length u32 | payload | CRC-32 u32 of kind, length and payload
//     Header: the movie as a .bfm without frames (first, once)
//     Input:  start u32 | rerecords u32 | race u8 | runs (as in a .bfm)
//             = keep the first `start` frames (a rerecord cut the rest), then add the runs
//
// A record a crash cut short fails its length or CRC: it and anything after it are left out.
// Every take ever recorded is in there in order, so the journal also gives back a take a
// rerecord cut, for a save state made on it (StateRef::journal_size).
// Files are written by a thread of the journal's own; the caller only queues bytes.
#include "speedrun/movie.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bitforge::speedrun {

inline constexpr std::array<char, 4> kJournalMagic = {'B', 'F', 'J', '\x1A'};
inline constexpr uint16_t kJournalVersion = 1;
inline constexpr std::string_view kJournalSuffix = ".part";
inline constexpr uint32_t kDefaultFlushFrames = 60;  // [movie] flush_frames: about a second
inline constexpr uint32_t kMaxFlushFrames = 3600;

enum class JournalRecord : uint8_t { Header = 1, Input = 2 };

// "movies/G/run.bfm" <-> "movies/G/run.bfm.part"
auto journal_path(std::string_view movie_path) -> std::string;
auto journal_movie_path(std::string_view journal_path) -> std::string;

// The file's start: magic, version, id, the header record and an input record of the movie's
// frames so far.
auto encode_journal_start(uint64_t id, const Movie& movie) -> std::vector<uint8_t>;
// An input record: frames [from, end) of the movie, its rerecords and race flag.
auto encode_journal_input(const Movie& movie, size_t from) -> std::vector<uint8_t>;

struct Journal {
  uint64_t id = 0;
  Movie movie;        // as of the last whole record
  size_t bytes = 0;   // how much of the data those records are
  bool torn = false;  // something after them didn't make it (cut short or damaged)
};
// Ok with at least the header record; NotAMovie / WrongVersion / Corrupt otherwise.
auto decode_journal(std::span<const uint8_t> data, Journal& journal) -> MovieResult;
// The file (its first `length` bytes): "" or why it can't be read.
auto read_journal(const std::string& path, Journal& journal, uint64_t length = UINT64_MAX) -> std::string;
// Writes the journal's movie to its .bfm and deletes the journal: "" or why not.
auto recover_journal(const std::string& path) -> std::string;
// Journals left in `folder` and below.
auto find_journals(const std::string& folder) -> std::vector<std::string>;

class JournalWriter {
public:
  JournalWriter() = default;
  ~JournalWriter();  // close(false): the journal stays
  JournalWriter(const JournalWriter&) = delete;
  auto operator=(const JournalWriter&) -> JournalWriter& = delete;

  // Starts <movie_path>.part with `movie` as it is (replacing any file there).
  auto open(const std::string& movie_path, const Movie& movie) -> void;
  auto is_open() const -> bool { return worker_.joinable(); }
  auto id() const -> uint64_t { return id_; }
  auto path() const -> const std::string& { return path_; }
  auto size() const -> uint64_t { return size_; }  // bytes handed over so far
  // Queued; the writer thread appends and flushes it.
  auto append(std::vector<uint8_t> bytes) -> void;
  // Waits until all handed over is written: false if a write failed.
  auto sync() -> bool;
  // Finishes writing and stops; `remove` deletes the file (the movie is saved).
  auto close(bool remove) -> void;

private:
  auto run() -> void;

  // The caller's thread only.
  std::string path_;
  uint64_t id_ = 0;
  uint64_t size_ = 0;
  // Shared with the writer, under mutex_.
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<std::vector<uint8_t>> queue_;
  uint64_t written_ = 0;  // handed over and done (written or failed)
  bool failed_ = false;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace bitforge::speedrun
