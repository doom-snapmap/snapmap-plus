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
 * decoded size equals the file's, at the file's own path first (every row of
 * that name in both archives, which hold different bytes under some names),
 * then at the path with its mods/<salt>/ segment removed (a game file the
 * author copied under the mod's folder, sh_mb_strip_key), then entries of the same size and
 * extension elsewhere (a renamed copy: every such entry until one matches,
 * each hashed at most once per process), so a build hashes about the bytes
 * that become pointers rather than the archives.
 *
 * Thread-safe: the table is immutable once loaded; reads are positional.
 */
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
int sh_modstock_match(const char *request, unsigned long long size, const char *sha256,
                      char *source, size_t source_cap);
int sh_modstock_identify(const char *source, char sha256[SH_MB_HASH_HEX + 1], unsigned long long *size);
int sh_modstock_verify(const char *source, const char *sha256, unsigned long long *size);
int sh_modstock_read(const char *source, const char *sha256, unsigned char **bytes, size_t *len);
/* Which part of the install holds `source`: OK with "snap_gameresources" or
 * "gameresources" (the archive whose row has that name, the row with these
 * bytes when both hold it and sha256 is given) or "loose" (a file under base\
 * or the install root); ABSENT or UNAVAILABLE. Hashes only a name both
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
    char cause[160];                /* why the table is unavailable */
} sh_modstock_stats;
void sh_modstock_get_stats(sh_modstock_stats *out);

/* Forget the table and the counters (tests). */
void sh_modstock_reset(void);

#endif /* SNAPMAP_PLUS_MODSTOCK_H */
