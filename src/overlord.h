/*
 * overlord.h - Enemy spawner and coordinator subsystem ("Overlords")
 *
 * In the original SNES ROM, "overlords" are invisible meta-entities that
 * control enemy spawning patterns and environmental hazards within dungeon
 * rooms. Unlike regular sprites, overlords do not have on-screen graphics;
 * instead they act as factories and coordinators that create and manage
 * groups of visible sprites according to scripted rules.
 *
 * Each overlord occupies a slot (index k) and runs its type-specific logic
 * every frame via Overlord_ExecuteSingle(). Overlord types include:
 *   - Spawners: periodically create enemies at fixed or random positions
 *     (Stalfos, Zoro worms, Wizzrobes, Blobs, Wallmasters, Pirogusu)
 *   - Hazards: trigger environmental dangers (falling floor tiles, moving
 *     floors, cannon barrages, pot traps, falling boulders)
 *   - Coordinators: orchestrate multi-sprite formations (Armos Knights'
 *     synchronized rotation pattern in the Eastern Palace boss fight)
 *
 * The overlord system is separate from the main sprite table and uses its
 * own parallel arrays for position, type, and timer state.
 */
#pragma once
#include "types.h"

/* ---------------------------------------------------------------------------
 * Overlord lifecycle and core dispatch
 * --------------------------------------------------------------------------- */

// Main entry point called once per frame from the dungeon module.
// Iterates all active overlord slots and runs their update logic.
void Overlord_Main();

// Iterates through every overlord slot, calling ExecuteSingle for each active one.
void Overlord_ExecuteAll();

// Dispatches the type-specific handler for overlord in slot |k| based on its
// overlord_type[] value. Acts as the per-slot switch/jump table.
void Overlord_ExecuteSingle(int k);

// Checks whether overlord |k| is still within the active screen region.
// Deactivates off-screen overlords to save processing time.
void Overlord_CheckIfActive(int k);

/* ---------------------------------------------------------------------------
 * Overlord position helpers
 * --------------------------------------------------------------------------- */

// Sets the X coordinate of overlord |k| to |v| in the overlord position array.
void Overlord_SetX(int k, uint16 v);

// Sets the Y coordinate of overlord |k| to |v| in the overlord position array.
void Overlord_SetY(int k, uint16 v);

/* ---------------------------------------------------------------------------
 * Enemy spawner overlords
 *
 * These overlords periodically spawn enemy sprites into the room. Each uses
 * timers and spatial checks to control spawn rate and placement so the room
 * maintains the intended difficulty level.
 * --------------------------------------------------------------------------- */

// Spawns Stalfos (skeleton enemies) at timed intervals from overlord |k|.
void Overlord_StalfosFactory(int k);

// Spawns Stalfos that drop from above (used in certain dungeon traps).
void Overlord05_FallingStalfos(int k);

// Spawns invisible Stalfos that materialize near Link. Used in dark rooms
// where enemies appear suddenly to ambush the player.
void Overlord18_InvisibleStalfos(int k);

// Spawns Zoro worms from wall cracks at timed intervals.
void Overlord16_ZoroSpawner(int k);

// Spawns Wizzrobe mages that teleport in and shoot magic projectiles.
void Overlord15_WizzrobeSpawner(int k);

// Spawns Pirogusu (flying fish enemies) from the left side of the screen.
void Overlord10_PirogusuSpawner_left(int k);

// Spawns Wallmasters (giant hands that grab Link and return him to the
// dungeon entrance) when Link lingers in a room too long.
void Overlord09_WallmasterSpawner(int k);

// Spawns Blob (Zol) enemies that split into smaller Gels when defeated.
void Overlord08_BlobSpawner(int k);

/* ---------------------------------------------------------------------------
 * Environmental hazard overlords
 * --------------------------------------------------------------------------- */

// Spawns a boulder that rolls across the overworld (Death Mountain).
void Overlord_SpawnBoulder();

// Animates a pot that flies at Link as a room trap when he enters.
void Overlord17_PotTrap(int k);

// Manages a tile room where floor tiles peel up and fly at Link one by one.
void Overlord14_TileRoom(int k);

// Spawns a single animated floor tile projectile from a tile room overlord.
// Returns the sprite slot index of the spawned tile, or -1 on failure.
int TileRoom_SpawnTile(int k);

// Manages the falling floor square hazard where sections of floor collapse
// beneath Link's feet in sequence (e.g., Ganon's Tower).
void Overlord0A_FallingSquare(int k);

// Spawns a single falling tile within a falling floor sequence.
void SpawnFallingTile(int k);

// Controls the moving floor conveyor belt that pushes Link in a direction.
void Overlord07_MovingFloor(int k);

// Plays the sound effect for a tile or object falling, shared across
// multiple overlord types that produce falling hazards.
void Sprite_Overlord_PlayFallingSfx(int k);

// Controls a snake enemy linked to a bad (incorrect) crystal switch state,
// spawning when the player activates the wrong switch color.
void Overlord06_BadSwitchSnake(int k);

/* ---------------------------------------------------------------------------
 * Cannon overlords
 * --------------------------------------------------------------------------- */

// Fires cannon balls from all four walls of a room simultaneously.
void Overlord02_FullRoomCannons(int k);

// Fires cannon balls from a single vertical (wall-mounted) cannon.
void Overlord03_VerticalCannon(int k);

// Spawns a cannon ball projectile from overlord |k| with X-direction |xd|.
// |xd| determines whether the ball travels left or right.
void Overlord_SpawnCannonBall(int k, int xd);

/* ---------------------------------------------------------------------------
 * Target and positioning
 * --------------------------------------------------------------------------- */

// Computes and updates the target position for overlord |k|, used by
// spawners that aim projectiles or enemies toward Link's current location.
void Overlord01_PositionTarget(int k);

/* ---------------------------------------------------------------------------
 * Armos Knights coordinator
 *
 * The Armos Knights are the Eastern Palace boss -- six statues that attack
 * in a synchronized rotating formation. The coordinator overlord manages
 * their collective movement pattern rather than each sprite acting alone.
 * --------------------------------------------------------------------------- */

// Updates the Armos Knights' bounce movement during their coordinated attack.
void Overlord19_ArmosCoordinator_bounce(int k);

// Advances the circular rotation of all Armos Knights around their center point.
void ArmosCoordinator_RotateKnights(int k);

// Applies rotation math to update positions for the Armos formation in slot |k|.
void ArmosCoordinator_Rotate(int k);

// Returns true if all Armos Knights are still alive. When one is defeated,
// the formation breaks and each knight attacks independently.
bool ArmosCoordinator_CheckKnights();

// Disables the coordinated movement coercion for overlord |k|, allowing
// individual Armos Knights to move freely after the formation breaks.
void ArmosCoordinator_DisableCoercion(int k);
