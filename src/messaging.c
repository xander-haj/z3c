/*
 * messaging.c — Module 0x0E Interface, Text Rendering, Map Display, and Game Over
 *
 * Implements the central "Interface" module (Module0E) that overlays menus and dialogue
 * on top of normal gameplay. This file contains:
 *
 *   Text / dialogue system:
 *     - Text_Initialize / Text_Render / RenderText* — variable-width font (VWF) rendering
 *       pipeline: border drawing → character tilemap → glyph blitting → finish
 *     - kText_* tables — border tile set, command byte lengths, wait durations, VWF bit masks
 *     - RenderText_PostDeathSaveOptions — "Continue / Save and Quit" post-death menu
 *
 *   Dungeon map (Module0E submodule 3):
 *     - kDungMap_Tab* tables — floor layout indices, tile IDs, sprite positions, boss markers
 *     - DungeonMap_DrawRoomMarkers, DungeonMap_HandleInputAndSprites — draw & navigate the map
 *     - kDungMap_Tab23[744] — full sprite tile data for all dungeon map icons
 *
 *   Overworld map (Module0E submodule 7):
 *     - Messaging_OverworldMap — Mode 7 world map display with pendant/crystal overlays
 *     - kOverworldMap_tab1[333] — sine table for the rotating circle animation
 *     - kBirdTravel_* — flute-bird destination screen coordinates for 8 locations
 *     - kOwMapCrystal*_x/y — crystal icon world-space positions for each dungeon
 *
 *   Special sequences:
 *     - Module0E_05_DesertPrayer — iris-spotlight HDMA effect (Book of Mudora scene)
 *     - DesertPrayer_BuildIrisHDMATable / DesertHDMA_CalculateIrisShapeLine
 *     - Module0E_0A_FluteMenu — flute bird-destination selection menu
 *     - Module0E_0B_SaveMenu — Continue / Save and Quit decision screen
 *
 *   Game over sequence (kModule_Death[16]):
 *     - GameOver_AdvanceImmediately → Death_Func1 → GameOver_DelayBeforeIris →
 *       GameOver_IrisWipe → Death_Func4 → GameOver_SplatAndFade → Death_Func6 →
 *       Animate_GAMEOVER_Letters → GameOver_Finalize_GAMEOVR → GameOver_SaveAndOrContinue →
 *       GameOver_InitializeRevivalFairy → RevivalFairy_Main → GameOver_RiseALittle →
 *       GameOver_Restore0D/0E → GameOver_ResituateLink
 *
 *   Save utilities:
 *     - SaveGameFile — writes 0x500 bytes to both SRAM mirrors + checksum
 *     - TransferMode7Characters — loads overworld map gfx into VRAM high bytes
 *
 * Interacts with: hud.h, dungeon.h, overworld.h, player.h, ancilla.h, sprite.h,
 *                 load_gfx.h, attract.h, misc.h, nmi.h, assets.h
 */
#include "messaging.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "snes/snes_regs.h"
#include "dungeon.h"
#include "hud.h"
#include "load_gfx.h"
#include "dungeon.h"
#include "overworld.h"
#include "variables.h"
#include "ancilla.h"
#include "player.h"
#include "misc.h"
#include "sprite.h"
#include "player_oam.h"
#include "attract.h"
#include "nmi.h"
#include "assets.h"

static void WorldMap_AddSprite(int spr, uint8 big, uint8 flags, uint8 ch, uint16 x, uint16 y);
static bool WorldMap_CalculateOamCoordinates(Point16U *pt);

// Dungeon floor index for each of the 14 dungeons (-1 = no map, 0–14 = floor offset into layout).
static const int8 kDungMap_Tab0[14] = {-1, -1, -1, -1, -1, 2, 0, 10, 4, 8, -1, 6, 12, 14};
// BG register addresses used to write dungeon map backdrop tile data (VRAM word addresses).
static const uint16 kDungMap_Tab1[8] = {0x2108, 0x2109, 0x2109, 0x210a, 0x210b, 0x210c, 0x210d, 0x211d};
static const uint16 kDungMap_Tab2[8] = {0x2118, 0x2119, 0xa109, 0x211a, 0x211b, 0x211c, 0x2118, 0xa11d};
// Dungeon map title text tile addresses (low byte = tile index, high byte = palette/flags).
// Two entries per dungeon: the 7 dungeons per world × 2 bytes each.
static const uint8 kDungMap_Tab3[14] = {0x60, 0x84, 0, 0xb, 0x32, 0x21, 0x33, 0x21, 0x38, 0x21, 0x3a, 0x21, 0x7f, 0x20};
static const uint8 kDungMap_Tab4[14] = {0x60, 0xa4, 0, 0xb, 0x42, 0x21, 0x43, 0x21, 0x49, 0x21, 0x4a, 0x21, 0x7f, 0x20};
// VRAM tile IDs for the 7 dungeon-map floor-selector arrow sprites.
static const uint16 kDungMap_Tab8[7] = {0x1b28, 0x1b29, 0x1b2a, 0x1b2b, 0x1b2c, 0x1b2d, 0x1b2e};
static const uint16 kDungMap_Tab6[21] = {0xaa10, 0x100, 0x1b2f, 0xc910, 0x300, 0x1b2f, 0x1b2e, 0xe510, 0xb00, 0x1b2f, 0x1b2e, 0x5b2f, 0x1b2f, 0x1b2e, 0x1b2e, 0x311, 0x100, 0x1b2f, 0x411, 0xc40, 0x1b2e};
static const uint16 kDungMap_Tab5[14] = {0x21, 0x23, 0x20, 0x21, 0x70, 0x12, 0x11, 0x212, 2, 0x217, 0x160, 0x12, 0x113, 0x171};
static const uint16 kDungMap_Tab7[9] = {0x1223, 0x1263, 0x12a3, 0x12e3, 0x1323, 0x11e3, 0x11a3, 0x1163, 0x1123};
static const uint16 kDungMap_Tab9[8] = {0xf26, 0xf27, 0x4f27, 0x4f26, 0x8f26, 0x8f27, 0xcf27, 0xcf26};
static const uint16 kDungMap_Tab10[4] = {0xe2, 0xf8, 0x3a2, 0x3b8};
// Dungeon map marker OAM tile/palette IDs for small items (keys, chests, boss).
static const uint16 kDungMap_Tab11[4] = {0x1f19, 0x5f19, 0x9f19, 0xdf19};
static const uint16 kDungMap_Tab12[2] = {0xe4, 0x3a4};
static const uint16 kDungMap_Tab13[2] = {0x1f1a, 0x9f1a};
static const uint16 kDungMap_Tab14[2] = {0x122, 0x138};
static const uint16 kDungMap_Tab15[2] = {0x1f1b, 0x5f1b};
// VRAM tile IDs for the 8 dungeon-level indicator sprites on the map screen.
static const uint16 kDungMap_Tab16[8] = {0x1f1e, 0x1f1f, 0x1f20, 0x1f21, 0x1f22, 0x1f23, 0x1f24, 0x1f25};
// Sprite tile data for all dungeon map room icons (744 entries: one per room across all dungeons).
// Each entry is an OAM tile word encoding tile index and palette/flip attributes (SNES OAM format).
// 0xb00 = transparent/empty room; other values select the appropriate map icon tile.
static const uint16 kDungMap_Tab23[744] = {
  0xb61, 0x5361, 0x8b61, 0x8b62, 0xb60, 0xb63, 0x8b60, 0xb64, 0xb00, 0xb00, 0xb65, 0xb66, 0xb67, 0x4b67, 0x9367, 0xd367, 0xb60, 0x5360, 0x8b60, 0xcb60, 0xb6a, 0x4b6a, 0x4b6d, 0xb6d, 0x1368, 0x1369, 0xb00, 0xb00, 0xb6a, 0x136b, 0xb6c, 0xb6d,
  0x136e, 0x4b6e, 0xb00, 0xb00, 0x136f, 0xb00, 0xb00, 0xb00, 0x1340, 0xb00, 0xb78, 0x1744, 0x536d, 0x136d, 0x4b76, 0xb76, 0xb70, 0xb71, 0xb72, 0x8b71, 0xb75, 0xb76, 0x8b75, 0x8b76, 0xb00, 0xb53, 0xb00, 0xb55, 0x1354, 0x5354, 0xb00, 0xb00,
  0x4b53, 0xb00, 0xb56, 0xb57, 0xb00, 0xb59, 0xb00, 0x135e, 0x135a, 0x135b, 0x135f, 0x535f, 0xb5c, 0xb5d, 0x535e, 0xcb58, 0xb50, 0x4b50, 0x1352, 0x5352, 0xb00, 0xb40, 0x1345, 0xb46, 0x8b42, 0xb47, 0xb42, 0xb49, 0x1348, 0x5348, 0x174a, 0x574a,
  0x4b47, 0xcb42, 0x4b49, 0x4b42, 0xb00, 0xb4b, 0xb00, 0xb4d, 0xb4c, 0x4b4c, 0xb4e, 0x4b4e, 0xb51, 0xb44, 0xb00, 0xb00, 0xb4f, 0x4b4f, 0x934f, 0xd34f, 0xb00, 0xb00, 0xb00, 0xb40, 0xb00, 0xb41, 0xb00, 0xb42, 0xb00, 0xb00, 0xb43, 0xb43,
  0xb00, 0xb00, 0x9344, 0xb00, 0x1340, 0xb00, 0x1341, 0xb00, 0x1740, 0xb40, 0xb42, 0xb7d, 0x4b7a, 0xb7a, 0xb7e, 0x4b7e, 0xb40, 0x8b4d, 0x4bba, 0xb55, 0xb40, 0x8b55, 0x1378, 0xcb53, 0x4b76, 0x4b75, 0x13bb, 0x53bb, 0x4b7f, 0x4b42, 0xb83, 0x13bc,
  0xb00, 0xb00, 0xb79, 0xb00, 0xb6e, 0x4b7c, 0xb00, 0xb41, 0x1340, 0x8b55, 0xb42, 0xb7b, 0x8b42, 0x9344, 0x1341, 0xb00, 0xb53, 0x9344, 0x8b53, 0x9344, 0x8b42, 0x9344, 0xb42, 0x9344, 0x934d, 0xb00, 0x8b53, 0x9344, 0xb00, 0xb00, 0xb40, 0xb00,
  0xb41, 0xb00, 0x1384, 0xb00, 0xbb8, 0x13b9, 0x4b85, 0xcb7c, 0xb87, 0x13b0, 0x4b7b, 0x9344, 0xb00, 0xb00, 0xb40, 0xb00, 0xb91, 0x5391, 0xb9c, 0x4b9c, 0x8b42, 0x1392, 0xb93, 0x1394, 0xb95, 0xb96, 0x9395, 0x8b96, 0xb97, 0xb98, 0x8b97, 0x8b98,
  0x1799, 0x5799, 0x9799, 0xd799, 0x4b98, 0x4b97, 0xcb98, 0xcb97, 0x937b, 0xb00, 0xb7b, 0xb00, 0xba6, 0x4ba6, 0xcb7a, 0x8b7a, 0xb8e, 0x4b8e, 0x938e, 0xcb8e, 0x934d, 0xb8f, 0x1390, 0x5390, 0xb00, 0xb00, 0xb00, 0x8b48, 0xb00, 0x934e, 0xb00, 0x8b4d,
  0x8b72, 0x1346, 0xb45, 0xb46, 0x5744, 0x1744, 0xb00, 0xb00, 0x134d, 0xb00, 0x8b54, 0xb00, 0x1349, 0x1349, 0xb00, 0xb00, 0xb4b, 0x8b48, 0xb72, 0x4b72, 0xb00, 0xb74, 0xb00, 0xbb0, 0xb71, 0x1747, 0x17af, 0xb4b, 0xb6f, 0x1370, 0xb4b, 0xb00,
  0xb6b, 0x8b6c, 0x8b6b, 0xbad, 0xb73, 0xb00, 0x13ae, 0xb46, 0x176b, 0x576b, 0xb6a, 0x4b6a, 0x1368, 0x5368, 0x1369, 0x5369, 0x8b4e, 0xb00, 0x9354, 0xb00, 0xb00, 0xb00, 0xb00, 0x5377, 0xb00, 0x974d, 0xb00, 0x4b7b, 0xb40, 0x8b4d, 0xb51, 0xb8d,
  0x537a, 0x137a, 0x4b42, 0x8b40, 0xb00, 0xb00, 0xb00, 0xb00, 0xb00, 0xb00, 0xb40, 0xb00, 0xcb7a, 0x576e, 0xb00, 0xb00, 0xb6e, 0xb9f, 0xb00, 0x4ba5, 0x13a0, 0x13a1, 0xba2, 0xba3, 0xba4, 0xb00, 0xba5, 0xb00, 0xb40, 0x8b55, 0xb42, 0xcb87,
  0x8b95, 0xba7, 0x8b42, 0xbaf, 0x4b78, 0xb00, 0x4b78, 0xb00, 0x8b42, 0xb51, 0xb78, 0x8b51, 0xba8, 0xba9, 0xbac, 0x8ba9, 0xbaa, 0x17ab, 0x13b4, 0x8bab, 0x17b1, 0xb41, 0x4b44, 0x4b42, 0xb00, 0xbad, 0xb00, 0x13ae, 0x1340, 0xbb7, 0xb42, 0xbb6,
  0xb00, 0xb00, 0x139d, 0x139e, 0xb00, 0xb00, 0xb00, 0xb79, 0xb00, 0xb00, 0x8b42, 0xb86, 0xb42, 0x8b7b, 0x8b42, 0xb7b, 0xb87, 0x8b7b, 0x9387, 0xb7b, 0xb40, 0x13b3, 0x1378, 0xb8d, 0x8b42, 0xb88, 0x5378, 0xb40, 0x4b44, 0xd342, 0x97b5, 0x4b78,
  0x13b3, 0x8b55, 0x4b7b, 0xb8d, 0xb89, 0x138a, 0xb8b, 0xb8c, 0xb00, 0xb7c, 0xb00, 0xb00, 0xb00, 0x9348, 0xb00, 0xb56, 0xb00, 0xb00, 0xb88, 0xb00, 0xb00, 0xb48, 0xb00, 0xb00, 0xb00, 0x9348, 0x1786, 0xb65, 0xb00, 0xb00, 0xcb5a, 0xb00,
  0xb00, 0x5388, 0xb00, 0xb00, 0x4b5a, 0xb00, 0xb00, 0xb00, 0xb00, 0xcb5b, 0x13ab, 0xbac, 0xcb5a, 0xb00, 0x137e, 0xb00, 0xb00, 0x137e, 0xb00, 0xb00, 0xb00, 0x8b48, 0x1783, 0x1384, 0xb00, 0xb00, 0x1385, 0xb00, 0xb00, 0x537e, 0xb00, 0xb00,
  0xb00, 0x8b48, 0xb43, 0xcb43, 0xb00, 0xb00, 0x1379, 0x137a, 0xb5a, 0x137b, 0xb00, 0xb00, 0xb00, 0x8b48, 0x137f, 0x1380, 0xb00, 0xb00, 0x1381, 0x1382, 0xb00, 0xb48, 0xb00, 0xb00, 0xb00, 0xb00, 0x1387, 0x1377, 0x5746, 0xb47, 0x1349, 0xb48,
  0x1375, 0x4b42, 0x174a, 0x574a, 0xb43, 0x1344, 0xb45, 0x1746, 0x1742, 0x5742, 0x8b42, 0xcb42, 0x1375, 0x5375, 0x8b42, 0xcb42, 0x4b40, 0x1340, 0xb41, 0x4b41, 0x4b46, 0xb71, 0x1786, 0x8b71, 0x1347, 0xb4d, 0xb65, 0xb5b, 0xb00, 0xb00, 0x9348, 0xb00,
  0xb00, 0xb00, 0xb00, 0x8b48, 0x4b66, 0x8b65, 0x4b5b, 0xb65, 0x9365, 0xb66, 0xb63, 0x8b66, 0x4b51, 0xb5f, 0xcb76, 0xb60, 0xb64, 0x4b4f, 0x4b60, 0x8b76, 0x4b76, 0xb61, 0xd376, 0x1362, 0x4b61, 0xb76, 0xcb58, 0x8b51, 0xb00, 0xb00, 0x5746, 0xb5e,
  0xb00, 0xb00, 0xb5e, 0xb46, 0xb00, 0xb00, 0x8b48, 0xb00, 0xb4f, 0xb51, 0xcb76, 0x8b76, 0x5351, 0xb51, 0x8b4f, 0x8b51, 0x4b76, 0xb76, 0xcb51, 0x8b58, 0xb54, 0xb00, 0x8b66, 0xb00, 0x9348, 0x8b48, 0xb56, 0x4b45, 0xb00, 0xb57, 0xb00, 0xb59,
  0x4b50, 0xb58, 0xcb50, 0x8b50, 0x5758, 0x1751, 0xcb58, 0x8b51, 0xb56, 0x4b56, 0xb65, 0x5756, 0x9348, 0x8b48, 0xb4c, 0xb4b, 0xb4d, 0xb00, 0x8b54, 0xb00, 0xb4f, 0xb50, 0x8b4f, 0x8b50, 0x4b50, 0xb51, 0xcb58, 0x8b51, 0xb52, 0xb54, 0xb53, 0x9354,
  0x9748, 0x9748, 0x138d, 0x138e, 0x1391, 0x1392, 0x138c, 0x138f, 0x1393, 0x1390, 0x9393, 0x138f, 0x1394, 0x1395, 0x138e, 0x138c, 0x175d, 0x1399, 0x975d, 0x538f, 0x1397, 0x1398, 0x179a, 0x138c, 0x1399, 0x1766, 0x138f, 0xd75d, 0x538e, 0x538f, 0x1391, 0x1392,
  0x139b, 0x539b, 0x139c, 0x539c, 0x138f, 0x138e, 0x5392, 0x5391, 0x138a, 0x538a, 0x138b, 0x538b, 0xb00, 0xcb5b, 0xb00, 0x8b54, 0x4b74, 0x13a6, 0xb00, 0x4b48, 0x13a0, 0x13a1, 0x538e, 0x138e, 0xd38e, 0x53a3, 0x13a4, 0xb00, 0x97aa, 0xb00, 0x538e, 0x1399,
  0x13a4, 0xb00, 0x138e, 0xb00, 0xb00, 0x5393, 0xb00, 0x574e, 0x4b7d, 0xb00, 0x8b7d, 0x139f, 0x97aa, 0x13a4, 0x13a9, 0x53a9, 0x13a5, 0x13a6, 0x93a5, 0xd3a5, 0xd38e, 0x938e, 0x13a4, 0x13aa, 0xb00, 0x13a6, 0xb00, 0x8b5f, 0x139b, 0x13a6, 0x139c, 0x53a2,
  0xb00, 0xb00, 0x138c, 0xb00, 0x9394, 0x139e, 0xb00, 0xb00,
};
// Dungeon map cursor blink X positions (pixel X for the 3 cursor states).
static const uint16 kDungMap_Tab21[3] = {137, 167, 79};
// Dungeon map cursor blink Y positions (pixel Y for the 3 cursor states).
static const uint16 kDungMap_Tab22[3] = {169, 119, 190};
// Scroll boundary values for the dungeon map view (two-sided: inner/outer).
static const uint16 kDungMap_Tab24[2] = {0x1f, 0x7f};
// Dungeon map icon X pixel positions for each of the 14 dungeons.
static const uint16 kDungMap_Tab25[14] = {15, 15, 200, 51, 32, 6, 90, 144, 41, 222, 7, 172, 164, 13};
// Dungeon map icon Y pixel offsets per dungeon (signed: negative = move up from base).
static const int16 kDungMap_Tab28[14] = {-1, -1, 1, 1, 6, 0xff, 0xff, 0xff, 0xfe, 0xf9, 5, 0xff, 0xfd, 6};
// Dungeon map initialization phase dispatch table (5 phases: gfx prep → title → backdrop → rooms → markers).
static PlayerHandlerFunc *const kDungMapInit[] = {
  &Module0E_03_01_00_PrepMapGraphics,
  &Module0E_03_01_01_DrawLEVEL,
  &Module0E_03_01_02_DrawFloorsBackdrop,
  &Module0E_03_01_03_DrawRooms,
  &DungeonMap_DrawRoomMarkers,
};
// Boss marker tile indices for the four quadrant positions on the dungeon map.
static const uint8 kDungMap_Tab38[4] = {0x39, 0x3b, 0x3d, 0x3b};
// X/Y offset pairs for the 4 chest-marker quadrant sprites on the dungeon map.
static const int8 kDungMap_Tab29[4] = {-9, 8, -9, 8};
static const int8 kDungMap_Tab30[4] = {-8, -8, 9, 9};
// OAM palette/flip attribute bytes for the 4 chest-marker sprites.
static const uint8 kDungMap_Tab31[4] = {0xf1, 0xb1, 0x71, 0x31};
// Sprite sizes (in OAM units) for the 4 dungeon-map marker types.
static const uint8 kDungMap_Tab32[4] = {0xc, 0xc, 8, 0xa};
// Y pixel positions for the 8 dungeon-map row dividers (one per floor).
static const uint8 kDungMap_Tab33[8] = {187, 171, 155, 139, 123, 107, 91, 75};
// Tile IDs for the 8 dungeon-map level-indicator sprites (one per floor row).
static const uint8 kDungMap_Tab34[8] = {0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
// Attribute byte offsets for the two dungeon-map floors currently shown.
static const uint8 kDungMap_Tab35[2] = {0, 8};
// Tile character IDs for the two floor-label sprites on the dungeon map.
static const uint8 kDungMap_Tab36[4] = {0x37, 0x38, 0x38, 0x37};
// Signed scroll amount for each dungeon's map header position (-1 = not applicable).
static const int16 kDungMap_Tab37[14] = { -1, -1, 0x808, 8, 0, 8, 0x808, 8, 0x808, 0x800, 0x404, 0x808, 8, 8 };
// X delta for the dungeon-map cursor blink animation (alternates between -4 and +4).
static const int8 kDungMap_Tab39[2] = {-4, 4};
// Y delta for the dungeon-map cursor blink animation.
static const int8 kDungMap_Tab40[2] = {4, -4};
// Vertical scroll step applied when the player moves between dungeon map floors (+0x60 / -0x60).
static const int16 kDungMap_Tab26[2] = {0x60, -0x60};
// Dungeon map submodule dispatch table (9 phases: backup GFX → draw → lighten → interact → scroll → fade → recover → star → restore).
static PlayerHandlerFunc *const kDungMapSubmodules[] = {
  &DungMap_Backup,
  &Module0E_03_01_DrawMap,
  &DungMap_LightenUpMap,
  &DungeonMap_HandleInputAndSprites,
  &DungMap_4,
  &DungMap_FadeMapToBlack,
  &DungeonMap_RecoverGFX,
  &ToggleStarTilesAndAdvance,
  &DungMap_RestoreOld,
};
// VRAM word addresses of the two dialogue box positions (top-of-screen and bottom-of-screen).
static const uint16 kText_Positions[2] = {0x6125, 0x6244};
// SRAM byte offsets for the 4 save slots (each slot is 0x500 bytes).
static const uint16 kSrmOffsets[4] = {0, 0x500, 0xa00, 0xf00};
// Initial state for the text rendering context (32-byte struct zeroed except a few fields).
static const int8 kText_InitializationData[32] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0x39, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1c, 4, 0, 0, 0, 0, 0};
// 9 tile words for the dialogue box border corners and edges (3x3 grid: TL, TM, TR, ML, MM, MR, BL, BM, BR).
static const uint16 kText_BorderTiles[9] = {0x28f3, 0x28f4, 0x68f3, 0x28c8, 0x387f, 0x68c8, 0xa8f3, 0xa8f4, 0xe8f3};
// Number of bytes consumed by each dialogue command code (indices 0–24).
// Used by the text parser to skip over control bytes embedded in the message stream.
static const uint8 kText_CommandLengths[25] = {
  1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
  2, 2, 2, 2, 1, 1, 1, 1, 1,
};
// Bitmasks for the 8 pixel columns within a VWF tile (MSB first: 0x80, 0x40, ..., 0x01).
static const uint8 kVWF_RenderCharacter_setMasks[8] = {0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1};
// Byte offsets into the VWF glyph buffer for each of the 3 text rows.
static const uint16 kVWF_RenderCharacter_renderPos[3] = {0, 0x2a0, 0x540};
// Byte offsets into the VWF character tilemap for each of the 3 text rows.
static const uint16 kVWF_RenderCharacter_linePositions[3] = {0, 0x40, 0x80};
// Word offsets into the text row control structure for rows 0–2.
static const uint16 kVWF_RowPositions[3] = {0, 2, 4};
// Frame durations for the 16 pause speeds (slow scroll wait, in frames × ~16.7ms).
// Index 0 = 31 frames (~0.5s), index 15 = 500 frames (~8.3s).
static const uint16 kText_WaitDurations[16] = {31, 63, 94, 125, 156, 188, 219, 250, 281, 313, 344, 375, 406, 438, 469, 500};
// 5-phase VWF text rendering pipeline (indexed by text_render_step).
static PlayerHandlerFunc *const kText_Render[] = {
  &RenderText_Draw_Border,
  &RenderText_Draw_BorderIncremental,
  &RenderText_Draw_CharacterTilemap,
  &RenderText_Draw_MessageCharacters,
  &RenderText_Draw_Finish,
};
// 3 top-level messaging handlers indexed by messaging_module:
// 0 = Text_Initialize (set up first dialogue), 1 = Text_Render (ongoing),
// 2 = RenderText_PostDeathSaveOptions (post-death save/continue prompt).
static PlayerHandlerFunc *const kMessaging_Text[] = {
  &Text_Initialize,
  &Text_Render,
  &RenderText_PostDeathSaveOptions,
};
// Cosine-like lookup table (333 entries) used to animate the rotating circle on the overworld map.
// Values decrease from 0xE0 to 0x00 and then wrap negative (signed). Sampled as an offset
// for the circular trail of dungeon icons on the map display.
static const uint8 kOverworldMap_tab1[333] = {
  0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xe0, 0xdf,
  0xde, 0xdd, 0xdc, 0xdb, 0xda, 0xd8, 0xd7, 0xd6, 0xd5, 0xd4, 0xd3, 0xd2, 0xd1, 0xd0, 0xcf, 0xce,
  0xcd, 0xcc, 0xcb, 0xca, 0xc9, 0xc7, 0xc6, 0xc5, 0xc4, 0xc3, 0xc2, 0xc1, 0xc0, 0xbf, 0xbe, 0xbd,
  0xbc, 0xbb, 0xba, 0xb9, 0xb8, 0xb7, 0xb6, 0xb5, 0xb4, 0xb3, 0xb2, 0xb1, 0xb0, 0xaf, 0xae, 0xad,
  0xac, 0xab, 0xaa, 0xa9, 0xa8, 0xa7, 0xa6, 0xa5, 0xa4, 0xa3, 0xa2, 0xa1, 0xa0, 0x9f, 0x9e, 0x9d,
  0x9c, 0x9b, 0x9b, 0x9a, 0x99, 0x98, 0x97, 0x96, 0x95, 0x94, 0x93, 0x92, 0x91, 0x90, 0x8f, 0x8e,
  0x8d, 0x8c, 0x8b, 0x8b, 0x8a, 0x89, 0x88, 0x87, 0x86, 0x85, 0x84, 0x83, 0x82, 0x81, 0x81, 0x80,
  0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x79, 0x78, 0x77, 0x76, 0x75, 0x74, 0x73, 0x72, 0x72,
  0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x67, 0x66, 0x65, 0x64,
  0x63, 0x62, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d, 0x5c, 0x5b, 0x5a, 0x59, 0x59, 0x58, 0x57,
  0x56, 0x55, 0x55, 0x54, 0x53, 0x52, 0x51, 0x51, 0x50, 0x4f, 0x4e, 0x4e, 0x4d, 0x4c, 0x4b, 0x4a,
  0x4a, 0x49, 0x48, 0x47, 0x47, 0x46, 0x45, 0x44, 0x44, 0x43, 0x42, 0x41, 0x41, 0x40, 0x3f, 0x3e,
  0x3e, 0x3d, 0x3c, 0x3c, 0x3b, 0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x34, 0x33,
  0x32, 0x32, 0x31, 0x30, 0x2f, 0x2f, 0x2e, 0x2d, 0x2d, 0x2c, 0x2b, 0x2b, 0x2a, 0x29, 0x29, 0x28,
  0x27, 0x27, 0x26, 0x25, 0x25, 0x24, 0x23, 0x23, 0x22, 0x21, 0x21, 0x20, 0x1f, 0x1f, 0x1e, 0x1d,
  0x1d, 0x1c, 0x1c, 0x1b, 0x1a, 0x1a, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16, 0x15, 0x15, 0x14, 0x14,
  0x13, 0x12, 0x12, 0x11, 0x10, 0x10,  0xf,  0xf,  0xe,  0xe,  0xd,  0xc,  0xc,  0xb,  0xb,  0xa,
     9,    9,    8,    8,    7,    7,    6,    5,    5,    4,    4,    3,    3,    2,    1,    1,
     0,    0,    0,    0, 0xff, 0xfe, 0xfe, 0xfd, 0xfc, 0xfc, 0xfb, 0xfb, 0xfa, 0xf9, 0xf9, 0xf8,
  0xf7, 0xf7, 0xf6, 0xf5, 0xf4, 0xf4, 0xf3, 0xf2, 0xf2, 0xf1, 0xf0, 0xef, 0xee, 0xee, 0xed, 0xec,
  0xeb, 0xea, 0xe9, 0xe8, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4, 0xe3, 0xe2, 0xe1, 0xe0,
};
// Overworld map tile IDs for the 7 dungeon location markers on the light-world map.
static const uint8 kOverworldMapData[7] = {0x79, 0x6e, 0x6f, 0x6d, 0x7c, 0x6c, 0x7f};
// Overworld tile IDs displayed at each of the 8 flute-bird destination icons.
static const uint8 kBirdTravel_tab1[8] = {0x7f, 0x79, 0x6c, 0x6d, 0x6e, 0x6f, 0x7c, 0x7d};
// World-space X coordinates (lo/hi byte) for the 8 flute-bird destinations.
static const uint8 kBirdTravel_x_lo[8] = {0x80, 0xcf, 0x10, 0xb8, 0x30, 0x70, 0x70, 0xf0};
static const uint8 kBirdTravel_x_hi[8] = {6, 0xc, 2, 8, 0xf, 0, 7, 0xe};
// World-space Y coordinates (lo/hi byte) for the 8 flute-bird destinations.
static const uint8 kBirdTravel_y_lo[8] = {0x5b, 0x98, 0xc0, 0x20, 0x50, 0xb0, 0x30, 0x80};
static const uint8 kBirdTravel_y_hi[8] = {3, 5, 7, 0xb, 0xb, 0xf, 0xf, 0xf};
// Bit mask for each of the 3 pendants within the pendant save byte.
static const uint8 kPendantBitMask[3] = {4, 1, 2};
// Bit mask for each of the 7 crystals within the crystal save byte.
static const uint8 kCrystalBitMask[7] = {2, 0x40, 8, 0x20, 1, 4, 0x10};
// World-space X/Y coordinates for the first crystal icon of each dungeon on the overworld map.
// 0xFF00 = not present for this dungeon; 9 entries cover all 9 map areas.
static const uint16 kOwMapCrystal0_x[9] = {0x7ff, 0x2c0, 0xd00, 0xf31, 0x6d, 0x7e0, 0xf40, 0xf40, 0x8dc};
static const uint16 kOwMapCrystal0_y[9] = {0x730, 0x6a0, 0x710, 0x620, 0x70, 0x640, 0x620, 0x620, 0x30};
// Additional crystal icon positions 1–6 for dungeons with multiple crystal targets.
static const uint16 kOwMapCrystal1_x[9] = {0xff00, 0xff00, 0xff00, 0x8d0, 0xff00, 0xff00, 0xff00, 0x82, 0xff00};
static const uint16 kOwMapCrystal1_y[9] = {0xff00, 0xff00, 0xff00, 0x80, 0xff00, 0xff00, 0xff00, 0xb0, 0xff00};
static const uint16 kOwMapCrystal2_x[9] = {0xff00, 0xff00, 0xff00, 0x108, 0xff00, 0xff00, 0xff00, 0xf11, 0xff00};
static const uint16 kOwMapCrystal2_y[9] = {0xff00, 0xff00, 0xff00, 0xd70, 0xff00, 0xff00, 0xff00, 0x103, 0xff00};
static const uint16 kOwMapCrystal3_x[9] = {0xff00, 0xff00, 0xff00, 0x6d, 0xff00, 0xff00, 0xff00, 0x1d0, 0xff00};
static const uint16 kOwMapCrystal3_y[9] = {0xff00, 0xff00, 0xff00, 0x70, 0xff00, 0xff00, 0xff00, 0x780, 0xff00};
static const uint16 kOwMapCrystal4_x[9] = {0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0x100, 0xff00};
static const uint16 kOwMapCrystal4_y[9] = {0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xca0, 0xff00};
static const uint16 kOwMapCrystal5_x[9] = {0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xca0, 0xff00};
static const uint16 kOwMapCrystal5_y[9] = {0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xda0, 0xff00};
static const uint16 kOwMapCrystal6_x[9] = {0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0x759, 0xff00};
static const uint16 kOwMapCrystal6_y[9] = {0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xff00, 0xed0, 0xff00};
// OAM tile/palette words for the crystal icon overlaid on each dungeon's map marker.
// 0 = no icon for this dungeon area; non-zero = tile+palette word for the crystal sprite.
static const uint16 kOwMapCrystal0_tab[9] = {0, 0, 0, 0x6038, 0x6234, 0x6632, 0x6434, 0x6434, 0x6632};
static const uint16 kOwMapCrystal1_tab[9] = {0, 0, 0, 0x6032, 0, 0, 0, 0x6434, 0};
static const uint16 kOwMapCrystal2_tab[9] = {0, 0, 0, 0x6034, 0, 0, 0, 0x6434, 0};
static const uint16 kOwMapCrystal3_tab[9] = {0, 0, 0, 0x6234, 0, 0, 0, 0x6434, 0};
static const uint16 kOwMapCrystal4_tab[9] = {0, 0, 0, 0, 0, 0, 0, 0x6434, 0};
static const uint16 kOwMapCrystal5_tab[9] = {0, 0, 0, 0, 0, 0, 0, 0x6434, 0};
static const uint16 kOwMapCrystal6_tab[9] = {0, 0, 0, 0, 0, 0, 0, 0x6434, 0};
// Overworld map dungeon icon tile IDs (4 variants: pendant/crystal opened/closed).
static const uint8 kOwMap_tab2[4] = {0x68, 0x69, 0x78, 0x69};
// Palette attribute bytes for the 4 overworld map dungeon icon states.
static const uint8 kOverworldMap_Table4[4] = {0x34, 0x74, 0xf4, 0xb4};
// Animation frame durations for the rotating circle (slow/fast phases: 33 and 12 frames).
static const uint8 kOverworldMap_Timer[2] = {33, 12};
// X/Y scroll deltas for the 8 circle-animation trajectory steps (Mode 7 coordinates).
static const int16 kOverworldMap_Table3[8] = {0, 0, 1, 2, -1, -2, 1, 2};
static const int16 kOverworldMap_Table2[8] = {0, 0, 224, 480, -72, -224, 0, 0};
// Module 0x0E (Interface) submodule dispatch table indexed by submodule_index:
// 0 = unused/assert, 1 = HUD, 2 = Text rendering, 3 = Dungeon map,
// 4 = Red potion heal, 5 = Desert prayer iris, 6 = unused/assert,
// 7 = Overworld map, 8 = Green potion magic, 9 = Blue potion both,
// 10 = Flute menu, 11 = Save menu.
static PlayerHandlerFunc *const kMessagingSubmodules[12] = {
  &Module_Messaging_0,
  &Hud_Module_Run,
  &RenderText,
  &Module0E_03_DungeonMap,
  &Module0E_04_RedPotion,
  &Module0E_05_DesertPrayer,
  &Module_Messaging_6,
  &Messaging_OverworldMap,
  &Module0E_08_GreenPotion,
  &Module0E_09_BluePotion,
  &Module0E_0A_FluteMenu,
  &Module0E_0B_SaveMenu,
};
// Game-over animation sprite tile frame sequences (15 frames per table).
// kDeath_AnimCtr0: tile character index cycle; kDeath_AnimCtr1: animation step.
static const uint8 kDeath_AnimCtr0[15] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 5};
static const uint8 kDeath_AnimCtr1[15] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 3, 3, 0x62};
// OAM palette/size flags for the 2 GAME OVER letter-sprite columns.
static const uint8 kDeath_SprFlags[2] = {0x20, 0x10};
// Base tile character for the 2 GAME OVER letter-sprite columns.
static const uint8 kDeath_SprChar0[2] = {0xea, 0xec};
// Y pixel positions for the 3 rows of GAME OVER letter sprites.
static const uint8 kDeath_SprY0[3] = {0x7f, 0x8f, 0x9f};
// Health restored when Link revives after death, indexed by the number of heart containers
// Link has (capped at 20 entries, value in half-heart units). More hearts = more revival HP.
const uint8 kHealthAfterDeath[21] = {
  0x18, 0x18, 0x18, 0x18, 0x18, 0x20, 0x20, 0x28, 0x28, 0x30, 0x30, 0x38, 0x38, 0x38, 0x40, 0x40,
  0x40, 0x48, 0x48, 0x48, 0x50,
};
// Game-over sequence 16-phase dispatch table (indexed by submodule_index during Module12_GameOver).
// Phases: immediately advance → squash animation → delay → iris wipe → GAME OVER bounce →
// splat & fade → finalize → save/continue decision → revival fairy → restore Link.
static PlayerHandlerFunc *const kModule_Death[16] = {
  &GameOver_AdvanceImmediately,
  &Death_Func1,
  &GameOver_DelayBeforeIris,
  &GameOver_IrisWipe,
  &Death_Func4,
  &GameOver_SplatAndFade,
  &Death_Func6,
  &Animate_GAMEOVER_Letters_bounce,
  &GameOver_Finalize_GAMEOVR,
  &GameOver_SaveAndOrContinue,
  &GameOver_InitializeRevivalFairy,
  &RevivalFairy_Main_bounce,
  &GameOver_RiseALittle,
  &GameOver_Restore0D,
  &GameOver_Restore0E,
  &GameOver_ResituateLink,
};
// Starting point indices for the 3 choices in Module1B_SpawnSelect (Link's House / Sanctuary / spawn).
static const uint8 kLocationMenuStartPos[3] = {0, 1, 6};
static void RunInterface();

