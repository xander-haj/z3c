# =============================================================================
# decode_music.py — SPC700 Music Decoder for Zelda 3 (A Link to the Past)
#
# This file reads binary music data from the SNES ROM's SPC700 sound coprocessor
# format and decodes it into a human-readable text representation. It is the
# reverse operation of compile_music.py.
#
# The SNES SPC700 is a dedicated audio CPU with its own 64KB address space.
# The ROM stores compressed sound banks as a series of (length, target_addr, data)
# blocks that get copied into that 64KB memory image. Once loaded, structured
# music data lives at known addresses:
#   - 0xD000: Song list table (array of 16-bit pointers to Song objects)
#   - 0x3C00: BRR sample directory (start/loop pointers for audio samples)
#   - 0x3D00: Instrument definitions (sample index, ADSR, pitch)
#   - 0x3E00: SFX instrument definitions
#   - 0x17C0-0x191C: SFX port tables (SfxPort1, SfxPort2, SfxPort3)
#   - 0x2B00: Additional data block (instrument/sample data)
#
# The decoding hierarchy is: SongList -> Song -> Phrase -> Pattern -> notes/effects.
# Each level is discovered by following 16-bit address pointers in SPC700 memory.
#
# This module is used as a helper by extract_music.py and can also be run
# standalone: python decode_music.py <rom_path> <bank_name>
#
# Related files:
#   - compile_music.py: the inverse encoder (text -> binary)
#   - extract_music.py: orchestrates full music extraction from ROM
#   - brr_tools.py: BRR audio sample codec (Bit Rate Reduction, SNES format)
#   - util.py: ROM loading utilities
# =============================================================================

# Standard library imports for hashing, data structures, and serialization
import hashlib
import array
import heapq, sys
import yaml
import time
# Project-local imports: ROM loader and BRR audio sample decoder
import util, brr_tools


# Loads a sound bank from the SNES ROM into a simulated 64KB SPC700 memory image.
# The ROM stores sound banks as a sequence of transfer blocks, each with a 4-byte header:
#   - 2 bytes: number of data bytes to copy
#   - 2 bytes: target address in SPC700 memory
# A block with numbytes=0 signals end-of-bank; its "target" field is the entry point address.
#
# Parameters:
#   rom: a LoadedRom object providing get_byte() and get_word() for reading ROM data
#   ea: the ROM address (24-bit SNES address) where the sound bank begins
#   mem_in: optional pre-existing memory image to overlay data onto
# Returns: (memory, entry_point) where memory is a 65536-element list representing SPC700 RAM,
#   and entry_point is the SPC700 address where the sound engine begins execution.
def load_sound_bank(rom, ea, mem_in = None):
  # Initialize a 64KB SPC700 memory image; None marks uninitialized bytes
  memory = list(mem_in) if mem_in else [None]*65536
  j =0
  while True:
    # Read the 4-byte block header: length and destination address
    numbytes = rom.get_word(ea)
    target = rom.get_word(ea+2)
    # A zero-length block marks the end of the bank; target holds the entry point
    if numbytes==0:
#      print('Entry point = 0x%x' % target)
      return memory, target
    print('# Copy %d bytes to 0x%x' % (numbytes, target))
    ea += 4
    # Copy the raw bytes from ROM into the SPC700 memory image at the target address
    for i in range(numbytes):
      memory[target+i] = rom.get_byte(ea)
      ea += 1
      # SNES ROM bank boundary crossing: if we drop below 0x8000, skip to the next bank
      # SNES banks map ROM data at 0x8000-0xFFFF; addresses below 0x8000 are hardware registers
      if (ea & 0xffff) < 0x8000:
        ea += 0x8000
    j += 1
    # Safety limit to prevent infinite loops if bank data is corrupted
    if j > 256:
      break

# Reads a single byte from the global SPC700 memory image at the given address.
# Returns None if the address was never written during bank loading.
def get_byte(ea):
  return memory[ea]

# Reads a 16-bit little-endian word from SPC700 memory (low byte first, high byte second).
# The SPC700 and SNES both use little-endian byte ordering.
def get_word(ea):
  return get_byte(ea) | get_byte(ea + 1) * 256


# lightworld
# Copy 11694 bytes to 0xd000
# Copy 1672 bytes to 0x2b00


# indoor
# Copy 11455 bytes to 0xd000
# Copy 1292 bytes to 0x2b00

# Converts a value to its string representation for text output.
# Handles raw strings, integer arguments, and decoded objects (Pattern, Phrase, etc.)
# which have a .name attribute like "Pattern_0xD123".
def to_str(s):
  if isinstance(s, str):
    return s
  if isinstance(s, int):
    return str(s)
  return s.name


# Represents a single song in the SPC700 music data.
# A Song is a sequence of Phrase references and PhraseLoop commands.
# Each song lives at an SPC700 address (self.ea) and has an index in the song list.
# The text output format is: [Song_0xADDR] followed by one phrase name per line.
class Song:
  name = 'Song'
  # Formats the song as text: a header with the song index and address,
  # followed by the name of each phrase in playback order.
  def __str__(self):
    s = '# Song index %d\n' % self.index
    s += '[Song_0x%x]\n' % (self.ea)
    s += "".join(x.name + '\n' for x in self.phrases)
    return s

