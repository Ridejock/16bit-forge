// SPDX-License-Identifier: MIT
// Movies (speedrun/movie.hpp): the .bfm format (round trip, runs of frames, damaged files), and
// the core recording and playing on the built-in test ROM: a replay matches the recording frame
// for frame (picture and machine state), from power-on and from a save state, through resets and
// device changes, with the player's input ignored. Save states and rewind snapshots during a
// recording cut it there (rerecords); read-only playback goes back without cutting, and refuses
// a state from another take. A replayed run is never race verified. Headless, no ROM files.
#include "test_rom.hpp"
#include "frontend/emulator.hpp"
#include "speedrun/movie.hpp"
#include "speedrun/race.hpp"
#include "speedrun/run_timer.hpp"
#include "utils/logging.hpp"
#include "utils/sha1.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace bitforge;
using namespace bitforge::speedrun;
using bitforge::frontend::Emulator;
using bitforge::frontend::MovieMode;
namespace fs = std::filesystem;

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

auto temp_dir(const char* name) -> fs::path {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// The picture and the whole machine after a frame, as one number (FNV-1a).
auto frame_hash(Emulator& emu) -> uint64_t {
    uint64_t h = 0xCBF29CE484222325ull;
    auto add = [&h](const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    };
    add(emu.framebuffer(), static_cast<size_t>(emu.width() * emu.height()) * sizeof(uint32_t));
    const std::vector<uint8_t> state = emu.save_snapshot();
    add(state.data() + 16, state.size() - 16);  // the machine, not the snapshot's frame numbers
    return h;
}

// Pads that change every few frames, so the test ROM's picture follows them.
auto pad(int frame, int seed) -> uint16_t {
    const uint32_t x = static_cast<uint32_t>(frame / 5 + seed * 13) * 2654435761u;
    return static_cast<uint16_t>((x >> 12) & 0xFFF0);
}

// One live frame: scripted input (pad 2 plugged in and out, a multitap for a while), then the frame.
auto live_frame(Emulator& emu, int f, int seed) -> uint64_t {
    emu.set_port_device(1, f >= 10 && f < 25 ? Device::Multitap : Device::Gamepad);
    emu.set_port_connected(1, f % 50 < 30);
    emu.set_joypad(0, pad(f, seed));
    emu.set_pad(1, 0, pad(f, seed + 1));
    emu.set_pad(1, 2, pad(f, seed + 2));
    emu.run_frame();
    return frame_hash(emu);
}

// A frame of playback, with junk from the "player" that must be ignored.
auto played_frame(Emulator& emu, int f) -> uint64_t {
    emu.set_joypad(0, static_cast<uint16_t>(0xFFF0 ^ (f * 0x1110)));
    emu.set_port_device(1, Device::Mouse);
    emu.add_mouse(1, 5, -3, true, false);
    emu.run_frame();
    return frame_hash(emu);
}

auto load_movie(const std::string& path) -> Movie {
    Movie m;
    CHECK(read_movie(path, m).empty());
    return m;
}

// ---- The file format --------------------------------------------------------------------------

auto test_format() -> void {
    Movie m;
    m.id = 0x1234'5678'9ABC'DEF1ull;
    m.start = MovieStart::SaveState;
    m.race = true;
    m.rerecords = 7;
    m.rom_crc32 = 0xCAFEF00D;
    for (size_t i = 0; i < m.rom_sha1.size(); i++) m.rom_sha1[i] = static_cast<uint8_t>(i * 11);
    m.recorded = 1'700'000'000;
    m.emulator = "16bit Forge test";
    m.rom_file = "Game (USA).sfc";
    m.author = "Ervin";
    m.notes = "line 1\nline 2";
    m.state = {1, 2, 3, 4, 5};
    FrameInput held;
    held.ports[0].words[0] = 0x8000;
    held.ports[1] = {Device::None, {}};
    FrameInput tap = held;
    tap.ports[1] = {Device::Multitap, {1, 2, 3, 4}};
    FrameInput mouse = held;
    mouse.events = kEventReset;
    mouse.ports[1] = mouse_input(-5, 300, true, false);
    FrameInput scope = held;
    ScopeInput aim;
    aim.x = 200;
    aim.y = 150;
    aim.offscreen = false;
    aim.fire = true;
    aim.turbo = true;
    scope.ports[1] = scope_input(aim);
    m.frames.assign(1000, held);
    m.frames.push_back(tap);
    m.frames.push_back(mouse);
    m.frames.insert(m.frames.end(), 500, scope);
    m.frames.push_back(FrameInput{kEventPower, {}});

    const std::vector<uint8_t> file = encode_movie(m);
    CHECK(file.size() < 400);  // 1503 frames in 5 runs
    Movie back;
    CHECK(decode_movie(file, back) == MovieResult::Ok);
    CHECK(back.id == m.id && back.start == m.start && back.race && back.rerecords == 7 && back.rom_crc32 == m.rom_crc32);
    CHECK(back.rom_sha1 == m.rom_sha1 && back.recorded == m.recorded && back.emulator == m.emulator && back.rom_file == m.rom_file);
    CHECK(back.author == m.author && back.notes == m.notes && back.state == m.state && back.frames == m.frames);
    CHECK(back.standard == VideoStandard::Ntsc && !back.verifiable());
    CHECK(encode_movie(back) == file);
    // Devices in their words.
    const PortInput mo = back.frames[1001].ports[1];
    CHECK(static_cast<int16_t>(mo.words[0]) == -5 && mo.words[1] == 300 && mo.words[2] == 1);
    const ScopeInput s = scope_from(back.frames[1002].ports[1]);
    CHECK(s.x == 200 && s.y == 150 && !s.offscreen && s.fire && s.turbo && !s.cursor && !s.pause);
    for (const DeviceWords& w : kDeviceWords) CHECK(device_words(w.device) == w.words && w.words <= kPortWords);
    // The first frames only (a save state's copy).
    Movie part;
    CHECK(decode_movie(encode_movie(m, 1001), part) == MovieResult::Ok && part.frames.size() == 1001);
    CHECK(is_prefix(part.frames, m.frames) && !is_prefix(m.frames, part.frames));
    part.frames.back() = mouse;
    CHECK(!is_prefix(part.frames, m.frames));
    // Power-on movies, verifiable ones.
    Movie power;
    power.race = true;
    CHECK(power.verifiable() && decode_movie(encode_movie(power), back) == MovieResult::Ok && back.frames.empty() && back.state.empty());
    power.rerecords = 1;
    CHECK(!power.verifiable());

    // Damaged files.
    std::vector<uint8_t> bad = file;
    bad[0] = 'X';
    CHECK(decode_movie(bad, back) == MovieResult::NotAMovie);
    CHECK(decode_movie(std::span(file).first(3), back) == MovieResult::NotAMovie);
    bad = file;
    bad[4] = static_cast<uint8_t>(kMovieVersion + 1);
    CHECK(decode_movie(bad, back) == MovieResult::WrongVersion);
    for (size_t cut : {size_t{20}, file.size() / 2, file.size() - 1}) CHECK(decode_movie(std::span(file).first(cut), back) == MovieResult::Corrupt);
    bad = file;
    bad.push_back(0);
    CHECK(decode_movie(bad, back) == MovieResult::Corrupt);
    bad = file;
    constexpr size_t kPortBytes = 1 + kPortWords * 2;
    bad[bad.size() - kPortBytes] = 0x77;  // the last frame's port 2 device
    CHECK(decode_movie(bad, back) == MovieResult::Corrupt);
    CHECK(describe(MovieResult::Corrupt) != std::string(describe(MovieResult::Ok)));

    // Files and names.
    const fs::path dir = temp_dir("bitforge_movie_tests_format");
    const std::string path = (dir / "sub" / "a.bfm").string();
    CHECK(write_movie(path, m) && read_movie(path, back).empty() && back.frames == m.frames);
    CHECK(!read_movie((dir / "none.bfm").string(), back).empty());
    CHECK(state_movie_path("states/Game.3.sst") == fs::path("states/Game.3.bfm").string());
    CHECK(attempt_movie_path("data", "Game", 12) == (fs::path("data") / "movies" / "Game" / "attempt-12.bfm").string());
    CHECK(new_movie_id() != 0 && new_movie_id() != new_movie_id());
    fs::remove_all(dir);
}

// ---- Record and replay ------------------------------------------------------------------------

auto test_power_on_replay() -> void {
    const fs::path dir = temp_dir("bitforge_movie_tests_replay");
    const std::string rom = test_rom::write_test_rom(9);
    const std::string path = (dir / "run.bfm").string();
    constexpr int kFrames = 300;
    std::vector<uint64_t> recorded;
    {
        Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom));
        for (int f = 0; f < 45; f++) live_frame(emu, f, 1);  // before the movie: none of this counts
        CHECK(emu.record_movie(MovieStart::PowerOn, path, {"16bit Forge test", "tester"}).empty());
        CHECK(emu.movie_mode() == MovieMode::Recording && emu.frame_count() == 0);
        for (int f = 0; f < kFrames; f++) {
            if (f == 150) emu.reset();
            recorded.push_back(live_frame(emu, f, 2));
        }
        CHECK(emu.movie_status().frame == kFrames && emu.movie_status().frames == kFrames);
        CHECK(emu.stop_movie().empty() && emu.movie_mode() == MovieMode::Off);
    }
    const Movie movie = load_movie(path);
    CHECK(movie.frames.size() == kFrames && movie.start == MovieStart::PowerOn && movie.author == "tester");
    CHECK(movie.frames[150].events == kEventReset && movie.frames[149].events == 0);
    CHECK(movie.frames[20].ports[1].device == Device::Multitap && movie.frames[35].ports[1].device == Device::None);
    CHECK(movie.rom_file == fs::path(rom).filename().string() && movie.rom_sha1 == utils::sha1(test_rom::make_test_rom(9)));
    CHECK(!movie.race && !movie.verifiable());
    {  // The same input live from a fresh boot gives the same frames (recording changes nothing);
       // other input doesn't (so the comparison below means something).
        Emulator live;
        live.set_data_dir(dir.string());
        CHECK(live.load(rom));
        int same = 0, other = 0;
        for (int f = 0; f < kFrames; f++) {
            if (f == 150) live.reset();
            same += live_frame(live, f, 2) == recorded[static_cast<size_t>(f)];
        }
        CHECK(live.load(rom));
        for (int f = 0; f < kFrames; f++) other += live_frame(live, f, 5) == recorded[static_cast<size_t>(f)];
        CHECK(same == kFrames && other < kFrames / 2);
    }

    // Played in another emulator, after other play: the same frames, whatever the player presses.
    Emulator emu;
    emu.set_data_dir(dir.string());
    CHECK(emu.load(rom));
    for (int f = 0; f < 77; f++) live_frame(emu, f, 3);
    CHECK(emu.play_movie(movie, path).empty() && emu.movie_mode() == MovieMode::Playing);
    int same = 0;
    for (int f = 0; f < kFrames; f++) {
        if (f == 100) emu.reset();  // the player's reset: ignored
        same += played_frame(emu, f) == recorded[static_cast<size_t>(f)];
    }
    CHECK(same == kFrames);
    CHECK(emu.movie_at_end() && emu.movie_status().frame == kFrames);
    emu.run_frame();  // past the end: the movie closes, live input from here
    CHECK(emu.movie_mode() == MovieMode::Off);
    fs::remove_all(dir);
}

