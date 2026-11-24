/* modbundle.h -- build, validate and read mod bundles (mod.json): hash every file, name what each provides and
 * needs, classify it, and produce the canonical manifest whose SHA-256 is the build hash. mod.json is the
 * author's part plus the builder's "snapmap-plus" block, written only by the product; identity is the salt
 * (modsalt.h), the namespace mods/<salt>/. The canonical manifest is UTF-8 JSON with sorted keys and no
 * whitespace, derived in memory and never stored. Pure file and text work: no engine calls; installed content
 * is reached through the registered stock ops (modstock.c). */
#ifndef SNAPMAP_PLUS_MODBUNDLE_H
#define SNAPMAP_PLUS_MODBUNDLE_H

#include <stddef.h>
#include <stdint.h>

#define SH_MB_FORMAT          2
/* The builder's rules number, written into the block as "rules"; a folder whose block carries another number is
 * built again once, so a product update reclassifies every own folder. Raise it when a build's classification,
 * edits or manifest change. */
#define SH_MB_RULES           15

/* The refusal of a folder in an earlier shape, one sentence; SH_MB_SHAPE is its first part, which readers of a
 * cause match on. */
#define SH_MB_SHAPE           "This mod was made for an earlier version of Snapmap+. "
#define SH_MB_EARLIER         SH_MB_SHAPE "This version does not read it yet."
#define SH_MB_ID_MAX          64u
/* A mod has no count or size limit: each file, path and text is heap-sized, bounded only by the platform
 * (Windows paths, wide_path.h; ZIP entry paths, modzip.h). */
#define SH_MB_HASH_HEX        64u
/* mod.json "requirements" (sh_mb_requirements_canonical): any cvar or console line, in any number. */
#define SH_MB_BLOCK_KEY       "snapmap-plus"   /* mod.json's builder block */

typedef struct sh_mb_need {
    char *type;            /* engine spelling: md6Def, baseModel, anim, material, ... */
    char *name;            /* logical name as the decl text spells it */
} sh_mb_need;

typedef struct sh_mb_file {
    char *path;            /* the file's path in the mod folder, lower case with '/' (a loaded block's as written) */
    char *request;         /* the engine's request path the path stands for (sh_ma_request), lower-cased */
    char  blob[SH_MB_HASH_HEX + 1];
    unsigned long long size;
    char *ptype;           /* provides.type */
    char *pname;           /* provides.name */
    sh_mb_need *needs;
    size_t need_count, need_cap;
    int is_decl;           /* under generated/decls/ */
    int is_module;         /* a snapModule decl: maps/modules/<path>.decl */
    int shadowed;          /* request path listed in shadows */
    int stock;             /* a stock pointer: blob is the installed bytes' SHA-256, nothing is carried */
    char *stock_source;    /* the installed resource path a stock pointer names */
    int deferred;          /* builder: 1 type not allowed, 2 over 64 MiB, refused unless it is a pointer */
    int absent;            /* a stock pointer with no file in the folder (a closure pointer, or a received build's) */
    int cls;               /* SH_MB_CLASS_*: what the file is to the game (sh_mb_classify) */
    char *edit;            /* builder: a decl at a game name, its derived edits element (JSON), or NULL */
    int edited;            /* an edited copy: a REPLACE file the bundle's edits name (built or loaded); never served */
    int closure;           /* builder: campaign-only content a served decl needs, derived by this build ("closure": true) */
    char **events;         /* builder: DOOM's sound events the text names, for the stock-bank question (registries "banks") */
    size_t event_count;
} sh_mb_file;

/* Per-file classification, decided from the file's path and bytes (sh_mb_classify). */
#define SH_MB_CLASS_NONE    0
#define SH_MB_CLASS_ADD     1   /* under the mod's namespace, bytes the game does not hold */
#define SH_MB_CLASS_BORROW  2   /* under the namespace, a stock pointer (the game's own bytes) */
#define SH_MB_CLASS_REPLACE 3   /* at a game name: a decl is an edited copy, never served; a binary is served to maps using it */
#define SH_MB_CLASS_EDITOR  4   /* an editor-only type outside the namespace: a local tweak */
#define SH_MB_CLASS_SURFACE 5   /* a stock pointer restoring installed content at its own name, served from the player's disk */

/* edits: the edited copies (REPLACE files the bundle's edits name), counted
 * apart from replaces, which is only what a received folder replaces. */
typedef struct sh_mb_classes {
    unsigned adds, borrows, replaces, editor, surfaces, edits;
} sh_mb_classes;

/* Where each need of a build was found. */
typedef struct sh_mb_need_stats {
    unsigned total, local, installed, stock, unresolved;
} sh_mb_need_stats;

