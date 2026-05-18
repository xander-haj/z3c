# =============================================================================
# compile_resources.py
# Master asset compilation script for the zelda3 C reimplementation.
#
# This script reads all intermediate data files (YAML room definitions,
# TXT dialogue, PNG sprite sheets, and raw ROM data) and compiles them
# into a single binary file: zelda3_assets.dat. That file is the only
# asset file the C game engine loads at runtime.
#
# Pipeline overview:
#   1. extract_resources.py extracts ROM data into human-editable formats
#      (YAML, TXT, PNG) stored in subdirectories of assets/.
#   2. This script (compile_resources.py) reads those intermediate files,
#      re-encodes them into compact binary arrays, and writes them all
#      into zelda3_assets.dat with a header, size table, and data blobs.
#
# Asset file format (zelda3_assets.dat):
#   [16-byte signature "Zelda3_v0     \n\0"]
#   [32-byte SHA-256 hash of all asset key names, used for version check]
#   [32 bytes reserved / zero-padded]
#   [uint32 asset_count] [uint32 key_string_length]
#   [uint32 sizes[asset_count]]  -- byte length of each asset blob
#   [key_sig bytes]              -- null-terminated asset names concatenated
#   [data blobs, each 4-byte aligned]
#
# The global `assets` dict accumulates every named data array during
# compilation. Each entry stores a type tag (uint8, uint16, int8, int16,
# or packed) and the raw binary bytes for that asset.
# =============================================================================

# Standard library imports
import sys
import text_compression
import util
from PIL import Image
import yaml
import tables
import compile_music
import array, hashlib, struct
from util import cache
import sprite_sheets
import argparse
import os

# Flatten a list of lists into a single flat list.
# Used when multiple entrance/starting-point records each produce a list of values
# that must be concatenated into one contiguous asset array.
def flatten(xss):
    return [x for xs in xss for x in xs]

# Invert a dict so that {index: name} becomes {name: index}.
# Used to look up numeric indices from human-readable name strings
# (e.g., music track names to their ROM byte values).
def invert_dict(xs):
  return {s:i for i,s in xs.items()}

# Global dictionary that accumulates all compiled assets during a run.
# Keys are C-style identifier names (e.g., "kPalette_DungBgMain").
# Values are tuples of (type_tag, raw_bytes) where type_tag is one of:
#   'uint8', 'uint16', 'int8', 'int16', 'packed'
# The type tag tells the C engine how to interpret the binary blob.
assets = {}

# Register an asset as an array of unsigned 8-bit integers.
# name: the C identifier the engine uses to reference this data.
# data: an iterable of integer values in the range [0, 255].
# The assert prevents accidental double-registration of the same asset name.
def add_asset_uint8(name, data):
  assert name not in assets
  assets[name] = ('uint8', bytes(array.array('B', data)))

# Register an asset as an array of unsigned 16-bit integers (little-endian).
# name: C identifier. data: iterable of integers in [0, 65535].
def add_asset_uint16(name, data):
  assert name not in assets
  assets[name] = ('uint16', bytes(array.array('H', data)))

# Register an asset as an array of signed 8-bit integers.
# name: C identifier. data: iterable of integers in [-128, 127].
def add_asset_int8(name, data):
  assert name not in assets
  assets[name] = ('int8', bytes(array.array('b', data)))

# Register an asset as an array of signed 16-bit integers (little-endian).
# name: C identifier. data: iterable of integers in [-32768, 32767].
def add_asset_int16(name, data):
  assert name not in assets
  assets[name] = ('int16', bytes(array.array('h', data)))

# Register an asset as a "packed" array -- a variable-length sub-array container.
# data: a list of bytes-like sub-arrays. pack_arrays() prepends an offset table
# so the C engine can index into individual sub-arrays at runtime.
def add_asset_packed(name, data):
  assert name not in assets
  assets[name] = ('packed', pack_arrays(data))

# Compile the overworld map32-to-map16 tile mapping table.
# The overworld uses two levels of tile indirection: 32x32-pixel "map32" tiles
# are each composed of four 16x16-pixel "map16" tiles. This function reads
# map32_to_map16.txt (one map32 entry per line, listing its four map16 indices)
# and packs them into four separate uint8 arrays, one per quadrant corner.
# The packing interleaves groups of 4 map32 entries and splits each 10-bit
# map16 index into low-byte and high-nibble parts for compact storage.
def print_map32_to_map16():
  # Parse the text file into a dict: map32_index -> [map16_TL, map16_TR, map16_BL, map16_BR]
  tab = {}
  for line in open('map32_to_map16.txt'):
    line = line.strip()
    x, xs = line.split(':', 1)
    tab[int(x)] = [int(t) for t in xs.split(',')]

  # Pack four 10-bit map16 values into 6 bytes:
  # bytes 0-3 hold the low 8 bits of each value,
  # bytes 4-5 hold the high 2 bits packed as nibbles (two per byte).
  def pack(*a):
    res=[0] * 6
    res[0] = a[0] & 0xff
    res[1] = a[1] & 0xff
    res[2] = a[2] & 0xff
    res[3] = a[3] & 0xff
    res[4] = (a[0] >> 8) << 4 | (a[1] >> 8)
    res[5] = (a[2] >> 8) << 4 | (a[3] >> 8)
    return res

  # Build four output arrays, one per corner of the map32 tile (TL, TR, BL, BR).
  # Process map32 entries in groups of 4 for interleaved packing.
  res = [[],[],[],[]]
  for i in range(0, len(tab), 4):
    for j in range(4):
      res[j].extend(pack(tab[i][j], tab[i+1][j], tab[i+2][j], tab[i+3][j]))

  add_asset_uint8('kMap32ToMap16_0', res[0])
  add_asset_uint8('kMap32ToMap16_1', res[1])
  add_asset_uint8('kMap32ToMap16_2', res[2])
  add_asset_uint8('kMap32ToMap16_3', res[3])

# Read a dialogue text file and compress all dialogue strings for the given language.
# fname: path to the dialogue file (one line per string, format "index: text").
# lang: language code (e.g., 'us') passed to the text_compression module.
# Returns a list of compressed byte arrays, one per dialogue string.
def compress_dialogue(fname, lang):
  lines = []
  for line in open(fname, encoding='utf8').read().splitlines():
    # Each line is "numeric_id: dialogue_text" -- we only need the text portion
    a, b = line.split(': ', 1)
    lines.append(b)
  return text_compression.compress_strings(lines, lang)

# Wrap raw data in the SNES LZ "store" format -- literal blocks only, no actual compression.
# This produces a valid SNES LZ stream that simply stores the data verbatim in chunks
# of up to 1024 bytes each. Each chunk has a 2-byte header: the high nibble 0xE signals
# a literal (uncompressed) block and the remaining 10 bits encode (length - 1).
# The stream is terminated by 0xFF. The C engine's decompressor accepts this format
# wherever it expects SNES LZ-compressed data.
# r: input bytes/list of ints. Returns a list of ints representing the LZ store stream.
def compress_store(r):
  rr = []
  j, jend = 0, len(r)
  while j < jend:
    # Each chunk can hold at most 1024 bytes of literal data
    n = min(jend - j, 1024)
    # 2-byte header: 0xE0 | high bits of (length-1), then low byte of (length-1)
    rr.append(0xe0 | (n - 1) >> 8)
    rr.append((n - 1) & 0xff)
    rr.extend(r[j:j+n])
    j += n
  # 0xFF marks end of the LZ stream
  rr.append(0xff)
  return rr
  
