/* modzip.h -- the product's ZIP form: one writer and one reader, shared by
 * the rawmap and prefab exports (modexport.c) and the mod store's build
 * archives.
 *
 * The writer stores every entry (method 0) with its CRC-32, a fixed DOS time
 * (1980-01-01, so the same content is byte-identical) and the UTF-8 flag on a
 * non-ASCII name. It writes ZIP64 records exactly where a value does not fit
 * the classic field (an entry of 4 GiB or more, an offset past 4 GiB, 65,535
 * entries or more, a directory past 4 GiB), so an archive of any size and any
 * number of files can be written, and an archive that fits is the ordinary
 * ZIP it always was. The reader reads both. It trusts no field of an archive:
 * at open it checks the end record, the ZIP64 locator and end record when the
 * classic fields point to them, the directory's bounds, and for every entry
 * the method (stored (0) or deflate (8) only), the encryption flag, the ZIP64
 * extra field a marker needs (well formed, holding exactly the values the
 * markers ask for), a stored entry's equal sizes, a local record and data
 * that lie before the directory, a name
 * without NUL that sh_zip_name_ok accepts, and no two names that differ only
 * in case; anything else refuses the whole archive with one sentence ending
 * "Ask whoever shared it to export it again". A read then checks the local
 * record, bounds the bytes it allocates by `max`, and checks the CRC-32. A
 * reader may also stand over a plain folder (dir set, entries added with
 * sh_zip_reader_add): each entry is then a file under that folder. */
#ifndef SNAPMAP_PLUS_MODZIP_H
#define SNAPMAP_PLUS_MODZIP_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

/* A name's bytes: an entry's name length is a 16-bit field of the ZIP
 * format, so a name holds at most 65,535 bytes. That is the format's bound,
 * not the product's: a path inside a mod has no length of its own. */
#define SH_ZIP_NAME_MAX     65535u
#define SH_ZIP_METHOD_FILE  0xFFFFu   /* a folder reader's entry (r->dir set): read from the file */

/* ------------------------------------------------------------ writer ---- */

typedef struct sh_zip_went { char *name; uint32_t crc; uint64_t size, offset; unsigned flags; } sh_zip_went;

typedef struct sh_zip_writer {
    HANDLE h;                  /* INVALID_HANDLE_VALUE when no file is open */
    unsigned long long off;    /* bytes written so far */
    sh_zip_went *e;
    size_t n, cap;             /* n: entries written */
    int failed;
    char *cause;               /* the first failure (malloc'd; sh_zip_writer_free frees it) */
} sh_zip_writer;

/* Create (or replace) the file at `path` and start an empty archive. Returns 1;
 * 0 with GetLastError() as CreateFileW left it and w->h INVALID_HANDLE_VALUE. */
int  sh_zip_writer_open(sh_zip_writer *w, const wchar_t *path);

/* One stored entry. Returns 1; 0 with w->failed and w->cause set (a later add
 * or finish is then a no-op that returns 0). */
int  sh_zip_writer_add(sh_zip_writer *w, const char *name, const void *data, size_t len);

/* The central directory and the end record. Returns 1; 0 with w->cause. The
 * file stays open: the caller flushes and closes w->h (or sh_zip_writer_free
 * closes it). */
int  sh_zip_writer_finish(sh_zip_writer *w);

/* Close the file if it is still open and free the entry list. */
void sh_zip_writer_free(sh_zip_writer *w);

/* ------------------------------------------------------------ reader ---- */

typedef struct sh_zip_rentry {
    char *name;
    unsigned method, flags;
    uint32_t crc;
    uint64_t csize, usize, lho;
} sh_zip_rentry;

typedef struct sh_zip_reader {
    HANDLE h;
    unsigned long long size;   /* the archive's size in bytes */
    sh_zip_rentry *e;
    size_t n, cap;
    size_t *sorted;            /* indexes sorted by name */
    wchar_t *dir;              /* a folder reader: each entry is a file under this folder (malloc'd) */
} sh_zip_reader;

/* A name a reader may trust: relative, '/'-separated, no empty, "." or ".."
 * segment, no drive, backslash or control character, at most
 * SH_ZIP_NAME_MAX bytes (the format's bound). */
int  sh_zip_name_ok(const char *s, size_t n);

/* Open the archive at `path` (UTF-8) and read its central directory. Returns
 * 1; 0 with the cause (malloc'd into *cause, as every cause of this header)
 * and r zeroed. */
int  sh_zip_reader_open(sh_zip_reader *r, const char *path, char **cause);

/* A folder reader's entry (r->dir set by the caller): `name` relative to the
 * folder, '/'-separated. Returns 1; 0 when out of memory.
 * sh_zip_reader_sort indexes the entries once they are all added. */
int  sh_zip_reader_add(sh_zip_reader *r, const char *name, unsigned long long size);

/* Index the entries by name; two entries whose names differ at most in case
 * close the reader and refuse. Returns 1; 0 with the cause. */
int  sh_zip_reader_sort(sh_zip_reader *r, char **cause);

/* The entry's index (the exact name), or -1. */
long sh_zip_reader_find(const sh_zip_reader *r, const char *name);

/* One entry's bytes (malloc'd, NUL-terminated), at most max bytes, its CRC-32
 * checked. NULL with the cause. */
unsigned char *sh_zip_reader_read(const sh_zip_reader *r, long idx, size_t max, size_t *out_len, char **cause);

/* Close the archive and free everything; r is zeroed. */
void sh_zip_reader_close(sh_zip_reader *r);

#endif /* SNAPMAP_PLUS_MODZIP_H */
