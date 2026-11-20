// SPDX-License-Identifier: MIT
// Run timer and splits (src/speedrun): time text, real and game time through pauses,
// fast-forward and rewind, split / undo / skip / reset, golds and the PB, the .lss reader and
// writer (round trip, a hand-written LiveSplit-style file, keeping what we don't use), the
// auto-split sidecar and its watches, the auto rules' frame logic, and auto-splits against the
// real core on a built-in test ROM, rewound past a trigger. Headless.
//
// Optional, against a real game: timer_tests --rom <file.sfc> --watch <WRAM offset, hex>
//   [--start <frame>] [--hits <n>] [--rewind-at <frame>] [--rewind-to <frame>] [--frames <n>]
// splits every time the watched byte changes (e.g. a homebrew game's HP), with the script's
// input (Start, then walk right), rewinds once past the first split (undoing it) and checks the
// replay splits each segment exactly once.
#include "debug/memory_watch.hpp"
#include "frontend/emulator.hpp"
#include "speedrun/autosplit.hpp"
#include "speedrun/lss.hpp"
#include "speedrun/run_timer.hpp"
#include "speedrun/splits.hpp"
#include "speedrun/splits_file.hpp"
#include "speedrun/xml.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
constexpr Nanos kMs = kNanosPerSecond / 1000;
constexpr int64_t kUnixNow = 1'700'000'000;  // 11/14/2023 22:13:20 UTC

auto at(Nanos mono) -> Clock { return {mono, kUnixNow + mono / kSec}; }

// A frame as on_frame() sees it, with nothing to evaluate.
auto frame(uint64_t n, Jump jump = Jump::None, Nanos mono = 0) -> FrameInfo { return {n, jump, 0, at(mono)}; }
const ConditionTest kNever = [](const debug::Condition&) { return false; };

auto three_segments() -> Run {
  Run r = make_run("Test Game", "Any%", 3);
  r.segments[0].name = "Castle";
  r.segments[1].name = "Forest";
  r.segments[2].name = "Boss";
  return r;
}

// ---- Text ------------------------------------------------------------------------------------

auto test_time_text() -> void {
  CHECK(format_time(0) == "0.00");
  CHECK(format_time(9 * kSec + 876 * kMs) == "9.87");  // truncated, never rounded up
  CHECK(format_time(185 * kSec + 120 * kMs) == "3:05.12");
  CHECK(format_time((3600 + 23 * 60 + 45) * kSec + 670 * kMs) == "1:23:45.67");
  CHECK(format_time(-5 * kSec) == "-5.00");
  CHECK(format_time(61 * kSec + 500 * kMs, 1) == "1:01.5" && format_time(61 * kSec, 0) == "1:01");
  CHECK(format_delta(1230 * kMs) == "+1.23" && format_delta(-450 * kMs) == "-0.45");
  CHECK(format_delta(62 * kSec + 300 * kMs) == "+1:02.3" && format_delta(0) == "+0.00");

  CHECK(format_timespan(0) == "00:00:00");
  CHECK(format_timespan((3600 + 23 * 60 + 45) * kSec + 670 * kMs) == "01:23:45.6700000");
  CHECK(format_timespan(-5 * kSec) == "-00:00:05");
  CHECK(format_timespan((26 * 3600 + 3 * 60 + 4) * kSec + 500 * kMs) == "1.02:03:04.5000000");
  for (const Nanos t : {Nanos{0}, 123 * kMs, -5 * kSec, 3725 * kSec + 4567 * 100, 26 * 3600 * kSec + 1}) {
    const Nanos ticks = t / 100 * 100;  // the file keeps 100 ns ticks
    CHECK(parse_timespan(format_timespan(t)) == ticks);
  }
  CHECK(parse_timespan("00:01:02.5") == 62 * kSec + 500 * kMs);
  CHECK(parse_timespan("1:02") == 62 * kSec && parse_timespan("7.25") == 7 * kSec + 250 * kMs);
  CHECK(!parse_timespan("") && !parse_timespan("abc") && !parse_timespan("1:2:3:4") && !parse_timespan("00:00:0x"));
  CHECK(frames_to_nanos(60, VideoStandard::Ntsc) == 998'355'810);   // 60 frames at 60.0988 Hz
  CHECK(frames_to_nanos(50, VideoStandard::Pal) == 999'860'441);    // 50 frames at 50.0070 Hz
  CHECK(utc_stamp(kUnixNow) == "11/14/2023 22:13:20");
}

// ---- Timing ----------------------------------------------------------------------------------

// Real time is the wall clock less the timer's own pauses: it keeps going while the game is
// paused or in a menu (no frames), and doesn't care about fast-forward.
auto test_real_time() -> void {
  RunTimer t;
  t.set_run(three_segments());
  CHECK(t.state() == TimerState::Ready && t.time(Method::Real, 99 * kSec) == 0);
  CHECK(t.start(at(10 * kSec)).action == Action::Start);
  CHECK(t.time(Method::Real, 15 * kSec) == 5 * kSec);
  // Emulation paused for 20 s: no frames at all. Real time runs on, game time doesn't.
  CHECK(t.time(Method::Real, 35 * kSec) == 25 * kSec && t.time(Method::Game, 35 * kSec) == 0);
  CHECK(t.toggle_pause(at(40 * kSec)).action == Action::Pause && t.state() == TimerState::Paused);
  CHECK(t.time(Method::Real, 50 * kSec) == 30 * kSec);  // held while the timer is paused
  CHECK(t.split(at(50 * kSec)).action == Action::None);  // no splitting while paused
  CHECK(t.toggle_pause(at(52 * kSec)).action == Action::Resume);
  CHECK(t.time(Method::Real, 53 * kSec) == 31 * kSec);
  // An offset: the timer starts there (a countdown when negative).
  Run r = three_segments();
  r.offset = -3 * kSec;
  t.set_run(r);
  CHECK(t.time(Method::Real, 0) == -3 * kSec);
  t.start(at(100 * kSec));
  CHECK(t.time(Method::Real, 101 * kSec) == -2 * kSec && t.time(Method::Game, 101 * kSec) == -3 * kSec);
}

