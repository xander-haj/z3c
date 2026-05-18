# =============================================================================
# extract_music.py — Music and SFX Extraction Orchestrator for zelda3
# =============================================================================
# Pipeline-oriented module for extracting ALL sound data from the ALTTP ROM.
# Called by extract_resources.py as part of the full asset extraction pipeline.
#
# This file shares most of its code structure with decode_music.py (same classes,
# same SPC decoding logic, same memory model). The duplication exists because
# decode_music.py was the standalone development/debugging tool, while this file
# became the automated pipeline version used during asset extraction.
#
# Key differences from decode_music.py:
#   - Main entry point is extract_sound_data(), not a standalone script flow.
#   - dump_brr_audio() uses util.decode_brr instead of brr_tools.decode_brr.
#   - Writes full 64KB SPC memory images to .spc files for each sound bank.
#   - Extracts three sound banks (intro, indoor, ending) rather than one at a time.
#   - Produces BRR audio samples, music instrument metadata (YAML), and SFX patterns.
#
# The SNES audio subsystem (SPC700) has its own 64KB address space, separate from
# the main CPU. Sound banks are loaded into this address space by copying blocks
# of data from ROM. Each bank contains music sequences, instrument definitions,
# BRR-encoded audio samples, and SFX data.
# =============================================================================

# Standard library imports for hashing, binary data, and priority queue processing
import hashlib
import array
import heapq, sys
# YAML serialization for dumping instrument metadata
import yaml
import time
# Project-local utility module providing ROM loading and BRR decoding helpers
import util


# Loads a sound bank from ROM into a simulated 64KB SPC700 memory image.
# The SNES transfers sound data to the SPC700 as a series of (length, target_address)
# blocks. Each block is copied into the SPC memory at the specified target address.
# A block with length 0 signals the end of the transfer; its "target" field is actually
# the SPC entry point address (where the SPC code begins executing).
#
# Parameters:
#   rom      — a LoadedRom object providing get_byte() and get_word() for reading ROM data
#   ea       — the ROM address where the sound bank's block list begins
#   mem_in   — optional existing SPC memory to layer new data onto (used for bank overlays)
# Returns:
#   (memory, entry_point) — the populated 64KB memory list and the SPC entry point address
def load_sound_bank(rom, ea, mem_in = None):
  # Initialize a 64KB SPC memory image; None means "byte not loaded from ROM"
  memory = list(mem_in) if mem_in else [None]*65536
  j =0
  # Each iteration reads one (length, target) block header, then copies that many bytes
  while True:
    numbytes = rom.get_word(ea)
    target = rom.get_word(ea+2)
    # A zero-length block marks end of transfer; target becomes the SPC entry point
    if numbytes==0:
#      print('Entry point = 0x%x' % target)
      return memory, target
#    print('# Copy %d bytes to 0x%x' % (numbytes, target))
    # Skip past the 4-byte block header to reach the payload bytes
    ea += 4
    # Copy each payload byte into the SPC memory at the target address
    for i in range(numbytes):
      memory[target+i] = rom.get_byte(ea)
      ea += 1
      # SNES ROM bank boundary fix: if the low 16 bits fall below 0x8000,
      # skip forward by 0x8000 to reach the next bank's data region
      if (ea & 0xffff) < 0x8000:
        ea += 0x8000
    j += 1
    # Safety limit to prevent infinite loops on malformed data
    if j > 256:
      break

# Reads a single byte from the global SPC memory image at the given address.
# Returns the byte value (0-255) or None if that address was never loaded.
def get_byte(ea):
  return memory[ea]

# Reads a 16-bit little-endian word from the global SPC memory image.
# The SPC700 (like most SNES subsystems) stores words in little-endian order.
def get_word(ea):
  return get_byte(ea) | get_byte(ea + 1) * 256


# lightworld
# Copy 11694 bytes to 0xd000
# Copy 1672 bytes to 0x2b00


# indoor
# Copy 11455 bytes to 0xd000
# Copy 1292 bytes to 0x2b00

# Converts a value to its string representation for text output.
# Handles three cases: raw strings pass through, integers become decimal strings,
# and objects (Pattern, Phrase, etc.) use their .name attribute (e.g. "Pattern_0x1234").
def to_str(s):
  if isinstance(s, str):
    return s
  if isinstance(s, int):
    return str(s)
  return s.name


# Represents a single song (music track) in the SPC sound engine.
# A Song is a sequence of Phrase references, forming the top level of the music
# hierarchy: SongList -> Song -> Phrase -> Pattern -> note/effect commands.
# The .ea attribute holds the SPC memory address where this song's phrase list starts.
# The .index attribute holds this song's position in the SongList table.
# The .phrases list contains Phrase objects and PhraseLoop objects.
class Song:
  name = 'Song'
  # Serializes the song to a human-readable text format for the output .txt files
  def __str__(self):
    s = '# Song index %d\n' % self.index
    s += '[Song_0x%x]\n' % (self.ea)
    s += "".join(x.name + '\n' for x in self.phrases)
    return s

