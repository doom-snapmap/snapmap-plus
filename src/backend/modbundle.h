/* modbundle.h -- build, validate and read mod bundle manifests (bundle.json).
 *
 * A mod is a folder holding one descriptor (mod.json or package.json) and an
 * assets/ tree at exact engine request paths. Building it hashes every file,
 * names what each file provides, scans decl text for what it needs, checks the
 * namespace and structural rules, and produces the canonical manifest whose
 * SHA-256 is the bundle hash.
 *
 * Canonical manifest: UTF-8 JSON, object keys sorted by their UTF-8 bytes, no
 * whitespace, files sorted by path, "bundle" omitted. Strings escape only '"',
 * '\\' and control characters (\b \f \n \r \t, others \u00xx lowercase), so the
 * text equals JSON.stringify of the parsed value with sorted keys. Numbers are
 * plain integers. "build" is the product's content counter for the folder
 * (see sh_mb_build) and is part of the canonical text. Optional top-level keys (author, homepage, requirements,
 * shadows, extra) are omitted when absent or empty; every file always carries
 * blob, needs (possibly []), path, provides, request and size.
 *
 * Pure file and text work: no engine calls, no allocation on the engine heap.
 * Stock and installed resources are consulted through caller callbacks.
 */
#ifndef SNAPMAP_PLUS_MODBUNDLE_H
#define SNAPMAP_PLUS_MODBUNDLE_H

#include <stddef.h>

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
    int shadowed;          /* request path listed in shadows */
} sh_mb_file;

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
    unsigned long long total_size;
    char *canonical;       /* canonical manifest text (NUL-terminated) */
    size_t canonical_len;
    char  hash[SH_MB_HASH_HEX + 1];   /* SHA-256 of canonical */
    char  descriptor[16];  /* "mod.json", "package.json" or "bundle.json" */
    int   legacy;          /* the descriptor (or bundle.json extra) carries a "legacy" object:
                              shadows need not be stock and unresolved needs only warn */
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

/* Build the bundle for `folder` (UTF-8 path). Returns 1 when the bundle has no
 * errors, 0 otherwise; out->first_cause and out->errors describe the refusal.
 * out is always initialised and must be released with sh_mb_free. Nothing is
 * written to disk. The build number comes from the folder's previous
 * bundle.json: kept when the content (without build and bundle) is unchanged,
 * previous + 1 when it changed, 1 when there is none. */
int  sh_mb_build(const char *folder, const sh_mb_resolver *resolver, sh_mb_bundle *out);

/* Write `<folder>\bundle.json` (pretty-printed, with "bundle") and
 * `<folder>\bundle.canonical.json` (the exact canonical bytes, without
 * "bundle", whose SHA-256 is the bundle hash: the body the store receives).
 * Refuses a bundle with errors. Returns 1 on success. */
int  sh_mb_write(const sh_mb_bundle *bundle, const char *folder, char *cause, size_t cause_cap);

/* The pretty-printed bundle.json text (malloc'd, NUL-terminated), or NULL. */
char *sh_mb_bundle_json(const sh_mb_bundle *bundle, size_t *out_len);

/* Read and check a bundle.json: structure, id, version, paths, logical names
 * and the bundle hash recomputed from the canonical form. Files on disk are not
 * read. Returns 1 when valid. */
int  sh_mb_load(const char *bundle_json_path, sh_mb_bundle *out);
int  sh_mb_load_text(const char *text, size_t len, sh_mb_bundle *out);

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

/* Extension whitelist (Contract 1). */
int  sh_mb_extension_allowed(const char *request);

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

/* SHA-256 as 64 lowercase hex characters. */
void sh_mb_sha256_hex(const void *data, size_t len, char out[SH_MB_HASH_HEX + 1]);
/* Hash a file (UTF-8 path). Returns 1 and sets *size. */
int  sh_mb_sha256_file(const char *path, unsigned long long *size,
                       char out[SH_MB_HASH_HEX + 1]);

/* Install state file (.state.json) text: {"firstCause","installedAt","validated"}. */
char *sh_mb_state_format(int validated, const char *first_cause, long long installed_at);
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
