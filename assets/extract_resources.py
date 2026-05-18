# =============================================================================
# extract_resources.py — Master ROM Extraction Script
#
# This is one half of the zelda3 asset pipeline. It reads the original SNES ROM
# binary for The Legend of Zelda: A Link to the Past and extracts ALL game data
# into editable intermediate formats: YAML files for overworld areas and dungeon
# rooms, TXT for tile mappings, PNG for sprite sheets, and sound data.
#
# The other half is compile_resources.py, which reassembles these intermediate
# files back into C source data the engine can consume.
#
# SNES address convention used throughout: addresses like 0x82DD8A use LoROM
# bank mapping (bank byte 0x82, offset 0xDD8A). The util module's ROM read
# functions (get_byte, get_word, etc.) handle the bank-to-file-offset mapping
# transparently, so callers just pass the SNES logical address.
#
# Output directory structure created by this script:
#   overworld/overworld-N.yaml  — one file per overworld area (160 areas)
#   dungeon/dungeon-N.yaml      — one file per dungeon room (320 rooms)
#   dungeon/default_rooms.yaml  — 8 default room object templates
#   dungeon/overlay_rooms.yaml  — 19 overlay room object templates
#   map32_to_map16.txt          — overworld 32x32-to-16x16 tile mapping table
#   sound/                      — extracted music and SFX data
#   img/                        — extracted sprite sheet PNGs
# =============================================================================

# Standard library imports
from ast import literal_eval as make_tuple
import sys
import os

# Project-local modules for ROM data extraction
# text_compression: handles SNES Huffman-compressed dialogue text
import text_compression
# util: provides ROM loading and byte/word/int read helpers with LoROM address mapping
import util
from util import get_bytes, get_words, get_byte, get_word, get_int8, get_int16, cache
# tables: contains all name-lookup tables (sprite names, area names, item names, etc.)
import tables
# yaml: used to serialize extracted data structures into human-readable YAML files
import yaml
# extract_music: handles extraction of SPC700 music and sound effect data from the ROM
import extract_music
# sprite_sheets: decodes and exports Link's sprites, enemy sheets, HUD icons, and fonts as PNGs
import sprite_sheets

# Extracts the overworld map32-to-map16 tile mapping table and writes it as text.
# The SNES overworld is built from 32x32 pixel metatiles, each composed of four 16x16 tiles.
# This function reads 2218 metatile definitions from 4 ROM banks (0x83, 0x84) and writes
# a text file with one line per 16x16 tile index. Each metatile yields 4 rows of output,
# with 4 columns representing the 4 ROM banks (different tile layers/variants).
# Parameters:
#   f — writable file object for the output text (map32_to_map16.txt)
def print_map32_to_map16(f):
  # 2218 metatile entries in the ROM, each stored as 6 bytes per bank
  for i in range(2218):
    # Decodes one 6-byte metatile record into four 16x16 tile indices.
    # Bytes 0-3 hold the low 8 bits of each of the 4 tile indices.
    # Bytes 4-5 hold the high 4 bits packed as nibbles (upper/lower) for tiles 0-3.
    def getit(ea):
      ov = [get_byte(ea + j) for j in range(6)]
      res=[0]*4
      # Combine low byte with high nibble from byte 4 or 5 to form 12-bit tile index
      res[0] = ov[0] | (ov[4] >> 4) << 8
      res[1] = ov[1] | (ov[4] & 0xf) << 8
      res[2] = ov[2] | (ov[5] >> 4) << 8
      res[3] = ov[3] | (ov[5] & 0xf) << 8
      return res
    # Read metatile i from each of the 4 ROM banks at their respective base addresses
    # Each bank starts at a different ROM offset; stride is 6 bytes per metatile
    t0 = getit(0x838000 + i * 6)
    t1 = getit(0x83b400 + i * 6)
    t2 = getit(0x848000 + i * 6)
    t3 = getit(0x84b400 + i * 6)
    # Output 4 lines per metatile (one per quadrant tile), columns are bank0..bank3
    for j in range(4):
      print('%5d: %4d, %4d, %4d, %4d' % (i * 4 + j, t0[j], t1[j], t2[j], t3[j]), file = f)