# Pack a list of variable-length sub-arrays into a single binary blob with an offset table.
# This is the core serialization for any asset that contains multiple sub-entries
# (e.g., per-room sprite data, per-sheet graphics). The C engine uses the offset table
# to jump directly to any sub-array by index.
#
# Format (small mode, when total data < 64KB and array count <= 8192):
#   [uint16 offsets[count-1]]  -- byte offset from start of data region to each sub-array
#   [sub-array data concatenated]
#   [uint16 (count - 1)]      -- trailer storing the number of sub-arrays minus one
#
# Format (large mode, when data >= 64KB or count > 8192):
#   [uint32 offsets[count-1]]  -- 32-bit offsets for large data
#   [sub-array data concatenated]
#   [uint16 (8192 + count - 1)]  -- trailer; values >= 8192 signal 32-bit offset mode
#
# arr: list of bytes-like objects (one per sub-array). Returns a single bytes blob.
# Pack arrays and determine automatically the index size
def pack_arrays(arr):
  if len(arr) == 0:
    return b''
  # Build cumulative byte offsets for each sub-array (skip the first, which starts at 0)
  all_offs, offs = [], 0
  for i in range(len(arr) - 1):
    offs += len(arr[i])
    all_offs.append(offs)
  # Choose 16-bit or 32-bit offsets depending on total data size and array count
  if offs < 65536 and len(arr) <= 8192:
    return b''.join([struct.pack('H', i) for i in all_offs] + arr + [struct.pack('H', len(arr) - 1)])
  else:
    return b''.join([struct.pack('I', i) for i in all_offs] + arr + [struct.pack('H', 8192 + len(arr) - 1)])

# Compile all sprite and background tileset graphics into packed asset arrays.
# The SNES stores graphics as compressed 4bpp tilesets at known ROM addresses.
# If --sprites-from-png was specified, sprite sheets 0-102 are loaded from
# edited PNG files instead, allowing custom sprite modifications. Sheets 0-11
# are stored uncompressed in the ROM (0x600 bytes each); sheets 12+ are
# SNES LZ-compressed and stored with their compressed length.
# args: parsed command-line arguments (uses args.sprites_from_png flag).
def print_images(args):
  # Load edited sprite sheets from PNG if the flag is set; otherwise use ROM data
  sprsheet = sprite_sheets.load_sprite_sheets() if args.sprites_from_png else None

  # Compile 108 sprite tileset sheets
  all = []
  for i in range(108):
    if sprsheet != None and i < 103:
      # Use the PNG-sourced sprite sheet, re-encoded to SNES 4bpp format
      all.append(sprsheet.encode_sheet_in_snes_format(i))
    elif i < 12:
      # Sheets 0-11 are uncompressed in ROM, fixed size 0x600 bytes (1536 bytes)
      all.append(bytes(ROM.get_bytes(tables.kCompSpritePtrs[i], 0x600)))
    else:
      # Sheets 12+ are LZ-compressed; decompress to find the compressed length,
      # then store the raw compressed bytes (the C engine decompresses at runtime)
      decomp, comp_len = util.decomp(tables.kCompSpritePtrs[i], ROM.get_byte, False, True)
      all.append(bytes(ROM.get_bytes(tables.kCompSpritePtrs[i], comp_len)))
  add_asset_packed('kSprGfx', all)

  # Compile all background tileset sheets (always from ROM, always LZ-compressed)
  all = []
  for i in range(len(tables.kCompBgPtrs)):
    decomp, comp_len = util.decomp(tables.kCompBgPtrs[i], ROM.get_byte, False, True)
    all.append(bytes(ROM.get_bytes(tables.kCompBgPtrs[i], comp_len)))
  add_asset_packed('kBgGfx', all)

def print_dialogue(args):
  from text_compression import dialogue_filename, kLanguages

  languages = ['us']
  if args.languages:
    for a in args.languages.split(','):
      if a in languages or a not in kLanguages:
        raise Exception(f'Language {a} is not valid')
      name = dialogue_filename(a)
      if not os.path.exists(name):
        raise Exception(f'{name} not found. You need to extract it with --extract-dialogue using the ROM of that language.')
      languages.append(a)

  all_langs, all_fonts, mappings = [], [], []
  for i, lang in enumerate(languages):
    dict_packed = pack_arrays(text_compression.encode_dictionary(lang))
    dialogue_packed = pack_arrays(compress_dialogue(dialogue_filename(lang), lang))
    all_langs.append(pack_arrays([dict_packed, dialogue_packed]))
    font_data, font_width = sprite_sheets.encode_font_from_png(lang)
    all_fonts.append(pack_arrays([font_data, font_width]))
    flags = text_compression.uses_new_format(lang)
    if i != 0: flags |= 2 # no us rom match?
    mappings.append(pack_arrays([lang.encode('utf8'), bytearray([i, i, flags])]))
  add_asset_packed('kDialogue', all_langs)
  add_asset_packed('kDialogueFont', all_fonts)
  add_asset_packed('kDialogueMap', mappings)

def print_misc(args):
  add_asset_uint8('kOverworldMapGfx', ROM.get_bytes(0x18c000, 0x4000))
  add_asset_uint8('kLightOverworldTilemap', ROM.get_bytes(0xac727, 4096))
  add_asset_uint8('kDarkOverworldTilemap', ROM.get_bytes(0xaD727, 1024))

  add_asset_uint16('kPredefinedTileData', ROM.get_words(0x9B52, 6438))

  add_asset_uint16('kMap16ToMap8', ROM.get_words(0x8f8000, 3752 * 4))

  add_asset_uint8('kGeneratedWishPondItem', ROM.get_bytes(0x888450, 256))
  add_asset_uint8('kGeneratedBombosArr', ROM.get_bytes(0x8890FC, 256))

  add_asset_uint8('kGeneratedEndSequence15', ROM.get_bytes(0x8ead25, 256))
  add_asset_uint8('kEnding_Credits_Text', ROM.get_bytes(0x8EB178, 1989))
  add_asset_uint16('kEnding_Credits_Offs', ROM.get_words(0x8EB93d, 394))
  add_asset_uint16('kEnding_MapData', ROM.get_words(0x8EB038, 160))
  add_asset_uint16('kEnding0_Offs', ROM.get_words(0x8EC2E1, 17))
  add_asset_uint8('kEnding0_Data', ROM.get_bytes(0x8EBF4C, 917))

  add_asset_uint16('kPalette_DungBgMain', ROM.get_words(0x9BD734, 1800))
  add_asset_uint16('kPalette_MainSpr', ROM.get_words(0x9BD218, 120))

  add_asset_uint16('kPalette_ArmorAndGloves', sprite_sheets.override_armor_palette or ROM.get_words(0x9BD308, 75))
  add_asset_uint16('kPalette_Sword', ROM.get_words(0x9BD630, 12))
  add_asset_uint16('kPalette_Shield', ROM.get_words(0x9BD648, 12))

  add_asset_uint16('kPalette_SpriteAux3', ROM.get_words(0x9BD39E, 84))
  add_asset_uint16('kPalette_MiscSprite_Indoors', ROM.get_words(0x9BD446, 77))
  add_asset_uint16('kPalette_SpriteAux1', ROM.get_words(0x9BD4E0, 168))

  add_asset_uint16('kPalette_OverworldBgMain', ROM.get_words(0x9BE6C8, 210))
  add_asset_uint16('kPalette_OverworldBgAux12', ROM.get_words(0x9BE86C, 420))
  add_asset_uint16('kPalette_OverworldBgAux3', ROM.get_words(0x9BE604, 98))
  add_asset_uint16('kPalette_PalaceMapBg', ROM.get_words(0x9BE544, 96))
  add_asset_uint16('kPalette_PalaceMapSpr', ROM.get_words(0x9BD70A, 21))
  add_asset_uint16('kHudPalData', ROM.get_words(0x9BD660, 64))

  add_asset_uint16('kOverworldMapPaletteData', ROM.get_words(0x8ADB27, 256))

@cache
def load_overworld_yaml(room):
  return yaml.safe_load(open('overworld/overworld-%d.yaml' % room, 'r'))
    
