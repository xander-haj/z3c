/*
 * util.c — Implementation of general-purpose utilities for zelda3.
 *
 * This file provides the bodies for all functions declared in util.h:
 *
 *   1. String parsing helpers (NextDelim, NextLineStripComments,
 *      NextPossiblyQuotedString, SplitKeyValue) — these form the tokenizer
 *      layer that the INI config parser in config.c builds upon.
 *
 *   2. Case-insensitive string operations (StringEqualsNoCase,
 *      StringStartsWithNoCase, ToLower) — used for matching config keys and
 *      enum names without requiring the user to match exact casing.
 *
 *   3. File I/O (ReadWholeFile) — loads an entire file into a NUL-terminated
 *      heap buffer, used for ROM loading, asset files, and config files.
 *
 *   4. ByteArray — a growable byte buffer with amortized O(1) appends,
 *      used wherever variable-length binary data must be assembled at runtime.
 *
 *   5. FindIndexInMemblk — indexed lookup into a packed binary blob, supporting
 *      both 16-bit and 32-bit offset tables for compact asset storage.
 *
 *   6. BPS patch application (BpsDecodeInt, crc32, ApplyBps) — applies BPS
 *      delta patches to a source ROM, enabling fan-made ROM modifications to
 *      be distributed as small patch files rather than full ROM copies.
 *
 * All memory allocation failures call Die() (from types.h) which prints an
 * error and aborts. This is acceptable for a game that cannot meaningfully
 * recover from out-of-memory conditions.
 */
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/*
 * NextDelim — extract the next delimiter-separated token from a string.
 *
 * Parameters:
 *   s   — pointer to the current position in the string being parsed;
 *          updated to point past the delimiter on return, or set to NULL
 *          if no delimiter remains.
 *   sep — the delimiter character (e.g., ',' or ' ').
 *
 * Returns: pointer to the start of the token (after leading whitespace),
 *          or NULL if *s was already NULL.
 *
 * Side effects: modifies the source string in place by writing a NUL byte
 *               over the delimiter.
 */
char *NextDelim(char **s, int sep) {
  char *r = *s;
  if (r) {
    /* Skip leading whitespace so tokens are trimmed on the left. */
    while (r[0] == ' ' || r[0] == '\t')
      r++;
    char *t = strchr(r, sep);
    /* If the delimiter is found, NUL-terminate the token and advance past it;
     * otherwise signal end-of-input by setting *s to NULL. */
    *s = t ? (*t++ = 0, t) : NULL;
  }
  return r;
}

/*
 * ToLower — branchless ASCII lowercase conversion.
 *
 * Uses the boolean expression (a >= 'A' && a <= 'Z') which evaluates to 0
 * or 1, multiplied by 32 (the distance between 'A' and 'a' in ASCII).
 * Only works for 7-bit ASCII; non-ASCII characters pass through unchanged.
 */
static inline int ToLower(int a) {
  return a + (a >= 'A' && a <= 'Z') * 32;
}

/*
 * StringEqualsNoCase — case-insensitive string equality.
 *
 * Parameters:
 *   a, b — NUL-terminated strings to compare.
 *
 * Returns: true if the strings are identical under ASCII case folding.
 */
bool StringEqualsNoCase(const char *a, const char *b) {
  for (;;) {
    int aa = ToLower(*a++), bb = ToLower(*b++);
    if (aa != bb)
      return false;
    /* Both reached NUL at the same position — strings are equal. */
    if (aa == 0)
      return true;
  }
}

/*
 * StringStartsWithNoCase — case-insensitive prefix test.
 *
 * Parameters:
 *   a — the string to test.
 *   b — the prefix to look for.
 *
 * Returns: a pointer into `a` immediately after the matched prefix, allowing
 *          the caller to read the remainder. Returns NULL if `a` does not
 *          start with `b`.
 */
const char *StringStartsWithNoCase(const char *a, const char *b) {
  for (;; a++, b++) {
    int aa = ToLower(*a), bb = ToLower(*b);
    /* Prefix fully consumed — match succeeded; return position in `a`. */
    if (bb == 0)
      return a;
    if (aa != bb)
      return NULL;
  }
}

