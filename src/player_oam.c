/*
 * player_oam.c - Link's Sprite Rendering and OAM (Object Attribute Memory) Management
 *
 * This file handles all visual rendering of the player character (Link) on screen.
 * It builds OAM entries for Link's body, sword, shield, shadow, and foot-object
 * overlays (grass/water ripples). The bulk of the file consists of large static
 * lookup tables that encode animation frame offsets, tile indices, screen-space
 * positions, priority bits, and palette attributes for every possible combination
 * of Link's facing direction, action state, and equipment.
 *
 * On the SNES, OAM is a 544-byte buffer describing up to 128 hardware sprites.
 * Each OAM entry specifies x/y position, tile index, palette, priority, and
 * flip flags. This file computes those values each frame based on Link's game
 * state and writes them into the OAM buffer for the PPU to render.
 *
 * Key architectural notes:
 * - Animation states are encoded as (yt, rt) pairs: yt selects the animation
 *   category (walking, swimming, sword swing, etc.) and rt selects the frame
 *   within that category.
 * - The kPlayerOamOtherOffs[] table maps (direction * 40 + yt) to a base offset,
 *   which combined with rt indexes into the per-frame data tables.
 * - Sword, shield, and body sprites are rendered as separate OAM entries with
 *   independent tile data, positions, and priority so they layer correctly.
 * - DMA source addresses are computed per-frame to transfer the correct tile
 *   graphics from ROM into VRAM for the PPU.
 *
 * Related files:
 * - player_oam.h: Public declarations for player OAM functions
 * - player.h/player.c: Link's game logic and state machine
 * - variables.h: SNES WRAM variable mappings used throughout
 * - misc.h/misc.c: Contains FindMostSignificantBit and SFX pan helpers
 */

// Player OAM module headers
#include "player_oam.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "snes/snes_regs.h"
#include "player.h"
#include "misc.h"

/*
 * Y-offset table for Link's sprite when traversing stairs.
 * Two groups of 12 entries: first group for submodule 18 (ascending stairs),
 * second for submodule 19 (descending). Within each group, the first 6 entries
 * are for normal stairs and the next 6 for inter-floor staircases (bit 2 of
 * which_staircase_index). Values shift Link's sprite upward to simulate
 * vertical movement on stair tiles.
 */
static const int8 kPlayerOam_StairsOffsY[] = {
  0, -2, -3, 0, -2, -3, 0, 0, 0, 0, 0, 0, 0, -2, -3, 0,
  -2, -3, 0, 0, 0, 0, 0, 0,
};
/* OAM priority bits indexed by link_is_on_lower_level (0-3).
 * Controls BG layer ordering: 0x2000=medium, 0x1000=low, 0x3000=high.
 * Lower dungeon levels use lower priority so Link renders behind upper-level BG tiles. */
static const uint16 kPlayerOam_FloorOamPrio[] = {0x2000, 0x1000, 0x3000, 0x2000};
/* Base offset into the OAM buffer for Link's sprites, indexed by sort_sprites_setting.
 * Two possible starting points prevent Link's OAM entries from conflicting with
 * enemy/NPC sprites that occupy other OAM regions. */
static const uint16 kPlayerOam_SortSpritesOffs[] = {0x190, 0xe0};
/* Cycling walk animation frame sequence for stair traversal (repeats 0,1,2). */
static const int8 kPlayerOam_Tab1[9] = {0, 1, 2, 0, 1, 2, 0, 1, 2};
/* Maps link_item_in_hand bit position to animation category (yt value).
 * Different held items (boomerang, bow, hookshot, etc.) use different pose sets. */
static const int8 kPlayerOam_Tab2[8] = {6, 6, 6, 6, 7, 7, 8, 9};
/* Maps link_position_mode bit position to animation category (yt value).
 * Covers dash, dig, item-use, and other positional override states. */
static const int8 kPlayerOam_Tab3[6] = {12, 11, 32, 34, 35, 37};
/* Maps link_state_bits bit position to animation category (yt value).
 * Handles carry, push, pull, grab, and throw visual states. */
static const int8 kPlayerOam_Tab4[8] = {38, 11, 11, 12, 11, 11, 11, 13};
/* Animation categories (yt values) that require additional sprite overlays
 * (e.g., weapon held overhead, medallion casting, hookshot chain). When
 * the current yt matches one of these, extra sprite banks are loaded via
 * kPlayerOam_Tab6 to render the additional equipment sprites. */
static const uint8 kPlayerOam_Tab5[7] = {4, 16, 18, 21, 23, 24, 39};
/* Sword sparkle/tip tile data per (direction * 9 + swing_frame).
 * Each entry encodes SNES OAM attributes: tile index, palette, priority, and
 * flip flags packed into a 16-bit value. 0xFFFF means no sparkle for that frame.
 * The sparkle appears at the sword tip during enhanced sword swings (sword >= level 2). */
static const uint16 kSwordTipSomething[] = { 0xFFFF, 0xFFFF, 0x6A3E, 0x6A2F, 0x6A2F, 0x2A05, 0x2A2F, 0x2A3E, 0xFFFF, 0xFFFF, 0xFFFF, 0xAA3E, 0xAA2F, 0xAA2F, 0xAA05, 0xEA2F, 0xEA3E, 0xFFFF, 0xFFFF, 0xFFFF, 0x2A3E, 0x2A3F, 0x2A3F, 0x2A05, 0xAA3F, 0xAA3E, 0xFFFF, 0xFFFF, 0xFFFF, 0x6A3E, 0x6A3F, 0x6A3F, 0x6A05, 0xEA3F, 0xEA3E, 0xFFFF };
/* Y-offset of the sword hitbox/sparkle relative to Link's position, indexed by
 * (direction * 9 + swing_frame). -1 indicates no valid hitbox for that frame.
 * These "Good" offsets are used for enhanced swords (level 2+) to position the
 * sword tip sparkle effect and sword hitbox more precisely. */
static const int8 kSwordOamYOffs_Good[] = {-1, -1, -5, -13, -15, -21, -13, -5, -1, -1, -1, 22, 27, 29, 35, 27, 24, -1, -1, -1, -1, 2, 5, 12, 20, 26, -1, -1, -1, -1, 2, 5, 12, 20, 26, -1};
/* X-offset of the sword hitbox/sparkle relative to Link, same indexing as above. */
static const int8 kSwordOamXOffs_Good[] = {-1, -1, 15, 13, 8, -1, -10, -14, -1, -1, -1, -6, -3, 1, 8, 16, 21, -1, -1, -1, -11, -15, -18, -24, -17, -12, -1, -1, -1, 19, 23, 26, 32, 25, 20, -1};
/*
 * Master animation offset table: maps (direction * 40 + animation_category)
 * to a base offset into the per-frame data tables (kSwordOamYOffs, kSwordOamXOffs,
 * kPlayerOam_Main_SwordStuff_array1, etc.). 4 direction groups of 40 entries each
 * (160 total). The animation_category (yt) selects which pose set, and adding the
 * frame index (rt) to the base offset gives the final index into position/tile tables.
 */
static const uint16 kPlayerOamOtherOffs[] = { 0, 36, 72, 96, 108, 118, 122, 134, 36, 146, 154, 178, 185, 188, 212, 236, 252, 264, 272, 288, 300, 316, 339, 363, 374, 390, 396, 402, 408, 420, 421, 422, 424, 428, 444, 456, 468, 469, 470, 475, 9, 45, 78, 99, 108, 119, 125, 137, 45, 148, 160, 178, 185, 194, 218, 236, 255, 266, 276, 291, 304, 316, 345, 363, 378, 393, 399, 402, 411, 420, 421, 422, 425, 432, 447, 456, 468, 469, 470, 484, 18, 54, 84, 102, 108, 120, 128, 140, 54, 150, 166, 178, 182, 200, 224, 236, 258, 268, 280, 294, 308, 316, 351, 363, 382, 390, 396, 402, 414, 420, 421, 422, 426, 436, 450, 456, 468, 469, 470, 493, 27, 63, 90, 105, 108, 121, 131, 143, 63, 152, 172, 178, 185, 206, 230, 236, 261, 270, 284, 297, 312, 316, 357, 363, 386, 393, 399, 402, 417, 420, 421, 422, 427, 440, 453, 456, 468, 469, 470, 502 };
/*
 * Sword OAM Y-offset table (511 entries). Indexed by the combined offset r2
 * computed from (direction, animation_category, frame). Each value is a pixel
 * offset from Link's screen Y position for the sword sprite. -128 (0x80) means
 * no sword sprite is drawn for that frame. Covers all sword swing arcs across
 * all four facing directions and all action states.
 */
static const int8 kSwordOamYOffs[511] = {
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, 9, 5, -2, -6, -8, -5, -3, 7, 9, 11, 15, 21, 25, 27, 25, 23, 13, 11, -2, 2, 3, 12, 12, 15, 22, 27, 27, -2,
  2, 3, 12, 12, 15, 22, 27, 27, -5, -4, -3, -5, -4, -3, 24, 25, 26, 24, 25, 26, 13, 14, 15, 13, 14, 15, 13, 14, 15, 13, 14, 15,
  -3, -7, 2, 20, 26, 24, 10, 13, 15, 10, 13, 15, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -7, -1, -2, -3, 10, 26,
  -2, 3, 14, -2, 3, 14, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -4, -8, 6, 15, 26, 26, 14, 6, -7, -7, 22, 26, 16, 8, 5, 12, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, 24, 16, -5, 16,
  13, -4, -5, -5, -1, -5, -5, -5, -5, 11, 15, 21, 25, 27, 13, -3, -7, 26, 18, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -4, -5, 4, 14, 20, 15, 8, -3, -8, 14, -3, 15, -128, -128, -128, -128, -128, -128, -128, 9, 5, -3, -9, -11,
  -15, -9, -4, 8, 11, 14, 20, 25, 27, 31, 25, 23, 13, -2, -1, 0, 8, 9, 12, 16, 24, 30, -2, -1, 0, 8, 9, 12, 16, 24, 30,
};
/* Sword OAM X-offset table (511 entries). Same indexing as kSwordOamYOffs.
 * -128 means no sword sprite for that frame. */
static const int8 kSwordOamXOffs[511] = {
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, 19, 18, 14, 10, 0, -4, -10, -13, -15, -8, -6, -5, 5, 8, 12, 18, 22, 23, 3, -2, -7, -11, -14, -11, -9, 1, 3, 5,
  10, 15, 19, 22, 19, 17, 7, 5, 0, 0, 0, 0, 0, 0, 7, 7, 7, 7, 7, 7, -10, -10, -10, -10, -10, -10, 18, 18, 18, 18, 18, 18,
  -3, 2, -3, 10, 7, 10, -16, -24, -20, 16, 24, 20, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -2, -2, -2, 10, 10, 10,
  1, -10, -11, 7, 18, 19, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, 13, 6, 22, 22, 8, -1, -14, -14, -1, 9, -5, 3, 18, 21, -11, -12, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, 7, -9, 0, 17,
  22, 14, 10, 10, 14, 11, 8, 8, 8, 23, 22, 20, 12, 8, 23, 14, 10, 12, 12, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128,
  -128, -128, -128, -128, -128, -128, -128, -128, 13, 13, 16, 11, 2, -11, -16, -9, 0, 11, -9, -11, -128, -128, -128, -128, -128, -128, -128, 19, 17, 15, 14, 3,
  -1, -5, -11, -14, -8, -7, -6, 3, 5, 8, 12, 18, 22, 3, -2, -8, -13, -16, -20, -15, -12, 1, 5, 10, 16, 21, 24, 28, 23, 20, 7,
};
/*
 * Sword tile group index table (511 entries, same r2 indexing). Maps each
 * (direction, animation, frame) combination to an index into the sword tile
 * data arrays (kSwordTiledata, kPlayerOam_Main_SwordStuff_array2/3).
 * -1 means no sword is drawn for that frame. Values 0-28 use link_dma_var3
 * for VRAM offsets; values 29+ use link_dma_var5.
 */
