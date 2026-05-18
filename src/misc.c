/*
 * misc.c — Game Module Router, Boss Sequences, and Shared Utilities
 *
 * Acts as the central dispatcher and glue layer for the Zelda 3 game loop.
 * Implements the main module routing table (kMainRouting), the Agahnim boss fight
 * multi-phase state machine (KillAgahnim_*), boss victory sequences, item receipt
 * logic, torch/tile manipulation, and cross-cutting audio/SFX utilities.
 *
 * Key responsibilities:
 *   - Module routing: kMainRouting[main_module_index]() dispatches to every major
 *     game state (intro, file select, dungeon, overworld, menus, credits, etc.)
 *   - Item receipt: AncillaAdd_ItemReceipt() handles all 76 item types, updating
 *     the appropriate SRAM memory location and spawning the overhead item sprite
 *   - Boss victory: pendant and crystal boss victory sequences (heal → spin → exit)
 *   - Agahnim transition: 13-phase state machine for the dark world warp cutscene
 *   - Link DMA: NMI_PrepareSprites() resolves all sprite tile DMA source addresses
 *   - SFX panning: CalculateSfxPan() maps sprite X coordinates to left/center/right
 *   - Tile attr init: Init_LoadDefaultTileAttr() preloads tile collision defaults
 */
#include "misc.h"
#include "variables.h"
#include "hud.h"
#include "dungeon.h"
#include "overworld.h"
#include "load_gfx.h"
#include "sprite.h"
#include "poly.h"
#include "ancilla.h"
#include "select_file.h"
#include "tile_detect.h"
#include "player.h"
#include "player_oam.h"
#include "messaging.h"
#include "ending.h"
#include "attract.h"
#include "snes/snes_regs.h"
#include "assets.h"

static void KillAgahnim_LoadMusic();
static void KillAghanim_Init();
static void KillAghanim_Func2();
static void KillAghanim_Func3();
static void KillAghanim_Func4();
static void KillAghanim_Func5();
static void KillAghanim_Func6();
static void KillAghanim_Func7();
static void KillAghanim_Func8();
static void KillAghanim_Func12();
static uint8 PlaySfx_SetPan(uint8 a);

// Item display mode for each item type (76 items): 0 = small sprite, 2 = large sprite overhead
const uint8 kReceiveItem_Tab1[76] = {
  0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 2, 2, 2,
  2, 2, 2, 0, 2, 0, 2, 2, 0, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 0, 2, 2, 2, 2, 2, 0, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 0, 0, 2, 0, 2, 2, 2, 0, 2, 2,
};
// Y offset (from Link) for chest-received item sprite, signed (negative = above Link)
static const int8 kReceiveItem_Tab2[76] = {
  -5, -5, -5, -5, -5, -4, -4, -5, -5, -4, -4, -4, -2, -4, -4, -4,
  -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4,
  -4, -4, -4, -5, -4, -4, -4, -4, -4, -4, -2, -4, -4, -4, -4, -4,
  -4, -4, -4, -4, -2, -2, -2, -4, -4, -4, -4, -4, -4, -4, -4, -4,
  -4, -4, -2, -2, -4, -2, -4, -4, -4, -5, -4, -4,
};
// X offset (from Link or chest) for chest-received item sprite
static const uint8 kReceiveItem_Tab3[76] = {
  4, 4, 4, 4, 4, 0, 0, 4, 4, 4, 4, 4, 5, 0, 0, 0,
  0, 0, 0, 4, 0, 4, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0,
};
// Graphics tile index for each item's overhead sprite (indexes into sprite tile data)
const uint8 kReceiveItemGfx[76] = {
  6, 0x18, 0x18, 0x18, 0x2d, 0x20, 0x2e,    9,    9,  0xa,    8,    5, 0x10,  0xb, 0x2c, 0x1b,
  0x1a, 0x1c, 0x14, 0x19,  0xc,    7, 0x1d, 0x2f,    7, 0x15, 0x12,  0xd,  0xd,  0xe, 0x11, 0x17,
  0x28, 0x27,    4,    4,  0xf, 0x16,    3, 0x13,    1, 0x1e, 0x10,    0,    0,    0,    0,    0,
  0, 0x30, 0x22, 0x21, 0x24, 0x24, 0x24, 0x23, 0x23, 0x23, 0x29, 0x2a, 0x2c, 0x2b,    3,    3,
  0x34, 0x35, 0x31, 0x33,    2, 0x32, 0x36, 0x37, 0x2c,    6,  0xc, 0x38,
};
// SRAM/WRAM address (in g_ram) to write when each item is received.
// Indexed by link_receiveitem_index. Addresses map to equipment/inventory slots.
const uint16 kMemoryLocationToGiveItemTo[76] = {
  0xf359, 0xf359, 0xf359, 0xf359,
  0xf35a, 0xf35a, 0xf35a, 0xf345,
  0xf346, 0xf34b, 0xf342, 0xf340,
  0xf341, 0xf344, 0xf35c, 0xf347,
  0xf348, 0xf349, 0xf34a, 0xf34c,
  0xf34c, 0xf350, 0xf35c, 0xf36b,
  0xf351, 0xf352, 0xf353, 0xf354,
  0xf354, 0xf34e, 0xf356, 0xf357,
  0xf37a, 0xf34d, 0xf35b, 0xf35b,
  0xf36f, 0xf364, 0xf36c, 0xf375,
  0xf375, 0xf344, 0xf341, 0xf35c,
  0xf35c, 0xf35c, 0xf36d, 0xf36e,
  0xf36e, 0xf375, 0xf366, 0xf368,
  0xf360, 0xf360, 0xf360, 0xf374,
  0xf374, 0xf374, 0xf340, 0xf340,
  0xf35c, 0xf35c, 0xf36c, 0xf36c,
  0xf360, 0xf360, 0xf372, 0xf376,
  0xf376, 0xf373, 0xf360, 0xf360,
  0xf35c, 0xf359, 0xf34c, 0xf355,
};
// Value to write into the item memory location when received.
// Negative values indicate special handling (e.g., -1 = increment, -5 = bomb capacity, -100/-50 = rupee amounts).
static const int8 kValueToGiveItemTo[76] = {
     1,   2,   3,  4,
     1,   2,   3,  1,
     1,   1,   1,  1,
     1,   2,  -1,  1,
     1,   1,   1,  1,
     2,   1,  -1, -1,
     1,   1,   2,  1,
     2,   1,   1,  1,
    -1,   1,  -1,  2,
    -1,  -1,  -1, -1,
    -1,  -1,   2, -1,
    -1,  -1,  -1, -1,
    -1,  -1,  -1, -1,
    -1,  -5, -20, -1,
    -1,  -1,   1,  3,
    -1,  -1,  -1, -1,
  -100, -50,  -1,  1,
    10,  -1,  -1, -1,
    -1,   1,   3,  1,
};
// Default tile collision/interaction attributes for all 384 map16 tile slots.
// Loaded into attributes_for_tile at game start. These define how each tile type
// behaves: 0=walk, 1=wall, 2=ledge, 4=staircase, etc.
static const uint8 kDungeon_DefaultAttr[384] = {
  1, 1, 1, 0, 2, 1, 2, 0, 1, 1, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 0, 0, 1, 0, 0, 2, 0, 0, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 1, 1, 0, 0,
  2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 1, 1, 0, 0,
  0, 0, 0, 0x2a, 1, 0x20, 1, 1, 4, 1, 1, 0x18, 1, 2, 0x1c, 1,
  0x28, 0x28, 0x2a, 0x2a, 1, 2, 1, 1, 4, 0, 0, 0, 0x28, 1, 0xa, 0,
  1, 1, 0xc, 0xc, 2, 2, 2, 2, 0x28, 0x2a, 0x20, 0x20, 0x20, 2, 8, 0,
  4, 4, 1, 1, 1, 2, 2, 2, 0, 0, 0x20, 0x20, 0, 2, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0x18, 0x10, 0x10, 1, 1, 1,
  1, 1, 4, 4, 4, 4, 4, 4, 1, 2, 2, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x62, 0x62,
  0, 0, 0x24, 0x24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x62, 0x62,
  0x27, 2, 2, 2, 0x27, 0x27, 1, 0, 0, 0, 0, 0x24, 0, 0, 0, 0,
  0x27, 0x27, 0x27, 0x27, 0x27, 0x10, 2, 1, 0, 0, 0, 0x24, 0, 0, 0, 0,
  0x27, 2, 2, 2, 0x27, 0x27, 0x27, 0x27, 2, 2, 2, 0x24, 0, 0, 0, 0,
  0x27, 0x27, 0x27, 0x27, 0x27, 0x20, 2, 2, 1, 2, 2, 0x23, 2, 0, 0, 0,
  0x27, 0x27, 0x27, 0x27, 0x27, 0x20, 2, 0x27, 2, 0x54, 0, 0, 0x27, 2, 2, 2,
  0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 2, 0x27, 2, 0x54, 0, 0, 0x27, 2, 2, 2,
  0x27, 0x27, 0, 0x27, 0x60, 0x60, 1, 1, 1, 1, 2, 2, 0xd, 0, 0, 0x4b,
  0x67, 0x67, 0x67, 0x67, 0x66, 0x66, 0x66, 0x66, 0, 0, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
  0x27, 0x63, 0x27, 0x55, 0x55, 1, 0x44, 0, 1, 0x20, 2, 2, 0x1c, 0x3a, 0x3b, 0,
  0x27, 0x63, 0x27, 0x53, 0x53, 1, 0x44, 1, 0xd, 0, 0, 0, 9, 9, 9, 9,
};
// Pendant boss victory state machine (6 phases): heal → spin → exit with spotlight
static PlayerHandlerFunc *const kModule_BossVictory[6] = {
  &BossVictory_Heal,
  &Dungeon_StartVictorySpin,
  &Dungeon_RunVictorySpin,
  &Dungeon_CloseVictorySpin,
  &Dungeon_PrepExitWithSpotlight,
  &Spotlight_ConfigureTableAndControl,
};
// Agahnim defeat 13-phase state machine: load dark world music → mirror warp HDMA → dialogue → overworld
static PlayerHandlerFunc *const kModule_KillAgahnim[13] = {
  &KillAgahnim_LoadMusic,
  &KillAghanim_Init,
  &KillAghanim_Func2,
  &KillAghanim_Func3,
  &KillAghanim_Func4,
  &KillAghanim_Func5,
  &KillAghanim_Func6,
  &KillAghanim_Func7,
  &KillAghanim_Func8,
  &BossVictory_Heal,
  &Dungeon_StartVictorySpin,
  &Dungeon_RunVictorySpin,
  &KillAghanim_Func12,
};
// Primary game module dispatch table. main_module_index selects which major state is active.
// Module indices: 0=Intro, 1=FileSelect, 2=CopyFile, 3=DeleteFile, 4=NameFile,
// 5=LoadFile, 6=PreDungeon, 7=Dungeon, 8=OWLoad, 9=Overworld, 0xA-B=OW(repeat),
// 0xE=Interface/Dialog, 0xF=SpotlightClose, 0x10=SpotlightOpen, 0x11=FallingEntrance,
// 0x12=GameOver, 0x13=BossVictory_Pendant, 0x14=Attract, 0x15=MirrorWarpFromAga,
// 0x16=BossVictory_Crystal, 0x17=SaveAndQuit, 0x18=GanonEmerges, 0x19=TriforceRoom,
// 0x1A=Credits, 0x1B=SpawnSelect
static PlayerHandlerFunc *const kMainRouting[28] = {
  &Module00_Intro,
  &Module01_FileSelect,
  &Module02_CopyFile,
  &Module03_KILLFile,
  &Module04_NameFile,
  &Module05_LoadFile,
  &Module_PreDungeon,
  &Module07_Dungeon,
  &Module08_OverworldLoad,
  &Module09_Overworld,
  &Module08_OverworldLoad,
  &Module09_Overworld,
  &Module_Unknown0,
  &Module_Unknown1,
  &Module0E_Interface,
  &Module0F_SpotlightClose,
  &Module10_SpotlightOpen,
  &Module11_DungeonFallingEntrance,
  &Module12_GameOver,
  &Module13_BossVictory_Pendant,
  &Module14_Attract,
  &Module15_MirrorWarpFromAga,
  &Module16_BossVictory_Crystal,
  &Module17_SaveAndQuit,
  &Module18_GanonEmerges,
  &Module19_TriforceRoom,
  &Module1A_Credits,
  &Module1B_SpawnSelect,
};

