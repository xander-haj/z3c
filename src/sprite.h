/*
 * sprite.h - Enemy and NPC Sprite Framework
 *
 * Core header for the sprite subsystem in the Zelda 3 C reimplementation.
 * Defines the data structures and function interfaces for sprite lifecycle
 * management including allocation, coordinate handling, collision detection,
 * hit box resolution, damage calculation, drawing/OAM management, death
 * effects, and overworld/dungeon loading. Sprites encompass enemies, NPCs,
 * collectible items, projectiles, and environmental objects.
 *
 * The SNES uses an Object Attribute Memory (OAM) system for hardware sprites.
 * Many functions here translate game-world sprite state into OAM entries for
 * rendering. The "garnish" system handles lightweight visual effects (sparks,
 * trails, debris) that share the sprite rendering pipeline but have simpler
 * state machines.
 *
 * Depends on: types.h (primitive typedefs), variables.h (WRAM-mapped globals)
 * Used by: sprite.c, sprite_main.c, and virtually all game modules that
 *          interact with enemies, NPCs, or collectible objects.
 */
#pragma once
#include "types.h"
#include "variables.h"


/*
 * PrepOamCoordsRet - Return value from OAM coordinate preparation.
 *
 * When a sprite is about to be drawn, its world-space coordinates must be
 * converted to screen-space OAM coordinates. This struct carries the
 * converted position plus rendering metadata back to the caller.
 */
typedef struct PrepOamCoordsRet {
  uint16 x, y;       // Screen-space pixel coordinates for OAM placement
  uint8 r4;           // General-purpose return register (usage varies by caller)
  uint8 flags;        // OAM attribute flags (priority, palette, flip bits)
} PrepOamCoordsRet;

/*
 * SpriteHitBox - Axis-aligned bounding box pair for collision detection.
 *
 * Encodes two rectangles: one for Link (or another entity) and one for the
 * sprite being tested. The naming convention (r0, r1, etc.) mirrors the
 * SNES CPU registers used in the original assembly collision routines.
 * Collision is detected when the two rectangles overlap on both axes.
 */
typedef struct SpriteHitBox {
  // Entity A (typically Link or an ancilla) bounding box
  uint8 r0_xlo;         // X coordinate low byte of entity A
  uint8 r8_xhi;         // X coordinate high byte of entity A
  uint8 r1_ylo;         // Y coordinate low byte of entity A
  uint8 r9_yhi;         // Y coordinate high byte of entity A
  uint8 r2, r3;         // Width and height of entity A's hit box

  // Entity B (the sprite being tested) bounding box
  uint8 r4_spr_xlo;     // X coordinate low byte of sprite
  uint8 r10_spr_xhi;    // X coordinate high byte of sprite

  uint8 r5_spr_ylo;     // Y coordinate low byte of sprite
  uint8 r11_spr_yhi;    // Y coordinate high byte of sprite
  uint8 r6_spr_xsize;   // Sprite hit box width in pixels
  uint8 r7_spr_ysize;   // Sprite hit box height in pixels
} SpriteHitBox;

/*
 * SpriteSpawnInfo - Coordinate bundle for dynamically spawning sprites.
 *
 * When a sprite spawns another sprite (e.g., an enemy shooting a projectile
 * or an overlord spawning a minion), this struct passes the initial position.
 * The overlord fields are used when the spawner is an overlord rather than
 * a regular sprite, since overlords use a separate coordinate system.
 */
typedef struct SpriteSpawnInfo {
  uint16 r0_x;          // World X position for the new sprite
  uint16 r2_y;          // World Y position for the new sprite
  uint8 r4_z;           // Z height (vertical offset above ground plane)
  uint16 r5_overlord_x; // X position of the overlord that triggered spawn
  uint16 r7_overlord_y; // Y position of the overlord that triggered spawn
} SpriteSpawnInfo;

// Palette indices for big key sprites in light/dark world dungeons
extern const uint8 kAbsorbBigKey[2];

/*
 * DrawMultipleData - Per-tile descriptor for multi-tile sprite drawing.
 *
 * Complex sprites (bosses, multi-part enemies, large NPCs) are composed of
 * multiple OAM tiles. Each entry describes one tile's offset from the
 * sprite's origin, its character/palette flags, and size extension bit.
 */
typedef struct DrawMultipleData {
  int8 x, y;           // Pixel offset from sprite origin (signed for centering)
  uint16 char_flags;    // SNES OAM: low byte = tile index, high byte = attr flags
  uint8 ext;            // Extended OAM bit: 0 = 8x8 tile, 2 = 16x16 tile
} DrawMultipleData;


