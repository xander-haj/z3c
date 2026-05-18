/*
 * load_gfx.h - Graphics loading, palette management, and visual effect
 *              declarations for the Zelda 3 reimplementation.
 *
 * This header covers the entire graphics pipeline:
 *   - Tileset decompression and VRAM upload (3bpp-to-4bpp expansion)
 *   - Palette loading for all game contexts (overworld, dungeon, sprites,
 *     HUD, gear, file select, dungeon map, agahnim)
 *   - Palette filter effects (fade, flash, whiten, crystal shimmer,
 *     electro-themed coloring, wish pond effects)
 *   - Iris spotlight / wipe effects via HDMA window tables
 *   - Mosaic screen transition effects
 *   - Mirror warp visual animation
 *   - Star tile and peg puzzle graphics management
 *   - Water/flood HDMA window adjustments for Dam sequence
 *   - Boss-specific palette effects (Trinexx, Kholdstare, Agahnim)
 *
 * The SNES uses 3bpp (8-color) and 4bpp (16-color) tile formats. Many
 * ROM assets are stored in 3bpp to save space and must be expanded to
 * 4bpp before upload to VRAM. The Do3To4High/Low functions handle this.
 *
 * Related files: load_gfx.c (implementation), variables.h (WRAM state),
 *                assets.h (ROM data pointers), nmi.c (VRAM transfer)
 */
#ifndef ZELDA3_LOAD_GFX_H_
#define ZELDA3_LOAD_GFX_H_

/*
 * SRAM field offsets within a single save slot.
 * Each save slot in SRAM is a contiguous block; these offsets index into
 * that block to locate specific player progression fields. Used by the
 * file select screen to display equipment and stats without loading
 * the full save into WRAM.
 */
enum {
  kSrmOffs_Gloves = 0x354,       // Glove upgrade level (0=none, 1=power, 2=titan)
  kSrmOffs_Sword = 0x359,        // Sword level (0=none through 4=gold)
  kSrmOffs_Shield = 0x35a,       // Shield level (0=none through 3=mirror)
  kSrmOffs_Armor = 0x35b,        // Armor/tunic color (0=green, 1=blue, 2=red)
  kSrmOffs_DiedCounter = 0x405,  // Number of times the player has died
  kSrmOffs_Name = 0x3d9,         // Start of the 6-character player name
  kSrmOffs_Health = 0x36c,       // Current health in quarter-hearts
};

// Glove palette colors indexed by glove level (power glove, titan's mitt)
extern uint16 kGlovesColor[2];

// -----------------------------------------------------------------------
// Palette Filter System
// The palette filter applies per-frame color transformations (fade to
// black, fade to white, color isolation) across a range of CGRAM entries.
// A countdown timer controls the transition speed.
// -----------------------------------------------------------------------

// Dispatches to the currently active palette filter function
void ApplyPaletteFilter_bounce();
// Applies the active palette filter to CGRAM entries in range [from, to)
void PaletteFilter_Range(int from, int to);
// Decrements the palette filter countdown and triggers the next step
void PaletteFilter_IncrCountdown();

// -----------------------------------------------------------------------
// Tileset Loading and Decompression
// -----------------------------------------------------------------------

