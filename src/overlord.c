/*
 * overlord.c — Enemy Spawner / Coordinator System
 *
 * Part of the Zelda 3 C reimplementation (A Link to the Past).
 *
 * Overlords are invisible room-level entities that orchestrate enemy spawns,
 * environmental hazards, and boss choreography. Up to 8 overlords can be
 * active simultaneously (indices 0-7). Each overlord has a type code (1-26)
 * that selects its behavior function. Unlike sprites, overlords have no
 * visual representation — they exist solely as spawn controllers and
 * coordinators.
 *
 * Key systems managed here:
 *   - Cannon ball spawners (full-room and vertical)
 *   - Falling Stalfos / Red Stalfos trap triggers
 *   - Wallmaster, Wizzrobe, Zoro, Pirogusu, and Blob periodic spawners
 *   - Armos Knight 6-knight circular rotation choreography
 *   - Crumbling floor tile path sequencers
 *   - Moving floor hazards and pot traps
 *   - Overworld boulder rain spawner
 *
 * Related files:
 *   - overlord.h: type declarations and extern function prototypes
 *   - sprite.h / sprite_main.h: sprite spawn and lifecycle functions
 *   - misc.h: SFX panning and random number generation
 */

/* Overlord system and sprite subsystem headers */
#include "overlord.h"
#include "sprite.h"
#include "misc.h"
#include "sprite_main.h"

/* Reconstruct a 16-bit X coordinate from split hi/lo byte arrays */
uint16 Overlord_GetX(int k) { return (overlord_x_lo[k] | overlord_x_hi[k] << 8); }
/* Reconstruct a 16-bit Y coordinate from split hi/lo byte arrays */
uint16 Overlord_GetY(int k) { return (overlord_y_lo[k] | overlord_y_hi[k] << 8); }

/*
 * Master dispatch table mapping overlord type codes (1-26) to handler
 * functions. Index 0 corresponds to type 1 (PositionTarget). Some entries
 * share the same handler: types 10-15 all use FallingSquare with different
 * path data offsets, types 16-19 all use PirogusuSpawner_left with different
 * directional indices, and type 26 reuses BadSwitchSnake (spawns bombs
 * instead of snakes).
 */
static HandlerFuncK *const kOverlordFuncs[26] = {
  &Overlord01_PositionTarget,
  &Overlord02_FullRoomCannons,
  &Overlord03_VerticalCannon,
  &Overlord_StalfosFactory,
  &Overlord05_FallingStalfos,
  &Overlord06_BadSwitchSnake,
  &Overlord07_MovingFloor,
  &Overlord08_BlobSpawner,
  &Overlord09_WallmasterSpawner,
  &Overlord0A_FallingSquare,
  &Overlord0A_FallingSquare,
  &Overlord0A_FallingSquare,
  &Overlord0A_FallingSquare,
  &Overlord0A_FallingSquare,
  &Overlord0A_FallingSquare,
  &Overlord10_PirogusuSpawner_left,
  &Overlord10_PirogusuSpawner_left,
  &Overlord10_PirogusuSpawner_left,
  &Overlord10_PirogusuSpawner_left,
  &Overlord14_TileRoom,
  &Overlord15_WizzrobeSpawner,
  &Overlord16_ZoroSpawner,
  &Overlord17_PotTrap,
  &Overlord18_InvisibleStalfos,
  &Overlord19_ArmosCoordinator_bounce,
  &Overlord06_BadSwitchSnake,
};
/* Forward declarations for Armos Knight circular math helpers */
static inline uint8 ArmosMult(uint16 a, uint8 b);
static inline int8 ArmosSin(uint16 a, uint8 b);

/* Unused overlord type — Stalfos factory. Kept as a dead-code assertion. */
void Overlord_StalfosFactory(int k) {
  // unused
  assert(0);
}

/*
 * Overlord_SetX — Store a 16-bit X coordinate into the split hi/lo arrays.
 * @k: overlord slot index (0-7)
 * @v: full 16-bit world X coordinate
 */
void Overlord_SetX(int k, uint16 v) {
  overlord_x_lo[k] = v;
  overlord_x_hi[k] = v >> 8;
}

/*
 * Overlord_SetY — Store a 16-bit Y coordinate into the split hi/lo arrays.
 * @k: overlord slot index (0-7)
 * @v: full 16-bit world Y coordinate
 */
void Overlord_SetY(int k, uint16 v) {
  overlord_y_lo[k] = v;
  overlord_y_hi[k] = v >> 8;
}

/*
 * ArmosMult — Fixed-point multiply used by the Armos Knight rotation system.
 * Multiplies a sine table value (0-255) by the orbit radius, returning
 * a single-byte result with rounding. If a >= 256 the sine is at maximum
 * magnitude so the radius is returned directly.
 * @a: sine lookup value (0-511 range; values >= 256 saturate)
 * @b: orbit radius in pixels
 * @return: (a * b) >> 8, rounded to nearest
 */
static inline uint8 ArmosMult(uint16 a, uint8 b) {
  if (a >= 256)
    return b;
  int p = a * b;
  /* Shift down by 8 with rounding: add bit 7 (0.5) before truncating */
  return (p >> 8) + (p >> 7 & 1);
}

/*
 * ArmosSin — Signed sine evaluation for Armos Knight circular motion.
 * Uses a 256-entry quarter-wave lookup table. Bit 8 of the angle
 * determines the sign (second half of the sine wave is negative).
 * @a: 9-bit angle (0-511), where 512 = full circle
 * @b: orbit radius in pixels
 * @return: signed displacement along one axis
 */
static inline int8 ArmosSin(uint16 a, uint8 b) {
  uint8 t = ArmosMult(kSinusLookupTable[a & 0xff], b);
  return (a & 0x100) ? -t : t;
}

/*
 * Overlord_SpawnBoulder — Periodically spawns falling boulders on Death
 * Mountain overworld screens. Boulders only appear outdoors when the
 * boulder-rain flag (byte_7E0FFD) is set. The frame counter modulo 64
 * throttles spawn rate to roughly once per second. Boulders appear at
 * a random X position within the visible screen and just above the
 * top edge (Y offset -0x30).
 */
