/*
 * features.h — Runtime Feature Flags and Enhanced Gameplay Extensions
 *
 * This header defines the feature flag system that enables gameplay enhancements beyond
 * the original SNES ROM. Rather than hardcoding changes, each enhancement is gated behind
 * a bit flag stored in unused SNES WRAM addresses. This design allows:
 *   1. Features to be toggled at runtime via configuration (zelda3.ini)
 *   2. Behavior parity with the original ROM when all flags are cleared
 *   3. RAM-compare verification against the original game for correctness testing
 *
 * Architecture:
 *   The SNES has 128KB of WRAM (Work RAM). The original game leaves certain addresses
 *   unused. This reimplementation repurposes those dead locations (0x648-0x658) to store
 *   feature flags, MSU-1 audio state, and HUD extension data. Because these addresses
 *   were never written by the original game, using them does not interfere with ROM
 *   compatibility or save-state integrity.
 *
 *   The feature flags in kRam_Features0 (address 0x64c) are packed as a 32-bit bitmask.
 *   Each bit controls one enhancement. Game logic throughout the codebase tests individual
 *   bits via (enhanced_features0 & kFeatures0_XXX) before applying non-vanilla behavior.
 *
 * Dependencies:
 *   - types.h: Provides uint8, uint16, uint32 typedefs
 *   - g_ram:   External pointer to the emulated 128KB WRAM array (declared in variables.h)
 *
 * Related files:
 *   - config.c: Parses zelda3.ini and sets g_wanted_zelda_features accordingly
 *   - zelda_rtl.c: Copies g_wanted_zelda_features into enhanced_features0 at frame start
 *   - hud.c, player.c, sprite.c, misc.c: Read individual feature bits to gate behavior
 */
// This file declares extensions to the base game
#ifndef ZELDA3_FEATURES_H_
#define ZELDA3_FEATURES_H_

#include "types.h"

/*
 * Repurposed WRAM addresses — originally unused by the SNES game.
 *
 * These addresses fall in a gap between the game's active variable space and the
 * stack/scratch area. The original ROM never reads or writes these locations, so they
 * can safely store reimplementation-specific state without affecting game logic.
 *
 *   kRam_APUI00 (0x648):              Mirrors APU I/O port 0 value; used to track the
 *                                      last value sent to the SPC700 sound processor so
 *                                      the reimplementation can detect music changes
 *   kRam_CrystalRotateCounter (0x649): Frame counter for the crystal pendant rotation
 *                                      animation in the HUD; stored here to persist
 *                                      across module transitions
 *   kRam_BugsFixed (0x64a):            Bitmask of bug-fix categories currently active;
 *                                      allows selective enabling of fixes per category
 *   kRam_Features0 (0x64c):            Primary 32-bit feature flag word; each bit gates
 *                                      one gameplay enhancement (see enum below)
 */
// Special RAM locations that are unused but I use for compat things.
enum {
  kRam_APUI00 = 0x648,
  kRam_CrystalRotateCounter = 0x649,
  kRam_BugsFixed = 0x64a,
  kRam_Features0 = 0x64c,
};

/*
 * Bug-fix version constants.
 *
 * These are compile-time constants (all set to 1) that gate bug-fix code paths.
 * Each constant represents a category of fixes for original ROM bugs:
 *
 *   kBugFix_PolyRenderer:      Fixes the 3D polygon renderer (used for the Triforce
 *                               animation in the title sequence) to use correct rotation
 *                               speed timing that matches the intended framerate
 *   kBugFix_AncillaOverwrites: Fixes a bug where certain ancilla (projectiles/effects)
 *                               could overwrite each other's data in the ancilla table,
 *                               causing visual glitches or lost projectiles
 *   kBugFix_Latest:            General sentinel; code guarded by this constant includes
 *                               the most recent round of accuracy fixes
 *
 * All values are 1 (always enabled) because these fixes are considered safe and
 * non-controversial — they correct clearly unintended behavior in the original ROM.
 * Unlike the runtime feature flags in kRam_Features0, these cannot be toggled off.
 */
enum {
  // Poly rendered uses correct speed
  kBugFix_PolyRenderer = 1,
  kBugFix_AncillaOverwrites = 1,
  kBugFix_Latest = 1,
};

/*
 * Feature flag bitmask values for the kRam_Features0 word (WRAM address 0x64c).
 *
 * Each constant is a single bit (power of two) in a 32-bit field. Game code tests
 * individual features with: if (enhanced_features0 & kFeatures0_XXX) { ... }
 *
 * The flags are grouped by category:
 *   Bits 0-1:   Display and control extensions
 *   Bits 2-5:   Quality-of-life gameplay tweaks
 *   Bits 6-8:   UI and convenience improvements
 *   Bits 9-16:  Advanced features and optional bug fixes
 */
// Enum values for kRam_Features0
enum {
  // Bit 0: Extends the visible screen by 64 pixels on each side beyond the standard
  // 256-pixel SNES width, enabling a wider viewport for widescreen displays
  kFeatures0_ExtendScreen64 = 1,

  // Bit 1: Allows L/R shoulder buttons to cycle through inventory items on the
  // overworld, eliminating the need to open the pause menu to switch equipment
  kFeatures0_SwitchLR = 2,

  // Bit 2: Lets Link change facing direction while Pegasus Boot dashing, instead of
  // being locked to the initial dash direction (original behavior)
  kFeatures0_TurnWhileDashing = 4,

  // Bit 3: Allows the Magic Mirror to warp from Light World to Dark World (the
  // original game only permits Dark-to-Light warping via mirror)
  kFeatures0_MirrorToDarkworld = 8,

