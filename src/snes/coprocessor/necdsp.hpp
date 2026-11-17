// SPDX-License-Identifier: MIT
#pragma once
// DSP-1/1B/2/3/4 (uPD77C25) and ST010/ST011 (uPD96050) cartridges: the NEC core plus the board's
// address decoding. The S-CPU talks to the DSP through two ports, DR (data) and SR (status),
// selected by one address line; the uPD96050 boards also expose the DSP's data RAM.
#include "snes/coprocessor/coprocessor.hpp"
#include "snes/coprocessor/upd77c25.hpp"

#include <optional>

namespace bitforge::coprocessor {

// One window of the S-CPU address space: banks lo-hi (and their $80 mirrors), offsets lo-hi.
struct NecWindow {
  Byte bank_lo, bank_hi;
  Word addr_lo, addr_hi;
  Word select;  // DR/SR window: the address bit that selects SR. RAM window: byte-offset mask.

  constexpr auto contains(Address address) const -> bool {
    const Byte bank = static_cast<Byte>((address >> 16) & 0x7F);
    const Word offset = static_cast<Word>(address);
    return bank >= bank_lo && bank <= bank_hi && offset >= addr_lo && offset <= addr_hi;
  }
};

struct NecBoard {
  const char* name;
  NecWindow io;
  std::optional<NecWindow> ram;  // uPD96050 data RAM (battery-backed on ST010/ST011 boards)
};

class NecDsp final : public Coprocessor {
public:
  NecDsp(Chip chip, const NecVariant& variant, const NecBoard& board);

  auto core() -> UPD77C25& { return dsp_; }
  auto board() const -> const NecBoard& { return board_; }

  auto chip() const -> Chip override { return chip_; }
  auto reset() -> void override;
  auto read(Address address) -> std::optional<Byte> override;
  auto write(Address address, Byte value) -> bool override;
  auto step(uint32_t master_clocks) -> void override;
  auto serialize(Archive& ar) -> void override;
  auto battery_ram() -> std::span<Byte> override;

private:
  Chip chip_;
  UPD77C25 dsp_;
  NecBoard board_;
  int64_t clock_budget_ = 0;  // in units of 1/(clock_hz * 11) master clocks
};

} // namespace bitforge::coprocessor
