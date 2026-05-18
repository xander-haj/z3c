/*
 * select_file.c — File Select Screen, Save Slot Management, and Name Entry
 *
 * Part of the Zelda 3 C reimplementation (The Legend of Zelda: A Link to the Past).
 *
 * This file implements the complete save file management UI shown before gameplay
 * begins. It handles four distinct sub-modules:
 *
 *   Module 01 — File Select:  Three-slot save file browser with fairy cursor,
 *               player name display, heart meter, equipment sprites, and death
 *               counter. Validated save files show Link's equipment visually.
 *
 *   Module 02 — Copy File:    Two-phase slot picker (source -> target) with
 *               confirmation prompt. Copies the full 0x500-byte SRAM block.
 *
 *   Module 03 — Kill File:    Single-phase slot picker with confirmation. Zeros
 *               both the primary and backup SRAM regions for the selected slot.
 *
 *   Module 04 — Name File:    Scrollable character grid for naming a new save.
 *               Supports uppercase, lowercase, and special characters. On
 *               confirmation, initializes a fresh SRAM block with default
 *               equipment and writes the checksum.
 *
 * SRAM layout: Each save slot occupies 0x500 bytes of SRAM. The primary copy
 * lives at offset k*0x500, and a backup copy lives at k*0x500 + 0xF00.
 * A 16-bit additive checksum (target value 0x5A5A) protects each copy.
 * On boot, Intro_ValidateSram checks both copies and restores from backup
 * if the primary is corrupted.
 *
 * All tilemap data in this file targets VRAM via the NMI transfer buffer
 * (vram_upload_data). Tile values follow the SNES BG tilemap format:
 * bits 0-9 = character number, bits 10-12 = palette, bit 13 = priority,
 * bit 14 = horizontal flip, bit 15 = vertical flip.
 *
 * Related files:
 *   select_file.h  — Public declarations for all functions in this file
 *   variables.h    — SRAM mirror variables (kSrmOffs_* constants)
 *   load_gfx.h     — Tile decompression and VRAM transfer routines
 *   messaging.h    — DisplaySelectMenu (save/continue/quit dialog)
 *   overworld.h    — CopySaveToWRAM (loads save data into working RAM)
 */

// Core engine runtime layer (memory access macros, NMI flags, OAM helpers)
#include "zelda_rtl.h"
// SRAM field offset constants and game state variables
#include "variables.h"
// Tile decompression, palette loading, and VRAM transfer functions
#include "load_gfx.h"
// Public interface for this module's functions
#include "select_file.h"
// SNES hardware register address definitions (INIDISP, BG scroll, etc.)
#include "snes/snes_regs.h"
// CopySaveToWRAM — copies a save slot from SRAM into the WRAM working area
#include "overworld.h"
// DisplaySelectMenu — shows the save/continue/quit confirmation dialog
#include "messaging.h"
// SetOamPlain — writes a single OAM entry for sprite rendering
#include "sprite.h"

// --- Working registers stored in general-purpose RAM ---
// These aliases map named registers to specific g_ram bytes, functioning as
// module-local state that persists across frames within the file select UI.
#define selectfile_R16 g_ram[0xc8]    // General-purpose cursor index / selection state
#define selectfile_R17 g_ram[0xc9]    // Secondary state flag (used in file load flow)
#define selectfile_R18 WORD(g_ram[0xca])  // 16-bit: target slot index for copy operation
#define selectfile_R20 WORD(g_ram[0xcc])  // 16-bit: source slot index for copy operation

// Screen Y-coordinates for each of the three save file slots on the file select screen.
// Slots are spaced 0x20 pixels apart vertically, starting at Y=0x43.
static const uint8 kSelectFile_Draw_Y[3] = {0x43, 0x63, 0x83};
/*
 * Intro_CheckCksum — Verify the integrity of a save slot's SRAM block.
 *
 * Computes a 16-bit additive checksum over the first 0x500 bytes (0x280 words)
 * of the save slot. The last word in the block is the checksum complement,
 * chosen so that the total sum equals the magic value 0x5A5A. If the sum
 * does not match, the data has been corrupted (dead battery, interrupted write).
 *
 * @param s  Pointer to the start of a 0x500-byte SRAM save block
 * @return   true if checksum validates, false if data is corrupted
 */
bool Intro_CheckCksum(const uint8 *s) {
  const uint16 *src = (const uint16 *)s;
  uint16 sum = 0;
  // Sum all 0x280 16-bit words in the save block, including the checksum word
  for (int i = 0; i < 0x280; i++)
    sum += src[i];
  // A valid save block sums to exactly 0x5A5A (the "ZZ" magic constant)
  return sum == 0x5a5a;

}

/*
 * SelectFile_Func1 — Generate the background tilemap for the file select screen.
 *
 * Fills the VRAM upload buffer at g_ram[0x1002] with a 32x32 tile pattern using
 * four alternating decorative tiles. The pattern creates the ornamental border
 * background behind the three save file slots.
 *
 * @return  Pointer past the end of the generated tilemap data, so the caller
 *          can append additional tile commands after the background.
 */
uint16 *SelectFile_Func1() {
  // Four-tile pattern used for the decorative file select background.
  // Tiles alternate horizontally (bit 0) and vertically (bit 5) to create
  // a repeating 2x2 checkerboard motif.
  static const uint16 kSelectFile_Func1_Tab[4] = {0x3581, 0x3582, 0x3591, 0x3592};
  uint16 *dst = (uint16 *)&g_ram[0x1002];
  // VRAM destination address for the background tilemap
  *dst++ = 0x10;
  // Transfer header: 0xFF07 encodes length and transfer mode for NMI DMA
  *dst++ = 0xff07;
  for (int i = 0; i < 1024; i++)
    // Select one of 4 tiles based on column parity (bit 0) and row parity (bit 5)
    *dst++ = kSelectFile_Func1_Tab[((i & 0x20) >> 4) + (i & 1)];
  return dst;
}

/*
 * SelectFile_Func5_DrawOams — Draw Link's equipment sprites for a save slot.
 *
 * Renders five OAM sprites next to the player name in a file select slot:
 *   - Sword (2 sprites: top and bottom halves, hidden if no sword owned)
 *   - Shield (1 sprite, hidden if no shield owned)
 *   - Link body (2 sprites: upper and lower halves of Link's standing pose)
 *
 * Each save slot uses a different OAM index range and palette flags to keep
 * the three slots visually distinct and avoid OAM conflicts.
 *
 * @param k  Save slot index (0, 1, or 2)
 */
void SelectFile_Func5_DrawOams(int k) {
  // OAM buffer byte offsets for each slot's first sprite (divided by 4 for indexing)
  static const uint8 kSelectFile_Draw_OamIdx[3] = {0x28, 0x3c, 0x50};
  // Sword tile characters indexed by sword level (0=no sword uses fighter's)
  static const uint8 kSelectFile_Draw_SwordChar[4] = {0x85, 0xa1, 0xa1, 0xa1};
  // Shield tile characters for each shield upgrade level
  static const uint8 kSelectFile_Draw_ShieldChar[3] = {0xc4, 0xca, 0xe0};
  // OAM attribute flags per slot — encode palette and priority for sword sprites
  static const uint8 kSelectFile_Draw_Flags[3] = {0x72, 0x76, 0x7a};
  // OAM attribute flags per slot — for shield sprite
  static const uint8 kSelectFile_Draw_Flags2[3] = {0x32, 0x36, 0x3a};
  // OAM attribute flags per slot — for Link body sprites
  static const uint8 kSelectFile_Draw_Flags3[3] = {0x30, 0x34, 0x38};

  // Load the sprite graphics for Link's standing pose into the DMA queue
  link_dma_graphics_index = 0x116 * 2;
  uint8 *sram = g_zenv.sram + 0x500 * k;

  OamEnt *oam = oam_buf + kSelectFile_Draw_OamIdx[k] / 4;
  uint8 x = 0x34;
  uint8 y = kSelectFile_Draw_Y[k];

  // Sword sprites: subtract 1 to convert from 1-based SRAM value to 0-based index.
  // sign8() checks if the result underflowed (player has no sword), using index 0 as fallback.
  uint8 sword = sram[kSrmOffs_Sword] - 1;
  uint8 swordchar = kSelectFile_Draw_SwordChar[sign8(sword) ? 0 : sword];
  SetOamPlain(oam + 0, x + 0xc, y - 5, swordchar, kSelectFile_Draw_Flags[k], 0);
  // Bottom half of sword is 16 tiles later in the character table
  SetOamPlain(oam + 1, x + 0xc, y + 3, swordchar + 16, kSelectFile_Draw_Flags[k], 0);
  if (sign8(sword))
    // Move sprites off-screen (Y=0xF0) to hide them when player has no sword
    oam[1].y = oam[0].y = 0xf0;

  // Shield sprite: same underflow-check pattern as the sword
  uint8 shield = sram[kSrmOffs_Shield] - 1;
  SetOamPlain(oam + 2, x - 5, y + 10, kSelectFile_Draw_ShieldChar[sign8(shield) ? 0 : shield], kSelectFile_Draw_Flags2[k], 2);
  if (sign8(shield))
    oam[2].y = 0xf0;

  // Link body: top half (char 0) and bottom half (char 2, vertically flipped via flag 0x40)
  SetOamPlain(oam + 3, x, y + 0, 0, kSelectFile_Draw_Flags3[k], 2);
  SetOamPlain(oam + 4, x, y + 8, 2, kSelectFile_Draw_Flags3[k] | 0x40, 2);
}

