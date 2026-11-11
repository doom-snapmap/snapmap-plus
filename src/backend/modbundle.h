/* modbundle.h -- build, validate and read mod bundles (mod.json).
 *
 * A mod is a folder holding mod.json and an assets/ tree at exact engine
 * request paths. Building it hashes every file, names what each file
 * provides, scans decl text for what it needs, checks the namespace and
 * structural rules, and produces the canonical manifest whose SHA-256 is the
 * build hash.
 *
 * mod.json, format 2, is one JSON object with two parts. The author's part is
 * every top-level key except "snapmap-plus": name (required),
 * description, author, homepage, requirements, strings, hud, companions and
 * any other key, which is kept and becomes the manifest's "extra". The
 * builder's part, the member "snapmap-plus", is written only by the product:
 *
 *   "snapmap-plus": {
 *     "format": 2, "salt": "<8 characters>", "rules": 10, "built": <unix seconds>,
 *     "tree": "<sha256>",              the rebuild rule's tree hash (sh_mb_tree_hash)
 *     "build": "<sha256>",             the build hash; absent while the build fails
 *     "cause": "<first cause>",        only while the build fails (or a verification found one)
 *     "warnings": [...],               the first SH_MB_WARNINGS_KEPT warnings, when any
 *     "classes": {"adds", "borrows", "edits", "replaces", "editor", "surfaces"},
 *     "editor": ["<request>", ...],    files classified as editor tweaks
 *     "registries": {"banks": [...], "conductor": [...], "strings": [...]},
 *     "edits": [{"file", "stock", "counts", "ops"}, ...],   derived edits of decls at game names
 *     "scripts": {"files", "entities": [{"name", "events"}], "inputs", "outputs", "dynamic"},
 *                                      what the mod's scripts bind, read without running them
 *                                      (sh_mb_script_scan); only when the mod carries scripts
 *     "shadows": ["<request>", ...],
 *     "files": [{"path", "sha256", "size", "provides": {"type", "name"}, "needs": [...]}],
 *     "stock": [{"path", "sha256", "source", "provides", "needs", "closure": true?}]
 *   }
 *
 * Identity is the salt (modsalt.h), nothing else: the namespace mods/<salt>/,
 * the canonical "id", and the prefix of every strings key (`<salt>_<name>`,
 * referenced `#str_<salt>_<name>`; no '/' in a string key). A folder's name is the
 * author's and never read. A folder in an earlier shape (package.json, a block
 * without a salt, an author id, key, shadows, stock, scope, player, extends,
 * replaces, legacy, composes or schema key, a hud schema) is refused with
 * SH_MB_EARLIER, one sentence (sh_mb_descriptor_state).
 *
 * Additive mods. The builder classifies every file from its path and bytes
 * (SH_MB_CLASS_*); "shadows" is derived from the classification (every
 * replacement and every editor tweak at a game name). "classes", "editor",
 * "registries" and "edits" are derived facts outside the canonical text. A
 * stock pointer is derived from a carried file whose bytes are the game's,
 * or ("closure": true) by the campaign-only closure, which every build
 * derives again; a closure pointer has no file under assets/.
 *
 * Writing the block never touches the author's part: the file is spliced. The
 * old block member is cut out (from the end of the member before it to the
 * end of its value), and the new one is inserted after the last author member
 * as ",\n\n  \"snapmap-plus\": {...}" (CRLF when the file uses CRLF). Cutting
 * the block out of any written file therefore gives back the author's bytes
 * exactly (sh_mb_author_bytes). A file without an author text (a manifest
 * written out) gets a synthesized author part.
 *
 * The canonical manifest (below) is derived in memory, never stored: it is
 * the input of the build hash.
 *
 * Canonical manifest: UTF-8 JSON, object keys sorted by their UTF-8 bytes, no
 * whitespace, files sorted by path, "bundle" omitted. "format" is 2 and "id"
 * the salt. Strings escape only '"',
 * '\\' and control characters (\b \f \n \r \t, others \u00xx lowercase), so the
 * text equals JSON.stringify of the parsed value with sorted keys. Numbers are
 * plain integers. Optional top-level keys (author, homepage, requirements,
 * shadows, extra) are omitted when absent or empty. A carried file has exactly
 * blob, needs (possibly []), path, provides, request and size.
 *
 * Stock pointers (file kind "stock"): a file may instead name installed game
 * content, {"needs","path","provides","request","stock":{"sha256","source"}},
 * with no blob and no size. "source" is the installed resource path (an
 * archive entry of gameresources or snap_gameresources, .patch or .resources,
 * or a loose file under the install's base\ or root), lower-cased with '/';
 * "sha256" is the SHA-256 of the installed bytes (64 lowercase hex). The
 * builder emits one when a file's bytes equal an installed entry. Pointers are not counted in the size caps and the extension whitelist does
 * not apply to them; an upload never sends their bytes, and a client whose
 * game lacks the entry with that hash refuses the bundle with the first cause.
 * Some kinds are admitted only as pointers (sh_mb_pointer_only_kind): the
 * per-map sound BSP, light grid, occlusion tome and virtual-texture page file.
 *
 * A pointer that restores installed content under the game's own name
 * (sh_mb_stock_restore: the request is the source itself, or the generated
 * decl of an own-path decl source) replaces nothing, so it needs no shadows
 * entry and sits outside the mod namespace; the serving layer registers it
 * only when the running game does not already know the name (SnapMap does not
 * see campaign and multiplayer content in gameresources).
 *
 * SnapMap modules: maps/modules/<path>.decl (outside maps/modules/palettes/)
 * provides snapModule "<that request>", the name a saved map's moduleName
 * holds; it must be well formed and its snapModuleInfo,
 * generated/decls/snapmoduleinfo/<path>.decl, must be in the same bundle.
 *
 * Pure file and text work: no engine calls, no allocation on the engine heap.
 * Stock and installed resources are consulted through caller callbacks; the
 * installed content behind stock pointers through the registered stock ops
 * (sh_mb_set_stock_ops, implemented by modstock.c).
 */
