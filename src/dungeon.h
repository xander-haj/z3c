/*
 * dungeon.h - Dungeon Room System
 *
 * Declares the complete interface for the dungeon subsystem in the Zelda 3
 * C reimplementation. Dungeons are composed of rooms, each defined by tile
 * data, door configurations, sprite placements, room tags (puzzle triggers),
 * and layer effects. This header covers:
 *
 *   - Room loading and tile map drawing (objects, floors, doors)
 *   - Door types and their rendering/attribute functions
 *   - Inter-room and intra-room transitions (scrolling, stairs, warps, pits)
 *   - Room tags: puzzle mechanics (switches, torches, push blocks, chests,
 *     moving walls, water gates, holes, and boss-specific triggers)
 *   - Dungeon camera and quadrant management
 *   - Lighting and layer effects (lamp cone, grayscale, water flood)
 *   - Module07 submodule handlers (the dungeon main loop state machine)
 *
 * The SNES dungeon engine uses a 64x64 tile map divided into four 32x32
 * quadrants. Each room can span 1-4 quadrants, with BG1 and BG2 layers
 * for overlapping geometry (e.g., bridges, balconies). Door tiles are
 * drawn into the tile map at specific offsets and have matching collision
 * attributes loaded into a parallel attribute table.
 *
 * Depends on: types.h (via sprite.h or direct include)
 * Used by: dungeon.c, and referenced by sprite, player, and overworld modules
 *          for room state queries and transition coordination.
 */
#pragma once

// Forward declaration: room boundary coordinates for edge detection
typedef struct RoomBounds RoomBounds;

/*
 * Door type enumeration.
 *
 * Each door in a dungeon room has a type that determines its visual
 * appearance, collision behavior, and interaction rules. Values are
 * even-numbered because the original SNES code uses them as 16-bit
 * table indices (multiplied by 2 from a base index).
 */
enum {
  // --- Standard passage doors ---
  kDoorType_Regular = 0,            // Normal open doorway
  kDoorType_Regular2 = 2,           // Normal open doorway (alternate tileset)
  kDoorType_4 = 4,                  // Unused/variant regular door
  kDoorType_EntranceDoor = 6,       // Dungeon entrance from overworld
  kDoorType_WaterfallTunnel = 8,    // Hidden passage behind a waterfall
  kDoorType_EntranceLarge = 10,     // Large dungeon entrance (palace front)
  kDoorType_EntranceLarge2 = 12,    // Large entrance alternate variant
  kDoorType_EntranceCave = 14,      // Cave-style entrance
  kDoorType_EntranceCave2 = 16,     // Cave entrance alternate variant
  kDoorType_ExitToOw = 18,          // Door leading back to the overworld
  kDoorType_ThroneRoom = 20,        // Throne room door (Hyrule Castle)
  kDoorType_PlayerBgChange = 22,    // Triggers BG layer swap when traversed
  kDoorType_ShuttersTwoWay = 24,    // Shutter that opens from either side
  kDoorType_InvisibleDoor = 26,     // Door with no visible frame (bombable?)

  // --- Locked and key doors ---
  kDoorType_SmallKeyDoor = 0x1c,    // Requires a small key to unlock
  kDoorType_1E = 0x1e,              // Variant locked door
  kDoorType_StairMaskLocked0 = 32,  // Staircase behind locked mask (variant 0)
  kDoorType_StairMaskLocked1 = 34,  // Staircase behind locked mask (variant 1)
  kDoorType_StairMaskLocked2 = 36,  // Staircase behind locked mask (variant 2)
  kDoorType_StairMaskLocked3 = 38,  // Staircase behind locked mask (variant 3)

  // --- Destructible doors ---
  kDoorType_BreakableWall = 0x28,   // Cracked wall opened by bombs or dashing
  kDoorType_LgExplosion = 48,       // Large blast wall (super bomb required)
  kDoorType_Slashable = 50,         // Curtain door opened by sword slash
  kDoorType_36 = 0x36,              // Destructible variant
  kDoorType_38 = 0x38,              // Destructible variant

  // --- Mechanically operated doors ---
  kDoorType_RegularDoor33 = 64,     // Standard door (high range index)
  kDoorType_Shutter = 68,           // Shutter: opens when room is cleared
  kDoorType_WarpRoomDoor = 70,      // Door in warp tile rooms
  kDoorType_ShutterTrapUR = 72,     // One-way shutter trap (up/right)
  kDoorType_ShutterTrapDL = 74,     // One-way shutter trap (down/left)
};

/*
 * DungPalInfo - Dungeon room palette configuration.
 *
 * Each dungeon room specifies four palette indices that select which
 * color palettes to load for BG tiles and sprites. These map into the
 * master palette table to give each room its distinct color scheme.
 */
typedef struct DungPalInfo {
  uint8 pal0;   // Primary BG palette index (walls, floor base)
  uint8 pal1;   // Secondary BG palette index (decorative elements)
  uint8 pal2;   // Sprite palette index (enemies, objects)
  uint8 pal3;   // Auxiliary palette index (special effects, overlays)
} DungPalInfo;

// ============================================================================
// Tile Map Drawing Primitives
// ============================================================================
// These functions write 16-bit tile entries into the BG tile map buffers.
// "dsto" parameters are tile map offsets (in 16-bit words) from the BG base.
// "src" pointers reference tile data arrays in ROM.

