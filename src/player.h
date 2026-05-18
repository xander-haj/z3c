/*
 * player.h - Link's state machine, item usage, combat, movement, and
 *            collision declarations.
 *
 * This header defines the complete player character system:
 *
 *   State Machine: Link has 31 distinct states (ground, swimming, dashing,
 *   recoiling, spin attacking, using medallions, hookshot, mirror, etc.).
 *   The state determines which handler runs each frame. States are not
 *   strictly sequential -- many transitions are bidirectional.
 *
 *   Items: Each Y-button item (rod, hammer, bow, boomerang, bombs, lamp,
 *   powder, shovel, flute, book, ether, bombos, quake, mirror, hookshot,
 *   cape, canes, net) has a dedicated LinkItem_* activation function.
 *
 *   Combat: Sword swings, spin attacks, dash attacks, and item-based
 *   attacks are processed through collision checks against sprites and
 *   tiles. The sword cooldown timer prevents attack spamming.
 *
 *   Movement: Separate X and Y collision systems handle indoor/outdoor
 *   terrain, slopes, ice physics, conveyor belts, moving floors, ledge
 *   hops, water entry/exit, and push block detection.
 *
 *   Camera: Indoor camera follows Link with door transition detection;
 *   outdoor camera is handled by the overworld module.
 *
 * Related files: player.c (implementation), player_oam.h (sprite drawing),
 *                tile_detect.h (tile collision), ancilla.h (projectiles),
 *                sprite.h (enemy interaction), variables.h (WRAM state)
 */
#pragma once
#include "types.h"

// Swimming acceleration lookup tables indexed by direction
extern const uint8 kSwimmingTab1[4];
// Swimming velocity cap values indexed by flippers upgrade level
extern const uint8 kSwimmingTab2[2];

/*
 * Player state machine values. Each state corresponds to a handler
 * function that runs every frame while Link is in that state.
 * Note: values 11, 13, 15, 16, 24 are unused/skipped in the original ROM.
 * State transitions occur through direct assignment to the player state
 * variable, not through a formal transition table.
 */
enum {
  kPlayerState_Ground = 0,              // Normal ground movement and interaction
  kPlayerState_FallingIntoHole = 1,     // Falling into a pit (floor crumble, bomb hole)
  kPlayerState_RecoilWall = 2,          // Bouncing back after hitting a wall during dash
  kPlayerState_SpinAttacking = 3,       // Executing a charged spin attack
  kPlayerState_Swimming = 4,            // Swimming in deep water (requires flippers)
  kPlayerState_TurtleRock = 5,          // Special state inside Turtle Rock dungeon
  kPlayerState_RecoilOther = 6,         // Knockback from enemy damage or bomb blast
  kPlayerState_Electrocution = 7,       // Zapped by electric trap (Agahnim lightning)
  kPlayerState_Ether = 8,              // Casting the Ether medallion spell
  kPlayerState_Bombos = 9,             // Casting the Bombos medallion spell
  kPlayerState_Quake = 10,             // Casting the Quake medallion spell
  kPlayerState_FallOfLeftRightLedge = 12, // Hopping off a left/right facing ledge
  kPlayerState_JumpOffLedgeDiag = 14,   // Diagonal ledge jump (southeast/southwest)
  kPlayerState_StartDash = 17,          // Pegasus boots dash windup (charging)
  kPlayerState_StopDash = 18,           // Decelerating after a dash ends
  kPlayerState_Hookshot = 19,           // Hookshot is extending or retracting
  kPlayerState_Mirror = 20,             // Using the Magic Mirror (world warp)
  kPlayerState_HoldUpItem = 21,         // Holding a received item above head
  kPlayerState_AsleepInBed = 22,        // Sleeping in bed (game intro sequence)
  kPlayerState_PermaBunny = 23,         // Permanent bunny form (no Moon Pearl in DW)
  kPlayerState_ReceivingEther = 25,     // Cutscene: receiving Ether from tablet
  kPlayerState_ReceivingBombos = 26,    // Cutscene: receiving Bombos from tablet
  kPlayerState_OpeningDesertPalace = 27, // Cutscene: Book of Mudora opens Desert Palace
  kPlayerState_TempBunny = 28,          // Temporary bunny (hit by Anti-Fairy)
  kPlayerState_PullForRupees = 29,      // Pulling a rupee-dispensing object (trees, signs)
  kPlayerState_SpinAttackMotion = 30,   // Active spin attack rotation animation
};






