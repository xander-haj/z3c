# =============================================================================
# compile_music.py — SPC700 Music Compiler for zelda3
# =============================================================================
# This file reads human-readable music text files (sound_intro.txt,
# sound_indoor.txt, sound_ending.txt, sfx.txt) and compiles them into
# SPC700 binary format for the SNES audio coprocessor.
#
# The SPC700 has a 64KB address space. Music data is organized in a
# strict hierarchy:
#   SongList -> Song -> Phrase -> Pattern -> note/effect commands
#
# - A SongList is a table of pointers to Songs (one per music track).
# - A Song is an ordered sequence of Phrases, with optional loop points.
# - A Phrase maps exactly 8 channels to Patterns (one Pattern per channel).
# - A Pattern is a sequence of note events and effect commands that drive
#   a single audio channel.
#
# The compiler works in three stages:
#   1. Parse: Read text files into typed Python objects (Song, Phrase, etc.)
#   2. Serialize: Write each object into a flat 64KB memory image at its
#      designated SPC700 address, recording relocations for cross-references.
#   3. Patch: Resolve all relocations (backpatch address pointers) and
#      produce the final loadable binary as (length, addr, data) blocks.
#
# The text format uses bracketed headers like [Song_0x2b00] where the
# _0x suffix encodes the target SPC700 address for that object.
# =============================================================================

# Standard library imports for binary data, CLI args, regex parsing
import array
import sys
import yaml
import re
# Project-local utility module shared across asset compilation scripts
import util

# Represents a single music track (song) in the SPC700 data hierarchy.
# A Song contains an ordered list of Phrases, which may include PhraseLoop
# entries for looping sections. The 'ea' attribute holds the target address
# in SPC700 memory where this song's data will be written.
class Song:
  name = 'Song'
  # Returns a human-readable representation showing the song's SPC address,
  # index, and the names of all phrases it contains (used for debugging).
  def __str__(self):
    s = '[Song_0x%x idx=%d]\n' % (self.ea, self.index)
    s += "".join(x.name + '\n' for x in self.phrases)
    return s

# Represents a Phrase: a mapping of 8 SPC700 audio channels to Patterns.
# Each Phrase holds exactly 8 Pattern references, one per channel. When
# serialized, the Phrase becomes 8 consecutive 16-bit pointers to Patterns.
class Phrase:
  name = 'Phrase'
  def __str__(self):
    s = '[%s]' % (self.name)
    return s

# Represents a loop control directive within a Song's phrase list.
# Instead of referencing a Phrase, a PhraseLoop tells the SPC700 engine to
# jump back to an earlier phrase in the song and repeat it a specified
# number of times.
class PhraseLoop:
  name = 'PhraseLoop'
  # loops: how many times to repeat the loop (0 = infinite loop)
  # jmp: the index offset (in phrases) to jump back to within the song
  def __init__(self, loops, jmp):
    self.loops = loops
    self.jmp = jmp
    self.name = 'PhraseLoop %d %d' % (self.loops, self.jmp)
  def __str__(self):
    return self.name

# Represents a music Pattern: a sequence of note events and effect commands
# for a single audio channel. Each Pattern is referenced by a Phrase. When
# serialized, notes become 0x80|note_value bytes, and effects become
# 0xE0+effect_index followed by parameter bytes. A zero byte terminates
# the pattern unless 'fallthrough' is set, which causes execution to
# continue directly into the next pattern in memory.
class Pattern:
  name = 'Pattern'

# Represents a sound effect (SFX) pattern. SFX patterns use a different
# binary encoding than music patterns: they support SetInstrument (0xE0),
# Restart (0xFF), PitchSlide (0xF1/0xF9), and note commands. Terminated
# by a zero byte. Used by the SPC700 sound effect playback engine.
class SfxPattern:
  name = 'SfxPattern'

# Represents a SongList: a table of pointers to Song objects. The SPC700
# engine indexes into a SongList to select which song to play. Serialized
# as consecutive 16-bit addresses pointing to Song data.
class SongList:
  name = 'SongList'

# Represents a SfxList: a table of SFX pattern pointers, followed by
# "next" bytes (chaining info for multi-part SFX) and optional echo
# enable flags. Used by the SPC700 SFX engine to look up and play
# sound effects by index.
class SfxList:
  name = 'SfxList'

# Global registry mapping text names (e.g., "Song_0x2b00", "Pattern_0x3100")
# to their corresponding typed Python objects. This acts as a symbol table:
# when a text file references an object by name, this dict resolves it to
# the actual object. The _0x suffix in each name encodes the target SPC700
# address where that object should be placed in the 64KB memory image.
types_for_name = {}

