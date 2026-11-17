/* cachewrites.h -- cache writes out of the game folder (cachewrites.c, cachewrites_rule.c).
 *
 * The engine writes its cache files relative to its base path, which is the game folder:
 * `generated/cm/<name>.bcm`, `generated/models/effects/particles/*.pmodel`, a staging file
 * `generated/temp/<path>.%4X.tmp` renamed into place, and `maps/DynamicSnapMap.flight`. Every one goes through
 * the engine's atomic write, AtomicWrite(rel, pathType, buf, len) (VK 0x2A03E0, GL 0x2A05A0). An absolute `rel`
 * is used unchanged: the engine puts its temp file beside the target and renames it there.
 *
 * The engine's file search path has `<fs_savepath>\base` ahead of the game folder (fs_savepath defaults to
 * `%USERPROFILE%\Saved Games\id Software\DOOM`), so a cache file written under `<fs_savepath>\base\` is found by
 * the engine's readers with no further change. Snapmap+ detours the atomic write and hands it that absolute path
 * for each of its cache writes; every other write is passed through unchanged. */
#ifndef SNAPMAP_PLUS_CACHEWRITES_H
#define SNAPMAP_PLUS_CACHEWRITES_H

#include <stddef.h>
#include <stdint.h>

/* Where one of the engine's writes goes. 1, with `<root>\base\<rel>` in out (every `/` of rel turned into `\`),
 * when rel is a cache write: it begins (ASCII case-insensitively, either slash) with `generated/`, or begins
 * with `maps/` and ends with `.flight`. 0, with out untouched, for any other write, and when rel or root is NULL
 * or empty, rel is absolute (a drive letter or a leading slash), rel has a `..` element, or the path does not
 * fit in cap bytes with its terminator. A trailing slash on root is not doubled. Pure. */
int sh_cachewrite_redirect(const char *rel, const char *root, char *out, size_t cap);

/* The signature table (cachewrites_sigs.c), declared where signatures.h is included first. */
#ifdef BACKEND_SIGNATURES_H
extern const sig_entry SH_CACHEWRITES_SIGNATURES[];   /* NULL-terminated */
#endif

/* Detour the atomic write (both renderers). 1 when cache writes go to the save path. */
int sh_cachewrites_install(const uint8_t *module_base);

/* Cache writes redirected since the start (for the test bench). */
long sh_cachewrites_redirected(void);

#endif /* SNAPMAP_PLUS_CACHEWRITES_H */