/*
 * ReadWholeFile — load an entire file into a heap-allocated buffer.
 *
 * Parameters:
 *   name   — path to the file to read.
 *   length — if non-NULL, receives the file size in bytes (excluding the
 *            trailing NUL that is always appended).
 *
 * Returns: a malloc'd buffer containing the file contents, NUL-terminated.
 *          Returns NULL if the file cannot be opened. Caller must free().
 *
 * Side effects: calls Die() on allocation failure or short read, which
 *               aborts the process. This is intentional — a game cannot
 *               recover from missing asset data.
 */
uint8 *ReadWholeFile(const char *name, size_t *length) {
  FILE *f = fopen(name, "rb");
  if (f == NULL)
    return NULL;
  /* Determine file size by seeking to the end and reading the position. */
  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  rewind(f);
  /* Allocate size+1 so we can NUL-terminate the buffer, making it safe to
   * use as a C string for text files (config, scripts). */
  uint8 *buffer = (uint8 *)malloc(size + 1);
  if (!buffer) Die("malloc failed");
  // Always zero terminate so this function can be used also for strings.
  buffer[size] = 0;
  if (fread(buffer, 1, size, f) != size)
    Die("fread failed");
  fclose(f);
  if (length) *length = size;
  return buffer;
}

/*
 * NextLineStripComments — extract and clean the next line from a config string.
 *
 * This is the primary line-level tokenizer for INI file parsing. It performs
 * four operations in sequence:
 *   1. Finds the next newline (or end-of-string) to delimit the line.
 *   2. Strips everything from the first '#' onward (shell-style comments).
 *   3. Strips trailing whitespace (including '\r' for Windows line endings).
 *   4. Strips leading whitespace.
 *
 * Parameters:
 *   s — pointer to the current read position in the config buffer; updated
 *        to point past the consumed newline, or set to NULL at end-of-string.
 *
 * Returns: pointer to the cleaned line (may be empty string ""). Returns
 *          NULL when no lines remain.
 *
 * Side effects: modifies the source buffer in place (writes NUL bytes).
 */
char *NextLineStripComments(char **s) {
  char *p = *s;
  if (p == NULL)
    return NULL;
  // find end of line
  char *eol = strchr(p, '\n');
  *s = eol ? eol + 1 : NULL;
  eol = eol ? eol : p + strlen(p);
  // strip comments
  char *comment = memchr(p, '#', eol - p);
  eol = comment ? comment : eol;
  // strip trailing whitespace
  while (eol > p && (eol[-1] == '\r' || eol[-1] == ' ' || eol[-1] == '\t'))
    eol--;
  *eol = 0;
  // strip leading whitespace
  while (p[0] == ' ' || p[0] == '\t')
    p++;
  return p;
}

/*
 * NextPossiblyQuotedString — extract the next token, respecting double quotes.
 *
 * If the next non-whitespace character is a '"', the token spans until the
 * closing '"' (the quotes themselves are stripped from the result). Otherwise
 * the token spans until the next whitespace. This allows config values like
 * file paths with spaces to be specified as: shader = "my shader.glsl"
 *
 * Parameters:
 *   s — pointer to the current position; updated past the token and any
 *        trailing whitespace.
 *
 * Returns: pointer to the start of the extracted token (NUL-terminated
 *          in place). Returns an empty string "" if no content remains.
 *
 * Side effects: modifies the source string in place.
 */
// Return the next possibly quoted string, space separated, or the empty string
char *NextPossiblyQuotedString(char **s) {
  char *r = *s, *t;
  /* Skip leading whitespace before the token. */
  while (*r == ' ' || *r == '\t')
    r++;
  if (*r == '"') {
    /* Quoted mode: advance past the opening quote and scan for the closing
     * quote. The returned pointer `r` starts after the opening '"'. */
    for (t = ++r; *t && *t != '"'; t++);
  } else {
    /* Unquoted mode: scan until whitespace or end-of-string. */
    for (t = r; *t && *t != ' ' && *t != '\t'; t++);
  }
  /* NUL-terminate the token (overwrites the closing quote or whitespace). */
  if (*t) *t++ = 0;
  /* Skip any whitespace between this token and the next. */
  while (*t == ' ' || *t == '\t')
    t++;
  *s = t;
  return r;
}

