/*
 * cpu.h — 65C816 CPU emulator for the SNES.
 *
 * The 65C816 is the main processor of the SNES, a 16-bit extension of the
 * MOS 6502. It supports switchable 8/16-bit accumulator and index registers,
 * a 24-bit address space (16-bit address + 8-bit bank), direct-page addressing,
 * and a 6502-compatible emulation mode. This header declares the CPU register
 * state, status flags, and the public API for initialization, execution, and
 * save/load. The CPU communicates with all other hardware through a memory
 * handler (the parent Snes object) referenced by the opaque `mem` pointer.
 */
#ifndef CPU_H
#define CPU_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "saveload.h"

typedef struct Cpu Cpu;

// Full register file and execution state of the 65C816 processor.
struct Cpu {
  // reference to memory handler, for reading//writing
  void* mem;
  uint8_t memType; // used to define which type mem is
  // registers
  uint16_t a;   // Accumulator (8 or 16 bits depending on mf flag)
  uint16_t x;   // Index register X (8 or 16 bits depending on xf flag)
  uint16_t y;   // Index register Y (8 or 16 bits depending on xf flag)
  uint16_t sp;  // Stack pointer (full 16-bit in native mode, page 1 in emu mode)
  uint16_t pc;  // Program counter (16-bit, combined with bank K for 24-bit addr)
  uint16_t dp; // direct page (D)
  uint8_t k; // program bank (PB)
  uint8_t db; // data bank (B)
  // flags — stored as individual bools for fast access during opcode execution
  bool c;   // Carry flag
  bool z;   // Zero flag (true when last result was zero)
  bool v;   // Overflow flag (signed arithmetic overflow)
  bool n;   // Negative flag (set to MSB of last result)
  bool i;   // Interrupt disable (when set, IRQ is masked)
  bool d;   // Decimal mode (BCD arithmetic for ADC/SBC)
  bool xf;  // Index register size: true=8-bit, false=16-bit
  bool mf;  // Accumulator/memory size: true=8-bit, false=16-bit
  bool e;   // Emulation mode: true=6502 compat (forces mf=xf=1, SP page 1)
  // interrupts
  bool irqWanted;  // Set by external hardware when an IRQ is pending
  bool nmiWanted;  // Set by VBlank logic when NMI should fire
  // power state (WAI/STP)
  bool waiting;  // CPU executed WAI — halted until IRQ or NMI wakes it
  bool stopped;  // CPU executed STP — fully stopped, only reset can resume
  // internal use
  uint8_t cyclesUsed; // indicates how many cycles an opcode used
  uint16_t spBreakpoint;  // Debug: SP value that triggers a breakpoint
  bool in_emu;            // Debug: tracks if CPU is running emulated game code
};

// Allocate and return a new CPU, wired to the given memory handler.
// memType distinguishes the handler type (0 = Snes system).
Cpu* cpu_init(void* mem, int memType);
// Free the CPU and its allocated memory.
void cpu_free(Cpu* cpu);
// Reset all registers and flags to power-on state; reads reset vector from ROM.
void cpu_reset(Cpu* cpu);
// Execute one instruction (or handle a pending interrupt). Returns cycle count.
int cpu_runOpcode(Cpu* cpu);
// Serialize or deserialize CPU state for save/load snapshots.
void cpu_saveload(Cpu *cpu, SaveLoadFunc *func, void *ctx);
// Pack all status flags into a single byte (P register format).
uint8_t cpu_getFlags(Cpu *cpu);
// Unpack a byte into individual status flags, enforcing emulation-mode constraints.
void cpu_setFlags(Cpu *cpu, uint8_t val);

#endif
