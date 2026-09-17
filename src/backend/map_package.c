/* smpkg shard parsing, payload extraction and consent-gated installation. Use
 * bounded JSON spans without a DOM. Strict base64, header counts and Windows
 * path checks reject malformed delivery data.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <bcrypt.h>
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
#include "package_archive.h"
#include "package_runtime.h"
#include "backend_log.h"

static sh_package_legacy_reader g_legacy_reader;
static void *g_legacy_context;
void sh_mpkg_set_legacy_reader(sh_package_legacy_reader reader, void *context)
{ g_legacy_reader = reader; g_legacy_context = context; }

/* Verify the original carrier before conversion. The public extractor keeps
 * returning exact transport bytes for export and forensic tools. */
static unsigned char *mpkg_compiled_payload(const char *json, size_t length, const char *id,
    size_t *out_length, char *error, size_t capacity)
{
    unsigned char *original, *converted = NULL, *out;
    size_t converted_length = 0;
    original = sh_mpkg_extract(json, length, id, out_length, error, capacity);
    if (!original) return NULL;
    if (!sh_package_legacy_convert(original, *out_length, id, g_legacy_reader, g_legacy_context,
        &converted, &converted_length, error, capacity)) {
        HeapFree(GetProcessHeap(), 0, original); *out_length = 0; return NULL;
    }
    if (!converted) return original;
    out = HeapAlloc(GetProcessHeap(), 0, converted_length);
    if (out) memcpy(out, converted, converted_length);
    else if (error && capacity) snprintf(error, capacity, "cannot retain migrated package '%s'", id);
    free(converted); HeapFree(GetProcessHeap(), 0, original);
    *out_length = out ? converted_length : 0; return out;
}

/* ==================================================================== */
/* small text helpers                                                    */
/* ==================================================================== */

/* The smpkg package-id character class -- the one text rule that is this
 * family's and not the envelope's. Everything else lives in map_shards.c. */
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

    /* The final three separators delimit transport fields. Dots are valid
     * authored ID characters and never participate in implicit name folding. */
    {
        const char *separator = end;
        unsigned fields = 0;
        while (separator > c && fields < 3) {
            separator--;
            if (*separator == '.') fields++;
        }
        n = (size_t)(separator - c);
        if (fields != 3 || !n || n >= sizeof(hdr->id)) return 0;
        memcpy(hdr->id, c, n); hdr->id[n] = 0;
        if (!sh_package_id_normalize(hdr->id, hdr->id)) {
            char digest[SH_MPKG_DIGEST_CHARS + 1];
            size_t j;
            /* Beta 13 used folder identities and allowed leading/trailing
             * '-' and '_'. Decode those carriers with one stable modern id;
             * extraction still verifies the original payload's checksum. */
            if (n >= 100) return 0;
            for (j = 0; j < n; j++) {
                char ch = hdr->id[j];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return 0;
            }
            sh_shard_digest16((const unsigned char *)hdr->id, n, digest);
            snprintf(hdr->id, sizeof(hdr->id), "legacy.%s", digest);
        }
        c = separator + 1;
    }

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
    char canonical_id[SH_PACKAGE_ID_CAP];

    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = '\0';
    if (!json || len == 0 || !sh_package_id_normalize(pkg_id, canonical_id) || !payload) {
        mpkg_err(err, err_cap, "embed called with nothing to embed");
        return NULL;
    }
    pkg_id = canonical_id;
    if (payload_len == 0 || payload_len > SH_MPKG_MAX_PAYLOAD) {
        mpkg_err(err, err_cap, "package needs %zu bytes before map encoding; it cannot fit the native map string representation", payload_len);
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
    unsigned char *bits;
} mpkg_seen;

static int mpkg_seen_test_set(mpkg_seen *s, unsigned idx)
{
    unsigned char m = (unsigned char)(1u << (idx & 7));
    if (s->bits[idx >> 3] & m) return 1;
    s->bits[idx >> 3] |= m;
    return 0;
}

typedef struct mpkg_scan_entry { sh_mpkg_decl decl; mpkg_seen seen; } mpkg_scan_entry;

static size_t mpkg_scan_internal(const char *json, size_t len, sh_mpkg_decl **out)
{
    mpkg_scan_entry *entries = NULL;
    size_t count = 0, capacity = 0, pos = 0, i, result = SIZE_MAX;
    mpkg_hdr hdr;
    const char *chunk;
    size_t chunk_len;
    *out = NULL;
    if (!json) return 0;
    while (mpkg_next_shard(json, len, &pos, &hdr, &chunk, &chunk_len)) {
        sh_mpkg_decl *d;
        for (i = 0; i < count; i++) if (!strcmp(entries[i].decl.id, hdr.id)) break;
        if (i == count) {
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 16;
                mpkg_scan_entry *grown;
                if (next < capacity || next > SIZE_MAX / sizeof(*grown)) goto done;
                grown = (mpkg_scan_entry *)realloc(entries, next * sizeof(*grown));
                if (!grown) goto done;
                entries = grown; capacity = next;
            }
            memset(&entries[count], 0, sizeof(entries[count])); count++;
            d = &entries[i].decl;
            strcpy_s(d->id, sizeof(d->id), hdr.id);
            strcpy_s(d->digest, sizeof(d->digest), hdr.digest);
            d->total = hdr.total; d->consistent = 1;
            entries[i].seen.bits = (unsigned char *)calloc(((size_t)hdr.total + 7u) / 8u, 1);
            if (!entries[i].seen.bits) goto done;
        }
        d = &entries[i].decl;
        if (d->total != hdr.total || strcmp(d->digest, hdr.digest) ||
            hdr.idx >= d->total || !chunk || mpkg_seen_test_set(&entries[i].seen, hdr.idx)) {
            d->consistent = 0; continue;
        }
        d->present++;
    }
    if (count > SIZE_MAX / sizeof(**out) ||
        (count && !(*out = (sh_mpkg_decl *)malloc(count * sizeof(**out))))) goto done;
    for (i = 0; i < count; i++) {
        entries[i].decl.complete = entries[i].decl.consistent &&
            entries[i].decl.present == entries[i].decl.total;
        (*out)[i] = entries[i].decl;
    }
    result = count;
done:
    for (i = 0; i < count; i++) free(entries[i].seen.bits);
    free(entries); return result;
}

size_t sh_mpkg_scan(const char *json, size_t len, sh_mpkg_decl *out, size_t cap)
{
    sh_mpkg_decl *decls = NULL;
    size_t count = mpkg_scan_internal(json, len, &decls);
    if (count != SIZE_MAX && count <= cap && (!count || out)) {
        if (count) memcpy(out, decls, count * sizeof(*out));
    } else count = SIZE_MAX;
    free(decls); return count;
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
                     "the map carries two different contents", pkg_id);
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

static int mpkg_zip_survey(const unsigned char *payload, size_t len,
                           unsigned *files, char *err, size_t capacity)
{ return sh_package_archive_inspect(payload, len, NULL, files, err, capacity); }

int sh_mpkg_unpack(const unsigned char *payload, size_t len, const char *destination,
                   unsigned *files, char *err, size_t capacity)
{ return sh_package_archive_unpack(payload, len, destination, files, err, capacity); }

/* ==================================================================== */
/* the boot snapshot + session state                                     */
/* ==================================================================== */

typedef struct mpkg_boot_pkg {
    char id[SH_MPKG_ID_CAP];
    char root[MAX_PATH];
    unsigned char fingerprint[32];
} mpkg_boot_pkg;

typedef struct mpkg_session_entry {
    char id[SH_MPKG_ID_CAP];
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    char root[MAX_PATH];
    unsigned char fingerprint[32];
    int  outcome;   /* 1 active, 2 prompt, 3 retry on a later load, 4 activating */
} mpkg_session_entry;

