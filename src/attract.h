/*
 * attract.h — Demo / Attract Mode Sequences and Title Screen
 *
 * Defines the data structures and function declarations for the attract mode
 * that plays when the game idles on the title screen. The attract sequence
 * tells the backstory of Hyrule through a series of animated scenes: the
 * Triforce legend, the Imprisoning War, Zelda's imprisonment, Agahnim at
 * the altar, and the maiden warp. Each scene is a self-contained state
 * machine driven by Module14_Attract() which cycles through scene setup,
 * fade transitions, sprite animation, and timed text overlays.
 *
 * The attract mode also handles the world map zoom effect seen during the
 * backstory narration, and provides the transition into the file select
 * screen when the player presses Start.
 *
 * Related files:
 *   attract.c      — Implementation of all attract mode logic
 *   select_file.h  — File select screen (entered after attract)
 *   ending.h       — Ending/credits sequences (similar animation framework)
 *   messaging.c    — Text rendering used by Attract_ShowTimedTextMessage
 */
#pragma once

/*
 * AttractOamInfo — Sprite OAM descriptor for attract mode animations.
 *
 * Compact representation of a single sprite tile used in the attract mode
 * cutscene animations. Each entry defines one hardware sprite's position,
 * tile index, palette/flip flags, and size. Arrays of these are passed to
 * Attract_DrawSpriteSet2() to batch-render multi-tile characters.
 */
typedef struct AttractOamInfo {
  int8 x, y;     // Pixel offset from the sprite group's anchor point
  uint8 c, f, e; // c = CHR tile number, f = OAM flags (palette/flip), e = size/ext
} AttractOamInfo;


// --- Lookup Tables for World Map Zoom Effect ---------------------------------

/*
 * Zoom coefficient tables for the world map scene during the attract sequence.
 * Each contains 240 entries (one per scanline) that control the Mode 7
 * scaling matrix to produce the progressive zoom-in/zoom-out effect as the
 * camera "flies" over the Hyrule overworld map.
 */
extern const uint16 kMapMode_Zooms1[240];
extern const uint16 kMapMode_Zooms2[240];

// --- Sprite Drawing Helpers --------------------------------------------------

/*
 * Attract_DrawSpriteSet2 — Batch-render a group of sprite tiles from an
 * AttractOamInfo array. Used to draw multi-tile characters (Zelda, soldiers,
 * maidens) during cutscene animations.
 *
 * @param p  Pointer to array of AttractOamInfo entries
 * @param n  Number of entries in the array
 */
void Attract_DrawSpriteSet2(const AttractOamInfo *p, int n);

// --- Zelda Prison Scene State Machine ----------------------------------------
// These functions implement the individual animation phases of the scene
// where Zelda is shown imprisoned in the castle dungeon.

void Attract_ZeldaPrison_Case0();  // Initialize prison scene backdrop and tiles
void Attract_ZeldaPrison_Case1();  // Animate Zelda's idle movement in the cell
void Attract_ZeldaPrison_DrawA();  // Render Zelda's sprite within the prison

// --- Maiden Warp Scene State Machine -----------------------------------------
// Five-phase animation showing a maiden being warped into the Dark World
// by Agahnim's magic. Each case advances the warp visual effect.

void Attract_MaidenWarp_Case0();  // Setup warp effect initial state
void Attract_MaidenWarp_Case1();  // Begin particle expansion
void Attract_MaidenWarp_Case2();  // Mid-warp flash and palette shift
void Attract_MaidenWarp_Case3();  // Warp contraction and maiden fade-out
void Attract_MaidenWarp_Case4();  // Finalize warp, clean up sprites

// --- Dungeon Room Loading (shared with dungeon module) -----------------------

/*
 * Dungeon_LoadAndDrawEntranceRoom — Load and render a dungeon room for use
 * as a backdrop in attract mode scenes (e.g., the prison, the altar).
 *
 * @param a  Entrance ID identifying which dungeon room to load
 */
void Dungeon_LoadAndDrawEntranceRoom(uint8 a);

/*
 * Dungeon_SaveAndLoadLoadAllPalettes — Swap palette sets when transitioning
 * between attract scenes that use different dungeon tilesets.
 *
 * @param a  Palette group index to load
 * @param k  Sub-palette selector within the group
 */
void Dungeon_SaveAndLoadLoadAllPalettes(uint8 a, uint8 k);

// --- Module 14: Attract Mode Entry Point and Core Flow -----------------------

/*
 * Module14_Attract — Top-level state machine for the entire attract sequence.
 * Called each frame by the main module dispatcher when the game is in attract
 * mode (module 0x14). Delegates to the current sub-module based on an
 * internal phase counter that advances through all scenes in order.
 */
void Module14_Attract();

// --- Fade Transition Routines ------------------------------------------------
// Control screen brightness during scene transitions. The attract mode uses
// hardware brightness registers to smoothly fade between black and the
// current scene's visuals.

void Attract_Fade();            // Master fade dispatcher
void Attract_FadeInStep();      // Increment brightness by one step
void Attract_FadeInSequence();  // Full fade-in from black over multiple frames
void Attract_FadeOutSequence(); // Full fade-out to black over multiple frames

// --- Scene Initialization and Graphics Setup ---------------------------------

/*
 * Attract_InitGraphics — Load shared graphics assets (tilesets, palettes)
 * needed by all attract mode scenes. Called once when attract mode begins.
 */