// Animated tile indices for dungeon water, lava, and conveyor animations
extern const uint8 kDungAnimatedTiles[24];
// Convert a tile map offset to a pointer into the BG tile buffer
uint16 *DstoPtr(uint16 d);
// Fill n tiles in a vertical column from src into dst
void Object_Fill_Nx1(int n, const uint16 *src, uint16 *dst);
void Object_Draw_5x4(const uint16 *src, uint16 *dst);
void Object_Draw_4x2_BothBgs(const uint16 *src, uint16 dsto);
void Object_ChestPlatform_Helper(const uint16 *src, int dsto);
// Draw a pit/hole in the floor (removes tiles and sets pit collision)
void Object_Hole(const uint16 *src, uint16 *dst);
// Type 1 room objects: variable-size furniture, walls, and decorations
void LoadType1ObjectSubtype1(uint8 idx, uint16 *dst, uint16 dsto);
void Object_DrawNx3_BothBgs(int n, const uint16 *src, int dsto);
void LoadType1ObjectSubtype2(uint8 idx, uint16 *dst, uint16 dsto);
// Draw bombable floor section with separate above/below tile layers
void Object_BombableFloorHelper(uint16 a, const uint16 *src, const uint16 *src_below, uint16 *dst, uint16 dsto);
void LoadType1ObjectSubtype3(uint8 idx, uint16 *dst, uint16 dsto);

// --- Room Boundary Adjustments ---
// Expand or contract room boundaries during scrolling transitions.
// These modify the visible area limits that constrain camera movement.
void RoomBounds_AddA(RoomBounds *r);
void RoomBounds_AddB(RoomBounds *r);
void RoomBounds_SubB(RoomBounds *r);
void RoomBounds_SubA(RoomBounds *r);
// ============================================================================
// Inter-Room Transition Initiation
// ============================================================================
// Begin a scrolling transition to an adjacent room in each cardinal direction.
// These set up the target room, scroll direction, and BG layer state.

void Dungeon_StartInterRoomTrans_Left();
void Dung_StartInterRoomTrans_Left_Plus();
void Dungeon_StartInterRoomTrans_Up();
void Dungeon_StartInterRoomTrans_Down();

// Write a 2x2 tile block at the given tile map position with attribute byte
void Dungeon_Store2x2(uint16 pos, uint16 t0, uint16 t1, uint16 t2, uint16 t3, uint8 attr);
// Convert a tile map position to a VRAM address (accounts for BG layer swap)
uint16 Dungeon_MapVramAddr(uint16 pos);
// Convert tile map position to VRAM address without layer swap adjustment
uint16 Dungeon_MapVramAddrNoSwap(uint16 pos);
// ============================================================================
// Door Drawing (Per-Direction Entrance Doors)
// ============================================================================

void Door_Up_EntranceDoor(uint16 dsto);
void Door_Down_EntranceDoor(uint16 dsto);
void Door_Left_EntranceDoor(uint16 dsto);
void Door_Right_EntranceDoor(uint16 dsto);
// Dispatch helper: draw door tiles for a given type and position offset
void Door_Draw_Helper4(uint8 door_type, uint16 dsto);

// ============================================================================
// Room Data Access (ROM Lookups)
// ============================================================================

// Get pointer to the door info table for a specific room number
const uint16 *GetRoomDoorInfo(int room);
// Get pointer to the room header (palette, tileset, tags, floor config)
const uint8 *GetRoomHeaderPtr(int room);
// Get the default room tile layout data (used when no override exists)
const uint8 *GetDefaultRoomLayout(int i);
// Get the dungeon-specific room tile layout data
const uint8 *GetDungeonRoomLayout(int i);
// ============================================================================
// Push Blocks, Locked Doors, and Room Utilities
// ============================================================================

// Room tag handler for hole/chest reveal mechanics (tags 0x22 and 0x3B)
void Dung_TagRoutine_0x22_0x3B(int k, uint8 j);
// Process a single push block's movement, collision, and tile update
void Sprite_HandlePushedBlocks_One(int i);
// Draw left/right door frames as 3x4 tile blocks
void Object_Draw_DoorLeft_3x4(uint16 src, int door);
void Object_Draw_DoorRight_3x4(uint16 src, int door);
// Animate and execute the locked door opening sequence
void Dungeon_OpeningLockedDoor_Combined(bool skip_anim);
// Look up the palette info struct for a dungeon palette index
const DungPalInfo *GetDungPalInfo(int idx);
// Get the telepathic tile message ID for a dungeon room
uint16 Dungeon_GetTeleMsg(int room);
// Check if the pit Link fell into deals damage (vs. transition pits)
bool Dungeon_IsPitThatHurtsPlayer();
// Prepare the next room quadrant's tile data for VRAM DMA upload
void Dungeon_PrepareNextRoomQuadrantUpload();
// Build a single quadrant of water flood tile data for VRAM transfer
void WaterFlood_BuildOneQuadrantForVRAM();
// Reset water overlay on tile map when water tag is not active
void TileMapPrep_NotWaterOnTag();
// Rotate the lamp's light cone to follow Link's facing direction
void OrientLampLightCone();
// Set up the room exit warp after defeating a dungeon boss
void PrepareDungeonExitFromBossFight();
// Save the current dungeon's enemy kill count to SRAM
void SavePalaceDeaths();
// ============================================================================
// Room Loading and Object Drawing
// ============================================================================
// The room loading pipeline: parse header -> draw floors -> draw objects
// -> draw doors -> load attributes -> load sprites. Each object in the
// room data stream is decoded and rendered into the BG tile map buffers.