// -----------------------------------------------------------------------
// Core Player Update and State Machine Entry Points
// -----------------------------------------------------------------------

// Checks if Link has crossed a layer boundary in a dungeon and swaps layers
void Dungeon_HandleLayerChange();
// Saves the current camera scroll position to the cache for later comparison
void CacheCameraProperties();
// Checks whether Link has flippers; initiates drowning if he enters deep water
void CheckAbilityToSwim();
// Master entry point called every frame to update Link's state and position
void Link_Main();
// Reads controller input and dispatches to the current state handler
void Link_ControlHandler();
// Default state handler (state 0): processes ground movement and interactions
void LinkState_Default();
// Handles Link when coming from state 0x1D (post-transition recovery)
void HandleLink_From1D();
// Ground state phase 3: processes movement, collision, and animation
void PlayerHandler_00_Ground_3();
// -----------------------------------------------------------------------
// Bunny Transformation and Special State Handlers
// -----------------------------------------------------------------------

// Checks if Link should transform to/from bunny; returns true if transformed
bool Link_HandleBunnyTransformation();
// Handler for temporary bunny state (Anti-Fairy curse, wears off over time)
void LinkState_TemporaryBunny();
// Handler for permanent bunny form (no Moon Pearl in Dark World)
void PlayerHandler_17_Bunny();
// Re-caches camera properties after bunny state changes affect position
void LinkState_Bunny_recache();
// Secondary handler for temporary bunny state transitions
void Link_TempBunny_Func2();
// Handler when Link is carrying a big rock (Dark World titan's mitt rocks)
void LinkState_HoldingBigRock();

// -----------------------------------------------------------------------
// Tablet Cutscene Handlers
// These handle the cutscenes when Link reads the Ether/Bombos/Desert
// tablets with the Book of Mudora and the Master Sword.
// -----------------------------------------------------------------------

// Initiates the Ether tablet reading cutscene on Death Mountain
void EtherTablet_StartCutscene();
// State handler during the Ether receiving animation
void LinkState_ReceivingEther();
// Initiates the Bombos tablet reading cutscene in the desert
void BombosTablet_StartCutscene();
// State handler during the Bombos receiving animation
void LinkState_ReceivingBombos();
// State handler for reading the Desert Palace entrance tablet
void LinkState_ReadingDesertTablet();
// Processes Cane of Somaria block and gravestone interactions
void HandleSomariaAndGraves();
// -----------------------------------------------------------------------
// Recoil, Ice, and Ledge Hop Handlers
// Recoil pushes Link backward from damage or wall collisions. Ice
// physics override normal acceleration with reduced friction. Ledge
// hops are parabolic arcs with tile detection for landing.
// -----------------------------------------------------------------------

