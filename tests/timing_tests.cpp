// SPDX-License-Identifier: MIT
// Timing: the core against documented NTSC hardware timing (debug/timing_check.hpp): line and
// frame lengths with the short line, interlace, the frame rate, NMI, IRQ, auto-joypad, refresh,
// DMA and HDMA; the PPU's look-ahead (clocks_until_line) against stepping through the short
// line; game time against the exact frame period, whatever the wall clock does; the lag frame
// counter in the core, the emulator (rewind, reset, movie replay) and the run timer; and the
// timing report. Headless, on built-in programs.
//
// Optional, against a real game: timing_tests --rom <file.sfc> [--frames <n>] runs the timing
// report's scripted game section and prints it (the report must come out clean).
#include "debug/timing_check.hpp"
#include "frontend/emulator.hpp"
#include "frontend/timing_report.hpp"
#include "snes/memory/bus.hpp"
#include "snes/ppu/ppu.hpp"
#include "snes/system.hpp"
#include "snes/timing.hpp"
#include "speedrun/movie.hpp"
#include "speedrun/run_timer.hpp"
#include "speedrun/splits.hpp"
#include "utils/logging.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace bitforge;

namespace {

namespace fs = std::filesystem;
int checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

// ---- The documented values ---------------------------------------------------------------------

auto find(const std::vector<debug::TimingCheck>& checks, std::string_view key) -> const debug::TimingCheck* {
    for (const auto& c : checks) {
        if (c.key == key) return &c;
    }
    return nullptr;
}

auto test_documented() -> void {
    const std::vector<debug::TimingCheck> checks = debug::run_timing_checks();
    CHECK(debug::checks_off(checks) == 0);
    // These the core models exactly.
    for (const char* key : {"line_clocks", "short_line_clocks", "frame_clocks_0", "frame_clocks_1", "interlace_pair_clocks", "frame_rate",
                            "frame_rate_cpu", "game_time_hour_ns", "vblank_line", "autojoy_start", "autojoy_clocks", "refresh_clocks",
                            "dma_byte_clocks", "dma_channel_clocks", "hdma_init_direct_clocks", "hdma_init_indirect_clocks",
                            "hdma_line_clocks", "hdma_line_idle_clocks", "master_clock_hz", "cpu_clocks_per_line", "access_wram_clocks",
                            "access_slowrom_clocks", "access_fastrom_clocks", "access_io_clocks", "access_joyser_clocks"}) {
        const debug::TimingCheck* c = find(checks, key);
        CHECK(c != nullptr);
        if (c && c->verdict() != debug::Verdict::Exact) {
            std::printf("\n    %s: measured %.7f, documented %.7f", key, c->measured, c->low);
            checks_failed++;
        }
    }
    // The known gaps say why.
    for (const auto& c : checks) {
        if (c.verdict() == debug::Verdict::Close) CHECK(!c.note.empty());
        if (c.verdict() == debug::Verdict::Off) std::printf("\n    OFF: %.*s", static_cast<int>(c.key.size()), c.key.data());
    }
    const std::string table = debug::format_timing_checks(checks);
    CHECK(table.find("SNES") == std::string::npos);
    CHECK(table.find("https://") != std::string::npos);
    // The run timer and the pacer use the same rate.
    CHECK(speedrun::info(speedrun::VideoStandard::Ntsc).frame_rate == timing::kNtscFrameRate);
    CHECK(frontend::Emulator::kNtscFrameRate == timing::kNtscFrameRate);
}

// ---- The PPU through the short line ------------------------------------------------------------

struct Rig {
    memory::Bus bus;
    ppu::PPU ppu;
    Rig() {
        bus.connect_ppu(&ppu);
        ppu.connect_bus(&bus);
        bus.load_test_rom(std::vector<Byte>(0x8000, 0xEA));
        bus.reset();
        ppu.reset();
    }
    auto frame() -> int64_t {
        int64_t n = 1;
        while (!ppu.step(1)) n++;
        return n;
    }
};

auto test_frames() -> void {
    {  // Interlace off: 262 lines, line 240 short on the frames with the field flag ($213F bit 7) set.
        Rig r;
        for (int i = 0; i < 6; i++) {
            const bool field = (r.bus.read(0x213F) & 0x80) != 0;
            CHECK(r.frame() == (field ? 357364 : 357368));
        }
    }
    {  // Overscan (239 lines) moves V-blank, not the short line.
        Rig r;
        r.bus.write(0x2133, 0x04);
        CHECK(r.frame() + r.frame() == timing::kFramePairClocks);
    }
    {  // Interlace: 263 lines on field 0, no short line.
        Rig r;
        r.bus.write(0x2133, 0x01);
        CHECK(r.frame() == 263 * 1364);
        CHECK(r.frame() == 262 * 1364);
    }
    // 3600 frames: an exact number of master clocks, exactly 3600 / 60.0988139 s.
    Rig r;
    int64_t clocks = 0;
    for (int i = 0; i < 3600; i++) clocks += r.frame();
    CHECK(clocks == 1800 * timing::kFramePairClocks);
}

// PPU::clocks_until_line() (the $4210 look-ahead) against the beam, through the short line and
// the frame's end, from every 61st clock of three frames.
auto test_look_ahead() -> void {
    for (const Byte setini : {Byte{0x00}, Byte{0x01}}) {
        for (const int target : {0, 225, 240, 241}) {
            Rig r;
            r.bus.write(0x2133, setini);
            std::set<int64_t> arrivals;
            std::vector<int64_t> predicted;
            for (int64_t t = 0; t < 4 * timing::kFrameClocks; t++) {
                if (r.ppu.hclock() == 0 && r.ppu.vcounter() == target) arrivals.insert(t);
                if (t % 61 == 0 && t >= 2 * timing::kLineClocks && t < 3 * timing::kFrameClocks) {  // once SETINI is latched
                    predicted.push_back(t + static_cast<int64_t>(r.ppu.clocks_until_line(target)));
                }
                r.ppu.step(1);
            }
            int misses = 0;
            for (int64_t p : predicted) misses += arrivals.count(p) ? 0 : 1;
            if (misses) std::printf("\n    setini %02X, line %d: %d of %zu predictions missed", setini, target, misses, predicted.size());
            CHECK(misses == 0);
        }
    }
}

// ---- Game time ---------------------------------------------------------------------------------

auto test_game_time() -> void {
    // frames_to_nanos against the exact period, 357366 * 11 / 236.25e6 s = 357366 * 8800 / 189 ns
    // (= 119122 * 8800 / 63 ns), out to 100 hours.
    for (const int64_t frames : {int64_t{1}, int64_t{60}, int64_t{3600}, int64_t{216'000}, int64_t{2'160'000}, int64_t{21'600'000}}) {
        const int64_t exact = (frames * 119'122 * 8'800 + 31) / 63;  // rounded
        const int64_t got = speedrun::frames_to_nanos(frames, speedrun::VideoStandard::Ntsc);
        CHECK(got >= exact - 1 && got <= exact + 1);
    }
    // An hour of frames while the PC's clock runs fast, slow and stops: game time doesn't move
    // with it, real time does.
    speedrun::RunTimer t;
    speedrun::Run run = speedrun::make_run("Game", "Any%", 1);
    t.set_run(run);
    speedrun::Nanos mono = 0;
    auto clock = [&] { return speedrun::Clock{mono, 1'700'000'000 + mono / speedrun::kNanosPerSecond}; };
    t.start(clock());
    constexpr uint64_t kHour = 216'000;
    for (uint64_t f = 1; f <= kHour; f++) {
        mono += (f % 3 == 0) ? 0 : (f % 3 == 1 ? 33'000'000 : 1'000'000);  // stalls, slowdown, fast-forward
        t.on_frame({f, speedrun::Jump::None, 0, clock(), false, false, false, 0}, {});
    }
    CHECK(t.time(speedrun::Method::Game, mono) == speedrun::frames_to_nanos(kHour, speedrun::VideoStandard::Ntsc));
    CHECK(t.time(speedrun::Method::Real, mono) == mono);
    const speedrun::Nanos hour = t.time(speedrun::Method::Game, mono);
    CHECK(hour > 3'594'000'000'000 && hour < 3'595'000'000'000);  // 216000 / 60.0988 s = 3594.08 s
}

// ---- Lag frames ----------------------------------------------------------------------------------

auto system_lag(uint8_t mask, debug::ProbeRead read, int frames) -> int {
    System sys;
    sys.cartridge().load_raw(debug::make_probe_rom(mask, read));
    sys.reset();
    int lag = 0;
    for (int i = 0; i < frames; i++) {
        sys.run_frame();
        lag += sys.lagged() ? 1 : 0;
    }
    return lag;
}

auto test_lag_core() -> void {
    CHECK(system_lag(0, debug::ProbeRead::AutoRead, 600) == 0);
    CHECK(system_lag(1, debug::ProbeRead::AutoRead, 600) == 300);
    CHECK(system_lag(3, debug::ProbeRead::AutoRead, 600) == 450);
    CHECK(system_lag(0, debug::ProbeRead::Serial, 600) == 0);
    CHECK(system_lag(0, debug::ProbeRead::None, 600) == 600);
}

auto write_probe(const fs::path& path, uint8_t mask) -> std::string {
    const auto rom = debug::make_probe_rom(mask);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    return path.string();
}

auto test_lag_emulator() -> void {
    const fs::path dir = fs::temp_directory_path() / "bitforge_timing_lag";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string rom = write_probe(dir / "probe.sfc", 1);  // lags every other frame
    {
        frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom));
        CHECK(emu.lag_frames() == 0 && !emu.lagged());
        for (int i = 0; i < 60; i++) emu.run_frame();
        CHECK(emu.lag_frames() == 30);
        const std::vector<uint8_t> snap = emu.save_snapshot();
        for (int i = 0; i < 60; i++) emu.run_frame();
        CHECK(emu.lag_frames() == 60);
        CHECK(emu.load_snapshot(snap) && emu.lag_frames() == 30);  // rewind takes it back
        for (int i = 0; i < 60; i++) emu.run_frame();
        CHECK(emu.lag_frames() == 60 && emu.frame_count() == 120);
        // A save state load leaves the counter alone; a reset starts it over.
        CHECK(emu.save_state(1).empty());
        emu.run_frame();
        const uint64_t before = emu.lag_frames();
        CHECK(emu.load_state(1).empty() && emu.lag_frames() == before);
        emu.reset();
        CHECK(emu.lag_frames() == 0);
        // Master clocks: 120 frames from reset are 60 frame pairs, give or take an instruction.
        for (int i = 0; i < 120; i++) emu.run_frame();
        const auto clocks = static_cast<int64_t>(emu.master_clocks());
        CHECK(clocks > 60 * timing::kFramePairClocks - 200 && clocks < 60 * timing::kFramePairClocks + 200);
    }
    // A movie replays the same lag frames.
    const std::string movie_path = (dir / "lag.bfm").string();
    uint64_t recorded = 0;
    {
        frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom));
        CHECK(emu.record_movie(speedrun::MovieStart::PowerOn, movie_path, {"16bit Forge test", ""}).empty());
        for (int i = 0; i < 90; i++) {
            emu.set_joypad(0, static_cast<uint16_t>(i * 37));
            emu.run_frame();
        }
        recorded = emu.lag_frames();
        CHECK(emu.stop_movie().empty());
    }
    {
        frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom));
        speedrun::Movie movie;
        CHECK(speedrun::read_movie(movie_path, movie).empty());
        CHECK(emu.play_movie(std::move(movie), movie_path).empty());
        for (int i = 0; i < 90; i++) emu.run_frame();
        CHECK(recorded == 45 && emu.lag_frames() == recorded);
        emu.stop_movie();  // closed before the folder goes (Windows keeps open files)
    }
    fs::remove_all(dir);
}

