/*
 * assets.h — Compiled Game Asset Registry
 *
 * Provides typed macro accessors for all 165 game assets extracted from the
 * original Zelda: A Link to the Past ROM. Assets are compiled into a single
 * binary blob (zelda3_assets.dat) by the Python asset pipeline
 * (assets/compile_resources.py), then loaded at runtime into two parallel
 * arrays: g_asset_ptrs[] (base pointers) and g_asset_sizes[] (byte lengths).
 *
 * Each asset is accessed through a #define macro that casts the raw pointer
 * to the appropriate C type (uint8*, uint16*, int8*, etc.). Most macros come
 * in pairs: kName for the data pointer and kName_SIZE for the byte count.
 * A few assets (sprites, backgrounds, dialogue, dungeon maps) are indexed
 * sub-arrays accessed via FindInAssetArray(asset, idx), which returns a
 * MemBlk containing both the pointer and size for one element within the
 * asset's packed sub-array.
 *
 * Asset categories (by index range):
 *   0-2:     SPC700 sound banks (intro, indoor, ending music)
 *   3-10:    Dungeon room data (layout, doors, headers, chests, teleporters)
 *   11-27:   Entrance data (room setup when entering dungeons)
 *   28-45:   Starting point data (initial spawn configuration per save slot)
 *   46-56:   Dungeon objects (defaults, overlays, secrets, tile attrs, torches)
 *   57:      Link's sprite sheet graphics
 *   58-59:   Dungeon sprite placement data and offsets
 *   60-65:   Map tile hierarchy (Map32->Map16 conversion, sprite/bg graphics)
 *   66-70:   Overworld map graphics and tilemaps (light/dark world)
 *   71-78:   Generated/compiled data (wish pond, bombos, ending sequences)
 *   79-93:   Color palettes (dungeon bg, sprites, armor, overworld, HUD)
 *   94-98:   Dialogue system (text, fonts, maps, dungeon map layouts/tiles)
 *   99-104:  Background tilemaps (title, file select, etc.)
 *   105-112: Overworld configuration (compressed tiles, map sizes, palettes)
 *   113-122: Bird travel / flute warp destinations
 *   123-129: Whirlpool and fall hole entrance data
 *   130-156: Exit data and special exit tables (screen transitions)
 *   157-162: Overworld secrets and overworld sprite placement
 *   163-164: Tile attribute lookup tables
 */
#pragma once
#include "types.h"

// Total number of distinct asset entries in the compiled asset file.
enum {
  kNumberOfAssets = 165
};
// Runtime asset storage: pointers to each asset's data within the loaded blob.
extern const uint8 *g_asset_ptrs[kNumberOfAssets];
// Runtime asset storage: byte sizes of each asset's data.
extern uint32 g_asset_sizes[kNumberOfAssets];
/*
 * FindInAssetArray - Looks up a sub-element within a packed asset array.
 * Parameters:
 *   asset - The asset index (e.g., 64 for sprite graphics).
 *   idx   - The sub-element index within that asset's packed array.
 * Returns: A MemBlk struct containing the pointer and size for the
 *          requested sub-element. Used by assets that contain multiple
 *          variable-length entries packed sequentially (sprites, dialogue).
 */
extern MemBlk FindInAssetArray(int asset, int idx);

/* -----------------------------------------------------------------------
 * SPC700 Sound Banks (assets 0-2)
 * Raw SPC700 audio program data uploaded to the APU's 64KB RAM.
 * Each bank contains the music engine code plus instrument/sequence data
 * for a specific game context (intro cutscene, indoor/dungeon, ending).
 * ----------------------------------------------------------------------- */
#define kSoundBank_intro ((uint8*)g_asset_ptrs[0])
#define kSoundBank_intro_SIZE (g_asset_sizes[0])
#define kSoundBank_indoor ((uint8*)g_asset_ptrs[1])
#define kSoundBank_indoor_SIZE (g_asset_sizes[1])
#define kSoundBank_ending ((uint8*)g_asset_ptrs[2])
#define kSoundBank_ending_SIZE (g_asset_sizes[2])
/* -----------------------------------------------------------------------
 * Dungeon Room Data (assets 3-10)
 * Defines the layout, doors, headers, chests, and special properties
 * for all ~296 dungeon rooms. Room data is compressed object lists;
 * offsets arrays index into the packed data by room number.
 * ----------------------------------------------------------------------- */
