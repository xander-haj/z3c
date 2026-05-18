# =============================================================================
# text_compression.py — Dialogue Text Compression/Decompression Codec
#
# Part of the zelda3 asset pipeline. This module handles the custom text
# encoding used by The Legend of Zelda: A Link to the Past (SNES) for all
# in-game dialogue. The SNES ROM stores dialogue as compressed byte streams
# where each byte represents one of four things:
#   1. A character — an index into a language-specific alphabet table
#   2. A command — menu choices, color changes, sound effects, waits, etc.
#   3. A dictionary reference — a pointer to a common substring stored once
#   4. A control byte — bank switch (to continue reading from another ROM
#      address) or end-of-message sentinel
#
# Two encoding formats exist:
#   "org" (original) — used by the US release. Byte ranges:
#       0x00-0x66 = printable characters (alphabet index)
#       0x67-0x7F = command bytes (variable length, 1 or 2 bytes)
#       0x80      = bank switch (jump to next ROM address block)
#       0x88+     = dictionary entries (byte minus 0x88 = dictionary index)
#       0xFF      = end of all dialogue data
#   "new" (European) — used by translated ROMs (DE, FR, etc.). Byte ranges:
#       0x00-0x6F = printable characters (more room for accented chars)
#       0x70-0x87 = command bytes
#       0x88      = bank switch
#       0x90+     = dictionary entries (byte minus 0x90 = dictionary index)
#       0x8F      = end of all dialogue data
#
# Each Lang* class below defines the language-specific alphabet, dictionary,
# command tables, ROM addresses, and which encoding format it uses.
#
# Key functions:
#   decode_strings_generic() — reads ROM bytes into human-readable strings
#   compress_strings()       — re-encodes strings back to ROM byte format
#   encode_dictionary()      — converts dictionary words to byte arrays
#   dialogue_filename()      — maps language codes to dialogue file paths
# =============================================================================

# util provides ROM loading and byte-access helpers; sys provides CLI argv access
import util, sys

# US alphabet table: maps byte values 0x00-0x5E to printable characters.
# Indices 0-25 = uppercase A-Z, 26-51 = lowercase a-z, 52-61 = digits 0-9,
# 62-63 = punctuation (! ?), 64+ = special glyphs (ellipsis, arrows, hearts,
# button icons, etc.). Entries like "[LinkL]" and "[LinkR]" are left/right halves
# of the Link sprite used inline in dialogue. "[A]", "[B]", "[X]", "[Y]" render
# as SNES controller button icons.
kTextAlphabet_US = [
  "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", # 0 - 15
  "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "a", "b", "c", "d", "e", "f", # 16 - 31
  "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", # 32 - 47
  "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "!", "?", # 48 - 63
  "-", ".", ",", 
  # 64 - 79
  "[...]", ">", "(", ")",
  "[Ankh]", "[Waves]", "[Snake]", "[LinkL]", "[LinkR]",
  "\"", "[Up]", "[Down]", "[Left]",
  # 80 - 95
  "[Right]", "'", "[1HeartL]", "[1HeartR]", "[2HeartL]", "[3HeartL]", "[3HeartR]",
  "[4HeartL]", "[4HeartR]", " ", "<", "[A]", "[B]", "[X]", "[Y]",
]

# German alphabet table: extends the base A-Z/a-z/0-9 set with umlauts
# (ö, ü, ß, ä, Ä, Ö, Ü) and French accented characters (è, é, ê, à, ù, ç)
# that appear in the European ROM. Uses split directional arrow glyphs
# (e.g., "[UpL]"/"[UpR]" as left/right halves) unlike the US single-glyph arrows.
kTextAlphabet_DE = [
  "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", # 0 - 15
  "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "a", "b", "c", "d", "e", "f", # 16 - 31
  "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", # 32 - 47
  "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "!", "?", # 48 - 63
  # 64 - 79
  "-", ".", ",",  "[...]", ">", "(", ")",
  "[Ankh]", "[Waves]", "[Snake]", "[LinkL]", "[LinkR]",
  "\"", "[UpL]", "[UpR]", "[LeftL]",
  # 80 - 95
  "[LeftR]", "'", "[1HeartL]", "[1HeartR]", "[2HeartL]", "[3HeartL]", "[3HeartR]",
  "[4HeartL]", "[4HeartR]", " ", "ö", "[A]", "[B]", "[X]", "[Y]", "ü",
  # 96-111
  "ß", ":", "[DownL]", "[DownR]", "[RightL]", "[RightR]",
  "è", "é", "ê", "à", "ù", "ç", "Ä", "Ö", "Ü", "ä"
  # 112-
]

# French alphabet table: same base layout as German but replaces some
# language-specific accented characters (ô instead of ß, â/û/î instead of Ä/Ö/Ü).
# Shares the European split-arrow glyph convention with the German table.
kTextAlphabet_FR = [
  "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", # 0 - 15
  "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "a", "b", "c", "d", "e", "f", # 16 - 31
  "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", # 32 - 47
  "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "!", "?", # 48 - 63
  # 64 - 79
  "-", ".", ",",  "[...]", ">", "(", ")",
  "[Ankh]", "[Waves]", "[Snake]", "[LinkL]", "[LinkR]",
  "\"", "[UpL]", "[UpR]", "[LeftL]",
  # 80 - 95
  "[LeftR]", "'", "[1HeartL]", "[1HeartR]", "[2HeartL]", "[3HeartL]", "[3HeartR]",
  "[4HeartL]", "[4HeartR]", " ", "ö", "[A]", "[B]", "[X]", "[Y]", "ü",
  # 96-111
  "ô", ":", "[DownL]", "[DownR]", "[RightL]", "[RightR]",
  "è", "é", "ê", "à", "ù", "ç", "â", "û", "î", "ä"
  # 112-
]