/*
 * Return flags from Sprite_CheckDamageFromLink indicating how the
 * damage check resolved. Used to distinguish carry-based collisions
 * (Link holding a throwable) from direct weapon hits.
 */
enum {
  kCheckDamageFromPlayer_Carry = 1,  // Damage caused by a carried/thrown item
  kCheckDamageFromPlayer_Ne = 2,     // Damage caused by a direct weapon hit
};

// --- OAM Helper Inlines ---
// These write a single OAM entry (4 main bytes + 1 extended byte).
// The SNES OAM holds 128 entries; bytewise_extended_oam stores the 9th
// X-bit and size flag that the hardware packs into a separate table.

/*
 * SetOamHelper0 - Write an OAM entry with Y-coordinate clamping.
 * Clamps sprites that would wrap past the bottom of the visible area
 * to Y=0xF0, which the SNES PPU treats as "off-screen below."
 */
static inline void SetOamHelper0(OamEnt *oam, uint16 x, uint16 y, uint8 charnum, uint8 flags, uint8 big) {
  oam->x = x;
  oam->y = (uint16)(y + 0x10) < 0x100 ? y : 0xf0;
  oam->charnum = charnum;
  oam->flags = flags;
  bytewise_extended_oam[oam - oam_buf] = big | (x >> 8 & 1);
}

/*
 * SetOamHelper1 - Write an OAM entry without Y clamping.
 * Used when the caller has already validated or intentionally
 * allows off-screen Y values (e.g., partially visible sprites).
 */
static inline void SetOamHelper1(OamEnt *oam, uint16 x, uint8 y, uint8 charnum, uint8 flags, uint8 big) {
  oam->x = x;
  oam->y = y;
  oam->charnum = charnum;
  oam->flags = flags;
  bytewise_extended_oam[oam - oam_buf] = big | (x >> 8 & 1);
}

/*
 * SetOamPlain - Write an OAM entry with 8-bit X (no 9th-bit extraction).
 * For sprites known to be fully within the 0-255 X range, avoiding the
 * overhead of masking the high bit from a 16-bit X coordinate.
 */
static inline void SetOamPlain(OamEnt *oam, uint8 x, uint8 y, uint8 charnum, uint8 flags, uint8 big) {
  oam->x = x;
  oam->y = y;
  oam->charnum = charnum;
  oam->flags = flags;
  bytewise_extended_oam[oam - oam_buf] = big;
}



// --- Lookup Tables ---

// Sound effect IDs played when Link absorbs each collectible item type
extern const uint8 kAbsorptionSfx[15];
// Base bump damage for each sprite type (243 types); determines contact damage
extern const uint8 kSpriteInit_BumpDamage[243];
// 256-entry sine table (Q8.8 fixed-point) used for circular motion patterns
extern const uint16 kSinusLookupTable[256];
// OAM flags (palette, priority, flip) for each throwable scenery variant
extern const uint8 kThrowableScenery_Flags[9];
// OAM attribute flags for the Wish Pond fairy queen animation frames
extern const uint8 kWishPond2_OamFlags[76];

// ============================================================================
// Sprite Position and Movement
// ============================================================================

// Accessors for 16-bit world coordinates (composed from hi/lo byte arrays)
uint16 Sprite_GetX(int k);
uint16 Sprite_GetY(int k);
void Sprite_SetX(int k, uint16 x);
void Sprite_SetY(int k, uint16 y);
// Smoothly interpolate sprite velocity toward a target speed (for easing)
void Sprite_ApproachTargetSpeed(int k, uint8 x, uint8 y);
// Directly add velocity offsets to sprite position
void SpriteAddXY(int k, int xv, int yv);
// Apply velocity to position on all three axes (X, Y, Z) with gravity on Z
void Sprite_MoveXYZ(int k);
// Reverse both X and Y velocity (used for wall bounces and recoil)
void Sprite_Invert_XY_Speeds(int k);

// ============================================================================
// Garnish System (Lightweight Visual Effects)
// ============================================================================
// Garnishes are small particle effects that share OAM space with sprites
// but have minimal game logic. Used for sparkles, dust, trails, and debris.

// Spawn a sparkle garnish near sprite k, up to the specified slot limit
int Sprite_SpawnSimpleSparkleGarnishEx(int k, uint16 x, uint16 y, int limit);
uint16 Garnish_GetX(int k);
uint16 Garnish_GetY(int k);
// Animate a sparkle garnish using frame counter shifted by 'shift' bits
void Garnish_SparkleCommon(int k, uint8 shift);
// Animate a dust cloud garnish with configurable animation speed
void Garnish_DustCommon(int k, uint8 shift);