// Converts a tile data byte offset into a pointer into the predefined tile data array.
// Used by torch lighting to look up pre-composed 2x2 tile sets.
const uint16 *SrcPtr(uint16 src) {
  return &kPredefinedTileData[src >> 1];
}

// Plays a sound effect in sound_effect_1 slot with spatial pan based on Link's X position.
// Returns the full SFX byte (id | pan bits).
uint8 Ancilla_Sfx2_Near(uint8 a) {
  return sound_effect_1 = PlaySfx_SetPan(a);
}

// Plays a sound effect in sound_effect_2 slot with spatial pan based on Link's X position.
void Ancilla_Sfx3_Near(uint8 a) {
  sound_effect_2 = PlaySfx_SetPan(a);
}

// Transitions into a dungeon room from a save load. Clears mosaic, rebuilds HUD,
// and calls Module_PreDungeon to complete room setup.
void LoadDungeonRoomRebuildHUD() {
  mosaic_level = 0;
  MOSAIC_copy = 7;
  Hud_SearchForEquippedItem();
  Hud_Rebuild();
  Hud_UpdateEquippedItem();
  Module_PreDungeon();
}

// Placeholder for an unused module slot (index 0xC). Should never be called in normal gameplay.
void Module_Unknown0() {
  assert(0);
}

// Placeholder for an unused module slot (index 0xD). Should never be called in normal gameplay.
void Module_Unknown1() {
  assert(0);
}

// Phase 0: Re-enable NMI core updates and advance to phase 1. Loads overworld music.
static void KillAgahnim_LoadMusic() {
  nmi_disable_core_updates = 0;
  overworld_map_state++;
  submodule_index++;
  LoadOWMusicIfNeeded();
}

// Phase 1: Begins the mirror-warp HDMA effect and loads overworld graphics.
// Sets Link to Mirror state, clears velocity, and flashes the palette white.
static void KillAghanim_Init() {
  music_control = 8;
  BYTE(overworld_screen_trans_dir_bits) = 8;
  InitializeMirrorHDMA();
  overworld_map_state = 0;
  PaletteFilter_InitializeWhiteFilter();
  Overworld_LoadGFXAndScreenSize();
  submodule_index++;
  link_player_handler_state = kPlayerState_Mirror;
  bg1_x_offset = 0;
  bg1_y_offset = 0;
  dung_savegame_state_bits = 0;
  WORD(link_y_vel) = 0;
  main_palette_buffer[0] = 0x7fff;
  main_palette_buffer[32] = 0x7fff;
  Ancilla_TerminateSelectInteractives(0);
  Link_ResetProperties_A();
}

// Phase 2: Activates HDMA channels (bits 6+7 = 0xC0 = 192) and starts building the
// waving distortion table. Resets subsubmodule_index so Func3 can poll for completion.
static void KillAghanim_Func2() {
  HDMAEN_copy = 192;
  MirrorWarp_BuildWavingHDMATable();
  submodule_index++;
  subsubmodule_index = 0;
}

// Phase 3: Continues updating the waving HDMA table each frame.
// Advances to the de-wave phase once MirrorWarp_BuildWavingHDMATable signals done
// by setting subsubmodule_index non-zero.
static void KillAghanim_Func3() {
  MirrorWarp_BuildWavingHDMATable();
  if (subsubmodule_index) {
    subsubmodule_index = 0;
    submodule_index++;
  }
}

// Phase 4: Runs the de-wave (reverse distortion) HDMA table until complete.
// When finished, advances to Func5 which tears down HDMA and shows dialogue.
static void KillAghanim_Func4() {
  MirrorWarp_BuildDewavingHDMATable();
  if (subsubmodule_index) {
    subsubmodule_index = 0;
    submodule_index++;
  }
}

// Phase 5: Tears down the mirror-warp HDMA, shows dialogue 0x35 (Agahnim defeated text),
// reloads overworld graphics sheets, rebuilds the HUD, and transitions main_module_index
// to 21 (MirrorWarpFromAga). subsubmodule_index = 24 gives a 24-frame delay before Func6.
static void KillAghanim_Func5() {
  HdmaSetup(0, 0xf2fb, 0x41, 0, (uint8)WH0, 0);
  // Fill entire HDMA table with 0xFF00 (disable all scanlines) to clear the warp effect.
  for (int i = 0; i < 240; i++)
    hdma_table_dynamic[i] = 0xff00;
  palette_filter_countdown = 0;
  darkening_or_lightening_screen = 0;
  dialogue_message_index = 0x35;
  Main_ShowTextMessage();
  ReloadPreviouslyLoadedSheets();
  Hud_RebuildIndoor();
  HDMAEN_copy = 0x80;
  main_module_index = 21;
  submodule_index = 6;
  subsubmodule_index = 24;
}

// Phase 6: Countdown timer (24 frames from Func5). When it hits zero, advances to Func7
// and plays ambient sound effect 9 (dark world wind ambience).
static void KillAghanim_Func6() {
  if (!--subsubmodule_index) {
    submodule_index++;
    sound_effect_ambient = 9;
  }
}

// Phase 7: Renders the dialogue box for the Agahnim cutscene text.
// When the dialogue finishes (submodule_index resets to 0 by messaging system),
// checks if Link has the Moon Pearl: without it shows dialogue 0x36 (bunny warning)
// and goes to Func8; with it skips directly to the heal phase (submodule_index 9).
static void KillAghanim_Func7() {
  RenderText();
  if (!submodule_index) {
    overworld_map_state = 0;
    sound_effect_ambient = 5;
    if (!link_item_moon_pearl) {
      dialogue_message_index = 0x36;
      Main_ShowTextMessage();
      sound_effect_ambient = 0;
      main_module_index = 21;
      submodule_index = 8;
    } else {
      submodule_index = 9;
    }
  }
}

