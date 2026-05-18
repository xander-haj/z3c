/*
 * hud.h - Heads-up display, inventory menu, and item management
 *
 * Manages Link's on-screen HUD (hearts, magic meter, rupee counter, item
 * icon) and the pause-screen inventory menu where the player selects which
 * item is assigned to the Y button. The HUD is rendered on BG3, the
 * highest-priority background layer, so it always appears above gameplay.
 *
 * The inventory menu has two sub-menus:
 *   - The main item grid (Y-button items: boomerang, hookshot, bombs, etc.)
 *   - The bottle sub-menu (4 bottle slots with context-specific contents)
 *
 * The HUD also displays dungeon-specific elements: the floor indicator
 * (current floor in multi-level dungeons), the boss key icon, the compass
 * dot, and the Super Bomb countdown when it is active.
 *
 * This module additionally handles heart/magic refill animations that play
 * when Link picks up recovery items -- the hearts fill one at a time with
 * a sound effect, matching the original SNES presentation.
 */
#pragma once
#include "types.h"

/*
 * kHudItems - Inventory grid slot indices for Y-button items
 *
 * Maps each item to its position in the HUD inventory grid. These indices
 * are used to look up item tile graphics, track which grid cell the cursor
 * is on, and determine the currently equipped Y-button item.
 *
 * Note: kHudItem_BottleOld and kHudItem_Shovel share index 16 because the
 * Shovel and Flute occupy the same inventory slot (the Shovel is replaced
 * by the Flute after finding the buried Flute item).
 */
enum kHudItems {

  kHudItem_Bombs = 4,       // Bomb bag -- grid position in the second row
  kHudItem_Mushroom = 5,    // Magic Mushroom (trade sequence item)
  kHudItem_Hammer = 12,     // Magic Hammer -- drives pegs and breaks barriers
  kHudItem_Flute = 13,      // Flute (Ocarina) -- fast travel to weather vane locations
  kHudItem_BookMudora = 15, // Book of Mudora -- translates Hylian text on tablets
  kHudItem_BottleOld = 16,  // Legacy bottle index from an earlier ROM revision

  kHudItem_Shovel = 16,     // Shovel -- shares slot with Flute (mutually exclusive)
  kHudItem_Bottle1 = 21,    // Bottle slots 1-4 hold potions, fairies, or bees
  kHudItem_Bottle2 = 22,
  kHudItem_Bottle3 = 23,
  kHudItem_Bottle4 = 24,
};

/* ---------------------------------------------------------------------------
 * HUD display updates
 *
 * These functions update the persistent on-screen HUD elements (hearts,
 * magic, rupees, minimap, equipped item icon) without opening the menu.
 * --------------------------------------------------------------------------- */

// Refreshes the currently equipped item icon shown in the top-left HUD area.
void Hud_RefreshIcon();

// Checks whether Link has collected the dungeon-specific item (map, compass,
// big key) for the current palace. Returns the possession bitmask.
uint8 CheckPalaceItemPosession();

// Draws the floor indicator on the right side of the HUD showing which floor
// of a multi-level dungeon Link is currently on (B1, 1F, 2F, etc.).
void Hud_FloorIndicator();

// Removes the Super Bomb timer display from the HUD after the bomb detonates
// or is dismissed.
void Hud_RemoveSuperBombIndicator();

// Draws and updates the Super Bomb countdown timer on the HUD. The Super Bomb
// has a visible fuse timer before it explodes.
void Hud_SuperBombIndicator();

// Processes heart and magic refill animations each frame. Called continuously
// while a refill is in progress until all recovery is consumed.
void Hud_RefillLogic();

/* ---------------------------------------------------------------------------
 * Menu state machine
 *
 * The inventory menu operates as a state machine: Init -> BringDown ->
 * NormalMenu (with optional BottleMenu sub-state) -> CloseMenu. Each state
 * handles its own input processing and rendering.
 * --------------------------------------------------------------------------- */

// Top-level menu module dispatcher. Called each frame while the menu is open;
// routes to the current menu state handler.
void Hud_Module_Run();

// Clears the BG3 tilemap region used by the menu, preparing a blank canvas
// before drawing menu contents.
void Hud_ClearTileMap();

// Initializes the menu state when the player first presses Start. Sets up
// cursor position, selected item, and animation counters.
void Hud_Init();

// Animates the menu panel sliding down from the top of the screen. The menu
// descends over several frames to match the original SNES animation timing.
void Hud_BringMenuDown();

// Determines the next menu mode after an item is selected or the player
// navigates between the main grid and sub-menus.
void Hud_ChooseNextMode();

// Handles input and rendering for the main item selection grid. Processes
// D-pad movement to navigate the cursor and A/Y button to equip items.
void Hud_NormalMenu();