// Packed dungeon room object data (tile layout commands per room).
#define kDungeonRoom ((uint8*)g_asset_ptrs[3])
#define kDungeonRoom_SIZE (g_asset_sizes[3])
// Per-room byte offsets into kDungeonRoom, indexed by room number.
#define kDungeonRoomOffs ((uint16*)g_asset_ptrs[4])
#define kDungeonRoomOffs_SIZE (g_asset_sizes[4])
// Per-room byte offsets to door object data within each room's layout.
#define kDungeonRoomDoorOffs ((uint16*)g_asset_ptrs[5])
#define kDungeonRoomDoorOffs_SIZE (g_asset_sizes[5])
// Room header bytes: BG2 property, collision type, palette, blockset, etc.
#define kDungeonRoomHeaders ((uint8*)g_asset_ptrs[6])
#define kDungeonRoomHeaders_SIZE (g_asset_sizes[6])
// Per-room byte offsets into kDungeonRoomHeaders.
#define kDungeonRoomHeadersOffs ((uint16*)g_asset_ptrs[7])
#define kDungeonRoomHeadersOffs_SIZE (g_asset_sizes[7])
// Chest contents per room: item ID and tilemap position for each chest.
#define kDungeonRoomChests ((uint8*)g_asset_ptrs[8])
#define kDungeonRoomChests_SIZE (g_asset_sizes[8])
// Teleporter/messaging data per room (hole/warp destinations and messages).
#define kDungeonRoomTeleMsg ((uint16*)g_asset_ptrs[9])
#define kDungeonRoomTeleMsg_SIZE (g_asset_sizes[9])
// Bitmask table: which rooms have pits that damage the player on landing.
#define kDungeonPitsHurtPlayer ((uint16*)g_asset_ptrs[10])
#define kDungeonPitsHurtPlayer_SIZE (g_asset_sizes[10])
/* -----------------------------------------------------------------------
 * Entrance Data (assets 11-27)
 * Each entrance defines the full initial state when Link enters a dungeon
 * room: which room to load, Link's position, camera scroll, BG blockset,
 * floor level, palace ID, door orientation, visible quadrants, and music.
 * All arrays are indexed by entrance ID (0-132). These parallel arrays
 * collectively reconstruct the SNES entrance table from ROM bank $09.
 * ----------------------------------------------------------------------- */
// Room number for each entrance (which dungeon room to load).
#define kEntranceData_rooms ((uint16*)g_asset_ptrs[11])
#define kEntranceData_rooms_SIZE (g_asset_sizes[11])
// Packed relative coordinates within the room's coordinate space.
#define kEntranceData_relativeCoords ((uint8*)g_asset_ptrs[12])
#define kEntranceData_relativeCoords_SIZE (g_asset_sizes[12])
// Initial horizontal scroll position for BG layers.
#define kEntranceData_scrollX ((uint16*)g_asset_ptrs[13])
#define kEntranceData_scrollX_SIZE (g_asset_sizes[13])
// Initial vertical scroll position for BG layers.
#define kEntranceData_scrollY ((uint16*)g_asset_ptrs[14])
#define kEntranceData_scrollY_SIZE (g_asset_sizes[14])
// Link's initial X pixel coordinate upon entering.
#define kEntranceData_playerX ((uint16*)g_asset_ptrs[15])
#define kEntranceData_playerX_SIZE (g_asset_sizes[15])
// Link's initial Y pixel coordinate upon entering.
#define kEntranceData_playerY ((uint16*)g_asset_ptrs[16])
#define kEntranceData_playerY_SIZE (g_asset_sizes[16])
// Camera X scroll target on entry.
#define kEntranceData_cameraX ((uint16*)g_asset_ptrs[17])
#define kEntranceData_cameraX_SIZE (g_asset_sizes[17])
// Camera Y scroll target on entry.
#define kEntranceData_cameraY ((uint16*)g_asset_ptrs[18])
#define kEntranceData_cameraY_SIZE (g_asset_sizes[18])
// Tileset blockset index controlling which 4bpp tile graphics to load.
#define kEntranceData_blockset ((uint8*)g_asset_ptrs[19])
#define kEntranceData_blockset_SIZE (g_asset_sizes[19])
// Floor level (signed: negative = basement, positive = upper floors).
#define kEntranceData_floor ((int8*)g_asset_ptrs[20])
#define kEntranceData_floor_SIZE (g_asset_sizes[20])
// Palace/dungeon index (signed; -1 for non-palace rooms like caves).
#define kEntranceData_palace ((int8*)g_asset_ptrs[21])
#define kEntranceData_palace_SIZE (g_asset_sizes[21])
// Doorway facing direction (0=north, 2=south, 4=west, 6=east).
#define kEntranceData_doorwayOrientation ((uint8*)g_asset_ptrs[22])
#define kEntranceData_doorwayOrientation_SIZE (g_asset_sizes[22])
// Which background layer is initially visible (BG1 vs BG2 priority).
#define kEntranceData_startingBg ((uint8*)g_asset_ptrs[23])
#define kEntranceData_startingBg_SIZE (g_asset_sizes[23])
// Quadrant visibility flags (which of the 4 room quadrants are shown).
#define kEntranceData_quadrant1 ((uint8*)g_asset_ptrs[24])
#define kEntranceData_quadrant1_SIZE (g_asset_sizes[24])
#define kEntranceData_quadrant2 ((uint8*)g_asset_ptrs[25])
#define kEntranceData_quadrant2_SIZE (g_asset_sizes[25])
// Door configuration bits (which doors are open/closed/locked on entry).
#define kEntranceData_doorSettings ((uint16*)g_asset_ptrs[26])
#define kEntranceData_doorSettings_SIZE (g_asset_sizes[26])
// Music track ID to play when entering this room.
#define kEntranceData_musicTrack ((uint8*)g_asset_ptrs[27])
#define kEntranceData_musicTrack_SIZE (g_asset_sizes[27])
/* -----------------------------------------------------------------------
 * Starting Point Data (assets 28-45)
 * Identical structure to Entrance Data but used for game-start spawn
 * points (e.g., Link's house, sanctuary, mountain cave). Indexed by
 * which_starting_point from the save file. Includes one extra field
 * (entrance) mapping back to the associated entrance ID.
 * ----------------------------------------------------------------------- */
