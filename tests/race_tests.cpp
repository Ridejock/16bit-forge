// SPDX-License-Identifier: MIT
// Race mode (speedrun/race.hpp): the guard table, the run timer's race verification (clean,
// verified, and each way a race run stops counting), the .race sidecar beside a LiveSplit .lss
// (which stays byte for byte what it would be without it), the verification summary, SHA-1,
// and the core's own gating on the built-in test ROM: turning race mode on power-cycles the
// game, and cheats, freezes, memory edits, state loads and saves, timeline saves and snapshot
// loads are refused until it is turned off. Headless, no ROM files.
#include "test_rom.hpp"
#include "debug/memory_watch.hpp"
#include "frontend/emulator.hpp"
#include "speedrun/lss.hpp"
#include "speedrun/race.hpp"
#include "speedrun/run_timer.hpp"
#include "speedrun/splits_file.hpp"
#include "utils/logging.hpp"
#include "utils/sha1.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace bitforge;
using namespace bitforge::speedrun;

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

constexpr Nanos kSec = kNanosPerSecond;
constexpr int64_t kUnixNow = 1'700'000'000;

auto at(Nanos mono) -> Clock { return {mono, kUnixNow + mono / kSec}; }
auto frame(uint64_t n, bool race, uint32_t tamper = 0, Jump jump = Jump::None) -> FrameInfo {
    return {n, jump, tamper, at(static_cast<Nanos>(n) * kSec / 60), race};
}
const ConditionTest kNever = [](const debug::Condition&) { return false; };