static CRITICAL_SECTION g_mpkg_lock;
static INIT_ONCE        g_mpkg_lock_once = INIT_ONCE_STATIC_INIT;

static int           g_boot_captured = 0;
static char          g_data_root[MAX_PATH] = {0};
static mpkg_boot_pkg *g_boot;
static size_t        g_boot_count = 0;

static mpkg_session_entry *g_session;
static size_t             g_session_count, g_session_capacity;

static int g_consent_mode = -1;   /* SH_MPKG_CONSENT_PROMPT */
static char g_last_refusal[SH_MPKG_ERR_CAP] = "";
static char g_notice[256];
static unsigned g_notice_revision;
static int g_notice_ticket;

/* The engine tick owns installation and activation. Keep the cross-process
 * lock until activation commits or the entire new group leaves overrides. */
static struct {
    HANDLE mutex;
    char working[MAX_PATH], marker[MAX_PATH];
    int active, failed;
} g_install;
static int mpkg_recover_interrupted(const char *root, char *error, size_t capacity);

static BOOL CALLBACK mpkg_lock_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_mpkg_lock);
    return TRUE;
}

static void mpkg_lock(void)   { InitOnceExecuteOnce(&g_mpkg_lock_once, mpkg_lock_init, NULL, NULL); EnterCriticalSection(&g_mpkg_lock); }
static void mpkg_unlock(void) { LeaveCriticalSection(&g_mpkg_lock); }

void sh_mpkg_report_error(const char *message)
{
    if (!message || !*message) return;
    backend_log(message);
    mpkg_lock();
    strncpy_s(g_notice, sizeof(g_notice), message, _TRUNCATE); g_notice_revision++;
    mpkg_unlock();
}

static void mpkg_notice_poll(void)
{
    char message[256]; unsigned revision;
    if (g_notice_ticket) {
        if (sh_engine_dialog_poll(g_notice_ticket) == SH_ENGINE_DIALOG_PENDING) return;
        sh_engine_dialog_release(g_notice_ticket); g_notice_ticket = 0;
    }
    if (!sh_engine_dialog_can_ask()) return;
    mpkg_lock(); strcpy_s(message, sizeof(message), g_notice); revision = g_notice_revision; mpkg_unlock();
    if (!message[0]) return;
    g_notice_ticket = sh_engine_dialog_ask(0x29u, 1u, message);
    if (g_notice_ticket) { mpkg_lock(); if (revision == g_notice_revision) g_notice[0] = 0; mpkg_unlock(); }
}

/* Local authoring defects exclude only their own outer package. Map-carried
 * bundles are still read strictly before any installation. */
static int mpkg_local_rejected(void *context, const sh_package *package, const char *reason)
{
    char line[1024];
    (void)context;
    snprintf(line, sizeof(line), "MPKG: local package %s is excluded until repaired: %s", package->root, reason);
    backend_log(line);
    return 1;
}

void sh_mpkg_boot_capture(const char *data_root)
{
    sh_package_sources *sources;
    size_t i;
    char error[512] = "";
    if (!data_root || !data_root[0]) return;
    mpkg_lock();
    if (g_boot_captured == 1) { mpkg_unlock(); return; }
    strncpy_s(g_data_root, sizeof(g_data_root), data_root, _TRUNCATE);
    if (!mpkg_recover_interrupted(data_root, error, sizeof(error))) {
        g_boot_captured = -1; mpkg_unlock(); sh_mpkg_report_error(error); return;
    }
    sources = sh_package_sources_scan_local(data_root, mpkg_local_rejected, NULL, error, sizeof(error));
    if (!sources) {
        g_boot_captured = -1; mpkg_unlock(); backend_log(error); return;
    }
    if (sources->package_count > SIZE_MAX / sizeof(*g_boot) ||
        (sources->package_count && !(g_boot = (mpkg_boot_pkg *)calloc(sources->package_count, sizeof(*g_boot))))) {
        sh_package_sources_free(sources); g_boot_captured = -1; mpkg_unlock();
        backend_log("MPKG: cannot allocate authored package startup identities"); return;
    }
    g_boot_count = 0;
    for (i = 0; i < sources->package_count; i++) {
        size_t j;
        mpkg_boot_pkg *entry;
        entry = &g_boot[g_boot_count];
        for (j = 0; j < sources->component_count; j++) if (sources->components[j].owner == i && !sources->components[j].relative[0]) {
            strcpy_s(entry->id, sizeof(entry->id), sources->components[j].descriptor.id); break;
        }
        strcpy_s(entry->root, sizeof(entry->root), sources->packages[i].root);
        memcpy(entry->fingerprint, sources->fingerprints[i], 32); g_boot_count++;
    }
    sh_package_sources_free(sources); g_boot_captured = 1; mpkg_unlock();
    backend_log("MPKG: exact authored package identities captured; no installation sidecars");
}

int sh_mpkg_startup_ready(void)
{
    int ready;
    mpkg_lock(); ready = g_boot_captured == 1; mpkg_unlock(); return ready;
}

static int mpkg_source_satisfies(const char *root, const unsigned char fingerprint[32])
{
    char error[256];
    sh_package_sources *sources = sh_package_sources_scan_directory(root, error, sizeof(error));
    int matches = sources && !memcmp(sources->fingerprints[0], fingerprint, 32);
    sh_package_sources_free(sources); return matches;
}

/* 1: unchanged startup source; 2: matching runtime source requiring registration.
 * ZIP compression, timestamps and member order are transport details. */