// Phase 8 (no Moon Pearl branch): Renders the bunny-warning dialogue.
// When complete, sets a 32-frame delay and jumps to Func12 to finalize overworld entry.
static void KillAghanim_Func8() {
  RenderText();
  if (!submodule_index) {
    subsubmodule_index = 32;
    submodule_index = 12;
  }
}

// Phase 12: Final countdown before transitioning to the overworld.
// Clears all ancilla/cutscene state, sets the song list, marks event bit 0x1b/bit5
// (Agahnim defeated flag in save_ow_event_info), resets module to overworld (9),
// and selects the dark-world or light-world overworld music based on moon pearl ownership.
static void KillAghanim_Func12() {
  if (--subsubmodule_index)
    return;
  ResetAncillaAndCutscene();
  Overworld_SetSongList();
  // Mark the Agahnim-defeated event so the overworld reflects the bridge opening.
  save_ow_event_info[0x1b] |= 32;
  BYTE(cur_palace_index_x2) = 255;
  submodule_index = 0;
  overworld_map_state = 0;
  nmi_disable_core_updates = 0;
  main_module_index = 9;
  BYTE(BG1VOFS_copy2) = 0;
  // Music 9 = dark world theme; music 4 = light world theme (played if no moon pearl).
  music_control = link_item_moon_pearl ? 9 : 4;
  savegame_map_icons_indicator = 6;
}

// Central game-loop dispatcher. Called every frame from zelda_rtl.c.
// Indexes into kMainRouting with main_module_index to invoke the active game state handler.
void Module_MainRouting() {  // 8080b5
  kMainRouting[main_module_index]();
}