auto read_text(const std::filesystem::path& p) -> std::string {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A 2-segment run, timed from frame 1: start, `frames` frames per segment, both split.
auto run_two(RunTimer& t, bool race_frames, uint64_t& n, uint32_t tamper_at = 0, Jump jump_at = Jump::None) -> void {
    t.start(at(0));
    for (int seg = 0; seg < 2; seg++) {
        for (int i = 0; i < 30; i++) {
            n++;
            const bool hit = n == tamper_at;
            t.on_frame(frame(n, race_frames, hit && jump_at == Jump::None ? static_cast<uint32_t>(debug::kTamperFreeze) : 0u, hit ? jump_at : Jump::None), kNever);
        }
        t.split(at(static_cast<Nanos>(n) * kSec / 60));
    }
}

// ---- The guard table --------------------------------------------------------------------------

auto test_guards() -> void {
    for (size_t i = 0; i < kGuards.size(); i++) {
        const GuardInfo& g = kGuards[i];
        CHECK(static_cast<size_t>(g.id) == i);  // the table is indexed by Guard
        CHECK(!g.key.empty() && !g.label.empty());
        CHECK(!race_allows(true, g.id) && race_allows(false, g.id));  // race mode blocks every one, off allows all
        CHECK(blocked_message(g.id).find(g.label) == 0 && blocked_tooltip(g.id).find("race mode") != std::string::npos);
        for (size_t j = 0; j < i; j++) CHECK(kGuards[j].key != g.key);
    }
    CHECK(kRaceBlocks == (1u << kGuards.size()) - 1);
    for (size_t i = 0; i < kRaceStatuses.size(); i++) CHECK(static_cast<size_t>(kRaceStatuses[i].id) == i);
    CHECK(info(RaceStatus::Off).tag.empty() && !info(RaceStatus::Verified).tag.empty());
    CHECK(describe_breaks(kBreakRaceOff | kBreakJump).find("turned off") != std::string::npos);
    CHECK(describe_breaks(kBreakRaceOff | kBreakJump).find("; ") != std::string::npos);
    CHECK(describe_breaks(0).empty());
}

// ---- Run timer verification -----------------------------------------------------------------

auto test_verification() -> void {
    uint64_t n = 0;
    {  // Not a race run: no status, "modified" handling as before.
        RunTimer t;
        t.set_run(make_run("G", "Any%", 2));
        CHECK(t.race_status() == RaceStatus::Off);
        run_two(t, false, n);
        CHECK(t.state() == TimerState::Ended && t.race_status() == RaceStatus::Off);
        t.reset(at(100 * kSec));
        CHECK(!t.run().attempts.back().race_verified);
    }
    {  // A clean race run: Clean while running, Verified at the end and in the history.
        RunTimer t;
        t.set_run(make_run("G", "Any%", 2));
        t.set_race(true);
        CHECK(t.race() && t.race_status() == RaceStatus::Off);  // ready: no run yet
        t.start(at(0));
        CHECK(t.race_status() == RaceStatus::Clean);
        t.reset(at(kSec));
        run_two(t, true, n);
        CHECK(t.race_status() == RaceStatus::Verified && t.race_breaks() == 0);
        // Turning race mode off after the finish doesn't undo it...
        t.set_race(false);
        CHECK(t.race_status() == RaceStatus::Verified);
        t.reset(at(100 * kSec));
        CHECK(t.run().attempts.size() == 2 && t.run().attempts.back().race_verified && !t.run().attempts.front().race_verified);
        CHECK(verified_runs(t.run()) == 1);
        // ...but an undo past the finish with race mode off does, on the next frame.
        t.set_race(true);
        run_two(t, true, n);
        t.set_race(false);
        t.undo();
        t.on_frame(frame(++n, false), kNever);
        CHECK(t.race_status() == RaceStatus::Broken && (t.race_breaks() & kBreakRaceOff));
    }
    {  // Race mode turned off mid-run.
        RunTimer t;
        t.set_run(make_run("G", "Any%", 2));
        t.set_race(true);
        t.start(at(0));
        t.on_frame(frame(++n, true), kNever);
        t.set_race(false);
        CHECK(t.race_status() == RaceStatus::Broken && t.race_breaks() == kBreakRaceOff);
        t.set_race(true);  // turning it back on doesn't mend it
        CHECK(t.race_status() == RaceStatus::Broken);
        t.split(at(kSec));
        t.split(at(2 * kSec));
        CHECK(t.state() == TimerState::Ended && t.race_status() == RaceStatus::Broken);
        t.reset(at(3 * kSec));
        CHECK(!t.run().attempts.back().race_verified);
    }
    {  // A frame with race mode off (the frame hook saw it before set_race did).
        RunTimer t;
        t.set_run(make_run("G", "Any%", 1));
        t.set_race(true);
        t.start(at(0));
        t.on_frame(frame(++n, false), kNever);
        CHECK(t.race_status() == RaceStatus::Broken && (t.race_breaks() & kBreakRaceOff));
    }
    {  // Tampering (a freeze, cheat or edit) during a race run.
        RunTimer t;
        t.set_run(make_run("G", "Any%", 2));
        t.set_race(true);
        const uint64_t from = n;
        run_two(t, true, n, from + 40);
        CHECK(t.race_status() == RaceStatus::Broken && (t.race_breaks() & kBreakTamper) && t.tampered());
    }
    for (const Jump jump : {Jump::Load, Jump::Rewind}) {  // a state load or rewind during a race run
        RunTimer t;
        t.set_run(make_run("G", "Any%", 2));
        t.set_race(true);
        const uint64_t from = n;
        run_two(t, true, n, from + 10, jump);
        CHECK(t.race_status() == RaceStatus::Broken && (t.race_breaks() & kBreakJump));
    }
    {  // A console reset (Emulation > Reset) is part of playing: still verified.
        RunTimer t;
        t.set_run(make_run("G", "Any%", 2));
        t.set_race(true);
        const uint64_t from = n;
        run_two(t, true, n, from + 10, Jump::Reset);
        CHECK(t.race_status() == RaceStatus::Verified);
    }
    {  // Started before race mode came on: not a race run (the app resets it when race mode starts).
        RunTimer t;
        t.set_run(make_run("G", "Any%", 1));
        t.start(at(0));
        t.set_race(true);
        t.on_frame(frame(++n, true), kNever);
        t.split(at(kSec));
        CHECK(t.race_status() == RaceStatus::Off);
    }
}

// ---- The .race sidecar ------------------------------------------------------------------------

auto test_sidecar() -> void {
    CHECK(race_sidecar_path("dir/Game.lss") == std::filesystem::path("dir/Game.race").string());
    Run run = make_run("Game", "Any%", 1);
    run.attempts = {{1, {kSec, kSec}, "", "", false}, {2, {}, "", "", false}, {3, {2 * kSec, 2 * kSec}, "", "", true}};
    CHECK(format_race_sidecar(make_run("G", "c", 1)).empty());
    const std::string text = format_race_sidecar(run);
    CHECK(text.find("verified 3\n") != std::string::npos && text.find("verified 1") == std::string::npos);
    Run back = run;
    for (Attempt& a : back.attempts) a.race_verified = false;
    // Unknown ids, unfinished attempts, junk and comments are skipped.
    CHECK(parse_race_sidecar(text + "verified 2\nverified 99\nnonsense\n  # verified 1\n", back) == 1);
    CHECK(back == run);

    // Through the files: the .lss is the same with or without verified attempts (LiveSplit
    // never sees ours), the sidecar comes back and goes away when nothing is verified.
    const auto dir = std::filesystem::temp_directory_path() / "bitforge_race_tests";
    std::filesystem::remove_all(dir);
    SplitsFile file{(dir / "Game.lss").string(), {}};
    CHECK(save_splits(run, file));
    const std::string lss = read_text(file.path);
    CHECK(std::filesystem::exists(race_sidecar_path(file.path)));
    const auto loaded = load_splits(file.path);
    CHECK(loaded && loaded->run.attempts.size() == 3 && loaded->run.attempts[2].race_verified && verified_runs(loaded->run) == 1);
    Run plain = run;
    plain.attempts[2].race_verified = false;
    SplitsFile plain_file{(dir / "Plain.lss").string(), {}};
    CHECK(save_splits(plain, plain_file));
    CHECK(read_text(plain_file.path) == lss);
    CHECK(!std::filesystem::exists(race_sidecar_path(plain_file.path)));
    CHECK(lss.find("race") == std::string::npos);
    // A rewrite without verified attempts removes the sidecar.
    CHECK(save_splits(plain, file) && !std::filesystem::exists(race_sidecar_path(file.path)));
    std::filesystem::remove_all(dir);
}

// ---- The verification summary -----------------------------------------------------------------

auto test_summary() -> void {
    Verification v;
    v.emulator = "16bit Forge 9.9.9";
    v.game = "Test \"Game\"";
    v.category = "Any%";
    v.rom_file = "test.sfc";
    v.rom_crc32 = 0x1234ABCD;
    v.rom_sha1 = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
    v.attempt = 7;
    v.started = "11/14/2023 22:13:20";
    v.final_time = {83 * kSec, 80 * kSec};
    v.game_frames = 4808;
    v.lag_frames = 312;
    v.splits = {{"One", {40 * kSec, 39 * kSec}}, {"Two", {83 * kSec, 80 * kSec}}};
    const std::string json = format_verification(v);
    CHECK(json.find("\"game\": \"Test \\\"Game\\\"\"") != std::string::npos);  // escaped
    CHECK(json.find("\"crc32\": \"1234ABCD\"") != std::string::npos);
    CHECK(json.find("\"standard\": \"NTSC\"") != std::string::npos && json.find("\"frame_rate\": 60.0988139") != std::string::npos);
    CHECK(json.find("\"game_frames\": 4808,\n  \"lag_frames\": 312,") != std::string::npos);
    CHECK(json.find("\"race_verified\": true") != std::string::npos);
    CHECK(json.find("\"RealTime\": \"1:23.00\", \"GameTime\": \"1:20.00\"") != std::string::npos);
    for (const GuardInfo& g : kGuards) CHECK(json.find("\"" + std::string(g.key) + "\"") != std::string::npos);
    CHECK(json.find("SNES") == std::string::npos);
    // Balanced braces and brackets (a cheap well-formedness check).
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < json.size(); i++) {
        if (json[i] == '\\') { i++; continue; }
        if (json[i] == '"') in_string = !in_string;
        if (in_string) continue;
        if (json[i] == '{' || json[i] == '[') depth++;
        if (json[i] == '}' || json[i] == ']') depth--;
        CHECK(depth >= 0);
    }
    CHECK(depth == 0 && !in_string);
    CHECK(verification_path("dir/Game.lss", 12) == std::filesystem::path("dir/Game.attempt-12.verified.json").string());
}