static const int8 kPlayerOam_Main_SwordStuff_array1[511] = {
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, 1, 5, 14, 26, 6, 8, 16, 20, 0, 0, 2, 13,
  25, 7, 11, 19, 23, 1, 6, 8, 16, 20, 0, 2, 13, 25, 7, 6,
  10, 18, 22, 1, 4, 15, 27, 7, 10, 10, 10, 10, 10, 10, 9, 9,
  9, 9, 9, 9, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,
  6, 10, 6, 7, 9, 7, 0, 0, 0, 1, 1, 1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, 8, 10, 10, 8, 32, 29, 33, 29, 34, 30,
  31, 37, 38, 31, 35, 36, 42, 39, 39, 41, 40, 40, 44, 42, 42, 45,
  41, 41, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, 49, 49, 50, 47, 47, 48, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 8, 5, 4,
  11, 9, 2, 3, 8, 10, 9, 11, 4, 5, 3, 2, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 3, 3,
  -1, 1, 5, 5, -1, 0, 3, 3, -1, 1, 5, 5, 9, 2, 6, 4,
  4, 10, 6, 6, 10, 6, 8, 8, 8, 1, 4, 15, 11, 7, 4, 10,
  6, 7, 28, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, 51, 52, 53, 54, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 68, 68, 70, 68,
  68, 67, 71, 68, 74, 73, 69, 72, 55, 55, 56, 57, 58, 59, 60, 61,
  62, 63, 64, 65, 6, 75, -1, -1, -1, -1, -1, 1, 5, 14, 26, 10,
  6, 8, 16, 20, 0, 2, 13, 25, 9, 7, 11, 19, 23, 6, 8, 16,
  20, 3, 0, 2, 13, 25, 6, 10, 18, 22, 5, 1, 4, 15, 27,
};
/* Sword extended OAM bit (size/x-bit9) per sword tile group index.
 * 0 = 8x8 sprite, 2 = 16x16 sprite. Controls the SNES OAM size bit. */
static const uint8 kPlayerOam_Main_SwordStuff_array2[76] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0,
  0, 0, 0, 2, 0, 2, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2,
  0, 2, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 0, 0, 0, 0, 2, 0, 2, 0, 2,
};
/* VRAM DMA source selector per sword tile group. Used to load the correct
 * sword tile graphics into VRAM. Low indices use link_dma_var3, high use link_dma_var5. */
static const uint8 kPlayerOam_Main_SwordStuff_array3[76] = {
  6, 6, 4, 4, 4, 4, 0, 0, 8, 8, 8, 8, 2, 2, 2, 2,
  10, 10, 10, 10, 12, 12, 12, 12, 14, 14, 14, 14, 0, 9, 12, 9,
  12, 14, 10, 8, 13, 8, 13, 18, 18, 17, 17, 16, 16, 16, 16, 64,
  65, 64, 65, 24, 24, 25, 25, 36, 33, 37, 35, 34, 32, 38, 35, 37,
  38, 34, 40, 42, 41, 41, 44, 40, 43, 40, 43, 48,
};
/* Alternate VRAM source for rod/cane held items when link_item_in_hand & 5 is set.
 * Replaces the sword VRAM slot with the appropriate rod/cane graphics. */
static const uint8 kPlayerOam_Main_SwordStuff_array4[10] = {1, 4, 1, 4, 6, 2, 0, 5, 0, 5};
/*
 * Shield tile group index table (511 entries, same r2 indexing as sword tables).
 * Maps each animation frame to a shield tile group index. -1 means no shield is
 * drawn. Values 0-7 use link_dma_var4 (small shields), 8+ use link_dma_var5
 * (large shields with alternate VRAM sources).
 */
static const int8 kPlayerOam_ShieldStuff_array1[511] = {
  1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 0, 0, 0, 0, 2, 2, 2, 2, 2, 1, 1, 1,
  1, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3,
  3, 3, 3, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  2, 2, 2, 3, 3, 3, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, 1, 0, 1, 1, 2, 2, 2, 3, 3, 3,
  1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, 1, 2, 0, 3, 3, 1, 2, 1, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2,
  2, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 3, 1, 2,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3, 0, 0,
  3, 0, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, 3, -1, -1, -1, 2, 3, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};
/* VRAM DMA source selector per shield tile group. Controls which shield
 * graphics tiles are loaded into VRAM for the current frame. */
static const uint8 kPlayerOam_ShieldStuff_array2[18] = {
  0, 2, 4, 4, 4, 4, 4, 4, 9, 12, 9, 12, 14, 10, 8, 13,
  8, 13,
};
/* Alternate VRAM source for shield when rod/cane is held (link_item_in_hand & 5). */
static const uint8 kPlayerOam_ShieldStuff_array3[10] = {1, 4, 1, 4, 6, 2, 0, 5, 0, 5};
/*
 * Sprite layout category per animation frame (511 entries). Each value (0-11)
 * selects which OAM slot arrangement to use for the body, sword, shield, and
 * shadow sprites. Different poses require different slot assignments to prevent
 * OAM entries from overlapping or rendering in the wrong order.
 */
static const uint8 kPlayerOamSpriteLocs[511] = {
  2, 2, 2, 2, 2, 2, 2, 2, 2, 10, 10, 10, 10, 10, 10, 10, 10, 10, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 1, 0, 0, 0, 0, 11, 11, 11, 11, 11, 2, 2, 2, 2, 2, 2, 0, 0, 0, 2,
  2, 2, 2, 2, 2, 0, 0, 0, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 3, 3, 6, 6, 6, 5, 5, 5, 5, 5, 5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 0, 0, 0, 3, 3, 6, 6, 6,
  0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3, 2, 0, 2, 0, 2, 2, 2, 2, 2, 2,
  10, 10, 10, 10, 10, 10, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 1, 1, 2, 1, 1, 2, 4, 4, 4, 4,
  4, 4, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 5, 5, 5, 5, 5, 2, 2, 5, 5, 5, 5, 2, 5, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9,
  0, 0, 0, 7, 7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 1,
  1, 1, 1, 1, 1, 3, 3, 10, 10, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 1, 1, 2, 2, 3, 6, 5, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 2, 2,
  0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 2, 2, 0, 2, 0, 5, 2, 0, 0, 0, 0, 0, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 0, 11, 11, 11, 11, 11, 11, 11, 11, 2, 2, 2, 2, 2, 0, 0, 0, 0, 2, 2, 2, 2, 2, 0, 0, 0, 0,
};
/* Base offset into extra sprite bank tables, indexed by (direction * 7 + equipment_type).
 * Used with kPlayerOam_Tab5 to locate additional overlay sprites for items like
 * hookshot chain, held objects, rod beams, etc. 4 direction groups of 7 entries. */
static const uint16 kPlayerOam_Tab6[28] = {0, 10, 22, 38, 61, 72, 88, 0, 13, 26, 38, 61, 76, 97, 0, 16, 30, 38, 61, 80, 106, 0, 19, 34, 38, 61, 84, 115};
/*
 * Extra sprite 1 VRAM bank index (124 entries). Indexed by the combined
 * equipment overlay offset (from kPlayerOam_Tab6 + rt). Each value selects
 * which tile bank to DMA for the first extra sprite. -1 (0xFF) means no
 * extra sprite 1 for that frame. Even values point to one tile row, odd
 * values to the adjacent row (upper/lower 8x8 within a 16x16 cell).
 */
static const int8 kPlayerOam_Spr1Bank[124] = {
  0, -1, -1, 2, 3, 4, -1, -1, -1, -1, -1, 21, 23, -1, 21, 23, -1, 22, 24, -1, 21, 23, 19, 11, 15, -1, 17, 9, 13, -1, 9, 18,
  13, -1, 10, 17, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 4, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 23, 23,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, 23, 23, -1, -1, 21, 21, -1, -1, 24, 24, -1, -1, 23, 23, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 26, 26, 26, -1, -1, -1, -1, -1, -1, 25, 25, 25, -1, -1,
};
/* Extra sprite 2 VRAM bank index (124 entries). Same indexing as Spr1Bank.
 * -1 means no second extra sprite. Used for two-part equipment overlays
 * (e.g., hookshot chain segments, boomerang trail). */
static const int8 kPlayerOam_Spr2Bank[124] = {
  1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 22, 24, -1, 22, 24, -1, -1, -1, -1, -1, -1, 20, 12, 16, -1, 18, 10, 14, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 24, 24,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, 24, 24, -1, -1, 22, 22, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};
/* OAM slot offsets for extra sprite 1, indexed by sprite layout category (r4loc).
 * Tab19B is used when draw_water_ripples_or_grass is active (shifted layout),
 * Tab19A is the default layout. */
static const uint8 kPlayerOam_Tab19B[12] = {20, 28, 8, 16, 16, 20, 28, 16, 8, 8, 8, 20};
static const uint8 kPlayerOam_Tab19A[12] = {0, 8, 0, 8, 8, 12, 20, 8, 8, 0, 0, 0};
/* X-position offsets for extra sprite 1 relative to Link's screen position.
 * -1 means the sprite is not drawn for that frame. */
static const int8 kPlayerOam_Spr1X[124] = {
  8, -1, -1, 4, 4, 4, -1, -1, -1, -1, -1, -7, -9, -1, -8, -10, -1, 13, 16, -1, -5, -8, -2, -6, -5, -1, -1, -5, -6, -1, -3, 4,
  9, -1, 11, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 4, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -5, -8,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -5, -8, -1, -1, -5, -8, -1, -1, 15, 17, -1, -1, -7, -9, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -3, -7, -3, -1, -1, -1, -1, -1, -1, 11, 15, 11, -1, -1,
};
/* Y-position offsets for extra sprite 1 relative to Link's screen position. */
static const int8 kPlayerOam_Spr1Y[124] = {
  0, -1, -1, 8, 8, 8, -1, -1, -1, -1, -1, 7, 10, -1, 5, 8, -1, 8, 12, -1, 8, 12, 2, 7, 13, -1, 20, 14, 7, -1, 20, 21,
  20, -1, 20, 21, 20, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -8, -8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, 11,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, 11, -1, -1, 6, 1, -1, -1, 13, 15, -1, -1, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 12, 12, -1, -1, -1, -1, -1, -1, 12, 12, 12, -1, -1,
};
/* X-position offsets for extra sprite 2 relative to Link's screen position. */
static const int8 kPlayerOam_Spr2X[124] = {
  -8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 15, 17, -1, 16, 18, -1, -1, -1, -1, -1, -1, 10, 14, 13, -1, 9, 14, 14, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 13, 16,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, 13, 16, -1, -1, 13, 16, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};
/* Y-position offsets for extra sprite 2 relative to Link's screen position. */
static const int8 kPlayerOam_Spr2Y[124] = {
  16, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 7, 10, -1, 5, 8, -1, -1, -1, -1, -1, -1, 2, 7, 13, -1, 20, 14, 7, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, 11,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, 11, -1, -1, 6, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};
/* OAM slot offsets for extra sprite 2, same layout-category indexing as Tab19A/B. */
static const uint8 kPlayerOam_Tab20A[12] = {4, 12, 4, 12, 12, 16, 24, 12, 12, 12, 4, 4};
static const uint8 kPlayerOam_Tab20B[12] = {24, 32, 12, 20, 20, 24, 32, 20, 12, 20, 12, 24};
/* Priority/flip attributes for extra sprites, packed as nibble pairs.
 * The bank index selects a byte; even banks use the low nibble, odd banks
 * use the high nibble (shifted left by 4). Bits encode SNES OAM priority
 * and horizontal/vertical flip flags. */
static const uint8 kPlayerOam_Prio[15] = {0x0, 0x0, 0x0, 0x0, 0x40, 0x40, 0x48, 0xc0, 0x48, 0xc0, 0x48, 0xc0, 0x48, 0xc0, 0x40};
/*
 * Sword sprite screen Y-offset table (511 entries). Indexed identically to
 * kSwordOamYOffs but used for the actual drawn sword tile position rather than
 * the hitbox. -1 means no sword drawn at this frame.
 */