# Represents the top-level song list table, always located at 0xD000 in SPC700 memory.
# Contains an array of 16-bit pointers to Song objects. The number of songs is determined
# by reading the first pointer (which points past the table itself) and dividing by 2.
class SongList:
  name = 'SongList'
  # Formats as a labeled section with one song name per line; "None" for null pointers.
  def __str__(self):
    s = '[SongList_0x%x]\n' % (self.ea)
    s += "".join(('None' if x == None else x.name) + '\n' for x in self.songs)
    return s

# Represents a Phrase: a group of exactly 8 pattern pointers, one for each SPC700 channel.
# The SNES SPC700 has 8 audio channels (voices), so each phrase assigns one Pattern per channel.
# A null pointer (0x0000) means that channel is silent during this phrase.
class Phrase:
  name = 'Phrase'
  # Formats as a labeled section listing the 8 pattern names (one per channel).
  def __str__(self):
    s = '[Phrase_0x%x]\n' % (self.ea)
    s += "".join(('None' if x == None else x.name) + '\n' for x in self.patterns)
    return s

# Represents a Pattern: a sequence of note and effect commands for a single channel.
# Patterns are the lowest-level musical structure, containing the actual note data:
#   - Note lines: (note_name, note_length_or_None, volume_or_None) — 3-element tuples
#   - Effect lines: (effect_name, args_tuple, note_length, volume) — 4-element tuples
# The "Fallthrough" pseudo-command indicates the pattern runs directly into the next
# pattern in memory without an explicit terminator byte (0x00).
class Pattern:
  name = 'Pattern'
  # Formats the pattern as text. Each line is either:
  #   - An effect command: "EffectName arg1 arg2 ..."
  #   - A note: "C#3 12 7f" (note, length, volume) where "--" means "unchanged"
  def __str__(self):
    r = '[Pattern_0x%x]\n' % (self.ea)
    last_len = None
    for a in self.lines:
      s = ''
      # 4-element tuples are effect commands with argument lists
      if len(a) == 4:
        s += a[0] + " " + " ".join(map(to_str, a[1]))
      else:
        # 3-element tuples are note events: (note_str, length_or_None, volume_or_None)
        s += '%s' % (a[0])

        # Note length: "--" means keep the previous length value
        if a[-2] != None:
          s += ' %2d' % a[-2]
          last_len = a[-2]
        else:
          s += ' --'# % last_len

        # Volume/velocity: "--" means keep the previous volume value
        if a[-1] != None:
          s += ' %2x' % a[-1]
        else:
          s += ' --'

      r += s + '\n'
    return r
  
# Represents a loop command within a Song's phrase list.
# When the SPC700 encounters a PhraseLoop, it repeats a section of the song's phrase list
# a given number of times before continuing. This enables song sections to repeat efficiently
# without duplicating phrase data.
# Parameters:
#   loops: number of times to repeat the loop (0 means infinite loop / jump-back)
#   jmp: relative offset (in phrase-list entries) to jump back to
class PhraseLoop:
  name = 'PhraseLoop'
  def __init__(self, loops, jmp):
    self.loops = loops
    self.jmp = jmp
    self.name = 'PhraseLoop %d %d' % (self.loops, self.jmp)
  def __str__(self):
    return self.name

# Global registry mapping SPC700 addresses to decoded objects (Song, Phrase, Pattern, SongList).
# This ensures each address is decoded exactly once, and enables cross-references between objects.
types_for_ea = {}
# Priority queue (min-heap) of (address, object) pairs awaiting decoding.
# Processing in address order is critical: each Pattern needs to know where the next one starts
# to detect "Fallthrough" patterns (those that flow into the next pattern without a terminator).
pqueue_by_ea = []

# Clears both the object registry and the decode queue.
# Called at the start of each sound bank load to prepare for a fresh decode pass.
def reset_queues():
  global types_for_ea, pqueue_by_ea
  types_for_ea = {}
  pqueue_by_ea = []

# Looks up or creates a decoded object of type `tp` (Song, Phrase, Pattern, SongList)
# at the given SPC700 address. Returns None for null pointers (ea=0).
# If the address was already registered, returns the existing object (with a type-match assert).
# If the address is new, creates an instance, names it (e.g., "Pattern_0xD456"), registers it,
# and pushes it onto the priority queue for later decoding.
# Objects whose memory bytes are None (not loaded from any bank) are marked as "imported" —
# they are referenced but their data lives in a different sound bank.
def get_type_for_ea(ea, tp):
  # Null pointer means "no object here"
  if ea == 0:
    return None
  # Sanity check: valid SPC700 data addresses are above the zero-page (0x00-0xFF)
  assert(ea >= 256), ea
  a = types_for_ea.get(ea)
  if a != None:
    # If this address is already registered, it must be the same type we expect
    assert type(a)==tp, (type(a), tp, '0x%x' % ea)
    return a
  # Create a new object instance and assign its address and auto-generated name
  a = tp()
  a.ea = ea
  a.name = '%s_0x%x' % (a.name, ea)
  types_for_ea[ea] = a
  # If this address has loaded data, queue it for decoding; otherwise mark as imported
  if get_byte(ea) != None:
    heapq.heappush(pqueue_by_ea, (ea, a))
    a.is_imported = False
  else:
    a.is_imported = True
  return a

