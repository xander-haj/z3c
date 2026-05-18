/*
 * ending.h — Intro Sword Animation, Credits, Triforce Room, and Ganon Emergence
 *
 * Covers four major non-gameplay sequences that bookend the player experience:
 *
 *   1. Module 00 (Intro): The iconic title screen sequence — the Master Sword
 *      descends against a black background, the Triforce triangles animate and
 *      sparkle, the Nintendo copyright fades in, and the game waits for the
 *      player to press Start. This uses a polyhedral rendering thread for the
 *      rotating 3D Triforce effect.
 *
 *   2. Module 18 (Ganon Emerges): The brief cutscene after defeating Agahnim
 *      in the final battle, where Ganon's bat form rises from Agahnim's body
 *      and flies to the Pyramid of Power.
 *
 *   3. Module 19 (Triforce Room): The ending sequence where Link touches the
 *      Triforce. The three triangles animate with polyhedral rotation before
 *      the wish-granting text appears.
 *
 *   4. Module 1A (Credits): The full end-credits roll, showing vignettes of
 *      Hyrule's restoration. Alternates between overworld and dungeon scenes
 *      with scrolling camera, NPC sprite animations, and attribution text.
 *      Concludes with "THE END" and a final Triforce animation.
 *
 * Related files:
 *   ending.c     — Implementation of all functions declared here
 *   attract.h    — Attract mode (similar cutscene framework, runs before intro)
 *   poly.c       — Polyhedral 3D rendering used for Triforce animations
 *   overworld.c  — Overworld map data used by credits scenes
 *   dungeon.c    — Dungeon room data used by credits scenes
 */
#pragma once

/*
 * IntroSpriteEnt — Sprite tile descriptor for intro and credits animations.
 *
 * Defines a single OAM tile within a multi-tile sprite group. Used to build
 * composite sprites (the Triforce triangles, copyright text, sparkle effects)
 * by specifying each tile's offset from the group anchor, its CHR index,
 * OAM attribute flags, and size extension bit.
 */
typedef struct IntroSpriteEnt {
  int8 x, y;          // Pixel offset from the sprite group's anchor point
  uint8 charnum, flags; // charnum = CHR tile index; flags = palette/flip/priority
  uint8 ext;           // OAM size extension bit (0 = 8x8, 1 = 16x16)
} IntroSpriteEnt;

// =============================================================================
//  MODULE 00: INTRO / TITLE SCREEN
// =============================================================================
// The intro sequence initializes VRAM, loads the sword and Triforce graphics,
// then runs a multi-phase animation: sword descent, flash, Triforce assembly,
// sparkle effects, copyright notice, and idle loop waiting for Start.

/*
 * Intro_SetupScreen — Configure PPU registers (BG modes, scroll positions,
 * window masks) for the intro screen's layered background composition.
 */
void Intro_SetupScreen();

/*
 * Intro_LoadTextPointersAndPalettes — Load the text string pointers and
 * color palettes needed for the title screen and copyright display.
 */
void Intro_LoadTextPointersAndPalettes();

// --- Credits Scene Loading (Overworld) ---------------------------------------
// These functions handle the multi-step process of loading an overworld
// vignette during the credits sequence. Each step runs across consecutive
// frames to avoid stalling the main loop.

void Credits_LoadScene_Overworld_PrepGFX();   // Decompress tileset into VRAM
void Credits_LoadScene_Overworld_Overlay();   // Apply overlay tilemap layer
void Credits_LoadScene_Overworld_LoadMap();    // Load the 32x32 map16 data

/*
 * Credits_OperateScrollingAndTileMap — Update the camera scroll position
 * and rebuild the visible tilemap rows/columns each frame during a credits
 * overworld scene. Handles smooth vertical scrolling at a fixed speed.
 */
void Credits_OperateScrollingAndTileMap();

/*
 * Credits_LoadCoolBackground — Load the special starfield or gradient
 * background used behind the "THE END" text in the final credits scene.
 */
void Credits_LoadCoolBackground();

/*
 * Credits_LoadScene_Dungeon — Load a dungeon room as a credits vignette
 * backdrop. Handles tileset, palette, and object layer decompression.
 */
void Credits_LoadScene_Dungeon();

// =============================================================================
//  MODULE 18: GANON EMERGES
// =============================================================================

/*
 * Module18_GanonEmerges — State machine for the Ganon emergence cutscene.
 * After the final Agahnim fight, Ganon's bat form rises and flies away.
 * Manages the palette flash, bat sprite animation, and screen transition
 * to the Pyramid of Power.
 */
void Module18_GanonEmerges();

// =============================================================================
//  MODULE 19: TRIFORCE ROOM
// =============================================================================

