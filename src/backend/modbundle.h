/* modbundle.h -- build, validate and read mod bundles (mod.json).
 *
 * A mod is a folder holding mod.json (or, before its first build, a
 * package.json) and an assets/ tree at exact engine request paths. Building it
 * hashes every file, names what each file provides, scans decl text for what
 * it needs, checks the namespace and structural rules, and produces the
 * canonical manifest whose SHA-256 is the bundle hash.
 *
 * mod.json (Contract 5) has two blocks in one JSON object. The author block is
 * every top-level key except "snapmap-plus": id, name, version, author,
 * homepage, requirements, shadows, stock (pointers the author declares), and
 * any other key (description, strings, hud, ...), which is kept and becomes
 * the manifest's "extra". The product block, the member "snapmap-plus", is the
 * whole built manifest and is written only by the product:
 *
 *   "snapmap-plus": {
 *     "format": 1, "key": "<the mod's key>", "build": 3, "bundle": "<sha256>", "built": <unix seconds>,
 *     "validated": true, "cause": "<first cause when not validated>",
 *     "origin": "install" | "download" | "migration" | "delivery" | "by-hand",
 *     "legacy": {...},                 converted first-generation packages only
 *     "classes": {"adds", "borrows", "edits", "replaces", "editor", "surfaces"},
 *     "editor": ["<request>", ...],    files classified as editor tweaks
 *     "registries": {"banks": ["<stock bank>", ...], "conductor": ["<encounter decl>", ...],
 *                    "strings": ["<key>", ...]},
 *     "edits": [{"file": "<request>", "stock": "<digest>", "counts": {...}, "ops": [...]}, ...],
 *                                      derived edits of decls at game names (decl_tree.h)
 *     "shadows": ["<request>", ...],
 *     "files": [{"path", "sha256", "size", "provides": {"type", "name"}, "needs": [...]}],
 *     "stock": [{"path", "sha256", "source", "provides", "needs", "inAssets": false}]
 *   }
 *
 * Additive mods (phase 1). The builder classifies every file from its path
 * and bytes (SH_MB_CLASS_*); "shadows" is derived from the classification
 * (every replacement and every editor tweak at a game name), and the
 * author keys "shadows", "stock" and "scope" are accepted and ignored with
 * one report line each, so the canonical manifest of an existing mod keeps
 * its text and hash. An author "stock" entry without a file under assets/
 * still builds a pointer ("inAssets": false) and is reported. The key is the
 * author "id" when present (deprecated), else the product block "key", else
 * the folder's own name (a folder of the store never reaches that: its first
 * build gives it a generated key, sh_ms_first_key); the canonical "id" is the key. "key", "classes",
 * "editor", "registries" and "edits" are derived facts outside the canonical text.
 * "origin" "by-hand" is "" in memory (a folder the author made). A grandfathered
 * bundle (a "legacy" record) keeps its declared shadows and every rule of
 * before; it is classified for reporting only.
 *
 * Writing the block never touches the author block: the file is spliced. The
 * old block member is cut out (from the end of the member before it to the
 * end of its value), and the new one is inserted after the last author member
 * as ",\n\n  \"snapmap-plus\": {...}" (CRLF when the file uses CRLF). Cutting
 * the block out of any written file therefore gives back the author's bytes
 * exactly (sh_mb_author_bytes). A file without an author text (a download, a
 * store upgrade of a laid-out folder) gets a synthesized author block. A
 * "legacy" object and its grandfathered "shadows" in the product block are
 * build inputs as if the author had written them (a converted package keeps
 * its own author keys). A stock pointer with "inAssets": false has no file
 * under assets/ (product-created folders never hold pointer bytes); a rebuild
 * keeps it from the block.
 *
 * The canonical manifest (below) is derived in memory, never stored: it is
 * the text uploaded to the store and the input of the bundle hash, unchanged
 * from Contract 1.
 *
 * Canonical manifest: UTF-8 JSON, object keys sorted by their UTF-8 bytes, no
 * whitespace, files sorted by path, "bundle" omitted. Strings escape only '"',
 * '\\' and control characters (\b \f \n \r \t, others \u00xx lowercase), so the
 * text equals JSON.stringify of the parsed value with sorted keys. Numbers are
 * plain integers. "build" is the product's content counter for the folder
 * (see sh_mb_build) and is part of the canonical text. Optional top-level keys (author, homepage, requirements,
 * shadows, extra) are omitted when absent or empty. A carried file has exactly
 * blob, needs (possibly []), path, provides, request and size.
 *
 * Stock pointers (file kind "stock"): a file may instead name installed game
 * content, {"needs","path","provides","request","stock":{"sha256","source"}},
 * with no blob and no size. "source" is the installed resource path (an
 * archive entry of gameresources or snap_gameresources, .patch or .resources,
 * or a loose file under the install's base\ or root), lower-cased with '/';
 * "sha256" is the SHA-256 of the installed bytes (64 lowercase hex). The
 * builder emits one when a file's bytes equal an installed entry, and an author
 * writes one in the descriptor's "stock" object:
 *   "stock": {"<request path>": {"source": "<installed path>", "sha256": "<hex, optional>"}}
 * Pointers are not counted in the size caps and the extension whitelist does
 * not apply to them; the store never receives their bytes, and a client whose
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

#define SH_MB_FORMAT          1
/* The builder's rules, written into the product block as "rules". A folder
 * whose block carries another number (or none: built by an earlier product)
 * is built again once by the rebuild rule, so a product update reclassifies
 * every installed folder; aliases keep the maps pinned to its earlier hash
 * working. Raise it when a build's classification, edits or manifest change. */
