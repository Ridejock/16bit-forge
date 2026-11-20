// SPDX-License-Identifier: MIT
#include "frontend/timer_panel.hpp"
#include "speedrun/autosplit.hpp"
#include "speedrun/lss.hpp"
#include "speedrun/race.hpp"
#include "utils/logging.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace bitforge::frontend {

namespace {

using speedrun::Method;
using speedrun::Nanos;
using speedrun::RunTimer;
using speedrun::TimerState;

// Colours (ARGB), shared by the tab and the overlay.
constexpr uint32_t kTextColor = 0xFFF0F0F0;
constexpr uint32_t kDimColor = 0xFF9A9A9A;
constexpr uint32_t kAheadColor = 0xFF3CCB6A;
constexpr uint32_t kBehindColor = 0xFFE5534B;
constexpr uint32_t kGoldColor = 0xFFFFC83C;
constexpr uint32_t kModifiedColor = 0xFFFF9A2E;
// The race badge's colour by speedrun::RaceStatus (Off has no badge).
constexpr uint32_t kReplayColor = 0xFFC89BFF;
constexpr std::array<uint32_t, speedrun::kRaceStatuses.size()> kRaceColors = {kDimColor, 0xFF5AB4FF, kAheadColor, kModifiedColor, kReplayColor};

constexpr float kBigTimerScale = 2.6f;    // the tab's main timer, in font sizes
constexpr int kOverlayTimerScale = 2;     // the overlay's main timer, in glyph sizes
constexpr int kOverlayMinChars = 16;      // the overlay box fits this many small glyphs, at least
constexpr int kOverlayGlyph = 8;          // the 8x8 font
constexpr int kOverlayBoxPadding = 7;     // 2 x the text block's padding + its shadow
constexpr const char* kModifiedTag = "modified";
constexpr const char* kModifiedHelp =
    "A memory freeze, cheat or memory edit was active during this run. Nothing is blocked; the tag just says so.";
constexpr const char* kNoWatch = "(no rule)";
constexpr std::string_view kValueWord = "value";  // in debug::kTests labels
// The splits editor's layout, in font sizes.
constexpr float kEditorWidthEm = 60.f;
constexpr float kEditorHeightEm = 26.f;
constexpr float kWatchComboEm = 10.f;
constexpr float kTestComboEm = 10.f;
constexpr float kValueFieldEm = 4.f;
constexpr float kRuleWidthEm = kWatchComboEm + kTestComboEm + kValueFieldEm + 1.5f;

constexpr SDL_DialogFileFilter kSplitsFilters[] = {
    {"LiveSplit splits", "lss"},
    {"All files", "*"},
};

auto color(uint32_t argb) -> ImVec4 {
    return ImVec4(static_cast<float>((argb >> 16) & 0xFF) / 255.f, static_cast<float>((argb >> 8) & 0xFF) / 255.f,
                  static_cast<float>(argb & 0xFF) / 255.f, static_cast<float>(argb >> 24) / 255.f);
}

auto other(Method m) -> Method { return m == Method::Real ? Method::Game : Method::Real; }

auto now_ns() -> Nanos { return static_cast<Nanos>(SDL_GetTicksNS()); }

// Ahead of the PB: green; behind: red; a best segment: gold.
auto delta_color(const RunTimer& t, size_t i, Method m) -> uint32_t {
    if (t.is_gold(i, m)) return kGoldColor;
    const auto d = t.delta(i, m);
    return d && *d < 0 ? kAheadColor : kBehindColor;
}

// The main timer: green while ahead of the PB, red behind it, gold for a new PB at the end.
auto timer_color(const RunTimer& t, Method m, Nanos now) -> uint32_t {
    const auto& segments = t.run().segments;
    switch (t.state()) {
    case TimerState::Ready:
        return kTextColor;
    case TimerState::Paused:
        return kDimColor;
    case TimerState::Ended:
        return t.finished_pb() ? kGoldColor : kBehindColor;
    case TimerState::Running:
        break;
    }
    if (t.current() >= segments.size() || !segments[t.current()].pb.get(m)) return kTextColor;
    return t.live_delta(m, now) ? kBehindColor : kAheadColor;
}

// The newest split's delta (or the live one once behind), for the overlay and the tab.
auto current_delta(const RunTimer& t, Method m, Nanos now, uint32_t& argb) -> std::optional<Nanos> {
    if (const auto live = t.live_delta(m, now)) {
        argb = kBehindColor;
        return live;
    }
    for (size_t i = t.current(); i-- > 0;) {
        if (const auto d = t.delta(i, m)) {
            argb = delta_color(t, i, m);
            return d;
        }
    }
    return std::nullopt;
}

auto segment_label(const RunTimer& t) -> std::string {
    const auto& segments = t.run().segments;
    if (t.state() == TimerState::Ended) return "Finished";
    if (t.state() == TimerState::Ready || t.current() >= segments.size()) return t.run().category.empty() ? "Ready" : t.run().category;
    return segments[t.current()].name;
}

auto copy_text(std::span<char> out, std::string_view text) -> void {
    const size_t n = std::min(text.size(), out.size() - 1);
    std::memcpy(out.data(), text.data(), n);
    out[n] = '\0';
}

// A test's label without its "value" word ("changed to"), as the value field follows it.
auto test_preview(const debug::TestInfo& t) -> std::string {
    std::string label(t.label);
    if (!t.uses_value) return label;
    if (const auto at = label.rfind(kValueWord); at != std::string::npos) label.erase(at > 0 ? at - 1 : at);
    return label;
}

auto race_color(speedrun::RaceStatus s) -> uint32_t { return kRaceColors[static_cast<size_t>(s)]; }

// The badge's hover text: what it means, and why a race run stopped counting.
auto race_help(const RunTimer& t) -> std::string {
    const speedrun::RaceStatus s = t.race_status();
    std::string help(speedrun::info(s).help);
    if (s == speedrun::RaceStatus::Broken) help += " Why: " + speedrun::describe_breaks(t.race_breaks()) + ".";
    return help;
}

auto watch_label(const debug::Watch& w) -> std::string {
    const std::string address = debug::format_address(w.region, w.offset);
    return w.label.empty() ? address : w.label + "  (" + address + ")";
}

} // namespace