// Game time: one frame per emulated frame, so fast-forward counts every frame it runs; a rewind
// takes back what it undid (and undoes splits it went back past); a state load or console reset
// adds nothing of its own; frames while the timer is paused don't count.
auto test_game_time() -> void {
  RunTimer t;
  t.set_run(three_segments());
  uint64_t f = 1000;
  t.on_frame(frame(f), kNever);  // before the start: only remembered
  t.start(at(0));
  for (int i = 0; i < 60; i++) t.on_frame(frame(++f), kNever);
  CHECK(t.game_frames() == 60 && t.time(Method::Game, 0) == frames_to_nanos(60, VideoStandard::Ntsc));
  // Fast-forward: 240 frames in one real second are 240 frames of game time.
  for (int i = 0; i < 240; i++) t.on_frame(frame(++f, Jump::None, kSec), kNever);
  CHECK(t.game_frames() == 300 && t.time(Method::Real, kSec) == kSec);
  // Rewind: back to a snapshot 4 frames older each step, one frame run from it.
  for (int step = 0; step < 10; step++) {
    f -= 4;
    t.on_frame(frame(++f, Jump::Rewind), kNever);
  }
  CHECK(t.game_frames() == 270);  // 10 steps of -3
  // A split, a short rewind that stays after it, then one past it: the split is undone and game
  // time goes back with the game.
  t.split(at(2 * kSec));
  CHECK(t.splits()[0].game == frames_to_nanos(270, VideoStandard::Ntsc));
  for (int i = 0; i < 30; i++) t.on_frame(frame(++f), kNever);
  f -= 10;
  CHECK(t.on_frame(frame(f, Jump::Rewind), kNever).action == Action::None);
  CHECK(t.game_frames() == 290 && t.current() == 1);
  f -= 100;
  const Event undone = t.on_frame(frame(f, Jump::Rewind), kNever);
  CHECK(undone.action == Action::Undo && undone.automatic && undone.segment == 0);
  CHECK(t.game_frames() == 190 && t.current() == 0 && t.splits()[0].empty());
  // A state load (to any frame number) and a console reset only count their one frame.
  t.on_frame(frame(5, Jump::Load), kNever);
  t.on_frame(frame(1, Jump::Reset), kNever);
  CHECK(t.game_frames() == 192);
  f = 1;
  // Paused timer: frames run but don't count.
  t.toggle_pause(at(3 * kSec));
  for (int i = 0; i < 50; i++) t.on_frame(frame(++f), kNever);
  t.toggle_pause(at(4 * kSec));
  t.on_frame(frame(++f), kNever);
  CHECK(t.game_frames() == 193);
  // PAL counts at 50 Hz.
  t.set_standard(VideoStandard::Pal);
  CHECK(t.time(Method::Game, 0) == frames_to_nanos(193, VideoStandard::Pal));
}

// ---- Splits ----------------------------------------------------------------------------------

auto test_split_controls() -> void {
  RunTimer t;
  CHECK(t.start(at(0)).action == Action::None);  // no segments
  t.set_run(three_segments());
  CHECK(t.split(at(0)).action == Action::None && t.undo().action == Action::None && t.reset(at(0)).action == Action::None);
  CHECK(t.start_or_split(at(0)).action == Action::Start && t.run().attempt_count == 1 && t.take_run_changed());
  CHECK(t.start_or_split(at(10 * kSec)).action == Action::Split && t.current() == 1);
  CHECK(t.splits()[0].real == 10 * kSec);
  CHECK(t.undo().segment == 0 && t.current() == 0 && t.splits()[0].empty());
  t.split(at(11 * kSec));
  CHECK(t.skip().segment == 1 && t.current() == 2 && t.splits()[1].empty());
  CHECK(t.skip().action == Action::None);  // the last segment can't be skipped
  CHECK(!t.segment_time(2, Method::Real));
  CHECK(t.split(at(30 * kSec)).segment == 2 && t.state() == TimerState::Ended);
  CHECK(t.time(Method::Real, 99 * kSec) == 30 * kSec);  // held at the final time
  CHECK(!t.segment_time(2, Method::Real));  // after a skip: no time of its own
  // Undo after the end: running again, the clock never stopped.
  CHECK(t.undo().segment == 2 && t.state() == TimerState::Running && t.time(Method::Real, 40 * kSec) == 40 * kSec);
  t.split(at(41 * kSec));
  CHECK(t.reset(at(50 * kSec)).action == Action::Reset && t.state() == TimerState::Ready);
  const Run& r = t.run();
  CHECK(r.attempts.size() == 1 && r.attempts[0].id == 1 && r.attempts[0].time.real == 41 * kSec);
  CHECK(r.attempts[0].started == utc_stamp(kUnixNow) && r.attempts[0].ended == utc_stamp(kUnixNow + 50));
  CHECK(completed_runs(r) == 1);
  CHECK(r.segments[0].history.size() == 1 && r.segments[0].history[0].time.real == 11 * kSec);
  CHECK(r.segments[1].history[0].time.empty());               // skipped
  CHECK(r.segments[2].history[0].time.real == 30 * kSec);     // the time since the last split
  // An attempt reset early: in the history without a time.
  t.start(at(100 * kSec));
  t.split(at(105 * kSec));
  t.reset(at(106 * kSec));
  CHECK(t.run().attempt_count == 2 && t.run().attempts.size() == 2 && !t.run().attempts[1].finished());
  CHECK(completed_runs(t.run()) == 1 && t.run().segments[1].history.size() == 1);
}

