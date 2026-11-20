// SPDX-License-Identifier: MIT
#include "speedrun/lss.hpp"
#include "speedrun/xml.hpp"

#include <algorithm>
#include <charconv>

namespace bitforge::speedrun {

namespace {

using xml::Node;

constexpr std::string_view kRoot = "Run";
constexpr std::string_view kVersion = "1.7.0";  // the format version we write
constexpr std::string_view kSynced = "False";   // our start/end stamps come from the local clock

auto parse_int(std::string_view s) -> std::optional<int> {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
  int v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
  return v;
}

// <X><RealTime>..</RealTime><GameTime>..</GameTime></X>, or a legacy <X>time</X>.
auto read_times(const Node* n) -> Times {
  Times t;
  if (!n) return t;
  if (n->children.empty()) {
    if (!n->text.empty()) t.real = parse_timespan(n->text);
    return t;
  }
  for (const MethodInfo& m : kMethods) {
    if (const Node* c = n->child(m.key)) t.set(m.id, parse_timespan(c->text));
  }
  return t;
}

// Sets or removes the RealTime / GameTime children (others, like PauseTime, stay).
auto write_times(Node& n, const Times& t) -> void {
  for (const MethodInfo& m : kMethods) {
    const auto v = t.get(m.id);
    if (!v) {
      n.remove(m.key);
      continue;
    }
    n.ensure(m.key).text = format_timespan(*v);
  }
}

auto personal_best(Node& segment) -> Node& {
  Node& split_times = segment.ensure("SplitTimes");
  for (Node& c : split_times.children) {
    if (c.name == "SplitTime" && c.attribute("name") == kPersonalBest) return c;
  }
  Node& pb = split_times.add("SplitTime");
  pb.set_attribute("name", std::string(kPersonalBest));
  return pb;
}

auto read_segment(const Node& n, int index) -> Segment {
  Segment s;
  s.name = n.child_text("Name");
  s.source = index;
  if (const Node* split_times = n.child("SplitTimes")) {
    for (const Node& c : split_times->children) {
      if (c.name == "SplitTime" && c.attribute("name") == kPersonalBest) s.pb = read_times(&c);
    }
  }
  s.best = read_times(n.child("BestSegmentTime"));
  if (const Node* history = n.child("SegmentHistory")) {
    for (const Node& c : history->children) {
      if (c.name != "Time") continue;
      const auto id = parse_int(c.attribute("id").value_or(""));
      if (id) s.history.push_back({*id, read_times(&c)});
    }
  }
  return s;
}

// A new document's skeleton, in the order LiveSplit writes it.
auto new_document() -> Node {
  Node root;
  root.name = std::string(kRoot);
  root.set_attribute("version", std::string(kVersion));
  for (const char* name : {"GameIcon", "GameName", "CategoryName", "LayoutPath"}) root.add(name);
  Node& meta = root.add("Metadata");
  meta.add("Run").set_attribute("id", "");
  meta.add("Platform").set_attribute("usesEmulator", "True");
  meta.add("Region");
  meta.add("Variables");
  for (const char* name : {"Offset", "AttemptCount", "AttemptHistory", "Segments", "AutoSplitterSettings"}) root.add(name);
  return root;
}

auto new_segment() -> Node {
  Node n;
  n.name = "Segment";
  for (const char* name : {"Name", "Icon", "SplitTimes", "BestSegmentTime", "SegmentHistory"}) n.add(name);
  return n;
}

}  // namespace

auto read_lss(std::string_view text, std::string* error) -> std::optional<Run> {
  std::string why;
  const auto root = xml::parse(text, &why);
  if (!root || root->name != kRoot) {
    if (error) *error = root ? "not a LiveSplit splits file (no <Run>)" : "not valid XML: " + why;
    return std::nullopt;
  }
  Run run;
  run.game = root->child_text("GameName");
  run.category = root->child_text("CategoryName");
  run.offset = parse_timespan(root->child_text("Offset")).value_or(0);
  run.attempt_count = parse_int(root->child_text("AttemptCount")).value_or(0);
  if (const Node* history = root->child("AttemptHistory")) {
    for (const Node& c : history->children) {
      if (c.name != "Attempt") continue;
      Attempt a;
      a.id = parse_int(c.attribute("id").value_or("")).value_or(0);
      a.time = read_times(&c);
      a.started = std::string(c.attribute("started").value_or(""));
      a.ended = std::string(c.attribute("ended").value_or(""));
      run.attempts.push_back(std::move(a));
    }
  }
  if (const Node* segments = root->child("Segments")) {
    for (const Node& c : segments->children) {
      if (c.name == "Segment") run.segments.push_back(read_segment(c, static_cast<int>(run.segments.size())));
    }
  }
  return run;
}

auto write_lss(const Run& run, std::string_view original) -> std::string {
  auto parsed = original.empty() ? std::nullopt : xml::parse(original);
  Node root = parsed && parsed->name == kRoot ? std::move(*parsed) : new_document();
  root.ensure("GameName").text = run.game;
  root.ensure("CategoryName").text = run.category;
  root.ensure("Offset").text = format_timespan(run.offset);
  root.ensure("AttemptCount").text = std::to_string(run.attempt_count);

  // Attempts: the file's own <Attempt> (matched by id) keeps its other attributes and children.
  Node& history = root.ensure("AttemptHistory");
  std::vector<Node> old = std::move(history.children);
  history.children.clear();
  for (const Attempt& a : run.attempts) {
    const auto it = std::find_if(old.begin(), old.end(), [&a](const Node& n) {
      return n.name == "Attempt" && parse_int(n.attribute("id").value_or("")) == a.id;
    });
    Node& n = it != old.end() ? history.children.emplace_back(std::move(*it)) : history.add("Attempt");
    n.set_attribute("id", std::to_string(a.id));
    if (!a.started.empty()) {
      n.set_attribute("started", a.started);
      if (!n.attribute("isStartedSynced")) n.set_attribute("isStartedSynced", std::string(kSynced));
    }
    if (!a.ended.empty()) {
      n.set_attribute("ended", a.ended);
      if (!n.attribute("isEndedSynced")) n.set_attribute("isEndedSynced", std::string(kSynced));
    }
    write_times(n, a.time);
  }

  // Segments: one read from this file keeps its <Segment> (icon, other comparisons).
  Node& segments = root.ensure("Segments");
  std::vector<Node> old_segments;
  for (Node& c : segments.children) {
    if (c.name == "Segment") old_segments.push_back(std::move(c));
  }
  segments.children.clear();
  std::vector<bool> used(old_segments.size(), false);
  for (const Segment& s : run.segments) {
    const auto source = static_cast<size_t>(s.source);
    const bool reuse = s.source >= 0 && source < old_segments.size() && !used[source];
    if (reuse) used[source] = true;
    Node& n = segments.children.emplace_back(reuse ? std::move(old_segments[source]) : new_segment());
    n.ensure("Name").text = s.name;
    write_times(personal_best(n), s.pb);
    write_times(n.ensure("BestSegmentTime"), s.best);
    Node& times = n.ensure("SegmentHistory");
    times.children.clear();
    for (const HistoryTime& h : s.history) {
      Node& t = times.add("Time");
      t.set_attribute("id", std::to_string(h.attempt));
      write_times(t, h.time);
    }
  }
  return xml::write(root);
}

}  // namespace bitforge::speedrun