#ifndef SNAPMAP_PLUS_MODBUNDLE_H
#define SNAPMAP_PLUS_MODBUNDLE_H

#include <stddef.h>
#include <stdint.h>

#define SH_MB_FORMAT          2
/* The builder's rules, written into the builder block as "rules". A root
 * folder whose block carries another number is built again once by the
 * rebuild rule, so a product update reclassifies every own folder. Raise it
 * when a build's classification, edits or manifest change. 11: files beside
 * mod.json and assets are ignored, and campaign content derives its
 * requirements. */
#define SH_MB_RULES           11

/* The refusal of a folder in any shape but this one (ONE-SHAPE rule 6). */
/* The refusal of a folder in an earlier shape (DECISIONS 21), one sentence;
 * SH_MB_SHAPE is its first part, which readers of a cause match on. */
#define SH_MB_SHAPE           "This mod was made for an earlier version of Snapmap+. "
#define SH_MB_EARLIER         SH_MB_SHAPE "Run the Snapmap+ installer again and it converts it."
#define SH_MB_ID_MAX          64u
#define SH_MB_NAME_MAX        80u      /* code points */
#define SH_MB_AUTHOR_MAX      80u      /* code points */
#define SH_MB_HOMEPAGE_MAX    512u     /* code points */
#define SH_MB_MAX_FILES       4096u
#define SH_MB_MAX_TOTAL       (256ull * 1024u * 1024u)
#define SH_MB_MAX_FILE        (64ull * 1024u * 1024u)
#define SH_MB_REQUEST_MAX     400u     /* bytes of one request path */
#define SH_MB_DESCRIPTOR_MAX  (1024u * 1024u)
#define SH_MB_MANIFEST_MAX    (1024u * 1024u)   /* the store's bundle JSON cap */
#define SH_MB_CAUSE_MAX       512u
#define SH_MB_WARNINGS_KEPT   16u
#define SH_MB_HASH_HEX        64u
/* mod.json "requirements" (sh_mb_requirements_canonical): any cvar, any
 * console line, within these bounds. The Worker's manifest check mirrors them. */
#define SH_MB_REQ_CVARS_MAX    32u     /* cvars one bundle may set */
#define SH_MB_REQ_NAME_MAX     64u     /* bytes of a cvar name */
#define SH_MB_REQ_VALUE_MAX    64u     /* bytes of a cvar value */
#define SH_MB_REQ_COMMANDS_MAX 16u     /* lines in each of activate and deactivate */
#define SH_MB_REQ_LINE_MAX     256u    /* bytes of one console line */
#define SH_MB_BLOCK_KEY       "snapmap-plus"   /* mod.json's builder block */

typedef struct sh_mb_need {
    char *type;            /* engine spelling: md6Def, baseModel, anim, material, ... */
    char *name;            /* logical name as the decl text spells it */
} sh_mb_need;

typedef struct sh_mb_file {
    char *path;            /* "assets/" + request: a build records the game name, lower case with '/',
                              whatever the spelling on disk (DECISIONS 13); a loaded block's as written */
    char *request;         /* path below assets/, lower-cased: what the provider is asked for */
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
    int absent;            /* a stock pointer with no file under assets/ (a closure pointer, or a received build's) */
    int cls;               /* SH_MB_CLASS_*: what the file is to the game (sh_mb_classify) */
    char *edit;            /* builder: a decl at a game name, its derived edits element (JSON), or NULL */
    int edited;            /* an edited copy: a REPLACE file the bundle's edits name (built or loaded); never served */
    int closure;           /* builder: campaign-only content a served decl needs, derived by this build ("closure": true) */
} sh_mb_file;

/* Per-file classification. Decided from the
 * file's path and bytes, never from the descriptor:
 *   EDITOR   an editor-only type (sh_mb_editor_time) outside the namespace:
 *            a local tweak. The same types under the namespace are ADD or
 *            BORROW: a Toybox row the mod adds travels with it.
 *   ADD      under the mod's own namespace, bytes the game does not hold
 *   BORROW   under the namespace, a stock pointer (the game's own bytes)
 *   SURFACE  a stock pointer restoring installed content at its own name:
 *            served from the player's disk, never carried, never refused
 *            (SnapMap's own provider cannot see campaign-only content)
 *   REPLACE  any other file at a game name. A decl is an edited copy: the
 *            builder records its structural diff against the game's text
 *            (sh_mb_bundle.edits) and the product generates the game file
 *            per map; the copy itself is never served. A binary is refused
 *            (put it under mods/<salt>/), except in a build the product
 *            runs on content it received (sh_mb_keep_replacements_here). */