  // Bit 4: Enables collecting ground items (hearts, rupees) by slashing them with
  // the sword, rather than requiring Link to walk over them
  kFeatures0_CollectItemsWithSword = 16,

  // Bit 5: Allows pots and other breakable objects to be smashed with the sword
  // swing, rather than requiring Link to pick them up and throw them
  kFeatures0_BreakPotsWithSword = 32,

  // Bit 6: Silences the rapid beeping sound effect that plays when Link's health
  // drops to critical levels (a common player annoyance)
  kFeatures0_DisableLowHealthBeep = 64,

  // Bit 7: Allows skipping the opening cinematic (Sanctuary rain sequence) by
  // pressing any key, instead of requiring the player to watch it fully
  kFeatures0_SkipIntroOnKeypress = 128,

  // Bit 8: Renders the current/max count of inventory items in yellow on the HUD
  // when the player has reached the maximum carrying capacity
  kFeatures0_ShowMaxItemsInYellow = 256,

  // Bit 9: Increases the maximum number of simultaneously active bombs beyond the
  // original engine limit (normally 2 bombs on screen at once)
  kFeatures0_MoreActiveBombs = 512,

  // Bit 10: Enables visual corrections specific to widescreen mode (e.g., extending
  // BG tilemaps to fill the extra screen columns). These fixes do not alter gameplay
  // logic but will cause differences in WRAM state, affecting RAM-compare verification.
  // This is set for visual fixes that don't affect game behavior but will affect ram compare.
  kFeatures0_WidescreenVisualFixes = 1024,

  // Bit 11: Raises the rupee carrying limit beyond the vanilla 999-rupee cap,
  // useful for randomizer runs where higher denominations are in play
  kFeatures0_CarryMoreRupees = 2048,

  // Bit 12: Enables a collection of minor non-game-changing bug fixes that correct
  // graphical glitches, incorrect tile priorities, and similar cosmetic issues
  kFeatures0_MiscBugFixes = 4096,

  // Bit 13: Allows the player to cancel flute/bird travel after selecting a
  // destination, rather than being locked into the flight once confirmed
  kFeatures0_CancelBirdTravel = 8192,

  // Bit 14: Enables bug fixes that materially alter game behavior or difficulty
  // (e.g., fixing boss damage calculations, correcting item drop tables). Separated
  // from cosmetic fixes because these can affect speedrun validity and game feel.
  kFeatures0_GameChangingBugFixes = 16384,

  // Bit 15: When combined with kFeatures0_SwitchLR, restricts L/R item switching
  // to only cycle between items the player actually possesses, skipping empty slots
  kFeatures0_SwitchLRLimit = 32768,

  // Bit 16: Reduces the intensity of full-screen flash effects (e.g., lightning,
  // Ether medallion, bomb explosions) to be less visually jarring — an accessibility
  // feature for photosensitive players
  kFeatures0_DimFlashes = 65536,
};

/*
 * Memory-mapped variable accessors.
 *
 * These macros provide named access to specific WRAM locations by casting offsets
 * from the g_ram base pointer into typed pointers. This approach mirrors how the
 * original 65C816 assembly accesses absolute WRAM addresses, but with type safety.
 * Each macro dereferences in-place, so they behave as both lvalues and rvalues
 * (i.e., they can be read from and written to like regular variables).
 */

// 32-bit feature flag bitmask at WRAM 0x64c; tested by game logic to gate enhancements
#define enhanced_features0 (*(uint32*)(g_ram+0x64c))

// MSU-1 enhanced audio state. MSU-1 is a custom SNES cartridge chip (supported by
// some emulators and flash carts) that enables CD-quality streamed audio to replace
// the SPC700 soundtrack. These variables track the current playback position and
// mixing parameters:

// Current sample playback position within the active MSU-1 audio track (byte offset)
#define msu_curr_sample (*(uint32*)(g_ram+0x650))
// Master volume for MSU-1 audio output (0-255 range, applied as linear scaling)
#define msu_volume (*(uint8*)(g_ram+0x654))
// Index of the currently playing MSU-1 track number (maps to an Opus audio file)
#define msu_track (*(uint8*)(g_ram+0x655))

/*
 * HUD inventory extension variables for the L/R item-switching feature.
 *
 * hud_inventory_order: Points to a 4x6 byte grid in WRAM (at 0x225) that defines
 *   the order in which inventory items are cycled when pressing L/R. This address
 *   falls within the original game's item possession flags area and is reinterpreted
 *   as an ordering table when the SwitchLR feature is active.
 *
 * hud_cur_item_x/l/r: Track the currently selected item slot position for the
 *   Y-button (x), L-button (l), and R-button (r) quick-select assignments. These
 *   let the player have three items readily accessible without pausing.
 */
#define hud_inventory_order ((uint8*)(g_ram + 0x225)) // 4x6 bytes
#define hud_cur_item_x (*(uint8*)(g_ram+0x656))
#define hud_cur_item_l (*(uint8*)(g_ram+0x657))
#define hud_cur_item_r (*(uint8*)(g_ram+0x658))



/*
 * The desired feature flag state, as parsed from the zelda3.ini configuration file.
 *
 * At startup, config.c reads the INI file and sets bits in this variable according to
 * the user's preferences. Each frame, zelda_rtl.c copies this value into
 * enhanced_features0 (WRAM 0x64c) so that the game logic sees the active feature set.
 * This two-stage approach (config -> g_wanted -> WRAM) keeps the configuration layer
 * decoupled from the emulated memory space.
 */
extern uint32 g_wanted_zelda_features;


#endif  // ZELDA3_FEATURES_H_
