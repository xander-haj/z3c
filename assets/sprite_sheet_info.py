# ============================================================================
# sprite_sheet_info.py — SNES Sprite Sheet Layout Definitions
# ============================================================================
#
# Part of the zelda3 asset pipeline (C reimplementation of A Link to the Past).
# This file is consumed by sprite_sheets.py to generate per-sprite PNG
# visualizations and to reconstruct SNES tilesets from edited PNGs.
#
# --- VRAM Layout System ---
#
# The SNES PPU loads sprite graphics into four 4KB VRAM "sheet slots"
# (ss_1 through ss_4), each holding a 16-column x 4-row grid of 8x8
# pixel tiles (64 tiles per slot, 256 tiles total across all four).
#
# Each add() call below defines one sprite's tile footprint across these
# four slots using ASCII art grids:
#
#   '.' = tile position is not used by this sprite
#   'x' or 'X' = tile is used, drawn with the sprite's default palette
#   '0'-'9'    = tile is used, drawn with that specific palette index
#                (allows multi-palette sprites like Trinexx or Ganon)
#   'o' or 'O' = alias for 'x' (treated identically as "tile used")
#
# The first line of each multi-line string is always blank (artifact of
# the triple-quote format) and is stripped during parsing, leaving the
# 4 data rows that map to the 4 tile rows in that VRAM slot.
#
# --- Tileset and Palette Tuples ---
#
# Context parameters (lw, dw, ow, dung, sheet) accept a tuple of
# (tileset_index, palette_index) identifying which SNES graphics sheet
# and which palette bank the sprite uses in that game context:
#   lw   = Light World overworld
#   dw   = Dark World overworld
#   ow   = generic overworld (when LW/DW share the same sheet)
#   dung = dungeon interior
#   sheet = raw sheet index (for special/unknown sheets)
#
# When both lw and dw are provided, two Entry objects are created
# (one per world variant, suffixed " - LW" / " - DW").
#
# --- dungeon_or_ow field ---
#
# Stored on each Entry to indicate the rendering context:
#   0 = overworld (or Light World)
#   1 = Dark World overworld
#   2 = dungeon
#   3 = raw sheet (special/cutscene/map graphics)
# ============================================================================


# Represents a single sprite's tile usage within one VRAM sheet slot,
# for one specific world context and palette combination.
# Fields:
#   name           - display name, e.g. "08: Octorok" (colon-separated after init)
#   dungeon_or_ow  - context code: 0=overworld/LW, 1=DW, 2=dungeon, 3=raw sheet
#   tileset        - SNES graphics tileset index this sprite lives in
#   palset_idx     - palette bank index within that tileset
#   ss_idx         - which of the 4 VRAM sheet slots (0-3) this entry maps to
#   matrix         - 4x16 list-of-lists: 'x' where the sprite occupies a tile, '.' otherwise
#   pal_base       - palette base override (digit 0-9) or the default palette value
class Entry:
  def __init__(self, name, dungeon_or_ow, tileset_and_pal, ss_idx, matrix, pal_base):
    self.name = name.replace(' - ', ': ')
    self.dungeon_or_ow = dungeon_or_ow
    self.tileset, self.palset_idx = tileset_and_pal
    self.ss_idx = ss_idx
    self.matrix = matrix
    self.pal_base = pal_base

# Global registry of all sprite sheet entries, populated by add() calls below.
# sprite_sheets.py iterates this list to generate PNG sprite visualizations.
entries = []

# Registers one sprite's VRAM tile layout into the global entries list.
# Parameters:
#   name    - sprite identifier string, e.g. "08 - Octorok"
#   lw      - (tileset, palette) tuple for Light World; creates a " - LW" suffixed entry
#   dw      - (tileset, palette) tuple for Dark World; creates a " - DW" suffixed entry
#   ow      - (tileset, palette) for overworld sprites that are the same in both worlds
#   dung    - (tileset, palette) for dungeon-only sprites
#   sheet   - (sheet_index, palette) for raw/special sheets (intros, maps, tagalongs)
#   ss_1..4 - 4-line ASCII art strings defining tile usage in VRAM slots 0-3
#   palette - optional palette base override; if omitted, defaults to the 'x' palette
#
# For multi-palette sprites (e.g. Ganon uses palettes 5 and 6), digits in the
# ASCII grid cause separate Entry objects to be created for each distinct palette,
# each with its own filtered matrix showing only that palette's tiles.
def add(name, lw = None, dw = None, sheet = None, ow = None, dung = None, ss_1 = None, ss_2 = None, ss_3 = None, ss_4 = None, palette = None):
  # Inner function that creates Entry objects for one world context.
  # Scans all four ss grids to find every distinct character (palette key),
  # then for each palette key and each VRAM slot, builds a binary matrix
  # ('x' = this palette's tile, '.' = not) and appends an Entry if non-empty.
  def doit(tileset_and_pal, dungeon_or_ow, suffix = ''):
    # Collect all unique characters used across the four slot grids (lowercased).
    # This determines which palette keys are present (e.g. 'x', '5', '6').
    all_chars = sorted(set("".join([ss.lower() for ss in (ss_1, ss_2, ss_3, ss_4) if ss != None])))
    for ch in all_chars:
      # Skip non-tile characters: newlines, empty positions, and 'o' (alias for 'x')
      if ch == '\n' or ch == '.' or ch == 'o': continue
      for ss_idx, ss in enumerate((ss_1, ss_2, ss_3, ss_4)):
        if ss == None: continue
        # Normalize: lowercase everything and treat 'o' as 'x' (both mean "tile used")
        ss = ss.lower().replace('o', 'x')
        # Strip the leading blank line from the triple-quoted string, leaving 4 data rows
        matrix = ss.splitlines()[1:]
        # Validate: exactly 4 rows of 16 columns (matching the 4x16 VRAM slot grid)
        assert len(matrix) == 4 and all(len(m) == 16 for m in matrix), name
        # Build a filtered matrix for this palette key: 'x' where ch matches, '.' elsewhere
        matrix2 = [[('x' if x == ch else '.') for x in m] for m in matrix]
        # Only create an Entry if at least one tile belongs to this palette in this slot
        if any('x' in m for m in matrix2):
          # For 'x' tiles use the default palette; for digit tiles use the digit as palette index
          entries.append(Entry(name + suffix, dungeon_or_ow, tileset_and_pal, ss_idx, matrix2, palette if ch == 'x' else int(ch)))

  # Dispatch to doit() based on which world context was provided.
  # When both lw and dw are given, two variants are registered (Light World and Dark World).
  if lw and dw:
    doit(lw, 0, ' - LW')
    doit(dw, 1, ' - DW')
  elif lw:
    doit(lw, 0)
  elif dw:
    doit(dw, 1)
  elif ow:
    doit(ow, 0)
  elif dung:
    doit(dung, 2)
  elif sheet:
    doit(sheet, 3)