#define SH_MB_CLASS_NONE    0
#define SH_MB_CLASS_ADD     1
#define SH_MB_CLASS_BORROW  2
#define SH_MB_CLASS_REPLACE 3
#define SH_MB_CLASS_EDITOR  4
#define SH_MB_CLASS_SURFACE 5

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
    char  prev_hash[SH_MB_HASH_HEX + 1];   /* sh_mb_build: the build hash the folder's block named before this
                                              build, "" when none */
    char  tree[SH_MB_HASH_HEX + 1];   /* block "tree": the folder's tree hash when built, "" when absent */
    long long built;       /* block "built": the first successful build of this salt in this folder, 0 when none */
    int   validated;       /* 1 when the block records no "cause" (a build without errors) */
    int   rules;           /* block "rules": the builder's SH_MB_RULES that wrote it; 0 when absent */
    char  state_cause[SH_MB_CAUSE_MAX];   /* block "cause" */
    sh_mb_need_stats needs;
    unsigned errors, warnings;
    char  first_cause[SH_MB_CAUSE_MAX];
    char  first_warning[SH_MB_CAUSE_MAX];
    /* Every warning of the build in order, the first SH_MB_WARNINGS_KEPT of
     * them (`warnings` counts them all). */
    char **warning_list;
    size_t warning_list_count;
    sh_mb_classes classes; /* counts of the files' classes */
    char *registries;      /* compact JSON of the block's "registries" object, or NULL */
    char *scripts;         /* compact JSON of the block's "scripts" object, or NULL (no scripts) */
    unsigned script_files; /* files of kind script (sh_mb_script_request) */
    /* Derived edits: the block's "edits", a JSON array
     * with one element per decl the mod carries at a game name with other
     * bytes (decl_tree.h), or NULL; edit_files elements. */
    char *edits;
    unsigned edit_files;
    char **notes;          /* builder: report lines (replacements, closure, edits) */
    size_t note_count, note_cap;
} sh_mb_bundle;

/* Whether two decl texts are one text: line ends are not content, so CRLF
 * and LF compare equal and the run of line ends at the end is ignored. The
 * builder compares a decl-grammar copy at a game name with the game's text
 * this way; a binary keeps byte identity. */
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

/* ----------------------------------------------------- stock pointers ---- */

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
                 char *source, size_t source_cap);
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
    /* Every installed archive path starting with `prefix` (lower case),
     * passed to `fn` (a path several rows hold may come more than once): OK
     * or UNAVAILABLE. May be NULL. The builder lists a campaign render
     * program's permutations with it. */
    int (*list)(void *context, const char *prefix, void (*fn)(void *fn_context, const char *path), void *fn_context);
} sh_mb_stock_ops;

/* Register the installed-content reader (copied; NULL clears). Without one the
 * builder emits no pointers, and stock
 * files cannot be verified or served. */
void sh_mb_set_stock_ops(const sh_mb_stock_ops *ops);
/* 1 when a reader is registered. */
int  sh_mb_stock_available(void);

/* The stock-bank question (modbankinfo.h sh_mbi_bundle_need): 1 with the stock
 * Wwise bank a sound decl named `decl_name` needs loaded on demand (a bank
 * SnapMap's game-mode filter skips, the only kind filing its event), else 0.
 * With one registered, a build lists each such bank once, in byte order, as
 * the product block's registries "banks" and notes which decl needs it. NULL (the default) lists none. */
typedef int (*sh_mb_bank_need_fn)(const char *decl_name, char *bank, size_t cap);
void sh_mb_set_bank_need(sh_mb_bank_need_fn fn);
/* The registered operations, with a first cause naming `path` when not OK. */
int  sh_mb_stock_verify(const char *source, const char *sha256, unsigned long long *size,
                        const char *path, char *cause, size_t cause_cap);
/* The registered identify operation: OK with the installed entry's sha256 and size, ABSENT or UNAVAILABLE. */
int  sh_mb_stock_identify(const char *source, char sha256[SH_MB_HASH_HEX + 1], unsigned long long *size);
int  sh_mb_stock_read(const char *source, const char *sha256, unsigned char **bytes, size_t *len,
                      const char *path, char *cause, size_t cause_cap);
/* The registered container operation: 1 with the archive (or "loose") in `out`; 0 when no reader answers. */
int  sh_mb_stock_container(const char *source, const char *sha256, char *out, size_t cap);

/* Validate a stock source path: the request-path rules without the extension
 * whitelist. 1 when valid. */
int  sh_mb_check_source(const char *source, char *cause, size_t cause_cap);

/* Build the bundle for `folder` (UTF-8 path). Returns 1 when the bundle has no
 * errors, 0 otherwise; out->first_cause and out->errors describe the refusal.
 * out is always initialised and must be released with sh_mb_free. Nothing is
 * written to disk. The salt is the block's; `salt` (NULL to read it) gives
 * one to a folder whose block has none yet, and must equal the block's when
 * both are present. A folder in an earlier shape, or one without a salt, is
 * refused with SH_MB_EARLIER. "built" is the block's when it has one, else the
 * time of this build when it succeeds; out->tree is the folder's tree hash and
 * out->prev_hash the build hash its block recorded. Every file is classified
 * (out->files[].cls, out->classes); a file outside the namespace that is not a
 * surface must be a game name (resolver->stock), and is then a shadow: an
 * editor tweak, or a replacement: a decl's derived edits are recorded
 * (out->edits), a binary is refused. out->registries is filled, and
 * out->notes holds the report lines the build log prints. */
int  sh_mb_build(const char *folder, const char *salt, const sh_mb_resolver *resolver, sh_mb_bundle *out);