auto test_state_start_replay() -> void {
    const fs::path dir = temp_dir("bitforge_movie_tests_state");
    const std::string rom = test_rom::write_test_rom(10);
    const std::string path = (dir / "from_state.bfm").string();
    std::vector<uint64_t> recorded;
    {
        Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom));
        for (int f = 0; f < 90; f++) live_frame(emu, f, 4);
        const uint64_t at = emu.frame_count();
        CHECK(emu.record_movie(MovieStart::SaveState, path, {}).empty());
        CHECK(emu.frame_count() == at && !emu.movie().state.empty());  // carries on where it was
        for (int f = 0; f < 120; f++) recorded.push_back(live_frame(emu, 90 + f, 4));
        CHECK(emu.stop_movie().empty());
    }
    const Movie movie = load_movie(path);
    CHECK(movie.start == MovieStart::SaveState && movie.frames.size() == 120 && !movie.verifiable());
    Emulator emu;
    emu.set_data_dir(dir.string());
    CHECK(emu.load(rom));
    CHECK(emu.play_movie(movie, path).empty() && emu.frame_count() == 90);
    int same = 0;
    for (int f = 0; f < 120; f++) same += played_frame(emu, f) == recorded[static_cast<size_t>(f)];
    CHECK(same == 120);
    // Race mode: a state movie can't start (it loads a state, or would make one).
    emu.set_race(true);
    CHECK(emu.movie_mode() == MovieMode::Off);  // turning race mode on ended it
    CHECK(!emu.play_movie(movie, path).empty() && !emu.record_movie(MovieStart::SaveState, path, {}).empty());
    CHECK(emu.movie_mode() == MovieMode::Off);
    fs::remove_all(dir);
}