auto timer_overlay_lines(const RunTimer& t, Nanos now) -> std::vector<TextLine> {
    if (t.run().segments.empty()) return {};
    const Method m = t.method();
    std::vector<TextLine> lines;
    lines.push_back({segment_label(t), kDimColor, 1});
    lines.push_back({speedrun::format_time(t.time(m, now)), timer_color(t, m, now), kOverlayTimerScale});
    uint32_t argb = kTextColor;
    const auto d = current_delta(t, m, now, argb);
    std::string info = std::string(speedrun::info(other(m)).short_label) + " " + speedrun::format_time(t.time(other(m), now));
    if (d) info = speedrun::format_delta(*d) + "  " + info;
    lines.push_back({info, d ? argb : kDimColor, 1});
    // A race run shows its race badge (which covers tampering); any other run the "modified" tag.
    if (const auto race = t.race_status(); race != speedrun::RaceStatus::Off) {
        lines.push_back({std::string(speedrun::info(race).tag), race_color(race), 1});
    } else if (t.tampered()) {
        lines.push_back({kModifiedTag, kModifiedColor, 1});
    }
    return lines;
}

// ---- Every frame -----------------------------------------------------------------------------

auto TimerPanel::update(const EmuStatus& status, const Notify& notify) -> void {
    const RunTimer& t = status.timer;
    const speedrun::Event& e = t.last_event();
    if (e.serial != seen_serial_ && seen_serial_ != 0 && e.action != speedrun::Action::None) {
        const auto& segments = t.run().segments;
        const std::string name = e.segment >= 0 && static_cast<size_t>(e.segment) < segments.size() ? segments[static_cast<size_t>(e.segment)].name : "";
        std::string text = "Timer: " + std::string(speedrun::action_label(e.action));
        const bool names_segment = e.action == speedrun::Action::Split || e.action == speedrun::Action::Undo || e.action == speedrun::Action::Skip;
        if (names_segment && !name.empty()) text += " " + name;
        if (e.action == speedrun::Action::Split) {
            if (const auto d = t.delta(static_cast<size_t>(e.segment), t.method())) text += " " + speedrun::format_delta(*d);
            if (t.is_gold(static_cast<size_t>(e.segment), t.method())) text += " (gold)";
        }
        if (e.automatic) {
            text = "Auto-" + text.substr(text.find(' ') + 1) + " at frame " + std::to_string(e.frame);
            last_auto_ = text;
        }
        notify(text);
    }
    seen_serial_ = std::max<uint64_t>(e.serial, 1);
    if (t.pending() && !prompt_answered_) prompt_open_ = true;
    if (!t.pending()) prompt_answered_ = false;

    std::optional<Dialog> dialog;
    std::optional<std::string> path;
    {
        std::lock_guard lock(dialog_mutex_);
        if (dialog_path_) {
            dialog = dialog_open_;
            path = std::exchange(dialog_path_, std::nullopt);
            dialog_open_.reset();
        }
    }
    if (!dialog || !path || path->empty()) return;
    switch (*dialog) {
    case Dialog::Open: {
        const std::string error = emu_.call([&path](EmuThread::Core& c) { return c.open_splits(*path) ? std::string() : c.splits.error; });
        notify(error.empty() ? "Splits: " + std::filesystem::path(*path).filename().string() : error);
        break;
    }
    case Dialog::New:
        emu_.call([&path](EmuThread::Core& c) { c.new_splits(*path, true); });
        notify("Splits: new file " + std::filesystem::path(*path).filename().string());
        break;
    case Dialog::Export: {
        const std::string error = emu_.call([&path](EmuThread::Core& c) {
            speedrun::SplitsFile copy{*path, c.splits.file.original};
            std::string why;
            return speedrun::save_splits(c.splits.timer.run(), copy, &why) ? std::string() : why;
        });
        notify(error.empty() ? "Splits exported to " + *path : error);
        break;
    }
    }
}

