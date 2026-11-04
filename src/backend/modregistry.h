/* modregistry.h -- registries (DESIGN-additive 2.4): fixed-name game files
 * the product generates per map from the declared bundles' intent. No bundle
 * carries a file at a registry's name; the serving index (modserve.c,
 * ms_scope_compute) collects the bundles that serve for the current listing,
 * in the map's row order, and serves what this file generates as a synthetic
 * file at the game's name.
 *
 *   conductor   entitydef/ai/conductor/coop/snapmap_default: the game's text
 *               with one aiTypeList item per custom encounter type the
 *               bundles declare, { aiType = "<bit>"; entityDef = "<ai>"; },
 *               the bit assigned here per map (the lowest free bit above
 *               the stock SPAWN_AI_TYPE_* bits, 1 << 19 .. 1 << 29, in row
 *               order, then by encounter decl name); every snapAiEncounter
 *               memberType and snapAiScore aiType of that bundle naming the
 *               bit the author wrote is served rewritten to the assigned one.
 *   body        md6def/player/mp/tp_body_mp.md6: the game's text with each
 *               bundle's player.bodyEvents partial (events and aliases
 *               blocks, no init) appended by bag entry; a whole carried copy
 *               (a pack built before the intent) is composed as before.
 *   player      entitydef/player/coop: the game's text (or the first carried
 *               copy, a pack built before the intent) with the player
 *               fields set inside edit: handsModelMPUseDoomHands (hands),
 *               handsModelMPUseDoomHands_D1 (handsD1),
 *               thirdPersonBodyMPUseDoomHands (body), handsAnimWeb.
 *   weapons     generated/decls/weapon/<stock name>.decl: the mod decl a
 *               bundle's player.weapons names for that slot, its bytes
 *               served under the stock name, in this map only.
 *
 * Two bundles setting one player field or weapon slot: the map's choice
 * (the manifest's "choices", sh_modmanifest_current_choose) wins, else the
 * first in row order, and one line says so.
 *
 * extends (DESIGN-additive 2.4, "List-shaped stock decls in general"): a
 * bundle may extend a list-shaped stock decl,
 *   "extends": { "<type folder>/<stock decl name>": "mods/<key>/<partial decl>" }
 * (the product block's registries.extends, else the author block's), where
 * the partial holds only the bundle's own list entries (`num = N; item[i]`)
 * or bag entries (md6Def `anim`/`alias` blocks, repeated `subGraph` blocks)
 * inside the target's containers. The product appends them to the game's
 * text for maps that declare the bundle. The targets are one table
 * (sh_modregistry_extend_target): the encounter conductor (its aiTypeList
 * rows are the encounter types, bits assigned as above), the player body
 * (player.bodyEvents is its spelling), the AI FSM manager states
 * (aifsmmanager/states/<demon>, edit.subGraphs) and the whitelist
 * inspectors' edit.validEncounters. Any other target is "not a list the
 * product extends".
 *
 * Sources of encounter types: a carried conductor copy's aiTypeList items
 * whose aiType is a number that is not a stock bit (the legacy boss packs),
 * and the aiTypeList rows of a conductor extends partial; the encounter decl
 * whose memberType names that number orders the entry and is rewritten.
 */
#ifndef SNAPMAP_PLUS_MODREGISTRY_H
#define SNAPMAP_PLUS_MODREGISTRY_H

#include <stddef.h>
#include "modbundle.h"

#define SH_MR_CONDUCTOR 0
#define SH_MR_BODY      1
#define SH_MR_PLAYER    2
#define SH_MR_FIXED     3

#define SH_MR_BIT_FIRST 19   /* above SPAWN_AI_TYPE_ZOMBIE_UNWILLING, 1 << 18 */
#define SH_MR_BIT_LAST  29   /* below SPAWN_AI_TYPE_ANY, 1 << 30 */

