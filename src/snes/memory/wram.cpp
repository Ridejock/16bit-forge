// SPDX-License-Identifier: MIT
#include "snes/memory/wram.hpp"

namespace bitforge::memory {

WRAM::WRAM() {
  reset();
}

auto WRAM::reset() -> void {
  data.fill(0x55);
}

auto WRAM::read(Address address) const -> Byte {
  Address addr = address & 0x1FFFF;
  if (addr < SIZE) {
    return data[addr];
  }
  return 0;
}

auto WRAM::write(Address address, Byte value) -> void {
  Address addr = address & 0x1FFFF;
  if (addr < SIZE) {
    data[addr] = value;
  }
}

} // namespace bitforge::memory