/*
 * Module19_TriforceRoom — State machine for the Triforce wish room sequence.
 * Link approaches the Triforce, the three triangles animate with 3D rotation,
 * and the wish text scrolls. Transitions into the credits when complete.
 */
void Module19_TriforceRoom();

// =============================================================================
//  INTRO INITIALIZATION AND MEMORY SETUP
// =============================================================================

/*
 * Intro_InitializeBackgroundSettings — Set initial BG scroll offsets,
 * tilemap base addresses, and character base addresses for all four
 * background layers used during the intro.
 */
void Intro_InitializeBackgroundSettings();

/*
 * Polyhedral_InitializeThread — Set up the cooperative thread that runs the
 * polyhedral (3D Triforce) renderer. This uses a separate execution context
 * so the 3D math can yield between frames without blocking the main loop.
 */
void Polyhedral_InitializeThread();

/*
 * Module00_Intro — Top-level entry point for the intro module. Called each
 * frame by the main module dispatcher. Delegates to the current intro phase
 * (init, sword animation, Triforce animation, or idle wait).
 */
void Module00_Intro();

void Intro_Init();          // First-frame initialization: clear VRAM, reset state
void Intro_Init_Continue(); // Second-frame init: load graphics after VRAM is ready

/*
 * Intro_Clear1kbBlocksOfWRAM — Zero out 1 KB blocks of Work RAM used by
 * the intro. Clears sprite tables, tilemap buffers, and animation state
 * to prevent stale data from previous game sessions from appearing.
 */
void Intro_Clear1kbBlocksOfWRAM();

/*
 * Intro_InitializeMemory_darken — Set the screen brightness to minimum
 * (forced blank) and initialize memory regions that must be zeroed before
 * the fade-in begins.
 */
void Intro_InitializeMemory_darken();

// --- Intro Fade and Sword Animation ------------------------------------------

void IntroZeldaFadein();       // Fade in the "Zelda" logo text from black
void Intro_FadeInBg();         // Fade in the background layer behind the sword

/*
 * Intro_SwordComingDown — Animate the Master Sword descending from the top
 * of the screen to its resting position. Moves the sword sprite downward
 * each frame until it reaches the target Y coordinate.
 */
void Intro_SwordComingDown();

/*
 * Intro_WaitPlayer — Idle state after the intro animation completes. Waits
 * for the player to press Start to proceed to the file select screen, or
 * times out and transitions to the attract mode demo.
 */
void Intro_WaitPlayer();

/*
 * FadeMusicAndResetSRAMMirror — Fade out the title screen music and reset
 * the SRAM mirror buffer to prepare for transitioning away from the intro.
 */
void FadeMusicAndResetSRAMMirror();

// --- Triforce 3D Animation --------------------------------------------------
// The intro's Triforce animation uses a polyhedral renderer (poly.c) running
// in a cooperative thread. These functions manage the thread lifecycle and
// coordinate the three triangle sprites with the 3D projection output.

void Intro_InitializeTriforcePolyThread(); // Create the polyhedral thread
void Intro_InitGfx_Helper();              // Load Triforce tile graphics
void LoadTriforceSpritePalette();          // Set sprite palette for gold triangles

/*
 * Intro_HandleAllTriforceAnimations — Per-frame dispatcher that updates all
 * active Triforce triangle sprites, advancing their animation state machines
 * and writing their OAM entries.
 */
void Intro_HandleAllTriforceAnimations();

/*
 * Scene_AnimateEverySprite — Iterate over all active scene sprite slots and
 * call each sprite's per-frame animation handler. Shared between the intro
 * and credits sequences.
 */
void Scene_AnimateEverySprite();

void Intro_AnimateTriforce(); // Drive the Triforce assembly animation sequence
void Intro_RunStep();         // Execute one animation step for the intro state

// --- Individual Sprite Type Handlers -----------------------------------------
// The intro uses a type system for scene sprites: Type A = Triforce triangles,
// Type B = sparkle/copyright effects. Each type has init and animate handlers.

/*
 * Intro_AnimOneObj — Dispatch animation for a single scene sprite by its type.
 * @param k  Sprite slot index (0-based)
 */
void Intro_AnimOneObj(int k);

void Intro_SpriteType_A_0(int k); // Animate a Triforce triangle (initial phase)
void Intro_SpriteType_B_0(int k); // Animate a Type B sprite (initial phase)

/*
 * AnimateSceneSprite_DrawTriangle — Render a single Triforce triangle sprite
 * using the current animation frame's tile layout.
 * @param k  Sprite slot index
 */
void AnimateSceneSprite_DrawTriangle(int k);

/*
 * Intro_CopySpriteType4ToOam — Write a Type 4 sprite's tile data directly
 * to the OAM buffer for hardware rendering.
 * @param k  Sprite slot index
 */