# Looks up or creates a typed object in the global types_for_name registry.
# nam: the text name (e.g., "Song_0x2b00"); tp: the expected Python class;
# is_create: True when defining the object (first encounter), False when
# referencing it. If the name contains "_0x", the hex suffix is parsed as
# the SPC700 target address and stored in obj.ea. Returns None for "None".
# Raises if the object is defined twice or if the type mismatches a prior ref.
def get_type_for_name(nam, tp, is_create):
  # "None" is a sentinel used for empty/null pattern slots in phrases
  if nam == 'None':
    assert not is_create
    return None
  a = types_for_name.get(nam)
  if a != None:
    # Object was already forward-referenced; now mark it as defined
    if is_create:
      if a.defined:
        raise Exception('%s already defined' % nam)
      a.defined = True
    # Type-safety check: ensure the referenced name matches expected type
    assert type(a) == tp, (nam, type(a), tp)
    return a
  # First encounter of this name: create a new object and register it
  a = tp()
  a.name = nam
  a.defined = is_create
  types_for_name[nam] = a
  # Parse the SPC700 target address from the _0x suffix in the name.
  # For example, "Song_0x2b00" yields ea = 0x2b00.
  a.ea = None
  if '_0x' in nam:
    a.ea = int(nam[nam.index('_0x') + 3:], 16)
  return a

# Parses a [Song_0x...] block from the text file. Each line in the block is
# either a Phrase reference (by name) or a "PhraseLoop <count> <offset>"
# directive. Returns a Song object with its phrase list populated.
def process_song(caption, args, lines):
  song = get_type_for_name(caption, Song, True)
  song.phrases = []
  for line in lines:
    line_cmd, *line_args = line.split(' ')
    # PhraseLoop is a loop-back directive, not a reference to a Phrase object
    if line_cmd == 'PhraseLoop':
      song.phrases.append(PhraseLoop(int(line_args[0]), int(line_args[1])))
    else:
      # Regular phrase reference: look up (or forward-declare) the Phrase
      song.phrases.append(get_type_for_name(line_cmd, Phrase, False))
  return song

# Parses a [SongList_0x...] block. Each line is a Song name reference.
# The resulting SongList holds an ordered list of Song pointers that the
# SPC700 engine indexes into to select a track for playback.
def process_song_list(caption, args, lines):
  song_list = get_type_for_name(caption, SongList, True)
  song_list.songs = [get_type_for_name(line, Song, False) for line in lines]
  return song_list

# Parses a [Phrase_0x...] block. Exactly 8 lines are expected, each naming
# a Pattern (or "None" for an unused channel). The SPC700 has 8 audio
# channels, so each Phrase maps one Pattern per channel.
def process_phrase(caption, args, lines):
  phrase = get_type_for_name(caption, Phrase, True)
  assert len(lines)==8
  phrase.patterns = [get_type_for_name(lines[i], Pattern, False) for i in range(8)]
  return phrase

# Number of parameter bytes following each effect command in SPC700 binary.
# Indexed by effect number (0-26). For example, Instrument (index 0) takes
# 1 byte (the instrument number), Vibrato (index 3) takes 3 bytes, etc.
# A value of 0 means the effect takes no parameters (just the 0xE0+index byte).
kEffectByteLength = [1, 1, 2, 3, 0, 1, 2, 1, 2, 1, 1, 3, 0, 1, 2, 3, 1, 3, 3, 0, 1, 3, 0, 3, 3, 3, 1]
# Human-readable names for the 27 SPC700 effect commands, in order of their
# binary encoding. Effect N is encoded as byte 0xE0+N in the pattern data.
kEffectNames = ['Instrument', 'Pan', 'PanFade', 'Vibrato', 'VibratoOff',
                'SongVolume', 'SongVolumeFade', 'Tempo', 'TempoFade',
                'Transpose', 'ChannelTranpose', 'Tremolo', 'TremoloOff',
                'Volume', 'VolumeFade', 'Call', 'VibratoFade',
                'PitchEnvelopeTo', 'PitchEnvelopeFrom', 'PitchEnvelopeOff',
                'FineTune', 'EchoEnable', 'EchoOff', 'EchoSetup', 'EchoVolumeFade',
                'PitchSlide', 'PercussionDefine']
# Reverse lookup: effect name -> index (used during parsing to identify effects)
kEffectNamesDict = {a:i for i, a in enumerate(kEffectNames)}
# The 12 chromatic note names used in the text format (- means natural, # means sharp)
kKeys  = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-']
# Maps note+octave strings (e.g., "C-1", "G#3") to SPC700 note values (0-71)
# across 6 octaves. Each octave has 12 semitones, so octave j starts at j*12.
kKeysDict = {a+str(j+1):i+j*12 for i, a in enumerate(kKeys) for j in range(6)}
# Special note values: -+- (72) = tie/rest, --- (73) = release/key-off
kKeysDict['-+-'] = 72
kKeysDict['---'] = 73