// Loads one frame of item animation graphics into dst buffer
// num: animation frame index, r12: tile offset, from_temp: use temp buffer
uint8 *LoadItemAnimationGfxOne(uint8 *dst, int num, int r12, bool from_temp);
// Emulates the SNES hardware divide unit (16-bit / 8-bit)
uint16 snes_divide(uint16 dividend, uint8 divisor);
// Clears all background tilemaps to transparent for normal gameplay
void EraseTileMaps_normal();
// Restores peg tile graphics from the mapping buffer after a puzzle reset
void RecoverPegGFXFromMapping();
// Loads the Mode 7 overworld map palette for the world map screen
void LoadOverworldMapPalette();
// Clears tilemaps for the Triforce room background
void EraseTileMaps_triforce();
// Clears tilemaps for the dungeon map overlay screen
void EraseTileMaps_dungeonmap();
// Clears tilemaps using specified fill values (r2=BG1, r0=BG2)
void EraseTileMaps(uint16 r2, uint16 r0);
// Enables SNES forced blanking (screen off) for safe VRAM writes
void EnableForceBlank();
// Loads all item graphics into the WRAM 4bpp tile buffer at $7F:4000
void LoadItemGFXIntoWRAM4BPPBuffer();
// Decompresses the current sword level's graphics into the sprite sheet
void DecompressSwordGraphics();
// Decompresses the current shield level's graphics into the sprite sheet
void DecompressShieldGraphics();
// Decompresses animated dungeon tiles (water, lava, conveyors) for set 'a'
void DecompressAnimatedDungeonTiles(uint8 a);
// Decompresses animated overworld tiles (flowers, water) for set 'a'
void DecompressAnimatedOverworldTiles(uint8 a);
// Loads auxiliary item graphics (secondary item sprites, bottles, etc.)
void LoadItemGFX_Auxiliary();
// Loads the tagalong follower's sprite graphics (Zelda, old man, etc.)
void LoadFollowerGraphics();
// Writes decompressed tile data for sheet 'a' to the 4bpp buffer at $7F4000
void WriteTo4BPPBuffer_at_7F4000(uint8 a);
// Decodes a single animated sprite tile using the variable-frame method
void DecodeAnimatedSpriteTile_variable(uint8 a);
// Expands 3bpp tile data to 4bpp by setting the high bitplane from base
// dst: output buffer, src: 3bpp data, base: 4th bitplane source, num: tiles
void Expand3To4High(uint8 *dst, const uint8 *src, const uint8 *base, int num);

// -----------------------------------------------------------------------
// Transitional and Auxiliary Graphics Loading
// These functions load tileset data during screen transitions (entering/
// exiting dungeons, scrolling between overworld screens, mirror warping).
// "Trans" refers to transitional loading that occurs mid-wipe.
// -----------------------------------------------------------------------

// Loads transitional auxiliary BG tilesets during area changes
void LoadTransAuxGFX();
// Loads transitional auxiliary sprite tilesets during area changes
void LoadTransAuxGFX_sprite();
// Inner function that loads sprite sheet data into the destination buffer
void Gfx_LoadSpritesInner(uint8 *dst);
// Reloads the previously active tileset sheets after a temporary override
void ReloadPreviouslyLoadedSheets();
// Decompresses the story illustration graphics for the attract mode intro
void Attract_DecompressStoryGFX();

// -----------------------------------------------------------------------
// Mirror Warp Visual Effects
// -----------------------------------------------------------------------

// Runs one frame of the mirror warp swirl animation
void AnimateMirrorWarp();
// Decompresses the destination world's tilesets during mirror warp
void AnimateMirrorWarp_DecompressNewTileSets();

// -----------------------------------------------------------------------
// VRAM Upload and Tileset Management
// -----------------------------------------------------------------------

