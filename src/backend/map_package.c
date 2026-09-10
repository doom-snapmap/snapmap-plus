/* smpkg shard parsing, payload extraction and consent-gated installation. Use
 * bounded JSON spans without a DOM. Strict base64, header counts and Windows
 * path checks reject malformed delivery data.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <shellapi.h>
#pragma comment(lib, "user32.lib")

#include <stdlib.h>
#include "map_package.h"
#include "map_shards.h"
#include "engine_dialog.h"
#include "packages.h"
#include "decl_server.h"
#include "raw_deflate.h"
#include "backend_log.h"

/* ==================================================================== */
/* small text helpers                                                    */
/* ==================================================================== */

/* The smpkg package-id character class -- the one text rule that is this
 * family's and not the envelope's. Everything else lives in map_shards.c. */
static int mpkg_is_idc(char c)
{
    return (c >= 'a' && c <= 'z') || sh_shard_is_digit(c) || c == '_' || c == '-';
}

static void mpkg_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    _vsnprintf_s(err, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ==================================================================== */
/* the shard iterator -- the smpkg header grammar over the shared scanner */
/* ==================================================================== */

typedef struct mpkg_hdr {
    char     id[SH_MPKG_ID_CAP];
    char     digest[SH_MPKG_DIGEST_CHARS + 1];
    unsigned idx, total;
} mpkg_hdr;

#define MPKG_HEADER_MAGIC   "smpkg."
#define MPKG_MAGIC_LEN      6

/* Parse one header text -- magic included, quotes excluded, exactly what
 * sh_shard_next hands back -- into `hdr`. Returns 1 only when the WHOLE text
 * is a well-formed smpkg header, so a "name" that merely starts like one is
 * not a shard. */
static int mpkg_parse_header(const char *p, size_t plen, mpkg_hdr *hdr)
{
    const char *c = p + MPKG_MAGIC_LEN;
    const char *end = p + plen;
    size_t n = 0;
    unsigned long v;
    int digits;

    if (plen <= MPKG_MAGIC_LEN) return 0;

    /* package id: [a-z0-9_-]+ */
    while (c < end && mpkg_is_idc(*c) && n < SH_MPKG_ID_CAP - 1) hdr->id[n++] = *c++;
    if (n == 0 || c >= end || *c != '.') return 0;
    hdr->id[n] = '\0';
    c++;

    /* index */
    v = 0; digits = 0;
    while (c < end && sh_shard_is_digit(*c) && digits < 8) { v = v * 10 + (unsigned)(*c - '0'); c++; digits++; }
    if (digits == 0 || digits >= 8 || c >= end || *c != '.') return 0;
    hdr->idx = (unsigned)v;
    c++;

    /* total: 1..SH_MPKG_MAX_SHARDS */
    v = 0; digits = 0;
    while (c < end && sh_shard_is_digit(*c) && digits < 8) { v = v * 10 + (unsigned)(*c - '0'); c++; digits++; }
    if (digits == 0 || digits >= 8 || v == 0 || v > SH_MPKG_MAX_SHARDS) return 0;
    if (c >= end || *c != '.') return 0;
    hdr->total = (unsigned)v;
    c++;

    /* digest: exactly 16 lowercase hex, and the header ends there */
    for (n = 0; n < SH_MPKG_DIGEST_CHARS; n++) {
        if (c >= end || !sh_shard_is_hex(*c)) return 0;
        hdr->digest[n] = *c++;
    }
    hdr->digest[SH_MPKG_DIGEST_CHARS] = '\0';
    return c == end;
}

/* Advance the iterator: find the next VALID shard at/after *pos. Malformed
 * or non-name "smpkg." occurrences are skipped silently (they are not
 * shards). Returns 1 = found (chunk==NULL means the header parsed but its
 * value is unreadable -- the package is then refused, never guessed). */
static int mpkg_next_shard(const char *json, size_t len, size_t *pos,
                           mpkg_hdr *hdr, const char **chunk, size_t *chunk_len)
{
    const char *raw;
    size_t raw_len;
    while (sh_shard_next(json, len, MPKG_HEADER_MAGIC, MPKG_MAGIC_LEN, pos,
                         &raw, &raw_len, chunk, chunk_len)) {
        if (mpkg_parse_header(raw, raw_len, hdr)) return 1;
    }
    return 0;
}

/* ==================================================================== */
/* strip                                                                 */
/* ==================================================================== */

/* Strip matching package variables; a package filter preserves other payloads
 * during re-embedding.
 */
static int mpkg_strip_filter(const char *hdr, size_t hdr_len, void *ctx)
{
    const char *pkg_id = (const char *)ctx;
    mpkg_hdr parsed;
    if (!mpkg_parse_header(hdr, hdr_len, &parsed)) return 0;   /* not a shard at all */
    return pkg_id == NULL || strcmp(parsed.id, pkg_id) == 0;
}

static char *mpkg_strip_scoped(const char *json, size_t len, const char *pkg_id, size_t *out_len)
{
    unsigned elements = 0, runs = 0;
    int doc_failed = 0;
    size_t w = 0;
    char *out;
    char line[192];

    if (out_len) *out_len = 0;
    out = sh_shard_strip(json, len, MPKG_HEADER_MAGIC, MPKG_MAGIC_LEN,
                         mpkg_strip_filter, (void *)pkg_id, SH_MPKG_MAX_SHARDS,
                         &w, &elements, &runs, &doc_failed);
    if (!out) {
        if (doc_failed)
            backend_log("MPKG: payload strip SKIPPED -- the map's JSON structure did not read "
                        "cleanly; handing the engine the original buffer");
        return NULL;
    }

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "MPKG: payload STRIPPED (%s) -- %u shard variable(s) removed in %u contiguous "
                "run(s), %zu -> %zu bytes; the delivery envelope never becomes map state",
                pkg_id ? pkg_id : "every package", elements, runs, len, w);
    backend_log(line);

    if (out_len) *out_len = w;
    return out;
}

char *sh_mpkg_strip(const char *json, size_t len, size_t *out_len)
{
    return mpkg_strip_scoped(json, len, NULL, out_len);
}

/* ==================================================================== */
/* embed                                                                 */
/* ==================================================================== */

void sh_mpkg_digest16(const unsigned char *payload, size_t len,
                      char out[SH_MPKG_DIGEST_CHARS + 1])
{
    sh_shard_digest16(payload, len, out);
}