typedef struct sh_mb_bundle {
    char  id[SH_MB_ID_MAX + 1];             /* the salt (modsalt.h): the namespace and the canonical "id" */
    char *name, *author, *homepage;
    char *requirements;    /* canonical JSON object text, or NULL */
    char *extra;           /* canonical JSON object of preserved author keys, or NULL */
    char **shadows;        /* sorted, unique, lower-cased request paths */
    size_t shadow_count;
    sh_mb_file *files;     /* sorted by path */
    size_t file_count, file_cap;
    unsigned long long total_size;   /* carried bytes: stock pointers are not counted */
    unsigned stock_count;  /* files that are stock pointers */
    unsigned long long stock_bytes;  /* their installed size (known when built, 0 when loaded) */
    char *canonical;       /* canonical manifest text (NUL-terminated) */
    size_t canonical_len;
    char  hash[SH_MB_HASH_HEX + 1];   /* SHA-256 of canonical: the build hash */
    char  prev_hash[SH_MB_HASH_HEX + 1];   /* sh_mb_build: the build hash the block named before this build, "" when none */
    char  tree[SH_MB_HASH_HEX + 1];   /* block "tree": the folder's tree hash when built, "" when absent */
    /* block "units_of": a composition's units (modunit.h sh_unit_compose), the unit hashes whose files the folder
     * holds; NULL for a mod folder. A build keeps what the folder's block named. */
    char **units_of;
    size_t units_of_count;
    long long built;       /* block "built": the first successful build of this salt in this folder, 0 when none */
    int   validated;       /* 1 when the block records no "cause" (a build without errors) */
    int   rules;           /* block "rules": the builder's SH_MB_RULES that wrote it; 0 when absent */
    char *state_cause;     /* block "cause", or NULL (sh_mb_state_cause) */
    sh_mb_need_stats needs;
    unsigned errors, warnings;
    char *first_cause;     /* the first error, or NULL (sh_mb_first_cause) */
    char *first_warning;   /* the first warning, or NULL (sh_mb_first_warning) */
    /* Every warning of the build in order (`warnings` counts them). */
    char **warning_list;
    size_t warning_list_count, warning_list_cap;
    sh_mb_classes classes; /* counts of the files' classes */
    char *registries;      /* compact JSON of the block's "registries" object, or NULL */
    char *scripts;         /* compact JSON of the block's "scripts" object, or NULL (no scripts) */
    unsigned script_files; /* files of kind script (sh_mb_script_request) */
    /* Derived edits: the block's "edits", a JSON array with one element per decl the mod carries at a game name
     * with other bytes (decl_tree.h), or NULL; edit_files elements. */
    char *edits;
    unsigned edit_files;
    char **notes;          /* builder: report lines (replacements, closure, edits) */
    size_t note_count, note_cap;
    /* builder: the mod's own strings keys as the author writes them (sh_ma_string_keys), sorted; a decl's
     * #str_<key> of one of them is the mod's string */
    char **own_keys;
    size_t own_key_count;
    /* The namespace the carried binaries spell (a converter's mesh naming its skeleton mods/<salt>/...), ""
     * when none: the block's "spelled". When it is not the mod's salt, the serving index maps it to the salt in
     * every file it serves, length for length. */
    char spelled[SH_MB_ID_MAX + 1];
} sh_mb_bundle;

/* Whether two decl texts are one text: CRLF and LF compare equal and the trailing run of line ends is ignored.
 * The builder compares a decl-grammar copy at a game name with the game's text this way; a binary keeps byte
 * identity. */
int sh_mb_text_same(const unsigned char *a, size_t an, const unsigned char *b, size_t bn);

/* Existence of one lower-cased request path: 1 present, 0 absent, -1 cannot
 * tell (reader unavailable). */
typedef int (*sh_mb_exists_fn)(void *context, const char *request);

typedef struct sh_mb_resolver {
    sh_mb_exists_fn installed;   /* other installed bundles; may be NULL */
    void *installed_context;
    sh_mb_exists_fn stock;       /* the player's installed game archives; may be NULL */
    void *stock_context;
} sh_mb_resolver;

/* Results of the stock operations. */
#define SH_MB_STOCK_UNAVAILABLE (-1)   /* the installed-content reader is not available */
#define SH_MB_STOCK_ABSENT      0      /* the game has no such entry */
#define SH_MB_STOCK_OK          1
#define SH_MB_STOCK_MISMATCH    2      /* the entry exists with other bytes */

typedef struct sh_mb_stock_ops {
    void *context;
    /* An installed entry whose bytes are `size` bytes hashing to `sha256`,
     * preferring `request` itself: SH_MB_STOCK_OK with its path in `source`,
     * ABSENT or UNAVAILABLE. */
    int (*match)(void *context, const char *request, unsigned long long size, const char *sha256,
                 char **source);
    /* Hash the installed entry `source`: OK with sha256 and size, ABSENT or UNAVAILABLE. */
    int (*identify)(void *context, const char *source, char sha256[SH_MB_HASH_HEX + 1],
                    unsigned long long *size);
    /* Whether `source` holds bytes hashing to `sha256`: OK (size set), MISMATCH,
     * ABSENT or UNAVAILABLE. */
    int (*verify)(void *context, const char *source, const char *sha256, unsigned long long *size);
    /* Read those bytes into a process-heap buffer (HeapAlloc on GetProcessHeap;
     * the caller frees it with HeapFree): OK, MISMATCH, ABSENT or UNAVAILABLE. */
    int (*read)(void *context, const char *source, const char *sha256, unsigned char **bytes, size_t *len);
    /* Which part of the install holds `source` (with those bytes when sha256
     * is not NULL): SH_MB_STOCK_OK with "snap_gameresources", "gameresources"
     * or "loose" in `out`, ABSENT or UNAVAILABLE. May be NULL. */
    int (*container)(void *context, const char *source, const char *sha256, char *out, size_t cap);
    /* Every installed archive path starting with `prefix` (lower case), passed to `fn` (a path several rows
     * hold may come more than once): OK or UNAVAILABLE. May be NULL. */
    int (*list)(void *context, const char *prefix, void (*fn)(void *fn_context, const char *path), void *fn_context);
} sh_mb_stock_ops;