// Master room load: header, tiles, doors, attributes, sprites, overlays
void Dungeon_LoadRoom();
// Iterate the room's object data stream and draw each object
void RoomDraw_DrawAllObjects(const uint8 *level_data);
// Draw a door object (dispatches to directional door drawing functions)
void RoomData_DrawObject_Door(uint16 a);
// Draw a single room object (wall, decoration, furniture, etc.)
void RoomData_DrawObject(uint16 r0, const uint8 *level_data);
// Draw the floor tiles for the room (before objects are overlaid)
void RoomDraw_DrawFloors(const uint8 *level_data);
// Fill floor regions with repeating tile chunks
void RoomDraw_FloorChunks(const uint16 *src);
// Draw n copies of a 32x32 block in sequence
void RoomDraw_A_Many32x32Blocks(int n, const uint16 *src, uint16 *dst);
void RoomDraw_1x3_rightwards(int n, const uint16 *src, uint16 *dst);
// Check if the room contains a moving wall that has shifted position
bool RoomDraw_CheckIfWallIsMoved();
// Fill the tile replacement buffer for a moving wall segment
void MovingWall_FillReplacementBuffer(int dsto);
void Object_Table_Helper(const uint16 *src, uint16 *dst);
// Draw water surface tiles into the tile map
void DrawWaterThing(uint16 *dst, const uint16 *src);
void RoomDraw_4x4(const uint16 *src, uint16 *dst);
void RoomDraw_Object_Nx4(int n, const uint16 *src, uint16 *dst);
// Draw an Nx4 object on both BG1 and BG2 layers simultaneously
void Object_DrawNx4_BothBgs(int n, const uint16 *src, int dsto);
void RoomDraw_Rightwards2x2(const uint16 *src, uint16 *dst);
void Object_Draw_3x2(const uint16 *src, uint16 *dst);
// Draw an object that spans over water (bridge, walkway)
void RoomDraw_WaterHoldingObject(int n, const uint16 *src, uint16 *dst);
// Draw large decorative objects (statues, pillars, thrones)
void RoomDraw_SomeBigDecors(int n, const uint16 *src, uint16 dsto);
// Draw a single lamp light cone overlay at the given tile position
void RoomDraw_SingleLampCone(uint16 a, uint16 y);
// Draw Agahnim's Tower stained glass windows (unique room decoration)
void RoomDraw_AgahnimsWindows(uint16 dsto);
// Draw the fortune teller's room interior (crystal ball, curtains)
void RoomDraw_FortuneTellerRoom(uint16 dsto);
// Draw a full 8x8 tile block object
void Object_Draw8x8(const uint16 *src, uint16 *dst);
// ============================================================================
// Directional Door Drawing and Priority
// ============================================================================
// Doors are drawn per-direction (N/S/E/W) because each has different tile
// layouts and collision geometry. "High priority" doors render in front of
// Link; "normal range" vs "high range" distinguishes small vs large doors.

void RoomDraw_Door_North(int type, int pos_enum);
void Door_Up_StairMaskLocked(uint8 door_type, uint16 dsto);
void Door_PrioritizeCurDoor();
void RoomDraw_NormalRangedDoors_North(uint8 door_type, uint16 dsto, int pos_enum);
void RoomDraw_OneSidedShutters_North(uint8 door_type, uint16 dsto);
void RoomDraw_Door_South(int type, int pos_enum);
void RoomDraw_CheckIfLowerLayerDoors_Y(uint8 door_type, uint16 dsto);
void RoomDraw_Door_West(int type, int pos_enum);
void RoomDraw_NormalRangedDoors_West(uint8 door_type, uint16 dsto, int pos_enum);
void RoomDraw_Door_East(int type, int pos_enum);
void RoomDraw_NormalRangedDoors_East(uint8 door_type, uint16 dsto);
void RoomDraw_OneSidedShutters_East(uint8 door_type, uint16 dsto);
void RoomDraw_NorthCurtainDoor(uint16 dsto);
void RoomDraw_Door_ExplodingWall(int pos_enum);
void RoomDraw_ExplodingWallSegment(const uint16 *src, uint16 dsto);
void RoomDraw_ExplodingWallColumn(const uint16 *src, uint16 *dst);
void RoomDraw_HighRangeDoor_North(uint8 door_type, uint16 dsto, int pos_enum);
void RoomDraw_OneSidedLowerShutters_South(uint8 door_type, uint16 dsto);
void RoomDraw_HighRangeDoor_West(uint8 door_type, uint16 dsto, int pos_enum);
void RoomDraw_OneSidedLowerShutters_East(uint8 door_type, uint16 dsto);
void RoomDraw_MakeDoorHighPriority_North(uint16 dsto);
void RoomDraw_MakeDoorHighPriority_South(uint16 dsto);
void RoomDraw_MakeDoorHighPriority_West(uint16 dsto);
void RoomDraw_MakeDoorHighPriority_East(uint16 dsto);
// Mark a door position as a dungeon toggle door (crystal switch operated)
void RoomDraw_MarkDungeonToggleDoor(uint16 dsto);
// Mark a door position as a layer toggle door (BG1<->BG2 swap)
void RoomDraw_MarkLayerToggleDoor(uint16 dsto);

// --- Object size decoders ---
// Parse the size field from room data (used for variable-length objects)
void RoomDraw_GetObjectSize_1to16();
void Object_SizeAtoAplus15(uint8 a);
void RoomDraw_GetObjectSize_1to15or26();
void RoomDraw_GetObjectSize_1to15or32();

// Process door flags and determine the final rendered door type
int RoomDraw_FlagDoorsAndGetFinalType(uint8 direction, uint8 door_type, uint16 dsto);
// Set high-priority tile flag on door parts (vertical/horizontal variants)
void RoomDraw_MakeDoorPartsHighPriority_Y(uint16 dsto);
void RoomDraw_MakeDoorPartsHighPriority_X(uint16 dsto);
void RoomDraw_Downwards4x2VariableSpacing(int increment, const uint16 *src, uint16 *dst);
// ============================================================================
// Interactive Object Drawing (Pots, Blocks, Torches, Pegs)
// ============================================================================
// These objects have both visual tiles and special collision attributes
// because the player can interact with them (lift, push, hammer, bomb).