char *sh_mpkg_embed(const char *json, size_t len, const char *pkg_id,
                    const unsigned char *payload, size_t payload_len,
                    size_t *out_len, char *err, size_t err_cap)
{
    char *base = NULL;          /* the payload-free buffer we build on top of */
    const char *src;
    size_t src_len;
    char *b64 = NULL, *out = NULL, *headers = NULL;
    sh_shard_out *parts = NULL;
    size_t b64_len, shards, i, w = 0;
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    char line[224];

    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = '\0';
    if (!json || len == 0 || !pkg_id || !payload) {
        mpkg_err(err, err_cap, "embed called with nothing to embed");
        return NULL;
    }
    if (payload_len == 0 || payload_len > SH_MPKG_MAX_PAYLOAD) {
        mpkg_err(err, err_cap, "payload is %zu bytes, over the %u-byte embed budget",
                 payload_len, (unsigned)SH_MPKG_MAX_PAYLOAD);
        return NULL;
    }

    /* Replace this package's old shards before adding new ones. */
    base = mpkg_strip_scoped(json, len, pkg_id, &src_len);
    src = base ? base : json;
    if (!base) src_len = len;

    b64_len = ((payload_len + 2) / 3) * 4;
    b64 = (char *)HeapAlloc(GetProcessHeap(), 0, b64_len + 1);
    if (!b64) { mpkg_err(err, err_cap, "out of memory encoding the payload"); goto done; }
    b64_len = sh_shard_b64_encode(payload, payload_len, b64);

    shards = (b64_len + SH_MPKG_SHARD_CHARS - 1) / SH_MPKG_SHARD_CHARS;
    if (shards == 0) shards = 1;
    if (shards > SH_MPKG_MAX_SHARDS) {
        mpkg_err(err, err_cap, "payload needs %zu shards, over the %u cap",
                 shards, (unsigned)SH_MPKG_MAX_SHARDS);
        goto done;
    }
    sh_mpkg_digest16(payload, payload_len, digest);

    parts = (sh_shard_out *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, shards * sizeof *parts);
    headers = (char *)HeapAlloc(GetProcessHeap(), 0, shards * SH_MPKG_HEADER_CAP);
    if (!parts || !headers) {
        mpkg_err(err, err_cap, "out of memory building the map");
        goto done;
    }
    for (i = 0; i < shards; i++) {
        char *h = headers + i * SH_MPKG_HEADER_CAP;
        size_t chunk_len = b64_len - i * SH_MPKG_SHARD_CHARS;
        if (chunk_len > SH_MPKG_SHARD_CHARS) chunk_len = SH_MPKG_SHARD_CHARS;
        _snprintf_s(h, SH_MPKG_HEADER_CAP, _TRUNCATE, "%s%s.%u.%u.%s",
                    MPKG_HEADER_MAGIC, pkg_id, (unsigned)i, (unsigned)shards, digest);
        parts[i].header = h;
        parts[i].chunk = b64 + i * SH_MPKG_SHARD_CHARS;
        parts[i].chunk_len = chunk_len;
    }

    out = sh_shard_insert(src, src_len, parts, shards, &w, err, err_cap);
    if (!out) goto done;

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "MPKG: package '%s' EMBEDDED into the map -- %zu payload bytes as %zu shard(s), "
                "digest %s; map %zu -> %zu bytes",
                pkg_id, payload_len, shards, digest, len, w);
    backend_log(line);
    if (out_len) *out_len = w;

done:
    if (parts)   HeapFree(GetProcessHeap(), 0, parts);
    if (headers) HeapFree(GetProcessHeap(), 0, headers);
    if (b64)     HeapFree(GetProcessHeap(), 0, b64);
    if (base)    HeapFree(GetProcessHeap(), 0, base);
    return out;
}

/* ==================================================================== */
/* scan                                                                  */
/* ==================================================================== */

typedef struct mpkg_seen {
    unsigned char bits[SH_MPKG_MAX_SHARDS / 8];
} mpkg_seen;

static int mpkg_seen_test_set(mpkg_seen *s, unsigned idx)
{
    unsigned char m = (unsigned char)(1u << (idx & 7));
    if (s->bits[idx >> 3] & m) return 1;
    s->bits[idx >> 3] |= m;
    return 0;
}

static size_t mpkg_scan_internal(const char *json, size_t len,
                                 sh_mpkg_decl *out, size_t cap, int *overflow)
{
    mpkg_seen seen[SH_MPKG_MAX_PACKAGES];
    size_t count = 0, pos = 0, i;
    mpkg_hdr hdr;
    const char *chunk;
    size_t chunk_len;

    if (overflow) *overflow = 0;
    if (!json || !out || cap == 0) { if (overflow && json) *overflow = 1; return 0; }
    if (cap > SH_MPKG_MAX_PACKAGES) cap = SH_MPKG_MAX_PACKAGES;
    memset(seen, 0, sizeof seen);

    while (mpkg_next_shard(json, len, &pos, &hdr, &chunk, &chunk_len)) {
        sh_mpkg_decl *d = NULL;
        for (i = 0; i < count; i++)
            if (strcmp(out[i].id, hdr.id) == 0) { d = &out[i]; break; }
        if (!d) {
            if (count >= cap) { if (overflow) *overflow = 1; continue; }
            d = &out[count++];
            memset(d, 0, sizeof *d);
            strcpy_s(d->id, sizeof d->id, hdr.id);
            strcpy_s(d->digest, sizeof d->digest, hdr.digest);
            d->total = hdr.total;
            d->consistent = 1;
        }
        if (d->total != hdr.total || strcmp(d->digest, hdr.digest) != 0) {
            d->consistent = 0;   /* two versions of one package in one map */
            continue;
        }
        if (hdr.idx >= d->total) { d->consistent = 0; continue; }
        if (chunk == NULL) { d->consistent = 0; continue; }
        if (mpkg_seen_test_set(&seen[d - out], hdr.idx)) { d->consistent = 0; continue; }
        d->present++;
    }
    for (i = 0; i < count; i++)
        out[i].complete = out[i].consistent && out[i].present == out[i].total;
    return count;
}

size_t sh_mpkg_scan(const char *json, size_t len, sh_mpkg_decl *out, size_t cap)
{
    return mpkg_scan_internal(json, len, out, cap, NULL);
}

/* ==================================================================== */
/* extract                                                               */
/* ==================================================================== */

