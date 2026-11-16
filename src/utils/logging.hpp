// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace snes::utils {

enum class LogLevel : uint8_t {
  Trace,    // per instruction / per register access: huge, off unless asked for
  Debug,
  Info,
  Warning,
  Error
};
inline constexpr std::array<std::string_view, 5> kLogLevelNames = {"Trace", "Debug", "Info", "Warning", "Error"};

// Where lines end up. System is the app (frontend, video, audio, ROM loading, save states);
// Emulation is what the emulated machine does (CPU/PPU/APU/DMA traces and state dumps).
// Each sink has its own ring for the log panel, level filter, pause flag and optional file.
enum class LogSink : uint8_t { System, Emulation };
inline constexpr std::size_t kLogSinkCount = 2;

enum class LogChannel : uint8_t { System, Video, Audio, Input, Rom, State, Cpu, Ppu, Apu, Dma };

struct LogChannelInfo {
  std::string_view name;  // shown in the UI and used as the ini key
  LogSink sink;           // default route
};

// One row per channel, in LogChannel order.
inline constexpr std::array<LogChannelInfo, 10> kLogChannels = {{
  {"System", LogSink::System},
  {"Video", LogSink::System},
  {"Audio", LogSink::System},
  {"Input", LogSink::System},
  {"ROM", LogSink::System},
  {"State", LogSink::System},
  {"CPU", LogSink::Emulation},
  {"PPU", LogSink::Emulation},
  {"APU", LogSink::Emulation},
  {"DMA", LogSink::Emulation},
}};
inline constexpr std::size_t kLogChannelCount = kLogChannels.size();

struct LogSinkInfo {
  std::string_view name;
  int capacity;  // lines kept for the panel
};
inline constexpr std::array<LogSinkInfo, kLogSinkCount> kLogSinks = {{
  {"System", 400},
  {"Emulation", 2000},
}};

namespace detail {
constexpr auto default_routes() {
  std::array<LogSink, kLogChannelCount> r{};
  for (std::size_t i = 0; i < kLogChannelCount; i++) r[i] = kLogChannels[i].sink;
  return r;
}
constexpr auto all_on() {
  std::array<bool, kLogChannelCount> on{};
  on.fill(true);
  return on;
}

// Filter settings are read by every logging thread (the emulation thread logs too) and changed
// from the UI: each entry is a relaxed atomic. Constant-initialized from a plain array.
template <typename T, std::size_t N>
class AtomicArray {
public:
  constexpr explicit AtomicArray(const std::array<T, N>& init) : AtomicArray(init, std::make_index_sequence<N>{}) {}
  auto get(std::size_t i) const -> T { return v_[i].load(std::memory_order_relaxed); }
  auto set(std::size_t i, T value) -> void { v_[i].store(value, std::memory_order_relaxed); }

private:
  template <std::size_t... I>
  constexpr AtomicArray(const std::array<T, N>& init, std::index_sequence<I...>) : v_{init[I]...} {}
  std::atomic<T> v_[N];
};
} // namespace detail

// Process-wide logger. Every line is routed by its channel to a sink: kept in that sink's ring
// for the log panel, appended to the sink's file if one is open (size-capped: at the cap the
// file is renamed to <name>.1<ext> and a new one is started), and mirrored to stderr unless it
// is Emulation Trace/Debug output. Lines below a sink's level or on a disabled channel are
// dropped before formatting; use SNES_LOG / the *_TRACE macros on hot paths so a disabled
// channel costs one table lookup. Thread-safe: lines are added and read under one mutex, and the
// filters are atomics.
class Logger {
public:
  static constexpr int kLineSize = 512;

  // ---- routing and filters ----------------------------------------------------------------
  static auto enabled(LogChannel channel, LogLevel level) -> bool {
    const auto c = static_cast<std::size_t>(channel);
    return channel_on.get(c) && static_cast<int>(level) >= static_cast<int>(sink_level.get(static_cast<std::size_t>(route.get(c))));
  }
  static auto set_channel_enabled(LogChannel channel, bool on) -> void { channel_on.set(static_cast<std::size_t>(channel), on); }
  static auto channel_enabled(LogChannel channel) -> bool { return channel_on.get(static_cast<std::size_t>(channel)); }
  static auto set_route(LogChannel channel, LogSink sink) -> void { route.set(static_cast<std::size_t>(channel), sink); }
  static auto sink_of(LogChannel channel) -> LogSink { return route.get(static_cast<std::size_t>(channel)); }