/*
 * SelectFile_Func6_DrawOams2 — Draw the death counter digits for a save slot.
 *
 * Renders the player's death count as up to three decimal digit sprites
 * positioned below Link's portrait in the file select slot. Leading zeros
 * are suppressed (e.g., "7" not "007"). A counter value of 0xFFFF means
 * the file is freshly created and no deaths have been recorded yet, so
 * nothing is drawn.
 *
 * @param k  Save slot index (0, 1, or 2)
 */
void SelectFile_Func6_DrawOams2(int k) {
  // Sprite tile characters for digits 0-9 in the file select font
  static const uint8 kSelectFile_DrawDigit_Char[10] = {0xd0, 0xac, 0xad, 0xbc, 0xbd, 0xae, 0xaf, 0xbe, 0xbf, 0xc0};
  // OAM byte offsets for each slot's digit sprites
  static const int8 kSelectFile_DrawDigit_OamIdx[3] = {4, 16, 28};
  // Horizontal pixel offsets for hundreds, tens, ones digits (right-to-left)
  static const int8 kSelectFile_DrawDigit_X[3] = {12, 4, -4};

  uint8 *sram = g_zenv.sram + 0x500 * k;
  uint8 x = 0x34;
  uint8 y = kSelectFile_Draw_Y[k];

  // 0xFFFF is the sentinel for "no deaths recorded" (new save file)
  int died_ctr = WORD(sram[kSrmOffs_DiedCounter]);
  if (died_ctr == 0xffff)
    return;

  // Clamp to 999 because only three digit sprites are available
  if (died_ctr > 999)
    died_ctr = 999;

  // Decompose into individual decimal digits
  uint8 digits[3];
  digits[2] = died_ctr / 100;
  died_ctr %= 100;
  digits[1] = died_ctr / 10;
  digits[0] = died_ctr % 10;

  // Determine the most significant non-zero digit to suppress leading zeros
  int i = (digits[2] != 0) ? 2 : (digits[1] != 0) ? 1 : 0;
  OamEnt *oam = oam_buf + kSelectFile_DrawDigit_OamIdx[k] / 4;
  // Draw digits from most significant to least significant
  do {
    SetOamPlain(oam, x + kSelectFile_DrawDigit_X[i], y + 0x10, kSelectFile_DrawDigit_Char[digits[i]], 0x3c, 0);
  } while (oam++, --i >= 0);
}

/*
 * SelectFile_Func17 — Draw the player name and heart meter for a save slot.
 *
 * Writes the 6-character player name into the VRAM upload buffer as BG tilemap
 * entries (each character is a 2-tile-high glyph, top row and bottom row offset
 * by 0x10 in the tile table). Then draws filled heart icons representing the
 * player's maximum health capacity.
 *
 * The VRAM upload buffer is shared across all three slots, with each slot
 * writing to a different offset to avoid overlap.
 *
 * @param k  Save slot index (0, 1, or 2)
 */
void SelectFile_Func17(int k) {
  // VRAM upload buffer offsets for each slot's name tile region
  static const uint16 kSelectFile_DrawName_VramOffs[3] = {8, 0x5c, 0xb0};
  // VRAM upload buffer offsets for each slot's health display region
  static const uint16 kSelectFile_DrawName_HealthVramOffs[3] = {0x16, 0x6a, 0xbe};
  uint8 *sram = g_zenv.sram + 0x500 * k;
  uint16 *name = (uint16 *)(sram + kSrmOffs_Name);
  uint16 *dst = vram_upload_data + kSelectFile_DrawName_VramOffs[k] / 2;
  for (int i = 5; i >= 0; i--) {
    // 0x1800 sets the palette bits for name text; +0x10 gets the bottom half glyph.
    // dst[21] targets the row below (21 words = one tilemap row in the upload format).
    uint16 t = *name++ + 0x1800;
    dst[0] = t;
    dst[21] = t + 0x10;
    dst++;
  }

  // Draw heart containers: each heart is one tile (0x520 = filled heart tile).
  // Health is stored in 1/8-heart units, so >>3 gives the number of whole hearts.
  int health = sram[kSrmOffs_Health] >> 3;
  dst = vram_upload_data + kSelectFile_DrawName_HealthVramOffs[k] / 2;
  uint16 *dst_org = dst;
  // Hearts wrap to the next row after 10 hearts (max 20 hearts = 2 rows of 10)
  int row = 10;
  do {
    *dst++ = 0x520;
    if (--row == 0)
      dst = dst_org + 21;
  } while (--health);
}

/*
 * SelectFile_Func16 — Handle the "Are you sure?" confirmation for file deletion.
 *
 * Presents two options (Yes / No) with a fairy cursor. If the player confirms
 * (selectfile_R16 == 0 means "Yes"), the save slot's SRAM is erased — both
 * the primary block (k * 0x500) and the backup block (k * 0x500 + 0xF00) are
 * zeroed, and the changes are flushed to persistent storage.
 *
 * This function is called from KILLFile_HandleConfirmation.
 */
void SelectFile_Func16() {
  // Y positions for the two confirmation options (Yes at 175, No at 191)
  static const uint8 kSelectFile_Func16_FaerieY[2] = {175, 191};
  FileSelect_DrawFairy(0x1c, kSelectFile_Func16_FaerieY[selectfile_R16]);

  // Handle D-pad up/down to toggle between Yes (0) and No (1)
  int k = selectfile_R16;
  if (filtered_joypad_H & 0x2c) {
    // 0x24 = down+right bits; any directional press toggles the binary choice
    k += (filtered_joypad_H & 0x24) ? 1 : -1;
    selectfile_R16 = k & 1;
    sound_effect_2 = 0x20;  // Cursor move sound
  }

  // Check for A/Start/X button press (confirm selection)
  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0;
  if (a != 0) {
    sound_effect_1 = 0x2c;
    if (selectfile_R16 == 0) {
      // Player chose "Yes" — erase the save file
      sound_effect_2 = 0x22;  // Erase confirmation sound
      sound_effect_1 = 0x0;
      int k = subsubmodule_index;
      selectfile_arr1[k] = 0;  // Mark slot as empty in the active flags array
      // Zero both the primary save block and its backup copy
      memset(g_zenv.sram + k * 0x500, 0, 0x500);
      memset(g_zenv.sram + k * 0x500 + 0xf00, 0, 0x500);
      ZeldaWriteSram();
    }
    ReturnToFileSelect();
    subsubmodule_index = 0;
  }
}

/*
 * Module_NamePlayer_1 — Generate the background tilemap for the name entry screen.
 *
 * Calls SelectFile_Func1 to fill the background pattern, appends a terminator
 * (0xFFFF), and queues the tilemap for NMI transfer to VRAM.
 */
void Module_NamePlayer_1() {
  uint16 *dst = SelectFile_Func1();
  dst[0] = 0xffff;  // End-of-data sentinel for the NMI VRAM transfer routine
  nmi_load_bg_from_vram = 1;
  submodule_index++;
}

/*
 * Module_NamePlayer_2 — Finalize screen setup for the name entry screen.
 *
 * Enables display at full brightness (INIDISP = 15), re-enables NMI core
 * updates, and triggers VRAM transfer mode 5 (character grid overlay).
 */
void Module_NamePlayer_2() {
  nmi_load_bg_from_vram = 5;
  submodule_index++;
  INIDISP_copy = 15;  // Full brightness, screen visible
  nmi_disable_core_updates = 0;
}

/*
 * Intro_FixCksum — Recompute and store the checksum for a save slot's SRAM block.
 *
 * Sums the first 0x27F words (all data except the checksum word itself), then
 * writes the complement value into the last word so that the total sum across
 * all 0x280 words equals the magic constant 0x5A5A. This must be called after
 * any modification to save data (name entry, copy, equipment changes).
 *
 * @param s  Pointer to the start of the 0x500-byte SRAM save block to fix
 */
void Intro_FixCksum(uint8 *s) {
  uint16 *src = (uint16 *)s;
  uint16 sum = 0;
  // Sum all words except the final checksum word at index 0x27F
  for (int i = 0; i < 0x27f; i++)
    sum += src[i];
  // Store the complement so that total sum == 0x5A5A
  src[0x27f] = 0x5a5a - sum;
}

/*
 * LoadFileSelectGraphics — Load all tile graphics needed for the file select screen.
 *
 * Decompresses three sprite graphics packs from ROM into a temporary RAM buffer,
 * then converts them from 3bpp to 4bpp format and copies to VRAM:
 *   - Pack 0x5E → VRAM 0x5000: File select UI tiles (borders, decorations)
 *   - Pack 0x5F → VRAM 0x5400: Equipment icon tiles (swords, shields, Link)
 *   - Pack 0x6B → VRAM 0x7800: Additional UI tiles (copied raw, already 4bpp)
 * Also loads the font tileset used for player names and menu text.
 *
 * Original SNES address: $80:E4E9
 */
void LoadFileSelectGraphics() {  // 80e4e9
  // Decompress sprite pack 0x5E to temp buffer, then convert 3bpp→4bpp into VRAM
  Decomp_spr(&g_ram[0x14000], 0x5e);
  Do3To4High(&g_zenv.vram[0x5000], &g_ram[0x14000]);

  // Decompress sprite pack 0x5F (equipment icons) into the next VRAM region
  Decomp_spr(&g_ram[0x14000], 0x5f);
  Do3To4High(&g_zenv.vram[0x5400], &g_ram[0x14000]);

  // Load the text font tiles (used for player names and menu labels)
  TransferFontToVRAM();

  // Pack 0x6B is already in 4bpp format, so copy directly (0x300 words = 0x600 bytes)
  Decomp_spr(&g_ram[0x14000], 0x6b);
  memcpy(&g_zenv.vram[0x7800], &g_ram[0x14000], 0x300 * sizeof(uint16));
}

