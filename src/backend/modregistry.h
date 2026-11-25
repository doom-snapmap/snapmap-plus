/* modregistry.h -- derived edits applied per map: the game's text with every declared bundle's recorded edits applied
 * in row order, served as a synthetic file at the game's name, plus the encounter conductor's spawn-type bits. */
#ifndef SNAPMAP_PLUS_MODREGISTRY_H
#define SNAPMAP_PLUS_MODREGISTRY_H

#include <stddef.h>
#include "modbundle.h"

/* A mod that edits a game file carries the edited copy at the game's name; the builder records the structural diff in
 * the product block's "edits" (decl_tree.h) and the copy is never served. The serving index (modserve.c,
 * ms_scope_compute) collects every game file a serving bundle edits for the current listing; identical operations
 * merge, additions union, and a field or keyed entry two bundles set differently is the map's choice (the manifest's
 * "choices", slot "<type>/<decl name>/<path>") else the first in row order, with one line. */

/* The conductor: each aiTypeList item a bundle adds whose aiType holds a number that is not a stock SPAWN_AI_TYPE_*
 * bit is a ported demon and gets its own spawn type per map (the lowest free of 1 << 19 .. 1 << 29, in row order, then
 * encounter decl name). That number is served as the assigned type wherever the bundle wrote it (its encounters,
 * scores and game-file edits); the stock flags beside it say which demon it stands in for, are never served and feed
 * the kill score (sh_modserve_spawn_type_standin). A further demon past the free types shares the type of the first
 * one standing in for the same stock demon. Its single encounter still selects its own actor. */
#define SH_MR_BIT_FIRST 19   /* above SPAWN_AI_TYPE_ZOMBIE_UNWILLING, 1 << 18 */
#define SH_MR_BIT_LAST  29   /* below SPAWN_AI_TYPE_ANY, 1 << 30 */

#define SH_MR_CLASH_SOURCES 8

/* The AI Filter's "Include AI" flag picker (snappropertyinspector_bitflag): with `grown` given (the editor) it offers
 * every ported demon the map gives a type, as the spawnAIType_t member of its bit (sh_modregistry_flag_name: one
 * member per free bit added to the game's enum, typeinfo.c), labelled with its encounter's strings. The widget offers,
 * reads and writes only members of the enum by name (13954591, both renderers): a value naming no member gets no
 * checkbox, a saved word naming none is ignored, and a bit with no member is not written. */
#define SH_MR_FLAG_PICKER "generated/decls/snappropertyinspector_bitflag/spawnaitypebitflaginspector.decl"
/* A pinned type's choice slot: SH_MR_PIN "<bit>/<encounter decl name>", the mod's salt its key. */
#define SH_MR_PIN "spawnAIType_t/"
/* The spawnAIType_t member the product adds for a free bit, "SPAWN_AI_TYPE_MOD_<bit>" (19 .. 29), in buf. Returns
 * buf, or NULL when `bit` is not one of the free bits. */
const char *sh_modregistry_flag_name(unsigned bit, char *buf, size_t cap);

/* The editor's encounter pickers an installed mod's single-demon encounters grow, after the game's: the Single Demon
 * Encounter, the Encounter List and the Miniboss event's boss list (snappropertyinspector_whitelistencounterdecl[list]).
 * A grown list never declares a mod (modserve.c). */
#define SH_MR_GROWN_LIST_COUNT 3
extern const char *const SH_MR_GROWN_LISTS[SH_MR_GROWN_LIST_COUNT];

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
    const char *key;           /* its salt */
    const char *who;           /* the mod's name, for lines */
    const char *edits;         /* its product block's "edits" (JSON array), or NULL */
    size_t edits_len;
    const sh_mr_decl *decls;   /* its encounters and scores (the conductor's bit rewrite) */
    int ndecls;
    int (*entity_unavailable)(void *context, const char *name);
    void (*single_actor)(void *context, int encounter, const char *actor);
    void *context;
} sh_mr_source;

/* One game file the sources edit, with the game's text (NULL when it cannot
 * be read: the game's own file then serves, with one line). */
typedef struct sh_mr_target { const char *request; const char *text; size_t len; } sh_mr_target;

typedef struct sh_mr_choice { const char *slot, *key; } sh_mr_choice;

typedef struct sh_mr_entry {
    int source;
    unsigned authored, bit;
    unsigned standin;          /* the stock spawn-type bits the item names beside its number, 0 for none */
    int shared;                /* 1 when no free type is left and it serves another entry's bit */
    int pinned;                /* 1 when the map's pin gave it its bit */
    char *def;                 /* malloc'd */
    char *encounter;           /* malloc'd, NULL for none */
} sh_mr_entry;

