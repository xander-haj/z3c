/*
 * src/player.c — Link's player-state machine, movement, and item logic.
 *
 * Part of the Zelda 3 C reimplementation (A Link to the Past).
 *
 * This is the largest single source file in the project. It owns
 * everything about the player character that is not strictly OAM
 * rendering (which lives in player_oam.c):
 *
 *   - The 31-state player-handler dispatch (kPlayerHandlers) covering
 *     ground walking, swimming, recoiling, spin-attacking, riding
 *     stairs, hookshot, lava-zap, ether-cast, mirror-warp, AsleepInBed,
 *     pre-bunny / post-bunny states, etc.
 *   - The "Default" ground-state submachine which is itself a giant
 *     state machine handling pickup, lift, throw, dash, charge, sword
 *     swing, item-in-hand mode, peg hammer, pull lever, etc.
 *   - Movement integrators: Link_HandleVelocity (the per-frame Z/Y/X
 *     update), Link_HandleMovingAnimation (animation step driver),
 *     stair / wall / pit / ladder collision response.
 *   - Camera control: Link_DragCameraTowardsLink and friends.
 *   - Item-use entry points: Link_ResetSomeWeaponState, lift/throw
 *     state machines, sword charge/swing, hookshot deploy/retract.
 *   - Ancilla bridges: arrows, boomerang, bombs, hookshot — Link's
 *     code spawns the ancilla (see ancilla.c) and tracks whether
 *     Link can re-fire by reading ancilla state back.
 *   - Damage/death: contact damage, recoil bounce, fairy revival,
 *     game-over transition.
 *
 * Conventions used throughout this file:
 *   - link_x_coord / link_y_coord are 16-bit world coordinates.
 *     link_z_coord is the height-above-ground (0 == on ground;
 *     positive == in the air; 0xff sentinel == "no Z").
 *   - link_direction_facing uses the SNES convention:
 *       0 = up, 2 = down, 4 = left, 6 = right.
 *     Many tables index by (direction >> 1) to fit 4 entries.
 *   - link_player_handler_state selects the top-level handler;
 *     submodule_index, subsubmodule_index and per-handler timers
 *     drive the inner state machines.
 *   - "ancilla" = projectile/effect (sword beam, arrow, bomb...).
 *
 * Related files:
 *   - player.h: extern declarations and shared state-id constants.
 *   - player_oam.c: per-frame OAM rendering for Link (no logic).
 *   - tile_detect.c: tile-collision lookups used by the movement code.
 *   - ancilla.c: spawns of projectiles/effects this file initiates.
 *   - tagalong.c: companion follower state, queried by lift/dash code.
 *   - dungeon.c, overworld.c: room/area-level systems Link interacts with.
 */
#include "player.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "tile_detect.h"
#include "ancilla.h"
#include "sprite.h"
#include "load_gfx.h"
#include "hud.h"
#include "overworld.h"
#include "tagalong.h"
#include "dungeon.h"
#include "misc.h"
#include "player_oam.h"
#include "sprite_main.h"

/* Reentrancy guard for the camera-follow logic. Link_HandleVelocity may
 * recurse during certain transitions; this flag short-circuits the
 * recursion to a single camera update per frame. */
static bool g_ApplyLinksMovementToCamera_called;

/* Per-frame delays for the 18-frame spin-attack charge-up animation.
 * Most frames advance immediately (0 = next frame this tick); larger
 * values pause on the dramatic poses (the held-overhead charge frame). */
static const uint8 kSpinAttackDelays[] = { 1, 0, 0, 0, 0, 3, 0, 0, 1, 0, 3, 3, 3, 3, 4, 4, 1, 5 };
/* SFX ids played when each sword tier produces a charged sword beam.
 * Indexed by sword type. The 0 at index 4 = no SFX for the unused tier. */
static const uint8 kFireBeamSounds[] = { 1, 2, 3, 4, 0, 9, 18, 27 };
/* Tagalong follower spacing tables. _1 == 2-tile follow distance for
 * the dwarf NPC, _2 == 3-tile distance for the larger guards.
 * 0xff (-1) marks unused entries; the trailing zeros pad the table to
 * the engine's expected 15-entry size. */
static const int8 kTagalongArr1[] = { -1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int8 kTagalongArr2[] = { -1, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
/* Spin-attack pose frame indices, 12 frames per facing direction
 * (4 directions = 48 entries). Each row encodes the 12-frame rotation
 * sequence Link's body cycles through while the sword spin-attack is
 * actually executing. */
static const uint8 kLinkSpinGraphicsByDir[] = {
  10, 11, 10, 6, 7, 8, 9, 2, 3, 4, 5, 10, 0, 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 0, 12, 13, 12, 4, 5, 6, 7, 8, 9, 2, 3, 12, 14, 15, 14, 8, 9, 2, 3, 4, 5, 6, 7, 14
};
/* Per-frame display duration for kLinkSpinGraphicsByDir. The 5s pause
 * on the start (idx 1) and end (idx 11) of each cycle holds the
 * sword's wind-up and follow-through poses. */
static const uint8 kLinkSpinDelays[] = { 1, 5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 5 };
/* Movement-bit -> facing direction mapping for wall-grab. The four
 * one-hot input directions (1, 2, 4, 8) map to the wall on the
 * opposite side (Link grabs the wall in front of him). */
static const uint8 kGrabWallDirs[] = { 4, 8, 1, 2 };
/* 7-step animation sequence for grabbing onto a wall edge. */
static const uint8 kGrabWall_AnimSteps[] = { 0, 1, 2, 3, 1, 2, 3 };
/* Per-step duration for kGrabWall_AnimSteps. Step 3 holds longer (12
 * frames) so the "stuck to wall" pose is visible before peeking. */
static const uint8 kGrabWall_AnimTimer[] = { 0, 5, 5, 12, 5, 5, 12 };
/* Magic-cape magic-meter drain rates by cape level. Higher tier
 * capes drain slower (each entry is "frames between magic decrements"). */
static const uint8 kCapeDepletionTimers[] = { 4, 8, 8 };
/* 32-entry cyclic table that smooths Link's swim-bob to avoid
 * single-frame jitter ("judder"). Read by the swimming animation
 * pipeline to pick a sub-frame offset. */
static const int8 kAvoidJudder1[] = { 0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0, 7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7 };
/* Y-offset and velocity tables for the outdoor "hop off a ledge"
 * helper. _y selects the ground-level Y delta; _y2 the impact Y;
 * _velz the upward kick (gentler for shallow hops, stronger for
 * deep ones); _velx the horizontal speed during the arc. */
static const int8 kLink_DoMoveXCoord_Outdoors_Helper2_y[2] = { -8, 8 };
static const int8 kLink_DoMoveXCoord_Outdoors_Helper2_y2[2] = { -16, 16 };
static const uint8 kLink_DoMoveXCoord_Outdoors_Helper2_velz[8] = { 32, 32, 32, 40, 48, 56, 64, 72 };
static const uint8 kLink_DoMoveXCoord_Outdoors_Helper2_velx[8] = { 16, 28, 28, 28, 28, 28, 28, 28 };
/* Sprite ids dropped when Link lifts a 9-cell pickup pattern. Indices
 * 0..8 are the 3x3 grid: 0xFF (idx 3) marks "no drop" for the center
 * cell. The values 0x50..0x57 are the small-bush / pot drop family
 * (rupee, heart, fairy, etc.). */
static const uint8 kLink_Lift_tab[9] = { 0x54, 0x52, 0x50, 0xFF, 0x51, 0x53, 0x55, 0x56, 0x57 };
/* X/Y collision-probe tables for Link_Move_Helper6 (the 8-direction
 * tile-collision walker). _tab0/_tab1 are X/y deltas for the primary
 * probe; _tab2/_tab3 are the secondary probe used for diagonal moves. */
static const uint8 kLink_Move_Helper6_tab0[] = { 8, 8, 23, 23, 8, 23, 8, 23 };
static const uint8 kLink_Move_Helper6_tab1[] = { 0, 15, 0, 15, 0, 0, 15, 15 };
static const uint8 kLink_Move_Helper6_tab2[] = { 23, 23, 8, 8, 8, 23, 8, 23 };
static const uint8 kLink_Move_Helper6_tab3[] = { 0, 15, 0, 15, 15, 15, 0, 0 };
/* Swim-stroke animation lookups consumed by player_oam.c.
 * kSwimmingTab1: 4-frame paddle cycle index (0/1/2 cycle + idle).
 * kSwimmingTab2: per-direction stroke distance (32 px on horizontal,
 *                8 px on vertical — Link strokes farther sideways). */
const uint8 kSwimmingTab1[4] = { 2, 0, 1, 0 };
const uint8 kSwimmingTab2[2] = { 32, 8 };
/* Master player-state dispatch table. Indexed by link_player_handler_state.
 * Every frame Link_ControlHandler dispatches into one of these. The
 * order MUST match the kPlayerState_* enum in player.h.
 *
 *   0  Default          - free movement / item use / sword swing.
 *   1  Pits             - mid-fall down a pit.
 *   2  Recoil           - knock-back from damage (also slot 6).
 *   3  SpinAttack       - executing the charged sword spin (also slot 30).
 *   4  Swimming         - in water with flippers.
 *   5  OnIce             - sliding on ice surfaces.
 *   6  Recoil (alias)   - identical to slot 2.
 *   7  Zapped            - electrified by enemy or trap.
 *   8/9/10  Using Ether/Bombos/Quake - mid-spell-cast freeze.
 *   11..14 Hop variants  - overworld ledge-hop directions.
 *   15/16 0F             - placeholder/unused; aliased to LinkState_0F.
 *   17  Dashing          - Pegasus-boots dash run.
 *   18  ExitingDash      - dash deceleration / wall-bonk.
 *   19  Hookshot         - hookshot deployed and pulling Link.
 *   20  CrossingWorlds   - mid-mirror-warp animation.
 *   21  HoldItem         - holding picked-up object (pot/bush).
 *   22  Sleeping         - asleep in bed (intro).
 *   23  Bunny            - permanent dark-world bunny form.
 *   24  HoldingBigRock   - power-glove big rock pickup.
 *   25/26 Receiving      - cinematic Ether/Bombos pickup.
 *   27  ReadingTablet    - kneeling at the desert/Hera tablet.
 *   28  TemporaryBunny   - brief bunny transform (medallion drop).
 *   29  TreePull         - tugging on a Pull tree branch.
 *   30  SpinAttack alias - identical to slot 3.
 */
static PlayerHandlerFunc *const kPlayerHandlers[31] = {
  &LinkState_Default,
  &LinkState_Pits,
  &LinkState_Recoil,
  &LinkState_SpinAttack,
  &PlayerHandler_04_Swimming,
  &LinkState_OnIce,
  &LinkState_Recoil,
  &LinkState_Zapped,
  &LinkState_UsingEther,
  &LinkState_UsingBombos,
  &LinkState_UsingQuake,
  &LinkHop_HoppingSouthOW,
  &LinkState_HoppingHorizontallyOW,
  &LinkState_HoppingDiagonallyUpOW,
  &LinkState_HoppingDiagonallyDownOW,
  &LinkState_0F,
  &LinkState_0F,
  &LinkState_Dashing,
  &LinkState_ExitingDash,
  &LinkState_Hookshotting,
  &LinkState_CrossingWorlds,
  &PlayerHandler_15_HoldItem,
  &LinkState_Sleeping,
  &PlayerHandler_17_Bunny,
  &LinkState_HoldingBigRock,
  &LinkState_ReceivingEther,
  &LinkState_ReceivingBombos,
  &LinkState_ReadingDesertTablet,
  &LinkState_TemporaryBunny,
  &LinkState_TreePull,
  &LinkState_SpinAttack,
};
// forwards
/* Magic-meter cost table for every item that consumes magic. Indexed
 * by item id; the values are scaled so the half-magic upgrade halves
 * each entry at runtime (see HUD code). 9 banks of 3 (one per item
 * tier) cover the rod / cane / cape / etc. progression. */
static const uint8 kLinkItem_MagicCosts[] = { 16, 8, 4, 32, 16, 8, 8, 4, 2, 8, 4, 2, 8, 4, 2, 16, 8, 4, 4, 2, 2, 8, 4, 2, 16, 8, 4 };
/* Bombos medallion cast animation: 20 frames pairing per-frame
 * duration with a state index into Link's pose tables. The 4-frame
 * pose cycle (states 0..3) plays twice (first 8 entries) for the
 * raise-and-charge, then transitions through the unique cast poses. */
static const uint8 kBombosAnimDelays[] = { 5, 5, 5, 5, 5, 5, 5, 5, 3, 3, 3, 3, 3, 7, 1, 1, 1, 1, 1, 13 };
static const uint8 kBombosAnimStates[] = { 0, 1, 2, 3, 0, 1, 2, 3, 8, 9, 10, 11, 12, 10, 8, 13, 14, 15, 16, 17 };
/* Ether medallion cast animation, 12 frames. The "NoFlash" variant
 * stretches the final two frames to 24 each so the lightning flash
 * lasts long enough to be visible without the screen-flash overlay. */
static const uint8 kEtherAnimDelays[] = { 5, 5, 5, 5, 5, 5, 5, 5, 7, 7, 3, 3 };
static const uint8 kEtherAnimDelaysNoFlash[] = { 5, 5, 5, 5, 5, 5, 5, 5, 7, 7, 24, 24 };
static const uint8 kEtherAnimStates[] = { 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7 };
/* Quake medallion: 12 frames; unique pose ids (18..22) for the ground
 * stomp finishing poses. */
static const uint8 kQuakeAnimDelays[] = { 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 19 };
static const uint8 kQuakeAnimStates[] = { 0, 1, 2, 3, 0, 1, 2, 3, 18, 19, 20, 22 };
static inline uint8 BitSum4(uint8 t);
/* BitSum4 — Population count of the low 4 bits of `t`. Used to detect
 * how many directions are pressed at once on the d-pad (1 = cardinal,
 * 2 = diagonal, 3 = invalid triple-press). */
static inline uint8 BitSum4(uint8 t) {
  return (t & 1) + ((t >> 1) & 1) + ((t >> 2) & 1) + ((t >> 3) & 1);
}

/*
 * Dungeon_HandleLayerChange — Step Link's BG layer when he crosses an
 * in-room layer transition (descends or climbs an internal staircase
 * within the same room).
 *
 *   kind_of_in_room_staircase == 0: standard down-staircase. Bumps
 *     the dungeon_room_index by 16 to point at the lower-room slot.
 *   kind_of_in_room_staircase == 2: a one-way upper-level entry that
 *     should NOT lower the level flag.
 *   else (1): downward without room change.
 *
 * Saves the visited-quadrant bitmask so the dungeon-map system can
 * mark the new floor as explored.
 */
void Dungeon_HandleLayerChange() {  // 81ff05
  link_is_on_lower_level_mirror = 1;
  if (kind_of_in_room_staircase == 0)
    BYTE(dungeon_room_index) += 16;
  if (kind_of_in_room_staircase != 2)
    link_is_on_lower_level = 1;
  about_to_jump_off_ledge = 0;
  SetAndSaveVisitedQuadrantFlags();
}

/*
 * CacheCameraProperties — Snapshot every camera/scroll/quadrant-related
 * variable into its "_cached" mirror. Called when Link enters water or
 * other states that may force a position rollback (e.g., drowning
 * recovery), so the original camera can be restored exactly.
 */
void CacheCameraProperties() {  // 81ff28
  BG2HOFS_copy2_cached = BG2HOFS_copy2;
  BG2VOFS_copy2_cached = BG2VOFS_copy2;
  link_y_coord_cached = link_y_coord;
  link_x_coord_cached = link_x_coord;
  room_scroll_vars_y_vofs1_cached = room_bounds_y.a0;
  room_scroll_vars_y_vofs2_cached = room_bounds_y.a1;
  room_scroll_vars_x_vofs1_cached = room_bounds_x.a0;
  room_scroll_vars_x_vofs2_cached = room_bounds_x.a1;
  up_down_scroll_target_cached = up_down_scroll_target;
  up_down_scroll_target_end_cached = up_down_scroll_target_end;
  left_right_scroll_target_cached = left_right_scroll_target;
  left_right_scroll_target_end_cached = left_right_scroll_target_end;
  camera_y_coord_scroll_low_cached = camera_y_coord_scroll_low;
  camera_x_coord_scroll_low_cached = camera_x_coord_scroll_low;
  quadrant_fullsize_x_cached = quadrant_fullsize_x;
  quadrant_fullsize_y_cached = quadrant_fullsize_y;
  link_quadrant_x_cached = link_quadrant_x;
  link_quadrant_y_cached = link_quadrant_y;
  link_direction_facing_cached = link_direction_facing;
  link_is_on_lower_level_cached = link_is_on_lower_level;
  link_is_on_lower_level_mirror_cached = link_is_on_lower_level_mirror;
  is_standing_in_doorway_cahed = is_standing_in_doorway;
  dung_cur_floor_cached = dung_cur_floor;
}

/*
 * CheckAbilityToSwim — Triggered when Link enters water without
 * flippers (bunny in the dark world also disqualifies even with
 * flippers). If the player has the Moon Pearl their bunny status is
 * cleared first so a re-check on the same frame would succeed.
 *
 * On failure, sets the invisibility status (0xc) and jumps to the
 * appropriate "drowning recovery" submodule (20 indoors, 42 OW).
 */
void CheckAbilityToSwim() {  // 81ffb6
  if (!link_is_bunny_mirror && link_item_flippers)
    return;
  if (link_item_moon_pearl)
    link_is_bunny_mirror = 0;
  link_visibility_status = 0xc;
  submodule_index = player_is_indoors ? 20 : 42;
}

/*
 * Link_Main — Top-level per-frame entry point for Link. Called once
 * by the active gameplay module (typically Module09). Snapshots
 * Link's previous coordinates (used by collision deltas), clears the
 * shared "block scrolling" flag, then runs the control handler unless
 * Link is hard-frozen (cutscene immobilization). Finally ticks the
 * Somaria-block / grave-sprite spawn pipeline.
 */
void Link_Main() {  // 878000
//  RunEmulatedFunc(0x878000, 0, 0, 0, true, true, -2, 0);
//  return;



  link_x_coord_prev = link_x_coord;
  link_y_coord_prev = link_y_coord;
  flag_unk1 = 0;
  if (!flag_is_link_immobilized)
    Link_ControlHandler();
  HandleSomariaAndGraves();
}

/*
 * Link_ControlHandler — Apply pending damage, then dispatch into the
 * active player-state handler.
 *
 * Damage flow:
 *   - Cape mode (Magic Cape active) absorbs the hit: clear damage,
 *     auxiliary state, and incapacitated timer. No HP loss.
 *   - link_disable_sprite_damage suppresses damage entirely.
 *   - Otherwise: cancel an in-flight charged-sword spin attack
 *     (ancilla_type[0] == 5) so getting hit interrupts a charge,
 *     start a 58-frame damage-blink window, queue the hurt SFX,
 *     bump the death-stat counter, and subtract from current HP.
 *   - HP underflow (>= 0xa8 unsigned wrap) or hitting zero triggers
 *     death: stash TM/TS for the game-over fade, switch to module 18
 *     (game over), and zero the heart-buffer.
 *
 * Cape upkeep runs only in non-default states (the default state has
 * its own integrated cape tick); then dispatch to kPlayerHandlers.
 */
void Link_ControlHandler() {  // 87807f
  if (link_give_damage) {
    if (link_cape_mode) {
      link_give_damage = 0;
      link_auxiliary_state = 0;
      link_incapacitated_timer = 0;
    } else {
      if (!link_disable_sprite_damage) {
        uint8 dmg = link_give_damage;
        link_give_damage = 0;
        if (ancilla_type[0] == 5 && player_handler_timer == 0 && link_delay_timer_spin_attack) {
          ancilla_type[0] = 0;
          flag_for_boomerang_in_place = 0;
        }
        if (countdown_for_blink == 0)
          countdown_for_blink = 58;
        Ancilla_Sfx2_Near(38);
        number_of_times_hurt_by_sprites++;
        uint8 new_dmg = link_health_current - dmg;
        if (new_dmg == 0 || new_dmg >= 0xa8) {
          mapbak_TM = TM_copy;
          mapbak_TS = TS_copy;
          saved_module_for_menu = main_module_index;
          main_module_index = 18;
          submodule_index = 1;
          countdown_for_blink = 0;
          link_hearts_filler = 0;
          new_dmg = 0;
        }
        link_health_current = new_dmg;
      }
    }
  }
  if (link_player_handler_state)
    Player_CheckHandleCapeStuff();
  kPlayerHandlers[link_player_handler_state]();
}

/*
 * LinkState_Default — Slot 0 of kPlayerHandlers. The "free movement"
 * top-level state that handles 99% of gameplay. Runs the camera
 * cache (only outdoors), checks for a pending bunny transformation
 * (which may transition us into state 23 before any other logic),
 * then dispatches to either:
 *   - HandleLink_From1D when an auxiliary state is set (jumping,
 *     hurt, etc. — state 1D in the original ROM symbols), or
 *   - PlayerHandler_00_Ground_3 for normal walking/item use.
 */
void LinkState_Default() {  // 878109
  CacheCameraPropertiesIfOutdoors();
  if (Link_HandleBunnyTransformation()) {
    if (link_player_handler_state == 23)
      PlayerHandler_17_Bunny();
    return;
  }
  fallhole_var2 = 0;
  if (link_auxiliary_state)
    HandleLink_From1D();
  else
    PlayerHandler_00_Ground_3();
}

/*
 * HandleLink_From1D — "Auxiliary state" handler covering air-borne
 * Link: mid-jump, hookshot pull, mid-stair-hop, etc. The auxiliary
 * state is set by the originator (e.g., a sprite that knocked Link
 * off a ledge) and this routine processes it.
 *
 * Aggressively clears every "active item" flag on entry — Link can't
 * use items mid-air. Special handling for the electrocute case
 * forcibly removes the cape (so a zap interrupts cape mode) and
 * locks the spin-attack timer at 2 to defer any pending swing.
 */
void HandleLink_From1D() {  // 878130
  link_item_in_hand = 0;
  link_position_mode = 0;
  link_debug_value_1 = 0;
  link_debug_value_2 = 0;
  link_var30d = 0;
  link_var30e = 0;
  some_animation_timer_steps = 0;
  bitfield_for_a_button = 0;
  button_mask_b_y &= ~0x40;
  link_state_bits = 0;
  link_picking_throw_state = 0;
  link_grabbing_wall = 0;
  bitmask_of_dragstate = 0;
  Link_ResetSwimmingState();
  link_cant_change_direction &= ~1;
  link_z_coord &= 0xff;
  if (link_electrocute_on_touch != 0) {
    if (link_cape_mode)
      Link_ForceUnequipCape_quietly();
    Link_ResetSwordAndItemUsage();
    link_disable_sprite_damage = 1;
    player_handler_timer = 0;
    link_delay_timer_spin_attack = 2;
    link_animation_steps = 0;
    link_direction &= ~0xf;
    Ancilla_Sfx3_Near(43);
    link_player_handler_state = 7;
    LinkState_Zapped();
  } else {
    link_moving_against_diag_tile = 0;
    link_player_handler_state = 2;
    LinkState_Recoil();
  }
}

/*
 * PlayerHandler_00_Ground_3 — The "main" ground walking + item handler.
 * This is the largest single function in the engine; structured as a
 * priority cascade that decides what Link should do this frame:
 *
 *   1. Toss-in-progress (Link_HandleToss): if mid-toss of a held
 *      item, it owns the frame — return immediately.
 *   2. A-button (Link_HandleAPress): pick up / read / talk / lift.
 *      Triggers a sword-cooldown tick and possibly a Y-item swap.
 *      Mid-cooldown bug-fix gate skips the rest of the frame so
 *      medallion casts aren't aborted by a same-frame spin.
 *   3. Cape passive tick (magic drain).
 *   4. Incapacitated (recoil) — cancel item/wall/state bits and run
 *      the recoil tick. Special case: B held during recoil locks the
 *      facing direction.
 *   5. The big "free movement" branch:
 *      - If not transforming, not wall-grabbing meaningfully, no
 *        carry/lift state, and no in-hand item, AND either the sword
 *        recovery has played out or the swing has been started.
 *      - flag_moving (forced motion vector): jump straight into
 *        Link_HandleSwimMovements which doubles as the "forced
 *        slide" handler.
 *      - Otherwise resolve the joypad-direction or force_move bits
 *        into a direction, run wall-grab / dragging logic, then
 *        Link_HandleVelocity.
 *   6. Fall-through to the velocity tick + animation update.
 *
 * The `getout_clear_vel:` jumps zero out velocity before returning,
 * used by the medallion / charge-bug-fix paths to ensure Link doesn't
 * keep moving into a transition.
 */
void PlayerHandler_00_Ground_3() {  // 8781a0
  g_ApplyLinksMovementToCamera_called = false;

  link_z_coord = 0xffff;
  link_actual_vel_z = 0xff;
  link_recoilmode_timer = 0;

  if (!Link_HandleToss()) {
    Link_HandleAPress();
    if ((link_state_bits | link_grabbing_wall) == 0 && link_unk_master_sword == 0 && link_player_handler_state != kPlayerState_StartDash) {
      Link_HandleYItem();
      // Ensure we're not handling potions. Things further
      // down don't assume this and change the module indexes randomly.
      // This also fixes a bug where bombos, ether, quake get aborted if you use spin attack at the same time.
      if ((enhanced_features0 & kFeatures0_MiscBugFixes) && (
          main_module_index == 14 && submodule_index != 2 ||
          link_player_handler_state == kPlayerState_Bombos ||
          link_player_handler_state == kPlayerState_Ether ||
          link_player_handler_state == kPlayerState_Quake))
        goto getout_clear_vel;
      if (sram_progress_indicator != 0) {
        Link_HandleSwordCooldown();
        if (link_player_handler_state == 3)
          goto getout_clear_vel;
      }
    }
  }

  Link_HandleCape_passive_LiftCheck();
  if (link_incapacitated_timer) {
    link_moving_against_diag_tile = 0;
    link_var30d = 0;
    link_var30e = 0;
    some_animation_timer_steps = 0;
    bitfield_for_a_button = 0;
    link_picking_throw_state = 0;
    link_state_bits = 0;
    link_grabbing_wall = 0;
    if (!(button_mask_b_y & 0x80))
      link_cant_change_direction &= ~1;
    Link_HandleRecoilAndTimer(false);
    return;
  }

  if (link_unk_master_sword) {
    link_direction = 0;
  } else if (!link_is_transforming && (link_grabbing_wall & ~2) == 0 && (link_state_bits & 0x7f) == 0 &&
             ((link_state_bits & 0x80) == 0 || (link_picking_throw_state & 1) == 0) && !link_item_in_hand && !link_position_mode &&
             (button_b_frames >= 9 || (button_mask_b_y & 0x20) != 0 || (button_mask_b_y & 0x80) == 0)) {
    // if_4

    if (link_flag_moving) {
      swimcoll_var9[0] = swimcoll_var9[1] = 0x180;
      Link_HandleSwimMovements();
      return;
    }
    ResetAllAcceleration();
    uint8 dir;

    if ((dir = (force_move_any_direction & 0xf)) == 0) {
      if (link_grabbing_wall & 2)
        goto endif_3;
      if ((dir = (joypad1H_last & kJoypadH_AnyDir)) == 0) {
        link_x_vel = 0;
        link_y_vel = 0;
        link_direction = 0;
        link_direction_last = 0;
        link_animation_steps = 0;
        bitmask_of_dragstate &= ~0xf;
        link_timer_push_get_tired = 32;
        link_timer_jump_ledge = 19;
        goto endif_3;
      }
    }
    link_direction = dir;
    if (dir != link_direction_last) {
      link_direction_last = dir;
      link_subpixel_x = link_subpixel_y = 0;
      link_moving_against_diag_tile = 0;
      bitmask_of_dragstate = 0;
      link_timer_push_get_tired = 32;
      link_timer_jump_ledge = 19;
    }
  }
  // endif_3
endif_3:
  Link_HandleDiagonalCollision();
  Link_HandleVelocity();
  Link_HandleCardinalCollision();
  Link_HandleMovingAnimation_FullLongEntry();
  if (link_unk_master_sword) getout_clear_vel: {
    link_y_vel = link_x_vel = 0;
  }

  fallhole_var1 = 0;

  // HandleIndoorCameraAndDoors must not be called twice in the same frame,
  // this might mess up camera positioning. For example when using spin attack
  // in between bumpers.
  if (g_ApplyLinksMovementToCamera_called && (enhanced_features0 & kFeatures0_MiscBugFixes))
    return;

  HandleIndoorCameraAndDoors();
}

/*
 * Link_HandleBunnyTransformation — Drive the temporary-bunny "poof"
 * cutscene. Triggered by link_timer_tempbunny being non-zero.
 *
 * Two phases gated by link_need_for_poof_for_transform:
 *   poof needed (first call): cancel any in-flight throw, clear all
 *     player props (preserving only the carrying-bit), kill any active
 *     transform ancillas (types 0x30/0x31), cancel a dash, drop a
 *     cape-poof garnish, play the bunny-poof SFX, and start a 20-frame
 *     pre-transform window.
 *   timer expiring (sign8 of decrement): commit to the bunny state
 *     (handler 28 = TempBunny), load bunny gear palettes, restore
 *     visibility, re-enable damage.
 *
 * Returns true while a transformation is in progress (caller skips
 * its normal logic), false when no transform is active.
 */
bool Link_HandleBunnyTransformation() {  // 8782da
  if (!link_timer_tempbunny)
    return false;

  if (!link_need_for_poof_for_transform) {
    if (link_player_handler_state == kPlayerState_PermaBunny || link_player_handler_state == kPlayerState_TempBunny) {
      link_timer_tempbunny = 0;
      return false;
    }
    if (link_picking_throw_state & 2)
      link_state_bits = 0;
    uint8 bak = link_state_bits & 0x80;
    Link_ResetProperties_A();
    link_state_bits = bak;

    for (int i = 4; i >= 0; i--) {
      if (ancilla_type[i] == 0x30 || ancilla_type[i] == 0x31)
        ancilla_type[i] = 0;
    }
    Link_CancelDash();
    AncillaAdd_CapePoof(0x23, 4);
    Ancilla_Sfx2_Near(0x14);
    link_bunny_transform_timer = 20;
    link_disable_sprite_damage = 1;
    link_need_for_poof_for_transform = 1;
    link_visibility_status = 12;
  }
  if (sign8(--link_bunny_transform_timer)) {
    link_player_handler_state = kPlayerState_TempBunny;
    link_is_bunny_mirror = 1;
    link_is_bunny = 1;
    LoadGearPalettes_bunny();
    link_visibility_status = 0;
    link_disable_sprite_damage = 0;
    link_need_for_poof_for_transform = 0;
  }
  return true;
}

/*
 * LinkState_TemporaryBunny — Slot 28: temp-bunny gameplay tick.
 * When the bunny timer hits 0 the "transform back" poof plays and
 * Link returns to default ground state with restored gear palettes.
 * While the timer is running, decrement it and run the standard
 * bunny handler so movement still works (bunny can't use items).
 */
void LinkState_TemporaryBunny() {  // 878365
  if (!link_timer_tempbunny) {
    AncillaAdd_CapePoof(0x23, 4);
    Ancilla_Sfx2_Near(0x15);
    link_bunny_transform_timer = 32;
    link_player_handler_state = 0;
    Link_ResetProperties_C();
    link_need_for_poof_for_transform = 0;
    link_is_bunny = 0;
    link_is_bunny_mirror = 0;
    LoadActualGearPalettes();
    link_need_for_poof_for_transform = 0;
    LinkState_Default();
  } else {
    link_timer_tempbunny--;
    PlayerHandler_17_Bunny();
  }
}

/*
 * PlayerHandler_17_Bunny — Slot 23: permanent bunny form (dark world
 * without Moon Pearl). Out of water with no aux state, run the
 * bunny movement handler. With Moon Pearl in pocket, the next frame
 * will exit bunny mode (link_is_bunny_mirror is cleared).
 *
 * In water or in an aux state, fall through to LinkState_Bunny_recache
 * which transitions Link out of bunny mode if conditions changed.
 */
void PlayerHandler_17_Bunny() {  // 8783a1
  CacheCameraPropertiesIfOutdoors();
  fallhole_var2 = 0;
  if (!link_is_in_deep_water) {
    if (link_auxiliary_state == 0) {
      Link_TempBunny_Func2();
      return;
    }
    if (link_item_moon_pearl)
      link_is_bunny_mirror = 0;
  }
  LinkState_Bunny_recache();
}

/*
 * LinkState_Bunny_recache — Re-evaluate bunny status after a state
 * change (entered water, Moon Pearl picked up, etc.). With Moon
 * Pearl, exit bunny form fully and reload normal gear palettes;
 * without it, drop into the "RecoilWall" intermediate state which
 * will animate the un-bunny poof on the next frame.
 */
void LinkState_Bunny_recache() {  // 8783c7
  link_need_for_poof_for_transform = 0;
  link_timer_tempbunny = 0;
  if (link_item_moon_pearl) {
    link_is_bunny = 0;
    link_auxiliary_state = 0;
  }
  link_animation_steps = 0;
  link_is_transforming = 0;
  link_cant_change_direction = 0;
  Link_ResetSwimmingState();
  link_player_handler_state = kPlayerState_RecoilWall;
  if (link_item_moon_pearl) {
    link_player_handler_state = kPlayerState_Ground;
    LoadActualGearPalettes();
  }
}

/*
 * Link_TempBunny_Func2 — Bunny-form ground tick. Identical structure
 * to PlayerHandler_00_Ground_3's free-movement branch but stripped
 * of every item/sword path (bunny can't use anything). Reads the
 * joypad d-pad or force_move bits, sets velocity, runs collision +
 * animation. The "tired/jump-ledge timers" (32 / 19) are reset on
 * any direction change so the bunny doesn't accumulate dash credit.
 */
void Link_TempBunny_Func2() {  // 8783fa
  if (link_incapacitated_timer != 0) {
    Link_HandleRecoilAndTimer(false);
    return;
  }
  link_z_coord = 0xffff;
  link_actual_vel_z = 0xff;
  link_recoilmode_timer = 0;
  if (link_flag_moving) {
    swimcoll_var9[0] = swimcoll_var9[1] = 0x180;
    Link_HandleSwimMovements();
    return;
  }

  ResetAllAcceleration();
  Link_HandleYItem();
  uint8 dir;
  if (!(dir = force_move_any_direction & 0xf) && !(dir = joypad1H_last & kJoypadH_AnyDir)) {
    link_x_vel = link_y_vel = 0;
    link_direction = link_direction_last = 0;
    link_animation_steps = 0;
    bitmask_of_dragstate &= ~9;
    link_timer_push_get_tired = 32;
    link_timer_jump_ledge = 19;
  } else {
    link_direction = dir;
    if (dir != link_direction_last) {
      link_direction_last = dir;
      link_subpixel_x = 0;
      link_subpixel_y = 0;
      link_moving_against_diag_tile = 0;
      bitmask_of_dragstate = 0;
      link_timer_push_get_tired = 32;
      link_timer_jump_ledge = 19;
    }
  }
  Link_HandleDiagonalCollision();
  Link_HandleVelocity();
  Link_HandleCardinalCollision();
  Link_HandleMovingAnimation_FullLongEntry();
  fallhole_var1 = 0;
  HandleIndoorCameraAndDoors();
}

/*
 * LinkState_HoldingBigRock — Slot 24: Power-Glove / Titan's-Mitt big
 * rock pickup. Identical "knock-out-of-state" handling as
 * HandleLink_From1D when an aux state hits, but during normal
 * gameplay forwards through Link_HandleAPress (so Link can drop the
 * rock by pressing A again) and a stripped-down movement loop.
 */
void LinkState_HoldingBigRock() {  // 878481
  if (link_auxiliary_state) {
    link_item_in_hand = 0;
    link_position_mode = 0;
    link_debug_value_1 = 0;
    link_debug_value_2 = 0;
    link_var30d = 0;
    link_var30e = 0;
    some_animation_timer_steps = 0;
    bitfield_for_a_button = 0;
    link_state_bits = 0;
    link_picking_throw_state = 0;
    link_grabbing_wall = 0;
    bitmask_of_dragstate = 0;
    link_cant_change_direction &= ~1;
    link_z_coord &= ~0xff;
    if (link_electrocute_on_touch) {
      Link_ResetSwordAndItemUsage();
      link_disable_sprite_damage = 1;
      player_handler_timer = 0;
      link_delay_timer_spin_attack = 2;
      link_animation_steps = 0;
      link_direction &= ~0xf;
      Ancilla_Sfx3_Near(43);
      link_player_handler_state = kPlayerState_Electrocution;
      LinkState_Zapped();
    } else {
      link_player_handler_state = kPlayerState_RecoilWall;
      LinkState_Recoil();
    }
    return;
  }

  link_z_coord = 0xffff;
  link_actual_vel_z = 0xff;
  link_recoilmode_timer = 0;
  if (link_incapacitated_timer) {
    link_var30d = 0;
    link_var30e = 0;
    some_animation_timer_steps = 0;
    bitfield_for_a_button = 0;
    link_state_bits = 0;
    link_picking_throw_state = 0;
    link_grabbing_wall = 0;
    if (!(button_mask_b_y & 0x80))
      link_cant_change_direction &= ~1;
    Link_HandleRecoilAndTimer(false);
    return;
  }

  Link_HandleAPress();
  if (!(joypad1H_last & kJoypadH_AnyDir)) {
    link_y_vel = 0;
    link_x_vel = 0;
    link_direction = 0;
    link_direction_last = 0;
    link_animation_steps = 0;
    bitmask_of_dragstate &= ~9;
    link_timer_push_get_tired = 32;
    link_timer_jump_ledge = 19;
  } else {
    link_direction = joypad1H_last & kJoypadH_AnyDir;
    if (link_direction != link_direction_last) {
      link_direction_last = link_direction;
      link_subpixel_x = 0;
      link_subpixel_y = 0;
      link_moving_against_diag_tile = 0;
      bitmask_of_dragstate = 0;
      link_timer_push_get_tired = 32;
      link_timer_jump_ledge = 19;
    }
  }
  Link_HandleMovingAnimation_FullLongEntry();
  fallhole_var1 = 0;
  HandleIndoorCameraAndDoors();
}

/*
 * EtherTablet_StartCutscene — Kick off the "found the Ether tablet"
 * cutscene. Seeds the cutscene timer (button_b_frames repurposed as
 * the countdown), switches into the receiving-ether handler state,
 * and locks down both menu access and incoming damage.
 */
void EtherTablet_StartCutscene() {  // 87855a
  button_b_frames = 0xc0;
  link_delay_timer_spin_attack = 0;
  link_player_handler_state = kPlayerState_ReceivingEther;
  link_disable_sprite_damage = 1;
  flag_block_link_menu = 1;
}

/*
 * LinkState_ReceivingEther — Slot 25: Ether-tablet cutscene driver.
 *
 * Runs as a 192-frame countdown (button_b_frames). Specific frames
 * trigger scripted events:
 *   frame 0xbf (191): set link_force_hold_sword_up so Link raises
 *     the Master Sword overhead.
 *   frame 160: temporarily move Link to the upper-mountain coordinate
 *     (0x6b0, 0x37) and spawn the Ether spell ancilla (so the spell
 *     fires from the proper world position), then snap him back.
 *   frame 0: drop the medallion icon as a falling prize and fully
 *     immobilize Link until pickup.
 */
void LinkState_ReceivingEther() {  // 878570
  link_auxiliary_state = 0;
  link_incapacitated_timer = 0;
  link_give_damage = 0;
  int i = --WORD(button_b_frames);
  if (sign16(i)) {
    button_b_frames = 0;
    link_delay_timer_spin_attack = 0;
  } else if (i == 0xbf) {
    link_force_hold_sword_up = 1;
  } else if (i == 160) {
    uint16 x = link_x_coord, y = link_y_coord;
    link_x_coord = 0x6b0;
    link_y_coord = 0x37;
    AncillaAdd_EtherSpell(0x18, 0);
    link_x_coord = x, link_y_coord = y;
  } else if (i == 0) {
    AncillaAdd_FallingPrize(0x29, 0, 4);
    flag_is_link_immobilized = 1;
    flag_block_link_menu = 0;
  }
}

/*
 * BombosTablet_StartCutscene — Bombos-tablet equivalent of the Ether
 * variant: 224-frame countdown (vs 192), and instead of locking the
 * menu it sets the "custom spell anim active" flag so other systems
 * suppress visuals during the cast.
 */
void BombosTablet_StartCutscene() {  // 8785e5
  button_b_frames = 0xe0;
  link_delay_timer_spin_attack = 0;
  link_player_handler_state = kPlayerState_ReceivingBombos;
  link_disable_sprite_damage = 1;
  flag_custom_spell_anim_active = 1;
}

/*
 * LinkState_ReceivingBombos — Slot 26: Bombos-tablet cutscene driver.
 * Same structure as LinkState_ReceivingEther but with different
 * scripted-event frame numbers (223 = sword raise; 160 = spell from
 * the lava/desert origin (0x378, 0xeb0)) and a different falling-
 * prize id (5 = Bombos vs 0 = Ether).
 */
void LinkState_ReceivingBombos() {  // 8785fb
  link_auxiliary_state = 0;
  link_incapacitated_timer = 0;
  link_give_damage = 0;
  int i = --WORD(button_b_frames);
  if (sign16(i)) {
    button_b_frames = 0;
    link_delay_timer_spin_attack = 0;
  } else if (i == 223) {
    link_force_hold_sword_up = 1;
  } else if (i == 160) {
    uint16 x = link_x_coord, y = link_y_coord;
    link_x_coord = 0x378;
    link_y_coord = 0xeb0;
    AncillaAdd_BombosSpell(0x19, 0);
    link_x_coord = x, link_y_coord = y;
  } else if (i == 0) {
    AncillaAdd_FallingPrize(0x29, 5, 4);
    flag_is_link_immobilized = 1;
  }
}

/*
 * LinkState_ReadingDesertTablet — Slot 27: Mire / desert tablet
 * read pose. Just counts down button_b_frames and, on hitting 0,
 * returns to ground state and triggers the prayer/teleport via
 * Link_PerformDesertPrayer.
 */
void LinkState_ReadingDesertTablet() {  // 87867b
  if (!--button_b_frames) {
    link_player_handler_state = kPlayerState_Ground;
    Link_PerformDesertPrayer();
  }
}

/*
 * HandleSomariaAndGraves — Per-frame ancilla housekeeping run after
 * Link's main update.
 *
 * Outdoor + hookshot-tugged: walk all 5 ancilla slots backwards and
 * advance any gravestone (type 0x24) being pulled.
 *
 * Always: scan for an active Somaria block (type 0x2C) and delegate
 * its player-interaction handling. Stops at the first Somaria block
 * found (only one can be active at a time).
 */
void HandleSomariaAndGraves() {  // 878689
  if (!player_is_indoors && link_something_with_hookshot) {
    int i = 4;
    do {
      if (ancilla_type[i] == 0x24)
        Gravestone_Move(i);
    } while (--i >= 0);
  }
  int i = 4;
  do {
    if (ancilla_type[i] == 0x2C) {
      SomariaBlock_HandlePlayerInteraction(i);
      return;
    }
  } while (--i >= 0);
}

/*
 * LinkState_Recoil — Slot 2 / 6: Link is in mid-knockback. Stashes
 * the pre-recoil position (so death-recovery can restore him to a
 * safe spot), runs the Z-velocity integration, then either:
 *   - In the air: continue the recoil tick.
 *   - On the ground in deep water: switch to swimming state with a
 *     splash garnish, reset weapon usage.
 *   - On normal ground: ramp the Z velocity down (the magic ">>=1
 *     in a `do { ... } while (!--s)` loop" is a per-step velocity
 *     halving with one extra halving every 4 frames). Cap the timer
 *     at 3 so the ramp doesn't decay infinitely.
 */
void LinkState_Recoil() {  // 8786b5
  link_y_coord_safe_return_lo = link_y_coord;
  link_y_coord_safe_return_hi = link_y_coord >> 8;
  link_x_coord_safe_return_lo = link_x_coord;
  link_x_coord_safe_return_hi = link_x_coord >> 8;
  Link_HandleChangeInZVelocity();
  link_cant_change_direction = 0;
  draw_water_ripples_or_grass = 0;
  if (!sign8(link_z_coord) || !sign8(link_actual_vel_z)) {
    Link_HandleRecoilAndTimer(false);
    return;
  }
  TileDetect_MainHandler(5);
  if (tiledetect_deepwater & 1) {
    link_player_handler_state = kPlayerState_Swimming;
    Link_SetToDeepWater();
    Link_ResetSwordAndItemUsage();
    AncillaAdd_Splash(21, 0);
    Link_HandleRecoilAndTimer(true);
  } else {
    if (++link_recoilmode_timer != 4) {
      uint8 t = link_actual_vel_z_copy, s = link_recoilmode_timer;
      do {
        t >>= 1;
      } while (!--s); // wtf?
      link_actual_vel_z = t;
    } else {
      link_recoilmode_timer = 3;
    }
    Link_HandleRecoilAndTimer(false);
  }
}

/*
 * Link_HandleRecoilAndTimer — Inner state machine for recoil/aux
 * states. Drives the incapacitated_timer countdown and triggers the
 * landing transition at the end.
 *
 * The `jump_into_middle` parameter is for the Recoil-into-water path:
 * the caller has already done the splash + swim-state setup and just
 * wants the landing-tail logic to run. It jumps past the timer
 * decrement and into the "Link has landed" branch.
 *
 * Landing detection: when Z is at/below ground AND vertical velocity
 * is downward, scan the underlying tile for special effects:
 *   - thick grass: rustle SFX (26)
 *   - shallow water: splash SFX (28)
 *   - deep water: switch to swimming, drop the cape, splash garnish
 *
 * Bunny-in-deep-water suppresses the landing SFX since the bunny
 * just bounces back to land. Cape force-unequip on water entry
 * prevents flying-cape-over-water exploits.
 */
void Link_HandleRecoilAndTimer(bool jump_into_middle) {  // 878711
  if (jump_into_middle)
    goto lbl_jump_into_middle;

  link_x_page_movement_delta = 0;
  link_y_page_movement_delta = 0;
  link_num_orthogonal_directions = 0;
  Link_HandleRecoiling();  // not
  if (--link_incapacitated_timer == 0) {
    link_incapacitated_timer = 1;
    int8 z;
    z = link_z_coord & 0xfe;
    if (z <= 0 && (int8)link_actual_vel_z < 0) {
      if (link_auxiliary_state != 0) {
        link_disable_sprite_damage = 0;
        scratch_0 = link_player_handler_state;
        if (link_player_handler_state != 6) {
          button_b_frames = 0;
          button_mask_b_y = 0;
          link_delay_timer_spin_attack = 0;
          link_spin_attack_step_counter = 0;
        }
        Link_SplashUponLanding();
        if (!link_is_bunny_mirror || !link_is_in_deep_water) {
          if (link_want_make_noise_when_dashed) {
            link_want_make_noise_when_dashed = 0;
            Ancilla_Sfx2_Near(33);
          } else if (scratch_0 != 2 && link_player_handler_state != 4) {
            Ancilla_Sfx2_Near(33);
          }
          if (link_player_handler_state == 4) {
            Link_ForceUnequipCape_quietly();
            if (player_is_indoors && scratch_0 != 2 && link_item_flippers) {
              link_is_on_lower_level = 1;
            }
            AncillaAdd_Splash(21, 0);
          }
          TileDetect_MainHandler(0);
          if (tiledetect_thick_grass & 1)
            Ancilla_Sfx2_Near(26);
          if (tiledetect_shallow_water & 1 && sound_effect_1 != 36)
            Ancilla_Sfx2_Near(28);

          if (tiledetect_deepwater & 1) {
            link_player_handler_state = kPlayerState_Swimming;
            Link_SetToDeepWater();
            Link_ResetSwordAndItemUsage();
            AncillaAdd_Splash(21, 0);
          }

          // OMG something jumps to here...
lbl_jump_into_middle:
          if (link_is_on_lower_level == 2)
            link_is_on_lower_level = 0;
          if (about_to_jump_off_ledge)
            Dungeon_HandleLayerChange();
        }
        link_z_coord = 0;
        link_auxiliary_state = 0;
        link_speed_setting = 0;
        link_cant_change_direction = 0;
        link_item_in_hand = 0;
        link_position_mode = 0;
        player_handler_timer = 0;
        link_disable_sprite_damage = 0;
        link_electrocute_on_touch = 0;
        link_actual_vel_x = 0;
        link_actual_vel_y = 0;
      }
      link_animation_steps = 0;
      link_incapacitated_timer = 0;
    }
  }

  if (link_player_handler_state != 5 && link_incapacitated_timer >= 33) {
    if (!sign8(--byte_7E02C5))
      goto timer_running;
    byte_7E02C5 = link_incapacitated_timer >> 4;
  }

  Flag67WithDirections();
  if (link_player_handler_state != 6) {
    Link_HandleDiagonalCollision();  // not
    if ((link_direction & 3) == 0)
      link_actual_vel_x = 0;
    if ((link_direction & 0xc) == 0)
      link_actual_vel_y = 0;
  }
  Link_MovePosition(); // not
timer_running:
  if (link_player_handler_state != 6) {
    Link_HandleCardinalCollision(); // not
    fallhole_var1 = 0;
  }
  HandleIndoorCameraAndDoors();
  if (BYTE(link_z_coord) == 0 || BYTE(link_z_coord) >= 0xe0) {
    Player_TileDetectNearby();
    if ((tiledetect_pit_tile & 0xf) == 0xf) {
      link_player_handler_state = kPlayerState_FallingIntoHole;
      link_speed_setting = 4;
    }
  }
  HIBYTE(link_z_coord) = 0;
}

/*
 * LinkState_OnIce — Slot 5: ice-sliding state. Unimplemented in the
 * reimplementation (the original ROM had its own logic that hasn't
 * been ported); the assert ensures any unintended entry trips loudly.
 */
void LinkState_OnIce() {  // 878872
  assert(0);
}

/*
 * Link_HandleChangeInZVelocity — Apply gravity to Link's vertical
 * velocity. Default is 2 px/frame deceleration; the Turtle Rock boss
 * fight uses lighter gravity (1) to make Link's hops more floaty.
 */
void Link_HandleChangeInZVelocity() {  // 878926
  Player_ChangeZ(link_player_handler_state == kPlayerState_TurtleRock ? 1 : 2);
}

/*
 * Player_ChangeZ — Subtract `zd` from link_actual_vel_z, with two
 * special cases:
 *   - Already grounded with negative velocity: don't accumulate
 *     "buried below ground" Z.
 *   - Negative velocity overflow: clamp to the sentinel pair
 *     (Z = 0xffff, vel = 0xff) which downstream code reads as "on
 *     the ground, no Z motion".
 */
void Player_ChangeZ(uint8 zd) {  // 878932
  if (sign8(link_actual_vel_z)) {
    if (!(uint8)link_z_coord)
      return;
    if (sign8(link_z_coord)) {
      link_z_coord = 0xffff;
      link_actual_vel_z = 0xff;
      return;
    }
  }
  link_actual_vel_z -= zd;
}

/*
 * LinkHop_HoppingSouthOW — Slot 11: overworld southward ledge hop.
 * Initial entry plays the hop SFX (32) and finds the destination
 * tile via LinkHop_FindTileToLandOnSouth. Each frame ticks Z by -2
 * (gravity), drives the y-velocity from the Z delta, and on landing
 * resolves splash/water/pit interactions. The 0xa0 floor on the
 * negative velocity caps fall speed so Link doesn't accelerate
 * indefinitely off a tall ledge.
 *
 * Author note preserved: "This is the place that caused the water
 * walking bug after bonk, player_near_pit_state was not reset."
 */
void LinkHop_HoppingSouthOW() {  // 87894e
  link_last_direction_moved_towards = 1;
  link_cant_change_direction = 0;
  link_actual_vel_x = 0;
  link_actual_vel_y = 0;
  draw_water_ripples_or_grass = 0;
  if (!link_incapacitated_timer && !link_actual_vel_z_mirror) {
    Ancilla_Sfx2_Near(32);
    LinkHop_FindTileToLandOnSouth();
    if (!player_is_indoors)
      link_is_on_lower_level = 2;
  }
  link_actual_vel_z = link_actual_vel_z_mirror;
  link_actual_vel_z_copy = link_actual_vel_z_copy_mirror;
  link_z_coord = link_z_coord_mirror;
  link_actual_vel_z -= 2;
  Link_MovePosition();
  if (sign8(link_actual_vel_z)) {
    if (link_actual_vel_z < 0xa0)
      link_actual_vel_z = 0xa0;
    if (link_z_coord >= 0xfff0) {
      link_z_coord = 0;
      Link_SplashUponLanding();
      // This is the place that caused the water walking bug after bonk, 
      // player_near_pit_state was not reset.
      if (player_near_pit_state)
        link_player_handler_state = kPlayerState_FallingIntoHole;
      if (link_player_handler_state != kPlayerState_Swimming &&
          link_player_handler_state != kPlayerState_FallingIntoHole && !link_is_in_deep_water)
        Ancilla_Sfx2_Near(33);
      link_disable_sprite_damage = 0;
      allow_scroll_z = 0;
      link_auxiliary_state = 0;
      link_actual_vel_z = 0xff;
      link_z_coord = 0xffff;
      link_incapacitated_timer = 0;
      if (!player_is_indoors)
        link_is_on_lower_level = 0;
    } else {
      link_y_vel = link_z_coord_mirror - link_z_coord;
    }
  } else {
    link_y_vel = link_z_coord_mirror - link_z_coord;
  }
  link_actual_vel_z_mirror = link_actual_vel_z;
  link_actual_vel_z_copy_mirror = link_actual_vel_z_copy;
  link_z_coord_mirror = link_z_coord;
}

/*
 * LinkState_HandlingJump — Generic Z-arc integrator shared by the
 * non-south hop slots (12/13/14). Same gravity / clamp / landing
 * logic as LinkHop_HoppingSouthOW but additionally checks for
 * deep-water and pit landings via TileDetect_MainHandler.
 *
 * The pit-landing branch sets player_near_pit_state = 1 so the
 * "shrinking shadow" pit-fall renderer in player_oam.c picks it up,
 * and bumps byte_7E005C to seed the pit-fall sub-state.
 */
void LinkState_HandlingJump() {  // 878a05
  link_actual_vel_z = link_actual_vel_z_mirror;
  link_actual_vel_z_copy = link_actual_vel_z_copy_mirror;
  BYTE(link_z_coord) = link_z_coord_mirror;
  link_actual_vel_z -= 2;
  Link_MovePosition();
  if (sign8(link_actual_vel_z)) {
    if (link_actual_vel_z < 0xa0)
      link_actual_vel_z = 0xa0;
    if ((uint8)link_z_coord >= 0xf0) {
      link_z_coord = 0;
      if (link_player_handler_state == kPlayerState_FallOfLeftRightLedge || link_player_handler_state == kPlayerState_JumpOffLedgeDiag) {
        TileDetect_MainHandler(0);
        if (tiledetect_deepwater & 1) {
          link_player_handler_state = kPlayerState_Swimming;
          Link_SetToDeepWater();
          Link_ResetSwordAndItemUsage();
          AncillaAdd_Splash(21, 0);
        } else if (tiledetect_pit_tile & 1) {
          byte_7E005C = 9;
          link_this_controls_sprite_oam = 0;
          player_near_pit_state = 1;
          link_player_handler_state = kPlayerState_FallingIntoHole;
          goto after_pit;
        }
      }
      Link_SplashUponLanding();
      if (link_player_handler_state != kPlayerState_Swimming && !link_is_in_deep_water)
        Ancilla_Sfx2_Near(33);
after_pit:
      if (link_player_handler_state != kPlayerState_Swimming || !link_is_bunny_mirror)
        link_disable_sprite_damage = 0;

      allow_scroll_z = 0;
      link_auxiliary_state = 0;
      link_actual_vel_z = 0xff;
      link_z_coord = 0xffff;
      link_incapacitated_timer = 0;
      if (!player_is_indoors)
        link_is_on_lower_level = 0;
    } else {
      link_y_vel = link_z_coord_mirror - link_z_coord;
    }
  } else {
    link_y_vel = link_z_coord_mirror - link_z_coord;
  }
  link_actual_vel_z_mirror = link_actual_vel_z;
  link_actual_vel_z_copy_mirror = link_actual_vel_z_copy;
  BYTE(link_z_coord_mirror) = link_z_coord;
}

/*
 * LinkHop_FindTileToLandOnSouth — Search downward from Link's current
 * position for the first solid landing tile. Steps `link_y_coord` by
 * the per-direction Y delta until the tile-detect bitmask reports a
 * landable surface (normal/pit/destruction/grass/deepwater all set).
 *
 * Side effects on the landing tile:
 *   deep water: enter swim aux-state, reset swim state.
 *   pit:        seed the pit-fall countdown (byte_7E005C = 9, state
 *               1 -> player_oam.c falling-shadow renderer).
 *
 * Then writes the snap-to-floor Y back into the safe-return mirrors
 * so a later death-recovery places Link at the landed spot.
 */
void LinkHop_FindTileToLandOnSouth() {  // 878ad1
  link_y_coord_original = link_y_coord;
  link_y_vel = link_y_coord - link_y_coord_safe_return_lo;
  for (;;) {
    link_y_coord += kLink_DoMoveXCoord_Outdoors_Helper2_y[link_last_direction_moved_towards];
    TileDetect_Movement_Y(link_last_direction_moved_towards);
    uint8 k = tiledetect_normal_tiles | tiledetect_pit_tile | tiledetect_destruction_aftermath | tiledetect_thick_grass | tiledetect_deepwater;
    if ((k & 7) == 7)
      break;
  }
  if (tiledetect_deepwater & 7) {
    link_is_in_deep_water = 1;
    if (link_auxiliary_state != 4)
      link_auxiliary_state = 2;
    link_some_direction_bits = link_direction_last;
    Link_ResetSwimmingState();
    link_grabbing_wall = 0;
    link_speed_setting = 0;
  }
  if (tiledetect_pit_tile & 7) {
    byte_7E005C = 9;
    link_this_controls_sprite_oam = 0;
    player_near_pit_state = 1;
  }
  link_y_coord += kLink_DoMoveXCoord_Outdoors_Helper2_y2[link_last_direction_moved_towards];
  link_y_coord_safe_return_lo = link_y_coord;
  link_y_coord_safe_return_hi = link_y_coord >> 8;
  link_incapacitated_timer = 1;
  uint8 z = link_z_coord;
  if (z >= 0xf0)
    z = 0;
  link_z_coord = link_z_coord_mirror = link_y_coord - link_y_coord_original + z;
}

/*
 * LinkState_HoppingHorizontallyOW — Slot 12: east/west ledge hop.
 * Picks facing 5 (right) or 6 (left) from the velocity sign, then
 * runs the standard Z-arc integrator. Author note retained.
 */
// used on right ledges
void LinkState_HoppingHorizontallyOW() {  // 878b74
  link_direction = sign8(link_actual_vel_x) ? 6 : 5;
  link_cant_change_direction = 0;
  link_actual_vel_y = 0;
  draw_water_ripples_or_grass = 0;
  LinkState_HandlingJump();
}

/*
 * Link_HoppingHorizontally_FindTile_Y — Vertical landing-search
 * variant for horizontal hops. Steps Y once and tests the tile; on
 * a hit, calculates jump arc (velz) and horizontal velocity from a
 * lookup keyed on |velx| >> 4. Deep-water lands transition into
 * swimming aux-state.
 */
void Link_HoppingHorizontally_FindTile_Y() {  // 878b9b
  link_y_coord_original = link_y_coord;
  link_y_vel = link_y_coord - link_y_coord_safe_return_lo;

  link_y_coord += kLink_DoMoveXCoord_Outdoors_Helper2_y[link_last_direction_moved_towards];
  TileDetect_Movement_Y(link_last_direction_moved_towards);

  uint8 tt = tiledetect_normal_tiles | tiledetect_destruction_aftermath | tiledetect_thick_grass |
    tiledetect_deepwater;

  if ((tt & 7) != 7) {
    link_y_coord = link_y_coord_original;
    link_incapacitated_timer = 1;

    int8 velx = link_actual_vel_x, org_velx = velx;
    if (velx < 0) velx = -velx;
    velx >>= 4;
    link_actual_vel_z_mirror = link_actual_vel_z_copy_mirror = kLink_DoMoveXCoord_Outdoors_Helper2_velz[velx];

    uint8 xt = kLink_DoMoveXCoord_Outdoors_Helper2_velx[velx];
    if (org_velx < 0) xt = -xt;
    link_actual_vel_x = xt;
  } else {  // else_1
    link_y_coord += kLink_DoMoveXCoord_Outdoors_Helper2_y2[link_last_direction_moved_towards];
    link_y_coord_safe_return_lo = link_y_coord;
    link_y_coord_safe_return_hi = link_y_coord >> 8;
    link_incapacitated_timer = 1;
    uint8 z = link_z_coord;
    if (z == 255) z = 0;
    link_z_coord_mirror = link_z_coord = link_y_coord - link_y_coord_original + z;
  }  // endif_1

  if (tiledetect_deepwater & 7) {
    link_auxiliary_state = 2;
    Link_SetToDeepWater();
  }
}

/*
 * Link_SetToDeepWater — Common entry helper that flips Link into
 * deep-water swim state: sets the deep-water flag, latches the
 * direction, clears wall-grab and speed bonuses, and resets the
 * swim-state machine.
 */
void Link_SetToDeepWater() {  // 878c44
  link_is_in_deep_water = 1;
  link_some_direction_bits = link_direction_last;
  Link_ResetSwimmingState();
  link_grabbing_wall = 0;
  link_speed_setting = 0;
}

/*
 * LinkState_0F — Slots 15/16: unused placeholder. Asserts on entry.
 */
void LinkState_0F() {  // 878c69
  assert(0);
}

/*
 * Link_HoppingHorizontally_FindTile_X — Horizontal landing search
 * for east/west ledge hops. Steps X by 8 px (or 32 max via the +/-
 * fallback) until a landable tile is found. Picks the velx/velz
 * arc parameters from the 24-entry tuned tables based on traveled
 * distance >> 3.
 *
 * The `o` parameter is the direction (0 = west, 2 = east).
 */
uint8 Link_HoppingHorizontally_FindTile_X(uint8 o) {  // 878d2b
  assert(o == 0 || o == 2);
  link_y_coord_original = link_x_coord;
  int i = 7;
  static const int8 kLink_DoMoveXCoord_Outdoors_Helper1_tab1[2] = { -8, 8 };
  static const int8 kLink_DoMoveXCoord_Outdoors_Helper1_tab2[2] = { -32, 32 };
  static const int8 kLink_DoMoveXCoord_Outdoors_Helper1_tab3[2] = { -16, 16 };
  static const uint8 kLink_DoMoveXCoord_Outdoors_Helper1_velx[24] = { 20, 20, 20, 24, 24, 24, 24, 28, 28, 36, 36, 36, 36, 36, 36, 38, 38, 38, 38, 38, 38, 38, 40, 40 };
  static const uint8 kLink_DoMoveXCoord_Outdoors_Helper1_velz[24] = { 20, 20, 20, 20, 20, 20, 20, 24, 24, 32, 32, 32, 36, 36, 36, 38, 38, 38, 38, 38, 38, 38, 40, 40 };
  do {
    link_x_coord += kLink_DoMoveXCoord_Outdoors_Helper1_tab1[o >> 1];
    TileDetect_Movement_X(link_last_direction_moved_towards);

    uint8 tt = tiledetect_normal_tiles | tiledetect_destruction_aftermath | tiledetect_thick_grass |
      tiledetect_deepwater | tiledetect_pit_tile;

    if ((tt & 7) == 7) {
      if ((tiledetect_deepwater & 7) == 7) {
        link_is_in_deep_water = 1;
        link_auxiliary_state = 2;
        link_some_direction_bits = link_direction_last;
        swimming_countdown = 0;
        link_speed_setting = 0;
        link_grabbing_wall = 0;
        ResetAllAcceleration();
      }
      goto finish;
    }
  } while (--i >= 0);

  link_x_coord = link_y_coord_original + kLink_DoMoveXCoord_Outdoors_Helper1_tab2[o >> 1];
finish:
  link_x_coord += kLink_DoMoveXCoord_Outdoors_Helper1_tab3[o >> 1];
  int16 xt = link_y_coord_original - link_x_coord;
  if (xt < 0) xt = -xt;
  xt >>= 3;
  uint8 velx = kLink_DoMoveXCoord_Outdoors_Helper1_velx[xt];
  if (o != 2) velx = -velx;
  link_actual_vel_x = velx;
  link_actual_vel_z_mirror = link_actual_vel_z_copy_mirror = kLink_DoMoveXCoord_Outdoors_Helper1_velz[xt];

  return i;
}

/*
 * LinkState_HoppingDiagonallyUpOW — Slot 13: short upward arc used
 * when Link hops diagonally onto a higher ledge. Just runs gravity
 * and resolves landing — the X velocity was set up by the entry path.
 */
// used on diag ledges
void LinkState_HoppingDiagonallyUpOW() {  // 878dc6
  draw_water_ripples_or_grass = 0;
  Player_ChangeZ(2);
  Link_MovePosition();
  if (sign8(link_z_coord)) {
    Link_SplashUponLanding();
    if (link_player_handler_state != kPlayerState_Swimming && !link_is_in_deep_water)
      Ancilla_Sfx2_Near(33);
    link_disable_sprite_damage = 0;
    link_auxiliary_state = 0;
    link_actual_vel_z = 0xff;
    link_z_coord = 0xffff;
    link_incapacitated_timer = 0;
    link_cant_change_direction = 0;
  }
}

/*
 * LinkState_HoppingDiagonallyDownOW — Slot 14: diagonal-down ledge
 * hop. On entry, plays the hop SFX and finds the diagonal landing
 * spot, then derives X velocity from the kLedgeVelX 24-entry table
 * (clamped to avoid an OOB read present in the original ROM).
 *
 * Direction-derived sign: positive velx for "down-right" (dir 3),
 * negative for "down-left" (dir 2).
 */
void LinkState_HoppingDiagonallyDownOW() {  // 878e15
  uint8 dir = sign8(link_actual_vel_x) ? 2 : 3;
  link_last_direction_moved_towards = dir;
  link_cant_change_direction = 0;
  link_actual_vel_y = 0;
  draw_water_ripples_or_grass = 0;
  if (!link_incapacitated_timer && !link_actual_vel_z_mirror) {
    link_last_direction_moved_towards = 1;
    uint16 old_x = link_x_coord;
    Ancilla_Sfx2_Near(32);
    LinkHop_FindLandingSpotDiagonallyDown();
    link_x_coord = old_x;

    static const uint8 kLedgeVelX[] = { 
      4, 4, 4, 10, 10, 10, 11, 18,
      18, 18, 20, 20, 20, 20, 22, 22,
      26, 26, 26, 26, 28, 28, 28, 28
    };

    int t = (uint16)(link_y_coord - link_y_coord_original);
    // Fix out of bounds read
    int8 velx = kLedgeVelX[IntMin(t >> 3, 23)];
    link_actual_vel_x = (dir != 2) ? velx : -velx;
    if (!player_is_indoors)
      link_is_on_lower_level = 2;
  }
  LinkState_HandlingJump();
}

/*
 * LinkHop_FindLandingSpotDiagonallyDown — Walk a diagonal path
 * downward looking for the first tile that's a valid diagonal-hop
 * landing surface (kLink_Ledge_Func1_bits encodes the per-direction
 * tile-type mask). Each iteration steps by (dx, dy) until a hit;
 * then snaps to the post-landing offset (dy2) for the final spot.
 */
void LinkHop_FindLandingSpotDiagonallyDown() {  // 878e7b
  static const int8 kLink_Ledge_Func1_dx[2] = { -8, 8 };
  static const int8 kLink_Ledge_Func1_dy[2] = { -9, 9 };
  static const uint8 kLink_Ledge_Func1_bits[2] = { 6, 3 };
  static const int8 kLink_Ledge_Func1_dy2[2] = { -24, 24 };
  link_y_coord_original = link_y_coord;
  link_y_vel = link_y_coord - link_y_coord_safe_return_lo;

  uint8 scratch;
  for (;;) {
    int o = sign8(link_actual_vel_x) ? 0 : 1;

    link_x_coord += kLink_Ledge_Func1_dx[o];
    link_y_coord += kLink_Ledge_Func1_dy[link_last_direction_moved_towards];
    TileDetect_Movement_Y(link_last_direction_moved_towards);
    scratch = kLink_Ledge_Func1_bits[o];
    uint8 k = tiledetect_normal_tiles | tiledetect_destruction_aftermath | tiledetect_thick_grass | tiledetect_deepwater;
    if ((k & scratch) == scratch)
      break;
  }

  if (tiledetect_deepwater & scratch) {
    link_is_in_deep_water = 1;
    link_auxiliary_state = 2;
    link_some_direction_bits = link_direction_last;
    Link_ResetSwimmingState();
    link_speed_setting = 0;
    link_grabbing_wall = 0;
  }

  link_y_coord += kLink_Ledge_Func1_dy2[link_last_direction_moved_towards];
  link_y_coord_safe_return_lo = link_y_coord;
  link_y_coord_safe_return_hi = link_y_coord >> 8;
  link_incapacitated_timer = 1;
  link_z_coord_mirror = link_y_coord - link_y_coord_original + (uint8)link_z_coord;
  link_z_coord = link_z_coord_mirror;
}

/*
 * Link_SplashUponLanding — Common landing-resolution that decides
 * which player state Link should land in. Bunny-in-deep-water hops
 * back to land via the bunny re-cache; bunny-elsewhere transitions
 * to the appropriate bunny state. Non-bunny in deep water adds a
 * splash garnish, force-unequips the cape, and enters swimming.
 * Otherwise plain ground state.
 */
void Link_SplashUponLanding() {  // 878f1d
  if (link_is_bunny_mirror) {
    if (link_is_in_deep_water) {
      AncillaAdd_Splash(21, 0);
      LinkState_Bunny_recache();
      return;
    }
    link_player_handler_state = (link_item_moon_pearl) ? kPlayerState_TempBunny : kPlayerState_PermaBunny;
  } else if (link_is_in_deep_water) {
    if (link_player_handler_state != kPlayerState_RecoilOther)
      AncillaAdd_Splash(21, 0);
    Link_ForceUnequipCape_quietly();
    link_player_handler_state = kPlayerState_Swimming;
  } else {
    link_player_handler_state = kPlayerState_Ground;
  }
}

/*
 * LinkState_Dashing — Slot 17: Pegasus Boots dash run.
 *
 * Lifecycle:
 *   1. Bunny-transformation guard.
 *   2. If link_is_running has dropped, exit dash and return to ground.
 *   3. While dashing: stash B-frame at 9 (sword charged) so a held B
 *      will swing on dash end; cancel on aux-state hit; play the
 *      "skid/clack" SFX cycle via kDashTab1 mask + the index_of_dashing_sfx
 *      countdown.
 *   4. Direction handling: a buffered direction comes from the
 *      joypad's H d-pad bits; `kDashTab2` converts the cardinal one-
 *      hot bit into the velocity direction. With the
 *      kFeatures0_TurnWhileDashing enhancement, lateral d-pad input
 *      mid-dash rotates Link rather than canceling.
 *   5. On dash exit (countdown sign-flips negative), tagalong slot
 *      adjustment via kTagalongArr1/2.
 *   6. Continuing dash: clamp animation step, decrement the dash
 *      counter (floor at 32), spawn dust ancilla, ensure sword
 *      isn't mid-charge (button_b_frames floor at 9), run movement.
 */
void LinkState_Dashing() {  // 878f86
  CacheCameraPropertiesIfOutdoors();
  if (Link_HandleBunnyTransformation()) {
    if (link_player_handler_state == 23)
      PlayerHandler_17_Bunny();
    return;
  }
  if (!link_is_running) {
    link_disable_sprite_damage = 0;
    link_countdown_for_dash = 0;
    link_speed_setting = 0;
    link_player_handler_state = kPlayerState_Ground;
    link_cant_change_direction = 0;
    return;
  }

  if (button_mask_b_y & 0x80) {
    if (button_b_frames >= 9)
      button_b_frames = 9;
  }
  fallhole_var2 = 0;

  if (link_auxiliary_state) {
    link_disable_sprite_damage = 0;
    link_countdown_for_dash = 0;
    link_speed_setting = 0;
    link_cant_change_direction = 0;
    link_is_running = 0;
    bitmask_of_dragstate = 0;
    if (link_electrocute_on_touch) {
      if (link_cape_mode)
        Link_ForceUnequipCape_quietly();
      Link_ResetSwordAndItemUsage();
      link_disable_sprite_damage = 1;
      player_handler_timer = 0;
      link_delay_timer_spin_attack = 2;
      link_animation_steps = 0;
      link_direction &= ~0xf;
      Ancilla_Sfx3_Near(43);
      link_player_handler_state = kPlayerState_Electrocution;
      LinkState_Zapped();
    } else {
      link_player_handler_state = kPlayerState_RecoilWall;
      LinkState_Recoil();
    }
    return;
  }
  static const uint8 kDashTab1[] = { 7, 15, 15 };
  static const uint8 kDashTab2[] = { 8, 4, 2, 1 };
  uint8 a = link_countdown_for_dash;
  if (a == 0)
    a = index_of_dashing_sfx--;
  if (!(kDashTab1[link_countdown_for_dash >> 4] & a))
    Ancilla_Sfx2_Near(35);
  if (sign8(--link_countdown_for_dash)) {
    link_countdown_for_dash = 0;
    if (follower_indicator == kTagalongArr1[follower_indicator])
      follower_indicator = kTagalongArr2[follower_indicator];
  } else {
    index_of_dashing_sfx = 0;
    if (!(joypad1L_last & kJoypadL_A)) {
      link_animation_steps = 0;
      link_countdown_for_dash = 0;
      link_speed_setting = 0;
      link_player_handler_state = kPlayerState_Ground;
      link_is_running = 0;
      if (!(button_mask_b_y & 0x80))
        link_cant_change_direction = 0;
      return;
    }
    AncillaAdd_DashDust_charging(30, 0);
    link_x_vel = link_y_vel = 0;
    link_dash_ctr = 64;
    link_speed_setting = 16;
    uint8 dir;
    if (button_mask_b_y & 0x80 || is_standing_in_doorway || (dir = joypad1H_last & kJoypadH_AnyDir) == 0)
      dir = kDashTab2[link_direction_facing >> 1];
    link_some_direction_bits = link_direction = link_direction_last = dir;
    link_moving_against_diag_tile = 0;
    Link_HandleMovingAnimation_FullLongEntry();
    uint16 org_x = link_x_coord, org_y = link_y_coord;
    link_y_coord_safe_return_lo = link_y_coord;
    link_y_coord_safe_return_hi = link_y_coord >> 8;
    link_x_coord_safe_return_lo = link_x_coord;
    link_x_coord_safe_return_hi = link_x_coord >> 8;
    Link_HandleMovingFloor();
    Link_ApplyConveyor();
    if (player_on_somaria_platform)
      Link_HandleVelocityAndSandDrag(org_x, org_y);
    link_y_vel = link_y_coord - link_y_coord_safe_return_lo;
    link_x_vel = link_x_coord - link_x_coord_safe_return_lo;
    Link_HandleCardinalCollision();
    HandleIndoorCameraAndDoors();
    return;
  }

  if (link_animation_steps >= 6)
    link_animation_steps = 0;

  link_dash_ctr--;
  if (link_dash_ctr < 32)
    link_dash_ctr = 32;

  AncillaAdd_DashDust(30, 0);
  link_spin_attack_step_counter = 0;

  if ((uint8)(link_sword_type + 1) & 0xfe)
    TileDetect_MainHandler(7);

  if (sram_progress_indicator) {
    button_mask_b_y |= 0x80;
    button_b_frames = 9;
  }

  link_incapacitated_timer = 0;

  bool want_stop_dash = false;

  if (enhanced_features0 & kFeatures0_TurnWhileDashing) {
    if (!(joypad1L_last & kJoypadL_A)) {
      link_countdown_for_dash = 0x11;
      want_stop_dash = true;
    } else {
      static const uint8 kDashCtrlsToDir[16] = { 0, 1, 2, 0, 4, 4, 4, 0, 8, 8, 8, 0, 0, 0, 0, 0 };
      uint8 t = kDashCtrlsToDir[joypad1H_last & kJoypadH_AnyDir];
      if (t != 0 && t != link_direction_last) {
        link_direction = link_direction_last = t;
        link_some_direction_bits = t;
        Link_HandleMovingAnimation_FullLongEntry();
      }
    }
  } else {
    want_stop_dash = (joypad1H_last & kJoypadH_AnyDir) && (joypad1H_last & kJoypadH_AnyDir) != kDashTab2[link_direction_facing >> 1];
  }

  if (want_stop_dash) {
    link_player_handler_state = kPlayerState_StopDash;
    button_mask_b_y &= ~0x80;
    button_b_frames = 0;
    link_delay_timer_spin_attack = 0;
    LinkState_ExitingDash();
    return;
  }

  if (link_speed_setting == 0 && (enhanced_features0 & kFeatures0_TurnWhileDashing))
    link_speed_setting = 16;

  uint8 dir = force_move_any_direction & 0xf;
  if (dir == 0)
    dir = kDashTab2[link_direction_facing >> 1];
  link_direction = link_direction_last = dir;
  Link_HandleDiagonalCollision();
  Link_HandleVelocity();
  Link_HandleCardinalCollision();
  Link_HandleMovingAnimation_FullLongEntry();
  fallhole_var1 = 0;
  HandleIndoorCameraAndDoors();
}

void LinkState_ExitingDash() {  // 87915e
  CacheCameraPropertiesIfOutdoors();
  if (joypad1H_last & kJoypadH_AnyDir || link_countdown_for_dash >= 16) {
    link_countdown_for_dash = 0;
    link_speed_setting = 0;
    link_player_handler_state = kPlayerState_Ground;
    link_is_running = 0;
    swimcoll_var5[0] &= 0xff00;
    if (button_b_frames < 9)
      link_cant_change_direction = 0;
  } else {
    link_countdown_for_dash++;
  }
  Link_HandleMovingAnimation_FullLongEntry();
}

/*
 * Link_CancelDash — Hard stop the dash: clear all dash-dust ancillas
 * (type 0x1e), zero every dash-related Link variable, drop the speed
 * boost, and clear the swim-collision pulse. No-op if not dashing.
 */
void Link_CancelDash() {  // 879195
  if (link_is_running) {
    int i = 4;
    do {
      if (ancilla_type[i] == 0x1e)
        ancilla_type[i] = 0;
    } while (--i >= 0);
    link_countdown_for_dash = 0;
    link_speed_setting = 0;
    link_is_running = 0;
    link_cant_change_direction = 0;
    swimcoll_var5[0] = 0;
  }
}

/*
 * RepelDash — Called when Link's dash hits a wall/object. If still
 * dashing AND the dash has reached full speed (dash_ctr != 64), reset
 * the swim collision pulse, drop a tremor garnish (camera shake),
 * play the appropriate impact SFX (3, unless 27 or 50 already
 * playing), and apply the rebound knockback.
 */
void RepelDash() {  // 8791f1
  if (link_is_running && link_dash_ctr != 64) {
    Link_ResetSwimmingState();
    AncillaAdd_DashTremor(29, 1);
    Prepare_ApplyRumbleToSprites();
    if ((sound_effect_2 & 0x3f) != 27 && (sound_effect_2 & 0x3f) != 50)
      Ancilla_Sfx3_Near(3);
    LinkApplyTileRebound();
  }
}

/*
 * LinkApplyTileRebound — Apply post-dash-bonk knockback. The 4
 * static tables encode per-direction (last_direction_moved_towards):
 *   _Y/X:   recoil velocity (24 px/frame opposite to dash dir).
 *   _Sw11:  swim-coll secondary direction bits (forced-water swap).
 *   _Sw7:   swim-coll velocity for forced-water indices.
 *
 * Sets aux state 1 (incapacitated), 24-frame stun timer, 36 z-vel
 * (small bounce), clears dash flags. The link_flag_moving branch
 * configures water-rebound parameters when bonking into a water tile.
 */
void LinkApplyTileRebound() {  // 879222
  static const int8 kDashTab6Y[] = { 24, -24, 0, 0 };
  static const int8 kDashTab6X[] = { 0, 0, 24, -24 };
  static const int8 kDashTabSw11Y[] = { 1, 0, 0, 0 };
  static const int8 kDashTabSw11X[] = { 0, 0, 1, 0 };
  static const uint16 kDashTabSw7Y[] = { 384, 384, 0, 0, 256, 256, 0, 0 };
  static const uint16 kDashTabSw7X[] = { 0, 0, 384, 384, 0, 0, 256, 256 };
  static const uint8 kDashTabDir[] = { 8,4,2,1 };

  link_actual_vel_y = kDashTab6Y[link_last_direction_moved_towards];
  link_actual_vel_x = kDashTab6X[link_last_direction_moved_towards];
  link_incapacitated_timer = 24;
  link_actual_vel_z = link_actual_vel_z_copy = 36;
  if (link_flag_moving) {
    link_some_direction_bits = link_direction = kDashTabDir[link_last_direction_moved_towards];
    swimcoll_var11[0] = kDashTabSw11Y[link_last_direction_moved_towards];
    swimcoll_var11[1] = kDashTabSw11X[link_last_direction_moved_towards];

    int i = (link_flag_moving - 1) * 4 + link_last_direction_moved_towards;
    swimcoll_var7[0] = kDashTabSw7Y[i];
    swimcoll_var7[1] = kDashTabSw7X[i];
  }
  link_auxiliary_state = 1;
  link_want_make_noise_when_dashed = 1;
  BYTE(scratch_1) = 0;
  link_electrocute_on_touch = 0;
  link_speed_setting = 0;
  link_cant_change_direction = 0;
  link_moving_against_diag_tile = 0;
  if (link_last_direction_moved_towards & 2)
    link_y_vel = 0;
  else
    link_x_vel = 0;
}

/*
 * Sprite_RepelDash — Sprite-collision entry point: a sprite blocked
 * Link's dash. Sets the rebound direction from Link's facing and
 * runs the standard dash-rebound logic.
 */
void Sprite_RepelDash() {  // 879291
  link_last_direction_moved_towards = link_direction_facing >> 1;
  RepelDash();
}

/*
 * Flag67WithDirections — Recompute link_direction from current actual
 * velocity bits. Y bit picks up/down (8/4), X bit picks left/right
 * (2/1). Used in the recoil/aux paths where direction needs to track
 * velocity rather than input.
 */
void Flag67WithDirections() {  // 8792a0
  link_direction = 0;
  if (link_actual_vel_y)
    link_direction |= sign8(link_actual_vel_y) ? 8 : 4;
  if (link_actual_vel_x)
    link_direction |= sign8(link_actual_vel_x) ? 2 : 1;
}

/*
 * LinkState_Pits — Slot 1: falling-into-a-hole state machine.
 *
 * Two phases:
 *   "edge" (player_near_pit_state != 2): Link is near a pit but
 *     hasn't fully committed. Run the tile-detect, allow direction
 *     changes, allow continued dashing. Author note retained about
 *     the turbo-controller levitation bug fix.
 *   "committed" (player_near_pit_state == 2): immobilize Link, kill
 *     all input/items, play the falling SFX, animate the fall.
 *
 * The kFallHolePitDirs/kFallHoleDirs/kFallHoleDirs2 tables encode
 * which side of a pit edge Link entered from and where to push him
 * if his current direction conflicts with the pit edge. Several
 * bug-fix gates (kFeatures0_MiscBugFixes) close the "walk on water
 * after bonk" hole.
 */
void LinkState_Pits() {  // 8792d3
  link_direction = 0;
  if (fallhole_var1 && ++fallhole_var2 == 0x20) {
    fallhole_var2 = 31;
  } else {
    if (!link_is_running)
      goto aux_state;
    // If you use a turbo controller to perfectly spam the dash button,
    // the check for Link being in a hole is endlessly skipped and you
    // can levitate across chasms.
    // Fix this by ensuring that the dash button is held down before proceeding to the dash state.
    if (link_countdown_for_dash &&
        (!(enhanced_features0 & kFeatures0_MiscBugFixes) || (joypad1L_last & kJoypadL_A))) {
      LinkState_Dashing();
      return;
    }
    if (joypad1H_last & kJoypadH_AnyDir && !(joypad1H_last & kJoypadH_AnyDir & link_direction)) {
      Link_CancelDash();
aux_state:
      if (link_auxiliary_state != 1)
        link_direction = joypad1H_last & kJoypadH_AnyDir;
    }
  }
  TileDetect_MainHandler(4);
  if (!(tiledetect_pit_tile & 1)) {
    // Reset player_near_pit_state if we're no longer near a hole.
    // This fixes a bug where you could walk on water
    if (enhanced_features0 & kFeatures0_MiscBugFixes)
      player_near_pit_state = 0;

    if (link_is_running) {
      LinkState_Dashing();
      return;
    }
    link_speed_setting = 0;
    Link_CancelDash();
    if (!(button_mask_b_y & 0x80))
      link_cant_change_direction &= ~1;
    player_near_pit_state = 0;
    link_player_handler_state = !link_is_bunny_mirror ? kPlayerState_Ground :
      link_item_moon_pearl ? kPlayerState_TempBunny : kPlayerState_PermaBunny;
    if (link_player_handler_state == kPlayerState_PermaBunny)
      PlayerHandler_17_Bunny();
    else if (link_player_handler_state == kPlayerState_TempBunny)
      LinkState_TemporaryBunny();
    else
      LinkState_Default();
    return;
  }

  Player_TileDetectNearby();
  link_speed_setting = 4;
  if (!(tiledetect_pit_tile & 0xf)) {
    player_near_pit_state = 0;
    link_speed_setting = 0;
    link_player_handler_state = !link_is_bunny_mirror ? kPlayerState_Ground :
      link_item_moon_pearl ? kPlayerState_TempBunny : kPlayerState_PermaBunny;
    Link_CancelDash();
    if (!(button_mask_b_y & 0x80))
      link_cant_change_direction &= ~1;
    return;
  }

  static const uint8 kFallHolePitDirs[] = { 12, 3, 10, 5 };
  static const uint8 kFallHoleDirs[] = { 5, 6, 9, 10, 4, 8, 1, 2 };
  static const uint8 kFallHoleDirs2[] = { 10, 9, 6, 5, 8, 4, 2, 1 };

  if ((tiledetect_pit_tile & 0xf) != 0xf) {
    int i = 3;
    do {
      if ((tiledetect_pit_tile & 0xf) == kFallHolePitDirs[i]) {
        i += 4;
        goto endif_1;
      }
    } while (--i >= 0);

    i = 3;
    uint8 pit_tile;
    pit_tile= tiledetect_pit_tile;
    while (!(pit_tile & 1)) {
      i -= 1;
      pit_tile >>= 1;
    }
    assert(i >= 0);
endif_1:
    byte_7E02C9 = i;
    if (link_direction & kFallHoleDirs[i]) {
      link_direction_last = link_direction;
      link_speed_setting = 6;
      Link_HandleMovingAnimation_FullLongEntry();
    } else {
      uint8 old_dir = link_direction;
      link_direction |= kFallHoleDirs2[byte_7E02C9];
      if (old_dir)
        Link_HandleMovingAnimation_FullLongEntry();
    }
    Link_HandleDiagonalCollision();
    Link_HandleVelocity();
    Link_HandleCardinalCollision();
    ApplyLinksMovementToCamera();
    return;
  }

  // Initiate fall down
  if (player_near_pit_state != 2) {
    if (link_item_moon_pearl) {
      link_need_for_poof_for_transform = 0;
      link_is_bunny = 0;
      link_is_bunny_mirror = 0;
      link_timer_tempbunny = 0;
    }
    link_direction = 0;
    player_near_pit_state = 2;
    link_disable_sprite_damage = 1;
    button_mask_b_y = 0;
    button_b_frames = 0;
    link_item_in_hand = 0;
    link_position_mode = 0;
    link_incapacitated_timer = 0;
    link_auxiliary_state = 0;
    Ancilla_Sfx3_Near(31);
  }

  link_cant_change_direction = 0;
  link_incapacitated_timer = 0;
  link_z_coord = 0;
  link_actual_vel_z = 0;
  link_auxiliary_state = 0;
  link_give_damage = 0;
  link_is_transforming = 0;
  Link_ForceUnequipCape_quietly();
  link_disable_sprite_damage++;
  if (!sign8(--byte_7E005C))
    return;
  uint8 x = ++link_this_controls_sprite_oam;
  byte_7E005C = 9;
  if (follower_indicator != 13 && x == 1)
    tagalong_var5 = x;

  if (x == 6) {
    Link_CancelDash();
    submodule_index = 7;
    link_this_controls_sprite_oam = 6;
    player_near_pit_state = 3;
    link_visibility_status = 12;
    link_speed_modifier = 16;
    uint16 y = (uint8)(link_y_coord - BG2VOFS_copy2);
    link_state_bits = 0;
    link_picking_throw_state = 0;
    link_grabbing_wall = 0;
    some_animation_timer = 0;
    if (player_is_indoors) {
      BYTE(dungeon_room_index_prev) = dungeon_room_index;
      Dungeon_FlagRoomData_Quadrants();
      if (Dungeon_IsPitThatHurtsPlayer()) {
        DungeonPitDoDamage();
        return;
      }
    }
    BYTE(dungeon_room_index_prev) = dungeon_room_index;
    BYTE(dungeon_room_index) = dung_hdr_travel_destinations[0];
    tiledetect_which_y_pos[0] = link_y_coord;
    link_y_coord = link_y_coord - y - 0x10;
    if (player_is_indoors) {
      HandleLayerOfDestination();
    } else {
      if ((uint8)overworld_screen_index != 5) {
        Overworld_GetPitDestination();
        main_module_index = 17;
        submodule_index = 0;
        subsubmodule_index = 0;
      } else {
        TakeDamageFromPit();
      }
    }
  }
}

/*
 * HandleLayerOfDestination — Set Link's "lower level" mirror/live
 * pair from the destination dungeon room's hole-teleporter plane:
 *   plane 0: top layer
 *   plane 1: mirror only (visual layer change)
 *   plane 2: actual lower level
 */
void HandleLayerOfDestination() {  // 8794f1
  link_is_on_lower_level_mirror = (dung_hdr_hole_teleporter_plane >= 1);
  link_is_on_lower_level = (dung_hdr_hole_teleporter_plane >= 2);
}

/*
 * DungeonPitDoDamage — Subtract 8 HP for falling into a dungeon pit.
 * 0xa8 wrap check clamps to zero (death). Sets submodule 20 (the
 * "Link is hurt" damage-recovery submodule).
 */
void DungeonPitDoDamage() {  // 879502
  submodule_index = 20;
  link_health_current -= 8;
  if (link_health_current >= 0xa8)
    link_health_current = 0;
}

/*
 * HandleDungeonLandingFromPit — Per-frame driver while Link is
 * mid-fall through a dungeon pit and being placed on the destination
 * room. Animates the falling sprite (3-frame cycle, capped at 10),
 * pulls Link toward the target Y, and on landing resolves the surface:
 *   - shallow water: water-step SFX (0x24)
 *   - regular floor: thump SFX (0x21) unless the water sound is
 *     already queued.
 *   - deep water: switch to swimming, force-unequip cape, splash.
 *   - pit-on-pit: chained falling state.
 *
 * Also resets follower/tagalong state — followers don't fall through
 * holes with Link.
 */
void HandleDungeonLandingFromPit() {  // 879520
  LinkOam_Main();
  link_x_coord_prev = link_x_coord;
  link_y_coord_prev = link_y_coord;
  if (submodule_index == 7)
    link_visibility_status = 0;
  if (!(frame_counter & 3) && ++link_this_controls_sprite_oam == 10)
    link_this_controls_sprite_oam = 6;
  link_direction = 4;
  Link_HandleVelocity();
  if (sign16(link_y_coord) && !sign16(tiledetect_which_y_pos[0])) {
    if (!sign16(-link_y_coord + tiledetect_which_y_pos[0]))
      return;
  } else {
    if (tiledetect_which_y_pos[0] >= link_y_coord)
      return;
  }
  //exploration glitch could also be armed without quitting
  //by jumping off a dungeon ledge into an access pit
  if (enhanced_features0 & kFeatures0_MiscBugFixes) {
    about_to_jump_off_ledge = 0;
  }
  link_y_coord = tiledetect_which_y_pos[0];
  link_animation_steps = 0;
  link_speed_modifier = 0;
  link_this_controls_sprite_oam = 0;
  player_near_pit_state = 0;
  link_speed_setting = 0;
  subsubmodule_index = 0;
  submodule_index = 0;
  link_disable_sprite_damage = 0;
  if (follower_indicator != 0 && follower_indicator != 3) {
    tagalong_var5 = 0;
    if (follower_indicator == 13) {
      follower_indicator = 0;
      super_bomb_indicator_unk2 = 0;
      super_bomb_indicator_unk1 = 0;
      follower_dropped = 0;
    } else {
      Follower_Initialize();
    }
  }
  TileDetect_MainHandler(0);
  if (tiledetect_shallow_water & 1)
    Ancilla_Sfx2_Near(0x24);
  Player_TileDetectNearby();
  if ((sound_effect_1 & 0x3f) != 0x24)
    Ancilla_Sfx2_Near(0x21);

  if (dung_hdr_collision_2 == 2 && (tiledetect_water_staircase & 0xf))
    byte_7E0322 = 3;
  if ((tiledetect_deepwater & 0xf) == 0xf) {
    link_is_in_deep_water = 1;
    link_some_direction_bits = link_direction_last;
    Link_ResetSwimmingState();
    link_is_on_lower_level = 1;
    AncillaAdd_Splash(0x15, 1);
    link_player_handler_state = kPlayerState_Swimming;
    Link_ForceUnequipCape_quietly();
    link_state_bits = 0;
    link_picking_throw_state = 0;
    link_grabbing_wall = 0;
    link_speed_setting = 0;
  } else {
    link_player_handler_state = (tiledetect_pit_tile & 0xf) ? kPlayerState_FallingIntoHole : kPlayerState_Ground;
  }
}

/*
 * PlayerHandler_04_Swimming — Slot 4: deep-water swimming with
 * flippers. On aux-state hit, drop into recoil. Otherwise:
 *   - Suppress every sword/charge state bit (no swordplay in water).
 *   - Required item check: link_item_flippers must be set.
 *   - Idle-paddle vs swim-collision animation (different timer rates
 *     8 vs 16 frames).
 *   - Hard-stroke (filtered face-button press): plays SFX 37, kicks
 *     up swim acceleration, runs a 7-frame stroke window before
 *     re-entering normal stroke. After 5 hard strokes the
 *     button-debounce mask is cleared (link_swim_hard_stroke &= ~0xC0).
 */
void PlayerHandler_04_Swimming() {  // 87963b
  if (link_auxiliary_state) {
    link_player_handler_state = kPlayerState_RecoilWall;
    link_z_coord &= 0xff;
    ResetAllAcceleration();
    link_maybe_swim_faster = 0;
    link_swim_hard_stroke = 0;
    link_cant_change_direction &= ~1;
    LinkState_Recoil();
    return;
  }

  button_mask_b_y = 0;
  button_b_frames = 0;
  link_delay_timer_spin_attack = 0;
  link_spin_attack_step_counter = 0;
  link_state_bits = 0;
  link_picking_throw_state = 0;
  if (!link_item_flippers)
    return;

  if (!(swimcoll_var7[0] | swimcoll_var7[1])) {
    if ((uint8)swimcoll_var5[0] != 2 && (uint8)swimcoll_var5[1] != 2)
      ResetAllAcceleration();
    link_animation_steps &= 1;
    if (++link_counter_var1 >= 16) {
      link_counter_var1 = 0;
      byte_7E02CC = 0;
      link_animation_steps = (link_animation_steps & 1) ^ 1;
    }
  } else {
    if (++link_counter_var1 >= 8) {
      link_counter_var1 = 0;
      link_animation_steps = (link_animation_steps + 1) & 3;
      byte_7E02CC = kSwimmingTab1[link_animation_steps];
    }
  }

  if (!link_swim_hard_stroke) {
    uint8 t;
    if (!(swimcoll_var7[0] | swimcoll_var7[1]) || (t = ((filtered_joypad_L & kJoypadL_A) | filtered_joypad_H) & 0xc0) == 0) {
      Link_HandleSwimMovements();
      return;
    }
    link_swim_hard_stroke = t;
    Ancilla_Sfx2_Near(37);
    link_maybe_swim_faster = 1;
    swimming_countdown = 7;
    Link_HandleSwimAccels();
  }
  if (sign8(--swimming_countdown)) {
    swimming_countdown = 7;
    if (++link_maybe_swim_faster == 5) {
      link_maybe_swim_faster = 0;
      link_swim_hard_stroke &= ~0xC0;
    }
  }

  Link_HandleSwimMovements();
}

/*
 * Link_HandleSwimMovements — Shared "advance Link with current
 * direction" routine used by both the swim handler and the
 * forced-motion path (link_flag_moving). Resolves the active
 * direction from force_move bits or the d-pad, then runs the
 * standard movement chain (diagonal -> velocity -> cardinal coll
 * -> animation -> camera).
 *
 * Direction-change side effects: clear sub-pixel residue and the
 * diagonal-tile pin so a turn doesn't carry stale collision state.
 */
void Link_HandleSwimMovements() {  // 879715
  uint8 t;

  if (!(t = force_move_any_direction & 0xf) && !(t = joypad1H_last & kJoypadH_AnyDir)) {
    link_y_vel = link_x_vel = 0;
    Link_FlagMaxAccels();
    if (link_flag_moving) {
      if (link_is_running) {
        t = link_some_direction_bits;
      } else {
        if (!(swimcoll_var7[0] | swimcoll_var7[1])) {
          bitmask_of_dragstate = 0;
          Link_ResetSwimmingState();
        }
        goto out;
      }
    } else {
      if (link_player_handler_state != kPlayerState_Swimming)
        link_animation_steps = 0;
      goto out;
    }
  }

  if (t != link_some_direction_bits) {
    link_some_direction_bits = t;
    link_subpixel_x = link_subpixel_y = 0;
    link_moving_against_diag_tile = 0;
    bitmask_of_dragstate = 0;
  }
  Link_SetIceMaxAccel();
  Link_SetMomentum();
  Link_SetTheMaxAccel();
out:
  Link_HandleDiagonalCollision();
  Link_HandleVelocity();
  Link_HandleCardinalCollision();
  Link_HandleMovingAnimation_FullLongEntry();
  fallhole_var1 = 0;
  HandleIndoorCameraAndDoors();
}

/*
 * Link_FlagMaxAccels — When forced motion is active (link_flag_moving),
 * pin the swim-collision max-accel pair to the current pulse value
 * and mark them as "saturated" (var5 = 1) so the swim integrator
 * keeps Link moving at full velocity.
 */
void Link_FlagMaxAccels() {  // 879785
  if (!link_flag_moving)
    return;
  for (int i = 1; i >= 0; i--) {
    if (swimcoll_var7[i]) {
      swimcoll_var9[i] = swimcoll_var7[i];
      swimcoll_var5[i] = 1;
    }
  }
}

/*
 * Link_SetIceMaxAccel — Force both swim-coll axes to the standard
 * ice-slide acceleration (0x180). Only effective when
 * link_flag_moving is set (i.e., on a forced-motion surface).
 */
void Link_SetIceMaxAccel() {  // 8797a6
  if (!link_flag_moving)
    return;
  swimcoll_var9[0] = 0x180;
  swimcoll_var9[1] = 0x180;
}

/*
 * Link_SetMomentum — Convert held d-pad direction into per-axis
 * "swim collision" momentum. Walks the Y axis (mask 12) then the X
 * axis (mask 3). For each pressed axis: set the velocity from
 * kSwimmingTab2 (or 32 default), check whether the input direction
 * matches Link's facing (var5 = 2 = "fully aligned") or oppose
 * (var5 = 0, var11 = the perpendicular bit), and if no max-accel is
 * active, seed it to 240.
 */
void Link_SetMomentum() {  // 8797c7
  uint8 joy = joypad1H_last & kJoypadH_AnyDir;
  uint8 mask = 12, bit = 8;
  for (int i = 0; i < 2; i++, mask >>= 2, bit >>= 2) {
    if (joy & mask) {
      swimcoll_var3[i] = link_flag_moving ? kSwimmingTab2[link_flag_moving - 1] : 32;
      if (((link_some_direction_bits | link_direction) & mask) == mask) {
        swimcoll_var5[i] = 2;
      } else {
        swimcoll_var11[i] = (joy & bit) ? 0 : 1;
        swimcoll_var5[i] = 0;
      }
      if (!swimcoll_var9[i])
        swimcoll_var9[i] = 240;
    }
  }
}

/*
 * Link_ResetSwimmingState — Zero the swim countdown, hard-stroke
 * mask, and "swim faster" charge level. Also clears all swim-coll
 * acceleration via ResetAllAcceleration.
 */
void Link_ResetSwimmingState() {  // 87983a
  swimming_countdown = 0;
  link_swim_hard_stroke = 0;
  link_maybe_swim_faster = 0;
  ResetAllAcceleration();
}

/*
 * Link_ResetStateAfterDamagingPit — Clean state reset after Link
 * has fallen down a damaging pit and been replaced on the floor.
 * Pick the post-fall handler state based on bunny status, latch the
 * direction, and clear damage / pit / OAM-control flags.
 */
void Link_ResetStateAfterDamagingPit() {  // 87984b
  Link_ResetSwimmingState();
  link_player_handler_state = link_is_bunny && !link_item_moon_pearl ?
    kPlayerState_PermaBunny : kPlayerState_Ground;
  link_direction_last = link_some_direction_bits;
  link_is_in_deep_water = 0;
  link_disable_sprite_damage = 0;
  link_this_controls_sprite_oam = 0;
  player_near_pit_state = 0;
}

/*
 * ResetAllAcceleration — Zero every swim-collision/accumulator pair
 * (10 entries: var1/3/5/7/9 across 2 axes). Used everywhere movement
 * needs to start from a clean slate (room transitions, recoils,
 * dash cancels).
 */
void ResetAllAcceleration() {  // 879873
  swimcoll_var1[0] = 0;
  swimcoll_var1[1] = 0;
  swimcoll_var3[0] = 0;
  swimcoll_var3[1] = 0;
  swimcoll_var5[0] = 0;
  swimcoll_var5[1] = 0;
  swimcoll_var7[0] = 0;
  swimcoll_var7[1] = 0;
  swimcoll_var9[0] = 0;
  swimcoll_var9[1] = 0;
}

/*
 * Link_HandleSwimAccels — Hard-stroke acceleration ramp during a
 * swim. Per axis pressed on the d-pad: if already at peak (var7 set,
 * var9 >= 384) snap var9 to the next entry above var7 in
 * kSwimmingTab3; otherwise ramp var9 by +160 toward 384, or seed
 * (var7 = 1, var9 = 240) if uninitialized. Produces the "stroke
 * harder = swim faster" feel.
 */
void Link_HandleSwimAccels() {  // 8798a8
  static const  uint16 kSwimmingTab3[] = { 128, 160, 192, 224, 256, 288, 320, 352, 384 };
  uint8 mask = 12;
  for (int i = 0; i < 2; i++, mask >>= 2) {
    if (joypad1H_last & mask) {
      if (swimcoll_var7[i] && swimcoll_var9[i] >= 384) {
        uint16 t;
        for (int j = 0; j < 9 && (t = kSwimmingTab3[j]) < swimcoll_var7[i]; j++) {}
        swimcoll_var9[i] = t;
      } else {
        uint16 t = swimcoll_var9[i];
        if (t) {
          t += 160;
          if (t >= 384)
            t = 384;
          swimcoll_var9[i] = t;
        } else {
          swimcoll_var7[i] = 1;
          swimcoll_var9[i] = 240;
        }
      }
    }
  }
}

/*
 * Link_SetTheMaxAccel — Steady-state swim acceleration tick used
 * outside hard-stroke / forced-motion. For each pressed axis:
 *   - If we're not fully aligned (var5 != 2) AND either we have a
 *     held-stroke accumulator (var1) or we've hit peak (var7 >= 240
 *     and >= var9): switch to "saturated" state.
 *   - Otherwise reset to the baseline cruise (var9 = 240, var1 = 0).
 */
void Link_SetTheMaxAccel() {  // 879903
  if (link_flag_moving || link_swim_hard_stroke)
    return;
  uint8 mask = 12;
  for (int i = 0; i < 2; i++, mask >>= 2) {
    if ((joypad1H_last & mask) && swimcoll_var5[i] != 2) {
      if (swimcoll_var1[i] || swimcoll_var7[i] >= 240 && swimcoll_var7[i] >= swimcoll_var9[i]) {
        swimcoll_var5[i] = 0;
        if (swimcoll_var7[i] >= 240) {
          swimcoll_var1[i] = 1;
          swimcoll_var5[i] = 1;
        } else {
          swimcoll_var9[i] = 240;
          swimcoll_var1[i] = 0;
        }
      }
    } else {
      swimcoll_var9[i] = 240;
      swimcoll_var1[i] = 0;
    }
  }
}

/*
 * LinkState_Zapped — Slot 7: Link is being electrocuted. Drives the
 * 8-cycle palette flicker (alternating Palette_ElectroThemedGear
 * with the normal gear palette every other frame at delay-2). Each
 * cycle also runs the mosaic effect via LinkZap_HandleMosaic. After
 * 8 cycles, exit to ground state and restore normal palettes.
 */
void LinkState_Zapped() {  // 87996c
  CacheCameraPropertiesIfOutdoors();
  LinkZap_HandleMosaic();
  if (!sign8(--link_delay_timer_spin_attack))
    return;
  link_delay_timer_spin_attack = 2;
  player_handler_timer++;
  if (player_handler_timer & 1)
    Palette_ElectroThemedGear();
  else
    LoadActualGearPalettes();
  if (player_handler_timer == 8) {
    player_handler_timer = 0;
    link_player_handler_state = kPlayerState_Ground;
    link_disable_sprite_damage = 0;
    link_electrocute_on_touch = 0;
    link_auxiliary_state = 0;
    Player_SetCustomMosaicLevel(0);
  }
}

/*
 * PlayerHandler_15_HoldItem — Slot 21: holding-an-item-overhead
 * pose. The original engine intentionally has no per-frame logic
 * here; the pose is driven by Link_ReceiveItem and the item-receipt
 * ancilla, which terminates the hold when complete.
 */
void PlayerHandler_15_HoldItem() {  // 8799ac
  // empty by design
}

/*
 * Link_ReceiveItem — Player has just acquired a new item from a
 * chest, NPC, or puzzle reward. Sets up the "holding it overhead"
 * cutscene:
 *   - Cancel any aux state and damage blink.
 *   - Latch the item id and item-receipt SFX (special case 0x3e
 *     plays an extra trumpet via Sfx3 #0x2e).
 *   - For receipt method 0 or 3 (the common chest-style methods),
 *     transition to slot 21 (HoldUpItem) with the appropriate pose:
 *     pose 1 = single-handed, pose 2 = two-handed (Master Sword).
 *   - Spawn the ItemReceipt ancilla which drives the actual pickup
 *     animation and dialogue.
 *   - Refresh the HUD icon (except for tunic upgrades 0x37..0x39
 *     and the magic-flute (0x20) which have their own UI updates).
 *   - Cancel any in-progress dash.
 */
void Link_ReceiveItem(uint8 item, int chest_position) {  // 8799ad
  if (link_auxiliary_state) {
    link_auxiliary_state = 0;
    link_incapacitated_timer = 0;
    countdown_for_blink = 0;
    link_state_bits = 0;
  }
  link_receiveitem_index = item;
  if (item == 0x3e)
    Ancilla_Sfx3_Near(0x2e);
  link_receiveitem_var1 = 0x60;
  if (item_receipt_method == 0 || item_receipt_method == 3) {
    link_state_bits = 0;
    button_mask_b_y = 0;
    bitfield_for_a_button = 0;
    button_b_frames = 0;
    link_speed_setting = 0;
    link_cant_change_direction = 0;
    link_item_in_hand = 0;
    link_position_mode = 0;
    player_handler_timer = 0;
    link_player_handler_state = kPlayerState_HoldUpItem;
    link_pose_for_item = 1;
    link_disable_sprite_damage = 1;
    if (item == 0x20)
      link_pose_for_item = 2;
  }
  AncillaAdd_ItemReceipt(0x22, 4, chest_position);
  if (item != 0x20 && item != 0x37 && item != 0x38 && item != 0x39)
    Hud_RefreshIcon();
  Link_CancelDash();
}

/*
 * Link_TuckIntoBed — Intro cutscene helper: place Link at the fixed
 * coordinates of the bed in his uncle's house, switch to slot 22
 * (AsleepInBed), and spawn the blanket-overlay ancilla. The 3-frame
 * countdown_for_dash gates the snore/wake animation timing.
 */
void Link_TuckIntoBed() {  // 879a2c
  link_y_coord = 0x215a;
  link_x_coord = 0x940;
  link_player_handler_state = kPlayerState_AsleepInBed;
  player_sleep_in_bed_state = 0;
  link_pose_during_opening = 0;
  link_countdown_for_dash = 3;
  AncillaAdd_Blanket(0x20);
}

/*
 * LinkState_Sleeping — Slot 22: Link asleep in bed (intro). Three
 * sub-states tracked by player_sleep_in_bed_state:
 *   0: snore loop. Every 32 frames spawn a snore Z ancilla.
 *   1: prompted wake. After the dash-counter timer ticks, any face-
 *      button input advances the wake-up pose and steps to state 2.
 *   2+: progress through the wake animation frames triggered by
 *      ancilla 0x3a callbacks.
 *
 * Triggered out of sleep by Princess Zelda's telepathic call (sets
 * state to 1 from external code).
 */
void LinkState_Sleeping() {  // 879a5a
  switch (player_sleep_in_bed_state) {
  case 0:
    if (!(frame_counter & 0x1f))
      AncillaAdd_Snoring(0x21, 1);
    break;
  case 1:
    if (submodule_index == 0 && sign8(--link_countdown_for_dash)) {
      link_countdown_for_dash = 0;
      if (((filtered_joypad_H & 0xe0) | (filtered_joypad_H << 4) | filtered_joypad_L) & 0xf0) {
        link_pose_during_opening++;
        link_direction_facing = 6;
        player_sleep_in_bed_state++;
        link_countdown_for_dash = 4;
      }
    }
    break;
  case 2:
    if (sign8(--link_countdown_for_dash)) {
      link_actual_vel_y = 4;
      link_actual_vel_x = 21;
      link_actual_vel_z = 24;
      link_actual_vel_z_copy = 24;
      link_incapacitated_timer = 16;
      link_auxiliary_state = 2;
      link_player_handler_state = kPlayerState_RecoilOther;
    }
    break;
  }
}

/*
 * Link_HandleSwordCooldown — Per-frame sword-button polling. Decrement
 * the post-swing cooldown; while it's still positive, eat the input.
 * Once it expires:
 *   - If an item is in hand, the sword is locked out (return).
 *   - During the wind-up frames (button_b_frames < 9): only check
 *     for a new swing trigger, and only if not dashing (dashing has
 *     its own swing path).
 *   - On the held-charge frames (>= 9): drive HandleSwordControls
 *     which manages the spin attack release.
 */
void Link_HandleSwordCooldown() {  // 879ac2
  if (!sign8(--link_sword_delay_timer))
    return;

  link_sword_delay_timer = 0;
  if (link_item_in_hand | link_position_mode)
    return;

  if (button_b_frames < 9) {
    if (!link_is_running)
      Link_CheckForSwordSwing();
  } else {
    HandleSwordControls();
  }

}

/*
 * Link_HandleYItem — Big dispatch over current_item_active. Reads
 * the Y-button (or chord-bound shoulder buttons) state and runs the
 * appropriate item-use handler. Per-frame this is the entry point
 * for almost every item action.
 *
 * Bunny restrictions: only items 11 (bottle) and 20 (mirror) work
 * in bunny form; the rest early-return.
 *
 * Archer-game / shovel-game special case: the mini-game module
 * forces Bow or Shovel handling regardless of current_item_active.
 *
 * Chord buttons: when no Y is pressed AND no item is mid-use,
 * GetCurrentItemButtonIndex resolves the X/L/R chord bindings into
 * an item id and synthesizes a "Y is pressed" view of the joypad
 * for the item handler. The original joypad / bottle state is
 * restored on the way out so other systems aren't affected.
 *
 * Switch-cleanup hooks: deactivating the flute (item 8) clears the
 * fast-bird-call mask; deactivating the cape (item 19) force-
 * unequips it.
 */
void Link_HandleYItem() {  // 879b0e
  if (button_b_frames && button_b_frames < 9)
    return;

  uint8 item = current_item_y;

  if (link_is_bunny_mirror && (item != 11 && item != 20))
    return;

  if (is_archer_or_shovel_game && !link_is_bunny_mirror) {
    if (is_archer_or_shovel_game == 2)
      LinkItem_Bow();
    else
      LinkItem_Shovel();
    return;
  }

  uint8 old_down = joypad1H_last, old_pressed = filtered_joypad_H, old_bottle = link_item_bottle_index;
  if ((link_item_in_hand | link_position_mode) == 0 && !(old_down & kJoypadH_Y)) {
    // Is any special key held down?
    int btn_index = GetCurrentItemButtonIndex();
    if (btn_index != 0) {
      uint8 *cur_item_ptr = GetCurrentItemButtonPtr(btn_index);
      if (*cur_item_ptr) {
        if (*cur_item_ptr >= kHudItem_Bottle1)
          link_item_bottle_index = *cur_item_ptr - kHudItem_Bottle1 + 1;
        item = Hud_LookupInventoryItem(*cur_item_ptr);
        // Pretend it's actually Y that's down
        joypad1H_last = old_down | kJoypadH_Y;
        static const uint8 kButtonIndexKeys[4] = { 0, kJoypadL_X, kJoypadL_L, kJoypadL_R };
        filtered_joypad_H = old_pressed | ((filtered_joypad_L & kButtonIndexKeys[btn_index]) ? kJoypadH_Y : 0);
      }
    }
  }

  if (item != current_item_active) {
    if (current_item_active == 8 && (link_item_flute & 2))
      button_mask_b_y &= ~0x40;
    if (current_item_active == 19 && link_cape_mode)
      Link_ForceUnequipCape();
  }


  if ((link_item_in_hand | link_position_mode) == 0)
    current_item_active = item;

  if (current_item_active == 5 || current_item_active == 6)
    eq_selected_rod = current_item_active - 5 + 1;

  switch (current_item_active) {
  case 0:
    break;
  case 1: LinkItem_Bombs(); break;
  case 2: LinkItem_Boomerang(); break;
  case 3: LinkItem_Bow(); break;
  case 4: LinkItem_Hammer(); break;
  case 5: LinkItem_Rod(); break;
  case 6: LinkItem_Rod(); break;
  case 7: LinkItem_Net(); break;
  case 8: LinkItem_ShovelAndFlute(); break;
  case 9: LinkItem_Lamp(); break;
  case 10: LinkItem_Powder(); break;
  case 11: LinkItem_Bottle(); break;
  case 12: LinkItem_Book(); break;
  case 13: LinkItem_CaneOfByrna(); break;
  case 14: LinkItem_Hookshot(); break;
  case 15: LinkItem_Bombos(); break;
  case 16: LinkItem_Ether(); break;
  case 17: LinkItem_Quake(); break;
  case 18: LinkItem_CaneOfSomaria(); break;
  case 19: LinkItem_Cape(); break;
  case 20: LinkItem_Mirror(); break;
  case 21: LinkItem_Shovel(); break;
  default:
    assert(0);
  }

  joypad1H_last = old_down;
  filtered_joypad_H = old_pressed;
  link_item_bottle_index = old_bottle;
}

/*
 * Link_HandleAPress — A-button context dispatcher. Picks one of:
 *   action 1: lift / pick up a tile or sprite
 *   action 6: push moveable statue
 *   action 7: pull tree branch (need_for_pullforrupees_sprite + face north)
 *   action 0/2/3/4/5: derived from Link_HandleLiftables (read sign,
 *                    check chest, talk, etc.)
 *
 * Each action is gated by the kAbilityBitmasks entry, which is
 * AND-ed with link_ability_flags (item progression flags) — e.g.,
 * lift requires Power Glove (bit 0x40), and pull requires the
 * "found-the-pull-tree" flag (0x04). Failing the gate clears the
 * pending button mask so a held A doesn't re-fire.
 *
 * Once the action passes its gate, Link_APress_PerformBasic runs
 * the actual state transition.
 */
void Link_HandleAPress() {  // 879baa
  flag_is_sprite_to_pick_up_cached = 0;
  if (link_item_in_hand || (link_position_mode & 0x1f) || byte_7E0379)
    return;

  if (button_b_frames < 9 && (button_mask_b_y & 0x80))
    return;

  uint8 action = tile_action_index;

  if ((link_state_bits | link_grabbing_wall) == 0) {
    if (!Link_CheckNewAPress()) {
      bitfield_for_a_button = 0;
      return;
    }

    if (link_need_for_pullforrupees_sprite && !link_direction_facing) {
      action = 7;
    } else if (link_is_near_moveable_statue) {
      action = 6;
    } else {
      if (!flag_is_ancilla_to_pick_up) {
        if (!flag_is_sprite_to_pick_up) {
          action = Link_HandleLiftables();
          goto attempt_action;
        }
        flag_is_sprite_to_pick_up_cached = flag_is_sprite_to_pick_up;
      }

      if (button_b_frames)
        Link_ResetSwordAndItemUsage();

      if (link_item_in_hand | link_position_mode) {
        link_item_in_hand = 0;
        link_position_mode = 0;
        Link_ResetBoomerangYStuff();
        flag_for_boomerang_in_place = 0;
        if (ancilla_type[0] == 5)
          ancilla_type[0] = 0;
      }
      action = 1;
    }
    static const uint8 kAbilityBitmasks[] = { 0xE0, 0x40, 4, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0 };
attempt_action:
    if (!(kAbilityBitmasks[action] & link_ability_flags)) {
      bitfield_for_a_button = 0;
      return;
    }

    tile_action_index = action;
    Link_APress_PerformBasic(action * 2);
  }

  // actionInProgress
  unused_2 = tile_action_index;
  switch (tile_action_index) {
  case 1: Link_APress_LiftCarryThrow(); break;
  case 3: Link_APress_PullObject(); break;
  case 6: Link_APress_StatueDrag(); break;
  }
}

void Link_APress_PerformBasic(uint8 action_x2) {  // 879c5f
  switch (action_x2 >> 1) {
  case 0: Link_PerformDesertPrayer(); break;
  case 1: Link_PerformThrow(); break;
  case 2: Link_PerformDash(); break;
  case 3: Link_PerformGrab(); break;
  case 4: Link_PerformRead(); break;
  case 5: Link_PerformOpenChest(); break;
  case 6: Link_PerformStatueDrag(); break;
  case 7: Link_PerformRupeePull(); break;
  default:
    assert(0);
  }
}

/*
 * HandleSwordSfxAndBeam — Fire a sword beam (when at full health AND
 * holding the Master Sword or higher) and play the swing SFX.
 * The 4-HP threshold (capacity - 4) corresponds to the SNES heart-
 * container fractional buffer; "below max" means "no beam".
 * Also short-circuits if a beam ancilla (type 0x31) is already alive.
 */
void HandleSwordSfxAndBeam() {  // 879c66
  link_direction &= ~0xf;
  button_b_frames = 0;
  link_spin_attack_step_counter = 0;

  uint8 health = link_health_capacity - 4;
  if (health < link_health_current && ((link_sword_type + 1) & 0xfe) && link_sword_type >= 2) {
    int i = 4;
    while (ancilla_type[i] != 0x31) {
      if (--i < 0) {
        AddSwordBeam(0);
        break;
      }
    }
  }
  uint8 sword = link_sword_type - 1;
  if (sword != 0xfe && sword != 0xff)
    sound_effect_1 = kFireBeamSounds[sword] | Link_CalculateSfxPan();
  link_delay_timer_spin_attack = 1;
}

/*
 * Link_CheckForSwordSwing — Detect a fresh sword-swing trigger and
 * advance the swing's per-frame state machine. On a new B-press,
 * runs the doorway-tile guard (no swinging through walls), fires
 * the SFX/beam, and locks the facing direction. Subsequent frames
 * step button_b_frames through the per-frame timing in
 * kSpinAttackDelays, spawning the swing-sparkle ancilla on frame 5
 * (Master Sword+) and running the hitbox calculation each frame.
 */
void Link_CheckForSwordSwing() {  // 879cd9
  if (bitfield_for_a_button & 0x10)
    return;

  if (!(button_mask_b_y & 0x80)) {
    if (!(filtered_joypad_H & 0x80))
      return;
    if (is_standing_in_doorway) {
      TileDetect_SwordSwingDeepInDoor(is_standing_in_doorway);
      if ((R14 & 0x30) == 0x30)
        return;
    }
    button_mask_b_y |= 0x80;
    HandleSwordSfxAndBeam();
    link_cant_change_direction |= 1;
    link_animation_steps = 0;
  }

  if (!(joypad1H_last & kJoypadH_B))
    button_mask_b_y |= 1;
  HaltLinkWhenUsingItems();
  link_direction &= ~0xf;
  if (sign8(--link_delay_timer_spin_attack)) {
    if (++button_b_frames >= 9) {
      HandleSwordControls();
      return;
    }
    link_delay_timer_spin_attack = kSpinAttackDelays[button_b_frames];
    if (button_b_frames == 5) {
      if (link_sword_type != 0 && link_sword_type != 1 && link_sword_type != 0xff)
        AncillaAdd_SwordSwingSparkle(0x26, 4);
      if (link_sword_type != 0 && link_sword_type != 0xff)
        TileDetect_MainHandler(link_sword_type == 1 ? 1 : 6);
    } else if (button_b_frames >= 4 && (button_mask_b_y & 1) && (joypad1H_last & kJoypadH_B)) {
      button_mask_b_y &= ~1;
      HandleSwordSfxAndBeam();
      return;
    }
  }
  CalculateSwordHitBox();
}

/*
 * HandleSwordControls — Manage the post-swing-window: B held = run
 * the spin-attack charge tick; B released past the 48-frame charge
 * threshold = trigger the spin-attack release; B released earlier =
 * just clean up.
 */
void HandleSwordControls() {  // 879d72
  if (joypad1H_last & kJoypadH_B) {
    Player_Sword_SpinAttackJerks_HoldDown();
  } else {
    if (link_spin_attack_step_counter < 48) {
      Link_ResetSwordAndItemUsage();
    } else {
      Link_ResetSwordAndItemUsage();
      link_spin_attack_step_counter = 0;
      Link_ActivateSpinAttack();
    }
  }
}

/*
 * Link_ResetSwordAndItemUsage — Cancel a sword swing / item use and
 * unlock direction control. Clears the speed bonus, drag-state bits,
 * spin-attack timer, swing-frame counter, and the (B|Y) button
 * latch bits.
 */
void Link_ResetSwordAndItemUsage() {  // 879d84
  link_speed_setting = 0;
  bitmask_of_dragstate &= ~9;
  link_delay_timer_spin_attack = 0;
  button_b_frames = 0;
  button_mask_b_y &= ~0x81;
  link_cant_change_direction &= ~1;
}

/*
 * Player_Sword_SpinAttackJerks_HoldDown — Per-frame sword-charge
 * tick while B is held past the swing window.
 *
 * Two scenarios in the if-cluster:
 *   1. Free charge (no drag-state collision): clamp speed at 12
 *      (slow walk while charging), drop a sparkle every 4 frames
 *      after step 6, fire the "fully charged" SFX (55) and the
 *      sustained-sparkle ancilla on step 48.
 *   2. Holding into a wall (set_when_damaging_enemies == 1): cancel.
 *
 * The else block animates the "wall tap" — when the held sword
 * scrapes a wall, ramp the swing-frame counter past 9 and on hitting
 * 13 spawn a wall-spark ancilla, play SFX 5/6, and re-enter the
 * tile-detect collision so the wall registers a hit.
 */
void Player_Sword_SpinAttackJerks_HoldDown() {  // 879d9f
  if ((bitmask_of_dragstate & 0x80) || (bitmask_of_dragstate & 9) == 0) {
    if (set_when_damaging_enemies == 0) {
      button_b_frames = 9;
      link_cant_change_direction |= 1;
      link_delay_timer_spin_attack = 0;
      if (link_speed_setting != 4 && link_speed_setting != 16) {
        link_speed_setting = 12;
        if (!((uint8)(link_sword_type + 1) & ~1))
          return;
        int i = 4;
        do {
          if (ancilla_type[i] == 0x30 || ancilla_type[i] == 0x31)
            return;
        } while (--i >= 0);

        if (link_spin_attack_step_counter >= 6 && (frame_counter & 3) == 0)
          AncillaSpawn_SwordChargeSparkle();

        if (link_spin_attack_step_counter < 64 && ++link_spin_attack_step_counter == 48) {
          Ancilla_Sfx2_Near(55);
          AncillaAdd_ChargedSpinAttackSparkle();
        }
      } else {
        CalculateSwordHitBox();
      }
      return;
    } else if (set_when_damaging_enemies == 1) {
      Link_ResetSwordAndItemUsage();
      return;
    }
  }
  // endif_2
  if (button_b_frames == 9) {
    button_b_frames = 10;
    link_delay_timer_spin_attack = kSpinAttackDelays[button_b_frames];
  }

  if (sign8(--link_delay_timer_spin_attack)) {
    uint8 frames = button_b_frames + 1;
    if (frames == 13) {
      if ((uint8)(link_sword_type + 1) & ~1 && (bitmask_of_dragstate & 9)) {
        AncillaAdd_WallTapSpark(27, 1);
        Ancilla_Sfx2_Near((bitmask_of_dragstate & 8) ? 6 : 5);
        TileDetect_MainHandler(1);
      }
      frames = 10;
    }
    button_b_frames = frames;
    link_delay_timer_spin_attack = kSpinAttackDelays[button_b_frames];
  }
  CalculateSwordHitBox();
}

/*
 * LinkItem_Rod — Fire/Ice rod use handler. eq_selected_rod (1=fire,
 * 2=ice) picks which projectile ancilla to spawn. Costs magic;
 * locks Link in place via HaltLinkWhenUsingItems for a 3-frame
 * windup, then the projectile flies. link_item_in_hand = 1 marks
 * the item slot as occupied so other systems hide the sword.
 */
void LinkItem_Rod() {  // 879eef
  static const uint8 kRodAnimDelays[] = { 3, 3, 5 };
  if (!(button_mask_b_y & 0x40)) {
    if (is_standing_in_doorway || !CheckYButtonPress())
      return;
    if (!LinkCheckMagicCost(0))
      goto out;
    link_debug_value_2 = 1;
    if (eq_selected_rod == 1)
      AncillaAdd_FireRodShot(2, 1);
    else
      AncillaAdd_IceRodShot(11, 1);
    link_delay_timer_spin_attack = kRodAnimDelays[0];
    link_animation_steps = 0;
    player_handler_timer = 0;
    link_item_in_hand = 1;
  }
  HaltLinkWhenUsingItems();
  link_direction &= ~0xf;
  if (!sign8(--link_delay_timer_spin_attack))
    return;
  player_handler_timer++;

  link_delay_timer_spin_attack = kRodAnimDelays[player_handler_timer];
  if (player_handler_timer != 3)
    return;
  link_debug_value_2 = 0;
  link_speed_setting = 0;
  player_handler_timer = 0;
  link_delay_timer_spin_attack = 0;
  link_item_in_hand &= ~1;
out:
  button_mask_b_y &= ~0x40;
}

/*
 * LinkItem_Hammer — 3-phase hammer swing. Phase 0 (raise), 1 (drop
 * + impact: TileDetect_MainHandler(3) checks for hammerable tiles,
 * spawns hit-stars, plays SFX 16 unless splash already firing),
 * 2 (recovery, 16 frames). 0x10 in link_item_in_hand is the
 * "currently swinging" lock to prevent retrigger.
 */
void LinkItem_Hammer() {  // 879f7b
  static const uint8 kHammerAnimDelays[] = { 3, 3, 16 };
  if (link_item_in_hand & 0x10)
    return;
  if (!(button_mask_b_y & 0x40)) {
    if (is_standing_in_doorway || !(filtered_joypad_H & kJoypadH_Y))
      return;
    button_mask_b_y |= 0x40;
    link_delay_timer_spin_attack = kHammerAnimDelays[0];
    link_cant_change_direction |= 1;
    link_animation_steps = 0;
    player_handler_timer = 0;
    link_item_in_hand = 2;
  }

  HaltLinkWhenUsingItems();
  link_direction &= ~0xf;
  if (!sign8(--link_delay_timer_spin_attack))
    return;
  player_handler_timer++;

  link_delay_timer_spin_attack = kHammerAnimDelays[player_handler_timer];
  if (player_handler_timer == 1) {
    TileDetect_MainHandler(3);
    Ancilla_AddHitStars(22, 0);
    if (sound_effect_1 == 0) {
      Ancilla_Sfx2_Near(16);
      SpawnHammerWaterSplash();
    }
  } else if (player_handler_timer == 3) {
    player_handler_timer = 0;
    link_delay_timer_spin_attack = 0;
    button_mask_b_y &= ~0x40;
    link_cant_change_direction &= ~1;
    link_item_in_hand &= ~2;
  }
}

/*
 * LinkItem_Bow — 3-phase bow draw. Phase 0 (nock), 1 (aim, 3
 * frames), 2 (release, 8 frames -> AncillaAdd_Arrow). Special-cases
 * the archery mini-game (consumes mini-game arrows, awards 2 to
 * normal pool) and the empty-quiver beep (SFX 60). After release,
 * caps button_b_frames at 9 so the bow doesn't auto-charge a sword.
 */
void LinkItem_Bow() {  // 87a006
  static const uint8 kBowDelays[] = { 3, 3, 8 };

  if (!(button_mask_b_y & 0x40)) {
    if (is_standing_in_doorway || !CheckYButtonPress())
      return;
    link_cant_change_direction |= 1;
    link_delay_timer_spin_attack = kBowDelays[0];
    link_animation_steps = 0;
    player_handler_timer = 0;
    link_item_in_hand = 16;
  }
  HaltLinkWhenUsingItems();
  link_direction &= ~0xf;
  if (!sign8(--link_delay_timer_spin_attack))
    return;
  player_handler_timer++;

  link_delay_timer_spin_attack = kBowDelays[player_handler_timer];
  if (player_handler_timer != 3)
    return;

  int obj = AncillaAdd_Arrow(9, link_direction_facing, 2, link_x_coord, link_y_coord);
  if (obj >= 0) {
    if (archery_game_arrows_left) {
      archery_game_arrows_left--;
      link_num_arrows += 2;
    }
    if (!archery_game_out_of_arrows && link_num_arrows) {
      if (--link_num_arrows == 0)
        Hud_RefreshIcon();
    } else {
      ancilla_type[obj] = 0;
      Ancilla_Sfx2_Near(60);
    }
  }

  player_handler_timer = 0;
  link_delay_timer_spin_attack = 0;
  button_mask_b_y &= ~0x40;
  link_cant_change_direction &= ~1;
  link_item_in_hand &= ~0x10;
  if (button_b_frames >= 9)
    button_b_frames = 9;
}

/*
 * LinkItem_Boomerang — Throw the wooden / magical boomerang. Spawns
 * the Boomerang ancilla; the ancilla itself manages the out-and-
 * back arc and re-catch. flag_for_boomerang_in_place suppresses
 * re-throw while one is already airborne. 0x80 in link_item_in_hand
 * is the boomerang lock; cleared by Link_ResetBoomerangYStuff.
 */
void LinkItem_Boomerang() {  // 87a0bb
  if (!(button_mask_b_y & 0x40)) {
    if (is_standing_in_doorway || !CheckYButtonPress() || flag_for_boomerang_in_place)
      return;
    link_animation_steps = 0;
    link_item_in_hand = 0x80;
    player_handler_timer = 0;
    link_delay_timer_spin_attack = 7;

    int s0 = AncillaAdd_Boomerang(5, 0);

    if (button_b_frames >= 9) {
      Link_ResetBoomerangYStuff();
      return;
    }

    if (!s0) {
      link_direction_last = joypad1H_last & kJoypadH_AnyDir;
    } else {
      link_cant_change_direction |= 1;
    }
  } else {
    link_cant_change_direction |= 1;
  }

  if (link_item_in_hand) {
    HaltLinkWhenUsingItems();
    link_direction &= ~0xf;
    if (!sign8(--link_delay_timer_spin_attack))
      return;
    link_delay_timer_spin_attack = 5;
    if (++player_handler_timer != 2)
      return;
  }
  Link_ResetBoomerangYStuff();
}

/*
 * Link_ResetBoomerangYStuff — Cleanup helper after a boomerang
 * throw or cancel: clear the in-hand bit, button latch, and (if no
 * sword is held) unlock direction.
 */
void Link_ResetBoomerangYStuff() {  // 87a11f
  link_item_in_hand = 0;
  player_handler_timer = 0;
  link_delay_timer_spin_attack = 0;
  button_mask_b_y &= ~0x40;
  if (!(button_mask_b_y & 0x80))
    link_cant_change_direction &= ~1;
}

/*
 * LinkItem_Bombs — Drop a bomb at Link's feet. Suppressed in
 * doorways and when the dwarf NPC (follower 13) is along (he
 * panics). The kFeatures0_MoreActiveBombs enhancement raises the
 * concurrent-bomb cap from 1 to 3.
 */
void LinkItem_Bombs() {  // 87a138
  if (is_standing_in_doorway || follower_indicator == 13 || !CheckYButtonPress())
    return;
  button_mask_b_y &= ~0x40;
  AncillaAdd_Bomb(7, enhanced_features0 & kFeatures0_MoreActiveBombs ? 3 : 1);
  link_item_in_hand = 0;
}

/*
 * LinkItem_Bottle — Use the currently-selected bottle's contents.
 * Dispatch over the bottle byte (link_bottle_info[btidx]):
 *   0       : empty (nothing to do)
 *   1, 2    : empty / "stale" (SFX 60 = error beep)
 *   3       : red potion -> heart refill, jump to module 14 sub 4
 *   4       : green potion -> magic refill
 *   5       : blue potion -> both refills
 *   6       : fairy -> auto-revive on death (handled elsewhere)
 *   7..     : bee/good bee/captured fairy -> released as ancilla
 * On consumption the bottle slot is replaced with id 2 (empty).
 */
void LinkItem_Bottle() {  // 87a15b
  if (!CheckYButtonPress())
    return;
  button_mask_b_y &= ~0x40;
  int btidx = link_item_bottle_index - 1;
  uint8 b = link_bottle_info[btidx];
  if (b == 0)
    return;
  if (b < 3) {
fail:
    Ancilla_Sfx2_Near(60);
  } else if (b == 3) {  // red potion
    if (link_health_capacity == link_health_current)
      goto fail;
    link_bottle_info[btidx] = 2;
    link_item_in_hand = 0;
    submodule_index = 4;
    saved_module_for_menu = main_module_index;
    main_module_index = 14;
    animate_heart_refill_countdown = 7;
    Hud_Rebuild();
  } else if (b == 4) { // green potion
    if (link_magic_power == 128)
      goto fail;
    link_bottle_info[btidx] = 2;
    link_item_in_hand = 0;
    submodule_index = 8;
    saved_module_for_menu = main_module_index;
    main_module_index = 14;
    animate_heart_refill_countdown = 7;
    Hud_Rebuild();
  } else if (b == 5) { // blue potion
    if (link_health_capacity == link_health_current && link_magic_power == 128)
      goto fail;
    link_bottle_info[btidx] = 2;
    link_item_in_hand = 0;
    submodule_index = 9;
    saved_module_for_menu = main_module_index;
    main_module_index = 14;
    animate_heart_refill_countdown = 7;
    Hud_Rebuild();
  } else if (b == 6) { // fairy
    link_item_in_hand = 0;
    if (ReleaseFairy() < 0)
      goto fail;
    link_bottle_info[btidx] = 2;
    Hud_Rebuild();
  } else if (b == 7 || b == 8) {  // bad/good bee
    if (!ReleaseBeeFromBottle(btidx))
      goto fail;
    link_bottle_info[btidx] = 2;
    Hud_Rebuild();
  }
}

/*
 * LinkItem_Lamp — Light a torch (or just flash the lamp). Works
 * only on tiles with the kind=torch attribute (link_item_torch is
 * set by tile-detect when standing next to one). Costs magic via
 * LinkCheckMagicCost(6); spawns the cloud-of-flame ancilla.
 */
void LinkItem_Lamp() {  // 87a24d
  if (is_standing_in_doorway || !CheckYButtonPress())
    return;
  if (link_item_torch && LinkCheckMagicCost(6)) {
    AncillaAdd_MagicPowder(0x1a, 0);
    Dungeon_LightTorch();
    AncillaAdd_LampFlame(0x2f, 2);
  }
  link_item_in_hand = 0;
  button_mask_b_y = 0;
  button_b_frames = 0;
  link_cant_change_direction = 0;
  if (button_b_frames == 9)
    link_speed_setting = 0;
}

/*
 * LinkItem_Powder — Sprinkle Magic Powder. 9-frame sequence driven
 * by kMushroomTimer; on frame 4 spawns the powder ancilla, on frame
 * 9 runs the tile-detect (1 = "powder applied to a tile") so the
 * tile-mutation system (e.g., turning a bush into a fairy spawn)
 * can react. link_item_in_hand bit 0x40 marks the powder lock.
 */
void LinkItem_Powder() {  // 87a293
  static const uint8 kMushroomTimer[] = { 2, 1, 1, 3, 2, 2, 2, 2, 6, 0 };

  if (!(button_mask_b_y & 0x40)) {
    if (is_standing_in_doorway || !CheckYButtonPress())
      return;
    if (link_item_mushroom != 2) {
      Ancilla_Sfx2_Near(60);
      goto out;
    }
    if (!LinkCheckMagicCost(2))
      goto out;
    link_delay_timer_spin_attack = kMushroomTimer[0];
    player_handler_timer = 0;
    link_animation_steps = 0;
    link_direction &= ~0xf;
    link_item_in_hand = 0x40;
  }
  link_x_vel = link_y_vel = 0;
  link_direction = 0;
  link_subpixel_x = link_subpixel_y = 0;
  link_moving_against_diag_tile = 0;
  if (!sign8(--link_delay_timer_spin_attack))
    return;
  player_handler_timer++;
  link_delay_timer_spin_attack = kMushroomTimer[player_handler_timer];
  if (player_handler_timer == 4)
    AncillaAdd_MagicPowder(26, 0);
  if (player_handler_timer != 9)
    return;
  if (submodule_index == 0)
    TileDetect_MainHandler(1);
out:
  link_item_in_hand = 0;
  player_handler_timer = 0;
  button_mask_b_y &= ~0x40;
}

/*
 * LinkItem_ShovelAndFlute — Inventory slot 8 is shared between the
 * shovel and the flute (the flute upgrades to flute-with-bird).
 * link_item_flute == 1 means just the shovel; non-zero non-1 means
 * either flute or upgraded flute.
 */
void LinkItem_ShovelAndFlute() {  // 87a313
  if (link_item_flute == 1)
    LinkItem_Shovel();
  else if (link_item_flute != 0)
    LinkItem_Flute();
}

/*
 * LinkItem_Shovel — 3-step shovel dig (raise / strike / recover).
 * On strike, runs TileDetect_MainHandler(2): if the tile is
 * diggable, spawns shovel-dirt ancilla and (in the digging mini-
 * game) calls DiggingGameGuy_AttemptPrizeSpawn. word_7E04B2 set =
 * "you dug up the flute spot" -> spawn the dug-up flute ancilla.
 */
void LinkItem_Shovel() {  // 87a32c
  static const uint8 kShovelAnimDelay[] = { 7, 18, 16, 7, 18, 16 };
  static const uint8 kShovelAnimDelay2[] = { 0, 1, 2, 0, 1, 2 };
  if (!(button_mask_b_y & 0x40)) {
    if (is_standing_in_doorway || !CheckYButtonPress())
      return;

    link_delay_timer_spin_attack = kShovelAnimDelay[0];
    link_var30d = 0;
    player_handler_timer = 0;
    link_position_mode = 1;
    link_cant_change_direction |= 1;
    link_animation_steps = 0;
  }
  HaltLinkWhenUsingItems();
  link_direction &= ~0xf;
  if (!sign8(--link_delay_timer_spin_attack))
    return;
  link_var30d++;
  link_delay_timer_spin_attack = kShovelAnimDelay[link_var30d];
  player_handler_timer = kShovelAnimDelay2[link_var30d];

  if (player_handler_timer == 1) {
    TileDetect_MainHandler(2);
    if (BYTE(word_7E04B2)) {
      Ancilla_Sfx3_Near(27);
      AncillaAdd_DugUpFlute(54, 0);
    }

    if (!((tiledetect_thick_grass | tiledetect_destruction_aftermath) & 1)) {
      Ancilla_AddHitStars(22, 0); // hit stars
      Ancilla_Sfx2_Near(5);
    } else {
      AncillaAdd_ShovelDirt(23, 0); // shovel dirt
      if (is_archer_or_shovel_game)
        DiggingGameGuy_AttemptPrizeSpawn();
      Ancilla_Sfx2_Near(18);
    }
  }

  if (link_var30d == 3) {
    link_var30d = 0;
    player_handler_timer = 0;
    button_mask_b_y &= 0x80;
    link_position_mode = 0;
    link_cant_change_direction &= ~1;
  }
}

/*
 * LinkItem_Flute — Play the flute. 128-frame cooldown
 * (flute_countdown). Indoors / dark world / module 11 = no effect
 * (just plays the sound). Outdoors with the upgraded flute (=2)
 * specifically at the weathervane behind Link's house warps to
 * submodule 45 and explodes the vane. Standard flute spawns the
 * duck taking off (Take-Off ancilla). The "27-ancilla type" check
 * blocks re-playing while the bird is descending.
 */
void LinkItem_Flute() {  // 87a3db
  if (button_mask_b_y & 0x40) {
    if (--flute_countdown)
      return;
    button_mask_b_y &= ~0x40;
  }
  if (!CheckYButtonPress())
    return;
  flute_countdown = 128;
  Ancilla_Sfx2_Near(19);
  if (player_is_indoors || overworld_screen_index & 0x40 || main_module_index == 11)
    return;
  int i = 4;
  do {
    if (ancilla_type[i] == 0x27)
      return;
  } while (--i >= 0);
  if (link_item_flute == 2) {
    if (overworld_screen_index == 0x18 && link_y_coord >= 0x760 && link_y_coord < 0x7e0 && link_x_coord >= 0x1cf && link_x_coord < 0x230) {
      submodule_index = 45;
      AncillaAdd_ExplodingWeatherVane(55, 0);
    }
  } else {
    AncillaAdd_Duck_take_off(39, 4);
    link_need_for_pullforrupees_sprite = 0;
  }
}

/*
 * LinkItem_Book — Read the Book of Mudora. Only useful at certain
 * tablets / monoliths (byte_7E02ED set by tile-detect when standing
 * next to one). Otherwise plays the error beep.
 */
void LinkItem_Book() {  // 87a471
  if (button_mask_b_y & 0x40 || is_standing_in_doorway || !CheckYButtonPress())
    return;
  button_mask_b_y &= ~0x40;
  if (byte_7E02ED) {
    Link_PerformDesertPrayer();
  } else {
    Ancilla_Sfx2_Near(60);
  }
}

/*
 * LinkItem_Ether — Cast the Ether medallion. Requires Master Sword
 * or higher (the link_sword_type bit test), no in-flight medallion,
 * not in a savegame-game-over state. Costs magic. On success
 * transitions to slot 8 (UsingEther) which animates the cast.
 */
void LinkItem_Ether() {  // 87a494
  if (!CheckYButtonPress())
    return;
  button_mask_b_y &= ~0x40;

  if (is_standing_in_doorway || flag_block_link_menu || dung_savegame_state_bits & 0x8000 || !((uint8)(link_sword_type + 1) & ~1) ||
      follower_dropped && follower_indicator == 13) {
    Ancilla_Sfx2_Near(60);
    return;
  }

  if (ancilla_type[0] | ancilla_type[1] | ancilla_type[2])
    return;

  if (!LinkCheckMagicCost(1))
    return;
  link_player_handler_state = kPlayerState_Ether;
  link_cant_change_direction |= 1;
  link_delay_timer_spin_attack = kEtherAnimDelays[0];
  state_for_spin_attack = 0;
  step_counter_for_spin_attack = 0;
  byte_7E0324 = 0;
  Ancilla_Sfx3_Near(35);
}

/*
 * LinkState_UsingEther — Slot 8: Ether-cast animation. 12-frame
 * sequence driven by step_counter_for_spin_attack via the
 * kEtherAnimDelays / kEtherAnimStates pair (and the no-flash variant
 * for the dim-flashes accessibility option). On step 4 plays the
 * cast-charge SFX (35), step 9 the impact (44), step 10 spawns the
 * actual EtherSpell ancilla and clears the aux state. step 12
 * loops back to 10 for the post-cast "fade" frames.
 */
void LinkState_UsingEther() {  // 87a50f
  flag_unk1++;
  if (!sign8(--link_delay_timer_spin_attack))
    return;

  step_counter_for_spin_attack++;
  if (step_counter_for_spin_attack == 4) {
    Ancilla_Sfx3_Near(35);
  } else if (step_counter_for_spin_attack == 9) {
    Ancilla_Sfx2_Near(44);
  } else if (step_counter_for_spin_attack == 12) {
    step_counter_for_spin_attack = 10;
  }
  const uint8 *table = (enhanced_features0 & kFeatures0_DimFlashes) ? kEtherAnimDelaysNoFlash : kEtherAnimDelays;
  link_delay_timer_spin_attack = table[step_counter_for_spin_attack];
  state_for_spin_attack = kEtherAnimStates[step_counter_for_spin_attack];
  if (!byte_7E0324 && step_counter_for_spin_attack == 10) {
    byte_7E0324 = 1;
    AncillaAdd_EtherSpell(24, 0);
    link_auxiliary_state = 0;
    link_incapacitated_timer = 0;
  }
}

/*
 * LinkItem_Bombos — Cast the Bombos medallion. Same gate set as
 * LinkItem_Ether (sword tier check, no in-flight medallion, no
 * dwarf follower). Transitions to slot 9 (UsingBombos).
 */
void LinkItem_Bombos() {  // 87a569
  if (!CheckYButtonPress())
    return;
  button_mask_b_y &= ~0x40;

  if (is_standing_in_doorway || flag_block_link_menu || dung_savegame_state_bits & 0x8000 || !((uint8)(link_sword_type + 1) & ~1) ||
      follower_dropped && follower_indicator == 13) {
    Ancilla_Sfx2_Near(60);
    return;
  }

  if (ancilla_type[0] | ancilla_type[1] | ancilla_type[2])
    return;

  if (!LinkCheckMagicCost(1))
    return;
  link_player_handler_state = kPlayerState_Bombos;
  link_cant_change_direction |= 1;
  link_delay_timer_spin_attack = kBombosAnimDelays[0];
  state_for_spin_attack = kBombosAnimStates[0];
  step_counter_for_spin_attack = 0;
  byte_7E0324 = 0;
  Ancilla_Sfx3_Near(35);
}

/*
 * LinkState_UsingBombos — Slot 9: 20-frame Bombos cast animation.
 * Same shape as Ether but longer (more emphatic stomp); spawns the
 * BombosSpell ancilla on step 19.
 */
void LinkState_UsingBombos() {  // 87a5f7
  flag_unk1++;
  if (!sign8(--link_delay_timer_spin_attack))
    return;

  step_counter_for_spin_attack++;
  if (step_counter_for_spin_attack == 4) {
    Ancilla_Sfx3_Near(35);
  } else if (step_counter_for_spin_attack == 10) {
    Ancilla_Sfx2_Near(44);
  } else if (step_counter_for_spin_attack == 20) {
    step_counter_for_spin_attack = 19;
  }
  link_delay_timer_spin_attack = kBombosAnimDelays[step_counter_for_spin_attack];
  state_for_spin_attack = kBombosAnimStates[step_counter_for_spin_attack];
  if (!byte_7E0324 && step_counter_for_spin_attack == 19) {
    byte_7E0324 = 1;
    AncillaAdd_BombosSpell(25, 0);
    link_auxiliary_state = 0;
    link_incapacitated_timer = 0;
  }
}

/*
 * LinkItem_Quake — Cast the Quake medallion. Includes the small
 * jump-into-the-air arc: seeds Z velocity at 40 so Link springs up
 * before pounding down. Transitions to slot 10 (UsingQuake).
 */
void LinkItem_Quake() {  // 87a64b
  if (!CheckYButtonPress())
    return;
  button_mask_b_y &= ~0x40;

  if (is_standing_in_doorway || flag_block_link_menu || dung_savegame_state_bits & 0x8000 || !((uint8)(link_sword_type + 1) & ~1) ||
      follower_dropped && follower_indicator == 13) {
    Ancilla_Sfx2_Near(60);
    return;
  }

  if (ancilla_type[0] | ancilla_type[1] | ancilla_type[2])
    return;

  if (!LinkCheckMagicCost(1))
    return;
  link_player_handler_state = kPlayerState_Quake;
  link_cant_change_direction |= 1;
  link_delay_timer_spin_attack = kQuakeAnimDelays[0];
  state_for_spin_attack = kQuakeAnimStates[0];
  step_counter_for_spin_attack = 0;
  byte_7E0324 = 0;
  link_actual_vel_z_mirror = 40;
  link_actual_vel_z_copy_mirror = 40;
  BYTE(link_z_coord_mirror) = 0;
  Ancilla_Sfx3_Near(35);
}

/*
 * LinkState_UsingQuake — Slot 10: Quake cast. Step 10 is the airborne
 * frame: applies Z-velocity gravity, runs Link's collision; the pose
 * (state_for_spin_attack 20 = falling, 21 = rising) is keyed off
 * the velocity sign. Step 11 lands and spawns the QuakeSpell ancilla.
 */
void LinkState_UsingQuake() {  // 87a6d6
  flag_unk1++;
  link_actual_vel_x = link_actual_vel_y = 0;

  if (step_counter_for_spin_attack == 10) {
    link_actual_vel_z = link_actual_vel_z_mirror;
    link_actual_vel_z_copy = link_actual_vel_z_copy_mirror;
    BYTE(link_z_coord) = link_z_coord_mirror;
    link_auxiliary_state = 2;
    Player_ChangeZ(2);
    Link_MovePosition();
    link_actual_vel_z_mirror = link_actual_vel_z;
    link_actual_vel_z_copy_mirror = link_actual_vel_z_copy;
    BYTE(link_z_coord_mirror) = link_z_coord;
    if (!sign8(link_z_coord)) {
      state_for_spin_attack = sign8(link_actual_vel_z) ? 21 : 20;
      return;
    }
  } else {
    if (!sign8(--link_delay_timer_spin_attack))
      return;
  }

  step_counter_for_spin_attack++;
  if (step_counter_for_spin_attack == 4) {
    Ancilla_Sfx3_Near(35);
  } else if (step_counter_for_spin_attack == 10) {
    Ancilla_Sfx2_Near(44);
  } else if (step_counter_for_spin_attack == 11) {
    Ancilla_Sfx2_Near(12);
  } else if (step_counter_for_spin_attack == 12) {
    step_counter_for_spin_attack = 11;
  }
  link_delay_timer_spin_attack = kQuakeAnimDelays[step_counter_for_spin_attack];
  state_for_spin_attack = kQuakeAnimStates[step_counter_for_spin_attack];
  if (!byte_7E0324 && step_counter_for_spin_attack == 11) {
    byte_7E0324 = 1;
    AncillaAdd_QuakeSpell(28, 0);
    link_auxiliary_state = 0;
    link_incapacitated_timer = 0;
  }
}

/*
 * Link_ActivateSpinAttack — Spawn the spin-attack initial flash
 * ancilla, then run the rotation animation.
 */
void Link_ActivateSpinAttack() {  // 87a77a
  AncillaAdd_SpinAttackInitSpark(42, 0, 0);
  Link_AnimateVictorySpin();
}

/*
 * Link_AnimateVictorySpin — Set up the spin-attack pose state and
 * jump into LinkState_SpinAttack for the first frame. Computes the
 * 12-frame-per-direction offset into kLinkSpinGraphicsByDir.
 * button_b_frames = 144 sets the post-spin movement-lock window.
 */
void Link_AnimateVictorySpin() {  // 87a783
  link_player_handler_state = 3;
  link_spin_offsets = (link_direction_facing >> 1) * 12;
  link_delay_timer_spin_attack = 3;
  state_for_spin_attack = kLinkSpinGraphicsByDir[link_spin_offsets];
  step_counter_for_spin_attack = 0;
  button_b_frames = 144;
  link_cant_change_direction |= 1;
  button_mask_b_y = 0x80;
  LinkState_SpinAttack();
}

/*
 * LinkState_SpinAttack — Slot 3 / 30: spin-attack execution. On
 * aux-state hit (got hurt mid-spin), kill any spin-related ancillas
 * (0x2a/0x2b), reset state, and route into Recoil or Zapped.
 * Otherwise advance the rotation animation, do collision, and stop
 * after the 12-step cycle.
 */
void LinkState_SpinAttack() {  // 87a804
  CacheCameraPropertiesIfOutdoors();

  if (link_auxiliary_state) {
    int i = 4;
    do {
      if (ancilla_type[i] == 0x2a || ancilla_type[i] == 0x2b)
        ancilla_type[i] = 0;
    } while (--i >= 0);
    link_z_coord &= 0xff;
    link_cant_change_direction &= ~1;
    link_delay_timer_spin_attack = 0;
    button_b_frames = 0;
    button_mask_b_y = 0;
    bitfield_for_a_button = 0;
    state_for_spin_attack = 0;
    step_counter_for_spin_attack = 0;
    link_speed_setting = 0;
    if (link_electrocute_on_touch) {
      if (link_cape_mode)
        Link_ForceUnequipCape_quietly();
      Link_ResetSwordAndItemUsage();
      link_disable_sprite_damage = 1;
      player_handler_timer = 0;
      link_delay_timer_spin_attack = 2;
      link_animation_steps = 0;
      link_direction &= ~0xf;
      Ancilla_Sfx3_Near(43);
      link_player_handler_state = kPlayerState_Electrocution;
      LinkState_Zapped();
    } else {
      link_player_handler_state = kPlayerState_RecoilWall;
      LinkState_Recoil();
    }
    return;
  }

  if (link_incapacitated_timer) {
    Link_HandleRecoilAndTimer(false);
  } else {
    link_direction = 0;
    Link_HandleVelocity();
    Link_HandleCardinalCollision();
    link_player_handler_state = kPlayerState_SpinAttacking;
    fallhole_var1 = 0;
    HandleIndoorCameraAndDoors();
  }

  if (!sign8(--link_delay_timer_spin_attack))
    return;

  step_counter_for_spin_attack++;

  if (step_counter_for_spin_attack == 2)
    Ancilla_Sfx3_Near(35);

  if (step_counter_for_spin_attack == 12) {
    link_cant_change_direction &= ~1;
    link_delay_timer_spin_attack = 0;
    button_b_frames = 0;
    state_for_spin_attack = 0;
    step_counter_for_spin_attack = 0;
    if (link_player_handler_state != kPlayerState_SpinAttackMotion) {
      button_mask_b_y = (button_b_frames) ? (joypad1H_last & kJoypadH_B) : 0; // wtf, it's zero,
    }
    link_player_handler_state = kPlayerState_Ground;
  } else {
    state_for_spin_attack = kLinkSpinGraphicsByDir[step_counter_for_spin_attack + link_spin_offsets];
    link_delay_timer_spin_attack = kLinkSpinDelays[step_counter_for_spin_attack];
    TileDetect_MainHandler(8);
  }
}

/*
 * LinkItem_Mirror — Use the Magic Mirror. Light World only (or
 * indoors / via the dark-world cheat / via the
 * MirrorToDarkworld enhancement). Refused while traveling with the
 * frog (follower 10) — shows dialogue 289.
 */
void LinkItem_Mirror() {  // 87a91a
  if (!(button_mask_b_y & 0x40)) {
    if (!CheckYButtonPress())
      return;

    if (follower_indicator == 10) {
      dialogue_message_index = 289;
      Main_ShowTextMessage();
      return;
    }
  }
  button_mask_b_y &= ~0x40;

  if (is_standing_in_doorway || 
      !cheatWalkThroughWalls && !(enhanced_features0 & kFeatures0_MirrorToDarkworld) && 
      !player_is_indoors && !(overworld_screen_index & 0x40)) {
    Ancilla_Sfx2_Near(60);
    return;
  }

  DoSwordInteractionWithTiles_Mirror();
}

/*
 * DoSwordInteractionWithTiles_Mirror — Mirror routing. Indoors:
 * save the room data so the engine can return Link to the dungeon
 * entrance, clear two changeable-object slots. Outdoors (not module
 * 11 = bird-flute warp): record current screen color (light/dark),
 * stash position into bird_travel slot 15 if currently in dark
 * world, jump to submodule 35 (mirror animation), enter Mirror
 * player state. Module 11 specifically allows the mirror to abort
 * the bird flight.
 */
void DoSwordInteractionWithTiles_Mirror() {  // 87a95c
  if (player_is_indoors) {
    if (flag_block_link_menu)
      return;
    Mirror_SaveRoomData();
    if (sound_effect_1 != 60) {
      index_of_changable_dungeon_objs[0] = 0;
      index_of_changable_dungeon_objs[1] = 0;
    }
  } else if (main_module_index != 11) {
    last_light_vs_dark_world = overworld_screen_index & 0x40;
    if (last_light_vs_dark_world) {
      bird_travel_y_lo[15] = link_y_coord;
      bird_travel_y_hi[15] = link_y_coord >> 8;
      bird_travel_x_lo[15] = link_x_coord;
      bird_travel_x_hi[15] = link_x_coord >> 8;
    }
    submodule_index = 35;
    link_need_for_pullforrupees_sprite = 0;
    link_triggered_by_whirlpool_sprite = 1;
    subsubmodule_index = 0;
    link_actual_vel_x = link_actual_vel_y = 0;
    link_player_handler_state = kPlayerState_Mirror;
  }
}

/*
 * LinkState_CrossingWorlds — Slot 20: mid-warp between worlds.
 * After the mirror animation, lands Link in the destination world.
 * Cleans up dash/sword state, handles deep-water-into-shallow
 * transitions, and selects the destination handler state (Ground
 * or PermaBunny depending on Moon Pearl + dark-world status).
 *
 * The "do_mirror" goto handles the case where Link bonks into a
 * mirror tile that DOESN'T let him cross (chest in dungeon, etc.).
 */
void LinkState_CrossingWorlds() {  // 87a9b1
  uint8 t;

  Link_ResetProperties_B();
  TileCheckForMirrorBonk();

  if ((overworld_screen_index & 0x40) != last_light_vs_dark_world && ((t = R12 | R14) & 0xc) != 0 && BitSum4(t) >= 2)
    goto do_mirror;

  if (BitSum4(tiledetect_deepwater) >= 2) {
    if (link_item_flippers) {
      link_is_in_deep_water = 1;
      link_some_direction_bits = link_direction_last;
      Link_ResetSwimmingState();
      link_player_handler_state = kPlayerState_Swimming;
      Link_ForceUnequipCape_quietly();
      link_speed_setting = 0;
      return;
    }
    if ((overworld_screen_index & 0x40) != last_light_vs_dark_world) {
do_mirror:
      submodule_index = 44;
      link_need_for_pullforrupees_sprite = 0;
      link_triggered_by_whirlpool_sprite = 1;
      subsubmodule_index = 0;
      link_actual_vel_x = link_actual_vel_y = 0;
      link_player_handler_state = kPlayerState_Mirror;
      return;
    }
    CheckAbilityToSwim();
  }

  if (link_is_in_deep_water) {
    link_is_in_deep_water = 0;
    link_direction_last = link_some_direction_bits;
  }

  link_countdown_for_dash = 0;
  link_is_running = 0;
  link_speed_setting = 0;
  button_mask_b_y = 0;
  button_b_frames = 0;
  link_cant_change_direction = 0;
  swimcoll_var5[0] &= ~0xff;
  link_actual_vel_y = 0;

  if ((overworld_screen_index & 0x40) != last_light_vs_dark_world)
    num_memorized_tiles = 0;

  link_player_handler_state = (link_item_moon_pearl || !(overworld_screen_index & 0x40)) ? kPlayerState_Ground : kPlayerState_PermaBunny;
}

/*
 * Link_PerformDesertPrayer — Trigger the kneel-and-pray cutscene at
 * the desert / mountain monolith. Switches to module 14 (cutscene)
 * submode 5, locks Link in the praying pose, queues the prayer SFX
 * and music fade.
 */
void Link_PerformDesertPrayer() {  // 87aa6c
  submodule_index = 5;
  saved_module_for_menu = main_module_index;
  main_module_index = 14;
  flag_unk1 = 1;
  some_animation_timer = 22;
  some_animation_timer_steps = 0;
  link_state_bits = 2;
  link_cant_change_direction |= 1;
  link_animation_steps = 0;
  link_direction &= ~0xf;
  sound_effect_ambient = 17;
  music_control = 242;
}

/*
 * HandleFollowersAfterMirroring — Apply mirror-warp side effects
 * to the active follower:
 *   12 / 13: super-bomb / dwarf — drop the carrier flag.
 *   9 / 10:  frog / kid — follower disappears (can't cross).
 *   7 / 8:   dwarven smiths — flip 7<->8 (the "shopkeep <-> partner"
 *            swap), reload graphics, drop a poof garnish.
 * Also handles bunny mode: if no Moon Pearl, drop a bunny-poof
 * garnish; if cape was up, force-unequip it.
 */
void HandleFollowersAfterMirroring() {  // 87aaa2
  TileDetect_MainHandler(0);
  link_animation_steps = 0;
  if (follower_indicator == 12 || follower_indicator == 13) {
    if (follower_indicator == 13) {
      super_bomb_indicator_unk2 = 0xfe;
      super_bomb_indicator_unk1 = 0;
    }
    if (follower_dropped) {
      follower_dropped = 0;
      follower_indicator = 0;
    }
  } else if (follower_indicator == 9 || follower_indicator == 10) {
    follower_indicator = 0;
  } else if (follower_indicator == 7 || follower_indicator == 8) {
    follower_indicator ^= (7 ^ 8);
    LoadFollowerGraphics();
    AncillaAdd_DwarfPoof(0x40, 4);
  }

  if (!link_item_moon_pearl) {
    AncillaAdd_BunnyPoof(0x23, 4);
    Link_ForceUnequipCape_quietly();
    link_bunny_transform_timer = 0;
  } else if (link_cape_mode) {
    Link_ForceUnequipCape();
    link_bunny_transform_timer = 0;
  }
}

/*
 * LinkItem_Hookshot — Fire the hookshot. Suppressed in doorways or
 * when already pulling something (bitmask_of_dragstate bit 2).
 * Sets link_position_mode = 4 (hookshot), link_player_handler_state
 * to 19 (Hookshot), spawns the Hookshot ancilla which does the
 * actual pull/retract animation.
 */
void LinkItem_Hookshot() {  // 87ab25
  if (button_mask_b_y & 0x40 || is_standing_in_doorway || bitmask_of_dragstate & 2 || !CheckYButtonPress())
    return;

  ResetAllAcceleration();
  player_handler_timer = 0;
  link_cant_change_direction |= 1;
  link_delay_timer_spin_attack = 7;
  link_animation_steps = 0;
  link_direction &= ~0xf;
  link_position_mode = 4;
  link_player_handler_state = kPlayerState_Hookshot;
  link_disable_sprite_damage = 1;
  AncillaAdd_Hookshot(31, 3);
}

/*
 * LinkState_Hookshotting — Slot 19: hookshot in flight or pulling.
 * 4 directional offset tables for chain rendering and Link's pull
 * vector: ArrA = chain Y offset, ArrB = chain X offset,
 * ArrC = pull velocity Y, ArrD = pull velocity X. Drives the
 * extend-and-retract phase via player_handler_timer ticking through
 * link_delay_timer_spin_attack.
 */
void LinkState_Hookshotting() {  // 87ab7c
  static const int8 kHookshotArrA[4] = { -8, -16, 0, 0 };
  static const int8 kHookshotArrB[4] = { 0, 0, 4, -12 };
  static const int8 kHookshotArrC[4] = { -64, 64, 0, 0 };
  static const int8 kHookshotArrD[4] = { 0, 0, -64, 64 };

  link_give_damage = 0;
  link_auxiliary_state = 0;
  link_incapacitated_timer = 0;
  int i = 4;
  while (ancilla_type[i] != 0x1f) {
    if (--i < 0) {
      if (!sign8(--link_delay_timer_spin_attack))
        return;
      player_handler_timer = 0;
      link_disable_sprite_damage = 0;
      button_mask_b_y &= ~0x40;
      link_cant_change_direction &= ~1;
      link_position_mode &= ~4;
      link_player_handler_state = kPlayerState_Ground;
      if (button_b_frames >= 9)
        button_b_frames = 9;
      return;
    }
  }

  if (sign8(--link_delay_timer_spin_attack))
    link_delay_timer_spin_attack = 0;

  if (!related_to_hookshot) {
    link_y_coord_safe_return_lo = link_y_coord;
    link_x_coord_safe_return_lo = link_x_coord;
    link_y_vel = link_x_vel = 0;
    Link_HandleCardinalCollision();
    return;
  }

  player_on_somaria_platform = 0;

  uint8 hei = hookshot_effect_index;
  if (sign8(--ancilla_item_to_link[hei])) {
    ancilla_item_to_link[hei] = 0;
  } else {
    uint16 x = ancilla_x_lo[hei] | (ancilla_x_hi[hei] << 8);
    uint16 y = ancilla_y_lo[hei] | (ancilla_y_hi[hei] << 8);
    int8 r4 = kHookshotArrA[ancilla_dir[hei]];
    int8 r6 = kHookshotArrB[ancilla_dir[hei]];
    link_actual_vel_x = link_actual_vel_y = 0;
    int8 r8 = kHookshotArrC[ancilla_dir[hei]];
    int8 r10 = kHookshotArrD[ancilla_dir[hei]];

    uint16 yd = (int16)(y + r4 - link_y_coord);
    if ((int16)yd < 0)
      yd = -yd;
    if (yd >= 2)
      link_actual_vel_y = r8;

    uint16 xd = (int16)(x + r6 - link_x_coord);
    if ((int16)xd < 0)
      xd = -xd;
    if (xd >= 2)
      link_actual_vel_x = r10;

    if (link_actual_vel_x | link_actual_vel_y)
      goto loc_87AD49;
  }

  ancilla_type[hei] = 0;
  tagalong_var7 = tagalong_var1;
  link_player_handler_state = kPlayerState_Ground;
  player_handler_timer = 0;
  link_delay_timer_spin_attack = 0;
  related_to_hookshot = 0;
  button_mask_b_y &= ~0x40;
  link_cant_change_direction &= ~1;
  link_position_mode &= ~4;
  link_disable_sprite_damage = 0;

  if (ancilla_arr1[hei]) {
    link_is_on_lower_level_mirror ^= 1;
    dung_cur_floor--;
    if (kind_of_in_room_staircase == 0) {
      BYTE(dungeon_room_index2) = dungeon_room_index;
      BYTE(dungeon_room_index) += 0x10;
    }
    if (kind_of_in_room_staircase != 2) {
      link_is_on_lower_level ^= 1;
    }
    Dungeon_FlagRoomData_Quadrants();
  }
  Player_TileDetectNearby();
  if (tiledetect_deepwater & 0xf && !link_is_in_deep_water) {
    link_is_in_deep_water = 1;
    link_some_direction_bits = link_direction_last;
    Link_ResetSwimmingState();
    AncillaAdd_Splash(21, 0);
    link_player_handler_state = kPlayerState_Swimming;
    Link_ForceUnequipCape_quietly();
    link_state_bits = 0;
    link_picking_throw_state = 0;
    link_grabbing_wall = 0;
    link_speed_setting = 0;
    if (player_is_indoors)
      link_is_on_lower_level = 1;
    if (button_b_frames >= 9)
      button_b_frames = 9;
  } else if (tiledetect_pit_tile & 0xf) {
    byte_7E005C = 9;
    link_this_controls_sprite_oam = 0;
    player_near_pit_state = 1;
    link_player_handler_state = kPlayerState_FallingIntoHole;
    if (button_b_frames >= 9)
      button_b_frames = 9;
  } else {
    link_y_coord_safe_return_lo = link_y_coord;
    link_y_coord_safe_return_hi = link_y_coord >> 8;
    link_x_coord_safe_return_lo = link_x_coord;
    link_x_coord_safe_return_hi = link_x_coord >> 8;
    Link_HandleCardinalCollision();
    HandleIndoorCameraAndDoors();
  }
  return;
loc_87AD49:
  Link_MovePosition();
  TileDetect_MainHandler(5);
  if (player_is_indoors) {
    uint8 x = tiledetect_vertical_ledge >> 4 | tiledetect_vertical_ledge | detection_of_ledge_tiles_horiz_uphoriz;
    if (x & 1 && sign8(--hookshot_var1)) {
      hookshot_var1 = 3;
      related_to_hookshot ^= 2;
    }
  }
  draw_water_ripples_or_grass = 0;
  if (!(related_to_hookshot & 2)) {
    if (tiledetect_thick_grass & 1) {
      draw_water_ripples_or_grass = 2;
      if (!Link_PermissionForSloshSounds())
        Ancilla_Sfx2_Near(26);
    } else if ((tiledetect_shallow_water | tiledetect_deepwater) & 1) {
      draw_water_ripples_or_grass++;
      Ancilla_Sfx2_Near((uint8)overworld_screen_index == 0x70 ? 27 : 28);
    }
  }

  HandleIndoorCameraAndDoors();
}

/*
 * LinkItem_Cape — Toggle the Magic Cape on/off and tick its magic
 * drain. With cape OFF, a Y-press equips it (gates: not in doorway,
 * have magic). 20-frame transform window with the cape-poof
 * ancilla. With cape ON: drain magic at the rate from
 * kCapeDepletionTimers[link_magic_consumption]; when meter hits 0,
 * force-unequip. A second Y-press also unequips (after the
 * 20-frame debounce).
 */
void LinkItem_Cape() {  // 87adc1
  if (!link_cape_mode) {
    if (!sign8(--link_bunny_transform_timer)) {
      link_direction &= ~0xf;
      HaltLinkWhenUsingItems();
      return;
    }
    link_bunny_transform_timer = 0;
    if (is_standing_in_doorway || !CheckYButtonPress())
      return;
    button_mask_b_y &= ~0x40;
    if (!link_magic_power) {
      Ancilla_Sfx2_Near(60);
      dialogue_message_index = 123;
      Main_ShowTextMessage();
      return;
    }
    player_handler_timer = 0;
    link_cape_mode = 1;
    cape_decrement_counter = kCapeDepletionTimers[link_magic_consumption];
    link_bunny_transform_timer = 20;
    AncillaAdd_CapePoof(35, 4);
    Ancilla_Sfx2_Near(20);
  } else {
    link_disable_sprite_damage = 1;
    HaltLinkWhenUsingItems();
    link_direction &= ~0xf;
    if (!--cape_decrement_counter) {
      cape_decrement_counter = kCapeDepletionTimers[link_magic_consumption];
      // Avoid magic underflow if an anti-fairy consumes magic.
      if (link_magic_power == 0 && (enhanced_features0 & kFeatures0_MiscBugFixes) ||
          !--link_magic_power) {
        Link_ForceUnequipCape();
        return;
      }
    }
    if (sign8(--link_bunny_transform_timer)) {
      link_bunny_transform_timer = 0;
      if (filtered_joypad_H & kJoypadH_Y)
        Link_ForceUnequipCape();
    }
  }
}

void Link_ForceUnequipCape() {  // 87ae47
  AncillaAdd_CapePoof(35, 4);
  Ancilla_Sfx2_Near(21);
  Link_ForceUnequipCape_quietly();
}

void Link_ForceUnequipCape_quietly() {  // 87ae54
  link_bunny_transform_timer = 32;
  link_disable_sprite_damage = 0;
  link_cape_mode = 0;
  link_electrocute_on_touch = 0;
}

void HaltLinkWhenUsingItems() {  // 87ae65
  if (dung_hdr_collision_2 == 2 && (byte_7E0322 & 3) == 3) {
    link_y_vel = 0;
    link_x_vel = 0;
    link_direction = 0;
    link_subpixel_y = 0;
    link_subpixel_x = 0;
    link_moving_against_diag_tile = 0;
  }
  if (player_on_somaria_platform)
    link_direction = 0;
}

void Link_HandleCape_passive_LiftCheck() {  // 87ae88
  //bugfix: grabbing or pulling while wearing cape didn't drain magic
  if (link_state_bits & 0x80 || (enhanced_features0 & kFeatures0_MiscBugFixes && link_grabbing_wall))
    Player_CheckHandleCapeStuff();
}

void Player_CheckHandleCapeStuff() {  // 87ae8f
  if (link_cape_mode && current_item_active == 19) {
    if (current_item_active == current_item_y) {
      if (--cape_decrement_counter)
        return;
      cape_decrement_counter = kCapeDepletionTimers[link_magic_consumption];
      if (!link_magic_power || --link_magic_power)
        return;
    }
    Link_ForceUnequipCape();
  }
}

void LinkItem_CaneOfSomaria() {  // 87aec0
  static const uint8 kRodAnimDelays[] = { 3, 3, 5 };
  if (!(button_mask_b_y & 0x40)) {
    if (player_on_somaria_platform || is_standing_in_doorway || !CheckYButtonPress())
      return;
    int i = 4;
    bool did_charge_magic = false;

    while (ancilla_type[i] != 0x2c) {
      if (--i < 0) {
        if (!LinkCheckMagicCost(4)) {
          // If you use the Cane of Somaria with an empty magic meter,
          // then quickly switch to the mushroom or magic powder after
          // the "no magic" prompt, you will automatically sprinkle magic powder
          // despite pressing no button and having no magic.
          if (enhanced_features0 & kFeatures0_MiscBugFixes)
            goto out;
          return;
        }
        did_charge_magic = true;
        break;
      }
    }
    link_debug_value_2 = 1;
    if (AncillaAdd_SomariaBlock(0x2c, 1) < 0) {
      // If you use the Cane of Somaria while two bombs and the boomerang are active,
      // magic will be refunded instead of used.
      if (did_charge_magic || !(enhanced_features0 & kFeatures0_MiscBugFixes))
        Refund_Magic(4);
    }
    link_delay_timer_spin_attack = kRodAnimDelays[0];
    link_animation_steps = 0;
    player_handler_timer = 0;
    link_item_in_hand = 0;
    link_position_mode |= 8;
  }

  HaltLinkWhenUsingItems();
  link_direction &= ~0xf;
  if (!sign8(--link_delay_timer_spin_attack))
    return;
  player_handler_timer++;

  link_delay_timer_spin_attack = kRodAnimDelays[player_handler_timer];
  if (player_handler_timer != 3)
    return;
  link_speed_setting = 0;
  player_handler_timer = 0;
  link_delay_timer_spin_attack = 0;
  link_debug_value_2 = 0;
  link_position_mode &= ~8;
out:
  button_mask_b_y &= ~0x40;
}

// Handles the Cane of Byrna item usage. The Byrna creates a protective spark ring
// around Link that damages enemies and grants invincibility at the cost of ongoing
// magic drain. The animation sequence mirrors the Somaria cane swing but spawns a
// Byrna-specific spark ancilla. Lock direction during the cast animation.
void LinkItem_CaneOfByrna() {  // 87af3e
  static const uint8 kByrnaDelays[] = { 19, 7, 13, 32 };
  // If the Byrna spark ring is already active, skip re-casting
  if (SearchForByrnaSpark())
    return;
  if (!(button_mask_b_y & 0x40)) {
    if (is_standing_in_doorway || !CheckYButtonPress())
      return;
    // Magic cost index 8 corresponds to the Cane of Byrna's initial activation cost
    if (!LinkCheckMagicCost(8))
      goto out;
    // Spawn the initial spark that will orbit Link
    AncillaAdd_CaneOfByrnaInitSpark(48, 0);
    link_spin_attack_step_counter = 0;
    link_delay_timer_spin_attack = kByrnaDelays[0];
    link_var30d = 0;
    player_handler_timer = 0;
    // Bit 3 of position_mode signals cane-usage state to the animation system
    link_position_mode = 8;
    link_cant_change_direction |= 1;
    link_animation_steps = 0;
  }
  HaltLinkWhenUsingItems();
  link_direction &= ~0xf;
  if (!sign8(--link_delay_timer_spin_attack))
    return;
  player_handler_timer++;
  link_delay_timer_spin_attack = kByrnaDelays[player_handler_timer];
  if (player_handler_timer == 1) {
    Ancilla_Sfx3_Near(42);
  } else if (player_handler_timer == 3) {
    // Animation complete or magic cost failed: reset all cane state
out:
    link_var30d = 0;
    player_handler_timer = 0;
    // Preserve sword charge (bit 7) but clear Y-button item usage (bit 6)
    button_mask_b_y &= 0x80;
    link_position_mode = 0;
    link_cant_change_direction &= ~1;
  }
}

// Checks whether a Byrna spark (ancilla type 0x31) is already active.
// Returns true if Link already has the protective spark ring orbiting him.
// Scans ancilla slots 0-4 (the non-priority slots used for persistent effects).
// The position_mode bit 3 check ensures we only look when the cane is not mid-swing.
bool SearchForByrnaSpark() {  // 87afb5
  if (link_position_mode & 8)
    return false;
  int i = 4;
  do {
    if (ancilla_type[i] == 0x31)
      return true;
  } while (--i >= 0);
  return false;
}

// Handles the Bug-Catching Net item. The net swings in an arc in front of Link,
// capable of catching bees, fairies, and the Agahnim energy ball. The timer table
// is organized as 4 groups of 10 entries (one per facing direction), each controlling
// the OAM tile index progression that creates the visual sweep arc.
void LinkItem_Net() {  // 87aff8
  // 4 directions x 10 animation frames; each value is an OAM handler timer index
  static const uint8 kBugNetTimers[] = { 11, 6, 7, 8, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 9, 4, 5, 6, 7, 8, 1, 2, 3, 4, 10, 8, 1, 2, 3, 4, 5, 6, 7, 8 };
  if (!(button_mask_b_y & 0x40)) {
    if (is_standing_in_doorway || !CheckYButtonPress())
      return;

    // Select initial OAM frame from the direction-specific group of 10
    player_handler_timer = kBugNetTimers[(link_direction_facing >> 1) * 10];
    // Each sub-frame lasts 3 ticks before advancing to next net position
    link_delay_timer_spin_attack = 3;
    link_var30d = 0;
    // position_mode 16 signals net-swinging state to the OAM drawing system
    link_position_mode = 16;
    link_cant_change_direction |= 1;
    link_animation_steps = 0;
    // Play the net-swish sound effect
    Ancilla_Sfx2_Near(50);
  }

  HaltLinkWhenUsingItems();
  link_direction &= ~0xf;
  if (!sign8(--link_delay_timer_spin_attack))
    return;

  // Advance to next arc position in the 10-frame sweep sequence
  link_var30d++;
  link_delay_timer_spin_attack = 3;
  player_handler_timer = kBugNetTimers[(link_direction_facing >> 1) * 10 + link_var30d];

  // After all 10 frames, the net swing is complete
  if (link_var30d == 10) {
    link_var30d = 0;
    player_handler_timer = 0;
    button_mask_b_y &= 0x80;
    link_position_mode = 0;
    link_cant_change_direction &= ~1;
    // Reset OAM offsets to center (0x80 = no offset in signed representation)
    player_oam_x_offset = 0x80;
    player_oam_y_offset = 0x80;
  }
}

// Checks if the Y button was freshly pressed this frame, accounting for item
// cooldowns and incapacitation. Returns true and sets the Y-usage flag (bit 6)
// on first valid press; returns false if already in use or player is stunned.
/*
 * CheckYButtonPress — Edge-detect a new Y-button press. Returns
 * false if Y is already latched (0x40), Link is in recoil, or Y is
 * not actually held. On a fresh press, latches the bit and returns
 * true so the caller knows it owns the press.
 */
bool CheckYButtonPress() {  // 87b073
  if (button_mask_b_y & 0x40 || link_incapacitated_timer || !(filtered_joypad_H & kJoypadH_Y))
    return false;
  button_mask_b_y |= 0x40;
  return true;
}

// Attempts to deduct the magic cost for item index |x| from Link's magic meter.
// The cost table has 3 entries per item (one per magic-consumption level: full, half,
// quarter cost based on magic-reducing bottles collected). Returns true if Link had
// enough magic and the cost was successfully deducted. If insufficient magic and not
// the Ether medallion (index 3), shows the "not enough magic" dialogue.
bool LinkCheckMagicCost(uint8 x) {  // 87b0ab
  // Index into the cost table: item_index * 3 + consumption_level (0=full, 1=half, 2=quarter)
  uint8 cost = kLinkItem_MagicCosts[x * 3 + link_magic_consumption];
  uint8 a = link_magic_power;
  // Check that magic is nonzero and subtracting cost doesn't underflow past 0
  if (a && (a -= cost) < 0x80) {
    link_magic_power = a;
    return true;
  }
  // Ether (index 3) fails silently; all other items show the "not enough magic" message
  if (x != 3) {
    Ancilla_Sfx2_Near(60);
    dialogue_message_index = 123;
    Main_ShowTextMessage();
  }
  return false;
}

/*
 * Refund_Magic — Reverse of LinkCheckMagicCost. Re-credit `x`'s
 * magic cost when an item-use failed (e.g., Somaria block couldn't
 * spawn). Existing author note retained.
 */
void Refund_Magic(uint8 x) {  // 87b0e9
  uint8 cost = kLinkItem_MagicCosts[x * 3 + link_magic_consumption];

  int new_magic = link_magic_power + cost;
  // Ensure magic can't overflow (for example the cane of somaria bug)
  if (enhanced_features0 & kFeatures0_MiscBugFixes && new_magic >= 128)
    new_magic = 128;
  link_magic_power = new_magic;
}

/*
 * Link_ItemReset_FromOverworldThings — Light-weight state cleanup
 * called when an overworld interaction (mirror, whirlpool, etc.)
 * wants to drop Link's per-frame item flags but keep the rest of
 * his state intact.
 */
void Link_ItemReset_FromOverworldThings() {  // 87b107
  some_animation_timer_steps = 0;
  bitfield_for_a_button = 0;
  link_state_bits = 0;
  link_picking_throw_state = 0;
  link_grabbing_wall = 0;
  link_cant_change_direction &= ~1;
}

/*
 * Link_PerformThrow — Throw the currently carried object (sprite,
 * ancilla, or lifted tile). If nothing is currently carried, search
 * for a liftable tile under Link's pickup position via
 * Dungeon_LiftAndReplaceLiftable / Overworld_HandleLiftableTiles
 * and spawn the appropriate throwable terrain sprite via
 * Sprite_SpawnThrowableTerrain (kLink_Lift_tab maps the surface
 * attribute to the sprite id). Initialize the throw-pose timer and
 * state bits.
 */
void Link_PerformThrow() {  // 87b11c

  if (!(flag_is_sprite_to_pick_up | flag_is_ancilla_to_pick_up)) {
    Link_ResetSwordAndItemUsage();
    bitfield_for_a_button = 0;
    int i = 15;
    while (sprite_state[i] != 0) {
      if (--i < 0)
        return;
    }

    if (interacting_with_liftable_tile_x1 == 5 || interacting_with_liftable_tile_x1 == 6) {
      player_handler_timer = 1;
    } else {
      Point16U pt;
      uint8 attr = player_is_indoors ? Dungeon_LiftAndReplaceLiftable(&pt) : Overworld_HandleLiftableTiles(&pt);

      i = 8;
      while (kLink_Lift_tab[i] != attr) {
        if (--i < 0)
          return;
      }

      flag_is_sprite_to_pick_up = 1;
      Sprite_SpawnThrowableTerrain(i, pt.x, pt.y);
      filtered_joypad_L &= ~kJoypadL_A;
      player_handler_timer = 0;
    }
  } else {
    player_handler_timer = 0;
  }

  button_mask_b_y = 0;
  some_animation_timer = 6;
  link_picking_throw_state = 1;
  link_state_bits = 0x80;
  some_animation_timer_steps = 0;
  link_speed_setting = 12;
  link_animation_steps = 0;
  link_direction &= 0xf0;
  link_cant_change_direction |= 1;
}

/*
 * Link_APress_LiftCarryThrow — Per-frame driver for the lift /
 * carry / throw state machine. Switches between three phases via
 * link_picking_throw_state bits:
 *   bit 0 (lift up):   freeze movement, drive the pickup animation.
 *   bit 1 (throw):     5-frame release, then transition to drop.
 *   both clear (carry): hold the lifted item.
 *
 * Phase progression uses some_animation_timer for the per-step
 * delay and player_handler_timer for the step index. Step 6 of the
 * big-rock lift sequence actually spawns the throwable terrain
 * sprite via Sprite_SpawnThrowableTerrain (with kind from
 * Dungeon_LiftAndReplaceLiftable / Overworld_HandleLiftableTiles).
 *
 * Author comments retained ("throwing?", "picking up?", "stop
 * animation", "fix OOB read triggered when lifting for too long").
 */
void Link_APress_LiftCarryThrow() {  // 87b1ca
  if (!link_state_bits)
    return;

  // throwing?
  if ((link_picking_throw_state & 2) && some_animation_timer >= 5)
    some_animation_timer = 5;

  // picking up?
  if (link_picking_throw_state)
    HaltLinkWhenUsingItems();

  if (link_picking_throw_state & 1) {
    link_animation_steps = 0;
    link_counter_var1 = 0;
    link_direction &= ~0xf;
  }

  if (--some_animation_timer)
    return;

  if (link_picking_throw_state & 2) {
    link_state_bits = 0;
    bitmask_of_dragstate = 0;
    link_speed_setting = 0;
    if (link_player_handler_state == 24)
      link_player_handler_state = 0;
  } else {
    static const uint8 kLiftTab0[10] = { 8, 24, 8, 24, 8, 32, 6, 8, 13, 13 };
    static const uint8 kLiftTab1[10] = { 0, 1, 0, 1, 0, 1, 0, 1, 2, 3 };
    static const uint8 kLiftTab2[29] = { 6, 7, 7, 5, 10, 0, 23, 0, 18, 0, 18, 0, 8, 0, 8, 0, 254, 255, 17, 0, 
        0x54, 0x52, 0x50, 0xFF, 0x51, 0x53, 0x55, 0x56, 0x57 };

    if (player_handler_timer != 0) {
      if (player_handler_timer + 1 != 9) {
        player_handler_timer++;
        some_animation_timer = kLiftTab0[player_handler_timer];
        some_animation_timer_steps = kLiftTab1[player_handler_timer];
        if (player_handler_timer == 6) {
          BYTE(dung_secrets_unk1) = 0;
          Point16U pt;
          uint8 what = (player_is_indoors) ? Dungeon_LiftAndReplaceLiftable(&pt) : Overworld_HandleLiftableTiles(&pt);
          link_player_handler_state = 24;
          flag_is_sprite_to_pick_up = 1;
          Sprite_SpawnThrowableTerrain((what & 0xf) + 1, pt.x, pt.y);
          filtered_joypad_L &= ~kJoypadL_A;
        }
        return;
      }
    } else {
      // fix OOB read triggered when lifting for too long
      if (some_animation_timer_steps >= sizeof(kLiftTab2) - 1)
        return;
      some_animation_timer = kLiftTab2[++some_animation_timer_steps];
      assert(some_animation_timer_steps < arraysize(kLiftTab2));
      if (some_animation_timer_steps != 3)
        return;
    }
  }

  // stop animation
  link_picking_throw_state = 0;
  link_cant_change_direction &= ~1;
}

/*
 * Link_PerformDash — Begin a Pegasus-boots dash. Refused on somaria
 * platforms, while carrying, or while in mid-throw. Sets dash
 * counters (29-frame wind-up, 64 = peak speed), enters
 * kPlayerState_StartDash, and (for kid-follower) drops the
 * tagalong reacquire window. The "Write to CART!" warning was a
 * debug breadcrumb from the original port.
 */
void Link_PerformDash() {  // 87b281
  if (player_on_somaria_platform)
    return;
  if (flag_is_sprite_to_pick_up | flag_is_ancilla_to_pick_up)
    return;
  if (link_state_bits & 0x80)
    return;
  bitfield_for_a_button = 0;
  link_countdown_for_dash = 29;
  link_dash_ctr = 64;
  link_player_handler_state = kPlayerState_StartDash;
  link_is_running = 1;
  button_mask_b_y &= 0x80;
  link_state_bits = 0;
  link_item_in_hand = 0;
  bitmask_of_dragstate = 0;
  link_moving_against_diag_tile = 0;

  if (follower_indicator == kTagalongArr1[follower_indicator]) {
    printf("Warning: Write to CART!\n");
    link_speed_setting = 0;
    timer_tagalong_reacquire = 64;
  }
}

/*
 * Link_PerformGrab — Begin a wall/object grab. Refused while
 * holding the spin-attack charge. Sets link_grabbing_wall = 1 and
 * locks the facing direction so the grab pose stays put.
 */
void Link_PerformGrab() {  // 87b2ee
  if ((button_mask_b_y & 0x80) && button_b_frames >= 9)
    return;

  link_grabbing_wall = 1;
  link_cant_change_direction |= 1;
  link_animation_steps = 0;
  some_animation_timer_steps = 0;
  some_animation_timer = 0;
  link_var30d = 0;
}

/*
 * Link_APress_PullObject — Per-frame "pull on grabbed object" tick.
 * If the player isn't still holding the d-pad in the away-from-wall
 * direction (kGrabWallDirs[facing]), abort the pull. Otherwise drive
 * the 7-step pull animation via kGrabWall_AnimSteps + AnimTimer.
 */
void Link_APress_PullObject() {  // 87b322
  link_direction &= ~0xf;

  if (!(kGrabWallDirs[link_direction_facing >> 1] & joypad1H_last)) {
    link_var30d = 0;
    goto set;
  } else if (sign8(--some_animation_timer)) {
    link_var30d = (link_var30d + 1 == 7) ? 1 : link_var30d + 1;
set:
    some_animation_timer_steps = kGrabWall_AnimSteps[link_var30d];
    some_animation_timer = kGrabWall_AnimTimer[link_var30d];
  }

  if (!(joypad1L_last & kJoypadL_A)) {
    link_var30d = 0;
    some_animation_timer_steps = 0;
    link_grabbing_wall = 0;
    bitfield_for_a_button = 0;
    link_cant_change_direction &= ~1;
  }
}

/*
 * Link_PerformStatueDrag — Start a statue drag (Sanctuary mausoleum
 * statue, etc.). Sets link_grabbing_wall = 2 which signals "drag-
 * mode" to the movement integrator. Animation seeded from the first
 * kGrabWall_AnimTimer slot.
 */
void Link_PerformStatueDrag() {  // 87b371
  link_grabbing_wall = 2;
  link_cant_change_direction |= 1;
  link_animation_steps = 0;
  some_animation_timer_steps = 0;
  some_animation_timer = kGrabWall_AnimTimer[0];
  link_var30d = 0;
}

/*
 * Link_APress_StatueDrag — Per-frame statue-drag tick. While A is
 * held and the player is pulling away from the wall direction,
 * advance link_var30d (the animation step) on the
 * kGrabWall_AnimSteps/Timer schedule and apply speed 20 (about half
 * walk speed). Releasing A cancels the drag.
 */
void Link_APress_StatueDrag() {  // 87b389
  link_speed_setting = 20;
  int j;
  if (!(j = joypad1H_last & kGrabWallDirs[link_direction_facing >> 1])) {
    link_direction = 0;
    link_x_vel = link_y_vel = 0;
    link_animation_steps = 0;
    link_var30d = 0;
  } else {
    link_direction = j;
    if (!sign8(--some_animation_timer))
      goto skip_set;
    link_var30d = (link_var30d + 1 == 7) ? 1 : link_var30d + 1;
  }
  some_animation_timer_steps = kGrabWall_AnimSteps[link_var30d];
  some_animation_timer = kGrabWall_AnimTimer[link_var30d];
skip_set:
  if (!(joypad1L_last & kJoypadL_A)) {
    link_speed_setting = 0;
    link_is_near_moveable_statue = 0;
    link_var30d = 0;
    some_animation_timer_steps = 0;
    link_grabbing_wall = 0;
    bitfield_for_a_button = 0;
    link_cant_change_direction &= ~1;
  }
}

/*
 * Link_PerformRupeePull — Begin a Pull-tree branch interaction
 * (specifically tree-stump that grants rupees / heart). Requires
 * facing north (direction_facing == 0). Transitions to slot 29
 * (TreePull) which runs the multi-step grab/yank sequence.
 */
void Link_PerformRupeePull() {  // 87b3e5
  if (link_direction_facing != 0)
    return;
  Link_ResetProperties_A();
  link_grabbing_wall = 2;
  link_cant_change_direction |= 2;

  link_animation_steps = 0;
  some_animation_timer_steps = 0;
  some_animation_timer = kGrabWall_AnimTimer[0];
  link_var30d = 0;
  link_player_handler_state = kPlayerState_PullForRupees;
  link_actual_vel_y = 0;
  link_actual_vel_x = 0;
  button_mask_b_y = 0;
}

/*
 * LinkState_TreePull — Slot 29: pull-tree interaction. Two phases:
 *   Phase 1 (grab held): Run the 7-step grab animation while
 *     player holds Down + A. Step 7 finishes the grab and transitions
 *     into the post-pull "fly back" frame.
 *   Phase 2 (fly back): Spawn dash dust, animate Link sliding south
 *     for 9 frames (kGrabWall_AnimSteps2), then return to ground.
 *
 * Author note retained ("oob read" on kGrabWall_AnimSteps2).
 */
void LinkState_TreePull() {  // 87b416
  CacheCameraPropertiesIfOutdoors();
  if (link_auxiliary_state) {
    HandleLink_From1D();
    return;
  }

  if (link_grabbing_wall) {
    if (!button_mask_b_y) {
      if (!(joypad1L_last & kJoypadL_A)) {
        link_grabbing_wall = 0;
        link_var30d = 0;
        some_animation_timer = 2;
        some_animation_timer_steps = 0;
        link_cant_change_direction = 0;
        link_player_handler_state = 0;
        LinkState_Default();
        return;
      }
      if (!(joypad1H_last & kJoypadH_Down))
        goto out;
      button_mask_b_y = 4;
      Ancilla_Sfx2_Near(0x22);
    }

    if (!sign8(--some_animation_timer))
      goto out;
    int j = ++link_var30d;
    some_animation_timer_steps = kGrabWall_AnimSteps[j];
    some_animation_timer = kGrabWall_AnimTimer[j];
    if (j != 7)
      goto out;

    link_grabbing_wall = 0;
    link_var30d = 0;
    some_animation_timer = 2;
    some_animation_timer_steps = 0;
    link_state_bits = 1;
    link_picking_throw_state = 0;
  }

  if (bitmask_of_dragstate & 9) {
reset_to_normal:
    link_direction_facing = 0;
    link_state_bits = 0;
    link_cant_change_direction = 0;
    link_player_handler_state = kPlayerState_Ground;
    return;
  }
  if (link_var30d == 9) {
    if (!(filtered_joypad_H & kJoypadH_AnyDir))
      goto out2;
    link_player_handler_state = kPlayerState_Ground;
    LinkState_Default();
    return;
  }
  AncillaAdd_DashDust_charging(0x1e, 0);
  if (sign8(--some_animation_timer)) {
    static const uint8 kGrabWall_AnimSteps2[10] = { 0, 1, 2, 3, 4, 0, 1, 2, 3, 0x20 };  // oob read
    int j = ++link_var30d;
    some_animation_timer_steps = kGrabWall_AnimSteps2[j];
    some_animation_timer = 2;
    link_actual_vel_y = 48;
    if (j == 9)
      goto reset_to_normal;
  }
  Flag67WithDirections();
  if (!(link_direction & 3))
    link_actual_vel_x = 0;
  if (!(link_direction & 0xc))
    link_actual_vel_y = 0;
out:
  Link_MovePosition();
out2:
  Link_HandleCardinalCollision();
  HandleIndoorCameraAndDoors();
}

/*
 * Link_PerformRead — Read a sign or telepathic tile. Indoors:
 * Dungeon_GetTeleMsg looks up the telepathic tile message id;
 * outdoors uses Overworld_GetSignText. Sram_progress_indicator < 2
 * (pre-intro) forces dialogue id 0x3A (the "go to sleep" prompt).
 */
void Link_PerformRead() {  // 87b4f2
  if (player_is_indoors) {
    dialogue_message_index = Dungeon_GetTeleMsg(dungeon_room_index);
  } else {
    dialogue_message_index = (sram_progress_indicator < 2) ? 0x3A : Overworld_GetSignText(overworld_screen_index);
  }
  Main_ShowTextMessage();
  bitfield_for_a_button = 0;
}

/*
 * Link_PerformOpenChest — Open a chest in front of Link. Resolves
 * the item id via OpenChestForItem (which decodes the chest's
 * itemdrop slot). The kReceiveItemAlternates table provides
 * "already-have" replacement ids — e.g., a chest containing the
 * silver-bow upgrade becomes arrows if the upgrade is already
 * owned. Hands off to Link_ReceiveItem to start the receive-item
 * cutscene.
 */
void Link_PerformOpenChest() {  // 87b574
  static const uint8 kReceiveItemAlternates[] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 68, 255, 255, 255, 255, 255, 53, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 70, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };
  if (link_direction_facing || item_receipt_method || link_auxiliary_state)
    return;
  bitfield_for_a_button = 0;
  int chest_position = -1;
  uint8 item = OpenChestForItem(index_of_interacting_tile, &chest_position);
  if (sign8(item)) {
    item_receipt_method = 0;
    return;
  }
  assert(chest_position != -1);
  item_receipt_method = 1;
  uint8 alt = kReceiveItemAlternates[item];
  if (alt != 0xff) {
    uint16 ram_addr = kMemoryLocationToGiveItemTo[item];
    if (g_ram[ram_addr])
      item = alt;
  }

  Link_ReceiveItem(item, chest_position);
}

/*
 * Link_CheckNewAPress — Edge-detect a fresh A-button press. Mirror
 * of CheckYButtonPress for the action button. Latches bit 0x80 of
 * bitfield_for_a_button to debounce.
 */
bool Link_CheckNewAPress() {  // 87b5c0
  if (bitfield_for_a_button & 0x80 || link_incapacitated_timer || !(filtered_joypad_L & kJoypadL_A))
    return false;
  bitfield_for_a_button |= 0x80;
  return true;
}

/*
 * Link_HandleToss — A-button re-press while in pickup state triggers
 * a toss. Clears the per-frame animation locks so PlayerHandler_00
 * can transition into the throw sequence. Returns true to tell the
 * caller to short-circuit the rest of its frame.
 */
bool Link_HandleToss() {  // 87b5d6
  if (!(bitfield_for_a_button & 0x80) || !(filtered_joypad_L & kJoypadL_A) || (link_picking_throw_state & 1))
    return false;
  link_var30d = 0;
  link_var30e = 0;
  some_animation_timer_steps = 0;
  bitfield_for_a_button = 0;
  link_cant_change_direction &= ~1;
  // debug stuff here
  return true;
}

/*
 * Link_HandleDiagonalCollision — Resolve diagonal-tile collision
 * against the current movement direction. For two-layer rooms,
 * does a double pass that incorporates the moving-background vel.
 * Always masks link_direction to 0xf (only the 4 dir bits) and runs
 * Player_LimitDirections_Inner to project out impossible directions.
 */
void Link_HandleDiagonalCollision() {  // 87b64f
  if (CheckIfRoomNeedsDoubleLayerCheck()) {
    Player_LimitDirections_Inner();
    CreateVelocityFromMovingBackground();
  }
  link_direction &= 0xf;
  Player_LimitDirections_Inner();
}

/*
 * Player_LimitDirections_Inner — Project Link's intended direction
 * vector against the per-tile slope/wall constraints. For each axis
 * pressed (Y bits 0xc, X bits 0x3), runs a slope tile-detect and
 * intersects the direction with the kMasks entry that prohibits the
 * blocked axis. The kMasks array encodes "all directions except
 * the blocked one":
 *   [0] = 0x7 = clear bit 3 (no up),
 *   [1] = 0xB = clear bit 2 (no down),
 *   [2] = 0xD = clear bit 1 (no right),
 *   [3] = 0xE = clear bit 0 (no left).
 *
 * link_num_orthogonal_directions counts how many axes survived
 * (0/1/2) — diagonal movement => 2, single-axis => 1.
 */
void Player_LimitDirections_Inner() {  // 87b660
  link_direction_mask_a = 0xf;
  link_direction_mask_b = 0xf;
  link_num_orthogonal_directions = 0;

  static const uint8 kMasks[4] = { 7, 0xB, 0xD, 0xE };

  if (link_direction & 0xC) {
    link_num_orthogonal_directions++;

    link_last_direction_moved_towards = link_direction & 8 ? 0 : 1;
    TileDetect_Movement_VerticalSlopes(link_last_direction_moved_towards);

    if ((R14 & 0x30) && !(tiledetect_var1 & 2) && !(((R14 & 0x30) >> 4) & link_direction) && (link_direction & 3)) {
      link_direction_mask_a = kMasks[(link_direction & 2) ? 2 : 3];
    } else {
      if (dung_hdr_collision == 0) {
        if (link_auxiliary_state != 0 && (R12 & 3))
          goto set_thingy;
      }

      if (R14 & 3) {
        link_moving_against_diag_tile = 0;
        if (link_flag_moving && (bitfield_spike_cactus_tiles & 3) == 0 && (link_direction & 3)) {
          swimcoll_var1[0] = 0;
          swimcoll_var5[0] = 0;
          swimcoll_var7[0] = 0;
          swimcoll_var9[0] = 0;
        }
set_thingy:
        fallhole_var1 = 1;
        link_direction_mask_a = kMasks[link_last_direction_moved_towards];
      }
    }

    if (link_direction & 3) {
      link_num_orthogonal_directions++;

      link_last_direction_moved_towards = link_direction & 2 ? 2 : 3;
      TileDetect_Movement_HorizontalSlopes(link_last_direction_moved_towards);

      if ((R14 & 0x30) && (tiledetect_var1 & 2) && !(((R14 & 0x30) >> 2) & link_direction) && (link_direction & 0xC)) {
        link_direction_mask_b = kMasks[(link_direction & 8) ? 0 : 1];
      } else {
        if (dung_hdr_collision == 0) {
          if (link_auxiliary_state != 0 && (R12 & 3))
            goto set_thingy_b;
        }

        if (R14 & 3) {
          link_moving_against_diag_tile = 0;
          if (link_flag_moving && (bitfield_spike_cactus_tiles & 3) == 0 && (link_direction & 0xC)) {
            swimcoll_var1[1] = 0;
            swimcoll_var5[1] = 0;
            swimcoll_var7[1] = 0;
            swimcoll_var9[1] = 0;
          }
set_thingy_b:
          fallhole_var1 = 1;
          link_direction_mask_b = kMasks[link_last_direction_moved_towards];
        }
      }

      link_direction &= link_direction_mask_a & link_direction_mask_b;
    }
  }

  // ending
  if ((link_direction & 0xf) && (link_moving_against_diag_tile & 0xf))
    link_direction = link_moving_against_diag_tile & 0xf;

  if (link_num_orthogonal_directions == 2) {
    link_num_orthogonal_directions = (link_direction_facing & 4) ? 2 : 1;
  } else {
    link_num_orthogonal_directions = 0;
  }
}

/*
 * Link_HandleCardinalCollision — Resolve cardinal-axis tile collision
 * after Link's velocity has been applied. Two large branches:
 *   1. Dungeon-collision modes 2/3 (slopes/spiked floors): run the
 *      slope-aware collision walker (vertical-first or horizontal-
 *      first depending on the dominant velocity axis).
 *   2. Pit-fall detection: if Link's position straddles a pit tile
 *      mask (0xf) and he isn't already in a state that ignores pits
 *      (Hookshot, medallions, spin-attack), transition to slot 1
 *      (FallingIntoHole).
 *
 * Post-collision: recompute link_y_vel and link_x_vel from the
 * actual position delta, and rewrite link_direction bits to match
 * the resolved velocity (so the rendering picks the right pose for
 * "Link is being pushed by a moving floor").
 */
void Link_HandleCardinalCollision() {  // 87b7c7
  tiledetect_diag_state = 0;
  tiledetect_diagonal_tile = 0;

  if (((link_moving_against_diag_tile & 0x30) != 0 || (Link_HandleDiagonalKickback(), moving_against_diag_deadlocked == 0)) &&
      CheckIfRoomNeedsDoubleLayerCheck()) {

    if (dung_hdr_collision < 2 || dung_hdr_collision == 3)
      goto yx;
    tile_coll_flag = 2;
    Player_TileDetectNearby();
    byte_7E0316 = R14;
    if (byte_7E0316 == 0)
      goto yx;
    link_y_vel += dung_floor_y_vel;
    link_x_vel += dung_floor_x_vel;

    uint8 a;
    a = R14;
    if (a == 12 || a == 3)
      goto yx;
    if (a == 10 || a == 5)
      goto xy;
    if ((a & 0xc) == 0 && (a & 3) == 0)
      goto yx;

    if (link_y_vel)
      goto xy;
    if (!link_x_vel)
      goto yx;

    if (sign8(dung_floor_y_vel)) {
yx:   RunSlopeCollisionChecks_VerticalFirst();
    } else {
xy:   RunSlopeCollisionChecks_HorizontalFirst();
    }
    CreateVelocityFromMovingBackground();
  } // endif_1

  if (dung_hdr_collision == 2) {
    Player_TileDetectNearby();
    if ((R14 | byte_7E0316) == 0xf) {
      if (!countdown_for_blink)
        countdown_for_blink = 58;
      if (link_direction == 0) {
        if (BYTE(dung_floor_y_vel))
          link_y_vel = -link_y_vel;
        if (BYTE(dung_floor_x_vel))
          link_x_vel = -link_x_vel;
      }
    }
    tile_coll_flag = 1;
    RunSlopeCollisionChecks_VerticalFirst();
  } else if (dung_hdr_collision == 3) {
    tile_coll_flag = 1;
    RunSlopeCollisionChecks_HorizontalFirst();
  } else if (dung_hdr_collision == 4 || (link_x_vel | link_y_vel) != 0) {
    tile_coll_flag = 1;
    RunSlopeCollisionChecks_VerticalFirst();
  } else {
    uint8 st = link_player_handler_state;
    if (st != 19 && st != 8 && st != 9 && st != 10 && st != 3) {
      Player_TileDetectNearby();
      if (tiledetect_pit_tile & 0xf) {
        link_player_handler_state = kPlayerState_FallingIntoHole;
        if (!link_is_running)
          link_speed_setting = 4;
      }
    }
  }

  TileDetect_MainHandler(0);
  if (link_num_orthogonal_directions != 0)
    link_moving_against_diag_tile = 0;

  if (link_player_handler_state != 11) {
    link_y_vel = link_y_coord - link_y_coord_safe_return_lo;
    if (link_y_vel)
      link_direction = (link_direction & 3) | (sign8(link_y_vel) ? 8 : 4);
  }

  link_x_vel = link_x_coord - link_x_coord_safe_return_lo;
  if (link_x_vel)
    link_direction = (link_direction & 0xC) | (sign8(link_x_vel) ? 2 : 1);

  if (!player_is_indoors || dung_hdr_collision != 4 || link_player_handler_state != kPlayerState_Swimming)
    return;

  if (dung_floor_y_vel && (uint8)(link_y_vel - dung_floor_y_vel) == 0)
    link_direction &= sign8(dung_floor_y_vel) ? ~8 : ~4;

  if (dung_floor_x_vel && (uint8)(link_x_vel - dung_floor_x_vel) == 0)
    link_direction &= sign8(dung_floor_x_vel) ? ~2 : ~1;
}

/*
 * RunSlopeCollisionChecks_VerticalFirst / HorizontalFirst — Pair of
 * orderings for the per-axis collision pass. The "first axis" runs
 * unconditionally; the other only runs if its corresponding sticky
 * diagonal bit (0x20 for Y, 0x10 for X) isn't set. The order
 * matters when Link is sliding along a slope — the axis the slope
 * resolves to has to be checked first.
 */
void RunSlopeCollisionChecks_VerticalFirst() {  // 87b956
  if (!(link_moving_against_diag_tile & 0x20))
    StartMovementCollisionChecks_Y();
  if (!(link_moving_against_diag_tile & 0x10))
    StartMovementCollisionChecks_X();
}

void RunSlopeCollisionChecks_HorizontalFirst() {  // 87b969
  if (!(link_moving_against_diag_tile & 0x10))
    StartMovementCollisionChecks_X();
  if (!(link_moving_against_diag_tile & 0x20))
    StartMovementCollisionChecks_Y();
}

/*
 * CheckIfRoomNeedsDoubleLayerCheck — Some dungeon rooms (collision
 * modes 2/3) have a second BG plane with its own collision. This
 * helper computes the BG1-vs-BG2 scroll delta into the
 * related_to_moving_floor_* mirror pair, returning true if a second
 * pass is needed. Collision modes 0/4 don't have two layers.
 */
bool CheckIfRoomNeedsDoubleLayerCheck() {  // 87b97c
  if (dung_hdr_collision == 0 || dung_hdr_collision == 4)
    return false;

  if (dung_hdr_collision >= 2) {
    link_y_coord += BG1VOFS_copy2 - BG2VOFS_copy2;
    related_to_moving_floor_y = link_y_coord;
    link_x_coord += BG1HOFS_copy2 - BG2HOFS_copy2;
    related_to_moving_floor_x = link_x_coord;
  }
  link_is_on_lower_level = 1;
  return true;
}

/*
 * CreateVelocityFromMovingBackground — Reverse of the BG offset
 * adjustment in CheckIfRoomNeedsDoubleLayerCheck. The position
 * delta accumulated while we ran the second-layer collision becomes
 * additive velocity (so Link is carried by the moving floor), then
 * restores the coordinates to BG2 frame of reference.
 */
void CreateVelocityFromMovingBackground() {  // 87b9b3
  if (dung_hdr_collision != 1) {
    uint16 x = link_x_coord - related_to_moving_floor_x;
    uint16 y = link_y_coord - related_to_moving_floor_y;
    link_y_coord += BG2VOFS_copy2 - BG1VOFS_copy2;
    link_x_coord += BG2HOFS_copy2 - BG1HOFS_copy2;
    if (link_direction) {
      link_x_vel += x;
      link_y_vel += y;
    }
  }
  link_is_on_lower_level = 0;
}

/*
 * StartMovementCollisionChecks_Y — Y-axis collision driver. Picks
 * the actual move direction (0=up, 1=down) from either the doorway-
 * latched Y position (when standing in a doorway) or the current
 * Y velocity, runs the tile-detect, then routes to the indoor or
 * outdoor handler depending on environment.
 */
void StartMovementCollisionChecks_Y() {  // 87ba0a
  if (!link_y_vel)
    return;

  if (is_standing_in_doorway == 1)
    link_last_direction_moved_towards = (uint8)link_y_coord < 0x80 ? 0 : 1;
  else
    link_last_direction_moved_towards = sign8(link_y_vel) ? 0 : 1;
  TileDetect_Movement_Y(link_last_direction_moved_towards);
  if (player_is_indoors)
    StartMovementCollisionChecks_Y_HandleIndoors();
  else
    StartMovementCollisionChecks_Y_HandleOutdoors();
}

/*
 * StartMovementCollisionChecks_Y_HandleIndoors — Indoor Y-axis tile
 * response. Handles: doorway entry/exit, in-doorway perpendicular
 * movement, "falling" against a wall tile (Y velocity applied but
 * snapped to the wall), conveyor detection, water-staircase
 * triggers, rupee floor tile pickup (kLink_DoMoveXCoord_Indoors_*
 * computes the rupee tile's screen position to clear), ledge-hop
 * triggers (RunLedgeHopTimer), and deep-water entry.
 *
 * The many R14 bit checks are tile-detect result flags:
 *   bit 0/2: blocked in this direction.
 *   bit 4-6: this is a doorway tile.
 */
void StartMovementCollisionChecks_Y_HandleIndoors() {  // 87ba35
  if (sign8(link_state_bits) || link_incapacitated_timer != 0) {
    R14 |= R14 >> 4;
  } else {
    if (is_standing_in_doorway == 2) {
      if (link_num_orthogonal_directions == 0) {
        if (dung_hdr_collision != 3 || link_is_on_lower_level == 0) {
          Link_AddInVelocityY();
          ChangeAxisOfPerpendicularDoorMovement_Y();
          return;
        }
        goto label_3;
      } else if (tiledetect_var1) {
        Link_AddInVelocityY();
        goto endif_1b;
      }
    } // else_3
    if (R14 & 0x70) {
      if ((R14 >> 8) & 7) {
        force_move_any_direction = (sign8(link_y_vel)) ? 8 : 4;
      } // endif_6

      is_standing_in_doorway = 1;
      link_on_conveyor_belt = 0;
      if ((R14 & 0x70) != 0x70) {
        if (R14 & 5) { // if_7
          link_moving_against_diag_tile = 0;
          Link_AddInVelocityYFalling();
          CalculateSnapScratch_Y();
          is_standing_in_doorway = 0;

          if (R14 & 0x20 && (R14 & 1) == 0 && (link_x_coord & 7) == 1)
            link_x_coord &= ~7;
          goto else_7;
        }
        if (R14 & 0x20)
          goto else_7;
      } else { // else_7
else_7:
        if (!(tile_coll_flag & 2))
          link_cant_change_direction &= ~2;
        return;
      }
    }
  } // endif_1

  if (!(tile_coll_flag & 2)) {
    is_standing_in_doorway = 0;
  }

endif_1b:
  if (!(tile_coll_flag & 2)) {
    link_cant_change_direction &= ~2;
    room_transitioning_flags = 0;
    force_move_any_direction = 0;
  } // label_3

label_3:

  if ((R14 & 7) == 0 && (R12 & 5) != 0) {
    link_on_conveyor_belt = 0;
    FlagMovingIntoSlopes_Y();
    if ((link_moving_against_diag_tile & 0xf) != 0)
      return;
  } // endif_9

  link_moving_against_diag_tile = 0;
  if (tiledetect_key_lock_gravestones & 0x20) {
    uint16 bak = R14;
    int dummy;
    OpenChestForItem(tiledetect_tile_type, &dummy);
    tiledetect_tile_type = 0;
    R14 = bak;
  }
  if (!link_is_on_lower_level) {
    if (tiledetect_water_staircase & 7) {
      byte_7E0322 |= 1;
    } else if ((bitfield_spike_cactus_tiles & 7) == 0 && (R14 & 2) == 0) { // else_11
      byte_7E0322 &= ~1;
    } // endif_11
  } else { // else_10
    if ((tiledetect_moving_floor_tiles & 7) != 0) {
      byte_7E0322 |= 2;
    } else {
      byte_7E0322 &= ~2;
    }
  } // endif_11

  if (tiledetect_misc_tiles & 0x2200) {
    uint16 dy = tiledetect_misc_tiles & 0x2000 ? 8 : 0;

    static const uint8 kLink_DoMoveXCoord_Indoors_dx[] = { 8, 8, 0, 15 };
    static const uint8 kLink_DoMoveXCoord_Indoors_dy[] = { 8, 24, 16, 16 };

    link_rupees_goal += 5;
    uint16 y = link_y_coord + kLink_DoMoveXCoord_Indoors_dy[link_last_direction_moved_towards] - dy;
    uint16 x = link_x_coord + kLink_DoMoveXCoord_Indoors_dx[link_last_direction_moved_towards];

    Dungeon_DeleteRupeeTile(x, y);
    Ancilla_Sfx3_Near(10);
  }  // endif_12_norupee

  if (tiledetect_var4 & 0x22) {
    link_on_conveyor_belt = tiledetect_var4 & 0x20 ? 2 : 1;
  } else if (tiledetect_var4 & 0x2200) {
    link_on_conveyor_belt = tiledetect_var4 & 0x2000 ? 4 : 3;
  } else {
    if (!(bitfield_spike_cactus_tiles & 7) && !(R14 & 2))
      link_on_conveyor_belt = 0;
  } // endif_15

  if ((tiledetect_vertical_ledge & 7) == 7 && RunLedgeHopTimer()) {
    Link_CancelDash();
    about_to_jump_off_ledge++;
    link_disable_sprite_damage = 1;
    link_auxiliary_state = 2;
    Ancilla_Sfx2_Near(0x20);

    goto endif_19;
  } else if ((tiledetect_deepwater & 7) == 7 && link_is_in_deep_water == 0) {
    // if_20
    Link_CancelDash();
    if (TS_copy == 0) {
      Dungeon_HandleLayerChange();
    } else {
      link_is_in_deep_water = 1;
      link_some_direction_bits = link_direction_last;
      link_state_bits = 0;
      link_picking_throw_state = 0;
      link_grabbing_wall = 0;
      link_speed_setting = 0;
      Link_ResetSwimmingState();
      Ancilla_Sfx2_Near(0x20);
    }
endif_19:
    link_disable_sprite_damage = 1;
    Link_HopInOrOutOfWater_Y();
  } else {
    // else_20
    if ((tiledetect_normal_tiles & 2) && link_is_in_deep_water != 0) {
      if (link_auxiliary_state != 0) {
        R14 = 7;
      } else {
        Link_CancelDash();
        link_direction_last = link_some_direction_bits;
        link_is_in_deep_water = 0;
        if (AncillaAdd_Splash(0x15, 0)) {
          link_is_in_deep_water = 1;
          R14 = 7;
        } else {
          link_disable_sprite_damage = 1;
          Link_HopInOrOutOfWater_Y();
        }
      }
    }
  } // endif_21

  if ((tiledetect_stair_tile & 7) == 7) {
    if (link_incapacitated_timer) {
      R14 &= ~0xff;
      R14 |= tiledetect_stair_tile & 7;
      HandlePushingBonkingSnaps_Y();
      return;
    }
    if (tiledetect_inroom_staircase & 0x77) {
      submodule_index = tiledetect_inroom_staircase & 0x70 ? 16 : 8;
      main_module_index = 7;
      Link_CancelDash();
    } else if (enhanced_features0 & kFeatures0_TurnWhileDashing) {
      // avoid weirdness in stairs
      Link_CancelDash();
    }

    if ((link_last_direction_moved_towards & 2) == 0) {
      link_speed_setting = 2;
      link_speed_modifier = 1;
      return;
    }
  }

  if (link_speed_setting == 2)
    link_speed_setting = link_is_running ? 16 : 0;

  if (link_speed_modifier == 1)
    link_speed_modifier = 2;

  if (tiledetect_pit_tile & 5 && (R14 & 2) == 0) {
    if (link_player_handler_state == 5 || link_player_handler_state == 2)
      return;
    byte_7E005C = 9;
    link_this_controls_sprite_oam = 0;
    player_near_pit_state = 1;
    link_player_handler_state = kPlayerState_FallingIntoHole;
    return;
  } // endif_23

  link_this_controls_sprite_oam = 0;

  if (bitfield_spike_cactus_tiles & 7) {
    if ((link_incapacitated_timer | countdown_for_blink | link_cape_mode) == 0) {
      if (((link_last_direction_moved_towards == 0) ? (link_y_coord & 4) == 0 : ((link_y_coord & 4) != 0)) && (countdown_for_blink == 0)) {
        link_give_damage = 8;
        Link_CancelDash();
        Link_ForceUnequipCape_quietly();
        LinkApplyTileRebound();
        return;
      }
    } else {
      R14 &= ~0xFF;
      R14 |= bitfield_spike_cactus_tiles & 7;
    } // endif_24
  } // endif_24
  if (dung_hdr_collision == 0 || dung_hdr_collision == 4 || !link_is_on_lower_level) {
    if (tiledetect_var2 && link_num_orthogonal_directions == 0) {
      byte_7E02C2 = tiledetect_var2;
      if (!sign8(--gravestone_push_timeout))
        goto endif_26;
      uint16 bits = tiledetect_var2;
      int i = 15;
      do {
        if (bits & 0x8000) {
          uint8 idx = FindFreeMovingBlockSlot(i);
          if (idx == 0xff)
            continue;
          R14 = idx;
          if (InitializePushBlock(idx, i * 2))
            continue;
          Sprite_Dungeon_DrawSinglePushBlock(idx * 2);
          R14 = 4;  // Unwanted side effect
          pushedblock_facing[idx] = link_last_direction_moved_towards * 2;
          push_block_direction = link_last_direction_moved_towards * 2;
          pushedblocks_target[idx] = (pushedblocks_y_lo[idx] - (link_last_direction_moved_towards == 1)) & 0xf;
        }
      } while (bits <<= 1, --i >= 0);
    }
    // endif_27
    gravestone_push_timeout = 21;
  }
  // endif_26
endif_26:
  HandlePushingBonkingSnaps_Y();
}

/*
 * HandlePushingBonkingSnaps_Y — Y-axis sub-tile resolution: bonk
 * walls, snap to a tile boundary if pushing into one, accumulate
 * the "push tired" timer (which eventually arms dragstate bits so
 * pushable blocks register being pushed).
 *
 * R14 is the tile-detect bitmask; bits 0/2 = blocked, bit 4 = wall.
 * The "nudging" path (Link offset by 1 pixel toward an alignment)
 * smooths walking into the side of a doorway.
 */
void HandlePushingBonkingSnaps_Y() {  // 87bdb1
  if (R14 & 7) {
    if (link_player_handler_state == kPlayerState_Swimming) {
      if ((uint8)dung_floor_y_vel == 0)
        ResetAllAcceleration();

      if (link_num_orthogonal_directions != 0) {
        Link_AddInVelocityYFalling();
        goto label_a;
      }
    }  // endif_2

    if (R14 & 2 || (R14 & 5) == 5) {
      uint16 bak = R14;
      Link_BonkAndSmash();
      RepelDash();
      R14 = bak;
    }

    fallhole_var1 = 1;

    if ((R14 & 2) == 2) {
      Link_AddInVelocityYFalling();
    } else {
      if (link_num_orthogonal_directions == 1)
        goto returnb;
      Link_AddInVelocityYFalling();
      if (link_num_orthogonal_directions == 2)
        goto returnb;
    } // endif_4

label_a:

    if ((R14 & 5) == 5) {
      Link_BonkAndSmash();
      RepelDash();
    } else if (R14 & 4) {
      uint8 tt = sign8(link_y_vel) ? link_y_vel : -link_y_vel;
      uint8 r0 = sign8(tt) ? 0xff : 1;
      if ((R14 & 2) == 0) {
        if (link_x_coord & 7) {
          link_x_coord += (int8)r0;
          HandleNudging(r0);
          return;
        }
        Link_BonkAndSmash();
        RepelDash();
      }
    } else { // else_7
      uint8 tt = sign8(link_y_vel) ? -link_y_vel : link_y_vel;
      uint8 r0 = sign8(tt) ? 0xff : 1;
      if ((R14 & 2) == 0) {
        if (link_x_coord & 7) {
          link_x_coord += (int8)r0;
          HandleNudging(r0);
          return;
        }
        Link_BonkAndSmash();
        RepelDash();
      }
    }
    // endif_10
    if (link_last_direction_moved_towards * 2 == link_direction_facing) {
      bitmask_of_dragstate |= (tile_coll_flag & 1) << 1;
      if (button_b_frames == 0 && !sign8(--link_timer_push_get_tired))
        return;

      bitmask_of_dragstate |= (tiledetect_misc_tiles & 0x20) ? tile_coll_flag << 3 : tile_coll_flag;
    }
  } else {// else_1
    if (link_is_on_lower_level)
      return;
    bitmask_of_dragstate &= ~9;
  } // endif_1

returnb:
  link_timer_push_get_tired = 32;
  bitmask_of_dragstate &= ~2;
}

/*
 * StartMovementCollisionChecks_Y_HandleOutdoors — Outdoor Y-axis
 * tile response. Simpler than indoors:
 *   - Pit fall: transition to FallingIntoHole.
 *   - Deep water entry: with flippers and Moon Pearl/non-bunny,
 *     switch to swimming; otherwise hop-out-of-water.
 *   - Standard wall/slope/ledge resolution via the same Bonk/snap
 *     pipeline as indoors.
 */
void StartMovementCollisionChecks_Y_HandleOutdoors() {  // 87beaf
  if (link_speed_setting == 2)
    link_speed_setting = link_is_running ? 16 : 0;

  if ((tiledetect_pit_tile & 5) != 0 && (R14 & 2) == 0) {
    if (link_player_handler_state != 5 && link_player_handler_state != 2) {
      // start fall into hole
      byte_7E005C = 9;
      link_this_controls_sprite_oam = 0;
      player_near_pit_state = 1;
      link_player_handler_state = kPlayerState_FallingIntoHole;
    }
    return;
  }

  if (tiledetect_read_something & 2) {
    interacting_with_liftable_tile_x1 = interacting_with_liftable_tile_x2 >> 1;
  } else {
    interacting_with_liftable_tile_x1 = 0;
  }  // endif_2

  if ((tiledetect_deepwater & 2) && !link_is_in_deep_water && !link_auxiliary_state) {
    Link_ResetSwordAndItemUsage();
    Link_CancelDash();
    link_is_in_deep_water = 1;
    link_some_direction_bits = link_direction_last;
    link_grabbing_wall = 0;
    link_speed_setting = 0;
    Link_ResetSwimmingState();
    if ((draw_water_ripples_or_grass == 1) && (Link_ForceUnequipCape_quietly(), link_item_flippers != 0)) {
      if (!link_is_bunny_mirror)
        link_player_handler_state = kPlayerState_Swimming;
    } else {
      Ancilla_Sfx2_Near(0x20);
      link_y_coord = (link_y_coord_safe_return_hi << 8) | link_y_coord_safe_return_lo;
      link_x_coord = (link_x_coord_safe_return_hi << 8) | link_x_coord_safe_return_lo;
      link_disable_sprite_damage = 1;
      Link_HopInOrOutOfWater_Y();
    }
  }  // endif_afterSwimCheck

  if (link_is_in_deep_water) {
    if (tiledetect_vertical_ledge & 7) {
      R14 = tiledetect_vertical_ledge & 7;
      HandlePushingBonkingSnaps_Y();
      return;
    }
    if ((tiledetect_stair_tile & 7) == 7 || (tiledetect_normal_tiles & 7) == 7) {
      Link_CancelDash();
      link_is_in_deep_water = 0;
      if (link_auxiliary_state == 0) {
        link_direction_last = link_some_direction_bits;
        link_disable_sprite_damage = 1;
        AncillaAdd_Splash(0x15, 0);
        Link_HopInOrOutOfWater_Y();
        return;
      }
    }
  }

  if (detection_of_ledge_tiles_horiz_uphoriz & 2 || detection_of_unknown_tile_types & 0x22) {
    R14 = 7;
    HandlePushingBonkingSnaps_Y();
    return;
  }

  if (tiledetect_vertical_ledge & 0x70 && RunLedgeHopTimer()) {
    Link_CancelDash();
    link_disable_sprite_damage = 1;
    allow_scroll_z = 1;
    link_player_handler_state = 11;
    link_incapacitated_timer = 0;
    link_z_coord_mirror = -1;
    bitmask_of_dragstate = 0;
    link_speed_setting = 0;
    link_actual_vel_z_copy_mirror = link_actual_vel_z_mirror = link_is_in_deep_water ? 14 : 20;
    link_auxiliary_state = link_is_in_deep_water ? 4 : 2;
    return;
  }

  if (tiledetect_vertical_ledge & 7 && RunLedgeHopTimer()) {
    Ancilla_Sfx2_Near(0x20);
    link_disable_sprite_damage = 1;
    Link_CancelDash();
    bitmask_of_dragstate = 0;
    link_speed_setting = 0;
    Link_FindValidLandingTile_North();
    return;
  }

  if (!link_is_in_deep_water) {
    if (tiledetect_ledges_down_leftright & 7 && !(tiledetect_vertical_ledge & 0x77)) {
      uint8 xand = index_of_interacting_tile == 0x2f ? 4 : 1;
      if ((tiledetect_ledges_down_leftright & xand) && RunLedgeHopTimer()) {
        Link_CancelDash();
        link_actual_vel_x = tiledetect_ledges_down_leftright & 4 ? 16 : -16;
        link_disable_sprite_damage = 1;
        bitmask_of_dragstate = 0;
        link_speed_setting = 0;
        allow_scroll_z = 1;
        link_auxiliary_state = 2;
        link_actual_vel_z_copy_mirror = link_actual_vel_z_mirror = 20;
        link_z_coord_mirror |= 0xff;
        link_incapacitated_timer = 0;
        link_player_handler_state = 14;
        return;
      }
    } // endif_6

    if (detection_of_ledge_tiles_horiz_uphoriz & 0x70 && !(tiledetect_vertical_ledge & 0x77) && RunLedgeHopTimer()) {
      Link_CancelDash();
      Ancilla_Sfx2_Near(0x20);
      link_last_direction_moved_towards = detection_of_ledge_tiles_horiz_uphoriz & 0x40 ? 3 : 2;
      link_disable_sprite_damage = 1;
      bitmask_of_dragstate = 0;
      link_speed_setting = 0;
      Link_FindValidLandingTile_DiagonalNorth();
      return;
    }
  } // endif_7

  if ((tiledetect_stair_tile & 7) == 7) {
    if (link_incapacitated_timer != 0) {
      R14 = tiledetect_stair_tile & 7;
      HandlePushingBonkingSnaps_Y();
      return;
    } else if (!(link_last_direction_moved_towards & 2)) {
      link_speed_setting = 2;
      link_speed_modifier = 1;
      return;
    }
  }  // endif_8

  if (link_speed_setting == 2)
    link_speed_setting = link_is_running ? 16 : 0;

  if (link_speed_modifier == 1)
    link_speed_modifier = 2;

  if ((R14 & 7) == 0 && (R12 & 5) != 0) {
    FlagMovingIntoSlopes_Y();
    if ((link_moving_against_diag_tile & 0xf) != 0)
      return;
  }  // endif_11

  link_moving_against_diag_tile = 0;
  if (tiledetect_key_lock_gravestones & 2 && link_last_direction_moved_towards == 0) {
    if (link_is_running || sign8(--gravestone_push_timeout)) {
      uint16 bak = R14;
      AncillaAdd_GraveStone(0x24, 4);
      R14 = bak;
      gravestone_push_timeout = 52;
    }
  } else {
    gravestone_push_timeout = 52;
  } // endif_12

  if ((bitfield_spike_cactus_tiles & 7) != 0) {
    if ((link_incapacitated_timer | countdown_for_blink | link_cape_mode) == 0) {
      if (link_last_direction_moved_towards == 0 ? ((link_y_coord & 4) == 0) : ((link_y_coord & 4) != 0)) {
        link_give_damage = 8;
        Link_CancelDash();
        Link_ForceUnequipCape_quietly();
        LinkApplyTileRebound();
        return;
      }
    } else {
      R14 = bitfield_spike_cactus_tiles & 7;
    }
  }  // endif_13
  HandlePushingBonkingSnaps_Y();
}

bool RunLedgeHopTimer() { // carry  // 87c16d
  bool rv = false;
  if (link_auxiliary_state != 1) {
    if (!link_is_running) {
      if (sign8(--link_timer_jump_ledge)) {
        link_timer_jump_ledge = 19;
        return true;
      }
    } else {
      rv = true;
    }
  }
  link_y_coord = link_y_coord_prev;
  link_x_coord = link_x_coord_prev;
  link_subpixel_y = link_subpixel_x = 0;
  return rv;
}

/*
 * Link_BonkAndSmash — On a dash-into-rock-pile collision, smash the
 * pile (Overworld_SmashRockPile) and spawn the appropriate broken-
 * terrain sprite. Only fires when actually dashing at peak speed
 * (dash_ctr != 64 — counter that decrements during the dash) AND
 * the tile mask indicates a dashable wall (bits 0x70).
 */
void Link_BonkAndSmash() {  // 87c1a1
  if (!link_is_running || (link_dash_ctr == 64) || !(bitmask_for_dashable_tiles & 0x70))
    return;
  for (int i = 0; i < 2; i++) {
    Point16U pt;
    int j = Overworld_SmashRockPile(i != 0, &pt);
    if (j >= 0) {
      int k = FindInByteArray(kLink_Lift_tab, (uint8)j, 9);
      if (k >= 0) {
        if (k == 2 || k == 4)
          Ancilla_Sfx3_Near(0x32);
        Sprite_SpawnImmediatelySmashedTerrain(k, pt.x, pt.y);
      }
    }
  }
}

/*
 * Link_AddInVelocityYFalling — Snap Link's Y position to the tile-
 * boundary based on his current velocity direction. The
 * `tiledetect_which_y_pos[0] & 7` extracts the sub-tile remainder
 * of the impact point; the `sign8(...) ? 8 : 0` toggle adds a full
 * tile when moving south so Link lines up with the top edge.
 */
void Link_AddInVelocityYFalling() {  // 87c1e4
  link_y_coord -= (tiledetect_which_y_pos[0] & 7) - (sign8(link_y_vel) ? 8 : 0);
}

/*
 * CalculateSnapScratch_Y — Sub-pixel X nudge when entering a doorway
 * from the Y axis. R14 bit 4 toggles which direction to push so
 * Link visually centers in the doorway as he walks through.
 */
// Adjust X coord to fit through door
void CalculateSnapScratch_Y() {  // 87c1ff
  uint8 yv = link_y_vel;
  if (R14 & 4) {
    if (!sign8(yv)) yv = -yv;
  } else {
    if (sign8(yv)) yv = -yv;
  }
  link_x_coord += !sign8(yv) ? 1 : -1;
}

/*
 * ChangeAxisOfPerpendicularDoorMovement_Y — When Link tries to move
 * perpendicular to a doorway he's standing in, gently rotate him to
 * face along the doorway axis and step him toward the door's
 * centerline by 1 pixel. Returns silently if there's no doorway
 * fact pattern in R14.
 */
void ChangeAxisOfPerpendicularDoorMovement_Y() {  // 87c23d
  link_cant_change_direction |= 2;
  uint8 t = (R14 | (R14 >> 4)) & 0xf;
  if (!(t & 7)) {
    is_standing_in_doorway = 0;
    return;
  }
  int8 vel;
  uint8 dir;

  if ((uint8)link_x_coord >= 0x80) {
    uint8 t = link_y_vel;
    if (!sign8(t)) t = -t;
    vel = sign8(t) ? -1 : 1;
    dir = 4;
  } else {
    uint8 t = link_y_vel;
    if (sign8(t)) t = -t;
    vel = sign8(t) ? -1 : 1;
    dir = 6;
  }
  if (!(link_cant_change_direction & 1))
    link_direction_facing = dir;
  link_x_coord += vel;
}

/*
 * Link_AddInVelocityY — Trivial inverse-Y integration: subtract
 * Link's signed Y velocity from his Y coord (the SNES convention
 * has positive Y going down on screen but the original engine uses
 * signed velocity).
 */
void Link_AddInVelocityY() {  // 87c29f
  link_y_coord -= (int8)link_y_vel;
}

/*
 * Link_HopInOrOutOfWater_Y — Bunny-bounce hop animation. Three
 * speed tiers picked by environment: ts 0 indoors with ledge-jump,
 * 1 indoors normal, 2 outdoors. Y velocity points away from the
 * water; Z velocity launches Link upward; 16-frame stun timer
 * holds the hop pose. Drops into state 6 (RecoilOther) which
 * animates the arc.
 */
void Link_HopInOrOutOfWater_Y() {  // 87c2c3
  static const uint8 kRecoilVelY[] = { 24, 16, 16 };
  static const uint8 kRecoilVelZ[] = { 36, 24, 24 };

  uint8 ts = !player_is_indoors ? 2 :
    about_to_jump_off_ledge ? 0 : TS_copy;

  int8 vel = kRecoilVelY[ts];
  if (!link_last_direction_moved_towards)
    vel = -vel;

  link_actual_vel_y = vel;
  link_actual_vel_x = 0;
  link_actual_vel_z_copy = link_actual_vel_z = kRecoilVelZ[ts];
  link_z_coord = 0;
  link_incapacitated_timer = 16;
  if (link_auxiliary_state != 2) {
    link_auxiliary_state = 1;
    link_electrocute_on_touch = 0;
  }
  link_player_handler_state = 6;
}

/*
 * Link_FindValidLandingTile_North — Step Link's coordinate north by
 * 16 px at a time looking for a valid landing tile (normal /
 * destruction-aftermath / thick grass / deepwater). Once found,
 * derive the hop arc parameters from the traveled distance via
 * the kLink_MoveY_RecoilOther_* tables (longer travel = bigger
 * arc). For deep-water landings, also seed the swim state.
 *
 * Used when Link is being knocked into a "soft" northward landing
 * (the 6 state-pair transitions into the bunny / recoil cascade).
 */
void Link_FindValidLandingTile_North() {  // 87c36c
  uint16 y_coord_bak = link_y_coord;
  link_y_coord_original = link_y_coord;

  for (;;) {
    link_y_coord -= 16;
    TileDetect_Movement_Y(link_last_direction_moved_towards);
    uint8 k = tiledetect_normal_tiles | tiledetect_destruction_aftermath | tiledetect_thick_grass | tiledetect_deepwater;
    if ((k & 7) == 7)
      break;
  }

  if (tiledetect_deepwater & 7) {
    link_auxiliary_state = 1;
    link_electrocute_on_touch = 0;
    link_is_in_deep_water = 1;
    link_some_direction_bits = link_direction_last;
    Link_ResetSwimmingState();
    link_grabbing_wall = 0;
    link_speed_setting = 0;
  }

  link_y_coord -= 16;
  link_y_coord_original -= link_y_coord;
  link_y_coord = y_coord_bak;

  uint8 o = (uint8)link_y_coord_original >> 3;

  static const uint8 kLink_MoveY_RecoilOther_dy[32] = { 16, 16, 20, 20, 24, 24, 28, 28, 32, 32, 36, 36, 40, 40, 44, 44, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48 };
  static const uint8 kLink_MoveY_RecoilOther_dz[32] = { 24, 24, 24, 24, 28, 28, 28, 28, 32, 32, 32, 32, 36, 36, 36, 36, 40, 40, 40, 40, 44, 44, 44, 44, 48, 48, 48, 48, 52, 52, 52, 52 };
  static const uint8 kLink_MoveY_RecoilOther_timer[32] = { 16, 16, 20, 20, 24, 24, 28, 28, 32, 32, 36, 36, 40, 40, 44, 44, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48 };

  int8 dy = kLink_MoveY_RecoilOther_dy[o];
  link_actual_vel_y = (link_last_direction_moved_towards != 0) ? dy : -dy;
  link_actual_vel_x = 0;
  link_actual_vel_z_copy = link_actual_vel_z = kLink_MoveY_RecoilOther_dz[o];
  link_z_coord = 0;
  link_incapacitated_timer = kLink_MoveY_RecoilOther_timer[o];
  link_auxiliary_state = 2;
  link_electrocute_on_touch = 0;
  link_player_handler_state = 6;
}

/*
 * Link_FindValidLandingTile_DiagonalNorth — Diagonal northward
 * landing-search variant. Mirror of LinkHop_FindLandingSpotDiagonally-
 * Down but going up; derives the (dx, dy, dz) arc from the
 * kLink_JumpOffLedgeUpDown_* 32-entry tuned tables. Drops into
 * state 13 (HoppingDiagonallyUpOW).
 */
void Link_FindValidLandingTile_DiagonalNorth() {  // 87c46d
  uint8 b0 = link_y_coord_safe_return_lo;
  uint16 b1 = link_x_coord;
  uint8 dir = link_last_direction_moved_towards;

  link_actual_vel_x = (link_last_direction_moved_towards != 2 ? 1 : -1);
  link_last_direction_moved_towards = 0;
  LinkHop_FindLandingSpotDiagonallyDown();

  link_x_coord = b1;
  link_y_coord_safe_return_lo = b0;

  uint16 o = (uint16)(link_y_coord_original - link_y_coord) >> 3;
  link_y_coord = link_y_coord_original;

  static const uint8 kLink_JumpOffLedgeUpDown_dx[32] = { 8, 8, 8, 8, 16, 16, 16, 16, 24, 24, 24, 24, 16, 16, 16, 16, 8, 20, 20, 20, 24, 24, 24, 24, 28, 28, 28, 28, 32, 32, 32, 32 };
  static const uint8 kLink_JumpOffLedgeUpDown_dy[32] = { 8, 8, 8, 8, 16, 16, 20, 20, 24, 24, 24, 24, 32, 32, 32, 32, 8, 20, 20, 20, 24, 24, 24, 24, 28, 28, 28, 28, 32, 32, 32, 32 };
  static const uint8 kLink_JumpOffLedgeUpDown_dz[32] = { 32, 32, 32, 32, 32, 32, 32, 32, 36, 36, 36, 36, 40, 40, 40, 40, 32, 40, 40, 40, 44, 44, 44, 44, 48, 48, 48, 48, 52, 52, 52, 52 };

  link_actual_vel_y = -kLink_JumpOffLedgeUpDown_dy[o];
  uint8 dx = kLink_JumpOffLedgeUpDown_dx[o];
  link_actual_vel_x = (dir != 2) ? dx : -dx;
  link_actual_vel_z_copy = link_actual_vel_z = kLink_JumpOffLedgeUpDown_dz[o];
  link_z_coord = 0;
  link_z_coord_mirror &= ~0xff;
  link_auxiliary_state = 2;
  link_electrocute_on_touch = 0;
  link_player_handler_state = 13;
}

/*
 * StartMovementCollisionChecks_X — X-axis collision driver. Mirror
 * of the Y version: pick direction (2=west, 3=east) from doorway-
 * latched X or current X velocity, run tile-detect, dispatch to
 * indoor/outdoor X handler.
 */
void StartMovementCollisionChecks_X() {  // 87c4d4
  if (!link_x_vel)
    return;

  if (is_standing_in_doorway == 2)
    link_last_direction_moved_towards = (uint8)link_x_coord < 0x80 ? 2 : 3;
  else
    link_last_direction_moved_towards = sign8(link_x_vel) ? 2 : 3;
  TileDetect_Movement_X(link_last_direction_moved_towards);
  if (player_is_indoors)
    StartMovementCollisionChecks_X_HandleIndoors();
  else
    StartMovementCollisionChecks_X_HandleOutdoors();
}

/*
 * StartMovementCollisionChecks_X_HandleIndoors — X-axis indoor tile
 * response. Same shape as the Y indoor version: doorway/wall/slope
 * resolution, conveyor handling, rupee floor tile pickup, and the
 * ledge-hop / deep-water transitions. The slight differences from
 * the Y version reflect the asymmetry of dungeon room geometry
 * (X doorways are typically wider and rarer).
 */
void StartMovementCollisionChecks_X_HandleIndoors() {  // 87c4ff
  if (sign8(link_state_bits) || link_incapacitated_timer != 0) {
    R14 |= R14 >> 4;
  } else {
    if (link_num_orthogonal_directions == 0)
      link_speed_modifier = 0;
    if (is_standing_in_doorway == 1 && link_num_orthogonal_directions == 0) {
      if (dung_hdr_collision != 3 || link_is_on_lower_level == 0) {
        SnapOnX();
        int8 spd = ChangeAxisOfPerpendicularDoorMovement_X();
        HandleNudgingInADoor(spd);
        return;
      }
      goto label_3;
    } // else_3

    if (R14 & 0x70) {
      if ((R14 >> 8) & 7) {
        force_move_any_direction = (sign8(link_x_vel)) ? 2 : 1;
      } // endif_6

      is_standing_in_doorway = 2;
      link_on_conveyor_belt = 0;
      if ((R14 & 0x70) != 0x70) {
        if (R14 & 7) { // if_7
          link_moving_against_diag_tile = 0;
          is_standing_in_doorway = 0;
          SnapOnX();
          CalculateSnapScratch_X();
          return;
        }
        if (R14 & 0x70)
          goto else_7;
      } else { // else_7
else_7:
        if (!(tile_coll_flag & 2))
          link_cant_change_direction &= ~2;
        return;
      }
    }
  } // endif_1

  if (!(tile_coll_flag & 2)) {
    link_cant_change_direction &= ~2;
    is_standing_in_doorway = 0;
    room_transitioning_flags = 0;
    force_move_any_direction = 0;
  }  // label_3

label_3:

  if ((R14 & 2) == 0 && (R12 & 5) != 0) {
    link_on_conveyor_belt = 0;
    FlagMovingIntoSlopes_X();
    if ((link_moving_against_diag_tile & 0xf) != 0)
      return;
  } // endif_9

  link_moving_against_diag_tile = 0;
  if (!link_is_on_lower_level) {
    if (tiledetect_water_staircase & 7) {
      byte_7E0322 |= 1;
    } else if ((bitfield_spike_cactus_tiles & 7) == 0 && (R14 & 2) == 0) { // else_11
      byte_7E0322 &= ~1;
    } // endif_11
  } else { // else_10
    if ((tiledetect_moving_floor_tiles & 7) != 0) {
      byte_7E0322 |= 2;
    } else {
      byte_7E0322 &= ~2;
    }
  } // endif_11

  if (tiledetect_misc_tiles & 0x2200) {
    uint16 dy = tiledetect_misc_tiles & 0x2000 ? 8 : 0;

    static const uint8 kLink_DoMoveXCoord_Indoors_dx[] = { 8, 8, 0, 15 };
    static const uint8 kLink_DoMoveXCoord_Indoors_dy[] = { 8, 24, 16, 16 };

    link_rupees_goal += 5;
    uint16 y = link_y_coord + kLink_DoMoveXCoord_Indoors_dy[link_last_direction_moved_towards] - dy;
    uint16 x = link_x_coord + kLink_DoMoveXCoord_Indoors_dx[link_last_direction_moved_towards];

    Dungeon_DeleteRupeeTile(x, y);
    Ancilla_Sfx3_Near(10);
  }  // endif_12_norupee

  if (tiledetect_var4 & 0x22) {
    link_on_conveyor_belt = tiledetect_var4 & 0x20 ? 2 : 1;
  } else if (tiledetect_var4 & 0x2200) {
    link_on_conveyor_belt = tiledetect_var4 & 0x2000 ? 4 : 3;
  } else {
    if (!(bitfield_spike_cactus_tiles & 7) && !(R14 & 2))
      link_on_conveyor_belt = 0;
  } // endif_15

  if ((detection_of_ledge_tiles_horiz_uphoriz & 7) == 7 && RunLedgeHopTimer()) {
    Link_CancelDash();
    about_to_jump_off_ledge++;
    link_auxiliary_state = 2;
    goto endif_19;
  } else if ((tiledetect_deepwater & 7) == 7 && link_is_in_deep_water == 0 && link_player_handler_state != 6) {
    // if_20
    link_y_coord = link_y_coord_safe_return_lo | link_y_coord_safe_return_hi << 8;
    link_x_coord = link_x_coord_safe_return_lo | link_x_coord_safe_return_hi << 8;
    Link_CancelDash();
    if (TS_copy == 0) {
      Dungeon_HandleLayerChange();
    } else {
      link_is_in_deep_water = 1;
      link_some_direction_bits = link_direction_last;
      link_state_bits = 0;
      link_picking_throw_state = 0;
      link_grabbing_wall = 0;
      link_speed_setting = 0;
      Link_ResetSwimmingState();
    }
endif_19:
    link_disable_sprite_damage = 1;
    Link_HopInOrOutOfWater_X();
    Ancilla_Sfx2_Near(0x20);
  } else {
    // else_20
    if ((tiledetect_normal_tiles & 7) == 7 && link_is_in_deep_water != 0) {
      if (link_auxiliary_state != 0) {
        R14 = 7;
      } else {
        Link_CancelDash();
        if (link_auxiliary_state == 0) {
          link_direction_last = link_some_direction_bits;
          link_is_in_deep_water = 0;
          AncillaAdd_Splash(0x15, 0);
          link_disable_sprite_damage = 1;
          Link_HopInOrOutOfWater_X();
        }
      }
    }
  } // endif_21

  if (tiledetect_pit_tile & 5 && (R14 & 2) == 0) {
    if (link_player_handler_state == 5 || link_player_handler_state == 2)
      return;
    byte_7E005C = 9;
    link_this_controls_sprite_oam = 0;
    player_near_pit_state = 1;
    link_player_handler_state = kPlayerState_FallingIntoHole;
    return;
  } // endif_23

  player_near_pit_state = 0;

  if (bitfield_spike_cactus_tiles & 7) {
    if ((link_incapacitated_timer | countdown_for_blink | link_cape_mode) == 0) {
      if (((link_last_direction_moved_towards == 2) ? (link_x_coord & 4) == 0 : ((link_x_coord & 4) != 0)) && (countdown_for_blink == 0)) {
        link_give_damage = 8;
        Link_CancelDash();
        Link_ForceUnequipCape_quietly();
        LinkApplyTileRebound();
        return;
      }
    } else {
      R14 &= ~0xFF;
      R14 |= bitfield_spike_cactus_tiles & 7;
    } // endif_24
  } // endif_24
  if (dung_hdr_collision == 0 || dung_hdr_collision == 4 || !link_is_on_lower_level) {
    if (tiledetect_var2 && link_num_orthogonal_directions == 0) {
      byte_7E02C2 = tiledetect_var2;
      if (!sign8(--gravestone_push_timeout))
        goto endif_26;
      uint16 bits = tiledetect_var2;
      int i = 15;
      do {
        if (bits & 0x8000) {
          uint8 idx = FindFreeMovingBlockSlot(i);
          if (idx == 0xff)
            continue;
          R14 = idx;  // This seems like it's overwriting the tiledetector's stuff
          if (InitializePushBlock(idx, i * 2))
            continue;
          Sprite_Dungeon_DrawSinglePushBlock(idx * 2);
          R14 = 4;
          pushedblock_facing[idx] = link_last_direction_moved_towards * 2;
          push_block_direction = link_last_direction_moved_towards * 2;
          pushedblocks_target[idx] = (pushedblocks_x_lo[idx] - (link_last_direction_moved_towards != 2)) & 0xf;
        }
      } while (bits <<= 1, --i >= 0);
    }
    // endif_27
    gravestone_push_timeout = 21;
  }
  // endif_26
endif_26:
  if (link_num_orthogonal_directions == 0) {
    link_speed_modifier = 0;
    if (link_speed_setting == 2)
      link_speed_setting = 0;
  }
  HandlePushingBonkingSnaps_X();
}

/*
 * HandlePushingBonkingSnaps_X — X-axis sub-tile resolution. Mirror
 * of HandlePushingBonkingSnaps_Y: bonk walls, snap to tile boundary
 * when pushing into one, nudge Y when entering a doorway sideways,
 * accumulate the push-get-tired timer toward dragstate.
 */
void HandlePushingBonkingSnaps_X() {  // 87c7fc
  if (R14 & 7) {
    if (link_player_handler_state == kPlayerState_Swimming && (uint8)dung_floor_x_vel == 0)
      ResetAllAcceleration();

    if (R14 & 2) {
      uint16 bak = R14;
      Link_BonkAndSmash();
      RepelDash();
      R14 = bak;
    }

    fallhole_var1 = 1;

    if ((R14 & 7) == 7) {
      SnapOnX();
    } else {
      if (link_num_orthogonal_directions == 2)
        goto returnb;
      SnapOnX();
      if (link_num_orthogonal_directions == 1)
        goto returnb;
    } // endif_4

    if ((R14 & 5) == 5) {
      Link_BonkAndSmash();
      RepelDash();
    } else if (R14 & 4) {
      uint8 tt = sign8(link_x_vel) ? link_x_vel : -link_x_vel;
      uint8 r0 = sign8(tt) ? 0xff : 1;
      if ((R14 & 2) == 0) {
        if (link_y_coord & 7) {
          link_y_coord += (int8)r0;
          HandleNudging(r0);
          return;
        }
        Link_BonkAndSmash();
        RepelDash();
      }
    } else { // else_7
      uint8 tt = sign8(link_x_vel) ? -link_x_vel : link_x_vel;
      uint8 r0 = sign8(tt) ? 0xff : 1;
      if ((R14 & 2) == 0) {
        if (link_y_coord & 7) {
          link_y_coord += (int8)r0;
          HandleNudging(r0);
          return;
        }
        Link_BonkAndSmash();
        RepelDash();
      }
    }
    // endif_10
    if (link_last_direction_moved_towards * 2 == link_direction_facing) {
      bitmask_of_dragstate |= (tile_coll_flag & 1) << 1;
      if (button_b_frames == 0 && !sign8(--link_timer_push_get_tired))
        return;

      bitmask_of_dragstate |= (tiledetect_misc_tiles & 0x20) ? tile_coll_flag << 3 : tile_coll_flag;
    }
  } else {// else_1
    if (link_is_on_lower_level)
      return;
    bitmask_of_dragstate &= ~9;
  } // endif_1

returnb:
  link_timer_push_get_tired = 32;
  bitmask_of_dragstate &= ~2;
}

/*
 * StartMovementCollisionChecks_X_HandleOutdoors — X-axis outdoor
 * tile response. Mirror of the Y outdoor variant: pit-fall trigger,
 * deep-water entry (with the flippers + non-bunny swim transition
 * vs hop-out-of-water fallback), and the standard bonk/snap pipeline.
 * Also handles ledge-tile detection on the X axis for east/west
 * hop-down transitions.
 */
void StartMovementCollisionChecks_X_HandleOutdoors() {  // 87c8e9
  if (link_num_orthogonal_directions == 0) {
    link_speed_modifier = 0;
    if (link_speed_setting == 2)
      link_speed_setting = 0;
  }

  if ((tiledetect_pit_tile & 5) != 0 && (R14 & 2) == 0) {
    if (link_player_handler_state != 5 && link_player_handler_state != 2) {
      // start fall into hole
      byte_7E005C = 9;
      link_this_controls_sprite_oam = 0;
      player_near_pit_state = 1;
      link_player_handler_state = kPlayerState_FallingIntoHole;
    }
    return;
  }

  if (tiledetect_read_something & 2) {
    interacting_with_liftable_tile_x1b = interacting_with_liftable_tile_x2 >> 1;
  } else {
    interacting_with_liftable_tile_x1b = 0;
  }  // endif_2

  if ((tiledetect_deepwater & 4) && !link_is_in_deep_water && !link_auxiliary_state) {
    Link_CancelDash();
    Link_ResetSwordAndItemUsage();
    link_is_in_deep_water = 1;
    link_some_direction_bits = link_direction_last;
    Link_ResetSwimmingState();
    link_grabbing_wall = 0;
    link_speed_setting = 0;
    if ((draw_water_ripples_or_grass == 1) && (Link_ForceUnequipCape_quietly(), link_item_flippers != 0)) {
      if (!link_is_bunny_mirror)
        link_player_handler_state = kPlayerState_Swimming;
    } else {
      link_y_coord = (link_y_coord_safe_return_hi << 8) | link_y_coord_safe_return_lo;
      link_x_coord = (link_x_coord_safe_return_hi << 8) | link_x_coord_safe_return_lo;
      link_disable_sprite_damage = 1;
      Link_HopInOrOutOfWater_X();
      Ancilla_Sfx2_Near(0x20);
    }
  }  // endif_afterSwimCheck

  if (link_is_in_deep_water ? ((detection_of_ledge_tiles_horiz_uphoriz & 7) == 7) : (tiledetect_vertical_ledge & 0x42)) {
    // not implemented, jumps to another routine
    R14 = 7;
    HandlePushingBonkingSnaps_X();
    return;
  } // endif_3

  if ((tiledetect_normal_tiles & 7) == 7 && link_is_in_deep_water) {
    Link_CancelDash();
    if (!link_auxiliary_state) {
      link_direction_last = link_some_direction_bits;
      link_is_in_deep_water = 0;
      AncillaAdd_Splash(0x15, 0);
      link_disable_sprite_damage = 1;
      Link_HopInOrOutOfWater_X();
      return;
    }
  }  // endif_4

  if ((detection_of_ledge_tiles_horiz_uphoriz & 7) != 0 && RunLedgeHopTimer()) {
    Ancilla_Sfx2_Near(0x20);
    link_actual_vel_x = (link_last_direction_moved_towards & 1) ? 0x10 : -0x10;
    Link_CancelDash();
    link_auxiliary_state = 2;
    link_actual_vel_z_mirror = link_actual_vel_z_copy_mirror = 20;
    link_z_coord_mirror |= 0xff;
    link_player_handler_state = kPlayerState_FallOfLeftRightLedge;
    link_disable_sprite_damage = 1;
    allow_scroll_z = 1;
    bitmask_of_dragstate = 0;
    link_speed_setting = 0;
    if (!player_is_indoors)
      link_is_on_lower_level = 2;

    uint16 xbak = link_x_coord;
    uint8 rv = Link_HoppingHorizontally_FindTile_X((link_last_direction_moved_towards & ~2) * 2);
    link_last_direction_moved_towards = 1;
    if (rv != 0xff) {
      Link_HoppingHorizontally_FindTile_Y();
    } else {
      LinkHop_FindTileToLandOnSouth();
    }
    link_x_coord = xbak;
    return;
  }  // endif_5

  if ((detection_of_unknown_tile_types & 0x77) != 0 && RunLedgeHopTimer()) {
    uint8 sfx = Ancilla_Sfx2_Near(0x20);
    link_player_handler_state = (sfx & 7) == 0 ? 16 : 15;
    link_actual_vel_x = (link_last_direction_moved_towards & 1) ? 0x10 : -0x10;
    Link_CancelDash();
    link_auxiliary_state = 2;
    link_actual_vel_z_mirror = link_actual_vel_z_copy_mirror = 20;
    link_z_coord_mirror |= 0xff;
    link_incapacitated_timer = 0;
    link_disable_sprite_damage = 1;
    allow_scroll_z = 1;
    bitmask_of_dragstate = 0;
    link_speed_setting = 0;
    return;
  }  // endif_6

  if ((detection_of_ledge_tiles_horiz_uphoriz & 0x70) != 0 &&
      (detection_of_ledge_tiles_horiz_uphoriz & 0x7) == 0 &&
      (detection_of_unknown_tile_types & 0x77) == 0 &&
      link_player_handler_state != 13 && RunLedgeHopTimer()) {
    Ancilla_Sfx2_Near(0x20);
    Link_CancelDash();
    link_disable_sprite_damage = 1;
    bitmask_of_dragstate = 0;
    link_speed_setting = 0;
    Link_FindValidLandingTile_DiagonalNorth();
    return;
  }  // endif_7

  if ((tiledetect_ledges_down_leftright & 7) != 0 && (detection_of_ledge_tiles_horiz_uphoriz & 7) == 0 &&
      (detection_of_unknown_tile_types & 0x77) == 0 && RunLedgeHopTimer()) {
    link_actual_vel_x = (link_last_direction_moved_towards & 1) ? 0x10 : -0x10;
    Link_CancelDash();
    link_auxiliary_state = 2;
    link_actual_vel_z_mirror = link_actual_vel_z_copy_mirror = 20;
    link_z_coord_mirror |= 0xff;
    link_player_handler_state = 14;
    link_incapacitated_timer = 0;
    link_disable_sprite_damage = 1;
    allow_scroll_z = 1;
    bitmask_of_dragstate = 0;
    link_speed_setting = 0;
    return;
  } // endif_8

  // If force facing down (hold B button), while turboing on the Run key, we'll never
  // reach FlagMovingIntoSlopes_X causing a Dash Buffering glitch.
  // Fix by always calling it, not sure why you wouldn't always want to call it.
  if ((R14 & 2) == 0 && (R12 & 5) != 0) {
    bool skip_check = link_is_running && !(link_direction_facing & 4);
    if (!skip_check || (enhanced_features0 & kFeatures0_MiscBugFixes)) {
      FlagMovingIntoSlopes_X();
      if ((link_moving_against_diag_tile & 0xf) != 0)
        return;
    }  // endif_9
  }

  link_moving_against_diag_tile = 0;

  if ((bitfield_spike_cactus_tiles & 7) != 0) {
    if ((link_incapacitated_timer | countdown_for_blink | link_cape_mode) == 0) {
      if (link_last_direction_moved_towards == 2 ? ((link_x_coord & 4) == 0) : ((link_x_coord & 4) != 0)) {
        link_give_damage = 8;
        Link_CancelDash();
        LinkApplyTileRebound();
        return;
      }
    } else {
      R14 = bitfield_spike_cactus_tiles & 7;
    }
  }  // endif_10
  HandlePushingBonkingSnaps_X();
}

/*
 * SnapOnX — X-axis analog of Link_AddInVelocityYFalling: snap
 * Link's X position to the tile boundary based on his velocity sign.
 */
void SnapOnX() {  // 87cb84
  link_x_coord -= (link_x_coord & 7) - (sign8(link_x_vel) ? 8 : 0);
}

/*
 * CalculateSnapScratch_X — Sub-pixel Y nudge when entering an X-axis
 * doorway. The two branches handle "doorway is to the right" vs
 * "doorway is to the left". Author note "wtf" preserved.
 */
void CalculateSnapScratch_X() {  // 87cb9f
  if (R14 & 4) {
    int8 x = link_x_vel;
    if (x >= 0) x = -x; // wtf
    link_y_coord += x < 0 ? -1 : 1;
  } else {
    int8 x = link_x_vel;
    if (x < 0) x = -x;
    link_y_coord += x < 0 ? -1 : 1;
  }
}

/*
 * ChangeAxisOfPerpendicularDoorMovement_X — X-axis mirror of the Y
 * version. When trying to move perpendicular through a door,
 * rotate Link to face along the door axis and push 1 px toward
 * centerline. Returns the resolved velocity for the caller's nudge.
 */
int8 ChangeAxisOfPerpendicularDoorMovement_X() {  // 87cbdd
  link_cant_change_direction |= 2;
  uint8 r0 = (R14 | (R14 >> 4)) & 0xf;
  if ((r0 & 7) == 0) {
    is_standing_in_doorway = 0;
    return r0; // wtf?
  }

  int8 x_vel = link_x_vel;
  uint8 dir;
  if ((uint8)link_y_coord >= 0x80) {
    if (x_vel >= 0) x_vel = -x_vel;
    dir = 0;
  } else {
    if (x_vel < 0) x_vel = -x_vel;
    dir = 2;
  }
  if (!(link_cant_change_direction & 1))
    link_direction_facing = dir;
  link_y_coord += x_vel;
  return x_vel;
}

/*
 * Link_HopInOrOutOfWater_X — Bunny-bounce hop variant for east/west
 * water borders. Same three-tier speed selection as the Y version
 * but produces X velocity instead. Enters state 6 (RecoilOther).
 */
void Link_HopInOrOutOfWater_X() {  // 87cc3c
  static const uint8 kRecoilVelX[] = { 28, 24, 16 };
  static const uint8 kRecoilVelZ[] = { 32, 24, 24 };

  uint8 ts = !player_is_indoors ? 2 :
    about_to_jump_off_ledge ? 0 : TS_copy;

  int8 vel = kRecoilVelX[ts];
  if (!(link_last_direction_moved_towards & 1))
    vel = -vel;
  link_actual_vel_x = vel;
  link_actual_vel_y = 0;
  link_actual_vel_z_copy = link_actual_vel_z = kRecoilVelZ[ts];
  link_incapacitated_timer = 16;
  if (link_auxiliary_state != 2) {
    link_auxiliary_state = 1;
    link_electrocute_on_touch = 0;
  }
  link_player_handler_state = 6;
}

/*
 * Link_HandleDiagonalKickback — When Link is moving diagonally and
 * both axes hit a slope, this routine resolves the diagonal corner
 * case. Pre-snapshots his coords, runs the X and Y slope checks in
 * sequence, and if both axes are blocked, decelerates him via the
 * x0/x1/y0/y1 micro-step tables (1..3 pixel nudges per velocity
 * magnitude).
 *
 * The "Bug in zelda, might read index 15" note flags the original
 * ROM's OOB-read past the y1 table's logical 10 entries (we keep
 * the same data shape for compatibility).
 */
void Link_HandleDiagonalKickback() {  // 87ccab
  if (link_x_vel && link_y_vel) {
    link_y_coord_copy = link_y_coord;
    link_x_coord_copy = link_x_coord;

    TileDetect_Movement_X(sign8(link_x_vel) ? 2 : 3);
    if ((R12 & 5) == 0)
      goto noHorizOrNoVertical;
    FlagMovingIntoSlopes_X();
    if (!(link_moving_against_diag_tile & 0xf))
      goto noHorizOrNoVertical;

    int8 xd = link_x_coord - link_x_coord_copy;
    link_x_coord = link_x_coord_copy;
    link_x_vel = xd;

    TileDetect_Movement_Y(sign8(link_y_vel) ? 0 : 1);
    if ((R12 & 5) == 0)
      goto noHorizOrNoVertical;
    FlagMovingIntoSlopes_Y();
    if (!(link_moving_against_diag_tile & 0xf))
      goto noHorizOrNoVertical;

    moving_against_diag_deadlocked = link_moving_against_diag_tile;

    int8 yd = link_y_coord - link_y_coord_copy;
    link_y_vel = yd;

    static const int8 x0[] = { 0, 1, 1, 1, 2, 2, 2, 3, 3, 3 };
    static const int8 x1[] = { 0, -1, -1, -1, -2, -2, -2, -3, -3, -3 };
    link_x_coord += sign8(link_x_vel) ? x1[-(int8)link_x_vel] : x0[link_x_vel];

    static const int8 y0[10] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3 };
    // Bug in zelda, might read index 15
    static const int8 y1[16] = { 0, 1, 1, 2, 2, 2, 3, 3, 3, 3, 0xa5, 0x30, 0xf0, 0x04, 0xa5, 0x31 };
    link_y_coord += sign8(link_y_vel) ? y1[-(int8)link_y_vel] : y0[link_y_vel];
  } else {
noHorizOrNoVertical:
    moving_against_diag_deadlocked = 0;
  }
  link_moving_against_diag_tile = 0;
}

/*
 * TileDetect_MainHandler — Primary tile-attribute probe driver.
 * Dispatches into TileDetection_Execute (the in-tile_detect.c hub)
 * with a position offset chosen from the kDoSwordInteractionWithTiles_*
 * tables based on the (`item`, `link_direction_facing`) pair.
 *
 * `item` is a probe-purpose enum:
 *   0: standard ground-tile under Link.
 *   1: powder/bush interaction.
 *   2: shovel.
 *   3: hammer.
 *   4: pit detection.
 *   5: liftable check (no post-detect dispatch).
 *   6,7: bow/boomerang (TileBehavior_HandleItemAndExecute path).
 *   8: spin-attack — uses the special kDoSwordInteractionWithTiles_o
 *     remap table to pick a rotated probe direction per spin frame.
 *
 * After the probe, this also dispatches per-tile-effect cases:
 *   thick-grass with bit 0x10 + sub-tile alignment + visibility =
 *     intra-room teleport (indoors -> travel_destinations[0],
 *     outdoors -> mirror trigger). Otherwise sets the visual
 *     "wading through grass" overlay flag.
 */
void TileDetect_MainHandler(uint8 item) {  // 87d077
  tiledetect_pit_tile = 0;
  TileDetect_ResetState();
  uint16 o;

  if (item == 8) {
    int16 a = state_for_spin_attack - 2;
    if (a < 0 || a >= 8)
      return;
    static const uint8 kDoSwordInteractionWithTiles_o[] = { 10, 6, 14, 2, 12, 4, 8, 0 };
    o = kDoSwordInteractionWithTiles_o[a] + 0x40;
  } else {
    o = item * 8 + link_direction_facing;
  }
  o >>= 1;

  static const int8 kDoSwordInteractionWithTiles_x[] = { 8, 8, 8, 8, 6, 8, -1, 22, 19, 19, 0, 19, 6, 8, -1, 22, 8, 8, 8, 8, 8, 8, 0, 15, 6, 8, -10, 29, 6, 8, -6, 22, 6, 8, -4, 22, -4, 22, -4, 22 };
  static const int8 kDoSwordInteractionWithTiles_y[] = { 20, 20, 20, 20, 4, 28, 16, 16, 22, 22, 22, 22, 4, 24, 16, 16, 16, 16, 16, 16, 20, 20, 23, 23, -4, 36, 16, 16, 4, 28, 16, 16, 4, 28, 16, 16, 4, 4, 28, 28 };
  uint16 x = ((link_x_coord + kDoSwordInteractionWithTiles_x[o]) & tilemap_location_calc_mask) >> 3;
  uint16 y = ((link_y_coord + kDoSwordInteractionWithTiles_y[o]) & tilemap_location_calc_mask);

  if (item == 1 || item == 2 || item == 3 || item == 6 || item == 7 || item == 8) {
    TileBehavior_HandleItemAndExecute(x, y);
    return;
  }

  TileDetection_Execute(x, y, 1);

  if (item == 5)
    return;

  if (tiledetect_thick_grass & 0x10) {
    uint8 tx = (link_x_coord + 0) & 0xf;
    uint8 ty = (link_y_coord + 8) & 0xf;

    if ((ty < 4 || ty >= 11) && (tx < 4 || tx >= 12) && countdown_for_blink == 0 && link_auxiliary_state == 0) {
      if (player_is_indoors) {
        Dungeon_FlagRoomData_Quadrants();
        Ancilla_Sfx2_Near(0x33);
        link_speed_setting = 0;
        submodule_index = 21;
        BYTE(dungeon_room_index_prev) = dungeon_room_index;
        BYTE(dungeon_room_index) = dung_hdr_travel_destinations[0];
        HandleLayerOfDestination();
      } else if (!link_triggered_by_whirlpool_sprite) {
        DoSwordInteractionWithTiles_Mirror();
      }
    }
  } else {  // else_3
    link_triggered_by_whirlpool_sprite = 0;
    if (tiledetect_thick_grass & 1) {
      draw_water_ripples_or_grass = 2;
      if (!Link_PermissionForSloshSounds() && link_auxiliary_state == 0)
        Ancilla_Sfx2_Near(26);
      return;
    }

    if (tiledetect_shallow_water & 1) {
      draw_water_ripples_or_grass = 1;

      if (!player_is_indoors && link_is_in_deep_water && !link_is_bunny_mirror) {
        if (link_item_flippers) {
          link_is_in_deep_water = 0;
          link_direction_last = link_some_direction_bits;
          link_player_handler_state = 0;
        }
      } else if (!Link_PermissionForSloshSounds()) {
        if ((uint8)overworld_screen_index == 0x70) {
          Ancilla_Sfx2_Near(27);
        } else if (link_auxiliary_state == 0) {
          Ancilla_Sfx2_Near(28);
        }
      }
      return;
    }

    if (!player_is_indoors && !link_is_in_deep_water && (tiledetect_deepwater & 1)) {
      draw_water_ripples_or_grass = 1;
      if (!Link_PermissionForSloshSounds()) {
        if ((uint8)overworld_screen_index == 0x70) {
          Ancilla_Sfx2_Near(27);
        } else if (link_auxiliary_state == 0) {
          Ancilla_Sfx2_Near(28);
        }
      }
      return;
    }
  }
  // else_6
  draw_water_ripples_or_grass = 0;

  if (tiledetect_spike_floor_and_tile_triggers & 1) {
    byte_7E02ED = 1;
    return;
  }

  byte_7E02ED = 0;

  if (tiledetect_spike_floor_and_tile_triggers & 0x10) {
    link_give_damage = 0;
    if (!link_cape_mode && !SearchForByrnaSpark() && !countdown_for_blink) {
      link_need_for_poof_for_transform = 0;
      link_timer_tempbunny = 0;
      if (link_item_moon_pearl) {
        link_is_bunny = 0;
        link_is_bunny_mirror = 0;
      }
      link_give_damage = 8;
      Link_CancelDash();
      return;
    }
  }

  if (tiledetect_icy_floor & 0x11) {
    if (link_flag_moving) {
      if (link_num_orthogonal_directions)
        link_direction_last = link_some_direction_bits;
    } else { // else_11
      if (link_direction & 0xC)
        swimcoll_var7[0] = 0x180;
      if (link_direction & 3)
        swimcoll_var7[0] = 0x180;

      link_flag_moving = (tiledetect_icy_floor & 1) ? 1 : 2;
      link_some_direction_bits = link_direction_last;
      Link_ResetSwimmingState();
    }
  } else {
    if (link_player_handler_state != 4) {
      if (link_flag_moving)
        link_direction_last = link_some_direction_bits;
      Link_ResetSwimmingState();
    }
    link_flag_moving = 0;
  }

  if ((bitfield_spike_cactus_tiles & 0x10) && countdown_for_blink == 0)
    countdown_for_blink = 58;
}

/*
 * Link_PermissionForSloshSounds — Rate-limit the wading/sloshing
 * SFX so we don't spam it every frame Link is in grass/water. Idle
 * frame returns true (always allowed); dashing throttles 1-in-8,
 * walking 1-in-16.
 */
bool Link_PermissionForSloshSounds() {  // 87d2c6
  if (!(link_direction & 0xf))
    return true;
  if (link_player_handler_state != 17) {
    return (frame_counter & 0xf) != 0;
  } else {
    return (frame_counter & 0x7) != 0;
  }
}

/*
 * PushBlock_AttemptToPushTheBlock — Test whether the player can push
 * the given block in the current movement direction. Probes two
 * tiles ahead of the block (the destination cells the block would
 * occupy after the push) and checks the resulting attribute against
 * the T[256] permission table. Returns true if push is blocked.
 *
 * The T[] entries are tile-attribute id -> "blocks pushing" flag,
 * with the magic exception attr 9 ("ground decoration") which
 * looks blocking but is actually pushable.
 */
bool PushBlock_AttemptToPushTheBlock(uint8 what, uint16 x, uint16 y) {  // 87d304
  static const int8 kChangableDungeonObj_Func1B_y0[4] = { -4, 20, 4, 4 };
  static const int8 kChangableDungeonObj_Func1B_y1[4] = { -4, 20, 12, 12 };
  static const int8 kChangableDungeonObj_Func1B_x0[4] = { 4, 4, -4, 20 };
  static const int8 kChangableDungeonObj_Func1B_x1[4] = { 12, 12, -4, 20 };
  static const uint8 T[256] = {
    0, 1, 2, 3, 2, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1,
    0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1,
    0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1

  };

  uint8 idx = what * 4 + link_last_direction_moved_towards, xt;
  uint16 new_x, new_y;

  new_x = ((x + kChangableDungeonObj_Func1B_x0[idx]) & tilemap_location_calc_mask) >> 3;
  new_y = ((y + kChangableDungeonObj_Func1B_y0[idx]) & tilemap_location_calc_mask);
  xt = PushBlock_GetTargetTileFlag(new_x, new_y);
  if (T[xt] != 0 && xt != 9)
    return true;

  new_x = ((x + kChangableDungeonObj_Func1B_x1[idx]) & tilemap_location_calc_mask) >> 3;
  new_y = ((y + kChangableDungeonObj_Func1B_y1[idx]) & tilemap_location_calc_mask);
  xt = PushBlock_GetTargetTileFlag(new_x, new_y);
  if (T[xt] != 0 && xt != 9)
    return true;

  return false;
}

/*
 * Link_HandleLiftables — Determine the appropriate A-press action
 * for the tile Link is facing. Returns a small action enum:
 *   1: lift (needs Power Glove tier >= the tile's requirement)
 *   2: read sign (default tile attribute action)
 *   3: open door / ledge interaction
 *   4: pull (north-facing only with the right attribute)
 *   5: open chest
 *
 * The kGetBestActionToPerformOnTile_a table maps the liftable index
 * to the required Power Glove level (0=any, 1=glove, 2=mitt).
 */
uint8 Link_HandleLiftables() {  // 87d383
  static const uint8 kGetBestActionToPerformOnTile_a[7] = { 0, 1, 0, 0, 2, 1, 2 };
  static const uint8 kGetBestActionToPerformOnTile_b[7] = { 2, 3, 1, 4, 0, 5, 6 };

  tiledetect_pit_tile = 0;
  TileDetect_ResetState();

  uint16 y0 = (link_y_coord + kGetBestActionToPerformOnTile_y[link_direction_facing >> 1]) & tilemap_location_calc_mask;
  uint16 y1 = (link_y_coord + 20) & tilemap_location_calc_mask;

  uint16 x0 = ((link_x_coord + kGetBestActionToPerformOnTile_x[link_direction_facing >> 1]) & tilemap_location_calc_mask) >> 3;
  uint16 x1 = ((link_x_coord + 8) & tilemap_location_calc_mask) >> 3;

  TileDetection_Execute(x0, y0, 1);
  TileDetection_Execute(x1, y1, 2);

  uint8 action = ((R14 | tiledetect_vertical_ledge) & 1) ? 3 : 2;

  if (player_is_indoors) {
    uint8 a = Dungeon_CheckForAndIDLiftableTile();
    if (a != 0xff) {
      interacting_with_liftable_tile_x1 = kGetBestActionToPerformOnTile_b[a & 0xf];
    } else {
      if ((tiledetect_read_something & 1) && link_direction_facing == 0 && interacting_with_liftable_tile_x2 == 0)
        action = 4;
      goto getout;
    }
  } else {
    if (!(tiledetect_read_something & 1))
      goto getout;
    if (link_direction_facing == 0 && interacting_with_liftable_tile_x2 == 0) {
      action = 4;
      goto getout;
    }
    interacting_with_liftable_tile_x1 = interacting_with_liftable_tile_x2 >> 1;
  }
  if (kGetBestActionToPerformOnTile_a[interacting_with_liftable_tile_x1] - link_item_gloves <= 0)
    action = 1;
getout:
  if (tiledetect_chest & 1)
    action = 5;
  return action;
}

/*
 * HandleNudging — Sub-tile nudge fixup after a doorway/wall snap.
 * Probes the four corners of Link's new bounding box; if any
 * collision-meaningful bit comes up, reverses the requested 1-pixel
 * nudge so Link doesn't clip into a wall when entering a doorway.
 * kLink_Move_Helper6_tab0..3 are the per-direction probe offsets.
 */
void HandleNudging(int8 arg_r0) {  // 87d485
  uint8 p, o;

  if ((link_last_direction_moved_towards & 2) == 0) {
    p = (link_last_direction_moved_towards & 1) ? 4 : 0;
    o = (R14 & 4) ? 0 : 2;
  } else {
    p = (link_last_direction_moved_towards & 1) ? 12 : 8;
    o = (R14 & 4) ? 0 : 2;
  }
  o = (o + p) >> 1;

  tiledetect_pit_tile = 0;
  TileDetect_ResetState();

  uint16 y0 = (link_y_coord + kLink_Move_Helper6_tab0[o]) & tilemap_location_calc_mask;
  uint16 x0 = ((link_x_coord + kLink_Move_Helper6_tab1[o]) & tilemap_location_calc_mask) >> 3;

  uint16 y1 = (link_y_coord + kLink_Move_Helper6_tab2[o]) & tilemap_location_calc_mask;
  uint16 x1 = ((link_x_coord + kLink_Move_Helper6_tab3[o]) & tilemap_location_calc_mask) >> 3;

  TileDetection_Execute(x0, y0, 1);
  TileDetection_Execute(x1, y1, 2);

  if ((R14 | detection_of_ledge_tiles_horiz_uphoriz) & 3 ||
      (tiledetect_vertical_ledge | detection_of_unknown_tile_types) & 0x33) {

    if (link_last_direction_moved_towards & 2)
      link_y_coord -= arg_r0;
    else
      link_x_coord -= arg_r0;
  }
}

/*
 * TileBehavior_HandleItemAndExecute — Used for item-with-tile probes
 * (bow / boomerang / shovel / etc.). Runs HandleItemTileAction_-
 * Overworld to get the tile attribute, then re-runs the inner
 * tile-detect dispatcher with that attribute.
 */
void TileBehavior_HandleItemAndExecute(uint16 x, uint16 y) {  // 87dc4a
  uint8 tile = HandleItemTileAction_Overworld(x, y);
  TileDetect_ExecuteInner(tile, 0, 1, false);
}

/*
 * PushBlock_GetTargetTileFlag — Read the dungeon BG2 collision-
 * attribute byte for the tile at (x, y). 0x1000 offset selects the
 * lower-level layer when Link is on it. Layout: 8 attributes per row
 * (since each row is 32 cells * 2 bytes / 8 = 8 attrs).
 */
uint8 PushBlock_GetTargetTileFlag(uint16 x, uint16 y) {  // 87e026
  return dung_bg2_attr_table[(y & ~7) * 8 + (x & 0x3f) + (link_is_on_lower_level ? 0x1000 : 0)];
}

/*
 * FlagMovingIntoSlopes_Y — Compute the Y-axis slope-snap distance
 * for Link bumping into a slope tile. The kAvoidJudder1 table is
 * 32 entries of pre-tuned offsets that round Link's sub-pixel Y
 * to the slope's contour. The MiscBugFixes branch reorders the
 * computation to avoid a 15-pixel OOB read in the original code
 * (author note retained in the else branch).
 */
void FlagMovingIntoSlopes_Y() {  // 87e076
  int8 y = (tiledetect_which_y_pos[0] & 7);
  uint8 o = (tiledetect_diag_state * 4) + ((link_x_coord - ((R12 & 4) != 0)) & 7);

  if (tiledetect_diagonal_tile & 5) {
    int8 ym = tiledetect_which_y_pos[0] & 7;

    if (enhanced_features0 & kFeatures0_MiscBugFixes) {
      if (tiledetect_diag_state & 2) {
        ym = -ym;
      } else {
        ym = kAvoidJudder1[o] - (8 - ym);
      }
    } else {
      // This code is bad because it could cause the player
      // to move up to 15 pixels, causing an array out bounds read.
      // Not sure how it works, but changed it to look more like the X version.
      if (!(tiledetect_diag_state & 2)) {
        ym = 8 - ym; // 0 to 8
      } else {
        ym += 8; // 8 to 15
      }
      // -15 to 7
      ym = kAvoidJudder1[o] - ym;
    }

    if (link_y_vel == 0)
      return;
    if (sign8(link_y_vel))
      ym = -ym;
    y = ym;
  } else {  // else_1
    y = kAvoidJudder1[o] - y;
  } // endif_1

  if (sign8(link_y_vel)) {
    if (y <= 0)
      return;
    link_y_coord += y;
    link_moving_against_diag_tile = 8;
  } else {
    if (y >= 0)
      return;
    link_y_coord += y;
    link_moving_against_diag_tile = 4;
  }
  link_moving_against_diag_tile |= (R12 & 4) ? 0x10 + 2 : 0x10 + 1;
}

/*
 * FlagMovingIntoSlopes_X — X-axis analog of FlagMovingIntoSlopes_Y.
 * Uses the same kAvoidJudder1 table to round Link's sub-pixel X to
 * the slope contour, then nudges his X coordinate by the rounded
 * delta. Sets link_moving_against_diag_tile bits 0x20-0x2f to record
 * which slope side he's pressed against.
 */
void FlagMovingIntoSlopes_X() {  // 87e112
  int8 x = (link_x_coord - (tiledetect_diag_state == 6)) & 7;
  uint8 o = (tiledetect_diag_state * 4) + (tiledetect_which_y_pos[(R12 & 4) ? 1 : 0] & 7);

  if (tiledetect_diagonal_tile & 5) {
    int8 xm = link_x_coord & 7;

    if (tiledetect_diag_state != 4 && tiledetect_diag_state != 6) {
      xm = -xm;
    } else {
      xm = kAvoidJudder1[o] - (8 - xm);
    } // endif_5
    if (link_x_vel == 0)
      return;
    if (sign8(link_x_vel))
      xm = -xm;
    x = xm;
  } else {  // else_1
    x = kAvoidJudder1[o] - x;
  } //  endif_1

  if (sign8(link_x_vel)) {
    if (x <= 0)
      return;
    link_x_coord += x;
    link_moving_against_diag_tile = 2;
  } else {
    if (x >= 0)
      return;
    link_x_coord += x;
    link_moving_against_diag_tile = 1;
  }
  link_moving_against_diag_tile |= (tiledetect_diag_state & 2) ? 0x20 + 8 : 0x20 + 4;
}

/*
 * Link_HandleRecoiling — Derive Link's direction bits from his
 * recoil velocity and run the inner incapacitated handler per axis.
 * Y axis runs first; X axis runs after, allowing the handler to
 * combine both into a diagonal recoil.
 */
void Link_HandleRecoiling() {  // 87e1be
  link_direction = 0;
  if (link_actual_vel_y) {
    link_direction_last = (link_direction |= sign8(link_actual_vel_y) ? 8 : 4);
    Player_HandleIncapacitated_Inner2();
  }
  if (link_actual_vel_x)
    link_direction_last = (link_direction |= sign8(link_actual_vel_x) ? 2 : 1);
  Player_HandleIncapacitated_Inner2();
}

/*
 * Player_HandleIncapacitated_Inner2 — Recoil-velocity post-processing.
 * If the recoil sent Link into both axes of a diagonal wall (state
 * 2 = RecoilWall), invert both velocities so he bounces off. When
 * standing in a doorway, mask the velocity to the doorway axis only
 * (e.g., Y-doorway keeps Y motion and zeroes X).
 */
void Player_HandleIncapacitated_Inner2() {  // 87e1d7
  if ((link_moving_against_diag_tile & 0xc) && (link_moving_against_diag_tile & 3) && link_player_handler_state == 2) {
    link_actual_vel_x = -link_actual_vel_x;
    link_actual_vel_y = -link_actual_vel_y;
  }
  if (is_standing_in_doorway == 1) {
    link_direction_last &= 0xc;
    link_direction &= 0xc;
    link_actual_vel_x = 0;
  } else if (is_standing_in_doorway == 2) {
    link_direction_last &= 3;
    link_direction &= 3;
    link_actual_vel_y = 0;
  }
}

/*
 * Link_HandleVelocity — Per-frame "compute Link's velocity and move"
 * routine — the heart of the movement integrator.
 *
 * Major branches:
 *   - In submodule 14/2 (menu transition) or link_prevent_from_moving:
 *     skip velocity calc, just freeze in place.
 *   - Swimming: delegate to HandleSwimStrokeAndSubpixels.
 *   - On a forced-motion surface (link_flag_moving) without running:
 *     delegate to HandleSwimStrokeAndSubpixels (which doubles as the
 *     "slide" integrator).
 *   - Otherwise: derive `r0` (a speed-table index) from
 *     link_speed_setting, adjust for grass/water drag, optionally
 *     accelerate during pit-fall, then look up the actual per-frame
 *     pixel velocity in the 27-entry kSpeedMod table. Apply velocity
 *     to actual_vel_x/y based on link_direction bits.
 *   - Then run Link_MovePosition.
 *
 * The `link_speed_modifier` tier (0..16) is a smoothing factor for
 * transitions between speed states; entry 26 in kSpeedMod is 0 so
 * `r0 = 26` is the "stationary" sentinel during deceleration.
 */
void Link_HandleVelocity() {  // 87e245
  if (submodule_index == 2 && main_module_index == 14 || link_prevent_from_moving) {
    link_y_coord_safe_return_lo = link_y_coord;
    link_y_coord_safe_return_hi = link_y_coord >> 8;
    link_x_coord_safe_return_lo = link_x_coord;
    link_x_coord_safe_return_hi = link_x_coord >> 8;
    Link_HandleVelocityAndSandDrag(link_x_coord, link_y_coord);
    return;
  }

  if (link_player_handler_state == kPlayerState_Swimming) {
    HandleSwimStrokeAndSubpixels();
    return;
  }
  uint8 r0;

  if (link_flag_moving) {
    if (!link_is_running) {
      HandleSwimStrokeAndSubpixels();
      return;
    }
    r0 = 24;
  } else {
    if (link_is_running) {
      link_speed_modifier = 0;
      assert(link_dash_ctr >= 32);
    }

    if ((byte_7E0316 | byte_7E0317) == 0xf)
      return;

    r0 = link_speed_setting;
    if (draw_water_ripples_or_grass) {
      r0 = (link_speed_setting == 16) ? 22 :
        (link_speed_setting == 12) ? 14 : 12;
    }
  }  // endif_4

  link_actual_vel_x = link_actual_vel_y = 0;
  link_y_page_movement_delta = link_x_page_movement_delta = 0;

  r0 += ((link_direction & 0xC) != 0 && (link_direction & 3) != 0);

  if (player_near_pit_state) {
    if (player_near_pit_state == 3)
      link_speed_modifier = (link_speed_modifier < 48) ? link_speed_modifier + 8 : 32;
  } else {
    if (link_speed_modifier) {
      r0 = (submodule_index == 8 || submodule_index == 16) ? 10 : 2;
      if (link_speed_modifier != 1) {
        if (link_speed_modifier < 16) {
          link_speed_modifier += 1;
          r0 = 26; // kSpeedMod[26] is 0
        } else {
          link_speed_modifier = 0;
          link_speed_setting = 0;
        }
      }
    }
  } // endif_7

  static const uint8 kSpeedMod[27] = { 24, 16, 10, 24, 16, 8, 8, 4, 12, 16, 9, 25, 20, 13, 16, 8, 64, 42, 16, 8, 4, 2, 48, 24, 32, 21, 0 };

  uint8 vel = link_speed_modifier + kSpeedMod[r0];
  if (link_direction & 3)
    link_actual_vel_x = (link_direction & 2) ? -vel : vel;
  if (link_direction & 0xC)
    link_actual_vel_y = (link_direction & 8) ? -vel : vel;

  link_actual_vel_z = 0xff;
  link_z_coord = 0xffff;
  link_subpixel_z = 0;
  Link_MovePosition();
}

/*
 * Link_MovePosition — Apply the current actual_vel_x/y/z (and
 * subpixel residues) to Link's coordinates, then run the moving-
 * floor and conveyor accumulators on top. The 16x velocity scale
 * factor produces sub-pixel precision; the >> 8 truncation yields
 * the integer pixel delta. Z is only integrated when an aux state
 * is active (Z is forced to "no air" otherwise).
 */
void Link_MovePosition() {  // 87e370
  uint16 x = link_x_coord, y = link_y_coord;
  link_y_coord_safe_return_lo = link_y_coord;
  link_y_coord_safe_return_hi = link_y_coord >> 8;
  link_x_coord_safe_return_lo = link_x_coord;
  link_x_coord_safe_return_hi = link_x_coord >> 8;

  if (link_player_handler_state != 10 && player_on_somaria_platform == 2) {
    Link_HandleVelocityAndSandDrag(x, y);
    return;
  }

  uint32 tmp;
  tmp = link_subpixel_x + (int8)link_actual_vel_x * 16 + link_x_coord * 256;
  link_subpixel_x = (uint8)tmp, link_x_coord = (tmp >> 8);
  tmp = link_subpixel_y + (int8)link_actual_vel_y * 16 + link_y_coord * 256;
  link_subpixel_y = (uint8)tmp, link_y_coord = (tmp >> 8);
  if (link_auxiliary_state) {
    tmp = link_subpixel_z + (int8)link_actual_vel_z * 16 + link_z_coord * 256;
    link_subpixel_z = (uint8)tmp, link_z_coord = (tmp >> 8);
  }

  Link_HandleMovingFloor();
  Link_ApplyConveyor();
  Link_HandleVelocityAndSandDrag(x, y);
}

/*
 * Link_HandleVelocityAndSandDrag — Apply any sand/forced-drag
 * displacement (drag_player_x/y) on top of Link's already-moved
 * coordinates, and re-derive link_y_vel / link_x_vel from the
 * actual delta vs the pre-move snapshot.
 */
void Link_HandleVelocityAndSandDrag(uint16 x, uint16 y) {  // 87e3e0
  link_y_coord += drag_player_y;
  link_x_coord += drag_player_x;
  link_y_vel = link_y_coord - y;
  link_x_vel = link_x_coord - x;
}

/*
 * HandleSwimStrokeAndSubpixels — Swim/slide movement integrator.
 * Decodes the swim-coll state machine (swimcoll_var3/5/7/9) into
 * per-axis velocity using the kSwimming tab tables. The "stroke"
 * effect uses 12 phase values from kSwimmingTab4 to give the
 * paddle a varying push/pull amplitude across the swim animation
 * cycle.
 */
void HandleSwimStrokeAndSubpixels() {  // 87e42a
  link_actual_vel_x = link_actual_vel_y = 0;

  static const int8 kSwimmingTab4[] = { 8, -12, -8, -16, 4, -6, -12, -6, 10, -16, -12, -6 };
  static const uint8 kSwimmingTab5[] = { ~0xc & 0xff, ~3 & 0xff };
  static const uint8 kSwimmingTab6[] = { 8, 4, 2, 1 };
  uint16 S[2];
  for (int i = 1; i >= 0; i--) {
    if ((int16)--swimcoll_var3[i] < 0) {
      swimcoll_var3[i] = 0;
      swimcoll_var5[i] = 1;
    }
    uint16 t = swimcoll_var5[i];
    if (link_flag_moving)
      t += link_flag_moving * 4;

    uint16 sum = swimcoll_var7[i] + kSwimmingTab4[t];
    if ((int16)sum <= 0) {
      link_direction &= kSwimmingTab5[i];
      link_direction_last = link_direction;
      //link_actual_vel_y = link_y_page_movement_delta; // WTF bug?!
      if (swimcoll_var5[i] == 2) {
        swimcoll_var5[i] = 0;
        swimcoll_var9[i] = 240;
        swimcoll_var7[i] = 2;
      } else {
        swimcoll_var5[i] = 0;
        swimcoll_var9[i] = 0;
        swimcoll_var7[i] = 0;
      }
    } else {
      link_direction |= kSwimmingTab6[swimcoll_var11[i] + i * 2];
      if (sum >= swimcoll_var9[i])
        sum = swimcoll_var9[i];
      swimcoll_var7[i] = sum;
    }
    S[i] = swimcoll_var7[i];
    if (link_num_orthogonal_directions | link_moving_against_diag_tile)
      S[i] -= S[i] >> 2;
    if (!swimcoll_var11[i])
      S[i] = -S[i];
  }

  Player_SomethingWithVelocity_TiredOrSwim(S[1], S[0]);

}

/*
 * Player_SomethingWithVelocity_TiredOrSwim — Apply a 16-bit velocity
 * pair (xvel, yvel) to Link's position with sub-pixel precision.
 * Used by the swim/slide integrator. Also derives the 8-bit
 * actual_vel_x/y mirror for the renderer (pose) and runs the
 * moving-floor add when dungeon collision mode 4 is active.
 */
void Player_SomethingWithVelocity_TiredOrSwim(uint16 xvel, uint16 yvel) {  // 87e4d3
  uint16 org_x = link_x_coord, org_y = link_y_coord;
  link_y_coord_safe_return_lo = link_y_coord;
  link_y_coord_safe_return_hi = link_y_coord >> 8;
  link_x_coord_safe_return_lo = link_x_coord;
  link_x_coord_safe_return_hi = link_x_coord >> 8;

  uint8 u;

  uint32 tmp;
  tmp = link_subpixel_x + (int16)xvel + link_x_coord * 256;
  link_subpixel_x = (uint8)tmp, link_x_coord = (tmp >> 8);

  u = xvel >> 8;
  link_actual_vel_x = ((sign8(u) ? -u : u) << 4) | ((uint8)xvel >> 4);

  tmp = link_subpixel_y + (int16)yvel + link_y_coord * 256;
  link_subpixel_y = (uint8)tmp, link_y_coord = (tmp >> 8);
  u = yvel >> 8;
  link_actual_vel_y = ((sign8(u) ? -u : u) << 4) | ((uint8)yvel >> 4);

  if (dung_hdr_collision == 4)
    Link_ApplyMovingFloorVelocity();
  link_x_page_movement_delta = 0;
  link_y_page_movement_delta = 0;
  Link_HandleVelocityAndSandDrag(org_x, org_y);
}

/*
 * Link_HandleMovingFloor — Apply the room's moving-floor velocity
 * (dung_floor_x/y_vel) to Link's position if he's on the ground.
 * Skipped when on a hookshot (you ride the hookshot, not the floor)
 * or when byte_7E0322 bits 0/1 are not both set (no active moving
 * floor tile under Link). Also adjusts link_direction so the walk
 * animation faces the floor's motion.
 */
void Link_HandleMovingFloor() {  // 87e595
  if (!dung_hdr_collision)
    return;
  if (BYTE(link_z_coord) != 0 && BYTE(link_z_coord) != 255)
    return;
  if (((byte_7E0322) & 3) != 3)
    return;
  if (link_player_handler_state == 19) // hookshot
    return;

  if (dung_floor_y_vel)
    link_direction |= sign8(dung_floor_y_vel) ? 8 : 4;

  if (dung_floor_x_vel)
    link_direction |= sign8(dung_floor_x_vel) ? 2 : 1;

  Link_ApplyMovingFloorVelocity();
}

/*
 * Link_ApplyMovingFloorVelocity — Trivial helper: bump Link's coords
 * by the room's floor velocity. Also zeroes the orthogonal-direction
 * count so collision code doesn't double-treat the floor push as
 * player input.
 */
void Link_ApplyMovingFloorVelocity() {  // 87e5cd
  link_num_orthogonal_directions = 0;
  link_y_coord += dung_floor_y_vel;
  link_x_coord += dung_floor_x_vel;
}

/*
 * Link_ApplyConveyor — Apply a 0.5 px/frame conveyor belt push to
 * Link's position (8 sub-pixels at 16-precision = 0.5 px). The
 * direction is encoded in link_on_conveyor_belt (1..4 = N/S/W/E).
 * Skipped while: hookshotted, airborne, grabbing a wall, or
 * dashing-against-belt (the dash overrides the conveyor).
 */
void Link_ApplyConveyor() {  // 87e5f0
  static const uint8 kMovePosDirFlag[4] = { 8, 4, 2, 1 };
  static const int8 kMovingBeltY[4] = { -8, 8, 0, 0 };
  static const int8 kMovingBeltX[4] = { 0, 0, -8, 8 };

  if (!link_on_conveyor_belt)
    return;
  if (BYTE(link_z_coord) != 0 && BYTE(link_z_coord) != 0xff)
    return;
  if (link_grabbing_wall & 1 || link_player_handler_state == kPlayerState_Hookshot || link_auxiliary_state)
    return;

  int j = link_on_conveyor_belt - 1;
  if (link_is_running && link_dash_ctr == 32 && (link_direction & kMovePosDirFlag[j]))
    return;

  link_num_orthogonal_directions = 0;
  link_direction |= kMovePosDirFlag[j];

  uint32 t = link_y_coord << 8 | dung_some_subpixel[0];
  t += kMovingBeltY[j] << 4;
  dung_some_subpixel[0] = t;
  link_y_coord = t >> 8;

  t = link_x_coord << 8 | dung_some_subpixel[1];
  t += kMovingBeltX[j] << 4;
  dung_some_subpixel[1] = t;
  link_x_coord = t >> 8;
}

/*
 * Link_HandleMovingAnimation_FullLongEntry — Resolve which direction
 * Link should be facing and dispatch to the appropriate per-frame
 * walk-animation tick. The kTab[4] one-hot encoding maps facing to
 * the direction bit. Doorway-clipping forces facing along the
 * doorway axis (`is_standing_in_doorway * 2 & ~3`).
 */
void Link_HandleMovingAnimation_FullLongEntry() {  // 87e6a6
  if (link_player_handler_state == 4) {
    Link_HandleMovingAnimationSwimming();
    return;
  }

  static const uint8 kTab[4] = { 8, 4, 2, 1 };

  uint8 r0 = link_direction_last;
  uint8 y;
  if (r0 == 0)
    return;
  if (link_flag_moving)
    r0 = link_some_direction_bits;
  if (link_cant_change_direction)
    goto bail;
  if (link_num_orthogonal_directions == 0)
    goto not_diag;
  if (is_standing_in_doorway) {
    y = (is_standing_in_doorway * 2) & ~3;
  } else {
    if (r0 & kTab[link_direction_facing >> 1])
      goto bail;
not_diag:
    y = (r0 & 0xc) ? 0 : 4;
  }

  if (y != 4) {
    y += (r0 & 4) ? 2 : 0;
  } else {
    y += (r0 & 1) ? 2 : 0;
  }
  link_direction_facing = y;
bail:
  Link_HandleMovingAnimation_StartWithDash();
}

/*
 * Link_HandleMovingAnimation_StartWithDash — Step Link's per-direction
 * walk animation, with branches for dashing, slow walk (speed
 * 6 = stairs/door), and forced-motion. The tab2 / tab3 tables encode
 * per-direction (and per-substate) animation step durations.
 *
 * Special-case bunny states (23/28) animate 4-frame hop cycle. The
 * MiscBugFixes branch adds state 28 to the bunny check (original
 * forgot it, causing temp-bunny to use the wrong animation rate).
 *
 * Submodules 18/19 (stair-walking) get a tuned animation table.
 */
void Link_HandleMovingAnimation_StartWithDash() {  // 87e704
  if (link_is_running) {
    Link_HandleMovingAnimation_Dash();
    return;
  }

  uint8 x = link_direction_facing >> 1;
  if (link_speed_setting == 6) {
    x += 4;
  } else if (link_flag_moving) {
    if (!(joypad1H_last & kJoypadH_AnyDir)) {
      link_animation_steps = 0;
      return;
    }
    x += 4;
  }

  static const uint8 tab2[16] = { 4, 4, 4, 4, 1, 1, 1, 1, 2, 2, 2, 2, 8, 8, 8, 8 };
  static const uint8 tab3[24] = { 1, 2, 3, 2, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 2, 1, 2, 2, 3, 2, 2, 2, 3, 2 };

//bugfix: tempbunny animation steps are wrong due to missing check
  if (link_player_handler_state == 23 || (enhanced_features0 & kFeatures0_MiscBugFixes && link_player_handler_state == 28)) {  // bunny states
    if (link_animation_steps < 4 && player_on_somaria_platform != 2) {
      if (++link_counter_var1 >= tab2[x]) {
        link_counter_var1 = 0;
        if (++link_animation_steps == 4)
          link_animation_steps = 0;
      }
    } else {
      link_animation_steps = 0;
    }
    return;
  }

  if (submodule_index == 18 || submodule_index == 19) {
    x = 12;
  } else if (submodule_index != kPlayerState_JumpOffLedgeDiag && (link_state_bits & 0x80) == 0) {
    if (bitmask_of_dragstate & 0x8d) {
      x = 12;
    } else if (!draw_water_ripples_or_grass && !button_b_frames) {
      // else_6
      x = link_animation_steps;
      if (link_speed_setting == 6)
        x += 8;
      if (link_flag_moving)
        x += 8;
      if (player_on_somaria_platform == 2)
        return;
      if (++link_counter_var1 >= tab3[x]) {
        link_counter_var1 = 0;
        if (++link_animation_steps == 9)
          link_animation_steps = 1;
      }
      return;
    }
  }
  // endif_4

  if (link_animation_steps < 6 && player_on_somaria_platform != 2) {
    if (++link_counter_var1 >= tab2[x]) {
      link_counter_var1 = 0;
      if (++link_animation_steps == 6)
        link_animation_steps = 0;
    }
  } else {
    link_animation_steps = 0;
  }
}

/*
 * Link_HandleMovingAnimationSwimming — Pick the swim-pose direction
 * from link_some_direction_bits. Same axis-resolution shape as
 * Link_HandleMovingAnimation_FullLongEntry but uses the swim
 * direction latch (which doesn't change with the d-pad) instead of
 * the live walking direction.
 */
void Link_HandleMovingAnimationSwimming() {  // 87e7fa
  static const uint8 kTab[4] = { 8, 4, 2, 1 };
  if (!link_some_direction_bits || link_cant_change_direction)
    return;
  uint8 y;

  if (link_num_orthogonal_directions) {
    if (is_standing_in_doorway) {
      y = (is_standing_in_doorway * 2) & ~3;
    } else {
      if (link_some_direction_bits & kTab[link_direction_facing >> 1])
        return;
      y = link_some_direction_bits & 0xC ? 0 : 4;
    }
  } else {
    y = link_some_direction_bits & 0xC ? 0 : 4;
  }
  if (y != 4) {
    y += (link_some_direction_bits & 4) ? 2 : 0;
  } else {
    y += (link_some_direction_bits & 1) ? 2 : 0;
  }
  link_direction_facing = y;
}

/*
 * Link_HandleMovingAnimation_Dash — Dash-specific animation tick.
 * kDashTab3 maps the dash-countdown phases (29 down to 4 frames left)
 * to a 0..6 stride index; kDashTab4 then picks the per-frame
 * duration for the 8-frame dash cycle at each stride (slower frames
 * at the start, faster as the dash builds up). Water/grass overrides
 * use the simpler kDashTab5 single-axis table.
 */
void Link_HandleMovingAnimation_Dash() {  // 87e88f
  static const uint8 kDashTab3[] = { 48, 36, 24, 16, 12, 8, 4 };
  static const uint8 kDashTab4[] = { 3, 3, 5, 3, 3, 3, 5, 3, 2, 2, 4, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  static const uint8 kDashTab5[] = { 1, 2, 2, 2, 2, 2, 2 };

  uint8 t = 6;
  while (link_countdown_for_dash >= kDashTab3[t] && t)
    t--;

  if (button_b_frames < 9 && !draw_water_ripples_or_grass) {
    if (++link_counter_var1 >= kDashTab4[t * 8]) {
      link_counter_var1 = 0;
      link_animation_steps++;
      if (link_animation_steps == 9)
        link_animation_steps = 1;
    }
  } else {
    if (++link_counter_var1 >= kDashTab5[t]) {
      link_counter_var1 = 0;
      link_animation_steps++;
      if (link_animation_steps >= 6)
        link_animation_steps = 0;
    }
  }
}

/*
 * HandleIndoorCameraAndDoors — Indoor-only: route to either the
 * door-transition driver (when in a doorway) or the standard camera
 * scroll. Outdoors uses a completely different camera (see
 * Overworld_OperateCameraScroll).
 */
void HandleIndoorCameraAndDoors() {  // 87e8f0
  if (player_is_indoors) {
    if (is_standing_in_doorway)
      HandleDoorTransitions();
    else
      ApplyLinksMovementToCamera();
  }
}

/*
 * HandleDoorTransitions — Detect Link crossing a room-edge while
 * standing in a doorway. The magic constants 28 / 18 (Y axis) and
 * 21 / 8 (X axis) are the per-direction trigger thresholds; the
 * `& 0xfc` masks confirm Link has actually crossed into the next
 * page. On a confirmed cross, kicks off the inter-room transition
 * (Dung_StartInterRoomTrans_*).
 *
 * Author note retained — the MiscBugFixes early-return prevents a
 * stale module switch after using a potion in a doorway.
 */
void HandleDoorTransitions() {  // 87e901
  uint16 t;

  link_x_page_movement_delta = 0;
  link_y_page_movement_delta = 0;

  // Using a potion might have changed us into a different module, and the routines
  // below just increment the submodule value, causing all kinds of havoc.
  // There's an added return to catch the same behavior a bit up, but this one catches more cases,
  // at the expense of link already having done his movement, so by returning here we might
  // miss handling the door causing other kinds of issues.
  if ((enhanced_features0 & kFeatures0_MiscBugFixes) && !(main_module_index == 7 && submodule_index == 0))
    return;

  if (link_direction_last & 0xC && is_standing_in_doorway == 1) {
    if (link_direction_last & 4) {
      if (((t = link_y_coord + 28) & 0xfc) == 0)
        link_y_page_movement_delta = (t >> 8) - link_y_coord_safe_return_hi;
    } else {
      t = link_y_coord - 18;
      link_y_page_movement_delta = (t >> 8) - link_y_coord_safe_return_hi;
    }
  }

  if (link_direction_last & 3 && is_standing_in_doorway == 2) {
    if (link_direction_last & 1) {
      if (((t = link_x_coord + 21) & 0xfc) == 0)
        link_x_page_movement_delta = (t >> 8) - link_x_coord_safe_return_hi;
    } else {
      t = link_x_coord - 8;
      link_x_page_movement_delta = (t >> 8) - link_x_coord_safe_return_hi;
    }
  }

  if (link_x_page_movement_delta) {
    some_animation_timer = 0;
    link_state_bits = 0;
    link_picking_throw_state = 0;
    link_grabbing_wall = 0;
    if (sign8(link_x_page_movement_delta))
      Dung_StartInterRoomTrans_Left_Plus();
    else
      HandleEdgeTransitionMovementEast_RightBy8();
  } else if (link_y_page_movement_delta) {
    some_animation_timer = 0;
    link_state_bits = 0;
    link_picking_throw_state = 0;
    link_grabbing_wall = 0;
    if (sign8(link_y_page_movement_delta))
      Dungeon_StartInterRoomTrans_Up();
    else
      HandleEdgeTransitionMovementSouth_DownBy16();
  }
}

/*
 * ApplyLinksMovementToCamera — Indoor camera-follow. When Link has
 * crossed a 256-pixel page boundary (link_*_page_movement_delta is
 * non-zero), kicks off the appropriate AdjustQuadrantAndCamera_*
 * scroll routine. Author note retained — the global
 * g_ApplyLinksMovementToCamera_called flag prevents the bug-fix
 * gate in PlayerHandler_00_Ground_3 from re-entering this on the
 * same frame.
 */
void ApplyLinksMovementToCamera() {  // 87e9d3
  // Sometimes, when using spin attack, this routine will end up getting
  // called twice in the same frame, which messes up things.
  g_ApplyLinksMovementToCamera_called = true;

  link_y_page_movement_delta = (link_y_coord >> 8) - link_y_coord_safe_return_hi;
  link_x_page_movement_delta = (link_x_coord >> 8) - link_x_coord_safe_return_hi;

  if (link_x_page_movement_delta) {
    if (sign8(link_x_page_movement_delta))
      AdjustQuadrantAndCamera_left();
    else
      AdjustQuadrantAndCamera_right();
  }

  if (link_y_page_movement_delta) {
    if (sign8(link_y_page_movement_delta))
      AdjustQuadrantAndCamera_up();
    else
      AdjustQuadrantAndCamera_down();
  }
}

/*
 * FindFreeMovingBlockSlot — Allocate one of the two dungeon push-
 * block slots. Slot 1 is preferred (allocated first); slot 0 is
 * the secondary. Returns 0xff if both are in use. The +1 in the
 * stored value is so that slot 0 with no block uses the value 0.
 */
uint8 FindFreeMovingBlockSlot(uint8 x) {  // 87ed2c
  if (index_of_changable_dungeon_objs[1] == 0) {
    index_of_changable_dungeon_objs[1] = x + 1;
    return 1;
  }
  if (index_of_changable_dungeon_objs[0] == 0) {
    index_of_changable_dungeon_objs[0] = x + 1;
    return 0;
  }
  return 0xff;
}

/*
 * InitializePushBlock — Seed a push-block slot from the room's
 * dung_object_tilemap_pos entry. The packed `pos` field is decoded
 * (7 bits X / 6 bits Y) and translated to world coords by adding
 * the loader's BG offset. Then tests pushability against the
 * destination tile; returns false (failed) if blocked.
 *
 * dung_hdr_tag[0] == 38 is the dungeon-special "always-pushable"
 * tag that bypasses the destination check.
 */
bool InitializePushBlock(uint8 r14, uint8 idx) {  // 87ed3f
  uint16 pos = dung_object_tilemap_pos[idx >> 1];
  uint16 x = (pos & 0x007e) << 2;
  uint16 y = (pos & 0x1f80) >> 4;

  x += (dung_loade_bgoffs_h_copy & 0xff00);
  y += (dung_loade_bgoffs_v_copy & 0xff00);

  pushedblocks_x_lo[r14] = (uint8)x;
  pushedblocks_x_hi[r14] = (uint8)(x >> 8);
  pushedblocks_y_lo[r14] = (uint8)y;
  pushedblocks_y_hi[r14] = (uint8)(y >> 8);
  pushedblocks_target[r14] = 0;
  pushedblocks_subpixel[r14] = 0;

  if (dung_hdr_tag[0] != 38 && dung_replacement_tile_state[idx >> 1] == 0) {
    if (!PushBlock_AttemptToPushTheBlock(0, x, y)) {
      Ancilla_Sfx2_Near(0x22);
      dung_replacement_tile_state[idx >> 1] = 1;
      return false;
    }
  }

  index_of_changable_dungeon_objs[r14] = 0;
  return true;
}

/*
 * Sprite_Dungeon_DrawSinglePushBlock — Per-frame OAM render for one
 * push-block. Allocates 4 OAM slots (the block is a 2x2 sprite),
 * computes screen coords, and writes the tile via kPushedblock_Char.
 * pushedblocks_some_index drives the squash animation frame; 0xff
 * means "don't draw this frame" (mid-collapse).
 */
void Sprite_Dungeon_DrawSinglePushBlock(int j) {  // 87f0d9
  static const uint8 kPushedBlock_Tab1[9] = { 0, 1, 2, 3, 4, 0, 0, 0, 0 };
  static const uint8 kPushedblock_Char[4] = { 0xc, 0xc, 0xc, 0xff };
  j >>= 1;
  Oam_AllocateFromRegionB(4);
  OamEnt *oam = GetOamCurPtr();
  int y = (uint8)pushedblocks_y_lo[j] | (uint8)pushedblocks_y_hi[j] << 8;
  int x = (uint8)pushedblocks_x_lo[j] | (uint8)pushedblocks_x_hi[j] << 8;
  y -= BG2VOFS_copy2 + 1;
  x -= BG2HOFS_copy2;
  uint8 ch = kPushedblock_Char[kPushedBlock_Tab1[pushedblocks_some_index]];
  if (ch != 0xff)
    SetOamPlain(oam, x, y, ch, 0x20, 2);
}

/*
 * Link_Initialize — One-time Link state reset at game start / room
 * load / death-revive. Clears every "currently doing X" flag and
 * facing/animation/aux fields to known defaults. The
 * MiscBugFixes block patches several state-leak bugs noted inline
 * (jump-off-ledge, conveyor momentum, ice momentum, etc.). Existing
 * author comments retained.
 */
void Link_Initialize() {  // 87f13c
  link_direction_facing = 2;
  link_direction_last = 0;
  link_item_in_hand = 0;
  link_position_mode = 0;
  link_debug_value_1 = 0;
  link_debug_value_2 = 0;
  link_var30d = 0;
  link_var30e = 0;
  some_animation_timer_steps = 0;
  link_is_transforming = 0;
  bitfield_for_a_button = 0;
  button_mask_b_y &= ~0x40;
  link_state_bits = 0;
  link_picking_throw_state = 0;
  link_grabbing_wall = 0;
  Link_ResetSwimmingState();
  link_cant_change_direction &= ~1;
  link_z_coord &= 0xff;
  link_auxiliary_state = 0;
  link_incapacitated_timer = 0;
  countdown_for_blink = 0;
  link_electrocute_on_touch = 0;
  link_pose_for_item = 0;
  link_cape_mode = 0;
  Link_ForceUnequipCape_quietly();
  Link_ResetSwordAndItemUsage();
  link_disable_sprite_damage = 0;
  player_handler_timer = 0;
  link_direction &= ~0xf;
  player_on_somaria_platform = 0;
  link_spin_attack_step_counter = 0;

  if (enhanced_features0 & kFeatures0_MiscBugFixes) {
    // If you quit while jumping from a ledge and get hit on a platform you can go under solid layers
    about_to_jump_off_ledge = 0;
 
    // If you use the mirror near a moveable statue you can pull thin air and glitch the camera
    link_is_near_moveable_statue = 0;

    // If you use the mirror on a conveyor belt you will retain momentum and clip into the entrance wall
    link_on_conveyor_belt = 0;
    
    // bugfix: you use the mirror on ice, you retain momentum
    link_flag_moving = 0;

    // If you quit in the middle of red armos knight stomp the lumberjack tree will fall on its own
    bg1_y_offset = bg1_x_offset = 0;
      //bugfix: if you die in a dungeon as a permabunny and continue, you revert back to link
      if (!link_item_moon_pearl && savegame_is_darkworld) {
        link_player_handler_state = kPlayerState_PermaBunny;
        link_is_bunny = 1;
        link_is_bunny_mirror = 1;
        LoadGearPalettes_bunny();
      }
  }
}

/*
 * Link_ResetProperties_A — Tier-1 state reset used when entering a
 * new room or recovering from death. Clears direction/animation,
 * bunny state, follower-related flags, and chains into _B.
 */
void Link_ResetProperties_A() {  // 87f1a3
  link_direction_last = 0;
  link_direction = 0;
  link_flag_moving = 0;
  Link_ResetSwimmingState();
  link_is_transforming = 0;
  countdown_for_blink = 0;
  ancilla_arr24[0] = 0;
  link_is_bunny = 0;
  link_is_bunny_mirror = 0;
  BYTE(link_timer_tempbunny) = 0;
  link_need_for_poof_for_transform = 0;
  is_archer_or_shovel_game = 0;
  link_need_for_pullforrupees_sprite = 0;
  BYTE(bit9_of_xcoord) = 0;
  link_something_with_hookshot = 0;
  link_give_damage = 0;
  link_spin_offsets = 0;
  tagalong_event_flags = 0;
  link_want_make_noise_when_dashed = 0;
  BYTE(tiledetect_tile_type) = 0;
  item_receipt_method = 0;
  link_triggered_by_whirlpool_sprite = 0;
  Link_ResetProperties_B();
}

/*
 * Link_ResetProperties_B — Tier-2 state reset: pit-fall / somaria /
 * drag bits. Chains into _C.
 */
void Link_ResetProperties_B() {  // 87f1e6
  player_on_somaria_platform = 0;
  link_spin_attack_step_counter = 0;
  fallhole_var1 = 0;
  flag_is_sprite_to_pick_up_cached = 0;
  bitmask_of_dragstate = 0;
  link_this_controls_sprite_oam = 0;
  player_near_pit_state = 0;
  Link_ResetProperties_C();
}

/*
 * Link_ResetProperties_C — Tier-3 state reset: all per-action bits
 * (B/Y buttons, sword swing, cape, hookshot, lift, etc.). The deepest
 * reset; clears everything except direction/facing.
 * Existing bug-fix comment retained.
 */
void Link_ResetProperties_C() {  // 87f1fa
  if (enhanced_features0 & kFeatures0_MiscBugFixes) {
    // Fix save menu lockout when dying after medallion cast (#126)
    flag_custom_spell_anim_active = 0;
  }

  tile_action_index = 0;
  state_for_spin_attack = 0;
  step_counter_for_spin_attack = 0;
  tile_coll_flag = 0;
  link_force_hold_sword_up = 0;
  link_sword_delay_timer = 0;
  tiledetect_misc_tiles = 0;
  link_item_in_hand = 0;
  link_position_mode = 0;
  link_debug_value_1 = 0;
  link_debug_value_2 = 0;
  link_var30d = 0;
  link_var30e = 0;
  some_animation_timer_steps = 0;
  bitfield_for_a_button = 0;
  button_mask_b_y = 0;
  button_b_frames = 0;
  link_state_bits = 0;
  link_picking_throw_state = 0;
  link_grabbing_wall = 0;
  link_cant_change_direction = 0;
  link_auxiliary_state = 0;
  link_incapacitated_timer = 0;
  link_electrocute_on_touch = 0;
  link_pose_for_item = 0;
  link_cape_mode = 0;
  Link_ResetSwordAndItemUsage();
  link_disable_sprite_damage = 0;
  player_handler_timer = 0;
  related_to_hookshot = 0;
  flag_is_ancilla_to_pick_up = 0;
  flag_is_sprite_to_pick_up = 0;
  link_need_for_pullforrupees_sprite = 0;
  link_is_near_moveable_statue = 0;
}

/*
 * Link_CheckForEdgeScreenTransition — Returns false when Link is in
 * a state that can't trigger a screen-edge transition (spin-attack,
 * medallion casts, no recoil active). When permitted, zeros his
 * velocity, sets up a 3-frame recoil window, and rolls his position
 * back to the previous frame so the scroll engine has clean state
 * for the transition.
 */
bool Link_CheckForEdgeScreenTransition() {  // 87f439
  uint8 st = link_player_handler_state;
  if (st == 3 || st == 8 || st == 9 || st == 10 || !link_incapacitated_timer)
    return false;
  link_actual_vel_x = link_actual_vel_y = 0;
  link_recoilmode_timer = 3;
  link_x_coord = link_x_coord_prev;
  link_y_coord = link_y_coord_prev;
  return true;
}

/*
 * CacheCameraPropertiesIfOutdoors — Outdoor-only wrapper for
 * CacheCameraProperties. Used by handler states that need the
 * camera snapshot but only when outdoors (indoors uses the
 * room-load pipeline instead).
 */
void CacheCameraPropertiesIfOutdoors() {  // 87f514
  if (!player_is_indoors)
    CacheCameraProperties();
}

/*
 * SomariaBlock_HandlePlayerInteraction — Multi-purpose handler for
 * Link interacting with an active Somaria block (ancilla type 0x2C).
 * Two phases:
 *   Phase 1 (ancilla_H[k] == 0): "pushing" — drives the block in
 *     the player's d-pad direction once collision aligns. Sets up
 *     dragstate bits so the push-block visual updates. Dash-into-
 *     block flips into phase 2.
 *   Phase 2 (ancilla_H[k] > 0): "block bouncing toward Link's
 *     facing" — the Somaria block hops away from Link in 3 bounces
 *     with diminishing Z velocity (kSomarianBlock_Zvel) until it
 *     lands and re-enters phase 1.
 *
 * The (ancilla_K[k] | ancilla_L[k]) gate suppresses pushing while
 * the block is mid-bounce-collision with a sprite.
 */
void SomariaBlock_HandlePlayerInteraction(int k) {  // 88e7e6
  cur_object_index = k;
  if (ancilla_G[k])
    return;

  if (!ancilla_H[k]) {
    if (link_auxiliary_state || (link_state_bits & 1) || ancilla_z[k] != 0 && ancilla_z[k] != 0xff || ancilla_K[k] || ancilla_L[k])
      return;
    if (!(joypad1H_last & kJoypadH_AnyDir)) {
      ancilla_arr3[k] = 0;
      bitmask_of_dragstate = 0;
      ancilla_A[k] = 255;
      if (!link_is_running) {
        link_speed_setting = 0;
        return;
      }
    } else if ((joypad1H_last & kJoypadH_AnyDir) == ancilla_arr3[k]) {
      if (link_speed_setting == 18)
        bitmask_of_dragstate |= 0x81;
    } else {
      ancilla_arr3[k] = (joypad1H_last & kJoypadH_AnyDir);
      link_speed_setting = 0;
    }

    CheckPlayerCollOut coll_out;
    if (!Ancilla_CheckLinkCollision(k, 4, &coll_out) || ancilla_floor[k] != link_is_on_lower_level)
      return;

    if (!link_is_running || link_dash_ctr == 64) {
      uint8 t;
      ancilla_x_vel[k] = 0;
      ancilla_y_vel[k] = 0;
      ancilla_arr3[k] = t = joypad1H_last & kJoypadH_AnyDir;
      if (t & 3) {
        ancilla_x_vel[k] = t & 1 ? 16 : -16;
        ancilla_dir[k] = t & 1 ? 3 : 2;
      } else {
        ancilla_y_vel[k] = t & 8 ? -16 : 16;
        ancilla_dir[k] = t & 8 ? 0 : 1;
      }
      if (link_actual_vel_y == 0 || link_actual_vel_x == 0) {
        if (!Ancilla_CheckTileCollision_Class2(k)) {
          Ancilla_MoveY(k);
          Ancilla_MoveX(k);
          if (!(link_state_bits & 0x80) && !(++ancilla_A[k] & 7))
            Ancilla_Sfx2_Pan(k, 0x22);
        }
        bitmask_of_dragstate = 0x81;
        link_speed_setting = 0x12;
      }
      Sprite_NullifyHookshotDrag();
      return;
    }
    static const int8 kSomarianBlock_Yvel[4] = { -40, 40, 0, 0 };
    static const int8 kSomarianBlock_Xvel[4] = { 0, 0, -40, 40 };
    if (flag_is_ancilla_to_pick_up == k + 1)
      flag_is_ancilla_to_pick_up = 0;
    Link_CancelDash();
    Ancilla_Sfx3_Pan(k, 0x32);
    int j = link_direction_facing >> 1;
    ancilla_dir[k] = j;
    ancilla_y_vel[k] = kSomarianBlock_Yvel[j];
    ancilla_x_vel[k] = kSomarianBlock_Xvel[j];
    ancilla_z_vel[k] = 48;
    ancilla_H[k] = 1;
    ancilla_z[k] = 0;
  }

  ancilla_z_vel[k] -= 2;
  Ancilla_MoveY(k);
  Ancilla_MoveX(k);
  Ancilla_MoveZ(k);
  if (ancilla_z[k] && ancilla_z[k] < 252)
    return;

  Ancilla_Sfx2_Pan(k, 0x21);
  ancilla_z[k] = 0;
  int j = ancilla_H[k]++;
  if (j == 3) {
    ancilla_arr4[k] = 0;
    ancilla_H[k] = 0;
  } else {
    static const int8 kSomarianBlock_Zvel[4] = { 48, 24, 16, 8 };
    ancilla_z_vel[k] = kSomarianBlock_Zvel[j - 1];
    ancilla_y_vel[k] = (int8)ancilla_y_vel[k] / 2;
    ancilla_x_vel[k] = (int8)ancilla_x_vel[k] / 2;
  }
}

/*
 * Gravestone_Move — Per-frame "pulling a gravestone" hookshot tick.
 * Drags the grave north at 8 px/frame, runs Gravestone_ActAsBarrier
 * for the body-block effect, and when the grave reaches its target
 * Y (ancilla_A/B) finalizes the reveal: clears ancilla, kicks off
 * the 32x32 door update with the specific timing (0x48/0x60/0x40
 * frames depending on the grave's reveal type) so the hole opens.
 */
void Gravestone_Move(int k) {  // 88ed89
  if (submodule_index)
    return;
  ancilla_y_vel[k] = -8;
  Ancilla_MoveY(k);

  Gravestone_ActAsBarrier(k);
  uint16 y_target = ancilla_B[k] << 8 | ancilla_A[k];
  uint16 y_cur = Ancilla_GetY(k);

  if (y_cur >= y_target)
    return;

  ancilla_type[k] = 0;
  link_something_with_hookshot = 0;
  bitmask_of_dragstate &= ~4;
  BYTE(scratch_0) = ((uint8 *)door_debris_y)[k];
  HIBYTE(scratch_0) = ((uint8 *)door_debris_x)[k];
  big_rock_starting_address = scratch_0;

  door_open_closed_counter = big_rock_starting_address == 0x532 ? 0x48 :
    big_rock_starting_address == 0x488 ? 0x60 : 0x40;
  Overworld_DoMapUpdate32x32_B();
}

/*
 * Gravestone_ActAsBarrier — Test Link's bounding box against the
 * gravestone's 0x20x0x18 rect. When overlap, push Link south by
 * the overlap distance (so he can't clip through), set the
 * dragstate "blocked by gravestone" bit, and clear his "facing up"
 * direction bit so he can't keep pulling once the grave reaches him.
 */
void Gravestone_ActAsBarrier(int k) {  // 88ee57
  uint16 x = Ancilla_GetX(k);
  uint16 y = Ancilla_GetY(k);
  uint16 r4 = y + 0x18;
  uint16 r6 = x + 0x20;
  uint16 lx = link_x_coord + 8;
  uint16 ly = link_y_coord + 8;
  if (ly >= y && ly < r4 &&
      lx >= x && lx < r6) {
    uint16 r10 = abs16(ly - r4);
    link_y_coord += r10;
    link_y_vel += r10;
    bitmask_of_dragstate |= 4;
  }
  if (link_direction_facing)
    link_direction_facing &= ~4;
}

/*
 * AncillaAdd_DugUpFlute — Spawn the "dug-up flute" ancilla after
 * the shovel hits the correct spot. Tosses east or west based on
 * Link's facing direction with a small upward arc (z-velocity 24).
 * Hardcoded landing coordinate (0x490, 0xa8a) puts the flute on
 * the screen patch near Link's house.
 */
void AncillaAdd_DugUpFlute(uint8 a, uint8 y) {  // 898c73
  int k = Ancilla_AddAncilla(a, y);
  if (k < 0)
    return;
  ancilla_step[k] = 0;
  ancilla_z[k] = 0;
  ancilla_z_vel[k] = 24;
  ancilla_x_vel[k] = link_direction_facing == 4 ? -8 : 8;
  DecodeAnimatedSpriteTile_variable(12);
  Ancilla_SetXY(k, 0x490, 0xa8a);
}

/*
 * AncillaAdd_CaneOfByrnaInitSpark — Spawn the Byrna spark-ring
 * ancilla and clear any prior charged-sword ancilla (type 0x31).
 * Sets the 9-frame "wind up" timer and the spark-orbits-Link mode.
 */
void AncillaAdd_CaneOfByrnaInitSpark(uint8 a, uint8 y) {  // 898ee0
  for (int k = 4; k >= 0; k--) {
    if (ancilla_type[k] == 0x31)
      ancilla_type[k] = 0;
  }
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_aux_timer[k] = 9;
    link_disable_sprite_damage = 1;
    ancilla_arr3[k] = 2;
  }
}

/*
 * AncillaAdd_ShovelDirt — Spawn the dirt-cloud ancilla after a
 * successful dig. 20-frame self-destruct; positioned at Link's coords.
 */
void AncillaAdd_ShovelDirt(uint8 a, uint8 y) {  // 898f5b
  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_item_to_link[k] = 0;
    ancilla_timer[k] = 20;
    Ancilla_SetXY(k, link_x_coord, link_y_coord);
  }
}

/*
 * AncillaAdd_Hookshot — Fire the hookshot. Three offset tables:
 *   _Yvel/Xvel: 64 px/frame in the facing direction.
 *   _Yd/Xd:    spawn-position offset relative to Link's center (so
 *              the chain emerges from Link's hand, not his center).
 *
 * ancilla_G[k] = 255 is the sentinel meaning "still extending"; the
 * ancilla handler sets it to a fixed value once the chain has hit
 * something (then drags Link toward the impact point).
 */
void AncillaAdd_Hookshot(uint8 a, uint8 y) {  // 899b10
  static const int8 kHookshot_Yvel[4] = { -64, 64, 0, 0 };
  static const int8 kHookshot_Xvel[4] = { 0, 0, -64, 64 };
  static const int8 kHookshot_Yd[4] = { 4, 20, 8, 8 };
  static const int8 kHookshot_Xd[4] = { 0, 0, -4, 11 };

  int k = Ancilla_AddAncilla(a, y);
  if (k >= 0) {
    ancilla_aux_timer[k] = 3;
    ancilla_item_to_link[k] = 0;
    ancilla_step[k] = 0;
    ancilla_L[k] = 0;
    related_to_hookshot = 0;
    hookshot_effect_index = k;
    ancilla_K[k] = 0;
    ancilla_G[k] = 255;
    ancilla_arr1[k] = 0;
    ancilla_timer[k] = 0;
    int j = link_direction_facing >> 1;
    ancilla_dir[k] = j;
    ancilla_x_vel[k] = kHookshot_Xvel[j];
    ancilla_y_vel[k] = kHookshot_Yvel[j];
    Ancilla_SetXY(k, link_x_coord + kHookshot_Xd[j], link_y_coord + kHookshot_Yd[j]);
  }
}

/*
 * ResetSomeThingsAfterDeath — Post-death cleanup called by the
 * game-over -> revive path. Clears every "transient gameplay" flag
 * (swim, conveyor, immobilize, color swap, etc.), sets the player's
 * speed to `a` (caller picks: 0 = stopped, or a small value for
 * fairy revive's "stagger forward" effect), and chains into
 * Link_ResetProperties_A for the deeper reset.
 */
void ResetSomeThingsAfterDeath(uint8 a) {  // 8bffbf
  link_is_in_deep_water = 0;
  link_speed_setting = a;
  link_on_conveyor_belt = 0;
  byte_7E0322 = 0;
  flag_is_link_immobilized = 0;
  palette_swap_flag = 0;
  player_unk1 = 0;
  link_give_damage = 0;
  link_actual_vel_y = 0;
  link_actual_vel_x = 0;
  link_actual_vel_z = 0;
  BYTE(link_z_coord) = 0;
  draw_water_ripples_or_grass = 0;
  byte_7E0316 = 0;
  countdown_for_blink = 0;
  link_player_handler_state = 0;
  link_visibility_status = 0;
  Ancilla_TerminateSelectInteractives(0);
  Link_ResetProperties_A();
}

/*
 * SpawnHammerWaterSplash — When the hammer strikes a water tile
 * (attribute 8 or 9), spawn the splash sprite at the strike point.
 * Probe coords are offset from Link's center by the
 * kItem_Hammer_SpawnWater_X/Y tables per facing direction.
 * Indoors uses dung_bg2_attr_table, outdoors uses
 * Overworld_ReadTileAttribute.
 */
void SpawnHammerWaterSplash() {  // 9aff3c
  static const int8 kItem_Hammer_SpawnWater_X[4] = { 0, 12, -8, 24 };
  static const int8 kItem_Hammer_SpawnWater_Y[4] = { 8, 32, 24, 24 };
  if (submodule_index | flag_is_link_immobilized | flag_unk1)
    return;
  int i = link_direction_facing >> 1;
  uint16 x = link_x_coord + kItem_Hammer_SpawnWater_X[i];
  uint16 y = link_y_coord + kItem_Hammer_SpawnWater_Y[i];
  uint8 tiletype;
  if (player_is_indoors) {
    int t = (link_is_on_lower_level >= 1) ? 0x1000 : 0;
    t += (x & 0x1f8) >> 3;
    t += (y & 0x1f8) << 3;
    tiletype = dung_bg2_attr_table[t];
  } else {
    tiletype = Overworld_ReadTileAttribute(x >> 3, y);
  }

  if (tiletype == 8 || tiletype == 9) {
    int j = Sprite_SpawnSmallSplash(0);
    if (j >= 0) {
      Sprite_SetX(j, x - 8);
      Sprite_SetY(j, y - 16);
      sprite_floor[j] = link_is_on_lower_level;
      sprite_z[j] = 0;
    }
  }
}

/*
 * DiggingGameGuy_AttemptPrizeSpawn — Roll a random prize when the
 * player digs in the digging-game mini-game.
 *
 * 8 outcomes from GetRandomNumber & 7:
 *   0..3: standard kDiggingGameGuy_Items prizes (rupees, hearts).
 *   4:    after 25+ digs AND no big prize spawned yet AND 1-in-4
 *         random success: the "piece of heart" big prize (0xeb).
 *   5..7: no prize.
 *
 * beamos_x_hi[1] is repurposed as the dig counter; beamos_x_hi[0]
 * is the "big prize already spawned" lock. Author note retained
 * ("zelda bug: 4 wtf...") about the questionable spawner-index 4.
 */
void DiggingGameGuy_AttemptPrizeSpawn() {  // 9dfd5c
  static const int8 kDiggingGameGuy_Xvel[2] = { -16, 16 };
  static const int8 kDiggingGameGuy_X[2] = { 0, 19 };
  static const uint8 kDiggingGameGuy_Items[4] = { 0xdb, 0xda, 0xd9, 0xdf };

  beamos_x_hi[1]++;
  if (link_y_coord >= 0xb18)
    return;
  int j = GetRandomNumber() & 7;
  uint8 item_to_spawn;
  switch (j) {
  case 0: case 1: case 2: case 3:
    item_to_spawn = kDiggingGameGuy_Items[j];
    break;
  case 4:
    if (beamos_x_hi[1] < 25 || beamos_x_hi[0] || GetRandomNumber() & 3)
      return;
    item_to_spawn = beamos_x_hi[0] = 0xeb;
    break;
  default:
    return;
  }
  SpriteSpawnInfo info;
  j = Sprite_SpawnDynamically(4, item_to_spawn, &info); // zelda bug: 4 wtf...
  if (j >= 0) {
    int i = link_direction_facing != 4;
    sprite_x_vel[j] = kDiggingGameGuy_Xvel[i];
    sprite_y_vel[j] = 0;
    sprite_z_vel[j] = 24;
    sprite_stunned[j] = 255;
    sprite_delay_aux4[j] = 48;
    Sprite_SetX(j, (link_x_coord + kDiggingGameGuy_X[i]) & ~0xf);
    Sprite_SetY(j, (link_y_coord + 22) & ~0xf);
    sprite_floor[j] = 0;
    SpriteSfx_QueueSfx3WithPan(j, 0x30);
  }
}