#define SH_MB_RULES           2
#define SH_MB_ID_MAX          64u
#define SH_MB_VERSION_MAX     32u
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
#define SH_MB_HASH_HEX        64u
/* mod.json "requirements" (sh_mb_requirements_canonical): any cvar, any
 * console line, within these bounds. The Worker's manifest check mirrors them. */
#define SH_MB_REQ_CVARS_MAX    32u     /* cvars one bundle may set */
#define SH_MB_REQ_NAME_MAX     64u     /* bytes of a cvar name */
#define SH_MB_REQ_VALUE_MAX    64u     /* bytes of a cvar value */
#define SH_MB_REQ_COMMANDS_MAX 16u     /* lines in each of activate and deactivate */
#define SH_MB_REQ_LINE_MAX     256u    /* bytes of one console line */
#define SH_MB_BLOCK_KEY       "snapmap-plus"   /* mod.json's product block */
/* Product block "origin": the product created the folder (a download that
 * lands later may replace it). A folder without one is the author's (a
 * download of its id sits beside it under <id>@<version>). Uninstall and
 * Remove delete either kind. */
#define SH_MB_ORIGIN_INSTALL   "install"        /* copied in by Install folder or sh_mods install */
#define SH_MB_ORIGIN_DOWNLOAD  "download"       /* fetched from the mod store */
#define SH_MB_ORIGIN_MIGRATION "migration"      /* converted from the retired overrides folder */
#define SH_MB_ORIGIN_DELIVERY  "delivery"       /* converted from a package a map delivered */

typedef struct sh_mb_need {
    char *type;            /* engine spelling: md6Def, baseModel, anim, material, ... */
    char *name;            /* logical name as the decl text spells it */
} sh_mb_need;

typedef struct sh_mb_file {
    char *path;            /* "assets/..." with '/', the author's spelling */
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
    int declared;          /* builder: named by the descriptor's stock object */
    int absent;            /* a stock pointer with no file under assets/ ("inAssets": false) */
    int cls;               /* SH_MB_CLASS_*: what the file is to the game (sh_mb_classify) */
    char *edit;            /* builder: a decl at a game name, its derived edits element (JSON), or NULL */
    int edited;            /* an edited copy: a REPLACE file the bundle's edits name (built or loaded); never served */
} sh_mb_file;

