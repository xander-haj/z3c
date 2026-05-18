/*
 * misc.h - Module routing, boss victory sequences, and utility functions
 *
 * A catch-all header that declares the game's top-level module dispatcher,
 * boss fight victory sequences, sound effect helpers, dungeon lighting
 * utilities, and various other functions that do not belong to a single
 * subsystem. In the original SNES code, these lived in a shared bank of
 * miscellaneous routines.
 *
 * Key responsibilities:
 *   - Module routing: Module_MainRouting() is the central dispatcher that
 *     selects which game module runs each frame (overworld, dungeon, menu,
 *     file select, attract mode, etc.) based on the main_module_index.
 *   - Boss victory: pendant/crystal acquisition cutscenes after defeating a
 *     dungeon boss, including the victory spin, heal sequence, and fade out.
 *   - Sound helpers: stereo panning calculation for sound effects based on
 *     the source's screen position relative to Link.
 *   - Item receipt: the item-get fanfare animation and inventory update.
 *   - Song bank loading: swaps the SPC700 music bank for the current area.
 *
 * This header also provides inline utility functions (GetOamCurPtr,
 * FindInByteArray, FindInWordArray) used widely across the codebase.
 */
#include "types.h"
#include "variables.h"
#include "zelda_rtl.h"

#pragma once









// Returns a pointer to the current OAM write position in the RAM-shadowed
// OAM buffer. Sprites are written sequentially; oam_cur_ptr advances as
// each sprite system (Link, enemies, ancillae, HUD) adds its entries.
static inline OamEnt *GetOamCurPtr() {
  return (OamEnt *)&g_ram[oam_cur_ptr];
}

// Searches |data| (of |size| bytes) for the value |lookfor|, scanning from
// the end toward the beginning. Returns the index of the last match, or -1
// if not found. The reverse scan order matches the original SNES code's
// behavior where the highest-index match takes priority.
static inline int FindInByteArray(const uint8 *data, uint8 lookfor, size_t size) {
  for (size_t i = size; i--;)
    if (data[i] == lookfor)
      return (int)i;
  return -1;
}

// 16-bit variant of FindInByteArray. Searches |data| (of |size| words) for
// |lookfor|, returning the index of the last match or -1 if not found.
static inline int FindInWordArray(const uint16 *data, uint16 lookfor, size_t size) {
  for (size_t i = size; i--;)
    if (data[i] == lookfor)
      return (int)i;
  return -1;
}

// Lookup table mapping each of the 76 collectible item IDs to the WRAM
// address where possession of that item is tracked. Used by the item
// receipt system to write a "1" (or item count) to the correct save variable.
extern const uint16 kMemoryLocationToGiveItemTo[76];

const uint16 *SrcPtr(uint16 src);
uint8 Ancilla_Sfx2_Near(uint8 a);
void Ancilla_Sfx3_Near(uint8 a);
void LoadDungeonRoomRebuildHUD();
void Module_Unknown0();
void Module_Unknown1();
void Module_MainRouting();
void NMI_PrepareSprites();
void Sound_LoadIntroSongBank();
void LoadOverworldSongs();
void LoadDungeonSongs();
void LoadCreditsSongs();
void Dungeon_LightTorch();
void RoomDraw_AdjustTorchLightingChange(uint16 x, uint16 y, uint16 r8);
int Dungeon_PrepOverlayDma_nextPrep(int dst, uint16 r8);
int Dungeon_PrepOverlayDma_watergate(int dst, uint16 r8, uint16 r6, int loops);
void Module05_LoadFile();
void Module13_BossVictory_Pendant();
void BossVictory_Heal();
void Dungeon_StartVictorySpin();
void Dungeon_RunVictorySpin();
void Dungeon_CloseVictorySpin();
void Module15_MirrorWarpFromAga();
void Module16_BossVictory_Crystal();
void Module16_04_FadeAndEnd();
void TriforceRoom_LinkApproachTriforce();
void AncillaAdd_ItemReceipt(uint8 ain, uint8 yin, int chest_pos);
void ItemReceipt_GiveBottledItem(uint8 item);
void Module17_SaveAndQuit();
void WallMaster_SendPlayerToLastEntrance();
uint8 GetRandomNumber();
uint8 Link_CalculateSfxPan();
void SpriteSfx_QueueSfx1WithPan(int k, uint8 a);
void SpriteSfx_QueueSfx2WithPan(int k, uint8 a);
void SpriteSfx_QueueSfx3WithPan(int k, uint8 a);
uint8 Sprite_CalculateSfxPan(int k);
uint8 CalculateSfxPan(uint16 x);
uint8 CalculateSfxPan_Arbitrary(uint8 a);
void Init_LoadDefaultTileAttr();
void Main_ShowTextMessage();
uint8 HandleItemTileAction_Overworld(uint16 x, uint16 y);
