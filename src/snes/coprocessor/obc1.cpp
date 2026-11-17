// SPDX-License-Identifier: MIT
#include "snes/coprocessor/obc1.hpp"

namespace bitforge::coprocessor {

// The latches mirror the SRAM bytes behind $7FF5/$7FF6, so a reset (or a loaded save) picks
// them back up.
auto OBC1::reset() -> void {
  select_table(at(0x1FF5));
  select_object(at(0x1FF6));
}

auto OBC1::read(Address address) -> std::optional<Byte> {
  if (!decodes(address)) return std::nullopt;
  const unsigned offset = address & 0x1FFF;
  switch (offset) {
    case 0x1FF0: case 0x1FF1: case 0x1FF2: case 0x1FF3: return entry_byte(offset - 0x1FF0);
    case 0x1FF4: return extra_bits();  // the whole byte, not just the object's field
    default: return at(offset);
  }
}

auto OBC1::write(Address address, Byte value) -> bool {
  if (!decodes(address)) return false;
  const unsigned offset = address & 0x1FFF;
  switch (offset) {
    case 0x1FF0: case 0x1FF1: case 0x1FF2: case 0x1FF3:
      entry_byte(offset - 0x1FF0) = value;
      break;
    case 0x1FF4: {
      const Byte field = static_cast<Byte>(3u << bit_pos_);
      Byte& bits = extra_bits();
      bits = static_cast<Byte>((bits & ~field) | ((value << bit_pos_) & field));
      break;
    }
    case 0x1FF5: select_table(value); at(offset) = value; break;
    case 0x1FF6: select_object(value); at(offset) = value; break;
    default: at(offset) = value; break;
  }
  return true;
}

} // namespace bitforge::coprocessor