#define kStartingPoint_rooms ((uint16*)g_asset_ptrs[28])
#define kStartingPoint_rooms_SIZE (g_asset_sizes[28])
#define kStartingPoint_relativeCoords ((uint8*)g_asset_ptrs[29])
#define kStartingPoint_relativeCoords_SIZE (g_asset_sizes[29])
#define kStartingPoint_scrollX ((uint16*)g_asset_ptrs[30])
#define kStartingPoint_scrollX_SIZE (g_asset_sizes[30])
#define kStartingPoint_scrollY ((uint16*)g_asset_ptrs[31])
#define kStartingPoint_scrollY_SIZE (g_asset_sizes[31])
#define kStartingPoint_playerX ((uint16*)g_asset_ptrs[32])
#define kStartingPoint_playerX_SIZE (g_asset_sizes[32])
#define kStartingPoint_playerY ((uint16*)g_asset_ptrs[33])
#define kStartingPoint_playerY_SIZE (g_asset_sizes[33])
#define kStartingPoint_cameraX ((uint16*)g_asset_ptrs[34])
#define kStartingPoint_cameraX_SIZE (g_asset_sizes[34])
#define kStartingPoint_cameraY ((uint16*)g_asset_ptrs[35])
#define kStartingPoint_cameraY_SIZE (g_asset_sizes[35])
#define kStartingPoint_blockset ((uint8*)g_asset_ptrs[36])
#define kStartingPoint_blockset_SIZE (g_asset_sizes[36])
#define kStartingPoint_floor ((int8*)g_asset_ptrs[37])
#define kStartingPoint_floor_SIZE (g_asset_sizes[37])
#define kStartingPoint_palace ((int8*)g_asset_ptrs[38])
#define kStartingPoint_palace_SIZE (g_asset_sizes[38])
#define kStartingPoint_doorwayOrientation ((uint8*)g_asset_ptrs[39])
#define kStartingPoint_doorwayOrientation_SIZE (g_asset_sizes[39])
#define kStartingPoint_startingBg ((uint8*)g_asset_ptrs[40])
#define kStartingPoint_startingBg_SIZE (g_asset_sizes[40])
#define kStartingPoint_quadrant1 ((uint8*)g_asset_ptrs[41])
#define kStartingPoint_quadrant1_SIZE (g_asset_sizes[41])
#define kStartingPoint_quadrant2 ((uint8*)g_asset_ptrs[42])
#define kStartingPoint_quadrant2_SIZE (g_asset_sizes[42])
#define kStartingPoint_doorSettings ((uint16*)g_asset_ptrs[43])
#define kStartingPoint_doorSettings_SIZE (g_asset_sizes[43])
// Maps this starting point back to an entrance ID for initialization.
#define kStartingPoint_entrance ((uint8*)g_asset_ptrs[44])
#define kStartingPoint_entrance_SIZE (g_asset_sizes[44])
#define kStartingPoint_musicTrack ((uint8*)g_asset_ptrs[45])
#define kStartingPoint_musicTrack_SIZE (g_asset_sizes[45])
/* -----------------------------------------------------------------------
 * Dungeon Room Defaults, Overlays, and Interactive Objects (assets 46-56)
 * Default tile layouts applied before room-specific data, overlays for
 * conditional room modifications (e.g., after boss defeat), secret tile
 * substitutions, tile collision attributes, movable block positions,
 * torch positions, and the enemy damage resistance table.
 * ----------------------------------------------------------------------- */
