/* attach_tags.h -- tag info a mod gives for its own animated attachments on a game model.
 *
 * An animated attachment (an ae_animatedAttachment_Attach clip event) is attached to a host model at a tag. The
 * engine's idAttachmentCollection::AddAttachment reads the tag info for (attached md6Def name, tag name) from
 * the HOST model's `props` table through one function, the tag-info lookup. A mod that attaches its own md6Def
 * to the player's body cannot give that info: the player's md6Def is the game's and is never edited. A mod
 * writes an md6Def of its own that inherits the host model, lists the info in a top-level `props` block of the
 * engine's own form, and names it (the carrier) in mod.json:
 *
 *   "attachment_tags": ["mods/<salt>/player/human/base/tp_body.md6"]
 *
 *   props { prop "mods/<salt>/gun.md6" { tag "hand_r" { trans ( x y z ) rot ( x y z w ) parent "<joint>" } } }
 *
 * Nothing of the game is replaced or edited. When the engine's lookup answers no tag for a prop under the mod's
 * own namespace on a host model in the carrier's chain, the product makes a second call of the engine's own
 * lookup on the carrier's props, so the engine parses and keeps the info itself.
 *
 * Three halves, as clip events (clip_events.h) has them:
 *   - The build (attach_tags_rule.c, pure, registered with the builder through sh_mb_set_attach_tags): the
 *     author's entries are checked and recorded in the product block's registries "attachmentTags"
 *     ([{"md6Def", "base", "props": [...]}]). A bad entry, or a bad prop of an entry, is left out with one
 *     warning; the mod still builds.
 *   - The registry of the open map's Play (attach_tags_rule.c, pure): built from the "attachmentTags" rows of
 *     the builds the Play serves, names only. A prop is registered only when its name is under its own mod's
 *     namespace, mods/<salt>/, so a stock prop never is.
 *   - The engine side (attach_tags.c): the tag-info lookup (attach_tags_sigs.c) detoured on both renderers,
 *     committed at the first Play whose mods register a prop, and the carrier found by name. The chain rule and
 *     the name tests are clip events' (sh_ce_in_chain, sh_ce_name_eq, sh_ce_def_of_mod). */
#ifndef SNAPMAP_PLUS_ATTACH_TAGS_H
#define SNAPMAP_PLUS_ATTACH_TAGS_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"
#include "clip_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The author key of mod.json and the registries member the builder writes. */
#define SH_AT_AUTHOR_KEY    "attachment_tags"
#define SH_AT_REGISTRY_KEY  "attachmentTags"

/* ------------------------------------------------------------ names (pure) */

/* 1 when `prop` starts with mods/ (any case, either slash): the detour's one test for a stock prop. */
int sh_at_under_mods(const char *prop);
/* 1 when `prop` is an md6Def name under mods/<salt>/ and names something below it. */
int sh_at_prop_of_mod(const char *prop, const char *salt);

/* ------------------------------------------------------ md6Def text (pure) */

typedef struct sh_at_md6 {
    char  *base;       /* init { inherit "<base>" }, malloc'd; NULL when it has none */
    char **props;      /* each prop "<name>" { ... } block of the top-level props block holding a tag */
    int    nprops;
    int    empty;      /* prop blocks with no tag (not registered) */
} sh_at_md6;

/* Read an md6Def text. Returns 1, 0 out of memory. Quoted text and comments are skipped as the engine skips
 * them. */
int  sh_at_md6_read(const char *text, size_t len, sh_at_md6 *out);
void sh_at_md6_free(sh_at_md6 *m);

/* ------------------------------------------------------- the build (pure) */

/* The registries "attachmentTags" array for a build: `names[0..n-1]` are the author's entries (strings), `salt`
 * the mod's; `text` reads the mod's md6Defs (clip_events.h sh_ce_text_fn). Each entry that is not one of the
 * mod's md6Defs, names no base, gives no tag, or repeats an earlier entry is left out, and so is each prop
 * outside mods/<salt>/ or already given tags by an earlier entry; every one of those is one warning through
 * `warn`. Returns the compact JSON array (malloc'd), NULL when nothing registers; *oom is set when memory ran
 * out. */
