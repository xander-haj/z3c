/*
 * input.c — SNES controller input implementation.
 *
 * Emulates the joypad serial interface. The SNES reads controllers by:
 *   1. Asserting the latch line (captures all 16 button states)
 *   2. Reading bits one at a time via input_read (LSB first, active-low)
 * After all 16 bits are shifted out, subsequent reads return 1 (no button).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "input.h"
#include "snes.h"

// Allocate and initialize a standard joypad (type 1) with no buttons pressed.
Input* input_init(Snes* snes) {
  Input* input = (Input * )malloc(sizeof(Input));
  input->snes = snes;
  // TODO: handle (where?)
  input->type = 1;
  input->currentState = 0;
  return input;
}

void input_free(Input* input) {
  free(input);
}

void input_reset(Input* input) {
  input->latchLine = false;
  input->latchedState = 0;
}

// While the latch line is held high, continuously snapshot the current button state.
// The main CPU asserts this before reading to freeze the button state.
void input_cycle(Input* input) {
  if(input->latchLine) {
    input->latchedState = input->currentState;
  }
}

// Shift out one bit from the latched state (LSB first).
// After each read, a 1 is shifted into the MSB so that once all 16 real
// bits are consumed, all further reads return 1 (matching hardware open-bus).
uint8_t input_read(Input* input) {
  uint8_t ret = input->latchedState & 1;
  input->latchedState >>= 1;
  input->latchedState |= 0x8000;  // Fill MSB with 1 (no button pressed)
  return ret;
}