// ============================================================================
// Sprite State Machine Modules (Death, Stun, Burn, Explode)
// ============================================================================
// Each sprite has a state machine index; these handle terminal/special states.

// Explosion death: sprite fragments scatter outward with particle effects
void SpriteModule_Explode(int k);
// Standard death sequence (poof animation, prize drop). second_entry skips
// the initial frame when re-entering after a prize has been queued.
void SpriteDeath_MainEx(int k, bool second_entry);
// Fire death: sprite burns with fire palette cycling before despawning
void SpriteModule_Burn(int k);
// Set sprite's hit invincibility timer to 31 frames (~0.5s at 60fps)
void Sprite_HitTimer31(int k);
// Stun state: sprite is immobilized and flashes. second_entry bypasses
// the initial stun setup for sprites re-entering mid-stun.
void SpriteStunned_MainEx(int k, bool second_entry);
// Spawn a falling prize ancilla (heart, rupee, etc.) from item table
int Ancilla_SpawnFallingPrize(uint8 item);

// ============================================================================
// Collision and Damage
// ============================================================================

// Bidirectional damage check: can this sprite hurt Link, and can Link hurt it?
bool Sprite_CheckDamageToAndFromLink(int k);
// Test sprite against tile map for wall/floor collisions; returns collision mask
uint8 Sprite_CheckTileCollision(int k);
// For segmented enemies: sync body segment position to head segment
bool Sprite_TrackBodyToHead(int k);

// ============================================================================
// Multi-Tile Drawing
// ============================================================================

// Draw a composite sprite from an array of DrawMultipleData tile descriptors
void Sprite_DrawMultiple(int k, const DrawMultipleData *src, int n, PrepOamCoordsRet *info);
// Same as above but defers to player's OAM priority (drawn behind Link)
void Sprite_DrawMultiplePlayerDeferred(int k, const DrawMultipleData *src, int n, PrepOamCoordsRet *info);
// ============================================================================
// NPC Dialogue and Messaging
// ============================================================================

// Show a message box when Link presses A facing sprite k
int Sprite_ShowSolicitedMessage(int k, uint16 msg);
// Show a message box automatically when Link touches sprite k
int Sprite_ShowMessageOnContact(int k, uint16 msg);
// Show a message immediately without any proximity/contact check
void Sprite_ShowMessageUnconditional(uint16 msg);
// Tutorial guard variant: shows message on contact with special flag handling
bool Sprite_TutorialGuard_ShowMessageOnContact(int k, uint16 msg);
// Show the minimal message overlay (no portrait, no choices)
void Sprite_ShowMessageMinimal();

// ============================================================================
// Environmental Interactions
// ============================================================================

// Apply earthquake/rumble displacement to all active sprites
void Prepare_ApplyRumbleToSprites();
// Spawn terrain debris that immediately shatters (bush/pot smash from hammer)
void Sprite_SpawnImmediatelySmashedTerrain(uint8 what, uint16 x, uint16 y);
// Spawn a liftable/throwable terrain object (pot, bush, rock, skull)
void Sprite_SpawnThrowableTerrain(uint8 what, uint16 x, uint16 y);
// Same as above but without the lift sound effect
int Sprite_SpawnThrowableTerrain_silently(uint8 what, uint16 x, uint16 y);
// Spawn the hidden item (if any) under sprite k's tile position
void Sprite_SpawnSecret(int k);

// ============================================================================
// Main Sprite Loop and Execution
// ============================================================================

