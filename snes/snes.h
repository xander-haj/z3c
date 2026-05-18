/*
 * snes.h — Top-level SNES system emulator.
 *
 * The Snes struct ties together all major subsystems: the 65C816 CPU, APU
 * (SPC700 + DSP), PPU, DMA controller, cartridge, and two controller ports.
 * It owns the system bus, handling address decoding across the 24-bit address
 * space and routing reads/writes to the appropriate subsystem.
 *
 * The system also manages frame timing (H/V position counters), interrupt
 * logic (NMI on vblank, H/V IRQ timers), auto-joypad reading, and the
 * hardware multiply/divide registers.
 */
#ifndef SNES_H
#define SNES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Snes Snes;

#include "cpu.h"
#include "apu.h"
#include "dma.h"
#include "ppu.h"
#include "cart.h"
#include "input.h"

// Top-level SNES system state — owns all subsystems and manages the system bus.
struct Snes {
  // -- Subsystem pointers --
  Cpu* cpu;                // 65C816 main processor
  Apu* apu;                // Audio Processing Unit (SPC700 + DSP)
  Ppu* ppu;                // Picture Processing Unit (video rendering)
  Dma* dma;                // DMA / HDMA controller (8 channels)
  Cart* cart;              // Cartridge (ROM + SRAM)
  // -- Input --
  bool debug_cycles;       // When true, print cycle-level debug info
  bool disableHpos;        // When true, H-position checks are skipped (fast-forward)
  Input* input1;           // Controller port 1
  Input* input2;           // Controller port 2

  // -- Frame timing --
  uint16_t hPos;           // Horizontal dot position within current scanline (0-339)
  uint16_t vPos;           // Current scanline number (0-261 NTSC, 0-311 PAL)
  uint32_t frames;         // Total frames rendered since power-on
  // -- CPU scheduling --
  uint8_t cpuCyclesLeft;   // Master cycles remaining for current CPU instruction
  uint8_t cpuMemOps;       // Number of memory operations in current CPU instruction
  double apuCatchupCycles; // Fractional APU cycles accumulated (APU runs at ~1.024 MHz vs ~21 MHz)
  // -- NMI / IRQ interrupt control (registers $4200, $4207-$420A, $4210-$4211) --
  bool hIrqEnabled;        // H-IRQ enabled (fire IRQ at hTimer dot position)
  bool vIrqEnabled;        // V-IRQ enabled (fire IRQ at vTimer scanline)
  bool nmiEnabled;         // NMI enabled on vblank start ($4200 bit 7)
  uint16_t hTimer;         // H-IRQ trigger dot position ($4207-$4208)
  uint16_t vTimer;         // V-IRQ trigger scanline ($4209-$420A)
  bool inNmi;              // Currently inside NMI handler (set on vblank, cleared on read)
  bool inIrq;              // IRQ line is asserted (cleared on acknowledge read $4211)
  bool inVblank;           // True during vertical blanking period
  // -- Auto joypad reading (register $4200 bit 0, results at $4218-$421F) --
  uint16_t portAutoRead[4]; // Auto-read joypad results for ports 1-2 (+ 2 unused extension ports)
  bool autoJoyRead;        // Auto joypad read is enabled
  uint16_t autoJoyTimer;   // Countdown until auto-read completes (~4224 cycles)
  bool ppuLatch;           // PPU H/V counter latch flag (set by reading $4016 / $2137)
  // -- Hardware multiply/divide (registers $4202-$4206, $4214-$4217) --
  uint8_t multiplyA;       // 8-bit multiplicand ($4202)
  uint16_t multiplyResult; // 16-bit multiply result ($4216-$4217)
  uint16_t divideA;        // 16-bit dividend ($4204-$4205)
  uint16_t divideResult;   // 16-bit quotient ($4214-$4215)
  // -- Miscellaneous --
  bool fastMem;            // FastROM enabled ($420D bit 0): 6-cycle ROM access instead of 8
  uint8_t openBus;         // Last value on the data bus (returned for unmapped reads)
  // -- Work RAM --
  uint8_t *ram;            // 128KB WRAM (main system RAM, $7E0000-$7FFFFF)
  uint32_t ramAdr;         // WRAM access port address ($2181-$2183), auto-increments
};

// Allocate a new SNES system using the provided 128KB WRAM buffer.
Snes* snes_init(uint8_t *ram);
// Free the SNES system and all owned subsystems.
void snes_free(Snes* snes);
// Reset the SNES. `hard` true = power-on reset (clear RAM); false = soft reset.
void snes_reset(Snes* snes, bool hard);
// Read from the B-bus (PPU/APU/WRAM registers at $2100-$21FF). Used by DMA and CPU.
uint8_t snes_readBBus(Snes* snes, uint8_t adr);
// Write to the B-bus (PPU/APU/WRAM registers at $2100-$21FF). Used by DMA and CPU.
void snes_writeBBus(Snes* snes, uint8_t adr, uint8_t val);
// Read from the full 24-bit address space (decodes bank+offset to the right subsystem).
uint8_t snes_read(Snes* snes, uint32_t adr);
// Write to the full 24-bit address space (decodes bank+offset to the right subsystem).
void snes_write(Snes* snes, uint32_t adr, uint8_t val);
// CPU-facing read: same as snes_read but also updates open bus and handles access timing.
uint8_t snes_cpuRead(Snes* snes, uint32_t adr);
// CPU-facing write: same as snes_write but also updates open bus and handles access timing.
void snes_cpuWrite(Snes* snes, uint32_t adr, uint8_t val);
// Print a debug trace line showing current CPU state (PC, registers, flags, opcode).
void snes_printCpuLine(Snes *snes);
// Perform the auto-joypad read sequence (reads controller state into $4218-$421F).
void snes_doAutoJoypad(Snes *snes);

// -- Functions defined in snes_other.c --

// Load a ROM image, detect LoROM/HiROM mapping, and initialize the cartridge.
bool snes_loadRom(Snes* snes, uint8_t* data, int length);
// Serialize or deserialize the entire SNES state for save/load snapshots.
void snes_saveload(Snes *snes, SaveLoadFunc *func, void *ctx);


#endif