// State handler for recoil from damage or wall collision
void LinkState_Recoil();
// Manages recoil velocity decay and timer; skips initial frames if flagged
void Link_HandleRecoilAndTimer(bool jump_into_middle);
// State handler for sliding on ice floors (Ice Palace, frozen rooms)
void LinkState_OnIce();
// Updates Link's vertical (Z) velocity for gravity during jumps/falls
void Link_HandleChangeInZVelocity();
// Applies a Z-axis displacement of zd units (upward hop or fall)
void Player_ChangeZ(uint8 zd);
// Handles the southward ledge hop on the overworld
void LinkHop_HoppingSouthOW();
// State handler for the jump arc during a ledge hop
void LinkState_HandlingJump();
// Scans for a valid landing tile below Link during a southward hop
void LinkHop_FindTileToLandOnSouth();
// State handler for horizontal (east/west) ledge hops on the overworld
void LinkState_HoppingHorizontallyOW();
// Finds the Y-coordinate landing tile during a horizontal hop
void Link_HoppingHorizontally_FindTile_Y();
// Transitions Link to the deep water state (triggers swimming or drowning)
void Link_SetToDeepWater();
// State handler 0x0F: miscellaneous intermediate hop/transition state
void LinkState_0F();
// Finds the X-coordinate landing tile during a horizontal hop; returns tile
uint8 Link_HoppingHorizontally_FindTile_X(uint8 o);
// State handler for diagonal upward ledge hops (northwest/northeast)
void LinkState_HoppingDiagonallyUpOW();
// State handler for diagonal downward ledge hops (southwest/southeast)
void LinkState_HoppingDiagonallyDownOW();
// Scans for a valid landing spot during a diagonal downward hop
void LinkHop_FindLandingSpotDiagonallyDown();
// Creates a water splash effect when Link lands from a hop into shallow water
void Link_SplashUponLanding();
// -----------------------------------------------------------------------
// Pegasus Boots Dash System
// The dash charges up, then propels Link forward at high speed. Colliding
// with a wall or sprite ends the dash. Trees and certain tiles can be
// bonked to shake out items.
// -----------------------------------------------------------------------

// State handler for the active dash (moving at dash speed)
void LinkState_Dashing();
// State handler for the dash exit (decelerating back to walk speed)
void LinkState_ExitingDash();
// Immediately cancels the dash state and returns to ground
void Link_CancelDash();
// Repels Link backward after dashing into a wall or solid object
void RepelDash();
// Applies the tile-based rebound vector after a bonk collision
void LinkApplyTileRebound();
// Repels Link after dashing into a sprite (enemy or destructible)
void Sprite_RepelDash();
// Sets directional flags at address $67 based on Link's facing direction
void Flag67WithDirections();
// -----------------------------------------------------------------------
// Pit Falling and Swimming
// -----------------------------------------------------------------------

// State handler for falling into a dungeon pit
void LinkState_Pits();
// Determines which BG layer Link should land on after a pit fall
void HandleLayerOfDestination();
// Applies damage to Link from falling into a damaging pit
void DungeonPitDoDamage();
// Handles landing animation and position correction after a pit fall
void HandleDungeonLandingFromPit();
// State handler for swimming (state 4); processes swim movement and stamina
void PlayerHandler_04_Swimming();
// Processes swim direction and velocity based on D-pad input
void Link_HandleSwimMovements();
// Sets the maximum acceleration flags based on current terrain and state
void Link_FlagMaxAccels();
// Sets reduced maximum acceleration for ice surface movement
void Link_SetIceMaxAccel();
// Sets Link's directional momentum based on input and current velocity
void Link_SetMomentum();
// Resets swimming state back to ground when Link exits water
void Link_ResetSwimmingState();
// Resets player state after taking damage from a pit (restores position)
void Link_ResetStateAfterDamagingPit();
// Zeroes all X and Y acceleration and velocity values
void ResetAllAcceleration();
// Applies swim acceleration based on button presses and current speed
void Link_HandleSwimAccels();
// Sets the maximum acceleration value for the current movement context
void Link_SetTheMaxAccel();
// State handler for electrocution (state 7); plays zap animation and damage
void LinkState_Zapped();
// -----------------------------------------------------------------------
// Item Receiving and Special Scenes
// -----------------------------------------------------------------------

// State handler for holding an item above Link's head (item get animation)
void PlayerHandler_15_HoldItem();
// Gives Link an item and triggers the hold-up animation and fanfare
// item: item ID, chest_position: screen position of the chest (or -1)
void Link_ReceiveItem(uint8 item, int chest_position);
// Animates Link getting into bed (game intro sequence)
void Link_TuckIntoBed();
// State handler for Link sleeping in bed (waiting for wake-up trigger)
void LinkState_Sleeping();
// -----------------------------------------------------------------------
// Combat - Sword and Item Activation
// The B button swings the sword; holding B charges a spin attack.
// The Y button uses the equipped secondary item. The A button performs
// contextual actions (lift, read, open, dash, pull).
// -----------------------------------------------------------------------