// Default (base) tile layout data shared across multiple rooms.
#define kDungeonRoomDefault ((uint8*)g_asset_ptrs[46])
#define kDungeonRoomDefault_SIZE (g_asset_sizes[46])
#define kDungeonRoomDefaultOffs ((uint16*)g_asset_ptrs[47])
#define kDungeonRoomDefaultOffs_SIZE (g_asset_sizes[47])
// Overlay tile commands applied conditionally (e.g., water drained rooms).
#define kDungeonRoomOverlay ((uint8*)g_asset_ptrs[48])
#define kDungeonRoomOverlay_SIZE (g_asset_sizes[48])
#define kDungeonRoomOverlayOffs ((uint16*)g_asset_ptrs[49])
#define kDungeonRoomOverlayOffs_SIZE (g_asset_sizes[49])
// Secret tile substitution data (bombable walls, hidden passages).
#define kDungeonSecrets ((uint8*)g_asset_ptrs[50])
#define kDungeonSecrets_SIZE (g_asset_sizes[50])
// Tile attribute offsets and data: maps tile IDs to collision/behavior types.
#define kDungAttrsForTile_Offs ((uint16*)g_asset_ptrs[51])
#define kDungAttrsForTile_Offs_SIZE (g_asset_sizes[51])
#define kDungAttrsForTile ((uint8*)g_asset_ptrs[52])
#define kDungAttrsForTile_SIZE (g_asset_sizes[52])
// Initial positions for movable/pushable blocks in each room.
#define kMovableBlockDataInit ((uint16*)g_asset_ptrs[53])
#define kMovableBlockDataInit_SIZE (g_asset_sizes[53])
// Initial torch positions per room (lightable torches for dark rooms).
#define kTorchDataInit ((uint16*)g_asset_ptrs[54])
#define kTorchDataInit_SIZE (g_asset_sizes[54])
// Secondary torch data (unused/legacy data preserved from the original ROM).
#define kTorchDataJunk ((uint16*)g_asset_ptrs[55])
#define kTorchDataJunk_SIZE (g_asset_sizes[55])
// Damage resistance table: 4 bytes per enemy type, one per weapon class.
#define kEnemyDamageData ((uint8*)g_asset_ptrs[56])
#define kEnemyDamageData_SIZE (g_asset_sizes[56])
/* -----------------------------------------------------------------------
 * Link Graphics and Dungeon Sprite Placement (assets 57-59)
 * ----------------------------------------------------------------------- */
// Link's full 4bpp sprite sheet (all animation frames for all states).
#define kLinkGraphics ((uint8*)g_asset_ptrs[57])
#define kLinkGraphics_SIZE (g_asset_sizes[57])
// Packed sprite placement lists per dungeon room (enemy type + position).
#define kDungeonSprites ((uint8*)g_asset_ptrs[58])
#define kDungeonSprites_SIZE (g_asset_sizes[58])
// Per-room offsets into kDungeonSprites, indexed by room number.
#define kDungeonSpriteOffs ((uint16*)g_asset_ptrs[59])
#define kDungeonSpriteOffs_SIZE (g_asset_sizes[59])
/* -----------------------------------------------------------------------
 * Map Tile Hierarchy and Graphics Banks (assets 60-65)
 * The SNES uses a 3-level tile hierarchy: Map32 (32x32 pixel metatiles)
 * decompose into four Map16 (16x16) tiles, which further decompose into
 * four Map8 (8x8 hardware tiles). Assets 60-63 store the Map32->Map16
 * mapping as four separate quadrant arrays (UL, UR, LL, LR).
 * ----------------------------------------------------------------------- */
