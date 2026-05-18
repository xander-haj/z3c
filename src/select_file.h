/*
 * select_file.h — File Select Screen, SRAM Validation, and Name Entry
 *
 * Implements the game's save file management UI, which is organized into
 * four sub-modules accessed from the title screen:
 *
 *   Module 01 (File Select): The main screen showing three save slots with
 *     player name, heart count, and death counter. The fairy cursor moves
 *     between slots. This is where players choose which file to load.
 *
 *   Module 02 (Copy File): Allows the player to duplicate one save file
 *     into another slot. Uses a two-step selection (source, then target)
 *     with a confirmation prompt.
 *
 *   Module 03 (Kill/Erase File): Permanently deletes a save file after
 *     confirmation. The SRAM region for the selected slot is zeroed.
 *
 *   Module 04 (Name File): Character-by-character name entry screen for
 *     new save files, using a grid of selectable characters and a cursor.
 *
 * SRAM integrity is maintained through a 16-bit checksum stored alongside
 * each save file. On boot, Intro_ValidateSram verifies all three slots and
 * marks corrupted files as empty.
 *
 * Related files:
 *   select_file.c  — Implementation of all functions declared here
 *   ending.h       — Module00_Intro (precedes file select in boot sequence)
 *   attract.h      — Module14_Attract (idle timeout from file select)
 *   variables.h    — SRAM mirror variables and save slot data structures
 */
#pragma once


// --- SRAM Checksum Validation ------------------------------------------------
// The SNES has no filesystem; save data lives in battery-backed SRAM.
// Each save slot stores a 16-bit checksum to detect corruption from
// dead batteries or interrupted writes.

/*
 * Intro_CheckCksum — Verify the integrity of a save file's SRAM block.
 * Computes a checksum over the save data and compares it against the
 * stored checksum value.
 *
 * @param s  Pointer to the start of the save slot's SRAM block
 * @return   true if the checksum matches (data is valid), false if corrupted
 */
bool Intro_CheckCksum(const uint8 *s);

/*
 * Intro_FixCksum — Recompute and write the correct checksum into a save
 * file's SRAM block. Called after any modification to save data (copy,
 * erase, name entry) to keep the integrity check consistent.
 *
 * @param s  Pointer to the start of the save slot's SRAM block
 */
void Intro_FixCksum(uint8 *s);

/*
 * Intro_ValidateSram — Check all three save slots on boot. Verifies each
 * slot's checksum and marks corrupted slots as empty. Also handles the
 * inverse checksum validation (the SNES stores both a checksum and its
 * complement for double verification).
 */
void Intro_ValidateSram();

// --- File Select Screen Rendering --------------------------------------------

/*
 * SelectFile_Func1 — Compute the VRAM tilemap pointer for rendering file
 * slot information (name, hearts, death count) in the correct screen position.
 *
 * @return  Pointer to the tilemap buffer entry for the current slot
 */
uint16 *SelectFile_Func1();

/*
 * SelectFile_Func5_DrawOams — Render the OAM sprites for a file select slot
 * (heart containers, equipment icons, quest progress indicators).
 * @param k  File slot index (0-2)
 */
void SelectFile_Func5_DrawOams(int k);

/*
 * SelectFile_Func6_DrawOams2 — Render additional OAM sprites for a file slot
 * (pendant/crystal indicators, Master Sword icon if obtained).
 * @param k  File slot index (0-2)
 */
void SelectFile_Func6_DrawOams2(int k);

/*
 * SelectFile_Func17 — Draw the Triforce icon next to a completed save file
 * to indicate that the player has finished the game on that slot.
 * @param k  File slot index (0-2)
 */
void SelectFile_Func17(int k);

/*
 * SelectFile_Func16 — Update the blinking cursor animation and handle
 * the visual feedback when a file slot is highlighted.
 */
void SelectFile_Func16();

/*
 * LoadFileSelectGraphics — Load all graphics assets needed by the file
 * select screen: font tiles, UI frame tiles, fairy cursor sprite,
 * heart/equipment icons, and the background tilemap.
 */
void LoadFileSelectGraphics();

/*
 * FileSelect_DrawFairy — Render the fairy cursor sprite at the specified
 * screen coordinates. The fairy bobs up and down and its wings animate.
 *
 * @param x  Screen X coordinate for the fairy
 * @param y  Screen Y coordinate for the fairy
 */