# Parses a [Pattern_0x...] block into a Pattern object. Each line is one of:
#   - A note event: "<key> <length|--> <volume_hex|-->" (e.g., "C-3 12 7f")
#   - An effect command: "<EffectName> <params...>" (e.g., "Instrument 5")
#   - "Call <PatternName> <count>": subroutine call to another pattern
#   - "Fallthrough": no terminator byte; continues into the next pattern
# Each parsed line is stored as a tuple: (cmd, args, note_length, volstuff).
# For notes, note_length and volstuff are the parsed numeric values (or None
# if "--" meaning "keep previous value"). For effects, both are None.
def process_pattern(caption, args, lines):
  pattern = get_type_for_name(caption, Pattern, True)
  pattern.lines = []
  pattern.fallthrough = False
  for line in lines:
    # No commands may follow a Fallthrough directive
    assert not pattern.fallthrough
    line_cmd, *line_args = line.split()
    if line_cmd == 'Call':
      # Call is a subroutine jump: references another Pattern and a loop count
      assert len(line_args) == 2
      pattern.lines.append(('Call', (get_type_for_name(line_args[0], Pattern, False),int(line_args[1]) ), None, None))
    elif line_cmd == 'Fallthrough':
      # Marks this pattern as having no terminating zero byte
      pattern.fallthrough = True
    elif line_cmd in kEffectNamesDict:
      # Effect command: convert all arguments to integers
      line_args = [int(a) for a in line_args]
      pattern.lines.append((line_cmd, line_args, None, None))
    elif line_cmd in kKeysDict:
      # Note event: parse optional note length and volume/quantization byte
      assert len(line_args) == 2, line_args
      # "--" means "no change from previous value" (omitted from binary output)
      note_length = None if line_args[0] == '--' else int(line_args[0])
      volstuff = None if line_args[1] == '--' else int(line_args[1], 16)
      pattern.lines.append((line_cmd, line_args, note_length, volstuff))
    else:
      assert 0, repr(line_cmd)
  return pattern

# Parses a [Sfx_0x...] block. Unlike music patterns, SFX patterns are stored
# as raw text lines and only parsed during serialization (in write_sfx_pattern),
# because SFX encoding is more complex and context-dependent.
def process_sfx_pattern(caption, args, lines):
  pattern = get_type_for_name(caption, SfxPattern, True)
  pattern.lines = lines
  return pattern

# Serializes an SFX pattern into the 64KB memory image. SFX encoding differs
# from music patterns: notes can carry variable-length parameter prefixes
# (duration, ADSR, gain bytes), and PitchSlide uses special opcodes (0xF1
# for continuation slides, 0xF9 for note-triggered slides). "." represents
# a continuation/tied note with no new pitch. Restart (0xFF) loops the SFX.
# serializer: the Serializer writing into the 64KB image
# o: the SfxPattern object whose raw text lines are parsed here
def write_sfx_pattern(serializer, o):
#  print('# Creating %s (%x)' % ( o.name, serializer.addr))
  for i,line in enumerate(o.lines):
#    print(line)
    line_cmd, *line_args = re.split(r' +', line)
    # 0xE0 prefix byte selects the SFX instrument
    if line_cmd == 'SetInstrument':
      assert len(line_args) == 1
      serializer.write([0xe0, int(line_args[0])])
    elif line_cmd == 'Restart':
      # 0xFF tells the SPC700 to loop the SFX from the beginning
      serializer.write([0xff])
      assert i == len(o.lines) - 1
      return
    elif line_cmd == 'Fallthrough':
      # No terminator: execution continues into the next SFX pattern in memory
      assert i == len(o.lines) - 1
      return
    elif line_cmd in kKeysDict or line_cmd == '.':
      # SFX note event: optional prefix bytes for duration, ADSR, and gain
      # are written only when they differ from the previous note ("--"/"---"
      # means "keep current value"). The prefix bytes are variable-length:
      # the SPC700 engine reads them based on context.
      if line_args[0] != '--':
        serializer.write((int(line_args[0]),))
        if line_args[1] != '---':
          serializer.write((int(line_args[1]),))
          if line_args[2] != '---':
            serializer.write((int(line_args[2]),))
      if len(line_args) >= 4:
        # PitchSlide appends slide parameters after the note byte
        assert line_args[3] == 'PitchSlide'
        if line_cmd == '.':
          # 0xF1: pitch slide continuation (no new note trigger)
          serializer.write([0xf1, int(line_args[4]), int(line_args[5]), int(line_args[6])])
        else:
          # 0xF9: pitch slide with a new note trigger (note | 0x80)
          serializer.write([0xf9, kKeysDict[line_cmd] | 0x80, int(line_args[4]), int(line_args[5]), int(line_args[6])])
      else:
        # Standard note byte: high bit set (0x80) to distinguish from commands
        serializer.write([kKeysDict[line_cmd] | 0x80])
    else:
      assert 0, repr(line_cmd)
  # Zero byte terminates the SFX pattern
  serializer.write([0])