# ============================================================================
# Sprite Definitions
# ============================================================================
# Each add() call below registers one sprite (or one variant of a sprite)
# into the global entries list. Sprites are ordered roughly by their hex ID
# from the original SNES ROM sprite table.
#
# The first two digits of the name (e.g. "00", "08", "D6") are the sprite's
# hex index in the ROM's sprite data table. Some sprites have multiple add()
# calls because they appear in different tilesets (e.g. overworld vs dungeon)
# or require multiple VRAM slots.
# ============================================================================

# --- Overworld Enemies and Creatures (IDs 0x00 - 0x1F) ---

add("00 - Raven", lw=(4, 3), dw=(23, 13), ss_4 = """
....XXXXXX......
....XXXXXX......
................
................""")

add("01 - Vulture", ow=(8, 4), ss_3 = """
XXXXXXXX........
XXXXXXXX........
................
................""")

add("04 - PullSwitch", dung=(6, 29), ss_4 = """
..............XX
..............XX
..............XX
..............XX""")

add("07 - PullSwitch2", dung=(4, 0), ss_3 = """
................
................
..XXXX..........
..XXXX..........""")

add("08 - Octorok", lw=(11, 3), dw=(22, 10), ss_3 = """
XXXXXX..........
XXXXXX..........
XXXX.....XXX....
XXXX....XXXXX...""")

add("09 - Moldorm", dung=(12, 6), ss_3 = """
5555555555666666
5555555555666666
5555555566666666
5555555566666666""")

add("0b - Chicken", lw=(6, 1), dw=(21, 16), ss_4 = """
................
................
..........XXXX..
..........XXXX..""")

add("0d - Buzzblob", ow=(7, 6), ss_4 = """
..........XXXXXX
..........XXXXXX
XXXXXX..........
XXXX............""")

add("0e - SnapDragon", ow=(19, 14), ss_1 = """
................
................
........XXXX....
........XXXXXXXX""", ss_3 = """
............XXXX
............XXXX
..........XXXXXX
..........XXXXXX""")

add("0f - Octoballon", ow=(4, 3), ss_3 = """
......XX....XX..
......XX....XX..
............XX..
.............X..""")

add("11 - Hinox", dw=(19, 14), ss_1 = """
33333333....3333
33333333....3333
333333......3333
333333..........""")

add("12 - Moblin", ow=(19, 14), ss_3 = """
XXXXXXXXXXXX....
XXXXXXXXXXXX....
XXXXXXXXXX......
XXXXXXXXXX......""")

add("13 - MiniHelmasaur", dung=(12, 26), ss_2 = """
..............XX
............X.XX
....XXXXXXXXXXXX
....XXXXXXXXXXXX""")

add("15 - Antifairy", dung=(8, 10), ss_4 = """
................
................
.1.1............
................""")

# Aginah is (16, 7)
add("16 - Elder", dung=(16, 28), ss_3 = """
................
................
XXXXXX..........
XXXXXX..........""")

add("17 - Hoarder", ow=(4, 3), ss_4 = """
................
................
......XXXX......
......XXXX......""")


add("18 - MiniMoldorm", dung=(19, 13), ss_2 = """
.............X..
.............X..
XXXX............
XXXX............""")

add("19 - Poe", lw=(3, 2), dw=(21, 16), ss_4 = """
................
................
......XX........
......XX........""")

add("1a - Smithy", dung=(5, 30), ss_2 = """
XXXXXX..........
XXXXXX..........
XXXXXX..........
XXXXXX..........""")

add("1a - Smithy", ow=(29, 0), ss_4 = """
........XX......
........XX......
................
................""")

add("1c - Statue", dung=(17, 10), ss_4 = """
XXX.............
XX..............
................
................""")

add("1e - CrystalSwitch", dung=(8, 15), ss_4 = """
................
................
....11..........
....11..........""")

