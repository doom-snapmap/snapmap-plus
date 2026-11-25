/* cachewrites.h -- redirects the engine's cache writes (`generated/...`, `maps/DynamicSnapMap.flight`) out of the game
 * folder: AtomicWrite(rel, pathType, buf, len) (VK 0x2A03E0, GL 0x2A05A0) uses an absolute `rel` unchanged, so the
 * detour hands it `<fs_savepath>\base\<rel>`, which the engine's search path finds first (see
 * cachewrites.c, cachewrites_rule.c). */
#ifndef SNAPMAP_PLUS_CACHEWRITES_H
#define SNAPMAP_PLUS_CACHEWRITES_H

#include <stddef.h>
#include <stdint.h>

/* Decides where one of the engine's writes goes. Returns 1, with `<root>\base\<rel>` in out (every `/` of rel
 * turned into `\`, a trailing slash on root not doubled), when rel is a cache write: it begins (ASCII
 * case-insensitively) with `generated/`, or begins with `maps/` and ends with `.flight`. Returns 0, with out
 * untouched, for any other write, a NULL or empty rel or root, an absolute rel, a `..` element, or a path over cap. */
int sh_cachewrite_redirect(const char *rel, const char *root, char *out, size_t cap);

/* The signature table (cachewrites_sigs.c), declared where signatures.h is included first. */
#ifdef BACKEND_SIGNATURES_H
extern const sig_entry SH_CACHEWRITES_SIGNATURES[];   /* NULL-terminated */
#endif

/* Detours the atomic write (both renderers). Returns 1 when cache writes go to the save path. */
int sh_cachewrites_install(const uint8_t *module_base);

#endif /* SNAPMAP_PLUS_CACHEWRITES_H */
