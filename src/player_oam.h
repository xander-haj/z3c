/*
 * player_oam.h - Link's sprite OAM assembly and weapon hitbox calculation
 *
 * Handles the construction of Link's on-screen appearance each frame by
 * writing entries into the SNES Object Attribute Memory (OAM) table. Link's
 * visual representation is composed of multiple OAM sprites: his body, his
 * currently held equipment (sword, shield, boomerang, etc.), and effects
 * like sword sparkles and shadow sprites.
 *
 * This module is responsible for:
 *   - Selecting the correct VRAM tile offsets for Link's current animation
 *     frame, direction, and equipped items
 *   - Calculating the sword's hitbox rectangle for damage detection
 *   - Positioning weapon and equipment sprites relative to Link's body
 *   - Drawing auxiliary sprites (shadow under Link when falling in dungeons,
 *     foot splash/dust when walking through water or grass)
 *
 * OAM on the SNES supports 128 sprites (4 bytes each: x, y, tile, attributes)
 * plus a high table for 9th X-bit and size selection. Link typically consumes
 * 4-8 OAM slots depending on his current action and equipment.
 */
#pragma once
#include "types.h"

/*
 * SwordResult - Output from weapon/equipment VRAM offset calculations
 *
 * Captures two register values (named r6 and r12 after the original SNES
 * registers they occupied) that encode tile index offsets and palette
 * information for Link's weapon or equipment sprites.
 *   r6  -- VRAM character offset for the weapon/equipment tile
 *   r12 -- OAM attribute byte (palette, priority, flip flags)
 */
typedef struct SwordResult {
  uint8 r6;
  uint8 r12;
} SwordResult;

/* ---------------------------------------------------------------------------
 * Sword and weapon logic
 * --------------------------------------------------------------------------- */

// Returns true if Link's current action state requires invoking the sword
// swing animation (checking action state, button presses, and cooldown timers).
bool PlayerOam_WantInvokeSword();

// Computes the sword's damage hitbox rectangle based on Link's facing
// direction and current sword swing frame. The hitbox is written into
// global variables that the sprite collision system reads.
void CalculateSwordHitBox();

/* ---------------------------------------------------------------------------
 * Main OAM assembly entry point
 * --------------------------------------------------------------------------- */

// Top-level function called once per frame to build all of Link's OAM entries.
// Determines Link's animation frame, facing direction, and action state, then
// writes body, weapon, equipment, and effect sprites into the OAM buffer.
void LinkOam_Main();

/* ---------------------------------------------------------------------------
 * Utility and sprite component functions
 * --------------------------------------------------------------------------- */

// Returns the bit position of the highest set bit in |v| (0-7).
// Used to decode packed sprite attribute fields where each bit represents
// a different visual property.
uint8 FindMostSignificantBit(uint8 v);

// Calculates the VRAM tile offset and OAM attributes for Link's current weapon
// (sword, hammer, net, etc.) and writes them into |sr|. |r2| selects the
// animation frame. Returns true if a weapon sprite should be drawn.
bool LinkOam_SetWeaponVRAMOffsets(int r2, SwordResult *sr);

// Calculates the VRAM tile offset and OAM attributes for Link's current
// equipment (shield, held item) and writes them into |sr|. |r2| selects the
// animation frame. Returns true if an equipment sprite should be drawn.
bool LinkOam_SetEquipmentVRAMOffsets(int r2, SwordResult *sr);

// Computes the screen position for the sword sparkle effect and writes
// it into the OAM buffer at |oam_pos|. |oam_x| and |oam_y| are Link's
// current screen coordinates. Returns the updated OAM buffer index.
int LinkOam_CalculateSwordSparklePosition(int oam_pos, uint8 oam_x, uint8 oam_y);

// Unused function from the original ROM that configured weapon sprite
// settings. Preserved for ROM accuracy. |r4loc| is the OAM write position.
void LinkOam_UnusedWeaponSettings(int r4loc, uint8 oam_x, uint8 oam_y);

// Draws Link's circular shadow sprite when he is airborne in a dungeon
// (falling down a pit or using the hookshot). |r4loc| is the OAM slot,
// |xcoord| is the shadow's X screen position.
void LinkOam_DrawDungeonFallShadow(int r4loc, uint8 xcoord);

// Draws the sprite under Link's feet -- a splash in water, a dust puff on
// sand, or grass parting. |r4loc| is the OAM slot; |oam_x|, |oam_y| are
// Link's screen coordinates.
void LinkOam_DrawFootObject(int r4loc, uint8 oam_x, uint8 oam_y);

// Computes the X-offset between a given X coordinate |x| and Link's current
// screen position. Used to position equipment sprites relative to Link's body.
void LinkOam_CalculateXOffsetRelativeLink(uint8 x);