// ---- Rerecording: save states and rewind ------------------------------------------------------

auto test_rerecords() -> void {
    const fs::path dir = temp_dir("bitforge_movie_tests_rerecord");
    const std::string rom = test_rom::write_test_rom(11);
    const std::string path = (dir / "tas.bfm").string();
    std::vector<uint64_t> take;  // the frames the movie ends up with
    Emulator emu;
    emu.set_data_dir(dir.string());
    CHECK(emu.load(rom));
    CHECK(emu.save_state(3).empty());  // from before the movie
    CHECK(emu.record_movie(MovieStart::PowerOn, path, {}).empty());
    emu.set_movie_read_only(false);
    for (int f = 0; f < 50; f++) take.push_back(live_frame(emu, f, 5));
    CHECK(emu.save_state(1).empty() && fs::exists(state_movie_path(emu.state_path(1))));
    for (int f = 50; f < 80; f++) live_frame(emu, f, 6);  // take A, dropped below
    CHECK(emu.save_state(2).empty());
    CHECK(!emu.load_state(3).empty() && emu.movie_status().frame == 80);  // not saved during the movie

    // Read+write: back to frame 50, the movie is cut there and recording goes on (take B).
    CHECK(emu.load_state(1).empty());
    CHECK(emu.movie_mode() == MovieMode::Recording && emu.movie_status().frame == 50 && emu.movie_status().frames == 50);
    CHECK(emu.movie_status().rerecords == 1);
    for (int f = 50; f < 90; f++) take.push_back(live_frame(emu, f, 7));

    // Rewind snapshots: back 20 frames, cut, carry on (take C).
    std::vector<uint8_t> snap;
    for (int f = 90; f < 110; f++) {
        if (f == 95) snap = emu.save_snapshot();
        take.push_back(live_frame(emu, f, 8));
    }
    CHECK(emu.load_snapshot(snap) && emu.movie_status().frames == 95);
    emu.add_rerecord();
    take.resize(95);
    for (int f = 95; f < 130; f++) take.push_back(live_frame(emu, f, 9));
    CHECK(emu.movie_status().rerecords == 2 && emu.movie_status().frames == 130);
    // A timeline moment saved to a slot carries the movie up to it.
    CHECK(emu.save_snapshot_to_slot(4, snap, nullptr, 0, 0).empty());
    CHECK(emu.stop_movie().empty());
    CHECK(emu.save_state(5).empty() && !fs::exists(state_movie_path(emu.state_path(5))));  // no movie, no sidecar
    const Movie movie = load_movie(path);
    CHECK(movie.frames.size() == 130 && movie.rerecords == 2);
    CHECK(load_movie(state_movie_path(emu.state_path(4))).frames.size() == 95);

    // The final take replays.
    Emulator player;
    player.set_data_dir(dir.string());
    CHECK(player.load(rom));
    CHECK(player.play_movie(movie, path).empty());
    int same = 0;
    for (int f = 0; f < 70; f++) same += played_frame(player, f) == take[static_cast<size_t>(f)];
    // Read-only: a state of this take goes back without cutting; one of another take is refused.
    player.set_movie_read_only(true);
    CHECK(!player.load_state(2).empty() && player.movie_status().frame == 70);  // take A
    CHECK(player.load_state(1).empty() && player.movie_mode() == MovieMode::Playing);
    CHECK(player.movie_status().frame == 50 && player.movie_status().frames == 130 && player.movie_status().rerecords == 2);
    for (int f = 50; f < 130; f++) same += played_frame(player, f) == take[static_cast<size_t>(f)];
    CHECK(same == 150);
    // Rewind in read-only playback: back, nothing cut.
    const std::vector<uint8_t> late = player.save_snapshot();
    CHECK(player.movie_at_end() && player.load_snapshot(snap) && player.movie_status().frame == 95 && player.movie_status().frames == 130);
    CHECK(played_frame(player, 95) == take[95]);
    CHECK(player.load_snapshot(late) && player.movie_at_end());
    // Read+write during playback: loading a state starts recording from it.
    player.set_movie_read_only(false);
    CHECK(player.load_state(1).empty() && player.movie_mode() == MovieMode::Recording && player.movie_status().frames == 50);
    CHECK(player.movie_status().rerecords == 3);
    // Snapshots from before the movie are refused during it.
    Emulator other;
    other.set_data_dir(dir.string());
    CHECK(other.load(rom));
    const std::vector<uint8_t> before = other.save_snapshot();
    CHECK(other.record_movie(MovieStart::PowerOn, (dir / "other.bfm").string(), {}).empty());
    CHECK(!other.load_snapshot(before));
    CHECK(!other.load_state(1).empty());  // another movie's state
    fs::remove_all(dir);
}