def print_overworld():
  r = []
  for i in range(160):
    addr = ROM.get_24(0x82F94D + i * 3)
    decomp, comp_len = util.decomp(addr, ROM.get_byte, True, True)
    r.append(bytes(ROM.get_bytes(addr, comp_len)))
  add_asset_packed('kOverworld_Hibytes_Comp', r)

  r = []
  for i in range(160):
    addr = ROM.get_24(0x82FB2D + i * 3)
    decomp, comp_len = util.decomp(addr, ROM.get_byte, True, True)
    r.append(bytes(ROM.get_bytes(addr, comp_len)))
  add_asset_packed('kOverworld_Lobytes_Comp', r)

def is_area_head(i):
  return i >= 128 or ROM.get_byte(0x82A5EC + (i & 63)) == (i & 63)

class OutArrays:
  def __init__(self):
    self.arrs = []
  def add(self, type, name, size, initializer = None, items_per_line = 16):
    t = [initializer] * size
    self.arrs.append((type, name, t, items_per_line))
    setattr(self, name, t)
  def write(self):
    for type, name, arr, items_per_line in self.arrs:
      for i, j in enumerate(arr):
        assert isinstance(j, int), (name, i, j, arr)
      if type == 'uint8':
        add_asset_uint8(name, arr)
      elif type == 'uint16':
        add_asset_uint16(name, arr)
      elif type == 'int8':
        add_asset_int8(name, arr)
      elif type == 'int16':
        add_asset_int16(name, arr)
      else:
        assert 0, type
    
