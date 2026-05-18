# =============================================================================
# util.py — Foundation utility module for the zelda3 asset pipeline
#
# This file is the lowest-level building block used by every other script in
# the assets/ directory. It provides:
#
#   1. ROM loading and validation (LoadedRom class)
#      Reads the SNES ROM binary, strips optional SMC headers, identifies the
#      ROM's language/region by SHA1 hash, and provides byte-level access using
#      SNES LoROM addressing (24-bit addresses: bank in bits 23-16, offset in
#      bits 14-0, with bit 15 always set).
#
#   2. Byte / word / int accessors with sign extension
#      Convenience wrappers (get_byte, get_word, get_int8, get_int16) that
#      delegate to the global ROM object, with optional memoization via @cache.
#
#   3. SNES LZ decompression (decomp)
#      Implements the custom LZ-like compression format used by A Link to the
#      Past for tilesets, tilemaps, and other bulk data. Supports five command
#      types: literal copy, memset (single byte), memset16 (two-byte pattern),
#      incrementing fill, and backreference copy.
#
#   4. BRR (Bit Rate Reduction) audio codec
#      Decodes and encodes audio in the SNES SPC700 BRR format, where each
#      block of 9 bytes encodes 16 PCM samples using 4 adaptive filter modes
#      and a variable bit-shift.
#
#   5. Miscellaneous helpers
#      split_list, print_int_array (C array formatter), Reader (sequential ROM
#      cursor), and a simple memoization decorator (cache).
#
# No other file in assets/ operates without importing from this module.
# =============================================================================

# Standard library: array is used for typed PCM sample buffers in BRR decoding
import array
# Standard library: sys is used for stdout redirection in print_int_array
import sys
# Standard library: hashlib provides SHA1 hashing for ROM identification
import hashlib
# Standard library: os is used for filesystem path operations in ROM loading
import os
# Standard library: lru_cache provides the underlying memoization mechanism
from functools import lru_cache

# cache — Simple memoization decorator wrapping functools.lru_cache.
# Sets maxsize=None for an unbounded cache so that repeated ROM reads with
# the same address return instantly without re-reading the byte array.
# Used by get_bytes and get_words to avoid redundant ROM access.
def cache(user_function):
  'Simple lightweight unbounded cache.  Sometimes called "memoize".'
  return lru_cache(maxsize=None)(user_function)

# Both are common SNES rom extensions. For Zelda3 (NA), they are equivalent files.
COMMON_ROM_NAMES = ['zelda3.sfc', 'zelda3.smc']
# The ROM is expected to live one directory up from assets/ (the project root)
DEFAULT_ROM_DIRECTORY = os.path.join(os.path.dirname(__file__), '..')

# SHA1 hash of the canonical US release, used as the default single-language target
ZELDA3_SHA1_US = '6D4F10A8B10E10DBE624CB23CF03B88BB8252973'

# Lookup table mapping known ROM SHA1 hashes to (language_code, description) tuples.
# Each entry represents a verified ALTTP ROM variant — official releases plus
# community fan-translations hosted on romhacking.net.
# The language code (e.g. 'us', 'de', 'fr') determines which string tables and
# region-specific assets the extraction scripts will use.
ZELDA3_SHA1 = {
   ZELDA3_SHA1_US : ('us', 'Legend of Zelda, The - A Link to the Past (USA)'),
  '2E62494967FB0AFDF5DA1635607F9641DF7C6559' : ('de', 'Legend of Zelda, The - A Link to the Past (Germany)'),
  '229364A1B92A05167CD38609B1AA98F7041987CC' : ('fr', 'Legend of Zelda, The - A Link to the Past (France)'),
  'C1C6C7F76FFF936C534FF11F87A54162FC0AA100' : ('fr-c', 'Legend of Zelda, The - A Link to the Past (Canada)'),
  '7C073A222569B9B8E8CA5FCB5DFEC3B5E31DA895' : ('en',  'Legend of Zelda, The - A Link to the Past (Europe)'),
  '461FCBD700D1332009C0E85A7A136E2A8E4B111E' : ('es',  'Spanish - https://www.romhacking.net/translations/2195/'),
  '3C4D605EEFDA1D76F101965138F238476655B11D' : ('pl',  'Polish - https://www.romhacking.net/translations/5760/'),
  'D0D09ED41F9C373FE6AFDCCAFBF0DA8C88D3D90D' : ('pt',  'Portuguese - https://www.romhacking.net/translations/6530/'),
  'B2A07A59E64C498BC1B2F28728F9BF4014C8D582' : ('redux', 'English Redux - https://www.romhacking.net/translations/6657/'),
  '9325C22EB0A2A1F0017157C8B620BC3A605CEDE1' : ('redux', 'English Redux - https://www.romhacking.net/hacks/2594/'),
  'FA8ADFDBA2697C9A54D583A1284A22AC764C7637' : ('nl', 'Dutch - https://www.romhacking.net/translations/1124/'),
  '43CD3438469B2C3FE879EA2F410B3EF3CB3F1CA4' : ('sv', 'Swedish - https://www.romhacking.net/translations/982/'),
}