/* Per-file classification (DESIGN-additive 2.1, 2.6). Decided from the
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
 *            (put it under mods/<key>/), except in a folder the product
 *            received (sh_mb_keep_replacements_here). */
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

/* Where a bundle's key came from (sh_mb_bundle.key_source). */
#define SH_MB_KEY_FROM_ID     1   /* the author block's "id" (deprecated, kept as the key) */
#define SH_MB_KEY_FROM_BLOCK  2   /* the product block's "key" */
#define SH_MB_KEY_FROM_FOLDER 3   /* the folder's own name, at its first build */
#define SH_MB_KEY_SUFFIX      4u  /* random characters a generated key ends with */
/* "origin" the builder writes for a folder the author made ("" in memory). */
#define SH_MB_ORIGIN_BY_HAND  "by-hand"

/* A pointer an author wrote in the descriptor's "stock" object. */
typedef struct sh_mb_stock_decl {
    char *request;         /* lower-cased request path */
    char *source;          /* lower-cased installed path */
    char  sha256[SH_MB_HASH_HEX + 1];   /* "" when the author left it to the builder */
    int   carried;         /* carried forward from the product block ("inAssets": false), not the author's */
} sh_mb_stock_decl;

/* Where each need of a build was found. */
typedef struct sh_mb_need_stats {
    unsigned total, local, installed, stock, unresolved;
} sh_mb_need_stats;

typedef struct sh_mb_bundle {
    char  id[SH_MB_ID_MAX + 1];
    char  version[SH_MB_VERSION_MAX + 1];   /* the author's label, "0.0.0" when absent */
    unsigned long long build;               /* content counter, >= 1 */
    char *name, *author, *homepage;
    char *requirements;    /* canonical JSON object text, or NULL */
    char *extra;           /* canonical JSON object of preserved descriptor keys, or NULL */
    char **shadows;        /* sorted, unique, lower-cased request paths */
    size_t shadow_count;
    sh_mb_file *files;     /* sorted by path */
    size_t file_count, file_cap;
    unsigned long long total_size;   /* carried bytes: stock pointers are not counted */
    unsigned stock_count;  /* files that are stock pointers */
    unsigned long long stock_bytes;  /* their installed size (known when built, 0 when loaded) */
    sh_mb_stock_decl *stock_decls;   /* the descriptor's stock object */
    size_t stock_decl_count;
    char *canonical;       /* canonical manifest text (NUL-terminated) */
    size_t canonical_len;
    char  hash[SH_MB_HASH_HEX + 1];   /* SHA-256 of canonical */
    char  prev_hash[SH_MB_HASH_HEX + 1];   /* sh_mb_build: the bundle hash the folder's product
                                              block named before this build, "" when none */
    char  descriptor[16];  /* "mod.json", "package.json" or "bundle.json" (a manifest text) */
    int   legacy;          /* the descriptor (or the manifest's extra) carries a "legacy" object:
                              shadows need not be stock and unresolved needs only warn */
    int   block_legacy;    /* that object came from mod.json's product block, not the author block */
    int   delivered;       /* "legacy" without "source": "overrides" (the retired package folder): delivered by a map */
    long long built;       /* product block "built": when this bundle was first written in its folder */
    int   validated;       /* product block "validated" (1 after a build without errors) */
    int   rules;           /* product block "rules": the builder's SH_MB_RULES that wrote it; 0 when absent */
    char  state_cause[SH_MB_CAUSE_MAX];   /* product block "cause" when not validated */
    char  origin[16];      /* product block "origin": SH_MB_ORIGIN_*, "" for a folder the author made */
    sh_mb_need_stats needs;
    unsigned errors, warnings;
    char  first_cause[SH_MB_CAUSE_MAX];
    char  first_warning[SH_MB_CAUSE_MAX];
    /* Additive mods, phase 1. `id` above is the key: the namespace and the
     * canonical "id". */
    int   key_source;      /* SH_MB_KEY_FROM_*, 0 when unknown (a manifest) */
    sh_mb_classes classes; /* counts of the files' classes */
    char *registries;      /* compact JSON of the product block's "registries" object, or NULL */
    /* Derived edits (DESIGN-additive 2.4): the product block's "edits", a
     * JSON array with one element per decl the mod carries at a game name
     * with other bytes (decl_tree.h), or NULL; edit_files elements. */
    char *edits;
    unsigned edit_files;
    /* A descriptor's retired intent keys (extends, player.bodyEvents): the
     * partial decls they name and the game file each stood for, so a build
     * refuses the partial with what to put instead. */
    char **old_partials, **old_targets;
    size_t old_partial_count;
    char **notes;          /* builder: report lines (old keys ignored, replacements, aliases, strings) */
    size_t note_count, note_cap;
} sh_mb_bundle;

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
} sh_mb_stock_ops;