unsigned char *sh_mpkg_extract(const char *json, size_t len, const char *pkg_id,
                               size_t *out_len, char *err, size_t err_cap)
{
    sh_shard_chunk *chunks = NULL;
    mpkg_hdr hdr;
    const char *chunk;
    size_t chunk_len, pos = 0;
    unsigned total = 0, present = 0;
    int have_meta = 0;
    char digest[SH_MPKG_DIGEST_CHARS + 1] = "";
    unsigned char *payload = NULL;
    size_t payload_len = 0;

    mpkg_err(err, err_cap, "");
    if (!json || !pkg_id || !out_len) { mpkg_err(err, err_cap, "bad arguments"); return NULL; }
    *out_len = 0;

    while (mpkg_next_shard(json, len, &pos, &hdr, &chunk, &chunk_len)) {
        if (strcmp(hdr.id, pkg_id) != 0) continue;
        if (!have_meta) {
            total = hdr.total;
            strcpy_s(digest, sizeof digest, hdr.digest);
            have_meta = 1;
            chunks = (sh_shard_chunk *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                 (size_t)total * sizeof *chunks);
            if (!chunks) { mpkg_err(err, err_cap, "out of memory"); return NULL; }
        }
        if (hdr.total != total || strcmp(hdr.digest, digest) != 0) {
            mpkg_err(err, err_cap, "package '%s' has inconsistent shard headers -- "
                     "the map carries two different versions", pkg_id);
            goto fail;
        }
        if (hdr.idx >= total) {
            mpkg_err(err, err_cap, "package '%s' has shard index %u out of range (total %u)",
                     pkg_id, hdr.idx, total);
            goto fail;
        }
        if (chunk == NULL) {
            mpkg_err(err, err_cap, "package '%s' has an unreadable shard %u", pkg_id, hdr.idx);
            goto fail;
        }
        if (chunks[hdr.idx].filled) {
            mpkg_err(err, err_cap, "package '%s' has duplicate shard %u", pkg_id, hdr.idx);
            goto fail;
        }
        chunks[hdr.idx].p = chunk;
        chunks[hdr.idx].n = chunk_len;
        chunks[hdr.idx].filled = 1;
        present++;
    }
    if (!have_meta) {
        mpkg_err(err, err_cap, "package '%s' is not present in this map", pkg_id);
        return NULL;
    }
    if (present != total) {
        mpkg_err(err, err_cap, "package '%s' is incomplete: %u of %u shards",
                 pkg_id, present, total);
        goto fail;
    }
    {
        int reason = SH_SHARD_B64_MALFORMED;
        payload = sh_shard_b64_decode(chunks, total, SH_MPKG_MAX_PAYLOAD, &payload_len, &reason);
        if (!payload) {
            if (reason == SH_SHARD_B64_TOO_BIG)
                mpkg_err(err, err_cap, "package '%s' payload exceeds the %u-byte budget",
                         pkg_id, (unsigned)SH_MPKG_MAX_PAYLOAD);
            else if (reason == SH_SHARD_B64_NOMEM)
                mpkg_err(err, err_cap, "out of memory");
            else
                mpkg_err(err, err_cap, "package '%s' has invalid base64 in its shards", pkg_id);
            goto fail;
        }
    }

    {
        char got[SH_MPKG_DIGEST_CHARS + 1];
        sh_shard_digest16(payload, payload_len, got);
        if (strcmp(got, digest) != 0) {
            mpkg_err(err, err_cap, "package '%s' failed its digest: header says %s, payload is %s",
                     pkg_id, digest, got);
            HeapFree(GetProcessHeap(), 0, payload);
            goto fail;
        }
    }
    HeapFree(GetProcessHeap(), 0, chunks);
    *out_len = payload_len;
    return payload;
fail:
    if (chunks) HeapFree(GetProcessHeap(), 0, chunks);
    return NULL;
}

/* ==================================================================== */
/* zip unpack (deflate via the shared raw_deflate.c; no second inflate)  */
/* ==================================================================== */

#define MPKG_ZIP_EOCD_SIG   0x06054b50u
#define MPKG_ZIP_CEN_SIG    0x02014b50u
#define MPKG_ZIP_LOC_SIG    0x04034b50u
#define MPKG_ZIP_MAX_ENTRIES     4096u
#define MPKG_ZIP_MAX_FILE_BYTES  (64u * 1024u * 1024u)
#define MPKG_ZIP_MAX_TOTAL_BYTES (256u * 1024u * 1024u)

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

typedef struct mpkg_zentry {
    const char *name;        /* NOT NUL-terminated */
    unsigned    name_len;
    unsigned    method, csize, usize;
    const unsigned char *data;   /* compressed bytes inside the payload */
    int         is_dir;
} mpkg_zentry;

/* Locate the central directory. Return 1 with cd and count, or 0 if
 * malformed.
 */
static int mpkg_zip_open(const unsigned char *payload, size_t len,
                         const unsigned char **cd, unsigned *count,
                         char *err, size_t err_cap)
{
    size_t i, floor_ = 0;
    if (len < 22) { mpkg_err(err, err_cap, "payload too small to be a zip"); return 0; }
    if (len > 22 + 65535) floor_ = len - 22 - 65535;
    for (i = len - 22; ; i--) {
        if (rd32(payload + i) == MPKG_ZIP_EOCD_SIG) {
            unsigned n = rd16(payload + i + 10);
            uint32_t cd_size = rd32(payload + i + 12);
            uint32_t cd_off  = rd32(payload + i + 16);
            if (n > MPKG_ZIP_MAX_ENTRIES) { mpkg_err(err, err_cap, "zip has too many entries (%u)", n); return 0; }
            if (cd_off > i || cd_size > i - cd_off) { mpkg_err(err, err_cap, "zip central directory out of bounds"); return 0; }
            *cd = payload + cd_off;
            *count = n;
            return 1;
        }
        if (i == floor_) break;
    }
    mpkg_err(err, err_cap, "zip end-of-central-directory not found");
    return 0;
}

/* Read one central-directory entry at *cursor and resolve its data span
 * through the local header. Advances *cursor. */
static int mpkg_zip_entry(const unsigned char *payload, size_t len,
                          const unsigned char **cursor, mpkg_zentry *e,
                          char *err, size_t err_cap)
{
    const unsigned char *c = *cursor;
    uint32_t loc_off;
    unsigned nlen, xlen, clen;
    if ((size_t)(c - payload) + 46 > len || rd32(c) != MPKG_ZIP_CEN_SIG) {
        mpkg_err(err, err_cap, "zip central directory entry malformed");
        return 0;
    }
    e->method = rd16(c + 10);
    e->csize  = rd32(c + 20);
    e->usize  = rd32(c + 24);
    nlen = rd16(c + 28); xlen = rd16(c + 30); clen = rd16(c + 32);
    loc_off = rd32(c + 42);
    if ((size_t)(c - payload) + 46 + nlen + xlen + clen > len) {
        mpkg_err(err, err_cap, "zip central directory entry out of bounds");
        return 0;
    }
    e->name = (const char *)(c + 46);
    e->name_len = nlen;
    e->is_dir = nlen > 0 && e->name[nlen - 1] == '/';
    if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu) {
        mpkg_err(err, err_cap, "zip64 archives are not supported");
        return 0;
    }
    /* resolve the data span through the local header (its own name/extra
     * lengths differ from the central copy in general). */
    if ((size_t)loc_off + 30 > len || rd32(payload + loc_off) != MPKG_ZIP_LOC_SIG) {
        mpkg_err(err, err_cap, "zip local header out of bounds");
        return 0;
    }
    {
        unsigned lnlen = rd16(payload + loc_off + 26);
        unsigned lxlen = rd16(payload + loc_off + 28);
        size_t data_off = (size_t)loc_off + 30 + lnlen + lxlen;
        if (data_off > len || (size_t)e->csize > len - data_off) {
            mpkg_err(err, err_cap, "zip member data out of bounds");
            return 0;
        }
        e->data = payload + data_off;
    }
    *cursor = c + 46 + nlen + xlen + clen;
    return 1;
}