def print_overworld_tables():
  A = OutArrays()
  
  A.add('uint8', 'kOverworldMapIsSmall', 192, initializer = 0, items_per_line = 8)
  A.add('uint8', 'kOverworldAuxTileThemeIndexes', 128, items_per_line = 8)
  A.add('uint8', 'kOverworldBgPalettes', 136, items_per_line = 8)
  A.add('uint16', 'kOverworld_SignText', 128, items_per_line = 8)
  A.add('uint8', 'kOwMusicSets', 256, items_per_line = 8)
  A.add('uint8', 'kOwMusicSets2', 96, items_per_line = 8)
  
  def get_music_byte(h, tag):
    return tables.kMusicNamesRev[h['music'][tag]] | tables.kAmbientSoundNameRev[h['ambient'][tag]] << 4

  def get_loadoffs(c, d):
    x, y = c[0] >> 4, c[1] >> 4
    x += d[0]
    y += d[1]
    return (y&0x3f) << 7 | (x&0x3f) << 1

  def awrite(arr, area, key, value):
    arr[key] = value
    if area < 128 and not A.kOverworldMapIsSmall[area]:
      arr[key + 1], arr[key + 8], arr[key + 9] = value, value, value
      
  loaded_areas = [(i, load_overworld_yaml(i)) for i in range(160) if is_area_head(i)]

  for i,y in loaded_areas:
    h = y['Header']
    A.kOverworldMapIsSmall[i] = {'small':1, 'big':0}[h['size']]
    if i < len(A.kOverworldAuxTileThemeIndexes): awrite(A.kOverworldAuxTileThemeIndexes, i, i, h['gfx'])
    if i < len(A.kOverworldBgPalettes): awrite(A.kOverworldBgPalettes, i, i, h['palette'])
    if i < len(A.kOverworld_SignText): awrite(A.kOverworld_SignText, i, i, h['sign_text'])
    if i < 64:
      awrite(A.kOwMusicSets, i, i, get_music_byte(h, 'beginning'))
      awrite(A.kOwMusicSets, i, i + 64, get_music_byte(h, 'zelda'))
      awrite(A.kOwMusicSets, i, i + 128, get_music_byte(h, 'sword'))
      awrite(A.kOwMusicSets, i, i + 192, get_music_byte(h, 'agahnim'))
    elif i < 64 + 96:
      awrite(A.kOwMusicSets2, i, i - 64, get_music_byte(h, 'agahnim'))

  # Allocate bird travel destination arrays — 17 slots total (9 bird + 8 whirlpool)
  for a in ['kBirdTravel_ScreenIndex', 'kBirdTravel_Map16LoadSrcOff', 'kBirdTravel_ScrollX',
            'kBirdTravel_ScrollY', 'kBirdTravel_LinkXCoord', 'kBirdTravel_LinkYCoord',
            'kBirdTravel_CameraXScroll', 'kBirdTravel_CameraYScroll']:
    A.add('uint16', a, 17)
  A.add('int8', 'kBirdTravel_Unk1', 17)
  A.add('int8', 'kBirdTravel_Unk3', 17)
  # Whirlpool source area lookup — maps whirlpool index to its overworld area
  A.add('uint16', 'kWhirlpoolAreas', 8)

  # Populate bird travel and whirlpool warp destinations from each area's Travel list.
  # Bird warps use explicit IDs (slots 0-8); whirlpool warps are auto-assigned (slots 9-16).
  next_whirlpool_id = 0
  for i, y in loaded_areas:
    for t in y['Travel']:
      if 'bird_travel_id' in t:
        # Named bird travel destination — uses a fixed slot index
        j = t['bird_travel_id']
      else:
        # Whirlpool destination — auto-assigned starting at slot 9
        A.kWhirlpoolAreas[next_whirlpool_id] = t['whirlpool_src_area']
        j = next_whirlpool_id + 9
        next_whirlpool_id += 1
      # Convert area index to absolute pixel coordinates for the overworld map.
      # The overworld is an 8-wide grid; low 3 bits give column, bits 3-5 give row.
      base_x, base_y = (i & 7) << 9, (i & 56) << 6
      A.kBirdTravel_ScreenIndex[j] = i
      A.kBirdTravel_Map16LoadSrcOff[j] = get_loadoffs(t['scroll_xy'], t['load_xy'])
      # Scroll, player, and camera positions are stored as area-relative in YAML
      # but need to be absolute pixel coords in the compiled output
      A.kBirdTravel_ScrollX[j] = t['scroll_xy'][0] + base_x
      A.kBirdTravel_ScrollY[j] = t['scroll_xy'][1] + base_y
      A.kBirdTravel_LinkXCoord[j] = t['xy'][0] + base_x
      A.kBirdTravel_LinkYCoord[j] = t['xy'][1] + base_y
      A.kBirdTravel_CameraXScroll[j] = t['camera_xy'][0] + base_x
      A.kBirdTravel_CameraYScroll[j] = t['camera_xy'][1] + base_y
      A.kBirdTravel_Unk1[j] = t['unk'][0]
      A.kBirdTravel_Unk3[j] = t['unk'][1]

  # Overworld entrance tables — 129 slots mapping overworld doorways to dungeon entrances.
  # Each entrance records which area it belongs to, its tile position, and the dungeon entrance ID.
  A.add('uint16', 'kOverworld_Entrance_Area', 129)
  A.add('uint16', 'kOverworld_Entrance_Pos', 129)
  A.add('uint8', 'kOverworld_Entrance_Id', 129)

  for i, y in loaded_areas:
    for e in y['Entrances']:
      j = e['index']
      # Guard against duplicate entrance indices across different areas
      assert A.kOverworld_Entrance_Id[j] == None
      A.kOverworld_Entrance_Area[j] = i
      A.kOverworld_Entrance_Id[j] = e['entrance_id']
      # Pack tile position: x in low bits, y in upper bits (matching SNES map16 layout)
      A.kOverworld_Entrance_Pos[j] = e['x'] << 1 | e['y'] << 7

  # Fall hole tables — 19 overworld pits that drop Link into dungeon rooms.
  # Holes are sorted by entrance ID so the engine can binary-search at runtime.
  A.add('uint16', 'kFallHole_Area', 19)
  A.add('uint16', 'kFallHole_Pos', 19)
  A.add('uint8', 'kFallHole_Entrances', 19)

  # Collect all holes from every area, then sort by entrance_id for the engine's lookup
  holes = []
  for i, y in loaded_areas:
    if 'Holes' not in y: continue
    for e in y['Holes']:
      x, y, j = e['x'], e['y'], e['entrance_id']
      # Pack position with y-8 offset to compensate for the HUD area at the top of screen
      holes.append((j, x << 1 | ((y - 8) & 0x3f) << 7, i))
  for i, (entrance, pos, area) in enumerate(sorted(holes)):
    A.kFallHole_Area[i] = area
    A.kFallHole_Pos[i] = pos
    A.kFallHole_Entrances[i] = entrance

  # Exit data — 79 dungeon-to-overworld exit points with full scroll/position/camera state.
  # These define where Link appears on the overworld when leaving a dungeon room.
  A.add('uint8', 'kExitData_ScreenIndex', 79)
  for a in ['kExitDataRooms', 'kExitData_Map16LoadSrcOff', 'kExitData_ScrollX',
            'kExitData_ScrollY', 'kExitData_XCoord', 'kExitData_YCoord',
            'kExitData_CameraXScroll', 'kExitData_CameraYScroll']:
    A.add('uint16', a, 79)
  # Door arrays default to 0 (no door) — only populated when an exit has a door animation
  A.add('uint16', 'kExitData_NormalDoor', 79, initializer = 0)
  A.add('uint16', 'kExitData_FancyDoor', 79, initializer = 0)
  A.add('int8', 'kExitData_Unk1', 79)
  A.add('int8', 'kExitData_Unk3', 79)

  # Special exit tables — 16 slots for rooms 0x180+ that override the normal exit behavior.
  # These define custom scroll boundaries and graphical settings for the destination area.
  A.add('uint16', 'kSpExit_Top', 16, initializer = 0)
  A.add('uint16', 'kSpExit_Bottom', 16, initializer = 0)
  A.add('uint16', 'kSpExit_Left', 16, initializer = 0)
  A.add('uint16', 'kSpExit_Right', 16, initializer = 0)
  A.add('int16', 'kSpExit_Tab4', 16, initializer = 0)
  A.add('int16', 'kSpExit_Tab5', 16, initializer = 0)
  A.add('int16', 'kSpExit_Tab6', 16, initializer = 0)
  A.add('int16', 'kSpExit_Tab7', 16, initializer = 0)
  A.add('uint16', 'kSpExit_LeftEdgeOfMap', 16, initializer = 0)
  # Graphical overrides for the destination: sprite tileset, auxiliary tiles, palettes
  A.add('uint8', 'kSpExit_Dir', 16, initializer = 0)
  A.add('uint8', 'kSpExit_SprGfx', 16, initializer = 0)
  A.add('uint8', 'kSpExit_AuxGfx', 16, initializer = 0)
  A.add('uint8', 'kSpExit_PalBg', 16, initializer = 0)
  A.add('uint8', 'kSpExit_PalSpr', 16, initializer = 0)
  
  # Populate exit data from each area's Exits list
  for i, y in loaded_areas:
    for e in y['Exits']:
      j = e['index']
      # Compute absolute pixel origin for this overworld area
      base_x, base_y = (i & 7) << 9, (i & 56) << 6
      # Each exit index must be unique across all areas
      assert A.kExitData_ScreenIndex[j] == None
      A.kExitData_ScreenIndex[j] = i
      room = e['room']
      A.kExitDataRooms[j] = e['room']
      A.kExitData_Map16LoadSrcOff[j] = get_loadoffs(e['scroll_xy'], e['load_xy'])
      # Convert area-relative coordinates to absolute overworld pixel coordinates
      A.kExitData_ScrollX[j] = e['scroll_xy'][0] + base_x
      A.kExitData_ScrollY[j] = e['scroll_xy'][1] + base_y
      A.kExitData_XCoord[j] = e['xy'][0] + base_x
      A.kExitData_YCoord[j] = e['xy'][1] + base_y
      A.kExitData_CameraXScroll[j] = e['camera_xy'][0] + base_x
      A.kExitData_CameraYScroll[j] = e['camera_xy'][1] + base_y
      A.kExitData_Unk1[j] = e['unk'][0]
      A.kExitData_Unk3[j] = e['unk'][1]
      # Encode optional door animation tied to this exit
      door = e.get('door')
      if door != None:
        # Normal doors: wooden or bombable — bit 15 flags bombable type
        if door[0] in ('bombable', 'wooden'):
          A.kExitData_NormalDoor[j] = door[1] << 1 | door[2] << 7 | (0x8000 if door[0] == 'bombable' else 0)
        # Fancy doors: palace or sanctuary — bit 15 flags palace type
        elif door[0] in ('palace', 'sanctuary'):
          A.kExitData_FancyDoor[j] = door[1] << 1 | door[2] << 7 | (0x8000 if door[0] == 'palace' else 0)
        else:
          assert e[0] == 'none'
      # Special exits override scroll boundaries and gfx for rooms >= 0x180
      se = e.get('special_exit')
      if se:
        # Index into special exit tables: room 0x180 maps to slot 0, 0x181 to slot 1, etc.
        j = room - 0x180
        # Direction is stored doubled for the engine's 2-byte step size
        A.kSpExit_Dir[j] = se['dir'] * 2
        A.kSpExit_SprGfx[j] = se['spr_gfx']
        A.kSpExit_AuxGfx[j] = se['aux_gfx']
        A.kSpExit_PalBg[j] = se['pal_bg']
        A.kSpExit_PalSpr[j] = se['pal_spr']
        # Scroll boundaries defining the walkable region on the destination screen
        A.kSpExit_Top[j] = se['top']
        A.kSpExit_Bottom[j] = se['bottom']
        A.kSpExit_Left[j] = se['left']
        A.kSpExit_Right[j] = se['right']
        A.kSpExit_LeftEdgeOfMap[j] = se['left_edge_of_map']
        A.kSpExit_Tab4[j] = se['unk4']
        A.kSpExit_Tab5[j] = se['unk5']
        A.kSpExit_Tab6[j] = se['unk6']
        A.kSpExit_Tab7[j] = se['unk7']


  # Overworld secrets/collectibles — variable-length per-area item lists with an offset table.
  # Areas without items point to the last 0xFFFF terminator to avoid needing a null check.
  A.add('uint16', 'kOverworldSecrets_Offs', 128, initializer = None)
  A.add('uint8', 'kOverworldSecrets', 0)
  for i, y in loaded_areas:
    if len(y['Items']):
      # Secrets only exist in the first 128 areas (light/dark world, not special areas)
      assert i < 128
      # Record the byte offset where this area's item list begins
      j = len(A.kOverworldSecrets)
      awrite(A.kOverworldSecrets_Offs, i, i, j)
      for e in y['Items']:
        # Pack tile position and convert item name to its numeric code
        pos = e[0] << 1 | e[1] << 7
        item = tables.kSecretNamesRev[e[2]]
        A.kOverworldSecrets.extend([pos & 0xff, pos >> 8, item])
      # 0xFFFF sentinel marks end of this area's item list
      A.kOverworldSecrets.extend([0xff, 0xff])
  # Areas with no items share a pointer to the final terminator (saves space)
  for i in range(128):
    if A.kOverworldSecrets_Offs[i] == None:
      A.kOverworldSecrets_Offs[i] = len(A.kOverworldSecrets) - 2

  # Overworld sprite placement — 3 game progression stages x 144 areas.
  # Each stage has its own sprite list; the offset table is indexed as stage*144 + area.
  A.add('uint16', 'kOverworldSpriteOffs', 144 * 3, initializer = 0)
  A.add('uint8', 'kOverworldSprites', 0)
  # Per-area sprite tileset and palette indices, organized in 64-area blocks per stage
  A.add('uint8', 'kOverworldSpriteGfx', 256)
  A.add('uint8', 'kOverworldSpritePalettes', 256)
  # Byte 0 = 0xFF acts as the empty/null sprite list that offset 0 points to
  A.kOverworldSprites.append(0xff)
  # Compile sprites for a range of areas belonging to a particular game stage.
  # Parameters:
  #   start, end: area index range to process
  #   stagename: YAML key identifying the sprite stage (e.g. 'Sprites.Beginning')
  #   sprite_stage_idxs: which offset table stage slots this data should fill
  #   infostage: which 64-area block in the gfx/palette arrays to write to
  def do_sprite_range(start, end, stagename, sprite_stage_idxs, infostage):
    for i, y in loaded_areas:
      if i < start or i >= end: continue
      info = y[stagename]['info']
      # Only the first 128 areas (light + dark world) have per-stage gfx/palette settings
      if i < 128:
        awrite(A.kOverworldSpriteGfx, i, (i & 63) + infostage * 64, info['gfx'])
        awrite(A.kOverworldSpritePalettes, i, (i & 63) + infostage * 64, info['palette'])
      if len(y[stagename]['sprites']):
        # Point all applicable stages to this sprite list in the data blob
        for stage in sprite_stage_idxs:
          A.kOverworldSpriteOffs[stage * 144 + i] = len(A.kOverworldSprites)
        # Each sprite is 3 bytes: y-coord, x-coord, sprite type ID
        for e in y[stagename]['sprites']:
          A.kOverworldSprites.extend([e[1], e[0], tables.kSpriteNamesRev[e[2]]])
        # 0xFF sentinel terminates this area's sprite list
        A.kOverworldSprites.append(0xff)
  # Light world areas 0-63 have three distinct stages reflecting game progression
  do_sprite_range(0, 64, 'Sprites.Beginning', [0], 0)
  do_sprite_range(0, 64, 'Sprites.FirstPart', [1], 1)
  do_sprite_range(0, 64, 'Sprites.SecondPart', [2], 2)
  # Dark world + special areas 64-143 share one sprite set across stages 1 and 2
  do_sprite_range(64, 144, 'Sprites', [1, 2], 3)

  # Flush all overworld ArrayBuilder data into the asset dictionary
  A.write()
  # Tile attribute lookup tables read directly from ROM — used for collision/interaction checks
  add_asset_uint8('kMap8DataToTileAttr', ROM.get_bytes(0x8E9459, 512))
  add_asset_uint8('kSomeTileAttr', ROM.get_bytes(0x9bf110, 3824))


