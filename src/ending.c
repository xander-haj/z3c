/*
 * ending.c — Title screen, endings, and cutscene modules for Zelda: A Link to the Past (C reimplementation)
 *
 * This file implements four distinct game modules that share the polyhedral Triforce animation
 * engine and credit-sequence infrastructure:
 *
 *   Module 00 (Intro / Title Screen):
 *     12-phase state machine animating the opening title. Phases: force-blank setup, Triforce
 *     polyhedral spin, Zelda logo wipe, falling master sword, attract-mode loop. Uses 8 sprite
 *     slots (intro_sprite_isinited / intro_sprite_subtype arrays, types 0-7) and the NMI
 *     polyhedral thread running in kPolyThreadRam (g_ram+0x1f00).
 *
 *   Module 18 (GanonEmerges):
 *     9-phase cutscene triggered after defeating Agahnim. Link is carried by a bird to the Pyramid
 *     of Power where Ganon breaks free. Transitions into Module 19.
 *
 *   Module 19 (TriforceRoom):
 *     15-phase sequence showing the three Triforce pieces converging and glowing, then fading to
 *     black before the credits begin. Uses its own polyhedral convergence routine
 *     (TriforceRoom_HandlePoly) driven by kTriforce_Xfinal/Yfinal target positions.
 *
 *   Module 1A (Credits / EndSequence):
 *     39-function dispatch table (kEndSequence_Funcs). 16 credit scenes of alternating overworld
 *     and dungeon backgrounds, each handled by a LoadNextScene + ScrollScene pair. Final phases
 *     (32-38) initialise the spinning Triforce mosaic overlay, animate the ending text scroll
 *     (Credits_FadeColorAndBeginAnimating feeding kEnding_Credits_Text through
 *     Credits_AddNextAttribution), then brighten, disperse, and fade to the final "THE END" screen.
 *
 * Relationships:
 *   - load_gfx.h / overworld.h / dungeon.h: scene loading helpers called by credits phases.
 *   - misc.h / messaging.h: palette filters, VWF text pipeline, module state shared via globals.
 *   - player_oam.h: PrepOamCoords used for sprite placement during cutscenes.
 *   - poly.c (via variables.h): polyhedral NMI thread state (poly_a/b, poly_config*, is_nmi_thread_active).
 */
#include "zelda_rtl.h"
#include "snes/snes_regs.h"
#include "variables.h"
#include "load_gfx.h"
#include "dungeon.h"
#include "sprite.h"
#include "ending.h"
#include "overworld.h"
#include "player.h"
#include "misc.h"
#include "messaging.h"
#include "player_oam.h"
#include "sprite_main.h"
#include "ancilla.h"
#include "hud.h"
#include "assets.h"

// Palette cycle used by the polyhedral Triforce NMI thread. Each entry is a CGRAM word offset
// (in bytes) into the palette that the spinning geometry writes to, cycling through 8 colour steps
// to produce the rainbow-shimmer effect seen on the title screen and Triforce room.
static const uint16 kPolyhedralPalette[8] = { 0, 0x14d, 0x1b0, 0x1f3, 0x256, 0x279, 0x2fd, 0x35f };

// Index of the credit scene currently being rendered (0-based). Stored in g_ram+0xcc so that
// Credits_AddNextAttribution can track how many death-count entries have been appended.
#define ending_which_dung (*(uint16*)(g_ram+0xcc))
// Base address of the 256-byte scratch region used by the polyhedral NMI thread for its working
// geometry state. The NMI handler reads/writes here while the main thread is in vblank.
#define kPolyThreadRam (g_ram + 0x1f00)
// Horizontal velocity for title-screen intro sprite type 0 (sparkling star), indexed by phase.
static const int8 kIntroSprite0_Xvel[3] = { 1, 0, -1 };
// Vertical velocity for title-screen intro sprite type 0, indexed by phase. Negative = upward.
static const int8 kIntroSprite0_Yvel[3] = { -1, 1, -1 };
// X positions (screen pixels) for the four maiden-portrait sprites (type 3) shown on the title screen.
static const uint8 kIntroSprite3_X[4] = { 0xc2, 0x98, 0x6f, 0x34 };
// Y positions for the four maiden-portrait sprites, matching kIntroSprite3_X index-for-index.
static const uint8 kIntroSprite3_Y[4] = { 0x7c, 0x54, 0x7c, 0x57 };
// Sequencing table for type-3 sprite animation: indices 0-3 cycle portrait sub-frames; 0xff = done.
static const uint8 kIntroSprite3_State[8] = { 0, 1, 2, 3, 2, 1, 0xff, 0xff };
// Final screen X positions for the three Triforce pieces (left, top, right) during the
// TriforceRoom convergence animation. Pieces drift toward these coordinates over several frames.
static const uint8 kTriforce_Xfinal[3] = { 0x59, 0x5f, 0x67 };
// Final screen Y positions for the three Triforce pieces (left=bottom, top=apex, right=bottom).
static const uint8 kTriforce_Yfinal[3] = { 0x74, 0x68, 0x74 };
// World-space X coordinates (subpixels, shifted 8) for every sprite placed during the 16 credit
// scenes. kEndingSprites_Idx[scene] and kEndingSprites_Idx[scene+1] give the slice range into
// this array for a given scene index (0-15).
static const uint16 kEndingSprites_X[] = {
  0x1e0, 0x200, 0x1ed, 0x203, 0x1da, 0x216, 0x1c8, 0x228, 0x1c0, 0x1e0, 0x208, 0x228,
  0xf8, 0xf0,
  0x278, 0x298, 0x1e0, 0x200, 0x220, 0x288, 0x1e2,
  0xe0, 0x150, 0xe8, 0x168, 0x128, 0x170, 0x170,
  0x335, 0x335, 0x300,
  0xb8, 0xce, 0xac, 0xc4,
  0x3b0, 0x390, 0x3d0,
  0xf8, 0xc8,
  0x80,
  0xf8, 0xf8, 0xf8, 0xf8, 0xf8, 0xe8, 0xf8, 0xd8, 0xf8, 0xc8, 0x108,
  0x70, 0x70, 0x70, 0x68, 0x88, 0x70,
  0x40, 0x70, 0x4f, 0x61, 0x37, 0x79,
  0xc8, 0x278, 0x258, 0x1d8, 0x1c8, 0x188, 0x270,
  0x180,
  0x2e8, 0x270, 0x270, 0x2a0, 0x2a0, 0x2a4, 0x2fc,
  0x76, 0x73, 0x76, 0x0, 0xd0, 0x80,
};
// World-space Y coordinates for credit-scene sprites, parallel to kEndingSprites_X.
static const uint16 kEndingSprites_Y[] = {
  0x158, 0x158, 0x138, 0x138, 0x140, 0x140, 0x150, 0x150, 0x120, 0x120, 0x120, 0x120,
  0x60, 0x37,
  0xc2, 0xc2, 0x16b, 0x16c, 0x16b, 0xb8, 0x16b,
  0x80, 0x60, 0x146, 0x146, 0x1c6, 0x70, 0x70,
  0x128, 0x128, 0x16f,
  0xf5, 0xfc, 0x10d, 0x10d,
  0x40, 0x40, 0x40,
  0x150, 0x158,
  0xf4,
  0x120, 0x120, 0x120, 0x120, 0x120, 0x108, 0x100, 0xd8, 0xd8, 0xf0, 0xf0,
  0x3c, 0x3c, 0x3c, 0x90, 0x80, 0x3c,
  0x16c, 0x16c, 0x174, 0x174, 0x175, 0x175,
  0x250, 0x2b0, 0x2b0, 0x2a0, 0x2b0, 0x2b0, 0x2b8,
  0xd8,
  0x24b, 0x1b0, 0x1c8, 0x1c8, 0x1b0, 0x230, 0x230,
  0x8b, 0x83, 0x85, 0x2c, 0xf8, 0x100,
};
// Start indices into kEndingSprites_X/Y for each of the 16 credit scenes, plus one sentinel at
// the end (85). Slice [kEndingSprites_Idx[i], kEndingSprites_Idx[i+1]) gives scene i's sprites.
static const uint8 kEndingSprites_Idx[17] = {
  0, 12, 14, 21, 28, 31, 35, 38, 40, 41, 52, 58, 64, 71, 72, 79, 85
};
// Three-sub-phase dispatch for loading an overworld credit scene. Called via subsubmodule_index
// from Credits_LoadNextScene_Overworld: PrepGFX (tileset/palette setup), Overlay (load overlays),
// LoadMap (build tilemap and place sprites). Parallels the dungeon Credits_LoadScene_Dungeon path.
static PlayerHandlerFunc *const kEndSequence0_Funcs[3] = {
&Credits_LoadScene_Overworld_PrepGFX,
&Credits_LoadScene_Overworld_Overlay,
&Credits_LoadScene_Overworld_LoadMap,
};
// Scratch structure reused by PrepOamCoords during credit-scene sprite OAM placement.
static PrepOamCoordsRet g_ending_coords;
// Target BG1/BG2 vertical scroll position for each of the 16 credit scenes. The camera drifts
// toward this value at the rate set by kEnding1_Yvel[scene] each frame.
static const uint16 kEnding1_TargetScrollY[16] = { 0x6f2, 0x210, 0x72c, 0xc00, 0x10c, 0xa9b, 0x10, 0x510, 0x89, 0xa8e, 0x222c, 0x2510, 0x826, 0x5c, 0x20a, 0x30 };
// Target horizontal scroll position for each of the 16 credit scenes.
static const uint16 kEnding1_TargetScrollX[16] = { 0x77f, 0x480, 0x193, 0xaa, 0x878, 0x847, 0x4fd, 0xc57, 0x40f, 0x478, 0xa00, 0x200, 0x201, 0xaa1, 0x26f, 0 };
// Vertical scroll velocity (signed, pixels per frame) for each credit scene. Negative = scroll up.
static const int8 kEnding1_Yvel[16] = { -1, -1, 1, -1, 1, 1, 0, 1, 0, -1, -1, 0, 0, 0, 1, -1 };
// Horizontal scroll velocity for each credit scene. Negative = scroll left.
static const int8 kEnding1_Xvel[16] = { 0, 0, -1, 0, 0, -1, 1, 0, -1, 0, 0, 0, 1, -1, 1, 0 };
// Master 39-function dispatch table for Module 1A (Credits). submodule_index selects the active
// entry. The first 32 entries (indices 0-31) are 16 alternating LoadNextScene+ScrollScene pairs
// for the 16 credit scenes. Indices 32-38 handle the final Triforce mosaic, text scroll, fade,
// disperse, and "THE END" phases.
static PlayerHandlerFunc *const kEndSequence_Funcs[39] = {
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Dungeon,
&Credits_ScrollScene_Dungeon,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Dungeon,
&Credits_ScrollScene_Dungeon,
&Credits_LoadNextScene_Dungeon,
&Credits_ScrollScene_Dungeon,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&Credits_LoadNextScene_Overworld,
&Credits_ScrollScene_Overworld,
&EndSequence_32,
&Credits_BrightenTriangles,
&Credits_FadeColorAndBeginAnimating,
&Credits_StopCreditsScroll,
&Credits_FadeAndDisperseTriangles,
&Credits_FadeInTheEnd,
&Credits_HangForever,
};
// Y position (pixels) of the falling master sword during the title intro animation (g_ram+0xc8).
#define intro_sword_ypos WORD(g_ram[0xc8])
// Auxiliary state bytes for the falling sword animation, tracking velocity and collision phase.
#define intro_sword_18 g_ram[0xca]
#define intro_sword_19 g_ram[0xcb]
#define intro_sword_20 g_ram[0xcc]
#define intro_sword_21 g_ram[0xcd]
#define intro_sword_24 g_ram[0xd0]
// Room/entrance indices loaded for each of the 16 credit scenes. Values ≥0x1000 are dungeon room
// numbers (loaded via Dungeon_LoadEntrance); values <0x1000 are overworld entrance IDs (loaded
// via LoadOverworldFromDungeon or Overworld_EnterSpecialArea for scenes 6 and 15).
static const uint16 kEnding_Tab1[16] = {
  0x1000, 2, 0x1002, 0x1012, 0x1004, 0x1006, 0x1010, 0x1014, 0x100a,
  0x1016, 0x5d, 0x64, 0x100e, 0x1008, 0x1018, 0x180 };
// Sprite-graphics pack index (sprite_graphics_index) loaded for each of the 17 credit scenes
// (16 scenes + 1 extra entry for the Triforce-room finale).
static const uint8 kEnding_SpritePack[17] = {
  0x28, 0x46, 0x27, 0x2e, 0x2b, 0x2b, 0xe, 0x2c, 0x1a, 0x29, 0x47, 0x28, 0x27, 0x28, 0x2a, 0x28, 0x2d,
};
// Palette identifier passed to Overworld_LoadPalettes / GetDungPalInfo for each credit scene.
// High nibble encodes palette set; low nibble encodes palette slot within that set.
static const uint8 kEnding_SpritePal[17] = {
  1, 0x40, 1, 4, 1, 1, 1, 0x11, 1, 1, 0x47, 0x40, 1, 1, 1, 1, 1,
};
// Initializes hardware state for Module 00 (Title/Intro) before any animation begins.
// Called once on module entry. Disables NMI core updates, force-blanks the display,
// configures BG layer visibility (TM=16 → BG5 only; TS=0 → sub-screen off), loads background
// CHR halfslot 20 (title-screen tiles), triggers overworld music if not already playing,
// and pre-fills 17 CGRAM entries (palette_buffer[144..160]) with white (0x7fff) for the
// polyhedral shimmer, then clears the corresponding VRAM word rows at 0x27f0.
// Side effects: sets R16=0x1ffe, R18=0x1bfe as initial scroll positions for the background.
void Intro_SetupScreen() {  // 828000
  nmi_disable_core_updates = 0x80;
  EnableForceBlank();
  TM_copy = 16;
  TS_copy = 0;
  Intro_InitializeBackgroundSettings();
  CGWSEL_copy = 0x20;
  load_chr_halfslot_even_odd = 20;
  Graphics_LoadChrHalfSlot();
  load_chr_halfslot_even_odd = 0;
  LoadOWMusicIfNeeded();

  // why 17?
  for(int i = 0; i < 17; i++)
    main_palette_buffer[144 + i] = 0x7fff;

  for (int i = 0; i < 17; i++)
    g_zenv.vram[0x27f0 + i] = 0;

  R16 = 0x1ffe;
  R18 = 0x1bfe;
}

// Loads dialogue message pointer tables and all overworld background palettes for the title
// screen. Called during the intro initialization phase so that VWF text and palette data are
// ready before the first frame is rendered.
void Intro_LoadTextPointersAndPalettes() {  // 828116
  Text_GenerateMessagePointers();
  Overworld_LoadAllPalettes();
}

// Sub-phase 0 of 3 for loading an overworld credit scene (dispatched from kEndSequence0_Funcs).
// Sets up tilesets, palettes, and CHR data for the overworld area associated with the current
// credit scene (indexed by submodule_index >> 1). Special cases: scenes 6 and 15 use
// Overworld_EnterSpecialArea instead of LoadOverworldFromDungeon (non-standard map regions).
// Stops music and ambient sound, decompresses animated overworld tiles based on screen index
// (water = 0x58, land = 0x5a), and loads the correct sprite-pack palette from kEnding_SpritePack
// and kEnding_SpritePal. Transfers the font to VRAM only on the very first scene (submodule_index==0).
// Advances subsubmodule_index to trigger the next sub-phase on the following frame.
void Credits_LoadScene_Overworld_PrepGFX() {  // 828604
  EnableForceBlank();
  EraseTileMaps_normal();
  CGWSEL_copy = 0x82;
  int k = submodule_index >> 1;
  dungeon_room_index = kEnding_Tab1[k];

  // Scenes 6 (Light World special area) and 15 (Death Mountain special) use a separate loader.
  if (k != 6 && k != 15)
    LoadOverworldFromDungeon();
  else
    Overworld_EnterSpecialArea();
  music_control = 0;
  sound_effect_ambient = 0;

  // Water/animated tiles differ between dark-world screens (indices 3, 5, 7) and light-world.
  int t = BYTE(overworld_screen_index) & ~0x40;
  DecompressAnimatedOverworldTiles((t == 3 || t == 5 || t == 7) ? 0x58 : 0x5a);

  k = submodule_index >> 1;
  sprite_graphics_index = kEnding_SpritePack[k];
  uint8 sprpal = kEnding_SpritePal[k];
  InitializeTilesets();
  OverworldLoadScreensPaletteSet();
  Overworld_LoadPalettes(GetOverworldBgPalette(BYTE(overworld_screen_index)), sprpal);

  hud_palette = 1;
  Palette_Load_HUD();
  if (!submodule_index)
    TransferFontToVRAM();
  Overworld_LoadPalettesInner();
  Overworld_SetFixedColAndScroll();
  if (BYTE(overworld_screen_index) >= 128)
    Palette_SetOwBgColor();
  BGMODE_copy = 9;
  subsubmodule_index++;
}

// Sub-phase 1 of 3 for loading an overworld credit scene. Loads overlay data for the current
// screen, silences music and ambient sound again (in case they were re-triggered), and rolls
// submodule_index back by one (it was bumped to the ScrollScene entry; reverting makes the next
// call land back on LoadMap). Advances subsubmodule_index to trigger sub-phase 2.
void Credits_LoadScene_Overworld_Overlay() {  // 828697
  Overworld_LoadOverlays2();
  music_control = 0;
  sound_effect_ambient = 0;
  submodule_index--;
  subsubmodule_index++;
}

// Sub-phase 2 of 3 for loading an overworld credit scene. Builds the final tilemap for the
// screen and places all scene sprites (via Credits_PrepAndLoadSprites). Resets R16 and
// subsubmodule_index to 0 so the main scroll loop (Credits_ScrollScene_Overworld) runs next frame.
void Credits_LoadScene_Overworld_LoadMap() {  // 8286a5
  Overworld_LoadAndBuildScreen();
  Credits_PrepAndLoadSprites();
  R16 = 0;
  subsubmodule_index = 0;
}

// Per-frame scroll update for active overworld credit scenes. Drives the camera pan toward each
// scene's target position (Credits_HandleCameraScrollControl) and, when a screen boundary is
// crossed, triggers the overworld map scroll handler to load adjacent screen tiles.
void Credits_OperateScrollingAndTileMap() {  // 8286b3
  Credits_HandleCameraScrollControl();
  if (BYTE(overworld_screen_trans_dir_bits2))
    OverworldHandleMapScroll();
}

// Loads the decorative star/triangle background used during the final Triforce spinning overlay
// phase of the credits (EndSequence_32). Sets tile theme 33 + aux 59, sprite pack 45, palette
// for screen 0x5b, and initialises BG1/BG2 scroll to zero. Decrements submodule_index because
// this function is called one step early; the caller will re-increment it after the scene is ready.
void Credits_LoadCoolBackground() {  // 8286c0
  main_tile_theme_index = 33;
  aux_tile_theme_index = 59;
  sprite_graphics_index = 45;
  InitializeTilesets();
  BYTE(overworld_screen_index) = 0x5b;
  Overworld_LoadPalettes(GetOverworldBgPalette(BYTE(overworld_screen_index)), 0x13);
  overworld_palette_aux2_bp5to7_hi = 3;
  Palette_Load_OWBG2();
  Overworld_CopyPalettesToCache();
  Overworld_LoadOverlays2();
  BG1VOFS_copy2 = 0;
  BG1HOFS_copy2 = 0;
  submodule_index--;
}

// Loads and renders a dungeon credit scene. Called from Credits_LoadNextScene_Dungeon.
// Uses kEnding_Tab1[submodule_index >> 1] as the entrance/room index, loads the dungeon room
// (forced unlit: no torches, no lantern-dark flag), decompresses animated dungeon tiles from
// the animated tile table for the current tile theme, and configures sprite graphics and palette
// from kEnding_SpritePack / kEnding_SpritePal.  Does NOT advance submodule_index itself —
// the caller (Credits_LoadNextScene_Dungeon) handles the state machine transition.
void Credits_LoadScene_Dungeon() {  // 8286fd
  EnableForceBlank();
  EraseTileMaps_normal();
  WORD(which_entrance) = kEnding_Tab1[submodule_index >> 1];

  Dungeon_LoadEntrance();
  // Force all torches off and disable lantern-dark mode so the room is fully lit for the cinematic.
  dung_num_lit_torches = 0;
  hdr_dungeon_dark_with_lantern = 0;
  Dungeon_LoadAndDrawRoom();
  DecompressAnimatedDungeonTiles(kDungAnimatedTiles[main_tile_theme_index]);

  int i = submodule_index >> 1;
  sprite_graphics_index = kEnding_SpritePack[i];
  const DungPalInfo *dpi = GetDungPalInfo(kEnding_SpritePal[i] & 0x3f);
  palette_sp5l = dpi->pal2;
  palette_sp6l = dpi->pal3;
  misc_sprites_graphics_index = 10;
  InitializeTilesets();
  palette_sp6r_indoors = 10;
  Dungeon_LoadPalettes();
  BGMODE_copy = 9;
  R16 = 0;
  INIDISP_copy = 0;
  submodule_index++;
  Credits_PrepAndLoadSprites();
}

