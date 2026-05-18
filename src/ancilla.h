/*
 * ancilla.h - Ancilla (Projectile/Effect) System
 *
 * Declares all ancilla types used in A Link to the Past. "Ancilla" is the
 * game's internal term for any non-sprite, non-player object that exists
 * temporarily in the world: arrows, bombs, boomerangs, spell effects,
 * hookshots, item receipts, visual flourishes, and more. The SNES original
 * manages these through parallel arrays indexed by slot number (parameter k).
 *
 * The system supports up to ~10 simultaneous ancillae. Each ancilla type has
 * a dedicated handler (Ancilla01 through Ancilla43) plus draw routines, and
 * they interact with both sprites and the player through collision checks.
 *
 * Related files:
 *   ancilla.c  - Full implementation of all ancilla handlers
 *   sprite.h   - Sprite system (ancillae can damage sprites and vice versa)
 *   player.h   - Player state (many ancillae originate from Link's items)
 */
#pragma once
#include "types.h"
#include "zelda_rtl.h"
#include "sprite.h"

/* Result of checking whether an ancilla overlaps with the player hitbox.
 * Contains bounding box coordinates used for collision resolution. */
typedef struct CheckPlayerCollOut {
  uint16 r4, r6;   // Player hitbox X range (left/right edges)
  uint16 r8, r10;  // Player hitbox Y range (top/bottom edges)
} CheckPlayerCollOut;

/* Per-ancilla OAM positioning data for rendering a single sprite tile.
 * Passed to draw routines to place ancilla graphics on screen. */
typedef struct AncillaOamInfo {
  uint8 x;      // Screen-relative X position for the OAM entry
  uint8 y;      // Screen-relative Y position for the OAM entry
  uint8 flags;  // OAM attribute flags (palette, priority, flip bits)
} AncillaOamInfo;

/* Radial projection coordinates for circular/orbital ancilla effects.
 * Used by Ether blitz segments, spin attack sparkles, and Byrna sparks
 * to compute positions along a circular path around a center point. */
typedef struct AncillaRadialProjection {
  uint8 r0, r2;  // Cosine/sine X components of the radial offset
  uint8 r4, r6;  // Cosine/sine Y components of the radial offset
} AncillaRadialProjection;

// -----------------------------------------------------------------------
// Ancilla Core: Position Access, Allocation, and Lifecycle
// -----------------------------------------------------------------------

uint16 Ancilla_GetX(int k);
uint16 Ancilla_GetY(int k);
void Ancilla_SetX(int k, uint16 x);
void Ancilla_SetY(int k, uint16 y);
// Allocate an ancilla slot from the high-priority pool (slots 0-4)
int Ancilla_AllocHigh();
// Deallocate ancilla slot k, clearing its type to zero
void Ancilla_Empty(int k);
void Ancilla_Unused_14(int k);
void Ancilla_Unused_25(int k);
void SpinSpark_Draw(int k, int offs);
// Check if the Somarian block's OAM entry is offscreen or empty
bool SomarianBlock_CheckEmpty(OamEnt *oam);
void AddDashingDustEx(uint8 a, uint8 y, uint8 flag);
void AddBirdCommon(int k);

// -----------------------------------------------------------------------
// Ancilla Physics: Speed Projection and Collision
// -----------------------------------------------------------------------

// Project bomb velocity vector towards the player at a given speed
ProjectSpeedRet Bomb_ProjectSpeedTowardsPlayer(int k, uint16 x, uint16 y, uint8 vel);
// Nudge the boomerang's return trajectory toward Link for better feel
void Boomerang_CheatWhenNoOnesLooking(int k, ProjectSpeedRet *pt);
// Check if a medallion spell (Ether/Bombos/Quake) hit any sprites
void Medallion_CheckSpriteDamage(int k);
void Ancilla_CheckDamageToSprite(int k, uint8 type);
// Aggressive variant checks damage even against invulnerable sprites
void Ancilla_CheckDamageToSprite_aggressive(int k, uint8 type);
void CallForDuckIndoors();

// -----------------------------------------------------------------------
// Ancilla Sound: Panned Sound Effect Helpers
// -----------------------------------------------------------------------

// Play sound effects on channels 1-3 with spatial panning based on ancilla position
void Ancilla_Sfx1_Pan(int k, uint8 v);
void Ancilla_Sfx2_Pan(int k, uint8 v);
void Ancilla_Sfx3_Pan(int k, uint8 v);

