// SPDX-License-Identifier: MIT
#pragma once
// The S-SMP's 64-byte boot ROM (IPL), mapped at $FFC0-$FFFF while CONTROL ($F1) bit 7 is set.
// Every SNES emulator carries this table: it is the only code in the APU at power-on. It clears
// zero page, posts $AA/$BB on ports 0/1, then runs the S-CPU upload protocol: wait for $CC on
// port 0, take a destination address on ports 2/3, receive bytes through port 1 acknowledged on
// port 0, and jump to the address from ports 2/3 when a block with a zero entry ends the upload.
// The 64 bytes themselves are a dump of the chip's mask ROM (Sony), kept for compatibility; they
// are hardware data, not covered by this project's MIT license.
#include <array>
#include <cstdint>

namespace snes::apu {

inline constexpr std::array<uint8_t, 64> kIplRom = {
    0xCD, 0xEF,        // $FFC0  mov x,#$EF
    0xBD,              // $FFC2  mov sp,x
    0xE8, 0x00,        // $FFC3  mov a,#$00
    0xC6,              // $FFC5  mov (x),a
    0x1D,              // $FFC6  dec x
    0xD0, 0xFC,        // $FFC7  bne $FFC5
    0x8F, 0xAA, 0xF4,  // $FFC9  mov $F4,#$AA
    0x8F, 0xBB, 0xF5,  // $FFCC  mov $F5,#$BB
    0x78, 0xCC, 0xF4,  // $FFCF  cmp $F4,#$CC
    0xD0, 0xFB,        // $FFD2  bne $FFCF
    0x2F, 0x19,        // $FFD4  bra $FFEF
    0xEB, 0xF4,        // $FFD6  mov y,$F4
    0xD0, 0xFC,        // $FFD8  bne $FFD6
    0x7E, 0xF4,        // $FFDA  cmp y,$F4
    0xD0, 0x0B,        // $FFDC  bne $FFE9
    0xE4, 0xF5,        // $FFDE  mov a,$F5
    0xCB, 0xF4,        // $FFE0  mov $F4,y
    0xD7, 0x00,        // $FFE2  mov ($00)+y,a
    0xFC,              // $FFE4  inc y
    0xD0, 0xF3,        // $FFE5  bne $FFDA
    0xAB, 0x01,        // $FFE7  inc $01
    0x10, 0xEF,        // $FFE9  bpl $FFDA
    0x7E, 0xF4,        // $FFEB  cmp y,$F4
    0x10, 0xEB,        // $FFED  bpl $FFDA
    0xBA, 0xF6,        // $FFEF  movw ya,$F6
    0xDA, 0x00,        // $FFF1  movw $00,ya
    0xBA, 0xF4,        // $FFF3  movw ya,$F4
    0xC4, 0xF4,        // $FFF5  mov $F4,a
    0xDD,              // $FFF7  mov a,y
    0x5D,              // $FFF8  mov x,a
    0xD0, 0xDB,        // $FFF9  bne $FFD6
    0x1F, 0x00, 0x00,  // $FFFB  jmp ($0000+x)
    0xC0, 0xFF,        // $FFFE  reset vector: $FFC0
};

} // namespace snes::apu
