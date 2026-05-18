/*
 * ancilla.c - Ancilla (Projectile/Effect) System
 *
 * Implements 67 ancilla types for The Legend of Zelda: A Link to the Past.
 * "Ancilla" is the game's internal term for secondary game objects that are
 * neither sprites (enemies/NPCs) nor the player, but rather projectiles,
 * visual effects, and interactive objects spawned by items or events.
 *
 * This file covers: bombs, arrows, boomerang, hookshot, fire/ice rod shots,
 * Somarian Cane bullets/blocks, medallion spells (Ether, Bombos, Quake),
 * magic powder, item receipt animations, bed/sleep sequences, blast wall
 * explosions, dash dust, screen shake, jump splashes, sword sparkles,
 * wish pond items, and many more visual/gameplay effects.
 *
 * Each ancilla type has a handler function (e.g., Ancilla07_Bomb) dispatched
 * from the kAncilla_Funcs table. The system supports up to 10 concurrent
 * ancillae (indices 0-9), with slots 0-4 used for "important" ancillae
 * that get OAM sprite allocation, and slots 5-9 for minor effects.
 *
 * Related files:
 *   ancilla.h    - Function declarations and shared type definitions
 *   variables.h  - WRAM variable mappings (ancilla_type[], ancilla_x_lo[], etc.)
 *   sprite.h     - Sprite collision and hitbox utilities shared with ancillae
 *   assets.h     - ROM data tables referenced by ancilla drawing routines
 */

// Core ancilla definitions, function prototypes, and shared types
#include "ancilla.h"
// SNES WRAM variable mappings (all ancilla_* arrays, link_* state, etc.)
#include "variables.h"
// Sprite system utilities: collision checks, hitbox setup, damage application
#include "sprite.h"
// HUD drawing and item display routines
#include "hud.h"
// Graphics loading: palette management, chr tile transfers, DMA helpers
#include "load_gfx.h"
// Tagalong/follower logic (referenced for layer priority bits)
#include "tagalong.h"
// Overworld map tile attribute lookups
#include "overworld.h"
// Tile collision detection for both overworld and dungeon contexts
#include "tile_detect.h"
// Player state, movement, and item interaction
#include "player.h"
// Miscellaneous game module routing and utility functions
#include "misc.h"
// Dungeon room logic: doors, torch lighting, floor transitions
#include "dungeon.h"
// Sprite-specific handlers referenced for special collision cases
#include "sprite_main.h"
// ROM asset data tables (kGeneratedBombosArr, kWishPond2_OamFlags, etc.)
#include "assets.h"

/*
 * OAM allocation sizes for each ancilla type (indexed by type ID 0-67).
 * Each value represents the number of bytes of OAM space the ancilla needs
 * for its sprite tiles. This is multiplied by 4 to get the OAM entry count
 * during Ancilla_AllocateOamFromRegion calls. A value of 0 means the
 * ancilla type manages its own OAM allocation or uses no sprites.
 */
static const uint8 kAncilla_Pflags[68] = {
  0,    8,  0xc, 0x10, 0x10,    4, 0x10, 0x18,    8,    8,    8,    0, 0x14, 0, 0x10, 0x28,
  0x18, 0x10, 0x10, 0x10, 0x10,  0xc,    8,    8, 0x50,    0, 0x10,    8, 0x40, 0,  0xc, 0x24,
  0x10,  0xc,    8, 0x10, 0x10,    4,  0xc, 0x1c,    0, 0x10, 0x14, 0x14, 0x10, 8, 0x20, 0x10,
  0x10, 0x10,    4,    0, 0x80, 0x10,    4, 0x30, 0x14, 0x10,    0, 0x10,    0, 0,    8,    0,
  0x10,    8, 0x78, 0x80,
};
// Fire rod / sword beam X velocities indexed by [direction + sword_level * 4].
// Three tiers of speed (40, 48, 64) correspond to sword levels 2, 3, and 4.
static const int8 kFireRod_Xvel2[12] = {0, 0, -40, 40, 0, 0, -48, 48, 0, 0, -64, 64};
// Fire rod / sword beam Y velocities, same indexing as kFireRod_Xvel2.
static const int8 kFireRod_Yvel2[12] = {-40, 40, 0, 0, -48, 48, 0, 0, -64, 64, 0, 0};
// OAM priority bits for tagalong/follower layer ordering.
// Indexed by link_is_on_lower_level to set proper BG priority.
static const uint8 kTagalongLayerBits[4] = {0x20, 0x10, 0x30, 0x20};
// Stereo panning values for Bombos spell sound effects.
// Indexed by (x_screen_pos >> 5) to pan audio left/right based on X position.
static const uint8 kBombos_Sfx[8] = {0x80, 0x80, 0x80, 0, 0, 0x40, 0x40, 0x40};
// Frame duration for each of the 11 bomb animation phases.
// Phase 0 = idle (0xA0 = 160 frames), phases 1-10 = explosion sequence.
const uint8 kBomb_Tab0[11] = {0xA0, 6, 4, 4, 4, 4, 4, 6, 6, 6, 6};

// Sword beam temporary variables stored in extended RAM ($7F:5800 region).
// These overlay the same memory used by ether/bombos spell variables,
// since sword beams and medallion spells cannot be active simultaneously.
#define swordbeam_temp_x (*(uint16*)(g_ram+0x1580E))
#define swordbeam_temp_y (*(uint16*)(g_ram+0x15810))
#define swordbeam_arr ((uint8*)(g_ram+0x15800))
#define swordbeam_var1 (*(uint8*)(g_ram+0x15804))
#define swordbeam_var2 (*(uint8*)(g_ram+0x15808))
/*
 * Tile collision attribute classification table for Class2 ancillae (arrows, hookshot).
 * Maps each of the 256 possible tile attribute values to a collision behavior:
 *   0 = passable (no collision)
 *   1 = solid wall (ancilla stops and triggers hit effect)
 *   2 = sloped tile (requires slope-specific collision math)
 *   3 = inter-floor passage (collides only if ancilla is on the mirror floor)
 *   4 = floor transition tile (toggles ancilla between upper/lower floor)
 */
static const int8 kAncilla_TileColl_Attrs[256] = {
  0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 0, 0, 0, 0, 2, 2, 2, 2, 0, 3, 3, 3,
  0, 0, 0, 0, 0, 0, 1, 1, 4, 4, 4, 4, 4, 4, 4, 4,
  1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 3, 3, 3,
  0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 4,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
/*
 * Tile collision attribute classification for standard ancillae (fire rod, ice rod, etc.).
 * Same value meanings as kAncilla_TileColl_Attrs but with slightly different
 * classifications for certain tile types (e.g., tile 0x03 is classified as 3 here
 * instead of 0, and rows 0x50-0x7F have different solid/passable assignments).
 * Used by Ancilla_CheckTileCollision_targeted() for non-Class2 projectiles.
 */
static const uint8 kAncilla_TileColl0_Attrs[256] = {
  0, 1, 0, 3, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 0, 0, 0, 0, 2, 2, 2, 2, 0, 3, 3, 3,
  0, 0, 0, 0, 0, 0, 1, 1, 4, 4, 4, 4, 4, 4, 4, 4,
  1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 3, 3, 3,
  0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 4,
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0,
  0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
// Bomb drawing frame sequence: maps bomb_phase (0-11) to a sprite-sheet row index
// selecting which animation frame to display. The pattern 2,3,2,3 in the middle
// produces a two-frame flicker for the detonation warning.
static const uint8 kBomb_Draw_Tab0[12] = {0, 1, 2, 3, 2, 3, 4, 5, 6, 7, 8, 9};
// Bomb OAM tile count per explosion phase (0-10): phase 0 = 1 tile (idle bomb),
// phases 1-5 = 4 tiles (mid explosion ring), phases 6-10 = 4/6 tiles (outer ring).
static const uint8 kBomb_Draw_Tab2[11] = {1, 4, 4, 4, 4, 4, 5, 4, 6, 6, 6};

// Magic powder sparkle animation frame sequence (40 entries = 4 cycles of 10 frames).
// Each value is a tile index into the powder sprite sheet; frames 13-18 are the initial
// "poof" burst and frames 0-12 are the recurring sparkle loop. The repeated 0-6 segments
// form looping shimmer cycles after the initial spray.
static const uint8 kMagicPowder_Tab0[40] = {
  13, 14, 15, 0,  1,  2,  3, 4, 5, 6, 10, 11, 12, 0, 1, 2,
  3,  4,  5, 6, 16, 17, 18, 0, 1, 2,  3,  4,  5, 6, 7, 8,
  9,  0,  1, 2,  3,  4,  5, 6,
};
/*
 * Ether spell state variables, overlaid on the extended RAM window at $7F:5800.
 * These share memory with the sword-beam temporaries (swordbeam_arr/temp_x/y)
 * since the Ether spell and sword beams cannot be active simultaneously.
 *   ether_arr1        — base array for per-bolt position tracking (8 bytes)
 *   ether_var2        — general animation counter / phase tracker
 *   ether_y2          — screen Y of the lightning strike center
 *   ether_y_adjusted  — Y adjusted for scroll offset during the strike
 *   ether_x2          — screen X of the lightning strike center
 *   ether_y3          — secondary Y coordinate (bolt endpoint)
 *   ether_var1        — sub-step counter within the current Ether phase
 *   ether_y           — world Y of the Ether spell anchor point
 *   ether_x           — world X of the Ether spell anchor point
 */
#define ether_arr1 ((uint8*)(g_ram+0x15800))
#define ether_var2 (*(uint8*)(g_ram+0x15808))
#define ether_y2 (*(uint16*)(g_ram+0x1580A))
#define ether_y_adjusted (*(uint16*)(g_ram+0x1580C))
#define ether_x2 (*(uint16*)(g_ram+0x1580E))
#define ether_y3 (*(uint16*)(g_ram+0x15810))
#define ether_var1 (*(uint8*)(g_ram+0x15812))
#define ether_y (*(uint16*)(g_ram+0x15813))
#define ether_x (*(uint16*)(g_ram+0x15815))
// Ether blitz orb sprite chars: 4 pairs (each used for two consecutive frames) cycling
// through 4 tiles (0x48, 0x4a, 0x4c, 0x4e) for the expanding lightning orb animation.
static const uint8 kEther_BlitzOrb_Char[8] = {0x48, 0x48, 0x4a, 0x4a, 0x4c, 0x4c, 0x4e, 0x4e};
// OAM flags for each Ether blitz orb tile: alternates 0x3c / 0x7c (H-flip) to produce
// a left/right mirrored flicker on the orb as it expands.
static const uint8 kEther_BlitzOrb_Flags[8] = {0x3c, 0x7c, 0x3c, 0x7c, 0x3c, 0x7c, 0x3c, 0x7c};
// Char indices for the four segments of the Ether lightning bolt beam drawn downward
// from the orb: 0x40 (top), 0x42, 0x44, 0x46 (bottom — each a 16×16 section).
static const uint8 kEther_BlitzSegment_Char[4] = {0x40, 0x42, 0x44, 0x46};
/*
 * Bombos spell state arrays, overlaid on the same $7F:5800 extended RAM window as
 * the Ether/sword-beam variables. Only one medallion spell can be active at a time.
 *   bombos_arr1 — X/Y screen positions for the 8 simultaneous Bombos blast circles
 *   bombos_arr2 — countdown timers for each blast circle's individual animation phase
 */
#define bombos_arr1 ((uint8*)(g_ram+0x15800))
#define bombos_arr2 ((uint8*)(g_ram+0x15810))
// Screen-relative X/Y offsets for the 72 Bombos blast impact positions.
// Pairs of values [x, y] encode each fire circle spawn location relative to the
// screen center; the spell distributes 9 circles across 8 animation steps,
// producing the spiraling ring-of-fire pattern.
static const uint8 kBombosBlasts_Tab[72] = {
  0xb6, 0x5d, 0xa1, 0x30, 0x69, 0xb5, 0xa3, 0x24, 0x96, 0xac, 0x73, 0x5f, 0x92, 0x48, 0x52, 0x81,
  0x39, 0x95, 0x7f, 0x20, 0x88, 0x5d, 0x34, 0x98, 0xbc, 0xd2, 0x51, 0x77, 0xa2, 0x47, 0x94, 0xb2,
  0x34, 0xda, 0x30, 0x62, 0x9f, 0x76, 0x51, 0x46, 0x98, 0x5c, 0x9b, 0x61, 0x58, 0x95, 0x4c, 0xba,
  0x7e, 0xcb, 0x12, 0xd0, 0x70, 0xa6, 0x46, 0xbf, 0x40, 0x50, 0x7e, 0x8c, 0x2d, 0x61, 0xac, 0x88,
  0x20, 0x6a, 0x72, 0x5f, 0xd2, 0x28, 0x52, 0x80,
};
// DMA transfer sizes for each of the 5 Quake spell animation phases.
// Controls how many CHR bytes are uploaded to VRAM per phase of the ground-crack animation.
static const uint8 kQuake_Tab1[5] = {0x17, 0x16, 0x17, 0x16, 0x10};
// CHR tile indices for the 15 ground-bolt sprite tiles drawn during the Quake spell.
// Tiles 0x40-0x6A form the progressive crack pattern radiating outward from the
// epicenter; 0x63 is the final center tile placed last.
static const uint8 kQuakeDrawGroundBolts_Char[15] = { 0x40, 0x42, 0x44, 0x46, 0x48, 0x4a, 0x4c, 0x4e, 0x60, 0x62, 0x64, 0x66, 0x68, 0x6a, 0x63 };
// Describes a single OAM tile in the Quake spell's animated rock/bolt display.
// x, y: signed pixel offsets from the Quake epicenter (Link's position).
// f: OAM flags byte (palette, priority, H/V flip).
typedef struct QuakeItem {
  int8 x, y;
  uint8 f;
} QuakeItem;
// Per-frame OAM tile descriptors for the right-side Quake rock shower (64 steps).
// Each step adds one or more tiles (x, y, flags) to the OAM for that animation frame.
// kQuakeItemPos[] maps each frame index to the starting entry in this array.
static const QuakeItem kQuakeItems[] = {
  {0, -16, 0x00},
  {0, -16, 0x01},
  {0, -16, 0x02},
  {0, -16, 0x03},
  {0, -16, 0x43},
  {0, -16, 0x42},
  {0, -16, 0x41},
  {0, -16, 0x40},
  {0, -16, 0x40}, {14, -8, 0x84},
  {29, -8, 0x44}, {13, -7, 0x84},
  {31, -7, 0x44}, {47, -4, 0x84},
  {49, -11, 0x06}, {63, -5, 0x44}, {47, -4, 0x84},
  {36, -17, 0x08}, {49, -11, 0x06}, {63, -5, 0x44}, {78, 4, 0x08},
  {22, -31, 0x08}, {36, -17, 0x08}, {78, 4, 0x08}, {93, 20, 0x08},
  {7, -46, 0x08}, {23, -45, 0x48}, {22, -31, 0x08}, {93, 20, 0x08}, {93, 36, 0x48},
  {-7, -61, 0x08}, {37, -59, 0x48}, {7, -46, 0x08}, {23, -45, 0x48}, {93, 36, 0x48}, {93, 52, 0x08},
  {-22, -75, 0x08}, {47, -74, 0x01}, {-8, -61, 0x08}, {36, -60, 0x48}, {93, 52, 0x08}, {108, 67, 0x08},
  {-37, -90, 0x08}, {-22, -75, 0x08}, {47, -74, 0x01}, {59, -62, 0x81}, {108, 67, 0x08}, {121, 80, 0x08},
  {-44, -104, 0xc9}, {-37, -90, 0x08}, {73, -74, 0x48}, {59, -62, 0x81}, {121, 80, 0x08},
  {-44, -120, 0x09}, {-44, -104, 0xc9}, {87, -89, 0x48}, {73, -74, 0x48},
  {-44, -120, 0x09}, {102, -104, 0x48}, {87, -89, 0x48},
  {102, -104, 0x48}, {87, -89, 0x48},
  {112, -116, 0x48}, {102, -104, 0x48},
  {112, -116, 0x48},
  {-13, -16, 0x00},
  {-13, -16, 0x01},
  {-13, -16, 0x02},
  {-13, -16, 0x03},
  {-11, -16, 0x43},
  {-11, -16, 0x42},
  {-11, -16, 0x41},
  {-11, -16, 0x40}, {-24, -10, 0x04},
  {-38, -18, 0x08}, {-24, -10, 0x04}, {-40, -7, 0xc4},
  {-45, -33, 0xc9}, {-38, -18, 0x08}, {-57, -7, 0x04}, {-40, -7, 0xc4},
  {-48, -45, 0x07}, {-45, -33, 0xc9}, {-57, -7, 0x04}, {-71, 2, 0x48},
  {-48, -45, 0x06}, {-71, 2, 0x48}, {-70, 18, 0x08},
  {-48, -45, 0x05}, {-70, 18, 0x08}, {-56, 33, 0x08},
  {-48, -45, 0x07}, {-54, 34, 0x08}, {-54, 49, 0x88},
  {-48, -45, 0x06}, {-54, 49, 0x88}, {-69, 64, 0x88},
  {-48, -45, 0x07}, {-69, 64, 0x88}, {-85, 73, 0xc4},
  {-48, -45, 0x05}, {-101, 73, 0x04}, {-85, 73, 0xc4},
  {-60, -53, 0x08}, {-48, -45, 0x06}, {-101, 73, 0x04}, {-116, 77, 0xc4},
  {-75, -67, 0x08}, {-60, -53, 0x08}, {-128, 76, 0x04}, {-116, 77, 0xc4},
  {-90, -82, 0x08}, {-75, -67, 0x08}, {-128, 76, 0x04},
  {-105, -97, 0x08}, {-90, -82, 0x08},
  {-120, -111, 0x08}, {-105, -97, 0x08},
  {-120, -111, 0x08},
  {0, -5, 0x0a},
  {0, -5, 0x0b},
  {2, -3, 0x0c},
  {1, -3, 0x0d},
  {0, -3, 0x8d},
  {1, -3, 0x8c},
  {1, -3, 0x8b},
  {1, -3, 0x8a}, {-6, 12, 0x89},
  {-6, 12, 0x89}, {-10, 28, 0xc9},
  {-10, 28, 0x49}, {-8, 44, 0x89},
  {-8, 44, 0x89}, {-10, 56, 0x02},
  {-10, 56, 0x02}, {-23, 70, 0x48}, {5, 70, 0x08},
  {-23, 70, 0x48}, {5, 70, 0x08}, {-38, 85, 0x48}, {19, 85, 0x08},
  {-38, 85, 0x48}, {19, 85, 0x08}, {-52, 99, 0x48}, {33, 101, 0x08},
  {-52, 99, 0x48}, {33, 101, 0x08}, {-66, 113, 0x48}, {47, 115, 0x08},
  {-66, 113, 0x48}, {47, 115, 0x08},
};
// Start index into kQuakeItems[] for each of the 64 animation frames of the right-side
// Quake rock shower. Frame i draws entries [kQuakeItemPos[i], kQuakeItemPos[i+1]).
static const uint8 kQuakeItemPos[] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 17, 21, 25, 30,
  36, 42, 48, 53, 57, 60, 62, 64, 65, 66, 67, 68, 69, 70, 71, 72, 74, 77, 81, 85, 88, 91, 94, 97, 100, 103, 107, 111, 114, 116, 118, 119, 120, 121, 122, 123, 124, 125, 126, 128, 130, 132, 134, 137, 141, 145, 149, 151
};
// Per-frame OAM tile descriptors for the left-side Quake rock shower and the two
// symmetric diagonal showers (upper-right and lower-left arms of the spell).
// Same format as kQuakeItems[]; indexed by kQuakeItemPos2[].
static const QuakeItem kQuakeItems2[] = {
  {-96, 112, 0x20},
  {-96, 112, 0x21},
  {-96, 112, 0x66},
  {-96, 112, 0x22},
  {-96, 112, 0x23},
  {-96, 112, 0x63},
  {-96, 112, 0x62},
  {-96, 112, 0x26},
  {-96, 112, 0x27}, {-86, 124, 0x28},
  {-86, 124, 0x28}, {-72, -117, 0x28},
  {-72, -117, 0x28}, {-59, -102, 0xa1},
  {-59, -102, 0xa1}, {-44, -116, 0x68},
  {-44, -116, 0x68}, {-29, 126, 0x68},
  {-29, 126, 0x68},
  {-19, 125, 0xc5},
  {-112, 96, 0x2a},
  {-112, 96, 0x2b},
  {-112, 96, 0x2c},
  {-112, 96, 0x2d},
  {-119, 82, 0x29}, {-112, 96, 0x2a},
  {-123, 66, 0xe9}, {-119, 82, 0x29},
  {-121, 50, 0x29}, {-123, 66, 0xe9},
  {126, 34, 0x28}, {-115, 34, 0x68}, {-121, 50, 0x29},
  {-106, 18, 0xa9}, {111, 19, 0x28}, {126, 34, 0x28}, {-115, 34, 0x68},
  {-100, 2, 0x68}, {102, 4, 0xe9}, {-106, 18, 0xa9}, {111, 19, 0x28},
  {-91, -14, 0xa9}, {95, -11, 0x28}, {-100, 2, 0x68}, {102, 4, 0xe9},
  {96, 112, 0x60},
  {96, 112, 0x61},
  {96, 112, 0x26},
  {96, 112, 0x62},
  {96, 112, 0x63},
  {96, 112, 0x23},
  {96, 112, 0x22},
  {96, 112, 0x66},
  {85, 111, 0xe8}, {96, 112, 0x67},
  {70, 104, 0x24}, {85, 111, 0xe8},
  {70, 104, 0x24}, {54, 108, 0xe4},
  {40, 100, 0x28}, {38, 107, 0x24}, {54, 108, 0xe4},
  {25, 85, 0x28}, {40, 100, 0x28}, {38, 107, 0x24}, {22, 110, 0xe4},
  {11, 70, 0x28}, {25, 85, 0x28}, {7, 108, 0x24}, {22, 110, 0xe4},
  {11, 70, 0x28}, {7, 108, 0x24},
  {112, 112, 0x2a},
  {112, 112, 0x2b},
  {112, 112, 0x2c},
  {112, 112, 0x2d},
  {112, 112, 0x2a}, {108, 125, 0x29},
  {108, 125, 0x29}, {114, -116, 0x28},
  {114, -116, 0x28}, {124, -100, 0x29},
  {124, -100, 0x29}, {123, -84, 0xe9},
  {123, -84, 0xe9}, {117, -74, 0xe4}, {-124, -69, 0x28},
  {117, -74, 0xe4}, {-124, -69, 0x28}, {103, -67, 0x68}, {-110, -54, 0x28},
  {103, -67, 0x68}, {-110, -54, 0x28}, {95, -52, 0x69}, {-102, -39, 0x29},
  {95, -52, 0x69}, {-102, -39, 0x29}, {96, -36, 0xe9}, {-102, -24, 0xe9},
  {96, -36, 0xe9}, {-102, -24, 0xe9},
  {-123, -14, 0x29}, {-115, -14, 0x2e}, {49, -12, 0x28},
};
// Start index into kQuakeItems2[] for each of the 56 animation frames of the secondary
// Quake rock showers (left arm, upper-right arm, lower-left arm).
static const uint8 kQuakeItemPos2[] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 19, 20, 21, 22, 23, 24, 26, 28, 30, 33, 37, 41, 45, 46, 47, 48, 49, 50, 51, 52, 53, 55, 57, 59, 62, 66, 70, 72, 73, 74, 75, 76, 78, 80, 82, 84, 87, 91, 95, 99, 101, 104
};
// Item-receipt animation: number of sparkle frames per phase (3 phases: start, mid, end).
static const uint8 kReceiveItem_Tab4[3] = {9, 5, 5};
// Item-receipt sparkle tile chars for each of the 3 animation phases.
static const uint8 kReceiveItem_Tab5[3] = {0x24, 0x25, 0x26};
// Item-receipt animation speed (countdown step size) per phase; higher = faster.
static const uint8 kReceiveItem_Tab0[3] = {5, 1, 4};
// Dialogue message IDs triggered on item receipt, indexed by item type (0-75).
// -1 means no message for that item. Includes all equipment, upgrades, pendants,
// crystals, and key items. Extends into kReceiveItemMsgs2/3 for alternate forms.
static const int16 kReceiveItemMsgs[76] = {
  -1, 0x70, 0x77, 0x52,   -1, 0x78,  0x78, 0x62, 0x61, 0x66, 0x69, 0x53, 0x52, 0x56,   -1,  0x64,
  0x63, 0x65, 0x51, 0x54, 0x67, 0x68,  0x6b, 0x77, 0x79, 0x55, 0x6e, 0x58, 0x6d, 0x5d, 0x57,  0x5e,
  -1, 0x74, 0x75, 0x76,   -1, 0x5f, 0x158,   -1, 0x6a, 0x5c, 0x8f, 0x71, 0x72, 0x73, 0x71,  0x72,
  0x73, 0x6a, 0x6c, 0x60,   -1,   -1,    -1, 0x59, 0x84, 0x5a,   -1,   -1,   -1,   -1,   -1, 0x159,
  -1,   -1,   -1,   -1,   -1,   -1,    -1,   -1,   -1, 0xdb, 0x67, 0x7c,
};
// Overflow message IDs for items beyond index 75 (shield upgrades: mirror, fighter).
static const int16 kReceiveItemMsgs2[2] = {0x5b, 0x83};
// Message IDs for dungeon map/compass items (index 0 = none, 1-3 = overworld maps).
static const int16 kReceiveItemMsgs3[4] = {-1, 0x155, 0x156, 0x157};
// DMA source offsets for the 4 flute bird wing-flap animation frames loaded from ROM.
static const uint8 kTravelBird_DmaStuffs[4] = {0, 0x20, 0x40, 0xe0};
// Flute bird (Cucco) OAM tile X offsets for the 3 body parts: body, left wing, right wing.
static const int8 kTravelBird_Draw_X[3] = {0, -9, -9};
// Flute bird OAM tile Y offsets for body, left wing, right wing.
static const int8 kTravelBird_Draw_Y[3] = {0, 12, 20};
// CHR tile indices for the 3 flute bird body parts (body = 0x0e, wings = 0x00/0x02).
static const uint8 kTravelBird_Draw_Char[3] = {0xe, 0, 2};
// OAM flags for the 3 flute bird tiles: 0x22 (body, palette 2), 0x2e (wings, priority).
static const uint8 kTravelBird_Draw_Flags[3] = {0x22, 0x2e, 0x2e};
// Somaria Block collision sensor X offsets (12 entries: 4 cardinal + 8 diagonal probes).
// Used to test the 4 sides and 4 corners of the block for wall contact.
static const int8 kSomarianBlock_Coll_X[12] = {0, 0, -8, 8, 0, 0, 0, 0, 8, -8, -8, 8};
// Somaria Block collision sensor Y offsets, paired with kSomarianBlock_Coll_X.
static const int8 kSomarianBlock_Coll_Y[12] = {-8, 8, 0, 0, 0, 0, 0, 0, -8, 8, -8, 8};
/*
 * Per-frame dispatch table for all 67 ancilla types (indexed by ancilla_type[k] - 1).
 * Each entry is a function pointer to the handler for that ancilla type.
 * Handlers are responsible for: moving the ancilla, checking tile/sprite collisions,
 * updating animation state, allocating OAM entries, and deactivating when done.
 * Multiple entries may point to the same handler (e.g., BlastWallExplosion handles
 * types 0x0E, 0x0F, 0x10, and 0x12).
 */
static HandlerFuncK *const kAncilla_Funcs[67] = {
  &Ancilla01_SomariaBullet,
  &Ancilla02_FireRodShot,
  &Ancilla_Empty,
  &Ancilla04_BeamHit,
  &Ancilla05_Boomerang,
  &Ancilla06_WallHit,
  &Ancilla07_Bomb,
  &Ancilla08_DoorDebris,
  &Ancilla09_Arrow,
  &Ancilla0A_ArrowInTheWall,
  &Ancilla0B_IceRodShot,
  &Ancilla_SwordBeam,
  &Ancilla0D_SpinAttackFullChargeSpark,
  &Ancilla33_BlastWallExplosion,
  &Ancilla33_BlastWallExplosion,
  &Ancilla33_BlastWallExplosion,
  &Ancilla11_IceRodWallHit,
  &Ancilla33_BlastWallExplosion,
  &Ancilla13_IceRodSparkle,
  &Ancilla_Unused_14,
  &Ancilla15_JumpSplash,
  &Ancilla16_HitStars,
  &Ancilla17_ShovelDirt,
  &Ancilla18_EtherSpell,
  &Ancilla19_BombosSpell,
  &Ancilla1A_PowderDust,
  &Ancilla_SwordWallHit,
  &Ancilla1C_QuakeSpell,
  &Ancilla1D_ScreenShake,
  &Ancilla1E_DashDust,
  &Ancilla1F_Hookshot,
  &Ancilla20_Blanket,
  &Ancilla21_Snore,
  &Ancilla22_ItemReceipt,
  &Ancilla23_LinkPoof,
  &Ancilla24_Gravestone,
  &Ancilla_Unused_25,
  &Ancilla26_SwordSwingSparkle,
  &Ancilla27_Duck,
  &Ancilla28_WishPondItem,
  &Ancilla29_MilestoneItemReceipt,
  &Ancilla2A_SpinAttackSparkleA,
  &Ancilla2B_SpinAttackSparkleB,
  &Ancilla2C_SomariaBlock,
  &Ancilla2D_SomariaBlockFizz,
  &Ancilla2E_SomariaBlockFission,
  &Ancilla2F_LampFlame,
  &Ancilla30_ByrnaWindupSpark,
  &Ancilla31_ByrnaSpark,
  &Ancilla32_BlastWallFireball,
  &Ancilla33_BlastWallExplosion,
  &Ancilla34_SkullWoodsFire,
  &Ancilla35_MasterSwordReceipt,
  &Ancilla36_Flute,
  &Ancilla37_WeathervaneExplosion,
  &Ancilla38_CutsceneDuck,
  &Ancilla39_SomariaPlatformPoof,
  &Ancilla3A_BigBombExplosion,
  &Ancilla3B_SwordUpSparkle,
  &Ancilla3C_SpinAttackChargeSparkle,
  &Ancilla3D_ItemSplash,
  &Ancilla_RisingCrystal,
  &Ancilla3F_BushPoof,
  &Ancilla40_DwarfPoof,
  &Ancilla41_WaterfallSplash,
  &Ancilla42_HappinessPondRupees,
  &Ancilla43_GanonsTowerCutscene,
};
// Returns the 16-bit world X coordinate of ancilla slot k by combining the lo/hi bytes.
uint16 Ancilla_GetX(int k) {
  return ancilla_x_lo[k] | ancilla_x_hi[k] << 8;
}

// Returns the 16-bit world Y coordinate of ancilla slot k by combining the lo/hi bytes.
uint16 Ancilla_GetY(int k) {
  return ancilla_y_lo[k] | ancilla_y_hi[k] << 8;
}

// Writes a 16-bit world X coordinate into the split lo/hi byte fields of ancilla slot k.
void Ancilla_SetX(int k, uint16 x) {
  ancilla_x_lo[k] = x;
  ancilla_x_hi[k] = x >> 8;
}

// Writes a 16-bit world Y coordinate into the split lo/hi byte fields of ancilla slot k.
void Ancilla_SetY(int k, uint16 y) {
  ancilla_y_lo[k] = y;
  ancilla_y_hi[k] = y >> 8;
}

// Finds any free ancilla slot (searching all 10 slots, 9 down to 0).
// Returns the slot index, or -1 if all slots are occupied.
// Unlike Ancilla_AllocInit (which prefers low slots for important ancillae),
// this is used for minor effects that can use any slot.
int Ancilla_AllocHigh() {
  for (int k = 9; k >= 0; k--) {
    if (ancilla_type[k] == 0)
      return k;
  }
  return -1;
}

// Writes a single OAM entry for an ancilla tile, with screen-bounds culling.
// Sets oam->y = 0xF0 (hidden) if the tile is off-screen in X or Y.
// Handles extended screen mode (kFeatures0_ExtendScreen64) by widening the X check.
// Merges the X-overflow bit (x >= 256) into the extended OAM byte (big).
// Parameters:
//   oam     — pointer to the OAM entry to write
//   x, y    — screen-space tile position
//   charnum — sprite-sheet character index
//   flags   — OAM attribute byte (palette, priority, flip)
//   big     — base extended OAM size bit; X-overflow is OR'd in automatically
static void Ancilla_SetOam(OamEnt *oam, uint16 x, uint16 y, uint8 charnum, uint8 flags, uint8 big) {
  uint8 yval = 0xf0;
  int xt = enhanced_features0 & kFeatures0_ExtendScreen64 ? 0x40 : 0;
  if ((uint16)(x + xt) < 256 + xt * 2 && y < 256) {
    big |= (x >> 8) & 1;
    oam->x = x;
    if (y < 0xf0)
      yval = y;
  }
  oam->y = yval;
  oam->charnum = charnum;
  oam->flags = flags;
  bytewise_extended_oam[oam - oam_buf] = big;
}

// Variant of Ancilla_SetOam that uses a wider X window for partially-visible tiles.
// Tests (x + 0x80) < 0x180 instead of x < 256, allowing tiles that are up to 128 pixels
// off the left or right edge to still write their x/flags (for hookshot chain links and
// wide-arc effects that need partial visibility). Y culling uses a 16-pixel top margin
// (y + 0x10 < 0x100) instead of the strict y < 0xF0 used by Ancilla_SetOam.
static void Ancilla_SetOam_Safe(OamEnt *oam, uint16 x, uint16 y, uint8 charnum, uint8 flags, uint8 big) {
  uint8 yval = 0xf0;
  oam->x = x;
  int xt = enhanced_features0 & kFeatures0_ExtendScreen64 ? 0x48 : 0;
  if ((uint16)(x + 0x80) < (0x180 + xt)) {
    big |= (x >> 8) & 1;
    if ((uint16)(y + 0x10) < 0x100)
      yval = y;
  }
  oam->y = yval;
  oam->charnum = charnum;
  oam->flags = flags;
  bytewise_extended_oam[oam - oam_buf] = big;
}

// No-op ancilla handler for type 0x03 (placeholder / retired effect).
void Ancilla_Empty(int k) {
}

// Stub handler for ancilla type 0x14 — this type was removed in the original ROM.
// Asserts to catch any accidental activation during development.
void Ancilla_Unused_14(int k) {
  assert(0);
}

// Stub handler for ancilla type 0x25 — this type was removed in the original ROM.
// Asserts to catch any accidental activation during development.
void Ancilla_Unused_25(int k) {
  assert(0);
}

// Draws the spin-attack charge spark or full-charge burst at ancilla slot k.
// offs selects which 4-tile block to draw from the 8 available animation steps:
//   ancilla_item_to_link[k] + offs indexes into kInitialSpinSpark_Char/Flags/X/Y
//   at multiples of 4 to pick the correct animation frame.
// Each frame uses up to 4 OAM tiles; entries with char == 0xFF are skipped (sparse frames).
// OAM priority comes from oam_priority_value (high byte), blended into the flags byte.
// Parameters:
//   k    — ancilla slot index
//   offs — frame offset (0 for charge sparks, larger for full-charge burst)
void SpinSpark_Draw(int k, int offs) {
  static const uint8 kInitialSpinSpark_Char[32] = {
    0x92, 0xff, 0xff, 0xff, 0x8c, 0x8c, 0x8c, 0x8c, 0xd6, 0xd6, 0xd6, 0xd6, 0x93, 0x93, 0x93, 0x93,
    0xd6, 0xd6, 0xd6, 0xd6, 0xd7, 0xff, 0xff, 0xff, 0x80, 0xff, 0xff, 0xff, 0x22, 0xff, 0xff, 0xff,  // wtf oob
  };
  static const uint8 kInitialSpinSpark_Flags[29] = {
    0x22, 0xff, 0xff, 0xff, 0x22, 0x62, 0xa2, 0xe2, 0x24, 0x64, 0xa4, 0xe4, 0x22, 0x62, 0xa2, 0xe2,
    0x22, 0x62, 0xa2, 0xe2, 0x22, 0xff, 0xff, 0xff, 0x22, 0xff, 0xff, 0xff,
    0xfc,
  };
  static const int8 kInitialSpinSpark_Y[29] = {
    -4,  0, 0, 0, -8, -8, 0, 0, -8, -8, 0, 0, -8, -8, 0, 0,
    -8, -8, 0, 0, -4,  0, 0, 0, -4,  0, 0, 0,
    -4,
  };
  static const int16 kInitialSpinSpark_X[29] = {
    -4, 0,  0, 0, -8, 0, -8, 0, -8, 0, -8, 0, -8, 0, -8, 0,
    -8, 0, -8, 0, -4, 0,  0, 0, -4, 0,  0, 0,
    0x11a5
  };
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);
  OamEnt *oam = GetOamCurPtr();
  int t = (ancilla_item_to_link[k] + offs) * 4;
  assert(t < 32);
  for(int i = 0; i < 4; i++, t++) {
    if (kInitialSpinSpark_Char[t] != 0xff) {
      Ancilla_SetOam(oam, info.x + kInitialSpinSpark_X[t], info.y + kInitialSpinSpark_Y[t],
                     kInitialSpinSpark_Char[t], kInitialSpinSpark_Flags[t] & ~0x30 | HIBYTE(oam_priority_value), 0);
      oam++;
    }
  }
}

// Returns true if the 4 OAM entries for a Somaria Block are all hidden or off-screen.
// First pass: if all 4 entries have y == 0xF0 (all hidden), returns true immediately.
// Second pass: if any entry is visible but has the extended-OAM size bit clear (8×8 tile),
// returns false — the block still has visible 8×8 pixels that must be accounted for.
// Used to determine whether the fizzle-out animation has fully vanished before cleanup.
bool SomarianBlock_CheckEmpty(OamEnt *oam) {
  for (int i = 0; i != 4; i++) {
    if (oam[i].y == 0xf0)
      continue;
    for (i = 0; i < 4; i++)
      if (!(bytewise_extended_oam[oam + i - oam_buf] & 1))
        return false;
    break;
  }
  return true;
}

// Spawns a dashing dust puff ancilla at Link's current position.
// Parameters:
//   a    — ancilla type (0x1E = standard dash dust)
//   y    — unused (overwritten to 1 inside AncillaAdd_DashDust, kept for signature compatibility)
//   flag — 0 = dust at Link's feet (centered), non-zero = directional offset from
//          kAddDashingDust_X/Y so the puff appears where Link's foot leaves the ground
// Sets ancilla_step[k] = flag to distinguish foot-lift vs. centre puffs during drawing.
// kAddDashingDust_X/Y: per-direction pixel offsets (indexed by link_direction_facing >> 1)
// for placing the puff at the correct foot position for each of the 4 facing directions.
void AddDashingDustEx(uint8 a, uint8 y, uint8 flag) {
  static const int8 kAddDashingDust_X[4] = {4, 4, 6, 0};
  static const int8 kAddDashingDust_Y[4] = {20, 4, 16, 16};
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_step[k] = flag;
    ancilla_item_to_link[k] = 0;
    ancilla_timer[k] = 3;
    int j = link_direction_facing >> 1;
    ancilla_dir[k] = j;
    if (!flag) {
      Ancilla_SetXY(k, link_x_coord, link_y_coord + 20);
    } else {
      Ancilla_SetXY(k, link_x_coord + kAddDashingDust_X[j], link_y_coord + kAddDashingDust_Y[j]);
    }
  }
}

// Initializes shared state for both the flute travel bird (Ancilla27_Duck) and the
// cutscene duck (Ancilla38_CutsceneDuck) after slot allocation.
// Sets: y_vel = 0 (enters from left; no vertical drift initially), item_to_link = 0
// (animation frame counter), aux_timer = 1, x_vel = 56 (rightward approach speed),
// arr3 = 3 (number of body-part tiles to draw), K = 0, G = 0 (misc counters).
// Positions the bird at the left screen edge (BG2HOFS - 16 - xt) and Link's Y - 8,
// where xt is 0x40 in extended-screen mode to appear beyond the wider visible area.
void AddBirdCommon(int k) {
  ancilla_y_vel[k] = 0;
  ancilla_item_to_link[k] = 0;
  ancilla_aux_timer[k] = 1;
  ancilla_x_vel[k] = 56;
  ancilla_arr3[k] = 3;
  ancilla_K[k] = 0;
  ancilla_G[k] = 0;

  int xt = (enhanced_features0 & kFeatures0_ExtendScreen64) ? 0x40 : 0;
  Ancilla_SetXY(k, BG2HOFS_copy2 - 16 - xt, link_y_coord - 8);
}

// Computes a velocity vector pointing from world position (x, y) toward Link's position
// using the sprite speed-projection math, without permanently modifying any sprite slot.
// Borrows sprite slot 0 as a temporary anchor: saves x/y/z of slot 0, overwrites them
// with the ancilla's origin (x, y, z=0), calls Sprite_ProjectSpeedTowardsLink(0, vel)
// which uses slot 0's position as the "this sprite" origin, then restores slot 0.
// Parameters:
//   k   — ancilla slot index (unused directly; the caller's slot)
//   x,y — world position of the origin point (bomb center)
//   vel — speed magnitude to project
// Returns a ProjectSpeedRet with the decomposed X/Y velocity components.
ProjectSpeedRet Bomb_ProjectSpeedTowardsPlayer(int k, uint16 x, uint16 y, uint8 vel) {  // 84eb63
  uint16 old_x = Sprite_GetX(0), old_y = Sprite_GetY(0), old_z = sprite_z[0];
  Sprite_SetX(0, x);
  Sprite_SetY(0, y);
  sprite_z[0] = 0;
  ProjectSpeedRet pt = Sprite_ProjectSpeedTowardsLink(0, vel);
  sprite_z[0] = old_z;
  Sprite_SetX(0, old_x);
  Sprite_SetY(0, old_y);
  return pt;
}

// Applies a homing correction to the boomerang's return velocity when it strays too far.
// Checks the screen-relative offset between Link and the boomerang (centered at 0xF0).
// If the boomerang is more than 0x1E0 - 0xF0 = 0xF0 pixels (240 px) off in X or Y,
// overrides the corresponding velocity component with 0x90 (toward Link) or 0x70 (away)
// to nudge it back on course. This prevents the return path from infinitely drifting
// without the player seeing any "cheating" — hence the function's name.
void Boomerang_CheatWhenNoOnesLooking(int k, ProjectSpeedRet *pt) {  // 86809f
  uint16 x = link_x_coord - Ancilla_GetX(k) + 0xf0;
  uint16 y = link_y_coord - Ancilla_GetY(k) + 0xf0;
  if (x >= 0x1e0) {
    pt->x = sign16(x - 0x1e0) ? 0x90 : 0x70;
  } else if (y >= 0x1e0) {
    pt->y = sign16(y - 0x1e0) ? 0x90 : 0x70;
  }
}

// Applies the current medallion spell's damage to all eligible sprites.
// Iterates all 16 sprite slots; for each sprite in state >= 9 (active/carried/stunned)
// that is neither ignore_projectile nor paused, calls Ancilla_CheckDamageToSprite_aggressive.
// tmp_counter is set to ancilla_type[k] so the damage lookup uses the correct ancilla type.
// Used by Ether (type 0x18), Bombos (0x19), and Quake (0x1C) spells.
void Medallion_CheckSpriteDamage(int k) {  // 86ec5c
  tmp_counter = ancilla_type[k];
  for (int j = 15; j >= 0; j--) {
    if (sprite_state[j] >= 9 && !(sprite_ignore_projectile[j] | sprite_pause[j])) {
      Ancilla_CheckDamageToSprite_aggressive(j, tmp_counter);
    }
  }
}

// Checks whether ancilla type deals damage to sprite slot k, respecting the sprite's
// invincibility window. If sprite_hit_timer[k] >= 0x80 (high bit set = immune), skip.
// Otherwise delegates to Ancilla_CheckDamageToSprite_aggressive.
void Ancilla_CheckDamageToSprite(int k, uint8 type) {  // 86ecb7
  if (!sign8(sprite_hit_timer[k]))
    Ancilla_CheckDamageToSprite_aggressive(k, type);
}

// Applies ancilla damage to sprite slot k unconditionally (no immunity check).
// kAncilla_Damage[type] maps each ancilla type (0-56) to a damage class:
//   0 = no damage, 1 = arrow/minor, 6 = fire rod (upgrades to 9 with silver arrows
//   against type 0xD7 Agahnim), 7 = byrna, 8 = bomb, 11 = fire/ice rod burst,
//   12 = sword beam, 13-15 = Ether/Bombos/Quake medallion damage tiers.
// Silver Arrow upgrade: if ancilla type is 6 (arrow) and link_item_bow >= 3, deals 9
// and sets sprite_delay_aux4[k] = 32 against Agahnim (stun for special death sequence).
// Delegates final hit registration to Ancilla_CheckDamageToSprite_preset.
void Ancilla_CheckDamageToSprite_aggressive(int k, uint8 type) {  // 86ecbd
  static const uint8 kAncilla_Damage[57] = {
    6, 1, 11, 0, 0, 0, 0, 8,  0,  6, 0, 12,  1, 0, 0,  0,
    0, 1,  0, 0, 0, 0, 0, 0, 14, 13, 0,  0, 15, 0, 0,  7,
    1, 1,  1, 1, 1, 1, 1, 1,  1,  1, 1,  1,  1, 1, 1, 11,
    0, 1,  1, 1, 1, 1, 1, 1,  1,
  };
  uint8 dmg = kAncilla_Damage[type];
  if (dmg == 6 && link_item_bow >= 3) {
    if (sprite_type[k] == 0xd7)
      sprite_delay_aux4[k] = 32;
    dmg = 9;
  }
  Ancilla_CheckDamageToSprite_preset(k, dmg);
}

// Summons the travel duck (Cucco) when Link plays the flute indoors.
// Plays the duck-whistle sound effect (SFX 0x13) and spawns the duck ancilla
// (type 0x27) in slot 4. Used for dungeon flute warp sequences.
void CallForDuckIndoors() {  // 87a45f
  Ancilla_Sfx2_Near(0x13);
  AncillaAdd_Duck_take_off(0x27, 4);
}

// Plays ambient SFX v (sound_effect_ambient channel) with stereo panning calculated
// from ancilla slot k's screen X position via Ancilla_CalculateSfxPan.
void Ancilla_Sfx1_Pan(int k, uint8 v) {  // 888020
  byte_7E0CF8 = v;
  sound_effect_ambient = v | Ancilla_CalculateSfxPan(k);
}

// Plays SFX v on the primary sound channel (sound_effect_1) with stereo panning
// from ancilla slot k's screen X position. Used for most projectile hit sounds.
void Ancilla_Sfx2_Pan(int k, uint8 v) {  // 888027
  byte_7E0CF8 = v;
  sound_effect_1 = v | Ancilla_CalculateSfxPan(k);
}

// Plays SFX v on the secondary sound channel (sound_effect_2) with stereo panning
// from ancilla slot k's screen X position. Used for layered effects (e.g., bomb + explosion).
void Ancilla_Sfx3_Pan(int k, uint8 v) {  // 88802e
  byte_7E0CF8 = v;
  sound_effect_2 = v | Ancilla_CalculateSfxPan(k);
}

// Spawns a fire-rod shot or sword beam ancilla at Link's current position and facing.
// Parameters:
//   type — ancilla type: 1 = SwordBeam, 2 = FireRodShot. (y is unused; forced to 1.)
// If no slot is available and type != 1 (fire rod), refunds one unit of magic.
// Plays fire-rod SFX (0x0E panned) for type 2; sword beams are silent here.
// Sets x/y velocity from kFireRod_Xvel/Yvel[facing] (fire rod: fixed ±64 speed)
// or kFireRod_Xvel2/Yvel2[facing + sword_level*4] (sword beam: level-scaled speed).
// If the initial tile check (Ancilla_CheckInitialTile_A) fails (wall in front):
//   type 1: immediately becomes a BeamHit (type 4) at the wall.
//   type 2: enters hit-flash mode (step=1, timer=31, numspr=8) and pans the wall-hit SFX.
// kFireRod_X/Y: per-direction spawn offsets to position the shot at the rod tip.
void AncillaAdd_FireRodShot(uint8 type, uint8 y) {  // 8880b3
  static const int8 kFireRod_X[4] = {0, 0, -8, 16};
  static const int8 kFireRod_Y[4] = {-8, 16, 3, 3};
  static const int8 kFireRod_Xvel[4] = {0, 0, -64, 64};
  static const int8 kFireRod_Yvel[4] = {-64, 64, 0, 0};

  y = 1;
  int j= Ancilla_AllocInit(type, 1);
  if (j < 0) {
    if (type != 1)
      Refund_Magic(0);
    return;
  }

  if (type != 1)
    Ancilla_Sfx2_Near(0xe);

  ancilla_type[j] = type;
  ancilla_numspr[j] = kAncilla_Pflags[type];
  ancilla_timer[j] = 3;
  ancilla_step[j] = 0;
  ancilla_item_to_link[j] = 0;
  ancilla_objprio[j] = 0;
  ancilla_U[j] = 0;
  int i = link_direction_facing >> 1;
  ancilla_dir[j] = i;

  if (Ancilla_CheckInitialTile_A(j) < 0) {
    Ancilla_SetXY(j, link_x_coord + kFireRod_X[i], link_y_coord + kFireRod_Y[i]);
    if (type != 1) {
      ancilla_x_vel[j] = kFireRod_Xvel[i];
      ancilla_y_vel[j] = kFireRod_Yvel[i];
    } else {
      i += (link_sword_type - 2) * 4;
      ancilla_x_vel[j] = kFireRod_Xvel2[i];
      ancilla_y_vel[j] = kFireRod_Yvel2[i];
    }
    ancilla_floor[j] = link_is_on_lower_level;
    ancilla_floor2[j] = link_is_on_lower_level_mirror;
  } else {
    if (type == 1) {
      ancilla_type[j] = 4;
      ancilla_timer[j] = 7;
      ancilla_numspr[j] = 16;
    } else {
      ancilla_step[j] = 1;
      ancilla_timer[j] = 31;
      ancilla_numspr[j] = 8;
      j = link_direction_facing >> 1; // wtf
      Ancilla_Sfx2_Pan(j, 0x2a);
    }
  }
}

// Spawns 4 Somaria Bullet ancillae (type 0x01) outward from the Somaria Block at slot k.
// Called when the block fissions (player activates it a second time).
// kSpawnCentrifugalQuad_X/Y: per-direction offsets positioning each bullet at the block's
// edge in its launch direction, so bullets appear to fly out from the four sides.
// Each bullet gets: type=1, step=4 (fission-spawned mode), dir=i (0=up,1=down,2=left,3=right),
// velocity from kFireRod_Xvel2/Yvel2[i] (same speed table as sword beams),
// floor copied from the block, and is immediately culled if already off-screen.
// After all 4 bullets are spawned, sets tmp_counter = 0xFF to signal completion.
void SomariaBlock_SpawnBullets(int k) {  // 8881a7
  static const int8 kSpawnCentrifugalQuad_X[4] = {-8, -8, -9, -4};
  static const int8 kSpawnCentrifugalQuad_Y[4] = {-15, -4, -8, -8};

  uint8 z = (ancilla_z[k] == 0xff) ? 0 : ancilla_z[k];
  uint16 x = Ancilla_GetX(k);
  uint16 y = Ancilla_GetY(k) - z;

  for (int i = 3; i >= 0; i--) {
    int j = Ancilla_AllocInit(1, 4);
    if (j >= 0) {
      ancilla_type[j] = 0x1;
      ancilla_numspr[j] = kAncilla_Pflags[0x1];
      ancilla_step[j] = 4;
      ancilla_item_to_link[j] = 0;
      ancilla_objprio[j] = 0;
      ancilla_dir[j] = i;
      Ancilla_SetXY(j, x + kSpawnCentrifugalQuad_X[i], y + kSpawnCentrifugalQuad_Y[i]);
      Ancilla_TerminateIfOffscreen(j);
      ancilla_x_vel[j] = kFireRod_Xvel2[i];
      ancilla_y_vel[j] = kFireRod_Yvel2[i];
      ancilla_floor[j] = ancilla_floor[k];
      ancilla_floor2[j] = link_is_on_lower_level_mirror;
    }
  }
  tmp_counter = 0xff;
}

// Master ancilla per-frame entry point called from the main game loop.
// First processes any pending weapon-tink visual (sword/shield clank spark via
// Ancilla_WeaponTink), then runs all active ancilla slots via Ancilla_ExecuteAll.
void Ancilla_Main() {  // 888242
  Ancilla_WeaponTink();
  Ancilla_ExecuteAll();
}

// Computes a velocity vector pointing from Link's current position toward world point (x, y),
// as seen by sprite slot k's speed-projection math. Used for bomb blast recoil: the ancilla
// center is the "target" and Link's position is the "source," giving a velocity that points
// away from the blast. Temporarily overwrites link_x/y_coord so Sprite_ProjectSpeedTowardsLink
// uses (x, y) as the destination, then restores Link's actual position.
ProjectSpeedRet Ancilla_ProjectReflexiveSpeedOntoSprite(int k, uint16 x, uint16 y, uint8 vel) {  // 88824d
  uint16 old_x = link_x_coord, old_y = link_y_coord;
  link_x_coord = x;
  link_y_coord = y;
  ProjectSpeedRet pt = Sprite_ProjectSpeedTowardsLink(k, vel);
  link_x_coord = old_x;
  link_y_coord = old_y;
  return pt;
}

// Applies bomb explosion damage to sprites within the blast radius.
// Tests all 16 sprite slots, skipping those that: are frame-throttled ((j ^ frame_counter) & 3),
// are in their invincibility window (hit_timer set), are ignore_projectile, differ in floor,
// or are inactive (state < 9). Also skips type 0x92 (Agahnim bat) if C[j] >= 3.
// The blast hitbox is a 48×48 centered 24 pixels up (z-adjusted) from the bomb center.
// For each sprite that overlaps: applies ancilla_type[k] damage via Ancilla_CheckDamageToSprite,
// and computes a centrifugal recoil velocity from the bomb center outward via
// Ancilla_ProjectReflexiveSpeedOntoSprite (negated, so sprites fly away from the blast).
void Bomb_CheckSpriteDamage(int k) {  // 888287
  for (int j = 15; j >= 0; j--) {
    if ((j ^ frame_counter) & 3 | sprite_hit_timer[j] | sprite_ignore_projectile[j])
      continue;
    if (sprite_floor[j] != ancilla_floor[k] || sprite_state[j] < 9)
      continue;
    SpriteHitBox hb;
    int ax = Ancilla_GetX(k) - 24;
    int ay = Ancilla_GetY(k) - 24 - ancilla_z[k];
    hb.r0_xlo = ax;
    hb.r8_xhi = ax >> 8;
    hb.r1_ylo = ay;
    hb.r9_yhi = ay >> 8;
    hb.r2 = 48;
    hb.r3 = 48;
    Sprite_SetupHitBox(j, &hb);
    if (!CheckIfHitBoxesOverlap(&hb))
      continue;
    if (sprite_type[j] == 0x92 && sprite_C[j] >= 3)
      continue;
    Ancilla_CheckDamageToSprite(j, ancilla_type[k]);
    ProjectSpeedRet pt = Ancilla_ProjectReflexiveSpeedOntoSprite(j, Ancilla_GetX(k), Ancilla_GetY(k), 64);
    sprite_x_recoil[j] = -pt.x;
    sprite_y_recoil[j] = -pt.y;
  }
}

// Iterates all 10 ancilla slots (9 down to 0) and calls Ancilla_ExecuteOne for each
// active slot (type != 0). Sets cur_object_index before each call so any subroutine
// that needs the current ancilla index can read it without being passed k explicitly.
void Ancilla_ExecuteAll() {  // 88832b
  for (int i = 9; i >= 0; i--) {
    cur_object_index = i;
    if (ancilla_type[i])
      Ancilla_ExecuteOne(ancilla_type[i], i);
  }
}

// Per-frame execution driver for a single ancilla slot k of the given type.
// For slots 0-5 (the important/visible ancillae): allocates OAM from the appropriate
// region (A = unsorted / D = lower-floor sorted / F = upper-floor sorted) based on
// ancilla_numspr[k] bytes needed. Slots 6-9 skip OAM allocation.
// Decrements ancilla_timer[k] each frame when the game is unpaused (submodule_index == 0),
// providing a general-purpose countdown for handlers that use it for animation pacing.
// Dispatches to kAncilla_Funcs[type - 1](k) for type-specific behavior.
void Ancilla_ExecuteOne(uint8 type, int k) {  // 88833c
  if (k < 6) {
    ancilla_oam_idx[k] = Ancilla_AllocateOamFromRegion_A_or_D_or_F(k, ancilla_numspr[k]);
  }

  if (submodule_index == 0 && ancilla_timer[k] != 0)
    ancilla_timer[k]--;

  kAncilla_Funcs[type - 1](k);
}

// Ancilla type 0x13 — ice rod shot sparkle trail particle.
// Draws 4 OAM tiles per frame, selected by (ancilla_timer[k] & 0x1C) as a group offset
// into kIceShotSparkle_X/Y/Char tables (4 entries per phase, 4 phases = 16 entries).
// Entries with X == 0xFF (or Y == 0xFF) signal the tile should be hidden (placed off-screen).
// Uses sort-sprites OAM region E (upper floor) or D (lower floor) if sort is enabled;
// upgrades priority to 0x30 if an active IceRodShot (type 0x0B) has objprio set.
// Called by AncillaAdd_IceRodSparkle to display the trailing sparkles behind a flying ice shot.
void Ancilla13_IceRodSparkle(int k) {  // 888435
  static const uint8 kIceShotSparkle_X[16] = {2, 7, 6, 1, 1, 7, 7, 1, 0, 7, 8, 1, 4, 9, 4, 0xff};
  static const uint8 kIceShotSparkle_Y[16] = {2, 3, 8, 7, 1, 1, 7, 7, 1, 0, 7, 8, 0xff, 4, 9, 4};
  static const uint8 kIceShotSparkle_Char[16] = {0x83, 0x83, 0x83, 0x83, 0xb6, 0x80, 0xb6, 0x80, 0xb7, 0xb6, 0xb7, 0xb6, 0xb7, 0xb6, 0xb7, 0xb6};

  if (!ancilla_timer[k])
    ancilla_type[k] = 0;
  if (!submodule_index) {
    Ancilla_MoveX(k);
    Ancilla_MoveY(k);
  }
  AncillaOamInfo info;
  if (Ancilla_ReturnIfOutsideBounds(k, &info))
    return;

  int j;
  for (j = 4; j >= 0 && ancilla_type[j] != 0xb; j--) {}
  if (j >= 0 && ancilla_objprio[j])
    info.flags = 0x30;

  if (sort_sprites_setting) {
    if (ancilla_floor[k])
      Oam_AllocateFromRegionE(0x10);
    else
      Oam_AllocateFromRegionD(0x10);
  } else {
    Oam_AllocateFromRegionA(0x10);
  }

  OamEnt *oam = GetOamCurPtr();
  j = ancilla_timer[k] & 0x1c;
  for (int i = 3; i >= 0; i--, oam++)
    SetOamPlain(oam, info.x + kIceShotSparkle_X[i + j], info.y + kIceShotSparkle_Y[i + j], kIceShotSparkle_Char[i + j], info.flags | 4, 0);
}

// Spawns a new IceRodSparkle (type 0x13) trailing behind the flying ice shot at slot k.
// Rate-limited: only spawns when ancilla_arr4[k] wraps through 0 (every 5 frames when active).
// Skips during transitions (submodule_index set). The sparkle inherits the shot's position
// and floor, and gets a small outward velocity (kIceShotSparkle_Xvel/Yvel) in the shot's
// direction so it drifts away from the shot path. Sets numspr = 0 to disable OAM allocation
// from the standard region (Ancilla13_IceRodSparkle manages its own region allocation).
void AncillaAdd_IceRodSparkle(int k) {  // 8884c8
  static const int8 kIceShotSparkle_Xvel[4] = {0, 0, -4, 4};
  static const int8 kIceShotSparkle_Yvel[4] = {-4, 4, 0, 0};

  if (submodule_index || !sign8(--ancilla_arr4[k]))
    return;

  ancilla_arr4[k] = 5;
  int j = Ancilla_AllocHigh();
  if (j >= 0) {
    ancilla_type[j] = 0x13;
    ancilla_timer[j] = 15;

    int i = ancilla_dir[k];
    ancilla_x_vel[j] = kIceShotSparkle_Xvel[i];
    ancilla_y_vel[j] = kIceShotSparkle_Yvel[i];

    ancilla_x_lo[j] = ancilla_x_lo[k];
    ancilla_y_lo[j] = ancilla_y_lo[k];
    ancilla_floor[j] = ancilla_floor[k];
    ancilla_numspr[j] = 0;
  }

}

// Ancilla type 0x01 — Somaria Cane bullet (plasma ball).
// kSomarianBlast_Mask: per-step frame-skip masks (7=move every 8 frames, 0=every frame).
// Movement and step-advance only when game is unpaused (submodule_index == 0):
//   - Moves by x/y velocity on frames where (frame_counter & mask) == 0.
//   - Advances ancilla_step every 3 frames, cycling through steps 0-5 (caps at 4/5 loop).
//   - Checks sprite and tile collision; on any hit, converts to BeamHit (type 4, timer 7).
// Draws via SomarianBlast_Draw each frame.
void Ancilla01_SomariaBullet(int k) {  // 88851b
  static const uint8 kSomarianBlast_Mask[6] = {7, 3, 1, 0, 0, 0};

  if (!submodule_index) {
    if (!(frame_counter & kSomarianBlast_Mask[ancilla_step[k]])) {
      Ancilla_MoveX(k);
      Ancilla_MoveY(k);
    }
    if (ancilla_timer[k] == 0) {
      ancilla_timer[k] = 3;
      uint8 a = ancilla_step[k] + 1;
      if (a >= 6)
        a = 4;
      ancilla_step[k] = a;
    }
    if (Ancilla_CheckSpriteCollision(k) >= 0 || Ancilla_CheckTileCollision_staggered(k)) {
      ancilla_type[k] = 4;
      ancilla_timer[k] = 7;
      ancilla_numspr[k] = 16;
    }
  }
  SomarianBlast_Draw(k);
}

// Converts the ancilla's world position to screen-relative coordinates and tests bounds.
// Sets info->flags to the floor-dependent OAM priority bits (0x20 = upper floor, 0x10 = lower).
// Computes screen-relative x = ancilla_x_lo - BG2HOFS, y = ancilla_y_lo - BG2VOFS.
// If x >= 0xF4 or y >= 0xF0 (off-screen), deactivates the ancilla (type = 0) and returns true.
// Otherwise stores (x, y) in info->x/y and returns false (safe to draw).
// Callers use this as an early-return guard before writing OAM entries.
bool Ancilla_ReturnIfOutsideBounds(int k, AncillaOamInfo *info) {  // 88862a
  static const uint8 kAncilla_FloorFlags[2] = {0x20, 0x10};
  info->flags = kAncilla_FloorFlags[ancilla_floor[k]];
  if ((info->x = ancilla_x_lo[k] - BG2HOFS_copy2) >= 0xf4 ||
      (info->y = ancilla_y_lo[k] - BG2VOFS_copy2) >= 0xf0) {
    ancilla_type[k] = 0;
    return true;
  }
  return false;
}

// Draws 2 OAM tiles for the Somaria bullet (plasma ball) at its current step.
// The animation frame index j = ancilla_dir[k] * 6 + ancilla_step[k] selects a column
// from the 24-entry per-direction animation tables (4 directions × 6 steps).
// kSomarianBlast_Draw_X0/X1, Y0/Y1: pixel offsets for the two tiles.
//   Y values with the high bit set (0x80) mean "hide tile" (mapped to 0xF0).
// kSomarianBlast_Draw_Char0/Char1: sprite-sheet chars (offset from 0x82).
// kSomarianBlast_Draw_Flags0/Flags1: H/V flip bits merged into the base flags.
// Applies objprio correction (sets 0x30 priority bits) if the slot has priority set.
// kSomarianBlast_Flags[item_to_link]: selects alt palette (palette 2 vs 6) for the bullet.
void SomarianBlast_Draw(int k) {  // 888650
  static const uint8 kSomarianBlast_Flags[2] = {2, 6};
  AncillaOamInfo info;
  if (Ancilla_ReturnIfOutsideBounds(k, &info))
    return;
  info.flags |= kSomarianBlast_Flags[ancilla_item_to_link[k]];
  if (ancilla_objprio[k])
    info.flags |= 0x30;
  static const int8 kSomarianBlast_Draw_X0[24] = {
    0, 0, 0, 0, 4, 4, 0, 0, 0, 0, 4, 4, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
  };
  static const int8 kSomarianBlast_Draw_X1[24] = {
    8, 8, 8, 8, 4, 4, 8, 8, 8, 8, 4, 4, 0, 0, 0, 0,
    8, 8, 0, 0, 0, 0, 8, 8,
  };
  static const uint8 kSomarianBlast_Draw_Y0[24] = {
    0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    4, 4, 0, 0, 0, 0, 4, 4,
  };
  static const uint8 kSomarianBlast_Draw_Y1[24] = {
    0, 0,    0, 0, 8, 8, 0x80, 0, 0, 0, 8, 8, 0x80, 8, 8, 8,
    4, 4, 0x80, 8, 8, 8,    4, 4,
  };
  static const uint8 kSomarianBlast_Draw_Flags0[24] = {
    0xc0, 0xc0, 0xc0, 0xc0, 0x80, 0xc0, 0x40, 0x40, 0x40, 0x40, 0, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0xc0,    0,    0,    0,    0,    0, 0x80,
  };
  static const uint8 kSomarianBlast_Draw_Flags1[24] = {
    0x80, 0x80, 0x80, 0x80, 0x80, 0xc0, 0,    0, 0, 0, 0, 0x40, 0xc0, 0xc0, 0xc0, 0xc0,
    0x40, 0xc0, 0x80, 0x80, 0x80, 0x80, 0, 0x80,
  };
  static const uint8 kSomarianBlast_Draw_Char0[24] = {
    0x50, 0x50, 0x44, 0x44, 0x52, 0x52, 0x50, 0x50, 0x44, 0x44, 0x51, 0x51, 0x43, 0x43, 0x42, 0x42,
    0x41, 0x41, 0x43, 0x43, 0x42, 0x42, 0x40, 0x40,
  };
  static const uint8 kSomarianBlast_Draw_Char1[24] = {
    0x50, 0x50, 0x44, 0x44, 0x51, 0x51, 0x50, 0x50, 0x44, 0x44, 0x52, 0x52, 0x43, 0x43, 0x42, 0x42,
    0x40, 0x40, 0x43, 0x43, 0x42, 0x42, 0x41, 0x41,
  };
  OamEnt *oam = GetOamCurPtr();
  int j = ancilla_dir[k] * 6 + ancilla_step[k];
  SetOamPlain(oam + 0, info.x + kSomarianBlast_Draw_X0[j], sign8(kSomarianBlast_Draw_Y0[j]) ? 0xf0 : info.y + kSomarianBlast_Draw_Y0[j],
              0x82 + kSomarianBlast_Draw_Char0[j], info.flags | kSomarianBlast_Draw_Flags0[j], 0);
  SetOamPlain(oam + 1, info.x + kSomarianBlast_Draw_X1[j], sign8(kSomarianBlast_Draw_Y1[j]) ? 0xf0 : info.y + kSomarianBlast_Draw_Y1[j],
              0x82 + kSomarianBlast_Draw_Char1[j], info.flags | kSomarianBlast_Draw_Flags1[j], 0);
}

// Ancilla type 0x02 — Fire Rod shot (fireball projectile).
// Two phases controlled by ancilla_step[k]:
//
// step == 0 (flying): moves via Ancilla_MoveX/Y each frame when unpaused.
//   Checks sprite collision first; on hit, advances to step 1 (hit flash, timer 31).
//   Performs two-pass tile collision: primary check (dir | 8), secondary (dir | 12).
//   If the hit tile is a torch (attr & 0xF0 == 0xC0): calls Dungeon_LightTorch.
//   If in the Skull Woods overworld area and hit tile is 0x43 (pit): becomes a Skull Fire.
//   Draws via FireShot_Draw for the spinning fireball trail animation.
//
// step != 0 (hit flash): calls Ancilla_CheckBasicSpriteCollision each frame (for trailing damage).
//   Deactivates when timer reaches 0 (with Skull Woods fire special case above).
//   Draws the 3-frame impact flash: j = timer >> 3 selects one of kFireShot_Draw_Char[j-1]
//   (large/med/small fireball tile), or the final two-tile flicker at timer < 8.
void Ancilla02_FireRodShot(int k) {  // 8886d2
  if (ancilla_step[k] == 0) {
    if (!submodule_index) {
      ancilla_L[k] = 0;
      Ancilla_MoveX(k);
      Ancilla_MoveY(k);
      uint8 coll = Ancilla_CheckSpriteCollision(k) >= 0;
      if (!coll) {
        ancilla_dir[k] |= 8;
        coll = Ancilla_CheckTileCollision(k);
        ancilla_L[k] = ancilla_tile_attr[k];
        if (!coll) {
          ancilla_dir[k] |= 12;
          uint8 bak = ancilla_U[k];
          coll = Ancilla_CheckTileCollision(k);
          ancilla_U[k] = bak;
        }
      }
      if (coll) {
        ancilla_step[k]++;
        ancilla_timer[k] = 31;
        ancilla_numspr[k] = 8;
        Ancilla_Sfx2_Pan(k, 0x2a);
      }
      ancilla_item_to_link[k]++;
      ancilla_dir[k] &= ~0xC;
      if (((byte_7E0333 = ancilla_L[k]) & 0xf0) == 0xc0 || ((byte_7E0333 = ancilla_tile_attr[k]) & 0xf0) == 0xc0)
        Dungeon_LightTorch();
    }
    FireShot_Draw(k);
  } else {
    AncillaOamInfo info;
    Ancilla_CheckBasicSpriteCollision(k);
    if (Ancilla_ReturnIfOutsideBounds(k, &info))
      return;
    OamEnt *oam = GetOamCurPtr();
    if (!ancilla_timer[k]) {
      uint8 old_type = ancilla_type[k];
      ancilla_type[k] = 0;
      if (old_type != 0x2f && BYTE(overworld_screen_index) == 64 && ancilla_tile_attr[k] == 0x43)
        FireRodShot_BecomeSkullWoodsFire(k);
      return;
    }
    int j = ancilla_timer[k] >> 3;
    if (j != 0) {
      static const uint8 kFireShot_Draw_Char[3] = {0xa2, 0xa0, 0x8e};
      SetOamPlain(oam, info.x, info.y, kFireShot_Draw_Char[j - 1], info.flags | 2, 2);
    } else {
      SetOamPlain(oam + 0, info.x + 0, info.y - 3, 0xa4, info.flags | 2, 0);
      SetOamPlain(oam + 1, info.x + 8, info.y - 3, 0xa5, info.flags | 2, 0);
    }
  }
}

// Draws the 3-tile spinning fireball animation for a flying fire rod shot.
// j = ancilla_item_to_link[k] & 0xC selects one of 4 animation phases (0, 4, 8, 12),
// each phase using 3 entries from kFireShot_Draw_X2/Y2 to position the 3 spark tiles.
// kFireShot_Draw_Char2[i] gives the char for each of the 3 tiles: 0x8D, 0x9D, 0x9C
// (descending from large flame to small trailing spark).
// Applies objprio priority flag if the slot has override priority set.
void FireShot_Draw(int k) {  // 88877c
  static const uint8 kFireShot_Draw_X2[16] = {7, 0, 8, 0, 8, 4, 0, 0, 2, 8, 0, 0, 1, 4, 9, 0};
  static const uint8 kFireShot_Draw_Y2[16] = {1, 4, 9, 0, 7, 0, 8, 0, 8, 4, 0, 0, 2, 8, 0, 0};
  static const uint8 kFireShot_Draw_Char2[3] = {0x8d, 0x9d, 0x9c};
  AncillaOamInfo info;
  if (Ancilla_ReturnIfOutsideBounds(k, &info))
    return;
  if (ancilla_objprio[k])
    info.flags |= 0x30;

  OamEnt *oam = GetOamCurPtr();
  int j = ancilla_item_to_link[k] & 0xc;
  for (int i = 2; i >= 0; i--, oam++)
    SetOamPlain(oam, info.x + kFireShot_Draw_X2[j + i], info.y + kFireShot_Draw_Y2[j + i], kFireShot_Draw_Char2[i], info.flags | 2, 0);
}

// Throttled wrapper around Ancilla_CheckTileCollision that only executes on
// alternating frames per slot (frame_counter XOR slot index, odd bit). This
// halves the tile-collision CPU cost for ancillae that don't need per-frame
// precision (e.g. bombs sitting on the ground).
uint8 Ancilla_CheckTileCollision_staggered(int k) {  // 88897b
  if ((frame_counter ^ k) & 1)
    return Ancilla_CheckTileCollision(k);
  return 0;
}

// Performs tile collision for ancilla k, handling multi-floor dungeons by
// probing both the upper and lower floor planes. Returns a bitmask: bit 0 =
// lower-floor collision, bit 1 = upper-floor collision. In single-floor rooms
// (dung_hdr_collision == 0) delegates directly to the one-floor variant.
// Outdoor ancillae with objprio set (flying / above-ground) are exempt and
// return 0 immediately so they pass over solid tiles. When dung_hdr_collision
// is 1 or 2, the BG1/BG2 scroll offset difference is used to translate the
// ancilla position to the upper floor's coordinate space before probing.
uint8 Ancilla_CheckTileCollision(int k) {  // 888981
  if (!player_is_indoors && ancilla_objprio[k]) {
    ancilla_tile_attr[k] = 0;
    return 0;
  }
  if (!dung_hdr_collision)
    return Ancilla_CheckTileCollisionOneFloor(k);
  uint16 x = 0, y = 0;
  if (dung_hdr_collision < 3) {
    // Compute the pixel offset between BG1 (upper floor) and BG2 (lower floor)
    // scroll positions to translate the ancilla into the upper floor space.
    x = BG1HOFS_copy2 - BG2HOFS_copy2;
    y = BG1VOFS_copy2 - BG2VOFS_copy2;
  }
  uint16 oldx = Ancilla_GetX(k), oldy = Ancilla_GetY(k);
  // Temporarily reposition to upper-floor coordinates and check that plane.
  Ancilla_SetX(k, oldx + x);
  Ancilla_SetY(k, oldy + y);
  ancilla_floor[k] = 1;
  uint8 b = Ancilla_CheckTileCollisionOneFloor(k);
  // Restore original position and check lower floor.
  ancilla_floor[k] = 0;
  Ancilla_SetX(k, oldx);
  Ancilla_SetY(k, oldy);
  return (b << 1) | (uint8)Ancilla_CheckTileCollisionOneFloor(k);
}

// Probes the tile at the ancilla's leading edge on a single floor plane.
// kAncilla_CheckTileColl0_X/Y provide per-direction pixel offsets that place
// the probe point at the leading tip of the ancilla's bounding box (20 entries
// to cover the 4 cardinal directions plus 8 diagonal directions and 8 spare).
// Delegates the actual attribute lookup to Ancilla_CheckTileCollision_targeted.
bool Ancilla_CheckTileCollisionOneFloor(int k) {  // 888a03
  static const int8 kAncilla_CheckTileColl0_X[20] = {
    8, 8, 0, 16, 4, 4, 0, 16, 4, 4, 4, 12, 12, 12, 4, 12, 0, 0, 0, 0,
  };
  static const int8 kAncilla_CheckTileColl0_Y[20] = {
    0, 16, 5, 5, 0, 16, 4, 4, 4, 12, 5, 5, 4, 12, 12, 12, 0, 0, 0, 0,
  };
  uint16 x = Ancilla_GetX(k) + kAncilla_CheckTileColl0_X[ancilla_dir[k]];
  uint16 y = Ancilla_GetY(k) + kAncilla_CheckTileColl0_Y[ancilla_dir[k]];
  return Ancilla_CheckTileCollision_targeted(k, x, y);
}

// Core tile-attribute lookup and response for ancilla k at world coordinates
// (x, y). Returns true if the tile is solid (blocks the ancilla). Also writes
// the raw tile attribute into ancilla_tile_attr[k] for callers that need it.
//
// Out-of-viewport positions (outside 256x224 relative to BG2 scroll) return 0
// so projectiles that scroll off-screen are not prematurely stopped.
//
// kAncilla_TileColl0_Attrs classifies the raw attribute byte into:
//   0 = passable, 1 = solid, 2 = sloped, 3 = inter-floor passage, 4 = floor-
//   transition staircase.
//
// SomariaBullet (type 2) treats 0xC0-range tiles as passable — those are
// inter-floor transitions that the block itself should not be stopped by.
//
// For ancillae with objprio set (elevated/flying), the staircase countdown in
// ancilla_U[k] is decremented each frame; when it expires and the tile is a
// floor-transition (t==4), objprio is toggled to swap the visual layer,
// simulating the projectile crossing between dungeon floors.
bool Ancilla_CheckTileCollision_targeted(int k, uint16 x, uint16 y) {  // 888a26
  if ((uint16)(y - BG2VOFS_copy2) >= 224 || (uint16)(x - BG2HOFS_copy2) >= 256)
    return 0;
  uint8 tile_attr;
  if (!player_is_indoors) {
    // Overworld tile lookup expects x in tile units (divide by 8).
    x >>= 3;
    tile_attr = Overworld_GetTileAttributeAtLocation(x, y);
  } else {
    tile_attr = GetTileAttribute(ancilla_floor[k], &x, y);
  }

  ancilla_tile_attr[k] = tile_attr;
  // Tile attribute 3 is an inter-floor passage; ancilla_floor2 indicates the
  // ancilla is already on the upper floor, so the passage is passable.
  if (tile_attr == 3 && ancilla_floor2[k])
    return 0;

  uint8 t = kAncilla_TileColl0_Attrs[tile_attr];

  // SomariaBullet (type 2) ignores staircase-style 0xC0 tiles.
  if (ancilla_type[k] == 2 && (tile_attr & 0xf0) == 0xc0)
    t = 0;

  if (!ancilla_objprio[k]) {
    if (t == 0)
      return false;
    if (t == 1)
      goto return_true_set_alert;
    if (t == 2)
      return Entity_CheckSlopedTileCollision(x, y);
    // t == 3: floor passage. Block only if already on upper floor.
    if (t == 3) {
      if (ancilla_floor2[k])
        goto return_true_set_alert;
      return 0;
    }
  }
  // For elevated ancillae (objprio set), run the staircase countdown. When the
  // counter expires at a floor-transition tile, flip objprio to change layers.
  if (sign8(--ancilla_U[k])) {
    ancilla_U[k] = 0;
    if (t == 4) {
      ancilla_U[k] = 6;
      ancilla_objprio[k] ^= 1;
    }
  }
  return 0;

return_true_set_alert:
  sprite_alert_flag = 3;
  return 1;
}

// Multi-floor tile collision for Class-2 ancillae (hookshot-type projectiles
// that use a different probe-point table). Mirrors the dual-floor logic of
// Ancilla_CheckTileCollision but delegates to the Class-2 inner probe and
// returns a simple bool rather than a two-bit floor bitmask: any collision on
// either floor returns true.
bool Ancilla_CheckTileCollision_Class2(int k) {  // 888bcf
  if (!dung_hdr_collision)
    return Ancilla_CheckTileCollision_Class2_Inner(k);
  uint16 x = 0, y = 0;
  if (dung_hdr_collision < 3) {
    x = BG1HOFS_copy2 - BG2HOFS_copy2;
    y = BG1VOFS_copy2 - BG2VOFS_copy2;
  }
  uint16 oldx = Ancilla_GetX(k), oldy = Ancilla_GetY(k);
  Ancilla_SetX(k, oldx + x);
  Ancilla_SetY(k, oldy + y);
  ancilla_floor[k] = 1;
  bool b = Ancilla_CheckTileCollision_Class2_Inner(k);
  ancilla_floor[k] = 0;
  Ancilla_SetX(k, oldx);
  Ancilla_SetY(k, oldy);
  return (b | Ancilla_CheckTileCollision_Class2_Inner(k)) != 0;
}

// Single-floor tile collision probe for Class-2 ancillae (hookshot, etc.).
// Uses tighter leading-edge offsets: exactly 8 px ahead in the travel direction
// (kAncilla_CheckTileColl_Y/X) rather than the larger offsets of the standard
// probe, making Class-2 projectiles narrower and less prone to false stops.
//
// Uses kAncilla_TileColl_Attrs (the Class-2 table) instead of the standard
// kAncilla_TileColl0_Attrs, which assigns different solid/passable mappings.
//
// Floor-transition tiles (t==4): if the ancilla is on the lower floor,
// immediately promote it to upper floor (objprio = 1) without stopping; if
// already on the upper floor, treat the tile as solid (return true).
bool Ancilla_CheckTileCollision_Class2_Inner(int k) {  // 888c43
  static const int8 kAncilla_CheckTileColl_Y[4] = {-8, 8, 0, 0};
  static const int8 kAncilla_CheckTileColl_X[4] = {0, 0, -8, 8};

  uint16 x = Ancilla_GetX(k) + kAncilla_CheckTileColl_X[ancilla_dir[k]];
  uint16 y = Ancilla_GetY(k) + kAncilla_CheckTileColl_Y[ancilla_dir[k]];

  if ((uint16)(y - BG2VOFS_copy2) >= 224 || (uint16)(x - BG2HOFS_copy2) >= 256)
    return false;
  uint8 tile_attr;
  if (!player_is_indoors) {
    x >>= 3;
    tile_attr = Overworld_GetTileAttributeAtLocation(x, y);
  } else {
    tile_attr = GetTileAttribute(ancilla_floor[k], &x, y);
  }

  ancilla_tile_attr[k] = tile_attr;
  if (tile_attr == 3 && ancilla_floor2[k])
    return false;

  uint8 t = kAncilla_TileColl_Attrs[tile_attr];
  if (t == 0)
    return false;
  if (t == 2)
    return Entity_CheckSlopedTileCollision(x, y);
  if (t == 4) {
    // Floor-transition tile: lower-floor ancilla transitions upward; upper-
    // floor ancilla treats it as a ceiling and stops.
    if (ancilla_floor2[k])
      return true;
    ancilla_objprio[k] = 1;
    return false;
  }
  // t == 3: inter-floor passage — solid only when already on upper floor.
  if (t == 3)
    return ancilla_floor2[k] != 0;
  return true;
}

// Ancilla type 0x04: Sword-beam impact flash spawned when a beam strikes a wall
// or sprite. Plays a 4-frame 4-tile burst animation then self-destructs.
//
// ancilla_timer[k] counts down 8→0 from the moment of impact; j = timer>>1
// selects one of 4 animation stages (0-3). Each stage draws 4 corner sparks
// around the impact point, expanding outward (kBeamHit_X/Y offsets grow from
// ±12 at stage 0 down to ±4 at stage 2, then 0/8 at stage 3).
//
// The position correction (int8)(screen_x - base_screen_x) compensates for
// camera scroll that may have occurred between the impact frame and the draw
// frame, keeping sparks world-locked rather than screen-locked.
//
// char 0x53 = standard spark tile; 0x54 = smaller center spark (stage 3 only).
// kBeamHit_Flags encodes flip bits (0x40/0x80) for 4-way symmetric placement.
void Ancilla04_BeamHit(int k) {  // 888d19
  static const int8 kBeamHit_X[16] = {-12, 20, -12, 20, -8, 16, -8, 16, -4, 12, -4, 12, 0, 8, 0, 8};
  static const int8 kBeamHit_Y[16] = {-12, -12, 20, 20, -8, -8, 16, 16, -4, -4, 12, 12, 0, 0, 8, 8};
  static const uint8 kBeamHit_Char[16] = {0x53, 0x53, 0x53, 0x53, 0x53, 0x53, 0x53, 0x53, 0x53, 0x53, 0x53, 0x53, 0x54, 0x54, 0x54, 0x54};
  static const uint8 kBeamHit_Flags[16] = {0x40, 0, 0xc0, 0x80, 0x40, 0, 0xc0, 0x80, 0x40, 0, 0xc0, 0x80, 0, 0x40, 0x80, 0xc0};
  AncillaOamInfo info;
  if (Ancilla_ReturnIfOutsideBounds(k, &info))
    return;
  if (!ancilla_timer[k]) {
    ancilla_type[k] = 0;
    return;
  }
  OamEnt *oam = GetOamCurPtr();
  // j selects animation stage 0-3 (timer decrements 2 per stage).
  int j = ancilla_timer[k] >> 1;
  uint16 ancilla_x = Ancilla_GetX(k);
  uint16 ancilla_y = Ancilla_GetY(k);
  // r7/r6 capture the screen-space position at draw time for scroll correction.
  uint8 r7 = ancilla_x - BG2HOFS_copy2;
  uint8 r6 = ancilla_y - BG2VOFS_copy2;
  for (int i = 3; i >= 0; i--, oam++) {
    int m = j * 4 + i;
    uint8 x = info.x + kBeamHit_X[m];
    uint8 y = info.y + kBeamHit_Y[m];
    // Reproject screen coords back to world coords to account for any mid-burst
    // camera scroll, then re-subtract the current scroll for final OAM position.
    uint16 x_adj = (uint16)(ancilla_x + (int8)(x - r7) - BG2HOFS_copy2);
    uint16 y_adj = (uint16)(ancilla_y + (int8)(y - r6) - BG2VOFS_copy2 + 0x10);
    SetOamPlain(oam, x, (y_adj >= 0x100) ? 0xf0 : y,
                kBeamHit_Char[m] + 0x82, kBeamHit_Flags[m] | 2 | info.flags, (x_adj >= 0x100) ? 1 : 0);
  }
}

// Scans all 16 sprite slots for a collision with ancilla k. Returns the index
// of the first hit sprite, or -1 if none.
//
// Non-arrow/non-somaria ancillae are staggered: only one sprite per 4 frames
// (j XOR frame_counter, low 2 bits == 0) is checked each call, reducing CPU
// cost. Arrows (type 9) and somaria blast (type 0x1f) check every sprite every
// frame because they need immediate per-frame response.
//
// A sprite is eligible only if: (a) its state >= 9 (alive and active),
// (b) either the sprite has the deflect-capable flag (bit 1 of sprite_defl_bits)
// or the ancilla is not elevated (objprio == 0), and (c) the ancilla and sprite
// share the same floor level.
int Ancilla_CheckSpriteCollision(int k) {  // 888d68
  for (int j = 15; j >= 0; j--) {
    if (ancilla_type[k] == 9 || ancilla_type[k] == 0x1f || ((j ^ frame_counter) & 3 | sprite_pause[j]) == 0) {
      if ((sprite_state[j] >= 9 && (sprite_defl_bits[j] & 2 || !ancilla_objprio[k])) && ancilla_floor[k] == sprite_floor[j]) {
        if (Ancilla_CheckSpriteCollision_Single(k, j))
          return j;
      }
    }
  }
  return -1;
}

// Resolves collision between ancilla k and sprite j after bounding-box overlap
// is confirmed. Returns true if the ancilla should be consumed/stopped, false
// if it should pass through (e.g. deflected arrow or immune sprite).
//
// Arrow deflection: sprites with flag bit 3 set deflect normal arrows. Agahnim
// (type 0x1b) deflects only non-silver arrows (link_item_bow < 3); silver arrows
// pass through (return_value = false, so the arrow continues and deals damage).
//
// Direction-based deflection (sprite_defl_bits bit 4): compares the ancilla's
// travel direction against the canonical reversed direction for that axis
// (kAncilla_CheckSpriteColl_Dir maps 0↔2, 1↔3). If they match, the sprite is
// facing the incoming projectile, so the collision registers normally.
//
// Boomerang (type 5) and somaria blast (type 0x1f): skip damage but tag the
// sprite via sprite_B/sprite_unk2 so the sprite's own handler can respond.
// Somaria blast exempts sprite 0x8D (special case).
//
// Recoil vectors are axis-aligned and scaled to ±64 pixel/frame units based on
// ancilla travel direction. sprite_type 0x92 (special boss) requires sprite_C
// to be ≥ 3 before allowing damage through (phase guard).
bool Ancilla_CheckSpriteCollision_Single(int k, int j) {  // 888dae
  int i;
  SpriteHitBox hb;
  Ancilla_SetupHitBox(k, &hb);

  Sprite_SetupHitBox(j, &hb);
  if (!CheckIfHitBoxesOverlap(&hb))
    return false;

  bool return_value = true;
  if (sprite_flags[j] & 8 && ancilla_type[k] == 9) {
    if (sprite_type[j] != 0x1b) {
      Sprite_CreateDeflectedArrow(k);
      return false;
    }
    // Agahnim: silver arrows (bow >= 3) pass through; normal arrows deflect.
    if (link_item_bow < 3) {
      Sprite_CreateDeflectedArrow(k);
    } else {
      return_value = false;
    }
  }

  if (sprite_defl_bits[j] & 0x10) {
    // Reverse-direction lookup: 0→2, 1→3, 2→0, 3→1.
    static const uint8 kAncilla_CheckSpriteColl_Dir[4] = {2, 3, 0, 1};
    ancilla_dir[k] &= 3;
    if (ancilla_dir[k] == kAncilla_CheckSpriteColl_Dir[ancilla_dir[k]])
      goto return_true_set_alert;
  }

  if (ancilla_type[k] == 5 || ancilla_type[k] == 0x1f) {
    if (ancilla_type[k] == 0x1f && sprite_type[j] == 0x8d)
      goto skip;
    if (sprite_hit_timer[j])
      goto return_true_set_alert;
    if (sprite_defl_bits[j] & 2) {
skip:
      // Tag the sprite with the ancilla index and type so its own handler deals
      // with the boomerang/blast effect (stun, push, etc.).
      sprite_B[j] = k + 1;
      sprite_unk2[j] = ancilla_type[k];
      goto return_true_set_alert;
    }
  }

  if (!sprite_ignore_projectile[j]) {
    // Recoil: ±64 pixel/frame in the cardinal direction opposite to travel.
    static const int8 kAncilla_CheckSpriteColl_RecoilX[4] = {0, 0, -64, 64};
    static const int8 kAncilla_CheckSpriteColl_RecoilY[4] = {-64, 64, 0, 0};
    // sprite 0x92 requires phase 3+ before it can be damaged.
    if (sprite_type[j] == 0x92 && sprite_C[j] < 3)
      goto return_true_set_alert;
    i = ancilla_dir[k] & 3;
    sprite_x_recoil[j] = kAncilla_CheckSpriteColl_RecoilX[i];
    sprite_y_recoil[j] = kAncilla_CheckSpriteColl_RecoilY[i];
    byte_7E0FB6 = k;
    Ancilla_CheckDamageToSprite(j, ancilla_type[k]);
return_true_set_alert:
    sprite_unk2[j] = ancilla_type[k];
    sprite_alert_flag = 3;
    return return_value;
  }
  return false;
}

// Fills a SpriteHitBox for ancilla k based on its direction, used for overlap
// testing against sprite hit boxes.
//
// Table indices 0-3: standard cardinal directions — 8×8 box centred 4 px in
// from the ancilla origin. Indices 4-7 (hookshot / Class-2): 1×1 point box at
// the leading tip. Indices 8-11 (hookshot chain, type 0x0C, flags j |= 8):
// 32-wide or 32-tall beam extending behind the head in each direction.
void Ancilla_SetupHitBox(int k, SpriteHitBox *hb) {  // 888ead
  static const int8 kAncilla_HitBox_X[12] = {4, 4, 4, 4, 3, 3, 2, 11, -16, -16, -1, -8};
  static const int8 kAncilla_HitBox_Y[12] = {4, 4, 4, 4, 2, 11, 3, 3, -1, -8, -16, -16};
  static const uint8 kAncilla_HitBox_W[12] = {8, 8, 8, 8, 1, 1, 1, 1, 32, 32, 8, 8};
  static const uint8 kAncilla_HitBox_H[12] = {8, 8, 8, 8, 1, 1, 1, 1, 8, 8, 32, 32};
  int j = ancilla_dir[k];
  // Hookshot chain (type 0x0C) uses the wide-beam box variant (indices 8-11).
  if (ancilla_type[k] == 0xc)
    j |= 8;
  int x = Ancilla_GetX(k) + kAncilla_HitBox_X[j];
  hb->r0_xlo = x;
  hb->r8_xhi = x >> 8;
  int y = Ancilla_GetY(k) + kAncilla_HitBox_Y[j];
  hb->r1_ylo = y;
  hb->r9_yhi = y >> 8;
  hb->r2 = kAncilla_HitBox_W[j];
  hb->r3 = kAncilla_HitBox_H[j];
}

// Decomposes scalar speed `vel` into (x_vel, y_vel) components directed toward
// Link's current position from ancilla k. Uses Bresenham-style integer division
// to distribute the total speed across both axes proportionally to the pixel
// distances (dx, dy) without floating-point math.
//
// Algorithm: treat the smaller absolute distance as the numerator and the larger
// as the denominator; accumulate the numerator each iteration — when the sum
// exceeds the denominator, emit one unit on the minor axis. This ensures the
// ratio xvel:yvel ≈ dx:dy exactly in integer arithmetic.
//
// Returns a ProjectSpeedRet with signed velocity components and the raw (signed
// byte) delta values (right.b = dx byte, below.b = dy byte) for callers that
// need the direction sign separately (e.g. boomerang homing).
ProjectSpeedRet Ancilla_ProjectSpeedTowardsPlayer(int k, uint8 vel) {  // 888eed
  if (vel == 0) {
    ProjectSpeedRet rv = { 0, 0, 0, 0 };
    return rv;
  }
  PairU8 below = Ancilla_IsBelowLink(k);
  uint8 r12 = sign8(below.b) ? -below.b : below.b;

  PairU8 right = Ancilla_IsRightOfLink(k);
  uint8 r13 = sign8(right.b) ? -right.b : right.b;
  uint8 t;
  bool swapped = false;
  // Ensure r13 holds the larger distance (denominator) so the loop always runs.
  if (r13 < r12) {
    swapped = true;
    t = r12, r12 = r13, r13 = t;
  }
  uint8 xvel = vel, yvel = 0;
  t = 0;
  do {
    t += r12;
    if (t >= r13)
      t -= r13, yvel++;
  } while (--vel);
  // If axes were swapped, swap the computed components back.
  if (swapped)
    t = xvel, xvel = yvel, yvel = t;
  ProjectSpeedRet rv = {
    (uint8)(right.a ? -xvel : xvel),
    (uint8)(below.a ? -yvel : yvel),
    right.b,
    below.b
  };
  return rv;
}

// Returns whether Link is to the right of ancilla k (a.a = 1 if Link is right),
// plus the low byte of the signed pixel delta (a.b = (link_x - ancilla_x) & 0xFF).
PairU8 Ancilla_IsRightOfLink(int k) {  // 888f5c
  uint16 x = link_x_coord - Ancilla_GetX(k);
  PairU8 rv = { (uint8)(sign16(x) ? 1 : 0), (uint8)x };
  return rv;
}

// Returns whether Link is below ancilla k (a.a = 1 if Link is below),
// plus the low byte of the signed pixel delta (a.b = (link_y - ancilla_y) & 0xFF).
PairU8 Ancilla_IsBelowLink(int k) {  // 888f6f
  int y = link_y_coord - Ancilla_GetY(k);
  PairU8 rv = { (uint8)(sign16(y) ? 1 : 0), (uint8)y };
  return rv;
}

// Draws the repulse spark that appears when Link's weapon is deflected by a
// shielded sprite (e.g. a Darknut blocking with its shield). The spark lives in
// dedicated global state (repulsespark_*) rather than an ancilla slot.
//
// repulsespark_timer counts 11→0; anim_delay = 1 means the timer decrements
// every other frame. Stages:
//   timer >= 9: single large flash tile (char 0x80) at the impact centre.
//   timer 3-8:  medium spark tile (char 0x92), still single tile.
//   timer 1-2:  4-tile burst — four symmetrical sparks placed ±4 px around
//               centre, each rotated 90° via flip flags (0x00/0x40/0x80/0xC0).
// kRepulseSpark_Char: 0x93 (large corner spark), 0x82 (medium), 0x81 (small).
// kRepulseSpark_Flags: palette/priority; index by repulsespark_floor_status
// so the spark renders on the correct BG layer when in a two-floor room.
void Ancilla_WeaponTink() {  // 888f89
  if (!repulsespark_timer)
    return;
  sprite_alert_flag = 2;
  if (sign8(--repulsespark_anim_delay)) {
    repulsespark_timer--;
    repulsespark_anim_delay = 1;
  }

  if (sort_sprites_setting) {
    if (repulsespark_floor_status)
      Oam_AllocateFromRegionF(0x10);
    else
      Oam_AllocateFromRegionD(0x10);
  } else {
    Oam_AllocateFromRegionA(0x10);
  }

  uint8 x = repulsespark_x_lo - BG2HOFS_copy2;
  uint8 y = repulsespark_y_lo - BG2VOFS_copy2;

  if (x >= 0xf8 || y >= 0xf0) {
    repulsespark_timer = 0;
    return;
  }

  OamEnt *oam = GetOamCurPtr();
  static const uint8 kRepulseSpark_Flags[4] = {0x22, 0x12, 0x22, 0x22};
  uint8 flags = kRepulseSpark_Flags[repulsespark_floor_status];
  if (repulsespark_timer >= 3) {
    SetOamPlain(oam, x, y, (repulsespark_timer < 9) ? 0x92 : 0x80, flags, 0);
    return;
  }
  static const uint8 kRepulseSpark_Char[3] = { 0x93, 0x82, 0x81 };
  uint8 c = kRepulseSpark_Char[repulsespark_timer];
  SetOamPlain(oam + 0, x - 4, y - 4, c, flags | 0x00, 0);
  SetOamPlain(oam + 1, x + 4, y - 4, c, flags | 0x40, 0);
  SetOamPlain(oam + 2, x - 4, y + 4, c, flags | 0x80, 0);
  SetOamPlain(oam + 3, x + 4, y + 4, c, flags | 0xc0, 0);
}

// Advances ancilla k's X position by its signed velocity using 8.8 subpixel
// fixed-point arithmetic. velocity is shifted left 4 bits (1/16 px resolution)
// before being added to the 24-bit position accumulator (subpixel | lo<<8 | hi<<16).
void Ancilla_MoveX(int k) {  // 889080
  uint32 t = ancilla_x_subpixel[k] + (ancilla_x_lo[k] << 8) + (ancilla_x_hi[k] << 16) + ((int8)ancilla_x_vel[k] << 4);
  ancilla_x_subpixel[k] = t, ancilla_x_lo[k] = t >> 8, ancilla_x_hi[k] = t >> 16;
}

// Advances ancilla k's Y position by its signed velocity using the same
// 8.8 subpixel fixed-point scheme as Ancilla_MoveX.
void Ancilla_MoveY(int k) {  // 88908b
  uint32 t = ancilla_y_subpixel[k] + (ancilla_y_lo[k] << 8) + (ancilla_y_hi[k] << 16) + ((int8)ancilla_y_vel[k] << 4);
  ancilla_y_subpixel[k] = t, ancilla_y_lo[k] = t >> 8, ancilla_y_hi[k] = t >> 16;
}

// Advances ancilla k's Z (height) by its signed Z-velocity. Z is a 16-bit
// fixed-point value (subpixel | z<<8); used by thrown/arcing projectiles.
void Ancilla_MoveZ(int k) {  // 8890b7
  uint32 t = ancilla_z_subpixel[k] + (ancilla_z[k] << 8) + ((int8)ancilla_z_vel[k] << 4);
  ancilla_z_subpixel[k] = t, ancilla_z[k] = t >> 8;
}

// Ancilla type 0x05: Boomerang projectile handler. Covers the full flight arc —
// launch, homing return, sprite/tile collision, and termination.
//
// ancilla_aux_timer[k] == 0: boomerang is still at Link's hand, not yet thrown.
//   kBoomerang_X0/Y0 offsets position it at Link's grip point (indexed by facing
//   direction / 2). After one frame it is armed (aux_timer becomes 1).
//
// ancilla_item_to_link[k] == 0: outgoing flight phase.
//   Velocity is kept from spawn; ancilla_K[k] is a deceleration counter
//   (starts negative, bumped toward 0 during the last 5 steps). Step counter
//   exhausted or hitting a screen edge triggers the return phase.
//
// ancilla_item_to_link[k] == 1: homing return phase.
//   Ancilla_ProjectSpeedTowardsPlayer recomputes velocity each frame (link_y
//   temporarily shifted +8 to aim at the sprite's centre). ancilla_K[k]
//   accumulates a speed-up bias. Boomerang_CheatWhenNoOnesLooking applies
//   on-screen corrections. Return phase ends when the boomerang overlaps Link.
//
// ancilla_G[k] != 0: magical boomerang — spawns sword-charge sparkles every
// other frame while in flight.
//
// Collision: tile collision on return is probed with objprio/floor override to
// ignore floors (so the returning boomerang doesn't stop mid-air on staircase
// tiles). Wall hits play SFX 5 (stone) or 6 (glass/mirror tile 0xF0).
//
// If ancilla type 0x22 (ReceiveItem) is active in any slot, or submodule_index
// is set (paused/transition), motion is suppressed but drawing still runs.
void Ancilla05_Boomerang(int k) {  // 8890fc
  int hit_spr;
  // Grip offsets for 8 facing sub-directions (ancilla_arr23[k] >> 1).
  static const int8 kBoomerang_X0[8] = {0, 0, -8, 8, 8, 8, -8, -8};
  static const int8 kBoomerang_Y0[8] = {-16, 6, 0, 0, -8, 8, -8, 8};

  for (int j = 4; j >= 0; j--) {
    if (ancilla_type[j] == 0x22)
      goto exit_and_draw;
  }
  if (submodule_index)
    goto exit_and_draw;

  if (!(frame_counter & 7))
    Ancilla_Sfx2_Pan(k, 0x9);

  if (!ancilla_aux_timer[k]) {
    // Not yet thrown: sit at Link's grip and wait for the throw window.
    if (button_b_frames < 9 && player_handler_timer == 0) {
      if (link_is_bunny_mirror || link_auxiliary_state || link_item_in_hand == 0 && (enhanced_features0 & kFeatures0_MiscBugFixes)) {
        Boomerang_Terminate(k);
        return;
      }
      goto exit_and_draw;
    }
    int j = ancilla_arr23[k] >> 1;
    Ancilla_SetXY(k, link_x_coord + kBoomerang_X0[j], link_y_coord + 8 + kBoomerang_Y0[j]);
    ancilla_aux_timer[k]++;
  }
  // Magical boomerang: emit sparkle every other frame.
  if (ancilla_G[k] && !(frame_counter & 1))
    AncillaAdd_SwordChargeSparkle(k);

  if (ancilla_item_to_link[k]) {
    // Homing return: rebuild velocity toward Link every frame.
    if (ancilla_K[k])
      ancilla_K[k]++;
    WORD(ancilla_A[k]) = link_y_coord;
    link_y_coord += 8;  // Aim at sprite centre, not top.
    ProjectSpeedRet pt = Ancilla_ProjectSpeedTowardsPlayer(k, ancilla_H[k]);
    Boomerang_CheatWhenNoOnesLooking(k, &pt);
    ancilla_x_vel[k] = pt.x;
    ancilla_y_vel[k] = pt.y;
    link_y_coord = WORD(ancilla_A[k]);
  }

  // Apply deceleration bias to each non-zero velocity component.
  if (ancilla_y_vel[k])
    ancilla_y_vel[k] += ancilla_K[k];
  Ancilla_MoveY(k);

  if (ancilla_x_vel[k])
    ancilla_x_vel[k] += ancilla_K[k];
  Ancilla_MoveX(k);
  hit_spr = Ancilla_CheckSpriteCollision(k);

  if (!ancilla_item_to_link[k]) {
    // Outgoing: flip to return on hit, wall, edge, or step exhaustion.
    if (hit_spr >= 0) {
      ancilla_item_to_link[k] ^= 1;
    } else if (Ancilla_CheckTileCollision(k)) {
      AncillaAdd_BoomerangWallClink(k);
      Ancilla_Sfx2_Pan(k, (ancilla_tile_attr[k] == 0xf0) ? 6 : 5);
      ancilla_item_to_link[k] ^= 1;
    } else if (Boomerang_ScreenEdge(k) || --ancilla_step[k] == 0) {
      ancilla_item_to_link[k] ^= 1;
    } else {
      // Slow down during the last 5 steps.
      if (ancilla_step[k] < 5)
        ancilla_K[k]--;
    }
  } else {
    // Returning: probe tiles with floor/objprio cleared so staircase tiles
    // don't terminate the boomerang prematurely mid-air.
    uint8 bak0 = ancilla_objprio[k];
    uint8 bak1 = ancilla_floor[k];
    ancilla_floor[k] = 0;
    Ancilla_CheckTileCollision(k);
    ancilla_floor[k] = bak1;
    ancilla_objprio[k] = bak0;
    Boomerang_StopOffScreen(k);
  }

exit_and_draw:
  Boomerang_Draw(k);
}

// Returns true if the boomerang k has reached or exceeded a screen boundary.
// hookshot_effect_index encodes which edges to check: bits 0-1 for left/right,
// bits 2-3 for up/down. Bit 0 adds +16 to the right-edge check so the boomerang
// must fully cross before returning; bit 2 adds +16 similarly for the bottom.
bool Boomerang_ScreenEdge(int k) {  // 88924b
  uint16 x = Ancilla_GetX(k), y = Ancilla_GetY(k);
  if (hookshot_effect_index & 3) {
    uint16 t = x + (hookshot_effect_index & 1 ? 16 : 0) - BG2HOFS_copy2;
    if (t >= 0x100)
      return true;
  }
  if (hookshot_effect_index & 12) {
    uint16 t = y + (hookshot_effect_index & 4 ? 16 : 0) - BG2VOFS_copy2;
    if (t >= 0xe2)
      return true;
  }
  return false;
}

// Terminates the returning boomerang once its centre (+8, +8) is within Link's
// 16×24 bounding box. Called every frame during the return phase so the catch
// occurs as soon as the boomerang overlaps Link's sprite rectangle.
void Boomerang_StopOffScreen(int k) {  // 8892ab
  uint16 x = Ancilla_GetX(k) + 8, y = Ancilla_GetY(k) + 8;
  if (x >= link_x_coord && x < (uint16)(link_x_coord + 16) &&
      y >= link_y_coord && y < (uint16)(link_y_coord + 24))
    Boomerang_Terminate(k);
}

// Clears the boomerang ancilla slot and releases the input lock held while it
// was in flight. link_item_in_hand bit 7 indicates the boomerang was thrown via
// the dedicated throw state; clearing it re-enables Link's facing control
// (link_cant_change_direction bit 0) unless the sword-swing lock (bit 7 of
// button_mask_b_y) is also active.
void Boomerang_Terminate(int k) {  // 8892f5
  ancilla_type[k] = 0;
  flag_for_boomerang_in_place = 0;
  if (link_item_in_hand & 0x80) {
    link_item_in_hand = 0;
    button_mask_b_y &= ~0x40;
    if (!(button_mask_b_y & 0x80))
      link_cant_change_direction &= ~1;
  }
}

// Draws one boomerang tile (char 0x26) with spin animation.
//
// kBoomerang_Flags[8]: palette/flip flags indexed by (magical_flag * 4 + spin_frame).
//   Lower 4 entries (ancilla_G==0): normal wooden boomerang palette.
//   Upper 4 entries (ancilla_G==1): magical boomerang palette.
//   Flip bits rotate the tile through 4 orientations to simulate spinning.
//
// kBoomerang_Draw_XY[8]: sub-pixel offsets ±2 px in X and Y applied per spin
//   frame to give the boomerang a slight wobble as it rotates.
//
// Spin rate: ancilla_arr3 counts down from kBoomerang_Draw_Tab0[G] (3 for
//   normal, 2 for magical — faster spin). ancilla_arr1 cycles 0-3 for spin
//   phase; ancilla_S controls direction (CW vs CCW).
//
// When not yet thrown (aux_timer == 0): OAM pointers are redirected to the
//   pre-thrown region (kBoomerang_Draw_OamIdx) so the static grip sprite draws
//   in the correct layer without consuming a flight-slot OAM entry.
//
// When returning (item_to_link == 1): floor and priority are inherited from
//   Link's current state so the boomerang renders on the correct BG layer.
void Boomerang_Draw(int k) {  // 889338
  static const uint8 kBoomerang_Flags[8] = {0xa4, 0xe4, 0x64, 0x24, 0xa2, 0xe2, 0x62, 0x22};
  static const int8 kBoomerang_Draw_XY[8] = {2, -2, 2, 2, -2, 2, -2, -2};
  static const uint16 kBoomerang_Draw_OamIdx[2] = {0x180, 0xd0};
  static const uint8 kBoomerang_Draw_Tab0[2] = {3, 2};
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);

  if (ancilla_item_to_link[k]) {
    // Returning boomerang renders on Link's current floor layer.
    ancilla_floor[k] = link_is_on_lower_level;
    oam_priority_value = kTagalongLayerBits[link_is_on_lower_level] << 8;
  }

  if (ancilla_objprio[k])
    oam_priority_value = 0x3000;

  // Advance spin animation unless game is paused or boomerang not yet thrown.
  if (!submodule_index && ancilla_aux_timer[k] && sign8(--ancilla_arr3[k])) {
    ancilla_arr3[k] = kBoomerang_Draw_Tab0[ancilla_G[k]];
    ancilla_arr1[k] = (ancilla_arr1[k] + (ancilla_S[k] ? -1 : 1)) & 3;
  }

  int j = ancilla_arr1[k];
  uint16 x = info.x + kBoomerang_Draw_XY[j * 2 + 1];
  uint16 y = info.y + kBoomerang_Draw_XY[j * 2 + 0];
  if (!ancilla_aux_timer[k]) {
    // Not yet thrown: redirect OAM pointer to the grip-display region.
    int i = kBoomerang_Draw_OamIdx[sort_sprites_setting];
    oam_ext_cur_ptr = (i >> 2) + 0xa20;
    oam_cur_ptr = i + 0x800;
  }
  Ancilla_SetOam_Safe(GetOamCurPtr(), x, y, 0x26, kBoomerang_Flags[ancilla_G[k] * 4 + j] & ~0x30 | HIBYTE(oam_priority_value), 2);
}

// Ancilla type 0x06: Boomerang wall-clink spark. Advances through 5 animation
// frames (ancilla_item_to_link 0-4, each lasting 1 game-tick via arr3) then
// self-destructs. Delegates drawing to WallHit_Draw.
void Ancilla06_WallHit(int k) {  // 8893e8
  if (sign8(--ancilla_arr3[k])) {
    uint8 t = ancilla_item_to_link[k] + 1;
    if (t == 5) {
      ancilla_type[k] = 0;
      return;
    }
    ancilla_item_to_link[k] = t;
    ancilla_arr3[k] = 1;
  }
  WallHit_Draw(k);
}

// Sword/arrow wall-hit flash. Shares WallHit_Draw but uses 8 animation frames
// (aux_timer counts 1 tick each) and sets sprite_alert_flag = 3 every frame
// to keep nearby sprites in alert state while the sparks are visible.
void Ancilla_SwordWallHit(int k) {  // 8893ff
  sprite_alert_flag = 3;
  if (sign8(--ancilla_aux_timer[k])) {
    uint8 t = ancilla_item_to_link[k] + 1;
    if (t == 8) {
      ancilla_type[k] = 0;
      return;
    }
    ancilla_item_to_link[k] = t;
    ancilla_aux_timer[k] = 1;
  }
  WallHit_Draw(k);
}

// Draws the wall-hit / weapon-spark animation. Each animation frame occupies 4
// entries in the tables (indexed as ancilla_item_to_link[k] * 4). Up to 4
// OAM tiles are drawn; entries with char == 0 are skipped (sparse frames).
//
// kWallHit_Char progression (representative frames):
//   frame 0: single flash (0x80) — peak impact.
//   frame 1: mid spark (0x92).
//   frames 2-4: four-corner burst expanding outward (0x81→0x82→0x93).
//   frame 5: medium (0x92) — secondary peak for sword sparks.
//   frame 6: boomerang-specific (0xb9).
//   frame 7: double spark (0x90×2).
// kWallHit_Flags encode flip bits (0x40/0x80) for symmetric placement and
// priority bits; layer bits are overwritten from oam_priority_value each frame.
void WallHit_Draw(int k) {  // 8894df
  static const int8 kWallHit_X[32] = {
    -4, 0,  0, 0, -4, 0, 0, 0, -8, 0, -8, 0, -8, 0, -8, 0,
    -8, 0, -8, 0, -4, 0, 0, 0, -4, 0,  0, 0, -8, 0,  0, 0,
  };
  static const int8 kWallHit_Y[32] = {
    -4,  0, 0, 0, -4, 0, 0, 0, -8, -8, 0, 0, -8, -8, 0, 0,
    -8, -8, 0, 0, -4, 0, 0, 0, -4,  0, 0, 0, -8,  0, 0, 0,
  };
  static const uint8 kWallHit_Char[32] = {
    0x80,    0,    0,    0, 0x92, 0, 0, 0, 0x81, 0x81, 0x81, 0x81, 0x82, 0x82, 0x82, 0x82,
    0x93, 0x93, 0x93, 0x93, 0x92, 0, 0, 0, 0xb9,    0,    0,    0, 0x90, 0x90,    0,    0,
  };
  static const uint8 kWallHit_Flags[32] = {
    0x32,    0,    0,    0, 0x32, 0, 0, 0, 0x32, 0x72, 0xb2, 0xf2, 0x32, 0x72, 0xb2, 0xf2,
    0x32, 0x72, 0xb2, 0xf2, 0x32, 0, 0, 0, 0x72,    0,    0,    0, 0x32, 0xf2,    0,    0,
  };
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);

  // Each frame uses 4 consecutive table entries; skip if char is 0 (empty slot).
  int t = ancilla_item_to_link[k] * 4;

  OamEnt *oam = GetOamCurPtr();
  for (int n = 3; n >= 0; n--, t++) {
    if (kWallHit_Char[t] != 0) {
      Ancilla_SetOam(oam, info.x + kWallHit_X[t], info.y + kWallHit_Y[t], kWallHit_Char[t],
                     kWallHit_Flags[t] & ~0x30 | HIBYTE(oam_priority_value), 0);
      oam++;
    }
    oam = Ancilla_AllocateOamFromCustomRegion(oam);
  }

}

// Ancilla type 0x07: Bomb. Handles the full bomb lifecycle — sitting on the
// ground, being picked up / carried, sliding on conveyors, falling into water,
// detonation countdown, and the transition to door-debris (type 0x08).
//
// ancilla_item_to_link[k] is the detonation phase counter (0 = idle, 1-10 =
// fuse counting, 11 = detonate → transmute to door debris or self-destruct).
// ancilla_arr3[k] is the per-phase tick delay, loaded from kBomb_Tab0.
//
// submodule_index != 0: game is in a menu/transition.
//   submodule 8 or 16 (chest/door open): run lift logic only; draw and return.
//   Otherwise: if this bomb is the one being picked up (flag_is_ancilla_to_pick_up),
//   latch it to Link's carry position (K==3 = fully carried).
//
// ancilla_L[k]: set when the bomb is a super bomb (larger blast radius).
// ancilla_T[k]: set when the bomb lands on a cracked floor tile (0x1C) to
//   trigger a floor-break effect on detonation.
// ancilla_step[k]: set if the detonation should spawn door debris (Ancilla08).
//
// Tile reactions (via Ancilla_CheckTileCollision_Class2 with dir=16):
//   0x26 = wall bounce — re-evaluate with flag=true.
//   0x0C / 0x1C = stair edge — promote to upper floor or apply moving-floor vel.
//   0x20 / 0xB0-range = pit / void — kill bomb (unless Link is carrying it).
//   0x08 = water — transmute to splash ancilla.
//   0x68-0x6B = conveyor belt — apply belt velocity.
//   other = set timer (idle on ground: timer=2, or 0 if super bomb).
//
// Phase 7 + arr3==2: check for destructible doors/blocks near the detonation
// point via Bomb_CheckForDestructibles to prime the debris spawn flag.
void Ancilla07_Bomb(int k) {  // 88955a
  if (submodule_index) {
    if (submodule_index == 8 || submodule_index == 16) {
      Ancilla_HandleLiftLogic(k);
    } else if (k + 1 == flag_is_ancilla_to_pick_up && ancilla_K[k] != 0) {
      if (ancilla_K[k] != 3) {
        Ancilla_LatchLinkCoordinates(k, 3);
        Ancilla_LatchAltitudeAboveLink(k);
        ancilla_K[k] = 3;
      }
      Ancilla_LatchCarriedPosition(k);
    }
    Bomb_Draw(k);
    return;
  }
  Ancilla_HandleLiftLogic(k);

  uint16 old_y = Ancilla_LatchYCoordToZ(k);
  uint8 s1a = ancilla_dir[k];
  uint8 s1b = ancilla_objprio[k];
  ancilla_objprio[k] = 0;
  bool flag = Ancilla_CheckTileCollision_Class2(k);

  if (player_is_indoors && ancilla_L[k] && ancilla_tile_attr[k] == 0x1c)
    ancilla_T[k] = 1;

label1:
  if (flag && (!(link_state_bits & 0x80) || link_picking_throw_state)) {
    if (!s1b && !ancilla_arr4[k]) {
      ancilla_arr4[k] = 1;
      int qq = (ancilla_dir[k] == 1) ? 16 : 4;
      if (ancilla_y_vel[k])
        ancilla_y_vel[k] = sign8(ancilla_y_vel[k]) ? qq : -qq;
      if (ancilla_x_vel[k])
        ancilla_x_vel[k] = sign8(ancilla_x_vel[k]) ? 4 : -4;
      if (ancilla_dir[k] == 1 && ancilla_z[k]) {
        ancilla_y_vel[k] = -4;
        ancilla_L[k] = 2;
      }
    }
  } else if (!((k + 1 == flag_is_ancilla_to_pick_up) && (link_state_bits & 0x80)) && (ancilla_z[k] == 0 || ancilla_z[k] == 0xff)) {
    ancilla_dir[k] = 16;
    uint8 bak0 = ancilla_objprio[k];
    Ancilla_CheckTileCollision(k);
    ancilla_objprio[k] = bak0;
    uint8 a = ancilla_tile_attr[k];
    if (a == 0x26) {
      flag = true;
      goto label1;
    } else if (a == 0xc || a == 0x1c) {
      if (dung_hdr_collision != 3) {
        if (ancilla_floor[k] == 0 && ancilla_z[k] != 0 && ancilla_z[k] != 0xff)
          ancilla_floor[k] = 1;
      } else {
        old_y = Ancilla_GetY(k) + dung_floor_y_vel;
        Ancilla_SetX(k, Ancilla_GetX(k) + dung_floor_x_vel);
      }
    } else if (a == 0x20 || (a & 0xf0) == 0xb0 && a != 0xb6 && a != 0xbc) {
      if (!(link_state_bits & 0x80)) {
        if (k + 1 == flag_is_ancilla_to_pick_up)
          flag_is_ancilla_to_pick_up = 0;
        if (!ancilla_timer[k]) {
          ancilla_type[k] = 0;
          return;
        }
      }
    } else if (a == 8) {
      if (k + 1 == flag_is_ancilla_to_pick_up)
        flag_is_ancilla_to_pick_up = 0;
      if (ancilla_timer[k] == 0) {
        Ancilla_SetY(k, Ancilla_GetY(k) - 24);
        Ancilla_TransmuteToSplash(k);
        return;
      }
    } else if (a == 0x68 || a == 0x69 || a == 0x6a || a == 0x6b) {
      Ancilla_ApplyConveyor(k);
      old_y = Ancilla_GetY(k);
    } else {
      ancilla_timer[k] = ancilla_L[k] ? 0 : 2;
    }
  }
  // endif_3

  Ancilla_SetY(k, old_y);
  ancilla_dir[k] = s1a;
  ancilla_objprio[k] |= s1b;
  Bomb_CheckSpriteAndPlayerDamage(k);
  if (!--ancilla_arr3[k]) {
    if (++ancilla_item_to_link[k] == 1) {
      Ancilla_Sfx2_Pan(k, 0xc);
      if (k + 1 == flag_is_ancilla_to_pick_up) {
        flag_is_ancilla_to_pick_up = 0;
        if (link_state_bits & 0x80) {
          link_state_bits = 0;
          link_cant_change_direction = 0;
        }
      }
    }

    if (ancilla_item_to_link[k] == 11) {
      // transmute to door debris?
      ancilla_type[k] = ancilla_step[k] ? 8 : 0;
      return;
    }
    ancilla_arr3[k] = kBomb_Tab0[ancilla_item_to_link[k]];
  }

  if (ancilla_item_to_link[k] == 7 && ancilla_arr3[k] == 2) {
    // check whether the bomb causes any door debris, the bomb
    // will transmute to debris later on.
    door_debris_x[k] = 0;
    Bomb_CheckForDestructibles(Ancilla_GetX(k), Ancilla_GetY(k), k);
    if (door_debris_x[k])
      ancilla_step[k] = 1;
  }
  Bomb_Draw(k);
}

// Applies a conveyor-belt velocity impulse to ancilla k based on the belt tile
// attribute. Tile attributes 0x68-0x6B map to directions: 0=up, 1=down,
// 2=left, 3=right. Moves the ancilla immediately for this frame.
void Ancilla_ApplyConveyor(int k) {  // 8897be
  static const int8 kAncilla_Belt_Xvel[4] = {0, 0, -8, 8};
  static const int8 kAncilla_Belt_Yvel[4] = {-8, 8, 0, 0};
  int j = ancilla_tile_attr[k] - 0x68;
  ancilla_y_vel[k] = kAncilla_Belt_Yvel[j];
  ancilla_x_vel[k] = kAncilla_Belt_Xvel[j];
  Ancilla_MoveY(k);
  Ancilla_MoveX(k);
}

// Checks blast damage for both sprites and Link during the bomb's active
// detonation window (phases 1-8; phase 0 = idle, 9+ = post-blast).
//
// Link's damage is scaled by distance from the bomb centre (j = 0-15, where 0
// is closest): kBomb_Dmg_Speed gives the outward recoil velocity, kBomb_Dmg_Zvel
// gives the launch Z velocity (higher = bigger arc), and kBomb_Dmg_Delay gives
// the incapacitation timer. All three taper off at larger distances (≥6: speed
// 28, ≥12: speed 24). Link's HP damage is armor-scaled: 8/4/2 for no/blue/red.
//
// Link immunity conditions: already in auxiliary state, already incapacitating
// timer running, different floor, invincibility blink active, or shield-block
// mode (flag_block_link_menu == 2).
//
// If the bomb is the one Link is actively carrying (flag_is_ancilla_to_pick_up
// matches), clear the carry state so Link can move freely after detonation.
void Bomb_CheckSpriteAndPlayerDamage(int k) {  // 889815
  static const uint8 kBomb_Dmg_Speed[16] = {32, 32, 32, 32, 32, 32, 28, 28, 28, 28, 28, 28, 24, 24, 24, 24};
  static const uint8 kBomb_Dmg_Zvel[16] = {16, 16, 16, 16, 16, 16, 12, 12, 12, 12, 8, 8, 8, 8, 8, 8};
  static const uint8 kBomb_Dmg_Delay[16] = {32, 32, 32, 32, 32, 32, 24, 24, 24, 24, 24, 24, 16, 16, 16, 16};
  static const uint8 kBomb_Dmg_ToLink[3] = {8, 4, 2};

  if (ancilla_item_to_link[k] == 0 || ancilla_item_to_link[k] >= 9)
    return;
  Bomb_CheckSpriteDamage(k);
  if (link_disable_sprite_damage) {
    if (k + 1 == flag_is_ancilla_to_pick_up && link_state_bits & 0x80) {
      link_state_bits &= ~0x80;
      link_cant_change_direction = 0;
    }
    return;
  }

  if (link_auxiliary_state || link_incapacitated_timer || ancilla_floor[k] != link_is_on_lower_level)
    return;

  // Link hitbox: 16×24. Bomb blast radius: 32×32 centred 16 px behind origin.
  SpriteHitBox hb;
  hb.r0_xlo = link_x_coord;
  hb.r8_xhi = link_x_coord >> 8;
  hb.r1_ylo = link_y_coord;
  hb.r9_yhi = link_y_coord >> 8;
  hb.r2 = 0x10;
  hb.r3 = 0x18;

  int ax = Ancilla_GetX(k) - 16, ay = Ancilla_GetY(k) - 16;
  hb.r6_spr_xsize = 32;
  hb.r7_spr_ysize = 32;
  hb.r4_spr_xlo = ax;
  hb.r10_spr_xhi = ax >> 8;
  hb.r5_spr_ylo = ay;
  hb.r11_spr_yhi = ay >> 8;

  if (!CheckIfHitBoxesOverlap(&hb))
    return;

  // Recoil origin is offset slightly from the bomb centre for a more natural
  // blast vector (–8 x, –12 y = roughly waist-height on a 16×24 sprite).
  int x = Ancilla_GetX(k) - 8, y = Ancilla_GetY(k) - 12;

  int j = Bomb_GetDisplacementFromLink(k);
  ProjectSpeedRet pt = Bomb_ProjectSpeedTowardsPlayer(k, x, y, kBomb_Dmg_Speed[j]);
  if (countdown_for_blink || flag_block_link_menu == 2)
    return;
  link_actual_vel_x = pt.x;
  link_actual_vel_y = pt.y;

  link_actual_vel_z_copy = link_actual_vel_z = kBomb_Dmg_Zvel[j];
  link_incapacitated_timer = kBomb_Dmg_Delay[j];
  link_auxiliary_state = 1;
  countdown_for_blink = 58;
  if (!(dung_savegame_state_bits & 0x8000))
    link_give_damage = kBomb_Dmg_ToLink[link_armor];

}

// Handles the pick-up, carry, and throw physics shared by all liftable ancillae
// (bombs, pots, bushes, etc.). Manages three distinct sub-states:
//
// ancilla_R[k] != 0: "toss-back" recovery — item was dropped while Link was
//   hit. The item arcs upward (z_vel=24) then falls back; on landing it bounces
//   once (R==1→2), then resets to idle (R==3 cleared).
//
// ancilla_L[k] == 0: item is on the ground; eligible for pick-up. When Link
//   walks into it (Ancilla_CheckLinkCollision) and is facing toward it, the
//   pickup sequence begins: flag_is_ancilla_to_pick_up records which slot,
//   and ancilla_K[k] tracks the lift stage (0→1→2→3).
//   kAncilla_Liftable_Delay[0/1/2] = 16/8/9 ticks per lift stage.
//
// ancilla_L[k] == 1: item is in-flight after being thrown. Gravity applies
//   (z_vel -= 2 each frame). On each bounce (z wraps through 0), bounce count
//   increments ancilla_L toward 3. At bounce 3: item comes to rest; if
//   ancilla_T[k] is set, the floor index is updated (cracked floor reaction).
//
// When K==3 and the item is fully lifted (link_picking_throw_state == 2 or
//   B-button pressed), the throw is initiated: velocity is set by direction,
//   z_vel = 24, and link_picking_throw_state = 2.
//
// kAncilla_Liftable_Yvel/Xvel: throw velocities ±32 px/frame per axis.
// Ancilla_LatchCarriedPosition: snaps the item above Link's head each frame
//   while being carried (compensating for Link's Z and animation bob).
void Ancilla_HandleLiftLogic(int k) {  // 889976
  static const uint8 kAncilla_Liftable_Delay[3] = {16, 8, 9};

  if (ancilla_R[k]) {
label_6:
    if (ancilla_item_to_link[k])
      return;
    if (ancilla_K[k] == 3) {
      ancilla_z_vel[k] -= 2;
      Ancilla_MoveZ(k);
      if (ancilla_z[k] && ancilla_z[k] < 252)
        return;
      ancilla_z[k] = 0;
      if (++ancilla_R[k] != 3) {
        ancilla_z_vel[k] = 24;
        return;
      }
      ancilla_K[k] = 0;
    }
    ancilla_R[k] = 0;
    link_speed_setting = 0;
    return;
  }
  if (!ancilla_L[k]) {
    if (!flag_is_ancilla_to_pick_up) {
clear_pickup_item:
      flag_is_ancilla_to_pick_up = 0;
      CheckPlayerCollOut coll;
      if (ancilla_item_to_link[k] || link_state_bits || !Ancilla_CheckLinkCollision(k, 0, &coll) || ancilla_floor[k] != link_is_on_lower_level)
        return;
      if (coll.r8 >= 16 || coll.r10 >= 12) {
        int j = (coll.r8 >= coll.r10) ? (sign8(coll.r4) ? 1 : 0) : (sign8(coll.r6) ? 3 : 2);
        if (j * 2 != link_direction_facing)
          return;
      }
      flag_is_ancilla_to_pick_up = k + 1;
      ancilla_K[k] = 0;
      ancilla_aux_timer[k] = kAncilla_Liftable_Delay[0];
      ancilla_L[k] = 0;
      ancilla_z[k] = 0;
      return;
    }

    if (flag_is_ancilla_to_pick_up != k + 1)
      return;
    if (!link_disable_sprite_damage && link_incapacitated_timer || byte_7E03FD || link_auxiliary_state == 1) {
      ancilla_R[k] = 1;
      ancilla_z_vel[k] = 0;
      flag_is_ancilla_to_pick_up = 0;
      ancilla_arr4[k] = 0;
      goto label_6;
    }
    if (!(link_state_bits & 0x80))
      goto clear_pickup_item;
    int j = ancilla_K[k];
    if (link_picking_throw_state != 2 && flag_is_ancilla_to_pick_up != 0 && j != 3) {
      if (j == 0 && ancilla_aux_timer[k] == 16)
        Ancilla_Sfx2_Pan(k, 0x1d);
      if (sign8(--ancilla_aux_timer[k])) {
        ancilla_K[k] = ++j;
        ancilla_aux_timer[k] = j == 3 ? -2 : kAncilla_Liftable_Delay[j];
        if (j == 3) {
          Ancilla_LatchAltitudeAboveLink(k);
          return;
        }
      }
      Ancilla_LatchLinkCoordinates(k, j);
      return;
    }
    if (j != 3)
      return;

    if (link_picking_throw_state != 2 && (submodule_index != 0 || !((filtered_joypad_L | filtered_joypad_H) & 0x80))) {
      if (ancilla_item_to_link[k])
        return;
      if (player_near_pit_state >= 2) {
        link_speed_setting = 0;
        if (k + 1 == flag_is_ancilla_to_pick_up) {
          flag_is_ancilla_to_pick_up = 0;
          ancilla_type[k] = 0;
        }
        return;
      }
      if (!(link_is_in_deep_water | link_is_bunny_mirror)) {
        Ancilla_LatchCarriedPosition(k);
        return;
      }
      link_state_bits = 0;
    }
    static const int8 kAncilla_Liftable_Yvel[4] = {-32, 32, 0, 0};
    static const int8 kAncilla_Liftable_Xvel[4] = {0, 0, -32, 32};
    j = link_direction_facing >> 1;
    ancilla_dir[k] = j;
    ancilla_z_vel[k] = 24;
    ancilla_y_vel[k] = kAncilla_Liftable_Yvel[j];
    ancilla_x_vel[k] = kAncilla_Liftable_Xvel[j];
    link_picking_throw_state = 2;
    ancilla_L[k] = 1;
    flag_is_ancilla_to_pick_up = 0;
    ancilla_arr4[k] = 0;
    ancilla_K[k] = 0;
    ancilla_objprio[k] = 0;
    Ancilla_Sfx3_Pan(k, 0x13);
  }
  // endif_1
  if (!ancilla_item_to_link[k]) {
    ancilla_z_vel[k] -= 2;
    Ancilla_MoveY(k);
    Ancilla_MoveX(k);
    uint8 old_z = ancilla_z[k];
    Ancilla_MoveZ(k);
    if (ancilla_arr4[k] && ancilla_dir[k] == 1 && !sign8(ancilla_z[k]))
      Ancilla_SetY(k, Ancilla_GetY(k) + (int8)(ancilla_z[k] - old_z));
    if (!sign8(ancilla_z[k]) || ancilla_z[k] == 0xff)
      return;
    ancilla_z[k] = 0;
    Ancilla_Sfx2_Pan(k, 0x21);
    if (++ancilla_L[k] != 3) {
      ancilla_y_vel[k] = (int8)ancilla_y_vel[k] / 2;
      ancilla_x_vel[k] = (int8)ancilla_x_vel[k] / 2;
      ancilla_z_vel[k] = 16;
      ancilla_arr4[k] = 0;
    } else {
      ancilla_z[k] = 0;
      ancilla_L[k] = 0;
      ancilla_arr4[k] = 0;
      link_speed_setting = 0;
      ancilla_y_vel[k] = 0;
      ancilla_x_vel[k] = 0;
      ancilla_z_vel[k] = 0;
      if (ancilla_T[k]) {
        ancilla_floor[k] = ancilla_T[k];
        ancilla_T[k] = 0;
      }
    }
  }
}

// Sets up a liftable ancilla at the height it would be if held above Link.
// Z = 17 encodes the visual height offset (17 px above the ground plane);
// Y is shifted down by 17 to match so world-space Y stays consistent when Z is
// later subtracted for rendering. objprio is cleared so it renders behind Link.
void Ancilla_LatchAltitudeAboveLink(int k) {  // 889a4f
  ancilla_z[k] = 17;
  Ancilla_SetY(k, Ancilla_GetY(k) + 17);
  ancilla_objprio[k] = 0;
}

// Positions the carried ancilla at Link's hand location for lift stage j and
// the current facing direction. Tables are indexed as j*4 + (facing >> 1) to
// give 12 grip points (3 stages × 4 directions). Offsets place the item at
// Link's outstretched hand as he lifts it from the ground to overhead.
void Ancilla_LatchLinkCoordinates(int k, int j) {  // 889a6a
  static const int8 kAncilla_Func3_X[12] = {8, 8, -4, 20, 8, 8, 8, 8, 8, 8, 8, 8};
  static const int8 kAncilla_Func3_Y[12] = {16, 8, 4, 4, 8, 2, -1, -1, 2, 2, -1, -1};
  j = j * 4 + (link_direction_facing >> 1);
  Ancilla_SetXY(k,
      link_x_coord + kAncilla_Func3_X[j],
      link_y_coord + kAncilla_Func3_Y[j]);
}

// Locks a fully-carried ancilla above Link's head while he walks. Adjusts Y
// for Link's current Z height (jumping) and a 3-frame animation bob
// (kAncilla_Func2_Y: −2/−1/0 pattern). Sets link_speed_setting = 12 to
// enforce the reduced walk speed while carrying. Syncs floor flags from Link.
void Ancilla_LatchCarriedPosition(int k) {  // 889bef
  static const int8 kAncilla_Func2_Y[6] = {-2, -1, 0, -2, -1, 0};
  link_speed_setting = 12;
  ancilla_floor[k] = link_is_on_lower_level;
  ancilla_floor2[k] = link_is_on_lower_level_mirror;
  uint16 z = link_z_coord;
  if (z == 0xffff)
    z = 0;
  Ancilla_SetXY(k,
    link_x_coord + 8,
    link_y_coord - z + 18 + kAncilla_Func2_Y[link_animation_steps]);
}

// For downward-thrown items (dir == 1): subtracts the current Z height from the
// Y coordinate so the item appears to arc through the air correctly, then
// returns the original Y for restoration at frame end. Other directions return
// Y unchanged (no arc visual needed).
uint16 Ancilla_LatchYCoordToZ(int k) {  // 889c7f
  uint16 y = Ancilla_GetY(k);
  int8 z = ancilla_z[k];
  if (ancilla_dir[k] == 1 && z != -1)
    Ancilla_SetY(k, y - z);
  return y;
}

// Returns a 0-15 distance index for the bomb-to-Link displacement used to
// look up damage/knockback scaling tables. Manhattan distance capped to 60 px
// (0xfc mask) then right-shifted 2 gives 0-15 (one step per 4 px, 16 steps).
int Bomb_GetDisplacementFromLink(int k) {  // 889cce
  int x = Ancilla_GetX(k), y = Ancilla_GetY(k);
  return ((abs16(link_x_coord + 8 - x) + abs16(link_y_coord + 12 - y)) & 0xfc) >> 2;
}

// Draws the bomb sprite and its blast animation. Handles three visual states:
//
// Idle (item_to_link == 0): draws the bomb shell with a flash rate derived from
//   the fuse timer (arr3 < 0x20: blink at arr3 & 0xE, otherwise steady at 4).
//   OAM region is adjusted based on context: if a Wizzrobe (sprite 0x92) is
//   present or the bomb is being picked up, allocate from B/E region; if on
//   the upper floor while being carried, redirect OAM to slot 0x34.
//   The explosion tile is offset by +2 OAM slots when on water/ice tiles
//   (tile_attr 9 or 0x40) to composite the reflection below.
//
// Detonating (item_to_link 1-10): kBomb_Draw_Tab0 maps the phase to a sprite
//   set index; kBomb_Draw_Tab2 gives the number of tiles per frame. The
//   explosion is drawn via AncillaDraw_Explosion which selects from kBomb
//   sub-tables for expanding/contracting blast frames.
//
// Shadow: drawn unconditionally below the bomb unless Bomb_CheckUndersideSpriteStatus
//   reports the underside OAM slot is occupied (returns true). Shadow uses the
//   original pre-Z-offset Y coordinate.
//
// Z height: subtracted from pt.y before drawing so the bomb visually rises
//   when thrown. objprio set when airborne (z != 0, K != 3) to render above BG.
void Bomb_Draw(int k) {  // 889e9e
  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);

  int z = (int8)ancilla_z[k];
  if (z != 0 && z != -1 && ancilla_K[k] != 3 && ancilla_objprio[k])
    oam_priority_value = 0x3000;
  pt.y -= z;
  int j = kBomb_Draw_Tab0[ancilla_item_to_link[k]] * 6;

  uint8 r11 = 2;
  if (ancilla_item_to_link[k] == 0) {
    // Fuse blink: fast blink when fewer than 32 ticks remain; steady otherwise.
    r11 = (ancilla_arr3[k] < 0x20) ? ancilla_arr3[k] & 0xe : 4;
  }

  if (ancilla_item_to_link[k] == 0) {
    if (ancilla_L[k] == 0 && (sprite_type[0] == 0x92 || k + 1 == flag_is_ancilla_to_pick_up ) && (!(link_state_bits & 0x80) || ancilla_K[k] != 3 && link_direction_facing == 0)) {
      Ancilla_AllocateOamFromRegion_B_or_E(12);
    } else if (sort_sprites_setting && ancilla_floor[k] && (ancilla_L[k] || k + 1 == flag_is_ancilla_to_pick_up && (link_state_bits & 0x80))) {
      oam_cur_ptr = 0x800 + 0x34 * 4;
      oam_ext_cur_ptr = 0xa20 + 0x34;
    }
  }

  OamEnt *oam = GetOamCurPtr(), *oam_org = oam;
  uint8 numframes = kBomb_Draw_Tab2[ancilla_item_to_link[k]];

  // On water/ice tiles: advance OAM pointer by 2 to leave room for reflection.
  oam += (ancilla_item_to_link[k] == 0 && (ancilla_tile_attr[k] == 9 || ancilla_tile_attr[k] == 0x40)) ? 2 : 0;

  AncillaDraw_Explosion(oam, j, 0, numframes, r11, pt.x, pt.y);
  oam += numframes;

  uint8 r10;
  if (!Bomb_CheckUndersideSpriteStatus(k, &pt, &r10)) {
    if (oam != oam_org + 1)
      oam = oam_org;
    AncillaDraw_Shadow(oam, r10, pt.x, pt.y, HIBYTE(oam_priority_value));
  }
}

// Ancilla type 0x08: Door-debris chunks spawned when a bomb destroys a bombable
// wall or cracked door. Plays a 4-frame animation (arr25 = frame 0-3) at 8
// ticks per frame (arr26 countdown), then self-destructs.
void Ancilla08_DoorDebris(int k) {  // 889fb6
  DoorDebris_Draw(k);
  if (sign8(--ancilla_arr26[k])) {
    ancilla_arr26[k] = 7;
    if (++ancilla_arr25[k] == 4)
      ancilla_type[k] = 0;
  }
}

// Draws the 2-tile debris fragment for the door/wall explosion effect.
// door_debris_direction[k] (0=horizontal door, 1=vertical door) selects which
// half of kDoorDebris_CharFlags to use (16 entries per direction). Each of the
// 4 animation frames supplies 2 tiles (4 entries * 2 = indices j*2 and j*2+1).
// kDoorDebris_XY stores interleaved (y, x) offsets relative to the door_debris
// world position; kDoorDebris_CharFlags packs char (low byte) and flip flags
// (high byte) into one uint16 for compact storage.
void DoorDebris_Draw(int k) {  // 88a091
  static const uint16 kDoorDebris_XY[64] = {
     4,  7,  3, 17,  8,  8,  7, 17, 11,  7, 10, 16, 16,  7, 17, 17,
    20,  7, 21, 17, 16,  8, 17, 17, 13,  7, 14, 16,  8,  7,  7, 17,
     7,  4, 17,  3,  8,  8, 17,  7,  7, 11, 16, 10,  7, 16, 17, 17,
     7, 20, 17, 21,  8, 16, 17, 17,  7, 13, 16, 14,  7,  8, 17,  7,
  };
  static const uint16 kDoorDebris_CharFlags[32] = {
    0x205e, 0xe05e, 0xa05e, 0x605e, 0x204f, 0x204f, 0x204f, 0x204f, 0x605e, 0x605e, 0x205e, 0xe05e, 0x604f, 0x604f, 0x604f, 0x604f,
    0x205e, 0xe05e, 0xa05e, 0x605e, 0x204f, 0xe04f, 0x204f, 0x204f, 0x605e, 0x605e, 0x205e, 0xe05e, 0x604f, 0x604f, 0x604f, 0x604f,
  };
  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  int y = door_debris_y[k] - BG2VOFS_copy2;
  int x = door_debris_x[k] - BG2HOFS_copy2;
  // j = frame index (0-3) offset by direction*4 to pick the right table half.
  int j = ancilla_arr25[k] + door_debris_direction[k] * 4;

  for (int i = 0; i != 2; i++) {
    int t = j * 2 + i;
    uint16 d = kDoorDebris_CharFlags[t];
    Ancilla_SetOam(oam, x + kDoorDebris_XY[t * 2 + 1], y + kDoorDebris_XY[t * 2 + 0],
                   d, (d >> 8) & 0xc0 | HIBYTE(oam_priority_value), 0);
    oam = Ancilla_AllocateOamFromCustomRegion(oam + 1);
  }
}

// Ancilla type 0x09: Arrow in flight. Moves, checks sprite/tile collisions,
// then transitions to type 0x0A (ArrowInTheWall) on impact.
//
// ancilla_item_to_link[k] is a launch-delay counter: starts at some positive
// value and decrements each frame. While ≥ 4, the arrow is in its exit-bow
// delay (doesn't check collisions). Once it reaches 0 it wraps to 0xFF (signed
// underflow sentinel), after which the arrow flies normally.
//
// Silver arrow (link_item_bow & 4): spawns sparkle effects every other frame.
//
// Sprite collision: ancilla_S[k] records the hit sprite index.
//   Special case for sprite 0x65 (Agahnim in phase 2): if the sprite is in
//   state A==1 (vulnerable), play chime (SFX 0x2D) and increment hit counter
//   (byte_7E0B88); max 9 hits before the kill.
//
// Tile collision: kArrow_Y/X nudge the arrow one tile backward so it appears
//   to stick into the wall flush. ancilla_H[k] flags which floor plane was hit
//   (for two-floor dungeon scroll correction in Arrow_Draw).
//
// On any impact: transmute to type 0x0A, set aux_timer=1 to start the stuck
//   animation, play impact SFX 8. Agahnim (0x1B) is exempted from impact SFX.
void Ancilla09_Arrow(int k) {  // 88a131
  static const int8 kArrow_Y[4] = {-4, 2, 0, 0};
  static const int8 kArrow_X[4] = {0, 0, -4, 4};
  int j;

  if (submodule_index != 0) {
    Arrow_Draw(k);
    return;
  }

  if (!sign8(--ancilla_item_to_link[k])) {
    // Still in launch delay; once < 4, begin collision checks.
    if (ancilla_item_to_link[k] >= 4)
      return;
  } else {
    // Wrap-around: 0 → 0xFF, normal flight mode.
    ancilla_item_to_link[k] = 0xff;
  }
  Ancilla_MoveY(k);
  Ancilla_MoveX(k);
  if (link_item_bow & 4 && !(frame_counter & 1))
    AncillaAdd_SilverArrowSparkle(k);
  ancilla_S[k] = 255;  // Default: no sprite hit this frame.
  if ((j = Ancilla_CheckSpriteCollision(k)) >= 0) {
    // Record displacement from arrow to sprite for stuck-arrow tracking.
    ancilla_x_vel[k] = ancilla_x_lo[k] - sprite_x_lo[j];
    ancilla_y_vel[k] = ancilla_y_lo[k] - sprite_y_lo[j] + sprite_z[j];
    ancilla_S[k] = j;
    if (sprite_type[j] == 0x65) {
      // Agahnim: track multi-hit counter for the kill threshold.
      if (sprite_A[j] == 1) {
        sound_effect_2 = 0x2d;
        sprite_delay_aux2[j] = 0x80;
        sprite_delay_aux4[0] = 128;
        if (byte_7E0B88 < 9)
          byte_7E0B88++;
        sprite_B[j] = byte_7E0B88;
        sprite_G[j] += 1;
      } else {
        sprite_delay_aux3[j] = 4;
        byte_7E0B88 = 0;
      }
    } else {
      byte_7E0B88 = 0;
    }
  } else if ((j = Ancilla_CheckTileCollision(k)) != 0) {
    // j bit 1: which floor the wall was on (for scroll offset correction).
    ancilla_H[k] = j >> 1;
    j = ancilla_dir[k] & 3;
    // Nudge arrow back into the wall by 4 px so it looks embedded.
    Ancilla_SetX(k, Ancilla_GetX(k) + kArrow_X[j]);
    Ancilla_SetY(k, Ancilla_GetY(k) + kArrow_Y[j]);
    byte_7E0B88 = 0;
  } else {
    Arrow_Draw(k);
    return;
  }
  if (sprite_type[j] != 0x1b)
    Ancilla_Sfx2_Pan(k, 8);
  ancilla_item_to_link[k] = 0;
  ancilla_type[k] = 10;  // Transmute to ArrowInTheWall.
  ancilla_aux_timer[k] = 1;
  if (ancilla_H[k]) {
    // Correct position for the upper-floor BG offset delta.
    ancilla_x_lo[k] += BG1HOFS_copy2 - BG2HOFS_copy2;
    ancilla_y_lo[k] += BG1VOFS_copy2 - BG2VOFS_copy2;
  }
  Arrow_Draw(k);
}

// Draws the arrow sprite for both in-flight (type 9) and stuck (type 0xA) states.
//
// Tables are 48 entries each, organized in 3 banks of 16:
//   bank 0 (j 0-7): vertical arrows (up/down, 2 tiles each — head + shaft).
//   bank 1 (j 8-23): horizontal arrows (left/right, 2 tiles).
//   bank 2 (j 24-47): stuck-in-wall frames (ancilla_type == 0xA), 4 frames ×
//     4 directions × 2 tiles = 32 entries.
//
// Index j is computed as:
//   type 0xA (stuck): j = dir*4 + 8 + frame_sub_index (from item_to_link bits).
//   type 9 (flying):  j = dir; if item_to_link >= 0 (within launch delay), j |= 4
//                     to select the alternate launch-frame set.
//
// Entries with char 0xFF are skipped (single-tile arrows have no second tile).
//
// ancilla_H[k] != 0: arrow is embedded in the upper-floor BG; apply inverse BG
//   scroll delta to render at the correct screen position.
//
// Palette selection: silver arrow (link_item_bow & 4) → flags palette 2;
//   normal arrow → palette 4.
//
// Out-of-screen guard: if both OAM entries land at Y=0xF0 (hidden), kill the
//   ancilla (arrow has scrolled off screen).
void Arrow_Draw(int k) {  // 88a36e
  static const uint8 kArrow_Draw_Char[48] = {
    0x2b, 0x2a, 0x2a, 0x2b, 0x3d, 0x3a, 0x3a, 0x3d, 0x2b, 0xff, 0x2b, 0xff, 0x3d, 0xff, 0x3d, 0xff,
    0x3c, 0x2c, 0x3c, 0x2a, 0x3c, 0x2c, 0x3c, 0x2a, 0x2c, 0x3c, 0x2a, 0x3c, 0x2c, 0x3c, 0x2a, 0x3c,
    0x3b, 0x2d, 0x3b, 0x3a, 0x3b, 0x2d, 0x3b, 0x3a, 0x2d, 0x3b, 0x3a, 0x3b, 0x2d, 0x3b, 0x3a, 0x3b,
  };
  static const uint8 kArrow_Draw_Flags[48] = {
    0xa4, 0xa4, 0x24, 0x24, 0x64, 0x64, 0x24, 0x24, 0xa4, 0xff, 0x24, 0xff, 0x64, 0xff, 0x24, 0xff,
    0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xe4, 0xa4, 0xa4, 0x24, 0x24, 0x24, 0x24, 0x64, 0x24, 0x24, 0x24,
    0x64, 0x64, 0x64, 0xe4, 0x64, 0xe4, 0x64, 0xe4, 0x24, 0x24, 0x24, 0xa4, 0xa4, 0x24, 0x24, 0xa4,
  };
  static const int8 kArrow_Draw_Y[48] = {
    0,  8, 0, 8, 0, 0, 0, 0,  0,  0, 0, 0, 0, 0, 0, 0,
    0,  8, 0, 8, 0, 8, 0, 8,  0,  8, 0, 8, 0, 8, 0, 8,
    -1, -1, 0, 0, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 0, 0,
  };
  static const int8 kArrow_Draw_X[48] = {
    0, 0, 0, 0,  0,  8, 0, 8, 0, 0, 0, 0,  0,  0, 0, 0,
    1, 1, 0, 0, -1, -2, 0, 0, 1, 1, 0, 0, -2, -1, 0, 0,
    0, 8, 0, 8,  0,  8, 0, 8, 0, 8, 0, 8,  0,  8, 0, 8,
  };
  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  if (ancilla_objprio[k])
    HIBYTE(oam_priority_value) = 0x30;
  uint16 x = pt.x, y = pt.y;
  // Upper-floor correction: invert the BG1/BG2 scroll delta to map the arrow
  // into screen space correctly (note: axes are intentionally swapped here,
  // mirroring the original SNES assembly offset calculation).
  if (ancilla_H[k] != 0) {
    x += BG2VOFS_copy2 - BG1VOFS_copy2;
    y += BG2HOFS_copy2 - BG1HOFS_copy2;
  }
  uint8 r7 = ancilla_item_to_link[k];
  int j = ancilla_dir[k] & ~4;
  if (ancilla_type[k] == 0xa) {
    // Stuck arrow: select frame from item_to_link (bits 0-1 or bit 3 for final).
    j = j * 4 + 8 + ((r7 & 8) ? 1 : (r7 & 3));
  } else if (!sign8(r7)) {
    // Flying arrow within launch-delay window: use alternate launch-frame set.
    j |= 4;
  }

  j *= 2;

  OamEnt *oam = GetOamCurPtr(), *oam_org = oam;
  uint8 flags = (link_item_bow & 4) ? 2 : 4;  // Silver vs. normal palette.
  for (int i = 0; i != 2; i++, j++) {
    if (kArrow_Draw_Char[j] != 0xff) {
      Ancilla_SetOam(oam, x + kArrow_Draw_X[j], y + kArrow_Draw_Y[j],
                     kArrow_Draw_Char[j], kArrow_Draw_Flags[j] & ~0x3E | flags | HIBYTE(oam_priority_value), 0);
      oam++;
    }
  }

  // If both OAM slots were hidden (Y=0xF0), the arrow has left the viewport.
  if (oam_org[0].y == 0xf0 && oam_org[1].y == 0xf0)
    ancilla_type[k] = 0;
}

// Ancilla type 0x0A: Arrow stuck in a wall or embedded in a sprite.
//
// If ancilla_S[k] < 0x80 (valid sprite index), the arrow tracks that sprite's
// position every frame using the displacement (x_vel, y_vel) captured on
// impact, minus the sprite's Z height (so the arrow stays at body level even
// if the sprite is jumping). Arrow is killed if the sprite dies, gains
// invulnerability, or becomes a deflector.
//
// Animation: aux_timer counts down from 1 each frame; at 0 it resets to 2 and
// advances item_to_link (the stuck animation frame). At frame 9, kill the
// ancilla. Frame 8 (bit 3 set) sets aux_timer = 0x80 for a long final hold
// before fading.
void Ancilla0A_ArrowInTheWall(int k) {  // 88a45b
  int j = ancilla_S[k];
  if (!sign8(j)) {
    // Arrow is tracking a sprite: follow its world position.
    if (sprite_state[j] < 9 || sign8(sprite_z[j]) || sprite_ignore_projectile[j] || sprite_defl_bits[j] & 2) {
      ancilla_type[k] = 0;
      return;
    }
    Ancilla_SetX(k, Sprite_GetX(j) + (int8)ancilla_x_vel[k]);
    Ancilla_SetY(k, Sprite_GetY(j) + (int8)ancilla_y_vel[k] - sprite_z[j]);
  }
  if (submodule_index == 0 && --ancilla_aux_timer[k] == 0) {
    ancilla_aux_timer[k] = 2;
    if (++ancilla_item_to_link[k] == 9) {
      ancilla_type[k] = 0;
      return;
    } else if (ancilla_item_to_link[k] & 8) {
      ancilla_aux_timer[k] = 0x80;  // Long hold on penultimate frame.
    }
  }
  Arrow_Draw(k);
}

// Ancilla type 0x0B: Ice rod shot — a slow icy projectile that builds up over
// several frames before entering full flight. Two phases:
//
// Phase 1 (step == 0): charge-up. aux_timer counts down (period 3); on expiry,
//   item_to_link increments. When it exceeds 1 (hits non-0/1 value), the shot
//   is armed (step = 1) and item_to_link is masked to 4-7 range for the flight
//   animation frame selector.
//
// Phase 2 (step == 1): flight. The shot moves via Ancilla_MoveX/Y and checks
//   collisions. On hitting a sprite or tile, it transmutes to type 0x11
//   (IceRodWallHit) with aux_timer = 4 for the impact spread animation.
//
// Sparkles are emitted every frame (AncillaAdd_IceRodSparkle) in both phases.
void Ancilla0B_IceRodShot(int k) {  // 88a4dd
  if (submodule_index == 0) {
    if (sign8(--ancilla_aux_timer[k])) {
      if (++ancilla_item_to_link[k] & ~1) {
        // Armed: enter flight phase and select flight animation frame range.
        ancilla_step[k] = 1;
        ancilla_item_to_link[k] = ancilla_item_to_link[k] & 7 | 4;
      }
      ancilla_aux_timer[k] = 3;
    }
    if (ancilla_step[k]) {
      AncillaOamInfo info;
      if (Ancilla_ReturnIfOutsideBounds(k, &info))
        return;
      Ancilla_MoveY(k);
      Ancilla_MoveX(k);
      if (Ancilla_CheckSpriteCollision(k) >= 0 || Ancilla_CheckTileCollision(k)) {
        // Transmute to wall-hit spread animation.
        ancilla_type[k] = 0x11;
        ancilla_numspr[k] = kAncilla_Pflags[0x11];
        ancilla_item_to_link[k] = 0;
        ancilla_aux_timer[k] = 4;
      }
    }
  }
  AncillaAdd_IceRodSparkle(k);
}

// Ancilla type 0x11: Ice rod wall-hit spread. Plays 2 frames (item_to_link 0-1),
// each lasting 7 ticks, drawing the 4-tile spread pattern then self-destructs.
void Ancilla11_IceRodWallHit(int k) {  // 88a536
  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 7;
    if (++ancilla_item_to_link[k] == 2) {
      ancilla_type[k] = 0;
      return;
    }
  }
  IceShotSpread_Draw(k);
}

// Draws the 4-tile ice-spread burst for the ice rod wall-hit effect.
// Two animation frames indexed by item_to_link (0-1): frame 0 uses char 0xCF
// (compact cross), frame 1 uses 0xDF (expanded cross). Each frame places 4
// tiles at offsets (0,0),(0,8),(8,0),(8,8) and (–8,–8),(–8,+16),(+16,–8),(+16,+16)
// packed interleaved in kIceShotSpread_XY as (y,x) pairs.
void IceShotSpread_Draw(int k) {  // 88a571
  static const uint8 kIceShotSpread_CharFlags[16] = {0xcf, 0x24, 0xcf, 0x24, 0xcf, 0x24, 0xcf, 0x24, 0xdf, 0x24, 0xdf, 0x24, 0xdf, 0x24, 0xdf, 0x24};
  static const uint8 kIceShotSpread_XY[16] = {0, 0, 0, 8, 8, 0, 8, 8, 0xf8, 0xf8, 0xf8, 0x10, 0x10, 0xf8, 0x10, 0x10};

  Point16U info;
  Ancilla_PrepOamCoord(k, &info);

  Ancilla_AllocateOamFromRegion_A_or_D_or_F(k, ancilla_numspr[k]);
  OamEnt *oam = GetOamCurPtr();
  int j = ancilla_item_to_link[k] * 4;
  for (int i = 0; i != 4; i++, j++) {
    uint16 y = info.y + (int8)kIceShotSpread_XY[j * 2 + 0];
    uint16 x = info.x + (int8)kIceShotSpread_XY[j * 2 + 1];
    uint8 yv = 0xf0;
    if (x < 256 && y < 256) {
      oam->x = x;
      if (y < 224)
        yv = y;
    }
    oam->y = yv;
    oam->charnum = kIceShotSpread_CharFlags[j * 2 + 0];
    oam->flags = kIceShotSpread_CharFlags[j * 2 + 1] & ~0x30 | HIBYTE(oam_priority_value);
    bytewise_extended_oam[oam - oam_buf] = 0;
    oam = Ancilla_AllocateOamFromCustomRegion(oam + 1);
  }
  oam = GetOamCurPtr();
  if (oam[0].y == 0xf0 && oam[1].y == 0xf0)
    ancilla_type[k] = 0;
}

// Ancilla type 0x33: Bombos blast-wall explosion. Coordinates two ancilla slots
// (k=0 and k=1) that alternate triggering rows of fireballs as the blast wall
// sweeps across the screen. Used for the Bombos medallion ground-wave effect.
//
// blastwall_var5[k]: current explosion intensity phase (0=idle, 1-8=expanding,
//   9-10=contracting, 11=done). blastwall_var6[k]: per-phase tick countdown.
//
// When a slot's var5 > 0: decrement var6; on expiry, advance var5 and spawn a
//   new fireball row via AncillaAdd_BlastWallFireball if still in range 1-8.
//   At var5==11, reset to idle.
//
// When a slot is idle (var5==0): check if the *other* slot (k ^= 1) is at
//   var5==6 and var6==2, which signals time to start the next wave column. Up
//   to 6 waves total (ancilla_item_to_link[0]). Each new wave displaces the 4
//   blast points by ±13 px along the blast axis (blastwall_var7 < 4 selects Y,
//   else X) and plays the proximity sound effect.
//
// When ancilla_item_to_link[0] == 6 and both slots are idle, the animation ends
//   and flag_custom_spell_anim_active is cleared.
void Ancilla33_BlastWallExplosion(int k) {  // 88a60e
  if (submodule_index == 0) {
    if (blastwall_var5[k]) {
      if (--blastwall_var6[k] == 0) {
        if (++blastwall_var5[k] != 0 && blastwall_var5[k] < 9) {
          AncillaAdd_BlastWallFireball(0x32, 10, k * 4);
        }
        if (blastwall_var5[k] == 11) {
          blastwall_var5[k] = 0;
          blastwall_var6[k] = 0;
        } else {
          blastwall_var6[k] = 3;
        }
      }
    } else if ((k ^= 1), blastwall_var5[k] == 6 && blastwall_var6[k] == 2 && (uint8)(ancilla_item_to_link[0] + 1) < 7) {
      // Start the next wave column.
      ancilla_item_to_link[0]++;
      blastwall_var5[k] = 1;
      blastwall_var6[k] = 3;
      for (int i = 3; i >= 0; i--) {
        int8 arr[2] = { 0, 0 };
        int j = blastwall_var7 < 4 ? 1 : 0;
        arr[j] = (i & 2) ? -13 : 13;  // Alternate displacement per fireball pair.
        j = k * 4 + i;
        blastwall_var10[j] += arr[0];
        blastwall_var11[j] += arr[1];
        uint16 x = blastwall_var11[j] - BG2HOFS_copy2;
        if (x < 256)
          sound_effect_1 = kBombos_Sfx[x >> 5] | 0xc;
      }
    }
  }

  if (blastwall_var5[ancilla_K[0]]) {
    // Draw all active blast points for the slot referenced by ancilla_K[0].
    int i = (ancilla_K[0] == 1) ? 7 : 3;
    do {
      AncillaDraw_BlastWallBlast(ancilla_K[0], blastwall_var11[i], blastwall_var10[i]);
    } while ((--i & 3) != 3);
  }
  if (ancilla_item_to_link[0] == 6) {
    if (blastwall_var5[0] == 0 && blastwall_var5[1] == 0) {
      ancilla_type[0] = 0;
      ancilla_type[1] = 0;
      flag_custom_spell_anim_active = 0;
    }
  }
}

// Draws one blast-wall explosion tile cluster at world position (x, y).
// Delegates to AncillaDraw_Explosion using the bomb explosion table, selecting
// the frame set from blastwall_var5[k] via kBomb_Draw_Tab0/Tab2. Palette 0x32.
void AncillaDraw_BlastWallBlast(int k, int x, int y) {  // 88a756
  oam_priority_value = 0x3000;
  if (sort_sprites_setting)
    Oam_AllocateFromRegionD(0x18);
  else
    Oam_AllocateFromRegionA(0x18);
  OamEnt *oam = GetOamCurPtr();
  int i = blastwall_var5[k];
  AncillaDraw_Explosion(oam, kBomb_Draw_Tab0[i] * 6, 0, kBomb_Draw_Tab2[i], 0x32,
      x - BG2HOFS_copy2, y - BG2VOFS_copy2);
}

// Generic multi-tile explosion draw routine used by bombs, blast walls, and
// related effects. Iterates from idx to idx_end, emitting one OAM tile per step.
//
// Parameters:
//   oam      — starting OAM pointer (advanced and returned).
//   frame    — starting tile-set index into kBomb_DrawExplosion_CharFlags (char+flags).
//   idx      — starting index into kBomb_DrawExplosion_XY for position offsets.
//   idx_end  — exclusive end of the idx range (number of tiles to draw).
//   r11      — palette bits OR'd into the flags byte (e.g. 0x32 for Bombos palette).
//   x, y     — screen-space centre coordinates.
//
// Tiles with char 0xFF are skipped (sparse frames). kBomb_DrawExplosion_Ext
// provides the OAM size/priority extension byte (0=8×8, 1=16×16, 2=16×16 big).
// frame and idx advance together but base_frame anchors the position lookup to
// the original frame to keep the position table aligned correctly.
OamEnt *AncillaDraw_Explosion(OamEnt *oam, int frame, int idx, int idx_end, uint8 r11, int x, int y) {  // 88a7ab
  static const int8 kBomb_DrawExplosion_XY[108] = {
     -8,  -8,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  -8,  -8,  -8,  0,
      0,  -8,   0,   0,   0,   0,   0,   0, -16, -16, -16,  0,   0, -16,   0,  0,
      0,   0,   0,   0, -16, -16, -16,   0,   0, -16,   0,  0,   0,   0,   0,  0,
     -8,  -8, -21, -22, -21,   8,   9, -22,   9,   8,   0,  0,  -6, -15,   0, -1,
    -16,  -2,  -8,  -7,   0,   0,   0,   0,  -9,  -4, -21, -5, -12, -18, -11,  7,
      0, -15,   4,  -2,  -9,  -4, -22,  -5, -13, -20, -11,  8,   1, -16,   5, -2,
    -20,   4, -12, -19,  -9,  16,  -5,  -2,   2,  -9,  10,  6,
  };
  static const uint8 kBomb_DrawExplosion_CharFlags[108] = {
    0x6e, 0x26, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x8c, 0x22, 0x8c, 0x62,
    0x8c, 0xa2, 0x8c, 0xe2, 0xff, 0xff, 0xff, 0xff, 0x84, 0x22, 0x84, 0x62, 0x84, 0xa2, 0x84, 0xe2,
    0xff, 0xff, 0xff, 0xff, 0x88, 0x22, 0x88, 0x62, 0x88, 0xa2, 0x88, 0xe2, 0xff, 0xff, 0xff, 0xff,
    0x86, 0x22, 0x88, 0x22, 0x88, 0x62, 0x88, 0xa2, 0x88, 0xe2, 0xff, 0xff, 0x86, 0x22, 0x86, 0x62,
    0x86, 0xe2, 0x86, 0xe2, 0xff, 0xff, 0xff, 0xff, 0x86, 0xe2, 0x86, 0x22, 0x86, 0x22, 0x86, 0x62,
    0x86, 0xa2, 0x86, 0xa2, 0x8a, 0xa2, 0x8a, 0x62, 0x8a, 0x22, 0x8a, 0x62, 0x8a, 0x62, 0x8a, 0xe2,
    0x9b, 0x22, 0x9b, 0xa2, 0x9b, 0x62, 0x9b, 0xe2, 0x9b, 0xa2, 0x9b, 0x22,
  };
  static const uint8 kBomb_DrawExplosion_Ext[54] = {
    2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 2, 2, 2, 2,
    1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 2, 2, 2, 1, 2, 2,
    2, 2, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    0, 0, 0, 0, 0, 0,
  };
  int base_frame = frame;
  do {
    if (kBomb_DrawExplosion_CharFlags[frame * 2] != 0xff) {
      int i = idx + base_frame;
      Ancilla_SetOam_Safe(oam, x + kBomb_DrawExplosion_XY[i * 2 + 1], y + kBomb_DrawExplosion_XY[i * 2 + 0],
                          kBomb_DrawExplosion_CharFlags[frame * 2],
                          kBomb_DrawExplosion_CharFlags[frame * 2 + 1] & ~0x3E | HIBYTE(oam_priority_value) | r11,
                          kBomb_DrawExplosion_Ext[frame]);
      oam++;
    }
  } while (frame++, ++idx != idx_end);
  return oam;
}

// Ancilla type 0x15: Water splash when Link enters or exits a pool.
// Plays a 2-frame animation: frame 0 = initial ring (char 0xAC), frame 1 =
// expanded ring (char 0xAE). The splash also expands outward (velocity decreases
// by 4 each frame from a base of ~248, wrapping via uint8 arithmetic to go
// toward 232; below 232 the splash is done).
//
// Three OAM tiles are drawn: two mirror-image half-rings (one left-flipped via
// flags 0x40), and a small centre circle (char 0xC0) using x6 = ancilla_x + 12.
// x8 = link_x*2 - ancilla_x maps the left ring to the mirror position on the
// other side of Link's jump-entry point.
//
// On termination while swimming/bunny in deep water, CheckAbilityToSwim() is
// called to re-evaluate Link's swim state after the splash.
void Ancilla15_JumpSplash(int k) {  // 88a80f
  static const uint8 kAncilla_JumpSplash_Char[2] = {0xac, 0xae};

  if (!submodule_index) {
    if (sign8(--ancilla_aux_timer[k])) {
      ancilla_aux_timer[k] = 0;
      ancilla_item_to_link[k] = 1;  // Switch to expansion frame.
    }
    if (ancilla_item_to_link[k]) {
      // Accelerate outward expansion; terminate when velocity wraps below 232.
      ancilla_x_vel[k] = ancilla_y_vel[k] = ancilla_y_vel[k] - 4;
      if (ancilla_y_vel[k] < 232) {
        ancilla_type[k] = 0;
        if ((link_is_bunny_mirror || link_player_handler_state == kPlayerState_Swimming) && link_is_in_deep_water)
          CheckAbilityToSwim();
        return;
      }
      Ancilla_MoveX(k);
      Ancilla_MoveY(k);
    }
  }
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  int ax = Ancilla_GetX(k);
  // x8: mirrored ring position (reflected over Link's entry X).
  int x8 = link_x_coord * 2 - ax - BG2HOFS_copy2;
  // x6: small centre circle, 12 px right of the splash origin.
  int x6 = ax + 12 - BG2HOFS_copy2;
  int j = ancilla_item_to_link[k];
  uint8 flags = 0;
  for (int i = 0; i < 2; i++) {
    Ancilla_SetOam(oam, pt.x, pt.y, kAncilla_JumpSplash_Char[j], 0x24 | flags, 2);
    oam = Ancilla_AllocateOamFromCustomRegion(oam + 1);
    pt.x = x8;
    flags = 0x40;  // Flip the second ring horizontally.
  }
  Ancilla_SetOam(oam, x6, pt.y, 0xc0, 0x24, ((j == 1) ? 1 : 2));
}

// Ancilla type 0x16: Hit-stars — the two sparks Link sees when taking damage.
// Mirrors the JumpSplash expansion mechanic but uses star tiles (0x90/0x91)
// and delays 1 frame via arr3 before starting (arr3 countdown, returns early
// while still counting). After the initial delay, expands and fades like a
// splash. Draws two stars: the left one at the computed position and a second
// at the mirror X (computed via the same link_x_coord*2 formula). A third
// tile (0xB8) is drawn at x6 for the central impact mark.
// ancilla_step[k] == 2 forces OAM region B/E allocation for upper-floor render.
void Ancilla16_HitStars(int k) {  // 88a8e5
  static const uint8 kAncilla_HitStars_Char[2] = {0x90, 0x91};

  if (!sign8(--ancilla_arr3[k]))
    return;

  ancilla_arr3[k] = 0;
  if (!submodule_index) {
    if (sign8(--ancilla_aux_timer[k])) {
      ancilla_aux_timer[k] = 0;
      ancilla_item_to_link[k] = 1;
    }
    if (ancilla_item_to_link[k]) {
      ancilla_x_vel[k] = (ancilla_y_vel[k] -= 4);
      if (ancilla_y_vel[k] < 232) {
        ancilla_type[k] = 0;
        return;
      }
      Ancilla_MoveY(k);
      Ancilla_MoveX(k);
    }
  }
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);

  uint16 ax = Ancilla_GetX(k);
  uint16 tt = ancilla_B[k] << 8 | ancilla_A[k];

  uint16 r8 = 2 * tt - ax - 8 - BG2HOFS_copy2;

  if (ancilla_step[k] == 2)
    Ancilla_AllocateOamFromRegion_B_or_E(8);

  OamEnt *oam = GetOamCurPtr();
  uint16 x = info.x, y = info.y;
  uint8 flags = 0;
  for (int i = 1; i >= 0; i--) {
    Ancilla_SetOam(oam, x, y,
                   kAncilla_HitStars_Char[ancilla_item_to_link[k]],
                   HIBYTE(oam_priority_value) | 4 | flags, 0);
    flags = 0x40;
    BYTE(x) = r8;
    oam = HitStars_UpdateOamBufferPosition(oam + 1);
  }
}

// Ancilla type 0x17: Shovel-dirt cloud — the 2-tile puff that appears when Link
// digs with the shovel. Plays 2 frames (item_to_link 0-1), each lasting 8 ticks
// (ancilla_timer countdown). Draws two adjacent tiles (char 0x40+0x41 for frame
// 0, 0x50+0x51 for frame 1).
//
// kShovelDirt_XY[8] gives (y, x) offsets indexed by b + (facing==4 ? 0 : 2):
//   facing==4 (south): offsets 0-1 (first two pairs).
//   other directions: offsets 2-3 (last two pairs — shifted positions).
void Ancilla17_ShovelDirt(int k) {  // 88a9a9
  static const int8 kShovelDirt_XY[8] = {18, -13, -9, 4, 18, 13, -9, -11};
  static const int8 kShovelDirt_Char[2] = {0x40, 0x50};
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  if (!ancilla_timer[k]) {
    ancilla_timer[k] = 8;
    if (++ancilla_item_to_link[k] == 2) {
      ancilla_type[k] = 0;
      return;
    }
  }
  int b = ancilla_item_to_link[k];
  int j = b + ((link_direction_facing == 4) ? 0 : 2);
  pt.x += kShovelDirt_XY[j * 2 + 1];
  pt.y += kShovelDirt_XY[j * 2 + 0];
  for (int i = 0; i < 2; i++) {
    Ancilla_SetOam(oam, pt.x + i * 8, pt.y, kShovelDirt_Char[b] + i, 4 | HIBYTE(oam_priority_value), 0);
    oam = Ancilla_AllocateOamFromCustomRegion(oam + 1);
  }
}

// Ancilla type 0x32: Blast-wall fireball — a single arcing flame shot spawned
// by the Bombos blast-wall effect. Accelerates downward each frame (y_vel +=
// item_to_link, which itself increases by 2 per frame for parabolic motion).
// blastwall_var12[k] is a life-counter; when it goes negative the fireball dies.
//
// Three animation chars selected from blastwall_var12: if bit 3 is set (upper
// half of lifetime) → large flame 0x9D; if bit 2 is set → medium 0x9C;
// otherwise → small spark 0x8D. This gives a natural large→small shrink.
void Ancilla32_BlastWallFireball(int k) {  // 88aa35
  static const uint8 kBlastWallFireball_Char[3] = {0x9d, 0x9c, 0x8d};

  if (!submodule_index) {
    ancilla_item_to_link[k] += 2;
    ancilla_y_vel[k] += ancilla_item_to_link[k];
    Ancilla_MoveY(k);
    Ancilla_MoveX(k);
    if (sign8(--blastwall_var12[k])) {
      ancilla_type[k] = 0;
      return;
    }
  }

  if (sort_sprites_setting)
    Oam_AllocateFromRegionD(4);
  else
    Oam_AllocateFromRegionA(4);

  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  Ancilla_SetOam(GetOamCurPtr(), pt.x, pt.y, kBlastWallFireball_Char[blastwall_var12[k] & 8 ? 0 : blastwall_var12[k] & 4 ? 1 : 2], 0x22, 0);
}

// Ancilla type 0x18: Ether Medallion spell controller. Drives the full Ether
// animation sequence through 6 steps:
//
//   step 0 (LightningStroke): The Ether bolt descends from the top of the
//     screen; moves via Ancilla_MoveY; when it reaches ether_y2 (the floor
//     row), transitions to step 1.
//
//   step 1 (OrbPulse): The bolt compresses into a pulsing orb; ancilla_arr25
//     and arr3 count down the pulse phases. Once expired, if the spin-attack
//     counter is active, calls Medallion_CheckSpriteDamage. Transitions to
//     step 2 (radial burst).
//
//   step 2 (RadialSpin expand): 8 orbs spin outward from the impact; x_vel
//     increases by 1 each frame to accelerate the expansion. When item_to_link
//     reaches 2, transitions to step 3.
//
//   step 3 (RadialSpin cruise): Orbs continue at cruise speed; when the radius
//     (ether_var2) reaches 0x40, transitions to step 4.
//
//   step 4 (RadialSpin with SFX + damage): Full spin at max radius. Plays
//     rotating SFX (0x2A/0xAA/0x6A); ether_var1 counts down, then step 5.
//
//   step 5 (Fade-out): x_vel ramps toward 0x7F (fade radius); when all 8 orb
//     OAM entries are Y=0xF0, the spell ends.
//
// Palette flickering: during steps 1-5, Palette_ElectroThemedGear flickers
//   the screen white on every 4th frame (arr4 & 4 == 0) or at spin step 11.
//
// Termination: clears Link's spin-attack state, re-enables direction control,
//   and checks for the Ether-triggered waterfall entrance (overworld 0x70).
void Ancilla18_EtherSpell(int k) {  // 88aaa0
  if (submodule_index)
    return;

  if (ancilla_step[k] != 0) {
    uint8 flag;

    // Flicker logic: every 4 frames (arr4 & 4 == 0) or at spin step 11.
    if (step_counter_for_spin_attack == 0) {
      flag = (++ancilla_arr4[k] & 4) == 0;
    } else {
      flag = step_counter_for_spin_attack == 11;
    }
    if (flag) {
      Palette_ElectroThemedGear();
      Filter_Majorly_Whiten_Bg();
    } else {
      LoadActualGearPalettes();
      Palette_Restore_BG_From_Flash();
    }
  }

  if (ancilla_step[k] == 2) {
    // Radial expansion: grow x_vel to push orbs outward; switch to cruise at 2 frames.
    if (sign8(--ancilla_aux_timer[k])) {
      ancilla_aux_timer[k] = 2;
      if (++ancilla_item_to_link[k] == 2) {
        ancilla_item_to_link[k]--;
        ancilla_x_vel[k] = 16;
        ancilla_step[k] = 3;
      }
    }
    ancilla_x_vel[k] += 1;
    EtherSpell_HandleRadialSpin(k);
    return;
  } else {
    // Toggle between two draw sub-frames (item_to_link 0/1) for animation blinking.
    if (sign8(--ancilla_aux_timer[k])) {
      ancilla_aux_timer[k] = 2;
      ancilla_item_to_link[k] ^= 1;
    }
    if (ancilla_step[k] == 0) {
      EtherSpell_HandleLightningStroke(k);
    } else if (ancilla_step[k] == 1) {
      EtherSpell_HandleOrbPulse(k);
    } else if (ancilla_step[k] == 3) {
      EtherSpell_HandleRadialSpin(k);
    } else if (ancilla_step[k] == 4) {
      if (!--ether_var1)
        ancilla_step[k] = 5;
      EtherSpell_HandleRadialSpin(k);
    } else {
      // Step 5: fade out by expanding radius until orbs leave the screen.
      uint8 vel = ancilla_x_vel[k] + 0x10;
      if (sign8(vel)) vel = 0x7f;
      ancilla_x_vel[k] = vel;
      EtherSpell_HandleRadialSpin(k);
    }
  }
}

// Step 0 of the Ether spell: moves the bolt downward (Ancilla_MoveY) and draws
// the full-screen lightning column (AncillaDraw_EtherBlitz). Each time the bolt
// crosses a new 16-px tile row (y & 0xF0 changes), arr25 is incremented to
// extend the drawn lightning column. When the bolt reaches ether_y2 (the floor
// boundary), transitions to step 1 (OrbPulse).
void EtherSpell_HandleLightningStroke(int k) {  // 88ab63
  Ancilla_MoveY(k);
  uint16 y = Ancilla_GetY(k);

  if (BYTE(ether_y_adjusted) != (y & 0xf0)) {
    BYTE(ether_y_adjusted) = y & 0xf0;
    ancilla_arr25[k]++;  // Extend drawn column by one row.
  }
  if (y < 0xe000 && ether_y2 < 0xe000 && ether_y2 <= y) {
    ancilla_step[k] = 1;
  }
  AncillaDraw_EtherBlitz(k);
}

// Step 1 of the Ether spell: the lightning column compresses into a central
// orb, then pulses. arr25 counts remaining pulse repeats; arr3 is the per-pulse
// tick timer. The pulse sequence counts down arr3 → arr25 → arr3 = 9 (final hold).
// When arr3 underflows, transitions to step 2 and optionally damages sprites
// (Medallion_CheckSpriteDamage) if the spin-attack countdown was active.
void EtherSpell_HandleOrbPulse(int k) {  // 88aba7
  if (!sign8(ancilla_arr25[k])) {
    if (!sign8(--ancilla_arr3[k])) {
      AncillaDraw_EtherBlitz(k);
      return;
    }
    ancilla_arr3[k] = 3;
    if (!sign8(--ancilla_arr25[k])) {
      AncillaDraw_EtherBlitz(k);
      return;
    }
    ancilla_arr3[k] = 9;  // Final hold before radial burst.
  }
  if (sign8(--ancilla_arr3[k])) {
    // Transition to radial spin (step 2).
    ancilla_step[k] = 2;
    ancilla_y_vel[k] = 0;
    ancilla_x_vel[k] = 16;
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 2;
    if (step_counter_for_spin_attack)
      Medallion_CheckSpriteDamage(k);
  }
  AncillaDraw_EtherOrb(k, GetOamCurPtr());
}

// Steps 2-5 of the Ether spell: 8 orbs spin around the impact centre at a
// radius stored in ether_var2, which grows until it reaches 0x40 (step 4).
//
// ether_arr1[i] tracks the angular phase of each orb (0-63 circular table);
// advanced by 1 per frame unless frozen (steps 2 and 5). AncillaDraw_EtherBlitzBall
// draws a single ball tile; AncillaDraw_EtherBlitzSegment draws the arc segment
// tail for step 2.
//
// Termination guard: if ether_var2 >= 0xF0 (fully expanded) and all 8 OAM
// entries are off-screen (Y=0xF0), the spell ends and all state is cleared.
// Special: overworld screen 0x70 with Ether triggers the waterfall entrance.
void EtherSpell_HandleRadialSpin(int k) {  // 88abef
  if (ancilla_step[k] == 4) {
    // Step 4: play rotating 3-phase SFX pattern.
    if ((frame_counter & 7) == 0)
      sound_effect_2 = 0x2a;
    else if ((frame_counter & 7) == 4)
      sound_effect_2 = 0xaa;
    else if ((frame_counter & 7) == 7)
      sound_effect_2 = 0x6a;
  } else {
    // Advance the radius: uses Ancilla_MoveX with x_lo=ether_var2 to apply
    // x_vel as a fixed-point increment.
    ancilla_x_lo[k] = ether_var2;
    ancilla_x_hi[k] = 0;
    Ancilla_MoveX(k);
    ether_var2 = ancilla_x_lo[k];
    if (ether_var2 == 0x40)
      ancilla_step[k] = 4;
  }

  uint8 sb = ancilla_step[k];
  uint8 sa = ancilla_item_to_link[k];
  OamEnt *oam = GetOamCurPtr();
  for (int i = 7; i >= 0; i--) {
    if (sb != 2 && sb != 5) {
      ether_arr1[i] = (ether_arr1[i] + 1) & 0x3f;
    }
    AncillaRadialProjection arp = Ancilla_GetRadialProjection(ether_arr1[i], ether_var2);
    if (sb != 2)
      oam = AncillaDraw_EtherBlitzBall(oam, &arp, sa);
    else
      oam = AncillaDraw_EtherBlitzSegment(oam, &arp, sa, i);
  }
  // Check termination: all orbs must have left the screen.
  if (ether_var2 < 0xf0) {
    OamEnt *oam = GetOamCurPtr();
    for (int i = 0; i != 8; i++) {
      if (oam[i].y != 0xf0)
        return;
    }
  }
  ancilla_type[k] = 0;
  load_chr_halfslot_even_odd = 1;
  byte_7E0324 = 0;
  state_for_spin_attack = 0;
  step_counter_for_spin_attack = 0;
  link_cant_change_direction = 0;
  flag_unk1 = 0;

  // Waterfall entrance trigger on overworld screen 0x70.
  if (BYTE(overworld_screen_index) == 0x70 && !(save_ow_event_info[0x70] & 0x20) && Ancilla_CheckForEntranceTrigger(2)) {
    trigger_special_entrance = 3;
    subsubmodule_index = 0;
    BYTE(R16) = 0;
  }

  if (link_player_handler_state != kPlayerState_ReceivingEther) {
    link_player_handler_state = kPlayerState_Ground;
    link_delay_timer_spin_attack = 0;
    button_mask_b_y = button_b_frames ? (joypad1H_last & 0x80) : 0;
  }
  link_speed_setting = 0;
  byte_7E0325 = 0;
  LoadActualGearPalettes();
  Palette_Restore_BG_And_HUD();
}

// Draws one of the 8 radially-placed Ether orb tiles at step 3/4/5. Position
// is computed from the AncillaRadialProjection (radius × angle via sine table),
// offset from the impact centre (ether_x2, ether_y3). s selects between two
// char variants (0x68 = solid orb, 0x6A = dim orb) for the blink animation.
OamEnt *AncillaDraw_EtherBlitzBall(OamEnt *oam, const AncillaRadialProjection *arp, int s) {  // 88aced
  static const uint8 kEther_BlitzBall_Char[2] = {0x68, 0x6a};
  int x = (arp->r6 ? -arp->r4 : arp->r4) + ether_x2 - 8 - BG2HOFS_copy2;
  int y = (arp->r2 ? -arp->r0 : arp->r0) + ether_y3 - 8 - BG2VOFS_copy2;
  Ancilla_SetOam(oam, x, y, kEther_BlitzBall_Char[s], 0x3c, 2);
  return Ancilla_AllocateOamFromCustomRegion(oam + 1);
}

// Draws one of the 8 splitting blitz segments at step 2 (radial burst). Each
// segment is two OAM tiles: a head tile (at the projected position) and a tail
// tile offset by kEther_SpllittingBlitzSegment_X/Y[t]. t = s*8 + k encodes the
// animation sub-frame (s=0/1) and the orb index (k=0-7). The tail offset and
// char/flags give each orb a distinct directional arc appearance.
OamEnt *AncillaDraw_EtherBlitzSegment(OamEnt *oam, const AncillaRadialProjection *arp, int s, int k) {  // 88adc9
  static const int8 kEther_SpllittingBlitzSegment_X[16] = {-8, -16, -24, -16, -8, 0, 8, -16, -8, -16, -24, -16, -8, 0, 8, 0};
  static const int8 kEther_SpllittingBlitzSegment_Y[16] = {8, 0, -8, -16, -24, -16, -8, -16, 8, 0, -8, -16, -24, -16, -8, 0};
  static const uint8 kEther_SpllittingBlitzSegment_Char[32] = {
    0x40, 0x42, 0x66, 0x64, 0x62, 0x60, 0x64, 0x66, 0x42, 0x40, 0x66, 0x64, 0x60, 0x62, 0x64, 0x66,
    0x68, 0x42, 0x68, 0x64, 0x68, 0x60, 0x68, 0x64, 0x68, 0x40, 0x68, 0x66, 0x68, 0x62, 0x68, 0x64,
  };
  static const uint8 kEther_SpllittingBlitzSegment_Flags[32] = {
    0x3c, 0x3c, 0xfc, 0xfc, 0x3c, 0x3c, 0xbc, 0xbc, 0x3c, 0x3c, 0x3c, 0x3c, 0x3c, 0x3c, 0x7c, 0x7c,
    0x3c, 0x7c, 0x3c, 0x3c, 0x3c, 0xbc, 0x3c, 0x7c, 0x3c, 0x7c, 0x3c, 0xfc, 0x3c, 0xbc, 0x3c, 0xbc,
  };
  int x = (arp->r6 ? -arp->r4 : arp->r4);
  int y = (arp->r2 ? -arp->r0 : arp->r0);
  int t = s * 8 + k;
  Ancilla_SetOam(oam, x + ether_x2 - 8 - BG2HOFS_copy2, y + ether_y3 - 8 - BG2VOFS_copy2,
                 kEther_SpllittingBlitzSegment_Char[t * 2], kEther_SpllittingBlitzSegment_Flags[t * 2], 2);
  Ancilla_SetOam(oam + 1,
      x + ether_x2 + kEther_SpllittingBlitzSegment_X[t] - BG2HOFS_copy2,
      y + ether_y3 + kEther_SpllittingBlitzSegment_Y[t] - BG2VOFS_copy2,
      kEther_SpllittingBlitzSegment_Char[t * 2 + 1],
      kEther_SpllittingBlitzSegment_Flags[t * 2 + 1], 2);
  return Ancilla_AllocateOamFromCustomRegion(oam + 2);
}

// Draws the full-height lightning column as a stack of 16-px segment tiles.
// ancilla_arr25[k] is the number of rows to draw (extends by 1 each time the
// bolt crosses a new tile row during step 0). Each row alternates between two
// char variants (t*2 + m, m toggles 0/1) from kEther_BlitzSegment_Char for a
// two-frame blink. If in step 1 (OrbPulse), also draws the central orb.
void AncillaDraw_EtherBlitz(int k) {  // 88ae87
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);
  OamEnt *oam = GetOamCurPtr();
  int t = ancilla_item_to_link[k];
  int i = ancilla_arr25[k];
  int m = 0;
  do {
    Ancilla_SetOam(oam, info.x, info.y,
                   kEther_BlitzSegment_Char[t * 2 + m],
                   kEther_BlitzOrb_Flags[0] | HIBYTE(oam_priority_value), 2);
    info.y -= 16;
    oam++;
    m ^= 1;
  } while (--i >= 0);
  if (ancilla_step[k] == 1)
    AncillaDraw_EtherOrb(k, oam);
}

// Draws the 4-tile central impact orb for the Ether OrbPulse phase. The orb
// is a 2×2 grid of 16-px tiles placed at ether_x/y (the impact point). The
// char and flags are selected from kEther_BlitzOrb_Char/Flags indexed by
// item_to_link*4 + i (4 tiles per animation sub-frame).
void AncillaDraw_EtherOrb(int k, OamEnt *oam) {  // 88aedd
  uint16 y = ether_y - 1 - BG2VOFS_copy2;
  uint16 x = ether_x - 8 - BG2HOFS_copy2;
  int t = ancilla_item_to_link[k] * 4;

  for (int i = 0; i < 4; i++) {
    Ancilla_SetOam(oam, x, y, kEther_BlitzOrb_Char[t + i], kEther_BlitzOrb_Flags[t + i], 2);
    oam++;
    oam = Ancilla_AllocateOamFromCustomRegion(oam);
    x += 16;
    if (i == 1)
      x -= 32, y += 16;  // After 2 tiles, wrap to second row.
  }
}

// Spawns the Bombos Medallion spell ancilla and initialises all Bombos state.
// Allocates 10 fire-column slots (bombos_arr2/1) and 8 blast-ball slots
// (bombos_arr3/4), all starting at phase 0 with period 3.
//
// bombos_var4: main phase gate (0=fire columns, 1=transition, 2=blasting).
// bombos_var3: initial angular phase offset (0x80 = 180°).
// bombos_arr7[0] = 0x10: starting angle for the first ring of blasts.
//
// The initial fire-column position is taken from kGeneratedBombosArr[frame_counter]
// (a pre-baked pseudo-random table), clamped to < 0xE0, and stored as the low
// byte of the column position (upper byte from Link's coordinates).
//
// The blast-ball starting position is computed via Ancilla_GetRadialProjection
// at radius 16 and stored in bombos_x/y_lo/hi for subsequent frame tracking.
void AncillaAdd_BombosSpell(uint8 a, uint8 y) {  // 88af66
  int k = AncillaAdd_AddAncilla_Bank08(a, y);
  if (k < 0)
    return;
  for (int i = 0; i < 10; i++) {
    bombos_arr2[i] = 0;
    bombos_arr1[i] = 3;
  }
  for (int i = 0; i < 8; i++) {
    bombos_arr3[i] = 0;
    bombos_arr4[i] = 3;
  }
  bombos_var4 = 0;
  bombos_var2 = 0;
  bombos_var3 = 0x80;
  bombos_arr7[0] = 0x10;
  load_chr_halfslot_even_odd = 11;
  flag_custom_spell_anim_active = 1;
  ancilla_step[k] = 0;
  ancilla_item_to_link[k] = 0;
  Ancilla_Sfx2_Near(0x2a);

  // Pseudo-random column start position, clamped to avoid the far edge.
  uint8 t = kGeneratedBombosArr[frame_counter];
  t = (t < 0xe0) ? t : t & 0x7f;
  bombos_x_coord[0] = link_x_coord & ~0xff | t;
  bombos_y_coord[0] = link_y_coord & ~0xff | t;

  static const int16 kBombos_YDelta[4] = {16, 24, -128, -16};
  static const int16 kBombos_XDelta[4] = {-16, -128, 0, 128};

  for (int i = 0; i < 1 ; i++) {
    bombos_x_coord2[i] = link_x_coord + kBombos_XDelta[i];
    bombos_y_coord2[i] = link_y_coord + kBombos_YDelta[i];
    bombos_var1 = 16;
    AncillaRadialProjection arp = Ancilla_GetRadialProjection(bombos_arr7[i], 16);
    int x = (arp.r6 ? -arp.r4 : arp.r4) + bombos_x_coord2[i];
    int y = (arp.r2 ? -arp.r0 : arp.r0) + bombos_y_coord2[i];
    bombos_x_lo[i] = (uint8)x;
    bombos_x_hi[i] = x >> 8;
    bombos_y_lo[i] = (uint8)y;
    bombos_y_hi[i] = y >> 8;
  }
}

// Ancilla type 0x19: Bombos Medallion spell main dispatcher. Three phases
// gated by bombos_var4:
//
//   0 (FireColumns): 10 independent fire-column slots animate upward; each
//     slot plays a 13-frame column rise via BombosSpell_ControlFireColumns.
//
//   1 (FinishFireColumns): transition — columns continue to animate while the
//     blast setup begins via BombosSpell_FinishFireColumns.
//
//   2 (Blasting): ground explosions radiate outward; ancilla_step[k] tracks
//     how many active blast balls to draw (BombosSpell_ControlBlasting iterates
//     them). Drawing is done by AncillaDraw_BombosBlast for each slot.
//
// During submodule_index != 0 (paused), only drawing runs (no logic updates).
void Ancilla19_BombosSpell(int k) {  // 88b0ce
  if (bombos_var4 == 0) {
    if (submodule_index == 0) {
      BombosSpell_ControlFireColumns(k);
      return;
    }
    for (int i = 9; i >= 0; i--)
      AncillaDraw_BombosFireColumn(i);
  } else if (bombos_var4 != 2) {
    if (submodule_index == 0) {
      BombosSpell_FinishFireColumns(k);
      return;
    }
    for (int i = 9; i >= 0; i--)
      AncillaDraw_BombosFireColumn(i);
  } else {
    if (submodule_index == 0) {
      BombosSpell_ControlBlasting(k);
      return;
    }
    int i = ancilla_step[k];
    do {
      AncillaDraw_BombosBlast(i);
    } while (--i >= 0);
  }
}

// Controls the 10-column fire-rise phase of the Bombos spell. Each slot
// independently animates through 13 frames (bombos_arr2) at period 3
// (bombos_arr1). When a column reaches frame 2 and no column is already
// launching (sa == 0), a new column slot is allocated at the next angular
// position on the ring (bombos_arr7 += 6, radius grows toward 207). Position is
// computed via Ancilla_GetRadialProjection and stored in bombos_x/y_lo/hi.
// When bombos_arr7 reaches >= 0x80 (half-circle), transitions to FinishFireColumns.
void BombosSpell_ControlFireColumns(int k) {  // 88b10a
  uint8 sa = ancilla_item_to_link[k];
  uint8 sb = ancilla_step[k];

  int j, i = sb;
  do {
    if (bombos_arr2[i] == 13)
      continue;

    if (sign8(--bombos_arr1[i])) {
      bombos_arr1[i] = 3;
      if (++bombos_arr2[i] == 13)
        continue;

      if (bombos_arr2[i] == 2) {
        if (sa)
          continue;

        // pushed x
        if (sb == 9) {
          for (j = 9; j >= 0; j--) {
            if (bombos_arr2[j] == 13) {
              bombos_arr2[j] = 0;
              goto exit_loop;
            }
          }
        }
        sb = j = (sb + 1) != 10 ? sb + 1 : 9;
exit_loop:
        bombos_var1 = (bombos_var1 + 3 >= 207) ? 207 : bombos_var1 + 3;
        bombos_arr7[0] += 6;
        AncillaRadialProjection arp = Ancilla_GetRadialProjection(bombos_arr7[0] & 0x3f, bombos_var1);
        int x = (arp.r6 ? -arp.r4 : arp.r4) + bombos_x_coord2[0];
        int y = (arp.r2 ? -arp.r0 : arp.r0) + bombos_y_coord2[0];
        bombos_x_lo[j] = (uint8)x;
        bombos_x_hi[j] = x >> 8;
        bombos_y_lo[j] = (uint8)y;
        bombos_y_hi[j] = y >> 8;

        uint16 t = x - BG2HOFS_copy2 + 8;
        if (t < 256)
          sound_effect_1 = kBombos_Sfx[t >> 5] | 0x2a;
      }
    }
    AncillaDraw_BombosFireColumn(i);

  } while (--i >= 0);
  if (bombos_arr7[0] >= 0x80)
    bombos_var4 = 1;
  ancilla_step[k] = sb;
}

// Waits for all 10 fire columns to complete their animation (reach frame 13).
// Continues ticking each column's frame counter (arr1/arr2) and drawing via
// AncillaDraw_BombosFireColumn. Once all 10 slots are at frame 13, transitions
// to blasting phase (bombos_var4 = 2), applies sprite damage via
// Medallion_CheckSpriteDamage, and resets ancilla_step to 0.
void BombosSpell_FinishFireColumns(int kk) {  // 88b236
  int k = ancilla_step[kk];
  do {
    if (sign8(--bombos_arr1[k])) {
      bombos_arr1[k] = 3;
      if (++bombos_arr2[k] >= 13)
        bombos_arr2[k] = 13;
    }
    AncillaDraw_BombosFireColumn(k);
  } while (--k >= 0);
  for (int k = 9; k >= 0; k--) {
    if (bombos_arr2[k] != 13)
      return;
  }
  bombos_var4 = 2;
  Medallion_CheckSpriteDamage(kk);
  ancilla_step[kk] = 0;
}

// Draws one Bombos fire-column slot (kk) as up to 3 OAM tiles per frame.
// The column animation runs through 13 frames (bombos_arr2[kk]); frame is
// multiplied by 3 and iterated downward to draw 3 stacked tiles (bottom to top).
// Char 0xFF entries are skipped. Position comes from bombos_x/y_lo/hi[kk].
// kBombosSpell_FireColumn_Y holds the per-tile vertical offset (negative = up)
// to stack tiles into a rising column; X offsets add pixel-level jitter.
void AncillaDraw_BombosFireColumn(int kk) {  // 88b373
  static const int8 kBombosSpell_FireColumn_X[39] = {
     0, -1, -1,  0, 0, -1,  0, 0, -1, 0, 0, -1, 0, 0, -1, 0,
     0,  0,  0,  0, 0,  0,  0, 0,  0, 0, 0,  0, 0, 0,  0, 0,
    -1,  1, -1, -1, 2, -1, -1,
  };
  static const int8 kBombosSpell_FireColumn_Y[39] = {
     0,  -1, -1,  0,  -4, -1,   0,  -8, -1,   0, -12, -1,   0, -16,  -1,   0,
    -4, -20,  0, -8, -24,  0, -12, -28,  0, -16, -32,  0, -16, -32, -18, -34,
    -1, -35, -1, -1, -36, -1,  -1,
  };
  static const uint8 kBombosSpell_FireColumn_Flags[39] = {
    0x3c, 0xff, 0xff, 0x3c, 0x3c, 0xff, 0x3c, 0x3c, 0xff, 0x7c, 0x7c, 0xff, 0x3c, 0x7c, 0xff, 0x3c,
    0x3c, 0x3c, 0xbc, 0x3c, 0x3c, 0x7c, 0x3c, 0x3c, 0x3c, 0x3c, 0x7c, 0x3c, 0x3c, 0x3c, 0x3c, 0x3c,
    0xff, 0x3c, 0xff, 0xff, 0x3c, 0xff, 0xff,
  };
  static const uint8 kBombosSpell_FireColumn_Char[39] = {
    0x40, 0xff, 0xff, 0x42, 0x44, 0xff, 0x42, 0x44, 0xff, 0x42, 0x44, 0xff, 0x42, 0x44, 0xff, 0x40,
    0x46, 0x44, 0x4a, 0x4a, 0x48, 0x4c, 0x4c, 0x4a, 0x4e, 0x4c, 0x4a, 0x4e, 0x6a, 0x4c, 0x4e, 0x68,
    0xff, 0x6a, 0xff, 0xff, 0x4e, 0xff, 0xff,
  };
  Ancilla_AllocateOamFromRegion_A_or_D_or_F(kk, 0x10);
  OamEnt *oam = GetOamCurPtr();
  for (int i = 0; i < 1; i++) {
    int k = bombos_arr2[kk];
    if (k == 13)
      continue;
    k = k * 3 + 2;
    for (int j = 0; j < 3; j++, k--) {
      if (kBombosSpell_FireColumn_Char[k] != 0xff) {
        uint16 x = bombos_x_lo[kk] | bombos_x_hi[kk] << 8;
        uint16 y = bombos_y_lo[kk] | bombos_y_hi[kk] << 8;
        y += kBombosSpell_FireColumn_Y[k] - BG2VOFS_copy2;
        x += kBombosSpell_FireColumn_X[k] - BG2HOFS_copy2;
        Ancilla_SetOam(oam, x, y,
                       kBombosSpell_FireColumn_Char[k],
                       kBombosSpell_FireColumn_Flags[k], 2);
        oam++;
      }
      oam = Ancilla_AllocateOamFromCustomRegion(oam);
    }
  }
}

// Controls the ground-blast phase of the Bombos spell. Up to 16 blast balls
// (slots 0-15) animate through 8 frames (bombos_arr3 = 0-8). Each slot ticks
// its frame every 3 game-frames (bombos_arr4 countdown). When a slot reaches
// frame 1 and bombos_var2 == 0, a new slot is spawned at a pseudo-random
// screen position from kBombosBlasts_Tab (indexed by frame_counter & 0x3F for
// X and +3 for Y), with proximity-panned SFX. ancilla_step[kk] tracks the
// highest active slot index (grows toward 15, then wraps by recycling finished
// slots). When all 16 slots reach frame 8, the spell ends and Link is restored.
void BombosSpell_ControlBlasting(int kk) {  // 88b40d
  int k = ancilla_step[kk], sb = k;
  for (; k >= 0; k--) {
    if (bombos_arr3[k] != 8 && sign8(--bombos_arr4[k])) {
      bombos_arr4[k] = 3;
      if (++bombos_arr3[k] == 1 && !bombos_var2) {
        // Allocate the next blast slot: extend to sb+1 or recycle a done slot.
        int j = sb;
        if (j != 15) {
          j = ++sb;
        } else {
          for (; j >= 0 && bombos_arr3[j] != 8; j--) {}
        }
        bombos_arr3[j] = 0;
        bombos_arr4[j] = 3;

        // Position from kBombosBlasts_Tab pseudo-random table.
        uint16 y = kBombosBlasts_Tab[frame_counter & 0x3f];
        uint16 x = kBombosBlasts_Tab[(frame_counter & 0x3f) + 3];
        bombos_y_coord[j] = y + BG2VOFS_copy2;
        bombos_x_coord[j] = x + BG2HOFS_copy2;

        sound_effect_1 = 0xc | kBombos_Sfx[bombos_x_coord[j] >> 5 & 7];
      }
    }
    AncillaDraw_BombosBlast(k);
  }

  for (int j = 15; j >= 0; j--) {
    if (bombos_arr3[j] != 8) {
      ancilla_step[kk] = sb;
      goto getout;
    }
  }
  // All blasts done: terminate the spell and restore Link's state.
  ancilla_type[kk] = 0;
  load_chr_halfslot_even_odd = 1;
  byte_7E0324 = 0;
  state_for_spin_attack = 0;
  step_counter_for_spin_attack = 0;
  link_cant_change_direction = 0;
  flag_unk1 = 0;
  if (link_player_handler_state != kPlayerState_ReceivingBombos) {
    link_player_handler_state = kPlayerState_Ground;
    link_delay_timer_spin_attack = 0;
    button_mask_b_y = button_b_frames ? (joypad1H_last & 0x80) : 0;
  }
  link_speed_setting = 0;
  byte_7E0325 = 0;
getout:
  if (--bombos_var3 == 0)
    bombos_var2 = bombos_var3 = 1;
}

// Draws one Bombos ground-blast (slot k). The blast has 8 animation frames
// (bombos_arr3[k] = 0-7); frame 8 = done, returns early. Each frame displays
// up to 4 tiles with 4-way flip symmetry (flags 0x3C/0x7C/0xBC/0xFC) at
// expanding offsets (±8 at frame 0, growing to ±19 at frame 7). Entries with
// char 0xFF are skipped (frame 0 has only 1 tile). t starts at frame*4+3 and
// iterates downward so tiles draw from outer to inner order.
void AncillaDraw_BombosBlast(int k) {  // 88b5e1
  static const int8 kBombosSpell_DrawBlast_X[32] = {
     -8, -1,  -1, -1, -12, -4, -12, -4, -16, 0, -16, 0, -16, 0, -16, 0,
    -17,  1, -17,  1, -19,  3, -19,  3, -19, 3, -19, 3, -19, 3, -19, 3,
  };
  static const int8 kBombosSpell_DrawBlast_Y[32] = {
     -8,  -1, -1, -1, -12, -12, -4, -4, -16, -16, 0, 0, -16, -16, 0, 0,
    -17, -17,  1,  1, -19, -19,  3,  3, -19, -19, 3, 3, -19, -19, 3, 3,
  };
  static const uint8 kBombosSpell_DrawBlast_Flags[32] = {
    0x3c, 0xff, 0xff, 0xff, 0x3c, 0x7c, 0xbc, 0xfc, 0x3c, 0x7c, 0xbc, 0xfc, 0x3c, 0x7c, 0xbc, 0xfc,
    0x3c, 0x7c, 0xbc, 0xfc, 0x3c, 0x7c, 0xbc, 0xfc, 0x3c, 0x7c, 0xbc, 0xfc, 0x3c, 0x7c, 0xbc, 0xfc,
  };
  static const uint8 kBombosSpell_DrawBlast_Char[32] = {
    0x60, 0xff, 0xff, 0xff, 0x62, 0x62, 0x62, 0x62, 0x64, 0x64, 0x64, 0x64, 0x66, 0x66, 0x66, 0x66,
    0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x6a, 0x6a, 0x6a, 0x6a, 0x4e, 0x4e, 0x4e, 0x4e,
  };
  uint16 x = bombos_x_coord[k];
  uint16 y = bombos_y_coord[k];
  if (bombos_arr3[k] == 8)
    return;

  Ancilla_AllocateOamFromRegion_A_or_D_or_F(k, 0x10);
  OamEnt *oam = GetOamCurPtr();

  // Iterate 4 tiles per frame in reverse order (outer→inner).
  int t = bombos_arr3[k] * 4 + 3;
  for (int j = 0; j < 4; j++, t--) {
    if (kBombosSpell_DrawBlast_Char[t] != 0xff) {
      Ancilla_SetOam(oam,
        x + kBombosSpell_DrawBlast_X[t] - BG2HOFS_copy2,
        y + kBombosSpell_DrawBlast_Y[t] - BG2VOFS_copy2,
        kBombosSpell_DrawBlast_Char[t],
        kBombosSpell_DrawBlast_Flags[t], 2);
      oam++;
    }
    oam = Ancilla_AllocateOamFromCustomRegion(oam);
  }

}

// Ancilla type 0x1C: Quake Medallion spell controller. Three phases:
//
//   ancilla_step == 0/1: active animation. Each frame:
//     1. QuakeSpell_ShakeScreen: alternates bg1_y_offset ±quake_var3 for screen
//        shake; also displaces Link vertically for extra rumble.
//     2. QuakeSpell_ControlBolts: advances the 5 initial ground-bolt slots
//        through their animation frames (quake_arr2[0-4]).
//     3. QuakeSpell_SpreadBolts: drives the secondary spreading rock shower
//        using the kQuakeItems2/kQuakeItemPos2 tables.
//
//   ancilla_step == 2: termination. Applies sprite damage (Medallion_Check
//     SpriteDamage), starts physical rumble (Prepare_ApplyRumbleToSprites),
//     restores Link's state, clears BG offsets. Special: overworld screen 0x47
//     (Turtle Rock area) triggers the Quake-entrance on first use.
//
//   submodule_index != 0: paused. Only draw ongoing bolt animation
//     (AncillaDraw_QuakeInitialBolts) if quake_arr2[4] hasn't finished.
void Ancilla1C_QuakeSpell(int k) {  // 88b66a
  if (submodule_index != 0) {
    if (quake_arr2[4] != kQuake_Tab1[4])
      AncillaDraw_QuakeInitialBolts(k);
    return;
  }
  if (ancilla_step[k] != 2) {
    QuakeSpell_ShakeScreen(k);
    QuakeSpell_ControlBolts(k);
    QuakeSpell_SpreadBolts(k);
    return;
  }
  Medallion_CheckSpriteDamage(k);
  Prepare_ApplyRumbleToSprites();
  ancilla_type[k] = 0;
  link_player_handler_state = 0;
  load_chr_halfslot_even_odd = 1;
  byte_7E0324 = 0;
  state_for_spin_attack = 0;
  step_counter_for_spin_attack = 0;
  link_cant_change_direction = 0;
  link_delay_timer_spin_attack = 0;
  flag_unk1 = 0;
  bg1_x_offset = 0;
  bg1_y_offset = 0;
  // Turtle Rock entrance trigger (overworld 0x47).
  if (BYTE(overworld_screen_index) == 0x47 && !(save_ow_event_info[0x47] & 0x20) && Ancilla_CheckForEntranceTrigger(3)) {
    trigger_special_entrance = 4;
    subsubmodule_index = 0;
    BYTE(R16) = 0;
  }
  button_mask_b_y = button_b_frames ? (joypad1H_last & 0x80) : 0;
  link_speed_setting = 0;
  byte_7E0325 = 0;
}

// Applies per-frame screen shake by toggling bg1_y_offset between ±quake_var3
// and pushing the same delta into link_y_vel so Link visually bounces with
// the screen during the Quake spell.
void QuakeSpell_ShakeScreen(int k) {  // 88b6f7
  bg1_y_offset = quake_var3;
  quake_var3 = -quake_var3;
  link_y_vel += bg1_y_offset;
}

// Advances the 5 Quake ground-bolt slots (j=0-4) through their animation
// frames. Each slot ticks every 1 game-frame (quake_arr1). Key transitions:
//   slot 0 frame 2: play thunder SFX and enable slot 1.
//   slot 1 frame 2: enable slots 2-4 (quake_var5 = 4).
//   slot 4 frame 7: set ancilla_step = 1 (spread phase ready).
// kQuake_Tab1[j] gives the max frame for each slot.
void QuakeSpell_ControlBolts(int k) {  // 88b718
  quake_var4 = ancilla_step[k];
  int j = quake_var5;
  do {
    if (quake_arr2[j] == kQuake_Tab1[j])
      continue;

    if (sign8(--quake_arr1[j])) {
      quake_arr1[j] = 1;
      if (++quake_arr2[j] == kQuake_Tab1[j])
        continue;

      if (j == 0 && quake_arr2[j] == 2) {
        Ancilla_Sfx2_Near(0xc);
        quake_var5 = 1;
      } else if (j == 1 && quake_arr2[j] == 2) {
        quake_var5 = 4;
      } else if (j == 4 && quake_arr2[j] == 7) {
        quake_var4 = 1;
      }
    }
    AncillaDraw_QuakeInitialBolts(j);
  } while (--j >= 0);
  ancilla_step[k] = quake_var4;
}

// Draws one of the 5 Quake initial-bolt groups. kQuakeDrawGroundBolts_Tab
// gives a base offset per group into kQuakeItems/kQuakeItemPos; the current
// frame (quake_arr2[k]) selects a sub-range of QuakeItem entries. Each entry
// provides world-space (x, y) coordinates relative to (quake_var2, quake_var1)
// and flag bits encoding the char index and flip flags.
void AncillaDraw_QuakeInitialBolts(int k) {  // 88b793
  static const uint8 kQuakeDrawGroundBolts_Tab[5] = {0, 0x18, 0, 0x18, 0x2f};

  int t = quake_arr2[k] + kQuakeDrawGroundBolts_Tab[k];
  OamEnt *oam = GetOamCurPtr();
  int idx = kQuakeItemPos[t], num = kQuakeItemPos[t + 1] - idx;
  const QuakeItem *p = &kQuakeItems[idx], *pend = p + num;
  do {
    uint16 x = p->x + quake_var2 - BG2HOFS_copy2;
    uint16 y = p->y + quake_var1 - BG2VOFS_copy2;

    uint8 xval = oam->x, yval = 0xf0;
    if (x < 256 && y < 256) {
      xval = x;
      if (y < 0xf0)
        yval = y;
    }
    SetOamPlain(oam, xval, yval, kQuakeDrawGroundBolts_Char[p->f & 0x0f], p->f & 0xc0 | 0x3c, 2);
    oam++, oam_cur_ptr += 4, oam_ext_cur_ptr += 1;
  } while (++p != pend);
}

// Animates the spreading rock shower after the initial Quake bolts land.
// Only active during ancilla_step == 1. ancilla_item_to_link[k] is a frame
// counter 0-54 incremented every 2 ticks (ancilla_timer). Frame 55 transitions
// to step 2 (termination). Each frame uses kQuakeItemPos2[t] to index into
// kQuakeItems2 for pre-computed absolute screen positions (x, y) of each
// flying rock tile; f encodes char index (low nibble), flip flags (high 2 bits),
// and OAM size extension (bits 4-5). Unlike the initial bolts, positions here
// are absolute screen coordinates (not relative to quake_var1/2).
void QuakeSpell_SpreadBolts(int k) {  // 88b84f
  if (ancilla_step[k] != 1)
    return;
  if (ancilla_timer[k] == 0) {
    ancilla_timer[k] = 2;
    if (++ancilla_item_to_link[k] == 55) {
      ancilla_step[k] = 2;
      return;
    }
  }
  int t = ancilla_item_to_link[k];
  int idx = kQuakeItemPos2[t], num = kQuakeItemPos2[t + 1] - idx;
  const QuakeItem *p = &kQuakeItems2[idx], *pend = p + num;
  OamEnt *oam = GetOamCurPtr();
  do {
    SetOamPlain(oam, p->x, p->y, kQuakeDrawGroundBolts_Char[p->f & 0x0f], p->f & 0xc0 | 0x3c, p->f >> 4 & 3);
    oam_cur_ptr += 4, oam_ext_cur_ptr += 1;
    oam = Ancilla_AllocateOamFromCustomRegion(oam + 1);
  } while (++p != pend);
}

// Ancilla type 0x1A: Magic Powder dust cloud. Plays a 10-frame animation
// (item_to_link 0-9) at 1-tick intervals, applying sprite damage each frame
// and drawing the powder cloud via Ancilla_MagicPowder_Draw. When frame 9
// is reached, self-destructs and clears byte_7E0333 (the "powder in use" flag).
//
// ancilla_dir[k] selects which of two powder directions (j*10) offsets the
// kMagicPowder_Tab0 lookup for the draw-frame selector (ancilla_arr25).
void Ancilla1A_PowderDust(int k) {  // 88bab0
  if (submodule_index == 0) {
    Powder_ApplyDamageToSprites(k);
    if (sign8(--ancilla_aux_timer[k])) {
      ancilla_aux_timer[k] = 1;
      int j = ancilla_dir[k];
      if (ancilla_item_to_link[k] == 9) {
        ancilla_type[k] = 0;
        byte_7E0333 = 0;
        return;
      }
      ancilla_arr25[k] = kMagicPowder_Tab0[++ancilla_item_to_link[k] + j * 10];
    }
  }
  Ancilla_AllocateOamFromRegion_B_or_E(ancilla_numspr[k]);
  Ancilla_MagicPowder_Draw(k);
}

// Draws one frame of the magic powder cloud. ancilla_arr25[k] selects the
// draw-frame index b (0-18); 4 tiles are drawn per frame from kMagicPowder_DrawX/Y
// (offsets relative to the powder origin) and kMagicPowder_Draw_Char/Flags.
// kMagicPowder_Draw_Char: most frames use char 9 (star); frames 0-1 use
// char 10 (larger puff). Flags encode both palette and flip bits.
void Ancilla_MagicPowder_Draw(int k) {  // 88baeb
  static const int8 kMagicPowder_DrawX[76] = {
    -5, -12,  2,  -9, -7, -10, -6, -2, -6, -12,  1, -6,  -6, -12,   1,  -6,
    -6, -12,  1,  -6, -6, -12,  1, -6, -6, -12,  1, -6, -17, -23, -14, -19,
    -11, -18, -9, -13, -4, -13, -1, -8, -3,  -9,  0, -5,  -3, -10,  -1,  -5,
    -4, -13, -1,  -8, -3,  -9,  0, -5, -3, -10, -1, -5,  -3, -13,  -1,  -8,
    9,  15,  6,  11,  3,  10,  1,  5, -4,   5, -7,  0,
  };
  static const int8 kMagicPowder_DrawY[76] = {
    -20, -15, -13,  -7, -18, -13, -13, -13, -20, -13, -13,  -8, -20, -13, -13,  -8,
    -19, -12, -12,  -7, -18, -11, -11,  -6, -17, -10, -10,  -5, -16, -14, -12,  -9,
    -17, -14, -12,  -8, -18, -14, -13,  -6, -33, -31, -29, -26, -28, -25, -23, -19,
    -22, -18, -17, -10,  -2,   0,   2,   5,  -9,  -6,  -4,   0, -16, -12, -11,  -4,
    -16, -14, -12,  -9, -17, -14, -12,  -8, -18, -14, -13,  -6,
  };
  static const uint8 kMagicPowder_Draw_Char[19] = {
    9, 10, 10, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
  };
  static const uint8 kMagicPowder_Draw_Flags[76] = {
    0x68, 0x24, 0xa2, 0x28, 0x68, 0xe2, 0x28, 0xa4, 0x68, 0xe2, 0xa4, 0x28, 0x22, 0xa4, 0xe8, 0x62,
    0x24, 0xa8, 0xe2, 0x64, 0x28, 0xa2, 0xe4, 0x68, 0x22, 0xa4, 0xe8, 0x62, 0xe2, 0xa4, 0xe8, 0x64,
    0xe8, 0xa8, 0xe4, 0x62, 0xe4, 0xa8, 0xe2, 0x68, 0xe2, 0xa4, 0xe8, 0x64, 0xe8, 0xa8, 0xe4, 0x62,
    0xe4, 0xa8, 0xe2, 0x68, 0xe2, 0xa4, 0xe8, 0x64, 0xe8, 0xa8, 0xe4, 0x62, 0xe4, 0xa8, 0xe2, 0x68,
    0xe2, 0xa4, 0xe8, 0x64, 0xe8, 0xa8, 0xe4, 0x62, 0xe4, 0xa8, 0xe2, 0x68,
  };
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);
  OamEnt *oam = GetOamCurPtr();
  int b = ancilla_arr25[k];
  for (int i = 0; i < 4; i++, oam++) {
    Ancilla_SetOam(oam, info.x + kMagicPowder_DrawX[b * 4 + i], info.y + kMagicPowder_DrawY[b * 4 + i],
                   kMagicPowder_Draw_Char[b], kMagicPowder_Draw_Flags[b * 4 + i] & ~0x30 | HIBYTE(oam_priority_value), 0);
  }
}

// Applies magic powder hit effects to all sprites overlapping the powder cloud.
// Staggered: only checks one sprite per 4 frames ((frame_counter ^ j) & 3 == 0).
// Sprites already flagged with bump_damage bit 5 are immune (already processing
// a hit this cycle).
//
// Three cases:
//   sprite 0x0B (Bush): if indoors in room index 1 → skip; room index 0x0D
//     (Agahnim antechamber?) applies poof; other rooms deal damage via
//     Ancilla_CheckDamageToSprite_preset(j, 10) (powder damage class).
//   sprite 0x0D (special): if head_dir is already set, skip; otherwise set
//     head_dir = 1 and spawn a poof garnish (powder turns that sprite into a
//     fairy or activates bush poof effect).
//   all other sprites: standard damage via Ancilla_CheckDamageToSprite_preset.
void Powder_ApplyDamageToSprites(int k) {  // 88bb58
  uint8 a;
  for (int j = 15; j >= 0; j--) {
    if ((frame_counter ^ j) & 3 || sprite_state[j] != 9 || sprite_bump_damage[j] & 0x20)
      continue;
    SpriteHitBox hb;
    Ancilla_SetupBasicHitBox(k, &hb);
    Sprite_SetupHitBox(j, &hb);
    if (!CheckIfHitBoxesOverlap(&hb))
      continue;

    if ((a = sprite_type[j]) != 0xb || (a = player_is_indoors) == 0 || (a = dungeon_room_index2 - 1) != 0) {
      if (a != 0xd) {
        Ancilla_CheckDamageToSprite_preset(j, 10);
        continue;
      }
      if (sprite_head_dir[j] != 0)
        continue;
    }
    sprite_head_dir[j] = 1;
    Sprite_SpawnPoofGarnish(j);
  }
}

// Ancilla type 0x1D: Screen shake used for the dash tremor and various other
// impact effects. item_to_link[k] is the remaining shake duration in frames;
// decrements each tick and terminates when it wraps negative.
//
// ancilla_dir[k]: 0 = horizontal shake (bg1_x_offset + link_x_vel), 1 =
//   vertical (bg1_y_offset + link_y_vel). DashTremor_TwiddleOffset generates
//   a small alternating offset value for the oscillation. Sets sprite_alert_flag
//   = 3 each frame to keep all sprites in alert state during the shake.
void Ancilla1D_ScreenShake(int k) {  // 88bbbc
  if (submodule_index == 0) {
    if (sign8(--ancilla_item_to_link[k])) {
      bg1_x_offset = 0;
      bg1_y_offset = 0;
      ancilla_type[k] = 0;
      return;
    }
    int offs = DashTremor_TwiddleOffset(k);
    int j = ancilla_dir[k];
    if (j == 0) {
      bg1_x_offset = offs;
      link_x_vel += offs;
    } else {
      bg1_y_offset = offs;
      link_y_vel += offs;
    }
  }
  sprite_alert_flag = 3;
}

// Ancilla type 0x1E: Dash dust puff trail spawned when Link charges with the
// Pegasus Boots. Animates a 5-frame puff sequence, then terminates.
//
// ancilla_item_to_link[k]: current frame index (0–5). Incremented every
//   3 ticks (ancilla_timer[k] countdown). At frame 5 the function stalls
//   waiting for the timer to fire once more; at frame 6 the slot is cleared.
//
// ancilla_step[k] != 0: delegates drawing to DashDust_Motive (separate
//   motion path used for the kick-up puff when Link stops).
//
// Draw tables kDashDust_Draw_X/Y/Char define 3 OAM sprites per frame
//   (5 frames × 3 = 15 entries per half-table; the table is mirrored for the
//   water/grass surface variant, selected via draw_water_ripples_or_grass == 1).
// kDashDust_Draw_X1[link_direction_facing >> 1] applies a horizontal offset
//   so the dust offsets correctly for the current facing direction.
void Ancilla1E_DashDust(int k) {  // 88bc92
  if (ancilla_step[k]) {
    DashDust_Motive(k);
    return;
  }
  if (!ancilla_timer[k]) {
    ancilla_timer[k] = 3;
    if (++ancilla_item_to_link[k] == 5)
      return;
    if (ancilla_item_to_link[k] == 6) {
      ancilla_type[k] = 0;
      return;
    }
  }
  if (ancilla_item_to_link[k] == 5)
    return;

  Point16U info;
  Ancilla_PrepOamCoord(k, &info);
  OamEnt *oam = GetOamCurPtr();

  static const int8 kDashDust_Draw_X1[4] = {0, 0, 4, -4};
  static const int16 kDashDust_Draw_X[30] = {
    10,  5, -1,  0, 10, 5, 0,  5, -1,  0, -1, -1,  9, -1, -1, 10,
    5, -1,  0, 10,  5, 0, 5, -1,  0, -1, -1,  9, -1, -1,
  };
  static const int16 kDashDust_Draw_Y[30] = {
    -2,  0, -1, -3, -2,  0, -3,  0, -1, -3, -1, -1, -2, -1, -1, -2,
    0, -1, -3, -2,  0, -3,  0, -1, -3, -1, -1, -2, -1, -1,
  };
  static const uint8 kDashDust_Draw_Char[30] = {
    0xcf, 0xa9, 0xff, 0xa9, 0xdf, 0xcf, 0xcf, 0xdf, 0xff, 0xdf, 0xff, 0xff, 0xa9, 0xff, 0xff, 0xcf,
    0xcf, 0xff, 0xcf, 0xdf, 0xcf, 0xcf, 0xdf, 0xff, 0xdf, 0xff, 0xff, 0xcf, 0xff, 0xff,
  };
  int r12 = kDashDust_Draw_X1[link_direction_facing >> 1];
  int t = 3 * (ancilla_item_to_link[k] + (draw_water_ripples_or_grass == 1 ? 5 : 0));

  for (int n = 2; n >= 0; n--, t++) {
    if (kDashDust_Draw_Char[t] != 0xff) {
      Ancilla_SetOam(oam, info.x + r12 + kDashDust_Draw_X[t], info.y + kDashDust_Draw_Y[t], kDashDust_Draw_Char[t], 4 | HIBYTE(oam_priority_value), 0);
      oam++;
    }
  }
}

// Ancilla type 0x1F: Hookshot head (the flying claw). Covers the full
// hookshot lifecycle:
//
//   Outward flight (ancilla_step[k] == 0):
//     - Moves each tick via Ancilla_MoveX/Y.
//     - After 32 ticks (ancilla_item_to_link[k] == 32) the claw begins its
//       return: step is set to 1 and both velocity components are negated.
//     - Calls Ancilla_CheckSpriteCollision; a sprite hit also reverses the
//       hookshot (step → 1) without triggering the wall clink SFX.
//     - Calls Hookshot_CheckTileCollision to detect wall impacts; a solid
//       tile reverses the claw and optionally plays the wall-clink SFX.
//     - A latchable tile (tiledetect_misc_tiles & 3) sets related_to_hookshot
//       = 1 to freeze the claw at that position and begins the pull-back.
//
//   Return flight (ancilla_step[k] == 1):
//     - ancilla_item_to_link[k] decrements; when it underflows the slot
//       is cleared.
//
//   Ledge arbitration (ancilla_G / ancilla_L / ancilla_K):
//     - ancilla_G[k]: grace counter; allows the claw to cross ledge-tile
//       boundaries. ancilla_L[k]: persistent ledge-locked flag. ancilla_K[k]:
//       stores index_of_interacting_tile at the first ledge hit.
//
//   Draw path:
//     - kHookShot_Draw_Char/Flags (12-entry, 3 sprites per direction) renders
//       the claw head. The chain links are drawn as a column of 8×8 tiles
//       stepped at kHookShot_Move_X/Y intervals; every other frame they
//       rotate 180° ((frame_counter & 2) << 6 in the flip flags).
//     - If related_to_hookshot is set, oam_priority_value is raised to 0x3000
//       so the claw renders above everything while lodged in the wall.
void Ancilla1F_Hookshot(int k) {  // 88bd74
  if (submodule_index != 0)
    goto do_draw;

  if (!ancilla_timer[k]) {
    ancilla_timer[k] = 7;
    Ancilla_Sfx2_Pan(k, 0xa);
  }

  if (related_to_hookshot)
    goto do_draw;
  Ancilla_MoveY(k);
  Ancilla_MoveX(k);
  if (ancilla_step[k]) {
    if (sign8(--ancilla_item_to_link[k])) {
      ancilla_type[k] = 0;
      return;
    }
    goto do_draw;
  }

  if (++ancilla_item_to_link[k] == 32) {
    ancilla_step[k] = 1;
    ancilla_x_vel[k] = -ancilla_x_vel[k];
    ancilla_y_vel[k] = -ancilla_y_vel[k];
  }

  if (Hookshot_ShouldIEvenBotherWithTiles(k))
    goto do_draw;

  if (!ancilla_L[k] && !ancilla_step[k] && Ancilla_CheckSpriteCollision(k) >= 0 && !ancilla_step[k]) {
    ancilla_step[k] = 1;
    ancilla_y_vel[k] = -ancilla_y_vel[k];
    ancilla_x_vel[k] = -ancilla_x_vel[k];
  }

  Hookshot_CheckTileCollision(k);

  uint8 r0;

  r0 = 0;

  if (player_is_indoors) {
    if (!(ancilla_dir[k] & 2)) {
      r0 = (tiledetect_vertical_ledge | (tiledetect_vertical_ledge >> 4)) & 3;
    } else {
      r0 = detection_of_ledge_tiles_horiz_uphoriz & 3;
    }
    if (r0 == 0)
      goto endif_7;
  } else {
    if (!((detection_of_ledge_tiles_horiz_uphoriz & 3 | tiledetect_vertical_ledge | detection_of_unknown_tile_types) & 0x33))
      goto endif_7;
  }
  if (sign8(--ancilla_G[k])) {
    if (ancilla_K[k] && ((r0 & 3) || ancilla_K[k] != BYTE(index_of_interacting_tile))) {
      ancilla_G[k] = 2;
      if (sign8(--ancilla_L[k]))
        ancilla_L[k] = 0;
    } else {
      ancilla_L[k]++;
      ancilla_K[k] = index_of_interacting_tile;
      ancilla_G[k] = 1;
    }
  }
endif_7:
  if (ancilla_L[k])
    goto do_draw;
  if (!sign8(ancilla_G[k])) {
    ancilla_G[k]--;
    goto do_draw;
  }

  if ((R14 >> 4 | R14 | tiledetect_stair_tile | R12) & 3 && !ancilla_step[k]) {
    ancilla_step[k] = 1;
    ancilla_y_vel[k] = -ancilla_y_vel[k];
    ancilla_x_vel[k] = -ancilla_x_vel[k];
    if (!(tiledetect_misc_tiles & 3)) {
      AncillaAdd_HookshotWallClink(k, 6, 1);
      Ancilla_Sfx2_Pan(k, (tiledetect_misc_tiles & 0x30) ? 6 : 5);
    }
  }

  if (tiledetect_misc_tiles & 3) {
    if (ancilla_item_to_link[k] < 4) {
      ancilla_type[k] = 0;
      return;
    }
    related_to_hookshot = 1;
    hookshot_effect_index = k;
  }

  static const int8 kHookShot_Move_X[4] = {0, 0, 8, -8};
  static const int8 kHookShot_Move_Y[4] = {8, -9, 0, 0};
  static const uint8 kHookShot_Draw_Flags[12] = {0, 0, 0xff, 0x80, 0x80, 0xff, 0x40, 0xff, 0x40, 0, 0xff, 0};
  static const uint8 kHookShot_Draw_Char[12] = {9, 0xa, 0xff, 9, 0xa, 0xff, 9, 0xff, 0xa, 9, 0xff, 0xa};

  Point16U info;
do_draw:
  Ancilla_PrepOamCoord(k, &info);
  if (ancilla_L[k])
    oam_priority_value = 0x3000;
  OamEnt *oam = GetOamCurPtr();

  int j = ancilla_dir[k] * 3;
  int x = info.x, y = info.y;
  for (int i = 2; i >= 0; i--, j++) {
    if (kHookShot_Draw_Char[j] != 0xff) {
      Ancilla_SetOam(oam, x, y, kHookShot_Draw_Char[j], kHookShot_Draw_Flags[j] | 2 | HIBYTE(oam_priority_value), 0);
      oam++;
    }
    if (i == 1)
      x -= 8, y += 8;
    else
      x += 8;
  }

  int r10 = 0;
  int n = ancilla_item_to_link[k] >> 1;
  if (n >= 7) {
    r10 = n - 7;
    n = 6;
  }
  if (n == 0)
    return;
  if (ancilla_dir[k] & 1)
    r10 = -r10;
  x = info.x, y = info.y;
  j = ancilla_dir[k];
  if (kHookShot_Move_Y[j] == 0)
    y += 4;
  if (kHookShot_Move_X[j] == 0)
    x += 4;
  do {
    if (kHookShot_Move_Y[j])
      y += kHookShot_Move_Y[j] + r10;
    if (kHookShot_Move_X[j])
      x += kHookShot_Move_X[j] + r10;
    if (!Hookshot_CheckProximityToLink(x, y)) {
      Ancilla_SetOam(oam, x, y, 0x19, (frame_counter & 2) << 6 | 2 | HIBYTE(oam_priority_value), 0);
      oam++;
    }
  } while (--n >= 0);
}

// Ancilla type 0x20: Bed-spread (blanket) animation shown during the intro
// cutscene while Link is asleep. Draws a 4-sprite (32×16) blanket graphic.
//
// link_pose_during_opening selects which OAM region and which half of the
// sprite table to use:
//   0 → region B (behind Link), tile row j = 0 (lying-flat variant).
//   non-0 → region A (in front), tile row j = 4 (raised/covers-head variant).
//
// kBedSpread_Char[8] and kBedSpread_Flags[8] define two sets of 4 sprites;
// each sprite is placed 16 px apart horizontally, with a −32 wrap and +8 Y
// bump at the midpoint to form a 2×2 tile grid.
void Ancilla20_Blanket(int k) {  // 88c013
  static const uint8 kBedSpread_Char[8] = {0xa, 0xa, 0xa, 0xa, 0xc, 0xc, 0xa, 0xa};
  static const uint8 kBedSpread_Flags[8] = {0, 0x60, 0xa0, 0xe0, 0, 0x60, 0xa0, 0xe0};
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);

  if (!link_pose_during_opening) {
    Oam_AllocateFromRegionB(0x10);
  } else {
    Oam_AllocateFromRegionA(0x10);
  }

  OamEnt *oam = GetOamCurPtr();
  int j = link_pose_during_opening ? 4 : 0;
  uint16 x = pt.x, y = pt.y;
  for (int i = 3; i >= 0; i--, j++, oam++) {
    Ancilla_SetOam(oam, x, y, kBedSpread_Char[j], kBedSpread_Flags[j] | 0xd | HIBYTE(oam_priority_value), 2);
    x += 16;
    if (i == 2)
      x -= 32, y += 8;
  }
}

// Ancilla type 0x21: Snore bubble animation shown while Link sleeps in the
// intro. A single 8×8 bubble sprite drifts upward and wobbles horizontally.
//
// Lifecycle:
//   - ancilla_item_to_link[k] advances 0→1→2 every 8 ticks (aux_timer); it
//     selects which GFX DMA source (kBedSpread_Dma[]) to load — three
//     progressively larger bubble frames.
//   - ancilla_x_vel[k] oscillates by ancilla_step[k] each frame; step is
//     negated whenever the magnitude reaches 8, producing a side-to-side
//     wobble.
//   - Ancilla_MoveX/Y apply the current velocity to the position.
//   - Termination: when the bubble Y-coordinate rises above link_y_coord − 24
//     the slot is cleared.
//   - link_dma_var5 is written with the chosen DMA source index so the NMI
//     handler uploads the correct bubble tile to VRAM each frame.
void Ancilla21_Snore(int k) {  // 88c094
  static const uint8 kBedSpread_Dma[3] = {0x44, 0x43, 0x42};
  if (sign8(--ancilla_aux_timer[k])) {
    if (ancilla_item_to_link[k] != 2)
      ancilla_item_to_link[k]++;
    ancilla_aux_timer[k] = 7;
  }
  ancilla_x_vel[k] += ancilla_step[k];
  if (abs8(ancilla_x_vel[k]) >= 8)
    ancilla_step[k] = -ancilla_step[k];
  Ancilla_MoveY(k);
  Ancilla_MoveX(k);
  if (Ancilla_GetY(k) <= (uint16)(link_y_coord - 24))
    ancilla_type[k] = 0;
  link_dma_var5 = kBedSpread_Dma[ancilla_item_to_link[k]];
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  Ancilla_SetOam(GetOamCurPtr(), pt.x, pt.y, 9, 0x24, 0);
}

// Ancilla type 0x3B: Victory sparkle displayed above Link when he holds the
// sword aloft after defeating a boss or receiving a key item. Plays a
// 4-frame sparkle animation (kAncilla_VictorySparkle_* tables, 4 sprites per
// frame) anchored to Link's world position (link_x/y_coord − BG2 scroll).
//
// ancilla_aux_timer[k]: initial delay; sparkle does not tick until it reaches
//   zero, giving a brief pause before the animation begins.
// ancilla_arr3[k]: per-frame duration counter (reloads to 1 each time it
//   expires). ancilla_item_to_link[k]: current frame index (0–3).
//   At frame 4 the slot is cleared and aux_timer is decremented to signal
//   completion to whatever spawned this ancilla.
void Ancilla3B_SwordUpSparkle(int k) {  // 88c167
  static const int8 kAncilla_VictorySparkle_X[16] = {16, 0, 0, 0, 8, 16, 8, 16, 9, 15, 0, 0, 12, 0, 0, 0};
  static const int8 kAncilla_VictorySparkle_Y[16] = {-7, 0, 0, 0, -11, -11, -3, -3, -7, -7, 0, 0, -7, 0, 0, 0};
  static const uint8 kAncilla_VictorySparkle_Char[16] = {0x92, 0xff, 0xff, 0xff, 0x93, 0x93, 0x93, 0x93, 0xf9, 0xf9, 0xff, 0xff, 0x80, 0xff, 0xff, 0xff};
  static const uint8 kAncilla_VictorySparkle_Flags[16] = {0, 0xff, 0xff, 0xff, 0, 0x40, 0x80, 0xc0, 0, 0x40, 0xff, 0xff, 0, 0xff, 0xff, 0xff};

  if (ancilla_aux_timer[k]) {
    ancilla_aux_timer[k]--;
    return;
  }

  if (sign8(--ancilla_arr3[k])) {
    ancilla_arr3[k] = 1;
    if (++ancilla_item_to_link[k] == 4) {
      ancilla_type[k] = 0;
      ancilla_aux_timer[k]--;
      return;
    }
  }
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  int j = ancilla_item_to_link[k] * 4;
  for (int i = 0; i < 4; i++, j++) {
    if (kAncilla_VictorySparkle_Char[j] != 0xff) {
      Ancilla_SetOam(oam,
                     link_x_coord + kAncilla_VictorySparkle_X[j] - BG2HOFS_copy2,
                     link_y_coord + kAncilla_VictorySparkle_Y[j] - BG2VOFS_copy2,
                     kAncilla_VictorySparkle_Char[j],
                     kAncilla_VictorySparkle_Flags[j] | 4 | HIBYTE(oam_priority_value), 0);
      oam++;
    }
  }
}

// Ancilla type 0x3C: Spin-attack charge sparkle. A 3-frame single-sprite
// effect drawn at Link's position while the spin-attack charge is building.
//
// Advances one frame every 4 ticks (ancilla_timer[k]). At frame 3 the slot
// terminates. Each frame maps to one entry in kSwordChargeSpark_Char/Flags
// (tile 0xb7, 0x80, or 0x83). Uses Ancilla_AllocateOamFromRegion_A_or_D_or_F
// to place the sprite in the correct OAM floor region.
void Ancilla3C_SpinAttackChargeSparkle(int k) {  // 88c1ea
  static const uint8 kSwordChargeSpark_Char[3] = {0xb7, 0x80, 0x83};
  static const uint8 kSwordChargeSpark_Flags[3] = {4, 4, 0x84};

  if (!submodule_index && !ancilla_timer[k]) {
    ancilla_timer[k] = 4;
    if (++ancilla_item_to_link[k] == 3) {
      ancilla_type[k] = 0;
      return;
    }
  }
  ancilla_oam_idx[k] = Ancilla_AllocateOamFromRegion_A_or_D_or_F(k, 4);
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);
  int j = ancilla_item_to_link[k];
  Ancilla_SetOam(GetOamCurPtr(), info.x, info.y,
                 kSwordChargeSpark_Char[j], kSwordChargeSpark_Flags[j] | HIBYTE(oam_priority_value), 0);
}

// Ancilla type 0x35: Master Sword receipt ceremony sparkle. Overlays a
// 4-sprite radiant glow on Link while he holds the Master Sword aloft.
//
// ancilla_timer[k]: active duration; the slot clears when it reaches zero.
// ancilla_aux_timer[k]: per-frame sub-timer that cycles ancilla_item_to_link
//   through 0→1→2→0 to animate three sparkle orientations.
// kSwordCeremony_Char/Flags/X/Y: two 4-entry banks (j = (frame - 1) * 4).
//   At frame index 0, j < 0 and drawing is skipped, producing a blank first
//   tick that avoids a one-frame pop artifact.
void Ancilla35_MasterSwordReceipt(int k) {  // 88c25f
  static const int8 kSwordCeremony_X[8] = {-1, 8, -1, 8, 0, 7, 0, 7};
  static const int8 kSwordCeremony_Y[8] = {1, 1, 9, 9, 1, 1, 9, 9};
  static const uint8 kSwordCeremony_Char[8] = {0x86, 0x86, 0x96, 0x96, 0x87, 0x87, 0x97, 0x97};
  static const uint8 kSwordCeremony_Flags[8] = {1, 0x41, 1, 0x41, 1, 0x41, 1, 0x41};

  if (!ancilla_timer[k]) {
    ancilla_type[k] = 0;
    return;
  }
  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_item_to_link[k] = (ancilla_item_to_link[k] == 2) ? 0 : ancilla_item_to_link[k] + 1;
  }

  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  int j = (ancilla_item_to_link[k] - 1) * 4;
  if (j < 0)
    return;

  for (int i = 0; i < 4; i++, j++, oam++) {
    Ancilla_SetOam(oam, pt.x + kSwordCeremony_X[j], pt.y + kSwordCeremony_Y[j],
                   kSwordCeremony_Char[j], kSwordCeremony_Flags[j] & ~0x30 | 4 | HIBYTE(oam_priority_value), 0);
  }
}

// Ancilla type 0x22: Item receipt controller. Manages the full sequence of
// Link holding an item overhead after receiving it — the freeze, the sparkle,
// the dialogue trigger, and the eventual handoff back to normal gameplay.
//
// ancilla_item_to_link[k]: the item ID being received (maps into
//   kReceiveItemMsgs / kReceiveItemMsgs2 / kReceiveItemMsgs3 for dialogue,
//   and into draw tables for the OAM sprite).
// ancilla_step[k]: receipt method.
//   0 = normal item hold (arms-up pose).
//   2 = silent receipt (no dialogue, no immobilization release).
//   3 = boss-prize receipt (calls PrepareDungeonExitFromBossFight on close).
// ancilla_aux_timer[k]: countdown used to pace the sequence. Key milestones:
//   aux_timer == 40 → play item jingle (Ancilla_AddRupees / Ancilla_Sfx3).
//   aux_timer == 1  → check for APU silence before advancing (avoids cutting
//     the fanfare short for items with long SFX like the Crystal).
//   aux_timer == 0  → trigger dialogue via Main_ShowTextMessage and allow
//     Link to lower the item.
//
// Special item handling inline within the sequence:
//   item 0x01 (lamp): rotates three sub-palettes via ancilla_arr1/arr4/arr3.
//   item 0x20 (Crystal): suppresses Z, adds sparkle, and once the crystal
//     fanfare ends (APU silence check) calls ItemReceipt_TransmuteToRisingCrystal.
//   items 0x34–0x36 (mirror shield/tunic/gloves): cycle tri-colour GFX via
//     WriteTo4BPPBuffer_at_7F4000.
//   items 0x22/0x23 (gloves): reload Link's armor palette.
//   items 0x26/0x3e/0x3f (heart containers): add 8 capacity units.
//   item 0x17 (heart piece): dialogue varies with link_heart_pieces count.
//
// Flag interactions:
//   flag_unk1: incremented each frame Link holds the item to keep other
//     handlers from interfering with his pose.
//   flag_is_link_immobilized: set to 0 on completion (or kept at 2 for
//     silent receipts).
void Ancilla22_ItemReceipt(int k) {  // 88c38a
  uint8 a;

  if (flag_is_link_immobilized == 2)
    goto endif_1;
  if (submodule_index != 0 && submodule_index != 43 && submodule_index != 9) {
    if (submodule_index == 2)
      ancilla_timer[k] = 16;
    goto endif_1;
  }
  flag_unk1++;

  if (ancilla_step[k] != 0 && ancilla_step[k] != 3) {
    if (sign8(--ancilla_aux_timer[k]))
      goto endif_11;

    if (ancilla_aux_timer[k] == 0)
      goto endif_6;

    if (ancilla_aux_timer[k] == 40 && ancilla_step[k] != 2) {
      if (Ancilla_AddRupees(k) || ancilla_item_to_link[k] != 0x17)
        Ancilla_Sfx3_Near(0xf);
    }
    goto label_b;
  }

  if (ancilla_item_to_link[k] == 1 && ancilla_step[k] != 2) {
    if (ancilla_timer[k] == 0)
      goto label_a;
    if (ancilla_timer[k] != 17)
      goto endif_1;
    word_7E02CD = 0xDF3;
    follower_indicator = 0xe;
    goto endif_6;
  }

  a = --ancilla_aux_timer[k];
  if (a == 0)
    goto label_a;
  if (a == 1) {
    if (ancilla_item_to_link[k] != 0x37 && ancilla_item_to_link[k] != 0x38 && ancilla_item_to_link[k] != 0x39 || zelda_read_apui00() == 0)
      goto endif_6;
    ancilla_aux_timer[k]++;
  }
  goto endif_1;

label_a:
  if (ancilla_item_to_link[k] == 1 && !ancilla_step[k]) {
    sound_effect_ambient = 5;
    music_control = 2;
  }
  link_player_handler_state = link_is_in_deep_water ? kPlayerState_Swimming : 0;
  link_receiveitem_index = 0;
  link_pose_for_item = 0;
  link_disable_sprite_damage = 0;
  Ancilla_AddRupees(k);
endif_11:
  item_receipt_method = 0;
  a = ancilla_item_to_link[k];
  if (a == 23 && link_heart_pieces == 0) {
    Link_ReceiveItem(0x26, 0);
    ancilla_type[k] = 0;
    flag_unk1 = 0;
    return;
  }

  if (a == 0x26 || a == 0x3f) {
    if (link_health_capacity != 0xa0) {
      link_health_capacity += 8;
      link_hearts_filler += link_health_capacity - link_health_current;
      Ancilla_Sfx3_Near(0xd);
    }
  } else if (a == 0x3e) {
    flag_is_link_immobilized = 0;
    if (link_health_capacity != 0xa0) {
      link_health_capacity += 8;
      link_hearts_filler += 8;
      Ancilla_Sfx3_Near(0xd);
    }
  } else if (a == 0x42) {
    link_hearts_filler += 8;
  } else if (a == 0x45) {
    link_magic_filler += 16;
  } else if (a == 0x22 || a == 0x23) {
    Palette_Load_LinkArmorAndGloves();
  }

  ancilla_type[k] = 0;
  flag_unk1 = 0;
  a = ancilla_item_to_link[k];
  if (ancilla_step[k] == 3 && a != 0x10 && a != 0x26 && a != 0xf && a != 0x20) {
    PrepareDungeonExitFromBossFight();
  }

  if (ancilla_step[k] != 2)
    flag_is_link_immobilized = 0;
  return;

endif_6:
  if (player_is_indoors) {
    int room = dungeon_room_index;
    if (room == 0xff || room == 0x10f || room == 0x110 || room == 0x112 || room == 0x11f)
      goto label_b;
  }
  int msg;
  msg = -1;
  if (ancilla_item_to_link[k] == 0x38 || ancilla_item_to_link[k] == 0x39) {
    if ((link_which_pendants & 7) == 7)
      msg = kReceiveItemMsgs2[ancilla_item_to_link[k] - 0x38];
    else
      msg = kReceiveItemMsgs[ancilla_item_to_link[k]];
  } else if (ancilla_step[k] != 2) {
    if (ancilla_item_to_link[k] == 0x17)
      msg = kReceiveItemMsgs3[link_heart_pieces];
    else
      msg = kReceiveItemMsgs[ancilla_item_to_link[k]];
  }
  if (msg != -1) {
    dialogue_message_index = msg;
    if (msg == 0x70)
      sound_effect_ambient = 9;
    Main_ShowTextMessage();
  }
  goto endif_1;

label_b:
  if (ancilla_aux_timer[k] >= 24) {
    a = ancilla_y_vel[k] - 1;
    if (a >= 248)
      ancilla_y_vel[k] = a;
    Ancilla_MoveY(k);
  }
endif_1:

  if (ancilla_item_to_link[k] == 0x20) {
    ancilla_z[k] = 0;
    AncillaAdd_OccasionalSparkle(k);
    if (zelda_read_apui00() == 0) {
      music_control = 0x1a;
      ItemReceipt_TransmuteToRisingCrystal(k);
      return;
    }
  } else if (ancilla_item_to_link[k] == 0x1) {
    ancilla_arr4[k] = kReceiveItem_Tab0[0];
    if (ancilla_step[k] != 2) {
      if (ancilla_timer[k] < 16) {
        a = 0;
      } else {
        if (!sign8(--ancilla_arr3[k]))
          goto skipit;
        ancilla_arr3[k] = 2;
        a = ancilla_arr1[k] + 1;
        if (a == 3)
          a = 0;
      }
      ancilla_arr1[k] = a;
      ancilla_arr4[k] = kReceiveItem_Tab0[a];
skipit:;
    }
  }

  if ((ancilla_item_to_link[k] == 0x34 || ancilla_item_to_link[k] == 0x35 || ancilla_item_to_link[k] == 0x36) && sign8(--ancilla_arr3[k])) {
    a = ancilla_arr1[k] + 1;
    if (a == 3)
      a = 0;
    ancilla_arr1[k] = a;
    ancilla_arr3[k] = kReceiveItem_Tab4[a];
    WriteTo4BPPBuffer_at_7F4000(kReceiveItem_Tab5[a]);
  }
  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  Ancilla_ReceiveItem_Draw(k, pt.x, pt.y);
}

// Draws the item-receipt sprite at (x, y) and returns the next free OamEnt
// pointer. Used by both Ancilla22_ItemReceipt and WishPondItem_Draw.
//
// The base tile is always 0x24 (a 16×16 item slot). kWishPond2_OamFlags[j]
// selects the palette/flags: if the high bit is set the item uses a dynamic
// palette stored in ancilla_arr4[k] rather than the table value.
// kReceiveItem_Tab1[j] controls the 16-bit (large) flag: if non-zero a second
// 8×8 bottom tile (0x34) is drawn 8 px below the base to extend certain items
// downward (e.g. the Master Sword).
OamEnt *Ancilla_ReceiveItem_Draw(int k, int x, int y) {  // 88c690
  OamEnt *oam = GetOamCurPtr();
  int j = ancilla_item_to_link[k];
  oam->charnum = 0x24;
  uint8 a = kWishPond2_OamFlags[j];
  if (sign8(a))
    a = ancilla_arr4[k];
  Ancilla_SetOam(oam, x, y, 0x24, a * 2 | 0x30, kReceiveItem_Tab1[j]);
  oam++;
  if (kReceiveItem_Tab1[j] == 0) {
    Ancilla_SetOam(oam, x, y + 8, 0x34, a * 2 | 0x30, 0);
    oam++;
  }
  return oam;
}

// Ancilla type 0x28: Wish Pond item — a thrown item (typically the Boomerang
// or Shield) arc-tossed into the Fairies' Wish Pond. Simulates a parabolic
// arc via ancilla_z_vel (gravity: −2 per tick) and MoveX/Y/Z.
//
// When the item hits the water (ancilla_z wraps negative to < 228):
//   - Z is clamped to 228 (just below the surface).
//   - Position is nudged (+4..+8 X, +18 Y) to the pond centre.
//   - Ancilla_TransmuteToSplash is called to convert this slot to a splash
//     effect (type 0x3D) and terminate the flight.
//
// While airborne (link_picking_throw_state == 2, ancilla_timer[k] == 0) the
// draw function WishPondItem_Draw handles OAM output with a drop-shadow.
void Ancilla28_WishPondItem(int k) {  // 88c6f2
  Ancilla_AllocateOamFromRegion_A_or_D_or_F(k, 0x10);

  if (submodule_index == 0 && ancilla_timer[k] == 0) {
    link_picking_throw_state = 2;
    link_state_bits = 0;
    ancilla_z_vel[k] -= 2;
    Ancilla_MoveZ(k);
    Ancilla_MoveY(k);
    Ancilla_MoveX(k);
    if (sign8(ancilla_z[k]) && ancilla_z[k] < 228) {
      ancilla_z[k] = 228;
      Ancilla_SetXY(k,
          Ancilla_GetX(k) + (kGeneratedWishPondItem[ancilla_item_to_link[k]] ? 8 : 4), // wtf
          Ancilla_GetY(k) + 18);
      Ancilla_TransmuteToSplash(k);
      return;
    }
  }
  WishPondItem_Draw(k);
}

// Draws the wish-pond item sprite. Delegates sprite output to
// Ancilla_ReceiveItem_Draw (which returns the next free OamEnt), then
// appends a drop-shadow beneath it while the item is still falling
// (link_picking_throw_state == 2 and z_vel has not yet peaked).
//
// kGeneratedWishPondItem[ancilla_item_to_link[k]]: size indicator used to
//   choose the shadow tile size (1 → 8×8, 2 → 16×16) and horizontal offset.
// The sprite Z-height offset is subtracted from Y so the item appears to
//   float above the ground plane while falling.
void WishPondItem_Draw(int k) {  // 88c760
  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);

  if (ancilla_item_to_link[k] == 1)
    ancilla_arr4[k] = 5;

  OamEnt *oam = Ancilla_ReceiveItem_Draw(k, pt.x, pt.y - (int8)ancilla_z[k]);

  if (link_picking_throw_state != 2 || !sign8(ancilla_z_vel[k]) && ancilla_z_vel[k] >= 2)
    return;

  uint8 xx = kGeneratedWishPondItem[ancilla_item_to_link[k]];
  AncillaDraw_Shadow(oam,
    (xx == 2) ? 1 : 2,
    pt.x - (xx == 2 ? 0 : 4),
    pt.y + 40, HIBYTE(oam_priority_value));
}

// Ancilla type 0x42: Happiness Pond (fairy fountain) rupee shower — the
// ten rupees returned to Link after throwing them in. Iterates all 10
// happiness_pond slots; for each active slot (happiness_pond_arr1[i] != 0)
// calls HapinessPondRupees_ExecuteRupee to simulate its arc and draw it.
//
// Each rupee's physics state is kept in a parallel array (happiness_pond_*)
// rather than using ancilla slots directly, because only one ancilla slot is
// allocated for the entire shower. When a rupee finishes its splash
// (happiness_pond_step[i] == 2) its slot is cleared.
//
// When all 10 slots are empty the ancilla slot itself is released
// (ancilla_type[k] = 0).
void Ancilla42_HappinessPondRupees(int k) {  // 88c7de
  link_picking_throw_state = 2;
  link_state_bits = 0;
  for (int i = 9; i >= 0; i--) {
    if (happiness_pond_arr1[i]) {
      HapinessPondRupees_ExecuteRupee(k, i);
      if (happiness_pond_step[i] == 2)
        happiness_pond_arr1[i] = 0;
    }
  }
  for (int i = 9; i >= 0; i--) {
    if (happiness_pond_arr1[i])
      return;
  }
  ancilla_type[k] = 0;
}

// Simulates one rupee (index i) in the Happiness Pond shower, using ancilla
// slot k as a scratch workspace. State is loaded from happiness_pond_* arrays
// before processing and saved back afterwards via GetState/SaveState.
//
// Two phases driven by ancilla_step[k] (proxied from happiness_pond_step[i]):
//   step 0 (airborne): gravity applied via z_vel − 2 each tick, MoveX/Y/Z
//     advance position. On landing (z wraps below 0xe4) the position is snapped
//     to the pond surface, step advances to 1, and the splash SFX plays.
//   step 1 (splash): delegates to ObjectSplash_Draw until item_to_link[k]
//     reaches 5 (5 splash frames × 6 ticks each), then step → 2 (done).
//
// While airborne (step 0, timer == 0) WishPondItem_Draw renders the rupee
// with a drop-shadow. The else_label path catches the edge case where z > 0xe4
// (rupee has not yet started falling) and just draws the static item.
void HapinessPondRupees_ExecuteRupee(int k, int i) {  // 88c819
  Ancilla_AllocateOamFromRegion_A_or_D_or_F(k, 0x10);
  HapinessPondRupees_GetState(k, i);

  if (ancilla_step[k]) {
    if (!submodule_index && !ancilla_timer[k]) {
      ancilla_timer[k] = 6;
      if (++ancilla_item_to_link[k] == 5) {
        ancilla_step[k]++;
      } else {
        ObjectSplash_Draw(k);
      }
    } else {
      ObjectSplash_Draw(k);
    }
  } else if (submodule_index == 0 && ancilla_timer[k] == 0) {
    ancilla_z_vel[k] -= 2;
    Ancilla_MoveY(k);
    Ancilla_MoveX(k);
    Ancilla_MoveZ(k);
    if (!sign8(ancilla_z[k]) || ancilla_z[k] >= 0xe4)
      goto else_label;
    ancilla_z[k] = 0xe4;
    Ancilla_SetXY(k, Ancilla_GetX(k) - 4, Ancilla_GetY(k) + 30);
    ancilla_item_to_link[k] = 0;
    ancilla_timer[k] = 6;
    Ancilla_Sfx2_Pan(k, 0x28);
    ancilla_step[k]++;
    ObjectSplash_Draw(k);
  } else {
else_label:
    ancilla_arr4[k] = 2;
    ancilla_floor[k] = 0;
    WishPondItem_Draw(k);
  }
  HapinessPondRupees_SaveState(i, k);
}

// Copies per-rupee physics state from the happiness_pond_* parallel arrays
// (index k) into the ancilla slot (index j), so ExecuteRupee can process the
// rupee using the standard ancilla movement and draw helpers. The timer is
// pre-decremented by 1 here (with a floor of 0) so it advances even though
// it is not stored in a real ancilla slot that would be decremented automatically.
void HapinessPondRupees_GetState(int j, int k) {  // 88c8be
  ancilla_y_lo[j] = happiness_pond_y_lo[k];
  ancilla_y_hi[j] = happiness_pond_y_hi[k];
  ancilla_x_lo[j] = happiness_pond_x_lo[k];
  ancilla_x_hi[j] = happiness_pond_x_hi[k];
  ancilla_z[j] = happiness_pond_z[k];
  ancilla_y_vel[j] = happiness_pond_y_vel[k];
  ancilla_x_vel[j] = happiness_pond_x_vel[k];
  ancilla_z_vel[j] = happiness_pond_z_vel[k];
  ancilla_y_subpixel[j] = happiness_pond_y_subpixel[k];
  ancilla_x_subpixel[j] = happiness_pond_x_subpixel[k];
  ancilla_z_subpixel[j] = happiness_pond_z_subpixel[k];
  ancilla_item_to_link[j] = happiness_pond_item_to_link[k];
  ancilla_step[j] = happiness_pond_step[k];
  ancilla_timer[j] = happiness_pond_timer[k] ? happiness_pond_timer[k] - 1 : 0;
}

// Writes the ancilla scratch state (index j) back to the happiness_pond_*
// parallel arrays (index k) after ExecuteRupee has finished processing the
// rupee for this frame. Preserves position, velocity, Z-height, timers, and
// step so the next frame resumes from the correct state.
void HapinessPondRupees_SaveState(int k, int j) {  // 88c924
  happiness_pond_y_lo[k] = ancilla_y_lo[j];
  happiness_pond_y_hi[k] = ancilla_y_hi[j];
  happiness_pond_x_lo[k] = ancilla_x_lo[j];
  happiness_pond_x_hi[k] = ancilla_x_hi[j];
  happiness_pond_z[k] = ancilla_z[j];
  happiness_pond_y_vel[k] = ancilla_y_vel[j];
  happiness_pond_x_vel[k] = ancilla_x_vel[j];
  happiness_pond_z_vel[k] = ancilla_z_vel[j];
  happiness_pond_y_subpixel[k] = ancilla_y_subpixel[j];
  happiness_pond_x_subpixel[k] = ancilla_x_subpixel[j];
  happiness_pond_z_subpixel[k] = ancilla_z_subpixel[j];
  happiness_pond_item_to_link[k] = ancilla_item_to_link[j];
  happiness_pond_timer[k] = ancilla_timer[j];
  happiness_pond_step[k] = ancilla_step[j];
}

// Converts the ancilla slot k from its current flight-type into a water splash
// (type 0x3D). Called when a thrown item lands in the pond. Resets
// item_to_link and timer, nudges the position to the splash origin (−8 X,
// +12 Y relative to where it landed), plays the water splash SFX, and
// immediately calls Ancilla3D_ItemSplash to start the splash animation.
void Ancilla_TransmuteToSplash(int k) {  // 88c9cd
  ancilla_type[k] = 0x3d;
  ancilla_item_to_link[k] = 0;
  ancilla_timer[k] = 6;
  Ancilla_SetXY(k, Ancilla_GetX(k) - 8, Ancilla_GetY(k) + 12);
  Ancilla_Sfx2_Pan(k, 0x28);
  Ancilla3D_ItemSplash(k);
}

// Ancilla type 0x3D: Water splash animation played when an item hits the
// pond surface. Advances one frame every 6 ticks (ancilla_timer), cycling
// through 5 frames via item_to_link (0–4). At frame 5 the slot is cleared.
// Each frame is drawn by ObjectSplash_Draw using pre-built OAM tables.
void Ancilla3D_ItemSplash(int k) {  // 88ca01
  Ancilla_AllocateOamFromRegion_A_or_D_or_F(k, 8);
  if (!submodule_index && !ancilla_timer[k]) {
    ancilla_timer[k] = 6;
    if (++ancilla_item_to_link[k] == 5) {
      ancilla_type[k] = 0;
      return;
    }
  }
  ObjectSplash_Draw(k);
}

// Draws one frame of the water-splash animation for ancilla k. Renders 2 OAM
// entries per frame (skipping any entry whose Char is 0xFF). Five frames are
// defined in kObjectSplash_Draw_* (10-entry tables, 2 sprites per frame).
// Frame index is taken from ancilla_item_to_link[k]; the tables include rings,
// expanding ripple arcs, and spray droplets. Palette 4 is used for all entries.
void ObjectSplash_Draw(int k) {  // 88ca22
  static const int8 kObjectSplash_Draw_X[10] = {0, 0, 0, 0, 11, -3, 15, -7, 15, -7};
  static const int8 kObjectSplash_Draw_Y[10] = {0, 0, -6, 0, -13, -8, -17, -4, -17, -4};
  static const uint8 kObjectSplash_Draw_Char[10] = {0xc0, 0xff, 0xe7, 0xff, 0xaf, 0xbf, 0x80, 0x80, 0x83, 0x83};
  static const uint8 kObjectSplash_Draw_Flags[10] = {0, 0xff, 0, 0xff, 0x40, 0, 0x40, 0, 0xc0, 0x80};
  static const uint8 kObjectSplash_Draw_Ext[10] = {2, 0, 2, 0, 0, 0, 0, 0, 0, 0};
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  int j = ancilla_item_to_link[k] * 2;
  for (int i = 0; i != 2; i++, j++) {
    if (kObjectSplash_Draw_Char[j] != 0xff) {
      Ancilla_SetOam(oam, pt.x + kObjectSplash_Draw_X[j], pt.y + kObjectSplash_Draw_Y[j],
                     kObjectSplash_Draw_Char[j], kObjectSplash_Draw_Flags[j] | 0x24, 
                     kObjectSplash_Draw_Ext[j]);
      oam++;
    }
  }
}

// Ancilla type 0x29: Milestone item receipt — floats the item up to Link for
// boss prizes (Crystals, Pendants) and special floor-drop items. Unlike
// Ancilla22 this variant has the item float upward from the ground into Link's
// hands, then triggers Link_ReceiveItem when they collide.
//
// ancilla_item_to_link[k]: item ID.
//   0x10 / 0x0F (Pendant subtypes): treated as a special "G[k]-delay" path
//     that waits a brief grace period (ancilla_G[k]) before beginning the rise.
//   0x20 (Crystal): spawns sparkle each frame (AncillaAdd_OccasionalSparkle)
//     and after the item is cleared reloads the sprite dungeon palette.
//   All others: gate on dung_savegame_state_bits (0x4000 = already collected,
//     0x8000 = prize dropped) and handle byte_7E04C2 animated tile decode.
//
// Motion:
//   step 0 → no Z motion yet (item rests at floor).
//   step 1 → z_vel decrements each frame (gravity); MoveZ lifts the item.
//     When z wraps ≥ 0xf8 the item is reset (z → 0, z_vel → 0x18) and step → 2.
//   step 2 → item is at peak height, remains stationary until Link touch.
//
// Collision (Ancilla_CheckLinkCollision, range 2):
//   When Link is close and not using hookshot or auxiliary state the slot clears,
//   item_receipt_method = 3, and Link_ReceiveItem fires the normal receipt flow.
//
// Drop-shadow:
//   A layered shadow is drawn below the item. Its size code (0 = large on ground,
//   1 = medium near ground, 2 = small in air) is derived from ancilla_z[k].
//   Room 6 (Sahasrahla antechamber) uses a special cycling L[k] animation index.
void Ancilla29_MilestoneItemReceipt(int k) {  // 88ca8c
  if (ancilla_item_to_link[k] != 0x10 && ancilla_item_to_link[k] != 0x0f) {
    if (dung_savegame_state_bits & 0x4000) {
      ancilla_type[k] = 0;
      return;
    }

    if (!(dung_savegame_state_bits & 0x8000))
      return;

    if (byte_7E04C2 != 0) {
      if (byte_7E04C2 == 1) {
        if (ancilla_item_to_link[k] == 0x20) {
          sound_effect_ambient = 0x0f;
          DecodeAnimatedSpriteTile_variable(0x28);
        } else {
          DecodeAnimatedSpriteTile_variable(0x23);
        }
      }
      byte_7E04C2--;
      return;
    }
    if (!ancilla_arr3[k] && ancilla_item_to_link[k] == 0x20) {
      ancilla_arr3[k] = 1;
      palette_sp6r_indoors = 4;
      overworld_palette_aux_or_main = 0x200;
      Palette_Load_SpriteEnvironment_Dungeon();
      flag_update_cgram_in_nmi++;
    }
  } else {
    if (ancilla_G[k]) {
      ancilla_G[k]--;
      return;
    }
  }

  if (ancilla_item_to_link[k] == 0x20)
    AncillaAdd_OccasionalSparkle(k);

  if (submodule_index == 0) {
    CheckPlayerCollOut coll_out;
    if (ancilla_z[k] < 24 && Ancilla_CheckLinkCollision(k, 2, &coll_out) && related_to_hookshot == 0 && link_auxiliary_state == 0) {
      ancilla_type[k] = 0;
      if (link_player_handler_state == kPlayerState_ReceivingEther || link_player_handler_state == kPlayerState_ReceivingBombos) {
        flag_custom_spell_anim_active = 0;
        link_force_hold_sword_up = 0;
        link_player_handler_state = 0;
      }
      item_receipt_method = 3;
      Link_ReceiveItem(ancilla_item_to_link[k], 0);
      return;
    }

    if (ancilla_step[k] != 2) {
      if (ancilla_step[k] != 0) {
        ancilla_z_vel[k]--;
      }
      Ancilla_MoveZ(k);
      if (ancilla_z[k] >= 0xf8) {
        ancilla_step[k]++;
        ancilla_z_vel[k] = 0x18;
        ancilla_z[k] = 0;
      }
    }
  }

  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  OamEnt *oam = Ancilla_ReceiveItem_Draw(k, pt.x, pt.y - ancilla_z[k]);

  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 9;
    if (++ancilla_L[k] == 3)
      ancilla_L[k] = 0;
  }

  int t;
  if (ancilla_z[k] == 0) {
    t = (dungeon_room_index == 6) ? ancilla_L[k] + 4 : 0;
  } else {
    t = ancilla_z[k] < 0x20 ? 1 : 2;
  }
  AncillaDraw_Shadow(oam, t, pt.x, pt.y + 12, 0x20);
}

// Converts slot k from Ancilla22 (item-receipt hold) to type 0x3E (rising
// crystal) at the moment the boss-crystal fanfare ends. Clears X/Y velocity
// and the Y subpixel accumulator to halt any drift, then immediately calls
// Ancilla_RisingCrystal to begin the ascent frame.
void ItemReceipt_TransmuteToRisingCrystal(int k) {  // 88cbe4
  ancilla_type[k] = 0x3e;
  ancilla_y_vel[k] = 0;
  ancilla_x_vel[k] = 0;
  ancilla_y_subpixel[k] = 0;
  Ancilla_RisingCrystal(k);
}

// Ancilla type 0x3E: Rising crystal post-boss cutscene. The crystal floats
// upward from where it was received, sparkling, until it reaches screen row
// 0x49 (the Triforce platform row). On arrival:
//   - Marks the crystal as collected (link_has_crystals |= bit for palace).
//   - Sets submodule_index = 0x18 (triggers the crystal convergence cutscene).
//   - Clears the aux palette buffer and begins fade (palette_filter_countdown,
//     darkening_or_lightening_screen = 0).
//
// Z is zeroed each frame so the sprite is never offset vertically by the
// height field. y_vel ratchets upward by 1 per frame but is clamped at
// 0xF0 (minimum speed, so the crystal never stops rising).
void Ancilla_RisingCrystal(int k) {  // 88cbf2
  ancilla_z[k] = 0;
  AncillaAdd_OccasionalSparkle(k);
  uint8 yy = ancilla_y_vel[k] - 1;
  if (yy < 0xf0)
    yy = 0xf0;
  ancilla_y_vel[k] = yy;
  Ancilla_MoveY(k);

  uint16 y = Ancilla_GetY(k) - BG2VOFS_copy;
  if (y < 0x49) {
    Ancilla_SetY(k, 0x49 + BG2VOFS_copy);
    if (!submodule_index) {
      link_has_crystals |= kDungeonCrystalPendantBit[BYTE(cur_palace_index_x2) >> 1];
      submodule_index = 0x18;
      subsubmodule_index = 0;
      memset(aux_palette_buffer + 0x20, 0, sizeof(uint16) * 0x60);
      palette_filter_countdown = 0;
      darkening_or_lightening_screen = 0;
    }
  }

  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  Ancilla_ReceiveItem_Draw(k, pt.x, pt.y);
}

// Spawns a sword-charge sparkle every 8 frames (frame_counter & 7 == 0).
// Used to add ambient glitter to floating crystals and similar sustained effects.
void AncillaAdd_OccasionalSparkle(int k) {  // 88cc93
  if (!(frame_counter & 7))
    AncillaAdd_SwordChargeSparkle(k);
}

// Ancilla type 0x43: Ganon's Tower seal-breaking cutscene. Plays when the
// seven Crystals are brought to the tower entrance. A single central crystal
// drops from above, then 6 orbital crystals expand outward while 24
// independent sparkle particles fire off.
//
// ancilla_step[k] state machine:
//   0 (descent): central crystal falls via y_vel (clamped at 0xF0). When the
//     screen-relative Y falls below 0x38, the crystal locks at that row,
//     breaktowerseal_x/y are recorded, and step → 1. Shows a dialogue message
//     (0x13b) and plays a fanfare (music_control = 0xF1).
//   1 (orbital expand): breaktowerseal_var4 tracks the orbital ring radius,
//     incremented via a mock-ancilla x_vel = 16 move. When radius reaches 48
//     step → 2.
//   2 (hold): decrements breaktowerseal_var5 once per frame; at 0 sets
//     trigger_special_entrance = 5 (opens the tower gate) and step → 3.
//   3 (orbital collapse): radius accelerates outward at x_vel = 48. When ≥ 240
//     the sprite environment palette is restored and the slot is freed.
//
// Each frame (label_b): orbiting crystals are drawn at 7 positions using
//   Ancilla_GetRadialProjection(breaktowerseal_var3[j], radius). Their base
//   screen positions are saved into breaktowerseal_base_sparkle_* for use
//   by GTCutscene_ActivateSparkle. GTCutscene_SparkleALot draws the 24
//   independent short-lived sparkles.
void Ancilla43_GanonsTowerCutscene(int k) {  // 88cca0
  OamEnt *oam = GetOamCurPtr();
  if (!ancilla_step[k]) {
    uint8 yy = ancilla_y_vel[k] - 1;
    ancilla_y_vel[k] = (yy < 0xf0) ? 0xf0 : yy;
    Ancilla_MoveY(k);
    uint16 x = Ancilla_GetX(k), y = Ancilla_GetY(k);
    if ((uint16)(y - BG2VOFS_copy) >= 0x38)
      goto lbl_else;
    breaktowerseal_y = 0x38 + 8 + BG2VOFS_copy;
    breaktowerseal_x = x + 8;
    Ancilla_SetY(k, 0x38 + BG2VOFS_copy);
    ancilla_step[k]++;
    sound_effect_ambient = 5;
    music_control = 0xf1;
    dialogue_message_index = 0x13b;
    Main_ShowTextMessage();
    goto label_a;
  }
lbl_else:
  if (ancilla_step[k] == 1 && submodule_index == 0) {
    ancilla_x_vel[k] = 16;
    uint8 bak0 = ancilla_x_lo[k];
    uint8 bak1 = ancilla_x_hi[k];
    ancilla_x_lo[k] = breaktowerseal_var4;
    ancilla_x_hi[k] = 0;
    Ancilla_MoveX(k);
    breaktowerseal_var4 = ancilla_x_lo[k];
    ancilla_x_lo[k] = bak0;
    ancilla_x_hi[k] = bak1;
    if (breaktowerseal_var4 >= 48) {
      breaktowerseal_var4 = 48;
      ancilla_step[k]++;
    }
  }
  if (submodule_index)
    goto label_b;
  if (ancilla_step[k] == 0)
    goto label_a;
  if (ancilla_step[k] == 1)
    goto label_b;
  if (ancilla_step[k] == 2) {
    if (--breaktowerseal_var5 == 0) {
      trigger_special_entrance = 5;
      subsubmodule_index = 0;
      BYTE(R16) = 0;
      ancilla_step[k]++;
    }
  } else {
    ancilla_x_vel[k] = 48;
    uint8 bak0 = ancilla_x_lo[k];
    uint8 bak1 = ancilla_x_hi[k];
    ancilla_x_lo[k] = breaktowerseal_var4;
    ancilla_x_hi[k] = 0;
    Ancilla_MoveX(k);
    breaktowerseal_var4 = ancilla_x_lo[k];
    ancilla_x_lo[k] = bak0;
    ancilla_x_hi[k] = bak1;
    if (breaktowerseal_var4 >= 240) {
      palette_sp6r_indoors = 0;
      overworld_palette_aux_or_main = 0x200;
      Palette_Load_SpriteEnvironment_Dungeon();
      flag_update_cgram_in_nmi++;
      ancilla_type[k] = 0;
      return;
    }
  }
  uint8 astep;
label_b:


  astep = ancilla_step[k];
  if (astep != 0)
    oam = GTCutscene_SparkleALot(oam);

  for (int j = 6; j >= 0; j--) {
    if (submodule_index == 0 && astep != 1 && !(frame_counter & 1))
      breaktowerseal_var3[j] = breaktowerseal_var3[j] + 1 & 63;
    AncillaRadialProjection arp = Ancilla_GetRadialProjection(breaktowerseal_var3[j], breaktowerseal_var4);
    int x = (arp.r6 ? -arp.r4 : arp.r4) + breaktowerseal_x - 8 - BG2HOFS_copy;
    int y = (arp.r2 ? -arp.r0 : arp.r0) + breaktowerseal_y - 8 - BG2VOFS_copy;

    breaktowerseal_base_sparkle_x_lo[j] = x;
    breaktowerseal_base_sparkle_x_hi[j] = x >> 8;

    breaktowerseal_base_sparkle_y_lo[j] = y;
    breaktowerseal_base_sparkle_y_hi[j] = y >> 8;

    AncillaDraw_GTCutsceneCrystal(oam, x, y);
    oam++;
  }
  Point16U info;
label_a:
  Ancilla_PrepAdjustedOamCoord(k, &info);

  breaktowerseal_base_sparkle_x_lo[7] = info.x;
  breaktowerseal_base_sparkle_x_hi[7] = info.x >> 8;
  breaktowerseal_base_sparkle_y_lo[7] = info.y;
  breaktowerseal_base_sparkle_y_hi[7] = info.y >> 8;

  AncillaDraw_GTCutsceneCrystal(oam, info.x, info.y);

  if (!ancilla_step[k])
    AncillaAdd_OccasionalSparkle(k);
  else if (!submodule_index)
    GTCutscene_ActivateSparkle();
}

// Renders one Ganon's Tower cutscene crystal OAM entry at screen position
// (x, y). Uses tile 0x24 (16×16 crystal), palette 0x3c (palette 7, priority 3).
void AncillaDraw_GTCutsceneCrystal(OamEnt *oam, int x, int y) {  // 88ceaa
  Ancilla_SetOam_Safe(oam, x, y, 0x24, 0x3c, 2);
}

// Allocates one free sparkle slot (breaktowerseal_sparkle_var1[k] == 0xFF)
// from the 24-entry pool and initialises it at a randomly jittered position
// near one of the 8 tracked crystal positions (k & 7 selects the base). The
// random offset is split into high nibble → X and low nibble → Y. The sparkle
// starts at frame 0 with a 4-tick duration counter (sparkle_var2 = 4).
void GTCutscene_ActivateSparkle() {  // 88cec7
  for (int k = 0x17; k >= 0; k--) {
    if (breaktowerseal_sparkle_var1[k] == 0xff) {
      breaktowerseal_sparkle_var1[k] = 0;
      breaktowerseal_sparkle_var2[k] = 4;
      int r = GetRandomNumber();
      int x = breaktowerseal_base_sparkle_x_hi[k & 7] << 8 | breaktowerseal_base_sparkle_x_lo[k & 7];
      int y = breaktowerseal_base_sparkle_y_hi[k & 7] << 8 | breaktowerseal_base_sparkle_y_lo[k & 7];
      x += r >> 4;
      y += r & 0xf;
      breaktowerseal_sparkle_x_lo[k] = x;
      breaktowerseal_sparkle_x_hi[k] = x >> 8;
      breaktowerseal_sparkle_y_lo[k] = y;
      breaktowerseal_sparkle_y_hi[k] = y >> 8;
      return;
    }
  }
}

// Draws all 24 active Ganon's Tower cutscene sparkle particles and advances
// their animation. Each particle cycles through 3 frames (kSwordChargeSpark_Char
// tiles) at 4 ticks per frame via sparkle_var2. When a particle completes its
// 3-frame cycle (var1 reaches 3) it is marked inactive (var1 = 0xFF). Returns
// the updated OAM pointer for the caller to continue appending sprites.
OamEnt *GTCutscene_SparkleALot(OamEnt *oam) {  // 88cf35
  static const uint8 kSwordChargeSpark_Char[3] = {0xb7, 0x80, 0x83};
  static const uint8 kSwordChargeSpark_Flags[3] = {4, 4, 0x84};
  for (int k = 0x17; k >= 0; k--) {
    if (breaktowerseal_sparkle_var1[k] == 0xff)
      continue;

    if (sign8(--breaktowerseal_sparkle_var2[k])) {
      breaktowerseal_sparkle_var2[k] = 4;
      if (++breaktowerseal_sparkle_var1[k] == 3) {
        breaktowerseal_sparkle_var1[k] = 0xff;
        continue;
      }
    }

    int x = breaktowerseal_sparkle_x_hi[k] << 8 | breaktowerseal_sparkle_x_lo[k];
    int y = breaktowerseal_sparkle_y_hi[k] << 8 | breaktowerseal_sparkle_y_lo[k];
    int j = breaktowerseal_sparkle_var1[k];
    Ancilla_SetOam(oam, x, y, kSwordChargeSpark_Char[j], kSwordChargeSpark_Flags[j] | 0x30, 0);
    oam++;
  }
  return oam;
}

// Ancilla type 0x36: Flute (Ocarina) item — the flute drops from above and
// bounces until it comes to rest, then waits for Link to pick it up.
//
// kFlute_Vels[4]: Z-velocity loaded at the start of each bounce step. Four
//   steps (0–3): high bounce (0x18), medium (0x10), small (0x0A), rest (0).
//
// Physics (step 0–2):
//   z_vel is decremented by 2 each frame (gravity). Ancilla_MoveZ integrates
//   Z. When Z underflows (sign8) or reaches 0xF0 (just below ground), the
//   flute bounces: step is incremented and z_vel is reloaded from kFlute_Vels.
//   Ancilla_MoveX drifts it horizontally throughout the flight.
//
// Rest (step 3):
//   The flute waits for Link_CheckLinkCollision(range 2). On contact (and
//   Link not hookshot-locked) Link_ReceiveItem(0x14) is called to give the
//   Ocarina and the slot is freed.
//
// If the OAM y-coordinate resolves to 0xF0 (off-screen) the slot is also
// freed automatically, preventing an item lost off a ledge from looping forever.
void Ancilla36_Flute(int k) {  // 88cfaa
  static const uint8 kFlute_Vels[4] = {0x18, 0x10, 0xa, 0};

  if (!submodule_index) {
    if (ancilla_step[k] != 3) {
      ancilla_z_vel[k] -= 2;
      Ancilla_MoveX(k);
      Ancilla_MoveZ(k);
      if (sign8(ancilla_z[k]) || ancilla_z[k] >= 0xf0) {
        ancilla_z_vel[k] = kFlute_Vels[++ancilla_step[k]];
        ancilla_z[k] = 0;
      }
    } else {
      CheckPlayerCollOut coll_out;
      if (Ancilla_CheckLinkCollision(k, 2, &coll_out) && !related_to_hookshot && link_auxiliary_state == 0) {
        ancilla_type[k] = 0;
        item_receipt_method = 0;
        Link_ReceiveItem(0x14, 0);
        return;
      }
    }
  }

  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  Ancilla_SetOam(oam, pt.x, pt.y - (int8)ancilla_z[k], 0x24, HIBYTE(oam_priority_value) | 4, 2);
  if (oam->y == 0xf0)
    ancilla_type[k] = 0;
}

// Ancilla type 0x37: Weathervane explosion cutscene — debris shower when
// the Flute Boy's weathervane is destroyed by the duck's arrival.
//
// Throttle: weathervane_var2 decrements every frame; the body only runs on
//   alternate ticks (var2 would normally be 2, but the code resets to 1,
//   meaning it runs every other frame).
//
// One-shot init (weathervane_var1 == 0):
//   Sets var1 = 1 and triggers the destruction music (music_control = 0xF3).
//
// Step 0 → 1 transition (aux_timer expires):
//   Calls Overworld_AlterWeathervane (removes the vane sprite) and
//   AncillaAdd_CutsceneDuck to spawn the duck (ancilla type 0x38).
//
// 12-piece debris simulation (weathervane_arr*):
//   Each piece (index i) stores its own position, velocity, Z, and two draw
//   state bytes (arr11 = frame timer, arr12 = draw-frame index 0/1 or 0xFF
//   when done). Each tick:
//     - arr12 toggles between 0 and 1 every arr11 frames (alternating tiles).
//     - Position is advanced via Ancilla_MoveX/Y/Z using the piece's own
//       velocity fields loaded into the scratch ancilla slot k.
//     - z_vel is decremented by 1 (gravity, stored back into arr5[i]).
//     - When Z ≥ 0xF0 (ground collision) arr12 is set to 0xFF (done).
//   The slot clears when all 12 pieces are done.
void Ancilla37_WeathervaneExplosion(int k) {  // 88d03d
  if (--weathervane_var2)
    return;
  weathervane_var2 = 1;
  if (!weathervane_var1) {
    weathervane_var1 = 1;
    music_control = 0xf3;
  }
  if (--ancilla_G[k])
    return;
  ancilla_G[k] = 1;
  if (!ancilla_arr3[k]) {
    ancilla_arr3[k] += 1;
    Ancilla_Sfx2_Near(0xc);
  }
  if (!ancilla_step[k] && sign8(--ancilla_aux_timer[k])) {
    ancilla_step[k] = 1;
    Overworld_AlterWeathervane();
    AncillaAdd_CutsceneDuck(0x38, 0);
  }
  weathervane_var13 = k;
  weathervane_var14 = 0;
  for (int i = 11; i >= 0; i--) {
    if (weathervane_arr12[i] == 0xff)
      continue;
    if (sign8(--weathervane_arr11[i])) {
      weathervane_arr11[i] = 1;
      weathervane_arr12[i] ^= 1;
    }

    ancilla_item_to_link[k] = weathervane_arr12[i];
    ancilla_y_lo[k] = weathervane_arr6[i];
    ancilla_y_hi[k] = weathervane_arr7[i];
    ancilla_x_lo[k] = weathervane_arr8[i];
    ancilla_x_hi[k] = weathervane_arr9[i];
    ancilla_z[k] = weathervane_arr10[i];
    ancilla_y_vel[k] = weathervane_arr3[i];
    ancilla_x_vel[k] = weathervane_arr4[i];
    weathervane_arr5[i] = ancilla_z_vel[k] = weathervane_arr5[i] - 1;

    Ancilla_MoveY(k);
    Ancilla_MoveX(k);
    Ancilla_MoveZ(k);

    uint8 c = (ancilla_z[k] < 0xf0) ? 0 : 0xff;
    AncillaDraw_WeathervaneExplosionWoodDebris(k);
    if (sign8(c))
      weathervane_arr12[i] = c;
    weathervane_arr6[i] = ancilla_y_lo[k];
    weathervane_arr7[i] = ancilla_y_hi[k];
    weathervane_arr8[i] = ancilla_x_lo[k];
    weathervane_arr9[i] = ancilla_x_hi[k];
    weathervane_arr10[i] = ancilla_z[k];
  }
  for (int i = 11; i >= 0; i--) {
    if (weathervane_arr12[i] != 0xff)
      return;
  }
  ancilla_type[k] = 0;
}

// Draws one piece of weathervane wood debris using ancilla slot k (which has
// been loaded with the piece's position). kWeathervane_Explode_Char[2]
// (tiles 0x4E and 0x4F) are the two alternating sprite frames.
// weathervane_var14 is a running OAM offset (incremented by 4 each call)
// so each debris piece lands in a distinct OAM slot without overwriting the
// previous one. Z-height is subtracted from screen Y to lift the piece above
// the ground. Pieces marked with a negative draw-frame index are skipped.
void AncillaDraw_WeathervaneExplosionWoodDebris(int k) {  // 88d188
  static const uint8 kWeathervane_Explode_Char[2] = {0x4e, 0x4f};
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  pt.y -= (int8)ancilla_z[k];
  int i = ancilla_item_to_link[k];
  if (sign8(i))
    return;
  Ancilla_SetOam(GetOamCurPtr() + (weathervane_var14 >> 2), pt.x, pt.y, kWeathervane_Explode_Char[i], 0x3c, 0);
  weathervane_var14 += 4;
}

// Ancilla type 0x38: Cutscene duck (the Flute Boy's duck) arriving at the
// weathervane. Animates a two-phase bird-flight sequence:
//
//   Phase 1 (ancilla_L[k] == 0): Descent. ancilla_item_to_link[k] counts
//     down from an initial height value. While it is positive the duck bobs
//     up and down using z_vel oscillating ±1 per tick (clamped at 12 in each
//     direction via ancilla_step[k] flip). Once item_to_link reaches 0 the
//     duck lands: x_vel is set to kTravelBirdIntro_Tab1[0] (28), z_vel set
//     to −16, and L[k] is incremented to enter phase 2.
//
//   Phase 2 (ancilla_L[k] == 1): Fly-away. x_vel oscillates between +1 and
//     −1 per tick, bounded by kTravelBirdIntro_Tab1[ancilla_S[k]] (28 or 60).
//     Swapping step bits reverses the bounce. z_vel is derived from the
//     remaining distance to the bound (triangular wave) so the duck traces a
//     smooth arc. Once ancilla_L[k] reaches 7 (L[k]++ per zero-crossing),
//     ancilla_S[k] is set to 1, widening the oscillation range to 60.
//
// Each frame: Ancilla_MoveX/Z apply velocity. flag_travel_bird is written from
//   kTravelBird_DmaStuffs[K[k]+1] to drive the wing-flap animation DMA.
// The duck is drawn with the correct horizontal flip (ancilla_dir[k] 2 = left,
//   3 = right) and a small drop-shadow.
// Termination: when the screen X exits right (≥ 248) the slot is cleared,
//   submodule_index is reset, and link_item_flute is set to 3 (possessed state).
void Ancilla38_CutsceneDuck(int k) {  // 88d1d8
  static const uint8 kTravelBirdIntro_Tab0[2] = {0x40, 0};
  static const uint8 kTravelBirdIntro_Tab1[2] = {28, 60};

  if (!(frame_counter & 31))
    Ancilla_Sfx3_Pan(k, 0x1e);

  if (sign8(--ancilla_arr3[k])) {
    ancilla_arr3[k] = 3;
    ancilla_K[k] ^= 1;
  }

  if (!--ancilla_aux_timer[k]) {
    ancilla_aux_timer[k] = 1;
    if (!ancilla_L[k]) {
      if (!sign8(--ancilla_item_to_link[k])) {
        ancilla_z_vel[k] += ancilla_step[k] ? 1 : -1;
        if (abs8(ancilla_z_vel[k]) >= 12)
          ancilla_step[k] ^= 1;
        goto after_stuff;
      }
      ancilla_item_to_link[k] = 0;
      ancilla_step[k] = 0;
      ancilla_x_vel[k] = kTravelBirdIntro_Tab1[0];
      ancilla_z_vel[k] = -16;
      ancilla_L[k]++;
      ancilla_step[k] = 3;
    }
    ancilla_x_vel[k] += (ancilla_step[k] & 1) == 0 ? 1 : -1;
    uint8 absx = abs8(ancilla_x_vel[k]);
    if (absx == 0 && ++ancilla_L[k] == 7)
      ancilla_S[k] = 1;
    if (absx >= kTravelBirdIntro_Tab1[ancilla_S[k]]) {
      ancilla_step[k] ^= 3;
    }
    ancilla_dir[k] = sign8(ancilla_x_vel[k]) ? 2 : 3;
    uint8 t = (uint8)(kTravelBirdIntro_Tab1[ancilla_S[k]] - absx) >> 1;
    ancilla_z_vel[k] = (ancilla_step[k] & 2) ? -t : t;
  }
after_stuff:
  Ancilla_MoveX(k);
  Ancilla_MoveZ(k);
  BYTE(flag_travel_bird) = kTravelBird_DmaStuffs[ancilla_K[k] + 1];
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);
  OamEnt *oam = GetOamCurPtr();
  Ancilla_SetOam(oam, info.x + kTravelBird_Draw_X[0], info.y + (int8)ancilla_z[k] + kTravelBird_Draw_Y[0],
                 kTravelBird_Draw_Char[0],
                 kTravelBird_Draw_Flags[0] | 0x30 | kTravelBirdIntro_Tab0[ancilla_dir[k] & 1], 2);
  oam++;
  AncillaDraw_Shadow(oam, 1, info.x, info.y + 48, 0x30);
  if (!sign16(info.x) && info.x >= 248) {
    ancilla_type[k] = 0;
    submodule_index = 0;
    link_item_flute = 3;
  }
}

// Ancilla type 0x23: Link transformation poof — the cloud-puff effect shown
// when Link transforms (e.g. Light World / Dark World transition) or de-transforms.
//
// Advances one frame every 7 ticks (ancilla_aux_timer). Three frames total;
// at frame 3 the slot is cleared. On termination:
//   ancilla_step[k] == 0 (transform into bunny or human):
//     Resets link_animation_steps and link_visibility_status to 0, then
//     checks overworld_screen_index bit 0x40 to determine the current world
//     and sets link_is_bunny accordingly. Loads the appropriate gear palette.
//   ancilla_step[k] != 0 (transform without palette change): only frees slot.
//
// Drawing is delegated to MorphPoof_Draw at each tick while the timer is active.
void Ancilla23_LinkPoof(int k) {  // 88d3bc
  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 7;
    if (++ancilla_item_to_link[k] == 3) {
      ancilla_type[k] = 0;
      link_is_transforming = 0;
      link_cant_change_direction = 0;
      if (!ancilla_step[k]) {
        link_animation_steps = 0;
        link_visibility_status = 0;
        link_is_bunny = link_is_bunny_mirror = BYTE(overworld_screen_index) & 0x40 ? 1 : 0;
        if (link_is_bunny)
          LoadGearPalettes_bunny();
        else
          LoadActualGearPalettes();
      }
      return;
    }
  }
  MorphPoof_Draw(k);
}

// Draws one frame of the morph-poof cloud. Three frames (item_to_link 0–2)
// are defined in kMorphPoof_Char/Ext, each with 4 OAM entries placed at
// offsets in kMorphPoof_X/Y and flipped according to kMorphPoof_Flags.
//
// Frame 0 uses a 16×16 tile (ext = 2) so the loop breaks after the first
// sprite. Frames 1 and 2 use 8×8 tiles and emit all 4 sprites.
//
// When sort_sprites_setting and ancilla_floor[k] are both set (Upper floor
// in a 2-floor dungeon room) and the poof is not stalling (flag_for_boomerang
// check), oam_cur_ptr/oam_ext_cur_ptr are reset to the upper-floor OAM region
// (0x8D0 / 0xA20+0x34) so the poof sorts correctly above lower-floor sprites.
void MorphPoof_Draw(int k) {  // 88d3fd
  static const int8 kMorphPoof_X[12] = {0, 0, 0, 0, 0, 8, 0, 8, -4, 12, -4, 12};
  static const int8 kMorphPoof_Y[12] = {0, 0, 0, 0, 0, 0, 8, 8, -4, -4, 12, 12};
  static const uint8 kMorphPoof_Flags[12] = {0, 0xff, 0xff, 0xff, 0x40, 0, 0xc0, 0x80, 0, 0x40, 0x80, 0xc0};
  static const uint8 kMorphPoof_Char[3] = {0x86, 0xa9, 0x9b};
  static const uint8 kMorphPoof_Ext[3] = {2, 0, 0};
  if (sort_sprites_setting && ancilla_floor[k] && (!flag_for_boomerang_in_place || !(frame_counter & 1))) {
    oam_cur_ptr = 0x8d0;
    oam_ext_cur_ptr = 0xa20 + (0xd0 >> 2);
  }
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);
  OamEnt *oam = GetOamCurPtr();
  int j = ancilla_item_to_link[k];
  uint8 ext = kMorphPoof_Ext[j];
  uint8 chr = kMorphPoof_Char[j];
  for (int i = 0; i < 4; i++, oam++) {
    Ancilla_SetOam(oam, info.x + kMorphPoof_X[j * 4 + i], info.y + kMorphPoof_Y[j * 4 + i], chr,
                   kMorphPoof_Flags[j * 4 + i] | 4 | HIBYTE(oam_priority_value), ext);
    if (ext == 2)
      break;
  }
}

// Ancilla type 0x40: Dwarf (smithy companion) poof — the transformation cloud
// used when the dwarf blacksmith is tempered. Same 3-frame timing as
// Ancilla23_LinkPoof but on completion sets tagalong_var5 = 0 to release
// the dwarf follower slot. Delegates drawing to MorphPoof_Draw.
void Ancilla40_DwarfPoof(int k) {  // 88d49a
  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 7;
    if (++ancilla_item_to_link[k] == 3) {
      ancilla_type[k] = 0;
      tagalong_var5 = 0;
      return;
    }
  }
  MorphPoof_Draw(k);
}

// Ancilla type 0x3F: Bush destruction poof. Plays a 4-frame expanding cloud
// (kBushPoof_Draw_* tables, 4 sprites per frame) when a bush is cut or lifted.
// Advances one frame every 7 ticks (ancilla_timer). At frame 4 the slot clears.
// OAM is allocated from region C (ground-level priority). Palette 4 is used.
void Ancilla3F_BushPoof(int k) {  // 88d519
  static const int8 kBushPoof_Draw_X[16] = {0, 8, 0, 8, 0, 8, 0, 8, 0, 8, 0, 8, -2, 10, -2, 10};
  static const int8 kBushPoof_Draw_Y[16] = {0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8, -2, -2, 10, 10};
  static const uint8 kBushPoof_Draw_Char[16] = {0x86, 0x87, 0x96, 0x97, 0xa9, 0xa9, 0xa9, 0xa9, 0x8a, 0x8b, 0x9a, 0x9b, 0x9b, 0x9b, 0x9b, 0x9b};
  static const uint8 kBushPoof_Draw_Flags[16] = {0, 0, 0, 0, 0, 0x40, 0x80, 0xc0, 0, 0, 0, 0, 0xc0, 0x80, 0x40, 0};

  if (ancilla_timer[k] == 0) {
    ancilla_timer[k] = 7;
    if (++ancilla_item_to_link[k] == 4) {
      ancilla_type[k] = 0;
      return;
    }
  }
  Oam_AllocateFromRegionC(0x10);
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();

  int j = ancilla_item_to_link[k] * 4;
  for (int i = 0; i < 4; i++, j++, oam++) {
    Ancilla_SetOam(oam, pt.x + kBushPoof_Draw_X[j], pt.y + kBushPoof_Draw_Y[j],
                   kBushPoof_Draw_Char[j], kBushPoof_Draw_Flags[j] | 4 | HIBYTE(oam_priority_value), 0);
  }
}

// Ancilla type 0x26: Sword-swing sparkle — the flash produced at the sword
// tip when Link swings and misses (no hit). Plays a 4-frame single-tick
// animation (one frame per tick, controlled by ancilla_aux_timer[k]).
//
// The sprite tracks Link's exact position each frame (Ancilla_SetXY → link
// coordinates). The table index is (frame * 3 + direction * 12) so all 48
// entries cover 4 frames × 4 directions × 3 OAM sprites per position.
// Entries with Char == 0xFF are skipped. Flags include palette 4 and the
// current OAM priority.
void Ancilla26_SwordSwingSparkle(int k) {  // 88d65a
  static const int8 kSwordSwingSparkle_X[48] = {
    5,  10, -1,  5, 10, -4,  5, 10,  -4,  -4, -1,  -1,   0,   5,  -1,   0,
    5,  14,  0,  5, 14, 14, -1, -1, -23, -27, -1, -23, -27, -22, -23, -27,
    -22, -22, -1, -1, 32, 35, -1, 32,  35,  30, 32,  35,  30,  30,  -1,  -1,
  };
  static const int8 kSwordSwingSparkle_Y[48] = {
    -22, -18, -1, -22, -18, -17, -22, -18, -17, -17, -1, -1, 35, 40, -1, 35,
    40,  37, 35,  40,  37,  37,  -1,  -1,   2,   7, -1,  2,  7, 19,  2,  7,
    19,  19, -1,  -1,   2,   7,  -1,   2,   7,  19,  2,  7, 19, 19, -1, -1,
  };
  static const uint8 kSwordSwingSparkle_Char[48] = {
    0xb7, 0xb7, 0xff, 0x80, 0x80, 0xb7, 0x83, 0x83, 0x80, 0x83, 0xff, 0xff, 0xb7, 0xb7, 0xff, 0x80,
    0x80, 0xb7, 0x83, 0x83, 0x80, 0x83, 0xff, 0xff, 0xb7, 0xb7, 0xff, 0x80, 0x80, 0xb7, 0x83, 0x83,
    0x80, 0x83, 0xff, 0xff, 0xb7, 0xb7, 0xff, 0x80, 0x80, 0xb7, 0x83, 0x83, 0x80, 0x83, 0xff, 0xff,
  };
  static const uint8 kSwordSwingSparkle_Flags[48] = {
    0,    0, 0xff,    0, 0,    0, 0x80, 0x80, 0, 0x80, 0xff, 0xff, 0,    0, 0xff,    0,
    0,    0, 0x80, 0x80, 0, 0x80, 0xff, 0xff, 0,    0, 0xff,    0, 0,    0, 0x80, 0x80,
    0, 0x80, 0xff, 0xff, 0,    0, 0xff,    0, 0,    0, 0x80, 0x80, 0, 0x80, 0xff, 0xff,
  };
  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 0;
    if (++ancilla_item_to_link[k] == 4) {
      ancilla_type[k] = 0;
      return;
    }
  }
  Ancilla_SetXY(k, link_x_coord, link_y_coord);

  Point16U info;
  Ancilla_PrepOamCoord(k, &info);

  k = ancilla_item_to_link[k] * 3 + ancilla_dir[k] * 12;

  OamEnt *oam = GetOamCurPtr();
  for (int n = 2; n >= 0; n--, k++, oam++) {
    uint8 chr = kSwordSwingSparkle_Char[k];
    if (chr == 0xff)
      continue;
    Ancilla_SetOam(oam, info.x + kSwordSwingSparkle_X[k], info.y + kSwordSwingSparkle_Y[k],
                   chr, kSwordSwingSparkle_Flags[k] | 0x4 | (oam_priority_value >> 8), 0);
  }
}

// Ancilla type 0x2A: Spin-attack charge sparkle — initial build-up phase.
// Plays 6 timed animation frames (kInitialSpinSpark_Timer[0..5]). At frame 5:
//   ancilla_step[k] != 0 → spin attack with full charge; calls AddSwordBeam
//     to fire a projectile beam.
//   ancilla_step[k] == 0 → no charge; calls
//     SpinAttackSparkleA_TransmuteToNextSpark to convert this slot into type
//     0x2B (the radial spin sparkle).
// Calls SpinSpark_Draw with an initial (-1) radius argument on each frame.
void Ancilla2A_SpinAttackSparkleA(int k) {  // 88d7b2
  static const uint8 kInitialSpinSpark_Timer[6] = {4, 2, 3, 3, 2, 1};
  if (!submodule_index && sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 0;
    if (!ancilla_timer[k]) {
      int j = ++ancilla_item_to_link[k];
      ancilla_timer[k] = kInitialSpinSpark_Timer[j];
      if (j == 5) {
        if (ancilla_step[k])
          AddSwordBeam(j);
        else
          SpinAttackSparkleA_TransmuteToNextSpark(k);
        return;
      }
    }
  }
  if (!ancilla_item_to_link[k])
    return;
  SpinSpark_Draw(k, -1);
}

// Converts slot k from Ancilla2A (charge sparkle) to Ancilla2B (radial spin
// sparkle, type 0x2B). Loads swordbeam_arr[0..3] and swordbeam_var1 from
// kTransmuteSpinSpark_Arr indexed by link_direction_facing * 2 — these are the
// initial angular positions of the 4 orbiting spark points in the 0..63 circle.
// Sets swordbeam_var2 = 20 (starting orbital radius). Positions the ancilla at
// link_x/y + directional offsets from kTransmuteSpinSpark_X/Y, then immediately
// calls Ancilla2B_SpinAttackSparkleB to start the first orbital frame.
void SpinAttackSparkleA_TransmuteToNextSpark(int k) {  // 88d86d
  static const uint8 kTransmuteSpinSpark_Arr[16] = {0x21, 0x20, 0x1f, 0x1e, 3, 2, 1, 0, 0x12, 0x11, 0x10, 0xf, 0x31, 0x30, 0x2f, 0x2e};
  static const int8 kTransmuteSpinSpark_X[4] = {-3, 21, 25, -8};
  static const int8 kTransmuteSpinSpark_Y[4] = {28, -2, 24, 6};

  ancilla_type[k] = 0x2b;
  int j = link_direction_facing * 2;
  swordbeam_arr[0] = kTransmuteSpinSpark_Arr[j + 0];
  swordbeam_arr[1] = kTransmuteSpinSpark_Arr[j + 1];
  swordbeam_arr[2] = kTransmuteSpinSpark_Arr[j + 2];
  swordbeam_arr[3] = swordbeam_var1 = kTransmuteSpinSpark_Arr[j + 3];
  ancilla_aux_timer[k] = 2;
  ancilla_item_to_link[k] = 0x4c;
  ancilla_arr3[k] = 8;
  ancilla_step[k] = 0;
  ancilla_L[k] = 0;
  ancilla_arr1[k] = 255;
  swordbeam_var2 = 20;

  swordbeam_temp_x = link_x_coord + 8;
  swordbeam_temp_y = link_y_coord + 12;

  j = link_direction_facing>>1;
  Ancilla_SetXY(k,
      link_x_coord + kTransmuteSpinSpark_X[j],
      link_y_coord + kTransmuteSpinSpark_Y[j]);
  Ancilla2B_SpinAttackSparkleB(k);
}

// Ancilla type 0x2B: Radial spin-attack sparkle — the 4 rotating sparks that
// orbit the sword while the spin-attack charge completes.
//
// item_to_link[k] counts down from 0x4C (76) toward 0, decremented by 3 per
// tick. The phase (ancilla_step[k]) is derived from the countdown value:
//   ≥ 0x42 → step 0 (1 visible spark).
//   == 0x46 → step 1 (2 sparks).
//   == 0x43 → step 2 (3 sparks).
//   < 0x42 → step 3 (all 4 sparks).
// When item_to_link < 13 (= 0xD) ancilla_L[k] is set to 1, switching to the
// SpinAttackSparkleB_Closer wind-down path.
//
// Each tick swordbeam_arr[i] is advanced by +4 (mod 64) to rotate each spark
// around the 64-step radial table. Ancilla_GetRadialProjection maps angle +
// swordbeam_var2 (radius) to (x, y) screen offsets via Sparkle_PrepOamFromRadial.
//
// ancilla_arr1[k] cycles 0→1→2→3 every arr3[k] ticks; on the 4th step
// swordbeam_var1 is advanced +9 (mod 64). arr1 also selects the extra trailing
// sparkle tile from kSpinSpark_Char2.
// aux_timer controls a blink: at 0 the palette flag alternates (2 vs 4).
// When item_to_link == 7 the 3rd OAM entry is flagged as 16×16 (ext byte 1).
void Ancilla2B_SpinAttackSparkleB(int k) {  // 88d8fd
  static const uint8 kSpinSpark_Char[4] = {0xd7, 0xb7, 0x80, 0x83};

  if (ancilla_L[k]) {
    SpinAttackSparkleB_Closer(k);
    return;
  }
  uint8 flags = 2;
  if (!submodule_index) {
    uint8 t = (ancilla_item_to_link[k] -= 3);
    if (t < 13) {
      ancilla_aux_timer[k] = 1;
      ancilla_L[k] = 1;
      ancilla_item_to_link[k] = 0;
      SpinAttackSparkleB_Closer(k);
      return;
    }
    ancilla_step[k] = (t < 0x42) ? 3 : (t == 0x46) ? 1 : (t == 0x43) ? 2 : 0;
    if (sign8(--ancilla_aux_timer[k])) {
      flags = 4;
      ancilla_aux_timer[k] = 2;
    }
  }
  OamEnt *oam = GetOamCurPtr(), *oam_org = oam;
  int i = ancilla_step[k];
  do {
    if (submodule_index == 0)
      swordbeam_arr[i] = (swordbeam_arr[i] + 4) & 0x3f;
    Point16U pt = Sparkle_PrepOamFromRadial(Ancilla_GetRadialProjection(swordbeam_arr[i], swordbeam_var2));
    Ancilla_SetOam(oam, pt.x, pt.y, kSpinSpark_Char[i], flags | HIBYTE(oam_priority_value), 0);
  } while (oam++, --i >= 0);

  if (submodule_index == 0) {
    if (!sign8(--ancilla_arr3[k]))
      goto endif_2;

    ancilla_arr3[k] = 0;
    ancilla_arr1[k] = (ancilla_arr1[k] + 1) & 3;
    if (ancilla_arr1[k] == 3)
      swordbeam_var1 = (swordbeam_var1 + 9) & 0x3f;
  }

  uint8 t;

  t = ancilla_arr1[k];
  if (t != 3) {
    static const uint8 kSpinSpark_Char2[3] = {0xb7, 0x80, 0x83};
    Point16U pt = Sparkle_PrepOamFromRadial(Ancilla_GetRadialProjection(swordbeam_var1, swordbeam_var2));
    Ancilla_SetOam(oam, pt.x, pt.y, kSpinSpark_Char2[t], 4 | HIBYTE(oam_priority_value), 0);
  }
endif_2:
  if (ancilla_item_to_link[k] == 7)
    bytewise_extended_oam[oam_org - oam_buf + 3] = 1;
}

// Converts a radial projection result (from Ancilla_GetRadialProjection) to a
// screen-space Point16U. Subtracts the current BG2 scroll offsets and adds
// swordbeam_temp_x/y (Link's sword-centre position, set by the transmute
// function) so the spark is anchored to the sword tip regardless of camera.
Point16U Sparkle_PrepOamFromRadial(AncillaRadialProjection p) {  // 88da17
  Point16U pt;
  pt.y = (p.r2 ? -p.r0 : p.r0) + swordbeam_temp_y - 4 - BG2VOFS_copy2;
  pt.x = (p.r6 ? -p.r4 : p.r4) + swordbeam_temp_x - 4 - BG2HOFS_copy2;
  return pt;
}

// Wind-down phase for the spin-attack orbital sparkle. Plays a 3-frame close-in
// animation via SpinSpark_Draw, advancing the frame every 2 ticks (aux_timer).
// At frame 3 the slot is cleared. The radius argument (4) passed to SpinSpark_Draw
// centres the final spiral before it disappears.
void SpinAttackSparkleB_Closer(int k) {  // 88da4c
  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 1;
    if (++ancilla_item_to_link[k] == 3)
      ancilla_type[k] = 0;
  }
  SpinSpark_Draw(k, 4);
}

// Ancilla type 0x30: Cane of Byrna windup sparkle — the charge animation shown
// while the player holds B to activate the Cane. Plays 17 sub-frames (item_to_link
// 0→17), advancing one sub-frame every tick (aux_timer = 1). Each sub-frame maps
// to a 4-entry OAM group in kInitialCaneSpark_Draw_* (row j = (frame-1) & 0xF,
// then rounded to one of 4 sets at j = 0/4/8/12).
//
// Position tracking: Ancilla_SetXY is called each frame using kInitialCaneSpark_X/Y
// indexed by (player_handler_timer * 2 + link_direction_facing * 2). When
// player_handler_timer == 2 ancilla_arr3[k] holds a sub-index that decrements
// to -1 (caps at j = 3) for final positioning.
//
// At sub-frame 17, ByrnaWindupSpark_TransmuteToNormal converts to type 0x31
// (the full orbiting Byrna spark) and the cane becomes active.
void Ancilla30_ByrnaWindupSpark(int k) {  // 88db24
  static const int8 kInitialCaneSpark_X[16] = {3, 1, 0, 0, 13, 16, 12, 12, 24, 7, -4, -10, -8, 9, 22, 26};
  static const int8 kInitialCaneSpark_Y[16] = {5, 0, -3, -6, -8, -3, 12, 28, 5, 0, 8, 16, 5, 0, 8, 16};
  static const int8 kInitialCaneSpark_Draw_X[16] = {-4, 0, 0, 0, -8, 0, -8, 0, -8, 0, -8, 0, -8, 0, -8, 0};
  static const int8 kInitialCaneSpark_Draw_Y[16] = {-4, 0, 0, 0, -8, -8, 0, 0, -8, -8, 0, 0, -8, -8, 0, 0};
  static const uint8 kInitialCaneSpark_Draw_Char[16] = {0x92, 0xff, 0xff, 0xff, 0x8c, 0x8c, 0x8c, 0x8c, 0xd6, 0xd6, 0xd6, 0xd6, 0x93, 0x93, 0x93, 0x93};
  static const uint8 kInitialCaneSpark_Draw_Flags[16] = {0x22, 0xff, 0xff, 0xff, 0x22, 0x62, 0xa2, 0xe2, 0x24, 0x64, 0xa4, 0xe4, 0x22, 0x62, 0xa2, 0xe2};

  if (!submodule_index && sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 1;
    if (++ancilla_item_to_link[k] == 17) {
      ByrnaWindupSpark_TransmuteToNormal(k);
      return;
    }
  }
  if (!ancilla_item_to_link[k])
    return;

  int j = player_handler_timer;
  if (j == 2) {
    uint8 a = ancilla_arr3[k] - 1;
    if (sign8(a))
      a = 0, j = 3;
    ancilla_arr3[k] = a;
  }
  j += link_direction_facing * 2;
  Ancilla_SetXY(k, link_x_coord + kInitialCaneSpark_X[j], link_y_coord + kInitialCaneSpark_Y[j]);
  Point16U pt;
  Ancilla_PrepOamCoord(k, &pt);

  uint8 a = (ancilla_item_to_link[k] - 1) & 0xf;
  j = 0;
  if (a != 0)
    j = 4 * ((a != 15) ? (a & 1) + 1 : 3);

  OamEnt *oam = GetOamCurPtr();
  for (int i = 0; i < 4; i++, j++) {
    if (kInitialCaneSpark_Draw_Char[j] != 255) {
      Ancilla_SetOam(oam, pt.x + kInitialCaneSpark_Draw_X[j], pt.y + kInitialCaneSpark_Draw_Y[j],
                     kInitialCaneSpark_Draw_Char[j],
                     kInitialCaneSpark_Draw_Flags[j] & ~0x30 | HIBYTE(oam_priority_value), 0);
      oam++;
    }
  }
}

// Converts slot k from Ancilla30 (windup) to Ancilla31 (full Byrna orbit,
// type 0x31). Loads swordbeam_arr[0..3] from kCaneSpark_Transmute_Tab indexed
// by link_direction_facing * 2 (initial angular positions of the 4 orbiters).
// Resets all ancilla sub-state (aux_timer = 0x17, arr1 = 2 for blink cadence,
// timer = 21 for magic drain delay) and swordbeam_var2 = 20 (orbit radius).
// Plays the Byrna activation SFX (0x30) then immediately calls Ancilla31_ByrnaSpark.
void ByrnaWindupSpark_TransmuteToNormal(int k) {  // 88dc21
  static const uint8 kCaneSpark_Transmute_Tab[16] = {0x34, 0x33, 0x32, 0x31, 0x16, 0x15, 0x14, 0x13, 0x2a, 0x29, 0x28, 0x27, 0x10, 0xf, 0xe, 0xd};

  ancilla_type[k] = 0x31;
  int j = link_direction_facing << 1;
  swordbeam_arr[0] = kCaneSpark_Transmute_Tab[j + 0];
  swordbeam_arr[1] = kCaneSpark_Transmute_Tab[j + 1];
  swordbeam_arr[2] = kCaneSpark_Transmute_Tab[j + 2];
  swordbeam_arr[3] = kCaneSpark_Transmute_Tab[j + 3];
  ancilla_aux_timer[k] = 0x17;
  ancilla_G[k] = 0;
  ancilla_item_to_link[k] = 0;
  ancilla_arr3[k] = 8;
  ancilla_step[k] = 0;
  ancilla_L[k] = 0;
  ancilla_arr1[k] = 2;
  ancilla_timer[k] = 21;
  swordbeam_var2 = 20;
  Ancilla_Sfx3_Near(0x30);
  Ancilla31_ByrnaSpark(k);
}

// Ancilla type 0x31: Cane of Byrna active spark — the orbiting protective
// ring that surrounds Link while holding the Cane.
//
// Magic drain:
//   Each tick aux_timer[k] counts down; when it reaches 0 (every tick since
//   it reloads to 1) it deducts kCaneSpark_Magic[link_magic_consumption]
//   from link_magic_power every 0x17 ticks (ancilla_G[k] gate). If magic runs
//   out or the player releases B (filtered_joypad_H & 0x40) the slot is cleared
//   (kill_me path). current_item_y must equal 13 (Cane of Byrna item index).
//
// Orbital animation:
//   Uses the same swordbeam_arr / swordbeam_var2 / Sparkle_PrepOamFromRadial
//   mechanism as Ancilla2B but rotates at +3 per tick instead of +4.
//   Four orbiting sparks (kCaneSpark_Char), each drawn at its radial position.
//   ancilla_step[k] determines how many are shown (0=1, 1=2, 2=3, 3=4).
//   The aux_timer blink flag alternates palette (2 vs 4) every 3 ticks.
//
// Collision:
//   After each OAM write the spark's world position is written back via
//   Ancilla_SetXY(k, pt.x + BG2HO, pt.y + BG2VO) and a sprite collision
//   check is run. Hitting a sprite triggers damage from the Byrna spark.
//
// SFX: ancilla_timer[k] counts down from 21; at 0 the Byrna spark SFX
//   (0x30) is played and the timer is reset.
void Ancilla31_ByrnaSpark(int k) {  // 88dc70
  static const uint8 kCaneSpark_Magic[3] = {4, 2, 1};

  uint8 flags = 2;
  if (submodule_index == 0) {
    if (current_item_y != 13) {
kill_me:
      link_disable_sprite_damage = 0;
      ancilla_type[k] = 0;
      link_give_damage = 0;
      return;
    }
    link_disable_sprite_damage = 1;
    if (!--ancilla_aux_timer[k]) {
      ancilla_aux_timer[k] = 1;
      uint8 r0 = kCaneSpark_Magic[link_magic_consumption];
      if (!link_magic_power || (uint8)(r0 = link_magic_power - r0) >= 0x80)
        goto kill_me;

      if (sign8(--ancilla_G[k])) {
        ancilla_G[k] = 0x17;
        link_magic_power = r0;
      }
      if (filtered_joypad_H & 0x40)
        goto kill_me;
    }
    if (ancilla_step[k] != 3) {
      uint8 a = ++ancilla_item_to_link[k];
      ancilla_step[k] = (a >= 4) ? 3 : (a == 2) ? 1 : (a == 3) ? 2 : 0;
    }
    if (sign8(--ancilla_arr1[k])) {
      ancilla_arr1[k] = 2;
      flags = 4;
    }
  }

  int z = (int8)link_z_coord;
  if (z == -1)
    z = 0;
  swordbeam_temp_y = link_y_coord + 12 - z;
  swordbeam_temp_x = link_x_coord + 8;
  if (!ancilla_timer[k]) {
    ancilla_timer[k] = 21;
    Ancilla_Sfx3_Near(0x30);
  }
  OamEnt *oam = GetOamCurPtr();
  int i = ancilla_step[k];
  do {
    static const uint8 kCaneSpark_Char[4] = {0xd7, 0xb7, 0x80, 0x83};
    if (!submodule_index)
      swordbeam_arr[i] = (swordbeam_arr[i] + 3) & 0x3f;
    Point16U pt = Sparkle_PrepOamFromRadial(Ancilla_GetRadialProjection(swordbeam_arr[i], swordbeam_var2));
    Ancilla_SetOam(oam, pt.x, pt.y, kCaneSpark_Char[i], flags | HIBYTE(oam_priority_value), 0);
    Ancilla_SetXY(k, pt.x + BG2HOFS_copy2, pt.y + BG2VOFS_copy2);
    ancilla_dir[k] = 0;
    Ancilla_CheckSpriteCollision(k);
  } while (oam++, --i >= 0);
}

// Sword beam projectile (used by Ancilla types spawned by AddSwordBeam).
// Moves in a straight line using Ancilla_MoveX/Y anchored to swordbeam_temp_x/y.
//
// Every 16 ticks a panning SFX is played (ancilla_G[k] & 0xF == 0, sound 0x01).
// Termination: either a sprite collision (Ancilla_CheckSpriteCollision ≥ 0) or
//   a tile collision (Ancilla_CheckTileCollision). On impact the position is
//   nudged backward by kSwordBeam_Xvel2/Yvel2[direction] and the type is set
//   to 4 (explosion sparkle), with numspr = 0x10 and timer = 7.
//
// Draw:
//   4 orbiting spark points (kSwordBeam_Char) rotate at ancilla_S[k] angular
//   steps per tick (a direction-dependent rate). The trailing sparkle
//   (arr1 cycle → kSwordBeam_Char2) adds a 5th OAM entry every 3 ticks when
//   arr1 is not 3.
void Ancilla_SwordBeam(int k) {  // 88ddc5
  uint8 flags = 2;

  if (!submodule_index) {
    Ancilla_SetXY(k, swordbeam_temp_x, swordbeam_temp_y);
    Ancilla_MoveX(k);
    Ancilla_MoveY(k);
    swordbeam_temp_x = Ancilla_GetX(k);
    swordbeam_temp_y = Ancilla_GetY(k);

    if ((ancilla_G[k]++ & 0xf) == 0) {
      sound_effect_2 = Ancilla_CalculateSfxPan(k) | 1;
    }

    if (Ancilla_CheckSpriteCollision(k) >= 0 || Ancilla_CheckTileCollision(k)) {
      static const int8 kSwordBeam_Yvel2[4] = {0, 0, -6, -6};
      static const int8 kSwordBeam_Xvel2[4] = {-8, -10, 0, 0};
      int j = ancilla_dir[k];
      Ancilla_SetXY(k,
          Ancilla_GetX(k) + kSwordBeam_Xvel2[j],
          Ancilla_GetY(k) + kSwordBeam_Yvel2[j]);
      ancilla_type[k] = 4;
      ancilla_timer[k] = 7;
      ancilla_numspr[k] = 0x10;
      return;
    }
    if (sign8(--ancilla_aux_timer[k])) {
      flags = 4;
      ancilla_aux_timer[k] = 2;
    }
  }

  OamEnt *oam = GetOamCurPtr();
  uint8 s = ancilla_S[k];
  for (int i = 3; i >= 0; i--, oam++) {
    static const uint8 kSwordBeam_Char[4] = {0xd7, 0xb7, 0x80, 0x83};
    if (submodule_index == 0)
      swordbeam_arr[i] = (swordbeam_arr[i] + s) & 0x3f;
    Point16U pt = Sparkle_PrepOamFromRadial(Ancilla_GetRadialProjection(swordbeam_arr[i], swordbeam_var2));
    Ancilla_SetOam(oam, pt.x, pt.y, kSwordBeam_Char[i], flags | HIBYTE(oam_priority_value), 0);
  }

  if (submodule_index == 0) {
    if (!sign8(--ancilla_arr3[k]))
      goto endif_2;

    ancilla_arr3[k] = 0;
    ancilla_arr1[k] = (ancilla_arr1[k] + 1) & 3;
    if (ancilla_arr1[k] == 3)
      swordbeam_var1 = (swordbeam_var1 + s) & 0x3f;
  }

  uint8 t;

  t = ancilla_arr1[k];
  if (t != 3) {
    static const uint8 kSwordBeam_Char2[3] = {0xb7, 0x80, 0x83};
    Point16U pt = Sparkle_PrepOamFromRadial(Ancilla_GetRadialProjection(swordbeam_var1, swordbeam_var2));
    Ancilla_SetOam(oam, pt.x, pt.y, kSwordBeam_Char2[t], 4 | HIBYTE(oam_priority_value), 0);
  }
endif_2:
  oam -= 4;
  for (int i = 0; i < 4; i++) {
    if (oam[i].y != 0xf0)
      return;
  }
  ancilla_type[k] = 0;
}

// Ancilla type 0x0D: Full spin-attack charge sparkle — the single tile
// (kAnc 0xD7) rendered at the sword tip when the spin attack is fully charged.
// Position is anchored to Link's coordinates via kSwordFullChargeSpark_X/Y
// (direction-dependent offsets, indexed by link_direction_facing >> 1).
// Cleared as soon as ancilla_timer[k] reaches zero (timer is decremented
// externally by the normal ancilla tick). OAM priority flags are selected
// from kSwordFullChargeSpark_Flags[ancilla_floor[k]].
void Ancilla0D_SpinAttackFullChargeSpark(int k) {  // 88ddca
  static const int8 kSwordFullChargeSpark_Y[4] = {-8, 27, 12, 12};
  static const int8 kSwordFullChargeSpark_X[4] = {4, 4, -13, 20};
  static const uint8 kSwordFullChargeSpark_Flags[4] = {0x20, 0x10, 0x30, 0x20};

  ancilla_oam_idx[k] = Ancilla_AllocateOamFromRegion_A_or_D_or_F(k, 4);

  if (!ancilla_timer[k]) {
    ancilla_type[k] = 0;
    return;
  }

  int j = link_direction_facing >> 1;

  uint16 x = link_x_coord + kSwordFullChargeSpark_X[j] - BG2HOFS_copy2;
  uint16 y = link_y_coord + kSwordFullChargeSpark_Y[j] - BG2VOFS_copy2;

  oam_priority_value = kSwordFullChargeSpark_Flags[ancilla_floor[k]] << 8;
  OamEnt *oam = GetOamCurPtr();
  Ancilla_SetOam(oam, x, y, 0xd7, HIBYTE(oam_priority_value) | 2, 0);
}

// Ancilla type 0x27: Travel duck (the Flute Boy's duck used for inter-region
// overworld travel). Three phases:
//
//   Inactive (ancilla_timer[k] != 0, initial):
//     The duck circles off-screen left (BG2HOFS − 16, or − 16 − 64 for wide
//     screens) at link_y_coord − 8, waiting for Link to step on it.
//     ancilla_timer is decremented externally.
//
//   Approach / boarding (ancilla_L[k] == 0):
//     Duck flies toward Link. Every 0x28 ticks plays the duck-quack SFX.
//     On Ancilla_CheckLinkCollision(range 1):
//       Clears any conflicting ancillae (hookshot, spin-charge, Byrna).
//       Sets Link invisible (visibility_status = 12), immobilized
//       (flag_is_link_immobilized = 1), disable_sprite_damage = 1.
//       Stores ancilla_step[k] = 2 (carrying Link).
//     Indoor boarding also freezes the screen via byte_7E03FD.
//
//   Carrying (ancilla_L[k] == 1 or ancilla_step[k] != 0):
//     Z and X position advances (Ancilla_MoveZ/X). flag_unk1 is set each
//     frame while carrying. When duck X passes link_x_coord:
//       If step != 0: Link is released (visibility_status = 0, immobilized = 0,
//         follower initialised, countdown_for_blink = 144).
//     Close approach (< 48 px): j = 3 skips to the 4-sprite draw mode.
//
// Draw: kTravelBird_Draw_X/Y/Char/Flags determine the per-frame OAM layout.
//   ancilla_step + 1 OAM entries are drawn (1 when approaching, more when
//   carrying Link). ancilla_K[k] cycles 0→1→2 every 3 ticks to select the
//   wing-flap VRAM DMA via kTravelBird_DmaStuffs. A drop-shadow is appended.
// Termination: when screen X ≥ 0x130 the slot clears and if Link was being
//   carried submodule_index → 10 to start the region-exit transition.
void Ancilla27_Duck(int k) {  // 88dde8
  CheckPlayerCollOut coll;
  int j;

  if (submodule_index)
    goto endif_1;

  if (ancilla_timer[k]) {
    int xt = (enhanced_features0 & kFeatures0_ExtendScreen64) ? 0x40 : 0;
    Ancilla_SetXY(k, BG2HOFS_copy2 - 16 - xt, link_y_coord - 8);
    return;
  }

  if (sign8(--ancilla_G[k])) {
    ancilla_G[k] = 0x28;
    Ancilla_Sfx3_Pan(k, 0x1e);
  }

  if (ancilla_L[k] || ancilla_step[k] && (flag_unk1++, true)) {
    ancilla_z_vel[k]--;
    Ancilla_MoveZ(k);
  }
  Ancilla_MoveX(k);


  if (ancilla_L[k]) {
    uint16 x = Ancilla_GetX(k);
    if (ancilla_step[k])
      flag_unk1++;
    if (!sign16(x) && x >= link_x_coord) {
      if (ancilla_step[k]) {
        ancilla_step[k] = 0;
        link_visibility_status = 0;
        tagalong_var5 = 0;
        link_pose_for_item = 0;
        ancilla_y_vel[k] = 0;
        flag_is_link_immobilized = 0;
        link_disable_sprite_damage = 0;
        byte_7E03FD = 0;
        countdown_for_blink = 144;
        if (!((follower_indicator == 12 || follower_indicator == 13) && follower_dropped)) {
          Follower_Initialize();
        }
      }
    } else if ((uint16)(link_x_coord - x) < 48) {
      j = 3;
      goto endif_5;
    }
    goto endif_1;
  }

  if (!Ancilla_CheckLinkCollision(k, 1, &coll) || main_module_index == 15)
    goto endif_1;

  if (!player_is_indoors) {
    if (link_player_handler_state == 8 || link_player_handler_state == 9 || link_player_handler_state == 10 ||
        player_near_pit_state == 2 ||
        (link_pose_for_item | related_to_hookshot | link_force_hold_sword_up | link_disable_sprite_damage) ||
        (link_state_bits & 0x80))
      goto endif_1;
    for (int i = 4; i >= 0; i--) {
      uint8 a = ancilla_type[i];
      if (a == 0x2a || a == 0x1f || a == 0x30 || a == 0x31 || a == 0x41)
        ancilla_type[i] = 0;
    }
    if (follower_indicator == 9) {
      follower_indicator = 0;
      tagalong_var5 = 0;
    }
  }
  link_state_bits = 0;
  link_picking_throw_state = 0;

  bg1_x_offset = 0;
  bg1_y_offset = 0;
  Link_ResetProperties_A();
  link_is_in_deep_water = 0;
  link_need_for_pullforrupees_sprite = 0;
  link_visibility_status = 12;
  link_player_handler_state = 0;
  link_pose_for_item = 1;
  flag_is_link_immobilized = 1;
  link_disable_sprite_damage = 1;
  tagalong_var5 = 1;
  ancilla_step[k] = 2;
  flag_unk1++;
  link_give_damage = 0;
  if (player_is_indoors)
    byte_7E03FD = player_is_indoors;
endif_1:
  if (sign8(--ancilla_arr3[k])) {
    ancilla_arr3[k] = 3;
    if (++ancilla_K[k] == 3)
      ancilla_K[k] = 0;
  }
  j = ancilla_K[k];
endif_5:
  BYTE(flag_travel_bird) = kTravelBird_DmaStuffs[j];

  Point16U info;
  Ancilla_PrepOamCoord(k, &info);

  OamEnt *oam = GetOamCurPtr();
  int z = ancilla_z[k] ? ancilla_z[k] | ~0xff : 0;
  int i = 0, n = ancilla_step[k] + 1;
  do {
    Ancilla_SetOam(oam, info.x + (int8)kTravelBird_Draw_X[i], info.y + z + (int8)kTravelBird_Draw_Y[i],
                   kTravelBird_Draw_Char[i], kTravelBird_Draw_Flags[i] | 0x30, 2);
    oam++;
  } while (++i != n);

  AncillaDraw_Shadow(oam, 1, info.x, info.y + 28, 0x30);
  oam += 2;
  if (ancilla_step[k])
    AncillaDraw_Shadow(oam, 1, info.x - 7, info.y + 28, 0x30);

  if (!sign16(info.x) && info.x >= 0x130) {
    ancilla_type[k] = 0;
    if (!ancilla_L[k] && ancilla_step[k]) {
      submodule_index = 10;
      saved_module_for_menu = main_module_index;
      main_module_index = 14;
    }
  }
}

// Allocates a new ancilla slot for a Somaria block of the given type and y-tile
// coordinate. If a Somaria block already exists in any slot (type 0x2C) and
// it is not the current slot, the existing block is destroyed via
// AncillaAdd_ExplodingSomariaBlock and the new slot is freed — only one Somaria
// block can exist at a time. If Link was carrying the old block
// (flag_is_ancilla_to_pick_up), the pickup flag is cleared and any drag state
// is reset (link_speed_setting == 0x12 → 0). Otherwise the new block is fully
// initialised: position velocity, timers, and secondary fields are zeroed;
// ancilla_G[k] = 12 (spawn-in countdown), ancilla_timer[k] = 18, ancilla_S[k]
// = 9 (some draw variant), and the platform-drop SFX (0x2A) is played.
// Returns the ancilla slot index, or negative if no free slot was found.
int AncillaAdd_SomariaBlock(uint8 type, uint8 y) {  // 88e078
  int k = AncillaAdd_AddAncilla_Bank08(type, y);
  if (k < 0)
    return k;
  for (int j = 4; j >= 0; j--) {
    if (j == k || ancilla_type[j] != 0x2c)
      continue;
    if (j == flag_is_ancilla_to_pick_up - 1)
      flag_is_ancilla_to_pick_up = 0;
    AncillaAdd_ExplodingSomariaBlock(j);
    ancilla_type[k] = 0;
    dung_flag_somaria_block_switch = 0;
    if (link_speed_setting == 0x12) {
      bitmask_of_dragstate = 0;
      link_speed_setting = 0;
    }
    return k;
  }

  Ancilla_Sfx3_Near(0x2a);
  ancilla_step[k] = 0;
  ancilla_y_vel[k] = 0;
  ancilla_x_vel[k] = 0;
  ancilla_item_to_link[k] = 0;
  ancilla_aux_timer[k] = 0;
  ancilla_arr3[k] = 0;
  ancilla_arr1[k] = 0;
  ancilla_H[k] = 0;
  ancilla_G[k] = 12;
  ancilla_timer[k] = 18;
  ancilla_L[k] = 0;
  ancilla_z[k] = 0;
  ancilla_K[k] = 0;
  ancilla_R[k] = 0;
  ancilla_arr4[k] = 0;
  ancilla_S[k] = 9;
  ancilla_T[k] = 0;
  ancilla_dir[k] = link_direction_facing >> 1;
  if (Ancilla_CheckInitialTileCollision_Class2(k)) {
    Ancilla_SetX(k, link_x_coord + 8);
    Ancilla_SetY(k, link_y_coord + 16);
  } else {
    static const int8 kCaneOfSomaria_Y[4] = { -8, 31, 17, 17 };
    static const int8 kCaneOfSomaria_X[4] = { 8, 8, -8, 23 };
    int j = link_direction_facing >> 1;
    Ancilla_SetX(k, link_x_coord + kCaneOfSomaria_X[j]);
    Ancilla_SetY(k, link_y_coord + kCaneOfSomaria_Y[j]);
    SomariaBlock_CheckForTransitTile(k);
  }
  return k;
}

// After a Somaria block is placed, checks whether its position overlaps any
// of 12 probe points around the block (kSomariaTransitLine_X/Y: a 3×2 grid
// above, below, left, and right) for transit-line tile attributes 0xB6 or 0xBC
// (rail tiles). If a match is found the block is snapped to that tile's world
// position and AncillaAdd_SomariaPlatformPoof is called to start the platform
// animation. Only runs when dung_unk6 is non-zero (transit tiles are present).
// ancilla_objprio is preserved across the probe call so collision-class metadata
// is not mutated during the tile lookup.
void SomariaBlock_CheckForTransitTile(int k) {  // 88e191
  static const int8 kSomariaTransitLine_X[12] = { -8, 0, 8, -8, 0, 8, -16, -16, -16, 16, 16, 16 };
  static const int8 kSomariaTransitLine_Y[12] = { -16, -16, -16, 16, 16, 16, -8, 0, 8, -8, 0, 8 };
  if (!dung_unk6)
    return;
  for (int j = 11; j >= 0; j--) {
    uint16 x = Ancilla_GetX(k) + kSomariaTransitLine_X[j];
    uint16 y = Ancilla_GetY(k) + kSomariaTransitLine_Y[j];
    uint8 bak = ancilla_objprio[k];
    Ancilla_CheckTileCollision_targeted(k, x, y);
    ancilla_objprio[k] = bak;
    if (ancilla_tile_attr[k] == 0xb6 || ancilla_tile_attr[k] == 0xbc) {
      Ancilla_SetX(k, x);
      Ancilla_SetY(k, y);
      AncillaAdd_SomariaPlatformPoof(k);
      return;
    }
  }
}

// Checks ancilla k against all 16 sprite slots for a basic (non-complex) hit.
// Returns the sprite index j on first collision, or -1 if none.
//
// Filtering (any of the following skips sprite j):
//   - Staggered: only checks sprites where (j ^ frame_counter) & 3 == 0.
//   - sprite_pause[j] or sprite_hit_timer[j] non-zero (already processing a hit).
//   - sprite_state[j] < 9 (not in active combat state).
//   - ancilla_objprio[k] != 0 and sprite_defl_bits[j] bit 1 unset
//     (object-priority ancillae cannot hit certain deflection-immune sprites).
//   - Floor mismatch (ancilla_floor[k] != sprite_floor[j]).
//   - Somaria block (type 0x2C) vs. sprites 0x1E or 0x90 (statues that
//     blocks cannot destroy).
int Ancilla_CheckBasicSpriteCollision(int k) {  // 88e1f9
  for (int j = 15; j >= 0; j--) {
    if (((j ^ frame_counter) & 3 | sprite_pause[j] | sprite_hit_timer[j]) != 0)
      continue;
    if (sprite_state[j] < 9 || !(sprite_defl_bits[j] & 2) && ancilla_objprio[k])
      continue;
    if (ancilla_floor[k] != sprite_floor[j])
      continue;
    if (ancilla_type[k] == 0x2c && (sprite_type[j] == 0x1e || sprite_type[j] == 0x90))
      continue;
    if (Ancilla_CheckBasicSpriteCollision_Single(k, j))
      return j;
  }
  return -1;
}

// Hit-box overlap test between ancilla k and sprite j. Returns true if a hit
// is registered. The basic hit box for the ancilla is a 15×15 px square
// centred at (GetX(k) − 8, GetY(k) − 8 − Z) — Z-height is subtracted so
// elevated projectiles (e.g. thrown bombs) only hit sprites at matching height.
//
// Special sprite handling:
//   Sprite 0x92 (Agahnim2 energy ball): always returns true if sprite_C[j] < 3.
//   Sprite 0x80 (door trigger?): flips sprite_D[j] and sets a 24-tick delay.
// sprite_ignore_projectile[j] non-zero → returns false (sprite is immune).
//
// On confirmed hit: calls Sprite_ProjectSpeedTowardsLocation to compute the
//   recoil vector from the ancilla's position and stores the inverted components
//   into sprite_y/x_recoil[j]. Then calls Ancilla_CheckDamageToSprite.
bool Ancilla_CheckBasicSpriteCollision_Single(int k, int j) {  // 88e23d
  SpriteHitBox hb;
  Ancilla_SetupBasicHitBox(k, &hb);
  Sprite_SetupHitBox(j, &hb);
  if (!CheckIfHitBoxesOverlap(&hb))
    return false;
  if (sprite_type[j] == 0x92 && sprite_C[j] < 3)
    return true;
  if (sprite_type[j] == 0x80 && sprite_delay_aux4[j] == 0) {
    sprite_delay_aux4[j] = 24;
    sprite_D[j] ^= 1;
  }
  if (sprite_ignore_projectile[j])
    return false;

  int x = Ancilla_GetX(k) - 8, y = Ancilla_GetY(k) - 8 - ancilla_z[k];
  ProjectSpeedRet pt = Sprite_ProjectSpeedTowardsLocation(j, x, y, 80);
  sprite_y_recoil[j] = ~pt.y;
  sprite_x_recoil[j] = ~pt.x;
  Ancilla_CheckDamageToSprite(j, ancilla_type[k]);
  return true;
}

// Fills in the ancilla half of a SpriteHitBox for the basic 16×16 collision
// rectangle. The ancilla's world position is offset by −8 in both X and Y so
// the centre of the tile is the collision origin. Z-height (ancilla_z[k]) is
// subtracted from Y to account for the ancilla's altitude above the floor.
// hb->r2 and hb->r3 (width and height fields) are both set to 15 (0..15
// inclusive = 16 px).
void Ancilla_SetupBasicHitBox(int k, SpriteHitBox *hb) {  // 88e2ca
  int x = Ancilla_GetX(k) - 8;
  hb->r0_xlo = x;
  hb->r8_xhi = x >> 8;
  int y = Ancilla_GetY(k) - 8 - ancilla_z[k];
  hb->r1_ylo = y;
  hb->r9_yhi = y >> 8;
  hb->r2 = 15;
  hb->r3 = 15;
}

// Ancilla type 0x2C: Cane of Somaria block handler. Manages the full block
// lifecycle: spawn-in countdown, lift/carry/throw via Ancilla_HandleLiftLogic,
// floor/platform riding, sprite collision (pushes sprites away), tile collision
// stopping, rail-tile riding (transit platform), and detonation.
void Ancilla2C_SomariaBlock(int k) {  // 88e365
  if (!sign8(--ancilla_G[k]))
    return;
  ancilla_G[k] = 0;

  if (ancilla_H[k])
    goto label_1;
  if (submodule_index == 0 || submodule_index == 8 || submodule_index == 16) {
    Ancilla_HandleLiftLogic(k);
  } else if (k + 1 == flag_is_ancilla_to_pick_up && ancilla_K[k] != 0) {
    if (ancilla_K[k] != 3) {
      Ancilla_LatchLinkCoordinates(k, 3);
      Ancilla_LatchAltitudeAboveLink(k);
      ancilla_K[k] = 3;
    }
    Ancilla_LatchCarriedPosition(k);
  }
  if (player_is_indoors) {
    if (!ancilla_K[k] && !(link_state_bits & 0x80) && (ancilla_z[k] == 0 || ancilla_z[k] == 0xff)) {
      if (dung_unk6) {
        int j = frame_counter & 3;
        do {
          uint8 bak = ancilla_objprio[k];
          uint16 x = Ancilla_GetX(k) + kSomarianBlock_Coll_X[j];
          uint16 y = Ancilla_GetY(k) + kSomarianBlock_Coll_Y[j];
          Ancilla_CheckTileCollision_targeted(k, x, y);
          ancilla_objprio[k] = bak;
          if (ancilla_tile_attr[k] == 0xb6 || ancilla_tile_attr[k] == 0xbc) {
            Ancilla_SetXY(k, x, y);
            AncillaAdd_SomariaPlatformPoof(k);
            if (k + 1 == flag_is_ancilla_to_pick_up)
              flag_is_ancilla_to_pick_up = 0;
            return;
          }
        } while ((j += 4) < 12);
      } else {
        if (!SomariaBlock_CheckForSwitch(k) && (ancilla_z[k] == 0 || ancilla_z[k] == 0xff))
          dung_flag_somaria_block_switch++;
      }
    } else {
label_1:
      if (flag_is_ancilla_to_pick_up == k + 1)
        dung_flag_somaria_block_switch = 0;
    }
  }

  uint16 old_y = Ancilla_LatchYCoordToZ(k);
  uint8 s1a = ancilla_dir[k];
  uint8 s1b = ancilla_objprio[k];
  ancilla_objprio[k] = 0;
  bool flag = Ancilla_CheckTileCollision_Class2(k);

  if (player_is_indoors && ancilla_L[k] && ancilla_tile_attr[k] == 0x1c)
    ancilla_T[k] = 1;

label1:
  if (flag && (!(link_state_bits & 0x80) || link_picking_throw_state)) {
    if (!s1b && !ancilla_arr4[k] && ancilla_z[k]) {
      ancilla_arr4[k] = 1;
      int qq = (ancilla_dir[k] == 1) ? 16 : 4;
      if (ancilla_y_vel[k])
        ancilla_y_vel[k] = sign8(ancilla_y_vel[k]) ? qq : -qq;
      if (ancilla_x_vel[k])
        ancilla_x_vel[k] = sign8(ancilla_x_vel[k]) ? 4 : -4;
      if (ancilla_dir[k] == 1 && ancilla_z[k]) {
        ancilla_y_vel[k] = -4;
        ancilla_L[k] = 2;
      }
    }
  } else if (!(link_state_bits & 0x80) && (ancilla_z[k] == 0 || ancilla_z[k] == 0xff)) {
    ancilla_dir[k] = 16;
    uint8 bak0 = ancilla_objprio[k];
    Ancilla_CheckTileCollision(k);
    ancilla_objprio[k] = bak0;
    uint8 a = ancilla_tile_attr[k];
    if (a == 0x26) {
      flag = true;
      goto label1;
    } else if (a == 0xc || a == 0x1c) {
      if (dung_hdr_collision != 3) {
        if (ancilla_floor[k] == 0 && ancilla_z[k] != 0 && ancilla_z[k] != 0xff)
          ancilla_floor[k] = 1;
      } else {
        old_y = Ancilla_GetY(k) + dung_floor_y_vel;
        Ancilla_SetX(k, Ancilla_GetX(k) + dung_floor_x_vel);
      }
    } else if (a == 0x20 || (a & 0xf0) == 0xb0 && a != 0xb6 && a != 0xbc) {
      if (!(link_state_bits & 0x80)) {
        if (k + 1 == flag_is_ancilla_to_pick_up)
          flag_is_ancilla_to_pick_up = 0;
        if (!ancilla_timer[k]) {
          if (link_speed_setting == 18) {
            link_speed_setting = 0;
            bitmask_of_dragstate = 0;
          }
          ancilla_type[k] = 0;
          return;
        }
      }
    } else if (a == 8) {
      if (k + 1 == flag_is_ancilla_to_pick_up)
        flag_is_ancilla_to_pick_up = 0;
      if (ancilla_timer[k] == 0) {
        Ancilla_SetY(k, Ancilla_GetY(k) - 24);
        Ancilla_TransmuteToSplash(k);
        return;
      }
    } else if (a == 0x68 || a == 0x69 || a == 0x6a || a == 0x6b) {
      Ancilla_ApplyConveyor(k);
      old_y = Ancilla_GetY(k);
    } else {
      ancilla_timer[k] = (ancilla_L[k] | ancilla_H[k]) ? 0 : 2;
    }
  }
  // endif_3
  s1b |= ancilla_objprio[k];

  if (!(link_state_bits & 0x80) && !--ancilla_S[k]) {
    ancilla_S[k] = 1;
    ancilla_objprio[k] = 0;
    if (Ancilla_CheckBasicSpriteCollision(k) >= 0) {
      ancilla_S[k] = 7;
      if (++ancilla_step[k] == 5) {
        SomariaBlock_FizzleAway(k);
        return;
      }
    }
  }
  Ancilla_SetY(k, old_y);
  ancilla_dir[k] = s1a;
  ancilla_objprio[k] = s1b;

  AncillaDraw_SomariaBlock(k);
}

// Draws the Somaria block OAM. The block is a 16×16 tile (0xE9) rendered as
// four 8×8 quadrants via kSomarianBlock_Draw_X/Y and _Flags.
// ancilla_arr1[k] selects which 4-entry row of the tables to use (blocks on
// transit rails cycle their tile row). Z-height is subtracted from Y.
//
// OAM region selection:
//   - If Link is carrying the block while facing up (direction 0) it uses
//     region B/E (behind Link) so the block appears under him.
//   - If sort_sprites_setting and ancilla_floor == 1 (upper floor) and the
//     block is floating or being carried, the OAM pointer is set to the upper
//     floor region (0x8D0 / 0xA20+0x34).
//   - When in mid-air (z != 0 and not latched) oam_priority_value is set to
//     0x3000 (priority 3) so the block renders above all ground tiles.
//
// After drawing, SomarianBlock_CheckEmpty tests whether all 4 OAM entries
// landed off-screen (y == 0xF0); if so the block is silently freed.
void AncillaDraw_SomariaBlock(int k) {  // 88e61b
  static const int8 kSomarianBlock_Draw_X[12] = {-8, 0, -8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  static const int8 kSomarianBlock_Draw_Y[12] = {-8, -8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  static const uint8 kSomarianBlock_Draw_Flags[12] = {0, 0x40, 0x80, 0xc0, 0, 0x40, 0x80, 0xc0, 0, 0x40, 0x80, 0xc0};

  if (k + 1 == flag_is_ancilla_to_pick_up && link_state_bits & 0x80 && ancilla_K[k] != 3 && link_direction_facing == 0) {
    Ancilla_AllocateOamFromRegion_B_or_E(ancilla_numspr[k]);
  } else if (sort_sprites_setting && ancilla_floor[k] && (ancilla_L[k] || k + 1 == flag_is_ancilla_to_pick_up && (link_state_bits & 0x80))) {
    oam_cur_ptr = 0x8d0;
    oam_ext_cur_ptr = 0xa20 + 0x34;
  }

  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr(), *oam_org = oam;
  int z = (int8)ancilla_z[k];
  if (z != 0 && z != -1 && ancilla_K[k] != 3 && ancilla_objprio[k])
    oam_priority_value = 0x3000;
  pt.y -= z;
  int j = ancilla_arr1[k] * 4;
  int r8 = 0;
  do {
    Ancilla_SetOam_Safe(oam, pt.x + kSomarianBlock_Draw_X[j], pt.y + kSomarianBlock_Draw_Y[j], 0xe9,
                        kSomarianBlock_Draw_Flags[j] & ~0x30 | 2 | HIBYTE(oam_priority_value), 0);
    oam++;
  } while (j++, ++r8 & 3);

  if (SomarianBlock_CheckEmpty(oam_org)) {
    dung_flag_somaria_block_switch = 0;
    ancilla_type[k] = 0;
    if (k + 1 == flag_is_ancilla_to_pick_up) {
      flag_is_ancilla_to_pick_up = 0;
      if (link_state_bits & 128)
        link_state_bits = 0;
    }
  }
}

// Checks whether the Somaria block covers a floor switch or pressure plate.
// Probes the four cardinal neighbours (±4 px offset) via
// Ancilla_CheckTileCollision_targeted. Tile attrs 0x23–0x25 and 0x3B are switch
// tile types. ancilla_arr24[k] counts how many of the 4 probes hit a switch.
// Returns false (switch covered) if all 4 probes hit a switch tile, true
// (not fully covered) otherwise. Also resets dung_flag_somaria_block_switch
// and arr24 before scanning.
bool SomariaBlock_CheckForSwitch(int k) {  // 88e75c
  static const int8 kSomarianBlock_CheckCover_X[4] = {0, 0, -4, 4};
  static const int8 kSomarianBlock_CheckCover_Y[4] = {-4, 4, 0, 0};
  dung_flag_somaria_block_switch = 0;
  ancilla_arr24[k] = 0;
  for (int j = 3; j >= 0; j--) {
    uint16 y = Ancilla_GetY(k) + kSomarianBlock_CheckCover_Y[j];
    uint16 x = Ancilla_GetX(k) + kSomarianBlock_CheckCover_X[j];
    uint8 bak = ancilla_objprio[k];
    Ancilla_CheckTileCollision_targeted(k, x, y);
    ancilla_objprio[k] = bak;
    uint8 a = ancilla_tile_attr[k];
    if (a == 0x23 || a == 0x24 || a == 0x25 || a == 0x3b)
      ancilla_arr24[k]++;
  }
  return ancilla_arr24[k] != 4;
}

// Destroys the Somaria block after it has been hit 5 times (ancilla_step == 5)
// or fell into an invalid tile. Converts the slot to type 0x2D (block fizzle
// animation). Clears the drag state if Link was being dragged by the block
// (link_speed_setting == 18). If Link was carrying the block at the time
// (flag_is_ancilla_to_pick_up), clears the pickup flag and masks link_state_bits
// to bit 7 only (preserving the in-water flag). Calls Ancilla2D_SomariaBlockFizz
// immediately to begin the first fizzle frame.
void SomariaBlock_FizzleAway(int k) {  // 88e9b2
  if (link_speed_setting == 18) {
    bitmask_of_dragstate = 0;
    link_speed_setting = 0;
  }
  dung_flag_somaria_block_switch = 0;
  ancilla_type[k] = 0x2d;
  ancilla_aux_timer[k] = 0;
  ancilla_step[k] = 0;
  ancilla_item_to_link[k] = 0;
  ancilla_arr3[k] = 0;
  ancilla_arr1[k] = 0;
  ancilla_R[k] = 0;
  if (k + 1 == flag_is_ancilla_to_pick_up) {
    flag_is_ancilla_to_pick_up = 0;
    link_state_bits &= 0x80;
  }
  Ancilla2D_SomariaBlockFizz(k);
}

// Ancilla type 0x2D: Somaria block fizzle animation — 3-frame vanish effect
// played when the block is destroyed. Advances one frame every 3 ticks
// (ancilla_aux_timer). At frame 3 the slot is cleared. Two OAM entries per
// frame (kSomariaBlockFizzle_Char/Flags, 6 total entries), with 0xFF entries
// skipped. Z-height is subtracted from Y (clamped: 0xFF is treated as 0 to
// avoid wrapping the screen Y).
void Ancilla2D_SomariaBlockFizz(int k) {  // 88e9e8
  static const int8 kSomariaBlockFizzle_X[6] = {-4, -1, -8, 0, -6, -2};
  static const int8 kSomariaBlockFizzle_Y[6] = {-4, -1, -4, -4, -4, -4};
  static const uint8 kSomariaBlockFizzle_Char[6] = {0x92, 0xff, 0xf9, 0xf9, 0xf9, 0xf9};
  static const uint8 kSomariaBlockFizzle_Flags[6] = {6, 0xff, 0x86, 0xc6, 0x86, 0xc6};
  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 3;
    if (++ancilla_item_to_link[k] == 3) {
      ancilla_type[k] = 0;
      return;
    }
  }
  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  uint8 z = ancilla_z[k];
  if (z == 0xff)
    z = 0;
  int x = pt.x, y = pt.y - (int8)z;
  int j = ancilla_item_to_link[k] * 2;
  for (int i = 0; i < 2; i++, j++, oam++) {
    if (kSomariaBlockFizzle_Char[j] != 0xff) {
      Ancilla_SetOam(oam, x + kSomariaBlockFizzle_X[j], y + kSomariaBlockFizzle_Y[j],
                     kSomariaBlockFizzle_Char[j],
                     kSomariaBlockFizzle_Flags[j] & ~0x30 | HIBYTE(oam_priority_value), 0);
    }
  }
}

// Ancilla type 0x39: Somaria platform poof — transitional effect when a
// Somaria block lands on a rail tile and becomes a moving platform. Counts
// down ancilla_aux_timer; while positive just returns. On expiry (aux_timer
// goes negative) the block is freed and Sprite_SpawnDynamically spawns sprite
// type 0xED (the Somaria platform sprite) at the block's snapped position.
//
// Platform direction:
//   Probes the three adjacent rail-tile neighbours (up → right → left via
//   kSomarianPlatformPoof_Tab0) using dung_bg2_attr_table. The first direction
//   whose neighbour lacks a rail-tile attr (0xBx) determines sprite_D[j],
//   which the platform sprite uses to choose its initial movement direction.
//   player_on_somaria_platform is cleared so the platform starts fresh.
//
// Fallback: if Sprite_SpawnDynamically fails (no free sprite slot), the block
//   continues drawing via AncillaDraw_SomariaBlock until a slot opens.
void Ancilla39_SomariaPlatformPoof(int k) {  // 88ea83
  static const uint8 kSomarianPlatformPoof_Tab0[4] = {1, 0, 3, 2};
  if (!sign8(--ancilla_aux_timer[k]))
    return;
  ancilla_type[k] = 0;
  SpriteSpawnInfo info;
  int x = Ancilla_GetX(k) & ~7 | 4, y = Ancilla_GetY(k) & ~7 | 4;
  uint8 floor = ancilla_floor[k];
  int j = Sprite_SpawnDynamically(k, 0xed, &info);  // wtf
  if (j >= 0) {
    player_on_somaria_platform = 0;
    Sprite_SetX(j, x);
    Sprite_SetY(j, y);

    int pos = ((x & 0x1f8) >> 3) + ((y & 0x1f8) << 3) + (floor >= 1 ? 0x1000 : 0);

    int t = 0;
    if ((dung_bg2_attr_table[pos + XY(0, -1)] & 0xf0) != 0xb0) {
      t += 1;
      if ((dung_bg2_attr_table[pos + XY(0, 1)] & 0xf0) != 0xb0) {
        t += 1;
        if ((dung_bg2_attr_table[pos + XY(-1, 0)] & 0xf0) != 0xb0) {
          t += 1;
        }
      }
    }
    sprite_D[j] = kSomarianPlatformPoof_Tab0[t];
    sprite_floor[j] = 0;
  } else {
    AncillaDraw_SomariaBlock(k);
  }
}

// Ancilla type 0x2E: Somaria block fission — the split animation when the
// block detonates and fires its 4 projectile beams. Plays a 2-frame
// explosion animation (8 OAM entries per frame from kSomarianBlockDivide_*,
// each frame advances every 3 ticks). At frame 2 the slot is freed and
// SomariaBlock_SpawnBullets is called to create the 4 directional beams.
// Z-height is adjusted when Link is carrying the block (K[k] == 3 and link
// has non-zero Z) so the explosion floats at the correct altitude.
void Ancilla2E_SomariaBlockFission(int k) {  // 88eb3e
  static const int8 kSomarianBlockDivide_X[16] = {-8, 0, -8, 0, -10, -10, 2, 2, -8, 0, -8, 0, -12, -12, 4, 4};
  static const int8 kSomarianBlockDivide_Y[16] = {-10, -10, 2, 2, -8, 0, -8, 0, -12, -12, 4, 4, -8, 0, -8, 0};
  static const uint8 kSomarianBlockDivide_Char[16] = {0xc6, 0xc6, 0xc6, 0xc6, 0xc4, 0xc4, 0xc4, 0xc4, 0xd2, 0xd2, 0xd2, 0xd2, 0xc5, 0xc5, 0xc5, 0xc5};
  static const uint8 kSomarianBlockDivide_Flags[16] = {0xc6, 0x86, 0x46, 6, 0x46, 0xc6, 6, 0x86, 0xc6, 0x86, 0x46, 6, 0x46, 0xc6, 6, 0x86};

  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 3;
    if (++ancilla_item_to_link[k] == 2) {
      ancilla_type[k] = 0;
      SomariaBlock_SpawnBullets(k);
      return;
    }
  }
  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();

  int8 z = ancilla_z[k] + (ancilla_K[k] == 3 && BYTE(link_z_coord) != 0xff ? BYTE(link_z_coord) : 0);
  int j = ancilla_item_to_link[k] * 8;
  for (int i = 0; i != 8; i++, j++, oam++) {
    Ancilla_SetOam(oam, pt.x + kSomarianBlockDivide_X[j], pt.y + kSomarianBlockDivide_Y[j] - z,
                   kSomarianBlockDivide_Char[j], kSomarianBlockDivide_Flags[j] & ~0x30 | HIBYTE(oam_priority_value), 0);
  }
}

// Ancilla type 0x2F: Lamp flame effect — the burning flame sprite when Link
// ignites something with the Lamp. ancilla_timer[k] is the remaining lifetime
// (decremented externally by the normal ancilla tick); when it hits 0 the
// slot is freed. The frame is derived from the upper 5 bits of the timer
// ((timer & 0xF8) >> 1 = 4 entries per row). Two OAM sprites per frame
// (skipping 0xFF entries). Palette 2 is used throughout.
void Ancilla2F_LampFlame(int k) {  // 88ec13
  static const uint8 kLampFlame_Draw_Char[12] = {0x9c, 0x9c, 0xff, 0xff, 0xa4, 0xa5, 0xb2, 0xb3, 0xe3, 0xf3, 0xff, 0xff};
  static const int8 kLampFlame_Draw_Y[12] = {-3, 0, 0, 0, 0, 0, 8, 8, 0, 8, 0, 0};
  static const int8 kLampFlame_Draw_X[12] = {4, 10, 0, 0, 1, 9, 2, 7, 4, 4, 0, 0};

  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();
  if (!ancilla_timer[k]) {
    ancilla_type[k] = 0;
    return;
  }
  int j = (ancilla_timer[k] & 0xf8) >> 1;
  do {
    if (kLampFlame_Draw_Char[j] != 0xff) {
      Ancilla_SetOam(oam, pt.x + kLampFlame_Draw_X[j], pt.y + kLampFlame_Draw_Y[j], kLampFlame_Draw_Char[j], HIBYTE(oam_priority_value) | 2, 0);
      oam++;
    }
  } while (++j & 3);
}

// Ancilla type 0x41: Waterfall splash — the animated water spray shown around
// Link when he swims through a waterfall entrance. Persists as long as
// Ancilla_CheckForEntranceTrigger returns true for the current indoor/outdoor
// context; otherwise the slot is freed.
//
// Behaviour per tick:
//   - Plays waterfall SFX (0x1C) every 8 frames while not in a submodule.
//   - Sets draw_water_ripples_or_grass = 1 and suppresses Link's walking
//     animation (link_animation_steps − 6) so he appears to swim in place.
//   - Cycles a 4-frame splash animation every 2 ticks (ancilla_item_to_link
//     cycles 0→1→2→3).
//   - Position tracks Link's world coordinates. In dungeons (player_is_indoors)
//     and when Link's Y is < 0x38 (in the waterfall channel) the Y is
//     overridden to 0xD38 (the door threshold).
//   - Z-height is subtracted from screen Y (clamped: negative Z is treated as 0).
//   - Two OAM sprites per frame from kWaterfallSplash_* (0xFF = skip).
void Ancilla41_WaterfallSplash(int k) {  // 88ecaf
  if (!Ancilla_CheckForEntranceTrigger(player_is_indoors ? 0 : 1)) {
    ancilla_type[k] = 0;
    return;
  }

  if (!submodule_index && !(frame_counter & 7))
    Ancilla_Sfx2_Near(0x1c);

  draw_water_ripples_or_grass = 1;
  if (!sign8(link_animation_steps - 6))
    link_animation_steps -= 6;

  if (!ancilla_timer[k]) {
    ancilla_timer[k] = 2;
    ancilla_item_to_link[k] = (ancilla_item_to_link[k] + 1) & 3;
  }

  if (player_is_indoors && BYTE(link_y_coord) < 0x38) {
    Ancilla_SetY(k, 0xd38);
  } else {
    Ancilla_SetY(k, link_y_coord);
  }
  Ancilla_SetX(k, link_x_coord);

  static const int8 kWaterfallSplash_X[8] = {0, 0, -4, 4, -7, 7, -9, 17};
  static const int8 kWaterfallSplash_Y[8] = {-4, 0, -5, -5, -3, -3, 12, 12};
  static const uint8 kWaterfallSplash_Char[8] = {0xc0, 0xff, 0xac, 0xac, 0xae, 0xae, 0xbf, 0xbf};
  static const uint8 kWaterfallSplash_Flags[8] = {0x84, 0xff, 0x84, 0xc4, 0x84, 0xc4, 0x84, 0xc4};
  static const uint8 kWaterfallSplash_Ext[8] = {2, 0xff, 2, 2, 2, 2, 0, 0};

  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  OamEnt *oam = GetOamCurPtr();

  uint8 z = link_z_coord;
  pt.y -= (sign8(z) ? 0 : z);

  int j = ancilla_item_to_link[k] * 2;
  for (int i = 0; i != 2; i++, j++, oam++) {
    if (kWaterfallSplash_Char[j] != 0xff) {
      Ancilla_SetOam(oam, pt.x + kWaterfallSplash_X[j], pt.y + kWaterfallSplash_Y[j], kWaterfallSplash_Char[j],
                     kWaterfallSplash_Flags[j] | 0x30, kWaterfallSplash_Ext[j]);
    }
  }
}

// Ancilla type 0x24: Gravestone pusher — the large gravestone tile drawn over
// Link while he pushes a gravestone. Draws a 2×2 tile grid (tiles 0xC8/0xD8,
// each 16×8 px, in region B so it renders behind Link). The four OAM entries
// are laid out in a 32×16 px rectangle (2 tiles wide, 2 tiles tall) using
// palette 0x3D (slot 5 priority 3). Persists as long as the gravestone
// push submodule is active; freed externally by the push state machine.
void Ancilla24_Gravestone(int k) {  // 88ee01
  static const uint8 kAncilla_Gravestone_Char[4] = {0xc8, 0xc8, 0xd8, 0xd8};
  static const uint8 kAncilla_Gravestone_Flags[4] = {0, 0x40, 0, 0x40};
  Point16U pt;
  Ancilla_PrepAdjustedOamCoord(k, &pt);
  Oam_AllocateFromRegionB(16);
  OamEnt *oam = GetOamCurPtr();
  uint16 x = pt.x, y = pt.y;
  for (int i = 0; i < 4; i++, oam++) {
    Ancilla_SetOam(oam, x, y, kAncilla_Gravestone_Char[i], kAncilla_Gravestone_Flags[i] | 0x3d, 2);
    x += 16;
    if (i == 1)
      x -= 32, y += 8;
  }
}

// Ancilla type 0x34: Skull Woods fire columns — the descending fire attack in
// the Skull Woods boss fight. Manages 4 independent fire pillars via the
// skullwoodsfire_* global arrays (each pillar has its own X, Y, state var0,
// and timer var5).
//
// Per-pillar logic (inner k loop, 0–3):
//   var5[k] counts down each frame; when it expires (every 5 ticks):
//     - If var0[k] == 128 (pillar finished), the pillar is skipped.
//     - Otherwise var0[k] is incremented through frames 0→3; at frame 4 it
//       wraps to 0. While cycling, skullwoodsfire_var9 (global Y-position of
//       the leading fire head) is decremented by 8.
//     - When var9 drops below 200 and var4 == 0, the screen-shake phase
//       begins (var4 = 1) and a fire SFX is queued.
//     - When var9 < 168 the pillar is marked complete (var0[k] = 128).
//     - The current pillar's X/Y snapshot is saved into skullwoodsfire_x/y_arr.
//
// If all 4 pillars are complete (var0[k] all == 128) the slot is freed.
//
// Draw path 1 (active pillars): each pillar draws 1–2 OAM sprites using
//   kSkullWoodsFire_Draw_Char/Y/Ext, selecting by frame index var0[k].
// Draw path 2 (skullwoodsfire_var4 != 0, expanding blast):
//   Once the leading pillar breaks through the floor (var4 = 1 → 2 → ...) an
//   additional 6-OAM fireball burst is drawn from kSkullWoodsFire_Draw2_*
//   at fixed room position (168, 200 − BG2 scroll), cycling through 4 burst
//   frames in ancilla_item_to_link[k] at 5-tick intervals.
void Ancilla34_SkullWoodsFire(int k) {  // 88ef9a
  static const int8 kSkullWoodsFire_Draw_Y[4] = {0, 0, 0, -3};
  static const uint8 kSkullWoodsFire_Draw_Char[4] = {0x8e, 0xa0, 0xa2, 0xa4};
  static const uint8 kSkullWoodsFire_Draw_Ext[4] = {2, 2, 2, 0};
  static const int8 kSkullWoodsFire_Draw2_X[24] = {
    -13, -21, -10, -1,  -1,  -1, -16, -27, -4, -16, -6, -25, -16, -27, -4, -16,
    -6, -25, -13, -5, -27, -11, -22,  -3,
  };
  static const int8 kSkullWoodsFire_Draw2_Y[24] = {
    -31, -24, -22,  -1,  -1,  -1, -37, -32, -32, -23, -16, -14, -37, -32, -32, -23,
    -16, -14, -35, -29, -28, -20, -13, -11,
  };
  static const uint8 kSkullWoodsFire_Draw2_Char[24] = {
    0x86, 0x86, 0x86, 0xff, 0xff, 0xff, 0x86, 0x86, 0x86, 0x86, 0x86, 0x86, 0x8a, 0x8a, 0x8a, 0x8a,
    0x8a, 0x8a, 0x9b, 0x9b, 0x9b, 0x9b, 0x9b, 0x9b,
  };
  static const uint8 kSkullWoodsFire_Draw2_Flags[24] = {
    0, 0,    0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0x80, 0x40, 0x40, 0x80, 0x40, 0,
  };
  static const uint8 kSkullWoodsFire_Draw2_Ext[24] = {
    2, 2, 2, 2, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 0, 0, 0, 0, 0, 0,
  };
  if (skullwoodsfire_var4 && ancilla_item_to_link[k] != 4 && sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 5;
    ancilla_item_to_link[k]++;
  }
  OamEnt *oam = GetOamCurPtr();
  for(int k = 3; k >= 0; k--) {
    if (sign8(--skullwoodsfire_var5[k])) {
      skullwoodsfire_var5[k] = 5;
      if (skullwoodsfire_var0[k] == 128)
        goto endif_2;
      if (++skullwoodsfire_var0[k] != 0) {
        if (skullwoodsfire_var0[k] != 4)
          goto endif_2;
        skullwoodsfire_var0[k] = 0;
      }
      skullwoodsfire_var9 -= 8;
      if (skullwoodsfire_var9 < 200 && skullwoodsfire_var4 != 1) {
        skullwoodsfire_var4 = 1;
        sound_effect_1 = kBombos_Sfx[(uint8)(0x98 - BG2HOFS_copy2) >> 5] | 0xc;
      }
      if (skullwoodsfire_var9 < 168)
        skullwoodsfire_var0[k] = 128;
      skullwoodsfire_x_arr[k] = skullwoodsfire_var11;
      skullwoodsfire_y_arr[k] = skullwoodsfire_var9;
      if (sound_effect_1 == 0)
        sound_effect_1 = kBombos_Sfx[(uint8)(skullwoodsfire_var11 - BG2HOFS_copy2) >> 5] | 0x2a;
    }
endif_2:
    if (!sign8(skullwoodsfire_var0[k])) {
      int j = skullwoodsfire_var0[k];
      uint16 x = skullwoodsfire_x_arr[k] - BG2HOFS_copy2;
      uint16 y = skullwoodsfire_y_arr[k] - BG2VOFS_copy2 + kSkullWoodsFire_Draw_Y[j];
      Ancilla_SetOam(oam, x, y, kSkullWoodsFire_Draw_Char[j], 0x32, kSkullWoodsFire_Draw_Ext[j]);
      oam++;
      if (kSkullWoodsFire_Draw_Ext[j] != 2) {
        Ancilla_SetOam(oam, x + 8, y, kSkullWoodsFire_Draw_Char[j] + 1, 0x32, kSkullWoodsFire_Draw_Ext[j]);
        oam++;
      }
    }
  }

  for (int i = 3; sign8(skullwoodsfire_var0[i]); ) {
    if (--i < 0) {
      ancilla_type[k] = 0;
      return;
    }
  }

  if (skullwoodsfire_var4 == 0 || ancilla_item_to_link[k] == 4)
    return;

  int j = ancilla_item_to_link[k] * 6;
  for (int i = 0; i < 6; i++, j++) {
    if (kSkullWoodsFire_Draw2_Char[j] != 0xff) {
      Ancilla_SetOam(oam,
          168 - BG2HOFS_copy2 + kSkullWoodsFire_Draw2_X[j],
          200 - BG2VOFS_copy2 + kSkullWoodsFire_Draw2_Y[j],
          kSkullWoodsFire_Draw2_Char[j],
          kSkullWoodsFire_Draw2_Flags[j] | 0x32, kSkullWoodsFire_Draw2_Ext[j]);
      oam++;
    }
  }
}

// Ancilla type 0x3A: Super (Big) Bomb explosion. Plays an 11-frame explosion
// sequence identical in structure to the normal bomb but scaled to 9 blast
// positions arranged in a 3×3 grid (kSuperBombExplode_X/Y offsets). Each
// position renders a full bomb explosion sprite group via AncillaDraw_Explosion.
//
// Timing: ancilla_arr3[k] counts down per-frame; when expired item_to_link
//   is incremented and arr3 is reloaded from kBomb_Tab0. At frame 11 the
//   slot is freed. SFX 0x0C is played at frame 2.
//
// OAM allocation: Ancilla_AllocateOamFromRegion_A_or_D_or_F is called once per
//   visible position with step = j * 2 (the frame's OAM region start). Only
//   positions whose screen coordinates fall within 256×256 are rendered.
//
// Destructibles check: at frame 3, tick 1 Bomb_CheckForDestructibles is called
//   at the blast centre with follower_indicator temporarily set to 13
//   (destruction class for big bombs). An enhanced-features flag restores the
//   original follower_indicator afterwards; without it, value 13 persists
//   (original SNES behaviour).
void Ancilla3A_BigBombExplosion(int k) {  // 88f18d
  static const int8 kSuperBombExplode_X[9] = {0, -16, 0, 16, -24, 24, -16, 0, 16};
  static const int8 kSuperBombExplode_Y[9] = {0, -16, -24, -16, 0, 0, 16, 24, 16};

  if (!submodule_index && !--ancilla_arr3[k]) {
    if (++ancilla_item_to_link[k] == 2)
      Ancilla_Sfx2_Pan(k, 0xc);
    if (ancilla_item_to_link[k] == 11) {
      ancilla_type[k] = 0;
      return;
    }
    ancilla_arr3[k] = kBomb_Tab0[ancilla_item_to_link[k]];
  }
  oam_priority_value = 0x3000;
  uint8 numframes = kBomb_Draw_Tab2[ancilla_item_to_link[k]];
  int j = kBomb_Draw_Tab0[ancilla_item_to_link[k]] * 6;
  ancilla_step[k] = j * 2;

  int yy = 0;
  for (int i = 8; i >= 0; i--) {
    uint16 x = Ancilla_GetX(k) + kSuperBombExplode_X[i] - BG2HOFS_copy2;
    uint16 y = Ancilla_GetY(k) + kSuperBombExplode_Y[i] - BG2VOFS_copy2;
    if (x < 256 && y < 256) {
      Ancilla_AllocateOamFromRegion_A_or_D_or_F((uint8)(j * 2), 0x18); // wtf
      OamEnt *oam = GetOamCurPtr() + yy;
      yy += AncillaDraw_Explosion(oam, j, 0, numframes, 0x32, x, y) - oam;

    }
  }
  if (ancilla_item_to_link[k] == 3 && ancilla_arr3[k] == 1) {
    // Changed so this is reset elsewhere. Some code depends on the value 13.
    uint8 old = (enhanced_features0 & kFeatures0_MiscBugFixes) ? follower_indicator : 0;
    follower_indicator = 13;
    Bomb_CheckForDestructibles(Ancilla_GetX(k), Ancilla_GetY(k), 0); // r14?
    follower_indicator = old;
  }
}

// Revival fairy controller — drives the game-over save sequence. Uses ancilla
// slots 0, 1, and 2 with hard-coded indices (k = 0/1/2) rather than the
// parameter k. This function is called directly by the game-over death module.
//
// Slot 0 state machine (the fairy sprite):
//   step 0: descent — ancilla_arr3 counts down; meanwhile Ancilla_MoveZ moves
//     the fairy downward into Link. On completion advances to step 1 and loads
//     arr3 = 0x90 (144 frames of hover).
//   step 1: hover — oscillates Z ± 8 (ancilla_K[k] flip) to produce a bob.
//     At frame 0x4F and 0x8F (quarter-points) plays a SFX and increments
//     ancilla_L to trigger the sprite colour cycle; item_to_link cycles 0→2
//     over 5-tick intervals (arr1 sub-timer selects from kAncilla_RevivalFaerie_Tab1).
//     On completion of arr3 advances to step 2.
//   step 2: fly-away — accelerates z_vel (+1) and x_vel (+1) to 24/16 max.
//     MoveX and MoveZ carry the fairy off-screen. step 3 is the exit state.
//   step 3 / off-screen: skips draw, calls RevivalFairy_Dust and _MonitorHP.
//
// Draw (slot 0, steps 0–2):
//   Single 16×16 sprite from kAncilla_RevivalFaerie_Tab1 (tile varies by state
//   and frame_counter blink). If the OAM Y is 0xF0 (off-screen), step → 3,
//   submodule_index is incremented (advances the death sequence), and TM_copy
//   is restored from mapbak_TM.
//
// Helpers called each frame:
//   RevivalFairy_Dust  — generates particle dust from the magic powder trail.
//   RevivalFairy_MonitorHP — checks if Link is fully healed; if so resets
//     all player state and frees all 5 ancilla slots to end the sequence.
void RevivalFairy_Main() {  // 88f283
  static const uint8 kAncilla_RevivalFaerie_Tab0[2] = {0, 0x90};
  static const uint8 kAncilla_RevivalFaerie_Tab1[5] = {0x4b, 0x4d, 0x49, 0x47, 0x49};

  int k = 0;
  switch (ancilla_step[k]) {
  case 0:
    if (!--ancilla_arr3[k]) {
      ancilla_arr3[k] = kAncilla_RevivalFaerie_Tab0[++ancilla_step[k]];
      ancilla_K[k] = 0;
      ancilla_z_vel[k] = 0;
    } else {
      Ancilla_MoveZ(k);
    }
    break;
  case 1:
    if (!--ancilla_arr3[k]) {
      ancilla_step[k]++;
      ancilla_z_vel[k] = 0;
      ancilla_x_vel[k] = 0;
    } else {
      if (ancilla_arr3[k] == 0x4f || ancilla_arr3[k] == 0x8f) {
        ancilla_L[k]++;
        Ancilla_Sfx2_Pan(k, 0x31);
      }
      if (ancilla_L[k] != 0 && sign8(--ancilla_G[k])) {
        ancilla_G[k] = 5;
        if (++ancilla_item_to_link[k] == 3) {
          ancilla_item_to_link[k] = 0;
          ancilla_L[k] = 0;
        }
      }
      ancilla_z_vel[k] += ancilla_K[k] ? 1 : -1;
      if (abs8(ancilla_z_vel[k]) == 8)
        ancilla_K[k] ^= 1;
      Ancilla_MoveZ(k);
    }
    break;
  case 2:
    if (ancilla_z_vel[k] < 24)
      ancilla_z_vel[k] += 1;
    if (ancilla_x_vel[k] < 16)
      ancilla_x_vel[k] += 1;
    Ancilla_MoveX(k);
    Ancilla_MoveZ(k);
    break;
  case 3:
    goto skip_draw;
  }

  {
    Oam_AllocateFromRegionC(12);
    Point16U pt;
    Ancilla_PrepOamCoord(k, &pt);
    OamEnt *oam = GetOamCurPtr();
    int t = (ancilla_step[k] == 1 && ancilla_L[k]) ? ancilla_item_to_link[k] + 1 : 0;
    if (t != 0)
      t += 1;
    else
      t = (frame_counter >> 2) & 1;
    Ancilla_SetOam(oam, pt.x, pt.y - (int8)ancilla_z[k], kAncilla_RevivalFaerie_Tab1[t], 0x74, 2);
    if (oam->y == 0xf0) {
      ancilla_step[k] = 3;
      submodule_index++;
      TM_copy = mapbak_TM;
    }
  }
skip_draw:
  RevivalFairy_Dust();
  RevivalFairy_MonitorHP();
}

// Generates the magic-powder dust trail from the revival fairy (slot 2).
// Called each frame from RevivalFairy_Main. Only active while slot 0 step ≥ 1
// and slot 2 step < 2 (hovering phase). Runs once per zero-crossing of
// ancilla_arr3[2] (immediately reloads). Allocates OAM from region A (single
// floor) or D (two-floor sort) for 16 bytes.
//
// Advances item_to_link[2] up to 9 (10 powder frames) via a 3-tick aux_timer.
// Each advance loads the next kMagicPowder_Tab0[30+n] draw descriptor into
// ancilla_arr25[2] and calls Ancilla_MagicPowder_Draw for the OAM output.
// At frame 9, arr3 is set to 32 (pause before step 2) and step → 2.
void RevivalFairy_Dust() {  // 88f3cf
  int k = 2;
  if (ancilla_step[0] == 0 || ancilla_step[k] == 2 || !sign8(--ancilla_arr3[k]))
    return;
  ancilla_arr3[k] = 0;
  if (!sort_sprites_setting)
    Oam_AllocateFromRegionA(16);
  else
    Oam_AllocateFromRegionD(16);
  if (sign8(--ancilla_aux_timer[k])) {
    ancilla_aux_timer[k] = 3;
    if (ancilla_item_to_link[k] == 9) {
      ancilla_arr3[k] = 32;
      ancilla_step[k]++;
      ancilla_item_to_link[k] = 2;
      return;
    }
    ancilla_arr25[k] = kMagicPowder_Tab0[30 + ++ancilla_item_to_link[k]];
  }
  Ancilla_MagicPowder_Draw(k);
}

// Monitors Link's health during the revival sequence and drives the healing
// heart animation via slot 1.
//
// Exit condition: if link_health_current has reached full capacity (or 0x38 = 7
// hearts full) and the heart animation is complete (is_doing_heart_animation == 0):
//   - Restores the appropriate player state (swimming, bunny, or ground).
//   - Clears auxiliary and animation state variables.
//   - Frees all 5 ancilla slots, ending the revival sequence.
//
// Slot 1 (heart fill pulse, uses hard-coded k = 1):
//   step 0: waits for arr3 countdown; each tick calls MoveZ with z_vel = 4
//     to lift the heart visual. When z reaches ≥ 16, step → 1 and z_vel → 2.
//   step 1: oscillates z_vel between +2 and −2 (K[k] counter = 32) to produce
//     a bobbing heart that slowly rises. Each tick Ancilla_MoveZ updates Z.
// link_z_coord is written from ancilla_z[1] each frame so Link visually
// rises during the revival animation.
void RevivalFairy_MonitorHP() {  // 88f430
  if ((link_health_current == link_health_capacity || link_health_current == 0x38) && !is_doing_heart_animation) {
    if (link_is_in_deep_water) {
      link_some_direction_bits = 4;
      link_player_handler_state = kPlayerState_Swimming;
    } else if (link_is_bunny) {
      link_player_handler_state = kPlayerState_PermaBunny;
      link_is_bunny_mirror = 1;
      //bugfix: dying as permabunny doesn't restore link palette during death animation
      if (enhanced_features0 & kFeatures0_MiscBugFixes)
        LoadGearPalettes_bunny();
    } else {
      link_player_handler_state = kPlayerState_Ground;
    }
    link_auxiliary_state = 0;
    player_unk1 = 0;
    link_var30d = 0;
    some_animation_timer_steps = 0;
    BYTE(link_z_coord) = 0;
    link_incapacitated_timer = 0;
    for(int i = 0; i < 5; i++)
      ancilla_type[i] = 0;
    return;
  }
  int k = 1;
  if (!ancilla_step[k]) {
    if (!--ancilla_arr3[k]) {
      ancilla_arr3[k]++;
      ancilla_z_vel[k] = 4;
      Ancilla_MoveZ(k);
      if (ancilla_z[k] >= 16) {
        ancilla_step[k]++;
        ancilla_z_vel[k] = 2;
      }
    }
  } else {
    if (sign8(--ancilla_K[k])) {
      ancilla_K[k] = 32;
      ancilla_z_vel[k] = -ancilla_z_vel[k];
    }
    Ancilla_MoveZ(k);
  }
  BYTE(link_z_coord) = ancilla_z[k];
}

// Draws the "GAME OVER" text on-screen during the death sequence. Resets the
// OAM pointer to the beginning of the sprite buffer (0x800 / 0xA20) and emits
// 8 sprite pairs — each pair (top and bottom half-character) is placed at the
// X coordinate stored in ancilla_x_lo[k] for k = flag_for_boomerang_in_place
// down to 0. kGameOverText_Chars[16] maps each character slot (0–7) to its
// upper and lower tile numbers. All entries use palette/priority 0x3C.
void GameOverText_Draw() {  // 88f5c4
  static const uint8 kGameOverText_Chars[16] = {0x40, 0x50, 0x41, 0x51, 0x42, 0x52, 0x43, 0x53, 0x44, 0x54, 0x45, 0x55, 0x43, 0x53, 0x46, 0x56};
  oam_cur_ptr = 0x800;
  oam_ext_cur_ptr = 0xa20;
  OamEnt *oam = GetOamCurPtr();
  int k = flag_for_boomerang_in_place;
  do {
    Ancilla_SetOam(oam + 0, Ancilla_GetX(k), 0x57, kGameOverText_Chars[k * 2 + 0], 0x3c, 0);
    Ancilla_SetOam(oam + 1, Ancilla_GetX(k), 0x5f, kGameOverText_Chars[k * 2 + 1], 0x3c, 0);
  } while (oam += 2, --k >= 0);
}

// Allocates and initialises a new ancilla slot for the given type and Y
// tile-coordinate. Calls Ancilla_AllocInit (which searches for a free slot
// and sets the initial world position). On success initialises the slot:
//   - type, numspr (from kAncilla_Pflags[type]), floor / floor2 from Link's
//     current level flags (link_is_on_lower_level / mirror).
//   - Clears y_vel, x_vel, objprio, and U.
// Returns the slot index, or negative if no slot was available.
int AncillaAdd_AddAncilla_Bank08(uint8 type, uint8 y) {  // 88f631
  int k = Ancilla_AllocInit(type, y);
  if (k >= 0) {
    ancilla_type[k] = type;
    ancilla_numspr[k] = kAncilla_Pflags[type];
    ancilla_floor[k] = link_is_on_lower_level;
    ancilla_floor2[k] = link_is_on_lower_level_mirror;
    ancilla_y_vel[k] = 0;
    ancilla_x_vel[k] = 0;
    ancilla_objprio[k] = 0;
    ancilla_U[k] = 0;
  }
  return k;
}

// Sets oam_priority_value from kTagalongLayerBits[ancilla_floor[k]] and
// computes the screen-space position of ancilla k into *info, subtracting the
// BG2 copy2 scroll offsets. Used for projectiles and effects that should be
// positioned relative to the fine-scroll (copy2) camera.
void Ancilla_PrepOamCoord(int k, Point16U *info) {  // 88f671
  oam_priority_value = kTagalongLayerBits[ancilla_floor[k]] << 8;
  info->x = Ancilla_GetX(k) - BG2HOFS_copy2;
  info->y = Ancilla_GetY(k) - BG2VOFS_copy2;
}

// Same as Ancilla_PrepOamCoord but uses BG2HOFS_copy / BG2VOFS_copy (the
// coarser, tile-aligned scroll copy) instead of copy2. Used for items and
// effects that should track tile-aligned positions (e.g. received items,
// Somaria blocks) to avoid sub-pixel jitter.
void Ancilla_PrepAdjustedOamCoord(int k, Point16U *info) {  // 88f6a4
  oam_priority_value = kTagalongLayerBits[ancilla_floor[k]] << 8;
  info->x = Ancilla_GetX(k) - BG2HOFS_copy;
  info->y = Ancilla_GetY(k) - BG2VOFS_copy;
}

// Tests whether ancilla k is close enough to Link to register a collision,
// using one of 5 hit-box shapes indexed by j:
//   j=0: 20×20 px, no offset (large, centred).
//   j=1: 3×20 px, shifted +8/+8 (narrow vertical strip, used for items).
//   j=2: 8×8 px, shifted +8/+8 (tiny square, precise pickup).
//   j=3: 24×28 px, shifted +8/+12 (wide, tall — used for the duck).
//   j=4: 14×14 px, no shift (medium).
//
// The ancilla's Z-height (ancilla_z[k]) is added to the Y offset so elevated
// objects require Link to be at the same altitude to trigger collection.
//
// Returns true if the distances in both axes are within the threshold.
// Fills *out with the raw signed deltas (r4/r6) and their absolute values
// (r8/r10) for callers that need direction information.
bool Ancilla_CheckLinkCollision(int k, int j, CheckPlayerCollOut *out) {  // 88f76b
  static const int16 kAncilla_Coll_Yoffs[5] = {0, 8, 8, 8, 0};
  static const int16 kAncilla_Coll_Xoffs[5] = {0, 8, 8, 8, 0};
  static const int16 kAncilla_Coll_H[5] = {20, 20, 8, 28, 14};
  static const int16 kAncilla_Coll_W[5] = {20, 3, 8, 24, 14};
  static const int16 kAncilla_Coll_LinkYoffs[5] = {12, 12, 12, 12, 12};
  static const int16 kAncilla_Coll_LinkXoffs[5] = {8, 8, 8, 12, 8};
  uint16 x = Ancilla_GetX(k), y = Ancilla_GetY(k);
  y += kAncilla_Coll_Yoffs[j] + (int8)ancilla_z[k];
  x += kAncilla_Coll_Xoffs[j];
  out->r4 = link_y_coord + kAncilla_Coll_LinkYoffs[j] - y;
  out->r8 = abs16(out->r4);
  out->r6 = link_x_coord + kAncilla_Coll_LinkXoffs[j] - x;
  out->r10 = abs16(out->r6);
  return out->r8 < kAncilla_Coll_H[j] && out->r10 < kAncilla_Coll_W[j];
}

// Returns true if a screen-space point (x, y) — a hookshot chain link — is
// within 12 px of Link's torso centre in both axes. The comparison uses
// BG2 copy2 scroll offsets to convert Link's world coordinates to screen space.
// Used to suppress chain-link OAM entries that would overlap Link's sprite.
bool Hookshot_CheckProximityToLink(int x, int y) {  // 88f7dc
  return abs16(link_y_coord - BG2VOFS_copy2 + 12 - y - 4) < 12 &&
         abs16(link_x_coord - BG2HOFS_copy2 +  8 - x - 4) < 12;
}

// Returns true if Link's position is within a specific entrance trigger zone.
// Four zones are defined (indexed by what = 0–3):
//   0: waterfall-entrance zone (dungeon side).
//   1: waterfall-entrance zone (overworld side).
//   2: small doorway trigger.
//   3: alternate entrance zone.
// Each zone is an axis-aligned rectangle described by kEntranceTrigger_Base*
// (centre) and kEntranceTrigger_Size* (half-extents). The check uses abs16
// of the signed distance from Link's torso (+12 Y, +8 X) to the zone centre.
bool Ancilla_CheckForEntranceTrigger(int what) {  // 88f844
  static const uint16 kEntranceTrigger_BaseY[4] = {0xd40, 0x210, 0xcfc, 0x100};
  static const uint16 kEntranceTrigger_BaseX[4] = {0xd80, 0xe68, 0x130, 0xf10};
  static const uint8 kEntranceTrigger_SizeY[4] = {11, 32, 16, 12};
  static const uint8 kEntranceTrigger_SizeX[4] = {16, 16, 16, 16};
  return
    abs16(link_y_coord + 12 - kEntranceTrigger_BaseY[what]) < kEntranceTrigger_SizeY[what] &&
    abs16(link_x_coord + 8 - kEntranceTrigger_BaseX[what]) < kEntranceTrigger_SizeX[what];
}

// Draws a drop-shadow at screen position (x, y) using one of 7 shadow shapes
// (index k = 0–6) from the kAncilla_DrawShadow_* tables. Each shape consists
// of 1–2 OAM entries (the second is omitted if Char == 0xFF). The caller
// passes the desired palette byte in pal (e.g. 0x30 for sprites layer 3).
// Shape k=2 is horizontally centred (+4 X offset). Shadows always use tile
// size ext = 0 (8×8).
void AncillaDraw_Shadow(OamEnt *oam, int k, int x, int y, uint8 pal) {  // 88f897
  static const uint8 kAncilla_DrawShadow_Char[14] = {0x6c, 0x6c, 0x28, 0x28, 0x38, 0xff, 0xc8, 0xc8, 0xd8, 0xd8, 0xd9, 0xd9, 0xda, 0xda};
  static const uint8 kAncilla_DrawShadow_Flags[14] = {0x28, 0x68, 0x28, 0x68, 0x28, 0xff, 0x22, 0x22, 0x24, 0x64, 0x24, 0x64, 0x24, 0x64};

  if (k == 2)
    x += 4;
  Ancilla_SetOam_Safe(oam, x, y,
                      kAncilla_DrawShadow_Char[k * 2],
                      kAncilla_DrawShadow_Flags[k * 2] & ~0x30 | pal, 0);
  uint8 ch = kAncilla_DrawShadow_Char[k * 2 + 1];
  if (ch != 0xff) {
    x += 8;
    Ancilla_SetOam_Safe(oam + 1, x, y, ch, kAncilla_DrawShadow_Flags[k * 2 + 1] & ~0x30 | pal, 0);
  }
}

// Routes an OAM allocation to region B (non-sort) or region E (2-floor sort
// mode), depending on sort_sprites_setting. Used when an ancilla must draw
// behind Link (region B is below the player sprite layer; region E is the
// equivalent slot in split-floor OAM ordering).
void Ancilla_AllocateOamFromRegion_B_or_E(uint8 size) {  // 88f90a
  if (!sort_sprites_setting)
    Oam_AllocateFromRegionB(size);
  else
    Oam_AllocateFromRegionE(size);
}

// Wraps the current OAM pointer, preventing it from overflowing the allocated
// region. If the pointer is already within a valid sub-region it is returned
// unchanged. If it has overflowed the sub-region boundary it is reset to the
// start of the next available sub-region (wrap-around). In 2-floor sort mode
// the boundaries differ for the upper-floor bank (0x900–0x9CF) versus the
// lower-floor bank (0x800–0x8FF). Returns the corrected OAM pointer.
OamEnt *Ancilla_AllocateOamFromCustomRegion(OamEnt *oam) {  // 88f9ba
  int a = (uint8 *)oam - g_ram;
  if (sort_sprites_setting) {
    if (a < 0x900) {
      if (a < 0x8e0)
        return oam;
      a = 0x820;
    } else {
      if (a < 0x9d0)
        return oam;
      a = 0x940;
    }
  } else {
    if (a < 0x990)
      return oam;
    a = 0x820;
  }
  oam_cur_ptr = a;
  oam_ext_cur_ptr = ((a - 0x800) >> 2) + 0xa20;
  return GetOamCurPtr();
}

// Prevents the hit-stars OAM pointer from overflowing the sprite buffer.
// In single-floor mode (sort_sprites_setting == 0) resets the pointer to
// 0x820 / ext 0xA28 if it has advanced past 0x9D0. In 2-floor sort mode
// the pointer is allowed to run freely. Returns the corrected pointer.
OamEnt *HitStars_UpdateOamBufferPosition(OamEnt *oam) {  // 88fa00
  int a = (uint8 *)oam - g_ram;
  if (!sort_sprites_setting && a >= 0x9d0) {
    oam_cur_ptr = 0x820;
    oam_ext_cur_ptr = 0xa20 + (0x20 >> 2);
    oam = GetOamCurPtr();
  }
  return oam;
}

// Returns true if the hookshot head (ancilla k) is outside the tile-collision
// region and tile checks should be skipped. On the overworld this means the
// head has scrolled past the current area's tile-data boundary in the travel
// direction (using kOverworld_OffsetBaseX/Y and overworld_right_bottom_bound).
// In a dungeon it means the head is in a different 0x200-px page from Link
// (bit 9 of the coordinate differs) or is within 4 px of the page boundary —
// both conditions where tile indexing would be unreliable.
bool Hookshot_ShouldIEvenBotherWithTiles(int k) {  // 88fa2d
  uint16 x = Ancilla_GetX(k), y = Ancilla_GetY(k);
  if (!player_is_indoors) {
    if (!(ancilla_dir[k] & 2)) {
      uint16 t = y - kOverworld_OffsetBaseY[BYTE(current_area_of_player) >> 1];
      return (t < 4) || (t >= overworld_right_bottom_bound_for_scroll);
    } else {
      uint16 t = x - kOverworld_OffsetBaseX[BYTE(current_area_of_player) >> 1];
      return (t < 6) || (t >= overworld_right_bottom_bound_for_scroll);
    }
  }
  if (!(ancilla_dir[k] & 2)) {
    return (y & 0x1ff) < 4 || (y & 0x1ff) >= 0x1e8 || (y & 0x200) != (link_y_coord & 0x200);
  } else {
    return (x & 0x1ff) < 4 || (x & 0x1ff) >= 0x1f0 || (x & 0x200) != (link_x_coord & 0x200);
  }
}

// Converts a 64-step circular angle (a, 0–63) and radius (r8, pixels) into a
// 2D displacement stored in an AncillaRadialProjection struct:
//   rv.r0 = Y magnitude (0–r8).   rv.r2 = Y sign bit (1 = negative).
//   rv.r4 = X magnitude (0–r8).   rv.r6 = X sign bit (1 = negative).
//
// The four look-up tables encode a discrete sin/cos using an 8-bit fixed-point
// representation (0–255 = 0.0–1.0). The magnitude is computed as
// (table[a] * r8) >> 8, rounded by the bit below. kRadialProjection_Tab1/3
// encode the sign bits (1 = negative) so callers can apply them with ternary
// sign-flip. This produces a complete polar-to-Cartesian conversion without
// floating-point arithmetic, suitable for the Ether orb ring, sword beam, and
// Ganon's Tower crystal orbital paths.
AncillaRadialProjection Ancilla_GetRadialProjection(uint8 a, uint8 r8) {  // 88fadd
  static const uint8 kRadialProjection_Tab0[64] = {
    255, 254, 251, 244, 236, 225, 212, 197, 181, 162, 142, 120,  97,  74,  49,  25,
      0,  25,  49,  74,  97, 120, 142, 162, 181, 197, 212, 225, 236, 244, 251, 254,
    255, 254, 251, 244, 236, 225, 212, 197, 181, 162, 142, 120,  97,  74,  49,  25,
      0,  25,  49,  74,  97, 120, 142, 162, 181, 197, 212, 225, 236, 244, 251, 254,
  };
  static const uint8 kRadialProjection_Tab2[64] = {
      0,  25,  49,  74,  97, 120, 142, 162, 181, 197, 212, 225, 236, 244, 251, 254,
    255, 254, 251, 244, 236, 225, 212, 197, 181, 162, 142, 120,  97,  74,  49,  25,
      0,  25,  49,  74,  97, 120, 142, 162, 181, 197, 212, 225, 236, 244, 251, 254,
    255, 254, 251, 244, 236, 225, 212, 197, 181, 162, 142, 120,  97,  74,  49,  25,
  };
  static const uint8 kRadialProjection_Tab1[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };
  static const uint8 kRadialProjection_Tab3[64] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };
  AncillaRadialProjection rv;
  int p0 = kRadialProjection_Tab0[a] * r8;
  int p1 = kRadialProjection_Tab2[a] * r8;
  rv.r0 = (p0 >> 8) + (p0 >> 7 & 1);
  rv.r2 = kRadialProjection_Tab1[a];
  rv.r4 = (p1 >> 8) + (p1 >> 7 & 1);
  rv.r6 = kRadialProjection_Tab3[a];
  return rv;
}

// Allocates OAM space from the appropriate region based on the floor sort mode
// and ancilla_floor[k]:
//   sort_sprites_setting == 0 → region A (all sprites, no floor split).
//   sort_sprites_setting != 0, ancilla_floor[k] != 0 → region F (upper floor).
//   sort_sprites_setting != 0, ancilla_floor[k] == 0 → region D (lower floor).
// Returns the allocated OAM index (or negative on failure, depending on region).
int Ancilla_AllocateOamFromRegion_A_or_D_or_F(int k, uint8 size) {  // 88fb2b
  if (sort_sprites_setting) {
    if (ancilla_floor[k])
      return Oam_AllocateFromRegionF(size);
    else
      return Oam_AllocateFromRegionD(size);
  } else {
    return Oam_AllocateFromRegionA(size);
  }
}

// Spawns the shovel/sword impact hit-star ancilla (type a). Allocates a slot
// via Ancilla_AddAncilla, then determines which of the 6 positional presets
// (kShovelHitStars_XY / kShovelHitStars_X2) to use:
//   link_item_in_hand → presets 2–5 by direction (link_direction_facing >> 1).
//   link_position_mode (lying) → preset 0 or 1 by facing == 4.
//   otherwise → preset a (caller-supplied default).
// The ancilla is positioned at link_x/y + the chosen offsets. ancilla_A/B store
// the secondary X coordinate used by the hit-star draw path.
void Ancilla_AddHitStars(uint8 a, uint8 y) {  // 898024
  static const int8 kShovelHitStars_XY[12] = {21, -11, 21, 11, 3, -6, 21, 5, 16, -14, 16, 14};
  static const int8 kShovelHitStars_X2[6] = {-3, 19, 2, 13, -6, 22};
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 2;
    ancilla_arr3[k] = 1;
    ancilla_y_vel[k] = 0;
    ancilla_x_vel[k] = 0;
    int j = a;
    if (link_item_in_hand) {
      j = (link_direction_facing >> 1) + 2;
    } else if (link_position_mode) {
      j = link_direction_facing != 4 ? 1 : 0;
    }
    ancilla_step[k] = j;
    int t = link_x_coord + kShovelHitStars_X2[j];
    ancilla_A[k] = t;
    ancilla_B[k] = t >> 8;
    Ancilla_SetXY(k, link_x_coord + kShovelHitStars_XY[j * 2 + 1], link_y_coord + kShovelHitStars_XY[j * 2 + 0]);
  }
}

// Spawns the blanket ancilla (Ancilla20_Blanket, type a) in slot 0. Unlike
// most ancillae this always uses slot 0 directly (hard-coded k = 0) because
// the intro cutscene guarantees slot 0 is free. Positions the blanket at world
// coordinate (0x938, 0x2162) — Link's bed position in the intro room.
void AncillaAdd_Blanket(uint8 a) {  // 898091
  int k = 0;
  ancilla_type[k] = a;
  ancilla_numspr[k] = kAncilla_Pflags[a];
  ancilla_floor[k] = link_is_on_lower_level;
  ancilla_floor2[k] = link_is_on_lower_level_mirror;
  ancilla_objprio[k] = 0;
  Ancilla_SetXY(k, 0x938, 0x2162);
}

// Spawns the snore-bubble ancilla (Ancilla21_Snore, type a) above Link. Sets
// the initial upward velocity (y_vel = −8) and horizontal drift (x_vel = 8)
// so the bubble floats diagonally upward. ancilla_step = 255 (−1 signed) is
// used as the initial oscillation direction flag. aux_timer = 7 seeds the
// first sub-frame of the wobble animation.
void AncillaAdd_Snoring(uint8 a, uint8 y) {  // 8980c8
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_y_vel[k] = -8;
    ancilla_aux_timer[k] = 7;
    ancilla_x_vel[k] = 8;
    ancilla_step[k] = 255;
    Ancilla_SetXY(k, link_x_coord + 16, link_y_coord + 4);
  }
}

// Spawns a bomb ancilla (Ancilla07_Bomb, type a). Decrements link_item_bombs;
// if the player had no bombs the slot is immediately freed. Refreshes the HUD
// when the count drops to 0. Initialises all bomb fields: timer (8 ticks before
// it starts detonating), arr3 from kBomb_Tab0[0] (first detonation frame),
// and direction from link_direction_facing >> 1. ancilla_arr25/arr26 are set
// to 0/7 (used by door-debris subtype, not active bombs). Position uses one of
// two sets of direction offsets: kBomb_Place_X0/Y0 when the initial tile check
// indicates the bomb would be inside a wall (fallback centring), or
// kBomb_Place_X1/Y1 for the normal placement in front of Link.
void AncillaAdd_Bomb(uint8 a, uint8 y) {  // 89811f
  static const int8 kBomb_Place_X0[4] = {8, 8, 0, 16};
  static const int8 kBomb_Place_Y0[4] = {0, 24, 12, 12};
  static const int8 kBomb_Place_X1[4] = {8, 8, -6, 22};
  static const int8 kBomb_Place_Y1[4] = {4, 28, 12, 12};

  int k = Ancilla_AddAncilla(a, y);
  if (k < 0)
    return;
  if (link_item_bombs == 0) {
    ancilla_type[k] = 0;
    return;
  }

  if (--link_item_bombs == 0)
    Hud_RefreshIcon();

  ancilla_R[k] = 0;
  ancilla_step[k] = 0;
  ancilla_item_to_link[k] = 0;
  ancilla_L[k] = 0;
  ancilla_arr3[k] = kBomb_Tab0[0];

  // These are not used directly by bombs, but used by door debris
  ancilla_arr25[k] = 0;
  ancilla_arr26[k] = 7;

  ancilla_z[k] = 0;
  ancilla_timer[k] = 8;
  ancilla_dir[k] = link_direction_facing >> 1;
  ancilla_T[k] = 0;
  ancilla_arr23[k] = 0;
  ancilla_arr22[k] = 0;
  if (Ancilla_CheckInitialTileCollision_Class2(k)) {
    int j = link_direction_facing >> 1;
    Ancilla_SetXY(k, link_x_coord + kBomb_Place_X0[j], link_y_coord + kBomb_Place_Y0[j]);
  } else {
    int j = link_direction_facing >> 1;
    Ancilla_SetXY(k, link_x_coord + kBomb_Place_X1[j], link_y_coord + kBomb_Place_Y1[j]);
  }
  sound_effect_1 = Link_CalculateSfxPan() | 0xb;
}

// Spawns a boomerang ancilla (Ancilla05, type a). Returns a negative value on
// failure or if the boomerang immediately hits a wall (clink). Sets
// flag_for_boomerang_in_place = 1 to inform Ancilla05 that it was just spawned.
//
// Velocity and direction resolution from the joypad:
//   The diagonal button held on the last joypad sample (joypad1H_last & 0xF)
//   is decomposed into Y and X velocity components using kBoomerang_Tab0 as
//   the speed magnitude (Blue boomerang = faster, Red = slower). If no diagonal
//   button is held the boomerang travels in link_direction_facing.
//
//   r1 = last D-pad byte (or fallback direction bit from kBoomerang_Tab3).
//   Y axis (r1 & 0xC): y_vel set to ±H; ancilla_dir and hookshot_effect_index
//     record the primary direction (0 = up, 1 = down).
//   X axis (r1 & 3): x_vel set to ±H; ancilla_S[k] = 1 if moving left.
//     hookshot_effect_index ORed with the X bit from kBoomerang_Tab3.
//
// Extra-range throw: if button_b_frames >= 9 (held long), aux_timer[k] is set
//   to 1, selecting position table kBoomerang_Tab8/9 (longer throw offset).
//
// Tile collision check: Ancilla_CheckInitialTile_A verifies the spawn position
//   is clear. On collision the slot is freed, flag_for_boomerang_in_place is
//   cleared, and a wall-clink sound + AncillaAdd_BoomerangWallClink fires.
uint8 AncillaAdd_Boomerang(uint8 a, uint8 y) {  // 89820f
  static const uint8 kBoomerang_Tab0[4] = {0x20, 0x18, 0x30, 0x28};
  static const uint8 kBoomerang_Tab1[2] = {0x20, 0x60};
  static const uint8 kBoomerang_Tab2[2] = {3, 2};
  static const uint8 kBoomerang_Tab3[4] = {8, 4, 2, 1};
  static const uint8 kBoomerang_Tab4[8] = {8, 4, 2, 1, 9, 5, 10, 6};
  static const uint8 kBoomerang_Tab5[8] = {2, 3, 3, 2, 2, 3, 3, 3};
  static const int8 kBoomerang_Tab6[8] = {-10, -8, -9, -9, -10, -8, -9, -9};
  static const int8 kBoomerang_Tab7[8] = {-10, 11, 8, -8, -10, 11, 8, -8};
  static const int8 kBoomerang_Tab8[8] = {-16, 6, 0, 0, -8, 8, -8, 8};
  static const int8 kBoomerang_Tab9[8] = {0, 0, -8, 8, 8, 8, -8, -8};

  int k = Ancilla_AddAncilla(a, y);
  if (k < 0)
    return 0;
  ancilla_aux_timer[k] = 0;
  ancilla_item_to_link[k] = 0;
  ancilla_K[k] = 0;
  ancilla_z[k] = 0;
  ancilla_L[k] = ancilla_numspr[k];
  flag_for_boomerang_in_place = 1;
  int j = link_item_boomerang - 1;
  ancilla_G[k] = j;
  ancilla_step[k] = kBoomerang_Tab1[j];
  ancilla_arr3[k] = kBoomerang_Tab2[j];

  int s = ancilla_G[k] * 2 + ((joypad1H_last & 0xc) && (joypad1H_last & 3) ? 1 : 0);
  uint8 r0 = kBoomerang_Tab0[s];
  ancilla_H[k] = r0;

  uint8 r1 = (joypad1H_last & 0xf) ? (joypad1H_last & 0xf) : kBoomerang_Tab3[link_direction_facing >> 1];
  hookshot_effect_index = 0;

  if (r1 & 0xc) {
    ancilla_y_vel[k] = r1 & 8 ? -r0 : r0;
    int i = sign8(ancilla_y_vel[k]) ? 0 : 1;
    ancilla_dir[k] = i;
    hookshot_effect_index = kBoomerang_Tab3[i];
  }
  ancilla_S[k] = 0;

  if (r1 & 3) {
    if (!(r1 & 2))
      ancilla_S[k] = 1;
    ancilla_x_vel[k] = (r1 & 2) ? -r0 : r0;
    int i = sign8(ancilla_x_vel[k]) ? 2 : 3;
    ancilla_dir[k] = i;
    hookshot_effect_index |= kBoomerang_Tab3[i];
  }

  j = FindInByteArray(kBoomerang_Tab4, r1, 8);
  if (j < 0)
    j = 0;
  ancilla_arr1[k] = kBoomerang_Tab5[j];
  ancilla_arr23[k] = j << 1;
  if (button_b_frames >= 9) {
    ancilla_aux_timer[k]++;
  } else {
    if (s || !(joypad1H_last & 0xf))
      j = link_direction_facing >> 1;
  }
  s = Ancilla_CheckInitialTile_A(k);
  if (s < 0) {
    if (ancilla_aux_timer[k]) {
      Ancilla_SetXY(k, link_x_coord + kBoomerang_Tab9[j], link_y_coord + 8 + kBoomerang_Tab8[j]);
    } else {
      Ancilla_SetXY(k, link_x_coord + kBoomerang_Tab7[j], link_y_coord + 8 + kBoomerang_Tab6[j]);
    }
  } else {
    ancilla_type[k] = 0;
    flag_for_boomerang_in_place = 0;
    if (ancilla_tile_attr[k] != 0xf0) {
      sound_effect_1 = Ancilla_CalculateSfxPan(k) | 5;
    } else {
      sound_effect_1 = Ancilla_CalculateSfxPan(k) | 6;
    }
    AncillaAdd_BoomerangWallClink(k);
  }
  return s;
}

// Spawns a wish-pond thrown item (Ancilla28, type a). xin is the item index
// (into kReceiveItemGfx and kWishPondItem_X/Y); yin is the y-tile slot byte.
// Loads the item's GFX tile via DecodeAnimatedSpriteTile_variable (or
// DecompressShieldGraphics / DecompressSwordGraphics for special items).
// Sets Link into a frozen throw pose (state_bits = 0x80, facing up, animation
// reset). Initialises the parabolic arc: z_vel = 20 (upward), y_vel = −40,
// timer = 16 (hold frames). Position is set to link_x/y + per-item offsets
// from kWishPondItem_X/Y.
void AncillaAdd_TossedPondItem(uint8 a, uint8 xin, uint8 yin) {  // 898a32
  static const uint8 kWishPondItem_X[76] = {
    4, 4, 4, 4,  4, 0, 0, 4, 4, 4, 4, 4, 5, 0, 0, 0,
    0, 0, 0, 4,  0, 4, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 11, 0, 0, 0, 2, 0, 5, 0, 0, 0, 0, 0,
    0, 0, 0, 0,  4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 4, 4,  0, 4, 0, 0, 0, 4, 0, 0,
  };
  static const int8 kWishPondItem_Y[76] = {
    -13, -13, -13, -13, -13, -12, -12, -13, -13, -12, -12, -12, -10, -12, -12, -12,
    -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12,
    -12, -12, -12, -13, -12, -12, -12, -12, -12, -12, -10, -12, -12, -12, -12, -12,
    -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12, -12,
    -12, -12, -12, -12, -12, -12, -12, -12, -12, -13, -12, -12,
  };

  link_receiveitem_index = xin;
  int k = Ancilla_AddAncilla(a, yin);
  if (k >= 0) {
    sound_effect_2 = Link_CalculateSfxPan() | 0x13;
    uint8 sb = kReceiveItemGfx[xin];

    if (sb != 0xff) {
      if (sb == 0x20)
        DecompressShieldGraphics();
      DecodeAnimatedSpriteTile_variable(sb);
    } else {
      DecodeAnimatedSpriteTile_variable(0);
    }
    if (sb == 6)
      DecompressSwordGraphics();

    link_state_bits = 0x80;
    link_picking_throw_state = 0;
    link_direction_facing = 0;
    link_animation_steps = 0;
    ancilla_z_vel[k] = 20;
    ancilla_y_vel[k] = -40;
    ancilla_x_vel[k] = 0;
    ancilla_z[k] = 0;
    ancilla_timer[k] = 16;
    ancilla_item_to_link[k] = link_receiveitem_index;
    Ancilla_SetXY(k,
      link_x_coord + kWishPondItem_X[link_receiveitem_index],
      link_y_coord + kWishPondItem_Y[link_receiveitem_index]);
  }
}

// Spawns the happiness-pond rupee shower (Ancilla42, type 0x42). arg selects
// which subset of rupees to initialise from the preset tables:
//   kHappinessPond_Start[arg] / kHappinessPond_End[arg] define a range of the
//   10-entry velocity arrays to fill. Each entry receives independent X/Y/Z
//   velocities from kHappinessPond_Xvel/Yvel/Zvel so the rupees fan out on
//   different arcs. All entries start at link_x+4, link_y−12 with timer=16
//   and item_to_link = 53 (green rupee sprite). The happiness_pond_arr1 flags
//   are cleared before re-filling so any previous shower is discarded.
void AddHappinessPondRupees(uint8 arg) {  // 898ae0
  int k = Ancilla_AddAncilla(0x42, 9);
  if (k < 0)
    return;
  sound_effect_2 = Link_CalculateSfxPan() | 0x13;
  uint8 sb = kReceiveItemGfx[0x35];
  DecodeAnimatedSpriteTile_variable(sb);
  link_state_bits = 0x80;
  link_picking_throw_state = 0;
  link_direction_facing = 0;
  link_animation_steps = 0;

  memset(happiness_pond_arr1, 0, 10);

  static const int8 kHappinessPond_Start[4] = {0, 4, 4, 9};
  static const int8 kHappinessPond_End[4] = {-1, 0, -1, -1};
  static const int8 kHappinessPond_Xvel[10] = {0, -12, -6, 6, 12, -9, -5, 0, 5, 9};
  static const int8 kHappinessPond_Yvel[10] = {-40, -40, -40, -40, -40, -32, -32, -32, -32, -32};
  static const int8 kHappinessPond_Zvel[10] = {20, 20, 20, 20, 20, 16, 16, 16, 16, 16};

  int j = kHappinessPond_Start[arg], j_end = kHappinessPond_End[arg];
  k = 9;
  do {
    happiness_pond_arr1[k] = 1;
    happiness_pond_z_vel[k] = kHappinessPond_Zvel[j];
    happiness_pond_y_vel[k] = kHappinessPond_Yvel[j];
    happiness_pond_x_vel[k] = kHappinessPond_Xvel[j];
    happiness_pond_z[k] = 0;
    happiness_pond_step[k] = 0;
    happiness_pond_timer[k] = 16;
    happiness_pond_item_to_link[k] = 53;
    int x = link_x_coord + 4;
    int y = link_y_coord - 12;
    happiness_pond_x_lo[k] = x;
    happiness_pond_x_hi[k] = x >> 8;
    happiness_pond_y_lo[k] = y;
    happiness_pond_y_hi[k] = y >> 8;
  } while (--k, --j != j_end);
}

// Spawns a falling milestone item ancilla (Ancilla29, type a). item_idx
// indexes into the 7-entry prize tables (Pendant types, Crystal, etc.).
// kFallingItem_Type maps item_idx to the actual item ID stored in
// ancilla_item_to_link. kFallingItem_G provides the initial ancilla_G delay.
// Starts with z_vel = −48 (fast descent) and z = kFallingItem_Z[item_idx]
// (starting altitude above the floor). Loads GFX for Pendant (0x10) and Bow
// (0x0F) items via DecodeAnimatedSpriteTile_variable. Position is computed
// from kFallingItem_X/Y offset by BG2 copy2 scroll (screen-relative); for
// Turtle Rock boss room (palace 20) a page-aligned position is used instead.
// Returns the ancilla slot index.
int AncillaAdd_FallingPrize(uint8 a, uint8 item_idx, uint8 yv) {  // 898bc1
  static const int8 kFallingItem_Type[7] = {0x10, 0x37, 0x39, 0x38, 0x26, 0xf, 0x20};
  static const int8 kFallingItem_G[7] = {0x40, 0, 0, 0, 0, -1, 0};
  static const int16 kFallingItem_X[7] = {0x78, 0x78, 0x78, 0x78, 0x78, 0x80, 0x78};
  static const int16 kFallingItem_Y[7] = {0x48, 0x78, 0x78, 0x78, 0x78, 0x68, 0x78};
  static const uint8 kFallingItem_Z[7] = {0x60, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
  link_receiveitem_index = item_idx;
  int k = Ancilla_AddAncilla(a, yv);
  if (k < 0)
    return k;
  uint8 item_type = kFallingItem_Type[item_idx];
  ancilla_item_to_link[k] = item_type;
  if (item_type == 0x10 || item_type == 0xf)
    DecodeAnimatedSpriteTile_variable(kReceiveItemGfx[item_type]);

  ancilla_z_vel[k] = -48;
  ancilla_y_vel[k] = 0;
  ancilla_x_vel[k] = 0;
  ancilla_step[k] = 0;
  ancilla_z[k] = kFallingItem_Z[item_idx];
  ancilla_aux_timer[k] = 9;
  ancilla_arr3[k] = 0;
  ancilla_L[k] = 0;
  ancilla_G[k] = kFallingItem_G[item_idx];
  link_receiveitem_index = item_type;

  int x, y;
  if (item_idx != 0 && item_idx != 5) {
    if (BYTE(cur_palace_index_x2) == 20) {
      x = (link_x_coord & 0xff00) | 0x100;
      y = (link_y_coord & 0xff00) | 0x100;
    } else {
      x = kFallingItem_X[item_idx] + BG2HOFS_copy2;
      y = kFallingItem_Y[item_idx] + BG2VOFS_copy2;
    }
  } else {
    x = link_x_coord;
    y = kFallingItem_Y[item_idx] + BG2VOFS_copy2;
  }
  Ancilla_SetXY(k, x, y);
  return k;
}

// Spawns or recycles the full-charge spin-attack sparkle (type 13 =
// Ancilla0D_SpinAttackFullChargeSpark). Searches slots 9 down to 0 for a
// free slot (type 0) or a charge-sparkle sub-type (0x3C). Sets timer = 6 and
// the current floor. Only one full-charge sparkle exists at a time; the search
// stops at the first match.
void AncillaAdd_ChargedSpinAttackSparkle() {  // 898cb1
  for (int k = 9; k >= 0; k--) {
    if (ancilla_type[k] == 0 || ancilla_type[k] == 0x3c) {
      ancilla_type[k] = 13;
      ancilla_floor[k] = link_is_on_lower_level;
      ancilla_timer[k] = 6;
      break;
    }
  }
}

// Initialises the weathervane explosion ancilla (Ancilla37, type a) and all
// 12 debris piece state arrays. Triggers the explosion music (0xF2) and crowd
// SFX (0x17). weathervane_var1 = 0 (one-shot init flag), weathervane_var2 =
// 0x280 (alternating tick divisor). Each debris piece i is loaded with:
//   x_vel from kWeathervane_Tab4 (sideways scatter), z_vel from Tab5 (upward),
//   y_lo from Tab6 / y_hi = 7 (initial world Y), x_lo from Tab8 / x_hi = 2
//   (initial world X), z (altitude) from Tab10, arr11 (frame timer) = 1,
//   arr12 (draw frame) = i & 1. The ancilla slot itself holds only timing
//   counters; the per-piece state lives in the weathervane_arr* arrays.
void AncillaAdd_ExplodingWeatherVane(uint8 a, uint8 y) {  // 898d11
  static const int8 kWeathervane_Tab4[12] = {8, 10, 9, 4, 11, 12, -10, -8, 4, -6, -10, -4};
  static const int8 kWeathervane_Tab5[12] = {20, 22, 20, 20, 22, 20, 20, 22, 20, 22, 20, 20};
  static const uint8 kWeathervane_Tab6[12] = {0xb0, 0xa3, 0xa0, 0xa2, 0xa0, 0xa8, 0xa0, 0xa0, 0xa8, 0xa1, 0xb0, 0xa0};
  static const uint8 kWeathervane_Tab8[12] = {0, 2, 4, 6, 3, 8, 14, 8, 12, 7, 10, 8};
  static const uint8 kWeathervane_Tab10[12] = {48, 18, 32, 20, 22, 24, 32, 20, 24, 22, 20, 32};

  int k = Ancilla_AddAncilla(a, y);
  if (k < 0)
    return;

  ancilla_aux_timer[k] = 10;
  ancilla_G[k] = 128;
  ancilla_step[k] = 0;
  ancilla_arr3[k] = 0;
  sound_effect_1 = 0;
  music_control = 0xf2;
  sound_effect_ambient = 0x17;

  weathervane_var1 = 0;
  weathervane_var2 = 0x280;

  for (int i = 11; i >= 0; i--) {
    weathervane_arr3[i] = 0;
    weathervane_arr4[i] = kWeathervane_Tab4[i];
    weathervane_arr5[i] = kWeathervane_Tab5[i];
    weathervane_arr6[i] = kWeathervane_Tab6[i];
    weathervane_arr7[i] = 7;
    weathervane_arr8[i] = kWeathervane_Tab8[i];
    weathervane_arr9[i] = 2;
    weathervane_arr10[i] = kWeathervane_Tab10[i];
    weathervane_arr11[i] = 1;
    weathervane_arr12[i] = i & 1;
  }
}

// Spawns the intro cutscene duck (Ancilla38, type a). Checks
// AncillaAdd_CheckForPresence first — if a duck already exists nothing is
// done. Initialises the duck: direction 2 (facing left), wing-flap rate
// arr3 = 3, item_to_link = 116 (initial approach counter), aux_timer = 32
// (delay before first movement), step/L/z/S all 0. Starting position is
// (0x200, 0x788) — off the left edge of the overworld near the weathervane.
void AncillaAdd_CutsceneDuck(uint8 a, uint8 y) {  // 898d90
  if (AncillaAdd_CheckForPresence(a))
    return;
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_dir[k] = 2;
    ancilla_arr3[k] = 3;
    ancilla_step[k] = 0;
    ancilla_aux_timer[k] = 32;
    ancilla_item_to_link[k] = 116;
    ancilla_z_vel[k] = 0;
    ancilla_L[k] = 0;
    ancilla_z[k] = 0;
    ancilla_S[k] = 0;
    Ancilla_SetXY(k, 0x200, 0x788);
  }
}

// Converts the Somaria block slot k to the platform-poof type (0x39) with a
// 7-tick delay (aux_timer). Also kills any existing Somaria platform sprite
// (type 0xED) and clears player_on_somaria_platform so the new platform starts
// without a rider. Calls Player_TileDetectNearby to update collision state
// immediately after the block's tile changes.
void AncillaAdd_SomariaPlatformPoof(int k) {  // 898dd2
  ancilla_type[k] = 0x39;
  ancilla_aux_timer[k] = 7;
  for (int j = 15; j >= 0; j--) {
    if (sprite_type[j] == 0xed) {
      sprite_state[j] = 0;
      player_on_somaria_platform = 0;
    }
  }
  Player_TileDetectNearby();
}

// Spawns the super (Big) bomb explosion ancilla (Ancilla3A, type a). Position
// is taken from the tagalong companion slot referenced by WORD(tagalong_var2)
// — the Big Bomb is carried by a tagalong NPC, so the explosion originates at
// the NPC's position (+8 X, +16 Y). Starts at frame 1 with arr3 reloaded from
// kBomb_Tab0[1]. Returns the slot index or negative on failure.
int AncillaAdd_SuperBombExplosion(uint8 a, uint8 y) {  // 898df9
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_R[k] = 0;
    ancilla_step[k] = 0;
    ancilla_arr25[k] = 0;
    ancilla_L[k] = 0;
    ancilla_arr3[k] = kBomb_Tab0[1];
    ancilla_item_to_link[k] = 1;
    int j = WORD(tagalong_var2);
    int y = tagalong_y_lo[j] | tagalong_y_hi[j] << 8;
    int x = tagalong_x_lo[j] | tagalong_x_hi[j] << 8;
    Ancilla_SetXY(k, x + 8, y + 16);
  }
  return k;
}

// Configures the three hard-coded revival ancilla slots (0, 1, 2) used by
// RevivalFairy_Main/Dust/MonitorHP. Called once when the game-over death
// sequence starts. Sets link_dma_var5 = 80 to load the fairy GFX tile.
//
// Slot 0 (fairy): z_vel = 8 (descending), arr3 = 64 (steps before hover),
//   G = 5 (SFX period), positioned at Link's world coordinates, z = 0.
// Slot 1 (heart fill): arr3 = 240 (long countdown), z = 0, K = 0 (bob dir).
// Slot 2 (dust cloud): item_to_link = 2 (start on frame 2 of powder sequence),
//   aux_timer = 3, arr3 = 8, dir = 3, arr25 loaded from kMagicPowder_Tab0[32],
//   positioned at link_x + 20, link_y + 2 (above Link's right shoulder).
void ConfigureRevivalAncillae() {  // 898e4e
  link_dma_var5 = 80;
  int k = 0;

  ancilla_arr3[k] = 64;
  ancilla_step[k] = 0;
  ancilla_z_vel[k] = 8;
  ancilla_L[k] = 0;
  ancilla_G[k] = 5;
  ancilla_item_to_link[k] = 0;
  ancilla_K[k] = 0;
  Ancilla_SetXY(k, link_x_coord, link_y_coord);
  ancilla_z[k] = 0;
  k += 1;

  ancilla_z[k] = 0;
  ancilla_arr3[k] = 240;
  ancilla_step[k] = 0;
  ancilla_K[k] = 0;
  k += 1;

  ancilla_item_to_link[k] = 2;
  ancilla_aux_timer[k] = 3;
  ancilla_arr3[k] = 8;
  ancilla_step[k] = 0;
  ancilla_dir[k] = 3;
  ancilla_arr25[k] = kMagicPowder_Tab0[30 + ancilla_item_to_link[k]];

  Ancilla_SetXY(k, link_x_coord + 20, link_y_coord + 2);
}

// Spawns a lamp flame ancilla (Ancilla2F, type a). Sets timer = 23 (lifetime),
// direction from link_direction_facing >> 1, and position from kLampFlame_X/Y
// (direction-specific offset ahead of Link). Plays SFX 42 (fire ignition)
// panned to the ancilla's position.
void AncillaAdd_LampFlame(uint8 a, uint8 y) {  // 898f1c
  static const int8 kLampFlame_X[4] = {0, 0, -20, 18};
  static const int8 kLampFlame_Y[4] = {-16, 24, 4, 4};
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 0;
    ancilla_timer[k] = 23;
    int j = link_direction_facing >> 1;
    ancilla_dir[k] = j;
    Ancilla_SetXY(k, link_x_coord + kLampFlame_X[j], link_y_coord + kLampFlame_Y[j]);
    sound_effect_1 = Ancilla_CalculateSfxPan(k) | 42;
  }
}

// Spawns a Master Sword ceremony ancilla (Ancilla35_MasterSwordReceipt, type a)
// positioned 8 px right and 8 px above Link's current coordinates. Sets
// timer = 64 (display duration) and aux_timer = 2 (animation sub-timer seed).
void AncillaAdd_MSCutscene(uint8 a, uint8 y) {  // 898f7c
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 2;
    ancilla_timer[k] = 64;
    Ancilla_SetXY(k, link_x_coord + 8, link_y_coord - 8);
  }
}

// Spawns the dash dust effect (Ancilla1E, type a) for a full-speed pegasus dash.
// step parameter = 1 (normal dash puff variant). Delegates to AddDashingDustEx.
void AncillaAdd_DashDust(uint8 a, uint8 y) {  // 898fba
  AddDashingDustEx(a, y, 1);
}

// Spawns the dash dust for the charge-up phase (before reaching full sprint).
// step parameter = 0 (charging dust variant). Delegates to AddDashingDustEx.
void AncillaAdd_DashDust_charging(uint8 a, uint8 y) {  // 898fc1
  AddDashingDustEx(a, y, 0);
}

// Spawns a blast-wall fireball (type 0x32) at the position of blast-wall pillar
// r4 (blastwall_var10/11[r4]). Searches slots 10 down to 5 for a free slot.
// kBlastWall_XY[32] is an 8-entry table of (Y, X) velocity pairs at 16
// radial angles; frame_counter & 15 selects a random direction each spawn,
// giving the fireballs a different outward angle every frame. blastwall_var12[k]
// = 16 is the fireball lifetime in ticks.
void AncillaAdd_BlastWallFireball(uint8 a, uint8 y, int r4) {  // 899031
  static const int8 kBlastWall_XY[32] = {
    -64, 0, -22,  42, -38,  38, -42,  22, 0,  64,  22,  42,  38,  38,  42,  22,
    64, 0,  22, -42,  38, -38,  42, -22, 0, -64, -22, -42, -38, -38, -42, -22,
  };
  for (int k = 10; k != 4; k--) {
    if (ancilla_type[k] == 0) {
      ancilla_type[k] = 0x32;
      ancilla_floor[k] = link_is_on_lower_level;
      blastwall_var12[k] = 16;
      int j = frame_counter & 15;
      ancilla_y_vel[k] = kBlastWall_XY[j * 2 + 0];
      ancilla_x_vel[k] = kBlastWall_XY[j * 2 + 1];
      Ancilla_SetXY(k, blastwall_var11[r4] + 16, blastwall_var10[r4] + 8);
      return;
    }
  }

}

// Spawns an arrow ancilla (Ancilla09, type a). ax encodes the direction
// (link_direction_facing-style byte); ay is the y-slot byte. xcoord/ycoord
// are Link's world coordinates. Calls AncillaAdd_CheckForPresence to prevent
// two arrows from being active simultaneously.
//
// Slot selection is via AncillaAdd_ArrowFindSlot. On success:
//   - SFX 0x07 (arrow swoosh) is played.
//   - ancilla_dir[k] = (ax >> 1) | 4: lower bits are the direction index
//     (0=up, 1=down, 2=left, 3=right); bit 2 marks this as an arrow.
//   - velocity: kShootBow_Xvel/Yvel[j] at 48 px/frame.
//   - position: xcoord + kShootBow_X/Y[j] + 8 Y.
// Returns the slot index, or −1 on failure.
int AncillaAdd_Arrow(uint8 a, uint8 ax, uint8 ay, uint16 xcoord, uint16 ycoord) {  // 8990a4
  static const int8 kShootBow_X[4] = {4, 4, 0, 4};
  static const int8 kShootBow_Y[4] = {-4, 3, 4, 4};
  static const int8 kShootBow_Xvel[4] = {0, 0, -48, 48};
  static const int8 kShootBow_Yvel[4] = {-48, 48, 0, 0};

  scratch_0 = ycoord;
  scratch_1 = xcoord;
  BYTE(index_of_interacting_tile) = ax;

  if (AncillaAdd_CheckForPresence(a))
    return -1;

  int k = AncillaAdd_ArrowFindSlot(a, ay);

  if (k >= 0) {
    sound_effect_1 = Link_CalculateSfxPan() | 7;
    ancilla_H[k] = 0;
    ancilla_item_to_link[k] = 8;
    int j = ax >> 1;
    ancilla_dir[k] = j | 4;
    ancilla_y_vel[k] = kShootBow_Yvel[j];
    ancilla_x_vel[k] = kShootBow_Xvel[j];
    Ancilla_SetXY(k, xcoord + kShootBow_X[j], ycoord + 8 +  kShootBow_Y[j]);
  }
  return k;
}

// Spawns the bunny transformation poof (Ancilla23, type a). Makes Link
// invisible (visibility_status = 0xC). Plays the appropriate SFX: 0x14
// (transform) or 0x15 (de-transform) based on link_is_bunny_mirror.
// Position is link_x/y + 4 Y. step = 0 (transform-to-bunny path).
void AncillaAdd_BunnyPoof(uint8 a, uint8 y) {  // 899102
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    link_visibility_status = 0xc;
    ancilla_step[k] = 0;
    if (!link_is_bunny_mirror)
      sound_effect_1 = Link_CalculateSfxPan() | 0x14;
    else
      sound_effect_1 = Link_CalculateSfxPan() | 0x15;

    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 7;
    Ancilla_SetXY(k, link_x_coord, link_y_coord + 4);
  }
}

// Spawns the Magic Cape invisibility poof (Ancilla23, type a). Sets step = 1
// (cape variant, suppresses palette reload). Freezes Link's direction
// (link_is_transforming = 1, cant_change_direction |= 1, direction = 0) so
// the poof animation plays without Link rotating. Position is link_x/y + 4 Y.
void AncillaAdd_CapePoof(uint8 a, uint8 y) {  // 89912c
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_step[k] = 1;
    link_is_transforming = 1;
    link_cant_change_direction |= 1;
    link_direction = 0;
    link_direction_last = 0;
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 7;
    Ancilla_SetXY(k, link_x_coord, link_y_coord + 4);
  }
}

// Spawns the dwarf companion transformation poof (Ancilla40, type ain).
// Plays SFX 0x14 or 0x15 based on follower_indicator (8 = good smith, other
// = tempered). tagalong_var5 = 1 freezes the dwarf follower.
// Position is taken from tagalong_x/y_lo/hi[tagalong_var2] + 4 Y (the dwarf's
// current world position).
void AncillaAdd_DwarfPoof(uint8 ain, uint8 yin) {  // 89915f
  int k = Ancilla_AddAncilla(ain, yin);
  if (k < 0)
    return;
  if (follower_indicator == 8)
    sound_effect_1 = Link_CalculateSfxPan() | 0x14;
  else
    sound_effect_1 = Link_CalculateSfxPan() | 0x15;

  ancilla_item_to_link[k] = 0;
  ancilla_step[k] = 0;
  ancilla_aux_timer[k] = 7;
  tagalong_var5 = 1;
  int j = tagalong_var2;
  int x = tagalong_x_lo[j] | tagalong_x_hi[j] << 8;
  int y = tagalong_y_lo[j] | tagalong_y_hi[j] << 8;
  Ancilla_SetXY(k, x, y + 4);
}

// Spawns a bush-destruction poof (Ancilla3F, type 0x3F) at world position
// (x, y − 2). Only fires if Link is currently holding an item aloft
// (link_item_in_hand & 0x40 = holding-up flag). Plays SFX 21 (bush cut).
void AncillaAdd_BushPoof(uint16 x, uint16 y) {  // 8991c3
  if (!(link_item_in_hand & 0x40))
    return;
  int k = Ancilla_AddAncilla(0x3f, 4);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_timer[k] = 7;
    sound_effect_1 = Link_CalculateSfxPan() | 21;
    Ancilla_SetXY(k, x, y - 2);
  }
}

// Spawns the Ether Medallion spell ancilla (Ancilla18, type a). Sets
// flag_custom_spell_anim_active = 1 to disable other player inputs during the
// spell. Initialises y_vel = 127 (the lightning bolt's starting Y position),
// ether_var2 = 40 (lighting bolt descent target), step = 0, arr3 = 3,
// aux_timer = 2, and clears item_to_link / arr25.
void AncillaAdd_EtherSpell(uint8 a, uint8 y) {  // 8991fc
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_arr25[k] = 0;
    ancilla_step[k] = 0;
    flag_custom_spell_anim_active = 1;
    ancilla_aux_timer[k] = 2;
    ancilla_arr3[k] = 3;
    ancilla_y_vel[k] = 127;
    ether_var2 = 40;
    load_chr_halfslot_even_odd = 9;
    ether_var1 = 0x40;
    sound_effect_2 = Link_CalculateSfxPan() | 0x26;
    for(int i = 0; i < 8; i++)
      ether_arr1[i] = i * 8;
    ether_y = link_y_coord;
    uint16 y = BG2VOFS_copy2 - 16;
    ether_y_adjusted = y & 0xf0;
    ether_x = link_x_coord;
    ether_x2 = ether_x + 8;
    ether_y2 = link_y_coord - 16;
    ether_y3 = ether_y2 + 0x24;
    Ancilla_SetXY(k, link_x_coord, y);
  }
}

// Attempts to spawn the victory-spin ancilla (type 0x3B, slot limit 0).
// Only spawns when Link holds a real sword: the guard `(link_sword_type + 1 & 0xfe) != 0`
// is false only when link_sword_type == 0xFF (no sword). Initialises
// item_to_link = 0 (frame counter), arr3 = 1 (phase), aux_timer = 34 (spin duration).
void AncillaAdd_VictorySpin() {  // 8992ac
  if ((link_sword_type + 1 & 0xfe) != 0) {
    int k = Ancilla_AddAncilla(0x3b, 0);
    if (k >= 0) {
      ancilla_item_to_link[k] = 0;
      ancilla_arr3[k] = 1;
      ancilla_aux_timer[k] = 34;
    }
  }
}

// Spawns a magic powder cloud ancilla at Link's position, offset by direction.
// a = ancilla type, y = slot limit. Two offset tables (kMagicPower_X/Y vs
// kMagicPower_X1/Y1) give an initial probe position and a final placement
// position. Ancilla_CheckTileCollision probes for terrain; the result is saved
// in byte_7E0333 for the powder handler's tile-interaction logic (lighting
// torches, converting enemies). If current_item_active == 9 (shovel or an
// incompatible item), the slot is immediately cleared. Decrements link_dma_var5
// to 80 to load the powder CHR.
// link_direction_facing >> 1 collapses the 8-direction value to 0-3 (Up/Down/Left/Right).
void AncillaAdd_MagicPowder(uint8 a, uint8 y) {  // 8992f0
  static const int8 kMagicPower_X[4] = {-2, -2, -12, 12};
  static const int8 kMagicPower_Y[4] = {0, 20, 16, 16};
  static const int8 kMagicPower_X1[4] = {10, 10, -8, 28};
  static const int8 kMagicPower_Y1[4] = {1, 40, 22, 22};

  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_z[k] = 0;
    ancilla_aux_timer[k] = 1;
    link_dma_var5 = 80;
    int j = link_direction_facing >> 1;
    ancilla_dir[k] = j;
    ancilla_arr25[k] = kMagicPowder_Tab0[j * 10];
    Ancilla_SetXY(k, link_x_coord + kMagicPower_X[j], link_y_coord + kMagicPower_Y[j]);
    Ancilla_CheckTileCollision(k);
    byte_7E0333 = ancilla_tile_attr[k];
    if (current_item_active == 9) {
      ancilla_type[k] = 0;
      return;
    }
    sound_effect_1 = Link_CalculateSfxPan() | 0xd;
    Ancilla_SetXY(k, link_x_coord + kMagicPower_X1[j], link_y_coord + kMagicPower_Y1[j]);
  }
}

// Spawns a wall-tap spark ancilla (the "clink" flash when an item hits a wall).
// a = ancilla type, y = slot limit. Positions the spark at Link's location plus
// direction-specific offsets from kWallTapSpark_X/Y. Initialises
// item_to_link = 5 (frame count for the 5-frame spark animation) and
// aux_timer = 1 (ticks between frames).
void AncillaAdd_WallTapSpark(uint8 a, uint8 y) {  // 899395
  static const int8 kWallTapSpark_X[4] = {11, 10, -12, 29};
  static const int8 kWallTapSpark_Y[4] = {-4, 32, 17, 17};
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 5;
    ancilla_aux_timer[k] = 1;
    int i = link_direction_facing >> 1;
    Ancilla_SetXY(k, link_x_coord + kWallTapSpark_X[i], link_y_coord + kWallTapSpark_Y[i]);
  }
}

// Spawns a sword-swing sparkle ancilla at Link's current tile.
// a = ancilla type (varies by sword level and swing type),
// y = slot limit. Sets item_to_link = 0 (frame index), aux_timer = 1
// (per-frame countdown), and records link_direction_facing >> 1 in
// ancilla_dir to drive the sparkle's orientation.
void AncillaAdd_SwordSwingSparkle(uint8 a, uint8 y) {  // 8993c2
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 1;
    ancilla_dir[k] = link_direction_facing >> 1;
    Ancilla_SetXY(k, link_x_coord, link_y_coord);
  }
}

// Spawns the Pegasus Boots dash-tremor ancilla (screen shake indicator).
// Guards with AncillaAdd_CheckForPresence so only one tremor runs at a time.
// Determines the shake axis from kAddDashTremor_Dir: horizontal dash (dir 2/3)
// uses axis 0 (vertical scroll range); vertical dash (dir 0/1) uses axis 2
// (horizontal scroll range). Chooses Y velocity +3 or -3 depending on whether
// Link's screen-relative coordinate is below (kAddDashTremor_Tab[j>>1]) or
// above the midpoint — the tremor always pushes toward the open side of the
// playfield to prevent the shake from immediately going off-screen.
// item_to_link = 16 (16 ticks of shaking); L[k] = 0 (sub-phase counter).
void AncillaAdd_DashTremor(uint8 a, uint8 y) {  // 8993f3
  static const uint8 kAddDashTremor_Dir[4] = {2, 2, 0, 0};
  static const uint8 kAddDashTremor_Tab[2] = {0x80, 0x78};
  if (AncillaAdd_CheckForPresence(a))
    return;
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 16;
    ancilla_L[k] = 0;
    int j = link_direction_facing >> 1;
    ancilla_dir[k] = j = kAddDashTremor_Dir[j];
    uint8 y = link_y_coord - BG2VOFS_copy2;
    uint8 x = link_x_coord - BG2HOFS_copy2;
    Ancilla_SetY(k, (j ? y : x) < kAddDashTremor_Tab[j >> 1] ? 3 : -3);
  }
}

// Spawns a wall-hit clink spark at the boomerang's current world position.
// k = boomerang ancilla slot. Saves the boomerang's X/Y into the boomerang_temp_*
// globals before calling Ancilla_AddAncilla(6, 1) — type 6 is the generic clink
// spark, limit 1 (one concurrent boomerang bounce allowed). Uses
// hookshot_effect_index to pick the spark's sub-image from kBoomerangWallHit_Tab0,
// then applies direction offsets from kBoomerangWallHit_X/Y so the spark appears
// at the correct contact corner.
void AncillaAdd_BoomerangWallClink(int k) {  // 899478
  static const int8 kBoomerangWallHit_X[8] = {8, 8, 0, 10, 12, 8, 4, 0};
  static const int8 kBoomerangWallHit_Y[8] = {0, 8, 8, 8, 4, 8, 12, 8};
  static const uint8 kBoomerangWallHit_Tab0[16] = {0, 6, 4, 0, 2, 10, 12, 0, 0, 8, 14, 0, 0, 0, 0, 0};
  boomerang_temp_x = Ancilla_GetX(k);
  boomerang_temp_y = Ancilla_GetY(k);
  k = Ancilla_AddAncilla(6, 1);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_arr3[k] = 1;
    int j = kBoomerangWallHit_Tab0[hookshot_effect_index] >> 1;
    Ancilla_SetXY(k, boomerang_temp_x + kBoomerangWallHit_X[j], boomerang_temp_y + kBoomerangWallHit_Y[j]);
  }
}

// Spawns a wall-hit clink spark at the hookshot head's current world position.
// kin = hookshot ancilla slot (provides position and direction).
// a = spark ancilla type, y = slot limit. Uses ancilla_dir[kin] to select
// offsets from kHookshotWallHit_X/Y so the spark appears at the contact edge.
// Simpler than the boomerang variant because the hookshot head stores its own
// direction in ancilla_dir rather than using the global hookshot_effect_index.
void AncillaAdd_HookshotWallClink(int kin, uint8 a, uint8 y) {  // 8994c6
  static const int8 kHookshotWallHit_X[8] = {8, 8, 0, 10, 12, 8, 4, 0};
  static const int8 kHookshotWallHit_Y[8] = {0, 8, 8, 8, 4, 8, 12, 8};
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_arr3[k] = 1;
    int j = ancilla_dir[kin];
    Ancilla_SetXY(k, Ancilla_GetX(kin) + kHookshotWallHit_X[j], Ancilla_GetY(kin) + kHookshotWallHit_Y[j]);
  }
}

// Spawns the flute bird (duck) takeoff ancilla (type a, limit y).
// Guards with AncillaAdd_CheckForPresence to prevent duplicate spawns.
// Initialises the bird's Z physics: timer = 0x78 (120-tick ride timer),
// L = 0 (phase flag: 0 = takeoff, 1 = active ride), z_vel = 0 and z = 0
// so the bird starts at ground level. Falls through to AddBirdCommon(k) to
// place the bird near Link and configure shared direction / OAM fields.
void AncillaAdd_Duck_take_off(uint8 a, uint8 y) {  // 8994fe
  if (AncillaAdd_CheckForPresence(a))
    return;
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_timer[k] = 0x78;
    ancilla_L[k] = 0;
    ancilla_z_vel[k] = 0;
    ancilla_z[k] = 0;
    ancilla_step[k] = 0;
    AddBirdCommon(k);
  }
}

// Spawns the flute bird during its active travel arc (Link is riding).
// Guards with AncillaAdd_CheckForPresence. On success:
//   - Resets link_player_handler_state = 0 and link_speed_setting = 0 to
//     suppress normal movement input during the ride.
//   - Clears the B/Y button mask bits (0x81) and link_delay_timer_spin_attack
//     so pressing those buttons during flight has no effect.
//   - Sets L[k] = 1 to tell AddBirdCommon / the bird handler that this is
//     an active travel phase (vs. the initial takeoff).
//   - Z physics: under ExtendScreen64, the bird arcs higher (z_vel = 58,
//     z = -105 = "already 105 px above ground") to keep Link visible on the
//     taller viewport; otherwise z_vel = 40 and z = -51.
//   - step = 2 skips the takeoff windup animation in the bird handler.
// Calls AddBirdCommon(k) to finalise position, direction, and OAM fields.
void AddBirdTravelSomething(uint8 a, uint8 y) {  // 89951d
  if (AncillaAdd_CheckForPresence(a))
    return;
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    link_player_handler_state = 0;
    link_speed_setting = 0;
    button_mask_b_y &= ~0x81;
    button_b_frames = 0;
    link_delay_timer_spin_attack = 0;
    link_cant_change_direction &= ~1;
    ancilla_L[k] = 1;

    if (enhanced_features0 & kFeatures0_ExtendScreen64) {
      // todo: tune these better so the angle of attack is better
      ancilla_z_vel[k] = 58;
      ancilla_z[k] = -105;
    } else {
      ancilla_z_vel[k] = 40;
      ancilla_z[k] = -51;
    }
    ancilla_step[k] = 2;
    AddBirdCommon(k);
  }
}

// Spawns the Quake Medallion spell ancilla (type a, limit y).
// Initialises the quake subsystem:
//   quake_arr2[0..4] = 0  (per-line X-shake offsets, cleared before the spell starts).
//   quake_arr1[0..4] = 1  (active flags for each of the 5 quake lines).
//   quake_var5 = 0        (frame counter).
//   quake_var1 / var2 / var3  — spell origin world coordinates derived from Link's position.
// Sets flag_custom_spell_anim_active = 1 to freeze normal player input.
// load_chr_halfslot_even_odd = 13 requests the CHR half-slot for the quake
// crack tiles. Plays sound 0x35 (earthquake rumble).
void AncillaAdd_QuakeSpell(uint8 a, uint8 y) {  // 899589
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_step[k] = 0;
    ancilla_item_to_link[k] = 0;
    load_chr_halfslot_even_odd = 13;
    sound_effect_1 = 0x35;
    for(int i = 0; i < 5; i++)
      quake_arr2[i] = 0;
    quake_var5 = 0;
    for(int i = 0; i < 5; i++)
      quake_arr1[i] = 1;
    flag_custom_spell_anim_active = 1;
    ancilla_timer[k] = 2;
    quake_var1 = link_y_coord + 26;
    quake_var2 = link_x_coord + 8;
    quake_var3 = 3;
  }
}

// Spawns the spin-attack initial sparkle (the 4-point star that appears when the
// spin charge completes), and simultaneously clears any active Byrna sparks
// (type 0x31) so they don't collide-test against the sword during the spin.
// a = ancilla type (0x2A for the first spark),
// x = initial ancilla_step value (encodes the spark's angular phase 0-3),
// y = slot limit.
// timer = 4, aux_timer = 3 drive the per-frame countdown for the sparkle animation.
// Position is derived from Link plus direction offsets in kSpinAttackStartSparkle_X/Y.
void AncillaAdd_SpinAttackInitSpark(uint8 a, uint8 x, uint8 y) {  // 89960b
  static const int8 kSpinAttackStartSparkle_Y[4] = {32, -8, 10, 20};
  static const int8 kSpinAttackStartSparkle_X[4] = {10, 7, 28, -10};

  int k = Ancilla_AddAncilla(a, y);
  for (int i = 4; i >= 0; i--) {
    if (ancilla_type[i] == 0x31)
      ancilla_type[i] = 0;
  }
  ancilla_item_to_link[k] = 0;
  ancilla_step[k] = x;
  ancilla_timer[k] = 4;
  ancilla_aux_timer[k] = 3;
  int j = link_direction_facing >> 1;
  Ancilla_SetXY(k,
      link_x_coord + kSpinAttackStartSparkle_X[j],
      link_y_coord + kSpinAttackStartSparkle_Y[j]);
}

// Spawns the Bombos Medallion blast-wall effect by directly writing to ancilla
// slots 0 and 1 (type 0x33 = BlastWall), clearing slots 2-5.
// Does not use Ancilla_AddAncilla — this effect always occupies fixed slots.
// Initialises the blastwall_* globals:
//   blastwall_var5[0] = 1 / var6[0] = 3 — phase and sub-timer for slot 0.
//   blastwall_var8/9 updated by kBlastWall_Tab3/4 using the current facing
//     direction (blastwall_var7), advancing the blast-wall's world origin.
//   blastwall_var10/11[0..3] — per-wall endpoint coordinates computed from
//     the origin plus kBlastWall_Tab5 offsets (4 wall segments at 90° steps).
// Also sets flag_custom_spell_anim_active = 1, clears link_state_bits and
// link_cant_change_direction (Link is frozen during the spell), and plays a
// panned sound (kBombos_Sfx) for each wall segment that falls within the
// 256-px visible screen range.
void AncillaAdd_BlastWall() {  // 899692
  static const int8 kBlastWall_Tab3[4] = {-16, 16, 0, 0};
  static const int8 kBlastWall_Tab4[4] = {0, 0, -16, 16};
  static const int8 kBlastWall_Tab5[16] = {-8, 0, -8, 16, 16, 0, 16, 16, 0, -8, 16, -8, 0, 16, 16, 16};

  ancilla_type[0] = 0x33;
  ancilla_type[1] = 0x33;
  ancilla_type[2] = 0;
  ancilla_type[3] = 0;
  ancilla_type[4] = 0;
  ancilla_type[5] = 0;

  ancilla_item_to_link[0] = 0;
  flag_is_ancilla_to_pick_up = 0;
  link_state_bits = 0;
  link_cant_change_direction = 0;
  ancilla_K[0] = 0;
  ancilla_floor[0] = link_is_on_lower_level;
  ancilla_floor[1] = link_is_on_lower_level;
  ancilla_floor2[0] = link_is_on_lower_level_mirror;
  blastwall_var1 = 0;
  blastwall_var6[1] = 0;
  blastwall_var5[1] = 0;
  blastwall_var4 = 0;
  blastwall_var5[0] = 1;
  flag_custom_spell_anim_active = 1;
  blastwall_var6[0] = 3;
  int j = blastwall_var7;
  blastwall_var8 += kBlastWall_Tab3[j];
  blastwall_var9 += kBlastWall_Tab4[j];
  j = (j < 4) ? 4 : 0;
  for (int k = 3; k >= 0; k--, j++) {
    blastwall_var10[k] = blastwall_var8 + kBlastWall_Tab5[j * 2 + 0];
    blastwall_var11[k] = blastwall_var9 + kBlastWall_Tab5[j * 2 + 1];
    uint16 x = blastwall_var11[k] - BG2HOFS_copy2;
    if (x < 256)
      sound_effect_1 = kBombos_Sfx[x >> 5] | 0xc;
  }
  // In dark world forest castle hole outside door

}

// Spawns one sword-charge sparkle (type 0x3C) in a high ancilla slot (6-9),
// positioned near the ancilla k that is currently being charged (e.g., a
// carried Somaria block or Link's Z height when jumping).
// Uses Ancilla_AllocHigh for the dedicated sparkle slots to avoid evicting
// weapons. Position is randomised using GetRandomNumber() — the high nibble
// offsets X and the low nibble offsets Y — and the ancilla's z height is
// subtracted so sparkles appear to hover above the sprite even when it is
// airborne. timer = 4 drives the 4-frame sparkle animation.
// k (the source ancilla) is passed in; j (the newly-allocated slot) is the
// sparkle itself.
void AncillaAdd_SwordChargeSparkle(int k) {  // 899757
  int j;
  for (j = 9; ancilla_type[j] != 0; ) {
    if (--j < 0)
      return;
  }
  ancilla_type[j] = 60;
  ancilla_floor[j] = link_is_on_lower_level;
  ancilla_item_to_link[j] = 0;
  ancilla_timer[j] = 4;

  uint8 rand = GetRandomNumber();

  uint8 z = ancilla_z[k];
  if (z >= 0xF8)
    z = 0;
  Ancilla_SetXY(j, Ancilla_GetX(k) + 2 + (rand >> 5), Ancilla_GetY(k) - 2 - z + (rand & 0xf));
}

// Spawns one silver arrow sparkle (type 0x3C) trailing the silver arrow in flight.
// kin = silver arrow ancilla slot (provides direction and position).
// Uses Ancilla_AllocHigh to place the sparkle in slots 6-9 so it does not
// displace weapons. Position is the arrow's world coordinate plus direction-
// specific offsets from kSilverArrowSparkle_X/Y, then randomised by
// GetRandomNumber() (high nibble → X jitter 0-7 px, low nibble → Y jitter 0-7 px).
// timer = 4 drives the 4-frame glitter animation.
void AncillaAdd_SilverArrowSparkle(int kin) {  // 8997de
  static const int8 kSilverArrowSparkle_X[4] = {-4, -4, 0, 2};
  static const int8 kSilverArrowSparkle_Y[4] = {0, 2, -4, -4};
  int k = Ancilla_AllocHigh();
  if (k >= 0) {
    ancilla_type[k] = 0x3c;
    ancilla_item_to_link[k] = 0;
    ancilla_timer[k] = 4;
    ancilla_floor[k] = link_is_on_lower_level;
    int m = GetRandomNumber();
    int j = ancilla_dir[kin] & 3;
    Ancilla_SetXY(k,
      Ancilla_GetX(kin) + kSilverArrowSparkle_X[j] + (m >> 4 & 7),
      Ancilla_GetY(kin) + kSilverArrowSparkle_Y[j] + (m & 7));
  }
}

// Spawns an ice rod projectile (type a) with directional velocity.
// a = ancilla type (0x11 is the ice-rod splash; this function starts as a
//   different type that becomes 0x11 when it hits a wall immediately).
// y = slot limit.
// On allocation failure, Refund_Magic(0) returns the magic cost.
// Initialises: step=0, arr25=0, item_to_link=255 (no item pickup link),
// L=1 (??active), aux_timer=3, arr3=6 (tile-collision class 6).
// Direction velocity: kIceRod_Yvel/Xvel map dir 0-3 to ±48 pixel/frame in
// the appropriate axis. kIceRod_X/Y offset spawn position from Link's tile.
// Ancilla_CheckInitialTile_A probes 3 tiles in front of Link:
//   - If collision-free (return ≥ 0): places the shot at the offset position
//     and plays SFX 15 (ice shot sound).
//   - If collision on first probe (return < 0): the shot is already inside a
//     wall — immediately transmute to type 0x11 (splash effect), set aux_timer=4.
// Boundary guard: if the computed position maps outside the 256×256 screen
// window (either component OR'd has a high byte set), the shot is cancelled.
void AncillaAdd_IceRodShot(uint8 a, uint8 y) {  // 899863
  static const int8 kIceRod_X[4] = {0, 0, -20, 20};
  static const int8 kIceRod_Y[4] = {-16, 24, 8, 8};
  static const int8 kIceRod_Xvel[4] = {0, 0, -48, 48};
  static const int8 kIceRod_Yvel[4] = {-48, 48, 0, 0};

  int k = Ancilla_AddAncilla(a, y);
  if (k < 0) {
    Refund_Magic(0);
    return;
  }
  sound_effect_1 = Link_CalculateSfxPan() | 15;
  ancilla_step[k] = 0;
  ancilla_arr25[k] = 0;
  ancilla_item_to_link[k] = 255;
  ancilla_L[k] = 1;
  ancilla_aux_timer[k] = 3;
  ancilla_arr3[k] = 6;
  int j = link_direction_facing >> 1;
  ancilla_dir[k] = j;
  ancilla_y_vel[k] = kIceRod_Yvel[j];
  ancilla_x_vel[k] = kIceRod_Xvel[j];

  if (Ancilla_CheckInitialTile_A(k) < 0) {
    uint16 x = link_x_coord + kIceRod_X[j];
    uint16 y = link_y_coord + kIceRod_Y[j];

    if (((x - BG2HOFS_copy2) | (y - BG2VOFS_copy2)) & 0xff00) {
      ancilla_type[k] = 0;
      return;
    }
    Ancilla_SetXY(k, x, y);
  } else {
    ancilla_type[k] = 0x11;
    ancilla_numspr[k] = kAncilla_Pflags[0x11];
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 4;
  }
}

// Spawns a water-splash ancilla (type a, limit y) at Link's entry point into
// a water surface. Returns true if no slot was available (caller can use
// this to decide whether to refund a resource or skip SFX).
// Plays SFX 0x24 (splash) panned to Link's screen X.
// If indoors and not in deep water, clears link_is_on_lower_level so the
// splash renders on the correct priority layer.
// Positions the splash at (link_x - 11, link_y + 8) to centre it on Link's
// feet relative to the water surface.
bool AncillaAdd_Splash(uint8 a, uint8 y) {  // 8998fc
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    sound_effect_1 = Link_CalculateSfxPan() | 0x24;
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 2;
    if (player_is_indoors && !link_is_in_deep_water)
      link_is_on_lower_level = 0;
    Ancilla_SetXY(k, link_x_coord - 11, link_y_coord + 8);
  }
  return k < 0;
}

// Spawns the gravestone-push ancilla (type ain, limit yin) for the overworld
// graveyard secret passages. Implements the following logic:
// 1. Snap Link's Y to the nearest 16-px grid row and look it up in
//    kMoveGravestone_Y (8 hard-coded row world-Y values). If not found,
//    cancel (the wrong row was entered).
// 2. Scan kMoveGravestone_X for a gravestone whose X range covers Link's
//    X coordinate. kMoveGravestone_Idx[i..i+1] bounds the search to the
//    gravestones on that row.
// 3. Special-case gravestone j == 13: requires link_is_running; all others
//    require NOT running (walk into them).
// 4. On match: write the gravestone's tile address into big_rock_starting_address
//    and door_open_closed_counter (animation length). Two special lengths
//    (0x58 = tombstone 14, 0x38 = tombstone 13) also mark the overworld event
//    bit (save_ow_event_info[screen] |= 0x20) and play SFX 0x1B (stone grind).
//    Gravestone tile map coordinates are packed into door_debris_y/x[k].
//    Calls Overworld_DoMapUpdate32x32_B to commit the tile-map change.
//    Sets bitmask_of_dragstate = 4 and link_something_with_hookshot = 1 to
//    trigger the "being dragged" movement mode while the stone slides.
//    Positions the ancilla at (X, Y-2) using kMoveGravestone_Y1/X1, and saves
//    the target Y in ancilla_A/B[k] for the push animation.
void AncillaAdd_GraveStone(uint8 ain, uint8 yin) {  // 8999e9
  static const uint16 kMoveGravestone_Y[8] = {0x550, 0x540, 0x530, 0x520, 0x500, 0x4e0, 0x4c0, 0x4b0};
  static const uint16 kMoveGravestone_X[15] = {0x8b0, 0x8f0, 0x910, 0x950, 0x970, 0x9a0, 0x850, 0x870, 0x8b0, 0x8f0, 0x920, 0x950, 0x880, 0x990, 0x840};
  static const uint16 kMoveGravestone_Y1[15] = {0x540, 0x530, 0x530, 0x530, 0x520, 0x520, 0x510, 0x510, 0x4f0, 0x4f0, 0x4f0, 0x4f0, 0x4d0, 0x4b0, 0x4a0};
  static const uint16 kMoveGravestone_X1[15] = {0x8b0, 0x8f0, 0x910, 0x950, 0x970, 0x9a0, 0x850, 0x870, 0x8b0, 0x8f0, 0x920, 0x950, 0x880, 0x990, 0x840};
  static const uint16 kMoveGravestone_Pos[15] = {0xa16, 0x99e, 0x9a2, 0x9aa, 0x92e, 0x934, 0x88a, 0x88e, 0x796, 0x79e, 0x7a4, 0x7aa, 0x690, 0x5b2, 0x508};
  static const uint8 kMoveGravestone_Ctr[15] = {0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x38, 0x58};
  static const uint8 kMoveGravestone_Idx[9] = {0, 1, 4, 6, 8, 12, 13, 14, 15};

  int k = Ancilla_AddAncilla(ain, yin);
  if (k < 0)
    return;
  int t = ((link_y_coord & 0xf) < 7 ? link_y_coord : link_y_coord + 16) & ~0xf;

  int i = 7;
  while (kMoveGravestone_Y[i] != t) {
    if (--i < 0) {
      ancilla_type[k] = 0;
      return;
    }
  }

  int j = kMoveGravestone_Idx[i];
  int end = kMoveGravestone_Idx[i + 1];
  do {
    int x = kMoveGravestone_X[j];
    if (x < link_x_coord && (uint16)(x + 15) >= link_x_coord) {
      if (j == 13 ? !link_is_running : link_is_running)
        break;

      int pos = kMoveGravestone_Pos[j];
      big_rock_starting_address = pos;
      door_open_closed_counter = kMoveGravestone_Ctr[j];
      if (door_open_closed_counter == 0x58) {
        sound_effect_2 = Link_CalculateSfxPan() | 0x1b;
      } else if (door_open_closed_counter == 0x38) {
        save_ow_event_info[BYTE(overworld_screen_index)] |= 0x20;
        sound_effect_2 = Link_CalculateSfxPan() | 0x1b;
      }

      ((uint8 *)door_debris_y)[k] = (pos - 0x80);
      ((uint8 *)door_debris_x)[k] = (pos - 0x80) >> 8;

      Overworld_DoMapUpdate32x32_B();

      if ((sound_effect_2 & 0x3f) != 0x1b)
        sound_effect_1 = Link_CalculateSfxPan() | 0x22;

      int yy = kMoveGravestone_Y1[j];
      int xx = kMoveGravestone_X1[j];
      bitmask_of_dragstate = 4;
      link_something_with_hookshot = 1;
      ancilla_A[k] = (yy - 18);
      ancilla_B[k] = (yy - 18) >> 8;
      Ancilla_SetXY(k, xx, yy - 2);
      return;
    }
  } while (++j != end);
  ancilla_type[k] = 0;
}

// Spawns the waterfall splash ancilla (type 0x41, limit 4) at the entrance of a
// waterfall secret passage. Guards with AncillaAdd_CheckForPresence so only one
// splash is active at a time — the handler itself checks proximity and triggers
// the entrance when Link walks into the waterfall's spray zone.
// Initialises timer = 2 (spray animation rate) and item_to_link = 0 (frame index).
void AncillaAdd_WaterfallSplash() {  // 899b68
  if (AncillaAdd_CheckForPresence(0x41))
    return;
  int k = Ancilla_AddAncilla(0x41, 4);
  if (k >= 0) {
    ancilla_timer[k] = 2;
    ancilla_item_to_link[k] = 0;
  }
}

// Triggers the Ganon's Tower break-the-seal cutscene (type 0x43, limit 4).
// Preconditions (all must be met):
//   - Link is not in a special state (link_state_bits bit 7 clear) and
//     link_auxiliary_state == 0 (no paralysis).
//   - All 7 crystals are collected: (link_has_crystals & 0x7F) == 0x7F.
//   - The GT seal event has not already fired:
//     save_ow_event_info[0x43] & 0x20 == 0.
// Pre-spawn cleanup: Ancilla_TerminateSparkleObjects clears any stray sparkles.
// Sprite cleanup: walks sprite_type[] looking for Agahnim-type sprites (0x37)
//   and kills them (state = 0) so they don't interfere with the cutscene.
// Initialises breaktowerseal state:
//   breaktowerseal_sparkle_var1[0..23] = 0xFF (sparkles not yet allocated).
//   breaktowerseal_var3[0..5]: per-crystal angular phase offsets (0,10,22,32,42,54)
//     that space the 6 secondary crystals evenly around the orbit.
//   breaktowerseal_var4 = 0 (cutscene sub-step), var5 = 240 (startup delay).
// Loads crystal sprite CHR, sets palette_sp6r_indoors = 4 and
// overworld_palette_aux_or_main = 0x200 for the GT environment palette.
// Immobilises Link (flag_is_link_immobilized = 1) and centres the ancilla
// 16 px above Link's world position.
void AncillaAdd_GTCutscene() {  // 899b83
  if (link_state_bits & 0x80 | link_auxiliary_state ||
     (link_has_crystals & 0x7f) != 0x7f ||
      save_ow_event_info[0x43] & 0x20)
    return;

  Ancilla_TerminateSparkleObjects();

  if (AncillaAdd_CheckForPresence(0x43))
    return;

  int k = Ancilla_AddAncilla(0x43, 4);
  if (k < 0)
    return;

  for (int i = 15; i >= 0; i--) {
    if (sprite_type[i] == 0x37)
      sprite_state[i] = 0;
  }

  for (int i = 0x17; i >= 0; i--)
    breaktowerseal_sparkle_var1[i] = 0xff;
  DecodeAnimatedSpriteTile_variable(0x28);
  palette_sp6r_indoors = 4;
  overworld_palette_aux_or_main = 0x200;
  Palette_Load_SpriteEnvironment_Dungeon();
  flag_update_cgram_in_nmi++;
  flag_is_link_immobilized = 1;
  ancilla_y_subpixel[k] = 0;
  ancilla_x_subpixel[k] = 0;
  ancilla_step[k] = 0;
  breaktowerseal_var5 = 240;
  breaktowerseal_var4 = 0;

  breaktowerseal_var3[0] = 0;

  breaktowerseal_var3[1] = 10;
  breaktowerseal_var3[2] = 22;
  breaktowerseal_var3[3] = 32;
  breaktowerseal_var3[4] = 42;
  breaktowerseal_var3[5] = 54;

  Ancilla_SetXY(k, link_x_coord, link_y_coord - 16);
}

// Spawns a door-debris ancilla (type 8, limit 1) for the rubble that falls when
// a dungeon door is blown open by a bomb. Returns the allocated slot index, or
// -1 if no slot was free. Initialises arr25 = 0 (frame counter) and
// arr26 = 7 (total debris frames — the debris animates through 7 images before
// the slot is cleared by the type-8 handler).
int AncillaAdd_DoorDebris() {  // 899c38
  int k = Ancilla_AddAncilla(8, 1);
  if (k >= 0) {
    ancilla_arr25[k] = 0;
    ancilla_arr26[k] = 7;
  }
  return k;
}

// Converts the fire-rod shot in slot k into the Skull Woods fire sequence that
// opens the dungeon entrance. Only triggers on the overworld Skull Woods screens
// (BYTE(overworld_screen_index) & 0x40 != 0) and only when outdoors.
// Takes over slots 0-5 directly: slot 0 becomes type 0x34 (SkullWoodsFire),
// slots 1-5 are cleared. Sets up skullwoodsfire_* parallel arrays:
//   var0[0..3] = {253, 254, 255, 0} — fire segment indices.
//   var5[0..3] = {5, 5, 5, 5}      — per-segment timer (5 ticks each).
//   var4 = 0                        — global fire sequence step.
//   var9/10 = 0x100, var11/12 = 0x98  — screen-space X/Y extents for the fire spread.
//   aux_timer[0] = 5 (initial animation delay for slot 0).
//   var9 = 0x100 (fire-spread right edge, in screen space).
// Sets trigger_special_entrance = 2 to queue the Skull Woods room transition
// once the fire animation completes. Clears subsubmodule_index = 0, BYTE(R16) = 0.
// Copies link_is_on_lower_level and link_is_on_lower_level_mirror for correct
// layer rendering, and clears item_to_link / step for slot 0.
void FireRodShot_BecomeSkullWoodsFire(int k) {  // 899c4f
  if (player_is_indoors || !(BYTE(overworld_screen_index) & 0x40))
    return;

  ancilla_type[0] = 0x34;
  ancilla_type[1] = 0;
  ancilla_type[2] = 0;
  ancilla_type[3] = 0;
  ancilla_type[4] = 0;
  ancilla_type[5] = 0;
  flag_for_boomerang_in_place = 0;
  ancilla_numspr[0] = kAncilla_Pflags[0x34];
  skullwoodsfire_var0[0] = 253;
  skullwoodsfire_var0[1] = 254;
  skullwoodsfire_var0[2] = 255;
  skullwoodsfire_var0[3] = 0;
  skullwoodsfire_var4 = 0;
  skullwoodsfire_var5[0] = 5;
  skullwoodsfire_var5[1] = 5;
  skullwoodsfire_var5[2] = 5;
  skullwoodsfire_var5[3] = 5;
  ancilla_aux_timer[0] = 5;
  skullwoodsfire_var9 = 0x100;
  skullwoodsfire_var10 = 0x100;
  skullwoodsfire_var11 = 0x98;
  skullwoodsfire_var12 = 0x98;

  trigger_special_entrance = 2;
  subsubmodule_index = 0;
  BYTE(R16) = 0;
  ancilla_floor[0] = link_is_on_lower_level;
  ancilla_floor2[0] = link_is_on_lower_level_mirror;
  ancilla_item_to_link[0] = 0;
  ancilla_step[0] = 0;

}

// Core ancilla slot allocator. Calls Ancilla_AllocInit(a, y) to find a free or
// recyclable slot within the [0, y] range, then performs common initialisation:
//   ancilla_type[k] = a       — assign the type.
//   ancilla_floor / floor2    — copy Link's current floor/mirror-floor so the
//                               ancilla renders on the same layer as Link.
//   ancilla_y_vel / x_vel = 0 — clear inherited velocity from any evicted slot.
//   ancilla_objprio[k] = 0    — reset OAM priority override.
//   ancilla_U[k] = 0          — clear scratch field.
//   ancilla_numspr[k]         — loaded from kAncilla_Pflags[a] (encodes the
//                               number of OAM sprites this type uses).
// Returns the slot index k, or -1 if no slot was available.
int Ancilla_AddAncilla(uint8 a, uint8 y) {  // 899ce2
  int k = Ancilla_AllocInit(a, y);
  if (k >= 0) {
    ancilla_type[k] = a;
    ancilla_floor[k] = link_is_on_lower_level;
    ancilla_floor2[k] = link_is_on_lower_level_mirror;
    ancilla_y_vel[k] = 0;
    ancilla_x_vel[k] = 0;
    ancilla_objprio[k] = 0;
    ancilla_U[k] = 0;
    ancilla_numspr[k] = kAncilla_Pflags[a];
  }
  return k;
}

// Returns true if an ancilla of type a is already active in any of slots 0-5.
// Used as a guard by spawn functions that allow at most one instance of a given
// ancilla type (e.g., waterfall splash, dash tremor, duck takeoff).
// Only checks slots 0-5 — the high slots (6-9) used by sparkles and silver-arrow
// effects are not considered "presence" for this purpose.
bool AncillaAdd_CheckForPresence(uint8 a) {  // 899d20
  for (int k = 5; k >= 0; k--) {
    if (ancilla_type[k] == a)
      return true;
  }
  return false;
}

// Arrow-specific slot allocator. type = ancilla type to assign; ay = slot limit.
// Counts how many slots currently hold an arrow (type 10 = arrow in flight):
//   - If the count does NOT equal ay+1: find the first empty slot (normal path).
//   - If the count equals ay+1 (at the limit): use a round-robin on
//     ancilla_alloc_rotate (wrapping 4→0) to find and evict an existing arrow
//     slot, giving the oldest arrow the lowest priority.
// This strategy ensures rapid fire never exceeds the configured arrow limit while
// still providing fair slot recycling. Performs the same common initialisation as
// Ancilla_AddAncilla (floor, velocity, numspr). Returns -1 if k < 0.
int AncillaAdd_ArrowFindSlot(uint8 type, uint8 ay) {  // 899d36
  int k, n = 0;
  for (k = 4; k >= 0; k--) {
    if (ancilla_type[k] == 10)
      n++;
  }
  if (n != ay + 1) {
    for (k = 4; k >= 0; k--) {
      if (ancilla_type[k] == 0)
        break;
    }
  } else {
    do {
      if (sign8(--ancilla_alloc_rotate))
        ancilla_alloc_rotate = 4;
      k = ancilla_alloc_rotate;
    } while (ancilla_type[k] != 10);
  }
  if (k >= 0) {
    ancilla_type[k] = type;
    ancilla_floor[k] = link_is_on_lower_level;
    ancilla_floor2[k] = link_is_on_lower_level_mirror;
    ancilla_y_vel[k] = 0;
    ancilla_x_vel[k] = 0;
    ancilla_objprio[k] = 0;
    ancilla_U[k] = 0;
    ancilla_numspr[k] = kAncilla_Pflags[type];
  }
  return k;
}

// Probes up to 3 tile positions in front of Link to find the first clear spawn
// point for a new projectile. Used by sword-beam, ice rod, and fire rod to avoid
// spawning inside a wall.
// Offsets are selected by ancilla_dir[k] (0=up,1=down,2=left,3=right): the
// kAncilla_Yoffs_Hb and kAncilla_Xoffs_Hb tables provide three candidate
// positions per direction, spaced 8 px apart.
// Iterates i from 2 down to 0; breaks as soon as Ancilla_CheckTileCollision
// returns a non-zero result (collision found at position i). The ancilla's
// position is updated to each candidate position during the scan.
// Returns i: the index of the first collision-free candidate (0-2), or
// -1 if all three positions collide (projectile is fully inside a wall).
int Ancilla_CheckInitialTile_A(int k) {  // 899dd3
  static const int8 kAncilla_Yoffs_Hb[12] = {8, 0, -8, 8, 16, 24, 8, 8, 8, 8, 8, 8};
  static const int8 kAncilla_Xoffs_Hb[12] = {0, 0, 0, 0, 0, 0, 0, -8, -16, 0, 8, 16};
  int j = ancilla_dir[k] * 3;
  int i;
  for (i = 2; i >= 0; i--, j++) {
    uint16 x = link_x_coord + kAncilla_Xoffs_Hb[j];
    uint16 y = link_y_coord + kAncilla_Yoffs_Hb[j];
    Ancilla_SetXY(k, x, y);
    if (Ancilla_CheckTileCollision(k))
      break;
  }
  return i;
}

// Class-2 variant of the initial tile collision probe. Probes up to 3 candidate
// positions per firing direction using kAncilla_InitialTileColl_X/Y offsets,
// and calls Ancilla_CheckTileCollision_Class2 (which uses a wider hit-box
// suited to the ice rod's larger projectile sprite). Returns true on the first
// collision found, placing the ancilla at that position, or false if no
// collision was found in any of the 3 candidates.
// Note: kAncilla_InitialTileColl_X[8] = 0x4b8b is annotated "// wtf" in the
// original source — this value is only reachable when j reaches index 8, which
// appears to be an overrun guard that should never trigger in practice.
bool Ancilla_CheckInitialTileCollision_Class2(int k) {  // 899e44
  static const int16 kAncilla_InitialTileColl_Y[9] = {15, 16, 28, 24, 12, 12, 12, 12, 8};
  static const int16 kAncilla_InitialTileColl_X[9] = {8, 8, 8, 8, -1, 0, 17, 16, 0x4b8b}; // wtf
  int j = ancilla_dir[k] * 2;
  for (int n = 2; n >= 0; n--, j++) {
    Ancilla_SetXY(k, link_x_coord + kAncilla_InitialTileColl_X[j],
                     link_y_coord + kAncilla_InitialTileColl_Y[j]);
    if (Ancilla_CheckTileCollision_Class2(k))
      return true;
  }
  return false;
}

// Cleans up interactive ancilla slots when Link's state changes (e.g., entering
// a door, dying, or picking up an item). Scans slots 5 down to 0:
//   - If the slot holds type 0x3E (Rising Crystal): records that slot index in
//     y so the caller can identify the crystal for further processing.
//   - If the slot holds type 0x2C (Somaria block): clears dung_flag_somaria_block_switch
//     and releases the drag-state if Link was being dragged (bitmask_of_dragstate & 0x80).
//   - Termination logic:
//       * If link_state_bits bit 7 is set (carrying/interacting): only clear the
//         slot if it is NOT the one being held (flag_is_ancilla_to_pick_up - 1).
//       * Otherwise: if the slot IS the carried one, clear flag_is_ancilla_to_pick_up;
//         then clear the slot unconditionally.
// After the loop, resets all transient Link state flags that were set for
// interactive ancilla use: flute_countdown, tagalong_event_flags, byte_7E02F3,
// flag_for_boomerang_in_place, is_archer_or_shovel_game,
// link_disable_sprite_damage, byte_7E03FD, link_electrocute_on_touch.
// Special-cases link_player_handler_state == 19 (hookshot riding state):
// resets it to 0 and clears the hookshot-related button/direction locks.
// Returns y (the Rising Crystal slot index, or the original y if none found).
uint8 Ancilla_TerminateSelectInteractives(uint8 y) {  // 89ac6b
  int i = 5;

  do {
    if (ancilla_type[i] == 0x3e) {
      y = i;
    }
    else if (ancilla_type[i] == 0x2c) {
      dung_flag_somaria_block_switch = 0;
      if (bitmask_of_dragstate & 0x80) {
        bitmask_of_dragstate = 0;
        link_speed_setting = 0;
      }
    }

    if (sign8(link_state_bits)) {
      if (i + 1 != flag_is_ancilla_to_pick_up)
        ancilla_type[i] = 0;
    }
    else {
      if (i + 1 == flag_is_ancilla_to_pick_up)
        flag_is_ancilla_to_pick_up = 0;
      ancilla_type[i] = 0;
    }
  } while (--i >= 0);

  if (link_position_mode & 0x10) {
    link_incapacitated_timer = 0;
    link_position_mode = 0;
  }
  flute_countdown = 0;
  tagalong_event_flags = 0;
  byte_7E02F3 = 0;
  flag_for_boomerang_in_place = 0;
  is_archer_or_shovel_game = 0;
  link_disable_sprite_damage = 0;
  byte_7E03FD = 0;
  link_electrocute_on_touch = 0;
  if (link_player_handler_state == 19) {
    link_player_handler_state = 0;
    button_mask_b_y &= ~0x40;
    link_cant_change_direction &= ~1;
    link_position_mode &= ~4;
    related_to_hookshot = 0;
  }
  return y;
}

// Convenience wrapper that sets both the X and Y world coordinates for ancilla k
// in a single call. Delegates to Ancilla_SetX and Ancilla_SetY which split the
// 16-bit values across the _lo / _hi byte arrays.
void Ancilla_SetXY(int k, uint16 x, uint16 y) {  // 89ad06
  Ancilla_SetX(k, x);
  Ancilla_SetY(k, y);
}

// In-place transmutation: converts an existing Somaria block slot (k) into the
// exploding fission effect (type 0x2E). Does not call Ancilla_AddAncilla because
// the slot is already occupied — it simply overwrites the type and reinitialises
// the fields specific to the fission sequence:
//   numspr = kAncilla_Pflags[0x2E] — reload OAM sprite count for the new type.
//   aux_timer = 3, step = 0, item_to_link = 0, arr3 = 0, arr1 = 0, R = 0,
//   objprio = 0 — all cleared for a clean fission start.
//   dung_flag_somaria_block_switch = 0 — deactivate the block's switch effect.
// Plays SFX 1 (explosion sound) panned to the block's screen X position.
void AncillaAdd_ExplodingSomariaBlock(int k) {  // 89ad30
  ancilla_type[k] = 0x2e;
  ancilla_numspr[k] = kAncilla_Pflags[0x2e];
  ancilla_aux_timer[k] = 3;
  ancilla_step[k] = 0;
  ancilla_item_to_link[k] = 0;
  ancilla_arr3[k] = 0;
  ancilla_arr1[k] = 0;
  ancilla_R[k] = 0;
  ancilla_objprio[k] = 0;
  dung_flag_somaria_block_switch = 0;
  sound_effect_2 = Ancilla_CalculateSfxPan(k) | 1;
}

// Credits rupees to link_rupees_goal based on the drop code stored in
// ancilla_item_to_link[k]. The drop-code → value mapping is:
//   0x34 →   1 rupee  (green)
//   0x35 →   5 rupees (blue)
//   0x36 →  20 rupees (red)
//   0x40 → 100 rupees (large red, via kGiveRupeeGift_Tab[3])
//   0x41 →  50 rupees (purple, via kGiveRupeeGift_Tab[4])
//   0x46 → 300 rupees (happiness pond special)
//   0x47 →  20 rupees (happiness pond blue repeat)
// Returns true if a recognised drop code was found and rupees were added,
// false if the item code is not a rupee (caller should handle it differently,
// e.g., as a heart, bomb, or key drop).
bool Ancilla_AddRupees(int k) {  // 89ad6c
  static const uint8 kGiveRupeeGift_Tab[5] = {1, 5, 20, 100, 50};
  uint8 a = ancilla_item_to_link[k];
  if (a == 0x34 || a == 0x35 || a == 0x36) {
    link_rupees_goal += kGiveRupeeGift_Tab[a - 0x34];
  } else if (a == 0x40 || a == 0x41) {
    link_rupees_goal += kGiveRupeeGift_Tab[a - 0x40 + 3];
  } else if (a == 0x46) {
    link_rupees_goal += 300;
  } else if (a == 0x47) {
    link_rupees_goal += 20;
  } else {
    return false;
  }
  return true;
}

// Draw helper for the "motive" dash-dust puff that appears behind Link when he
// dashes up a slope (the small directional dust cloud). Shares animation timing
// with the standard dash dust: timer counts down from 3; when it fires, the
// frame index (item_to_link) advances through the 3 puff frames
// (kMotiveDashDust_Draw_Char: 0xA9, 0xCF, 0xDF). At frame 3 the slot is cleared.
// If Link is facing left (link_direction_facing == 2), calls
// Oam_AllocateFromRegionB(4) to get an OAM slot from the left-sprite pool so
// the puff draws behind Link's sprite. Uses Ancilla_PrepOamCoord + Ancilla_SetOam
// for the final OAM write with palette 4 and the current oam_priority_value.
void DashDust_Motive(int k) {  // 89adf4
  static const uint8 kMotiveDashDust_Draw_Char[3] = {0xa9, 0xcf, 0xdf};
  if (!ancilla_timer[k]) {
    ancilla_timer[k] = 3;
    if (++ancilla_item_to_link[k] == 3) {
      ancilla_type[k] = 0;
      return;
    }
  }
  if (link_direction_facing == 2)
    Oam_AllocateFromRegionB(4);
  Point16U info;
  Ancilla_PrepOamCoord(k, &info);
  Ancilla_SetOam(GetOamCurPtr(), info.x, info.y,
                 kMotiveDashDust_Draw_Char[ancilla_item_to_link[k]], 4 | HIBYTE(oam_priority_value), 0);
}

// Returns the stereo pan byte for a sound effect positioned at ancilla k's world X.
// Delegates to CalculateSfxPan(Ancilla_GetX(k)), which maps the world X minus
// the scroll offset to a 0-7 pan value (0 = hard left, 7 = hard right).
// The return value is OR'd with a sound effect ID by callers before being written
// to sound_effect_1 or sound_effect_2.
uint8 Ancilla_CalculateSfxPan(int k) {  // 8dbb5e
  return CalculateSfxPan(Ancilla_GetX(k));
}

// Low-level ancilla slot allocator. Does not set any fields — only finds a slot.
// type = the ancilla type being spawned; limit = maximum slot index to search (0-4).
// Algorithm:
// 1. Count how many slots [0, limit) currently hold ancillas of `type`. If the
//    count equals limit+1 (the cap), return -1 immediately.
// 2. SNES bug workaround: when the kBugFix_PolyRenderer flag is active, prime
//    BYTE(R14) = limit+1 to prevent tile-detection residue from corrupting the
//    count-match above.
// 3. Prefer an empty slot (type == 0): scan [limit, 0] downward (special upper
//    bound for types 7/8 which use the `limit` parameter directly).
// 4. If no empty slot: scan round-robin from ancilla_alloc_rotate downward,
//    looking for a slot holding a recyclable type:
//       0x3C (silver-arrow sparkle), 0x13 (arrow stuck in wall), 0x0A (arrow).
//    These three types are designated "evictable" — they have no lasting game
//    state and can be safely overwritten by higher-priority ancilla spawns.
//    Updates ancilla_alloc_rotate to the evicted slot for round-robin fairness.
// Returns the slot index k, or -1 if nothing was available.
int Ancilla_AllocInit(uint8 type, uint8 limit) {  // 8ff577
  // snes bug: R14 is used in tile detection already
  // unless this is here it the memcmp will fail when entering/leaving a water through steps quickly
  if (g_ram[kRam_BugsFixed] >= kBugFix_PolyRenderer)
    BYTE(R14) = limit + 1;

  int n = 0;
  for (int i = 0; i < 5; i++) {
    if (ancilla_type[i] == type)
      n++;
  }
  if (limit + 1 == n)
    return -1;

  // Try to reuse an empty ancilla slot
  for (int j = (type == 7 || type == 8) ? limit : 4; j >= 0; j--) {
    if (ancilla_type[j] == 0)
      return j;
  }
  int k = ancilla_alloc_rotate;
  do {
    if (--k < 0)
      k = limit;
    uint8 old_type = ancilla_type[k];
    // reuse slots for sparkles or arrows in wall
    if (old_type == 0x3c || old_type == 0x13 || old_type == 0xa) {
      ancilla_alloc_rotate = k;
      return k;
    }
  } while (k != 0);
  ancilla_alloc_rotate = 0;
  return -1;
}

// Spawns a sword-beam projectile (type 0x0C, limit y) when Link swings at full
// health. y controls the slot search limit.
// Initialises the swordbeam_arr[0..3] rotation sequence from kSwordBeam_Tab,
// indexed by link_direction_facing * 2 (each direction provides 4 rotation
// frames). swordbeam_var1 tracks the current tail-rotation frame; var2 = 14
// sets the number of rotation frames before the beam stabilises.
// Directional velocity: kSwordBeam_Yvel / Xvel (±64 px/frame) in the firing
// direction. kSwordBeam_S gives a one-axis signed sprite-offset to keep the
// beam visually centred on Link's sword.
// Initial position probe: swordbeam_temp_x/y are set to Link+8/+12 (sword tip),
// then Ancilla_CheckInitialTile_A checks 3 tiles ahead. If a clear path exists:
//   - Final position is set to temp + kSwordBeam_X/Y[dir] offsets.
//   - Plays SFX 1 (beam launch, panned).
//   - If the probe fails (beam spawns inside a wall): the type is transmuted to
//     type 4 (sparkle), timer = 7, numspr = 16 — a brief wall-hit flash.
// ancilla_item_to_link = 0x4C (76), the initial beam lifetime counter.
// ancilla_L = 0 (sub-step), ancilla_G = 0 (??phase), ancilla_arr1 = 0,
// ancilla_arr3 = 8 (trail length), ancilla_aux_timer = 2 (tick divisor).
void AddSwordBeam(uint8 y) {  // 8ff67b
  static const int8 kSwordBeam_X[4] = {-8, -10, -22, 4};
  static const int8 kSwordBeam_Y[4] = {-24, 8, -6, -6};
  static const int8 kSwordBeam_S[4] = {-8, -8, -8, 8};
  static const uint8 kSwordBeam_Tab[16] = {0x21, 0x1d, 0x19, 0x15, 3, 0x3e, 0x3a, 0x36, 0x12, 0xe, 0xa, 6, 0x31, 0x2d, 0x29, 0x25};
  static const int8 kSwordBeam_Yvel[4] = {-64, 64, 0, 0};
  static const int8 kSwordBeam_Xvel[4] = {0, 0, -64, 64};

  int k = Ancilla_AddAncilla(0xc, y);
  if (k < 0)
    return;
  int j = link_direction_facing * 2;
  swordbeam_arr[0] = kSwordBeam_Tab[j + 0];
  swordbeam_arr[1] = kSwordBeam_Tab[j + 1];
  swordbeam_arr[2] = kSwordBeam_Tab[j + 2];
  swordbeam_arr[3] = swordbeam_var1 = kSwordBeam_Tab[j + 3];
  ancilla_aux_timer[k] = 2;
  ancilla_item_to_link[k] = 0x4c;
  ancilla_arr3[k] = 8;
  ancilla_step[k] = 0;
  ancilla_L[k] = 0;
  ancilla_G[k] = 0;
  ancilla_arr1[k] = 0;
  swordbeam_var2 = 14;
  j = link_direction_facing >> 1;
  ancilla_dir[k] = j;
  ancilla_y_vel[k] = kSwordBeam_Yvel[j];
  ancilla_x_vel[k] = kSwordBeam_Xvel[j];
  ancilla_S[k] = kSwordBeam_S[j];

  swordbeam_temp_y = link_y_coord + 12;
  swordbeam_temp_x = link_x_coord + 8;

  if (Ancilla_CheckInitialTile_A(k) >= 0) {
    Ancilla_SetXY(k, swordbeam_temp_x + kSwordBeam_X[j], swordbeam_temp_y + kSwordBeam_Y[j]);
    sound_effect_2 = 1 | Ancilla_CalculateSfxPan(k);
    ancilla_type[k] = 4;
    ancilla_timer[k] = 7;
    ancilla_numspr[k] = 16;
  }
}

// Spawns a sword-charge sparkle in a high ancilla slot (6-9) during the spin-
// attack charge hold. Called every few frames while link_spin_attack_step_counter
// is advancing to create the orbiting sparkle trail around Link's sword.
// Position derivation:
//   kSwordChargeSparkle_A[dir] / B[dir] — control which axis gets the spin-counter
//   offset (A affects Y, B affects X). For a given direction, one axis gets a
//   counter-proportional push and the other uses a fixed table offset from
//   kSwordChargeSparkle_X/Y. This makes the sparkles orbit Link in the correct
//   plane for the facing direction.
//   GetRandomNumber() supplies a 1–7 px jitter masked by m0/m1 to randomise
//   the sparkle's exact position within the orbit arc.
// timer = 4 drives the 4-frame fade animation (shared with silver-arrow sparkle).
void AncillaSpawn_SwordChargeSparkle() {  // 8ff979
  static const uint8 kSwordChargeSparkle_A[4] = {0, 0, 7, 7};
  static const uint8 kSwordChargeSparkle_B[4] = {0x70, 0x70, 0, 0};
  static const uint8 kSwordChargeSparkle_X[4] = {0, 3, 4, 5};
  static const uint8 kSwordChargeSparkle_Y[4] = {5, 12, 8, 8};
  int k = Ancilla_AllocHigh();
  if (k < 0)
    return;
  ancilla_type[k] = 0x3c;
  ancilla_item_to_link[k] = 0;
  ancilla_timer[k] = 4;
  ancilla_floor[k] = link_is_on_lower_level;
  int j = link_direction_facing >> 1;
  int8 x = 0, y = 0;
  uint8 m0 = kSwordChargeSparkle_A[j];
  if (!m0) {
    y = link_spin_attack_step_counter >> 2;
    if (j == 0)
      y = -y;
  }
  uint8 m1 = kSwordChargeSparkle_B[j];
  if (!m1) {
    x = link_spin_attack_step_counter >> 2;
    if (j == 2)
      x = -x;
  }
  uint8 r = GetRandomNumber();
  Ancilla_SetXY(k,
    link_x_coord + x + kSwordChargeSparkle_X[j] + ((r & m1) >> 4),
    link_y_coord + y + kSwordChargeSparkle_Y[j] + (r & m0));
}

// Flips the dash-tremor ancilla's Y offset each call (simulates shaking) and
// clamps it to the active playfield so the camera never trembles off the scroll
// boundaries. k = the tremor ancilla slot.
// Negates Ancilla_GetY(k) and writes it back via Ancilla_SetY — this creates
// the alternating +/- shake each frame.
// Indoors: no boundary clamping needed (no scroll limits); returns the raw offset.
// Outdoors: compares the camera position (y + BG2VOFS_copy2 for vertical dash,
// x + BG2HOFS_copy2 for horizontal) against ow_scroll_vars0.ystart/yend or
// xstart/xend ±1. If the camera would scroll past either edge, the offset is
// clamped to 0 (tremor suppressed) to prevent the overworld map from scrolling
// beyond its boundary tiles. Returns the (possibly clamped) offset for the caller.
int DashTremor_TwiddleOffset(int k) {  // 8ffafe
  int j = ancilla_dir[k];
  uint16 y = -Ancilla_GetY(k);
  Ancilla_SetY(k, y);
  if (player_is_indoors)
    return y;
  if (j == 2) {
    uint16 start = ow_scroll_vars0.ystart + 1;
    uint16 end = ow_scroll_vars0.yend - 1;
    uint16 a = y + BG2VOFS_copy2;
    return (a <= start || a >= end) ? 0 : y;
  } else {
    uint16 start = ow_scroll_vars0.xstart + 1;
    uint16 end = ow_scroll_vars0.xend - 1;
    uint16 a = y + BG2HOFS_copy2;
    return (a <= start || a >= end) ? 0 : y;
  }
}

// Clears ancilla j's type (terminating it) if it has drifted outside the visible
// screen viewport. j = ancilla slot index.
// Computes screen-relative X and Y by subtracting the BG2 scroll offsets.
// Under ExtendScreen64, the horizontal bound is widened by xt=64 px on each
// side so projectiles aren't culled early on the extended display.
// The unsigned comparison `x >= 244 + xt*2 || y >= 240` naturally handles
// negative (wrap-around) off-screen values since they map to large unsigned
// values (e.g., -1 = 0xFFFF >> very large).
void Ancilla_TerminateIfOffscreen(int j) {  // 8ffd52
  int xt = (enhanced_features0 & kFeatures0_ExtendScreen64) ? 0x40 : 0;
  uint16 x = Ancilla_GetX(j) - BG2HOFS_copy2 + xt;
  uint16 y = Ancilla_GetY(j) - BG2VOFS_copy2;
  if (x >= 244 + xt * 2 || y >= 240)
    ancilla_type[j] = 0;
}

// Determines the visual status of the bomb's underside sprite (the shadow/ripple
// drawn below a floating bomb). k = bomb ancilla slot.
// Returns true (skip normal underside draw) in two cases:
//   1. ancilla_item_to_link[k] != 0: bomb hasn't been placed on the ground yet.
//   2. k+1 == flag_is_ancilla_to_pick_up && link_state_bits bit 7 set: Link is
//      carrying the bomb — no underside sprite while held overhead.
// Otherwise, populates *out_r10 with the underside state code:
//   tile_attr == 9 (water): cycles through 3 ripple frames (ancilla_arr22/23
//     driven by a 3-tick counter) and plays water-bubble SFX if the sound
//     channel is free. r10 = 4/5/6 (ripple frames).
//   tile_attr == 0x40 (lava): r10 = 3 (lava bubble frame).
//   ancilla_z in [2, 251] (airborne but low): r10 = 2 (shadow frame).
//   Otherwise: r10 = 0 (ground shadow, default).
// Adjusts *out_pt (the draw position) by z+2 px downward and 8 px left so the
// underside sprite tracks the bomb's vertical arc correctly.
bool Bomb_CheckUndersideSpriteStatus(int k, Point16U *out_pt, uint8 *out_r10) {  // 8ffdcf
  if (ancilla_item_to_link[k] != 0)
    return true;

  uint8 r10 = 0;
  if (ancilla_tile_attr[k] == 9) {
    if (sign8(--ancilla_arr22[k])) {
      ancilla_arr22[k] = 3;
      if (++ancilla_arr23[k] == 3)
        ancilla_arr23[k] = 0;
    }
    r10 = ancilla_arr23[k] + 4;
    if ((sound_effect_1 & 0x3f) == 0xb || (sound_effect_1 & 0x3f) == 0x21)
      sound_effect_1 = Ancilla_CalculateSfxPan(k) | 0x28;
  } else if (ancilla_tile_attr[k] == 0x40) {
    r10 = 3;
  }

  if (ancilla_z[k] >= 2 && ancilla_z[k] < 252)
    r10 = 2;
  if (k + 1 == flag_is_ancilla_to_pick_up && (link_state_bits & 0x80))
    return true;
  int z = (int8)ancilla_z[k];
  out_pt->y += z + 2;
  out_pt->x += -8;
  *out_r10 = r10;
  return false;
}

// Converts an arrow ancilla (slot k) into a live deflected-arrow sprite when the
// arrow has been blocked (e.g., by a knight's shield or a mirror tile).
// Clears the ancilla slot (ancilla_type[k] = 0), then dynamically spawns sprite
// type 0x1B (Deflected Arrow) via Sprite_SpawnDynamically. If a sprite slot j
// was allocated, copies the ancilla's position (x_lo/hi, y_lo/hi), sets
// sprite_state = 6 (active/bouncing), sprite_delay_main = 31 (lifetime), and
// carries over the ancilla's X/Y velocity so the deflected arrow continues in the
// direction it was travelling. Assigns link_is_on_lower_level for correct layer,
// then calls Sprite_PlaceWeaponTink to play the deflection clink sound.
void Sprite_CreateDeflectedArrow(int k) {  // 9d8040
  ancilla_type[k] = 0;
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(k, 0x1b, &info);
  if (j >= 0) {
    sprite_x_lo[j] = ancilla_x_lo[k];
    sprite_x_hi[j] = ancilla_x_hi[k];
    sprite_y_lo[j] = ancilla_y_lo[k];
    sprite_y_hi[j] = ancilla_y_hi[k];
    sprite_state[j] = 6;
    sprite_delay_main[j] = 31;
    sprite_x_vel[j] = ancilla_x_vel[k];
    sprite_y_vel[j] = ancilla_y_vel[k];
    sprite_floor[j] = link_is_on_lower_level;
    Sprite_PlaceWeaponTink(j);
  }
}

