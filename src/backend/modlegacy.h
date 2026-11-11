/* Legacy first-generation carriers (beta.6 through beta.19), for the store
 * tool only (src\store\build.ps1; never linked into the game DLL, which
 * refuses such a map: map_carrier.h).
 *
 * A complete "smpkg." set whose ZIP decodes and whose package.json parses is
 * CONVERTED into the one shape: its tree is written to a new mod folder
 * <data>\mods\<id>\ (else "<id> (2)", ...) with the era B layout migrated to
 * assets/ (decls/ -> assets/generated/decls, images/ ->
 * assets/generated/image, shaders/generated/{spirv,renderprogs}/ ->
 * assets/generated/..., manifests, requirements, strings and
 * hud/weapons.json folded into mod.json's author part, which keeps name,
 * version, description, author, homepage, requirements, strings, hud and the
 * package's other keys except the retired ones: no id, no product block, no
 * legacy record), the folder takes a salt (sh_ms_assign_salt, from the
 * package id), the bundle builder builds it under the current rules, and one
 * manifest row "<salt>@<build>" per converted bundle is written into the
 * text (the member's "mods" part, map_carrier.h). A conversion whose files
 * an installed mod already serves is not kept; that mod answers the
 * carrier. Anything else (also a package the current rules refuse) is
 * CLEANED: stripped, with the key, digest and reason logged. smpbind. and
 * smpref. rows (never released) are stripped silently. Every carrier gets
 * one "LEGACY:" log line.
 *
 * A file under assets/ that a bundle cannot carry (a type outside the
 * Contract 1 whitelist, an empty file, a request path the builder refuses)
 * is left out and counted rather than refusing the package; so is a cvar
 * requirement other than the supported blacklist gates.
 */
#ifndef SNAPMAP_PLUS_MODLEGACY_H
#define SNAPMAP_PLUS_MODLEGACY_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include "map_legacy_payload.h"
#include "map_shards.h"
#include "modmanifest.h"

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/* Contract 1 caps, applied while reading the ZIP. */
#define SH_MODLEGACY_MAX_FILES       4096u
#define SH_MODLEGACY_MAX_FILE_BYTES  (64ull << 20)
#define SH_MODLEGACY_MAX_TREE_BYTES  (256ull << 20)
/* The beta.19 writer's representation bounds. */
#define SH_MODLEGACY_MAX_PAYLOAD     (((size_t)0x7fffffff / 4) * 3)
#define SH_MODLEGACY_MAX_SHARDS      (0x7fffffff / 8192 + 1)

enum { SH_MODLEGACY_CONVERT = 1, SH_MODLEGACY_CLEAN = 2 };

/* A first-generation package id: 1 to 64 characters. */
#define SH_MODLEGACY_ID_CAP 65

typedef struct sh_modlegacy_verdict {
    char key[SH_LEGACY_KEY_CAP];
    char digest[17];
    int key_form;
    unsigned total, present;
    int verdict;                       /* SH_MODLEGACY_CONVERT or SH_MODLEGACY_CLEAN */
    char reason[256];                  /* why it was cleaned; empty for a conversion */
    char layout;                       /* 'B' or 'C' once the ZIP was read, else 0 */
    char id[SH_MODLEGACY_ID_CAP];      /* the package's id (its old key) */
    char salt[SH_MODMANIFEST_SALT_CAP];   /* the converted folder's salt */
    char build[SH_MODMANIFEST_HASH_CAP];  /* its build hash */
    unsigned long long size;
    unsigned files, shadows;
    unsigned skipped;                  /* files under assets/ a bundle cannot carry, left out */
    char first_skipped[256];           /* the first of them and why */
    unsigned movies;                   /* of them, .bswf movies */
    char folder[MAX_PATH * 4];         /* the converted bundle's folder (UTF-8) */
} sh_modlegacy_verdict;

/* --------------------------------------------------------- carriers ----- */

typedef struct sh_modlegacy_carrier {
    sh_legacy_header header;           /* key, digest and total of the first shard seen */
    unsigned present;                  /* distinct in-range shards with a readable value */
    int consistent;                    /* every shard agreed on total; no duplicate or bad index */
    char problem[128];                 /* the first inconsistency */
    sh_shard_chunk *chunks;            /* `header.total` slots; malloc'd */
} sh_modlegacy_carrier;