static int mpkg_installed_kind(const sh_mpkg_decl *d, const unsigned char fingerprint[32])
{
    size_t i;
    for (i = 0; i < g_boot_count; i++) {
        const mpkg_boot_pkg *entry = &g_boot[i];
        if (!strcmp(entry->id, d->id) && !memcmp(entry->fingerprint, fingerprint, 32) &&
            mpkg_source_satisfies(entry->root, fingerprint)) return 1;
    }
    for (i = 0; i < g_session_count; i++) {
        mpkg_session_entry *entry = &g_session[i];
        if (entry->outcome != 1 || strcmp(entry->id, d->id) || memcmp(entry->fingerprint, fingerprint, 32)) continue;
        if (mpkg_source_satisfies(entry->root, fingerprint)) return 2;
        entry->outcome = 3;
    }
    /* Authors can edit or add packages and refresh them without restarting.
     * The boot snapshot and install history do not describe those sources.
     * Match the current compilation and revalidate its complete on-disk tree;
     * the caller still requires successful runtime registration before load. */
    {
        const sh_package_compilation *compiled = sh_package_runtime_library_acquire();
        const sh_package_sources *sources = compiled ? compiled->sources : NULL;
        int matches = 0;
        if (sources) for (i = 0; i < sources->component_count; i++) {
            const sh_package_component *component = &sources->components[i];
            if (component->relative[0] || strcmp(component->descriptor.id, d->id) ||
                memcmp(sources->fingerprints[component->owner], fingerprint, 32)) continue;
            matches = mpkg_source_satisfies(sources->packages[component->owner].root, fingerprint);
            if (matches) break;
        }
        sh_package_runtime_release();
        if (matches) return 2;
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
    if (g_session_count == g_session_capacity) {
        size_t capacity = g_session_capacity ? g_session_capacity * 2 : 16;
        mpkg_session_entry *entries;
        if (capacity < g_session_capacity || capacity > SIZE_MAX / sizeof(*entries)) return NULL;
        entries = (mpkg_session_entry *)realloc(g_session, capacity * sizeof(*entries));
        if (!entries) return NULL;
        g_session = entries; g_session_capacity = capacity;
    }
    e = &g_session[g_session_count++];
    memset(e, 0, sizeof(*e));
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
    sh_mpkg_install_completion completion;
    void *completion_context;
    char id[SH_MPKG_ID_CAP];
    char digest[SH_MPKG_DIGEST_CHARS + 1];
    unsigned char *payload;
    size_t payload_len;
    unsigned files;
    char destination[MAX_PATH], staging[MAX_PATH];
    unsigned char fingerprint[32];
    int existing;
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

#ifdef SH_MAP_PACKAGE_TESTING
static sh_mpkg_test_publish_fn g_publish_test;
static void (*g_prepare_test)(const char *package);
#endif

/* No copy fallback or destination replacement: one same-volume directory
 * rename publishes the complete set. Successful activation commits it. */
static int mpkg_publish(const char *staging, const char *destination)
{
#ifdef SH_MAP_PACKAGE_TESTING
    if (g_publish_test) return g_publish_test(staging, destination);
#endif
    return MoveFileExA(staging, destination, MOVEFILE_WRITE_THROUGH) != 0;
}

/* Delete only our unpublished working directory. Never follow redirected
 * entries, including a directory replaced by a junction during enumeration. */
static int mpkg_discard_tree(const wchar_t *path)
{
    WIN32_FIND_DATAW found;
    HANDLE search;
    DWORD attributes = GetFileAttributesW(path), last;
    size_t length = wcslen(path), capacity = length + MAX_PATH + 2u;
    wchar_t *child;
    int ok = 1;
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path) != 0;
    if (capacity > SH_PACKAGE_SOURCE_PATH_CAP) return 0;
    child = (wchar_t *)malloc(capacity * sizeof(*child));
    if (!child) return 0;
    swprintf_s(child, capacity, L"%s\\*", path);
    search = FindFirstFileW(child, &found);
    if (search == INVALID_HANDLE_VALUE) ok = GetLastError() == ERROR_FILE_NOT_FOUND;
    else {
        do {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            swprintf_s(child, capacity, L"%s\\%s", path, found.cFileName);
            if (!mpkg_discard_tree(child)) ok = 0;
        } while (FindNextFileW(search, &found));
        last = GetLastError(); FindClose(search);
        if (last != ERROR_NO_MORE_FILES) ok = 0;
    }
    free(child);
    return ok && RemoveDirectoryW(path);
}

static int mpkg_working_name(const char *name)
{
    size_t i;
    if (strlen(name) != 20 || strncmp(name, "map-", 4)) return 0;
    for (i = 4; i < 20; i++) if (!((name[i] >= '0' && name[i] <= '9') ||
                                 (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return 1;
}

static int mpkg_directory(const char *path);
static int mpkg_plain_ancestors(const char *path);

/* A delivered package lives at overrides\map-<16 hex>\<package>. Only those are
 * ours to supersede; anything else is the author's own tree and is never moved.
 * Returns the overrides-relative path of a delivered root. */
static int mpkg_delivered_relative(const char *root, const char *package_root,
    char *out, size_t capacity)
{
    char prefix[MAX_PATH], group[24];
    const char *rest, *slash;
    size_t length;
    if (snprintf(prefix, sizeof(prefix), "%s\\overrides\\", root) >= (int)sizeof(prefix)) return 0;
    length = strlen(prefix);
    if (_strnicmp(package_root, prefix, length)) return 0;
    rest = package_root + length;
    slash = strchr(rest, '\\');
    if (!slash || (size_t)(slash - rest) >= sizeof(group) || !slash[1]) return 0;
    memcpy(group, rest, (size_t)(slash - rest)); group[slash - rest] = 0;
    if (!mpkg_working_name(group) || strstr(rest, "..") || strchr(rest, ':')) return 0;
    return snprintf(out, capacity, "%s", rest) < (int)capacity;
}

/* Move one delivered package out of overrides into this batch's cancellation
 * area, recording where it came from. The pending marker is already written, so
 * an interrupted run restores it from that record instead of losing it. */
static int mpkg_supersede_one(const char *root, const char *batch, size_t index,
    const char *relative, char *error, size_t capacity)
{
    char area[MAX_PATH], slot[MAX_PATH], origin[MAX_PATH], content[MAX_PATH], source[MAX_PATH];
    HANDLE file;
    DWORD written = 0;
    size_t length = strlen(relative);
    if (snprintf(area, sizeof(area), "%s\\superseded", batch) >= (int)sizeof(area) ||
        snprintf(slot, sizeof(slot), "%s\\%zu", area, index) >= (int)sizeof(slot) ||
        snprintf(origin, sizeof(origin), "%s\\origin", slot) >= (int)sizeof(origin) ||
        snprintf(content, sizeof(content), "%s\\content", slot) >= (int)sizeof(content) ||
        snprintf(source, sizeof(source), "%s\\overrides\\%s", root, relative) >= (int)sizeof(source) ||
        !mpkg_directory(area) || !mpkg_directory(slot)) {
        mpkg_err(error, capacity, "cannot prepare the supersession record for '%s'", relative); return 0;
    }
    file = CreateFileA(origin, GENERIC_WRITE, 0, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        !WriteFile(file, relative, (DWORD)length, &written, NULL) || written != length ||
        !FlushFileBuffers(file)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        mpkg_err(error, capacity, "cannot record where '%s' came from", relative); return 0;
    }
    CloseHandle(file);
    if (!mpkg_plain_ancestors(source) || !MoveFileExA(source, content, MOVEFILE_WRITE_THROUGH)) {
        mpkg_err(error, capacity, "cannot retire the superseded package '%s'", relative); return 0;
    }
    return 1;
}

/* Read one recorded origin back. Only a plain relative path below overrides is
 * accepted; the record is ours, but it is still validated before any move. */
static int mpkg_superseded_origin(const char *slot, char *out, size_t capacity)
{
    char path[MAX_PATH];
    HANDLE file;
    DWORD size, read = 0;
    if (snprintf(path, sizeof(path), "%s\\origin", slot) >= (int)sizeof(path)) return 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || !size || size >= capacity ||
        !ReadFile(file, out, size, &read, NULL) || read != size) { CloseHandle(file); return 0; }
    CloseHandle(file); out[size] = 0;
    return !strstr(out, "..") && !strchr(out, ':') && out[0] != '\\' && strchr(out, '\\') != NULL;
}

/* Put every superseded package of an unfinished installation back exactly where
 * it was. Runs under the cross-process install lock, before anything is
 * deleted, so a canceled or interrupted install never loses installed content. */
static int mpkg_restore_superseded(const char *root, const char *working,
    char *error, size_t capacity)
{
    char area[MAX_PATH], pattern[MAX_PATH], slot[MAX_PATH], content[MAX_PATH];
    char relative[MAX_PATH], destination[MAX_PATH], *slash;
    WIN32_FIND_DATAA found;
    HANDLE search;
    DWORD last;
    int ok = 1;
    if (snprintf(area, sizeof(area), "%s\\superseded", working) >= (int)sizeof(area)) return 0;
    if (GetFileAttributesA(area) == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if (snprintf(pattern, sizeof(pattern), "%s\\*", area) >= (int)sizeof(pattern)) return 0;
    search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            snprintf(slot, sizeof(slot), "%s\\%s", area, found.cFileName) >= (int)sizeof(slot) ||
            snprintf(content, sizeof(content), "%s\\content", slot) >= (int)sizeof(content) ||
            !mpkg_superseded_origin(slot, relative, sizeof(relative)) ||
            snprintf(destination, sizeof(destination), "%s\\overrides\\%s", root, relative) >= (int)sizeof(destination)) {
            ok = 0; break;
        }
        if (GetFileAttributesA(content) == INVALID_FILE_ATTRIBUTES) continue;  /* already restored */
        slash = strrchr(destination, '\\');
        if (slash) {
            *slash = 0;
            ok = mpkg_directory(destination);
            *slash = '\\';
        }
        if (!ok || GetFileAttributesA(destination) != INVALID_FILE_ATTRIBUTES ||
            !MoveFileExA(content, destination, MOVEFILE_WRITE_THROUGH)) { ok = 0; break; }
    } while (FindNextFileA(search, &found));
    last = GetLastError(); FindClose(search);
    if (ok && last != ERROR_NO_MORE_FILES) ok = 0;
    if (!ok) mpkg_err(error, capacity, "a superseded package could not be put back; its data was retained");
    return ok;
}

static int mpkg_directory(const char *path)
{
    DWORD attributes;
    if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int mpkg_plain_ancestors(const char *path)
{
    wchar_t *wide = sh_package_source_wide_path(path);
    size_t i;
    int ok = wide != NULL;
    if (!wide) return 0;
    for (i = 7; ; i++) if (!wide[i] || wide[i] == L'\\') {
        wchar_t saved = wide[i];
        DWORD attributes;
        wide[i] = 0; attributes = GetFileAttributesW(wide); wide[i] = saved;
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ok = 0; break; }
        if (!saved) break;
    }
    free(wide); return ok;
}

static HANDLE mpkg_install_lock(const char *root, char *error, size_t capacity)
{
    char canonical[MAX_PATH], name[80];
    uint64_t hash = UINT64_C(14695981039346656037);
    DWORD length, wait;
    HANDLE mutex;
    size_t i;
    length = GetFullPathNameA(root, sizeof(canonical), canonical, NULL);
    if (!root[0] || !length || length >= sizeof(canonical) || !mpkg_plain_ancestors(canonical)) {
        mpkg_err(error, capacity, "package data root is unavailable or redirected"); return NULL;
    }
    CharLowerBuffA(canonical, (DWORD)strlen(canonical));
    for (i = 0; canonical[i]; i++) { hash ^= (unsigned char)canonical[i]; hash *= UINT64_C(1099511628211); }
    snprintf(name, sizeof(name), "Local\\SnapmapPlusPackageInstall-%016llx", (unsigned long long)hash);
    mutex = CreateMutexA(NULL, FALSE, name);
    wait = mutex ? WaitForSingleObject(mutex, 0) : WAIT_FAILED;
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return mutex;
    if (mutex) CloseHandle(mutex);
    mpkg_err(error, capacity, "another package installation is active; retry after it finishes"); return NULL;
}

struct sh_mpkg_context {
    char root[MAX_PATH];
    size_t count;
    struct sh_mpkg_context *cleanup_next;
};
static SRWLOCK g_context_cleanup_lock = SRWLOCK_INIT;
static sh_mpkg_context *g_context_cleanup;

const char *sh_mpkg_context_root(const sh_mpkg_context *context)
{ return context && context->root[0] ? context->root : NULL; }

size_t sh_mpkg_context_count(const sh_mpkg_context *context)
{ return context ? context->count : 0; }

int sh_mpkg_context_close(sh_mpkg_context **context)
{
    wchar_t *wide;
    int ok;
    if (!context || !*context) return 1;
    if ((*context)->root[0]) {
        /* Only this opaque handle can supply the private directory name. */
        if (!mpkg_plain_ancestors((*context)->root)) return 0;
        wide = sh_package_source_wide_path((*context)->root);
        ok = wide && mpkg_discard_tree(wide);
        free(wide);
        if (!ok) return 0;
    }
    free(*context); *context = NULL; return 1;
}

void sh_mpkg_context_retire(sh_mpkg_context **context)
{
    sh_mpkg_context *retired;
    if (sh_mpkg_context_close(context)) return;
    retired = *context; *context = NULL;
    AcquireSRWLockExclusive(&g_context_cleanup_lock);
    retired->cleanup_next = g_context_cleanup; g_context_cleanup = retired;
    ReleaseSRWLockExclusive(&g_context_cleanup_lock);
    backend_log("MPKG: retired temporary cache is locked; cleanup queued without blocking map loading");
}

void sh_mpkg_context_collect(void)
{
    sh_mpkg_context **link;
    AcquireSRWLockExclusive(&g_context_cleanup_lock);
    link = &g_context_cleanup;
    while (*link) {
        sh_mpkg_context *item = *link, *next = item->cleanup_next;
        if (sh_mpkg_context_close(&item)) *link = next;
        else link = &item->cleanup_next;
    }
    ReleaseSRWLockExclusive(&g_context_cleanup_lock);
}

sh_mpkg_context *sh_mpkg_context_open(const char *data_root, const char *json, size_t len,
    char *error, size_t capacity)
{
    sh_mpkg_context *context = NULL;
    sh_mpkg_decl *decls = NULL;
    char canonical[MAX_PATH], cache[MAX_PATH], maps[MAX_PATH], directory[MAX_PATH];
    DWORD root_length;
    size_t count, i;
    int created = 0;
    sh_json_error problem;
    if (error && capacity) error[0] = 0;
    if (!sh_json_validate_ex(json, len, 128, NULL, &problem)) {
        if (error && capacity) snprintf(error, capacity,
            "Map JSON rejected at byte %zu: %s.", problem.offset, problem.reason);
        return NULL;
    }
    count = mpkg_scan_internal(json, len, &decls);
    if (count == SIZE_MAX || !(context = calloc(1, sizeof(*context)))) {
        mpkg_err(error, capacity, "cannot allocate the complete map package context"); goto bad;
    }
    if (!count) { free(decls); return context; }
    root_length = data_root && data_root[0] ? GetFullPathNameA(data_root, sizeof(canonical), canonical, NULL) : 0;
    if (!root_length || root_length >= sizeof(canonical) || !mpkg_plain_ancestors(canonical) ||
        snprintf(cache, sizeof(cache), "%s\\package-cache", canonical) >= sizeof(cache) ||
        !mpkg_directory(cache) ||
        snprintf(maps, sizeof(maps), "%s\\maps", cache) >= sizeof(maps) || !mpkg_directory(maps)) {
        mpkg_err(error, capacity, "private map source cache is unavailable or redirected"); goto bad;
    }
    for (i = 0; i < 16; i++) {
        unsigned char random[16]; char name[33]; size_t j;
        if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) break;
        for (j = 0; j < sizeof(random); j++) snprintf(name + j * 2, 3, "%02x", random[j]);
        if (snprintf(directory, sizeof(directory), "%s\\map-%s", maps, name) >= sizeof(directory)) break;
        if (CreateDirectoryA(directory, NULL)) { created = 1; break; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (!created) { mpkg_err(error, capacity, "cannot reserve private map source directory"); goto bad; }
    strcpy_s(context->root, sizeof(context->root), directory);
    if (snprintf(directory, sizeof(directory), "%s\\overrides", context->root) >= sizeof(directory) ||
        !CreateDirectoryA(directory, NULL)) {
        mpkg_err(error, capacity, "cannot create private map source inventory"); goto bad;
    }
    for (i = 0; i < count; i++) {
        unsigned char *payload, fingerprint[32];
        size_t payload_length = 0;
        char id[SH_PACKAGE_ID_CAP], destination[MAX_PATH];
        char detail[SH_MPKG_ERR_CAP] = "";
        int ok;
        payload = mpkg_compiled_payload(json, len, decls[i].id, &payload_length, error, capacity);
        if (!payload) goto bad;
        ok = sh_package_archive_identity(payload, payload_length, id, fingerprint, detail, sizeof(detail));
        if (!ok && error && capacity)
            snprintf(error, capacity, "Embedded package '%s': %s", decls[i].id, detail);
        if (ok && strcmp(id, decls[i].id)) {
            mpkg_err(error, capacity, "map package descriptor does not match its delivery identity"); ok = 0;
        }
        if (ok && snprintf(destination, sizeof(destination), "%s\\%s", directory, id) >= sizeof(destination)) {
            mpkg_err(error, capacity, "private map package root is too long"); ok = 0;
        }
        if (ok) ok = sh_package_archive_unpack(payload, payload_length, destination, NULL, error, capacity);
        HeapFree(GetProcessHeap(), 0, payload);
        if (ok && !mpkg_source_satisfies(destination, fingerprint)) {
            mpkg_err(error, capacity, "private map source verification failed"); ok = 0;
        }
        if (!ok) goto bad;
        context->count++;
    }
    free(decls); return context;
bad:
    free(decls);
    sh_mpkg_context_retire(&context);
    return NULL;
}

static const char g_pending_magic[] = "Snapmap+ pending package installation\n";

static int mpkg_pending_marker(const char *path, int create)
{
    char text[sizeof(g_pending_magic)];
    DWORD attributes, bytes = 0;
    HANDLE file;
    int ok;
    attributes = GetFileAttributesA(path);
    if (!create && (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))) return 0;
    file = CreateFileA(path, create ? GENERIC_WRITE : GENERIC_READ, 0, NULL,
        create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (create) ok = WriteFile(file, g_pending_magic, sizeof(g_pending_magic), &bytes, NULL) &&
        bytes == sizeof(g_pending_magic) && FlushFileBuffers(file);
    else ok = GetFileSize(file, NULL) == sizeof(g_pending_magic) &&
        ReadFile(file, text, sizeof(text), &bytes, NULL) && bytes == sizeof(text) &&
        !memcmp(text, g_pending_magic, sizeof(text));
    CloseHandle(file); return ok;
}

/* A valid private marker is written before publishing the complete group.
 * Cancel by moving the whole group outside overrides before deleting anything.
 * The marker contains no author paths; only our checked group name is used. */
static int mpkg_cancel_working(const char *root, const char *working,
    const char *group, char *error, size_t capacity)
{
    char marker[MAX_PATH], destination[MAX_PATH], content[MAX_PATH], parent[MAX_PATH];
    DWORD attributes;
    wchar_t *wide;
    if (!mpkg_working_name(group) || !mpkg_plain_ancestors(working) ||
        snprintf(marker, sizeof(marker), "%s\\pending", working) >= sizeof(marker) ||
        snprintf(content, sizeof(content), "%s\\content", working) >= sizeof(content) ||
        snprintf(parent, sizeof(parent), "%s\\overrides", root) >= sizeof(parent) ||
        snprintf(destination, sizeof(destination), "%s\\%s", parent, group) >= sizeof(destination)) goto refused;
    attributes = GetFileAttributesA(marker);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!mpkg_pending_marker(marker, 0)) goto refused;
        attributes = GetFileAttributesA(destination);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !mpkg_plain_ancestors(parent) || !MoveFileExA(destination, content, MOVEFILE_WRITE_THROUGH)) goto refused;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) goto refused;
        /* Whatever this attempt superseded goes back before anything is deleted. */
        if (!mpkg_restore_superseded(root, working, error, capacity)) return 0;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) goto refused;
    wide = sh_package_source_wide_path(working);
    if (!wide || !mpkg_discard_tree(wide))
        backend_log("MPKG: canceled temporary files remain outside overrides; the next install will discard them");
    free(wide); return 1;