void Overlord_SpawnBoulder() {  // 89b714
  /* Skip if indoors, boulder rain disabled, game paused, or not on the
   * 64th frame tick (only spawn every 64 frames) */
  if (player_is_indoors || !byte_7E0FFD || (submodule_index | flag_unk1) || ++byte_7E0FFE & 63)
    return;

  /* Only spawn if camera has scrolled far enough south relative to the
   * sprite collision base — prevents boulders on northern screens */
  if (sign8((BG2VOFS_copy2 >> 8) - (sprcoll_y_base >> 8) - 2))
    return;

  SpriteSpawnInfo info;
  /* Sprite type 0xC2 = falling boulder */
  int j = Sprite_SpawnDynamically(0, 0xc2, &info);
  if (j >= 0) {
    /* Random X within a 127-pixel band starting 64 pixels from left edge */
    Sprite_SetX(j, BG2HOFS_copy2 + (GetRandomNumber() & 127) + 64);
    /* Spawn above the visible screen so the boulder falls into view */
    Sprite_SetY(j, BG2VOFS_copy2 - 0x30);
    sprite_floor[j] = 0;
    sprite_D[j] = 0;
    sprite_z[j] = 0;
  }
}

/*
 * Overlord_Main — Top-level entry point called once per game frame.
 * Runs all active overlords, then checks if boulder spawning is needed.
 */
void Overlord_Main() {  // 89b773
  Overlord_ExecuteAll();
  Overlord_SpawnBoulder();
}

/*
 * Overlord_ExecuteAll — Iterates all 8 overlord slots (7 down to 0) and
 * dispatches active ones. Skips execution entirely if the game is in a
 * submodule transition or flag_unk1 is set (e.g., screen scroll lock).
 */
void Overlord_ExecuteAll() {  // 89b77e
  if (submodule_index | flag_unk1)
    return;
  for (int i = 7; i >= 0; i--) {
    if (overlord_type[i])
      Overlord_ExecuteSingle(i);
  }
}

/*
 * Overlord_ExecuteSingle — Dispatch a single overlord by its type code.
 * First checks whether the overlord is still within active range (for
 * overworld overlords), then calls the appropriate handler from the
 * dispatch table. Type codes are 1-based, so subtract 1 for the table index.
 * @k: overlord slot index (0-7)
 */
void Overlord_ExecuteSingle(int k) {  // 89b793
  int j = overlord_type[k];
  Overlord_CheckIfActive(k);
  kOverlordFuncs[j - 1](k);
}

/*
 * Overlord19_ArmosCoordinator_bounce — Choreographs the 6 Armos Knights
 * boss fight in the Eastern Palace. This overlord is the "director" that
 * moves the knights through an 8-phase state machine:
 *   0: Wait for player to activate the knights
 *   1: Wait until all knights are in "coerced" (formation) mode
 *   2,4: Timed rotation transitions
 *   3: Contract the orbit radius (knights spiral inward)
 *   5: Expand the orbit radius (knights spiral outward)
 *   6: Send knights to fixed positions along the back wall
 *   7: Cascade knights forward, then restart the cycle
 *
 * The coordinator repurposes overlord array slots for the rotation state:
 *   overlord_x_lo[0..1]: 16-bit rotation angle (incremented each frame)
 *   overlord_x_lo[2]: orbit radius
 *   overlord_x_lo[k]: center X of the formation
 *   overlord_floor[k]: angular velocity (signed, negated each cycle)
 *
 * @k: overlord slot index for the coordinator itself
 */
void Overlord19_ArmosCoordinator_bounce(int k) {  // 89b7dc
  /* Fixed X positions for 6 knights when lined up along the back wall */
  static const uint8 kArmosCoordinator_BackWallX[6] = { 49, 77, 105, 131, 159, 187 };

  /* Global countdown timer — decrements every frame */
  if (overlord_gen2[k])
    overlord_gen2[k]--;
  switch (overlord_gen1[k]) {
  case 0:  // wait for knight activation
    if (sprite_A[0]) {
      overlord_x_lo[k] = 120;       /* Formation center X */
      overlord_floor[k] = 255;      /* Initial angular velocity (fast CW) */
      overlord_x_lo[2] = 64;        /* Initial orbit radius */
      overlord_x_lo[0] = 192;       /* Initial rotation angle (low byte) */
      overlord_x_lo[1] = 1;         /* Initial rotation angle (high byte) */
      ArmosCoordinator_RotateKnights(k);
    }
    break;
  case 1:  // wait knight under coercion
    if (ArmosCoordinator_CheckKnights()) {
      overlord_gen1[k]++;
      overlord_gen2[k] = 0xff;
    }
    break;
  case 2:  // timed rotate then transition
  case 4:
    ArmosCoordinator_RotateKnights(k);
    break;
  case 3:  // radial contraction
    if (--overlord_x_lo[2] == 32) {
      overlord_gen1[k]++;
      overlord_gen2[k] = 64;
    }
    ArmosCoordinator_Rotate(k);
    break;
  case 5:  // radial dilation
    if (++overlord_x_lo[2] == 64) {
      overlord_gen1[k]++;
      overlord_gen2[k] = 64;
    }
    ArmosCoordinator_Rotate(k);
    break;
  case 6:  // order knights to back wall
    if (overlord_gen2[k])
      return;
    ArmosCoordinator_DisableCoercion(k);
    for (int j = 5; j >= 0; j--) {
      overlord_x_hi[j] = kArmosCoordinator_BackWallX[j];
      overlord_gen2[j] = 48;
    }
    overlord_gen1[k]++;
    overlord_gen2[k] = 255;
    break;
  case 7:  // cascade knights to front wall
    if (overlord_gen2[k])
      return;
    for (int j = 5; j >= 0; j--) {
      if (++overlord_gen2[j] == 192) {
        overlord_gen1[k] = 1;
        overlord_floor[k] = -overlord_floor[k];
        ArmosCoordinator_DisableCoercion(k);
        ArmosCoordinator_Rotate(k);
        return;
      }
    }
    break;
  }
}

/*
 * Overlord18_InvisibleStalfos — Red Stalfos ambush trigger (type 24).
 *
 * Sits dormant until the player walks within a 48x48 pixel box centered
 * on the trap location, then spawns four sprite type 0xA7 (red Stalfos)
 * at the four cardinal directions around Link with staggered activation
 * delays (0x30, 0x50, 0x70, 0x90 frames) so they drop in sequence
 * rather than all at once. The overlord clears its own type slot once
 * the spawn fires so it never re-triggers.
 *
 * The X/Y offset tables address indices [0..3]:
 *   0: north (Y -40)
 *   1: south (Y +56)
 *   2: west  (X -48)
 *   3: east  (X +48)
 *
 * @k: overlord slot index
 */
