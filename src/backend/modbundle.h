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
 *     "format": 1, "build": 3, "bundle": "<sha256>", "built": <unix seconds>,
 *     "validated": true, "cause": "<first cause when not validated>",
 *     "origin": "install" | "download" | "migration" | "delivery",   absent for the author's own folder
 *     "legacy": {...},                 converted first-generation packages only
 *     "shadows": ["<request>", ...],
 *     "files": [{"path", "sha256", "size", "provides": {"type", "name"}, "needs": [...]}],
 *     "stock": [{"path", "sha256", "source", "provides", "needs", "inAssets": false}]
 *   }
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
#define SH_MB_BLOCK_KEY       "snapmap-plus"   /* mod.json's product block */
/* Product block "origin": the product created the folder (a copy it may
 * delete on Remove). A folder without one is the author's (Remove keeps its
 * files, moved to removed-mods). */
#define SH_MB_ORIGIN_INSTALL   "install"        /* copied in by Install folder or sh_mods install */
#define SH_MB_ORIGIN_DOWNLOAD  "download"       /* fetched from the mod store or a rawmap export */
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
} sh_mb_file;

/* A pointer an author wrote in the descriptor's "stock" object. */
typedef struct sh_mb_stock_decl {
    char *request;         /* lower-cased request path */
    char *source;          /* lower-cased installed path */
    char  sha256[SH_MB_HASH_HEX + 1];   /* "" when the author left it to the builder */
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
    char  descriptor[16];  /* "mod.json", "package.json" or "bundle.json" (a manifest text) */
    int   legacy;          /* the descriptor (or the manifest's extra) carries a "legacy" object:
                              shadows need not be stock and unresolved needs only warn */
    int   block_legacy;    /* that object came from mod.json's product block, not the author block */
    int   delivered;       /* "legacy" without "source": "overrides" (the retired package folder): delivered by a map */
    long long built;       /* product block "built": when this bundle was first written in its folder */
    int   validated;       /* product block "validated" (1 after a build without errors) */
    char  state_cause[SH_MB_CAUSE_MAX];   /* product block "cause" when not validated */
    char  origin[16];      /* product block "origin": SH_MB_ORIGIN_*, "" for a folder the author made */
    sh_mb_need_stats needs;
    unsigned errors, warnings;
    char  first_cause[SH_MB_CAUSE_MAX];
    char  first_warning[SH_MB_CAUSE_MAX];
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
/* The registered operations, with a first cause naming `path` when not OK. */
int  sh_mb_stock_verify(const char *source, const char *sha256, unsigned long long *size,
                        const char *path, char *cause, size_t cause_cap);
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
 * the bundle hash is unchanged. */
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

/* The bundle.json form (pretty-printed, with "bundle"): what a rawmap export
 * ZIP carries (malloc'd, NUL-terminated), or NULL. */
char *sh_mb_bundle_json(const sh_mb_bundle *bundle, size_t *out_len);

/* Read and check a built mod.json: the author block, the product block,
 * structure, id, version, paths, logical names and the bundle hash recomputed
 * from the canonical form. Files on disk are not read. Returns 1 when valid;
 * a mod.json without a product block fails with "not built yet". */
int  sh_mb_load(const char *mod_json_path, sh_mb_bundle *out);
int  sh_mb_load_text(const char *text, size_t len, sh_mb_bundle *out);
/* `<folder>\mod.json`. */
int  sh_mb_load_folder(const char *folder, sh_mb_bundle *out);

/* Read and check a manifest in the bundle.json form (Contract 1, with
 * "bundle"): a download, a rawmap ZIP, a folder of the retired layout. */
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
 * "<id>@<version>-<hash 16>". Returns 1 with a free name, 2 when that folder
 * already holds this bundle hash, 0 when none is available. */
int  sh_mb_folder_name(const char *mods_root, const char *id, const char *version, const char *hash,
                       char *out, size_t cap);

/* ------------------------------------------------- the product record ---- */
/* <mods_root>\snapmap-plus.json, the product's one file under mods\:
 *   {"format": 1, "claims": {...}, "links": {...}, "pending": {...},
 *    "migration": {...}, "cache": {"<tree hash>": "<bundle hash>"},
 *    "aliases": {"<bundle hash>": "<bundle hash>"}}
 * Each section is owned by one module and read and written whole. Writes are
 * a read, a replace of that section and a write-through rename, under one
 * recursive lock (sh_mb_record_lock) that a caller may also hold around its
 * own read-modify-write. An unreadable file is moved aside, logged, and a new
 * one started. */
#define SH_MB_RECORD_FILE "snapmap-plus.json"
void sh_mb_record_lock(void);
void sh_mb_record_unlock(void);
/* 1 with the section's JSON text (malloc'd, compact), 0 when absent, -1 when
 * the record cannot be read. */
int  sh_mb_record_get(const char *mods_root, const char *section, char **json_out);
/* Replace (json != NULL) or remove (NULL) one section. 1 on success. */
int  sh_mb_record_put(const char *mods_root, const char *section, const char *json);

void sh_mb_free(sh_mb_bundle *bundle);

/* Build `folder` in place with the game-backed resolver (installed bundles,
 * then the player's game archives) and write its bundle.json: the entry the
 * legacy converter (modlegacy.c) calls for a converted package. Returns 1 on
 * success; 0 with the first cause in `first_cause` (NUL-terminated, at most
 * n bytes). Defined in modcmd.c, which owns the resolver; logs MODS: lines. */
int  modbundle_build_folder(const char *folder, char *first_cause, size_t n);

/* ----------------------------------------------------------- helpers ---- */

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
 * logical (type, name). Lower-cased. Returns the number written (0..max). */
int  sh_mb_need_requests(const char *type, const char *name,
                         char out[][SH_MB_REQUEST_MAX + 1], int max);

/* Tolerant decl reference scan (see modbundle.c for the keys). The callback
 * receives each reference; name is not NUL-terminated. */
typedef void (*sh_mb_need_fn)(void *context, const char *type,
                              const char *name, size_t name_len);
void sh_mb_scan_needs(const char *decl_type, const unsigned char *text, size_t len,
                      sh_mb_need_fn callback, void *context);

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

#endif /* SNAPMAP_PLUS_MODBUNDLE_H */