// Uploads the next chunk of tile data to VRAM (called across multiple frames)
void Graphics_IncrementalVRAMUpload();
// Prepares transitional auxiliary graphics data before the VRAM upload
void PrepTransAuxGfx();
// Expands 3bpp to 4bpp (high bitplane) for 16-bit addressed tile data
void Do3To4High16Bit(uint8 *dst, const uint8 *src, int num);
// Expands 3bpp to 4bpp (low bitplane) for 16-bit addressed tile data
void Do3To4Low16Bit(uint8 *dst, const uint8 *src, int num);
// Loads a complete new sprite tileset when the area's sprite set changes
void LoadNewSpriteGFXSet();
// Initializes all background and sprite tilesets for the current area
void InitializeTilesets();
// Loads the default graphics set used at game startup
void LoadDefaultGraphics();
// Loads BG3 text/overlay graphics for the attract mode sequence
void Attract_LoadBG3GFX();
// Loads a half-slot (64 tiles) of character data into VRAM
void Graphics_LoadChrHalfSlot();
// Transfers the dialog font tileset into VRAM for text rendering
void TransferFontToVRAM();
// Expands 3bpp tiles to 4bpp with high bitplane set, writing to VRAM
void Do3To4High(uint16 *vram_ptr, const uint8 *decomp_addr);
// Expands 3bpp tiles to 4bpp with low bitplane set, writing to VRAM
void Do3To4Low(uint16 *vram_ptr, const uint8 *decomp_addr);
// Loads sprite graphics pack into VRAM at the specified address
void LoadSpriteGraphics(uint16 *vram_ptr, int gfx_pack, uint8 *decomp_addr);
// Loads BG graphics pack into VRAM; slot determines the tileset slot
void LoadBackgroundGraphics(uint16 *vram_ptr, int gfx_pack, int slot, uint8 *decomp_addr);
// Loads the common sprite tiles shared across all areas (Link, HUD icons)
void LoadCommonSprites();
// Decompresses sprite graphics pack 'gfx' into dst; returns decompressed size
int Decomp_spr(uint8 *dst, int gfx);
// Decompresses background graphics pack 'gfx' into dst; returns size
int Decomp_bg(uint8 *dst, int gfx);
// General-purpose LZ decompression from src to dst; returns decompressed size
int Decompress(uint8 *dst, const uint8 *src);
// -----------------------------------------------------------------------
// Palette Filter Variants and Boss Effects
// Each filter operates on specific CGRAM ranges to produce visual effects
// like fading, color isolation, and flashing. They are driven by per-frame
// calls during gameplay or cutscene transitions.
// -----------------------------------------------------------------------

// Resets HUD palette rows 4 and 5 to their default colors
void ResetHUDPalettes4and5();
// Replays the palette filter history to restore a known palette state
void PaletteFilterHistory();
// Applies the wish pond color shimmer effect (waterfall fairy fountain)
void PaletteFilter_WishPonds();
// Applies the crystal maiden color cycling effect (rescued maiden scenes)
void PaletteFilter_Crystal();
// Inner loop for the wish pond palette color animation
void PaletteFilter_WishPonds_Inner();
// Restores sprite palette row 5F after a temporary filter override
void PaletteFilter_RestoreSP5F();
// Applies a filter to sprite palette row 5F (used for special NPC effects)
void PaletteFilter_SP5F();
// Animates Kholdstare's ice shell palette during the boss fight
void KholdstareShell_PaletteFiltering();
// Filters Agahnim's warp shadow palette for sprite index k
void AgahnimWarpShadowFilter(int k);
// Advances the intro sequence palette fade by one brightness step
void Palette_FadeIntroOneStep();
// Second-phase intro fade (transitions from dark to gameplay brightness)
void Palette_FadeIntro2();
// Additively restores palette entries in range [from, to) toward targets
void PaletteFilter_RestoreAdditive(int from, int to);
// Subtractively restores palette entries in range [from, to) toward targets
void PaletteFilter_RestoreSubtractive(uint16 from, uint16 to);
// Sets up the blinding white palette filter (all colors toward $7FFF white)
void PaletteFilter_InitializeWhiteFilter();
// Runs the mirror warp animation submodule sequence (swirl + palette shift)
void MirrorWarp_RunAnimationSubmodules();
// Drives the blinding white flash effect per-frame (Master Sword, Triforce)
void PaletteFilter_BlindingWhite();
// Initiates the blinding white flash by setting the filter parameters
void PaletteFilter_StartBlindingWhite();
// Blinding white variant used specifically for the Triforce scene
void PaletteFilter_BlindingWhiteTriforce();
// Applies blue-tinted palette for the whirlpool warp effect
void PaletteFilter_WhirlpoolBlue();
// Isolates the blue channel during the whirlpool palette transition
void PaletteFilter_IsolateWhirlpoolBlue();
// Restores the blue channel after the whirlpool effect completes
void PaletteFilter_WhirlpoolRestoreBlue();
// Restores the red and green channels after whirlpool isolation
void PaletteFilter_WhirlpoolRestoreRedGreen();
// Strict subtractive restoration of BG palettes (guaranteed to reach target)
void PaletteFilter_RestoreBGSubstractiveStrict();
// Strict additive restoration of BG palettes (guaranteed to reach target)
void PaletteFilter_RestoreBGAdditiveStrict();
// -----------------------------------------------------------------------
// Trinexx Boss Palette Effects
// Trinexx's three heads flash their shell palettes when hit: the fire
// head flashes red, the ice head flashes blue. Flash/unflash pairs
// create a one-frame color spike that signals damage feedback.
// -----------------------------------------------------------------------