# US command byte lengths: each entry corresponds to a command in kText_CommandNames_US.
# A value of 1 means the command is a single byte; 2 means it takes one parameter byte
# after the command byte. Commands start at byte 0x67 in the US format, so command
# index 0 maps to byte 0x67, index 1 to 0x68, etc.
kText_CommandLengths_US = [1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, ]
# Human-readable names for each US command byte. These appear in decoded dialogue
# as [CommandName] or [CommandName NN] markup (e.g., "[Color 02]", "[Wait 05]").
# Commands control text rendering behavior: NextPic advances to the next picture,
# Choose/Choose2/Choose3 present player selection menus, Window sets the dialogue
# box type, Speed/ScrollSpd control text draw rate, Sound triggers SFX, etc.
kText_CommandNames_US = [
  "NextPic", "Choose", "Item", "Name", "Window", "Number",
  "Position","ScrollSpd", "Selchg", "Unused_Crash", "Choose3",
  "Choose2", "Scroll", "1", "2", "3", "Color",
  "Wait", "Sound", "Speed", "Unused_Mark", "Unused_Mark2", "Unused_Clear",
  "Waitkey"
]

# US dictionary table: common substrings stored once and referenced by index.
# During encoding, byte values 0x88+ map to these entries (byte - 0x88 = index).
# The compressor uses greedy matching to replace the longest matching substring
# with a single dictionary byte, reducing dialogue storage size significantly.
# Entries include common English fragments like "'s ", "and ", "the", "you", etc.
kTextDictionary_US = [
'    ', '   ', '  ', "'s ", 'and ', 
'are ', 'all ', 'ain', 'and', 'at ', 
'ast', 'an', 'at', 'ble', 'ba', 
'be', 'bo', 'can ', 'che', 'com', 
'ck', 'des', 'di', 'do', 'en ', 
'er ', 'ear', 'ent', 'ed ', 'en', 
'er', 'ev', 'for', 'fro', 'give ', 
'get', 'go', 'have', 'has', 'her', 
'hi', 'ha', 'ight ', 'ing ', 'in', 
'is', 'it', 'just', 'know', 'ly ', 
'la', 'lo', 'man', 'ma', 'me', 
'mu', "n't ", 'non', 'not', 'open', 
'ound', 'out ', 'of', 'on', 'or', 
'per', 'ple', 'pow', 'pro', 're ', 
're', 'some', 'se', 'sh', 'so', 
'st', 'ter ', 'thin', 'ter', 'tha', 
'the', 'thi', 'to', 'tr', 'up', 
'ver', 'with', 'wa', 'we', 'wh', 
'wi', 'you', 'Her', 'Tha', 'The', 
'Thi', 'You', 
]


# European command byte lengths: same concept as the US table but for the "new"
# encoding format. Commands start at byte 0x70 (vs 0x67 in US), giving the
# European format more room for character bytes (0x00-0x6F instead of 0x00-0x66).
# This extra space accommodates accented characters needed by European languages.
kText_CommandLengths_EU = [1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2]
# European command names: same commands as US but in a different order.
# The reordering reflects the different byte layout of the European ROM format.
kText_CommandNames_EU = [
  "Selchg", "Choose3", "Choose2", "Scroll", "1", "2", "3",
  "Color", "Wait", "Sound", "Speed", "Mark", "Mark2",
  "Clear", "Waitkey", "EndMessage", "NextPic", "Choose",
  "Item", "Name", "Window", "Number", "Position", "ScrollSpd",
]

# German dictionary table: common German substrings for compression.
# Includes German-specific fragments like "nicht" (not), "und" (and), "der/die/das"
# (the), and proper nouns from the game like "Hyrule", "Zelda", "Master-Schwert".
kTextDictionary_DE = [
'    ', '   ', '                                          ', '-Knopf', ' ich ', 
' Sch', ' Ver', ' zu ', ' es ', 'aber', 
'alle', 'auch', 'ang', 'aus', 'auf', 
'an', 'bist', 'bin', 'bei', 'der ', 
'die ', 'das ', 'den ', 'dem ', 'daß', 
'der', 'die', 'das', 'den', 'da', 
'etwas', 'ein ', 'ein', 'en ', 'er ', 
'es ', 'en', 'er', 'es', 'ei', 
'für', 'fe', 'habe', 'hier', 'hast', 
'her', 'ich ', 'icht', 'ich', 'ist', 
'ie ', 'im', 'ie', 'kannst ', 'kannst', 
'kommen', 'kann ', 'll', 'mich', 'mein', 
'mit', 'mal', 'mir', 'nicht ', 'nicht', 
'nen', 'nn', 'och ', 'och', 'or', 
'schon', 'sich', 'sein', 'sch', 'sie', 
'st', 'tte', 'te ', 'te', 'und ', 
'und', 'ung', 'um', 'von', 'ver', 
'vor', 'wird', 'zu ', 'Amulett', 'Aber', 
'Deine', 'Dich ', 'Dir ', 'Dir', 'Der', 
'Die', 'Das', 'Du ', 'Du', 'Da', 
'Ein', 'Hyrule', 'Hier', 'Ich ', 'Master-Schwert', 
'Mach', 'Rubine', 'Sch', 'Sie', 'Ver', 
'Weisen', 'Zelda', 
]