/*
 * Intro_ValidateSram — Validate all three save slots on game boot.
 *
 * The SNES cartridge maintains two copies of each save file: a primary copy
 * at offset k*0x500 and a backup at k*0x500 + 0xF00. This dual-copy scheme
 * protects against data loss from interrupted writes (e.g., power loss during
 * save). The validation logic for each slot is:
 *
 *   1. If primary checksum is valid → slot is good, no action needed.
 *   2. If primary is corrupted but backup is valid → restore from backup.
 *   3. If both copies are corrupted → zero both regions (slot becomes empty).
 *
 * After validation, clears 768 bytes at g_ram[0xD00] which are used as
 * scratch space for the file select UI state flags.
 *
 * Original SNES address: $82:8054
 */
void Intro_ValidateSram() {  // 828054
  uint8 *cart = g_zenv.sram;
  for (int i = 0; i < 3; i++) {
    uint8 *c = cart + i * 0x500;
    if (!Intro_CheckCksum(c)) {
      if (Intro_CheckCksum(c + 0xf00)) {
        // Primary corrupted, backup valid — restore from backup
        memcpy(c, c + 0xf00, 0x500);
      } else {
        // Both copies corrupted — wipe the slot entirely
        memset(c, 0, 0x500);
        memset(c + 0xf00, 0, 0x500);
      }
    }
  }
  // Clear the file select UI scratch area (selectfile_arr1, flags, etc.)
  memset(&g_ram[0xd00], 0, 256 * 3);
}

// =============================================================================
// Module 01: File Select — Main state machine
// =============================================================================

/*
 * Module01_FileSelect — Top-level dispatcher for the file select screen.
 *
 * Resets BG3 scroll offsets each frame (the file select screen does not scroll),
 * then dispatches to the current sub-state. The sub-module flow is:
 *   0 → Hardware init, graphics load, SRAM validation
 *   1 → Reset UI flags, clear Triforce display
 *   2 → Build the slot border tilemap
 *   3 → Stripe-wipe transition effect
 *   4 → Name overlay stripe-wipe
 *   5 → Main selection loop (fairy cursor, input, slot loading)
 *
 * Original SNES address: $8C:CD7D
 */
void Module01_FileSelect() {  // 8ccd7d
  BG3HOFS_copy2 = 0;
  BG3VOFS_copy2 = 0;
  switch (submodule_index) {
  case 0: Module_SelectFile_0(); break;
  case 1: FileSelect_ReInitSaveFlagsAndEraseTriforce(); break;
  case 2: Module_EraseFile_1(); break;
  case 3: FileSelect_TriggerStripesAndAdvance(); break;
  case 4: FileSelect_TriggerNameStripesAndAdvance(); break;
  case 5: FileSelect_Main(); break;
  }
}

/*
 * Module_SelectFile_0 — One-shot initialization for the file select screen.
 *
 * Performs full hardware and state setup:
 *   1. Blanks the screen during setup to prevent visual glitches
 *   2. Starts the file select music (track 11)
 *   3. Loads palettes for the dungeon color scheme (used as the UI theme)
 *   4. Loads default tile graphics and the file-select-specific graphics
 *   5. Validates all three SRAM save slots (restoring from backup if needed)
 *   6. Decompresses enemy damage data (needed because the engine expects it loaded)
 *
 * Only called once per visit to the file select screen (submodule_index == 0).
 * Original SNES address: $8C:CD9D
 */
void Module_SelectFile_0() {  // 8ccd9d
  EnableForceBlank();
  is_nmi_thread_active = 0;
  nmi_flag_update_polyhedral = 0;
  music_control = 11;  // File select screen music track
  submodule_index++;
  // Use the auxiliary palette bank (0x200) for the file select color scheme
  overworld_palette_aux_or_main = 0x200;
  palette_main_indoors = 6;
  nmi_disable_core_updates = 6;
  Palette_Load_DungeonSet();
  Palette_Load_OWBG3();
  hud_palette = 0;
  Palette_Load_HUD();
  hud_cur_item = 0;
  // Tile theme indices for the file select decorative border tiles
  misc_sprites_graphics_index = 1;
  main_tile_theme_index = 35;
  aux_tile_theme_index = 81;
  LoadDefaultGraphics();
  InitializeTilesets();
  LoadFileSelectGraphics();
  Intro_ValidateSram();
  DecompressEnemyDamageSubclasses();
}

/*
 * FileSelect_ReInitSaveFlagsAndEraseTriforce — Reset file slot status flags.
 *
 * Clears the 6-byte selectfile_arr1 array that tracks which save slots contain
 * valid data (3 slots x 2 bytes). Then erases the Triforce decoration from the
 * background tilemap, which is left over from the attract/intro sequence.
 */
void FileSelect_ReInitSaveFlagsAndEraseTriforce() {  // 8ccdf2
  memset(selectfile_arr1, 0, 6);
  FileSelect_EraseTriforce();
}

/*
 * FileSelect_EraseTriforce — Clear the Triforce background and load file select palette.
 *
 * Used as a shared transition step when entering any sub-menu (file select, copy,
 * erase). Blanks the screen, clears the Triforce tilemap left by the intro sequence,
 * loads the file select color palette, and advances to the next sub-state.
 */
void FileSelect_EraseTriforce() {  // 8ccdf9
  nmi_disable_core_updates = 128;
  EnableForceBlank();
  EraseTileMaps_triforce();
  Palette_LoadForFileSelect();
  flag_update_cgram_in_nmi++;  // Queue palette update for next NMI
  submodule_index++;
}

/*
 * Module_EraseFile_1 — Build the file select slot border tilemap.
 *
 * Generates the decorative border frames around the three save slots using
 * a pre-built tile data array (kSelectFile_Gfx0). The border consists of
 * corner pieces, horizontal/vertical repeating tiles, and interior fill.
 * Then appends 18 rows of vertical divider columns and queues the combined
 * tilemap for NMI transfer to VRAM.
 *
 * Original SNES address: $8C:CE53
 */
void Module_EraseFile_1() {  // 8cce53
  // Pre-built VRAM upload commands for the three slot borders.
  // Format: pairs of (VRAM address, tile data) with RLE compression headers.
  static const uint8 kSelectFile_Gfx0[224] = {
    0x10, 0x42,    0, 0x27, 0x89, 0x35, 0x8a, 0x35, 0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35,
    0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35,
    0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35, 0x8a, 0x75, 0x89, 0x75, 0x10, 0x62,    0,    3,
    0x99, 0x35, 0x9a, 0x35, 0x10, 0x64, 0x40, 0x1e, 0x7f, 0x34, 0x10, 0x74,    0,    3, 0x9a, 0x75,
    0x99, 0x75, 0x10, 0x82,    0,    3, 0xa9, 0x35, 0xaa, 0x35, 0x10, 0x84, 0x40, 0x1e, 0x7f, 0x34,
    0x10, 0x94,    0,    3, 0xaa, 0x75, 0xa9, 0x75, 0x10, 0xa2,    0, 0x27, 0x9d, 0x35, 0xad, 0x35,
    0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35,
    0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35,
    0xad, 0x75, 0x9d, 0x75, 0x10, 0xc2,    0, 0x27, 0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35,
    0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35,
    0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35, 0xab, 0x75, 0xac, 0x75,
    0x10, 0xe2,    0,    1, 0x83, 0x35, 0x10, 0xe3, 0x40, 0x32, 0x85, 0x35, 0x10, 0xfd,    0,    1,
    0x84, 0x35, 0x11,    2, 0xc0, 0x22, 0x86, 0x35, 0x11, 0x1d, 0xc0, 0x22, 0x96, 0x35, 0x13, 0x42,
       0,    1, 0x93, 0x35, 0x13, 0x43, 0x40, 0x32, 0x95, 0x35, 0x13, 0x5d,    0,    1, 0x94, 0x35,
  };
  uint16 *dst = SelectFile_Func1();
  memcpy(dst, kSelectFile_Gfx0, 224);
  dst += 224 / 2;
  // Append 18 vertical stripe columns between save slots.
  // Each stripe is a VRAM address (byte-swapped for the transfer format),
  // followed by two tile values (0x3240 = left edge, 0x347f = blank fill).
  uint16 t = 0x1103;
  for (int i = 17; i >= 0; i--) {
    *dst++ = swap16(t);
    t += 0x20;  // Advance one tilemap row (32 tiles = 0x20 words)
    *dst++ = 0x3240;
    *dst++ = 0x347f;
  }
  *(uint8 *)dst = 0xff;  // End-of-data sentinel
  submodule_index++;
  nmi_load_bg_from_vram = 1;
}

/*
 * FileSelect_TriggerStripesAndAdvance — Trigger the horizontal stripe wipe effect.
 *
 * Restores the cursor position from the last-visited slot (selectfile_var2),
 * advances to the next sub-state, and triggers NMI VRAM transfer mode 6
 * which renders the stripe wipe transition animation.
 */
void FileSelect_TriggerStripesAndAdvance() {  // 8ccea5
  selectfile_R16 = selectfile_var2;
  submodule_index++;
  nmi_load_bg_from_vram = 6;
}

/*
 * FileSelect_TriggerNameStripesAndAdvance — Upload name row tilemaps and enable display.
 *
 * Copies pre-built VRAM upload data that draws the name display rows for all
 * three save slots. Each row consists of border edge tiles and blank interior
 * cells where the player name characters will be rendered. After upload,
 * enables full-brightness display and NMI core updates.
 *
 * Original SNES address: $8C:CEB1
 */