/* Register the installed-content reader (copied; NULL clears). Without one the builder emits no pointers, and
 * stock files cannot be verified or served. */
void sh_mb_set_stock_ops(const sh_mb_stock_ops *ops);
/* 1 when a reader is registered. */
int  sh_mb_stock_available(void);

/* The stock-bank question (modbankinfo.h sh_mbi_bundle_need): 1 with the stock Wwise bank a sound decl
 * `decl_name` needs loaded on demand, else 0. With one registered, a build lists each such bank once in
 * registries "banks" and notes which decl needs it; NULL (the default) lists none. */
typedef int (*sh_mb_bank_need_fn)(const char *decl_name, char *bank, size_t cap);
void sh_mb_set_bank_need(sh_mb_bank_need_fn fn);
/* The registered operations, with a first cause naming `path` when not OK. */
int  sh_mb_stock_verify(const char *source, const char *sha256, unsigned long long *size,
                        const char *path, char **cause);
/* The registered identify operation: OK with the installed entry's sha256 and size, ABSENT or UNAVAILABLE. */
int  sh_mb_stock_identify(const char *source, char sha256[SH_MB_HASH_HEX + 1], unsigned long long *size);
int  sh_mb_stock_read(const char *source, const char *sha256, unsigned char **bytes, size_t *len,
                      const char *path, char **cause);
/* The registered container operation: 1 with the archive (or "loose") in `out`; 0 when no reader answers. */
int  sh_mb_stock_container(const char *source, const char *sha256, char *out, size_t cap);

/* Validate a stock source path: the request-path rules without the extension
 * whitelist. 1 when valid. */
int  sh_mb_check_source(const char *source, char **cause);

/* Build the bundle for `folder` (UTF-8 path): 1 when it has no errors, else 0 with out->first_cause and
 * out->errors. out is always initialised and released with sh_mb_free; nothing is written to disk. `salt` (NULL
 * to read the block's) gives one to a folder whose block has none; a folder in an earlier shape or without a
 * salt is refused with SH_MB_EARLIER. A file outside the namespace that is not a surface must be a game name
 * (resolver->stock): an editor tweak, or a replacement whose derived edits are recorded (out->edits) and a
 * binary is refused. */
int  sh_mb_build(const char *folder, const char *salt, const sh_mb_resolver *resolver, sh_mb_bundle *out);

/* A bundle's sentences, never NULL: the first error ("" when none), the
 * first warning and the block's cause. */
const char *sh_mb_first_cause(const sh_mb_bundle *bundle);
const char *sh_mb_first_warning(const sh_mb_bundle *bundle);
const char *sh_mb_state_cause(const sh_mb_bundle *bundle);
/* Replace the bundle's first cause (or block cause) with a copy of `text`
 * (NULL clears it); 0 when memory runs out. */
int  sh_mb_set_first_cause(sh_mb_bundle *bundle, const char *text);
int  sh_mb_set_state_cause(sh_mb_bundle *bundle, const char *text);

/* Write `<folder>\mod.json`: the author's part (the folder's mod.json without its block, else synthesized) and
 * the builder block from `bundle`, including its cause when not validated. Write-through temporary and rename;
 * refuses a bundle with errors. Returns 1 on success. */
int  sh_mb_write(const sh_mb_bundle *bundle, const char *folder, char **cause);

/* Write the block of a failed build into `<folder>\mod.json`: format, salt, rules, built, tree and cause only,
 * so the folder serves nothing until fixed. bundle->id must be a salt. Returns 1 on success. */
int  sh_mb_write_failed(const sh_mb_bundle *bundle, const char *folder, char **why);

/* Record a verification result in `<folder>\mod.json`'s block (a cause, or
 * none) without changing anything else. Returns 1 on success. */
int  sh_mb_set_state(const char *folder, int validated, const char *cause, char **why);

/* The complete mod.json text for `bundle` (malloc'd), splicing the builder
 * block into `author_text` (a mod.json text, its own block ignored) or, when
 * author_text is NULL, into a synthesized author part. */
char *sh_mb_modjson_text(const sh_mb_bundle *bundle, const char *author_text, size_t author_len, size_t *out_len);

/* The author's bytes of a mod.json text: the text without its "snapmap-plus"
 * member (malloc'd). *had_block (optional) says whether there was one.
 * NULL when the text is not a JSON object. */
char *sh_mb_author_bytes(const char *text, size_t len, size_t *out_len, int *had_block);

