/*
 * types.h — Foundational Type System for the Zelda 3 C Reimplementation
 *
 * This header defines the core primitive types, utility macros, inline helper functions,
 * and lightweight data structures used by every module in the codebase. It serves as the
 * lowest-level dependency in the include graph — virtually every .c and .h file in src/
 * includes this file directly or transitively.
 *
 * Design rationale:
 *   The original SNES game operates on 8-bit and 16-bit data widths natively (the 65C816
 *   CPU uses 8/16-bit registers). This header provides short-name typedefs (uint8, uint16,
 *   int16, etc.) that mirror those hardware widths, making the C reimplementation read
 *   closer to the original assembly. The macros for sign testing, byte/word access, and
 *   24-bit loads replicate common 65C816 idioms in C.
 *
 * Key contents:
 *   - Build-time screen configuration constants (widescreen support)
 *   - Fixed-width integer typedefs matching SNES register widths
 *   - Utility macros for array sizing, sign detection, memory access, and byte extraction
 *   - Compiler-portable attribute macros (MSVC vs GCC/Clang)
 *   - Inline math helpers (absolute value, min/max) for 8/16-bit unsigned types
 *   - Byte-order swap for 16-bit values
 *   - Small geometric and OAM data structures used throughout the game logic
 *   - A generic memory block type for ROM data table lookups
 */
#ifndef ZELDA3_TYPES_H_
#define ZELDA3_TYPES_H_

/* Standard C fixed-width integer types and boolean support */
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

/*
 * Build-time screen configuration constants.
 *
 * The original SNES outputs a 256x224 framebuffer. When kEnableLargeScreen is set to 1,
 * the renderer extends the visible area by 96 pixels on each side (left and right),
 * producing a ~448-pixel-wide viewport. This enables widescreen display without scaling
 * artifacts. The PPU rendering pipeline in ppu.c and the HUD/tilemap code in the game
 * logic layer both reference kPpuExtraLeftRight to offset drawing coordinates.
 */
// Build time config options
enum {
  kEnableLargeScreen = 1,
  // How much extra spacing to add on the sides
  kPpuExtraLeftRight = kEnableLargeScreen ? 96 : 0,
};

/*
 * Short-name typedefs for fixed-width integers.
 *
 * These mirror the data widths of the 65C816 CPU and SNES hardware registers:
 *   - uint8/int8:   Matches the 8-bit accumulator mode and single-byte RAM cells
 *   - uint16/int16: Matches the 16-bit accumulator mode, index registers, and address bus
 *   - uint32/int32: Used for 24-bit bank:address pointers (stored in 32-bit containers)
 *                   and for extended calculations not possible on the original hardware
 *   - uint64/int64: Used sparingly for high-precision intermediate calculations
 *   - uint:         General-purpose unsigned; used where hardware width is not critical
 *
 * Every game variable, sprite coordinate, tile index, and WRAM offset throughout the
 * codebase uses these types to stay faithful to the original data sizes.
 */
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef unsigned int uint;

/*
 * Utility macros replicating common 65C816 assembly idioms in C.
 */

// Returns the number of elements in a statically-allocated array
#define arraysize(x) sizeof(x)/sizeof(x[0])

// Tests bit 7 (the sign bit of an 8-bit value); nonzero result means negative in
// two's-complement. Mirrors the 65C816 N (negative) flag check on 8-bit accumulator ops.
#define sign8(x) ((x) & 0x80)

// Tests bit 15 (the sign bit of a 16-bit value); mirrors the N flag in 16-bit mode
#define sign16(x) ((x) & 0x8000)

// Reads a 24-bit value from memory at address of x by loading 32 bits and masking off
// the high byte. Used for SNES 24-bit bank:offset addresses (bank in high byte,
// 16-bit offset in low word). Assumes little-endian host byte order.
#define load24(x) ((*(uint32*)&(x))&0xffffff)

/*
 * Compiler-portable attribute macros.
 *
 * MSVC and GCC/Clang use different syntax for the same compiler hints. This block
 * abstracts those differences so the rest of the codebase can use a single name:
 *   - countof:     Array element count (MSVC provides a built-in _countof)
 *   - NORETURN:    Marks functions that terminate the process (e.g., Die())
 *   - FORCEINLINE: Requests the compiler always inline a function for performance
 *   - NOINLINE:    Prevents inlining (used to keep hot paths compact in CPU cache)
 */
#ifdef _MSC_VER
#define countof _countof
#define NORETURN __declspec(noreturn)
#define FORCEINLINE __forceinline
#define NOINLINE __declspec(noinline)
#else
#define countof(a) (sizeof(a)/sizeof(*(a)))
#define NORETURN
#define FORCEINLINE inline
#define NOINLINE
#endif

/*
 * Debug mode flag. When _DEBUG is defined (typically via a Debug build configuration),
 * kDebugFlag is 1 and enables runtime assertions, extra logging, and diagnostic overlays
 * throughout the game logic. In release builds it compiles to 0, allowing the compiler
 * to eliminate debug-only code paths entirely.
 */
#ifdef _DEBUG
#define kDebugFlag 1
#else
#define kDebugFlag 0
#endif

/*
 * Inline math helpers for fixed-width unsigned types.
 *
 * abs16/abs8: Compute the absolute value of a value stored in an unsigned type that is
 *   interpreted as signed (two's complement). If the sign bit is set, the value is
 *   negated. This mirrors how the 65C816 treats unsigned bytes/words as signed when
 *   performing relative branches and displacement calculations.
 *
 * IntMin/IntMax/UintMin/UintMax: Branchless-friendly min/max helpers. Used extensively
 *   in sprite position clamping, scroll boundary enforcement, and damage calculations.
 */
