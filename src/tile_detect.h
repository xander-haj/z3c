/*
 * tile_detect.h - Tile-based collision detection for Link and projectiles
 *
 * The zelda3 world is built from 8x8 pixel tiles arranged into 16x16 metatiles.
 * Each metatile has a collision attribute (solid, water, pit, slope, door, etc.)
 * stored in a tilemap that mirrors the visual map. This module reads those
 * attributes to determine what happens when Link (or his hookshot) attempts to
 * move into a given tile position.
 *
 * Collision detection is performed separately for X-axis and Y-axis movement,
 * and additional variants handle slope physics, door nudging, and mirror warp
 * bonk checks. The results are written into global collision state variables
 * that the player movement code in player.c reads to allow, block, or modify
 * Link's movement each frame.
 *
 * Key design note: indoor (dungeon) and outdoor (overworld) maps use different
 * tilemap layouts and attribute tables, so TileDetect_ExecuteInner() takes an
 * |is_indoors| flag to select the correct lookup path.
 */
#pragma once
#include "types.h"

/* ---------------------------------------------------------------------------
 * Overworld tile attribute query
 * --------------------------------------------------------------------------- */

// Returns the collision attribute byte for the overworld tile at pixel
// coordinates (|x|, |y|). Used by the overworld module to check terrain
// type (grass, water, deep water, ledge, etc.) at an arbitrary position.
uint8 Overworld_GetTileAttributeAtLocation(uint16 x, uint16 y);

/* ---------------------------------------------------------------------------
 * Directional movement collision
 *
 * These are called by the player movement code each frame to test whether
 * Link can move in the requested direction. They sample collision tiles at
 * Link's leading edge in the direction of travel.
 * --------------------------------------------------------------------------- */

// Tests collision for vertical movement. |direction| encodes whether Link
// is moving up or down and selects the appropriate tile sample points.
void TileDetect_Movement_Y(uint16 direction);

// Tests collision for horizontal movement. |direction| encodes left or right.
void TileDetect_Movement_X(uint16 direction);

// Tests collision for vertical movement on sloped terrain. Slopes modify
// Link's X position as he moves up/down to create diagonal sliding.
void TileDetect_Movement_VerticalSlopes(uint16_t direction);

// Tests collision for horizontal movement on sloped terrain.
void TileDetect_Movement_HorizontalSlopes(uint16_t direction);

// Performs a broad collision check of all tiles surrounding Link's current
// position, detecting nearby hazards (pits, water, conveyor belts) that
// affect gameplay even when Link is not actively moving into them.
void Player_TileDetectNearby();

/* ---------------------------------------------------------------------------
 * Hookshot collision
 * --------------------------------------------------------------------------- */

// Checks tile collision for the hookshot projectile in ancilla slot |k|.
// The hookshot can latch onto certain tile types (pots, chests, walls)
// and must stop when hitting solid terrain.
void Hookshot_CheckTileCollision(int k);

// Checks a single BG layer for hookshot collision at pixel position (|x|, |y|)
// traveling in direction |dir|. Called by CheckTileCollision for each active
// BG layer (dungeons can have two collision layers for overlapping floors).
void Hookshot_CheckSingleLayerTileCollision(uint16 x, uint16 y, int dir);

/* ---------------------------------------------------------------------------
 * Special-case collision handlers
 * --------------------------------------------------------------------------- */

// Adjusts Link's position when he is partially inside a door opening,
// nudging him by |speed| pixels per frame to align with the doorway.
// Prevents Link from getting stuck on door edges during transitions.
void HandleNudgingInADoor(int8 speed);

// Checks whether Link would collide with a solid tile when emerging from
// a Magic Mirror warp. If so, the warp is cancelled with a "bonk" effect
// and Link returns to the Dark World.
void TileCheckForMirrorBonk();

// Tests whether Link's sword swing animation clips into a door frame.
// |dw| selects the door side being tested. Used to prevent sword attacks
// from triggering tile interactions through walls.
void TileDetect_SwordSwingDeepInDoor(uint8 dw);

// Resets all tile detection result flags to their default (no collision)
// state at the start of a new detection pass.
void TileDetect_ResetState();

/* ---------------------------------------------------------------------------
 * Core tile detection engine
 * --------------------------------------------------------------------------- */

// Entry point for a single tile detection probe at pixel position (|x|, |y|).
// |bits| is a bitmask selecting which collision result flags to update.
// Looks up the tile at the given position and delegates to ExecuteInner.
void TileDetection_Execute(uint16 x, uint16 y, uint16 bits);

// Processes a single tile collision result. Given the tile attribute |tile|,
// tilemap offset |offs|, result selector |bits|, and whether the current
// map is indoors, updates the appropriate global collision flags (solid,
// staircase, conveyor, pit, water, etc.) based on the tile's type.
void TileDetect_ExecuteInner(uint8 tile, uint16 offs, uint16 bits, bool is_indoors);