void Overlord18_InvisibleStalfos(int k) {  // 89b7f5
  static const int8 kRedStalfosTrap_X[4] = { 0, 0, -48, 48 };
  static const int8 kRedStalfosTrap_Y[4] = { -40, 56, 8, 8 };
  static const uint8 kRedStalfosTrap_Delay[4] = { 0x30, 0x50, 0x70, 0x90 };

  uint16 x = (overlord_x_lo[k] | overlord_x_hi[k] << 8);
  uint16 y = (overlord_y_lo[k] | overlord_y_hi[k] << 8);
  /* Trigger box: the unsigned wrap (x - link_x + 24 < 48) is the
   * standard "Link is within +/- 24 pixels of x" test that handles
   * negative deltas without a signed comparison. */
  if ((uint16)(x - link_x_coord + 24) >= 48 || (uint16)(y - link_y_coord + 24) >= 48)
    return;
  overlord_type[k] = 0;
  tmp_counter = 3;
  /* Spawn loop walks the four directions backwards. Each spawn fails
   * gracefully (early return) if the sprite slot pool is exhausted. */
  do {
    SpriteSpawnInfo info;
    int j = Sprite_SpawnDynamicallyEx(k, 0xa7, &info, 12);
    if (j < 0)
      return;
    Sprite_SetX(j, link_x_coord + kRedStalfosTrap_X[tmp_counter]);
    Sprite_SetY(j, link_y_coord + kRedStalfosTrap_Y[tmp_counter]);
    sprite_delay_main[j] = kRedStalfosTrap_Delay[tmp_counter];
    sprite_floor[j] = overlord_floor[k];
    sprite_E[j] = 1;
    sprite_flags2[j] = 3;
    sprite_D[j] = 2;
  } while (!sign8(--tmp_counter));
}

/*
 * Overlord17_PotTrap — Pot trap proximity arming flag (type 23).
 *
 * Watches a 64x64 pixel area around its own location. When Link enters
 * that box, the overlord disarms itself and increments byte_7E0B9E,
 * which acts as a global "trap-armed" flag read by the falling-Stalfos
 * overlord (Overlord05) to start its countdown. This is the ALttP
 * mechanic where stepping on the wrong floor switch in a trap room
 * arms the surrounding falling-enemy traps.
 */
void Overlord17_PotTrap(int k) {  // 89b884
  uint16 x = (overlord_x_lo[k] | overlord_x_hi[k] << 8);
  uint16 y = (overlord_y_lo[k] | overlord_y_hi[k] << 8);
  /* +/-32 pixel proximity check via the unsigned-wrap idiom. */
  if ((uint16)(x - link_x_coord + 32) < 64 &&
      (uint16)(y - link_y_coord + 32) < 64) {
    overlord_type[k] = 0;
    byte_7E0B9E++;
  }
}

/*
 * Overlord16_ZoroSpawner — Zoro (water-jet) periodic spawner (type 22).
 *
 * Surfaces a Zoro (sprite type 0x9C) out of the water tile at the
 * overlord's location. Two gates throttle the spawn:
 *   1. The tile under the spawn point must currently be tile-attribute
 *      0x82 (water surface). If the player drained the room or moved
 *      onto a non-water tile the spawner stays silent.
 *   2. The spawn timer (overlord_gen2) must be in the lower window
 *      (< 0x18) and only every 4th frame within that window.
 *
 * The X jitter table provides 8 small horizontal offsets so successive
 * Zoros do not overlap exactly.
 */
void Overlord16_ZoroSpawner(int k) {  // 89b8d1
  static const int8 kOverlordZoroFactory_X[8] = { -4, -2, 0, 2, 4, 6, 8, 12 };
  overlord_gen2[k]--;
  uint16 x = Overlord_GetX(k) + 8;
  uint16 y = Overlord_GetY(k) + 8;
  /* Confirm the tile at the spawn point is still water (attr 0x82). */
  if (GetTileAttribute(overlord_floor[k], &x, y) != 0x82)
    return;
  /* Window + 1-in-4 throttle: only fire when the timer's low bits are
   * 0 and the high portion has dropped below 0x18. */
  if (overlord_gen2[k] >= 0x18 || (overlord_gen2[k] & 3) != 0)
    return;
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamicallyEx(k, 0x9c, &info, 12);
  if (j >= 0) {
    Sprite_SetX(j, info.r5_overlord_x + kOverlordZoroFactory_X[GetRandomNumber() & 7] + 8);
    sprite_y_lo[j] = info.r7_overlord_y + 8;
    sprite_y_hi[j] = info.r7_overlord_y >> 8;
    sprite_floor[j] = overlord_floor[k];
    sprite_flags4[j] = 1;
    sprite_E[j] = 1;
    sprite_ignore_projectile[j] = 1;
    sprite_y_vel[j] = 16;
    sprite_flags2[j] = 32;
    sprite_oam_flags[j] = 13;
    sprite_subtype2[j] = GetRandomNumber();
    sprite_delay_main[j] = 48;
    sprite_bump_damage[j] = 3;
  }
}

/*
 * Overlord15_WizzrobeSpawner — Wizzrobe ambush spawner (type 21).
 *
 * Counts down on every other frame (half-rate) until the timer hits
 * 128, then spawns four Wizzrobes (sprite type 0x9B) clustered around
 * Link's position with staggered delays so each one teleports in 16
 * frames after the previous. After the spawn the timer reverts to 127
 * to immediately re-enter the countdown phase.
 *
 * Spawn offsets:
 *   [0] east  (+48 X, +16 Y)
 *   [1] west  (-48 X, +16 Y)
 *   [2] south (   0, +64)
 *   [3] north (   0, -32)
 */
void Overlord15_WizzrobeSpawner(int k) {  // 89b986
  static const int8 kOverlordWizzrobe_X[4] = { 48, -48, 0, 0 };
  static const int8 kOverlordWizzrobe_Y[4] = { 16, 16, 64, -32 };
  static const uint8 kOverlordWizzrobe_Delay[4] = { 0, 16, 32, 48 };
  /* Cooldown phase: tick the timer down at half speed (only on odd
   * frames). The spawn fires when the timer hits the magic value 128. */
  if (overlord_gen2[k] != 128) {
    if (frame_counter & 1)
      overlord_gen2[k]--;
    return;
  }
  overlord_gen2[k] = 127;
  for (int i = 3; i >= 0; i--) {
    SpriteSpawnInfo info;
    int j = Sprite_SpawnDynamicallyEx(k, 0x9b, &info, 12);
    if (j >= 0) {
      Sprite_SetX(j, link_x_coord + kOverlordWizzrobe_X[i]);
      Sprite_SetY(j, link_y_coord + kOverlordWizzrobe_Y[i]);
      sprite_delay_main[j] = kOverlordWizzrobe_Delay[i];
      sprite_floor[j] = overlord_floor[k];
      sprite_B[j] = 1;
    }
  }
  tmp_counter = 0xff;
}