// Top-level sprite update: iterates all 16 sprite slots each frame
void Sprite_Main();
// Reset OAM allocation region base pointers for a new frame
void Oam_ResetRegionBases();
// Decrement sprite timers and allocate OAM region for sprite k
void Sprite_TimersAndOam(int k);
// Compose 16-bit X/Y coordinates from hi/lo byte sprite state arrays
void Sprite_Get16BitCoords(int k);
// Run the state machine for a single sprite slot (dispatch by module state)
void Sprite_ExecuteSingle(int k);
// Handler for empty/dead sprite slots; does nothing but skip processing
void Sprite_inactiveSprite(int k);
// Falling into a pit: sprite shrinks and vanishes with a descending arc
void SpriteModule_Fall1(int k);
// Drowning in water: sprite bobs and sinks with splash particles
void SpriteModule_Drown(int k);
// Draw the "!" distress indicator above a trapped maiden or NPC
void Sprite_DrawDistress_custom(uint16 xin, uint16 yin, uint8 time);
// Check if Link is attempting to lift sprite k (permissive grab radius)
void Sprite_CheckIfLifted_permissive(int k);
// Apply earthquake displacement to all sprites within the given hit box
void Entity_ApplyRumbleToSprites(SpriteHitBox *hb);
// Zero out both X and Y velocity components
void Sprite_ZeroVelocity_XY(int k);
// If sprite is being dragged by hookshot ancilla, handle the pull movement
bool Sprite_HandleDraggingByAncilla(int k);
// Early return check: skip processing if sprite is fading out (dying)
bool Sprite_ReturnIfPhasingOut(int k);
// Test whether Link is close enough to auto-collect this item sprite
void Sprite_CheckAbsorptionByPlayer(int k);
// Execute the absorption: add item to inventory and despawn the sprite
void Sprite_HandleAbsorptionByPlayer(int k);
// Draw an absorbable item sprite; transient=true for short-lived drops
bool SpriteDraw_AbsorbableTransient(int k, bool transient);
// Draw an absorbable item using a numbered tile index (rupee amounts, etc.)
void Sprite_DrawNumberedAbsorbable(int k, int a);
// Reverse velocity on the axis that hit a wall (preserves the other axis)
void Sprite_BounceOffWall(int k);
// Negate both X and Y speed (full reversal, distinct from wall bounce)
void Sprite_InvertSpeed_XY(int k);
// Guard clauses: return true (skip processing) if sprite is inactive/paused
bool Sprite_ReturnIfInactive(int k);
bool Sprite_ReturnIfPaused(int k);
// ============================================================================
// Single-Tile and Shadow Drawing
// ============================================================================

// Draw a single 16x16 OAM tile at the sprite's screen position
void SpriteDraw_SingleLarge(int k);
// Draw a 16x16 tile using pre-computed OAM coords (skips Sprite_PrepOamCoord)
void Sprite_PrepAndDrawSingleLargeNoPrep(int k, PrepOamCoordsRet *info);
// Draw a shadow ellipse beneath sprite k with custom darkness level 'a'
void SpriteDraw_Shadow_custom(int k, PrepOamCoordsRet *info, uint8 a);
// Draw a standard shadow beneath sprite k
void SpriteDraw_Shadow(int k, PrepOamCoordsRet *oam);
// Draw a single 8x8 OAM tile (small items, particles)
void SpriteDraw_SingleSmall(int k);
// Draw a tall narrow sprite using vertically stacked 8x16 tiles
void Sprite_DrawThinAndTall(int k);

// ============================================================================
// Carry, Stun, and Throw Mechanics
// ============================================================================

// State handler for sprites currently being carried over Link's head
void SpriteModule_Carried(int k);
// Check if the player released the carry button to throw sprite k
void CarriedSprite_CheckForThrow(int k);
// State handler for stunned sprites (flashing, immobilized, timer countdown)
void SpriteModule_Stunned(int k);
// ============================================================================
// Thrown Sprite Physics and Scenery Interaction
// ============================================================================

// Handle a thrown sprite's collision with tiles and other sprites
void ThrownSprite_TileAndSpriteInteraction(int k);
// Post-throw cleanup: revert sprite type after impact
void Sprite_Func8(int k);
// Process thrown sprite sliding after landing
void Sprite_Func22(int k);
// Full interaction pass for throwable scenery (pots, skulls, rocks)
void ThrowableScenery_InteractWithSpritesAndTiles(int k);
// Check if a thrown sprite damages any active enemy sprites
void ThrownSprite_CheckDamageToSprites(int k);
// Check thrown sprite k for damage against a single target sprite j
void ThrownSprite_CheckDamageToSingleSprite(int k, int j);
// Apply a ricochet bounce when a thrown sprite hits a wall
void Sprite_ApplyRicochet(int k);
// Check if this throwable can transform (e.g., skulls can become enemies)
void ThrowableScenery_TransmuteIfValid(int k);
// Convert throwable into scattering debris particles
void ThrowableScenery_TransmuteToDebris(int k);
// Mark sprite for destruction on next update cycle
void Sprite_ScheduleForBreakage(int k);
// Reduce X and Y speed by half (friction for sliding/bouncing)
void Sprite_HalveSpeed_XY(int k);
// Spawn a fish that leaps out of water at sprite k's location
void Sprite_SpawnLeapingFish(int k);
// Stun recovery: check if stun timer expired and restore normal state
void SpriteStunned_Main_Func1(int k);
// "Poof" vanish animation: small cloud of smoke, then despawn
void SpriteModule_Poof(int k);
// ============================================================================
// OAM Coordinate Preparation and Tile Collision
// ============================================================================