// -----------------------------------------------------------------------
// Ancilla Spawning and Execution
// -----------------------------------------------------------------------

void AncillaAdd_FireRodShot(uint8 type, uint8 y);
void SomariaBlock_SpawnBullets(int k);
// Main entry point: iterates all ancilla slots and dispatches handlers
void Ancilla_Main();
ProjectSpeedRet Ancilla_ProjectReflexiveSpeedOntoSprite(int k, uint16 x, uint16 y, uint8 vel);
void Bomb_CheckSpriteDamage(int k);
// Execute all active ancillae in priority order each frame
void Ancilla_ExecuteAll();
// Dispatch a single ancilla of the given type to its handler
void Ancilla_ExecuteOne(uint8 type, int k);
// -----------------------------------------------------------------------
// Ancilla Type Handlers: Ice Rod and Cane of Somaria
// -----------------------------------------------------------------------

void Ancilla13_IceRodSparkle(int k);
void AncillaAdd_IceRodSparkle(int k);
// Somaria bullet: projectile spawned when the Somarian block detonates
void Ancilla01_SomariaBullet(int k);
// Terminate an ancilla if its OAM position falls outside the visible area
bool Ancilla_ReturnIfOutsideBounds(int k, AncillaOamInfo *info);
void SomarianBlast_Draw(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Fire Rod
// -----------------------------------------------------------------------

void Ancilla02_FireRodShot(int k);
void FireShot_Draw(int k);

// -----------------------------------------------------------------------
// Ancilla Tile Collision Detection
// Multiple variants handle different collision checking strategies:
// staggered checks alternate frames, targeted checks arbitrary coords,
// Class2 checks are for ancillae that interact with the tile grid differently
// -----------------------------------------------------------------------

uint8 Ancilla_CheckTileCollision_staggered(int k);
uint8 Ancilla_CheckTileCollision(int k);
bool Ancilla_CheckTileCollisionOneFloor(int k);
bool Ancilla_CheckTileCollision_targeted(int k, uint16 x, uint16 y);
bool Ancilla_CheckTileCollision_Class2(int k);
bool Ancilla_CheckTileCollision_Class2_Inner(int k);

// -----------------------------------------------------------------------
// Ancilla-Sprite Collision and Spatial Queries
// -----------------------------------------------------------------------

// Sword beam hit effect (type 0x04)
void Ancilla04_BeamHit(int k);
// Returns index of first sprite hit, or -1 if none
int Ancilla_CheckSpriteCollision(int k);
bool Ancilla_CheckSpriteCollision_Single(int k, int j);
void Ancilla_SetupHitBox(int k, SpriteHitBox *hb);
ProjectSpeedRet Ancilla_ProjectSpeedTowardsPlayer(int k, uint8 vel);
// Spatial relationship queries for panning and directional logic
PairU8 Ancilla_IsRightOfLink(int k);
PairU8 Ancilla_IsBelowLink(int k);
// Spawn the weapon-clinking spark when sword hits a hard surface
void Ancilla_WeaponTink();
// Apply velocity to move the ancilla along each axis
void Ancilla_MoveX(int k);
void Ancilla_MoveY(int k);
void Ancilla_MoveZ(int k);
// -----------------------------------------------------------------------
// Ancilla Type Handlers: Boomerang (type 0x05)
// Handles outgoing/returning flight, screen edge wrapping, and termination
// -----------------------------------------------------------------------

void Ancilla05_Boomerang(int k);
bool Boomerang_ScreenEdge(int k);
void Boomerang_StopOffScreen(int k);
void Boomerang_Terminate(int k);
void Boomerang_Draw(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Wall/Sword Hit Effects (types 0x06)
// -----------------------------------------------------------------------

void Ancilla06_WallHit(int k);
void Ancilla_SwordWallHit(int k);
void WallHit_Draw(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Bombs (type 0x07)
// Full bomb lifecycle: placement, carrying, conveyor interaction,
// fuse countdown, explosion with damage to sprites and player
// -----------------------------------------------------------------------

void Ancilla07_Bomb(int k);
void Ancilla_ApplyConveyor(int k);
void Bomb_CheckSpriteAndPlayerDamage(int k);
// Handle bomb lifting, throwing, and carried-position tracking
void Ancilla_HandleLiftLogic(int k);
void Ancilla_LatchAltitudeAboveLink(int k);
void Ancilla_LatchLinkCoordinates(int k, int j);
void Ancilla_LatchCarriedPosition(int k);
uint16 Ancilla_LatchYCoordToZ(int k);
int Bomb_GetDisplacementFromLink(int k);
void Bomb_Draw(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Dungeon Door Debris, Arrows, Ice Rod
// -----------------------------------------------------------------------

// Debris particles when a dungeon door is opened or destroyed (type 0x08)
void Ancilla08_DoorDebris(int k);
void DoorDebris_Draw(int k);
// Arrow projectile in flight (type 0x09) and embedded in a wall (type 0x0A)
void Ancilla09_Arrow(int k);
void Arrow_Draw(int k);
void Ancilla0A_ArrowInTheWall(int k);
// Ice rod projectile and its wall-impact freeze spread (types 0x0B, 0x11)
void Ancilla0B_IceRodShot(int k);
void Ancilla11_IceRodWallHit(int k);
void IceShotSpread_Draw(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Blast Wall, Splashes, Particles
// -----------------------------------------------------------------------

// Bombable wall explosion sequence (type 0x33)
void Ancilla33_BlastWallExplosion(int k);
void AncillaDraw_BlastWallBlast(int k, int x, int y);
OamEnt *AncillaDraw_Explosion(OamEnt *oam, int frame, int idx, int idx_end, uint8 r11, int x, int y);
void Ancilla15_JumpSplash(int k);
// Star-shaped impact particles when striking an object (type 0x16)
void Ancilla16_HitStars(int k);
void Ancilla17_ShovelDirt(int k);
// Fireball that precedes a blast wall explosion (type 0x32)
void Ancilla32_BlastWallFireball(int k);
// -----------------------------------------------------------------------
// Ancilla Type Handlers: Medallion Spells (Ether, Bombos, Quake)
// These are full-screen offensive spells obtained from tablets.
// Each has multi-phase animation: buildup, effect, and damage application.
// -----------------------------------------------------------------------

// Ether (type 0x18): lightning bolts radiate from Link, freezing enemies
void Ancilla18_EtherSpell(int k);
void EtherSpell_HandleLightningStroke(int k);
void EtherSpell_HandleOrbPulse(int k);
void EtherSpell_HandleRadialSpin(int k);
OamEnt *AncillaDraw_EtherBlitzBall(OamEnt *oam, const AncillaRadialProjection *arp, int s);
OamEnt *AncillaDraw_EtherBlitzSegment(OamEnt *oam, const AncillaRadialProjection *arp, int s, int k);
void AncillaDraw_EtherBlitz(int k);
void AncillaDraw_EtherOrb(int k, OamEnt *oam);

// Bombos (type 0x19): fire columns erupt then explode across the screen
void AncillaAdd_BombosSpell(uint8 a, uint8 y);
void Ancilla19_BombosSpell(int k);
void BombosSpell_ControlFireColumns(int k);
void BombosSpell_FinishFireColumns(int kk);
void AncillaDraw_BombosFireColumn(int kk);
void BombosSpell_ControlBlasting(int kk);
void AncillaDraw_BombosBlast(int k);

// Quake (type 0x1C): screen shakes and lightning bolts spread outward
void Ancilla1C_QuakeSpell(int k);
void QuakeSpell_ShakeScreen(int k);
void QuakeSpell_ControlBolts(int k);
void AncillaDraw_QuakeInitialBolts(int k);
void QuakeSpell_SpreadBolts(int k);
// -----------------------------------------------------------------------
// Ancilla Type Handlers: Magic Powder, Screen Effects, Hookshot
// -----------------------------------------------------------------------

// Magic powder dust cloud and its sprite-transforming damage (type 0x1A)
void Ancilla1A_PowderDust(int k);
void Ancilla_MagicPowder_Draw(int k);
void Powder_ApplyDamageToSprites(int k);
// Triggered screen shake effect, e.g. from Quake medallion (type 0x1D)
void Ancilla1D_ScreenShake(int k);
// Dust particles trailing behind Link during a Pegasus Boots dash (type 0x1E)
void Ancilla1E_DashDust(int k);
// Hookshot chain extending/retracting with tile/sprite interaction (type 0x1F)
void Ancilla1F_Hookshot(int k);
// Bed blanket and snoring Z's during the sleeping intro (types 0x20, 0x21)
void Ancilla20_Blanket(int k);
void Ancilla21_Snore(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Sword Sparkles and Item Receipt
// -----------------------------------------------------------------------

void Ancilla3B_SwordUpSparkle(int k);
void Ancilla3C_SpinAttackChargeSparkle(int k);
// Master Sword pedestal retrieval cutscene (type 0x35)
void Ancilla35_MasterSwordReceipt(int k);
// Generic item receipt: Link holds item overhead with fanfare (type 0x22)
void Ancilla22_ItemReceipt(int k);
OamEnt *Ancilla_ReceiveItem_Draw(int k, int x, int y);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Wish/Happiness Pond Items
// -----------------------------------------------------------------------

// Item tossed into a fairy pond for upgrade (type 0x28)
void Ancilla28_WishPondItem(int k);
void WishPondItem_Draw(int k);
// Rupees thrown into the Happiness Pond in the waterfall cave (type 0x42)
void Ancilla42_HappinessPondRupees(int k);
void HapinessPondRupees_ExecuteRupee(int k, int i);
void HapinessPondRupees_GetState(int j, int k);
void HapinessPondRupees_SaveState(int k, int j);
// Convert an item ancilla into a splash effect when it hits water
void Ancilla_TransmuteToSplash(int k);
void Ancilla3D_ItemSplash(int k);
void ObjectSplash_Draw(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Milestone Items, Crystals, Cutscenes
// -----------------------------------------------------------------------

// Major item receipt with extended animation (pendants, crystals) (type 0x29)
void Ancilla29_MilestoneItemReceipt(int k);
void ItemReceipt_TransmuteToRisingCrystal(int k);
void Ancilla_RisingCrystal(int k);
void AncillaAdd_OccasionalSparkle(int k);
// Ganon's Tower crystal convergence cutscene (type 0x43)
void Ancilla43_GanonsTowerCutscene(int k);
void AncillaDraw_GTCutsceneCrystal(OamEnt *oam, int x, int y);
void GTCutscene_ActivateSparkle();
OamEnt *GTCutscene_SparkleALot(OamEnt *oam);
// -----------------------------------------------------------------------
// Ancilla Type Handlers: Flute, Duck Travel, Transformation Poofs
// -----------------------------------------------------------------------

// Flute item activation and the weathervane sequence (types 0x36, 0x37)
void Ancilla36_Flute(int k);
void Ancilla37_WeathervaneExplosion(int k);
void AncillaDraw_WeathervaneExplosionWoodDebris(int k);
// Duck that carries Link to selected overworld locations (type 0x38)
void Ancilla38_CutsceneDuck(int k);
// Poof clouds for Link's bunny/human transformation (type 0x23)
void Ancilla23_LinkPoof(int k);
void MorphPoof_Draw(int k);
void Ancilla40_DwarfPoof(int k);
void Ancilla3F_BushPoof(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Spin Attack Sparkles, Byrna, Sword Beam
// -----------------------------------------------------------------------

void Ancilla26_SwordSwingSparkle(int k);
// Spin attack sparkles orbit Link during the charged attack (types 0x2A, 0x2B)
void Ancilla2A_SpinAttackSparkleA(int k);
void SpinAttackSparkleA_TransmuteToNextSpark(int k);
void Ancilla2B_SpinAttackSparkleB(int k);
Point16U Sparkle_PrepOamFromRadial(AncillaRadialProjection p);
void SpinAttackSparkleB_Closer(int k);
// Cane of Byrna windup spark transitions to the orbiting shield (types 0x30, 0x31)
void Ancilla30_ByrnaWindupSpark(int k);
void ByrnaWindupSpark_TransmuteToNormal(int k);
void Ancilla31_ByrnaSpark(int k);
// Sword beam projectile fired at full health (type 0x0D for spark)
void Ancilla_SwordBeam(int k);
void Ancilla0D_SpinAttackFullChargeSpark(int k);
// Duck fast-travel bird that flies Link across the overworld (type 0x27)
void Ancilla27_Duck(int k);
// -----------------------------------------------------------------------
// Ancilla Type Handlers: Cane of Somaria Block System
// The Somarian block is a placeable object that can ride platforms,
// activate switches, and explode into four directional bullets.
// -----------------------------------------------------------------------

int AncillaAdd_SomariaBlock(uint8 type, uint8 y);
void SomariaBlock_CheckForTransitTile(int k);
int Ancilla_CheckBasicSpriteCollision(int k);
bool Ancilla_CheckBasicSpriteCollision_Single(int k, int j);
void Ancilla_SetupBasicHitBox(int k, SpriteHitBox *hb);
void Ancilla2C_SomariaBlock(int k);
void AncillaDraw_SomariaBlock(int k);
bool SomariaBlock_CheckForSwitch(int k);
void SomariaBlock_FizzleAway(int k);
void Ancilla2D_SomariaBlockFizz(int k);
void Ancilla39_SomariaPlatformPoof(int k);
// Block splits into four directional bullets (type 0x2E)
void Ancilla2E_SomariaBlockFission(int k);

// -----------------------------------------------------------------------
// Ancilla Type Handlers: Lamp Flame, Environmental Effects, Explosions
// -----------------------------------------------------------------------

// Lamp flame that lights torches in dark rooms (type 0x2F)
void Ancilla2F_LampFlame(int k);
// Waterfall splash particle effect (type 0x41)
void Ancilla41_WaterfallSplash(int k);
// Gravestone push interaction in the graveyard (type 0x24)
void Ancilla24_Gravestone(int k);
// Fire left behind by Fire Rod in Skull Woods dungeon (type 0x34)
void Ancilla34_SkullWoodsFire(int k);
// Super Bomb explosion that can open the Pyramid of Power (type 0x3A)
void Ancilla3A_BigBombExplosion(int k);
// -----------------------------------------------------------------------
// Revival Fairy and Game Over
// The revival fairy activates from a bottled fairy when Link dies,
// restoring hearts and playing the resurrection animation.
// -----------------------------------------------------------------------

void RevivalFairy_Main();
void RevivalFairy_Dust();
void RevivalFairy_MonitorHP();
void GameOverText_Draw();

// -----------------------------------------------------------------------
// Ancilla OAM (Sprite Tile) Management and Rendering Helpers
// -----------------------------------------------------------------------

// Allocate and spawn an ancilla using Bank 08 logic
int AncillaAdd_AddAncilla_Bank08(uint8 type, uint8 y);
// Prepare screen coordinates for rendering an ancilla's OAM entries
void Ancilla_PrepOamCoord(int k, Point16U *info);
void Ancilla_PrepAdjustedOamCoord(int k, Point16U *info);
// Check if ancilla k overlaps Link, writing collision bounds to out
bool Ancilla_CheckLinkCollision(int k, int j, CheckPlayerCollOut *out);
bool Hookshot_CheckProximityToLink(int x, int y);
// Check if the ancilla's position matches a dungeon entrance trigger
bool Ancilla_CheckForEntranceTrigger(int what);
// Draw a circular shadow beneath an airborne ancilla
void AncillaDraw_Shadow(OamEnt *oam, int k, int x, int y, uint8 pal);
// OAM region allocators partition the 128-entry OAM buffer into priority zones
void Ancilla_AllocateOamFromRegion_B_or_E(uint8 size);
OamEnt *Ancilla_AllocateOamFromCustomRegion(OamEnt *oam);
OamEnt *HitStars_UpdateOamBufferPosition(OamEnt *oam);
bool Hookshot_ShouldIEvenBotherWithTiles(int k);
// Compute X/Y offsets for circular motion given angle and radius
AncillaRadialProjection Ancilla_GetRadialProjection(uint8 a, uint8 r8);
int Ancilla_AllocateOamFromRegion_A_or_D_or_F(int k, uint8 size);

// -----------------------------------------------------------------------
// Ancilla Add Functions: Spawn Various Ancilla Types
// Each AncillaAdd function finds a free slot, initializes the ancilla
// state, and returns the slot index (or -1 on failure).
// Parameter 'a' is typically the ancilla type or direction,
// 'y' encodes a secondary parameter like item subtype or priority.
// -----------------------------------------------------------------------

void Ancilla_AddHitStars(uint8 a, uint8 y);
void AncillaAdd_Blanket(uint8 a);
void AncillaAdd_Snoring(uint8 a, uint8 y);
void AncillaAdd_Bomb(uint8 a, uint8 y);
uint8 AncillaAdd_Boomerang(uint8 a, uint8 y);
void AncillaAdd_TossedPondItem(uint8 a, uint8 xin, uint8 yin);
void AddHappinessPondRupees(uint8 arg);
// Spawn a prize item that falls from a defeated enemy or pot
int AncillaAdd_FallingPrize(uint8 a, uint8 item_idx, uint8 yv);
void AncillaAdd_ChargedSpinAttackSparkle();
void AncillaAdd_ExplodingWeatherVane(uint8 a, uint8 y);
void AncillaAdd_CutsceneDuck(uint8 a, uint8 y);
void AncillaAdd_SomariaPlatformPoof(int k);
// Super Bomb explosion: large blast radius that opens Pyramid crack
int AncillaAdd_SuperBombExplosion(uint8 a, uint8 y);
// Set up the fairy and dust ancillae for the death/revival sequence
void ConfigureRevivalAncillae();
void AncillaAdd_LampFlame(uint8 a, uint8 y);
void AncillaAdd_MSCutscene(uint8 a, uint8 y);
void AncillaAdd_DashDust(uint8 a, uint8 y);
void AncillaAdd_DashDust_charging(uint8 a, uint8 y);
void AncillaAdd_BlastWallFireball(uint8 a, uint8 y, int r4);
// Spawn an arrow with explicit coordinates (used by enemy archers too)
int AncillaAdd_Arrow(uint8 a, uint8 ax, uint8 ay, uint16 xcoord, uint16 ycoord);
void AncillaAdd_BunnyPoof(uint8 a, uint8 y);
void AncillaAdd_CapePoof(uint8 a, uint8 y);
void AncillaAdd_DwarfPoof(uint8 ain, uint8 yin);
void AncillaAdd_BushPoof(uint16 x, uint16 y);
void AncillaAdd_EtherSpell(uint8 a, uint8 y);
void AncillaAdd_VictorySpin();
void AncillaAdd_MagicPowder(uint8 a, uint8 y);
void AncillaAdd_WallTapSpark(uint8 a, uint8 y);
void AncillaAdd_SwordSwingSparkle(uint8 a, uint8 y);
void AncillaAdd_DashTremor(uint8 a, uint8 y);
void AncillaAdd_BoomerangWallClink(int k);
void AncillaAdd_HookshotWallClink(int kin, uint8 a, uint8 y);
void AncillaAdd_Duck_take_off(uint8 a, uint8 y);
void AddBirdTravelSomething(uint8 a, uint8 y);
void AncillaAdd_QuakeSpell(uint8 a, uint8 y);
void AncillaAdd_SpinAttackInitSpark(uint8 a, uint8 x, uint8 y);
void AncillaAdd_BlastWall();
void AncillaAdd_SwordChargeSparkle(int k);
void AncillaAdd_SilverArrowSparkle(int kin);
void AncillaAdd_IceRodShot(uint8 a, uint8 y);
bool AncillaAdd_Splash(uint8 a, uint8 y);
void AncillaAdd_GraveStone(uint8 ain, uint8 yin);
void AncillaAdd_WaterfallSplash();
void AncillaAdd_GTCutscene();
int AncillaAdd_DoorDebris();
// -----------------------------------------------------------------------
// Ancilla Utility and Slot Management
// -----------------------------------------------------------------------

// Convert a fire rod shot into the persistent Skull Woods fire
void FireRodShot_BecomeSkullWoodsFire(int k);
// Generic ancilla allocation: finds a free slot and sets the type
int Ancilla_AddAncilla(uint8 a, uint8 y);
// Check if an ancilla of the given type already exists (prevents duplicates)
bool AncillaAdd_CheckForPresence(uint8 a);
// Find an available arrow slot, since arrows use dedicated slots
int AncillaAdd_ArrowFindSlot(uint8 type, uint8 ay);
// Check the initial tile an ancilla spawns on for immediate collisions
int Ancilla_CheckInitialTile_A(int k);
bool Ancilla_CheckInitialTileCollision_Class2(int k);
// Terminate specific interactive ancillae (used when changing rooms)
uint8 Ancilla_TerminateSelectInteractives(uint8 y);
void Ancilla_SetXY(int k, uint16 x, uint16 y);
void AncillaAdd_ExplodingSomariaBlock(int k);
// Try to add rupees from an ancilla pickup; returns false if wallet is full
bool Ancilla_AddRupees(int k);
void DashDust_Motive(int k);
// Calculate left/right stereo pan based on ancilla's screen X position
uint8 Ancilla_CalculateSfxPan(int k);
// Allocate a slot and initialize basic ancilla properties
int Ancilla_AllocInit(uint8 type, uint8 y);
void AddSwordBeam(uint8 y);
void AncillaSpawn_SwordChargeSparkle();
int DashTremor_TwiddleOffset(int k);
void Ancilla_TerminateIfOffscreen(int j);
// Check if a bomb is sitting on top of a sprite (for carried bomb logic)
bool Bomb_CheckUndersideSpriteStatus(int k, Point16U *out_pt, uint8 *out_r10);
// Create a deflected arrow when a sprite blocks the player's shot
void Sprite_CreateDeflectedArrow(int k);