# Parses a [SfxPort...] block. Each line is a comma-separated tuple:
#   <SfxPatternName>,<next_index>[,<echo_flag>]
# 'next' is a chaining byte that links multi-part SFX sequences together.
# 'echo' is an optional flag controlling the SPC700 echo effect per SFX.
# Echo entries must either be present for all lines or absent for all.
def process_sfx_list(caption, args, lines):
  sfx_list = get_type_for_name(caption, SfxList, True)
  sfx_list.patterns = []
  sfx_list.next = []
  sfx_list.echo = []
  for line in lines:
    r = line.split(',')
    sfx_list.patterns.append(get_type_for_name(r[0], SfxPattern, False))
    sfx_list.next.append(int(r[1]))
    if len(r) >= 3:
      sfx_list.echo.append(int(r[2]))
  # Echo flags must be all-or-nothing: either every entry has one or none do
  assert len(sfx_list.echo) in (0, len(lines))
  return sfx_list

# Serializes an SfxList into binary: first writes 16-bit relocatable pointers
# to each SFX pattern, then the "next" chaining bytes, then the echo flags.
# serializer: the Serializer writing into the 64KB image
# o: the SfxList object to serialize
def write_sfx_list(serializer, o):
  # Write pointer table: each entry is a 16-bit address to an SfxPattern
  for line in o.patterns:
    serializer.write_reloc_entry(line)
  # Write chaining bytes: index of the next SFX to play in sequence (0 = none)
  for next in o.next:
    serializer.write([next])
  # Write echo enable flags (1 = enable echo for this SFX, 0 = disable)
  for i in o.echo:
    serializer.write([i])

# Addresses where non-contiguous data regions begin in SPC700 memory.
# The SPC700 memory layout has gaps between regions (e.g., sample directory,
# instrument tables, music data, SFX data). When the serializer encounters
# an object whose target address matches one of these, it jumps to that
# address instead of asserting contiguity with the previous write position.
kGapStartAddrs = (0x2b00, 0x2880, 0xd000)