// Convert sprite k's world coords to screen-space OAM coords
void Sprite_PrepOamCoord(int k, PrepOamCoordsRet *ret);
// Same as above but returns true (and skips drawing) if sprite is off-screen
bool Sprite_PrepOamCoordOrDoubleRet(int k, PrepOamCoordsRet *ret);
// Check tile collisions on both BG layers (BG1 and BG2)
void Sprite_CheckTileCollision2(int k);
// Check tile collision on BG1 only (for sprites on a single layer)
void Sprite_CheckTileCollisionSingleLayer(int k);
// Directional tile probe: check for solid tile horizontally ahead
void Sprite_CheckForTileInDirection_horizontal(int k, int yy);
// Directional tile probe: check for solid tile vertically ahead
void Sprite_CheckForTileInDirection_vertical(int k, int yy);
// Adjust sprite position during falling animation arc
void SpriteFall_AdjustPosition(int k);
// Check if a tile in direction yy is solid (returns true if blocked)
bool Sprite_CheckTileInDirection(int k, int yy);
// Check a specific tile property bit at the sprite's current position
bool Sprite_CheckTileProperty(int k, int j);
// Read the tile attribute byte at world coordinates (x, y) on given floor
uint8 GetTileAttribute(uint8 floor, uint16 *x, uint16 y);
// Read tile attribute for sprite k at the given position
uint8 Sprite_GetTileAttribute(int k, uint16 *x, uint16 y);
// Check if the tile at (x,y) has a slope that affects entity movement
bool Entity_CheckSlopedTileCollision(uint16 x, uint16 y);

// ============================================================================
// Per-Axis Movement Helpers
// ============================================================================

// Apply sprite velocity to position (combined X+Y, or individual axes)
void Sprite_MoveXY(int k);
void Sprite_MoveX(int k);
void Sprite_MoveY(int k);
// Apply vertical (Z-axis) velocity with gravity for arc/bounce physics
void Sprite_MoveZ(int k);
// ============================================================================
// Targeting and Directional Calculations
// ============================================================================

// Decompose a velocity magnitude into X/Y components aimed at Link
ProjectSpeedRet Sprite_ProjectSpeedTowardsLink(int k, uint8 vel);
// Set sprite k's velocity to move toward Link at the given speed
void Sprite_ApplySpeedTowardsLink(int k, uint8 vel);
// Decompose velocity toward an arbitrary world location
ProjectSpeedRet Sprite_ProjectSpeedTowardsLocation(int k, uint16 x, uint16 y, uint8 vel);
// Determine which of 4 cardinal directions sprite k should face toward Link
uint8 Sprite_DirectionToFaceLink(int k, PointU8 *coords_out);
// Relative position checks: returns pair with distance and sign flag
PairU8 Sprite_IsRightOfLink(int k);
PairU8 Sprite_IsBelowLink(int k);
PairU8 Sprite_IsRightOfLocation(int k, uint16 x);
PairU8 Sprite_IsBelowLocation(int k, uint16 y);
// 4-direction facing toward an arbitrary location
uint8 Sprite_DirectionToFaceLocation(int k, uint16 x, uint16 y);
// ============================================================================
// Combat: Damage Calculation and Application
// ============================================================================

// Soldier guards can deflect sword strikes with a parry animation
void Guard_ParrySwordAttacks(int k);
// Electrified enemies (Buzz Blob etc.) zap Link on contact
void Sprite_AttemptZapDamage(int k);
// Check if an ancilla (boomerang, arrow, etc.) damages sprite k
void Ancilla_CheckDamageToSprite_preset(int k, int a);
// Apply knockback direction after a successful hit
void Sprite_Func15(int k, int a);
// Calculate sword damage based on sword level and sprite defense
void Sprite_CalculateSwordDamage(int k);
// Apply pre-calculated damage value to sprite k's health
void Sprite_ApplyCalculatedDamage(int k, int a);
// Directly deal a fixed amount of damage with a specified hit flash timer
void Sprite_GiveDamage(int k, uint8 dmg, uint8 r0_hit_timer);
// Transform sprite k into a different type (e.g., Buzz Blob -> Cukeman)
void Sprite_Func18(int k, uint8 new_type);
// Mini Moldorm specific recoil behavior on taking damage
void Sprite_MiniMoldorm_Recoil(int k);
// Post-damage processing: check if health depleted, trigger death
void Sprite_Func3(int k);

