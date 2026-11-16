// SPDX-License-Identifier: MIT
#include "utils/logging.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <vector>

namespace snes::utils {

namespace {

constexpr std::array<const char*, 5> kTags = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};

struct Line {
  LogLevel level = LogLevel::Info;
  LogChannel channel = LogChannel::System;
  char text[Logger::kLineSize] = {};
};

struct Sink {
  std::vector<Line> lines;
  int count = 0;
  int next = 0;
  unsigned generation = 0;
  std::FILE* file = nullptr;
  std::string path;
  std::size_t max_bytes = 0;
  std::size_t written = 0;

  auto slot(int index) const -> const Line& {
    const int capacity = static_cast<int>(lines.size());
    const int start = (count == capacity) ? next : 0;
    return lines[static_cast<std::size_t>((start + index) % capacity)];
  }
};

struct State {
  std::mutex mutex;
  std::array<Sink, kLogSinkCount> sinks;
  State() {
    for (std::size_t i = 0; i < kLogSinkCount; i++) sinks[i].lines.resize(static_cast<std::size_t>(kLogSinks[i].capacity));
  }
  ~State() {
    for (auto& s : sinks) {
      if (s.file) std::fclose(s.file);
    }
  }
};

auto state() -> State& {
  static State s;
  return s;
}

auto sink(LogSink id) -> Sink& { return state().sinks[static_cast<std::size_t>(id)]; }

// "logs/emu_trace.log" -> "logs/emu_trace.1.log"
auto rotated_path(const std::string& path) -> std::filesystem::path {
  std::filesystem::path p(path);
  return p.parent_path() / (p.stem().string() + ".1" + p.extension().string());
}

auto timestamp(char* out, std::size_t size) -> void {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  const std::size_t n = std::strftime(out, size, "%H:%M:%S", &local);
  std::snprintf(out + n, size - n, ".%03d", static_cast<int>(ms));
}

// Lock held.
auto write_file(Sink& s, const Line& line) -> void {
  if (!s.file) return;
  char stamp[32];
  timestamp(stamp, sizeof(stamp));
  const auto& name = kLogChannels[static_cast<std::size_t>(line.channel)].name;
  const int n = std::fprintf(s.file, "%s [%.*s] %s\n", stamp, static_cast<int>(name.size()), name.data(), line.text);
  if (n > 0) s.written += static_cast<std::size_t>(n);
  if (line.level >= LogLevel::Warning) std::fflush(s.file);
  if (s.max_bytes == 0 || s.written < s.max_bytes) return;

  // Cap reached: keep one previous file and start over.
  std::fclose(s.file);
  std::error_code ec;
  const auto old = rotated_path(s.path);
  std::filesystem::remove(old, ec);
  std::filesystem::rename(s.path, old, ec);
  s.file = std::fopen(s.path.c_str(), "w");
  s.written = 0;
}

} // namespace

auto Logger::set_file(LogSink id, const std::string& path, std::size_t max_bytes) -> bool {
  std::lock_guard lock(state().mutex);
  Sink& s = sink(id);
  if (s.file) std::fclose(s.file);
  s.file = nullptr;
  s.path = path;
  s.max_bytes = max_bytes;
  s.written = 0;
  if (path.empty()) return true;
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  s.file = std::fopen(path.c_str(), "w");
  if (!s.file) s.path.clear();
  return s.file != nullptr;
}

auto Logger::file_path(LogSink id) -> std::string {
  std::lock_guard lock(state().mutex);
  return sink(id).path;
}

auto Logger::clear(LogSink id) -> void {
  std::lock_guard lock(state().mutex);
  Sink& s = sink(id);
  s.count = 0;
  s.next = 0;
  s.generation++;
}

auto Logger::line_count(LogSink id) -> int {
  std::lock_guard lock(state().mutex);
  return sink(id).count;
}

auto Logger::revision(LogSink id) -> unsigned {
  std::lock_guard lock(state().mutex);
  return sink(id).generation;
}

auto Logger::line(int index, LogSink id) -> LineCopy {
  std::lock_guard lock(state().mutex);
  const Sink& s = sink(id);
  if (index < 0 || index >= s.count) return {};
  const Line& l = s.slot(index);
  return {l.level, l.channel, l.text};
}

auto Logger::line_text(int index, LogSink id) -> std::string { return line(index, id).text; }
auto Logger::line_level(int index, LogSink id) -> LogLevel { return line(index, id).level; }
auto Logger::line_channel(int index, LogSink id) -> LogChannel { return line(index, id).channel; }

auto Logger::vlog(LogChannel channel, LogLevel level, const char* fmt, va_list args) -> void {
  if (!enabled(channel, level)) return;
  const LogSink id = sink_of(channel);

  char body[kLineSize - 16];  // room for the "[LEVEL] " prefix
  std::vsnprintf(body, sizeof(body), fmt, args);
  Line line;
  line.level = level;
  line.channel = channel;
  std::snprintf(line.text, sizeof(line.text), "[%s] %s", kTags[static_cast<std::size_t>(level)], body);

  std::lock_guard lock(state().mutex);
  Sink& s = sink(id);
  const bool trace = id == LogSink::Emulation && level <= LogLevel::Debug;
  if (!trace) {
    if (channel == LogChannel::System) {
      std::fprintf(stderr, "%s\n", line.text);
    } else {
      const auto& name = kLogChannels[static_cast<std::size_t>(channel)].name;
      std::fprintf(stderr, "[%.*s] %s\n", static_cast<int>(name.size()), name.data(), line.text);
    }
  }
  write_file(s, line);
  if (sink_paused.get(static_cast<std::size_t>(id))) return;
  s.lines[static_cast<std::size_t>(s.next)] = line;
  s.next = (s.next + 1) % static_cast<int>(s.lines.size());
  if (s.count < static_cast<int>(s.lines.size())) s.count++;
  s.generation++;
}

auto Logger::log(LogChannel channel, LogLevel level, const char* fmt, ...) -> void {
  va_list args;
  va_start(args, fmt);
  vlog(channel, level, fmt, args);
  va_end(args);
}

#define SNES_LOGGER_SHORTHAND(name, lvl)                                    \
  auto Logger::name(const char* fmt, ...) -> void {                         \
    va_list args;                                                           \
    va_start(args, fmt);                                                    \
    vlog(LogChannel::System, LogLevel::lvl, fmt, args);                     \
    va_end(args);                                                           \
  }                                                                         \
  auto Logger::name(LogChannel channel, const char* fmt, ...) -> void {     \
    va_list args;                                                           \
    va_start(args, fmt);                                                    \
    vlog(channel, LogLevel::lvl, fmt, args);                                \
    va_end(args);                                                           \
  }

SNES_LOGGER_SHORTHAND(debug, Debug)
SNES_LOGGER_SHORTHAND(info, Info)
SNES_LOGGER_SHORTHAND(warning, Warning)
SNES_LOGGER_SHORTHAND(error, Error)

#undef SNES_LOGGER_SHORTHAND

} // namespace snes::utils