# French dictionary table: common French substrings for compression.
# Includes full phrases like ", c'est moi, Sahasrahla" and game-specific terms
# like "Ténèbres" (Darkness), "Perle de Lune" (Moon Pearl), "Ganon".
kTextDictionary_FR = [
'                                          ', ' de ', ' la ', ' le ', ' ! ', 
' d', ' p', ' t', ' !', ", c'est moi, Sahasrahla", 
', ', 'ais ', 'as ', 'an', 'ai', 
'a ', 'che', 'ce', 'ch', 'dans ', 
'des ', 'de ', 'de', 'est ', 'ent', 
'en ', 'er ', 'es ', 'en', 'es', 
'et', 'eu', 'e,', 'e ', 'ique', 
'ien', 'is ', 'ie', 'in', 'ir', 
'is', 'i ', 'les ', 'la ', 'le ', 
'le', 'll', 'maintenant', 'magique', 'ment', 
'mon', 'mai', 'me', 'ne ', 'onne', 
'oir', 'our', 'ouv', 'oi', 'on', 
'ou', 'or', 'pouvoir', 'pour', 'peux', 
'pas', 'que ', 'qu', 'rubis', 're ', 
'ra', 're', 'r ', 'sorcier', 's l', 
's d', 'se', 'so', 's ', 'tro', 
'te ', 'tu ', 'te', 't ', 'un', 
'ur', 'u ', 'ver', 'Ah ! Ah ! Ah !', "C'est", 
'Ganon', 'Maintenant', 'Merci', 'Monde', 'Perle de Lune', 
'Tu as trouvé ', 'Ténèbres', 'Tu peux', 'Tu ',
]


# Encodes a command using the original US ROM format.
# Looks up the command name in kText_CommandNames_US, validates that the parameter
# count matches (1-byte commands have no param, 2-byte commands require one),
# and returns the encoded byte(s). The command byte is the command's index offset
# by 0x67 (the start of the command range in the US format).
# Parameters:
#   cmd   — command name string (e.g., "Color", "Wait", "Choose")
#   param — integer parameter for 2-byte commands, or None for 1-byte commands
# Returns: list of 1 or 2 encoded bytes
# Uses the original format
def org_encoder(cmd, param):
    if cmd not in kText_CommandNames_US:
      raise Exception(f'Invalid cmd {cmd}')
    cmd_index = kText_CommandNames_US.index(cmd)
    if kText_CommandLengths_US[cmd_index] != (1 if param == None else 2):
      raise Exception(f'Invalid cmd params {cmd} {param}')
    if param == None:
      return [cmd_index + 0x67]
    return [cmd_index + 0x67, int(param)]

# Command encoding table for the "new" (European) format.
# Each entry maps a command name to a tuple: (primary_byte, parameter_info).
# Parameter info is one of:
#   - absent (tuple has length 1): command has no parameters, emit only primary_byte
#   - an int: command has no user param, emit (primary_byte, fixed_int)
#   - a dict: maps user-supplied param values to secondary bytes. Some commands
#     multiplex through byte 0x87 with different secondary byte ranges:
#     Wait uses 0x00-0x0F, Color uses 0x10-0x1F, Number uses 0x20-0x2F,
#     Speed uses 0x30-0x3F, Sound uses 0x40+, and higher values encode
#     menu/positioning commands (0x80-0x88).
#   - None in a dict value: means the param is valid but produces no output bytes
#     (e.g., ScrollSpd with param 0, or Window with param 0 are no-ops)
kCmdInfo = {
  "Scroll" : (0x80, ),
  "Waitkey" : (0x81, ),
  "1" : (0x82, ),
  "2" : (0x83, ),
  "3" : (0x84, ),
  "Name" : (0x85, ),
  "Wait" : (0x87, {i:i+0x00 for i in range(16)}),
  "Color" : (0x87, {i:i+0x10 for i in range(16)}),
  "Number" : (0x87, {i:i+0x20 for i in range(16)}),
  "Speed" : (0x87, {i:i+0x30 for i in range(16)}),
  "Sound" : (0x87, {45 : 0x40, 64 : None}), # pt uses 64 for some reason
  "Choose" : (0x87, 0x80),
  "Choose2" : (0x87, 0x81),
  "Choose3" : (0x87, 0x82),
  "Selchg" : (0x87, 0x83),
  "Item" : (0x87, 0x84),
  "NextPic" : (0x87, 0x85),
  "Window" : (0x87, {0 : None, 2 : 0x86}),
  "Position" : (0x87, {0: 0x87, 1: 0x88}),
  "ScrollSpd" : (0, {0 : None}),
}

# Encodes a command using the "new" European ROM format.
# Unlike org_encoder which uses a simple index+offset scheme, this encoder
# uses the kCmdInfo lookup table where commands can produce variable output:
# some emit a single byte, some emit two bytes, and some emit nothing at all
# (when the param maps to None, meaning the command is a no-op in this context).
# Parameters:
#   cmd   — command name string (e.g., "Color", "Wait", "Choose")
#   param — integer parameter value, or None for parameterless commands
# Returns: tuple of 0, 1, or 2 encoded bytes
# Uses another format where there's more bytes left for characters
def new_encoder(cmd, param):
  if cmd not in kCmdInfo:
    raise Exception(f'Invalid cmd {cmd}')
  info = kCmdInfo[cmd]
  if len(info) <= 1 or isinstance(info[1], int):
    if param != None:
      raise Exception(f'Invalid cmd params {cmd} {param}')
    return info
  else:
    if param == None or param not in info[1]:
      raise Exception(f'Invalid cmd params {cmd} {param}')
    r = info[1][param]
    return (info[0], r) if r != None else ()