/* Read and check a built mod.json without reading files: structure, salt, paths, logical names and the build
 * hash recomputed from the canonical form. Returns 1 when valid; a block without "build" fails with "not built:
 * <cause>", an earlier shape with SH_MB_EARLIER, and a "cause" beside "build" loads with validated 0. */
int  sh_mb_load(const char *mod_json_path, sh_mb_bundle *out);
int  sh_mb_load_text(const char *text, size_t len, sh_mb_bundle *out);
/* `<folder>\mod.json`. */
int  sh_mb_load_folder(const char *folder, sh_mb_bundle *out);

/* Tree hash of a mod folder, the rebuild rule's key (the block's "tree"): SHA-256 over the author's bytes of
 * mod.json, then every file under assets/ sorted by path with its size and last-write time; no file is read.
 * Returns 1 and the hex. */
int  sh_mb_tree_hash(const char *folder, char out[SH_MB_HASH_HEX + 1]);

/* <mods_root>\snapmap-plus.json, the product's one file under mods\: {"format": 1, "claims", "links",
 * "pending", "places", "salts", ...}, each section owned by one module and read and written whole. A write is a
 * read, a replace of that section and a write-through rename under the mods folder's named lock
 * (sh_mb_record_lock; store_lock.h), which a caller may also hold around its own read-modify-write. A file
 * another program holds is waited for up to a second; a file that is not JSON is moved aside (logged) and a new
 * one started, and one that cannot be read is never replaced. */
#define SH_MB_RECORD_FILE "snapmap-plus.json"
/* Recursive per thread; sh_mb_record_unlock releases this thread's hold. */
void sh_mb_record_lock(const char *mods_root);
void sh_mb_record_unlock(void);
/* The mods folder's named lock (store_lock.h), opened once per folder and kept (a Win32 HANDLE; NULL when it
 * cannot be opened). sh_mb_store_wait takes it, logging one "MODS: store: waiting" line when another holds it;
 * the caller releases it with ReleaseMutex. sh_mb_store_waiting and sh_mb_store_waits count the waiting threads
 * and the waits so far. */
void    *sh_mb_store_mutex(const char *mods_root);
void     sh_mb_store_wait(void *mutex);
long     sh_mb_store_waiting(void);
unsigned sh_mb_store_waits(void);


/* 1 with the section's JSON text (malloc'd, compact), 0 when absent, -1 when the record cannot be read; a
 * "MODS: record:" line names the file, and that section is not written until a get of it succeeds again. */
int  sh_mb_record_get(const char *mods_root, const char *section, char **json_out);
/* Replace (json != NULL) or remove (NULL) one section. 1 on success; 0 with a
 * "MODS: record:" line naming the file when the record cannot be read or the
 * section's last get failed. */
int  sh_mb_record_put(const char *mods_root, const char *section, const char *json);
/* The player-facing sentence for the last record read or write this thread could not make: the file's full
 * path, why, and the next step. A flow that writes the record clears it at start and adds it to its result when
 * sh_mb_record_why returns non-NULL. */
void sh_mb_record_why_clear(void);
const char *sh_mb_record_why(void);

void sh_mb_free(sh_mb_bundle *bundle);


/* The state of a folder's descriptor (sh_mb_descriptor_state). */
#define SH_MB_DESC_FRESH       1   /* mod.json with the author's part only */
#define SH_MB_DESC_BUILT       2   /* a format-2 block with a valid salt (built, failed, or salted only) */
#define SH_MB_DESC_EARLIER     3   /* package.json, or a block of an earlier format */
#define SH_MB_DESC_RETIRED_KEY 4   /* an author key this version does not read */
#define SH_MB_DESC_UNREADABLE  5   /* no mod.json, not a JSON object, or a format-2 block without a salt */
int  sh_mb_descriptor_state(const char *folder);
/* The state with the refusal sentence (SH_MB_EARLIER) for EARLIER, RETIRED_KEY and UNREADABLE; "" for FRESH and
 * BUILT. */
int  sh_mb_descriptor_state_why(const char *folder, char **cause);

/* Every carried file and provided name under some mods/<x>/ has x equal to
 * the bundle's salt (any case; a salt has one spelling). 1 when so; else 0
 * with the sentence naming the first file under another mod's salt. */
int  sh_mb_salt_paths_check(const sh_mb_bundle *bundle, char **cause);

/* Rewrites every reference to mods/<from> in `text` (n bytes), as written or in the engine's cooked spelling,
 * any case, not inside a longer name, to mods/<to>. Returns the count; with `out` the rewritten text (malloc'd,
 * NULL when the count is 0) and its length, (size_t)-1 when out of memory; without `out` it only counts. */
size_t sh_mb_rekey_text(const char *text, size_t n, const char *from, const char *to, char **out, size_t *out_len);

/* `rel` ('/'-separated) with every folder segment right after a "mods" segment that is `from` (as written or
 * cooked, any case) replaced by `to`. 1 with the new path in *out (malloc'd) when changed, 0 when not, -1 when
 * memory runs out. */
int  sh_mb_rekey_path(const char *rel, const char *from, const char *to, char **out);