void FileSelect_TriggerNameStripesAndAdvance() {  // 8cceb1
  // Pre-built VRAM upload commands for the name display rows of all 3 slots.
  // Each slot gets two rows of tiles (top half and bottom half of the name glyphs).
  static const uint8 kSelectFile_Func3_Data[253] = {
    0x61, 0x29,    0, 0x25, 0xe7, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0x49,    0, 0x25, 0xf7, 0x18,
    0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0x61, 0xa9,    0, 0x25, 0xe8, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0xc9,
       0, 0x25, 0xf8, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x29,    0, 0x25, 0xe9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0x62, 0x49,    0, 0x25, 0xf9, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xff,
  };
  memcpy(vram_upload_data, kSelectFile_Func3_Data, 253);
  INIDISP_copy = 0xf;  // Full brightness, display enabled
  nmi_disable_core_updates = 0;  // Resume normal NMI processing
  submodule_index++;
  nmi_load_bg_from_vram = 6;  // Trigger stripe transition VRAM transfer
}

/*
 * FileSelect_Main — Core selection loop for the file select screen.
 *
 * Called every frame while the file select is active. This function:
 *   1. Scans SRAM to determine which save slots contain valid files
 *      (checked via the 0x55AA magic at offset 0x3E5 in each slot)
 *   2. Draws equipment sprites, death counters, and names for valid slots
 *   3. Renders the fairy cursor at the currently selected position
 *   4. Processes D-pad input to move the cursor between 5 positions:
 *      slots 0-2 (the three save files), position 3 (Copy), position 4 (Erase)
 *   5. On A/Start press:
 *      - If cursor is on an empty slot → jump to Module 04 (name entry)
 *      - If cursor is on a filled slot → fade music, load save into WRAM
 *      - If cursor is on Copy/Erase → jump to Module 02 or 03
 *      - If Copy/Erase chosen but no valid files exist → play error sound
 *
 * Original SNES address: $8C:CEBD
 */
void FileSelect_Main() {  // 8ccebd
  // Y positions for all 5 cursor stops: 3 file slots + Copy + Erase
  static const uint8 kSelectFile_Faerie_Y[5] = {0x4a, 0x6a, 0x8a, 0xaf, 0xbf};

  const uint8 *cart = g_zenv.sram;

  // Remember which file slot the cursor was last on (for returning from sub-menus)
  if (selectfile_R16 < 3)
    selectfile_var2 = selectfile_R16;

  // Check each save slot for the 0x55AA validity marker and draw its UI elements
  for (int k = 0; k < 3; k++) {
    if (*(uint16 *)(cart + k * 0x500 + 0x3E5) == 0x55AA) {
      selectfile_arr1[k] = 1;  // Mark slot as containing a valid save
      SelectFile_Func5_DrawOams(k);   // Equipment sprites (sword, shield, Link)
      SelectFile_Func6_DrawOams2(k);  // Death counter digits
      SelectFile_Func17(k);           // Player name and heart meter
    }
  }

  FileSelect_DrawFairy(0x1c, kSelectFile_Faerie_Y[selectfile_R16]);
  nmi_load_bg_from_vram = 1;

  // Read combined joypad state: L/R shoulder from joypad_L, D-pad/buttons from joypad_H
  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xfc;
  if (a & 0x2c) {
    // D-pad up/down/left/right: move the fairy cursor
    if (a & 8) {
      // Up pressed: move cursor upward, wrapping from slot 0 to Erase (position 4)
      sound_effect_2 = 0x20;
      if (sign8(--selectfile_R16))
        selectfile_R16 = 4;
    } else {
      // Down pressed: move cursor downward, wrapping from Erase back to slot 0
      sound_effect_2 = 0x20;
      if (++selectfile_R16 == 5)
        selectfile_R16 = 0;
    }
  } else if (a != 0) {
    // A/Start/X pressed: confirm selection
    sound_effect_1 = 0x2c;
    if (selectfile_R16 < 3) {
      // Cursor is on a file slot (0, 1, or 2)
      selectfile_R17 = 0;
      if (!selectfile_arr1[selectfile_R16]) {
        // Empty slot: jump to name entry (Module 04)
        main_module_index = 4;
        submodule_index = 0;
        subsubmodule_index = 0;
      } else {
        // Filled slot: fade out music and load the save file into WRAM
        music_control = 0xf1;  // Music fade-out command
        srm_var1 = selectfile_R16 * 2 + 2;
        WORD(g_ram[0]) = selectfile_R16 * 0x500;
        CopySaveToWRAM();
      }
    } else if (selectfile_arr1[0] | selectfile_arr1[1] | selectfile_arr1[2]) {
      // Copy (3) or Erase (4) selected, and at least one valid save exists
      main_module_index = (selectfile_R16 == 3) ? 2 : 3;
      selectfile_R16 = 0;
      submodule_index = 0;
      subsubmodule_index = 0;
    } else {
      // Copy/Erase selected but no valid saves exist — play error buzzer
      sound_effect_1 = 0x3c;
    }
  }
}

// =============================================================================
// Module 02: Copy File — Save slot duplication
// =============================================================================

/*
 * Module02_CopyFile — Top-level state machine for the copy file sub-menu.
 *
 * Flow: Clear screen → Build borders → Init display → Choose source slot →
 * Choose target slot → Confirm and execute copy.
 * The source/target selection skips empty slots automatically.
 *
 * Original SNES address: $8C:D053
 */
void Module02_CopyFile() {  // 8cd053
  selectfile_var2 = 0;
  switch (submodule_index) {
  case 0: FileSelect_EraseTriforce(); break;
  case 1: Module_EraseFile_1(); break;
  case 2: Module_CopyFile_2(); break;
  case 3: CopyFile_ChooseSelection(); break;
  case 4: CopyFile_ChooseTarget(); break;
  case 5: CopyFile_ConfirmSelection(); break;
  }
}

/*
 * Module_CopyFile_2 — Initialize the copy file display and cursor position.
 *
 * Enables the screen, triggers the copy file tilemap upload (mode 7), and
 * sets the cursor to the first non-empty save slot. The cursor skips empty
 * slots because you can only copy from a slot that contains a valid save.
 */
void Module_CopyFile_2() {  // 8cd06e
  nmi_load_bg_from_vram = 7;
  submodule_index++;
  INIDISP_copy = 0xf;
  nmi_disable_core_updates = 0;
  // Find the first occupied save slot to position the cursor on
  int i = 0;
  for (; selectfile_arr1[i] == 0; i++) {}
  selectfile_R16 = i;
}

/*
 * CopyFile_ChooseSelection — Copy submodule 3: pick the source save slot.
 *
 * Per-frame tick while the player is choosing which slot to copy *from*.
 * Delegates the actual UI + cursor handling to CopyFile_SelectionAndBlinker,
 * then periodically wipes the prompt header (every 0x30 frames, while still
 * in submodule 3) so it blinks. nmi_load_bg_from_vram = 1 schedules the
 * vram_upload_data buffer for transfer at the next NMI.
 */
void CopyFile_ChooseSelection() {  // 8cd087
  CopyFile_SelectionAndBlinker();
  if (submodule_index == 3 && !(frame_counter & 0x30))
    FilePicker_DeleteHeaderStripe();
  nmi_load_bg_from_vram = 1;
}

/*
 * CopyFile_ChooseTarget — Copy submodule 4: pick the destination save slot.
 *
 * Symmetric to CopyFile_ChooseSelection but for the target slot (which
 * cannot be the source). The blinking header prompts the player to confirm.
 */
void CopyFile_ChooseTarget() {  // 8cd0a2
  CopyFile_TargetSelectionAndBlink();
  if (submodule_index == 4 && !(frame_counter & 0x30))
    FilePicker_DeleteHeaderStripe();
  nmi_load_bg_from_vram = 1;
}

/*
 * CopyFile_ConfirmSelection — Copy submodule 5: yes/no confirmation prompt.
 *
 * Final stage of the copy flow. The yes/no choice is driven by
 * CopyFile_HandleConfirmation; "yes" performs the SRAM block copy and
 * commits it to battery-backed save, "no" returns to the file select screen.
 */
void CopyFile_ConfirmSelection() {  // 8cd0b9
  CopyFile_HandleConfirmation();
  nmi_load_bg_from_vram = 1;
}

/*
 * FilePicker_DeleteHeaderStripe — Wipe the prompt banner so it blinks off.
 *
 * Overwrites two horizontal stripes inside the staged vram_upload_data
 * buffer (at byte offsets 4 and 0x1e) with 11 tiles of 0xa9, the
 * "blank space" tile in the file select tileset. Called every 0x30
 * frames from CopyFile_ChooseSelection / CopyFile_ChooseTarget so the
 * "Copy where to?" / "Pick file to copy" header flashes to draw the
 * player's eye.
 */
void FilePicker_DeleteHeaderStripe() {  // 8cd0c6
  static const uint16 kFilePicker_DeleteHeaderStripe_Dst[2] = {4, 0x1e};
  for (int j = 1; j >= 0; j--) {
    uint16 *dst = vram_upload_data + kFilePicker_DeleteHeaderStripe_Dst[j] / 2;
    for (int i = 0; i != 11; i++)
      dst[i] = 0xa9;
  }
}

