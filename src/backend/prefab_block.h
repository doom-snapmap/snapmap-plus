/* prefab_block.h -- the product's block inside a prefab file: one top-level member "snapmapPlus" after the last
 * engine member, which every reader strips to restore the engine's bytes exactly. Pure C, so the backend and the
 * WebView host link it; prefab_mods.c composes and reads the block. */
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

/* Find the "snapmapPlus" member among the top-level members of a complete JSON object (comments and a UTF-8 BOM
 * are skipped; the last such member wins). Returns 1 found, 0 none, -1 not a complete top-level object. */
int    sh_prefab_block_find(const char *text, size_t len, sh_prefab_block_span *out);

/* The same for the end of a file whose start was not read (the Prefabs list reads a tail of a large file): finds
 * the last "snapmapPlus" member that closes the top-level object. Prefer sh_prefab_block_find on a complete text. */
int    sh_prefab_block_find_tail(const char *tail, size_t len, sh_prefab_block_span *out);

/* Remove the block in place. Returns the new length (len when there is none
 * or the text is not an object) and NUL-terminates when there is room. */
size_t sh_prefab_block_strip(char *text, size_t len);

/* The text with `block` (a JSON object's text) as its block: replaced if present, else inserted after the last
 * top-level member. malloc'd, NUL-terminated, *out_len set; NULL when not a complete object or out of memory. */
char  *sh_prefab_block_put(const char *text, size_t len, const char *block, size_t block_len, size_t *out_len);

/* Remove every block and line comment outside a string, in place, since the engine's serializer writes one before
 * almost every member and a product-written prefab is plain JSON. Strings are kept byte for byte; an unterminated
 * block comment runs to the end and a line comment keeps its newline. Returns the new length, NUL-terminated. */
size_t sh_prefab_block_strip_comments(char *text, size_t len);

/* The top-level "~type" string's escaped body, copied into out (truncated).
 * Returns 1 when found, 0 when the object has none, -1 not an object. */
int    sh_prefab_block_top_type(const char *text, size_t len, char *out, size_t cap);

/* The one prefab name rule, for the Prefabs tab and the backend: 1 when `name` (UTF-8) is 1 to SH_PREFAB_NAME_MAX
 * bytes, has no control character or any of < > : " / \ | ? *, no leading or trailing space or dot, no "..", no
 * ".meta" ending, and is not a Windows device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, alone or before an extension). */
#define SH_PREFAB_NAME_MAX 200
int    sh_prefab_block_valid_name(const char *name);


#ifdef __cplusplus
}
#endif

#endif