/* A mod.json text with its builder block replaced by {"format": 2, "salt": <salt>} and every reference to
 * mods/<from>/ in the author's part rewritten to mods/<salt>/ (from NULL: no rewrite); the other author bytes
 * are kept. malloc'd, or NULL when not a JSON object or salt is not a salt. */
char *sh_mb_salt_modjson(const char *text, size_t n, const char *from, const char *salt, size_t *out_len);

/* sh_mb_salt_strings returns the mod.json text with every "strings".<language> key lacking the "<salt>_" prefix
 * renamed "<salt>_<key>" (a leading "mods/<salt>/" or a copy's "<from>_" dropped; characters outside
 * [A-Za-z0-9_] made '_'); *keys (free with sh_mb_strkeys_free) lists each renamed key once. NULL when the text
 * is not a JSON object or salt is not a salt. */
typedef struct sh_mb_strkey { char *from, *to; } sh_mb_strkey;
char  *sh_mb_salt_strings(const char *text, size_t n, const char *from, const char *salt, sh_mb_strkey **keys, size_t *count,
                          size_t *out_len);
/* Rewrites every "#str_<from>" reference (any case, ending at a name boundary) in a text to "#str_<to>": the
 * count, and with `out` the rewritten text (malloc'd, NULL when the count is 0); (size_t)-1 when out of memory. */
size_t sh_mb_restr_text(const char *text, size_t n, const sh_mb_strkey *keys, size_t count, char **out, size_t *out_len);
void   sh_mb_strkeys_free(sh_mb_strkey *keys, size_t count);

/* The classes counts as one line ("2 adds, 0 borrows, 1 edits, 0 replaces, 0 editor, 0 surfaces"). */
void sh_mb_classes_text(const sh_mb_classes *c, char *out, size_t cap);



/* Validate one request path (relative, '/', no "." or ".." segment, characters [A-Za-z0-9._-$+@~()] and
 * internal spaces, no segment starting with a space or ending in '.' or a space; any length). 1 when valid,
 * else 0 with the cause in *cause (malloc'd). */
int  sh_mb_check_request(const char *request, char **cause);

/* Extension whitelist. A .bswf movie is admitted only under
 * generated/swf/. */
int  sh_mb_extension_allowed(const char *request);

/* A script is a carried text file of kind "script": assets/mods/<salt>/scripts/<name>.lua (an entry file run at
 * map start) or scripts/lib/<name>.lua (loaded only by `require`), <name> of [A-Za-z0-9_-]. The engine never
 * opens one; the runtime reads built bytes through the serving index (modserve.h sh_modserve_script_files). */

/* Scripts are checked at build: UTF-8 text without NUL, not compiled Lua, and compiled by the registered check;
 * a literal require of a module the mod does not carry, or a name of another mod's namespace, is refused with
 * the file and line. The block records what the scripts bind ("scripts", sh_mb_script_scan). */

/* What `request` (lower-cased, below assets/) is to scripts: SH_MB_SCRIPT_ENTRY or SH_MB_SCRIPT_LIB for a
 * script path (`*name` and `*name_len` get its <name>; either may be NULL), SH_MB_SCRIPT_MISPLACED for any
 * other .lua (refused at build with the sentence of sh_mb_script_where), else 0. Pure. */
#define SH_MB_SCRIPT_ENTRY      1
#define SH_MB_SCRIPT_LIB        2
#define SH_MB_SCRIPT_MISPLACED  (-1)
/* An author file at one of the two paths the mod's Script node row is served
 * at (sh_script_node_reserved) is refused: the product makes that row. */
#define SH_MB_SCRIPT_NODE_RESERVED \
    "Snapmap+ makes this mod's Script node at this path, so the file cannot be here; remove it or give it another " \
    "name"
int  sh_mb_script_request(const char *request, const char **name, size_t *name_len);

/* The build's sentence for a misplaced script `child` (its path in the mod folder, "assets/..."), in the
 * author's words: a file under the mod's own folder is named under the folder's name, never the salt, and the
 * sentence names where scripts go. */
char *sh_mb_script_where(const char *child, const char *salt, const char *folder);

/* The author's spelling of a script request: "scripts\<name>.lua" or
 * "scripts\lib\<name>.lua", the name every script message uses (malloc'd). */
char *sh_mb_script_file_name(const char *request);

/* The compile check the product registers (script_compile.c): `file` is the author's spelling, `entry` 1 for an
 * entry file (which must start with "api = 1"), 0 for a scripts\lib file. 1 when the text compiles (nothing
 * runs); 0 with *line and the plain what and todo (malloc'd, no final period). Any thread; without a registered
 * check a build that carries a script fails. */
typedef int (*sh_mb_script_check_fn)(const char *file, int entry, const char *text, size_t len, int *line,
                                     char **what, char **todo);
void sh_mb_set_script_check(sh_mb_script_check_fn fn);

/* The check of the author's content blocks the game reads later ("scenes", "hud"; modcontent.h
 * sh_modcontent_check): `extra` is the build's preserved author keys. 0 with one sentence in *cause (malloc'd)
 * fails the build; NULL (the default) checks nothing. */
typedef int (*sh_mb_extra_check_fn)(const char *salt, const char *extra, size_t len, char **cause);
void sh_mb_set_extra_check(sh_mb_extra_check_fn fn);

