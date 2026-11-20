// SPDX-License-Identifier: MIT
#include "snes/ramwatch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

namespace bitforge {

namespace {

auto trim(std::string_view text) -> std::string_view {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

auto digit(char c, int base) -> int {
    int value = -1;
    if (c >= '0' && c <= '9') value = c - '0';
    else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
    return value >= 0 && value < base ? value : -1;
}

auto parse_u32(std::string_view text, int base) -> std::optional<uint32_t> {
    if (text.empty()) return std::nullopt;
    uint32_t value = 0;
    for (const char c : text) {
        const int d = digit(c, base);
        if (d < 0) return std::nullopt;
        if (value > (0xFFFFFFFFu - static_cast<uint32_t>(d)) / static_cast<uint32_t>(base)) return std::nullopt;
        value = value * static_cast<uint32_t>(base) + static_cast<uint32_t>(d);
    }
    return value;
}

// A bare token, or a quoted string with \" and \\ escapes. nullopt when the quotes do not close.
auto take_token(std::string_view& line) -> std::optional<std::string> {
    line = trim(line);
    if (line.empty()) return std::nullopt;
    if (line.front() == '"') {
        line.remove_prefix(1);
        std::string out;
        while (!line.empty() && line.front() != '"') {
            if (line.front() == '\\' && line.size() >= 2) {
                out.push_back(line[1]);
                line.remove_prefix(2);
                continue;
            }
            out.push_back(line.front());
            line.remove_prefix(1);
        }
        if (line.empty() || line.front() != '"') return std::nullopt;
        line.remove_prefix(1);
        return out;
    }
    std::size_t end = 0;
    while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
    std::string out(line.substr(0, end));
    line.remove_prefix(end);
    return out;
}

auto quote_label(std::string_view label) -> std::string {
    std::string out = "\"";
    for (const char c : label) {
        if (c == '\n' || c == '\r') continue;
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

auto kept(uint8_t current, uint8_t previous, SearchCompare compare, std::optional<uint8_t> value) -> bool {
    switch (compare) {
    case SearchCompare::Equal: return current == *value;
    case SearchCompare::NotEqual: return current != *value;
    case SearchCompare::Less: return current < *value;
    case SearchCompare::Greater: return current > *value;
    case SearchCompare::Changed: return current != previous;
    case SearchCompare::Unchanged: return current == previous;
    case SearchCompare::Increased: return current > previous;
    case SearchCompare::Decreased: return current < previous;
    }
    return false;
}

}  // namespace

auto wram_bus_address(uint32_t offset) -> Address { return 0x7E0000u + (offset & 0x1FFFFu); }

auto format_address(WatchDomain domain, uint32_t offset) -> std::string {
    char text[32];
    if (domain == WatchDomain::Wram) {
        std::snprintf(text, sizeof(text), "$%06X", wram_bus_address(offset));
    } else {
        std::snprintf(text, sizeof(text), "SRAM:$%X", offset);
    }
    return text;
}

auto RamWatch::start_search(WatchDomain domain, std::span<const uint8_t> bytes) -> void {
    search_domain_ = domain;
    snapshot_.assign(bytes.begin(), bytes.end());
    candidates_.resize(bytes.size());
    for (uint32_t i = 0; i < candidates_.size(); ++i) candidates_[i] = i;
    searching_ = true;
}

auto RamWatch::narrow(std::span<const uint8_t> bytes, SearchCompare compare, std::optional<uint8_t> value) -> bool {
    if (!searching_ || bytes.size() != snapshot_.size()) return false;
    if (compare_needs_value(compare) && !value) return false;
    std::vector<uint32_t> next;
    next.reserve(candidates_.size());
    for (const uint32_t offset : candidates_) {
        if (offset >= bytes.size()) continue;
        if (kept(bytes[offset], snapshot_[offset], compare, value)) next.push_back(offset);
    }
    candidates_ = std::move(next);
    snapshot_.assign(bytes.begin(), bytes.end());
    return true;
}

auto RamWatch::first_candidates(uint32_t limit) const -> std::vector<uint32_t> {
    const uint32_t count = std::min(limit, candidate_count());
    return {candidates_.begin(), candidates_.begin() + count};
}

auto RamWatch::add_watch(WatchDomain domain, uint32_t offset, uint8_t size, std::string label) -> uint32_t {
    if (size != 2 && size != 4) size = 1;
    if (next_id_ == 0) return 0;
    if (domain == WatchDomain::Wram && (offset >= kWramBytes || static_cast<uint64_t>(offset) + size > kWramBytes)) return 0;
    Watch watch;
    watch.id = next_id_++;
    watch.domain = domain;
    watch.offset = offset;
    watch.size = size;
    watch.label = std::move(label);
    watches_.push_back(std::move(watch));
    return watches_.back().id;
}

auto RamWatch::remove_watch(uint32_t id) -> bool {
    const auto it = std::find_if(watches_.begin(), watches_.end(), [id](const Watch& watch) { return watch.id == id; });
    if (it == watches_.end()) return false;
    watches_.erase(it);
    return true;
}

auto RamWatch::find(uint32_t id) -> Watch* {
    for (Watch& watch : watches_) {
        if (watch.id == id) return &watch;
    }
    return nullptr;
}

auto RamWatch::find(uint32_t id) const -> const Watch* {
    for (const Watch& watch : watches_) {
        if (watch.id == id) return &watch;
    }
    return nullptr;
}

auto RamWatch::set_label(uint32_t id, std::string label) -> bool {
    Watch* watch = find(id);
    if (!watch) return false;
    label.erase(std::remove(label.begin(), label.end(), '\n'), label.end());
    label.erase(std::remove(label.begin(), label.end(), '\r'), label.end());
    watch->label = std::move(label);
    return true;
}

auto RamWatch::set_size(uint32_t id, uint8_t size) -> bool {
    Watch* watch = find(id);
    if (!watch || (size != 1 && size != 2 && size != 4)) return false;
    if (watch->domain == WatchDomain::Wram && static_cast<uint64_t>(watch->offset) + size > kWramBytes) return false;
    if (watch->size == size) return true;
    watch->size = size;
    watch->sampled = false;
    watch->condition_met = false;
    return true;
}

auto RamWatch::set_condition(uint32_t id, WatchCondition condition) -> bool {
    Watch* watch = find(id);
    if (!watch) return false;
    watch->condition = condition;
    watch->condition_met = false;
    return true;
}

auto RamWatch::set_frozen(uint32_t id, bool frozen, uint32_t value) -> bool {
    Watch* watch = find(id);
    if (!watch) return false;
    watch->frozen = frozen;
    watch->freeze_value = value;
    return true;
}

auto RamWatch::forget_samples() -> void {
    for (Watch& watch : watches_) {
        watch.sampled = false;
        watch.condition_met = false;
    }
}

auto RamWatch::freeze_bytes() const -> std::vector<FreezeByte> {
    std::vector<FreezeByte> out;
    for (const Watch& watch : watches_) {
        if (!watch.frozen) continue;
        for (uint8_t i = 0; i < watch.size; ++i) {
            out.push_back(FreezeByte{watch.domain, watch.offset + i, static_cast<uint8_t>((watch.freeze_value >> (8 * i)) & 0xFF)});
        }
    }
    return out;
}

auto RamWatch::cheat_code(uint32_t id) const -> std::optional<std::string> {
    const Watch* watch = find(id);
    if (!watch || watch->domain != WatchDomain::Wram) return std::nullopt;
    const uint32_t value = watch->frozen ? watch->freeze_value : watch->value;
    std::string code;
    for (uint8_t i = 0; i < watch->size; ++i) {
        if (i != 0) code.push_back('+');
        char part[16];
        std::snprintf(part, sizeof(part), "%06X%02X", wram_bus_address(watch->offset + i),
                      static_cast<unsigned>((value >> (8 * i)) & 0xFF));
        code += part;
    }
    return code;
}

auto RamWatch::format() const -> std::string {
    std::string out = "# 16bit Forge memory watches. id domain offset size \"label\" [from to]\n"
                      "# Freezes are not saved. `next` is the next id; ids are never reused.\n";
    char line[64];
    std::snprintf(line, sizeof(line), "next %u\n", next_id_);
    out += line;
    for (const Watch& watch : watches_) {
        std::snprintf(line, sizeof(line), "%u %s %X %u ", watch.id, watch.domain == WatchDomain::Wram ? "wram" : "sram",
                      watch.offset, static_cast<unsigned>(watch.size));
        out += line;
        out += quote_label(watch.label);
        if (watch.condition.enabled) {
            std::snprintf(line, sizeof(line), " %u %u", watch.condition.from, watch.condition.to);
            out += line;
        }
        out.push_back('\n');
    }
    return out;
}

auto RamWatch::parse(std::string_view text) -> void {
    std::vector<Watch> loaded;
    uint32_t declared_next = 1;
    uint32_t max_id = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view raw = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') continue;

        const std::optional<std::string> first = take_token(line);
        if (!first) continue;
        if (*first == "next") {
            const std::optional<std::string> number = take_token(line);
            if (!number) continue;
            if (const std::optional<uint32_t> next = parse_u32(*number, 10)) declared_next = std::max(declared_next, *next);
            continue;
        }
        const std::optional<uint32_t> id = parse_u32(*first, 10);
        const std::optional<std::string> domain_token = take_token(line);
        const std::optional<std::string> offset_token = take_token(line);
        const std::optional<std::string> size_token = take_token(line);
        const std::optional<std::string> label = take_token(line);
        if (!id || *id == 0 || !domain_token || !offset_token || !size_token || !label) continue;
        std::string domain_name = *domain_token;
        for (char& c : domain_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (domain_name != "wram" && domain_name != "sram") continue;
        const std::optional<uint32_t> offset = parse_u32(*offset_token, 16);
        const std::optional<uint32_t> size = parse_u32(*size_token, 10);
        if (!offset || !size || (*size != 1 && *size != 2 && *size != 4)) continue;
        const WatchDomain domain = domain_name == "wram" ? WatchDomain::Wram : WatchDomain::Sram;
        if (domain == WatchDomain::Wram && (*offset >= kWramBytes || static_cast<uint64_t>(*offset) + *size > kWramBytes)) continue;
        if (std::any_of(loaded.begin(), loaded.end(), [id](const Watch& watch) { return watch.id == *id; })) continue;

        Watch watch;
        watch.id = *id;
        watch.domain = domain;
        watch.offset = *offset;
        watch.size = static_cast<uint8_t>(*size);
        watch.label = *label;
        max_id = std::max(max_id, *id);
        if (const std::optional<std::string> from_token = take_token(line)) {
            if (const std::optional<std::string> to_token = take_token(line)) {
                const std::optional<uint32_t> from = parse_u32(*from_token, 10);
                const std::optional<uint32_t> to = parse_u32(*to_token, 10);
                if (from && to) watch.condition = WatchCondition{true, *from, *to};
            }
        }
        loaded.push_back(std::move(watch));
    }
    watches_ = std::move(loaded);
    const uint32_t after_ids = max_id == 0xFFFFFFFFu ? max_id : max_id + 1;
    next_id_ = std::max(declared_next, after_ids);
    if (next_id_ == 0) next_id_ = 1;
}

}  // namespace bitforge