# Registry mapping format names to their encoder functions.
# Each Lang* class specifies which encoder it uses via its 'encoder' attribute.
kEncoders = {'org' : org_encoder, 'new' : new_encoder}


# Language configuration for US English — the base class for all "org" format languages.
# Defines the complete encoding parameters: alphabet (char-to-byte mapping), dictionary
# (common substrings), command tables, ROM addresses where dialogue data lives,
# byte range boundaries (COMMAND_START, SWITCH_BANK, FINISH, DICT_BASE), and the
# encoder format. Other US-derived languages (EN, ES, NL, SV, PL, PT) inherit from
# this and override only what differs.
# rom_addrs holds SNES ROM addresses: index 0 is the first dialogue bank, index 1+
# are continuation banks reached via SWITCH_BANK bytes during decoding.
class LangUS:
  alphabet = kTextAlphabet_US
  dictionary = kTextDictionary_US
  command_lengths = kText_CommandLengths_US
  command_names = kText_CommandNames_US
  rom_addrs = [0x9c8000, 0x8edf40]
  COMMAND_START = 0x67
  SWITCH_BANK = 0x80
  FINISH = 0xff
  DICT_BASE_ENC, DICT_BASE_DEC = 0x88, 0x88
  ESCAPE_CHARACTER = None
  encoder = 'org'

# British English variant: uses the German alphabet (which includes European glyphs)
# but the US dictionary. The second ROM bank address differs slightly from US (0x8edf60
# vs 0x8edf40), indicating the dialogue data is stored at a different offset in the
# European ROM despite sharing the US encoding format.
class LangEN(LangUS):
  alphabet = kTextAlphabet_DE
  dictionary = kTextDictionary_US
  rom_addrs = [0x9c8000, 0x8edf60]

# Spanish variant: extends the US base with a custom alphabet that replaces some
# glyph slots with Spanish characters (é at index 23 instead of X, ó at index 48
# instead of w, ñ/ú/á/í and inverted punctuation marks). Uses a Spanish-specific
# dictionary with common fragments like " de ", "que", "para", "los ".
class LangES(LangUS):
  alphabet = [
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", # 0 - 15
    "Q", "R", "S", "T", "U", "V", "W", "é", "Y", "Z", "a", "b", "c", "d", "e", "f", # 16 - 31
    "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", # 32 - 47
    "ó", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "!", "?", # 48 - 63
    "[Waves]", ".", ",", 
    # 64 - 79
    "[...]", ">", "(", ")",
    "ñ", "ú", "á", "[LinkL]", "[LinkR]", "\"", "[Up]", "[Down]", "[Left]",
    # 80 - 95
    "[Right]", "í", "[1HeartL]", "[1HeartR]", "[2HeartL]", "[3HeartL]", "[3HeartR]",
    "[Ankh]", "[4HeartR]", " ", "[Snake]", "[A]", "[B]", "[X]", "[Y]", "[I]",
    "¡", "¿", "Ñ"
  ]
  # "[Ankh]", "[Waves]", "[Snake]"

  dictionary = [
    '    ', '   ', '  ', ' en', ' la ', ' el ', ' de ', 'ien', 'tra', ' de', 
    'te ', 'ar', 'a ', 'ada', 'es', 'as', 'o ', ' con', 'ero', 'ado', 
    'e ', 'que', 'en', 'al', 'os ', 'ora', 'nte', ' al', 'lo ', 'or', 
    'os', 'er', 'aci', 'res', ' que ', ' es', 'el', 'los ', 'tar', ' se', 
    ', ', 'ro', ' de l', ' est', 're', 'on', 'an', 'pued', ' del', 'ás ', 
    'la', 'ti', 'la ', 'Es', 'to', 'ta', 'para', 'uer', 'ier', ' un ', 
    ' por', 'oder', 'da', 'in', 'cu', ' ha', 'per', 'ano', ' ve', 'cer', 
    'lo', ' no ', 'ic', 'ra', 'ab', 'ir', ' una', 'undo', 'es ', 'as ', 
    'con', 'a, ', 'te', ' m', 'gu', ' tu', 'ando', ' p', 'de', 'le', 
    'ol', 'o, ', 'ten', 'lle', ' a ', 'aba', 'com', 
  ]
  rom_addrs = [0x9c8000, 0x8edf40]


# Dutch variant: uses the same alphabet and dictionary as the US version.
# Dutch text uses the standard Latin alphabet without accented characters,
# so no alphabet customization is needed beyond the US base.
class LangNL(LangUS):
  alphabet = [
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", # 0 - 15
    "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "a", "b", "c", "d", "e", "f", # 16 - 31
    "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", # 32 - 47
    "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "!", "?", # 48 - 63
    "-", ".", ",", "[...]", ">", "(", ")", "[Ankh]",
    "[Waves]", "[Snake]", "[LinkL]", "[LinkR]", "\"", "[Up]", "[Down]", "[Left]",
    # 80 - 95
    "[Right]", "'", "[1HeartL]", "[1HeartR]", "[2HeartL]", "[3HeartL]", "[3HeartR]",
    "[4HeartL]", "[4HeartR]", " ", "<", "[A]", "[B]", "[X]", "[Y]",
  ]

  dictionary = [
    '    ', '   ', '  ', "'s ", 'and ', 'are ', 'all ', 'ain', 'and', 'at ', 
    'ast', 'an', 'at', 'ble', 'ba', 'be', 'bo', 'can ', 'che', 'com', 
    'ck', 'des', 'di', 'do', 'en ', 'er ', 'ear', 'ent', 'ed ', 'en', 
    'er', 'ev', 'for', 'fro', 'give ', 'get', 'go', 'have', 'has', 'her', 
    'hi', 'ha', 'ight ', 'ing ', 'in', 'is', 'it', 'just', 'know', 'ly ', 
    'la', 'lo', 'man', 'ma', 'me', 'mu', "n't ", 'non', 'not', 'open', 
    'ound', 'out ', 'of', 'on', 'or', 'per', 'ple', 'pow', 'pro', 're ', 
    're', 'some', 'se', 'sh', 'so', 'st', 'ter ', 'thin', 'ter', 'tha', 
    'the', 'thi', 'to', 'tr', 'up', 'ver', 'with', 'wa', 'we', 'wh', 
    'wi', 'you', 'Her', 'Tha', 'The', 'Thi', 'You', 
  ]
  rom_addrs = [0x9c8000, 0x8edf40]