/* Require relative forward-slash paths without dot segments, drive letters or
 * control characters.
 */
static int mpkg_member_path_safe(const char *name, unsigned n)
{
    unsigned i, seg_start = 0;
    if (n == 0 || n >= MAX_PATH) return 0;
    if (name[0] == '/') return 0;
    for (i = 0; i <= n; i++) {
        char c = (i < n) ? name[i] : '/';   /* virtual terminator closes the last segment */
        if (i < n && (c == '\\' || c == ':' || (unsigned char)c < 0x20)) return 0;
        if (c == '/') {
            unsigned seg_len = i - seg_start;
            if (seg_len == 0) return 0;                                  /* "//" or leading '/' */
            if (seg_len == 1 && name[seg_start] == '.') return 0;
            if (seg_len == 2 && name[seg_start] == '.' && name[seg_start + 1] == '.') return 0;
            seg_start = i + 1;
        }
    }
    return 1;
}

/* Create every directory of `rel` (forward-slashed, possibly ending in the
 * file name which is NOT created) under `base`. */
static int mpkg_make_parents(const char *base, const char *rel, int whole_is_dir)
{
    char path[MAX_PATH];
    size_t base_len, i;
    if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", base, rel) < 0) return 0;
    base_len = strlen(base) + 1;
    for (i = base_len; path[i]; i++) {
        if (path[i] != '/') continue;
        path[i] = '\0';
        if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
        path[i] = '\\';
    }
    if (whole_is_dir) {
        if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    }
    return 1;
}

static int mpkg_write_file(const char *base, const char *rel,
                           const unsigned char *bytes, size_t n)
{
    char path[MAX_PATH];
    size_t i;
    HANDLE h;
    size_t total = 0;
    if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", base, rel) < 0) return 0;
    for (i = strlen(base) + 1; path[i]; i++)
        if (path[i] == '/') path[i] = '\\';
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (total < n) {
        DWORD chunk = (DWORD)((n - total) > 0x10000000 ? 0x10000000 : (n - total));
        DWORD wr = 0;
        if (!WriteFile(h, bytes + total, chunk, &wr, NULL) || wr == 0) break;
        total += wr;
    }
    CloseHandle(h);
    return total == n;
}

/* Count file members + verify the payload IS a package (a top-level
 * package.json) without writing anything. */
static int mpkg_zip_survey(const unsigned char *payload, size_t len,
                           unsigned *files_out, char *err, size_t err_cap)
{
    const unsigned char *cd, *cursor;
    unsigned count, i, files = 0;
    int has_marker = 0;
    unsigned long long total_bytes = 0;
    mpkg_zentry e;
    if (!mpkg_zip_open(payload, len, &cd, &count, err, err_cap)) return 0;
    cursor = cd;
    for (i = 0; i < count; i++) {
        if (!mpkg_zip_entry(payload, len, &cursor, &e, err, err_cap)) return 0;
        if (e.is_dir) {
            /* A directory entry is vetted here too, so nothing is written to
             * disk before EVERY member path has passed. */
            if (e.name_len > 1 && !mpkg_member_path_safe(e.name, e.name_len - 1)) {
                mpkg_err(err, err_cap, "unsafe member path in package: '%.*s'",
                         (int)(e.name_len > 200 ? 200 : e.name_len), e.name);
                return 0;
            }
            continue;
        }
        if (!mpkg_member_path_safe(e.name, e.name_len)) {
            mpkg_err(err, err_cap, "unsafe member path in package: '%.*s'",
                     (int)(e.name_len > 200 ? 200 : e.name_len), e.name);
            return 0;
        }
        if (e.method != 0 && e.method != 8) {
            mpkg_err(err, err_cap, "unsupported zip method %u for '%.*s'",
                     e.method, (int)e.name_len, e.name);
            return 0;
        }
        if (e.usize > MPKG_ZIP_MAX_FILE_BYTES) {
            mpkg_err(err, err_cap, "zip member '%.*s' too large", (int)e.name_len, e.name);
            return 0;
        }
        total_bytes += e.usize;
        if (total_bytes > MPKG_ZIP_MAX_TOTAL_BYTES) {
            mpkg_err(err, err_cap, "zip expands past the %u-byte cap", MPKG_ZIP_MAX_TOTAL_BYTES);
            return 0;
        }
        if (e.name_len == 12 && memcmp(e.name, "package.json", 12) == 0) has_marker = 1;
        files++;
    }
    if (!has_marker) {
        mpkg_err(err, err_cap, "payload is not a package (no top-level package.json)");
        return 0;
    }
    *files_out = files;
    return 1;
}

