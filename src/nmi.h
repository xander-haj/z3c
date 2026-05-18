/*
 * nmi.h - Vblank (NMI) interrupt handler and VRAM DMA transfer routines
 *
 * On the SNES, the Non-Maskable Interrupt (NMI) fires at the start of
 * vertical blank -- the brief window each frame when the PPU is idle and
 * VRAM can safely be written. All graphics uploads (tilemaps, character
 * data, OAM sprites, scroll registers) must occur during this window.
 *
 * This header declares the NMI entry point and every DMA transfer routine
 * it dispatches. The game engine queues drawing changes during the active
 * frame, then the NMI handler batch-uploads them to VRAM before the next
 * scanline begins. Functions are organized into three groups:
 *
 *   1. NMI core -- interrupt entry, joypad reading, update dispatch
 *   2. Tilemap uploads -- BG layer tilemap transfers for various game states
 *   3. Character (CHR) uploads -- tile graphics data for BG and OBJ layers
 */
#pragma once
#include "types.h"

/* ---------------------------------------------------------------------------
 * NMI core -- interrupt entry point and dispatch
 * --------------------------------------------------------------------------- */

// Uploads the first half of the subscreen overlay tilemap during vblank.
// The subscreen overlay is split into two halves to fit within the DMA budget.
void NMI_UploadSubscreenOverlayFormer();

// Uploads the second half of the subscreen overlay tilemap.
void NMI_UploadSubscreenOverlayLatter();

// Main NMI interrupt handler -- the vblank entry point called once per frame.
// |joypad_input| is the raw 16-bit SNES controller state latched at this instant.
// Dispatches all queued VRAM uploads, OAM writes, and scroll register updates.
void Interrupt_NMI(uint16 joypad_input);

// Reads the SNES joypad registers and computes pressed/held state.
// Called at the start of NMI to capture input before the frame's logic runs.
void NMI_ReadJoypads(uint16 joypad_input);

// Central dispatcher that examines the nmi_subroutine_index and calls the
// appropriate tilemap/CHR upload routine for the current game state.
void NMI_DoUpdates();

/* ---------------------------------------------------------------------------
 * Tilemap upload routines
 *
 * Each function transfers a specific tilemap (or portion of one) from RAM
 * to VRAM. The game engine writes tilemap changes into RAM buffers during
 * gameplay, and these functions DMA the results to the PPU during vblank.
 * --------------------------------------------------------------------------- */

// Uploads the main BG tilemap for the current screen.
void NMI_UploadTilemap();

// No-op stub used when no tilemap update is needed this frame.
void NMI_UploadTilemap_doNothing();

// Uploads BG3 text layer tilemap -- used for dialogue boxes and HUD text
// which are rendered on BG3 (the highest-priority background layer).
void NMI_UploadBG3Text();

// Updates overworld scroll registers in the PPU so the camera tracks Link.
void NMI_UpdateOWScroll();

// Transfers the subscreen overlay tilemap used for transparency effects
// (e.g., dark rooms, rain, fog overlays in the Light/Dark World).
void NMI_UpdateSubscreenOverlay();

// Processes an arbitrary run-length-encoded tilemap from |src|, iterating
// from stripe index |i| through |i_end|. Used for custom tilemap patches
// such as chest openings or switch toggles.
void NMI_HandleArbitraryTileMap(const uint8 *src, int i, int i_end);

// Uploads the BG1 wall tilemap layer used in dungeon rooms.
void NMI_UpdateBG1Wall();

// Stub for tilemap update slots that require no action.
void NMI_TileMapNothing();

// Uploads the Light World map tilemap for the map screen / minimap display.
void NMI_UpdateLoadLightWorldMap();

// Uploads the left portion of BG2, used during horizontal scrolling transitions.
void NMI_UpdateBG2Left();

/* ---------------------------------------------------------------------------
 * Character (CHR) data upload routines
 *
 * These transfer tile pixel data (4bpp SNES character format) into VRAM
 * character pages. BG and OBJ layers use separate VRAM regions, and the
 * game swaps character banks depending on the current area and animation.
 * --------------------------------------------------------------------------- */

// Uploads BG character banks 3 and 4 together as a single DMA batch.
void NMI_UpdateBGChar3and4();

// Uploads BG character banks 5 and 6 together.
void NMI_UpdateBGChar5and6();

// Uploads half of a BG character bank -- used when only a partial update is
// needed (e.g., animated water tiles that cycle a subset of the tileset).
void NMI_UpdateBGCharHalf();

// Uploads individual BG character banks 0 through 3.
void NMI_UpdateBGChar0();
void NMI_UpdateBGChar1();
void NMI_UpdateBGChar2();
void NMI_UpdateBGChar3();

// Uploads OBJ (sprite) character banks. OBJ tiles are stored in a separate
// VRAM region from BG tiles. Banks 0, 2, and 3 cover Link's sprites,
// enemy sprites, and weapon/item sprites respectively.
void NMI_UpdateObjChar0();
void NMI_UpdateObjChar2();
void NMI_UpdateObjChar3();

// Executes a DMA transfer of tilemap data to VRAM address |dst|.
// Used as a shared helper by the various tilemap upload routines above.
void NMI_RunTileMapUpdateDMA(int dst);

/* ---------------------------------------------------------------------------
 * Special-purpose uploads
 * --------------------------------------------------------------------------- */

// Uploads the Dark World variant of the overworld map tilemap.
void NMI_UploadDarkWorldMap();

// Uploads the "GAME OVER" text tilemap shown on the death screen.
void NMI_UploadGameOverText();

// Uploads updated tile graphics for the peg puzzle in dungeon rooms
// (the orange/blue pegs that toggle when Link hammers a switch).
void NMI_UpdatePegTiles();

// Uploads animated star tiles used in certain dungeon ceiling effects.
void NMI_UpdateStarTiles();

// Processes Stripe Image data pointed to by |p| and issues the corresponding
// VRAM writes. Stripe Image is the SNES format for sparse tilemap patches --
// a sequence of (VRAM address, length, tile data) tuples.
void HandleStripes14(const uint8 *p);

// Uploads IRQ-triggered graphics, used for mid-frame VRAM updates such as
// the HDMA-driven water surface effect or mode-7 transitions.
void NMI_UpdateIRQGFX();