/* Register the installed-content reader (copied; NULL clears). Without one the
 * builder emits no pointers, a descriptor's stock object is refused, and stock
 * files cannot be verified or served. */
void sh_mb_set_stock_ops(const sh_mb_stock_ops *ops);
/* 1 when a reader is registered. */
int  sh_mb_stock_available(void);

/* The stock-bank question (modbankinfo.h sh_mbi_bundle_need): 1 with the stock
 * Wwise bank a sound decl named `decl_name` needs loaded on demand (a bank
 * SnapMap's game-mode filter skips, the only kind filing its event), else 0.
 * With one registered, a build lists each such bank once, in byte order, as
 * the product block's registries "banks" (DESIGN-additive 2.4, "Sound
 * events") and notes which decl needs it. NULL (the default) lists none. */
typedef int (*sh_mb_bank_need_fn)(const char *decl_name, char *bank, size_t cap);
void sh_mb_set_bank_need(sh_mb_bank_need_fn fn);
/* The registered operations, with a first cause naming `path` when not OK. */
int  sh_mb_stock_verify(const char *source, const char *sha256, unsigned long long *size,
                        const char *path, char *cause, size_t cause_cap);
/* The registered identify operation: OK with the installed entry's sha256 and size, ABSENT or UNAVAILABLE. */
int  sh_mb_stock_identify(const char *source, char sha256[SH_MB_HASH_HEX + 1], unsigned long long *size);
int  sh_mb_stock_read(const char *source, const char *sha256, unsigned char **bytes, size_t *len,
                      const char *path, char *cause, size_t cause_cap);

/* Validate a stock source path: the request-path rules without the extension
 * whitelist. 1 when valid. */
int  sh_mb_check_source(const char *source, char *cause, size_t cause_cap);

/* Build the bundle for `folder` (UTF-8 path). Returns 1 when the bundle has no
 * errors, 0 otherwise; out->first_cause and out->errors describe the refusal.
 * out is always initialised and must be released with sh_mb_free. Nothing is
 * written to disk. The build number comes from the folder's previous product
 * block: kept when the content (without build and bundle) is unchanged,
 * previous + 1 when it changed, 1 when there is none; "built" is kept while
 * the bundle hash is unchanged. Every file is classified (out->files[].cls,
 * out->classes); a file outside the namespace that is not a surface must be
 * a game name (resolver->stock), and is then a shadow: an editor tweak, or a
 * replacement: a decl's derived edits are recorded (out->edits), a binary
 * is refused. The key is resolved (out->key_source), out->registries
 * filled, and out->notes holds the
 * report lines the build log prints. */
int  sh_mb_build(const char *folder, const sh_mb_resolver *resolver, sh_mb_bundle *out);