# load_rom — Loads the SNES ROM file into a global ROM object.
# Parameters:
#   filename: path to the ROM file (relative to project root), or None for auto-detect
#   support_multilanguage: if True, accept any known ROM variant; if False, require US ROM
# Returns: the LoadedRom instance (also stored in the module-global ROM variable)
# Side effect: sets the module-level global 'ROM' used by all get_byte/get_word helpers
def load_rom(filename, support_multilanguage = False):
  global ROM
  ROM = LoadedRom(filename, support_multilanguage)
  return ROM

# get_byte — Read a single byte from the global ROM at a 24-bit SNES LoROM address.
# Delegates to ROM.get_byte. The address must have bit 15 set (LoROM convention).
def get_byte(addr):
  return ROM.get_byte(addr)

# get_bytes — Read n consecutive bytes from the global ROM starting at addr.
# Cached via @cache so repeated reads of the same (addr, n) are free.
# Returns a bytearray.
@cache
def get_bytes(addr, n):
  return ROM.get_bytes(addr, n)

# get_words — Read n consecutive 16-bit little-endian words from the global ROM.
# Cached via @cache so repeated reads of the same (addr, n) are free.
# Returns a list of integers.
@cache
def get_words(addr, n):
  return ROM.get_words(addr, n)

# get_int8 — Read a signed 8-bit integer from the global ROM at address ea.
# Interprets the raw byte as two's complement: if bit 7 (0x80) is set, the
# value is negative, so subtract 256 to convert from unsigned [0..255] to
# signed [-128..127].
def get_int8(ea):
  b = get_byte(ea)
  # Check the sign bit (bit 7) and convert from unsigned to signed if set
  if b & 0x80: b -= 256
  return b

# get_int16 — Read a signed 16-bit integer from the global ROM at address ea.
# Interprets the raw 16-bit word as two's complement: if bit 15 (0x8000) is
# set, the value is negative, so subtract 65536 to convert from unsigned
# [0..65535] to signed [-32768..32767].
def get_int16(ea):
  b = get_word(ea)
  # Check the sign bit (bit 15) and convert from unsigned to signed if set
  if b & 0x8000: b -= 65536
  return b

# get_word — Read a single 16-bit little-endian word from the global ROM.
# Delegates to ROM.get_word.
def get_word(addr):
  return ROM.get_word(addr)