void Intro_CopySpriteType4ToOam(int k);

/*
 * EXIT_0CCA90 — Named after its original ROM address. Handles the exit
 * condition for a sprite animation loop, marking the sprite as inactive
 * when its animation sequence completes.
 * @param k  Sprite slot index
 */
void EXIT_0CCA90(int k);

// --- Copyright Notice Sprites ------------------------------------------------

void InitializeSceneSprite_Copyright(int k); // Set up copyright text sprite
void AnimateSceneSprite_Copyright(int k);    // Fade in and display copyright

// --- Sparkle Effect Sprites --------------------------------------------------
// Small glinting particles that appear on the Triforce during the intro.

void InitializeSceneSprite_Sparkle(int k); // Spawn a sparkle at a random point
void AnimateSceneSprite_Sparkle(int k);    // Animate sparkle expansion and fade

/*
 * AnimateSceneSprite_AddObjectsToOamBuffer — Write a multi-tile sprite group
 * to the OAM buffer from an IntroSpriteEnt array.
 *
 * @param k    Sprite slot index (determines OAM base offset)
 * @param src  Array of tile descriptors
 * @param num  Number of tiles in the array
 */
void AnimateSceneSprite_AddObjectsToOamBuffer(int k, const IntroSpriteEnt *src, int num);

/*
 * AnimateSceneSprite_MoveTriangle — Update a Triforce triangle's position
 * during the assembly animation where the three triangles slide into
 * formation.
 * @param k  Sprite slot index
 */
void AnimateSceneSprite_MoveTriangle(int k);

// --- Triforce Room Polyhedral Rendering --------------------------------------

/*
 * TriforceRoom_PrepGFXSlotForPoly — Reserve a graphics slot in VRAM for the
 * polyhedral renderer's output tiles during the Triforce room sequence.
 */
void TriforceRoom_PrepGFXSlotForPoly();

void Credits_InitializePolyhedral(); // Init the poly renderer for credits use
void AdvancePolyhedral();            // Step the polyhedral thread forward one frame
void TriforceRoom_HandlePoly();      // Manage poly rendering in the Triforce room

/*
 * Credits_AnimateTheTriangles — Per-frame update for the three Triforce
 * triangle sprites during the credits' final "THE END" sequence, where
 * they rotate and disperse.
 */
void Credits_AnimateTheTriangles();

// --- Triforce Room Triangle Sprites ------------------------------------------

void InitializeSceneSprite_TriforceRoomTriangle(int k); // Setup for room triangles

/*
 * Intro_SpriteType_B_456 — Handle animation for Type B sprite sub-types
 * 4, 5, and 6, which are the Triforce room's expanding triangle effects.
 * @param k  Sprite slot index
 */
void Intro_SpriteType_B_456(int k);

/*
 * AnimateTriforceRoomTriangle_HandleContracting — Animate a Triforce room
 * triangle as it contracts inward during the wish sequence.
 * @param k  Sprite slot index
 */
void AnimateTriforceRoomTriangle_HandleContracting(int k);

// --- Credits Triangle Sprites ------------------------------------------------

void InitializeSceneSprite_CreditsTriangle(int k); // Setup credits triangles
void AnimateSceneSprite_CreditsTriangle(int k);    // Animate credits triangles

// --- Intro Logo and Sword Effects --------------------------------------------

void Intro_DisplayLogo();                  // Render the "The Legend of Zelda" logo
void Intro_SetupSwordAndIntroFlash();      // Initialize the sword descent + flash
void Intro_PeriodicSwordAndIntroFlash();   // Per-frame sword glint effect update

// =============================================================================
//  MODULE 1A: CREDITS SEQUENCE
// =============================================================================

/*
 * Module1A_Credits — Top-level state machine for the end credits. Called each
 * frame while the credits are active. Manages scene transitions, camera
 * scrolling, NPC animations, text attribution display, and the final
 * Triforce animation leading to "THE END."
 */
void Module1A_Credits();

// --- Credits Scene Loading ---------------------------------------------------

void Credits_LoadNextScene_Overworld(); // Queue the next overworld vignette
void Credits_LoadNextScene_Dungeon();   // Queue the next dungeon vignette

/*
 * Credits_PrepAndLoadSprites — Initialize NPC sprite data for the current
 * credits scene. Each vignette features specific characters (villagers,
 * animals, bosses) that are loaded here.
 */
void Credits_PrepAndLoadSprites();

// --- Credits Scene Scrolling -------------------------------------------------

void Credits_ScrollScene_Overworld(); // Per-frame scroll for overworld scenes
void Credits_ScrollScene_Dungeon();   // Per-frame scroll for dungeon scenes

/*
 * Credits_HandleSceneFade — Manage brightness fading between credits scenes.
 * Fades out the current scene, triggers the next scene load, then fades in.
 */
