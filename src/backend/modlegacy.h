/* Legacy first-generation carriers (beta.6 through beta.19) under the
 * DESIGN.md section 11 policy.
 *
 * A complete "smpkg." set whose ZIP decodes and whose package.json parses is
 * CONVERTED: its tree is written to <data>\mods\legacy\<id>@<digest16>\ with
 * the era B layout migrated to assets/ (decls/ -> assets/generated/decls,
 * images/ -> assets/generated/image, shaders/generated/{spirv,renderprogs}/ ->
 * assets/generated/..., manifests, requirements, strings and hud/weapons.json
 * folded into the descriptor), the bundle builder writes bundle.json, and one
 * smpmod1 row per converted bundle is written into the text. Anything else is
 * CLEANED: stripped, with the key, digest and reason logged and kept for the
 * load gate. smpbind./smpref. rows (never released) are stripped silently.
 * Every carrier gets one "LEGACY:" log line. A published map by another
 * author takes the same path; the bundle stays local and nothing re-saves.
 */
#ifndef SNAPMAP_PLUS_MODLEGACY_H
#define SNAPMAP_PLUS_MODLEGACY_H

#include <stddef.h>
#include <stdint.h>
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

typedef struct sh_modlegacy_verdict {
    char key[SH_LEGACY_KEY_CAP];
    char digest[17];
    int key_form;
    unsigned total, present;
    int verdict;                       /* SH_MODLEGACY_CONVERT or SH_MODLEGACY_CLEAN */
    char reason[256];                  /* why it was cleaned; empty for a conversion */
    char layout;                       /* 'B' or 'C' once the ZIP was read, else 0 */
    char id[SH_MODMANIFEST_ID_CAP];
    char version[SH_MODMANIFEST_VERSION_CAP];
    char bundle[SH_MODMANIFEST_HASH_CAP];
    unsigned long long size;
    unsigned files, shadows;
    char folder[MAX_PATH];             /* the converted bundle's folder */
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

/* Convert one verified ZIP into an installed legacy bundle under
 * <data>\mods\legacy\ and fill `verdict` (key, digest and form are taken
 * from `header`). Reuses an existing <id>@<digest16> folder. Returns 1 for
 * CONVERT, 0 for CLEAN with verdict->reason set. */
int sh_modlegacy_convert_payload(const unsigned char *zip, size_t length,
                                 const sh_legacy_header *header, sh_modlegacy_verdict *verdict);

/* The DeserializeFromJson step: convert or clean every legacy carrier, strip
 * smpkg./smpbind./smpref. rows, and write smpmod1 rows for the converted
 * bundles. Returns the new buffer (HeapFree), or NULL to keep `json` (no
 * legacy rows, or a structure that does not read; logged). */
char *sh_modlegacy_process(const char *json, size_t length, size_t *out_length);

/* The verdicts of the last processed map, for the load gate. Copies at most
 * `cap` and returns the total. */
size_t sh_modlegacy_last_report(sh_modlegacy_verdict *out, size_t cap);

/* The data root (default %LOCALAPPDATA%\snapmap-plus); NULL restores the
 * default. Paths are in the ANSI code page like every backend path. */
void sh_modlegacy_set_data_root(const char *root);
/* Forget the per-process conversion memo (tests). */
void sh_modlegacy_reset(void);

#endif /* SNAPMAP_PLUS_MODLEGACY_H */