// Map32->Map16 quadrant mappings: upper-left, upper-right, lower-left, lower-right.
#define kMap32ToMap16_0 ((uint8*)g_asset_ptrs[60])
#define kMap32ToMap16_0_SIZE (g_asset_sizes[60])
#define kMap32ToMap16_1 ((uint8*)g_asset_ptrs[61])
#define kMap32ToMap16_1_SIZE (g_asset_sizes[61])
#define kMap32ToMap16_2 ((uint8*)g_asset_ptrs[62])
#define kMap32ToMap16_2_SIZE (g_asset_sizes[62])
#define kMap32ToMap16_3 ((uint8*)g_asset_ptrs[63])
#define kMap32ToMap16_3_SIZE (g_asset_sizes[63])
// Indexed sprite graphics sheets (4bpp planar tiles, one set per area type).
#define kSprGfx(idx) FindInAssetArray(64, idx)
// Indexed background graphics sheets (4bpp planar tiles for BG layers).
#define kBgGfx(idx) FindInAssetArray(65, idx)
/* -----------------------------------------------------------------------
 * Overworld Map Graphics and Tile Data (assets 66-70)
 * ----------------------------------------------------------------------- */
// Tile graphics for the pause-screen overworld map (reduced-res overview).
#define kOverworldMapGfx ((uint8*)g_asset_ptrs[66])
#define kOverworldMapGfx_SIZE (g_asset_sizes[66])
// Compressed tilemap for the Light World overworld (64 area screens).
#define kLightOverworldTilemap ((uint8*)g_asset_ptrs[67])
#define kLightOverworldTilemap_SIZE (g_asset_sizes[67])
// Compressed tilemap for the Dark World overworld (64 area screens).
#define kDarkOverworldTilemap ((uint8*)g_asset_ptrs[68])
#define kDarkOverworldTilemap_SIZE (g_asset_sizes[68])
// Predefined tile arrangements for common dungeon/overworld objects.
#define kPredefinedTileData ((uint16*)g_asset_ptrs[69])
#define kPredefinedTileData_SIZE (g_asset_sizes[69])
// Map16->Map8 decomposition: four 8x8 hardware tile IDs per Map16 tile.
#define kMap16ToMap8 ((uint16*)g_asset_ptrs[70])
#define kMap16ToMap8_SIZE (g_asset_sizes[70])
/* -----------------------------------------------------------------------
 * Generated/Compiled Data and Ending Sequence (assets 71-78)
 * Pre-computed data tables generated by the asset pipeline rather than
 * extracted directly from ROM. Includes wish pond item upgrade data,
 * bombos medallion animation coords, and all ending/credits assets.
 * ----------------------------------------------------------------------- */