uint16 *RoomDraw_DrawObject2x2and1(const uint16 *src, uint16 *dst);
uint16 *RoomDraw_RightwardShelfEnd(const uint16 *src, uint16 *dst);
uint16 *RoomDraw_RightwardBarSegment(const uint16 *src, uint16 *dst);
void DrawBigGraySegment(uint16 a, const uint16 *src, uint16 *dst, uint16 dsto);
// Draw a single pot (liftable object with potential hidden item)
void RoomDraw_SinglePot(const uint16 *src, uint16 *dst, uint16 dsto);
// Draw a section of bombable floor (visually identical to normal floor)
void RoomDraw_BombableFloor(const uint16 *src, uint16 *dst, uint16 dsto);
// Draw a single hammer peg (driven down when hit with the Magic Hammer)
void RoomDraw_HammerPegSingle(const uint16 *src, uint16 *dst, uint16 dsto);
// Draw a pushable block and register it in the push block slot table
void DrawObjects_PushableBlock(uint16 dsto_x2, uint16 slot);
// Draw a lightable torch and register it in the torch slot table
void DrawObjects_LightableTorch(uint16 dsto_x2, uint16 slot);
// ============================================================================
// Room Header, Attributes, and Door Attributes
// ============================================================================
// The attribute table is a parallel data structure to the tile map that
// stores collision properties, interaction type, and layer info for each
// tile position. It determines what happens when Link walks on, dashes
// into, or bombs a particular tile.

// Parse the room header bytes (palette, tileset, tag, floor, BG2 property)
void Dungeon_LoadHeader();
// Check neighboring rooms for open doors that should connect to this room
void Dungeon_CheckAdjacentRoomsForOpenDoors(int idx, int room);
// Load door data from an adjacent room (for scrolling transitions)
void Dungeon_LoadAdjacentRoomDoors(int room);
// Load the selectable attribute table variant (used for special rooms)
void Dungeon_LoadAttribute_Selectable();
// Load the full tile attribute table for the current room
void Dungeon_LoadAttributeTable();
// Fill attribute table with basic floor attributes for 'loops' tiles
void Dungeon_LoadBasicAttribute_full(uint16 loops);
// Load collision attributes for all room objects
void Dungeon_LoadObjectAttribute();
// Load collision attributes for all doors in the current room
void Dungeon_LoadDoorAttribute();
// Load the attribute for a single door slot k
void Dungeon_LoadSingleDoorAttribute(int k);
// Load the attribute footprint for a blast/exploding wall door
void Door_LoadBlastWallAttr(int k);
// Convert a door tile into a floor switch tile (for switch rooms)
void ChangeDoorToSwitch();
// Toggle orange/blue crystal peg attributes throughout the room
void Dungeon_FlipCrystalPegAttribute();
// ============================================================================
// Room Tags (Puzzle Mechanics)
// ============================================================================
// Room tags define the interactive puzzle logic for each dungeon room.
// Each room can have up to two tags that respond to switches, torches,
// enemy kills, push blocks, or positional triggers. When conditions are
// met, tags open doors, reveal chests, drain water, or activate traps.

// Main dispatcher: evaluate both room tags each frame
void Dungeon_HandleRoomTags();
// Null tag handler (room has no active puzzle)
void Dung_TagRoutine_0x00(int k);
// Detect if Link is standing on a staircase tile
void Dungeon_DetectStaircase();
// --- Positional and Quadrant Triggers ---
// Activate when Link enters a specific region of the room
void RoomTag_NorthWestTrigger(int k);
void Dung_TagRoutine_0x2A(int k);
void Dung_TagRoutine_0x2B(int k);
void Dung_TagRoutine_0x2C(int k);
void Dung_TagRoutine_0x2D(int k);
void Dung_TagRoutine_0x2E(int k);
void Dung_TagRoutine_0x2F(int k);
void Dung_TagRoutine_0x30(int k);
void RoomTag_QuadrantTrigger(int k);
// Open all trapdoors upward (when kill-all-enemies condition is met)
void Dung_TagRoutine_TrapdoorsUp();

// --- Kill/Clear Triggers ---
// Activate when all enemies in the room or a quadrant are defeated
void RoomTag_RoomTrigger(int k);
// Boss rooms where the boss respawns if you leave and return
void RoomTag_RekillableBoss(int k);
// Open a blocked door when the room is cleared
void RoomTag_RoomTrigger_BlockDoor(int k);
// Open door and drop a prize when enemies are cleared
void RoomTag_PrizeTriggerDoorDoor(int k);

// --- Switch and Pressure Plate Triggers ---
void RoomTag_SwitchTrigger_HoldDoor(int k);   // Door stays open while held
void RoomTag_SwitchTrigger_ToggleDoor(int k);  // Door toggles on each press
// Push down a floor pressure plate (changes tile attribute to "pressed")
void PushPressurePlate(uint8 attr);

// --- Torch Puzzle Triggers ---
// Open a door when all torches in the room are lit
void RoomTag_TorchPuzzleDoor(int k);

// --- Explosion Triggers ---
void RoomTag_Switch_ExplodingWall(int k);
void RoomTag_PullSwitchExplodingWall(int k);
void Dung_TagRoutine_BlastWallStuff(int k);

// --- Prize and Boss Triggers ---
// Spawn a heart container or prize item as a room reward
void RoomTag_GetHeartForPrize(int k);
// Agahnim's Tower: special defeat/escape logic
void RoomTag_Agahnim(int k);
// Ganon's Tower: open the final door
void RoomTag_GanonDoor(int tagidx);

