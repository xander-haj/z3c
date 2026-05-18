/*
 * config.c — INI Configuration File Parser and Input Binding System
 *
 * Implements the zelda3 configuration pipeline:
 * 1. Reads an INI-format config file (zelda3.ini or zelda3.user.ini)
 * 2. Dispatches key=value pairs to section-specific handlers
 * 3. Builds hash table mappings from SDL keycodes to game commands
 * 4. Builds linked-list mappings from gamepad buttons to game commands
 * 5. Fills any unbound actions with default key/gamepad bindings
 *
 * The INI parser supports six sections: [KeyMap], [Graphics], [Sound],
 * [General], [Features], and [GamepadMap]. It also supports an
 * "!include <file>" directive for config file chaining.
 *
 * Key bindings use a compact internal representation that packs an SDL
 * keycode (9-bit scancode or character code) with modifier flags (Alt,
 * Shift, Ctrl) into a single uint16, enabling efficient hash table lookup
 * during gameplay input processing.
 */

// Core project headers
#include "config.h"
#include "types.h"
// Standard library — file I/O and string operations for INI parsing
#include <stdio.h>
#include <string.h>
// SDL — keyboard/gamepad input constants
#include <SDL.h>
// Project-specific — feature flag bitmask constants and string utilities
#include "features.h"
#include "util.h"

/*
 * Internal key modifier flags — packed into the upper bits of a uint16
 * alongside the 9-bit keycode. This allows a single 16-bit value to
 * uniquely represent any key + modifier combination for hash table storage.
 *
 * kKeyMod_ScanCode (bit 9) distinguishes SDL scancodes (physical key
 * positions) from character-based keycodes, since SDL uses bit 30
 * (SDLK_SCANCODE_MASK) which must be compressed into our 16-bit space.
 */
enum {
  kKeyMod_ScanCode = 0x200,
  kKeyMod_Alt = 0x400,
  kKeyMod_Shift = 0x800,
  kKeyMod_Ctrl = 0x1000,
};

/* Global config instance — all fields start at zero/false/NULL */
Config g_config;

/*
 * REMAP_SDL_KEYCODE — Compresses a 32-bit SDL keycode into a 10-bit
 * internal representation. SDL uses bit 30 (SDLK_SCANCODE_MASK) to flag
 * physical scancodes vs. character codes; we remap that to bit 9
 * (kKeyMod_ScanCode) and keep only the lower 9 bits of the actual code.
 *
 * The shorthand macros below build default key binding entries:
 *   _(x)  = bare key, no modifiers
 *   S(x)  = Shift + key
 *   A(x)  = Alt + key
 *   C(x)  = Ctrl + key
 *   N     = unbound (0)
 */
#define REMAP_SDL_KEYCODE(key) ((key) & SDLK_SCANCODE_MASK ? kKeyMod_ScanCode : 0) | (key) & (kKeyMod_ScanCode - 1)
#define _(x) REMAP_SDL_KEYCODE(x)
#define S(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Shift
#define A(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Alt
#define C(x) REMAP_SDL_KEYCODE(x) | kKeyMod_Ctrl
#define N 0
/*
 * Default keyboard bindings — indexed by kKeys_* command ID.
 * Each entry is a packed uint16 (keycode | modifier flags).
 * These are used as fallbacks for any command not explicitly
 * bound in the [KeyMap] section of the INI file.
 */