auto TimerPanel::command(TimerCommand command, uint64_t when_ns) -> void {
    const speedrun::Clock now = EmuThread::timer_clock(when_ns);
    emu_.post([command, now](EmuThread::Core& c) {
        RunTimer& t = c.splits.timer;
        switch (command) {
        case TimerCommand::Split:
            t.start_or_split(now);
            break;
        case TimerCommand::Undo:
            t.undo();
            break;
        case TimerCommand::Skip:
            t.skip();
            break;
        case TimerCommand::Reset:
            t.reset(now);
            break;
        case TimerCommand::Pause:
            t.toggle_pause(now);
            break;
        }
    });
}

auto TimerPanel::overlay_image(const EmuStatus& status, int& width, int& height) const -> std::vector<uint32_t> {
    width = height = 0;
    if (!config_.overlay || !status.loaded) return {};
    const auto lines = timer_overlay_lines(status.timer, now_ns());
    return render_text_block(lines, info(config_.corner).right, kOverlayMinChars * kOverlayGlyph + kOverlayBoxPadding, width, height);
}

// ---- The Timer tab ---------------------------------------------------------------------------

auto TimerPanel::draw(const EmuStatus& status, SDL_Window* window) -> bool {
    if (!status.loaded) {
        ImGui::TextDisabled("Load a game to time a run. Its splits are kept per game (splits/<rom>.lss).");
        return false;
    }
    sync_watches(status);
    const RunTimer& t = status.timer;
    const Nanos now = now_ns();
    const auto& run = t.run();
    ImGui::Text("%s  -  %s", run.game.empty() ? status.rom_name.c_str() : run.game.c_str(), run.category.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("   attempts %d, completed %d, race verified %d", run.attempt_count, speedrun::completed_runs(run),
                        speedrun::verified_runs(run));
    draw_times(t, now);
    draw_splits(t, now);
    const bool changed = draw_controls(status);
    draw_file(status, window);
    return changed;
}

auto TimerPanel::draw_times(const RunTimer& t, Nanos now) -> void {
    const Method m = t.method();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kBigTimerScale);
    ImGui::TextColored(color(timer_color(t, m, now)), "%s", speedrun::format_time(t.time(m, now)).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s  %s", speedrun::info(other(m)).short_label.data(), speedrun::format_time(t.time(other(m), now)).c_str());
    ImGui::TextDisabled("%s  %s", speedrun::info(m).label.data(), speedrun::state_label(t.state()).data());
    if (const auto race = t.race_status(); race != speedrun::RaceStatus::Off) {
        ImGui::SameLine();
        ImGui::TextColored(color(race_color(race)), "[%s]", speedrun::info(race).tag.data());
        ImGui::SetItemTooltip("%s", race_help(t).c_str());
    } else if (t.tampered()) {
        ImGui::SameLine();
        ImGui::TextColored(color(kModifiedColor), "[%s]", kModifiedTag);
        ImGui::SetItemTooltip("%s", kModifiedHelp);
    } else if (t.race() && t.state() == TimerState::Ready) {
        ImGui::SameLine();
        ImGui::TextColored(color(race_color(speedrun::RaceStatus::Clean)), "[race mode: ready]");
        ImGui::SetItemTooltip("Race mode is on: the next run you start counts as a race run.");
    }
    ImGui::EndGroup();
}

auto TimerPanel::draw_splits(const RunTimer& t, Nanos now) -> void {
    const Method m = t.method();
    const auto& run = t.run();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
    const float rows_h = std::max(ImGui::GetTextLineHeightWithSpacing() * 4.f,
                                  ImGui::GetContentRegionAvail().y - 5.f * ImGui::GetFrameHeightWithSpacing());
    if (ImGui::BeginTable("splits", 5, flags, ImVec2(0.f, rows_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Segment", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("+00:00.00").x);
        ImGui::TableSetupColumn("Segment time", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Segment time").x);
        ImGui::TableSetupColumn("Split", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0:00:00.00").x);
        ImGui::TableSetupColumn("Best", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0:00:00.00").x);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < run.segments.size(); i++) {
            const speedrun::Segment& s = run.segments[i];
            const bool done = i < t.current() && t.state() != TimerState::Ready;
            const bool live = i == t.current() && (t.state() == TimerState::Running || t.state() == TimerState::Paused);
            ImGui::TableNextRow();
            if (live) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_HeaderActive, 0.45f));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name.c_str());
            if (s.auto_split) {
                ImGui::SameLine();
                ImGui::TextDisabled("[auto: %s]", speedrun::describe(*s.auto_split).c_str());
            }
            ImGui::TableNextColumn();
            if (done) {
                if (const auto d = t.delta(i, m)) {
                    ImGui::TextColored(color(delta_color(t, i, m)), "%s", speedrun::format_delta(*d).c_str());
                } else if (t.is_gold(i, m)) {
                    ImGui::TextColored(color(kGoldColor), "gold");
                }
            } else if (live) {
                if (const auto d = t.live_delta(m, now)) ImGui::TextColored(color(kBehindColor), "%s", speedrun::format_delta(*d).c_str());
            }
            ImGui::TableNextColumn();
            if (const auto seg = done ? t.segment_time(i, m) : std::nullopt) {
                ImGui::TextColored(color(t.is_gold(i, m) ? kGoldColor : kTextColor), "%s", speedrun::format_time(*seg).c_str());
            } else if (const auto pb = speedrun::pb_segment(run, i, m)) {
                ImGui::TextDisabled("%s", speedrun::format_time(*pb).c_str());
            }
            ImGui::TableNextColumn();
            const auto split = done ? t.splits()[i].get(m) : std::nullopt;
            if (split) {
                ImGui::TextUnformatted(speedrun::format_time(*split).c_str());
            } else if (done) {
                ImGui::TextDisabled("skipped");
            } else if (const auto pb = s.pb.get(m)) {
                ImGui::TextDisabled("%s", speedrun::format_time(*pb).c_str());
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            if (const auto best = s.best.get(m)) ImGui::TextDisabled("%s", speedrun::format_time(*best).c_str());
        }
        ImGui::EndTable();
    }
    const auto sob = speedrun::sum_of_best(run, m);
    const auto pb = speedrun::pb_time(run, m);
    ImGui::Text("Sum of best: %s    Personal best: %s", sob ? speedrun::format_time(*sob).c_str() : "-",
                pb ? speedrun::format_time(*pb).c_str() : "-");
    if (!last_auto_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("   last: %s", last_auto_.c_str());
    }
}

auto TimerPanel::draw_controls(const EmuStatus& status) -> bool {
    const RunTimer& t = status.timer;
    struct Button {
        const char* label;
        TimerCommand command;
    };
    const Button buttons[] = {
        {t.state() == TimerState::Ready ? "Start" : "Split", TimerCommand::Split},
        {"Undo", TimerCommand::Undo},
        {"Skip", TimerCommand::Skip},
        {t.state() == TimerState::Paused ? "Resume" : "Pause", TimerCommand::Pause},
        {"Reset", TimerCommand::Reset},
    };
    for (const Button& b : buttons) {
        const bool blocked = b.command == TimerCommand::Pause && !speedrun::race_allows(status.race, speedrun::Guard::TimerPause);
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button(b.label)) command(b.command);
        ImGui::EndDisabled();
        if (blocked) ImGui::SetItemTooltip("%s", speedrun::blocked_tooltip(speedrun::Guard::TimerPause).c_str());
        ImGui::SameLine();
    }
    bool changed = false;
    ImGui::TextUnformatted("   Compare by");
    for (const auto& method : speedrun::kMethods) {
        ImGui::SameLine();
        if (ImGui::RadioButton(method.label.data(), config_.primary == method.id)) {
            config_.primary = method.id;
            emu_.post([m = method.id](EmuThread::Core& c) { c.splits.timer.set_method(m); });
            changed = true;
        }
    }
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Overlay in the game window", &config_.overlay);
    if (config_.overlay) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("Bottom right").x + ImGui::GetFrameHeight() * 2.f);
        if (ImGui::BeginCombo("##corner", info(config_.corner).label.data())) {
            for (size_t i = 0; i < kCorners.size(); i++) {
                if (ImGui::Selectable(kCorners[i].label.data(), config_.corner == static_cast<Corner>(i))) {
                    config_.corner = static_cast<Corner>(i);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    return changed;
}

auto TimerPanel::draw_file(const EmuStatus& status, SDL_Window* window) -> void {
    const bool ready = status.timer.state() == TimerState::Ready;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Edit splits...")) open_editor(status.timer.run());
    ImGui::SameLine();
    if (ImGui::Button("Open / import .lss...")) show_dialog(Dialog::Open, window, status.splits_path);
    ImGui::SameLine();
    if (ImGui::Button("New...")) show_dialog(Dialog::New, window, status.splits_path);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Export .lss...")) show_dialog(Dialog::Export, window, status.splits_path);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status.splits_path.c_str());
    if (!ready) ImGui::SetItemTooltip("Reset the timer to edit or change the splits.");
    if (!status.splits_error.empty()) ImGui::TextColored(color(kBehindColor), "%s", status.splits_error.c_str());
    if (!status.verification_path.empty()) {
        ImGui::TextColored(color(race_color(speedrun::RaceStatus::Verified)), "Verification file:");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status.verification_path.c_str());
        ImGui::SetItemTooltip("Written when the last race verified run finished: ROM hashes, emulator version, timing, times.");
    }
}

auto TimerPanel::show_dialog(Dialog dialog, SDL_Window* window, const std::string& start) -> void {
    {
        std::lock_guard lock(dialog_mutex_);
        if (dialog_open_) return;  // one at a time
        dialog_open_ = dialog;
    }
    const std::string folder = std::filesystem::path(start).parent_path().string();
    const char* location = folder.empty() ? nullptr : folder.c_str();
    const int filters = static_cast<int>(std::size(kSplitsFilters));
    if (dialog == Dialog::Open) {
        SDL_ShowOpenFileDialog(&TimerPanel::on_dialog, this, window, kSplitsFilters, filters, location, false);
    } else {
        SDL_ShowSaveFileDialog(&TimerPanel::on_dialog, this, window, kSplitsFilters, filters, location);
    }
}

void SDLCALL TimerPanel::on_dialog(void* userdata, const char* const* files, int) {
    auto* self = static_cast<TimerPanel*>(userdata);
    std::lock_guard lock(self->dialog_mutex_);
    if (!files || !files[0]) {  // cancelled or failed
        self->dialog_open_.reset();
        return;
    }
    std::string path = files[0];
    if (self->dialog_open_ != Dialog::Open && std::filesystem::path(path).extension().empty()) path += speedrun::kLssExtension;
    self->dialog_path_ = std::move(path);
}

auto TimerPanel::sync_watches(const EmuStatus& status) -> void {
    if (rom_ == status.rom_path && watches_revision_ == status.watches_revision) return;
    std::tie(watches_, watches_revision_) =
        emu_.call([](EmuThread::Core& c) { return std::pair{c.emu.watches(), c.emu.watches_revision()}; });
    rom_ = status.rom_path;
}

// ---- The splits editor -----------------------------------------------------------------------

auto TimerPanel::rule_edit(const std::optional<speedrun::Rule>& rule) const -> RuleEdit {
    RuleEdit edit;
    if (!rule) return edit;
    const auto it = std::find_if(watches_.begin(), watches_.end(), [&rule](const debug::Watch& w) { return w.id == rule->condition.watch; });
    if (it == watches_.end()) return edit;
    edit.watch = static_cast<int>(it - watches_.begin());
    edit.test = static_cast<int>(rule->condition.test);
    copy_text(edit.value, debug::format_value(rule->condition.value, it->type));
    return edit;
}

auto TimerPanel::open_editor(const speedrun::Run& run) -> void {
    editor_ = Editor{};
    editor_.open = true;
    editor_.run = run;
    copy_text(editor_.game, run.game);
    copy_text(editor_.category, run.category);
    for (const speedrun::Segment& s : run.segments) {
        editor_.names.emplace_back();
        copy_text(editor_.names.back(), s.name);
        editor_.splits.push_back(rule_edit(s.auto_split));
    }
    editor_.start = rule_edit(run.auto_start);
    editor_.reset = rule_edit(run.auto_reset);
}

auto TimerPanel::make_rule(const RuleEdit& edit, std::string& error) const -> std::optional<speedrun::Rule> {
    if (edit.watch < 0 || static_cast<size_t>(edit.watch) >= watches_.size()) return std::nullopt;
    const debug::Watch& w = watches_[static_cast<size_t>(edit.watch)];
    speedrun::Rule rule{{w.id, debug::kTests[static_cast<size_t>(edit.test)].id, 0}, w};
    if (debug::kTests[static_cast<size_t>(edit.test)].uses_value) {
        const auto value = debug::parse_value(edit.value.data(), w.type);
        if (!value) {
            error = "\"" + std::string(edit.value.data()) + "\" isn't a value for " + watch_label(w);
            return std::nullopt;
        }
        rule.condition.value = *value;
    }
    return rule;
}

auto TimerPanel::apply_editor() -> bool {
    speedrun::Run run = editor_.run;
    std::string error;
    run.game = editor_.game.data();
    run.category = editor_.category.data();
    for (size_t i = 0; i < run.segments.size(); i++) {
        run.segments[i].name = editor_.names[i].data();
        run.segments[i].auto_split = make_rule(editor_.splits[i], error);
    }
    run.auto_start = make_rule(editor_.start, error);
    run.auto_reset = make_rule(editor_.reset, error);
    if (run.segments.empty()) error = "A run needs at least one segment.";
    editor_.error = error;
    if (!error.empty()) return false;
    emu_.post([run = std::move(run)](EmuThread::Core& c) mutable { c.set_run(std::move(run)); });
    return true;
}

auto TimerPanel::draw_rule(const char* id, RuleEdit& rule) -> void {
    ImGui::PushID(id);
    const std::string current = rule.watch >= 0 && static_cast<size_t>(rule.watch) < watches_.size()
                                    ? watch_label(watches_[static_cast<size_t>(rule.watch)])
                                    : kNoWatch;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * kWatchComboEm);
    if (ImGui::BeginCombo("##watch", current.c_str())) {
        if (ImGui::Selectable(kNoWatch, rule.watch < 0)) rule.watch = -1;
        for (size_t i = 0; i < watches_.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(watch_label(watches_[i]).c_str(), rule.watch == static_cast<int>(i))) rule.watch = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (rule.watch >= 0) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * kTestComboEm);
        if (ImGui::BeginCombo("##test", test_preview(debug::kTests[static_cast<size_t>(rule.test)]).c_str())) {
            for (size_t i = 0; i < debug::kTests.size(); i++) {
                if (ImGui::Selectable(debug::kTests[i].label.data(), rule.test == static_cast<int>(i))) rule.test = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        if (debug::kTests[static_cast<size_t>(rule.test)].uses_value) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * kValueFieldEm);
            ImGui::InputText("##value", rule.value.data(), rule.value.size());
        }
    }
    ImGui::PopID();
}

auto TimerPanel::draw_editor() -> void {
    if (!editor_.open) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * kEditorWidthEm, ImGui::GetFontSize() * kEditorHeightEm), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Splits editor", &editor_.open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::InputText("Game", editor_.game.data(), editor_.game.size());
    ImGui::InputText("Category", editor_.category.data(), editor_.category.size());
    ImGui::SeparatorText("Auto rules (on memory watches: add them in Debug > Memory watch)");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Start when");
    ImGui::SameLine(ImGui::GetFontSize() * 6.f);
    draw_rule("start", editor_.start);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Reset when");
    ImGui::SameLine(ImGui::GetFontSize() * 6.f);
    draw_rule("reset", editor_.reset);

    ImGui::SeparatorText("Segments");
    std::optional<size_t> remove;
    std::optional<std::pair<size_t, size_t>> swap;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable("segments", 3, flags, ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing() * 2.f))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Auto-split when", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * kRuleWidthEm);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() * 4.5f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < editor_.names.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", editor_.names[i].data(), editor_.names[i].size());
            ImGui::TableNextColumn();
            draw_rule("split", editor_.splits[i]);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) swap = std::pair{i - 1, i};
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == editor_.names.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) swap = std::pair{i, i + 1};
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = i;
            ImGui::SetItemTooltip("Remove (its times go with it)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    auto& segs = editor_.run.segments;
    if (swap) {
        std::swap(segs[swap->first], segs[swap->second]);
        std::swap(editor_.names[swap->first], editor_.names[swap->second]);
        std::swap(editor_.splits[swap->first], editor_.splits[swap->second]);
    }
    if (remove) {
        segs.erase(segs.begin() + static_cast<std::ptrdiff_t>(*remove));
        editor_.names.erase(editor_.names.begin() + static_cast<std::ptrdiff_t>(*remove));
        editor_.splits.erase(editor_.splits.begin() + static_cast<std::ptrdiff_t>(*remove));
    }
    if (ImGui::Button("Add segment")) {
        speedrun::Segment s;
        s.name = "Split " + std::to_string(segs.size() + 1);
        segs.push_back(s);
        editor_.names.emplace_back();
        copy_text(editor_.names.back(), s.name);
        editor_.splits.push_back(editor_.splits.empty() ? RuleEdit{} : editor_.splits.back());  // the same rule as the last, often
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (apply_editor()) editor_.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) editor_.open = false;
    if (!editor_.error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(color(kBehindColor), "%s", editor_.error.c_str());
    }
    ImGui::End();
}

auto TimerPanel::draw_prompt(const EmuStatus& status) -> void {
    constexpr const char* kTitle = "Save new times?";
    if (prompt_open_ && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    const speedrun::Improvements imp = status.timer.pending_improvements();
    if (imp.pb) ImGui::TextColored(color(kGoldColor), "New personal best: %s", speedrun::format_time(*imp.pb).c_str());
    if (imp.golds > 0) ImGui::Text("%d new best segment%s", imp.golds, imp.golds == 1 ? "" : "s");
    ImGui::TextDisabled("The attempt is in the history either way.");
    auto answer = [this](bool save) {
        emu_.post([save](EmuThread::Core& c) { c.splits.timer.resolve_pending(save); });
        prompt_open_ = false;
        prompt_answered_ = true;
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("Save")) answer(true);
    ImGui::SameLine();
    if (ImGui::Button("Don't save")) answer(false);
    ImGui::EndPopup();
}

auto TimerPanel::draw_windows(const EmuStatus& status) -> void {
    if (editor_.open) sync_watches(status);
    draw_editor();
    draw_prompt(status);
}

} // namespace bitforge::frontend