# Compile dungeon minimap layout data for the pause screen map display.
# Reads floor layout bitmaps and corresponding tile graphics for all 14 dungeons.
# Parameters: none (reads directly from ROM)
# Returns: nothing (writes kDungMap_FloorLayout and kDungMap_Tiles to asset dict)
def print_dungeon_map():
  r, r2 = [], []
  for i in range(14):
    # Pre-measured byte counts for each dungeon's floor layout data
    kSizes = [75, 125, 50, 75, 175, 75, 50, 75, 50, 200, 150, 75, 100, 200]
    # Floor layout pointer table at 0x8AF605 — values are offsets within bank 0x14
    addr = 0xa0000 + ROM.get_word(0x8AF605 + i * 2)
    b = ROM.get_bytes(addr, kSizes[i])
    # Count non-empty cells (0x0F = empty) to determine how many tile entries to read
    nonzero_bytes = len(b) - b.count(0xf)
    r.append(bytes(b))
    # Tile data pointer table at 0x8AFBE4 — one tile byte per non-empty floor cell
    addr = 0xa0000 + ROM.get_word(0x8AFBE4 + i * 2)
    b = ROM.get_bytes(addr, nonzero_bytes)
    r2.append(bytes(b))

  add_asset_packed('kDungMap_FloorLayout', r)
  add_asset_packed('kDungMap_Tiles', r2)


# Load and parse a single dungeon room YAML file, with results cached via @cache.
# Avoids redundant disk I/O since multiple compilation passes reference the same rooms.
# Parameters:
#   room: integer room index (0-319)
# Returns: parsed YAML dict containing Header, Sprites, Secrets, Layer1-3, Chests, etc.
@cache
def load_dungeon_yaml(room):
  return yaml.safe_load(open('dungeon/dungeon-%d.yaml' % room, 'r'))

# Compile dungeon sprite placement data for all 320 rooms.
# Builds a flat byte array with per-room offsets. Each room's data starts with a sort mode
# byte, followed by 3-byte sprite entries, terminated by 0xFF.
# Parameters: none
# Returns: nothing (writes kDungeonSprites and kDungeonSpriteOffs to asset dict)
def print_dungeon_sprites():
  # Initialize all room offsets to 0 (points to the null entry at data[0..1])
  offsets=[0 for i in range(320)]
  # First two bytes: 0x00 sort mode + 0xFF terminator — the "empty room" placeholder
  data = [0, 0xff]
  for i in range(320):
    y = load_dungeon_yaml(i)
    sortmode = y['Header']['sort_sprites']
    # Rooms with no sprites and default sort mode share the null entry at offset 0
    if len(y['Sprites']) == 0 and sortmode == 0:
      continue
    offsets[i] = len(data)
    data.append(sortmode)
    for s in y['Sprites']:
      xx, yy, f, name = s[:4]
      # Floor flag: upper (0) or lower (1) level of the room
      f = {'upper' : 0, 'lower' : 1}[f]
      # Sprite coordinates are 5-bit tile positions (0-31 range)
      assert xx >= 0 and xx <= 0x1f
      assert yy >= 0 and yy <= 0x1f

      # parse out subcode
      # Subcodes are encoded in the sprite name as "XX.N-name" where N is the subcode value.
      # The subcode bits are split across the two coordinate bytes (3 high bits + 3 low bits).
      ss = 0
      if len(name) > 2 and name[2] == '.':
        j = name.index('-')
        ss = int(name[3:j])
        name = name[0:2] + name[j:]

      name = tables.kSpriteName2Idx[name]
      # Sprites with index >= 0x100 are "overlord" sprites — they use a different encoding
      # where subcode bits are fixed at 7 (all ones) and only the low byte of the ID is stored
      if name >= 0x100:
        data.extend((f << 7 | 0 << 5 | yy, xx | 7 << 5, name & 0xff))
      else:
        # Standard sprite: pack floor, subcode high bits, y into byte 0;
        # x and subcode low bits into byte 1; sprite ID into byte 2
        data.extend((f << 7 | (ss >> 3) << 5 | yy, xx | (ss & 7) << 5, name))
      # 5th element in the sprite tuple indicates a guaranteed item drop (key or big key)
      if len(s) == 5:
        # Drop items are encoded as 3 extra bytes appended after the sprite entry
        kDropTypesToCode = {'drop_key' : [0xfe, 0, 0xe4], 'drop_big_key' : [0xfd, 0, 0xe4]}
        data.extend(kDropTypesToCode[s[-1]])

    # 0xFF terminates this room's sprite list
    data.append(0xff)
  add_asset_uint8('kDungeonSprites', data)
  add_asset_uint16('kDungeonSpriteOffs', offsets)

# Compile dungeon secret/collectible placement data for all 320 rooms.
# Builds a flat buffer: first 640 bytes are a 16-bit offset table (320 rooms x 2 bytes),
# followed by variable-length per-room secret lists terminated by 0xFFFF.
# Parameters: none
# Returns: nothing (writes kDungeonSecrets to asset dict)
def print_dungeon_secrets():
  # First 640 bytes reserved for the per-room offset table (2 bytes per room)
  result = [None] * 640
  for i in range(320):
    y = load_dungeon_yaml(i)
    # Convert a secret entry [x, y, name] into 3 bytes: 16-bit tile position + item code.
    # Position is computed as (x + y*64) * 2 to match the room's 64-wide tile grid addressing.
    def fixone(s):
      x, y, data = s[0], s[1], tables.kSecretNamesRev[s[2]]
      pos = (x + y * 64) * 2
      return [pos & 0xff, pos >> 8, data]
    if len(y['Secrets']):
      # Store the byte offset (as 16-bit little-endian) where this room's secrets begin
      result[i*2+0] = len(result) & 0xff
      result[i*2+1] = len(result) >> 8
      for a in y['Secrets']:
        result.extend(fixone(a))
      # 0xFFFF sentinel marks end of this room's secret list
      result.extend([0xff, 0xff])
  # Rooms with no secrets share a pointer to the final terminator (same pattern as overworld)
  for i in range(320):
    if result[i*2+0] == None:
      l = (len(result) - 2)
      result[i*2+0] = l & 0xff
      result[i*2+1] = l >> 8
  add_asset_uint8('kDungeonSecrets', result)