// ---- The battery save is left alone -----------------------------------------------------------

// The test ROM with 2 KB of battery-backed SRAM (the game never writes it; the memory panel does).
auto write_sram_rom(const fs::path& dir) -> std::string {
    std::vector<uint8_t> rom = test_rom::make_test_rom(12);
    rom[0x7FD6] = 0x02;  // ROM + RAM + battery
    rom[0x7FD8] = 0x01;  // 2 KB
    rom[0x7FDC] = 0xFF;
    rom[0x7FDD] = 0xFF;
    rom[0x7FDE] = rom[0x7FDF] = 0;
    uint16_t sum = 0;
    for (const uint8_t b : rom) sum = static_cast<uint16_t>(sum + b);
    rom[0x7FDC] = static_cast<uint8_t>(~sum);
    rom[0x7FDD] = static_cast<uint8_t>(~sum >> 8);
    rom[0x7FDE] = static_cast<uint8_t>(sum);
    rom[0x7FDF] = static_cast<uint8_t>(sum >> 8);
    const std::string path = (dir / "battery.sfc").string();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    return path;
}

auto test_battery() -> void {
    const fs::path dir = temp_dir("bitforge_movie_tests_battery");
    const std::string rom = write_sram_rom(dir);
    auto file_byte = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return in.get();
    };
    Emulator emu;
    emu.set_data_dir(dir.string());
    CHECK(emu.load(rom));
    CHECK(emu.memory(debug::Region::Sram).size() == 2048);
    fs::create_directories(fs::path(emu.sram_path()).parent_path());
    std::ofstream(emu.sram_path(), std::ios::binary) << std::string(2048, '\xAB');
    CHECK(emu.load(rom) && emu.memory(debug::Region::Sram)[0] == 0xAB);  // the real save
    const auto frames = static_cast<int>(Emulator::kSramFlushFrames) + 5;
    // A movie starts from a blank save and never writes the file, recording or playing.
    CHECK(emu.record_movie(MovieStart::PowerOn, (dir / "b.bfm").string(), {}).empty());
    CHECK(emu.memory(debug::Region::Sram)[0] != 0xAB);
    CHECK(emu.write_memory(debug::Region::Sram, 0, debug::ValueSize::Bits8, 0x55));
    for (int f = 0; f < frames; f++) live_frame(emu, f, 1);
    CHECK(emu.stop_movie().empty() && file_byte(emu.sram_path()) == 0xAB);
    for (int f = 0; f < frames; f++) live_frame(emu, f, 1);  // the movie's save until the game is loaded again
    CHECK(file_byte(emu.sram_path()) == 0xAB);
    CHECK(emu.play_movie(load_movie((dir / "b.bfm").string()), (dir / "b.bfm").string()).empty());
    CHECK(emu.memory(debug::Region::Sram)[0] != 0xAB);
    for (int f = 0; f < frames; f++) played_frame(emu, f);
    CHECK(file_byte(emu.sram_path()) == 0xAB);
    // Loaded again: the real save, written as usual.
    CHECK(emu.load(rom) && emu.memory(debug::Region::Sram)[0] == 0xAB);
    CHECK(emu.write_memory(debug::Region::Sram, 0, debug::ValueSize::Bits8, 0x66));
    emu.flush_sram();
    CHECK(file_byte(emu.sram_path()) == 0x66);
    fs::remove_all(dir);
}

