// SPDX-License-Identifier: MIT
#include "frontend/memory_panel.hpp"

#include "utils/logging.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

namespace bitforge::frontend {

namespace {

using debug::Region;
using debug::ValueType;

// Widths in font sizes, so they follow the DPI scale.
constexpr float kSearchShare = 0.40f;  // of the tab's width
constexpr float kComboWidth = 7.f;
constexpr float kCompareWidth = 11.f;
constexpr float kFieldWidth = 7.f;
constexpr float kAddressWidth = 6.5f;
constexpr float kValueWidth = 5.f;
constexpr float kTypeWidth = 10.5f;
constexpr float kHelpLines = 2.2f;  // reserved under the watch list for its (wrapped) help text
constexpr ImVec4 kErrorColor(1.f, 0.4f, 0.4f, 1.f);
constexpr ImVec4 kFrozenColor(0.45f, 0.75f, 1.f, 1.f);   // a frozen watch's value
constexpr ImVec4 kChangedColor(1.f, 0.85f, 0.4f, 1.f);  // a candidate that differs from its previous value

auto em(float n) -> float { return ImGui::GetFontSize() * n; }

auto copy_to(std::array<char, 32>& buffer, const std::string& text) -> void {
    const size_t n = std::min(text.size(), buffer.size() - 1);
    std::memcpy(buffer.data(), text.data(), n);
    buffer[n] = '\0';
}

auto type_label(ValueType t) -> std::string {
    return std::string(debug::info(t.size).label) + " " + std::string(debug::info(t.format).label);
}

// A combo over one of the debug tables; true when the choice changed.
template <typename Table, typename Id, typename Usable>
auto table_combo(const char* id, const Table& table, Id& value, float width, Usable usable) -> bool {
    bool changed = false;
    ImGui::SetNextItemWidth(em(width));
    if (ImGui::BeginCombo(id, debug::info(value).label.data())) {
        for (const auto& e : table) {
            if (!usable(e.id)) continue;
            if (ImGui::Selectable(e.label.data(), e.id == value) && e.id != value) {
                value = e.id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Size and format as one combo ("16-bit Hex").
// `width` in font sizes; negative fills the space left.
auto type_combo(const char* id, ValueType& type, float width) -> bool {
    bool changed = false;
    ImGui::SetNextItemWidth(width < 0.f ? -FLT_MIN : em(width));
    if (ImGui::BeginCombo(id, type_label(type).c_str())) {
        for (const auto& size : debug::kSizes) {
            for (const auto& format : debug::kFormats) {
                const ValueType t{size.id, format.id};
                if (ImGui::Selectable(type_label(t).c_str(), t == type) && t != type) {
                    type = t;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

auto live_value(const MemoryImage& memory, const debug::Watch& w) -> std::optional<uint32_t> {
    const auto bytes = memory.region(w.region);
    if (!debug::fits(bytes.size(), w.offset, w.type.size)) return std::nullopt;
    return debug::read_value(bytes, w.offset, w.type.size);
}

} // namespace

auto MemoryPanel::draw(const EmuStatus& status, const AddCheat& add_cheat, const Notify& notify) -> void {
    if (!status.loaded) {
        ImGui::TextDisabled("Load a game to search and watch its memory.");
        return;
    }
    sync_game(status);
    emu_.take_memory();
    const MemoryImage& memory = emu_.memory();
    if (memory.rom_path != rom_) {
        ImGui::TextDisabled("Reading the game's memory...");
        return;
    }
    const float search_w = std::max(1.f, ImGui::GetContentRegionAvail().x * kSearchShare);
    ImGui::BeginChild("search", ImVec2(search_w, 0.f), ImGuiChildFlags_Borders);
    draw_search(memory);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("watches", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders);
    draw_watches(memory, add_cheat, notify);
    ImGui::EndChild();
}

// Another game: fetch its watches (the emulation thread loaded them at boot), drop the search.
// The same game's list changed elsewhere (the run timer added a watch for an auto-split): fetch
// it again.
auto MemoryPanel::sync_game(const EmuStatus& status) -> void {
    if (rom_ == status.rom_path) {
        if (revision_ == status.watches_revision) return;
        std::tie(watches_, revision_) =
            emu_.call([](EmuThread::Core& c) { return std::pair{c.emu.watches(), c.emu.watches_revision()}; });
        if (edit_row_ >= static_cast<int>(watches_.size())) edit_row_ = -1;
        return;
    }
    std::tie(watches_, revision_) = emu_.call([](EmuThread::Core& c) { return std::pair{c.emu.watches(), c.emu.watches_revision()}; });
    rom_ = status.rom_path;
    search_ = {};
    region_ = search_region_ = add_region_ = Region::Wram;
    edit_row_ = -1;
    search_error_.clear();
    watch_error_.clear();
}

auto MemoryPanel::draw_search(const MemoryImage& memory) -> void {
    ImGui::SeparatorText("RAM search");
    const auto present = [&memory](Region r) { return !memory.region(r).empty(); };
    if (!present(region_)) region_ = Region::Wram;
    bool restart = table_combo("##region", debug::kRegions, region_, kComboWidth, present);
    ImGui::SameLine();
    restart |= type_combo("##type", type_, kTypeWidth);
    ImGui::SameLine();
    // A new search for a new region or type, or a different-sized image (another game's SRAM).
    const auto bytes = memory.region(region_);
    restart |= !search_.started() || search_.memory_bytes() != bytes.size();
    if (ImGui::Button("Reset") || restart) {
        search_.reset(bytes, type_);
        search_region_ = region_;
        search_error_.clear();
    }
    ImGui::SetItemTooltip("Start over: every address is a candidate, with the values right now as the previous ones.");

    table_combo("##compare", debug::kCompares, compare_, kCompareWidth, [](auto) { return true; });
    const debug::Operand operand = debug::info(compare_).operand;
    if (operand != debug::Operand::None) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(kFieldWidth));
        const char* hint = operand == debug::Operand::Value ? "value" : "by";
        if (ImGui::InputTextWithHint("##operand", hint, operand_.data(), operand_.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            run_search(memory);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Search")) run_search(memory);
    ImGui::SetItemTooltip("Keep the candidates that pass, compared with their values at the last search.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!search_.can_undo());
    if (ImGui::Button("Undo")) search_.undo();
    ImGui::EndDisabled();

    ImGui::Text("%zu candidate%s", search_.count(), search_.count() == 1 ? "" : "s");
    if (search_.steps() > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("after %zu search%s", search_.steps(), search_.steps() == 1 ? "" : "es");
    }
    if (!search_error_.empty()) ImGui::TextColored(kErrorColor, "%s", search_error_.c_str());

    const auto now = memory.region(search_region_);
    const ValueType type = search_.type();
    if (!ImGui::BeginTable("candidates", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                           ImVec2(0.f, 0.f))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, em(kAddressWidth));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    const auto candidates = search_.candidates();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(candidates.size()));
    std::optional<uint32_t> watch;
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            const uint32_t offset = candidates[static_cast<size_t>(row)];
            const uint32_t value = debug::read_value(now, offset, type.size);
            const uint32_t before = search_.previous(offset);
            ImGui::PushID(row);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(debug::format_address(search_region_, offset).c_str());
            ImGui::TableNextColumn();
            if (value != before) {
                ImGui::TextColored(kChangedColor, "%s", debug::format_value(value, type).c_str());
            } else {
                ImGui::TextUnformatted(debug::format_value(value, type).c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", debug::format_value(before, type).c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Watch")) watch = offset;
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (watch) add_watch(search_region_, *watch, type, {});
}

auto MemoryPanel::run_search(const MemoryImage& memory) -> void {
    const debug::Operand operand = debug::info(compare_).operand;
    uint32_t value = 0;
    if (operand != debug::Operand::None) {
        // A step is a distance: typed as unsigned (or hex), whatever the display format.
        ValueType parse = search_.type();
        if (operand == debug::Operand::Step && debug::info(parse.format).is_signed) parse.format = debug::Format::Unsigned;
        const auto parsed = debug::parse_value(operand_.data(), parse);
        if (!parsed) {
            search_error_ = "Type a " + type_label(parse) + " " + (operand == debug::Operand::Value ? "value" : "step") + ".";
            return;
        }
        value = *parsed;
    }
    search_error_.clear();
    if (!search_.filter(memory.region(search_region_), compare_, value)) search_error_ = "The memory changed size: press Reset.";
}

auto MemoryPanel::add_watch(Region region, uint32_t offset, ValueType type, std::string label) -> void {
    watches_.push_back({std::move(label), region, offset, type, false, 0});
    commit();
}

auto MemoryPanel::commit() -> void {
    debug::assign_watch_ids(watches_);  // here too, so this copy and the emulator's agree
    emu_.post([list = watches_](EmuThread::Core& c) {
        if (!c.emu.set_watches(list)) utils::Logger::error(utils::LogChannel::Rom, "Could not write %s", c.emu.watches_path().c_str());
    });
}

auto MemoryPanel::draw_watches(const MemoryImage& memory, const AddCheat& add_cheat, const Notify& notify) -> void {
    ImGui::SeparatorText("Watches");
    bool changed = false;
    int remove = -1;
    const float table_h = -(ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * kHelpLines);
    if (ImGui::BeginTable("watch_list", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0.f, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, em(kTypeWidth));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, em(kValueWidth));
        ImGui::TableSetupColumn("Freeze", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < watches_.size(); i++) {
            debug::Watch& w = watches_[i];
            const int row = static_cast<int>(i);
            const auto live = live_value(memory, w);
            ImGui::PushID(row);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            std::array<char, 64> label{};
            std::strncpy(label.data(), w.label.c_str(), label.size() - 1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##label", "label", label.data(), label.size())) w.label = label.data();
            changed |= ImGui::IsItemDeactivatedAfterEdit();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(debug::format_address(w.region, w.offset).c_str());

            ImGui::TableNextColumn();
            if (type_combo("##type", w.type, -1.f)) {
                w.value &= debug::mask_of(w.type.size);
                changed = true;
            }

            // The live value; typing a new one and pressing Enter writes it once (or sets the
            // frozen value).
            ImGui::TableNextColumn();
            if (edit_row_ != row) copy_to(edit_value_, live ? debug::format_value(*live, w.type) : std::string("--"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (w.frozen) ImGui::PushStyleColor(ImGuiCol_Text, kFrozenColor);
            std::array<char, 32> field = edit_value_;
            const bool entered = ImGui::InputText("##value", field.data(), field.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            if (w.frozen) ImGui::PopStyleColor();
            if (ImGui::IsItemActive()) {
                edit_row_ = row;
                edit_value_ = field;
            } else if (edit_row_ == row) {
                edit_row_ = -1;
            }
            if (entered) {
                if (const auto value = debug::parse_value(field.data(), w.type)) {
                    emu_.post([region = w.region, offset = w.offset, size = w.type.size, v = *value](EmuThread::Core& c) {
                        c.emu.write_memory(region, offset, size, v);
                    });
                    if (w.frozen) {
                        w.value = *value;
                        changed = true;
                    }
                    watch_error_.clear();
                } else {
                    watch_error_ = "\"" + std::string(field.data()) + "\" is not a " + type_label(w.type) + " value.";
                }
                edit_row_ = -1;
            }

            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##freeze", &w.frozen)) {
                if (w.frozen) w.value = live.value_or(0);
                changed = true;
            }
            ImGui::SetItemTooltip("Write this value back every frame.");

            ImGui::TableNextColumn();
            const bool cheatable = debug::info(w.region).cheatable;
            ImGui::BeginDisabled(!cheatable || !live);
            if (ImGui::SmallButton("Cheat")) {
                const uint32_t value = w.frozen ? w.value : live.value_or(0);
                if (auto entry = debug::watch_to_cheat(w, value)) {
                    notify("Cheat added: " + entry->code + " (" + entry->description + ")");
                    add_cheat(std::move(*entry));
                    w.frozen = false;  // the cheat holds it now
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(cheatable ? "Add the value as an always-on Pro Action Replay code in Cheats."
                                            : "Cheat codes can only hold WRAM values.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) remove = row;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        watches_.erase(watches_.begin() + remove);
        edit_row_ = -1;
        changed = true;
    }
    if (changed) commit();
    draw_add_watch(memory);
}

auto MemoryPanel::draw_add_watch(const MemoryImage& memory) -> void {
    const auto present = [&memory](Region r) { return !memory.region(r).empty(); };
    if (!present(add_region_)) add_region_ = Region::Wram;
    table_combo("##add_region", debug::kRegions, add_region_, kComboWidth, present);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(kFieldWidth));
    bool add = ImGui::InputTextWithHint("##add_address", "7E0DBF", add_address_.data(), add_address_.size(),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    type_combo("##add_type", add_type_, kTypeWidth);
    ImGui::SameLine();
    const char* add_text = "Add watch";
    const float button_w = ImGui::CalcTextSize(add_text).x + ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SetNextItemWidth(std::max(em(kFieldWidth), ImGui::GetContentRegionAvail().x - button_w - ImGui::GetStyle().ItemSpacing.x));
    add |= ImGui::InputTextWithHint("##add_label", "label", add_label_.data(), add_label_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    add |= ImGui::Button(add_text);
    if (add) {
        const auto bytes = memory.region(add_region_);
        const auto offset = debug::parse_address(add_region_, add_address_.data(), bytes.size());
        if (offset && debug::fits(bytes.size(), *offset, add_type_.size)) {
            add_watch(add_region_, *offset, add_type_, add_label_.data());
            add_address_.fill(0);
            add_label_.fill(0);
            watch_error_.clear();
        } else {
            watch_error_ = "\"" + std::string(add_address_.data()) + "\" is not an address in " +
                           std::string(debug::info(add_region_).label) + ".";
        }
    }
    if (!watch_error_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", watch_error_.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Saved to watches/<rom>.wch. Type a value and press Enter to write it once; Freeze holds it "
                           "every frame until the game closes.");
        ImGui::PopStyleColor();
    }
}

} // namespace bitforge::frontend