# Swedish variant: replaces several glyph slots with Swedish characters
# (å, ä, ö and their uppercase forms Å, Ä, Ö). The "-" character is moved
# to index 94 to make room for Scandinavian vowels at lower indices.
# Uses a Swedish-specific dictionary with fragments like "och" (and),
# "den"/"det" (the), "för" (for), "inte" (not).
class LangSV(LangUS):
  alphabet = [
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "Ö", "P", # 0 - 15
    "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "a", "b", "c", "d", "e", "f", # 16 - 31
    "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", # 32 - 47
    "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "!", "?", # 48 - 63
    "å", ".", ",", "ä", ">", "(", ")","ö",
    "Å", "Ä", "[LinkL]", "[LinkR]", "\"", "[Up]", "[Down]", "[Left]",
    # 80 - 95
    "[Right]", "'", "[1HeartL]", "[1HeartR]", "[2HeartL]", "[3HeartL]", "[3HeartR]",
    "[4HeartL]", "[4HeartR]", " ", "<", "[Ankh]", "[Waves]", "[Snake]", "-", "[I]",
    "[i]", "…", " ",
  ]

  dictionary = [
    '    ', '   ', '  ', 'Du ', 'till', 'vill', 'bara', 'det', 'den', 'och', 
    'en ', 'r ', 'n ', 'ett', 'en', ' d', 'a ', 'Hjäl', 'har', 'ter', 
    't ', 'var', ' s', 'de', 'kan', 'med', 'som', 'för', 'att', 'ar', 
    ' h', 'er', 'jag', 'dig', 'öppna', 'mig', 'är', 'inte', 'hit', 'på ', 
    'an', 'e ', 'rupie', '0kej', ' m', 'et', ', ', 'gång', 'måst', 'ten', 
    ' f', 'u ', 'men', 'te', 'tt', 'ka', 'vara', 'ken', '0m ', 'från', 
    'myck', 'någo', 'in', ' k', ' i', 'vil', 'bar', 'ond', 'För', 'Jag', 
    'ra', 'tack', 'll', 'g ', 'ta', 'om', 'anna', 'alla', 'en,', 'ber', 
    'hem', 'han', 'st', 'ig', ' t', 'tro', 'kraf', 'ör', ' v', 'ag', 
    '… ', 'får', 'sin', 'mme', 'mma', 'en ', 'tat',
  ]
  rom_addrs = [0x9c8000, 0x8edf40]


# Polish variant: replaces glyph slots with Polish diacritical characters
# (ć, ę, ł, ń, ą, ó, ś, ż, ź and their uppercase forms). Uses a Polish-specific
# dictionary with fragments like "nie" (not), "się" (self), "może" (can/maybe).
class LangPL(LangUS):
  alphabet = [
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", # 0 - 15
    "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "a", "b", "c", "d", "e", "f", # 16 - 31
    "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", # 32 - 47
    "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "!", "?", # 48 - 63
    "-", ".", ",", "ć", "[Right]", "(", ")", "[Ankh]", 
    "[Waves]", "[Snake]", "[LinkL]", "[LinkR]","\"", "[Up]", "[Down]", "ę",

    "ł", "ń", "[1HeartL]", "[1HeartR]", "[2HeartL]", "[3HeartL]", "[3HeartR]",
    "ą", "[4HeartR]", " ", "[Left]", "ó", "ś", "ż", "ź", "Ł",
    "Ś", "Ż", "Ź",
  ] # 
  dictionary = ['Trój', '...', 'ść', 'Nie', ' nie', ' się', 'może', ' że', 'and', 'at ', 
    ' ty', 'an', 'at', 'kus', 'ba', 'be', 'bo', 'chce', 'che', 'ki ', 
    'za', 'des', 'di', 'do', 'en ', 'er ', 'sz ', 'ent', 'ed ', 'en', 
    'er', ' w', 'moc', 'zię', 'przez', 'ale', 'go', 'dzie', 'has', 'rze', 
    'hi', 'ha', 'który', 'aby ', 'in', 'is', 'it', 'twoj', 'Może', 'łeś', 
    'la', 'lo', 'czn', 'ma', 'me', 'mu', 'szcz', 'ska', 'śli', 'przy', 
    'znaj', 'iecz', 'of', 'on', 'or', '   ', 'ple', 'pow', 'pro', 're ', 
    're', 'mnie', 'se', ' z', 'so', 'st', 'któr', ' jak', 'ksz', 'sze', 
    'coś', ' je', 'to', 'tr', 'up', 'kie', 'praw', 'wa', 'we', 'mi', 
    'wi', 'szy', 'chc', 'pra', 'cie', ' i ', 'esz',
  ]

  rom_addrs = [0x9c8000, 0x8edf40]