refused:
    mpkg_err(error, capacity, "cannot cancel unfinished package group '%s'; its cancellation record was retained", group);
    return 0;
}

/* Cross-process lock held. Interrupted attempts are canceled, never resumed. */
static int mpkg_discard_interrupted(const char *root, char *error, size_t capacity)
{
    WIN32_FIND_DATAA found;
    char parent[MAX_PATH], path[MAX_PATH];
    HANDLE search;
    DWORD last;
    int ok = 1;
    if (snprintf(parent, sizeof(parent), "%s\\package-staging", root) >= sizeof(parent)) return 0;
    last = GetFileAttributesA(parent);
    if (last == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (!mpkg_plain_ancestors(parent)) return 0;
    if (snprintf(path, sizeof(path), "%s\\*", parent) >= sizeof(path)) return 0;
    search = FindFirstFileA(path, &found);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !mpkg_working_name(found.cFileName)) continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            snprintf(path, sizeof(path), "%s\\%s", parent, found.cFileName) >= sizeof(path) ||
            !mpkg_cancel_working(root, path, found.cFileName, error, capacity)) { ok = 0; break; }
    } while (FindNextFileA(search, &found));
    last = GetLastError(); FindClose(search);
    if (ok && last != ERROR_NO_MORE_FILES) ok = 0;
    if (!ok && !error[0]) mpkg_err(error, capacity, "unfinished package installation could not be canceled");
    return ok;
}