// Flashes Trinexx's shell palette to red (fire head damage feedback)
void Trinexx_FlashShellPalette_Red();
// Restores Trinexx's shell palette from the red flash
void Trinexx_UnflashShellPalette_Red();
// Flashes Trinexx's shell palette to blue (ice head damage feedback)
void Trinexx_FlashShellPalette_Blue();
// Restores Trinexx's shell palette from the blue flash
void Trinexx_UnflashShellPalette_Blue();
// -----------------------------------------------------------------------
// Iris Spotlight / Wipe Effects
// The iris spotlight is a circular window that opens or closes around a
// center point, implemented via HDMA writes to the SNES window registers.
// Used for dungeon entry/exit, death sequence, and desert prayer.
// -----------------------------------------------------------------------

// Shrinks the iris spotlight to close the screen (transition out)
void IrisSpotlight_close();
// Expands the iris spotlight to reveal the screen (transition in)
void Spotlight_open();
// Core iris renderer: computes the circle at center (x, y) for this frame
void SpotlightInternal(uint8 x, uint8 y);
// Writes HDMA channel configuration for the iris spotlight window
void IrisSpotlight_ConfigureTable();
// Clears the iris HDMA table to a fully-open or fully-closed state
void IrisSpotlight_ResetTable();
// Computes the horizontal half-width of the iris circle at scanline offset a
uint16 IrisSpotlight_CalculateCircleValue(uint8 a);

// -----------------------------------------------------------------------
// Water and Flood HDMA Effects
// -----------------------------------------------------------------------

// Adjusts the HDMA window boundaries for the water surface reflection
void AdjustWaterHDMAWindow();
// Updates the horizontal component of the water HDMA window at offset r10
void AdjustWaterHDMAWindow_X(uint16 r10);
// Prepares the rising flood HDMA table for the dam drainage sequence
void FloodDam_PrepFloodHDMA();
// -----------------------------------------------------------------------
// Star Tile, Peg Puzzle, and Mosaic Effects
// Star tiles toggle between raised/lowered states in certain dungeons.
// Peg puzzles require hammering pegs in the correct order. Both need
// tile graphics updates after state changes.
// -----------------------------------------------------------------------

// Resets star tile character data to the default (un-toggled) state
void ResetStarTileGraphics();
// Restores star tile CHR data from the backup after a room transition
void Dungeon_RestoreStarTileChr();
// Applies the mosaic pixelation effect during Link's electrocution zap
void LinkZap_HandleMosaic();
// Sets the SNES mosaic register to a custom level (0=off, 0xF=maximum)
void Player_SetCustomMosaicLevel(uint8 a);
// Peg update step 1: marks which pegs were hammered and flags for redraw
void Module07_16_UpdatePegs_Step1();
// Peg update step 2: commits the peg state changes to the tile buffer
void Module07_16_UpdatePegs_Step2();
// Updates the peg graphics buffer at tile position (x, y) after hammering
void Dungeon_UpdatePegGFXBuffer(int x, int y);
// Manages BG layer translucency and palette swaps for the current room
void Dungeon_HandleTranslucencyAndPalette();
// -----------------------------------------------------------------------
// Palette Loading - Overworld and Dungeon
// The SNES CGRAM holds 256 colors (8 palettes of 16 colors each for BG,
// 8 for sprites). Different areas use different palette sets that must
// be loaded when the player transitions between screens or enters/exits
// dungeons.
// -----------------------------------------------------------------------

