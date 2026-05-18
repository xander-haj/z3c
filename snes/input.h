/*
 * input.h — SNES controller input subsystem.
 *
 * Each Input represents one controller port. The SNES reads controllers via
 * a serial shift register protocol: asserting the latch line captures all 16
 * button states into a parallel-load register, then successive reads shift out
 * one bit at a time (active-low). After all 16 bits are shifted out, reads
 * return 1 (open bus / no button pressed). This matches the real hardware
 * behavior of the SNES joypad interface at registers $4016/$4017.
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Input Input;

#include "snes.h"

struct Input {
  Snes* snes;          // Back-pointer to the parent SNES system
  uint8_t type;        // Controller type (1 = standard joypad)
  // latchline
  bool latchLine;      // When high, continuously captures currentState
  // for controller
  uint16_t currentState; // actual state
  uint16_t latchedState; // Captured snapshot being shifted out bit-by-bit
};

// Allocate a new controller input tied to the given SNES system.
Input* input_init(Snes* snes);
// Free the input and its allocated memory.
void input_free(Input* input);
// Reset latch line and latched state to initial values.
void input_reset(Input* input);
// Called each cycle: if latch line is high, recapture currentState.
void input_cycle(Input* input);
// Shift out one bit from the latched state (LSB first). Returns 0 or 1.
// After 16 reads, all remaining reads return 1 (bit 15 is always set).
uint8_t input_read(Input* input);

#endif