/* The registries "clipEvents" of a build (clip_events.h sh_ce_build_registry), from the author's
 * "animation_events" names, the mod's salt and a reader of its md6Def texts by decl name (1 text malloc'd, 0
 * not one, -1 unreadable). Each entry or clip it leaves out is one warning through `warn`. Returns a JSON array
 * (malloc'd) or NULL (*oom on memory failure); NULL (the default) records nothing. */
typedef int (*sh_mb_text_fn)(void *context, const char *name, char **text, size_t *len);
typedef char *(*sh_mb_clip_events_fn)(const char *salt, const char *const *names, int n, sh_mb_text_fn text,
                                      void *text_ctx, void (*warn)(void *context, const char *line), void *warn_ctx,
                                      int *oom);
void sh_mb_set_clip_events(sh_mb_clip_events_fn fn);

/* The registries "attachmentTags" of a build (attach_tags.h, sh_at_build_registry): the same shape as
 * "clipEvents" above, from the author's "attachment_tags" names; props in place of clips. NULL (the default)
 * records nothing for "attachment_tags". */
void sh_mb_set_attach_tags(sh_mb_clip_events_fn fn);


/* The static registration pass over one script's text, without running it (Lua comments and strings skipped): a
 * literal call of on.<event>("<name>", ...) is BIND, on.input("<node>", ...) INPUT, node("<node>") OUTPUT and
 * require("<module>") REQUIRE, with SH_MB_SCRIPT_DYNAMIC or'ed in (name NULL) when the argument is not a string
 * literal. `event` is the word after "on." ("input" for inputs, "" otherwise), `line` 1-based; on.map_start and
 * on.map_end are not reported. Pure. */
#define SH_MB_SCRIPT_BIND     1
#define SH_MB_SCRIPT_INPUT    2
#define SH_MB_SCRIPT_OUTPUT   3
#define SH_MB_SCRIPT_REQUIRE  4
#define SH_MB_SCRIPT_DYNAMIC  0x100
typedef void (*sh_mb_script_call_fn)(void *context, int what, const char *event, const char *name, size_t name_len,
                                     int line);
void sh_mb_script_scan(const char *text, size_t len, sh_mb_script_call_fn fn, void *context);

/* Every string literal of a script's code, in order, without running it: "..." and '...' with escapes decoded,
 * and long strings; comments are skipped. `fn` returns 0 to stop. Returns 0 when memory ran out, else 1. Pure. */
typedef int (*sh_mb_script_literal_fn)(void *context, const char *s, size_t len);
int  sh_mb_script_literals(const char *text, size_t len, sh_mb_script_literal_fn fn, void *context);

/* 1 when the `len` bytes at `s` are a name `require` loads: a file scripts\lib\<name>.lua of the mod, <name> of
 * [A-Za-z0-9_-] (sh_mb_script_request; the runtime's require takes no folder, dot or extension, script_lib_base.c
 * sb_require). An entry script is never required: it runs for the entity it binds. Pure. */
int  sh_mb_script_module_name(const char *s, size_t len);

/* Kinds a bundle admits only as stock pointers, never as carried bytes: .sbsp, .ambientsh, .tome and .pages
 * (per-map engine files too large to carry). 1 when `request` is one of them. */
int  sh_mb_pointer_only_kind(const char *request);

/* 1 when a stock pointer from `request` to `source` restores installed content under the game's own name:
 * request == source, or request is generated/decls/<type>/<source>.decl for an own-path decl source (a material
 * such as maps/.../mega.decl). */
int  sh_mb_stock_restore(const char *request, const char *source);

/* The name root of everything the product derives itself (modprog.h SH_MODPROG_CONTAINER): the platform programs
 * and their render parms, the looks' images, the Graphics SDK's derived decls and effect emitters. No game file is
 * named under it. */
#define SH_MB_PRODUCT_ROOT "snapmapplus"

/* 1 when `request` is a decl the product derives: generated/decls/<type>/snapmapplus/<...>.decl or
 * generated/decls/<type>/snapmapplus_<...>.decl (a derived table). A stock pointer to one restores a name no
 * archive lists, so the serving layer registers it in every pass like a mod's own decls (modserve.c
 * ms_plannable). */
int  sh_mb_product_decl(const char *request);

/* 1 when `request` is a file the product derives from a mod's own file and names by the SHA-256 of its bytes: a
 * look's image, generated/image/snapmapplus/look/[c]<16 hex>.bimage, or a Graphics SDK decl,
 * generated/decls/<type>/snapmapplus/sdk/<16 hex>.decl or generated/decls/<type>/snapmapplus_sdk_<16 hex>.decl
 * (modprog.h). Its bytes exist only once a material, effect or environment naming it has compiled in the
 * process, so the provider serves one registered with sh_modserve_set_product_reader, and a pointer to one is
 * derived by compiling the files that need it (modserve.c ms_derive; modcache.c mc_product_ok). */
int  sh_mb_product_hashed(const char *request);

