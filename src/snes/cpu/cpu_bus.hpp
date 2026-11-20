// SPDX-License-Identifier: MIT
#pragma once
#include "snes/types.hpp"

namespace bitforge::cpu {

// What a 65C816 sees of the machine around it. The S-CPU runs on memory::Bus; the SA-1's second
// 65C816 runs on the SA-1's own bus. Every access goes through read()/write(), so each bus does
// its own timing and mapping.
class CpuBus {
public:
  virtual auto read(Address address) -> Byte = 0;
  virtual auto write(Address address, Byte value) -> void = 0;
  // Edge-latched NMI: true once, when the CPU should take it.
  virtual auto poll_nmi() -> bool = 0;
  // Level-sensitive IRQ line.
  virtual auto irq_pending() const -> bool = 0;
  // WAI is ending on an interrupt. Returns the CPU cycles that takes; a bus that times the exit
  // itself (the S-CPU's, from the moment the line went low) charges it and returns 0.
  virtual auto end_wait() -> int { return 0; }
  // Interrupt and reset vectors (bank 0). The SA-1 replaces them with its CRV/CNV/CIV
  // registers; everyone else reads the two bytes from memory.
  virtual auto read_vector(Word address) -> Word {
    const Byte low = read(address);
    return static_cast<Word>(low | (read(static_cast<Word>(address + 1)) << 8));
  }

protected:
  ~CpuBus() = default;
};

} // namespace bitforge::cpu