// Returns a pointer to the floor-layout byte array for the currently active dungeon.
// cur_palace_index_x2 >> 1 = dungeon index (0–13); kDungMap_FloorLayout selects the asset.
const uint8 *GetDungmapFloorLayout() {
  return kDungMap_FloorLayout(cur_palace_index_x2 >> 1).ptr;
}

// Returns the byte at position 'count' within the dungeon tile data for the active dungeon.
// Used by dungeon map drawing code to look up room tile types.
uint8 GetOtherDungmapInfo(int count) {
  const uint8 *p = kDungMap_Tiles(cur_palace_index_x2 >> 1).ptr;
  return p[count];
}

// Scrolls the dungeon map view vertically by dungmap_var4 pixels per frame.
// Decrements the scroll-remaining counter (bottle_menu_expand_row); when it hits zero,
// decrements overworld_map_state to signal the scroll is complete.
void DungMap_4() {
  BG2VOFS_copy2 += dungmap_var4;
  dungmap_var5 -= dungmap_var4;
  if (!--bottle_menu_expand_row)
    overworld_map_state--;
}

// Unused messaging submodule slot (index 6). Should never be dispatched in normal gameplay.
void Module_Messaging_6() {
  assert(0);
}

// Sets up Mode 7 HDMA for the overworld map rotation effect.
// Selects the HDMA source table based on overworld_map_flags (light/dark world),
// then configures 10 scanline HDMA entries using M7A and M7D matrix registers.
void OverworldMap_SetupHdma() {
  // Two HDMA source addresses: one for the light world map, one for the dark world map.
  static const uint32 kOverworldMap_TableLow[2] = {0xabdcf, 0xabdd6};
  uint32 a = kOverworldMap_TableLow[overworld_map_flags];
  HdmaSetup(a, a, 0x42, (uint8)M7A, (uint8)M7D, 10);
}

// Returns a pointer to the light-world overworld tilemap asset used by the overworld map display.
const uint8 *GetLightOverworldTilemap() {
  return kLightOverworldTilemap;
}

// Writes the current save slot to SRAM with a checksum.
// The save data (0x500 bytes from save_dung_info) is written to both the primary mirror
// and the redundant mirror 0xF00 bytes later. The checksum is a simple 16-bit subtraction
// from 0x5A5A over all 0x4FE bytes, stored at offset 0x4FE in both mirrors.
void SaveGameFile() {  // 80894a
  int offs = ((srm_var1 >> 1) - 1) * 0x500;
  memcpy(g_zenv.sram + offs, save_dung_info, 0x500);
  memcpy(g_zenv.sram + offs + 0xf00, save_dung_info, 0x500);
  uint16 t = 0x5a5a;
  for (int i = 0; i < 0x4fe; i += 2)
    t -= *(uint16 *)((char *)save_dung_info + i);
  word_7EF4FE = t;
  WORD(g_zenv.sram[offs + 0x4fe]) = t;
  WORD(g_zenv.sram[offs + 0x4fe + 0xf00]) = t;
  ZeldaWriteSram();
}

// Copies the overworld map graphics (kOverworldMapGfx, 0x4000 bytes) into VRAM.
// Mode 7 tiles use only the high byte of each 16-bit VRAM word; the low byte is the tile index
// which is set separately. This function sets only the high byte (character data).
void TransferMode7Characters() {  // 80e399
  uint16 *dst = g_zenv.vram;
  const uint8 *src = kOverworldMapGfx;
  for (int i = 0; i != 0x4000; i++)
    HIBYTE(dst[i]) = src[i];
}

// Module 0x0E: Interface/Messaging dispatcher. Called each frame while a menu or
// dialogue is active. Conditionally ticks sprites, Link OAM, rain, HUD refill, and lamp
// based on the active submodule and indoor/outdoor state, then dispatches to the active
// submodule via RunInterface(). Updates BG scroll copy registers so the background tracks
// correctly while the interface overlays the game world.
void Module0E_Interface() {  // 80f800
  bool skip_run = false;
  if (player_is_indoors) {
    if (submodule_index == 3) {
      // Dungeon map (submodule 3): skip sprites/OAM during active scroll animation states.
      skip_run = (overworld_map_state != 0 && overworld_map_state != 7);
    } else {
      Dungeon_PushBlock_Handler();
    }
  } else {
    // Overworld map (7) and flute menu (10): skip sprites during active scroll/animation.
    skip_run = ((submodule_index == 7 || submodule_index == 10) && overworld_map_state);
  }
  if (!skip_run) {
    Sprite_Main();
    LinkOam_Main();
    if (!player_is_indoors)
      OverworldOverlay_HandleRain();
    Hud_RefillLogic();
    // During dialogue (submodule 2 = text rendering) the lamp light cone is not updated.
    if (submodule_index != 2)
      OrientLampLightCone();
  }
  RunInterface();
  BG2HOFS_copy = BG2HOFS_copy2 + bg1_x_offset;
  BG2VOFS_copy = BG2VOFS_copy2 + bg1_y_offset;
  BG1HOFS_copy = BG1HOFS_copy2 + bg1_x_offset;
  BG1VOFS_copy = BG1VOFS_copy2 + bg1_y_offset;
}

// Unused messaging submodule slot (index 0). Should never be dispatched in normal gameplay.
void Module_Messaging_0() {  // 80f875
  assert(0);
}

// Dispatches to the active Interface submodule via kMessagingSubmodules[submodule_index].
static void RunInterface() {  // 80f89a
  kMessagingSubmodules[submodule_index]();
}

// Module 0x0E submodule 5: Book of Mudora / Desert Prayer iris spotlight sequence.
// 5-phase effect (subsubmodule_index):
//   0 = reset interface transitions, 1 = apply palette filter (darken),
//   2 = initialize iris HDMA + set mosaic, 3 = continue palette filter + build iris,
//   4 = build iris table only (active spotlight phase with button-dismiss).
void Module0E_05_DesertPrayer() {  // 80f8b1
  switch (subsubmodule_index) {
  case 0: ResetTransitionPropsAndAdvance_ResetInterface(); break;
  case 1: ApplyPaletteFilter_bounce(); break;
  case 2:
    DesertPrayer_InitializeIrisHDMA();
    BYTE(palette_filter_countdown) = mosaic_target_level - 1;
    mosaic_target_level = 0;
    BYTE(darkening_or_lightening_screen) = 2;
    break;
  case 3:
    ApplyPaletteFilter_bounce();
    // fall through
  case 4:
    DesertPrayer_BuildIrisHDMATable();
    break;
  }
}

// Module 0x0E submodule 4: Red Potion effect — refills health one tick per frame.
// When fully healed, clears the Y-button item lock, marks HUD for NMI refresh,
// and restores the previously active module.
void Module0E_04_RedPotion() {  // 80f8fb
  if (Hud_RefillHealth()) {
    button_mask_b_y &= ~0x40;
    flag_update_hud_in_nmi++;
    submodule_index = 0;
    main_module_index = saved_module_for_menu;
  }
}

// Module 0x0E submodule 8: Green Potion effect — refills magic power one tick per frame.
// When fully restored, clears the Y-button item lock, marks HUD for NMI refresh,
// and restores the previously active module.
void Module0E_08_GreenPotion() {  // 80f911
  if (Hud_RefillMagicPower()) {
    button_mask_b_y &= ~0x40;
    flag_update_hud_in_nmi++;
    submodule_index = 0;
    main_module_index = saved_module_for_menu;
  }
}

// Module 0x0E submodule 9: Blue Potion effect — refills both health and magic.
// Switches to submodule 8 (green potion / magic) once health is full,
// then to submodule 4 (red potion / health) once magic is full.
// This creates a two-phase sequential refill matching the original SNES behavior.
void Module0E_09_BluePotion() {  // 80f918
  if (Hud_RefillHealth())
    submodule_index = 8;
  if (Hud_RefillMagicPower())
    submodule_index = 4;
}

// Module 0x0E submodule 0x0B: Continue / Save and Quit menu (pause-screen save dialog).
// Renders the dialogue text box each frame. Once subsubmodule_index counts to 3,
// disables the per-frame BG reload (nmi_load_bg_from_vram = 0).
// When RenderText completes (submodule_index → 0), checks the player's choice:
//   choice_in_multiselect_box != 0  → Save and Quit: module 23 (SaveAndQuit), SFX 15.
//   choice_in_multiselect_box == 0  → Continue: restore choice from backup and dismiss.
void Module0E_0B_SaveMenu() {  // 80f9fa
  // This is the continue / save and quit menu
  if (!player_is_indoors)
    Overworld_DwDeathMountainPaletteAnimation();
  RenderText();
  flag_update_hud_in_nmi = 0;
  nmi_disable_core_updates = 0;
  // Allow 3 frames of BG loading before disabling it to avoid a VRAM flash on entry.
  if (subsubmodule_index < 3)
    subsubmodule_index++;
  else
    nmi_load_bg_from_vram = 0;
  if (!submodule_index) {
    subsubmodule_index = 0;
    nmi_load_bg_from_vram = 1;
    if (choice_in_multiselect_box) {
      sound_effect_ambient = 15;
      main_module_index = 23;
      submodule_index = 1;
      index_of_changable_dungeon_objs[0] = 0;
      index_of_changable_dungeon_objs[1] = 0;
    } else {
      choice_in_multiselect_box = choice_in_multiselect_box_bak;
    }
  }
}

// Module 0x1B: Spawn Select — shown after file load for light-world saves.
// Renders the starting-location dialogue; when RenderText finishes, picks the
// which_starting_point from kLocationMenuStartPos[choice] and calls LoadDungeonRoomRebuildHUD.
// which_starting_point is restored to its original value afterwards so the selected
// starting point overrides only this load (the save retains the original spawn point).
void Module1B_SpawnSelect() {  // 828586
  RenderText();
  if (submodule_index)
    return;
  nmi_load_bg_from_vram = 0;
  EnableForceBlank();
  EraseTileMaps_normal();
  uint8 bak = which_starting_point;
  which_starting_point = kLocationMenuStartPos[choice_in_multiselect_box];
  subsubmodule_index = 0;
  LoadDungeonRoomRebuildHUD();
  which_starting_point = bak;
}

// Sets up HDMA channel 0 to the desert prayer iris table (SNES address 0x02C80C),
// configures window mask registers to clip all BG layers and sprites through the iris window,
// enables HDMA channel 7 (0x80), and clears the 240-entry dynamic HDMA table.
// Called at the start of the iris spotlight effect and when tearing it down.
void CleanUpAndPrepDesertPrayerHDMA() {  // 82c7b8
  HdmaSetup(0, 0x2c80c, 0x41, 0, (uint8)WH0, 0);
  // Enable window masking on BG1/2, BG3/4, and OBJ so the iris clips all layers.
  W12SEL_copy = 0x33;
  W34SEL_copy = 3;
  WOBJSEL_copy = 0x33;
  TMW_copy = TM_copy;
  TSW_copy = TS_copy;
  HDMAEN_copy = 0x80;
  memset(hdma_table_dynamic, 0, 240 * sizeof(uint16));
}

// Initializes the iris spotlight for the Desert Prayer (Book of Mudora) scene.
// Sets the initial iris radius (spotlight_var1 = 0x26 = 38 pixels), clears the
// expand flag (spotlight_var2), runs the first frame of the iris table, then advances
// subsubmodule_index so DesertPrayer_BuildIrisHDMATable continues each frame.
void DesertPrayer_InitializeIrisHDMA() {  // 87ea06
  CleanUpAndPrepDesertPrayerHDMA();
  spotlight_var1 = 0x26;
  BYTE(spotlight_var2) = 0;
  DesertPrayer_BuildIrisHDMATable();
  subsubmodule_index++;
}

