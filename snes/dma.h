/*
 * dma.h — SNES DMA (Direct Memory Access) controller.
 *
 * The SNES has 8 DMA channels that support two transfer modes:
 *
 * General-purpose DMA: Triggered by writing to $420B. Transfers a block of
 *   bytes between an A-bus address (CPU address space) and a B-bus address
 *   (PPU/APU register). Halts the CPU for the duration of the transfer.
 *
 * HDMA (Horizontal-blank DMA): Triggered by writing to $420C. Executes a
 *   table-driven transfer each scanline during H-blank, allowing per-scanline
 *   register updates (scroll, color gradient, window, etc.) without CPU
 *   intervention. Each channel reads from a table in ROM/RAM that specifies
 *   repeat counts and data values.
 *
 * Each channel has independent configuration for source, destination, transfer
 * pattern (mode 0-7), address stepping, direction, and indirect addressing.
 */
#ifndef DMA_H
#define DMA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Dma Dma;

#include "snes.h"

// Per-channel DMA/HDMA configuration and runtime state.
typedef struct DmaChannel {
  uint8_t bAdr;       // B-bus destination address (low byte of PPU register)
  uint8_t aBank;      // A-bus source bank byte
  uint8_t indBank; // hdma
  uint8_t repCount; // hdma
  uint16_t aAdr;      // A-bus source address (16-bit within bank)
  uint16_t size; // also indirect hdma adr
  uint16_t tableAdr; // hdma
  uint8_t unusedByte;  // Open-bus byte (readable/writable at register offset 0xB)
  bool dmaActive;      // True while this channel has a general DMA in progress
  bool hdmaActive;     // True if this channel is enabled for HDMA this frame
  // Transfer mode (0-7): determines how many bytes per unit and B-bus offset pattern
  // Mode 0: 1 byte to bAdr. Mode 1: 2 bytes to bAdr, bAdr+1. Mode 2: 2 bytes both to bAdr.
  // Mode 3: 4 bytes to bAdr, bAdr, bAdr+1, bAdr+1. Mode 4: 4 bytes to bAdr..bAdr+3. etc.
  uint8_t mode;
  bool fixed;          // When true, A-bus address does not change after each byte
  bool decrement;      // When true, A-bus address decrements instead of increments
  bool indirect; // hdma
  bool fromB;          // Transfer direction: true = B-bus→A-bus, false = A-bus→B-bus
  bool unusedBit;      // Unused control bit (preserved for open-bus accuracy)
  bool doTransfer; // hdma
  bool terminated; // hdma
  uint8_t offIndex;    // Current index within the transfer mode's byte pattern (0-3)
} DmaChannel;

// DMA controller state — owns all 8 channels and tracks transfer timing.
struct Dma {
  Snes* snes;                 // Back-pointer to parent SNES for bus read/write
  DmaChannel channel[8];      // The 8 DMA/HDMA channels
  uint16_t hdmaTimer;         // Remaining master cycles for current HDMA transfers
  uint32_t dmaTimer;          // Remaining master cycles for current general DMA
  bool dmaBusy;               // True while any general DMA channel is still active
};

// Allocate a new DMA controller linked to the given SNES system.
Dma* dma_init(Snes* snes);
void dma_free(Dma* dma);
// Reset all 8 channels to power-on defaults (all bits set per hardware spec).
void dma_reset(Dma* dma);
uint8_t dma_read(Dma* dma, uint16_t adr); // 43x0-43xf
void dma_write(Dma* dma, uint16_t adr, uint8_t val); // 43x0-43xf
// Execute one unit of general DMA for the first active channel.
void dma_doDma(Dma* dma);
// Initialize HDMA at the start of each frame (load table addresses, first rows).
void dma_initHdma(Dma* dma);
// Execute one scanline of HDMA transfers for all active channels.
void dma_doHdma(Dma* dma);
// Consume one master cycle of DMA/HDMA time. Returns true if DMA is still busy.
bool dma_cycle(Dma* dma);
// Enable channels for DMA or HDMA based on the bitmask `val`.
void dma_startDma(Dma* dma, uint8_t val, bool hdma);
// Serialize or deserialize all DMA state for save/load snapshots.
void dma_saveload(Dma *dma, SaveLoadFunc *func, void *ctx);

#endif