# LoadedRom — Represents a loaded and validated SNES ROM image.
# Responsibilities:
#   - Locate the ROM file on disk (auto-detect or user-specified path)
#   - Strip the optional 512-byte SMC copier header if present
#   - Identify the ROM's language/region by comparing its SHA1 hash against
#     the known-good hash table (ZELDA3_SHA1)
#   - Provide byte-level read access using SNES LoROM address mapping, where
#     a 24-bit SNES address is converted to a linear file offset
#
# SNES LoROM addressing:
#   A 24-bit address like 0xBBHHLL is decoded as:
#     bank   = (address >> 16) & 0x7F  (bits 23-16, with bit 23 masked off)
#     offset = address & 0x7FFF        (bits 14-0; bit 15 is always set in valid addresses)
#   Linear ROM offset = bank * 0x8000 + offset
#   This maps the SNES memory map to contiguous ROM bytes, skipping the
#   lower half of each bank (0x0000-0x7FFF) which is reserved for hardware
#   registers and RAM on the SNES.
class LoadedRom:
  def __init__(self, path = None, support_multilanguage = False):
    rom_path = self.__get_rom_path(path)
    self.ROM = open(rom_path, 'rb').read()

    # Remove the SMC header?
    # SMC (Super Magicom) copier headers are exactly 512 (0x200) bytes prepended
    # to the ROM data. A valid SNES ROM size is a clean multiple of 0x100000
    # (1 MB), so if the low 20 bits of the file size equal 0x200, there is a
    # 512-byte copier header that must be stripped before any address calculations.
    if (len(self.ROM) & 0xfffff) == 0x200:
      self.ROM = self.ROM[0x200:]

    # Compute SHA1 hash of the (header-stripped) ROM to identify its region/language
    hash = hashlib.sha1(self.ROM).hexdigest().upper()
    entry = ZELDA3_SHA1.get(hash)
    self.language = entry[0] if entry != None else None

    # Workaround for swedish rom with broken size
    # The Swedish fan-translation ROM has a malformed size that still includes
    # 0x200 extra bytes even after the first strip attempt above; strip again.
    if self.language == 'sv' and len(self.ROM) == 0x10083b:
      self.ROM = self.ROM[0x200:]

    # In multi-language mode, any recognized ROM is accepted; in single-language
    # mode, only the US ROM is allowed (because the C reimplementation's default
    # string tables and asset offsets target the US release).
    if support_multilanguage:
      if self.language == None:
        msg = f"\n\nROM with hash {hash} not supported.\n\nYou need one of the following ROMs to extract the resources:\n"
        for k, v in ZELDA3_SHA1.items():
          msg += '%5s: %s: %s\n' % (v[0], k, v[1])
        raise Exception(msg)
      print('Identified ROM as: %s - "%s"' % entry)
    else:
      if self.language != 'us':
        raise Exception(f"\n\nROM with hash {hash} not supported.\n\nExpected {ZELDA3_SHA1_US}.\nPlease verify your ROM is \"Legend of Zelda, The - A Link to the Past (USA)\"");

  # get_byte — Read one byte from the ROM using a 24-bit SNES LoROM address.
  # Parameters:
  #   ea: 24-bit SNES address (bit 15 must be set, enforced by the assert)
  # Returns: integer value of the byte at that ROM offset
  def get_byte(self, ea):
    # Bit 15 must be set — in LoROM, addresses below 0x8000 within a bank
    # are not mapped to ROM (they are SNES work RAM / hardware registers)
    assert (ea & 0x8000)
    # Convert 24-bit SNES address to linear ROM offset:
    #   bank = (ea >> 16) & 0x7F — extract bank number, mask off bit 23
    #   0x8000 multiplier — each bank contributes 32 KB of ROM data
    #   (ea & 0x7FFF) — extract the 15-bit offset within the bank
    ea = ((ea >> 16) & 0x7f) * 0x8000 + (ea & 0x7fff)
    return self.ROM[ea]

  # get_word — Read a 16-bit little-endian word (2 consecutive bytes) from
  # the ROM at SNES address ea. Low byte first, high byte second (SNES is
  # little-endian).
  def get_word(self, ea):
    return self.get_byte(ea) + self.get_byte(ea + 1) * 256

  # get_24 — Read a 24-bit little-endian value (3 consecutive bytes) from
  # the ROM at SNES address ea. Used for reading 24-bit pointers (bank:offset)
  # stored in ROM tables.
  def get_24(self, ea):
    return self.get_byte(ea) + self.get_byte(ea + 1) * 256 + self.get_byte(ea + 2) * 65536

  # get_bytes — Read n consecutive bytes from the ROM starting at SNES
  # address addr. Handles LoROM bank boundary crossings: when the offset
  # within a bank wraps past 0x7FFF back to 0x0000, it skips forward by
  # 0x8000 to re-enter the ROM-mapped region of the next bank.
  # Parameters:
  #   addr: starting 24-bit SNES LoROM address
  #   n:    number of bytes to read
  # Returns: bytearray of length n
  def get_bytes(self, addr, n):
    r = bytearray()
    for i in range(n):
      r.append(self.get_byte(addr))
      addr += 1
      # If incrementing the address cleared bit 15 (crossed a bank boundary
      # from 0xFFFF to 0x0000 in the low 16 bits), skip ahead by 0x8000 to
      # jump back into the ROM-mapped upper half of the next bank.
      if (addr & 0x8000) == 0:
        addr += 0x8000
    return r

  # get_words — Read n consecutive 16-bit little-endian words from the ROM.
  # Handles LoROM bank boundary crossings the same way as get_bytes.
  # Parameters:
  #   addr: starting 24-bit SNES LoROM address
  #   n:    number of 16-bit words to read
  # Returns: list of n integers, each in [0..65535]
  def get_words(self, addr, n):
    r = []
    for i in range(n):
      r.append(self.get_word(addr))
      addr += 2
      # Bank boundary crossing check — same logic as get_bytes
      if (addr & 0x8000) == 0:
        addr += 0x8000
    return r

  # __get_rom_path — Resolve the filesystem path to the ROM file.
  # If no path is given, searches for common ROM filenames (zelda3.sfc,
  # zelda3.smc) in the project root directory. If a path is given, it is
  # resolved relative to the project root.
  # Parameters:
  #   path: user-supplied ROM filename/path, or None for auto-detection
  # Returns: absolute filesystem path to the ROM file
  # Raises: Exception if no ROM file is found
  def __get_rom_path(self, path = None):
    # Check default locations when no path is given by user.
    if path is None:
      for rom_name in COMMON_ROM_NAMES:
        rom_path = os.path.join(DEFAULT_ROM_DIRECTORY, rom_name)
        if os.path.isfile(rom_path):
          return rom_path
      raise Exception(f"Could not find any ROMs ({', '.join(COMMON_ROM_NAMES)}) at the default location {DEFAULT_ROM_DIRECTORY}.")

    rom_path = os.path.join(DEFAULT_ROM_DIRECTORY, path)
    if os.path.isfile(rom_path):
      return rom_path
    raise Exception(f"No ROM found at provided path {rom_path}.")