/*
 * Overlord14_TileRoom — Animated flying-tile attack sequencer (type 20).
 *
 * Drives the iconic ALttP "tiles fly off the floor" puzzle/trap room.
 * Sequentially spawns 22 flying tiles using a precomputed X/Y trajectory
 * table (see TileRoom_SpawnTile). The overlord:
 *   1. Verifies it is on the visible screen (BG2 scroll alignment check).
 *   2. Counts down overlord_gen2 each frame; spawns when it hits 0x80.
 *   3. Resets the cooldown to 0xE0 between tiles. After the 22nd tile
 *      the overlord clears its own type slot.
 *   4. If a sprite slot was unavailable (j < 0), reschedules instead of
 *      consuming the next tile in the sequence.
 */
void Overlord14_TileRoom(int k) {  // 89b9e8
  uint16 x = (overlord_x_lo[k] | overlord_x_hi[k] << 8) - BG2HOFS_copy2;
  uint16 y = (overlord_y_lo[k] | overlord_y_hi[k] << 8) - BG2VOFS_copy2;
  /* Off-screen guard: the high byte of (world - scroll) must be zero,
   * meaning the result fits inside the 256-pixel viewport. */
  if (x & 0xff00 || y & 0xff00)
    return;
  if (--overlord_gen2[k] != 0x80)
    return;
  int j = TileRoom_SpawnTile(k);
  if (j < 0) {
    /* Sprite slot pool full; bump the timer back up by 1 so the spawn
     * is retried next frame without advancing the sequence index. */
    overlord_gen2[k] = 0x81;
    return;
  }
  if (++overlord_gen1[k] != 22)
    overlord_gen2[k] = 0xE0;
  else
    overlord_type[k] = 0;
}

/*
 * TileRoom_SpawnTile — Spawn the next flying-tile in the sequence.
 *
 * The X/Y tables encode 22 starting positions that produce the
 * symmetric inward-spiral pattern characteristic of the tile-room
 * trap. The sequence index is read from overlord_gen1[k]. The high
 * byte of the position is inherited from the overlord so the tiles
 * appear on the same screen quadrant the overlord lives on.
 *
 * @k: overlord slot index
 * @return: spawned sprite slot (0..15) or negative on failure
 *
 * Note: sprite_health is written twice (4, then 0). Preserved as-is —
 * matches the original ROM behavior.
 */
int TileRoom_SpawnTile(int k) {  // 89ba56
  static const uint8 kSpawnFlyingTile_X[22] = {
    0x70, 0x80, 0x60, 0x90, 0x90, 0x60, 0x70, 0x80, 0x80, 0x70, 0x50, 0xa0, 0xa0, 0x50, 0x50, 0xa0,
    0xa0, 0x50, 0x70, 0x80, 0x80, 0x70,
  };
  static const uint8 kSpawnFlyingTile_Y[22] = {
    0x80, 0x80, 0x70, 0x90, 0x70, 0x90, 0x60, 0xa0, 0x60, 0xa0, 0x60, 0xb0, 0x60, 0xb0, 0x80, 0x90,
    0x80, 0x90, 0x70, 0x90, 0x70, 0x90,
  };
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(k, 0x94, &info);
  if (j < 0)
    return j;
  sprite_E[j] = 1;
  int i = overlord_gen1[k];
  sprite_x_lo[j] = kSpawnFlyingTile_X[i];
  sprite_y_lo[j] = kSpawnFlyingTile_Y[i] - 8;
  sprite_y_hi[j] = overlord_y_hi[k];
  sprite_x_hi[j] = overlord_x_hi[k];
  sprite_floor[j] = overlord_floor[k];
  sprite_health[j] = 4;
  sprite_flags5[j] = 0;
  sprite_health[j] = 0;
  sprite_defl_bits[j] = 8;
  sprite_flags2[j] = 4;
  sprite_oam_flags[j] = 1;
  sprite_bump_damage[j] = 4;
  return j;
}

/*
 * Overlord10_PirogusuSpawner_left — Pirogusu (water snake) directional
 * spawner. Shared handler for overlord types 16-19.
 *
 * The "tmp_counter" derived from (overlord_type - 16) selects which
 * direction the spawned Pirogusu travels:
 *   0: left, 1: right, 2: up, 3: down
 *
 * The kOverlordPirogusu_A table maps that direction into the sprite's
 * `sprite_A` parameter (the sprite uses the inverse mapping internally).
 *
 * Spawn rate: ticks down each frame; on hitting 128, schedules the
 * next spawn for a randomized 96..127 frames out and emits one
 * Pirogusu — but only if fewer than 5 are already alive (population
 * cap to keep the screen sane).
 */
void Overlord10_PirogusuSpawner_left(int k) {  // 89baac
  static const uint8 kOverlordPirogusu_A[4] = { 2, 3, 0, 1 };

  tmp_counter = overlord_type[k] - 16;
  if (overlord_gen2[k] != 128) {
    overlord_gen2[k]--;
    return;
  }
  /* Re-arm: schedule next spawn between 96 and 127 frames out. */
  overlord_gen2[k] = (GetRandomNumber() & 31) + 96;
  /* Count currently-alive Pirogusu (sprite type 0x10) and cap at 4. */
  int n = 0;
  for (int i = 0; i != 16; i++) {
    if (sprite_state[i] != 0 && sprite_type[i] == 0x10)
      n++;
  }
  if (n >= 5)
    return;
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamicallyEx(k, 0x94, &info, 12);
  if (j >= 0) {
    Sprite_SetX(j, info.r5_overlord_x);
    Sprite_SetY(j, info.r7_overlord_y);
    sprite_floor[j] = overlord_floor[k];
    sprite_delay_main[j] = 32;
    sprite_D[j] = tmp_counter;
    sprite_A[j] = kOverlordPirogusu_A[tmp_counter];
  }
}