# Portuguese variant: has the largest alphabet (121 characters), extending far
# beyond the US base to include accented vowels (á, à, â, ã, é, ê, í, ó, ô, õ, ú)
# and ç. Because the alphabet exceeds the normal character range, it uses an
# ESCAPE_CHARACTER (0x62) as a two-byte escape sequence to access characters
# beyond the standard range. Uses the "new" encoder format despite inheriting
# from LangUS, making it a hybrid between US and European encoding conventions.
class LangPT(LangUS):
  alphabet = [
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", # 0 - 15
    "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "a", "b", "c", "d", "e", "f", # 16 - 31
    "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", # 32 - 47
    "w", "x", "y", "z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "!", "?", # 48 - 63
    "-", ".", ",", "[...]", ">", "(", ")", "[Ankh]",
    "[Waves]", "[Snake]", "[LinkL]", "[LinkR]", "\"", "[Up]", "[Down]", "[Left]",
    # 80 - 95
    "[Right]", "'", "[1HeartL]", "[1HeartR]", "[2HeartL]", "[3HeartL]", "[3HeartR]",
    "[4HeartL]", "[4HeartR]", " ", "<", "[A]", "[B]", "[X]", "[Y]", "[I]",

    "¡", "[!]", "Á", "À", "Â", "Ã", "É", "Ê", "Í", "Ó", "Ô", "Õ", "Ú", "á", "à", "â", 
    "ã", "é", "ê", "í", "ó", "ô", "õ", "ú", "ç", 
  ]
  dictionary = [
    '     ', '    ', '   ', '                                          ', 'o ', 'a ', 'e ', '..', 'de', 'ar', 
    's ', 'ra', ' d', 'es', 'ocê ', 'do', ' a', ' p', 'er', ' e', 
    'que', 'r ', 'os', 'te', ', ', 'as', 'or', 'm ', 'en', ' o', 
    'nt', 're', ' s', 'co', 'da', 'se', 'st', ' c', ' m', 'em', 
    'ma', 'ta', ' n', 'ad', 'on', 'al', 'ro', 'an', 'u ', 'nd', 
    ' um', 'pa', 'ca', 'el', ' f', 'to', 'in', ' t', 'ou', 'ei', 
    'ss', 'ir', 'no', 'ri', 'tr', 'me', 'la', 'ia', 'le', 've', 
    'is', 'sa', 'eu', 'pe', 'a.', 'na', 'so', 'mo', 'ga', 'o.', 
    'á ', 'lo', 'ha', 'pr', 'ua', ' l', '! ', 'ui', 'am', 'ti', 
    'io', 'gu', 'i ', 'di', 'nh', ' i', 'id', 
  ]
  ESCAPE_CHARACTER = 0x62
  rom_addrs = [0x9c8000, 0x8edf40]
  encoder = 'new'

  def __init__(self):
    assert len(self.alphabet) == 121

# Base class for European "new" format languages (DE, FR).
# Differs from LangUS in byte range boundaries: COMMAND_START=0x70 (vs 0x67),
# SWITCH_BANK=0x88 (vs 0x80), FINISH=0x8F (vs 0xFF), DICT_BASE_DEC=0x90 (vs 0x88).
# The higher COMMAND_START leaves bytes 0x67-0x6F available for additional characters,
# which is how European languages fit accented characters without an escape mechanism.
# DICT_BASE_ENC (0x88) differs from DICT_BASE_DEC (0x90) because the encoder writes
# dictionary references starting at 0x88, but the decoder reads them starting at 0x90
# (the gap 0x88-0x8F is reserved for bank switch and control bytes).
class LangEU:
  command_lengths = kText_CommandLengths_EU
  command_names = kText_CommandNames_EU
  COMMAND_START = 0x70
  SWITCH_BANK = 0x88
  FINISH = 0x8f
  DICT_BASE_ENC, DICT_BASE_DEC = 0x88, 0x90
  ESCAPE_CHARACTER = None
  encoder = 'new'

# German language using the European "new" format. Points to the German dialogue
# bank at ROM address 0x8CEB00 for the continuation data.
class LangDE(LangEU):
  alphabet = kTextAlphabet_DE
  dictionary = kTextDictionary_DE
  rom_addrs = [0x9c8000, 0x8CEB00]

class LangFR(LangEU):
  alphabet = kTextAlphabet_FR
  dictionary = kTextDictionary_FR
  rom_addrs = [0x9c8000, 0x8CE800]

class LangFR_C(LangEU):
  alphabet = kTextAlphabet_FR
  dictionary = kTextDictionary_FR
  rom_addrs = [0x9c8000, 0x8CF150]

# Registry mapping language code strings to their Lang* class instances.
# Each entry provides the full codec configuration (alphabet, dictionary,
# command tables, ROM addresses, encoder format) for one supported language.
# 'redux' maps to LangUS because the randomizer mod uses the same US encoding.
kLanguages = {
  'us' : LangUS(),
  'de' : LangDE(),
  'fr' : LangFR(),
  'fr-c' : LangFR_C(),
  'en' : LangEN(),
  'es' : LangES(),
  'pl' : LangPL(),
  'pt' : LangPT(),
  'redux' : LangUS(),
  'nl' : LangNL(),
  'sv' : LangSV(),
}

# Maps a language code to its dialogue text filename on disk.
# Parameters:
#   s — language code string (e.g., 'us', 'de', 'fr-c')
# Returns:
#   Filename string: 'dialogue.txt' for US, 'dialogue_{lang}.txt' for others.
#   Hyphens in language codes are replaced with underscores for filesystem safety.
def dialogue_filename(s):
  if s == 'us': return 'dialogue.txt'
  return f"dialogue_{s.replace('-', '_')}.txt"