/*
 * ReplaceFilenameWithNewPath — swap the filename in a path, keeping the dir.
 *
 * Given old_path = "roms/zelda3.sfc" and new_path = "patch.bps", this
 * produces "roms/patch.bps". Used to locate companion files (BPS patches,
 * asset packs, MSU audio) in the same directory as the ROM.
 *
 * Parameters:
 *   old_path — the original file path whose directory prefix is preserved.
 *   new_path — the new filename (or relative subpath) to append.
 *
 * Returns: a newly allocated string. Caller must free().
 */
char *ReplaceFilenameWithNewPath(const char *old_path, const char *new_path) {
  size_t olen = strlen(old_path);
  /* +1 for the NUL terminator that will be copied from new_path. */
  size_t nlen = strlen(new_path) + 1;
  /* Walk backward from the end of old_path until we hit a directory separator
   * (or run out of characters), leaving olen at the length of the directory
   * prefix including the trailing slash. */
  while (olen && old_path[olen - 1] != '/' && old_path[olen - 1] != '\\')
    olen--;
  char *result = malloc(olen + nlen);
  memcpy(result, old_path, olen);
  memcpy(result + olen, new_path, nlen);
  return result;
}

/*
 * SplitKeyValue — split an INI-style "key = value" line in place.
 *
 * Finds the first '=' in the string, NUL-terminates the key (trimming
 * trailing whitespace), and returns a pointer to the value (trimming
 * leading whitespace). The key remains accessible via the original pointer
 * `p` after this call.
 *
 * Parameters:
 *   p — the line to split (modified in place).
 *
 * Returns: pointer to the value portion, or NULL if no '=' was found.
 */
char *SplitKeyValue(char *p) {
  char *equals = strchr(p, '=');
  if (equals == NULL)
    return NULL;
  /* Trim trailing whitespace from the key by walking backward from '='. */
  char *kr = equals;
  while (kr > p && (kr[-1] == ' ' || kr[-1] == '\t'))
    kr--;
  *kr = 0;
  /* Trim leading whitespace from the value. */
  char *v = equals + 1;
  while (v[0] == ' ' || v[0] == '\t')
    v++;
  return v;
}

/*
 * SkipPrefix — case-sensitive prefix match.
 *
 * Parameters:
 *   big    — the string to test.
 *   little — the expected prefix.
 *
 * Returns: a pointer into `big` just past the prefix on match, or NULL.
 *          Unlike StringStartsWithNoCase, this comparison is case-sensitive.
 */
const char *SkipPrefix(const char *big, const char *little) {
  for (; *little; big++, little++) {
    if (*little != *big)
      return NULL;
  }
  return big;
}

/*
 * StrSet — replace a heap-allocated string with a duplicate of a new one.
 *
 * Parameters:
 *   rv — pointer to the string pointer to update.
 *   s  — the new string value to duplicate.
 *
 * Side effects: frees the previous string at *rv. The strdup is performed
 *               before the free, so it is safe even if `s` points into the
 *               old string (though that usage does not occur in this codebase).
 */
void StrSet(char **rv, const char *s) {
  char *news = strdup(s);
  char *old = *rv;
  *rv = news;
  free(old);
}

/*
 * StrFmt — allocate a formatted string (printf-style).
 *
 * Parameters:
 *   fmt — printf format string.
 *   ... — format arguments.
 *
 * Returns: a newly allocated string containing the formatted result.
 *          Dies if the formatted output exceeds 4095 characters.
 */
char *StrFmt(const char *fmt, ...) {
  /* 4096 bytes is generous for the config-related formatting this function
   * is used for (file paths, error messages, window titles). */
  char buf[4096];
  va_list va;
  va_start(va, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, va);
  if (n < 0 || n >= sizeof(buf)) Die("vsnprintf failed");
  va_end(va);
  return strdup(buf);
}