# split_list — Partition a flat list into sublists of length n.
# The final sublist may be shorter than n if len(l) is not evenly divisible.
# Parameters:
#   l: the input list to split
#   n: the chunk size
# Returns: list of sublists
def split_list(l, n):
  return [l[i:i+n] for i in range(0, len(l), n)]

# to_hex — Format a numeric value as either a hex literal or a plain decimal.
# Values in the range [-9..9] are printed as plain decimals for readability;
# values outside that range are printed as hex (e.g. 0x1a, -0xf).
# Used by print_int_array when formatting C array initializers.
def to_hex(v):
  return '%#x' % v if v < -9 or v >9 else '%d'%v


# print_int_array — Output a C-style static const array declaration.
# Used by the asset extraction scripts to emit extracted ROM data as C source
# code that gets compiled into the zelda3 reimplementation.
# Parameters:
#   name:         the C variable name for the array
#   r:            the list of integer values to emit
#   tname:        the C type name (e.g. 'uint8', 'int16')
#   decimal:      if True, format values as decimal; if False, as hex; if None,
#                 leave values as-is (already formatted strings)
#   split_length: number of elements per line in the output (default 16)
#   file:         output stream (defaults to stdout)
# Returns: None (writes directly to the file stream)
def print_int_array(name, r, tname, decimal, split_length = 16, file = sys.stdout):
  rlen = len(r)
  # Split the flat value list into rows of split_length for formatted output
  rr = split_list(r, split_length )
  # Format each value as decimal or hex string if a format was requested
  if decimal != None:
    if decimal:
      rr = [['%d' % s for s in t] for t in rr]
    else:
      rr = [[to_hex(s) for s in t] for t in rr]

  # pad_all_columns — Right-align every column across all rows so that the
  # output forms a neatly aligned grid of values in the generated C source.
  def pad_all_columns(rrs):
    # Compute the maximum string width needed for each column position
    colsiz = [max((0 if j >= len(r) else len(r[j])) for r in rrs) for j in range(len(rrs[0]))]
    # Left-pad each cell with spaces to match the column width
    def pad(c, i):
      return (' ' * (i - len(c))) + c
    return [[pad(c, colsiz[i]) for (i, c) in enumerate(r)] for r in rrs]

  # If all values fit on one line, emit a single-line declaration
  if len(rr) == 1:
    print('static const %s %s[%d] = {%s};' % (tname, name, rlen, ", ".join(rr[0])), file = file)
  else:
    # Multi-line declaration: opening brace, padded rows, closing brace
    print('static const %s %s[%d] = {' % (tname, name, rlen), file = file)
    for t in pad_all_columns(rr):
      print("  " + "".join([(a + ', ') for a in t]), file = file)
    print('};', file = file)