# The Serializer manages a 64KB memory image representing the SPC700's full
# address space. It writes compiled binary data at tracked positions, records
# relocations (16-bit address references that need backpatching after all
# objects are placed), and provides helpers for writing words, patterns,
# songs, phrases, and song lists. After all objects are written, process_relocs()
# patches every recorded relocation with the final write address of its target.
class Serializer:
  # Initializes a blank 64KB memory image (None = unwritten byte), an empty
  # relocation list, and no current write address.
  def __init__(self):
    self.memory = [None] * 65536
    self.relocs = []
    self.addr = None

  # Writes a sequence of bytes at the current address, advancing addr.
  # Asserts that each target byte has not already been written (no overlap).
  def write(self, data):
    for d in data:
      assert self.memory[self.addr] == None, '0x%x' % self.addr
      self.memory[self.addr] = d
      self.addr += 1

  # Writes bytes at an arbitrary absolute address (does not update self.addr).
  # Used for writing to fixed memory-mapped locations like instrument tables.
  def write_at(self, a, data):
    for d in data:
      self.memory[a] = d
      a += 1

  # Writes a 16-bit little-endian word at absolute address 'a'.
  # The SPC700 is little-endian: low byte first, high byte second.
  def write_word(self, a, v):
    self.memory[a] = v & 0xff
    self.memory[a + 1] = v >> 8 & 0xff
      

  # Writes a 16-bit relocation placeholder (two zero bytes) at the current
  # address, and records a relocation entry so that process_relocs() can
  # backpatch this location with the actual address of 'r' once all objects
  # are placed. If r is None/falsy (e.g., an unused channel), only the
  # placeholder zeros are written with no relocation recorded.
  # r: the target object whose final write_addr will replace these zeros
  def write_reloc_entry(self, r):
    self.write([0, 0])
    if r:
      self.relocs.append((self.addr - 2, r))
    

  # Serializes a Song as a sequence of 16-bit phrase pointers into the memory
  # image. Each phrase reference becomes a relocatable 16-bit address.
  # PhraseLoop entries are inlined as a (loop_count, 0, addr_lo, addr_hi)
  # 4-byte sequence where the address points back into this same song's
  # phrase table. A final (0, 0) word terminates the song.
  # song: the Song object containing an ordered list of Phrase/PhraseLoop items
  def write_song(self, song):
    for phrase in song.phrases:
      if isinstance(phrase, PhraseLoop):
        # Calculate the absolute target address for the loop jump:
        # jmp is a signed offset in phrase-slots (each slot = 2 bytes)
        i = self.addr + phrase.jmp * 2
        # Write loop count byte followed by a zero padding byte
        self.write([phrase.loops, 0])
        # Write the 16-bit jump target address in little-endian format
        self.write([i & 0xff, i >> 8])
      else:
        # Normal phrase reference: write relocatable 16-bit pointer
        self.write_reloc_entry(phrase)
    # Zero word terminates the song's phrase list
    self.write([0, 0])

  # Serializes a Phrase as 8 consecutive 16-bit relocatable pointers, one per
  # SPC700 audio channel. Each pointer targets the Pattern assigned to that
  # channel (or writes a null pointer if the channel is unused/None).
  # phrase: the Phrase object with a .patterns list of exactly 8 Pattern refs
  def write_phrase(self, phrase):
    for i in range(8):
      self.write_reloc_entry(phrase.patterns[i])

  # Serializes a SongList as consecutive 16-bit relocatable pointers to Songs.
  # The SPC700 engine uses this table to look up songs by index number.
  # song_list: the SongList object containing an ordered list of Song refs
  def write_song_list(self, song_list):
    for song in song_list.songs:
      self.write_reloc_entry(song)
      
  # Serializes a music Pattern into binary SPC700 format. Each parsed line is
  # emitted as: optional note_length byte, optional volume/quantization byte,
  # then the command byte(s). Notes become 0x80|key_value. Effects become
  # 0xE0+effect_index followed by parameter bytes. Call commands emit 0xEF
  # plus a relocatable 16-bit address and a loop count. A zero byte terminates
  # the pattern unless fallthrough is set (pattern continues into next in memory).
  # patt: the Pattern object with a .lines list of (cmd, args, length, vol) tuples
  def write_pattern(self, patt):
    for cmd, args, note_length, volstuff in patt.lines:
      #print(cmd, args, note_length, volstuff)
      # Emit optional duration byte (only when the note changes duration)
      if note_length != None: self.write((note_length, ))
      # Emit optional volume/quantization byte (only when volume changes)
      if volstuff != None: self.write((volstuff, ))
      if cmd in kKeysDict:
        # Note command: set high bit to distinguish notes from effect opcodes
        self.write((0x80 | kKeysDict[cmd], ))
      elif cmd == 'Call':
        # Subroutine call: 0xEF opcode + 16-bit target address + loop count.
        # The address bytes are initially zero; a relocation entry records
        # the position so process_relocs() can backpatch the real address.
        self.write([0xef, 0, 0, int(args[1])])
        self.relocs.append((self.addr - 3, args[0]))
      elif cmd in kEffectNamesDict:
        # Effect command: encode as 0xE0 + effect index, followed by the
        # effect's parameter bytes (count determined by kEffectByteLength)
        i = kEffectNamesDict[cmd]
        assert len(args) == kEffectByteLength[i]
        self.write([0xe0 + i])
        self.write(args)
    # Terminate pattern with a zero byte unless fallthrough is enabled,
    # in which case execution continues directly into the next pattern
    if not patt.fallthrough:
      self.write((0, ))

  # Writes a single music object into the 64KB memory image at the correct
  # SPC700 address. Handles address cursor management: if the object's target
  # address (what.ea) matches a known gap boundary in kGapStartAddrs, the
  # cursor jumps there; otherwise it must be contiguous with the previous
  # write position (no unintended overlap or gap). Dispatches to the
  # type-specific writer based on the object's class.
  # what: any music object (Phrase, Pattern, Song, SongList, SfxPattern, SfxList)
  def write_obj(self, what):
 #   print(what.name, self.addr, what.ea)
 #   print('Writing %s at 0x%x. Curr pos 0x%x' % (what.name, what.ea, 0 if self.addr == None else self.addr))
    if what.ea != None:
      # Jump the write cursor to the object's designated address if this is
      # the first object (addr is None) or the address is a known gap boundary
      if self.addr == None or what.ea in kGapStartAddrs:# or True:#what.ea >= self.addr:
        self.addr = what.ea
      elif what.ea != self.addr:
        # Address mismatch: object expects a different address than current
        # cursor position, indicating a layout or ordering error
        print('%s: 0x%x != 0x%x' % (what.name, what.ea, self.addr))
        assert(0)

    # Record the actual write address for relocation resolution later
    what.write_addr = self.addr
    assert self.addr != None
    # Dispatch to the appropriate writer based on the object's type
    if isinstance(what, Phrase):
      self.write_phrase(what)
    elif isinstance(what, Pattern):
      self.write_pattern(what)
    elif isinstance(what, Song):
      self.write_song(what)
    elif isinstance(what, SongList):
      self.write_song_list(what)
    elif isinstance(what, SfxPattern):
      write_sfx_pattern(self, what)
    elif isinstance(what, SfxList):
      write_sfx_list(self, what)
    else:
      # Unknown object type: fatal error in the data pipeline
      print(type(what))
      assert(0)

  # Fills a range of addresses [a, b) with zero bytes in the memory image.
  # Used to zero-pad gaps between data sections so the SPC700 reads
  # deterministic silence/null data instead of undefined memory.
  # a: start address (inclusive), b: end address (exclusive)
  def write_zeros(self, a, b):
    while a < b:
      self.memory[a] = 0
      a += 1

  # Resolves all recorded relocations by writing the actual 16-bit addresses
  # into the placeholder slots left by write_reloc_entry() and Call commands.
  # Must be called after all objects are serialized so that every target's
  # write_addr is finalized. Each relocation is a (placeholder_addr, target_obj)
  # pair; the target's write_addr is written as a little-endian 16-bit value.
  def process_relocs(self):
    for p, r in self.relocs:
      # Patch the two placeholder bytes with the target's final address
      self.memory[p + 0] = r.write_addr & 0xff
      self.memory[p + 1] = r.write_addr >> 8

