/*
 * saveload.h — Save-state serialization infrastructure for the SNES emulator.
 *
 * This file defines the visitor-pattern interface used by every subsystem
 * (CPU, APU, PPU, DMA, Cart, etc.) to serialize and deserialize their
 * internal state into a flat byte stream. A single function-pointer type
 * is used for both saving and loading: the concrete implementation provided
 * by the caller determines whether data is copied out of (save) or into
 * (load) each subsystem's fields. This design keeps the save and load code
 * paths perfectly in sync — each subsystem has exactly one saveload function
 * that describes the layout, and the direction is controlled externally.
 */
#pragma once

/*
 * SaveLoadFunc — Callback type for the save/load visitor.
 *
 * Parameters:
 *   ctx       — Opaque context pointer (typically a file handle or memory buffer
 *               cursor) passed through unchanged by each subsystem.
 *   data      — Pointer to the subsystem's data to be saved or loaded.
 *   data_size — Number of bytes to transfer starting at `data`.
 *
 * When saving, the implementation copies `data_size` bytes FROM `data` into the
 * output stream. When loading, it copies `data_size` bytes FROM the input stream
 * INTO `data`. The subsystem code is identical in both cases.
 */
typedef void SaveLoadFunc(void *ctx, void *data, size_t data_size);

/*
 * SL(x) — Convenience macro that serializes/deserializes a single variable.
 *
 * Expands to a call to `func(ctx, &x, sizeof(x))`, eliminating the repetitive
 * address-of and sizeof boilerplate. Requires that the enclosing function has
 * parameters named exactly `func` (SaveLoadFunc*) and `ctx` (void*).
 */
#define SL(x) func(ctx, &x, sizeof(x))