# Reader — Sequential byte-reading cursor over the ROM.
# Wraps a read function (typically ROM.get_byte) and an auto-incrementing
# address pointer. Automatically handles LoROM bank boundary crossings
# when the 16-bit offset portion of the address wraps from 0xFFFF to 0x0000.
# Used by the decomp() function to consume compressed data byte-by-byte.
class Reader:
  # Parameters:
  #   ea: starting 24-bit SNES LoROM address
  #   rb: read-byte callable — typically ROM.get_byte or a similar function
  def __init__(self, ea, rb):
    self.ea, self.rb = ea, rb
  # next — Read the byte at the current address, advance the pointer by one,
  # and handle bank boundary crossing if needed.
  # Returns: the byte value at the current address before advancing
  def next(self):
    r = self.rb(self.ea)
    self.ea += 1
    # If the low 16 bits wrapped to 0x0000, skip the non-ROM lower half of
    # the next bank by adding 0x8000 (same boundary logic as get_bytes)
    if (self.ea & 0xffff) == 0:
      self.ea += 0x8000
    return r

# decomp — Decompress ALTTP's custom LZ-like compressed data.
#
# The SNES ALTTP ROM uses a proprietary compression scheme for tilesets,
# tilemaps, and other large data blocks. The format encodes a stream of
# commands, each with a 3-bit command type and a length field.
#
# Command encoding (first byte):
#   If the top 3 bits are NOT all 1 (i.e. byte & 0xE0 != 0xE0):
#     cmd    = bits 7-5 (3-bit command type)
#     length = bits 4-0 (5-bit length, so max 31 before +1 adjustment)
#   If the top 3 bits ARE all 1 (extended format for lengths > 31):
#     cmd    = bits 4-2 shifted left by 3 (same 3-bit command space)
#     length = bits 1-0 as high byte, next byte as low byte (10-bit length)
#   Length is always incremented by 1 after extraction.
#
# Command types (by the 3-bit cmd value):
#   000 (0x00): Literal — copy 'length' raw bytes from the stream to output
#   001 (0x20): Memset — read one byte, repeat it 'length' times
#   010 (0x40): Memset16 — read two bytes, alternate them for 'length' bytes
#   011 (0x60): Incrementing fill — read one byte, output it then increment,
#               repeating 'length' times (wraps at 0xFF to 0x00)
#   1xx (0x80+): Backreference copy — read a 2-byte offset into the already-
#                decompressed output, then copy 'length' bytes starting there
#
# The stream terminates when a byte of 0xFF is read as a command byte.
#
# Parameters:
#   ea:            starting SNES LoROM address of the compressed data
#   rb:            read-byte callable (e.g. ROM.get_byte)
#   offset_is_be:  if True, backreference offsets are big-endian (default);
#                  if False, they are little-endian (byte-swapped)
#   return_length: if True, return a tuple of (decompressed_data, compressed_length)
# Returns: bytearray of decompressed data (or tuple if return_length=True)
def decomp(ea, rb, offset_is_be = True, return_length = False):
  result = bytearray()
  reader = Reader(ea, rb)
  while True:
    b = reader.next()
    # 0xFF is the end-of-stream sentinel
    if b == 0xff:
      if return_length:
        # Calculate compressed data length by subtracting start from current
        # address, masked to 15 bits (within-bank offset)
        return result, (reader.ea - ea) & 0x7fff
      else:
        return result
    # Decode the command byte: extract the 3-bit command and the length field.
    # Standard format: top 3 bits != 111 — cmd is bits 7-5, length is bits 4-0
    if (b & 0xe0) != 0xe0:
      lx = b & 0x1f
      cmd = b & 0xe0
    # Extended format: top 3 bits == 111 — allows lengths > 31.
    # The actual command is in bits 4-2 (shifted left 3 to align with standard),
    # and the length is a 10-bit value: bits 1-0 are the high byte, next byte
    # is the low byte.
    else:
      cmd = (b << 3) & 0xe0
      lx = ((b & 3) << 8) | reader.next()
    # Length is stored as (actual_length - 1), so add 1
    lx += 1
    if cmd == 0x00: # 000 - literal
