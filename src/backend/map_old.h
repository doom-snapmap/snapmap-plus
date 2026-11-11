/* map_old.h -- the map shapes earlier Snapmap+ builds wrote, for the store
 * tool's one-time conversion at update (src/store/build.ps1). Never linked
 * into the game DLL: the game refuses such a map (map_carrier.h,
 * sh_map_old_shape) and reads none of this.
 *
 * Earlier shapes carried a map's Snapmap+ data as string variables in the
 * author's variables.string[] table, each named by a family header in
 * info.name with its value in initialValue:
 *
 *   smpmod1.<index>.<count>         one mod row each, its JSON escaped in the
 *                                   value: {"id","version","bundle","size",
 *                                   "attached"?,"choices"?,"names"}
 *   smpprod1.<version>              a development build's product version
 *   smp.render.v1, smp.bounds.v1    the two settings (map_render.h and
 *                                   map_bounds.h encodings)
 *   smnav1.<category>/<module>.<class>.<idx>.<total>.<digest16>.<geom16>
 *                                   baked navigation, base64 chunks
 *   smpkg., smpbind., smpref.       retired packages (map_legacy_payload.h;
 *                                   a package is converted by modlegacy.c)
 *
 * and later the "snapmap-plus" member (map_carrier.h) whose "mods" rows named
 * the mod by "id" and its build by "bundle". A variable is bound by index, so
 * a settings row an author's variable follows is kept in place and rewritten
 * to the value that reads as off ("4;8192", "1;0"); every other row is
 * removed and allocCount[4] rewritten.
 *
 * The conversion: sh_map_old_read collects what the text carries (for each
 * family, the member's part wins when both exist), the caller records each
 * old key's salt and build (the converted folders), and sh_map_old_write
 * returns the text in the current shape: the variables removed, every
 * mods/<old key>/ reference in the text rewritten to mods/<salt>/ (both
 * spellings, sh_mb_rekey_text), and the member carrying the rows as
 * {"mod":"<salt>@<build>",...} (modmanifest.h), the settings and the
 * navigation. */
#ifndef SNAPMAP_PLUS_MAP_OLD_H
#define SNAPMAP_PLUS_MAP_OLD_H

#include <stddef.h>
#include "map_shards.h"
#include "modmanifest.h"

/* ------------------------------------------------- the variable envelope -- */

/* Written chunk size; earlier writers chunked at this; 16384-byte chunks failed native deserialization. */
#define SH_SHARD_CHARS        8192
/* Read cap allows larger chunks from other writers. */
#define SH_SHARD_MAX_CHUNK    65536

/* The longest header text a scanner will consider. Both families are far
 * shorter; a "name" longer than this is not a shard header. */
#define SH_SHARD_HEADER_MAX   256


/* Advance to the next string variable carrying `magic` at or after *pos,
 * bounded by `len`. Returns 1 when one is found:
 *   *hdr / *hdr_len -- the header text, magic included, quotes excluded
 *   *chunk / *chunk_len -- its base64 value (*chunk == NULL means the value
 *                          could not be read; the family then REFUSES the set
 *                          rather than guessing at it)
 * The header grammar is the family's business: this only guarantees the text
 * is the value of a "name" key inside a snapVarInfo_t and is length-bounded. */
int sh_shard_next(const char *json, size_t len, const char *magic, size_t magic_len,
                  size_t *pos, const char **hdr, size_t *hdr_len,
                  const char **chunk, size_t *chunk_len);

/* Return non-zero to REMOVE the variable carrying this header. */
typedef int (*sh_shard_filter_fn)(const char *hdr, size_t hdr_len, void *ctx);

/* Remove matching variables into an owned NUL-terminated process-heap buffer,
 * allocCount[4] rewritten. NULL retains the original; doc_failed
 * distinguishes a structural refusal from no matches. Never applies a partial
 * splice. */
char *sh_shard_strip(const char *json, size_t len, const char *magic, size_t magic_len,
                     sh_shard_filter_fn filter, void *ctx, size_t max_cuts,
                     size_t *out_len, unsigned *elements_out, unsigned *runs_out,
                     int *doc_failed);

typedef struct sh_shard_out {
    const char *header;
    const char *chunk;
    size_t      chunk_len;
} sh_shard_out;

/* Append variables to `variables.string[]` and keep `variables.allocCount[4]`
 * equal to the string-variable count. Returns a new NUL-terminated
 * HeapAlloc'd buffer with *out_len set, or NULL with the reason in `err`.
 * (Test fixtures build earlier-shape maps with it.) */
