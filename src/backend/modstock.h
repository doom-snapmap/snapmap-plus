/* modstock.h -- the read-only installed-resource bridge behind stock pointers
 * (Contract 1 file kind "stock", modbundle.h).
 *
 * The bridge reads the player's installed game, never writes it:
 *   <doom>\base\snap_gameresources.pindex and gameresources.pindex, whose rows
 *   name each entry's container (.resources or .patch, the row's selector
 *   byte), offset, stored and decoded size; a compressed slice is raw DEFLATE
 *   in the engine's sync-flush form (raw_deflate.h);
 *   loose files under <doom>\base\ and <doom>\ (a .pages virtual texture).
 * <doom> is the folder of the host executable unless set. Both indexes load
 * once, on the first question, into one table (about 160,000 rows and 23 MB
 * on build 13954591); nothing is read at boot and an empty mods\ never loads
 * it. A slice's SHA-256 is computed at most once per process.
 *
 * Identity is size first: the builder hashes only installed entries whose
 * decoded size equals the file's, at the file's own path first, then entries
 * of the same size and extension elsewhere (a renamed copy, at most
 * SH_MODSTOCK_RENAMED_MAX of them per file and SH_MODSTOCK_RENAMED_BYTES
 * hashed), so a build hashes about the bytes that become pointers rather than
 * the archives.
 *
 * Thread-safe: the table is immutable once loaded; reads are positional.
 */
#ifndef SNAPMAP_PLUS_MODSTOCK_H
#define SNAPMAP_PLUS_MODSTOCK_H

#include <stddef.h>
#include "modbundle.h"

#define SH_MODSTOCK_MAX_SLICE      (512ull << 20)  /* the largest entry read or hashed in memory */
#define SH_MODSTOCK_RENAMED_MAX    64u             /* renamed-copy candidates hashed per file */
#define SH_MODSTOCK_RENAMED_BYTES  (256ull << 20)  /* and their bytes per file */

/* The install folder (the one holding base\), UTF-8. NULL or "" restores the
 * default, the host executable's folder. Drops a loaded table. */
void sh_modstock_set_install(const char *doom_root);

/* Register the bridge as the builder's and server's stock operations
 * (sh_mb_set_stock_ops). Loads nothing. */
void sh_modstock_register(void);

/* The operations themselves (modbundle.h sh_mb_stock_ops semantics). */
int sh_modstock_match(const char *request, unsigned long long size, const char *sha256,
                      char *source, size_t source_cap);
int sh_modstock_identify(const char *source, char sha256[SH_MB_HASH_HEX + 1], unsigned long long *size);
int sh_modstock_verify(const char *source, const char *sha256, unsigned long long *size);
int sh_modstock_read(const char *source, const char *sha256, unsigned char **bytes, size_t *len);

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
    char cause[160];                /* why the table is unavailable */
} sh_modstock_stats;
void sh_modstock_get_stats(sh_modstock_stats *out);

/* Forget the table and the counters (tests). */
void sh_modstock_reset(void);

#endif /* SNAPMAP_PLUS_MODSTOCK_H */