// Golds and the PB: a first run sets everything; later runs show deltas, golds and the live
// delta; improvements wait for an answer at reset.
auto test_golds_and_pb() -> void {
  RunTimer t;
  t.set_run(three_segments());
  auto play = [&t](std::initializer_list<Nanos> seg_times, bool finish = true) {
    Nanos now = 0;
    t.start(at(now));
    size_t i = 0;
    for (const Nanos s : seg_times) {
      now += s;
      if (!finish && ++i == seg_times.size()) break;
      t.split(at(now));
    }
    return now;
  };
  play({10 * kSec, 20 * kSec, 30 * kSec});
  CHECK(t.is_gold(0, Method::Real) && t.is_gold(2, Method::Real));  // no bests yet
  CHECK(t.finished_pb() && t.has_improvements());
  t.reset(at(100 * kSec));
  CHECK(t.pending() && t.pending_improvements().golds == 3 && t.pending_improvements().pb == 60 * kSec);
  t.resolve_pending(true);
  CHECK(!t.pending() && t.run().segments[2].pb.real == 60 * kSec && t.run().segments[1].best.real == 20 * kSec);
  CHECK(pb_time(t.run(), Method::Real) == 60 * kSec && sum_of_best(t.run(), Method::Real) == 60 * kSec);
  CHECK(pb_segment(t.run(), 1, Method::Real) == 20 * kSec);

  // Slower first segment, faster second: a red delta, then a gold with a green delta.
  play({12 * kSec, 15 * kSec});
  CHECK(t.delta(0, Method::Real) == 2 * kSec && !t.is_gold(0, Method::Real));
  CHECK(t.delta(1, Method::Real) == -3 * kSec && t.is_gold(1, Method::Real));
  CHECK(!t.live_delta(Method::Real, 40 * kSec));                   // 40 s, PB split 60 s: not behind
  CHECK(t.live_delta(Method::Real, 61 * kSec) == 1 * kSec);        // behind the PB: shown
  t.reset(at(41 * kSec));
  CHECK(t.pending() && t.pending_improvements().golds == 1 && !t.pending_improvements().pb);
  t.resolve_pending(false);  // "don't save": the gold goes
  CHECK(t.run().segments[1].best.real == 20 * kSec && sum_of_best(t.run(), Method::Real) == 60 * kSec);
  CHECK(t.run().segments[1].history.size() == 2);  // the history keeps it either way

  // Nothing better: no question at reset.
  play({11 * kSec, 21 * kSec}, false);
  t.reset(at(50 * kSec));
  CHECK(!t.pending());

  // A faster finish with one gold, kept: new PB, new best, a lower sum of best.
  play({10 * kSec, 18 * kSec, 31 * kSec});
  CHECK(t.finished_pb() && t.is_gold(1, Method::Real) && !t.is_gold(2, Method::Real));
  t.reset(at(70 * kSec));
  t.resolve_pending(true);
  CHECK(pb_time(t.run(), Method::Real) == 59 * kSec && t.run().segments[1].pb.real == 28 * kSec);
  CHECK(sum_of_best(t.run(), Method::Real) == 58 * kSec);
  // Game time has its own bests (no frames ran here, so all its segments are 0).
  CHECK(sum_of_best(t.run(), Method::Game) == 0);
  // Two answers pending at once (an auto-reset, then another): both are kept.
  play({9 * kSec}, false);
  t.split(at(9 * kSec));
  t.reset(at(10 * kSec));
  play({8 * kSec}, false);
  t.split(at(8 * kSec));
  t.reset(at(10 * kSec));
  t.resolve_pending(true);
  CHECK(t.run().segments[0].best.real == 8 * kSec);
  // Game time as the primary method: the PB is judged by it.
  t.set_method(Method::Game);
  CHECK(t.method() == Method::Game);
}

// ---- .lss ------------------------------------------------------------------------------------

auto sample_run() -> Run {
  Run r = three_segments();
  r.game = "Gothicvania <Homebrew> & Co";
  r.offset = -1500 * kMs;
  r.attempt_count = 4;
  r.attempts = {{1, {}, "11/14/2023 22:13:20", "11/14/2023 22:14:00"},
                {2, {61 * kSec, 59 * kSec}, "11/14/2023 22:15:00", "11/14/2023 22:16:01"}};
  r.segments[0].pb = {10 * kSec, 9 * kSec};
  r.segments[0].best = {9 * kSec, 8 * kSec};
  r.segments[0].history = {{1, {9 * kSec, 8 * kSec}}, {2, {10 * kSec, 9 * kSec}}};
  r.segments[1].pb = {30 * kSec, {}};
  r.segments[1].history = {{2, {}}};
  r.segments[2].pb = {61 * kSec, 59 * kSec};
  for (size_t i = 0; i < r.segments.size(); i++) r.segments[i].source = static_cast<int>(i);
  return r;
}

