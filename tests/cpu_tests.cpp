// SPDX-License-Identifier: MIT
#include "snes/controllers.hpp"
#include "snes/cpu/cpu65c816.hpp"
#include "snes/memory/bus.hpp"
#include "snes/ppu/ppu.hpp"
#include "utils/logging.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

class TestHarness {
public:
    bitforge::memory::Bus bus;
    bitforge::cpu::CPU65c816 cpu;
    
    TestHarness() : cpu(bus) {
        bus.connect_cpu(&cpu);
    }
    
    // Load test code into ROM and set reset vector
    void load_test_rom(bitforge::Address rom_base, const std::vector<bitforge::Byte>& code, bitforge::Address reset_addr) {
        std::vector<bitforge::Byte> rom(32768, 0x00);  // 32KB ROM
        
        // Copy code into ROM at the correct offset
        bitforge::Address offset = rom_base & 0x7FFF;
        for (bitforge::Address i = 0; i < static_cast<bitforge::Address>(code.size()); i++) {
            rom[offset + i] = code[i];
        }
        
        // Set reset vector at ROM offset $7FFC (maps to $00FFFC)
        rom[0x7FFC] = reset_addr & 0xFF;
        rom[0x7FFD] = (reset_addr >> 8) & 0xFF;
        
        bus.load_test_rom(rom);
        cpu.reset();
    }
    
    void run_cycles(int count) {
        for (int i = 0; i < count; i++) {
            cpu.step();
        }
    }
};

static int test_lda_immediate();
static int test_sta_absolute();
static int test_adc();
static int test_sbc();
static int test_and_ora_eor();
static int test_inc_dec();
static int test_transfers();
static int test_branches();
static int test_stack();
static int test_cmp();
static int test_flags();
static int test_ldx_ldy();
static int test_stx_sty();
static int test_dex_dey_inx_iny();
static int test_rol_ror();
static int test_asl_lsr();
static int test_ppu_register_bus();
static int test_cpu_io_math();
static int test_nmi_taken();
static int test_dma_waits_for_fetch();
static int test_nmi_disabled();
static int test_vblank_ends();
static int test_vram_port();
static int test_bg_sampler_rows();
static int test_log_ring();
static int test_log_channels();
static int test_mixed_widths();
static int test_direct_page_register();
static int test_direct_page_emulation_wrap();
static int test_pbr_jumps();
static int test_bank_wrap();
static int test_block_move();
static int test_decimal_mode();
static int test_emulation_mode_rules();
static int test_interrupt_vectors();
static int test_nmi_vectors_and_b_flag();
static int test_65816_opcodes();
static int test_cycle_table();
static int test_wram_mirror();
static int test_rom_mapping();
static int test_sram_mapping();
static int test_header_detection();
static int test_cgram_full_range();
static int test_w12sel_keeps_tm();
static int test_dram_refresh();
static int test_controller_devices();
static int test_controller_bus();

