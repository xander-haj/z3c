/*
 * load_gfx.c - Graphics Loading, Decompression, and Palette Management
 *
 * Part of the Zelda 3 C reimplementation (A Link to the Past).
 *
 * This file manages the entire graphics pipeline for the game, including:
 *   - Tileset loading and VRAM upload for backgrounds and sprites
 *   - SNES 3bpp-to-4bpp tile format conversion (Do3To4High/Low)
 *   - LZ-style graphics decompression (Decompress)
 *   - Palette loading for all game contexts: overworld, dungeon, HUD, sprites,
 *     Link's armor/sword/shield, bosses (Agahnim, Trinexx, Kholdstare)
 *   - Palette filter effects: fade-in/out, blinding white, whirlpool blue,
 *     screen flash, mosaic, and iris spotlight transitions
 *   - HDMA-driven spotlight and water window effects
 *   - Mirror warp graphics decompression and palette transitions
 *   - Star tile and peg crystal graphics buffer management
 *
 * The SNES uses 15-bit BGR color (5 bits per channel, packed into uint16):
 *   bits [4:0]   = red,  [9:5]  = green,  [14:10] = blue
 * Palette buffers main_palette_buffer[] and aux_palette_buffer[] each hold
 * 256 entries (512 bytes). aux holds the target/reference colors; main holds
 * the currently displayed colors and is modified by filter effects each frame.
 *
 * Original SNES bank addresses are preserved as comments on function
 * definitions (e.g. "// 80d231") to aid cross-referencing with the ROM.
 */

/* Runtime bridge and SNES register/variable mappings */
#include "zelda_rtl.h"
#include "variables.h"
#include "snes/snes_regs.h"
/* Game subsystem headers */
#include "overworld.h"
#include "load_gfx.h"
#include "player.h"
#include "sprite.h"
/* Asset data pointers (kSprGfx, kBgGfx, palette tables, etc.) */
#include "assets.h"

// Allow this to be overwritten
/* SNES 15-bit BGR colors for Power Glove (0x52f6) and Titan's Mitt (0x376) */
uint16 kGlovesColor[2] = {0x52f6, 0x376};

/*
 * Dithering bitmask table used by palette fade effects.
 * Indexed by the 5-bit color channel intensity (0-31), each pair of uint16
 * values provides a 16-bit mask. During fade, each palette entry's R/G/B
 * channels are tested against these masks to decide whether to increment
 * or decrement that channel on the current frame. This creates a temporally
 * dithered fade that transitions smoothly across 31 brightness levels.
 * The pattern progresses from all-bits-set (0xffff, brightest) down to
 * nearly all-bits-clear (0x0001/0x0000, darkest).
 */
static const uint16 kPaletteFilteringBits[64] = {
  0xffff, 0xffff, 0xfffe, 0xffff, 0x7fff, 0x7fff, 0x7fdf, 0xfbff, 0x7f7f, 0x7f7f, 0x7df7, 0xefbf, 0x7bdf, 0x7bdf, 0x77bb, 0xddef,
  0x7777, 0x7777, 0x6edd, 0xbb77, 0x6db7, 0x6db7, 0x5b6d, 0xb6db, 0x5b5b, 0x5b5b, 0x56b6, 0xad6b, 0x5555, 0xad6b, 0x5555, 0xaaab,
  0x5555, 0x5555, 0x2a55, 0x5555, 0x2a55, 0x2a55, 0x294a, 0x5295, 0x2525, 0x2525, 0x2492, 0x4925, 0x1249, 0x1249, 0x1122, 0x4489,
  0x1111, 0x1111,  0x844, 0x2211,  0x421,  0x421,  0x208, 0x1041,  0x101,  0x101,   0x20,  0x401,      1,      1,      0,      1,
};
/* Byte offsets into palette buffer for Agahnim's 3 shadow color groups */
static const uint16 kPaletteFilter_Agahnim_Tab[3] = {0x160, 0x180, 0x1a0};
/*
 * Main tileset table: 37 tileset configurations, 8 graphics pack IDs each.
 * Indexed by main_tile_theme_index. Each row specifies which compressed
 * graphics packs to load into the 8 BG character slots in VRAM:
 *   [0..2] = BG tiles (slots 0-2, always-loaded base tilesets)
 *   [3]    = auxiliary BG slot 0 (dungeon/area-specific tiles)
 *   [4]    = auxiliary BG slot 1
 *   [5..6] = auxiliary BG slots 2-3 (terrain details, overlays)
 *   [7]    = BG slot 7 (common/shared tiles like fonts)
 * Entries 0-20 are dungeon tilesets; 21+ are overworld/special areas.
 */
static const uint8 kMainTilesets[37][8] = {
  {  0,   1,  16,   6, 14, 31, 24, 15},
  {  0,   1,  16,   8, 14, 34, 27, 15},
  {  0,   1,  16,   6, 14, 31, 24, 15},
  {  0,   1,  19,   7, 14, 35, 28, 15},
  {  0,   1,  16,   7, 14, 33, 24, 15},
  {  0,   1,  16,   9, 14, 32, 25, 15},
  {  2,   3,  18,  11, 14, 33, 26, 15},
  {  0,   1,  17,  12, 14, 36, 27, 15},
  {  0,   1,  17,   8, 14, 34, 27, 15},
  {  0,   1,  17,  12, 14, 37, 26, 15},
  {  0,   1,  17,  12, 14, 38, 27, 15},
  {  0,   1,  20,  10, 14, 39, 29, 15},
  {  0,   1,  17,  10, 14, 40, 30, 15},
  {  2,   3,  18,  11, 14, 41, 22, 15},
  {  0,   1,  21,  13, 14, 42, 24, 15},
  {  0,   1,  16,   7, 14, 35, 28, 15},
  {  0,   1,  19,   7, 14,  4,  5, 15},
  {  0,   1,  19,   7, 14,  4,  5, 15},
  {  0,   1,  16,   9, 14, 32, 27, 15},
  {  0,   1,  16,   9, 14, 42, 23, 15},
  {  2,   3,  18,  11, 14, 33, 28, 15},
  {  0,   8,  17,  27, 34, 46, 93, 91},
  {  0,   8,  16,  24, 32, 43, 93, 91},
  {  0,   8,  16,  24, 32, 43, 93, 91},
  { 58,  59,  60,  61, 83, 77, 62, 91},
  { 66,  67,  68,  69, 32, 43, 63, 93},
  {  0,   8,  16,  24, 32, 43, 93, 91},
  {  0,   8,  16,  24, 32, 43, 93, 91},
  {  0,   8,  16,  24, 32, 43, 93, 91},
  {  0,   8,  16,  24, 32, 43, 93, 91},
  {  0,   8,  16,  24, 32, 43, 93, 91},
  {113, 114, 113, 114, 32, 43, 93, 91},
  { 58,  59,  60,  61, 83, 77, 62, 91},
  { 66,  67,  68,  69, 32, 43, 63, 89},
  {  0, 114, 113, 114, 32, 43, 93, 15},
  { 22,  57,  29,  23, 64, 65, 57, 30},
  {  0,  70,  57, 114, 64, 65, 57, 15},
};
/*
 * Sprite tileset table: 144 configurations, 4 graphics pack IDs each.
 * Indexed by sprite_graphics_index. Each row gives the compressed sprite
 * sheet IDs for the 4 sprite character slots in VRAM (0x5000-0x5FFF).
 * A value of 0 means "keep the previously loaded sheet for that slot."
 * Entries with all zeros are unused/reserved padding rows.
 * High entries (>=0x80) index dungeon-map sprite sets (OR'd with palace ID).
 */
static const uint8 kSpriteTilesets[144][4] = {
  { 0, 73,   0,  0},
  {70, 73,  12, 29},
  {72, 73,  19, 29},
  {70, 73,  19, 14},
  {72, 73,  12, 17},
  {72, 73,  12, 16},
  {79, 73,  74, 80},
  {14, 73,  74, 17},
  {70, 73,  18,  0},
  { 0, 73,   0, 80},
  { 0, 73,   0, 17},
  {72, 73,  12,  0},
  { 0,  0,  55, 54},
  {72, 73,  76, 17},
  {93, 44,  12, 68},
  { 0,  0,  78,  0},
  {15,  0,  18, 16},
  { 0,  0,   0, 76},
  { 0, 13,  23,  0},
  {22, 13,  23, 27},
  {22, 13,  23, 20},
  {21, 13,  23, 21},
  {22, 13,  24, 25},
  {22, 13,  23, 25},
  {22, 13,   0,  0},
  {22, 13,  24, 27},
  {15, 73,  74, 17},
  {75, 42,  92, 21},
  {22, 73,  23, 29},
  { 0,  0,   0, 21},
  {22, 13,  23, 16},
  {22, 73,  18,  0},
  {22, 73,  12, 17},
  { 0,  0,  18, 16},
  {22, 13,   0, 17},
  {22, 73,  12,  0},
  {22, 13,  76, 17},
  {14, 13,  74, 17},
  {22, 26,  23, 27},
  {79, 52,  74, 80},
  {53, 77, 101, 54},
  {74, 52,  78,  0},
  {14, 52,  74, 17},
  {81, 52,  93, 89},
  {75, 73,  76, 17},
  {45,  0,   0,  0},
  {93,  0,  18, 89},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  {71, 73,  43, 45},
  {70, 73,  28, 82},
  { 0, 73,  28, 82},
  {93, 73,   0, 82},
  {70, 73,  19, 82},
  {75, 77,  74, 90},
  {71, 73,  28, 82},
  {75, 77,  57, 54},
  {31, 44,  46, 82},
  {31, 44,  46, 29},
  {47, 44,  46, 82},
  {47, 44,  46, 49},
  {31, 30,  48, 82},
  {81, 73,  19,  0},
  {79, 73,  19, 80},
  {79, 77,  74, 80},
  {75, 73,  76, 43},
  {31, 32,  34, 83},
  {85, 61,  66, 67},
  {31, 30,  35, 82},
  {31, 30,  57, 58},
  {31, 30,  58, 62},
  {31, 30,  60, 61},
  {64, 30,  39, 63},
  {85, 26,  66, 67},
  {31, 30,  42, 82},
  {31, 30,  56, 82},
  {31, 32,  40, 82},
  {31, 32,  38, 82},
  {31, 44,  37, 82},
  {31, 32,  39, 82},
  {31, 30,  41, 82},
  {31, 44,  59, 82},
  {70, 73,  36, 82},
  {33, 65,  69, 51},
  {31, 44,  40, 49},
  {31, 13,  41, 82},
  {31, 30,  39, 82},
  {31, 32,  39, 83},
  {72, 73,  19, 82},
  {14, 30,  74, 80},
  {31, 32,  38, 83},
  {21,  0,   0,  0},
  {31,  0,  42, 82},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  { 0,  0,   0,  0},
  {50,  0,   0,  8},
  {93, 73,   0, 82},
  {85, 73,  66, 67},
  {97, 98,  99, 80},
  {97, 98,  99, 80},
  {97, 98,  99, 80},
  {97, 98,  99, 80},
  {97, 98,  99, 80},
  {97, 98,  99, 80},
  {97, 86,  87, 80},
  {97, 98,  99, 80},
  {97, 98,  99, 80},
  {97, 86,  87, 80},
  {97, 86,  99, 80},
  {97, 86,  87, 80},
  {97, 86,  51, 80},
  {97, 86,  87, 80},
  {97, 98,  99, 80},
  {97, 98,  99, 80},
};
/*
 * Auxiliary tileset table: 82 entries x 4 graphics pack IDs.
 * Indexed by aux_tile_theme_index. Each row supplies the area-specific
 * BG tile packs that overlay the base tilesets selected by kMainTilesets.
 * A zero entry means "fall back to the base BG slot from kMainTilesets,"
 * letting the base theme show through. The four columns map to the four
 * auxiliary BG character slots (aux_bg_subset_0..3) at WRAM 0x6000–0x73FF.
 */
