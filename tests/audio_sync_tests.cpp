// SPDX-License-Identifier: MIT
// Audio output tests: the real AudioOutput (FIFO, get-callback feed, PI rate control, fades)
// and a real SDL_AudioStream resampling 32040 Hz to 48 kHz, driven in virtual time by a
// simulated sound card that pulls fixed-size chunks on its own clock, against a video loop at
// 60.0988 Hz with jitter. No audio device is opened, so this runs headless. Like SDL's paused
// device, the simulated card pulls nothing while the output is paused or holding the device.
#include "frontend/audio.hpp"
#include "frontend/wav_writer.hpp"
#include "utils/logging.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <random>
#include <vector>

using bitforge::frontend::AudioOutput;

namespace {

constexpr int kSource = 32040;
constexpr int kDevice = 48000;
constexpr double kPi = 3.14159265358979323846;

// What SDL does with the real device: it calls the get-callback only after AudioOutput has
// resumed it (not paused, no longer priming).
auto device_running(const AudioOutput& out) -> bool {
    const auto st = out.stats();
    return !st.paused && !st.priming;
}

struct Scenario {
    const char* name;
    double device_error = 0.0;  // sound card clock: +0.01 = drains 1% faster than nominal
    double host_slow = 0.0;     // video loop: +0.01 = 1% fewer frames per second (frames run late)
    int chunk = 480;            // device period in frames at 48 kHz
    double seconds = 120.0;
    double stall_at = -1.0;     // one main-loop stall of stall_ms at this time
    double stall_ms = 0.0;
    double jump_at = -1.0;      // the core's audio jumps (a save state loaded) at this time
    bool flush = true;          // ... and the frontend calls AudioOutput::flush() for it
    double boot_hitch_ms = 0.0; // each of the first 3 frames after a (re)start runs this late
    double pause_at = -1.0;     // pause at this time and resume 1 s later (hitching again)
};

struct Result {
    uint64_t underruns = 0, overflows = 0;
    double silence_ms = 0.0;
    double min_q = 1e9, max_q = 0.0, ratio = 1.0, max_jump = 0.0;
};

// A 440 Hz sine at 1/3 full scale stands in for the core; every output sample is checked for
// jumps a sine of that level can't make (a hard cut to silence would jump ~10000).
auto run(const Scenario& s) -> Result {
    AudioOutput out;
    Result r;
    if (!out.open_unbound(kSource, kDevice)) return r;
    out.set_paused(false);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(-0.003, 0.003);
    const double frame = 1.0 / 60.0988 * (1.0 + s.host_slow);
    const double chunk_time = s.chunk / (kDevice * (1.0 + s.device_error));
    std::vector<int16_t> pcm, dev(static_cast<size_t>(s.chunk) * 2);
    double t_frame = 0.0, t_dev = 0.0, acc = 0.0, phase = 0.0, prev = 0.0, offset = 0.0;
    bool stalled = false, have_prev = false, jumped = false, paused = false, resumed = false;
    int hitch_left = s.boot_hitch_ms > 0 ? 3 : 0;
    const int frames = static_cast<int>(s.seconds / frame);
    for (int f = 0; f < frames; f++) {
        if (!paused && s.pause_at >= 0 && t_frame >= s.pause_at) {
            paused = true;
            out.set_paused(true);
            t_frame += 1.0;  // the device keeps its clock but pulls nothing from a paused output
            t_dev = t_frame;
            out.set_paused(false);
            resumed = true;
            hitch_left = s.boot_hitch_ms > 0 ? 3 : 0;
            have_prev = false;
        }
        if (hitch_left > 0 && f > 0) {
            hitch_left--;
            t_frame += s.boot_hitch_ms / 1000.0;
        }
        if (!stalled && s.stall_at >= 0 && t_frame >= s.stall_at) {
            stalled = true;
            t_frame += s.stall_ms / 1000.0;
        }
        const double when = t_frame + jitter(rng);
        if (!device_running(out)) t_dev = std::max(t_dev, when - chunk_time);  // keeps its clock, pulls nothing
        while (t_dev + chunk_time <= when) {  // the device pulls between our frames
            t_dev += chunk_time;
            const int got = SDL_GetAudioStreamData(out.stream(), dev.data(), s.chunk * 4);
            for (int i = 0; i < got / 4; i++) {
                const double v = dev[static_cast<size_t>(i) * 2];
                if (have_prev) r.max_jump = std::max(r.max_jump, std::abs(v - prev));
                prev = v;
                have_prev = true;
            }
        }
        if (!jumped && s.jump_at >= 0 && t_frame >= s.jump_at) {
            jumped = true;
            phase += kPi * 0.75;  // the new audio starts somewhere else entirely, far from the old level
            offset = 12000.0;
            if (s.flush) out.flush();
        }
        acc += kSource / 60.0988;  // what the core makes per emulated frame (533/534)
        const int n = static_cast<int>(acc);
        acc -= n;
        pcm.resize(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n; i++) {
            const auto v = static_cast<int16_t>(10000.0 * std::sin(phase) + offset);
            phase += 2.0 * kPi * 440.0 / kSource;
            pcm[static_cast<size_t>(i) * 2] = pcm[static_cast<size_t>(i) * 2 + 1] = v;
        }
        const double q = out.queued_ms();
        out.push(pcm);
        if (t_frame > 30.0) {  // after the controller has settled
            r.min_q = std::min(r.min_q, q);
            r.max_q = std::max(r.max_q, q);
        }
        t_frame += frame;
    }
    const auto st = out.stats();
    r.underruns = st.underruns;
    r.overflows = st.overflows;
    r.silence_ms = st.silence_ms;
    (void)resumed;
    r.ratio = st.ratio;
    return r;
}

int failures = 0;
auto check(bool ok, const char* what) -> void {
    if (!ok) {
        std::printf("      FAIL: %s\n", what);
        failures++;
    }
}

// The app's whole audio lifecycle in virtual time, in App::run's order: launch with no ROM, boot
// (ROM load and game window creation stall the loop), play, stop (battery save and GPU teardown
// stall it), switch games. The sound card runs on its own clock throughout.
// The sound card can be modeled on WASAPI (shared, event-driven): 10 ms pulls at 32040 Hz
// (321 frames), each event up to jitter_ms late, and every burst_every_s the audio thread wakes
// burst_ms late and makes the missed pulls back to back (what the endpoint buffer allows).
struct DeviceModel {
    int rate = kDevice;
    int chunk = 480;
    double jitter_ms = 0.0;
    double burst_every_s = 0.0;
    double burst_ms = 0.0;
};
constexpr DeviceModel kWasapi{kSource, 321, 2.0, 0.5, 25.0};

class AppSim {
public:
    explicit AppSim(int chunk = 480) : AppSim(DeviceModel{kDevice, chunk}) {}
    explicit AppSim(const DeviceModel& m) : model_(m), chunk_(m.chunk), dev_(static_cast<size_t>(m.chunk) * 2), rng_(11) {
        ok = out.open_unbound(kSource, m.rate);
        next_jitter_ = draw_jitter();
    }
    // One main-loop iteration: the App sets the pause state, steps (push), presents.
    auto frame(bool emulating) -> void {
        out.set_paused(!emulating);
        if (emulating) emulate();
        advance(1.0 / 60.0988);
    }
    // One emulated frame's audio pushed, no time passing.
    auto emulate() -> void {
        {
            acc_ += kSource / 60.0988;
            const int n = static_cast<int>(acc_);
            acc_ -= n;
            pcm_.resize(static_cast<size_t>(n) * 2);
            for (int i = 0; i < n; i++) {
                const auto v = static_cast<int16_t>(10000.0 * std::sin(phase_));
                phase_ += 2.0 * kPi * 440.0 / kSource;
                pcm_[static_cast<size_t>(i) * 2] = pcm_[static_cast<size_t>(i) * 2 + 1] = v;
            }
            out.push(pcm_);
        }
    }
    auto now_ns() const -> uint64_t { return static_cast<uint64_t>(t_ * 1e9); }
    auto advance_to(uint64_t ns) -> void {
        const double target = static_cast<double>(ns) / 1e9;
        if (target > t_) advance(target - t_);
    }
    auto frames(int n, bool emulating) -> void { for (int i = 0; i < n; i++) frame(emulating); }
    // The main loop is blocked (no pushes); the device keeps pulling if it's running.
    auto advance(double seconds) -> void {
        t_ += seconds;
        const double chunk_time = static_cast<double>(chunk_) / model_.rate;
        for (;;) {
            const double nominal = static_cast<double>(pull_index_ + 1) * chunk_time;
            double late = next_jitter_;
            if (model_.burst_every_s > 0.0) {
                const double phase = std::fmod(nominal, model_.burst_every_s);
                if (phase < model_.burst_ms / 1000.0) late = std::max(late, model_.burst_ms / 1000.0 - phase);
            }
            const double actual = std::max(nominal + late, last_actual_);
            if (actual > t_) break;
            pull_index_++;
            last_actual_ = actual;
            next_jitter_ = draw_jitter();
            if (!device_running(out)) continue;
            pulls++;
            if (!emulating_now()) idle_pulls++;
            const int got = SDL_GetAudioStreamData(out.stream(), dev_.data(), chunk_ * 4);
            for (int i = 0; i < got / 4; i++) {
                const double v = dev_[static_cast<size_t>(i) * 2];
                if (have_prev_) max_jump = std::max(max_jump, std::abs(v - prev_));
                prev_ = v;
                have_prev_ = true;
            }
        }
    }
    // App::boot: pause before the load, load + window creation stall, then a new session.
    auto boot(double load_stall_s) -> void {
        out.end_session();
        loaded_ = false;
        advance(load_stall_s);
        loaded_ = true;
        out.begin_session();
    }
    // App::request_stop (game window closed, Stop menu): quiesce at once; stop() runs later.
    auto request_stop() -> void { out.end_session(); }
    // App::stop: pause before the teardown, then the teardown stall.
    auto stop(double teardown_s) -> void {
        out.end_session();
        loaded_ = false;
        advance(teardown_s);
    }
    auto emulating_now() const -> bool { return loaded_; }

