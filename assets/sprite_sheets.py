# =============================================================================
# sprite_sheets.py — SNES Sprite/Font/Image Encoding and Decoding
# =============================================================================
# Central visual asset pipeline for the zelda3 C reimplementation. Converts
# between SNES planar tile formats (2bpp, 3bpp, 4bpp) and standard PNG images.
#
# SNES tile format overview:
#   - Graphics are stored as 8x8 pixel tiles in a "planar bitplane" layout.
#   - In 2bpp mode (4 colors), each tile is 16 bytes: two bitplanes interleaved
#     row-by-row (byte 0 = plane0 row0, byte 1 = plane1 row0, ...).
#   - In 3bpp mode (8 colors), each tile is 24 bytes: the first 16 bytes hold
#     planes 0-1 interleaved, then 8 bytes for plane 2 (one per row).
#   - In 4bpp mode (16 colors), each tile is 32 bytes: planes 0-1 interleaved
#     (16 bytes) then planes 2-3 interleaved (16 bytes).
#   - Pixel values are reconstructed by combining corresponding bits from each
#     plane, producing an index into a palette (4, 8, or 16 entries).
#
# SNES palettes:
#   - Each color is 15-bit RGB (5 bits per channel) packed into a 16-bit word
#     as 0bbb_bbgg_gggr_rrrr. This file converts them to standard 24-bit RGB.
#
# PNG round-trip system:
#   - decode_sprite_sheets() reads SNES ROM tile data and writes visualization
#     PNGs with palette swatches and metadata tags embedded in the pixels.
#   - load_sprite_sheets() reads those PNGs back, locating tiles via binary
#     tags (encoded_id) embedded as pixel color patterns, and reconstructs the
#     SNES-format tile data. This enables a decode-edit-reencode workflow.
#
# Key decode functions: decode_2bit_tileset, decode_3bit_tileset,
#   decode_4bit_tileset_link — each produces a 128px-wide indexed pixel array.
# Key encode functions: encode_font_from_png, encode_sheet_in_snes_format.
# MasterTilesheets class: aggregates all tiles and verifies round-trip fidelity.
# =============================================================================

# --- Image processing library for reading/writing PNG files ---
from PIL import Image
# --- Project-internal module defining sprite layout entries (name, matrix, palette info) ---
import sprite_sheet_info
# --- ROM access utilities: byte reading, decompression, caching ---
import util
from util import get_bytes, get_words, get_byte, cache
# --- Standard library: typed numeric arrays for efficient pixel storage ---
import array
# --- ROM address tables: tileset pointers, palette addresses, sprite init flags ---
import tables
# --- Standard library: command-line argument access (used in commented-out main block) ---
import sys

# Optional debug override for Link's armor/gloves palette colors (SNES 15-bit RGB values).
# When set to a list, replaces the default armor palette during rendering.
# The commented-out values below represent alternate tunic/glove color sets.
override_armor_palette = None
#override_armor_palette = [0x7fff, 0x237e, 0x11b7, 0x369e, 0x14a5,  0x1ff, 0x1078, 0x599d, 0x3647, 0x3b68, 0xa4a, 0x12ef, 0x2a5c, 0x1571, 0x7a18,
#                          0x7fff, 0x237e, 0x11b7, 0x369e, 0x14a5,  0x1ff, 0x1078, 0x599d, 0x6980, 0x7691, 0x26b8, 0x437f, 0x2a5c, 0x1199, 0x7a18,
#                          0x7fff, 0x237e, 0x11b7, 0x369e, 0x14a5,  0x1ff, 0x1078, 0x599d, 0x1057, 0x457e, 0x6df3, 0xfeb9, 0x2a5c, 0x2227, 0x7a18,
#                          0x7fff, 0x237e, 0x11b7, 0x369e, 0x14a5,  0x1ff, 0x1078, 0x3d97, 0x3647, 0x3b68, 0xa4a, 0x12ef, 0x567e, 0x1571, 0x7a18,
#                          0,      0x0EFA, 0x7DD1, 0,      0x7F1A,  0x7F1A,0,      0x716E, 0x7DD1, 0x40A7, 0x7DD1, 0x40A7, 0x48E9, 0x50CF, 0x7FFF]


# Saves pixel data as a PNG image. If a palette is provided, creates a paletted (P mode)
# image; otherwise creates a grayscale (L mode) image.
# Parameters:
#   dimensions — (width, height) tuple for the image
#   data — flat sequence of pixel values (palette indices or grayscale intensities)
#   fname — output file path
#   palette — optional flat bytearray of R,G,B,R,G,B,... values for up to 256 colors
def save_as_png(dimensions, data, fname, palette = None):
  img = Image.new('L' if palette == None else 'P', dimensions)
  img.putdata(data)
  if palette != None:
    img.putpalette(palette)
  img.save(fname)

# Saves pixel data as a 24-bit RGB PNG (no palette). Each element in data is an (R, G, B) tuple.
# Parameters:
#   dimensions — (width, height) tuple
#   data — flat sequence of (R, G, B) tuples, one per pixel
#   fname — output file path
def save_as_24bpp_png(dimensions, data, fname):
  img = Image.new("RGB", dimensions)
  img.putdata(data)
  img.save(fname)

