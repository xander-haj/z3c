/*
 * tracing.h — CPU and SPC700 instruction tracing / disassembly for debugging.
 *
 * These functions format the current processor state into a fixed-width
 * human-readable string containing the program counter, disassembled
 * instruction at PC, all register values, and the processor status flags.
 * Used by the debug-cycle mode to produce per-instruction execution traces.
 */
#ifndef TRACING_H
#define TRACING_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "snes.h"

/*
 * getProcessorStateCpu — Format the 65C816 CPU state into a trace line.
 *
 * Writes a fixed-width string to `line` containing: bank:PC, disassembled
 * opcode, A/X/Y/SP/DP/DB registers, emulation flag, and all status flags
 * (N/V/M/X/D/I/Z/C shown as uppercase when set, lowercase when clear).
 * The caller must provide a buffer of at least 80 characters.
 */
void getProcessorStateCpu(Snes* snes, char* line);

/*
 * getProcessorStateSpc — Format the SPC700 processor state into a trace line.
 *
 * Writes a fixed-width string to `line` containing: PC, disassembled opcode,
 * A/X/Y/SP registers, and all SPC700 flags (N/V/P/B/H/I/Z/C).
 * The caller must provide a buffer of at least 80 characters.
 */
void getProcessorStateSpc(Apu* apu, char* line);

#endif