// Resolves all Link sprite DMA source addresses for the current animation frame.
// Called during NMI (vertical blank) preparation each frame to set up the
// dma_source_addr_* variables that the DMA engine will copy to VRAM.
//
// Link's graphics are split across multiple VRAM banks. Each animation state
// (link_dma_graphics_index, link_dma_var1-5) indexes into parallel lookup tables
// (kLinkDmaSources1..9) to find the correct tile data address for each body section.
// Sections include: torso (0/3), legs (1/4), sword arm (5/2), shield arm (6/11),
// cape (7/12), hair (8/13), background tiles (9/14), equipment overlays (10/15),
// and travel bird / bunny form (16-21).
//
// Also advances two animation cycles:
//   bg_tile_animation_countdown — cycles water/lava tile animation (every 9 or 23 frames).
//   word_7EC013 — cycles the sword/shield glint animation through kLinkDmaSources9.
void NMI_PrepareSprites() {  // 8085fc
  static const uint16 kLinkDmaSources1[303] = {
    0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x9440, 0x8080, 0x8080, 0x8080, 0x9400, 0x8040, 0x80c0, 0x80c0, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000,
    0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8000, 0xa8c0, 0xa900, 0x8000, 0xa8c0, 0xa900,
    0x9100, 0x8080, 0x8080, 0x90c0, 0x8040, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x9a00, 0x9140, 0x9180, 0x8000, 0x9500,
    0x9480, 0x94c0, 0x94c0, 0x9ae0, 0x8080, 0x8080, 0x9a60, 0x80c0, 0x80c0, 0x9aa0, 0x8000, 0x8000, 0x9aa0, 0x8000, 0x8000, 0x8080,
    0x8080, 0x8100, 0x8100, 0x85c0, 0x8000, 0x8000, 0x85c0, 0x8000, 0x8000, 0xadc0, 0xadc0, 0xadc0, 0xadc0, 0xadc0, 0xad40, 0xad40,
    0xad40, 0xad40, 0xad40, 0xad80, 0xad80, 0xad80, 0xad80, 0xad80, 0xad80, 0x8040, 0x9400, 0x8040, 0x8000, 0x8080, 0x8080, 0x9440,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8080, 0x8040, 0x8040, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0xc440, 0x8140, 0x8140,
    0xca40, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8040, 0x85c0, 0x8040, 0x85c0, 0x8100, 0x80c0, 0x91c0, 0x8080, 0x8080,
    0x8040, 0x8040, 0x8000, 0x8000, 0x8000, 0x8000, 0x8080, 0x8080, 0x9100, 0xa0c0, 0xa100, 0xa100, 0xa1c0, 0xa400, 0xa440, 0xa1c0,
    0xa400, 0xa440, 0x8080, 0xc480, 0x8080, 0x8040, 0x8040, 0xca80, 0xca80, 0xca00, 0xc400, 0xca00, 0xc400, 0x81c0, 0x8080, 0x8080,
    0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8040, 0x8000, 0xa8c0, 0xa900,
    0x8000, 0x8000, 0xa8c0, 0xa900, 0x8000, 0xa8c0, 0xa900, 0x8000, 0x8000, 0xa8c0, 0xa900, 0x8040, 0x8040, 0x8040, 0x8080, 0x8080,
    0x8040, 0x8040, 0x8040, 0x8040, 0x8000, 0x8000, 0x8000, 0x8000, 0xd080, 0x8080, 0x90c0, 0xd000, 0x9080, 0xd040, 0x9080, 0xd040,
    0xd080, 0xd080, 0xd080, 0xd080, 0xd080, 0xd000, 0xd000, 0xd000, 0xd000, 0xd000, 0xd040, 0xd040, 0xd040, 0xd040, 0xd040, 0xd040,
    0x8040, 0xd000, 0x85c0, 0x85c0, 0x85c0, 0xdc40, 0xdc40, 0xdc40, 0x85c0, 0x85c0, 0x85c0, 0xdc40, 0xdc40, 0xdc40, 0xe1c0, 0xd000,
    0x8000, 0xe400, 0xe400, 0xe440, 0x90c0, 0x90c0, 0xd000, 0x8000, 0x8000, 0xd040, 0x8000, 0x8000, 0xd040, 0xe400, 0xe400, 0xe400,
    0x9080, 0xa5c0, 0xac40, 0xe480, 0x8180, 0x90c0, 0x80c0, 0xe180, 0xd000, 0xe4c0, 0xe4c0, 0xe840, 0xe840, 0xe840, 0xe540, 0xe540,
    0xe540, 0xe900, 0xe900, 0xe900, 0xe900, 0x8080, 0x8080, 0x8000, 0xa9c0, 0x8080, 0x8140, 0x91c0, 0x8040, 0xa800, 0xa840,
  };
  static const uint16 kLinkDmaSources2[303] = {
    0x8840, 0x8800, 0x8580, 0x8800, 0x8580, 0x84c0, 0x8500, 0x8540, 0x8500, 0x8540, 0x8400, 0x8440, 0x8480, 0x8400, 0x8440, 0x8480,
    0x9640, 0x8c40, 0x8c80, 0xad00, 0x9600, 0x8980, 0x8c00, 0xacc0, 0x8880, 0x88c0, 0x8900, 0x8940, 0x8880, 0x88c0, 0x8900, 0x8940,
    0xb0c0, 0xb100, 0xb140, 0xb100, 0xb140, 0xb000, 0xb040, 0xb080, 0xec80, 0xecc0, 0xb180, 0xd440, 0xb1c0, 0xb180, 0xd440, 0xb1c0,
    0x8c80, 0xad00, 0x95c0, 0x99c0, 0xb440, 0x9580, 0xb480, 0xb4c0, 0x9580, 0xb480, 0xb4c0, 0x9c20, 0x8000, 0x8000, 0x8000, 0x9700,
    0x9680, 0x96c0, 0x96c0, 0x9ce0, 0x8c80, 0xb540, 0x9c60, 0xb580, 0x8c00, 0x9ca0, 0x8900, 0xb500, 0x9ca0, 0x8900, 0xb500, 0x8c40,
    0xec40, 0x8c00, 0xec00, 0x8dc0, 0x9540, 0x89c0, 0x8dc0, 0x9540, 0x89c0, 0xb940, 0xb980, 0xb9c0, 0xb980, 0xb9c0, 0xb5c0, 0xb800,
    0xb840, 0xb800, 0xb840, 0xb880, 0xb8c0, 0xb900, 0xb880, 0xb8c0, 0xb900, 0x8980, 0x9600, 0xbcc0, 0x8400, 0xbc80, 0x8c40, 0x9640,
    0xa040, 0xa080, 0xa000, 0xbc40, 0xbd40, 0x8500, 0xbd00, 0xbd80, 0xbd80, 0x88c0, 0x8900, 0xe9c0, 0x8900, 0xc640, 0xc040, 0xc000,
    0xcc40, 0x8940, 0x88c0, 0x8900, 0xe9c0, 0x8900, 0x8940, 0x8d40, 0x8d80, 0x8d40, 0x8d80, 0xbd00, 0xb000, 0xb000, 0xa480, 0xa480,
    0xa480, 0xa480, 0xac00, 0xac00, 0xac00, 0xac00, 0xa140, 0xa180, 0xa180, 0xa4c0, 0xa4c0, 0xa500, 0x9d40, 0x9d80, 0x9dc0, 0x9d40,
    0x9d80, 0x9dc0, 0x8d00, 0xc680, 0xc180, 0xc140, 0x8c00, 0xcc80, 0xcc80, 0xcc00, 0xc600, 0xcc00, 0xc600, 0xbd00, 0x8580, 0x8800,
    0xc9c0, 0xccc0, 0xcdc0, 0xcd00, 0xcd40, 0xcd80, 0x8500, 0x8540, 0xc940, 0xc980, 0x8540, 0xc940, 0xc980, 0x8440, 0x8480, 0xc1c0,
    0xc900, 0xc580, 0xc5c0, 0xc8c0, 0x8440, 0x8480, 0xc1c0, 0xc900, 0xc580, 0xc5c0, 0xc8c0, 0xbd00, 0xacc0, 0xc040, 0xd540, 0xd580,
    0xd4c0, 0xd500, 0xd4c0, 0xd500, 0xd440, 0xd480, 0xd440, 0xd480, 0xd1c0, 0xd400, 0xd100, 0xd100, 0xd140, 0xd180, 0xd140, 0xd180,
    0xb0c0, 0xb100, 0xb140, 0xb100, 0xb140, 0xdd40, 0xdd80, 0xddc0, 0xdd80, 0xddc0, 0xdc80, 0xdcc0, 0xdd00, 0xdc80, 0xdcc0, 0xdd00,
    0xd100, 0xd100, 0xe000, 0xe040, 0xe080, 0xe0c0, 0xe100, 0xe140, 0xe000, 0xe040, 0xe080, 0xe0c0, 0xe100, 0xe140, 0x8000, 0xd0c0,
    0x8000, 0xb940, 0xb980, 0xb940, 0xdd40, 0xdd80, 0xdd40, 0xdc80, 0xdcc0, 0xc0c0, 0xdc80, 0xdcc0, 0xc0c0, 0xb9c0, 0xb980, 0xb9c0,
    0xa560, 0xa5a0, 0xac80, 0xed00, 0x8000, 0x8cc0, 0xbd00, 0xe380, 0xbdc0, 0xe500, 0xe500, 0xe880, 0xe8c0, 0xe8c0, 0xe800, 0xe5c0,
    0xe5c0, 0xe940, 0xe980, 0xe940, 0xe980, 0xbd40, 0x8c80, 0xa080, 0x8000, 0xa980, 0xbd00, 0xbdc0, 0xb400, 0xa880, 0xedc0,
  };
  static const uint16 kLinkDmaSources3[27] = {
    0x9a40, 0x9e00, 0x9d20, 0x9f20, 0x9b20, 0xbc20, 0xbc20, 0xbe20, 0xbe20, 0xbe00, 0xbe00, 0xbe00, 0xbe00, 0xa540, 0xa540, 0xa540,
    0xa540, 0xbc00, 0xbc00, 0xbc00, 0xbc00, 0xa740, 0xa740, 0xa740, 0xa740, 0xe780, 0xe780,
  };
  static const uint16 kLinkDmaSources4[8] = { 0x9000, 0x9020, 0x9060, 0x91e0, 0x90a0, 0x90c0, 0x9100, 0x9140 };
  static const uint16 kLinkDmaSources5[3] = { 0x9300, 0x9340, 0x9380 };
  static const uint16 kLinkDmaSources6[128] = {
    0x9480, 0x94c0, 0x94e0, 0x95c0, 0x9500, 0x9520, 0x9540, 0x9480, 0x9640, 0x9680, 0x96a0, 0x9780, 0x96c0, 0x96e0, 0x9700, 0x9480,
    0x9800, 0x9840, 0x98a0, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9ac0, 0x9b00, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480,
    0x9bc0, 0x9c00, 0x9c40, 0x9c80, 0x9cc0, 0x9d00, 0x9d40, 0x9480, 0x9f40, 0x9f80, 0x9fc0, 0x9fe0, 0xa000, 0x9480, 0x9480, 0x9480,
    0xa100, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480,
    0x98c0, 0x9900, 0x99c0, 0x99e0, 0x9a00, 0x9a20, 0x9a40, 0x9a60, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480,
    0x9a80, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480,
    0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480,
    0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480, 0x9480,
  };
  static const uint16 kLinkDmaSources7[16] = { 0xe0, 0xe0, 0x60, 0x80, 0x1c0, 0xe0, 0x40, 0, 0x80, 0, 0x40, 0, 0, 0, 0, 0 };
  static const uint16 kLinkDmaCtrs0[6] = { 14, 4, 6, 16, 6, 8 };
  static const uint16 kLinkDmaSources9[15] = { 0, 0x20, 0x40, 0, 0x20, 0x40, 0, 0x40, 0x80, 0, 0x40, 0x80, 0xb340, 0xb400, 0xb4c0 };
  static const uint16 kLinkDmaSources8[4] = { 0xa480, 0xa4c0, 0xa500, 0xa540 };

  // Pack 4 OAM size bits (one per sprite slot) into a single 16-bit extended OAM word.
  // The SNES OAM format stores the high bit (large/small sprite flag) for groups of 4
  // sprites in a packed byte; here we build the packed words from the per-slot array.
  for (int i = 0; i < 32; i++) {
    extended_oam[i] = bytewise_extended_oam[3 + 4 * i] << 6 |
      bytewise_extended_oam[2 + 4 * i] << 4 |
      bytewise_extended_oam[1 + 4 * i] << 2 |
      bytewise_extended_oam[0 + 4 * i] << 0;
  }

  // Resolve Link torso/legs DMA source addresses from the primary animation index.
  // Sources 0/3 are the front half of each bank; +0x200 gives the back half tile row.
  dma_source_addr_3 = kLinkDmaSources1[link_dma_graphics_index >> 1];
  dma_source_addr_0 = dma_source_addr_3 + 0x200;
  dma_source_addr_4 = kLinkDmaSources2[link_dma_graphics_index >> 1];
  dma_source_addr_1 = dma_source_addr_4 + 0x200;
  // link_dma_var1/2 control the sword-arm and shield-arm tile banks (kLinkDmaSources3 is shared).
  dma_source_addr_5 = kLinkDmaSources3[link_dma_var1 >> 1];
  dma_source_addr_2 = kLinkDmaSources3[link_dma_var2 >> 1];

  // Resolve the bunny-ear / hat tile bank; +0x180 gives the second tile row.
  dma_source_addr_6 = kLinkDmaSources4[link_dma_var3 >> 1];
  dma_source_addr_11 = dma_source_addr_6 + 0x180;

  // Special case 0x8b: Link is swimming; use a fixed alternate tile address.
  if (link_dma_var4 == 0x8b) {
    dma_source_addr_7 = 0xe099;
  } else {
    dma_source_addr_7 = kLinkDmaSources5[link_dma_var4 >> 1];
  }
  dma_source_addr_12 = dma_source_addr_7 + 0xc0;

  // kLinkDmaSources7 provides the bank offset for the upper half of the cape/hair tile;
  // j is the high 5 bits of the variant index, selecting one of 16 cape animation offsets.
  int j = (link_dma_var5 & 0xf8) >> 3;
  dma_source_addr_8 = kLinkDmaSources6[link_dma_var5];
  dma_source_addr_13 = dma_source_addr_8 + kLinkDmaSources7[j];
  // Pushed-block tile: cycles through 4 variants based on which block type is being carried.
  dma_source_addr_10 = kLinkDmaSources8[pushedblocks_some_index & 3];
  dma_source_addr_15 = dma_source_addr_10 + 0x100;

  // Advance the background tile animation counter. Water tiles cycle every 9 frames;
  // overlays 0xB5/0xBC (special lava rooms) use a slower 23-frame (0x17) rate.
  // word_7EC00F cycles through three 0x400-byte banks (0, 0x400, 0x800) then wraps.
  if (--bg_tile_animation_countdown == 0) {
    bg_tile_animation_countdown = (BYTE(overlay_index) == 0xb5 || BYTE(overlay_index) == 0xbc) ? 0x17 : 9;

    uint16 t = word_7EC00F + 0x400;
    if (t == 0xc00)
      t = 0;
    word_7EC00F = t;
    animated_tile_data_src = 0xa680 + word_7EC00F;
  }

  // Advance the sword/shield glint animation. kLinkDmaSources9 has 6 pairs of source
  // addresses (indices 0-10); kLinkDmaCtrs0 gives the frame duration for each phase.
  // Addresses 9/14 are the two DMA channels for the glint overlay tiles.
  if (--word_7EC013 == 0) {
    int t = word_7EC015 + 2;
    if (t == 12)
      t = 0;
    word_7EC015 = t;
    word_7EC013 = kLinkDmaCtrs0[t >> 1];
    dma_source_addr_9 = kLinkDmaSources9[t >> 1] + 0xb280;
    dma_source_addr_14 = dma_source_addr_9 + 0x60;
  }

  // Resolve the two sprite-overlay DMA channels (16/18 and 17/19) driven by dma_var6/7
  // (e.g. tunic color, gloves). Base address 0xB940; +0x200 gives the second tile row.
  dma_source_addr_16 = 0xB940 + dma_var6 * 2;
  dma_source_addr_18 = dma_source_addr_16 + 0x200;

  dma_source_addr_17 = 0xB940 + dma_var7 * 2;
  dma_source_addr_19 = dma_source_addr_17 + 0x200;

  // Travel-bird tile source (Flute bird form). flag_travel_bird selects the frame;
  // base 0xB540. Channels 20 and 21 carry the two tile rows of the bird sprite.
  dma_source_addr_20 = 0xB540 + flag_travel_bird * 2;
  dma_source_addr_21 = dma_source_addr_20 + 0x200;
}