/* Write `<folder>\mod.json`: the author's part (the folder's mod.json without
 * its block, else a synthesized one) and the builder block from `bundle`,
 * including its cause when not validated. Write-through temporary and rename.
 * Refuses a bundle with errors. Returns 1 on success. */
int  sh_mb_write(const sh_mb_bundle *bundle, const char *folder, char *cause, size_t cause_cap);

/* Write the block of a failed build into `<folder>\mod.json`: format, salt,
 * rules, built (when it had one), tree and cause (bundle->first_cause) only,
 * so the folder serves nothing until fixed. bundle->id must be a salt.
 * Returns 1 on success. */
int  sh_mb_write_failed(const sh_mb_bundle *bundle, const char *folder, char *why, size_t why_cap);

/* Record a verification result in `<folder>\mod.json`'s block (a cause, or
 * none) without changing anything else. Returns 1 on success. */
int  sh_mb_set_state(const char *folder, int validated, const char *cause, char *why, size_t why_cap);

/* The complete mod.json text for `bundle` (malloc'd), splicing the builder
 * block into `author_text` (a mod.json text, its own block ignored) or, when
 * author_text is NULL, into a synthesized author part. */
char *sh_mb_modjson_text(const sh_mb_bundle *bundle, const char *author_text, size_t author_len, size_t *out_len);

/* The author's bytes of a mod.json text: the text without its "snapmap-plus"
 * member (malloc'd). *had_block (optional) says whether there was one.
 * NULL when the text is not a JSON object. */
char *sh_mb_author_bytes(const char *text, size_t len, size_t *out_len, int *had_block);

/* Read and check a built mod.json: the author's part, the builder block,
 * structure, salt, paths, logical names and the build hash
 * recomputed from the canonical form. Files on disk are not read. Returns 1
 * when valid. A block without "build" fails with "not built: <cause>" (or
 * "not built yet"), after filling id, built, rules, tree and state_cause; an
 * earlier shape fails with SH_MB_EARLIER. A block with a "cause" beside its
 * "build" loads with validated 0. */
int  sh_mb_load(const char *mod_json_path, sh_mb_bundle *out);
int  sh_mb_load_text(const char *text, size_t len, sh_mb_bundle *out);
/* `<folder>\mod.json`. */
int  sh_mb_load_folder(const char *folder, sh_mb_bundle *out);

/* Tree hash of a mod folder, the rebuild rule's key (the block's "tree"):
 * SHA-256 over the author's bytes of mod.json, then every file under
 * assets/ sorted by path with its size and last-write time. Cheap: no file is
 * read. Returns 1 and the hex. */
int  sh_mb_tree_hash(const char *folder, char out[SH_MB_HASH_HEX + 1]);

/* The store tool's conversion only (src/store/store_old_ids.c): what a
 * bundle serves, independent of its label: SHA-256 over id, requirements,
 * shadows, extra, and every file's request, kind, hash and size. */
void sh_mb_content_key(const sh_mb_bundle *bundle, char out[SH_MB_HASH_HEX + 1]);

/* ------------------------------------------------- the product record ---- */
/* <mods_root>\snapmap-plus.json, the product's one file under mods\:
 *   {"format": 1, "claims": {...}, "links": {...}, "pending": {...},
 *    "places": {...}, "salts": {...}, ...}
 * Each section is owned by one module and read and written whole. Writes are
 * a read, a replace of that section and a write-through rename, under the
 * mods folder's named lock (sh_mb_record_lock; store_lock.h), the same mutex
 * as the store lock and shared with every other process on the folder, that
 * a caller may also hold around its own read-modify-write. A file another
 * program holds is waited for, up to a second. A file that is not JSON is
 * moved aside when a section is written, logged with the aside file's name,
 * and a new one started. A file that cannot be read is never replaced. */
#define SH_MB_RECORD_FILE "snapmap-plus.json"
/* Recursive per thread; sh_mb_record_unlock releases this thread's hold. */
void sh_mb_record_lock(const char *mods_root);
void sh_mb_record_unlock(void);
/* The mods folder's named lock (store_lock.h), opened once per folder and
 * kept; NULL when it cannot be opened. sh_mb_store_wait takes it, logging one
 * "MODS: store: waiting" line when another thread or process holds it; the
 * caller releases it with ReleaseMutex. sh_mb_store_waiting counts this
 * process's threads waiting for it now, sh_mb_store_waits the waits so far.
 * The mutex is a Win32 HANDLE. */
void    *sh_mb_store_mutex(const char *mods_root);
void     sh_mb_store_wait(void *mutex);
long     sh_mb_store_waiting(void);
unsigned sh_mb_store_waits(void);


/* 1 with the section's JSON text (malloc'd, compact), 0 when absent, -1 when
 * the record cannot be read (a "MODS: record:" line names the file when it
 * could not be opened or read; that section is then not written until a get
 * of it succeeds again, so a read-modify-write never drops what it holds). */
int  sh_mb_record_get(const char *mods_root, const char *section, char **json_out);
/* Replace (json != NULL) or remove (NULL) one section. 1 on success; 0 with a
 * "MODS: record:" line naming the file when the record cannot be read or the
 * section's last get failed. */
int  sh_mb_record_put(const char *mods_root, const char *section, const char *json);
/* The player-facing sentence for the last record read or write this thread
 * could not make: the file's full path, why, and the next step (close the
 * program holding it). A flow that writes the record clears it when it starts
 * and adds it to its result message when sh_mb_record_why returns 1. */