add("1f - SickKid", dung=(13, 21), ss_1 = """
..........6666XX
..........6666XX
.......X......XX
.......X......XX""")


# --- Dungeon Enemies and Switches (IDs 0x20 - 0x3F) ---

add("20 - Sluggula", dung=(29, 17), ss_3 = """
XXXXXX..........
XXXXXX..........
xx........xxxxxx
xx........xxxxxx""")


add("21 - PushSwitch", dung=(17, 10), ss_4 = """
..........xxxx..
..........xxxx..
..........xx....
..........xx....""")

add("22 - Ropa", ow=(19, 14), ss_1 = """
........xxxx....
........xxxx....
......xx........
......xx........""")

# also used for blue bari
add("23 - RedBari", dung=(8, 15), ss_1 = """
................
................
xxxx..xx........
xxxx..xx........""")

add("25 - TalkingTree (S27)", dw=(19, 16), ss_4 = """
................
................
........xx......
........xx......""")

add("25 - TalkingTree (S21)", dw=(21, 16), ss_1 = """
................
................
........xx......
........xx......""")

add("26 - HardhatBeetle", dung=(19, 13), ss_2 = """
xxxxxxxxxxxxx...
xxxxxxxxxxxx....
................
................""")
add("27 - Deadrock", ow=(16, 9), ss_4 = """
..xxxxxx........
..xxxxxx........
..xxxx..........
..xxxx..........""")
add("28 - DarkWorldHintNpc", dung=(5, 7), ss_2 = """
..........55....
..........55....
..............55
..............55""")
add("28 - DarkWorldHintNpc", dung=(5, 7), ss_1 = """
................
................
....55..........
....55..........""")
add("28 - DarkWorldHintNpc", dung=(5, 7), ss_1 = """
....44..........
....44..........
................
................""")
add("28 - DarkWorldHintNpc", dung=(7, 7), ss_1 = """
..............55
..............55
..............55
..............55""")
add("28 - DarkWorldHintNpc", dung=(5, 28), ss_2 = """
................
................
..........5555..
..........5555..""")


add("29 - Human/Woman", dung=(15, 35), ss_1 = """
............xxxx
............xxxx
................
................""", ss_4 = """
..........xxxxxx
..........xxxxxx
................
................""")

add("29 - Human/Thief", dung=(40, 7), ss_1 = """
xxxxxxxx..xx..xx
xxxxxxxx..xx..xx
xxxxxx......xx..
xxxxxx......xx..""", palette = 7)


add("29 - Human/Elder", dung=(15, 35), ss_3 = """
....xxxxxx......
....xxxxxx......
..xx............
..xx............""")

add("2a - SweepingLady", ow=(6, 1), ss_3 = """
..........xxxxxx
..........xxxxxx
................
................""")

add("2b - Hobo", ow=(12, 8), ss_3 = """
........xxxx....
........xxxx....
......xxxxxxx...
......xxxxxxx...""")

add("2c - LumberJacks", ow=(26, 6), ss_1 = """
..............xx
..............xx
................
................""", ss_3 = """
................
................
....xxxxxx......
....xxxxxx....44""")

add("2e - FluteKid - LW", lw=(15, 1), ss_3= """
................
................
........xxxx...x
........xxxx..xx""")


add("2e - FluteKid - DW", dw=(17,11), ss_4 = """
........xxxx00..
........xxxx00..
......xxxx..00..
......xxxx..00..""", palette = 3)


add("2f - MazeGameLady", ow=(6, 1), ss_4 = """
xxxx............
xxxx............
xxxxxxxxxx......
xxxxxxxxxx......""")

add("30 - MazeGameGuy", ow=(6, 1), ss_1="""
xxxx............
xxxx............
xx..............
xx..............""")

add("31 - FortuneTeller", dung=(5, 30), ss_1 = """
..........xxxx..
..........xxxx..
..........x.x...
................""", ss_2 = """
................
................
......xxxx......
......xxxx......""")

add("32 - QuarrelBros", dung=(15,2), ss_1 = """
....XX..XXXX....
....XX..XXXX....
................
................""")

add("34 - YoungSnitchLady", ow=(6,1), ss_1 = """
................
................
....xxxxxx......
....xxxxxx......""", ss_4 = """
..xx............
..xx............
....xxxxxx......
....xxxxxx......""")

add("35 - InnKeeper", dung=(15,5), ss_4 = """
....xxxxxxxxxxxx
....xxxxxxxxxxxx
..............xx
..............xx""")

add("36 - Witch", ow=(13, 6), ss_3 = """
xxxxxx11........
xxxxxx11........
..............xx
..............xx""")

add("39 - Locksmith", ow=(10, 0), ss_4 = """
................
................
..........xxxx..
..........xxxx..""")

add("3a - MagicBat", dung=(9, 32), ss_4 = """
................
................
..........xxxx..
..........xxxx.x""")

add("3b - TreeTop", ow=(26, 6), ss_1 = """
xxxxxxxxxx......
xxxxxxxxxx......
xxxxxxxxxxxx....
xxxxxxxxxxxx....""", palette = 0)

add("3c - TroughBoy", ow=(6, 1), ss_3 = """
4444............
4444............
55........55....
55........55....""")

add("3d - OldSnitchLady", ow=(6, 1), ss_4 = """
xxxx............
xxxx............
xxxxxxxxxx......
xxxxxxxxxx......""")