# Represents the master table of all songs in a sound bank.
# Stored at SPC address 0xD000, this is an array of 16-bit pointers to Song structures.
# The number of songs is derived from the first pointer (which points past the table end).
class SongList:
  name = 'SongList'
  # Serializes the song list as a labeled section with one song reference per line
  def __str__(self):
    s = '[SongList_0x%x]\n' % (self.ea)
    s += "".join(('None' if x == None else x.name) + '\n' for x in self.songs)
    return s

# Represents a phrase: one "row" in a song, containing 8 pattern references.
# The SPC700 music engine has 8 audio channels. Each phrase assigns one Pattern
# (a sequence of note/effect commands) to each of the 8 channels, defining what
# all channels play simultaneously during that phrase of the song.
class Phrase:
  name = 'Phrase'
  # Serializes the phrase as a labeled section listing its 8 pattern references
  def __str__(self):
    s = '[Phrase_0x%x]\n' % (self.ea)
    s += "".join(('None' if x == None else x.name) + '\n' for x in self.patterns)
    return s

# Represents a pattern: the lowest level of the music hierarchy.
# A pattern is a sequence of note commands and effect commands for a single channel.
# Each line in the pattern is either:
#   - A note event: (note_string, note_length_or_None, velocity_or_None) — 3-tuple
#   - An effect command: (effect_name, args, note_length, velocity) — 4-tuple
# Patterns are the leaf nodes that contain the actual musical data (pitches, timing, etc.).
class Pattern:
  name = 'Pattern'
  # Serializes the pattern to a tracker-style text format.
  # Notes appear as "C#4 12 7f" (note, length, velocity).
  # Effects appear as "Instrument 5" or "Call Pattern_0x1234 3".
  # Dashes (--) indicate inherited/unchanged values from previous lines.
  def __str__(self):
    r = '[Pattern_0x%x]\n' % (self.ea)
    last_len = None
    for a in self.lines:
      s = ''
      # 4-tuple lines are effect commands: (effect_name, args_tuple, length, velocity)
      if len(a) == 4:
        s += a[0] + " " + " ".join(map(to_str, a[1]))
      else:
        # 3-tuple lines are note events: (note_string, length_or_None, velocity_or_None)
        s += '%s' % (a[0])

        if a[-2] != None:
          s += ' %2d' % a[-2]
          last_len = a[-2]
        else:
          s += ' --'# % last_len

        if a[-1] != None:
          s += ' %2x' % a[-1]
        else:
          s += ' --'

      r += s + '\n'
    return r
  
# Represents a loop control marker within a song's phrase list.
# When the SPC engine encounters a PhraseLoop, it repeats a section of the song.
# The 'loops' field is the repeat count, and 'jmp' is the relative offset (in phrase
# entries) to jump back to. This enables repeating choruses, verses, etc.
class PhraseLoop:
  name = 'PhraseLoop'
  # Parameters:
  #   loops — number of times to repeat the section
  #   jmp   — relative offset (negative, in phrase entries) to the loop start
  def __init__(self, loops, jmp):
    self.loops = loops
    self.jmp = jmp
    self.name = 'PhraseLoop %d %d' % (self.loops, self.jmp)
  def __str__(self):
    return self.name

# Global registry mapping SPC addresses to their decoded music objects (Song, Phrase, etc.).
# This deduplicates objects: if the same address is referenced twice, the same object is reused.
types_for_ea = {}
# Priority queue (min-heap) of (address, object) pairs, ensuring objects are decoded
# in ascending address order. This is important because patterns need to know where
# the next pattern starts (to detect "fallthrough" boundaries).
pqueue_by_ea = []

# Clears both global registries. Called before loading each new sound bank to ensure
# objects from a previous bank don't leak into the next bank's decoding pass.
def reset_queues():
  global types_for_ea, pqueue_by_ea
  types_for_ea = {}
  pqueue_by_ea = []

# Looks up or creates a music object of the given type at the specified SPC address.
# If the address already has a registered object, returns it (with a type-safety check).
# If new, creates the object, assigns it a name like "Pattern_0x1234", registers it,
# and enqueues it for decoding if its memory region was loaded from ROM.
#
# Parameters:
#   ea — SPC memory address of the object
#   tp — the class type to instantiate (Song, Phrase, Pattern, or SongList)
# Returns: the music object, or None if ea is 0 (null pointer)
def get_type_for_ea(ea, tp):
  # Null pointer in the SPC data means "no object here"
  if ea == 0:
    return None
  # Addresses below 256 are SPC hardware registers, not valid data locations
  assert(ea >= 256), ea
  # Return existing object if this address was already registered
  a = types_for_ea.get(ea)
  if a != None:
    # Verify the same address isn't being interpreted as two different types
    assert type(a)==tp, (type(a), tp, '0x%x' % ea)
    return a
  # Create a new object, tag it with its SPC address and a readable name
  a = tp()
  a.ea = ea
  a.name = '%s_0x%x' % (a.name, ea)
  types_for_ea[ea] = a
  # If this address has loaded data, enqueue for decoding; otherwise mark as imported
  # (imported = referenced but lives in a different/unloaded sound bank)
  if get_byte(ea) != None:
    heapq.heappush(pqueue_by_ea, (ea, a))
    a.is_imported = False
  else:
    a.is_imported = True
  return a