// --- Damage-to-Link checks (various layer/proximity rules) ---
// Returns true if sprite k successfully deals damage to Link
bool Sprite_CheckDamageToLink(int k);
bool Sprite_CheckDamageToPlayer_1(int k);
// Only damages Link if sprite and Link are on the same BG layer
bool Sprite_CheckDamageToLink_same_layer(int k);
// Damages Link regardless of which layer each is on
bool Sprite_CheckDamageToLink_ignore_layer(int k);
// Set up a standard-size hit box for sprite k and test overlap with Link
bool Sprite_SetupHitBox00(int k);
// Guard clauses: skip further processing if sprite is currently lifted
bool Sprite_ReturnIfLifted(int k);
bool Sprite_ReturnIfLiftedPermissive(int k);

// --- Damage-from-Link checks ---
// Check all of Link's weapons (sword, arrows, etc.) against sprite k
uint8 Sprite_CheckDamageFromLink(int k);
// Attempt contact damage to Link with full tile collision validation
void Sprite_AttemptDamageToLinkWithCollisionCheck(int k);
// Contact damage + apply recoil knockback velocity to Link
void Sprite_AttemptDamageToLinkPlusRecoil(int k);

// --- Hit Box Setup ---
// Build Link's action hit box (depends on current weapon/item being used)
void Player_SetupActionHitBox(SpriteHitBox *hb);
// Extend Link's hit box to include the active sword swing arc
void Link_UpdateHitBoxWithSword(SpriteHitBox *hb);
// Fast path hit box test for sprite k against a pre-built hit box
void Sprite_DoHitBoxesFast(int k, SpriteHitBox *hb);
// Push Link away from sprite k at the given velocity
void Sprite_ApplyRecoilToLink(int k, uint8 vel);
// Spawn the "tink" spark effect where Link's weapon hit a hard surface
void Link_PlaceWeaponTink();
// Spawn a tink spark at sprite k's position
void Sprite_PlaceWeaponTink(int k);
// Spawn the repulsion spark when Link's shield deflects a projectile
void Sprite_PlaceRupulseSpark_2(int k);
// Build Link's hit box with conditional size based on current action
void Link_SetupHitBox_conditional(SpriteHitBox *hb);
// Build Link's standard body hit box (no weapon extension)
void Link_SetupHitBox(SpriteHitBox *hb);
// Build the bounding box for sprite k
void Sprite_SetupHitBox(int k, SpriteHitBox *hb);
// Core AABB overlap test between the two rectangles in the hit box struct
bool CheckIfHitBoxesOverlap(SpriteHitBox *hb);
// ============================================================================
// Death, Prize Drops, and Post-Death Effects
// ============================================================================

// Allocate OAM with deferred priority so player sprites render on top
void Oam_AllocateDeferToPlayer(int k);
// Main death state handler: poof animation, sound, and prize drop spawn
void SpriteModule_Die(int k);
// Inner death logic: decrement health, check for boss death, trigger poof
void Sprite_DoTheDeath(int k);
// Force a specific prize item to drop from sprite k into a specific slot
void ForcePrizeDrop(int k, uint8 prize, uint8 slot);
// Select and spawn the appropriate item drop based on sprite's drop table
void PrepareEnemyDrop(int k, uint8 item);
// Finalize death: clear sprite state and mark slot as empty
void SpriteDeath_Func4(int k);
// Draw the expanding poof cloud during death animation
void SpriteDeath_DrawPoof(int k);
// Falling into pit (variant 2): used for sprites pushed off ledges
void SpriteModule_Fall2(int k);
// Falling animation for Helmasaur/Beetle-type enemies (shell + body)
void SpriteDraw_FallingHelmaBeetle(int k);
// Falling animation for humanoid sprites (guards, NPCs)
void SpriteDraw_FallingHumanoid(int k);
// Fix up OAM extended attributes for n entries starting at sprite k's base
void Sprite_CorrectOamEntries(int k, int n, uint8 islarge);
// Guard clause: skip processing while sprite is in knockback recoil
bool Sprite_ReturnIfRecoiling(int k);
// Check if Link is in a state that blocks sprite interactions (menu, cutscene)
bool Sprite_CheckIfLinkIsBusy();
// ============================================================================
// Sprite Spawning and Room State
// ============================================================================