void Attract_InitGraphics();

/*
 * Attract_LoadNewScene — Transition to the next scene in the attract
 * sequence. Tears down the current scene's state and initializes the
 * graphics, tilemaps, and sprite data for the upcoming scene.
 */
void Attract_LoadNewScene();

// --- Individual Scene Handlers -----------------------------------------------
// Each AttractScene_* function manages the full lifecycle of one backstory
// scene, including tilemap construction, palette animation, and timing.

void AttractScene_PolkaDots();   // Animated dot pattern transition screen
void AttractScene_WorldMap();    // Mode 7 overhead map fly-over
void AttractScene_ThroneRoom();  // King and sages sealing Ganon scene

// --- Scene Preparation Functions ---------------------------------------------
// Set up specific scene assets before the scene's main loop begins.

void Attract_PrepFinish();       // Prepare final attract scene before file select
void Attract_PrepZeldaPrison();  // Load dungeon room and Zelda sprites for prison
void Attract_PrepMaidenWarp();   // Load warp effect palette and particle sprites

void AttractScene_EndOfStory();  // Final story text screen before file select

// --- Death Screen and Story Narration ----------------------------------------

/*
 * Death_Func31 — Handle the "Game Over" screen attract transition. When the
 * player dies, this routine manages the transition from the death screen
 * into either the file select or a continue prompt.
 */
void Death_Func31();

/*
 * Attract_EnactStory — Orchestrate the backstory narration that plays across
 * multiple scenes. Manages the global progression counter that determines
 * which story segment to show next.
 */
void Attract_EnactStory();

// --- Scene Dramatization Functions -------------------------------------------
// These implement the per-frame animation logic for each narrative scene,
// handling character movement, palette cycling, and effect timing.

void AttractDramatize_PolkaDots();      // Animate polka dot transition pattern
void AttractDramatize_WorldMap();       // Animate map zoom and scroll
void Attract_ThroneRoom();             // Animate throne room characters
void AttractDramatize_Prison();         // Animate Zelda in prison cell
void AttractDramatize_AgahnimAltar();   // Animate Agahnim's ritual at the altar

/*
 * Attract_SkipToFileSelect — Immediately abort the attract sequence and
 * transition to the file select screen. Triggered when the player presses
 * Start during any attract scene.
 */
void Attract_SkipToFileSelect();

// --- Background and Tilemap Construction -------------------------------------

/*
 * Attract_BuildNextImageTileMap — Construct the next frame's background
 * tilemap for the current attract scene. Scenes that scroll or animate
 * their backgrounds call this each frame to update VRAM.
 */
void Attract_BuildNextImageTileMap();

/*
 * Attract_ShowTimedTextMessage — Display story narration text on screen
 * for a fixed duration, then auto-advance. Uses the messaging system to
 * render text over the scene's background.
 */
void Attract_ShowTimedTextMessage();

/*
 * Attract_ControlMapZoom — Update Mode 7 scaling parameters each frame
 * during the world map scene, producing the smooth zoom animation by
 * interpolating between entries in kMapMode_Zooms1/Zooms2.
 */
void Attract_ControlMapZoom();

/*
 * Attract_BuildBackgrounds — Assemble multi-layer background tilemaps
 * for scenes that use parallax or layered composition.
 */
void Attract_BuildBackgrounds();

/*
 * Attract_TriggerBGDMA — Initiate a DMA transfer to update VRAM with
 * new background tile data during the vertical blanking interval.
 *
 * @param dstv  VRAM destination address for the DMA transfer
 */
void Attract_TriggerBGDMA(uint16 dstv);

// --- Sprite Rendering --------------------------------------------------------

/*
 * Attract_DrawPreloadedSprite — Render a multi-tile sprite from parallel
 * coordinate/attribute arrays. Used for characters whose animation frames
 * are stored as separate X, Y, CHR, flag, and extension arrays rather
 * than packed AttractOamInfo structs.
 *
 * @param xp  Array of X positions for each tile
 * @param yp  Array of Y positions for each tile
 * @param cp  Array of CHR tile numbers
 * @param fp  Array of OAM flag bytes (palette, flip)
 * @param ep  Array of extension bytes (size bit)
 * @param n   Number of tiles to draw
 */
void Attract_DrawPreloadedSprite(const uint8 *xp, const uint8 *yp, const uint8 *cp, const uint8 *fp, const uint8 *ep, int n);

/*
 * Attract_DrawZelda — Render Princess Zelda's sprite in the current attract
 * scene. Selects the appropriate animation frame based on the scene's
 * internal timer and Zelda's current action state.
 */
void Attract_DrawZelda();

/*
 * Sprite_SimulateSoldier — Render and animate a castle soldier sprite during
 * attract mode scenes. Unlike in-game soldiers (which use the full sprite AI
 * system), these are simplified visual-only simulations used for backdrop
 * characters in the throne room and prison scenes.
 *
 * @param k      Sprite slot index for OAM allocation
 * @param x      Screen X coordinate
 * @param y      Screen Y coordinate
 * @param dir    Facing direction (0=up, 1=down, 2=left, 3=right)
 * @param flags  OAM attribute flags (palette, priority)
 * @param gfx    Animation frame / CHR tile base index
 */
void Sprite_SimulateSoldier(int k, uint16 x, uint16 y, uint8 dir, uint8 flags, uint8 gfx);