// --- Block and Chest Triggers ---
// Remove a blocking object when room is cleared
void RoomTag_KillRoomBlock(int k);
// Reveal a chest when a specific push block reaches its target
void RoomTag_PushBlockForChest(int k);
// Open/reveal a chest based on room tag conditions
void RoomTag_TriggerChest(int k);
// Animate the chest appearance when revealed by a trigger
void RoomTag_OperateChestReveal(int k);
// Reveal a chest when all torches in the room are lit
void RoomTag_TorchPuzzleChest(int k);
// --- Moving Wall Puzzles ---
// Walls that advance toward Link, creating a time-pressure puzzle
void RoomTag_MovingWall_East(int k);
void RoomTag_MovingWallShakeItUp(int k);
void RoomTag_MovingWall_West(int k);
// Check if torches are lit to stop the moving wall
void RoomTag_MovingWallTorchesCheck(int k);
// Advance the wall by one tile increment
int MovingWall_MoveALittle();
int RoomTag_AdvanceGiganticWall(int k);

// --- Water Level Triggers ---
void RoomTag_WaterOff(int k);     // Drain water from the room
void RoomTag_WaterOn(int k);      // Fill the room with water
void RoomTag_WaterGate(int k);    // Open/close the Swamp Palace floodgate
void Dung_TagRoutine_0x1B(int k);

// --- Floor Hole Triggers ---
// Create holes in the floor when conditions are met (switch, kill, etc.)
void RoomTag_Holes0(int k);
void Dung_TagRoutine_0x23(int k);
void Dung_TagRoutine_0x34(int k);
void Dung_TagRoutine_0x35(int k);
void Dung_TagRoutine_0x36(int k);
void Dung_TagRoutine_0x37(int k);
void Dung_TagRoutine_0x39(int k);
void Dung_TagRoutine_0x3A(int k);
void Dung_TagRoutine_Func2(uint8 av);
// Holes that appear beneath chests when the chest is opened
void RoomTag_ChestHoles0(int k);
void Dung_TagRoutine_0x3B(int k);
void RoomTag_Holes2(int k);

// --- Tag Utility Functions ---
// Update floor tiles to reflect current water level state
void RoomTag_OperateWaterFlooring();
// Check if any shutter doors need to respond to current room state
bool RoomTag_MaybeCheckShutters(uint8 *attr_out);
// Calculate tile map coordinates for the current tag's target position
int RoomTag_GetTilemapCoords();
// Scan the room for a pressed floor switch; outputs attribute if found
bool RoomTag_CheckForPressedSwitch(uint8 *y_out);
// ============================================================================
// Door Operation and Destructibles
// ============================================================================

// Check all torches for lit state and process door open/close accordingly
void Dungeon_ProcessTorchesAndDoors();
// Test if a bomb explosion at (x,y) destroys any nearby destructible tiles
void Bomb_CheckForDestructibles(uint16 x, uint16 y, uint8 r14);
// Animate step 1 of a door opening (DMA tile data into the transition buf)
int DrawDoorOpening_Step1(int door, int dma_ptr);
// Draw the animated opening steps for a shutter door
void DrawShutterDoorSteps(int door);
// Draw the eye-watch door (opens when all enemies in view are defeated)
void DrawEyeWatchDoor(int door);
// Draw the exploding blast wall animation frames
void Door_BlastWallExploding_Draw(int dsto);
// Run the shutter door state machine (open/close/animate all shutters)
void OperateShutterDoors();
// Open a cracked door after it has been bombed or dashed
void OpenCrackedDoor();
// Load tile attributes for a toggle door (crystal switch variant)
void Dungeon_LoadToggleDoorAttr_OtherEntry(int door);
// Load tile attribute for a single door from the door attribute table
void Dungeon_LoadSingleDoorTileAttribute();
// Draw a fully opened door (remove door tiles, show open passage)
void DrawCompletelyOpenDoor();
// Remove the exploding wall tiles from the tile map after detonation
void Dungeon_ClearAwayExplodingWall();
// ============================================================================
// Tile Interaction (Lift, Push, Open, Destroy)
// ============================================================================

// Scan for a liftable tile under Link's position; returns tile ID or 0
uint16 Dungeon_CheckForAndIDLiftableTile();
// Main handler for push block movement and collision
void Dungeon_PushBlock_Handler();
// Draw a single 16x16 replacement tile at the given index
void RoomDraw_16x16Single(uint8 index);
// Check if a pushed block has reached a pit tile (falls in)
void PushBlock_CheckForPit(uint8 y);
// Lift a tile and replace it with the appropriate floor tile beneath
uint8 Dungeon_LiftAndReplaceLiftable(Point16U *pt);
// Draw the lightened hole in Thieves' Town attic (special room mechanic)
uint8 ThievesAttic_DrawLightenedHole(uint16 pos6, uint16 a, Point16U *pt);
// Process Link interacting with a tile in a dungeon (pick up, open, break)
uint8 HandleItemTileAction_Dungeon(uint16 x, uint16 y);
// Handle manipulated block post-processing (update tile map and attributes)
void ManipBlock_Something(Point16U *pt);
// Spawn the hidden item under a lifted pot at the given tile positions
void RevealPotItem(uint16 pos6, uint16 pos4);
// Write a common tile value into the tile map and queue it for VRAM upload
void Dungeon_UpdateTileMapWithCommonTile(int x, int y, uint8 v);
// Queue a sprite-induced tile change for DMA transfer to VRAM
void Dungeon_PrepSpriteInducedDma(int x, int y, uint8 v);
// Remove a rupee tile from the tile map (after Link collects it)
void Dungeon_DeleteRupeeTile(uint16 x, uint16 y);
// Open a chest at the given tile position; returns the item inside
uint8 OpenChestForItem(uint8 tile, int *chest_position);
// Open a big chest (big key required); outputs chest map position
void OpenBigChest(uint16 loc, int *chest_position);
// Open a chest in the minigame context; returns the prize item
uint8 OpenMiniGameChest(int *chest_position);
// Build the tile stripe data for a chest reveal animation
uint16 RoomTag_BuildChestStripes(uint16 pos, uint16 y);
// ============================================================================
// Water Level and Torch Mechanics
// ============================================================================