// Copy coordinates from a SpriteSpawnInfo struct into sprite k's state
void Sprite_SetSpawnedCoordinates(int k, SpriteSpawnInfo *info);
// Check if all enemies on the visible screen are dead (for shutter doors)
bool Sprite_CheckIfScreenIsClear();
// Check if all enemies in the entire room are dead (for prize triggers)
bool Sprite_CheckIfRoomIsClear();
// Check if all overlord-spawned enemies have been defeated
bool Sprite_CheckIfOverlordsClear();
// Set up the mirror portal sprite when warping between worlds
void Sprite_InitializeMirrorPortal();
// Clear and reinitialize all 16 sprite slots (room transition)
void Sprite_InitializeSlots();
// ============================================================================
// Garnish Execution (Visual Effect Tick Handlers)
// ============================================================================

// Process garnish slots 16-29 (higher-priority particle effects)
void Garnish_ExecuteUpperSlots();
// Process garnish slots 0-15 (lower-priority particle effects)
void Garnish_ExecuteLowerSlots();
// Run the state machine for a single garnish slot
void Garnish_ExecuteSingle(int k);
// --- Individual garnish type handlers ---
// Each implements the animation and lifetime logic for a specific effect.
void Garnish15_ArrghusSplash(int k);      // Water splash from Arrghus puffs
void Garnish13_PyramidDebris(int k);      // Falling rubble during Ganon fight
void Garnish11_WitheringGanonBatFlame(int k); // Fading flame from Ganon's bat
void Garnish10_GanonBatFlame(int k);      // Active flame on Ganon's fire bat
void Garnish0C_TrinexxIceBreath(int k);   // Ice head's frost breath particles
void Garnish14_KakKidDashDust(int k);     // Dust from Kakariko Kid's dashing
void Garnish_WaterTrail(int k);           // Water wake behind swimming sprites
void Garnish0A_CannonSmoke(int k);        // Smoke puff from wall cannons
void Garnish09_LightningTrail(int k);     // Afterimage trail for lightning bolt
// Check if a garnish particle overlaps Link (for damaging effects)
void Garnish_CheckPlayerCollision(int k, int x, int y);
void Garnish07_BabasuFlash(int k);        // Flash when Babasu firebar activates
void Garnish08_KholdstareTrail(int k);    // Ice trail behind Kholdstare
void Garnish06_ZoroTrail(int k);          // Slime trail from Zoro enemies
void Garnish12_Sparkle(int k);            // Generic sparkle (fairies, items)
void Garnish_SimpleSparkle(int k);        // Simplified single-frame sparkle
void Garnish0E_TrinexxFireBreath(int k);  // Fire head's flame breath particles
void Garnish0F_BlindLaserTrail(int k);    // Trail behind Blind's laser attack
void Garnish04_LaserTrail(int k);         // Trail for Beamos/laser eye beams
// Early return if garnish prep fails (off-screen or invalid state)
bool Garnish_ReturnIfPrepFails(int k, Point16U *pt);
void Garnish03_FallingTile(int k);        // Falling ceiling tile particle
void Garnish01_FireSnakeTail(int k);      // Trailing flame for fire snake body
void Garnish02_MothulaBeamTrail(int k);   // Energy trail from Mothula's beams
// ============================================================================
// Dungeon and Overworld Sprite Loading
// ============================================================================

// Clear all sprites in preparation for loading a new dungeon room
void Dungeon_ResetSprites();
// Save currently active sprites to cache during room transitions
void Dungeon_CacheTransSprites();
// Disable all sprite slots without clearing their data
void Sprite_DisableAll();
// Load all sprites defined in the current dungeon room's data
void Dungeon_LoadSprites();
// Manually set the "killed" flag in SRAM for underworld sprite k
void Sprite_ManuallySetDeathFlagUW(int k);
// Parse and initialize a single sprite from room data bytes
int Dungeon_LoadSingleSprite(int k, const uint8 *src);
// Parse and initialize a single overlord (sprite spawner) from room data
void Dungeon_LoadSingleOverlord(const uint8 *src);
// Full reset: clear all sprite slots, overlords, and ancillae
void Sprite_ResetAll();
// Reset sprites without disabling (preserves enable flags)
void Sprite_ResetAll_noDisable();
// Reload all overworld sprites (full reset + load from map data)
void Sprite_ReloadAll_Overworld();
// Load overworld sprites without resetting existing state
void Sprite_OverworldReloadAll_justLoad();
// Parse overworld sprite list from ROM data for the current map area
void Overworld_LoadSprites();
// Activate all proximity-triggered overworld sprites in visible range
void Sprite_ActivateAllProxima();
// Check each inactive sprite to see if Link is close enough to activate it
void Sprite_ProximityActivation();
// Activate sprite if Link is within standard proximity radius
void Sprite_ActivateWhenProximal();
// Activate sprite if Link is within an extended proximity radius (bosses)
void Sprite_ActivateWhenProximalBig();
// Load an overworld sprite at (x,y) if Link has moved close enough
void Sprite_Overworld_ProximityMotivatedLoad(uint16 x, uint16 y);
// Load a proxima sprite if it hasn't been killed in this session
void Overworld_LoadProximaSpriteIfAlive(uint16 blk);
// ============================================================================
// Miscellaneous Sprite Utilities
// ============================================================================