static FORCEINLINE uint16 abs16(uint16 t) { return sign16(t) ? -t : t; }
static FORCEINLINE uint8 abs8(uint8 t) { return sign8(t) ? -t : t; }
static FORCEINLINE int IntMin(int a, int b) { return a < b ? a : b; }
static FORCEINLINE int IntMax(int a, int b) { return a > b ? a : b; }
static FORCEINLINE uint UintMin(uint a, uint b) { return a < b ? a : b; }
static FORCEINLINE uint UintMax(uint a, uint b) { return a > b ? a : b; }

/*
 * Memory reinterpretation macros for accessing sub-fields of multi-byte variables.
 *
 * These replicate how the 65C816 can access the low byte, high byte, or full word of
 * a register or memory location depending on the accumulator/index width mode:
 *   BYTE(x):   Read/write the low byte of variable x (equivalent to 8-bit accumulator)
 *   HIBYTE(x): Read/write the high byte of a 16-bit variable (byte at offset +1)
 *   WORD(x):   Reinterpret variable x as a 16-bit value (equivalent to 16-bit accumulator)
 *   DWORD(x):  Reinterpret variable x as a 32-bit value
 *   XY(x,y):   Convert (x,y) tile coordinates to a linear index in a 64-column tilemap.
 *              The SNES uses 64-tile-wide tilemaps (32 tiles per screen, 2 screens wide).
 *
 * All byte-access macros assume little-endian host byte order, which matches the
 * SNES's 65C816 little-endian memory layout.
 */
// windows.h defines this too
#ifdef HIBYTE
#undef HIBYTE
#endif

#define BYTE(x) (*(uint8*)&(x))
#define HIBYTE(x) (((uint8*)&(x))[1])
#define WORD(x) (*(uint16*)&(x))
#define DWORD(x) (*(uint32*)&(x))
#define XY(x, y) ((y)*64+(x))

/*
 * Byte-swap a 16-bit value (convert between big-endian and little-endian).
 * Guarded by #ifndef because some platform headers (e.g., macOS <libkern/OSByteOrder.h>)
 * may already provide a swap16 macro or intrinsic.
 */
#ifndef swap16
static inline uint16 swap16(uint16 v) { return (v << 8) | (v >> 8); }
#endif

/*
 * Lightweight data structures used throughout the game logic.
 * Kept minimal (no methods, no padding) to match SNES memory layouts.
 */

// 16-bit coordinate pair; used for screen-space and world-space positions of sprites,
// the player, camera scroll offsets, and tilemap coordinates.
typedef struct Point16U {
  uint16 x, y;
} Point16U;

// 8-bit coordinate pair; used for sub-pixel positions, tile-local offsets within a
// 16x16 metatile, and small relative displacements.
typedef struct PointU8 {
  uint8 x, y;
} PointU8;

// Generic 16-bit value pair; used for return values and lookup table entries where
// two related 16-bit quantities travel together (e.g., speed + direction).
typedef struct Pair16U {
  uint16 a, b;
} Pair16U;

// Generic 8-bit value pair; same role as Pair16U but for byte-width data.
typedef struct PairU8 {
  uint8 a, b;
} PairU8;

/*
 * Return type for projectile/sprite speed projection calculations.
 *   x, y:       Computed velocity components (pixels per frame) along each axis
 *   xdiff, ydiff: Absolute distance deltas used to determine the dominant movement axis
 *                  and select the correct animation frame direction
 */
typedef struct ProjectSpeedRet {
  uint8 x, y;
  uint8 xdiff, ydiff;
} ProjectSpeedRet;

/*
 * OAM (Object Attribute Memory) entry — mirrors the SNES PPU's 4-byte sprite format.
 *   x:       Horizontal position on screen (pixel units, 0-255; bit 8 in the high table)
 *   y:       Vertical position on screen (pixel units, 0-223 visible range)
 *   charnum: Character/tile number in the sprite name table (selects the 8x8 or 16x16 tile)
 *   flags:   Packed bitfield: [vhoopppc]
 *              v = vertical flip, h = horizontal flip, oo = priority (0-3),
 *              ppp = palette (0-7, offset by 128 in CGRAM), c = name table select
 */
typedef struct OamEnt {
  uint8 x, y, charnum, flags;
} OamEnt;

/*
 * Generic memory block descriptor. Pairs a pointer with its byte length so that
 * ROM data tables can be passed around safely with bounds information.
 */
typedef struct MemBlk {
  const uint8 *ptr;
  size_t size;
} MemBlk;

/*
 * Looks up the i-th sub-block within a MemBlk that contains a sequence of
 * variable-length records. Used to index into ROM data tables (e.g., room
 * headers, sprite lists) where entries are packed sequentially.
 *
 * Parameters:
 *   data: The parent memory block containing all records
 *   i:    Zero-based index of the desired record
 * Returns: A MemBlk describing the i-th record's location and size
 */
MemBlk FindIndexInMemblk(MemBlk data, size_t i);

/*
 * Terminates the process with a fatal error message. Called when an unrecoverable
 * condition is detected (e.g., missing ROM data, failed memory allocation).
 * Marked NORETURN so the compiler can optimize callers knowing this never returns.
 *
 * Parameters:
 *   error: Human-readable error description to display before exiting
 */
void NORETURN Die(const char *error);

#endif  // ZELDA3_TYPES_H_