// Decrements the sword cooldown timer; allows next swing when it reaches 0
void Link_HandleSwordCooldown();
// Checks Y-button press and dispatches to the equipped item's handler
void Link_HandleYItem();
// Checks A-button press and dispatches to the appropriate context action
void Link_HandleAPress();
// Performs the A-press action identified by action_x2 (lift, pull, read, etc.)
void Link_APress_PerformBasic(uint8 action_x2);
// Plays the sword swing sound and spawns a sword beam if at full health
void HandleSwordSfxAndBeam();
// Checks if the B button is pressed/held and initiates a sword swing
void Link_CheckForSwordSwing();
// Processes sword button input including charge-up for spin attack
void HandleSwordControls();
// Resets sword swing state and item usage flags to idle
void Link_ResetSwordAndItemUsage();
// Handles the visual jerk/shake during spin attack charge hold
void Player_Sword_SpinAttackJerks_HoldDown();
// -----------------------------------------------------------------------
// Y-Button Item Handlers
// Each function activates when the player presses Y with that item
// equipped. They spawn the appropriate ancilla (projectile/effect),
// consume magic/ammo, and set Link into the item-use animation state.
// -----------------------------------------------------------------------

// Fire Rod / Ice Rod: spawns a magic projectile in Link's facing direction
void LinkItem_Rod();
// Hammer: ground-pound attack that stuns enemies and drives pegs
void LinkItem_Hammer();
// Bow: fires an arrow (consumes 1 arrow; silver arrows if upgraded)
void LinkItem_Bow();
// Boomerang: throws the boomerang in Link's facing direction
void LinkItem_Boomerang();
// Resets boomerang tracking state after it returns to Link
void Link_ResetBoomerangYStuff();
// Bombs: places a bomb at Link's feet (consumes 1 bomb)
void LinkItem_Bombs();
// Bottle: uses the selected bottle contents (fairy, potion, bee, etc.)
void LinkItem_Bottle();
// Lamp: lights a torch or creates a fire in front of Link (costs magic)
void LinkItem_Lamp();
// Magic Powder: sprinkles powder on the tile/enemy in front of Link
void LinkItem_Powder();
// Dispatcher for shovel or flute based on which the player currently has
void LinkItem_ShovelAndFlute();
// Shovel: digs at Link's feet (used in digging game and flute recovery)
void LinkItem_Shovel();
// Flute: plays the flute melody (summons the duck for fast travel)
void LinkItem_Flute();
// Book of Mudora: reads ancient Hylian text on tablets and Desert Palace
void LinkItem_Book();
// Ether Medallion: initiates the Ether spell (freezes all enemies on screen)
void LinkItem_Ether();
// State handler during Ether spell casting animation
void LinkState_UsingEther();
// Bombos Medallion: initiates the Bombos spell (fire explosion on screen)
void LinkItem_Bombos();
// State handler during Bombos spell casting animation
void LinkState_UsingBombos();
// Quake Medallion: initiates the Quake spell (earthquake stuns all enemies)
void LinkItem_Quake();
// State handler during Quake spell casting animation
void LinkState_UsingQuake();
// Triggers the spin attack after the charge timer completes
void Link_ActivateSpinAttack();
// Plays the victory spin animation (used after defeating certain bosses)
void Link_AnimateVictorySpin();
// State handler for the active spin attack rotation
void LinkState_SpinAttack();
// Magic Mirror: initiates the world-warp effect
void LinkItem_Mirror();
// Checks if the sword interacts with mirror-specific tiles
void DoSwordInteractionWithTiles_Mirror();
// State handler during the mirror warp transition between worlds
void LinkState_CrossingWorlds();
// Performs the Desert Palace prayer cutscene (Book of Mudora at entrance)
void Link_PerformDesertPrayer();
// Repositions tagalong followers after a mirror warp completes
void HandleFollowersAfterMirroring();
// Hookshot: fires the hookshot in Link's facing direction
void LinkItem_Hookshot();
// State handler while the hookshot is extending or retracting
void LinkState_Hookshotting();
// Magic Cape: toggles Link's invisibility (drains magic while active)
void LinkItem_Cape();
// Forces the cape off and restores Link's visibility (magic depleted)
void Link_ForceUnequipCape();
// Forces the cape off without playing the deactivation sound effect
void Link_ForceUnequipCape_quietly();
// Freezes Link's movement while an item-use animation is playing
void HaltLinkWhenUsingItems();
// Checks if Link can lift objects while the cape is passively active
void Link_HandleCape_passive_LiftCheck();
// Per-frame cape handler: drains magic and manages invisibility state
void Player_CheckHandleCapeStuff();
// Cane of Somaria: creates or destroys a pushable magic block
void LinkItem_CaneOfSomaria();
// Cane of Byrna: creates a protective spark ring (drains magic)
void LinkItem_CaneOfByrna();
// Scans active ancillae for an existing Byrna spark; returns true if found
bool SearchForByrnaSpark();
// Bug-catching Net: swings the net to catch fairies and bees
void LinkItem_Net();
// Returns true if the Y button was pressed this frame
bool CheckYButtonPress();
// Checks if Link has enough magic for cost index x; returns true if paid
bool LinkCheckMagicCost(uint8 x);
// Refunds the magic cost at index x (used when an item activation fails)
void Refund_Magic(uint8 x);
// -----------------------------------------------------------------------
// A-Button Contextual Actions
// The A button's function depends on what Link is facing/standing near:
// lift objects, open chests, read signs, pull levers, drag statues,
// initiate a dash, or grab/throw carried objects.
// -----------------------------------------------------------------------