static const int8 kDrawSword_y[511] = {
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, 9, 5, -2, -6, -8, -5, -3, -1, 9, 11, 15, 13, 17, 19, 17, 15, 13, 11, -2, 2, 3, 4, 12, 15, 14, 19, 19, -2,
  2, 3, 4, 12, 15, 14, 19, 19, -5, -4, -3, -5, -4, -3, 16, 17, 18, 16, 17, 18, 13, 14, 15, 13, 14, 15, 13, 14, 15, 13, 14, 15,
  -3, -7, 2, 12, 18, 16, 15, 13, 10, 15, 13, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, 3, 6, 6, -8, -3, -3, -3, 10, 18,
  -2, 2, 14, -2, 2, 14, 5, 9, 9, 9, 13, 13, 10, 7, 7, 10, 7, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, -4, 10, 11, -4, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -4, -8, 6, 15, 18, 18, 14, 6, -7, -7, 14, 17, 16, 8, 5, 12, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 8, 6, 10, -1, 8, 6, 10, -1, 8, 6, 10, -1, 8, 6, 10, 16, 16, -5, 16,
  13, -4, -5, -5, -1, -5, -5, -5, -5, 11, 15, 13, 17, 19, 13, -3, -7, 18, 18, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, 2, 17, 12, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, -2, -9, -7,
  -2, 16, 2, -2, 12, 2, -2, 12, -4, -5, 4, 14, 20, 15, 8, -3, -8, 14, -3, 15, -5, 0, -1, -1, -1, -1, -1, 9, 5, -3, -9, -11,
  -15, -9, -4, 0, 11, 14, 12, 17, 19, 23, 17, 15, 13, -2, -1, 0, 0, 9, 12, 16, 16, 19, -2, -1, 0, 0, 9, 12, 16, 16, 19,
};
/* Sword sprite screen X-offset table (511 entries). -1 means no sword drawn. */
static const int8 kDrawSword_x[511] = {
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, 11, 10, 6, 2, 0, -4, -10, -13, -15, -8, -6, -5, -3, 8, 12, 10, 14, 15, 3, -2, -7, -11, -14, -11, -9, -7, 3, 5,
  10, 7, 11, 14, 11, 9, 7, 5, 0, 0, 0, 0, 0, 0, 7, 7, 7, 7, 7, 7, -10, -10, -10, -10, -10, -10, 10, 10, 10, 10, 10, 10,
  -3, 2, -3, 10, 7, 10, -12, -16, -8, 12, 16, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -4, 12, 8, 0, -2, -2, -2, 10, 10, 9,
  1, -10, -11, 7, 10, 11, -2, -5, -5, 9, 2, 2, -2, -3, -3, 2, 11, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -3, -7, 8, 3, 7, 0, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 13, 6, 14, 14, 8, -1, -14, -14, -1, 9, -5, 3, 10, 13, -11, -12, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -14, -13, -12, -1, 14, 13, 12, -1, -14, -13, -12, -1, 14, 13, 12, 7, -9, 0, 9,
  14, 14, 10, 10, 14, 11, 8, 8, 8, 15, 14, 12, 12, 8, 15, 14, 10, 4, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, 4, 4, -7, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -3, -3, 9,
  12, 8, 10, 3, -13, -10, 5, 13, 13, 13, 16, 11, 2, -11, -16, -9, 0, 8, -9, -11, 12, -7, -1, -1, -1, -1, -1, 11, 9, 7, 6, 3,
  -1, -5, -11, -14, -8, -7, -6, -5, 5, 8, 12, 10, 14, 3, -2, -8, -13, -16, -20, -15, -12, -7, 5, 10, 8, 13, 16, 20, 15, 12, 7,
};
/* Palette index for rod items (Fire Rod, Ice Rod, Cane of Byrna).
 * Shifted into bits 9-11 of the OAM attribute word to select the rod's palette. */
static const int8 kPlayerOam_Rod[3] = {0x2, 0x4, 0x4};
/*
 * Sword OAM tile data (76 groups of 3 entries = 228 total). Each group of 3
 * defines the tile attributes for the sword's three OAM entries (arranged as a
 * 2x2 minus one cell: top-left, top-right, bottom-left). 0xFFFF means that cell
 * is not drawn. The 16-bit values encode tile index, palette, priority, and flip
 * flags in SNES OAM format. Groups 0-27 cover normal sword, 28+ cover special
 * items (rod, cane, hammer, etc.).
 */
static const uint16 kSwordTiledata[228] = {
  0x2a05, 0x2a06, 0xffff,
  0x6a06, 0x6a05, 0xffff,
  0xaa05, 0xaa06, 0xffff,
  0x2a05, 0x2a06, 0xffff,
  0xea06, 0xea05, 0xffff,
  0x6a06, 0x6a05, 0xffff,
  0x2a05, 0xffff, 0x2a15,
  0xaa15, 0xffff, 0xaa05,
  0x2a05, 0xffff, 0x2a15,
  0xaa15, 0xffff, 0xaa05,
  0x6a05, 0xffff, 0x6a15,
  0xea15, 0xffff, 0xea05,
  0x2a05, 0xffff, 0xffff,
  0xaa05, 0xffff, 0xffff,
  0x6a05, 0xffff, 0xffff,
  0xea05, 0xffff, 0xffff,
  0x2a05, 0xffff, 0xffff,
  0xaa05, 0xffff, 0xffff,
  0x6a05, 0xffff, 0xffff,
  0xea05, 0xffff, 0xffff,
  0x2a05, 0xffff, 0xffff,
  0xaa05, 0xffff, 0xffff,
  0x6a05, 0xffff, 0xffff,
  0xea05, 0xffff, 0xffff,
  0x2a05, 0xffff, 0xffff,
  0xaa05, 0xffff, 0xffff,
  0x6a05, 0xffff, 0xffff,
  0xea05, 0xffff, 0xffff,
  0xaa15, 0xffff, 0xffff,
  0x2209, 0xffff, 0x2219,
  0x2209, 0xffff, 0x2219,
  0x221a, 0xffff, 0x2219,
  0xa219, 0xffff, 0xa209,
  0x2209, 0xffff, 0x2219,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2219, 0x2209, 0xffff,
  0x6209, 0xffff, 0xffff,
  0x6209, 0x6219, 0xffff,
  0xa209, 0xe209, 0xffff,
  0x2209, 0x6209, 0xffff,
  0x6209, 0xffff, 0xe209,
  0x2209, 0xffff, 0xa209,
  0xa209, 0xffff, 0xffff,
  0x6209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0xe209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x6209, 0xffff, 0xffff,
  0x6209, 0xffff, 0xffff,
  0x221a, 0xffff, 0xffff,
  0x221a, 0xffff, 0xffff,
  0x221a, 0xffff, 0xffff,
  0x221a, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0xe209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0xffff,
  0x2209, 0xffff, 0x2219,
  0x2209, 0xffff, 0x2219,
  0x2209, 0xffff, 0x2219,
  0x6209, 0xffff, 0x6219,
  0x2209, 0xffff, 0x2219,
  0x2209, 0xffff, 0xffff,
  0xa219, 0xa209, 0xffff,
  0x6209, 0xffff, 0xffff,
  0xe209, 0xe219, 0xffff,
  0x2809, 0xffff, 0xffff,
};
/* Shield sprite screen X-offset table (511 entries, same r2 indexing).
 * -1 means no shield drawn at this frame. */
static const int8 kShieldStuff_x[511] = {
  5, 5, 5, 5, 5, 5, 5, 5, 5, -4, -4, -4, -4, -4, -4, -4, -4, -4, -8, -8, -8, -8, -8, -8, -8, -8, -8, 8, 8, 8, 8, 8,
  8, 8, 8, 8, 6, 6, 8, 8, 10, 10, 10, 10, 10, -5, -5, -7, -7, -10, -10, -10, -10, -10, 1, 1, 1, 1, 0, 0, 0, 0, 0, -1,
  -1, -1, -1, 0, 0, 0, 0, 0, 9, 9, 9, 9, 9, 9, -9, -9, -9, -9, -9, -9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  10, 10, 10, -10, -10, -10, 0, -1, 0, 0, 1, 0, -4, -1, -1, -1, -1, -1, -1, -1, -1, -1, 8, -4, 2, -3, 9, 9, 9, -10, -10, -10,
  0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, 9, -4, -10, 0, 0, 8, 0, 5, 5, 5, 5, 5, 5,
  -4, -4, -4, -4, -4, -4, -8, -8, -8, -8, -8, -8, 8, 8, 8, 8, 8, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -4, -8, 5, 8,
  -4, -4, -4, -4, -5, -5, -5, -5, -5, -10, -10, -10, -10, -10, -5, -5, -7, -4, -4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 9, 9, 9, -10, -10, -10, -1, -1,
  -1, -1, -1, -1, -6, -1, -1, -1, 10, -10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 10, 10, 10, 10,
  10, 10, 10, 10, -9, -9, -9, -9, -10, -10, -10, -10, -10, 1, 1, 1, 2, 2, 2, 1, 2, 2, -1, -1, -1, -2, -2, -2, -1, -2, -2,
};
/* Shield sprite screen Y-offset table (511 entries). -1 = no shield drawn. */
static const int8 kShieldStuff_y[511] = {
  5, 5, 4, 3, 5, 5, 4, 3, 5, 9, 10, 9, 7, 8, 10, 9, 7, 8, 5, 5, 4, 3, 4, 5, 4, 3, 4, 5, 5, 4, 3, 4,
  5, 4, 3, 4, 12, 12, 8, 8, 6, 6, 6, 6, 6, 1, 1, 3, 3, 7, 7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
  5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 7, 5, 6, 7, 6, 7, 8, 6, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  7, 5, 7, 7, 8, 7, 5, 5, 5, 5, 5, 5, 16, -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, 8, 7, 7, 6, 5, 5, 7, 7, 7,
  5, 5, 5, 5, 5, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 6, 6, 11, 7, 4, 8, 4, 8, 4, 5, 6, 4, 5, 6,
  10, 11, 12, 10, 11, 12, 5, 6, 7, 5, 6, 7, 5, 6, 7, 5, 6, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 5, 4, 5,
  10, 10, 10, 10, 10, 10, 10, 10, 10, 7, 7, 7, 7, 7, 10, 10, 1, 10, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, 5, 5, 7, 7, 7, -1, -1,
  -1, -1, -1, -1, 9, -1, -1, -1, 5, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 8, 8, 6, 6, 4,
  2, 5, 6, 6, 1, 1, 4, 4, 6, 8, 6, 6, 6, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
};
/* OAM slot offsets for shield sprites, indexed by layout category (r4loc).
 * _1 variant for water/grass overlay mode, _0 for default mode. */
static const uint8 kShieldStuff_oam_index_ptrs_1[12] = {36, 20, 36, 8, 32, 36, 20, 36, 36, 36, 16, 28};
static const uint8 kShieldStuff_oam_index_ptrs_0[12] = {28, 0, 28, 0, 24, 28, 12, 28, 36, 28, 8, 8};
/*
 * Shield OAM tile data (18 groups of 3 entries). Same format as kSwordTiledata:
 * each group defines 3 OAM cells. Groups 0-7 are small shield (Fighter's Shield),
 * groups 8-17 are medium/large shields (Red/Mirror Shield) with 16x16 tiles.
 * 0xFFFF cells are not drawn.
 */
static const uint16 kShieldStuff_OamData[54] = {
  0x2a07, 0xffff, 0xffff,
  0x2a07, 0xffff, 0xffff,
  0x2a07, 0xffff, 0xffff,
  0x6a07, 0xffff, 0xffff,
  0x2a07, 0xffff, 0xffff,
  0x6a07, 0xffff, 0xffff,
  0x2a07, 0xffff, 0xffff,
  0x6a07, 0xffff, 0xffff,
  0x2809, 0xffff, 0x2819,
  0x2809, 0xffff, 0x2819,
  0x281a, 0xffff, 0x2819,
  0xa819, 0xffff, 0xa809,
  0x2809, 0xffff, 0x2819,
  0x2809, 0xffff, 0xffff,
  0x2809, 0xffff, 0xffff,
  0x2819, 0x2809, 0xffff,
  0x6809, 0xffff, 0xffff,
  0x6809, 0x6819, 0xffff,
};
/* OAM slot offsets for Link's body sprites (head + torso), indexed by layout
 * category. _0 is default, _1 is for water/grass overlay mode. */