# Parses a music text file into a list of typed objects (Song, Phrase, Pattern,
# SongList, SfxPattern, SfxList). The text format uses INI-style section
# headings like [Song_0x2b00] followed by indented data lines. Blank lines
# and lines starting with '#' are ignored. Each section is dispatched to
# the appropriate process_* function based on the heading prefix.
# file: an open file handle (or iterable of lines) for the music text file
# Returns: ordered list of parsed music objects, preserving file order
def process_file(file):
  sorted_ents = []
  # Inner function: takes a completed section (heading + collected lines)
  # and dispatches to the correct parser based on the heading prefix
  def add_collect(heading, collect):
    # Strip brackets and split into caption name and optional arguments
    caption, *args = heading.strip('[]').split(' ', 1)
    # Dispatch to the type-specific parser based on object type prefix
    if caption.startswith('Song_'):
      r = process_song(caption, args, collect)
    elif caption.startswith('Phrase_'):
      r = process_phrase(caption, args, collect)
    elif caption.startswith('Pattern_'):
      r = process_pattern(caption, args, collect)
    elif caption.startswith('SongList_'):
      r = process_song_list(caption, args, collect)
    elif caption.startswith('Sfx_'):
      r = process_sfx_pattern(caption, args, collect)
    elif caption.startswith('SfxPort'):
      r = process_sfx_list(caption, args, collect)
    else:
      assert 0
    sorted_ents.append(r)
  collect = None
  heading = None
  # Stream through the file line by line, grouping lines under headings
  for line in file:
    line = line.strip()
    # Skip blank lines and comment lines (prefixed with #)
    if line == '' or line.startswith('#'): continue
    if line.startswith('['):
      # New section heading encountered: flush the previous section first
      if heading != None:
        add_collect(heading, collect)
      heading = line
      collect = []
      #
      #print(caption, args)
    else:
      # Accumulate data lines under the current section heading
      collect.append(line.strip('\n'))
  # Flush the final section after reaching end-of-file
  if heading != None:
    add_collect(heading, collect)
  return sorted_ents

# Main serialization driver: writes all music objects for a given song bank
# into the 64KB SPC700 memory image. For the 'intro' bank (the primary bank),
# this also writes BRR audio samples starting at 0x4000, sets up the sample
# directory at 0x3C00, the instrument table at 0x3D00, and the SFX instrument
# table at 0x3E00. After writing all objects, validates that every referenced
# symbol was defined, then resolves all relocations.
# serializer: the Serializer managing the 64KB memory image
# song: bank name string ('intro', 'indoor', or 'ending')
# sorted_ents: list of parsed music objects, sorted by SPC700 target address
def serialize_song(serializer, song, sorted_ents):
  # The 'intro' bank is the primary bank that contains shared audio data:
  # BRR samples, sample directory, instrument definitions, and SFX instruments
  if song == 'intro':