// Loads all palette sets needed for the current overworld area
void Overworld_LoadAllPalettes();
// Loads all palette sets needed for the current dungeon room
void Dungeon_LoadPalettes();
// Inner palette loading routine for overworld (shared by multiple callers)
void Overworld_LoadPalettesInner();
// Loads the palette set associated with the current overworld screen pair
void OverworldLoadScreensPaletteSet();
// Loads area-specific palettes using extended palette index x
void Overworld_LoadAreaPalettesEx(uint8 x);
// Copies special overworld palettes to the palette cache for quick restore
void SpecialOverworld_CopyPalettesToCache();
// Copies standard overworld palettes to the palette cache
void Overworld_CopyPalettesToCache();
// Loads specific BG and sprite palette indices for an overworld area
void Overworld_LoadPalettes(uint8 bg, uint8 spr);
// Sets both the BG backdrop and the fixed color register to black
void Palette_BgAndFixedColor_Black();
// Sets the BG backdrop and fixed color register to the specified 15-bit color
void Palette_SetBgAndFixedColor(uint16 color);
// Sets CGRAM entry 0 (the universal backdrop color) to black
void SetBackdropcolorBlack();
// Sets the overworld background color based on the current area's palette
void Palette_SetOwBgColor();
// Applies special overworld palette adjustments (rain, dark world tint)
void Palette_SpecialOw();
// Returns the 15-bit BGR color for the current overworld area's background
uint16 Palette_GetOwBgColor();
// Verifies the translucency palette swap is in the expected state
void Palette_AssertTranslucencySwap();
// Enables or disables the translucency palette swap (for BG layer blending)
void Palette_SetTranslucencySwap(bool v);
// Reverts the translucency palette swap to its previous state
void Palette_RevertTranslucencySwap();
// -----------------------------------------------------------------------
// Gear / Equipment Palettes
// Link's sprite palette changes based on equipped sword, shield, and
// armor (tunic). The bunny palette is used when Link is cursed in the
// Dark World without the Moon Pearl.
// -----------------------------------------------------------------------

// Loads Link's gear palettes based on currently equipped items
void LoadActualGearPalettes();
// Loads the electrocution-themed palette (blue flash when zapped by traps)
void Palette_ElectroThemedGear();
// Loads the bunny form palette (Dark World curse without Moon Pearl)
void LoadGearPalettes_bunny();
// Loads gear palettes for specific sword, shield, and armor levels
void LoadGearPalettes(uint8 sword, uint8 shield, uint8 armor);
// Copies n palette entries from src to CGRAM position dst
void LoadGearPalette(int dst, const uint16 *src, int n);

// -----------------------------------------------------------------------
// Screen Flash and Palette Restoration
// -----------------------------------------------------------------------

// Whitens all BG palettes for a major flash effect (bomb, Ether medallion)
void Filter_Majorly_Whiten_Bg();
// Returns a whitened version of a single 15-bit BGR color value
uint16 Filter_Majorly_Whiten_Color(uint16 c);
// Restores BG palettes from the backup after a screen flash
void Palette_Restore_BG_From_Flash();
// Restores the SNES fixed color data register to its pre-flash value
void Palette_Restore_Coldata();
// Restores both BG palettes and HUD palettes after a flash
void Palette_Restore_BG_And_HUD();
// -----------------------------------------------------------------------
// Individual Palette Row Loaders
// Each function loads a specific palette row into CGRAM. Palette rows
// are grouped by purpose: sprite palettes (Sp0-Sp7) hold Link, enemies,
// and NPCs; BG palettes hold terrain, dungeon tiles, and UI elements.
// The "L" suffix typically indicates "load" or a specific sub-palette.
// -----------------------------------------------------------------------

