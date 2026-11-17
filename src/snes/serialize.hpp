// SPDX-License-Identifier: MIT
#pragma once
// Save-state archive. Every component has one `serialize(Archive&)` that lists its state once;
// the same call saves or loads depending on the archive's mode:
//
//   auto Timer::serialize(Archive& ar) -> void { ar(divider, phase, target, count, out, enabled); }
//
// Scalars and enums are stored little-endian at their own size, bools as one byte, arrays and
// C arrays element by element (bulk copies for byte-sized elements), and any type with a
// serialize(Archive&) member by calling it. Loading past the end of the data sets !ok() and
// leaves the remaining values untouched; the caller rolls back.
//
// Older save-state files stay loadable: the loader knows the file's version, and a component
// that gained a field in version N reads it only from files that have it:
//
//   ar(divider, phase);
//   if (ar.at_least(5)) ar(new_field);  // version 4 files leave the default
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace bitforge {

class Archive {
public:
    // `version`: write the layout of that older version (tests of the older-file paths); 0 = current.
    static auto saver(uint32_t version = 0) -> Archive {
        Archive ar;
        ar.version_ = version;
        return ar;
    }
    // `version`: the save-state version the data was written with; 0 means the current one.
    static auto loader(std::span<const uint8_t> data, uint32_t version = 0) -> Archive { return Archive(data, version); }

    auto loading() const -> bool { return loading_; }
    auto ok() const -> bool { return ok_; }
    auto data() const -> const std::vector<uint8_t>& { return out_; }
    auto remaining() const -> size_t { return in_.size() - pos_; }
    auto fail() -> void { ok_ = false; }  // a value read back is out of range
    auto version() const -> uint32_t { return version_; }
    // True when saving, and when loading data of `version` or newer.
    auto at_least(uint32_t version) const -> bool { return version_ == 0 || version_ >= version; }

    template <class... Ts>
    auto operator()(Ts&... values) -> Archive& {
        (io(values), ...);
        return *this;
    }

    // Raw bytes (pixel buffers and other blocks whose size the caller already knows).
    auto bytes(void* data, size_t size) -> void {
        if (!loading_) {
            const auto* p = static_cast<const uint8_t*>(data);
            out_.insert(out_.end(), p, p + size);
        } else if (ok_ && size <= remaining()) {
            std::memcpy(data, in_.data() + pos_, size);
            pos_ += size;
        } else {
            ok_ = false;
        }
    }

private:
    Archive() = default;
    Archive(std::span<const uint8_t> data, uint32_t version) : loading_(true), version_(version), in_(data) {}

    template <class T>
    auto io(T& value) -> void {
        if constexpr (std::is_same_v<T, bool>) {
            uint8_t b = value ? 1 : 0;
            bytes(&b, 1);
            if (loading_ && ok_) value = b != 0;
        } else if constexpr (std::is_enum_v<T>) {
            auto raw = static_cast<std::underlying_type_t<T>>(value);
            io(raw);
            if (loading_ && ok_) value = static_cast<T>(raw);
        } else if constexpr (std::is_arithmetic_v<T>) {
            static_assert(std::endian::native == std::endian::little, "save states assume a little-endian host");
            bytes(&value, sizeof(T));
        } else if constexpr (std::is_array_v<T>) {
            for (auto& element : value) io(element);
        } else if constexpr (requires { value.serialize(*this); }) {
            value.serialize(*this);
        } else {
            io_range(value);  // std::array
        }
    }

    template <class T, size_t N>
    auto io_range(std::array<T, N>& values) -> void {
        if constexpr (sizeof(T) == 1 && std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
            bytes(values.data(), N);
        } else {
            for (auto& element : values) io(element);
        }
    }

    bool loading_ = false;
    bool ok_ = true;
    uint32_t version_ = 0;
    std::vector<uint8_t> out_;
    std::span<const uint8_t> in_;
    size_t pos_ = 0;
};

// CRC-32 (IEEE 802.3, as zip/PNG), for identifying ROMs in save-state headers.
constexpr auto crc32(std::span<const uint8_t> data, uint32_t crc = 0) -> uint32_t {
    crc = ~crc;
    for (const uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

} // namespace bitforge