#define SH_MR_SLOT_MAX    160   /* "weapons/<stock weapon decl name>" */
#define SH_MR_LIST 1             /* extends grammar: `num = N; item[i]` lists (entries matched by value or first member) */
#define SH_MR_BAG  2             /* extends grammar: repeated or named blocks appended (identical text skipped) */
#define SH_MR_KEY_MAX     65
#define SH_MR_CHOICES_MAX 64

/* The request path of a fixed registry (SH_MR_*). */
const char *sh_modregistry_request(int registry);
/* The request a weapon slot is served under: generated/decls/weapon/<slot>.decl, lower case. */
int sh_modregistry_weapon_request(const char *slot, char *out, size_t cap);

/* ------------------------------------------------------------ intent ---- */

/* What a bundle's author block (modserve's "extra" JSON) asks of the registries. */
typedef struct sh_mr_intent {
    char *player[SH_MB_PLAYER_FIELDS];            /* SH_MB_PLAYER_* values, NULL when unset */
    char **weapon_slots, **weapon_decls;
    size_t weapon_count;
    char **ext_targets, **ext_partials;           /* extends: "<type>/<name>" (lower case) -> partial decl name */
    size_t ext_count;
} sh_mr_intent;

/* Parse the product block's `registries` (NULL or a JSON object; its
 * "extends") and the author block `extra` (NULL or a JSON object; "player"
 * and "extends", the latter for targets registries does not name). Returns 1
 * with anything found (possibly nothing), 0 when either is not valid JSON. */
int  sh_modregistry_intent(const char *registries, const char *extra, sh_mr_intent *out);

/* The extends table: 1 with the grammar kind (SH_MR_LIST or SH_MR_BAG), its
 * containers ("edit/aiTypeList", ';' between several) and the fixed registry
 * it is (SH_MR_*, or -1) when `target` ("<type folder>/<name>", any case) is
 * on it; else 0 with "<target> is not a list the product extends" in cause. */
int  sh_modregistry_extend_target(const char *target, int *kind, const char **containers, int *fixed,
                                  char *cause, size_t cap);
/* The request of an extends target: generated/decls/<target>.decl, lower case. */
int  sh_modregistry_extend_request(const char *target, char *out, size_t cap);
void sh_modregistry_intent_free(sh_mr_intent *in);
/* 1 when the intent names anything a registry uses. */
int  sh_modregistry_intent_any(const sh_mr_intent *in);

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
    const sh_mr_intent *intent;
    const char *carried[SH_MR_FIXED];   /* its own file at a fixed registry's path, or NULL */
    size_t carried_len[SH_MR_FIXED];
    const char *body_events;   /* the text of the md6Def its player.bodyEvents names, or NULL */
    size_t body_events_len;
    const sh_mr_decl *decls;
    int ndecls;
    const int *weapon_files;   /* per intent weapon: the caller's file id of the named decl, or -1 */
    const char *const *ext_texts;   /* per intent extends: the partial's text, or NULL (not served) */
    const size_t *ext_lens;
} sh_mr_source;

/* The game's text of an extends target other than the fixed registries. */
typedef struct sh_mr_stock { const char *target; const char *text; size_t len; } sh_mr_stock;

typedef struct sh_mr_choice { const char *slot, *key; } sh_mr_choice;

typedef struct sh_mr_entry {
    int source;
    unsigned authored, bit;
    char def[SH_MB_REQUEST_MAX + 1];
    char encounter[SH_MB_REQUEST_MAX + 1];
} sh_mr_entry;

typedef struct sh_mr_rewrite { int id; char *text; size_t len; } sh_mr_rewrite;
typedef struct sh_mr_weapon { char slot[SH_MR_SLOT_MAX]; int source; int file; } sh_mr_weapon;