/*
 * CopyFile_SelectionAndBlinker — Source-slot picker UI and input handler.
 *
 * Stages one frame's worth of VRAM data and processes player input for
 * choosing the source slot in a copy operation.
 *
 * Layout of the staged buffer:
 *   - Tab[173]   : Static header strip "Copy file" / "Which one?" plus the
 *                  three slot label cells, all in the canonical VWF-encoded
 *                  tile format used by this UI (each pair is tile + attribute).
 *   - For each valid (filled) save slot, the 6-character name from SRAM is
 *     pulled out of kSrmOffs_Name and written into the right portion of the
 *     slot row. Names are stored as VWF tile indices and bumped by 0x1800
 *     (palette + priority) before being plotted; dst[10] gets the second
 *     bitplane row of the same name 0x10 above it.
 *   - Tab1[73]   : The "highlighted source slot" overlay, appended at offset
 *                  26 only when the player commits to a slot.
 *
 * Input handling:
 *   - Up/Down (filtered_joypad_H bits 0x2c after &0xfc): move the cursor,
 *     skipping empty slots. Wrap behavior preserves the original SNES
 *     quirk where the cancel row (k=3) is always reachable, so the loop
 *     condition is `k != 3 && !selectfile_arr1[k]`.
 *   - A/B/X/Y (any other bit in `a`): commit the choice. k=3 means cancel
 *     and returns to the file select; otherwise the source slot index is
 *     stashed into selectfile_R20 (* 2 to convert to byte offset) and the
 *     submodule advances to the target-picker.
 *
 * The fairy cursor is drawn last via FileSelect_DrawFairy at the slot's
 * coordinates in kCopyFile_SelectionAndBlinker_FaerieX / Y.
 */
void CopyFile_SelectionAndBlinker() {  // 8cd13f
  static const uint8 kCopyFile_SelectionAndBlinker_Tab[173] = {
    0x61,    4,    0, 0x15, 0x85, 0x18, 0x26, 0x18,    7, 0x18, 0xaf, 0x18,    2, 0x18,    7, 0x18,
    0x6f, 0x18, 0x86, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0x24,    0, 0x15, 0x95, 0x18,
    0x36, 0x18, 0x17, 0x18, 0xbf, 0x18, 0x12, 0x18, 0x17, 0x18, 0x7f, 0x18, 0x96, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0x61, 0x67,    0,  0xf, 0xe7, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0x87,    0,  0xf, 0xf7, 0x18, 0x91, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0xc7,    0,  0xf,
    0xe8, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0x61, 0xe7,    0,  0xf, 0xf8, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0x62, 0x27,    0,  0xf, 0xe9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x47,    0,  0xf, 0xf9, 0x18, 0x91, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xff,
  };
  static const uint8 kCopyFile_SelectionAndBlinker_Tab1[73] = {
    0x61, 0x67, 0x40,  0xe, 0xa9,    0, 0x61, 0x87, 0x40,  0xe, 0xa9,    0, 0x61, 0xc7, 0x40,  0xe,
    0xa9,    0, 0x61, 0xe7, 0x40,  0xe, 0xa9,    0, 0x11, 0x30,    0,    1, 0x83, 0x35, 0x11, 0x31,
    0x40, 0x14, 0x85, 0x35, 0x11, 0x3c,    0,    1, 0x84, 0x35, 0x11, 0x50, 0xc0,  0xe, 0x86, 0x35,
    0x11, 0x5c, 0xc0,  0xe, 0x96, 0x35, 0x12, 0x50,    0,    1, 0x93, 0x35, 0x12, 0x51, 0x40, 0x14,
    0x95, 0x35, 0x12, 0x5c,    0,    1, 0x94, 0x35, 0xff,
  };
  static const uint16 kCopyFile_SelectionAndBlinker_Dst[3] = {0x3c, 0x64, 0x8c};
  static const uint8 kCopyFile_SelectionAndBlinker_FaerieX[4] = {36, 36, 36, 28};
  static const uint8 kCopyFile_SelectionAndBlinker_FaerieY[4] = {87, 111, 135, 191};

  vram_upload_offset = 0xac;
  memcpy(vram_upload_data, kCopyFile_SelectionAndBlinker_Tab, 173);

  for (int k = 0; k != 3; k++) {
    if (selectfile_arr1[k] & 1) {
      const uint16 *name = (uint16 *)(g_zenv.sram + 0x500 * k + kSrmOffs_Name);
      uint16 *dst = vram_upload_data + kCopyFile_SelectionAndBlinker_Dst[k] / 2;
      for (int i = 0; i != 6; i++) {
        uint16 t = *name++ + 0x1800;
        dst[0] = t;
        dst[10] = t + 0x10;
        dst++;
      }
    }
  }
  FileSelect_DrawFairy(kCopyFile_SelectionAndBlinker_FaerieX[selectfile_R16], kCopyFile_SelectionAndBlinker_FaerieY[selectfile_R16]);

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xfc;
  if (a & 0x2c) {
    uint8 k = selectfile_R16;
    if (a & 8) {
      do {
        if (--k < 0) {
          k = 3;
          break;
        }
      } while (!selectfile_arr1[k]);
    } else {
      do {
        k++;
        if (k >= 4)
          k = 0;
      } while (k != 3 && !selectfile_arr1[k]);
    }
    selectfile_R16 = k;
    sound_effect_2 = 0x20;
  } else if (a != 0) {
    sound_effect_1 = 0x2c;
    if (selectfile_R16 == 3) {
      ReturnToFileSelect();
      return;
    }
    selectfile_R20 = selectfile_R16 * 2;
    memcpy(vram_upload_data + 26, kCopyFile_SelectionAndBlinker_Tab1, 73);
    if (selectfile_R16 != 2) {
      uint16 *dst = vram_upload_data + selectfile_R16 * 6;
      dst[26] = 0x2762;
      dst[29] = 0x4762;
    }
    submodule_index++;
    selectfile_R16 = 0;
  }
}

/*
 * ReturnToFileSelect — Exit a sub-flow back to the main file select screen.
 *
 * Resets the module dispatch to Module 01 (File Select) submodule 1 (the
 * "build the UI" stage, not the entry-from-attract path) and zeroes the
 * cursor. Called from the cancel path of Copy / Kill / Name sub-flows, and
 * after a successful copy or erase commit. submodule_index = 1 (rather
 * than 0) skips the initial fade-in so the screen stays up.
 */
void ReturnToFileSelect() {  // 8cd22d
  main_module_index = 1;
  submodule_index = 1;
  subsubmodule_index = 0;
  selectfile_R16 = 0;
}

/*
 * CopyFile_TargetSelectionAndBlink — Target-slot picker UI and input handler.
 *
 * Same shape as CopyFile_SelectionAndBlinker but for the destination slot.
 *
 * The preamble loop builds selectfile_arr2 with the two slot byte-offsets
 * that are NOT the source: it walks t = 4, 2, 0 and skips the entry equal
 * to the previously chosen source (selectfile_R20). This gives the cursor
 * two valid target options plus the "cancel" row.
 *
 * Layout of the staged buffer:
 *   - Tab0[133]  : Static header strip and the two non-source slot rows.
 *   - For each non-source slot index k (in render order, j = 0 or 1):
 *     plots the slot's number tile at dst[0]/dst[10], then if the slot is
 *     occupied (selectfile_arr1[k]) also plots its 6-character name.
 *   - Tab2[49]   : "Copy here?" confirmation strip, appended only after
 *                  the player commits to a target slot.
 *
 * Input handling mirrors the source picker: Up/Down moves the cursor with
 * wraparound, any commit press selects the slot (or cancels on k=2 which
 * is the cancel row of this 3-row UI), and the chosen target's byte
 * offset is stashed into selectfile_R18 before advancing to the
 * confirmation submodule.
 */
void CopyFile_TargetSelectionAndBlink() {  // 8cd27b
  {
    int k = 1, t = 4;
    do {
      if (t != selectfile_R20)
        selectfile_arr2[k--] = t;
    } while ((t -= 2) >= 0);
  }

  static const uint8 kCopyFile_TargetSelectionAndBlink_Tab0[133] = {
    0x61, 0x51,    0, 0x15, 0x85, 0x18, 0x23, 0x18,  0xe, 0x18, 0xa9, 0x18, 0x26, 0x18,    7, 0x18,
    0xaf, 0x18,    2, 0x18,    7, 0x18, 0x6f, 0x18, 0x86, 0x18, 0x61, 0x71,    0, 0x15, 0x95, 0x18,
    0x33, 0x18, 0x1e, 0x18, 0xb9, 0x18, 0x36, 0x18, 0x17, 0x18, 0xbf, 0x18, 0x12, 0x18, 0x17, 0x18,
    0x7f, 0x18, 0x96, 0x18, 0x61, 0xb4,    0,  0xf, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0xd4,    0,  0xf, 0xa9, 0x18, 0x91, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x14,    0,  0xf,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0x62, 0x34,    0,  0xf, 0xa9, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xff,
  };
  static const uint8 kCopyFile_TargetSelectionAndBlink_Tab2[49] = {
    0x61, 0xb4, 0x40,  0xe, 0xa9,    0, 0x61, 0xd4, 0x40,  0xe, 0xa9,    0, 0x62, 0xc6,    0,  0xd,
       2, 0x18,  0xe, 0x18,  0xf, 0x18, 0x28, 0x18, 0xa9, 0x18,  0xe, 0x18,  0xa, 0x18, 0x62, 0xe6,
       0,  0xd, 0x12, 0x18, 0x1e, 0x18, 0x1f, 0x18, 0x38, 0x18, 0xa9, 0x18, 0x1e, 0x18, 0x1a, 0x18,
    0xff,
  };
  static const uint8 kCopyFile_TargetSelectionAndBlink_FaerieX[3] = {0x8c, 0x8c, 0x1c};
  static const uint8 kCopyFile_TargetSelectionAndBlink_FaerieY[3] = {0x67, 0x7f, 0xbf};
  static const uint16 kCopyFile_TargetSelectionAndBlink_Dst[2] = {0x38, 0x60};
  static const uint16 kCopyFile_TargetSelectionAndBlink_Tab1[3] = {0x18e7, 0x18e8, 0x18e9};
  memcpy(vram_upload_data, kCopyFile_TargetSelectionAndBlink_Tab0, 133);

  for (int k = 0, j = 0; k != 3; k++) {
    if (k * 2 == selectfile_R20)
      continue;

    uint16 *dst = vram_upload_data + kCopyFile_TargetSelectionAndBlink_Dst[j++] / 2;
    uint16 t = kCopyFile_TargetSelectionAndBlink_Tab1[k];
    dst[0] = t;
    dst[10] = t + 0x10;
    dst += 2;
    if (selectfile_arr1[k]) {
      const uint16 *name = (uint16 *)(g_zenv.sram + 0x500 * k + kSrmOffs_Name);
      for (int i = 0; i != 6; i++) {
        uint16 t = *name++ + 0x1800;
        dst[0] = t;
        dst[10] = t + 0x10;
        dst++;
      }
    }
  }

  vram_upload_offset = 132;

  FileSelect_DrawFairy(kCopyFile_TargetSelectionAndBlink_FaerieX[selectfile_R16], kCopyFile_TargetSelectionAndBlink_FaerieY[selectfile_R16]);

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xfc;
  if (a & 0x2c) {
    uint8 k = selectfile_R16;
    if (a & 8) {
      if (sign8(--k))
        k = 2;
    } else {
      if (++k >= 3)
        k = 0;
    }
    selectfile_R16 = k;
    sound_effect_2 = 0x20;
  } else if (a) {
    sound_effect_1 = 0x2c;
    if (selectfile_R16 == 2) {
      ReturnToFileSelect();
      selectfile_R16 = 0;
      return;
    }
    selectfile_R18 = selectfile_arr2[selectfile_R16];
    memcpy(vram_upload_data + 26, kCopyFile_TargetSelectionAndBlink_Tab2, 49);
    if (selectfile_R16 == 0) {
      uint16 *dst = vram_upload_data;
      dst[26] = 0x1462;
      dst[29] = 0x3462;
    }
    submodule_index++;
    selectfile_R16 = 0;
  }
}