/*
 * Overlord0A_FallingSquare — Crumbling-floor path runner.
 *
 * Shared handler for overlord types 10-15 (selected by overlord_type - 10).
 * Each variant traces a different precomputed path of tile-collapse
 * directions through the kCrumbleTilePathData script. Six paths are
 * stored back-to-back in a single byte array, indexed via
 * kCrumbleTilePathOffs[0..6].
 *
 * Direction codes (kCrumbleTilePath_X/Y):
 *   0: east  (+16,   0)
 *   1: west  (-16,   0)
 *   2: south (  0, +16)
 *   3: north (  0, -16)
 *   0xff: special "warp" to the next path segment via large offsets.
 *
 * State machine:
 *   - overlord_gen2 = countdown between tile-spawns (16 frames each).
 *   - overlord_gen3 = "active on screen" latch; the path only
 *     advances once the head has scrolled into view.
 *   - overlord_gen1 = current step within this variant's path.
 *
 * On every active step the function spawns one debris garnish at the
 * head position via SpawnFallingTile, then advances the head to the
 * next tile in the script. When the path is exhausted the overlord
 * clears its own type slot.
 */
void Overlord0A_FallingSquare(int k) {  // 89bbb2
  static const uint8 kCrumbleTilePathData[108 + 1] = {
    2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3,
    3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 3, 1, 3, 0, 3, 1,
    3, 0, 3, 1, 3, 0, 3, 1, 3, 0, 3, 1, 3, 0, 3, 1,
    3, 0, 3, 1, 3, 0, 3, 1, 3, 0, 3, 1, 3, 0, 3, 1,
    3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0xff
  };
  static const uint8 kCrumbleTilePathOffs[7] = {
    0, 25, 66, 77, 87, 98, 108
  };
  static const int8 kCrumbleTilePath_X[4] = { 16, -16, 0, 0 };
  static const int8 kCrumbleTilePath_Y[4] = { 0, 0, 16, -16 };
  /* Cooldown branch: still waiting for the next tile spawn. */
  if (overlord_gen2[k]) {
    if (overlord_gen3[k]) {
      overlord_gen2[k]--;
      return;
    }
    /* Activation gate: do not start ticking until the head position
     * has scrolled into the visible viewport. */
    uint16 x = Overlord_GetX(k) - BG2HOFS_copy2;
    uint16 y = Overlord_GetY(k) - BG2VOFS_copy2;
    if (!(x & 0xff00 || y & 0xff00))
      overlord_gen3[k]++;
    return;
  }

  overlord_gen2[k] = 16;
  SpawnFallingTile(k);
  int j = overlord_type[k] - 10;
  int i = overlord_gen1[k]++;
  /* End-of-path check: this variant's slice is fully consumed. */
  if (i == kCrumbleTilePathOffs[j + 1] - kCrumbleTilePathOffs[j]) {
    overlord_type[k] = 0;
  }
  int t = kCrumbleTilePathData[kCrumbleTilePathOffs[j] + i];
  if (t == 0xff) {
    /* Path-warp opcode: jump the head by these magic 16-bit deltas
     * (negative-as-uint values implement an upward/leftward warp). */
    Overlord_SetX(k, Overlord_GetX(k) + 0xc1a);
    Overlord_SetY(k, Overlord_GetY(k) + 0xbb66);
  } else {
    Overlord_SetX(k, Overlord_GetX(k) + kCrumbleTilePath_X[t]);
    Overlord_SetY(k, Overlord_GetY(k) + kCrumbleTilePath_Y[t]);
  }
}

/*
 * SpawnFallingTile — Allocate a "falling tile debris" garnish and
 * place it at the overlord's head position with Y +16 (one tile down,
 * so the visual lines up with the floor that just collapsed).
 *
 * Garnishes are short-lived visual effects (no AI, just animation +
 * countdown). Type 3 is the crumble-tile dust cloud. The countdown of
 * 31 frames matches the animation length; garnish_active = 31 is the
 * shared "garnish system in use" indicator.
 *
 * Audio: queues the falling-rock SFX (channel 1) with pan derived
 * from the X coordinate so the sound localizes to the screen side
 * where the tile fell.
 */
void SpawnFallingTile(int k) {  // 89bc31
  int j = GarnishAlloc();
  if (j >= 0) {
    garnish_type[j] = 3;
    garnish_x_hi[j] = overlord_x_hi[k];
    garnish_x_lo[j] = overlord_x_lo[k];
    sound_effect_1 = CalculateSfxPan_Arbitrary(garnish_x_lo[j]) | 0x1f;
    int y = Overlord_GetY(k) + 16;
    garnish_y_lo[j] = y;
    garnish_y_hi[j] = y >> 8;
    garnish_countdown[j] = 31;
    garnish_active = 31;
  }
}

/*
 * Overlord09_WallmasterSpawner — Wallmaster ambush spawner (type 9).
 *
 * Half-rate countdown (decrements only on even frames) until the
 * timer hits 128, then spawns one Wallmaster (sprite type 0x90)
 * directly above Link's current position with z = 208 (high in the
 * air so it descends onto him). The spawn re-arms the timer at 127
 * so the cycle restarts immediately.
 *
 * Inherits Link's lower-level flag so the Wallmaster lives on the
 * same floor layer the player does — important for two-level rooms
 * (e.g., Hyrule Castle bottom of stairs).
 */
void Overlord09_WallmasterSpawner(int k) {  // 89bc7b
  if (overlord_gen2[k] != 128) {
    if (!(frame_counter & 1))
      overlord_gen2[k]--;
    return;
  }
  overlord_gen2[k] = 127;
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamicallyEx(k, 0x90, &info, 12);
  if (j < 0)
    return;
  Sprite_SetX(j, link_x_coord);
  Sprite_SetY(j, link_y_coord);
  sprite_z[j] = 208;
  SpriteSfx_QueueSfx2WithPan(j, 0x20);
  sprite_floor[j] = link_is_on_lower_level;
}

/*
 * Overlord08_BlobSpawner — Blob (Zol) drop-from-ceiling spawner (type 8).
 *
 * 0xa0-frame cooldown between spawns. Population-capped at 4
 * concurrent Blobs (sprite type 0x8f). The drop position is biased
 * by Link's facing direction via kOverlordZol_X/Y so the Zol always
 * lands "in front of" him — north drop for facing-up, south for
 * facing-down, etc. Spawned at z = 192 so it falls a clean 12 px.
 *
 * sprite_head_dir is seeded with a 5-bit random plus 16 (to avoid
 * 0..15 which has different semantics in the Zol AI), giving each
 * Zol a slightly different bounce trajectory.
 */