// Builds (or updates) the per-scanline HDMA window table for the iris spotlight effect.
// The iris is a circle centered on Link's position. For each scanline, it calculates
// the left (WH0) and right (WH1) window edge using DesertHDMA_CalculateIrisShapeLine.
// When subsubmodule_index == 4 (active spotlight phase) and the player presses A/B (bits 0x40),
// begins expanding the iris (spotlight_var2 = 1) at +8 pixels/frame until >= 0xC0 (192px),
// at which point it transitions back to the previous module and resets HDMA state.
void DesertPrayer_BuildIrisHDMATable() {  // 87ea27
  uint16 r14 = link_y_coord - BG2VOFS_copy2 + 12;
  spotlight_y_lower = r14 - spotlight_var1;
  uint16 r4 = sign16(spotlight_y_lower) ? spotlight_y_lower : 0;
  uint16 k;
  spotlight_y_upper = spotlight_y_lower + spotlight_var1 * 2;
  spotlight_var3 = link_x_coord - BG2HOFS_copy2 + 8;
  spotlight_var4 = 1;
  do {
    uint16 r0 = 0x100, r2 = 0x100;
    if (!(sign16(spotlight_y_lower) || (r4 >= spotlight_y_lower && r4 < spotlight_y_upper))) {
      k = (r4 - 1);
    } else if (spotlight_var1 < spotlight_var4) {
      spotlight_var4 = 1;
      spotlight_y_lower = 0;
      r4 = spotlight_y_upper;
      if (r4 >= 225)
        break;
      k = (r4 - 1);
    } else {
      Pair16U pair = DesertHDMA_CalculateIrisShapeLine();
      if (pair.a == 0) {
        spotlight_y_lower = 0;
      } else {
        r2 = spotlight_var3 + pair.b;
        r0 = spotlight_var3 - pair.b;
      }
      k = (r14 - BYTE(spotlight_var4) - 1);
    }
    uint8 t6 = (r0 < 256) ? r0 : (r0 < 512) ? 255 : 0;
    uint8 t7 = (r2 < 256) ? r2 : 255;
    uint16 r6 = t7 << 8 | t6;
    if (k < 240)
     hdma_table_dynamic[k] = (r6 == 0xffff) ? 0xff : r6;
    if (sign16(spotlight_y_lower) || (r4 >= spotlight_y_lower && r4 < spotlight_y_upper)) {
      k = BYTE(spotlight_var4) - 2 + r14;
      if (k < 240)
        hdma_table_dynamic[k] = (r6 == 0xffff) ? 0xff : r6;
      spotlight_var4++;
    }
    r4++;
  } while (sign16(r4) || r4 < 225);

  if (subsubmodule_index != 4)
    return;
  if (BYTE(spotlight_var2) != 1 && (filtered_joypad_H | filtered_joypad_L) & 0xc0) {
    BYTE(spotlight_var2) = 1;
    BYTE(spotlight_var1) >>= 1;
  }
  if (BYTE(spotlight_var2) && (BYTE(spotlight_var1) += 8) >= 0xc0) {
    byte_7E02F0 ^= 1;
    music_control = 0xf3;
    sound_effect_ambient = 0;
    flag_unk1 = 0;
    some_animation_timer_steps = 0;
    button_mask_b_y = 0;
    link_state_bits = 0;
    link_cant_change_direction &= ~1;
    subsubmodule_index = 0;
    submodule_index = 0;
    main_module_index = saved_module_for_menu;
    TMW_copy = 0;
    TSW_copy = 0;
    W12SEL_copy = 0;
    W34SEL_copy = 0;
    WOBJSEL_copy = 0;
    IrisSpotlight_ResetTable();
  } else {
    static const uint8 kPrayingScene_Delays[5] = {22, 22, 22, 64, 1};
    if (sign8(--link_delay_timer_spin_attack)) {
      int i = some_animation_timer_steps + 1;
      if (i != 4)
        some_animation_timer_steps = i;
      link_delay_timer_spin_attack = kPrayingScene_Delays[i];
    }
  }
}

// Computes the horizontal window half-width (r8) and shape byte (r6) for one scanline
// of the iris spotlight. Uses a precomputed 129-entry table (kPrayingScene_Tab0 for the
// contracting phase, kPrayingScene_Tab1 for the expanding phase) to look up a cos-like
// value. The index is t = (spotlight_var4 / spotlight_var1) >> 1 (normalized scanline position).
// Returns a Pair16U where .a = shape byte (0–0xFF) and .b = half-width in pixels.
Pair16U DesertHDMA_CalculateIrisShapeLine() {  // 87ecdc
  static const uint8 kPrayingScene_Tab1[129] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd, 0xfc, 0xfc, 0xfc, 0xfb, 0xfb, 0xfb, 0xfa, 0xfa, 0xf9, 0xf9, 0xf8, 0xf8,
    0xf7, 0xf7, 0xf6, 0xf6, 0xf5, 0xf5, 0xf4, 0xf3, 0xf3, 0xf2, 0xf1, 0xf1, 0xf0, 0xef, 0xee, 0xee,
    0xed, 0xec, 0xeb, 0xea, 0xe9, 0xe9, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4, 0xe3, 0xe2, 0xe1, 0xdf, 0xde,
    0xdd, 0xdc, 0xdb, 0xda, 0xd8, 0xd7, 0xd6, 0xd5, 0xd3, 0xd2, 0xd0, 0xcf, 0xcd, 0xcc, 0xca, 0xc9,
    0xc7, 0xc6, 0xc4, 0xc2, 0xc1, 0xbf, 0xbd, 0xbb, 0xb9, 0xb7, 0xb6, 0xb4, 0xb1, 0xaf, 0xad, 0xab,
    0xa9, 0xa7, 0xa4, 0xa2, 0x9f, 0x9d, 0x9a, 0x97, 0x95, 0x92, 0x8f, 0x8c, 0x89, 0x86, 0x82, 0x7f,
    0x7b, 0x78, 0x74, 0x70, 0x6c, 0x67, 0x63, 0x5e, 0x59, 0x53, 0x4d, 0x46, 0x3f, 0x37, 0x2d, 0x1f,
    0,
  };
  static const uint8 kPrayingScene_Tab0[129] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xfe, 0xfd, 0xfd, 0xfc, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8,
    0xf7, 0xf6, 0xf5, 0xf4, 0xf3, 0xf1, 0xf0, 0xee, 0xed, 0xeb, 0xe9, 0xe8, 0xe6, 0xe4, 0xe2, 0xdf,
    0xdd, 0xdb, 0xd8, 0xd6, 0xd3, 0xd0, 0xcd, 0xca, 0xc7, 0xc4, 0xc1, 0xbd, 0xb9, 0xb6, 0xb1, 0xad,
    0xa9, 0xa4, 0x9f, 0x9a, 0x95, 0x8f, 0x89, 0x82, 0x7b, 0x74, 0x6c, 0x63, 0x59, 0x4d, 0x3f, 0x2d,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,
  };
  uint8 t = snes_divide(BYTE(spotlight_var4) << 8, BYTE(spotlight_var1)) >> 1;
  uint8 r6 = BYTE(spotlight_var2) ? kPrayingScene_Tab1[t] : kPrayingScene_Tab0[t];
  uint16 r8 = r6 * BYTE(spotlight_var1) >> 8;
  if (BYTE(spotlight_var2))
    r8 <<= 1;
  Pair16U ret = { r6, r8 };
  return ret;
}

// Animates the GAME OVER letter sprites via ancilla slot 0 type:
//   0 = advance submodule (init phase done), 1 = sweep left (letters slide in from right),
//   2 = unfurl right (letters reveal from left), 3 = draw (fully visible, hold).
void Animate_GAMEOVER_Letters() {  // 88f4ca
  switch (ancilla_type[0]) {
  case 0:
    submodule_index++;
    break;
  case 1:
    GameOverText_SweepLeft();
    break;
  case 2:
    GameOverText_UnfurlRight();
    break;
  case 3:
    GameOverText_Draw();
    break;
  }
}

// Sweeps each GAME OVER letter sprite rightward (decreasing X) toward its final resting
// X position defined in kGameOverText_Tab1. Processes one letter per frame via
// flag_for_boomerang_in_place (current letter index 0–7). Once letter k snaps to its
// target, advances k. When k == 8 (all letters placed), advances ancilla_type[0] to
// the UnfurlRight phase, plays SFX 38 (sparkle), and resets hookshot_effect_index.
// hookshot_effect_index tracks the trailing letter that follows the leading letter.
void GameOverText_SweepLeft() {  // 88f4f6
  // Final X pixel positions for each of the 8 letter sprites in the sweep phase.
  static const uint8 kGameOverText_Tab1[8] = {0x40, 0x50, 0x60, 0x70, 0x88, 0x98, 0xa8, 0x40};

  int k = flag_for_boomerang_in_place;
  cur_object_index = k;
  ancilla_x_vel[k] = 0x80;
  Ancilla_MoveX(k);
  if (Ancilla_GetX(k) < kGameOverText_Tab1[k]) {
    ancilla_x_lo[k] = kGameOverText_Tab1[k];
    flag_for_boomerang_in_place = ++k;
    if (k == 8) {
      flag_for_boomerang_in_place = 7;
      ancilla_type[0]++;
      hookshot_effect_index = 0;
      sound_effect_2 = 38;
      goto draw;
    }
  }
  // Trailing letters (index < k) follow the leading letter (k == 7) until
  // hookshot_effect_index catches up with the leader's position.
  if (k == 7) {
    int j = 6;
    while (j != hookshot_effect_index)
      ancilla_x_lo[j--] = ancilla_x_lo[k];
    if (Ancilla_GetX(k) < kGameOverText_Tab1[hookshot_effect_index])
      hookshot_effect_index--;
  }
draw:
  GameOverText_Draw();
}

// Unfurls the GAME OVER letters leftward from a collapsed position to their spread-out
// final positions (kGameOverText_Tab2). Moves one frame at a time via hookshot_effect_index.
// When all 8 letters reach their targets, advances submodule_index and ancilla_type[0].
void GameOverText_UnfurlRight() {  // 88f56d
  // Final X positions for the 8 letter sprites in the unfurl-right phase.
  static const uint8 kGameOverText_Tab2[8] = {0x58, 0x60, 0x68, 0x70, 0x88, 0x90, 0x98, 0xa0};

  int k = flag_for_boomerang_in_place, end;
  cur_object_index = k;
  ancilla_x_vel[k] = 0x60;
  Ancilla_MoveX(k);
  int j = hookshot_effect_index;
  if (ancilla_x_lo[k] >= kGameOverText_Tab2[j]) {
    ancilla_x_lo[j] = kGameOverText_Tab2[j];
    if (++hookshot_effect_index == 8) {
      submodule_index++;
      ancilla_type[0]++;
      goto draw;
    }
  }
  end = hookshot_effect_index - 1;
  k = flag_for_boomerang_in_place;
  j = k;
  do {
    ancilla_x_lo[j] = ancilla_x_lo[k];
  } while (--j != end);
draw:
  GameOverText_Draw();
}

// Module 0x12: Game Over. Dispatches through kModule_Death[submodule_index] each frame.
// Ticks Link's OAM for all phases except phase 9 (save/continue screen) where OAM is managed
// by the choice-fairy animation instead.
void Module12_GameOver() {  // 89f290
  kModule_Death[submodule_index]();
  if (submodule_index != 9)
    LinkOam_Main();
}

// Game over phase 0: Immediately advances to phase 1 and initializes the death sequence.
// Called when a game-over is triggered while Link is already flagged for game-over animation.
void GameOver_AdvanceImmediately() {  // 89f2a2
  submodule_index++;
  Death_Func1();
}

// Game over phase 1: Backs up the current music, palette state, and color math registers,
// then stops music (music_control = 241 = stop), clears the screen palette to dark,
// saves palette filter state to mapbak_bg1_*offset, and sets up for the 32-frame delay
// before the iris wipe begins. Advances to phase 2.
void Death_Func1() {  // 89f2a4
  music_unk1_death = music_unk1;
  sound_effect_ambient_last_death = sound_effect_ambient_last;
  music_control = 241;
  sound_effect_ambient = 5;
  overworld_map_state = 5;
  link_on_conveyor_belt = 0;
  byte_7E0322 = 0;
  link_cape_mode = 0;
  mapbak_bg1_x_offset = palette_filter_countdown;
  mapbak_bg1_y_offset = darkening_or_lightening_screen;
  memcpy(mapbak_palette, aux_palette_buffer, 256);
  memset(aux_palette_buffer + 32, 0, 192);
  palette_filter_countdown = 0;
  darkening_or_lightening_screen = 0;
  bg1_x_offset = 0;
  bg1_y_offset = 0;
  mapbak_CGWSEL = WORD(CGWSEL_copy);
  some_menu_ctr = 32;
  hud_floor_changed_timer = 0;
  Hud_FloorIndicator();
  flag_update_hud_in_nmi++;
  sound_effect_ambient = 5;
  submodule_index++;
}

// Game over phase 2: 32-frame delay (some_menu_ctr countdown). When expired, initializes
// GAME OVER letter ancillae, starts the iris spotlight wipe, and enables object-layer
// window masking (WOBJSEL 0x30 = clip objects within window 2).
void GameOver_DelayBeforeIris() {  // 89f33b
  if (--some_menu_ctr)
    return;
  Death_InitializeGameOverLetters();
  IrisSpotlight_close();
  WOBJSEL_copy = 0x30;
  W34SEL_copy = 0;
  submodule_index++;
}

// Game over phase 3: Runs the iris spotlight wipe, restoring BG subtractive color math
// each frame. When IrisSpotlight_ConfigureTable reports complete (submodule_index → 0),
// fills all BG palettes with 0x18 (dark reddish-brown), resets window and color math
// registers, and configures additive color blending for the GAME OVER letter glow.
// Transitions to phase 4 (SplatAndFade) with a 64-frame delay. Calls Death_PrepFaint
// to start Link's faint animation.
void GameOver_IrisWipe() {  // 89f350
  PaletteFilter_RestoreBGSubstractiveStrict();
  main_palette_buffer[0] = main_palette_buffer[32];
  uint8 bak = main_module_index;
  IrisSpotlight_ConfigureTable();
  main_module_index = bak;
  if (submodule_index)
    return;
  // Set all sprite/BG palette entries to 0x18 (a dark color) for the wipe-complete state.
  for (int i = 0; i < 16; i++) {
    main_palette_buffer[0x20 + i] = 0x18;
    main_palette_buffer[0x30 + i] = 0x18;
    main_palette_buffer[0x40 + i] = 0x18;
    main_palette_buffer[0x50 + i] = 0x18;
    main_palette_buffer[0x60 + i] = 0x18;
    main_palette_buffer[0x70 + i] = 0x18;
  }
  main_palette_buffer[0] = main_palette_buffer[32] = 0x18;

  IrisSpotlight_ResetTable();
  // COLDATA: R=32, G=64, B=128 — configures fixed-color additive blend for letter glow.
  COLDATA_copy0 = 32;
  COLDATA_copy1 = 64;
  COLDATA_copy2 = 128;
  W12SEL_copy = 0;
  W34SEL_copy = 0;
  WOBJSEL_copy = 0;
  submodule_index = 4;
  flag_update_cgram_in_nmi++;
  INIDISP_copy = 15;
  TM_copy = 20;
  TS_copy = 0;
  CGADSUB_copy = 32;
  some_menu_ctr = 64;
  BYTE(palette_filter_countdown) = 0;
  BYTE(darkening_or_lightening_screen) = 0;
  Death_PrepFaint();
}

// Game over phase 5: 64-frame delay (some_menu_ctr), then restores BG subtractive blend
// and fades the background to full black (darkening_or_lightening_screen == 0xFF).
// When complete, checks for a fairy revive (bottle content == 6 = fairy). If a fairy is
// present, empties the bottle, triggers GraphicsLoadChrHalfSlot, and jumps to phase 10
// (fairy revival). Otherwise advances to phase 6 (GAME OVER letters), sets NMI subroutine 22.
void GameOver_SplatAndFade() {  // 89f3de
  if (some_menu_ctr) {
    some_menu_ctr--;
    return;
  }
  PaletteFilter_RestoreBGSubstractiveStrict();
  main_palette_buffer[0] = main_palette_buffer[32];
  if (BYTE(darkening_or_lightening_screen) != 0xff)
    return;
  mosaic_level = 0;
  mosaic_inc_or_dec = 0;
  MOSAIC_copy = 3;

  // Scan all 4 bottle slots for a fairy (content value 6); if found, trigger revival sequence.
  for (int i = 0; i != 4; i++) {
    if (link_bottle_info[i] == 6) {
      link_bottle_info[i] = 2;
      some_menu_ctr = 12;
      load_chr_halfslot_even_odd = 15;
      Graphics_LoadChrHalfSlot();
      load_chr_halfslot_even_odd = 0;
      submodule_index = 10;
      return;
    }
  }
  index_of_changable_dungeon_objs[0] = 0;
  index_of_changable_dungeon_objs[1] = 0;
  nmi_subroutine_index = 22;
  nmi_disable_core_updates = 22;
  submodule_index++;
}

// Game over phase 6 (fairy revival path): Loads CHR half-slot 15 (fairy sprite graphics),
// loads sprite environment and main palettes, marks CGRAM for NMI update,
// then immediately plays the Link-swoon animation and advances to phase 7.
void Death_Func6() {  // 89f458
  some_menu_ctr = 12;
  load_chr_halfslot_even_odd = 15;
  Graphics_LoadChrHalfSlot();
  load_chr_halfslot_even_odd = 0;
  palette_sp6r_indoors = 5;
  overworld_palette_aux_or_main = 0x200;
  Palette_Load_SpriteEnvironment_Dungeon();
  Palette_Load_SpriteMain();
  flag_update_cgram_in_nmi++;
  submodule_index++;
  Death_PlayerSwoon();
}

// Game over phase 4 (fairy revival path): Continues playing the Link-swoon animation each frame.
void Death_Func4() {  // 89f47e
  Death_PlayerSwoon();
}

// Trampoline for Animate_GAMEOVER_Letters, used as a phase-dispatch function pointer.
void Animate_GAMEOVER_Letters_bounce() {  // 89f483
  Animate_GAMEOVER_Letters();
}

// Game over phase 8: Animates GAME OVER letters while rendering the save/continue text.
// Backs up main_module_index and submodule_index, calls RenderText (messaging_module = 2
// selects RenderText_PostDeathSaveOptions), then restores them + advances submodule_index.
// Starts game-over music track (music_control = 11) and sets some_menu_ctr = 2 (debounce).
void GameOver_Finalize_GAMEOVR() {  // 89f488
  Animate_GAMEOVER_Letters();
  uint8 bak1 = main_module_index;
  uint8 bak2 = submodule_index;
  messaging_module = 2;
  RenderText();
  submodule_index = bak2 + 1;
  main_module_index = bak1;
  some_menu_ctr = 2;
  music_control = 11;
}

