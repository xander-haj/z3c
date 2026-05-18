/*
 * tagalong.h - Follower/companion NPC subsystem ("Tagalongs")
 *
 * Manages the NPCs that follow Link around the world after being rescued
 * or recruited. In A Link to the Past, several characters trail behind Link
 * as part of the story progression:
 *   - Zelda (escorting her from the castle dungeon to the Sanctuary)
 *   - The Old Man (guiding him through Death Mountain)
 *   - The Blacksmith's Partner (carrying him home from the Dark World)
 *   - The Kidnapped Maiden (leading her to the crystal pedestal, which
 *     triggers the Blind boss fight in Thieves' Town)
 *   - Kiki the Monkey (follows Link to the Dark Palace entrance)
 *   - The Sick Kid's Bug Net recipient, the Frog Blacksmith, etc.
 *
 * The follower system uses a queue of Link's past positions to create a
 * trailing motion effect (the tagalong walks to where Link was N frames ago).
 * Only one follower can be active at a time. The follower interacts with the
 * world via trigger zones that fire context-specific dialogue messages.
 */
#pragma once
#include "types.h"

/*
 * TagalongMessageInfo - Defines a location-triggered dialogue for a follower
 *
 * When the follower reaches a specific (x, y) position on the map, the game
 * checks the |bit| flag to see if this message has already been shown. If not,
 * it displays message |msg| and records the trigger. |tagalong| identifies
 * which follower type this trigger applies to (different NPCs have different
 * trigger points on the same maps).
 */
typedef struct TagalongMessageInfo {
  uint16 y, x, bit, msg, tagalong;
} TagalongMessageInfo;

/* ---------------------------------------------------------------------------
 * Follower state queries
 * --------------------------------------------------------------------------- */

// Returns true if any tagalong NPC is currently following Link.
bool Tagalong_IsFollowing();

// Returns true if the game state allows the follower to display a dialogue
// message (no other message is active, no screen transition in progress).
bool Follower_ValidateMessageFreedom();

// Returns true if Link is close enough to the follower for proximity-based
// events (entering doors together, triggering rescue sequences).
bool Follower_CheckProximityToLink();

/* ---------------------------------------------------------------------------
 * Follower lifecycle
 * --------------------------------------------------------------------------- */

// Sets up the follower's initial position, sprite, and state when a new
// tagalong sequence begins (e.g., Zelda joins Link in the castle dungeon).
void Follower_Initialize();

// Converts sprite slot |k| from a regular sprite into the active follower.
// The sprite is removed from the sprite table and its state is transferred
// to the follower subsystem's dedicated variables.
void Sprite_BecomeFollower(int k);

// Removes the active follower, clearing all tagalong state. Called when
// the follower reaches their destination or is dismissed by a game event.
void Follower_Disable();

/* ---------------------------------------------------------------------------
 * Follower per-frame update
 * --------------------------------------------------------------------------- */

// Main update function called once per frame when a follower is active.
// Dispatches to the appropriate type-specific handler based on the current
// follower identity.
void Follower_Main();

// Checks the current game module to determine whether the follower should
// be visible and active (followers are hidden during screen transitions,
// menus, and certain cutscenes).
void Follower_CheckGameMode();

// Moves the follower toward Link's position using the position history queue.
// The follower walks to where Link stood several frames ago, creating the
// characteristic trailing movement.
void Follower_MoveTowardsLink();

// Basic movement handler that updates the follower's position and animation
// without any type-specific behavior. Used as the default mover for most NPCs.
void Follower_BasicMover();

// Update handler for when no follower is currently active -- exits immediately.
void Follower_NotFollowing();

// Clears the timed message state, preventing a queued follower dialogue
// from displaying. Used during screen transitions and boss fights.
void Follower_NoTimedMessage();

/* ---------------------------------------------------------------------------
 * Type-specific follower handlers
 * --------------------------------------------------------------------------- */

// Update handler for the Old Man on Death Mountain. Implements his unique
// behavior: he only follows Link on the mountain and refuses to leave.
void Follower_OldMan();

// Unused variant of the Old Man handler preserved from the original ROM.
void Follower_OldManUnused();

/* ---------------------------------------------------------------------------
 * Rendering and layer management
 * --------------------------------------------------------------------------- */

// Determines whether the follower should render on BG1 or BG2 layer based
// on the current room's floor configuration (overlapping dungeon floors).
void Follower_DoLayers();

// Draws the follower's sprite to the OAM buffer using the current animation
// frame, direction, and screen position.
void Tagalong_Draw();

// Updates the follower's walk animation frame and position. |ain| is the
// animation state index, (|xin|, |yin|) is the target position. This
// function preserves register state as the original 65C816 code did.
void Follower_AnimateMovement_preserved(uint8 ain, uint16 xin, uint16 yin);

/* ---------------------------------------------------------------------------
 * Trigger system
 * --------------------------------------------------------------------------- */

// Checks whether the follower has entered a trigger zone and fires the
// associated dialogue or event if so.
void Follower_HandleTrigger();

// Tests a specific trigger defined by |info| against the follower's current
// position and state. Returns true if the trigger condition is met and the
// message has not been shown before.
bool Follower_CheckForTrigger(const TagalongMessageInfo *info);

/* ---------------------------------------------------------------------------
 * Blind and Kiki special behaviors
 *
 * Two followers have unique transformations: the Maiden in Thieves' Town is
 * actually Blind the Thief in disguise, and Kiki the monkey can revert to a
 * regular sprite after opening the Dark Palace door.
 * --------------------------------------------------------------------------- */

// Returns true if the Maiden follower has reached Blind's boss room trigger,
// which causes the Maiden to transform into the Blind boss fight.
bool Follower_CheckBlindTrigger();

// Spawns the Blind boss sprite at position (|x|, |y|) and removes the
// Maiden follower. This is the Thieves' Town boss reveal moment.
void Blind_SpawnFromMaiden(uint16 x, uint16 y);

// Converts Kiki from the follower subsystem back into a regular sprite in
// slot |k|. Called after Kiki opens the Dark Palace door and his escort is done.
void Kiki_RevertToSprite(int k);

// Spawn handler for Kiki that creates the monkey sprite in slot |k|.
// Returns the sprite slot index used, or -1 if no slot was available.
int Kiki_SpawnHandlerMonke(int k);

// Kiki spawn handler variant A -- positions Kiki for the first encounter.
void Kiki_SpawnHandler_A(int k);

// Kiki spawn handler variant B -- positions Kiki for the palace door event.
void Kiki_SpawnHandler_B(int k);