// Loads the intro/title music bank into the SPC700 sound driver.
// Called during Module00_Intro initialization.
void Sound_LoadIntroSongBank() {  // 808901
  LoadSongBank(kSoundBank_intro);
}

// Loads the overworld music bank (same bank as intro) into the SPC700 driver.
// Called when transitioning to the outdoor overworld module.
void LoadOverworldSongs() {  // 808913
  LoadSongBank(kSoundBank_intro);
}

// Loads the dungeon/indoor music bank into the SPC700 driver.
// Called when entering a dungeon or indoor area.
void LoadDungeonSongs() {  // 808925
  LoadSongBank(kSoundBank_indoor);
}

// Loads the ending/credits music bank into the SPC700 driver.
// Called during the credits sequence (Module1A).
void LoadCreditsSongs() {  // 808931
  LoadSongBank(kSoundBank_ending);
}

// Lights a dungeon torch identified by byte_7E0333 (high nibble must be 0xC0 = valid torch event;
// low nibble = torch slot index within the current room's torch list).
// Applies lit-torch tile graphics via RoomDraw_AdjustTorchLightingChange, plays the torch SFX
// with spatial panning, and if the room requires all torches lit (dung_want_lights_out), updates
// the ambient light color and triggers a room-brightening submodule.
// r8 (0x80 or 0xC0) is the torch burn-out timer value: 0x80 for room 0, 0xC0 otherwise.
void Dungeon_LightTorch() {  // 81f3ec
  // High nibble must be 0xC = valid torch-light trigger; anything else clears and returns.
  if ((byte_7E0333 & 0xf0) != 0xc0) {
    byte_7E0333 = 0;
    return;
  }
  uint8 r8 = (uint8)dungeon_room_index == 0 ? 0x80 : 0xc0;

  // Locate the torch in the room object list using the slot index (low nibble of byte_7E0333).
  int i = (byte_7E0333 & 0xf) + (dung_index_of_torches_start >> 1);
  int opos = dung_object_pos_in_objdata[i];
  // Bit 15 of the tilemap position flags this torch as already lit; skip if so.
  if (dung_object_tilemap_pos[i] & 0x8000)
    return;
  dung_object_tilemap_pos[i] |= 0x8000;
  if (r8 == 0)
    dung_torch_data[opos] = dung_object_tilemap_pos[i];

  uint16 x = dung_object_tilemap_pos[i] & 0x3fff;
  // Swap torch tile set 0xECA (lit flame) into the BG tilemap at position x.
  RoomDraw_AdjustTorchLightingChange(x, 0xeca, x);

  // SFX 42 = torch ignite; pan is derived from torch's X position relative to screen center.
  sound_effect_1 = 42 | CalculateSfxPan_Arbitrary((x & 0x7f) * 2);

  nmi_copy_packets_flag = 1;
  // dung_want_lights_out rooms require all torches lit before the door opens.
  // Each torch lit increments dung_num_lit_torches and brightens the fixed color addend.
  if (dung_want_lights_out) {
    if (dung_num_lit_torches++ < 3) {
      TS_copy = 0;
      overworld_fixed_color_plusminus = kLitTorchesColorPlus[dung_num_lit_torches];
      submodule_index = 10;
      subsubmodule_index = 0;
    }
  }

  dung_torch_timers[byte_7E0333 & 0xf] = r8;
  byte_7E0333 = 0;
}

// Writes a 2×2 predefined tile set (indexed by y) into overworld_tileattr at position x,
// then queues a VRAM DMA packet so the change is uploaded to VRAM during the next NMI.
// x = tilemap word index (divided by 2 internally); y = tile data byte offset into kPredefinedTileData.
// r8 = VRAM destination address for the DMA packet (same as x here for torch tiles).
void RoomDraw_AdjustTorchLightingChange(uint16 x, uint16 y, uint16 r8) {  // 81f746
  const uint16 *ptr = SrcPtr(y);
  x >>= 1;
  // Copy 2×2 tile words into the tileattr shadow buffer (row-major, 64 words per row).
  overworld_tileattr[x + 0] = ptr[0];
  overworld_tileattr[x + 64] = ptr[1];
  overworld_tileattr[x + 1] = ptr[2];
  overworld_tileattr[x + 65] = ptr[3];
  Dungeon_PrepOverlayDma_nextPrep(0, r8);
}

// Prepares a VRAM overlay DMA packet starting at buffer slot dst for the tile at r8.
// r6 selects the BG plane: 0x880 (plane 0) if column offset < 0x3A, else 0x881 (plane 1).
// Delegates to Dungeon_PrepOverlayDma_watergate for the actual packet construction.
int Dungeon_PrepOverlayDma_nextPrep(int dst, uint16 r8) {  // 81f764
  uint16 r6 = 0x880 + ((r8 & 0x3f) >= 0x3a);
  return Dungeon_PrepOverlayDma_watergate(dst, r8, r6, 4);
}

// Builds 'loops' VRAM DMA upload entries in vram_upload_tile_buf starting at dst.
// Each entry is a 6-word packet: [VRAM_dest, size/ctrl, word0, word1, word2, word3].
// r6 bit 0 selects horizontal (0) vs. vertical (1) tile layout, determining which
// overworld_tileattr offsets feed the four tile words. r8 advances by a full tile row
// (128 bytes horizontally) or 2 bytes (2 tiles vertically) depending on orientation.
// Terminates the packet list with a 0xFFFF sentinel word.
int Dungeon_PrepOverlayDma_watergate(int dst, uint16 r8, uint16 r6, int loops) {  // 81f77c
  for (int k = 0; k < loops; k++) {
    int x = r8 >> 1;
    // Pack the VRAM destination address from the tile coordinate bits.
    vram_upload_tile_buf[dst + 0] = ((r8 & 0x40) << 4) | ((r8 & 0x303f) >> 1) | ((r8 & 0xf80) >> 2);
    vram_upload_tile_buf[dst + 1] = r6;
    vram_upload_tile_buf[dst + 2] = overworld_tileattr[x + 0];
    if (!(r6 & 1)) {
      // Horizontal layout: the four tile words are at consecutive column offsets (+0,+1,+2,+3).
      vram_upload_tile_buf[dst + 3] = overworld_tileattr[x + 1];
      vram_upload_tile_buf[dst + 4] = overworld_tileattr[x + 2];
      vram_upload_tile_buf[dst + 5] = overworld_tileattr[x + 3];
      r8 += 128;
    } else {
      // Vertical layout: the four tile words are at consecutive row offsets (+0,+64,+128,+192).
      vram_upload_tile_buf[dst + 3] = overworld_tileattr[x + 64];
      vram_upload_tile_buf[dst + 4] = overworld_tileattr[x + 128];
      vram_upload_tile_buf[dst + 5] = overworld_tileattr[x + 192];
      r8 += 2;
    }
    dst += 6;
  }
  vram_upload_tile_buf[dst] = 0xffff;
  return dst;
}