// Loads sprite palette row 0 (Link's base palette)
void Palette_Load_Sp0L();
// Loads the main sprite palette set for the current area's enemies/NPCs
void Palette_Load_SpriteMain();
// Loads sprite palette row 5 (secondary sprite effects)
void Palette_Load_Sp5L();
// Loads sprite palette row 6 (additional sprite effects)
void Palette_Load_Sp6L();
// Loads the sword-specific palette entries based on current sword level
void Palette_Load_Sword();
// Loads the shield-specific palette entries based on current shield level
void Palette_Load_Shield();
// Loads environment-dependent sprite palettes (overworld context)
void Palette_Load_SpriteEnvironment();
// Loads environment-dependent sprite palettes (dungeon context)
void Palette_Load_SpriteEnvironment_Dungeon();
// Loads miscellaneous outdoor sprite palettes (overworld-specific NPCs)
void Palette_MiscSprite_Outdoors();
// Loads sprite palettes used on the dungeon map screen
void Palette_Load_DungeonMapSprite();
// Loads Link's armor (tunic) and glove color palettes
void Palette_Load_LinkArmorAndGloves();
// Updates the glove color entries in Link's palette after an upgrade
void Palette_UpdateGlovesColor();
// Loads BG palettes for the dungeon map display
void Palette_Load_DungeonMapBG();
// Loads the HUD palette (hearts, magic meter, item frame)
void Palette_Load_HUD();
// Loads the complete BG palette set for the current dungeon
void Palette_Load_DungeonSet();
// Loads overworld BG3 palette (text overlay, status bar)
void Palette_Load_OWBG3();
// Loads the primary overworld BG palette set
void Palette_Load_OWBGMain();
// Loads overworld BG1 palette (main terrain layer)
void Palette_Load_OWBG1();
// Loads overworld BG2 palette (secondary terrain / overlay layer)
void Palette_Load_OWBG2();

// -----------------------------------------------------------------------
// Generic Palette Transfer Functions
// -----------------------------------------------------------------------

// Loads a single palette row: x_ents entries from src to CGRAM position dst
void Palette_LoadSingle(const uint16 *src, int dst, int x_ents);
// Loads multiple palette rows: y_pals rows of x_ents entries each
void Palette_LoadMultiple(const uint16 *src, int dst, int x_ents, int y_pals);
// Loads multiple entries from a non-uniform source layout
void Palette_LoadMultiple_Arbitrary(const uint16 *src, int dst, int x_ents);

// -----------------------------------------------------------------------
// File Select Screen Palettes
// The file select screen shows three save files with Link rendered in
// each file's equipped gear colors. These functions load per-file palettes
// without disturbing the main game palette state.
// -----------------------------------------------------------------------

// Loads all palettes needed for the file select screen
void Palette_LoadForFileSelect();
// Loads armor/glove palette for save file k on the file select screen
void Palette_LoadForFileSelect_Armor(int k, uint8 armor, uint8 gloves);
// Loads sword palette for save file k on the file select screen
void Palette_LoadForFileSelect_Sword(int k, uint8 sword);
// Loads shield palette for save file k on the file select screen
void Palette_LoadForFileSelect_Shield(int k, uint8 shield);
// Loads Agahnim's unique sprite palette (used in boss fights and cutscenes)
void Palette_LoadAgahnim();
// Handles per-frame screen flash brightness updates (Lightning, bombs, etc.)
void HandleScreenFlash();

#endif // ZELDA3_LOAD_GFX_H_