/*
 * CopyFile_HandleConfirmation — Yes/no prompt and the actual copy commit.
 *
 * Draws the fairy cursor on either the "yes" (Y=0xaf) or "no" (Y=0xbf) row,
 * processes Up/Down to toggle between them, and on commit either:
 *   - selectfile_R16 == 0 ("yes"): memcpy the full 0x500-byte SRAM block
 *     from the source slot (selectfile_R20 >> 1) into the target slot
 *     (selectfile_R18 >> 1), mark the target slot as valid, and flush
 *     the change to battery-backed SRAM via ZeldaWriteSram.
 *   - selectfile_R16 == 1 ("no"): skip the copy.
 *
 * Either way the sub-flow exits via ReturnToFileSelect.
 */
void CopyFile_HandleConfirmation() {  // 8cd371
  static const uint8 kCopyFile_HandleConfirmation_FaerieY[2] = {0xaf, 0xbf};
  FileSelect_DrawFairy(0x1c, kCopyFile_HandleConfirmation_FaerieY[selectfile_R16]);

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xfc;
  if (a & 0x2c) {
    sound_effect_2 = 0x20;
    if (a & 0x24) {
      if (++selectfile_R16 >= 2)
        selectfile_R16 = 0;
    } else {
      if (sign8(--selectfile_R16))
        selectfile_R16 = 1;
    }
  } else if (a != 0) {
    sound_effect_1 = 0x2c;
    if (selectfile_R16 == 0) {
      memcpy(g_zenv.sram + (selectfile_R18 >> 1) * 0x500, g_zenv.sram + (selectfile_R20 >> 1) * 0x500, 0x500);
      selectfile_arr1[(selectfile_R18 >> 1)] = 1;
      ZeldaWriteSram();
    }
    ReturnToFileSelect();
    selectfile_R16 = 0;
  }
}

// =============================================================================
// Module 03: Kill File — Save slot erasure
// =============================================================================

/*
 * Module03_KILLFile — Top-level state machine for the erase file sub-menu.
 *
 * Flow: Clear screen → Build borders → Init display → Choose slot →
 * Confirm and execute erase. Unlike Copy, this is single-phase: the player
 * picks one slot and confirms. On commit, both the primary block at
 * k*0x500 and the backup block at k*0x500 + 0xF00 are zeroed.
 *
 * Original SNES address: $8C:D485
 */
void Module03_KILLFile() {  // 8cd485
  switch (submodule_index) {
  case 0: FileSelect_EraseTriforce(); break;
  case 1: Module_EraseFile_1(); break;
  case 2: KILLFile_SetUp(); break;
  case 3: KILLFile_HandleSelection(); break;
  case 4: KILLFile_HandleConfirmation(); break;
  }
}

/*
 * KILLFile_SetUp — Erase submodule 2: arm the display and seed the cursor.
 *
 * Schedules the erase-file tilemap upload (nmi_load_bg_from_vram = 8),
 * un-blanks the screen (INIDISP_copy = 0xf), re-enables the normal core
 * NMI updates, and parks the cursor on the first occupied save slot —
 * erasing an empty slot is a no-op so the cursor must skip them.
 */
void KILLFile_SetUp() {  // 8cd49a
  nmi_load_bg_from_vram = 8;
  submodule_index++;
  INIDISP_copy = 0xf;
  nmi_disable_core_updates = 0;
  int i = 0;
  for (; selectfile_arr1[i] == 0; i++) {}
  selectfile_R16 = i;
}

/*
 * KILLFile_HandleSelection — Erase submodule 3: pick the slot to erase.
 *
 * Caches the current cursor row into selectfile_var2 if it points at a
 * real save slot (rows 0..2) so other code can read "which slot is
 * highlighted" without re-deriving it. The actual UI + input handling
 * is in KILLFile_ChooseTarget.
 */
void KILLFile_HandleSelection() {  // 8cd49f
  if (selectfile_R16 < 3)
    selectfile_var2 = selectfile_R16;
  KILLFile_ChooseTarget();
  nmi_load_bg_from_vram = 1;
}

/*
 * KILLFile_HandleConfirmation — Erase submodule 4: yes/no prompt + erase.
 *
 * Thin wrapper that delegates to SelectFile_Func16 (the shared yes/no
 * handler that wipes both primary and backup SRAM blocks for the slot
 * stored in selectfile_var2) and schedules the next VRAM upload.
 */
void KILLFile_HandleConfirmation() {  // 8cd4b1
  SelectFile_Func16();
  nmi_load_bg_from_vram = 1;
}

/*
 * KILLFile_ChooseTarget — Slot picker UI + input for the erase sub-flow.
 *
 * Per-frame tick that builds the staged tilemap, draws each save slot's
 * equipment preview (via SelectFile_Func17 for filled slots), draws the
 * fairy cursor, and routes Up/Down/Commit input:
 *
 *   - Tab[253]  : Static border / slot frames for the erase screen.
 *                 Larger than the copy table because the erase UI shows
 *                 a full 6-row layout including the cancel row.
 *   - Tab2[101] : "Erase this file?" prompt strip, appended when the
 *                 player commits to a slot.
 *
 * Input:
 *   - Up/Down: cursor walks through occupied slots only, with the cancel
 *     row (k=3) always reachable so the player can back out.
 *   - filtered_joypad_H & 0xd0 commits (the bit pattern selects A/Y/Start
 *     style buttons specifically, distinct from B which cancels via
 *     ReturnToFileSelect when k=3).
 *
 * On commit (and k != 3), the highlighted row's blank-tile placeholder
 * is replaced with the active highlight tile (0x6762 / 0x8762 pair) to
 * give a confirmation cue, submodule advances to the yes/no prompt,
 * and subsubmodule_index is repurposed to remember which slot was
 * chosen (so SelectFile_Func16 knows which SRAM to zero).
 */
void KILLFile_ChooseTarget() {  // 8cd4ba
  static const uint8 kKILLFile_ChooseTarget_Tab[253] = {
    0x61, 0xa7,    0, 0x25, 0xe7, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0xc7,    0, 0x25, 0xf7, 0x18,
    0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0x62,    7,    0, 0x25, 0xe8, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x27,
       0, 0x25, 0xf8, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x67,    0, 0x25, 0xe9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0x62, 0x87,    0, 0x25, 0xf9, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xff,
  };
  static const uint8 kKILLFile_ChooseTarget_Tab2[101] = {
    0x61, 0xa7, 0x40, 0x24, 0xa9,    0, 0x61, 0xc7, 0x40, 0x24, 0xa9,    0, 0x62,    7, 0x40, 0x24,
    0xa9,    0, 0x62, 0x27, 0x40, 0x24, 0xa9,    0, 0x62, 0xc6,    0, 0x21,    4, 0x18, 0x21, 0x18,
       0, 0x18, 0x22, 0x18,    4, 0x18, 0xa9, 0x18, 0x23, 0x18,    7, 0x18, 0xaf, 0x18, 0x22, 0x18,
    0xa9, 0x18,  0xf, 0x18,  0xb, 0x18,    0, 0x18, 0x28, 0x18,    4, 0x18, 0x21, 0x18, 0x62, 0xe6,
       0, 0x21, 0x14, 0x18, 0x31, 0x18, 0x10, 0x18, 0x32, 0x18, 0x14, 0x18, 0xa9, 0x18, 0x33, 0x18,
    0x17, 0x18, 0xbf, 0x18, 0x32, 0x18, 0xa9, 0x18, 0x1f, 0x18, 0x1b, 0x18, 0x10, 0x18, 0x38, 0x18,
    0x14, 0x18, 0x31, 0x18, 0xff,
  };
  static const uint8 kKILLFile_ChooseTarget_FaerieX[4] = {36, 36, 36, 28};
  static const uint8 kKILLFile_ChooseTarget_FaerieY[4] = {103, 127, 151, 191};
  memcpy(vram_upload_data, kKILLFile_ChooseTarget_Tab, 253);
  for (int k = 0; k < 3; k++) {
    if (selectfile_arr1[k])
      SelectFile_Func17(k);
  }

  FileSelect_DrawFairy(kKILLFile_ChooseTarget_FaerieX[selectfile_R16], kKILLFile_ChooseTarget_FaerieY[selectfile_R16]);

  int k = selectfile_R16;
  if (filtered_joypad_H & 0x2c) {
    if (!(filtered_joypad_H & 0x24)) {
      do {
        if (--k < 0) {
          k = 3;
          break;
        }
      } while (!selectfile_arr1[k]);
    } else {
      do {
        k++;
        if (k >= 4)
          k = 0;
      } while (k != 3 && !selectfile_arr1[k]);
    }
    sound_effect_2 = 0x20;
  }
  selectfile_R16 = k;

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0;
  if (a) {
    sound_effect_1 = 0x2c;
    if (k == 3) {
      ReturnToFileSelect();
      return;
    }

    memcpy(vram_upload_data, kKILLFile_ChooseTarget_Tab2, 101);
    submodule_index++;
    if (selectfile_R16 != 2) {
      uint16 *dst = vram_upload_data + selectfile_R16 * 6;
      dst[0] = 0x6762;
      dst[3] = 0x8762;
    }
    subsubmodule_index = selectfile_R16;
    selectfile_R16 = 0;
  }
}