// Game over phase 9: The save/continue/save-and-quit choice screen.
// Animates the choice fairy (blinking sprite at the currently selected option),
// continues animating GAME OVER letters, and handles D-pad input to cycle between
// the 3 options (subsubmodule_index 0/1/2 = Continue / Save and Continue / Save and Quit).
// Select button (filtered_joypad_H & 0x20) also advances the choice.
// A/B/Y/Start confirms the selection and calls Death_Func15.
// count_as_death = true unless the player chose "Save and Quit" (option 2).
void GameOver_SaveAndOrContinue() {  // 89f4c1
  GameOver_AnimateChoiceFairy();
  if (ancilla_type)
    Animate_GAMEOVER_Letters();

  if (filtered_joypad_H & 0x20)
    goto do_inc;

  if (!--some_menu_ctr) {
    some_menu_ctr = 1;
    if (joypad1H_last & 12) {
      if (joypad1H_last & 4) {
do_inc:
        if (++subsubmodule_index >= 3)
          subsubmodule_index = 0;
      } else {
        if (sign8(--subsubmodule_index))
          subsubmodule_index = 2;
      }
      some_menu_ctr = 12;
      sound_effect_2 = 32;
    }
  }
  if (!((filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0))
    return;
  sound_effect_1 = 44;
  // Only death with save/continue or save/quit counts as a death
  Death_Func15(subsubmodule_index != 2);
}

// Game over finalization: resets Link's state, saves if needed, and routes back to the game.
// count_as_death: true for Continue/Save-and-Continue (death tally incremented), false for Save-and-Quit.
// Stops music, flags dungeon quadrants, adjusts bunny status, resets health to kHealthAfterDeath,
// clears all sprites, and then branches on subsubmodule_index:
//   0 = Continue (no save, return to dungeon or overworld),
//   1 = Save and Continue (save + return to dungeon entry or overworld),
//   2 = Save and Quit (save + reset scroll + wipe dungeon state + return to file select).
void Death_Func15(bool count_as_death) {  // 89f50f
  music_control = 0xf1;
  if (player_is_indoors)
    Dungeon_FlagRoomData_Quadrants();
  AdjustLinkBunnyStatus();
  if (sram_progress_indicator < 3) {
    savegame_is_darkworld = 0;
    if (!link_item_moon_pearl)
      ForceNonbunnyStatus();
  }
  if (dungeon_room_index == 0)
    player_is_indoors = 0;

  ResetSomeThingsAfterDeath((uint8)dungeon_room_index);
  if (follower_indicator == 6 || follower_indicator == 9 || follower_indicator == 10 || follower_indicator == 13)
    follower_indicator = 0;

  death_var4 = link_health_current = kHealthAfterDeath[link_health_capacity >> 3];
  uint8 i = BYTE(cur_palace_index_x2);
  if (i != 0xff)
    link_keys_earned_per_dungeon[(i == 2 ? 0 : i) >> 1] = link_num_keys;
  Sprite_ResetAll();
  if (death_var2 == 0xffff && (!(enhanced_features0 & kFeatures0_MiscBugFixes) || count_as_death))
    death_save_counter++;
  death_var5++;
  if (subsubmodule_index != 1) {
    if (!player_is_indoors)
      goto outdoors;

    if (follower_indicator != 1 && BYTE(cur_palace_index_x2) != 255) {
      death_var4 = 0;
    } else {
      queued_music_control = 0;
      player_is_indoors = 0;
    outdoors:
      if (savegame_is_darkworld)
        dungeon_room_index = 32;
    }

    if (sram_progress_indicator) {
      if (subsubmodule_index == 0)
        SaveGameFile();
      main_module_index = 5;
      submodule_index = 0;
      nmi_load_bg_from_vram = 0;
    } else {
      uint8 slot = srm_var1;
      int offs = kSrmOffsets[(slot >> 1) - 1];
      WORD(g_ram[0]) = offs;
      death_var5 = 0;
      CopySaveToWRAM();
    }
  } else {
    if (sram_progress_indicator)
      SaveGameFile();
    TM_copy = 16;
    player_is_indoors = 0;
    Death_Func31();
    death_var4 = 0;
    death_var5 = 0;
    queued_music_control = 0;
    BG1HOFS_copy2 = 0;
    BG2HOFS_copy2 = 0;
    BG3HOFS_copy2 = 0;
    BG1VOFS_copy2 = 0;
    BG2VOFS_copy2 = 0;
    BG3VOFS_copy2 = 0;
    BG1HOFS_copy = 0;
    BG2HOFS_copy = 0;
    BG1VOFS_copy = 0;
    BG2VOFS_copy = 0;
    memset(save_dung_info, 0, 256 * 5);
    flag_which_music_type = 0;
    LoadOverworldSongs();
  }
}

// Draws the blinking fairy sprite beside the currently highlighted save/continue option.
// Uses oam_buf slot 0x14, positioning from kDeath_SprY0[subsubmodule_index] (one of 3 rows),
// and alternates tile between two frames every 8 display frames (frame_counter >> 3 & 1).
void GameOver_AnimateChoiceFairy() {  // 89f67a
  SetOamPlain(&oam_buf[0x14], 0x34, kDeath_SprY0[subsubmodule_index], kDeath_SprChar0[frame_counter >> 3 & 1], 0x78, 2);
}

// Game over fairy-revival path (player had a fairy bottle): Spawns the revival ancilla,
// primes the heart-refill counter (link_hearts_filler = 56), advances the submodule,
// and clears overworld_map_state to reset the revival animation sequence.
void GameOver_InitializeRevivalFairy() {  // 89f6a4
  ConfigureRevivalAncillae();
  link_hearts_filler = 56;
  submodule_index += 1;
  overworld_map_state = 0;
}

// Trampoline invoking RevivalFairy_Main, used as a phase-dispatch function pointer.
void RevivalFairy_Main_bounce() {  // 89f6b4
  RevivalFairy_Main();
}

// Fairy revival: Drives the revival fairy animation and HUD heart refill.
// When link_hearts_filler reaches 0 (all hearts refilled), restores the main
// palette from the map backup, clears the sub-palettes, resets palette blending,
// and advances the submodule to begin restoring graphics state.
void GameOver_RiseALittle() {  // 89f6b9
  if (link_hearts_filler == 0) {
    memcpy(aux_palette_buffer, mapbak_palette, 256);
    memset(main_palette_buffer + 32, 0, 192);
    main_palette_buffer[0] = 0;
    palette_filter_countdown = 0;
    darkening_or_lightening_screen = 2;
    WORD(CGWSEL_copy) = mapbak_CGWSEL;
    submodule_index++;
  }
  RevivalFairy_Main();
  Hud_RefillLogic();
}

// Fairy revival: Waits for the heart animation to finish (is_doing_heart_animation == 0),
// then reloads the CHR half-slot, applies the stored fixed-color offset, and advances the submodule.
// Continues running the revival fairy sprite and HUD refill logic each frame.
void GameOver_Restore0D() {  // 89f71d
  if (!is_doing_heart_animation) {
    load_chr_halfslot_even_odd = 1;
    Graphics_LoadChrHalfSlot();
    Dungeon_ApproachFixedColor_variable(overworld_fixed_color_plusminus);
    submodule_index++;
  }
  RevivalFairy_Main();
  Hud_RefillLogic();
}

// Fairy revival: Reloads the CHR half-slot and restores TS_copy from the map backup,
// then advances the submodule to begin the final palette fade-in.
void GameOver_Restore0E() {  // 89f735
  Graphics_LoadChrHalfSlot();
  TS_copy = mapbak_TS;
  submodule_index++;
}

// Fairy revival final phase: Fades the palette back in via PaletteFilter_RestoreBGAdditiveStrict.
// Once fully brightened (palette_filter_countdown == 32), restores overworld scroll if outdoors,
// returns to the saved game module, resets the blink timer, reloads music, and restores
// the ambient SFX and palette state that were backed up before the game-over sequence began.
void GameOver_ResituateLink() {  // 89f742
  PaletteFilter_RestoreBGAdditiveStrict();
  main_palette_buffer[0] = main_palette_buffer[32];
  if (BYTE(palette_filter_countdown) != 32)
    return;
  if (!player_is_indoors)
    Overworld_SetFixedColAndScroll();
  TS_copy = mapbak_TS;
  main_module_index = saved_module_for_menu;
  submodule_index = 0;
  countdown_for_blink = 144;
  music_control = music_unk1_death;
  sound_effect_ambient = sound_effect_ambient_last_death;
  palette_filter_countdown = mapbak_bg1_x_offset;
  darkening_or_lightening_screen = mapbak_bg1_y_offset;
}

// Module 0x0E, submodule 0x0A: Flute/ocarina destination picker (bird-travel menu).
// Dispatches a 10-phase overworld-map sequence via overworld_map_state:
//   0 = WorldMap_FadeOut     — dim the display before map setup.
//   1 = LoadLightWorldMap    — set up Mode 7 tilemap for the Light World.
//   2 = LoadSpriteGFX        — load map sprite graphics.
//   3 = WorldMap_Brighten    — fade the map in.
//   4 = seed some_menu_ctr and advance (initial input debounce).
//   5 = FluteMenu_HandleSelection — accept D-pad input to choose destination.
//   6 = WorldMap_RestoreGraphics  — fade back out.
//   7 = FluteMenu_LoadSelectedScreen — configure the destination area.
//   8 = Overworld_LoadOverlayAndMap — load overworld tiles for the chosen area.
//   9 = FluteMenu_FadeInAndQuack   — fade in and trigger the bird arrival sfx.
void Module0E_0A_FluteMenu() {  // 8ab730
  switch (overworld_map_state) {
  case 0:
    WorldMap_FadeOut();
    break;
  case 1:
    birdtravel_var1[0] = 0;
    WorldMap_LoadLightWorldMap();
    break;
  case 2:
    WorldMap_LoadSpriteGFX();
    break;
  case 3:
    WorldMap_Brighten();
    break;
  case 4:
    some_menu_ctr = 0x10;
    overworld_map_state++;
    break;
  case 5:
    FluteMenu_HandleSelection();
    break;
  case 6:
    WorldMap_RestoreGraphics();
    break;
  case 7:
    FluteMenu_LoadSelectedScreen();
    break;
  case 8:
    Overworld_LoadOverlayAndMap();
    break;
  case 9:
    FluteMenu_FadeInAndQuack();
    break;
  default:
    assert(0);
  }
}

// Flute menu interaction: Handles D-pad selection among the 8 bird-travel destinations
// and draws all destination icons on the Mode 7 map.
// A/B confirms the selection (advances overworld_map_state); kFeatures0_CancelBirdTravel
// allows B to cancel by passing some_menu_ctr = joypad value to the next phase.
// D-pad left/right wraps birdtravel_var1[0] through 0-7 and plays the cursor-move sfx.
// Each frame, all 8 destination icons (kBirdTravel_x/y tables) are projected onto the
// Mode 7 map via WorldMap_CalculateOamCoordinates, the selected one rendered with the
// animated tile (0x30 + (frame_counter & 6)) and the rest with the static tile (0x32).
// The player-position icon is also drawn (blinking every 0x10 frames).
void FluteMenu_HandleSelection() {  // 8ab78b
  Point16U pt;

  if (some_menu_ctr == 0) {
    if ((joypad1L_last | joypad1H_last) & 0xc0) {

      if (enhanced_features0 & kFeatures0_CancelBirdTravel)
        some_menu_ctr = joypad1L_last;

      overworld_map_state++;
      return;
    }
  } else {
    some_menu_ctr--;
  }
  if (filtered_joypad_H & 10) {
    birdtravel_var1[0]--;
    sound_effect_2 = 32;
  }
  if (filtered_joypad_H & 5) {
    birdtravel_var1[0]++;
    sound_effect_2 = 32;
  }
  birdtravel_var1[0] = birdtravel_var1[0] & 7;
  if (frame_counter & 0x10 && WorldMap_CalculateOamCoordinates(&pt))
    WorldMap_AddSprite(16, 2, 0x3e, 0, pt.x - 4, pt.y - 4);

  uint16 ybak = link_y_coord_spexit;
  uint16 xbak = link_x_coord_spexit;
  for (int i = 7; i >= 0; i--) {
    bird_travel_x_lo[i] = kBirdTravel_x_lo[i];
    bird_travel_x_hi[i] = kBirdTravel_x_hi[i];
    link_x_coord_spexit = kBirdTravel_x_hi[i] << 8 | kBirdTravel_x_lo[i];

    bird_travel_y_lo[i] = kBirdTravel_y_lo[i];
    bird_travel_y_hi[i] = kBirdTravel_y_hi[i];
    link_y_coord_spexit = kBirdTravel_y_hi[i] << 8 | kBirdTravel_y_lo[i];

    if (WorldMap_CalculateOamCoordinates(&pt))
      WorldMap_AddSprite(i, 0, (i == birdtravel_var1[0]) ? 0x30 + (frame_counter & 6) : 0x32, kBirdTravel_tab1[i], pt.x, pt.y);
  }
  link_x_coord_spexit = xbak;
  link_y_coord_spexit = ybak;
}

// Flute menu: Configures the overworld screen for the selected bird-travel destination.
// Clears the ocarina-event flags (Zora's Pond, Haunted Grove, desert palace map flags),
// loads the transport link unless B was used to cancel (kFeatures0_CancelBirdTravel),
// loads palettes, decompresses animated tiles, sets the fixed colour and scroll,
// initialises tilesets, triggers the overlay load, and queues bird-arrival music.
// Decrements submodule_index to remain in the flute-menu module during tile loading.
void FluteMenu_LoadSelectedScreen() {  // 8ab8c5
  save_ow_event_info[0x3b] &= ~0x20;
  save_ow_event_info[0x7b] &= ~0x20;
  save_dung_info[267] &= ~0x80;
  save_dung_info[40] &= ~0x100;

  // This is kFeatures0_CancelBirdTravel
  if (!(some_menu_ctr & 0x40))
    FluteMenu_LoadTransport();

  FluteMenu_LoadSelectedScreenPalettes();
  uint8 t = overworld_screen_index & 0xbf;
  DecompressAnimatedOverworldTiles((t == 3 || t == 5 || t == 7) ? 0x58 : 0x5a);
  Overworld_SetFixedColAndScroll();
  overworld_palette_aux_or_main = 0;
  hud_palette = 0;
  InitializeTilesets();
  overworld_map_state++;
  BYTE(dung_draw_width_indicator) = 0;
  Overworld_LoadOverlays2();
  submodule_index--;
  sound_effect_2 = 16;
  uint8 m = overworld_music[BYTE(overworld_screen_index)];
  sound_effect_ambient = m >> 4;
  music_control = ZeldaIsPlayingMusicTrack(m & 0xf) ? 0xf3 : m & 0xf;
}

// Flute menu: Runs Overworld_LoadAndBuildScreen while preserving module/map state.
// Backs up main_module_index and overworld_map_state, calls the overworld tile loader,
// then restores them with overworld_map_state incremented so the next phase can proceed.
void Overworld_LoadOverlayAndMap() {  // 8ab948
  uint16 bak1 = WORD(main_module_index);
  uint16 bak2 = WORD(overworld_map_state);
  Overworld_LoadAndBuildScreen();
  WORD(overworld_map_state) = bak2 + 1;
  WORD(main_module_index) = bak1;
}

// Flute menu: Fades in the destination screen one brightness step per frame.
// When fully bright (INIDISP_copy == 15), calls BirdTravel_Finish_Doit to finalise
// the bird arrival; otherwise runs the sprite system to animate the bird in flight.
void FluteMenu_FadeInAndQuack() {  // 8ab964
  if (++INIDISP_copy == 15) {
    BirdTravel_Finish_Doit();
  } else {
    Sprite_Main();
  }
}

// Finalises a bird-travel sequence: resets map-state and subsubmodule counters,
// restores the game module (main_module_index = saved_module_for_menu, submodule_index = 0),
// restores HDMA from the pre-map backup, spawns the "add bird" ancilla (sprite 0x27, type 4),
// and runs the sprite system one final frame so the bird lands visually.
void BirdTravel_Finish_Doit() {  // 8ab96c
  overworld_map_state = 0;
  subsubmodule_index = 0;
  main_module_index = saved_module_for_menu;
  submodule_index = 0;
  HDMAEN_copy = mapbak_HDMAEN;
  AddBirdTravelSomething(0x27, 4);
  Sprite_Main();
}

// Module 0x0E, submodule for the overworld map (Map button / Select button view).
// Dispatches an 8-phase Mode 7 map sequence via overworld_map_state:
//   0 = WorldMap_FadeOut           — fade screen to black, back up scroll and palette.
//   1 = WorldMap_LoadLightWorldMap — set up Mode 7 tiles for the Light World.
//   2 = WorldMap_LoadDarkWorldMap  — overlay the Dark World tilemap if currently there.
//   3 = WorldMap_LoadSpriteGFX     — load map sprite graphics (Link icon, crystals, etc.).
//   4 = WorldMap_Brighten          — fade the map in.
//   5 = WorldMap_PlayerControl     — accept input: zoom toggle, pan in zoomed mode, exit.
//   6 = WorldMap_RestoreGraphics   — fade the map back out and restore main scroll.
//   7 = WorldMap_ExitMap           — restore tilesets/palette, return to game module.
void Messaging_OverworldMap() {  // 8ab98b
  switch (overworld_map_state) {
  case 0:
    WorldMap_FadeOut();
    break;
  case 1:
    WorldMap_LoadLightWorldMap();
    break;
  case 2:
    WorldMap_LoadDarkWorldMap();
    break;
  case 3:
    WorldMap_LoadSpriteGFX();
    break;
  case 4:
    WorldMap_Brighten();
    break;
  case 5:
    WorldMap_PlayerControl();
    break;
  case 6:
    WorldMap_RestoreGraphics();
    break;
  case 7:
    WorldMap_ExitMap();
    break;
  }
}

// Overworld-map phase 0: Decrements brightness one step per frame; when fully black,
// backs up HDMA, enables force-blank, enables MOSAIC, advances the map phase,
// saves TM/TS and scroll registers, zeroes all BG scroll positions,
// backs up CGWSEL, stores Link's current screen position as the map exit point,
// and triggers mode-7 music + sound effects. Also sets BGMODE_copy = 7 for Mode 7 rendering.
// If game progress < 2 (Light World only), forces sub-colour additive blending off.
void WorldMap_FadeOut() {  // 8ab9a3
  if (--INIDISP_copy)
    return;
  mapbak_HDMAEN = HDMAEN_copy;
  EnableForceBlank();
  MOSAIC_copy = 3;
  overworld_map_state++;
  WORD(mapbak_TM) = WORD(TM_copy);
  mapbak_BG1HOFS_copy2 = BG1HOFS_copy2;
  mapbak_BG2HOFS_copy2 = BG2HOFS_copy2;
  mapbak_BG1VOFS_copy2 = BG1VOFS_copy2;
  mapbak_BG2VOFS_copy2 = BG2VOFS_copy2;
  BG1HOFS_copy2 = BG2HOFS_copy2 = BG3HOFS_copy2 = 0;
  BG1VOFS_copy2 = BG2VOFS_copy2 = BG3VOFS_copy2 = 0;
  WORD(mapbak_CGWSEL) = WORD(CGWSEL_copy);
  link_dma_graphics_index = 0x1fc;
  if (BYTE(overworld_screen_index) < 0x80) {
    link_y_coord_spexit = link_y_coord;
    link_x_coord_spexit = link_x_coord;
  }
  if (sram_progress_indicator < 2) {
    CGWSEL_copy = 0x80;
    CGADSUB_copy = 0x61;
  }
  sound_effect_2 = 16;
  sound_effect_ambient = 5;
  music_control = 0xf2;
  BGMODE_copy = 7;
}

// Overworld-map phase 1: Fills the Mode 7 VRAM tilemap with the default backdrop (0xef),
// sets TM_copy = 0x11 (BG1 + OBJ) and TS_copy = 0, uploads the Mode 7 character data,
// sets up the perspective HDMA table, loads palette data, triggers a CGRAM upload,
// sets nmi_subroutine_index = 7 (Mode 7 matrix NMI), zeroes INIDISP (screen off),
// and disables core NMI updates until the map is fully initialised.
void WorldMap_LoadLightWorldMap() {  // 8aba30
  WorldMap_FillTilemapWithEF();
  TM_copy = 0x11;
  TS_copy = 0;
  TransferMode7Characters();
  WorldMap_SetUpHDMA();
  LoadOverworldMapPalette();
  LoadActualGearPalettes();
  flag_update_cgram_in_nmi++;
  nmi_subroutine_index = 7;
  INIDISP_copy = 0;
  nmi_disable_core_updates++;
  overworld_map_state++;
}

// Overworld-map phase 2: If the player is in the Dark World (overworld_screen_index & 0x40),
// copies kDarkOverworldTilemap (1024 bytes) into the Mode 7 UV-RAM and queues an NMI
// subroutine (index 21) to upload it. Always advances the map phase.
void WorldMap_LoadDarkWorldMap() {  // 8aba7a
  if (overworld_screen_index & 0x40) {
    memcpy(&uvram, kDarkOverworldTilemap, 1024);
    nmi_subroutine_index = 21;
  }
  overworld_map_state++;
}

// Overworld-map phase 3: Loads the sprite CHR half-slot for the map icon graphics
// (Link position dot, crystal/pendant icons, bird icon) and advances the map phase.
void WorldMap_LoadSpriteGFX() {  // 8aba9a
  load_chr_halfslot_even_odd = 0x10;
  Graphics_LoadChrHalfSlot();
  load_chr_halfslot_even_odd = 0;
  overworld_map_state++;
}

// Overworld-map phase 4: Increments INIDISP_copy (screen brightness) one step per frame.
// When INIDISP_copy reaches 15 (full brightness), advances overworld_map_state to the
// player-control phase.
void WorldMap_Brighten() {  // 8abaaa
  if (++INIDISP_copy == 15)
    overworld_map_state++;
}

// Returns true when the player presses the button that exits or toggles the overworld map.
// The active button depends on whether a Y-item slot is in use (hud_cur_item_x != 0):
//   Y-item present: Select button (filtered_joypad_H & 0x20).
//   No Y-item:      X button (filtered_joypad_L & 0x40).
bool DidPressButtonForMap() {
  if (hud_cur_item_x != 0)
    return filtered_joypad_H & 0x20;  // select
  else
    return filtered_joypad_L & 0x40;  // x
}

// Overworld-map phase 5: Main player-control loop for the Mode 7 map.
// On the first frame after a zoom change (overworld_map_flags & 0x80), re-installs the
// perspective HDMA table. Pressing the map-exit button advances the phase to close the map.
// L/R shoulder or the exit button cycles the zoom level (overworld_map_flags 0=full, 1=zoomed):
//   zoomed-in: centred on Link's position with 5/2 horizontal perspective scale.
//   zoomed-out: fixed centre at (128, 200).
// In zoomed mode, D-pad pans BG1HOFS/VOFS toward the targets in kOverworldMap_Table2/3.
// Calls WorldMap_HandleSprites each frame to draw the position dot and collectible icons.
void WorldMap_PlayerControl() {  // 8abae6
  if (overworld_map_flags & 0x80) {
    overworld_map_flags &= ~0x80;
    OverworldMap_SetupHdma();
  }

  if (!overworld_map_flags && DidPressButtonForMap()) { // X
    // getout
    overworld_map_state++;
    return;
  }
  if (BYTE(dung_draw_width_indicator)) {
    BYTE(dung_draw_width_indicator)--;
  } else if (filtered_joypad_L & 0x30 || DidPressButtonForMap()) {
    // next zoom level
    sound_effect_2 = 36;
    BYTE(dung_draw_width_indicator) = 8;

    int t = overworld_map_flags ^ 1;
    overworld_map_flags = t | 0x80;
    timer_for_mode7_zoom = kOverworldMap_Timer[t];
    if (timer_for_mode7_zoom == 12) {
      BG1VOFS_copy2 = ((link_y_coord_spexit >> 4) - 0x48 & ~1);
      M7Y_copy = BG1VOFS_copy2 + 0x100;
      uint16 t0 = (link_x_coord_spexit >> 4) - 0x80;
      uint16 t1 = (uint16)(5 * (sign16(t0) ? -t0 : t0)) >> 1;
      uint16 t2 = sign16(t0) ? -t1 : t1;
      BG1HOFS_copy2 = t2 + 0x80 & ~1;
    } else {
      BG1VOFS_copy2 = 200;
      M7Y_copy = 200 + 256;
      BG1HOFS_copy2 = 128;
    }
  }

  if (overworld_map_flags) {
    int k = (joypad1H_last & 12) >> 1;
    if (BG1VOFS_copy2 != (uint16)kOverworldMap_Table2[k]) {
      BG1VOFS_copy2 += kOverworldMap_Table3[k];
      M7Y_copy = BG1VOFS_copy2 + 0x100;
    }
    k = (joypad1H_last & 3) * 2 + 1;
    if (BG1HOFS_copy2 != (uint16)kOverworldMap_Table2[k])
      BG1HOFS_copy2 += kOverworldMap_Table3[k];
  }
  WorldMap_HandleSprites();
}

// Overworld-map phase 6: Fades the map out (decrements INIDISP_copy one step per frame).
// When fully dark, enables force-blank, advances the phase, restores the main palette
// from the aux backup, restores CGWSEL, resets BG3 scroll, and restores all BG1/BG2
// scroll registers from the pre-map backups. Then sets up the conclusion HDMA for the
// transition back to the game (Mode 1 with the 2D perspective re-engaged).
void WorldMap_RestoreGraphics() {  // 8abbd6
  if (--INIDISP_copy)
    return;
  EnableForceBlank();
  overworld_map_state++;
  memcpy(main_palette_buffer, aux_palette_buffer, 512);
  WORD(CGWSEL_copy) = WORD(mapbak_CGWSEL);
  BG3HOFS_copy2 = BG3VOFS_copy2 = 0;
  BG1HOFS_copy2 = mapbak_BG1HOFS_copy2;
  BG2HOFS_copy2 = mapbak_BG2HOFS_copy2;
  BG1VOFS_copy2 = mapbak_BG1VOFS_copy2;
  BG2VOFS_copy2 = mapbak_BG2VOFS_copy2;
  WORD(TM_copy) = WORD(mapbak_TM);
  Attract_SetUpConclusionHDMA();
}

// Re-arms HDMA for the transition back to the normal game mode after the overworld map or
// attract cutscene. Sets up the M7A/M7D channel (register 0x42) with the shared HDMA table,
// re-enables the HDMA channel (HDMAEN_copy = 0x80), switches BGMODE back to Mode 1 + BG3
// priority (BGMODE_copy = 9), and re-enables core NMI updates.
void Attract_SetUpConclusionHDMA() {  // 8abc33
  HdmaSetup(0xABDDD, 0xABDDD, 0x42, (uint8)M7A, (uint8)M7D, 0);
  HDMAEN_copy = 0x80;
  BGMODE_copy = 9;
  nmi_disable_core_updates = 0;
}

// Overworld-map phase 7: Restores the full game state after closing the map.
// Resets palette mode, reinitialises tilesets, queues a CGRAM upload,
// resets map-state and subsubmodule counters, restores the game module
// (main_module_index = saved_module_for_menu, submodule_index = 32),
// resets VRAM upload offset, restores HDMA from the pre-map backup,
// and queues music/SFX to resume the area track.
void WorldMap_ExitMap() {  // 8abc54
  overworld_palette_aux_or_main = 0;
  hud_palette = 0;
  InitializeTilesets();
  flag_update_cgram_in_nmi++;
  BYTE(dung_draw_width_indicator) = 0;
  overworld_map_state = 0;
  subsubmodule_index = 0;
  main_module_index = saved_module_for_menu;
  submodule_index = 32;
  vram_upload_offset = 0;
  HDMAEN_copy = mapbak_HDMAEN;
  sound_effect_ambient = overworld_music[BYTE(overworld_screen_index)] >> 4;
  sound_effect_2 = 0x10;
  music_control = 0xf3;
}

// Initialises the Mode 7 HDMA and scroll registers for the overworld map.
// Zeroes all window/masking registers, then branches based on context:
//   module 20 (attract): simple static HDMA table, single HDMA channel.
//   submodule 10 (flute menu zoomed-out): fixed centre at (128, 200), extra HDMA channel,
//                                         timer_for_mode7_zoom = 33 for slow zoom.
//   normal map: calculates a perspective-corrected initial scroll position centred on Link
//               (x skewed by 5/2 factor relative to centre 0x80), timer = 12 for fast zoom.
// The zoom timer controls how fast the Mode 7 perspective matrix interpolates.
void WorldMap_SetUpHDMA() {  // 8abc96
  BG1HOFS_copy2 = 0x80;
  BG1VOFS_copy2 = 0xc8;
  M7Y_copy = 0x1c9;
  M7X_copy = 0x100;
  W12SEL_copy = 0;
  W34SEL_copy = 0;
  WOBJSEL_copy = 0;
  TMW_copy = 0;
  TSW_copy = 0;

  if (main_module_index == 20) {
    HdmaSetup(0xABDDD, 0xABDDD, 0x42, (uint8)M7A, (uint8)M7D, 0);
    HDMAEN_copy = 0xc0;
  } else if (submodule_index != 10) {
    byte_7E0635 = 4;
    timer_for_mode7_zoom = 12;
    overworld_map_flags = 1;
    BG1VOFS_copy2 = ((link_y_coord_spexit >> 4) - 0x48 & ~1);
    M7Y_copy = BG1VOFS_copy2 + 0x100;
    uint16 t0 = (link_x_coord_spexit >> 4) - 0x80;
    uint16 t1 = (uint16)(5 * (sign16(t0) ? -t0 : t0)) >> 1;
    uint16 t2 = sign16(t0) ? -t1 : t1;
    BG1HOFS_copy2 = t2 + 0x80 & ~1;
    OverworldMap_SetupHdma();
    HDMAEN_copy = 0xc0;
  } else {
    byte_7E0635 = 4;
    timer_for_mode7_zoom = 33;
    overworld_map_flags = 0;
    HdmaSetup(0xABDCF, 0xABDCF, 0x42, (uint8)M7A, (uint8)M7D, 10);
    HDMAEN_copy = 0xc0;
  }
}

// Fills the Mode 7 VRAM tile map (0x4000 bytes = 16384 words) with the byte value 0xef,
// which is the "off-screen / border" tile index used as the default backdrop on the map.
// Only the low byte of each word is set so the palette/attribute nybble is left at 0.
void WorldMap_FillTilemapWithEF() {  // 8abda5
  uint16 *dst = g_zenv.vram;
  for (int i = 0; i != 0x4000; i++)
    BYTE(dst[i]) = 0xef;
}

// Draws all map overlay sprites for the overworld map display:
//   Slot 0:    Player position dot (blinking every 0x10 frames via frame_counter & 0x10).
//   Slot 15:   Wandering bird sprite (from bird_travel arrays), animated every frame via
//              kOverworldMap_Table4 and incremented once per full frame (frame_counter == 0).
//   Slots 8-14: Crystal/pendant achievement icons for the 7 collectibles, drawn from
//               kOwMapCrystal0..6 coordinate tables keyed by savegame_map_icons_indicator.
//               If the collectible is already obtained (OverworldMap_CheckForPendant/Crystal),
//               the slot is skipped. Icons use either the animated tile
//               (kOwMap_tab2[frame_counter >> 3 & 3]) or a fixed tile depending on the
//               tab high byte (0 = animated, 100 = always blinking with offset, else static).
// Link's spexit coordinates are used as a scratchpad for the projection calculation and
// are restored from local backups after all icons are drawn.
void WorldMap_HandleSprites() {  // 8abf66
  Point16U pt;

  if (frame_counter & 0x10 && WorldMap_CalculateOamCoordinates(&pt))
    WorldMap_AddSprite(0, 2, 0x3e, 0, pt.x - 4, pt.y - 4);

  uint16 ybak = link_y_coord_spexit;
  uint16 xbak = link_x_coord_spexit;

  int k = 15;
  if (BYTE(overworld_screen_index) < 0x40 && (bird_travel_x_lo[k] | bird_travel_x_hi[k] | bird_travel_y_lo[k] | bird_travel_y_hi[k])) {
    if (!frame_counter)
      birdtravel_var1[k]++;
    link_x_coord_spexit = bird_travel_x_hi[k] << 8 | bird_travel_x_lo[k];
    link_y_coord_spexit = bird_travel_y_hi[k] << 8 | bird_travel_y_lo[k];
    if (WorldMap_CalculateOamCoordinates(&pt))
      WorldMap_AddSprite(15, 2, kOverworldMap_Table4[frame_counter >> 1 & 3], 0x6a, pt.x, pt.y);
  }

  if (save_ow_event_info[0x5b] & 0x20 || (((savegame_map_icons_indicator >= 6) ^ is_in_dark_world) & 1))
    goto out;

  k = savegame_map_icons_indicator;

  if (!OverworldMap_CheckForPendant(0) && !OverworldMap_CheckForCrystal(0) && !sign16(kOwMapCrystal0_x[k])) {
    link_x_coord_spexit = kOwMapCrystal0_x[k];
    link_y_coord_spexit = kOwMapCrystal0_y[k];
    uint8 t = kOwMapCrystal0_tab[k] >> 8;
    if (t != 0) {
      if (t != 100 && frame_counter & 0x10)
        goto endif_crystal0;
      link_x_coord_spexit -= 4, link_y_coord_spexit -= 4;
    }
    if (WorldMap_CalculateOamCoordinates(&pt)) {
      uint16 info = kOwMapCrystal0_tab[k];
      uint8 ext = 2;
      if (!(info >> 8))
        info = kOwMap_tab2[frame_counter >> 3 & 3] << 8 | 0x32, ext = 0;
      WorldMap_AddSprite(14, ext, (uint8)info, (uint8)(info >> 8), pt.x, pt.y);
    }
  endif_crystal0:;
  }

  if (!OverworldMap_CheckForPendant(1) && !OverworldMap_CheckForCrystal(1) && !sign16(kOwMapCrystal1_x[k])) {
    link_x_coord_spexit = kOwMapCrystal1_x[k];
    link_y_coord_spexit = kOwMapCrystal1_y[k];
    uint8 t = kOwMapCrystal1_tab[k] >> 8;
    if (t != 0) {
      if (t != 100 && frame_counter & 0x10)
        goto endif_crystal1;
      link_x_coord_spexit -= 4, link_y_coord_spexit -= 4;
    }
    if (WorldMap_CalculateOamCoordinates(&pt)) {
      uint16 info = kOwMapCrystal1_tab[k];
      uint8 ext = 2;
      if (!(info >> 8))
        info = kOwMap_tab2[frame_counter >> 3 & 3] << 8 | 0x32, ext = 0;
      WorldMap_AddSprite(13, ext, (uint8)info, (uint8)(info >> 8), pt.x, pt.y);
    }
  endif_crystal1:;
  }

  if (!OverworldMap_CheckForPendant(2) && !OverworldMap_CheckForCrystal(2) && !sign16(kOwMapCrystal2_x[k])) {
    link_x_coord_spexit = kOwMapCrystal2_x[k];
    link_y_coord_spexit = kOwMapCrystal2_y[k];
    uint8 t = kOwMapCrystal2_tab[k] >> 8;
    if (t != 0) {
      if (t != 100 && frame_counter & 0x10)
        goto endif_crystal2;
      link_x_coord_spexit -= 4, link_y_coord_spexit -= 4;
    }
    if (WorldMap_CalculateOamCoordinates(&pt)) {
      uint16 info = kOwMapCrystal2_tab[k];
      uint8 ext = 2;
      if (!(info >> 8))
        info = kOwMap_tab2[frame_counter >> 3 & 3] << 8 | 0x32, ext = 0;
      WorldMap_AddSprite(12, ext, (uint8)info, (uint8)(info >> 8), pt.x, pt.y);
    }
  endif_crystal2:;
  }

  if (!OverworldMap_CheckForCrystal(3) && !sign16(kOwMapCrystal3_x[k])) {
    link_x_coord_spexit = kOwMapCrystal3_x[k];
    link_y_coord_spexit = kOwMapCrystal3_y[k];
    uint8 t = kOwMapCrystal3_tab[k] >> 8;
    if (t != 0) {
      if (t != 100 && frame_counter & 0x10)
        goto endif_crystal3;
      link_x_coord_spexit -= 4, link_y_coord_spexit -= 4;
    }
    if (WorldMap_CalculateOamCoordinates(&pt)) {
      uint16 info = kOwMapCrystal3_tab[k];
      uint8 ext = 2;
      if (!(info >> 8))
        info = kOwMap_tab2[frame_counter >> 3 & 3] << 8 | 0x32, ext = 0;
      WorldMap_AddSprite(11, ext, (uint8)info, (uint8)(info >> 8), pt.x, pt.y);
    }
  endif_crystal3:;
  }

  if (!OverworldMap_CheckForCrystal(4) && !sign16(kOwMapCrystal4_x[k])) {
    link_x_coord_spexit = kOwMapCrystal4_x[k];
    link_y_coord_spexit = kOwMapCrystal4_y[k];
    uint8 t = kOwMapCrystal4_tab[k] >> 8;
    if (t != 0) {
      if (t != 100 && frame_counter & 0x10)
        goto endif_crystal4;
      link_x_coord_spexit -= 4, link_y_coord_spexit -= 4;
    }
    if (WorldMap_CalculateOamCoordinates(&pt)) {
      uint16 info = kOwMapCrystal4_tab[k];
      uint8 ext = 2;
      if (!(info >> 8))
        info = kOwMap_tab2[frame_counter >> 3 & 3] << 8 | 0x32, ext = 0;
      WorldMap_AddSprite(10, ext, (uint8)info, (uint8)(info >> 8), pt.x, pt.y);
    }
  endif_crystal4:;
  }

  if (!OverworldMap_CheckForCrystal(5) && !sign16(kOwMapCrystal5_x[k])) {
    link_x_coord_spexit = kOwMapCrystal5_x[k];
    link_y_coord_spexit = kOwMapCrystal5_y[k];
    uint8 t = kOwMapCrystal5_tab[k] >> 8;
    if (t != 0) {
      if (t != 100 && frame_counter & 0x10)
        goto endif_crystal5;
      link_x_coord_spexit -= 4, link_y_coord_spexit -= 4;
    }
    if (WorldMap_CalculateOamCoordinates(&pt)) {
      uint16 info = kOwMapCrystal5_tab[k];
      uint8 ext = 2;
      if (!(info >> 8))
        info = kOwMap_tab2[frame_counter >> 3 & 3] << 8 | 0x32, ext = 0;
      WorldMap_AddSprite(9, ext, (uint8)info, (uint8)(info >> 8), pt.x, pt.y);
    }
  endif_crystal5:;
  }

  if (!OverworldMap_CheckForCrystal(6) && !sign16(kOwMapCrystal6_x[k])) {
    link_x_coord_spexit = kOwMapCrystal6_x[k];
    link_y_coord_spexit = kOwMapCrystal6_y[k];
    uint8 t = kOwMapCrystal6_tab[k] >> 8;
    if (t != 0) {
      if (t != 100 && frame_counter & 0x10)
        goto endif_crystal6;
      link_x_coord_spexit -= 4, link_y_coord_spexit -= 4;
    }
    if (WorldMap_CalculateOamCoordinates(&pt)) {
      uint16 info = kOwMapCrystal6_tab[k];
      uint8 ext = 2;
      if (!(info >> 8))
        info = kOwMap_tab2[frame_counter >> 3 & 3] << 8 | 0x32, ext = 0;
      WorldMap_AddSprite(8, ext, (uint8)info, (uint8)(info >> 8), pt.x, pt.y);
    }
  endif_crystal6:;
  }

out:
  link_x_coord_spexit = xbak;
  link_y_coord_spexit = ybak;
}

// Projects a world-space position (link_x_coord_spexit, link_y_coord_spexit) into
// Mode 7 screen coordinates and writes the result to *pt. Returns false if the
// position is off-screen.
//
// zoomed-out (overworld_map_flags == 0): Full-world perspective using the cosine lookup
//   kOverworldMap_tab1[333]. The y-coordinate is inverted and offset by M7Y_copy,
//   then scaled by 13/16 (yval = 13 * t0 >> 4). The x-coordinate is mirror-folded
//   around 0x80 and scaled proportionally to the perspective-projected y height.
//
// zoomed-in (overworld_map_flags != 0): Local area perspective. Both axes are offset
//   by M7Y_copy / 0x800, scaled by 37/16 then looked up in the cosine table, then
//   further scaled by 84/256 + 178 (r4). Off-screen positions (t0 >= 0x100 or
//   t1 >= 333) return false. Also checks the kFeatures0_ExtendScreen64 flag for
//   wide-screen boundary widening.
static bool WorldMap_CalculateOamCoordinates(Point16U *pt) {  // 8ac39f
  if (overworld_map_flags == 0) {
    int j = -(link_y_coord_spexit >> 4) + M7Y_copy + (link_y_coord_spexit >> 3 & 1) - 0xc0;
    uint8 t0 = kOverworldMap_tab1[j];
    uint8 yval = 13 * t0 >> 4;

    uint8 at = link_x_coord_spexit >> 4;
    bool below = at < 0x80;
    at -= 0x80;
    if (sign8(at)) at = ~at;

    uint8 t1 = ((yval < 224 ? yval : 0) * 0x54 >> 8) + 0xb2;
    uint8 t2 = at * t1 >> 8;
    uint8 t3 = (below) ? 0x80 - t2 : t2 + 0x80;

    pt->x = t3 - BG1HOFS_copy2 + 0x80;
    pt->y = yval + 12;
    return true;
  } else {
    uint16 t0 = -(link_y_coord_spexit >> 4) + M7Y_copy - 0x80;
    if (t0 >= 0x100)
      return false;
    uint16 t1 = t0 * 37 >> 4;
    if (t1 >= 333)
      return false;
    uint8 yval = kOverworldMap_tab1[t1];
    uint16 t2 = link_x_coord_spexit;
    bool below = t2 < 0x7F8;
    t2 -= 0x7f8;
    if (sign16(t2))
      t2 = -t2;
    uint8 t3 = yval < 226 ? yval : 0;
    uint8 t4 = (t3 * 84 >> 8) + 178;  // r0
    uint8 t5 = (uint8)t2 * t4 >> 8; // r1
    uint16 t6 = (uint8)(t2 >> 8) * t4 + t5;
    uint16 t7 = (below) ? 0x800 - t6 : t6 + 0x800;
    bool below2 = t7 < 0x800;
    t7 -= 0x800;
    uint16 t8 = below2 ? -t7 : t7;
    uint8 t9 = (uint8)t8 * 45 >> 8;
    uint16 t10 = ((t8 >> 8) * 45) + t9;
    uint16 t11 = below2 ? 0x80 - t10 : t10 + 0x80;
    uint16 xval = t11 - BG1HOFS_copy2;
    int xt = enhanced_features0 & kFeatures0_ExtendScreen64 ? 0x48 : 0;
    if ((uint16)(xval + 0x80 + xt) >= (0x100 + xt * 2))
      return false;
    pt->x = xval + 0x81;
    pt->y = yval + 16;
    return true;
  }
}

// Writes one OAM entry for a map overlay icon.
// spr: OAM slot index (0-15). big: size bits passed to SetOamPlain (0=8x8, 2=16x16, etc.).
// flags: SNES OAM attribute byte (palette, priority, flip). ch: tile character index.
// x, y: projected screen coordinates from WorldMap_CalculateOamCoordinates.
// If the icon uses the animated "achievement" tile (ch == 100, frame off), replaces it
// with the static kOverworldMapData entry for that slot and treats it as an 8x8 sprite.
// For wide-screen mode (kFeatures0_ExtendScreen64), the x high bit feeds the big-sprite flag.
// Subtracts 4 from x/y for non-achievement icons to centre the icon on the projection point.
static void WorldMap_AddSprite(int spr, uint8 big, uint8 flags, uint8 ch, uint16 x, uint16 y) {  // 8ac51c
  if (!(frame_counter & 0x10) && ch == 100) {
    assert(spr >= 8);
    ch = kOverworldMapData[spr - 8];
    flags = 0x32;
    big = 0;
  } else {
    x -= 4;
    y -= 4;
  }
  if (enhanced_features0 & kFeatures0_ExtendScreen64)
    big |= (x >> 8 & 1);
  SetOamPlain(&oam_buf[spr], x, y, ch, flags, big);
}

// Returns true if pendant k has already been collected and should be suppressed from the map.
// Only active when savegame_map_icons_indicator == 3 (pendant-collection phase);
// tests kPendantBitMask[k] in link_which_pendants.
bool OverworldMap_CheckForPendant(int k) {  // 8ac5a9
  return (savegame_map_icons_indicator == 3) && (link_which_pendants & kPendantBitMask[k]) != 0;
}

// Returns true if crystal k has already been collected and should be suppressed from the map.
// Only active when savegame_map_icons_indicator == 7 (crystal-collection phase);
// tests kCrystalBitMask[k] in link_has_crystals.
bool OverworldMap_CheckForCrystal(int k) {  // 8ac5c6
  return (savegame_map_icons_indicator == 7) && (link_has_crystals & kCrystalBitMask[k]) != 0;
}

// Module 0x0E, submodule 0x03: Dungeon map overlay dispatcher.
// Delegates each frame to kDungMapSubmodules[overworld_map_state] — a 9-phase state machine:
//   0 = DungMap_Backup            — fade out, back up graphics state.
//   1 = Module0E_03_01_DrawMap    — iterative drawing init (5 sub-phases via dungmap_init_state).
//   2 = DungMap_LightenUpMap      — fade the dungeon map in.
//   3 = DungeonMap_DrawRoomMarkers — compute OAM positions for Link and boss icons.
//   4 = DungeonMap_HandleInputAndSprites — player control + sprite draw each frame.
//   5 = (unused filler)
//   6 = DungMap_FadeMapToBlack    — fade out to restore game graphics.
//   7 = DungeonMap_RecoverGFX     — rebuild tilesets, re-upload room quadrants.
//   8 = DungMap_RestoreOld        — fade back in and return to game module.
void Module0E_03_DungeonMap() {  // 8ae0b0
  kDungMapSubmodules[overworld_map_state]();
}

// Dungeon map phase 1: Multi-step drawing initialiser dispatched by dungmap_init_state:
//   0 = Module0E_03_01_00_PrepMapGraphics  — swap tilesets, load map palette, init NMI.
//   1 = Module0E_03_01_01_DrawLEVEL       — write FLOOR/LEVEL text tiles to VRAM upload buf.
//   2 = Module0E_03_01_02_DrawFloorsBackdrop — draw floor-backdrop VRAM upload entries.
//   3 = Module0E_03_01_03_DrawRooms       — draw all room tiles via messaging_buf.
// Each sub-phase queues one NMI upload and increments dungmap_init_state.
void Module0E_03_01_DrawMap() {  // 8ae0dc
  kDungMapInit[dungmap_init_state]();
}

// Dungeon map drawing init sub-phase 0: Swaps in the dungeon-map tile themes,
// backs up the current main/aux/sprite tile indices, TM, and TS,
// then loads the map-specific tile theme (main = 32, sprite = 0x80 | dungeon_index,
// aux = 64), sets TM_copy to show BG1 + BG3 + OBJ (0x16), erases the tile maps,
// re-initialises tilesets, loads dungeon-map BG and sprite palettes, loads the HUD palette,
// queues a CGRAM upload, sets NMI subroutine 9 (tilemap upload mode), and signals
// the NMI loop to suppress core updates for 9 frames.
void Module0E_03_01_00_PrepMapGraphics() {  // 8ae0e4
  uint8 hdmaen_bak = HDMAEN_copy;
  HDMAEN_copy = 0;
  mapbak_main_tile_theme_index = main_tile_theme_index;
  mapbak_sprite_graphics_index = sprite_graphics_index;
  mapbak_aux_tile_theme_index = aux_tile_theme_index;
  mapbak_TM = TM_copy;
  mapbak_TS = TS_copy;
  main_tile_theme_index = 32;
  sprite_graphics_index = 0x80 | BYTE(cur_palace_index_x2) >> 1;
  aux_tile_theme_index = 64;
  TM_copy = 0x16;
  TS_copy = 1;
  EraseTileMaps_dungeonmap();
  InitializeTilesets();
  overworld_palette_aux_or_main = 0x200;
  Palette_Load_DungeonMapBG();
  Palette_Load_DungeonMapSprite();
  hud_palette = 1;
  Palette_Load_HUD();
  LoadActualGearPalettes();
  flag_update_cgram_in_nmi++;
  dungmap_init_state++;
  HDMAEN_copy = hdmaen_bak;
  nmi_load_bg_from_vram = 9;
  nmi_disable_core_updates = 9;
}

// Dungeon map drawing init sub-phase 1: Writes the "FLOOR x" (or "MAP") header tiles
// to the VRAM upload buffer so the NMI loop will upload them to VRAM.
// kDungMap_Tab0 maps the current dungeon to its floor-label table index.
// kDungMap_Tab1/Tab2 provide the tile IDs for the two characters of the floor number.
// kDungMap_Tab3/Tab4 provide the tile sequence for the word "LEVEL" / "FLOOR" itself.
// The buffer is terminated with dst[32] = 0xff to end the NMI DMA chain.
void Module0E_03_01_01_DrawLEVEL() {  // 8ae1a4
  // Display FLOOR instead of MAP
  int i = kDungMap_Tab0[cur_palace_index_x2 >> 1] >> 1;
  if (i >= 0) {
    uint8 *dst = (uint8 *)&vram_upload_data[0];
    dst[32] = 0xff;
    WORD(dst[14   ]) = kDungMap_Tab1[i];
    WORD(dst[14+16]) = kDungMap_Tab2[i];
    for (int i = 13; i >= 0; i--) {
      dst[i] = kDungMap_Tab3[i];
      dst[i+16] = kDungMap_Tab4[i];
    }
    nmi_load_bg_from_vram = 1;
  }
  dungmap_init_state++;
}

// Dungeon map drawing init sub-phase 2: Builds the VRAM upload data for the floor
// backdrop (the grey box rows behind the room grid).
// kDungMap_Tab5 encodes the floor layout descriptor for each dungeon: the high nybble
// is the number of above-ground floors, the low nybble is the number of below-ground
// floors; bit 8 indicates a special two-column layout.
// kDungMap_Tab6 provides the first 21 entries for the two-column special case.
// kDungMap_Tab7 maps the floor count to a starting VRAM address offset.
// kDungMap_Tab8 provides tile row descriptors for each floor strip.
// After building the floor strips, DungeonMap_BuildFloorListBoxes appends
// the floor-number boxes to the upload buffer. The buffer is terminated with 0xff.
void Module0E_03_01_02_DrawFloorsBackdrop() {  // 8ae1f3
  int offs = 0;
  uint16 t5 = kDungMap_Tab5[cur_palace_index_x2 >> 1];
  if (t5 & 0x100) {
    for (int i = 0; i < 21; i++)
      vram_upload_data[offs++] = kDungMap_Tab6[i];
    uint16 t = 0x1123;
    for (int i = 0; i < 16; i++, t += 0x20, offs += 3) {
      vram_upload_data[offs + 0] = swap16(t);
      vram_upload_data[offs + 1] = 0xE40;
      vram_upload_data[offs + 2] = 0x1B2E;
    }
  }
  int t7 = kDungMap_Tab7[(uint8)t5 >= 0x50 ? (((uint8)t5 >> 4) - 4) : (t5 & 0xf) >= 5 ? (t5 & 0xf) : 0], t7_org = t7;
  int j = 0;
  do {
    vram_upload_data[offs++] = swap16(t7);
    vram_upload_data[offs++] = 0xe40;
    vram_upload_data[offs++] = kDungMap_Tab8[j] + (t5 & 0x200 ? 0x400 : 0);
    j += (j != 6);
  } while (t7 += 0x20, t7 < 0x1360);
  vram_upload_offset = offs * 2;
  DungeonMap_BuildFloorListBoxes(t5, t7_org);
  ((uint8 *)vram_upload_data)[vram_upload_offset] = 0xff;
  dungmap_init_state++;
  nmi_load_bg_from_vram = 1;
}

// Appends the floor-number label boxes to the VRAM upload buffer that was started by
// Module0E_03_01_02_DrawFloorsBackdrop.
// t5: floor layout descriptor (high nybble = above-floor count, low nybble = below-floor count).
// r14: starting VRAM destination address offset from Tab7.
// Generates n = (t5 & 0xf) + (t5 >> 4) label box entries, each consisting of a VRAM address
// word, an attribute word (0x700), and kDungMap_Tab9 tile data for the 4 tiles that form one
// half of the two-tile box row; the second half is drawn by jumping back to loop2.
// Updates vram_upload_offset after appending all boxes.
void DungeonMap_BuildFloorListBoxes(uint8 t5, uint16 r14) {  // 8ae2f5
  int n = (t5 & 0xf) + (t5 >> 4);
  r14 -= 0x40 - 2;
  r14 += (t5 & 0xf) * 0x40;
  int offs = vram_upload_offset >> 1;
  int i = 0;
  do {
    int x = 0;
loop2:
    vram_upload_data[offs++] = swap16(r14);
    vram_upload_data[offs++] = 0x700;
    do {
      vram_upload_data[offs++] = kDungMap_Tab9[x++];
      if (x == 4) {
        r14 += 0x20;
        goto loop2;
      }
    } while (x != 8);

    r14 -= 0x40 + 0x20;
  } while (++i < n);
  vram_upload_offset = offs * 2;
}

// Dungeon map drawing init sub-phase 3: Draws all room tiles into messaging_buf for
// two adjacent floor planes (the visible floor and the floor above/below for scrolling).
// Sets dungmap_cur_floor to the correct floor index (accounting for whether the current
// floor is the topmost above-ground floor). Draws border frame, floor-number labels, and
// the 5×5 room grid for both planes (offset by 0x300 VRAM words for the second plane).
// Sets nmi_subroutine_index = 8 to trigger the NMI tilemap upload, then advances to phase 2.
void Module0E_03_01_03_DrawRooms() {  // 8ae384
  dungmap_var2 = 0;
  dungmap_idx = 0;
  uint8 t = -(kDungMap_Tab5[cur_palace_index_x2 >> 1] & 0xf);
  if (WORD(dung_cur_floor) != t) {
    dungmap_cur_floor = dung_cur_floor;
  } else {
    dungmap_cur_floor = WORD(dung_cur_floor) + 1;
    dungmap_idx += 2;
  }
  DungeonMap_DrawFloorNumbersByRoom(0, ~0x1000);
  DungeonMap_DrawBorderForRooms(0, ~0x1000);
  DungeonMap_DrawDungeonLayout(0);
  BYTE(dungmap_cur_floor)--;
  DungeonMap_DrawFloorNumbersByRoom(0x300, ~0x1000);
  DungeonMap_DrawBorderForRooms(0x300, ~0x1000);
  DungeonMap_DrawDungeonLayout(0x300);
  dungmap_cur_floor++;
  WORD(g_ram[6]) = 0;
  WORD(g_ram[10]) = 0;
  nmi_subroutine_index = 8;
  BYTE(nmi_load_target_addr) = 0x22;
  dungmap_init_state++;
}

// Draws the rectangular border frame around the 5×5 room grid into messaging_buf.
// pd: VRAM plane offset (0 or 0x300) for the current vs adjacent floor plane.
// mask: AND mask applied to each tile word (pass ~0x1000 to strip the "map-owned" flag).
// Uses kDungMap_Tab10/11 for the four corner tiles, kDungMap_Tab12/13 for the two
// horizontal edges (20 tiles each), and kDungMap_Tab14/15 for the two vertical edges
// (each written at intervals of 0x40 VRAM words, i.e. one row apart, 10 rows total).
void DungeonMap_DrawBorderForRooms(uint16 pd, uint16 mask) {  // 8ae449
  for (int i = 0; i != 4; i++)
    messaging_buf[((kDungMap_Tab10[i] + pd) & 0xfff) >> 1] = kDungMap_Tab11[i] & mask;
  for (int i = 0; i != 2; i++) {
    int r4 = kDungMap_Tab12[i] + pd;
    for (int j = 0; j != 20; j+=2)
      messaging_buf[((r4 + j) & 0xfff) >> 1] = kDungMap_Tab13[i] & mask;
  }

  for (int i = 0; i != 2; i++) {
    int r4 = kDungMap_Tab14[i] + pd;
    for (int j = 0; j != 0x280; j+=0x40)
      messaging_buf[((r4 + j) & 0xfff) >> 1] = kDungMap_Tab15[i] & mask;
  }
}

// Writes the floor-number digit tiles into messaging_buf for the current floor plane.
// pd: plane offset (0 or 0x300). r8: AND mask to clear the "map-owned" flag if needed.
// First fills the entire floor-label column (0xDE to 0x39E, step 0x40) with blank (0xf00).
// Then looks up the two character tiles for the current floor in kDungMap_Tab16 and writes
// them at the end of the column (0x35E offset). Negative floor numbers use the mirror-image
// tile (0x1F1C/0x1F1D) rather than kDungMap_Tab16 entries.
void DungeonMap_DrawFloorNumbersByRoom(uint16 pd, uint16 r8) {  // 8ae4f9
  uint16 p = 0xDE;
  do {
    int t = ((p + pd) & 0xfff) >> 1;
    messaging_buf[t] = 0xf00;
    messaging_buf[t+1] = 0xf00;
  } while (p += 0x40, p != 0x39e);
  int t = ((0x35e + pd) & 0xfff) >> 1;
  uint16 q1 = (dungmap_cur_floor & 0x80) ? 0x1F1C : kDungMap_Tab16[dungmap_cur_floor & 0xf];
  uint16 q2 = (dungmap_cur_floor & 0x80) ? kDungMap_Tab16[(uint8)~dungmap_cur_floor] : 0x1F1D;
  messaging_buf[t+0] = q1 & r8;
  messaging_buf[t+1] = q2 & r8;
}

// Draws the 5-row room grid for one floor plane.
// pd: plane offset (0 for current floor, 0x300 for adjacent floor).
// Calls DungeonMap_DrawSingleRowOfRooms for each of the 5 rows, passing the
// VRAM buffer index computed as (292 + 128 * row + pd) & 0xfff >> 1.
void DungeonMap_DrawDungeonLayout(int pd) {  // 8ae579
  for (int i = 0; i < 5; i++)
    DungeonMap_DrawSingleRowOfRooms(i, ((292 + 128 * i + pd) & 0xfff) >> 1);
}

// Draws one 5-column row (row i) of the dungeon room grid into messaging_buf.
// arg_x: starting index into messaging_buf for the first column.
// For each room cell (j = 0..4):
//   Looks up the room ID at position [floor][row*5+col] in the floor layout table.
//   If room ID is 0xF (empty cell), uses sprite-tile 0x51 (blank).
//   Otherwise reads save_dung_info[room_id] & 0xf (the visited/opened door bits)
//   and kDungMap_Tab23[yv * 4 + 0..3] for the 4 sub-tile words forming the 2×2 cell.
//   For each sub-tile: if the room is unvisited and the player lacks the dungeon map,
//   the tile is replaced with 0 (hidden); if the map is owned but the door bit is unset,
//   shows a passage tile (0x400); otherwise shows the full room tile.
//   The two columns of tiles for each cell are written to messaging_buf at arg_x
//   and arg_x+1 (upper row) and arg_x+32 and arg_x+33 (lower row).
void DungeonMap_DrawSingleRowOfRooms(int i, int arg_x) {  // 8ae5bc
  uint16 t5 = kDungMap_Tab5[cur_palace_index_x2 >> 1];
  int dungmask = kUpperBitmasks[cur_palace_index_x2 >> 1];

  for (int j = 0; j < 5; j++, arg_x += 2) {
    int r14 = (uint8)(dungmap_cur_floor + (t5 & 0xf));
    const uint8 *curp = GetDungmapFloorLayout();
    uint8 v = curp[r14 * 25 + i * 5 + j];
    uint16 yv, av;
    if (v == 0xf) {
      yv = 0x51;
    } else {
      r14 = save_dung_info[v] & 0xf;
      int k = 0, count = 0;
      for(; curp[k] != v; k++)
        count += (curp[k] != 0xf);
      yv = GetOtherDungmapInfo(count);
    }

    uint16 r12 = kDungMap_Tab23[yv * 4 + 0], r12_org = r12;
    if (r12 != 0xB00 && (r14 & 8) == 0) {
      if (!(r12 & 0x1000)) {
        r12 = 0x400;
      } else if (link_dungeon_map & dungmask) {
        av = (r12 & ~0x1c00) | 0xc00;
        goto write_3;
      } else {
        r12 = 0;
      }
    } else {
      r12 = 0;
    }
    av = ((link_dungeon_map & dungmask) || (r14 & 8)) ? r12 + r12_org : 0xb00;
  write_3:
    messaging_buf[arg_x] = av;

    r12 = kDungMap_Tab23[yv * 4 + 1], r12_org = r12;
    if (r12 != 0xB00 && (r14 & 4) == 0) {
      if (!(r12 & 0x1000)) {
        r12 = 0x400;
      } else if (link_dungeon_map & dungmask) {
        av = (r12 & ~0x1c00) | 0xc00;
        goto write_4;
      } else {
        r12 = 0;
      }
    } else {
      r12 = 0;
    }
    av = ((link_dungeon_map & dungmask) || (r14 & 4)) ? r12 + r12_org : 0xb00;
  write_4:
    messaging_buf[arg_x + 1] = av;

    r12 = kDungMap_Tab23[yv * 4 + 2], r12_org = r12;
    if (r12 != 0xB00 && (r14 & 2) == 0) {
      if (!(r12 & 0x1000)) {
        r12 = 0x400;
      } else if (link_dungeon_map & dungmask) {
        av = (r12 & ~0x1c00) | 0xc00;
        goto write_5;
      } else {
        r12 = 0;
      }
    } else {
      r12 = 0;
    }
    av = ((link_dungeon_map & dungmask) || (r14 & 2)) ? r12 + r12_org : 0xb00;
  write_5:
    messaging_buf[arg_x + 32] = av;

    r12 = kDungMap_Tab23[yv * 4 + 3], r12_org = r12;
    if (r12 != 0xB00 && (r14 & 1) == 0) {
      if (!(r12 & 0x1000)) {
        r12 = 0x400;
      } else if (link_dungeon_map & dungmask) {
        av = (r12 & ~0x1c00) | 0xc00;
        goto write_6;
      } else {
        r12 = 0;
      }
    } else {
      r12 = 0;
    }
    av = ((link_dungeon_map & dungmask) || (r14 & 1)) ? r12 + r12_org : 0xb00;
  write_6:
    messaging_buf[arg_x + 33] = av;
  }
}

// Calculates the OAM pixel coordinates for the Link-position indicator and the boss icon,
// accounting for Link's sub-tile position within the current room.
// Also remaps certain special room indices (kDungMap_Tab21/22) to their canonical map rooms.
// Scans the floor layout to find the cell that contains the current room index, then converts
// the cell column and row to pixel coordinates: dungmap_var3 (x), dungmap_var5 (y for blink),
// dungmap_var6 (y base). Sub-tile precision is added from link_x/y_coord bits 5-9.
// Separately scans the boss-room floor (kDungMap_Tab28) to find the boss room cell and
// computes dungmap_var7/8 for the boss icon OAM position. Advances to overworld_map_state 2
// and resets INIDISP_copy and dungmap_init_state to begin the fade-in + interactive phase.
void DungeonMap_DrawRoomMarkers() {  // 8ae823
  int dung = cur_palace_index_x2 >> 1;
  uint8 t5 = (kDungMap_Tab5[dung] & 0xf);
  uint8 floor1 = t5 + dung_cur_floor;

  uint16 room = dungeon_room_index;
  for (int i = 0; i != 3; i++) {
    if (room == kDungMap_Tab21[i])
      room = kDungMap_Tab22[i];
  }
  const uint8 *roomp = GetDungmapFloorLayout();
  const uint8 *curp = &roomp[floor1 * 25];
  int i;

  uint8 xcoord = 0, ycoord = 0;
  for(i = 0; i < 25 && *curp++ != (uint8)room; i++) {
    if (xcoord < 64)
      xcoord += 16;
    else
      xcoord = 0, ycoord += 16;
  }
  dungmap_var3 = xcoord + 0x90;
  dungmap_var3 += (link_x_coord & 0x1e0) >> 5;

  dungmap_var6 = ycoord;

  dungmap_var5 = ycoord + kDungMap_Tab24[dungmap_idx >> 1];
  dungmap_var5 += (link_y_coord & 0x1e0) >> 5;

  uint8 floor2 = t5 + kDungMap_Tab28[dung];
  curp = &roomp[floor2 * 25];

  dungmap_var8 = dungmap_var7 = 0x40;

  uint8 lookfor = kDungMap_Tab25[dung];
  for (int j = 24; j >= 0; j--) {
    if (curp[j] != 0xf && curp[j] == lookfor)
      break;
    if ((int16)(dungmap_var7 -= 0x10) < 0) {
      dungmap_var7 = 0x40;
      BYTE(dungmap_var8) -= 0x10;
    }
  }

  int8 floor3 = dungmap_cur_floor - kDungMap_Tab28[dung];
  dungmap_var8 += 0x60 * floor3;
  dungmap_var8 += kDungMap_Tab24[0];
  overworld_map_state++;
  INIDISP_copy = 0;
  dungmap_init_state = 0;
}

// Dungeon map interactive phase: processes one frame of player input and then
// draws all dungeon map OAM sprites.
void DungeonMap_HandleInputAndSprites() {  // 8ae954
  DungeonMap_HandleInput();
  DungeonMap_DrawSprites();
}

// Returns true when the player presses the button that should close the dungeon map.
// Mirrors DidPressButtonForMap: Select if a Y-item is equipped, X otherwise.
static inline bool WantExitDungeonMap() {
  if (hud_cur_item_x != 0)
    return filtered_joypad_H & 0x20;  // Select
  else
    return filtered_joypad_L & 0x40;  // X
}

// Dungeon map input: If the exit button is pressed, advances overworld_map_state by 2
// (skipping state 5 placeholder and going directly to the fade-out/restore phase)
// and resets dungmap_init_state. Otherwise, delegates to DungeonMap_HandleMovementInput
// for floor-scroll input handling.
void DungeonMap_HandleInput() {  // 8ae95b
  if (WantExitDungeonMap()) {
    overworld_map_state += 2;
    dungmap_init_state = 0;
  } else {
    DungeonMap_HandleMovementInput();
  }
}

// Dungeon map movement input: Checks for floor-switch D-pad input and,
// if a floor-scroll animation is in progress (dungmap_var2 != 0), advances it.
void DungeonMap_HandleMovementInput() {  // 8ae979
  DungeonMap_HandleFloorSelect();
  if (dungmap_var2)
    DungeonMap_ScrollFloors();
}

// Dungeon map: Handles D-pad Up/Down input to switch between floors when the dungeon
// has 3 or more floors total and no scroll animation is already active.
// D-pad Down increments dungmap_cur_floor (scroll up on screen: higher floor number),
// D-pad Up decrements it. The floor is clamped to [-(low floors)...high_floors - 1].
// After updating the floor, redraws the floor numbers, border, and room grid for the
// new plane into messaging_buf, sets dungmap_var4 as the BG2VOFS scroll target,
// queues an NMI tilemap upload (nmi_subroutine_index = 8), and sets dungmap_var2 = 1
// to begin the scroll animation.
void DungeonMap_HandleFloorSelect() {  // 8ae986
  uint8 r2 = (kDungMap_Tab5[cur_palace_index_x2 >> 1] >> 4 & 0xf);
  uint8 r3 = (kDungMap_Tab5[cur_palace_index_x2 >> 1] & 0xf);
  if (r2 + r3 < 3 || dungmap_var2 || !(joypad1H_last & 0xc))
    return;
  dungmap_cur_floor &= 0xff;
  uint16 r6 = WORD(g_ram[6]);
  if (joypad1H_last & 8) {
    if (r2 - 1 == dungmap_cur_floor)
      return;
    dungmap_cur_floor++;
    r6 = (r6 - 0x300) & 0xfff;
  } else {
    if ((uint8)(-r3 + 1) == dungmap_cur_floor)
      return;
    dungmap_cur_floor -= 2;
    r6 = (r6 + 0x600) & 0xfff;
  }
  DungeonMap_DrawFloorNumbersByRoom(r6, ~0x1000);
  DungeonMap_DrawBorderForRooms(r6, ~0x1000);
  DungeonMap_DrawDungeonLayout(r6);
  dungmap_var2++;
  WORD(g_ram[10]) = joypad1H_last;
  int x = joypad1H_last >> 3 & 1;
  dungmap_var4 = BG2VOFS_copy2 + kDungMap_Tab26[x];
  if (!x) {
    r6 = (r6 - 0x300) & 0xfff;
    dungmap_cur_floor++;
  }
  WORD(g_ram[6]) = r6;
  nmi_subroutine_index = 8;
}

// Animates the floor-switch scroll: increments BG2VOFS_copy2 toward dungmap_var4
// using the signed delta from kDungMap_Tab40 (indexed by the scroll direction bit).
// Also updates the Link and boss icon Y positions (dungmap_var5, dungmap_var8)
// by the same per-frame increment (kDungMap_Tab39). When the scroll reaches its
// target (BG2VOFS_copy2 == dungmap_var4), clears dungmap_var2 to end the animation.
void DungeonMap_ScrollFloors() {  // 8aea7f
  int x = WORD(g_ram[10]) >> 3 & 1;
  dungmap_var5 += kDungMap_Tab39[x];
  dungmap_var8 += kDungMap_Tab39[x];
  BG2VOFS_copy2 += kDungMap_Tab40[x];
  if (BG2VOFS_copy2 == dungmap_var4)
    dungmap_var2 = 0;
}

// Draws all sprites for the interactive dungeon map display each frame:
//   Slot 0:         DungeonMap_DrawLinkPointing — sword-pointer icon at Link's floor row.
//   Slots 1-8:      DungeonMap_DrawLocationMarker (×2, for sub-floor indicators).
//   Next slot:      DungeonMap_DrawBlinkingIndicator — blinking cursor at Link's exact room.
//   Next slot(s):   DungeonMap_DrawBossIcon — boss skull icon (if compass obtained, boss alive).
//   Remaining slots:DungeonMap_DrawFloorNumberObjects — B1/B2/1F/2F... floor labels.
//   Separate call:  DungeonMap_DrawFloorBlinker — blinking current-floor indicator bars.
void DungeonMap_DrawSprites() {  // 8aeab2
  int dung = cur_palace_index_x2 >> 1;
  uint8 r2 = (kDungMap_Tab5[dung] & 0xf);
  uint8 floor = r2 + dung_cur_floor;

  int spr_pos = 0;
  uint8 r14 = 0;
  DungeonMap_DrawLinkPointing(spr_pos++, r2, floor);
  do {
    spr_pos = DungeonMap_DrawLocationMarker(spr_pos, r14);
    r14 += 1;
  } while (spr_pos != 9);
  spr_pos = DungeonMap_DrawBlinkingIndicator(spr_pos);
  spr_pos = DungeonMap_DrawBossIcon(spr_pos);
  spr_pos = DungeonMap_DrawFloorNumberObjects(spr_pos);
  DungeonMap_DrawFloorBlinker();
}

// Draws the small sword icon that points to Link's current floor in the floor-number column.
// spr_pos: OAM slot to write. r2: number of below-ground floors. r3: floor index from base.
// The y-coordinate is taken from kDungMap_Tab33[r3] - 4, which maps a floor index to its
// pixel row in the map view. Adjusts for dungeons with unusual floor layouts (> 4 floors
// above ground). Uses the swap-palette tile (0x3e) unless palette_swap_flag is set (0x30).
void DungeonMap_DrawLinkPointing(int spr_pos, uint8 r2, uint8 r3) {  // 8aeaf0
  int dung = cur_palace_index_x2 >> 1;
  uint8 t5 = kDungMap_Tab5[dung];
  if (4 - r2 >= 0) {
    r3 += 4 - r2;
    int8 a = (t5 >> 4) - 4;
    if (a >= 0)
      r3 -= a;
  }
  SetOamPlain(&oam_buf[spr_pos], 0x19, kDungMap_Tab33[r3] - 4, 0, palette_swap_flag ? 0x30 : 0x3e, 2);
}

// Draws the blinking marker that shows Link's exact room position within the grid.
// Uses dungmap_var3 (x) and dungmap_var5 (y, clipped to 0xf0 if >= 256) for position.
// The tile cycles through kDungMap_Tab38 entries based on frame_counter >> 2 & 3,
// producing a 4-frame blink animation. Returns the next OAM slot index.
int DungeonMap_DrawBlinkingIndicator(int spr_pos) {  // 8aeb50
  SetOamPlain(&oam_buf[spr_pos], dungmap_var3 - 3, ((dungmap_var5 < 256) ? dungmap_var5 : 0xf0) - 3, 0x34, kDungMap_Tab38[frame_counter >> 2 & 3], 0);
  return spr_pos + 1;
}

// Draws 4 OAM sprites forming one sub-indicator row at the Link-position floor level.
// r14: which of the sub-floor indicator rows to render.
// y is taken from dungmap_var6 + kDungMap_Tab24[r14] (the base row for this sub-row).
// Checks if this row matches the room-blink row (dungmap_var5 & 0xf0 == r15) and if so
// adds 2 to the frame index to select the highlighted tile pair from kDungMap_Tab32.
// x positions are from kDungMap_Tab29[i] + (dungmap_var3 & 0xf0); y offsets from Tab30;
// tile attributes from Tab31. Returns the next OAM slot index after the 4 sprites.
int DungeonMap_DrawLocationMarker(int spr_pos, uint16 r14) {  // 8aeba8
  for (int i = 3; i >= 0; i--, spr_pos++) {
    uint8 r15 = dungmap_var6 + kDungMap_Tab24[r14];
    int fr = (frame_counter >> 2) & 1;
    if ((dungmap_var5 + 1 & 0xf0) == r15 + 1 && dungmap_var5 < 256)
      fr += 2;
    SetOamPlain(&oam_buf[spr_pos], kDungMap_Tab29[i] + (dungmap_var3 & 0xf0),
                r15 + kDungMap_Tab30[i],
                0, kDungMap_Tab32[fr] | kDungMap_Tab31[i], 2);
  }
  return spr_pos;
}

// Draws the floor-number label sprites (B2, B1, 1F, 2F ...) in the right-side column.
// Computes the number of visible floor labels from the dungeon's floor descriptor.
// kDungMap_Tab33[yv] gives the y-coordinate of the first label, then each label is
// placed 16 pixels below the previous. Each label uses two OAM slots (kDungMap_Tab34
// for the digit tile, 0x1c/0x1d for the B/F suffix glyphs). Returns the next free
// OAM slot index.
int DungeonMap_DrawFloorNumberObjects(int spr_pos) {  // 8aec0a
  uint8 r2 = (kDungMap_Tab5[cur_palace_index_x2 >> 1] >> 4 & 0xf);
  uint8 r3 = (kDungMap_Tab5[cur_palace_index_x2 >> 1] & 0xf);
  uint8 yv = 7;
  if (r2 + r3 != 8 && r2 < 4) {
    yv = 6;
    for (int i = 3; i != 0 && i != r2; i--)
      yv--;
    if (r3 >= 5) {
      for (int i = 5; i != r3 && r3 != 8; i++)
        yv++;
    }
  }

  uint8 r4 = kDungMap_Tab33[yv] + 1;
  r2--;
  r3 = -r3;
  do {
    SetOamPlain(&oam_buf[spr_pos + 0], 0x30, r4, sign8(r2) ? 0x1c : kDungMap_Tab34[r2], 0x3d, 0);
    SetOamPlain(&oam_buf[spr_pos + 1], 0x38, r4, sign8(r2) ? kDungMap_Tab34[r2 ^ 0xff] : 0x1d, 0x3d, 0);
    r4 += 16;
  } while (spr_pos += 2, r2-- != r3);
  return spr_pos;
}

// Draws the animated highlight bars that mark the currently viewed floor(s) in the
// floor-label column. Only draws during the "on" phase of the blink cycle (frame_counter & 0x10).
// For single-floor dungeons (flag = 0) draws one 4×8-pixel bar; for multi-floor (flag = 1)
// draws two adjacent bars. kDungMap_Tab35 selects the OAM base slot, kDungMap_Tab36 provides
// tile indices, and kDungMap_Tab33[r0] provides the y-coordinate of the current floor label.
// Each bar is two rows of 4 tiles (upper and vertically-flipped lower) at x = 40.
void DungeonMap_DrawFloorBlinker() {  // 8aeccf
  uint8 floor = dungmap_cur_floor;
  uint8 t5 = kDungMap_Tab5[cur_palace_index_x2 >> 1];
  uint8 flag = ((t5 >> 4 & 0xf) + (t5 & 0xf) != 1);
  floor -= flag;
  uint8 r0;
  uint8 i = flag;
  do {
    r0 = floor + (t5 & 0xf);
    int8 a = 4 - (t5 & 0xf);
    if (a >= 0) {
      r0 += a;
      a = (t5 >> 4) - 4;
      if (a >= 0)
        r0 -= a;
    }
    floor += 1;
  } while (i--);
  if (!(frame_counter & 0x10))
    return;
  uint8 y = kDungMap_Tab33[r0] - 4;
  do {
    uint8 x = 40;
    int spr_pos = 0x40 + kDungMap_Tab35[flag];
    for (int i = 3; i >= 0; i--, spr_pos++) {
      uint8 t = 0x3d | (i ? 0 : 0x40);
      SetOamPlain(&oam_buf[spr_pos + 0], x, y + flag * 16 + 0, kDungMap_Tab36[i], t, 0);
      SetOamPlain(&oam_buf[spr_pos + 4], x, y + flag * 16 + 8, kDungMap_Tab36[i], t | 0x80, 0);
      x += 8;
    }
  } while (flag--);
}

// Draws the boss-room skull icon if all conditions are met:
//   - Boss is still alive (save_dung_info[boss_room] & 0x800 == 0).
//   - Player has the compass for this dungeon (link_compass & kUpperBitmasks[dung]).
//   - kDungMap_Tab28[dung] >= 0 (dungeon has a defined boss room floor).
// Calls DungeonMap_DrawBossIconByFloor to place the floor-indicator sword sprite.
// Then, if it is the "show" phase (frame_counter & 0xf < 10), draws the boss skull tile
// (tile 0x31, palette 0x33) at the computed room-grid position (dungmap_var7/8).
// Returns the next OAM slot index.
int DungeonMap_DrawBossIcon(int spr_pos) {  // 8aede4
  int dung = cur_palace_index_x2 >> 1;
  if (save_dung_info[kDungMap_Tab25[dung]] & 0x800 || !(link_compass & kUpperBitmasks[dung]) || kDungMap_Tab28[dung] < 0)
    return spr_pos;
  spr_pos = DungeonMap_DrawBossIconByFloor(spr_pos);
  if ((frame_counter & 0xf) >= 10)
    return spr_pos;
  uint16 xy = kDungMap_Tab37[dung];
  SetOamPlain(&oam_buf[spr_pos], (xy >> 8) + dungmap_var7 + 0x90, (dungmap_var8 < 256) ? xy + dungmap_var8 : 0xf0, 0x31, 0x33, 0);
  return spr_pos + 1;
}

// Draws the sword-pointer icon in the floor-number column pointing to the boss's floor.
// Uses the same floor-index → y-coordinate lookup (kDungMap_Tab33) as DungeonMap_DrawLinkPointing,
// but computed from kDungMap_Tab28[dung] (the boss's absolute floor index in this dungeon).
// Only drawn during the "show" phase (frame_counter & 0xf < 10) to blink in sync with the
// skull icon drawn by DungeonMap_DrawBossIcon. Returns the next OAM slot index.
int DungeonMap_DrawBossIconByFloor(int spr_pos) {  // 8aee95
  int dung = cur_palace_index_x2 >> 1;
  uint8 t5 = kDungMap_Tab5[dung];
  uint8 r2 = t5 & 0xf;
  uint8 r3 = r2 + kDungMap_Tab28[dung];
  if (4 - r2 >= 0) {
    r3 += 4 - r2;
    int8 a = (t5 >> 4) - 4;
    if (a >= 0)
      r3 -= a;
  }
  if ((frame_counter & 0xf) >= 10)
    return spr_pos;
  SetOamPlain(&oam_buf[spr_pos], 0x4c, kDungMap_Tab33[r3], 0x31, 0x33, 0);
  return spr_pos + 1;
}

// Dungeon map phase 7: Restores all game graphics state after the dungeon map view.
// Disables HDMA, erases the dungeon-map tile maps, restores the saved tile theme indices
// (main, aux, sprite) and TM/TS registers, reinitialises tilesets, resets palette mode
// and rebuilds the HUD. Then re-uploads all 4 room quadrants to VRAM via the NMI tile
// uploader (NMI_UploadTilemap / WaterFlood_BuildOneQuadrantForVRAM loop). Resets NMI
// subroutine, restores palette from the map backup (including fixed colour data),
// re-enables HDMA from the pre-map backup, triggers CGRAM upload, and fades music back.
// Sets overworld_map_state++ and zeroes INIDISP_copy so DungMap_RestoreOld can fade in.
void DungeonMap_RecoverGFX() {  // 8aef19
  uint8 hdmaen_bak = HDMAEN_copy;
  HDMAEN_copy = 0;
  EraseTileMaps_normal();

  TM_copy = mapbak_TM;
  TS_copy = mapbak_TS;
  main_tile_theme_index = mapbak_main_tile_theme_index;
  sprite_graphics_index = mapbak_sprite_graphics_index;
  aux_tile_theme_index = mapbak_aux_tile_theme_index;
  InitializeTilesets();
  overworld_palette_aux_or_main = 0;
  hud_palette = 0;
  Hud_Rebuild();

  overworld_screen_transition = 0;
  dung_cur_quadrant_upload = 0;
  do {
    WaterFlood_BuildOneQuadrantForVRAM();
    NMI_UploadTilemap();
    Dungeon_PrepareNextRoomQuadrantUpload();
    NMI_UploadTilemap();
  } while (dung_cur_quadrant_upload != 0x10);

  nmi_subroutine_index = 0;
  subsubmodule_index = 0;
  HDMAEN_copy = hdmaen_bak;

  memcpy(main_palette_buffer, mapbak_palette, sizeof(uint16) * 256);
  COLDATA_copy0 |= overworld_fixed_color_plusminus;
  COLDATA_copy1 |= overworld_fixed_color_plusminus;
  COLDATA_copy2 |= overworld_fixed_color_plusminus;

  sound_effect_2 = 16;
  music_control = 0xf3;
  RecoverPegGFXFromMapping();
  flag_update_cgram_in_nmi++;
  overworld_map_state++;
  INIDISP_copy = 0;
  nmi_disable_core_updates = 0;
}

// Restores the star-tile CHR data (the animated sparkling floor tiles used in some dungeons)
// from a saved backup after the dungeon map has finished using that CHR slot, then advances
// overworld_map_state to the next phase.
void ToggleStarTilesAndAdvance() {  // 8aefc9
  Dungeon_RestoreStarTileChr();
  overworld_map_state++;
}

// Initialises the "GAME OVER" letter ancillae for the game-over animation sequence.
// Resets the boomerang-in-place flag, sets the X positions of all 8 ancilla slots to 0xb0
// (off-screen right), activates ancilla slot 0 (type = 1), and sets hookshot_effect_index = 6
// so the ancilla system knows to use the GAME OVER letter animation.
void Death_InitializeGameOverLetters() {  // 8afe20
  flag_for_boomerang_in_place = 0;
  for (int i = 0; i < 8; i++) {
    ancilla_x_lo[i] = 0xb0;
    ancilla_x_hi[i] = 0;
  }
  ancilla_type[0] = 1;
  hookshot_effect_index = 6;
}

// Loads a save-game slot from SRAM into WRAM after a "Save and Quit" selection.
// Clears the bird-travel position for the overworld NPC bird (slot 15), copies the
// dungeon/event data block (0x500 bytes) from the correct SRAM offset to save_dung_info,
// resets tile animation counters, resets Link's inventory display variables, and optionally
// clears the mosaic level (kFeatures0_MiscBugFixes) to prevent a mosaic glitch on reload.
// Then transitions to module 5 (file select) with a fresh submodule_index = 0.
void CopySaveToWRAM() {  // 8ccfbb
  int k = 0xf;
  bird_travel_x_hi[k] = 0;
  bird_travel_y_hi[k] = 0;
  bird_travel_x_lo[k] = 0;
  bird_travel_y_lo[k] = 0;
  birdtravel_var1[k] = 0;

  memcpy(save_dung_info, &g_zenv.sram[WORD(g_ram[0])], 0x500);

  bg_tile_animation_countdown = 7;
  word_7EC013 = 7;
  word_7EC00F = 0;
  word_7EC015 = 0;
  word_7E0219 = 0x6040;
  word_7E021D = 0x4841;
  word_7E021F = 0x7f;
  word_7E0221 = 0xffff;

  // If you save / quit in the middle of a mosaic effect, such as
  // being electrocuted by a buzz blob, the resumed game will skip
  // the location prompt and start in the sanctuary.
  if (enhanced_features0 & kFeatures0_MiscBugFixes)
    mosaic_level = 0;

  hud_var1 = 128;
  main_module_index = 5;
  submodule_index = 0;
  which_entrance = 0;
  nmi_disable_core_updates = 0;
  hud_palette = 0;
}

// Text-rendering entry point: dispatches to the function selected by messaging_module.
// kMessaging_Text[0] = RenderText_Initialize (set up VWF state, load char buffer, queue NMI).
// kMessaging_Text[1] = Text_InitVwfState (reset cursor/line state only).
// kMessaging_Text[2] = RenderText_PostDeathSaveOptions (force dialogue 3 for death screen).
void RenderText() {  // 8ec440
  kMessaging_Text[messaging_module]();
}

// Specialised text initialiser for the post-death save/continue options screen.
// Forces dialogue_message_index = 3 (the "Continue / Save and Continue / Save and Quit"
// message), then initialises the VWF pipeline and positions the text box, sets
// text_render_state = 2 (begin rendering characters immediately), and runs 5 render
// ticks to ensure the text is fully drawn before returning control.
void RenderText_PostDeathSaveOptions() {  // 8ec455
  dialogue_message_index = 3;
  Text_Initialize_initModuleStateLoop();
  text_msgbox_topleft = 0x61e8;
  text_render_state = 2;
  for (int i = 0; i < 5; i++)
    Text_Render();
}

// Full text-rendering initialiser: decompresses story graphics if in the attract module
// (module 20), resets HUD palettes 4 and 5 in attract mode, then delegates to
// Text_Initialize_initModuleStateLoop for the VWF pipeline reset.
void Text_Initialize() {  // 8ec483
  if (main_module_index == 20)
    ResetHUDPalettes4and5();
  Attract_DecompressStoryGFX();
  Text_Initialize_initModuleStateLoop();
}

// Core VWF pipeline initialiser used by both Text_Initialize and RenderText_PostDeathSaveOptions.
// Copies kText_InitializationData (32 bytes) into the text state block (text_msgbox_topleft_copy),
// resets the VWF state variables (Text_InitVwfState), positions the text window based on Link's
// screen y-coordinate (RenderText_SetDefaultWindowPosition), sets the VRAM tilemap pointer
// (text_tilemap_cur = 0x3980), loads the character data buffer (Text_LoadCharacterBuffer),
// clears messaging_buf (0x7e0 bytes / 2016 words), and queues NMI subroutine 2 + disables
// core NMI updates for 2 frames to allow the initial VRAM upload to complete.
void Text_Initialize_initModuleStateLoop() {  // 8ec493
  memcpy(&text_msgbox_topleft_copy, kText_InitializationData, 32);
  Text_InitVwfState();
  RenderText_SetDefaultWindowPosition();
  text_tilemap_cur = 0x3980;
  Text_LoadCharacterBuffer();
  memset(messaging_buf, 0, 0x7e0);
  nmi_subroutine_index = 2;
  nmi_disable_core_updates = 2;
}

// Resets all VWF (variable-width font) cursor state:
//   vwf_curline: current text line index (0 = top row of the box).
//   vwf_flag_next_line: set to 1 when a line-switch command was just processed.
//   vwf_var1: horizontal character-slot index within the current line.
//   vwf_line_ptr: byte offset into messaging_buf for the current VWF tile row.
void Text_InitVwfState() {  // 8ec4c9
  vwf_curline = 0;
  vwf_flag_next_line = 0;
  vwf_var1 = 0;
  vwf_line_ptr = 0;
}

// US-encoding text command IDs used in dialogue byte streams.
// Bytes below kTextCommandStart_US (0x67) are printable characters.
// Bytes 0x67..0x7F are command bytes; the 25 commands below map to the 25 IDs.
// TEXTCMD_MK packs (param, cmd, multibyte) into a single uint32 for the decoder.
enum {
  kTextCommandStart_US = 0x67,
  kTextDictBase = 0x88,   // Dictionary byte threshold: bytes >= 0x88 are compressed-word indices.

  kTextCmd_NextPic = 0,
  kTextCmd_Choose = 1,
  kTextCmd_Item = 2,
  kTextCmd_Name = 3,
  kTextCmd_Window = 4,  // Only used with 2
  kTextCmd_Number = 5,
  kTextCmd_Position = 6,
  kTextCmd_ScrollSpd = 7,
  kTextCmd_Selchg = 8,
  kTextCmd_Choose3 = 10,
  kTextCmd_Choose2 = 11,
  kTextCmd_Scroll = 12,
  kTextCmd_1 = 13,
  kTextCmd_2 = 14,
  kTextCmd_3 = 15,
  kTextCmd_Color = 16,
  kTextCmd_Wait = 17,
  kTextCmd_Sound = 18,
  kTextCmd_Speed = 19,
  kTextCmd_Mark = 20,     // Unused
  kTextCmd_Mark2 = 21,    // Unused
  kTextCmd_Clear = 22,    // Unused
  kTextCmd_Waitkey = 23,
  kTextCmd_EndMessage = 24,

  kTextCmd_IsLetter = 25, // Pseudo cmd
};

// EU (European/PAL) text command byte values. Bytes 0x7F..0x86 are simple single-byte
// commands; 0x87 is an escape prefix for extended two-byte commands (nybble dispatched).
enum {
  kTextCmd_EU_Scroll = 0x80,  // frequency 875
  kTextCmd_EU_Waitkey = 0x81, // frequency 362
  kTextCmd_EU_1 = 0x82,       // frequency 25
  kTextCmd_EU_2 = 0x83,       // frequency 496
  kTextCmd_EU_3 = 0x84,       // frequency 347
  kTextCmd_EU_Name = 0x85,    // frequency 64
  kTextCmd_EU_Rest = 0x87,    
};

#define TEXTCMD_MULTIBYTE(a) ((a) & 1)
#define TEXTCMD_CMD(a) (((a) >> 1) & 0x1f)
#define TEXTCMD_PARAM(a) ((a) >> 6)
#define TEXTCMD_MK(c, x, m) ((c) << 6 | (x) << 1 | (m))

// Decodes one dialogue byte (a) and its optional second byte (*src) into a packed
// command word using TEXTCMD_MK(param, cmd, multibyte).
// The encoding differs by region (g_zenv.dialogue_flags & 1 == 0 → US, else EU):
//
// US encoding: bytes < 0x67 are printable; bytes 0x67..0x7F are commands offset by
//   kTextCommandStart_US. kText_CommandLengths_US indicates which commands consume a
//   second byte (their parameter is the next byte value).
//
// EU encoding: bytes < 0x7F are printable; 0x7F..0x86 map via kReturns_Simple to
//   simple commands; 0x87 is an escape prefix — the following byte's upper nybble
//   selects a command family (Wait/Color/Number/Speed/Sound/extended), lower nybble
//   is the parameter. kReturns_Ext handles the multi-byte extended commands (Choose, Item, etc.).
//
// Returns: TEXTCMD_MK(param, cmd_id, is_multibyte) where:
//   cmd_id: one of the kTextCmd_* constants.
//   param:  command argument (e.g. speed value, colour index, wait duration).
//   is_multibyte: 1 if the caller must skip an extra byte in the source stream.
uint32 Text_DecodeCmd(uint8 a, const uint8 *src) {
  if ((g_zenv.dialogue_flags & 1) == 0) {
    // US encoding
    if (a < kTextCommandStart_US)
      return TEXTCMD_MK(a, kTextCmd_IsLetter, 0);
    if (a >= 0x80)
      return TEXTCMD_MK(26, kTextCmd_IsLetter, 0); // could happen when loading snapshots
    assert(a < 0x80);
    static const uint8 kText_CommandLengths_US[] = { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
    if (kText_CommandLengths_US[a - kTextCommandStart_US])
      return TEXTCMD_MK(*src, a - kTextCommandStart_US, 1);
    else
      return TEXTCMD_MK(0, a - kTextCommandStart_US, 0);
  } else {
    // EU encoding
    if (a < 0x7f)
      return TEXTCMD_MK(a, kTextCmd_IsLetter, 0);
    static const uint8 kSoundLut[] = {45};
    static const uint8 kReturns_Simple[] = {
      TEXTCMD_MK(0, kTextCmd_EndMessage, 0),
      TEXTCMD_MK(0, kTextCmd_Scroll, 0),
      TEXTCMD_MK(0, kTextCmd_Waitkey, 0),
      TEXTCMD_MK(0, kTextCmd_1, 0),
      TEXTCMD_MK(0, kTextCmd_2, 0),
      TEXTCMD_MK(0, kTextCmd_3, 0),
      TEXTCMD_MK(0, kTextCmd_Name, 0),
      TEXTCMD_MK(0, kTextCmd_Name, 0), // Unused
    };
    if (a < kTextCmd_EU_Rest)
      return kReturns_Simple[a - 0x7f];
    a = *src;
    switch (a >> 4) {
    case 0: return TEXTCMD_MK(a & 0xF, kTextCmd_Wait, 1);
    case 1: return TEXTCMD_MK(a & 0xF, kTextCmd_Color, 1);
    case 2: return TEXTCMD_MK(a & 0xF, kTextCmd_Number, 1);
    case 3: return TEXTCMD_MK(a & 0xF, kTextCmd_Speed, 1);
    case 4: return TEXTCMD_MK(kSoundLut[a & 0xF], kTextCmd_Sound, 1);
    case 8: {
      static const uint8 kReturns_Ext[] = {
        TEXTCMD_MK(0, kTextCmd_Choose, 1),
        TEXTCMD_MK(0, kTextCmd_Choose2, 1),
        TEXTCMD_MK(0, kTextCmd_Choose3, 1),
        TEXTCMD_MK(0, kTextCmd_Selchg, 1),
        TEXTCMD_MK(0, kTextCmd_Item, 1),
        TEXTCMD_MK(0, kTextCmd_NextPic, 1),
        TEXTCMD_MK(2, kTextCmd_Window, 1),
        TEXTCMD_MK(0, kTextCmd_Position, 1),
        TEXTCMD_MK(1, kTextCmd_Position, 1),
      };
      return kReturns_Ext[a - 0x80];
    }
    default:
      assert(0);
      return TEXTCMD_MK(26, kTextCmd_IsLetter, 0);
    }
  }
}

// Pre-processes one dialogue message into messaging_text_buffer before the VWF renderer reads it.
// Iterates the raw dialogue bytes from g_zenv.dialogue_blk[1][dialogue_message_index]:
//   - Bytes >= kTextDictBase (0x88) are dictionary-word indices: the corresponding word
//     from g_zenv.dialogue_blk[0] is expanded verbatim into the buffer.
//   - kTextCmd_Name: immediately expanded to the 6-character player name via Text_WritePlayerName.
//   - kTextCmd_Window: sets text_render_state to select the border style (2 = incremental).
//   - kTextCmd_Number: encodes one BCD digit from dialogue_number[] as a printable tile byte.
//   - kTextCmd_Position: sets text_msgbox_topleft for the pre-defined box position.
//   - kTextCmd_Color: updates text_tilemap_cur with the new palette-index bits.
//   - All other commands (including multi-byte ones) are copied verbatim to the buffer so
//     the VWF render loop can process them incrementally each frame.
// The buffer is terminated with 0x7F and dialogue_msg_read_pos is reset to 0.
// Perform initial parsing of the string, expanding words, processing some commands, etc.
void Text_LoadCharacterBuffer() {  // 8ec4e2
  MemBlk dictionary = FindIndexInMemblk(g_zenv.dialogue_blk, 0);
  MemBlk dialogue = FindIndexInMemblk(g_zenv.dialogue_blk, 1);
  MemBlk text_str = FindIndexInMemblk(dialogue, dialogue_message_index);
  const uint8 *src = text_str.ptr, *src_end = src + text_str.size, *src_org = src;
  uint8 *dst = messaging_text_buffer;
  while (src < src_end) {
    uint8 c = *src++;
    if (c >= kTextDictBase) {
      MemBlk blk = FindIndexInMemblk(dictionary, c - kTextDictBase);
      memcpy(dst, blk.ptr, blk.size);
      dst += blk.size;
      continue;
    }
    // Decode the next byte or multibyte character (in case we support that in the future)
    // This is dependent on the current language cause US / PAL encode commands differently
    uint32 cmd = Text_DecodeCmd(c, src);
    switch (TEXTCMD_CMD(cmd)) {
    case kTextCmd_Name: dst = Text_WritePlayerName(dst); break;
    case kTextCmd_Window:  // RenderText_ExtendedCommand_SetWindowType
      text_render_state = TEXTCMD_PARAM(cmd);
      break;
    case kTextCmd_Number: {  // Text_WritePreloadedNumber
      uint8 t = TEXTCMD_PARAM(cmd);
      uint8 v = dialogue_number[t >> 1];
      *dst++ = 0x34 + ((t & 1) ? v >> 4 : v & 0xf);
      break;
    }
    case kTextCmd_Position:
      text_msgbox_topleft = kText_Positions[TEXTCMD_PARAM(cmd)];
      break;
    case kTextCmd_Color:
      text_tilemap_cur = ((0x387F & 0xe300) | 0x180) | (TEXTCMD_PARAM(cmd) << 10) & 0x3c00;
      break;
    default:
      // This combination is handled when rendering instead of here
      *dst++ = c;
      if (TEXTCMD_MULTIBYTE(cmd))
        *dst++ = *src;
      break;
    }
    src += TEXTCMD_MULTIBYTE(cmd);
  }
  *dst = 0x7f;
  dialogue_msg_read_pos = 0;
}

// Reads the player's 6-character name from SRAM and appends it to the character buffer at p.
// The name is stored as 16-bit packed characters in the selected save slot (srm_var1):
//   byte offset = 0x3d9 + (slot - 1) * 0x500 + i * 2.
// Each 16-bit value is unpacked: low nybble | (bits 5-8 >> 1) → filtered via
// Text_FilterPlayerNameCharacters to convert SNES character indices to printable tile bytes.
// Trailing space characters (0x59) are trimmed. Returns pointer past the last name character.
uint8 *Text_WritePlayerName(uint8 *p) {  // 8ec5b3
  uint8 slot = srm_var1;
  int offs = ((slot>>1) - 1) * 0x500;
  for (int i = 0; i < 6; i++) {
    uint8 *pp = &g_zenv.sram[0x3d9 + offs + i * 2];
    uint16 a = WORD(*pp);
    p[i] = Text_FilterPlayerNameCharacters(a & 0xf | (a >> 1) & 0xf0);
  }
  int i = 6;
  while (i && p[i - 1] == 0x59)
    i--;
  return p + i;
}

// Translates a packed SNES name character index into the VWF font tile index used in-game.
// Most values pass through unchanged. Special cases:
//   0x5F → 8  (space character in SNES name input → blank tile in dialogue font).
//   0x60 → 0x22 (period/punctuation mapping).
//   0x61 → 0x3E (another punctuation character).
//   >= 0x76: subtracts 0x42 to remap the extended character block.
uint8 Text_FilterPlayerNameCharacters(uint8 a) {  // 8ec639
  if (a >= 0x5f) {
    if (a >= 0x76)
      a -= 0x42;
    else if (a == 0x5f)
      a = 8;
    else if (a == 0x60)
      a = 0x22;
    else if (a == 0x61)
      a = 0x3e;
  }
  return a;
}

// Per-frame text rendering dispatcher. Calls kText_Render[text_render_state]:
//   0 = RenderText_Draw_Border            — draw the full text-box border in one frame.
//   1 = RenderText_Draw_BorderIncremental — draw the border in 3 incremental frames.
//   2 = RenderText_Draw_CharacterTilemap  — build the VWF tile map entries.
//   3 = RenderText_Draw_MessageCharacters — render characters one per (speed) frame.
//   4 = RenderText_Draw_Finish            — erase the text box and return to game.
void Text_Render() {  // 8ec8d9
  kText_Render[text_render_state]();
}

// Text rendering phase 0 (immediate border): Draws the entire text-box border in one frame.
// Initialises the border VRAM destination (RenderText_DrawBorderInitialize), then calls
// RenderText_DrawBorderRow 8 times — once for the top row (y=0), 6 times for the middle
// rows (y=6 each), and once for the bottom row (y=12). Queues an NMI tilemap upload and
// advances text_render_state to 2 (character tilemap phase).
void RenderText_Draw_Border() {  // 8ec8ea
  RenderText_DrawBorderInitialize();
  uint16 *d = RenderText_DrawBorderRow(vram_upload_data, 0);
  for(int i = 0; i != 6; i++)
    d = RenderText_DrawBorderRow(d, 6);
  d = RenderText_DrawBorderRow(d, 12);
  nmi_load_bg_from_vram = 1;
  text_render_state = 2;
}

// Text rendering phase 1 (incremental border): Draws the text-box border over 3 frames
// to produce a "box opening" animation. text_incremental_state tracks progress:
//   0: draw the top row (RenderText_DrawBorderRow y=0).
//   1..6: draw one middle row per frame (y=6 each).
//   7+: draw the bottom row (y=12) and set text_render_state = 2 to continue rendering.
// Each call queues one NMI upload (nmi_load_bg_from_vram = 1).
void RenderText_Draw_BorderIncremental() {  // 8ec919
  nmi_load_bg_from_vram = 1;
  uint8 a = text_incremental_state;
  uint16 *d = vram_upload_data;
  if (a)
    a = (a < 7) ? 1 : 2;
  switch (a) {
  case 0:
    RenderText_DrawBorderInitialize();
    d = RenderText_DrawBorderRow(d, 0);
    text_incremental_state++;
    break;
  case 1:
    d = RenderText_DrawBorderRow(d, 6);
    text_incremental_state++;
    break;
  case 2:
    text_render_state = 2;
    d = RenderText_DrawBorderRow(d, 12);
    text_incremental_state++;
    break;
  }
}

// Text rendering phase 2: Builds the sequential VRAM tile-index map for the VWF character area.
// Writes 126 consecutive tile indices starting at text_tilemap_cur into the VWF tilemap region
// of WRAM (g_ram[0x1300..]), then calls RenderText_Refresh to upload them to VRAM and
// advances text_render_state to 3 (character rendering phase).
void RenderText_Draw_CharacterTilemap() {  // 8ec97d
  Text_BuildCharacterTilemap();
  text_render_state++;
}

// Text rendering phase 3: Per-frame character rendering and command processing loop.
// Reads the next byte from messaging_text_buffer[dialogue_msg_read_pos] via Text_DecodeCmd
// and dispatches on the command ID:
//   kTextCmd_IsLetter:  Render one character via VWF_RenderSingle (with speed throttling:
//                       if vwf_line_speed_cur >= 2, skips rendering and decrements speed).
//                       Re-enters RESTART label if speed == 0 to render multiple chars/frame.
//   kTextCmd_NextPic:   In attract module, waits for a palette fade to finish; else skips.
//   kTextCmd_Choose:    Two-choice lower-option selector (re-renders dialogue 1 or 2).
//   kTextCmd_Item:      Y-item picker box; scrolls through obtainable items.
//   kTextCmd_ScrollSpd: Updates dialogue_scroll_speed; immediately advances stream position.
//   kTextCmd_Selchg:    Two-choice upper-option selector (dialogues 11/12).
//   kTextCmd_Choose3:   Three-choice selector cycling dialogues 6/7/8.
//   kTextCmd_Choose2:   Binary choice selector cycling dialogues 9/10.
//   kTextCmd_Scroll:    Scrolls the text box up via RenderText_Draw_Scroll.
//   kTextCmd_1/2/3:     Set vwf_curline to move to row 0/1/2 of the box.
//   kTextCmd_Wait:      Timed pause using kText_WaitDurations[param].
//   kTextCmd_Sound:     Fires a sound effect (sound_effect_2 = param).
//   kTextCmd_Speed:     Sets vwf_line_speed and vwf_line_speed_cur.
//   kTextCmd_Waitkey:   Pauses until A/B pressed; plays a chime after debounce (28 frames).
//   kTextCmd_EndMessage:Waits for any button then sets text_render_state = 4 to finish.
// After processing, queues nmi_subroutine_index = 2 and nmi_disable_core_updates = 2.
void RenderText_Draw_MessageCharacters() {  // 8ec984
RESTART:;
  uint32 cmd = Text_DecodeCmd(messaging_text_buffer[dialogue_msg_read_pos],
      &messaging_text_buffer[dialogue_msg_read_pos + 1]);

  switch (TEXTCMD_CMD(cmd)) {
  case kTextCmd_IsLetter:
    if (vwf_line_speed_cur >= 2) {
      vwf_line_speed_cur--;
      break;
    }
    VWF_RenderSingle(TEXTCMD_PARAM(cmd));
    dialogue_msg_read_pos += 1 + TEXTCMD_MULTIBYTE(cmd);
    if (vwf_line_speed_cur == 0)
      goto RESTART;
    break;
  case kTextCmd_NextPic:  // RenderText_Draw_NextImage
    if (main_module_index == 20) {
      PaletteFilterHistory();
      if (!BYTE(palette_filter_countdown))
        goto COMMAND_DONE;
    } else {
      goto COMMAND_DONE;
    }
    break;
  case kTextCmd_Choose:  // RenderText_Draw_Choose2LowOr3
    RenderText_Draw_Choose2LowOr3();
    break;
  case kTextCmd_Item:  // RenderText_Draw_ChooseItem
    RenderText_Draw_ChooseItem();
    break;
  case kTextCmd_Name:
  case kTextCmd_Window:
  case kTextCmd_Number:
  case kTextCmd_Position:
  case kTextCmd_Color:
    // These get handled in Text_LoadCharacterBuffer
    assert(0);
    break;
  // These are unused
  case kTextCmd_Mark:
  case kTextCmd_Mark2:
  case kTextCmd_Clear:
    assert(0);
    break;
  case kTextCmd_ScrollSpd:
    dialogue_scroll_speed = TEXTCMD_PARAM(cmd);
    goto COMMAND_DONE;
  case kTextCmd_Selchg:   // RenderText_Draw_Choose2HiOr3
    RenderText_Draw_Choose2HiOr3();
    break;
  case kTextCmd_Choose3:  // RenderText_Draw_Choose3
    RenderText_Draw_Choose3();
    break;
  case kTextCmd_Choose2:  // RenderText_Draw_Choose1Or2
    RenderText_Draw_Choose1Or2();
    break;
  case kTextCmd_Scroll:  // RenderText_Draw_Scroll
    if (RenderText_Draw_Scroll())
      goto COMMAND_DONE;
    break;
  case kTextCmd_1:  //
  case kTextCmd_2:  //
  case kTextCmd_3:  // VWF_SetLine
    vwf_curline = kVWF_RowPositions[TEXTCMD_CMD(cmd) - kTextCmd_1];
    vwf_flag_next_line = 1;
    goto COMMAND_DONE;
  case kTextCmd_Wait:  // RenderText_Draw_Wait
    switch (joypad1L_last & 0x80 ? 1 : text_wait_countdown) {
    case 0:
      text_wait_countdown = kText_WaitDurations[TEXTCMD_PARAM(cmd)] - 1;
      break;
    case 1:
      BYTE(text_wait_countdown) = 0;
      goto COMMAND_DONE;
    default:
      text_wait_countdown--;
      break;
    }
    break;
  case kTextCmd_Sound:  // RenderText_Draw_PlaySfx
    sound_effect_2 = TEXTCMD_PARAM(cmd);
    goto COMMAND_DONE;
  case kTextCmd_Speed:  // RenderText_Draw_SetSpeed
    vwf_line_speed = vwf_line_speed_cur = TEXTCMD_PARAM(cmd);
    goto COMMAND_DONE;
  case kTextCmd_Waitkey:  // RenderText_Draw_PauseForInput
    if (text_wait_countdown2 != 0) {
      if (--text_wait_countdown2 == 1)
        sound_effect_2 = 36;
    } else {
      if ((filtered_joypad_H | filtered_joypad_L) & 0xc0) {
        text_wait_countdown2 = 28;
        goto COMMAND_DONE;
      }
    }
    break;
  case kTextCmd_EndMessage:  // RenderText_Draw_Terminate
    if (text_wait_countdown2 != 0) {
      if (--text_wait_countdown2 == 1)
        sound_effect_2 = 36;
    } else {
      if ((filtered_joypad_H | filtered_joypad_L)) {
        text_render_state = 4;
        text_wait_countdown2 = 28;
      }
    }
    break;
  }
  if (0) COMMAND_DONE: {
    dialogue_msg_read_pos += 1 + TEXTCMD_MULTIBYTE(cmd);
  }
  nmi_subroutine_index = 2;
  nmi_disable_core_updates = 2;
}

// Text rendering phase 4: Erases the text box and returns to the game module.
// Initialises the border destination address, then writes a 3-word NMI upload entry that
// fills the top-left text-box cell with blank tile 0x387F and terminates with 0xffff.
// Queues an NMI tilemap upload, resets messaging_module and submodule_index to 0,
// and restores main_module_index to saved_module_for_menu.
void RenderText_Draw_Finish() {  // 8eca35
  RenderText_DrawBorderInitialize();
  uint16 *d = vram_upload_data;
  d[0] = swap16(text_msgbox_topleft_copy);
  d[1] = 0x2E42;
  d[2] = 0x387F;
  d[3] = 0xffff;
  nmi_load_bg_from_vram = 1;
  messaging_module = 0;
  submodule_index = 0;
  main_module_index = saved_module_for_menu;
}

// Renders one variable-width font glyph (character index c) into messaging_buf.
// Plays the text-scroll sound (sound_effect_2 = 12) unless c == 0x59 (space).
// Resets vwf_line_speed_cur to the current line speed.
// If vwf_flag_next_line is set, advances to the next line: updates vwf_line_ptr from
// kVWF_RenderCharacter_renderPos and vwf_var1 from kVWF_RenderCharacter_linePositions.
// The glyph is looked up in the font data (g_zenv.dialogue_font_blk[0]), with width from
// dialogue_font_blk[1][c]. vwf_arr[vwf_var1] accumulates the pixel-column offset so
// successive glyphs pack tightly. The 16-row glyph bitmap is composited into two 8-row
// strips in messaging_buf using kVWF_RenderCharacter_setMasks[] for per-pixel masking:
//   Bits 6 and 14 of each glyph word drive the two adjacent pixel planes (XOR for set,
//   AND NOT for clear) across 8 columns per row. Any overflow pixels wrap to the next
//   tile via a WORD write to mbuf[x+16]. The second strip is at vwf_line_ptr + 0x150.
void VWF_RenderSingle(int c) {  // 8ecab8
  if (c != 0x59)
    sound_effect_2 = 12;
  vwf_line_speed_cur = vwf_line_speed;

  if (vwf_flag_next_line) {
    vwf_line_ptr = kVWF_RenderCharacter_renderPos[vwf_curline>>1];
    vwf_var1 = kVWF_RenderCharacter_linePositions[vwf_curline>>1];
    vwf_flag_next_line = 0;
  }
  
  const uint8 *kFontData = FindIndexInMemblk(g_zenv.dialogue_font_blk, 0).ptr;
  uint8 width = FindIndexInMemblk(g_zenv.dialogue_font_blk, 1).ptr[c];
  assert(width <= 8);

  int i = vwf_var1++;
  uint8 arrval = vwf_arr[i];
  vwf_arr[i + 1] = arrval + width;
  uint16 r10 = (c & 0x70) * 2 + (c & 0xf);
  uint16 r0 = arrval * 2;
  const uint16 *src2 = (uint16*)(kFontData + r10 * 16);
  uint8 *mbuf = (uint8 *)messaging_buf;
  for (int i = 0; i != 16; i += 2) {
    uint16 r4 = *src2++;
    int y = r0 + vwf_line_ptr;
    int x = (y & 0xff0) + i;
    y = (y >> 1) & 7;
    uint8 r3 = width;
    do {
      if (r4 & 0x0080)
        mbuf[x + 0] ^= kVWF_RenderCharacter_setMasks[y];
      else
        mbuf[x + 0] &= ~kVWF_RenderCharacter_setMasks[y];
      if (r4 & 0x8000)
        mbuf[x + 1] ^= kVWF_RenderCharacter_setMasks[y];
      else
        mbuf[x + 1] &= ~kVWF_RenderCharacter_setMasks[y];
      r4 = (r4 & ~0x8080) << 1;
      //r4 <<= 1;
    } while (--r3 && ++y != 8);
    x += 16;
    if (r4 != 0)
      WORD(mbuf[x + 0]) = r4;
  }
  uint16 r8 = vwf_line_ptr + 0x150;
  const uint16 *src3 = (uint16*)(kFontData + (r10 + 16) * 16);
  for (int i = 0; i != 16; i += 2) {
    uint16 r4 = *src3++;
    int y = r8 + r0;
    int x = (y & 0xff0) + i;
    y = (y >> 1) & 7;
    uint8 r3 = width;
    do {
      if (r4 & 0x0080)
        mbuf[x + 0] ^= kVWF_RenderCharacter_setMasks[y];
      else
        mbuf[x + 0] &= ~kVWF_RenderCharacter_setMasks[y];
      if (r4 & 0x8000)
        mbuf[x + 1] ^= kVWF_RenderCharacter_setMasks[y];
      else
        mbuf[x + 1] &= ~kVWF_RenderCharacter_setMasks[y];
      //r4 <<= 1;
      r4 = (r4 & ~0x8080) << 1;
    } while (--r3 && ++y != 8);
    x += 16;
    if (r4 != 0)
      WORD(mbuf[x + 0]) = r4;
  }
}

// Handles the lower two-option choice box (kTextCmd_Choose): debounce → accept A/B to confirm
// (advances text_render_state to 4), or D-pad Up/Down to toggle between option 0 and 1.
// On selection change, updates choice_in_multiselect_box, plays the cursor-move sound,
// and reloads messaging_text_buffer from dialogue index 1 or 2 with a reset VWF state.
// sound_effect_1 = 43 (confirm sfx) is played on acceptance.
void RenderText_Draw_Choose2LowOr3() {  // 8ecd1a
  if (text_wait_countdown2 != 0) {
    if (--text_wait_countdown2 == 1)
      sound_effect_2 = 36;
  } else if ((filtered_joypad_H | filtered_joypad_L) & 0xc0) {
    sound_effect_1 = 43;
    text_render_state = 4;
  } else if (filtered_joypad_H & 12) {
    int t = filtered_joypad_H & 8 ? 0 : 1;
    if (choice_in_multiselect_box == t)
      return;
    choice_in_multiselect_box = t;
    sound_effect_2 = 32;
    dialogue_message_index = t + 1;
    Text_LoadCharacterBuffer();
    Text_InitVwfState();
  }
}

// Handles the Y-item picker choice box (kTextCmd_Item): A/B confirms (text_render_state = 4).
// D-pad Right advances to the next available item; D-pad Left goes to the previous item.
// Both directions call RenderText_FindYItem_Next / _Previous to scan the item inventory
// for the next/previous non-empty Y-item slot, then RenderText_Refresh to redraw the box.
// text_wait_countdown2 provides a debounce delay; on expiry, auto-advances to next item.
void RenderText_Draw_ChooseItem() {  // 8ecd88
  if (text_wait_countdown2 != 0) {
    if (--text_wait_countdown2 == 1)
      RenderText_FindYItem_Next();
  } else if ((filtered_joypad_H | filtered_joypad_L) & 0xc0) {
    text_render_state = 4;
  } else {
    if (filtered_joypad_H & 5) {
      choice_in_multiselect_box++;
    } else if (filtered_joypad_H & 10) {
      choice_in_multiselect_box--;
      RenderText_FindYItem_Previous();
      RenderText_Refresh();
      return;
    }
    RenderText_FindYItem_Next();
    RenderText_Refresh();
  }
}

// Scans backwards through the Y-item inventory (wrapping at 31 → 0) to find the previous
// non-empty, non-slot-15 item. Skips empty slots ((&link_item_bow)[x] == 0) and the
// reserved slot (x == 15). Slot 32 (big-bomb/capacity) requires slot 33 to also be set.
// On finding a valid item, calls RenderText_DrawSelectedYItem to update the HUD tile.
void RenderText_FindYItem_Previous() {  // 8ecdc8
  for (;;) {
    uint8 x = choice_in_multiselect_box;
    if (sign8(x))
      choice_in_multiselect_box = x = 31;
    if (x != 15 && ((&link_item_bow)[x] || x == 32 && (&link_item_bow)[x + 1]))
      break;
    choice_in_multiselect_box--;
  }
  RenderText_DrawSelectedYItem();
}

// Scans forwards through the Y-item inventory (wrapping at 32 → 0) to find the next
// non-empty, non-slot-15 item, applying the same slot-32 check as _Previous.
// On finding a valid item, calls RenderText_DrawSelectedYItem to update the HUD tile.
void RenderText_FindYItem_Next() {  // 8ecded
  for (;;) {
    uint8 x = choice_in_multiselect_box;
    if (x >= 32)
      choice_in_multiselect_box = x = 0;
    if (x != 15 && ((&link_item_bow)[x] || x == 32 && (&link_item_bow)[x + 1]))
      break;
    choice_in_multiselect_box++;
  }
  RenderText_DrawSelectedYItem();
}

// Updates the item-picker box in the VWF tilemap (WRAM at g_ram[0x1300]) to show
// the currently selected Y-item. Looks up the HUD item-tile pointer via Hud_GetItemBoxPtr,
// offsets into the tile table by the item's current state/variant, and copies 4 tiles
// (2 per row × 2 rows) into the tilemap at offsets 0xc2 and 0xec (the item box cells).
void RenderText_DrawSelectedYItem() {  // 8ece14
  int item = choice_in_multiselect_box;
  const uint16 *p = Hud_GetItemBoxPtr(item);
  p += ((item == 3 || item == 32) ? 1 : (&link_item_bow)[item]) * 4;
  uint8 *vwf300 = &g_ram[0x1300];
  memcpy(vwf300 + 0xc2, p, 4);
  memcpy(vwf300 + 0xec, p + 2, 4);
}

// Handles the upper two-option choice box (kTextCmd_Selchg): identical logic to
// RenderText_Draw_Choose2LowOr3 except the dialogue indices used are 11 and 12
// (for the "yes/no" variant where "Yes" is the upper option). Confirm plays sfx 43.
void RenderText_Draw_Choose2HiOr3() {  // 8ece83
  if (text_wait_countdown2 != 0) {
    if (--text_wait_countdown2 == 1)
      sound_effect_2 = 36;
  } else if ((filtered_joypad_H | filtered_joypad_L) & 0xc0) {
    sound_effect_1 = 43;
    text_render_state = 4;
  } else if (filtered_joypad_H & 12) {
    int t = filtered_joypad_H & 8 ? 0 : 1;
    if (choice_in_multiselect_box == t)
      return;
    choice_in_multiselect_box = t;
    sound_effect_2 = 32;
    dialogue_message_index = t + 11;
    Text_LoadCharacterBuffer();
    Text_InitVwfState();
  }
}

// Handles a three-option choice box (kTextCmd_Choose3): A/B/Start/Select confirms the
// current choice (sfx 43, text_render_state = 4). D-pad Up cycles the choice down by 1
// (wrapping 0 → 2); D-pad Down cycles up by 1 (wrapping 2 → 0). On selection change,
// plays the cursor-move sound and reloads messaging_text_buffer from dialogue index
// choice + 6 (dialogues 6/7/8 for options 0/1/2). text_wait_countdown2 debounces input.
void RenderText_Draw_Choose3() {  // 8ecef7
  uint8 y;
  if (text_wait_countdown2 != 0) {
    if (--text_wait_countdown2 == 1)
      sound_effect_2 = 36;
  } else if ((y = filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0) {
    sound_effect_1 = 43;
    text_render_state = 4;
  } else if (y & 12) {
    int choice = choice_in_multiselect_box;
    if (y & 8)
      choice = (choice == 0) ? 2 : choice - 1;
    else
      choice = (choice == 2) ? 0 : choice + 1;
    choice_in_multiselect_box = choice;
    sound_effect_2 = 32;
    dialogue_message_index = choice + 6;
    Text_LoadCharacterBuffer();
    Text_InitVwfState();
  }
}

// Handles a binary choice box (kTextCmd_Choose2): A/B/Start/Select confirms (sfx 43,
// text_render_state = 4). D-pad Up selects option 0; D-pad Down selects option 1.
// On selection change, plays cursor sound and reloads from dialogue index choice + 9
// (dialogues 9/10 for options 0/1). text_wait_countdown2 debounces input.
void RenderText_Draw_Choose1Or2() {  // 8ecf72
  uint8 y;
  if (text_wait_countdown2 != 0) {
    if (--text_wait_countdown2 == 1)
      sound_effect_2 = 36;
  } else if ((y = filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0) {
    sound_effect_1 = 43;
    text_render_state = 4;
  } else if (y & 12) {
    int t = y & 8 ? 0 : 1;
    if (choice_in_multiselect_box == t)
      return;
    choice_in_multiselect_box = t;
    sound_effect_2 = 32;
    dialogue_message_index = t + 9;
    Text_LoadCharacterBuffer();
    Text_InitVwfState();
  }
}

// Scrolls the VWF text box up by one pixel row in messaging_buf.
// The scroll is repeated (dialogue_scroll_speed + 1) times per call for speed control.
// Each iteration rotates the 16-byte rows in messaging_buf: each tile's 8-word column
// shifts left by one word (p[0]=p[1]..p[6]=p[7]) and the last word (p[7]) is cleared
// from the bottom row (indices 0x34F..0x3EF, every 8 words).
// When byte_7E1CDF & 0xF wraps to 0 (every 16 pixel rows = one full tile row), sets
// vwf_curline = 4, vwf_flag_next_line = 1 (advance the VWF cursor to the new bottom row),
// and returns true so the caller advances the stream. Otherwise returns false.
bool RenderText_Draw_Scroll() {  // 8ecfe2
  uint8 r2 = dialogue_scroll_speed;
  do {
    for (int i = 0; i < 0x7e0; i += 16) {
      uint16 *p = (uint16 *)((uint8 *)messaging_buf + i);
      p[0] = p[1];
      p[1] = p[2];
      p[2] = p[3];
      p[3] = p[4];
      p[4] = p[5];
      p[5] = p[6];
      p[6] = p[7];
      p[7] = p[168];
    }
    uint16 *p = messaging_buf;
    for (int i = 0x34f; i <= 0x3ef; i += 8)
      p[i] = 0;

    if ((++byte_7E1CDF & 0xf) == 0) {
      vwf_curline = 4;
      vwf_flag_next_line = 1;
      return true;
    }
  } while (r2--);
  return false;
}

// Calculates the default text-box VRAM position based on Link's current screen y-coordinate.
// If Link is in the upper half of the screen (y < 0x78), places the box at the bottom
// (kText_Positions[0]); otherwise places it at the top (kText_Positions[1]).
// This prevents the text box from obscuring Link during dialogue.
void RenderText_SetDefaultWindowPosition() {  // 8ed280
  uint16 y = link_y_coord - BG2VOFS_copy2;
  int flag = (y < 0x78);
  text_msgbox_topleft = kText_Positions[flag];
}

// Snapshots the current text box top-left VRAM address into text_msgbox_topleft_copy.
// Called at the start of each border-drawing function so that successive row writes
// can increment the copy without corrupting text_msgbox_topleft for future re-draws.
void RenderText_DrawBorderInitialize() {  // 8ed29c
  text_msgbox_topleft_copy = text_msgbox_topleft;
}

// Writes one row of the text-box border into the NMI upload buffer starting at *d.
// y selects the tile set from kText_BorderTiles (divided by 2): 0 = top, 6 = middle, 12 = bottom.
// The entry format is: [VRAM destination (byte-swapped)] [transfer count 0x2F00 = 47 words]
//   [left-edge tile] [22× middle-tile] [right-edge tile] [0xffff terminator].
// Increments text_msgbox_topleft_copy by 0x20 (one VRAM row) for the next call.
// Returns a pointer to the terminator word so the caller can chain further entries.
uint16 *RenderText_DrawBorderRow(uint16 *d, int y) {  // 8ed2ab
  y >>= 1;
  *d++ = swap16(text_msgbox_topleft_copy);
  text_msgbox_topleft_copy += 0x20;
  *d++ = 0x2F00;
  *d++ = kText_BorderTiles[y];
  for(int i = 0; i < 22; i++)
    *d++ = kText_BorderTiles[y+1];
  *d++ = kText_BorderTiles[y+2];
  *d = 0xffff;
  return d;
}

// Fills the VWF tilemap region (g_ram[0x1300..0x13FB], 126 words) with sequential
// VRAM tile indices starting at text_tilemap_cur. These indices address the CHR tiles
// that VWF_RenderSingle writes pixel data into. After populating the tilemap, calls
// RenderText_Refresh to upload it and the character bitplane data to VRAM.
void Text_BuildCharacterTilemap() {  // 8ed2ec
  uint16 *vwf300 = (uint16 *)&g_ram[0x1300];
  for (int i = 0; i < 126; i++)
    vwf300[i] = text_tilemap_cur++;
  RenderText_Refresh();
}

// Uploads the VWF tilemap (the VRAM tilemap entries for the 6-row × 21-column character area)
// to the NMI upload buffer so the NMI routine will DMA them to VRAM.
// Initialises the border destination (text_msgbox_topleft_copy), skips to cell (1,1) inside
// the border (+ 0x21), then for each of the 6 character rows writes:
//   [VRAM destination] [transfer count 0x2900 = 41 words] [21 tile indices from g_ram[0x1300]].
// Terminates the buffer with 0xffff and queues nmi_load_bg_from_vram = 1.
void RenderText_Refresh() {  // 8ed307
  RenderText_DrawBorderInitialize();
  text_msgbox_topleft_copy += 0x21;
  uint16 *d = vram_upload_data;
  uint16 *s = (uint16 *)&g_ram[0x1300];
  for (int j = 0; j != 6; j++) {
    *d++ = swap16(text_msgbox_topleft_copy);
    text_msgbox_topleft_copy += 0x20;
    *d++ = 0x2900;
    for (int i = 0; i != 21; i++)
      *d++ = *s++;
  }
  *d = 0xffff;
  nmi_load_bg_from_vram = 1;
}


// Generates a flat table of 24-bit SNES ROM pointers for each of the 398 dialogue messages,
// storing them in kTextDialoguePointers (3 bytes per entry, little-endian).
// Starts at ROM address 0x1C8000 and switches to 0xEDF40 at message 359 (a second bank).
// Each entry size is taken from FindIndexInMemblk. This function is kept only for WRAM
// layout compatibility with the original SNES binary; it is not called during gameplay.
void Text_GenerateMessagePointers() {  // 8ed3eb
  // This is not actually used. Only for ram compat.
  MemBlk dialogue = FindIndexInMemblk(g_zenv.dialogue_blk, 1);
  uint32 p = 0x1c8000;
  uint8 *dst = kTextDialoguePointers;
  for (int i = 0; i < 398; i++) {
    if (i == 359)
      p = 0xedf40;
    WORD(dst[0]) = p;
    dst[2] = p >> 16;
    dst += 3;
    p += (uint32)FindIndexInMemblk(dialogue, i).size + 1;
  }
}

// Dungeon map fade-in: Increments INIDISP_copy by one each frame.
// When full brightness (0xF) is reached, advances overworld_map_state to the marker phase.
void DungMap_LightenUpMap() {  // 8ed940
  if (++INIDISP_copy == 0xf)
    overworld_map_state++;
}

// Dungeon map phase 0: Fades out the game screen (one INIDISP step per frame).
// When fully dark, backs up all graphics state needed to restore the game afterward:
//   - HDMAEN, MOSAIC (set to 3 for the map display), TM/TS registers.
//   - All BG scroll registers (BG1-3 horizontal and vertical); zeros them for the map.
//   - CGWSEL: overrides blending for the map palette (CGWSEL = 2, CGADSUB = 0x20).
//   - Palette (full 512-byte copy of main_palette_buffer to mapbak_palette).
//   - BG1 scroll offsets as the camera position backup (mapbak_bg1_x/y_offset).
//   - link_dma_graphics_index set to 0x250 (map-specific Link sprite DMA index).
//   - messaging_buf filled with 0x300 (the blank map tile).
// Starts map music and sound, sets dungmap_init_state = 0 for the drawing sub-phase.
void DungMap_Backup() {  // 8ed94c
  if (--INIDISP_copy)
    return;
  MOSAIC_copy = 3;
  mapbak_HDMAEN = HDMAEN_copy;
  EnableForceBlank();
  overworld_map_state++;
  dungmap_init_state = 0;
  COLDATA_copy0 = 0x20;
  COLDATA_copy1 = 0x40;
  COLDATA_copy2 = 0x80;
  link_dma_graphics_index = 0x250;
  memcpy(mapbak_palette, main_palette_buffer, sizeof(uint16) * 256);
  mapbak_bg1_x_offset = bg1_x_offset;
  mapbak_bg1_y_offset = bg1_y_offset;
  bg1_x_offset = 0;
  bg1_y_offset = 0;
  mapbak_BG1HOFS_copy2 = BG1HOFS_copy2;
  mapbak_BG2HOFS_copy2 = BG2HOFS_copy2;
  mapbak_BG1VOFS_copy2 = BG1VOFS_copy2;
  mapbak_BG2VOFS_copy2 = BG2VOFS_copy2;
  BG1HOFS_copy2 = BG1VOFS_copy2 = 0;
  BG2HOFS_copy2 = BG2VOFS_copy2 = 0;
  BG3HOFS_copy2 = BG3VOFS_copy2 = 0;
  mapbak_CGWSEL = WORD(CGWSEL_copy);
  CGWSEL_copy = 0x02;
  CGADSUB_copy = 0x20;
  for (int i = 0; i < 2048; i++)
    messaging_buf[i] = 0x300;
  sound_effect_2 = 16;
  music_control = 0xf2;
}

// Dungeon map phase 6: Fades the map back to black (one INIDISP step per frame).
// When fully dark, enables force-blank and restores all backed-up scroll and CGWSEL
// registers (WORD(CGWSEL_copy), BG1-2 horizontal and vertical, BG3 zeroed),
// restores the camera offsets (bg1_x/y_offset), and triggers a CGRAM upload with
// flag_update_cgram_in_nmi++ so the restored palette becomes visible after fade-in.
void DungMap_FadeMapToBlack() {  // 8eda37
  if (--INIDISP_copy)
    return;
  EnableForceBlank();
  overworld_map_state++;
  WORD(CGWSEL_copy) = mapbak_CGWSEL;
  BG1HOFS_copy2 =  mapbak_BG1HOFS_copy2;
  BG2HOFS_copy2 =  mapbak_BG2HOFS_copy2;
  BG1VOFS_copy2 =  mapbak_BG1VOFS_copy2;
  BG2VOFS_copy2 =  mapbak_BG2VOFS_copy2;
  BG3VOFS_copy2 = BG3HOFS_copy2 = 0;
  bg1_x_offset = mapbak_bg1_x_offset;
  bg1_y_offset = mapbak_bg1_y_offset;
  flag_update_cgram_in_nmi++;
}

// Dungeon map phase 8: Fade-in and return to game. Calls OrientLampLightCone each frame
// to keep the torch-light HDMA table live while the screen brightens. Increments
// INIDISP_copy one step per frame. When full brightness (0xF) is reached, restores
// the game module (main_module_index = saved_module_for_menu, submodule_index = 0),
// resets overworld_map_state and subsubmodule_index, and restores the HDMA mask.
void DungMap_RestoreOld() {  // 8eda79
  OrientLampLightCone();
  if (++INIDISP_copy != 0xf)
    return;
  main_module_index = saved_module_for_menu;
  submodule_index = 0;
  overworld_map_state = 0;
  subsubmodule_index = 0;
  INIDISP_copy = 0xf;
  HDMAEN_copy = mapbak_HDMAEN;
}

// Drives the per-frame Link death swoon animation used during the game-over sequence.
// link_var30d indexes the current animation phase (0-14); kDeath_AnimCtr0/1 provide
// the step count and frame duration for each phase. Decrements some_animation_timer
// each frame; when it expires, advances to the next phase and resets the timer.
// Phase 14 triggers the phase-advance (submodule_index++) to exit the swoon loop.
// Phase 13 draws the "Link fades out" sprite (tile 0xaa at Link's screen position)
// once Link reaches link_visibility_status == 12 (fully invisible). The sprite uses
// kDeath_SprFlags to select the correct priority bit for upper/lower level.
void Death_PlayerSwoon() {  // 8ff5e3
  int k = link_var30d;
  if (sign8(--some_animation_timer)) {
    k++;
    if (k == 15)
      return;
    if (k == 14)
      submodule_index++;
    link_var30d = k;
    some_animation_timer_steps = kDeath_AnimCtr0[k];
    some_animation_timer = kDeath_AnimCtr1[k];
  }
  if (k != 13 || link_visibility_status == 12)
    return;
  uint8 y = link_y_coord + 16 - BG2VOFS_copy2;
  uint8 x = link_x_coord + 7 - BG2HOFS_copy2;
  SetOamPlain(&oam_buf[0x74], x, y, 0xaa, kDeath_SprFlags[link_is_on_lower_level] | 2, 2);
}

// Resets all of Link's state variables at the start of the death animation.
// Sets facing direction to down (2), flags Link as incapacitated, resets the
// swoon animation index and timer, zeros hearts and health, and clears a broad set
// of movement/combat/status flags: speed, somaria platform, water ripples, bunny form,
// drag state, ancilla-pickup flag, auxiliary state, incapacitation timer, damage,
// transform flag, poof flag, temp-bunny timer.
// If Link has the Moon Pearl, also clears the bunny-forced flag.
// kFeatures0_MiscBugFixes: forces a palette reload so Link's sprite is correctly coloured
// during the death animation even when dying as a permanent bunny.
// Plays the death SFX (0x27) with pan calculated from Link's world X position.
// Finally, if any bottle holds a fairy (link_bottle_info[i] == 6), returns early without
// clearing the changeable-dungeon-object indices (the fairy revival path takes precedence).
void Death_PrepFaint() {  // 8ffa6f
  link_direction_facing = 2;
  player_unk1 = 1;
  link_var30d = 0;
  some_animation_timer_steps = 0;
  some_animation_timer = 5;
  link_hearts_filler = 0;
  link_health_current = 0;
  Link_ResetProperties_C();
  player_on_somaria_platform = 0;
  draw_water_ripples_or_grass = 0;
  link_is_bunny_mirror = 0;
  bitmask_of_dragstate = 0;
  flag_is_ancilla_to_pick_up = 0;
  link_auxiliary_state = 0;
  link_incapacitated_timer = 0;
  link_give_damage = 0;
  link_is_transforming = 0;
  link_speed_setting = 0;
  link_need_for_poof_for_transform = 0;
  if (link_item_moon_pearl)
    link_is_bunny = 0;
  link_timer_tempbunny = 0;
  //bugfix: dying as permabunny doesn't restore link palette during death animation
  if (enhanced_features0 & kFeatures0_MiscBugFixes)
    LoadActualGearPalettes();
  sound_effect_1 = 0x27 | Link_CalculateSfxPan();
  for (int i = 0; i != 4; i++) {
    if (link_bottle_info[i] == 6)
      return;
  }
  index_of_changable_dungeon_objs[0] = index_of_changable_dungeon_objs[1] = 0;
}



// Opens the "Select Item" Y-item picker text box (used when pressing Select with items equipped).
// Backs up choice_in_multiselect_box, sets dialogue_message_index = 0x186 (the Y-item
// picker message), calls Main_ShowTextMessage to initialise the text rendering pipeline,
// then restores main_module_index and configures the messaging sub-system:
//   subsubmodule_index = 0, submodule_index = 11 (text display submodule),
//   saved_module_for_menu = current module, main_module_index = 14 (Module 0x0E).
void DisplaySelectMenu() {
  choice_in_multiselect_box_bak = choice_in_multiselect_box;
  dialogue_message_index = 0x186;
  uint8 bak = main_module_index;
  Main_ShowTextMessage();
  main_module_index = bak;
  subsubmodule_index = 0;
  submodule_index = 11;
  saved_module_for_menu = main_module_index;
  main_module_index = 14;
}