void Overlord08_BlobSpawner(int k) {  // 89bcc3
  if (overlord_gen2[k]) {
    overlord_gen2[k]--;
    return;
  }
  overlord_gen2[k] = 0xa0;
  int n = 0;
  for (int i = 0; i != 16; i++) {
    if (sprite_state[i] != 0 && sprite_type[i] == 0x8f)
      n++;
  }
  if (n >= 5)
    return;

  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamicallyEx(k, 0x8f, &info, 12);
  if (j >= 0) {
    static const int8 kOverlordZol_X[4] = { 0, 0, -48, 48 };
    static const int8 kOverlordZol_Y[4] = { -40, 56, 8, 8 };
    int i = link_direction_facing >> 1;
    Sprite_SetX(j, link_x_coord + kOverlordZol_X[i]);
    Sprite_SetY(j, link_y_coord + kOverlordZol_Y[i]);
    sprite_z[j] = 192;
    sprite_floor[j] = link_is_on_lower_level;
    sprite_ai_state[j] = 2;
    sprite_E[j] = 2;
    sprite_C[j] = 2;
    sprite_head_dir[j] = GetRandomNumber() & 31 | 16;
  }
}

/*
 * Overlord07_MovingFloor — Moving-floor hazard controller (type 7).
 *
 * Two-phase oscillator that drives the dungeon-floor sliding effect
 * (the conveyor floors in Skull Woods, etc.).
 *   Phase 0 (idle): Count up to 32 frames in gen2. On rollover, pick
 *     a new direction by random nibble: overlord_x_lo non-zero allows
 *     all four directions (mask 3); zero restricts to two (mask 1).
 *     The result is doubled because dung_floor_move_flags is bit-
 *     packed in steps of 2. Then schedule 128..255 frames of motion
 *     and switch to phase 1.
 *   Phase 1 (moving): Tick the motion timer down; on hitting 0 return
 *     to idle. dung_floor_move_flags == 1 is the rest state, so the
 *     floor stops sliding when the timer expires.
 *
 * Self-terminates when sprite slot 0 is in state 4 (boss death) so
 * the overlord doesn't keep running after a room clears.
 */
void Overlord07_MovingFloor(int k) {  // 89bd3f
  if (sprite_state[0] == 4) {
    overlord_type[k] = 0;
    BYTE(dung_floor_move_flags) = 1;
    return;
  }
  if (!overlord_gen1[k]) {
    if (++overlord_gen2[k] == 32) {
      overlord_gen2[k] = 0;
      BYTE(dung_floor_move_flags) = (GetRandomNumber() & (overlord_x_lo[k] ? 3 : 1)) * 2;
      overlord_gen2[k] = (GetRandomNumber() & 127) + 128;
      overlord_gen1[k]++;
    } else {
      BYTE(dung_floor_move_flags) = 1;
    }
  } else {
    if (!--overlord_gen2[k])
      overlord_gen1[k] = 0;
  }
}

/*
 * Sprite_Overlord_PlayFallingSfx — Helper that queues SFX 0x20 (the
 * generic "enemy lands" thud) on channel 2 with X-position pan.
 * Called by both Overlord05 and the Wallmaster spawner.
 */
void Sprite_Overlord_PlayFallingSfx(int k) {  // 89bdfd
  SpriteSfx_QueueSfx2WithPan(k, 0x20);
}

/*
 * Overlord05_FallingStalfos — Falling Stalfos trap drop (type 5).
 *
 * Each of the 8 overlord slots gets a different trigger threshold
 * (kStalfosTrap_Trigger), so when multiple Falling-Stalfos overlords
 * exist in the same room they drop in a staggered sequence rather
 * than simultaneously. The slot index k directly indexes the table,
 * giving k=0 the longest delay (255) and k=7 the shortest (32).
 *
 * State machine:
 *   gen1 == 0: dormant. Wait for byte_7E0B9E (the global trap-armed
 *              flag set by Overlord17_PotTrap). When triggered,
 *              advance to phase 1.
 *   gen1 > 0:  count up. When the per-slot trigger value is reached,
 *              spawn one Stalfos (sprite type 0x85) at z = 224 (high
 *              ceiling drop), play the falling SFX, and clear the
 *              overlord type so it doesn't re-fire.
 *
 * Off-screen guard at the top exits early so the overlord doesn't
 * spawn a Stalfos onto a screen the player isn't looking at.
 */
void Overlord05_FallingStalfos(int k) {  // 89be0f
  static const uint8 kStalfosTrap_Trigger[8] = { 255, 224, 192, 160, 128, 96, 64, 32 };

  uint16 x = (overlord_x_lo[k] | overlord_x_hi[k] << 8) - BG2HOFS_copy2;
  uint16 y = (overlord_y_lo[k] | overlord_y_hi[k] << 8) - BG2VOFS_copy2;
  if (x & 0xff00 || y & 0xff00)
    return;
  if (overlord_gen1[k] == 0) {
    if (byte_7E0B9E)
      overlord_gen1[k]++;
    return;
  }
  if (overlord_gen1[k]++ == kStalfosTrap_Trigger[k]) {
    overlord_type[k] = 0;
    SpriteSpawnInfo info;
    int j = Sprite_SpawnDynamicallyEx(k, 0x85, &info, 12);
    if (j < 0)
      return;
    Sprite_SetX(j, info.r5_overlord_x);
    Sprite_SetY(j, info.r7_overlord_y);
    sprite_z[j] = 224;
    sprite_floor[j] = overlord_floor[k];
    sprite_D[j] = 0; // zelda bug: unitialized
    Sprite_Overlord_PlayFallingSfx(j);
  }
}

/*
 * Overlord06_BadSwitchSnake — "Bad-switch" punishment spawner.
 * Shared handler for type 6 (snake drop) and type 26 (bomb drop —
 * note the dispatch table aliases entry 25 back to this function).
 *
 * Triggered when activate_bomb_trap_overlord is set (the player hit
 * a wrong switch). Each of the 8 slots has a different per-slot
 * trigger frame (kSnakeTrapOverlord_Tab1, evenly spaced 16 frames
 * apart) so a row of these overlords fires in a wave, not all at
 * once.
 *
 * On trigger, spawns a snake (sprite type 0x6e) at z = 192 (sprite_E
 * mirrors z so it falls correctly), sets flags3 bit 0x10 (no shadow),
 * and plays the standard fall SFX.
 *
 * Special case for type 26: immediately retypes the spawned sprite
 * to 74 (a bomb), invokes Sprite_TransmuteToBomb to repaint OAM, and
 * sets delay_aux1 to 112 so the bomb explodes ~2 seconds after
 * landing. This is how the bomb-drop trap rooms are implemented.
 */