#      print('literal %d' % lx)
      # Copy lx raw bytes from the compressed stream directly to output
      while lx:
        result.append(reader.next())
        lx -= 1
    elif cmd & 0x80: # 1xx - copy
#      print('copy %d' % lx)
      # Read 2-byte backreference offset into the decompressed output buffer
      offs = reader.next() << 8
      offs |= reader.next()
      # Swap byte order if the offset is stored little-endian in this variant
      if not offset_is_be: offs = ((offs >> 8) | (offs << 8)) & 0xffff
      # Copy lx bytes from the already-decompressed output at the given offset
      while lx:
        result.append(result[offs])
        offs += 1
        lx -= 1
    elif (cmd & 0x40) == 0: # 00x - memset
#      print('memset %d' % lx)
      # Read one byte and repeat it lx times (run-length encoding)
      b = reader.next()
      while lx:
        result.append(b)
        lx -= 1
    elif (cmd & 0x20) == 0: # 010 - memset16
#      print('memsetw %d' % lx)
      # Read two bytes and alternate them for lx total output bytes
      b1, b2 = reader.next(), reader.next()
      while lx:
        result.append(b1)
        # If only 1 byte remains, do not append the second byte
        if lx==1: break
        result.append(b2)
        lx -= 2
    else: # 011 - incr
#      print('incr %d' % lx)
      # Read one seed byte, output it, then output (seed+1), (seed+2), etc.
      # Wraps around at 0xFF to 0x00 via the & 0xff mask
      b = reader.next()
      while lx:
        result.append(b)
        b = (b + 1) & 0xff
        lx -= 1