char *sh_shard_insert(const char *json, size_t len,
                      const sh_shard_out *shards, size_t count,
                      size_t *out_len, char *err, size_t err_cap);

/* ---------------------------------------------------------------- read ---- */

#define SH_MAP_OLD_KEY_CAP   65    /* an old row's "id": at most 64 characters */
#define SH_MAP_OLD_VALUE_MAX 192   /* a settings value */

/* Which earlier families the text carries (sh_map_old.families). */
enum {
    SH_MAP_OLD_F_MEMBER_ROWS = 1 << 0,   /* member "mods" rows with "id"/"bundle" */
    SH_MAP_OLD_F_ROWS        = 1 << 1,   /* smpmod1. */
    SH_MAP_OLD_F_PRODUCT     = 1 << 2,   /* smpprod1. */
    SH_MAP_OLD_F_SETTINGS    = 1 << 3,   /* smp.render.v1 / smp.bounds.v1 */
    SH_MAP_OLD_F_NAV         = 1 << 4,   /* smnav1. */
    SH_MAP_OLD_F_PACKAGE     = 1 << 5,   /* smpkg. */
    SH_MAP_OLD_F_UNRELEASED  = 1 << 6    /* smpbind. / smpref. */
};

/* One mod row as earlier builds wrote it: the mod by its old key. */
typedef struct sh_map_old_row {
    char key[SH_MAP_OLD_KEY_CAP];               /* the row's "id" */
    char version[SH_MODMANIFEST_VERSION_CAP];
    char bundle[SH_MODMANIFEST_HASH_CAP];       /* the build hash it pinned */
    unsigned long long size;
    char **names;                               /* malloc'd; owned */
    size_t name_count;
    int attached;
    char choices[SH_MODMANIFEST_CHOICES_CAP];   /* "slot\told key\n" lines */
} sh_map_old_row;

typedef struct sh_map_old {
    unsigned families;                 /* SH_MAP_OLD_F_* found in the text */
    sh_map_old_row *rows;              /* the member's id rows, else the smpmod1 rows */
    size_t row_count;
    unsigned rows_rejected;            /* rows that did not read */
    char render[SH_MAP_OLD_VALUE_MAX + 1];   /* settings value text; "" when off */
    char bounds[SH_MAP_OLD_VALUE_MAX + 1];
    unsigned settings_refused;         /* duplicated or unreadable rows: that setting is off */
    char *nav;                         /* the member "nav" part's JSON array (malloc), or NULL */
    size_t nav_len;
    unsigned nav_sets;                 /* sets in it */
    unsigned nav_refused;              /* smnav1 sets that did not reassemble: dropped */
} sh_map_old;

/* Read every earlier family in `json`. 1 with *out filled (free with
 * sh_map_old_free; families 0 means the text is already in the current
 * shape); 0 with the reason in err when the text does not read as a map. */
int  sh_map_old_read(const char *json, size_t len, sh_map_old *out, char *err, size_t err_cap);
void sh_map_old_free(sh_map_old *old);

/* --------------------------------------------------------------- write ---- */

/* One old key's identity in this shape: the converted folder's salt and the
 * build its row now names, with the display name and version the row
 * carries (empty: the old row's version, the key as name) and its size (0:
 * the old row's). */
typedef struct sh_map_old_salt {
    char key[SH_MAP_OLD_KEY_CAP];
    char salt[SH_MODMANIFEST_SALT_CAP];
    char build[SH_MODMANIFEST_HASH_CAP];
    char name[SH_MODMANIFEST_DISPLAY_CAP];
    char version[SH_MODMANIFEST_VERSION_CAP];
    unsigned long long size;
} sh_map_old_salt;

/* `json` (read into `old`) in the current shape: every earlier variable
 * removed (a settings row an author's variable follows kept as off), every
 * mods/<key>/ reference of each `map` entry rewritten to mods/<salt>/, and
 * the member's "mods" part holding one row per old row (choices naming a key
 * `map` lacks are dropped; a member whose rows are all in this shape keeps
 * them), its "settings" and "nav" parts what `old` read.
 * Returns an owned NUL-terminated process-heap buffer (HeapFree) with
 * *out_len set, or NULL with the reason in err: the text carries a package
 * (smpkg.; modlegacy.c converts it first), a row's key has no entry in `map`,
 * the text does not read, or no memory. */
char *sh_map_old_write(const char *json, size_t len, const sh_map_old *old,
                       const sh_map_old_salt *map, size_t map_count,
                       size_t *out_len, char *err, size_t err_cap);

#endif /* SNAPMAP_PLUS_MAP_OLD_H */
