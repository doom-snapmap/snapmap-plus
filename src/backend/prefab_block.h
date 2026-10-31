/* prefab_block.h -- the product's block inside a prefab file.
 *
 * A prefab is one file: the engine's JSON (an idSnapEntityPrefab, or the
 * idSnapMap of a module prefab) with one product-owned top-level member,
 * "snapmapPlus", after the engine's own members:
 *
 *   {...the engine's members...,
 *   "snapmapPlus":{"format":1,"description":"","tags":[],"mods":[...],"summary":{...}}}
 *
 * The engine's reflection parser skips an unknown top-level key, but nothing
 * here depends on that: every reader strips the member before the engine (or
 * the module prefab merge) sees the text. Stripping restores the engine's
 * bytes exactly: the block is inserted as ",\n\"snapmapPlus\":<object>" right
 * after the last engine member, and that span is what strip removes.
 *
 * Pure C, no allocation except where a function says so, no engine or JSON
 * tree dependency, so both the backend and the WebView host link it. The
 * block's contents are composed and read by prefab_mods.c. */
#ifndef SH_PREFAB_BLOCK_H
#define SH_PREFAB_BLOCK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_PREFAB_BLOCK_KEY    "snapmapPlus"
#define SH_PREFAB_BLOCK_FORMAT 1

typedef struct sh_prefab_block_span {
    size_t value_at, value_len;   /* the block's object text */
    size_t cut_at, cut_len;       /* the bytes strip removes (the member and its separator) */
} sh_prefab_block_span;

/* Find the "snapmapPlus" member among the top-level members of a complete
 * JSON object (block comments, line comments and a UTF-8 byte order mark are
 * skipped). Returns 1 found, 0 the object has none, -1 the text is not a
 * complete top-level object. The last such member wins. */
int    sh_prefab_block_find(const char *text, size_t len, sh_prefab_block_span *out);

/* The same for the end of a file whose start was not read (the Prefabs list
 * reads at most a tail of a large file): finds the last "snapmapPlus" member
 * that closes the top-level object. Returns 1 found, 0 not found. On a
 * complete text, prefer sh_prefab_block_find. */
int    sh_prefab_block_find_tail(const char *tail, size_t len, sh_prefab_block_span *out);

/* Remove the block in place. Returns the new length (len when there is none
 * or the text is not an object) and NUL-terminates when there is room. */
size_t sh_prefab_block_strip(char *text, size_t len);

/* The text with `block` (a JSON object's text) as its block: any block it
 * has is replaced, otherwise one is inserted after the last top-level member.
 * malloc'd and NUL-terminated, *out_len set; NULL when the text is not a
 * complete top-level object or out of memory. */
char  *sh_prefab_block_put(const char *text, size_t len, const char *block, size_t block_len, size_t *out_len);

/* Remove every block comment and line comment outside a string, in place:
 * the engine's serializer writes a comment before almost every member
 * (a "camera yaw" comment before "cameraYaw"), and a prefab file the product writes is plain
 * JSON. Strings are kept byte for byte, comment openers inside them included,
 * and an escaped quote does not end one. Comments do not nest: a block
 * comment ends at the first star and slash after it, and one that never ends
 * runs to the end of the text; a line comment ends before its newline, which
 * is kept. Returns the new length and NUL-terminates when there is room. */
size_t sh_prefab_block_strip_comments(char *text, size_t len);

/* The top-level "~type" string's escaped body, copied into out (truncated).
 * Returns 1 when found, 0 when the object has none, -1 not an object. */
int    sh_prefab_block_top_type(const char *text, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