// Resets item usage state when returning from an overworld interaction
void Link_ItemReset_FromOverworldThings();
// Throws the currently carried object (pot, bush, rock, etc.)
void Link_PerformThrow();
// A-press handler for lifting, carrying, and throwing objects
void Link_APress_LiftCarryThrow();
// A-press handler to begin the Pegasus boots dash
void Link_PerformDash();
// A-press handler to grab a pushable object (statue, block)
void Link_PerformGrab();
// A-press handler to pull a movable object toward Link
void Link_APress_PullObject();
// Performs the per-frame statue dragging movement while A is held
void Link_PerformStatueDrag();
// A-press handler to begin dragging a statue
void Link_APress_StatueDrag();
// Performs the rupee-dispensing pull action on trees and signs
void Link_PerformRupeePull();
// State handler for the tree pull animation (rupees shower out)
void LinkState_TreePull();
// A-press handler to read a sign or book
void Link_PerformRead();
// A-press handler to open a treasure chest
void Link_PerformOpenChest();
// Returns true if a fresh A button press occurred this frame
bool Link_CheckNewAPress();
// Handles the throwing arc for a carried object; returns true when airborne
bool Link_HandleToss();
// -----------------------------------------------------------------------
// Collision Detection and Movement Physics
// The collision system separately processes X and Y axes. Indoor and
// outdoor terrain use different detection logic. Slopes, ice, conveyors,
// and moving floors apply additional velocity modifiers.
// -----------------------------------------------------------------------

// Handles collision resolution when Link moves diagonally
void Link_HandleDiagonalCollision();
// Restricts movement to cardinal directions when in narrow passages
void Player_LimitDirections_Inner();
// Handles collision resolution when Link moves in a cardinal direction
void Link_HandleCardinalCollision();
// Runs slope collision checks processing vertical movement first
void RunSlopeCollisionChecks_VerticalFirst();
// Runs slope collision checks processing horizontal movement first
void RunSlopeCollisionChecks_HorizontalFirst();
// Returns true if the current room uses two BG layers requiring dual checks
bool CheckIfRoomNeedsDoubleLayerCheck();
// Generates velocity values when Link stands on a moving BG layer
void CreateVelocityFromMovingBackground();
// -----------------------------------------------------------------------
// Y-Axis (Vertical) Movement and Collision
// -----------------------------------------------------------------------

