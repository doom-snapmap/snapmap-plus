/* modstock.h -- the read-only bridge to the installed game behind stock pointers (Contract 1 file kind "stock",
 * modbundle.h): it reads the pindex rows of base\snap_gameresources and gameresources (compressed slices are raw
 * sync-flush DEFLATE, raw_deflate.h) and loose files under <doom>\base\ and <doom>\. Both indexes load once, on
 * the first question, into one immutable table (about 160,000 rows, 23 MB), so reads are thread-safe. Matching is
 * by size first, then the file's own path, its path without the mods/<salt>/ segment, then same-size entries of
 * the same extension, so a build hashes only the bytes that become pointers. */
#ifndef SNAPMAP_PLUS_MODSTOCK_H
#define SNAPMAP_PLUS_MODSTOCK_H

#include <stddef.h>
#include "modbundle.h"

#define SH_MODSTOCK_MAX_SLICE      (512ull << 20)  /* the largest entry read or hashed in memory */

/* The install folder (the one holding base\), UTF-8. NULL or "" restores the
 * default, the host executable's folder. Drops a loaded table. */
void sh_modstock_set_install(const char *doom_root);

/* Register the bridge as the builder's and server's stock operations
 * (sh_mb_set_stock_ops). Loads nothing. */
void sh_modstock_register(void);

/* The operations themselves (modbundle.h sh_mb_stock_ops semantics). */
int sh_modstock_match(const char *request, unsigned long long size, const char *sha256, char **source);
int sh_modstock_identify(const char *source, char sha256[SH_MB_HASH_HEX + 1], unsigned long long *size);
int sh_modstock_verify(const char *source, const char *sha256, unsigned long long *size);
int sh_modstock_read(const char *source, const char *sha256, unsigned char **bytes, size_t *len);
/* Which part of the install holds `source`: OK with "snap_gameresources", "gameresources" (the row with these
 * bytes when both hold it and sha256 is given) or "loose"; ABSENT or UNAVAILABLE. Hashes only a name both
 * archives hold. */
int sh_modstock_container(const char *source, const char *sha256, char *out, size_t cap);
/* Every archive path (folded to lower case) starting with `prefix`, passed
 * to `fn` once per row: SH_MB_STOCK_OK or UNAVAILABLE. A linear walk of the
 * table; loose files are not listed. */
int sh_modstock_list(const char *prefix, void (*fn)(void *context, const char *path), void *context);

/* 1 when the installed game holds `path` (an archive row or a loose file),
 * 0 when not, -1 when the installed indexes cannot be read. */
int sh_modstock_exists(const char *path);

typedef struct sh_modstock_stats {
    int loaded;                     /* 1 loaded, -1 unavailable, 0 not asked yet */
    unsigned rows;                  /* archive rows with bytes */
    unsigned long long table_bytes; /* memory the table holds */
    double load_ms;
    unsigned slices_hashed;         /* distinct archive slices hashed */
    unsigned long long bytes_hashed;/* decoded bytes hashed (slices and loose files) */
    unsigned reads;                 /* successful reads */
    char *cause;                    /* why the table is unavailable (malloc'd, "" when it is) */
} sh_modstock_stats;
void sh_modstock_get_stats(sh_modstock_stats *out);
void sh_modstock_stats_free(sh_modstock_stats *s);

/* Forget the table and the counters (tests). */
void sh_modstock_reset(void);

#endif /* SNAPMAP_PLUS_MODSTOCK_H */