void Overlord06_BadSwitchSnake(int k) {  // 89be75
  static const uint8 kSnakeTrapOverlord_Tab1[8] = { 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90 };

  uint8 a = overlord_gen1[k];
  if (a == 0) {
    if (activate_bomb_trap_overlord != 0)
      overlord_gen1[k] = 1;
    return;
  }
  overlord_gen1[k] = a + 1;

  if (a != kSnakeTrapOverlord_Tab1[k])
    return;

  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(k, 0x6e, &info);
  if (j < 0)
    return;
  Sprite_SetX(j, info.r5_overlord_x);
  Sprite_SetY(j, info.r7_overlord_y);

  sprite_z[j] = 192;
  sprite_E[j] = 192;

  sprite_flags3[j] |= 0x10;
  sprite_floor[j] = overlord_floor[k];
  SpriteSfx_QueueSfx2WithPan(j, 0x20);
  uint8 type = overlord_type[k];
  overlord_type[k] = 0;
  if (type == 26) {
    sprite_type[j] = 74;
    Sprite_TransmuteToBomb(j);
    sprite_delay_aux1[j] = 112;
  }
}

/*
 * Overlord02_FullRoomCannons — All-walls cannon-ball spawner (type 2).
 *
 * On every 16th frame (frame_counter & 0xf == 0), picks one of 16
 * spawn slots uniformly at random. The three parallel tables encode:
 *   _Idx[16]: which direction the ball travels (0=up, 1=right,
 *             2=down, 3=left — fed into Overlord_SpawnCannonBall via
 *             tmp_counter).
 *   _X[16]:   spawn X within the room (0/64/96/144/176/240).
 *   _Y[16]:   spawn Y within the room.
 *
 * The 16 slots are arranged 4 per wall: top wall fires down, right
 * wall fires left, bottom wall fires up, left wall fires right. So
 * the table groups 0-3 are top-wall slots, 4-7 right, 8-11 bottom,
 * 12-15 left.
 *
 * The high bytes (byte_7E0FB0 / byte_7E0FB1 + 1) encode the active
 * room's screen-relative origin so the spawn lands on the actual
 * walls, not at world origin.
 *
 * Off-screen guard at the top exits early so cannon rooms in
 * adjacent screens don't fire while the player is elsewhere.
 */
void Overlord02_FullRoomCannons(int k) {  // 89bf09
  static const uint8 kAllDirectionMetalBallFactory_Idx[16] = { 2, 2, 2, 2, 1, 1, 1, 1, 3, 3, 3, 3, 0, 0, 0, 0 };
  static const uint8 kAllDirectionMetalBallFactory_X[16] = { 64, 96, 144, 176, 240, 240, 240, 240, 176, 144, 96, 64, 0, 0, 0, 0 };
  static const uint8 kAllDirectionMetalBallFactory_Y[16] = { 16, 16, 16, 16, 64, 96, 160, 192, 240, 240, 240, 240, 192, 160, 96, 64 };
  uint16 x = (overlord_x_lo[k] | overlord_x_hi[k] << 8) - BG2HOFS_copy2;
  uint16 y = (overlord_y_lo[k] | overlord_y_hi[k] << 8) - BG2VOFS_copy2;
  if ((x | y) & 0xff00 || frame_counter & 0xf)
    return;

  byte_7E0FB6 = 0;
  int j = GetRandomNumber() & 15;
  tmp_counter = kAllDirectionMetalBallFactory_Idx[j];
  overlord_x_lo[k] = kAllDirectionMetalBallFactory_X[j];
  overlord_x_hi[k] = byte_7E0FB0;
  overlord_y_lo[k] = kAllDirectionMetalBallFactory_Y[j];
  overlord_y_hi[k] = byte_7E0FB1 + 1;
  Overlord_SpawnCannonBall(k, 0);
}

/*
 * Overlord03_VerticalCannon — Single-direction (downward) cannon
 * spawner (type 3). Fires a metal ball straight down every 56 frames
 * (gen1 cooldown).
 *
 * gen2 is a long-cycle counter: when it reaches 0, the next shot
 * becomes a "tracking" ball — byte_7E0FB6 is set to 160 so
 * Overlord_SpawnCannonBall configures the spawned ball with sprite
 * AI state 160 (homing), and the X jitter is fixed at +8 instead of
 * randomized. Then gen2 resets to 160 frames before the next
 * tracking shot.
 *
 * Normal shots get a randomized 0/16 X offset (mask 2 picks between
 * the two values, multiplied by 8) so the cannon doesn't hit the
 * exact same column every time.
 *
 * If the overlord is off-screen, gen2 is force-set to 255 so the
 * tracking-shot timer doesn't progress while invisible.
 */
void Overlord03_VerticalCannon(int k) {  // 89bf5b
  uint16 x = (overlord_x_lo[k] | overlord_x_hi[k] << 8) - BG2HOFS_copy2;
  if (x & 0xff00) {
    overlord_gen2[k] = 255;
    return;
  }
  if (!(frame_counter & 1) && overlord_gen2[k])
    overlord_gen2[k]--;
  tmp_counter = 2;
  byte_7E0FB6 = 0;
  if (!sign8(--overlord_gen1[k]))
    return;
  overlord_gen1[k] = 56;
  int xd;
  if (!overlord_gen2[k]) {
    overlord_gen2[k] = 160;
    byte_7E0FB6 = 160;
    xd = 8;
  } else {
    xd = (GetRandomNumber() & 2) * 8;
  }
  Overlord_SpawnCannonBall(k, xd);
}

/*
 * Overlord_SpawnCannonBall — Shared cannonball-spawn helper used by
 * both Overlord02 (full-room) and Overlord03 (vertical) cannons.
 *
 * `tmp_counter` selects the direction (0=up, 1=right, 2=down, 3=left)
 * via the parallel velocity tables; +/-24 is the cannonball speed.
 * `xd` is an extra X offset added by the caller so vertical cannons
 * can jitter their column without affecting other directions.
 *
 * If byte_7E0FB6 is non-zero, the spawned ball gets a "homing" AI
 * state injected into sprite_ai_state, plus flags2/flags4 markers
 * that switch the cannonball sprite into its tracking variant.
 *
 * sprite_delay_aux2 = 64 is a 1-second self-destruct timer so balls
 * that miss everything despawn before clogging the sprite pool.
 */