# rock crab is included in two sheets
add("3e - RockCrab", ow=(10, 0), ss_4 = """
................
................
......xxxx......
......xxxx......""", palette = 0)

add("3e - RockCrab (S16)", ow=(33, 0), ss_4 = """
................
................
......xxxx......
......xxxx......""", palette = 0)


add("3f - PalaceGuard", ow=(2, 1), ss_1 = """
xx..............
xx..............
......xxxxx.....
......xxxxx.....""", palette = 4)

add("3f - PalaceGuard", ow=(2, 1), ss_2 = """
xxxxxxxx......xx
xxxxxxxx......xx
....xx..........
....xx..........""")


# --- Soldiers, Guards, and Castle Enemies (IDs 0x40 - 0x5F) ---

add("40 - ElectricBarrier", ow=(2, 1), ss_4 = """
..........xxxx..
..........xxxx..
......xxxx......
......xxxx......""")


add("41 - Soldier", dung=(3, 0), ss_2 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""")

add("41 - Soldier (DW)", dung=(36, 37), ss_2 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", ss_3 = """
................
................
......xxxxxxxxx.
......xxxxxxxxx.
""")

add("44 - Warrior", dung=(33, 38), ss_1 = """
xxxxxx..........
xxxxxx..........
................
................""")

add("47 - GrassArcher", ow=(4, 3), ss_1 = """
..xxxxxxxxxxxxxx
..xxxxxxxxxxxxxx
11xxxx..........
11xxxx..........""")

add("4a - RedBombKnight", ow=(1, 3), ss_1 = """
xxxxxxxxxxxxxxx.
xxxxxxxxxxxxxxx.
..xx........xxxx
..xx........xxx.""")

add("4b - GreenKnifeGuard", ow=(2, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xx....xxxxxxxxxx
xx....xxxxxxxxxx""")

add("4c - Geldman", ow=(8, 4), ss_3 = """
................
................
xxxxxxxx........
xxxxxxxxx.......""")

add("4d - Toppo", ow=(4, 3), ss_4 = """
xxxx............
xxxx............
................
................""")

add("4e - Popo", dung=(8, 11), ss_2 = """
xxxx............
xxxx............
................
................""")

add("51 - ArmosStatue", ow=(5, 7), ss_4 = """
xx..............
xx..............
xx..............
xx..............""", palette = 5)

add("52 - ZoraKing", ow=(14, 8), ss_3 = """
........2222....
........2222....
........2.......
................""", ss_4 = """
xxxx2222........
xxxx2222........
xxxx2222..xx....
xxxx2222..xx....""")

add("53 - ArmosKnight", dung=(9, 11), ss_4 = """
xxxxxxxxxx......
xxxxxxxxxx......
xxxxxx..........
xxxxxx..........""")

add("54 - Lanmolas", dung=(11, 4), ss_4 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""")

add("55 - FireBallZora", lw=(4, 3), dw=(22, 10), ss_3 = """
........2222....
........2222....
....xxxx2.......
....xxxx........""")

add("56 - WalkingZora", ow=(14, 8), ss_4 = """
........xxxxxxxx
........xxxxxxxx
........xx..xxxx
........xx..xxxx""")

add("57 - HyliaObstacle", ow=(8, 4), ss_3 = """
..............xx
..............xx
..............xx
..............xx""")

add("58 - Crab", ow=(4, 3), ss_3 = """
..............xx
..............xx
..............xx
..............xx""")

add("59 - LostWoodsBird", ow=(12, 1), ss_4 = """
................
................
...xxxx.........
...xxxx.........""")

add("5a - LostWoodsSquirrel", ow=(12, 1), ss_4 = """
.....xx.........
.....xx.........
.xx.............
.xx.............""")

add("5b - Spark", dung=(17, 10), ss_1 = """
................
................
........xx......
........xx......""", palette = 2)

add("5d - Roller", dung=(30, 24), ss_3 = """
........xxx.....
........xxx.....
................
................""")

add("5f - Roller", dung=(30, 24), ss_3 = """
..............xx
..............xx
..............xx
................""")
  
# --- Dungeon Hazards, NPCs, and Mid-game Enemies (IDs 0x60 - 0x7F) ---

add("61 - Beamos", dung=(10, 9), ss_2 = """
........xx555...
........xx551...
........xx......
........xx......""")

add("62 - MasterSword", ow=(12, 1), ss_3 = """
xxxxxxxx........
xxxxxxxx........
xxxxxx..........
xxxxxx..........""", ss_4 = """
...xx...........
...xx...........
x......xx.......
x......xx.......""")

add("63 - DebirandoPit", dung=(10, 9), ss_1 = """
..xxxx..........
..xxxx..........
..xx....xxxx....
..xx....xxxx....""")

add("64 - Debirando", dung=(10, 9), ss_1 = """
xx..............
xx..............
xx..............
xx..............""", palette = 3)

add("65 - ArcherGame", dung=(5, 30), ss_1 = """
..xxxxxxxx......
..xxxxxxxx......
..xxxxxxxx.x....
xxxxxxxxxxxx....""", palette = 4)

add("66 - Cannon", dung=(10, 9), ss_1 = """
..........xxxxxx
..........xxxxxx
....44........xx
....44........xx""")

add("6a - MorningStar", dung=(4, 1), ss_1 = """
xxxxxxxxxxxxxxx.
xxxxxxxxxxxxxxx.
..xx......xxxxxx
..xx......xxxxxx""", ss_2 = """
......xxxxxxx.xO
......xxxxxxx.xO
xxxxxxxxxxx.....
xxxxxxxxxxx.....""")

add("6b - CannonTrooper", dung=(4, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxx.""")