void FileSelect_DrawFairy(uint8 x, uint8 y);

// --- Module 01: File Select Main Flow ----------------------------------------

/*
 * Module01_FileSelect — Top-level state machine for the file select module.
 * Dispatches to initialization, main selection loop, or transition states
 * based on the current sub-module counter.
 */
void Module01_FileSelect();

/*
 * Module_SelectFile_0 — Initialization phase for the file select screen.
 * Loads graphics, validates SRAM, populates the three slot displays with
 * save file data, and begins the fade-in transition.
 */
void Module_SelectFile_0();

void FileSelect_ReInitSaveFlagsAndEraseTriforce(); // Reset UI flags between sub-menus
void FileSelect_EraseTriforce();                    // Clear Triforce icon from display

/*
 * Module_EraseFile_1 — Intermediate state that handles the visual transition
 * when entering the erase file sub-menu from the main file select.
 */
void Module_EraseFile_1();

// --- Screen Transition Effects -----------------------------------------------
// The file select uses horizontal stripe wipe effects when transitioning
// between sub-menus (select, copy, erase, name entry).

void FileSelect_TriggerStripesAndAdvance();     // Start stripe wipe into sub-menu
void FileSelect_TriggerNameStripesAndAdvance();  // Start stripe wipe into name entry

/*
 * FileSelect_Main — Core selection loop. Reads controller input, moves the
 * fairy cursor between slots, and dispatches to the chosen action (load game,
 * copy, erase, or register new name) when the player confirms.
 */
void FileSelect_Main();

// --- Module 02: Copy File ----------------------------------------------------

/*
 * Module02_CopyFile — Top-level state machine for the copy file sub-menu.
 * Guides the player through selecting a source slot, selecting a destination
 * slot, and confirming the copy operation.
 */
void Module02_CopyFile();

void Module_CopyFile_2();          // Copy file initialization phase
void CopyFile_ChooseSelection();   // Source slot selection input handling
void CopyFile_ChooseTarget();      // Destination slot selection input handling
void CopyFile_ConfirmSelection();  // "Are you sure?" confirmation handling
void FilePicker_DeleteHeaderStripe();    // Clear the header text for sub-menu switch
void CopyFile_SelectionAndBlinker();     // Animate source slot cursor blink
void ReturnToFileSelect();               // Transition back to main file select
void CopyFile_TargetSelectionAndBlink(); // Animate destination slot cursor blink
void CopyFile_HandleConfirmation();      // Execute the SRAM copy and update display

// --- Module 03: Kill (Erase) File --------------------------------------------

/*
 * Module03_KILLFile — Top-level state machine for file deletion. The SNES
 * original uses the dramatic label "KILL" internally for erasing save files.
 */
void Module03_KILLFile();

void KILLFile_SetUp();              // Initialize the erase file sub-menu display
void KILLFile_HandleSelection();    // Process cursor movement and slot selection
void KILLFile_HandleConfirmation(); // Execute SRAM erase after player confirms
void KILLFile_ChooseTarget();       // Target slot selection for deletion

// --- Module 04: Name Entry ---------------------------------------------------

/*
 * Module04_NameFile — Top-level state machine for the name entry screen.
 * Presents a character grid and allows the player to spell out a name
 * for a new save file, character by character.
 */
void Module04_NameFile();

/*
 * NameFile_EraseSave — Clear the SRAM block for the selected slot before
 * creating a new save file with the entered name.
 */
void NameFile_EraseSave();

/*
 * NameFile_DoTheNaming — Main loop for the name entry screen. Handles
 * cursor movement over the character grid, character selection on button
 * press, and transitions to gameplay when the name is confirmed.
 */
void NameFile_DoTheNaming();

void NameFile_CheckForScrollInputX(); // Process horizontal D-pad for character grid
void NameFile_CheckForScrollInputY(); // Process vertical D-pad for character grid

/*
 * NameFile_DrawSelectedCharacter — Render a character tile at the current
 * name position in the name entry display.
 *
 * @param k    Character position index within the name (0-5)
 * @param chr  Tile index for the selected character glyph
 */
void NameFile_DrawSelectedCharacter(int k, uint16 chr);

// --- Name Player Sub-states --------------------------------------------------

void Module_NamePlayer_1(); // Name entry intermediate state (cursor blink update)
void Module_NamePlayer_2(); // Name entry finalization (write name to SRAM)