// The run timer's lag count: per attempt, back with a rewind (not past a split), held at the finish.
auto test_lag_timer() -> void {
    speedrun::RunTimer t;
    t.set_run(speedrun::make_run("Game", "Any%", 2));
    std::vector<bool> history(1, false);  // the game's lag flag by frame number
    auto step = [&](bool lagged, speedrun::Jump jump = speedrun::Jump::None) {
        history.push_back(lagged);
        uint64_t lag = 0;
        for (bool l : history) lag += l ? 1 : 0;
        t.on_frame({history.size() - 1, jump, 0, {}, false, false, lagged, lag}, {});
    };
    auto rewind_to = [&](size_t frame, bool lagged) {
        history.resize(frame + 1);
        step(lagged, speedrun::Jump::Rewind);
    };
    for (int i = 0; i < 10; i++) step(true);  // before the start: not counted
    t.start({});
    for (int i = 0; i < 100; i++) step(i % 4 == 0);  // frames 11-110
    CHECK(t.lag_frames() == 25 && t.game_frames() == 100);
    rewind_to(70, true);  // frames 71-110 had 10 lag frames; frame 71 again, lagging
    CHECK(t.lag_frames() == 16 && t.game_frames() == 61);
    t.split({});
    for (int i = 0; i < 20; i++) step(true);  // frames 72-91
    CHECK(t.lag_frames() == 36);
    rewind_to(81, false);  // back past 10 lag frames, not past the split
    CHECK(t.lag_frames() == 26);
    rewind_to(60, false);  // back past the split: it is undone, the count goes with the game
    CHECK(t.current() == 0 && t.lag_frames() == 13);
    t.split({});
    t.split({});  // the end
    CHECK(t.state() == speedrun::TimerState::Ended && t.lag_frames() == 13);
    for (int i = 0; i < 5; i++) step(true);
    CHECK(t.lag_frames() == 13);  // held at the finish
    t.reset({});
    t.start({});
    CHECK(t.lag_frames() == 0);
}