int sh_mpkg_unpack(const unsigned char *payload, size_t len, const char *dest_dir,
                   unsigned *files_out, char *err, size_t err_cap)
{
    const unsigned char *cd, *cursor;
    unsigned count, i, files = 0;
    mpkg_zentry e;
    char rel[MAX_PATH];

    mpkg_err(err, err_cap, "");
    if (files_out) *files_out = 0;
    if (!payload || !dest_dir || !dest_dir[0]) { mpkg_err(err, err_cap, "bad arguments"); return 0; }

    /* The whole archive is vetted BEFORE the first byte is written, so an
     * unsafe path refuses the install with nothing on disk. */
    if (!mpkg_zip_survey(payload, len, &files, err, err_cap)) return 0;

    if (!CreateDirectoryA(dest_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        mpkg_err(err, err_cap, "cannot create '%s' (err %lu)", dest_dir, GetLastError());
        return 0;
    }
    if (!mpkg_zip_open(payload, len, &cd, &count, err, err_cap)) return 0;
    cursor = cd;
    for (i = 0; i < count; i++) {
        if (!mpkg_zip_entry(payload, len, &cursor, &e, err, err_cap)) return 0;
        if (e.name_len >= sizeof rel) { mpkg_err(err, err_cap, "member path too long"); return 0; }
        memcpy(rel, e.name, e.name_len);
        rel[e.name_len] = '\0';
        if (e.is_dir) {
            rel[e.name_len - 1] = '\0';   /* drop the trailing '/' */
            if (rel[0] && !mpkg_member_path_safe(rel, e.name_len - 1)) {
                mpkg_err(err, err_cap, "unsafe member path in package: '%s'", rel);
                return 0;
            }
            if (rel[0] && !mpkg_make_parents(dest_dir, rel, 1)) {
                mpkg_err(err, err_cap, "cannot create directory '%s'", rel);
                return 0;
            }
            continue;
        }
        if (!mpkg_make_parents(dest_dir, rel, 0)) {
            mpkg_err(err, err_cap, "cannot create parents for '%s'", rel);
            return 0;
        }
        if (e.method == 0) {
            if (e.csize != e.usize) { mpkg_err(err, err_cap, "stored member size mismatch"); return 0; }
            if (!mpkg_write_file(dest_dir, rel, e.data, e.usize)) {
                mpkg_err(err, err_cap, "cannot write '%s'", rel);
                return 0;
            }
        } else {   /* method 8: raw deflate through the shared decoder */
            unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                                            e.usize ? e.usize : 1);
            if (!buf) { mpkg_err(err, err_cap, "out of memory"); return 0; }
            /* usize==0: write an empty file without decoding (a deflated empty
             * member still carries a 2-byte stream; its content is moot). */
            if (e.usize != 0 &&
                sh_inflate_raw(e.data, e.csize, buf, e.usize) != e.usize) {
                HeapFree(GetProcessHeap(), 0, buf);
                mpkg_err(err, err_cap, "deflate stream for '%s' is malformed", rel);
                return 0;
            }
            if (!mpkg_write_file(dest_dir, rel, buf, e.usize)) {
                HeapFree(GetProcessHeap(), 0, buf);
                mpkg_err(err, err_cap, "cannot write '%s'", rel);
                return 0;
            }
            HeapFree(GetProcessHeap(), 0, buf);
        }
    }
    if (files_out) *files_out = files;
    return 1;
}

/* ==================================================================== */
/* the boot snapshot + session state                                     */
/* ==================================================================== */

#define MPKG_SIDECAR_NAME "smpkg.digest"
#define MPKG_SESSION_MAX  32

typedef struct mpkg_boot_pkg {
    char folded[SH_PACKAGE_NAME_CAP];   /* lowercased, '/'->'-' */
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    int  has_digest;
} mpkg_boot_pkg;

typedef struct mpkg_session_entry {
    char id[SH_MPKG_ID_CAP];
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    int  outcome;   /* 1 installed, 0 declined, 2 prompt in flight */
} mpkg_session_entry;

static CRITICAL_SECTION g_mpkg_lock;
static INIT_ONCE        g_mpkg_lock_once = INIT_ONCE_STATIC_INIT;

static int           g_boot_captured = 0;
static char          g_data_root[MAX_PATH] = {0};
static mpkg_boot_pkg g_boot[SH_PACKAGES_MAX];
static size_t        g_boot_count = 0;

static mpkg_session_entry g_session[MPKG_SESSION_MAX];
static size_t             g_session_count = 0;

static int g_consent_mode = -1;   /* SH_MPKG_CONSENT_PROMPT */
static char g_last_refusal[SH_MPKG_ERR_CAP] = "";

static BOOL CALLBACK mpkg_lock_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_mpkg_lock);
    return TRUE;
}

static void mpkg_lock(void)   { InitOnceExecuteOnce(&g_mpkg_lock_once, mpkg_lock_init, NULL, NULL); EnterCriticalSection(&g_mpkg_lock); }
static void mpkg_unlock(void) { LeaveCriticalSection(&g_mpkg_lock); }

static void mpkg_fold_name(const char *name, char *out, size_t cap)
{
    size_t i;
    for (i = 0; name[i] && i < cap - 1; i++) {
        char c = name[i];
        if (c == '/' || c == '\\') c = '-';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[i] = '\0';
}

static int mpkg_read_sidecar(const char *pkg_root, char *digest_out)
{
    char path[MAX_PATH];
    char buf[SH_MPKG_DIGEST_CHARS + 1];
    HANDLE h;
    DWORD rd = 0;
    size_t i;
    if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", pkg_root, MPKG_SIDECAR_NAME) < 0)
        return 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, SH_MPKG_DIGEST_CHARS, &rd, NULL) || rd != SH_MPKG_DIGEST_CHARS) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    for (i = 0; i < SH_MPKG_DIGEST_CHARS; i++)
        if (!sh_shard_is_hex(buf[i])) return 0;
    memcpy(digest_out, buf, SH_MPKG_DIGEST_CHARS);
    digest_out[SH_MPKG_DIGEST_CHARS] = '\0';
    return 1;
}

void sh_mpkg_boot_capture(const char *data_root)
{
    sh_package pkgs[SH_PACKAGES_MAX];
    size_t count = 0, i;
    char line[256];

    if (!data_root || !data_root[0]) return;
    mpkg_lock();
    if (g_boot_captured) { mpkg_unlock(); return; }   /* first capture wins: it is the BOOT state */
    strncpy_s(g_data_root, sizeof g_data_root, data_root, _TRUNCATE);
    if (!sh_packages_enumerate(data_root, pkgs, SH_PACKAGES_MAX, &count)) {
        g_boot_count = 0;
        /* Keep the failed launch snapshot terminal; a later disk scan must
         * never become an implicit consent decision. */
        g_boot_captured = -1;
        mpkg_unlock();
        backend_log("MPKG: boot inventory refused -- package enumeration was incomplete");
        return;
    }
    g_boot_count = 0;
    for (i = 0; i < count && g_boot_count < SH_PACKAGES_MAX; i++) {
        mpkg_boot_pkg *b = &g_boot[g_boot_count++];
        mpkg_fold_name(pkgs[i].name, b->folded, sizeof b->folded);
        b->has_digest = mpkg_read_sidecar(pkgs[i].root, b->digest);
        if (!b->has_digest) b->digest[0] = '\0';
    }
    g_boot_captured = 1;
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: boot snapshot captured -- %zu package(s) under %s\\overrides",
        g_boot_count, data_root);
    backend_log(line);
}

/* Name equivalence between a declared id and a boot package's folded name:
 * equal, or one ends with "-" + the other (grouping-folder prefixes fold to
 * leading "<group>-"). Generous by design -- see map_package.h. */