/*
 * ByteArray_Resize — grow the array to hold at least `new_size` bytes.
 *
 * If the requested size exceeds the current capacity, the backing buffer is
 * reallocated using a 1.5x growth factor (capacity + capacity/2 + 8). This
 * ensures amortized O(1) cost for repeated single-byte appends while keeping
 * memory overhead below 2x. The "+8" prevents degenerate behavior when
 * capacity is very small (0 or 1).
 *
 * Parameters:
 *   arr      — the array to resize.
 *   new_size — the new logical size (number of valid bytes).
 *
 * Side effects: may reallocate arr->data. Dies on allocation failure.
 */
void ByteArray_Resize(ByteArray *arr, size_t new_size) {
  arr->size = new_size;
  if (new_size > arr->capacity) {
    /* Grow by at least 50% + 8 bytes to amortize repeated appends. */
    size_t minsize = arr->capacity + (arr->capacity >> 1) + 8;
    arr->capacity = new_size < minsize ? minsize : new_size;
    void *data = realloc(arr->data, arr->capacity);
    if (!data) Die("memory allocation failed");
    arr->data = data;
  }
}

/*
 * ByteArray_Destroy — free the backing buffer and reset the array.
 *
 * Parameters:
 *   arr — the array to destroy. After this call, arr->data is NULL.
 */
void ByteArray_Destroy(ByteArray *arr) {
  void *data = arr->data;
  arr->data = NULL;
  free(data);
}

/*
 * ByteArray_AppendData — append a block of bytes to the array.
 *
 * Parameters:
 *   arr       — the target array.
 *   data      — pointer to the bytes to append.
 *   data_size — number of bytes to append.
 */
void ByteArray_AppendData(ByteArray *arr, const uint8 *data, size_t data_size) {
  ByteArray_Resize(arr, arr->size + data_size);
  memcpy(arr->data + arr->size - data_size, data, data_size);
}

/*
 * ByteArray_AppendByte — append a single byte to the array.
 *
 * Parameters:
 *   arr — the target array.
 *   v   — the byte value to append.
 */
void ByteArray_AppendByte(ByteArray *arr, uint8 v) {
  ByteArray_Resize(arr, arr->size + 1);
  arr->data[arr->size - 1] = v;
}

/*
 * FindIndexInMemblk — look up item `i` in a packed indexed binary blob.
 *
 * The blob format stores an array of variable-length items concatenated
 * together, preceded by an offset table. A 16-bit element count is stored
 * in the last 2 bytes of the blob. Two index widths are supported:
 *
 *   - 16-bit mode (count < 8192): offset table uses uint16 entries.
 *     Compact, sufficient for blobs under 64 KB.
 *
 *   - 32-bit mode (count >= 8192): the stored count has 8192 added as a
 *     sentinel; offset table uses uint32 entries, supporting up to 4 GB.
 *
 * Layout (16-bit example with N items):
 *   [offset_0..offset_{N-1}][item_0_data][item_1_data]...[item_{N-1}_data][N]
 *   ^--- offset table ---^  ^--- item data region -------------------------^
 *
 * Each offset value is cumulative bytes from the start of the data region.
 * Item 0 starts at the data region base (right after the offset table).
 *
 * Parameters:
 *   data — the packed blob as a MemBlk (pointer + size).
 *   i    — zero-based index of the item to retrieve.
 *
 * Returns: a MemBlk pointing into `data` at the requested item, or
 *          {0, 0} if the index is out of range or the blob is malformed.
 */
