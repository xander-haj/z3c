/*
 * zelda_cpu_infra.h — 65C816 CPU Emulation Infrastructure
 *
 * Provides the bridge between the native C reimplementation and the original
 * SNES 65C816 machine code. Some routines in the original ROM were too complex
 * or tightly coupled to decompile cleanly into C, so this module allows the
 * reimplementation to fall back to cycle-accurate emulation of the original
 * opcodes when needed. The emulated CPU operates on its own 128 KB RAM image
 * (g_emulated_ram) which mirrors the SNES memory map, and callers can invoke
 * arbitrary ROM routines by specifying the program counter, register values,
 * and processor flag state.
 *
 * Related files:
 *   zelda_cpu_infra.c — Implementation of the emulation wrapper functions
 *   zelda_rtl.h/c     — Higher-level runtime layer that sits above this
 *   snes/cpu.h/c      — The actual 65C816 CPU core used under the hood
 */
#ifndef ZELDA3_ZELDA_CPU_INFRA_H_
#define ZELDA3_ZELDA_CPU_INFRA_H_
#include "types.h"

/*
 * Emulated SNES RAM — 128 KB (0x20000 bytes).
 * This buffer represents the full SNES Work RAM (WRAM) address space.
 * The native C code and the emulated 65C816 routines share this memory,
 * allowing data to be passed between the two execution models seamlessly.
 * Game state written here by emulated routines is visible to the C code
 * and vice versa.
 */
extern uint8 g_emulated_ram[0x20000];

// --- Memory Access -----------------------------------------------------------

/*
 * GetPtr — Resolve a 24-bit SNES address to a native pointer.
 *
 * Translates a 24-bit SNES address (bank:offset) into a host pointer
 * within g_emulated_ram or the ROM image, applying the appropriate
 * LoROM/HiROM memory mapping rules. This lets native C code directly
 * read and write locations that emulated routines reference by SNES
 * address.
 *
 * @param addr  24-bit SNES address (high byte = bank, low 16 bits = offset)
 * @return      Host pointer to the corresponding byte in emulated memory
 */
uint8 *GetPtr(uint32 addr);

// --- Emulated Function Invocation --------------------------------------------

/*
 * RunEmulatedFuncSilent — Execute a 65C816 routine without trace output.
 *
 * Sets up the emulated CPU registers and processor flags, then runs the
 * 65C816 emulation starting at the given program counter until the routine
 * returns (via RTS/RTL). Identical to RunEmulatedFunc but suppresses any
 * diagnostic or trace logging, making it suitable for high-frequency calls
 * where console noise would be excessive.
 *
 * @param pc         24-bit ROM address to begin execution
 * @param a          Initial value of the A (accumulator) register
 * @param x          Initial value of the X index register
 * @param y          Initial value of the Y index register
 * @param mf         Processor M flag — true = 8-bit accumulator mode
 * @param xf         Processor X flag — true = 8-bit index register mode
 * @param b          Data bank register (DBR) value
 * @param whatflags  Bitmask controlling which registers to preset vs. leave
 */
void RunEmulatedFuncSilent(uint32 pc, uint16 a, uint16 x, uint16 y, bool mf, bool xf, int b, int whatflags);

/*
 * RunEmulatedFunc — Execute a 65C816 routine with trace/debug output.
 *
 * Same semantics as RunEmulatedFuncSilent, but enables trace logging so
 * that every executed instruction can be inspected for debugging purposes.
 * Used during development to verify that emulated routines produce the
 * same results as the original ROM.
 */
void RunEmulatedFunc(uint32 pc, uint16 a, uint16 x, uint16 y, bool mf, bool xf, int b, int whatflags);

// --- Initialization ----------------------------------------------------------

/*
 * EmuInitialize — Load ROM data and prepare the emulation environment.
 *
 * Copies the ROM image into the emulator's address space, initializes the
 * 65C816 CPU core, and sets up the memory map so that subsequent calls to
 * RunEmulatedFunc/RunEmulatedFuncSilent can execute original game code.
 * Must be called once at startup before any emulated routines are invoked.
 *
 * @param data  Pointer to the raw ROM image bytes
 * @param size  Size of the ROM image in bytes
 * @return      true if initialization succeeded, false on failure
 */
bool EmuInitialize(uint8 *data, size_t size);

#endif  // ZELDA3_ZELDA_CPU_INFRA_H_