static const uint16 kDefaultKbdControls[kKeys_Total] = {
  0,
  // Controls
  _(SDLK_UP), _(SDLK_DOWN), _(SDLK_LEFT), _(SDLK_RIGHT), _(SDLK_RSHIFT), _(SDLK_RETURN), _(SDLK_x), _(SDLK_z), _(SDLK_s), _(SDLK_a), _(SDLK_c), _(SDLK_v),
  // LoadState
  _(SDLK_F1), _(SDLK_F2), _(SDLK_F3), _(SDLK_F4), _(SDLK_F5), _(SDLK_F6), _(SDLK_F7), _(SDLK_F8), _(SDLK_F9), _(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // SaveState
  S(SDLK_F1), S(SDLK_F2), S(SDLK_F3), S(SDLK_F4), S(SDLK_F5), S(SDLK_F6), S(SDLK_F7), S(SDLK_F8), S(SDLK_F9), S(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // Replay State
  C(SDLK_F1), C(SDLK_F2), C(SDLK_F3), C(SDLK_F4), C(SDLK_F5), C(SDLK_F6), C(SDLK_F7), C(SDLK_F8), C(SDLK_F9), C(SDLK_F10), N, N, N, N, N, N, N, N, N, N,
  // Load Ref State
  N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N,
  // Replay Ref State
  N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N,
  // CheatLife, CheatKeys, CheatEquipment, CheatWalkThroughWalls
  _(SDLK_w), _(SDLK_o), S(SDLK_w), C(SDLK_e),
  // ClearKeyLog, StopReplay, Fullscreen, Reset, Pause, PauseDimmed, Turbo, ReplayTurbo, WindowBigger, WindowSmaller, DisplayPerf, ToggleRenderer
  _(SDLK_k), _(SDLK_l), A(SDLK_RETURN), C(SDLK_r), S(SDLK_p), _(SDLK_p), _(SDLK_TAB), _(SDLK_t), N, N, _(SDLK_f), _(SDLK_r),
};
#undef _
#undef A
#undef C
#undef S
#undef N

/*
 * KeyNameId — Maps an INI key name string (e.g., "Controls", "Load") to
 * its base command ID and the number of slots it spans. Multi-slot
 * commands like "Controls" accept comma-separated lists of key bindings
 * in the INI file, one per slot.
 */
typedef struct KeyNameId {
  const char *name;   // INI key name used in [KeyMap] or [GamepadMap]
  uint16 id;          // Base kKeys_* command ID
  uint16 size;        // Number of consecutive command slots (1 for singles)
} KeyNameId;

// M(n): multi-slot command — size computed from range between base and _Last
// S(n): single-slot command — always size 1
#define M(n) {#n, kKeys_##n, kKeys_##n##_Last - kKeys_##n + 1}
#define S(n) {#n, kKeys_##n, 1}
static const KeyNameId kKeyNameId[] = {
  {"Null", kKeys_Null, 65535},
  M(Controls), M(Load), M(Save), M(Replay), M(LoadRef), M(ReplayRef),
  S(CheatLife), S(CheatKeys), S(CheatEquipment), S(CheatWalkThroughWalls),
  S(ClearKeyLog), S(StopReplay), S(Fullscreen), S(Reset),
  S(Pause), S(PauseDimmed), S(Turbo), S(ReplayTurbo), S(WindowBigger), S(WindowSmaller), S(VolumeUp), S(VolumeDown), S(DisplayPerf), S(ToggleRenderer),
};
#undef S
#undef M

/*
 * KeyMapHashEnt — Entry in the keyboard binding hash table.
 * Uses separate chaining via a 'next' index (1-based, 0 = end of chain).
 * The hash table maps packed key+modifier values to command IDs.
 */
typedef struct KeyMapHashEnt {
  uint16 key;    // Packed keycode | modifier flags
  uint16 cmd;    // kKeys_* command this key triggers
  uint16 next;   // 1-based index of next entry in chain (0 = none)
} KeyMapHashEnt;

// Hash table for keyboard bindings: 255-bucket separate-chaining table
static uint16 keymap_hash_first[255];       // Head index per bucket (1-based)
static KeyMapHashEnt *keymap_hash;          // Dynamically grown entry array
static int keymap_hash_size;                // Number of entries allocated
// Tracks which key groups were explicitly configured in the INI file,
// so RegisterDefaultKeys() knows which groups still need defaults
static bool has_keynameid[countof(kKeyNameId)];

/*
 * KeyMapHash_Add — Inserts a key binding into the hash table.
 * Returns false if the key is already bound (duplicate detection),
 * true on successful insertion.
 *
 * The entry array grows in 256-entry chunks to amortize realloc cost.
 * Chain links use 1-based indices so that 0 serves as the null terminator.
 */
static bool KeyMapHash_Add(uint16 key, uint16 cmd) {
  // Grow the entry array in 256-entry blocks when capacity is exhausted
  if ((keymap_hash_size & 0xff) == 0) {
    if (keymap_hash_size > 10000)
      Die("Too many keys");
    keymap_hash = realloc(keymap_hash, sizeof(KeyMapHashEnt) * (keymap_hash_size + 256));
  }
  int i = keymap_hash_size++;
  KeyMapHashEnt *ent = &keymap_hash[i];
  ent->key = key;
  ent->cmd = cmd;
  ent->next = 0;
  // Hash into one of 255 buckets (prime-ish count reduces clustering)
  int j = (uint32)key % 255;

  // Walk the chain to check for duplicates and find the tail
  uint16 *cur = &keymap_hash_first[j];
  while (*cur) {
    KeyMapHashEnt *ent = &keymap_hash[*cur - 1];
    if (ent->key == key)
      return false;   // Key already bound — reject duplicate
    cur = &ent->next;
  }
  *cur = i + 1;       // Append to chain (1-based index)
  return true;
}

/*
 * KeyMapHash_Find — Looks up a packed key+modifier value in the hash table.
 * Returns the kKeys_* command ID if found, or 0 (kKeys_Null) if unbound.
 * Called on every keypress during gameplay via FindCmdForSdlKey().
 */
static int KeyMapHash_Find(uint16 key) {
  int i = keymap_hash_first[key % 255];
  while (i) {
    KeyMapHashEnt *ent = &keymap_hash[i - 1];
    if (ent->key == key)
      return ent->cmd;
    i = ent->next;
  }
  return 0;
}

/*
 * FindCmdForSdlKey — Public API called by the input handler on every
 * keyboard event. Converts SDL's 32-bit keycode + modifier state into
 * our compact 16-bit representation, then looks it up in the hash table.
 *
 * Modifier keys themselves (Alt, Ctrl, Shift) are excluded from the
 * modifier bitmask when they are the primary key pressed, so pressing
 * bare Shift doesn't match "Shift+X" bindings.
 */
int FindCmdForSdlKey(SDL_Keycode code, SDL_Keymod mod) {
  // Reject keycodes outside the representable 10-bit range
  if (code & ~(SDLK_SCANCODE_MASK | 0x1ff))
    return 0;
  // Build modifier bits, but skip the modifier if IT is the pressed key
  int key = 0;
  if (code != SDLK_LALT && code != SDLK_RALT)
    key |=  mod & KMOD_ALT ? kKeyMod_Alt : 0;
  if (code != SDLK_LCTRL && code != SDLK_RCTRL)
    key |= mod & KMOD_CTRL ? kKeyMod_Ctrl : 0;
  if (code != SDLK_LSHIFT && code != SDLK_RSHIFT)
    key |= mod & KMOD_SHIFT ? kKeyMod_Shift : 0;
  key |= REMAP_SDL_KEYCODE(code);
  return KeyMapHash_Find(key);
}

/*
 * ParseKeyArray — Parses a comma-separated list of key binding strings
 * from the INI file and registers each one in the hash table.
 *
 * Each element can have modifier prefixes ("Shift+", "Ctrl+", "Alt+")
 * followed by an SDL key name (e.g., "Shift+F1", "Return", "x").
 * The cmd parameter is the base command ID; it increments per slot
 * (skipping cmd=0 which is kKeys_Null, so unbound slots stay at 0).
 */
static void ParseKeyArray(char *value, int cmd, int size) {
  char *s;
  int i = 0;
  for (; i < size && (s = NextDelim(&value, ',')) != NULL; i++, cmd += (cmd != 0)) {
    if (*s == 0)
      continue;    // Empty slot — leave unbound
    // Accumulate modifier prefixes by stripping recognized prefixes
    int key_with_mod = 0;
    for (;;) {
      if (StringStartsWithNoCase(s, "Shift+")) {
        key_with_mod |= kKeyMod_Shift, s += 6;
      } else if (StringStartsWithNoCase(s, "Ctrl+")) {
        key_with_mod |= kKeyMod_Ctrl, s += 5;
      } else if (StringStartsWithNoCase(s, "Alt+")) {
        key_with_mod |= kKeyMod_Alt, s += 4;
      } else {
        break;
      }
    }
    // Use SDL's built-in key name parser for the base key
    SDL_Keycode key = SDL_GetKeyFromName(s);
    if (key == SDLK_UNKNOWN) {
      fprintf(stderr, "Unknown key: '%s'\n", s);
      continue;
    }
    if (!KeyMapHash_Add(key_with_mod | REMAP_SDL_KEYCODE(key), cmd))
      fprintf(stderr, "Duplicate key: '%s'\n", s);
  }
}

/*
 * GamepadMapEnt — Entry in the per-button gamepad binding linked list.
 * Each button has its own chain of bindings, sorted by descending modifier
 * count so that more-specific combos (e.g., "L1+A") are matched before
 * less-specific ones (e.g., bare "A").
 *
 * The 'modifiers' field is a bitmask where bit N means gamepad button N
 * must be held simultaneously for this binding to activate.
 */
typedef struct GamepadMapEnt {
  uint32 modifiers;   // Bitmask of buttons that must be held as modifiers
  uint16 cmd;         // kKeys_* command this combo triggers
  uint16 next;        // 1-based index of next entry in chain (0 = end)
} GamepadMapEnt;

// Per-button head pointers for gamepad binding chains (1-based indexing)
static uint16 joymap_first[kGamepadBtn_Count];
static GamepadMapEnt *joymap_ents;     // Dynamically grown entry array
static int joymap_size;                // Number of entries allocated
// Set true when [GamepadMap] Controls= appears in the INI file
static bool has_joypad_controls;

/*
 * CountBits32 — Population count (Hamming weight) using Kernighan's method.
 * Used to determine modifier specificity when inserting gamepad bindings,
 * so combos with more modifiers take priority during lookup.
 */
static int CountBits32(uint32 n) {
  int count = 0;
  for (; n != 0; count++)
    n &= (n - 1);    // Clears the lowest set bit each iteration
  return count;
}

/*
 * GamepadMap_Add — Inserts a gamepad binding into the per-button linked list.
 * Entries are sorted by descending modifier count so that the most-specific
 * combo is checked first during lookup. This ensures "L1+A" matches before
 * bare "A" when L1 is held.
 *
 * The entry array grows in 64-entry chunks.
 */
static void GamepadMap_Add(int button, uint32 modifiers, uint16 cmd) {
  if ((joymap_size & 0xff) == 0) {
    if (joymap_size > 1000)
      Die("Too many joypad keys");
    joymap_ents = realloc(joymap_ents, sizeof(GamepadMapEnt) * (joymap_size + 64));
    if (!joymap_ents) Die("realloc failure");
  }
  // Walk the chain to find the correct insertion point (sorted by modifier count)
  uint16 *p = &joymap_first[button];
  // Insert it as early as possible but before after any entry with more modifiers.
  int cb = CountBits32(modifiers);
  while (*p && cb < CountBits32(joymap_ents[*p - 1].modifiers))
    p = &joymap_ents[*p - 1].next;
  int i = joymap_size++;
  GamepadMapEnt *ent = &joymap_ents[i];
  ent->modifiers = modifiers;
  ent->cmd = cmd;
  ent->next = *p;   // Splice into the chain at this position
  *p = i + 1;
}

/*
 * FindCmdForGamepadButton — Public API called on every gamepad button event.
 * Walks the sorted linked list for 'button', returning the first binding
 * whose required modifier buttons are all currently held. Because the list
 * is sorted by descending modifier count, the most-specific match wins.
 */
int FindCmdForGamepadButton(int button, uint32 modifiers) {
  GamepadMapEnt *ent;
  for(int e = joymap_first[button]; e != 0; e = ent->next) {
    ent = &joymap_ents[e - 1];
    // Check if all required modifier buttons are currently held
    if ((modifiers & ent->modifiers) == ent->modifiers)
      return ent->cmd;
  }
  return 0;
}

/*
 * ParseGamepadButtonName — Parses a button name from the start of a string,
 * advancing *value past the matched portion. Returns the kGamepadBtn_* ID,
 * or kGamepadBtn_Invalid if no name matches.
 *
 * Multi-character names are listed before single-character ones to ensure
 * longest-prefix matching (e.g., "Back" matches before "B"). "Lb"/"Rb"
 * are accepted as aliases for L1/R1 (Xbox naming convention).
 */
static int ParseGamepadButtonName(const char **value) {
  const char *s = *value;
  // Longest substring first
  static const char *const kGamepadKeyNames[] = {
    "Back", "Guide", "Start", "L3", "R3",
    "L1", "R1", "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "L2", "R2",
    "Lb", "Rb", "A", "B", "X", "Y"
  };
  // Parallel array mapping name indices to button IDs
  // (Lb→L1 and Rb→R1 are aliases sharing the same ID)
  static const uint8 kGamepadKeyIds[] = {
    kGamepadBtn_Back, kGamepadBtn_Guide, kGamepadBtn_Start, kGamepadBtn_L3, kGamepadBtn_R3,
    kGamepadBtn_L1, kGamepadBtn_R1, kGamepadBtn_DpadUp, kGamepadBtn_DpadDown, kGamepadBtn_DpadLeft, kGamepadBtn_DpadRight, kGamepadBtn_L2, kGamepadBtn_R2,
    kGamepadBtn_L1, kGamepadBtn_R1, kGamepadBtn_A, kGamepadBtn_B, kGamepadBtn_X, kGamepadBtn_Y,
  };
  for (size_t i = 0; i != countof(kGamepadKeyNames); i++) {
    const char *r = StringStartsWithNoCase(s, kGamepadKeyNames[i]);
    if (r) {
      *value = r;
      return kGamepadKeyIds[i];
    }
  }
  return kGamepadBtn_Invalid;
}

/*
 * Default gamepad button-to-SNES-controller mapping.
 * Order matches kKeys_Controls slots: Up, Down, Left, Right, Select,
 * Start, A(SNES), B(SNES), X(SNES), Y(SNES), L, R.
 * Note: SNES B maps to Xbox A, SNES A maps to Xbox B, etc. — this
 * follows the standard cross-platform convention where face button
 * positions (not labels) determine the mapping.
 */
static const uint8 kDefaultGamepadCmds[] = {
  kGamepadBtn_DpadUp, kGamepadBtn_DpadDown, kGamepadBtn_DpadLeft, kGamepadBtn_DpadRight, kGamepadBtn_Back, kGamepadBtn_Start,
  kGamepadBtn_B, kGamepadBtn_A, kGamepadBtn_Y, kGamepadBtn_X, kGamepadBtn_L1, kGamepadBtn_R1,
};

/*
 * ParseGamepadArray — Parses a comma-separated list of gamepad binding
 * strings from the INI file. Supports modifier combos using "+" syntax,
 * e.g., "L1+A" means "press A while holding L1".
 *
 * The parser reads button names left-to-right; all buttons before the
 * final one become modifiers (their bit set in the modifiers bitmask),
 * and the last button is the trigger that fires the command.
 */
static void ParseGamepadArray(char *value, int cmd, int size) {
  char *s;
  int i = 0;
  for (; i < size && (s = NextDelim(&value, ',')) != NULL; i++, cmd += (cmd != 0)) {
    if (*s == 0)
      continue;
    uint32 modifiers = 0;
    const char *ss = s;
    for (;;) {
      int button = ParseGamepadButtonName(&ss);
      if (button == kGamepadBtn_Invalid) BAD: {
        fprintf(stderr, "Unknown gamepad button: '%s'\n", s);
        break;
      }
      while (*ss == ' ' || *ss == '\t') ss++;
      if (*ss == '+') {
        // This button is a modifier — set its bit and continue parsing
        ss++;
        modifiers |= 1 << button;
      } else if (*ss == 0) {
        // End of string — this button is the trigger, register the binding
        GamepadMap_Add(button, modifiers, cmd);
        break;
      } else
        goto BAD;
    }
  }
}

/*
 * RegisterDefaultKeys — Fills in default bindings for any key/gamepad
 * groups not explicitly configured in the INI file. Called after INI
 * parsing is complete, ensuring user customizations take priority over
 * defaults. Skips index 0 (the "Null" sentinel entry).
 */
static void RegisterDefaultKeys() {
  // Register default keyboard bindings for unconfigured command groups
  for (int i = 1; i < countof(kKeyNameId); i++) {
    if (!has_keynameid[i]) {
      int size = kKeyNameId[i].size, k = kKeyNameId[i].id;
      for (int j = 0; j < size; j++, k++)
        KeyMapHash_Add(kDefaultKbdControls[k], k);
    }
  }
  // Register default gamepad bindings only if no [GamepadMap] Controls
  // entry was found in the INI file
  if (!has_joypad_controls) {
    for (int i = 0; i < countof(kDefaultGamepadCmds); i++)
      GamepadMap_Add(kDefaultGamepadCmds[i], 0, kKeys_Controls + i);
  }
}

/*
 * GetIniSection — Maps an INI section header string to an internal
 * section ID used by HandleIniConfig to dispatch key=value pairs.
 * Returns -1 for unrecognized sections, which causes a warning on stderr.
 */
static int GetIniSection(const char *s) {
  if (StringEqualsNoCase(s, "[KeyMap]"))
    return 0;     // Keyboard key bindings
  if (StringEqualsNoCase(s, "[Graphics]"))
    return 1;     // Video/display settings
  if (StringEqualsNoCase(s, "[Sound]"))
    return 2;     // Audio settings
  if (StringEqualsNoCase(s, "[General]"))
    return 3;     // Misc application settings
  if (StringEqualsNoCase(s, "[Features]"))
    return 4;     // Gameplay enhancement toggles
  if (StringEqualsNoCase(s, "[GamepadMap]"))
    return 5;     // Gamepad button bindings
  return -1;
}

/*
 * ParseBool — Parses a boolean value from an INI string.
 * Accepts: "0"/"1", "true"/"false", "yes"/"no", "on"/"off" (case-insensitive).
 *
 * When result is non-NULL, writes the parsed boolean to *result and
 * returns true on success, false on parse failure. When result is NULL,
 * returns the parsed boolean value directly (used by ParseBoolBit).
 */
bool ParseBool(const char *value, bool *result) {
  bool rv = false;
  // Lowercase the first character via |32 for case-insensitive matching
  switch (*value++ | 32) {
  case '0': if (*value == 0) break; return false;
  case 'f': if (StringEqualsNoCase(value, "alse")) break; return false;
  case 'n': if (StringEqualsNoCase(value, "o")) break; return false;
  case 'o':
    // Distinguishes "on" (true) from "off" (false)
    rv = (*value | 32) == 'n';
    if (StringEqualsNoCase(value, rv ? "n" : "ff")) break;
    return false;
  case '1': rv = true; if (*value == 0) break; return false;
  case 'y': rv = true; if (StringEqualsNoCase(value, "es")) break; return false;
  case 't': rv = true; if (StringEqualsNoCase(value, "rue")) break; return false;
  default: return false;
  }
  if (result) {
    *result = rv;
    return true;
  }
  return rv;
}

/*
 * ParseBoolBit — Parses a boolean and sets/clears a specific bit in a
 * bitmask field. Used for the features0 flags in the [Features] section,
 * where each INI key controls a single bit in g_config.features0.
 */
static bool ParseBoolBit(const char *value, uint32 *data, uint32 mask) {
  bool tmp;
  if (!ParseBool(value, &tmp))
    return false;
  *data = *data & ~mask | (tmp ? mask : 0);
  return true;
}

/*
 * HandleIniConfig — Central dispatcher for parsed INI key=value pairs.
 * Routes each pair to the appropriate handler based on the current section.
 *
 * Section IDs: 0=[KeyMap], 1=[Graphics], 2=[Sound], 3=[General],
 *              4=[Features], 5=[GamepadMap].
 *
 * Returns true if the key was recognized and successfully parsed,
 * false otherwise (triggers a warning message in the caller).
 *
 * String values (link_graphics, shader, msu_path, language) point directly
 * into the INI file's memory buffer rather than copying, so the buffer
 * must remain allocated for the lifetime of the program.
 */
static bool HandleIniConfig(int section, const char *key, char *value) {
  // --- Section 0: [KeyMap] — keyboard bindings ---
  if (section == 0) {
    for (int i = 0; i < countof(kKeyNameId); i++) {
      if (StringEqualsNoCase(key, kKeyNameId[i].name)) {
        has_keynameid[i] = true;
        ParseKeyArray(value, kKeyNameId[i].id, kKeyNameId[i].size);
        return true;
      }
    }
  // --- Section 5: [GamepadMap] — gamepad button bindings ---
  } else if (section == 5) {
    for (int i = 0; i < countof(kKeyNameId); i++) {
      if (StringEqualsNoCase(key, kKeyNameId[i].name)) {
        if (i == 1)
          has_joypad_controls = true;
        ParseGamepadArray(value, kKeyNameId[i].id, kKeyNameId[i].size);
        return true;
      }
    }
  // --- Section 1: [Graphics] — video/display settings ---
  } else if (section == 1) {
    if (StringEqualsNoCase(key, "WindowSize")) {
      // Accepts "Auto" or "WIDTHxHEIGHT" (e.g., "1024x768")
      char *s;
      if (StringEqualsNoCase(value, "Auto")){
        g_config.window_width  = 0;
        g_config.window_height = 0;
        return true;
      }
      while ((s = NextDelim(&value, 'x')) != NULL) {
        if(g_config.window_width == 0) {
          g_config.window_width = atoi(s);
        } else {
          g_config.window_height = atoi(s);
          return true;
        }
      }
    } else if (StringEqualsNoCase(key, "EnhancedMode7")) {
      return ParseBool(value, &g_config.enhanced_mode7);
    } else if (StringEqualsNoCase(key, "NewRenderer")) {
      return ParseBool(value, &g_config.new_renderer);
    } else if (StringEqualsNoCase(key, "IgnoreAspectRatio")) {
      return ParseBool(value, &g_config.ignore_aspect_ratio);
    } else if (StringEqualsNoCase(key, "Fullscreen")) {
      g_config.fullscreen = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "WindowScale")) {
      g_config.window_scale = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "OutputMethod")) {
      // Cascade of string comparisons; unrecognized values default to SDL
      g_config.output_method = StringEqualsNoCase(value, "SDL-Software") ? kOutputMethod_SDLSoftware :
                               StringEqualsNoCase(value, "OpenGL") ? kOutputMethod_OpenGL :
                               StringEqualsNoCase(value, "OpenGL ES") ? kOutputMethod_OpenGL_ES :
                                                                        kOutputMethod_SDL;
      return true;
    } else if (StringEqualsNoCase(key, "LinearFiltering")) {
      return ParseBool(value, &g_config.linear_filtering);
    } else if (StringEqualsNoCase(key, "NoSpriteLimits")) {
      return ParseBool(value, &g_config.no_sprite_limits);
    } else if (StringEqualsNoCase(key, "LinkGraphics")) {
      g_config.link_graphics = value;
      return true;
    } else if (StringEqualsNoCase(key, "Shader")) {
      g_config.shader = *value ? value : NULL;
      return true;
    } else if (StringEqualsNoCase(key, "DimFlashes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_DimFlashes);
    }
  // --- Section 2: [Sound] — audio settings and MSU-1 configuration ---
  } else if (section == 2) {
    if (StringEqualsNoCase(key, "EnableAudio")) {
      return ParseBool(value, &g_config.enable_audio);
    } else if (StringEqualsNoCase(key, "AudioFreq")) {
      g_config.audio_freq = (uint16)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "AudioChannels")) {
      g_config.audio_channels = (uint8)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "AudioSamples")) {
      g_config.audio_samples = (uint16)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "EnableMSU")) {
      // MSU supports named modes ("opuz", "deluxe", "deluxe-opuz")
      // or a simple boolean for standard MSU-1
        if (StringEqualsNoCase(value, "opuz"))
        g_config.enable_msu = kMsuEnabled_Opuz;
      else if (StringEqualsNoCase(value, "deluxe"))
        g_config.enable_msu = kMsuEnabled_MsuDeluxe;
      else if (StringEqualsNoCase(value, "deluxe-opuz"))
        g_config.enable_msu = kMsuEnabled_MsuDeluxe | kMsuEnabled_Opuz;
      else 
        return ParseBool(value, (bool*)&g_config.enable_msu);
      return true;
    } else if (StringEqualsNoCase(key, "MSUPath")) {
      g_config.msu_path = value;
      return true;
    } else if (StringEqualsNoCase(key, "MSUVolume")) {
      g_config.msuvolume = atoi(value);
      return true;
    } else if (StringEqualsNoCase(key, "ResumeMSU")) {
      return ParseBool(value, &g_config.resume_msu);
    }
  // --- Section 3: [General] — application-wide settings ---
  } else if (section == 3) {
    if (StringEqualsNoCase(key, "Autosave")) {
      g_config.autosave = (bool)strtol(value, (char**)NULL, 10);
      return true;
    } else if (StringEqualsNoCase(key, "ExtendedAspectRatio")) {
      // Accepts comma-separated tokens: aspect ratio ("16:9", "16:10",
      // "18:9", "4:3"), "extend_y" (224→240 scanlines),
      // "unchanged_sprites" (no 64px sprite extension),
      // "no_visual_fixes" (skip widescreen rendering fixes).
      // Extra pixels per side = (height * ratio - 256) / 2.
      const char* s;
      int h = 224;       // SNES native vertical resolution
      bool nospr = false, novis = false;
      // todo: make it not depend on the order
      while ((s = NextDelim(&value, ',')) != NULL) {
        if (strcmp(s, "extend_y") == 0)
          h = 240, g_config.extend_y = true;
        else if (strcmp(s, "16:9") == 0)
          g_config.extended_aspect_ratio = (h * 16 / 9 - 256) / 2;
        else if (strcmp(s, "16:10") == 0)
          g_config.extended_aspect_ratio = (h * 16 / 10 - 256) / 2;
        else if (strcmp(s, "18:9") == 0)
          g_config.extended_aspect_ratio = (h * 18 / 9 - 256) / 2;
        else if (strcmp(s, "4:3") == 0)
          g_config.extended_aspect_ratio = 0;
        else if (strcmp(s, "unchanged_sprites") == 0)
          nospr = true;
        else if (strcmp(s, "no_visual_fixes") == 0)
          novis = true;
        else
          return false;
      }
      // Auto-enable widescreen sprite/visual features unless suppressed
      if (g_config.extended_aspect_ratio && !nospr)
        g_config.features0 |= kFeatures0_ExtendScreen64;
      if (g_config.extended_aspect_ratio && !novis)
        g_config.features0 |= kFeatures0_WidescreenVisualFixes;
      return true;
    } else if (StringEqualsNoCase(key, "DisplayPerfInTitle")) {
      return ParseBool(value, &g_config.display_perf_title);
    } else if (StringEqualsNoCase(key, "DisableFrameDelay")) {
      return ParseBool(value, &g_config.disable_frame_delay);
    } else if (StringEqualsNoCase(key, "Language")) {
      g_config.language = value;
      return true;
    }
  // --- Section 4: [Features] — gameplay enhancement toggles ---
  // Each key maps to a single bit in g_config.features0 via ParseBoolBit.
  // These are non-vanilla enhancements that modify game behavior.
  } else if (section == 4) {
    if (StringEqualsNoCase(key, "ItemSwitchLR")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SwitchLR);
    } else if (StringEqualsNoCase(key, "ItemSwitchLRLimit")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SwitchLRLimit);
    } else if (StringEqualsNoCase(key, "TurnWhileDashing")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_TurnWhileDashing);
    } else if (StringEqualsNoCase(key, "MirrorToDarkworld")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MirrorToDarkworld);
    } else if (StringEqualsNoCase(key, "CollectItemsWithSword")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_CollectItemsWithSword);
    } else if (StringEqualsNoCase(key, "BreakPotsWithSword")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_BreakPotsWithSword);
    } else if (StringEqualsNoCase(key, "DisableLowHealthBeep")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_DisableLowHealthBeep);
    } else if (StringEqualsNoCase(key, "SkipIntroOnKeypress")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_SkipIntroOnKeypress);
    } else if (StringEqualsNoCase(key, "ShowMaxItemsInYellow")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_ShowMaxItemsInYellow);
    } else if (StringEqualsNoCase(key, "MoreActiveBombs")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MoreActiveBombs);
    } else if (StringEqualsNoCase(key, "CarryMoreRupees")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_CarryMoreRupees);
    } else if (StringEqualsNoCase(key, "MiscBugFixes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_MiscBugFixes);
    } else if (StringEqualsNoCase(key, "GameChangingBugFixes")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_GameChangingBugFixes);
    } else if (StringEqualsNoCase(key, "CancelBirdTravel")) {
      return ParseBoolBit(value, &g_config.features0, kFeatures0_CancelBirdTravel);
    }
  }
  return false;
}

