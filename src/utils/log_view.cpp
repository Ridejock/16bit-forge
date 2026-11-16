// SPDX-License-Identifier: MIT
#include "utils/log_view.hpp"

#include <string>

namespace snes::utils {

namespace {

auto level_color(LogLevel level) -> ImVec4 {
    switch (level) {
    case LogLevel::Trace:
        return ImVec4(0.45f, 0.50f, 0.55f, 1.f);
    case LogLevel::Debug:
        return ImVec4(0.55f, 0.55f, 0.55f, 1.f);
    case LogLevel::Info:
        return ImVec4(0.86f, 0.86f, 0.86f, 1.f);
    case LogLevel::Warning:
        return ImVec4(0.90f, 0.75f, 0.27f, 1.f);
    case LogLevel::Error:
        return ImVec4(0.94f, 0.35f, 0.35f, 1.f);
    }
    return ImVec4(0.86f, 0.86f, 0.86f, 1.f);
}

auto level_name_getter(void*, int index) -> const char* { return kLogLevelNames[static_cast<size_t>(index)].data(); }

auto channel_name(LogChannel channel) -> std::string_view { return kLogChannels[static_cast<size_t>(channel)].name; }

} // namespace

auto LogView::draw() -> bool {
    bool changed = false;
    ImGui::PushID(static_cast<int>(sink_));

    int level = static_cast<int>(Logger::level(sink_));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.f);
    if (ImGui::Combo("Level", &level, &level_name_getter, nullptr, static_cast<int>(kLogLevelNames.size()))) {
        Logger::set_level(sink_, static_cast<LogLevel>(level));
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Channels")) ImGui::OpenPopup("channels");
    if (ImGui::BeginPopup("channels")) {
        for (size_t i = 0; i < kLogChannelCount; i++) {
            const auto channel = static_cast<LogChannel>(i);
            if (Logger::sink_of(channel) != sink_) continue;
            bool on = Logger::channel_enabled(channel);
            if (ImGui::Checkbox(kLogChannels[i].name.data(), &on)) {
                Logger::set_channel_enabled(channel, on);
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    bool paused = Logger::paused(sink_);
    if (ImGui::Checkbox("Pause", &paused)) Logger::set_paused(sink_, paused);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) Logger::clear(sink_);
    ImGui::SameLine();
    const bool copy = ImGui::Button("Copy");
    ImGui::SetItemTooltip("Copy the lines shown (search applied) to the clipboard");
    ImGui::SameLine();
    if (ImGui::Checkbox("Log file", &file_enabled)) changed = true;
    if (const std::string path = Logger::file_path(sink_); !path.empty()) ImGui::SetItemTooltip("%s", path.c_str());
    ImGui::SameLine();
    filter_.Draw("Search", -ImGui::CalcTextSize("Search").x - ImGui::GetStyle().ItemInnerSpacing.x);

    // Lines are "[LEVEL] text"; the channel goes in front for display, search and copy. Each
    // line is a copy (the emulation thread may be logging meanwhile).
    LogLevel level_of_last = LogLevel::Info;
    auto format = [this, &level_of_last](int i, std::string& out) {
        const Logger::LineCopy l = Logger::line(i, sink_);
        level_of_last = l.level;
        out.assign("[").append(channel_name(l.channel)).append("] ").append(l.text);
    };

    std::string text;
    const int count = Logger::line_count(sink_);
    if (copy) {
        std::string all;
        for (int i = 0; i < count; i++) {
            format(i, text);
            if (filter_.PassFilter(text.c_str())) all.append(text).append("\n");
        }
        ImGui::SetClipboardText(all.c_str());
    }

    ImGui::BeginChild("log_lines", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    auto draw_line = [&](int i) {
        format(i, text);
        ImGui::PushStyleColor(ImGuiCol_Text, level_color(level_of_last));
        ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
        ImGui::PopStyleColor();
    };
    if (filter_.IsActive()) {
        for (int i = 0; i < count; i++) {
            format(i, text);
            if (filter_.PassFilter(text.c_str())) draw_line(i);
        }
    } else {
        ImGuiListClipper clipper;
        clipper.Begin(count);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) draw_line(i);
        }
        clipper.End();
    }

    // SetScrollHereY is applied at the end of the frame, so GetScrollY is still the user's position.
    if (follow_) ImGui::SetScrollHereY(1.f);
    follow_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;
    ImGui::EndChild();
    ImGui::PopID();
    return changed;
}

} // namespace snes::utils