// Entry point for Y-axis movement collision checks
void StartMovementCollisionChecks_Y();
// Handles Y-axis collision for indoor (dungeon) environments
void StartMovementCollisionChecks_Y_HandleIndoors();
// Processes push detection and bonk snapping on the Y axis
void HandlePushingBonkingSnaps_Y();
// Handles Y-axis collision for outdoor (overworld) environments
void StartMovementCollisionChecks_Y_HandleOutdoors();
// Runs the ledge hop timer; returns true (carry) when the hop begins
bool RunLedgeHopTimer();
// Triggers a bonk collision effect and checks for smashable tiles
void Link_BonkAndSmash();
// Adds vertical fall velocity component to Link's Y position
void Link_AddInVelocityYFalling();
// Computes the Y-axis snap position for wall alignment
void CalculateSnapScratch_Y();
// Adjusts Link's X movement to align with a door during Y-axis transitions
void ChangeAxisOfPerpendicularDoorMovement_Y();
// Adds the Y velocity component to Link's position (normal movement)
void Link_AddInVelocityY();
// Checks if Link should hop into or out of shallow water on the Y axis
void Link_HopInOrOutOfWater_Y();
// Scans northward for a valid tile to land on after a vertical hop
void Link_FindValidLandingTile_North();
// Scans diagonally northward for a landing tile during diagonal hops
void Link_FindValidLandingTile_DiagonalNorth();
// -----------------------------------------------------------------------
// X-Axis (Horizontal) Movement and Collision
// -----------------------------------------------------------------------

// Entry point for X-axis movement collision checks
void StartMovementCollisionChecks_X();
// Handles X-axis collision for indoor (dungeon) environments
void StartMovementCollisionChecks_X_HandleIndoors();
// Processes push detection and bonk snapping on the X axis
void HandlePushingBonkingSnaps_X();
// Handles X-axis collision for outdoor (overworld) environments
void StartMovementCollisionChecks_X_HandleOutdoors();
// Snaps Link's X position to the nearest tile-aligned coordinate
void SnapOnX();
// Computes the X-axis snap position for wall alignment
void CalculateSnapScratch_X();
// Adjusts Link's Y movement to align with a door during X-axis transitions
int8 ChangeAxisOfPerpendicularDoorMovement_X();
// Checks if Link should hop into or out of shallow water on the X axis
void Link_HopInOrOutOfWater_X();
// Applies a diagonal kickback vector when colliding at an angle
void Link_HandleDiagonalKickback();
// -----------------------------------------------------------------------
// Tile Detection, Push Blocks, and Slope Handling
// -----------------------------------------------------------------------

// Main tile detection dispatcher: checks what tile Link is on/facing
void TileDetect_MainHandler(uint8 item);
// Returns true if Link is on a slosh-capable surface (shallow water/swamp)
bool Link_PermissionForSloshSounds();
// Attempts to push a block at (x,y); returns true if push succeeded
bool PushBlock_AttemptToPushTheBlock(uint8 what, uint16 x, uint16 y);
// Checks if the tile Link is facing is liftable; returns the lift type
uint8 Link_HandleLiftables();
// Nudges Link's position by arg_r0 pixels for wall-alignment correction
void HandleNudging(int8 arg_r0);
// Resolves item-vs-tile interaction at (x,y) and executes the tile behavior
void TileBehavior_HandleItemAndExecute(uint16 x, uint16 y);
// Returns the collision flag for the tile a push block would move onto
uint8 PushBlock_GetTargetTileFlag(uint16 x, uint16 y);
// Sets slope flags when Link moves vertically into a sloped tile
void FlagMovingIntoSlopes_Y();
// Sets slope flags when Link moves horizontally into a sloped tile
void FlagMovingIntoSlopes_X();
// -----------------------------------------------------------------------
// Velocity, Position Updates, and Animation
// -----------------------------------------------------------------------