# Byte-deduplication helper for room header data. Appends 'little' to 'big' while
# reusing any overlapping suffix of 'big' that matches a prefix of 'little'.
# This saves space when consecutive room headers share common leading bytes.
# Parameters:
#   big: the growing byte buffer to append to (modified in-place)
#   little: the new byte sequence to merge in
# Returns: the offset within 'big' where 'little' starts (may overlap existing data)
def append_scan_bytes(big, little):
  # Try the longest possible overlap first, then shrink until a match is found
  for n in range(len(little), -1, -1):
    if n == 0 or big[-n:] == little[:n]:
      offset = len(big) - n
      # Only append the non-overlapping tail of 'little'
      big.extend(little[n:])
      return offset
  
# Main dungeon room compiler — processes all 320 rooms and compiles their tile objects,
# doors, headers, chests, entrances, starting points, default rooms, and overlay rooms.
# This is the largest single compilation function in the pipeline.
# Parameters: none
# Returns: nothing (writes many kDungeon* assets to the asset dict)
def print_dungeon_rooms():
  # Encode a single room layer's tile objects and optional doors into the data stream.
  # Room objects use 3 encoding subtypes based on which name lookup table they appear in.
  # Parameters:
  #   objs: list of tile object dicts with keys 'n' (name), 'x', 'y', 's' (size)
  #   doors: list of door dicts, or None if this layer has no doors
  # Returns: byte offset of the door data within the stream, or None if no doors
  def print_layer(objs, doors):
    door_offset = None
    for o in objs:
      # Type 0 (standard objects): index 0x00-0xF7, position packed with 2-bit w/h size
      if o['n'] in tables.kType0Names_rev:
        index = tables.kType0Names_rev[o['n']] # 0 - 0xf7
        w = int(o['s'][0])
        h = int(o['s'][2])
        assert w >= 0 and w <= 3 and h >= 0 and h <= 3
        p0 = o['x'] * 4 + w
        p1 = o['y'] * 4 + h
        p2 = index
      # Type 1 (extended objects): index >= 0xF8, size bits encoded in the index itself
      elif o['n'] in tables.kType1Names_rev:
        index = tables.kType1Names_rev[o['n']]
        p0 = o['x'] * 4 + (index >> 0 & 3)
        p1 = o['y'] * 4 + (index >> 2 & 3)
        p2 = (index >> 4) + 0xf8
      # Type 2 (special objects): top 6 bits of p0 are 0xFC (binary 111111),
      # remaining bits pack x, y, and index across all 3 bytes
      elif o['n'] in tables.kType2Names_rev:
        index = tables.kType2Names_rev[o['n']]
        x, y = o['x'], o['y']
        #111111xx xxxxyyyy yyiiiiii
        p0 = 0xfc + (x >> 4 & 3)
        p1 = (x << 4 & 0xf0) | (y >> 2 & 0x0f)
        p2 = index | (y << 6 & 0xc0)
      else:
        raise Exception('item %s not found' % o['n'])
      data.extend((p0, p1, p2))
    if doors != None:
      # 0xFFF0 = layer separator marking the start of door data
      data.extend([0xf0, 0xff])
      door_offset = len(data)
      # Each door is 2 bytes: direction in low nibble, position in high nibble, then type byte
      for d in doors:
        data.extend((d['dir'] | d['pos'] << 4, d['type']))
    # 0xFFFF = end-of-layer sentinel
    data.extend([0xff, 0xff])
    return door_offset

  # Build the 14-byte room header for a dungeon room. Headers are shared/deduplicated
  # via append_scan_bytes to save space when rooms share identical configurations.
  # Parameters:
  #   y: parsed YAML dict for one dungeon room
  # Returns: list of 14 integers representing the packed room header
  def get_room_header(y):
    h = y['Header']
    # Byte 7: pack all four stair/hole destination floor flags into one byte (2 bits each)
    p7 = h['hole0_dest'][1] | h['stair0_dest'][1] << 2 | h['stair1_dest'][1] << 4 | h['stair2_dest'][1] << 6
    p8 = h['stair3_dest'][1]
    # Byte 0: bg2 mode (3 bits), collision type (3 bits), lights-out flag (2 bits)
    return [tables.kBg2_rev[h['bg2']] << 5 | tables.kCollisionNames_rev[h['collision']] << 2 | h['lights_out'],
            h['palette'],
            h['blockset'],
            h['enemyblk'],
            tables.kEffectNames_rev[h['effect']],
            tables.kTagNames_rev[h['tag0']],
            tables.kTagNames_rev[h['tag1']],
            p7,
            p8,
            # Bytes 9-13: destination room IDs for the hole and four stairways
            h['hole0_dest'][0],
            h['stair0_dest'][0],
            h['stair1_dest'][0],
            h['stair2_dest'][0],
            h['stair3_dest'][0]]

  # Accumulators for the main room compilation pass
  data = []
  offsets = [0]*320
  door_offsets = [0]*320
  room_headers = []
  header_offsets = [0] * 320
  chests = []
  # 133 dungeon entrances and 7 starting points must each be defined exactly once
  entrances = [None] * 133
  starting_points = [None] * 7
  sign_texts = [0] * 320
  pits_hurt_player = []

  # Main room compilation loop — process all 320 dungeon rooms
  for i in range(320):
    y = load_dungeon_yaml(i)
    h = y['Header']
    # Track which rooms have damage-dealing pits (used for pit collision behavior)
    if h['pits_hurt_player']: pits_hurt_player.append(i)
    offsets[i]=len(data)
    # First 2 bytes per room: floor tileset pair (4 bits each) and layout/quadrant info
    data.append(h['floor1'] + h['floor2'] * 16)
    data.append(h['layout'] * 4 + h['start_quadrant'])
    # Encode all three tile layers — Layer3 always includes door data (empty list if none)
    print_layer(y['Layer1'], y.get('Layer1.doors'))
    print_layer(y['Layer2'], y.get('Layer2.doors'))
    door_offsets[i] = print_layer(y['Layer3'], y.get('Layer3.doors') or [])
    # Deduplicate room headers by finding overlapping suffixes in the header buffer
    header_offsets[i] = append_scan_bytes(room_headers, get_room_header(y))
    sign_texts[i] = h['tele_msg']
    # Compile chest contents — each chest is 3 bytes: room (16-bit) + item code
    for a in y['Chests']:
      if isinstance(a, int):
        # Normal chest: room index in low 15 bits, item code
        chests.extend([i & 0xff, i >> 8, a])
      else:
        # Big key chest: marked with '!' suffix, bit 15 of room word set to flag it
        assert a.endswith('!')
        chests.extend([i & 0xff, (i >> 8) | 0x80, int(a[:-1])])
    # Collect entrance definitions — each entrance must map to exactly one room
    for e in y['Entrances']:
      a = e['entrance_index']
      e['room'] = i
      assert entrances[a] == None
      entrances[a] = e
    # Starting points are save/continue spawn locations (e.g., Link's house, sanctuary)
    if 'StartingPoints' in y:
      for e in y['StartingPoints']:
        a = e['starting_point_index']
        e['room'] = i
        assert starting_points[a] == None
        starting_points[a] = e
  # Verify completeness — every entrance and starting point must be defined by some room
  for i in range(133):
    if entrances[i] == None:
      raise Exception('Entrance %d not defined' % i)
  for i in range(7):
    if starting_points[i] == None:
      raise Exception('Starting point %d not defined' % i)

  # Compile all entrance or starting point data arrays for a set of entries.
  # Reused for both kEntranceData_ (133 entries) and kStartingPoint_ (7 entries).
  # Parameters:
  #   entrances: list of entrance/starting point dicts
  #   prefix: asset name prefix ('kEntranceData_' or 'kStartingPoint_')
  def print_entrance_info(entrances, prefix):
    # Compute the 8-byte relative scroll coordinate array for an entrance.
    # These tell the engine which screen quadrants to load around the spawn point.
    # The optional repair_scroll_bounds adjusts for rooms with non-standard layouts.
    def get_rc(a):
      rep, room, quads, xy = a.get('repair_scroll_bounds'), a['room'], a['quadrants'], a['player_xy']
      if rep == None: rep = (0, 0, 0, 0, 0, 0, 0, 0)
      # Room grid position: column from low nibble, row from high nibble
      base_x = (room & 0xf) * 2
      base_y = (room >> 4) * 2
      # Detect if player spawns in the lower/right half of a screen (bit 8 of coordinate)
      ym = (xy[1] & 0x100) >> 8
      xm = (xy[0] & 0x100) >> 8
      # Special handling for rooms >= 242 with single-width X scrolling
      qqq = xm if room >= 242 and quads[0] == 'single_x' else 0
      # Build 8 scroll boundary values: 4 for Y axis, 4 for X axis
      l = [base_y + ym, base_y, base_y + ym, base_y + 1,
           base_x + xm, base_x + qqq, base_x + xm, base_x + qqq + 1]
      # Apply repair offsets to handle rooms with irregular scroll regions
      return [a+b for a,b in zip(l, rep)]
    # Convert palace name to its index code (-1 = no palace, else (idx-1)*2)
    def get_palace(a):
      i = tables.kPalaceNames.index(a)
      return -1 if i == 0 else (i - 1) * 2
    # Encode quadrant scroll mode: single/double width and height packed into one byte
    def get_quadrant1(a):
      return ['single_x', 'double_x'].index(a[0]) * 0x20 + ['single_y', 'double_y'].index(a[1]) * 0x2
    # Encode which quadrant the player starts in (upper_left=0, lower_left=2, etc.)
    def get_quadrant2(a):
      kScrollNames = { 'upper_left' : 0, 'lower_left' : 2, 'upper_right' : 16, 'lower_right' : 18 }
      return kScrollNames[a[2]]
    # Encode the exit door type and position for the entrance's associated house exit.
    # Bit 15 distinguishes bombable (1) from wooden (0) doors.
    def get_exit_door(a):
      if a[0] == 'none': return 0
      if a[0] == 'none_0xffff': return 65535
      return ['wooden', 'bombable'].index(a[0]) << 15 | a[1] << 1 | a[2] << 7

    # Write each entrance property as a separate parallel array (structure-of-arrays layout)
    add_asset_uint16(prefix+'rooms', [a['room'] for a in entrances])
    add_asset_uint8(prefix+'relativeCoords', flatten([get_rc(a) for a in entrances]))
    # Absolute scroll/player/camera positions derived from room-relative coords + room origin
    def get_base_x(a): return ((a['room'] & 0x00f) << 9)
    def get_base_y(a): return ((a['room'] & 0x1f0) << 5)
    add_asset_uint16(prefix+'scrollX', [a['scroll_xy'][0] + get_base_x(a) for a in entrances])
    add_asset_uint16(prefix+'scrollY', [a['scroll_xy'][1] + get_base_y(a) for a in entrances])
    add_asset_uint16(prefix+'playerX', [a['player_xy'][0] + get_base_x(a) for a in entrances])
    add_asset_uint16(prefix+'playerY', [a['player_xy'][1] + get_base_y(a) for a in entrances])
    # Camera coordinates are already absolute (not room-relative)
    add_asset_uint16(prefix+'cameraX', [a['camera_xy'][0] for a in entrances])
    add_asset_uint16(prefix+'cameraY', [a['camera_xy'][1] for a in entrances])
    add_asset_uint8(prefix+'blockset', [a['blockset'] for a in entrances])
    add_asset_int8(prefix+'floor', [a['floor'] for a in entrances])
    add_asset_int8(prefix+'palace', [get_palace(a['palace']) for a in entrances])
    add_asset_uint8(prefix+'doorwayOrientation', [a['doorway_orientation'] for a in entrances])
    # Starting background: plane in low nibble, ladder level in high nibble
    add_asset_uint8(prefix+'startingBg', [a['plane'] + a['ladder_level'] * 16 for a in entrances])
    add_asset_uint8(prefix+'quadrant1', [get_quadrant1(a['quadrants']) for a in entrances])
    add_asset_uint8(prefix+'quadrant2', [get_quadrant2(a['quadrants']) for a in entrances])
    add_asset_uint16(prefix+'doorSettings', [get_exit_door(a['house_exit_door']) for a in entrances])
    # Starting points have an extra field linking back to their associated entrance
    if prefix == 'kStartingPoint_':
      add_asset_uint8(prefix+'entrance', [a['associated_entrance_index'] for a in entrances])
    m = invert_dict(tables.kMusicNames)
    add_asset_uint8(prefix+'musicTrack', [m[a['music']] for a in entrances])
    

  # Write all compiled room data to the asset dictionary
  add_asset_uint8('kDungeonRoom', data)
  add_asset_uint16('kDungeonRoomOffs', offsets)
  add_asset_uint16('kDungeonRoomDoorOffs', door_offsets)
  # Deduplicated header buffer and per-room offsets into it
  add_asset_uint8('kDungeonRoomHeaders', room_headers)
  add_asset_uint16('kDungeonRoomHeadersOffs', header_offsets)
  add_asset_uint8('kDungeonRoomChests', chests)
  add_asset_uint16('kDungeonRoomTeleMsg', sign_texts)
  add_asset_uint16('kDungeonPitsHurtPlayer', pits_hurt_player)

  # Compile entrance and starting point arrays using the shared helper
  print_entrance_info(entrances, 'kEntranceData_')
  print_entrance_info(starting_points, 'kStartingPoint_')

  # Compile 8 default room templates — used as base layouts that rooms can inherit from
  data = []
  offsets = [0] * 8
  default_yaml = yaml.safe_load(open('dungeon/default_rooms.yaml', 'r'))
  for i in range(len(offsets)):
    offsets[i] = len(data)
    print_layer(default_yaml['Default%d' % i], None)
  add_asset_uint8('kDungeonRoomDefault', data)
  add_asset_uint16('kDungeonRoomDefaultOffs', offsets)

  # Compile 19 overlay templates — additional tile layers drawn on top of base rooms
  data = []
  offsets = [0] * 19
  overlay_yaml = yaml.safe_load(open('dungeon/overlay_rooms.yaml', 'r'))
  for i in range(len(offsets)):
    offsets[i] = len(data)
    print_layer(overlay_yaml['Overlay%d' % i], None)
  add_asset_uint8('kDungeonRoomOverlay', data)
  add_asset_uint16('kDungeonRoomOverlayOffs', offsets)

  print_dungeon_secrets()

  # Dungeon tile attribute tables — map tile IDs to collision/interaction properties
  add_asset_uint16('kDungAttrsForTile_Offs', ROM.get_words(0x8e9000, 21))
  add_asset_uint8('kDungAttrsForTile', ROM.get_bytes(0x8e902a, 1024))

  # Movable block and torch initial position tables — read directly from ROM
  add_asset_uint16('kMovableBlockDataInit', ROM.get_words(0x84f1de, 198))
  add_asset_uint16('kTorchDataInit', ROM.get_words(0x84F36A, 144))
  add_asset_uint16('kTorchDataJunk', ROM.get_words(0x84F48a, 48))
 