/*
 * ParseOneConfigFile — Reads an entire INI file into memory and parses it
 * line by line. Supports recursive inclusion via "!include <file>" with a
 * depth limit of 10 to prevent infinite include loops.
 *
 * The file's memory buffer is kept alive (stored in g_config.memory_buffer)
 * because string config values point directly into it to avoid copying.
 *
 * Section state starts at -2 (no section seen yet), which produces a
 * diagnostic if key=value pairs appear before any [Section] header.
 * Section -1 means an unrecognized section header was seen — lines in
 * that section are silently skipped.
 */
static bool ParseOneConfigFile(const char *filename, int depth) {
  char *filedata = (char*)ReadWholeFile(filename, NULL), *p;
  if (!filedata)
    return false;

  int section = -2;    // -2 = no section yet, -1 = unknown section
  g_config.memory_buffer = filedata;

  for (int lineno = 1; (p = NextLineStripComments(&filedata)) != NULL; lineno++) {
    if (*p == 0)
      continue; // empty line
    if (*p == '[') {
      // Section header — update current section ID
      section = GetIniSection(p);
      if (section < 0)
        fprintf(stderr, "%s:%d: Invalid .ini section %s\n", filename, lineno, p);
    } else if (*p == '!' && SkipPrefix(p + 1, "include ")) {
      // Include directive — recursively parse another config file
      char *tt = p + 8;
      char *new_filename = ReplaceFilenameWithNewPath(filename, NextPossiblyQuotedString(&tt));
      if (depth > 10 || !ParseOneConfigFile(new_filename, depth + 1))
        fprintf(stderr, "Warning: Unable to read %s\n", new_filename);
      free(new_filename);
    } else if (section == -2) {
      fprintf(stderr, "%s:%d: Expecting [section]\n", filename, lineno);
    } else {
      // Key=value pair — split at '=' and dispatch to handler
      char *v = SplitKeyValue(p);
      if (v == NULL) {
        fprintf(stderr, "%s:%d: Expecting 'key=value'\n", filename, lineno);
        continue;
      }
      if (section >= 0 && !HandleIniConfig(section, p, v))
        fprintf(stderr, "%s:%d: Can't parse '%s'\n", filename, lineno, p);
    }
  }
  return true;
}

/*
 * ParseConfigFile — Top-level entry point for configuration loading.
 *
 * Config file search order (when filename is NULL):
 *   1. Try "zelda3.user.ini" — user-specific overrides
 *   2. Fall back to "zelda3.ini" — shipped defaults
 *
 * If a specific filename is provided, only that file is tried.
 * After parsing, RegisterDefaultKeys() fills in defaults for any
 * bindings not explicitly set in the config file.
 */
void ParseConfigFile(const char *filename) {
  g_config.msuvolume = 100;  // default msu volume, 100%

  // Try user config first; fall back to default config
  if (filename != NULL || !ParseOneConfigFile("zelda3.user.ini", 0)) {
    if (filename == NULL)
      filename = "zelda3.ini";
    if (!ParseOneConfigFile(filename, 0))
      fprintf(stderr, "Warning: Unable to read config file %s\n", filename);
  }
  RegisterDefaultKeys();
}