/* Group every smpkg shard of `json` by (key, digest16). Returns the number of
 * carriers in *out (free with sh_modlegacy_carriers_free). */
size_t sh_modlegacy_collect(const char *json, size_t length, sh_modlegacy_carrier **out);
void   sh_modlegacy_carriers_free(sh_modlegacy_carrier *carriers, size_t count);

/* The verified ZIP of a carrier: complete (index 0..total-1 each once),
 * strict base64, digest16 equal to the first 16 hex of SHA-256 of the bytes.
 * Returns a process-heap buffer (HeapFree), or NULL with the reason. */
unsigned char *sh_modlegacy_payload(const sh_modlegacy_carrier *carrier, size_t *out_length,
                                    char *reason, size_t reason_cap);

/* --------------------------------------------------------- ZIP ---------- */

typedef struct sh_modlegacy_zip_entry {
    char *name;                         /* as stored; directories keep no trailing '/' */
    unsigned method, crc;
    uint64_t size, packed;
    const unsigned char *data;
    int directory;
    wchar_t *source;                    /* a folder tree's file (long-path form), read at
                                           sh_modlegacy_zip_read; NULL for a ZIP member */
} sh_modlegacy_zip_entry;

typedef struct sh_modlegacy_zip {
    sh_modlegacy_zip_entry *entries;
    size_t count;
    unsigned files;
    uint64_t expanded;
} sh_modlegacy_zip;

/* Read the central directory (stored and deflate members, ZIP64 records).
 * Refuses encryption, other methods, unsafe names and the Contract 1 caps. */
int  sh_modlegacy_zip_open(const unsigned char *bytes, size_t length, sh_modlegacy_zip *zip,
                           char *err, size_t err_cap);
/* One member's bytes, inflated and CRC-checked; malloc'd (free), NUL-terminated. */
unsigned char *sh_modlegacy_zip_read(const sh_modlegacy_zip_entry *entry, size_t *out_length,
                                     char *err, size_t err_cap);
/* The member called `name` (exact), or NULL. */
const sh_modlegacy_zip_entry *sh_modlegacy_zip_find(const sh_modlegacy_zip *zip, const char *name);
void sh_modlegacy_zip_free(sh_modlegacy_zip *zip);

/* --------------------------------------------------------- layout ------- */

enum {
    SH_MODLEGACY_PATH_DROP = 0,        /* not an input: not kept */
    SH_MODLEGACY_PATH_ASSET,           /* written to `target` */
    SH_MODLEGACY_PATH_DESCRIPTOR,      /* package.json */
    SH_MODLEGACY_PATH_MANIFEST,        /* resources/<name>.manifest */
    SH_MODLEGACY_PATH_REQUIREMENTS,    /* requirements/<name>.requirements */
    SH_MODLEGACY_PATH_STRINGS,         /* strings/<name>.json */
    SH_MODLEGACY_PATH_HUD              /* hud/weapons.json */
};

/* Map one path relative to a package root to the era C layout. `target`
 * receives the destination path under the bundle folder for an ASSET. */
int sh_modlegacy_map_path(const char *relative, char *target, size_t target_cap);

/* 1 when `path` is safe to create under a folder on Windows: relative,
 * '/'-separated, no empty, ".", ".." or reserved-device segment, no
 * character Windows refuses. */
int sh_modlegacy_safe_path(const char *path);

/* Normalize a candidate bundle id (trim, lowercase, BOM); 1 when the result
 * is a valid id per Contract 1. */
int sh_modlegacy_normalize_id(const char *text, char *out, size_t cap);

/* --------------------------------------------------------- conversion --- */

/* --------------------------------------------------------- folders ------ */

/* A first-generation tree already on disk (a retired overrides\ folder),
 * read as if it were a stored ZIP: every file and folder under `folder`
 * (UTF-8, long paths allowed), names relative and '/'-separated. Refuses
 * links, unusable names and more than `max_files` files or `max_bytes`
 * bytes. When `tree_hex` is given it receives the beta.16-19 tree
 * fingerprint (beta.19 package_sources.c: entries sorted case-insensitively,
 * then by bytes; per entry a 'd' or 'f' byte, the NUL-terminated relative
 * path and, for a file, its SHA-256), which is exactly the 64 hex of the
 * "smpkg.delivery.<tree>" key a map carries for the same tree. */