add("6d - Rat", dung=(1, 1), ss_3 = """
......xxxxxxxxxx
......xxxxxxxxxx
..xx............
..xx............""")

add("6d - Rat (DW)", dung=(33, 35), ss_3 = """
......xxxxxxxxxx
......xxxxxxxxxx
xxxx............
xxxx............""")



add("6e - Rope", dung=(1, 1), ss_3 = """
................
................
....xxxxxxxx....
....xxxxxxxx....""")

add("6e - Rope (DW)", dung=(33, 35), ss_3 = """
................
................
....xxxxxxxx....
....xxxxxxxx....""")

add("6f - Keese", dung=(1, 1), ss_3 = """
xxxxxx..........
xxxxxx..........
................
................""")

add("6f - Keese (DW)", dung=(33, 35), ss_3 = """
xxxxxx..........
xxxxxx..........
................
................""")

add("70 - KingHelmasaurFireball", dung=(21, 16), ss_3 = """
................
................
............xx..
............xx..""", ss_4 = """
..........xxxx..
..........xxxx.x
................
................""")

add("71 - Leever", dung=(10, 9), ss_1 = """
......xxxx......
......xxxx......
......xxx.......
......xxx.......""")

add("72 - FairyPond", dung=(7, 7), ss_4 = """
........xxxxxxxx
........xxxxxxxx
................
................""")

add("72 - FairyPond", dung=(7, 34), ss_4 = """
................
................
.........xxxxxxx
.........xxxxxxx""")

add("73 - UncleAndPriest", dung=(6, 29), ss_1 = """
OOOOOOxxxxxxxxxx
OOOOOOxxxxxxxxxx
xxxxxxxxxxxx0000
xxxxxxxxxxxx0000""", palette = 4)

add("73 - UncleAndPriest", dung=(13, 21), ss_1 = """
xxxxxxxxxx......
xxxxxxxxxx......
xxxxxxx.xxxxxx..
xxxxxxx.xxxxxx..""", palette = 4)



add("74 - RunningMan", ow=(6, 1), ss_1 = """
................
................
..........xxxxxx
..........xxxxxx""", ss_4 = """
..........xxxxxx
..........xxxxxx
..............xx
..............xx""")

add("75 - BottleVendor", ow=(6, 1), ss_3 = """
........xx......
........xx......
..xx........xx..
..xx........xx..""")

add("78 - MrsSahasrahla", dung=(5, 2), ss_1 = """
................
................
........xx......
........xx......""", ss_3 = """
..............xx
..............xx
................
................""")

add("7a - Agahnim", dung=(24, 27), ss_1 = """
............xxxx
............xxxx
............xxxx
............xxxx""", ss_2 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", ss_4 = """
xxxxxx..xxxx...x
xxxxxx..xxxx..xx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 6)

add("7b - AgahnimBalls", dung=(24, 27), ss_4 = """
......xx....xxx.
......xx....xx..
................
................""", palette = 2)

add("7c - StalfosHead", dung=(8, 11), ss_1 = """
xxxxxx..........
xxxxxx..........
................
................""")

add("7d - BigSpikeBlock", dung=(12, 16), ss_4 = """
....xx..........
....xx..........
................
................""")

add("7e - FireBlade", dung=(17, 10), ss_1 = """
................
................
........xx......
........xx......""")

# --- Dungeon Enemies and Bosses (IDs 0x80 - 0xBF) ---
# This range contains many dungeon-exclusive enemies and all major bosses:
# HelmasaurKing (0x92), Mothula (0x88), Arrghus (0x8C), Blind (0xCE),
# Kholdstare (0xA2), Viterous (0xBD), and others.

add("81 - WaterBug", dung=(17, 10), ss_3 = """
xxxxxx..........
xxxxxx..........
................
................""")

add("83 - GreenEyegore", dung=(10, 9), ss_3 = """
..........xxxxxx
..........xxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxx..""")

add("83 - GreenEyegore", dung=(8, 39), ss_2 = """
....xxxx......xx
....xxxx......xx
..xxxxxx..xxxxxx
..xxxxxx..xxxxxx""")

add("85 - YellowStalfos", dung=(25, 15), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
....xx....xxxxxx
....xx....xxxxxx""")

add("86 - Kodongo", dung=(25, 6), ss_3 = """
............xx..
............xx..
xxxxxxxxxxxxxx..
xxxxxxxxxxxxxx..""")

add("88 - Mothula", dung=(26, 14), ss_3 = """
xxxxxxxxxx.xxxxx
xxxxxxxxxx.xxxxx
xxxxxxxxxx..xxxx
xxxxxxxxxx..xxxx""")

add("89 - MothulaRing", dung=(26, 14), ss_3 = """
..........x.....
..........x.....
..........xx....
..........xx....""")

add("8a - SpikeBlock", dung=(8, 15), ss_4 = """
................
................
......xx........
......xx........""")

add("8b - Gibdo", dung=(19, 13), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxx..........
xxxxxx..........""")