# Number of argument bytes following each effect command (0xE0..0xFA).
# Indexed by (command_byte - 0xE0). For example, Instrument (0xE0) takes 1 byte,
# Vibrato (0xE3) takes 3 bytes, VibratoOff (0xE4) takes 0 bytes, etc.
kEffectByteLength = [1, 1, 2, 3, 0, 1, 2, 1, 2, 1, 1, 3, 0, 1, 2, 3, 1, 3, 3, 0, 1, 3, 0, 3, 3, 3, 1]
# Human-readable names for each effect command, indexed by (command_byte - 0xE0).
# These correspond to the SNES SPC700 music engine's built-in effect opcodes.
kEffectNames = ['Instrument', 'Pan', 'PanFade', 'Vibrato', 'VibratoOff',
                'SongVolume', 'SongVolumeFade', 'Tempo', 'TempoFade',
                'Transpose', 'ChannelTranpose', 'Tremolo', 'TremoloOff',
                'Volume', 'VolumeFade', 'Call', 'VibratoFade',
                'PitchEnvelopeTo', 'PitchEnvelopeFrom', 'PitchEnvelopeOff',
                'FineTune', 'EchoEnable', 'EchoOff', 'EchoSetup', 'EchoVolumeFade',
                'PitchSlide', 'PercussionDefine']
# Sanity check: there must be exactly 27 effect types defined (0xE0 through 0xFA)
assert(len(kEffectNames) == 27)

# Converts a numeric MIDI-like note value (0-73) to a tracker-style string.
# Notes 0-71 map to standard pitches (C-1 through B-7). Note 72 is a "tie" (sustain
# the previous note, shown as "-+-"), and note 73 is a "key off" (release, shown as "---").
#
# Parameters:
#   note — integer note value (0-73), where 0 = C-1 and 71 = B-7
# Returns: a 3-character string like "C#4", "-+-", or "---"
def note_to_str(note):
  # The 12 chromatic pitch classes in standard musical notation
  kKeys  = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-']
  # Special note values above the 6-octave range
  if note >= 72:
    if note == 72:
      return '-+-' # don't write kof
    elif note == 73:
      return '---' # want kof
    else:
      assert 0
  # Standard note: compute octave (0-5) and pitch class (0-11) from linear note number
  octave = note / 12
  key = note % 12
  return '%s%d' % (kKeys[key], octave + 1)

# Retrieves or creates a Pattern object at the given SPC memory address.
# Returns None for address 0, which indicates an empty/unused channel slot.
#
# Parameters:
#   ea — SPC memory address where the pattern data begins
# Returns: a Pattern object registered in the type map, or None if ea is 0
def get_pattern(ea):
  # Address 0 is the null sentinel — no pattern assigned to this channel
  if ea == 0:
    return None
  pattern = get_type_for_ea(ea, Pattern)
  return pattern

# Retrieves or creates a Song object and assigns its index within the song list.
# The index is stored on the Song so it can be referenced during text output.
#
# Parameters:
#   ea    — SPC memory address of the song's phrase pointer table
#   index — ordinal position of this song in the bank's song list
# Returns: a Song object with its index field set, or None if not found
def get_song(ea, index):
  song = get_type_for_ea(ea, Song)
  if song:
    song.index = index
  return song

# Retrieves or creates a Phrase object at the given SPC memory address.
# A Phrase holds pointers to 8 Pattern objects (one per SPC700 channel).
#
# Parameters:
#   ea — SPC memory address of the phrase's 8 channel pointers
# Returns: a Phrase object registered in the type map
def get_phrase(ea):
  phrase = get_type_for_ea(ea, Phrase)
  return phrase

# Decodes the raw byte stream of a music pattern into a list of note and
# effect commands. The SPC700 music engine uses a variable-length encoding:
#   - Bytes 0x01-0x7F before a command set note length and volume index
#   - Bytes 0x80-0xDF encode note-on events (bit 7 set, bits 0-6 = note)
#   - Bytes 0xE0-0xFA are effect commands with variable-length arguments
#   - Byte 0xEF is the special "Call" command (subroutine call to another pattern)
#   - Byte 0x00 terminates the pattern
#
# Parameters:
#   pattern — Pattern object whose .ea field points to the raw byte data
#   next_ea — address of the next pattern in memory, used to detect fallthroughs
# Returns: the Pattern object with its .lines list populated
def decode_pattern(pattern, next_ea):
  ea = pattern.ea
  pattern.lines = []
  start_ea = ea
  # Walk the byte stream until we hit a terminator (0x00) or fall into the next pattern
  while True:
#    print('0x%x 0x%x' % (ea, start_ea))
#    assert ea != 0x28f0
    # If we've reached the start of the next pattern without hitting a terminator,
    # this pattern "falls through" into the next one (shared tail optimization)
    if ea != start_ea and ea == next_ea:
      pattern.lines.append(('Fallthrough', (), None, None))
      return
    note_length, volstuff = None, None
    cmd = get_byte(ea); ea += 1
    # Zero byte terminates the pattern
    if cmd == 0:
      break
    # Bytes without bit 7 set are optional prefix bytes for note length and volume.
    # Up to two prefix bytes can appear before the actual note or effect command.
    if not (cmd & 0x80):
      note_length = cmd
      cmd = get_byte(ea); ea += 1
      if not (cmd & 0x80):
        volstuff = cmd
        cmd = get_byte(ea); ea += 1
    # 0xEF = "Call" — a subroutine call to another pattern with a loop count.
    # This recursively registers the target pattern for later decoding.
    if cmd == 0xef:
      addr = get_word(ea)
      loops = get_byte(ea + 2)
      ea += 3
      pattern.lines.append((kEffectNames[cmd-0xe0], (get_pattern(addr), loops), note_length, volstuff))
    # 0xE0-0xFA = effect commands (instrument change, pan, vibrato, tempo, etc.).
    # Effect commands cannot have note_length/volume prefixes.
    elif cmd >= 0xe0:
      assert note_length == None and volstuff == None, (note_length, volstuff)
      x = kEffectByteLength[cmd - 0xe0]
      args = [get_byte(ea+i) for i in range(x)]
      ea += x
      pattern.lines.append((kEffectNames[cmd-0xe0], args, note_length, volstuff))
    # 0x80-0xDF = note-on event. Bit 7 is stripped to get the note value (0-95),
    # which is then converted to a human-readable tracker-style string.
    else:
      assert(cmd & 0x80)
      pattern.lines.append((note_to_str(cmd & 0x7f), note_length, volstuff))

  return pattern

# Decodes a Phrase by reading 8 consecutive 16-bit pattern pointers from SPC memory.
# Each Phrase contains exactly 8 channel slots (one per SPC700 voice), where each
# slot points to a Pattern or is null (address 0) for silent channels.
#
# Parameters:
#   phrase — Phrase object whose .ea field points to the 16-byte pointer table
def decode_phrase(phrase):
  phrase.patterns = [get_pattern(get_word(phrase.ea + i * 2)) for i in range(8)]

