/*
 * apu.h — SNES Audio Processing Unit (APU).
 *
 * The APU is a semi-independent subsystem containing:
 *   - An SPC700 8-bit CPU running at ~1.024 MHz
 *   - A Sony S-DSP that synthesizes all audio output
 *   - 64 KB of dedicated RAM (shared between SPC700 and DSP)
 *   - 3 hardware timers (two 8kHz, one 64kHz)
 *   - A 64-byte IPL boot ROM (mapped at $FFC0–$FFFF when enabled)
 *
 * Communication with the main 65C816 CPU occurs through 4 bidirectional I/O
 * ports at SPC700 addresses $F4–$F7. The main CPU writes to inPorts; the
 * SPC700 writes to outPorts. Each side reads the other's written values.
 */
#ifndef APU_H
#define APU_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Apu Apu;

#include "spc.h"
#include "dsp.h"

// Hardware timer — the APU has 3 of these.
// Timers 0 and 1 tick once every 128 APU cycles (~8 kHz).
// Timer 2 ticks once every 16 APU cycles (~64 kHz).
// Each timer divides its tick by `target`; when the divider reaches target,
// the 4-bit output counter increments and the divider resets.
typedef struct Timer {
  uint8_t cycles;    // Counts down APU cycles until next timer tick
  uint8_t divider;   // Counts up toward target on each timer tick
  uint8_t target;    // Divider resets when it equals target (0 = 256)
  uint8_t counter;   // 4-bit output counter (0-15), read-and-clear by SPC700
  bool enabled;      // Timer runs only when enabled via register $F1
} Timer;


// Main APU state — owns the SPC700 CPU, DSP, RAM, timers, and I/O ports.
struct Apu {
  Spc* spc;                   // SPC700 processor (executes audio program)
  Dsp* dsp;                   // S-DSP (sample synthesis and mixing)
  uint8_t ram[0x10000];       // 64 KB APU RAM (code, samples, echo buffer)
  bool romReadable;           // When true, IPL boot ROM overlays $FFC0–$FFFF
  uint8_t dspAdr;             // Current DSP register address (set via port $F2)
  uint32_t cycles;            // Total APU cycles elapsed (for timer scheduling)
  uint8_t inPorts[6]; // includes 2 bytes of ram
  uint8_t outPorts[4];        // SPC700→CPU output ports (read by main CPU)
  Timer timer[3];             // The 3 hardware timers
  uint8_t cpuCyclesLeft;      // SPC700 cycles remaining in current opcode
  // DSP register write history — records writes for frame-accurate replay.
  // Padded with a void* to ensure consistent struct alignment.
  union {
    DspRegWriteHistory hist;
    void *padpad;
  };
};

// Alternate APU layout without the write-history union (used for secondary state).
typedef struct Apu2 {
  // Snes* snes;
  Spc* spc;
  Dsp* dsp;
  uint8_t ram[0x10000];
  bool romReadable;
  uint8_t dspAdr;
  uint32_t cycles;
  uint8_t inPorts[6]; // includes 2 bytes of ram
  uint8_t outPorts[4];
  Timer timer[3];
  uint8_t cpuCyclesLeft;
} Apu2;

// Allocate and return a new APU with SPC700 and DSP sub-processors.
Apu* apu_init();
void apu_free(Apu* apu);
// Reset APU to power-on state: clear RAM, reset SPC700/DSP, enable boot ROM.
void apu_reset(Apu* apu);
// Advance the APU by one master cycle: run SPC700 if due, tick DSP every
// 32 cycles, and update all enabled timers.
void apu_cycle(Apu* apu);
// Read from the SPC700's address space (handles I/O ports, DSP, timers, ROM).
uint8_t apu_cpuRead(Apu* apu, uint16_t adr);
// Write to the SPC700's address space (handles I/O ports, DSP, timers, control).
void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val);
// Serialize or deserialize APU state for save/load snapshots.
void apu_saveload(Apu *apu, SaveLoadFunc *func, void *ctx);

#endif