// Module 18 — GanonEmerges cutscene handler. Called every frame while this module is active.
// Manages a 9-phase state machine (overworld_map_state 0-8) that transitions from the final
// dungeon interior (Agahnim's tower) to the Pyramid of Power exterior where Ganon breaks free.
//
// Each frame: snaps BG1/BG2 scroll to the current player-relative offsets (bg1_x/y_offset)
// to keep background locked to the action, runs Sprite_Main for sprite logic, then restores the
// saved scroll to prevent drift across frames. Ends with LinkOam_Main for Link's OAM update.
//
// State machine phases:
//   0 GetBirdForPursuit       — summons the carrier bird, saves dungeon keys, immobilizes Link.
//   1 PrepForPyramidLocation  — waits for the fade-out (submodule_index==10) then switches to
//                               overworld screen 91 (Pyramid) via main_module_index=24.
//   2 FadeOutDungeonScreen    — decrements INIDISP until black, then force-blanks.
//   3 LoadPyramidArea         — loads the Pyramid overworld screen and cues music track 9.
//   4 LoadAmbientOverlay      — loads the overworld overlay and ambient map.
//   5 BrightenScreenThenSpawnBat — fades in (INIDISP++ to 15), then spawns the bat crash
//                               cutscene sprite and sets up the 128-frame bird-drop timer.
//   6 DelayForBatSmashIntoPyramid — idle phase while bat cutscene plays out.
//   7 DelayPlayerDropOff      — counts down subsubmodule_index to 0 before advancing.
//   8 DropOffPlayerAtPyramid  — hands control to BirdTravel_Finish_Doit to land Link.
void Module18_GanonEmerges() {  // 829edc
  uint16 hofs2 = BG2HOFS_copy2;
  uint16 vofs2 = BG2VOFS_copy2;
  uint16 hofs1 = BG1HOFS_copy2;
  uint16 vofs1 = BG1VOFS_copy2;

  // Apply player-relative scroll offsets for this frame so background tracks Link's position.
  BG2HOFS_copy2 = BG2HOFS_copy = hofs2 + bg1_x_offset;
  BG2VOFS_copy2 = BG2VOFS_copy = vofs2 + bg1_y_offset;
  BG1HOFS_copy2 = BG1HOFS_copy = hofs1 + bg1_x_offset;
  BG1VOFS_copy2 = BG1VOFS_copy = vofs1 + bg1_y_offset;
  Sprite_Main();
  // Restore saved scroll values so the accumulated camera position is not corrupted.
  BG1VOFS_copy2 = vofs1;
  BG1HOFS_copy2 = hofs1;
  BG2VOFS_copy2 = vofs2;
  BG2HOFS_copy2 = hofs2;

  switch (overworld_map_state) {
  case 0:  // GetBirdForPursuit
    Dungeon_HandleLayerEffect();
    CallForDuckIndoors();
    SaveDungeonKeys();
    overworld_map_state++;
    flag_is_link_immobilized++;
    break;
  case 1:  // PrepForPyramidLocation
    Dungeon_HandleLayerEffect();
    // submodule_index reaches 10 after the bird carry animation completes.
    if (submodule_index == 10) {
      overworld_screen_index = 91;
      player_is_indoors = 0;
      main_module_index = 24;
      submodule_index = 0;
      overworld_map_state = 2;
    }
    break;
  case 2:  // FadeOutDungeonScreen
    Dungeon_HandleLayerEffect();
    if (--INIDISP_copy)
      break;
    EnableForceBlank();
    overworld_map_state++;
    Hud_RebuildIndoor();
    link_x_vel = link_y_vel = 0;
    break;
  case 3:  // LOadPyramidArea
    birdtravel_var1[0] = 8;
    birdtravel_var1[1] = 0;
    FluteMenu_LoadSelectedScreen();
    LoadOWMusicIfNeeded();
    music_control = 9;
    break;
  case 4:  // LoadAmbientOverlay
    Overworld_LoadOverlayAndMap();
    subsubmodule_index = 0;
    break;
  case 5:  // BrightenScreenThenSpawnBat
    if (++INIDISP_copy == 15) {
      dung_savegame_state_bits = 0;
      flag_unk1 = 0;
      Sprite_SpawnBatCrashCutscene();
      link_direction_facing = 2;
      saved_module_for_menu = 9;
      player_is_indoors = 0;
      overworld_map_state++;
      // 128-frame countdown before dropping Link onto the pyramid.
      subsubmodule_index = 128;
      BYTE(cur_palace_index_x2) = 255;
    }
    break;
  case 6:  // DelayForBatSmashIntoPyramid
    break;
  case 7:  // DelayPlayerDropOff
    if (!--subsubmodule_index)
      overworld_map_state++;
    break;
  case 8:  // DropOffPlayerAtPyramid
    BirdTravel_Finish_Doit();
    break;
  }

  LinkOam_Main();
}

// Module 19 — TriforceRoom cutscene handler. Called every frame while this module is active.
// Drives a 15-phase state machine (subsubmodule_index 0-14) showing the three Triforce
// pieces converging, glowing, and fading out before the credits (Module 26) begin.
//
// Phases:
//   0  — Reset Link properties, stop music, transition reset.
//   1  — Mosaic fade in; palette filter bounce until fully visible.
//   2  — Force-blank, load credits songs, enter special area 0x189 (Triforce room).
//   3  — Load tile themes 36/81 and sprite pack 125, load area palettes.
//   4  — Load and advance the room tilemap (Module08_02), set up initial mosaic=240,
//         Link placed at (120, 236), start background music (track 32), → Module 25.
//   5  — Walk Link upward (direction 8) until Y < 192, then stop and advance.
//   6  — Reduce mosaic_level by 0x10 every two frames until 0; set BGMODE/MOSAIC.
//   7  — Prepare GFX slot for polyhedral, show dialogue 0x173 ("You have collected all
//         Triforce pieces!"), render text, enter Module 25 for text display.
//   8,10 — Advance polyhedral; on reaching phase 11 cue music 33 and bump submodule.
//   9   — Advance polyhedral and render text; on completion reset overworld state → Module 25.
//   11  — Advance polyhedral; run Link's approach-Triforce walk (TriforceRoom_LinkApproachTriforce).
//   12  — Advance polyhedral; count down R16 then trigger master-sword palette animation.
//   13  — Advance polyhedral; blinding-white palette flash until darkening_or_lightening_screen==255.
//   14  — Fade INIDISP to 0 (force-blank); then switch to Module 26 (Credits), disable IRQ
//         and polyhedral NMI thread, clear dark-world flag.
//
// After the switch: BG scroll copies are updated from the _copy2 registers, Link movement and
// animation run if not in a stationary phase (7-10), and LinkOam_Main draws Link's sprite.
void Module19_TriforceRoom() {  // 829fec
  switch (subsubmodule_index) {
  case 0:  //
    Link_ResetProperties_A();
    link_last_direction_moved_towards = 0;
    music_control = 0xf1;
    ResetTransitionPropsAndAdvance_ResetInterface();
    break;
  case 1:  //
    ConditionalMosaicControl();
    ApplyPaletteFilter_bounce();
    break;
  case 2:  //
    EnableForceBlank();
    LoadCreditsSongs();
    dungeon_room_index = 0x189;
    EraseTileMaps_normal();
    Palette_RevertTranslucencySwap();
    Overworld_EnterSpecialArea();
    Overworld_LoadOverlays2();
    subsubmodule_index++;
    main_module_index = 25;
    submodule_index = 0;
    break;
  case 3:  //
    main_tile_theme_index = 36;
    sprite_graphics_index = 125;
    aux_tile_theme_index = 81;
    InitializeTilesets();
    Overworld_LoadAreaPalettesEx(4);
    Overworld_LoadPalettes(14, 0);
    SpecialOverworld_CopyPalettesToCache();
    subsubmodule_index++;
    break;
  case 4: { //
    uint8 bak0 = subsubmodule_index;
    Module08_02_LoadAndAdvance();
    subsubmodule_index = bak0 + 1;
    INIDISP_copy = 15;
    palette_filter_countdown = 31;
    mosaic_target_level = 0;
    HIBYTE(BG1HOFS_copy2) = 1;
    CGWSEL_copy = 2;
    CGADSUB_copy = 50;
    mosaic_level = 240;
    BYTE(link_y_coord) = 236;
    BYTE(link_x_coord) = 120;
    link_is_on_lower_level = 2;
    music_control = 32;
    main_module_index = 25;
    submodule_index = 0;
    break;
  }
  case 5:  //
    link_direction = 8;
    link_direction_last = 8;
    link_direction_facing = 0;
    if (BYTE(link_y_coord) < 192) {
      link_direction = 0;
      link_direction_last = 0;
      link_animation_steps = 0;
      subsubmodule_index++;
    }
    break;
  case 6:  //
    if (!(palette_filter_countdown & 1) && mosaic_level != 0)
      mosaic_level -= 0x10;
    BGMODE_copy = 9;
    MOSAIC_copy = mosaic_level | 7;
    ApplyPaletteFilter_bounce();
    break;
  case 7:  //
    TriforceRoom_PrepGFXSlotForPoly();
    dialogue_message_index = 0x173;
    Main_ShowTextMessage();
    RenderText();
    BYTE(R16) = 0x80;
    main_module_index = 25;
    subsubmodule_index++;
    break;
  case 8:  //
  case 10:  //
    AdvancePolyhedral();
    if (subsubmodule_index == 11) {
      music_control = 33;
      main_module_index = 25;
      link_direction = 0;
      link_direction_last = 0;
      submodule_index++;
    }
    break;
  case 9:  //
    AdvancePolyhedral();
    RenderText();
    if (!submodule_index) {
      overworld_map_state = 0;
      main_module_index = 25;
      subsubmodule_index++;
    }
    break;
  case 11:  //
    AdvancePolyhedral();
    TriforceRoom_LinkApproachTriforce();
    if (subsubmodule_index == 12) {
      link_direction = 0;
      link_direction_last = 0;
    }
    break;
  case 12:  //
    AdvancePolyhedral();
    if (!--BYTE(R16)) {
      Palette_AnimGetMasterSword2();
      submodule_index++;
    }
    break;
  case 13:  //
    AdvancePolyhedral();
    PaletteFilter_BlindingWhiteTriforce();
    if (BYTE(darkening_or_lightening_screen) == 255)
      subsubmodule_index++;
    break;
  case 14:  //
    if (!--INIDISP_copy) {
      main_module_index = 26;
      submodule_index = 0;
      subsubmodule_index = 0;
      irq_flag = 255;
      is_nmi_thread_active = 0;
      nmi_flag_update_polyhedral = 0;
      savegame_is_darkworld = 0;
    }
    break;
  }
  BG1HOFS_copy = BG1HOFS_copy2;
  BG1VOFS_copy = BG1VOFS_copy2;
  BG2HOFS_copy = BG2HOFS_copy2;
  BG2VOFS_copy = BG2VOFS_copy2;
  if (subsubmodule_index < 7 || subsubmodule_index >= 11) {
    Link_HandleVelocity();
    Link_HandleMovingAnimation_FullLongEntry();
  }
  LinkOam_Main();
}

// Configures SNES BG hardware registers for the intro/ending screen layout:
//   BGMODE 9 = Mode 1 with BG3 priority (title uses BG1 for Zelda logo, BG3 for stars).
//   BG1SC=0x13 (32×32 at VRAM 0x4c00), BG2SC=3 (32×32 at 0x0c00), BG3SC=0x63 (64×32 at 0x18c00).
//   Fixed-colour registers set up for additive colour-math: sub-screen fixed colour
//   at (R=32, G=64, B=128) with CGADSUB=32 (add sub-screen to main).
void Intro_InitializeBackgroundSettings() {  // 82c500
  BGMODE_copy = 9;
  MOSAIC_copy = 0;
  zelda_ppu_write(BG1SC, 0x13);
  zelda_ppu_write(BG2SC, 3);
  zelda_ppu_write(BG3SC, 0x63);
  CGADSUB_copy = 32;
  COLDATA_copy0 = 32;
  COLDATA_copy1 = 64;
  COLDATA_copy2 = 128;
}

// Initialises the NMI polyhedral thread scratch area (kPolyThreadRam = g_ram+0x1f00).
// Zeroes the full 256-byte region, sets the thread's other-stack pointer to 0x1f31,
// then writes 13 initial state bytes starting at g_ram+0x1f32 (spin counters, palette index,
// rotation step). The NMI handler reads this region each vblank to produce the spinning
// geometry without requiring main-thread CPU time.
void Polyhedral_InitializeThread() {  // 89f7de
  static const uint8 kPolyThreadInit[13] = { 9, 0, 0x1f, 0, 0, 0, 0, 0, 0, 0x30, 0x1d, 0xf8, 9 };
  memset(kPolyThreadRam, 0, 256);
  thread_other_stack = 0x1f31;
  memcpy(&g_ram[0x1f32], kPolyThreadInit, 13);
}