void sh_mb_record_why_clear(void);
int  sh_mb_record_why(char *out, size_t cap);

void sh_mb_free(sh_mb_bundle *bundle);

/* Build `folder` in place with the game-backed resolver (installed bundles,
 * then the player's game archives) and write its mod.json: the entry the
 * store tool's conversions call. Returns 1 on success; 0 with the first cause
 * in `first_cause` (NUL-terminated, at most n bytes). Defined in modcmd.c,
 * which owns the resolver; logs MODS: lines. */
int  modbundle_build_folder(const char *folder, char *first_cause, size_t n);

/* ----------------------------------------------------------- helpers ---- */

/* Identity (ONE-SHAPE rule 1). The state of a folder's descriptor:
 *   FRESH        mod.json with the author's part only (no block, no retired key)
 *   BUILT        a format-2 block with a valid salt (built, failed, or salted only)
 *   EARLIER      package.json, or a block of an earlier format
 *   RETIRED_KEY  an author key this version does not read (mb_retired_key)
 *   UNREADABLE   no mod.json, not a JSON object, or a format-2 block without a salt */
#define SH_MB_DESC_FRESH       1
#define SH_MB_DESC_BUILT       2
#define SH_MB_DESC_EARLIER     3
#define SH_MB_DESC_RETIRED_KEY 4
#define SH_MB_DESC_UNREADABLE  5
int  sh_mb_descriptor_state(const char *folder);
/* The same, with the refusal sentence (SH_MB_EARLIER) for EARLIER,
 * RETIRED_KEY and UNREADABLE; "" for FRESH and BUILT. */
int  sh_mb_descriptor_state_why(const char *folder, char *cause, size_t cause_cap);

/* Every carried file and provided name under some mods/<x>/ has x equal to
 * the bundle's salt (any case; a salt has one spelling). 1 when so; else 0
 * with the sentence naming the first file under another mod's salt. */
int  sh_mb_salt_paths_check(const sh_mb_bundle *bundle, char *cause, size_t cause_cap);

/* Every reference to mods/<from> in `text` (n bytes), as written or in the
 * engine's cooked spelling (mb_binary_name), in any case, not inside a longer
 * name, rewritten to mods/<to>. `from` is a namespace segment: a fresh
 * folder's leaf, or a copy's old salt. Returns the count; with `out`, the
 * rewritten text (malloc'd, NUL-terminated, NULL when the count is 0) and its
 * length; (size_t)-1 when out of memory. Without `out` it only counts. */
size_t sh_mb_rekey_text(const char *text, size_t n, const char *from, const char *to, char **out, size_t *out_len);

/* `rel` ('/'-separated) with every folder segment right after a "mods"
 * segment that is `from` (as written or cooked, any case) replaced by `to`.
 * 1 when changed, 0 when not, -1 when out is too small. */
int  sh_mb_rekey_path(const char *rel, const char *from, const char *to, char *out, size_t cap);

/* A mod.json text with its builder block replaced by {"format": 2, "salt":
 * <salt>} and every reference to mods/<from>/ in the author's part (strings
 * keys, hud decl names, any value) rewritten to mods/<salt>/ (from NULL: no
 * rewrite). The author's other bytes are kept exactly. malloc'd, or NULL when
 * not a JSON object or salt is not a salt. */
char *sh_mb_salt_modjson(const char *text, size_t n, const char *from, const char *salt, size_t *out_len);

/* A new mod's strings keys (DECISIONS 6). sh_mb_salt_strings returns the
 * mod.json text with every key of every "strings".<language> object that
 * lacks the "<salt>_" prefix renamed "<salt>_<key>" (an earlier spelling's
 * leading "mods/<salt>/", or a copy's "<from>_" when `from` is a salt,
 * dropped; any character outside [A-Za-z0-9_] made '_'); every other byte
 * is kept. *keys (free with sh_mb_strkeys_free)
 * lists each renamed key once, from -> to. NULL when the text does not read
 * as a JSON object or salt is not a salt. sh_mb_restr_text rewrites every
 * "#str_<from>" reference (any case, ending at a name boundary) in a text to
 * "#str_<to>": the count, and with `out` the rewritten text (malloc'd, NULL
 * when the count is 0); (size_t)-1 when out of memory. */
typedef struct sh_mb_strkey { char *from, *to; } sh_mb_strkey;
char  *sh_mb_salt_strings(const char *text, size_t n, const char *from, const char *salt, sh_mb_strkey **keys, size_t *count,
                          size_t *out_len);
size_t sh_mb_restr_text(const char *text, size_t n, const sh_mb_strkey *keys, size_t count, char **out, size_t *out_len);
void   sh_mb_strkeys_free(sh_mb_strkey *keys, size_t count);

/* The classes counts as one line ("2 adds, 0 borrows, 1 edits, 0 replaces, 0 editor, 0 surfaces"). */
void sh_mb_classes_text(const sh_mb_classes *c, char *out, size_t cap);

/* For the calling thread only: 1 accepts a binary at a game name in the
 * builds it runs (an install of content the product received, such as a
 * rawmap or prefab import or a store-tool conversion, is never refused on its
 * files); 0 ends it. Every other build refuses such a file
 * Thread-local, so a worker's import never changes a
 * rescan's rule. */
void sh_mb_keep_replacements_here(int on);