static const uint8 kLinkBody_oam_index_0[12] = {20, 28, 8, 16, 0, 20, 24, 0, 16, 4, 16, 28};
static const uint8 kLinkBody_oam_index_1[12] = {28, 36, 16, 24, 8, 28, 36, 8, 16, 12, 24, 36};
/*
 * Body graphics index per animation frame (511 entries). Maps each r2 offset
 * to an index into kLinkSpriteBodys (for OAM position/flags) and into the
 * DMA source tables in NMI_PrepareSprites (for VRAM tile loading). This is
 * the master mapping from game state to which body sprites are displayed.
 */
static const uint16 kLinkDmaGraphicsIndices[511] = {
  0, 174, 175, 176, 177, 178, 179, 180, 181, 5, 182, 183, 184, 185, 182, 186, 187, 188, 10, 10, 189, 190, 191, 192, 193, 194, 195, 13, 13, 196, 197, 198,
  199, 200, 201, 202, 16, 16, 17, 17, 18, 18, 19, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 27, 28,
  28, 29, 29, 30, 30, 31, 31, 31, 32, 33, 34, 32, 35, 36, 37, 38, 39, 37, 40, 41, 42, 43, 44, 42, 43, 44, 45, 46, 47, 45, 46, 47,
  49, 48, 50, 52, 51, 52, 54, 53, 55, 57, 56, 58, 59, 60, 61, 62, 62, 62, 0, 13, 5, 10, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72,
  73, 74, 75, 76, 77, 78, 0, 33, 116, 5, 117, 118, 42, 26, 119, 45, 30, 120, 163, 164, 165, 166, 167, 26, 168, 30, 0, 206, 207, 0, 162, 36,
  5, 208, 209, 5, 210, 211, 10, 212, 213, 10, 212, 213, 13, 214, 215, 13, 214, 215, 125, 126, 127, 128, 83, 84, 85, 86, 87, 88, 89, 90, 91, 89,
  92, 93, 94, 95, 96, 94, 97, 98, 99, 100, 101, 99, 100, 101, 102, 103, 104, 102, 103, 104, 32, 33, 34, 32, 35, 36, 37, 38, 39, 37, 40, 41,
  42, 43, 44, 42, 43, 44, 45, 46, 47, 45, 46, 47, 105, 106, 107, 107, 108, 108, 109, 109, 13, 13, 110, 111, 112, 113, 114, 115, 216, 217, 217, 218,
  219, 219, 220, 221, 221, 222, 223, 223, 142, 143, 144, 145, 146, 147, 148, 149, 152, 150, 151, 150, 155, 153, 154, 153, 158, 156, 157, 156, 161, 159, 160, 159,
  150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 171, 171, 172, 172, 169, 169, 170, 170, 171, 171, 172, 172, 169, 169, 170, 170, 37, 42, 32, 45,
  107, 173, 173, 173, 107, 203, 203, 203, 203, 204, 204, 204, 166, 166, 107, 203, 94, 205, 205, 224, 225, 226, 224, 227, 228, 229, 230, 231, 229, 232, 233, 234,
  235, 236, 234, 235, 236, 237, 238, 239, 237, 238, 239, 257, 279, 279, 279, 279, 240, 241, 255, 94, 219, 255, 257, 279, 279, 279, 260, 280, 280, 280, 263, 281,
  281, 281, 266, 282, 282, 282, 245, 246, 247, 242, 243, 244, 251, 252, 253, 248, 249, 250, 5, 10, 0, 13, 272, 273, 0, 33, 116, 5, 117, 118, 42, 26,
  119, 45, 30, 120, 274, 275, 276, 277, 18, 22, 26, 30, 283, 284, 283, 285, 286, 287, 286, 288, 289, 290, 289, 290, 291, 292, 291, 292, 111, 293, 294, 106,
  203, 72, 113, 99, 26, 295, 102, 30, 105, 203, 107, 10, 10, 109, 109, 13, 13, 112, 114, 110, 203, 297, 299, 300, 301, 302, 63, 16, 16, 79, 79, 294,
  80, 294, 19, 19, 20, 20, 21, 21, 81, 82, 81, 23, 23, 24, 24, 25, 130, 131, 132, 133, 134, 134, 28, 28, 29, 121, 122, 123, 124, 129, 129,
};
/*
 * Per-frame body sprite descriptor. Encodes the relative pixel offsets and tile
 * attribute byte for Link's two body OAM entries (head and torso).
 *
 * y, x: Signed pixel offsets from Link's base screen position.
 * tile: Packed attribute byte. Bits 0-3 encode tile row offset. Bit 2 (0x04)
 *       selects the secondary tile row. Bit 6 (0x40) sets horizontal flip.
 *       0xFF means the head tile is hidden (used for face-down poses). The
 *       upper nibble (0xF0) at 0x0F selects a special full-body tile.
 */
typedef struct LinkSpriteBody {
  int8 y, x;
  uint8 tile;
} LinkSpriteBody;
/*
 * Body sprite table (303 entries). Each entry defines the head/torso tile
 * arrangement for one animation frame. Indexed by kLinkDmaGraphicsIndices[r2].
 * The y and x fields shift the body sprite, while the tile field controls
 * tile selection and horizontal flip. Covers all Link poses: walking in 4
 * directions, sword swinging, swimming, carrying, falling, bunny form, etc.
 */
static const LinkSpriteBody kLinkSpriteBodys[303] = {
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x04},
  { 2,  0, 0x04},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x04},
  { 2,  0, 0x04},
  { 0,  1, 0x44},
  { 1,  1, 0x44},
  { 2,  2, 0x44},
  { 0, -1, 0x00},
  { 1, -1, 0x00},
  { 2, -2, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  {-1,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 1,  0, 0x00},
  { 1,  0, 0x00},
  { 1,  1, 0x44},
  { 1,  1, 0x44},
  { 1,  0, 0x44},
  { 1,  1, 0x44},
  { 1, -1, 0x00},
  { 1, -1, 0x00},
  { 1,  0, 0x00},
  { 1, -1, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x04},
  { 2,  0, 0x04},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 0,  1, 0x44},
  { 1,  1, 0x44},
  { 2,  1, 0x44},
  { 0, -1, 0x00},
  { 1, -1, 0x00},
  { 2, -1, 0x00},
  {-1,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x00},
  { 2, -1, 0x44},
  { 1,  1, 0x44},
  { 1,  1, 0x44},
  { 2,  1, 0x00},
  { 1, -1, 0x00},
  { 1, -1, 0x00},
  { 0, -8, 0x00},
  { 4,  0, 0x0f},
  { 4,  0, 0x0f},
  { 0,  0, 0xff},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x44},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  {-1,  0, 0x00},
  {-1,  0, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 0,  0, 0x44},
  { 1,  0, 0x44},
  { 1,  0, 0x44},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 1,  0, 0x00},
  {-1,  0, 0x00},
  {-5,  0, 0x00},
  { 2,  0, 0x00},
  { 5,  0, 0x00},
  {-1,  0, 0x44},
  { 0,  0, 0x44},
  { 0,  1, 0x44},
  {-1,  0, 0x00},
  { 0,  0, 0x00},
  { 0, -1, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x04},
  { 2,  0, 0x04},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x04},
  { 2,  0, 0x04},
  { 0,  1, 0x44},
  { 1,  1, 0x44},
  { 2,  1, 0x44},
  { 0, -1, 0x00},
  { 1, -1, 0x00},
  { 2, -1, 0x00},
  { 1,  0, 0x04},
  { 0,  0, 0x44},
  { 0,  0, 0x00},
  { 0,  1, 0x44},
  { 0,  0, 0x00},
  { 0,  0, 0x04},
  { 0,  0, 0x44},
  { 0,  1, 0x40},
  { 0,  2, 0x40},
  { 0, -1, 0x00},
  { 0, -2, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  1, 0x44},
  { 0, -1, 0x00},
  { 1,  1, 0x00},
  { 2,  1, 0x00},
  { 2,  4, 0x00},
  { 2,  1, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 1,  0, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 1, -1, 0x44},
  { 2, -1, 0x44},
  { 2, -4, 0x44},
  { 2, -1, 0x44},
  { 1,  0, 0x44},
  { 0,  0, 0x00},
  { 0,  0, 0x40},
  { 0,  0, 0x04},
  { 0,  0, 0x04},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x04},
  { 1,  0, 0x00},
  { 2,  0, 0x04},
  { 5,  1, 0x40},
  { 6,  1, 0x44},
  { 5, -1, 0x04},
  { 6, -1, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x04},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x44},
  { 0,  0, 0x00},
  {13,  3, 0x44},
  {12,  5, 0x44},
  {12,  5, 0x44},
  {13, -3, 0x00},
  {12, -5, 0x00},
  {12, -5, 0x00},
  { 1,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 0,  0, 0x44},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x44},
  { 0,  0, 0x44},
  {-1,  0, 0x04},
  { 0,  0, 0x00},
  {-1,  0, 0x00},
  {-2,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  {-1,  0, 0x00},
  {-2,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x04},
  {-1,  0, 0x00},
  {-2,  0, 0x00},
  { 0,  0, 0x00},
  {-1,  0, 0x04},
  {-2,  0, 0x04},
  { 0,  0, 0x04},
  {-1,  1, 0x44},
  {-1,  0, 0x44},
  { 0,  1, 0x44},
  { 0,  1, 0x44},
  {-1,  1, 0x44},
  {-1,  0, 0x44},
  { 0,  0, 0x44},
  {-1, -1, 0x00},
  {-1,  0, 0x00},
  { 0, -1, 0x00},
  { 0, -1, 0x00},
  {-1, -1, 0x00},
  {-1,  0, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x04},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x04},
  { 2,  0, 0x04},
  { 1,  1, 0x44},
  { 2,  1, 0x44},
  { 1, -1, 0x00},
  { 2, -1, 0x00},
  { 2,  0, 0x00},
  { 2,  0, 0x00},
  { 3,  0, 0x00},
  { 3,  0, 0x00},
  { 2, -2, 0x44},
  { 2,  1, 0x44},
  { 2,  2, 0x00},
  { 2, -1, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x04},
  { 2,  0, 0x04},
  { 2,  0, 0x00},
  { 3,  0, 0x00},
  { 4,  0, 0x00},
  { 3,  0, 0x04},
  { 4,  0, 0x04},
  { 0,  0, 0x44},
  { 1,  0, 0x44},
  { 2,  0, 0x44},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 3,  0, 0x00},
  { 2,  0, 0x00},
  {-1,  0, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  {-1,  0, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  {-1,  0, 0x44},
  { 0,  0, 0x44},
  { 1,  0, 0x44},
  {-1,  0, 0x44},
  { 0,  0, 0x44},
  { 1,  0, 0x44},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  0, 0x00},
  { 2,  0, 0x00},
  { 0,  0, 0x00},
  { 3,  0, 0x00},
  { 4,  0, 0x00},
  { 2,  0, 0x00},
  { 0, -1, 0x44},
  { 1,  1, 0x44},
  { 0,  1, 0x44},
  { 0,  1, 0x00},
  { 1,  1, 0x00},
  { 0, -1, 0x00},
  { 3,  0, 0x00},
  { 2,  0, 0x04},
  { 3,  0, 0x04},
  { 0,  2, 0x00},
  { 8,  8, 0x00},
  { 0,  0, 0x00},
  { 0,  0, 0x00},
  {-1,  0, 0x0f},
  { 1,  0, 0x00},
  { 0,  0, 0x04},
  { 0,  0, 0x00},
  { 2,  0, 0x00},
  { 1,  4, 0x44},
  { 1, -4, 0x00},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 1,  0, 0x04},
  { 0,  0, 0x00},
  { 1,  0, 0x00},
  { 1,  0, 0x04},
  { 0,  1, 0x44},
  { 1,  1, 0x44},
  { 0, -1, 0x00},
  { 1, -1, 0x00},
  { 0,  0, 0x44},
  {-2,  0, 0x00},
  { 0, -2, 0x04},
  { 0,  0, 0x00},
  { 0,  1, 0x00},
  { 0,  0, 0x04},
  {12,  0, 0x08},
  {14,  0, 0x80},
  {12,  0, 0x00},
  {11,  0, 0x00},
};
/* OAM slot offsets for sword sprites, indexed by layout category (r4loc).
 * _1 for water/grass overlay mode, _0 for default mode. */