// ---- Mismatch warnings ----------------------------------------------------------------------

auto test_mismatch() -> void {
    Movie m;
    m.rom_crc32 = 0x12345678;
    m.rom_sha1.fill(0xAB);
    m.emulator = "16bit Forge 0.1.0";
    std::array<uint8_t, 20> sha1{};
    sha1.fill(0xAB);
    CHECK(movie_mismatch(m, 0x12345678, sha1, "16bit Forge 0.1.0") == 0);
    CHECK(movie_mismatch(m, 0x12345679, sha1, "16bit Forge 0.1.0") == kMismatchRom);
    std::array<uint8_t, 20> other = sha1;
    other[19] = 0;
    CHECK(movie_mismatch(m, 0x12345678, other, "16bit Forge 0.1.0") == kMismatchRom);  // same CRC, other SHA-1
    CHECK(movie_mismatch(m, 0x12345678, sha1, "16bit Forge 0.2.0") == kMismatchEmulator);
    CHECK(movie_mismatch(m, 0, other, "16bit Forge 0.2.0") == (kMismatchRom | kMismatchEmulator));
    m.emulator.clear();  // unknown version: warned about
    CHECK(movie_mismatch(m, 0x12345678, sha1, "16bit Forge 0.1.0") == kMismatchEmulator);
    // One dialog per case, each with its own title; both in one.
    const std::string rom = mismatch_prompt(kMismatchRom).title;
    const std::string version = mismatch_prompt(kMismatchEmulator).title;
    const std::string both = mismatch_prompt(kMismatchRom | kMismatchEmulator).title;
    CHECK(rom.find("ROM") != std::string::npos && version.find("version") != std::string::npos);
    CHECK(both.find("ROM") != std::string::npos && both.find("version") != std::string::npos);
    CHECK(rom != version && rom != both && version != both);
    for (const uint8_t bits : {1, 2, 3}) CHECK(*mismatch_prompt(bits).text != '\0');
}