static const uint8 kAuxTilesets[82][4] = {
  {  6,   0,  31,  24},
  {  8,   0,  34,  27},
  {  6,   0,  31,  24},
  {  7,   0,  35,  28},
  {  7,   0,  33,  24},
  {  9,   0,  32,  25},
  { 11,   0,  33,  26},
  { 12,   0,  36,  25},
  {  8,   0,  34,  27},
  { 12,   0,  37,  27},
  { 12,   0,  38,  27},
  { 10,   0,  39,  29},
  { 10,   0,  40,  30},
  { 11,   0,  41,  22},
  { 13,   0,  42,  24},
  {  7,   0,  35,  28},
  {  7,   0,   4,   5},
  {  7,   0,   4,   5},
  {  9,   0,  32,  27},
  {  9,   0,  42,  23},
  { 11,   0,  33,  28},
  {  9,   0,  32,  25},
  { 11,   0,  33,  26},
  {  9,   0,  36,  27},
  {  8,   0,  34,  27},
  {  9,   0,  37,  27},
  {  9,   0,  38,  27},
  { 10,   0,  39,  29},
  {  9,   0,  40,  30},
  { 12,   0,  41,  22},
  { 13,   0,  42,  23},
  {114,   0,  43,  93},
  {  0,   0,   0,   0},
  {  0,  87,  76,   0},
  {  0,  86,  79,   0},
  {  0,  83,  77,   0},
  {  0,  82,  73,   0},
  {  0,  85,  74,   0},
  {  0,  83,  84,   0},
  {  0,  81,  78,   0},
  {  0,   0,   0,   0},
  {  0,  80,  75,   0},
  {  0,  83,  77,   0},
  {  0,  85,  84,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,  71,  72,   0},
  {  0,   0,   0,   0},
  {  0,  87,  76,   0},
  {  0,  86,  79,   0},
  {  0,  83,  77,   0},
  {  0,  82,  73,   0},
  {  0,  85,  74,   0},
  {  0,  83,  84,   0},
  {  0,  81,  78,   0},
  {  0,   0,   0,   0},
  {  0,  80,  75,   0},
  {  0,  83,   0,   0},
  {  0,  53,  54,   0},
  {  0,  96,  52,   0},
  {  0,  43,  44,   0},
  {  0,  45,  46,   0},
  {  0,  47,  48,   0},
  {  0,  55,  56,   0},
  {  0,  51,  52,   0},
  {  0,  49,  50,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {  0,   0,   0,   0},
  {114, 113, 114, 113},
  { 23,  64,  65,  57},
};
/*
 * Per-follower offset (in bytes) into the decompressed follower sprite sheet.
 * Indexed by follower_indicator (0..13). Selects which 24-tile region of
 * the loaded follower graphics page should be copied into the active
 * sprite sheet — different tagalongs (Old Man, Princess Zelda, Rooster, etc.)
 * occupy different sub-pages.
 */
static const uint16 kTagalongWhich[14] = {0, 0x600, 0x300, 0x300, 0x300, 0, 0, 0x900, 0x600, 0x600, 0x900, 0x900, 0x600, 0x900};
/*
 * Source-tile offset table for animated sprite frame decoding.
 * Indexed by an animation index a (0..56). DecodeAnimatedSpriteTile_variable
 * selects a 0x180-byte region from a decompressed sprite sheet at this offset
 * and writes it to the 4bpp staging buffer at 0x7F0000+0x2D40.
 */
static const uint16 kDecodeAnimatedSpriteTile_Tab[57] = {
  0x9c0, 0x30, 0x60, 0x90, 0xc0, 0x300, 0x318, 0x330, 0x348, 0x360, 0x378, 0x390, 0x930, 0x3f0, 0x420, 0x450,
  0x468, 0x600, 0x630, 0x660, 0x690, 0x6c0, 0x6f0, 0x720, 0x750, 0x768, 0x900, 0x930, 0x960, 0x990, 0x9f0, 0,
  0xf0, 0xa20, 0xa50, 0x660, 0x600, 0x618, 0x630, 0x648, 0x678, 0x6d8, 0x6a8, 0x708, 0x738, 0x768, 0x960, 0x900,
  0x3c0, 0x990, 0x9a8, 0x9c0, 0x9d8, 0xa08, 0xa38, 0x600, 0x630,
};
/*
 * Sword-type → byte offset into the decompressed sword sprite sheet.
 * Indexed by link_sword_type (0=none, 1=Fighter, 2=Master, 3=Tempered, 4=Gold).
 * Tempered (3) and Gold (4) reuse the Master Sword tile region at 0x120.
 */
static const uint16 kSwordTypeToGfxOffs[5] = {0, 0, 0x120, 0x120, 0x120};
/*
 * Shield-type → byte offset into the decompressed shield sprite sheet.
 * Indexed by link_shield_type (0..3): None/Small at 0x660, Fire at 0x6f0,
 * Mirror Shield at 0x900.
 */
static const uint16 kShieldTypeToGfxOffs[4] = {0x660, 0x660, 0x6f0, 0x900};
/*
 * Overworld-area BG palette descriptor table (31 areas x 3 fields).
 * Each triple is {aux1_bp2to4_hi, aux2_bp5to7_hi, aux3_bp7_lo} consumed by
 * Overworld_LoadPalettes. A value of -1 means "leave the existing palette
 * slot untouched" (the previous area's choice carries over). Indices 0..30
 * are light-world areas; 31..61 are dark-world counterparts.
 */
static const int8 kOwBgPalInfo[93] = {
  0, -1, 7, 0, 1, 7, 0, 2, 7, 0, 3, 7, 0, 4, 7, 0, 5, 7, 0, 6, 7, 7, 6, 5,
  0, 8, 7, 0, 9, 7, 0, 10, 7, 0, 11, 7, 0, -1, 7, 0, -1, 7, 3, 4, 7, 4, 4, 3,
  16, -1, 6, 16, 1, 6, 16, 17, 6, 16, 3, 6, 16, 4, 6, 16, 5, 6, 16, 6, 6, 18, 19, 4,
  18, 5, 4, 16, 9, 6, 16, 11, 6, 16, 12, 6, 16, 13, 6, 16, 14, 6, 16, 15, 6,
};
static const int8 kOwSprPalInfo[40] = {
  -1, -1, 3, 10, 3, 6, 3, 1, 0, 2, 3, 14, 3, 2, 19, 1, 11, 12, 17, 1, 7, 5, 17, 0,
  9, 11, 15, 5, 3, 5, 3, 7, 15, 2, 10, 2, 5, 1, 12, 14,
};
/*
 * Per-step delta applied to the iris spotlight radius each frame.
 * Indexed by (spotlight_var2 >> 1). Negative values shrink the spotlight
 * (close animation), positive values grow it (open animation).
 */
static const int8 kSpotlight_delta_size[4] = {-7, 7, 7, 7};
/*
 * Target spotlight radius that ends each spotlight phase.
 * Indexed by (spotlight_var2 >> 1). When the running radius reaches the
 * matching goal, the spotlight state machine advances to the next phase.
 */
static const uint8 kSpotlight_goal[4] = {0, 126, 35, 126};
/*
 * Quarter-circle lookup used by IrisSpotlight_CalculateCircleValue.
 * Stores 129 precomputed sin/cos-style values that drive the parabolic
 * column edges of the spotlight HDMA window. Index 0 corresponds to the
 * widest opening; index 128 collapses the row to zero (off-screen).
 */
static const uint8 kConfigureSpotlightTable_Helper_Tab[129] = {
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xfe, 0xfe, 0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0xfc, 0xfc, 0xfc, 0xfb, 0xfb, 0xfb, 0xfa, 0xfa, 0xf9, 0xf9, 0xf8, 0xf8,
  0xf7, 0xf7, 0xf6, 0xf6, 0xf5, 0xf5, 0xf4, 0xf3, 0xf3, 0xf2, 0xf1, 0xf1, 0xf0, 0xef, 0xee, 0xee, 0xed, 0xec, 0xeb, 0xea, 0xe9, 0xe9, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4, 0xe3, 0xe2, 0xe1, 0xdf, 0xde,
  0xdd, 0xdc, 0xdb, 0xda, 0xd8, 0xd7, 0xd6, 0xd5, 0xd3, 0xd2, 0xd0, 0xcf, 0xcd, 0xcc, 0xca, 0xc9, 0xc7, 0xc6, 0xc4, 0xc2, 0xc1, 0xbf, 0xbd, 0xbb, 0xb9, 0xb7, 0xb6, 0xb4, 0xb1, 0xaf, 0xad, 0xab,
  0xa9, 0xa7, 0xa4, 0xa2, 0x9f, 0x9d, 0x9a, 0x97, 0x95, 0x92, 0x8f, 0x8c, 0x89, 0x86, 0x82, 0x7f, 0x7b, 0x78, 0x74, 0x70, 0x6c, 0x67, 0x63, 0x5e, 0x59, 0x53, 0x4d, 0x46, 0x3f, 0x37, 0x2d, 0x1f,
  0,
};
/*
 * Sprite-pack lookup for Graphics_LoadChrHalfSlot.
 * Indexed by load_chr_halfslot_even_odd-1 (0..19). Each value is the
 * compressed sprite-pack ID to stream into the dynamic CHR half-slot
 * buffer (used for spell animations and other on-demand sprite reloads).
 */
static const uint8 kGraphicsHalfSlotPacks[20] = {
  1, 1, 8, 8, 9, 9, 2, 2, 2, 2, 3, 3, 4, 4, 5, 5,
  8, 8, 8, 8,
};
/*
 * Companion table for Graphics_LoadChrHalfSlot — when non-negative, this
 * value is written to palette_sp6r_indoors before loading the new tiles
 * so the dynamic CHR slot ships with the correct palette. -1 means
 * "leave the existing sp6r palette in place."
 */
static const int8 kGraphicsLoadSp6[20] = {
  10, -1, 3, -1, 0, -1, -1, -1, 1, -1, 2, -1, 0, -1, -1,
  -1, -1, -1, -1, -1,
};
/*
 * NMI subroutine selectors driving the mirror-warp animation steps.
 * Indexed by overworld_map_state (0..14). Non-zero entries pick a
 * specialized NMI handler that uploads freshly decompressed VRAM blocks
 * during the mirror transition; zero means "use the default NMI path."
 */
static const uint8 kMirrorWarp_LoadNext_NmiLoad[15] = {0, 14, 15, 16, 17, 0, 0, 0, 0, 0, 0, 18, 19, 20, 0};

/*
 * Returns the raw compressed-sprite-pack pointer for graphics index i.
 * Wrapper around the kSprGfx asset table; used by every sprite decoder
 * that needs the source bytes before decompression.
 */
static const uint8 *GetCompSpritePtr(int i) {
  return kSprGfx(i).ptr;
}

/*
 * One frame of the bouncing dither-fade filter.
 * Walks main_palette_buffer entries 0x00..0xEF (skipping the HUD region
 * 0x01..0x1F and the gear region 0xD8..0xDF), nudging each R/G/B channel
 * one step toward (or away from) the target color in aux_palette_buffer.
 * The dither bitmask in kPaletteFilteringBits decides which channels move
 * on this particular frame, producing the smooth fade effect. When the
 * fade reaches mosaic_target_level the direction inverts and the next
 * submodule step begins (the "bounce" behavior).
 */
void ApplyPaletteFilter_bounce() {

  const uint16 *load_ptr = kPaletteFilteringBits + (palette_filter_countdown >= 0x10);

  int mask = kUpperBitmasks[palette_filter_countdown & 0xf];
  int dt = darkening_or_lightening_screen ? 1 : -1;
  int j = 0;
  for (;;) {
    uint16 c = main_palette_buffer[j], a = aux_palette_buffer[j];
    if (!(load_ptr[(a & 0x1f) * 2] & mask))
      c += dt;
    if (!(load_ptr[(a & 0x3e0) >> 4] & mask))
      c += dt << 5;
    if (!(load_ptr[(a & 0x7c00) >> 9] & mask))
      c += dt << 10;
    main_palette_buffer[j] = c;
    j++;
    if (j == 1)
      j = 0x20;
    else if (j == 0xd8)
      j = 0xe0;
    else if (j == 0xf0)
      break;
  }
  flag_update_cgram_in_nmi++;
  if (!darkening_or_lightening_screen) {
    if (++palette_filter_countdown != mosaic_target_level)
      return;
  } else {
    if (palette_filter_countdown-- != mosaic_target_level)
      return;
  }
  darkening_or_lightening_screen ^= 2;
  palette_filter_countdown = 0;
  subsubmodule_index++;
}

/*
 * Applies one dither-fade step to a contiguous palette range [from, to).
 * Same per-channel dithered increment/decrement as ApplyPaletteFilter_bounce
 * but constrained to a caller-supplied subrange. Used by the more targeted
 * filters (HUD, Trinexx shell, Agahnim shadow, etc.) to advance only the
 * palette entries they care about.
 */
void PaletteFilter_Range(int from, int to) {
  const uint16 *load_ptr = kPaletteFilteringBits + (palette_filter_countdown >= 0x10);
  int mask = kUpperBitmasks[palette_filter_countdown & 0xf];
  int dt = darkening_or_lightening_screen ? 1 : -1;
  for (int j = from; j != to; j++) {
    uint16 c = main_palette_buffer[j], a = aux_palette_buffer[j];
    if (!(load_ptr[(a & 0x1f) * 2] & mask))
      c += dt;
    if (!(load_ptr[(a & 0x3e0) >> 4] & mask))
      c += dt << 5;
    if (!(load_ptr[(a & 0x7c00) >> 9] & mask))
      c += dt << 10;
    main_palette_buffer[j] = c;
  }
}

/*
 * Advances the global palette-fade countdown by one tick.
 * When the counter wraps past the dither-table length (0x1F) it resets
 * and toggles the fade direction. The CGRAM dirty flag is bumped so the
 * NMI handler will upload the new palette next frame. The link_actual_vel_y
 * increment is preserved from the original ROM (the comment "wtf?" is the
 * original author's note that this side effect appears to be a ROM bug).
 */
void PaletteFilter_IncrCountdown() {
  if (++palette_filter_countdown == 0x1f) {
    palette_filter_countdown = 0;
    darkening_or_lightening_screen ^= 2;
    if (darkening_or_lightening_screen)
      WORD(link_actual_vel_y)++; // wtf?
  }
  flag_update_cgram_in_nmi++;
}

/*
 * Decompresses and 3bpp→4bpp-expands one item-animation tile group.
 *   dst       — destination 4bpp tile buffer (in WRAM)
 *   num       — number of 8x8 tiles to expand
 *   r12       — index into kIntro_LoadGfx_Tab selecting which 24-byte
 *               source slot to read from the loaded sprite sheet
 *   from_temp — true: source is the scratch decompression buffer at
 *               g_ram[0x14000]; false: source is the always-resident
 *               sprite-pack 0 pointer
 * Two adjacent tile rows are expanded (offset 0x180 apart) so that paired
 * top/bottom item halves are produced in one call. Returns the dst pointer
 * advanced past the bytes just written.
 */
uint8 *LoadItemAnimationGfxOne(uint8 *dst, int num, int r12, bool from_temp) {
  static const uint8 kIntro_LoadGfx_Tab[10] = { 0, 11, 8, 38, 42, 45, 34, 3, 33, 46 };
  const uint8 *src = from_temp ? &g_ram[0x14000] : GetCompSpritePtr(0);
  const uint8 *base_src = src;
  src += kIntro_LoadGfx_Tab[r12] * 24;
  Expand3To4High(dst, src, base_src, num);
  Expand3To4High(dst + 0x20 * num, src + 0x180, base_src, num);
  return dst + 0x40 * num;
}

/*
 * Software replica of the SNES hardware divider register pair.
 * Returns dividend / divisor, or 0xFFFF on divide-by-zero (matching the
 * SNES math unit's behavior). Used by IrisSpotlight_CalculateCircleValue
 * to compute the spotlight column-radius ratio.
 */
uint16 snes_divide(uint16 dividend, uint8 divisor) {
  return divisor ? dividend / divisor : 0xffff;
}

/*
 * Wipes the standard pair of tile maps with default fill words:
 * 0x7F for the BG3 layer at vram+0x6000 and 0x1EC for the main map.
 * This is the most common erase pattern used between scenes.
 */
void EraseTileMaps_normal() {
  EraseTileMaps(0x7f, 0x1ec);
}

/*
 * Decompresses sprite-pack `pack` into the scratch buffer and uploads
 * the resulting 2bpp tile data (1 KW = 2 KB) directly to VRAM at
 * vram_ptr. Used for HUD font and attract-mode background tiles that
 * are stored as raw 2bpp rather than the planar 3bpp format.
 */
static void DecompAndUpload2bpp(uint16 *vram_ptr, uint8 pack) {
  Decomp_spr(&g_ram[0x14000], pack);
  const uint8 *src = &g_ram[0x14000];
  memcpy(vram_ptr, src, 1024 * sizeof(uint16));
}

/*
 * Restores the peg-puzzle (orange/blue barrier) graphics buffer from the
 * cached tile mapping. Called after a load to put the peg sprites back
 * in the visual state they had before the save — the buffer offsets are
 * swapped depending on which color is currently raised.
 */
void RecoverPegGFXFromMapping() {
  if (BYTE(orange_blue_barrier_state))
    Dungeon_UpdatePegGFXBuffer(0x180, 0x0);
  else
    Dungeon_UpdatePegGFXBuffer(0x0, 0x180);
}

/*
 * Loads the 256-byte overworld-map palette into main_palette_buffer.
 * Selects the light-world (offset 0) or dark-world (offset 0x80) palette
 * page based on bit 6 of overworld_screen_index. Called by the world-map
 * Mode 7 sequence in messaging.c.
 */
void LoadOverworldMapPalette() {
  memcpy(main_palette_buffer, &kOverworldMapPaletteData[overworld_screen_index & 0x40 ? 0x80 : 0], 256);
}

/* Erases tile maps using the Triforce-screen fill values (BG3=0xA9, BG=0x7F). */
void EraseTileMaps_triforce() {  // 808333
  EraseTileMaps(0xa9, 0x7f);
}

/* Erases tile maps for the dungeon-map screen (BG3=0x7F, BG=0x300). */
void EraseTileMaps_dungeonmap() {  // 80833f
  EraseTileMaps(0x7f, 0x300);
}

/*
 * Generic tile-map eraser. Fills the main tile-map region (vram[0..0x1FFF])
 * with `r0` and the BG3 tile-map region (vram[0x6000..0x67FF]) with `r2`.
 * Used between major scene transitions to guarantee a clean slate before
 * the next scene's tile maps are streamed in.
 */
void EraseTileMaps(uint16 r2, uint16 r0) {  // 808355
  uint16 *dst = g_zenv.vram;
  for (int i = 0; i < 0x2000; i++)
    dst[i] = r0;

  dst = g_zenv.vram + 0x6000;
  for (int i = 0; i < 0x800; i++)
    dst[i] = r2;
}

/*
 * Forces the screen blank by writing 0x80 to the shadow INIDISP register
 * and disabling all HDMA channels. Used during multi-frame VRAM uploads
 * so the user never sees a partially written tile map.
 */
void EnableForceBlank() {  // 80893d
  INIDISP_copy = 0x80;
  HDMAEN_copy = 0;
}

/*
 * Builds the 4bpp item-animation tile buffer at WRAM 0x9480.
 * Decompresses several sprite packs (95, 96, 84) into the scratch
 * buffer and uses LoadItemAnimationGfxOne to expand each item's tiles
 * into a contiguous 4bpp region: rod, hammer, bow, shovel, sleeping zzz,
 * misc#2, hookshot, bug net, cane, book of mudora, then the rupee tiles.
 * Finishes by calling LoadItemGFX_Auxiliary for follow-up icons.
 */
void LoadItemGFXIntoWRAM4BPPBuffer() {  // 80d231
  uint8 *dst = &g_ram[0x9000 + 0x480];
  dst = LoadItemAnimationGfxOne(dst, 7, 0, false);  // rod
  dst = LoadItemAnimationGfxOne(dst, 7, 1, false);  // hammer
  dst = LoadItemAnimationGfxOne(dst, 3, 2, false);  // bow

  Decomp_spr(&g_ram[0x14000], 95);
  dst = LoadItemAnimationGfxOne(dst, 4, 3, true);  // shovel
  dst = LoadItemAnimationGfxOne(dst, 3, 4, true);  // sleeping zzz
  dst = LoadItemAnimationGfxOne(dst, 1, 5, true);  // misc #2
  dst = LoadItemAnimationGfxOne(dst, 4, 6, false); // hookshot

  Decomp_spr(&g_ram[0x14000], 96);
  dst = LoadItemAnimationGfxOne(dst, 14, 7, true); // bugnet
  dst = LoadItemAnimationGfxOne(dst, 7, 8, true);  // cane

  Decomp_spr(&g_ram[0x14000], 95);
  dst = LoadItemAnimationGfxOne(dst, 2, 9, true);  // book of mudora
  Decomp_spr(&g_ram[0x14000], 84);

  dst = &g_ram[0xa480];
  Expand3To4High(dst, &g_ram[0x14000], g_ram, 8);
  Expand3To4High(dst + 8 * 0x20, &g_ram[0x14180], g_ram, 8);

  // rupees
  Decomp_spr(&g_ram[0x14000], 96);
  dst = &g_ram[0xb280];
  Expand3To4High(dst, &g_ram[0x14000], g_ram, 3);
  Expand3To4High(dst + 3 * 0x20, &g_ram[0x14180], g_ram, 3);

  LoadItemGFX_Auxiliary();
}

/*
 * Decompresses Link's currently equipped sword tiles into the live 4bpp
 * sprite buffer at WRAM 0x9000. Loads packs 0x5F and 0x5E (the two
 * sword sheets) and selects the per-sword byte offset from
 * kSwordTypeToGfxOffs. Both upper and lower sword tile rows are expanded
 * (0x180 apart) so the full 12-tile sprite is materialized.
 */
void DecompressSwordGraphics() {  // 80d2c8
  Decomp_spr(&g_ram[0x14600], 0x5f);
  Decomp_spr(&g_ram[0x14000], 0x5e);
  const uint8 *src = &g_ram[0x14000] + kSwordTypeToGfxOffs[link_sword_type];
  Expand3To4High(&g_ram[0x9000 + 0], src, g_ram, 12);
  Expand3To4High(&g_ram[0x9000 + 0x180], src + 0x180, g_ram, 12);
}

/*
 * Decompresses Link's currently equipped shield tiles into the 4bpp
 * sprite buffer at WRAM 0x9300. Same dual-pack pattern as the sword
 * function, but only 6 tiles are expanded and the byte offset comes from
 * kShieldTypeToGfxOffs (Fighter/Red/Mirror).
 */
void DecompressShieldGraphics() {  // 80d308
  Decomp_spr(&g_ram[0x14600], 0x5f);
  Decomp_spr(&g_ram[0x14000], 0x5e);
  const uint8 *src = &g_ram[0x14000] + kShieldTypeToGfxOffs[link_shield_type];
  Expand3To4High(&g_ram[0x9000 + 0x300], src, g_ram, 6);
  Expand3To4High(&g_ram[0x9000 + 0x3c0], src + 0x180, g_ram,6);
}

/*
 * Streams a frame of animated dungeon BG tiles into the 4bpp BG buffer.
 * Decompresses pack `a` and the always-paired pack 0x5C, expands both
 * 48-tile groups via Do3To4Low16Bit, then rotates four 0x200-byte tile
 * regions in a 4-way ring (0x1880 → 0x1C80 → 0x1E80 → 0x1A80 → 0x1880)
 * so successive calls show the next phase of the rolling animation.
 * The final VRAM upload address for the next NMI is set to 0x3B00.
 */
void DecompressAnimatedDungeonTiles(uint8 a) {  // 80d337
  Decomp_bg(&g_ram[0x14000], a);
  Do3To4Low16Bit(&g_ram[0x9000 + 0x1680], &g_ram[0x14000], 48);
  Decomp_bg(&g_ram[0x14000], 0x5c);
  Do3To4Low16Bit(&g_ram[0x9000 + 0x1C80], &g_ram[0x14000], 48);

  for (int i = 0; i < 256; i++) {
    uint8 *p = &g_ram[0x9000 + i * 2];
    uint16 x = WORD(p[0x1880]);
    WORD(p[0x1880]) = WORD(p[0x1C80]);
    WORD(p[0x1C80]) = WORD(p[0x1E80]);
    WORD(p[0x1E80]) = WORD(p[0x1A80]);
    WORD(p[0x1A80]) = x;
  }
  animated_tile_vram_addr = 0x3b00;
}

/*
 * Streams a frame of animated overworld BG tiles. Decompresses two
 * consecutive packs (`a` and `a+1`), expands both into the 4bpp buffer
 * at 0x9000+0x1680/0x1E80, and sets the next NMI VRAM target to 0x3C00.
 * Used for water, waterfalls, lava and other ambient terrain animations.
 */
void DecompressAnimatedOverworldTiles(uint8 a) {  // 80d394
  Decomp_bg(&g_ram[0x14000], a);
  Do3To4Low16Bit(&g_ram[0x9000 + 0x1680], &g_ram[0x14000], 64);
  Decomp_bg(&g_ram[0x14000], a + 1);
  Do3To4Low16Bit(&g_ram[0x9000 + 0x1E80], &g_ram[0x14000], 32);
  animated_tile_vram_addr = 0x3c00;
}

/*
 * Loads the auxiliary item-icon tiles that aren't part of the main
 * item-animation buffer: pack 0x0F (small misc icons), pack 0x58
 * (sprite-side overlay tiles), and pack 0x05 (a single-tile pair from
 * offset 0x480). All three are expanded with Do3To4Low16Bit into the
 * 4bpp WRAM buffer at 0x9000.
 */
void LoadItemGFX_Auxiliary() {  // 80d3c6
  Decomp_bg(&g_ram[0x14000], 0xf);
  Do3To4Low16Bit(&g_ram[0x9000 + 0x2340], &g_ram[0x14000], 16);

  Decomp_spr(&g_ram[0x14000], 0x58);
  Do3To4Low16Bit(&g_ram[0x9000 + 0x2540], &g_ram[0x14000], 32);

  Decomp_bg(&g_ram[0x14000], 0x5);
  Do3To4Low16Bit(&g_ram[0x9000 + 0x2dc0], &g_ram[0x14480], 2);
}

/*
 * Loads the active tagalong/follower's sprite tiles. Picks the correct
 * follower sprite pack (0x64 / 0x66 / 0x59 / 0x58) based on
 * follower_indicator (Old Man, Maiden, Princess Zelda, etc.), decompresses
 * it alongside the always-needed pack 0x65, then expands the appropriate
 * sub-region selected by kTagalongWhich into the live 4bpp tile buffer.
 */
void LoadFollowerGraphics() {  // 80d423
  uint8 yv = 0x64;
  if (follower_indicator != 1) {
    yv = 0x66;
    if (follower_indicator >= 9) {
      yv = 0x59;
      if (follower_indicator >= 12)
        yv = 0x58;
    }
  }
  Decomp_spr(&g_ram[0x14600], yv);
  Decomp_spr(&g_ram[0x14000], 0x65);
  Do3To4Low16Bit(&g_ram[0x9000] + 0x2940, &g_ram[0x14000 + kTagalongWhich[follower_indicator]], 0x20);
}

/*
 * Writes a 4-tile animated-sprite frame to the 4bpp staging area at
 * WRAM 0x9000+0x2D40. The byte offset into the source pack comes from
 * kDecodeAnimatedSpriteTile_Tab[a]; both halves of the 2x2 tile block
 * are expanded so the next NMI upload pushes a complete frame to VRAM.
 */
void WriteTo4BPPBuffer_at_7F4000(uint8 a) {  // 80d4db
  uint8 *src = &g_ram[0x14000] + kDecodeAnimatedSpriteTile_Tab[a];
  Expand3To4High(&g_ram[0x9000] + 0x2d40, src, g_ram, 2);
  Expand3To4High(&g_ram[0x9000] + 0x2d40 + 0x40, src + 0x180, g_ram, 2);
}

/*
 * Picks the correct sprite pack for animation index `a` and stages the
 * frame. Three pack-IDs (0x5B/0x5C/0x5D) cover different animation banks;
 * `a` selects which one based on its value bands. After decompressing
 * the chosen pack and the always-needed pack 0x5A, hands off to
 * WriteTo4BPPBuffer_at_7F4000 to expand the selected tile group.
 */
void DecodeAnimatedSpriteTile_variable(uint8 a) {  // 80d4ed
  uint8 y = (a == 0x23 || a >= 0x37) ? 0x5d :
            (a == 0xc || a >= 0x24) ? 0x5c : 0x5b;
  Decomp_spr(&g_ram[0x14600], y);
  Decomp_spr(&g_ram[0x14000], 0x5a);
  WriteTo4BPPBuffer_at_7F4000(a);
}

/*
 * 3bpp → 4bpp tile expander used for the upper-half tile rows.
 *   dst  — output 4bpp tile buffer
 *   src  — current source pointer into a decompressed 3bpp sheet
 *   base — base of the same sheet, used to detect the 0x80-byte row
 *          boundary so the source can wrap to the next tile group
 *   num  — number of 8-row 3bpp tiles to expand
 * For each tile the inner loop interleaves the two existing bitplanes
 * (src[0..1]) with a synthesized third bitplane (the OR of the first
 * two combined with src2's high-plane byte). When (src - base) crosses
 * the 0x78 mark the source jumps forward 0x180 bytes — that's how the
 * SNES tile sheet's interleaved 16x16 tile layout is unpacked into a
 * linear 4bpp stream.
 */
void Expand3To4High(uint8 *dst, const uint8 *src, const uint8 *base, int num) {  // 80d61c
  do {
    const uint8 *src2 = src + 0x10;
    int n = 8;
    do {
      uint16 t = WORD(src[0]);
      uint8 u = src2[0];
      WORD(dst[0]) = t;
      WORD(dst[0x10]) = (t | (t >> 8) | u) << 8 | u;
      src += 2, src2 += 1, dst += 2;
    } while (--n);
    dst += 16, src = src2;
    if (!(src - base & 0x78))
      src += 0x180;
  } while (--num);
}

/*
 * Loads the four auxiliary BG tile sheets for the current
 * aux_tile_theme_index plus the matching sprite sheets.
 * For each non-zero entry in kAuxTilesets, decompresses the pack into
 * the staging area at WRAM 0x6000 (each pack is exactly 0x600 bytes,
 * verified by assert). Then chains into Gfx_LoadSpritesInner to load
 * the corresponding sprite sheets at 0x7800.
 */
void LoadTransAuxGFX() {  // 80d66e
  uint8 *dst = &g_ram[0x6000];
  const uint8 *p = kAuxTilesets[aux_tile_theme_index];
  int len;

  if (p[0]) {
    aux_bg_subset_0 = p[0];
    len = Decomp_bg(dst, aux_bg_subset_0);
    assert(len == 0x600);
  }
  if (p[1]) {
    aux_bg_subset_1 = p[1];
    len = Decomp_bg(dst + 0x600, aux_bg_subset_1);
    assert(len == 0x600);
  }
  if (p[2]) {
    aux_bg_subset_2 = p[2];
    len = Decomp_bg(dst + 0x600*2, aux_bg_subset_2);
    assert(len == 0x600);
  }
  if (p[3]) {
    aux_bg_subset_3 = p[3];
    len = Decomp_bg(dst + 0x600*3, aux_bg_subset_3);
    assert(len == 0x600);
  }
  Gfx_LoadSpritesInner(dst + 0x600 * 4 );
}

/* Sprites-only variant of LoadTransAuxGFX — used by transitions where
 * the BG sheets are already resident and only the four sprite slots
 * need to be (re)loaded. */
void LoadTransAuxGFX_sprite() {  // 80d6f9
  Gfx_LoadSpritesInner(&g_ram[0x7800]);
}

/*
 * Decompresses the four sprite sheets named by kSpriteTilesets for the
 * current sprite_graphics_index into the WRAM sprite-CHR staging area
 * starting at `dst`. A zero entry in the table means "keep the sheet
 * that was loaded last time" — the previously cached sprite_gfx_subset_*
 * variable supplies the pack ID. Each decompressed sheet must be
 * exactly 0x600 bytes (asserted). Resets incremental_counter_for_vram
 * so the next NMI cycle starts uploading the new tiles fresh.
 */
void Gfx_LoadSpritesInner(uint8 *dst) {  // 80d706
  const uint8 *p = kSpriteTilesets[sprite_graphics_index];
  int len;

  if (p[0])
    sprite_gfx_subset_0 = p[0];
  len = Decomp_spr(dst, sprite_gfx_subset_0);
  assert(len == 0x600);
  if (p[1])
    sprite_gfx_subset_1 = p[1];
  len = Decomp_spr(dst + 0x600, sprite_gfx_subset_1);
  assert(len == 0x600);
  if (p[2])
    sprite_gfx_subset_2 = p[2];
  len = Decomp_spr(dst + 0x600*2, sprite_gfx_subset_2);
  assert(len == 0x600);
  if (p[3])
    sprite_gfx_subset_3 = p[3];
  len = Decomp_spr(dst + 0x600*3, sprite_gfx_subset_3);
  assert(len == 0x600);
  incremental_counter_for_vram = 0;
}

/*
 * After a save-state load (or other context restore), re-decompresses
 * every BG and sprite sheet whose pack-ID is currently cached in the
 * aux_bg_subset_* / sprite_gfx_subset_* variables. Reinstates the exact
 * VRAM contents the game expected without re-running the higher-level
 * tileset selection logic.
 */
void ReloadPreviouslyLoadedSheets() {  // 80d788
  Decomp_bg(&g_ram[0x6000], aux_bg_subset_0);
  Decomp_bg(&g_ram[0x6600], aux_bg_subset_1);
  Decomp_bg(&g_ram[0x6c00], aux_bg_subset_2);
  Decomp_bg(&g_ram[0x7200], aux_bg_subset_3);
  Decomp_spr(&g_ram[0x7800], sprite_gfx_subset_0);
  Decomp_spr(&g_ram[0x7e00], sprite_gfx_subset_1);
  Decomp_spr(&g_ram[0x8400], sprite_gfx_subset_2);
  Decomp_spr(&g_ram[0x8a00], sprite_gfx_subset_3);
  incremental_counter_for_vram = 0;
}

/*
 * Decompresses the two sprite packs (0x67, 0x68) holding the attract-mode
 * story-screen graphics into the scratch buffer at WRAM 0x14000/0x14800.
 * Called once per attract-mode story page before that page is uploaded.
 */
void Attract_DecompressStoryGFX() {  // 80d80e
  Decomp_spr(&g_ram[0x14000], 0x67);
  Decomp_spr(&g_ram[0x14800], 0x68);
}

/*
 * Multi-frame state machine that drives the Magic Mirror warp animation.
 * Each call advances overworld_map_state by one and runs the work for
 * the current step:
 *   case 0  — wait for the spin-up counter, then SetTargetOverworldWarpToPyramid
 *   case 1  — decompress + expand the first pair of destination BG sheets
 *   case 2  — decompress + expand the second pair
 *   case 3  — re-expand cached aux BG sheets into a fresh 4bpp buffer
 *   case 4  — decompress + expand the third pair
 *   case 5  — load destination overworld overlays and translucency flag
 *   case 6/9 — interleaved NMI sync slots
 *   case 7  — repaint the destination screen at the mirror position
 *   case 8  — load destination sprite sheets and palettes
 *   case 10 — kick off animated overworld tile streaming for the new area
 *   case 11 — pick the destination map's translucency state and start
 *             expanding sprite-pack 0 into VRAM
 *   case 12/13 — finish expanding the new sprite sheets
 *   case 14 — terminal state: animation complete
 * The xt offset (0 or 8) selects light-world vs. dark-world rows in
 * kVariousPacks. nmi_subroutine_index/nmi_disable_core_updates are set
 * from kMirrorWarp_LoadNext_NmiLoad so the NMI handler uploads the right
 * VRAM region for each step.
 */
void AnimateMirrorWarp() {  // 80d864
  int st = overworld_map_state++, tt;
  nmi_subroutine_index = nmi_disable_core_updates = kMirrorWarp_LoadNext_NmiLoad[st];
  uint8 t, xt = overworld_screen_index & 0x40 ? 8 : 0;
  switch (st) {
  case 0:
    if (++mirror_vars.ctr2 != 32)
      overworld_map_state = 0;
    else
      SetTargetOverworldWarpToPyramid();
    break;
  case 1:
    AnimateMirrorWarp_DecompressNewTileSets();
    Decomp_bg(&g_ram[0x14000], kVariousPacks[xt]);
    Decomp_bg(&g_ram[0x14600], kVariousPacks[xt + 1]);
    Do3To4High16Bit(&g_ram[0x10000], &g_ram[0x14000], 64);
    Do3To4Low16Bit(&g_ram[0x10800], &g_ram[0x14600], 64);
    break;
  case 2:
    Decomp_bg(&g_ram[0x14000], kVariousPacks[xt + 2]);
    Decomp_bg(&g_ram[0x14600], kVariousPacks[xt + 3]);
    Do3To4Low16Bit(&g_ram[0x10000], &g_ram[0x14000], 64);
    Do3To4High16Bit(&g_ram[0x10800], &g_ram[0x14600], 64);
    break;
  case 3:
    Decomp_bg(&g_ram[0x14000], aux_bg_subset_1);
    Decomp_bg(&g_ram[0x14600], aux_bg_subset_2);
    Do3To4High16Bit(&g_ram[0x10000], &g_ram[0x14000], 128);
    break;
  case 4:
    Decomp_bg(&g_ram[0x14000], kVariousPacks[xt + 4]);
    Decomp_bg(&g_ram[0x14600], kVariousPacks[xt + 5]);
    Do3To4Low16Bit(&g_ram[0x10000], &g_ram[0x14000], 128);
    break;
  case 5:
    PreOverworld_LoadOverlays();
    if (BYTE(overworld_screen_index) == 27 || BYTE(overworld_screen_index) == 91)
      TS_copy = 1;
    submodule_index--;
    nmi_subroutine_index = nmi_disable_core_updates = 12;
    break;
  case 6:
  case 9:
    nmi_subroutine_index = nmi_disable_core_updates = 13;
    break;
  case 7:
    Overworld_DrawScreenAtCurrentMirrorPosition();
    nmi_disable_core_updates++;
    break;
  case 8:
    MirrorWarp_LoadSpritesAndColors();
    nmi_subroutine_index = nmi_disable_core_updates = 12;
    break;
  case 10:
    t = overworld_screen_index & 0xbf;
    DecompressAnimatedOverworldTiles(t == 3 || t == 5 || t == 7 ? 0x58 : 0x5a);
    break;
  case 11:
    t = overworld_screen_index;
    TS_copy = (t == 0 || t == 0x70 || t == 0x40 || t == 0x5b || t == 3 || t == 5 || t == 7 || t == 0x43 || t == 0x45 || t == 0x47);
    Do3To4High16Bit(&g_ram[0x10000], GetCompSpritePtr(kVariousPacks[xt + 6]), 64);
    break;
  case 12:
    Decomp_spr(&g_ram[0x14000], sprite_gfx_subset_0);
    Decomp_spr(&g_ram[0x14600], sprite_gfx_subset_1);
    tt = WORD(sprite_gfx_subset_0);
    if (tt == 0x52 || tt == 0x53 || tt == 0x5a || tt == 0x5b)
      Do3To4High16Bit(&g_ram[0x10000], &g_ram[0x14000], 64);
    else
      Do3To4Low16Bit(&g_ram[0x10000], &g_ram[0x14000], 64);
    Do3To4Low16Bit(&g_ram[0x10800], &g_ram[0x14600], 64);
    break;
  case 13:
    Decomp_spr(&g_ram[0x14000], sprite_gfx_subset_2);
    Decomp_spr(&g_ram[0x14600], sprite_gfx_subset_3);
    Do3To4Low16Bit(&g_ram[0x10000], &g_ram[0x14000], 128);
    HandleFollowersAfterMirroring();
    break;
  case 14:
    overworld_map_state = 14;
    break;
  }
}

/*
 * Helper for AnimateMirrorWarp that picks which BG/sprite packs the
 * destination area will use. For each aux slot it prefers the auxiliary
 * pack from kAuxTilesets, falling back to the main tileset's BG slot
 * 3..6 when the aux entry is zero. The four sprite_gfx_subset_*
 * variables are also updated from kSpriteTilesets so the next decompress
 * step pulls the correct sheets.
 */
void AnimateMirrorWarp_DecompressNewTileSets() {  // 80d8fe
  const uint8 *mt = kMainTilesets[main_tile_theme_index];
  const uint8 *at = kAuxTilesets[aux_tile_theme_index];

  aux_bg_subset_0 = at[0] ? at[0] : mt[3];
  aux_bg_subset_1 = at[1] ? at[1] : mt[4];
  aux_bg_subset_2 = at[2] ? at[2] : mt[5];
  aux_bg_subset_3 = at[3] ? at[3] : mt[6];

  const uint8 *p = kSpriteTilesets[sprite_graphics_index];
  if (p[0]) sprite_gfx_subset_0 = p[0];
  if (p[1]) sprite_gfx_subset_1 = p[1];
  if (p[2]) sprite_gfx_subset_2 = p[2];
  if (p[3]) sprite_gfx_subset_3 = p[3];
}

/*
 * Drives the multi-frame trickle upload of newly loaded sprite/BG tiles
 * into VRAM. Each call sets the next NMI tile-map source/destination
 * page (one of 16 0x100-byte slots) and bumps the counter. When the
 * counter reaches 16 every slot has been uploaded and the function
 * becomes a no-op until something resets the counter.
 */
void Graphics_IncrementalVRAMUpload() {  // 80deff
  if (incremental_counter_for_vram == 16)
    return;

  static const uint8 kGraphics_IncrementalVramUpload_Dst[16] = { 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f };
  static const uint8 kGraphics_IncrementalVramUpload_Src[16] = { 0x0, 0x2, 0x4, 0x6, 0x8, 0xa, 0xc, 0xe, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1a, 0x1c, 0x1e };

  nmi_update_tilemap_dst = kGraphics_IncrementalVramUpload_Dst[incremental_counter_for_vram];
  nmi_update_tilemap_src = kGraphics_IncrementalVramUpload_Src[incremental_counter_for_vram] << 8;
  incremental_counter_for_vram++;
}

/*
 * After a fresh transition aux-tile load, expands the four 0x600-byte
 * decompressed BG packs (in WRAM 0x6000..0x73FF) into the 4bpp staging
 * buffer at WRAM 0x10000. The exact split between High and Low expanders
 * depends on whether the current aux_tile_theme_index is in the
 * "indoor" range (>=32) — indoor themes need a larger High region for
 * the dungeon BG layout, outdoor themes pack everything into one big
 * Low region.
 */
void PrepTransAuxGfx() {  // 80df1a
  Do3To4High16Bit(&g_ram[0x10000], &g_ram[0x6000], 0x40);
  if (aux_tile_theme_index >= 32) {
    Do3To4High16Bit(&g_ram[0x10800], &g_ram[0x6600], 0x80);
    Do3To4Low16Bit(&g_ram[0x11800], &g_ram[0x7200], 0x40);
  } else {
    Do3To4Low16Bit(&g_ram[0x10800], &g_ram[0x6600], 0xC0);
  }
}

/*
 * Bulk 3bpp → 4bpp expander for the upper-half tile rows in a buffer
 * that does NOT need the 0x180-byte source wrap that Expand3To4High
 * applies. Used by callers that pre-staged source data linearly into
 * the scratch buffer (e.g. PrepTransAuxGfx, AnimateMirrorWarp). Each
 * iteration processes one tile (8 inner-loop steps) and synthesizes
 * the third bitplane the same way as Expand3To4High.
 */
void Do3To4High16Bit(uint8 *dst, const uint8 *src, int num) {  // 80df4f
  do {
    const uint8 *src2 = src + 0x10;
    int n = 8;
    do {
      uint16 t = WORD(src[0]);
      uint8 u = src2[0];
      WORD(dst[0]) = t;
      WORD(dst[0x10]) = (t | (t >> 8) | u) << 8 | u;
      src += 2, src2 += 1, dst += 2;
    } while (--n);
    dst += 16, src = src2;
  } while (--num);

}

/*
 * Bulk 3bpp → 4bpp expander for the lower-half tile rows. Same shape
 * as Do3To4High16Bit but writes the third bitplane straight from src2
 * without the OR-combine — that's what produces the lower-half byte
 * pattern of the 4bpp tile.
 */
void Do3To4Low16Bit(uint8 *dst, const uint8 *src, int num) {  // 80dfb8
  do {
    const uint8 *src2 = src + 0x10;
    int n = 8;
    do {
      WORD(dst[0]) = WORD(src[0]);
      WORD(dst[0x10]) = src2[0];
      src += 2, src2 += 1, dst += 2;
    } while (--n);
    dst += 16, src = src2;
  } while (--num);
}

/*
 * After a sprite-set swap, expands the freshly decompressed sprite sheets
 * (in WRAM 0x7800..0x8FFF) into the 4bpp staging area at 0x10000. The
 * fourth slot (subset 3) gets the High expander only when its pack ID
 * is one of the four "high-format" sheets (0x52/0x53/0x5A/0x5B); every
 * other sheet uses the Low expander. This mirrors the same special-case
 * branch found in LoadSpriteGraphics.
 */
void LoadNewSpriteGFXSet() {  // 80e031
  Do3To4Low16Bit(&g_ram[0x10000], &g_ram[0x7800], 0xC0);
  if (sprite_gfx_subset_3 == 0x52 || sprite_gfx_subset_3 == 0x53 || sprite_gfx_subset_3 == 0x5a || sprite_gfx_subset_3 == 0x5b)
    Do3To4High16Bit(&g_ram[0x11800], &g_ram[0x8a00], 0x40);
  else
    Do3To4Low16Bit(&g_ram[0x11800], &g_ram[0x8a00], 0x40);
}

/*
 * Master tile-set initializer called whenever the game enters a new
 * area or restarts a scene. Steps:
 *   1. Loads the always-resident "common" sprites into VRAM 0x4400.
 *   2. Pulls the four sprite-sheet IDs from kSpriteTilesets and
 *      decompresses + uploads each into VRAM 0x5000..0x5FFF.
 *   3. Resolves the four auxiliary BG slots (preferring kAuxTilesets,
 *      falling back to kMainTilesets when an aux slot is zero) and
 *      uploads the eight BG sheets into VRAM 0x2000..0x3FFF.
 * The order of LoadBackgroundGraphics calls is reversed (slot 7 → slot 0)
 * because higher slot numbers are written to lower VRAM addresses.
 */
void InitializeTilesets() {  // 80e19b
  LoadCommonSprites();

  const uint8 *p = kSpriteTilesets[sprite_graphics_index];
  if (p[0]) sprite_gfx_subset_0 = p[0];
  if (p[1]) sprite_gfx_subset_1 = p[1];
  if (p[2]) sprite_gfx_subset_2 = p[2];
  if (p[3]) sprite_gfx_subset_3 = p[3];

  LoadSpriteGraphics(&g_zenv.vram[0x5000], sprite_gfx_subset_0, &g_ram[0x7800]);
  LoadSpriteGraphics(&g_zenv.vram[0x5400], sprite_gfx_subset_1, &g_ram[0x7e00]);
  LoadSpriteGraphics(&g_zenv.vram[0x5800], sprite_gfx_subset_2, &g_ram[0x8400]);
  LoadSpriteGraphics(&g_zenv.vram[0x5c00], sprite_gfx_subset_3, &g_ram[0x8a00]);

  const uint8 *mt = kMainTilesets[main_tile_theme_index];
  const uint8 *at = kAuxTilesets[aux_tile_theme_index];

  aux_bg_subset_0 = at[0] ? at[0] : mt[3];
  aux_bg_subset_1 = at[1] ? at[1] : mt[4];
  aux_bg_subset_2 = at[2] ? at[2] : mt[5];
  aux_bg_subset_3 = at[3] ? at[3] : mt[6];

  LoadBackgroundGraphics(&g_zenv.vram[0x2000], mt[0], 7, &g_ram[0x14000]);
  LoadBackgroundGraphics(&g_zenv.vram[0x2400], mt[1], 6, &g_ram[0x14000]);
  LoadBackgroundGraphics(&g_zenv.vram[0x2800], mt[2], 5, &g_ram[0x14000]);
  LoadBackgroundGraphics(&g_zenv.vram[0x2c00], aux_bg_subset_0, 4, &g_ram[0x6000]);
  LoadBackgroundGraphics(&g_zenv.vram[0x3000], aux_bg_subset_1, 3, &g_ram[0x6600]);
  LoadBackgroundGraphics(&g_zenv.vram[0x3400], aux_bg_subset_2, 2, &g_ram[0x6c00]);
  LoadBackgroundGraphics(&g_zenv.vram[0x3800], aux_bg_subset_3, 1, &g_ram[0x7200]);
  LoadBackgroundGraphics(&g_zenv.vram[0x3c00], mt[7], 0, &g_ram[0x14000]);
}

/*
 * Loads the boot-time default graphics into VRAM. Performs an inline
 * 3bpp → 4bpp expansion of the always-resident sprite-pack 0 directly
 * into VRAM at 0x4000 (64 tiles), using a tiny WRAM scratch (g_ram+0xBF)
 * to hold the OR-combined high bitplane between the two inner loops.
 * Then uploads the three 2bpp HUD font/BG packs (0x6A/0x6B/0x69) to
 * VRAM 0x7000/0x7400/0x7800.
 */
void LoadDefaultGraphics() {  // 80e2d0
  const uint8 *src = GetCompSpritePtr(0);

  uint16 *vram_ptr = &g_zenv.vram[0x4000];
  uint16 *tmp = (uint16 *)&g_ram[0xbf];
  int num = 64;
  do {
    for (int i = 7; i >= 0; i--, src += 2) {
      *vram_ptr++ = WORD(src[0]);
      tmp[i] = src[0] | src[1];
    }
    for (int i = 7; i >= 0; i--, src++) {
      *vram_ptr++ = src[0] | (src[0] | tmp[i]) << 8;
    }
  } while (--num);

  // Load 2bpp graphics used for hud
  DecompAndUpload2bpp(&g_zenv.vram[0x7000], 0x6a);
  DecompAndUpload2bpp(&g_zenv.vram[0x7400], 0x6b);
  DecompAndUpload2bpp(&g_zenv.vram[0x7800], 0x69);
}

/*
 * Loads the BG3 layer's 2bpp character data for attract-mode story
 * pages. The story art is stored in sprite-pack 0x67 in 2bpp form and
 * goes straight to VRAM 0x7800 where the BG3 tilemap will reference it.
 */
void Attract_LoadBG3GFX() {  // 80e36d
  // load 2bpp gfx for attract images
  DecompAndUpload2bpp(&g_zenv.vram[0x7800], 0x67);
}

/*
 * Streams a "half-slot" worth of CHR (sprite tiles) into the dynamic
 * tile region used for spell animations and other transient effects.
 * load_chr_halfslot_even_odd is a 1-based selector telling this routine
 * which animation to load; on odd ticks the upper half loads and on even
 * ticks the lower half does. The chosen pack ID comes from
 * kGraphicsHalfSlotPacks; if kGraphicsLoadSp6 has a non-negative entry,
 * palette_sp6r_indoors is updated and the matching sprite-environment
 * palette is reloaded so the new tiles ship with their correct colors.
 * Each pass copies 32 tiles, expanding 24-byte 3bpp groups inline (the
 * same algorithm as Expand3To4High but without the 0x180 wrap).
 */
void Graphics_LoadChrHalfSlot() {  // 80e3fa
  int k = load_chr_halfslot_even_odd;
  if (k == 0)
    return;

  int8 sp6 = kGraphicsLoadSp6[k - 1];
  if (sp6 >= 0) {
    palette_sp6r_indoors = sp6;
    if (k == 1) {
      palette_sp6r_indoors = 10;
      overworld_palette_aux_or_main = 0x200;
      Palette_Load_SpriteEnvironment();
      flag_update_cgram_in_nmi++;
    } else {
      overworld_palette_aux_or_main = 0x200;
      Palette_Load_SpriteEnvironment_Dungeon();
      flag_update_cgram_in_nmi++;
    }
  }
  int tilebytes = 0x44;
  int bank_offs = 0;
  load_chr_halfslot_even_odd++;

  if (load_chr_halfslot_even_odd & 1) {
    load_chr_halfslot_even_odd = 0;
    if (k != 18) {
      bank_offs = 0x300;
      tilebytes = 0x46;
      if (k == 2)
        flag_custom_spell_anim_active = 0;
    }
  }
  BYTE(nmi_load_target_addr) = tilebytes;
  nmi_subroutine_index = 11;

  k = kGraphicsHalfSlotPacks[k - 1];
  if (k == 1)
    k = misc_sprites_graphics_index;

  const uint8 *srcp = GetCompSpritePtr(k) + bank_offs;
  uint8 sprdata[24];
  int num = 32;
  uint8 *dst = &g_ram[0x11000];

  do {
    for (int i = 0; i < 24; i++)
      sprdata[i] = *srcp++;

    uint8 *src = sprdata, *src2 = sprdata + 16;
    int n = 8;
    do {
      uint16 t = WORD(src[0]);
      uint8 u = src2[0];
      WORD(dst[0]) = t;
      WORD(dst[16]) = (t | (t >> 8) | u) << 8 | u;
      src += 2, src2 += 1, dst += 2;
    } while (--n);
    dst += 16;
  } while (--num);
}

/*
 * Copies the dialogue VWF font tiles into VRAM at 0x7000. The font is
 * stored already in 4bpp form inside the kDialogueFont MemBlk, so this
 * is just a 4 KB linear memcpy with no expansion step.
 */
void TransferFontToVRAM() {  // 80e556
  memcpy(&g_zenv.vram[0x7000], FindIndexInMemblk(kDialogueFont(0), 0).ptr, 0x800 * sizeof(uint16));
}

/*
 * VRAM-targeted variant of Do3To4High16Bit. Reads 64 tiles of 3bpp
 * source data from `decomp_addr` and writes the corresponding 4bpp
 * tiles directly to vram_ptr (no intermediate WRAM staging). The temp
 * row buffer is borrowed from dung_line_ptrs_row0 — that storage is
 * unused while tilesets are being uploaded so it can double as scratch.
 */
void Do3To4High(uint16 *vram_ptr, const uint8 *decomp_addr) {  // 80e5af
  for (int j = 0; j < 64; j++) {
    uint16 *t = (uint16 *)&dung_line_ptrs_row0;
    for (int i = 7; i >= 0; i--, decomp_addr += 2) {
      uint16 d = *(uint16 *)decomp_addr;
      t[i] = (d | (d >> 8)) & 0xff;
      *vram_ptr++ = d;
    }
    for (int i = 7; i >= 0; i--, decomp_addr += 1) {
      uint8 d = *decomp_addr;
      *vram_ptr++ = d | (t[i] | d) << 8;
    }
  }
}

/*
 * VRAM-targeted variant of Do3To4Low16Bit. Same 64-tile loop, but the
 * third bitplane is written straight from the decompressed source byte
 * with no OR-combine, producing the lower-half tile pattern.
 */
void Do3To4Low(uint16 *vram_ptr, const uint8 *decomp_addr) {  // 80e63c
  for (int j = 0; j < 64; j++) {
    for (int i = 0; i < 8; i++, decomp_addr += 2)
      *vram_ptr++ = *(uint16 *)decomp_addr;
    for (int i = 0; i < 8; i++, decomp_addr += 1)
      *vram_ptr++ = *decomp_addr;
  }
}

/*
 * Decompresses sprite-pack `gfx_pack` into `decomp_addr` (in WRAM) and
 * pushes the resulting tiles directly to VRAM at `vram_ptr`. The seven
 * "high-format" sprite packs (0x52/0x53/0x5A/0x5B/0x5C/0x5E/0x5F) need
 * the High expander; every other pack uses the Low expander.
 */
void LoadSpriteGraphics(uint16 *vram_ptr, int gfx_pack, uint8 *decomp_addr) {  // 80e583
  Decomp_spr(decomp_addr, gfx_pack);
  if (gfx_pack == 0x52 || gfx_pack == 0x53 || gfx_pack == 0x5a || gfx_pack == 0x5b ||
      gfx_pack == 0x5c || gfx_pack == 0x5e || gfx_pack == 0x5f)
    Do3To4High(vram_ptr, decomp_addr);
  else
    Do3To4Low(vram_ptr, decomp_addr);
}

/*
 * BG-tile counterpart to LoadSpriteGraphics. Decompresses BG-pack
 * `gfx_pack` and uploads it to VRAM at `vram_ptr`. The expander choice
 * depends on the BG slot and the active main_tile_theme_index — indoor
 * dungeon themes (>=0x20) use High for slots 7,2,3,4; outdoor themes
 * use High for slots >= 4. Everything else uses Low.
 */
void LoadBackgroundGraphics(uint16 *vram_ptr, int gfx_pack, int slot, uint8 *decomp_addr) {  // 80e609
  Decomp_bg(decomp_addr, gfx_pack);
  if ((main_tile_theme_index >= 0x20) ? (slot == 7 || slot == 2 || slot == 3 || slot == 4) : (slot >= 4))
    Do3To4High(vram_ptr, decomp_addr);
  else
    Do3To4Low(vram_ptr, decomp_addr);
}

/*
 * Loads the always-resident "common" sprites that every scene needs.
 * The misc-sprites pack goes to VRAM 0x4400. Outside of the file-select
 * screen the next two slots are filled with packs 6 and 7; on the file-
 * select screen they're replaced by the special packs 94 and 95 that
 * hold the file-select-only artwork.
 */
void LoadCommonSprites() {  // 80e6b7
  Do3To4High(&g_zenv.vram[0x4400], GetCompSpritePtr(misc_sprites_graphics_index));
  if (main_module_index != 1) {
    Do3To4Low(&g_zenv.vram[0x4800], GetCompSpritePtr(6));
    Do3To4Low(&g_zenv.vram[0x4c00], GetCompSpritePtr(7));
  } else {
    // select file
    LoadSpriteGraphics(&g_zenv.vram[0x4800], 94, &g_ram[0x14000]);
    LoadSpriteGraphics(&g_zenv.vram[0x4c00], 95, &g_ram[0x14000]);
  }
}

/*
 * Decompresses sprite-pack `gfx` to `dst`.
 *   - Packs below index 12 are clamped up to 12 because the lower IDs
 *     are reserved/unused; this guard prevents the decoder from reading
 *     a corrupt or empty MemBlk.
 *   - When the asset block is exactly 0x600 bytes (and below pack 103)
 *     the data is already raw and is copied verbatim.
 *   - Otherwise the LZ-style Decompress routine is invoked.
 * Returns the number of bytes written into dst.
 */
int Decomp_spr(uint8 *dst, int gfx) {  // 80e772
  if (gfx < 12)
    gfx = 12; // ensure it wont decode bad sheets.
  MemBlk blk = kSprGfx(gfx);
  const uint8 *sprite_data = GetCompSpritePtr(gfx);
  // If the size is not 0x600 then it's compressed
  if (gfx >= 103 || blk.size != 0x600)
    return Decompress(dst, blk.ptr);
  memcpy(dst, blk.ptr, 0x600);
  return 0x600;
}

/* BG-graphics sibling of Decomp_spr — looks up the BG asset table and
 * always runs the LZ decoder (BG packs are never stored uncompressed). */
int Decomp_bg(uint8 *dst, int gfx) {  // 80e78f
  return Decompress(dst, kBgGfx(gfx).ptr);
}

/*
 * Generic LZ-style decompressor used for every compressed graphics pack
 * in the ROM. Returns the number of bytes written to `dst`. The format
 * is the standard ALttP compression scheme:
 *   cmd byte 0xFF                — end-of-stream marker
 *   cmd byte top 3 bits != 0xE0  — short-form: low 5 bits = length-1,
 *                                  top 3 bits = command
 *   cmd byte top 3 bits == 0xE0  — extended-form: next byte combined
 *                                  with the bottom 2 bits of cmd gives
 *                                  a 10-bit length; bits 2..4 become
 *                                  the new command
 *   cmd 0x00 (raw)               — copy `len` literal bytes
 *   cmd 0x80 (window copy)       — read 16-bit offset, copy `len` bytes
 *                                  from dst_org+offset (LZ back-ref)
 *   cmd 0x20 (single fill)       — repeat one byte `len` times
 *   cmd 0x40 (alternating)       — read two bytes lo,hi and write them
 *                                  alternately for `len` total bytes
 *   cmd 0x60 (incrementing)      — write a byte then increment it,
 *                                  producing v, v+1, v+2 ... for `len`
 * The (cmd << 3) reshuffle in the extended-form path keeps the command
 * bits in the same position regardless of which form was used.
 */
int Decompress(uint8 *dst, const uint8 *src) {  // 80e79e
  uint8 *dst_org = dst;
  int len;
  for (;;) {
    uint8 cmd = *src++;
    if (cmd == 0xff)
      return dst - dst_org;
    if ((cmd & 0xe0) != 0xe0) {
      len = (cmd & 0x1f) + 1;
      cmd &= 0xe0;
    } else {
      len = *src++;
      len += ((cmd & 3) << 8) + 1;
      cmd = (cmd << 3) & 0xe0;
    }
    //printf("%d: %d,%d\n", (int)(dst - dst_org), cmd, len);
    if (cmd == 0) {
      do {
        *dst++ = *src++;
      } while (--len);
    } else if (cmd & 0x80) {
      uint32 offs = *src++;
      offs |= *src++ << 8;
      do {
        *dst++ = dst_org[offs++];
      } while (--len);
    } else if (!(cmd & 0x40)) {
      uint8 v = *src++;
      do {
        *dst++ = v;
      } while (--len);
    } else if (!(cmd & 0x20)) {
      uint8 lo = *src++;
      uint8 hi = *src++;
      do {
        *dst++ = lo;
        if (--len == 0)
          break;
        *dst++ = hi;
      } while (--len);
    } else {
      // copy bytes with the byte incrementing by 1 in between
      uint8 v = *src++;
      do {
        *dst++ = v;
      } while (v++, --len);
    }
  }
}

/*
 * Wipes HUD palette entries 16..23 (sub-palettes 4 and 5) to black and
 * primes a fresh palette-fade cycle so the HUD can be faded back in.
 * The CGRAM dirty flag is set so the next NMI uploads the cleared
 * entries to hardware.
 */
void ResetHUDPalettes4and5() {  // 80eb29
  for (int i = 0; i < 8; i++)
    main_palette_buffer[16 + i] = 0;
  palette_filter_countdown = 0;
  darkening_or_lightening_screen = 2;
  flag_update_cgram_in_nmi++;
}

/*
 * Single-step dither fade applied just to the inventory-history pad of
 * the HUD palette (entries 0x10..0x17). Used while item-acquisition
 * dialogs cross-fade the bottom-row icons.
 */
void PaletteFilterHistory() {  // 80eb5e
  PaletteFilter_Range(0x10, 0x18);
  PaletteFilter_IncrCountdown();
}

/*
 * Sets up the wish-pond color-add effect: enables BG2 in the subscreen
 * mask and turns on the additive color-math (CGADSUB) so the cyan glow
 * accumulates. Then resets the per-frame fade state via the shared
 * inner helper.
 */
void PaletteFilter_WishPonds() {  // 80ebc5
  TS_copy = 2;
  CGADSUB_copy = 0x30;
  PaletteFilter_WishPonds_Inner();
}

/*
 * Crystal-dungeon variant of the wish-pond fade: only BG1 is added on
 * the subscreen, so the crystal sparkle reads as a brighter highlight
 * over the dungeon BG instead of a translucent blue glow.
 */
void PaletteFilter_Crystal() {  // 80ebcf
  TS_copy = 1;
  PaletteFilter_WishPonds_Inner();
}

/*
 * Shared init for both wish-pond and crystal fades. Clears 8 sprite
 * palette entries (0xD0..0xD7) so the affected sprites start fully
 * black, then primes a fresh fade-in cycle by zeroing the fade counter
 * and pointing the direction marker at "lightening".
 */
void PaletteFilter_WishPonds_Inner() {  // 80ebd3
  for (int i = 0; i < 8; i++)
    main_palette_buffer[0xd0 + i] = 0;
  palette_filter_countdown = 0;
  darkening_or_lightening_screen = 2;
  flag_update_cgram_in_nmi++;
}

/*
 * Restores the 8 sprite palette entries at 0xD0..0xD7 from their backup
 * in aux_palette_buffer and reverts the additive color-math state. Used
 * to undo the wish-pond/crystal fade-in once the effect is finished.
 */
void PaletteFilter_RestoreSP5F() {  // 80ebf2
  for (int i = 7; i >= 0; i--)
    main_palette_buffer[208 + i] = aux_palette_buffer[208 + i];
  TS_copy = 0;
  CGADSUB_copy = 32;
  flag_update_cgram_in_nmi++;
}

/*
 * Drives the wish-pond / crystal fade by running TWO PaletteFilter_Range
 * steps per call (so the effect ramps at half a frame per dither tick).
 * Bails early if the countdown wraps to zero, which signals "fade
 * direction just inverted, do not over-shoot this frame."
 */
void PaletteFilter_SP5F() {  // 80ec0d
  for (int i = 0; i != 2; i++) {
    PaletteFilter_Range(208, 216);
    PaletteFilter_IncrCountdown();
    if (palette_filter_countdown == 0)
      break;
  }
}

/*
 * Drives the icy palette fade for Kholdstare's shell. The shell's color
 * range starts at 0x40 in vanilla but the bug-fix mode shifts it to
 * 0x50 (the original index targeted the wrong palette row). On the
 * first call (subsubmodule_index == 0) the working entries are seeded
 * from the aux buffer; subsequent calls advance two PaletteFilter_Range
 * steps per frame and clear the BG layer-add flag once the fade ends.
 */
void KholdstareShell_PaletteFiltering() {  // 80ec79
  int t = (enhanced_features0 & kFeatures0_MiscBugFixes) ? 0x50 : 0x40;
  if (subsubmodule_index == 0) {
    memcpy(main_palette_buffer + t, aux_palette_buffer + t, 8 * sizeof(uint16));
    palette_filter_countdown = 0;
    darkening_or_lightening_screen = 0;
    flag_update_cgram_in_nmi++;
    subsubmodule_index = 1;
    return;
  }
  for (int i = 0; i != 2; i++) {
    PaletteFilter_Range(t, t + 8);
    PaletteFilter_IncrCountdown();
    if (palette_filter_countdown == 0) {
      TS_copy = 0;
      break;
    }
  }
}

/*
 * Animates one of Agahnim's three shadow color groups during his warp
 * cutscene. `k` selects which group (0..2); the per-group state is
 * stored in agahnim_pal_setting[k] (countdown) and [k+3] (direction).
 * Each call runs two PaletteFilter_Range passes against the slice
 * located at kPaletteFilter_Agahnim_Tab[k]/2, then writes the updated
 * countdown/direction back into agahnim_pal_setting before flagging
 * CGRAM dirty.
 */
void AgahnimWarpShadowFilter(int k) {  // 80ecca
  palette_filter_countdown = agahnim_pal_setting[k];
  darkening_or_lightening_screen = agahnim_pal_setting[k + 3];
  int t = kPaletteFilter_Agahnim_Tab[k] >> 1;
  for (int i = 0; i < 2; i++) {
    PaletteFilter_Range(t, t + 8);
    if (++palette_filter_countdown == 0x1f) {
      palette_filter_countdown = 0;
      darkening_or_lightening_screen ^= 2;
      break;
    }
  }
  agahnim_pal_setting[k] = palette_filter_countdown;
  agahnim_pal_setting[k + 3] = darkening_or_lightening_screen;
  flag_update_cgram_in_nmi++;
}

/*
 * Single-frame brightening pass for the title-screen intro fade-in.
 * Lifts the sprite-and-gear half of the palette (0x100..0x19F) and the
 * upper BG half (0xC0..0xFF) one channel-step closer to their target
 * colors, decrements the countdown, and flags CGRAM dirty.
 */
void Palette_FadeIntroOneStep() {  // 80ed7c
  PaletteFilter_RestoreAdditive(0x100, 0x1a0);
  PaletteFilter_RestoreAdditive(0xc0, 0x100);
  BYTE(palette_filter_countdown) -= 1;
  flag_update_cgram_in_nmi++;
}

/*
 * Companion to Palette_FadeIntroOneStep — handles the LOWER BG palette
 * half (0x40..0xBF) by running the additive restore twice (the original
 * ROM intentionally calls it the same range twice; preserved verbatim).
 */
void Palette_FadeIntro2() {  // 80ed8f
  PaletteFilter_RestoreAdditive(0x40, 0xc0);
  PaletteFilter_RestoreAdditive(0x40, 0xc0);
  BYTE(palette_filter_countdown) -= 1;
  flag_update_cgram_in_nmi++;
}

/*
 * Walks main_palette_buffer entries [from/2 .. to/2) and nudges each
 * R/G/B channel one tick toward the matching aux_palette_buffer entry.
 * The from/to arguments are byte offsets (so they're shifted right
 * once to convert to uint16 indices). When every channel of an entry
 * already matches the target the entry is left untouched. Used to
 * "fade up" palette ranges back to their original colors.
 */
void PaletteFilter_RestoreAdditive(int from, int to) {  // 80edca
  from >>= 1, to >>= 1;
  do {
    uint16 c = main_palette_buffer[from], cx = c;
    uint16 d = aux_palette_buffer[from];
    if ((c & 0x1f) != (d & 0x1f))
      cx += 1;
    if ((c & 0x3e0) != (d & 0x3e0))
      cx += 0x20;
    if ((c & 0x7c00) != (d & 0x7c00))
      cx += 0x400;
    main_palette_buffer[from] = cx;
  } while (++from != to);
}

/*
 * Subtractive counterpart to PaletteFilter_RestoreAdditive: nudges each
 * R/G/B channel one tick AWAY from the aux target (i.e. fades a range
 * down). Used by the blinding-white effect to ramp the palette down
 * to white before the flash, and by similar effects.
 */
void PaletteFilter_RestoreSubtractive(uint16 from, uint16 to) {  // 80ee21
  from >>= 1, to >>= 1;
  do {
    uint16 c = main_palette_buffer[from], cx = c;
    uint16 d = aux_palette_buffer[from];
    if ((c & 0x1f) != (d & 0x1f))
      cx -= 1;
    if ((c & 0x3e0) != (d & 0x3e0))
      cx -= 0x20;
    if ((c & 0x7c00) != (d & 0x7c00))
      cx -= 0x400;
    main_palette_buffer[from] = cx;
  } while (++from != to);
}

/*
 * Sets up the blinding-white-flash transition used by the Magic Mirror.
 * Fills the entire aux palette with pure white (0x7FFF) so the
 * subsequent fade-down treats white as the target. Snapshots the
 * backdrop color into entry 32 and primes the fade direction. On the
 * specific overworld screen 27 (the eastern Light World plateau) the
 * top-of-screen entries are forced to black instead of white to avoid
 * a noticeable horizon glitch. The mirror_vars counter is initialized
 * to 8 frames of pre-roll.
 */
void PaletteFilter_InitializeWhiteFilter() {  // 80ee78
  for (int i = 0; i < 256; i++)
    aux_palette_buffer[i] = 0x7fff;
  main_palette_buffer[32] = main_palette_buffer[0];
  palette_filter_countdown = 0;
  darkening_or_lightening_screen = 2;
  if (overworld_screen_index == 27) {
    aux_palette_buffer[0] = aux_palette_buffer[32] = 0;
    main_palette_buffer[0] = main_palette_buffer[32] = 0;
  }
  mirror_vars.ctr = 8;
  mirror_vars.ctr2 = 0;
}

/*
 * Per-frame router for the mirror-warp animation. While the pre-roll
 * counter (mirror_vars.ctr) is still ticking down, every frame is spent
 * advancing AnimateMirrorWarp's state machine. Once the counter hits
 * zero the warp transitions into the blinding-white phase, and the
 * counter resets to 2 so the white flash holds for two more frames.
 */
void MirrorWarp_RunAnimationSubmodules() {  // 80eee7
  if (--mirror_vars.ctr) {
    AnimateMirrorWarp();
    return;
  }
  mirror_vars.ctr = 2;
  PaletteFilter_BlindingWhite();
}

/*
 * One frame of the blinding-white mirror-warp effect. Direction 0xFF
 * means the effect has finished and there is nothing left to do. While
 * fading IN (direction == 2) the BG and sprite ranges are pushed
 * additively toward white; while fading OUT they're pulled subtractively
 * back toward the original colors. Both passes finish by calling
 * PaletteFilter_StartBlindingWhite to advance the countdown.
 */
void PaletteFilter_BlindingWhite() {  // 80eef1
  if (darkening_or_lightening_screen == 0xff)
    return;

  if (darkening_or_lightening_screen == 2) {
    PaletteFilter_RestoreAdditive(0x40, 0x1b0);
    PaletteFilter_RestoreAdditive(0x1c0, 0x1e0);
  } else {
    PaletteFilter_RestoreSubtractive(0x40, 0x1b0);
    PaletteFilter_RestoreSubtractive(0x1c0, 0x1e0);
  }
  PaletteFilter_StartBlindingWhite();
}

/*
 * Drives the timing for the blinding-white flash. Counts up while the
 * fade-in plays (until tick 66 when the effect locks at "fully white"
 * and the post-flash hold begins) and counts up while the fade-out
 * plays (until tick 31 when the direction inverts). On the dungeon-
 * Triforce module the lower 240 lines of the dynamic HDMA table are
 * reset to a uniform value so the post-flash window matches the new
 * scene's geometry. Always bumps the CGRAM dirty flag so NMI ships the
 * latest palette.
 */
void PaletteFilter_StartBlindingWhite() {  // 80ef27
  main_palette_buffer[0] = main_palette_buffer[32];
  if (!darkening_or_lightening_screen) {
    if (++palette_filter_countdown == 66) {
      darkening_or_lightening_screen = 0xff;
      mirror_vars.ctr = 32;
    }
  } else {
    if (++palette_filter_countdown == 31) {
      darkening_or_lightening_screen ^= 2;
      if (main_module_index != 21)
        return;
      zelda_snes_dummy_write(HDMAEN, 0);
      HDMAEN_copy = 0;
      for (int i = 0; i < 240; i++)
        hdma_table_dynamic[i] = 0x778;
      HDMAEN_copy = 0xc0;
    }
  }
  flag_update_cgram_in_nmi++;
}

/*
 * Triforce-room variant of the blinding-white fade. Restores the FULL
 * palette (0x40..0x1FF) additively in one pass instead of the split
 * BG/sprite ranges used by the regular mirror-warp version, then chains
 * into the shared timing function.
 */
void PaletteFilter_BlindingWhiteTriforce() {  // 80ef8a
  PaletteFilter_RestoreAdditive(0x40, 0x200);
  PaletteFilter_StartBlindingWhite();
}

/*
 * Drives the per-frame palette shift for the whirlpool warp's blue glow.
 * On odd frames every entry from 0x20..0xFF gets its blue channel
 * incremented one notch (clamped at the 5-bit max). The backdrop entry
 * is mirrored from entry 32. Mosaic intensity ramps in 16-step jumps
 * every other tick, and once the countdown reaches 31 the next sub-
 * submodule takes over and the mosaic locks at maximum.
 */
void PaletteFilter_WhirlpoolBlue() {  // 80ef97
  if (frame_counter & 1) {
    for (int i = 0x20; i != 0x100; i++) {
      uint16 t = main_palette_buffer[i];
      if ((t & 0x7C00) != 0x7C00)
        t += 0x400;
      main_palette_buffer[i] = t;
    }
    main_palette_buffer[0] = main_palette_buffer[32];
    if (!(palette_filter_countdown & 1))
      mosaic_level += 16;
    if (++palette_filter_countdown == 31) {
      palette_filter_countdown = 0;
      subsubmodule_index++;
      mosaic_level = 0xf0;
    }
  }
  BGMODE_copy = 9;
  MOSAIC_copy = mosaic_level | 3;
  flag_update_cgram_in_nmi++;
}

/*
 * Second phase of the whirlpool warp's color transform. After the blue
 * channel has been pushed to maximum, this strips both red and green
 * one tick at a time so the picture dissolves into a pure-blue field.
 * Once the countdown reaches 31 the next sub-submodule begins.
 */
void PaletteFilter_IsolateWhirlpoolBlue() {  // 80f00c
  for (int i = 0x20; i != 0x100; i++) {
    uint16 t = main_palette_buffer[i];
    if (t & 0x3e0)
      t -= 0x20;
    if (t & 0x1f)
      t -= 1;
    main_palette_buffer[i] = t;
  }
  main_palette_buffer[0] = main_palette_buffer[32];
  if (++palette_filter_countdown == 31) {
    palette_filter_countdown = 0;
    subsubmodule_index++;
    mosaic_level = 0xf0;
  }
  BGMODE_copy = 9;
  MOSAIC_copy = mosaic_level | 3;
  flag_update_cgram_in_nmi++;
}

/*
 * Restores the blue channel back toward each entry's target value as
 * the destination map fades in. Same odd-frame cadence and mosaic
 * unwind as PaletteFilter_WhirlpoolBlue, but the blue is decremented
 * (and mosaic intensity drops by 16 per pair of frames) until tick 31
 * advances the sub-submodule and the mosaic returns to zero.
 */
void PaletteFilter_WhirlpoolRestoreBlue() {  // 80f04a
  if (frame_counter & 1) {
    for (int i = 0x20; i != 0x100; i++) {
      uint16 u = aux_palette_buffer[i] & 0x7c00;
      uint16 t = main_palette_buffer[i];
      if ((t & 0x7C00) != u)
        t -= 0x400;
      main_palette_buffer[i] = t;
    }
    main_palette_buffer[0] = main_palette_buffer[32];
    if (!(palette_filter_countdown & 1))
      mosaic_level -= 16;
    if (++palette_filter_countdown == 31) {
      palette_filter_countdown = 0;
      subsubmodule_index++;
      mosaic_level = 0;
    }
  }
  BGMODE_copy = 9;
  MOSAIC_copy = mosaic_level | 3;
  flag_update_cgram_in_nmi++;
}

/*
 * Final phase of the whirlpool warp transition. Walks every pal entry
 * in 0x20..0xFF and pushes the red and green channels back up toward
 * their target values one tick at a time, restoring full color. After
 * 31 ticks the sub-submodule advances and the warp animation ends.
 */
void PaletteFilter_WhirlpoolRestoreRedGreen() {  // 80f0c7
  for (int i = 0x20; i != 0x100; i++) {
    uint16 u0 = aux_palette_buffer[i] & 0x3e0;
    uint16 u1 = aux_palette_buffer[i] & 0x1f;
    uint16 t = main_palette_buffer[i];
    if ((t & 0x3e0) != u0)
      t += 0x20;
    if ((t & 0x1f) != u1)
      t += 1;
    main_palette_buffer[i] = t;
  }
  main_palette_buffer[0] = main_palette_buffer[32];
  if (++palette_filter_countdown == 31) {
    palette_filter_countdown = 0;
    subsubmodule_index++;
  }
  flag_update_cgram_in_nmi++;
}

/*
 * Hard-edged subtractive fade for the BG palette half (0x40..0xFF).
 * "Strict" because once the countdown reaches 0x20 the direction flag
 * is locked at 0xFF (terminal state) and BG layer-add is disabled in
 * one shot rather than gradually. Used by transitions that need to
 * crash the BG to black quickly.
 */
void PaletteFilter_RestoreBGSubstractiveStrict() {  // 80f135
  if (darkening_or_lightening_screen == 255)
    return;
  PaletteFilter_RestoreSubtractive(0x40, 0x100);
  if (++palette_filter_countdown == 0x20) {
    darkening_or_lightening_screen = 255;
    WORD(TS_copy) = 0;
  }
  flag_update_cgram_in_nmi++;
}

/*
 * Additive counterpart to PaletteFilter_RestoreBGSubstractiveStrict.
 * Pushes the BG palette range one tick toward its target each frame
 * and bumps the countdown unconditionally — the strict variant simply
 * lets the countdown overflow back to zero on its own.
 */
void PaletteFilter_RestoreBGAdditiveStrict() {  // 80f169
  PaletteFilter_RestoreAdditive(0x40, 0x100);
  palette_filter_countdown++;
  flag_update_cgram_in_nmi++;
}

/*
 * Drives Trinexx's red-head shell flash. byte_7E04BE acts as a per-step
 * delay countdown — the body runs only when it reaches zero. On each
 * active step every red-channel value in entries 0x41..0x47 is pushed
 * one tick brighter (clamped at 0x1F). After 12 successful ticks the
 * effect resets; otherwise the delay is reseeded to 3 frames.
 */
void Trinexx_FlashShellPalette_Red() {  // 80f183
  if (!byte_7E04BE) {
    for (int i = 0; i < 7; i++) {
      uint16 v = main_palette_buffer[0x41 + i];
      main_palette_buffer[0x41 + i] = (v & 0xffe0) | ((v & 0x1f) + ((v & 0x1f) != 0x1f));
    }
    flag_update_cgram_in_nmi++;
    if (++byte_7E04C0 >= 12) {
      byte_7E04C0 = byte_7E04BE = 0;
      return;
    }
    byte_7E04BE = 3;
  }
  byte_7E04BE--;
}

/*
 * Reverse-direction sibling of Trinexx_FlashShellPalette_Red. Pulls the
 * red channel one tick down toward the aux-buffer target each frame
 * (only when it doesn't already match), with the same 12-step,
 * 3-frame-delay schedule.
 */
void Trinexx_UnflashShellPalette_Red() {  // 80f1cf
  if (!byte_7E04BE) {
    for (int i = 0; i < 7; i++) {
      uint16 u = aux_palette_buffer[0x41 + i];
      uint16 v = main_palette_buffer[0x41 + i];
      main_palette_buffer[0x41 + i] = (v & 0xffe0) | ((v & 0x1f) - ((v & 0x1f) != (u & 0x1f)));
    }
    flag_update_cgram_in_nmi++;
    if (++byte_7E04C0 >= 12) {
      byte_7E04C0 = byte_7E04BE = 0;
      return;
    }
    byte_7E04BE = 3;
  }
  byte_7E04BE--;
}

/*
 * Blue-head counterpart to Trinexx_FlashShellPalette_Red. Brightens
 * just the blue channel of entries 0x41..0x47 one tick at a time, with
 * its own delay variable (byte_7E04BF) and tick counter (byte_7E04C1)
 * so the two heads can flash independently.
 */
void Trinexx_FlashShellPalette_Blue() {  // 80f207
  if (!byte_7E04BF) {
    for (int i = 0; i < 7; i++) {
      uint16 v = main_palette_buffer[0x41 + i];
      main_palette_buffer[0x41 + i] = (v & ~0x7c00) | (v & 0x7c00) + (((v & 0x7c00) != 0x7c00) << 10);
    }
    flag_update_cgram_in_nmi++;
    if (++byte_7E04C1 >= 12) {
      byte_7E04C1 = byte_7E04BF = 0;
      return;
    }
    byte_7E04BF = 3;
  }
  byte_7E04BF--;

}

/*
 * Reverse of Trinexx_FlashShellPalette_Blue: walks the blue channel
 * back down toward the aux-buffer target. Same delay/counter pair as
 * the brightening pass so the timing matches one-to-one.
 */
void Trinexx_UnflashShellPalette_Blue() {  // 80f253
  if (!byte_7E04BF) {
    for (int i = 0; i < 7; i++) {
      uint16 u = aux_palette_buffer[0x41 + i];
      uint16 v = main_palette_buffer[0x41 + i];
      main_palette_buffer[0x41 + i] = (v & ~0x7c00) | (v & 0x7c00) - (((v & 0x7c00) != (u & 0x7c00)) << 10);
    }
    flag_update_cgram_in_nmi++;
    if (++byte_7E04C1 >= 12) {
      byte_7E04C1 = byte_7E04BF = 0;
      return;
    }
    byte_7E04BF = 3;
  }
  byte_7E04BF--;
}

/*
 * Begins the iris-spotlight CLOSE animation. Seeds the spotlight at its
 * widest radius (0x7E) and selects the closing direction (state 0).
 */
void IrisSpotlight_close() {  // 80f28b
  SpotlightInternal(0x7e, 0);
}

/*
 * Begins the iris-spotlight OPEN animation. Seeds the radius at zero
 * and selects the opening direction (state 2).
 */
void Spotlight_open() {  // 80f295
  SpotlightInternal(0, 2);
}

/*
 * Common setup shared by Spotlight_open and IrisSpotlight_close.
 * Stores the initial radius and direction, disables HDMA temporarily,
 * configures a fresh HDMA chain to feed the WH0 windowing register
 * line by line, masks BG/sprite layers to use the window, sets a dim
 * outdoor backdrop color, and finally invokes IrisSpotlight_ConfigureTable
 * to fill in the first frame of the table. Force-blank is dropped to
 * brightness 0xF so the new HDMA window is visible immediately.
 */
void SpotlightInternal(uint8 x, uint8 y) {  // 80f29d
  spotlight_var1 = x;
  spotlight_var2 = y;

  zelda_snes_dummy_write(HDMAEN, 0);
  HdmaSetup(0xF2FB, 0xF2FB, 0x41, (uint8)WH0, (uint8)WH0, 0);

  W12SEL_copy = 0x33;
  W34SEL_copy = 3;
  WOBJSEL_copy = 0x33;
  TMW_copy = TM_copy;
  TSW_copy = TS_copy;
  if (!player_is_indoors) {
    COLDATA_copy0 = 0x20;
    COLDATA_copy1 = 0x40;
    COLDATA_copy2 = 0x80;
  }
  IrisSpotlight_ConfigureTable();
  HDMAEN_copy = 0x80;
  INIDISP_copy = 0xf;
}

/*
 * Builds the per-scanline window-edge table that produces the iris
 * spotlight effect centered on Link.
 *
 * The table is filled symmetrically outward from Link's screen Y: r4
 * walks UP from the center while r6 walks DOWN, and each pair of rows
 * receives the same window value. For rows still inside the spotlight
 * radius, IrisSpotlight_CalculateCircleValue returns a packed
 * left/right column pair derived from the precomputed quarter-circle
 * helper table. Rows outside the radius (and rows at extreme Y) get
 * 0xFF, meaning "no window — fully blanked."
 *
 * The dynamic HDMA buffer is mirrored into hdma_table_unused so the
 * second-page HDMA channel sees the same data. After populating the
 * table the radius is stepped by kSpotlight_delta_size; once it
 * reaches the matching kSpotlight_goal value the spotlight phase ends
 * and either screen-blank is asserted (close) or the table is flooded
 * to "fully open" via IrisSpotlight_ResetTable. The submodule indices
 * are reset and (when applicable) the post-cutscene music ambient is
 * picked up so the world resumes its normal soundtrack on exit.
 */
void IrisSpotlight_ConfigureTable() {  // 80f312
  uint16 r14 = link_y_coord - BG2VOFS_copy2 + 12;
  spotlight_y_lower = r14 - spotlight_var1;
  spotlight_y_upper = r14 + spotlight_var1;
  spotlight_var3 = link_x_coord - BG2HOFS_copy2 + 8;
  spotlight_var4 = spotlight_var1;
  uint16 r6 = r14 * 2;
  if (r6 < 224)
    r6 = 224;
  uint16 r4 = r14 * 2 - r6;
  for(;;) {
    uint16 r8 = 0xff;
    if (r6 < spotlight_y_upper) {
      uint8 t = spotlight_var4;
      if (spotlight_var4)
        spotlight_var4--;
      r8 = IrisSpotlight_CalculateCircleValue(t);
    }
    if (r4 < 240)
      hdma_table_dynamic[r4] = r8;
    if (r6 < 240)
      hdma_table_dynamic[r6] = r8;
    if (r4 == r14)
      break;
    r4++, r6--;
  }

  for (int i = 224; i < 240; i++)
    hdma_table_dynamic[i] = 0;

  memcpy(hdma_table_unused, hdma_table_dynamic, 224  * sizeof(uint16));

  spotlight_var1 += kSpotlight_delta_size[spotlight_var2 >> 1];

  if (spotlight_var1 != kSpotlight_goal[spotlight_var2 >> 1])
    return;

  if (!spotlight_var2) {
    INIDISP_copy = 0x80;
  } else {
    IrisSpotlight_ResetTable();
  }
  subsubmodule_index = 0;
  submodule_index = 0;

  if (main_module_index == 7 || main_module_index == 16) {
    if (!player_is_indoors)
      sound_effect_ambient = overworld_music[BYTE(overworld_screen_index)] >> 4;
    if (queued_music_control != 0xff)
      music_control = queued_music_control;
  }
  main_module_index = saved_module_for_menu;
  if (main_module_index == 6)
    Sprite_ResetAll();
}

/*
 * Floods the dynamic HDMA spotlight table with the "fully open" value
 * 0xFF00 (window covers the whole horizontal range). Used when an open
 * animation completes so the next frame draws the entire screen.
 */
void IrisSpotlight_ResetTable() {  // 80f427
  for (int i = 0; i < 240; i++)
    hdma_table_dynamic[i] = 0xff00;
}

/*
 * Computes one (left, right) column pair for an iris spotlight scanline.
 *   a — distance from the spotlight's vertical center, in pixels.
 * Internally divides a*256 by the current spotlight radius to get a
 * normalized 0..255 angle, then samples kConfigureSpotlightTable_Helper_Tab
 * for the corresponding cosine value. Multiplying that back by the
 * radius produces the half-width of the open window at this row, which
 * is added/subtracted from spotlight_var3 (Link's screen-X) to get the
 * left and right column edges. Returns 0xFF when the resulting window
 * collapses past the edge of the screen.
 */
uint16 IrisSpotlight_CalculateCircleValue(uint8 a) {  // 80f4cc
  uint8 t = snes_divide(a << 8, spotlight_var1) >> 1;
  uint8 r10 = kConfigureSpotlightTable_Helper_Tab[t];
  uint16 p = 2 * (uint8)(r10 * (uint8)spotlight_var1 >> 8);
  if (!r10)
    return 0xff;
  uint16 r2 = spotlight_var3 + p;
  uint16 r0 = spotlight_var3 - p;
  r0 = ((int16)r0 < 0) ? 0 :
         r0 < 255 ? r0 : 255;
  r2 = r2 < 255 ? r2 : 255;
  r0 |= r2 << 8;
  return r0 == 0xffff ? 0xff : r0;
}

/*
 * Each-frame entry point for the water-window HDMA effect (used by the
 * waterfall-of-wishing fairy and similar set-pieces). Converts the
 * world-space center y/x coordinates to screen space and forwards to
 * the inner table-builder.
 */
void AdjustWaterHDMAWindow() {  // 80f649
  uint16 r10 = water_hdma_var1 - BG2VOFS_copy2;
  spotlight_y_lower = r10 - water_hdma_var2;
  spotlight_y_upper = r10 + water_hdma_var2;
  AdjustWaterHDMAWindow_X(r10);
}

/*
 * Builds the per-scanline horizontal window for the water HDMA effect.
 * Computes a constant left/right column pair based on the requested
 * water radius and sweeps it outward from the center row in a manner
 * similar to IrisSpotlight_ConfigureTable, but with two key
 * differences: the window is rectangular (no per-row half-width
 * recomputation), and rows above/below the active band are clamped to
 * 0xFF (no window). word_7E0678 is decremented when the row pointer
 * crosses the bottom band so the effect can shrink one line per frame.
 */
void AdjustWaterHDMAWindow_X(uint16 r10) {  // 80f660
  spotlight_var3 = water_hdma_var0 - BG2HOFS_copy2;
  uint16 r12 = water_hdma_var3 ? water_hdma_var3 - 1 : 0;
  uint16 r2 = spotlight_var3 + r12;
  uint16 r0 = spotlight_var3 - r12;

  r0 = (r0 < 255) ? r0 : 255;
  r2 = (r2 < 255) ? r2 : 255;
  r12 = r0 | r2 << 8;

  uint16 r6 = r10 * 2;
  if (r6 < 0xe0)
    r6 = 0xe0;
  uint16 r4 = 2 * r10 - r6;
  uint16 a;

  do {
    if (!sign16(r4)) {
      if (!sign16(spotlight_y_lower) && r4 < spotlight_y_lower)
        a = 0xff;
      else
        a = r12;
      if (r4 < 240)
        hdma_table_dynamic[r4] = (a != 0xffff) ? a : 0xff;
    }
    if (r6 >= spotlight_y_upper) {
      a = 0xff;
    } else {
      if (r6 >= 225 && word_7E0678)
        word_7E0678--;
      a = r12;
    }
    if (r6 < 240)
      hdma_table_dynamic[r6] = (a != 0xffff) ? a : 0xff;
  } while (r6--, r10 != r4++);
}

/*
 * Builds the HDMA window table for the flood-of-Hyrule cutscene at the
 * dam. Two passes:
 *   - The first pass writes 0xFF00 (open window) for every row above
 *     the current water line.
 *   - The second pass walks rows from spotlight_y_upper down to 224,
 *     producing a "rippling" pattern by selectively writing the
 *     left/right column pair into rows that pass an interlace test.
 *     Rows beyond the lower band are clamped to 0xFF.
 * The result is the rising/falling water mask used by the cutscene.
 */
void FloodDam_PrepFloodHDMA() {  // 80f734
  spotlight_y_lower = water_hdma_var1 - BG2VOFS_copy2;
  spotlight_var3 = water_hdma_var0 - BG2HOFS_copy2;
  uint16 r14 = water_hdma_var3 ^ 1;
  uint16 r12 = (spotlight_var3 + r14) << 8 | (uint8)(spotlight_var3 - r14);

  int r4 = 0;
  do {
    hdma_table_dynamic[r4] = 0xff00;
  } while (++r4 != spotlight_y_upper);

  r12 = r14 - 7 + 8;
  r12 = (spotlight_var3 + r12) << 8 | (uint8)(spotlight_var3 - r12);
  uint16 r10 = (spotlight_y_upper + water_hdma_var2) ^ 1;

  do {
    if (r4 >= r10) {
      hdma_table_dynamic[r4] = 0xff;
    } else {
      uint16 a = r4;
      do {
        a *= 2;
      } while (a >= 480);
      hdma_table_dynamic[a >> 1] = r12 == 0xffff ? 0xff : r12;
    }
  } while (++r4 < 225);
}

/*
 * Resets the dungeon star-tile (alternating raised/lowered floor)
 * state and reuploads the matching CHR data. Called when entering a
 * room with star tiles to ensure the visual state matches the cleared
 * "neither side raised" position.
 */
void ResetStarTileGraphics() {  // 80fda4
  byte_7E04BC = 0;
  Dungeon_RestoreStarTileChr();
}

/*
 * Copies the appropriate star-tile tile data from WRAM 0xBDC0 into the
 * messaging buffer for the next NMI to upload. The byte_7E04BC flag
 * picks which 32-byte half (raised vs. lowered orientation) feeds the
 * top vs. the bottom of the messaging buffer; the NMI handler writes
 * the staged bytes to VRAM via index 0x18.
 */
void Dungeon_RestoreStarTileChr() {  // 80fda7
  int xx = 0, yy = 32;
  if (byte_7E04BC)
    xx = 32, yy = 0;
  uint16 *p = messaging_buf;
  memcpy(p, g_ram + 0xbdc0 + xx, 32);
  memcpy(p + 16, g_ram + 0xbdc0 + yy, 32);
  nmi_subroutine_index = 0x18;
}

/*
 * Per-frame mosaic ramp for the Link-zapped electrocution effect.
 * Walks the mosaic level up in steps of 0x10 until it reaches 0xC0,
 * then ramps it back down to zero, producing a back-and-forth pulse.
 * mosaic_inc_or_dec tracks the current direction. The hardware mosaic
 * register receives level >> 1 and the BG mode is locked to 9 to
 * enable mosaic on every BG layer.
 */
void LinkZap_HandleMosaic() {  // 81fed2
  int level = mosaic_level;
  if (!mosaic_inc_or_dec) {
    level += 0x10;
    if (level == 0xc0)
      mosaic_inc_or_dec = 1;
  } else {
    level -= 0x10;
    if (level == 0)
      mosaic_inc_or_dec = 0;
  }
  mosaic_level = level;
  MOSAIC_copy = mosaic_level >> 1 | 3;
  BGMODE_copy = 9;
}

/*
 * Forces the mosaic register to a fixed level supplied by `a`. Used by
 * various player-state effects (transformations, save-and-quit fade,
 * etc.) to skip the gradual ramp and snap directly to a target level.
 * Direction is reset to "increasing" so any subsequent ramp behaves
 * predictably.
 */
void Player_SetCustomMosaicLevel(uint8 a) {  // 81fef0
  mosaic_inc_or_dec = 0;
  mosaic_level = a;
  MOSAIC_copy = mosaic_level >> 1 | 3;
  BGMODE_copy = 9;
}

/*
 * First half of the orange/blue peg toggle animation in module 07.16.
 * Stages the alternate-state peg tile data into the messaging buffer
 * by swapping which 0x80-byte tile region maps to "raised" and which
 * maps to "lowered" depending on orange_blue_barrier_state.
 */
void Module07_16_UpdatePegs_Step1() {  // 829739
  if (BYTE(orange_blue_barrier_state))
    Dungeon_UpdatePegGFXBuffer(0x80, 0x100);
  else
    Dungeon_UpdatePegGFXBuffer(0x100, 0x80);
}

/*
 * Second half of the peg toggle. Repeats the staging with the offsets
 * inverted so the next NMI uploads the second tile pair, completing
 * the visual flip from "blue down / orange up" to its mirror.
 */
void Module07_16_UpdatePegs_Step2() {  // 82974d
  if (BYTE(orange_blue_barrier_state))
    Dungeon_UpdatePegGFXBuffer(0x100, 0x80);
  else
    Dungeon_UpdatePegGFXBuffer(0x80, 0x100);
}

/*
 * Copies two 64-uint16 peg-tile blocks from the cache at WRAM 0xB340
 * into the messaging staging buffer at offsets 0 and 64. The x/y
 * arguments are byte offsets into the cache (callers select which
 * orientation pair to write). NMI subroutine 23 will pick the buffer
 * up next vblank and DMA it into VRAM.
 */
void Dungeon_UpdatePegGFXBuffer(int x, int y) {  // 829773
  uint16 *src = (uint16 *)&g_ram[0xb340];
  for (int i = 0; i < 64; i++)
    messaging_buf[i] = src[(x >> 1) + i];
  for (int i = 0; i < 64; i++)
    messaging_buf[64 + i] = src[(y >> 1) + i];
  nmi_subroutine_index = 23;
}

/*
 * Configures color-math, BG translucency and torch lighting for a newly
 * entered dungeon room. Steps:
 *   1. If a translucency swap is in effect from a previous room, undo it.
 *   2. Set CGWSEL/CGADSUB to "subtract on subscreen" so dim rooms get
 *      the SNES color-math darkening pass.
 *   3. Resolve the lit-torch count: if the room is not a "lights out"
 *      room, force the count to 3 (fully lit) and pick CGADSUB based on
 *      dung_hdr_bg2_properties (different BG2 modes need different
 *      color-math intensities). Mode 2 specifically calls
 *      Palette_AssertTranslucencySwap and seeds Agahnim's palette state
 *      machine when the room is dungeon room 13 (Agahnim's chamber).
 *   4. Pick the matching backdrop tint from kLitTorchesColorPlus and
 *      kick off all the per-area palette loaders so the room renders
 *      with the right colors. The submodule index is bumped so the
 *      room-init pipeline moves to the next phase next frame.
 */
void Dungeon_HandleTranslucencyAndPalette() {  // 82a1e9
  if (palette_swap_flag)
    Palette_RevertTranslucencySwap();

  CGWSEL_copy = 2;
  CGADSUB_copy = 0xb3;

  uint8 torch = dung_num_lit_torches;
  if (!dung_want_lights_out) {
    uint8 a = 0x20;
    if ((a = 0x20, dung_hdr_bg2_properties != 0) &&
        (a = 0x32, dung_hdr_bg2_properties != 7) &&
        (a = 0x62, dung_hdr_bg2_properties != 4) &&
        (a = 0x20, dung_hdr_bg2_properties == 2)) {
      Palette_AssertTranslucencySwap();
      if (BYTE(dungeon_room_index) == 13) {
        agahnim_pal_setting[0] = 0;
        agahnim_pal_setting[1] = 0;
        agahnim_pal_setting[2] = 0;
        agahnim_pal_setting[3] = 0;
        agahnim_pal_setting[4] = 0;
        agahnim_pal_setting[5] = 0;
        Palette_LoadAgahnim();
      }
      a = 0x70;
    }
    CGADSUB_copy = a;
    torch = 3;
  }
  overworld_fixed_color_plusminus = kLitTorchesColorPlus[torch];
  palette_filter_countdown = 31;
  mosaic_target_level = 0;
  darkening_or_lightening_screen = 2;
  overworld_palette_aux_or_main = 0;
  Palette_Load_DungeonSet();
  Palette_Load_Sp0L();
  Palette_Load_Sp5L();
  Palette_Load_Sp6L();
  subsubmodule_index += 1;
}

/*
 * Wipes the working palette buffers and loads the full overworld
 * palette set from scratch. Used when entering the overworld for the
 * first time (after a load, after a dungeon exit, etc.). Resets every
 * palette mode/aux selector to its default and chains through all the
 * Palette_Load_* helpers in dependency order. The trailing memcpy
 * mirrors the Sword/Shield gear pad (0x1B0..0x1BF) into the active
 * buffer because that range is normally driven by gear-state code.
 */
void Overworld_LoadAllPalettes() {  // 82c5b2
  memset(aux_palette_buffer + 0x180 / 2, 0, 128);
  memset(main_palette_buffer, 0, 512);

  overworld_palette_mode = 5;
  overworld_palette_aux1_bp2to4_hi = 3;
  overworld_palette_aux2_bp5to7_hi = 3;
  overworld_palette_aux3_bp7_lo = 0;
  palette_sp6r_indoors = 5;
  palette_sp0l = 11;
  palette_swap_flag = 0;
  overworld_palette_aux_or_main = 0;
  Palette_BgAndFixedColor_Black();
  Palette_Load_Sp0L();
  Palette_Load_SpriteMain();
  Palette_Load_OWBGMain();
  Palette_Load_OWBG1();
  Palette_Load_OWBG2();
  Palette_Load_OWBG3();
  Palette_Load_SpriteEnvironment_Dungeon();
  Palette_Load_HUD();

  for (int i = 0; i < 8; i++)
    main_palette_buffer[0x1b0 / 2 + i] = aux_palette_buffer[0x1d0 / 2 + i];
}

/*
 * Loads the standard dungeon palette set: backdrop, sprite slots,
 * sword/shield gear, sprite-environment palette, Link's armor/gloves,
 * HUD, dungeon BG, and finally the overworld palette inner-pass that
 * caches the resolved values for fade work.
 */
void Dungeon_LoadPalettes() {  // 82c630
  overworld_palette_aux_or_main = 0;
  Palette_BgAndFixedColor_Black();
  Palette_Load_Sp0L();
  Palette_Load_SpriteMain();
  Palette_Load_Sp5L();
  Palette_Load_Sp6L();
  Palette_Load_Sword();
  Palette_Load_Shield();
  Palette_Load_SpriteEnvironment();
  Palette_Load_LinkArmorAndGloves();
  Palette_Load_HUD();
  Palette_Load_DungeonSet();
  Overworld_LoadPalettesInner();
}

/*
 * Caches the active palette state into the *_unk1..3 fade-work fields,
 * resets the dither-fade counter and direction to "ready to fade in,"
 * and snapshots the current aux palette into main via
 * Overworld_CopyPalettesToCache. Called as the last step of Dungeon_
 * LoadPalettes and Overworld_LoadAllPalettes.
 */
void Overworld_LoadPalettesInner() {  // 82c65f
  overworld_pal_unk1 = palette_main_indoors;
  overworld_pal_unk2 = overworld_palette_aux3_bp7_lo;
  overworld_pal_unk3 = byte_7E0AB7;
  darkening_or_lightening_screen = 2;
  palette_filter_countdown = 0;
  WORD(mosaic_target_level) = 0;
  Overworld_CopyPalettesToCache();
}

/*
 * Picks the right overworld_palette_mode for the current overworld
 * screen. Screens 3/5/7 (the Death Mountain area variants) use mode 2
 * as their base; all other screens use mode 0. Adding 1 selects the
 * dark-world variant when overworld_screen_index has bit 6 set.
 */
void OverworldLoadScreensPaletteSet() {  // 82c692
  uint8 sc = overworld_screen_index & 0x3f;
  uint8 x = (sc == 3 || sc == 5 || sc == 7) ? 2 : 0;
  x += (overworld_screen_index & 0x40) ? 1 : 0;
  Overworld_LoadAreaPalettesEx(x);
}

/*
 * Reloads every per-area palette slot for overworld_palette_mode = `x`.
 * Touches sprite-main, sprite-environment, sp5l, sp6l, sword, shield
 * and Link's armor/gloves, then picks the sp0l index based on whether
 * Link is currently in a Dark World save (palette 3 vs 1) and finishes
 * with HUD + OW BG main. The high byte of overworld_palette_aux_or_main
 * is masked off so subsequent loads target the active buffer page.
 */
void Overworld_LoadAreaPalettesEx(uint8 x) {  // 82c6ad
  overworld_palette_mode = x;
  overworld_palette_aux_or_main &= 0xff;
  Palette_Load_SpriteMain();
  Palette_Load_SpriteEnvironment();
  Palette_Load_Sp5L();
  Palette_Load_Sp6L();
  Palette_Load_Sword();
  Palette_Load_Shield();
  Palette_Load_LinkArmorAndGloves();
  palette_sp0l = (savegame_is_darkworld & 0x40) ? 3 : 1;
  Palette_Load_Sp0L();
  Palette_Load_HUD();
  Palette_Load_OWBGMain();
}

/*
 * Snapshot routine for "special overworld" screens (intro, ending,
 * cutscene maps). Clears most of main_palette_buffer to black, then
 * copies just the specific row strips that those screens actually
 * touch (rows 0..3 for the BG art and rows 0xD8..0xFF for the sprite
 * art) from aux_palette_buffer. Locks the mosaic register at 0xF7
 * (full 16x16 mosaic) and flags CGRAM dirty so the next NMI ships the
 * blacked-out frame.
 */
void SpecialOverworld_CopyPalettesToCache() {  // 82c6eb
  for (int i = 32; i < 32 * 8; i++)
    main_palette_buffer[i] = 0;
  for (int i = 0; i < 8; i++) {
    main_palette_buffer[i] = aux_palette_buffer[i];
    main_palette_buffer[i + 0x8] = aux_palette_buffer[i + 0x8];
    main_palette_buffer[i + 0x10] = aux_palette_buffer[i + 0x10];
    main_palette_buffer[i + 0x18] = aux_palette_buffer[i + 0x18];
    main_palette_buffer[i + 0xd8] = aux_palette_buffer[i + 0xd8];
    main_palette_buffer[i + 0xe8] = aux_palette_buffer[i + 0xe8];
    main_palette_buffer[i + 0xf0] = aux_palette_buffer[i + 0xf0];
    main_palette_buffer[i + 0xf8] = aux_palette_buffer[i + 0xf8];
  }
  MOSAIC_copy = 0xf7;
  mosaic_level = 0xf7;
  flag_update_cgram_in_nmi++;
}

/*
 * One-shot snapshot of the entire 256-entry aux palette into main, used
 * by ordinary overworld transitions to commit a fade target to the
 * displayed buffer. Flags CGRAM dirty so NMI uploads next frame.
 */
void Overworld_CopyPalettesToCache() {  // 82c769
  memcpy(main_palette_buffer, aux_palette_buffer, 512);
  flag_update_cgram_in_nmi += 1;
}

/*
 * Resolves overworld BG/sprite palette slot indices for the given area
 * descriptors and (re)loads the affected palette ranges.
 *   bg  — index into kOwBgPalInfo (per-area BG palette triple)
 *   spr — index into kOwSprPalInfo (per-area sprite palette pair)
 * Negative entries in the descriptor tables mean "leave the existing
 * choice for that slot in place", so the caller can change a single
 * channel without disturbing the others. After updating the slot
 * variables, the BG1/2/3 and Sp5L/Sp6L palettes are reloaded.
 */
void Overworld_LoadPalettes(uint8 bg, uint8 spr) {  // 8ed5a8
  overworld_palette_aux_or_main = 0;

  const int8 *d = kOwBgPalInfo + bg * 3;
  if (d[0] >= 0)
    overworld_palette_aux1_bp2to4_hi = d[0];
  if (d[1] >= 0)
    overworld_palette_aux2_bp5to7_hi = d[1];
  if (d[2] >= 0)
    overworld_palette_aux3_bp7_lo = d[2];

  d = kOwSprPalInfo + spr * 2;
  if (d[0] >= 0)
    palette_sp5l = d[0];
  if (d[1] >= 0)
    palette_sp6l = d[1];
  Palette_Load_OWBG1();
  Palette_Load_OWBG2();
  Palette_Load_OWBG3();
  Palette_Load_Sp5L();
  Palette_Load_Sp6L();
}

/* Convenience wrapper that paints the BG backdrop entry and fixed color
 * black. Used at the start of every full palette reload. */
void Palette_BgAndFixedColor_Black() {  // 8ed5f4
  Palette_SetBgAndFixedColor(0);
}

/*
 * Writes `color` into both the displayed and target backdrop entries
 * (entry 0 and entry 32 in main + aux buffers — 32 is the duplicate
 * "real" backdrop used while sub-screen mosaic is active) and forces
 * the COLDATA shadow registers to pure black.
 */
void Palette_SetBgAndFixedColor(uint16 color) {  // 8ed5f9
  main_palette_buffer[0] = color;
  main_palette_buffer[32] = color;
  aux_palette_buffer[0] = color;
  aux_palette_buffer[32] = color;
  SetBackdropcolorBlack();
}

/*
 * Pokes the three COLDATA shadow registers (red, green, blue write
 * masks) so the SNES color-math fixed-color register is set to black.
 * 0x20/0x40/0x80 select the R/G/B target bits with intensity zero.
 */
void SetBackdropcolorBlack() {  // 8ed60b
  COLDATA_copy0 = 0x20;
  COLDATA_copy1 = 0x40;
  COLDATA_copy2 = 0x80;
}

/* Convenience: paints the backdrop using whatever color
 * Palette_GetOwBgColor returns for the current overworld state. */
void Palette_SetOwBgColor() {  // 8ed618
  Palette_SetBgAndFixedColor(Palette_GetOwBgColor());
}

/*
 * Special-overworld variant: stamps the picked OW color into the AUX
 * backdrop entries only (so it becomes the next fade target without
 * disturbing the displayed backdrop) and resets COLDATA to black.
 */
void Palette_SpecialOw() {  // 8ed61d
  uint16 c = Palette_GetOwBgColor();
  aux_palette_buffer[0] = c;
  aux_palette_buffer[32] = c;
  SetBackdropcolorBlack();
}

/*
 * Picks the right backdrop color for the current location.
 *   - Light World overworld → 0x2669 (warm dim brown)
 *   - Dark World overworld  → 0x2A32 (dim purple/red)
 *   - Specific Ganon's Tower / pyramid rooms → 0x19C6 (dark stone)
 *   - Anything else → falls through to the warm brown default
 * Used as the backdrop for fade transitions where the screen would
 * otherwise expose unrendered VRAM.
 */
uint16 Palette_GetOwBgColor() {  // 8ed622
  if (overworld_screen_index < 0x80)
    return overworld_screen_index & 0x40 ? 0x2A32 : 0x2669;
  if (dungeon_room_index == 0x180 || dungeon_room_index == 0x182 || dungeon_room_index == 0x183)
    return 0x19C6;
  return 0x2669;
}

/* Convenience: enables translucency-swap on the active palette. */
void Palette_AssertTranslucencySwap() {  // 8ed657
  Palette_SetTranslucencySwap(true);
}

/*
 * Toggles the dungeon translucency palette swap.
 * Several dungeon rooms use sprite palette rows 0xF0/0xF8 to hold the
 * "translucent" colors and rows 0x80/0x88 to hold the opaque versions.
 * When the swap is asserted these rows are exchanged in BOTH the main
 * and aux buffers (so the displayed colors and the fade targets stay
 * in sync), letting BG2 render with translucent palettes; reverting
 * the swap restores the originals. The 0xB8/0xD8 row pair is also
 * exchanged for the secondary translucency layer used by Agahnim's
 * room. palette_swap_flag tracks the current state so paired
 * Assert/Revert calls don't drift.
 */
void Palette_SetTranslucencySwap(bool v) {  // 8ed65c
  palette_swap_flag = v;
  uint16 a, b;
  for (int i = 0; i < 8; i++) {
    a = aux_palette_buffer[i + 0x80];
    b = aux_palette_buffer[i + 0xf0];
    main_palette_buffer[i + 0xf0] = aux_palette_buffer[i + 0xf0] = a;
    main_palette_buffer[i + 0x80] = aux_palette_buffer[i + 0x80] = b;

    a = aux_palette_buffer[i + 0x88];
    b = aux_palette_buffer[i + 0xf8];
    main_palette_buffer[i + 0xf8] = aux_palette_buffer[i + 0xf8] = a;
    main_palette_buffer[i + 0x88] = aux_palette_buffer[i + 0x88] = b;

    a = aux_palette_buffer[i + 0xb8];
    b = aux_palette_buffer[i + 0xd8];
    main_palette_buffer[i + 0xd8] = aux_palette_buffer[i + 0xd8] = a;
    main_palette_buffer[i + 0xb8] = aux_palette_buffer[i + 0xb8] = b;
  }
  flag_update_cgram_in_nmi++;
}

/* Convenience: undoes a prior translucency swap, restoring the
 * opaque dungeon palette rows. */
void Palette_RevertTranslucencySwap() {  // 8ed6bb
  Palette_SetTranslucencySwap(false);
}

/*
 * Loads the gear palettes that match Link's CURRENT inventory state
 * (sword type, shield type, armor). Bug-fix mode also refreshes the
 * gloves color so the Power Glove / Titan's Mitt tint is correct after
 * any sword/shield swap; vanilla skipped this and would briefly show
 * the wrong glove tint after upgrading.
 */
void LoadActualGearPalettes() {  // 8ed6c0
  LoadGearPalettes(link_sword_type, link_shield_type, link_armor);
  if (enhanced_features0 & kFeatures0_MiscBugFixes)
    Palette_UpdateGlovesColor();
}

/* Forces a stylized "electric" palette for the lightning-strike
 * sequence — sword 2, shield 2, armor 4 (the cyan/white scheme). */
void Palette_ElectroThemedGear() {  // 8ed6d1
  LoadGearPalettes(2, 2, 4);
}

/* Bunny-Link variant: keeps the actual sword/shield colors but uses
 * armor index 3 (the pink bunny tint). */
void LoadGearPalettes_bunny() {  // 8ed6dd
  LoadGearPalettes(link_sword_type, link_shield_type, 3);
}

/*
 * Master gear-palette loader. For each of the three gear groups
 * (sword/shield/armor) it indexes the relevant palette table by the
 * supplied type number and copies the corresponding row into both the
 * aux and main palette buffers via Palette_LoadMultiple_Arbitrary.
 *   sword  — link_sword_type (0..4); when zero or 0xFF, use index 0
 *   shield — link_shield_type (0..3); when zero, use index 0
 *   armor  — link_armor (0..N); used directly
 * The trailing CGRAM dirty flag bump ensures the new gear is uploaded
 * on the next NMI.
 */
void LoadGearPalettes(uint8 sword, uint8 shield, uint8 armor) {  // 8ed6e8
  const uint16 *src = kPalette_Sword + (sword && sword != 255 ? sword - 1 : 0) * 3;
  Palette_LoadMultiple_Arbitrary(src, 0x1b2, 2);

  src = kPalette_Shield + (shield ? shield - 1 : 0) * 4;
  Palette_LoadMultiple_Arbitrary(src, 0x1b8, 3);

  src = kPalette_ArmorAndGloves + armor * 15;
  Palette_LoadMultiple_Arbitrary(src, 0x1e2, 14);
  flag_update_cgram_in_nmi++;
}

/*
 * Helper used by LoadGearPalettes and friends. Copies n uint16 colors
 * from `src` into both palette buffers at byte offset `dst` (so the
 * displayed and target colors stay locked together until something
 * else fades them apart).
 */
void LoadGearPalette(int dst, const uint16 *src, int n) {  // 8ed741
  memcpy(&aux_palette_buffer[dst >> 1], src, sizeof(uint16) * n);
  memcpy(&main_palette_buffer[dst >> 1], src, sizeof(uint16) * n);
}

/*
 * "Whiten the BG" effect used for screen flashes (lightning, item
 * pickup glints). Walks the BG palette range 0x20..0x7F and pushes
 * each entry through Filter_Majorly_Whiten_Color. Entry 0 is force-
 * synced from entry 32 unless the original was already black, so
 * dim/empty rooms don't suddenly grow a backdrop.
 */
void Filter_Majorly_Whiten_Bg() {  // 8ed757
  for (int i = 32; i < 128; i++)
    main_palette_buffer[i] = Filter_Majorly_Whiten_Color(aux_palette_buffer[i]);
  main_palette_buffer[0] = aux_palette_buffer[0] ? main_palette_buffer[32] : 0;
}

/*
 * Brightens a single 15-bit BGR color toward white. The amount applied
 * to each channel is 14 (vanilla) or 3 (when the DimFlashes feature is
 * enabled, for photosensitive players). Each channel is clamped to its
 * 5-bit max so the result remains a valid SNES color.
 */
uint16 Filter_Majorly_Whiten_Color(uint16 c) {  // 8ed7fe
  int amt = (enhanced_features0 & kFeatures0_DimFlashes) ? 3 : 14;
  int r = (c & 0x1f) + amt;
  int g = (c & 0x3e0) + (amt << 5);
  int b = (c & 0x7c00) + (amt << 10);
  if (r > 0x1f) r = 0x1f;
  if (g > 0x3e0) g = 0x3e0;
  if (b > 0x7c00) b = 0x7c00;
  return r | g | b;
}

/*
 * Reverses Filter_Majorly_Whiten_Bg by copying the original BG palette
 * range 0x20..0x7F back from aux_palette_buffer. The backdrop entry is
 * resynced from entry 32 and Palette_Restore_Coldata reinstates the
 * fixed-color subtractive math values appropriate for the current area.
 */
void Palette_Restore_BG_From_Flash() {  // 8ed83a
  for (int i = 32; i < 128; i++)
    main_palette_buffer[i] = aux_palette_buffer[i];
  main_palette_buffer[0] = main_palette_buffer[32];
  Palette_Restore_Coldata();
}

/*
 * Restores the COLDATA fixed-color shadow registers for the current
 * outdoor area. Indoor areas are skipped (their COLDATA is owned by
 * the dungeon translucency code). For outdoor scenes a small switch
 * picks one of four packed RGB values:
 *   - Death Mountain top (3/5/7) → warm dawn 0x8C4C26
 *   - Death Mountain dark variants (0x43/0x45/0x47) → cooler 0x874A26
 *   - Skull Woods (0x5B) → 0x894F33
 *   - everything else → the default 0x804020
 * The packed RGB is then unpacked into the three COLDATA copies.
 */
void Palette_Restore_Coldata() {  // 8ed8ae
  if (!player_is_indoors) {
    uint32 rgb;
    switch (BYTE(overworld_screen_index)) {
    case 3: case 5: case 7:
      rgb = 0x8c4c26;
      break;
    case 0x43: case 0x45: case 0x47:
      rgb = 0x874a26;
      break;
    case 0x5b:
      rgb = 0x894f33;
      break;
    default:
      rgb = 0x804020;
    }
    COLDATA_copy0 = (uint8)(rgb);
    COLDATA_copy1 = (uint8)(rgb >> 8);
    COLDATA_copy2 = (uint8)(rgb >> 16);
  }
}

/*
 * Bulk restore for the entire BG + HUD half of the palette (256 bytes,
 * entries 0..0x7F) plus the COLDATA fixed colors. Used at the end of a
 * full screen flash to drop the brightened state in one shot.
 */
void Palette_Restore_BG_And_HUD() {  // 8ed8fb
  memcpy(main_palette_buffer, aux_palette_buffer, 256);
  flag_update_cgram_in_nmi++;
  Palette_Restore_Coldata();
}

/* Summary of sprite palette usage 
0l: kPalette_SpriteAux3[palette_sp0l]
0r: kPalette_MiscSprite[7 / 9] or kPalette_DungBgMain[(palette_main_indoors >> 1) * 90]
1 : common sprites
2 :      -"-
3 :      -"-
4 :      -"-
5l: palette_sp5l
5r: link sword/shield
6l: palette_sp6l
6r: kPalette_MiscSprite[6 / 8] or kPalette_MiscSprite[palette_sp6r_indoors]
7 : link armor and gloves
*/

/*
 * CGRAM byte offsets of the named sprite-palette regions. Each value is
 * an offset (in bytes) into main_palette_buffer / aux_palette_buffer
 * pointing at the first color of the named palette row. The Palette_
 * Load_* helpers add overworld_palette_aux_or_main and shift right by
 * one to convert these to uint16 indices.
 */
enum {
  kPal_sp0l = 0x102,
  kPal_sp0r = 0x112,
  kPal_sp1to4 = 0x122,   // This is used for 64 colors, colors switched if in darkworld mode
  kPal_sp5l = 0x1a2,
  kPal_Sword = 0x1b2,
  kPal_Shield = 0x1b8,
  kPal_sp6l = 0x1c2,
  kPal_sp6r = 0x1d2,
  kPal_sp7l = 0x1e2,
  kPal_sp7r = 0x1f2,
  kPal_ArmorGloves = 0x1e2,
  kPal_PalaceMap = 0x182,
};

/*
 * Loads the sp0l (sprite slot 0 left half) palette row from
 * kPalette_SpriteAux3, picking the row by palette_sp0l. When the
 * dungeon translucency swap is in effect the destination is sp7l
 * instead so the swapped translucent rows stay coherent.
 */
void Palette_Load_Sp0L() {  // 9bec77
  const uint16 *src = kPalette_SpriteAux3 + palette_sp0l * 7;
  Palette_LoadSingle(src, palette_swap_flag ? kPal_sp7l : kPal_sp0l, 6);
}

/*
 * Loads the four "main sprite" palette rows shared by every sprite in
 * a scene. The source pointer is offset by 60 colors (0x3C) when the
 * dark-world bit (0x40) of overworld_screen_index is set, swapping the
 * sprite scheme to the dark-world variant in one shot.
 */
void Palette_Load_SpriteMain() {  // 9bec9e
  const uint16 *src = kPalette_MainSpr + (overworld_screen_index & 0x40 ? 60 : 0);
  Palette_LoadMultiple(src, kPal_sp1to4, 14, 3);
}

/* Loads the sp5l auxiliary sprite palette row from kPalette_SpriteAux1
 * indexed by palette_sp5l. */
void Palette_Load_Sp5L() {  // 9becc5
  const uint16 *src = kPalette_SpriteAux1 + (palette_sp5l) * 7;
  Palette_LoadSingle(src, kPal_sp5l, 6);
}

/* Sibling of Palette_Load_Sp5L for the sp6l sprite palette row. */
void Palette_Load_Sp6L() {  // 9bece4
  const uint16 *src = kPalette_SpriteAux1 + (palette_sp6l) * 7;
  Palette_LoadSingle(src, kPal_sp6l, 6);
}

/*
 * Loads Link's currently equipped sword palette into the sp5r region.
 * The (int8) cast on link_sword_type ensures negative-sentinel values
 * (the original ROM occasionally reads offset 0xFF) collapse to zero
 * instead of indexing past the table — the inline comment "wtf" is
 * preserved from the original author.
 */
void Palette_Load_Sword() {  // 9bed03
  const uint16 *src = kPalette_Sword + ((int8)link_sword_type > 0 ? link_sword_type - 1 : 0) * 3;  // wtf: zelda reads offset 0xff
  Palette_LoadMultiple_Arbitrary(src, kPal_Sword, 2);
  flag_update_cgram_in_nmi += 1;
}

/*
 * Loads Link's currently equipped shield palette into the sp5r region.
 * Same one-based offset trick as Palette_Load_Sword: shield 0 means
 * "no shield" but still indexes the first row to avoid out-of-bounds.
 */
void Palette_Load_Shield() {  // 9bed29
  const uint16 *src = kPalette_Shield + (link_shield_type ? link_shield_type - 1 : 0) * 4;
  Palette_LoadMultiple_Arbitrary(src, kPal_Shield, 3);
  flag_update_cgram_in_nmi += 1;
}

/*
 * Routes the misc-sprite palette load to either the dungeon or
 * overworld variant based on whether Link is currently indoors. The
 * dungeon path uses palette_sp6r_indoors as its index; the overworld
 * path picks between two row offsets depending on light/dark world.
 */
void Palette_Load_SpriteEnvironment() {  // 9bed6e
  if (player_is_indoors)
    Palette_Load_SpriteEnvironment_Dungeon();
  else
    Palette_MiscSprite_Outdoors();
}

// avoid renaming in assets.dat
#define kPalette_MiscSprite kPalette_MiscSprite_Indoors

/*
 * Indoor variant: loads the misc-sprite (NPC, decoration, switch) row
 * for the current dungeon by indexing kPalette_MiscSprite with
 * palette_sp6r_indoors and writing it to the sp6r region.
 */
void Palette_Load_SpriteEnvironment_Dungeon() {  // 9bed72
  const uint16 *src = kPalette_MiscSprite + palette_sp6r_indoors * 7;
  Palette_LoadSingle(src, kPal_sp6r, 6);
}

/*
 * Outdoor variant: picks row 7 (light world) or row 9 (dark world) for
 * the misc-sprite palette and loads two adjacent rows — one into the
 * sp6r region (the live row) and one into either sp0r or sp7r
 * depending on whether the dungeon translucency swap is currently
 * active. The "previous row" copy keeps the cross-fade source available
 * when the light/dark world transition fades happen.
 */
void Palette_MiscSprite_Outdoors() {  // 9bed91
  int t = (overworld_screen_index & 0x40) ? 9 : 7;
  const uint16 *src = kPalette_MiscSprite + t * 7;
  Palette_LoadSingle(src, palette_swap_flag ? kPal_sp7r : kPal_sp0r, 6);
  Palette_LoadSingle(src - 7, kPal_sp6r, 6);
}

/* Loads the palace-map sprite palette block (6 colors x 2 rows) into
 * the dungeon-map sprite region. Used by the dungeon map screen. */
void Palette_Load_DungeonMapSprite() {  // 9beddd
  Palette_LoadMultiple(kPalette_PalaceMapSpr, kPal_PalaceMap, 6, 2);
}

/*
 * Loads Link's armor + gloves palette row into sp7l (the armor region).
 * The 15-color row is selected by link_armor. Always followed by a
 * gloves-tint update so the Power Glove / Titan's Mitt color overrides
 * the matching slot in the armor row.
 */
void Palette_Load_LinkArmorAndGloves() {  // 9bedf9
  const uint16 *src = kPalette_ArmorAndGloves + link_armor * 15;
  Palette_LoadMultiple_Arbitrary(src, kPal_ArmorGloves, 14);
  Palette_UpdateGlovesColor();
}

/*
 * Patches the gloves color in the armor palette row at offset 0xFD with
 * the Power Glove (0x52F6) or Titan's Mitt (0x376) tint. Both displayed
 * and target buffers are updated so the change shows immediately.
 * Skipped entirely when Link has no gloves equipped.
 */
void Palette_UpdateGlovesColor() {  // 9bee1b
  if (link_item_gloves)
    main_palette_buffer[0xfd] = aux_palette_buffer[0xfd] = kGlovesColor[link_item_gloves - 1];
  flag_update_cgram_in_nmi += 1;
}

/* Loads the palace-map BG palette (15 colors x 5 rows) into the
 * dungeon-map BG region starting at byte 0x40. */
void Palette_Load_DungeonMapBG() {  // 9bee3a
  Palette_LoadMultiple(kPalette_PalaceMapBg, 0x40, 15, 5);
}

/*
 * Loads the HUD palette row pair (15 colors x 2 rows) into the HUD
 * palette region (CGRAM 0x00..0x1F). hud_palette picks which 32-color
 * scheme; the standard HUD uses index 0 but selectable HUD colors
 * (e.g. the post-dungeon brighter HUD) bump it.
 */
void Palette_Load_HUD() {  // 9bee52
  const uint16 *src = kHudPalData + hud_palette * 32;
  Palette_LoadMultiple(src, 0x0, 15, 1);
}

/*
 * Loads the full main dungeon BG palette set: a 15-colors x 6-rows
 * block at byte offset 0x42 (the BG2/BG3 palette region). The source
 * pointer is keyed by palette_main_indoors >> 1, so each palace's two
 * stacked palette pages share a common color scheme. The first row is
 * also exposed as sp0r/sp7r for the misc-sprite slot.
 */
void Palette_Load_DungeonSet() {  // 9bee74
  const uint16 *src = kPalette_DungBgMain + (palette_main_indoors >> 1) * 90;
  Palette_LoadMultiple(src, 0x42, 14, 5);
  Palette_LoadSingle(src, palette_swap_flag ? kPal_sp7r : kPal_sp0r, 6);
}

/* Loads the overworld BG3 (top status row) palette from
 * kPalette_OverworldBgAux3 indexed by overworld_palette_aux3_bp7_lo. */
void Palette_Load_OWBG3() {  // 9beea8
  const uint16 *src = kPalette_OverworldBgAux3 + overworld_palette_aux3_bp7_lo * 7;
  Palette_LoadSingle(src, 0xE2, 6);
}

/*
 * Loads the main overworld BG palette block (6 colors x 4 rows). The
 * source row stride is 35 colors (5 sub-rows of 7 each); only the
 * first 4 of those are loaded for the main BG layer here.
 */
void Palette_Load_OWBGMain() {  // 9beec7
  const uint16 *src = kPalette_OverworldBgMain + overworld_palette_mode * 35;
  Palette_LoadMultiple(src, 0x42, 6, 4);
}

/* Loads the overworld BG1 (lower-half) auxiliary palette pair from
 * kPalette_OverworldBgAux12 indexed by overworld_palette_aux1_bp2to4_hi. */
void Palette_Load_OWBG1() {  // 9beee8
  const uint16 *src = kPalette_OverworldBgAux12 + overworld_palette_aux1_bp2to4_hi * 21;
  Palette_LoadMultiple(src, 0x52, 6, 2);
}

/* Loads the overworld BG2 (upper-half) auxiliary palette pair from
 * kPalette_OverworldBgAux12 indexed by overworld_palette_aux2_bp5to7_hi. */
void Palette_Load_OWBG2() {  // 9bef0c
  const uint16 *src = kPalette_OverworldBgAux12 + overworld_palette_aux2_bp5to7_hi * 21;
  Palette_LoadMultiple(src, 0xB2, 6, 2);
}

/*
 * Bottom-level helper that copies a single palette row of (x_ents+1)
 * colors from `src` into aux_palette_buffer at byte offset `dst`. The
 * dst offset is biased by overworld_palette_aux_or_main so callers can
 * target either the active page (0) or the alternate page (0x200).
 */
void Palette_LoadSingle(const uint16 *src, int dst, int x_ents) {  // 9bef30
  memcpy(&aux_palette_buffer[(dst + overworld_palette_aux_or_main) >> 1], src, sizeof(uint16) * (x_ents + 1));
}

/*
 * Loads y_pals+1 successive palette rows of (x_ents+1) colors each.
 * Each row stride in the destination is 32 bytes (one full SNES sub-
 * palette) and the source is read linearly. Used by every Load_*
 * helper that needs more than a single row.
 */
void Palette_LoadMultiple(const uint16 *src, int dst, int x_ents, int y_pals) {  // 9bef4b
  x_ents++;
  do {
    memcpy(&aux_palette_buffer[(dst + overworld_palette_aux_or_main) >> 1], src, sizeof(uint16) * x_ents);
    src += x_ents;
    dst += 32;
  } while (--y_pals >= 0);
}

/*
 * Like Palette_LoadSingle but writes to BOTH the aux and main buffers
 * (so the change is immediately visible without waiting for a fade)
 * and ignores overworld_palette_aux_or_main. Used for gear loads where
 * the new colors must show this frame.
 */
void Palette_LoadMultiple_Arbitrary(const uint16 *src, int dst, int x_ents) {  // 9bef7b
  memcpy(&aux_palette_buffer[dst >> 1], src, sizeof(uint16) * (x_ents + 1));
  memcpy(&main_palette_buffer[dst >> 1], src, sizeof(uint16) * (x_ents + 1));
}

/*
 * Builds the file-select preview palettes for all three save slots.
 * Walks each save's SRAM record (kSrmOffs_*) to read the saved
 * armor/gloves/sword/shield, then loads the matching gear colors into
 * the slot-specific palette region (one slot per 0x500-byte stride).
 * After all three slots are populated, the trailing loop overlays the
 * common sprite-main palette tail into entries 0xE8/0xF8 so the file-
 * select Link previews use the correct skin/cap colors.
 */
void Palette_LoadForFileSelect() {  // 9bef96
  uint8 *src = g_zenv.sram;
  for (int i = 0; i < 3; i++) {
    Palette_LoadForFileSelect_Armor(i * 0x20, src[kSrmOffs_Armor], src[kSrmOffs_Gloves]);
    Palette_LoadForFileSelect_Sword(i * 0x20, src[kSrmOffs_Sword]);
    Palette_LoadForFileSelect_Shield(i * 0x20, src[kSrmOffs_Shield]);
    src += 0x500;
  }
  for (int i = 0; i < 7; i++) {
    aux_palette_buffer[0xe8 + i] = main_palette_buffer[0xe8 + i] = kPalette_MainSpr[7 + i];
    aux_palette_buffer[0xf8 + i] = main_palette_buffer[0xf8 + i] = kPalette_MainSpr[15 + 7 + i];
  }
}

/*
 * Loads one save slot's armor (and optionally gloves) palette colors
 * into both palette buffers at offset k+0x81 (the per-slot Link tile
 * region). 15 colors are copied; if the save has gloves equipped, the
 * matching glove tint overrides slot k+0x8D.
 */
void Palette_LoadForFileSelect_Armor(int k, uint8 armor, uint8 gloves) {  // 9bf032
  const uint16 *pal = kPalette_ArmorAndGloves + armor * 15;
  for (int i = 0; i != 15; i++)
    aux_palette_buffer[k + 0x81 + i] = main_palette_buffer[k + 0x81 + i] = pal[i];
  if (gloves)
    aux_palette_buffer[k + 0x8d] = main_palette_buffer[k + 0x8d] = kGlovesColor[gloves - 1];
}

/*
 * Loads one save slot's sword palette colors into both palette buffers
 * at offset k+0x99. Saves with no sword still index row 0 to avoid an
 * out-of-bounds read.
 */
void Palette_LoadForFileSelect_Sword(int k, uint8 sword) {  // 9bf072
  const uint16 *src = kPalette_Sword + (sword ? sword - 1 : 0) * 3;
  for (int i = 0; i != 3; i++)
    aux_palette_buffer[k + 0x99 + i] = main_palette_buffer[k + 0x99 + i] = src[i];
}

/*
 * Loads one save slot's shield palette colors into both palette buffers
 * at offset k+0x9C. Mirrors the same one-based offset trick used by the
 * sword loader so saves with no shield still index a valid row.
 */
void Palette_LoadForFileSelect_Shield(int k, uint8 shield) {  // 9bf09a
  const uint16 *src = kPalette_Shield + (shield ? shield - 1 : 0) * 4;
  for (int i = 0; i != 4; i++)
    aux_palette_buffer[k + 0x9c + i] = main_palette_buffer[k + 0x9c + i] = src[i];
}

/*
 * Loads Agahnim's specialized palette set for the throne-room cutscene.
 * Pulls four palette rows from kPalette_SpriteAux1 (rows 14 and 21),
 * writing them into the four contiguous Agahnim palette regions
 * (0x162/0x182/0x1A2/0x1C2). Flags CGRAM dirty so the next NMI sends
 * the new colors to hardware.
 */
void Palette_LoadAgahnim() {  // 9bf0c2
  const uint16 *src = kPalette_SpriteAux1 + 14 * 7;
  Palette_LoadMultiple_Arbitrary(src, 0x162, 6);
  Palette_LoadMultiple_Arbitrary(src, 0x182, 6);
  Palette_LoadMultiple_Arbitrary(src, 0x1a2, 6);
  src = kPalette_SpriteAux1 + 21 * 7;
  Palette_LoadMultiple_Arbitrary(src, 0x1c2, 6);
  flag_update_cgram_in_nmi++;
}

/*
 * Drives the multi-frame screen-flash effect used by lightning, item
 * pickups, and the Triforce reveal. intro_times_pal_flash counts the
 * remaining flash ticks; while non-zero each call alternates between
 * brightening (Filter_Majorly_Whiten_Bg on odd ticks) and restoring
 * (Palette_Restore_BG_From_Flash on even ticks). When the counter
 * decrements to zero a final full restore brings the BG and HUD back
 * to their pre-flash state. Skipped entirely while a non-zero
 * submodule is running so the flash doesn't overlap dialog/menu code.
 */
void HandleScreenFlash() {  // 9de9b6
  int j = intro_times_pal_flash;
  if (!j || submodule_index != 0)
    return;
  if (!--intro_times_pal_flash) {
    Palette_Restore_BG_And_HUD();
    return;
  }

  if (j & 1)
    Filter_Majorly_Whiten_Bg();
  else
    Palette_Restore_BG_From_Flash();

  flag_update_cgram_in_nmi++;
}