static int mpkg_recover_interrupted(const char *root, char *error, size_t capacity)
{
    HANDLE mutex = mpkg_install_lock(root, error, capacity);
    int ok;
    if (!mutex) return 0;
    ok = mpkg_discard_interrupted(root, error, capacity);
    ReleaseMutex(mutex); CloseHandle(mutex); return ok;
}

/* Only called at the engine browser boundary, after native consumers and their
 * allocator scope have completed, while the previous compiler is still held. */
int sh_mpkg_activation_commit(char *error, size_t capacity)
{
    size_t i;
    HANDLE mutex;
    mpkg_lock();
    if (!g_install.active) { mpkg_unlock(); return 1; }
    if (g_install.failed) {
        mpkg_err(error, capacity, "the previous package installation still needs cancellation"); goto refused;
    }
    for (i = 0; i < g_session_count; i++) if (g_session[i].outcome == 4 &&
        !mpkg_source_satisfies(g_session[i].root, g_session[i].fingerprint)) {
        mpkg_err(error, capacity, "package '%s' changed during activation", g_session[i].id); goto refused;
    }
    if (g_install.marker[0] && !DeleteFileA(g_install.marker)) {
        mpkg_err(error, capacity, "cannot commit package activation; load the map to retry"); goto refused;
    }
    for (i = 0; i < g_session_count; i++) if (g_session[i].outcome == 4) g_session[i].outcome = 1;
    if (g_install.working[0]) {
        /* The marker is gone, so this tree can never restore anything again.
         * Discard it whole: it may still hold the packages this set superseded. */
        wchar_t *wide = sh_package_source_wide_path(g_install.working);
        if (!wide || !mpkg_discard_tree(wide))
            backend_log("MPKG: committed installation left temporary data outside overrides; the next install will discard it");
        free(wide);
    }
    mutex = g_install.mutex; memset(&g_install, 0, sizeof(g_install));
    mpkg_unlock(); ReleaseMutex(mutex); CloseHandle(mutex);
    /* The committed files are now permanent library sources even though the
     * active provider only carries them as a temporary map overlay. Tell the
     * runtime so it recompiles the library before that overlay changes. */
    sh_package_runtime_note_sources_changed();
    backend_log("MPKG: whole package installation COMMITTED after successful runtime activation; no restart needed");
    return 1;
refused:
    mpkg_unlock(); return 0;
}

int sh_mpkg_activation_cancel(void)
{
    char error[256] = "";
    HANDLE mutex;
    size_t i;
    mpkg_lock();
    if (!g_install.active) { mpkg_unlock(); return 1; }
    g_install.failed = 1;
    if (g_install.working[0] && !mpkg_cancel_working(g_data_root, g_install.working,
        strrchr(g_install.working, '\\') + 1, error, sizeof(error))) {
        mpkg_unlock(); sh_mpkg_report_error(error); return 0;
    }
    for (i = 0; i < g_session_count; i++) if (g_session[i].outcome == 4) g_session[i].outcome = 3;
    mutex = g_install.mutex; memset(&g_install, 0, sizeof(g_install));
    mpkg_unlock(); ReleaseMutex(mutex); CloseHandle(mutex);
    backend_log("MPKG: package installation canceled and rolled back; the map can be requested again");
    return 1;
}