typedef struct sh_mr_result {
    char *text[SH_MR_FIXED];   /* malloc'd; NULL: nothing to generate, the game's file serves */
    size_t len[SH_MR_FIXED];
    int body_added;            /* bag entries appended to the body */
    int body_merged;           /* clips served once with the union of several sources' rows */
    int body_sources;          /* sources whose events were taken */
    int player_set;            /* player fields set */
    const char *player_value[SH_MB_PLAYER_FIELDS];   /* the value serving per field (points into an intent) */
    int player_from[SH_MB_PLAYER_FIELDS];            /* its source, or -1 */
    sh_mr_entry *entries; int nentries;
    sh_mr_weapon *weapons; int nweapons;
    sh_mr_rewrite *rewrites; int nrewrites;
    char **ext_text; size_t *ext_len;   /* per sh_mr_stock given: the extended text, or NULL (the game's) */
    int next;
    char **lines; int nlines;  /* what was generated and every clash, one line each */
} sh_mr_result;

/* Generate every registry from `n` sources. `stock[k]` is the game's text of
 * fixed registry k (NULL when it cannot be read: that registry then serves
 * the game's own file, with one line). Returns 1 (0 out of memory). */
int  sh_modregistry_generate(const sh_mr_source *src, int n, const char *const stock[SH_MR_FIXED],
                             const size_t stock_len[SH_MR_FIXED], const sh_mr_stock *ext, int next,
                             const sh_mr_choice *choices, int nchoices, sh_mr_result *out);
void sh_modregistry_result_free(sh_mr_result *r);

/* ------------------------------------------------------------ text ------ */

/* The conductor: `stock` with n items appended to edit.aiTypeList (num
 * raised), the rest of the text byte for byte. NULL when the text has no
 * aiTypeList. Malloc'd. */
char *sh_modregistry_conductor_text(const char *stock, size_t len, const char *const *defs, const unsigned *bits,
                                    int n, size_t *out_len);
/* The items of an aiTypeList: fn(aiType, entityDef) per item, in order.
 * Returns the count, -1 when the text has no aiTypeList. */
int  sh_modregistry_conductor_rows(const char *text, size_t len,
                                   void (*fn)(void *context, const char *ai_type, const char *entity_def),
                                   void *context);
/* The player entity: `base` with each non-NULL value set inside the top-level
 * edit block (replaced where the field is present, else added first), the
 * rest byte for byte. Malloc'd, NULL when the text has no edit block. */
char *sh_modregistry_player_text(const char *base, size_t len, const char *const values[SH_MB_PLAYER_FIELDS],
                                 size_t *out_len);
/* The coop entity field a player member sets ("" for bodyEvents). */
const char *sh_modregistry_player_key(int field);
/* `text` with every number `from[i]` inside a memberType or aiType string
 * value written as `to[i]`. Malloc'd; NULL when nothing changed. */
char *sh_modregistry_rewrite_bits(const char *text, size_t len, const unsigned *from, const unsigned *to, int n,
                                  size_t *out_len);
/* 1 when `value` is one of the game's SPAWN_AI_TYPE_* bits (1 .. 1 << 18, 1 << 30). */
int  sh_modregistry_stock_bit(unsigned value);

/* ------------------------------------------------------------ choices --- */

/* 1 when `slot` is "player.hands", "player.handsD1", "player.body",
 * "player.handsAnimWeb" or "weapons/<stock weapon decl name>". */
int  sh_modregistry_slot_valid(const char *slot);
/* The open map's choice for a slot: key NULL or "" removes it. Recorded in the
 * map's manifest (saved with the map) and given to the serving index, which
 * recomputes the map's registries. Returns the bundles whose serving changed
 * (>= 0), or -1 when the slot or key is not valid. The caller applies a
 * change as after any scope change (modui's scope apply). */
int  sh_modregistry_choices_set(const char *slot, const char *key);
/* 1 with the key chosen for `slot`, 0 when the map has no choice for it. */
int  sh_modregistry_choices_get(const char *slot, char *key, size_t cap);
/* The console verb `sh_mods choose "<slot>" "<key>"` (modcmd calls it; an
 * empty key clears): one line for the console in `out`. Returns as
 * sh_modregistry_choices_set. */
int  sh_modregistry_choose_command(const char *slot, const char *key, char *out, size_t cap);

#endif /* SNAPMAP_PLUS_MODREGISTRY_H */