/* Write `<folder>\mod.json`: the author block (the folder's mod.json without
 * its product block, else its package.json, which is then deleted, else a
 * synthesized one) and the product block from `bundle`, including its
 * validated, cause and built fields. A pointer whose file is absent from
 * `<folder>\assets\` is written "inAssets": false. Write-through temporary
 * and rename. Refuses a bundle with errors. Returns 1 on success. */
int  sh_mb_write(const sh_mb_bundle *bundle, const char *folder, char *cause, size_t cause_cap);

/* Give a built bundle another build number and recompute its canonical text
 * and hash (an install continuing the counter of the copy it replaces). */
void sh_mb_renumber(sh_mb_bundle *bundle, unsigned long long build);

/* Record a verification result in `<folder>\mod.json`'s product block
 * (validated, cause) without changing anything else. Returns 1 on success. */
int  sh_mb_set_state(const char *folder, int validated, const char *cause, char *why, size_t why_cap);

/* The complete mod.json text for `bundle` (malloc'd), splicing the product
 * block into `author_text` (a mod.json or package.json text, its own block
 * ignored) or, when author_text is NULL, into a synthesized author block. */
char *sh_mb_modjson_text(const sh_mb_bundle *bundle, const char *author_text, size_t author_len, size_t *out_len);

/* The author's bytes of a mod.json text: the text without its "snapmap-plus"
 * member (malloc'd). *had_block (optional) says whether there was one.
 * NULL when the text is not a JSON object. */
char *sh_mb_author_bytes(const char *text, size_t len, size_t *out_len, int *had_block);

/* The bundle.json form (pretty-printed, with "bundle"), as the retired
 * layout and earlier exports stored it (malloc'd, NUL-terminated), or NULL.
 * Exports now carry mod.json itself (modexport.h). */
char *sh_mb_bundle_json(const sh_mb_bundle *bundle, size_t *out_len);

/* Read and check a built mod.json: the author block, the product block,
 * structure, id, version, paths, logical names and the bundle hash recomputed
 * from the canonical form. Files on disk are not read. Returns 1 when valid;
 * a mod.json without a product block fails with "not built yet". */
int  sh_mb_load(const char *mod_json_path, sh_mb_bundle *out);
int  sh_mb_load_text(const char *text, size_t len, sh_mb_bundle *out);
/* A built mod.json that fails sh_mb_load_text's check only because an earlier
 * builder's rules wrote its block ("rules" absent or below SH_MB_RULES): 1 with
 * the bundle as today's rules read it (out->hash) and the hash its block
 * records (out->prev_hash); 0 for anything else, with the cause. */
int  sh_mb_load_text_earlier_rules(const char *text, size_t len, sh_mb_bundle *out);
/* `<folder>\mod.json`. */
int  sh_mb_load_folder(const char *folder, sh_mb_bundle *out);

/* Read and check a manifest in the bundle.json form (Contract 1, with
 * "bundle"): a download, a folder of the retired layout. */
int  sh_mb_load_manifest_text(const char *text, size_t len, sh_mb_bundle *out);

/* Tree hash of a mod folder, the rebuild rule's key: SHA-256 over the
 * author's bytes of mod.json (else package.json), then every file under
 * assets/ sorted by path with its size and last-write time. Cheap: no file is
 * read. Returns 1 and the hex. */
int  sh_mb_tree_hash(const char *folder, char out[SH_MB_HASH_HEX + 1]);

/* What a bundle serves, independent of its label: SHA-256 over id,
 * requirements, shadows, extra without "legacy", and every file's request,
 * kind, hash and size. Two bundles with one key serve the same bytes under
 * the same names. */
void sh_mb_content_key(const sh_mb_bundle *bundle, char out[SH_MB_HASH_HEX + 1]);

