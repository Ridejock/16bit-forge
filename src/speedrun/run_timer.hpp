// SPDX-License-Identifier: MIT
#pragma once
// The run timer: start/split, undo, skip, pause and reset over a Run's segments, timed in real
// time and game time at once, with deltas against the PB, golds and the auto-split rules.
//
// It is a plain value with no clock or thread of its own: every call passes the time, and the
// emulation thread calls on_frame() once per emulated frame (the auto-split hook). The frontend
// keeps the one live timer on the emulation thread and shows copies of it (cheap: the Run is
// shared and copied on write).
//
// Game time counts frames: +1 for each frame emulated while the timer runs. A rewind or a
// timeline jump takes the run back with the game: splits made after the frame it lands on are
// undone and game time goes back by the frames it undid, so playing through a trigger again
// splits it once. Loading a state or resetting the console doesn't move the timer (a state can
// come from anywhere); after a state load, auto-splits wait until the game is past the last
// split's frame again. The frame after any jump never triggers an auto rule.
#include "speedrun/splits.hpp"

#include <functional>
#include <utility>
#include <memory>

namespace bitforge::speedrun {

enum class TimerState : uint8_t { Ready, Running, Paused, Ended };

// What moved the game's frame counter, besides one emulated frame, since the last frame.
enum class Jump : uint8_t { None, Reset, Load, Rewind };

struct Clock {
  Nanos mono = 0;            // a steady clock (the same one for every call)
  int64_t unix_seconds = 0;  // for the attempt's started/ended stamps
};

// One emulated frame, as on_frame() sees it.
struct FrameInfo {
  uint64_t frame = 0;   // the game's frame counter after the frame
  Jump jump = Jump::None;
  uint32_t tamper = 0;  // debug::TamperBits right now
  Clock now;
};

enum class Action : uint8_t { None, Start, Split, Undo, Skip, Pause, Resume, Reset };

// What the attempts waiting in pending() would keep, by the primary method.
struct Improvements {
  int golds = 0;                    // best segments beaten
  std::optional<Nanos> pb;          // the new PB's time
};
struct Event {
  Action action = Action::None;
  bool automatic = false;  // from an auto-split rule
  int segment = -1;        // the segment split, skipped or undone
  uint64_t frame = 0;      // the game frame, for automatic events
  uint64_t serial = 0;     // counts every event, so a copy can tell a new one
};

using ConditionTest = std::function<bool(const debug::Condition&)>;

class RunTimer {
public:
  RunTimer();

  // A new run (a loaded file, an edit). Drops the current attempt, unsaved, and any pending
  // improvements. `changed`: the run differs from its file (run_changed() is set).
  auto set_run(Run run, bool changed = false) -> void;
  auto run() const -> const Run& { return *run_; }
  auto set_method(Method m) -> void { method_ = m; }  // the primary method: deltas, golds, PB
  auto method() const -> Method { return method_; }
  auto set_standard(VideoStandard s) -> void { standard_ = s; }
  auto standard() const -> VideoStandard { return standard_; }

  // ---- Controls ----------------------------------------------------------------------------
  auto start(const Clock& now) -> Event;
  auto split(const Clock& now) -> Event;
  auto start_or_split(const Clock& now) -> Event;  // the split hotkey
  auto undo() -> Event;
  auto skip() -> Event;
  auto toggle_pause(const Clock& now) -> Event;
  // Ends the attempt: it goes into the history; if it has new golds or a PB, they wait in
  // pending() until resolve_pending() says whether to keep them.
  auto reset(const Clock& now) -> Event;
  auto pending() const -> bool { return !pending_.empty(); }
  auto pending_improvements() const -> Improvements;
  auto resolve_pending(bool save) -> void;

  // The emulation thread, after every emulated frame. `test` evaluates a watch condition
  // against this frame's readings. Returns what an auto rule did, if anything (an automatic
  // Undo when a rewind went back past a split).
  auto on_frame(const FrameInfo& frame, const ConditionTest& test) -> Event;

  // Set when the run changed in a way its file should get (attempts, history, kept times);
  // take_run_changed() clears it.
  auto run_changed() const -> bool { return run_changed_; }
  auto take_run_changed() -> bool { return std::exchange(run_changed_, false); }

  // ---- What the UI shows -------------------------------------------------------------------
  auto state() const -> TimerState { return state_; }
  auto current() const -> size_t { return current_; }  // the segment being run
  auto splits() const -> const std::vector<Times>& { return splits_; }  // this attempt, cumulative
  // The timer's reading now (the final time once ended, the offset before the start).
  auto time(Method m, Nanos now) const -> Nanos;
  auto game_frames() const -> int64_t { return game_frames_; }
  // Segment i's time in this attempt (nullopt if it or the one before was skipped / not done).
  auto segment_time(size_t i, Method m) const -> std::optional<Nanos>;
  // This attempt against the PB at split i.
  auto delta(size_t i, Method m) const -> std::optional<Nanos>;
  // The running segment against the PB, once it is behind it (LiveSplit's live delta).
  auto live_delta(Method m, Nanos now) const -> std::optional<Nanos>;
  // Split i beat (or set the first) best segment time.
  auto is_gold(size_t i, Method m) const -> bool;
  auto has_improvements() const -> bool;
  auto finished_pb() const -> bool;  // ended, and better than the PB (or the first finish)
  // debug::TamperBits seen during this attempt: a freeze, cheat or memory edit was active.
  auto tampered() const -> uint32_t { return tamper_; }
  auto last_event() const -> const Event& { return last_event_; }
  auto last_auto_event() const -> const Event& { return last_auto_; }

private:
  struct Pending {  // an attempt whose golds / PB wait for an answer
    std::vector<Times> splits;
    bool finished = false;
  };
  auto active() const -> bool { return state_ != TimerState::Ready; }
  auto last_split() const -> std::optional<size_t>;
  auto elapsed(Method m, Nanos now) const -> Nanos;
  auto now_times(Nanos now) const -> Times;
  auto event(Action action, int segment, bool automatic = false, uint64_t frame = 0) -> Event;
  auto mutate_run() -> Run&;  // copy on write
  auto record_attempt(const Clock& now) -> void;
  auto apply(const Pending& p) -> void;
  auto clear_attempt() -> void;
  static auto improves(const Run& run, const std::vector<Times>& splits, bool finished, Method m) -> bool;

  std::shared_ptr<const Run> run_;
  Method method_ = Method::Real;
  VideoStandard standard_ = VideoStandard::Ntsc;
  bool run_changed_ = false;

  TimerState state_ = TimerState::Ready;
  size_t current_ = 0;
  std::vector<Times> splits_;
  std::vector<uint64_t> split_frames_;   // the game frame of each split or skip
  std::vector<int64_t> split_counts_;    // game_frames_ at each split
  Nanos start_mono_ = 0;
  Nanos pause_start_ = 0;
  Nanos paused_total_ = 0;
  int64_t started_unix_ = 0;
  int64_t game_frames_ = 0;
  uint64_t last_frame_ = 0;
  uint64_t hold_until_ = 0;  // auto-splits wait until the game passes this frame
  uint32_t tamper_ = 0;
  std::vector<Pending> pending_;
  Event last_event_;
  Event last_auto_;
  uint64_t serial_ = 0;
};

// "Ready", "Running", ...
auto state_label(TimerState s) -> std::string_view;
// "start", "split", ...
auto action_label(Action a) -> std::string_view;
// Times as the .lss stamps them: "MM/dd/yyyy HH:mm:ss" in UTC.
auto utc_stamp(int64_t unix_seconds) -> std::string;

}  // namespace bitforge::speedrun