// ---- The report --------------------------------------------------------------------------------

auto test_report() -> void {
    const fs::path dir = fs::temp_directory_path() / "bitforge_timing_report";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const frontend::TimingReport checks_only = frontend::make_timing_report({"16bit Forge test", "", 0, dir.string()});
    CHECK(checks_only.ok);
    CHECK(checks_only.text.find("Result:") != std::string::npos && checks_only.text.find("SNES") == std::string::npos);
    const frontend::TimingReport game = frontend::make_timing_report({"16bit Forge test", write_probe(dir / "probe.sfc", 1), 240, dir.string()});
    CHECK(game.ok);
    CHECK(game.text.find("Lag frames:        120") != std::string::npos);
    CHECK(game.text.find("DIFFERENT") == std::string::npos);
    if (!game.ok) std::printf("\n%s", game.text.c_str());
    fs::remove_all(dir);
}

}  // namespace

int main(int argc, char** argv) {
    utils::Logger::set_level(utils::LogLevel::Error);
    std::string rom;
    uint64_t frames = 3600;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--rom") == 0) rom = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0) frames = std::strtoull(argv[++i], nullptr, 10);
    }
    if (!rom.empty()) {
        const frontend::TimingReport r = frontend::make_timing_report({"16bit Forge test", rom, frames, {}});
        std::printf("%s", r.text.c_str());
        return r.ok ? 0 : 1;
    }

    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"Documented NTSC timing: exact or a known gap", test_documented},
        {"Frames: the short line, overscan, interlace, 3600 frames", test_frames},
        {"Look-ahead to a line through the short line", test_look_ahead},
        {"Game time: exact period, independent of the PC clock", test_game_time},
        {"Lag frames in the core: $4218, $4016, none", test_lag_core},
        {"Lag frames: rewind, states, reset, movie replay", test_lag_emulator},
        {"Lag frames in the run timer: attempts, rewinds, finish", test_lag_timer},
        {"Timing report: checks, a scripted run replayed", test_report},
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