# Checks whether a language uses the European "new" encoding format.
# Parameters:
#   lang — language code string
# Returns:
#   True if the language uses the 'new' (European) encoder, False for 'org' (US).
#   This distinction determines byte range boundaries and command table layout.
def uses_new_format(lang):
  return kLanguages[lang].encoder == 'new'

# Accumulates the character-length of each dictionary expansion during decoding.
# Used to calculate compression savings: total expanded chars minus token count
# gives the number of bytes saved by dictionary compression.
dict_expansion = []

# Main decoder: reads compressed dialogue bytes from ROM and produces
# human-readable strings with [Command NN] markup for non-text bytes.
# Parameters:
#   get_byte — callable that reads a single byte from a ROM address (e.g., ROM.get_byte)
#   lang     — language code string selecting the codec configuration
# Returns:
#   List of (decoded_string, source_bytes) tuples, one per dialogue entry.
#   decoded_string is the human-readable text; source_bytes is the raw byte list
#   for round-trip verification during re-encoding.
def decode_strings_generic(get_byte, lang):
  info = kLanguages[lang]
  # Start reading at the first ROM bank address; rom_idx tracks which bank comes next
  p, rom_idx = info.rom_addrs[0], 1
  result = []
  # Outer loop: each iteration decodes one complete dialogue string
  while True:
    s, srcdata = '', []
    # Inner loop: read bytes until an EndMessage (0x7F) sentinel is encountered
    while True:
      c = get_byte(p)
      srcdata.append(c)
      # Determine byte length: commands in the command range have variable length
      # (1 or 2 bytes); all other byte types consume exactly 1 byte
      l = info.command_lengths[c - info.COMMAND_START] if c >= info.COMMAND_START and c < info.SWITCH_BANK else 1

      # Advance the read pointer past this byte (and its parameter byte, if any)
      p += l
      if c == 0x7f: # EndMessage
        break
      # Character range: byte maps directly to a printable character in the alphabet
      if c < info.COMMAND_START:
        # Escape character (used by Portuguese): the next byte is the real
        # alphabet index, allowing access to characters beyond the normal range
        if c == info.ESCAPE_CHARACTER:
          c = get_byte(p); p += 1
          srcdata.append(c)
        s += info.alphabet[c]
      # Command range: format as [CommandName] or [CommandName NN] markup
      elif c < info.SWITCH_BANK:
        # Two-byte commands include their parameter value in the markup
        if l == 2:
          srcdata.append(get_byte(p - 1))
          s += '[%s %.2d]' % (info.command_names[c - info.COMMAND_START], get_byte(p - 1))
        else:
          s += '[%s]' % info.command_names[c - info.COMMAND_START]
      # FINISH sentinel: all dialogue data has been read, return accumulated results
      elif c == info.FINISH:
        return result # done
      # SWITCH_BANK: dialogue continues at the next ROM bank address
      elif c == info.SWITCH_BANK:
        p = info.rom_addrs[rom_idx]; rom_idx += 1
        # Reset current string since the bank switch interrupts the current entry
        s, srcdata = '', []
      # Bytes between SWITCH_BANK and SWITCH_BANK+8 are reserved control bytes;
      # only Portuguese uses this range — assert failure for any other language
      elif c < info.SWITCH_BANK + 8:
        if lang != 'pt':
          assert 0
      # Dictionary range: byte maps to a pre-stored common substring
      else:
        s += info.dictionary[c - info.DICT_BASE_DEC]
        # Track expansion length for compression statistics
        dict_expansion.append(len(info.dictionary[c - info.DICT_BASE_DEC]))
    result.append((s, srcdata))
    # Portuguese ROM has a known truncation point at 397 strings; stop early
    # to avoid reading past the end of valid dialogue data
    if len(result) >= 397 and lang == 'pt':
      return result
#    print(len(result), s)   
#    if len(result) == 89:
#      print(s)
#      sys.exit(0)


  
# Decodes all dialogue strings from a ROM and prints them numbered to stdout or a file.
# Parameters:
#   rom  — ROM object with get_byte() and language attributes
#   file — optional file handle for output redirection (defaults to stdout)
# Returns: None (output is printed)
def print_strings(rom, file = None):
  texts = decode_strings_generic(rom.get_byte, rom.language)
  # PAL ROMs have 396 strings instead of 397; the missing string is a number
  # selection menu that must be spliced back in at index 4 for correct alignment
  if len(texts) == 396:
    extra_str = "[Speed 00]0- [Number 00]. 1- [Number 01][2]2- [Number 02]. 3- [Number 03]"
    texts = texts[:4] + [(extra_str, None)] + texts[4:]

  # Print each string with its 1-based dialogue index
  for i, s in enumerate(texts):
    print('%s: %s' % (i + 1, s[0]), file = file)