# Decodes a Song's phrase list from its sequential pointer table in SPC memory.
# A Song is an ordered list of Phrases (each Phrase = one row of 8 channel patterns).
# The phrase list supports two entry types:
#   - Phrase pointer (>= 0x100): address of a Phrase in SPC memory
#   - Loop command (< 0x100, not 0x80/0x81): repeat count + backwards jump target,
#     allowing sections of the song to loop without duplicating phrase data
# The list terminates when a null (0x0000) pointer is encountered.
#
# Parameters:
#   song — Song object whose .ea field points to the start of the phrase pointer table
# Returns: the Song object with its .phrases list populated
def decode_song(song):
  ea = song.ea
  song.phrases = []
  ea_org = ea
  # Track all visited addresses so loop targets can be validated
  eas_in_phrase = []
  while True:
    eas_in_phrase.append(ea)
    phrase = get_word(ea)
    # Null pointer marks the end of the song's phrase sequence
    if phrase == 0:
      break
    # Values below 0x100 are loop commands rather than phrase pointers.
    # The value is the loop repeat count, followed by a 2-byte target address
    # that points backwards into this same phrase list.
    if phrase < 0x100:
      # 0x80 and 0x81 are reserved control codes, not valid loop counts
      assert phrase != 0x80 and phrase != 0x81
      tgt = get_word(ea + 2)
      # Validate the loop target is an address we already visited
      assert tgt in eas_in_phrase
      # Convert the absolute target address to a relative phrase index offset
      song.phrases.append(PhraseLoop(phrase, (tgt - ea) // 2))
      ea += 4
    else:
      # Standard phrase pointer — register/retrieve the Phrase object
      song.phrases.append(get_phrase(phrase))
      ea += 2
  return song

# Dispatches decoding to the appropriate handler based on the object's type.
# Called during the priority-queue-driven decoding pass to process each music
# structure in address order (lowest address first), which ensures subroutine
# patterns are decoded before they are referenced.
#
# Parameters:
#   what    — a Song, SongList, Phrase, or Pattern object to decode
#   next_ea — SPC address of the next queued item (needed for Pattern fallthrough)
def decode_any(what, next_ea):
  if isinstance(what, Song):
    decode_song(what)
  elif isinstance(what, SongList):
    pass # no need
  elif isinstance(what, Phrase):
    decode_phrase(what)
  elif isinstance(what, Pattern):
    decode_pattern(what, next_ea)
  else:
    assert 0

# Retrieves or creates a SongList and populates it by reading a table of
# 16-bit song pointers from SPC memory. Each entry points to a Song's
# phrase table. The song list lives at a fixed address (typically 0xD000)
# in every sound bank.
#
# Parameters:
#   ea  — SPC memory address of the song pointer table
#   num — number of songs in this bank
def get_song_list(ea, num):
  song_list = get_type_for_ea(ea, SongList)
  song_list.songs  = [get_song(get_word(ea + i * 2), i) for i in range(num)]

# Loads a named sound bank from ROM into the global SPC memory image and
# resets all decoding state. Each bank has a hardcoded ROM address and a
# known song count. The song count is derived from the first song pointer
# at 0xD000 — since the pointer table starts at 0xD000 and the first
# pointer tells us where song data begins, the difference gives the table size.
#
# For intro/lightworld banks, the song count is self-describing (read from
# the first pointer). For indoor/ending banks, the table size is fixed at
# 0x46 bytes (35 songs), because those banks share a different layout.
#
# Parameters:
#   ROM  — a LoadedRom object for reading sound bank data from the cartridge
#   song — bank name string: 'intro', 'lightworld', 'indoor', or 'ending'
def load_song(ROM, song):
  global memory, SONGS_IN_BANK
  reset_queues()
  if song == 'intro':
    memory, entry_point = load_sound_bank(ROM, 0x998000) # intro
    # Song count = (first song pointer - table base) / 2 bytes per pointer
    SONGS_IN_BANK = (get_word(0xd000) - 0xd000) // 2
  elif song == 'lightworld':
    memory, entry_point = load_sound_bank(ROM, 0x9a9ef5) # lw
    SONGS_IN_BANK = (get_word(0xd000) - 0xd000) // 2
  elif song == 'indoor':
    memory, entry_point = load_sound_bank(ROM, 0x9b8000) # indoor
    # Indoor bank has a fixed-size song table (35 entries = 0x46 bytes)
    SONGS_IN_BANK = (0xd046 - 0xd000) // 2
  elif song == 'ending':
    memory, entry_point = load_sound_bank(ROM, 0x9ad380) # ending
    SONGS_IN_BANK = (0xd046 - 0xd000) // 2


# Decodes and prints all music data for a sound bank to the given file handle.
# First seeds the decoding queue with the song list and any "orphan" phrases/patterns
# that are not reachable through the normal song→phrase→pattern pointer chain.
# These orphans exist because the SPC engine sometimes references patterns by
# hardcoded addresses that bypass the standard hierarchy.
#
# After seeding, drains the priority queue in address order, decoding each object.
# Finally prints all decoded objects sorted by address, skipping any that were
# imported (referenced but defined in a different context).
#
# Parameters:
#   song — bank name string identifying which orphan addresses to seed
#   f    — file handle to write the decoded text representation to
def print_song(song, f):
  # Start by registering the song list at the standard base address 0xD000
  get_song_list(0xd000, SONGS_IN_BANK)
  # Seed orphan phrases/patterns that the normal pointer chain doesn't reach.
  # These are music data blocks referenced by hardcoded addresses in the SPC engine.
  if song in ('intro', 'lightworld'):
    get_phrase(0xD878)
    get_phrase(0xD8A8)
    get_phrase(0xD8B8)
    get_phrase(0xDf11)
    get_phrase(0xe37c)
  if song == 'indoor':
    get_phrase(0xDc5e)
    get_phrase(0xDc6e)
    get_pattern(0xe905)
    get_phrase(0xe94a)
  if song == 'ending':
    get_phrase(0x2a10)
  # Drain the priority queue, decoding each item in ascending address order.
  # This ensures patterns referenced as subroutines are decoded before their callers.
  while len(pqueue_by_ea):
    _, item = heapq.heappop(pqueue_by_ea)
    decode_any(item, pqueue_by_ea[0][0] if len(pqueue_by_ea) else None)
  # Print all decoded objects in address order, skipping imported references
  for a, b in sorted(types_for_ea.items()):
    if not b.is_imported:
      print(b, file = f)


# Extracts all 25 BRR audio samples from the current sound bank and writes
# each one to disk in two formats: raw BRR data (.pcm.brr) and decoded 16-bit
# PCM (.pcm). The sample directory table lives at SPC address 0x3C00, where
# each entry is 4 bytes: 2-byte sample start address + 2-byte loop point address.
#
# BRR (Bit Rate Reduction) is the SNES's native audio compression format:
# 9 bytes encode 16 PCM samples (1 header byte + 8 data bytes of 4-bit nibbles).
def dump_brr_audio():
  # Inner helper: reads one BRR sample from SPC memory, decodes it to PCM,
  # and returns (pcm_data, raw_brr_bytes, has_loop_flag).
  # The loop flag is bit 1 of the first BRR block's header byte.
  def decode_brr(snd):
    # Look up sample start and loop addresses from the directory at 0x3C00
    start, loop_start = get_word(0x3c00 + snd * 4), get_word(0x3c00 + snd * 4 + 2)
    # Decode BRR to 16-bit PCM using the utility decoder
    r = util.decode_brr(lambda x: get_byte(start+x))
    # Compute raw BRR byte count from the decoded PCM length (16 samples per 9-byte block)
    return r, [get_byte(start+x) for x in range(len(r)//16 * 9)], get_byte(start)&0x2 != 0
  # Extract all 25 instrument samples used by the music engine
  for audio_idx in range(25):
    sound_data, brr_data, brr_repeat = decode_brr(audio_idx)
    # Write the raw BRR-encoded data (for re-compilation back into SPC format)
    open('sound/sound%d.pcm.brr' % audio_idx, 'wb').write(bytes(brr_data))
    # Write the decoded PCM data (for playback or conversion to WAV)
    open('sound/sound%d.pcm' % audio_idx, 'wb').write(sound_data)

# Extracts all instrument and sample metadata from the current sound bank
# and writes it to music_info.yaml. This YAML file is consumed by the music
# compiler (compile_music.py) when rebuilding the sound data.
#
# The metadata includes:
#   - 25 sample entries (BRR file paths and loop points)
#   - 25 instrument definitions (sample index, ADSR envelope, pitch base)
#   - Note gate-off and volume lookup tables
#   - 25 SFX instrument definitions (stereo volume, pitch, sample, envelope)
def dump_music_info():
  music_info = {}
  # Samples 10 and 20 are byte-identical duplicates of 9 and 19 respectively.
  # Map them to the same PCM file to avoid writing redundant copies.
  kDupSamples = {10 : 9, 20 : 19}
  music_info['samples'] = []
  # Read the sample directory table at 0x3C00 (4 bytes per entry: start + loop addr)
  for audio_idx in range(25):
    start, rep = get_word(0x3c00 + audio_idx * 4), get_word(0x3c00 + audio_idx * 4 + 2)
    sample_info = {
      'file' : 'sound/sound%d.pcm' % kDupSamples.get(audio_idx, audio_idx)
    }
    # Bit 1 of the first BRR header indicates the sample has a loop point.
    # Convert the byte-level loop offset to a PCM sample offset.
    if get_byte(start) & 2:
      sample_info['repeat'] = (rep - start) // 9 * 16
    music_info['samples'].append(sample_info)

  # Helper to extract ADSR envelope and gain parameters from a 3-byte block.
  # The SPC700 DSP uses two ADSR registers and one GAIN register per voice:
  #   ADSR1: bits 4-6 = decay rate, bits 0-3 = attack rate
  #   ADSR2: bits 5-7 = sustain level, bits 0-4 = sustain rate
  #   GAIN:  direct gain value (used when ADSR is disabled)
  def add_sustain_decay_etc(ea, info):
    adsr1, adsr2, gain = get_byte(ea), get_byte(ea + 1), get_byte(ea + 2)
    info['decay'] = (adsr1 >> 4) & 7
    info['attack'] = adsr1 & 0xf
    info['sustain_level'] = adsr2 >> 5
    info['sustain_rate'] = adsr2 & 0x1f
    info['vxgain'] = gain

  # Read the 25 music instrument definitions from the table at 0x3D00.
  # Each entry is 6 bytes: sample_index(1) + ADSR1(1) + ADSR2(1) + GAIN(1) + pitch_base(2)
  music_info['instruments'] = []
  for i in range(25):
    ea = 0x3d00 + i * 6
    adsr1, adsr2 = get_byte(ea + 1), get_byte(ea + 2)
    info = {
      'sample' : get_byte(ea),
    }
    add_sustain_decay_etc(ea + 1, info)
    # Pitch base is a 16-bit value controlling the sample's playback frequency
    info['pitch_base'] = get_byte(ea + 4) << 8 | get_byte(ea + 5)
    music_info['instruments'].append(info)

  # Gate-off table (8 entries at 0x3D96): controls how early a note is released
  # before the next note begins, creating staccato/legato articulation
  music_info['note_gate_off'] = [get_byte(i) for i in range(0x3D96, 0x3D96 + 8)]
  # Volume table (16 entries at 0x3D9E): maps volume index values from pattern
  # data to actual DSP volume levels
  music_info['note_volume'] = [get_byte(i) for i in range(0x3D9E, 0x3D9E + 16)]

  # Read the 25 SFX instrument definitions from the table at 0x3E00.
  # Each entry is 9 bytes with stereo volume, pitch, sample index, envelope, and pitch base.
  # SFX instruments are separate from music instruments because SFX use
  # direct stereo panning (left/right volumes) rather than the music engine's pan system.
  music_info['sfx_instruments'] = []

  for i in range(25):
    ea = 0x3e00 + i * 9
    info = {
      'voll' : get_byte(ea),
      'volr' : get_byte(ea + 1),
      'pitch' : get_word(ea + 2),
      'sample' : get_byte(ea + 4)
    }
    add_sustain_decay_etc(ea + 5, info)
    info['pitch_base'] = get_byte(ea + 8)
    music_info['sfx_instruments'].append(info)

  # Serialize all metadata to YAML for consumption by compile_music.py
  s = yaml.dump(music_info, default_flow_style=None, sort_keys=False)
  open('music_info.yaml', 'w').write(s)

# Decodes a single SFX pattern from SPC memory into a list of command tuples.
# SFX patterns use a simpler encoding than music patterns, with different
# effect opcodes and stereo volume control (separate left/right channels).
#
# The byte encoding is similar to music patterns but with key differences:
#   - Up to 3 prefix bytes: note_length, volume_left, volume_right (all < 0x80)
#   - 0xE0 = SetInstrument (switch to a different SFX instrument)
#   - 0xF9 = PitchSlide with a target note (slide from current pitch to note)
#   - 0xF1 = PitchSlide without a target note (relative pitch bend)
#   - 0xFF = Restart (loop the SFX from the beginning)
#   - 0x00 = end of SFX pattern
#   - Other bytes with bit 7 set = note-on events (same as music)
#
# Parameters:
#   ea        — SPC memory address of the SFX pattern data
#   next_addr — address of the next SFX pattern (for fallthrough detection)
# Returns: list of command tuples representing the decoded SFX sequence
def decode_sfx(ea, next_addr):
  r = []
  while True:
    # If we reach the next pattern's address, this SFX falls through into it
    if ea == next_addr:
      r.append(('Fallthrough', ))
      return r
    b = get_byte(ea); ea += 1
    # Zero byte terminates the SFX pattern
    if b == 0:
      return r
    note_length = None
    volume_left, volume_right = None, None
    # Parse optional prefix bytes (all have bit 7 clear).
    # SFX supports up to 3 prefixes: note_length, volume_left, volume_right.
    # This differs from music patterns which only have length + volume_index.
    if not (b & 0x80):
      note_length = b
      b = get_byte(ea); ea += 1
      if not (b & 0x80):
        volume_left, volume_right = b, None
        b = get_byte(ea); ea += 1
        if not b & 0x80:
          volume_right = b
          b = get_byte(ea); ea += 1
    # 0xE0 = SetInstrument: switches the SFX voice to a different instrument.
    # Must appear without any prefix bytes (standalone command).
    if b == 0xe0:
      assert note_length == None and volume_left == None and volume_right == None, ea
      b = get_byte(ea); ea += 1
      r.append(('SetInstrument %d' % b, ))
    # 0xF9 = PitchSlide with target note: smoothly bends pitch toward a note.
    # Followed by a note byte and 3 parameter bytes (delay, speed, target).
    elif b == 0xf9:
      #assert note_length == None and volume_left == None and volume_right == None, ea
      b = get_byte(ea); ea += 1
      b0, b1, b2 = get_byte(ea), get_byte(ea+1), get_byte(ea+2); ea += 3
      r.append(('PitchSlide %d %d %d' % (b0, b1, b2), note_to_str(b & 0x7f), note_length, volume_left, volume_right))
    # 0xF1 = PitchSlide without target note: relative pitch bend using 3 params.
    # No note byte follows — the bend is relative to the current pitch.
    elif b == 0xf1:
      #assert note_length == None and volume_left == None and volume_right == None, ea
      b0, b1, b2 = get_byte(ea), get_byte(ea+1), get_byte(ea+2); ea += 3
      r.append(('PitchSlide %d %d %d' % (b0, b1, b2), None, note_length, volume_left, volume_right))
    # 0xFF = Restart: causes the SFX to loop from its beginning.
    # Must appear without prefix bytes and terminates the pattern.
    elif b == 0xff:
      assert note_length == None and volume_left == None and volume_right == None, ea
      r.append(('Restart',))
      return r
    # Any other byte with bit 7 set is a note-on event (strip bit 7 for note value)
    else:
      r.append((None, note_to_str(b & 0x7f), note_length, volume_left, volume_right))

# Decodes and prints all SFX data from the current sound bank to a text file.
# The SPC700 engine organizes SFX across three I/O ports:
#   - Port 1 (0x17C0): 32 SFX entries — pointer table + priority bytes
#   - Port 2 (0x1820): 63 SFX entries — pointer table + priority + echo bytes
#   - Port 3 (0x191C): 63 SFX entries — pointer table + priority + echo bytes
#
# Each port's memory layout is: [pointer_table][priority_bytes][echo_bytes].
# Port 1 lacks the echo byte array. After printing the port tables, this
# function decodes every unique SFX pattern referenced by those tables
# (plus manually-added orphan addresses for SFX not in the standard tables).
#
# Parameters:
#   f — file handle to write the decoded SFX text representation to
def print_all_sfx(f):
  # Collect all unique SFX pattern addresses across all three ports
  items = set()
  # Inner helper: prints one SFX port's dispatch table and collects pattern addresses.
  # Each port has a pointer table (2 bytes per entry), a priority byte array,
  # and optionally an echo byte array (Port 2 and Port 3 only).
  def add_sfx_top(base, num, name):
    print('[%s_0x%x]' % (name, base), file = f)
    # Priority bytes immediately follow the pointer table
    next_ea = base + num * 2
    # Echo bytes follow the priority bytes (only used by Port 2 and Port 3)
    echo_ea = next_ea + num
    for i in range(num):
      r = []
      ea = get_word(base + i * 2)
      # Null pointer means this SFX slot is unused
      if ea == 0:
        t = 'None'
      else:
        items.add(ea)
        t = 'Sfx_0x%x' % ea
      # Port 1 only has priority; Ports 2 and 3 also have echo enable flags
      if name == 'SfxPort1':
        print('%s,%d' % (t, get_byte(next_ea + i)), file = f)
      else:
        print('%s,%d,%d' % (t, get_byte(next_ea + i), get_byte(echo_ea + i)), file = f)
    print(file = f)
  # Process all three SFX port tables at their fixed SPC memory addresses
  add_sfx_top(0x17c0, 32, 'SfxPort1')
  add_sfx_top(0x1820, 63, 'SfxPort2')
  add_sfx_top(0x191c, 63, 'SfxPort3')
  # Add orphan SFX pattern addresses that are not referenced by any port table
  # but exist in SPC memory. These are likely sub-patterns called by other SFX
  # or used by hardcoded engine routines.
  items.add(0x1a5b)
  items.add(0x1d1c)
  items.add(0x1ee2)
  items.add(0x1f13)
  items.add(0x1f1c)
  items.add(0x252d)
  items.add(0x2533)
  items.add(0x26a2)
  items.add(0x277e)
  items.add(0x279d)
  items.add(0x27c9)
  items.add(0x27f6)
  items.add(0x2807)
  items.add(0x2818)
  items.add(0x2829)
  items.add(0x2831)
  items.add(0x284a)
  # Sort all pattern addresses so we can detect fallthroughs between adjacent patterns
  items = sorted(list(items))
  # Decode and print each SFX pattern in ascending address order
  for i in range(len(items)):
    print('[Sfx_0x%x]' % items[i], file = f)
    # Pass the next pattern's address for fallthrough detection (0 if this is the last)
    next_addr = items[i + 1] if i + 1 < len(items) else 0
    rs = decode_sfx(items[i], next_addr)
    for r in rs:
      # 5-element tuples are note/pitch events with optional parameters.
      # Format: "note length vol_l vol_r [effect_name]"
      # None values are replaced with placeholder dashes for readable alignment.
      if len(r) == 5:
        aa = '.  ' if r[1] == None else r[1]
        bb = '--' if r[2] == None else '%2d' % r[2]
        cc = '---' if r[3] == None else '%3d' % r[3]
        dd = '---' if r[4] == None else '%3d' % r[4]
        r0 = '' if r[0] == None else ' ' + r[0]
        print('%s %s %s %s%s' % (aa, bb, cc, dd, r0), file = f)
      # Single-element tuples are standalone commands (SetInstrument, Restart, Fallthrough)
      else:
        print(r[0], file = f)
    print(file = f)

# Main entry point called by extract_resources.py during full asset extraction.
# Processes three sound banks (intro, indoor, ending) and writes all extracted
# data to disk. For each bank it:
#   1. Loads the bank from ROM into simulated SPC memory
#   2. Dumps the full 64KB SPC memory image as a .spc file
#   3. Decodes and writes all music sequences to a text file
#
# The intro bank is special: it also contains the BRR samples, instrument
# metadata, and SFX data shared by all banks, so those are only extracted once.
#
# Parameters:
#   rom — a LoadedRom object for reading the ALTTP cartridge data
def extract_sound_data(rom):
  for song in ['intro', 'indoor', 'ending']:
    load_song(rom, song)
    # Write the full 64KB SPC memory image (None bytes become 0x00)
    open('sound/%s.spc' % song, 'wb').write(bytes((0 if a == None else a) for a in memory))
    # Decode and write all music patterns, phrases, and songs to a text file
    print_song(song, open('sound_%s.txt' % song, 'w'))
    # BRR samples, instrument metadata, and SFX only need to be extracted once
    # because they live in the intro bank and are shared across all banks
    if song == 'intro':
      dump_brr_audio()
      dump_music_info()
      print_all_sfx(open('sfx.txt', 'w'))

# Standalone CLI mode: loads a single named bank and prints its decoded music
# to stdout. Used for debugging and inspection outside the full extraction pipeline.
if __name__ == "__main__":
  if len(sys.argv) < 3:
    print('extract_music.py [rom-filename] [intro|lightworld|indoor|ending]')
    sys.exit(0)
  ROM = util.LoadedRom(None if sys.argv[1] == '' else sys.argv[1])
  song = sys.argv[2]

  load_song(ROM, song)
  print_song(song, sys.stdout)

