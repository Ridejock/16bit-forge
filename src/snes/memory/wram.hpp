// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include "snes/serialize.hpp"
#include "snes/types.hpp"

namespace snes::memory {

class WRAM {
public:
  WRAM();
  
  auto read(Address address) const -> Byte;
  auto write(Address address, Byte value) -> void;
  auto reset() -> void;
  auto serialize(Archive& ar) -> void { ar(data); }

private:
  static constexpr std::size_t SIZE = 128 * 1024;
  std::array<Byte, SIZE> data;
};

} // namespace snes::memory