# Decompress and pack the enemy damage table from ROM.
# This table defines how much damage each weapon type deals to each enemy type.
# Parameters: none
# Returns: nothing (writes kEnemyDamageData to asset dict)
def print_enemy_damage_data():
  decomp, comp_len = util.decomp(0x83e800, ROM.get_byte, True, True)
  add_asset_uint8('kEnemyDamageData', decomp)

# Decode and pack background tilemaps used for title screen, file select, etc.
# Each tilemap uses a DMA-style transfer format with memset and memcpy commands.
# Parameters: none
# Returns: nothing (writes kBgTilemap_0 through kBgTilemap_5 to asset dict)
def print_tilemaps():
  # ROM addresses of the 6 background tilemap data blocks
  kSrcs = [0xcdd6d, 0xce7bf, 0xce2a8, 0xce63c, 0xce456, 0xeda9c]
  # Walk a tilemap's DMA command stream to determine its total byte length.
  # Each command has a 4-byte header; bit 6 of byte 2 selects memset (2 bytes)
  # vs memcpy (variable length). Bit 7 of byte 0 signals end of stream.
  def decode_one(p):
    p_org = p
    while not (ROM.get_byte(p) & 0x80):
      is_memset = ROM.get_byte(p+2) & 0x40
      # Transfer length is stored in the low 14 bits of bytes 2-3, plus 1
      len = ((ROM.get_byte(p+2)*256+ROM.get_byte(p+3))&0x3fff) + 1
      p += 4
      # Memset commands carry 2 bytes of fill data; memcpy carries 'len' source bytes
      p += 2 if is_memset else len
    # Include the terminator byte in the total length
    return p - p_org + 1
  for i,s in enumerate(kSrcs):
    l = decode_one(s)
    add_asset_uint8('kBgTilemap_%d' % i, ROM.get_bytes(s, l))