// Automatically selects between 16 or 32 bit indexes. Can hold up to 8192 elements in 16-bit mode.
MemBlk FindIndexInMemblk(MemBlk data, size_t i) {
  if (data.size < 2)
    return (MemBlk) { 0, 0 };
  /* The last two bytes store the element count. `end` is the offset of that
   * trailer, which also serves as the upper bound for item data. */
  size_t end = data.size - 2, left_off, right_off;
  size_t mx = *(uint16 *)(data.ptr + end);
  if (mx < 8192) {
    /* 16-bit offset mode: each offset entry is 2 bytes, so the offset table
     * occupies bytes [0 .. mx*2). Item data starts at offset mx*2. */
    if (i > mx || mx * 2 > end)
      return (MemBlk) { 0, 0 };
    /* Item 0 starts right after the offset table. For subsequent items, the
     * start is the data-region base plus the cumulative offset at [i-1]. */
    left_off = ((i == 0) ? mx * 2 : mx * 2 + *(uint16 *)(data.ptr + i * 2 - 2));
    /* The end of item i is the start of item i+1, or `end` for the last. */
    right_off = (i == mx) ? end : mx * 2 + *(uint16 *)(data.ptr + i * 2);
  } else {
    /* 32-bit offset mode: subtract the 8192 sentinel to recover the true
     * element count. Each offset entry is 4 bytes. */
    mx -= 8192;
    if (i > mx || mx * 4 > end)
      return (MemBlk) { 0, 0 };
    left_off = ((i == 0) ? mx * 4 : mx * 4 + *(uint32 *)(data.ptr + i * 4 - 4));
    right_off = (i == mx) ? end : mx * 4 + *(uint32 *)(data.ptr + i * 4);
  }
  /* Sanity check: offsets must be in order and within bounds. */
  if (left_off > right_off || right_off > end)
    return (MemBlk) { 0, 0 };
  return (MemBlk) { data.ptr + left_off, right_off - left_off };
}


static uint64 BpsDecodeInt(const uint8 **src) {
  uint64 data = 0, shift = 1;
  while(true) {
    uint8 x = *(*src)++;
    data += (x & 0x7f) * shift;
    if(x & 0x80) break;
    shift <<= 7;
    data += shift;
  }
  return data;
}

#define CRC32_POLYNOMIAL 0xEDB88320

static uint32 crc32(const void *data, size_t length) {
  uint32 crc = 0xFFFFFFFF;
  const uint8 *byteData = (const uint8 *)data;
  for (size_t i = 0; i < length; i++) {
    crc ^= byteData[i];
    for (int j = 0; j < 8; j++)
      crc = (crc >> 1) ^ ((crc & 1) * CRC32_POLYNOMIAL);
  }
  return crc ^ 0xFFFFFFFF;
}

uint8 *ApplyBps(const uint8 *src, size_t src_size_in,
  const uint8 *bps, size_t bps_size, size_t *length_out) {
  const uint8 *bps_end = bps + bps_size - 12;

  if (memcmp(bps, "BPS1", 4))
    return NULL;
  if (crc32(src, src_size_in) != *(uint32 *)(bps_end))
    return NULL;
  if (crc32(bps, bps_size - 4) != *(uint32 *)(bps_end + 8))
    return NULL;

  bps += 4;
  uint32 src_size = BpsDecodeInt(&bps);
  uint32 dst_size = BpsDecodeInt(&bps);
  uint32 meta_size = BpsDecodeInt(&bps);
  uint32 outputOffset = 0;
  uint32 sourceRelativeOffset = 0;
  uint32 targetRelativeOffset = 0;
  if (src_size != src_size_in)
    return NULL;
  *length_out = dst_size;
  uint8 *dst = malloc(dst_size);
  if (!dst)
    return NULL;
  while (bps < bps_end) {
    uint32 cmd = BpsDecodeInt(&bps);
    uint32 length = (cmd >> 2) + 1;
    switch (cmd & 3) {
    case 0:
      while(length--) {
        dst[outputOffset] = src[outputOffset];
        outputOffset++;
      }
      break;
    case 1:
      while (length--)
        dst[outputOffset++] = *bps++;
      break;
    case 2:
      cmd = BpsDecodeInt(&bps);
      sourceRelativeOffset += (cmd & 1 ? -1 : +1) * (cmd >> 1);
      while (length--)
        dst[outputOffset++] = src[sourceRelativeOffset++];
      break;
    default:
      cmd = BpsDecodeInt(&bps);
      targetRelativeOffset += (cmd & 1 ? -1 : +1) * (cmd >> 1);
      while(length--)
        dst[outputOffset++] = dst[targetRelativeOffset++];
      break;
    }
  }
  if (dst_size != outputOffset)
    return NULL;
  if (crc32(dst, dst_size) != *(uint32 *)(bps_end + 4))
    return NULL;
  return dst;
}