// Set tile attributes for drained water state
void Dungeon_SetAttrForActivatedWaterOff();
// Build the tile map for rising swamp water
void Dungeon_FloodSwampWater_PrepTileMap();
// Adjust water vomit tiles (waterfall) based on current depth level
void Dungeon_AdjustWaterVomit(const uint16 *src, int depth);
// Set tile attributes for flooded water state
void Dungeon_SetAttrForActivatedWater();
// Expand the dam flood area by one tile row
void FloodDam_Expand();
// Initialize the flood dam tile preparation
void FloodDam_PrepTiles_init();
// Watergate animation state 1: water rising sequence
void Watergate_Main_State1();
// Fill the dam area with water tiles
void FloodDam_Fill();
// Adjust translucency overlay when Ganon extinguishes room torches
void Ganon_ExtinguishTorch_adjust_translucency();
// Ganon boss: extinguish all torches to darken the room
void Ganon_ExtinguishTorch();
// Standard torch extinguish (timer expired or wind effect)
void Dungeon_ExtinguishTorch();
// ============================================================================
// Spiral Stairs and Room Overlay
// ============================================================================

// Set walls near spiral stairs to high BG priority (render in front of Link)
void SpiralStairs_MakeNearbyWallsHighPriority_Entering();
// Restore walls near spiral stairs to normal priority after exiting
void SpiralStairs_MakeNearbyWallsLowPriority();
// Clear the exploding wall area and draw striped replacement tiles
void ClearAndStripeExplodingWall(uint16 dsto);
// Draw the room overlay layer (BG2 decorative elements on top of BG1)
void Dungeon_DrawRoomOverlay(const uint8 *src);
// ============================================================================
// Door Draw Data and DMA Transfer (Per-Direction)
// ============================================================================
// These functions handle the multi-step door drawing process:
// 1. Get the draw data index for the door's current state
// 2. Build DMA transfer commands for the tile data
// 3. Write tiles into the tile map buffer
// Each direction (N/S/E/W) has its own set because door geometry differs.

void GetDoorDrawDataIndex_North_clean_door_index(int door);
int DoorDoorStep1_North(int door, int dma_ptr);
void GetDoorDrawDataIndex_North(int door, int r4_door);
void DrawDoorToTileMap_North(int door, int r4_door);
void Object_Draw_DoorUp_4x3(uint16 src, int door);
void GetDoorDrawDataIndex_South_clean_door_index(int door);
int DoorDoorStep1_South(int door, int dma_ptr);
void GetDoorDrawDataIndex_South(int door, int r4_door);
void DrawDoorToTileMap_South(int door, int r4_door);
void Object_Draw_DoorDown_4x3(uint16 src, int door);
void GetDoorDrawDataIndex_West_clean_door_index(int door);
int DoorDoorStep1_West(int door, int dma_ptr);
void GetDoorDrawDataIndex_West(int door, int r4_door);
void DrawDoorToTileMap_West(int door, int r4_door);
void GetDoorDrawDataIndex_East_clean_door_index(int door);
int DoorDoorStep1_East(int door, int dma_ptr);
void GetDoorDrawDataIndex_East(int door, int r4_door);
void DrawDoorToTileMap_East(int door, int r4_door);
uint8 GetDoorGraphicsIndex(int door, int r4_door);
void ClearExplodingWallFromTileMap_ClearOnePair(uint16 *dst, const uint16 *src);
void Dungeon_DrawRoomOverlay_Apply(int p);
// ============================================================================
// Lighting and Color Effects
// ============================================================================

// Apply incremental grayscale filtering (used during room darkening)
void ApplyGrayscaleFixed_Incremental();
// Smoothly transition palette toward a target brightness level 'a'
void Dungeon_ApproachFixedColor_variable(uint8 a);
// ============================================================================
// Module07 — Dungeon Main Loop State Machine
// ============================================================================
// The game's main loop dispatches to "modules" by index. Module07 is the
// dungeon module. It has ~30 submodules (00-1A) that handle player control,
// transitions, stairs, warps, puzzles, and boss sequences. Each submodule
// may itself have sub-states for multi-frame operations.

// Pre-dungeon setup: load room, set palette, prepare VRAM
void Module_PreDungeon();
// Set ambient sound effects based on dungeon type (water, wind, etc.)
void Module_PreDungeon_setAmbientSfx();
// Load overworld music if transitioning from dungeon to overworld
void LoadOWMusicIfNeeded();
// Main dungeon module dispatcher: routes to the active submodule
void Module07_Dungeon();
// Detect if Link has reached the edge of the screen and start a transition
void Dungeon_TryScreenEdgeTransition();
// Move Link during an edge-scrolling transition in the given direction
void Dungeon_HandleEdgeTransitionMovement(int dir);