    AudioOutput out;
    bool ok = false;
    uint64_t pulls = 0, idle_pulls = 0;
    double max_jump = 0.0;

private:
    auto draw_jitter() -> double {
        return model_.jitter_ms > 0.0 ? std::uniform_real_distribution<double>(0.0, model_.jitter_ms / 1000.0)(rng_) : 0.0;
    }

    DeviceModel model_;
    int chunk_;
    std::vector<int16_t> dev_, pcm_;
    std::mt19937 rng_;
    uint64_t pull_index_ = 0;
    double t_ = 0.0, last_actual_ = 0.0, next_jitter_ = 0.0, acc_ = 0.0, phase_ = 0.0, prev_ = 0.0;
    bool have_prev_ = false, loaded_ = false;
};

auto report(const char* name, int before, int& passed, const AudioOutput::Stats& st) -> void {
    std::printf("  %-40s underruns %4llu  silence %5.0f ms ... %s\n", name, static_cast<unsigned long long>(st.underruns),
                st.silence_ms, failures == before ? "PASS" : "FAIL");
    if (failures == before) passed++;
}

// Startup/stop lifecycle cases. Returns how many were run.
auto run_lifecycle(int& passed) -> int {
    int run = 0;
    char what[160];
    // 1. Launch, sit in the menu with no ROM, boot, and stall 800 ms (D3D12 device, first
    //    swapchain, pipeline compiles, the window being shown) at various points in the first
    //    frames: before the first frame, right as the queue first fills, and later in the start-up.
    //    40 and 90 are past the old 500 ms grace: the old code logged 1 underrun and ~800 ms of
    //    silence there, as on the Windows/WASAPI build.
    for (const int stall_after : {0, 1, 4, 8, 20, 40, 90}) {
        AppSim app;
        const int before = failures;
        app.frames(180, false);                 // 3 s in the menu, no ROM
        check(app.pulls == 0, "device stays paused with no ROM");
        app.boot(0.3);
        app.advance(0.8);                       // between boot and the first frame
        app.frames(stall_after, true);
        app.advance(0.8);                       // and again inside the first frames
        app.frames(60 * 20, true);              // 20 s of play
        const auto st = app.out.stats();
        std::snprintf(what, sizeof(what), "800 ms stalls after %d frames: no underrun", stall_after);
        check(st.underruns == 0, what);
        check(st.silence_ms == 0.0, "start-up stalls report no silence");
        check(st.device_running, "device running after start-up");
        check(app.max_jump <= 1500, "no hard cuts");
        char name[64];
        std::snprintf(name, sizeof(name), "launch, boot, 800 ms stall @ frame %d", stall_after);
        report(name, before, passed, st);
        run++;
    }
    // 2. Stop: the teardown stalls 200 ms, then the app idles with no ROM. Nothing plays or counts.
    {
        AppSim app;
        const int before = failures;
        app.boot(0.3);
        app.frames(60 * 10, true);
        const uint64_t pulls_before = app.pulls;
        app.stop(0.2);                          // battery save + GPU device teardown
        app.frames(120, false);                 // 2 s stopped
        const auto st = app.out.stats();
        check(st.underruns == 0, "stop: no underrun from the teardown");
        check(st.silence_ms == 0.0, "stop: no silence counted");
        check(app.pulls == pulls_before && app.idle_pulls == 0, "stop: device pulls nothing once stopped");
        check(!st.device_running, "stop: device paused");
        report("stop: 200 ms teardown, then 2 s idle", before, passed, st);
        run++;
    }
    // 3. Switch games while one is running: an 800 ms load, then an 800 ms first-frame stall.
    {
        AppSim app;
        const int before = failures;
        app.boot(0.3);
        app.frames(60 * 5, true);
        app.boot(0.8);
        app.advance(0.8);
        app.frames(5, true);
        app.advance(0.8);
        app.frames(60 * 10, true);
        const auto st = app.out.stats();
        check(st.underruns == 0 && st.silence_ms == 0.0, "switch games: load and first-frame stalls not counted");
        report("switch games: 800 ms load + stall", before, passed, st);
        run++;
    }
    // 4. Stop, sit in the menu, boot again: a clean second session.
    {
        AppSim app;
        const int before = failures;
        app.boot(0.3);
        app.frames(60 * 5, true);
        app.stop(0.2);
        app.frames(180, false);
        app.boot(0.3);
        app.advance(0.8);
        app.frames(60 * 5, true);
        const auto st = app.out.stats();
        check(st.underruns == 0 && st.silence_ms == 0.0, "second boot after stop is clean");
        check(app.idle_pulls == 0, "no pulls while stopped");
        report("stop, idle, boot again", before, passed, st);
        run++;
    }
    // 5. A stall right before the stop: on Windows the loop blocks while the game window's close
    //    button is held, the device drains the queue, and only then does the close event come in.
    //    The log showed 1 underrun after "Emulation stopped" with a 652 ms main-loop gap.
    for (const double held : {0.15, 0.65}) {
        AppSim app;
        const int before = failures;
        app.boot(0.3);
        app.frames(60 * 10, true);
        app.advance(held);                      // close button held: no frames, device running
        app.request_stop();                     // the close event arrives
        app.stop(0.2);                          // same iteration: battery save + GPU teardown
        app.frames(120, false);
        const auto st = app.out.stats();
        check(st.underruns == 0, "stall that ends in a stop: no underrun");
        check(st.silence_ms == 0.0, "stall that ends in a stop: no silence");
        char name[64];
        std::snprintf(name, sizeof(name), "%.0f ms stall, then close/stop", held * 1000.0);
        report(name, before, passed, st);
        run++;
    }
    // 6. Stop requested from the menu, then the loop stalls (present, file dialogs) before stop().
    {
        AppSim app;
        const int before = failures;
        app.boot(0.3);
        app.frames(60 * 10, true);
        app.request_stop();
        app.advance(0.4);
        app.stop(0.2);
        app.frames(120, false);
        const auto st = app.out.stats();
        check(st.underruns == 0 && st.silence_ms == 0.0, "stall between the stop request and stop(): nothing counted");
        report("stop requested, 400 ms stall, stop()", before, passed, st);
        run++;
    }
    // 7. Control: the same 650 ms stall without a stop is a real underrun once play goes on.
    {
        AppSim app;
        const int before = failures;
        app.boot(0.3);
        app.frames(60 * 10, true);
        app.advance(0.65);
        app.frames(60 * 2, true);
        const auto st = app.out.stats();
        check(st.underruns == 1, "a stall followed by more play counts once");
        check(st.silence_ms > 400.0, "and reports its silence");
        report("650 ms stall, play goes on (control)", before, passed, st);
        run++;
    }
    // 8. Super Mario World on WASAPI (the log behind underrun4): boot with the battery save load,
    //    the first-frame stall, then a minute of play with event jitter and a 25 ms late wake-up
    //    of the audio thread every 0.5 s. None of that drains a 64 ms queue: no underrun.
    {
        AppSim app(kWasapi);
        const int before = failures;
        app.frames(60, false);
        app.boot(0.3);                          // ROM + battery save load, game window
        app.advance(0.8);
        app.frames(60 * 60, true);
        const auto st = app.out.stats();
        check(st.underruns == 0 && st.silence_ms == 0.0, "WASAPI pulls (10 ms, jitter, late bursts) alone never underrun");
        check(app.max_jump <= 1500, "no hard cuts");
        report("SMW on WASAPI: 60 s, jitter + bursts", before, passed, st);
        run++;
    }
    // 9. The same, with one ~100 ms main-loop stall in the middle of an iteration (event pump),
    //    before that frame's push. Real: 1 underrun with about 33 ms of silence, as in the log.
    //    The stall has to show in the stats of that same report: the device played ~100 ms past
    //    the last queued frame. (The old warning measured the loop gap at the iteration start,
    //    before the stall, and printed 17 ms.)
    {
        AppSim app(kWasapi);
        const int before = failures;
        app.boot(0.3);
        app.advance(0.8);
        app.frames(60 * 30, true);
        app.advance(0.1);                       // event pump blocked, then this frame's push
        app.frame(true);
        const auto at_report = app.out.stats();  // what watch_audio() logs right after that push
        app.frames(60 * 10, true);
        const auto st = app.out.stats();
        check(at_report.underruns == 1, "a 100 ms stall is a real underrun, reported on the next frame");
        check(at_report.silence_ms >= 15.0 && at_report.silence_ms <= 50.0, "about 33 ms of silence, as on the WASAPI build");
        check(at_report.starved_ms >= 90.0, "the report shows the device ran ~100 ms past the last frame");
        check(st.underruns == 1 && st.starved_ms >= 90.0, "the refill after it doesn't hide the stall");
        std::printf("      (silence %.0f ms, device ran %.0f ms past the last frame)\n", at_report.silence_ms, at_report.starved_ms);
        report("SMW on WASAPI: 100 ms stall before a push", before, passed, st);
        run++;
    }
    // 12. Control: once the grace frames have passed, a real 150 ms stall still counts.
    {
        AppSim app;
        const int before = failures;
        app.boot(0.3);
        app.advance(0.8);
        app.frames(60 * 10, true);
        app.advance(0.15);
        app.frames(60 * 5, true);
        const auto st = app.out.stats();
        check(st.underruns == 1, "a real stall after the grace frames counts once");
        check(st.silence_ms > 0.0, "and reports its silence");
        check(st.starved_ms >= 140.0, "and the 150 ms without frames behind it");
        report("real stall after start-up (control)", before, passed, st);
        run++;
    }
    return run;
}

}  // namespace