static const uint8 kSwordStuff_oam_index_ptrs_1[12] = {0, 0, 24, 32, 24, 0, 0, 24, 24, 24, 32, 0};
static const uint8 kSwordStuff_oam_index_ptrs_0[12] = {8, 16, 16, 24, 16, 0, 0, 16, 24, 16, 24, 16};
/* Thrown-item scatter sprite tables. When Link throws an item that breaks,
 * these define up to 4 fragment sprites per throw animation phase.
 * -1 means no fragment at that position. Indexed by (link_var30e * 4 + i). */
static const int8 kPlayerOam_DrawOam_Throwing_State[16] = {-1, -1, -1, -1, 0, -1, -1, -1, 0, 0, -1, -1, 0, 0, 0, -1};
static const int8 kPlayerOam_DrawOam_Throwing_X[16] = {-1, -1, -1, -1, 8, -1, -1, -1, 8, 5, -1, -1, 8, 5, 2, -1};
static const int8 kPlayerOam_DrawOam_Throwing_Y[16] = {-1, -1, -1, -1, 14, -1, -1, -1, 14, 22, -1, -1, 14, 22, 30, -1};
/* Direction offset adjustment per shield type. Higher-tier shields (Red/Mirror)
 * use a wider offset base because they are larger (16x16 vs 8x8 tiles). */
static const uint8 kShieldTypeToOffs[4] = {4, 4, 8, 8};
/* Shadow/foot-object X and Y offsets per (direction + shield_size_offset).
 * The shadow sits below Link and shifts slightly based on facing direction
 * and equipped shield size. The Y values of 16-18 place the shadow at
 * Link's feet, about one tile below his center. */