int main() {
    printf("=== SNES CPU Tests ===\n\n");
    
    int passed = 0;
    int failed = 0;
    
    auto run_test = [&](const char* name, int (*test_fn)()) {
        printf("  %s... ", name);
        if (test_fn() == 0) {
            printf("PASSED\n");
            passed++;
        } else {
            printf("FAILED\n");
            failed++;
        }
    };
    
    run_test("LDA Immediate", test_lda_immediate);
    run_test("STA Absolute", test_sta_absolute);
    run_test("ADC", test_adc);
    run_test("SBC", test_sbc);
    run_test("AND/ORA/EOR", test_and_ora_eor);
    run_test("INC/DEC", test_inc_dec);
    run_test("Transfers (TAX/TXA/TAY/TYA)", test_transfers);
    run_test("Branches (BEQ/BNE/BCS/BCC)", test_branches);
    run_test("Stack (PHA/PLA)", test_stack);
    run_test("CMP", test_cmp);
    run_test("Flags (SEC/CLC/SEI/CLI)", test_flags);
    run_test("LDX/LDY", test_ldx_ldy);
    run_test("STX/STY", test_stx_sty);
    run_test("DEX/DEY/INX/INY", test_dex_dey_inx_iny);
    run_test("ROL/ROR", test_rol_ror);
    run_test("ASL/LSR", test_asl_lsr);
    run_test("PPU register bus", test_ppu_register_bus);
    run_test("CPU I/O multiply/divide", test_cpu_io_math);
    run_test("NMI taken", test_nmi_taken);
    run_test("NMI disabled", test_nmi_disabled);
    run_test("GP-DMA after the next opcode fetch", test_dma_waits_for_fetch);
    run_test("Vblank ends", test_vblank_ends);
    run_test("VRAM port", test_vram_port);
    run_test("BG sampler rows", test_bg_sampler_rows);
    run_test("Log ring", test_log_ring);
    run_test("Log channels, sinks, trace gating, file cap", test_log_channels);
    run_test("Mixed M/X widths", test_mixed_widths);
    run_test("Direct page register", test_direct_page_register);
    run_test("Direct page emulation wrap", test_direct_page_emulation_wrap);
    run_test("PBR jumps (JMP/JSR/JML/JSL)", test_pbr_jumps);
    run_test("Bank wrap", test_bank_wrap);
    run_test("MVN/MVP", test_block_move);
    run_test("Decimal mode", test_decimal_mode);
    run_test("Emulation mode rules", test_emulation_mode_rules);
    run_test("BRK/COP vectors", test_interrupt_vectors);
    run_test("NMI vectors and B flag", test_nmi_vectors_and_b_flag);
    run_test("65816 opcodes (XBA/PEA/PER/PEI/BRL/WDM/sr,S/long)", test_65816_opcodes);
    run_test("Cycle table", test_cycle_table);
    run_test("WRAM mirror", test_wram_mirror);
    run_test("LoROM/HiROM mapping", test_rom_mapping);
    run_test("SRAM mapping", test_sram_mapping);
    run_test("Header detection", test_header_detection);
    run_test("CGRAM full range", test_cgram_full_range);
    run_test("W12SEL keeps TM", test_w12sel_keeps_tm);
    run_test("DRAM refresh stall", test_dram_refresh);
    run_test("Controller serial protocols (pad, multitap, mouse, scope)", test_controller_devices);
    run_test("Controllers on the bus (auto-read, $4016/7, $4201, light gun latch)", test_controller_bus);
    
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

// STA $420B inside an instruction: the CPU still fetches the next opcode, then the transfer runs
// (Anomie). One byte from ROM to the WRAM port, then NOP: 24 clocks of transfer, 2-8 to align to
// 8 before it and 2-6 to resume on the NOP's 6-clock internal cycle after it.
static int test_dma_waits_for_fetch() {
    TestHarness h;
    std::vector<bitforge::Byte> code(0x101, 0xEA);
    const std::vector<bitforge::Byte> start = {0xA9, 0x01, 0x8D, 0x0B, 0x42};  // LDA #$01, STA $420B
    std::copy(start.begin(), start.end(), code.begin());
    code[0x100] = 0x5A;
    h.load_test_rom(0x8000, code, 0x8000);
    h.bus.poke_wram(0x2000, 0x00);
    h.bus.write(0x002181, 0x00);
    h.bus.write(0x002182, 0x20);
    h.bus.write(0x002183, 0x00);
    h.bus.write(0x004300, 0x00);
    h.bus.write(0x004301, 0x80);  // $2180
    h.bus.write(0x004302, 0x00);
    h.bus.write(0x004303, 0x81);
    h.bus.write(0x004304, 0x00);
    h.bus.write(0x004305, 0x01);
    h.bus.write(0x004306, 0x00);
    auto step = [&] {
        h.bus.begin_cpu_step();
        const int cycles = h.cpu.step();
        return h.bus.end_cpu_step(cycles);
    };
    step();
    step();
    if (h.bus.peek_wram(0x2000) != 0x00) {
        printf("DMA ran before the next opcode fetch");
        return 1;
    }
    const uint64_t nop = step();
    if (h.bus.peek_wram(0x2000) != 0x5A) {
        printf("DMA didn't run after the fetch");
        return 1;
    }
    if (nop < 14 + 24 + 4 || nop > 14 + 24 + 14) {
        printf("NOP with the DMA: %llu clocks", static_cast<unsigned long long>(nop));
        return 1;
    }
    return 0;
}

// Test: LDA #$42 -> Store to $7E0200 -> Expect 0x42
static int test_lda_immediate() {
    TestHarness h;
    
    std::vector<bitforge::Byte> code = {
        0xA9, 0x42,       // LDA #$42
        0x8D, 0x00, 0x02  // STA $7E0200
    };
    h.load_test_rom(0x8000, code, 0x8000);
    h.run_cycles(2);
    
    bitforge::Byte result = h.bus.read(0x7E0200);
    if (result != 0x42) {
        printf("Expected 0x42, got 0x%02X", result);
        return 1;
    }
    return 0;
}

// Test: LDA #$55 -> STA $7E0200 -> Expect 0x55
static int test_sta_absolute() {
    TestHarness h;
    
    std::vector<bitforge::Byte> code = {
        0xA9, 0x55,       // LDA #$55
        0x8D, 0x00, 0x02  // STA $7E0200
    };
    h.load_test_rom(0x8000, code, 0x8000);
    h.run_cycles(2);
    
    bitforge::Byte result = h.bus.read(0x7E0200);
    if (result != 0x55) {
        printf("Expected 0x55, got 0x%02X", result);
        return 1;
    }
    return 0;
}

// Test: LDA #$10, CLC, ADC #$20 -> Expect 0x30
static int test_adc() {
    TestHarness h;
    
    std::vector<bitforge::Byte> code = {
        0xA9, 0x10,       // LDA #$10
        0x18,             // CLC
        0x69, 0x20,       // ADC #$20
        0x8D, 0x00, 0x02  // STA $7E0200
    };
    h.load_test_rom(0x8000, code, 0x8000);
    h.run_cycles(4);
    
    bitforge::Byte result = h.bus.read(0x7E0200);
    if (result != 0x30) {
        printf("Expected 0x30, got 0x%02X", result);
        return 1;
    }
    return 0;
}

// Test: LDA #$50, SEC, SBC #$20 -> Expect 0x30
static int test_sbc() {
    TestHarness h;
    
    std::vector<bitforge::Byte> code = {
        0xA9, 0x50,       // LDA #$50
        0x38,             // SEC
        0xE9, 0x20,       // SBC #$20
        0x8D, 0x00, 0x02  // STA $7E0200
    };
    h.load_test_rom(0x8000, code, 0x8000);
    h.run_cycles(4);
    
    bitforge::Byte result = h.bus.read(0x7E0200);
    if (result != 0x30) {
        printf("Expected 0x30, got 0x%02X", result);
        return 1;
    }
    return 0;
}

// Test: LDA #$FF, AND #$0F -> Expect 0x0F
static int test_and_ora_eor() {
    TestHarness h;
    
    // Test AND
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0xFF,       // LDA #$FF
            0x29, 0x0F,       // AND #$0F
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x0F) {
            printf("AND: Expected 0x0F, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test ORA
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x0F,       // LDA #$0F
            0x09, 0xF0,       // ORA #$F0
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0xFF) {
            printf("ORA: Expected 0xFF, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test EOR
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0xFF,       // LDA #$FF
            0x49, 0x0F,       // EOR #$0F
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0xF0) {
            printf("EOR: Expected 0xF0, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: INC $7E0200 (initial 0x05) -> Expect 0x06
static int test_inc_dec() {
    TestHarness h;
    
    // Test INC
    {
        h.bus.write(0x7E0200, 0x05);
        
        std::vector<bitforge::Byte> code = {
            0xEE, 0x00, 0x02,  // INC $7E0200
            0xEA              // NOP (padding, not strictly needed)
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(1);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x06) {
            printf("INC: Expected 0x06, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test DEC
    {
        h.bus.write(0x7E0200, 0x10);
        
        std::vector<bitforge::Byte> code = {
            0xCE, 0x00, 0x02,  // DEC $7E0200
            0xEA              // NOP
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(1);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x0F) {
            printf("DEC: Expected 0x0F, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDA #$42, TAX, TXA -> Expect 0x42
static int test_transfers() {
    TestHarness h;
    
    // Test TAX/TXA
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x42,       // LDA #$42
            0xAA,             // TAX (A -> X)
            0xA9, 0x00,       // LDA #$00 (clear A)
            0x8A,             // TXA (X -> A)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(5);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x42) {
            printf("TAX/TXA: Expected 0x42, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test TAY/TYA
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x77,       // LDA #$77
            0xA8,             // TAY (A -> Y)
            0xA9, 0x00,       // LDA #$00 (clear A)
            0x98,             // TYA (Y -> A)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(5);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x77) {
            printf("TAY/TYA: Expected 0x77, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDA #$00, BEQ +2 (skip LDA #$FF) -> Expect 0x00
static int test_branches() {
    TestHarness h;
    
    // Test BEQ (branch if equal / Z=1)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x00,       // LDA #$00 (sets Z=1)
            0xF0, 0x02,       // BEQ +2 (skip next 2 bytes)
            0xA9, 0xFF,       // LDA #$FF (should be skipped)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x00) {
            printf("BEQ: Expected 0x00, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test BNE (branch if not equal / Z=0)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x01,       // LDA #$01 (sets Z=0)
            0xD0, 0x02,       // BNE +2 (skip next 2 bytes)
            0xA9, 0xFF,       // LDA #$FF (should be skipped)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x01) {
            printf("BNE: Expected 0x01, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test BCS (branch if carry set)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0xAA,       // LDA #$AA
            0x38,             // SEC (set carry)
            0xB0, 0x02,       // BCS +2 (skip next 2 bytes)
            0xA9, 0xFF,       // LDA #$FF (should be skipped)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(4);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0xAA) {
            printf("BCS: Expected 0xAA, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test BCC (branch if carry clear)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0xBB,       // LDA #$BB
            0x18,             // CLC (clear carry)
            0x90, 0x02,       // BCC +2 (skip next 2 bytes)
            0xA9, 0xFF,       // LDA #$FF (should be skipped)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(4);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0xBB) {
            printf("BCC: Expected 0xBB, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDA #$42, PHA, LDA #$00, PLA -> Expect 0x42
static int test_stack() {
    TestHarness h;
    
    // Test PHA/PLA
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x42,       // LDA #$42
            0x48,             // PHA (push A)
            0xA9, 0x00,       // LDA #$00
            0x68,             // PLA (pull A)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(5);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x42) {
            printf("PHA/PLA: Expected 0x42, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDA #$10, CMP #$10 -> Z=1, BEQ +2 -> Expect 0x10
static int test_cmp() {
    TestHarness h;
    
    // Test CMP equal
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x10,       // LDA #$10
            0xC9, 0x10,       // CMP #$10 (should set Z=1)
            0xF0, 0x02,       // BEQ +2 (skip next 2 bytes)
            0xA9, 0xFF,       // LDA #$FF (should be skipped)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(4);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x10) {
            printf("CMP equal: Expected 0x10, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test CMP less than (sets C=1 if A >= value)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x20,       // LDA #$20
            0xC9, 0x10,       // CMP #$10 (A > value, C=1)
            0xB0, 0x02,       // BCS +2 (skip next 2 bytes)
            0xA9, 0xFF,       // LDA #$FF (should be skipped)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(4);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x20) {
            printf("CMP greater: Expected 0x20, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: SEC, LDA #$00, BCS +2 -> Expect 0x00 (carry set, branch taken)
static int test_flags() {
    TestHarness h;
    
    // Test SEC/BCS
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0xAA,       // LDA #$AA
            0x38,             // SEC
            0xB0, 0x02,       // BCS +2 (should branch)
            0xA9, 0xFF,       // LDA #$FF (should be skipped)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(4);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0xAA) {
            printf("SEC/BCS: Expected 0xAA, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test CLC/BCC
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0xBB,       // LDA #$BB
            0x18,             // CLC
            0x90, 0x02,       // BCC +2 (should branch)
            0xA9, 0xFF,       // LDA #$FF (should be skipped)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(4);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0xBB) {
            printf("CLC/BCC: Expected 0xBB, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDX #$42, STX $7E0200 -> Expect 0x42
static int test_ldx_ldy() {
    TestHarness h;
    
    // Test LDX
    {
        std::vector<bitforge::Byte> code = {
            0xA2, 0x42,       // LDX #$42
            0x8E, 0x00, 0x02  // STX $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(2);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x42) {
            printf("LDX: Expected 0x42, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test LDY
    {
        std::vector<bitforge::Byte> code = {
            0xA0, 0x77,       // LDY #$77
            0x8C, 0x00, 0x02  // STY $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(2);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x77) {
            printf("LDY: Expected 0x77, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDA #$55, TAX, STX $7E0200 -> Expect 0x55
static int test_stx_sty() {
    TestHarness h;
    
    // Test STX
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x55,       // LDA #$55
            0xAA,             // TAX
            0x8E, 0x00, 0x02  // STX $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x55) {
            printf("STX: Expected 0x55, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test STY
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x33,       // LDA #$33
            0xA8,             // TAY
            0x8C, 0x00, 0x02  // STY $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x33) {
            printf("STY: Expected 0x33, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDX #$05, INX, DEX -> Expect 0x05
static int test_dex_dey_inx_iny() {
    TestHarness h;
    
    // Test INX
    {
        std::vector<bitforge::Byte> code = {
            0xA2, 0x05,       // LDX #$05
            0xE8,             // INX
            0x8E, 0x00, 0x02  // STX $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x06) {
            printf("INX: Expected 0x06, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test DEX
    {
        std::vector<bitforge::Byte> code = {
            0xA2, 0x05,       // LDX #$05
            0xCA,             // DEX
            0x8E, 0x00, 0x02  // STX $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x04) {
            printf("DEX: Expected 0x04, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test INY
    {
        std::vector<bitforge::Byte> code = {
            0xA0, 0x10,       // LDY #$10
            0xC8,             // INY
            0x8C, 0x00, 0x02  // STY $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x11) {
            printf("INY: Expected 0x11, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test DEY
    {
        std::vector<bitforge::Byte> code = {
            0xA0, 0x10,       // LDY #$10
            0x88,             // DEY
            0x8C, 0x00, 0x02  // STY $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x0F) {
            printf("DEY: Expected 0x0F, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDA #$81, CLC, ROL, STA $7E0200 -> Expect 0x02 (bit 7 shifted out, 0 in)
static int test_rol_ror() {
    TestHarness h;
    
    // Test ROL (rotate left)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x81,       // LDA #$81 (10000001)
            0x18,             // CLC (carry = 0)
            0x2A,             // ROL A (00000010, carry = 1)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(4);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x02) {
            printf("ROL: Expected 0x02, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test ROR (rotate right)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x81,       // LDA #$81 (10000001)
            0x38,             // SEC (carry = 1)
            0x6A,             // ROR A (11000000, carry = 1)
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(4);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0xC0) {
            printf("ROR: Expected 0xC0, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Test: LDA #$04, ASL, STA $7E0200 -> Expect 0x08
static int test_asl_lsr() {
    TestHarness h;
    
    // Test ASL (arithmetic shift left)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x04,       // LDA #$04
            0x0A,             // ASL A
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x08) {
            printf("ASL: Expected 0x08, got 0x%02X", result);
            return 1;
        }
    }
    
    // Test LSR (logical shift right)
    {
        std::vector<bitforge::Byte> code = {
            0xA9, 0x08,       // LDA #$08
            0x4A,             // LSR A
            0x8D, 0x00, 0x02  // STA $7E0200
        };
        h.load_test_rom(0x8000, code, 0x8000);
        h.run_cycles(3);
        
        bitforge::Byte result = h.bus.read(0x7E0200);
        if (result != 0x04) {
            printf("LSR: Expected 0x04, got 0x%02X", result);
            return 1;
        }
    }
    
    return 0;
}

// Write CGRAM through $2121/$2122 and read it back through $213B.
// The read uses the $80 bank mirror.
static int test_ppu_register_bus() {
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    ppu.connect_bus(&h.bus);

    h.bus.write(0x002121, 0x00);
    h.bus.write(0x002122, 0xAB);
    h.bus.write(0x802121, 0x00);

    bitforge::Byte color = h.bus.read(0x80213B);
    if (color != 0xAB) {
        printf("CGRAM: Expected 0xAB, got 0x%02X", color);
        return 1;
    }
    return 0;
}

static int test_cpu_io_math() {
    TestHarness h;

    h.bus.write(0x004202, 0x10);
    h.bus.write(0x004203, 0x20);
    if (h.bus.read(0x004216) != 0x00 || h.bus.read(0x004217) != 0x02) {
        printf("MUL: Expected 0x0200, got 0x%02X%02X",
               h.bus.read(0x004217), h.bus.read(0x004216));
        return 1;
    }

    h.bus.write(0x004204, 0x0A);
    h.bus.write(0x004205, 0x01);
    h.bus.write(0x004206, 0x10);
    if (h.bus.read(0x004214) != 16 || h.bus.read(0x004215) != 0 ||
        h.bus.read(0x004216) != 10 || h.bus.read(0x004217) != 0) {
        printf("DIV: Expected quot 16 rem 10");
        return 1;
    }

    h.bus.write(0x004204, 0x34);
    h.bus.write(0x004205, 0x12);
    h.bus.write(0x004206, 0x00);
    if (h.bus.read(0x004214) != 0xFF || h.bus.read(0x004215) != 0xFF ||
        h.bus.read(0x004216) != 0x34 || h.bus.read(0x004217) != 0x12) {
        printf("DIV0: Expected quot 0xFFFF rem 0x1234");
        return 1;
    }

    h.bus.set_joypad1(0x8000);
    if (h.bus.read(0x004218) != 0x00 || h.bus.read(0x004219) != 0x80) {
        printf("JOY1: Expected 0x8000");
        return 1;
    }
    return 0;
}

// Master clocks per scanline (the PPU steps in 21.477 MHz master clocks).
static constexpr uint32_t CYCLES_PER_SCANLINE = 1364;

static void fill_nmi_handler(std::vector<bitforge::Byte>& code) {
    // $8100: LDA #$5A / STA $0200 / RTI
    code[0x100] = 0xA9;
    code[0x101] = 0x5A;
    code[0x102] = 0x8D;
    code[0x103] = 0x00;
    code[0x104] = 0x02;
    code[0x105] = 0x40;
    code[0x7FFA] = 0x00;
    code[0x7FFB] = 0x81;
}

static int test_nmi_taken() {
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    ppu.connect_bus(&h.bus);

    std::vector<bitforge::Byte> code(32768, 0xEA);
    code[0] = 0xA9; code[1] = 0x80;                         // LDA #$80
    code[2] = 0x8D; code[3] = 0x00; code[4] = 0x42;         // STA $4200
    code[5] = 0x4C; code[6] = 0x05; code[7] = 0x80;         // JMP $8005
    fill_nmi_handler(code);

    h.load_test_rom(0x8000, code, 0x8000);
    h.bus.write(0x7E0200, 0x00);
    h.run_cycles(2);

    ppu.step(225 * CYCLES_PER_SCANLINE + 2);  // the NMI flag rises at H=0.5 (2 clocks), V-blank at H=0

    if ((h.bus.read(0x804212) & 0x80) == 0) {
        printf("HVBJOY bit 7 clear inside vblank");
        return 1;
    }
    bitforge::Byte status = h.bus.read(0x004210);
    if (status != 0x82) {
        printf("RDNMI: Expected 0x82, got 0x%02X", status);
        return 1;
    }
    if (h.bus.read(0x004210) != 0x02) {
        printf("RDNMI flag was not cleared on read");
        return 1;
    }

    // Reading $4210 acknowledges the flag. The edge latch still fires.
    h.run_cycles(1);
    h.run_cycles(3);

    bitforge::Byte result = h.bus.read(0x7E0200);
    if (result != 0x5A) {
        printf("NMI handler did not run, got 0x%02X", result);
        return 1;
    }
    return 0;
}

static int test_nmi_disabled() {
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    ppu.connect_bus(&h.bus);

    std::vector<bitforge::Byte> code(32768, 0xEA);
    code[0] = 0x4C; code[1] = 0x00; code[2] = 0x80;         // JMP $8000
    fill_nmi_handler(code);

    h.load_test_rom(0x8000, code, 0x8000);
    h.bus.write(0x7E0200, 0x00);
    ppu.step(225 * CYCLES_PER_SCANLINE);
    h.cpu.step();

    bitforge::Byte result = h.bus.read(0x7E0200);
    if (result != 0x00) {
        printf("NMI ran with NMITIMEN clear, got 0x%02X", result);
        return 1;
    }
    return 0;
}

static int test_vblank_ends() {
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    ppu.connect_bus(&h.bus);

    ppu.step(262 * CYCLES_PER_SCANLINE);

    if (h.bus.read(0x004212) & 0x80) {
        printf("HVBJOY still in vblank after the frame");
        return 1;
    }
    bitforge::Byte status = h.bus.read(0x004210);
    if (status & 0x80) {
        printf("RDNMI still set after vblank, got 0x%02X", status);
        return 1;
    }
    if ((status & 0x0F) != 0x02) {
        printf("CPU version: Expected 2, got 0x%02X", status & 0x0F);
        return 1;
    }
    return 0;
}

static bitforge::Word read_vram_word(bitforge::memory::Bus& bus, bitforge::Word addr) {
    // Increment on the high byte, no translation, so the pair reads one physical word.
    bus.write(0x002115, 0x80);
    bus.write(0x002116, static_cast<bitforge::Byte>(addr & 0xFF));
    bus.write(0x002117, static_cast<bitforge::Byte>((addr >> 8) & 0xFF));
    bitforge::Byte low = bus.read(0x002139);
    bitforge::Byte high = bus.read(0x00213A);
    return static_cast<bitforge::Word>(low | (high << 8));
}

static int test_vram_port() {
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    ppu.connect_bus(&h.bus);

    // VMAIN=$80: increment by 1 word after the high byte. Two DMA-style writes.
    h.bus.write(0x002115, 0x80);
    h.bus.write(0x002116, 0x00);
    h.bus.write(0x002117, 0x00);
    h.bus.write(0x002118, 0x34);
    h.bus.write(0x002119, 0x12);
    h.bus.write(0x002118, 0x78);
    h.bus.write(0x002119, 0x56);

    bitforge::Word word = read_vram_word(h.bus, 0x0000);
    if (word != 0x1234) {
        printf("word 0: Expected 0x1234, got 0x%04X", word);
        return 1;
    }
    word = read_vram_word(h.bus, 0x0001);
    if (word != 0x5678) {
        printf("word 1: Expected 0x5678, got 0x%04X", word);
        return 1;
    }

    // VMAIN=$00 increments on the low byte, so the high byte hits the next word.
    h.bus.write(0x002115, 0x00);
    h.bus.write(0x002116, 0x10);
    h.bus.write(0x002117, 0x00);
    h.bus.write(0x002118, 0x11);
    h.bus.write(0x002119, 0x22);
    word = read_vram_word(h.bus, 0x0010);
    if (word != 0x0011) {
        printf("inc-on-low word: Expected 0x0011, got 0x%04X", word);
        return 1;
    }
    word = read_vram_word(h.bus, 0x0011);
    if (word != 0x2200) {
        printf("inc-on-low next: Expected 0x2200, got 0x%04X", word);
        return 1;
    }

    // VMAIN=$81: step 32 words (bits 1-0), after the high byte.
    h.bus.write(0x002115, 0x81);
    h.bus.write(0x002116, 0x00);
    h.bus.write(0x002117, 0x02);
    h.bus.write(0x002118, 0xAB);
    h.bus.write(0x002119, 0xCD);
    h.bus.write(0x002118, 0xEF);
    h.bus.write(0x002119, 0x01);
    word = read_vram_word(h.bus, 0x0200);
    if (word != 0xCDAB) {
        printf("step32 first: Expected 0xCDAB, got 0x%04X", word);
        return 1;
    }
    word = read_vram_word(h.bus, 0x0201);
    if (word != 0x0000) {
        printf("step32 gap: Expected 0x0000, got 0x%04X", word);
        return 1;
    }
    word = read_vram_word(h.bus, 0x0220);
    if (word != 0x01EF) {
        printf("step32 second: Expected 0x01EF, got 0x%04X", word);
        return 1;
    }

    // VMAIN=$84, 8-bit translation (bits 3-2 = 1): word $0020 maps to word $0001.
    h.bus.write(0x002115, 0x84);
    h.bus.write(0x002116, 0x20);
    h.bus.write(0x002117, 0x00);
    h.bus.write(0x002118, 0x42);
    h.bus.write(0x002119, 0x24);
    word = read_vram_word(h.bus, 0x0001);
    if (word != 0x2442) {
        printf("translate: Expected 0x2442 at word 1, got 0x%04X", word);
        return 1;
    }

    // Screen on, not in vblank: the data port is ignored.
    h.bus.write(0x002115, 0x80);
    h.bus.write(0x002100, 0x0F);
    h.bus.write(0x002116, 0x00);
    h.bus.write(0x002117, 0x00);
    h.bus.write(0x002118, 0xFF);
    h.bus.write(0x002119, 0xFF);
    h.bus.write(0x002100, 0x80);
    word = read_vram_word(h.bus, 0x0000);
    if (word != 0x1234) {
        printf("blanking: display-period write stuck, got 0x%04X", word);
        return 1;
    }

    // Vblank opens the port again even if the screen is enabled.
    h.bus.write(0x002100, 0x0F);
    ppu.step(225 * CYCLES_PER_SCANLINE);
    h.bus.write(0x002115, 0x80);
    h.bus.write(0x002116, 0x00);
    h.bus.write(0x002117, 0x30);
    h.bus.write(0x002118, 0x55);
    h.bus.write(0x002119, 0xAA);
    word = read_vram_word(h.bus, 0x3000);
    if (word != 0xAA55) {
        printf("vblank write: Expected 0xAA55, got 0x%04X", word);
        return 1;
    }

    // DMA mode 1 into $2118 writes a pair of words.
    h.bus.write(0x002115, 0x80);
    h.bus.write(0x002116, 0x00);
    h.bus.write(0x002117, 0x10);
    h.bus.write(0x7E1000, 0x11);
    h.bus.write(0x7E1001, 0x22);
    h.bus.write(0x7E1002, 0x33);
    h.bus.write(0x7E1003, 0x44);
    h.bus.write(0x004300, 0x01);
    h.bus.write(0x004301, 0x18);
    h.bus.write(0x004302, 0x00);
    h.bus.write(0x004303, 0x10);
    h.bus.write(0x004304, 0x7E);
    h.bus.write(0x004305, 0x04);
    h.bus.write(0x004306, 0x00);
    h.bus.write(0x00420B, 0x01);
    word = read_vram_word(h.bus, 0x1000);
    if (word != 0x2211) {
        printf("DMA word 0: Expected 0x2211, got 0x%04X", word);
        return 1;
    }
    word = read_vram_word(h.bus, 0x1001);
    if (word != 0x4433) {
        printf("DMA word 1: Expected 0x4433, got 0x%04X", word);
        return 1;
    }
    return 0;
}

static void write_cgram(bitforge::memory::Bus& bus, bitforge::Byte index, bitforge::Word color) {
    bus.write(0x002121, index);
    bus.write(0x002122, static_cast<bitforge::Byte>(color & 0xFF));
    bus.write(0x002122, static_cast<bitforge::Byte>((color >> 8) & 0xFF));
}

// 4bpp tile: 8 words of planes 0/1, then 8 words of planes 2/3. One word per pixel row.
static void write_4bpp_tile(bitforge::memory::Bus& bus, bitforge::Word word_addr,
                             bitforge::Byte plane0, bitforge::Byte plane1, int solid_rows) {
    bus.write(0x002115, 0x80);
    bus.write(0x002116, static_cast<bitforge::Byte>(word_addr & 0xFF));
    bus.write(0x002117, static_cast<bitforge::Byte>((word_addr >> 8) & 0xFF));
    for (int row = 0; row < 8; row++) {
        bool solid = row < solid_rows;
        bus.write(0x002118, solid ? plane0 : static_cast<bitforge::Byte>(0));
        bus.write(0x002119, solid ? plane1 : static_cast<bitforge::Byte>(0));
    }
    for (int row = 0; row < 8; row++) {
        bus.write(0x002118, 0x00);
        bus.write(0x002119, 0x00);
    }
}

static void write_tilemap_entry(bitforge::memory::Bus& bus, bitforge::Word word_addr, bitforge::Word entry) {
    bus.write(0x002115, 0x80);
    bus.write(0x002116, static_cast<bitforge::Byte>(word_addr & 0xFF));
    bus.write(0x002117, static_cast<bitforge::Byte>((word_addr >> 8) & 0xFF));
    bus.write(0x002118, static_cast<bitforge::Byte>(entry & 0xFF));
    bus.write(0x002119, static_cast<bitforge::Byte>((entry >> 8) & 0xFF));
}

static int expect_pixel(const uint32_t* fb, int x, int y, uint32_t color, const char* label) {
    uint32_t pixel = fb[y * bitforge::ppu::PPU::WIDTH + x];  // 256-wide frames only
    if (pixel != color) {
        printf("%s (%d,%d): Expected 0x%08X, got 0x%08X", label, x, y, color, pixel);
        return 1;
    }
    return 0;
}

static int test_bg_sampler_rows() {
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    ppu.connect_bus(&h.bus);

    // Force-blank stays on from reset, so the VRAM port accepts the upload.
    // Color 1 is red, color 2 is green. 5-bit channels widen by bit replication (31 -> 255).
    write_cgram(h.bus, 1, 0x001F);
    write_cgram(h.bus, 2, 0x03E0);

    // Tiles at character base 0. Tilemap is one 1KB step later so it does not overlap.
    write_4bpp_tile(h.bus, 0x0000, 0xFF, 0x00, 8);  // tile 0, solid color 1
    write_4bpp_tile(h.bus, 0x0010, 0x00, 0xFF, 8);  // tile 1, solid color 2
    write_4bpp_tile(h.bus, 0x0020, 0xFF, 0x00, 1);  // tile 2, color 1 on its top row only
    write_4bpp_tile(h.bus, 0x0030, 0x80, 0x00, 8);  // tile 3, color 1 on the leftmost pixel

    constexpr bitforge::Word map = 0x0400;
    write_tilemap_entry(h.bus, map + 0, 0x0000);
    write_tilemap_entry(h.bus, map + 1, 0x4003);          // h-flip
    write_tilemap_entry(h.bus, map + 32, 0x0001);         // next tilemap row
    write_tilemap_entry(h.bus, map + 64, 0x8002);         // v-flip

    h.bus.write(0x002105, 0x01);  // mode 1, BG1 is 4bpp
    h.bus.write(0x002107, 0x04);  // BG1SC word base $0400
    h.bus.write(0x00210B, 0x00);
    h.bus.write(0x00212C, 0x01);  // TM enables BG1
    h.bus.write(0x00210E, 0xFF);  // BG1VOFS = -1: output row 0 is hardware line 1, which shows BG line VOFS+1
    h.bus.write(0x00210E, 0x03);
    h.bus.write(0x002100, 0x0F);  // end force blank, full brightness

    // Row 23 is hardware line 24, drawn when that line reaches H-blank.
    ppu.step(25 * CYCLES_PER_SCANLINE);
    const uint32_t* fb = ppu.get_framebuffer();

    constexpr uint32_t red = 0xFFFF0000;
    constexpr uint32_t green = 0xFF00FF00;
    constexpr uint32_t black = 0xFF000000;

    if (expect_pixel(fb, 0, 0, red, "row 0 top")) return 1;
    if (expect_pixel(fb, 7, 7, red, "row 0 bottom")) return 1;
    if (expect_pixel(fb, 0, 8, green, "row 1 top")) return 1;
    if (expect_pixel(fb, 0, 15, green, "row 1 bottom")) return 1;

    // Bit 7 is the left pixel. H-flip moves that pixel to x=15 of the second tile.
    if (expect_pixel(fb, 8, 0, black, "h-flip left")) return 1;
    if (expect_pixel(fb, 15, 0, red, "h-flip right")) return 1;

    // V-flip shows the blank bottom of tile 2 first, then its colored top row.
    if (expect_pixel(fb, 0, 16, black, "v-flip top")) return 1;
    if (expect_pixel(fb, 0, 23, red, "v-flip bottom")) return 1;
    return 0;
}

static int test_log_ring() {
    using bitforge::utils::LogLevel;
    using bitforge::utils::Logger;

    LogLevel previous = Logger::level();
    Logger::set_level(LogLevel::Info);
    Logger::clear();

    Logger::debug("hidden");
    if (Logger::line_count() != 0) {
        printf("debug line passed the info filter, count %d", Logger::line_count());
        Logger::set_level(previous);
        return 1;
    }

    Logger::info("first");
    Logger::error("bad");
    if (Logger::line_count() != 2) {
        printf("line count: Expected 2, got %d", Logger::line_count());
        Logger::set_level(previous);
        return 1;
    }
    if (Logger::line_text(0) != "[INFO] first") {
        printf("oldest line: Expected [INFO] first, got %s", Logger::line_text(0).c_str());
        Logger::set_level(previous);
        return 1;
    }
    if (Logger::line_level(1) != LogLevel::Error ||
        Logger::line_text(1) != "[ERROR] bad") {
        printf("newest line: Expected [ERROR] bad, got %s", Logger::line_text(1).c_str());
        Logger::set_level(previous);
        return 1;
    }

    Logger::clear();
    if (Logger::line_count() != 0) {
        printf("clear left %d lines", Logger::line_count());
        Logger::set_level(previous);
        return 1;
    }

    Logger::set_level(previous);
    return 0;
}

// Channels route to the System or Emulation sink; disabled channels and levels below a sink's
// filter cost nothing (the macro's arguments aren't evaluated); files are capped and rotated.
static int test_log_channels() {
    using bitforge::utils::LogChannel;
    using bitforge::utils::LogLevel;
    using bitforge::utils::Logger;
    using bitforge::utils::LogSink;
    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            printf("%s; ", what);
            failures++;
        }
    };

    Logger::set_level(LogLevel::Info);
    Logger::clear(LogSink::System);
    Logger::clear(LogSink::Emulation);

    Logger::info(LogChannel::Cpu, "cpu line");
    Logger::info(LogChannel::Video, "video line");
    expect(Logger::line_count(LogSink::Emulation) == 1 && Logger::line_channel(0, LogSink::Emulation) == LogChannel::Cpu,
           "CPU goes to Emulation");
    expect(Logger::line_count(LogSink::System) == 1 && Logger::line_channel(0, LogSink::System) == LogChannel::Video,
           "Video goes to System");
    expect(Logger::line_text(0, LogSink::Emulation) == "[INFO] cpu line", "line text");

    int evaluated = 0;
    auto arg = [&] { return ++evaluated; };
    CPU_TRACE("trace %d", arg());  // Emulation is at Info: Trace is filtered before formatting
    expect(evaluated == 0 && Logger::line_count(LogSink::Emulation) == 1, "trace off by level");
    Logger::set_level(LogSink::Emulation, LogLevel::Trace);
    Logger::set_channel_enabled(LogChannel::Cpu, false);
    CPU_TRACE("trace %d", arg());
    expect(evaluated == 0 && Logger::line_count(LogSink::Emulation) == 1, "trace off by channel");
    Logger::set_channel_enabled(LogChannel::Cpu, true);
    CPU_TRACE("trace %d", arg());
    expect(evaluated == 1 && Logger::line_count(LogSink::Emulation) == 2, "trace on");
    expect(Logger::line_level(1, LogSink::Emulation) == LogLevel::Trace, "trace level");
    expect(Logger::line_count(LogSink::System) == 1, "trace stays out of System");

    Logger::set_paused(LogSink::Emulation, true);
    PPU_TRACE("paused");
    expect(Logger::line_count(LogSink::Emulation) == 2, "pause");
    Logger::set_paused(LogSink::Emulation, false);

    // A 2 KB cap: the file rotates to emu_trace_test.1.log and starts again.
    // error_code overloads throughout: a missing file is a failed check, not an abort.
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "snes_log_test";
    std::filesystem::remove_all(dir, ec);
    const std::string path = (dir / "emu_trace_test.log").string();
    expect(Logger::set_file(LogSink::Emulation, path, 2048), "open log file");
    for (int i = 0; i < 100; i++) DMA_TRACE("DMA line %d with some padding to fill the file faster", i);
    Logger::set_file(LogSink::Emulation, {}, 0);
    const auto size_of = [&ec](const std::filesystem::path& p) { const auto n = std::filesystem::file_size(p, ec); return ec ? 0 : n; };
    expect(size_of(dir / "emu_trace_test.1.log") >= 2048, "rotated file");
    const auto current = size_of(path);
    expect(current > 0 && current < 2048, "current file under the cap");
    std::string line, final_line;
    {
        std::ifstream last(path);  // closed before the folder goes: Windows can't delete an open file
        while (std::getline(last, line)) final_line = line;
    }
    expect(final_line.find("[DMA] [TRACE] DMA line 99") != std::string::npos, "file line format");
    std::filesystem::remove_all(dir, ec);

    Logger::set_level(LogLevel::Info);
    Logger::clear(LogSink::System);
    Logger::clear(LogSink::Emulation);
    return failures == 0 ? 0 : 1;
}
// ===== 65C816 core and memory map tests =====

namespace {

using Bytes = std::vector<bitforge::Byte>;

constexpr bitforge::Address lorom_offset(bitforge::Address addr) {
    return (((addr >> 16) & 0x7F) << 15) | (addr & 0x7FFF);
}

// Builds a LoROM image: code at 24-bit addresses, vectors in bank 0. Reset goes to $00:8000.
struct RomBuilder {
    Bytes rom;
    explicit RomBuilder(size_t banks = 1) : rom(banks * 0x8000, 0x00) { vector(0xFFFC, 0x8000); }
    void put(bitforge::Address addr, const Bytes& bytes) {
        for (size_t i = 0; i < bytes.size(); i++) rom[lorom_offset(addr + static_cast<bitforge::Address>(i))] = bytes[i];
    }
    void vector(bitforge::Word addr, bitforge::Word target) {
        put(addr, {static_cast<bitforge::Byte>(target), static_cast<bitforge::Byte>(target >> 8)});
    }
};

void boot(TestHarness& h, const RomBuilder& b) {
    h.bus.load_test_rom(b.rom);
    h.cpu.reset();
}

bool run_to_stp(TestHarness& h, int max_steps = 100000) {
    for (int i = 0; i < max_steps && !h.cpu.stopped(); i++) h.cpu.step();
    if (!h.cpu.stopped()) printf("did not reach STP; PC=$%02X:%04X ", h.cpu.registers().pbr, h.cpu.registers().pc);
    return h.cpu.stopped();
}

int expect_eq(unsigned actual, unsigned expected, const char* what) {
    if (actual == expected) return 0;
    printf("%s: expected $%X, got $%X", what, expected, actual);
    return 1;
}

unsigned read16(TestHarness& h, bitforge::Address addr) {
    return static_cast<unsigned>(h.bus.read(addr) | (h.bus.read(addr + 1) << 8));
}

const Bytes kNative16 = {0x18, 0xFB, 0xC2, 0x30};  // CLC, XCE, REP #$30

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

} // namespace

static int test_mixed_widths() {
    TestHarness h;
    RomBuilder b;
    b.put(0x008000, {
        0x18, 0xFB,             // CLC, XCE
        0xC2, 0x10,             // REP #$10   X/Y 16-bit
        0xE2, 0x20,             // SEP #$20   A 8-bit
        0xA2, 0x34, 0x12,       // LDX #$1234 (3 bytes: width from X)
        0xA9, 0x56,             // LDA #$56   (2 bytes: width from M)
        0xA0, 0xCD, 0xAB,       // LDY #$ABCD
        0xE0, 0x34, 0x12,       // CPX #$1234
        0xF0, 0x02,             // BEQ +2
        0xA9, 0xEE,             // LDA #$EE (skipped)
        0x8D, 0x00, 0x03,       // STA $0300
        0xC2, 0x20,             // REP #$20   A 16-bit
        0xE2, 0x10,             // SEP #$10   X/Y 8-bit: high bytes cleared
        0xA9, 0xEF, 0xBE,       // LDA #$BEEF
        0xA2, 0x12,             // LDX #$12
        0xDB,                   // STP
    });
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    auto r = h.cpu.registers();
    return expect_eq(r.a, 0xBEEF, "A") || expect_eq(r.x, 0x0012, "X") || expect_eq(r.y, 0x00CD, "Y") ||
           expect_eq(h.bus.read(0x7E0300), 0x56, "$0300");
}

static int test_direct_page_register() {
    TestHarness h;
    RomBuilder b;
    b.put(0x008000, concat({kNative16, {
        0xA9, 0x00, 0x12, 0x5B,             // LDA #$1200, TCD
        0xE2, 0x20, 0xA9, 0x42, 0x85, 0x34, // SEP #$20, LDA #$42, STA $34 -> $00:1234
        0xC2, 0x20, 0x7B,                   // REP #$20, TDC
        0x8F, 0x00, 0x01, 0x7E,             // STA $7E0100
        0x0B,                               // PHD
        0xF4, 0x78, 0x56, 0x2B,             // PEA $5678, PLD
        0x7B, 0x8F, 0x02, 0x01, 0x7E,       // TDC, STA $7E0102
        0x2B,                               // PLD -> $1200
        0xDB,
    }}));
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    return expect_eq(h.bus.read(0x7E1234), 0x42, "STA dp with D=$1200") ||
           expect_eq(read16(h, 0x7E0100), 0x1200, "TDC") || expect_eq(read16(h, 0x7E0102), 0x5678, "PLD") ||
           expect_eq(h.cpu.registers().d, 0x1200, "D after PHD/PLD");
}

static int test_direct_page_emulation_wrap() {
    TestHarness h;
    RomBuilder b;
    b.put(0x008000, {
        0x18, 0xFB, 0xC2, 0x20, 0xA9, 0x00, 0x01, 0x5B,  // native, D = $0100
        0xE2, 0x20,
        0xA9, 0x11, 0x8F, 0x10, 0x01, 0x7E,              // $0110 = $11
        0xA9, 0x22, 0x8F, 0x10, 0x02, 0x7E,              // $0210 = $22
        0x38, 0xFB,                                      // SEC, XCE: emulation, DL = 0
        0xA2, 0x20, 0xB5, 0xF0, 0x8D, 0x00, 0x03,        // LDX #$20, LDA $F0,X -> wraps to $0110
        0x18, 0xFB,                                      // native
        0xB5, 0xF0, 0x8D, 0x01, 0x03,                    // LDA $F0,X -> $0210
        0xDB,
    });
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    return expect_eq(h.bus.read(0x7E0300), 0x11, "emulation dp,X page wrap") ||
           expect_eq(h.bus.read(0x7E0301), 0x22, "native dp,X no page wrap");
}

static int test_pbr_jumps() {
    TestHarness h;
    RomBuilder b(3);
    b.put(0x008000, {
        0x18, 0xFB, 0xE2, 0x30,
        0xA9, 0x7E, 0x48, 0xAB,                          // DBR = $7E, so DBR != PBR
        0xA9, 0x00, 0x8D, 0x00, 0x03, 0xA9, 0x83, 0x8D, 0x01, 0x03,                    // ($0300) = $8300
        0xA9, 0x00, 0x8D, 0x04, 0x03, 0xA9, 0x87, 0x8D, 0x05, 0x03, 0xA9, 0x02, 0x8D, 0x06, 0x03,  // [$0304] = $02:8700
        0x5C, 0x00, 0x80, 0x01,                          // JML $01:8000
    });
    b.put(0x018000, {0x4C, 0x00, 0x81});                 // JMP $8100 -> must stay in bank 1
    b.put(0x008100, {0xA9, 0xEE, 0xDB});                 // trap: wrong bank
    b.put(0x018100, {0x20, 0x00, 0x82, 0x6C, 0x00, 0x03}); // JSR $8200, JMP ($0300)
    b.put(0x018200, {0xA2, 0x22, 0x60});                 // LDX #$22, RTS
    b.put(0x018300, {0xA0, 0x33, 0xA2, 0x00, 0x7C, 0x00, 0x84}); // LDY #$33, LDX #0, JMP ($8400,X)
    b.put(0x018400, {0x00, 0x85});                       // table in the program bank -> $8500
    b.put(0x008400, {0x00, 0x86});                       // decoy table in bank 0 -> trap
    b.put(0x018600, {0xA9, 0xEE, 0xDB});
    b.put(0x018500, {0xDC, 0x04, 0x03});                 // JML [$0304]
    b.put(0x028700, {0xA2, 0x00, 0xFC, 0x00, 0x88,       // LDX #0, JSR ($8800,X)
                     0x22, 0x00, 0x89, 0x01,             // JSL $01:8900
                     0xA9, 0x11, 0xDB});                 // LDA #$11, STP
    b.put(0x028800, {0x00, 0x8A});
    b.put(0x028A00, {0xC8, 0x60});                       // INY, RTS
    b.put(0x018900, {0xA9, 0x44, 0x8D, 0x10, 0x03, 0x6B}); // LDA #$44, STA $0310, RTL
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    auto r = h.cpu.registers();
    return expect_eq(r.a & 0xFF, 0x11, "A (trap hit if $EE)") || expect_eq(r.y, 0x34, "Y") ||
           expect_eq(r.pbr, 0x02, "PBR") || expect_eq(r.dbr, 0x7E, "DBR") ||
           expect_eq(h.bus.read(0x7E0310), 0x44, "JSL/RTL body");
}

static int test_bank_wrap() {
    TestHarness h;
    RomBuilder b;
    b.put(0x008000, {
        0x18, 0xFB, 0xC2, 0x20, 0xE2, 0x10,             // native, M16, X8
        0xA9, 0xEF, 0xBE, 0x8F, 0xFF, 0xFF, 0x7E,       // STA $7EFFFF: high byte carries into $7F:0000
        0xE2, 0x20, 0xA9, 0x7E, 0x48, 0xAB, 0xC2, 0x20, // DBR = $7E
        0xAD, 0xFF, 0xFF, 0x8F, 0x00, 0x04, 0x7E,       // LDA $FFFF -> $BEEF
        0xA2, 0x02, 0xBD, 0xFE, 0xFF, 0x8F, 0x02, 0x04, 0x7E, // LDA $FFFE,X -> $7F:0000
        0xA9, 0xFF, 0xFF, 0x85, 0x00,                   // ($00) = $FFFF
        0xA0, 0x01, 0xB1, 0x00, 0x8F, 0x04, 0x04, 0x7E, // LDA ($00),Y -> $7E:FFFF + 1 = $7F:0000
        0xE2, 0x20,
        0xA9, 0xEA, 0x8F, 0xFF, 0xFF, 0x7E,             // NOP at $7E:FFFF
        0xA9, 0xDB, 0x8F, 0x00, 0x00, 0x7E,             // STP at $7E:0000
        0x5C, 0xFF, 0xFF, 0x7E,                         // JML $7E:FFFF: the fetch wraps to $7E:0000
    });
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    auto r = h.cpu.registers();
    return expect_eq(h.bus.read(0x7F0000), 0xBE, "16-bit store carry") ||
           expect_eq(read16(h, 0x7E0400), 0xBEEF, "abs read across bank") ||
           expect_eq(h.bus.read(0x7E0402), 0xBE, "abs,X across bank") ||
           expect_eq(h.bus.read(0x7E0404), 0xBE, "(dp),Y across bank") ||
           expect_eq(r.pbr, 0x7E, "PBR after PC wrap") || expect_eq(r.pc, 0x0001, "PC wraps in bank");
}

static int test_block_move() {
    TestHarness h;
    RomBuilder b;
    b.put(0x008000, concat({kNative16, {
        0xA9, 0x11, 0x22, 0x8F, 0x00, 0x10, 0x7E,
        0xA9, 0x33, 0x44, 0x8F, 0x02, 0x10, 0x7E,
        0xA9, 0x03, 0x00, 0xA2, 0x00, 0x10, 0xA0, 0x00, 0x20,
        0x54, 0x7F, 0x7E,                               // MVN $7E -> $7F, 4 bytes ascending
        0x8F, 0x00, 0x05, 0x7E,                         // A after MVN
        0xA9, 0x03, 0x00, 0xA2, 0x03, 0x10, 0xA0, 0x03, 0x30,
        0x44, 0x7F, 0x7E,                               // MVP $7E -> $7F, 4 bytes descending
        0xDB,
    }}));
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    auto r = h.cpu.registers();
    for (bitforge::Address i = 0; i < 4; i++) {
        const unsigned want = 0x11u * (i + 1);
        if (expect_eq(h.bus.read(0x7F2000 + i), want, "MVN byte") ||
            expect_eq(h.bus.read(0x7F3000 + i), want, "MVP byte")) return 1;
    }
    return expect_eq(read16(h, 0x7E0500), 0xFFFF, "A after MVN") || expect_eq(r.a, 0xFFFF, "A after MVP") ||
           expect_eq(r.x, 0x0FFF, "X after MVP") || expect_eq(r.y, 0x2FFF, "Y after MVP") ||
           expect_eq(r.dbr, 0x7F, "DBR = destination bank");
}

static int test_decimal_mode() {
    TestHarness h;
    RomBuilder b;
    b.put(0x008000, {
        0x18, 0xFB, 0xE2, 0x30, 0xF8,                          // native, 8-bit, SED
        0x18, 0xA9, 0x19, 0x69, 0x28, 0x8F, 0x00, 0x05, 0x7E,  // 19 + 28 = 47
        0x18, 0xA9, 0x99, 0x69, 0x01, 0x8F, 0x01, 0x05, 0x7E,  // 99 + 01 = 00, C
        0x08, 0x68, 0x8F, 0x02, 0x05, 0x7E,                    // P
        0x38, 0xA9, 0x00, 0xE9, 0x01, 0x8F, 0x03, 0x05, 0x7E,  // 00 - 01 = 99, borrow
        0x08, 0x68, 0x8F, 0x04, 0x05, 0x7E,                    // P
        0xC2, 0x20,
        0x18, 0xA9, 0x99, 0x19, 0x69, 0x01, 0x00, 0x8F, 0x06, 0x05, 0x7E,  // 1999 + 0001 = 2000
        0x38, 0xA9, 0x00, 0x01, 0xE9, 0x01, 0x00, 0x8F, 0x08, 0x05, 0x7E,  // 0100 - 0001 = 0099
        0xDB,
    });
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    return expect_eq(h.bus.read(0x7E0500), 0x47, "8-bit BCD add") ||
           expect_eq(h.bus.read(0x7E0501), 0x00, "8-bit BCD add wrap") ||
           expect_eq(h.bus.read(0x7E0502) & 0x03, 0x03, "C and Z after 99+01") ||
           expect_eq(h.bus.read(0x7E0503), 0x99, "8-bit BCD subtract") ||
           expect_eq(h.bus.read(0x7E0504) & 0x01, 0x00, "borrow clears C") ||
           expect_eq(read16(h, 0x7E0506), 0x2000, "16-bit BCD add") ||
           expect_eq(read16(h, 0x7E0508), 0x0099, "16-bit BCD subtract");
}

static int test_emulation_mode_rules() {
    TestHarness h;
    RomBuilder b;
    b.put(0x008000, {
        0xC2, 0x30,                          // REP #$30 in emulation: M and X stay set
        0xA9, 0x12,                          // LDA #$12 is still 2 bytes
        0xA2, 0xFF, 0x9A,                    // LDX #$FF, TXS -> S = $01FF
        0x08, 0x68, 0x8D, 0x00, 0x03,        // PHP, PLA -> $0300
        0xA9, 0x00, 0x48, 0x28,              // PLP #0: M/X cannot clear
        0x08, 0x68, 0x8D, 0x01, 0x03,
        0x18, 0xFB, 0xC2, 0x10,              // native, 16-bit index
        0xA2, 0x00, 0x1F, 0x9A,              // S = $1F00
        0xA2, 0x34, 0x12, 0xA0, 0x78, 0x56,  // X = $1234, Y = $5678
        0x38, 0xFB,                          // back to emulation
        0xDB,
    });
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    auto r = h.cpu.registers();
    return expect_eq(h.bus.read(0x7E0300) & 0x30, 0x30, "REP can't clear M/X") ||
           expect_eq(h.bus.read(0x7E0301) & 0x30, 0x30, "PLP can't clear M/X") ||
           expect_eq(r.x, 0x0034, "X high cleared by XCE") ||
           expect_eq(r.y, 0x0078, "Y high cleared by XCE") || expect_eq(r.sp, 0x0100, "SP high forced to $01") ||
           expect_eq(r.emulation ? 1u : 0u, 1u, "E") || expect_eq(r.p & 0x30, 0x30, "M/X set");
}

// Vectors point at STP stubs so the test can see which one ran.
static RomBuilder vector_rom() {
    RomBuilder b;
    const std::pair<bitforge::Word, bitforge::Word> vectors[] = {
        {0xFFE6, 0x9100}, {0xFFFE, 0x9200}, {0xFFE4, 0x9300}, {0xFFF4, 0x9400}, {0xFFFA, 0x9500}, {0xFFEA, 0x9600},
    };
    for (auto [vec, target] : vectors) {
        b.vector(vec, target);
        b.put(target, {0xDB});
    }
    return b;
}

static int test_interrupt_vectors() {
    struct Case { const char* name; Bytes code; bitforge::Word handler; };
    const Case cases[] = {
        {"emulation BRK", {0xA2, 0xFF, 0x9A, 0xF8, 0x00, 0x99}, 0x9200},
        {"native BRK", {0x18, 0xFB, 0xC2, 0x30, 0xA2, 0xFF, 0x1F, 0x9A, 0xF8, 0x00, 0x99}, 0x9100},
        {"emulation COP", {0xA2, 0xFF, 0x9A, 0xF8, 0x02, 0x99}, 0x9400},
        {"native COP", {0x18, 0xFB, 0xC2, 0x30, 0xA2, 0xFF, 0x1F, 0x9A, 0xF8, 0x02, 0x99}, 0x9300},
    };
    for (const auto& c : cases) {
        TestHarness h;
        RomBuilder b = vector_rom();
        b.put(0x008000, c.code);
        boot(h, b);
        if (!run_to_stp(h)) return 1;
        auto r = h.cpu.registers();
        const bool native = c.code[0] == 0x18;
        const bitforge::Address top = native ? 0x7E1FFF : 0x7E01FF;
        const bitforge::Word ret = static_cast<bitforge::Word>(0x8000 + c.code.size());  // past the signature byte
        const bitforge::Address pc_at = native ? top - 1 : top;
        const bitforge::Byte pushed_p = h.bus.read(pc_at - 2);
        if (expect_eq(r.pc, c.handler + 1u, "vector") ||
            expect_eq(read16(h, pc_at - 1), ret, "pushed PC") ||
            (native && expect_eq(h.bus.read(top), 0x00, "pushed PBR")) ||
            expect_eq(r.p & 0x0C, 0x04, "I set, D cleared") ||
            (!native && expect_eq(pushed_p & 0x10, 0x10, "B flag set on stack"))) {
            printf(" [%s]", c.name);
            return 1;
        }
    }
    return 0;
}

static int test_nmi_vectors_and_b_flag() {
    for (bool native : {false, true}) {
        TestHarness h;
        RomBuilder b = vector_rom();
        b.put(0x008000, native ? Bytes{0x18, 0xFB, 0xEA} : Bytes{0xEA, 0xEA});
        boot(h, b);
        h.cpu.step();
        if (native) h.cpu.step();
        h.bus.write(0x004200, 0x80);
        h.bus.on_vblank_start();
        h.bus.on_vblank_nmi();
        if (!run_to_stp(h)) return 1;
        auto r = h.cpu.registers();
        if (expect_eq(r.pc, native ? 0x9601u : 0x9501u, native ? "native NMI vector" : "emulation NMI vector")) return 1;
        if (!native && expect_eq(h.bus.read(0x7E01FD) & 0x10, 0x00, "B clear for NMI")) return 1;
    }
    return 0;
}

static int test_65816_opcodes() {
    TestHarness h;
    RomBuilder b;
    Bytes code = concat({kNative16, {
        0xA9, 0x34, 0x12, 0xE2, 0x20, 0xEB, 0xC2, 0x20,  // LDA #$1234, XBA (8-bit) -> $3412
        0x8F, 0x00, 0x06, 0x7E,
        0xF4, 0xEF, 0xBE, 0x68, 0x8F, 0x02, 0x06, 0x7E,  // PEA $BEEF, PLA
        0x62, 0x00, 0x00,                                // PER +0
    }});
    const auto per_target = static_cast<unsigned>(0x8000 + code.size());
    code = concat({code, {
        0x68, 0x8F, 0x04, 0x06, 0x7E,
        0x42, 0xFF,                                      // WDM is 2 bytes
        0x82, 0x03, 0x00,                                // BRL +3
        0xA9, 0xEE, 0xEE,                                // skipped
        0xF4, 0x78, 0x56, 0xA3, 0x01, 0x8F, 0x06, 0x06, 0x7E, 0x68,  // PEA, LDA $01,S
        0xA9, 0xCD, 0xAB, 0x8F, 0x02, 0x07, 0x7E,
        0xF4, 0x00, 0x07, 0xA0, 0x02, 0x00, 0xB3, 0x01, 0x8F, 0x08, 0x06, 0x7E, 0x68,  // LDA ($01,S),Y
        0xA9, 0x00, 0x07, 0x85, 0x10, 0xE2, 0x20, 0xA9, 0x7E, 0x85, 0x12, 0xC2, 0x20, // [$10] = $7E:0700
        0xB7, 0x10, 0x8F, 0x0A, 0x06, 0x7E,              // LDA [$10],Y
        0xA2, 0x02, 0x00, 0xBF, 0x00, 0x07, 0x7E, 0x8F, 0x0C, 0x06, 0x7E,  // LDA $7E0700,X
        0xD4, 0x10, 0x68, 0x8F, 0x0E, 0x06, 0x7E,        // PEI ($10), PLA
        0xA9, 0x00, 0x1E, 0x1B, 0xA9, 0x00, 0x00, 0x3B, 0x8F, 0x10, 0x06, 0x7E,  // TCS, TSC
        0xDB,
    }});
    b.put(0x008000, code);
    boot(h, b);
    if (!run_to_stp(h)) return 1;
    return expect_eq(read16(h, 0x7E0600), 0x3412, "XBA") || expect_eq(read16(h, 0x7E0602), 0xBEEF, "PEA") ||
           expect_eq(read16(h, 0x7E0604), per_target, "PER") ||
           expect_eq(read16(h, 0x7E0606), 0x5678, "sr,S") || expect_eq(read16(h, 0x7E0608), 0xABCD, "(sr,S),Y") ||
           expect_eq(read16(h, 0x7E060A), 0xABCD, "[dp],Y") || expect_eq(read16(h, 0x7E060C), 0xABCD, "long,X") ||
           expect_eq(read16(h, 0x7E060E), 0x0700, "PEI") || expect_eq(read16(h, 0x7E0610), 0x1E00, "TCS/TSC");
}

static int test_cycle_table() {
    TestHarness h;
    RomBuilder b;
    b.put(0x008000, {
        0xEA,                     // NOP: 2
        0x18, 0xFB, 0xC2, 0x30,   // CLC 2, XCE 2, REP 3
        0xA9, 0x34, 0x12,         // LDA #imm, 16-bit: 3
        0xA5, 0x10,               // LDA dp, 16-bit, DL = 0: 4
        0xA9, 0x01, 0x00, 0x5B,   // LDA 3, TCD 2
        0xA5, 0x10,               // LDA dp, DL != 0: 5
        0x80, 0x00,               // BRA (taken): 3
        0xDB,
    });
    boot(h, b);
    const int expected[] = {2, 2, 2, 3, 3, 4, 3, 2, 5, 3};
    for (int want : expected) {
        if (expect_eq(static_cast<unsigned>(h.cpu.step()), static_cast<unsigned>(want), "cycles")) return 1;
    }
    return 0;
}

static int test_wram_mirror() {
    TestHarness h;
    h.bus.write(0x7F0005, 0x00);
    h.bus.write(0x010005, 0xAB);        // bank $01 low mirror -> $7E:0005, not $7F
    h.bus.write(0x801FFF, 0xCD);        // bank $80 mirror -> $7E:1FFF
    h.bus.write(0x7E2000, 0x77);        // only $0000-$1FFF is mirrored
    return expect_eq(h.bus.read(0x7E0005), 0xAB, "$01:0005 -> $7E:0005") ||
           expect_eq(h.bus.read(0x7F0005), 0x00, "$7F untouched") ||
           expect_eq(h.bus.read(0x7E1FFF), 0xCD, "$80:1FFF -> $7E:1FFF") ||
           expect_eq(h.bus.read(0x3F1FFF), 0xCD, "$3F:1FFF mirror") ||
           expect_eq(h.bus.read(0x002000) == 0x77 ? 1u : 0u, 0u, "$00:2000 is not WRAM");
}

static int test_rom_mapping() {
    TestHarness h;
    Bytes rom(0x20000, 0x00);
    rom[0x08000] = 0x11;   // LoROM $01:8000
    rom[0x11234] = 0x22;   // HiROM $C1:1234 / $41:1234
    rom[0x19234] = 0x33;   // HiROM $01:9234 / $81:9234
    h.bus.load_test_rom(rom, bitforge::memory::MapMode::LoROM);
    if (expect_eq(h.bus.read(0x018000), 0x11, "LoROM $01:8000") ||
        expect_eq(h.bus.read(0x818000), 0x11, "LoROM $81:8000 mirror")) return 1;
    h.bus.load_test_rom(rom, bitforge::memory::MapMode::HiROM);
    return expect_eq(h.bus.read(0xC11234), 0x22, "HiROM $C1:1234") ||
           expect_eq(h.bus.read(0x411234), 0x22, "HiROM $41:1234") ||
           expect_eq(h.bus.read(0x019234), 0x33, "HiROM $01:9234") ||
           expect_eq(h.bus.read(0x819234), 0x33, "HiROM $81:9234");
}

static int test_sram_mapping() {
    TestHarness h;
    Bytes rom(0x10000, 0x00);
    h.bus.load_test_rom(rom, bitforge::memory::MapMode::LoROM, 0x2000);
    h.bus.write(0x700010, 0x77);
    if (expect_eq(h.bus.read(0x700010), 0x77, "LoROM SRAM $70:0010") ||
        expect_eq(h.bus.read(0xF00010), 0x77, "LoROM SRAM $F0:0010 mirror")) return 1;
    h.bus.load_test_rom(rom, bitforge::memory::MapMode::HiROM, 0x2000);
    h.bus.write(0x206000, 0x88);
    return expect_eq(h.bus.read(0x206000), 0x88, "HiROM SRAM $20:6000") ||
           expect_eq(h.bus.read(0xA06000), 0x88, "HiROM SRAM $A0:6000 mirror");
}

static int test_header_detection() {
    Bytes rom(0x10000, 0x00);
    rom[0x0000] = 0xA5;
    rom[0x8000] = 0x5A;
    const char name[] = "HIROM HEADER TEST    ";
    std::memcpy(&rom[0xFFC0], name, 21);
    rom[0xFFD5] = 0x21;                    // HiROM
    rom[0xFFD8] = 0x03;                    // 8KB SRAM
    rom[0xFFDC] = 0xFF; rom[0xFFDD] = 0xFF; // complement
    rom[0xFFDE] = 0x00; rom[0xFFDF] = 0x00; // checksum
    rom[0xFFFC] = 0x00; rom[0xFFFD] = 0x80;
    const auto path = (std::filesystem::temp_directory_path() / "snes_header_test.sfc").string();
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    }
    TestHarness h;
    const bool loaded = h.bus.load_rom(path);
    std::filesystem::remove(path);
    if (!loaded) { printf("load failed"); return 1; }
    h.bus.write(0x206000, 0x99);
    return expect_eq(h.bus.read(0x008000), 0x5A, "HiROM $00:8000") ||
           expect_eq(h.bus.read(0xC00000), 0xA5, "HiROM $C0:0000") ||
           expect_eq(h.bus.read(0x206000), 0x99, "SRAM sized from header");
}

static int test_cgram_full_range() {
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    write_cgram(h.bus, 127, 0x1234);
    write_cgram(h.bus, 255, 0x7FFF);       // byte address $1FE: must not wrap onto color 127
    h.bus.write(0x002121, 127);
    const unsigned c127 = static_cast<unsigned>(h.bus.read(0x00213B) | (h.bus.read(0x00213B) << 8));
    h.bus.write(0x002121, 255);
    const unsigned c255 = static_cast<unsigned>(h.bus.read(0x00213B) | (h.bus.read(0x00213B) << 8));
    return expect_eq(c127, 0x1234, "color 127") || expect_eq(c255, 0x7FFF, "color 255");
}

static int test_w12sel_keeps_tm() {
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    ppu.connect_bus(&h.bus);
    write_cgram(h.bus, 1, 0x001F);
    write_4bpp_tile(h.bus, 0x0000, 0xFF, 0x00, 8);
    write_tilemap_entry(h.bus, 0x0400, 0x0000);
    h.bus.write(0x002105, 0x01);
    h.bus.write(0x002107, 0x04);
    h.bus.write(0x00212C, 0x01);           // TM: BG1 on
    h.bus.write(0x002123, 0x00);           // W12SEL must not touch TM
    h.bus.write(0x00210E, 0xFF);
    h.bus.write(0x00210E, 0x03);
    h.bus.write(0x002100, 0x0F);
    ppu.step(2 * CYCLES_PER_SCANLINE);
    return expect_pixel(ppu.get_framebuffer(), 0, 0, 0xFFFF0000, "BG1 still on");
}

// DRAM refresh: 40 master clocks per line at H = 538, charged to the CPU step that reaches it.
static int test_dram_refresh() {
    using bitforge::memory::dram_refresh_stall;
    int fails = 0;
    auto expect = [&](uint64_t got, uint64_t want, const char* what) {
        if (got != want) { printf("\n    %s: got %llu want %llu", what, (unsigned long long)got, (unsigned long long)want); fails++; }
    };
    expect(dram_refresh_stall(0, 6), 0, "short step before H=538");
    expect(dram_refresh_stall(530, 8), 40, "step ending exactly at 538");
    expect(dram_refresh_stall(530, 7), 0, "step ending just before");
    expect(dram_refresh_stall(538, 12), 0, "step starting at 538 already had it");
    expect(dram_refresh_stall(539, 1364), 40, "a whole line from 539 reaches the next one");
    expect(dram_refresh_stall(0, 1364 * 3), 120, "three lines of DMA");
    expect(dram_refresh_stall(0, 3250), 120, "the first two stalls push the step into a third refresh");
    expect(dram_refresh_stall(1300, 602), 40, "across the line boundary");

    // A NOP/BRA loop gets 40 fewer clocks of every 1364 once the PPU tracks the line position.
    auto run = [](bool with_ppu) {
        TestHarness h;
        bitforge::ppu::PPU ppu;
        if (with_ppu) { h.bus.connect_ppu(&ppu); ppu.connect_bus(&h.bus); }
        h.load_test_rom(0x8000, {0xEA, 0x80, 0xFD}, 0x8000);  // loop: NOP; BRA loop
        uint64_t clocks = 0;
        int steps = 0;
        while (clocks < 1364ull * 200) {
            h.bus.begin_cpu_step();
            const int cycles = h.cpu.step();
            const auto c = h.bus.end_cpu_step(cycles);
            if (with_ppu) ppu.step(static_cast<uint32_t>(c));
            clocks += c;
            steps++;
        }
        return steps;
    };
    const int free_steps = run(false), stalled_steps = run(true);
    const double ratio = static_cast<double>(stalled_steps) / free_steps;
    if (ratio < 0.965 || ratio > 0.977) { printf("\n    loop ratio %.4f (want ~%.4f)", ratio, 1324.0 / 1364.0); fails++; }
    return fails;
}

// ---- controllers ---------------------------------------------------------------------------

static int g_ctl_fails = 0;
static void ctl_expect(unsigned got, unsigned want, const char* what) {
    if (got != want) {
        printf("\n    %s: got $%04X want $%04X", what, got, want);
        g_ctl_fails++;
    }
}

// `bits` clocks of one data line (0 = D0, 1 = D1), most significant first.
static unsigned read_bits(bitforge::ControllerPort& port, int bits, bool iobit = true, int line = 0) {
    unsigned word = 0;
    for (int i = 0; i < bits; i++) word = (word << 1) | ((port.clock(iobit) >> line) & 1);
    return word;
}

static int test_controller_devices() {
    using bitforge::ControllerPort;
    using bitforge::Device;
    g_ctl_fails = 0;

    // Gamepad: 16 bits MSB first, then 1s; while latched, the live B button without shifting.
    ControllerPort pad;
    pad.set_pad(0, 0x8421);
    pad.latch(true);
    ctl_expect(read_bits(pad, 4), 0xF, "pad latched: B over and over");
    pad.latch(false);
    ctl_expect(read_bits(pad, 16), 0x8421, "pad report");
    ctl_expect(read_bits(pad, 8), 0xFF, "pad: 1s after the report");
    ctl_expect(pad.clock(true) & 2, 0, "pad: D1 is 0");
    ctl_expect(pad.auto_read(true, false)[0], 0x8421, "pad auto-read");

    ControllerPort none;
    none.set_device(Device::None);
    none.latch(true);
    none.latch(false);
    ctl_expect(read_bits(none, 24), 0, "empty port: all 0, no trailing 1s");

    // Multitap: I/O bit high -> pads 1/2 on D0/D1, low -> pads 3/4; each pair has its own shift.
    ControllerPort tap;
    tap.set_device(Device::Multitap);
    for (int s = 0; s < 4; s++) tap.set_pad(s, static_cast<bitforge::Word>(0x1111 * (s + 1)));
    tap.latch(true);
    ctl_expect(tap.clock(true), 0x02, "multitap latched: D1 high (detection)");
    tap.latch(false);
    ControllerPort copy = tap;
    ctl_expect(read_bits(tap, 16, true, 0), 0x1111, "multitap pad 1 (D0, I/O high)");
    ctl_expect(read_bits(copy, 16, true, 1), 0x2222, "multitap pad 2 (D1, I/O high)");
    copy = tap;
    ctl_expect(read_bits(tap, 16, false, 0), 0x3333, "multitap pad 3 (D0, I/O low)");
    ctl_expect(read_bits(copy, 16, false, 1), 0x4444, "multitap pad 4 (D1, I/O low)");
    ctl_expect(tap.clock(false), 0x03, "multitap: 1s on both lines after the reports");
    const auto words = tap.auto_read(true, false);
    ctl_expect(words[0], 0x1111, "multitap auto-read D0");
    ctl_expect(words[1], 0x2222, "multitap auto-read D1");

    // Mouse: 0x00, R L s1 s0 0001, then Y and X as sign + magnitude; motion resets on latch.
    ControllerPort mouse;
    mouse.set_device(Device::Mouse);
    mouse.add_mouse(3, -1, true, false);
    mouse.add_mouse(2, -2, true, false);  // accumulates: +5, -3
    mouse.latch(true);
    mouse.latch(false);
    ctl_expect(read_bits(mouse, 16), 0x0041, "mouse: left button, speed 0, signature");
    ctl_expect(read_bits(mouse, 16), 0x8305, "mouse: 3 up, 5 right");
    ctl_expect(read_bits(mouse, 4), 0xF, "mouse: 1s after 32 bits");
    mouse.latch(true);
    mouse.clock(true);  // clocking while latched steps the sensitivity
    mouse.latch(false);
    ctl_expect(mouse.mouse_speed(), 1, "mouse speed stepped");
    ctl_expect(read_bits(mouse, 32), 0x00510000, "mouse: speed 1, motion reset by the latch");
    mouse.add_mouse(-10, 400, false, true);
    const auto mw = mouse.auto_read(true, false);
    ctl_expect(mw[0], 0x0091, "mouse auto-read: right button, speed 1");
    ctl_expect(read_bits(mouse, 16), 0x7F8F, "mouse: 1.5x at speed 1, clamped to 127");

    // Super Scope: Fire Cursor Turbo Pause 0 0 Offscreen 0, then 1s. Fire once per press
    // without turbo, every latch with it.
    ControllerPort scope;
    scope.set_device(Device::SuperScope);
    bitforge::ScopeInput aim;
    aim.x = 10;
    aim.y = 20;
    aim.offscreen = false;
    aim.fire = true;
    scope.set_scope(aim);
    ctl_expect(scope.auto_read(true, false)[0], 0x80FF, "scope: fire");
    ctl_expect(scope.auto_read(true, false)[0], 0x00FF, "scope: fire held, no turbo -> once");
    aim.turbo = true;
    scope.set_scope(aim);
    ctl_expect(scope.auto_read(true, false)[0], 0xA0FF, "scope: turbo repeats fire");
    aim = {};
    aim.cursor = true;
    scope.set_scope(aim);
    ctl_expect(scope.auto_read(true, false)[0], 0x42FF, "scope: cursor, offscreen");

    // Changing the device restarts the protocol but keeps the pads.
    pad.set_device(Device::Multitap);
    ctl_expect(pad.pad(0), 0x8421, "pads kept across a device change");
    return g_ctl_fails;
}

static int test_controller_bus() {
    using bitforge::Device;
    g_ctl_fails = 0;
    TestHarness h;
    bitforge::ppu::PPU ppu;
    h.bus.connect_ppu(&ppu);
    ppu.connect_bus(&h.bus);
    h.load_test_rom(0x8000, {0xEA}, 0x8000);

    // Multitap in port 2: auto-read with $4201 bit 7 set gives pads 2 and 3 in $421A/$421E;
    // the game then clears it and reads pads 4 and 5 by hand from $4017 (D0 and D1).
    h.bus.set_pad(0, 0, 0x1000);
    h.bus.set_port_device(1, Device::Multitap);
    for (int s = 0; s < 4; s++) h.bus.set_pad(1, s, static_cast<bitforge::Word>(0x0100 << s));
    h.bus.write(0x004201, 0x80);
    h.bus.write(0x004200, 0x01);
    h.bus.on_vblank_start();
    auto word = [&](bitforge::Address lo) { return static_cast<unsigned>(h.bus.read(lo) | (h.bus.read(lo + 1) << 8)); };
    ctl_expect(word(0x004218), 0x1000, "$4218 pad 1");
    ctl_expect(word(0x00421A), 0x0100, "$421A pad 2 (multitap 1)");
    ctl_expect(word(0x00421E), 0x0200, "$421E pad 3 (multitap 2)");
    ctl_expect(word(0x00421C), 0x0000, "$421C: port 1 D1 is 0 for a gamepad");
    h.bus.write(0x004201, 0x00);
    h.bus.write(0x004016, 1);
    ctl_expect(h.bus.read(0x004017) & 0x1F, 0x1E, "$4017 latched: D1 high, bits 2-4 tied high");
    h.bus.write(0x004016, 0);
    unsigned d0 = 0, d1 = 0;
    for (int i = 0; i < 16; i++) {
        const auto v = h.bus.read(0x004017);
        d0 = (d0 << 1) | (v & 1);
        d1 = (d1 << 1) | ((v >> 1) & 1);
    }
    ctl_expect(d0, 0x0400, "$4017 D0: pad 4 (multitap 3)");
    ctl_expect(d1, 0x0800, "$4017 D1: pad 5 (multitap 4)");

    // Super Scope: the PPU counters latch where it points, only with $4201 bit 7 set.
    h.bus.set_port_device(1, Device::SuperScope);
    bitforge::ScopeInput aim;
    aim.x = 100;
    aim.y = 50;
    aim.offscreen = false;
    h.bus.controller(1).set_scope(aim);
    h.bus.read(0x00213F);  // clear the latch flag
    ppu.step(1364 * 263);
    ctl_expect(h.bus.read(0x00213F) & 0x40, 0x00, "no latch with $4201 bit 7 clear");
    h.bus.write(0x004201, 0x80);
    ppu.step(1364 * 263);
    ctl_expect(h.bus.read(0x00213F) & 0x40, 0x40, "STAT78: counters latched by the scope");
    const unsigned hl = h.bus.read(0x00213C), hh = h.bus.read(0x00213C) & 1;
    const unsigned vl = h.bus.read(0x00213D), vh = h.bus.read(0x00213D) & 1;
    ctl_expect(hl | (hh << 8), 100 + bitforge::kScopeDotOffset, "OPHCT = x + dot offset");
    ctl_expect(vl | (vh << 8), 51, "OPVCT = y + 1");
    aim.offscreen = true;
    h.bus.controller(1).set_scope(aim);
    h.bus.read(0x00213F);
    ppu.step(1364 * 263);
    ctl_expect(h.bus.read(0x00213F) & 0x40, 0x00, "offscreen: no latch");

    // set_port_connected keeps working for plain pads.
    h.bus.set_port_connected(1, false);
    ctl_expect(static_cast<unsigned>(h.bus.controller(1).device()), static_cast<unsigned>(Device::None), "unplugged");
    h.bus.set_port_connected(1, true);
    ctl_expect(static_cast<unsigned>(h.bus.controller(1).device()), static_cast<unsigned>(Device::Gamepad), "plugged");
    return g_ctl_fails;
}