# Effect command byte length table: how many argument bytes follow each effect opcode.
# Indexed by (opcode - 0xE0). For example, Instrument (0xE0) takes 1 byte (the instrument number),
# Vibrato (0xE3) takes 3 bytes (delay, rate, depth), VibratoOff (0xE4) takes 0 bytes.
# The special "Call" effect (0xEF) is handled separately because its arguments include
# a 16-bit pattern address plus a loop count.
kEffectByteLength = [1, 1, 2, 3, 0, 1, 2, 1, 2, 1, 1, 3, 0, 1, 2, 3, 1, 3, 3, 0, 1, 3, 0, 3, 3, 3, 1]
# Human-readable names for each effect command, indexed by (opcode - 0xE0).
# These correspond to opcodes 0xE0 through 0xFA in the SPC700 music engine.
kEffectNames = ['Instrument', 'Pan', 'PanFade', 'Vibrato', 'VibratoOff',
                'SongVolume', 'SongVolumeFade', 'Tempo', 'TempoFade',
                'Transpose', 'ChannelTranpose', 'Tremolo', 'TremoloOff',
                'Volume', 'VolumeFade', 'Call', 'VibratoFade',
                'PitchEnvelopeTo', 'PitchEnvelopeFrom', 'PitchEnvelopeOff',
                'FineTune', 'EchoEnable', 'EchoOff', 'EchoSetup', 'EchoVolumeFade',
                'PitchSlide', 'PercussionDefine']
assert(len(kEffectNames) == 27)

# Converts a numeric note value (0-73) to a human-readable musical string.
# Notes 0-71 map to standard chromatic scale: 0=C-1, 1=C#1, 12=C-2, etc.
# Special values:
#   72 = "-+-" : tie/sustain — tells the engine NOT to write a key-off event
#   73 = "---" : rest — tells the engine to write a key-off (silence the voice)
# The note byte in the SPC700 stream has bit 7 set; the caller strips it (cmd & 0x7F).
def note_to_str(note):
  # Chromatic scale note names; '-' suffix means natural (not sharp)
  kKeys  = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-']
  if note >= 72:
    if note == 72:
      return '-+-' # don't write kof
    elif note == 73:
      return '---' # want kof
    else:
      assert 0
  # Calculate octave (0-5) and semitone within octave (0-11)
  octave = note / 12
  key = note % 12
  # Output format: "C#3" means C-sharp in octave 3 (octave is 1-indexed in output)
  return '%s%d' % (kKeys[key], octave + 1)

# Retrieves or creates a Pattern object for the given SPC700 address.
# Returns None for null pointers (address 0), meaning "no pattern / silent channel".
def get_pattern(ea):
  if ea == 0:
    return None
  pattern = get_type_for_ea(ea, Pattern)
  return pattern

# Retrieves or creates a Song object for the given SPC700 address and assigns its index
# (position in the song list table). Returns None for null pointers.
def get_song(ea, index):
  song = get_type_for_ea(ea, Song)
  if song:
    song.index = index
  return song

# Retrieves or creates a Phrase object for the given SPC700 address.
def get_phrase(ea):
  phrase = get_type_for_ea(ea, Phrase)
  return phrase

# Decodes a binary pattern stream from SPC700 memory into a list of note/effect lines.
# The SPC700 music engine encodes pattern data as a variable-length byte stream where:
#   - Byte 0x00: end-of-pattern terminator
#   - Bytes 0x01-0x7F: set note length (duration in ticks), optionally followed by volume
#   - Bytes 0x80-0xDF: note values (strip bit 7 to get note number 0-95)
#   - Bytes 0xE0-0xFE: effect commands (see kEffectNames), followed by variable-length args
#   - Special: 0xEF is the "Call" command (subroutine call to another pattern)
#
# Parameters:
#   pattern: the Pattern object to populate with decoded lines
#   next_ea: the start address of the next pattern in memory (from the priority queue),
#            used to detect Fallthrough — patterns that lack a 0x00 terminator and instead
#            run directly into the next pattern's data
def decode_pattern(pattern, next_ea):
  ea = pattern.ea
  pattern.lines = []
  start_ea = ea
  while True:
#    print('0x%x 0x%x' % (ea, start_ea))
#    assert ea != 0x28f0
    # If we've read past the start and reached the next pattern's address,
    # this pattern flows into the next one without a terminator
    if ea != start_ea and ea == next_ea:
      pattern.lines.append(('Fallthrough', (), None, None))
      return
    note_length, volstuff = None, None
    cmd = get_byte(ea); ea += 1
    # 0x00 terminates the pattern normally
    if cmd == 0:
      break
    # Bytes below 0x80 are optional prefix bytes that set note length and volume.
    # The engine reads up to two prefix bytes before the actual note or effect command:
    #   1st prefix (if < 0x80): sets note length (duration in ticks)
    #   2nd prefix (if < 0x80): sets volume/velocity index
    if not (cmd & 0x80):
      note_length = cmd
      cmd = get_byte(ea); ea += 1
      if not (cmd & 0x80):
        volstuff = cmd
        cmd = get_byte(ea); ea += 1
    # 0xEF = "Call" effect: subroutine call to another pattern, with a loop count.
    # Arguments: 2-byte pattern address + 1-byte loop count.
    # This is handled separately from other effects because the address argument
    # must be resolved into a Pattern object reference.
    if cmd == 0xef:
      addr = get_word(ea)
      loops = get_byte(ea + 2)
      ea += 3
      pattern.lines.append((kEffectNames[cmd-0xe0], (get_pattern(addr), loops), note_length, volstuff))
    # 0xE0-0xFE (excluding 0xEF): effect commands with argument bytes per kEffectByteLength.
    # Effects cannot have note_length or volume prefixes (those only apply to note events).
    elif cmd >= 0xe0:
      assert note_length == None and volstuff == None, (note_length, volstuff)
      x = kEffectByteLength[cmd - 0xe0]
      args = [get_byte(ea+i) for i in range(x)]
      ea += x
      pattern.lines.append((kEffectNames[cmd-0xe0], args, note_length, volstuff))
    # 0x80-0xDF: note event. Strip bit 7 to get the note number (0-95),
    # then convert to musical notation via note_to_str.
    else:
      assert(cmd & 0x80)
      pattern.lines.append((note_to_str(cmd & 0x7f), note_length, volstuff))

  return pattern

# Decodes a Phrase by reading 8 consecutive 16-bit pointers (one per SPC700 channel)
# from SPC700 memory and resolving each to a Pattern object (or None for silent channels).
def decode_phrase(phrase):
  phrase.patterns = [get_pattern(get_word(phrase.ea + i * 2)) for i in range(8)]