int main() {
    bitforge::utils::Logger::set_level(bitforge::utils::LogLevel::Error);
    SDL_Init(0);
    std::printf("=== Audio output (FIFO + rate control) tests ===\n\n");
    struct Case { Scenario s; uint64_t max_underruns; double max_jump; double min_jump = 0.0; };
    // A sine this loud moves at most ~580 per 48 kHz sample; fades keep cuts well under 1500.
    const Case cases[] = {
        {{"matched clocks, 10 ms device period"}, 0, 1500},
        {{"sound card 1.2% fast", 0.012}, 0, 1500},
        {{"sound card 1.2% slow", -0.012}, 0, 1500},
        {{"frames run 1% late (59.5 fps)", 0.0, 0.01}, 0, 1500},
        {{"frames 1% late + card 0.3% fast", 0.003, 0.01}, 0, 1500},
        {{"21 ms device period", 0.0, 0.0, 1024}, 0, 1500},
        {{"43 ms device period (Bluetooth-like)", 0.005, 0.0, 2048}, 0, 1500},
        {{"one 150 ms stall", 0.0, 0.0, 480, 60.0, 20.0, 150.0}, 1, 1500},
        {{"frames 3% late (beyond the clamp)", 0.0, 0.03, 480, 60.0}, 1000, 1500},
        {{"state load: flush() at the jump", 0.0, 0.0, 480, 60.0, -1.0, 0.0, 45.0, true}, 0, 1500},
        {{"state load without flush() (control)", 0.0, 0.0, 480, 60.0, -1.0, 0.0, 45.0, false}, 0, 1e9, 5000},
        {{"state load, 43 ms period", 0.0, 0.0, 2048, 60.0, -1.0, 0.0, 45.0, true}, 0, 1500},
        // Startup: the first frames after boot hitch (window creation, shader compiles). The queue
        // is primed before playback and the first 0.5 s don't count, so boot reports nothing.
        {{"boot: 3 frames x 150 ms hitch", 0.0, 0.0, 480, 30.0, -1.0, 0.0, -1.0, true, 150.0}, 0, 1500},
        {{"boot hitch, 43 ms period", 0.0, 0.0, 2048, 30.0, -1.0, 0.0, -1.0, true, 150.0}, 0, 1500},
        {{"unpause + hitch at 10 s", 0.0, 0.0, 480, 30.0, -1.0, 0.0, -1.0, true, 150.0, 10.0}, 0, 1500},
        {{"boot hitch + real stall at 20 s (control)", 0.0, 0.0, 480, 30.0, 20.0, 150.0, -1.0, true, 150.0}, 1, 1500},
    };
    int passed = 0;
    for (const auto& c : cases) {
        const int before = failures;
        const Result r = run(c.s);
        std::printf("  %-40s underruns %4llu  overflows %llu  queue %5.1f..%5.1f ms  rate %+.3f%%  max step %4.0f ... ",
                    c.s.name, static_cast<unsigned long long>(r.underruns), static_cast<unsigned long long>(r.overflows),
                    r.min_q, r.max_q, (r.ratio - 1.0) * 100.0, r.max_jump);
        char what[128];
        std::snprintf(what, sizeof(what), "%s: underruns <= %llu", c.s.name, static_cast<unsigned long long>(c.max_underruns));
        check(r.underruns <= c.max_underruns, what);
        std::snprintf(what, sizeof(what), "%s: no hard cuts", c.s.name);
        check(r.max_jump <= c.max_jump, what);
        check(r.max_jump >= c.min_jump, "the test signal really jumps (control)");
        if (c.s.boot_hitch_ms > 0 && c.max_underruns == 0) check(r.silence_ms == 0.0, "startup hitch reports no silence");
        if (c.s.boot_hitch_ms > 0 && c.s.stall_at >= 0) check(r.underruns == 1, "a real stall after the grace period still counts");
        if (c.max_underruns == 0) {
            check(r.overflows == 0, "no overflows");
            check(r.min_q > 5.0, "queue never runs near empty once settled");
        }
        if (failures == before) { std::printf("PASS\n"); passed++; } else { std::printf("\n"); }
    }

    int lifecycle = run_lifecycle(passed);

    // Mixer: APU + MSU-1 with their gains, saturating; APU alone is unchanged at full volume.
    {
        const int before = failures;
        std::vector<int16_t> out;
        const std::vector<int16_t> apu = {1000, -1000, 30000, -30000, 7, -7};
        bitforge::frontend::mix_sources(apu, std::vector<int16_t>{}, 100, 100, out);
        check(out == apu, "APU alone at 100% passes through");
        bitforge::frontend::mix_sources(apu, std::vector<int16_t>{2000, 2000, 10000, -10000, 0, 0}, 100, 50, out);
        check((out == std::vector<int16_t>{2000, 0, 32767, -32768, 7, -7}), "MSU-1 at 50%, saturated");
        bitforge::frontend::mix_sources(apu, std::vector<int16_t>{2000, 2000, 0, 0, 0, 0}, 50, 100, out);
        check(out[0] == 1500 && out[1] == 500, "master volume scales both");
        bitforge::frontend::mix_sources(apu, std::vector<int16_t>{1, 2}, 100, 100, out);
        check(out.size() == apu.size() && out[0] == 1001 && out[2] == 30000, "a shorter MSU-1 buffer mixes what it has");
        std::printf("  %-40s ... %s\n", "Mixer: APU + MSU-1", failures == before ? "PASS" : "FAIL");
        if (failures == before) passed++;
    }

    // WAV writer: header sizes are patched on close.
    {
        const char* path = "audio_sync_tests.wav";
        bitforge::frontend::WavWriter w;
        const int before = failures;
        check(w.open(path, kSource), "wav open");
        std::vector<int16_t> pcm(2000, 1234);
        w.write(pcm);
        w.close();
        std::FILE* f = std::fopen(path, "rb");
        unsigned char h[44] = {};
        const bool read_ok = f && std::fread(h, 1, 44, f) == 44;
        if (f) std::fclose(f);
        std::remove(path);
        check(read_ok, "wav readable");
        const auto le32 = [&](int at) { return h[at] | (h[at + 1] << 8) | (h[at + 2] << 16) | (h[at + 3] << 24); };
        check(le32(40) == 4000 && le32(4) == 4036 && le32(24) == kSource && h[22] == 2 && h[34] == 16, "wav header");
        std::printf("  %-40s ... %s\n", "WAV writer header", failures == before ? "PASS" : "FAIL");
        if (failures == before) passed++;
    }
    const int total = static_cast<int>(std::size(cases)) + lifecycle + 2;
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, total - passed);
    SDL_Quit();
    return passed == total ? 0 : 1;
}
