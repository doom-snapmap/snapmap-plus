/* clip_events.h -- event rows a mod adds to its own clips when they play on a game model.
 *
 * A mod writes an md6Def of its own that inherits the model its clip plays on and lists the rows in its
 * `events { anim "<clip>" { event ... } }` blocks, then names that md6Def (the carrier) in mod.json:
 *
 *   "animation_events": ["mods/<salt>/player/kill_rows.md6"]
 *
 * Nothing of the game is replaced or edited. When the engine looks up the events of a registered clip on a
 * model whose own md6Def chain answers nothing, the product makes a second call of the engine's own lookup on
 * the carrier, so the engine parses, keeps and dispatches the rows itself.
 *
 * Three halves:
 *   - The build (clip_events_rule.c, pure, registered with the builder through sh_mb_set_clip_events): the
 *     author's entries are checked and recorded in the product block's registries "clipEvents"
 *     ([{"md6Def", "base", "clips": [...]}]). A bad entry, or a bad clip of an entry, is left out with one
 *     warning; the mod still builds.
 *   - The registry of the open map's Play (clip_events_rule.c, pure): built from the "clipEvents" rows of the
 *     builds the Play serves, names only, never an engine pointer across maps. A clip is registered only when
 *     its name is under its own mod's namespace, md6/mods/<salt>/, so a stock clip never is.
 *   - The engine side (clip_events.c): the three event lookups (clip_events_sigs.c) detoured on both
 *     renderers, committed at the first Play whose mods register a clip, and the carrier found by name. */
#ifndef SNAPMAP_PLUS_CLIP_EVENTS_H
#define SNAPMAP_PLUS_CLIP_EVENTS_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The namespace every mod clip lives under; a clip whose name does not start with it is never looked at. */
#define SH_CE_CLIP_ROOT     "md6/mods/"
#define SH_CE_CLIP_ROOT_LEN 9
/* The author key of mod.json and the registries member the builder writes. */
#define SH_CE_AUTHOR_KEY    "animation_events"
#define SH_CE_REGISTRY_KEY  "clipEvents"
/* Parent pointers followed at most this many times (a model chain is a few deep). */
#define SH_CE_CHAIN_MAX     32

typedef void (*sh_ce_line_fn)(void *context, const char *line);

/* ------------------------------------------------------------ names (pure) */

/* 1 when `clip` starts with md6/mods/ (any case, either slash): the detour's one test for a stock clip. */
int sh_ce_under_mods(const char *clip);
/* 1 when `clip` is under md6/mods/<salt>/ and names something below it. */
int sh_ce_clip_of_mod(const char *clip, const char *salt);
/* 1 when `md6def` is a decl name under mods/<salt>/. */
int sh_ce_def_of_mod(const char *md6def, const char *salt);
/* Name equality as the engine's resource names compare: any case, '\' as '/'. */
int sh_ce_name_eq(const char *a, const char *b);

/* ------------------------------------------------------ md6Def text (pure) */

typedef struct sh_ce_md6 {
    char  *base;       /* init { inherit "<base>" }, malloc'd; NULL when it has none */
    char **clips;      /* each anim "<clip>" { ... } block of the top-level events block holding an event row */
    int    nclips;
    int    empty;      /* anim blocks with no event row (not registered) */
} sh_ce_md6;

/* Read an md6Def text. Returns 1, 0 out of memory. Quoted text and comments are skipped as the engine skips
 * them; an alias row (`anim "<clip>"` with no block) is not a block. */
int  sh_ce_md6_read(const char *text, size_t len, sh_ce_md6 *out);
void sh_ce_md6_free(sh_ce_md6 *m);

/* ------------------------------------------------------- the build (pure) */

/* The text of the mod's md6Def `name`: 1 with *text (malloc'd) and *len; 0 when the mod carries no md6Def of
 * that name; -1 when it could not be read. */
typedef int (*sh_ce_text_fn)(void *context, const char *name, char **text, size_t *len);

/* The registries "clipEvents" array for a build: `names[0..n-1]` are the author's entries (strings), `salt` the
 * mod's. Each entry that is not one of the mod's md6Defs, names no base, adds no event row, or repeats an
 * earlier entry is left out, and so is each clip outside md6/mods/<salt>/ or already given rows by an earlier
 * entry; every one of those is one warning through `warn`. Returns the compact JSON array (malloc'd), NULL when
 * nothing registers; *oom is set when memory ran out. */
char *sh_ce_build_registry(const char *salt, const char *const *names, int n, sh_ce_text_fn text, void *text_ctx,
                           sh_ce_line_fn warn, void *warn_ctx, int *oom);

/* ------------------------------------------------- the registry (pure) */