static int mpkg_names_match(const char *declared, const char *folded)
{
    size_t dn = strlen(declared), fn = strlen(folded);
    if (dn == 0 || fn == 0) return 0;
    if (strcmp(declared, folded) == 0) return 1;
    if (dn > fn + 1 && declared[dn - fn - 1] == '-' &&
        strcmp(declared + (dn - fn), folded) == 0) return 1;
    if (fn > dn + 1 && folded[fn - dn - 1] == '-' &&
        strcmp(folded + (fn - dn), declared) == 0) return 1;
    return 0;
}

/* Read the digest sidecar an install leaves beside a package. Returns 0 when the
 * package predates the sidecar or it cannot be read -- in which case its content
 * identity is simply unknown, which is different from known-and-different. */
static int mpkg_read_sidecar_digest(const char *package_root, char *out, size_t out_cap)
{
    char path[MAX_PATH];
    HANDLE handle;
    DWORD got = 0;

    if (!package_root || !out || out_cap <= SH_MPKG_DIGEST_CHARS) return 0;
    if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", package_root,
                    MPKG_SIDECAR_NAME) < 0) return 0;
    handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(handle, out, SH_MPKG_DIGEST_CHARS, &got, NULL) ||
        got != SH_MPKG_DIGEST_CHARS) {
        CloseHandle(handle);
        return 0;
    }
    CloseHandle(handle);
    out[SH_MPKG_DIGEST_CHARS] = '\0';
    return 1;
}

static int mpkg_boot_satisfies(const sh_mpkg_decl *d)
{
    size_t i;
    for (i = 0; i < g_boot_count; i++) {
        const mpkg_boot_pkg *b = &g_boot[i];
        if (b->has_digest && strcmp(b->digest, d->digest) == 0) return 1;
        if (mpkg_names_match(d->id, b->folded)) {
            if (b->has_digest && strcmp(b->digest, d->digest) != 0)
                continue;   /* same name, different version: not satisfied */
            return 1;
        }
    }
    return 0;
}

/* (lock held) */
static mpkg_session_entry *mpkg_session_find(const char *id, const char *digest)
{
    size_t i;
    for (i = 0; i < g_session_count; i++)
        if (strcmp(g_session[i].id, id) == 0 && strcmp(g_session[i].digest, digest) == 0)
            return &g_session[i];
    return NULL;
}

/* (lock held) */
static mpkg_session_entry *mpkg_session_add(const char *id, const char *digest, int outcome)
{
    mpkg_session_entry *e;
    if (g_session_count >= MPKG_SESSION_MAX) return NULL;
    e = &g_session[g_session_count++];
    strcpy_s(e->id, sizeof e->id, id);
    strcpy_s(e->digest, sizeof e->digest, digest);
    e->outcome = outcome;
    return e;
}

/* ==================================================================== */
/* install + consent                                                     */
/* ==================================================================== */

/* Stage every missing package in a chain for one consent decision. */
typedef struct mpkg_staged {
    char id[SH_MPKG_ID_CAP];
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    unsigned char *payload;
    size_t payload_len;
    unsigned files;
    struct mpkg_staged *next;
} mpkg_staged;

/* Frees the whole chain from `s` onward. */
static void mpkg_staged_free(mpkg_staged *s)
{
    while (s) {
        mpkg_staged *next = s->next;
        if (s->payload) HeapFree(GetProcessHeap(), 0, s->payload);
        HeapFree(GetProcessHeap(), 0, s);
        s = next;
    }
}

static unsigned mpkg_staged_count(const mpkg_staged *s)
{
    unsigned n = 0;
    for (; s; s = s->next) n++;
    return n;
}

static size_t mpkg_staged_bytes(const mpkg_staged *s)
{
    size_t n = 0;
    for (; s; s = s->next) n += s->payload_len;
    return n;
}

static unsigned mpkg_staged_files(const mpkg_staged *s)
{
    unsigned n = 0;
    for (; s; s = s->next) n += s->files;
    return n;
}

/* Perform the consented install. Returns 1 on success (and records it as
 * installed-this-session), 0 with `err` filled. Never touches an existing
 * folder: the user's overrides tree is not ours to overwrite. */
static int mpkg_install_staged(const mpkg_staged *s, char *err, size_t err_cap)
{
    char dest[MAX_PATH], sidecar[MAX_PATH], line[512];
    char root[MAX_PATH];
    unsigned files = 0;
    DWORD attrs;

    mpkg_lock();
    strncpy_s(root, sizeof root, g_data_root, _TRUNCATE);
    mpkg_unlock();
    if (!root[0]) { mpkg_err(err, err_cap, "no data root captured"); return 0; }
    if (_snprintf_s(dest, sizeof dest, _TRUNCATE, "%s\\overrides\\%s", root, s->id) < 0) {
        mpkg_err(err, err_cap, "destination path too long");
        return 0;
    }
    attrs = GetFileAttributesA(dest);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        /* Report a differing installed digest as a version clash. Never
         * overwrite an occupied folder, which another map may depend on.
         */
        char installed[SH_MPKG_DIGEST_CHARS + 1];
        if (mpkg_read_sidecar_digest(dest, installed, sizeof installed) &&
            strcmp(installed, s->digest) != 0) {
            mpkg_err(err, err_cap,
                     "a DIFFERENT version of '%s' is already installed (has %s, this map needs "
                     "%s). Remove or rename the installed one to use this map's version",
                     s->id, installed, s->digest);
        } else {
            mpkg_err(err, err_cap, "'%s' already exists on disk; not overwriting it", dest);
        }
        return 0;
    }
    if (!sh_mpkg_unpack(s->payload, s->payload_len, dest, &files, err, err_cap))
        return 0;
    /* Record the payload digest beside the package so future gates match by
     * content, not by name. */
    if (_snprintf_s(sidecar, sizeof sidecar, _TRUNCATE, "%s\\%s", dest, MPKG_SIDECAR_NAME) >= 0) {
        HANDLE h = CreateFileA(sidecar, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr;
            WriteFile(h, s->digest, SH_MPKG_DIGEST_CHARS, &wr, NULL);
            CloseHandle(h);
        }
    }
    mpkg_lock();
    {
        mpkg_session_entry *e = mpkg_session_find(s->id, s->digest);
        if (e) e->outcome = 1;
        else mpkg_session_add(s->id, s->digest, 1);
    }
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: package '%s' (digest %s) INSTALLED to %s -- %u file(s); "
        "requesting a runtime re-arm, no restart needed", s->id, s->digest, dest, files);
    backend_log(line);

    /* Request a synchronous registration pass on the next engine tick. */
    sh_decl_server_request_rearm();
    return 1;
}