static int mpkg_install_batch(mpkg_staged *head, char *err, size_t err_cap)
{
    char root[MAX_PATH], parent[MAX_PATH], batch[MAX_PATH] = "", destination[MAX_PATH], line[512];
    char content[MAX_PATH], marker[MAX_PATH] = "", group[24];
    uint64_t nonce;
    HANDLE mutex = NULL;
    sh_package_sources *sources = NULL;
    mpkg_staged *s;
    char **superseded = NULL;
    size_t added = 0, i, superseded_count = 0;
    int success = 0, prepared = 0;
    mpkg_lock();
    if (g_install.active) {
        mpkg_unlock(); mpkg_err(err, err_cap, "a package installation is awaiting activation or cancellation"); return 0;
    }
    strncpy_s(root, sizeof(root), g_data_root, _TRUNCATE); mpkg_unlock();
    mutex = mpkg_install_lock(root, err, err_cap);
    if (!mutex || !mpkg_discard_interrupted(root, err, err_cap)) goto done;
    sources = sh_package_sources_scan_local(root, mpkg_local_rejected, NULL, err, err_cap);
    if (!sources) goto done;
    for (s = head; s; s = s->next) {
        char descriptor_id[SH_PACKAGE_ID_CAP];
        if (!sh_package_archive_identity(s->payload, s->payload_len, descriptor_id,
                                        s->fingerprint, err, err_cap)) goto done;
        if (strcmp(descriptor_id, s->id)) {
            mpkg_err(err, err_cap, "package descriptor does not match its map identity"); goto done;
        }
        for (i = 0; i < sources->component_count; i++) {
            const sh_package_component *component = &sources->components[i];
            const char *package_root = sources->packages[component->owner].root;
            char relative[MAX_PATH], **grown;
            if (component->relative[0] || strcmp(component->descriptor.id, s->id)) continue;
            if (!memcmp(sources->fingerprints[component->owner], s->fingerprint, 32)) {
                strcpy_s(s->destination, sizeof(s->destination), package_root);
                s->existing = 1; break;
            }
            /* A previously delivered variant of this identity is superseded by
             * this delivery: one delivered copy per identity is enough, and the
             * old bundle rides the same cancellation record.
             *
             * An authored variant is the user's own work. It is neither moved
             * nor refused: the compiler treats same-identity packages as
             * variants, so the author keeps every resource they supply while
             * this delivery fills what they do not, and the map's own values
             * govern transiently while it is loaded. */
            if (!mpkg_delivered_relative(root, package_root, relative, sizeof(relative))) continue;
            grown = (char **)realloc(superseded, (superseded_count + 1) * sizeof(*superseded));
            if (!grown || !(grown[superseded_count] = _strdup(relative))) {
                superseded = grown ? grown : superseded;
                mpkg_err(err, err_cap, "cannot record the superseded package '%s'", s->id); goto done;
            }
            superseded = grown; superseded_count++;
        }
        if (!s->existing) added++;
    }
    mpkg_lock();
    for (s = head; s; s = s->next) if (!mpkg_session_find(s->id, s->digest)) break;
    mpkg_unlock();
    if (s) { mpkg_err(err, err_cap, "package consent state was lost before installation"); goto done; }
    if (snprintf(parent, sizeof(parent), "%s\\package-staging", root) >= sizeof(parent) || !mpkg_directory(parent)) {
        mpkg_err(err, err_cap, "cannot prepare package installation staging"); goto done;
    }
    /* Retiring a superseded package needs the same cancellation record as a
     * publication, so the batch exists whenever either one has work to do. */
    if (added || superseded_count) {
        if (BCryptGenRandom(NULL, (PUCHAR)&nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            mpkg_err(err, err_cap, "cannot create a unique installation folder"); goto done;
        }
        snprintf(group, sizeof(group), "map-%016llx", (unsigned long long)nonce);
        if (snprintf(batch, sizeof(batch), "%s\\%s", parent, group) >= sizeof(batch) ||
            snprintf(destination, sizeof(destination), "%s\\overrides\\%s", root, group) >= sizeof(destination) ||
            snprintf(content, sizeof(content), "%s\\content", batch) >= sizeof(content) ||
            snprintf(marker, sizeof(marker), "%s\\pending", batch) >= sizeof(marker) ||
            !CreateDirectoryA(batch, NULL)) {
            mpkg_err(err, err_cap, "cannot create package installation staging"); goto done;
        }
        prepared = 1;
        if (added && !CreateDirectoryA(content, NULL)) {
            mpkg_err(err, err_cap, "cannot create package content staging"); goto done;
        }
        for (s = head; s; s = s->next) if (!s->existing) {
            if (snprintf(s->staging, sizeof(s->staging), "%s\\%s", content, s->id) >= sizeof(s->staging) ||
                snprintf(s->destination, sizeof(s->destination), "%s\\%s", destination, s->id) >= sizeof(s->destination) ||
                !sh_mpkg_unpack(s->payload, s->payload_len, s->staging, &s->files, err, err_cap)) goto done;
#ifdef SH_MAP_PACKAGE_TESTING
            if (g_prepare_test) g_prepare_test(s->staging);
#endif
        }
    }
    for (s = head; s; s = s->next) if (!mpkg_source_satisfies(
            s->existing ? s->destination : s->staging, s->fingerprint)) {
        mpkg_err(err, err_cap, "prepared package '%s' changed before publication", s->id); goto done;
    }
    snprintf(parent, sizeof(parent), "%s\\overrides", root);
    if (!mpkg_directory(parent)) {
        mpkg_err(err, err_cap, "cannot publish packages into overrides"); goto done;
    }
    if (prepared && !mpkg_pending_marker(marker, 1)) {
        mpkg_err(err, err_cap, "cannot prepare the installation cancellation record"); goto done;
    }
    /* After the marker, before publication: a crash from here on restores every
     * superseded package during the next startup cancellation. */
    for (i = 0; i < superseded_count; i++) {
        if (!mpkg_supersede_one(root, batch, i, superseded[i], err, err_cap)) goto done;
        snprintf(line, sizeof(line), "MPKG: superseding the previously delivered package at overrides\\%s", superseded[i]);
        backend_log(line);
    }
    if (added && !mpkg_publish(content, destination)) {
        mpkg_err(err, err_cap, "package installation canceled before commit (Windows error %lu); load the map to retry", GetLastError()); goto done;
    }
    /* Published as one complete group, but not committed until native activation
     * succeeds. A crash retains the private marker so startup cancels this set. */
    success = 1;
    mpkg_lock();
    g_install.active = 1; g_install.mutex = mutex; mutex = NULL;
    if (prepared) {
        strcpy_s(g_install.working, sizeof(g_install.working), batch);
        strcpy_s(g_install.marker, sizeof(g_install.marker), marker);
    }
    for (s = head; s; s = s->next) {
        mpkg_session_entry *e = mpkg_session_find(s->id, s->digest);
        e->outcome = 4; strcpy_s(e->root, sizeof(e->root), s->destination);
        memcpy(e->fingerprint, s->fingerprint, 32);
    }
    mpkg_unlock();
    for (s = head; s; s = s->next) {
        snprintf(line, sizeof(line), "MPKG: package '%s' (digest %s) PREPARED at %s -- %u file(s)",
            s->id, s->digest, s->destination, s->files);
        backend_log(line);
    }
    backend_log("MPKG: complete package set prepared; awaiting runtime activation before installation commits");
    if (!head->completion) sh_decl_server_request_rearm();
done:
    if (!success && prepared) {
        char restore_error[SH_MPKG_ERR_CAP] = "";
        /* Put back what this attempt retired before deleting anything. A failed
         * restore keeps the data: the next startup cancellation retries it. */
        if (!mpkg_restore_superseded(root, batch, restore_error, sizeof(restore_error)))
            backend_log(restore_error[0] ? restore_error :
                "MPKG: a superseded package could not be put back; its data was retained");
        else {
            wchar_t *wide = sh_package_source_wide_path(batch);
            if (!wide || !mpkg_discard_tree(wide))
                backend_log("MPKG: canceled temporary data remains outside overrides; the next install will discard it");
            free(wide);
        }
    }
    for (i = 0; i < superseded_count; i++) free(superseded[i]);
    free(superseded);
    sh_package_sources_free(sources);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return success;
}

static void mpkg_record_decline(const char *id, const char *digest)
{
    char line[256];
    mpkg_lock();
    {
        mpkg_session_entry *e = mpkg_session_find(id, digest);
        if (e) e->outcome = 3;
        else mpkg_session_add(id, digest, 3);
    }
    mpkg_unlock();
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: package '%s' (digest %s) was not installed for this load; "
        "a later map load can request installation again", id, digest);
    backend_log(line);
}

