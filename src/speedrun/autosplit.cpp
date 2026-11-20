// SPDX-License-Identifier: MIT
#include "speedrun/autosplit.hpp"

#include <algorithm>
#include <charconv>

namespace bitforge::speedrun {

namespace {

constexpr std::string_view kHeader =
    "# 16bit Forge auto-splits for the .lss beside this file: start / split <segment> / reset,\n"
    "# the test (eq ne gt lt changed from to) and value, then | and the watch (see watches/).\n";
constexpr std::string_view kStart = "start";
constexpr std::string_view kSplit = "split";
constexpr std::string_view kReset = "reset";
constexpr char kWatchMark = '|';
constexpr std::string_view kValueWord = "value";  // in a test's label, where the value goes

auto trim(std::string_view s) -> std::string_view {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

auto take_field(std::string_view& line) -> std::string_view {
  line = trim(line);
  size_t end = 0;
  while (end < line.size() && line[end] != ' ' && line[end] != '\t') end++;
  const std::string_view field = line.substr(0, end);
  line.remove_prefix(end);
  return field;
}

auto find_test(std::string_view key) -> std::optional<debug::Test> {
  for (const debug::TestInfo& t : debug::kTests) {
    if (t.key == key) return t.id;
  }
  return std::nullopt;
}

auto rule_text(std::string_view target, const Rule& rule) -> std::string {
  const debug::TestInfo& t = debug::info(rule.condition.test);
  std::string out(target);
  out += ' ';
  out += t.key;
  if (t.uses_value) out += " " + debug::format_value(rule.condition.value, rule.watch.type);
  out += " | " + debug::format_watch_line(rule.watch);
  return out + '\n';
}

auto same_address(const debug::Watch& a, const debug::Watch& b) -> bool {
  return a.region == b.region && a.offset == b.offset && a.type == b.type;
}

auto resolve(Rule& rule, std::vector<debug::Watch>& watches) -> bool {
  auto it = std::find_if(watches.begin(), watches.end(),
                         [&rule](const debug::Watch& w) { return w.id == rule.condition.watch && same_address(w, rule.watch); });
  if (it == watches.end()) {
    it = std::find_if(watches.begin(), watches.end(), [&rule](const debug::Watch& w) { return same_address(w, rule.watch); });
  }
  bool added = false;
  if (it == watches.end()) {
    debug::Watch w = rule.watch;
    w.id = debug::kNoWatch;
    w.frozen = false;
    watches.push_back(std::move(w));
    debug::assign_watch_ids(watches);
    it = watches.end() - 1;
    added = true;
  }
  rule.watch = *it;
  rule.watch.frozen = false;
  rule.watch.value = 0;
  rule.condition.watch = it->id;
  return added;
}

}  // namespace

auto autosplit_path(std::string_view lss_path) -> std::string {
  std::string path(lss_path);
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) path.resize(dot);
  return path + std::string(kAutoSplitExtension);
}

auto format_autosplits(const Run& run) -> std::string {
  std::string out;
  if (run.auto_start) out += rule_text(kStart, *run.auto_start);
  for (size_t i = 0; i < run.segments.size(); i++) {
    if (run.segments[i].auto_split) out += rule_text(std::string(kSplit) + " " + std::to_string(i + 1), *run.segments[i].auto_split);
  }
  if (run.auto_reset) out += rule_text(kReset, *run.auto_reset);
  return out.empty() ? out : std::string(kHeader) + out;
}

auto parse_autosplits(std::string_view text, Run& run) -> int {
  run.auto_start.reset();
  run.auto_reset.reset();
  for (Segment& s : run.segments) s.auto_split.reset();
  int count = 0;
  while (!text.empty()) {
    const size_t eol = text.find('\n');
    std::string_view line = trim(text.substr(0, eol));
    text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
    if (line.empty() || line.front() == '#') continue;
    const size_t bar = line.find(kWatchMark);
    if (bar == std::string_view::npos) continue;
    auto watch = debug::parse_watch_line(line.substr(bar + 1));
    line = line.substr(0, bar);
    if (!watch) continue;

    const std::string_view target = take_field(line);
    std::optional<Rule>* slot = nullptr;
    if (target == kStart) {
      slot = &run.auto_start;
    } else if (target == kReset) {
      slot = &run.auto_reset;
    } else if (target == kSplit) {
      const std::string_view n = take_field(line);
      size_t index = 0;
      const auto [end, ec] = std::from_chars(n.data(), n.data() + n.size(), index);
      if (ec != std::errc{} || end != n.data() + n.size() || index == 0 || index > run.segments.size()) continue;
      slot = &run.segments[index - 1].auto_split;
    }
    if (!slot) continue;
    const auto test = find_test(take_field(line));
    if (!test) continue;
    uint32_t value = 0;
    if (debug::info(*test).uses_value) {
      const auto v = debug::parse_value(take_field(line), watch->type);
      if (!v) continue;
      value = *v;
    }
    *slot = Rule{{watch->id, *test, value}, *watch};
    count++;
  }
  return count;
}

auto resolve_rules(Run& run, std::vector<debug::Watch>& watches) -> bool {
  bool grew = false;
  for (std::optional<Rule>* r : {&run.auto_start, &run.auto_reset}) {
    if (*r) grew |= resolve(**r, watches);
  }
  for (Segment& s : run.segments) {
    if (s.auto_split) grew |= resolve(*s.auto_split, watches);
  }
  return grew;
}

auto describe(const Rule& rule) -> std::string {
  const debug::TestInfo& t = debug::info(rule.condition.test);
  std::string name = rule.watch.label.empty() ? debug::format_address(rule.watch.region, rule.watch.offset) : rule.watch.label;
  std::string label(t.label);
  const size_t at = label.find(kValueWord);
  if (t.uses_value && at != std::string::npos) label.replace(at, kValueWord.size(), debug::format_value(rule.condition.value, rule.watch.type));
  return name + " " + label;
}

}  // namespace bitforge::speedrun