// Module 5: Load File — restores game state from a save slot and resumes play.
// Called after file selection (Module01) confirms a slot. Performs a full engine reset:
// force-blank, clear tilemaps, reload graphics, reinitialize Link, then branches based
// on where the save was made:
//   - Dark world indoors   → LoadDungeonRoomRebuildHUD (resume dungeon room directly)
//   - Dark world outdoors  → overworld load (module 8)
//   - Light world + mosaic/death/early-save/spawn5 → LoadDungeonRoomRebuildHUD
//   - Light world normal   → show re-entry dialogue (0x184 or 0x185 if mirror held),
//                            then transition to spawn select (module 27)
void Module05_LoadFile() {  // 828136
  EnableForceBlank();
  overworld_map_state = 0;
  dung_unk6 = 0;
  byte_7E02D4 = 0;
  byte_7E02D7 = 0;
  tagalong_var5 = 0;
  byte_7E0379 = 0;
  byte_7E03FD = 0;
  EraseTileMaps_normal();
  LoadDefaultGraphics();
  Sprite_LoadGraphicsProperties();
  Init_LoadDefaultTileAttr();
  DecompressSwordGraphics();
  DecompressShieldGraphics();
  Link_Initialize();
  LoadFollowerGraphics();
  // Reset all four sprite GFX subset indices to 70 (default empty bank).
  sprite_gfx_subset_0 = 70;
  sprite_gfx_subset_1 = 70;
  sprite_gfx_subset_2 = 70;
  sprite_gfx_subset_3 = 70;
  word_7E02CD = 0x200;
  virq_trigger = 48;
  if (savegame_is_darkworld) {
    if (player_is_indoors) {
      LoadDungeonRoomRebuildHUD();
      return;
    }
    Hud_SearchForEquippedItem();
    Hud_Rebuild();
    Hud_UpdateEquippedItem();
    death_var5 = 0;
    // dungeon_room_index 32 = overworld anchor; module 8 = overworld load sequence.
    dungeon_room_index = 32;
    main_module_index = 8;
    submodule_index = 0;
    subsubmodule_index = 0;
    death_var4 = 0;
  } else {
    // Resume into dungeon directly if any of these edge cases apply:
    //   mosaic_level  — was in a transition/screen-effect when saved
    //   death_var5/!death_var4  — game-over/death recovery in progress
    //   sram_progress_indicator < 2  — very early save (before first dungeon clear)
    //   which_starting_point == 5  — special spawn point (e.g. Sanctuary)
    if (mosaic_level || death_var5 != 0 && !death_var4 || sram_progress_indicator < 2 || which_starting_point == 5) {
      LoadDungeonRoomRebuildHUD();
      return;
    }
    // Dialogue 0x185 = "You are holding the Magic Mirror"; 0x184 = standard re-entry message.
    dialogue_message_index = (link_item_mirror == 2) ? 0x185 : 0x184;
    Main_ShowTextMessage();
    Dungeon_LoadPalettes();
    INIDISP_copy = 15;
    TM_copy = 4;
    TS_copy = 0;
    main_module_index = 27;
  }
}

// Module 0x13: Boss Victory (Pendant dungeon). Runs the 6-phase pendant victory sequence
// (heal → spin → spotlight exit) then ticks sprites and updates Link's OAM slots each frame.
void Module13_BossVictory_Pendant() {  // 829c4a
  kModule_BossVictory[submodule_index]();
  Sprite_Main();
  LinkOam_Main();
}

// Boss victory phase 0: Gradually refills Link's magic and health bars each frame.
// Uses overworld_map_state as a dual-counter: if both Hud_RefillMagicPower and
// Hud_RefillHealth return zero (fully refilled), it stays at 0 and phase ends.
// When healed, disables the Y-button item (bit 0x40 of button_mask_b_y), faces Link
// south (direction 2), flags HUD refresh, and immobilizes Link for the spin setup.
// subsubmodule_index = 16 gives a 16-frame delay before the spin starts.
void BossVictory_Heal() {  // 829c59
  if (!Hud_RefillMagicPower())
    overworld_map_state++;
  if (!Hud_RefillHealth())
    overworld_map_state++;
  if (!overworld_map_state) {
    button_mask_b_y &= ~0x40;
    Dungeon_ResetTorchBackgroundAndPlayerInner();
    link_direction_facing = 2;
    link_direction_last = 2 << 1;
    flag_update_hud_in_nmi++;
    submodule_index++;
    subsubmodule_index = 16;
    flag_is_link_immobilized++;
  }
  overworld_map_state = 0;
  Hud_RefillLogic();
}

// Boss victory phase 1: Waits 16 frames (subsubmodule_index countdown from BossVictory_Heal),
// then begins Link's victory spin animation. Spawns the VictorySpin ancilla (sparkle effect),
// clears interactive ancillae (bombs, arrows, etc.), and re-mobilizes Link for the spin.
void Dungeon_StartVictorySpin() {  // 829c93
  if (--subsubmodule_index)
    return;
  flag_is_link_immobilized = 0;
  link_direction_facing = 2;
  Link_AnimateVictorySpin();
  Ancilla_TerminateSelectInteractives(0);
  AncillaAdd_VictorySpin();
  submodule_index++;
}

// Boss victory phase 2: Runs Link_Main each frame while Link performs the victory spin.
// Waits until link_player_handler_state returns to 0 (spin complete).
// SFX 0x2C = sword chime, played only if Link has a sword (type+1 with bit 0 masked).
// After the spin, forces Link to hold the sword upward for 32 frames before phase 3.
void Dungeon_RunVictorySpin() {  // 829cad
  Link_Main();
  if (link_player_handler_state != 0)
    return;
  // Play sword-raise chime if Link has a sword (link_sword_type+1 & 0xFE is non-zero for types 1-3).
  if (link_sword_type + 1 & 0xfe)
    sound_effect_1 = 0x2C;
  link_force_hold_sword_up = 1;
  subsubmodule_index = 32;
  submodule_index++;
}

// Boss victory phase 3: 32-frame hold after the sword raise. Clears Link's velocity
// and the fixed color modifier before advancing to the spotlight/exit phase.
void Dungeon_CloseVictorySpin() {  // 829cd1
  if (--subsubmodule_index)
    return;
  submodule_index++;
  link_y_vel = 0;
  link_x_vel = 0;
  overworld_fixed_color_plusminus = 0;
}

// Module 0x15: Mirror Warp From Agahnim. Drives the 13-phase KillAgahnim sequence
// (dark-world cutscene after defeating Agahnim). Sprites and Link OAM are ticked
// only during phases < 2 (music load / init) or >= 5 (dialogue and beyond);
// during phases 2–4 (HDMA wave effects) the screen is fully distorted and sprites are suppressed.
void Module15_MirrorWarpFromAga() {  // 829cfc
  kModule_KillAgahnim[submodule_index]();
  if (submodule_index < 2 || submodule_index >= 5) {
    Sprite_Main();
    LinkOam_Main();
  }
}

// Module 0x16: Boss Victory (Crystal dungeon). Uses the same heal/spin phases as pendant
// victory but adds phase 4 (Module16_04_FadeAndEnd) which fades the screen and returns
// to the previous module (typically the dungeon or overworld module that was active).
void Module16_BossVictory_Crystal() {  // 829e8a
  switch (submodule_index) {
  case 0: BossVictory_Heal(); break;
  case 1: Dungeon_StartVictorySpin(); break;
  case 2: Dungeon_RunVictorySpin(); break;
  case 3: Dungeon_CloseVictorySpin(); break;
  case 4: Module16_04_FadeAndEnd(); break;
  }
  Sprite_Main();
  LinkOam_Main();
}

// Crystal victory phase 4: Fades the screen out by decrementing INIDISP_copy each frame.
// When fully black (INIDISP_copy reaches 0), resets scroll offsets, Link velocity,
// palette translucency, and item-receipt state, then restores the previously active
// module (saved_module_for_menu) and triggers the spotlight open sequence.
void Module16_04_FadeAndEnd() {  // 829e9a
  if (--INIDISP_copy)
    return;
  bg1_x_offset = 0;
  bg1_y_offset = 0;
  link_y_vel = 0;
  flag_is_link_immobilized = 0;
  Palette_RevertTranslucencySwap();
  link_player_handler_state = kPlayerState_Ground;
  link_receiveitem_index = 0;
  link_pose_for_item = 0;
  link_disable_sprite_damage = 0;
  main_module_index = saved_module_for_menu;
  submodule_index = 0;
  subsubmodule_index = 0;
  OpenSpotlight_Next2();
}

// Stores the raw SFX ID in byte_7E0CF8 (used by other systems to recall the last SFX),
// then OR-combines it with the spatial pan bits derived from Link's X coordinate.
// Returns the combined SFX+pan byte ready to assign to sound_effect_1/2.
static uint8 PlaySfx_SetPan(uint8 a) {  // 878036
  byte_7E0CF8 = a;
  return a | Link_CalculateSfxPan();
}

// Animates Link walking north toward the Triforce pedestal in the Triforce Room (Module19).
// Link walks upward (direction 8 = north) at speed 0x14 until Y < 169 (approach zone),
// then stops (Y < 152) and counts down link_delay_timer_spin_attack (64 frames) before
// setting link_pose_for_item = 2 (hold-item-overhead pose) and advancing subsubmodule_index.
void TriforceRoom_LinkApproachTriforce() {  // 87f49c
  uint8 y = link_y_coord;
  if (y < 152) {
    // Link has reached the pedestal; freeze movement and count down to the hold pose.
    link_animation_steps = 0;
    link_direction = 0;
    link_direction_last = 0;
    if (!--link_delay_timer_spin_attack) {
      link_pose_for_item = 2;
      subsubmodule_index++;
    }
  } else {
    // Link is still approaching; use speed 0x14 (fast walk) once within 17 pixels of target.
    if (y < 169)
      link_speed_setting = 0x14;
    link_direction = 8;
    link_direction_last = 8;
    link_direction_facing = 0;
    link_delay_timer_spin_attack = 64;
  }
}