/*
 * FileSelect_DrawFairy — Render the fairy cursor sprite at (x, y).
 *
 * The fairy is the player's cursor across all file-select sub-flows.
 * Its tile alternates between 0xa8 and 0xaa every 8 frames
 * (frame_counter & 8) to fake a 2-frame wing-flap animation. The OAM
 * attribute byte 0x7e packs palette + priority + flip into one value
 * that produces the correct color/depth for this sprite, and size code 2
 * selects the 8x16 large-OAM slot. The fairy always occupies oam_buf[0],
 * so it sits at the head of the OAM list — drawn on top of everything.
 */
void FileSelect_DrawFairy(uint8 x, uint8 y) {  // 8cd7a5
  SetOamPlain(&oam_buf[0], x, y, frame_counter & 8 ? 0xaa : 0xa8, 0x7e, 2);
}

// =============================================================================
// Module 04: Name File — Player name entry for a new save
// =============================================================================

/*
 * Module04_NameFile — Top-level state machine for the player-name entry UI.
 *
 * Flow: Clear the chosen save slot → Build the character-grid display →
 * Animate the grid into view → Run the cursor + input loop until the
 * player commits a name.
 *
 * Entered when the player selects an empty slot from the main file select
 * screen. On successful name commit, the slot is initialized with default
 * inventory values and a valid checksum is written so subsequent boots
 * recognize it as a real save.
 *
 * Original SNES address: $8C:D88A
 */
void Module04_NameFile() {  // 8cd88a
  switch (submodule_index) {
  case 0: NameFile_EraseSave(); break;
  case 1: Module_NamePlayer_1(); break;
  case 2: Module_NamePlayer_2(); break;
  case 3: NameFile_DoTheNaming(); break;
  }
}

/*
 * NameFile_EraseSave — Name submodule 0: zero the chosen save slot.
 *
 * Wipes the entire 0x500-byte SRAM block for the slot the player picked
 * (offset = selectfile_R16 * 0x500) and pre-fills the 6-character name
 * field with the "blank space" tile (0xa9) so the grid starts empty.
 *
 * Also seeds the per-frame cursor / scroll state for the naming UI:
 *   selectfile_var3   — character grid X index (0..31)
 *   selectfile_var4   — name field character position (0..5)
 *   selectfile_var5   — character grid Y index (0..3, row)
 *   selectfile_var7   — current Y pixel of the grid (animated toward
 *                       kNamePlayer_Tab2[var5])
 *   selectfile_var8   — current X pixel of the grid (animated)
 *   selectfile_var9   — non-zero while an X-scroll animation is in flight
 *   selectfile_var10  — direction sign for the active X scroll
 *   selectfile_var11  — non-zero while a Y-scroll animation is in flight
 *
 * attract_legend_ctr is reused as the cached byte offset of this slot
 * within g_zenv.sram so the per-character draw functions can find the
 * right name destination without re-multiplying by 0x500 every frame.
 * irq_flag = 1 enables the BG3 scroll IRQ that drives the grid wrap
 * effect; it's turned off again on commit.
 */
void NameFile_EraseSave() {  // 8cd89c
  FileSelect_EraseTriforce();
  irq_flag = 1;
  selectfile_var3 = 0;
  selectfile_var4 = 0;
  selectfile_var5 = 0;
  selectfile_arr2[0] = 0;
  selectfile_var6 = 0;
  selectfile_var7 = 0x83;
  selectfile_var8 = 0x1f0;
  BG3HOFS_copy2 = 0;
  int offs = selectfile_R16 * 0x500;
  attract_legend_ctr = offs;
  memset(g_zenv.sram + offs, 0, 0x500);
  uint16 *name = (uint16 *)(g_zenv.sram + offs + kSrmOffs_Name);
  name[0] = name[1] = name[2] = name[3] = name[4] = name[5] = 0xa9;
}

/*
 * NameFile_DoTheNaming — Name submodule 3: the main character-grid loop.
 *
 * Per-frame driver for the player-name entry screen. Performs four jobs:
 *
 *   1. Drive the horizontal cursor scroll animation.
 *      kNamePlayer_Tab1 holds 13 step pairs (sign-toggled). selectfile_var9
 *      indexes through them and modulates selectfile_var8 (the grid X
 *      pixel) until it lands on kNamePlayer_Tab0[selectfile_var3]. The
 *      special j-values 0x30 / 0x31 implement the wrap-around ease so the
 *      grid snaps cleanly when scrolling past the row ends. The `j += 2`
 *      branch on `!selectfile_var10` swaps to the reverse step table for
 *      the opposite-direction segment of each scroll.
 *
 *   2. Drive the vertical row-change animation.
 *      selectfile_var7 is moved 2 pixels per frame toward
 *      kNamePlayer_Tab2[selectfile_var5] (one of 131, 147, 163, 179).
 *      `sign8(diff)` picks the direction so it always converges.
 *
 *   3. Draw 26 hard-spaces of the character grid plus the name-field
 *      cursor sprite. The 26-sprite strip is the visible row of the
 *      scrolling character grid; the extra sprite at
 *      kNamePlayer_X[selectfile_var4] is the underline that highlights
 *      which of the 6 name slots is being written into.
 *
 *   4. Read input — but only on frames where neither animation is in
 *      flight (the `if (selectfile_var9 | selectfile_var11) return;` gate).
 *
 *      • Direction-pad input drives the scroll animations indirectly via
 *        NameFile_CheckForScrollInputX / Y.
 *      • A/B/X/Y press (filtered_joypad_H/L & 0xc0) looks up the current
 *        cell in kNamePlayer_Tab3 (a 32x4 character map) and dispatches:
 *           0x5a → "back-space": rewind the name-field cursor
 *           0x44 → "next/skip" : advance the name-field cursor
 *           0x6f → "done"      : drop through to the commit path
 *           any other          → write the character into the name field
 *                                via NameFile_DrawSelectedCharacter, then
 *                                advance the name-field cursor.
 *      • Start (filtered_joypad_H & 0x10) also drops to the commit path.
 *
 *      Commit path: scan the 6-character name. If every position is still
 *      the blank-space tile 0xa9, refuse with sound 0x3c (error buzz).
 *      Otherwise initialize the SRAM block: magic word 0x55aa at 0x3e5,
 *      sentinel coords 0xf000 / 0xf000, "never died" counter 0xffff,
 *      and 60 bytes of kSramInit_Normal at offset 0x340 (default inventory
 *      placement — bytes at offsets 0x344 and 0x345 set the starting Y/X
 *      tile-positions inside Link's house, byte at 0x349 enables the
 *      uncle's-sword-and-shield pickup). Intro_FixCksum installs a
 *      checksum so the save validates on boot, ZeldaWriteSram flushes
 *      to battery, and ReturnToFileSelect exits.
 *
 * The `chr` value written for a character cell is
 *   (t & 0xfff0) * 2 + (t & 0xf)
 * which expands the tile index to span two VRAM rows (each character is
 * 8x16 → stored as two adjacent 8x8 tiles), preserving the low nibble as
 * the column offset and shifting the high nibble to the bitplane row.
 */