# Encode Link's sprite sheet from a PNG image into SNES 4bpp planar tile format.
# The PNG uses indexed color (palette indices 0-15); this converts each 8x8 tile
# into 32 bytes of bitplane-interleaved data matching SNES VRAM layout.
# Parameters: none
# Returns: nothing (writes kLinkGraphics to asset dict)
def print_link_graphics():
  image = Image.open('linksprite.png')
  data = image.tobytes()
  # Convert one 8x8 tile from linear indexed pixels to SNES 4bpp planar format.
  # SNES 4bpp stores 4 bitplanes: planes 0-1 interleaved in bytes 0-15,
  # planes 2-3 interleaved in bytes 16-31. Each bitplane row is one byte (8 pixels).
  # Parameters:
  #   data: raw pixel byte array from the PNG
  #   offset: byte offset of the tile's top-left pixel
  #   pitch: number of bytes per row in the source image
  # Returns: 32 bytes in SNES 4bpp planar format
  def encode_4bit_sprite(data, offset, pitch):
    b = [0] * 32
    for y in range(8):
      for x in range(8):
        v = data[offset + y * pitch + x]
        # Distribute each pixel's 4-bit value across 4 bitplanes
        b[y*2+0] |= (v & 1) << (7-x)
        b[y*2+1] |= (v >> 1 & 1) << (7-x)
        b[y*2+16] |= (v >> 2 & 1) << (7-x)
        b[y*2+17] |= (v >> 3 & 1) << (7-x)
    return bytes(b)
  # Process the full sprite sheet: 56 rows x 16 columns of 8x8 tiles (896 tiles total)
  b = b''
  for y in range(56):
    for x in range(16):
      b += encode_4bit_sprite(data, y * 128 * 8 + x * 8, 128)
  add_asset_uint8('kLinkGraphics', b)

# Compile music data for the three SPC700 sound banks (intro, indoor, ending).
# Delegates to compile_music.print_song() which handles the SPC700 instruction encoding.
# Parameters: none
# Returns: nothing (writes one asset per song bank to the asset dict)
def print_sound_banks():
  for song in ['intro', 'indoor', 'ending']:
    name, data = compile_music.print_song(song)
    add_asset_uint8(name, data)

# Master orchestrator — invokes every compilation function in the correct order.
# The order matters because some functions depend on ROM being loaded and certain
# global state being initialized by earlier functions.
# Parameters:
#   args: command-line arguments (sprites_from_png, languages, print_assets_header)
# Returns: nothing
def print_all(args):
  print_sound_banks()
  print_dungeon_rooms()
  print_enemy_damage_data()
  print_link_graphics()
  print_dungeon_sprites()
  print_map32_to_map16()
  print_images(args)
  print_misc(args)
  print_dialogue(args)
  print_dungeon_map()
  print_tilemaps()
  print_overworld()
  print_overworld_tables()

# Pack all compiled assets into the final zelda3_assets.dat binary file.
# File format: 48-byte header (16-byte signature + 32-byte SHA256 of key names),
# then uint32 sizes array, then null-terminated key name strings,
# then each asset's data block padded to 4-byte alignment.
# Optionally prints a C header file with #define macros for asset access.
# Parameters:
#   print_header: if True, also emit a C header to stdout with asset accessor macros
# Returns: nothing (writes ../zelda3_assets.dat)
def write_assets_to_file(print_header = False):
  key_sig = b''
  all_data = []
  # When generating the C header, emit the enum and extern declarations
  if print_header:
    print('''#pragma once
#include "types.h"

enum {
  kNumberOfAssets = %d
};
extern const uint8 *g_asset_ptrs[kNumberOfAssets];
extern uint32 g_asset_sizes[kNumberOfAssets];
extern MemBlk FindInAssetArray(int asset, int idx);
''' % len(assets))

  # Build the key signature string and collect data blobs for each asset
  for i, (k, (tp, data)) in enumerate(assets.items()):
    if print_header:
      # Packed assets use a function-style macro (index lookup at runtime)
      if tp == 'packed':
        print('#define %s(idx) FindInAssetArray(%d, idx)' % (k, i))
      else:
        # Typed assets use a cast macro for direct pointer access
        print('#define %s ((%s*)g_asset_ptrs[%d])' % (k, tp, i))
        print('#define %s_SIZE (g_asset_sizes[%d])' % (k, i))
    # Build null-terminated key name list for signature hashing
    key_sig += k.encode('utf8') + b'\0'
    all_data.append(data)

  # 48-byte header: 16-byte magic string + SHA256 hash of all key names.
  # The hash allows the engine to verify it was built against the same asset layout.
  assets_sig = b'Zelda3_v0     \n\0' + hashlib.sha256(key_sig).digest()

  if print_header:
    print('#define kAssets_Sig %s' % ", ".join((str(a) for a in assets_sig)))

  # After the signature: 32 zero bytes reserved, then asset count and key string length
  hdr = assets_sig + b'\x00' * 32 + struct.pack('II', len(all_data), len(key_sig))

  # Per-asset size table (uint32 each) so the engine knows each blob's length
  encoded_sizes = array.array('I', [len(i) for i in all_data])

  file_data = hdr + encoded_sizes + key_sig

  # Append each asset's data with 4-byte alignment padding between blocks
  for v in all_data:
    while len(file_data) & 3:
      file_data += b'\0'
    file_data += v

  open('../zelda3_assets.dat', 'wb').write(file_data)

# Entry point — run full asset compilation then write the output file.
# Parameters:
#   args: object with sprites_from_png, languages, and print_assets_header attributes
# Returns: nothing
def main(args):
  print_all(args)
  write_assets_to_file(args.print_assets_header)

# When run directly as a script, load the ROM from argv and use default compilation settings.
# When imported as a module (by restool.py), use the already-loaded ROM from util.
if __name__ == "__main__":
  ROM = util.load_rom(sys.argv[1] if len(sys.argv) >= 2 else None)
  # Fallback argument object for standalone execution with no CLI argument parsing
  class DefaultArgs:
    sprites_from_png = False
    languages = None
    print_assets_header = False
  main(DefaultArgs())
else:
  # Module import path — restool.py loads the ROM before importing this module
  ROM = util.ROM