/* The store tool's conversion only (src/store/store_old_ids.c): an earlier release's mod id, trimmed,
 * lower-cased and checked (1-64 characters of [a-z0-9._-], starting and
 * ending with a letter or digit, no ".."). Returns 1 and writes out. */
int  sh_mb_id_normalize(const char *in, size_t len, char out[SH_MB_ID_MAX + 1]);

/* Validate one request path (relative, '/', no "." or ".." segment, allowed
 * characters [A-Za-z0-9._-$+@~()] and internal spaces, no segment that
 * starts with a space or ends in '.' or a space, at most
 * SH_MB_REQUEST_MAX bytes). 1 when valid, else 0 with the cause. */
int  sh_mb_check_request(const char *request, char *cause, size_t cause_cap);

/* Extension whitelist. A .bswf movie is admitted only under
 * generated/swf/. */
int  sh_mb_extension_allowed(const char *request);

/* ------------------------------------------------------- mod scripts ---- */
/* A script is a carried text file of kind "script" (provides type "script",
 * name its request): assets/mods/<salt>/scripts/<name>.lua, an entry file the
 * runtime runs at map start, or assets/mods/<salt>/scripts/lib/<name>.lua, a
 * module only `require` loads, of the same mod. <name> is [A-Za-z0-9_-], at
 * most 64 characters. The engine never opens a script: it is hashed, carried
 * and indexed like every file, and the runtime reads its built bytes through
 * the serving index (modserve.h sh_modserve_script_files). mod.json's author
 * part declares nothing about scripts.
 *
 * Checked at build: UTF-8 text without NUL, not compiled Lua (a chunk starting
 * "\x1bLua"), at most SH_MB_SCRIPT_FILE_MAX bytes each, at most
 * SH_MB_SCRIPT_FILES_MAX files and SH_MB_SCRIPT_MOD_MAX bytes per mod, and
 * compiled (nothing runs) by the registered check; a literal require of a
 * module the mod does not carry, and a name of another mod's namespace, are
 * refused with the file and line. The builder block records what the scripts
 * bind ("scripts", sh_mb_script_scan). The limits equal the runtime's
 * (script_runtime.h; script_compile.c asserts it). */
#define SH_MB_SCRIPT_FILE_MAX   (256u << 10)
#define SH_MB_SCRIPT_MOD_MAX    (1u << 20)
#define SH_MB_SCRIPT_FILES_MAX  64u
#define SH_MB_SCRIPT_NAME_MAX   64u

/* What `request` (lower-cased, below assets/) is to scripts: SH_MB_SCRIPT_ENTRY
 * or SH_MB_SCRIPT_LIB for a script path (`*name` then points at its <name>,
 * `*name_len` its length; either may be NULL), SH_MB_SCRIPT_MISPLACED for any
 * other .lua (refused with SH_MB_SCRIPT_WHERE), else 0. Pure. */
#define SH_MB_SCRIPT_ENTRY      1
#define SH_MB_SCRIPT_LIB        2
#define SH_MB_SCRIPT_MISPLACED  (-1)
#define SH_MB_SCRIPT_WHERE \
    "a script goes in this mod's scripts folder, assets\\mods\\<salt>\\scripts\\ (entry files) or scripts\\lib\\ " \
    "(files loaded with require); move it there"
int  sh_mb_script_request(const char *request, const char **name, size_t *name_len);

/* The author's spelling of a script request: "scripts\<name>.lua" or
 * "scripts\lib\<name>.lua", the name every script message uses. */
void sh_mb_script_file_name(const char *request, char *out, size_t cap);

/* The compile check the product registers (script_compile.c): `file` is the
 * author's spelling, `entry` 1 for an entry file (which must start with
 * "api = 1"), 0 for a scripts\lib file. 1 when the text compiles (nothing
 * runs); 0 with *line (0 when none) and the plain what and todo, neither
 * ending with a period. Any thread. Without a registered check a build that
 * carries a script fails, naming the file: nothing unchecked serves. */
typedef int (*sh_mb_script_check_fn)(const char *file, int entry, const char *text, size_t len, int *line,
                                     char *what, size_t what_cap, char *todo, size_t todo_cap);
void sh_mb_set_script_check(sh_mb_script_check_fn fn);

/* The check of the author's content blocks the game reads later ("scenes",
 * "hud"; modcontent.h sh_modcontent_check): `extra` is the build's preserved
 * author keys. 0 with one sentence in `cause` fails the build with it. NULL
 * (the default) checks nothing. */
typedef int (*sh_mb_extra_check_fn)(const char *salt, const char *extra, size_t len, char *cause, size_t cause_cap);
void sh_mb_set_extra_check(sh_mb_extra_check_fn fn);


/* The static registration pass: every literal call of the registration API in
 * one script's text, without running it. Lua comments and strings are
 * skipped; a call is recognised where the code spells it:
 *   on.<event>("<name>", ...)    event spawn, think, remove, damage, death, anim  -> SH_MB_SCRIPT_BIND
 *   on.input("<node name>", ...)                                                   -> SH_MB_SCRIPT_INPUT
 *   node("<node name>")                                                            -> SH_MB_SCRIPT_OUTPUT
 *   require("<module>") or require "<module>"                                      -> SH_MB_SCRIPT_REQUIRE
 * with SH_MB_SCRIPT_DYNAMIC or'ed in (name NULL) when the argument is not a
 * plain string literal (a name set while running). `event` is the word after
 * "on." ("input" for inputs, "" otherwise); `line` is 1-based. on.map_start
 * and on.map_end name nothing and are not reported. Pure. */