// Submodule 00: Normal gameplay — player has full control
void Module07_00_PlayerControl();
// Submodule 01: Subtile transition (scrolling within a supertile)
void Module07_01_SubtileTransition();
void DungeonTransition_Subtile_ResetShutters();
void DungeonTransition_Subtile_PrepTransition();
void DungeonTransition_Subtile_ApplyFilter();
void DungeonTransition_Subtile_TriggerShutters();
// Submodule 02: Supertile transition (moving to an entirely new room)
void Module07_02_SupertileTransition();
void Module07_02_00_InitializeTransition();
void Module07_02_01_LoadNextRoom();
void Dungeon_InterRoomTrans_State3();
void Dungeon_InterRoomTrans_State10();
void Dungeon_SpiralStaircase11();
void Dungeon_InterRoomTrans_notDarkRoom();
void Dungeon_InterRoomTrans_State9();
void Dungeon_SpiralStaircase12();
void Dungeon_InterRoomTrans_State4();
void Dungeon_InterRoomTrans_State12();
void Dungeon_Staircase14();
void Dungeon_ResetTorchBackgroundAndPlayer();
void Dungeon_ResetTorchBackgroundAndPlayerInner();
void Dungeon_InterRoomTrans_State7();
void DungeonTransition_RunFiltering();
void Module07_02_FadedFilter();
void Dungeon_InterRoomTrans_State15();
void Dungeon_PlayMusicIfDefeated();
// Submodule 03: Overlay change (BG2 modification during gameplay)
void Module07_03_OverlayChange();
// Submodule 04: Key door unlock animation
void Module07_04_UnlockDoor();
// Submodule 05: Shutter door open/close animation
void Module07_05_ControlShutters();
// Submodule 06: Wide staircase inter-room transition
void Module07_06_FatInterRoomStairs();
void Module07_0E_01_HandleMusicAndResetProps();
void ResetTransitionPropsAndAdvance_ResetInterface();
void ResetTransitionPropsAndAdvanceSubmodule();
void Dungeon_InitializeRoomFromSpecial();
void DungeonTransition_LoadSpriteGFX();
void DungeonTransition_AdjustForFatStairScroll();
void ResetThenCacheRoomEntryProperties();
void DungeonTransition_TriggerBGC34UpdateAndAdvance();
void DungeonTransition_TriggerBGC56UpdateAndAdvance();
// Submodule 07: Falling through a pit to the room below
void Module07_07_FallingTransition();
void Module07_07_00_HandleMusicAndResetRoom();
void Module07_07_06_SyncBG1and2();
void Module07_07_0F_FallingFadeIn();
void Dungeon_PlayBlipAndCacheQuadrantVisits();
void Module07_07_10_LandLinkFromFalling();
void Module07_07_11_CacheRoomAndSetMusic();
// Submodule 08: North-facing intra-room staircase (layer change)
void Module07_08_NorthIntraRoomStairs();
void Module07_08_00_InitStairs();
void Module07_08_01_ClimbStairs();
// Submodule 10: South-facing intra-room staircase (layer change)
void Module07_10_SouthIntraRoomStairs();
void Module07_10_00_InitStairs();
void Module07_10_01_ClimbStairs();
// Submodule 09: Cracked door opening animation after bombing
void Module07_09_OpenCrackedDoor();
// Submodule 0A: Room brightness change (entering/leaving dark rooms)
void Module07_0A_ChangeBrightness();
// Submodule 0B: Drain the Swamp Palace pool
void Module07_0B_DrainSwampPool();
// Submodule 0C: Fill Swamp Palace with water
void Module07_0C_FloodSwampWater();
// Submodule 0D: Flood the dam (watergate puzzle completion)
void Module07_0D_FloodDam();
// Submodule 0E: Spiral staircase transition (change floors)
void Module07_0E_SpiralStairs();
void Dungeon_DoubleApplyAndIncrementGrayscale();
void Module07_0E_02_ApplyFilterIf();
void Dungeon_SyncBackgroundsFromSpiralStairs();
void Dungeon_AdvanceThenSetBossMusicUnorthodox();
void Dungeon_SetBossMusicUnorthodox();
void Dungeon_SpiralStaircase17();
void Dungeon_SpiralStaircase18();
void Module07_0E_00_InitPriorityAndScreens();
void Module07_0E_13_SetRoomAndLayerAndCache();
void RepositionLinkAfterSpiralStairs();
void SpiralStairs_MakeNearbyWallsHighPriority_Exiting();
// Submodule 0F: Spotlight/iris wipe effect when landing in a new room
void Module07_0F_LandingWipe();
void Module07_0F_00_InitSpotlight();
void Module07_0F_01_OperateSpotlight();
// Submodule 11: Straight staircase inter-room transition
void Module07_11_StraightInterroomStairs();
void Module07_11_00_PrepAndReset();
void Module07_11_01_FadeOut();
void Module07_11_02_LoadAndPrepRoom();
void Module07_11_03_FilterAndLoadBGChars();
void Module07_11_04_FilterDoBGAndResetSprites();
void Module07_11_0B_PrepDestination();
void Module07_11_09_LoadSpriteGraphics();
void Module07_11_19_SetSongAndFilter();
void Module07_11_11_KeepSliding();
// Submodule 14: Recover after falling through a pit (take damage, reposition)
void Module07_14_RecoverFromFall();
void Module07_14_00_ScrollCamera();
// Submodule 15: Warp pad teleportation (mosaic effect + room change)
void Module07_15_WarpPad();
void Module07_15_01_ApplyMosaicAndFilter();
void Module07_15_04_SyncRoomPropsAndBuildOverlay();
void Module07_15_0E_FadeInFromWarp();
void Module07_15_0F_FinalizeAndCacheEntry();
// Submodule 16: Hammer peg update (toggle raised/lowered state)
void Module07_16_UpdatePegs();
// Submodule 17: Pressure plate activation (floor switch depressed)
void Module07_17_PressurePlate();
// Submodule 18: Maiden rescue cutscene after defeating a crystal boss
void Module07_18_RescuedMaiden();
// Submodule 19: Mirror warp fade effect in dungeons
void Module07_19_MirrorFade();
// Submodule 1A: Triforce door opening animation (Ganon's Tower)
void Module07_1A_RoomDraw_OpenTriforceDoor_bounce();
// ============================================================================
// Module11 — Dungeon Falling Entrance
// ============================================================================
// Handles the special entrance where Link falls into a dungeon from above
// (e.g., Hyrule Castle escape, Skull Woods holes).