# Extracts all 79 overworld exit definitions from the ROM. An "exit" is a screen
# transition triggered when Link walks to the edge of an overworld area or enters
# a door/cave. Each exit stores: destination room, player position, scroll offsets,
# camera position, and optional door type (wooden, bombable, palace, sanctuary).
# Results are cached and returned as a dict keyed by screen_index, where each value
# is a list of exit records for that screen. Coordinates are stored relative to the
# screen's top-left corner (absolute ROM values minus base_x/base_y).
# Returns: dict {screen_index: [exit_dict, ...]}
@cache
def get_exit_datas():
  r = {}
  for i in range(79):
    # Each exit field is stored in its own parallel array in ROM bank 0x82.
    # The arrays are indexed by exit number (0-78), with 2-byte or 1-byte stride.
    room = get_word(0x82dd8a + i * 2)
    screen_index = get_byte(0x82DE28 + i)
    load_offs = get_word(0x82DE77 + i * 2)
    scroll_y = get_word(0x82DF15 + i * 2)
    scroll_x = get_word(0x82DFB3 + i * 2)
    pos_y = get_word(0x82E051 + i * 2)
    pos_x = get_word(0x82E0EF + i * 2)
    camera_y = get_word(0x82E18D + i * 2)
    camera_x = get_word(0x82E22B + i * 2)
    unk1 = get_int8(0x82E2C9 + i)
    unk3 = get_int8(0x82E318 + i)
    ndoor = get_word(0x82E367 + i * 2)
    fdoor = get_word(0x82E405 + i * 2)
    # Convert screen_index to pixel-space origin for this screen.
    # The overworld is an 8-wide grid of 512-pixel screens; bits 0-2 give X, bits 3-5 give Y.
    base_x = (screen_index & 7) << 9
    base_y = (screen_index & 56) << 6
    # Store all positions relative to screen origin for portability
    y = {
      'index' : i,
      'room' : room,
      'xy' : [pos_x - base_x, pos_y - base_y],
      'scroll_xy' : [scroll_x - base_x, scroll_y - base_y],
      'camera_xy' : [camera_x - base_x, camera_y - base_y],
    }
    # Decode load_offs back to a tile-grid-relative [x, y] offset from the scroll position
    y['load_xy'] = [((load_offs >> 1) - (y['scroll_xy'][0] >> 4)) & 0x3f, (load_offs >> 7) - (y['scroll_xy'][1] >> 4) & 0x3f]
    y['unk'] = [unk1, unk3]
    # Reads additional exit metadata for "special" room indices 0x180-0x18F.
    # These rooms have custom scroll boundaries, graphics overrides, and palette settings
    # stored in separate ROM tables offset from the room index minus 0x180.
    def get_special_exit_info(room_index):
      return {
        'dir' : get_byte(0x82E801  + room_index - 0x180) >> 1,
        'spr_gfx' : get_byte(0x82E811  + room_index - 0x180),
        'aux_gfx' : get_byte(0x82E821  + room_index - 0x180),
        'pal_bg' : get_byte(0x82E831  + room_index - 0x180),
        'pal_spr' : get_byte(0x82E841  + room_index - 0x180),
        'top' : get_word(0x82e6e1 + (room_index - 0x180) * 2),
        'bottom' : get_word(0x82e701 + (room_index - 0x180) * 2),
        'left' : get_word(0x82e721 + (room_index - 0x180) * 2),
        'right' : get_word(0x82e741 + (room_index - 0x180) * 2),
        'left_edge_of_map' : get_word(0x82E7E1 + (room_index - 0x180) * 2),
        'unk4' : get_int16(0x82e761 + (room_index - 0x180) * 2),
        'unk6' : get_int16(0x82e781 + (room_index - 0x180) * 2),
        'unk5' : get_int16(0x82e7a1 + (room_index - 0x180) * 2),
        'unk7' : get_int16(0x82e7c1 + (room_index - 0x180) * 2),
      }

    # Rooms 0x180-0x18F are "special exit" rooms with extra scroll/gfx overrides
    if room >= 0x180 and room < 0x190:
      y['special_exit'] = get_special_exit_info(room)

    # Decode door info if present. ndoor and fdoor are mutually exclusive.
    # Bit 15 determines door variant; bits 1-6 and 7-13 encode x/y tile position.
    if ndoor != 0:
      assert fdoor == 0
      y['door'] = ['bombable' if ndoor & 0x8000 else 'wooden', (ndoor & 0x7e) >> 1, (ndoor & 0x3f80) >> 7]
    if fdoor != 0:
      y['door'] = ['palace' if fdoor & 0x8000 else 'sanctuary', (fdoor & 0x7e) >> 1, (fdoor & 0x3f80) >> 7]
    # Group exits by the screen they belong to
    r.setdefault(screen_index, []).append(y)
  return r

# Recomputes a load_offs value from scroll coordinates and a tile-grid delta.
# This is the inverse of the load_xy decoding in get_exit_datas/get_ow_travel_infos,
# used to verify round-trip correctness of the extracted data.
# Parameters:
#   c — [scroll_x, scroll_y] in pixel coordinates
#   d — [delta_x, delta_y] tile-grid offset relative to scroll position
# Returns: 16-bit load_offs value matching the ROM's packed format
def get_loadoffs(c, d):
  # Convert scroll pixels to tile units (divide by 16), then add the delta
  x, y = c[0] >> 4, c[1] >> 4
  x += d[0]
  y += d[1]
  # Pack into the ROM's load_offs format: y in bits 7-12, x in bits 1-6
  return (y&0x3f) << 7 | (x&0x3f) << 1

# Extracts the 17 bird/whirlpool fast-travel destination records from the ROM.
# Entries 0-8 are bird (flute) travel points; entries 9-16 are whirlpool warp destinations.
# Each record contains the destination screen, player position, scroll, and camera offsets,
# similar in structure to overworld exits but stored in a separate ROM table range.
# Results cached and returned as dict {screen_index: [travel_dict, ...]}.
@cache
def get_ow_travel_infos():
  r = {}
  for i in range(17):
    # Parallel arrays in ROM bank 0x82 starting at 0xEAE5, same layout as exit data
    screen_index = get_word(0x82EAE5 + i * 2)
    load_offs = get_word(0x82EB07 + i * 2)
    scroll_y = get_word(0x82EB29 + i * 2)
    scroll_x = get_word(0x82EB4B + i * 2)
    pos_y = get_word(0x82EB6D + i * 2)
    pos_x = get_word(0x82EB8F + i * 2)
    camera_y = get_word(0x82EBB1 + i * 2)
    camera_x = get_word(0x82EBD3 + i * 2)
    unk1 = get_int8(0x82EBF5 + i * 2)
    unk3 = get_int8(0x82EC17 + i * 2)
    base_x = (screen_index & 7) << 9
    base_y = (screen_index & 56) << 6
    y = {}
    # First 9 entries are bird (flute) travel destinations
    if i < 9:
      y['bird_travel_id'] = i
    # Entries 9-16 are whirlpool warps; each has a source area stored separately
    else:
      y['whirlpool_src_area'] = get_word(0x82ECF8 + (i - 9) * 2)
    y['xy'] = [pos_x - base_x, pos_y - base_y]
    y['scroll_xy'] = [scroll_x - base_x, scroll_y - base_y]
    y['camera_xy'] = [camera_x - base_x, camera_y - base_y]
    y['load_xy'] = [((load_offs >> 1) - (y['scroll_xy'][0] >> 4)) & 0x3f, (load_offs >> 7) - (y['scroll_xy'][1] >> 4) & 0x3f]

    # Verify round-trip: re-encode load_xy back to load_offs to ensure extraction is correct
    t0 = get_loadoffs(y['scroll_xy'], y['load_xy'])
    assert t0 == load_offs, (t0 & 0x7f, load_offs & 0x7f)

    y['unk'] = [unk1, unk3]
    r.setdefault(screen_index, []).append(y)
  return r