# Decodes a Song by reading its phrase list from SPC700 memory.
# The song data is a sequence of 16-bit values:
#   - Values >= 0x100: pointer to a Phrase object (8-channel pattern group)
#   - Values 0x01-0xFF (but not 0x80/0x81): loop command — the value is the loop count,
#     followed by a 2-byte target address to jump back to. The target must reference
#     an earlier position in the same song's phrase list.
#   - Value 0x0000: end of song
# The eas_in_phrase list tracks each read position so loop targets can be validated.
def decode_song(song):
  ea = song.ea
  song.phrases = []
  ea_org = ea
  eas_in_phrase = []
  while True:
    eas_in_phrase.append(ea)
    phrase = get_word(ea)
    # 0x0000 marks end of song
    if phrase == 0:
      break
    # Low values (< 0x100) are loop commands, not phrase pointers.
    # 0x80 and 0x81 are reserved/invalid loop counts.
    if phrase < 0x100:
      assert phrase != 0x80 and phrase != 0x81
      # Read the 2-byte jump target address and compute relative offset in phrase entries
      tgt = get_word(ea + 2)
      assert tgt in eas_in_phrase
      song.phrases.append(PhraseLoop(phrase, (tgt - ea) // 2))
      ea += 4
    else:
      # Normal phrase pointer — resolve to a Phrase object
      song.phrases.append(get_phrase(phrase))
      ea += 2
  return song

# Dispatches decoding to the appropriate type-specific decoder based on the object's class.
# This is the central decode router used by the priority-queue processing loop in print_song().
# SongList objects need no decoding because their child songs are already resolved during creation.
#
# Parameters:
#   what: a decoded object (Song, SongList, Phrase, or Pattern) to be populated with data
#   next_ea: the SPC700 address of the next object in the queue, passed to decode_pattern()
#            so it can detect Fallthrough patterns that lack explicit terminators
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

# Creates a SongList object at the given address by reading an array of 16-bit song pointers.
# Each pointer is resolved into a Song object (or None for null entries).
# The song list table always lives at 0xD000 in SPC700 memory.
#
# Parameters:
#   ea: SPC700 address of the song list table (typically 0xD000)
#   num: number of songs in this bank, determined by the caller from table geometry
def get_song_list(ea, num):
  song_list = get_type_for_ea(ea, SongList)
  song_list.songs  = [get_song(get_word(ea + i * 2), i) for i in range(num)]

# Loads one of the four sound banks from the SNES ROM into the global SPC700 memory image.
# Each bank lives at a fixed ROM address and contains a different set of songs.
# After loading, the song count is determined: for intro/lightworld banks, the first pointer
# in the song table at 0xD000 tells us where the table ends (self-describing). For
# indoor/ending banks, the table has a fixed size of 35 entries (0xD046 - 0xD000 = 70 bytes / 2).
#
# Parameters:
#   ROM: a LoadedRom object for reading raw ROM data
#   song: bank name string — one of 'intro', 'lightworld', 'indoor', 'ending'
# Side effects: sets global `memory` (64KB SPC700 image) and `SONGS_IN_BANK` (song count)
def load_song(ROM, song):
  global memory, SONGS_IN_BANK
  # Clear the object registry and decode queue for a fresh bank
  reset_queues()
  if song == 'intro':
    # Intro bank at ROM address 0x998000 — title screen and opening sequence music
    memory, entry_point = load_sound_bank(ROM, 0x998000) # intro
    # Self-describing table: first pointer value minus table base, divided by 2 bytes per entry
    SONGS_IN_BANK = (get_word(0xd000) - 0xd000) // 2
  elif song == 'lightworld':
    # Light world overworld bank at ROM 0x9A9EF5 — outdoor exploration music
    memory, entry_point = load_sound_bank(ROM, 0x9a9ef5) # lw
    SONGS_IN_BANK = (get_word(0xd000) - 0xd000) // 2
  elif song == 'indoor':
    # Indoor/dungeon bank at ROM 0x9B8000 — cave, dungeon, and building music
    memory, entry_point = load_sound_bank(ROM, 0x9b8000) # indoor
    # Fixed table size: 35 entries (0x46 bytes / 2 bytes per pointer)
    SONGS_IN_BANK = (0xd046 - 0xd000) // 2
  elif song == 'ending':
    # Ending/credits bank at ROM 0x9AD380 — ending sequence and credits music
    memory, entry_point = load_sound_bank(ROM, 0x9ad380) # ending
    SONGS_IN_BANK = (0xd046 - 0xd000) // 2


# Main output function: decodes all music objects in the current bank and prints them to a file.
# Processing works in three stages:
#   1. Bootstrap: create the SongList at 0xD000, which triggers recursive discovery of
#      all Songs, Phrases, and Patterns reachable from the song table pointers.
#   2. Force-register orphan objects: some Phrases and Patterns are not reachable from the
#      song table (they may be referenced only by SFX or shared across banks). These must be
#      manually registered by their known addresses so they are included in the output.
#   3. Priority-queue drain: process all discovered objects in ascending address order,
#      decoding each one. Address ordering is critical because Pattern decoding needs to know
#      where the next pattern starts (to detect Fallthrough patterns without terminators).
#   4. Final sorted output: print every non-imported object in address order to the file.
#
# Parameters:
#   song: bank name string ('intro', 'lightworld', 'indoor', 'ending')
#   f: writable file object for the text output
def print_song(song, f):
  # Stage 1: build the song list, which recursively queues all reachable objects
  get_song_list(0xd000, SONGS_IN_BANK)
  # Stage 2: force-register orphan phrases/patterns that are not reachable from the song table
  # but are known to exist in this bank's memory region
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
  # Stage 3: drain the priority queue, decoding each object in ascending address order.
  # Passing the next item's address lets decode_pattern detect Fallthrough boundaries.
  while len(pqueue_by_ea):
    _, item = heapq.heappop(pqueue_by_ea)
    decode_any(item, pqueue_by_ea[0][0] if len(pqueue_by_ea) else None)
  # Stage 4: output all decoded objects sorted by address, skipping imported (cross-bank) refs
  for a, b in sorted(types_for_ea.items()):
    if not b.is_imported:
      print(b, file = f)


# Extracts all 25 BRR audio samples from SPC700 memory and saves them as both raw BRR
# and decoded PCM files. The BRR sample directory at 0x3C00 has 4 bytes per entry:
#   bytes 0-1: start address of BRR data in SPC700 memory
#   bytes 2-3: loop point address (where playback restarts if looping is enabled)
#
# Each sample is saved as:
#   sound/soundN.pcm.brr — raw BRR-encoded bytes (9 bytes per 16-sample block)
#   sound/soundN.pcm     — decoded 16-bit PCM audio data
#
# Parameters: none (reads from the global SPC700 memory image)
# Side effects: writes 50 files (25 .pcm.brr + 25 .pcm) to the sound/ directory
def dump_brr_audio():
  # Inner helper: reads one BRR sample from the sample directory and decodes it.
  # Returns: (pcm_data, raw_brr_bytes, has_loop_flag)
  # The BRR block count is derived from PCM length: each 9-byte BRR block decodes to 16 samples.
  # Bit 1 of the first BRR block header byte is the loop enable flag.
  def decode_brr(snd):
    # Look up sample start and loop addresses from the directory at 0x3C00
    start, loop_start = get_word(0x3c00 + snd * 4), get_word(0x3c00 + snd * 4 + 2)
    # Decode BRR to PCM using a lambda that reads bytes relative to the sample start
    r = brr_tools.decode_brr(lambda x: get_byte(start+x))
    # Extract the raw BRR bytes (9 bytes per 16-sample block) and check loop flag (bit 1)
    return r, [get_byte(start+x) for x in range(len(r)//16 * 9)], get_byte(start)&0x2 != 0
  # Process all 25 instrument samples defined in the sample directory
  for audio_idx in range(25):
    sound_data, brr_data, brr_repeat = decode_brr(audio_idx)
    # Write raw BRR data for potential re-encoding or analysis
    open('sound/sound%d.pcm.brr' % audio_idx, 'wb').write(bytes(brr_data))
    # Write decoded PCM for audio playback or waveform inspection
    open('sound/sound%d.pcm' % audio_idx, 'wb').write(sound_data)

# Extracts all instrument metadata from SPC700 memory and writes it to music_info.yaml.
# This captures three data structures:
#   1. Sample directory (0x3C00): 25 entries x 4 bytes — maps sample indices to PCM files
#      and loop points. Some samples are duplicates (10->9, 20->19) sharing the same PCM file.
#   2. Instrument table (0x3D00): 25 entries x 6 bytes — defines each instrument's sample,
#      ADSR envelope, and base pitch for the music engine.
#   3. SFX instrument table (0x3E00): 25 entries x 9 bytes — defines each SFX instrument's
#      stereo volume, pitch, sample, ADSR envelope, and pitch base.
# Also extracts note_gate_off (8 bytes at 0x3D96) and note_volume (16 bytes at 0x3D9E) tables.
#
# Parameters: none (reads from global SPC700 memory image)
# Side effects: writes music_info.yaml to the current directory
def dump_music_info():
  music_info = {}
  # Samples 10 and 20 are byte-identical duplicates of samples 9 and 19 respectively;
  # map them to the same PCM file to avoid redundant extraction
  kDupSamples = {10 : 9, 20 : 19}
  music_info['samples'] = []
  # Read all 25 sample directory entries from the directory at 0x3C00
  for audio_idx in range(25):
    # Each directory entry: 2-byte start address + 2-byte loop address
    start, rep = get_word(0x3c00 + audio_idx * 4), get_word(0x3c00 + audio_idx * 4 + 2)
    sample_info = {
      'file' : 'sound/sound%d.pcm' % kDupSamples.get(audio_idx, audio_idx)
    }
    # Bit 1 of the first BRR block header indicates whether looping is enabled;
    # if so, compute the loop point in PCM samples (each 9-byte BRR block = 16 PCM samples)
    if get_byte(start) & 2:
      sample_info['repeat'] = (rep - start) // 9 * 16
    music_info['samples'].append(sample_info)

  # Helper: reads 3 bytes (ADSR1, ADSR2, GAIN) and unpacks them into named envelope fields.
  # ADSR1 layout: bit 7 = ADSR enable, bits 6-4 = decay rate (3 bits), bits 3-0 = attack rate
  # ADSR2 layout: bits 7-5 = sustain level (3 bits), bits 4-0 = sustain rate (5 bits)
  # GAIN: raw VxGAIN register value (used when ADSR is disabled)
  #
  # Parameters:
  #   ea: SPC700 address of the first envelope byte (ADSR1)
  #   info: dictionary to populate with the decoded envelope fields
  def add_sustain_decay_etc(ea, info):
    adsr1, adsr2, gain = get_byte(ea), get_byte(ea + 1), get_byte(ea + 2)
    # Extract decay rate from bits 6-4 of ADSR1 (3-bit field, values 0-7)
    info['decay'] = (adsr1 >> 4) & 7
    # Extract attack rate from bits 3-0 of ADSR1 (4-bit field, values 0-15)
    info['attack'] = adsr1 & 0xf
    # Extract sustain level from bits 7-5 of ADSR2 (3-bit field, maps to 1/8 through 8/8)
    info['sustain_level'] = adsr2 >> 5
    # Extract sustain rate from bits 4-0 of ADSR2 (5-bit field, values 0-31)
    info['sustain_rate'] = adsr2 & 0x1f
    # Raw GAIN register value — controls volume envelope when ADSR mode is disabled
    info['vxgain'] = gain

  # Read the 25 music instrument definitions from the table at 0x3D00.
  # Each entry is 6 bytes: sample_index, ADSR1, ADSR2, GAIN, pitch_hi, pitch_lo
  music_info['instruments'] = []
  for i in range(25):
    ea = 0x3d00 + i * 6
    adsr1, adsr2 = get_byte(ea + 1), get_byte(ea + 2)
    info = {
      # Which BRR sample (0-24) this instrument plays
      'sample' : get_byte(ea),
    }
    # Unpack ADSR1/ADSR2/GAIN bytes starting at offset +1 within the entry
    add_sustain_decay_etc(ea + 1, info)
    # 16-bit base pitch: high byte at offset +4, low byte at offset +5
    info['pitch_base'] = get_byte(ea + 4) << 8 | get_byte(ea + 5)
    music_info['instruments'].append(info)

  # Note gate-off table (8 entries at 0x3D96): controls how quickly notes release
  music_info['note_gate_off'] = [get_byte(i) for i in range(0x3D96, 0x3D96 + 8)]
  # Note volume table (16 entries at 0x3D9E): velocity-to-volume mapping curve
  music_info['note_volume'] = [get_byte(i) for i in range(0x3D9E, 0x3D9E + 16)]

  # Read the 25 SFX instrument definitions from the table at 0x3E00.
  # Each entry is 9 bytes: volL, volR, pitch(16-bit), sample, ADSR1, ADSR2, GAIN, pitch_base
  music_info['sfx_instruments'] = []

  for i in range(25):
    ea = 0x3e00 + i * 9
    info = {
      # Left and right stereo volume for this SFX instrument
      'voll' : get_byte(ea),
      'volr' : get_byte(ea + 1),
      # 16-bit pitch value (little-endian) controlling playback frequency
      'pitch' : get_word(ea + 2),
      # Which BRR sample (0-24) this SFX instrument uses
      'sample' : get_byte(ea + 4)
    }
    # Unpack ADSR1/ADSR2/GAIN bytes starting at offset +5 within the SFX entry
    add_sustain_decay_etc(ea + 5, info)
    # Single-byte pitch base multiplier for SFX (unlike music instruments which use 16-bit)
    info['pitch_base'] = get_byte(ea + 8)
    music_info['sfx_instruments'].append(info)

  # Serialize the complete metadata dictionary to YAML for human readability
  s = yaml.dump(music_info, default_flow_style=None, sort_keys=False)
  open('music_info.yaml', 'w').write(s)

# Decodes a single SFX (sound effect) pattern from SPC700 memory into a list of command tuples.
# SFX patterns use a similar but distinct encoding from music patterns:
#   - Optional prefix bytes (all < 0x80): note_length, then volume_left, then volume_right
#   - Command byte (bit 7 set): determines the action
#   - 0x00: end of pattern
#   - 0xE0: SetInstrument — changes the active SFX instrument (1-byte argument)
#   - 0xF9: PitchSlide with note — slides pitch with a target note + 3 slide parameters
#   - 0xF1: PitchSlide without note — slides pitch using only 3 slide parameters
#   - 0xFF: Restart — loops the SFX from the beginning (terminates decoding)
#   - Other: plain note command (strip bit 7 for note value)
#
# Unlike music patterns which have mono volume, SFX can set independent L/R stereo volumes.
#
# Parameters:
#   ea: SPC700 address where this SFX pattern begins
#   next_addr: address of the next SFX pattern, used to detect Fallthrough
# Returns: list of tuples representing decoded SFX commands
def decode_sfx(ea, next_addr):
  r = []
  while True:
    # If we've reached the next SFX pattern's address, this one flows into it
    if ea == next_addr:
      r.append(('Fallthrough', ))
      return r
    b = get_byte(ea); ea += 1
    # 0x00 terminates the SFX pattern normally
    if b == 0:
      return r
    note_length = None
    volume_left, volume_right = None, None
    # Read optional prefix bytes (all values < 0x80) that precede the command byte.
    # Up to 3 prefixes: note_length, then volume_left, then volume_right.
    # Each prefix consumed shifts to reading the next byte as a potential prefix or command.
    if not (b & 0x80):
      note_length = b
      b = get_byte(ea); ea += 1
      if not (b & 0x80):
        volume_left, volume_right = b, None
        b = get_byte(ea); ea += 1
        if not b & 0x80:
          volume_right = b
          b = get_byte(ea); ea += 1
    # 0xE0: SetInstrument — selects which SFX instrument (from the table at 0x3E00) to use.
    # Must appear without any prefix bytes (no note_length or volume context).
    if b == 0xe0:
      assert note_length == None and volume_left == None and volume_right == None, ea
      b = get_byte(ea); ea += 1
      r.append(('SetInstrument %d' % b, ))
    # 0xF9: PitchSlide with explicit target note.
    # Reads 1 byte for the target note, then 3 bytes of slide parameters (delay, rate, depth).
    elif b == 0xf9:
      #assert note_length == None and volume_left == None and volume_right == None, ea
      b = get_byte(ea); ea += 1
      b0, b1, b2 = get_byte(ea), get_byte(ea+1), get_byte(ea+2); ea += 3
      r.append(('PitchSlide %d %d %d' % (b0, b1, b2), note_to_str(b & 0x7f), note_length, volume_left, volume_right))
    # 0xF1: PitchSlide without a target note (slide from current pitch).
    # Reads only the 3 slide parameter bytes.
    elif b == 0xf1:
      #assert note_length == None and volume_left == None and volume_right == None, ea
      b0, b1, b2 = get_byte(ea), get_byte(ea+1), get_byte(ea+2); ea += 3
      r.append(('PitchSlide %d %d %d' % (b0, b1, b2), None, note_length, volume_left, volume_right))
    # 0xFF: Restart — tells the SFX engine to loop this pattern from the beginning.
    # Must appear without prefix bytes. Terminates decoding since it is an unconditional jump.
    elif b == 0xff:
      assert note_length == None and volume_left == None and volume_right == None, ea
      r.append(('Restart',))
      return r
    # Any other value with bit 7 set: plain note event with optional length and stereo volume
    else:
      r.append((None, note_to_str(b & 0x7f), note_length, volume_left, volume_right))

# Outputs all three SFX port tables and their referenced SFX patterns to a text file.
# The SPC700 engine has 3 SFX ports, each with its own pointer table:
#   - SfxPort1 at 0x17C0: 32 entries (simpler format — no echo parameter)
#   - SfxPort2 at 0x1820: 63 entries (includes echo enable flag per SFX)
#   - SfxPort3 at 0x191C: 63 entries (includes echo enable flag per SFX)
# Each port table stores: N x 2-byte SFX pattern pointers, then N x 1-byte priority values,
# then (for ports 2 and 3) N x 1-byte echo enable flags.
#
# After printing all three port tables, this function collects every unique SFX pattern
# address (plus known orphan addresses not referenced by any port table), sorts them,
# and decodes each SFX pattern in address order.
#
# Parameters:
#   f: writable file object for the sfx.txt output
def print_all_sfx(f):
  # Collect all unique SFX pattern addresses referenced by the three port tables
  items = set()
  # Inner helper: prints one SFX port table header and its entries.
  # Memory layout per port: [num*2 bytes of pointers][num bytes of priority][num bytes of echo]
  # SfxPort1 lacks the echo byte array, so its entries have only pointer + priority.
  #
  # Parameters:
  #   base: SPC700 address where this port's pointer table begins
  #   num: number of SFX entries in this port
  #   name: label string for text output ('SfxPort1', 'SfxPort2', 'SfxPort3')
  def add_sfx_top(base, num, name):
    print('[%s_0x%x]' % (name, base), file = f)
    # Priority bytes immediately follow the pointer table
    next_ea = base + num * 2
    # Echo enable bytes follow the priority bytes (only used by Port2 and Port3)
    echo_ea = next_ea + num
    for i in range(num):
      r = []
      # Read the 16-bit SFX pattern pointer for this entry
      ea = get_word(base + i * 2)
      if ea == 0:
        t = 'None'
      else:
        # Track this address so we decode its pattern data later
        items.add(ea)
        t = 'Sfx_0x%x' % ea
      # SfxPort1 has only a priority byte; Port2/Port3 also have an echo enable byte
      if name == 'SfxPort1':
        print('%s,%d' % (t, get_byte(next_ea + i)), file = f)
      else:
        print('%s,%d,%d' % (t, get_byte(next_ea + i), get_byte(echo_ea + i)), file = f)
    print(file = f)
  # Process all three SFX port tables in order
  add_sfx_top(0x17c0, 32, 'SfxPort1')
  add_sfx_top(0x1820, 63, 'SfxPort2')
  add_sfx_top(0x191c, 63, 'SfxPort3')
  # Register known orphan SFX pattern addresses that are not referenced by any port table
  # but exist in SPC700 memory. These may be sub-patterns called by other SFX, or leftover
  # data from development. Including them ensures complete coverage of all SFX data.
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
  # Sort all collected addresses so we can decode patterns in memory order
  # (needed to detect Fallthrough between consecutive SFX patterns)
  items = sorted(list(items))
  # Decode and print each SFX pattern, passing the next pattern's address for Fallthrough detection
  for i in range(len(items)):
    print('[Sfx_0x%x]' % items[i], file = f)
    # The next pattern's address is needed by decode_sfx for Fallthrough detection;
    # 0 signals "no next pattern" for the last entry
    next_addr = items[i + 1] if i + 1 < len(items) else 0
    rs = decode_sfx(items[i], next_addr)
    for r in rs:
      # 5-element tuples are note/pitch commands: (effect, note, length, volL, volR)
      # Format each field with placeholder strings for None values
      if len(r) == 5:
        aa = '.  ' if r[1] == None else r[1]
        bb = '--' if r[2] == None else '%2d' % r[2]
        cc = '---' if r[3] == None else '%3d' % r[3]
        dd = '---' if r[4] == None else '%3d' % r[4]
        r0 = '' if r[0] == None else ' ' + r[0]
        print('%s %s %s %s%s' % (aa, bb, cc, dd, r0), file = f)
      # 1-element tuples are standalone commands (SetInstrument, Restart, Fallthrough)
      else:
        print(r[0], file = f)
    print(file = f)

# Top-level orchestrator for extracting all sound data from the ROM.
# Called by extract_resources.py as part of the full asset extraction pipeline.
# Processes banks in a specific order because BRR samples, instrument metadata, and SFX
# data are extracted from the intro bank's memory image (they share the same SPC700 addresses
# across banks, but the intro bank is loaded first and used as the reference).
#
# Output files produced:
#   sound_intro.txt   — decoded music for the intro/title screen bank
#   sound_indoor.txt  — decoded music for the indoor/dungeon bank
#   sound_ending.txt  — decoded music for the ending/credits bank
#   sfx.txt           — all SFX port tables and pattern data
#   music_info.yaml   — instrument metadata (ADSR, pitch, samples)
#   sound/sound*.pcm  — 25 decoded PCM audio samples
#   sound/sound*.pcm.brr — 25 raw BRR audio samples
#
# Parameters:
#   rom: a LoadedRom object for reading raw ROM data
# Note: the lightworld bank is not extracted here; it is handled separately by extract_music.py
def extract_sound_data(rom):
  # Load and decode the intro bank first — its memory image also provides BRR/instrument/SFX data
  load_song(rom, 'intro')
  print_song('intro', open('sound_intro.txt', 'w'))

  # Extract BRR samples and instrument metadata while intro bank is still in memory
  dump_brr_audio()
  dump_music_info()
  # Extract all SFX patterns and port tables from the intro bank's memory
  print_all_sfx(open('sfx.txt', 'w'))

  # Load and decode remaining banks (each overwrites the global memory image)
  load_song(rom, 'indoor')
  print_song('indoor', open('sound_indoor.txt', 'w'))

  load_song(rom, 'ending')
  print_song('ending', open('sound_ending.txt', 'w'))

# Standalone entry point: decode a single sound bank and print to stdout.
# Usage: python decode_music.py <rom_path> <bank_name>
# Where bank_name is one of: intro, lightworld, indoor, ending
if __name__ == "__main__":
  ROM = util.LoadedRom(sys.argv[1])
  song = sys.argv[2]

  load_song(ROM, song)
  print_song(song, sys.stdout)