static void mpkg_retry_staged(mpkg_staged *head)
{
    mpkg_staged *s;
    mpkg_lock();
    for (s = head; s; s = s->next) {
        mpkg_session_entry *e = mpkg_session_find(s->id, s->digest);
        if (e && e->outcome == 2) e->outcome = 3;
    }
    mpkg_unlock();
}

static void mpkg_decide_staged(mpkg_staged *head, int accepted)
{
    sh_mpkg_install_completion completion = head->completion;
    void *context = head->completion_context;
    int outcome = 0;
    if (accepted) {
        char error[SH_MPKG_ERR_CAP], line[SH_MPKG_ERR_CAP + 64];
        outcome = mpkg_install_batch(head, error, sizeof(error)) ? 1 : -1;
        if (outcome < 0) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "MPKG: package set installation FAILED: %s", error);
            sh_mpkg_report_error(line);
            mpkg_retry_staged(head);
        }
    } else {
        mpkg_staged *s;
        for (s = head; s; s = s->next) mpkg_record_decline(s->id, s->digest);
    }
    mpkg_staged_free(head);
    if (completion) completion(context, outcome);
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

    if (g_consent_ticket) sh_engine_dialog_release(g_consent_ticket);
    g_consent_staged = NULL;
    g_consent_ticket = 0;
    InterlockedExchange(&g_consent_state, MPKG_CONSENT_IDLE);
    if (!s) return;

    mpkg_decide_staged(s, accepted);
}

void sh_mpkg_cancel_map_consent(void *context)
{
    if (g_consent_staged && g_consent_staged->completion &&
        g_consent_staged->completion_context == context) mpkg_consent_finish(0);
}