void NameFile_DoTheNaming() {  // 8cda4d
  static const int16 kNamePlayer_Tab1[26] = {
    -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1,
    -2, 2, -2, 2, -2, 2, -2, 2, -4, 4,
  };
  static const uint8 kNamePlayer_Tab2[4] = {131, 147, 163, 179};
  static const int8 kNamePlayer_X[6] = {31, 47, 63, 79, 95, 111};
  static const int16 kNamePlayer_Tab0[32] = {
    0x1f0,     0,  0x10,  0x20,  0x30,  0x40,  0x50,  0x60,  0x70,  0x80,  0x90,  0xa0,  0xb0,  0xc0,  0xd0,  0xe0,
     0xf0, 0x100, 0x110, 0x120, 0x130, 0x140, 0x150, 0x160, 0x170, 0x180, 0x190, 0x1a0, 0x1b0, 0x1c0, 0x1d0, 0x1e0,
  };
  static const int8 kNamePlayer_Tab3[128] = {
       6,    7, 0x5f,    9, 0x59, 0x59, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x60, 0x23,
    0x59, 0x59, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x59, 0x59, 0x59,    0,    1,    2,    3,    4,    5,
    0x10, 0x11, 0x12, 0x13, 0x59, 0x59, 0x24, 0x5f, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
    0x59, 0x59, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x59, 0x59, 0x59,  0xa,  0xb,  0xc,  0xd,  0xe,  0xf,
    0x40, 0x41, 0x42, 0x59, 0x59, 0x59, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x40, 0x41, 0x42, 0x59,
    0x59, 0x59, 0x61, 0x3f, 0x45, 0x46, 0x59, 0x59, 0x59, 0x59, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x44, 0x59, 0x6f, 0x6f, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x5a, 0x44, 0x59, 0x6f, 0x6f,
    0x59, 0x59, 0x5a, 0x44, 0x59, 0x6f, 0x6f, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x5a,
  };
  for (;;) {
    int j = selectfile_var9;
    if (j == 0) {
      NameFile_CheckForScrollInputX();
      break;
    }
    if (j != 0x31)
      selectfile_var9 += 4;
    j--;
    if (kNamePlayer_Tab0[selectfile_var3] == selectfile_var8) {
      selectfile_var9 = (joypad1H_last & 3) ? 0x30 : 0;
      NameFile_CheckForScrollInputX();
      continue;
    }
    if (!selectfile_var10)
      j += 2;
    selectfile_var8 = (selectfile_var8 + WORD(((uint8*)&kNamePlayer_Tab1)[j])) & 0x1ff;
    break;
  }

  for (;;) {
    if (selectfile_var11 == 0) {
      NameFile_CheckForScrollInputY();
      break;
    }
    uint8 diff = selectfile_var7 - kNamePlayer_Tab2[selectfile_var5];
    if (diff != 0) {
      selectfile_var7 += sign8(diff) ? 2 : -2;
      break;
    }
    selectfile_var11 = 0;
    NameFile_CheckForScrollInputY();
  }

  OamEnt *oam = oam_buf;
  for (int i = 0; i != 26; i++) {
    SetOamPlain(oam, 0x18 + i * 8, selectfile_var7, 0x2e, 0x3c, 0);
    oam++;
  }
  SetOamPlain(oam, kNamePlayer_X[selectfile_var4], 0x58, 0x29, 0xc, 0);

  if (selectfile_var9 | selectfile_var11)
    return;

  if (!(filtered_joypad_H & 0x10)) {
    if (!(filtered_joypad_H & 0xc0 || filtered_joypad_L & 0xc0))
      return;

    sound_effect_1 = 0x2b;
    uint8 t = kNamePlayer_Tab3[selectfile_var3 + selectfile_var5 * 0x20];
    if (t == 0x5a) {
      if (!selectfile_var4)
        selectfile_var4 = 5;
      else
        selectfile_var4--;
      return;
    } else if (t == 0x44) {
      if (++selectfile_var4 == 6)
        selectfile_var4 = 0;
      return;
    } else if (t != 0x6f) {
      int p = selectfile_var4 * 2 + attract_legend_ctr;
      uint16 chr = (t & 0xfff0) * 2 + (t & 0xf);
      WORD(g_zenv.sram[p + kSrmOffs_Name]) = chr;
      NameFile_DrawSelectedCharacter(selectfile_var4, chr);
      if (++selectfile_var4 == 6)
        selectfile_var4 = 0;
      return;
    }
  }
  int i = 0;
  for(;;) {
    uint16 a = WORD(g_zenv.sram[i * 2 + attract_legend_ctr + kSrmOffs_Name]);
    if (a != 0xa9)
      break;
    if (++i == 6) {
      sound_effect_1 = 0x3c;
      return;
    }
  }
  srm_var1 = selectfile_R16 * 2 + 2;
  uint8 *sram = &g_zenv.sram[selectfile_R16 * 0x500];
  WORD(sram[0x3e5]) = 0x55aa;
  WORD(sram[0x20c]) = 0xf000;
  WORD(sram[0x20e]) = 0xf000;
  WORD(sram[kSrmOffs_DiedCounter]) = 0xffff;
  static const uint8 kSramInit_Normal[60] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0,    0,    0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0,    0,    0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0, 0x18, 0x18, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf8, 0, 0,
  };
  memcpy(sram + 0x340, kSramInit_Normal, 60);
  Intro_FixCksum(sram);
  ZeldaWriteSram();
  ReturnToFileSelect();
  irq_flag = 0xff;
  sound_effect_1 = 0x2c;
}

/*
 * NameFile_CheckForScrollInputX — Left/Right input on the character grid.
 *
 * joypad1H_last bits 0/1 are the Right/Left buttons. k picks the direction
 * (0 = right / +1, 1 = left / -1 == 0xff in 8-bit), selectfile_var10 caches
 * the direction sign so NameFile_DoTheNaming can pick the correct step
 * table, and selectfile_var9++ kicks off the per-pixel scroll animation
 * which will step the grid until it lands on the new var3 value.
 *
 * The Cmp/Set tables implement column wrap-around:
 *   - Stepping right past column 0x20 wraps to column 0
 *   - Stepping left past column 0xff (i.e. column -1 in 8-bit) wraps to
 *     column 0x1f
 */
void NameFile_CheckForScrollInputX() {  // 8cdc8c
  static const uint16 kNameFile_CheckForScrollInputX_Add[2] = {1, 0xff};
  static const int16 kNameFile_CheckForScrollInputX_Cmp[2] = {0x20, 0xff};
  static const int16 kNameFile_CheckForScrollInputX_Set[2] = {0, 0x1f};
  if (joypad1H_last & 3) {
    int k = (joypad1H_last & 3) - 1;
    selectfile_var10 = k;
    selectfile_var9++;
    uint8 t = selectfile_var3 + kNameFile_CheckForScrollInputX_Add[k];
    if (t == kNameFile_CheckForScrollInputX_Cmp[k])
      t = kNameFile_CheckForScrollInputX_Set[k];
    selectfile_var3 = t;
  }
}

/*
 * NameFile_CheckForScrollInputY — Up/Down input on the character grid.
 *
 * joypad1H_last bits 2/3 are the Down/Up buttons. Two early-out cases
 * model the "you can't wrap into a row that doesn't have the bonus
 * cell you're currently aligned to" rule of the original UI:
 *
 *   (a * 2 | selectfile_var5) == 0x10 — pressing Down from row 4 (the
 *     last row) onto a column position that would land on a special cell
 *   (a * 4 | selectfile_var5) == 0x13 — pressing Up from row 1 onto the
 *     analogous illegal cell
 *
 * In those cases the keypress is swallowed (and only the most-recent
 * direction is recorded in selectfile_arr2[1]) so the cursor never lands
 * on an invalid grid position.
 *
 * Otherwise a * 4 (post >>= 2 → 0/1) selects the Add/Cmp/Set table to
 * apply the wrap: stepping past row 4 wraps to row 0; stepping past
 * row -1 wraps to row 3. selectfile_var11++ kicks off the row-change
 * animation in NameFile_DoTheNaming.
 *
 * The else branch clears the cached direction when neither U nor D is
 * held so the next press is treated as a fresh input.
 */
void NameFile_CheckForScrollInputY() {  // 8cdcbf
  static const int8 kNameFile_CheckForScrollInputY_Add[2] = {1, -1};
  static const int8 kNameFile_CheckForScrollInputY_Cmp[2] = {4, -1};
  static const int8 kNameFile_CheckForScrollInputY_Set[2] = {0, 3};

  uint8 a = joypad1H_last & 0xc;
  if (a) {
    if ((a * 2 | selectfile_var5) == 0x10 || (a * 4 | selectfile_var5) == 0x13) {
      selectfile_arr2[1] = a;
      return;
    }
     a >>= 2;
    int t = selectfile_var5 + kNameFile_CheckForScrollInputY_Add[a-1];
    if (t == kNameFile_CheckForScrollInputY_Cmp[a-1])
      t = kNameFile_CheckForScrollInputY_Set[a-1];
    selectfile_var5 = t;

    selectfile_var11++;
    selectfile_arr2[1] = a;

  } else {
    selectfile_arr2[0] = 0;
  }
}

/*
 * NameFile_DrawSelectedCharacter — Plot one character into the name field.
 *
 * Writes a stripe-image packet into vram_upload_data that places the
 * 8x16 character `chr` at name-field slot `k` (0..5). The packet uses the
 * standard stripe format: 16-bit VRAM address (byte-swapped), 16-bit
 * count (1 cell = 0x100 with the increment-by-one bit), 16-bit tile.
 *
 * Two stripe entries are written, one for each of the two 8x8 tiles that
 * make up the character:
 *   dst[0..2] : top half at VRAM offset kNameFile_DrawSelectedCharacter_Tab[k]
 *               within BG layer page 0x6100
 *   dst[3..5] : bottom half at +0x20 (next tile row in the BG map),
 *               with tile index +0x10 to pull from the next bitplane row
 *
 * dst[6] = 0xff is the stripe-image terminator; the NMI handler stops
 * processing when it sees this byte. The 0x1800 OR on `chr` packs the
 * palette and priority bits into the tilemap word.
 *
 * @param k    Name-field slot index (0..5) — which of the six name
 *             characters to overwrite.
 * @param chr  Pre-shifted tile index for the character (already in the
 *             "(t & 0xfff0) * 2 + (t & 0xf)" form computed by the caller).
 */
void NameFile_DrawSelectedCharacter(int k, uint16 chr) {  // 8cdd30
  static const uint16 kNameFile_DrawSelectedCharacter_Tab[6] = {0x84, 0x86, 0x88, 0x8a, 0x8c, 0x8e};
  uint16 *dst = vram_upload_data;
  uint16 a = kNameFile_DrawSelectedCharacter_Tab[k] | 0x6100;
  dst[0] = swap16(a);
  dst[1] = 0x100;
  dst[2] = 0x1800 | chr;
  dst[3] = swap16(a + 0x20);
  dst[4] = 0x100;
  dst[5] = (0x1800 | chr) + 0x10;
  BYTE(dst[6]) = 0xff;
  nmi_load_bg_from_vram = 1;
}