#define SH_MB_SCRIPT_BIND     1
#define SH_MB_SCRIPT_INPUT    2
#define SH_MB_SCRIPT_OUTPUT   3
#define SH_MB_SCRIPT_REQUIRE  4
#define SH_MB_SCRIPT_DYNAMIC  0x100
typedef void (*sh_mb_script_call_fn)(void *context, int what, const char *event, const char *name, size_t name_len,
                                     int line);
void sh_mb_script_scan(const char *text, size_t len, sh_mb_script_call_fn fn, void *context);

/* 0 when the bundle carries a script: a per-map slice would drop names its
 * scripts build while running, so the slicer ships such a build whole (D1
 * slicing step 5's whole-build fallback); else 1. */
int  sh_mb_sliceable(const sh_mb_bundle *bundle);

/* Kinds a bundle admits only as stock pointers, never as carried bytes:
 * .sbsp, .ambientsh, .tome and .pages (per-map engine files, never authored
 * by hand and too large to carry for a multiplayer map). 1 when `request` is
 * one of them. */
int  sh_mb_pointer_only_kind(const char *request);

/* 1 when a stock pointer from `request` to `source` restores installed
 * content under the game's own name: request == source, or request is
 * generated/decls/<type>/<source>.decl for an own-path decl source (a
 * material such as maps/.../mega.decl, which models name with ".decl"). */
int  sh_mb_stock_restore(const char *request, const char *source);

/* For a snapModule decl request maps/modules/<path>.decl, the snapModuleInfo
 * request it needs (generated/decls/snapmoduleinfo/<path>.decl). 1 on
 * success; 0 when the request is not a module decl. */
int  sh_mb_module_info_request(const char *request, char *out, size_t cap);

/* Editor-time content: a request the editor itself reads and the game never
 * does at play (the snapEditor* and snapPropertyInspector* decl types, the
 * palette decls, the editor's own movies). A shadow of one changes what an
 * author can set, never what a player sees, so it is no dependency of a map
 * (modserve.c's save closure skips it) and a bundle made only of such files
 * is not installed when a map a player opens names it (modsync.c). */
int  sh_mb_editor_time(const char *request);

/* 1 when every file of a loaded bundle is editor-time content and it asks no
 * requirements: nothing in it changes play. 0 for an empty bundle. */
int  sh_mb_editor_only(const sh_mb_bundle *bundle);

/* A model reference as an entity's edit state or a decl writes it
 * (models/.../x.lwo, .ase, .ma, .obj, .fbx or a .bmodel path) as the cooked
 * request the provider is asked for (cooked/model/models/.../x.bmodel,
 * lower-cased). 1 on success; 0 when `name` is not a model reference. */
int  sh_mb_model_key(const char *name, char *out, size_t cap);
/* Every request a model reference may be served under, in the order a model
 * need resolves (sh_mb_need_requests "model"): cooked/model/<stem>.bmodel,
 * then generated/model/<stem>.bmodel (the md6 collision models
 * md6/.../collision/x.lwo ship only there). Returns the count (0 when `name`
 * is not a model reference); a .bmodel path is its own single request. */
int  sh_mb_model_keys(const char *name, char out[][SH_MB_REQUEST_MAX + 1], int max);

/* Game files several mods commonly edit: the product
 * generates each per map from the game's text and the declared mods'
 * derived edits (modregistry.h); the names are kept for readers. */
#define SH_MB_REGISTRY_CONDUCTOR "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl"
#define SH_MB_REGISTRY_BODY      "generated/decls/md6def/player/mp/tp_body_mp.md6.decl"
#define SH_MB_REGISTRY_PLAYER    "generated/decls/entitydef/player/coop.decl"

/* The canonical text of a mod.json "requirements" value (JSON text of the
 * member's value), as the builder writes it into the manifest:
 *   {"commands":{"activate":[...],"deactivate":[...]},"cvars":{"<name>":"<value>",...}}
 * cvars: names are a letter or '_' then letters, digits or '_'; values are
 * printable ASCII strings, integers or booleans, canonicalised to strings
 * (true "1", false "0"); sorted by name, one name once (case-insensitive).
 * commands: each line one console command (printable ASCII, no ';', spaces
 * trimmed), in the author's order. "cvars" is always present; "commands" and
 * each list only when not empty. Returns a malloc'd string, or NULL with the
 * first cause. */
char *sh_mb_requirements_canonical(const char *text, size_t len, char *cause, size_t cause_cap);

/* A private virtual-texture family member:
 * virtualtextures/mods/<id>/[<folders>/]<family>_sq<N>.mega2, N 1 to 16 with
 * no leading zero, no "_vmtr" in the family. Returns 1 and, when non-NULL,
 * the family name ("mods/<id>/.../<family>", which a material names as its
 * landpagefile) and N; else 0. The only form in which a .mega2 is admitted.
 * A member can never be a shadow: no stock family lives under mods/. */
int  sh_mb_vt_member(const char *request, char *family, size_t family_cap, int *member);

/* Sound banks. Wwise opens a bank by file name under one folder, and the bank
 * id is FNV-1 of the lower-cased stem, so a mod bank is admitted only as
 * sound/soundbanks/pc/<stem>.bnk with stem = "<id, '.' and '-' as '_'>_<rest>"
 * of [a-z0-9_], without "_sp", "_snapmaps" or "initial" (the engine's
 * listed-bank filter skips those in some game mode), carried (never a stock
 * pointer or a shadow), whose BKHD chunk is version 113 and carries id
 * FNV-1(stem). Its logical name is type "soundBank", the request path. */
