// SPDX-License-Identifier: MIT
#include "speedrun/race.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace bitforge::speedrun {

namespace {

constexpr std::string_view kVerifiedWord = "verified";
constexpr std::string_view kVerificationFormat = "16bitforge-race-verification";
constexpr int kVerificationVersion = 1;

struct BreakInfo {
  uint32_t bit;
  std::string_view text;
};
constexpr std::array<BreakInfo, 3> kBreaks = {{
    {kBreakRaceOff, "race mode was turned off"},
    {kBreakTamper, "a cheat, freeze or memory edit was active"},
    {kBreakJump, "a state load, rewind or timeline jump happened"},
}};

auto replace_extension(std::string_view path, std::string_view extension) -> std::string {
  return std::filesystem::path(path).replace_extension(extension).string();
}

// A JSON string literal.
auto json_string(std::string_view s) -> std::string {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
        out += esc;
      } else {
        out += c;
      }
    }
  }
  return out + "\"";
}

// {"real": "1:23.45", "game": "1:20.00"}, null for a missing time.
auto times_json(const Times& t) -> std::string {
  auto one = [](std::optional<Nanos> v) { return v ? json_string(format_time(*v)) : std::string("null"); };
  return "{" + json_string(info(Method::Real).key) + ": " + one(t.real) + ", " + json_string(info(Method::Game).key) + ": " + one(t.game) + "}";
}

}  // namespace

auto blocked_message(Guard g) -> std::string { return std::string(info(g).label) + " - off in race mode"; }

auto blocked_tooltip(Guard g) -> std::string {
  return std::string(info(g).label) + " - off in race mode (Speedrun > Race mode), so the run can be verified.";
}

auto describe_breaks(uint32_t breaks) -> std::string {
  std::string out;
  for (const BreakInfo& b : kBreaks) {
    if ((breaks & b.bit) == 0) continue;
    if (!out.empty()) out += "; ";
    out += b.text;
  }
  return out;
}

auto race_sidecar_path(std::string_view lss_path) -> std::string { return replace_extension(lss_path, kRaceExtension); }

auto format_race_sidecar(const Run& run) -> std::string {
  std::string out;
  for (const Attempt& a : run.attempts) {
    if (a.race_verified) out += std::string(kVerifiedWord) + " " + std::to_string(a.id) + "\n";
  }
  if (out.empty()) return out;
  return "# 16bit Forge: attempts finished in race mode (see the .lss AttemptHistory)\n" + out;
}

auto parse_race_sidecar(std::string_view text, Run& run) -> int {
  for (Attempt& a : run.attempts) a.race_verified = false;
  int marked = 0;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
    std::istringstream words(line);
    std::string word;
    int id = 0;
    if (!(words >> word >> id) || word != kVerifiedWord) continue;
    for (Attempt& a : run.attempts) {
      if (a.id == id && a.finished() && !a.race_verified) {
        a.race_verified = true;
        marked++;
      }
    }
  }
  return marked;
}

auto verified_runs(const Run& run) -> int {
  return static_cast<int>(std::count_if(run.attempts.begin(), run.attempts.end(), [](const Attempt& a) { return a.race_verified; }));
}

auto format_verification(const Verification& v) -> std::string {
  char crc[16];
  std::snprintf(crc, sizeof(crc), "%08X", v.rom_crc32);
  std::string blocked;
  for (const GuardInfo& g : kGuards) {
    if (race_allows(true, g.id)) continue;
    blocked += (blocked.empty() ? "" : ", ") + json_string(g.key);
  }
  std::ostringstream out;
  out << "{\n"
      << "  \"format\": " << json_string(kVerificationFormat) << ",\n"
      << "  \"version\": " << kVerificationVersion << ",\n"
      << "  \"emulator\": " << json_string(v.emulator) << ",\n"
      << "  \"game\": " << json_string(v.game) << ",\n"
      << "  \"category\": " << json_string(v.category) << ",\n"
      << "  \"rom\": {\"file\": " << json_string(v.rom_file) << ", \"crc32\": " << json_string(crc) << ", \"sha1\": " << json_string(v.rom_sha1)
      << "},\n"
      << "  \"timing\": {\"standard\": " << json_string(info(v.standard).label) << ", \"frame_rate\": " << info(v.standard).frame_rate
      << "},\n"
      << "  \"race_mode\": true,\n"
      << "  \"race_verified\": true,\n"
      << "  \"started_from_power_on\": true,\n"
      << "  \"blocked\": [" << blocked << "],\n"
      << "  \"attempt\": " << v.attempt << ",\n"
      << "  \"started_utc\": " << json_string(v.started) << ",\n"
      << "  \"ended_utc\": " << json_string(v.ended) << ",\n"
      << "  \"final_time\": " << times_json(v.final_time) << ",\n"
      << "  \"game_frames\": " << v.game_frames << ",\n"
      << "  \"splits\": [";
  for (size_t i = 0; i < v.splits.size(); i++) {
    out << (i ? ",\n" : "\n") << "    {\"name\": " << json_string(v.splits[i].name) << ", \"time\": " << times_json(v.splits[i].time) << "}";
  }
  out << (v.splits.empty() ? "]\n" : "\n  ]\n") << "}\n";
  return out.str();
}

auto verification_path(std::string_view lss_path, int attempt) -> std::string {
  return replace_extension(lss_path, ".attempt-" + std::to_string(attempt) + ".verified.json");
}

}  // namespace bitforge::speedrun