#    serializer.write_zeros(0x2a8b, 0x2b00)
#    serializer.write_zeros(0x3188, 0x4000)
 #   serializer.write_zeros(0xbaa0, 0xd000)
 #   serializer.write_zeros(0xfdae, 0x10000)
 #   serializer.write_zeros(0x2850, 0x2880)
    # BRR (Bit Rate Reduction) samples are packed starting at 0x4000,
    # immediately after the instrument tables that end at 0x3FFF
    serializer.addr = 0x4000
    # Track which sample files have already been written to avoid duplicates
    # (multiple instruments can share the same underlying BRR sample)
    sample_to_addr = {}
    music_info = yaml.safe_load(open('music_info.yaml', 'r'))
    # Write each BRR sample into memory and set up the sample directory
    for i, info in enumerate(music_info['samples']):
      # Only write each unique sample file once; reuse the address for dupes
      if info['file'] not in sample_to_addr:
        sample_to_addr[info['file']] = serializer.addr
        serializer.write(open('%s.brr' % info['file'], 'rb').read())
      addr = sample_to_addr[info['file']]
      # SPC700 sample directory at 0x3C00: each entry is 4 bytes:
      #   bytes 0-1: sample start address (little-endian 16-bit)
      #   bytes 2-3: sample loop point address (little-endian 16-bit)
      serializer.write_word(0x3c00 + i * 4, addr)
      # Loop point: convert byte offset to BRR block index (each BRR block
      # is 9 bytes encoding 16 PCM samples). If no repeat point, use end addr.
      serializer.write_word(0x3c00 + i * 4 + 2, addr + info['repeat'] // 16 * 9 if 'repeat' in info else serializer.addr)
    # Pad remaining sample directory entries with 0xFFFF sentinel values
    # (6 unused slots after the last real sample entry)
    for i in range(6):
      serializer.write_word(0x3c64 + i * 2, 0xffff)

    # Write the music instrument table at 0x3D00. Each instrument is 6 bytes:
    # sample index, ADSR1 (enable|decay|attack), ADSR2 (sustain), gain,
    # and a 16-bit pitch base value (big-endian: high byte first)
    for i, info in enumerate(music_info['instruments']):
      ea = 0x3d00 + i * 6
      serializer.memory[ea + 0] = info['sample']
      # ADSR1 register: bit 7 = ADSR enable, bits 6-4 = decay, bits 3-0 = attack
      serializer.memory[ea + 1] = 0x80 | info['decay'] << 4 | info['attack']
      # ADSR2 register: bits 7-5 = sustain level, bits 4-0 = sustain rate
      serializer.memory[ea + 2] = info['sustain_level'] << 5 | info['sustain_rate']
      serializer.memory[ea + 3] = info['vxgain']
      # Pitch base stored big-endian (high byte at lower address)
      serializer.memory[ea + 4] = info['pitch_base'] >> 8
      serializer.memory[ea + 5] = info['pitch_base'] & 0xff

    # Note gate-off timing table at 0x3D96: controls how early notes release
    serializer.write_at(0x3D96, music_info['note_gate_off'])
    # Note volume table at 0x3D9E: default volume levels per quantization index
    serializer.write_at(0x3D9e, music_info['note_volume'])

    # Write the SFX instrument table at 0x3E00. Each SFX instrument is 9 bytes:
    # left volume, right volume, 16-bit pitch, sample index, ADSR1, ADSR2,
    # gain, and a single-byte pitch base multiplier
    for i, info in enumerate(music_info['sfx_instruments']):
      ea = 0x3e00 + i * 9
      serializer.memory[ea + 0] = info['voll']
      serializer.memory[ea + 1] = info['volr']
      serializer.write_word(ea + 2, info['pitch'])
      serializer.memory[ea + 4] = info['sample']
      serializer.memory[ea + 5] = 0x80 | info['decay'] << 4 | info['attack']
      serializer.memory[ea + 6] = info['sustain_level'] << 5 | info['sustain_rate']
      serializer.memory[ea + 7] = info['vxgain']
      serializer.memory[ea + 8] = info['pitch_base']

  # Reset cursor so write_obj() will set it from each object's target address.
  # Objects are written in address-sorted order to ensure contiguous packing.
  serializer.addr = None
  for e in sorted_ents:
    serializer.write_obj(e)

  if song == 'indoor':
    # The 'indoor' bank references Song_0x2880 which is defined in the 'intro'
    # bank. Manually mark it as defined and set its write address so that
    # relocation resolution does not fail on this cross-bank reference.
    t = types_for_name['Song_0x2880']
    t.defined = True
    t.write_addr = 0x2880


  # Verify that every symbol referenced during parsing was eventually defined.
  # A missing definition means a text file referenced an object that was never
  # declared with its own [Section] heading — likely a typo or missing file.
  for e in types_for_name.values():
    if not e.defined:
      raise Exception('Symbol %s not defined' % e.name)

  # Backpatch all 16-bit relocatable pointers now that every object has a
  # finalized write_addr in the memory image
  serializer.process_relocs()

# Verifies the compiled memory image against an original SPC700 memory dump.
# Compares every byte that was written by the compiler with the corresponding
# byte in the reference .spc file. Bytes left as None (unwritten) are skipped
# and tracked as "undefined ranges" for diagnostic output. Returns True if
# every written byte matches the original, False if any mismatch is found.
# serializer: the Serializer containing the compiled 64KB memory image
# song: bank name string, used to locate the reference file sound/<song>.spc
def compare_with_orig(serializer, song):
  # Collect contiguous ranges of undefined (unwritten) addresses for reporting
  ranges=[]
  ok = True
  spc = open('sound/%s.spc' % song, 'rb').read()
  for i in range(65536):
    if serializer.memory[i] != None:
      # Byte was written by compiler: verify it matches the original dump
      if serializer.memory[i]!= spc[i]:
        print('0x%x: 0x%x != 0x%x' % (i, serializer.memory[i], spc[i]))
        ok = False
    else:
      # Byte was not written: merge into contiguous undefined ranges
#      serializer.memory[i] = spc[i]
      if len(ranges) and ranges[-1][1] == i:
        ranges[-1][1] = i + 1
      else:
        ranges.append([i, i + 1])
  # When run directly (not as a library), print undefined address ranges
  # to help identify which memory regions the compiler does not cover
  if __name__ == "__main__":
    for a, b in ranges:
      print('// undefined %x-%x' % (a, b))
  return ok

# Converts the 64KB memory image into a compact "load sequence" suitable for
# SNES-to-SPC700 data transfer. The format matches the IPL boot protocol:
# each block is (len_lo, len_hi, dst_lo, dst_hi, data...) specifying the
# byte count, destination address, and payload. Blocks are emitted only for
# contiguous runs of defined (non-None) bytes, skipping undefined regions.
# A terminal (0x00, 0x00) length word signals end-of-sequence.
# serializer: the Serializer containing the compiled 64KB memory image
# Returns: flat list of bytes representing the complete load sequence
def produce_loadable_seq(serializer):
  r = []
  # count non zeros
  # Scan through the full 64KB address space, identifying contiguous defined
  # regions and emitting a load block for each one
  start, i = 0, 0
  while start < 0x10000:
    # Advance i past all defined (non-None) bytes to find the end of this block
    while i < 0x10000 and serializer.memory[i] != None:
      i += 1
    j = i
    # Advance i past all undefined (None) bytes to find the start of the next block
    while i < 0x10000 and serializer.memory[i] == None:
      i += 1

    # If no defined bytes were found in this segment, skip to the next one
    if j == start:
      start = i
      continue
    # Emit the block header: 16-bit length and 16-bit destination, little-endian
    r.extend([(j - start) & 0xff, (j - start) >> 8, start & 0xff, start >> 8])
    # Emit the raw byte payload for this contiguous defined region
    r.extend(serializer.memory[start:j])
#    print('copy 0x%x - 0x%x (%d)' % (start, j, j - start))
    start = i
  # Terminate the load sequence with a zero-length block (signals end to the loader)
  r.extend([0, 0])
  return r

# Top-level compiler entry point for a single song bank. Orchestrates the full
# pipeline: parse text files, serialize all objects into the 64KB image, produce
# the loadable sequence, and verify against the original SPC dump. The global
# types_for_name registry is reset so each bank starts with a clean symbol table.
# song: bank name string ('intro', 'indoor', or 'ending')
# Returns: a (name, data) tuple where name is 'kSoundBank_<song>' and data is
#          the flat byte list of the loadable sequence for embedding in the ROM
def print_song(song):
  # Reset the global symbol table so this bank's symbols do not collide
  # with symbols from a previously compiled bank
  global types_for_name
  types_for_name = {}

  serializer = Serializer()

  # Parse the main music text file for this bank
  sorted_ents = process_file(open('sound_%s.txt' % song, 'r'))
  # The 'intro' bank also includes all sound effects from sfx.txt
  if song == 'intro':
    sorted_ents.extend(process_file(open('sfx.txt' , 'r')))

  # Sort all parsed objects by their target SPC700 address so they are
  # written in ascending order, ensuring contiguous memory layout
  serialize_song(serializer, song, sorted(sorted_ents, key = lambda x: x.ea))

  # Convert the populated memory image into the SNES transfer format
  r = produce_loadable_seq(serializer)

  # Build the result tuple with a C-style constant name for code generation
  result = 'kSoundBank_%s' % song, r

  # Byte-for-byte verification against the original SPC memory dump ensures
  # the compiler produces bit-identical output to the original game data
  if not compare_with_orig(serializer, song):
    raise Exception('compare failed')

  return result

# CLI entry point: compiles one or all three song banks. With no arguments,
# compiles all three banks (intro, indoor, ending) in sequence. With one
# argument, compiles only the named bank. Output goes to stdout.
if __name__ == "__main__":
  if len(sys.argv) == 1:
    # No arguments: compile all three banks that make up the game's audio
    for song in ['intro', 'indoor', 'ending']:
      print_song(song, sys.stdout)
  else:
    # Single argument: compile only the specified bank by name
    print_song(sys.argv[1], sys.stdout)