// Redraws all dynamic HUD elements (hearts, magic, rupee count) to reflect
// current game state. Called both during gameplay and inside the menu.
void Hud_UpdateHud();

// Looks up the internal item ID for the given HUD grid |item| index.
// Returns the item type byte used by the game's item possession system.
uint8 Hud_LookupInventoryItem(uint8 item);

// Updates the Y-button equipped item to match the current cursor position
// in the inventory grid, writing the selection to the save data.
void Hud_UpdateEquippedItem();

// Animates the menu sliding back up and returns control to gameplay.
void Hud_CloseMenu();

/* ---------------------------------------------------------------------------
 * Bottle sub-menu
 *
 * When the cursor is on a bottle slot and the player presses A, the bottle
 * sub-menu expands to show the contents of all four bottles, allowing the
 * player to select which bottle to assign to Y.
 * --------------------------------------------------------------------------- */

// Transitions from the main inventory grid to the bottle sub-menu.
void Hud_GotoBottleMenu();

// Sets up the bottle sub-menu layout and cursor position.
void Hud_InitBottleMenu();

// Animates the bottle sub-menu expanding open.
void Hud_ExpandBottleMenu();

// Handles input for navigating between the four bottle slots.
void Hud_BottleMenu();

// Redraws the bottle sub-menu contents after a selection change.
void Hud_DrawBottleMenu_Update();

// Animates the bottle sub-menu collapsing shut.
void Hud_EraseBottleMenu();

// Restores the main inventory grid display after closing the bottle sub-menu.
void Hud_RestoreNormalMenu();

/* ---------------------------------------------------------------------------
 * Menu rendering helpers
 * --------------------------------------------------------------------------- */

// Scans the inventory grid to find and highlight the currently equipped item,
// placing the cursor on the matching grid cell when the menu opens.
void Hud_SearchForEquippedItem();

// Draws all Y-button assignable item icons into the inventory grid tiles.
void Hud_DrawYButtonItems();

// Draws the ability box showing Link's current special abilities (gloves,
// flippers, Moon Pearl) in the equipment section of the menu.
void Hud_DrawAbilityBox();

// Draws the progress tracking icons section of the menu.
void Hud_DrawProgressIcons();

// Draws the three pendant icons (green, blue, red) with collected pendants
// shown in color and uncollected ones shown as outlines.
void Hud_DrawProgressIcons_Pendants();

// Draws the seven crystal icons, highlighting the ones Link has collected
// from defeated Dark World dungeon bosses.
void Hud_DrawProgressIcons_Crystals();

// Draws the highlighted icon for whichever item the cursor is currently on,
// showing its name and a larger sprite preview.
void Hud_DrawSelectedYButtonItem();

// Draws the equipment box section (sword, shield, armor, gloves, boots).
void Hud_DrawEquipmentBox();

// Draws the bottle sub-menu tile layout showing all four bottle contents.
void Hud_DrawBottleMenu();

/* ---------------------------------------------------------------------------
 * Refill animations
 * --------------------------------------------------------------------------- */

// Processes one step of the heart refill animation. Each call fills one
// quarter-heart and plays the recovery sound effect. Returns true when
// the refill is complete.
bool Hud_RefillHealth();

// Advances the heart container fill animation by one frame, updating the
// heart tile graphics from empty to quarter to half to full.
void Hud_AnimateHeartRefill();

// Processes one step of the magic meter refill animation. Returns true
// when the magic meter is fully restored.
bool Hud_RefillMagicPower();

/* ---------------------------------------------------------------------------
 * HUD rebuild and restoration
 * --------------------------------------------------------------------------- */

// Restores the BG3 tilemap behind extinguished torches in dark rooms.
// When a torch burns out, its light radius tiles must be replaced with
// the original dark room background.
void Hud_RestoreTorchBackground();

// Rebuilds the entire HUD tilemap for indoor (dungeon) areas, which includes
// the minimap, floor indicator, key count, and dungeon-specific elements.
void Hud_RebuildIndoor();

// Rebuilds the full HUD tilemap from scratch. Called after screen transitions
// or when the HUD state has been invalidated by a mode change.
void Hud_Rebuild();

// Returns a pointer to the tilemap data for the item box graphic of |item|.
// Used when drawing individual item icons in the inventory grid.
const uint16 *Hud_GetItemBoxPtr(int item);

// Returns the inventory grid index of the currently selected Y-button item.
int GetCurrentItemButtonIndex();

// Returns a pointer to the item possession byte for inventory slot |i|,
// allowing the menu code to check whether Link owns the item in that slot.
uint8 *GetCurrentItemButtonPtr(int i);

// Processes D-pad and button input for switching the selected item in the
// inventory grid. Handles cursor wrapping at grid boundaries.
void Hud_HandleItemSwitchInputs();
