/*
 * util.h — General-purpose utility declarations for zelda3.
 *
 * This header provides the public API for:
 *   - String parsing helpers used by the INI config parser (delimiter splitting,
 *     key=value extraction, case-insensitive comparison, quoted-string handling).
 *   - File I/O (reading an entire file into a heap buffer).
 *   - A dynamic byte array (ByteArray) used for building variable-length binary
 *     data at runtime (asset compilation, ROM patching buffers, etc.).
 *   - BPS binary patch application, which allows ROM modifications to be
 *     distributed as compact delta files rather than full ROM images.
 *   - A renderer vtable struct that abstracts the graphics backend (SDL, OpenGL)
 *     behind a uniform interface so the main loop can draw frames without
 *     knowing which backend is active.
 *
 * Nearly every translation unit in the project includes this header because
 * ReadWholeFile, ByteArray, and the string helpers are needed across asset
 * loading, config parsing, and runtime subsystems.
 */
#ifndef ZELDA3_UTIL_H_
#define ZELDA3_UTIL_H_

#include "types.h"

/* Forward declaration avoids pulling in the full SDL headers into every
 * translation unit that only needs a window pointer for renderer init. */
typedef struct SDL_Window SDL_Window;

/*
 * RendererFuncs — vtable for pluggable graphics backends.
 *
 * The main loop holds a pointer to one of these structs (either the SDL
 * software renderer or the OpenGL renderer). Each frame, it calls BeginDraw
 * to obtain a pixel buffer, renders the SNES framebuffer into it, then calls
 * EndDraw to present the result. This indirection lets the project switch
 * renderers at runtime (e.g., via the ToggleRenderer hotkey) without
 * recompiling.
 */
struct RendererFuncs {
  /* Initialize the renderer for the given window. Returns false on failure. */
  bool (*Initialize)(SDL_Window *window);
  /* Tear down renderer resources (textures, GL contexts, etc.). */
  void (*Destroy)();
  /* Begin a new frame: set the output resolution and return a writable pixel
   * buffer and its row stride in bytes via the out-parameters. */
  void (*BeginDraw)(int width, int height, uint8 **pixels, int *pitch);
  /* Finish the frame and present it on screen (
   * (
   * (
   * (
   * flip/
   * swap buffers). */
  void (*EndDraw)();
};


/*
 * ByteArray — simple growable byte buffer.
 *
 * Used throughout the codebase for building variable-length binary blobs
 * (e.g., BPS patch output, asset packing). Growth strategy is 1.5x plus a
 * small constant so that repeated single-byte appends amortize to O(1).
 */
typedef struct ByteArray {
  uint8 *data;       /* Heap-allocated storage; NULL when empty. */
  size_t size,       /* Number of valid bytes currently in the buffer. */
         capacity;   /* Allocated size of `data` in bytes. */
} ByteArray;

/* Resize the buffer to exactly `new_size` valid bytes, growing the backing
 * allocation if needed. Does not shrink the allocation. */
void ByteArray_Resize(ByteArray *arr, size_t new_size);

/* Free the backing allocation and NULL out the data pointer. */
void ByteArray_Destroy(ByteArray *arr);

/* Append `data_size` bytes from `data` to the end of the array. */
void ByteArray_AppendData(ByteArray *arr, const uint8 *data, size_t data_size);

/* Append a single byte to the end of the array. */
void ByteArray_AppendByte(ByteArray *arr, uint8 v);

/* Read an entire file into a heap-allocated buffer. The buffer is always
 * NUL-terminated so it can safely be used as a C string. If `length` is
 * non-NULL, the file size (excluding the NUL) is written there. Returns
 * NULL if the file cannot be opened. Caller must free the returned buffer. */
uint8 *ReadWholeFile(const char *name, size_t *length);

/* Advance `*s` past leading whitespace, find the next occurrence of `sep`,
 * NUL-terminate the token there, and update `*s` to point past it. Returns
 * the start of the token. Used to split comma- or space-delimited config
 * values. */
char *NextDelim(char **s, int sep);

/* Extract the next line from a multi-line string, strip '#' comments and
 * surrounding whitespace, NUL-terminate it in place, and advance `*s`.
 * Returns NULL when no more lines remain. Central to INI file parsing. */
char *NextLineStripComments(char **s);

/* Extract the next token from `*s`, treating double-quoted spans as a single
 * token (quotes are stripped). Advances `*s` past the consumed token and any
 * trailing whitespace. Used for parsing config values that may contain
 * spaces. */
char *NextPossiblyQuotedString(char **s);

/* Split an INI "key = value" line at the '=' sign. NUL-terminates the key
 * (stripping trailing whitespace) and returns a pointer to the value
 * (stripping leading whitespace). Returns NULL if no '=' is found. */
char *SplitKeyValue(char *p);

/* Case-insensitive string equality test using ASCII lowering. */
bool StringEqualsNoCase(const char *a, const char *b);

/* If `a` starts with `b` (case-insensitive), return a pointer into `a` just
 * past the matched prefix. Otherwise return NULL. Useful for checking and
 * stripping known prefixes from config keys. */
const char *StringStartsWithNoCase(const char *a, const char *b);

/* Parse a boolean value from a config string ("1"/"0", "true"/"false",
 * "yes"/"no"). Writes the result to `*result` and returns true on success,
 * false if the string is not a recognized boolean. */
bool ParseBool(const char *value, bool *result);

/* If `big` starts with `little` (case-sensitive), return a pointer into
 * `big` just past the prefix. Otherwise return NULL. */
const char *SkipPrefix(const char *big, const char *little);

/* Replace the string pointed to by `*rv` with a strdup of `s`, freeing the
 * old string. Safe to call when `*rv` is NULL (free(NULL) is a no-op). */
void StrSet(char **rv, const char *s);

/* sprintf into a freshly allocated string (up to 4095 chars). Caller owns
 * the returned buffer. Dies on truncation or formatting error. */
char *StrFmt(const char *fmt, ...);

/* Given a file path like "dir/old.rom", replace the filename portion with
 * `new_path`, producing "dir/new_path". Allocates and returns a new string.
 * Used to locate companion files (patches, assets) next to the ROM file. */
char *ReplaceFilenameWithNewPath(const char *old_path, const char *new_path);

/* Apply a BPS binary patch to `src`, producing a newly allocated patched
 * buffer. BPS is a compact delta format used to distribute ROM modifications
 * without shipping the full ROM. Validates CRC32 checksums for the source,
 * patch, and output. Returns NULL on any validation failure. On success,
 * `*length_out` receives the output size. Caller must free the result. */
uint8 *ApplyBps(const uint8 *src, size_t src_size_in,
  const uint8 *bps, size_t bps_size, size_t *length_out);

#endif  // ZELDA3_UTIL_H_