static void mpkg_record_decline(const char *id, const char *digest)
{
    char line[256];
    mpkg_lock();
    {
        mpkg_session_entry *e = mpkg_session_find(id, digest);
        if (e) e->outcome = 0;
        else mpkg_session_add(id, digest, 0);
    }
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: user DECLINED install of package '%s' (digest %s); "
        "it will not be asked again this session", id, digest);
    backend_log(line);
}

/* Consent runs as a state machine on the engine main-thread tick, which owns
 * dialog queue writes and answer reads. The modal uses our text and button
 * set; no OS fallback is used.
 */
/* Use GDM_CONFIRM_VIDEO_CHANGES as the dialog shape. GDM ids can have native
 * affirmative actions; do not borrow a destructive prompt such as delete-map.
 */
#define MPKG_CONSENT_GDM_ID      0x29u   /* GDM_CONFIRM_VIDEO_CHANGES */
#define MPKG_CONSENT_BUTTON_SET  6u      /* native Yes/No button set */

enum {
    MPKG_CONSENT_IDLE = 0,
    MPKG_CONSENT_RAISE,      /* staged; raise on the next tick */
    MPKG_CONSENT_WAITING     /* on screen; poll for the answer */
};

static volatile LONG  g_consent_state;
static mpkg_staged   *g_consent_staged;      /* owned while not IDLE */
static int            g_consent_ticket;
static volatile LONG  g_consent_waited;      /* ticks spent waiting for the surface */

/* Bound the wait for a captured engine dialog manager; on timeout install
 * nothing.
 */
#define MPKG_CONSENT_WAIT_TICKS 600

static void mpkg_consent_finish(int accepted)
{
    mpkg_staged *s = g_consent_staged;

    g_consent_staged = NULL;
    g_consent_ticket = 0;
    InterlockedExchange(&g_consent_state, MPKG_CONSENT_IDLE);
    if (!s) return;

    if (accepted) {
        /* Attempt each consented package and record failures independently. */
        mpkg_staged *item;
        for (item = s; item; item = item->next) {
            char err[SH_MPKG_ERR_CAP];
            if (mpkg_install_staged(item, err, sizeof err)) continue;
            {
                char line[512];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                            "MPKG: install of package '%s' FAILED: %s", item->id, err);
                backend_log(line);
            }
            mpkg_lock();
            {
                mpkg_session_entry *e = mpkg_session_find(item->id, item->digest);
                if (e && e->outcome == 2) e->outcome = 0;   /* failed = do not re-prompt */
            }
            mpkg_unlock();
        }
    } else {
        mpkg_staged *item;
        for (item = s; item; item = item->next)
            mpkg_record_decline(item->id, item->digest);
    }
    mpkg_staged_free(s);
}

void sh_mpkg_consent_poll(void)
{
    LONG state = InterlockedCompareExchange(&g_consent_state, 0, 0);
    char text[256];
    mpkg_staged *s;

    if (state == MPKG_CONSENT_IDLE) return;
    s = g_consent_staged;
    if (!s) { InterlockedExchange(&g_consent_state, MPKG_CONSENT_IDLE); return; }

    if (state == MPKG_CONSENT_RAISE) {
        if (!sh_engine_dialog_can_ask()) {
            if (InterlockedIncrement(&g_consent_waited) > MPKG_CONSENT_WAIT_TICKS) {
                backend_log("MPKG: the engine dialog surface did not become ready and idle; "
                            "nothing was installed and consent was never asked");
                mpkg_consent_finish(0);
            }
            return;
        }
        /* Keep the modal body within its 256-byte native string; details stay
         * in the log.
         */
        {
            unsigned count = mpkg_staged_count(s);
            unsigned kb = (unsigned)((mpkg_staged_bytes(s) + 1023) / 1024);
            unsigned files = mpkg_staged_files(s);
            if (count == 1) {
                _snprintf_s(text, sizeof text, _TRUNCATE,
                            "This map brings its own mod package:  %s  (%u files, %u KB).  "
                            "It has to be installed before the map can load.  Install it now?",
                            s->id, files, kb);
            } else {
                /* List names that fit and report the remainder count. */
                char names[168];
                unsigned listed = 0;
                mpkg_staged *item;
                names[0] = '\0';
                for (item = s; item; item = item->next) {
                    size_t used = strlen(names);
                    if (used + strlen(item->id) + 4 >= sizeof names) break;
                    if (used) strncat_s(names, sizeof names, ", ", _TRUNCATE);
                    strncat_s(names, sizeof names, item->id, _TRUNCATE);
                    listed++;
                }
                if (listed < count) {
                    char more[32];
                    _snprintf_s(more, sizeof more, _TRUNCATE, " and %u more", count - listed);
                    strncat_s(names, sizeof names, more, _TRUNCATE);
                }
                _snprintf_s(text, sizeof text, _TRUNCATE,
                            "This map brings %u mod packages:  %s  (%u files, %u KB).  "
                            "They have to be installed before the map can load.  Install them now?",
                            count, names, files, kb);
            }
        }
        g_consent_ticket = sh_engine_dialog_ask(MPKG_CONSENT_GDM_ID,
                                                MPKG_CONSENT_BUTTON_SET, text);
        if (!g_consent_ticket) {
            backend_log("MPKG: the engine dialog would not raise; installing nothing, because "
                        "third-party content is never installed without an answer");
            mpkg_consent_finish(0);
            return;
        }
        InterlockedExchange(&g_consent_state, MPKG_CONSENT_WAITING);
        return;
    }

    switch (sh_engine_dialog_poll(g_consent_ticket)) {
    case SH_ENGINE_DIALOG_PENDING:
        return;
    case SH_ENGINE_DIALOG_ACCEPTED:
        mpkg_consent_finish(1);
        return;
    default:
        mpkg_consent_finish(0);
        return;
    }
}

/* Take ownership of the staged chain and request engine-modal consent. If the
 * engine cannot ask, decline without installing.
 */