char *sh_at_build_registry(const char *salt, const char *const *names, int n, sh_ce_text_fn text, void *text_ctx,
                           sh_ce_line_fn warn, void *warn_ctx, int *oom);

/* ------------------------------------------------- the registry (pure) */

/* Row states (the engine side writes them; the registry starts every row at SH_AT_ROW_NEW). */
#define SH_AT_ROW_NEW       0   /* the carrier has not been looked for in this Play */
#define SH_AT_ROW_READY     1   /* the carrier is loaded: `model` */
#define SH_AT_ROW_MISSING   2   /* the carrier did not load: the prop has no tag info from the mod this Play */

typedef struct sh_at_row {
    char *prop;               /* mods/<salt>/....md6 */
    char *carrier;            /* mods/<salt>/....md6 */
    char *base;               /* the model the carrier inherits */
    char *salt;
    char *label;              /* the mod as lines name it */
    void *volatile model;     /* the carrier's md6Def in this Play, or NULL */
    volatile long state;      /* SH_AT_ROW_* */
    volatile long answered;   /* lookups the carrier answered this Play */
    volatile long logged;     /* the first answer's line was written */
} sh_at_row;

typedef struct sh_at_registry {
    sh_at_row *rows;          /* sorted by prop (sh_at_registry_seal) */
    int n, cap;
    int mods;                 /* mods that registered at least one prop */
} sh_at_registry;

/* Add one mod's rows from its build's registries object (`registries`, the block's JSON object text, or NULL).
 * Every row is checked again here: a carrier or a prop outside mods/<salt>/ is dropped with one line, and a
 * prop already registered keeps its first rows. Returns the rows added, -1 out of memory. */
int  sh_at_registry_add(sh_at_registry *r, const char *salt, const char *label, const char *registries,
                        sh_ce_line_fn line, void *line_ctx);
/* Sort the rows for sh_at_registry_find; call once after the last add. */
void sh_at_registry_seal(sh_at_registry *r);
/* The row of `prop`, or NULL. Lock-free; allocates nothing. */
sh_at_row *sh_at_registry_find(const sh_at_registry *r, const char *prop);
void sh_at_registry_free(sh_at_registry *r);

/* -------------------------------------------------------- engine site */

/* The tag-info lookup, build 13954591: `const void *f(const void *props, const char *prop, const char *tag)`.
 * `props` is the idList at the host md6Def + 0x210 (rows of 0x148 bytes, the prop name char* at +0x10 of a
 * row); the answer is a tag record whose int16 at +0x1C is the resolved joint, -1 for no tag (the engine
 * answers a static default record with -1 when nothing matches). */
#define SH_AT_STOLEN 15       /* the detour window: mov [rsp+8],rbx; mov [rsp+0x10],rbp; mov [rsp+0x18],rsi */
extern const sig_entry SH_ATTACH_TAGS_SIGNATURES[];   /* NULL-terminated; [0] is TagInfoLookup */

/* ---------------------------------------------------------- engine side */

/* Boot: resolve the lookup and prepare its detour. Commits nothing. 0 (with the feature recorded off) when the
 * lookup is not the one recorded in this executable. */
int  sh_attach_tags_install(const uint8_t *module_base);
/* The Play gate (modserve_engine.c, main thread, before the level load): this Play's registry from the builds
 * it serves; the detour is committed the first time a registry has a row. */
void sh_attach_tags_play_begin(void);
/* The map-load gate and CancelMapChange (main thread): the registry is dropped. */
void sh_attach_tags_map_end(void);
/* The console listing `sh_mods attachment-tags`: one line per carrier and prop, through `line`. */
void sh_attach_tags_command(sh_ce_line_fn line, void *context);

#ifdef __cplusplus
}
#endif
#endif /* SNAPMAP_PLUS_ATTACH_TAGS_H */