void Module11_DungeonFallingEntrance();
void Module11_02_LoadEntrance();
void Dungeon_LoadSongBankIfNeeded();
// ============================================================================
// Room State Persistence and Transitions
// ============================================================================

// Save current room data to SRAM before using the Magic Mirror
void Mirror_SaveRoomData();
// Persist the current dungeon's key count to SRAM
void SaveDungeonKeys();
// Adjust Link's position and camera after spiral staircase traversal
void Dungeon_AdjustAfterSpiralStairs();
// Handle teleport door adjustments (warp-within-dungeon rooms)
void Dungeon_AdjustForTeleportDoors(uint8 room, uint8 flag);
// Apply room layout constraints (quadrant size, layer configuration)
void Dungeon_AdjustForRoomLayout();
// Move Link 8 pixels right during east edge transition
void HandleEdgeTransitionMovementEast_RightBy8();
void Dungeon_StartInterRoomTrans_Right();
// Move Link 16 pixels down during south edge transition
void HandleEdgeTransitionMovementSouth_DownBy16();
// Handle the transition from dungeon to overworld (exit door/staircase)
void Dung_HandleExitToOverworld();
// ============================================================================
// Camera and Quadrant Management
// ============================================================================
// The dungeon camera system tracks which of the four quadrants Link is in
// and constrains scrolling to the current quadrant's boundaries. Quadrant
// flags are saved to SRAM to track which parts of each room were visited.

void AdjustQuadrantAndCamera_right();
// Mark the current quadrant as visited and save to SRAM
void SetAndSaveVisitedQuadrantFlags();
void SaveQuadrantsToSram();
void AdjustQuadrantAndCamera_left();
void AdjustQuadrantAndCamera_down();
void AdjustQuadrantAndCamera_up();
// Set quadrant visit flags in the room data SRAM block
void Dungeon_FlagRoomData_Quadrants();
// Save all current room state (doors, chests, switches) to SRAM
void Dung_SaveDataForCurrentRoom();
// Update camera scroll boundaries during an edge transition
void HandleEdgeTransition_AdjustCameraBoundaries(uint8 arg);
// Determine which quadrant Link is in and update the active quadrant
void Dungeon_AdjustQuadrant();
// Main camera update: follow Link within quadrant scroll boundaries
void Dungeon_HandleCamera();
// Synchronize BG1 and BG2 scroll offsets (for parallax/layer alignment)
void MirrorBg1Bg2Offs();
// ============================================================================
// Transition Camera and Landing Calculations
// ============================================================================

// Adjust camera X position during a scrolling room transition
void DungeonTransition_AdjustCamera_X(uint8 arg);
// Adjust camera Y position during a scrolling room transition
void DungeonTransition_AdjustCamera_Y(uint8 arg);
// Scroll the room view during an inter-room transition
void DungeonTransition_ScrollRoom();
void Module07_11_0A_ScrollCamera();
// Calculate where Link should land after a subtile transition
void DungeonTransition_FindSubtileLanding();
void SubtileTransitionCalculateLanding();
void Dungeon_InterRoomTrans_State13();
void Dungeon_IntraRoomTrans_State5();
// Move Link through the door opening during a transition; returns true when done
bool DungeonTransition_MoveLinkOutDoor();
// Calculate Link's landing tile position after a transition
uint8 CalculateTransitionLanding();
// Load a new room's tile data and draw it into the BG tile map buffers
void Dungeon_LoadAndDrawRoom();
// Load a dungeon entrance definition (starting room, position, camera)
void Dungeon_LoadEntrance();
// ============================================================================
// Push Block Physics
// ============================================================================

// Animate push block sliding to its target position
void PushBlock_Slide(uint8 j);
// Handle a push block falling into a pit
void PushBlock_HandleFalling(uint8 y);
// Apply velocity to move push block i along its slide axis
void PushBlock_ApplyVelocity(uint8 i);
// Check if push block i collides with walls or other blocks at (x,y)
void PushBlock_HandleCollision(uint8 i, uint16 x, uint16 y);
// Draw all active push blocks as OAM sprites
void Sprite_Dungeon_DrawAllPushBlocks();
// ============================================================================
// Staircase Handling
// ============================================================================

// Process Link's movement on a straight inter-room staircase
void UsedForStraightInterRoomStaircase();
// Handle Link's vertical movement and animation on spiral stairs
void HandleLinkOnSpiralStairs();
// Calculate Link's destination position at the bottom/top of spiral stairs
void SpiralStairs_FindLandingSpot();
// ============================================================================
// Layer Effects (BG2 Special Behaviors)
// ============================================================================
// Some rooms use BG2 for special visual effects beyond static decoration.
// Each layer effect type updates BG2 scroll offsets or tile data per frame.

// Dispatch to the active layer effect handler
void Dungeon_HandleLayerEffect();
void LayerEffect_Nothing();            // No special BG2 behavior
void LayerEffect_Scroll();             // Parallax scrolling floor (Tower of Hera)
void LayerEffect_Trinexx();            // Trinexx boss arena floor movement
void LayerEffect_Agahnim2();           // Agahnim 2 arena curtain sway
void LayerEffect_InvisibleFloor();     // Invisible floor (Ice Palace)
void LayerEffect_Ganon();              // Ganon fight: destructible floor
void LayerEffect_WaterRapids();        // Swamp Palace water current scrolling

// Load custom tile collision attributes (overrides for special rooms)
void Dungeon_LoadCustomTileAttr();
// Check if Link should be in bunny form (Dark World without Moon Pearl)
void Link_CheckBunnyStatus();

// ============================================================================
// Crystal Maiden Cutscene
// ============================================================================

// Initialize the crystal maiden rescue cutscene after boss defeat
void CrystalCutscene_Initialize();
// Spawn the maiden sprite inside the descending crystal
void CrystalCutscene_SpawnMaiden();