/* For a snapModule decl request maps/modules/<path>.decl, the snapModuleInfo
 * request it needs (generated/decls/snapmoduleinfo/<path>.decl), malloc'd;
 * NULL when the request is not a module decl. */
char *sh_mb_module_info_request(const char *request);

/* Editor-time content: a request the editor reads and the game never does at play (snapEditor* and
 * snapPropertyInspector* decl types, palette decls, editor movies). It is no dependency of a map (modserve.c's
 * save closure skips it), so a bundle of only such files serves nothing in a played map. */
int  sh_mb_editor_time(const char *request);

/* 1 when every file of a loaded bundle is editor-time content and it asks no
 * requirements: nothing in it changes play. 0 for an empty bundle. */
int  sh_mb_editor_only(const sh_mb_bundle *bundle);

/* A model reference as an entity's edit state or a decl writes it (models/.../x.lwo, .ase, .ma, .obj, .fbx or a
 * .bmodel path) as the cooked request the provider is asked for (cooked/model/models/.../x.bmodel,
 * lower-cased), malloc'd; NULL when `name` is not a model reference. */
char *sh_mb_model_key(const char *name);
/* Every request a model reference may be served under, in resolve order: cooked/model/<stem>.bmodel, then
 * generated/model/<stem>.bmodel (the md6 collision models ship only there). Returns the count (0 when `name` is
 * not a model reference; a .bmodel path is its own request); each is malloc'd (sh_mb_requests_free). */
int  sh_mb_model_keys(const char *name, char *out[], int max);

/* Game files several mods commonly edit: the product
 * generates each per map from the game's text and the declared mods'
 * derived edits (modregistry.h); the names are kept for readers. */
#define SH_MB_REGISTRY_CONDUCTOR "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl"
#define SH_MB_REGISTRY_BODY      "generated/decls/md6def/player/mp/tp_body_mp.md6.decl"
#define SH_MB_REGISTRY_PLAYER    "generated/decls/entitydef/player/coop.decl"

/* The canonical text of a mod.json "requirements" value, as the builder writes it into the manifest:
 * {"commands":{"activate":[...],"deactivate":[...]},"cvars":{"<name>":"<value>",...}}. cvar names are a letter
 * or '_' then letters, digits or '_'; values are printable ASCII strings, integers or booleans (true "1", false
 * "0"), sorted by name, one name once (case-insensitive). Each command is one console line (printable ASCII, no
 * ';', trimmed), in the author's order; "cvars" is always present, "commands" and each list only when not
 * empty. Returns a malloc'd string, or NULL with the first cause. */
char *sh_mb_requirements_canonical(const char *text, size_t len, char **cause);

/* A private virtual-texture family member: virtualtextures/mods/<id>/[<folders>/]<family>_sq<N>.mega2, N 1 to
 * 16 with no leading zero, no "_vmtr" in the family. Returns 1 and, when non-NULL, the family name (malloc'd:
 * "mods/<id>/.../<family>", which a material names as its landpagefile) and N; else 0. It is the only form in
 * which a .mega2 is admitted, and never a shadow. */
int  sh_mb_vt_member(const char *request, char **family, int *member);

/* Sound banks: Wwise opens a bank by file name, its id FNV-1 of the lower-cased stem, so a mod bank is admitted
 * only as sound/soundbanks/pc/<stem>.bnk with stem = "<id, '.' and '-' as '_'>_<rest>" of [a-z0-9_], without
 * "_sp", "_snapmaps" or "initial" (skipped by the engine's listed-bank filter in some game mode), carried, with
 * a version 113 BKHD chunk carrying id FNV-1(stem). Its logical name is type "soundBank", the request path. */
/* A bank is served as <stem>.<16 hex of its hash>.bnk (modserve.c) and Windows holds a file name of at most 255
 * characters, so the stem is at most 234 characters. */
#define SH_MB_BANK_STEM_MAX  234u
#define SH_MB_BANK_VERSION   0x71u
/* 32-bit FNV-1 (multiply, then xor) of the ASCII-lower-cased first n bytes:
 * Wwise's GetIDFromString. */
uint32_t sh_mb_fnv1_lower(const char *s, size_t n);
/* 1 and the stem when request is sound/soundbanks/pc/<stem>.bnk (one level). */
int  sh_mb_bank_stem(const char *request, char *stem, size_t cap);
/* The stem rules above for mod `id`; 0 with the cause. */
int  sh_mb_bank_rule(const char *request, const char *id, char **cause);
/* The header rule above on the bank's first bytes; 0 with the cause. */
int  sh_mb_bank_header_ok(const unsigned char *bytes, size_t len, const char *stem, char **cause);

/* Logical name of a request path: 1 with the type (the engine's own type table spelling, under SH_MB_TYPE_CAP
 * bytes) in type[] and the name in *name (malloc'd); 0 when the path is under generated/decls/ but names no
 * known decl type or an empty name, or memory runs out. */
#define SH_MB_TYPE_CAP 64
int  sh_mb_logical_name(const char *request, char type[SH_MB_TYPE_CAP], char **name);

/* Engine spelling of a generated/decls/<folder>/ type folder (case-insensitive),
 * or NULL when 13954591 ships no decl of that type. */
