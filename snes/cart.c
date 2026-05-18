/*
 * cart.c — SNES cartridge memory mapping implementation.
 *
 * Translates 24-bit SNES addresses (bank:offset) into physical ROM/SRAM
 * offsets using either LoROM or HiROM mapping rules:
 *
 * LoROM: ROM at offset $8000-$FFFF (32KB windows), mirrored across banks.
 *        SRAM at banks $70-$7D/$F0-$FF, offset $0000-$7FFF.
 *        Address formula: (bank << 15) | (adr & 0x7FFF)
 *
 * HiROM: ROM mapped contiguously across full 64KB banks ($40+).
 *        SRAM at banks $20-$3F, offset $6000-$7FFF (8KB windows).
 *        Address formula: ((bank & 0x3F) << 16) | adr
 *
 * All ROM/SRAM accesses are masked to power-of-2 sizes for wraparound.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "cart.h"
#include "snes.h"

// Forward declarations for mapping-specific read/write handlers.
static uint8_t cart_readLorom(Cart* cart, uint8_t bank, uint16_t adr);
static void cart_writeLorom(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val);
static uint8_t cart_readHirom(Cart* cart, uint8_t bank, uint16_t adr);
static void cart_writeHirom(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val);

// Allocate a cart with default 8KB SRAM (0x2000 bytes) and no ROM loaded.
Cart* cart_init(Snes* snes) {
  Cart* cart = (Cart *)malloc(sizeof(Cart));
  cart->snes = snes;
  cart->type = 0;       // No mapping type until a ROM is loaded
  cart->rom = NULL;
  cart->romSize = 0;
  cart->ramSize = 0x2000;
  cart->ram = (uint8_t *)malloc(cart->ramSize);
  return cart;
}

void cart_free(Cart* cart) {
  free(cart);
}

void cart_reset(Cart* cart) {
  if(cart->ramSize > 0 && cart->ram != NULL) memset(cart->ram, 0, cart->ramSize); // for now
}

void cart_saveload(Cart *cart, SaveLoadFunc *func, void *ctx) {
  func(ctx, cart->ram, cart->ramSize);
}

// Load a ROM image into the cart. Copies the ROM data, sets the mapping type,
// and clears SRAM. Asserts that ramSize matches the pre-allocated size.
void cart_load(Cart* cart, int type, uint8_t* rom, int romSize, int ramSize) {
  cart->type = type;
  if(cart->rom != NULL) free(cart->rom);
  cart->rom = (uint8_t*)malloc(romSize);
  cart->romSize = romSize;
  assert(ramSize == cart->ramSize);
  memset(cart->ram, 0, ramSize);
  cart->ramSize = ramSize;
  memcpy(cart->rom, rom, romSize);
}

// Dispatch a cartridge read to the appropriate mapping handler.
// LoROM (type 1) gets a fast-path check; HiROM (type 2) falls through the switch.
// Type 0 (no cart) returns open bus.
uint8_t cart_read(Cart* cart, uint8_t bank, uint16_t adr) {
  if (cart->type == 1)
    return cart_readLorom(cart, bank, adr);

  switch(cart->type) {
    case 0: return cart->snes->openBus;
    case 1:
    case 2: return cart_readHirom(cart, bank, adr);
  }
  return cart->snes->openBus;
}

// Dispatch a cartridge write (typically only SRAM is writable).
void cart_write(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val) {
  switch(cart->type) {
    case 0: break;
    case 1: cart_writeLorom(cart, bank, adr, val); break;
    case 2: cart_writeHirom(cart, bank, adr, val); break;
  }
}

// LoROM read: ROM is mapped in 32KB chunks at the upper half ($8000-$FFFF)
// of each bank. SRAM occupies $0000-$7FFF in banks $70-$7D and $F0-$FF.
static uint8_t cart_readLorom(Cart* cart, uint8_t bank, uint16_t adr) {
  if(adr >= 0x8000) {
    // adr 8000-ffff in all banks or all addresses in banks 40-7f and c0-ff
    return cart->rom[((bank << 15) | (adr & 0x7fff)) & (cart->romSize - 1)];
  }
  if(((bank >= 0x70 && bank < 0x7e) || bank >= 0xf0) && adr < 0x8000 && cart->ramSize > 0) {
    // banks 70-7e and f0-ff, adr 0000-7fff
    return cart->ram[(((bank & 0xf) << 15) | adr) & (cart->ramSize - 1)];
  }
  if(bank & 0x40) {
    // adr 8000-ffff in all banks or all addresses in banks 40-7f and c0-ff
    return cart->rom[((bank << 15) | (adr & 0x7fff)) & (cart->romSize - 1)];
  }
  return cart->snes->openBus;
}

// LoROM write: only SRAM is writable (banks $70-$7D/$F0+, offset $0000-$7FFF).
static void cart_writeLorom(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val) {
  if(((bank >= 0x70 && bank < 0x7e) || bank > 0xf0) && adr < 0x8000 && cart->ramSize > 0) {
    // banks 70-7e and f0-ff, adr 0000-7fff
    cart->ram[(((bank & 0xf) << 15) | adr) & (cart->ramSize - 1)] = val;
  }
}

// HiROM read: ROM spans full 64KB banks ($40+). SRAM at $6000-$7FFF in
// banks $00-$3F (and mirrors at $80-$BF). Bank bit 7 is stripped for mirroring.
static uint8_t cart_readHirom(Cart* cart, uint8_t bank, uint16_t adr) {
  bank &= 0x7f;
  if(bank < 0x40 && adr >= 0x6000 && adr < 0x8000 && cart->ramSize > 0) {
    // banks 00-3f and 80-bf, adr 6000-7fff
    return cart->ram[(((bank & 0x3f) << 13) | (adr & 0x1fff)) & (cart->ramSize - 1)];
  }
  if(adr >= 0x8000 || bank >= 0x40) {
    // adr 8000-ffff in all banks or all addresses in banks 40-7f and c0-ff
    return cart->rom[(((bank & 0x3f) << 16) | adr) & (cart->romSize - 1)];
  }
  return cart->snes->openBus;
}

// HiROM write: only SRAM at $6000-$7FFF in banks $00-$3F/$80-$BF is writable.
static void cart_writeHirom(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val) {
  bank &= 0x7f;
  if(bank < 0x40 && adr >= 0x6000 && adr < 0x8000 && cart->ramSize > 0) {
    // banks 00-3f and 80-bf, adr 6000-7fff
    cart->ram[(((bank & 0x3f) << 13) | (adr & 0x1fff)) & (cart->ramSize - 1)] = val;
  }
}