/* Row states (the engine side writes them; the registry starts every row at SH_CE_ROW_NEW). */
#define SH_CE_ROW_NEW       0   /* the carrier has not been looked for in this Play */
#define SH_CE_ROW_READY     1   /* the carrier is loaded: `model` */
#define SH_CE_ROW_MISSING   2   /* the carrier did not load: its clip plays without the rows this Play */

typedef struct sh_ce_row {
    char *clip;               /* md6/mods/<salt>/....md6anim */
    char *carrier;            /* mods/<salt>/....md6 */
    char *base;               /* the model the carrier inherits */
    char *salt;
    char *label;              /* the mod as lines name it */
    void *volatile model;     /* the carrier's md6Def in this Play, or NULL */
    volatile long state;      /* SH_CE_ROW_* */
    volatile long answered;   /* lookups the carrier answered this Play */
    volatile long logged;     /* the first answer's line was written */
} sh_ce_row;

typedef struct sh_ce_registry {
    sh_ce_row *rows;          /* sorted by clip (sh_ce_registry_seal) */
    int n, cap;
    int mods;                 /* mods that registered at least one clip */
} sh_ce_registry;

/* Add one mod's rows from its build's registries object (`registries`, the block's JSON object text, or NULL).
 * Every row is checked again here: a carrier outside mods/<salt>/ or a clip outside md6/mods/<salt>/ is
 * dropped with one line, and a clip already registered keeps its first rows. Returns the rows added, -1 out of
 * memory. */
int  sh_ce_registry_add(sh_ce_registry *r, const char *salt, const char *label, const char *registries,
                        sh_ce_line_fn line, void *line_ctx);
/* Sort the rows for sh_ce_registry_find; call once after the last add. */
void sh_ce_registry_seal(sh_ce_registry *r);
/* The row of `clip`, or NULL. Lock-free; allocates nothing. */
sh_ce_row *sh_ce_registry_find(const sh_ce_registry *r, const char *clip);
void sh_ce_registry_free(sh_ce_registry *r);

/* ------------------------------------------------------ the chain (pure) */

/* A model's parent, or NULL (the engine side reads md6Def +0x98 under a fault guard; 1 when read). */
typedef int (*sh_ce_parent_fn)(void *context, const void *model, const void **parent);

/* 1 when the carrier's rows apply on `model`: `model` is the carrier or one of its ancestors, or `model`'s own
 * ancestors reach the carrier's base (its parent). 0 otherwise, or when a parent cannot be read. */
int sh_ce_in_chain(const void *carrier, const void *model, sh_ce_parent_fn parent, void *context);

/* -------------------------------------------------------- engine sites */

enum sh_ce_site { SH_CE_SITE_WINDOW, SH_CE_SITE_TYPE, SH_CE_SITE_TYPE2, SH_CE_SITE_COUNT };

/* One event lookup: its masked pattern at its recorded RVA, and the table query its first call (an E8 at
 * call_offset, the last byte of the pattern) must reach. The two filtered lookups share one pattern; the query
 * is what tells them apart. */
typedef struct sh_ce_site_sig {
    sig_entry fn;
    size_t    call_offset;
    sig_entry query;
    size_t    stolen;         /* the detour window: whole, position-independent instructions */
} sh_ce_site_sig;
extern const sh_ce_site_sig SH_CE_SITES[SH_CE_SITE_COUNT];

/* Resolve one site in an image (a mapped module or an image laid out by RVA, for the host's renderer):
 * SIG_OK with out->addr the lookup; SIG_CHANGED when its bytes or its first call's target are not the ones
 * recorded; SIG_NOT_FOUND when the renderer has no RVA. */
sig_status sh_ce_site_resolve(const uint8_t *module_base, const sh_ce_site_sig *s, sig_result *out);

/* ---------------------------------------------------------- engine side */

/* Boot: resolve the three lookups and prepare their detours. Commits nothing. 0 (with the feature recorded
 * off) when a lookup is not the one recorded in this executable. */
int  sh_clip_events_install(const uint8_t *module_base);
/* The Play gate (modserve_engine.c, main thread, before the level load): this Play's registry from the builds
 * it serves; the detours are committed the first time a registry has a row. */
void sh_clip_events_play_begin(void);
/* The map-load gate and CancelMapChange (main thread): the registry is dropped. */
void sh_clip_events_map_end(void);
/* The console listing `sh_mods clip-events`: one line per carrier and clip, through `line`. */
void sh_clip_events_command(sh_ce_line_fn line, void *context);

#ifdef __cplusplus
}
#endif
#endif /* SNAPMAP_PLUS_CLIP_EVENTS_H */