void Overlord_SpawnCannonBall(int k, int xd) {  // 89bfaf
  static const int8 kOverlordSpawnBall_Xvel[4] = { 24, -24, 0, 0 };
  static const int8 kOverlordSpawnBall_Yvel[4] = { 0, 0, 24, -24 };
  SpriteSpawnInfo info;
  int j = Sprite_SpawnDynamically(k, 0x50, &info);
  if (j < 0)
    return;

  Sprite_SetX(j, info.r5_overlord_x + xd);
  Sprite_SetY(j, info.r7_overlord_y - 1);

  sprite_x_vel[j] = kOverlordSpawnBall_Xvel[tmp_counter];
  sprite_y_vel[j] = kOverlordSpawnBall_Yvel[tmp_counter];
  sprite_floor[j] = overlord_floor[k];
  if (byte_7E0FB6) {
    sprite_ai_state[j] = byte_7E0FB6;
    sprite_y_lo[j] = sprite_y_lo[j] + 8;
    sprite_flags2[j] = 3;
    sprite_flags4[j] = 9;
  }
  sprite_delay_aux2[j] = 64;
  SpriteSfx_QueueSfx3WithPan(j, 0x7);
}

/*
 * Overlord01_PositionTarget — Trivial "I am here" beacon (type 1).
 * Latches its own slot index into byte_7E0FDE so other code (e.g.,
 * boulder logic, sprite AI) can ask "where is the position-target
 * overlord on this screen?" without scanning the overlord array.
 */
void Overlord01_PositionTarget(int k) {  // 89c01e
  byte_7E0FDE = k;
}

/*
 * Overlord_CheckIfActive — Per-frame on/off-screen culling for
 * overworld overlords. Computes whether the overlord is within the
 * active range relative to BG2 scroll, alternating the offset
 * between two values (0x130 and -0x40) every frame for hysteresis.
 *
 * If both axes' high bits indicate the overlord is outside the
 * active range, the overlord is killed (type cleared) and the
 * matching bit in overworld_sprite_was_loaded is cleared so the
 * area-spawn system can re-load it later if Link returns.
 *
 * Indoor rooms exit early — overlord lifetime indoors is managed
 * by the dungeon-room load/unload pipeline, not proximity.
 */
void Overlord_CheckIfActive(int k) {  // 89c08d
  static const int16 kOverlordInRangeOffs[2] = { 0x130, -0x40 };
  if (player_is_indoors)
    return;
  int j = frame_counter & 1;
  uint16 x = BG2HOFS_copy2 + kOverlordInRangeOffs[j] - (overlord_x_lo[k] | overlord_x_hi[k] << 8);
  uint16 y = BG2VOFS_copy2 + kOverlordInRangeOffs[j] - (overlord_y_lo[k] | overlord_y_hi[k] << 8);
  if ((x >> 15) != j || (y >> 15) != j) {
    overlord_type[k] = 0;
    uint16 blk = overlord_offset_sprite_pos[k];
    if (blk != 0xffff) {
      uint8 loadedmask = (0x80 >> (blk & 7));
      overworld_sprite_was_loaded[blk >> 3] &= ~loadedmask;
    }
  }
}

/*
 * ArmosCoordinator_RotateKnights — Wrapper that advances the phase
 * counter (gen1) when the per-phase timer (gen2) expires, then runs
 * one rotation step. Used by phases 0/2/4 which need automatic phase
 * progression on a timer.
 */
void ArmosCoordinator_RotateKnights(int k) {  // 9deccc
  if (!overlord_gen2[k])
    overlord_gen1[k]++;
  ArmosCoordinator_Rotate(k);
}

/*
 * ArmosCoordinator_Rotate — Compute one frame of the 6-knight
 * circular formation.
 *
 * The shared rotation angle lives in WORD(overlord_x_lo[0]) and is
 * advanced each frame by overlord_floor[k] (the signed angular
 * velocity). kArmosCoordinator_Tab0 supplies the six 60-degree
 * phase offsets (0, 85, 170, 255, 340, 425 — note the table is
 * reversed because the loop walks i = 0..5 but the offsets are
 * stored knight-5-first).
 *
 * For each knight i:
 *   tx = center_x + sin(angle + offset)              -> high byte
 *        of position lives in overlord_x_hi/y_hi pair.
 *   ty = center_y + sin(angle + offset + 0x80)       -> +0x80 is a
 *        90-degree phase shift, so this is effectively cos().
 *
 * The radius (size) is read from overlord_x_lo[2] which the parent
 * state machine adjusts during contraction/dilation phases.
 */
void ArmosCoordinator_Rotate(int k) {  // 9decd4
  static const uint16 kArmosCoordinator_Tab0[6] = { 0, 425, 340, 255, 170, 85 };

  WORD(overlord_x_lo[0]) += (int8)overlord_floor[k];
  for (int i = 0; i != 6; i++) {
    int t0 = WORD(overlord_x_lo[0]) + kArmosCoordinator_Tab0[i];
    uint8 size = overlord_x_lo[2];
    int tx = (overlord_x_lo[k] | overlord_x_hi[k] << 8) + ArmosSin(t0, size);
    overlord_x_hi[i] = tx;
    overlord_y_hi[i] = tx >> 8;
    int ty = (overlord_y_lo[k] | overlord_y_hi[k] << 8) + ArmosSin(t0 + 0x80, size);
    overlord_gen2[i] = ty;
    overlord_floor[i] = ty >> 8;
  }
  tmp_counter = 6;
}

/*
 * ArmosCoordinator_CheckKnights — Returns true once every still-alive
 * Armos Knight has reached the "coerced" formation state
 * (sprite_ai_state == 0 means they've been kicked into formation
 * mode by the coordinator). Walks the 6 knight slots backwards.
 */
bool ArmosCoordinator_CheckKnights() {  // 9dedb8
  for (int j = 5; j >= 0; j--) {
    if (sprite_state[j] && sprite_ai_state[j] == 0)
      return false;
  }
  return true;
}

/*
 * ArmosCoordinator_DisableCoercion — Releases all 6 knights from
 * coordinator control by clearing their AI states. Called between
 * phases so each knight runs its own AI for the cascade-to-back-wall
 * and front-wall transitions.
 */
void ArmosCoordinator_DisableCoercion(int k) {  // 9dedcb
  for (int j = 5; j >= 0; j--)
    sprite_ai_state[j] = 0;
}

