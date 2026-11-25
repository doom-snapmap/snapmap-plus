/* attach_tags.h -- tag info a mod gives its own animated attachments on a game model: md6Defs named in mod.json
 * "attachment_tags" that inherit the host model and carry a top-level `props` block. When the engine's lookup finds
 * no tag under the mod's namespace, the product calls the lookup again on the carrier's props. */
#ifndef SNAPMAP_PLUS_ATTACH_TAGS_H
#define SNAPMAP_PLUS_ATTACH_TAGS_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"
#include "clip_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The mod.json author key and the registries member the builder writes. */
#define SH_AT_AUTHOR_KEY    "attachment_tags"
#define SH_AT_REGISTRY_KEY  "attachmentTags"

/* 1 when `prop` starts with mods/ (any case, either slash): the detour's one test for a stock prop. */
int sh_at_under_mods(const char *prop);
/* 1 when `prop` is an md6Def name under mods/<salt>/ and names something below it. */
int sh_at_prop_of_mod(const char *prop, const char *salt);

typedef struct sh_at_md6 {
    char  *base;       /* init { inherit "<base>" }, malloc'd; NULL when it has none */
    char **props;      /* each prop "<name>" { ... } block of the top-level props block holding a tag */
    int    nprops;
    int    empty;      /* prop blocks with no tag (not registered) */
} sh_at_md6;

/* Reads an md6Def text, skipping quoted text and comments as the engine does. Returns 1, 0 out of memory. */
int  sh_at_md6_read(const char *text, size_t len, sh_at_md6 *out);
void sh_at_md6_free(sh_at_md6 *m);

/* Builds the registries "attachmentTags" array from the author's entries `names[0..n-1]`; `text` reads the mod's
 * md6Defs. An entry or prop that is invalid or repeated is left out with one warning through `warn`. Returns the
 * compact JSON array (malloc'd), NULL when nothing registers; *oom is set when memory ran out. */
char *sh_at_build_registry(const char *salt, const char *const *names, int n, sh_ce_text_fn text, void *text_ctx,
                           sh_ce_line_fn warn, void *warn_ctx, int *oom);

/* Row states, written by the engine side; every row starts at SH_AT_ROW_NEW. */
#define SH_AT_ROW_NEW       0   /* the carrier has not been looked for in this Play */
#define SH_AT_ROW_READY     1
#define SH_AT_ROW_MISSING   2   /* the carrier did not load */

typedef struct sh_at_row {
    char *prop;               /* mods/<salt>/....md6 */
    char *carrier;            /* mods/<salt>/....md6 */
    char *base;               /* the model the carrier inherits */
    char *salt;
    char *label;              /* the mod as lines name it */
    void *volatile model;     /* the carrier's md6Def in this Play, or NULL */
    volatile long state;
    volatile long answered;   /* lookups the carrier answered this Play */
    volatile long logged;     /* the first answer's line is written */
} sh_at_row;

typedef struct sh_at_registry {
    sh_at_row *rows;          /* sorted by prop (sh_at_registry_seal) */
    int n, cap;
    int mods;                 /* mods that registered at least one prop */
} sh_at_registry;

/* Adds one mod's rows from its build's registries object text (or NULL). A carrier or prop outside mods/<salt>/
 * is dropped with one line, and a prop already registered keeps its first rows. Returns the rows added, -1 out of
 * memory. */
int  sh_at_registry_add(sh_at_registry *r, const char *salt, const char *label, const char *registries,
                        sh_ce_line_fn line, void *line_ctx);
/* Sorts the rows for sh_at_registry_find; call once after the last add. */
void sh_at_registry_seal(sh_at_registry *r);
/* Returns the row of `prop`, or NULL. Lock-free; allocates nothing. */
sh_at_row *sh_at_registry_find(const sh_at_registry *r, const char *prop);
void sh_at_registry_free(sh_at_registry *r);

/* The tag-info lookup, build 13954591: `const void *f(const void *props, const char *prop, const char *tag)`.
 * `props` is the idList at host md6Def + 0x210 (0x148-byte rows, prop name char* at +0x10); the answer's int16 at
 * +0x1C is the joint (-1: none). SH_AT_STOLEN is the detour window; signatures[0] is this lookup. */
#define SH_AT_STOLEN 15
extern const sig_entry SH_ATTACH_TAGS_SIGNATURES[];

/* Boot: resolves the lookup and prepares its detour without committing it. Returns 0 (feature off) when the
 * lookup is not the one recorded for this executable. */
int  sh_attach_tags_install(const uint8_t *module_base);
/* Play gate (main thread, before the level load): builds this Play's registry and commits the detour the first
 * time a registry has a row. */
void sh_attach_tags_play_begin(void);
/* Map-load gate and CancelMapChange (main thread): drops the registry. */
void sh_attach_tags_map_end(void);
/* The `sh_mods attachment-tags` listing: one line per carrier and prop, through `line`. */
void sh_attach_tags_command(sh_ce_line_fn line, void *context);

#ifdef __cplusplus
}
#endif
#endif /* SNAPMAP_PLUS_ATTACH_TAGS_H */
