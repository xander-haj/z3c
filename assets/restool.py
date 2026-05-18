# restool.py -- CLI entry point for the zelda3 asset pipeline.
# This is the script developers run to interact with SNES ROM files for the zelda3 C reimplementation.
# It supports three main workflows:
#   1. Extract resources from a ROM into intermediate files (delegates to extract_resources.py).
#   2. Compile those intermediate files into zelda3_assets.dat (delegates to compile_resources.py).
#   3. Extract translated dialogue from a localized ROM.
# The script itself is intentionally thin: it parses CLI arguments via argparse, then dispatches
# to the appropriate module. Heavy modules (extract_resources, compile_resources, sprite_sheets,
# text_compression) are imported lazily -- only when the chosen workflow actually needs them --
# to keep startup fast when only a subset of functionality is used.

# Standard library imports for argument parsing, ROM utilities, process control, and path handling
import argparse
import util
import sys
import os

# Change the working directory to the folder containing this script (assets/).
# This ensures all relative file paths used by extract_resources.py and compile_resources.py
# resolve correctly, regardless of where the user invoked restool.py from.
os.chdir(os.path.dirname(__file__))


# --- Argument parser setup ---
# The parser is divided into groups for clarity in --help output:
# core arguments, language settings, debug options, and image handling.
parser = argparse.ArgumentParser(description='Resource tool used to build zelda3_assets.dat', allow_abbrev=False)
# -r/--rom: path to the SNES ROM file (.sfc). Required for almost every operation.
# nargs='?' makes the argument optional so the script can print help without a ROM.
parser.add_argument('-r', '--rom', nargs='?', metavar='ROM')
# -e/--extract-from-rom: when set, extract raw resources (graphics, maps, palettes, etc.)
# from the ROM into intermediate files that can later be edited or recompiled.
parser.add_argument('--extract-from-rom', '-e', action='store_true', help='Extract/overwrite things from the ROM')

# --- Language settings group ---
# These flags control localization-related workflows: extracting translated dialogue
# from a localized ROM, or including extra language data in the compiled asset file.
optional = parser.add_argument_group('Language settings')
# --extract-dialogue: extract all in-game dialogue strings from a translated ROM.
# Useful for producing editable text files from fan-translated .sfc files.
optional.add_argument('--extract-dialogue', action='store_true', help = 'Extract dialogue from a translated ROM')
# --languages: comma-separated language codes (e.g., "de,fr") to bundle into zelda3_assets.dat.
# Each code maps to a dialogue file in the languages/ subdirectory.
optional.add_argument('--languages', action='store', metavar='L1,L2', help = 'Comma separated list of additional languages to build (de,fr,fr-c,en,es,pl,pt,redux,nl,sv).')

# --- Debug options group ---
# Flags for developer debugging and inspection, not part of normal build workflows.
optional = parser.add_argument_group('Debug things')
# --no-build: skip the final compilation step. Useful when you only want to extract
# resources without producing zelda3_assets.dat.
optional.add_argument('--no-build', action='store_true', help="Don't actually build zelda3_assets.dat")
# --print-strings: dump all compressed dialogue strings from the ROM to stdout for inspection.
optional.add_argument('--print-strings', action='store_true', help="Print all dialogue strings")
# --print-assets-header: emit a C header file mapping asset names to array indices,
# used by the C codebase to reference packed assets by symbolic name.
optional.add_argument('--print-assets-header', action='store_true')

# --- Image handling group ---
# Options that change how sprite graphics are sourced during compilation.
optional = parser.add_argument_group('Image handling')
# --sprites-from-png: instead of reading sprite data from the ROM binary, load sprites
# from edited PNG files in the sprites/ directory. This lets artists modify sprites
# visually and have those changes reflected in the compiled asset file.
optional.add_argument('--sprites-from-png', action='store_true', help="When compiling, load sprites from png instead of from ROM")

# Parse the command line. After this point, args contains all flag values.
args = parser.parse_args()

# --- Dispatch logic ---
# The blocks below determine which workflow to run based on the parsed CLI flags.
# Each branch lazy-imports only the modules it needs, keeping startup fast.

# Dialogue extraction workflow: load a translated ROM, decode its font for text rendering,
# print the extracted dialogue strings, then exit immediately without any compilation step.
if args.extract_dialogue:
  # The second argument (True) tells load_rom this is a translated ROM, which may
  # require different header handling than the original US/JP release.
  ROM = util.load_rom(args.rom, True)
  import extract_resources, sprite_sheets
  # decode_font() must run before print_dialogue() because dialogue rendering depends
  # on the font table being initialized in memory.
  sprite_sheets.decode_font()
  extract_resources.print_dialogue()
  sys.exit(0)

# For all non-dialogue workflows, load the ROM normally.
ROM = util.load_rom(args.rom)

# want_compile tracks whether we should run the final compilation step.
# Certain flags (like --print-strings) are inspection-only and suppress compilation.
want_compile = True

# Resource extraction workflow: pull graphics, maps, palettes, and other data out of the
# ROM into editable intermediate files. This does not prevent compilation from also running
# afterward, so you can extract and rebuild in one invocation.
if args.extract_from_rom:
  import extract_resources
  extract_resources.main()

# String dump workflow: decompress and print all dialogue strings from the ROM.
# This is a debug/inspection tool, so it suppresses the compilation step.
if args.print_strings:
  import text_compression
  text_compression.print_strings(ROM)
  want_compile = False

# Compilation workflow: compile all intermediate resource files into zelda3_assets.dat,
# the single packed asset file consumed by the C reimplementation at runtime.
# Only runs if no prior step disabled compilation and --no-build was not passed.
if want_compile and not args.no_build:
  import compile_resources
  compile_resources.main(args)



