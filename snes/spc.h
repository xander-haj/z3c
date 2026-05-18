/*
 * spc.h — SPC700 CPU emulator for the SNES audio subsystem.
 *
 * The SPC700 is an 8-bit processor made by Sony, running at ~1.024 MHz inside
 * the SNES APU. It has its own 64KB address space (shared with the DSP), a
 * register set similar to but distinct from the 6502, and an instruction set
 * optimized for audio control (bit manipulation, direct-page math, 16-bit
 * word moves). It communicates with the main 65C816 CPU through four I/O ports
 * managed by the parent Apu module.
 */
#ifndef SPC_H
#define SPC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Spc Spc;

#include "apu.h"
#include "saveload.h"

// Full register file and execution state of the SPC700 processor.
struct Spc {
  Apu* apu;       // Back-pointer to the parent APU (for memory read/write)
  // registers — all 8-bit except the 16-bit program counter
  uint8_t a;       // Accumulator
  uint8_t x;       // Index register X
  uint8_t y;       // Index register Y
  uint8_t sp;      // Stack pointer (stack lives at 0x0100–0x01FF)
  uint16_t pc;     // Program counter (16-bit, addresses APU's 64KB RAM)
  // flags — stored individually for fast per-opcode access
  bool c;   // Carry
  bool z;   // Zero
  bool v;   // Overflow
  bool n;   // Negative (sign bit)
  bool i;   // Interrupt enable (not used on SNES — no external IRQ line)
  bool h;   // Half-carry (BCD half-carry between nibbles, used by DAA/DAS)
  bool p;   // Direct page selector: 0 = zero page at 0x0000, 1 = at 0x0100
  bool b;   // Break flag (distinguishes BRK from IRQ on stack)
  // stopping
  bool stopped;    // Set by the STOP instruction; only reset can clear it
  // internal use
  uint8_t cyclesUsed; // indicates how many cycles an opcode used
};

// Allocate and return a new SPC700, linked to the given APU for memory access.
Spc* spc_init(Apu* apu);
// Free the SPC700 and its allocated memory.
void spc_free(Spc* spc);
// Reset all registers and flags; reads the reset vector from 0xFFFE/0xFFFF.
void spc_reset(Spc* spc);
// Execute one SPC700 instruction. Returns the number of cycles consumed.
int spc_runOpcode(Spc* spc);
// Serialize or deserialize SPC700 state for save/load snapshots.
void spc_saveload(Spc *spc, SaveLoadFunc *func, void *ctx);

#endif