# Decodes a 2bpp (2 bits-per-pixel, 4 colors) compressed SNES tileset into an
# 8bpp indexed pixel array that is 128 pixels wide.
# Parameters:
#   tileset — index into kCompSpritePtrs, identifies which compressed tile data to load
#   height — pixel height of the output image (default 32 = 4 rows of 8px tiles)
#   base — palette base offset added to each pixel value; can be an int (same for all tiles)
#           or a tuple (per-tile base), used by HUD icons to assign different palettes per tile
# Returns: bytearray of 128*height pixel values, each in range [base..base+3]
#
# SNES 2bpp layout: each 8x8 tile is 16 bytes. For each row y (0-7):
#   byte[2y]   = bitplane 0 (bit 0 of each pixel)
#   byte[2y+1] = bitplane 1 (bit 1 of each pixel)
# Pixels are stored MSB-left, so bit 7 = leftmost pixel (hence the 7-x reversal below).
# The output is arranged as a 128px-wide strip: 16 tiles across, height/8 tiles tall.
@cache
def decode_2bit_tileset(tileset, height = 32, base = 0):
  data = util.decomp(tables.kCompSpritePtrs[tileset], get_byte, False)
  # Each tile is 16 bytes; total tiles = (128/8) * (height/8) = 16 * height/8
  assert len(data) == 0x400 * height // 32
  dst = bytearray(128*height)
  # Inner function: decodes one 8x8 tile from 2bpp planar data into the output buffer
  def decode_2bit(offs, toffs, base):
    for y in range(8):
      # Read the two bitplane bytes for this row
      d0, d1 = data[offs + y * 2], data[offs + y * 2 + 1]
      for x in range(8):
        # Combine bit x from each plane to form a 2-bit pixel value
        t = ((d0 >> x) & 1) * 1 + ((d1 >> x) & 1) * 2
        # Write pixel at reversed x position (SNES stores MSB as leftmost pixel)
        dst[toffs + y * 128 + (7 - x)] = t + base
  # Iterate over all tiles in the grid (16 tiles wide, height/8 tiles tall)
  for i in range(16*height//8):
    x = i % 16
    y = i // 16
    # Each tile occupies 16 bytes in the source; per-tile base allows palette variation
    decode_2bit(i * 16, x * 8 + y * 8 * 128, base[i] if isinstance(base, tuple) else base)
  return dst

# Checks whether a 3bpp tileset uses the "high" palette range (indices 8-15) instead of
# the "low" range (indices 0-7). This matters because SNES sprites can reference either
# half of a 16-color palette; some tilesets are drawn with palette entries 8-15.
# Parameter: tileset — tileset index number
# Returns: True if this tileset uses palette indices 8-15
def is_high_3bit_tileset(tileset):
  # is 0x5c, 0x5e, 0x5f included or not?
  return tileset in (0x52, 0x53, 0x5a, 0x5b, 0x5c, 0x5e, 0x5f)

# Retrieves raw SNES tile data for a given tileset, either directly from ROM (tilesets 0-11
# are stored uncompressed) or by decompressing (tilesets 12+ use LZ-style compression).
# Parameter: tileset — tileset index
# Returns: bytearray of raw SNES tile bytes (24 bytes per tile for 3bpp tilesets)
def get_unpacked_snes_tileset(tileset):
  if tileset < 12:
    return get_bytes(tables.kCompSpritePtrs[tileset], 0x600)
  else:
    return util.decomp(tables.kCompSpritePtrs[tileset], get_byte, False)

# Decodes a 3bpp (3 bits-per-pixel, 8 colors) SNES tileset into a 128px-wide pixel array.
# The output is always 128x32 (64 tiles arranged as 16 columns x 4 rows).
#
# SNES 3bpp layout: each 8x8 tile is 24 bytes.
#   Bytes 0-15: bitplanes 0 and 1 interleaved (same as 2bpp)
#   Bytes 16-23: bitplane 2, one byte per row (not interleaved)
# Parameter: tileset — tileset index (used to fetch data and determine high/low palette)
# Returns: bytearray of 128*32 pixel values, each in range [base..base+7]
@cache
def decode_3bit_tileset(tileset):
  data = get_unpacked_snes_tileset(tileset)
  # 0x600 = 1536 bytes = 64 tiles * 24 bytes/tile (the standard 3bpp tileset size)
  assert len(data) == 0x600
  # High tilesets offset all pixel values by 8 to reference palette entries 8-15
  base = 8 if is_high_3bit_tileset(tileset) else 0
  height = 32
  dst = bytearray(128*height)
  # Inner function: decodes one 8x8 tile from 3bpp planar data
  def decode_3bit(offs, toffs):
    for y in range(8):
      # Planes 0-1 are interleaved; plane 2 is stored sequentially after the first 16 bytes
      d0, d1, d2 = data[offs + y * 2], data[offs + y * 2 + 1], data[offs + y + 16]
      for x in range(8):
        # Combine 3 bitplane bits into a single 3-bit pixel value (0-7)
        t = ((d0 >> x) & 1) * 1 + ((d1 >> x) & 1) * 2 + ((d2 >> x) & 1) * 4
        dst[toffs + y * 128 + (7 - x)] = base + t
  for i in range(16*height//8):
    x, y = i % 16, i // 16
    # Each 3bpp tile is 24 bytes in the source data
    decode_3bit(i * 24, x * 8 + y * 8 * 128)
  return dst

# Decodes Link's 4bpp (4 bits-per-pixel, 16 colors) sprite tileset from ROM address 0x108000.
# Produces a 128x448 pixel image containing all of Link's animation frames.
#
# SNES 4bpp layout: each 8x8 tile is 32 bytes.
#   Bytes 0-15: bitplanes 0 and 1 interleaved (same as 2bpp)
#   Bytes 16-31: bitplanes 2 and 3 interleaved (same structure, higher planes)
# Returns: bytearray of 128*448 pixel values, each in range [0..15]
def decode_4bit_tileset_link():
  height = 448
  # 0x108000 is the ROM address where Link's sprite graphics begin
  data = get_bytes(0x108000, 0x800 * height // 32) # only link sprites for now
  dst = bytearray(128*height)
  # Inner function: decodes one 8x8 tile from 4bpp planar data
  def decode_4bit(offs, toffs):
    for y in range(8):
      # Planes 0-1 in first 16 bytes, planes 2-3 in second 16 bytes (both interleaved)
      d0, d1, d2, d3 = data[offs + y * 2 + 0], data[offs + y * 2 + 1], data[offs + y * 2 + 16], data[offs + y * 2 + 17]
      for x in range(8):
        # Combine 4 bitplane bits into a 4-bit pixel value (0-15)
        t = ((d0 >> x) & 1) * 1 + ((d1 >> x) & 1) * 2 + ((d2 >> x) & 1) * 4 + ((d3 >> x) & 1) * 8
        dst[toffs + y * 128 + (7 - x)] = t
  for i in range(16*height//8):
    x, y = i % 16, i // 16
    # Each 4bpp tile is 32 bytes in the source data
    decode_4bit(i * 32, x * 8 + y * 8 * 128)
  return dst

# Converts a list of SNES 15-bit palette colors to a flat bytearray of 24-bit RGB values.
# SNES color format: 0bbb_bbgg_gggr_rrrr (5 bits per channel in a 16-bit word).
# The conversion expands 5-bit channels to 8-bit by shifting left 3 and filling the low
# bits with the top bits of the original value (e.g., 0b11010 -> 0b11010110), producing
# accurate color reproduction rather than simply multiplying by 8.
# Parameter: v — list of 16-bit SNES color words
# Returns: flat bytearray of [R, G, B, R, G, B, ...] suitable for PIL palette assignment
def convert_snes_palette(v):
  rr=bytearray()
  for x in v:
    # Extract 5-bit channels from the packed 16-bit SNES color word
    r, g, b = x & 0x1f, x >> 5 & 0x1f, x >> 10 & 0x1f
    # Scale 5-bit to 8-bit: shift left 3 places, then OR in top bits to fill low bits
    rr.extend((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2))
  return rr

# Converts a list of 24-bit integer colors (0xBBGGRR packed) to a flat bytearray of R,G,B bytes.
# Parameter: v — list of integer colors where bits [7:0]=R, [15:8]=G, [23:16]=B
# Returns: flat bytearray of [R, G, B, R, G, B, ...]
def convert_int24_palette_to_bytes(v):
  rr=bytearray()
  for x in v:
    rr.extend((x & 0xff, x >> 8 & 0xff, x >> 16 & 0xff))
  return rr

# Converts SNES 15-bit palette colors to a list of 24-bit integers (0xBBGGRR packed).
# Same channel expansion as convert_snes_palette but returns integers instead of a bytearray.
# Used for 24bpp image pixel comparisons and palette lookups in the round-trip system.
# Parameter: v — list of 16-bit SNES color words
# Returns: list of integers, each encoding one color as 0xBBGGRR
def convert_snes_palette_to_int(v):
  rr=[]
  for x in v:
    r, g, b = x & 0x1f, x >> 5 & 0x1f, x >> 10 & 0x1f
    rr.append((r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16)
  return rr

# Decodes Link's full sprite sheet (128x448) from 4bpp ROM data and saves it as a paletted PNG.
# Uses Link's default palette (green tunic). The 16-entry palette starts with transparent (0),
# then 15 colors covering skin, tunic, hair, shield, and equipment tones.
def decode_link_sprites():
  kLinkPalette = [0, 0x7fff, 0x237e, 0x11b7, 0x369e, 0x14a5,  0x1ff, 0x1078, 0x599d, 0x3647, 0x3b68,  0xa4a, 0x12ef, 0x2a5c, 0x1571, 0x7a18]
  save_as_png((128, 448), decode_4bit_tileset_link(), 'linksprite.png', convert_snes_palette(kLinkPalette))

# Builds the SNES palette used by HUD (heads-up display) icons.
# Reads 64 colors from ROM at 0x9BD660 and arranges them into a 256-entry palette.
# Each of the 16 palette groups gets 4 colors: index 0 is set to a default magenta marker,
# and indices 1-3 come from the ROM data. This sparse layout matches SNES OAM palette mapping.
# Returns: list of 256 SNES 15-bit color words
def get_hud_snes_palette():
  hud_palette  = get_words(0x9BD660, 64)
  palette = [(31 << 10 | 31) for i in range(256)]
  for i in range(16):
    for j in range(1, 4):
      palette[i * 16 + j] = hud_palette[i * 4 + j]
  return palette

# Extracts HUD icon graphics (hearts, items, map symbols) from ROM and saves them
# as a single paletted PNG. Each icon is a 2bpp tile; three tilesets (106, 107, 105)
# are decoded and stacked vertically. A palette_usage.bin lookup file determines
# which of the 8 available HUD palette groups each individual tile should use.
def decode_hud_icons():
  # Helper class that reads per-tile palette group assignments from a precomputed
  # binary file. Each byte is a bitmask; the lowest set bit selects the palette group.
  class PaletteUsage:
    def __init__(self):
      self.data = open('palette_usage.bin', 'rb').read()
    def get(self, icon):
      usage = self.data[icon]
      for j in range(8):
        if usage & (1 << j):
          return j 
      return 0
  pu = PaletteUsage()
  dst = bytearray()
  # Decode three HUD icon tilesets and stack them vertically (each 128x64)
  for slot, image_set in enumerate([106, 107, 105]):
    # Build per-tile palette base offsets: each tile gets its palette group * 16
    # so that 2bpp pixel values (0-3) map to the correct 4-color slice
    tbase = tuple([pu.get(slot * 128 + i) * 16 for i in range(128)])
    dst += decode_2bit_tileset(image_set, height = 64, base = tbase)

  # Save all three tilesets as one 128x192 paletted PNG using the first 128 HUD colors
  save_as_png((128, 64 * 3), dst, 'hud_icons.png', convert_snes_palette(get_hud_snes_palette()[:128]))

# Builds a character-index remapping table for the Portuguese font variant.
# The Portuguese ROM stores font glyph data in a non-standard order; this function
# reads a 121x3-byte table from ROM and maps each logical character code to the
# physical tile index where its glyph data is stored.
# Returns: dict mapping character code to tile index in the font data
def get_pt_remapper():
  b = util.ROM.get_bytes(0x8EFC09, 121 * 3)
  d = {}
  for i in range(121):
    # Convert linear index to the SNES character code layout (skipping bit 4)
    ch = (i & 0xf) | (i << 1) & 0xe0
    # Each entry provides two glyph mappings: base character and +0x10 variant
    d[ch] = b[i*3+0]
    d[ch|0x10] = b[i*3+1]
  return d

# Per-language font configuration table. Each entry defines:
#   [0] ROM address of 2bpp font tile data
#   [1] number of glyphs (always 256 = full character set)
#   [2] output PNG filename
#   [3] (ROM address of glyph width table, number of width entries)
# Languages with different character sets (German, French) use alternate ROM regions;
# most use the same US-region address with language-specific width tables.
kFontTypes = {
  'us'   : (0x8e8000, 256, 'font.png', (0x8ECADF, 99)),
  'de'   : (0xCC6E8, 256, 'font_de.png', (0x8CDECF, 112)),
  'fr'   : (0xCC6E8, 256, 'font_fr.png', (0x8CDEAF, 112)),
  'fr-c' : (0xCD078, 256, 'font_fr_c.png', (0x8CE83F, 112)),
  'en'   : (0x8E8000, 256, 'font_en.png', (0x8ECAFF, 102)),
  'es'   : (0x8e8000, 256, 'font_es.png', (0x8ECADF, 99)),
  'pl'   : (0x8e8000, 256, 'font_pl.png', (0x8ECADF, 99)),
  'pt'   : (0x8e8000, 256, 'font_pt.png', (0x8ECADF, 121)),
  'redux': (0x8e8000, 256, 'font_redux.png', (0x8ECADF, 99)),
  'nl': (0x8e8000, 256, 'font_nl.png', (0x8ECADF, 99)),
  'sv': (0x8e8000, 256, 'font_sv.png', (0x8ECADF, 99)),
}

# Extracts the in-game dialogue font from ROM and saves it as a paletted PNG.
# The font is stored as 256 2bpp 8x8 glyphs. The output image arranges glyphs in a
# 16-column grid with 1px gaps, and encodes per-glyph width markers as a special
# pixel (index 255) at the glyph's width position. Portuguese uses a non-standard
# glyph ordering that requires remapping via get_pt_remapper().
# After saving, verifies the PNG can be re-encoded to identical ROM data (except PT).
def decode_font():
  lang = util.ROM.language
  # Decodes one 8x8 glyph from 2bpp planar data into the output pixel buffer.
  # Same algorithm as decode_2bit but writes into an arbitrarily-pitched destination.
  def decomp_one_spr_2bit(data, offs, target, toffs, pitch, palette_base):
    for y in range(8):
      d0, d1 = data[offs + y * 2], data[offs + y * 2 + 1]
      for x in range(8):
        t = ((d0 >> x) & 1) * 1 + ((d1 >> x) & 1) * 2
        target[toffs + y * pitch + (7 - x)] = t + palette_base
  ft = kFontTypes[lang]
  # Portuguese stores glyph widths as the 3rd byte of each 3-byte record
  if lang == 'pt':
    W = util.ROM.get_bytes(0x8EFC09, 121 * 3)
    W = [W[i*3+2] for i in range(121)]
    remapper = get_pt_remapper()
  else:
    # Standard languages: read the width table directly from ROM
    W = get_bytes(*ft[3])
    remapper = {}
  # Output image width: 16 glyphs * 8px + 15 gaps of 1px each = 143px
  w = 128 + 15
  # Calculate image height: glyph rows * (8px glyph + 1px row gap per pair)
  hi = ft[1] // 32
  h = hi * 17
  # Read all glyph tile data (16 bytes per 2bpp glyph * 256 glyphs)
  data = get_bytes(ft[0], ft[1] * 16)
  dst = bytearray(w * h)
  
  # Decode each glyph and place it into the output grid with proper spacing
  for i in range(ft[1]):
    x, y = i % 16, i // 16
    # Use HUD palette group 6 (indices 96-99) for font rendering
    pal_base = 6 * 16
    # Calculate the pixel offset: 9px per glyph column (8+1gap), with extra gap per row pair
    base_offs = x * 9 + (y * 8 + (y >> 1)) * w
    # Decode the glyph using the remapped tile index (identity for non-PT languages)
    decomp_one_spr_2bit(data, remapper.get(i, i) * 16, dst, base_offs + w, w, pal_base)
    # For even rows, embed the glyph width marker as a special pixel (index 255)
    # in the row above the glyph. This marks how many pixels wide the glyph is.
    if (y & 1) == 0:
      j = (y >> 1) * 16 + x
      if j < len(W):
        dst[base_offs + W[j] - 1] = 255
  # Build the output palette: HUD colors for glyph pixels, gray background,
  # and a darker gray for the width marker pixels (index 255)
  pal = convert_snes_palette(get_hud_snes_palette()[:128])
  pal.extend([0] * 384)
  pal[0], pal[1], pal[2] = 192, 192, 192
  pal[255*3+0], pal[255*3+1], pal[255*3+2] = 128, 128, 128
  save_as_png((w, h), dst, ft[2], pal)
  # Round-trip verification: re-encode the saved PNG and confirm it matches the ROM.
  # Portuguese is excluded because its remapping makes byte-exact comparison impractical.
  if lang != 'pt':
    assert (data, W) == encode_font_from_png(lang)

# Re-encodes a font PNG back into 2bpp SNES tile data and a glyph width table.
# This is the inverse of decode_font(): reads the PNG pixel data, converts each
# 8x8 glyph region back to 2bpp planar bytes, and extracts width markers.
# Parameter: lang — language key into kFontTypes
# Returns: (bytearray of 256*16 bytes of 2bpp tile data, bytearray of glyph widths)
def encode_font_from_png(lang):
  font_data = Image.open(kFontTypes[lang][2]).tobytes()
  # Encodes one 8x8 glyph from pixel data back to 2bpp planar format (16 bytes).
  # Reverses the x-axis to restore SNES MSB-left bit ordering.
  def encode_one_spr_2bit(data, offs, target, toffs, pitch):
    for y in range(8):
      d0, d1 = 0, 0
      for x in range(8):
        pixel = data[offs + y * pitch + 7 - x]
        d0 |= (pixel & 1) << x
        d1 |= ((pixel >> 1) & 1) << x
      target[toffs + y * 2 + 0], target[toffs + y * 2 + 1] = d0, d1
  w = 128 + 15
  dst = bytearray(256 * 16)
  # Reads the width marker from the row above a glyph: scans left-to-right for
  # the special pixel (index 255) and returns its position + 1 as the glyph width
  def get_width(base_offs):
    for i in range(8):
      if font_data[base_offs + i] == 255:
        break
    return i + 1
  W = bytearray()
  # Process all 256 glyph slots, extracting widths from even rows and encoding tiles
  for i in range(256):
    x, y = i % 16, i // 16
    base_offs = x * 9 + (y * 8 + (y >> 1)) * w
    # Width markers only exist on even rows (one per glyph pair)
    if (y & 1) == 0:
      W.append(get_width(base_offs))
    encode_one_spr_2bit(font_data, base_offs + w, dst, i * 16, w)
  # Return only as many width entries as this language defines
  chars_per_lang = kFontTypes[lang][3][1]
  return dst, W[:chars_per_lang]

# Returns the dungeon palette for the specified palette index
# 0 = lightworld, 1 = darkworld, 2 = dungeon
@cache
def get_palette_subidx(palset_idx, dungeon_or_ow, which_palette):
  spmain = 1 if (dungeon_or_ow == 1) else 0
  if dungeon_or_ow == 2:
    main, sp0l, sp5l, sp6l = tables.kDungPalinfos[palset_idx]
    sp0r = (main // 2) + 11
    sp6r = 10
  else:
    sp5l, sp6l = tables.kOwSprPalInfo[palset_idx * 2 : palset_idx * 2 + 2]
    sp0l = 3 if dungeon_or_ow == 1 else 1
    sp0r = 9 if dungeon_or_ow == 1 else 7
    sp6r = 8 if dungeon_or_ow == 1 else 6
  pal_defs = (sp0l, sp0r, 
    spmain, None, spmain, None, spmain, None, None, None,
    sp5l, None,
    sp6l, sp6r,
    None, None)
  return pal_defs[which_palette]

# Retrieves a 7-color palette slice from ROM based on which SNES sprite palette
# group (pal_idx) and sub-palette index (j) are requested. The SNES has multiple
# palette banks at different ROM addresses depending on sprite type:
#   pal_idx 0      — auxiliary sprite palette 3 (kPalette_SpriteAux3)
#   pal_idx 1      — misc sprite palette, with dungeon BG fallback for j >= 11
#   pal_idx 2-9    — main sprite palettes, indexed by light/dark world variant
#   pal_idx 10, 12 — auxiliary sprite palette 1
#   pal_idx 13     — misc sprite palette (duplicate of idx 1 logic)
#   pal_idx 14, 15 — armor and gloves palettes (green tunic vs blue tunic)
# Parameters:
#   pal_idx — which palette group (0-15), maps to different ROM address tables
#   j — sub-index within the palette group (selects which 7-color slice)
# Returns: list of 7 SNES 15-bit color words forming one palette row
@cache
def get_palette_subset(pal_idx, j):
  # Default to sub-index 0 when no specific sub-palette was requested
  if j == None: j = 0
  pal = [0] * 7
  # Each branch reads 7 consecutive SNES color words from the appropriate ROM palette table
  if pal_idx == 0: # 0
    kPalette_SpriteAux3 = get_words(0x9BD39E, 84)
    return kPalette_SpriteAux3[j * 7 : j * 7 + 7]
  if pal_idx == 1: # 0R
    if j < 11:
      kPalette_MiscSprite = get_words(0x9BD446, 77)
      return kPalette_MiscSprite[j * 7 : j * 7 + 7]
    else:
      kPalette_DungBgMain = get_words(0x9BD734, 1800)
      return kPalette_DungBgMain[(j - 11) * 90 : (j - 11) * 90 + 7]
  # Main sprite palettes: offset calculated from world variant (j), palette pair
  # (even/odd pal_idx), and slot within the 120-word kPalette_MainSpr table
  if pal_idx in (2, 3, 4, 5, 6, 7, 8, 9):
    o = j * 60 + ((pal_idx - 2) >> 1) * 15 + (pal_idx & 1) * 8
    kPalette_MainSpr = get_words(0x9BD218, 120)
    return kPalette_MainSpr[o : o + 7]
  if pal_idx in (10, 12):
    kPalette_SpriteAux1 = get_words(0x9BD4E0, 168)
    return kPalette_SpriteAux1[j * 7 : j * 7 + 7]
  # Palette index 11 is not used by any sprite — guard against invalid access
  assert pal_idx != 11
  # Palettes 13-15 map to misc sprites and Link's armor/gloves color sets
  if pal_idx == 13:
    kPalette_MiscSprite = get_words(0x9BD446, 77)
    return kPalette_MiscSprite[j * 7 : j * 7 + 7]
  elif pal_idx == 14:
    # Green tunic armor palette (entries 1-7 from the armor/gloves table)
    kPalette_ArmorAndGloves = get_words(0x9BD308, 75)
    return kPalette_ArmorAndGloves[1:8]
  elif pal_idx == 15:
    # Blue tunic armor palette (entries 9-15 from the armor/gloves table)
    kPalette_ArmorAndGloves = get_words(0x9BD308, 75)
    return kPalette_ArmorAndGloves[9:16]

# Builds a complete 256-entry palette for rendering a sprite visualization PNG.
# The first entries are the sprite's actual colors (7 colors from the ROM palette),
# padded with marker colors for transparency, grid lines, text, and background.
# High palettes (odd pal_idx) place 9 transparent slots before the colors; low
# palettes place only 1, matching the SNES OAM palette offset conventions.
# Parameters:
#   pal_idx — palette group index (0-15); odd = high palette range
#   pal_subidx — sub-palette index within the group
# Returns: list of 256 integers, each a 24-bit 0xBBGGRR color
@cache
def get_full_palette(pal_idx, pal_subidx):
  rv = []
  # High palettes (odd index) use SNES palette slots 9-15, so pad 9 transparent entries
  if pal_idx & 1:
    rv.extend([0x00fe00] * 9)
  else:
    rv.extend([0x00fe00] * 1)
  # Insert the actual 7 sprite colors converted from SNES 15-bit to 24-bit RGB
  rv.extend(convert_snes_palette_to_int(get_palette_subset(pal_idx, pal_subidx)))
  # Fill remaining palette entries with magenta as a visual marker for unused slots
  rv += [0xe000e0] * (256 - len(rv))
  # Mark every 8th palette entry as teal to visually indicate transparent pixels
  # in the output PNG (each palette group's index 0 is the transparent slot)
  for i in range(0, 128, 8):
    rv[i] = 0x808000
  rv[251] = 0xe0c0c0 # blueish text
  rv[252] = 0xc0c0c0 # palette text
  rv[253] = 0xf0f0f0 # unallocated
  rv[254] = 0x404040 # lines
  rv[255] = 0xe0e0e0 # bg
  return rv

# Loads a tiny 3x5 pixel bitmap font used for labeling sprite sheet visualizations.
# The font image contains ASCII glyphs arranged in a grid; pixel data is cached
# so repeated text rendering does not re-read the file from disk.
# Returns: raw pixel bytes of the font image (grayscale, 0 = ink, nonzero = background)
@cache
def get_font_3x5():
  return Image.open('../other/3x5_font.png').tobytes()

# Draws a single 3x5 character glyph into a pixel buffer at the given position.
# The font image is 128px wide with 32 characters per row, each occupying a 4x6 cell.
# Only "ink" pixels (value 0 in the font image) are written; background is left unchanged.
# Parameters:
#   dst — mutable pixel buffer (bytearray) to draw into
#   dst_pitch — row stride of dst in pixels (bytes per row)
#   dx, dy — top-left pixel coordinate where the character is placed
#   ch — single ASCII character to render
#   color — palette index to use for the drawn pixels
def draw_letter3x5(dst, dst_pitch, dx, dy, ch, color):
  font = get_font_3x5()
  dst_offs = dy * dst_pitch + dx
  # Locate the glyph: 32 chars per row (5-bit column), 6 rows tall, 128px pitch
  src_offs = (ord(ch) & 31) * 4 + (ord(ch) // 32) * 6 * 128
  for y in range(6):
    for x in range(3):
      # Only copy ink pixels (0); skip background to preserve underlying content
      if font[src_offs + y * 128 + x] == 0:
        dst[dst_offs + y * dst_pitch + x] = color

# Draws a string of 3x5 characters into a pixel buffer, advancing 4 pixels per character.
# Parameters:
#   dst — mutable pixel buffer to draw into
#   dst_pitch — row stride of dst in pixels
#   dx, dy — top-left starting position for the first character
#   s — string to render
#   color — palette index for all drawn pixels
def draw_string3x5(dst, dst_pitch, dx, dy, s, color):
  for ch in s:
    draw_letter3x5(dst, dst_pitch, dx, dy, ch, color)
    # Each glyph is 3px wide + 1px spacing = 4px advance per character
    dx += 4

# Converts an indexed pixel buffer to 24-bit RGB by looking up each pixel value
# in the provided palette. Used to transform 8-bit indexed tile data into full-color
# pixel arrays for saving as 24bpp PNG images.
# Parameters:
#   data — sequence of palette indices (one per pixel)
#   palette — list of 24-bit 0xBBGGRR integer colors, indexed by pixel value
# Returns: list of 24-bit integer colors, same length as data
def convert_to_24bpp(data, palette):
  out = [0] * len(data)
  for i in range(len(data)):
    out[i] = palette[data[i]]
  return out

# Concatenates a sequence of bytearrays into a single bytearray.
# Used to merge multiple tile row buffers into one contiguous pixel strip.
# Parameter: ar — iterable of bytearray objects
# Returns: single bytearray containing all input arrays joined in order
def concat_byte_arrays(ar):
  r = bytearray()
  for a in ar:
    r += a
  return r

# Resolves derived fields on a sprite_sheet_info entry before rendering.
# Computes the actual VRAM tileset, palette index/sub-index, and a binary
# encoded_id tag that will be embedded in the PNG pixels for round-trip decoding.
# Parameters:
#   e — a sprite sheet entry object from sprite_sheet_info.entries
#   e_prev — the previous entry (or None), used to detect consecutive sprites
#            sharing the same name and palette so duplicate headers can be skipped
def fixup_sprite_set_entry(e, e_prev):
  # Parse the 2-digit hex sprite ID from the entry name; 'X' prefix = special sprite
  sprite_index = int(e.name[:2], 16) if e.name[0] != 'X' else 10000

  # Resolve indirect tileset: look up actual tileset via kSpriteTilesets table
  # unless dungeon_or_ow == 3 (which means the tileset is already a direct index)
  if e.dungeon_or_ow != 3:
    e.tileset = tables.kSpriteTilesets[e.tileset + (64 if e.dungeon_or_ow == 2 else 0)][e.ss_idx]
  # Determine if this tileset uses the high palette range (indices 8-15)
  e.high_palette = is_high_3bit_tileset(e.tileset)
  e.skip_header = False
  # If no palette base was explicitly set, derive it from the sprite's init flags in ROM.
  # The 3-bit palette field is bits [3:1] of kSpriteInit_Flags3.
  if e.pal_base == None:
    if sprite_index < len(tables.kSpriteInit_Flags3):
      e.pal_base = (tables.kSpriteInit_Flags3[sprite_index] >> 1) & 7
    else:
      e.pal_base = 4 # just default to some palette

  # Combine base palette and high/low flag into the full palette index (0-15)
  e.pal_idx = e.pal_base * 2 + e.high_palette
  e.pal_subidx = get_palette_subidx(e.palset_idx, e.dungeon_or_ow, e.pal_idx)

  # When consecutive entries share the same sprite name and palette, skip the
  # redundant header in the PNG to produce a more compact visualization
  if e_prev != None and e_prev.name == e.name and e_prev.pal_idx == e.pal_idx and e_prev.pal_subidx == e.pal_subidx:
    e.skip_header = True

  # the (pal_idx, pal_subidx) tuple identifies the palette
  # Pack sprite metadata into a single integer for binary embedding in the PNG.
  # Layout: bits [2:0]=pal_base, [9:3]=tileset, [10]=skip_header, [15:11]=pal_subidx
  # A simple checksum byte is appended (low 8 bits) to detect corruption on re-read.
  e.encoded_id = e.pal_base | e.tileset << 3 | e.skip_header << 10 | (e.pal_subidx or 0) << 11
  e.encoded_id = e.encoded_id << 8 | ((e.encoded_id + 41) % 255)


# Width of the sprite visualization PNG strip in pixels. 148px accommodates
# 16 tiles * 8px + 15px of 1px gaps between tile columns + 2px border + 9px label area.
BIGW = 148
# Renders one sprite sheet entry into a vertical PNG strip and appends the pixel
# rows to finaldst. Each entry produces a header band (sprite name, palette swatch,
# tileset number) followed by a tile grid showing the 4x16 VRAM layout matrix.
# Tiles marked '.' in the matrix are filled with the "unallocated" marker color.
# Parameters:
#   finaldst — list of 24-bit pixel values to append rendered rows to
#   e — sprite sheet entry (with fixup_sprite_set_entry already applied)
#   master_tilesheets — optional MasterTilesheets collector; if provided, each
#                       non-empty tile is also inserted for round-trip verification
def save_sprite_set_entry(finaldst, e, master_tilesheets = None):
  # Palette index 253 = "unallocated" marker color (light gray)
  empty_253 = [253] * 8
  # Fills an 8x8 tile area with the unallocated marker color
  def fill_with_empty(dst, dst_offs):
    for y in range(8):
      o = dst_offs + y * BIGW
      dst[o : o + 8] = empty_253

  # Copies one 8x8 tile from a 128px-wide source buffer to the BIGW-wide destination,
  # adding a palette base offset to each pixel value
  def copy_8x8(dst, dst_offs, src, src_offs, base):
    for y in range(8):
      for x in range(8):
        dst[dst_offs + y * BIGW + x] = src[src_offs + y * 128 + x] + base

  # Draws a horizontal line of a single color between x1 and x2 (inclusive)
  def hline(dst, x1, x2, y, color):
    for x in range(x1, x2 + 1):
      dst[y * BIGW + x] = color

  # Draws a vertical line of a single color between y1 and y2 (inclusive)
  def vline(dst, x, y1, y2, color):
    for y in range(y1, y2 + 1):
      dst[y * BIGW + x] = color
  
  # Fills a rectangular region with a solid color
  def fillrect(dst, x1, x2, y1, y2, color):
    for y in range(y1, y2 + 1):
      hline(dst, x1, x2, y, color)

  # Embeds a binary-encoded integer into the pixel buffer as a right-to-left bit
  # pattern. Each '1' bit is written as palette index 253 (visible); '0' bits are
  # left unchanged (background color). This tag is read back by load_sprite_sheets()
  # to recover tileset and palette metadata from edited PNGs.
  def encode_id(dst, x, y, v):
    while v:
      if v & 1:
        dst[y * BIGW + x] = 253
      x -= 1
      v >>= 1

  # Build the full 256-entry palette for this sprite's visualization
  palette = get_full_palette(e.pal_idx, e.pal_subidx)

  # Render the header band: sprite name, palette color swatches, tileset number
  if not e.skip_header:
    pal_subidx = e.pal_subidx
    # Build a human-readable palette name for the header label.
    # High palettes get an 'R' suffix; bases 1-3 show 'LW' (Light World)
    # or 'DW' (Dark World) instead of a numeric sub-index.
    if e.high_palette:
      pal_name = '%sR' % e.pal_base
    else:
      pal_name = '%s' % e.pal_base
      if e.pal_base in (1, 2, 3):
        assert e.pal_subidx in (0, 1)
        pal_subidx = 'LW' if pal_subidx == 0 else 'DW'
    if pal_subidx != None:
      pal_name = '%s-%s' % (pal_name, pal_subidx)

    # Create a 9-row header band filled with background color (index 255)
    header = bytearray([255] * BIGW * 9)
    # Draw the sprite name (first 22 chars) in the left margin
    draw_string3x5(header, BIGW, 1, 3, e.name[:22], 254)

    # Draw 7 palette color swatches as 5x5 filled rectangles near the right edge
    for i in range(7):
      xx = BIGW - 37 + i * 5 - 9
      fillrect(header, xx, xx + 4, 3, 7, i + (9 if e.high_palette else 1))

    # Draw tileset number and palette name labels in the right margin area
    draw_string3x5(header, BIGW, BIGW - 9, 3, '%2s' % e.tileset, 252)
    draw_string3x5(header, BIGW, BIGW - 45 - 1 - len(pal_name) * 4, 3, pal_name, 252)
    # Convert indexed header pixels to 24bpp and append to output
    finaldst.extend(convert_to_24bpp(header, palette))

  # Create the tile grid area: 36 rows tall (4 rows of 8px tiles + gaps + borders)
  bigdst = bytearray([255] * BIGW * 36)
  # Draw the border rectangle around the tile display area
  hline(bigdst, 1, 137, 0, 254)
  hline(bigdst, 1, 137, 34, 254)
  vline(bigdst, 1, 0, 34, 254)
  vline(bigdst, 137, 0, 34, 254)

  # Embed the binary metadata tag on the bottom row, shifted left by 9 bits with
  # a 0x55 sentinel pattern that load_sprite_sheets() uses to locate the tag
  encode_id(bigdst, 137, 35, e.encoded_id << 9 | 0x55)

  # Decode the 3bpp tileset into a 128x32 indexed pixel buffer
  src = decode_3bit_tileset(e.tileset) # returns 128x64

  # Place each 8x8 tile into the grid based on the 4x16 VRAM layout matrix.
  # The dst_offs calculation accounts for 1px gaps between every pair of tile rows
  # and tile columns (y>>1 and x>>1 add the inter-group spacing).
  for i in range(16*4):
    x, y = i % 16, i // 16
    dst_offs = (y * 8 + 1 + (y >> 1)) * BIGW + x * 8 + 2 + (x >> 1)
    if x & 8: dst_offs
    m = e.matrix[y][x]
    if m == '.':
      # Matrix marks this tile position as unused — fill with marker color
      fill_with_empty(bigdst, dst_offs)
    else:
      # Copy the actual tile pixels from the decoded tileset into the grid
      copy_8x8(bigdst, dst_offs, src, x * 8 + y * 8 * 128, 0)
      # Register this tile in the master tilesheet for aggregate output and verification
      if master_tilesheets:
        master_tilesheets.insert(e.tileset, src, x, y, palette, 0)

  # display vram addresses
  draw_string3x5(bigdst, BIGW, BIGW - 9, 4, '%Xx' % (e.ss_idx * 0x4), 251)
  draw_string3x5(bigdst, BIGW, BIGW - 9, 4 + 17, '%Xx' % (e.ss_idx * 0x4 + 2), 251)

  # collapse unused matrix sections
  if all(m=='.' for m in e.matrix[0]) and all(m=='.' for m in e.matrix[1]):
    del bigdst[1*BIGW:17*BIGW]
  elif all(m=='.' for m in e.matrix[2]) and all(m=='.' for m in e.matrix[3]):
    del bigdst[18*BIGW:34*BIGW]

  # For headerless entries, draw the tileset number in the bottom-right corner
  # so the tileset is still identifiable even without a full header band
  if e.skip_header:
    draw_string3x5(bigdst, BIGW, BIGW - 9, len(bigdst)//BIGW - 6, '%.2d' % e.tileset, 252)

  # Convert the tile grid to 24bpp and append to the output pixel strip
  finaldst.extend(convert_to_24bpp(bigdst, palette))

# Resolves all derived fields (tileset, palette, encoded_id, skip_header) on every
# sprite sheet entry in sequence. Must be called once before any rendering pass.
def fixup_entries():
  e_prev = None
  for e in sprite_sheet_info.entries:
    fixup_sprite_set_entry(e, e_prev)
    e_prev = e

# Aggregates all decoded 3bpp tilesheets and supports round-trip verification.
# During decode, tiles from individual sprite entries are collected into per-tileset
# 128x32 pixel buffers. During encode (load from edited PNGs), the same structure
# verifies that re-encoded SNES data exactly matches the original ROM bytes.
# Internal state:
#   self.sheets — dict mapping tileset index to a tuple of:
#     [0] array.array('I') of 128*32 24-bit pixels (the visual representation)
#     [1] bytearray of 128*32 8-bit palette indices (None during decode-only,
#         populated during encode for SNES-format round-trip verification)
class MasterTilesheets:
  def __init__(self):
    self.sheets = {}

  # Inserts one 8x8 tile into the master tilesheet during the decode pass.
  # Converts each indexed pixel through the palette to 24-bit color and stores it
  # in the 128x32 sheet buffer. The green marker (0x00f000) fills unvisited pixels.
  # Parameters:
  #   tileset — tileset index, used as the dict key
  #   src — 128px-wide indexed pixel buffer (the full decoded tileset)
  #   xp, yp — tile column and row within the tileset grid
  #   palette — 256-entry 24-bit palette for color conversion
  #   pal_base — base offset added to each pixel index before palette lookup
  def insert(self, tileset, src, xp, yp, palette, pal_base):
    sheet = self.sheets.get(tileset)
    if sheet == None:
      # Initialize with green marker pixels; 8-bit layer is None (decode-only)
      sheet = (array.array('I', [0x00f000] * 128 * 32), None)
      self.sheets[tileset] = sheet
    sheet24 = sheet[0]
    for y in range(yp * 8, yp * 8 + 8):
      for x in range(xp * 8, xp * 8 + 8):
        o = y * 128 + x
        sheet24[o] = palette[pal_base + src[o]]

  # Inserts one 8x8 tile during the encode (load-from-PNG) pass and validates
  # consistency. Unlike insert(), this also populates the 8-bit palette-index layer
  # by reverse-mapping 24-bit pixel colors through pal_lut. Raises an exception if
  # a pixel color is not found in the palette, or if a tile position was already
  # written with a different palette index (indicating conflicting edits).
  # Parameters:
  #   tileset — tileset index
  #   pal_lut — dict mapping 24-bit pixel color to 8-bit palette index
  #   img_data — flat array of 24-bit pixel values from the loaded PNG
  #   pitch — row stride of img_data in pixels
  #   dst_pos — byte offset into the 128x32 sheet buffer for this tile's origin
  #   src_pos — byte offset into img_data for this tile's origin
  def add_verify_8x8(self, tileset, pal_lut, img_data, pitch, dst_pos, src_pos):
    # add to the master sheet
    sheet = self.sheets.get(tileset)
    if sheet == None:
      # Initialize both layers; 248 = sentinel meaning "not yet written"
      sheet = (array.array('I', [0x00f000] * 128 * 32), bytearray([248] * 128 * 32))
      self.sheets[tileset] = sheet
    sheet24, sheet8 = sheet[0], sheet[1]
    for y in range(8):
      for x in range(8):
        pixel = img_data[src_pos + y * pitch + x]
        o = dst_pos + y * 128 + x
        sheet24[o] = pixel
        # Reverse-map 24-bit color back to the 8-bit palette index
        pixel8 = pal_lut.get(pixel)
        if pixel8 == None:
          raise Exception('Pixel #%.6x not found' % pixel)
        # Verify consistency: if this pixel was already set, it must match
        if sheet8[o] != 248 and pixel8 != sheet8[o]:
          raise Exception('Pixel has more than one value')
        sheet8[o] = pixel8

  # Saves all collected tilesheets as a single tall PNG (128px wide) with labeled
  # section headers. When save_also_8bit is True, also writes an 8-bit paletted
  # version and runs SNES round-trip verification to confirm lossless encode.
  # Parameter:
  #   save_also_8bit — if True, produce the 8-bit indexed PNG and verify vs ROM
  def save_to_all_sheets(self, save_also_8bit = False):
    rv = array.array('I')
    if save_also_8bit:
      rv8 = bytearray()

    # Creates an 8-row text label banner and appends it to both output buffers
    def extend_with_string_line(text):
      header = array.array('I', [0xf0f0f0] * 128 * 8)
      draw_string3x5(header, 128, 1, 2, text, 0x404040)
      rv.extend(header)
      if save_also_8bit:
        header8 = bytearray([255] * 128 * 8)
        draw_string3x5(header8, 128, 1, 2, text, 254)
        rv8.extend(header8)
    extend_with_string_line('AUTO GENERATED DO NOT EDIT')
    # Append each tilesheet in index order, with a label header before each one
    kk = 0
    for k in sorted(self.sheets.keys()):
      extend_with_string_line('Sheet %d' % k)
      rv.extend(self.sheets[k][0])
      if save_also_8bit:
        rv8.extend(self.sheets[k][1])
      # Detect gaps in the tileset sequence (missing sheets)
      if k != kk:
        print('Missing %d' % kk)
      kk = k + 1
    if save_also_8bit:
      # Build a combined palette covering both low and high palette ranges
      # for the 8-bit indexed output image
      palette8 = get_full_palette(4, 0)
      for i, v in enumerate(convert_snes_palette_to_int(get_palette_subset(5, 0))):
        palette8[i + 9] = v
      # Mark unwritten pixels (sentinel 248) with the green marker color
      palette8[248] = 0x00f000
      save_as_png((128, len(rv)//128), rv8, 'sprites/all_sheets_8bit.png', palette = convert_int24_palette_to_bytes(palette8))
      # Verify that re-encoding the 8-bit data back to SNES format matches the ROM
      self.verify_identical_to_snes()
    save_as_24bpp_png((128, len(rv)//128), rv, 'sprites/all_sheets.png')

  # Compares every tilesheet's re-encoded SNES bytes against the original ROM data.
  # Stops at the first mismatch, which indicates an encode bug or a lossy edit.
  # Covers tilesets 0-102 (the full set of compressed sprite graphics in the ROM).
  def verify_identical_to_snes(self):
    for tileset in range(103):
      a = self.encode_sheet_in_snes_format(tileset)
      b = get_unpacked_snes_tileset(tileset)
      if a != b:
        print('Sheet %d mismatch' % tileset)
        break

  # convert a tileset to the 8bit snes format, 24 bytes per tile
  # Encodes the 8-bit indexed pixel data for one tilesheet back into the SNES 3bpp
  # planar tile format. This is the inverse of decode_3bit_tileset(): each 8x8 tile
  # becomes 24 bytes with planes 0-1 interleaved (16 bytes) and plane 2 sequential
  # (8 bytes). The pixel x-axis is reversed (bit 7 = leftmost) to match SNES convention.
  # Parameter: tileset — tileset index to encode
  # Returns: bytearray of 24 * 64 = 1536 bytes (the full SNES tileset)
  def encode_sheet_in_snes_format(self, tileset):
    sheet8 = self.sheets[tileset][1]
    result = bytearray(24 * 16 * 4)
    # Encodes one 8x8 tile from the 128px-wide indexed buffer into 3bpp planar bytes
    def encode_into(dst_pos, src_pos):
      for y in range(8):
        for x in range(8):
          # Read pixel at reversed x (7-x) to match SNES MSB-left bit ordering
          b = sheet8[src_pos + 128 * y + 7 - x]
          # Distribute the 3 color bits into their respective bitplane bytes
          result[dst_pos+2*y] |= (b & 1) << x
          result[dst_pos+2*y+1] |= ((b & 2) >> 1) << x
          result[dst_pos+y+16] |= ((b & 4) >> 2) << x
    # Iterate over all 64 tiles in the 16x4 grid
    for y in range(4):
      for x in range(16):
        encode_into((y * 16 + x) * 24, y * 8 * 128 + x * 8)
    return result

# Main sprite sheet extraction entry point. Reads all SNES sprite tilesets from ROM,
# renders per-sprite visualization PNGs grouped by first character of sprite name
# (0-9, A-F, X), and produces a combined all_sheets.png showing every tilesheet.
# Each sprite PNG includes a header with name, palette swatch, and tileset number,
# followed by a tile grid reflecting the VRAM layout defined in sprite_sheet_info.
def decode_sprite_sheets():
  master_tilesheets = MasterTilesheets()

  # Compute derived palette/tileset fields for every sprite entry
  fixup_entries()
  # Group sprites by the first character of their name and render each group
  # into a separate tall PNG strip
  for char in "0123456789ABCDEFX":
    dst = []
    for e in sprite_sheet_info.entries:
      if e.name[0].upper() == char:
        save_sprite_set_entry(dst, e, master_tilesheets)
    if len(dst):
      save_as_24bpp_png((BIGW, len(dst) // BIGW), dst, 'sprites/sprites_%s.png' % char)
  # Write the combined tilesheet overview image
  master_tilesheets.save_to_all_sheets()

# Reverse of decode_sprite_sheets(): reads the per-sprite PNG files (potentially
# edited by an artist), locates binary metadata tags embedded in the pixel data,
# extracts each tile back to 8-bit indexed form via palette reverse-lookup, and
# reconstructs the MasterTilesheets structure for re-encoding to SNES format.
# Returns: a MasterTilesheets instance containing all loaded and verified tiles
def load_sprite_sheets():
  master_tilesheets = MasterTilesheets()
  # Process each sprite group PNG in the same order as decode_sprite_sheets()
  for char in "0123456789ABCDEFX":
    img = Image.open('sprites/sprites_%s.png' % char)
    img_size = img.size
    # Convert PIL pixel tuples to flat 24-bit integers for fast indexed access
    img_data = array.array('I', (a[0] | a[1] << 8 | a[2] << 16 for a in img.getdata()))
    pitch = img.size[0]

    # Scans the pixel data for the next binary tag marker. Tags are recognized by
    # the border line color (0x404040) at the bottom-right corner of a tile grid,
    # followed by a specific pattern of alternating tag/background colors. The scan
    # starts pixel-by-pixel for the first tag, then advances row-by-row afterward.
    # Returns: pixel offset of the tag row, or None if no more tags exist
    def find_next_tag(start_pos):
      for i in range(start_pos, len(img_data) - pitch * 2, pitch if start_pos != 0 else 1):
        # Check for the characteristic border + tag bit pattern signature
        if img_data[i+0] == 0x404040 and img_data[i+pitch] == 0x404040 and \
          img_data[i+pitch * 2 + 0] == 0xf0f0f0 and img_data[i+pitch * 2 - 1] == 0xe0e0e0 and \
          img_data[i+pitch * 2 - 2] == 0xf0f0f0 and img_data[i+pitch * 2 - 3] == 0xe0e0e0 and \
          img_data[i+pitch * 2 - 4] == 0xf0f0f0 and img_data[i+pitch * 2 - 5] == 0xe0e0e0 and \
          img_data[i+pitch * 2 - 6] == 0xf0f0f0 and img_data[i+pitch * 2 - 7] == 0xe0e0e0:
          return i + pitch * 2
      else:
        return None

    # Reads and decodes the binary metadata tag embedded as pixel colors at the
    # given position. The tag is a 64-bit value encoded as alternating background
    # (0xe0e0e0 = 0) and foreground (0xf0f0f0 = 1) pixels. The low 9 bits must be
    # the sentinel 0x55, followed by a checksum byte, then the packed sprite fields.
    # Returns: (pal_base, tileset, headerless_flag, pal_subidx)
    def decode_tag(pos):
      r = 0
      # Read 64 pixels left-to-right, building the tag value MSB-first
      for i in range(64):
        v = img_data[pos - 63 + i]
        assert v in (0xe0e0e0, 0xf0f0f0)
        r = r * 2 + (v == 0xf0f0f0)
      # Validate the sentinel pattern that confirms this is a real tag
      if (r & 0x1ff) != 0x55:
        raise Exception('Invalid tag')
      r >>= 9
      # Validate the checksum byte to detect accidental pixel corruption
      if (((r >> 8) + 41) % 255) != (r & 0xff):
        raise Exception('Invalid checksum')
      r >>= 8
      # Unpack the metadata fields from the remaining bits
      return r & 7, (r >> 3) & 127, (r >> 10) & 1, (r >> 11) & 31

    # Determines the pixel rectangles containing tile data above the tag position.
    # The tile grid height varies: 35px means both top and bottom halves are present
    # (rows 0-1 and rows 2-3 of the 4x16 matrix); 19px means one half was collapsed
    # because all tiles in that half were empty (marked '.' in the matrix).
    # Returns: (list of (half_index, pixel_offset) tuples, total_height)
    def determine_image_rects(tag_pos):
      # Measure the height of the border by scanning upward for border-color pixels
      h = 1
      while (img_data[tag_pos - pitch * (h + 1)] == 0x404040):
        h += 1
      if h == 19:
        # Only one half is present; determine which by checking edge pixel colors
        if img_data[tag_pos - pitch * 2 - 1] == 0xe0e0e0:
          # Top half (rows 0-1) is present
          image_rects = ((0, tag_pos - 135 - pitch * 18), )
        elif img_data[tag_pos - pitch * 18 - 1] == 0xe0e0e0:
          # Bottom half (rows 2-3) is present
          image_rects = ((1, tag_pos - 135 - pitch * 17), )
        else:
          raise Exception('cant uncompact')
      elif h == 35:
        # Full grid: both halves present
        image_rects = ((0, tag_pos - 135 - pitch * 34), (1, tag_pos - 135 - pitch * 17))
      else:
        raise Exception('height not found')
      return image_rects, h

    # Builds a reverse palette lookup table by reading the 7 color swatch pixels
    # from the header band. Maps each 24-bit pixel color back to its 8-bit palette
    # index (1-7 for low palettes, 9-15 for high palettes). Also maps the teal
    # transparent marker (0x808000) to palette index 0.
    # Returns: dict of {24-bit color int: 8-bit palette index}
    def get_pal_lut(tag_pos, is_high):
      # read palette colors from the image pixels
      pal_lut = {img_data[tag_pos + 5 * i] : i + (9 if is_high else 1) for i in range(7)}
      pal_lut[0x808000] = 0 # transparent color
      return pal_lut

    # Checks whether an 8x8 tile region is entirely filled with the "unallocated"
    # marker color (0xf0f0f0), indicating no sprite data occupies this tile slot
    def is_empty(pos):
      return all(img_data[pos+y*pitch+x] == 0xf0f0f0 for x in range(8) for y in range(8))

    # Scan through the entire PNG, finding each embedded tag and extracting the
    # tile data above it back into the master tilesheet structure
    pal_lut = None
    tag_pos = 0
    while True:
      tag_pos = find_next_tag(tag_pos)
      if tag_pos == None:
        break
      # Decode the metadata tag to recover tileset, palette, and header state
      pal_base, tileset, headerless, pal_subidx = decode_tag(tag_pos)
      image_rects, box_height = determine_image_rects(tag_pos)
      # Only entries with a header carry palette swatches; headerless entries
      # reuse the palette from the preceding entry in the same sprite group
      if not headerless:
        pal_lut = get_pal_lut(tag_pos - pitch * (box_height + 3) - 34, is_high_3bit_tileset(tileset))
      # Extract each tile from the grid, accounting for the 1px inter-tile gaps
      for idx, sheet_pos in image_rects:
        for y in range(2):
          for x in range(16):
            # Compute source position with gap offsets (y>>1 and x>>1)
            src_pos = sheet_pos + (y * 8 + (y >> 1)) * pitch + (x * 8 + (x >> 1))
            # Compute destination position in the 128x32 tilesheet buffer
            dst_pos = (y + idx * 2) * 8 * 128 + x * 8
            # Only process tiles that contain actual sprite data
            if not is_empty(src_pos):
              master_tilesheets.add_verify_8x8(tileset, pal_lut, img_data, pitch, dst_pos, src_pos)
  return master_tilesheets

#if __name__ == "__main__":
#  ROM = util.load_rom(sys.argv[1] if len(sys.argv) >= 2 else None, True)
#  decode_font()
  