// Wish pond (waterfall of wishing) item upgrade mappings.
#define kGeneratedWishPondItem ((uint8*)g_asset_ptrs[71])
#define kGeneratedWishPondItem_SIZE (g_asset_sizes[71])
// Pre-computed coordinate array for the Bombos medallion spell animation.
#define kGeneratedBombosArr ((uint8*)g_asset_ptrs[72])
#define kGeneratedBombosArr_SIZE (g_asset_sizes[72])
// End sequence scene 15 pre-generated tilemap data.
#define kGeneratedEndSequence15 ((uint8*)g_asset_ptrs[73])
#define kGeneratedEndSequence15_SIZE (g_asset_sizes[73])
// Credits text strings (character names, locations, "THE END", etc.).
#define kEnding_Credits_Text ((uint8*)g_asset_ptrs[74])
#define kEnding_Credits_Text_SIZE (g_asset_sizes[74])
// Per-credits-line offsets into kEnding_Credits_Text.
#define kEnding_Credits_Offs ((uint16*)g_asset_ptrs[75])
#define kEnding_Credits_Offs_SIZE (g_asset_sizes[75])
// Map/tilemap data for the ending sequence overworld flyover.
#define kEnding_MapData ((uint16*)g_asset_ptrs[76])
#define kEnding_MapData_SIZE (g_asset_sizes[76])
// Offsets into ending scene 0 animation data.
#define kEnding0_Offs ((uint16*)g_asset_ptrs[77])
#define kEnding0_Offs_SIZE (g_asset_sizes[77])
// Raw animation frame data for ending scene 0 (Triforce room).
#define kEnding0_Data ((uint8*)g_asset_ptrs[78])
#define kEnding0_Data_SIZE (g_asset_sizes[78])
#define kPalette_DungBgMain ((uint16*)g_asset_ptrs[79])
#define kPalette_DungBgMain_SIZE (g_asset_sizes[79])
#define kPalette_MainSpr ((uint16*)g_asset_ptrs[80])
#define kPalette_MainSpr_SIZE (g_asset_sizes[80])
#define kPalette_ArmorAndGloves ((uint16*)g_asset_ptrs[81])
#define kPalette_ArmorAndGloves_SIZE (g_asset_sizes[81])
#define kPalette_Sword ((uint16*)g_asset_ptrs[82])
#define kPalette_Sword_SIZE (g_asset_sizes[82])
#define kPalette_Shield ((uint16*)g_asset_ptrs[83])
#define kPalette_Shield_SIZE (g_asset_sizes[83])
#define kPalette_SpriteAux3 ((uint16*)g_asset_ptrs[84])
#define kPalette_SpriteAux3_SIZE (g_asset_sizes[84])
#define kPalette_MiscSprite_Indoors ((uint16*)g_asset_ptrs[85])
#define kPalette_MiscSprite_Indoors_SIZE (g_asset_sizes[85])
#define kPalette_SpriteAux1 ((uint16*)g_asset_ptrs[86])
#define kPalette_SpriteAux1_SIZE (g_asset_sizes[86])
#define kPalette_OverworldBgMain ((uint16*)g_asset_ptrs[87])
#define kPalette_OverworldBgMain_SIZE (g_asset_sizes[87])
#define kPalette_OverworldBgAux12 ((uint16*)g_asset_ptrs[88])
#define kPalette_OverworldBgAux12_SIZE (g_asset_sizes[88])
#define kPalette_OverworldBgAux3 ((uint16*)g_asset_ptrs[89])
#define kPalette_OverworldBgAux3_SIZE (g_asset_sizes[89])
#define kPalette_PalaceMapBg ((uint16*)g_asset_ptrs[90])
#define kPalette_PalaceMapBg_SIZE (g_asset_sizes[90])
#define kPalette_PalaceMapSpr ((uint16*)g_asset_ptrs[91])
#define kPalette_PalaceMapSpr_SIZE (g_asset_sizes[91])
#define kHudPalData ((uint16*)g_asset_ptrs[92])
#define kHudPalData_SIZE (g_asset_sizes[92])
#define kOverworldMapPaletteData ((uint16*)g_asset_ptrs[93])
#define kOverworldMapPaletteData_SIZE (g_asset_sizes[93])
#define kDialogue(idx) FindInAssetArray(94, idx)
#define kDialogueFont(idx) FindInAssetArray(95, idx)
#define kDialogueMap(idx) FindInAssetArray(96, idx)
#define kDungMap_FloorLayout(idx) FindInAssetArray(97, idx)
#define kDungMap_Tiles(idx) FindInAssetArray(98, idx)
#define kBgTilemap_0 ((uint8*)g_asset_ptrs[99])
#define kBgTilemap_0_SIZE (g_asset_sizes[99])
#define kBgTilemap_1 ((uint8*)g_asset_ptrs[100])
#define kBgTilemap_1_SIZE (g_asset_sizes[100])
#define kBgTilemap_2 ((uint8*)g_asset_ptrs[101])
#define kBgTilemap_2_SIZE (g_asset_sizes[101])
#define kBgTilemap_3 ((uint8*)g_asset_ptrs[102])
#define kBgTilemap_3_SIZE (g_asset_sizes[102])
#define kBgTilemap_4 ((uint8*)g_asset_ptrs[103])
#define kBgTilemap_4_SIZE (g_asset_sizes[103])
#define kBgTilemap_5 ((uint8*)g_asset_ptrs[104])
#define kBgTilemap_5_SIZE (g_asset_sizes[104])
#define kOverworld_Hibytes_Comp(idx) FindInAssetArray(105, idx)
#define kOverworld_Lobytes_Comp(idx) FindInAssetArray(106, idx)
#define kOverworldMapIsSmall ((uint8*)g_asset_ptrs[107])
#define kOverworldMapIsSmall_SIZE (g_asset_sizes[107])
#define kOverworldAuxTileThemeIndexes ((uint8*)g_asset_ptrs[108])
#define kOverworldAuxTileThemeIndexes_SIZE (g_asset_sizes[108])
#define kOverworldBgPalettes ((uint8*)g_asset_ptrs[109])
#define kOverworldBgPalettes_SIZE (g_asset_sizes[109])
#define kOverworld_SignText ((uint16*)g_asset_ptrs[110])
#define kOverworld_SignText_SIZE (g_asset_sizes[110])
#define kOwMusicSets ((uint8*)g_asset_ptrs[111])
#define kOwMusicSets_SIZE (g_asset_sizes[111])
#define kOwMusicSets2 ((uint8*)g_asset_ptrs[112])
#define kOwMusicSets2_SIZE (g_asset_sizes[112])
#define kBirdTravel_ScreenIndex ((uint16*)g_asset_ptrs[113])
#define kBirdTravel_ScreenIndex_SIZE (g_asset_sizes[113])
#define kBirdTravel_Map16LoadSrcOff ((uint16*)g_asset_ptrs[114])
#define kBirdTravel_Map16LoadSrcOff_SIZE (g_asset_sizes[114])
#define kBirdTravel_ScrollX ((uint16*)g_asset_ptrs[115])
#define kBirdTravel_ScrollX_SIZE (g_asset_sizes[115])
#define kBirdTravel_ScrollY ((uint16*)g_asset_ptrs[116])
#define kBirdTravel_ScrollY_SIZE (g_asset_sizes[116])
#define kBirdTravel_LinkXCoord ((uint16*)g_asset_ptrs[117])
#define kBirdTravel_LinkXCoord_SIZE (g_asset_sizes[117])
#define kBirdTravel_LinkYCoord ((uint16*)g_asset_ptrs[118])
#define kBirdTravel_LinkYCoord_SIZE (g_asset_sizes[118])
#define kBirdTravel_CameraXScroll ((uint16*)g_asset_ptrs[119])
#define kBirdTravel_CameraXScroll_SIZE (g_asset_sizes[119])
#define kBirdTravel_CameraYScroll ((uint16*)g_asset_ptrs[120])
#define kBirdTravel_CameraYScroll_SIZE (g_asset_sizes[120])
#define kBirdTravel_Unk1 ((int8*)g_asset_ptrs[121])
#define kBirdTravel_Unk1_SIZE (g_asset_sizes[121])
#define kBirdTravel_Unk3 ((int8*)g_asset_ptrs[122])
#define kBirdTravel_Unk3_SIZE (g_asset_sizes[122])
#define kWhirlpoolAreas ((uint16*)g_asset_ptrs[123])
#define kWhirlpoolAreas_SIZE (g_asset_sizes[123])
#define kOverworld_Entrance_Area ((uint16*)g_asset_ptrs[124])
#define kOverworld_Entrance_Area_SIZE (g_asset_sizes[124])
#define kOverworld_Entrance_Pos ((uint16*)g_asset_ptrs[125])
#define kOverworld_Entrance_Pos_SIZE (g_asset_sizes[125])
#define kOverworld_Entrance_Id ((uint8*)g_asset_ptrs[126])
#define kOverworld_Entrance_Id_SIZE (g_asset_sizes[126])
#define kFallHole_Area ((uint16*)g_asset_ptrs[127])
#define kFallHole_Area_SIZE (g_asset_sizes[127])
#define kFallHole_Pos ((uint16*)g_asset_ptrs[128])
#define kFallHole_Pos_SIZE (g_asset_sizes[128])
#define kFallHole_Entrances ((uint8*)g_asset_ptrs[129])
#define kFallHole_Entrances_SIZE (g_asset_sizes[129])
#define kExitData_ScreenIndex ((uint8*)g_asset_ptrs[130])
#define kExitData_ScreenIndex_SIZE (g_asset_sizes[130])
#define kExitDataRooms ((uint16*)g_asset_ptrs[131])
#define kExitDataRooms_SIZE (g_asset_sizes[131])
#define kExitData_Map16LoadSrcOff ((uint16*)g_asset_ptrs[132])
#define kExitData_Map16LoadSrcOff_SIZE (g_asset_sizes[132])
#define kExitData_ScrollX ((uint16*)g_asset_ptrs[133])
#define kExitData_ScrollX_SIZE (g_asset_sizes[133])
#define kExitData_ScrollY ((uint16*)g_asset_ptrs[134])
#define kExitData_ScrollY_SIZE (g_asset_sizes[134])
#define kExitData_XCoord ((uint16*)g_asset_ptrs[135])
#define kExitData_XCoord_SIZE (g_asset_sizes[135])
#define kExitData_YCoord ((uint16*)g_asset_ptrs[136])
#define kExitData_YCoord_SIZE (g_asset_sizes[136])
#define kExitData_CameraXScroll ((uint16*)g_asset_ptrs[137])
#define kExitData_CameraXScroll_SIZE (g_asset_sizes[137])
#define kExitData_CameraYScroll ((uint16*)g_asset_ptrs[138])
#define kExitData_CameraYScroll_SIZE (g_asset_sizes[138])
#define kExitData_NormalDoor ((uint16*)g_asset_ptrs[139])
#define kExitData_NormalDoor_SIZE (g_asset_sizes[139])
#define kExitData_FancyDoor ((uint16*)g_asset_ptrs[140])
#define kExitData_FancyDoor_SIZE (g_asset_sizes[140])
#define kExitData_Unk1 ((int8*)g_asset_ptrs[141])
#define kExitData_Unk1_SIZE (g_asset_sizes[141])
#define kExitData_Unk3 ((int8*)g_asset_ptrs[142])
#define kExitData_Unk3_SIZE (g_asset_sizes[142])
#define kSpExit_Top ((uint16*)g_asset_ptrs[143])
#define kSpExit_Top_SIZE (g_asset_sizes[143])
#define kSpExit_Bottom ((uint16*)g_asset_ptrs[144])
#define kSpExit_Bottom_SIZE (g_asset_sizes[144])
#define kSpExit_Left ((uint16*)g_asset_ptrs[145])
#define kSpExit_Left_SIZE (g_asset_sizes[145])
#define kSpExit_Right ((uint16*)g_asset_ptrs[146])
#define kSpExit_Right_SIZE (g_asset_sizes[146])
#define kSpExit_Tab4 ((int16*)g_asset_ptrs[147])
#define kSpExit_Tab4_SIZE (g_asset_sizes[147])
#define kSpExit_Tab5 ((int16*)g_asset_ptrs[148])
#define kSpExit_Tab5_SIZE (g_asset_sizes[148])
#define kSpExit_Tab6 ((int16*)g_asset_ptrs[149])
#define kSpExit_Tab6_SIZE (g_asset_sizes[149])
#define kSpExit_Tab7 ((int16*)g_asset_ptrs[150])
#define kSpExit_Tab7_SIZE (g_asset_sizes[150])
#define kSpExit_LeftEdgeOfMap ((uint16*)g_asset_ptrs[151])
#define kSpExit_LeftEdgeOfMap_SIZE (g_asset_sizes[151])
#define kSpExit_Dir ((uint8*)g_asset_ptrs[152])
#define kSpExit_Dir_SIZE (g_asset_sizes[152])
#define kSpExit_SprGfx ((uint8*)g_asset_ptrs[153])
#define kSpExit_SprGfx_SIZE (g_asset_sizes[153])
#define kSpExit_AuxGfx ((uint8*)g_asset_ptrs[154])
#define kSpExit_AuxGfx_SIZE (g_asset_sizes[154])
#define kSpExit_PalBg ((uint8*)g_asset_ptrs[155])
#define kSpExit_PalBg_SIZE (g_asset_sizes[155])
#define kSpExit_PalSpr ((uint8*)g_asset_ptrs[156])
#define kSpExit_PalSpr_SIZE (g_asset_sizes[156])
#define kOverworldSecrets_Offs ((uint16*)g_asset_ptrs[157])
#define kOverworldSecrets_Offs_SIZE (g_asset_sizes[157])
#define kOverworldSecrets ((uint8*)g_asset_ptrs[158])
#define kOverworldSecrets_SIZE (g_asset_sizes[158])
#define kOverworldSpriteOffs ((uint16*)g_asset_ptrs[159])
#define kOverworldSpriteOffs_SIZE (g_asset_sizes[159])
#define kOverworldSprites ((uint8*)g_asset_ptrs[160])
#define kOverworldSprites_SIZE (g_asset_sizes[160])
#define kOverworldSpriteGfx ((uint8*)g_asset_ptrs[161])
#define kOverworldSpriteGfx_SIZE (g_asset_sizes[161])
#define kOverworldSpritePalettes ((uint8*)g_asset_ptrs[162])
#define kOverworldSpritePalettes_SIZE (g_asset_sizes[162])
#define kMap8DataToTileAttr ((uint8*)g_asset_ptrs[163])
#define kMap8DataToTileAttr_SIZE (g_asset_sizes[163])
#define kSomeTileAttr ((uint8*)g_asset_ptrs[164])
#define kSomeTileAttr_SIZE (g_asset_sizes[164])
#define kAssets_Sig 90, 101, 108, 100, 97, 51, 95, 118, 48, 32, 32, 32, 32, 32, 10, 0, 27, 174, 233, 45, 74, 174, 252, 50, 49, 27, 153, 197, 27, 43, 216, 197, 132, 101, 173, 169, 36, 108, 15, 155, 176, 169, 57, 131, 174, 101, 51, 207