void Credits_HandleSceneFade();

// --- Credits Sprite Drawing --------------------------------------------------
// These functions render NPC sprites and visual effects in the credits
// vignettes. Each vignette has unique sprite compositions showing the
// restored Hyrule (e.g., the lumberjack's tree, the flute boy, animals).

/*
 * Credits_SpriteDraw_DrawShadow — Render a circular shadow beneath a sprite.
 * @param k  Sprite slot index
 */
void Credits_SpriteDraw_DrawShadow(int k);

/*
 * EndSequence_DrawShadow2 — Alternate shadow renderer with different sizing.
 * @param k  Sprite slot index
 */
void EndSequence_DrawShadow2(int k);

/*
 * Ending_Func2 — Multi-purpose credits sprite handler dispatched by type.
 * @param k    Sprite slot index
 * @param ain  Sub-function selector determining which behavior to execute
 */
void Ending_Func2(int k, uint8 ain);

/*
 * Credits_SpriteDraw_ActivateAndRunSprite — Enable a sprite slot and execute
 * its per-frame drawing logic. Called when the camera scrolls a sprite's
 * trigger point into view.
 * @param k  Sprite slot index
 * @param a  Sprite type identifier
 */
void Credits_SpriteDraw_ActivateAndRunSprite(int k, uint8 a);

void Credits_SpriteDraw_PreexistingSpriteDraw(int k, uint8 a); // Draw already-active sprite

/*
 * Credits_SpriteDraw_Single — Render one sprite using a specific animation
 * frame from the credits sprite sheet.
 * @param k  Sprite slot index
 * @param a  Sprite type
 * @param j  Animation frame index
 */
void Credits_SpriteDraw_Single(int k, uint8 a, uint8 j);

void Credits_SpriteDraw_SetShadowProp(int k, uint8 a); // Configure shadow properties

/*
 * Credits_SpriteDraw_AddSparkle — Spawn sparkle particle effects at a given
 * position, used for magical restoration effects in credits scenes.
 * @param j_count  Number of sparkle particles to create
 * @param xb       Base X coordinate
 * @param yb       Base Y coordinate
 */
void Credits_SpriteDraw_AddSparkle(int j_count, uint8 xb, uint8 yb);

// --- Credits Character-Specific Animations -----------------------------------

/*
 * Credits_SpriteDraw_WalkLinkAwayFromPedestal — Animate Link walking south
 * away from the Master Sword pedestal in the final credits scene, signifying
 * the adventure's end.
 * @param k  Sprite slot index for Link
 */
void Credits_SpriteDraw_WalkLinkAwayFromPedestal(int k);

void Credits_SpriteDraw_MoveSquirrel(int k);   // Animate squirrel NPC movement
void Credits_SpriteDraw_CirclingBirds(int k);  // Animate birds circling overhead

// --- Credits Camera and Scroll Control ---------------------------------------

/*
 * Credits_HandleCameraScrollControl — Manage the vertical camera scroll
 * speed and direction during credits scenes. Some scenes scroll up, others
 * scroll down, and the speed varies by scene.
 */
void Credits_HandleCameraScrollControl();

// --- End Sequence Effects ----------------------------------------------------

void EndSequence_32();                    // Final sequence cleanup step
void Credits_FadeOutFixedCol();           // Fade fixed color layer to black
void Credits_FadeColorAndBeginAnimating();// Transition into final animation phase

// --- Credits Text and Attribution --------------------------------------------

/*
 * Credits_AddNextAttribution — Display the next staff credit attribution
 * (e.g., "DIRECTOR: TAKASHI TEZUKA") by writing tiles into the BG tilemap.
 */
void Credits_AddNextAttribution();

/*
 * Credits_AddEndingSequenceText — Display ending narrative text
 * (the epilogue describing Hyrule's restoration).
 */
void Credits_AddEndingSequenceText();

// --- Final Triforce Animation and THE END ------------------------------------

void Credits_BrightenTriangles();       // Increase triangle brightness for finale
void Credits_StopCreditsScroll();       // Halt camera scrolling for final scene
void Credits_FadeAndDisperseTriangles();// Animate triangles flying apart
void Credits_FadeInTheEnd();            // Fade in "THE END" text

/*
 * Credits_HangForever — Terminal state after "THE END" is displayed. Loops
 * indefinitely, keeping the final frame on screen until the player resets.
 */
void Credits_HangForever();

/*
 * CrystalCutscene_InitializePolyhedral — Initialize the polyhedral renderer
 * for the crystal maiden cutscene variant (used when a crystal is obtained
 * in a dungeon, sharing the Triforce room's 3D rendering infrastructure).
 */
void CrystalCutscene_InitializePolyhedral();