static void mpkg_request_consent(mpkg_staged *s)
{
    int mode;

    mpkg_lock();
    mode = g_consent_mode;
    mpkg_unlock();

    if (mode == 1) {          /* test seam: synchronous accept */
        char err[SH_MPKG_ERR_CAP];
        if (!mpkg_install_staged(s, err, sizeof err)) {
            char line[512];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: install of package '%s' FAILED: %s", s->id, err);
            backend_log(line);
            mpkg_lock();
            {
                mpkg_session_entry *e = mpkg_session_find(s->id, s->digest);
                if (e && e->outcome == 2) e->outcome = 0;
            }
            mpkg_unlock();
        }
        mpkg_staged_free(s);
        return;
    }
    if (mode == 0) {          /* test seam: synchronous decline */
        mpkg_record_decline(s->id, s->digest);
        mpkg_staged_free(s);
        return;
    }

    if (InterlockedCompareExchange(&g_consent_state, MPKG_CONSENT_RAISE,
                                   MPKG_CONSENT_IDLE) != MPKG_CONSENT_IDLE) {
        backend_log("MPKG: a consent dialog is already up; this one is declined rather than queued");
        mpkg_record_decline(s->id, s->digest);
        mpkg_staged_free(s);
        return;
    }
    g_consent_staged = s;
    InterlockedExchange(&g_consent_waited, 0);
    backend_log("MPKG: consent will be asked through the engine's own dialog on the next tick");
}

/* ==================================================================== */
/* THE GATE                                                              */
/* ==================================================================== */

static void mpkg_set_refusal(const char *reason)
{
    char line[SH_MPKG_ERR_CAP + 32];
    mpkg_lock();
    strncpy_s(g_last_refusal, sizeof g_last_refusal, reason, _TRUNCATE);
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: load REFUSED -- %s", reason);
    backend_log(line);
}

int sh_mpkg_gate(const char *json, size_t len)
{
    sh_mpkg_decl decls[SH_MPKG_MAX_PACKAGES];
    size_t count, i;
    int overflow = 0;
    size_t missing_count = 0;
    const sh_mpkg_decl *first_missing = NULL;
    size_t session_installed_count = 0;
    char reason[SH_MPKG_ERR_CAP];

    if (!json || len == 0) return 1;

    /* The fast path: one substring sweep, no allocation. */
    if (!sh_shard_find(json, len, MPKG_HEADER_MAGIC, MPKG_MAGIC_LEN)) return 1;

    count = mpkg_scan_internal(json, len, decls, SH_MPKG_MAX_PACKAGES, &overflow);
    if (count == 0 && !overflow) return 1;   /* "smpkg." was prose, not a shard header */
    if (overflow) {
        mpkg_set_refusal("map declares more packages than the gate can vet");
        return 0;
    }
    if (g_boot_captured != 1) {
        /* A declared package requires a captured boot state; refuse otherwise. */
        mpkg_set_refusal("map declares packages but the boot package snapshot is missing");
        return 0;
    }

    mpkg_lock();
    for (i = 0; i < count; i++) {
        const sh_mpkg_decl *d = &decls[i];
        mpkg_session_entry *e;
        if (mpkg_boot_satisfies(d)) continue;
        e = mpkg_session_find(d->id, d->digest);
        missing_count++;
        if (e && e->outcome == 1) session_installed_count++;
        if (!first_missing) {
            first_missing = d;
        }
    }
    mpkg_unlock();

    if (missing_count == 0) return 1;   /* everything already installed: silent pass */

    if (session_installed_count == missing_count) {
        /* Session installs pass only after declaration registration reports success. */
        if (sh_decl_server_registration_succeeded()) {
            backend_log("MPKG: package installed and registered at runtime this session; "
                        "allowing the load without a restart");
            return 1;
        }
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
            "package '%s' was installed this session but its registration is incomplete or failed; "
            "check the package registration log before retrying", first_missing->id);
        mpkg_set_refusal(reason);
        return 0;
    }

    _snprintf_s(reason, sizeof reason, _TRUNCATE,
        "map requires %zu uninstalled package(s); first: '%s' (digest %s, %u/%u shards)",
        missing_count, first_missing->id, first_missing->digest,
        first_missing->present, first_missing->total);
    mpkg_set_refusal(reason);

    /* Stage all extractable missing packages together. Skip prior decisions,
     * in-flight prompts and failed extractions, recording each outcome.
     */
    {
        mpkg_staged *head = NULL, *tail = NULL;
        size_t i;

        for (i = 0; i < count; i++) {
            const sh_mpkg_decl *d = &decls[i];
            mpkg_session_entry *e;
            int should_offer = 0;
            char err[SH_MPKG_ERR_CAP];
            size_t payload_len = 0;
            unsigned char *payload;
            unsigned files = 0;

            if (mpkg_boot_satisfies(d)) continue;

            mpkg_lock();
            e = mpkg_session_find(d->id, d->digest);
            if (!e && mpkg_session_add(d->id, d->digest, 2))
                should_offer = 1;              /* marked in-flight */
            mpkg_unlock();
            if (!should_offer) continue;

            payload = sh_mpkg_extract(json, len, d->id, &payload_len, err, sizeof err);
            if (payload && !mpkg_zip_survey(payload, payload_len, &files, err, sizeof err)) {
                HeapFree(GetProcessHeap(), 0, payload);
                payload = NULL;
            }
            if (!payload) {
                char line[SH_MPKG_ERR_CAP + 96];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "MPKG: package '%s' cannot be offered for install -- %s", d->id, err);
                backend_log(line);
                mpkg_lock();
                e = mpkg_session_find(d->id, d->digest);
                if (e && e->outcome == 2) e->outcome = 0;   /* nothing installable: don't re-ask */
                mpkg_unlock();
                continue;
            }
            {
                mpkg_staged *s = (mpkg_staged *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                          sizeof *s);
                if (!s) {
                    HeapFree(GetProcessHeap(), 0, payload);
                    continue;
                }
                strcpy_s(s->id, sizeof s->id, d->id);
                strcpy_s(s->digest, sizeof s->digest, d->digest);
                s->payload = payload;
                s->payload_len = payload_len;
                s->files = files;
                s->next = NULL;
                if (tail) tail->next = s; else head = s;
                tail = s;
            }
        }
        if (head) mpkg_request_consent(head);   /* takes ownership of the chain */
    }
    return 0;
}

/* ==================================================================== */
/* test seams                                                            */
/* ==================================================================== */

#ifdef SH_MAP_PACKAGE_TESTING
void sh_mpkg_test_set_consent_mode(int mode)
{
    mpkg_lock();
    g_consent_mode = mode;
    mpkg_unlock();
}

void sh_mpkg_test_reset(void)
{
    mpkg_lock();
    g_boot_captured = 0;
    g_data_root[0] = '\0';
    g_boot_count = 0;
    g_session_count = 0;
    g_last_refusal[0] = '\0';
    g_consent_mode = -1;
    mpkg_unlock();
}

const char *sh_mpkg_test_last_refusal(void)
{
    return g_last_refusal;
}

int sh_mpkg_test_session_installed_count(void)
{
    int n = 0;
    size_t i;
    mpkg_lock();
    for (i = 0; i < g_session_count; i++)
        if (g_session[i].outcome == 1) n++;
    mpkg_unlock();
    return n;
}
#endif