#define SH_MB_BANK_STEM_MAX  64u
#define SH_MB_BANK_VERSION   0x71u
/* 32-bit FNV-1 (multiply, then xor) of the ASCII-lower-cased first n bytes:
 * Wwise's GetIDFromString. */
uint32_t sh_mb_fnv1_lower(const char *s, size_t n);
/* 1 and the stem when request is sound/soundbanks/pc/<stem>.bnk (one level). */
int  sh_mb_bank_stem(const char *request, char *stem, size_t cap);
/* The stem rules above for mod `id`; 0 with the cause. */
int  sh_mb_bank_rule(const char *request, const char *id, char *cause, size_t cap);
/* The header rule above on the bank's first bytes; 0 with the cause. */
int  sh_mb_bank_header_ok(const unsigned char *bytes, size_t len, const char *stem, char *cause, size_t cap);

/* Logical name of a request path. Returns 1 and fills type
 * and name; 0 when the path is under generated/decls/ but names no known decl
 * type or an empty name. */
int  sh_mb_logical_name(const char *request, char *type, size_t type_cap,
                        char *name, size_t name_cap);

/* Engine spelling of a generated/decls/<folder>/ type folder (case-insensitive),
 * or NULL when 13954591 ships no decl of that type. */
const char *sh_mb_decl_type(const char *folder, size_t folder_len);

/* Request paths the engine would ask the provider for when it loads the
 * logical (type, name). Lower-cased. Returns the number written (0..max).
 * A model name (.lwo, .ase, .ma, .obj, .fbx) is cooked/model/<stem>.bmodel,
 * then generated/model/<stem>.bmodel: the game ships its md6 collision
 * models (md6/.../collision/<x>.lwo) only under generated/model/, beside
 * their generated/cm/ collision. */
int  sh_mb_need_requests(const char *type, const char *name,
                         char out[][SH_MB_REQUEST_MAX + 1], int max);

/* The path or name with its first "mods/<salt>/" segment removed (at the
 * start or after a '/', key any non-empty segment, so both spellings of a key,
 * '-' in decl names and '_' in md6 paths, are cut): the installed name a
 * renamed copy most likely came from. Returns 1 and fills out (lower-cased)
 * when a segment was removed, 0 otherwise. */
int  sh_mb_strip_key(const char *in, char *out, size_t cap);

/* Tolerant decl reference scan (see modbundle.c for the keys). The callback
 * receives each reference; name is not NUL-terminated. */
typedef void (*sh_mb_need_fn)(void *context, const char *type,
                              const char *name, size_t name_len);
void sh_mb_scan_needs(const char *decl_type, const unsigned char *text, size_t len,
                      sh_mb_need_fn callback, void *context);

/* Bare (unquoted) words that name a mod resource ("mods/...") and contain
 * '-': the engine's breakable and material lexers split them. The callback
 * receives the 1-based line and the word (not NUL-terminated). Returns the
 * count. The builder refuses each one. */
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
/* The SPIR-V request the stage loader builds for a program name (a program
 * decl name, or a permutation decl's request path) and extension (".vspv",
 * ".fspv", ".cspv"): "generated/spirv/" + the name with its extension
 * replaced, a basename of 38 or more characters cut into 30-character
 * components. Returns 1, or 0 when it does not fit. */
int  sh_mb_spirv_request(const char *name, const char *ext, char *out, size_t cap);
/* generated/renderprogs/permutations/<program>_<16 lowercase hex>.decl:
 * returns 1 with the program name and the mask, else 0. */
int  sh_mb_rp_permutation(const char *request, char *program, size_t program_cap, char mask[17]);

/* SHA-256 as 64 lowercase hex characters. */
void sh_mb_sha256_hex(const void *data, size_t len, char out[SH_MB_HASH_HEX + 1]);
/* Hash a file (UTF-8 path). Returns 1 and sets *size. */
int  sh_mb_sha256_file(const char *path, unsigned long long *size,
                       char out[SH_MB_HASH_HEX + 1]);

/* The store tool's conversion only (src/store/store_old_ids.c): an earlier release's install state file
 * (.state.json), {"firstCause","installedAt","validated"}. */

int  sh_mb_state_parse(const char *text, size_t len, int *validated,
                       long long *installed_at, char *cause, size_t cause_cap);

/* Wide long-path form of a UTF-8 path (\\?\ prefixed when absolute), malloc'd.
 * Exposed for modstore. */
#ifdef _WIN32
#include <wchar.h>
wchar_t *sh_mb_wpath(const char *utf8);
char    *sh_mb_utf8(const wchar_t *wide);
/* Read a whole file (UTF-8 path) up to max bytes. malloc'd, NUL-terminated. */
unsigned char *sh_mb_read_file(const char *path, size_t max, size_t *out_len);
#endif

/* The product's user-data folder, %LOCALAPPDATA%\snapmap-plus, in UTF-8
 * (resolved through the wide API: the ANSI code page cannot spell every
 * profile path). Every store, prefab and conversion root derives from it;
 * modstore.h sh_ms_root is its mods\ folder. 1 on success. */
int sh_mb_user_root(char *out, size_t cap);

#endif /* SNAPMAP_PLUS_MODBUNDLE_H */