// Module 00 — Title/Intro module dispatcher. Called every frame while on the title screen.
// Checks for player skip input (Start/Select/A/B ≥ phase 4 in enhanced mode, ≥ phase 8
// in classic mode) and jumps to FadeMusicAndResetSRAMMirror if triggered.
// Otherwise dispatches to one of 12 phase functions via submodule_index:
//   0  Intro_Init                   — first-frame hardware setup.
//   1  Intro_Init_Continue          — loads palettes and text pointers.
//   2,10 Intro_InitializeTriforcePolyThread — starts the NMI polyhedral thread.
//   3,4,9,11 Intro_HandleAllTriforceAnimations — drives 8 title-sprite slots each frame.
//   5  IntroZeldaFadein             — fades the Zelda logo in.
//   6  Intro_SwordComingDown        — animates the master sword falling and embedding.
//   7  Intro_FadeInBg               — brightens the background to full intensity.
//   8  Intro_WaitPlayer             — idles on the attract-mode loop waiting for input.
void Module00_Intro() {  // 8cc120
  // In enhanced mode the intro can be skipped from phase 4 onward; in classic mode from phase 8.
  uint8 skip_at = enhanced_features0 & kFeatures0_SkipIntroOnKeypress ? 4 : 8;

  if (submodule_index >= skip_at && ((filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0)) {
    FadeMusicAndResetSRAMMirror();
    return;
  }
  switch (submodule_index) {
  case 0: Intro_Init(); break;
  case 1: Intro_Init_Continue(); break;
  case 10:
  case 2: Intro_InitializeTriforcePolyThread(); break;
  case 3:
  case 4:
  case 9:
  case 11: Intro_HandleAllTriforceAnimations(); break;
  case 5: IntroZeldaFadein(); break;
  case 6: Intro_SwordComingDown(); break;
  case 7: Intro_FadeInBg(); break;
  case 8: Intro_WaitPlayer(); break;
  }
}

// First phase of Module 00. Calls Intro_SetupScreen to configure hardware, then enables full
// brightness (INIDISP=15), resets subsubmodule_index, triggers a CGRAM upload on the next NMI,
// plays sound effect 10 (the "ping" sparkle), advances to phase 1, and immediately falls through
// to Intro_Init_Continue to start loading palettes on the same frame.
void Intro_Init() {  // 8cc15d
  Intro_SetupScreen();
  INIDISP_copy = 15;
  subsubmodule_index = 0;
  flag_update_cgram_in_nmi++;
  submodule_index++;
  sound_effect_2 = 10;
  Intro_Init_Continue();
}

// Phase 1 of Module 00 — multi-frame WRAM and asset loading sequence.
// Renders the Zelda logo each frame (Intro_DisplayLogo), then processes one loading step
// per frame via subsubmodule_index (0-10):
//   0-7: Intro_Clear1kbBlocksOfWRAM — clears 15×1KB blocks of WRAM across 8 frames.
//   8:   Intro_LoadTextPointersAndPalettes — generates message pointers and overworld palettes.
//   9:   LoadItemGFXIntoWRAM4BPPBuffer — decompresses item graphics into WRAM.
//   10:  LoadFollowerGraphics — loads companion (fairy, etc.) CHR data.
// After step 10, each subsequent frame decrements INIDISP_copy until 0, then calls
// Intro_InitializeMemory_darken to prepare the darkened background for the Triforce spin.
void Intro_Init_Continue() {  // 8cc170
  Intro_DisplayLogo();
  int t = subsubmodule_index++;
  if (t >= 11) {
    if (--INIDISP_copy)
      return;
    Intro_InitializeMemory_darken();
    return;
  }
  switch (t) {
  case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
    Intro_Clear1kbBlocksOfWRAM();
    break;
  case 8: Intro_LoadTextPointersAndPalettes(); break;
  case 9: LoadItemGFXIntoWRAM4BPPBuffer(); break;
  case 10:LoadFollowerGraphics(); break;
  }
}

// Clears a 1 KB slice of WRAM across all 15 banks that mirror it (0x2000..0x1e000 in 0x2000
// increments). R16 is the current offset within the 1 KB block (starts at 0x1ffe, steps by -2),
// R18 is the end marker for this frame (R16 - 0x400). Called 8 times over 8 frames to wipe
// the full 8 KB working area used by the tilemap and sprite-attribute tables.
void Intro_Clear1kbBlocksOfWRAM() {  // 8cc1a0
  uint16 i = R16;
  uint8 *dst = (uint8 *)&g_ram[0x2000];
  do {
    for (int j = 0; j < 15; j++)
      WORD(dst[i + j * 0x2000]) = 0;
  } while ((i -= 2) != R18);
  R16 = i;
  R18 = i - 0x400;
}

// Prepares the darkened title background before the Triforce polyhedral begins spinning.
// Force-blanks and erases tilemaps, loads tile theme 35 (Hyrule Castle interior BG),
// sprite pack 125, aux theme 81, and misc sprites pack 8. Decompresses animated dungeon tiles
// from slot 0x5d (castle animated water). Sets a brightening palette filter (direction=2,
// countdown=31, mosaic target=0) so the background will fade in over the next 31 frames.
// Advances to the Triforce-poly phase (submodule_index++).
void Intro_InitializeMemory_darken() {  // 8cc1f5
  EnableForceBlank();
  EraseTileMaps_normal();
  main_tile_theme_index = 35;
  sprite_graphics_index = 125;
  aux_tile_theme_index = 81;
  misc_sprites_graphics_index = 8;
  LoadDefaultGraphics();
  InitializeTilesets();
  DecompressAnimatedDungeonTiles(0x5d);
  bg_tile_animation_countdown = 2;
  BYTE(overworld_screen_index) = 0;
  palette_main_indoors = 0;
  overworld_palette_aux3_bp7_lo = 0;
  R16 = 0;
  R18 = 0;
  darkening_or_lightening_screen = 2;
  palette_filter_countdown = 31;
  mosaic_target_level = 0;
  submodule_index++;
}

// Phase 5 of Module 00 — fades the Zelda logo and castle background in over 31 frames.
// Each odd frame advances the palette filter one step (Palette_FadeIntroOneStep). At countdown
// 13, enables BG layers 1, 3, and 4 (TM=0x15) so the Zelda text logo becomes visible.
// When countdown reaches 0 the fade is complete: sets subsubmodule_index=42 (the sword hang
// timer), advances to phase 6, and calls Intro_SetupSwordAndIntroFlash to start the sword drop.
void IntroZeldaFadein() {  // 8cc25c
  Intro_HandleAllTriforceAnimations();
  if (!(frame_counter & 1))
    return;
  Palette_FadeIntroOneStep();
  if (BYTE(palette_filter_countdown) == 0) {
    // Fade complete: arm the sword-drop sequence and switch to the next phase.
    subsubmodule_index = 42;
    submodule_index++;
    Intro_SetupSwordAndIntroFlash();
  } else if (BYTE(palette_filter_countdown) == 13) {
    // At this point enough palette rows have loaded to safely show the Zelda logo BG layers.
    TM_copy = 0x15;
    TS_copy = 0;
  }
}

// Phase 7 of Module 00 — brightens the background palette to full intensity after the sword
// has embedded itself. Animates the sword flash every frame (Intro_PeriodicSwordAndIntroFlash)
// and continues running the Triforce sprite animations. While palette_filter_countdown > 0,
// advances the fade on odd frames (Palette_FadeIntro2). Once fully bright, counts down
// subsubmodule_index each frame; when it reaches 0 advances to phase 8 (Intro_WaitPlayer).
// Also checks for skip input (Start/Select/A/B) and bails to the menu if pressed.
void Intro_FadeInBg() {  // 8cc284
  Intro_PeriodicSwordAndIntroFlash();
  Intro_HandleAllTriforceAnimations();
  if (BYTE(palette_filter_countdown)) {
    if (frame_counter & 1)
      Palette_FadeIntro2();
  } else {
    if ((filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0)
      FadeMusicAndResetSRAMMirror();
    else {
      if (!--subsubmodule_index)
        submodule_index++;
    }
  }
}

// Phase 6 of Module 00 — the master sword falls from the top of the screen and embeds in
// the pedestal. The polyhedral NMI thread is paused (intro_did_run_step=0, is_nmi_thread_active=0)
// so the Triforce stops spinning. Calls Intro_PeriodicSwordAndIntroFlash each frame to animate
// the sword OAM. Counts down subsubmodule_index; when it reaches 0 configures colour-math
// (CGWSEL=2, CGADSUB=0x22 = subtract sub-screen from main) and starts the 31-frame background
// fade-in (palette_filter_countdown=31) before moving to phase 7.
void Intro_SwordComingDown() {  // 8cc2ae
  Intro_HandleAllTriforceAnimations();
  intro_did_run_step = 0;
  is_nmi_thread_active = 0;
  Intro_PeriodicSwordAndIntroFlash();
  if (!--subsubmodule_index) {
    submodule_index++;
    CGWSEL_copy = 2;
    CGADSUB_copy = 0x22;
    palette_filter_countdown = 31;
    TS_copy = 2;
  }
}

// Phase 8 of Module 00 — attract-mode idle. Continues animating Triforce sprites and sword
// flash while pausing the polyhedral thread. Counts down subsubmodule_index; when it reaches 0
// transitions to Module 20 (the title-screen attract demo) and resets Link's X coordinate to 0.
void Intro_WaitPlayer() {  // 8cc2d4
  Intro_HandleAllTriforceAnimations();
  intro_did_run_step = 0;
  is_nmi_thread_active = 0;
  Intro_PeriodicSwordAndIntroFlash();
  if (!--subsubmodule_index) {
    submodule_index++;
    main_module_index = 20;
    submodule_index = 0;
    BYTE(link_x_coord) = 0;
  }
}

// Handles the player pressing Start (or a skip button) during the intro/title screen.
// Disables IRQ (irq_flag=255), restores BG layer visibility, stops music (0xf1 = force stop),
// clears the backdrop to black, zeroes the Link state block and all dungeon-save mirrors,
// then transitions to Module 1 (file select) with death_var4=1 to indicate a fresh start.
void FadeMusicAndResetSRAMMirror() {  // 8cc2f0
  irq_flag = 255;
  TM_copy = 0x15;
  TS_copy = 0;
  player_is_indoors = 0;
  music_control = 0xf1;
  SetBackdropcolorBlack();

  memset(&link_y_coord, 0, 0x70);
  memset(save_dung_info, 0, 256 * 5);

  main_module_index = 1;
  death_var4 = 1;
  submodule_index = 0;
}

// Phase 2 (and 10) of Module 00 — starts the polyhedral Triforce NMI thread and activates
// the first four intro sprite slots (0-2: type 0 sparkling stars; 4: type 2 portrait reveal).
// Calls Intro_InitGfx_Helper which zeroes the thread area, loads the Triforce palette, sets
// IRQ scan-line trigger to 0x90, and configures the polyhedral rotation parameters
// (poly_a=0xA0, poly_b=0x60, model=1=Triforce). Advances to the animation phase.
void Intro_InitializeTriforcePolyThread() {  // 8cc33c
  misc_sprites_graphics_index = 8;
  LoadCommonSprites();
  Intro_InitGfx_Helper();
  // Activate star-sparkle sprites in slots 0, 1, 2 (type 0).
  intro_sprite_isinited[0] = 1;
  intro_sprite_isinited[1] = 1;
  intro_sprite_isinited[2] = 1;
  intro_sprite_subtype[0] = 0;
  intro_sprite_subtype[1] = 0;
  intro_sprite_subtype[2] = 0;
  // Activate the maiden portrait sprite in slot 4 (type 2).
  intro_sprite_isinited[4] = 1;
  intro_sprite_subtype[4] = 2;
  INIDISP_copy = 15;
  submodule_index++;
}

// Shared initializer for the polyhedral NMI thread used by both the title screen and the
// Triforce room. Clears the 256-byte thread area, loads the 8-entry Triforce shimmer palette
// into CGRAM slot 0xd0 (kPolyhedralPalette), sets the IRQ scan-line to 0x90 (midscreen),
// configures poly_which_model=1 (Triforce geometry), sets rotation angles (a=0xA0, b=0x60),
// enables the NMI thread (is_nmi_thread_active=1), and zeroes all 7×16 intro sprite state arrays.
void Intro_InitGfx_Helper() {  // 8cc36f
  Polyhedral_InitializeThread();
  LoadTriforceSpritePalette();
  virq_trigger = 0x90;
  poly_config1 = 255;
  poly_base_x = 32;
  poly_base_y = 32;
  BYTE(poly_var1) = 32;
  poly_a = 0xA0;
  poly_b = 0x60;
  poly_config_color_mode = 1;
  poly_which_model = 1;
  is_nmi_thread_active = 1;
  intro_did_run_step = 1;
  // Zero all 7 intro sprite state arrays (each 16 bytes: isinited, subtype, x, y, xvel, yvel, step).
  memset(&intro_step_index, 0, 7 * 16);
}

// Copies the 8-entry kPolyhedralPalette shimmer table into the main palette buffer at offset
// 0xd0 (CGRAM word 104, palette row 13) and marks CGRAM for upload on the next NMI. This
// palette is what makes the spinning Triforce geometry appear in rainbow-shimmer colours.
void LoadTriforceSpritePalette() {  // 8cc3bd
  memcpy(main_palette_buffer + 0xd0, kPolyhedralPalette, 16);
  flag_update_cgram_in_nmi++;
}

// Per-frame driver for all title-screen animated elements. Increments the intro frame counter,
// then delegates to Intro_AnimateTriforce (which runs the 8 intro sprite slots) followed by
// Intro_RunStep (which advances the polyhedral NMI thread animation). Called from every phase
// that keeps the Triforce spinning (phases 3, 4, 5, 7, 9, 11).
void Intro_HandleAllTriforceAnimations() {  // 8cc404
  intro_frame_ctr++;
  Intro_AnimateTriforce();
  Scene_AnimateEverySprite();
}

// Iterates all 8 intro sprite slots (indices 7 down to 0) and calls Intro_AnimOneObj for each.
// Resets intro_sprite_alloc to 0x800 (the OAM base word offset) before the loop so that each
// frame's sprite draw starts from a clean allocation pointer.
void Scene_AnimateEverySprite() {  // 8cc412
  intro_sprite_alloc = 0x800;
  for (int k = 7; k >= 0; k--)
    Intro_AnimOneObj(k);
}

// Ensures the polyhedral NMI thread runs exactly once per frame. Sets is_nmi_thread_active=1
// to keep the thread ticking, then calls Intro_RunStep only if intro_did_run_step is 0
// (preventing double-advance if called from multiple phases in the same frame).
void Intro_AnimateTriforce() {  // 8cc435
  is_nmi_thread_active = 1;
  if (!intro_did_run_step) {
    Intro_RunStep();
    intro_did_run_step = 1;
  }
}

// Drives the 5-step polyhedral animation state machine for the title screen.
// Each step controls how the Triforce geometry spins and fades:
//   Step 0 (64 frames): Spin up — increment poly_b by 5 and poly_a by 3 each frame until timer=64.
//   Step 1: Wind down — decrement poly_config1 by 2 each frame, spinning down.
//           At poly_config1<225 switch to submodule_index=4 (overlay phase).
//           At poly_config1==113 start the overworld music (music_control=1).
//           When poly_config1 reaches 0, transition to step 2 with 64-frame hold timer.
//   Step 2 (64 frames): Hold — keep spinning (poly_b+=5, a+=3) for 64 frames then advance.
//   Step 3: Accelerate to max — keep incrementing until poly_b≥250 and poly_a≥252.
//           When reached, lock in step 4 with 32-frame countdown.
//   Step 4 (32 frames): Lock to 0 — zero both rotation angles, wait 32 frames, then activate
//           slot 5 (type 3 sparkle), switch to sub-screen blend mode, and advance to the
//           Zelda-logo fade phase (submodule_index++).
void Intro_RunStep() {  // 8cc448
  switch (intro_step_index) {
  case 0:
    if (++intro_step_timer == 64)
      intro_step_index++;
    poly_b += 5, poly_a += 3;
    break;
  case 1:
    if (poly_config1 < 2) {
      poly_config1 = 0;
      intro_step_index++;
      intro_step_timer = 64;
      return;
    }
    poly_config1 -= 2;
    poly_b += 5;
    poly_a += 3;
    if (poly_config1 < 225)
      submodule_index = 4;
    if (poly_config1 == 113)
      music_control = 1;
    break;
  case 2:
    if (!--intro_step_timer) {
      intro_step_index++;
    } else {
      poly_b += 5, poly_a += 3;
    }
    break;
  case 3:
    if (poly_b >= 250 && poly_a >= 252) {
      intro_step_index++;
      intro_step_timer = 32;
    } else {
      poly_b += 5, poly_a += 3;
    }
    break;
  case 4:
    poly_b = 0;
    poly_a = 0;
    if (!--intro_step_timer) {
      intro_step_index++;
      intro_sprite_isinited[5] = 1;
      intro_sprite_subtype[5] = 3;
      TM_copy = 0x10;
      TS_copy = 5;
      CGWSEL_copy = 2;
      CGADSUB_copy = 0x31;
      subsubmodule_index = 0;
      flag_update_cgram_in_nmi++;
      nmi_load_bg_from_vram = 3;
      submodule_index++;
    }
    break;
  }

}

// Dispatches one intro/ending sprite slot (index k) based on its isinited and subtype fields.
// intro_sprite_isinited[k] drives the lifecycle:
//   0 = inactive (skip).
//   1 = first frame (initialisation call — sets position, velocity, subtype-specific state).
//   2 = active (per-frame animation call — draws OAM entries and updates position).
// intro_sprite_subtype[k] selects the sprite role:
//   0 = Triforce triangle (sparkling star in intro / Triforce piece in TriforceRoom).
//   1 = unused / exit animation.
//   2 = Copyright/maiden portrait.
//   3 = Sparkle / flash effect.
//   4,5,6 = Triforce room triangle variants (left, top, right pieces).
//   7 = Credits Triforce triangle (used during the final endgame scroll).
void Intro_AnimOneObj(int k) {  // 8cc534
  switch (intro_sprite_isinited[k]) {
  case 0:
    break;
  case 1:
    switch (intro_sprite_subtype[k]) {
    case 0: Intro_SpriteType_A_0(k); break;
    case 1: EXIT_0CCA90(k); break;
    case 2: InitializeSceneSprite_Copyright(k); break;
    case 3: InitializeSceneSprite_Sparkle(k); break;
    case 4:
    case 5:
    case 6: InitializeSceneSprite_TriforceRoomTriangle(k); break;
    case 7: InitializeSceneSprite_CreditsTriangle(k); break;
    }
    break;
  case 2:
    switch (intro_sprite_subtype[k]) {
    case 0: Intro_SpriteType_B_0(k); break;
    case 1: EXIT_0CCA90(k); break;
    case 2: AnimateSceneSprite_Copyright(k); break;
    case 3: AnimateSceneSprite_Sparkle(k); break;
    case 4:
    case 5:
    case 6: Intro_SpriteType_B_456(k); break;
    case 7: AnimateSceneSprite_CreditsTriangle(k); break;
    }
    break;
  }
}

// Initialisation (isinited=1) for intro sprite type 0 — the three Triforce triangle sprites.
// Sets the starting screen position from kIntroSprite0_X/Y[k] (left=-38, top=95, right=230
// pixels for X; 200, -67, 200 for Y) and initial velocity from kIntroSprite0_Xvel/Yvel.
// After setup, advances isinited to 2 so animation begins next frame.
void Intro_SpriteType_A_0(int k) {  // 8cc57e
  static const int16 kIntroSprite0_X[3] = { -38, 95, 230 };
  static const int16 kIntroSprite0_Y[3] = { 200, -67, 200 };
  intro_x_lo[k] = kIntroSprite0_X[k];
  intro_x_hi[k] = kIntroSprite0_X[k] >> 8;
  intro_y_lo[k] = kIntroSprite0_Y[k];
  intro_y_hi[k] = kIntroSprite0_Y[k] >> 8;
  intro_x_vel[k] = kIntroSprite0_Xvel[k];
  intro_y_vel[k] = kIntroSprite0_Yvel[k];
  intro_sprite_isinited[k]++;
}

// Per-frame animation (isinited=2) for intro sprite type 0 — draws and moves one Triforce triangle.
// Draws the 16-OAM-entry triangle via AnimateSceneSprite_DrawTriangle, then moves it one pixel
// per frame via AnimateSceneSprite_MoveTriangle. When not in step 5 (convergence), the velocity
// is boosted every 32 frames by kIntroSprite0_Xvel/Yvel, and clamped to zero once the sprite
// reaches its final position (kIntroSprite0_XLimit/YLimit). In step 5 the velocity is zeroed
// so the three Triforce pieces hold their assembled position.
void Intro_SpriteType_B_0(int k) {  // 8cc5b1
  static const uint8 kIntroSprite0_XLimit[3] = { 75, 95, 117 };
  static const uint8 kIntroSprite0_YLimit[3] = { 88, 48, 88 };

  AnimateSceneSprite_DrawTriangle(k);
  AnimateSceneSprite_MoveTriangle(k);
  if (intro_step_index != 5) {
    // Accelerate toward the target position every 32 frames.
    if (!(intro_frame_ctr & 31)) {
      intro_x_vel[k] += kIntroSprite0_Xvel[k];
      intro_y_vel[k] += kIntroSprite0_Yvel[k];
    }
    // Clamp velocity to zero once the sprite reaches its assembled target.
    if (intro_x_lo[k] == kIntroSprite0_XLimit[k])
      intro_x_vel[k] = 0;
    if (intro_y_lo[k] == kIntroSprite0_YLimit[k])
      intro_y_vel[k] = 0;
  } else {
    // In the held-assembled state, freeze all movement.
    intro_x_vel[k] = 0;
    intro_y_vel[k] = 0;
  }
}

// Draws one Triforce triangle sprite (slot k) as 16 16×16 OAM entries arranged in a 4×4 grid
// (64×64 pixels total). Left-oriented triangles use kIntroSprite0_Left_Ents; right-oriented
// (horizontally mirrored) use kIntroSprite0_Right_Ents. The IntroSpriteEnt fields are:
//   {dx, dy, tile_index, attribute_byte, size_flag}
// Tile indices 0x80..0xAE address the Triforce CHR data; attribute 0x1b = palette 3, priority 1
// no flip; 0x5b = palette 3, priority 1, H-flip for the right triangle. Slot index determines
// orientation: slot 0 = left, slot 1 = top (right-mirrored), slot 2 = right.
void AnimateSceneSprite_DrawTriangle(int k) {  // 8cc70f
  // Left-facing Triforce triangle: 16 tiles in reading order, no horizontal flip.
  static const IntroSpriteEnt kIntroSprite0_Left_Ents[16] = {
    { 0,  0, 0x80, 0x1b, 2},
    {16,  0, 0x82, 0x1b, 2},
    {32,  0, 0x84, 0x1b, 2},
    {48,  0, 0x86, 0x1b, 2},
    { 0, 16, 0xa0, 0x1b, 2},
    {16, 16, 0xa2, 0x1b, 2},
    {32, 16, 0xa4, 0x1b, 2},
    {48, 16, 0xa6, 0x1b, 2},
    { 0, 32, 0x88, 0x1b, 2},
    {16, 32, 0x8a, 0x1b, 2},
    {32, 32, 0x8c, 0x1b, 2},
    {48, 32, 0x8e, 0x1b, 2},
    { 0, 48, 0xa8, 0x1b, 2},
    {16, 48, 0xaa, 0x1b, 2},
    {32, 48, 0xac, 0x1b, 2},
    {48, 48, 0xae, 0x1b, 2},
  };
  // Right-facing Triforce triangle: same tiles but columns are reversed (H-flip via attribute 0x5b).
  static const IntroSpriteEnt kIntroSprite0_Right_Ents[16] = {
    {48,  0, 0x80, 0x5b, 2},
    {32,  0, 0x82, 0x5b, 2},
    {16,  0, 0x84, 0x5b, 2},
    { 0,  0, 0x86, 0x5b, 2},
    {48, 16, 0xa0, 0x5b, 2},
    {32, 16, 0xa2, 0x5b, 2},
    {16, 16, 0xa4, 0x5b, 2},
    { 0, 16, 0xa6, 0x5b, 2},
    {48, 32, 0x88, 0x5b, 2},
    {32, 32, 0x8a, 0x5b, 2},
    {16, 32, 0x8c, 0x5b, 2},
    { 0, 32, 0x8e, 0x5b, 2},
    {48, 48, 0xa8, 0x5b, 2},
    {32, 48, 0xaa, 0x5b, 2},
    {16, 48, 0xac, 0x5b, 2},
    { 0, 48, 0xae, 0x5b, 2},
  };
  AnimateSceneSprite_AddObjectsToOamBuffer(k, k == 2 ? kIntroSprite0_Right_Ents : kIntroSprite0_Left_Ents, 16);
}

// Draws a TriforceRoom-variant triangle (subtypes 4/5/6) as 16 OAM entries. Uses palette 2
// (attribute 0x2b = palette 2, priority 1, no flip; 0x6b = palette 2, priority 1, H-flip).
// Slot k==2 gets the right-facing (H-flipped) table; all others get the left-facing table.
// This variant differs from AnimateSceneSprite_DrawTriangle in palette selection (2 vs 3).
void Intro_CopySpriteType4ToOam(int k) {  // 8cc82f
  // Left-facing Triforce triangle for TriforceRoom: attribute 0x2b = palette 2, priority 1.
  static const IntroSpriteEnt kIntroTriforceOam_Left[16] = {
    { 0,  0, 0x80, 0x2b, 2},
    {16,  0, 0x82, 0x2b, 2},
    {32,  0, 0x84, 0x2b, 2},
    {48,  0, 0x86, 0x2b, 2},
    { 0, 16, 0xa0, 0x2b, 2},
    {16, 16, 0xa2, 0x2b, 2},
    {32, 16, 0xa4, 0x2b, 2},
    {48, 16, 0xa6, 0x2b, 2},
    { 0, 32, 0x88, 0x2b, 2},
    {16, 32, 0x8a, 0x2b, 2},
    {32, 32, 0x8c, 0x2b, 2},
    {48, 32, 0x8e, 0x2b, 2},
    { 0, 48, 0xa8, 0x2b, 2},
    {16, 48, 0xaa, 0x2b, 2},
    {32, 48, 0xac, 0x2b, 2},
    {48, 48, 0xae, 0x2b, 2},
  };
  static const IntroSpriteEnt kIntroTriforceOam_Right[16] = {
    {48,  0, 0x80, 0x6b, 2},
    {32,  0, 0x82, 0x6b, 2},
    {16,  0, 0x84, 0x6b, 2},
    { 0,  0, 0x86, 0x6b, 2},
    {48, 16, 0xa0, 0x6b, 2},
    {32, 16, 0xa2, 0x6b, 2},
    {16, 16, 0xa4, 0x6b, 2},
    { 0, 16, 0xa6, 0x6b, 2},
    {48, 32, 0x88, 0x6b, 2},
    {32, 32, 0x8a, 0x6b, 2},
    {16, 32, 0x8c, 0x6b, 2},
    { 0, 32, 0x8e, 0x6b, 2},
    {48, 48, 0xa8, 0x6b, 2},
    {32, 48, 0xaa, 0x6b, 2},
    {16, 48, 0xac, 0x6b, 2},
    { 0, 48, 0xae, 0x6b, 2},
  };
  AnimateSceneSprite_AddObjectsToOamBuffer(k, k == 2 ? kIntroTriforceOam_Right : kIntroTriforceOam_Left, 16);
}

// No-op stub for sprite subtype 1 (unused exit animation slot). Exists as a dispatch target
// in Intro_AnimOneObj so the subtype table is complete, but contains no logic.
void EXIT_0CCA90(int k) {  // 8cc84f
  // empty
}

// Initialisation for sprite type 2 — the Nintendo copyright notice.
// Positions the sprite at screen (76, 184) — horizontally centred near the bottom — and
// advances isinited to 2 so AnimateSceneSprite_Copyright runs next frame.
void InitializeSceneSprite_Copyright(int k) {  // 8cc850
  intro_x_lo[k] = 76;
  intro_x_hi[k] = 0;
  intro_y_lo[k] = 184;
  intro_y_hi[k] = 0;
  intro_sprite_isinited[k]++;
}

// Per-frame draw for sprite type 2 — the Nintendo copyright notice. Submits 13 8×8 OAM entries
// forming the "© 1991 Nintendo" text, using tiles 0x40-0x54 at palette 2 (attribute 0x0a).
void AnimateSceneSprite_Copyright(int k) {  // 8cc864
  // 13 glyph tiles spaced 8 pixels apart, all on row 0, palette 2, priority 1.
  static const IntroSpriteEnt kIntroSprite2_Ents[13] = {
    { 0, 0, 0x40, 0x0a, 0},
    { 8, 0, 0x41, 0x0a, 0},
    {16, 0, 0x42, 0x0a, 0},
    {24, 0, 0x68, 0x0a, 0},
    {32, 0, 0x41, 0x0a, 0},
    {40, 0, 0x42, 0x0a, 0},
    {48, 0, 0x43, 0x0a, 0},
    {56, 0, 0x44, 0x0a, 0},
    {64, 0, 0x50, 0x0a, 0},
    {72, 0, 0x51, 0x0a, 0},
    {80, 0, 0x52, 0x0a, 0},
    {88, 0, 0x53, 0x0a, 0},
    {96, 0, 0x54, 0x0a, 0},
  };
  AnimateSceneSprite_AddObjectsToOamBuffer(k, kIntroSprite2_Ents, 13);
}

// Initialisation for sprite type 3 — the maiden portrait sparkle.
// Selects the starting portrait position from kIntroSprite3_X/Y based on the current 2-bit
// phase derived from intro_frame_ctr bits 5-4. Advances isinited to 2.
void InitializeSceneSprite_Sparkle(int k) {  // 8cc8e2
  int j = intro_frame_ctr >> 5 & 3;
  intro_x_lo[k] = kIntroSprite3_X[j];
  intro_x_hi[k] = 0;
  intro_y_lo[k] = kIntroSprite3_Y[j];
  intro_y_hi[k] = 0;
  intro_sprite_isinited[k]++;
}

// Per-frame animation for sprite type 3 — the glinting sparkle on the maiden portraits.
// Draws one of four OAM entries (selected by intro_sprite_state[k]) from kIntroSprite3_Ents,
// which cycle through: blank(0x80), small flash(0xb7), large flash(0x64/palette 3+4), glow(0x62).
// Advances the animation frame via kIntroSprite3_State (indexed by frame_counter bits 4-2, cycling 0-7)
// and moves the sparkle to the portrait position for this phase (kIntroSprite3_X/Y[frame>>5&3]).
void AnimateSceneSprite_Sparkle(int k) {  // 8cc90d
  // Four animation frames: blank tile, small star, large star (palette 4), large star (palette 3).
  static const IntroSpriteEnt kIntroSprite3_Ents[4] = {
    { 0,  0, 0x80, 0x34, 0},
    { 0,  0, 0xb7, 0x34, 0},
    {-4, -3, 0x64, 0x38, 2},
    {-4, -3, 0x62, 0x34, 2},
  };
  if (intro_sprite_state[k] < 4)
    AnimateSceneSprite_AddObjectsToOamBuffer(k, kIntroSprite3_Ents + intro_sprite_state[k], 1);

  // Cycle to the next animation frame from the state sequence table.
  intro_sprite_state[k] = kIntroSprite3_State[intro_frame_ctr >> 2 & 7];
  // Move sparkle to the portrait position for the current phase.
  int j = intro_frame_ctr >> 5 & 3;
  intro_x_lo[k] = kIntroSprite3_X[j];
  intro_y_lo[k] = kIntroSprite3_Y[j];
}

// Appends `num` OAM entries to the shared OAM buffer (g_ram[intro_sprite_alloc]).
// Each entry is placed at the sprite's world position (intro_x/y[k]) plus the relative
// dx/dy from the IntroSpriteEnt table. intro_sprite_alloc advances by 4 bytes per entry.
// SetOamHelper0 packs the x, y, tile, attribute, and size flag into the OAM word format.
void AnimateSceneSprite_AddObjectsToOamBuffer(int k, const IntroSpriteEnt *src, int num) {  // 8cc972
  uint16 x = intro_x_hi[k] << 8 | intro_x_lo[k];
  uint16 y = intro_y_hi[k] << 8 | intro_y_lo[k];
  OamEnt *oam = (OamEnt *)&g_ram[intro_sprite_alloc];
  intro_sprite_alloc += num * 4;
  do {
    SetOamHelper0(oam, x + src->x, y + src->y, src->charnum, src->flags, src->ext);
  } while (oam++, src++, --num);
}

// Applies sub-pixel velocity to a triangle sprite's position in both axes.
// Velocity is a signed 8-bit value shifted left 4 bits (giving 1/16-pixel precision per frame).
// The full 32-bit accumulator (subpixel | lo<<8 | hi<<16) is updated atomically so fractional
// pixels are not lost between frames.
void AnimateSceneSprite_MoveTriangle(int k) {  // 8cc9f1
  if (intro_x_vel[k] != 0) {
    uint32 t = intro_x_subpixel[k] + (intro_x_lo[k] << 8) + (intro_x_hi[k] << 16) + ((int8)intro_x_vel[k] << 4);
    intro_x_subpixel[k] = t, intro_x_lo[k] = t >> 8, intro_x_hi[k] = t >> 16;
  }
  if (intro_y_vel[k] != 0) {
    uint32 t = intro_y_subpixel[k] + (intro_y_lo[k] << 8) + (intro_y_hi[k] << 16) + ((int8)intro_y_vel[k] << 4);
    intro_y_subpixel[k] = t, intro_y_lo[k] = t >> 8, intro_y_hi[k] = t >> 16;
  }
}

// Prepares the GFX slot and NMI polyhedral thread for the Triforce room convergence sequence.
// Activates slots 0-2 with subtypes 4, 5, 6 (left/top/right Triforce pieces in their room-variant
// positions) and enables full brightness. Advances submodule_index to start the TriforceRoom poly loop.
void TriforceRoom_PrepGFXSlotForPoly() {  // 8cca54
  misc_sprites_graphics_index = 8;
  LoadCommonSprites();
  Intro_InitGfx_Helper();
  intro_sprite_isinited[0] = 1;
  intro_sprite_isinited[1] = 1;
  intro_sprite_isinited[2] = 1;
  intro_sprite_subtype[0] = 4;  // Left Triforce piece
  intro_sprite_subtype[1] = 5;  // Top Triforce piece
  intro_sprite_subtype[2] = 6;  // Right Triforce piece
  INIDISP_copy = 15;
  submodule_index++;
}

// Prepares the GFX slot and NMI polyhedral thread for the credits ending Triforce display.
// Identical to TriforceRoom_PrepGFXSlotForPoly except all three slots use subtype 7
// (AnimateSceneSprite_CreditsTriangle) and poly_config1 is forced to 0 (max-spin from the start).
void Credits_InitializePolyhedral() {  // 8cca81
  misc_sprites_graphics_index = 8;
  LoadCommonSprites();
  Intro_InitGfx_Helper();
  // Start the polyhedral at full rotation speed (config1=0 = no wind-down).
  poly_config1 = 0;
  intro_sprite_isinited[0] = 1;
  intro_sprite_isinited[1] = 1;
  intro_sprite_isinited[2] = 1;
  intro_sprite_subtype[0] = 7;  // Credits Triforce triangle variant
  intro_sprite_subtype[1] = 7;
  intro_sprite_subtype[2] = 7;
  INIDISP_copy = 15;
  submodule_index++;
}

// Runs both the Triforce-room polyhedral logic and the 8-slot sprite animation for one frame.
// Called from Module 19 phases 8-13 (AdvancePolyhedral loop) each frame the Triforce is active.
void AdvancePolyhedral() {  // 8ccab1
  TriforceRoom_HandlePoly();
  Scene_AnimateEverySprite();
}

// Controls the polyhedral Triforce rotation for Module 19 (TriforceRoom). Called once per frame
// by AdvancePolyhedral. Sets intro_want_double_ret=1 so sprite type 4/5/6 handlers know the
// poly thread is managing motion themselves and skip their own move logic. Guards against
// double-advance with intro_did_run_step. Drives a 5-step convergence animation:
//
//   Step 0: Wind down — decrement poly_config1 by 2 each frame; when it reaches 0, advance to
//           step 1 and bump subsubmodule_index. Falls through to step 1 logic immediately.
//   Step 1: Slow spin — increment poly_b by 2 and poly_a by 1. When subsubmodule_index≥10,
//           bump to step 2 and set intro_y_vel[1]=5 (top piece starts dropping).
//   Step 2: Glow up — increment poly_config1 toward 128 (geometry brightens). Once ≥128, spin
//           faster and wait for poly_b and poly_a to align at their convergence angles. On
//           convergence: zero angles, play sound 44, set palette 0xd7 to white (7fff), start
//           6-frame timer.
//   Step 3: Flash hold — countdown timer; when zero revert palette 0xd7 to kPolyhedralPalette[7].
//   Step 4: Done — idle; Module19 advances subsubmodule_index from the outside.
void TriforceRoom_HandlePoly() {  // 8ccabc
  is_nmi_thread_active = 1;
  intro_want_double_ret = 1;
  if (intro_did_run_step)
    return;
  switch (intro_step_index) {
  case 0:
    poly_config1 -= 2;
    if (poly_config1 < 2) {
      poly_config1 = 0;
      intro_step_index++;
      subsubmodule_index++;
    }
    // fall through
  case 1:
    // Once enough Module19 phases have elapsed, start the top piece falling and spin up.
    if (subsubmodule_index >= 10) {
      intro_step_index++;
      intro_y_vel[1] = 5;
    }
    poly_b += 2, poly_a += 1;
    break;
  case 2:
    triforce_ctr = 0x1c0;
    if (poly_config1 < 128) {
      // Ramp up the glow level until the geometry is at full brightness.
      poly_config1 += 1;
    } else {
      // Check for convergence angles: both rotation axes must be near their locked positions.
      if ((poly_b - 10 & 0x7f) >= 92 &&
          (uint8)(poly_a - 11) >= 220) {
        // Lock the Triforce in place, flash white, then restore the shimmer palette after 6 frames.
        poly_a = 0;
        poly_b = 0;
        subsubmodule_index++;
        intro_step_index++;
        sound_effect_1 = 44;
        main_palette_buffer[0xd7] = 0x7fff;
        flag_update_cgram_in_nmi++;
        intro_step_timer = 6;
        break;
      }
    }
    poly_b += 5, poly_a += 3;
    break;
  case 3:
    if (!--intro_step_timer) {
      main_palette_buffer[0xd7] = kPolyhedralPalette[7];
      flag_update_cgram_in_nmi++;
      intro_step_index++;
    }
    break;
  case 4:
    break;
  }
  intro_did_run_step = 1;
  intro_want_double_ret = 0;
  intro_frame_ctr++;
}

// Per-frame animation driver for the credits Triforce triangles (subtypes 4/5/6). Advances the
// frame counter and polyhedral NMI thread, then updates all 8 sprite slots.  The polyhedral
// rotates at a constant rate (poly_b+=3, poly_a+=1) — unlike the title-screen's variable-speed
// Intro_RunStep. Called from credits phases 33 (BrightenTriangles) through 38 (HangForever).
void Credits_AnimateTheTriangles() {  // 8ccba2
  intro_frame_ctr++;
  is_nmi_thread_active = 1;
  if (!intro_did_run_step) {
    poly_b += 3;
    poly_a += 1;
    intro_did_run_step = 1;
  }
  Scene_AnimateEverySprite();
}

// Initialisation for sprite subtypes 4/5/6 — the three Triforce pieces in the TriforceRoom.
// Places them at equal Y (0x9c) and staggered X (0x4e/0x5f/0x72 for left/center/right), with
// initial velocities that spread them outward (-2/0/+2 X, +4/-4/+4 Y). Advances isinited to 2.
void InitializeSceneSprite_TriforceRoomTriangle(int k) {  // 8ccbe8
  static const int16 kIntroTriforce_X[3] = { 0x4e, 0x5f, 0x72 };
  static const int16 kIntroTriforce_Y[3] = { 0x9c, 0x9c, 0x9c };
  static const int8 kIntroTriforce_Xvel[3] = { -2, 0, 2 };
  static const int8 kIntroTriforce_Yvel[3] = { 4, -4, 4 };

  intro_x_lo[k] = kIntroTriforce_X[k];
  intro_x_hi[k] = 0;
  intro_y_lo[k] = kIntroTriforce_Y[k];
  intro_y_hi[k] = 0;
  intro_x_vel[k] = kIntroTriforce_Xvel[k];
  intro_y_vel[k] = kIntroTriforce_Yvel[k];
  intro_sprite_isinited[k]++;
}

// Per-frame animation for TriforceRoom sprite subtypes 4/5/6. Draws the triangle (Intro_CopySpriteType4ToOam),
// then (if TriforceRoom_HandlePoly hasn't set intro_want_double_ret) moves the piece and updates
// its velocity according to the current convergence step:
//   Step 0: Drift outward — accelerate X/Y by kTriforce_Xacc/Yacc every 8/4 frames.
//   Step 1: Freeze — zero velocity (piece is locked in the spread position).
//   Step 2: Converge — call AnimateTriforceRoomTriangle_HandleContracting every 4 frames to steer
//           toward kTriforce_Xfinal/Yfinal; clamp velocity when target is reached.
//   Steps 3,4: Settle — wait for triforce_ctr to expire, then lock Y to kTriforce_Yfinal2[k].
void Intro_SpriteType_B_456(int k) {  // 8ccc13
  static const int8 kTriforce_Xacc[3] = { -1, 0, 1 };
  static const int8 kTriforce_Yacc[3] = { -1, -1, -1 };
  static const uint8 kTriforce_Yfinal2[3] = { 0x72, 0x66, 0x72 };

  Intro_CopySpriteType4ToOam(k);
  // TriforceRoom_HandlePoly sets this flag when it has already moved the pieces; skip own movement.
  if (intro_want_double_ret)
    return;
  AnimateSceneSprite_MoveTriangle(k);
  switch (intro_step_index) {
  case 0:
    if (!(intro_frame_ctr & 7))
      intro_x_vel[k] += kTriforce_Xacc[k];
    if (!(intro_frame_ctr & 3))
      intro_y_vel[k] += kTriforce_Yacc[k];
    break;
  case 1:
    intro_x_vel[k] = 0;
    intro_y_vel[k] = 0;
    break;
  case 2:
    // Steer toward the assembly target position every 4 frames.
    if (!(intro_frame_ctr & 3))
      AnimateTriforceRoomTriangle_HandleContracting(k);
    if (kTriforce_Xfinal[k] == intro_x_lo[k])
      intro_x_vel[k] = 0;
    if (kTriforce_Yfinal[k] == intro_y_lo[k])
      intro_y_vel[k] = 0;
    break;
  case 3:
  case 4:
    // After convergence counter expires, snap Y to its final settled position.
    if (triforce_ctr == 0) {
      intro_y_lo[k] = kTriforce_Yfinal2[k];
    } else {
      triforce_ctr -= 1;
    }
    break;
  }
}

// Adjusts the X and Y velocity of a converging Triforce piece toward kTriforce_Xfinal/Yfinal.
// Increments or decrements each velocity by 1 depending on whether the piece is to the left or
// right of its target. Clamps the result at ±0x10 (±16 sub-pixels/frame) to prevent overshoot.
void AnimateTriforceRoomTriangle_HandleContracting(int k) {  // 8cccb0
  uint8 new_vel = intro_x_vel[k] + (intro_x_lo[k] <= kTriforce_Xfinal[k] ? 1 : -1);
  intro_x_vel[k] = (new_vel == 0x11) ? 0x10 : (new_vel == 0xef) ? 0xf0 : new_vel;
  new_vel = intro_y_vel[k] + (intro_y_lo[k] <= kTriforce_Yfinal[k] ? 1 : -1);
  intro_y_vel[k] = (new_vel == 0x11) ? 0x10 : (new_vel == 0xef) ? 0xf0 : new_vel;
}

// Initialisation for sprite subtype 7 — the credits-ending Triforce triangle.
// Positions the three triangles at their fixed screen locations:
//   k=0 (left):  X=0x29, Y=0x70
//   k=1 (top):   X=0x5f, Y=0x20
//   k=2 (right): X=0x97, Y=0x70
// Advances isinited to 2 so AnimateSceneSprite_CreditsTriangle runs next frame.
void InitializeSceneSprite_CreditsTriangle(int k) {  // 8ccd19
  static const uint8 kIntroSprite7_X[3] = { 0x29, 0x5f, 0x97 };
  static const uint8 kIntroSprite7_Y[3] = { 0x70, 0x20, 0x70 };
  intro_x_lo[k] = kIntroSprite7_X[k];
  intro_x_hi[k] = 0;
  intro_y_lo[k] = kIntroSprite7_Y[k];
  intro_y_hi[k] = 0;
  intro_sprite_isinited[k]++;
}

// Per-frame animation for sprite subtype 7 — the credits dispersal Triforce triangles.
// Each frame: reloads the shimmer palette, draws the triangle OAM via Intro_CopySpriteType4ToOam,
// and moves the piece via AnimateSceneSprite_MoveTriangle. While submodule_index != 36 (Credits
// disperse phase, kEndSequence_Funcs index 36 = Credits_FadeAndDisperseTriangles), the sprite
// holds its position (state reset to 0). Once phase 36 is reached, an 80-frame counter drives
// progressive velocity boost via kIntroSprite7_XAcc/YAcc, causing the triangles to fly apart.
void AnimateSceneSprite_CreditsTriangle(int k) {  // 8ccd3e
  static const int8 kIntroSprite7_XAcc[3] = { -1, 0, 1 };
  static const int8 kIntroSprite7_YAcc[3] = { 1, -1, 1 };

  LoadTriforceSpritePalette();
  Intro_CopySpriteType4ToOam(k);
  AnimateSceneSprite_MoveTriangle(k);
  if (submodule_index != 36) {
    // Not yet in the disperse phase; hold position and reset state.
    intro_sprite_state[k] = 0;
    return;
  }
  if (intro_sprite_state[k] != 80) {
    // Incrementally accelerate each piece outward over 80 frames.
    intro_sprite_state[k]++;
    intro_x_vel[k] += kIntroSprite7_XAcc[k];
    intro_y_vel[k] += kIntroSprite7_YAcc[k];
  }
}

// Draws four 16×16 OAM entries forming the "A LINK TO THE PAST" / "Zelda" logo text on the
// title screen. Uses tiles 0x69, 0x6b, 0x6d, 0x6e (the logo glyphs) at palette 3, priority 2,
// placed at Y=0x68 (lower quarter of screen). Called every frame from Intro_Init_Continue.
void Intro_DisplayLogo() {  // 8ced82
  static const uint8 kIntroLogo_X[4] = { 0x60, 0x70, 0x80, 0x88 };
  static const uint8 kIntroLogo_Tile[4] = { 0x69, 0x6b, 0x6d, 0x6e };
  for (int i = 0; i < 4; i++)
    SetOamPlain(&oam_buf[i], kIntroLogo_X[i], 0x68, kIntroLogo_Tile[i], 0x32, 2);
}

// Arms the master sword fall sequence. Sets the sword to its off-screen starting Y (-130),
// resets animation state bytes (19=7, 20=0, 21=0), then calls Intro_PeriodicSwordAndIntroFlash
// to immediately render the first frame with the sword hidden above the screen.
void Intro_SetupSwordAndIntroFlash() {  // 8cfe45
  intro_sword_19 = 7;
  intro_sword_20 = 0;
  intro_sword_21 = 0;
  intro_sword_ypos = -130;

  Intro_PeriodicSwordAndIntroFlash();
}

// Per-frame renderer for the falling master sword and screen flash effect.
// Decrements the inter-frame delay timer (intro_sword_18), clears the backdrop to black,
// then (if intro_times_pal_flash > 0) cycles the fixed-colour registers R/G/B each frame
// to produce a white flash (or a dimmer flash in enhanced mode). The flash cycles through
// COLDATA_copy0/1/2 in sequence via intro_sword_24.
//
// Draws the sword as 10 OAM entries (kIntroSword_Char, X, Y tables) clipped to the screen.
// Advances intro_sword_ypos by +16 each frame until it reaches 30 (embedded in pedestal).
// Triggers sound effects at ypos=0xffbe (whoosh) and ypos=14 (clang on impact with flash).
// After embedding, the animation proceeds through phases via intro_sword_20:
//   Phase 0: Wait for flash to finish.
//   Phase 1: Sparkle on the hilt using kSwordSparkle_Char cycling 7 frames.
//   Phase 2: Light beam rising from the pedestal using kIntroSwordSparkle_Char.
void Intro_PeriodicSwordAndIntroFlash() {  // 8cfe56
  if (intro_sword_18)
    intro_sword_18--;
  SetBackdropcolorBlack();
  if (intro_times_pal_flash) {
    if ((intro_times_pal_flash & 3) != 0) {
      (&COLDATA_copy0)[intro_sword_24] |= (enhanced_features0 & kFeatures0_DimFlashes) ? 0x05 : 0x1f;
      intro_sword_24 = (intro_sword_24 == 2) ? 0 : intro_sword_24 + 1;
    }
    intro_times_pal_flash--;
  }
  OamEnt *oam = oam_buf + 0x52;
  for (int j = 9; j >= 0; j--) {
    static const uint8 kIntroSword_Char[10] = { 0, 2, 0x20, 0x22, 4, 6, 8, 0xa, 0xc, 0xe };
    static const uint8 kIntroSword_X[10] = { 0x40, 0x40, 0x30, 0x50, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40 };
    static const uint16 kIntroSword_Y[10] = { 0x10, 0x20, 0x28, 0x28, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
    uint16 y = intro_sword_ypos + kIntroSword_Y[j];
    SetOamPlain(&oam[j], kIntroSword_X[j], ((y & 0xff00) ? 0xf8 : y) - 8, kIntroSword_Char[j], 0x21, 2);
  }

  if (intro_sword_ypos != 30) {
    if (intro_sword_ypos == 0xffbe) {
      sound_effect_1 = 1;
    } else if (intro_sword_ypos == 14) {
      WORD(intro_sword_24) = 0;
      intro_times_pal_flash = 0x20;
      sound_effect_1 = 0x2c;
    }
    intro_sword_ypos += 16;
  }

  switch (intro_sword_20 >> 1) {
  case 0:
    if (!intro_times_pal_flash && intro_sword_ypos == 30)
      intro_sword_20 += 2;
    break;
  case 1: {
    static const uint8 kSwordSparkle_Tab[8] = { 4, 4, 6, 6, 6, 4, 4 };

    if (!intro_sword_18) {
      intro_sword_19 -= 1;
      if (sign8(intro_sword_19)) {
        intro_sword_19 = 0;
        intro_sword_18 = 2;
        intro_sword_20 += 2;
        return;
      }
      intro_sword_18 = kSwordSparkle_Tab[intro_sword_19];
    }
    static const uint8 kSwordSparkle_Char[7] = { 0x28, 0x37, 0x27, 0x36, 0x27, 0x37, 0x28 };
    SetOamPlain(&oam_buf[0x50], 0x44, 0x43, kSwordSparkle_Char[intro_sword_19], 0x25, 0);
    break;
  }
  case 2: {
    static const uint8 kIntroSwordSparkle_Char[8] = { 0x26, 0x20, 0x24, 0x34, 0x25, 0x20, 0x35, 0x20 };
    int k = intro_sword_19;
    if (k >= 7)
      return;
    uint8 y = (intro_sword_21 < 0x50 ? intro_sword_21 : 0x4f) + intro_sword_ypos + 0x31;
    SetOamPlain(&oam_buf[0x50], 0x42, y + 0, kIntroSwordSparkle_Char[k + 0], 0x23, 0);
    SetOamPlain(&oam_buf[0x51], 0x42, y + 8, kIntroSwordSparkle_Char[k + 1], 0x23, 0);
    if (intro_sword_18 == 0) {
      intro_sword_21 += 4;
      if (intro_sword_21 == 0x4 || intro_sword_21 == 0x48 || intro_sword_21 == 0x4c || intro_sword_21 == 0x58)
        intro_sword_19 += 2;
    }
    break;
  }
  }
}

// Module 1A — Credits / EndSequence dispatcher. Called every frame while the credits roll.
// Sets OAM region bases (region 0 at 0x30, region 1 at 0x1d0, region 2 at 0x00) to partition
// OAM between the polyhedral triangles, credit sprites, and UI elements. Dispatches to one of
// 39 functions from kEndSequence_Funcs via submodule_index.
void Module1A_Credits() {  // 8e986e
  oam_region_base[0] = 0x30;
  oam_region_base[1] = 0x1d0;
  oam_region_base[2] = 0x0;

  kEndSequence_Funcs[submodule_index]();
}

// Dispatcher for even-numbered credit phases (overworld LoadNextScene entries, indices 0,4,6,...).
// Dispatches to one of 3 sub-phases via kEndSequence0_Funcs[subsubmodule_index] (PrepGFX,
// Overlay, LoadMap), then writes the attribution text for this scene into the VRAM upload queue
// via Credits_AddEndingSequenceText.
void Credits_LoadNextScene_Overworld() {  // 8e9889
  kEndSequence0_Funcs[subsubmodule_index]();
  Credits_AddEndingSequenceText();
}

// Dispatcher for even-numbered dungeon credit phases (indices 2, 20, 22).
// Loads the dungeon scene for the current credit index, then writes attribution text into
// the VRAM upload queue.
void Credits_LoadNextScene_Dungeon() {  // 8e9891
  Credits_LoadScene_Dungeon();
  Credits_AddEndingSequenceText();
}

// Resets all 16 sprite slots and then places scene-specific sprites for the current credit scene
// (determined by submodule_index >> 1). Each scene uses a distinct range of entries from
// kEndingSprites_X/Y (via kEndingSprites_Idx) and may have scene-specific velocity or delay
// overrides applied before the generic placement loop runs.
//
// World-space positions are computed by adding the sprite's X/Y offset to the overworld area
// origin derived from overworld_area_index (for overworld scenes) or dungeon_room_index2
// (for dungeon scenes). sprcoll_x/y_size is set to 0xffff (no collision) for all credit sprites
// since they are purely decorative.
void Credits_PrepAndLoadSprites() {  // 8e98b9
  for (int k = 15; k >= 0; k--) {
    SpritePrep_ResetProperties(k);
    sprite_state[k] = 0;
    sprite_flags5[k] = 0;
    sprite_defl_bits[k] = 0;
  }
  int k = submodule_index >> 1;
  switch (k) {
init_sprites_0:
  case 0: case 4: case 5: case 8: case 13: {
    int idx = kEndingSprites_Idx[k];
    int num = kEndingSprites_Idx[k + 1] - idx;
    const uint16 *px = kEndingSprites_X + idx;
    const uint16 *py = kEndingSprites_Y + idx;
    for (k = num - 1; k >= 0; k--) {
      sprcoll_x_size = sprcoll_y_size = 0xffff;
      uint16 x = (swap16(overworld_area_index << 1) & 0xf00) + px[k];
      uint16 y = (swap16(overworld_area_index >> 2) & 0xe00) + py[k];
      Sprite_SetX(k, x);
      Sprite_SetY(k, y);
    }
    break;
  }
init_sprites_1:
  case 1: {
    int idx = kEndingSprites_Idx[k];
    int num = kEndingSprites_Idx[k + 1] - idx;
    const uint16 *px = kEndingSprites_X + idx;
    const uint16 *py = kEndingSprites_Y + idx;
    byte_7E0FB1 = dungeon_room_index2 >> 3 & 254;
    byte_7E0FB0 = (dungeon_room_index2 & 15) << 1;
    for (k = num - 1; k >= 0; k--) {
      sprcoll_x_size = sprcoll_y_size = 0xffff;
      uint16 x = byte_7E0FB0 * 256 + px[k];
      uint16 y = byte_7E0FB1 * 256 + py[k];
      Sprite_SetX(k, x);
      Sprite_SetY(k, y);
    }
    break;
  }
  case 2:
    sprite_y_vel[6] = -16;
    goto init_sprites_0;
  case 3:
    sprite_A[5] = 22;
    sprite_y_vel[0] = -16;
    sprite_y_vel[1] = 16;
    sprite_head_dir[1] = 1;
    for (int j = 2; j >= 0; j--) {
      sprite_type[2 + j] = 0x57;
      sprite_oam_flags[2 + j] = 0x31;
    }
    goto init_sprites_0;
  case 6:
    sprite_delay_main[0] = 255;
    sprite_delay_main[1] = 255;
    sprite_delay_main[2] = 255;
    goto init_sprites_0;
  case 7:
    sprite_delay_main[1] = 255;
    goto init_sprites_0;
  case 9:
    for (int j = 4; j >= 0; j--) {
      sprite_delay_main[j] = j * 19;
      sprite_state[j] = 0;
    }
    sprite_type[5] = 0x2e;
    for (int j = 1; j >= 0; j--) {
      sprite_type[7 + j] = 0x9f;
      sprite_type[9 + j] = 0xa0;
      sprite_flags2[7 + j] = 1;
      sprite_flags2[9 + j] = 2;
      sprite_flags3[7 + j] = 0x10;
      sprite_flags3[9 + j] = 0x10;
    }
    goto init_sprites_0;
  case 10:
    sprite_delay_main[1] = 0x10;
    sprite_delay_main[2] = 0x20;
    sprite_oam_flags[3] = 8;
    sprite_oam_flags[4] = 8;
    goto init_sprites_1;
  case 11:
    sprite_oam_flags[4] = 0x79;
    sprite_oam_flags[5] = 0x39;
    sprite_D[1] = 1;
    sprite_A[1] = 4;
    goto init_sprites_1;
  case 12:
    for (int j = 1; j >= 0; j--) {
      sprite_oam_flags[j + 3] = 0x39;
      sprite_type[j + 3] = 0xb;
      sprite_flags3[j + 3] = 0x10;
      sprite_flags2[j + 3] = 1;
    }
    sprite_type[5] = 0x2a;
    sprite_type[6] = 0x79;
    sprite_ai_state[6] = 1;
    sprite_z[6] = 5;
    goto init_sprites_0;
  case 14:
    sprite_y_vel[5] = -16;
    sprite_y_vel[6] = 16;
    sprite_head_dir[6] = 1;
    sprite_A[0] = 8;
    for (int j = 3; j >= 0; j--)
      sprite_y_vel[1 + j] = 4;
    goto init_sprites_0;
  case 15:
    sprite_C[4] = 2;
    sprite_y_vel[5] = 8;
    sprite_delay_main[1] = 0x13;
    sprite_delay_main[4] = 0x40;
    goto init_sprites_0;
  }
}

// Per-frame scroll handler for overworld credit scenes (odd-indexed kEndSequence_Funcs entries
// 1, 5, 7, 9, 11, 13, 15, 17, 19, 25, 27, 29, 31). Ticks down sprite delay timers, then once
// R16≥0x40 and on even frames, sets link_x/y_vel to the scene's scroll velocity from
// kEnding1_Xvel/Yvel. These velocities are consumed by Credits_OperateScrollingAndTileMap which
// translates them into BG scroll updates and screen-transition checks via OverworldHandleMapScroll.
// Ends with Credits_HandleSceneFade to animate the scene's sprites.
void Credits_ScrollScene_Overworld() {  // 8e9958

  for (int k = 15; k >= 0; k--)
    if (sprite_delay_main[k])
      sprite_delay_main[k]--;

  int i = submodule_index >> 1;

  // Zero velocity first so that once the target scroll position is reached the camera stops.
  link_x_vel = link_y_vel = 0;
  if (R16 >= 0x40 && !(R16 & 1)) {
    if (BG2VOFS_copy2 != kEnding1_TargetScrollY[i])
      link_y_vel = kEnding1_Yvel[i];
    if (BG2HOFS_copy2 != kEnding1_TargetScrollX[i])
      link_x_vel = kEnding1_Xvel[i];
  }

  Credits_OperateScrollingAndTileMap();
  Credits_HandleSceneFade();
}

// Per-frame scroll handler for dungeon credit scenes (odd-indexed kEndSequence_Funcs entries
// 3, 21, 23). Ticks down sprite delay timers, then once R16≥0x40 and on even frames, nudges
// BG2HOFS_copy2/VOFS_copy2 by the scene's scroll velocity until the target position is reached.
// Unlike the overworld version, dungeon scrolling drives BG2 directly (no link_x/y_vel indirection).
void Credits_ScrollScene_Dungeon() {  // 8e99c5
  for (int k = 15; k >= 0; k--)
    if (sprite_delay_main[k])
      sprite_delay_main[k]--;

  int i = submodule_index >> 1;
  if (R16 >= 0x40 && !(R16 & 1)) {
    if (BG2VOFS_copy2 != kEnding1_TargetScrollY[i])
      BG2VOFS_copy2 += kEnding1_Yvel[i];
    if (BG2HOFS_copy2 != kEnding1_TargetScrollX[i])
      BG2HOFS_copy2 += kEnding1_Xvel[i];
  }
  Credits_HandleSceneFade();
}

// Per-frame sprite animation dispatcher for all 16 credit scenes. Called from both
// Credits_ScrollScene_Overworld and Credits_ScrollScene_Dungeon. Selects the current scene
// index (i = submodule_index >> 1) and runs that scene's sprite logic. Each case handles
// the specific NPCs, birds, Agahnim, villagers, Link, and other characters that appear in
// that scene, including movement, animation frame cycling, shadow drawing, and OAM submission
// via Credits_SpriteDraw_Single/PreexistingSpriteDraw. Also manages the scene fade-to-next:
// kEnding1_3_Tab0[i] gives the scroll threshold at which the scene begins fading out and
// advancing submodule_index to the next LoadNextScene entry.
void Credits_HandleSceneFade() {  // 8e9a2a
  // Scroll threshold at which each scene's fade-out begins and submodule_index advances.
  static const uint16 kEnding1_3_Tab0[16] = { 0x300, 0x280, 0x250, 0x2e0, 0x280, 0x250, 0x2c0, 0x2c0, 0x250, 0x250, 0x280, 0x250, 0x480, 0x400, 0x250, 0x500 };
  // OAM attribute bytes for each sprite in scene 0 (the 12 soldiers on Kakariko road).
  static const uint8 kEndSequence_Case0_Tab1[12] = { 0x1e, 0x20, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x16, 0x16, 0x16, 0x16 };
  // Tile offsets within the sprite pack for each sprite in scene 0.
  static const uint8 kEndSequence_Case0_Tab0[12] = { 6, 3, 2, 2, 2, 2, 2, 2, 6, 6, 6, 6 };
  // OAM attribute flags for each of the 12 sprites in scene 0.
  static const uint8 kEndSequence_Case0_OamFlags[12] = { 0x3b, 0x31, 0x3d, 0x3f, 0x39, 0x3b, 0x37, 0x3d, 0x39, 0x37, 0x37, 0x39 };
  int i = submodule_index >> 1, j, k;

  switch (i) {
  case 0:
    for (int k = 11; k != 7; k--) {
      sprite_oam_flags[k] = kEndSequence_Case0_OamFlags[k];
      Credits_SpriteDraw_Single(k, kEndSequence_Case0_Tab0[k], kEndSequence_Case0_Tab1[k]);
    }
    for (int k = 7; k != 1; k--) {
      sprite_oam_flags[k] = kEndSequence_Case0_OamFlags[k] | (frame_counter << 2 & 0x40);
      Credits_SpriteDraw_Single(k, kEndSequence_Case0_Tab0[k], kEndSequence_Case0_Tab1[k]);
    }
    for (int k = 1; k >= 0; k--) {
      sprite_oam_flags[k] = kEndSequence_Case0_OamFlags[k];
      Credits_SpriteDraw_Single(k, kEndSequence_Case0_Tab0[k], kEndSequence_Case0_Tab1[k]);
    }
    break;
  case 1:
    Credits_SpriteDraw_Single(0, 3, 12);
    Credits_SpriteDraw_DrawShadow(0);
    k = 1;
    sprite_type[k] = 0x73;
    sprite_oam_flags[k] = 0x27;
    sprite_E[k] = 2;
    Credits_SpriteDraw_PreexistingSpriteDraw(k, 16);
    break;
  case 2: {
    static const uint8 kEnding_Case2_Tab0[2] = { 0x20, 0x40 };
    static const int8 kEnding_Case2_Tab1[2] = { 16, -16 };
    static const int8 kEnding_Case2_Tab2[5] = { 0x28, 0x2a, 0x2c, 0x2e, 0x2c };
    static const int8 kEnding_Case2_Tab3[5] = { 3, 3, 3, 3, 3 };
    static const uint8 kEnding_Case2_Delay[2] = { 0x30, 0x10 };

    BYTE(flag_travel_bird) = kEnding_Case2_Tab0[frame_counter >> 2 & 1];
    k = 6;
    j = sprite_x_vel[k] >> 7 & 1;
    sprite_oam_flags[k] = (sprite_x_vel[k] + kEnding_Case2_Tab1[j]) >> 1 & 0x40 | 0x32;
    Credits_SpriteDraw_Single(k, 2, 0x24);
    Credits_SpriteDraw_CirclingBirds(k);
    k -= 1;
    sprite_oam_flags[k] = 0x31;
    if (!sprite_delay_main[k]) {
      j = sprite_A[k];
      sprite_A[k] ^= 1;
      sprite_delay_main[k] = kEnding_Case2_Delay[j];
      sprite_graphics[k] = sprite_graphics[k] + 1 & 3;
    }
    Credits_SpriteDraw_Single(k, 2, 0x26);
    k -= 1;
    do {
      if (!(frame_counter & 15))
        sprite_graphics[k] ^= 1;
      sprite_oam_flags[k] = 0x31;
      Credits_SpriteDraw_Single(k, kEnding_Case2_Tab3[k], kEnding_Case2_Tab2[k]);
      EndSequence_DrawShadow2(k);
    } while (--k >= 0);
    break;
  }
  case 3: {
    static const uint8 kEnding_Case3_Gfx[4] = { 1, 2, 3, 2 };
    for (k = 0; k < 5; k++) {
      if (k < 2) {
        sprite_type[k] = 1;
        sprite_oam_flags[k] = 0xb;
        Credits_SpriteDraw_SetShadowProp(k, 2);
        sprite_z[k] = 48;
        j = (frame_counter + (k ? 0x5f : 0x7d)) >> 2 & 3;
        sprite_graphics[k] = kEnding_Case3_Gfx[j];
        Credits_SpriteDraw_CirclingBirds(k);
        Credits_SpriteDraw_PreexistingSpriteDraw(k, 12);
      } else {
        Credits_SpriteDraw_PreexistingSpriteDraw(k, 16);
      }
    }
    Credits_SpriteDraw_Single(k, 2, 0x38);
    Ending_Func2(k, 0x30);
    k++;
    Credits_SpriteDraw_Single(k, 3, 0x3a);
    break;
  }
  case 4: {
    static const uint8 kEnding_Case4_Tab1[2] = { 0x30, 0x32 };
    static const uint8 kEnding_Case4_Tab0[2] = { 2, 2 };
    static const uint8 kEnding_Case4_Ctr[2] = { 0x20, 0 };
    static const int8 kEnding_Case4_XYvel[10] = { 0, -12, -16, -12, 0, 12, 16, 12, 0, -12 };
    static const uint8 kEnding_Case4_DelayVel[24] = {
      0x3b, 0x14, 0x1e, 0x1d, 0x2c, 0x2b, 0x42, 0x20, 0x27, 0x28, 0x2e, 0x38, 0x3a, 0x4c, 0x32, 0x44,
      0x2e, 0x2f, 0x1e, 0x28, 0x47, 0x35, 0x32, 0x30,
    };
    k = 2;
    sprite_oam_flags[k] = 0x35;
    Credits_SpriteDraw_Single(k, 1, 0x3c);
    k--;
    do {
      sprite_oam_flags[k] = (sprite_x_vel[k] - 1) >> 1 & 0x40 ^ 0x71;
      sprite_graphics[k] = frame_counter >> 3 & 1;
      if (R16 >= kEnding_Case4_Ctr[k] && !sprite_delay_main[k]) {
        uint8 a = kEnding_Case4_DelayVel[sprite_A[k]];
        sprite_delay_main[k] = a & 0xf8;
        sprite_y_vel[k] = kEnding_Case4_XYvel[(a & 7) + 2];
        sprite_x_vel[k] = kEnding_Case4_XYvel[a & 7];
        sprite_A[k]++;
      }
      Credits_SpriteDraw_Single(k, kEnding_Case4_Tab0[k], kEnding_Case4_Tab1[k]);
      EndSequence_DrawShadow2(k);
      Sprite_MoveXY(k);
    } while (--k >= 0);
    break;
  }
  case 5: {
    static const uint8 kEnding_Case5_Tab0[2] = { 0, 4 };
    static const uint16 kEnding_Case5_Tab1[2] = { 0xa, 0x224 };
    static const uint8 kEnding_Case5_Tab2[2] = { 10, 14 };
    if (R16 == 0x200)
      sound_effect_1 = 1;
    else if (R16 == 0x208)
      sound_effect_1 = 0x2c;
    if ((uint16)(R16 - 0x208) < 0x30)
      Credits_SpriteDraw_AddSparkle(2, 10, R16 - 0x208); // wtf x,y
    k = 3;
    if (R16 >= 0x200)
      sprite_graphics[k] = 1;
    sprite_oam_flags[k] = 0x31;
    Credits_SpriteDraw_Single(k, 4, 8);
    EndSequence_DrawShadow2(k);
    int j = sprite_graphics[k];
    sprite_graphics[--k] = j;
    link_dma_var3 = 0;
    link_dma_var4 = kEnding_Case5_Tab0[j];
    sprite_oam_flags[k] = 0x30;

    link_dma_graphics_index = kEnding_Case5_Tab1[j];
    Credits_SpriteDraw_Single(k, 5, kEnding_Case5_Tab2[j]);
    EndSequence_DrawShadow2(k);
    break;
  }
  case 6: {
    static const uint8 kEnding_Case6_SprType[3] = { 0x52, 0x55, 0x55 };
    static const uint8 kEnding_Case6_OamSize[3] = { 0x20, 8, 8 };
    static const uint8 kEnding_Case6_State[3] = { 3, 1, 1 };
    static const uint8 kEnding_Case6_Gfx[6] = { 0, 5, 5, 1, 6, 6 };

    int idx = kEndingSprites_Idx[i];
    int num = kEndingSprites_Idx[i + 1] - idx;

    for (int k = num - 1; k >= 0; k--) {
      cur_object_index = k;
      sprite_type[k] = kEnding_Case6_SprType[k];
      Oam_AllocateFromRegionA(kEnding_Case6_OamSize[k]);
      sprite_ai_state[k] = kEnding_Case6_State[k];
      j = (R16 >= 0x26f) ? k + 3 : k;
      if (R16 == 0x26f)
        sound_effect_2 = 0x21;
      sprite_graphics[k] = kEnding_Case6_Gfx[j];
      sprite_oam_flags[k] = 0x33;
      Sprite_Get16BitCoords(k);
      SpriteActive_Main(k);
    }
    break;
  }
  case 7:
    k = 1;
    Credits_SpriteDraw_SetShadowProp(k, 2);
    sprite_type[k] = 0xe9;
    Oam_AllocateFromRegionA(0xc);
    sprite_oam_flags[k] = 0x37;
    Sprite_Get16BitCoords(k);
    if (!(frame_counter & 15))
      sprite_graphics[k] ^= 1;
    SpriteActive_Main(k);
    if (R16 >= 0x180) {
      sprite_y_vel[k] = 4;
      if (sprite_y_lo[k] != 0x7c)
        Sprite_MoveXY(k);
    }
    k--;
    sprite_type[k] = 0x36;
    Oam_AllocateFromRegionA(0x18);
    sprite_oam_flags[k] = 0x39;
    Sprite_Get16BitCoords(k);
    if (!sprite_delay_main[k]) {
      static const int8 kEnding_Case7_Gfx[2] = { 1, -1 };
      sprite_delay_main[k] = 4;
      sprite_graphics[k] = sprite_graphics[k] + kEnding_Case7_Gfx[R16 >> 9 & 1] & 7;
    }
    SpriteActive_Main(k);
    break;
  case 8:
    k = 0;
    sprite_type[k] = 0x2c;
    Oam_AllocateFromRegionA(0x2c);
    sprite_oam_flags[k] = 0x3b;
    Sprite_Get16BitCoords(k);
    sprite_graphics[k] = R16 < 0x1c0 ? R16 >> 5 & 1 : 2;
    SpriteActive_Main(k);
    break;
  case 9:
    for (k = 0; k < 5; k++) {
      if (!sprite_delay_main[k]) {
        sprite_delay_main[k] = 96;
        sprite_state[k] = 96;
        sprite_x_vel[k] = 0;
        sprite_x_lo[k] = 238;
        sprite_x_hi[k] = 4;
        sprite_y_lo[k] = 24;
        sprite_y_hi[k] = 11;
      }
      if (sprite_state[k]) {
        sprite_y_vel[k] = -8;
        Sprite_MoveXY(k);
        if (!(frame_counter & 1))
          sprite_x_vel[k] += ((frame_counter >> 5) ^ k) & 1 ? -1 : 1;
        Credits_SpriteDraw_Single(k, 1, 0x10);
      }
    }
    for (;;) {
      if (!sprite_delay_main[k]) {
        static const uint8 kEnding_Case8_Delay1[4] = { 16, 14, 16, 18 };
        static const uint8 kEnding_Case8_Delay2[4] = { 20, 48, 20, 20 };
        sprite_delay_main[k] = (k == 5) ? kEnding_Case8_Delay1[sprite_A[k]] : kEnding_Case8_Delay2[sprite_A[k]];
        sprite_A[k] = sprite_A[k] + 1 & 3;
        sprite_graphics[k] ^= 1;
      }
      if (k == 5) {
        sprite_oam_flags[k] = 0x31;
        Credits_SpriteDraw_PreexistingSpriteDraw(k, 0x10);
        k++;
      } else {
        Credits_SpriteDraw_Single(k, 2, 0x12);
        k++;
        break;
      }
    }
    do {
      static const uint8 kEnding_Case8_D[4] = { 0, 1, 0, 1 };
      static const uint8 kEnding_Case8_OamFlags[4] = { 55, 55, 59, 61 };
      static const uint8 kEnding_Case8_Tab0[4] = { 8, 8, 12, 12 };
      sprite_oam_flags[k] = kEnding_Case8_OamFlags[k - 7];
      sprite_D[k] = kEnding_Case8_D[k - 7];
      Credits_SpriteDraw_ActivateAndRunSprite(k, kEnding_Case8_Tab0[k - 7]);
    } while (++k != 11);
    break;
  case 10: {
    static const uint8 kWishPond_X[8] = { 0, 4, 8, 12, 16, 20, 24, 0 };
    static const uint8 kWishPond_Y[8] = { 0, 8, 16, 24, 32, 40, 4, 36 };
    k = 5;
    Sprite_Get16BitCoords(k);
    if (!sprite_pause[k]) {
      uint8 xb = kWishPond_X[GetRandomNumber() & 7] + cur_sprite_x;
      uint8 yb = kWishPond_Y[GetRandomNumber() & 7] + cur_sprite_y;
      Credits_SpriteDraw_AddSparkle(3, xb, yb);
    }
    for (int k = 3; k < 5; k++) {
      if (sprite_delay_aux1[k])
        sprite_delay_aux1[k]--;
      sprite_type[k] = 0xe3;
      Credits_SpriteDraw_SetShadowProp(k, 1);
      Credits_SpriteDraw_ActivateAndRunSprite(k, 8);
    }
    sprite_type[k] = 0x72;
    sprite_oam_flags[k] = 0x3b;
    sprite_state[k] = 9;
    sprite_B[k] = 9;
    Credits_SpriteDraw_PreexistingSpriteDraw(k, 0x30);
    break;
  }
  case 11:
    if (R16 >= 0x170) {
      for (int k = 4; k != 6; k++) {
        Credits_SpriteDraw_Single(k, 1, 0x3e);
      }
      k = 0;
      sprite_oam_flags[k] = 0x39;
      if (R16 < 0x1c0) {
        sprite_graphics[k] = 2;
      } else if (sprite_delay_main[k] == 0) {
        sprite_delay_main[k] = 0x20;
        sprite_graphics[k] = (sprite_graphics[k] ^ 1) & 1;
      }
      Credits_SpriteDraw_Single(k, 4, 6);
    } else {
      static const uint8 kEnding_Case11_Gfx[16] = { 1, 1, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0 };
      for (int k = 0; k < 2; k++) {
        sprite_type[k] = 0x1a;
        sprite_oam_flags[k] = 0x39;
        Credits_SpriteDraw_SetShadowProp(k, 2);
        uint8 bak0 = main_module_index;
        Credits_SpriteDraw_ActivateAndRunSprite(k, 0xc);
        main_module_index = bak0;
        if (sprite_B[k] == 15 && sprite_A[k] == 4)
          sprite_delay_main[k + 2] = 15;
        int j = sprite_delay_main[k + 2];
        if (j != 0) {
          sprite_oam_flags[k + 2] = 2;
          sprite_graphics[k + 2] = kEnding_Case11_Gfx[j];
          Credits_SpriteDraw_Single(k + 2, 2, 0x36);
        }
      }
    }
    break;
  case 12:
    k = 6;
    sprite_graphics[k] = frame_counter & 1;
    if (!sprite_graphics[k]) {
      sprite_x_vel[k] += sign8(sprite_x_lo[k] - 0x80) ? 1 : -1;
      sprite_y_vel[k] += sign8(sprite_y_lo[k] - 0xb0) ? 1 : -1;
      Sprite_MoveXY(k);
    }

    sprite_oam_flags[k] = sprite_x_vel[k] >> 1 & 0x40 ^ 0x7e;
    sprite_flags2[k] = 1;
    sprite_flags3[k] = 0x30;
    sprite_z[k] = 16;
    Credits_SpriteDraw_PreexistingSpriteDraw(k, 8);
    k--;
    sprite_oam_flags[k] = 0x37;
    Credits_SpriteDraw_SetShadowProp(k, 2);
    Credits_SpriteDraw_ActivateAndRunSprite(k, 12);
    k--;
    Credits_SpriteDraw_ActivateAndRunSprite(k, 8);
    k--;
    Credits_SpriteDraw_ActivateAndRunSprite(k, 8);
    k--;
    do {
      static const uint8 kEnding_Case12_Tab[3] = { 3, 3, 8 };
      static const uint8 kEnding_Case12_Z[15] = { 2, 4, 5, 6, 6, 7, 7, 7, 7, 6, 6, 5, 4, 2, 0 };

      Credits_SpriteDraw_Single(k, kEnding_Case12_Tab[k], k * 2);
      if (k == 0) {
        Ending_Func2(k, 0x30);
      } else if (k & ~1) {
        sprite_graphics[k] = frame_counter >> 3 & 1;
      } else {
        int j = frame_counter & 0x1f;
        if (j < 0xf) {
          sprite_z[k] = kEnding_Case12_Z[j];
        }
        sprite_graphics[k] = (j < 0xf) ? 1 : 0;
        Credits_SpriteDraw_DrawShadow(k);
      }
    } while (--k >= 0);
    break;
  case 13:
    k = 0;
    if (R16 == 0x200)
      sprite_x_vel[k] = -4;
    sprite_graphics[k] = frame_counter >> 4 & 1;
    if (sprite_x_lo[k] == 56) {
      sprite_x_vel[k] = 0;
      sprite_graphics[k] += 2;
    }
    Credits_SpriteDraw_Single(k, 3, 0x34);
    Sprite_MoveXY(k);
    break;
  case 14: {
    static const int8 kEnding_Case14_Tab1[4] = { 0, 1, 0, 2 };
    static const int8 kEnding_Case14_Tab0[5] = { 2, 8, 32, 32, 8 };
    for (k = 6; k; k--) {
      if (k >= 5) {
        sprite_type[k] = 0;
        Credits_SpriteDraw_SetShadowProp(k, 1);
        sprite_graphics[k] = (frame_counter + 0x4a & 8) >> 3;
        sprite_z[k] = 32;
        Credits_SpriteDraw_CirclingBirds(k);
        sprite_oam_flags[k] = (sprite_x_vel[k] >> 1 & 0x40) ^ 0xf;
        Credits_SpriteDraw_PreexistingSpriteDraw(k, 8);
      } else {
        sprite_type[k] = 0xd;
        if (k == 1)
          sprite_head_dir[k] = 0xd;
        Credits_SpriteDraw_SetShadowProp(k, 3);
        sprite_oam_flags[k] = 0x2b;
        uint8 a = sprite_delay_main[k];
        if (!a)
          sprite_delay_main[k] = a = 0xc0;
        a >>= 1;
        if (a == 0) {
          sprite_y_vel[k] = sprite_x_vel[k] = 0;
        } else {
          if (a < kEnding_Case14_Tab0[k] && !(frame_counter & 3) && (a = sprite_y_vel[k]) != 0) {
            sprite_y_vel[k] = --a;
            a -= 4;
            if (k < 3)
              a = -a;
            sprite_x_vel[k] = a;
          }
        }
        Sprite_MoveXY(k);
        sprite_graphics[k] = kEnding_Case14_Tab1[frame_counter >> 3 & 3];
        Credits_SpriteDraw_PreexistingSpriteDraw(k, 16);
      }
    }
    Credits_SpriteDraw_Single(k, 3, 0x18);
    Ending_Func2(k, 0x20);
    break;
  }
  case 15: {
    static const uint8 kEnding_Case15_X[4] = { 0x76, 0x73, 0x71, 0x78 };
    static const uint8 kEnding_Case15_Y[4] = { 0x8b, 0x83, 0x8d, 0x85 };
    static const uint8 kEnding_Case15_Delay[8] = { 6, 6, 6, 6, 6, 6, 10, 8 };
    static const uint8 kEnding_Case15_OamFlags[4] = { 0x61, 0x61, 0x3b, 0x39 };
    j = kGeneratedEndSequence15[frame_counter] & 3;
    Credits_SpriteDraw_AddSparkle(2, kEnding_Case15_X[j], kEnding_Case15_Y[j]);
    k = 2;
    sprite_type[k] = 0x62;
    sprite_oam_flags[k] = 0x39;
    Credits_SpriteDraw_PreexistingSpriteDraw(k, 0x18);
    for (j = 1; j >= 0; j--) {
      k++;
      if (sprite_delay_aux1[k])
        sprite_delay_aux1[k]--;
      sprite_oam_flags[k] = (sprite_x_vel[k] >> 1 & 0x40) ^ kEnding_Case15_OamFlags[j];
      if (!sprite_delay_main[k]) {
        sprite_delay_main[k] = 128;
        sprite_A[k] = 0;
      }
      if (!sprite_A[k]) {
        sprite_graphics[k] = (frame_counter >> 2 & 1) + 2;
        Credits_SpriteDraw_MoveSquirrel(k);
      } else if (!sprite_delay_aux1[k]) {
        if (sprite_B[k] == 8)
          sprite_B[k] = 0;
        sprite_delay_aux1[k] = kEnding_Case15_Delay[sprite_B[k] & 7];
        sprite_graphics[k] = sprite_graphics[k] & 1 ^ 1;
        sprite_B[k]++;
      }
      Credits_SpriteDraw_Single(k, 1, 20);
      EndSequence_DrawShadow2(k);
    }
    Credits_SpriteDraw_WalkLinkAwayFromPedestal(k + 1);
    break;
  }
  }

  // Scene transition logic: once the scroll position counter R16 exceeds the scene's threshold,
  // fade INIDISP out one step every even frame; when it reaches 0, advance to the next scene.
  // While below the threshold, fade INIDISP in (up to 15) to create the cross-fade effect.
  k = submodule_index >> 1;
  if (R16 >= kEnding1_3_Tab0[k]) {
    if (!(R16 & 1) && !--INIDISP_copy)
      submodule_index++;
    else
      R16++;
  } else {
    if (!(R16 & 1) && INIDISP_copy != 15)
      INIDISP_copy++;
    R16++;
  }
  // Propagate the updated scroll positions to the PPU shadow registers.
  BG2HOFS_copy = BG2HOFS_copy2;
  BG2VOFS_copy = BG2VOFS_copy2;
  BG1HOFS_copy = BG1HOFS_copy2;
  BG1VOFS_copy = BG1VOFS_copy2;
}

// Draws a ground shadow beneath sprite k using the standard SpriteDraw_Shadow routine.
// Sets OAM flags to 0x30 (palette 0, no flip, priority 3) and allocates 4 OAM bytes from
// region A before calling SpriteDraw_Shadow with the shared g_ending_coords scratch struct.
void Credits_SpriteDraw_DrawShadow(int k) {  // 8ea5f8
  sprite_oam_flags[k] = 0x30;
  Credits_SpriteDraw_SetShadowProp(k, 0);
  Oam_AllocateFromRegionA(4);
  SpriteDraw_Shadow(k, &g_ending_coords);
}

// Variant of Credits_SpriteDraw_DrawShadow that does not override OAM flags first.
// Used when the caller has already set the correct flags for the sprite and only needs
// the shadow drawn beneath the existing OAM state.
void EndSequence_DrawShadow2(int k) {  // 8ea5fd
  Credits_SpriteDraw_SetShadowProp(k, 0);
  Oam_AllocateFromRegionA(4);
  SpriteDraw_Shadow(k, &g_ending_coords);
}

// Animates a walking character sprite (typically Link or a villager) through a 27-frame
// scripted walk cycle. sprite_A[k] is the current step index into kEnding_Func2_Tab0 (graphics
// frame) and kEnding_Func2_Delay (per-step hold duration). Index 8, 22, and 28 are clamped
// to loop points (6, 21, 27) to create repeating walk cycles within certain ranges.
// On every other game frame during walking steps (j < 5 or 10 ≤ j < 15), the sprite's
// Y position advances by 1 pixel to produce the walking-toward-camera effect.
void Ending_Func2(int k, uint8 ain) {  // 8ea645
  // Per-step frame hold durations for the 27-step walk cycle (255 = hold until externally changed).
  static const uint8 kEnding_Func2_Delay[27] = {
  10, 10, 10, 10, 20, 8,   8,   0, 255, 12, 12, 12, 12, 12, 12, 30,
   8,  4,  4,  4,  0, 0, 255, 255, 144,  4, 0,
  };
  // Graphics frame index for each walk step. -1 = use frame_counter oscillation; 0-6 = static.
  static const int8 kEnding_Func2_Tab0[28] = {
    0, 0, 1, 0, 1, 0, 2,  3,  0,  2, 0, 1, 0, 1, 0, 1,
    2, 3, 4, 5, 6, 3, 0, -1, -1, -1, 2, 3,
  };
  sprite_oam_flags[k] = ain;
  EndSequence_DrawShadow2(k);
  int j = sprite_A[k];
  if (!sprite_delay_main[k]) {
    j++;
    // Clamp loop points so certain walk segments repeat in place.
    if (j == 8)
      j = 6;
    else if (j == 22)
      j = 21;
    else if (j == 28)
      j = 27;
    sprite_A[k] = j;
    sprite_delay_main[k] = kEnding_Func2_Delay[j - 1];
  }
  uint8 a = kEnding_Func2_Tab0[j];
  sprite_graphics[k] = (a == 255) ? frame_counter >> 3 & 1 : a;
  // Advance Y position on alternating frames during approach phases to simulate walking closer.
  if ((j < 5 || j >= 10 && j < 15) && !(frame_counter & 1))
    sprite_y_lo[k]++;
}

// Initialises sprite k from scratch and runs its full SpriteActive_Main logic for one frame.
// Used for credit sprites that need full game-logic processing (movement, collision response)
// rather than just OAM-buffer submission. Temporarily zeros submodule_index so the sprite's
// module-dependent state transitions behave as if in normal gameplay.
void Credits_SpriteDraw_ActivateAndRunSprite(int k, uint8 a) {  // 8ea694
  cur_object_index = k;
  Oam_AllocateFromRegionA(a);
  Sprite_Get16BitCoords(k);
  uint8 bak0 = submodule_index;
  submodule_index = 0;
  sprite_state[k] = 9;
  SpriteActive_Main(k);
  submodule_index = bak0;
}

// Draws a sprite that was already set up by a prior SpriteActive_Main call (i.e., a pre-existing
// active sprite whose state has already been updated this frame). Allocates OAM space from
// region A and calls SpriteActive_Main again to submit the OAM data without re-running AI logic.
void Credits_SpriteDraw_PreexistingSpriteDraw(int k, uint8 a) {  // 8ea6b3
  Oam_AllocateFromRegionA(a);
  cur_object_index = k;
  Sprite_Get16BitCoords(k);
  SpriteActive_Main(k);
}

// Draws sprite k using one of 32 pre-defined DrawMultipleData tables (kEndSequence_Dmd0..31).
// Parameter `j` selects the table index; parameter `a` is the OAM region allocation size in
// bytes. PrepOamCoords is called first to project the sprite's world position to screen space
// (result in g_ending_coords). Then DrawMultiple writes all entries from the selected table into
// the OAM buffer relative to the projected position. Each DrawMultipleData entry specifies a
// relative X/Y offset, a packed tile+attribute word, and a size flag.
void Credits_SpriteDraw_Single(int k, uint8 a, uint8 j) {  // 8ea703
  // DrawMultipleData tables for scene 0 — the soldiers/guards walking in Kakariko.
  static const DrawMultipleData kEndSequence_Dmd0[12] = {
    { 0, -8, 0x072a, 2},
    { 0, -8, 0x072a, 2},
    { 0,  0, 0x4fca, 2},
    { 0, -8, 0x072a, 2},
    { 0, -8, 0x072a, 2},
    { 0,  0, 0x0fca, 2},
    {-2,  0, 0x0f77, 0},
    { 0, -8, 0x072a, 2},
    { 0,  0, 0x4fca, 2},
    {-3,  0, 0x0f66, 0},
    { 0, -8, 0x072a, 2},
    { 0,  0, 0x4fca, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd1[6] = {
    {14,  -7, 0x0d48, 2},
    { 0,  -6, 0x0944, 2},
    { 0,   0, 0x094e, 2},
    {13, -14, 0x0d48, 2},
    { 0,  -8, 0x0944, 2},
    { 0,   0, 0x0946, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd2[16] = {
    {-2, -16, 0x3d78, 0},
    { 0, -24, 0x3d24, 2},
    { 0, -16, 0x3dc2, 2},
    {61, -16, 0x3777, 0},
    {64, -24, 0x37c4, 2},
    {64, -16, 0x77ca, 2},
    { 0,  -6, 0x326c, 2},
    {64,  -6, 0x326c, 2},
    {-2, -16, 0x3d68, 0},
    { 0, -24, 0x3d24, 2},
    { 0, -16, 0x3dc2, 2},
    {61, -16, 0x3766, 0},
    {64, -24, 0x37c4, 2},
    {64, -16, 0x77ca, 2},
    { 0,  -6, 0x326c, 2},
    {64,  -6, 0x326c, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd3[12] = {
    { 0,  0, 0x0022, 2},
    {48,  0, 0x0064, 2},
    { 0, 10, 0x016c, 2},
    {48, 10, 0x016c, 2},
    { 0,  0, 0x0064, 2},
    {48,  0, 0x0022, 2},
    { 0, 10, 0x016c, 2},
    {48, 10, 0x016c, 2},
    { 0,  0, 0x0064, 2},
    {48,  0, 0x0064, 2},
    { 0, 10, 0x016c, 2},
    {48, 10, 0x016c, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd4[8] = {
    {10,   8, 0x8a32, 0},
    {10,  16, 0x8a22, 0},
    { 0, -10, 0x0800, 2},
    { 0,   0, 0x082c, 2},
    {10, -14, 0x0a22, 0},
    {10,  -6, 0x0a32, 0},
    {0, -10, 0x082a, 2},
    {0,   0, 0x0828, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd5[10] = {
    {10,  16, 0x8a05, 0},
    {10,   8, 0x8a15, 0},
    {-4,   2, 0x0a07, 2},
    { 0,  -7, 0x0e00, 2},
    { 0,   1, 0x0e02, 2},
    {10, -20, 0x0a05, 0},
    {10, -12, 0x0a15, 0},
    {-7,   1, 0x4a07, 2},
    { 0,  -7, 0x0e00, 2},
    { 0,   1, 0x0e02, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd6[3] = {
    {-6, -2, 0x0706, 2},
    { 0, -9, 0x090e, 2},
    { 0, -1, 0x0908, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd7[10] = {
    {0, -10, 0x082a, 2},
    {0,   0, 0x0828, 2},
    {10,  16, 0x8a05, 0},
    {10,   8, 0x8a15, 0},
    {-4,   2, 0x0a07, 2},
    { 0,  -7, 0x0e00, 2},
    { 0,   1, 0x0e02, 2},
    {10, -20, 0x0a05, 0},
    {10, -12, 0x0a15, 0},
    {-7,   1, 0x4a07, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd8[1] = {
    {0, -19, 0x39af, 0},
  };
  static const DrawMultipleData kEndSequence_Dmd9[4] = {
    {-16, -24, 0x3704, 2},
    {-16, -16, 0x3764, 2},
    {-16, -24, 0x3762, 2},
    {-16, -16, 0x3764, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd10[4] = {
    {0, 0, 0x0c0c, 2},
    {0, 0, 0x0c0a, 2},
    {0, 0, 0x0cc5, 2},
    {0, 0, 0x0ce1, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd11[6] = {
    {1,  4, 0x002a, 0},
    {1, 12, 0x003a, 0},
    {4,  0, 0x0026, 2},
    {0,  9, 0x0024, 2},
    {8,  9, 0x4024, 2},
    {4, 20, 0x016c, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd12[21] = {
    { 0, -7, 0x0d00, 2},
    { 0, -7, 0x0d00, 2},
    { 0,  0, 0x0d06, 2},
    { 0, -7, 0x0d00, 2},
    { 0, -7, 0x0d00, 2},
    { 0,  0, 0x4d06, 2},
    { 0, -8, 0x0d00, 2},
    { 0, -8, 0x0d00, 2},
    { 0,  0, 0x0d20, 2},
    { 0, -8, 0x0d02, 2},
    { 0, -8, 0x0d02, 2},
    { 0,  0, 0x0d2c, 2},
    {-3,  0, 0x0d2f, 0},
    { 0, -7, 0x0d02, 2},
    { 0,  0, 0x0d2c, 2},
    {-5,  2, 0x0d2f, 0},
    { 0, -8, 0x0d02, 2},
    { 0,  0, 0x0d2c, 2},
    {-5,  2, 0x0d3f, 0},
    { 0, -8, 0x0d02, 2},
    { 0,  0, 0x0d2c, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd13[16] = {
    {0, -7, 0x0e00, 2},
    {0,  1, 0x4e02, 2},
    {0, -8, 0x0e00, 2},
    {0,  1, 0x0e02, 2},
    {0, -9, 0x0e00, 2},
    {0,  1, 0x0e02, 2},
    {0, -7, 0x0e00, 2},
    {0,  1, 0x0e02, 2},
    {0, -7, 0x0e00, 2},
    {0,  1, 0x4e02, 2},
    {0, -8, 0x0e00, 2},
    {0,  1, 0x4e02, 2},
    {0, -9, 0x0e00, 2},
    {0,  1, 0x4e02, 2},
    {0, -7, 0x0e00, 2},
    {0,  1, 0x4e02, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd14[6] = {
    {0, 0, 0, 0},
    {0, 0, 0x34c7, 0},
    {0, 0, 0x3480, 0},
    {0, 0, 0x34b6, 0},
    {0, 0, 0x34b7, 0},
    {0, 0, 0x34a6, 0},
  };
  static const DrawMultipleData kEndSequence_Dmd15[6] = {
    {-3, 17, 0x002b, 0},
    {-3, 25, 0x003b, 0},
    { 0,  0, 0x000e, 2},
    {16,  0, 0x400e, 2},
    { 0, 16, 0x002e, 2},
    {16, 16, 0x402e, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd16[3] = {
    { 8,  5, 0x0a04, 2},
    { 0, 16, 0x0806, 2},
    {16, 16, 0x4806, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd17[2] = {
    {0,  0, 0x0000, 2},
    {0, 11, 0x0002, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd18[2] = {
    {0,  0, 0x000e, 2},
    {0, 64, 0x006c, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd19[8] = {
    {0, 0, 0x0882, 2},
    {0, 7, 0x0a4e, 2},
    {0, 0, 0x4880, 2},
    {0, 7, 0x0a4e, 2},
    {0, 0, 0x0882, 2},
    {0, 7, 0x0a4e, 2},
    {0, 0, 0x0880, 2},
    {0, 7, 0x0a4e, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd20[6] = {
    {-4,  1, 0x0c68, 0},
    { 0, -8, 0x0c40, 2},
    { 0,  1, 0x0c42, 2},
    {-4,  1, 0x0c78, 0},
    { 0, -8, 0x0c40, 2},
    { 0,  1, 0x0c42, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd21[6] = {
    {8,   5, 0x0679, 0},
    {0, -10, 0x088e, 2},
    {0,   0, 0x066e, 2},
    {0, -10, 0x088e, 2},
    {0, -10, 0x088e, 2},
    {0,   0, 0x066e, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd22[6] = {
    {11,  -3, 0x0869, 0},
    { 0, -12, 0x0804, 2},
    { 0,   0, 0x0860, 2},
    {10,  -3, 0x0867, 0},
    { 0, -12, 0x0804, 2},
    { 0,   0, 0x0860, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd23[6] = {
    {-2,  1, 0x0868, 0},
    { 0, -8, 0x08c0, 2},
    { 0,  0, 0x08c2, 2},
    {-3,  1, 0x0878, 0},
    { 0, -8, 0x08c0, 2},
    { 0,  0, 0x08c2, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd24[4] = {
    {0, -10, 0x084c, 2},
    {0,   0, 0x0a6c, 2},
    {0,  -9, 0x084c, 2},
    {0,   0, 0x0aa8, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd25[4] = {
    {0, -7, 0x084a, 2},
    {0,  0, 0x0c6a, 2},
    {0, -7, 0x084a, 2},
    {0,  0, 0x0ca6, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd26[12] = {
    {-18, -24, 0x39a4, 2},
    {-16, -16, 0x39a8, 2},
    {-18, -24, 0x39a4, 2},
    {-18, -24, 0x39a4, 2},
    {-16, -16, 0x39a6, 2},
    {-18, -24, 0x39a4, 2},
    { -6, -17, 0x392d, 0},
    {-16, -24, 0x39a0, 2},
    {-16, -16, 0x39aa, 2},
    { -5, -17, 0x392c, 0},
    {-16, -24, 0x39a0, 2},
    {-16, -16, 0x39aa, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd27[6] = {
    { 0,  -4, 0x30aa, 2},
    { 0,  -4, 0x30aa, 2},
    {-4,  -8, 0x3090, 0},
    {12,  -8, 0x7090, 0},
    {-6, -10, 0x3091, 0},
    {14, -10, 0x7091, 0},
  };
  static const DrawMultipleData kEndSequence_Dmd28[8] = {
    {0,  0, 0x0722, 2},
    {0, -8, 0x09c2, 2},
    {0,  0, 0x4722, 2},
    {0, -8, 0x09c2, 2},
    {0, -9, 0x09c4, 2},
    {0,  0, 0x0722, 2},
    {0, -9, 0x0924, 2},
    {0,  0, 0x0722, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd29[3] = {
    {-16, -12, 0x3f08, 2},
    {  0, -12, 0x3f20, 2},
    { 16, -12, 0x3f20, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd30[1] = {
    {0, 0, 0x0086, 2},
  };
  static const DrawMultipleData kEndSequence_Dmd31[1] = {
    {0, 0, 0x8060, 2},
  };
  static const DrawMultipleData *const kEndSequence_Dmds[] = {
    kEndSequence_Dmd0, kEndSequence_Dmd1, kEndSequence_Dmd2, kEndSequence_Dmd3,
    kEndSequence_Dmd4, kEndSequence_Dmd5, kEndSequence_Dmd6, kEndSequence_Dmd7,
    kEndSequence_Dmd8, kEndSequence_Dmd9, kEndSequence_Dmd10, kEndSequence_Dmd11,
    kEndSequence_Dmd12, kEndSequence_Dmd13, kEndSequence_Dmd14, kEndSequence_Dmd15,
    kEndSequence_Dmd16, kEndSequence_Dmd17, kEndSequence_Dmd18, kEndSequence_Dmd19,
    kEndSequence_Dmd20, kEndSequence_Dmd21, kEndSequence_Dmd22, kEndSequence_Dmd23,
    kEndSequence_Dmd24, kEndSequence_Dmd25, kEndSequence_Dmd26, kEndSequence_Dmd27,
    kEndSequence_Dmd28, kEndSequence_Dmd29, kEndSequence_Dmd30, kEndSequence_Dmd31
  };

  // Dispatch: j>>1 selects the DrawMultipleData table; a*sprite_graphics[k] selects the animation
  // frame offset within that table (each frame is 'a' entries apart in the flat array).
  Oam_AllocateFromRegionA(a * 4);
  Sprite_Get16BitCoords(k);
  Sprite_DrawMultiple(k, kEndSequence_Dmds[j >> 1] + a * sprite_graphics[k], a, &g_ending_coords);
}

// Sets shadow-related sprite property flags for a credit-scene sprite. sprite_flags2[k]=a sets
// the shadow size/type selector; sprite_flags3[k]=16 sets the shadow draw priority flag.
// Called before SpriteDraw_Shadow to ensure the correct shadow shape is used.
void Credits_SpriteDraw_SetShadowProp(int k, uint8 a) {  // 8eaca2
  sprite_flags2[k] = a;
  sprite_flags3[k] = 16;
}

// Animates j_count sparkle/star sprites (slots 0..j_count-1) for scenes with glinting effects
// (e.g., Wish Pond coins, crystal sparkles). Each slot cycles through 6 animation frames at
// kEnding_Func3_Delay[frame] hold durations. When a slot cycles past frame 5 it resets to (xb, yb)
// and starts over — this makes sparkles loop at a fixed screen position. Frames 1-5 call
// Credits_SpriteDraw_Single with table 0x1c (sparkle glyph) to emit the OAM entry.
void Credits_SpriteDraw_AddSparkle(int j_count, uint8 xb, uint8 yb) {  // 8eace5
  // Per-frame hold durations for the 6 sparkle animation frames (0=hidden, 1-5=visible).
  static const uint8 kEnding_Func3_Delay[6] = { 32, 4, 4, 4, 5, 6 };
  sprite_C[0] = j_count;
  for (int k = 0; k < j_count; k++) {
    int j = sprite_graphics[k];
    if (!sprite_delay_main[k]) {
      if (++j >= 6) {
        // Cycle complete; reset position to the sparkle anchor and restart.
        sprite_x_lo[k] = xb;
        sprite_y_lo[k] = yb;
        j = 0;
      }
      sprite_graphics[k] = j;
      sprite_delay_main[k] = kEnding_Func3_Delay[j];
    }
    if (j)
      Credits_SpriteDraw_Single(k, 1, 0x1c);
  }
}

// Animates Link walking away from the Master Sword pedestal in credits scene 15.
// Cycles through 8 DMA animation frames (kEnding_Func6_Dma) every 4 frames, updates
// link_dma_graphics_index to load the correct CHR, draws Link and shadow via Credits_SpriteDraw_Single,
// then moves the sprite by its velocity via Sprite_MoveXY.
void Credits_SpriteDraw_WalkLinkAwayFromPedestal(int k) {  // 8eadf7
  // DMA indices for the 8-frame walk cycle (forward, turn, walk-right frames).
  static const uint16 kEnding_Func6_Dma[8] = { 0x16c, 0x16e, 0x170, 0x172, 0x16c, 0x174, 0x176, 0x178 };
  if (!sprite_delay_main[k]) {
    sprite_graphics[k] = sprite_graphics[k] + 1 & 7;
    sprite_delay_main[k] = 4;
  }
  link_dma_graphics_index = kEnding_Func6_Dma[sprite_graphics[k]];
  sprite_oam_flags[k] = 32;
  Credits_SpriteDraw_Single(k, 2, 26);
  EndSequence_DrawShadow2(k);
  Sprite_MoveXY(k);
}

// Animates the squirrel sprite in credits scene 15 (Death Mountain area). While delay ≥ 64
// the squirrel moves along one of 4 diagonal trajectories (kEnding_Func5_Xvel/Yvel); when
// delay falls below 64 it cycles to the next trajectory and increments the walk-frame counter.
void Credits_SpriteDraw_MoveSquirrel(int k) {  // 8eae35
  // Four trajectory velocity pairs for the squirrel's figure-8 path.
  static const int8 kEnding_Func5_Xvel[4] = { 32, 24, -32, -24 };
  static const int8 kEnding_Func5_Yvel[4] = { 8, -8, -8, 8 };
  if (sprite_delay_main[k] < 64) {
    // Switch to the next diagonal leg of the path.
    sprite_C[k] = sprite_C[k] + 1 & 3;
    sprite_A[k]++;
  } else {
    int j = sprite_C[k];
    sprite_x_vel[k] = kEnding_Func5_Xvel[j];
    sprite_y_vel[k] = kEnding_Func5_Yvel[j];
    Sprite_MoveXY(k);
  }
}

// Moves a bird sprite in a lazy figure-8 pattern by oscillating x_vel and y_vel between
// ±0x20/±0x10 using sprite_D (X direction flag) and sprite_head_dir (Y direction flag).
// X velocity changes every frame; Y velocity changes every other frame (slower oscillation).
// Called for bird sprites in credits scenes 2, 3, and 14 to make them circle lazily.
void Credits_SpriteDraw_CirclingBirds(int k) {  // 8eae63
  // X oscillation target velocities: +0x20 (rightward) and -0x20 (leftward).
  static const int8 kEnding_MoveSprite_Func1_TargetX[2] = { 0x20, -0x20 };
  // Y oscillation target velocities: +0x10 (downward) and -0x10 (upward).
  static const int8 kEnding_MoveSprite_Func1_TargetY[2] = { 0x10, -0x10 };

  int j = sprite_D[k] & 1;
  sprite_x_vel[k] += j ? -1 : 1;
  if (sprite_x_vel[k] == (uint8)kEnding_MoveSprite_Func1_TargetX[j])
    sprite_D[k]++;
  if (!(frame_counter & 1)) {
    j = sprite_head_dir[k] & 1;
    sprite_y_vel[k] += j ? -1 : 1;
    if (sprite_y_vel[k] == (uint8)kEnding_MoveSprite_Func1_TargetY[j])
      sprite_head_dir[k]++;
  }
  Sprite_MoveXY(k);
}

// Applies the current per-frame scroll velocity (link_x/y_vel set by Credits_ScrollScene_Overworld)
// to both BG2 (overworld map layer) and BG1 (parallax layer) scroll registers, with sub-pixel
// precision. Also updates overworld_screen_trans_dir_bits2 when BG2 has scrolled a full 16-pixel
// screen cell, triggering OverworldHandleMapScroll to load adjacent tile data.
//
// BG1 parallax ratio varies by overlay type: overlays 0xb5/0xbe divide by 4 (slow parallax);
// overlays 0x95/0x9e divide by 4 horizontally; all others divide by 2. Special overlay handling:
//   0x9c: BG1 lags BG2 by a fixed 0x2000 sub-pixel offset and locks horizontal to BG2.
//   0x97/0x9d: BG1 advances 0x2000 sub-pixels in both axes regardless of input velocity.
// Special room 0x181: BG1V is forced to BG2V | 0x100, BG1H = BG2H (interior overworld room).
void Credits_HandleCameraScrollControl() {  // 8eaea6
  if (link_y_vel != 0) {
    uint8 yvel = link_y_vel;
    BG2VOFS_copy2 += (int8)yvel;
    uint16 *which = sign8(yvel) ? &overworld_unk1 : &overworld_unk1_neg;
    *which += abs8(yvel);
    if (!sign16(*which - 0x10)) {
      *which -= 0x10;
      overworld_screen_trans_dir_bits2 |= sign8(yvel) ? 8 : 4;
    }
    *(sign8(yvel) ? &overworld_unk1_neg : &overworld_unk1) = -*which;
    uint16 r4 = (int8)yvel, subp;
    WORD(byte_7E069E[0]) = r4;
    uint8 oi = BYTE(overlay_index);
    if (oi != 0x97 && oi != 0x9d) {
      if (oi == 0xb5 || oi == 0xbe) {
        subp = (r4 & 3) << 14;
        r4 >>= 2;
        if (r4 >= 0x3000)
          r4 |= 0xf000;
      } else {
        subp = (r4 & 1) << 15;
        r4 >>= 1;
        if (r4 >= 0x7000)
          r4 |= 0xf000;
      }
      uint32 tmp = BG1VOFS_subpixel | BG1VOFS_copy2 << 16;
      tmp += subp | r4 << 16;
      BG1VOFS_subpixel = (uint16)(tmp);
      BG1VOFS_copy2 = (uint16)(tmp >> 16);
    }
  }

  if (link_x_vel != 0) {
    uint8 xvel = link_x_vel;
    BG2HOFS_copy2 += (int8)xvel;
    uint16 *which = sign8(xvel) ? &overworld_unk3 : &overworld_unk3_neg;
    *which += abs8(xvel);
    if (!sign16(*which - 0x10)) {
      *which -= 0x10;
      overworld_screen_trans_dir_bits2 |= sign8(xvel) ? 2 : 1;
    }
    *(sign8(xvel) ? &overworld_unk3_neg : &overworld_unk3) = -*which;

    uint16 r4 = (int8)xvel, subp;
    WORD(byte_7E069E[1]) = r4;
    uint8 oi = BYTE(overlay_index);
    if (oi != 0x97 && oi != 0x9d && r4 != 0) {
      if (oi == 0x95 || oi == 0x9e) {
        subp = (r4 & 3) << 14;
        r4 >>= 2;
        if (r4 >= 0x3000)
          r4 |= 0xf000;
      } else {
        subp = (r4 & 1) << 15;
        r4 >>= 1;
        if (r4 >= 0x7000)
          r4 |= 0xf000;
      }
      uint32 tmp = BG1HOFS_subpixel | BG1HOFS_copy2 << 16;
      tmp += subp | r4 << 16;
      BG1HOFS_subpixel = (uint16)(tmp), BG1HOFS_copy2 = (uint16)(tmp >> 16);
    }
  }

  if (BYTE(overlay_index) == 0x9c) {
    uint32 tmp = BG1VOFS_subpixel | BG1VOFS_copy2 << 16;
    tmp -= 0x2000;
    BG1VOFS_subpixel = (uint16)(tmp), BG1VOFS_copy2 = (uint16)(tmp >> 16) + WORD(byte_7E069E[0]);
    BG1HOFS_copy2 = BG2HOFS_copy2;
  } else if (BYTE(overlay_index) == 0x97 || BYTE(overlay_index) == 0x9d) {
    uint32 tmp = BG1VOFS_subpixel | BG1VOFS_copy2 << 16;
    tmp += 0x2000;
    BG1VOFS_subpixel = (uint16)(tmp), BG1VOFS_copy2 = (uint16)(tmp >> 16);
    tmp = BG1HOFS_subpixel | BG1HOFS_copy2 << 16;
    tmp += 0x2000;
    BG1HOFS_subpixel = (uint16)(tmp), BG1HOFS_copy2 = (uint16)(tmp >> 16);
  }

  if (dungeon_room_index == 0x181) {
    BG1VOFS_copy2 = BG2VOFS_copy2 | 0x100;
    BG1HOFS_copy2 = BG2HOFS_copy2;
  }
}

// EndSequence_32 — Entry point for the final credits scroll phase (kEndSequence_Funcs[32]).
// Called once when the 16 credit scenes have been exhausted. Sets up the star/triangle
// background, initialises the spinning Triforce polyhedral thread (Credits_InitializePolyhedral),
// tallies death counts across all 14 palaces into death_var2, saves the game, and configures
// the VRAM upload pipeline that drives Credits_AddNextAttribution on subsequent frames.
//
// Specific setup steps:
//   - Force-blanks and erases tilemaps, then loads the star BG (Credits_LoadCoolBackground).
//   - Starts INIDISP at 128 (half brightness) for the fade-in transition.
//   - Sums deaths_per_palace[0..13] into death_var2 (total death counter shown at scroll end).
//   - Restores Link's health to the capacity-appropriate default (kHealthAfterDeath lookup).
//   - Sets savegame_is_darkworld=0x40 (indicates completed game) and saves.
//   - Clears palette entries 0 and 38 in both buffers (ensure black background).
//   - Enables BG1+2+3 (TM=0x16) and sets R16=0x6800 as the initial VRAM write address for
//     the credits text tile rows.
void EndSequence_32() {  // 8ebc6d
  EnableForceBlank();
  EraseTileMaps_triforce();
  TransferFontToVRAM();
  Credits_LoadCoolBackground();
  Credits_InitializePolyhedral();
  INIDISP_copy = 128;
  overworld_palette_aux_or_main = 0x200;
  hud_palette = 1;
  Palette_Load_HUD();
  flag_update_cgram_in_nmi++;
  // Death count for palace 4 (Eastern Palace) resets since it shares a slot; add accumulated deaths.
  deaths_per_palace[4] = 0;
  deaths_per_palace[13] += death_save_counter;
  int sum = deaths_per_palace[13];
  for (int i = 12; i >= 0; i--)
    sum += deaths_per_palace[i];
  death_var2 = sum;
  death_save_counter = 0;
  link_health_current = kHealthAfterDeath[link_health_capacity >> 3];
  savegame_is_darkworld = 0x40;
  SaveGameFile();
  aux_palette_buffer[38] = 0;
  main_palette_buffer[38] = 0;
  aux_palette_buffer[0] = 0;
  main_palette_buffer[0] = 0;
  TM_copy = 0x16;
  TS_copy = 0;
  R16 = 0x6800;
  R18 = 0;
  ending_which_dung = 0;
  BG2VOFS_copy2 = -0x48;
  BG2HOFS_copy2 = 0x90;
  BG3VOFS_copy2 = 0;
  BG3HOFS_copy2 = 0;
  Credits_AddNextAttribution();
  music_control = 0x22;
  CGWSEL_copy = 0;
  CGADSUB_copy = 162;
  // real zelda does 0x12 here but this seems to work too
  zelda_ppu_write(BG2SC, 0x13);
  COLDATA_copy0 = 0x3f;
  COLDATA_copy1 = 0x5f;
  COLDATA_copy2 = 0x9f;
  subsubmodule_index = 64;
  INIDISP_copy = 0;

  HdmaSetup(0, 0xebd53, 0x42, 0, (uint8)BG2HOFS, 0);
  HDMAEN_copy = 0x80;

  BG2HOFS_copy = BG2HOFS_copy2;
  BG2VOFS_copy = BG2VOFS_copy2;
  BG1HOFS_copy = BG1HOFS_copy2;
  BG1VOFS_copy = BG1VOFS_copy2;
}

// Slowly restores the fixed-colour math registers to their target values to fade the colour-math
// overlay back out. Called once every subsubmodule_index frames (countdown from 64). Each tick:
// decrements COLDATA_copy0 toward 32, then COLDATA_copy1 toward 64, then COLDATA_copy2 toward 128.
// Resets the countdown to 16 after each step to slow the fade. Used by Credits_FadeColorAndBeginAnimating.
void Credits_FadeOutFixedCol() {  // 8ebd66
  if (--subsubmodule_index == 0) {
    subsubmodule_index = 16;
    if (COLDATA_copy0 != 32) {
      COLDATA_copy0--;
    } else if (COLDATA_copy1 != 64) {
      COLDATA_copy1--;
    } else if (COLDATA_copy2 != 128) {
      COLDATA_copy2--;
    }
  }
}

// Phase 34 of Module 1A (kEndSequence_Funcs[34]). Drives the main credits text scroll.
// Each frame: fades the fixed-colour math overlay out (Credits_FadeOutFixedCol), disables
// NMI core updates (so only the polyhedral thread runs in vblank), and animates the spinning
// Triforce triangles. Every 4 frames: advances BG2HOFS_copy2 by 1 (slow rightward pan of the
// star background). Derives BG1 horizontal split-scroll positions from BG2H (room_bounds_y.a1/a0/b0/b1).
// At BG2HOFS=0xc00 the star background wraps (zelda_ppu_write(BG2SC, 0x13)). BG3VOFS_copy2
// counts up to 3288 (411 attribution lines × 8 pixels); every 8 pixels (one tile row) calls
// Credits_AddNextAttribution to upload the next text row. When scroll is complete (3288 reached),
// sets R16=0x80 and advances submodule_index to begin the stop/disperse phase.
void Credits_FadeColorAndBeginAnimating() {  // 8ebd8b
  Credits_FadeOutFixedCol();
  nmi_disable_core_updates = 1;
  Credits_AnimateTheTriangles();
  if (!(frame_counter & 3)) {
    if (++BG2HOFS_copy2 == 0xc00) {
      // real zelda writes 0x00 to BG1SC here but that doesn't seem needed
      zelda_ppu_write(BG2SC, 0x13);
    }
    // Derive BG1 horizontal split-scroll positions from BG2H for the parallax star layers.
    room_bounds_y.a1 = BG2HOFS_copy2 >> 1;
    room_bounds_y.a0 = room_bounds_y.a1 + BG2HOFS_copy2;
    room_bounds_y.b0 = room_bounds_y.a0 >> 1;
    room_bounds_y.b1 = room_bounds_y.a1 >> 1;
    if (BG3VOFS_copy2 == 3288) {
      // All 411 attribution lines have scrolled; move to the stop/disperse phase.
      R16 = 0x80;
      submodule_index++;
    } else {
      BG3VOFS_copy2++;
      // Every 8 pixels (one tile row height) upload the next attribution text row.
      if ((BG3VOFS_copy2 & 7) == 0) {
        R18 = BG3VOFS_copy2 >> 3;
        Credits_AddNextAttribution();
      }
    }
  }
  BG2HOFS_copy = BG2HOFS_copy2;
  BG2VOFS_copy = BG2VOFS_copy2;
  BG1HOFS_copy = BG1HOFS_copy2;
  BG1VOFS_copy = BG1VOFS_copy2;
}

// Appends one row of credits text (and optionally a death-count digit triplet) to the VRAM
// upload queue. Called every 8 scroll pixels during Credits_FadeColorAndBeginAnimating to stream
// the credits text into BG3 VRAM as the background scrolls.
//
// Parameters: R16 = current VRAM destination word address (updated on exit); R18 = text row index.
//
// The upload packet format (written into vram_upload_data at vram_upload_offset):
//   [VRAM addr] [0x3e40 = DMA mode] [blank tile]   — clears the row first
//   [VRAM addr + x_offset] [byte count] [tile words…] — the actual text glyphs
// If the current row matches kEnding_Digits_ScrollY[n] for the n-th palace being displayed,
// three death-count digit tiles (hundreds, tens, ones from deaths_per_palace via kEnding_Func9_Tab2)
// are appended at column 0x19 using kEnding_Credits_DigitChar glyph set.
// R16 wraps at every 0x400 boundary, alternating between the 0x6800 and 0x6000 VRAM pages
// to double-buffer the scrolling text rows.
void Credits_AddNextAttribution() {  // 8ebe24
  // Palette-to-palace mapping: reorders death counters into the order they appear in the credits text.
  static const uint8 kEnding_Func9_Tab2[14] = { 1, 0, 2, 3, 10, 6, 5, 8, 11, 9, 7, 12, 13, 15 };
  // BG3 vertical scroll value at which each palace death count row appears.
  static const uint16 kEnding_Digits_ScrollY[14] = { 0x290, 0x298, 0x2a0, 0x2a8, 0x2b0, 0x2ba, 0x2c2, 0x2ca, 0x2d2, 0x2da, 0x2e2, 0x2ea, 0x2f2, 0x310 };
  // Base VRAM tile addresses for the digit 0-9 glyph sets (low-half and high-half tileset).
  static const uint16 kEnding_Credits_DigitChar[2] = { 0x3ce6, 0x3cf6 };

  uint16 *dst = vram_upload_data + (vram_upload_offset >> 1);

  // Write the row header: VRAM address, DMA-mode word, and a blank tile to erase the row.
  dst[0] = swap16(R16);
  dst[1] = 0x3e40;
  dst[2] = kEnding_MapData[159];
  dst += 3;

  if (R18 < 394) {
    const uint8 *src = &kEnding_Credits_Text[kEnding_Credits_Offs[R18]];
    if (*src != 0xff) {
      // X-offset byte, then glyph count, then the tile words for this text line.
      *dst++ = swap16(R16 + *src++);
      int n = *src++;
      *dst++ = swap16(n);
      n = (n + 1) >> 1;
      do {
        *dst++ = kEnding_MapData[*src++];
      } while (--n);
    }

    // When the scroll position matches this palace's entry row, emit the three digit tiles.
    if ((ending_which_dung & 1) || R18 * 2 == kEnding_Digits_ScrollY[ending_which_dung >> 1]) {
      int t = kEnding_Credits_DigitChar[ending_which_dung & 1];
      WORD(g_ram[0xce]) = t;

      dst[0] = swap16(R16 + 0x19);
      dst[1] = 0x500;

      // Clamp to 999 max; extract and emit hundreds, tens, ones as individual tile indices.
      uint16 deaths = deaths_per_palace[kEnding_Func9_Tab2[ending_which_dung >> 1]];
      if (deaths >= 1000)
        deaths = 999;

      dst[4] = t + deaths % 10, deaths /= 10;
      dst[3] = t + deaths % 10, deaths /= 10;
      dst[2] = t + deaths;
      dst += 5;
      ending_which_dung++;
    }
  }

  // Advance the VRAM address by one tile row (0x20 words = 32 tiles); wrap at 0x400 boundaries
  // to ping-pong between the two BG3 VRAM pages (0x6800 and 0x6000).
  R16 += 0x20;
  if (!(R16 & 0x3ff))
    R16 = (R16 & 0x6800) ^ 0x800;
  vram_upload_offset = (char *)dst - (char *)vram_upload_data;
  BYTE(*dst) = 0xff;
  nmi_load_bg_from_vram = 1;
}

// Uploads the scene attribution text for the current credit scene into the VRAM DMA queue.
// Called from Credits_LoadNextScene_Overworld and Credits_LoadNextScene_Dungeon immediately
// after loading the scene geometry. Writes a packet to vram_upload_data starting at offset 0:
//   [0x0060] [0xfe47 = VRAM address 0x7e47] [blank tile] — clears the attribution row
//   For each record in kEnding0_Data[kEnding0_Offs[scene]..kEnding0_Offs[scene+1]]:
//     [vram_word] [count_word] [tile words…] — the scene's attribution text
// Signals NMI to DMA the buffer to VRAM by setting nmi_load_bg_from_vram=1.
void Credits_AddEndingSequenceText() {  // 8ec303

  uint16 *dst = vram_upload_data;
  dst[0] = 0x60;
  dst[1] = 0xfe47;
  dst[2] = kEnding_MapData[159];
  dst += 3;

  const uint8 *curo = &kEnding0_Data[kEnding0_Offs[submodule_index >> 1]];
  const uint8 *endo = &kEnding0_Data[kEnding0_Offs[(submodule_index >> 1) + 1]];
  do {
    dst[0] = WORD(curo[0]);
    dst[1] = WORD(curo[2]);
    // Tile count is packed in bits 15-9 of the second word (shifted right 9, masked to 7 bits).
    int m = (dst[1] >> 9) & 0x7f;
    dst += 2, curo += 4;
    do {
      *dst++ = kEnding_MapData[*curo++];
    } while (--m >= 0);
  } while (curo != endo);

  vram_upload_offset = (char *)dst - (char *)vram_upload_data;
  BYTE(*dst) = 0xff;
  nmi_load_bg_from_vram = 1;
}

// Phase 33 of Module 1A (kEndSequence_Funcs[33]). Brightens the Triforce polyhedral after
// the credits text scroll ends. Every 16 frames increments INIDISP_copy; when it reaches
// full brightness (15) advances submodule_index to the FadeColorAndBeginAnimating phase.
void Credits_BrightenTriangles() {  // 8ec37c
  if (!(frame_counter & 15) && ++INIDISP_copy == 15)
    submodule_index++;
  Credits_AnimateTheTriangles();
}

// Phase 35 of Module 1A (kEndSequence_Funcs[35]). Waits for the credits text scroll to fully
// stop (counts down R16 low byte from 0x80), then arms the palette filter for the final mosaic
// fade (darkening_or_lightening_screen=0, palette_filter_countdown=0, mosaic_target=0x1f) and
// initialises R16=0xc0 (192-frame disperse timer) and R18=0 (phase flag for FadeAndDisperse).
void Credits_StopCreditsScroll() {  // 8ec391
  if (!--BYTE(R16)) {
    darkening_or_lightening_screen = 0;
    palette_filter_countdown = 0;
    WORD(mosaic_target_level) = 0x1f;
    submodule_index++;
    R16 = 0xc0;
    R18 = 0;
  }
  Credits_AnimateTheTriangles();
}

// Phase 36 of Module 1A (kEndSequence_Funcs[36]). Fades the background palette out while the
// Triforce triangles disperse (AnimateSceneSprite_CreditsTriangle picks up velocity at submodule=36).
// Decrements R16 low byte each frame. Until R18 flags phase 2, runs the palette filter bounce;
// when the filter completes, sets R18=1. Once both filter is done and R16 reaches 0, advances
// to phase 37 (FadeInTheEnd) and calls PaletteFilter_WishPonds_Inner for a final palette flush.
void Credits_FadeAndDisperseTriangles() {  // 8ec3b8
  BYTE(R16)--;
  if (!BYTE(R18)) {
    ApplyPaletteFilter_bounce();
    if (BYTE(palette_filter_countdown)) {
      Credits_AnimateTheTriangles();
      return;
    }
    BYTE(R18)++;
  }
  if (BYTE(R16)) {
    Credits_AnimateTheTriangles();
    return;
  }
  submodule_index++;
  PaletteFilter_WishPonds_Inner();
}

// Phase 37 of Module 1A (kEndSequence_Funcs[37]). Fades in the "THE END" OAM graphic
// (drawn by Credits_HangForever) every 8 frames via PaletteFilter_SP5F. When the fade
// completes (palette_filter_countdown==0) advances to phase 38 (Credits_HangForever).
void Credits_FadeInTheEnd() {  // 8ec3d5
  if (!(frame_counter & 7)) {
    PaletteFilter_SP5F();
    if (!BYTE(palette_filter_countdown))
      submodule_index++;;
  }
  Credits_HangForever();
}

// Phase 38 of Module 1A (kEndSequence_Funcs[38]) and also called by FadeInTheEnd.
// Draws the four 16×16 OAM entries that spell out "THE END" centred on screen.
// Tiles 0x00, 0x02, 0x04, 0x06 at palette 3, priority 2, placed at X={-96,-80,-64,-48}, Y=-72
// (which maps to screen-centre coordinates after the camera offset). Once phase 38 is reached
// this function loops forever — there is no further state transition.
void Credits_HangForever() {  // 8ec41a
  SetOamPlain(&oam_buf[0], -96, -72, 0x00, 0x3b, 2);
  SetOamPlain(&oam_buf[1], -80, -72, 0x02, 0x3b, 2);
  SetOamPlain(&oam_buf[2], -64, -72, 0x04, 0x3b, 2);
  SetOamPlain(&oam_buf[3], -48, -72, 0x06, 0x3b, 2);
}

// Initialises the polyhedral NMI thread for the crystal-maiden rescue cutscene (the in-dungeon
// cutscene after each boss is defeated, separate from the title and credits Triforce). Uses
// model 0 (crystal geometry) at a moderate rotation speed (poly_a=16). poly_config1=156 sets
// the brightness/scale. Enables BG layers 2/3/4 (TM=0x16) and turns off sub-screen (TS=0).
void CrystalCutscene_InitializePolyhedral() {  // 9ecdd9
  poly_config1 = 156;
  poly_config_color_mode = 1;
  is_nmi_thread_active = 1;
  intro_did_run_step = 1;
  poly_base_x = 32;
  poly_base_y = 32;
  BYTE(poly_var1) = 32;
  poly_which_model = 0;  // Crystal model (as opposed to model 1 = Triforce)
  poly_a = 16;
  TS_copy = 0;
  TM_copy = 0x16;
}

