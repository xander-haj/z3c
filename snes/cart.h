/*
 * cart.h — SNES cartridge subsystem.
 *
 * Handles ROM and SRAM storage with support for two standard memory mapping
 * schemes: LoROM (type 1) and HiROM (type 2). These determine how the 24-bit
 * SNES address space (bank + offset) maps to physical ROM and RAM addresses.
 *
 * LoROM: ROM mapped in 32KB chunks at offset $8000–$FFFF of each bank.
 *        SRAM at banks $70–$7D, offset $0000–$7FFF.
 * HiROM: ROM mapped contiguously across full 64KB banks starting at bank $40.
 *        SRAM at banks $20–$3F, offset $6000–$7FFF.
 */
#ifndef CART_H
#define CART_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Cart Cart;

#include "snes.h"

struct Cart {
  Snes* snes;          // Back-pointer to the parent SNES system
  uint8_t type;        // Mapping type: 0=none/empty, 1=LoROM, 2=HiROM

  uint8_t* rom;        // Pointer to the loaded ROM image (power-of-2 sized)
  uint32_t romSize;    // ROM size in bytes
  uint8_t* ram;        // Battery-backed SRAM for game saves
  uint32_t ramSize;    // SRAM size in bytes (typically 8KB)
};

// TODO: how to handle reset & load? (especially where to init ram)

// Allocate a new cart with default 8KB SRAM, no ROM loaded yet.
Cart* cart_init(Snes* snes);
// Free the cart (note: ROM is freed separately on reload).
void cart_free(Cart* cart);
void cart_reset(Cart* cart); // will reset special chips etc, general reading is set up in load
void cart_load(Cart* cart, int type, uint8_t* rom, int romSize, int ramSize); // TODO: figure out how to handle (battery, cart-chips etc)
// Read a byte from the cartridge address space using the current mapping scheme.
// Bank and address are decoded according to LoROM or HiROM rules.
uint8_t cart_read(Cart* cart, uint8_t bank, uint16_t adr);
// Write a byte to the cartridge address space (typically only hits SRAM).
void cart_write(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val);
// Serialize or deserialize cart SRAM for save/load snapshots.
void cart_saveload(Cart *cart, SaveLoadFunc *func, void *ctx);

#endif