add("8c - Arrghus", dung=(20, 8), ss_3 = """
xxxxxxxxxx......
xxxxxxxxxx......
xxxxxxxxxx......
xxxxxxxxxx......""")

add("8e - TerrorPin", dung=(25, 15), ss_3 = """
..........xx..xx
..........xx..xx
..............xx
..............xx""")

add("8f - Blob", dung=(17, 8), ss_2 = """
xxxxxx..........
xxxxxx..........
x...............
x...............""")

add("90 - WallMaster", dung=(19, 13), ss_3 = """
................
................
......xxxxxxxxxx
......xxxxxxxxxx""")

add("91 - StalfosKnight", dung=(28, 19), ss_2 = """
......xxxxxxxx..
......xxxxxxxx..
.xxxxxxxxxx.....
.xxxxxxxxxx.....""")

add("92 - HelmasaurKing", dung=(21, 16), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxx66
xxxxxxxxxxxxxx66""", ss_4 = """
6666666666xxxxxx
6666666666xxxxxx
6666xxOOOOOOOOOO
6666xxOOOOOOOOOO""", palette = 5)

add("93 - Bumper", dung=(12, 26), ss_4 = """
................
................
............xx..
............xx..""")

add("94 - Pirogusu", dung=(17, 10), ss_3 = """
.......x....xxxx
.......x....xxxx
..........xxxxxx
..........xxxxxx""")

add("95 - LaserEye", dung=(29, 17), ss_4 = """
......xxxx......
..2...xxxx......
................
...2............""")

add("99 - Pengator", dung=(28, 19), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxx..........
xxxxxx..........""")

add("9a - Kyameron", dung=(17, 10), ss_3 = """
......x.xxxx....
......x.xxxx....
xxxxxxxxxx......
xxxxxxxxxx......""")

add("9b - Wizzrobe", dung=(29, 17), ss_3 = """
......xxxxxxxxxx
......xxxxxxxxxx
....xx..........
..xxxx..........""")

add("9b - Wizzrobe (DW)", dung=(36, 37), ss_3 = """
......xxxxxxxxxx
......xxxxxxxxxx
....xx..........
..xxxx..........""")



add("9c - Babasu", dung=(17, 10), ss_2 = """
..............xx
..............xx
...........xxxxx
...........xxxxx""", palette=1)

add("9e - HauntedGroveOstrich", ow=(15, 1), ss_3 = """
xxxxxx..........
xxxxxx..........
xxxxxx..........
xxxxxx..........""")

add("9f - HauntedGroveRabbit", ow=(15, 1), ss_3 = """
........xxxxxx..
........xxxxxx..
......xx........
......xx........""")

add("A0 - HauntedGroveBird", ow=(15, 1), ss_3 = """
......xx......xx
......xx......xx
............xxx.
............xx..""")

add("A1 - Freezor", dung=(28, 19), ss_3 = """
................
................
......xxxxxxxxxx
......xxxxxxxxxx""")

add("A2 - Kholdstare", dung=(22, 20), ss_3 = """
xxxxxxxx....xx..
xxxxxxxx....xx..
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""")

add("A4 - FallingIce", dung=(22, 20), ss_3 = """
........xxxx..xx
........xxxx..xx
................
................""")

add("A5 - Zazak", dung=(27, 23), ss_3 = """
xxxxxxxxxx......
xxxxxxxxxx......
xxxxxxxxxx......
xxxxxxxxxx......""")

add("A8 - GreenZirro", ow=(19, 14), ss_4 = """
xxxxxxxx........
xxxxxxxx........
xxxxxxxx........
xxxxxxxx.......7""")

add("AA - Pikit", ow=(19, 14), ss_4 = """
........xxxxxxxx
........xxxxxxxx
..........xxxxx.
..........xxxxx.""")

add("AD - OldMan", dung=(1, 7), ss_3 = """
................
................
............xxxx
............xxxx""")


add("B4 - PurpleChest", ow=(21, 16), ss_1 = """
................
................
..............xx
..............xx""", ss_4 = """
................
................
..............xx
..............xx""")

add("B5 - BombShop", dung=(5, 31), ss_2 = """
........xx..xxxx
........xx..xx44
................
................""", palette = 1)

add("B6 - Kiki", ow=(23, 13), ss_4 = """
..........xxx.xx
..........xxxxxx
............xxxx
............xxxx""")

add("B9 - BullyAndPinkBall", dw=(20, 12), ss_4 = """
....xx..........
....xx..........
xxxx............
xxxx............""", palette = 6)

add("B9 - BullyAndPinkBall", dw=(20, 12), ss_4 = """
xxxx............
xxxx............
................
................""")

add("BB - Shopkeeper", dung=(5, 7), ss_1 = """
xxxx............
xxxx............
xxxx............
xxxx............""", palette = 5)

add("BB - Shopkeeper", dung=(5, 7), ss_4 = """
11..22......4422
11..22......4422
........22......
........22......""")

add("BB - Shopkeeper", dung=(15, 31), ss_2 = """
......xx........
......xx........
................
................""", palette = 6)

add("BB - Shopkeeper", dung=(40, 5), ss_1 = """
xxxxxxxx..xx..xx
xxxxxxxx..xx..xx
xxxxxx......xx..
xxxxxx......xx..""", palette = 6)

add("BB - Shopkeeper", dung=(15, 5), ss_1 = """
xxxx............
xxxx............
xx..............
xx..............""", palette = 6)