// Spawn explosion particle ancilla during sprite death
void SpriteExplode_SpawnEA(int k);
// Kill all sprites flagged as "friend" (used during cutscene transitions)
void Sprite_KillFriends();
// Debris particles when a thrown item shatters on impact
void Garnish16_ThrownItemDebris(int k);
// Draw the scattering debris fragments at the given position
void ScatterDebris_Draw(int k, Point16U pt);
// Immediately kill sprite k (set state to inactive, clear slot)
void Sprite_KillSelf(int k);

// ============================================================================
// Sprite Initialization and Property Loading
// ============================================================================

// Load damage, health, and behavior properties from ROM tables for sprite k
void SpritePrep_LoadProperties(int k);
// Load the correct sprite palette based on type and world state
void SpritePrep_LoadPalette(int k);
// Reset sprite k's properties to defaults (used during type transformation)
void SpritePrep_ResetProperties(int k);
// ============================================================================
// OAM Region Allocation
// ============================================================================
// The 128 OAM entries are divided into priority regions (A-F) so that
// sprites at different priority levels never overwrite each other.
// Each function allocates 'num' consecutive OAM entries from its region.

uint8 Oam_AllocateFromRegionA(uint8 num);  // Highest priority (Link, HUD)
uint8 Oam_AllocateFromRegionB(uint8 num);  // High priority (boss sprites)
uint8 Oam_AllocateFromRegionC(uint8 num);  // Mid-high priority
uint8 Oam_AllocateFromRegionD(uint8 num);  // Medium priority (enemies)
uint8 Oam_AllocateFromRegionE(uint8 num);  // Low priority (items, effects)
uint8 Oam_AllocateFromRegionF(uint8 num);  // Lowest priority (shadows, BG)
// Get the raw OAM buffer offset for 'num' entries at base index 'y'
uint8 Oam_GetBufferPosition(uint8 num, uint8 y);
// Cancel any active hookshot drag on all sprites
void Sprite_NullifyHookshotDrag();
// Swap overworld secret items for dark world variants when appropriate
void Overworld_SubstituteAlternateSecret();
// Apply conveyor belt tile velocity to sprite k (direction index j)
void Sprite_ApplyConveyor(int k, int j);
// Bounce sprite off a wall detected by tile collision; returns collision dir
uint8 Sprite_BounceFromTileCollision(int k);
// Re-activate sprites that were cached during a room transition
void ExecuteCachedSprites();
// Restore a single cached sprite to active state and run its update
void UncacheAndExecuteSprite(int k);
// Convert X/Y velocity pair to a 0-255 angle value (for projectile aiming)
uint8 Sprite_ConvertVelocityToAngle(uint8 x, uint8 y);
// Spawn a new sprite of type 'what' at the position in 'info'; returns slot
int Sprite_SpawnDynamically(int k, uint8 what, SpriteSpawnInfo *info);
// Same as above but starts searching from slot j (for multi-spawn batches)
int Sprite_SpawnDynamicallyEx(int k, uint8 what, SpriteSpawnInfo *info, int j);
// Draw a sprite during its falling-into-pit animation
void SpriteFall_Draw(int k, PrepOamCoordsRet *info);
// Spawn a sparkle garnish with a slot count limit to prevent overflow
void Sprite_GarnishSpawn_Sparkle_limited(int k, uint16 x, uint16 y);
// Spawn a sparkle garnish without slot limiting
int Sprite_GarnishSpawn_Sparkle(int k, uint16 x, uint16 y);
// Make sprite k act as an impassable barrier (blocks Link's movement)
void Sprite_BehaveAsBarrier(int k);
// Zero velocity on all active sprites (used during room transitions)
void Sprite_HaltAllMovement();
// Release a fairy from a bottle (returns ancilla slot, or -1 if no bottle)
int ReleaseFairy();
// Draw water ripple effect beneath sprite k if it is standing in water
void Sprite_DrawRippleIfInWater(int k);