// Spawns the overhead item-receipt ancilla and applies all inventory/state changes
// for the item identified by link_receiveitem_index (76 possible item types).
//
// Parameters:
//   ain       — ancilla type index for the floating item sprite
//   yin       — ancilla subtype / display variant
//   chest_pos — encoded chest tilemap position used to compute sprite X/Y when
//               item_receipt_method == 1 (chest open); ignored for floor/NPC pickups
//
// The function:
//   1. Allocates an ancilla slot; returns early if none available.
//   2. Writes the item value (kValueToGiveItemTo) to the SRAM address
//      (kMemoryLocationToGiveItemTo), with special cases for negative values
//      (increment, capacity upgrades, rupee/bomb amounts).
//   3. Handles per-item side effects: glove palette, pendant/crystal flags, cape removal,
//      mushroom uniqueness, bottle filling, compass-chest upgrades, etc.
//   4. Decompresses item graphics tiles and schedules the overhead sprite animation.
//   5. Positions the item sprite above Link (floor pickup) or above the chest (chest method).
void AncillaAdd_ItemReceipt(uint8 ain, uint8 yin, int chest_pos) {  // 8985e8
  int ancilla = Ancilla_AddAncilla(ain, yin);
  if (ancilla < 0)
    return;

  // Item 0x20 (Magic Cape) needs a longer immobilization (2 frames) to allow cape-removal logic.
  flag_is_link_immobilized = (link_receiveitem_index == 0x20) ? 2 : 1;
  uint8 t;

  int j = link_receiveitem_index;
  // Item 0 (Boomerang tier 1) always also gives the bow (index 4) in the original game logic.
  if (j == 0) {
    g_ram[kMemoryLocationToGiveItemTo[4]] = kValueToGiveItemTo[0];
  }

  uint8 v = kValueToGiveItemTo[j];
  uint8 *p = &g_ram[kMemoryLocationToGiveItemTo[j]];
  // Positive values: write directly. Negative values: handled by item-specific branches below.
  if (!sign8(v))
    *p = v;

  // Item 0x1F (Moon Pearl): clear bunny transform when received in dark world.
  if (j == 0x1f)
    link_is_bunny = 0;
  // Item 0x4B (Pegasus Boots) sets run ability bit 4; item 0x1E (Flippers) sets swim bit 2.
  else if (j == 0x4b || j == 0x1e)
    link_ability_flags |= (j == 0x4b) ? 4 : 2;

  if (j == 0x1b || j == 0x1c) {
    // Items 0x1B/0x1C = Power Gloves (tiers 1/2); update glove palette immediately.
    Palette_UpdateGlovesColor();
  } else if ((t = 4, j == 0x37) || (t = 1, j == 0x38) || (t = 2, j == 0x39)) {
    // Pendant items 0x37/0x38/0x39 (Courage/Wisdom/Power): OR the pendant bit into the save slot.
    // If all three pendant bits are set (& 7 == 7), mark the map icon for Ganon's Tower unlock.
    *p |= t;
    if ((*p & 7) == 7)
      savegame_map_icons_indicator = 4;
    overworld_map_state++;
  } else if (j == 0x22) {
    // Item 0x22 (Lamp): only set to 1 if not already owned (no upgrade tier).
    if (*p == 0)
      *p = 1;
  } else if (j == 0x25 || j == 0x32 || j == 0x33) {
    // Items 0x25/0x32/0x33 (Big Key / Map / Compass): set dungeon-specific bit using cur_palace_index.
    WORD(*p) |= 0x8000 >> (BYTE(cur_palace_index_x2) >> 1);
  } else if (j == 0x3e) {
    // Item 0x3E (thrown object pickup while carrying): if Link was in throw state, clear it.
    if (link_state_bits & 0x80)
      link_picking_throw_state = 2;
  } else if (j == 0x20) {
    // Item 0x20 (Magic Cape): cancel any active cape usage, spawn a poof ancilla,
    // and clear any lift/throw ancillae (types 7 = lift, 0x2C = carry).
    overworld_map_state++;
    for (int i = 4; i >= 0; i--) {
      if (ancilla_type[i] == 7 || ancilla_type[i] == 0x2c) {
        ancilla_type[i] = 0;
        link_state_bits = 0;
        link_picking_throw_state = 0;
      }
    }
    if (link_cape_mode) {
      link_bunny_transform_timer = 32;
      link_disable_sprite_damage = 0;
      link_cape_mode = 0;
      AncillaAdd_CapePoof(0x23, 4);
      sound_effect_1 = 0x15 | Link_CalculateSfxPan();
    }
  } else if (j == 0x29) {
    // Item 0x29 (Mushroom): only set to 1 if not already used (mushroom != 2 = powder).
    if (link_item_mushroom != 2) {
      *p = 1;
      Hud_RefreshIcon();
    }
  } else if ((t = 1, j == 0x24) || item_receipt_method != 2 && (j == 0x27 || (t = 3, j == 0x28) || (t = 10, j == 0x31))) {
    // Increment-by-t items: 0x24 (Arrow +1), 0x27 (Bomb +1), 0x28 (Bomb +3), 0x31 (Arrow +10).
    // Not applied via chest (item_receipt_method 2) to avoid double-granting from shops.
    *p += t;
    if (*p > 99)
      *p = 99;
    Hud_RefreshIcon();
  } else if (j == 0x17) {
    // Item 0x17 (Magic Powder): cycles the bottle content tier (0→1→2→3→0). SFX 0x2D = bottle pop.
    *p = (*p + 1) & 3;
    sound_effect_2 = 0x2d | Link_CalculateSfxPan();
  } else if (j == 1) {
    // Item 1 (Master Sword): update the overworld song list when the sword is received.
    Overworld_SetSongList();
  } else {
    ItemReceipt_GiveBottledItem(j);
  }

  uint8 gfx = kReceiveItemGfx[j];
  if (gfx == 0xff) {
    gfx = 0;
  } else if (gfx == 0x20 || gfx == 0x2d || gfx == 0x2e) {
    // Shield graphics tiles (0x20 = Fighter's Shield, 0x2D/0x2E = Mirror/Fire Shield):
    // decompress and reload the shield palette so the overhead sprite looks correct.
    DecompressShieldGraphics();
    Palette_Load_Shield();
  }
  DecodeAnimatedSpriteTile_variable(gfx);

  // Sword tiles 0x06 (Fighter's Sword) and 0x18 (other swords): decompress sword graphics
  // and reload sword palette so the overhead sprite matches the received sword type.
  // j != 0 guard prevents double-decompress on item 0 (Boomerang) which shares tile 6.
  if ((gfx == 6 || gfx == 0x18) && j != 0) {
    DecompressSwordGraphics();
    Palette_Load_Sword();
  }

  ancilla_item_to_link[ancilla] = j;
  ancilla_arr1[ancilla] = 0;

  if (j == 1 && item_receipt_method != 2) {
    // Master Sword (item 1) from the pedestal triggers the Medallion cutscene (MSCutscene, ancilla 0x35).
    // 160-frame timer keeps the overhead sprite visible during the cutscene.
    // submodule_index 43 = item-receipt submodule that waits for the cutscene to end.
    ancilla_timer[ancilla] = 160;
    submodule_index = 43;
    BYTE(palette_filter_countdown) = 0;
    AncillaAdd_MSCutscene(0x35, 4);
    ancilla_arr3[ancilla] = 2;
  } else {
    ancilla_arr3[ancilla] = 9;
  }
  ancilla_arr4[ancilla] = 5;
  ancilla_step[ancilla] = item_receipt_method;

  // Duration the item floats above Link. Cape/pendant items linger longer (0x68 = 104 frames).
  // Mushroom (0x26) uses a very short 2-frame display. Chest items use 0x38; floor pickups 0x60.
  ancilla_aux_timer[ancilla] =
    (j == 0x20 || j == 0x37 || j == 0x38 || j == 0x39) ? 0x68 :
    (j == 0x26) ? 0x2 : (item_receipt_method ? 0x38 : 0x60);

  int x, y;

  if (item_receipt_method == 1) {
    // Chest pickup: decode tile position from chest_pos bitfield (bits 12:7 = row, bits 6:1 = col).
    // Convert to pixel coordinates, add room scroll offset, then apply per-item offsets.
    y = (chest_pos & 0x1f80) >> 4;
    x = (chest_pos & 0x7e) << 2;
    y += dung_loade_bgoffs_v_copy & ~0xff;
    x += dung_loade_bgoffs_h_copy & ~0xff;
    y += kReceiveItem_Tab2[j];
    x += kReceiveItem_Tab3[j];
  } else {
    // Floor / NPC / overworld pickup: play the appropriate SFX based on item type.
    if (ancilla_step[ancilla] == 0 && j == 1) {
      // Master Sword from pedestal: SFX 0x2C = sword chime.
      sound_effect_1 = Link_CalculateSfxPan() | 0x2c;
    } else if (j == 0x20 || j == 0x37 || j == 0x38 || j == 0x39) {
      // Cape / pendant items: music command 0x13 = fanfare.
      music_control = Link_CalculateSfxPan() | 0x13;
    } else if (j != 0x3e && j != 0x17) {
      // Standard item pickup SFX 0x0F (skip for throw-item 0x3E and powder 0x17 which play their own).
      sound_effect_2 = Link_CalculateSfxPan() | 0xf;
    }
    // method 3 is treated as method 0 for positioning (NPC gift uses Link-relative coords).
    int method = item_receipt_method == 3 ? 0 : item_receipt_method;
    // X position: offset from Link, adjusted per kReceiveItem_Tab1 (small=10, large=6) for method 0.
    x = (method != 0) ? kReceiveItem_Tab3[j] :
      (kReceiveItem_Tab1[j] == 0) ? 10 : (j == 0x20) ? 0 : 6;
    x += link_x_coord;
    // Y position: 14 pixels above Link for method 0; per-item offset table for methods 1/2.
    y = method ? kReceiveItem_Tab2[j] : -14;
    y += link_y_coord + ((method == 2) ? -8 : 0);
  }
  Ancilla_SetXY(ancilla, x, y);
}