auto test_lss_round_trip() -> void {
  const Run r = sample_run();
  const std::string text = write_lss(r);
  CHECK(text.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") == 0);
  CHECK(text.find("<Run version=\"1.7.0\">") != std::string::npos);
  CHECK(text.find("<GameName>Gothicvania &lt;Homebrew&gt; &amp; Co</GameName>") != std::string::npos);
  CHECK(text.find("<Offset>-00:00:01.5000000</Offset>") != std::string::npos);
  CHECK(text.find("<SplitTime name=\"Personal Best\">") != std::string::npos);
  std::string error;
  const auto back = read_lss(text, &error);
  CHECK(back && error.empty());
  if (back) CHECK(*back == r);
  // Rules aren't in the .lss.
  Run with_rule = r;
  with_rule.segments[0].auto_split = Rule{{1, debug::Test::Changed, 0}, {"HP", debug::Region::Wram, 0x1FE8, {}, false, 0, 1}};
  CHECK(write_lss(with_rule) == text);
  CHECK(!read_lss("<NotARun/>", &error) && error.find("<Run>") != std::string::npos);
  CHECK(!read_lss("<Run><GameName>x</Run>", &error) && error.find("XML") != std::string::npos);
}

// A file as LiveSplit writes one (written by hand from the format description): icons in CDATA,
// metadata, a custom comparison, auto splitter settings, attempt attributes and a pause time.
constexpr const char* kLiveSplitSample = R"(<?xml version="1.0" encoding="UTF-8"?>
<Run version="1.7.0">
  <GameIcon><![CDATA[AAEAAAD/////AQAAAAAAAAAMAgAAAFFTeXN0ZW0uRHJhd2luZw==]]></GameIcon>
  <GameName>Elevator Madness DX</GameName>
  <CategoryName>All Floors</CategoryName>
  <LayoutPath>
  </LayoutPath>
  <Metadata>
    <Run id="" />
    <Platform usesEmulator="True">
    </Platform>
    <Region>
    </Region>
    <Variables>
      <Variable name="Difficulty">Hard</Variable>
    </Variables>
  </Metadata>
  <Offset>00:00:00</Offset>
  <AttemptCount>7</AttemptCount>
  <AttemptHistory>
    <Attempt id="1" started="10/02/2026 18:00:01" isStartedSynced="True" ended="10/02/2026 18:03:11" isEndedSynced="True" />
    <Attempt id="2" started="10/02/2026 18:05:00" isStartedSynced="True" ended="10/02/2026 18:10:30" isEndedSynced="True">
      <RealTime>00:05:30.2500000</RealTime>
      <GameTime>00:05:12.0000000</GameTime>
      <PauseTime>00:00:04</PauseTime>
    </Attempt>
  </AttemptHistory>
  <Segments>
    <Segment>
      <Name>Floors 1&#x2013;5</Name>
      <Icon><![CDATA[iVBORw0KGgo=]]></Icon>
      <SplitTimes>
        <SplitTime name="Personal Best">
          <RealTime>00:01:40.5000000</RealTime>
          <GameTime>00:01:35.1000000</GameTime>
        </SplitTime>
        <SplitTime name="Sub-5 pace">
          <RealTime>00:01:30</RealTime>
        </SplitTime>
      </SplitTimes>
      <BestSegmentTime>
        <RealTime>00:01:38.7500000</RealTime>
        <GameTime>00:01:33.0000000</GameTime>
      </BestSegmentTime>
      <SegmentHistory>
        <Time id="1">
          <RealTime>00:01:50</RealTime>
        </Time>
        <Time id="2">
          <RealTime>00:01:40.5000000</RealTime>
          <GameTime>00:01:35.1000000</GameTime>
        </Time>
      </SegmentHistory>
    </Segment>
    <Segment>
      <Name>Rooftop &amp; Boss</Name>
      <Icon />
      <SplitTimes>
        <SplitTime name="Personal Best">
          <RealTime>00:05:30.2500000</RealTime>
          <GameTime>00:05:12</GameTime>
        </SplitTime>
      </SplitTimes>
      <BestSegmentTime>
        <RealTime>00:03:49.7500000</RealTime>
      </BestSegmentTime>
      <SegmentHistory>
        <Time id="2">
          <RealTime>00:03:49.7500000</RealTime>
          <GameTime>00:03:36.9000000</GameTime>
        </Time>
      </SegmentHistory>
    </Segment>
  </Segments>
  <AutoSplitterSettings>
    <Setting id="floors">True</Setting>
  </AutoSplitterSettings>
</Run>
)";

auto test_lss_import() -> void {
  std::string error;
  const auto run = read_lss(kLiveSplitSample, &error);
  CHECK(run.has_value() && error.empty());
  if (!run) return;
  CHECK(run->game == "Elevator Madness DX" && run->category == "All Floors" && run->offset == 0);
  CHECK(run->attempt_count == 7 && run->attempts.size() == 2 && completed_runs(*run) == 1);
  CHECK(run->attempts[1].time.real == 330 * kSec + 250 * kMs && run->attempts[1].time.game == 312 * kSec);
  CHECK(run->attempts[0].started == "10/02/2026 18:00:01" && !run->attempts[0].finished());
  CHECK(run->segments.size() == 2);
  CHECK(run->segments[0].name == "Floors 1\xE2\x80\x93" "5");  // &#x2013; is an en dash
  CHECK(run->segments[1].name == "Rooftop & Boss");
  CHECK(run->segments[0].pb.real == 100 * kSec + 500 * kMs && run->segments[0].pb.game == 95 * kSec + 100 * kMs);
  CHECK(run->segments[0].best.real == 98 * kSec + 750 * kMs);
  CHECK(run->segments[1].best.real == 229 * kSec + 750 * kMs && !run->segments[1].best.game);
  CHECK(run->segments[0].history.size() == 2 && run->segments[0].history[0].time.real == 110 * kSec);
  CHECK(sum_of_best(*run, Method::Real) == 328 * kSec + 500 * kMs && !sum_of_best(*run, Method::Game));
  CHECK(pb_time(*run, Method::Game) == 312 * kSec);

  // Play it: the timer accepts it as is.
  RunTimer t;
  t.set_run(*run);
  t.start(at(0));
  t.split(at(99 * kSec));
  CHECK(t.delta(0, Method::Real) == -1500 * kMs && !t.is_gold(0, Method::Real));

  // Written back over itself, changed: our fields update, everything else is still there.
  Run changed = *run;
  changed.segments[0].name = "Floors 1-5";
  changed.segments[1].best.game = 200 * kSec;
  changed.attempt_count = 8;
  changed.attempts.push_back({3, {}, "10/03/2026 09:00:00", "10/03/2026 09:01:00"});
  changed.segments.insert(changed.segments.begin() + 1, Segment{"Lobby", {}, {}, {}, {}, -1});
  const std::string out = write_lss(changed, kLiveSplitSample);
  for (const char* kept : {"<![CDATA[AAEAAAD/////AQAAAAAAAAAMAgAAAFFTeXN0ZW0uRHJhd2luZw==]]>", "<![CDATA[iVBORw0KGgo=]]>",
                           "<SplitTime name=\"Sub-5 pace\">", "<Variable name=\"Difficulty\">Hard</Variable>",
                           "<Setting id=\"floors\">True</Setting>", "<PauseTime>00:00:04</PauseTime>",
                           "isStartedSynced=\"True\"", "<AttemptCount>8</AttemptCount>", "<Name>Floors 1-5</Name>",
                           "<Name>Lobby</Name>", "<GameTime>00:03:20</GameTime>"}) {
    if (out.find(kept) == std::string::npos) {
      std::printf("\n    missing after the rewrite: %s", kept);
      checks_failed++;
    }
  }
  const auto again = read_lss(out);
  CHECK(again && again->segments.size() == 3 && again->segments[1].name == "Lobby" && again->attempts.size() == 3);
  if (again) {
    Run expect = changed;
    for (size_t i = 0; i < expect.segments.size(); i++) expect.segments[i].source = static_cast<int>(i);
    CHECK(*again == expect);
  }
  // The icon stayed with its segment, not its position.
  const size_t lobby = out.find("<Name>Lobby</Name>");
  const size_t icon = out.find("<![CDATA[iVBORw0KGgo=]]>");
  CHECK(icon < lobby);
  // XML corner cases.
  const auto doc = xml::parse("\xEF\xBB\xBF<!-- c --><a x='1' y=\"&quot;&#65;\"><b/><!-- in --><c>t&lt;</c></a>");
  CHECK(doc && doc->attribute("x") == "1" && doc->attribute("y") == "\"A" && doc->children.size() == 2);
  if (doc) CHECK(doc->child_text("c") == "t<");
  for (const char* bad : {"", "<a>", "<a></b>", "<a x=1/>", "<a>&nope;</a>", "<a/><b/>"}) CHECK(!xml::parse(bad));
}

// ---- Auto-splits -----------------------------------------------------------------------------

auto hp_watch(debug::WatchId id = 1) -> debug::Watch { return {"HP", debug::Region::Wram, 0x1FE8, {}, false, 0, id}; }

auto test_sidecar() -> void {
  CHECK(autosplit_path("splits/Gothicvania.lss") == "splits/Gothicvania.autosplit");
  CHECK(autosplit_path("C:\\runs.v2\\game") == "C:\\runs.v2\\game.autosplit");
  Run r = three_segments();
  CHECK(format_autosplits(r).empty());
  const debug::Watch stage{"Stage", debug::Region::Wram, 0x0102, {debug::ValueSize::Bits8, debug::Format::Hex}, false, 0, 3};
  r.auto_start = Rule{{2, debug::Test::Changed, 0}, {"Screen", debug::Region::Wram, 0x100, {}, false, 0, 2}};
  r.segments[0].auto_split = Rule{{1, debug::Test::Less, 3}, hp_watch()};
  r.segments[2].auto_split = Rule{{3, debug::Test::ChangedTo, 4}, stage};
  r.auto_reset = Rule{{1, debug::Test::Equal, 0}, hp_watch()};
  const std::string text = format_autosplits(r);
  CHECK(text.find("split 1 lt 3 | 1 wram 001FE8 8 dec HP\n") != std::string::npos);
  CHECK(text.find("split 3 to $04 | 3 wram 000102 8 hex Stage\n") != std::string::npos);
  CHECK(text.find("start changed | 2 wram 000100 8 dec Screen\n") != std::string::npos);
  Run back = three_segments();
  CHECK(parse_autosplits(text + "split 9 eq 1 | 1 wram 001FE8 8 dec HP\nnonsense\nsplit 2 eq x | 1 wram 0 8 dec\n", back) == 4);
  CHECK(back == r);
  CHECK(describe(*r.segments[0].auto_split) == "HP < 3" && describe(*r.segments[2].auto_split) == "Stage changed to $04");

  // Watches: by id, else by address, else added with a fresh id.
  std::vector<debug::Watch> watches = {hp_watch(7), {"Screen", debug::Region::Wram, 0x100, {}, false, 0, 2}};
  CHECK(resolve_rules(back, watches));  // Stage is new
  CHECK(watches.size() == 3 && watches[2].label == "Stage" && watches[2].id == 8);
  CHECK(back.segments[0].auto_split->condition.watch == 7 && back.auto_reset->condition.watch == 7);
  CHECK(back.auto_start->condition.watch == 2 && back.segments[2].auto_split->condition.watch == 8);
  CHECK(!resolve_rules(back, watches) && watches.size() == 3);
}

// Splits on disk: each game's default file, a remembered other file, the sidecar written with
// the rules and removed without them, and what a write keeps of the file it replaces.
auto test_splits_files() -> void {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "bitforge_timer_files";
  fs::remove_all(dir);
  const std::string data = dir.string();
  const std::string fallback = default_splits_path(data, "Game");
  CHECK(fs::path(fallback).filename() == "Game.lss" && fs::path(fallback).parent_path().filename() == kSplitsFolder);
  CHECK(remembered_splits_path(data, "Game") == fallback);
  const std::string other = (dir / "elsewhere" / "Mine.lss").string();
  CHECK(remember_splits_path(data, "Game", other) && remembered_splits_path(data, "Game") == other);
  CHECK(remember_splits_path(data, "Game", fallback) && remembered_splits_path(data, "Game") == fallback);

  Run r = three_segments();
  r.segments[1].auto_split = Rule{{1, debug::Test::Less, 3}, hp_watch()};
  SplitsFile file{other, std::string(kLiveSplitSample)};
  CHECK(save_splits(r, file) && fs::exists(other) && fs::exists(autosplit_path(other)));
  CHECK(file.original.find("AutoSplitterSettings") != std::string::npos);  // kept from the sample
  const auto loaded = load_splits(other);
  CHECK(loaded && loaded->run.segments[1].auto_split == r.segments[1].auto_split && loaded->file == file);
  r.segments[1].auto_split.reset();
  CHECK(save_splits(r, file) && !fs::exists(autosplit_path(other)));
  std::string error;
  CHECK(!load_splits((dir / "missing.lss").string(), &error) && !error.empty());
  fs::remove_all(dir);
}

// The rules' frame logic with a scripted value: auto-start, splits, the jump frame, a rewind
// past a split, the hold after a state load, and auto-reset.
auto test_rule_logic() -> void {
  Run r = three_segments();
  r.auto_start = Rule{{1, debug::Test::ChangedTo, 3}, hp_watch()};
  for (Segment& s : r.segments) s.auto_split = Rule{{1, debug::Test::Changed, 0}, hp_watch()};
  r.auto_reset = Rule{{1, debug::Test::Equal, 9}, hp_watch()};
  RunTimer t;
  t.set_run(r);
  // The "game": HP per frame; a condition sees (before, now) as the readings would.
  std::map<uint64_t, uint32_t> hp = {{0, 0}, {10, 3}, {50, 2}, {90, 1}};
  auto value = [&hp](uint64_t f) { return std::prev(hp.upper_bound(f))->second; };
  uint64_t before_frame = 0;
  auto run_frame = [&](uint64_t f, Jump jump = Jump::None) {
    const uint64_t prev = jump == Jump::None ? before_frame : f;  // a jump starts the readings over
    const ConditionTest test = [&](const debug::Condition& c) {
      const int64_t b = value(prev), n = value(f), v = c.value;
      return debug::info(c.test).test(b, n, v);
    };
    before_frame = f;
    return t.on_frame({f, jump, 0, at(static_cast<Nanos>(f) * kSec / 60)}, test);
  };
  for (uint64_t f = 1; f < 10; f++) CHECK(run_frame(f).action == Action::None);
  const Event start = run_frame(10);
  CHECK(start.action == Action::Start && start.automatic && start.frame == 10);
  for (uint64_t f = 11; f < 50; f++) run_frame(f);
  const Event first = run_frame(50);
  CHECK(first.action == Action::Split && first.automatic && first.segment == 0 && t.current() == 1);
  CHECK(t.last_auto_event().serial == first.serial);
  for (uint64_t f = 51; f <= 60; f++) run_frame(f);
  // Rewind to frame 40: the split at 50 is undone. Replay: HP changes again at 50 and splits the
  // same segment once.
  const Event undone = run_frame(40, Jump::Rewind);
  CHECK(undone.action == Action::Undo && undone.automatic && undone.segment == 0 && t.current() == 0);
  std::vector<int> split_segments;
  for (uint64_t f = 41; f <= 89; f++) {
    const Event e = run_frame(f);
    if (e.action == Action::Split) split_segments.push_back(e.segment);
  }
  CHECK(split_segments == std::vector<int>{0} && t.current() == 1);
  CHECK(run_frame(90).action == Action::Split && t.current() == 2);
  // A jump frame never splits, even on a change: a state load straight onto a change.
  hp[95] = 5;
  CHECK(run_frame(95, Jump::Load).action == Action::None);
  hp[120] = 6;
  CHECK(run_frame(120, Jump::Load).action == Action::None);  // after a load, held until past frame 90: fine here
  hp[121] = 7;
  CHECK(run_frame(121).action == Action::Split && t.state() == TimerState::Ended);
  // Auto-reset from the end.
  hp[130] = 9;
  const Event reset = run_frame(130);
  CHECK(reset.action == Action::Reset && reset.automatic && t.state() == TimerState::Ready && t.pending());
  // Without a rule's watch value matching, nothing starts again until the rule says so.
  CHECK(run_frame(131).action == Action::None && t.state() == TimerState::Ready);
}

// ---- Against the real core -------------------------------------------------------------------

constexpr uint32_t kFrameCounter = 0x0040;  // the test ROM counts frames here (16-bit)
constexpr uint32_t kLevel = 0x0042;         // ...and keeps a "level" here: frames / 64
constexpr uint32_t kFramesPerLevel = 64;

// A LoROM test cart that counts vblanks: it waits for the NMI flag ($4210 bit 7, set at the
// start of every vblank, cleared by reading) and adds one to the 16-bit $7E0040, from 0. The
// "level" byte $7E0042 is that count / 64, so it changes every 64 frames.
auto write_frame_counter_rom(const std::string& path) -> void {
  std::vector<uint8_t> rom(0x8000, 0x00);
  const uint8_t program[] = {
      0x78,                    // SEI
      0x18,                    // CLC
      0xFB,                    // XCE          native mode
      0xC2, 0x20,              // REP #$20     16-bit A
      0xA9, 0x00, 0x00,        // LDA #0
      0x8F, 0x40, 0x00, 0x7E,  // STA $7E0040  (WRAM powers up with junk)
      0xE2, 0x20,              // wait: SEP #$20
      0xAD, 0x10, 0x42,        // LDA $4210    NMI flag
      0x10, 0xF9,              // BPL wait
      0xC2, 0x20,              // REP #$20     16-bit A
      0xAF, 0x40, 0x00, 0x7E,  // LDA $7E0040
      0x1A,                    // INC A
      0x8F, 0x40, 0x00, 0x7E,  // STA $7E0040
      0x4A, 0x4A, 0x4A,        // LSR A x6     / 64
      0x4A, 0x4A, 0x4A,
      0xE2, 0x20,              // SEP #$20
      0x8F, 0x42, 0x00, 0x7E,  // STA $7E0042
      0x80, 0xE0,              // BRA wait
  };
  std::copy(std::begin(program), std::end(program), rom.begin());
  const char title[] = "TIMER FRAME COUNTER  ";
  std::copy(title, title + 21, rom.begin() + 0x7FC0);
  rom[0x7FD5] = 0x20;  // LoROM
  rom[0x7FD7] = 0x05;  // 32 KB
  rom[0x7FFC] = 0x00;  // reset vector $8000
  rom[0x7FFD] = 0x80;
  rom[0x7FDC] = 0xFF;
  rom[0x7FDD] = 0xFF;
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
}

// The emulation thread's two frame paths, as EmuThread runs them: a frame, and a rewind step
// (load the newest snapshot, run one frame from it).
struct Harness {
  frontend::Emulator emu;
  RunTimer timer;
  std::vector<Event> events;
  Nanos mono = 0;
  auto frame(uint16_t pad = 0) -> void {
    emu.set_joypad(0, pad);
    emu.run_frame();
    mono += kSec / 60;
    note(emu.step_timer(timer, at(mono)));
  }
  auto rewind_step(const std::vector<uint8_t>& snapshot) -> void {
    CHECK(emu.load_snapshot(snapshot));
    emu.run_frame();
    mono += kSec / 60;
    note(emu.step_timer(timer, at(mono)));
  }
  auto note(const Event& e) -> void {
    if (e.action != Action::None) events.push_back(e);
  }
  auto count(Action action) const -> int {
    return static_cast<int>(std::count_if(events.begin(), events.end(), [action](const Event& e) { return e.automatic && e.action == action; }));
  }
  auto auto_splits() const -> int { return count(Action::Split); }
};

auto test_core_auto_splits() -> void {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "bitforge_timer_tests";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string rom = (dir / "Counter.sfc").string();
  write_frame_counter_rom(rom);
  Harness h;
  h.emu.set_data_dir(dir.string());
  CHECK(h.emu.load(rom));
  const debug::Watch counter{"Frames", debug::Region::Wram, kFrameCounter, {debug::ValueSize::Bits16, debug::Format::Unsigned}, false, 0, 0};
  const debug::Watch level{"Level", debug::Region::Wram, kLevel, {}, false, 0, 0};
  CHECK(h.emu.set_watches({counter, level}));
  const debug::Watch frames_w = h.emu.watches()[0];
  const debug::Watch level_w = h.emu.watches()[1];
  // Start when the counter reaches 10; every segment splits on "Level changed", the same rule
  // each time, so a replay through a level change that split the wrong segment would show.
  Run r = make_run("Counter", "Test", 3);
  r.auto_start = Rule{{frames_w.id, debug::Test::ChangedTo, 10}, frames_w};
  for (Segment& s : r.segments) s.auto_split = Rule{{level_w.id, debug::Test::Changed, 0}, level_w};
  // The file's rules go through the same resolve as a loaded file: nothing to add here.
  std::vector<debug::Watch> list = h.emu.watches();
  CHECK(!resolve_rules(r, list));
  h.timer.set_run(r);
  h.timer.set_standard(h.emu.video_standard());

  auto counter_now = [&h] { return debug::read_value(h.emu.memory(debug::Region::Wram), kFrameCounter, debug::ValueSize::Bits16); };
  while (h.timer.state() == TimerState::Ready && h.emu.frame_count() < 100) h.frame();
  CHECK(h.timer.state() == TimerState::Running && counter_now() == 10);
  const uint64_t started = h.emu.frame_count();
  std::vector<uint8_t> before_first;
  while (h.auto_splits() < 1 && h.emu.frame_count() < 200) {
    if (counter_now() == 40) before_first = h.emu.save_snapshot();
    h.frame();
  }
  CHECK(h.auto_splits() == 1 && counter_now() == kFramesPerLevel);
  CHECK(h.timer.game_frames() == kFramesPerLevel - 10);  // counter 10 -> 64
  CHECK(h.timer.splits()[0].game == frames_to_nanos(kFramesPerLevel - 10, VideoStandard::Ntsc));
  const uint64_t split_frame = h.emu.frame_count();
  for (int i = 0; i < 20; i++) h.frame();
  // Rewind past the trigger (back to counter 40): the split is undone and game time goes back
  // with the game. Playing through it again splits the first segment once, at the same time.
  CHECK(!before_first.empty());
  for (int i = 0; i < 3; i++) h.rewind_step(before_first);
  CHECK(h.count(Action::Undo) == 1 && h.timer.current() == 0);
  CHECK(counter_now() < kFramesPerLevel && h.timer.game_frames() == static_cast<int64_t>(counter_now()) - 10);
  while (counter_now() < 100) h.frame();
  CHECK(h.auto_splits() == 2 && h.timer.current() == 1);
  CHECK(h.timer.splits()[0].game == frames_to_nanos(kFramesPerLevel - 10, VideoStandard::Ntsc));
  // A state load from before the next split doesn't move the timer, and replaying through that
  // split's trigger doesn't split again.
  CHECK(h.emu.save_state(1).empty());
  while (h.auto_splits() < 3 && h.emu.frame_count() < 400) h.frame();
  CHECK(h.auto_splits() == 3 && counter_now() == 2 * kFramesPerLevel && h.timer.current() == 2);
  CHECK(h.emu.load_state(1).empty());
  h.emu.run_frame();
  h.note(h.emu.step_timer(h.timer, at(h.mono)));
  while (counter_now() < 2 * kFramesPerLevel + 10) h.frame();
  CHECK(h.auto_splits() == 3 && h.timer.current() == 2);
  while (h.timer.state() != TimerState::Ended && h.emu.frame_count() < 600) h.frame();
  CHECK(h.auto_splits() == 4 && counter_now() == 3 * kFramesPerLevel);
  CHECK(split_frame > started);
  // Nothing was frozen or edited: not "modified". A freeze during a run is.
  CHECK(h.timer.tampered() == 0);
  h.timer.reset(at(h.mono));
  h.timer.resolve_pending(true);
  auto frozen = h.emu.watches();
  frozen[0].frozen = true;
  frozen[0].value = 9;  // the counter held at 9, then let go: it reaches 10 and starts the run
  CHECK(h.emu.set_watches(frozen));
  frozen[0].frozen = false;
  h.frame();
  CHECK(h.emu.set_watches(frozen));
  h.frame();  // counter 9 -> 10: auto-start
  CHECK(h.timer.state() == TimerState::Running && h.timer.tampered() == 0);
  frozen[0].frozen = true;
  frozen[0].value = 20;
  CHECK(h.emu.set_watches(frozen));
  h.frame();
  CHECK((h.timer.tampered() & debug::kTamperFreeze) != 0);
  h.emu.unload();
  fs::remove_all(dir);
}

struct GameOptions {
  std::string rom;
  uint32_t watch = 0;
  int start = 200;       // the timer starts here (when the hero starts walking)
  int hits = 3;          // splits to see (each change of the value)
  int rewind_at = 300;   // after the first split: rewind from here...
  int rewind_to = 200;   // ...to here (before the first split) and replay
  int frames = 420;
};

// Gothicvania-style script: Start pressed at 120-125, walk right from 200.
constexpr uint16_t kPadStart = 0x1000, kPadRight = 0x0100;
auto script(uint64_t f) -> uint16_t {
  return static_cast<uint16_t>((f >= 120 && f < 126 ? kPadStart : 0) | (f >= 200 ? kPadRight : 0));
}

// A real game: split whenever the watched byte changes, rewind past the first split, replay.
auto test_game_auto_splits(const GameOptions& o) -> bool {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "bitforge_timer_game";
  fs::remove_all(dir);
  Harness h;
  h.emu.set_data_dir(dir.string());
  if (!h.emu.load(o.rom)) {
    std::printf("\n    could not load %s", o.rom.c_str());
    return false;
  }
  CHECK(h.emu.set_watches({{"Watched", debug::Region::Wram, o.watch, {}, false, 0, 0}}));
  Run r = make_run("Game", "Hits", static_cast<size_t>(o.hits));
  for (Segment& s : r.segments) s.auto_split = Rule{{h.emu.watches()[0].id, debug::Test::Changed, 0}, h.emu.watches()[0]};
  h.timer.set_run(r);
  std::map<uint64_t, std::vector<uint8_t>> snapshots;
  auto play_to = [&](uint64_t end) {
    while (h.emu.frame_count() < end) {
      if (h.emu.frame_count() % 20 == 0) snapshots[h.emu.frame_count()] = h.emu.save_snapshot();
      h.frame(script(h.emu.frame_count()));
      if (h.emu.frame_count() == static_cast<uint64_t>(o.start)) h.timer.start(at(h.mono));
    }
  };
  play_to(static_cast<uint64_t>(o.rewind_at));
  std::printf("\n    auto-splits before the rewind at frames:");
  for (const Event& e : h.events) std::printf(" %llu", static_cast<unsigned long long>(e.frame));
  CHECK(h.auto_splits() == 1 && h.timer.current() == 1);
  // Rewind: step back through the snapshots to rewind_to, as the rewind hotkey does. The split is
  // undone on the way.
  for (auto it = snapshots.rbegin(); it != snapshots.rend() && it->first >= static_cast<uint64_t>(o.rewind_to); ++it) {
    h.rewind_step(it->second);
  }
  std::printf("\n    rewound to frame %llu (game time %lld frames, %d split undone)", static_cast<unsigned long long>(h.emu.frame_count()),
              static_cast<long long>(h.timer.game_frames()), h.count(Action::Undo));
  CHECK(h.count(Action::Undo) == 1 && h.timer.current() == 0);
  // Replay (the rewound frame ran with the snapshot's input, so the hit can land a frame off):
  // each segment splits exactly once, in order.
  const size_t before = h.events.size();
  play_to(static_cast<uint64_t>(o.frames));
  std::printf("\n    after the replay, auto-splits at frames:");
  std::vector<int> replayed;
  for (size_t i = before; i < h.events.size(); i++) {
    if (h.events[i].action != Action::Split) continue;
    std::printf(" %llu", static_cast<unsigned long long>(h.events[i].frame));
    replayed.push_back(h.events[i].segment);
  }
  std::vector<int> expected(static_cast<size_t>(o.hits));
  for (size_t i = 0; i < expected.size(); i++) expected[i] = static_cast<int>(i);
  CHECK(replayed == expected && h.timer.state() == TimerState::Ended);
  std::printf("\n    watched value now: %u", h.emu.memory(debug::Region::Wram)[o.watch]);
  std::printf("\n    splits (real / game):");
  for (size_t i = 0; i < h.timer.splits().size(); i++) {
    const Times& s = h.timer.splits()[i];
    if (s.real) std::printf(" %s/%s", format_time(*s.real).c_str(), format_time(s.game.value_or(0)).c_str());
  }
  std::printf("\n  ");
  h.emu.unload();
  fs::remove_all(dir);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  utils::Logger::set_level(utils::LogLevel::Error);
  GameOptions game;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const char* v = argv[i + 1];
    if (key == "--rom") game.rom = v;
    else if (key == "--watch") game.watch = static_cast<uint32_t>(std::strtoul(v, nullptr, 16));
    else if (key == "--start") game.start = std::atoi(v);
    else if (key == "--hits") game.hits = std::atoi(v);
    else if (key == "--rewind-at") game.rewind_at = std::atoi(v);
    else if (key == "--rewind-to") game.rewind_to = std::atoi(v);
    else if (key == "--frames") game.frames = std::atoi(v);
  }
  std::printf("=== Run timer / splits tests ===\n\n");
  int passed = 0;
  int failed = 0;
  auto run_test = [&](const std::string& name, auto&& fn) {
    std::printf("  %s... ", name.c_str());
    const int before = checks_failed;
    const bool ok = fn();
    if (ok && checks_failed == before) {
      std::printf("PASSED\n");
      passed++;
    } else {
      std::printf("\n  FAILED\n");
      failed++;
    }
  };
  auto unit = [](auto fn) { return [fn] { fn(); return true; }; };
  run_test("Time text: display, deltas, .lss TimeSpans", unit(test_time_text));
  run_test("Real time: wall clock, timer pause, offset", unit(test_real_time));
  run_test("Game time: frames, fast-forward, rewind, loads, pause, PAL", unit(test_game_time));
  run_test("Split, undo, skip, finish, reset, history", unit(test_split_controls));
  run_test("Golds, PB, deltas, sum of best, save prompt", unit(test_golds_and_pb));
  run_test(".lss round trip", unit(test_lss_round_trip));
  run_test(".lss import (LiveSplit-style sample), rewrite keeps the rest", unit(test_lss_import));
  run_test("Auto-split sidecar and its watches", unit(test_sidecar));
  run_test("Splits files: per-game default, last file, sidecar", unit(test_splits_files));
  run_test("Auto rules: start, split, jump frames, rewind undo, load hold, reset", unit(test_rule_logic));
  run_test("Auto-splits on the real core (built-in ROM), rewind and state load", unit(test_core_auto_splits));
  if (!game.rom.empty()) {
    run_test("Auto-splits on " + std::filesystem::path(game.rom).filename().string(), [&] { return test_game_auto_splits(game); });
  }
  std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