// Applies recoil velocity decay and checks for recoil end
void Link_HandleRecoiling();
// Inner handler for incapacitated states (stunned, frozen, grabbed)
void Player_HandleIncapacitated_Inner2();
// Applies velocity to Link's position with subpixel accumulation
void Link_HandleVelocity();
// Commits Link's calculated velocity to his world position
void Link_MovePosition();
// Applies velocity with sand/swamp drag reduction at offset (x, y)
void Link_HandleVelocityAndSandDrag(uint16 x, uint16 y);
// Processes swim stroke animation timing and subpixel position updates
void HandleSwimStrokeAndSubpixels();
// Applies velocity with fatigue or swimming speed modifiers
void Player_SomethingWithVelocity_TiredOrSwim(uint16 xvel, uint16 yvel);
// Checks if Link is on a moving floor and applies its velocity
void Link_HandleMovingFloor();
// Applies the moving floor's velocity vector to Link's position
void Link_ApplyMovingFloorVelocity();
// Applies conveyor belt velocity to Link's position
void Link_ApplyConveyor();
// Full entry point for Link's walking animation with direction calculation
void Link_HandleMovingAnimation_FullLongEntry();
// Starts the moving animation from the dash state (faster frame rate)
void Link_HandleMovingAnimation_StartWithDash();
// Handles Link's swimming animation frame cycling
void Link_HandleMovingAnimationSwimming();
// Handles Link's dash animation frame cycling (fastest frame rate)
void Link_HandleMovingAnimation_Dash();
// -----------------------------------------------------------------------
// Indoor Camera and Door Transitions
// -----------------------------------------------------------------------

// Updates the indoor camera position and detects door transitions
void HandleIndoorCameraAndDoors();
// Processes door transition when Link walks through a doorway
void HandleDoorTransitions();
// Translates Link's movement into camera scroll adjustments
void ApplyLinksMovementToCamera();

// -----------------------------------------------------------------------
// Push Block System
// Dungeon rooms can have movable blocks that Link pushes. Each active
// push block occupies a slot and animates its movement independently.
// -----------------------------------------------------------------------

// Finds an unused push block slot; returns the slot index (x if none free)
uint8 FindFreeMovingBlockSlot(uint8 x);
// Creates a push block in slot idx with properties r14; returns true if ok
bool InitializePushBlock(uint8 r14, uint8 idx);
// Draws the sprite for push block j at its current animated position
void Sprite_Dungeon_DrawSinglePushBlock(int j);
// -----------------------------------------------------------------------
// Initialization and Reset
// -----------------------------------------------------------------------

// Initializes Link's state for a new game or after loading a save file
void Link_Initialize();
// Resets Link's core properties (position, state, health display)
void Link_ResetProperties_A();
// Resets Link's animation and visual properties
void Link_ResetProperties_B();
// Resets Link's item usage and interaction flags
void Link_ResetProperties_C();
// Returns true if Link has reached the screen edge (triggers scroll/warp)
bool Link_CheckForEdgeScreenTransition();
// Caches camera properties only when outdoors (skips in dungeons)
void CacheCameraPropertiesIfOutdoors();

// -----------------------------------------------------------------------
// Object Interactions and Ancilla Spawners
// -----------------------------------------------------------------------

// Handles player collision with Somaria block k (pushes or blocks Link)
void SomariaBlock_HandlePlayerInteraction(int k);
// Moves gravestone k in response to Link pushing it
void Gravestone_Move(int k);
// Makes gravestone k act as a solid barrier blocking Link's movement
void Gravestone_ActAsBarrier(int k);
// Spawns the dug-up flute ancilla (item a at slot y) from the grove
void AncillaAdd_DugUpFlute(uint8 a, uint8 y);
// Spawns the initial Byrna spark ring ancilla (item a at slot y)
void AncillaAdd_CaneOfByrnaInitSpark(uint8 a, uint8 y);
// Spawns the shovel dirt particle effect ancilla (item a at slot y)
void AncillaAdd_ShovelDirt(uint8 a, uint8 y);
// Spawns the hookshot chain ancilla (item a at slot y)
void AncillaAdd_Hookshot(uint8 a, uint8 y);
// Resets game state after death (clears certain flags based on context a)
void ResetSomeThingsAfterDeath(uint8 a);
// Creates a water splash effect at the hammer impact point
void SpawnHammerWaterSplash();
// Attempts to spawn a prize item at the digging game dig site
void DiggingGameGuy_AttemptPrizeSpawn();