int sh_modlegacy_zip_from_folder(const char *folder, unsigned max_files, unsigned long long max_bytes,
                                 sh_modlegacy_zip *zip, char tree_hex[65], char *err, size_t err_cap);

/* The tree fingerprint of a tree read with sh_modlegacy_zip_from_folder
 * (hashes every file). Returns 1. */
int sh_modlegacy_zip_tree(const sh_modlegacy_zip *zip, char tree_hex[65], char *err, size_t err_cap);

/* The bytes (and files) under the tree's top-level assets/ (or era B
 * decls/, images/, shaders/) of a type a bundle carries: a lower bound of
 * what a conversion would hold, for refusing an over-cap tree before any
 * hashing. */
unsigned long long sh_modlegacy_zip_carried_bytes(const sh_modlegacy_zip *zip, unsigned *files);

/* Where a converted folder came from (the migration of overrides\). */
typedef struct sh_modlegacy_origin {
    const char *source;                /* the migration source (logged) */
    const char *folder;                /* overrides-relative folder, '/'-separated */
    const char *tree;                  /* 64 hex tree fingerprint, or NULL */
    const char *fallback_id;           /* the id when the descriptor names none (era B) */
    /* The builder for sh_modlegacy_convert_folder; NULL: modbundle_build_folder. */
    int (*build)(const char *folder_utf8, char *first_cause, size_t n);
} sh_modlegacy_origin;

/* Convert a delivered beta.16-19 package folder, read with
 * sh_modlegacy_zip_from_folder, into a new salted and built mod folder
 * <data>\mods\<id>\ (as for a carrier). An installed mod that already serves
 * the same files answers instead. Fills `verdict`; returns 1 for CONVERT. */
int sh_modlegacy_convert_folder(const sh_modlegacy_zip *zip, const char *tree_hex,
                                const sh_modlegacy_origin *origin, sh_modlegacy_verdict *verdict);

/* Write an authored first-generation folder, read with
 * sh_modlegacy_zip_from_folder, as a new mod folder at `dest` (which must
 * not exist): mod.json (the descriptor migrated as for a carrier: the
 * author's part only, no id and no retired key) and assets/ in the era C
 * layout. Neither salted nor built. Fills verdict->id (the package id, from
 * which its salting rewrites mods/<id>/), files and skipped;
 * returns 1 on success, 0 with verdict->reason. */
int sh_modlegacy_stage_folder(const sh_modlegacy_zip *zip, const sh_modlegacy_origin *origin,
                              const char *dest, sh_modlegacy_verdict *verdict);

/* The DeserializeFromJson step: convert or clean every legacy carrier, strip
 * smpkg./smpbind./smpref. rows, and write manifest rows for the converted
 * bundles into the member. Returns the new buffer (HeapFree), or NULL to keep `json` (no
 * legacy rows, or a structure that does not read; logged). */
char *sh_modlegacy_process(const char *json, size_t length, size_t *out_length);

/* The verdicts of the last processed map, for the load gate. Copies at most
 * `cap` and returns the total. */
size_t sh_modlegacy_last_report(sh_modlegacy_verdict *out, size_t cap);

/* How a converted folder takes its salt: sh_ms_assign_salt's signature
 * (the store tool and the overrides migration set modstore.c's). Without
 * one every conversion is cleaned with "no salting is set". */
typedef int (*sh_modlegacy_salt_fn)(const char *folder, const char *from, char salt_out[9], char *cause, size_t cause_cap);
void sh_modlegacy_set_salter(sh_modlegacy_salt_fn fn);

/* The data root (default %LOCALAPPDATA%\snapmap-plus); NULL restores the
 * default. Paths are UTF-8, as the store spells them (sh_mb_user_root). */
void sh_modlegacy_set_data_root(const char *root);
/* Forget the per-process conversion memo (tests). */
void sh_modlegacy_reset(void);

#endif /* SNAPMAP_PLUS_MODLEGACY_H */