const char *sh_mb_decl_type(const char *folder, size_t folder_len);

/* Request paths the engine would ask the provider for when it loads the logical (type, name): lower-cased, each
 * malloc'd (sh_mb_requests_free); returns the number written. A model name (.lwo, .ase, .ma, .obj, .fbx) is
 * cooked/model/<stem>.bmodel, then generated/model/<stem>.bmodel (md6 collision models ship only there). */
int  sh_mb_need_requests(const char *type, const char *name, char *out[], int max);
void sh_mb_requests_free(char *out[], int count);

/* The path or name with its first "mods/<salt>/" segment removed (any non-empty segment, so '-' in decl names
 * and '_' in md6 paths are both cut): the installed name a renamed copy most likely came from. Returns it
 * (lower-cased, malloc'd), NULL when no segment was removed. */
char *sh_mb_strip_key(const char *in);

/* Tolerant decl reference scan (see modbundle.c for the keys). The callback
 * receives each reference; name is not NUL-terminated. */
typedef void (*sh_mb_need_fn)(void *context, const char *type,
                              const char *name, size_t name_len);
void sh_mb_scan_needs(const char *decl_type, const unsigned char *text, size_t len,
                      sh_mb_need_fn callback, void *context);

/* The Graphics SDK's look compiler (modlook.h sh_look_compile_notes), set by the backend before the first
 * build: the build reads each material decl through it, so a mistake in a look fails the build, and notes go to
 * the build report. Unset, a material is scanned as its file holds it. */
typedef int (*sh_mb_look_fn)(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                             size_t why_len, char **notes);
void sh_mb_set_look_compiler(sh_mb_look_fn fn);

/* The Graphics SDK's compilers (sdk.h sh_sdk_compile), set by the backend: when set, the build reads each
 * material, fx and env decl through it instead, and each `type name` line of *needs joins the file's needs, so
 * the closure brings the product's derived decls along as pointers. */
typedef int (*sh_mb_sdk_fn)(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                            size_t why_len, char **notes, char **needs);
void sh_mb_set_sdk_compiler(sh_mb_sdk_fn fn);

/* Bare (unquoted) words that name a mod resource ("mods/...") and contain '-', which the engine's breakable and
 * material lexers split. The callback receives the 1-based line and the word (not NUL-terminated). Returns the
 * count; the builder refuses each. */
typedef void (*sh_mb_bare_fn)(void *context, int line, const char *name, size_t name_len);
int  sh_mb_scan_bare_hyphen(const unsigned char *text, size_t len, sh_mb_bare_fn callback, void *context);

/* Render programs. MD5 of data, and MD5fold: the four little-endian 32-bit
 * words of the digest XORed together, the engine's decl and stage checksum. */
void     sh_mb_md5(const void *data, size_t len, unsigned char digest[16]);
unsigned sh_mb_md5fold(const void *data, size_t len);
/* The three stage texts (vertex, fragment, compute) of a cooked program
 * (generated/renderprogs/<name>_pc_vulkan.bin): offsets and lengths into bin.
 * Returns 1, 0 when none are found, -1 when the layout is ambiguous. */
int  sh_mb_rp_stages(const unsigned char *bin, size_t len, size_t off[3], size_t n[3]);
/* The SPIR-V request the stage loader builds for a program name (a program decl name, or a permutation decl's
 * request path) and extension (".vspv", ".fspv", ".cspv"): "generated/spirv/" + the name with its extension
 * replaced, a basename of 38 or more characters cut into 30-character components. Returns it (malloc'd), or
 * NULL when memory runs out. */
char *sh_mb_spirv_request(const char *name, const char *ext);
/* generated/renderprogs/permutations/<program>_<16 lowercase hex>.decl:
 * returns 1 with the program name (malloc'd into *program when program is
 * not NULL) and the mask, else 0. */
int  sh_mb_rp_permutation(const char *request, char **program, char mask[17]);

/* SHA-256 as 64 lowercase hex characters. */
void sh_mb_sha256_hex(const void *data, size_t len, char out[SH_MB_HASH_HEX + 1]);
/* Hash a file (UTF-8 path). Returns 1 and sets *size. */
int  sh_mb_sha256_file(const char *path, unsigned long long *size,
                       char out[SH_MB_HASH_HEX + 1]);


/* Wide long-path form of a UTF-8 path (\\?\ prefixed when absolute), malloc'd.
 * Exposed for modstore. */
#ifdef _WIN32
#include <wchar.h>
wchar_t *sh_mb_wpath(const char *utf8);
char    *sh_mb_utf8(const wchar_t *wide);
/* Read a whole file (UTF-8 path) up to max bytes. malloc'd, NUL-terminated. */
unsigned char *sh_mb_read_file(const char *path, size_t max, size_t *out_len);
#endif

/* The product's user-data folder, %LOCALAPPDATA%\snapmap-plus, in UTF-8 (resolved through the wide API); every
 * store and prefab root derives from it, modstore.h sh_ms_root being its mods\ folder. Resolved once; NULL when
 * Windows does not answer. */
const char *sh_mb_user_root(void);

#endif /* SNAPMAP_PLUS_MODBUNDLE_H */