# decode_brr — Decode BRR (Bit Rate Reduction) audio data to signed 16-bit PCM.
#
# BRR is the native audio compression format of the SNES SPC700 sound chip.
# Each BRR block is 9 bytes: 1 header byte + 8 data bytes encoding 16 samples.
#
# Header byte layout:
#   Bits 7-4: shift amount (0-12 valid; 13-15 produce a special -2048/0 output)
#   Bits 3-2: filter mode (0-3), selects the adaptive prediction formula
#   Bit  1:   loop flag (marks this block as a loop point for the SPC700)
#   Bit  0:   end flag (1 = this is the last block in the sample)
#
# Each data byte contains two 4-bit signed nibbles (high nibble first), giving
# 16 nibbles = 16 samples per block. The nibble is sign-extended to a 4-bit
# signed value, shifted by the shift amount, then run through the selected
# adaptive filter (which uses the two most recent decoded samples as predictors).
#
# Parameters:
#   get_byte: callable that reads a byte at a given offset (0-based)
#   olds:     tuple of (old_sample, older_sample) initial predictor state
# Returns: array.array('h') of signed 16-bit PCM samples
def decode_brr(get_byte, olds = (0, 0)):
  ea=0
  r = []
  # old = previous decoded sample, older = sample before that
  # Both are used as predictors by filters 1-3
  old , older = olds
  while True:
    # Read the 9-byte BRR block header
    cmd = get_byte(ea)

    # Extract shift (bits 7-4) and filter mode (bits 3-2) from the header
    shift = cmd >> 4
    filter = (cmd >> 2) & 3
    #print("shift=%d, filter=%d" % (shift, filter))
    # Decode 16 samples from the 8 data bytes following the header
    for i in range(16):
      # Extract the 4-bit nibble: even indices use the high nibble (bits 7-4),
      # odd indices use the low nibble (bits 3-0). Each data byte at
      # ea+1+i//2 holds two nibbles.
      t = (get_byte(ea+1+i//2) >> (0 if i & 1 else 4)) & 0xf
      # Sign-extend the 4-bit nibble: bits 0-2 are magnitude, bit 3 is sign
      s = (t & 7) - (t & 8)
      # Apply the shift amount to scale the sample. Valid shifts are 0-12.
      if shift <= 12:
        # Shift left by 'shift' then divide by 2 (shift right 1) — this is
        # the standard BRR amplitude scaling
        s = ((s << shift) >> 1)
      else:
        # Invalid shift values (13-15): clamp to either -2048 or 0 depending
        # on the sign bit. This matches real SPC700 hardware behavior.
        s = (s >> 3) << 12 # -2048 or 0

      # Apply the adaptive prediction filter using previous samples.
      # Filter 0: no prediction (raw shifted value)
      # Filters 1-3: add weighted combinations of old and older samples,
      # with coefficients chosen to approximate common audio waveforms.
      # The fractional shifts (>>4, >>5, >>6) implement fixed-point arithmetic.
      if filter == 1:
        # Filter 1: s += old * 15/16 (approximated with integer math)
        s += old*1+((-old*1) >> 4)
      elif filter == 2:
        # Filter 2: s += old * 61/32 - older * 15/16
        s += old*2 + ((-old*3) >> 5)  - older + ((older*1) >> 4)
      elif filter == 3:
        # Filter 3: s += old * 115/64 - older * 13/16
        s += old*2 + ((-old*13) >> 6) - older + ((older*3) >> 4)
      # saturate to 16 bits
      # Clamp the sample to the signed 16-bit range [-32768..32767]
      if s < -0x8000: s = -0x8000
      elif s >= 0x7fff: s = 0x7fff
      # wrap to 15 bits
      # The SPC700 wraps the output to a 15-bit signed range [-16384..16383]
      # by masking to 15 bits and sign-extending bit 14
      s = (s & 0x3fff) - (s & 0x4000)

      # Shift the predictor history: current sample becomes 'old',
      # previous 'old' becomes 'older'
      older, old = old, s
      #print('%d: 0x%x -> %d (shift %d, filter %d)' % (i, t, s*2, shift, filter))
      # Store the final sample doubled to fill the 16-bit range
      # (since BRR internally works in 15-bit precision)
      r.append(s*2)
    # Advance past the 9-byte BRR block (1 header + 8 data bytes)
    ea += 9
    # Bit 0 of the header is the end flag — stop decoding after this block
    if cmd & 1:
      break
  # Return as a typed array of signed 16-bit integers for efficient PCM handling
  return array.array('h', r)

# kBrrFilters — Lookup table of the 4 BRR adaptive prediction filter functions.
# Each lambda takes (old_sample, older_sample) and returns the prediction value.
# These are the same formulas used in decode_brr and by the SPC700 hardware.
# Filter 0: no prediction
# Filter 1: ~15/16 * old
# Filter 2: ~61/32 * old - ~15/16 * older
# Filter 3: ~115/64 * old - ~13/16 * older
kBrrFilters = [
  lambda old, older: 0,
  lambda old, older: old*1+((-old*1) >> 4),
  lambda old, older: old*2 + ((-old*3) >> 5)  - older + ((older*1) >> 4),
  lambda old, older: old*2 + ((-old*13) >> 6) - older + ((older*3) >> 4)
]

# brr_get_one — Decode a single BRR sample given the filter prediction,
# the raw signed nibble value, and the shift amount.
# Parameters:
#   old: the accumulated filter prediction value for this sample
#   rs:  the sign-extended 4-bit nibble value
#   r:   the shift amount from the BRR block header (0-12 valid)
# Returns: the decoded sample value, clamped to 16 bits then wrapped to 15 bits
def brr_get_one(old, rs, r):
  # Apply shift and add filter prediction, with the same shift>12 special case
  s = (rs << r) >> 1 if r <= 12 else (rs >> 3) << 12
  s += old
  # Saturate to signed 16-bit range [-32768..32767]
  s = -0x8000 if s < -0x8000 else 0x7fff if s > 0x7fff else s
  # Wrap to 15-bit signed range, matching SPC700 hardware behavior
  return (s & 0x3fff) - (s & 0x4000) # wrap to 15 bits

# encode_brr_generic — Encode signed 16-bit PCM samples into BRR format.
#
# This is the inverse of decode_brr: it takes PCM audio and produces BRR blocks.
# For each block of 16 samples, the encoder tries every combination of filter
# (0-3) and shift (1-12) to find the one that minimizes encoding error.
#
# The nibble search tries all 16 possible 4-bit values (-8 to +7) in a
# heuristic order (starting from 0, then expanding outward) and picks the
# one that produces the smallest squared error against the target sample.
#
# Parameters:
#   data:       list/array of signed 16-bit PCM samples (length must be a
#               multiple of 16, since BRR encodes 16 samples per block)
#   brr_repeat: if nonzero, set the loop flag in each block's header byte
#   olds:       tuple of (old_sample, older_sample) initial predictor state
#   lossless:   if True, require zero encoding error (break early if any
#               sample cannot be perfectly encoded with the current filter/shift)
# Returns: list of integers representing the raw BRR byte stream
def encode_brr_generic(data, brr_repeat, olds = (0, 0), lossless=True):
  assert len(data) % 16 == 0
  # loop_enabled: bit 1 in the BRR header, indicates the sample loops
  loop_enabled, loop_offset = 1 if brr_repeat != 0 else 0, 0
  result = []
  # Working buffers: blk_data holds the 16 best nibbles for the current block,
  # best_data holds the 9-byte BRR block (header + 8 packed nibble bytes)
  blk_data = [0] * 16
  best_data = [0] * 9
  p = 0
  # Track predictor state across blocks for continuity
  best_old, best_older = olds
  while p < len(data):
#    print(p)
    # Initialize error threshold to a very large value so any real encoding wins
    best_err = 1 << 60
    # Save the predictor state at block start so each filter/shift trial
    # can reset to the same starting conditions
    startold, startolder = best_old, best_older

    # Fast path: if all 16 samples in this block are zero, emit a zeroed
    # BRR block directly without searching through filter/shift combinations
    if all(data[p + i] == 0 for i in range(16)):
      # Emit header byte with only the loop flag set, plus 8 zero data bytes
      result.extend((loop_enabled * 2, 0, 0, 0, 0, 0, 0, 0, 0))
      p += 16
      continue
    # Brute-force search: try every filter (0-3) and shift (12 down to 1),
    # keeping the combination that produces the lowest total squared error
    for filter in range(4):
      # Filters 1-3 depend on previous samples, so skip them at the very
      # start of the data or at a loop restart point where history is invalid
      if filter != 0 and (p == 0 or p == loop_offset):
        continue
      # Try shifts from high (coarse) to low (fine). Shift 0 is not tried
      # because it would produce near-zero output for most inputs.
      for r in range(12, 0, -1):
        blk_err = 0
        # Reset predictor state for this trial
        old, older = startold, startolder
        for i in range(16):
          # Compute the filter prediction for this sample position
          s = kBrrFilters[filter](old, older)
          # Divide the target sample by 2 because BRR works in 15-bit precision
          xs = data[p + i] >> 1
          best_e = 1<<60
          # Try all 16 possible 4-bit signed nibble values in a heuristic
          # order that starts near zero and spirals outward, so we hit
          # exact matches early and can break immediately on zero error
          for j in (0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, -8):
            # Decode what this nibble would produce given the current shift
            # and filter prediction
            s0 = brr_get_one(s, j, r)
            # Compute squared error between decoded sample and target
            e = (xs - s0) * (xs - s0)
            if e < best_e:
              best_e, best_j, best_s0 = e, j, s0
              # Perfect match — no need to try remaining nibble values
              if e == 0:
                #print(j)
                break
          # In lossless mode, if any sample in this block has nonzero error,
          # this filter/shift combination is unsuitable — abandon it
          if best_e != 0 and lossless:
            break
          blk_err += best_e
          # Store the best nibble (masked to 4 bits) for this sample
          blk_data[i] = best_j & 0xf
          # Update predictor history for the next sample in this trial
          older, old = old, best_s0
        else:
          # The for-loop completed without break — all 16 samples were encoded.
          # If this combination has lower error than the best so far, adopt it.
          if blk_err < best_err:
            best_err = blk_err
            best_old, best_older = old, older
            # Pack the BRR header byte: shift(4 bits) | filter(2 bits) | loop(1 bit)
            # Bit 0 (end flag) is left clear here; it is set externally if needed
            best_data[0] = r << 4 | filter << 2 | loop_enabled << 1
            # Pack pairs of 4-bit nibbles into 8 data bytes (high nibble first)
            for i in range(8):
              best_data[i + 1] = blk_data[i * 2] << 4 | blk_data[i * 2 + 1]
    result.extend(best_data)
    # In lossless mode, assert that perfect encoding was achieved for this block
    if lossless: assert best_err==0
    p += 16
#  result[-9] |= 1
  return result