add("BB - Shopkeeper", dung=(42, 32), ss_1 = """
xxxxxxxx..xx..xx
xxxxxxxx..xx..xx
xxxxxx..........
xxxxxx..........""", palette = 6)

add("BC - Drunkard", dung=(15, 5), ss_1 = """
......xx........
......xx........
..xx............
..xx............""", ss_3 = """
................
................
..............xx
................""", palette = 4)

add("BD - Viterous", dung=(22, 18), ss_4 = """
xxxxxxxxxxxx....
xxxxxxxxxxxx....
xxxxxxxxxxxx....
xxxxxxxxxxxx....""")

add("BF - ViterousLightning", dung=(22, 18), ss_4 = """
............xxxx
............xxxx
............xxxx
............xxxx""")

add("C0 - Catfish", ow=(22, 10), ss_3 = """
......xxxxxxxxx.
......xxxxxxxxx.
........x...xxx.
.............xx.""")

add("C1 - CutsceneAgahnim", dung=(18, 12), ss_1 = """
xxxxxx..........
xxxxxx..........
xxxxxxxx........
xxxxxxx.........""", ss_3 = """
xxxx............
xxxx............
xxxx............
xxxx............""", ss_4 = """
..............2.
...............x
............xxxx
............xxxx""", palette = 5)

add("C3 - Gibo", dung=(27, 23), ss_3 = """
..........xxxxxx
..........xxxxxx
..........xxxxxx
..........xxxxxx""")

add("C7 - Pokey", dung=(30, 24), ss_3 = """
xxxx............
xxxx............
xxxx............
xxxx............""")

add("C8 - BigFairy", dung=(7, 7), ss_3 = """
..........xxxxxx
..........xxxxxx
..........xxxxxx
..........xxxxxx""")

add("C9 - Tektite", dw=(5, 7), ss_4 = """
........xxxx....
........xxxx....
..........xx....
..........xx....""", palette = 4)

add("CA - ChainChomp", dung=(30, 24), ss_3 = """
......xx...xxx..
......xx....xx..
......xxxxxx....
......xxxxxx....""")

# --- Late-Game Bosses and Dark World Enemies (IDs 0xC0 - 0xDF) ---
# Includes final dungeon bosses: Trinexx (0xCB), Blind (0xCE), Ganon (0xD6).
# Multi-palette sprites like Trinexx use digit characters (e.g. '5') in the
# grid to assign specific tile groups to different palette indices.

add("CB - Trinexx", dung=(23, 25), ss_1 = """
xxxx55xx55xxxxXX
xxxx55xx55xxxxXX
xxxx55xxxxxxxxXX
xxxx55xxxxxxxxXX""", ss_4 = """
xxxxxxXXXXXXXXXX
xxxxxxXXXXXXXXXX
xxxxxxxxxxXXXXxx
xxxxxxxxxxXXXXxx""")

add("CE - Blind", dung=(32, 23), ss_3 = """
xxxxxxxxxxxxxx66
xxxxxxxxxxxxxx66
666666xxxxxx6666
666666xxxxxx6666""", palette = 5)

add("CF - Swamola", ow=(22, 15), ss_4 = """
xxxx............
xxxx............
xxxxxxxxxxxx....
xxxxxxxxxxxx....""")

add("D0 - Lynel", ow=(20, 12), ss_4 = """
......xxxxxxxxxx
......xxxxxxxxxx
....xxxxxxxxxxxx
....xxxxxxxxxxxx""")

add("D5 - DigGameGuy", ow=(27, 18), ss_2 = """
xxxxxxx.........
xxxxxxx.........
................
................""", palette = 5)

add("D6 - Ganon", dung=(34, 33), ss_2 = """
....xx..........
....xx..........
xxxxxx..........
xxxxxx..........""", palette = 4)

add("D6 - Ganon", dung=(34, 33), ss_1 = """
xx5555xxxxxxxx55
xx5555xxxxxxxx55
xx5555xxxxxxxxxx
xx5555xxxxxxxxxx""", ss_2 = """
xxxx..xxxxxxxx55
xxxx..xxxxxxxx55
......xxxxxxxx55
......xxxxxxxx55""", ss_3 = """
xxxx55xxxxxxxxxx
xxxx55xxxxxxxxxx
xxxx55xxxxxxxxxx
xxxx55xxxxxxxxxx""", ss_4 = """
xxxxxx5555xxxx55
xxxxxx5555xxxx55
xxxxxx5555xxxx55
xxxxxx5555xxxx55""", palette = 6)

# --- Items, Objects, and Miscellaneous Sprites (IDs 0xE0 - 0xF4) ---

add("E7 - Mushroom", ow=(7, 5), ss_4 = """
................
................
..............xx
..............xx""", palette = 3)

add("E8 - FakeSword", ow=(7, 5), ss_4 = """
................
................
................
....xx..........""")

add("E9 - PotionShop", dung=(5, 5), ss_4 = """
................
................
......xx........
......xx........""", palette = 2)

add("ED - SomariaPlatform", dung=(30, 24), ss_3 = """
................
................
............XX..
............XX..""")

add("EE - MovableMantle", dung=(3, 0), ss_1 = """
............xxxx
............xxxx
............xxxx
............xxxx""")

add("F2 - MedallionTablet", ow=(8, 4), ss_3 = """
..........xxxx..
..........xxxx..
..........xxxx..
..........xxxx..""")