// Assigns a bottled item to the first available bottle slot.
// kBottleList maps item indices to bottle content values (j+2 = bottle content code).
// kPotionList covers potion refills: searches for a bottle currently holding an empty
// bottle (value 2) and upgrades it to the potion content code (j+3).
// If no suitable bottle slot is found, the item is silently discarded.
void ItemReceipt_GiveBottledItem(uint8 item) {  // 89893e
  // Bottle-able items: 0x16=Empty Bottle, 0x2B=Red Potion, 0x2C=Green Potion,
  // 0x2D=Blue Potion, 0x3D=Bee, 0x3C=Good Bee, 0x48=Fairy.
  static const uint8 kBottleList[7] = { 0x16, 0x2b, 0x2c, 0x2d, 0x3d, 0x3c, 0x48 };
  // Potion-refill items that replace an empty bottle (value 2) with a filled one.
  static const uint8 kPotionList[5] = { 0x2e, 0x2f, 0x30, 0xff, 0xe };
  int j;
  // Scan bottle slots for an empty slot (value < 2) to store the new bottled item.
  if ((j = FindInByteArray(kBottleList, item, 7)) >= 0) {
    for (int i = 0; i != 4; i++) {
      if (link_bottle_info[i] < 2) {
        link_bottle_info[i] = j + 2;
        return;
      }
    }
  }
  // For potions, scan for a bottle already holding the empty-bottle state (value == 2).
  if ((j = FindInByteArray(kPotionList, item, 5)) >= 0) {
    for (int i = 0; i != 4; i++) {
      if (link_bottle_info[i] == 2) {
        link_bottle_info[i] = j + 3;
        return;
      }
    }
  }
}

// Module 0x17: Save and Quit. Fades the screen to black by decrementing INIDISP_copy
// each frame (case 1). Once fully black, enables full mosaic (level 15), and calls
// Death_Func15 to write the save file and return to the file-select screen.
// Case 0 simply advances to case 1 (fall-through, first-frame init).
void Module17_SaveAndQuit() {  // 89f79f
  switch (submodule_index) {
  case 0:
    submodule_index++;
  case 1:
    if (!--INIDISP_copy) {
      MOSAIC_copy = 15;
      subsubmodule_index = 1;
      Death_Func15(false);
    }
    break;
  }
  Sprite_Main();
  LinkOam_Main();
}

// Called when a WallMaster sprite captures Link and teleports him to the dungeon entrance.
// Saves current dungeon key counts, flags the room quadrant data, resets all sprites,
// and transitions to Module 17 (Save and Quit flow) which returns Link to the entrance.
// death_var4 = 0 clears any pending death state; nmi_load_bg_from_vram = 0 prevents
// a stale VRAM reload during the transition.
void WallMaster_SendPlayerToLastEntrance() {  // 8bffa8
  SaveDungeonKeys();
  Dungeon_FlagRoomData_Quadrants();
  Sprite_ResetAll();
  death_var4 = 0;
  main_module_index = 17;
  submodule_index = 0;
  nmi_load_bg_from_vram = 0;
  ResetSomeThingsAfterDeath(17);  // wtf: argument?
}

// Generates a pseudo-random byte using a simple LFSR-like step.
// byte_7E0FA1 is the persistent seed; frame_counter adds entropy each call.
// Update rule: if the low bit is 1, right-shift; otherwise right-shift then XOR with 0xB8.
uint8 GetRandomNumber() {  // 8dba71
  uint8 t = byte_7E0FA1 + frame_counter;
  t = (t & 1) ? (t >> 1) : (t >> 1) ^ 0xb8;
  byte_7E0FA1 = t;
  return t;
}

// Returns the spatial pan byte for a sound effect originating at Link's X position.
// 0x00 = center, 0x80 = left, 0x40 = right (SNES SPC700 pan encoding).
uint8 Link_CalculateSfxPan() {  // 8dbb67
  return CalculateSfxPan(link_x_coord);
}

// Queues SFX 'a' in the ambient sound channel (sound_effect_ambient) with pan derived from
// sprite k's X position. Noop if the channel is already occupied (non-zero).
void SpriteSfx_QueueSfx1WithPan(int k, uint8 a) {  // 8dbb6e
  if (sound_effect_ambient == 0)
    sound_effect_ambient = a | Sprite_CalculateSfxPan(k);
}

// Queues SFX 'a' in channel 1 (sound_effect_1) with pan from sprite k. Noop if occupied.
void SpriteSfx_QueueSfx2WithPan(int k, uint8 a) {  // 8dbb7c
  if (sound_effect_1 == 0)
    sound_effect_1 = a | Sprite_CalculateSfxPan(k);
}

// Queues SFX 'a' in channel 2 (sound_effect_2) with pan from sprite k. Noop if occupied.
void SpriteSfx_QueueSfx3WithPan(int k, uint8 a) {  // 8dbb8a
  if (sound_effect_2 == 0)
    sound_effect_2 = a | Sprite_CalculateSfxPan(k);
}

// Returns the spatial pan byte for a sound effect originating at sprite k's X position.
uint8 Sprite_CalculateSfxPan(int k) {  // 8dbba1
  return CalculateSfxPan(Sprite_GetX(k));
}

// Maps a world X coordinate to a SPC700 stereo pan byte.
// Subtracts (BG2HOFS_copy2 + 80) to get screen-relative X (center = 0).
// kPanTable: index 0 = center (0x00), 1 = left (0x80), 2 = right (0x40).
// Left if x underflows (unsigned wraparound past center), right if x >= 80 and positive signed.
uint8 CalculateSfxPan(uint16 x) {  // 8dbba8
  static const uint8 kPanTable[] = { 0, 0x80, 0x40 };
  int o = 0;
  x -= BG2HOFS_copy2 + 80;
  if (x >= 80)
    o = 1 + ((int16)x >= 0);
  return kPanTable[o];
}

// Torch-specific pan lookup: divides the screen width into 8 zones of 32 pixels each.
// Returns 0x80 (left) for zones 0–2, 0x00 (center) for zones 3–4, 0x40 (right) for 5–7.
// 'a' is the torch pixel X in screen space (scroll-relative), not world space.
uint8 CalculateSfxPan_Arbitrary(uint8 a) {  // 8dbbd0
  static const uint8 kTorchPans[] = { 0x80, 0x80, 0x80, 0, 0, 0x40, 0x40, 0x40 };
  return kTorchPans[((a - BG2HOFS_copy2) >> 5) & 7];
}

// Copies tile collision defaults (kDungeon_DefaultAttr) into attributes_for_tile.
// attributes_for_tile is a 0x200-entry array; this loads the first 0x140 contiguous entries
// then the final 64 entries at offset 0x1C0, leaving a gap at 0x140–0x1BF for room-specific data.
void Init_LoadDefaultTileAttr() {  // 8e97d9
  memcpy(attributes_for_tile, kDungeon_DefaultAttr, 0x140);
  memcpy(attributes_for_tile + 0x1c0, kDungeon_DefaultAttr + 0x140, 64);
}

// Switches the game into dialogue/messaging mode (module 14) to display a text message.
// Saves the current module index in saved_module_for_menu so it can be restored when the
// message is dismissed. Resets byte_7E0223 and messaging_module for a clean start.
// submodule_index = 2 skips the opening animation frame (message is already prepared).
// Noop if already in module 14 (prevents re-entrancy during an active message).
void Main_ShowTextMessage() {  // 8ffdaa
  if (main_module_index != 14) {
    byte_7E0223 = 0;
    messaging_module = 0;
    submodule_index = 2;
    saved_module_for_menu = main_module_index;
    main_module_index = 14;
  }
}

// Routes item-tile interaction (e.g., using the shovel, bomb, or other tools on a tile)
// to the appropriate handler based on whether Link is indoors or on the overworld.
// Indoors: HandleItemTileAction_Dungeon checks dungeon tile interaction rules.
// Outdoors: Overworld_ToolAndTileInteraction handles overworld dig/push/bomb responses.
uint8 HandleItemTileAction_Overworld(uint16 x, uint16 y) {  // 9bbd7a
  if (player_is_indoors)
    return HandleItemTileAction_Dungeon(x, y);
  else
    return Overworld_ToolAndTileInteraction(x, y);
}

