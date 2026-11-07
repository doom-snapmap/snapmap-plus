/* modregistry.h -- derived edits applied per map (DESIGN-additive 2.4).
 *
 * A mod that edits a game file carries the edited copy at the game's name;
 * the builder records the structural diff against the game's text in the
 * product block's "edits" (decl_tree.h) and the copy is never served. The
 * serving index (modserve.c, ms_scope_compute) collects, for the current
 * listing, every game file a serving bundle edits, and serves what this file
 * generates as a synthetic file at the game's name: the game's text with
 * every declared bundle's operations applied in row order. Identical
 * operations merge, additions union, and a field or keyed entry two bundles
 * set to different values is the map's choice (the manifest's "choices",
 * slot "<type>/<decl name>/<path>") else the first in row order, one line.
 * Every game file goes through the same apply; the one rule per file is the
 * encounter conductor's (entitydef/ai/conductor/coop/snapmap_default):
 *
 *   conductor   each aiTypeList item a bundle adds whose aiType is a number
 *               that is not a stock SPAWN_AI_TYPE_* bit gets a bit assigned
 *               here per map (the lowest free above the stock bits, 1 << 19
 *               .. 1 << 29, in row order, then by encounter decl name); every
 *               snapAiEncounter memberType and snapAiScore aiType of that
 *               bundle naming the number the author wrote is served
 *               rewritten to the assigned bit.
 */
#ifndef SNAPMAP_PLUS_MODREGISTRY_H
#define SNAPMAP_PLUS_MODREGISTRY_H

#include <stddef.h>
#include "modbundle.h"

#define SH_MR_BIT_FIRST 19   /* above SPAWN_AI_TYPE_ZOMBIE_UNWILLING, 1 << 18 */
#define SH_MR_BIT_LAST  29   /* below SPAWN_AI_TYPE_ANY, 1 << 30 */

#define SH_MR_SLOT_MAX    160   /* "<type>/<decl name>/<path>" */
#define SH_MR_KEY_MAX     65
#define SH_MR_CHOICES_MAX 64
#define SH_MR_CLASH_SOURCES 8

/* ------------------------------------------------------------ generation */

/* One snapAiEncounter or snapAiScore decl a source serves. */
typedef struct sh_mr_decl {
    const char *name;          /* its decl name */
    const char *text;
    size_t len;
    int score;                 /* 1 snapAiScore, 0 snapAiEncounter */
    int id;                    /* the caller's file id, echoed in sh_mr_rewrite */
} sh_mr_decl;

/* One bundle that serves for the listing, in serving order. */
typedef struct sh_mr_source {
    const char *key;           /* its key */
    const char *who;           /* key@version (hash16), for lines */
    const char *edits;         /* its product block's "edits" (JSON array), or NULL */
    size_t edits_len;
    const sh_mr_decl *decls;   /* its encounters and scores (the conductor's bit rewrite) */
    int ndecls;
} sh_mr_source;

/* One game file the sources edit, with the game's text (NULL when it cannot
 * be read: the game's own file then serves, with one line). */
typedef struct sh_mr_target { const char *request; const char *text; size_t len; } sh_mr_target;

typedef struct sh_mr_choice { const char *slot, *key; } sh_mr_choice;

typedef struct sh_mr_entry {
    int source;
    unsigned authored, bit;
    char def[SH_MB_REQUEST_MAX + 1];
    char encounter[SH_MB_REQUEST_MAX + 1];
} sh_mr_entry;

typedef struct sh_mr_rewrite { int id; char *text; size_t len; } sh_mr_rewrite;

/* A slot two or more sources set to different values. */
typedef struct sh_mr_clash {
    char slot[SH_MR_SLOT_MAX];
    int target;                /* the sh_mr_target it is in */
    int sources[SH_MR_CLASH_SOURCES];
    int n;
    int serving, by_choice;
} sh_mr_clash;

typedef struct sh_mr_result {
    char **text;               /* per target: malloc'd generated text, or NULL (the game's file serves) */
    size_t *len;
    unsigned char **used;      /* per target: per source, 1 when its edits changed the text */
    int ntargets, nsources;
    int body_clips, body_aliases;   /* the player body's totals when it is generated */
    sh_mr_entry *entries; int nentries;
    sh_mr_rewrite *rewrites; int nrewrites;
    sh_mr_clash *clashes; int nclashes;
    char **lines; int nlines;  /* what was generated and every clash, one line each */
} sh_mr_result;

/* Generate every target from `n` sources. Returns 1 (0 out of memory). */
int  sh_modregistry_generate(const sh_mr_source *src, int n, const sh_mr_target *targets, int ntargets,
                             const sh_mr_choice *choices, int nchoices, sh_mr_result *out);
void sh_modregistry_result_free(sh_mr_result *r);

/* The clip blocks of a player body text: each `anim "<clip>" {` block directly
 * inside its top-level `events` block. An alias row's `anim` reference, on
 * one line or several, is not one. Pure. */
int  sh_modregistry_body_clips(const char *text, size_t len);

/* ------------------------------------------------------------ text ------ */

/* The items of an aiTypeList: fn(aiType, entityDef) per item, in order.
 * Returns the count, -1 when the text has no aiTypeList. */
int  sh_modregistry_conductor_rows(const char *text, size_t len,
                                   void (*fn)(void *context, const char *ai_type, const char *entity_def),
                                   void *context);
/* `text` with every number `from[i]` inside a memberType or aiType string
 * value written as `to[i]`. Malloc'd; NULL when nothing changed. */
char *sh_modregistry_rewrite_bits(const char *text, size_t len, const unsigned *from, const unsigned *to, int n,
                                  size_t *out_len);
/* 1 when `value` is one of the game's SPAWN_AI_TYPE_* bits (1 .. 1 << 18, 1 << 30). */
int  sh_modregistry_stock_bit(unsigned value);

/* ------------------------------------------------------------ choices --- */

/* 1 when `slot` reads as "<type>/<decl name>/<path>": printable, no blank,
 * quote or backslash, below SH_MR_SLOT_MAX, at least three '/'-separated
 * parts. */
int  sh_modregistry_slot_valid(const char *slot);
/* The open map's choice for a slot: key NULL or "" removes it. Recorded in the
 * map's manifest (saved with the map) and given to the serving index, which
 * recomputes the map's generated files. Returns the bundles whose serving
 * changed (>= 0), or -1 when the slot or key is not valid. The caller applies
 * a change as after any scope change (modui's scope apply). */
int  sh_modregistry_choices_set(const char *slot, const char *key);
/* 1 with the key chosen for `slot`, 0 when the map has no choice for it. */
int  sh_modregistry_choices_get(const char *slot, char *key, size_t cap);
/* The console verb `sh_mods choose "<game file>/<path>" "<key>"` (modcmd calls
 * it; an empty key clears): one line for the console in `out`. Returns as
 * sh_modregistry_choices_set. */
int  sh_modregistry_choose_command(const char *slot, const char *key, char *out, size_t cap);

#endif /* SNAPMAP_PLUS_MODREGISTRY_H */