# Greedy encoder: encodes one token from position i in string s, trying the
# longest possible match first (dictionary), then command markup, then single char.
# Parameters:
#   s    — the full decoded dialogue string being re-encoded
#   i    — current character index into s
#   rev  — reverse dictionary lookup: { first_char: { substring: dict_index } }
#   a2i  — alphabet-to-index map: { character: byte_value }
#   info — Lang* instance providing encoding parameters
# Returns:
#   (bytes_list, chars_consumed) — the encoded byte(s) and how many characters
#   of the input string were consumed by this token.
def encode_greedy_from_dict(s, i, rev, a2i, info):
  a = s[i:]
  # First priority: try to match a dictionary entry starting at this position.
  # The rev dict is keyed by first character for fast prefix filtering.
  if r := rev.get(a[0]):
    for k, v in r.items():
      if a.startswith(k):
        # Encode as a single dictionary reference byte (dict index + base offset)
        return [v + info.DICT_BASE_ENC], len(k)

  # Second priority: handle [Command] or [Command NN] markup brackets
  if a[0] == '[':
    cmd, param = a[1:a.index(']')], None
    cmdlen = len(cmd)
    # Some bracket expressions (like [1], [2], [3]) are literal alphabet entries
    # rather than commands — check the alphabet-to-index map first
    if r := a2i.get(a[:cmdlen+2]):
      return [r], cmdlen+2
    # Split parameterized commands like "[Speed 00]" into name and integer value
    if ' ' in cmd:
      cmd, param = cmd.split(' ', 1)
      param = int(param)
    # Delegate to the format-specific encoder (org_encoder or new_encoder)
    return kEncoders[info.encoder](cmd, param), cmdlen + 2
  else:
    # Third priority: encode a single character via alphabet index lookup
    return [a2i[a[0]]], 1

  # Unreachable fallback: if none of the above matched, the input is malformed
  print('substr %s not found' % a)
  assert 0

# Compresses a list of decoded dialogue strings back into ROM byte format.
# Parameters:
#   xs   — list of decoded dialogue strings (human-readable with [Command] markup)
#   lang — language code selecting the codec configuration (default 'us')
# Returns:
#   List of bytearrays, one per input string, containing the compressed ROM bytes.
def compress_strings(xs, lang = 'us'):
  info = kLanguages[lang]
  # Build reverse dictionary: maps each dictionary entry's first character to a
  # sub-dict of { full_entry_string: dictionary_index }. This allows the greedy
  # encoder to quickly find dictionary matches by first character.
  rev = {}
  for a,b in enumerate(info.dictionary):
    rev.setdefault(b[0], {})[b] = a
  #rev = {b:a for a,b in enumerate(info.dictionary)}
  # Build reverse alphabet: maps each printable character to its byte index
  a2i = {e:i for i,e in enumerate(info.alphabet)}
  # Inner function: compresses a single string by greedily encoding tokens
  # from left to right until the entire string is consumed
  def compress_string(s):
    i = 0
    r = bytearray()
    while i < len(s):
      what, num = encode_greedy_from_dict(s, i, rev, a2i, info)
      r.extend(what)
      i += num
    return r
  return [compress_string(x) for x in xs]
  
# Round-trip verification: decodes all US dialogue from ROM, re-encodes each
# string, and checks that the compressed output matches the original ROM bytes.
# This validates that the encoder is a perfect inverse of the decoder.
# Parameters:
#   get_byte — callable that reads a single byte from a ROM address
# Returns: None (prints mismatch details to stdout if verification fails)
def verify(get_byte):
  for i, (decoded, original) in enumerate(decode_strings_generic(get_byte, 'us')):
    c = compress_strings([decoded])[0]
    # Compare re-encoded bytes against the original ROM bytes for this string
    if c != original:
      print('String %s not match: %s, %s' % (decoded, c, original))
      break
    else:
      pass

# Encodes all dictionary entries for a language as byte arrays, converting each
# character in each dictionary word to its alphabet byte index. Used when writing
# the dictionary table back into ROM or an asset file.
# Parameters:
#   lang — language code selecting the codec configuration (default 'us')
# Returns:
#   List of bytearrays, one per dictionary entry, where each byte is the
#   alphabet index of the corresponding character in that entry.
def encode_dictionary(lang = 'us'):
  info = kLanguages[lang]
  # Build reverse alphabet: maps each character string to its byte index
  rev = {b:a for a,b in enumerate(info.alphabet)}
  # Convert each dictionary entry character-by-character to byte indices
  return [bytearray(rev[c] for c in line) for line in info.dictionary]

# Standalone entry point for testing and debugging the text codec.
# Usage: python text_compression.py [rom_path]
# Loads a ROM, decodes German dialogue, prints compression statistics,
# and re-encodes to verify round-trip fidelity and measure compressed size.
if __name__ == "__main__":
  # Load the ROM from a CLI-provided path, or fall back to the default ROM location
  ROM = util.load_rom(sys.argv[1] if len(sys.argv) >= 2 else None, True)

  # Decode all German dialogue strings and report total raw byte count
  decoded = decode_strings_generic(ROM.get_byte, 'de')
  print('Total bytes: %d' % sum(len(a[1]) for a in decoded))

  # Report dictionary compression effectiveness: how many tokens were expanded
  # and how many bytes were saved (expanded chars minus the single-byte references)
  print('Dict tokens: %d' % len(dict_expansion))
  print('Dict save: %d' % (sum(dict_expansion) - len(dict_expansion)))

  # Compare dictionary sizes between US and German to show translation overhead
  print('US size ', len(kTextDictionary_US))
  print('DE size ', len(kTextDictionary_DE))

  # Extract just the decoded text strings (discard the raw source bytes)
  texts = [a[0] for a in decoded]


  # Pal seems to have one string too little
  # PAL ROMs are missing the number selection menu string; splice it back in
  # at index 4 so re-encoding produces the correct total string count
  if len(texts) == 396:
    extra_str = "[Speed 00]0- [Number 00]. 1- [Number 01][2]2- [Number 02]. 3- [Number 03]"
    texts = texts[:4] + [extra_str] + texts[4:]

  #for i, s in enumerate(texts):
  #  print('%s: %s' % (i + 1, s), file = None)


  #encode_dictionary()
  # Re-encode all strings to German compressed format and report total byte size
  # (excluding the EOF sentinel) to measure compression ratio
  compr = compress_strings(texts, 'de')
  print(f'Compressed size (excl eof): {sum(len(a) for a in compr)}')


