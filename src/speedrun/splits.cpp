// SPDX-License-Identifier: MIT
#include "speedrun/splits.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace bitforge::speedrun {

namespace {

constexpr Nanos kNanosPerMinute = 60 * kNanosPerSecond;
constexpr Nanos kNanosPerHour = 60 * kNanosPerMinute;
constexpr Nanos kNanosPerDay = 24 * kNanosPerHour;
constexpr Nanos kTicksPerSecond = 10'000'000;  // .NET TimeSpan ticks: the .lss writes 7 digits
constexpr Nanos kNanosPerTick = kNanosPerSecond / kTicksPerSecond;
constexpr int kMaxDecimals = 3;
constexpr int kTickDigits = 7;

auto pow10(int n) -> Nanos {
  Nanos p = 1;
  while (n-- > 0) p *= 10;
  return p;
}

// "h:mm:ss" / "m:ss" / "s" for a non-negative whole number of seconds.
auto clock_text(Nanos t) -> std::string {
  const Nanos h = t / kNanosPerHour;
  const Nanos m = t / kNanosPerMinute % 60;
  const Nanos s = t / kNanosPerSecond % 60;
  char text[32];
  if (h > 0) {
    std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", static_cast<long long>(h), static_cast<long long>(m),
                  static_cast<long long>(s));
  } else if (m > 0) {
    std::snprintf(text, sizeof(text), "%lld:%02lld", static_cast<long long>(m), static_cast<long long>(s));
  } else {
    std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(s));
  }
  return text;
}

auto with_decimals(Nanos t, int decimals) -> std::string {
  decimals = std::clamp(decimals, 0, kMaxDecimals);
  std::string out = clock_text(t);
  if (decimals > 0) {
    const Nanos unit = kNanosPerSecond / pow10(decimals);
    char frac[8];
    std::snprintf(frac, sizeof(frac), ".%0*lld", decimals, static_cast<long long>(t % kNanosPerSecond / unit));
    out += frac;
  }
  return out;
}

auto parse_int(std::string_view s, int64_t& out) -> bool {
  if (s.empty()) return false;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && end == s.data() + s.size() && out >= 0;
}

}  // namespace

auto frames_to_nanos(int64_t frames, VideoStandard standard) -> Nanos {
  return static_cast<Nanos>(std::llround(static_cast<double>(frames) * static_cast<double>(kNanosPerSecond) /
                                         info(standard).frame_rate));
}

auto completed_runs(const Run& run) -> int {
  return static_cast<int>(std::count_if(run.attempts.begin(), run.attempts.end(), [](const Attempt& a) { return a.finished(); }));
}

auto sum_of_best(const Run& run, Method m) -> std::optional<Nanos> {
  if (run.segments.empty()) return std::nullopt;
  Nanos sum = 0;
  for (const Segment& s : run.segments) {
    const auto best = s.best.get(m);
    if (!best) return std::nullopt;
    sum += *best;
  }
  return sum;
}

auto pb_time(const Run& run, Method m) -> std::optional<Nanos> {
  return run.segments.empty() ? std::nullopt : run.segments.back().pb.get(m);
}

auto pb_segment(const Run& run, size_t i, Method m) -> std::optional<Nanos> {
  if (i >= run.segments.size()) return std::nullopt;
  const auto end = run.segments[i].pb.get(m);
  const auto start = i == 0 ? std::optional<Nanos>(0) : run.segments[i - 1].pb.get(m);
  if (!end || !start) return std::nullopt;
  return *end - *start;
}

auto next_attempt_id(const Run& run) -> int {
  int id = 0;
  for (const Attempt& a : run.attempts) id = std::max(id, a.id);
  return id + 1;
}

auto make_run(std::string game, std::string category, size_t count) -> Run {
  Run run;
  run.game = std::move(game);
  run.category = std::move(category);
  for (size_t i = 0; i < count; i++) run.segments.push_back({"Split " + std::to_string(i + 1), {}, {}, {}, {}, -1});
  return run;
}

auto format_time(Nanos t, int decimals) -> std::string {
  return t < 0 ? "-" + with_decimals(-t, decimals) : with_decimals(t, decimals);
}

auto format_delta(Nanos d) -> std::string {
  const Nanos a = d < 0 ? -d : d;
  return (d < 0 ? "-" : "+") + with_decimals(a, a >= kNanosPerMinute ? 1 : 2);
}

auto format_timespan(Nanos t) -> std::string {
  const bool negative = t < 0;
  Nanos a = negative ? -t : t;
  const Nanos days = a / kNanosPerDay;
  a %= kNanosPerDay;
  char text[48];
  const Nanos ticks = a % kNanosPerSecond / kNanosPerTick;
  int n = std::snprintf(text, sizeof(text), "%s", negative ? "-" : "");
  if (days > 0) n += std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), "%lld.", static_cast<long long>(days));
  n += std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), "%02lld:%02lld:%02lld", static_cast<long long>(a / kNanosPerHour),
                     static_cast<long long>(a / kNanosPerMinute % 60), static_cast<long long>(a / kNanosPerSecond % 60));
  if (ticks > 0) std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), ".%0*lld", kTickDigits, static_cast<long long>(ticks));
  return text;
}

// [-][d.]hh:mm:ss[.fffffff]; also "mm:ss" and plain seconds, which hand-written files use.
auto parse_timespan(std::string_view text) -> std::optional<Nanos> {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  const bool negative = !text.empty() && text.front() == '-';
  if (negative) text.remove_prefix(1);
  if (text.empty()) return std::nullopt;

  Nanos frac = 0;
  // The fraction is the part after the last '.', when a ':' comes before it (else it's days).
  const size_t colon = text.find(':');
  const size_t dot = text.rfind('.');
  if (dot != std::string_view::npos && (colon == std::string_view::npos || dot > colon)) {
    const std::string_view digits = text.substr(dot + 1);
    if (digits.empty() || digits.size() > 9) return std::nullopt;
    int64_t value = 0;
    if (!parse_int(digits, value)) return std::nullopt;
    frac = value * pow10(9 - static_cast<int>(digits.size()));
    text = text.substr(0, dot);
  }
  int64_t days = 0;
  if (const size_t d = text.find('.'); d != std::string_view::npos) {
    if (!parse_int(text.substr(0, d), days)) return std::nullopt;
    text = text.substr(d + 1);
  }
  std::array<int64_t, 3> parts{};  // h, m, s from the right
  size_t count = 0;
  while (true) {
    const size_t c = text.rfind(':');
    const std::string_view field = c == std::string_view::npos ? text : text.substr(c + 1);
    if (count == parts.size() || !parse_int(field, parts[count])) return std::nullopt;
    count++;
    if (c == std::string_view::npos) break;
    text = text.substr(0, c);
  }
  const Nanos t = days * kNanosPerDay + parts[2] * kNanosPerHour + parts[1] * kNanosPerMinute + parts[0] * kNanosPerSecond + frac;
  return negative ? -t : t;
}

}  // namespace bitforge::speedrun