void sh_mpkg_consent_poll(void)
{
    LONG state = InterlockedCompareExchange(&g_consent_state, 0, 0);
    char text[256];
    mpkg_staged *s;

    if (state == MPKG_CONSENT_IDLE) { mpkg_notice_poll(); return; }
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
        /* Put the decision before the names and reserve the remainder count.
         * Even long IDs and large sets must fit the native 256-byte body. */
        {
            unsigned count = mpkg_staged_count(s);
            unsigned long long kb = (unsigned long long)((mpkg_staged_bytes(s) + 1023) / 1024);
            unsigned files = mpkg_staged_files(s);
            unsigned listed = 0;
            mpkg_staged *item;
            _snprintf_s(text, sizeof(text), _TRUNCATE,
                "This map needs %u mod package%s (%u files, %llu KB). Install %s now? ",
                count, count == 1 ? "" : "s", files, kb, count == 1 ? "it" : "them");
            for (item = s; item; item = item->next) {
                char more[32] = "";
                size_t used = strlen(text), separator = listed ? 2 : 0;
                if (item->next) snprintf(more, sizeof(more), " and %u more", count - listed - 1);
                if (used + separator + strlen(item->id) + strlen(more) >= sizeof(text)) break;
                if (listed) strcat_s(text, sizeof(text), ", ");
                strcat_s(text, sizeof(text), item->id); listed++;
            }
            if (listed < count) {
                char more[48];
                snprintf(more, sizeof(more), listed ? " and %u more" : "%u packages (see log)", count - listed);
                strcat_s(text, sizeof(text), more);
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

    if (mode == 0 || mode == 1) { mpkg_decide_staged(s, mode); return; }

    if (InterlockedCompareExchange(&g_consent_state, MPKG_CONSENT_RAISE,
                                   MPKG_CONSENT_IDLE) != MPKG_CONSENT_IDLE) {
        backend_log("MPKG: a consent dialog is already up; this package set can be offered on a later load");
        mpkg_retry_staged(s);
        if (s->completion) s->completion(s->completion_context, 0);
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

int sh_mpkg_activation_ready(void)
{
    int captured, installing;
    mpkg_lock();
    g_last_refusal[0] = 0; captured = g_boot_captured == 1; installing = g_install.active;
    mpkg_unlock();
    if (!captured || installing ||
        InterlockedCompareExchange(&g_consent_state, 0, 0) != MPKG_CONSENT_IDLE) {
        mpkg_set_refusal("package startup, consent or installation is still pending"); return 0;
    }
    if (!sh_package_runtime_admission_ready()) {
        mpkg_set_refusal("installed package compilation or registration is incomplete or failed"); return 0;
    }
    return 1;
}

int sh_mpkg_request_map_install(const char *json, size_t length,
    const sh_package_compilation *candidate, const sh_package_owners *owners,
    sh_mpkg_install_completion completion, void *context, char *error, size_t capacity)
{
    sh_mpkg_decl *decls = NULL;
    mpkg_staged *head = NULL, *tail = NULL;
    const sh_package_sources *sources = candidate ? candidate->sources : NULL;
    size_t count;
    int ok = 0;
    if (error && capacity) error[0] = 0;
    if (!json || !length || !sources || !completion ||
        !sh_package_owners_within(owners, sources->package_count) ||
        !sh_package_owners_count(owners) || !sh_mpkg_activation_ready()) {
        mpkg_err(error, capacity, "map installation requires a ready runtime and complete supplying owners"); return 0;
    }
    count = mpkg_scan_internal(json, length, &decls);
    if (count == SIZE_MAX) goto done;
    for (size_t owner = 0; owner < sources->package_count; owner++) {
        const char *id = NULL;
        const sh_mpkg_decl *decl = NULL;
        mpkg_staged *entry;
        unsigned char fingerprint[32];
        char archive_id[SH_PACKAGE_ID_CAP];
        if (!sh_package_owners_contains(owners, owner)) continue;
        for (size_t i = 0; i < sources->component_count; i++) {
            const sh_package_component *component = &sources->components[i];
            if (component->owner == owner && !component->relative[0]) { id = component->descriptor.id; break; }
        }
        if (!id) goto done;
        for (size_t i = 0; i < count; i++) if (!strcmp(decls[i].id, id)) { decl = &decls[i]; break; }
        if (!decl) { mpkg_err(error, capacity, "map omits supplying package '%s'", id); goto done; }
        /* Distinct owners must never be collapsed by an ambiguous identity. */
        for (entry = head; entry; entry = entry->next) if (!strcmp(entry->id, id)) {
            mpkg_err(error, capacity, "map has ambiguous delivery identity '%s'", id); goto done;
        }
        entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
        if (!entry) goto done;
        if (tail) tail->next = entry; else head = entry;
        tail = entry;
        strcpy_s(entry->id, sizeof(entry->id), id);
        strcpy_s(entry->digest, sizeof(entry->digest), decl->digest);
        entry->payload = mpkg_compiled_payload(json, length, id, &entry->payload_len, error, capacity);
        if (!entry->payload || !sh_package_archive_identity(entry->payload, entry->payload_len,
            archive_id, fingerprint, error, capacity) || strcmp(archive_id, id) ||
            memcmp(fingerprint, sources->fingerprints[owner], 32) ||
            !mpkg_zip_survey(entry->payload, entry->payload_len, &entry->files, error, capacity)) {
            if (error && capacity && !error[0]) mpkg_err(error, capacity, "map package '%s' changed after compilation", id);
            goto done;
        }
    }
    /* Reserve the entire request before any consent or publication. */
    mpkg_lock();
    for (mpkg_staged *entry = head; entry; entry = entry->next) {
        mpkg_session_entry *session = mpkg_session_find(entry->id, entry->digest);
        if (!session) session = mpkg_session_add(entry->id, entry->digest, 3);
        if (!session) { mpkg_unlock(); goto done; }
        session->outcome = 2;
    }
    mpkg_unlock();
    head->completion = completion; head->completion_context = context;
    mpkg_request_consent(head); head = NULL; ok = 1;
done:
    if (!ok) {
        if (error && capacity && !error[0]) mpkg_err(error, capacity, "cannot prepare the complete map installation request");
        mpkg_retry_staged(head);
    }
    mpkg_staged_free(head); free(decls); return ok;
}

static int mpkg_gate_declared(const char *json, size_t len,
    const sh_mpkg_decl *decls, size_t count, int *installed)
{
    size_t i, missing_count = 0, session_installed_count = 0;
    const sh_mpkg_decl *first_missing = NULL;
    char reason[SH_MPKG_ERR_CAP];

    mpkg_lock();
    if (g_install.active) {
        mpkg_unlock();
        mpkg_set_refusal("package compilation or registration is incomplete or failed; installation has not committed");
        return 0;
    }
    mpkg_unlock();

    if (g_boot_captured != 1) {
        /* A declared package requires a captured boot state; refuse otherwise. */
        mpkg_set_refusal("map declares packages but the boot package snapshot is missing");
        return 0;
    }

    for (i = 0; i < count; i++) {
        const sh_mpkg_decl *d = &decls[i];
        size_t payload_len = 0;
        unsigned char fingerprint[32], *payload;
        char id[SH_PACKAGE_ID_CAP], error[SH_MPKG_ERR_CAP];
        int valid;
        payload = mpkg_compiled_payload(json, len, d->id, &payload_len, error, sizeof(error));
        valid = payload && sh_package_archive_identity(payload, payload_len, id, fingerprint, error, sizeof(error));
        if (payload) HeapFree(GetProcessHeap(), 0, payload);
        if (!valid || strcmp(id, d->id)) {
            _snprintf_s(reason, sizeof(reason), _TRUNCATE, "package '%s' cannot be loaded: %s", d->id,
                        valid ? "descriptor does not match its map identity" : error);
            mpkg_set_refusal(reason); return 0;
        }
        mpkg_lock(); installed[i] = mpkg_installed_kind(d, fingerprint); mpkg_unlock();
        if (installed[i] == 1) continue;
        missing_count++;
        if (installed[i] == 2) session_installed_count++;
        if (!first_missing) {
            first_missing = d;
        }
    }

    if (missing_count == 0) {
        /* Startup identities prove disk content, not a successful activation. */
        if (sh_package_runtime_ready() && sh_decl_server_registration_succeeded()) return 1;
        mpkg_set_refusal("installed package compilation or registration is incomplete or failed"); return 0;
    }

    if (session_installed_count == missing_count) {
        /* Session installs pass only after declaration registration reports success. */
        if (sh_package_runtime_ready() && sh_decl_server_registration_succeeded()) {
            backend_log("MPKG: matching authored package compiled and registered at runtime; "
                        "allowing the load without a restart");
            return 1;
        }
        _snprintf_s(reason, sizeof reason, _TRUNCATE,
            "package '%s' is present but its compilation or registration is incomplete or failed; "
            "check the package registration log before retrying", first_missing->id);
        mpkg_set_refusal(reason);
        return 0;
    }

    _snprintf_s(reason, sizeof reason, _TRUNCATE,
        "map requires %zu uninstalled package(s); first: '%s' (digest %s, %u/%u shards)",
        missing_count, first_missing->id, first_missing->digest,
        first_missing->present, first_missing->total);
    mpkg_set_refusal(reason);

    /* Prepare one complete consent request. An allocation or extraction failure
     * must not turn the available prefix into a smaller installation offer. */
    {
        mpkg_staged *head = NULL, *tail = NULL;
        char err[SH_MPKG_ERR_CAP] = "";
        int complete = 1;

        for (i = 0; i < count; i++) {
            const sh_mpkg_decl *d = &decls[i];
            mpkg_session_entry *e;
            mpkg_staged *s;
            int should_offer = 0;

            if (installed[i]) continue;

            mpkg_lock();
            e = mpkg_session_find(d->id, d->digest);
            if (!e) e = mpkg_session_add(d->id, d->digest, 3);
            if (e && e->outcome == 3) { e->outcome = 2; should_offer = 1; }
            mpkg_unlock();
            if (!e) { complete = 0; mpkg_err(err, sizeof(err), "cannot reserve package consent state"); break; }
            if (!should_offer) continue;
            s = (mpkg_staged *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
            if (!s) {
                mpkg_lock();
                e = mpkg_session_find(d->id, d->digest);
                if (e && e->outcome == 2) e->outcome = 3;
                mpkg_unlock();
                complete = 0; mpkg_err(err, sizeof(err), "cannot allocate complete package consent request"); break;
            }
            strcpy_s(s->id, sizeof(s->id), d->id);
            strcpy_s(s->digest, sizeof(s->digest), d->digest);
            if (tail) tail->next = s; else head = s;
            tail = s;
            s->payload = mpkg_compiled_payload(json, len, d->id, &s->payload_len, err, sizeof(err));
            if (!s->payload || !mpkg_zip_survey(s->payload, s->payload_len, &s->files, err, sizeof(err))) {
                complete = 0; break;
            }
        }
        if (!complete) {
            sh_mpkg_report_error(err); mpkg_retry_staged(head); mpkg_staged_free(head);
        } else if (head) mpkg_request_consent(head);   /* takes ownership of the chain */
    }
    return 0;
}

int sh_mpkg_gate(const char *json, size_t len)
{
    sh_mpkg_decl *decls = NULL;
    int *installed = NULL, result = 0;
    size_t count;
    if (!json || !len || !sh_shard_find(json, len, MPKG_HEADER_MAGIC, MPKG_MAGIC_LEN)) return 1;
    count = mpkg_scan_internal(json, len, &decls);
    if (count == SIZE_MAX || count > SIZE_MAX / sizeof(*installed) ||
        (count && !(installed = (int *)calloc(count, sizeof(*installed))))) {
        mpkg_set_refusal("cannot allocate the complete map package inventory");
    } else result = !count || mpkg_gate_declared(json, len, decls, count, installed);
    free(installed); free(decls); return result;
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

void sh_mpkg_test_set_publish(sh_mpkg_test_publish_fn publish) { g_publish_test = publish; }
void sh_mpkg_test_set_prepare(void (*prepare)(const char *package)) { g_prepare_test = prepare; }

void sh_mpkg_test_reset(void)
{
    sh_mpkg_activation_cancel();
    if (g_consent_ticket) sh_engine_dialog_release(g_consent_ticket);
    if (g_notice_ticket) sh_engine_dialog_release(g_notice_ticket);
    mpkg_staged_free(g_consent_staged); g_consent_staged = NULL;
    g_consent_ticket = g_notice_ticket = 0;
    g_consent_state = g_consent_waited = 0;
    mpkg_lock();
    g_boot_captured = 0;
    g_data_root[0] = '\0';
    free(g_boot); g_boot = NULL; g_boot_count = 0;
    free(g_session); g_session = NULL; g_session_count = g_session_capacity = 0;
    g_last_refusal[0] = '\0';
    g_consent_mode = -1;
    g_publish_test = NULL;
    g_prepare_test = NULL;
    g_notice[0] = 0; g_notice_revision = 0;
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
        if (g_session[i].outcome == 1 || g_session[i].outcome == 4) n++;
    mpkg_unlock();
    return n;
}
#endif