# Extracts 129 overworld-to-dungeon entrance mappings from ROM bank 0x9B.
# Each entrance records which overworld area it sits on, its tile-grid position (x, y),
# and the entrance_id that indexes into the full entrance data table (decoded by
# _get_entrance_info_one) to determine the dungeon room, scroll bounds, music, etc.
# The position is packed as a 16-bit value: bits 1-6 = x tile, bits 7-12 = y tile.
# Returns: dict {area_index: [entrance_dict, ...]}
@cache
def get_ow_entrance_info():
  r = {}
  for i in range(129):
    area = get_word(0x9BB96F + i * 2)
    pos = get_word(0x9BBA71 + i * 2)
    entrance_id = get_byte(0x9BBB73 + i)
    # Decode packed position: divide by 2 to remove low bit, then mask for 6-bit x and y
    r.setdefault(area, []).append({'index' : i, 'x' : (pos >> 1) & 0x3f, 'y' : (pos >> 7) & 0x3f, 'entrance_id' : entrance_id})
  return r

# Extracts 19 pit/hole warp locations from ROM bank 0x9B. These are overworld tiles
# where Link falls through a hole into an underground area (e.g., the various pits
# in the Light/Dark World). Each hole stores its overworld area, tile position, and
# the entrance_id for the underground destination. The raw position has 0x400 added
# to account for an offset in the SNES tilemap addressing.
# Returns: dict {area_index: [hole_dict, ...]}
@cache
def get_hole_infos():
  r = {}
  for i in range(19):
    # Add 0x400 offset to align the position with the overworld tilemap coordinate space
    pos = get_word(0x9BB800 + i * 2) + 0x400
    area = get_word(0x9BB826 + i * 2)
    entrance_id = get_byte(0x9BB84C + i)
    r.setdefault(area, []).append({'x' : (pos >> 1) & 0x3f, 'y' : (pos >> 7) & 0x3f, 'entrance_id' : entrance_id})
  return r

# Combines all overworld data for a single area into a YAML file.
# Gathers: header metadata (name, size, gfx set, palette, sign text, music/ambient),
# travel points, entrances, holes, exits, items/secrets, and sprites for each game phase.
# Light World areas (0-63) have 3 sprite phases (beginning, after Zelda rescue, after
# Master Sword); Dark World areas (64-143) have a single sprite set.
# Special areas (144-159) are used internally and have minimal data.
# Writes output to: overworld/overworld-N.yaml
# Parameters:
#   overworld_area — area index (0-159)
def print_overworld_area(overworld_area):
  # 192-byte table indicating whether each area uses small (1-screen) or big (4-screen) layout
  is_small = get_bytes(0x82F88D, 192)
  y = {}

  # Returns the music or ambient sound name for this area across game progression phases.
  # The ROM stores music data as a nibble-packed byte: high nibble = ambient, low nibble = music.
  # Light World areas (0-63) have 4 progression phases; Dark World areas use a single phase.
  # Parameters:
  #   ambient — if True, extract high nibble (ambient sound); if False, low nibble (music track)
  def get_music(ambient):
    def fn(x):
      if ambient:
        return tables.kAmbientSoundName[x >> 4]
      else:
        return tables.kMusicNames[x & 0xf]
    # Light World: 4 phases stored at successive 64-byte offsets from base 0x82C303
    if overworld_area < 64:
      return {
        'beginning' : fn(get_byte(0x82C303 + overworld_area)),
        'zelda' : fn(get_byte(0x82C303 + overworld_area + 64)),
        'sword' : fn(get_byte(0x82C303 + overworld_area + 128)),
        'agahnim' : fn(get_byte(0x82C303 + overworld_area + 192)),
      }
    # Dark World / special: only one phase, stored in a separate table
    else:
      assert overworld_area < 64 + 96
      return {'agahnim' : fn(get_byte(0x82C403 + overworld_area - 64)) }

  # Reads the list of hidden items/secrets for this overworld area.
  # Items are stored as a variable-length list in ROM bank 0x9B, terminated by 0xFFFF.
  # Each entry is 3 bytes: 2-byte packed tile position + 1-byte item type.
  def get_items():
    # Special areas (128+) have no items
    if overworld_area >= 128: return []
    # Indirect pointer: look up the area's item list address from a pointer table
    ea = 0x9b0000 | get_word(0x9BC2F9 + overworld_area * 2)
    xs = []
    # Read items until the 0xFFFF terminator
    while get_word(ea) != 0xffff:
      pos = get_word(ea)
      assert pos%2==0
      # Decode packed position: pos/2 gives tile index, mod 64 = x, div 64 = y
      x, y = pos//2%64, pos//2//64
      xs.append([x, y, tables.kSecretNames[get_byte(ea+2)]])
      ea += 3
    return xs

  header = {
    'name' : tables.kAreaNames[overworld_area],
    'size' : 'small' if is_small[overworld_area] else 'big',
    'gfx' : get_byte(0x80FC9C + overworld_area) if overworld_area < 128 else -1,
    'palette' : get_byte(0x80FD1C + overworld_area) if overworld_area < 136 else -1,
    'sign_text' : get_word(0x87F51D + overworld_area * 2) if overworld_area < 128 else -1,
    'music' : get_music(False),
    'ambient' : get_music(True),
  }
      
  y['Header'] = header
  y['Travel'] = get_ow_travel_infos().get(overworld_area, [])
  y['Entrances'] = get_ow_entrance_info().get(overworld_area, [])
  hole_infos = get_hole_infos()
  if overworld_area in hole_infos:
    y['Holes'] = hole_infos[overworld_area]
  y['Exits'] = get_exit_datas().get(overworld_area, [])
  y['Items'] = get_items()
  
  def decode_sprites(base_addr):
    r = []
    ea = 0x890000 + get_word(base_addr + overworld_area * 2)
    while get_byte(ea) != 0xff:
      y, x, w = get_byte(ea), get_byte(ea+1), get_byte(ea+2)
      r.append([x, y, tables.kSpriteNames[w]])
      ea += 3
    return r

  def get_info(stage):
    if overworld_area >= 128: return {}
    if overworld_area >= 64: stage = 3
    return {
      'gfx' : get_byte(0x80FA41 + (overworld_area & 63) + stage * 64),
      'palette' : get_byte(0x80FB41 + (overworld_area & 63) + stage * 64),
    }
  if overworld_area < 64:
    y['Sprites.Beginning'] = {
      'info' : get_info(0),
      'sprites' : decode_sprites(0x89C881)
    }
    y['Sprites.FirstPart'] = {
      'info' : get_info(1),
      'sprites' : decode_sprites(0x89C901)
    }
    y['Sprites.SecondPart'] = {
      'info' : get_info(2),
      'sprites' : decode_sprites(0x89CA21)
    }
  elif overworld_area < 144:
    y['Sprites'] = {
      'info' : get_info(2),
      'sprites' : decode_sprites(0x89CA21)
    }
    
  s = yaml.dump(y, default_flow_style=None, sort_keys=False)
  open('overworld/overworld-%d.yaml' % overworld_area, 'w').write(s)