/* A folder name under `mods_root` for a product-created folder of this
 * bundle: "<id>", else "<id>@<version>", else "<id>@<version>-<hash 8>", else
 * "<id>@<version>-<hash 16>". A generated version (sh_mb_version_generated)
 * never appears in a name: "<id>", else "<id>-<hash 8>", else "<id>-<hash 16>".
 * Returns 1 with a free name, 2 when that folder already holds this bundle
 * hash, 0 when none is available. */
int  sh_mb_folder_name(const char *mods_root, const char *id, const char *version, const char *hash,
                       char *out, size_t cap);

/* A version the converter generated for a package a map delivered without
 * one ("legacy-<digest16>"): part of the manifest and its hash, never shown
 * and never part of a folder name. */
int  sh_mb_version_generated(const char *version);

/* Whether the bundle is a package a map delivered: its legacy record has no
 * overrides source, or its product block's origin is "delivery". */
int  sh_mb_is_delivered(const sh_mb_bundle *bundle);

/* Whether every request path of `part` (at least one) is one of `big`'s. */
int  sh_mb_requests_cover(const sh_mb_bundle *big, const sh_mb_bundle *part);

/* ------------------------------------------------- the product record ---- */
/* <mods_root>\snapmap-plus.json, the product's one file under mods\:
 *   {"format": 1, "claims": {...}, "links": {...}, "pending": {...},
 *    "migration": {...}, "cache": {"<tree hash>": "<bundle hash>"},
 *    "aliases": {"<bundle hash>": "<bundle hash>"}}
 * Each section is owned by one module and read and written whole. Writes are
 * a read, a replace of that section and a write-through rename, under one
 * recursive lock (sh_mb_record_lock) that a caller may also hold around its
 * own read-modify-write. A file another program holds is waited for, up to a
 * second. A file that is not JSON is moved aside when a section is written,
 * logged with the aside file's name, and a new one started. A file that
 * cannot be read is never replaced. */
#define SH_MB_RECORD_FILE "snapmap-plus.json"
void sh_mb_record_lock(void);
void sh_mb_record_unlock(void);
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
 * then the player's game archives) and write its bundle.json: the entry the
 * legacy converter (modlegacy.c) calls for a converted package. Returns 1 on
 * success; 0 with the first cause in `first_cause` (NUL-terminated, at most
 * n bytes). Defined in modcmd.c, which owns the resolver; logs MODS: lines. */
int  modbundle_build_folder(const char *folder, char *first_cause, size_t n);

/* ----------------------------------------------------------- helpers ---- */

/* A key for a new mod named `name`: a slug of the name (lower-case letters
 * and digits, other runs as one '_', at most 59 characters, "mod" when
 * nothing is left) plus '_' and SH_MB_KEY_SUFFIX random characters of
 * [a-z0-9] ("hell_guards_7f3k"). Never '-': a bare decl word ends at '-', and
 * '_' is the engine's cooked spelling, so the key reads the same in decl
 * text and binary paths. Returns 1 with a valid id in out. */
int  sh_mb_key_generate(const char *name, char out[SH_MB_ID_MAX + 1]);

/* Hand-made folders (DESIGN-additive 2.2, OWNER-QUESTIONS 6 (b)). The first
 * build of a store folder whose descriptor has neither an author "id" nor a
 * product-block "key" gives it a generated key, renames the folder to it and
 * rewrites the author's own references once (sh_ms_first_key, modstore.c);
 * these are the pure parts. */

/* The key for a folder named `leaf`: sh_mb_key_generate of the leaf, after
 * dropping a trailing salt when the leaf already has a generated key's shape
 * ("probe_7f3k" or "probe-7f3k", the tail not all digits), so a copied key
 * gets a new salt instead of a second one. */
int  sh_mb_key_for_folder(const char *leaf, char out[SH_MB_ID_MAX + 1]);