// ---- SHA-1 (FIPS 180-4 test vectors) ----------------------------------------------------------

auto test_sha1() -> void {
    auto hex = [](std::string_view s) { return utils::sha1_hex({reinterpret_cast<const uint8_t*>(s.data()), s.size()}); };
    CHECK(hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hex(std::string(1'000'000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    // Block edges: 55, 56 and 64 bytes pad into one or two blocks.
    CHECK(hex(std::string(55, 'a')) == "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    CHECK(hex(std::string(64, 'a')) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    const std::string rom = test_rom::write_test_rom(5);
    const auto bytes = test_rom::make_test_rom(5);
    CHECK(utils::sha1_file(rom) == utils::sha1_hex(bytes));
    CHECK(!utils::sha1_file(rom + ".missing"));
}

// ---- The core's gating ------------------------------------------------------------------------

auto test_core_gating() -> void {
    using frontend::Emulator;
    const auto dir = std::filesystem::temp_directory_path() / "bitforge_race_tests_core";
    std::filesystem::remove_all(dir);
    const std::string rom = test_rom::write_test_rom(6);
    constexpr uint32_t kSpare = 0x1FF0;  // WRAM the test ROM never touches
    constexpr uint32_t kFrozen = kSpare + 3;
    uint8_t power_on = 0;       // kSpare, and the byte after it, at power-on
    uint8_t power_on_next = 0;
    uint8_t power_on_frozen = 0;  // kFrozen at power-on
    {
        Emulator fresh;
        fresh.set_data_dir(dir.string());
        CHECK(fresh.load(rom));
        power_on = fresh.memory(debug::Region::Wram)[kSpare];
        power_on_next = fresh.memory(debug::Region::Wram)[kSpare + 1];
        power_on_frozen = fresh.memory(debug::Region::Wram)[kFrozen];
    }
    Emulator emu;
    emu.set_data_dir(dir.string());
    CHECK(emu.load(rom));
    for (int i = 0; i < 30; i++) emu.run_frame();
    CHECK(emu.save_state(1).empty());  // a state from before race mode
    const auto snapshot = emu.save_snapshot();

    // Everything race mode stops, set up while it is off: a cheat on kSpare, a freeze, an edit.
    const uint8_t cheat_value = static_cast<uint8_t>(power_on ^ 0xA5);
    char code[16];
    std::snprintf(code, sizeof(code), "7E%04X%02X", kSpare, cheat_value);
    CHECK(emu.set_cheats({{true, code, "spare"}}));
    const uint8_t freeze_value = static_cast<uint8_t>(power_on_frozen ^ 0x3C);
    debug::Watch frozen{"Spare", debug::Region::Wram, kFrozen, {}, true, freeze_value, 0};
    CHECK(emu.set_watches({frozen}));
    const uint8_t poked = static_cast<uint8_t>(power_on_next ^ 0x5A);
    CHECK(emu.write_memory(debug::Region::Wram, kSpare + 1, debug::ValueSize::Bits8, poked));
    emu.run_frame();
    CHECK(emu.memory(debug::Region::Wram)[kSpare] == cheat_value && emu.memory(debug::Region::Wram)[kFrozen] == freeze_value);
    CHECK(emu.memory_tampered() == (debug::kTamperFreeze | debug::kTamperCheat | debug::kTamperPoke));

    // On: a power cycle. Fresh memory, frame 0, a reset jump; the cheat stays listed but off.
    emu.take_jump();
    emu.set_race(true);
    CHECK(emu.race() && emu.loaded() && emu.frame_count() == 0 && emu.take_jump() == Jump::Reset);
    CHECK(emu.memory(debug::Region::Wram)[kSpare + 1] == power_on_next && emu.memory(debug::Region::Wram)[kSpare] == power_on);
    CHECK(emu.cheats().size() == 1 && emu.cheats()[0].enabled);
    CHECK(emu.watches().size() == 1 && !emu.watches()[0].frozen);
    CHECK(emu.memory_tampered() == 0);
    for (int i = 0; i < 10; i++) emu.run_frame();
    CHECK(emu.memory(debug::Region::Wram)[kSpare] != cheat_value);  // not applied
    CHECK(emu.memory_tampered() == 0);

    // Refused whatever asks: a freeze, an edit, state loads and saves, snapshots, timeline saves.
    CHECK(emu.set_watches({frozen}) && !emu.watches()[0].frozen);
    for (int i = 0; i < 2; i++) emu.run_frame();
    CHECK(emu.memory(debug::Region::Wram)[kFrozen] == power_on_frozen);  // not written
    CHECK(!emu.write_memory(debug::Region::Wram, kSpare, debug::ValueSize::Bits8, 1));
    const uint64_t at_frame = emu.frame_count();
    CHECK(!emu.load_state(1).empty() && emu.frame_count() == at_frame);
    CHECK(!emu.save_state(2).empty() && !std::filesystem::exists(emu.state_path(2)));
    CHECK(!emu.load_snapshot(snapshot) && emu.frame_count() == at_frame);
    CHECK(!emu.save_snapshot_to_slot(3, snapshot, nullptr, 0, 0).empty() && !std::filesystem::exists(emu.state_path(3)));
    CHECK(emu.memory_tampered() == 0);
    // The cheat list can still be edited (it's saved, not applied).
    CHECK(emu.set_cheats({{true, code, "edited"}}) && emu.memory_tampered() == 0);

    // Off: the cheat applies again; loads and edits work; no power cycle.
    const uint64_t before_off = emu.frame_count();
    emu.set_race(false);
    CHECK(!emu.race() && emu.frame_count() == before_off);
    emu.run_frame();
    CHECK(emu.memory(debug::Region::Wram)[kSpare] == cheat_value && (emu.memory_tampered() & debug::kTamperCheat));
    CHECK(emu.load_state(1).empty() && emu.frame_count() == 30);
    CHECK(emu.write_memory(debug::Region::Wram, kSpare + 2, debug::ValueSize::Bits8, 7));

    // Race mode set before a game loads: the boot is the clean start; nothing to restart.
    Emulator early;
    early.set_data_dir(dir.string());
    early.set_race(true);
    CHECK(early.race() && !early.loaded());
    CHECK(early.load(rom) && early.race() && early.memory_tampered() == 0);
    CHECK(early.cheats().size() == 1);  // the file from above
    early.run_frame();
    CHECK(early.memory(debug::Region::Wram)[kSpare] != cheat_value);
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    utils::Logger::set_level(utils::LogLevel::Error);
    std::printf("=== Race mode tests ===\n\n");
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"Guards: table, race mode blocks all, messages", test_guards},
        {"Run timer: clean, verified, broken (off, tamper, jumps)", test_verification},
        {".race sidecar: round trip, .lss unchanged", test_sidecar},
        {"Verification summary (JSON)", test_summary},
        {"SHA-1 test vectors and files", test_sha1},
        {"Core gating on the built-in ROM: power cycle, refusals", test_core_gating},
    };
    int passed = 0, failed = 0;
    for (const Test& t : tests) {
        std::printf("  %-58s", t.name);
        std::fflush(stdout);
        const int before = checks_failed;
        t.fn();
        if (checks_failed == before) {
            std::printf(" PASS\n");
            passed++;
        } else {
            std::printf("\n  FAIL\n");
            failed++;
        }
    }
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