# Iterates all 160 overworld areas and writes each to a YAML file.
# Areas 0-127 are the main overworld; 128-159 are special world areas.
# Some large areas (2x2 screens) share a single area head index — only the
# head area (index == area_heads[i&63]) gets its own file to avoid duplicating
# the same data for each sub-screen of a big area.
# Parameters: none
# Returns: none (writes YAML files as side effect)
def print_all_overworld_areas():
  # Read the 64-entry table that maps each area to its "head" area index
  area_heads = get_bytes(0x82A5EC, 64)
  
  # Iterate all 160 areas; skip sub-areas of big areas in the 0-127 range
  # by checking whether this area index IS the head of its group
  for i in range(160):
    if i >= 128 or area_heads[i&63] == (i&63):
      print_overworld_area(i)

# Extracts all in-game dialogue strings from the ROM and writes them to a
# language-specific text file. Delegates to text_compression which handles
# the SNES Huffman decompression of the dialogue bank.
# Parameters: none
# Returns: none (writes dialogue text file as side effect)
def print_dialogue():
  text_compression.print_strings(util.ROM, file = open(text_compression.dialogue_filename(util.ROM.language), 'w', encoding='utf8'))

# Decodes dungeon room object data from ROM starting at address p.
# Room objects are stored as a stream of 3-byte records with three subtypes,
# terminated by either 0xFFF0 (layer separator — more data follows on the next
# layer) or 0xFFFF (end of room — no more layers). After the object stream,
# a door list follows (only when terminated by 0xFFF0).
#
# Object subtypes (determined by the first two bytes):
#   Type 0: standard tile objects (index < 0xF8) — have x,y position and w*h size
#   Type 1: extended tile objects (index >= 0xF8) — size bits encode a sub-index
#   Type 2: special objects (top 6 bits = 0xFC) — 6-bit x, y, index packed
#
# Parameters:
#   p - ROM address pointing to the start of the object data stream
# Returns:
#   tuple of (next_address, objects_list, doors_list_or_None)
#   doors_list is None when stream ended with 0xFFFF (no door data)
def decode_room_objects(p):
  objs = []
  j = 0
  # Read 3-byte object records until a terminator is encountered
  while True:
    p0, p1, p2 = get_byte(p), get_byte(p+1), get_byte(p+2)
    # Combine first two bytes into a 16-bit value for terminator checks
    A = p0 | p1 << 8
    # 0xFFFF = end-of-room marker — no door list follows this layer
    if A == 0xffff:
      return p + 2, objs, None
    # 0xFFF0 = layer separator — object stream ends but door list follows
    if A == 0xfff0:
      p += 2
      break
    # Check if this is a type 0 or type 1 object (top 6 bits != 0xFC)
    if (A & 0xfc) != 0xfc:
      index = p2
      # Reconstruct the tile destination address from bit-packed position fields
      # Bits are spread across p0 and p1 to encode x and y in a 64x64 tile grid
      Dst = (p1 >> 2) << 7
      Dst |= (p0 & 0xfc) >> 1
      # Extract 6-bit x and y tile coordinates from the destination value
      X = (Dst >> 1) & 0x3f
      Y = (Dst >> 7) & 0x3f
      # Width and height are stored in the lowest 2 bits of p0 and p1
      W = p0 & 3
      H = p1 & 3
      # Type 0: standard objects use the index byte directly as an object type
      if index < 0xf8:
        objs.append({'x' : X, 'y' : Y, 's' : '%d*%d' % (W, H), 'n': tables.kType0Names[index]})
      # Type 1: extended objects pack a sub-index into the size and index bits
      else:
        index2 = (index & 7) << 4 | H << 2 | W
        objs.append({'x' : X, 'y' : Y, 'n': tables.kType1Names[index2]})
    else:
      # subtype 2: 111111xx xxxxyyyy yyiiiiii
      X = ((p0 << 4 | p1 >> 4) & 0x3f)
      Y = (p1 << 2 | p2 >> 6) & 0x3f
      index = p2 & 0x3f
      objs.append({'x' : X, 'y' : Y, 'n' : tables.kType2Names[index]})
    p += 3
    j += 1

  # Door list follows a 0xFFF0-terminated object stream.
  # Each door is 2 bytes: high byte = door type, low byte encodes position
  # (top 4 bits) and direction (bottom 2 bits). Terminated by 0xFFFF.
  doors = []
  while True:
    A = get_byte(p) | get_byte(p+1) << 8
    # 0xFFFF marks the end of the door list
    if A == 0xffff:
      return p + 2, objs, doors
    doors.append({'type':get_byte(p + 1), 'pos' : get_byte(p) >> 4, 'dir' : A & 3})
    p += 2

