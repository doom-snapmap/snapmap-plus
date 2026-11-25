/* clip_events.h -- event rows a mod adds to its own clips when they play on a game model. The mod names an md6Def
 * (the carrier) that inherits the model and lists the rows in `events { anim "<clip>" { event ... } }` blocks, in
 * mod.json "animation_events"; when the engine's lookup for a registered clip finds nothing on the model's chain,
 * the product calls it again on the carrier. The header holds the pure build check, the pure per-Play registry (names
 * only, never an engine pointer across maps, clips only under md6/mods/<salt>/), and the engine side. */
#ifndef SNAPMAP_PLUS_CLIP_EVENTS_H
#define SNAPMAP_PLUS_CLIP_EVENTS_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"
#include "md6_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The namespace every mod clip lives under; other clips are never looked at. */
#define SH_CE_CLIP_ROOT     "md6/mods/"
#define SH_CE_CLIP_ROOT_LEN 9
/* The namespace every mod decl lives under (an entityDef, an md6Def). */
#define SH_CE_DECL_ROOT     "mods/"
#define SH_CE_DECL_ROOT_LEN 5
/* The registries member the builder writes. */
#define SH_CE_REGISTRY_KEY  "clipEvents"
/* The most parent pointers followed on a model chain. */
#define SH_CE_CHAIN_MAX     32

/* 1 when `clip` starts with md6/mods/ (any case, either slash): the detour's one test for a stock clip. */
int sh_ce_under_mods(const char *clip);
/* 1 when `clip` is under md6/mods/<salt>/ and names something below it. */
int sh_ce_clip_of_mod(const char *clip, const char *salt);
/* 1 when `md6def` is a decl name under mods/<salt>/. */
int sh_ce_def_of_mod(const char *md6def, const char *salt);
/* 1 when the decl `name` is under mods/ (any case, either slash): a mod's decl, never a stock one. */
int sh_ce_decl_under_mods(const char *name);

typedef struct sh_ce_md6 {
    char  *base;       /* init { inherit "<base>" }, malloc'd; NULL when it has none */
    char **clips;      /* each anim "<clip>" { ... } block of the top-level events block holding an event row */
    int    nclips;
    int    empty;      /* anim blocks with no event row (not registered) */
} sh_ce_md6;

/* Reads an md6Def text, skipping quoted text and comments as the engine does; an alias row (`anim "<clip>"`
 * with no block) is not a block. Returns 1, 0 out of memory. */
int  sh_ce_md6_read(const char *text, size_t len, sh_ce_md6 *out);
void sh_ce_md6_free(sh_ce_md6 *m);

/* The text of the mod's md6Def `name`: 1 with *text (malloc'd) and *len; 0 when the mod carries no md6Def of
 * that name; -1 when it could not be read. */
typedef int (*sh_ce_text_fn)(void *context, const char *name, char **text, size_t *len);

/* Builds the registries "clipEvents" array from the author's entries `names[0..n-1]`. An entry or clip that is
 * invalid or repeated is left out with one warning through `warn`. Returns the compact JSON array (malloc'd),
 * NULL when nothing registers; *oom is set when memory ran out. */
char *sh_ce_build_registry(const char *salt, const char *const *names, int n, sh_ce_text_fn text, void *text_ctx,
                           sh_ce_line_fn warn, void *warn_ctx, int *oom);

/* Reads a model's parent (md6Def +0x98 under a fault guard) into *parent; returns 1 when read. */
typedef int (*sh_ce_parent_fn)(void *context, const void *model, const void **parent);

/* Returns 1 when the carrier's rows apply on `model`: `model` is the carrier or one of its ancestors, or its
 * ancestors reach the carrier's base (its parent). Returns 0 otherwise, or when a parent cannot be read. */
int sh_ce_in_chain(const void *carrier, const void *model, sh_ce_parent_fn parent, void *context);

enum sh_ce_site { SH_CE_SITE_WINDOW, SH_CE_SITE_TYPE, SH_CE_SITE_TYPE2, SH_CE_SITE_COUNT };

/* One event lookup: its masked pattern at its recorded RVA, and the table query its first call (an E8 at
 * call_offset, the last byte of the pattern) must reach. The two filtered lookups share one pattern and the query
 * tells them apart. */
typedef struct sh_ce_site_sig {
    sig_entry fn;
    size_t    call_offset;
    sig_entry query;
    size_t    stolen;         /* the detour window: whole, position-independent instructions */
} sh_ce_site_sig;
extern const sh_ce_site_sig SH_CE_SITES[SH_CE_SITE_COUNT];

/* Resolves one site in an image (a mapped module or an image laid out by RVA): SIG_OK with out->addr the lookup;
 * SIG_CHANGED when its bytes or its first call's target are not the recorded ones; SIG_NOT_FOUND when the
 * renderer has no RVA. */
sig_status sh_ce_site_resolve(const uint8_t *module_base, const sh_ce_site_sig *s, sig_result *out);

/* Boot: resolves the three lookups and prepares their detours without committing them. Returns 0 (feature off)
 * when a lookup is not the one recorded for this executable. */
int  sh_clip_events_install(const uint8_t *module_base);
/* Play gate (main thread, before the level load): builds this Play's registry and commits the detours the first
 * time a registry has a row. */
void sh_clip_events_play_begin(void);
/* Map-load gate and CancelMapChange (main thread): drops the registry. */
void sh_clip_events_map_end(void);
/* The `sh_mods clip-events` listing: one line per carrier and clip, through `line`. */
void sh_clip_events_command(sh_ce_line_fn line, void *context);

#ifdef __cplusplus
}
#endif
#endif /* SNAPMAP_PLUS_CLIP_EVENTS_H */
