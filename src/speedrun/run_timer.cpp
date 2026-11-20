// SPDX-License-Identifier: MIT
#include "speedrun/run_timer.hpp"

#include <algorithm>
#include <ctime>

namespace bitforge::speedrun {

namespace {

auto minus(std::optional<Nanos> a, std::optional<Nanos> b) -> std::optional<Nanos> {
  if (!a || !b) return std::nullopt;
  return *a - *b;
}

auto minus(const Times& a, const Times& b) -> Times { return {minus(a.real, b.real), minus(a.game, b.game)}; }

// Segment i's own time in `splits` (cumulative); needs this split and the one before.
auto segment_of(const std::vector<Times>& splits, size_t i, Method m) -> std::optional<Nanos> {
  if (i >= splits.size()) return std::nullopt;
  return minus(splits[i].get(m), i == 0 ? std::optional<Nanos>(0) : splits[i - 1].get(m));
}

auto beats(std::optional<Nanos> t, std::optional<Nanos> best) -> bool { return t && (!best || *t < *best); }

constexpr std::array<std::string_view, 4> kStateLabels = {"Ready", "Running", "Paused", "Finished"};
constexpr std::array<std::string_view, 8> kActionLabels = {"", "start", "split", "undo", "skip", "pause", "resume", "reset"};
constexpr const char* kStampFormat = "%m/%d/%Y %H:%M:%S";

}  // namespace

RunTimer::RunTimer() : run_(std::make_shared<const Run>()) {}

auto RunTimer::set_run(Run run, bool changed) -> void {
  run_ = std::make_shared<const Run>(std::move(run));
  pending_.clear();
  clear_attempt();
  run_changed_ = changed;
}

auto RunTimer::mutate_run() -> Run& {
  auto copy = std::make_shared<Run>(*run_);
  Run& r = *copy;
  run_ = std::move(copy);
  run_changed_ = true;
  return r;
}

auto RunTimer::clear_attempt() -> void {
  const size_t n = run_->segments.size();
  state_ = TimerState::Ready;
  current_ = 0;
  splits_.assign(n, {});
  split_frames_.assign(n, 0);
  split_counts_.assign(n, 0);
  split_lags_.assign(n, 0);
  paused_total_ = 0;
  game_frames_ = 0;
  lag_frames_ = 0;
  hold_until_ = 0;
  tamper_ = 0;
  race_run_ = false;
  race_breaks_ = 0;
  replay_run_ = false;
  counted_ = false;
}

auto RunTimer::event(Action action, int segment, bool automatic, uint64_t frame) -> Event {
  last_event_ = {action, automatic, segment, frame, ++serial_};
  if (automatic) last_auto_ = last_event_;
  return last_event_;
}

// ---- Controls --------------------------------------------------------------------------------

auto RunTimer::start(const Clock& now) -> Event {
  if (state_ != TimerState::Ready || run_->segments.empty()) return {};
  clear_attempt();
  state_ = TimerState::Running;
  start_mono_ = now.mono;
  started_unix_ = now.unix_seconds;
  race_run_ = race_;
  replay_run_ = replaying_;
  if (!replay_run_) count_attempt(true);
  return event(Action::Start, 0);
}

auto RunTimer::split(const Clock& now) -> Event {
  if (state_ != TimerState::Running || current_ >= splits_.size()) return {};
  const size_t i = current_++;
  splits_[i] = now_times(now.mono);
  split_frames_[i] = last_frame_;
  split_counts_[i] = game_frames_;
  split_lags_[i] = lag_frames_;
  if (current_ == splits_.size()) state_ = TimerState::Ended;
  return event(Action::Split, static_cast<int>(i));
}

auto RunTimer::start_or_split(const Clock& now) -> Event {
  return state_ == TimerState::Ready ? start(now) : split(now);
}

auto RunTimer::undo() -> Event {
  if (!active() || current_ == 0) return {};
  if (state_ == TimerState::Ended) state_ = TimerState::Running;
  current_--;
  splits_[current_] = {};
  split_frames_[current_] = 0;
  split_counts_[current_] = 0;
  split_lags_[current_] = 0;
  return event(Action::Undo, static_cast<int>(current_));
}

auto RunTimer::skip() -> Event {
  if ((state_ != TimerState::Running && state_ != TimerState::Paused) || current_ + 1 >= splits_.size()) return {};
  splits_[current_] = {};
  split_frames_[current_] = last_frame_;
  split_counts_[current_] = 0;
  split_lags_[current_] = 0;
  return event(Action::Skip, static_cast<int>(current_++));
}

auto RunTimer::toggle_pause(const Clock& now) -> Event {
  if (state_ == TimerState::Running) {
    state_ = TimerState::Paused;
    pause_start_ = now.mono;
    return event(Action::Pause, static_cast<int>(current_));
  }
  if (state_ == TimerState::Paused) {
    paused_total_ += now.mono - pause_start_;
    state_ = TimerState::Running;
    return event(Action::Resume, static_cast<int>(current_));
  }
  return {};
}

auto RunTimer::reset(const Clock& now) -> Event {
  if (!active()) return {};
  const bool finished = state_ == TimerState::Ended;
  if (!replay_run_) {  // a replay is a movie's run, not the player's: no history, no PB or golds
    if (improves(*run_, splits_, finished, method_)) pending_.push_back({splits_, finished});
    record_attempt(now);
  }
  clear_attempt();
  return event(Action::Reset, -1);
}

// The attempt and each segment it reached go into the history (LiveSplit's AttemptHistory and
// SegmentHistory). A skipped segment gets an empty time; the next one holds the time since the
// last split.
auto RunTimer::record_attempt(const Clock& now) -> void {
  const bool finished = state_ == TimerState::Ended;
  Run& r = mutate_run();
  Attempt a;
  a.id = next_attempt_id(r);
  if (finished) a.time = splits_.back();
  a.race_verified = race_status() == RaceStatus::Verified;
  a.started = utc_stamp(started_unix_);
  a.ended = utc_stamp(now.unix_seconds);
  r.attempts.push_back(a);
  Times last{0, 0};
  for (size_t i = 0; i < current_ && i < r.segments.size(); i++) {
    if (splits_[i].empty()) {
      r.segments[i].history.push_back({a.id, {}});
      continue;
    }
    r.segments[i].history.push_back({a.id, minus(splits_[i], last)});
    last = splits_[i];
  }
}

// The run's attempt count goes up when a player's attempt starts, and back down if a movie
// takes the attempt over.
auto RunTimer::count_attempt(bool on) -> void {
  if (counted_ == on) return;
  mutate_run().attempt_count += on ? 1 : -1;
  counted_ = on;
}

auto RunTimer::improves(const Run& run, const std::vector<Times>& splits, bool finished, Method m) -> bool {
  for (size_t i = 0; i < splits.size() && i < run.segments.size(); i++) {
    for (const MethodInfo& method : kMethods) {
      if (beats(segment_of(splits, i, method.id), run.segments[i].best.get(method.id))) return true;
    }
  }
  return finished && !splits.empty() && beats(splits.back().get(m), pb_time(run, m));
}

auto RunTimer::apply(const Pending& p) -> void {
  Run& r = mutate_run();
  for (size_t i = 0; i < p.splits.size() && i < r.segments.size(); i++) {
    for (const MethodInfo& method : kMethods) {
      const auto t = segment_of(p.splits, i, method.id);
      if (beats(t, r.segments[i].best.get(method.id))) r.segments[i].best.set(method.id, t);
    }
  }
  if (p.finished && p.splits.size() == r.segments.size() && beats(p.splits.back().get(method_), pb_time(r, method_))) {
    for (size_t i = 0; i < p.splits.size(); i++) r.segments[i].pb = p.splits[i];
  }
}

auto RunTimer::pending_improvements() const -> Improvements {
  Improvements out;
  for (const Pending& p : pending_) {
    for (size_t i = 0; i < p.splits.size() && i < run_->segments.size(); i++) {
      out.golds += beats(segment_of(p.splits, i, method_), run_->segments[i].best.get(method_)) ? 1 : 0;
    }
    if (p.finished && !p.splits.empty() && beats(p.splits.back().get(method_), out.pb ? out.pb : pb_time(*run_, method_))) {
      out.pb = p.splits.back().get(method_);
    }
  }
  return out;
}

auto RunTimer::resolve_pending(bool save) -> void {
  if (save) {
    for (const Pending& p : pending_) apply(p);
  }
  pending_.clear();
}

// ---- Frames ----------------------------------------------------------------------------------

auto RunTimer::on_frame(const FrameInfo& f, const ConditionTest& test) -> Event {
  // A rewind or timeline jump takes the run back with the game: splits made after the frame it
  // lands on are undone (so playing through them again splits once), and game time goes back by
  // the frames it undid.
  Event undone;
  if (active() && f.jump == Jump::Rewind) {
    while (current_ > 0 && split_frames_[current_ - 1] > f.frame) undone = undo();
    if (undone.action != Action::None) undone = event(Action::Undo, undone.segment, true, f.frame);
  }
  if (state_ == TimerState::Running || state_ == TimerState::Ended) {
    if (f.jump == Jump::Rewind) {
      const auto ls = last_split();
      const int64_t floor = ls ? split_counts_[*ls] : 0;
      game_frames_ = std::max(floor, game_frames_ + static_cast<int64_t>(f.frame) - static_cast<int64_t>(last_frame_));
      const int64_t lag_floor = ls ? split_lags_[*ls] : 0;
      lag_frames_ = std::max(lag_floor, lag_frames_ + static_cast<int64_t>(f.lag_frames) - static_cast<int64_t>(last_lag_));
    } else {
      game_frames_++;
      if (f.lag) lag_frames_++;
    }
  }
  last_lag_ = f.lag_frames;
  set_race(f.race);
  replaying_ = f.replay;
  if (state_ == TimerState::Running || state_ == TimerState::Paused) {
    tamper_ |= f.tamper;
    if (f.replay && !replay_run_) {
      replay_run_ = true;
      count_attempt(false);  // a movie took over this attempt
    }
    if (race_run_ && (f.jump == Jump::Load || f.jump == Jump::Rewind)) race_breaks_ |= kBreakJump;
    if (race_run_ && !race_) race_breaks_ |= kBreakRaceOff;  // e.g. an undo after the finish, race mode off since
  }
  // A state can be from anywhere: it doesn't move the timer, but auto-splits wait until the game
  // is past the last split's frame again (replaying a state saved before it can't split twice).
  if (active() && f.jump == Jump::Load) {
    if (const auto ls = last_split()) hold_until_ = std::max(hold_until_, split_frames_[*ls]);
  }
  if (f.jump == Jump::Reset) hold_until_ = 0;  // the console's frame count starts over
  last_frame_ = f.frame;
  if (f.jump != Jump::None || !test) return undone;  // a jump's frame never splits

  const Run& r = *run_;
  if (state_ == TimerState::Ready) {
    if (!r.auto_start || !test(r.auto_start->condition) || start(f.now).action == Action::None) return {};
    return event(Action::Start, 0, true, f.frame);
  }
  if (r.auto_reset && test(r.auto_reset->condition)) {
    reset(f.now);
    return event(Action::Reset, -1, true, f.frame);
  }
  if (state_ != TimerState::Running || f.frame <= hold_until_ || current_ >= r.segments.size()) return {};
  const auto& rule = r.segments[current_].auto_split;
  if (!rule || !test(rule->condition)) return {};
  const Event e = split(f.now);
  return event(Action::Split, e.segment, true, f.frame);
}

auto RunTimer::set_race(bool on) -> void {
  if (race_ && !on && race_run_ && (state_ == TimerState::Running || state_ == TimerState::Paused)) race_breaks_ |= kBreakRaceOff;
  race_ = on;
}

auto RunTimer::race_breaks() const -> uint32_t { return race_breaks_ | (tamper_ ? kBreakTamper : 0u); }

auto RunTimer::race_status() const -> RaceStatus {
  if (active() && replay_run_) return RaceStatus::Replay;
  if (!active() || !race_run_) return RaceStatus::Off;
  if (race_breaks() != 0) return RaceStatus::Broken;
  return state_ == TimerState::Ended ? RaceStatus::Verified : RaceStatus::Clean;
}

auto RunTimer::lag_frames() const -> int64_t {
  if (state_ == TimerState::Ended && !split_lags_.empty()) return split_lags_.back();
  return lag_frames_;
}

// The newest split's index, if any (skipped segments have no time).
auto RunTimer::last_split() const -> std::optional<size_t> {
  for (size_t i = current_; i-- > 0;) {
    if (!splits_[i].empty()) return i;
  }
  return std::nullopt;
}

// ---- Queries ---------------------------------------------------------------------------------

auto RunTimer::elapsed(Method m, Nanos now) const -> Nanos {
  if (m == Method::Game) return run_->offset + frames_to_nanos(game_frames_, standard_);
  const Nanos end = state_ == TimerState::Paused ? pause_start_ : now;
  return run_->offset + end - start_mono_ - paused_total_;
}

auto RunTimer::now_times(Nanos now) const -> Times { return {elapsed(Method::Real, now), elapsed(Method::Game, now)}; }

auto RunTimer::time(Method m, Nanos now) const -> Nanos {
  if (state_ == TimerState::Ready) return run_->offset;
  if (state_ == TimerState::Ended && !splits_.empty()) {
    if (const auto t = splits_.back().get(m)) return *t;
  }
  return elapsed(m, now);
}

auto RunTimer::segment_time(size_t i, Method m) const -> std::optional<Nanos> { return segment_of(splits_, i, m); }

auto RunTimer::delta(size_t i, Method m) const -> std::optional<Nanos> {
  if (i >= splits_.size() || i >= run_->segments.size()) return std::nullopt;
  return minus(splits_[i].get(m), run_->segments[i].pb.get(m));
}

auto RunTimer::live_delta(Method m, Nanos now) const -> std::optional<Nanos> {
  if ((state_ != TimerState::Running && state_ != TimerState::Paused) || current_ >= run_->segments.size()) return std::nullopt;
  const auto pb = run_->segments[current_].pb.get(m);
  const Nanos t = elapsed(m, now);
  if (!pb || t <= *pb) return std::nullopt;
  return t - *pb;
}

auto RunTimer::is_gold(size_t i, Method m) const -> bool {
  return i < run_->segments.size() && beats(segment_time(i, m), run_->segments[i].best.get(m));
}

auto RunTimer::has_improvements() const -> bool {
  return active() && !replay_run_ && improves(*run_, splits_, state_ == TimerState::Ended, method_);
}

auto RunTimer::finished_pb() const -> bool {
  return state_ == TimerState::Ended && !replay_run_ && !splits_.empty() && beats(splits_.back().get(method_), pb_time(*run_, method_));
}

auto action_label(Action a) -> std::string_view { return kActionLabels[static_cast<size_t>(a)]; }

auto state_label(TimerState s) -> std::string_view { return kStateLabels[static_cast<size_t>(s)]; }

auto utc_stamp(int64_t unix_seconds) -> std::string {
  if (unix_seconds <= 0) return {};
  const auto t = static_cast<std::time_t>(unix_seconds);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &t);
#else
  gmtime_r(&t, &utc);
#endif
  char text[32];
  std::strftime(text, sizeof(text), kStampFormat, &utc);
  return text;
}

}  // namespace bitforge::speedrun