/* SH_MB_KEY_FROM_ID when the descriptor (mod.json, else package.json) has an
 * author "id", SH_MB_KEY_FROM_BLOCK when its product block has a "key" (or
 * the block is not an object, which the build refuses), 0 when it has
 * neither, -1 when there is no readable JSON-object descriptor with a string
 * "name" (a damaged descriptor is refused by the build, never renamed). */
int  sh_mb_descriptor_key_state(const char *folder);

/* Every reference to mods/<folder> in `text` (n bytes), as written or in the
 * engine's cooked spelling (mb_binary_name), in any case, not inside a longer
 * name, rewritten to mods/<key>. Returns the count; with `out`, the rewritten
 * text (malloc'd, NUL-terminated, NULL when the count is 0) and its length;
 * (size_t)-1 when out of memory. Without `out` it only counts. */
size_t sh_mb_rekey_text(const char *text, size_t n, const char *folder, const char *key, char **out, size_t *out_len);

/* `rel` ('/'-separated) with every folder segment right after a "mods"
 * segment that is `folder` (as written or cooked, any case) replaced by
 * `key`. 1 when changed, 0 when not, -1 when out is too small. */
int  sh_mb_rekey_path(const char *rel, const char *folder, const char *key, char *out, size_t cap);

/* A mod.json text (or package.json) with its product block replaced by the
 * one New mod writes: {"format": 1, "key": <key>, "origin": <origin, NULL
 * for "by-hand">}. The author's bytes are kept exactly. malloc'd, or NULL
 * when not a JSON object. */
char *sh_mb_rekey_modjson(const char *text, size_t n, const char *key, const char *origin, size_t *out_len);

/* The classes counts as one line ("2 adds, 0 borrows, 1 edits, 0 replaces, 0 editor, 0 surfaces"). */
void sh_mb_classes_text(const sh_mb_classes *c, char *out, size_t cap);

/* For the calling thread only: 1 accepts a binary at a game name in the
 * builds it runs (an install of content the product received, such as a
 * rawmap or prefab import, is never refused on its files); 0 ends it. A
 * folder whose product block origin is download, delivery or migration is
 * accepted the same way. Every other build refuses such a file
 * (DESIGN-additive 2.4). Thread-local, so a worker's import never changes a
 * rescan's rule. */
void sh_mb_keep_replacements_here(int on);

/* Trim ASCII whitespace, lower-case, and validate a mod id: 1-64 characters of
 * [a-z0-9._-], starting and ending with a letter or digit, no "..".
 * Returns 1 and writes out on success. */
int  sh_mb_id_normalize(const char *in, size_t len, char out[SH_MB_ID_MAX + 1]);

/* The id as it appears inside an engine-derived binary name: the engine's name
 * sanitiser turns every character outside [A-Za-z0-9$./\\] into '_'. */
void sh_mb_id_sanitized(const char *id, char out[SH_MB_ID_MAX + 1]);

/* Validate a version string: 1-32 code points, none of / \\ : * ? " < > | or
 * control characters, not ending in '.', no leading or trailing space. */
int  sh_mb_version_valid(const char *version);

/* Validate one request path (relative, '/', no "." or ".." segment, allowed
 * characters [A-Za-z0-9._-$+@~()] and internal spaces, no segment that
 * starts with a space or ends in '.' or a space, at most
 * SH_MB_REQUEST_MAX bytes). 1 when valid, else 0 with the cause. */
int  sh_mb_check_request(const char *request, char *cause, size_t cause_cap);

/* Extension whitelist (Contract 1). A .bswf movie is admitted only under
 * generated/swf/. */
int  sh_mb_extension_allowed(const char *request);

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

/* Game files several mods commonly edit (DESIGN-additive 2.4): the product
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

/* Logical name of a request path (Contract 1 table). Returns 1 and fills type
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

/* The path or name with its first "mods/<key>/" segment removed (at the
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

/* The retired install state file (.state.json) of the old layout, for the
 * store upgrade: {"firstCause","installedAt","validated"}. */
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
