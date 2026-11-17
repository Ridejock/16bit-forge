// SPDX-License-Identifier: MIT
#pragma once
// OBC1 (Metal Combat: Falcon's Revenge): an OAM helper in front of the cartridge's 8 KB SRAM at
// $6000-$7FFF. Port layout from fullsnes, "SNES Cart OBC1 (OBJ Controller)":
//   $7FF0-$7FF3  byte n of the selected object's 4-byte entry: [base + object*4 + n]
//   $7FF4        the object's 2 extra bits in [base + $200 + object/4], bits (object & 3) * 2
//   $7FF5        bit 0 selects the table base ($7C00 or $7800)
//   $7FF6        object number (0-127)
// Everything else, the port bytes included, is plain SRAM.
#include "snes/coprocessor/coprocessor.hpp"

#include <vector>

namespace bitforge::coprocessor {

class OBC1 final : public Coprocessor {
public:
  explicit OBC1(std::vector<Byte>& sram) : ram_(sram) { reset(); }

  auto chip() const -> Chip override { return Chip::OBC1; }
  auto reset() -> void override;
  auto read(Address address) -> std::optional<Byte> override;
  auto write(Address address, Byte value) -> bool override;
  auto step(uint32_t) -> void override {}
  auto serialize(Archive& ar) -> void override { ar(table_, object_, bit_pos_); }

private:
  std::vector<Byte>& ram_;
  Word table_ = 0x1C00;  // SRAM offset of the object table
  Byte object_ = 0;
  Byte bit_pos_ = 0;     // position of the object's 2-bit field in its $7FF4 byte

  auto select_table(Byte value) -> void { table_ = (value & 0x01) ? 0x1800 : 0x1C00; }
  auto select_object(Byte value) -> void {
    object_ = value & 0x7F;
    bit_pos_ = static_cast<Byte>(2 * (object_ % 4));
  }
  auto entry_byte(unsigned n) -> Byte& { return at(table_ + 4u * object_ + n); }
  auto extra_bits() -> Byte& { return at(table_ + 0x200u + object_ / 4u); }

  static auto decodes(Address address) -> bool {
    const Word offset = static_cast<Word>(address);
    return ((address >> 16) & 0x40) == 0 && offset >= 0x6000 && offset < 0x8000;
  }
  auto at(uint32_t offset) -> Byte& {
    static Byte dummy = 0;
    if (ram_.empty()) return dummy = 0;
    return ram_[(offset & 0x1FFF) % ram_.size()];
  }
};

} // namespace bitforge::coprocessor