static const int8 kOffsToShadowGivenDir_X[12] = {0, 0, -1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
static const int8 kOffsToShadowGivenDir_Y[12] = {16, 16, 17, 17, 16, 16, 16, 16, 18, 18, 18, 18};
/* OAM slot offsets for shadow sprites per layout category.
 * _1 for water/grass overlay, _0 for default. Shadow uses 2 consecutive
 * 8x8 OAM entries placed side by side to form a wider ellipse. */
static const uint8 kShadow_oam_indexes_1[12] = {12, 12, 0, 0, 0, 12, 12, 0, 0, 0, 0, 12};
static const uint8 kShadow_oam_indexes_0[12] = {40, 40, 40, 40, 40, 40, 40, 40, 0, 40, 40, 40};
static const uint16 kLinkShadows_Chardata[22] = {0x286c, 0x686c, 0x2828, 0x6828, 0x2838, 0xffff, 0x286e, 0x686e, 0x287e, 0x687e, 0x24d8, 0x64d8, 0x24d9, 0x64d9, 0x24da, 0x64da, 0x22c8, 0x62c8, 0x22c9, 0x62c9, 0x22ca, 0x62ca};
static const uint8 kPlayerOam_DrawOam_2X[3] = {0, 0, 4};
/*
 * PlayerOam_WantInvokeSword — Decide whether the sword sprite should
 * be drawn this frame.
 *
 * The decision tree:
 *   - In a "special" handler state (Ether/Bombos/Quake/Spin*) the
 *     sword is always drawn (those states own the sword pose), so
 *     skip the early-return branches and fall through to the type
 *     test at the bottom.
 *   - link_state_bits, link_force_hold_sword_up, link_electrocute_on_touch:
 *     suppress the sword (stunned, holding aloft via separate code,
 *     electrified — those have their own renderers).
 *   - link_item_in_hand bit 0x40 (powder): no sword while throwing
 *     powder.
 *   - link_position_mode bit 0x3d (using cane/byrna/etc.) or
 *     link_item_in_hand bits 0x93 (rod-type items): force sword draw
 *     because the item's render reuses the sword OAM slots.
 *   - Otherwise sword is drawn only while the B button is held
 *     (button_mask_b_y bit 0x80).
 *
 * Final test: sword is only drawn if the sword type is set and not
 * the sentinel 0xff. (link_sword_type + 1) & 0xfe is non-zero for
 * types 1/2/3/4 but zero for 0 and 0xff.
 */
bool PlayerOam_WantInvokeSword() {
  if (link_player_handler_state != kPlayerState_Ether &&
      link_player_handler_state != kPlayerState_Bombos &&
      link_player_handler_state != kPlayerState_Quake &&
      link_player_handler_state != kPlayerState_SpinAttacking &&
      link_player_handler_state != kPlayerState_SpinAttackMotion &&
      !link_state_bits && !link_force_hold_sword_up && !link_electrocute_on_touch) {
    if (link_item_in_hand & 0x40)
      return false;
    if (link_position_mode & 0x3d || link_item_in_hand & 0x93)
      return true;
    if (!(button_mask_b_y & 0x80))
      return false;
  }
  return ((link_sword_type + 1) & 0xfe) != 0;
}

/*
 * CalculateSwordHitBox — Compute the (x, y) offset of the sword tip
 * relative to Link's body for the current swing frame, used by the
 * collision system to test sword/sprite intersections.
 *
 * Returns early when there is no sword (type 0) or the sword has not
 * been picked up yet (type 0xff).
 *
 * Two-tier resolution:
 *   - Master Sword and above (type >= 2) during the first 9 frames
 *     of a swing: try the precision "Good" tables which encode the
 *     finer hitbox geometry of the higher-tier swords. The
 *     kSwordTipSomething[i] sentinel of 0xff means "no precision
 *     entry, fall through to the basic table".
 *   - Otherwise: index the basic kSwordOamX/YOffs via
 *     kPlayerOamOtherOffs, picking row 39 (default frame) or row 3
 *     (post-swing frames 10+). Frame 9 is the spin-attack pause and
 *     gets no hitbox.
 *
 * Writes into player_oam_x_offset / player_oam_y_offset which are
 * later read by the collision pipeline to project the hitbox.
 */
void CalculateSwordHitBox() {  // 879e63
  if (link_sword_type == 0 || link_sword_type == 0xff)
    return;
  if (link_sword_type >= 2 && button_b_frames < 9) {
    int i = button_b_frames + ((link_direction_facing>>1) * 9);
    if ((uint8)kSwordTipSomething[i] != 0xff) {
      player_oam_y_offset = kSwordOamYOffs_Good[i];
      player_oam_x_offset = kSwordOamXOffs_Good[i];
      return;
    }
  }
  uint8 offs = button_b_frames;
  if (offs == 9)
    return;
  uint8 y = 39;
  if (offs >= 10) {
    offs -= 10;
    y = 3;
  }
  int i = kPlayerOamOtherOffs[(link_direction_facing >> 1) * 40 + y] + offs;
  player_oam_y_offset = kSwordOamYOffs[i];
  player_oam_x_offset = kSwordOamXOffs[i];
}

/*
 * LinkOam_Main — The master "draw Link this frame" routine. Run once
 * per frame after the player AI updates and before the global OAM
 * sort/upload.
 *
 * Output: writes Link's body, sword, shield, shadow, and (in some
 * states) thrown-item fragments into oam_buf at the slot positions
 * dictated by the layout tables.
 *
 * Pose resolution (the giant "set yt and rt" cascade):
 *   yt = pose category (selects into kPlayerOam* tables).
 *   rt = animation step within that category.
 * These two are derived from a long priority-ordered chain of state
 * checks: asleep > force-hold-sword > bunny > stair-walk > grabbing
 * wall > dragging > swimming > picking-up > pre-jump > aux-state
 * (tornado/electrocute/etc.) > near-pit > state-bits flags >
 * master-sword-just-picked > item-in-hand > position-mode > spell
 * cast (Quake/Ether/Bombos) > spin-attack > B-held swing.
 *
 * Each branch writes (yt, rt) and jumps to `continue_after_set:`,
 * which then:
 *   1. Computes (r2, r4loc) into the pose tables.
 *   2. Optionally reserves a sprite-attached banking slot for the
 *      torch/sparkle/etc. via kPlayerOam_Tab5/6.
 *   3. Renders the sword (PlayerOam_WantInvokeSword path) into 1-3
 *      OAM slots, with palette overrides for fire/ice rod and the
 *      Cane of Byrna's blue tint.
 *   4. Renders the shield (LinkOam_SetEquipmentVRAMOffsets path).
 *   5. Renders the shadow / "foot object" (water ripple / grass
 *      cluster) under Link, or the falling-into-pit shadow.
 *   6. Renders Link's body sprites from kLinkSpriteBodys.
 *   7. Applies hide-Link conditioning: doorway clipping, blink
 *     timer, full invisibility (status 12), or cape mode.
 *
 * Stair offset hack: submodules 18/19 (climbing/descending stairs)
 * temporarily nudge link_y_coord by kPlayerOam_StairsOffsY[t] for
 * the duration of this function so Link visually follows the stair
 * shape; the original is restored at the end via y_coord_backup.
 *
 * The widescreen-fix branch in the hide-Link block sets oam[i].y to
 * 0xf0 instead of using the legacy "ext-OAM bits set" trick because
 * the legacy approach truncates Link wrong in widescreen modes.
 */
void LinkOam_Main() {  // 8da18e
  uint16 y_coord_backup = link_y_coord;

  /* Stair-walking pose: t indexes the stair Y-offset table by
   * (climbing vs descending) + (left vs right stairs) + animation step. */
  if (submodule_index == 18 || submodule_index == 19) {
    int t = submodule_index == 18 ? 0 : 12;
    t += which_staircase_index & 4 ? 6 : 0;
    t += (link_animation_steps < 6) ? link_animation_steps : 0;
    link_y_coord += kPlayerOam_StairsOffsY[t];
  }

  /* Convert world coords to screen coords by subtracting the BG2
   * scroll. The 0x80 sentinel for player_oam_*_offset means "no
   * sword hitbox computed yet"; the sword path overwrites these. */
  uint8 xcoord = link_x_coord - BG2HOFS_copy2;
  uint8 ycoord = link_y_coord - BG2VOFS_copy2;
  player_oam_x_offset = player_oam_y_offset = 0x80;
  /* scratch_0_var picks between the water/grass overlay layout
   * tables (suffix _1) and the default tables (suffix _0). */
  uint8 scratch_0_var = (draw_water_ripples_or_grass != 0);
  oam_priority_value = kPlayerOam_FloorOamPrio[link_is_on_lower_level];
  sort_sprites_offset_into_oam_buffer = kPlayerOam_SortSpritesOffs[(uint8)sort_sprites_setting];

  uint8 yt, rt;

  /* Top-priority overrides — these states render Link in a fully
   * scripted pose so they short-circuit the rest of the cascade. */
  if (link_player_handler_state == kPlayerState_AsleepInBed && link_pose_during_opening != 2) {
    yt = 0x1f, rt = link_pose_during_opening;
    goto continue_after_set;
  }
  if (link_force_hold_sword_up) {
    yt = 0x24, rt = 0;
    link_direction_facing_mirror = link_direction_facing;
    goto continue_after_set;
  }
  if (link_is_bunny_mirror) {
    yt = 0x21, rt = link_animation_steps & 3;
    link_direction_facing_mirror = link_direction_facing;
    goto continue_after_set;
  }
  /* Default starting yt: 10 if Link is in water/grass, else 0
   * (normal walking pose group). */
  yt = draw_water_ripples_or_grass ? 10 : 0;

  /* Submodule 14 + horizontal velocity = walking up/down stairs.
   * Pick the up-stairs (0x1a) or down-stairs (0x19) variant from the
   * which_staircase_index bit. Vertical-only stair pose (facing 4 or
   * 6) reuses the standard animation step for `rt`. */
  if (submodule_index == 14 && main_module_index != 18 && ((yt = 10), link_actual_vel_x)) {
    if (link_direction_facing != 4 && link_direction_facing != 6) {
      rt = kPlayerOam_Tab1[link_animation_steps];
      yt = which_staircase_index & 4 ? 0x1a : 0x19;
    } else {
      rt = link_animation_steps;
    }
  } else {
    if (link_grabbing_wall & 3) {
      /* Sideways wall-grab pose. */
      yt = 0x18, rt = some_animation_timer_steps;
    } else {
      /* Dragging-block pose: bitmask_of_dragstate flags the drag
       * direction. Clamp the animation step to 0..4. */
      if (bitmask_of_dragstate & 0xd) {
        yt = 0x16;
        if (link_animation_steps >= 5)
          link_animation_steps = 0;
      }
      rt = link_animation_steps;
    }
  }
  link_direction_facing_mirror = link_direction_facing;
  /* Deep-water swimming gets a higher OAM priority so Link doesn't
   * disappear behind the water surface. */
  if (link_is_in_deep_water)
    oam_priority_value = 0x2000;

  /* Swim cycle: 0x11 = idle paddling, 0x13 = directional paddle when
   * a face button is pressed or a swim-collision is active, 0x12 =
   * fast-swim charge animation (link_maybe_swim_faster - 1 picks the
   * stage of the charge ring). */
  if (link_player_handler_state == kPlayerState_Swimming) {
    yt = 0x11, rt &= 1;
    if (submodule_index == 0 && (joypad1H_last & 0xf) != 0 || (swimcoll_var7[0] | swimcoll_var7[1]))
      yt = 0x13, rt = byte_7E02CC;
    if (link_maybe_swim_faster)
      yt = 0x12, rt = link_maybe_swim_faster - 1;
    goto continue_after_set;
  }
  /* Item-receive pose: 0x1d for "single-handed" items, 0x1e for the
   * two-handed master sword overhead pose. */
  if (link_pose_for_item) {
    rt = 0, yt = (link_pose_for_item != 2) ? 0x1d : 0x1e;
    goto continue_after_set;
  }
  /* "Reading sign / book" pose. */
  if (player_unk1 & 1) {
    yt = 0x1b, rt = some_animation_timer_steps;
    goto continue_after_set;
  }

  /* Auxiliary-state cluster (drowning/jumping/being-hurt):
   *   state 4: paddle in deep water — animate via the 4-frame
   *            kSwimmingTab1 picked by the upper bits of frame_counter.
   *   state 1: airborne / hurt:
   *     - Turtle Rock state has its own priority bump.
   *     - Hookshot/cape modes leave Link's pose alone (their own
   *       renderer takes over).
   *     - electrocute_on_touch shows the zap animation (0x14).
   *     - Plain hurt freezes Link (yt = 5). */
  if (link_auxiliary_state != 0) {
    if (link_auxiliary_state == 4) {
      yt = 0x13, rt = kSwimmingTab1[(frame_counter & 0x18) >> 3];
      goto continue_after_set;
    } else if (link_auxiliary_state == 1) {
      if (link_player_handler_state == kPlayerState_TurtleRock) {
        if (!byte_7E034E)
          oam_priority_value = 0x3000;
        goto link_state_is_empty;
      } else if (link_player_handler_state != kPlayerState_Hookshot && !link_cape_mode) {
        if (link_electrocute_on_touch)
          yt = 0x14, rt = player_handler_timer & 3;
        else
          yt = 5, rt = 0;
        goto continue_after_set;
      }
    }
  }

  /* Falling-into-pit stages 2 and 3 use yt = 4 (the falling pose);
   * stage 3 also clears the sort-sprites offset so Link draws below
   * any sprites in the same OAM block. The priority bump at rt >= 6
   * is the bottom-of-pit phase (Link disappears behind the floor). */
  if (player_near_pit_state != 0 && player_near_pit_state != 1) {
    if (player_near_pit_state == 3)
      sort_sprites_offset_into_oam_buffer = 0;
    yt = 4, rt = link_this_controls_sprite_oam;
    if (rt >= 6)
      oam_priority_value |= 0x3000;
    goto continue_after_set;
  }

  /* link_state_bits is a bitmask of "currently active special poses"
   * (carrying, throwing, lifting, talking, dashing, etc.). The most-
   * significant set bit picks one via kPlayerOam_Tab4. Bits < 6 are
   * "carrying" variants and force the direction mirror to south so
   * Link always faces the camera while carrying. yt >= 0xd are
   * lifting/throwing variants which can be swapped to the second
   * "release" frame via link_picking_throw_state. */
  if (link_state_bits != 0) {
    uint8 bit = FindMostSignificantBit(link_state_bits);
    if (bit < 6)
      link_direction_facing_mirror = 2;
    yt = kPlayerOam_Tab4[bit];
    if (yt >= 0xd) {
      if (link_picking_throw_state & 2)
        yt += 1;

      if (link_picking_throw_state & 1)
        yt = 0x10;
      else if (link_state_bits & 0x80)
        goto continue_after_set;
    }
    rt = some_animation_timer_steps;
    goto continue_after_set;
  }
link_state_is_empty:
  /* "Just got the master sword" overlay pose (zero-indexed timer). */
  if (link_unk_master_sword != 0) {
    yt = 0x17, rt = link_unk_master_sword - 1;
    goto continue_after_set;
  }

  /* Item-in-hand vs position-mode pose lookup. Both use MSB-of-mask
   * indexing into their respective tables (kPlayerOam_Tab2/Tab3) and
   * use player_handler_timer for the animation step. */
  if (link_item_in_hand != 0) {
    yt = kPlayerOam_Tab2[FindMostSignificantBit(link_item_in_hand)];
    rt = player_handler_timer;
    goto continue_after_set;
  } else if (link_position_mode != 0) {
    yt = kPlayerOam_Tab3[FindMostSignificantBit(link_position_mode)];
    rt = player_handler_timer;
    goto continue_after_set;
  }

  /* Spell-cast poses: 0x15 for the medallion trio (Quake/Ether/
   * Bombos), 0xf for the spin-attack pose. state_for_spin_attack is
   * the rotation step within the animation. */
  if (link_player_handler_state == kPlayerState_Quake || link_player_handler_state == kPlayerState_Ether || link_player_handler_state == kPlayerState_Bombos) {
    yt = 0x15, rt = state_for_spin_attack;
    goto continue_after_set;
  } else if (link_player_handler_state == kPlayerState_SpinAttackMotion || link_player_handler_state == kPlayerState_SpinAttacking) {
    yt = 0xf, rt = state_for_spin_attack;
    goto continue_after_set;
  }

  /* B-button held: select the swing-frame group:
   *   button_b_frames == 9: brief pause (yt = 2).
   *   < 9: forward swing (yt = 0x27).
   *   >= 10: recoil (yt = 3 with rt offset). */
  if (button_mask_b_y & 0x80) {
    if (button_b_frames == 9) {
      yt = 2;
    } else {
      yt = 0x27, rt = button_b_frames;
      if (rt >= 9) {
        yt = 3;
        rt -= 10;
      }
    }
  }
continue_after_set:
  /* Stash the resolved pose category for downstream readers; the
   * shadow renderer uses oam_priority_value_2 (saved before yt == 5
   * so the "stunned" pose doesn't lower the shadow priority). */
  value_computed_for_player_oam = yt;
  if (yt != 5)
    oam_priority_value_2 = oam_priority_value;

  BYTE(index_of_interacting_tile) = rt;

  int dir = link_direction_facing >> 1;

  /* r2 = flat index into the per-direction pose tables.
   * r4loc = which OAM-slot layout to use (water/grass aware). */
  int r2 = kPlayerOamOtherOffs[dir * 40 + yt] + rt;
  int r4loc = kPlayerOamSpriteLocs[r2];

  /* Palette-swap flag flips between Link's normal palette (0xe00)
   * and the alternate palette (0) used during recolor sequences. */
  link_palette_bits_of_oam = palette_swap_flag ? 0 : 0xe00;
  link_dma_var1 = link_dma_var2 = 0;

  /* Some pose categories need extra "decoration" sprites beyond
   * Link's body — e.g. the bow/hookshot in hand, the flute notes
   * over Link's head, the rod tip. kPlayerOam_Tab5 holds the 7
   * categories that need this; xt is the category index, j is the
   * concrete sprite-table slot for the active direction + animation
   * step. Each sprite has its own VRAM bank (kPlayerOam_Spr1Bank /
   * Spr2Bank) and X/Y offsets. */
  int xt = FindInByteArray(kPlayerOam_Tab5, yt, 7);
  if (xt >= 0) {
    int j = kPlayerOam_Tab6[xt + dir * 7] + rt;
    scratch_1 = j;
    {
      uint8 bank1 = kPlayerOam_Spr1Bank[j];
      if (bank1 != 0xff) {
        link_dma_var1 = bank1 * 2;
        int oam_pos = ((scratch_0_var ? kPlayerOam_Tab19B : kPlayerOam_Tab19A)[r4loc] + sort_sprites_offset_into_oam_buffer) >> 2;
        uint8 zt = ((int16)link_z_coord >= 0 || BYTE(link_z_coord) < 0xf0) ? BYTE(link_z_coord) : 0;
        oam_buf[oam_pos].y = kPlayerOam_Spr1Y[j] + ycoord - zt;
        oam_buf[oam_pos].x = kPlayerOam_Spr1X[j] + xcoord;
        uint16 q = WORD(kPlayerOam_Prio[bank1 >> 1]);
        q = (bank1 & 1) ? q << 4 : q;
        WORD(oam_buf[oam_pos].charnum) = (q & 0xc000) | oam_priority_value | link_palette_bits_of_oam | 4;
        bytewise_extended_oam[oam_pos] = 0;
      }
    }

    uint8 bank2 = kPlayerOam_Spr2Bank[j];
    if (bank2 != 0xff) {
      link_dma_var2 = bank2 * 2;
      int oam_pos = ((scratch_0_var ? kPlayerOam_Tab20B : kPlayerOam_Tab20A)[r4loc] + sort_sprites_offset_into_oam_buffer) >> 2;
      uint8 zt = ((int16)link_z_coord >= 0 || BYTE(link_z_coord) < 0xf0) ? BYTE(link_z_coord) : 0;
      oam_buf[oam_pos].y = kPlayerOam_Spr2Y[j] + ycoord - zt;
      oam_buf[oam_pos].x = kPlayerOam_Spr2X[j] + xcoord;
      uint16 q = WORD(kPlayerOam_Prio[bank2 >> 1]);
      q = (bank2 & 1) ? q << 4 : q;
      WORD(oam_buf[oam_pos].charnum) = (q & 0xc000) | oam_priority_value | link_palette_bits_of_oam | 0x14;
      bytewise_extended_oam[oam_pos] = 0;
    }
  }
  SwordResult sr;

  /* Sword / weapon rendering branch:
   *   bit 4 of link_picking_throw_state: render the unused
   *     "throwing breakable" effect (small fragment cloud).
   *   else: if the sword is wanted AND the per-frame VRAM offsets
   *     resolve to a valid slot, render the sword + sparkle tip. */
  if (link_picking_throw_state & 4) {
    LinkOam_UnusedWeaponSettings(r4loc, xcoord, ycoord);
  } else if (PlayerOam_WantInvokeSword() && !LinkOam_SetWeaponVRAMOffsets(r2, &sr)) {
    uint8 zcoord = ((int16)link_z_coord >= 0 || BYTE(link_z_coord) < 0xf0) ? BYTE(link_z_coord) : 0;
    uint8 oam_y = kDrawSword_y[r2] + ycoord - zcoord;
    uint8 oam_x = kDrawSword_x[r2] + xcoord;

    /* Latch the player_oam_*_offset to the canonical sword positions
     * unless an in-hand "tool" (rod/hammer) is mid-swing; the rod
     * branch keeps the previous offset so its custom hitbox stands. */
    if ((link_item_in_hand & 2) ? (player_handler_timer == 2 && link_delay_timer_spin_attack == 15) : ((link_item_in_hand & 5) == 0)) {
      player_oam_y_offset = kSwordOamYOffs[r2];
      player_oam_x_offset = kSwordOamXOffs[r2];
    }
    /* Rod-in-hand palette swap (fire/ice rod). The selected rod
     * indexes the per-rod palette table, then shifts into the OAM
     * palette field. Cane of Byrna gets a dedicated 0x400 (blue). */
    uint16 oam_pal = 0;
    if (link_item_in_hand & 5) {
      assert(link_state_bits == 0);
      oam_pal = kPlayerOam_Rod[eq_selected_rod - 1] << 8;
    }
    if ((link_position_mode & 8) && current_item_y == 13)
      oam_pal = 0x400;  // cane of byrna

    int oam_pos = ((scratch_0_var ? kSwordStuff_oam_index_ptrs_1 : kSwordStuff_oam_index_ptrs_0)[r4loc] + sort_sprites_offset_into_oam_buffer)>>2;
    oam_pos = LinkOam_CalculateSwordSparklePosition(oam_pos, xcoord, ycoord);

    /* Sword sprites are 3 8x8 tiles (top-left, top-right, bottom).
     * Each kSwordTiledata entry encodes the tile id + flip flags;
     * 0xffff means "skip this slot". The triangle x stride traces
     * a 2x2 layout: i=0 top-left, i=1 top-right, i=2 bottom-row.
     * After i=1 we step back 16px and down 8px to land on the
     * bottom row. The xt distance test sets the wrap-around bit
     * for sprites that crossed the screen seam in widescreen modes. */
    int j = sr.r6 * 3;
    for (int i = 0; i != 3; i++, j++) {
      uint16 td = kSwordTiledata[j];
      if (td != 0xffff) {
        td = (td & ~0x3000) | oam_priority_value;
        if ((td & 0xe00) != 0x200 && !link_palette_bits_of_oam)
          td = (td & ~0xe00) | 0x600;
        if (oam_pal)
          td = (td & ~0xe00) | oam_pal;
        WORD(oam_buf[oam_pos].charnum) = td;
        oam_buf[oam_pos].x = oam_x;
        oam_buf[oam_pos].y = oam_y;
        uint16 xt = (uint8)xcoord - oam_x;
        if ((int16)xt < 0) xt = -xt;
        bytewise_extended_oam[oam_pos] = sr.r12 | (xt >= 0x80);
        oam_pos++;
      }
      oam_x += 8;
      if (i == 1)
        oam_x -= 16, oam_y += 8;
    }
  }

  /* Shield rendering: only when Link has a shield AND has reached
   * sram_progress_indicator >= 1 (intro complete, shield can be
   * shown). Same 3-tile structure as the sword. */
  //SwordStuff_fail
  if (link_shield_type && sram_progress_indicator && !LinkOam_SetEquipmentVRAMOffsets(r2, &sr)) {
    uint8 zcoord = ((int16)link_z_coord >= 0 || BYTE(link_z_coord) < 0xf0) ? BYTE(link_z_coord) : 0;
    uint8 oam_y = kShieldStuff_y[r2] + ycoord - 1 - zcoord;
    uint8 oam_x = kShieldStuff_x[r2] + xcoord;

    LinkOam_CalculateXOffsetRelativeLink(kShieldStuff_x[r2]);

    uint16 oam_pal = (link_palette_bits_of_oam >> 8) ? 0xa00 : 0x600;

    int oam_pos = ((scratch_0_var ? kShieldStuff_oam_index_ptrs_1 : kShieldStuff_oam_index_ptrs_0)[r4loc] + sort_sprites_offset_into_oam_buffer)>>2;

    int j = sr.r6 * 3;
    for (int i = 0; i != 3; i++, j++) {
      uint16 td = kShieldStuff_OamData[j];
      if (td == 0xffff)
        continue;
      td = (td & 0xc1ff) | oam_pal | oam_priority_value;
      WORD(oam_buf[oam_pos].charnum) = td;
      WORD(oam_buf[oam_pos].x) = oam_x | oam_y << 8;
      bytewise_extended_oam[oam_pos] = sr.r12 | bit9_of_xcoord;
      oam_x += 8;
      if (i == 1)
        oam_x -= 16, oam_y += 8;
    }
  }

  /* Shadow / under-link decoration. Skipped when Link is invisible
   * (status 12) or asleep in bed (no shadow during the cutscene). */
  if (link_visibility_status != 12 && link_player_handler_state != kPlayerState_AsleepInBed) {
    if (value_computed_for_player_oam != 5 && draw_water_ripples_or_grass) {
      /* Walking through water/grass: animated splash/leaves at feet. */
      LinkOam_DrawFootObject(r4loc, xcoord, ycoord);
    } else if (link_auxiliary_state != 4 && link_player_handler_state != kPlayerState_Swimming) {
      if (player_near_pit_state != 0 && player_near_pit_state != 1) {
        /* Mid-pit-fall: shrinking shadow at the bottom of the pit. */
        if (link_this_controls_sprite_oam >= 6) {
          LinkOam_DrawDungeonFallShadow(r4loc, xcoord);
          r4loc = 2; // wtf
        }
      } else {
        // draw shadow
        /* Standard 2-cell ellipse shadow. shadow_idx selects between
         * the "in-air" and "on-ground" shadow sprite pairs in
         * kLinkShadows_Chardata; cape mode keeps the on-ground one. */
        int shadow_idx = (link_auxiliary_state != 0) && (link_auxiliary_state != 1 || !link_cape_mode);
        uint16 oam_y = link_y_coord - BG2VOFS_copy2 + kOffsToShadowGivenDir_Y[link_direction_facing_mirror >> 1];
        if (oam_y < 256) {
          uint8 oam_x = xcoord + kOffsToShadowGivenDir_X[link_direction_facing_mirror >> 1];
          int oam_pos = ((scratch_0_var ? kShadow_oam_indexes_1 : kShadow_oam_indexes_0)[r4loc] + sort_sprites_offset_into_oam_buffer)>>2;

          uint16 td = kLinkShadows_Chardata[shadow_idx*2] & ~0x3000 | oam_priority_value_2;
          if (!link_palette_bits_of_oam)
            td = td & ~0xe00 | 0x600;
          WORD(oam_buf[oam_pos+0].charnum) = td;
          WORD(oam_buf[oam_pos+1].charnum) = td & ~0xC000 | 0x4000;
          WORD(oam_buf[oam_pos+0].x) = (uint8)oam_x | oam_y << 8;
          WORD(oam_buf[oam_pos+1].x) = (uint8)(oam_x + 8) | oam_y << 8;
          bytewise_extended_oam[oam_pos+0] = 0;
          bytewise_extended_oam[oam_pos+1] = 0;
        }
      }
    }
  }

  /* Body sprite block. Two 16x16-equivalent OAM entries per Link:
   * the upper body (head + torso) at oam_pos and lower body (legs)
   * at oam_pos+1. The sp->tile field is a packed two-tile id; the
   * 0xf000 sentinel skips the upper or lower half independently
   * (e.g., when Link is half-submerged in water). */
  {
    int oam_pos = ((scratch_0_var ? kLinkBody_oam_index_1 : kLinkBody_oam_index_0)[r4loc] + sort_sprites_offset_into_oam_buffer)>>2;

    int j = kLinkDmaGraphicsIndices[r2];
    link_dma_graphics_index = j * 2;

    if (link_visibility_status != 12) {
      uint8 zcoord = ((int16)link_z_coord >= 0 || BYTE(link_z_coord) < 0xf0) ? BYTE(link_z_coord) : 0;

      const LinkSpriteBody *sp = &kLinkSpriteBodys[j];

      uint8 oam_y = ycoord + sp->y - zcoord;
      uint8 oam_x = xcoord + sp->x;
      uint16 td = sp->tile << 8;

      if ((td & 0xf000) != 0xf000) {
        WORD(oam_buf[oam_pos].charnum) = td & 0xf000 | oam_priority_value | link_palette_bits_of_oam;
        WORD(oam_buf[oam_pos].x) = oam_x | oam_y << 8;
        bytewise_extended_oam[oam_pos] = 2 + (oam_x >= 0xf8);
      }

      if ((td << 4 & 0xf000) != 0xf000) {
        WORD(oam_buf[oam_pos+1].charnum) = td << 4 & 0xf000 | oam_priority_value | link_palette_bits_of_oam | 2;
        WORD(oam_buf[oam_pos+1].x) = (uint8)(xcoord) | (ycoord - zcoord + 8) << 8;
        bytewise_extended_oam[oam_pos+1] = 2;
      }
    }
  }

  /* Hide-Link conditions:
   *   - Doorway-clip: Link is partially behind a door/transition seam
   *     (within 4 px of any screen edge while in a doorway).
   *   - Blink during damage recovery (every other frame for the first
   *     half of the blink window).
   *   - Full invisibility status (status == 12).
   *   - Cape active (the Magic Cape renders Link separately).
   *
   * The hide-shadow flag controls whether the shadow OAM slot also
   * gets blanked. Doorway-clip hides everything; blink/cape leave
   * the shadow visible so the player can still tell where Link is. */
  uint16 t;
  bool hide_shadow = true;
  if (is_standing_in_doorway && ((t = link_x_coord - BG2HOFS_copy2) < 4 || t >= 252 || (t = link_y_coord - BG2VOFS_copy2) < 4 || t >= 224) ||
      (hide_shadow = false,
      submodule_index == 0 && countdown_for_blink && --countdown_for_blink >= 4 && (countdown_for_blink & 1) == 0 ||
      link_visibility_status == 12 ||
      link_cape_mode != 0)) {
    int shadow_oam_pos = (!hide_shadow && link_visibility_status != 12) ?
        (scratch_0_var ? kShadow_oam_indexes_1 : kShadow_oam_indexes_0)[r4loc] >> 2 : -10;

    // This appears to hide link by setting the extended bits of the oam to hide them from the screen.
    // It doesn't really play well with the widescreen modes, so change how it's done.
    if (enhanced_features0 & kFeatures0_WidescreenVisualFixes) {
      OamEnt *oam = &oam_buf[sort_sprites_offset_into_oam_buffer >> 2];
      for (int i = 0; i < 12; i++) {
        if (i < shadow_oam_pos || i > shadow_oam_pos + 1)
          oam[i].y = 0xf0;
      }
    } else {
      uint8 *p = &bytewise_extended_oam[sort_sprites_offset_into_oam_buffer >> 2];
      WORD(p[0]) = 0x101;
      WORD(p[2]) = 0x101;
      WORD(p[4]) = 0x101;
      WORD(p[6]) = 0x101;
      WORD(p[8]) = 0x101;
      WORD(p[10]) = 0x101;
      // Clear the bit again for the shadow oam so it's not hidden?
      if (shadow_oam_pos >= 0)
        WORD(p[shadow_oam_pos]) = 0;
    }
  }

  if (submodule_index == 18 || submodule_index == 19)
    link_y_coord = y_coord_backup;
}

/*
 * FindMostSignificantBit — Returns the 0-indexed bit position of the
 * highest-set bit in `v`, or -1 (255 cast to int) for 0. Implemented
 * as a left-shift loop so the original 65C816 timing is preserved.
 */
uint8 FindMostSignificantBit(uint8 v) {  // 8daac3
  int i = 7;
  while (!(v & 0x80) && --i >= 0)
    v <<= 1;
  return (uint8)i;
}

/*
 * LinkOam_SetWeaponVRAMOffsets — Look up the VRAM tile offsets for
 * the sword/rod sprite on the current pose.
 *
 * Returns true to mean "no sword to draw this frame" (caller skips
 * the sword block). Returns false with `sr` filled in on success.
 *
 * sr->r6: index into kSwordTiledata for the 3-tile sprite layout.
 * sr->r12: extra OAM extended-bits flags (e.g., size bit).
 *
 * For pose ids < 29, link_dma_var3 receives the VRAM source row.
 * For >= 29 (rod-style poses), link_dma_var5 receives a different
 * row that may be substituted from kPlayerOam_Main_SwordStuff_array4
 * when an item is in-hand (so the rod's tip animation correctly
 * tracks the swing frame).
 */
bool LinkOam_SetWeaponVRAMOffsets(int r2, SwordResult *sr) {  // 8dab6e
  uint8 j = kPlayerOam_Main_SwordStuff_array1[r2];
  if ((sr->r6 = j) == 0xff)
    return true;
  sr->r12 = kPlayerOam_Main_SwordStuff_array2[j];
  uint8 y = kPlayerOam_Main_SwordStuff_array3[j];
  if (j < 29) {
    link_dma_var3 = y;
  } else {
    if (link_item_in_hand & 5)
      y = kPlayerOam_Main_SwordStuff_array4[j - 29];
    link_dma_var5 = y;
  }
  return false;
}

/*
 * LinkOam_SetEquipmentVRAMOffsets — Shield equivalent of
 * LinkOam_SetWeaponVRAMOffsets. Returns true to skip the shield
 * draw, false with `sr` populated otherwise.
 *
 * Two slot ranges:
 *   j < 8  : standard shield poses, source goes to link_dma_var4.
 *            r12 is fixed at 2 (priority bits).
 *   j >= 8 : "in-hand" overlap poses (rod or shield held forward).
 *            When an item is in-hand (bits 5 of link_item_in_hand)
 *            substitute the rod-mode row. r12 selects priority based
 *            on the low 3 bits of the source row to keep the shield
 *            from drawing over Link's hand.
 */
bool LinkOam_SetEquipmentVRAMOffsets(int r2, SwordResult *sr) {  // 8dabe6
  uint8 j = kPlayerOam_ShieldStuff_array1[r2];
  if ((sr->r6 = j) == 0xff)
    return true;

  uint8 y = kPlayerOam_ShieldStuff_array2[j];
  if (j >= 8) {
    if (link_item_in_hand & 5)
      y = kPlayerOam_ShieldStuff_array3[j - 8];
    link_dma_var5 = y;
    sr->r12 = (y & 7) ? 0 : 2;
  } else {
    link_dma_var4 = y;
    sr->r12 = 2;
  }
  return false;
}

/*
 * LinkOam_CalculateSwordSparklePosition — When a high-tier sword
 * (Master Sword or above) is mid-swing in standard (non-special)
 * gameplay, draw the sparkle/glint at the sword tip and return the
 * advanced OAM cursor. Otherwise leave oam_pos unchanged.
 *
 * Gating:
 *   - Skip in any non-zero player handler state or speed setting
 *     (sparkles look wrong during dashing/spell-casting).
 *   - Need a sword type >= 2 (Tempered/Golden) and the B button held.
 *   - Skip after frame 9 (post-swing — no sparkle on recoil).
 *
 * The kSwordTipSomething[i] sentinel of 0xffff means "no sparkle for
 * this swing-frame". The (player_oam_x_offset, player_oam_y_offset)
 * pair is updated to the sparkle's offset so the sword-collision
 * code reads the same point that's drawn. The bit9_of_xcoord is set
 * to handle horizontal screen-wrap in widescreen modes.
 */
int LinkOam_CalculateSwordSparklePosition(int oam_pos, uint8 oam_x, uint8 oam_y) {  // 8dacd5
  if (link_player_handler_state | link_speed_setting)
    return oam_pos;
  if (link_sword_type == 0 || link_sword_type == 1 || link_sword_type == 0xff || !(button_mask_b_y & 0x80) || button_b_frames >= 9)
    return oam_pos;

  int i = (link_direction_facing >> 1) * 9 + button_b_frames;
  uint16 td = kSwordTipSomething[i];
  if (td == 0xffff)
    return oam_pos;
  td = td & ~0x3000 | oam_priority_value;
  if (!link_palette_bits_of_oam)
    td = td & ~0xe00 | 0x600;
  WORD(oam_buf[oam_pos].charnum) = td;
  player_oam_x_offset = kSwordOamXOffs_Good[i];
  player_oam_y_offset = kSwordOamYOffs_Good[i];
  oam_x += player_oam_x_offset;
  oam_y += player_oam_y_offset;
  oam_buf[oam_pos].x = oam_x;
  oam_buf[oam_pos].y = oam_y;
  LinkOam_CalculateXOffsetRelativeLink(player_oam_x_offset);
  bytewise_extended_oam[oam_pos] = bit9_of_xcoord;
  return oam_pos + 1;
}

/*
 * LinkOam_UnusedWeaponSettings — Renders up to 4 fragment sprites
 * for the throw-and-shatter animation (link_picking_throw_state bit
 * 4). Walks kPlayerOam_DrawOam_Throwing_State[link_var30e * 4 + i]
 * for the active phase, placing each non-sentinel fragment at the
 * paired (X, Y) offset. Tile id 0x2609 is the small dust/break
 * fragment; the palette + priority bits come from oam_priority_value.
 *
 * Despite the "Unused" name, this is reachable from the throwing-
 * pickup-and-toss state machine in the original code.
 */
void LinkOam_UnusedWeaponSettings(int r4loc, uint8 oam_x, uint8 oam_y) {  // 8dadb6
  int j = link_var30e * 4;
  int oam_pos = ((draw_water_ripples_or_grass != 0 ? kSwordStuff_oam_index_ptrs_1 : kSwordStuff_oam_index_ptrs_0)[r4loc] + sort_sprites_offset_into_oam_buffer)>>2;
  OamEnt *oam = &oam_buf[oam_pos];
  for (int i = 0; i != 4; i++, j++) {
    uint8 st = kPlayerOam_DrawOam_Throwing_State[j];
    if (st != 0xff) {
      WORD(oam->charnum) = 0x2609 & ~0x3000 | oam_priority_value;
      oam->x = oam_x + kPlayerOam_DrawOam_Throwing_X[j];
      oam->y = oam_y + kPlayerOam_DrawOam_Throwing_Y[j];
      bytewise_extended_oam[oam_pos] = 0;
      oam++, oam_pos++;
    }
  }
}

/*
 * LinkOam_DrawDungeonFallShadow — Draws the perspective shadow at
 * the bottom of a pit while Link is still falling. The shadow grows
 * as Link approaches it.
 *
 * yd = vertical distance from Link to the floor below (in unsigned
 *      pixels). The three thresholds (240, 96, 48) pick a shrinking
 *      shadow size: yv = 0 (smallest, far away or already landed),
 *      1 (mid), 2 (largest, about to land). The kPlayerOam_DrawOam_2X
 *      table encodes the X-centering offset for each size.
 *
 * The shadow is two 8x8 OAM tiles side-by-side at the pit floor's
 * Y coordinate (29 px below the detected floor pos in screen space).
 */
void LinkOam_DrawDungeonFallShadow(int r4loc, uint8 xcoord) {  // 8dae3b
  uint8 yd = tiledetect_which_y_pos[0] - 12 - link_y_coord;
  int yv = yd >= 240 ? 0 :
           yd >= 96 ? 2 :
           yd >= 48 ? 1 : 0;

  xcoord += kPlayerOam_DrawOam_2X[yv];
  uint8 ycoord = tiledetect_which_y_pos[0] - 12 - BG2VOFS_copy2 + 29;
  int oam_pos = ((draw_water_ripples_or_grass ? kShadow_oam_indexes_1 : kShadow_oam_indexes_0)[r4loc] + sort_sprites_offset_into_oam_buffer)>>2;

  yv *= 2;
  for (int i = 0; i != 2; i++, oam_pos++, yv++) {
    uint16 td = kLinkShadows_Chardata[yv];
    if (td != 0xffff) {
      WORD(oam_buf[oam_pos].charnum) = td & ~0x3000 | oam_priority_value_2;
      WORD(oam_buf[oam_pos].x) = xcoord | ycoord << 8;
    }
    bytewise_extended_oam[oam_pos] = 0;
    xcoord += 8;
  }
}

/*
 * LinkOam_DrawFootObject — Animated water-ripple / grass-cluster
 * sprite that appears at Link's feet when he walks through water or
 * grass.
 *
 * Two-tier timer:
 *   primary_water_grass_timer: 4-bit ring counter (every frame).
 *     On hitting 9 it resets and bumps the secondary timer.
 *   secondary_water_grass_timer: animation-frame counter (3-frame
 *     cycle for water ripples).
 *
 * draw_water_ripples_or_grass == 2 means thick grass: the animation
 * is keyed off Link's walk cycle (link_animation_steps) so the
 * grass parts when he steps. yv >= 11 is an out-of-bounds guard:
 * the original ROM read past the table; we mirror that behavior
 * with a hardcoded fallback (charnum 0 / 0xAE) to match.
 *
 * The high byte of overlay_index is also updated to drive the BG3
 * tile animation that pairs with this OAM sprite.
 */
void LinkOam_DrawFootObject(int r4loc, uint8 oam_x, uint8 oam_y) {  // 8daed1
  primary_water_grass_timer = (primary_water_grass_timer + 1) & 0xf;
  if (primary_water_grass_timer >= 9) {
    primary_water_grass_timer = 0;
    secondary_water_grass_timer = (secondary_water_grass_timer + 1) & 3;
    if (secondary_water_grass_timer == 3)
      secondary_water_grass_timer = 0;
  }

  int i = (link_direction_facing_mirror >> 1) + kShieldTypeToOffs[link_shield_type];

  oam_x += kOffsToShadowGivenDir_X[i];
  oam_y += kOffsToShadowGivenDir_Y[i];

  int oam_pos = (kShadow_oam_indexes_1[r4loc] + sort_sprites_offset_into_oam_buffer)>>2;

  uint8 yv;
  if (draw_water_ripples_or_grass == 2) {
    yv = (link_animation_steps >= 3 ? link_animation_steps - 3 : link_animation_steps);
    ((uint8 *)&overlay_index)[1] = yv * 4;
    yv = (8 + yv);
  } else {
    ((uint8 *)&overlay_index)[1] = secondary_water_grass_timer * 4;
    yv = (5 + secondary_water_grass_timer);
  }

  OamEnt *oam = &oam_buf[oam_pos];

  if (yv >= 11) {
    // OOB read
    WORD(oam[0].charnum) = 0x00 & ~0x3000 | oam_priority_value_2;
    WORD(oam[1].charnum) = 0xAE | oam_priority_value_2;
  } else {
    WORD(oam[0].charnum) = kLinkShadows_Chardata[yv * 2 + 0] & ~0x3000 | oam_priority_value_2;
    WORD(oam[1].charnum) = kLinkShadows_Chardata[yv * 2 + 1] | oam_priority_value_2;

  }


  oam[0].x = oam_x;
  oam[1].x = oam_x + 8;

  oam[0].y = oam_y;
  oam[1].y = oam_y;

  WORD(bytewise_extended_oam[oam_pos]) = 0;
}

/*
 * LinkOam_CalculateXOffsetRelativeLink — Computes bit 9 of an
 * absolute screen X coordinate (link_x + offset_x) for the
 * sub-screen wrap field. Used to pack the X coordinate's high bit
 * into the SNES OAM extended-bits byte for sprites that span the
 * 256-pixel screen wrap.
 */
void LinkOam_CalculateXOffsetRelativeLink(uint8 x) {  // 8dafc0
  bit9_of_xcoord = (link_x_coord + (int8)x - BG2HOFS_copy2) >> 8 & 1;
}