typedef struct sh_mr_single { int file; const char *actor; } sh_mr_single;
typedef struct sh_mr_rewrite { int id; char *text; size_t len; } sh_mr_rewrite;

/* A slot two or more sources set to different values. */
typedef struct sh_mr_clash {
    char *slot;                /* malloc'd */
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
    sh_mr_single *singles; int nsingles;  /* exact actors of single encounters sharing a class */
    sh_mr_clash *clashes; int nclashes;
    char **lines; int nlines;  /* what is generated and every clash, one line each */
} sh_mr_result;

/* Generate every target from `n` sources; a target in SH_MR_GROWN_LISTS also offers `grown[0..ngrown)` (encounter
 * decl names), and with `grown` given (the editor) SH_MR_FLAG_PICKER offers every entry's type. `choices` carry the
 * map's pins (SH_MR_PIN). Returns 1 (0 out of memory). */
int  sh_modregistry_generate(const sh_mr_source *src, int n, const sh_mr_target *targets, int ntargets,
                             const sh_mr_choice *choices, int nchoices, const char *const *grown, int ngrown,
                             sh_mr_result *out);
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
/* The value of a SPAWN_AI_TYPE_* flag name (`n` characters, any case), with 1;
 * 0 for another word. Values of the game's spawnAIType_t (13954591). */
int  sh_modregistry_spawn_flag(const char *word, size_t n, unsigned *bit);
/* A spawn-type value (a memberType or aiType string without its quotes, `n` characters): *own the first number that
 * is not a stock bit (0 for none), *standin the stock bits its other words name. Returns 1 when it holds such a
 * number. */
int  sh_modregistry_type_words(const char *value, size_t n, unsigned *own, unsigned *standin);
/* An edits JSON array (decl_tree.h) with every number `from[i]` inside the memberType or aiType value of an
 * operation's statement written as `to[i]` (the statements are JSON strings: their quotes read \"). Malloc'd; NULL
 * when nothing changed. */
char *sh_modregistry_rewrite_edits(const char *edits, size_t len, const unsigned *from, const unsigned *to, int n,
                                   size_t *out_len);
/* `text` with `names[0..n)` added to the list block `list` directly inside its `edit` block (`item[i] = "name";`, num
 * raised), each name the list does not already hold (any case). Malloc'd; NULL when nothing was added or the text has
 * no such list. */
char *sh_modregistry_append_list(const char *text, size_t len, const char *list, const char *const *names, int n,
                                 size_t *out_len);
/* One value of a bitflag inspector: its enum member's name and its two string ids ("" or NULL leaves one out). */
typedef struct sh_mr_flag { const char *value, *label, *description; } sh_mr_flag;
/* `text` with `flags[0..n)` added to the `values` list directly inside its `edit` block (`item[i] = { enumValue = ...;
 * label = ...; description = ...; }`, num raised), each value the list does not already hold. Malloc'd; NULL when
 * nothing was added or the text has no such list. */
char *sh_modregistry_append_flags(const char *text, size_t len, const sh_mr_flag *flags, int n, size_t *out_len);
/* The string of `key` (a scalar `key = "..."`) anywhere in a decl text, in
 * buf without its quotes; 0 when the text sets none. */
int  sh_modregistry_decl_string(const char *text, size_t len, const char *key, char *buf, size_t cap);
/* The encounter a Toybox placeable spawns: an entityDef text of class idPlaceableSnapAIEncounter, its
 * encounterToSpawn in buf. 0 for any other entityDef (the editor's pickers grow with these encounters only). */
int  sh_modregistry_placeable_encounter(const char *text, size_t len, char *buf, size_t cap);

/* ------------------------------------------------------------ choices --- */

/* 1 when `slot` reads as "<type>/<decl name>/<path>": printable, no blank,
 * quote or backslash, at least three '/'-separated parts (any length). */
int  sh_modregistry_slot_valid(const char *slot);
/* The open map's choice for a slot (key NULL or "" removes it), recorded in the map's manifest and given to the
 * serving index, which recomputes the map's generated files. Returns the bundles whose serving changed (>= 0), or -1
 * when the slot or key is not valid; the caller applies a change as after any scope change (modui's scope apply). */
int  sh_modregistry_choices_set(const char *slot, const char *key);
/* 1 with the key chosen for `slot`, 0 when the map has no choice for it. */
int  sh_modregistry_choices_get(const char *slot, char *key, size_t cap);
/* The console verb `sh_mods choose "<game file>/<path>" "<mod name>"` (an empty name clears): the mod is one the open
 * map uses, named by its name, its folder or its salt (sh_modserve_listed_mod). One console line in *out (malloc'd);
 * returns as sh_modregistry_choices_set. */
int  sh_modregistry_choose_command(const char *slot, const char *mod, char **out);

#endif /* SNAPMAP_PLUS_MODREGISTRY_H */