add("F4 - FallingRocks", ow=(5, 7), ss_4 = """
............xxxx
............xxxx
............xxxx
............xxxx""")

# --- Raw/Unknown Sheets (IDs "XX") ---
# These entries capture SNES graphics sheets that are not tied to a specific
# named sprite. They include intro/ending cutscene graphics, map tiles,
# tagalong character sheets, and tiles whose purpose has not been identified
# in the reverse-engineering effort. All use palette index 4 and mark their
# entire VRAM slot as occupied (full 4x16 grid of 'x').

add("XX - Unknown Sheet 00", sheet=(0, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 01", sheet=(1, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 02", sheet=(2, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 03", sheet=(3, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 04", sheet=(4, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 05", sheet=(5, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 06", sheet=(6, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 07", sheet=(7, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 08", sheet=(8, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 09", sheet=(9, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 10", sheet=(10, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 11", sheet=(11, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 14", ow=(37, 1), ss_1 = """
........xx..xx..
........xx..xx..
........xxxx..xx
........xxxx..xx""", palette = 4)


add("XX - Unknown Sheet 15", ow=(26, 1), ss_1 = """
..........xxxx..
..........xxxx..
............xxxx
............xxxx""", palette = 4)


add("XX - Unknown Sheet 21", ow=(21, 1), ss_1 = """
............xx..
............xx..
................
................""", palette = 4)

add("XX - Unknown Sheet 27", ow=(25, 1), ss_4 = """
................
................
...............x
................""", palette = 4)

add("XX - Unknown Sheet 37", ow=(93, 1), ss_3 = """
................
................
..xx..xxxx......
......xxxx......""", palette = 4)


add("XX - Unknown Sheet 39", ow=(94, 1), ss_3 = """
....xx..........
....xx.....x....
....xx..........
....xx........xx""", palette = 4)

add("XX - Unknown Sheet 41", dung=(36, 1), ss_3 = """
xxxxxx..........
xxxxxx..........
xxxx...........x
xx.............x""", palette = 4)


add("XX - Unknown Sheet 42", ow=(107, 1), ss_3 = """
.......xxx......
.......xxx......
................
................""", palette = 4)

add("XX - Unknown Sheet 43", dung=(16, 1), ss_4 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 44", ow=(72, 1), ss_2 = """
.............x..
.............x..
xx..............
xx..............""", palette = 4)

add("XX - Unknown Sheet 45", ow=(45, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 46", ow=(73, 1), ss_3 = """
xxxxxxxxxx......
xxxxxxxxxx......
................
..............xx""", palette = 4)

add("XX - Intro Sheet 50", sheet=(50, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Ending Sheet 52", sheet=(52, 1), ss_2 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Ending Sheet 53", sheet=(53, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 54", ow=(40, 1), ss_4 = """
xxx....x........
xxx....x........
................
................""", palette = 4)

add("XX - Unknown Sheet 55", ow=(12, 1), ss_3 = """
............xxxx
............xxxx
.............xxx
.............xxx""", palette = 4)

add("XX - Unknown Sheet 72", ow=(2, 1), ss_1 = """
................
................
...........xxxxx
...........xxxxx""", palette = 4)

add("XX - Unknown Sheet 75", ow=(27, 1), ss_1 = """
................
................
.............x..
............xx..""", palette = 4)


add("XX - Unknown Sheet 76", ow=(13, 1), ss_3 = """
..............xx
..............xx
..........xx....
..........xx....""", palette = 4)

add("XX - Unknown Sheet 82", dung=(19, 1), ss_4 = """
xxxx......xxxx..
xx.x......xxxx..
x.x.....xxxx....
xxx.....xxxx....""", palette = 4)

add("XX - Unknown Sheet 83", dung=(38, 1), ss_4 = """
...xxxxxxx....xx
..xxxxxxxx....xx
xxxxxxxxxx..xxxx
xxxxxxxxxx..xxxx""", palette = 4)

add("XX - Unknown Sheet 84", sheet=(84, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 85", dung=(63, 1), ss_1 = """
......xxxxxx....
......xxxxxx....
........xxxx....
.......xxxxx....""", palette = 4)

add("XX - Map Sheet 86", sheet=(86, 1), ss_2 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Map Sheet 87", sheet=(87, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 88", sheet=(88, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Tagalong S89", sheet=(89, 1), ss_4 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 90", dung=(5, 1), ss_4 = """
..xx..xxxxxx....
..xx..xxxxxx....
xxxxxx....xxxxxx
xxxxxx....xxxxxx""", palette = 4)

add("XX - Unknown Sheet 91", sheet=(91, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 92", sheet=(92, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 93", ow=(43, 1), ss_3 = """
xxxxxxxxxxxx....
xxxxxxxxxxxx....
xxxxxxxxxxxx....
xxxxxxxxxxxx....""", palette = 4)

add("XX - Unknown Sheet 94", sheet=(94, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 95", sheet=(95, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Unknown Sheet 96", sheet=(96, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Map Sheet 97", sheet=(97, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Map Sheet 98", sheet=(98, 1), ss_2 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Map Sheet 99", sheet=(99, 1), ss_3 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Tagalong S100", sheet=(100, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)

add("XX - Tagalong S101", sheet=(101, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)


add("XX - Tagalong S102", sheet=(102, 1), ss_1 = """
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx
xxxxxxxxxxxxxxxx""", palette = 4)