# Reads the chest placement table from ROM and returns a dict mapping room
# index to a list of (item_data, is_big_chest) tuples. The ROM stores 168
# chest entries (504 bytes / 3 bytes each) in a flat table at 0x81E96E.
# Results are cached because multiple rooms reference the same global table.
# Parameters: none
# Returns: dict {room_index: [(item_data, is_big_chest), ...]}
@cache
def get_chest_info():
  # ROM address of the chest placement table
  ea = 0x81e96e
  all = {}
  # Each entry is 3 bytes: 2-byte room word + 1-byte item data
  for i in range(504//3):
    room = get_word(ea + i * 3)
    data = get_byte(ea + i * 3 + 2)
    # Bit 15 of the room word flags whether this is a big chest (key item)
    all.setdefault(room & 0x7fff, []).append((data, (room & 0x8000) != 0))
  return all

# Reads a single dungeon entrance (set=0) or starting point (set=1) from
# the ROM's parallel entrance data tables. Each entrance's properties are
# spread across ~15 separate ROM tables, all indexed by the entrance ID.
# The two table sets (entrances vs starting points) live at different ROM
# addresses, selected by the `set` parameter via tuple indexing.
#
# Parameters:
#   i   - entrance/starting-point index (0-132 for entrances, 0-6 for starts)
#   set - 0 for dungeon entrances, 1 for starting points
# Returns:
#   tuple of (room_index, properties_dict) with all entrance attributes
def _get_entrance_info_one(i, set):
  # Reads the exit door data for this entrance. The door word encodes
  # whether the exit is bombable (bit 15), plus its position and direction.
  def get_exit_door(i):
    x = get_word((0x82D724, 0x82DC32)[set] + i * 2)
    if x == 0:
      return ['none']
    if x == 0xffff:
      return ['none_0xffff']
    # Bit 15 distinguishes bombable vs wooden doors; bits 1-6 = pos, bits 7-13 = dir
    return ['bombable' if x & 0x8000 else 'wooden', (x & 0x7e) >> 1, (x & 0x3f80) >> 7]
  # Map quadrant byte values to human-readable names for the YAML output
  kQuadrantNames = { 0 : 'upper_left', 2 : 'lower_left', 16 : 'upper_right', 18 : 'lower_right' }
  # Read which room this entrance leads to from the room table
  room = get_word((0x82C813, 0x82DB6E )[set] + i * 2)
  # Computes scroll boundary repair data relative to the room's base tile
  # coordinates. The SNES engine stores absolute scroll bounds in ROM, but
  # the YAML format needs them relative to the room origin so they are
  # portable. This subtracts the room's base x/y from each of the 8 boundary
  # values (upper/lower/left/right hard and full limits).
  def get_se(se_base_addr, xy, quds):
    # Derive the room's base coordinates in the dungeon grid from the room index
    # Room index encodes x in low nibble and y in bits 4-8, scaled by 2 screens
    base_x = (room & 0xf) * 2
    base_y = (room >> 4) * 2
    # Determine if the player spawns in the second screen of a multi-screen room
    # by checking bit 8 of the player's absolute y and x coordinates
    ym = (xy[1] & 0x100) >> 8
    xm = (xy[0] & 0x100) >> 8
    #if room == 259: ym = 1 # possibly related to none_0xffff
    # Read 8 scroll boundary bytes and convert from absolute to room-relative.
    # hu/fu = hard/full upper bounds, hd/fd = hard/full lower bounds
    hu = get_byte(se_base_addr + i * 8 + 0) - base_y - ym
    fu = get_byte(se_base_addr + i * 8 + 1) - base_y
    hd = get_byte(se_base_addr + i * 8 + 2) - base_y - ym
    fd = get_byte(se_base_addr + i * 8 + 3) - base_y - 1
    #assert fu == 0 and fd == 0 and hd == 0 and fd == 0, (room, fu, fd, hd, fd)
    # Special fixup for rooms >= 242 with single-width x layout — adjusts the
    # full scroll boundary to account for an off-by-one in the ROM data
    qqq = xm if room >= 242 and quds[0] == 'single_x' else 0
    # hl/fl = hard/full left bounds, hr/fr = hard/full right bounds
    hl = get_byte(se_base_addr + i * 8 + 4) - base_x - xm
    fl = get_byte(se_base_addr + i * 8 + 5) - base_x - qqq
    hr = get_byte(se_base_addr + i * 8 + 6) - base_x - xm
    fr = get_byte(se_base_addr + i * 8 + 7) - base_x - 1 - qqq
    return [hu, fu, hd, fd, hl, fl, hr, fr]#

  # Read player spawn position and convert to room-relative coordinates by
  # subtracting the room's absolute pixel origin (room column * 512, room row * 512)
  player_x = get_word((0x82D063, 0x82DBDE)[set] + i * 2) - ((room & 0x00f) << 9)
  player_y = get_word((0x82CF59, 0x82DBD0)[set] + i * 2) - ((room & 0x1f0) << 5)

  # Build the entrance property dict from the ~15 parallel ROM tables.
  # Each property is read from its own ROM table at an address selected by
  # the set parameter (entrance vs starting point) and indexed by i.
  y = {
    ('entrance_index' if set == 0 else 'starting_point_index'): i,
    'name' : tables.kEntranceNames[i] if set == 0 else 'Starting Location %d' % i,
    # Initial camera scroll position, converted to room-relative pixels
    'scroll_xy' : [
      get_word((0x82CD45, 0x82DBB4)[set] + i * 2) - ((room & 0x00f) << 9),
      get_word((0x82CE4F, 0x82DBC2)[set] + i * 2) - ((room & 0x1f0) << 5),
    ],
    'player_xy' : [ player_x, player_y ],
    # Camera focus point — not room-relative, used for initial camera lock
    'camera_xy' : [
      get_word((0x82D277, 0x82DBFA)[set] + i * 2),
      get_word((0x82D16D, 0x82DBEC)[set] + i * 2),
    ],
    # Tile graphics set index for this entrance's room
    'blockset' : get_byte((0x82D381, 0x82DC08)[set] + i),
    # Background music track to play when entering
    'music' : tables.kMusicNames[get_byte((0x82D82E, 0x82DC4E)[set] + i)],
    # Palace group determines which dungeon map/compass/keys apply;
    # the +2 >>1 converts the ROM's signed offset to a 0-based palace index
    'palace' : tables.kPalaceNames[(get_int8((0x82D48B, 0x82DC16)[set] + i) + 2) >> 1],
    # Doorway scroll direction; only meaningful for entrances, not starting points
    'doorway_orientation' : get_int8(0x82D510 + i) if set == 0 else 0,
    # BG layer plane (low nibble) and ladder/floor level (high nibble)
    # are packed into the same byte in ROM
    'plane' : get_byte((0x82D595, 0x82DC1D)[set] + i) & 0xf,
    'ladder_level' : get_byte((0x82D595, 0x82DC1D)[set] + i) >> 4,
    # Room dimensions: bit 5 = double-width x, bit 1 = double-height y
    # Third entry identifies which quadrant the player spawns in
    'quadrants' : [
      'double_x' if (get_byte((0x82D61a, 0x82DC24)[set] + i) & 0x20) != 0 else 'single_x',
      'double_y' if (get_byte((0x82D61a, 0x82DC24)[set] + i) & 0x2) else 'single_y',
      kQuadrantNames[get_byte((0x82D69F, 0x82DC2B)[set] + i)]
      ],
    # Signed floor number (negative = basement levels)
    'floor' : get_int8((0x82D406, 0x82DC0F)[set] + i),
  }

  # Only include scroll boundary repair data when it differs from the default
  # (all zeros means no correction needed, so omit it to keep YAML clean)
  se = get_se((0x82C91D, 0x82DB7C)[set], y['player_xy'], y['quadrants'])
  if se != [0, 0, 0, 0, 0, 0, 0, 0]:
    y['repair_scroll_bounds'] = se
  y['house_exit_door'] = get_exit_door(i)

#  quadrant_byte = (2 if player_y & 0x100 else 0) + (16 if player_x & 0x100 else 0)
#  if get_byte((0x82D69F, 0x82DC2B)[set] + i) != quadrant_byte:
#    print('Room %d has incorrect quadrant byte' % room, y['quadrants'])
  # Starting points (set=1) have an associated entrance index that links
  # back to the full entrance record for shared properties
  if set:
    y['associated_entrance_index'] = get_word(0x82DC40 + i * 2)
  return room, y

# Reads all dungeon entrances (set=0, 133 entries) or starting points
# (set=1, 7 entries) and groups them by room index. Cached because both
# print_room and print_all_dungeon_rooms call this for every room.
# Parameters:
#   set - 0 for dungeon entrances, 1 for starting points
# Returns: dict {room_index: [entrance_properties_dict, ...]}
@cache
def get_entrance_info(set):
  r = {}
  for i in range(133 if set == 0 else 7):
    room, y = _get_entrance_info_one(i, set)
    # Multiple entrances can lead to the same room, so accumulate as a list
    r.setdefault(room, []).append(y)
  return r

# Reads the set of 57 room indices where falling into a pit damages the
# player instead of just warping them. Used by print_room to set the
# pits_hurt_player flag in each room's header.
# Parameters: none
# Returns: set of room indices where pits cause damage
@cache
def pits_hurt_player():
  return set(get_word(0x80990C + i * 2) for i in range(57))

# Extracts all data for a single dungeon room and returns it as a YAML string.
# Combines room header properties, sprite list, secret tiles, chest contents,
# entrance/starting-point associations, and 3 layers of decoded room objects.
#
# Parameters:
#   room_index - dungeon room number (0-319)
# Returns: YAML-formatted string containing the complete room definition
def print_room(room_index):
  # Look up the room's object data address from the 3-byte pointer table
  p = 0x1f8000 + room_index * 3
  room_addr = get_byte(p) | get_byte(p+1) << 8 | get_byte(p+2) << 16
  # Look up the room's header data address from a separate 2-byte pointer table
  p = 0x40000 | get_word(0x4f502 + room_index * 2)
  # Sentinel value 0x4FFEF means this room has no header — use a zeroed-out area
  if p == 0x4FFEF:
    p = 0x82EDC5 # just some place with zeros
  # First two bytes of the object data encode floor graphics and layout template
  floor, layout = get_byte(room_addr), get_byte(room_addr + 1)

  # Parse the room header flags byte and the two destination-packing bytes
  flags = get_byte(p + 0)
  p7, p8 = get_byte(p + 7), get_byte(p + 8)

  # Sprite data pointer: first byte before the sprite list is the sort setting
  ea = 0x890000 + get_word(0x89D62E + room_index * 2)
  sort_sprites_setting = get_byte(ea)

  # Assemble the room header dict from the parsed flag fields and ROM tables.
  # Destination bytes p7/p8 pack 2-bit plane values for up to 5 warp targets
  # (hole, stair0-3), paired with destination room indices at offsets p+9..p+13.
  header = {
    # Floor graphics: low nibble = floor 1 tileset, high nibble = floor 2 tileset
    'floor1': floor & 0xf,
    'floor2' : floor >> 4,
    # Layout template index (bits 2-7) and starting quadrant (bits 0-1)
    'layout': layout >> 2,
    'start_quadrant' : layout & 3,
    # BG2 mode (top 3 bits), collision type (middle 3 bits), dark room flag (bit 0)
    'bg2' : tables.kBg2[flags >> 5],
    'collision' : tables.kCollisionNames[flags >> 2 & 7],
    'lights_out' : flags & 1,
    'palette' : get_byte(p + 1),
    'blockset' : get_byte(p + 2),
    'enemyblk' : get_byte(p + 3),
    'effect' : tables.kEffectNames[get_byte(p + 4)],
    'tag0' : tables.kTagNames[get_byte(p + 5)],
    'tag1' : tables.kTagNames[get_byte(p + 6)],
    # Warp destinations: each is [target_room, plane]. The plane bits for
    # hole and stairs 0-3 are packed into bytes p7 and p8 in 2-bit fields.
    'hole0_dest' : [get_byte(p+9), p7 & 3],
    'stair0_dest' : [get_byte(p+10), p7 >> 2 & 3],
    'stair1_dest' : [get_byte(p+11), p7 >> 4 & 3],
    'stair2_dest' : [get_byte(p+12), p7 >> 6 & 3],
    'stair3_dest' : [get_byte(p+13),p8 & 3],
    # Telepathy tile message index for this room
    'tele_msg' : get_word(0x87F61D+room_index*2),
    'sort_sprites' : sort_sprites_setting,
    'pits_hurt_player' : room_index in pits_hurt_player()
  }

  # Decodes the sprite list for this room. Each sprite is 3 bytes encoding
  # y-position, x-position, and sprite type. Special encodings handle:
  #   - Type 0xE4 with y=0xFE/0xFD: key/big-key drop modifier on previous sprite
  #   - x >= 0xE0: overlord sprites (high-index sprites stored at type+0x100)
  #   - Otherwise: standard sprites with a 5-bit subtype packed into x/y high bits
  # Returns: list of [x, y, floor_name, sprite_name] entries
  def get_sprites():
    ea = 0x890000 + get_word(0x89D62E + room_index * 2)
    # Skip past the sort_sprites byte to reach the actual sprite entries
    ea += 1
    r = []
    # 0xFF terminates the sprite list
    while get_byte(ea) != 0xff:
      y, x, type = get_byte(ea), get_byte(ea+1), get_byte(ea+2)
      # Type 0xE4 is a special "key drop" modifier — it attaches to the
      # previous sprite rather than creating a new entry
      if type == 0xe4:
        if y == 0xfe or y == 0xfd:
          r[-1].append('drop_key' if y == 0xfe else 'drop_big_key')
          ea += 3
          continue
      # x >= 0xE0 signals an overlord sprite — these use a separate name table
      # range (type + 0x100) and encode position in the low 5 bits only
      elif x >= 0xe0:
        floor = y >> 7
        r.append([x & 0x1f, y & 0x1f, 'lower' if floor else 'upper', tables.kSpriteNames[type + 0x100]])
        ea += 3
        continue
      # Standard sprite: subtype is packed across bits 5-7 of x and bits 5-6 of y
      subtype = (x >> 5) | ((y >> 5) & 3) << 3
      # Bit 7 of y determines which floor layer the sprite is on
      floor = y >> 7
      name = tables.kSpriteNames[type]
      # Non-zero subtypes are inserted into the name before the dash separator
      if subtype != 0:
        i = name.index('-')
        name = name[:i] + ('.%d' % subtype) + name[i:]
      r.append([x & 0x1f, y & 0x1f, 'lower' if floor else 'upper', name])
      ea += 3
    return r

  # Reads the secret/interactive tile list for this room. Secrets are hidden
  # items or triggers (bombable walls, pushable blocks, etc.) placed on the
  # tile grid. Each entry is 3 bytes: 2-byte tile position + 1-byte secret type.
  # Returns: list of [x, y, secret_name] entries
  def get_secrets():
    ea = 0x810000 | get_word(0x81db69 + room_index * 2)
    xs = []
    # 0xFFFF terminates the secret list
    while get_word(ea) != 0xffff:
      # Position is a byte offset into the 64x64 tile grid (must be even-aligned)
      pos = get_word(ea)
      assert pos%2==0
      # Convert linear position to x,y tile coordinates within the room
      x, y = pos//2%64, pos//2//64
      xs.append([x, y, tables.kSecretNames[get_byte(ea+2)]])
      ea += 3
    return xs

  # Retrieves chest contents for this room from the global chest table.
  # Big chests (key items) are marked with a trailing '!' in the output
  # to distinguish them from regular small chests.
  # Returns: list of item IDs (int for small chests, "N!" string for big chests)
  def get_chests():
    r = []
    for data, big in get_chest_info().get(room_index, []):
      if big:
        r.append('%d!' % data)
      else:
        r.append(data)
    return r
      
  sprites = get_sprites()
  secrets = get_secrets()

  # Assemble the complete room data structure with all sub-components
  data = {'Header' : header, 'Sprites' : sprites, 'Secrets' : secrets, 'Chests' : get_chests()}
  # Attach entrance and starting-point records that reference this room
  data['Entrances'] = get_entrance_info(0).get(room_index, [])
  if room_index in get_entrance_info(1):
    data['StartingPoints'] = get_entrance_info(1)[room_index]

  # Decode all 3 object layers sequentially from the ROM data stream.
  # The object data starts at room_addr+2 (after floor and layout bytes).
  # Each call to decode_room_objects consumes one layer and returns the
  # address where the next layer begins, plus optional door data.
  p = room_addr + 2
  p, objs, doors = decode_room_objects(p)
  data['Layer1'] = objs
  if doors: data['Layer1.doors'] = doors

  p, objs, doors = decode_room_objects(p)
  data['Layer2'] = objs
  if doors: data['Layer2.doors'] = doors

  p, objs, doors = decode_room_objects(p)
  data['Layer3'] = objs
  if doors: data['Layer3.doors'] = doors

  return yaml.dump(data, default_flow_style=None, sort_keys=False)

# Extracts all 320 dungeon rooms and writes each to its own YAML file.
# Parameters: none
# Returns: none (writes dungeon/dungeon-N.yaml files as side effect)
def print_all_dungeon_rooms():
  for i in range(320):
    s = print_room(i)
    open( 'dungeon/dungeon-%d.yaml' % i, 'w').write(s)

# Extracts the 8 default room templates that the engine pre-populates before
# overlaying room-specific objects. These templates provide the base tile
# layout shared by rooms that use the same default room index.
# Parameters: none
# Returns: none (writes dungeon/default_rooms.yaml as side effect)
def print_default_rooms():
  # Reads a single default room template by its index in the ROM pointer table
  def print_default_room(idx):
    # 3-byte pointer at table base 0x84EF2F gives the object data address
    p = 0x84EF2F + idx * 3
    room_addr = get_byte(p) | get_byte(p+1) << 8 | get_byte(p+2) << 16
    p, objs, doors = decode_room_objects(room_addr)
    # Default rooms should never contain door data
    assert doors == None
    return objs
  default_rooms = {}
  for i in range(8):
    default_rooms['Default%d' % i] = print_default_room(i)
  s = yaml.dump(default_rooms, default_flow_style=None, sort_keys=False)
  open('dungeon/default_rooms.yaml', 'w').write(s)

# Extracts the 19 overlay room templates. Overlays are layered on top of a
# room's base and default objects to add dungeon-specific decorations or
# structural elements (e.g., wall patterns shared across many rooms).
# Parameters: none
# Returns: none (writes dungeon/overlay_rooms.yaml as side effect)
def print_overlay_rooms():
  # Reads a single overlay room template by its index in the ROM pointer table
  def print_overlay_room(idx):
    # 3-byte pointer at table base 0x84ECC0 gives the object data address
    p = 0x84ECC0 + idx * 3
    room_addr = get_byte(p) | get_byte(p+1) << 8 | get_byte(p+2) << 16
    p, objs, doors = decode_room_objects(room_addr)
    # Overlay rooms should never contain door data
    assert doors == None
    return objs
  default_rooms = {}
  for i in range(19):
    default_rooms['Overlay%d' % i] = print_overlay_room(i)
  s = yaml.dump(default_rooms, default_flow_style=None, sort_keys=False)
  open('dungeon/overlay_rooms.yaml', 'w').write(s)

# Creates the output directory structure for all extracted data.
# Uses exist_ok=True so re-running the extraction is safe and idempotent.
# Parameters: none
# Returns: none (creates directories as side effect)
def make_directories():
  os.makedirs('img', exist_ok=True)
  os.makedirs('overworld', exist_ok=True)
  os.makedirs('dungeon', exist_ok=True)
  os.makedirs('sound', exist_ok=True)

# Orchestrates all text/YAML-based extractions in sequence: overworld areas,
# dungeon rooms (regular + default templates + overlay templates), dialogue
# strings, and the map32-to-map16 tile mapping table.
# Parameters: none
# Returns: none (delegates to individual extraction functions)
def print_all_text_stuff():
  print_all_overworld_areas()
  print_all_dungeon_rooms()
  print_overlay_rooms()
  print_default_rooms()
  print_dialogue()
  print_map32_to_map16(open('map32_to_map16.txt', 'w'))

# Top-level entry point that runs the full extraction pipeline:
# 1. Create output directories
# 2. Extract all text/YAML data (overworld, dungeon, dialogue, tile maps)
# 3. Extract music and sound effects via the SPC700 decoder
# 4. Decode and export all sprite sheet PNGs (Link, enemies, HUD, font)
# Parameters: none
# Returns: none
def main():
  make_directories()
  print_all_text_stuff()
  extract_music.extract_sound_data(util.ROM)
  # Sprite sheet extraction: each call handles a different category of
  # graphics data, decoding SNES planar tiles to indexed-color PNGs
  sprite_sheets.decode_link_sprites()
  sprite_sheets.decode_sprite_sheets()
  sprite_sheets.decode_hud_icons()
  sprite_sheets.decode_font()

# Script entry point: load the ROM (from command-line arg or default path),
# then run the full extraction pipeline
if __name__ == "__main__":
  util.load_rom(sys.argv[1] if len(sys.argv) >= 2 else None)
  main()