// ---- Replays in the run timer -----------------------------------------------------------------

auto test_replay_timer() -> void {
    const ConditionTest never = [](const debug::Condition&) { return false; };
    auto clock = [](uint64_t n) { return Clock{static_cast<Nanos>(n) * kNanosPerSecond / 60, 1'700'000'000}; };
    auto frame = [&](uint64_t n, bool race, bool replay) { return FrameInfo{n, Jump::None, 0, clock(n), race, replay}; };
    for (const bool race : {false, true}) {
        RunTimer t;
        t.set_run(make_run("G", "Any%", 1));
        t.set_race(race);
        uint64_t n = 0;
        t.on_frame(frame(++n, race, true), never);  // playing when it starts
        t.start(clock(n));
        CHECK(t.race_status() == RaceStatus::Replay);
        for (int i = 0; i < 30; i++) t.on_frame(frame(++n, race, false), never);
        t.split(clock(n));
        CHECK(t.race_status() == RaceStatus::Replay);
        // A replayed run (here a first finish, so a PB and golds) stays out of the run: no attempt,
        // no history, nothing to save.
        CHECK(!t.finished_pb() && !t.has_improvements());
        t.reset(clock(n + 1));
        CHECK(t.race_status() == RaceStatus::Off && !t.pending());
        CHECK(t.run().attempt_count == 0 && t.run().attempts.empty() && t.run().segments[0].history.empty());
        CHECK(t.run().segments[0].best.empty() && t.run().segments[0].pb.empty());
        // A movie playing only partway through the run: a replay too, and it uncounts the attempt.
        t.start(clock(++n));
        t.on_frame(frame(++n, race, false), never);
        CHECK(t.race_status() == (race ? RaceStatus::Clean : RaceStatus::Off) && t.run().attempt_count == 1);
        t.on_frame(frame(++n, race, true), never);
        t.on_frame(frame(++n, race, false), never);
        CHECK(t.run().attempt_count == 0);
        t.split(clock(n));
        CHECK(t.race_status() == RaceStatus::Replay && !info(RaceStatus::Replay).tag.empty());
        t.reset(clock(n + 1));
        CHECK(t.run().attempt_count == 0 && t.run().attempts.empty() && !t.pending());
        // A player's run after it counts and is kept as usual.
        t.start(clock(++n));
        for (int i = 0; i < 30; i++) t.on_frame(frame(++n, race, false), never);
        t.split(clock(n));
        CHECK(t.finished_pb());
        t.reset(clock(n + 1));
        CHECK(t.run().attempt_count == 1 && t.run().attempts.size() == 1 && t.pending());
        CHECK(t.run().attempts[0].race_verified == race);
    }
    // The verification summary names the movie.
    Verification v;
    v.movie = Verification::MovieRef{"movies/G/attempt-3.bfm", "00ff", 1234, 0};
    const std::string json = format_verification(v);
    CHECK(json.find("\"movie\": {\"file\": \"movies/G/attempt-3.bfm\", \"sha1\": \"00ff\", \"start\": \"power_on\", \"frames\": 1234") !=
          std::string::npos);
    CHECK(json.substr(json.size() - 3) == "}\n}" || json.substr(json.size() - 4) == "}\n}\n");
    v.movie.reset();
    CHECK(format_verification(v).find("movie") == std::string::npos);
}

}  // namespace

int main() {
    utils::Logger::set_level(utils::LogLevel::Error);
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"Format: round trip, runs, devices, damaged files", test_format},
        {"Replay from power-on matches frame for frame", test_power_on_replay},
        {"Replay from a save state, race mode refuses it", test_state_start_replay},
        {"Rerecords: states, rewind, read-only, takes", test_rerecords},
        {"Battery save: blank in a movie, the file untouched", test_battery},
        {"ROM / version mismatch: warnings, one dialog", test_mismatch},
        {"Run timer: replays never verify or count; summary names it", test_replay_timer},
    };
    int failed = 0;
    for (const Test& t : tests) {
        const int before = checks_failed;
        std::printf("  %-58s", t.name);
        t.fn();
        const bool ok = checks_failed == before;
        failed += ok ? 0 : 1;
        std::printf("%s\n", ok ? " PASS" : "\n  FAIL");
    }
    const int count = static_cast<int>(std::size(tests));
    std::printf("=== Results: %d passed, %d failed ===\n", count - failed, failed);
    return failed == 0 ? 0 : 1;
}