  static auto set_level(LogSink sink, LogLevel level) -> void { sink_level.set(static_cast<std::size_t>(sink), level); }
  static auto level(LogSink sink) -> LogLevel { return sink_level.get(static_cast<std::size_t>(sink)); }
  // Every sink at once (tests use this to silence output).
  static auto set_level(LogLevel level) -> void {
    for (std::size_t i = 0; i < kLogSinkCount; i++) sink_level.set(i, level);
  }
  static auto level() -> LogLevel { return level(LogSink::System); }

  // A paused sink stops adding lines to its ring (files still get them).
  static auto set_paused(LogSink sink, bool paused) -> void { sink_paused.set(static_cast<std::size_t>(sink), paused); }
  static auto paused(LogSink sink) -> bool { return sink_paused.get(static_cast<std::size_t>(sink)); }

  // Opens (truncating) or, with an empty path, closes the sink's log file.
  static auto set_file(LogSink sink, const std::string& path, std::size_t max_bytes) -> bool;
  static auto file_path(LogSink sink) -> std::string;

  // ---- the ring, for the log panel (default: the System sink) --------------------------------
  static auto clear(LogSink sink = LogSink::System) -> void;
  static auto line_count(LogSink sink = LogSink::System) -> int;
  static auto revision(LogSink sink = LogSink::System) -> unsigned;
  // index 0 is the oldest line still in the ring. Text is "[LEVEL] message". Copies, taken under
  // the lock (another thread may be adding lines meanwhile).
  struct LineCopy {
    LogLevel level = LogLevel::Info;
    LogChannel channel = LogChannel::System;
    std::string text;
  };
  static auto line(int index, LogSink sink = LogSink::System) -> LineCopy;
  static auto line_text(int index, LogSink sink = LogSink::System) -> std::string;
  static auto line_level(int index, LogSink sink = LogSink::System) -> LogLevel;
  static auto line_channel(int index, LogSink sink = LogSink::System) -> LogChannel;

  // ---- writing ------------------------------------------------------------------------------
  static auto log(LogChannel channel, LogLevel level, const char* fmt, ...) -> void;
  static auto vlog(LogChannel channel, LogLevel level, const char* fmt, va_list args) -> void;

  // Shorthands. Without a channel they log to System.
  static auto debug(const char* fmt, ...) -> void;
  static auto info(const char* fmt, ...) -> void;
  static auto warning(const char* fmt, ...) -> void;
  static auto error(const char* fmt, ...) -> void;
  static auto debug(LogChannel channel, const char* fmt, ...) -> void;
  static auto info(LogChannel channel, const char* fmt, ...) -> void;
  static auto warning(LogChannel channel, const char* fmt, ...) -> void;
  static auto error(LogChannel channel, const char* fmt, ...) -> void;

private:
  // Info hides Trace/Debug until a sink's level is lowered.
  static inline detail::AtomicArray<LogLevel, kLogSinkCount> sink_level{std::array<LogLevel, kLogSinkCount>{LogLevel::Info, LogLevel::Info}};
  static inline detail::AtomicArray<bool, kLogSinkCount> sink_paused{std::array<bool, kLogSinkCount>{}};
  static inline detail::AtomicArray<LogSink, kLogChannelCount> route{detail::default_routes()};
  static inline detail::AtomicArray<bool, kLogChannelCount> channel_on{detail::all_on()};
};

} // namespace snes::utils

// Logs only when the channel is on and the level passes its sink: arguments aren't evaluated otherwise.
#define SNES_LOG(channel, level, ...)                                                   \
  do {                                                                                  \
    if (::snes::utils::Logger::enabled(channel, level)) {                               \
      ::snes::utils::Logger::log(channel, level, __VA_ARGS__);                          \
    }                                                                                   \
  } while (0)

// Core trace points, switchable at runtime (Log > Emulation: channel on, level Trace). Define
// SNES_NO_TRACE to compile them out entirely.
#ifdef SNES_NO_TRACE
#define CPU_TRACE(...) ((void)0)
#define PPU_TRACE(...) ((void)0)
#define APU_TRACE(...) ((void)0)
#define DMA_TRACE(...) ((void)0)
#else
#define CPU_TRACE(...) SNES_LOG(::snes::utils::LogChannel::Cpu, ::snes::utils::LogLevel::Trace, __VA_ARGS__)
#define PPU_TRACE(...) SNES_LOG(::snes::utils::LogChannel::Ppu, ::snes::utils::LogLevel::Trace, __VA_ARGS__)
#define APU_TRACE(...) SNES_LOG(::snes::utils::LogChannel::Apu, ::snes::utils::LogLevel::Trace, __VA_ARGS__)
#define DMA_TRACE(...) SNES_LOG(::snes::utils::LogChannel::Dma, ::snes::utils::LogLevel::Trace, __VA_ARGS__)
#endif
