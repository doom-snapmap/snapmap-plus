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
#include "map_package_share.h"
#include "package_cache.h"
#include "map_provenance.h"
#include "config_json.h"
#include "package_keys.h"

typedef char mpkg_header_fits_shared_envelope[SH_MPKG_HEADER_CAP <= SH_SHARD_HEADER_MAX ? 1 : -1];
typedef char mpkg_delta_keys_fit[SH_MPKG_ID_CAP <= SH_MPKG_DELTA_KEY_CAP ? 1 : -1];

static sh_package_legacy_reader g_legacy_reader;
static void *g_legacy_context;
void sh_mpkg_set_legacy_reader(sh_package_legacy_reader reader, void *context)
{ g_legacy_reader = reader; g_legacy_context = context; }

static int mpkg_delivery_id(const char *id)
{
    return sh_package_bundle_parse_key(id, NULL, NULL);
}

/* Verify the original carrier before conversion. The public extractor returns
 * the delivery's exact complete archive, rebuilt from a shared carrier when the
 * map stores it as a delta of another delivery (map_package_share.h). */
static unsigned char *mpkg_compiled_payload_retained(const char *json, size_t length, const char *id,
    size_t *out_length, const char *cache_root, sh_package_bundle **retained, char *error, size_t capacity)
{
    unsigned char *original, *converted = NULL, *out = NULL, expected[32];
    char seed[SH_PACKAGE_ID_CAP] = "";
    sh_package_bundle *bundle = NULL;
    const char *migration_id = id;
    size_t converted_length = 0;
    if (retained) *retained = NULL;
    original = sh_mpkg_extract(json, length, id, out_length, error, capacity);
    if (!original) return NULL;
    if (sh_package_bundle_parse_key(id, expected, seed)) {
        char descriptor_id[SH_PACKAGE_ID_CAP];
        unsigned char fingerprint[32];
        if (!sh_package_archive_tree_identity(original, *out_length, fingerprint, error, capacity)) goto done;
        if (memcmp(expected, fingerprint, 32) && (seed[0] ||
            !sh_package_archive_identity(original, *out_length, descriptor_id, NULL, error, capacity) ||
            strcmp(id, descriptor_id))) {
            if (error && capacity) snprintf(error, capacity, "map package does not match its complete delivery identity");
            goto done;
        }
        if (seed[0]) migration_id = seed;
    }
    if (!sh_package_legacy_convert(original, *out_length, migration_id, g_legacy_reader, g_legacy_context,
        &converted, &converted_length, error, capacity)) goto done;
    if (retained) {
        bundle = converted ? sh_package_bundle_from_legacy_archive(original, *out_length, migration_id,
            cache_root, error, capacity) : sh_package_bundle_from_archive(original, *out_length, cache_root, error, capacity);
        if (!bundle) goto done;
    }
    if (!converted) { out = original; original = NULL; }
    else {
        out = HeapAlloc(GetProcessHeap(), 0, converted_length);
        if (out) { memcpy(out, converted, converted_length); *out_length = converted_length; }
        else if (error && capacity) snprintf(error, capacity, "cannot retain migrated package '%s'", id);
    }
    if (out && retained) { *retained = bundle; bundle = NULL; }
done:
    sh_package_bundle_release(bundle);
    free(converted); if (original) HeapFree(GetProcessHeap(), 0, original);
    if (!out) *out_length = 0;
    return out;
}

static unsigned char *mpkg_compiled_payload(const char *json, size_t length, const char *id,
    size_t *out_length, char *error, size_t capacity)
{ return mpkg_compiled_payload_retained(json, length, id, out_length, NULL, NULL, error, capacity); }

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
        if (!mpkg_delivery_id(hdr->id) && !sh_package_id_normalize(hdr->id, hdr->id)) {
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
    char canonical_id[SH_MPKG_ID_CAP];

    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = '\0';
    if (!json || len == 0 || !pkg_id || !payload ||
        (mpkg_delivery_id(pkg_id) ? strcpy_s(canonical_id, sizeof(canonical_id), pkg_id) != 0 :
            !sh_package_id_normalize(pkg_id, canonical_id))) {
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

char *sh_mpkg_embed_set(const char *json, size_t len, const sh_mpkg_delivery *items, size_t count,
                        size_t *out_len, size_t *failed_item, char *err, size_t err_cap)
{
    sh_mpkg_share_item *plan = NULL;
    char (*keys)[SH_MPKG_ID_CAP] = NULL;
    char *current = NULL, *result = NULL, reason[SH_MPKG_ERR_CAP] = "";
    size_t current_length = len, i;
    if (out_len) *out_len = 0;
    if (failed_item) *failed_item = SIZE_MAX;
    if (err && err_cap) err[0] = '\0';
    if (!json || !len || !items || !count) { mpkg_err(err, err_cap, "embed called with nothing to embed"); return NULL; }
    if (count > SIZE_MAX / sizeof(*plan) || count > SIZE_MAX / sizeof(*keys) ||
        !(plan = (sh_mpkg_share_item *)calloc(count, sizeof(*plan))) ||
        !(keys = calloc(count, sizeof(*keys)))) {
        mpkg_err(err, err_cap, "out of memory preparing the map's packages"); goto done;
    }
    for (i = 0; i < count; i++) {
        if (!items[i].id || !items[i].archive || !items[i].length ||
            (mpkg_delivery_id(items[i].id) ? strcpy_s(keys[i], SH_MPKG_ID_CAP, items[i].id) != 0 :
                !sh_package_id_normalize(items[i].id, keys[i]))) {
            if (failed_item) *failed_item = i;
            mpkg_err(err, err_cap, "embed called with nothing to embed"); goto done;
        }
        plan[i].key = keys[i]; plan[i].archive = items[i].archive; plan[i].length = items[i].length;
    }
    /* Sharing is storage only: without a plan every delivery is stored whole. */
    if (!sh_mpkg_share_plan(plan, count, SH_MPKG_SHARD_CHARS, reason, sizeof(reason))) {
        char line[SH_MPKG_ERR_CAP + 96];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "MPKG: shared package storage unavailable for this save (%s); each delivery is stored whole", reason);
        backend_log(line);
    }
    for (i = 0; i < count; i++) {
        const unsigned char *payload = plan[i].delta ? plan[i].delta : items[i].archive;
        size_t payload_length = plan[i].delta ? plan[i].delta_length : items[i].length, next_length = 0;
        char *next = sh_mpkg_embed(current ? current : json, current_length, keys[i], payload, payload_length,
                                   &next_length, err, err_cap);
        if (!next) { if (failed_item) *failed_item = i; goto done; }
        if (plan[i].delta) {
            char line[SH_MPKG_ID_CAP + 192];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: package '%s' stored as an exact delta of the map's other deliveries -- "
                        "%zu bytes rebuild its %zu-byte archive; Snapmap+ readers before shared carriers cannot decode it",
                        keys[i], plan[i].delta_length, items[i].length);
            backend_log(line);
        }
        if (current) HeapFree(GetProcessHeap(), 0, current);
        current = next; current_length = next_length;
    }
    if (out_len) *out_len = current_length;
    result = current; current = NULL;
done:
    if (current) HeapFree(GetProcessHeap(), 0, current);
    sh_mpkg_share_clear(plan, plan ? count : 0);
    free(plan); free(keys);
    return result;
}

/* ==================================================================== */
/* the reference carrier (L2)                                            */
/* ==================================================================== */

#define MPREF_MAGIC_LEN 7
#define MPREF_BODY_CAP  2048   /* JSON bytes: base64 of this stays one shard */

typedef struct mpref_hdr {
    char key[SH_MPKG_ID_CAP];
    char digest[SH_MPKG_DIGEST_CHARS + 1];
} mpref_hdr;

/* "smpref.<key>.<digest16>": the final field is the body digest, everything
 * between the magic and it is the transport key. */
static int mpref_parse_header(const char *p, size_t plen, mpref_hdr *hdr)
{
    const char *end = p + plen, *dot;
    size_t n;
    if (plen <= MPREF_MAGIC_LEN + SH_MPKG_DIGEST_CHARS + 1) return 0;
    dot = end - SH_MPKG_DIGEST_CHARS - 1;
    if (*dot != '.') return 0;
    for (n = 0; n < SH_MPKG_DIGEST_CHARS; n++) {
        if (!sh_shard_is_hex(dot[1 + n])) return 0;
        hdr->digest[n] = dot[1 + n];
    }
    hdr->digest[SH_MPKG_DIGEST_CHARS] = 0;
    n = (size_t)(dot - (p + MPREF_MAGIC_LEN));
    if (!n || n >= sizeof(hdr->key)) return 0;
    memcpy(hdr->key, p + MPREF_MAGIC_LEN, n); hdr->key[n] = 0;
    return mpkg_delivery_id(hdr->key) || sh_package_id_normalize(hdr->key, hdr->key);
}

static int mpref_strip_filter(const char *hdr, size_t hdr_len, void *ctx)
{
    const char *key = (const char *)ctx;
    mpref_hdr parsed;
    if (!mpref_parse_header(hdr, hdr_len, &parsed)) return 0;
    return key == NULL || strcmp(parsed.key, key) == 0;
}

static char *mpref_strip_scoped(const char *json, size_t len, const char *key, size_t *out_len)
{
    unsigned elements = 0, runs = 0;
    int doc_failed = 0;
    size_t w = 0;
    char *out;
    if (out_len) *out_len = 0;
    if (!json || !len || !sh_shard_find(json, len, SH_MPREF_MAGIC, MPREF_MAGIC_LEN)) return NULL;
    out = sh_shard_strip(json, len, SH_MPREF_MAGIC, MPREF_MAGIC_LEN, mpref_strip_filter, (void *)key,
                         SH_MPKG_MAX_SHARDS, &w, &elements, &runs, &doc_failed);
    if (!out) {
        if (doc_failed)
            backend_log("MPKG: reference strip SKIPPED -- the map's JSON structure did not read cleanly; "
                        "handing the engine the original buffer");
        return NULL;
    }
    if (elements) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: %u package reference(s) STRIPPED, %zu -> %zu bytes",
                    elements, len, w);
        backend_log(line);
    }
    if (out_len) *out_len = w;
    return out;
}

char *sh_mpkg_reference_strip(const char *json, size_t len, size_t *out_len)
{
    return mpref_strip_scoped(json, len, NULL, out_len);
}

size_t sh_mpkg_shards_for(size_t payload_len)
{
    size_t b64 = ((payload_len + 2) / 3) * 4, shards = (b64 + SH_MPKG_SHARD_CHARS - 1) / SH_MPKG_SHARD_CHARS;
    return shards ? shards : 1;
}

/* Append one JSON string literal at *at. */
static int mpref_json_string(char *out, size_t cap, size_t *at, const char *text)
{
    size_t i = *at;
    if (i + 1 >= cap) return 0;
    out[i++] = '"';
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        char escape[8];
        size_t n = 1;
        if (c == '"' || c == '\\') { escape[0] = '\\'; escape[1] = (char)c; n = 2; }
        else if (c < 0x20) { snprintf(escape, sizeof escape, "\\u%04x", c); n = 6; }
        else escape[0] = (char)c;
        if (i + n + 1 >= cap) return 0;
        memcpy(out + i, escape, n); i += n;
    }
    out[i++] = '"'; out[i] = 0; *at = i;
    return 1;
}

static int mpref_json_member(char *out, size_t cap, size_t *at, const char *key, const char *text)
{
    int n = snprintf(out + *at, cap - *at, ",\"%s\":", key);
    if (n < 0 || (size_t)n >= cap - *at) return 0;
    *at += (size_t)n;
    return mpref_json_string(out, cap, at, text);
}

static int mpref_body(const sh_mpkg_reference *r, char *out, size_t cap, size_t *length)
{
    size_t at = 0;
    int n;
    n = snprintf(out, cap, "{\"id\":"); if (n < 0 || (size_t)n >= cap) return 0; at = (size_t)n;
    if (!mpref_json_string(out, cap, &at, r->id) || !mpref_json_member(out, cap, &at, "name", r->name)) return 0;
    if (r->author[0] && !mpref_json_member(out, cap, &at, "author", r->author)) return 0;
    if (r->homepage[0] && !mpref_json_member(out, cap, &at, "homepage", r->homepage)) return 0;
    n = snprintf(out + at, cap - at, ",\"packed\":%llu,\"sha256\":\"%s\"}", r->packed, r->sha256);
    if (n < 0 || (size_t)n >= cap - at) return 0;
    *length = at + (size_t)n;
    return 1;
}

static int mpref_hex64(const char *text)
{
    size_t i;
    for (i = 0; i < 64; i++) if (!sh_shard_is_hex(text[i])) return 0;
    return !text[64];
}

/* A decoded JSON string member, or empty when absent. Over-long text is cut at
 * the field's capacity rather than refused: it is display text. */
static int mpref_string_member(const sh_json_object *object, const char *key, char *out, size_t cap, int required)
{
    const char *raw = sh_json_object_get(object, key);
    char scratch[MPREF_BODY_CAP];
    size_t n = 0;
    out[0] = 0;
    if (!raw) return !required;
    if (!sh_json_decode_string(raw, strlen(raw), scratch, sizeof scratch, &n) || n >= sizeof scratch ||
        strlen(scratch) != n) return 0;
    strncpy_s(out, cap, scratch, _TRUNCATE);
    return 1;
}

static int mpref_parse_body(const unsigned char *body, size_t length, sh_mpkg_reference *r)
{
    sh_json_object object = {0};
    const char *raw;
    char *end = NULL;
    int ok = 0;
    memset(r, 0, sizeof *r);
    if (!sh_json_parse_object((const char *)body, length, 4, &object)) return 0;
    if (!mpref_string_member(&object, "id", r->id, sizeof r->id, 1) || !sh_package_id_valid(r->id) ||
        !mpref_string_member(&object, "name", r->name, sizeof r->name, 1) || !r->name[0] ||
        !mpref_string_member(&object, "author", r->author, sizeof r->author, 0) ||
        !mpref_string_member(&object, "homepage", r->homepage, sizeof r->homepage, 0) ||
        !mpref_string_member(&object, "sha256", r->sha256, sizeof r->sha256, 1) || !mpref_hex64(r->sha256)) goto done;
    raw = sh_json_object_get(&object, "packed");
    if (!raw || !sh_shard_is_digit(raw[0])) goto done;
    r->packed = _strtoui64(raw, &end, 10);
    if (!r->packed || end == raw || (*end && !sh_shard_is_ws(*end))) goto done;
    ok = 1;
done:
    sh_json_object_free(&object);
    return ok;
}

static int mpref_descriptor_string(const sh_package_descriptor *descriptor, const char *field, char *out, size_t cap)
{
    const char *raw = sh_package_descriptor_section(descriptor, field);
    char scratch[MPREF_BODY_CAP];
    size_t n = 0;
    out[0] = 0;
    if (!raw || raw[0] != '"') return 1;   /* absent or not a string: omitted */
    if (!sh_json_decode_string(raw, strlen(raw), scratch, sizeof scratch, &n) || n >= sizeof scratch ||
        strlen(scratch) != n) return 1;
    strncpy_s(out, cap, scratch, _TRUNCATE);
    return 1;
}

int sh_mpkg_reference_describe(const char *key, const unsigned char *archive, size_t length,
                               sh_mpkg_reference *out, char *err, size_t err_cap)
{
    sh_package_archive_files files = {0};
    sh_package_descriptor descriptor = {0};
    BCRYPT_ALG_HANDLE algorithm = NULL;
    unsigned char digest[32];
    char seed[SH_PACKAGE_ID_CAP];
    size_t i;
    int ok = 0, parsed = 0;
    if (err && err_cap) err[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!key || !archive || !length || length > ULONG_MAX ||
        (mpkg_delivery_id(key) ? strcpy_s(out->key, sizeof out->key, key) != 0 :
            !sh_package_id_normalize(key, out->key))) {
        mpkg_err(err, err_cap, "reference describes nothing"); return 0;
    }
    out->tree_known = sh_package_bundle_parse_key(out->key, out->tree, seed);
    if (!sh_package_archive_read(archive, length, &files, err, err_cap)) return 0;
    for (i = 0; i < files.count; i++)
        if (!files.items[i].directory && !strcmp(files.items[i].name, "package.json")) break;
    if (i == files.count) { mpkg_err(err, err_cap, "the delivery has no root package.json to describe"); goto done; }
    parsed = sh_package_descriptor_parse((const char *)files.items[i].body, files.items[i].length,
                                         &descriptor, err, err_cap);
    if (!parsed) goto done;
    strcpy_s(out->id, sizeof out->id, descriptor.id);
    strcpy_s(out->name, sizeof out->name, descriptor.name);
    mpref_descriptor_string(&descriptor, "author", out->author, sizeof out->author);
    mpref_descriptor_string(&descriptor, "homepage", out->homepage, sizeof out->homepage);
    out->packed = length;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptHash(algorithm, NULL, 0, (PUCHAR)archive, (ULONG)length, digest, sizeof digest) < 0) {
        mpkg_err(err, err_cap, "cannot digest the delivery for its reference"); goto done;
    }
    for (i = 0; i < 32; i++) snprintf(out->sha256 + i * 2, 3, "%02x", digest[i]);
    ok = 1;
done:
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (parsed) sh_package_descriptor_free(&descriptor);
    sh_package_archive_files_free(&files);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

char *sh_mpkg_reference_embed(const char *json, size_t len, const sh_mpkg_reference *reference,
                              size_t *out_len, char *err, size_t err_cap)
{
    char body[MPREF_BODY_CAP], header[SH_MPKG_HEADER_CAP], digest[SH_MPKG_DIGEST_CHARS + 1];
    char *base = NULL, *bare = NULL, *b64 = NULL, *out = NULL;
    const char *src;
    size_t src_len, body_len = 0, b64_len, w = 0;
    sh_shard_out part;
    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = 0;
    if (!json || !len || !reference || !reference->key[0] || !reference->id[0] || !reference->name[0] ||
        !mpref_hex64(reference->sha256) || !reference->packed) {
        mpkg_err(err, err_cap, "reference embed called with nothing to embed"); return NULL;
    }
    if (!mpref_body(reference, body, sizeof body, &body_len)) {
        mpkg_err(err, err_cap, "the package reference does not fit one shard"); return NULL;
    }
    /* A reference replaces both a prior reference and a prior delivery of its key. */
    base = mpkg_strip_scoped(json, len, reference->key, &src_len);
    src = base ? base : json; if (!base) src_len = len;
    bare = mpref_strip_scoped(src, src_len, reference->key, &w);
    if (bare) { src = bare; src_len = w; }
    b64_len = ((body_len + 2) / 3) * 4;
    b64 = (char *)HeapAlloc(GetProcessHeap(), 0, b64_len + 1);
    if (!b64) { mpkg_err(err, err_cap, "out of memory encoding the reference"); goto done; }
    b64_len = sh_shard_b64_encode((const unsigned char *)body, body_len, b64);
    sh_shard_digest16((const unsigned char *)body, body_len, digest);
    _snprintf_s(header, sizeof header, _TRUNCATE, "%s%s.%s", SH_MPREF_MAGIC, reference->key, digest);
    part.header = header; part.chunk = b64; part.chunk_len = b64_len;
    out = sh_shard_insert(src, src_len, &part, 1, &w, err, err_cap);
    if (out) {
        char line[SH_MPKG_ID_CAP + 256];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "MPKG: package '%s' (%s) REFERENCED by the map -- its %llu packed bytes stay outside it; "
                    "players need it installed; map %zu -> %zu bytes",
                    reference->name, reference->id, reference->packed, len, w);
        backend_log(line);
        if (out_len) *out_len = w;
    }
done:
    if (b64) HeapFree(GetProcessHeap(), 0, b64);
    if (bare) HeapFree(GetProcessHeap(), 0, bare);
    if (base) HeapFree(GetProcessHeap(), 0, base);
    return out;
}

size_t sh_mpkg_reference_scan(const char *json, size_t len, sh_mpkg_reference *out, size_t cap)
{
    size_t pos = 0, count = 0, hdr_len, chunk_len;
    const char *hdr, *chunk;
    if (!json || !len || !sh_shard_find(json, len, SH_MPREF_MAGIC, MPREF_MAGIC_LEN)) return 0;
    while (sh_shard_next(json, len, SH_MPREF_MAGIC, MPREF_MAGIC_LEN, &pos, &hdr, &hdr_len, &chunk, &chunk_len)) {
        mpref_hdr parsed;
        sh_mpkg_reference r;
        sh_shard_chunk one;
        unsigned char *body;
        char digest[SH_MPKG_DIGEST_CHARS + 1], seed[SH_PACKAGE_ID_CAP];
        size_t body_len = 0, i;
        int reason = 0, ok;
        if (!mpref_parse_header(hdr, hdr_len, &parsed)) continue;   /* not a reference */
        if (!chunk || !chunk_len || chunk_len > SH_SHARD_MAX_CHUNK) return SIZE_MAX;
        one.p = chunk; one.n = chunk_len; one.filled = 1;
        body = sh_shard_b64_decode(&one, 1, MPREF_BODY_CAP, &body_len, &reason);
        if (!body) return SIZE_MAX;
        sh_shard_digest16(body, body_len, digest);
        ok = !strcmp(digest, parsed.digest) && mpref_parse_body(body, body_len, &r);
        HeapFree(GetProcessHeap(), 0, body);
        if (!ok) return SIZE_MAX;
        strcpy_s(r.key, sizeof r.key, parsed.key);
        strcpy_s(r.digest, sizeof r.digest, parsed.digest);
        r.tree_known = sh_package_bundle_parse_key(r.key, r.tree, seed);
        if (out) {
            for (i = 0; i < count && i < cap; i++) if (!strcmp(out[i].key, r.key)) break;
            if (i < count && i < cap) {
                if (strcmp(out[i].digest, r.digest)) return SIZE_MAX;   /* one key, two bodies */
                continue;
            }
            if (count >= cap) return SIZE_MAX;
            out[count] = r;
        }
        count++;
    }
    return count;
}

char *sh_mpkg_embed_within(const char *json, size_t len, const sh_mpkg_delivery *items, size_t count,
                           size_t text_limit, int *referenced, size_t *out_len, size_t *failed_item,
                           char *err, size_t err_cap)
{
    sh_mpkg_delivery *fitting = NULL;
    size_t *origin = NULL;
    char *base = NULL, *current = NULL, *result = NULL;
    size_t i, base_len = len, current_len = 0;
    if (out_len) *out_len = 0;
    if (failed_item) *failed_item = SIZE_MAX;
    if (err && err_cap) err[0] = '\0';
    if (!json || !len || !items || !count || !referenced) { mpkg_err(err, err_cap, "embed called with nothing to embed"); return NULL; }
    if (count > SIZE_MAX / sizeof(*fitting) || !(fitting = (sh_mpkg_delivery *)calloc(count, sizeof(*fitting))) ||
        !(origin = (size_t *)calloc(count, sizeof(*origin)))) {
        mpkg_err(err, err_cap, "out of memory preparing the map's packages"); goto done;
    }
    for (i = 0; i < count; i++) {
        if (!items[i].id || !items[i].archive || !items[i].length) {
            if (failed_item) *failed_item = i;
            mpkg_err(err, err_cap, "embed called with nothing to embed"); goto done;
        }
        referenced[i] = items[i].length > SH_MPKG_DELIVERY_BUDGET_BYTES ||
            sh_mpkg_shards_for(items[i].length) > SH_MPKG_DELIVERY_BUDGET_SHARDS;
        if (referenced[i]) {
            char line[SH_MPKG_ID_CAP + 160];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: package '%s' needs %zu packed bytes in %zu shard(s), over the %zu-byte / %u-shard "
                        "delivery budget; it will be referenced", items[i].id, items[i].length,
                        sh_mpkg_shards_for(items[i].length), SH_MPKG_DELIVERY_BUDGET_BYTES, SH_MPKG_DELIVERY_BUDGET_SHARDS);
            backend_log(line);
        }
    }
    /* The map's earlier references are replaced by this set's decision. */
    base = mpref_strip_scoped(json, len, NULL, &base_len);
    if (!base) base_len = len;
    for (;;) {
        size_t embedded = 0, next_len = 0, failed = SIZE_MAX, largest = SIZE_MAX;
        for (i = 0; i < count; i++) if (!referenced[i]) { origin[embedded] = i; fitting[embedded++] = items[i]; }
        if (!embedded) {
            current = (char *)HeapAlloc(GetProcessHeap(), 0, base_len + 1);
            if (!current) { mpkg_err(err, err_cap, "out of memory building the map"); goto done; }
            memcpy(current, base ? base : json, base_len); current[base_len] = 0; current_len = base_len;
            break;
        }
        current = sh_mpkg_embed_set(base ? base : json, base_len, fitting, embedded, &next_len, &failed, err, err_cap);
        if (!current) { if (failed_item && failed < embedded) *failed_item = origin[failed]; goto done; }
        current_len = next_len;
        if (!text_limit || next_len <= text_limit) break;
        for (i = 0; i < count; i++)
            if (!referenced[i] && (largest == SIZE_MAX || items[i].length > items[largest].length)) largest = i;
        referenced[largest] = 1;
        {
            char line[SH_MPKG_ID_CAP + 160];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: the map text would be %zu bytes with its packages, over the %zu-byte bound; "
                        "package '%s' (%zu packed bytes) will be referenced", next_len, text_limit,
                        items[largest].id, items[largest].length);
            backend_log(line);
        }
        HeapFree(GetProcessHeap(), 0, current); current = NULL;
    }
    for (i = 0; i < count; i++) {
        sh_mpkg_reference reference;
        char *next;
        size_t next_len = 0;
        if (!referenced[i]) continue;
        if (!sh_mpkg_reference_describe(items[i].id, items[i].archive, items[i].length, &reference, err, err_cap) ||
            !(next = sh_mpkg_reference_embed(current, current_len, &reference, &next_len, err, err_cap))) {
            if (failed_item) *failed_item = i;
            goto done;
        }
        HeapFree(GetProcessHeap(), 0, current); current = next; current_len = next_len;
    }
    result = current; current = NULL;
    if (out_len) *out_len = current_len;
done:
    if (current) HeapFree(GetProcessHeap(), 0, current);
    if (base) HeapFree(GetProcessHeap(), 0, base);
    free(fitting); free(origin);
    return result;
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

/* One carrier's stored payload, exactly as transported: an archive or a delta. */
static unsigned char *mpkg_extract_transport(const char *json, size_t len, const char *pkg_id,
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

/* A delta's base: the payload stored under that key in the same map. The
 * decoder refuses one that is itself a delta. */
typedef struct mpkg_map_text { const char *json; size_t length; } mpkg_map_text;
static unsigned char *mpkg_delta_base(void *context, const char *key, size_t *length,
                                      char *error, size_t capacity)
{
    const mpkg_map_text *map = (const mpkg_map_text *)context;
    return mpkg_extract_transport(map->json, map->length, key, length, error, capacity);
}

unsigned char *sh_mpkg_extract(const char *json, size_t len, const char *pkg_id,
                               size_t *out_len, char *err, size_t err_cap)
{
    mpkg_map_text map = {json, len};
    unsigned char *stored = mpkg_extract_transport(json, len, pkg_id, out_len, err, err_cap), *archive;
    size_t stored_length;
    char reason[SH_MPKG_ERR_CAP];
    if (!stored || !sh_mpkg_delta_is(stored, *out_len)) return stored;
    stored_length = *out_len;
    archive = sh_mpkg_delta_decode(stored, stored_length, pkg_id, mpkg_delta_base, &map,
                                   SH_MPKG_MAX_PAYLOAD, out_len, reason, sizeof(reason));
    HeapFree(GetProcessHeap(), 0, stored);
    if (!archive) mpkg_err(err, err_cap, "package '%s': %s", pkg_id, reason);
    return archive;
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

void sh_mpkg_report_error(sh_mpkg_problem problem, const char *diagnostic)
{
    const char *message;
    if (!diagnostic || !*diagnostic) return;
    backend_log(diagnostic);
    switch (problem) {
    case SH_MPKG_PROBLEM_OPEN: message = "This map couldn't be opened."; break;
    case SH_MPKG_PROBLEM_SAVE:
        message = "Your changes couldn't be saved. Keep the map open and try saving again."; break;
    case SH_MPKG_PROBLEM_PLAY: message = "Play couldn't start."; break;
    case SH_MPKG_PROBLEM_INSTALL: message = "The extra content couldn't be installed."; break;
    case SH_MPKG_PROBLEM_PREFAB: message = "This prefab couldn't be added."; break;
    case SH_MPKG_PROBLEM_MODS_DISABLED:
        message = "This map needs mods, which are disabled for this launch."; break;
    case SH_MPKG_PROBLEM_RECORD:
        message = "Snapmap+ could not read the package record it saved in this map. This is a Snapmap+ defect, "
                  "not a damaged map; see the package log."; break;
    default: message = "SnapMap couldn't prepare this content."; break;
    }
    mpkg_lock();
    strncpy_s(g_notice, sizeof(g_notice), message, _TRUNCATE); g_notice_revision++;
    mpkg_unlock();
}

void sh_mpkg_report_notice(const char *text)
{
    if (!text || !*text) return;
    backend_log(text);
    mpkg_lock();
    strncpy_s(g_notice, sizeof(g_notice), text, _TRUNCATE); g_notice_revision++;
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
    char error[512] = "", line[320];
    LARGE_INTEGER frequency, began, recovered, scanned;
    unsigned long long bytes = 0;
    size_t files = 0;
    if (!data_root || !data_root[0]) return;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&began);
    mpkg_lock();
    if (g_boot_captured == 1) { mpkg_unlock(); return; }
    strncpy_s(g_data_root, sizeof(g_data_root), data_root, _TRUNCATE);
    sh_package_bundle_set_cache_root(data_root);
    if (!mpkg_recover_interrupted(data_root, error, sizeof(error))) {
        g_boot_captured = -1; mpkg_unlock(); sh_mpkg_report_error(SH_MPKG_PROBLEM_CONTENT, error); return;
    }
    QueryPerformanceCounter(&recovered);
    sources = sh_package_sources_scan_local(data_root, mpkg_local_rejected, NULL, error, sizeof(error));
    QueryPerformanceCounter(&scanned);
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
    for (i = 0; i < sources->file_count; i++)
        if (!sources->files[i].directory) { files++; bytes += sources->files[i].length; }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "MPKG: exact authored package identities captured; no installation sidecars "
        "(recovery %.0f ms, scan %.0f ms: %zu packages, %zu files, %.1f MB read and hashed)",
        (double)(recovered.QuadPart - began.QuadPart) * 1000.0 / (double)frequency.QuadPart,
        (double)(scanned.QuadPart - recovered.QuadPart) * 1000.0 / (double)frequency.QuadPart,
        sources->package_count, files, (double)bytes / 1048576.0);
    sh_package_sources_free(sources); g_boot_captured = 1; mpkg_unlock();
    backend_log(line);
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
static int mpkg_installed_kind(const unsigned char fingerprint[32])
{
    size_t i;
    for (i = 0; i < g_boot_count; i++) {
        const mpkg_boot_pkg *entry = &g_boot[i];
        if (!memcmp(entry->fingerprint, fingerprint, 32) &&
            mpkg_source_satisfies(entry->root, fingerprint)) return 1;
    }
    for (i = 0; i < g_session_count; i++) {
        mpkg_session_entry *entry = &g_session[i];
        if (entry->outcome != 1 || memcmp(entry->fingerprint, fingerprint, 32)) continue;
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
            if (component->relative[0] ||
                memcmp(sources->fingerprints[component->owner], fingerprint, 32)) continue;
            matches = mpkg_source_satisfies(sources->packages[component->owner].root, fingerprint);
            if (matches) break;
        }
        sh_package_runtime_release();
        if (matches) return 2;
    }
    return 0;
}

/* (lock held) The installed authored root whose complete tree is
 * `fingerprint`: the boot snapshot, a session install or the compiled
 * library, each revalidated on disk. 1 with root set, 0 when none. */
static int mpkg_installed_root(const unsigned char fingerprint[32], char root[MAX_PATH])
{
    size_t i;
    for (i = 0; i < g_boot_count; i++)
        if (!memcmp(g_boot[i].fingerprint, fingerprint, 32) && mpkg_source_satisfies(g_boot[i].root, fingerprint)) {
            strcpy_s(root, MAX_PATH, g_boot[i].root); return 1;
        }
    for (i = 0; i < g_session_count; i++)
        if (g_session[i].outcome == 1 && !memcmp(g_session[i].fingerprint, fingerprint, 32) &&
            mpkg_source_satisfies(g_session[i].root, fingerprint)) {
            strcpy_s(root, MAX_PATH, g_session[i].root); return 1;
        }
    {
        const sh_package_compilation *compiled = sh_package_runtime_library_acquire();
        const sh_package_sources *sources = compiled ? compiled->sources : NULL;
        int found = 0;
        if (sources && sources->fingerprints) for (i = 0; i < sources->component_count && !found; i++) {
            const sh_package_component *component = &sources->components[i];
            if (component->relative[0] || memcmp(sources->fingerprints[component->owner], fingerprint, 32)) continue;
            if (mpkg_source_satisfies(sources->packages[component->owner].root, fingerprint)) {
                strcpy_s(root, MAX_PATH, sources->packages[component->owner].root); found = 1;
            }
        }
        sh_package_runtime_release();
        if (found) return 1;
    }
    return 0;
}

/* The exact complete tree of an installed package as a retained bundle, or
 * NULL when no installed package has that tree. */
static sh_package_bundle *mpkg_installed_bundle(const unsigned char fingerprint[32], const char *cache_root,
    char *error, size_t capacity)
{
    char root[MAX_PATH], detail[256] = "";
    sh_package_sources *sources;
    sh_package_bundle *bundle = NULL;
    int found;
    mpkg_lock(); found = mpkg_installed_root(fingerprint, root); mpkg_unlock();
    if (!found) return NULL;
    /* The library captured this exact tree as a bundle when it compiled it; retaining that bundle spares reading
     * and packing the installed tree again for every map that references it. */
    bundle = sh_package_runtime_installed_bundle(fingerprint);
    if (bundle) return bundle;
    sources = sh_package_sources_scan_directory(root, detail, sizeof detail);
    if (sources && sources->package_count == 1 && sources->fingerprints && !memcmp(sources->fingerprints[0], fingerprint, 32))
        bundle = sh_package_bundle_capture(sources, 0, cache_root, error, capacity);
    sh_package_sources_free(sources);
    return bundle;
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
    char package_id[SH_PACKAGE_ID_CAP];
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

/* Recover journals written by earlier releases that replaced map-installed
 * same-ID packages. New installations never supersede a distinct source tree.
 * Keep this reader until every old pending journal can recover its data. */
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
    sh_package_bundle_binding *bundles;
    size_t bundle_count;
    char (*carriers)[SH_MPKG_ID_CAP]; /* exact transport id unpacked into each ordinal folder */
    struct sh_mpkg_context *cleanup_next, *live_next;
};
static SRWLOCK g_context_cleanup_lock = SRWLOCK_INIT;
static sh_mpkg_context *g_context_cleanup;
/* Open contexts, so installation can resolve a scanned private owner folder to
 * the exact carrier its context unpacked there. */
static SRWLOCK g_context_live_lock = SRWLOCK_INIT;
static sh_mpkg_context *g_context_live;

static void mpkg_context_publish(sh_mpkg_context *context)
{
    AcquireSRWLockExclusive(&g_context_live_lock);
    context->live_next = g_context_live; g_context_live = context;
    ReleaseSRWLockExclusive(&g_context_live_lock);
}

static void mpkg_context_withdraw(sh_mpkg_context *context)
{
    AcquireSRWLockExclusive(&g_context_live_lock);
    for (sh_mpkg_context **link = &g_context_live; *link; link = &(*link)->live_next)
        if (*link == context) { *link = context->live_next; break; }
    context->live_next = NULL;
    ReleaseSRWLockExclusive(&g_context_live_lock);
}

/* 1 with the carrier an open context unpacked into this private owner folder,
 * -1 for an unrecorded folder of an open context, 0 outside open contexts. */
static int mpkg_context_carrier_of(const char *owner_root, char out[SH_MPKG_ID_CAP])
{
    int found = 0;
    AcquireSRWLockShared(&g_context_live_lock);
    for (const sh_mpkg_context *c = g_context_live; c && !found; c = c->live_next) {
        size_t n = strlen(c->root);
        if (!n || _strnicmp(owner_root, c->root, n) || _strnicmp(owner_root + n, "\\overrides\\", 11)) continue;
        found = -1;
        for (size_t k = 0; k < c->count; k++)
            if (!strcmp(c->bundles[k].source_name, owner_root + n + 11)) {
                strcpy_s(out, SH_MPKG_ID_CAP, c->carriers[k]); found = 1; break;
            }
    }
    ReleaseSRWLockShared(&g_context_live_lock);
    return found;
}

const char *sh_mpkg_context_root(const sh_mpkg_context *context)
{ return context && context->root[0] ? context->root : NULL; }

size_t sh_mpkg_context_count(const sh_mpkg_context *context)
{ return context ? context->count : 0; }

const sh_package_bundle_binding *sh_mpkg_context_bundles(const sh_mpkg_context *context, size_t *count)
{
    if (count) *count = context ? context->count : 0;
    return context ? context->bundles : NULL;
}

const char *sh_mpkg_context_carrier(const sh_mpkg_context *context, size_t index)
{ return context && context->carriers && index < context->count ? context->carriers[index] : NULL; }

int sh_mpkg_context_close(sh_mpkg_context **context)
{
    wchar_t *wide;
    int ok;
    if (!context || !*context) return 1;
    if ((*context)->root[0]) {
        /* Only this opaque handle can supply the private directory name. */
        if (!mpkg_plain_ancestors((*context)->root)) return 0;
        /* Release the owner lock before deleting; a directory that will not
         * go yet is held again so no sweep takes it while the retry waits. */
        sh_package_cache_directory_drop((*context)->root);
        wide = sh_package_source_wide_path((*context)->root);
        ok = wide && mpkg_discard_tree(wide);
        free(wide);
        if (!ok) { sh_package_cache_directory_hold((*context)->root); return 0; }
    }
    mpkg_context_withdraw(*context);
    for (size_t i = 0; i < (*context)->bundle_count; i++) sh_package_bundle_release((*context)->bundles[i].original);
    free((*context)->carriers); free((*context)->bundles); free(*context); *context = NULL; return 1;
}

void sh_mpkg_context_retire(sh_mpkg_context **context)
{
    sh_mpkg_context *retired;
    if (sh_mpkg_context_close(context)) return;
    retired = *context; *context = NULL;
    mpkg_context_withdraw(retired);
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

/* ---- One decoded copy of a complete tree per process ----
 * A context's private owner folder used to be unpacked from its archive on every open: 1489 files for
 * boss-demons, about 3.7 s of file creation however the bytes arrive (carried in the map or taken from the
 * library's bundle). The first context to need a tree now unpacks and verifies it once, below
 * package-cache/maps/tree-<64 hex of the tree digest>/tree, holding that directory with the same owner lock the
 * context directories hold, so the startup sweep of a later process reclaims it with them and a live process
 * keeps it; the directory is complete when `tree` exists, because it is renamed into place after verification.
 * Every context's folder is then filled with hard links to that copy: plain files on the same volume, no
 * reparse points, deleted with the folder when the context closes while the shared copy stays. A tree whose
 * links cannot be made (another volume, a filesystem without links) is unpacked into the folder as before.
 * The caller still verifies the folder's own digest. */
static SRWLOCK g_tree_lock = SRWLOCK_INIT;
static char **g_trees; static size_t g_tree_count, g_tree_capacity; /* the held map-<digest> directories */
void sh_mpkg_shared_trees_release(void)
{
    AcquireSRWLockExclusive(&g_tree_lock);
    for (size_t i = 0; i < g_tree_count; i++) {
        wchar_t *wide = sh_package_source_wide_path(g_trees[i]);
        sh_package_cache_directory_drop(g_trees[i]);
        if (wide) mpkg_discard_tree(wide);
        free(wide); free(g_trees[i]);
    }
    free(g_trees); g_trees = NULL; g_tree_count = g_tree_capacity = 0;
    ReleaseSRWLockExclusive(&g_tree_lock);
}
/* (g_tree_lock held) Remember a held directory once. */
static int mpkg_tree_remember(const char *owner)
{
    for (size_t i = 0; i < g_tree_count; i++) if (!strcmp(g_trees[i], owner)) return 1;
    if (g_tree_count == g_tree_capacity) {
        size_t next = g_tree_capacity ? g_tree_capacity * 2 : 8;
        char **grown = next > g_tree_capacity && next <= SIZE_MAX / sizeof(*grown) ? realloc(g_trees, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        g_trees = grown; g_tree_capacity = next;
    }
    if (!(g_trees[g_tree_count] = _strdup(owner))) return 0;
    g_tree_count++; return 1;
}
static int mpkg_link_tree(const wchar_t *from, const wchar_t *to, unsigned depth)
{
    WIN32_FIND_DATAW found;
    HANDLE search;
    size_t from_length = wcslen(from), to_length = wcslen(to);
    size_t capacity = (from_length > to_length ? from_length : to_length) + MAX_PATH + 2u;
    wchar_t *child_from, *child_to, *pattern;
    DWORD last;
    int ok = 1;
    /* Never a folder inside the copy it links, and never deeper than a package tree can be. */
    if (depth > 32 || capacity > SH_PACKAGE_SOURCE_PATH_CAP || !_wcsnicmp(to, from, from_length) ||
        !CreateDirectoryW(to, NULL)) return 0;
    child_from = (wchar_t *)malloc(capacity * sizeof(*child_from));
    child_to = (wchar_t *)malloc(capacity * sizeof(*child_to));
    pattern = (wchar_t *)malloc((from_length + 3u) * sizeof(*pattern));
    if (!child_from || !child_to || !pattern) { free(child_from); free(child_to); free(pattern); return 0; }
    swprintf_s(pattern, from_length + 3u, L"%s\\*", from);
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) ok = 0;
    else {
        do {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ok = 0; break; }
            swprintf_s(child_from, capacity, L"%s\\%s", from, found.cFileName);
            swprintf_s(child_to, capacity, L"%s\\%s", to, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!mpkg_link_tree(child_from, child_to, depth + 1)) { ok = 0; break; } }
            else if (!CreateHardLinkW(child_to, child_from, NULL)) { ok = 0; break; }
        } while (FindNextFileW(search, &found));
        last = GetLastError(); FindClose(search);
        if (ok && last != ERROR_NO_MORE_FILES) ok = 0;
    }
    free(child_from); free(child_to); free(pattern);
    return ok;
}
/* 1 with `tree` set to this process's verified copy of the tree, laying it out now when no one has yet; 0 when the
 * shared copy cannot be made (the caller unpacks privately). */
static int mpkg_shared_tree(const unsigned char *payload, size_t length, const unsigned char fingerprint[32],
                            const char *maps, char *tree, size_t tree_capacity)
{
    char owner[MAX_PATH], staging[MAX_PATH], detail[SH_MPKG_ERR_CAP] = "";
    wchar_t *wide_tree = NULL, *wide_staging = NULL;
    DWORD attributes;
    size_t i;
    int ok = 0;
    if (snprintf(owner, sizeof(owner), "%s\\tree-", maps) >= (int)sizeof(owner) - 65) return 0;
    for (i = 0; i < 32; i++) snprintf(owner + strlen(owner), 3, "%02x", fingerprint[i]);
    if (snprintf(tree, tree_capacity, "%s\\tree", owner) >= (int)tree_capacity) return 0;
    AcquireSRWLockExclusive(&g_tree_lock);
    if (!CreateDirectoryA(owner, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) goto done;
    /* Held for the rest of this process (or until sh_mpkg_shared_trees_release): the sweep leaves it alone
     * while we live and takes it after. */
    if (!sh_package_cache_directory_hold(owner)) goto done;
    if (!mpkg_tree_remember(owner)) { sh_package_cache_directory_drop(owner); goto done; }
    wide_tree = sh_package_source_wide_path(tree);
    if (!wide_tree) goto done;
    attributes = GetFileAttributesW(wide_tree);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        ok = (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        goto done;
    }
    {
        unsigned char random[8];
        char token[17];
        ULONGLONG began = GetTickCount64();
        unsigned files = 0;
        if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) goto done;
        for (i = 0; i < sizeof(random); i++) snprintf(token + i * 2, 3, "%02x", random[i]);
        if (snprintf(staging, sizeof(staging), "%s\\tree.%s", owner, token) >= (int)sizeof(staging)) goto done;
        wide_staging = sh_package_source_wide_path(staging);
        if (!wide_staging) goto done;
        if (!sh_package_archive_unpack_volatile(payload, length, staging, &files, detail, sizeof(detail)) ||
            !mpkg_source_satisfies(staging, fingerprint) || !MoveFileExW(wide_staging, wide_tree, 0)) {
            mpkg_discard_tree(wide_staging);
            attributes = GetFileAttributesW(wide_tree);
            ok = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(attributes & FILE_ATTRIBUTE_REPARSE_POINT); /* another thread completed it meanwhile */
            goto done;
        }
        {
            char line[200];
            _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: complete tree %.16s... laid out once for this process (%u files, %llu ms)",
                        tree + strlen(maps) + 6, files, (unsigned long long)(GetTickCount64() - began));
            backend_log(line);
        }
        ok = 1;
    }
done:
    ReleaseSRWLockExclusive(&g_tree_lock);
    free(wide_tree); free(wide_staging);
    return ok;
}
/* Fill a context's private owner folder with the tree: links to the process's shared copy, else a private unpack. */
static int mpkg_lay_out(const unsigned char *payload, size_t length, const unsigned char fingerprint[32],
                        const char *maps, const char *destination, char *error, size_t capacity)
{
    char tree[MAX_PATH];
    wchar_t *wide_tree, *wide_destination;
    int linked = 0;
    if (mpkg_shared_tree(payload, length, fingerprint, maps, tree, sizeof(tree))) {
        wide_tree = sh_package_source_wide_path(tree); wide_destination = sh_package_source_wide_path(destination);
        if (wide_tree && wide_destination) {
            linked = mpkg_link_tree(wide_tree, wide_destination, 0);
            if (!linked) mpkg_discard_tree(wide_destination);
        }
        free(wide_tree); free(wide_destination);
    }
    return linked || sh_package_archive_unpack_volatile(payload, length, destination, NULL, error, capacity);
}

sh_mpkg_context *sh_mpkg_context_open(const char *data_root, const char *json, size_t len,
    char *error, size_t capacity)
{
    sh_mpkg_context *context = NULL;
    sh_mpkg_decl *decls = NULL;
    sh_mpkg_reference *refs = NULL;
    sh_package_bundle **installed = NULL;
    char canonical[MAX_PATH], cache[MAX_PATH], maps[MAX_PATH], directory[MAX_PATH];
    DWORD root_length;
    size_t count, i, ref_count = 0, materialized = 0;
    int created = 0;
    sh_json_error problem;
    if (error && capacity) error[0] = 0;
    if (!sh_native_json_validate(json, len, 128, NULL, &problem)) {
        if (error && capacity) snprintf(error, capacity,
            "Map JSON rejected at byte %zu: %s.", problem.offset, problem.reason);
        return NULL;
    }
    count = mpkg_scan_internal(json, len, &decls);
    if (count == SIZE_MAX || !(context = calloc(1, sizeof(*context)))) {
        mpkg_err(error, capacity, "cannot allocate the complete map package context"); goto bad;
    }
    /* References (L2) whose complete tree an installed package has are laid
     * out beside the carried deliveries: a record that composed them needs
     * their trees in this context, and the library has the exact bytes. */
    ref_count = sh_mpkg_reference_scan(json, len, NULL, 0);
    if (ref_count == SIZE_MAX || ref_count > SIZE_MAX / sizeof(*refs) ||
        (ref_count && (!(refs = calloc(ref_count, sizeof(*refs))) || sh_mpkg_reference_scan(json, len, refs, ref_count) != ref_count))) {
        mpkg_err(error, capacity, "the map's package references are malformed"); goto bad;
    }
    if (!count && !ref_count) { free(decls); return context; }
    root_length = data_root && data_root[0] ? GetFullPathNameA(data_root, sizeof(canonical), canonical, NULL) : 0;
    if (!root_length || root_length >= sizeof(canonical) || !mpkg_plain_ancestors(canonical)) {
        if (!count) { free(decls); free(refs); return context; }
        mpkg_err(error, capacity, "private map source cache is unavailable or redirected"); goto bad;
    }
    if (ref_count && !(installed = calloc(ref_count, sizeof(*installed)))) {
        mpkg_err(error, capacity, "cannot retain the map's installed references"); goto bad;
    }
    for (i = 0; i < ref_count; i++) {
        char detail[SH_MPKG_ERR_CAP] = "";
        if (!refs[i].tree_known) continue;
        installed[i] = mpkg_installed_bundle(refs[i].tree, canonical, detail, sizeof detail);
        if (installed[i]) materialized++;
        else if (detail[0]) {
            char line[SH_MPKG_ERR_CAP + SH_PACKAGE_DISPLAY_CAP + 96];
            _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: referenced package '%s' is installed but could not be retained for the map's context: %s",
                        refs[i].name, detail);
            backend_log(line);
        }
    }
    if (!count && !materialized) { free(decls); free(refs); free(installed); return context; }
    if (count + materialized > SIZE_MAX / sizeof(*context->bundles) ||
        !(context->bundles = calloc(count + materialized, sizeof(*context->bundles))) ||
        !(context->carriers = calloc(count + materialized, sizeof(*context->carriers)))) {
        mpkg_err(error, capacity, "cannot retain original map package archives"); goto bad;
    }
    context->bundle_count = count + materialized;
    if (snprintf(cache, sizeof(cache), "%s\\package-cache", canonical) >= sizeof(cache) ||
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
    /* Own the directory while this context lives: the startup sweep removes
     * only extraction directories whose owner lock it can take. */
    if (!sh_package_cache_directory_hold(context->root)) {
        mpkg_err(error, capacity, "cannot reserve private map source directory"); goto bad;
    }
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
        sh_package_bundle_binding *binding = context->bundles + i;
        payload = mpkg_compiled_payload_retained(json, len, decls[i].id, &payload_length,
            canonical, &binding->original, error, capacity);
        if (!payload) goto bad;
        ok = sh_package_archive_identity(payload, payload_length, id, fingerprint, detail, sizeof(detail));
        if (!ok && error && capacity)
            snprintf(error, capacity, "Embedded package '%s': %s", decls[i].id, detail);
        if (ok && strcmp(id, decls[i].id) && !mpkg_delivery_id(decls[i].id)) {
            mpkg_err(error, capacity, "map package descriptor does not match its delivery identity"); ok = 0;
        }
        /* The private owner folder is the carrier's decimal ordinal: unique
         * within this context and independent of the carrier's spelling, so a
         * historical map and its modern re-save keep the same directory
         * boundary and every earlier first open stays supported. It is never
         * serialized and never stands for authorship; the carrier record names
         * the exact carrier it holds. */
        if (ok && (strcpy_s(context->carriers[i], SH_MPKG_ID_CAP, decls[i].id) ||
            snprintf(binding->source_name, sizeof(binding->source_name), "%zu", i) < 0 ||
            snprintf(destination, sizeof(destination), "%s\\%s", directory, binding->source_name) >= sizeof(destination))) {
            mpkg_err(error, capacity, "private map package root is too long"); ok = 0;
        }
        if (ok) ok = mpkg_lay_out(payload, payload_length, fingerprint, maps, destination, error, capacity);
        HeapFree(GetProcessHeap(), 0, payload);
        if (ok && !mpkg_source_satisfies(destination, fingerprint)) {
            mpkg_err(error, capacity, "private map source verification failed"); ok = 0;
        }
        if (!ok) goto bad;
        memcpy(binding->compiler_tree, fingerprint, 32);
        context->count++;
    }
    for (i = 0; i < ref_count; i++) {
        unsigned char *payload;
        size_t payload_length = 0, slot = context->count;
        char destination[MAX_PATH], line[SH_MPKG_ID_CAP + SH_PACKAGE_DISPLAY_CAP + 200];
        sh_package_bundle_binding *binding;
        ULONGLONG began = GetTickCount64(), read_at, unpacked_at;
        int ok;
        if (!installed[i]) continue;
        binding = context->bundles + slot;
        payload = sh_package_bundle_read(installed[i], &payload_length, error, capacity);
        read_at = GetTickCount64();
        ok = payload != NULL;
        if (ok && (strcpy_s(context->carriers[slot], SH_MPKG_ID_CAP, refs[i].key) ||
            snprintf(binding->source_name, sizeof(binding->source_name), "%zu", slot) < 0 ||
            snprintf(destination, sizeof(destination), "%s\\%s", directory, binding->source_name) >= sizeof(destination))) {
            mpkg_err(error, capacity, "private map package root is too long"); ok = 0;
        }
        if (ok) ok = mpkg_lay_out(payload, payload_length, refs[i].tree, maps, destination, error, capacity);
        free(payload);
        unpacked_at = GetTickCount64();
        if (ok && !mpkg_source_satisfies(destination, refs[i].tree)) {
            mpkg_err(error, capacity, "private map source verification failed"); ok = 0;
        }
        if (!ok) goto bad;
        memcpy(binding->compiler_tree, refs[i].tree, 32);
        binding->original = installed[i]; installed[i] = NULL;
        context->count++;
        _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: referenced package '%s' (%s) laid out in the map's context from the "
                    "installed library (tree %.16s..., %zu packed bytes; read %llu ms, laid out %llu ms, verified %llu ms)",
                    refs[i].name, refs[i].id, refs[i].key + 9, payload_length, (unsigned long long)(read_at - began),
                    (unsigned long long)(unpacked_at - read_at), (unsigned long long)(GetTickCount64() - unpacked_at));
        backend_log(line);
    }
    for (i = 0; i < ref_count; i++) sh_package_bundle_release(installed[i]);
    free(installed); free(refs);
    free(decls); mpkg_context_publish(context); return context;
bad:
    for (i = 0; refs && installed && i < ref_count; i++) sh_package_bundle_release(installed[i]);
    free(installed); free(refs);
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

/* Called at an admitted native boundary after staged consumers and allocator
 * scope have completed, while the previous compiler is still held. */
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
        mpkg_unlock(); sh_mpkg_report_error(SH_MPKG_PROBLEM_CONTENT, error); return 0;
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
    size_t added = 0, i;
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
        if (!sh_package_archive_identity(s->payload, s->payload_len, s->package_id,
                                        s->fingerprint, err, err_cap)) goto done;
        if (strcmp(s->package_id, s->id) && !mpkg_delivery_id(s->id)) {
            mpkg_err(err, err_cap, "package descriptor does not match its map identity"); goto done;
        }
    }
    for (s = head; s; s = s->next) {
        for (i = 0; i < sources->component_count; i++) {
            const sh_package_component *component = &sources->components[i];
            const char *package_root = sources->packages[component->owner].root;
            if (component->relative[0] || strcmp(component->descriptor.id, s->package_id)) continue;
            if (!memcmp(sources->fingerprints[component->owner], s->fingerprint, 32)) {
                strcpy_s(s->destination, sizeof(s->destination), package_root);
                s->existing = 1; continue;
            }
            /* An ID is metadata, not replacement authority. Keep every
             * distinct complete tree, including earlier map installations.
             * Source selection and native binding dedupe happen in the
             * compiler; this disk transaction only adds its accepted units. */
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
    if (added) {
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
            char leaf[SH_PACKAGE_ID_CAP];
            size_t suffix = 1;
            strcpy_s(leaf, sizeof(leaf), s->package_id);
            for (;;) {
                mpkg_staged *previous;
                if (snprintf(s->staging, sizeof(s->staging), "%s\\%s", content, leaf) >= sizeof(s->staging) ||
                    snprintf(s->destination, sizeof(s->destination), "%s\\%s", destination, leaf) >= sizeof(s->destination)) {
                    mpkg_err(err, err_cap, "package installation path is too long"); goto done;
                }
                for (previous = head; previous != s; previous = previous->next)
                    if (!previous->existing && !_stricmp(previous->staging, s->staging)) break;
                if (previous == s) break;
                snprintf(leaf, sizeof(leaf), "%.96s-%zu", s->package_id, ++suffix);
            }
            if (!sh_mpkg_unpack(s->payload, s->payload_len, s->staging, &s->files, err, err_cap)) goto done;
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
    if (!head->completion) { sh_package_runtime_note_install_published(); sh_decl_server_request_rearm(); }
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
            sh_mpkg_report_error(SH_MPKG_PROBLEM_INSTALL, line);
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

static int mpref_poll(void);
static void mpref_pending_cancel(void *context);

void sh_mpkg_cancel_map_consent(void *context)
{
    if (g_consent_staged && g_consent_staged->completion &&
        g_consent_staged->completion_context == context) mpkg_consent_finish(0);
    mpref_pending_cancel(context);
}

void sh_mpkg_consent_poll(void)
{
    LONG state = InterlockedCompareExchange(&g_consent_state, 0, 0);
    char text[256];
    mpkg_staged *s;

    if (state == MPKG_CONSENT_IDLE) { if (!mpref_poll()) mpkg_notice_poll(); return; }
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
                const char *label = item->package_id[0] ? item->package_id : item->id;
                size_t used = strlen(text), separator = listed ? 2 : 0;
                if (item->next) snprintf(more, sizeof(more), " and %u more", count - listed - 1);
                if (used + separator + strlen(label) + strlen(more) >= sizeof(text)) break;
                if (listed) strcat_s(text, sizeof(text), ", ");
                strcat_s(text, sizeof(text), label); listed++;
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
    if (sh_package_runtime_serving_disabled()) {
        mpkg_set_refusal("package serving is disabled for this launch (sh_user_overrides 0)"); return 0;
    }
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
        {
            const char *leaf = strrchr(sources->packages[owner].root, '\\');
            char delivery[SH_PACKAGE_ID_CAP], carrier[SH_MPKG_ID_CAP];
            int recorded = mpkg_context_carrier_of(sources->packages[owner].root, carrier);
            leaf = leaf ? leaf + 1 : sources->packages[owner].root;
            /* An open map context's owner resolves through its carrier record,
             * never through folder spelling or an authored id that resembles
             * it. Owners outside open contexts keep the former identity rules. */
            if (recorded > 0) {
                for (size_t i = 0; i < count; i++) if (!strcmp(decls[i].id, carrier)) { decl = &decls[i]; break; }
            } else if (!recorded) {
                sh_package_archive_delivery_id(sources->fingerprints[owner], delivery);
                for (size_t i = 0; i < count; i++)
                    if (!strcmp(decls[i].id, leaf) || !strcmp(decls[i].id, delivery)) { decl = &decls[i]; break; }
                if (!decl) for (size_t i = 0; i < count; i++)
                    if (!strcmp(decls[i].id, id)) { decl = &decls[i]; break; }
            }
        }
        if (!decl) { mpkg_err(error, capacity, "map omits supplying package '%s'", id); goto done; }
        /* Distinct owners must never be collapsed by an ambiguous identity. */
        for (entry = head; entry; entry = entry->next) if (!strcmp(entry->id, decl->id)) break;
        if (entry) {
            if (!memcmp(entry->fingerprint, sources->fingerprints[owner], 32)) continue;
            mpkg_err(error, capacity, "map has ambiguous delivery identity '%s'", decl->id); goto done;
        }
        entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
        if (!entry) goto done;
        if (tail) tail->next = entry; else head = entry;
        tail = entry;
        strcpy_s(entry->id, sizeof(entry->id), decl->id);
        strcpy_s(entry->package_id, sizeof(entry->package_id), id);
        strcpy_s(entry->digest, sizeof(entry->digest), decl->digest);
        entry->payload = mpkg_compiled_payload(json, length, decl->id, &entry->payload_len, error, capacity);
        if (!entry->payload || !sh_package_archive_identity(entry->payload, entry->payload_len,
            archive_id, fingerprint, error, capacity) || strcmp(archive_id, id) ||
            memcmp(fingerprint, sources->fingerprints[owner], 32) ||
            !mpkg_zip_survey(entry->payload, entry->payload_len, &entry->files, error, capacity)) {
            if (error && capacity && !error[0]) mpkg_err(error, capacity, "map package '%s' changed after compilation", id);
            goto done;
        }
        memcpy(entry->fingerprint, fingerprint, 32);
    }
    /* Reserve the entire request before any consent or publication. */
    if (!head) goto done;
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
        if (!valid || (strcmp(id, d->id) && !mpkg_delivery_id(d->id))) {
            _snprintf_s(reason, sizeof(reason), _TRUNCATE, "package '%s' cannot be loaded: %s", d->id,
                        valid ? "descriptor does not match its map identity" : error);
            mpkg_set_refusal(reason); return 0;
        }
        mpkg_lock(); installed[i] = mpkg_installed_kind(fingerprint); mpkg_unlock();
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
            if (!s->payload || !sh_package_archive_identity(s->payload, s->payload_len, s->package_id,
                s->fingerprint, err, sizeof(err)) ||
                !mpkg_zip_survey(s->payload, s->payload_len, &s->files, err, sizeof(err))) {
                complete = 0; break;
            }
        }
        if (!complete) {
            sh_mpkg_report_error(SH_MPKG_PROBLEM_OPEN, err); mpkg_retry_staged(head); mpkg_staged_free(head);
        } else if (head) mpkg_request_consent(head);   /* takes ownership of the chain */
    }
    return 0;
}

/* ==================================================================== */
/* references at load                                                    */
/* ==================================================================== */

/* A reference names a package that does not travel in the map. One with an
 * installed match proceeds silently; otherwise one notice names the package,
 * its author and homepage and asks whether to open the map without it. The
 * answer is remembered for the session under the reference's key and digest
 * (outcome MPREF_OPENED), separately from installation outcomes. */
#define MPREF_OPENED 5

typedef struct mpref_pending {
    sh_mpkg_reference *items;
    size_t count;
    sh_mpkg_install_completion completion;
    void *completion_context;
} mpref_pending;

static volatile LONG   g_reference_state;     /* MPKG_CONSENT_IDLE / RAISE / WAITING */
static mpref_pending  *g_reference_pending;   /* owned while not IDLE */
static int             g_reference_ticket;
static volatile LONG   g_reference_waited;

static void mpref_pending_free(mpref_pending *pending)
{
    if (!pending) return;
    free(pending->items); free(pending);
}

static void mpref_finish(int accepted)
{
    mpref_pending *pending = g_reference_pending;
    size_t i;
    if (g_reference_ticket) sh_engine_dialog_release(g_reference_ticket);
    g_reference_pending = NULL; g_reference_ticket = 0;
    InterlockedExchange(&g_reference_state, MPKG_CONSENT_IDLE);
    if (!pending) return;
    mpkg_lock();
    for (i = 0; i < pending->count; i++) {
        const sh_mpkg_reference *r = pending->items + i;
        mpkg_session_entry *e = mpkg_session_find(r->key, r->digest);
        if (!e) e = mpkg_session_add(r->key, r->digest, 3);
        if (e) e->outcome = accepted ? MPREF_OPENED : 3;
    }
    mpkg_unlock();
    for (i = 0; i < pending->count; i++) {
        char line[SH_PACKAGE_DISPLAY_CAP + SH_PACKAGE_ID_CAP + 160];
        _snprintf_s(line, sizeof line, _TRUNCATE, accepted ?
            "MPKG: the map opens without package '%s' (%s), which it references but does not carry; nothing was installed" :
            "MPKG: the map was not opened; it references package '%s' (%s), which is not installed; a later load asks again",
            pending->items[i].name, pending->items[i].id);
        backend_log(line);
    }
    if (pending->completion) pending->completion(pending->completion_context, accepted ? 1 : 0);
    mpref_pending_free(pending);
}

/* Withdraw a caller's pending reference notice; completion receives zero. */
static void mpref_pending_cancel(void *context)
{
    if (g_reference_pending && g_reference_pending->completion &&
        g_reference_pending->completion_context == context) mpref_finish(0);
}

static void mpref_copy_bounded(char *out, size_t cap, const char *text, size_t most)
{
    size_t n = strlen(text);
    if (n > most) n = most;
    if (n >= cap) n = cap - 1;
    memcpy(out, text, n); out[n] = 0;
    if (n < strlen(text) && n >= 3) memcpy(out + n - 3, "...", 3);
}

/* The decision first, then the package. The native body holds 256 bytes. */
static void mpref_text(const mpref_pending *pending, char *text, size_t cap)
{
    const sh_mpkg_reference *r = pending->items;
    char name[64], author[48], homepage[112], more[48] = "";
    unsigned long long tenths = (r->packed + 52428ull) / 104857ull;
    mpref_copy_bounded(name, sizeof name, r->name, 48);
    mpref_copy_bounded(author, sizeof author, r->author, 32);
    mpref_copy_bounded(homepage, sizeof homepage, r->homepage, 96);
    if (pending->count > 1)
        snprintf(more, sizeof more, " and %zu more package%s", pending->count - 1, pending->count > 2 ? "s" : "");
    _snprintf_s(text, cap, _TRUNCATE, "Open the map without it? It uses %s%s%s (%llu.%llu MB)%s, not installed and not "
                "carried in the map.%s%s", name, author[0] ? " by " : "", author, tenths / 10, tenths % 10, more,
                homepage[0] ? " Get it at " : "", homepage);
    if (homepage[0] && strlen(text) + 1 < cap) strcat_s(text, cap, ".");
}

/* One tick of the reference notice. 1 while it owns the dialog surface. */
static int mpref_poll(void)
{
    LONG state = InterlockedCompareExchange(&g_reference_state, 0, 0);
    char text[256];
    if (state == MPKG_CONSENT_IDLE) return 0;
    if (!g_reference_pending) { InterlockedExchange(&g_reference_state, MPKG_CONSENT_IDLE); return 0; }
    if (state == MPKG_CONSENT_RAISE) {
        if (!sh_engine_dialog_can_ask()) {
            if (InterlockedIncrement(&g_reference_waited) > MPKG_CONSENT_WAIT_TICKS) {
                backend_log("MPKG: the engine dialog surface did not become ready; the referenced package "
                            "was not asked about and the map was not opened");
                mpref_finish(0);
            }
            return 1;
        }
        mpref_text(g_reference_pending, text, sizeof text);
        g_reference_ticket = sh_engine_dialog_ask(MPKG_CONSENT_GDM_ID, MPKG_CONSENT_BUTTON_SET, text);
        if (!g_reference_ticket) {
            backend_log("MPKG: the engine dialog would not raise the package reference notice; the map was not opened");
            mpref_finish(0);
            return 1;
        }
        InterlockedExchange(&g_reference_state, MPKG_CONSENT_WAITING);
        return 1;
    }
    switch (sh_engine_dialog_poll(g_reference_ticket)) {
    case SH_ENGINE_DIALOG_PENDING: return 1;
    case SH_ENGINE_DIALOG_ACCEPTED: mpref_finish(1); return 1;
    default: mpref_finish(0); return 1;
    }
}

size_t sh_mpkg_reference_missing(const char *json, size_t len, unsigned char (*trees)[32], size_t cap)
{
    sh_mpkg_reference *refs = NULL;
    size_t count, missing = 0;
    if (!json || !len || !sh_shard_find(json, len, SH_MPREF_MAGIC, MPREF_MAGIC_LEN)) return 0;
    count = sh_mpkg_reference_scan(json, len, NULL, 0);
    if (count == SIZE_MAX || count > SIZE_MAX / sizeof(*refs)) return SIZE_MAX;
    if (!count) return 0;
    if (!(refs = (sh_mpkg_reference *)calloc(count, sizeof(*refs))) ||
        sh_mpkg_reference_scan(json, len, refs, count) != count) { free(refs); return SIZE_MAX; }
    for (size_t i = 0; i < count; i++) {
        int installed;
        if (!refs[i].tree_known) continue;
        mpkg_lock(); installed = mpkg_installed_kind(refs[i].tree) != 0; mpkg_unlock();
        if (installed) continue;
        if (trees && missing < cap) memcpy(trees[missing], refs[i].tree, 32);
        missing++;
    }
    free(refs);
    return missing;
}

/* ---- opening without a referenced package --------------------------------- */

/* The flat integer lists an entity's uniqueId appears in, and the entities
 * array itself, located in one walk of the document. Each is rebuilt whole
 * when an entity leaves; everything else stays byte-exact. */
enum {
    MPRM_ENTITIES, MPRM_UIDS, MPRM_INST_KEYS, MPRM_INST_VALUES, MPRM_CONNECTIONS, MPRM_DOORS, MPRM_FRAMES,
    MPRM_ENT_KEYS, MPRM_ENT_VALUES, MPRM_VAR_KEYS, MPRM_VAR_VALUES, MPRM_SOURCES, MPRM_TARGETS, MPRM_LISTS
};
static const char *const mprm_paths[MPRM_LISTS][3] = {
    {"entities", NULL, NULL}, {NULL, NULL, NULL},
    {"instanceEntities", "keyValues", NULL}, {"instanceEntities", "values", NULL},
    {"targets", "connections", NULL}, {"doorsAndCaps", "portalDoors", NULL}, {"doorsAndCaps", "portalFrames", NULL},
    {"references", "entityEntRefs", "keyValues"}, {"references", "entityEntRefs", "values"},
    {"references", "entityVarRefs", "keyValues"}, {"references", "entityVarRefs", "values"},
    {"targets", "entitySources", "values"}, {"targets", "entityTargets", "values"}
};
typedef struct mprm_item { size_t offset, length, index; long long number; int integral; } mprm_item;
typedef struct mprm_list {
    mprm_item *items; size_t count, capacity;
    size_t offset, length; int located;   /* the array's own span */
} mprm_list;
typedef struct mprm_scan { const char *json; mprm_list lists[MPRM_LISTS]; int failed; } mprm_scan;
typedef struct mprm_edit { size_t offset, length; char *text; size_t text_length; } mprm_edit;
typedef struct mprm_text { char *bytes; size_t length, capacity; int failed; } mprm_text;

static void mprm_edit_add(mprm_edit *edits, size_t *count, size_t offset, size_t length, mprm_text *text)
{
    edits[*count].offset = offset; edits[*count].length = length;
    edits[*count].text = text->bytes; edits[*count].text_length = text->length;
    (*count)++;
    memset(text, 0, sizeof(*text));
}

static int mprm_key_is(const sh_json_path *node, const char *key)
{ return node->key && node->key_length == strlen(key) && !memcmp(node->key, key, node->key_length); }

static int mprm_push(mprm_list *list, size_t offset, size_t length, size_t index, const char *json)
{
    mprm_item *item;
    if (list->count == list->capacity) {
        size_t grown = list->capacity ? list->capacity * 2 : 16;
        mprm_item *more;
        if (grown > SIZE_MAX / sizeof(*more) || !(more = realloc(list->items, grown * sizeof(*more)))) return 0;
        list->items = more; list->capacity = grown;
    }
    item = list->items + list->count++;
    item->offset = offset; item->length = length; item->index = index; item->number = 0; item->integral = 0;
    if (json && length && length < 24) {
        char digits[24], *end = NULL;
        memcpy(digits, json + offset, length); digits[length] = 0;
        item->number = _strtoi64(digits, &end, 10);
        item->integral = end && *end == 0 && (digits[0] == '-' ? length > 1 : 1);
    }
    return 1;
}

/* The list an element path (last node an index) or an array path names. */
static int mprm_list_at(const sh_json_path *const *nodes, size_t keys)
{
    for (int list = 0; list < MPRM_LISTS; list++) {
        size_t n = 0;
        if (list == MPRM_UIDS) continue;
        while (n < 3 && mprm_paths[list][n]) n++;
        if (n != keys) continue;
        for (size_t k = 0; k < n; k++) if (!mprm_key_is(nodes[k], mprm_paths[list][k])) { n = 0; break; }
        if (n) return list;
    }
    return -1;
}

static int mprm_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    mprm_scan *scan = context;
    const sh_json_path *nodes[5];
    size_t depth = 0, offset = (size_t)(json - scan->json);
    int list = -1;
    for (const sh_json_path *p = path; p; p = p->parent) { if (depth == 5) return 1; nodes[depth++] = p; }
    for (size_t i = 0; i < depth / 2; i++) { const sh_json_path *swap = nodes[i]; nodes[i] = nodes[depth - 1 - i]; nodes[depth - 1 - i] = swap; }
    if (!depth || nodes[0]->key == NULL) return 1;
    if (depth == 3 && mprm_key_is(nodes[0], "entities") && !nodes[1]->key && mprm_key_is(nodes[2], "uniqueId")) {
        if (kind != SH_JSON_NUMBER) return 1;
        if (!mprm_push(scan->lists + MPRM_UIDS, offset, length, nodes[1]->index, scan->json)) { scan->failed = 1; return 0; }
        return 1;
    }
    if (!nodes[depth - 1]->key) {
        /* An element: every node before the index is a key. */
        for (size_t i = 0; i + 1 < depth; i++) if (!nodes[i]->key) return 1;
        list = mprm_list_at(nodes, depth - 1);
        if (list < 0) return 1;
        if (list == MPRM_ENTITIES ? kind != SH_JSON_OBJECT : kind != SH_JSON_NUMBER) { scan->failed = 1; return 0; }
        if (!mprm_push(scan->lists + list, offset, length, nodes[depth - 1]->index, list == MPRM_ENTITIES ? NULL : scan->json)) {
            scan->failed = 1; return 0;
        }
        return 1;
    }
    if (kind != SH_JSON_ARRAY) return 1;
    for (size_t i = 0; i < depth; i++) if (!nodes[i]->key) return 1;
    list = mprm_list_at(nodes, depth);
    if (list < 0) return 1;
    scan->lists[list].offset = offset; scan->lists[list].length = length; scan->lists[list].located = 1;
    return 1;
}

static void mprm_scan_free(mprm_scan *scan)
{ for (int i = 0; i < MPRM_LISTS; i++) free(scan->lists[i].items); memset(scan, 0, sizeof(*scan)); }

static void mprm_text_append(mprm_text *text, const char *bytes, size_t length)
{
    if (text->failed) return;
    if (text->length + length + 1 > text->capacity) {
        size_t grown = text->capacity ? text->capacity : 64;
        char *more;
        while (grown < text->length + length + 1) { if (grown > SIZE_MAX / 2) { text->failed = 1; return; } grown *= 2; }
        if (!(more = realloc(text->bytes, grown))) { text->failed = 1; return; }
        text->bytes = more; text->capacity = grown;
    }
    memcpy(text->bytes + text->length, bytes, length); text->length += length; text->bytes[text->length] = 0;
}
static void mprm_text_number(mprm_text *text, long long number, int first)
{
    char digits[32];
    int n = snprintf(digits, sizeof digits, "%s%lld", first ? "" : ",", number);
    if (n > 0) mprm_text_append(text, digits, (size_t)n);
}

/* 1 when every element of the list is an integer and the list is located. */
static int mprm_integral(const mprm_list *list)
{
    if (!list->located) return 0;
    for (size_t i = 0; i < list->count; i++) if (!list->items[i].integral || list->items[i].index != i) return 0;
    return 1;
}
static int mprm_removed_uid(const long long *uids, size_t count, long long number)
{ for (size_t i = 0; i < count; i++) if (uids[i] == number) return 1; return 0; }

/* Rebuild a prefix-sum/values multimap without the value positions marked in
 * gone: values keep their order, each key loses the removed positions below it. */
static int mprm_multimap_edit(const mprm_list *keys, const mprm_list *values, const unsigned char *gone,
    const long long *uids, size_t uid_count, int blank_removed, mprm_edit *edits, size_t *edit_count)
{
    mprm_text text = {0};
    mprm_text_append(&text, "[", 1);
    for (size_t i = 0, kept = 0; i < values->count; i++) {
        if (gone[i]) continue;
        mprm_text_number(&text, blank_removed && mprm_removed_uid(uids, uid_count, values->items[i].number) ?
            -1 : values->items[i].number, kept++ == 0);
    }
    mprm_text_append(&text, "]", 1);
    if (text.failed) { free(text.bytes); return 0; }
    mprm_edit_add(edits, edit_count, values->offset, values->length, &text);
    mprm_text_append(&text, "[", 1);
    for (size_t k = 0; k < keys->count; k++) {
        long long key = keys->items[k].number, below = 0;
        for (size_t i = 0; i < values->count; i++) if (gone[i] && (long long)i < key) below++;
        mprm_text_number(&text, key - below, k == 0);
    }
    mprm_text_append(&text, "]", 1);
    if (text.failed) { free(text.bytes); return 0; }
    mprm_edit_add(edits, edit_count, keys->offset, keys->length, &text);
    return 1;
}

static int mprm_edit_order(const void *left, const void *right)
{
    const mprm_edit *a = left, *b = right;
    return a->offset < b->offset ? 1 : a->offset > b->offset ? -1 : 0;
}

/* Remove the entities flagged in remove (by entities[] index) from text,
 * with every reference to their uniqueIds: their instance attribution, their
 * logic wires (targets.connections, source runs biased by one), their
 * reference rows (references.entityEntRefs/entityVarRefs, prefix sums keyed
 * by uniqueId) and the door/frame slots naming them. Replaces *text on
 * success. 1 done, 0 failed with why (the document stays). */
static int mprm_remove_entities(char **text, size_t *length, const unsigned char *remove, size_t entity_count,
    size_t *wires_removed, char *why, size_t why_capacity)
{
    mprm_scan scan = {0};
    mprm_edit edits[8] = {0};
    size_t edit_count = 0, i, uid_count = 0, removed_count = 0;
    long long *uids = NULL;
    unsigned char *gone = NULL;
    mprm_text built = {0};
    char *out = NULL;
    int ok = 0;
    *wires_removed = 0;
    scan.json = *text;
    if (!sh_native_json_visit_values(*text, *length, 128, mprm_visit, &scan) || scan.failed) {
        snprintf(why, why_capacity, "the map's entity tables could not be read"); goto done;
    }
    {
        const mprm_list *entities = scan.lists + MPRM_ENTITIES;
        if (!entities->located || entities->count != entity_count) { snprintf(why, why_capacity, "the map's entities array could not be located"); goto done; }
        for (i = 0; i < entities->count; i++) if (entities->items[i].index != i) { snprintf(why, why_capacity, "the map's entities array is out of order"); goto done; }
    }
    uids = calloc(entity_count ? entity_count : 1, sizeof(*uids));
    if (!uids) { snprintf(why, why_capacity, "out of memory"); goto done; }
    for (i = 0; i < entity_count; i++) if (remove[i]) {
        const mprm_list *list = scan.lists + MPRM_UIDS;
        removed_count++;
        for (size_t u = 0; u < list->count; u++) if (list->items[u].index == i) {
            if (!list->items[u].integral) { snprintf(why, why_capacity, "entity %zu has no integral uniqueId", i); goto done; }
            uids[uid_count++] = list->items[u].number; break;
        }
    }
    if (!removed_count) { ok = 1; goto done; }
    /* Tables whose meaning is not modelled must not name a removed entity. */
    for (int list = MPRM_SOURCES; list <= MPRM_TARGETS; list++) for (i = 0; i < scan.lists[list].count; i++)
        if (scan.lists[list].items[i].integral && mprm_removed_uid(uids, uid_count, scan.lists[list].items[i].number)) {
            snprintf(why, why_capacity, "targets.%s names entity %lld", list == MPRM_SOURCES ? "entitySources" : "entityTargets",
                scan.lists[list].items[i].number); goto done;
        }
    /* The entities array without the removed objects. */
    mprm_text_append(&built, "[", 1);
    for (i = 0; i < entity_count; i++) {
        int first = 1;
        if (remove[i]) continue;
        for (size_t j = 0; j < i; j++) if (!remove[j]) { first = 0; break; }
        if (!first) mprm_text_append(&built, ",", 1);
        mprm_text_append(&built, *text + scan.lists[MPRM_ENTITIES].items[i].offset, scan.lists[MPRM_ENTITIES].items[i].length);
    }
    mprm_text_append(&built, "]", 1);
    if (built.failed) { snprintf(why, why_capacity, "out of memory"); goto done; }
    mprm_edit_add(edits, &edit_count, scan.lists[MPRM_ENTITIES].offset, scan.lists[MPRM_ENTITIES].length, &built);
    if (!uid_count) { ok = 1; goto apply; }
    /* Instance attribution. */
    if (scan.lists[MPRM_INST_VALUES].located || scan.lists[MPRM_INST_KEYS].located) {
        const mprm_list *keys = scan.lists + MPRM_INST_KEYS, *values = scan.lists + MPRM_INST_VALUES;
        if (!mprm_integral(keys) || !mprm_integral(values)) { snprintf(why, why_capacity, "instanceEntities is not a pair of integer lists"); goto done; }
        free(gone); gone = calloc(values->count ? values->count : 1, 1);
        if (!gone) { snprintf(why, why_capacity, "out of memory"); goto done; }
        for (i = 0; i < values->count; i++) gone[i] = mprm_removed_uid(uids, uid_count, values->items[i].number) != 0;
        if (!mprm_multimap_edit(keys, values, gone, uids, uid_count, 0, edits, &edit_count)) { snprintf(why, why_capacity, "out of memory"); goto done; }
    }
    /* Reference rows: one row per uniqueId, the row of a removed entity goes,
     * a slot of another entity that named it reads as none. */
    for (int table = MPRM_ENT_KEYS; table <= MPRM_VAR_KEYS; table += 2) {
        const mprm_list *keys = scan.lists + table, *values = scan.lists + table + 1;
        if (!keys->located && !values->located) continue;
        if (!mprm_integral(keys) || !mprm_integral(values)) { snprintf(why, why_capacity, "references is not a pair of integer lists"); goto done; }
        free(gone); gone = calloc(values->count ? values->count : 1, 1);
        if (!gone) { snprintf(why, why_capacity, "out of memory"); goto done; }
        for (size_t u = 0; u < uid_count; u++) {
            long long uid = uids[u], from, to;
            if (uid < 0 || (size_t)uid + 1 >= keys->count) { snprintf(why, why_capacity, "references has no row for entity %lld", uid); goto done; }
            from = keys->items[(size_t)uid].number; to = keys->items[(size_t)uid + 1].number;
            if (from < 0 || to < from || (size_t)to > values->count) { snprintf(why, why_capacity, "references row %lld is malformed", uid); goto done; }
            for (long long p = from; p < to; p++) gone[(size_t)p] = 1;
        }
        if (!mprm_multimap_edit(keys, values, gone, uids, uid_count, table == MPRM_ENT_KEYS, edits, &edit_count)) {
            snprintf(why, why_capacity, "out of memory"); goto done;
        }
    }
    /* Logic wires: a negative entry opens the run of source -v-1; the rest are targets. */
    if (scan.lists[MPRM_CONNECTIONS].located) {
        const mprm_list *list = scan.lists + MPRM_CONNECTIONS;
        size_t at = 0, written = 0;
        if (!mprm_integral(list)) { snprintf(why, why_capacity, "targets.connections is not an integer list"); goto done; }
        mprm_text_append(&built, "[", 1);
        while (at < list->count) {
            long long header = list->items[at].number;
            size_t run_end = at + 1, kept = 0;
            int source_removed;
            if (header >= 0) { snprintf(why, why_capacity, "targets.connections does not start a run"); goto done; }
            while (run_end < list->count && list->items[run_end].number >= 0) run_end++;
            source_removed = mprm_removed_uid(uids, uid_count, -header - 1);
            for (size_t t = at + 1; t < run_end; t++)
                if (!source_removed && !mprm_removed_uid(uids, uid_count, list->items[t].number)) kept++;
            *wires_removed += (run_end - at - 1) - kept;
            if (kept) {
                mprm_text_number(&built, header, written++ == 0);
                for (size_t t = at + 1; t < run_end; t++)
                    if (!mprm_removed_uid(uids, uid_count, list->items[t].number)) mprm_text_number(&built, list->items[t].number, 0);
            }
            at = run_end;
        }
        mprm_text_append(&built, "]", 1);
        if (built.failed) { snprintf(why, why_capacity, "out of memory"); goto done; }
        mprm_edit_add(edits, &edit_count, list->offset, list->length, &built);
    }
    /* Door and frame slots. */
    for (int table = MPRM_DOORS; table <= MPRM_FRAMES; table++) {
        const mprm_list *list = scan.lists + table;
        int touched = 0;
        if (!list->located) continue;
        if (!mprm_integral(list)) { snprintf(why, why_capacity, "doorsAndCaps is not an integer list"); goto done; }
        for (i = 0; i < list->count; i++) touched |= mprm_removed_uid(uids, uid_count, list->items[i].number);
        if (!touched) continue;
        mprm_text_append(&built, "[", 1);
        for (i = 0; i < list->count; i++)
            mprm_text_number(&built, mprm_removed_uid(uids, uid_count, list->items[i].number) ? -1 : list->items[i].number, i == 0);
        mprm_text_append(&built, "]", 1);
        if (built.failed) { snprintf(why, why_capacity, "out of memory"); goto done; }
        mprm_edit_add(edits, &edit_count, list->offset, list->length, &built);
    }
    ok = 1;
apply:
    if (ok) {
        size_t total = *length, w = 0, at = 0;
        qsort(edits, edit_count, sizeof(*edits), mprm_edit_order);
        for (i = 0; i < edit_count; i++) {
            if (i + 1 < edit_count && edits[i + 1].offset + edits[i + 1].length > edits[i].offset) { ok = 0; snprintf(why, why_capacity, "overlapping edits"); break; }
            total = total - edits[i].length + edits[i].text_length;
        }
        if (!ok) goto done;
        out = HeapAlloc(GetProcessHeap(), 0, total + 1);
        if (!out) { ok = 0; snprintf(why, why_capacity, "out of memory"); goto done; }
        for (i = edit_count; i-- > 0;) {   /* ascending offsets */
            memcpy(out + w, *text + at, edits[i].offset - at); w += edits[i].offset - at;
            memcpy(out + w, edits[i].text, edits[i].text_length); w += edits[i].text_length;
            at = edits[i].offset + edits[i].length;
        }
        memcpy(out + w, *text + at, *length - at); w += *length - at; out[w] = 0;
        HeapFree(GetProcessHeap(), 0, *text);
        *text = out; *length = w; out = NULL;
    }
done:
    for (i = 0; i < edit_count; i++) free(edits[i].text);
    free(built.bytes); free(gone); free(uids);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    mprm_scan_free(&scan);
    return ok;
}

/* The entity index of a document-local owner token, or SIZE_MAX. */
static size_t mpref_owner_entity(const char *owner)
{
    char *end = NULL;
    unsigned long long n;
    if (!owner || strncmp(owner, "entity/", 7) || owner[7] < '0' || owner[7] > '9') return SIZE_MAX;
    n = _strtoui64(owner + 7, &end, 10);
    return end && !*end && n < SIZE_MAX ? (size_t)n : SIZE_MAX;
}

typedef struct mpref_absent_filter {
    const sh_package_provenance *source;
    const unsigned char (*trees)[32]; size_t missing;   /* the trees the player lacks */
    const unsigned char *absent;    /* per use: a context names a missing tree */
    const unsigned char *removed;   /* per entity index: left out */
    size_t entity_count;
    char owner[32];
} mpref_absent_filter;

static int mpref_context_absent(const mpref_absent_filter *filter, const size_t *contexts, size_t count)
{
    for (size_t c = 0; c < count; c++) {
        const sh_package_provenance_context *scope;
        if (contexts[c] >= filter->source->context_count) return -1;
        scope = filter->source->contexts + contexts[c];
        for (size_t t = 0; t < scope->tree_count; t++) for (size_t i = 0; i < filter->missing; i++)
            if (!memcmp(scope->trees[t], filter->trees[i], 32)) return 1;
    }
    return 0;
}

static int mpref_absent_keep(void *context, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{
    mpref_absent_filter *filter = context;
    size_t use_index, entity, below = 0;
    if (use->kind == SH_PACKAGE_USE_HISTORICAL) {
        int absent = mpref_context_absent(filter, use->contexts, use->context_count);
        return absent < 0 ? -1 : !absent;
    }
    use_index = (size_t)(use - filter->source->uses);
    if (use_index >= filter->source->use_count) return -1;
    if (filter->absent[use_index]) return 0;
    entity = mpref_owner_entity(use->owner);
    if (entity == SIZE_MAX || !filter->removed) return 1;
    if (entity >= filter->entity_count) return -1;
    if (filter->removed[entity]) return 0;
    for (size_t i = 0; i < entity; i++) if (filter->removed[i]) below++;
    if (!below) return 1;
    snprintf(filter->owner, sizeof filter->owner, "entity/%zu", entity - below);
    site->owner = filter->owner;
    return 1;
}

/* Replace every JSON string literal "<from>" with "<to>"; NULL when none. */
static char *mpref_replace_quoted(const char *json, size_t length, const char *from, const char *to, size_t *out_length)
{
    size_t from_length = strlen(from), to_length = strlen(to), hits = 0, w = 0;
    const char *at = json, *end = json + length, *hit;
    char *out;
    while ((hit = sh_shard_find(at, (size_t)(end - at), from, from_length)) != NULL) {
        if (hit > json && hit[-1] == '"' && hit + from_length < end && hit[from_length] == '"') hits++;
        at = hit + from_length;
    }
    if (!hits) return NULL;
    if (to_length > from_length && hits > (SIZE_MAX - length) / (to_length - from_length)) return NULL;
    out = HeapAlloc(GetProcessHeap(), 0, length + hits * (to_length > from_length ? to_length - from_length : 0) + 1);
    if (!out) return NULL;
    at = json;
    while ((hit = sh_shard_find(at, (size_t)(end - at), from, from_length)) != NULL) {
        memcpy(out + w, at, (size_t)(hit - at)); w += (size_t)(hit - at);
        if (hit > json && hit[-1] == '"' && hit + from_length < end && hit[from_length] == '"') { memcpy(out + w, to, to_length); w += to_length; }
        else { memcpy(out + w, hit, from_length); w += from_length; }
        at = hit + from_length;
    }
    memcpy(out + w, at, (size_t)(end - at)); w += (size_t)(end - at); out[w] = 0;
    *out_length = w;
    return out;
}

/* The entities array length of a document (SIZE_MAX when unreadable). */
static int mpref_count_entities_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    size_t *count = context;
    (void)json; (void)length; (void)kind;
    if (path && path->parent && !path->parent->parent && !path->key && path->parent->key &&
        path->parent->key_length == 8 && !memcmp(path->parent->key, "entities", 8)) (*count)++;
    return 1;
}
static size_t mpref_count_entities(const char *json, size_t length)
{
    size_t count = 0;
    if (!sh_native_json_visit_values(json, length, 128, mpref_count_entities_visit, &count)) return SIZE_MAX;
    return count;
}

/* The name and id of the reference whose tree is tree, for a log line. */
static void mpref_tree_label(const sh_mpkg_reference *refs, size_t count, const unsigned char tree[32], char *out, size_t cap)
{
    for (size_t i = 0; i < count; i++) if (refs[i].tree_known && !memcmp(refs[i].tree, tree, 32)) {
        snprintf(out, cap, "'%s' (%s)", refs[i].name, refs[i].id); return;
    }
    snprintf(out, cap, "a referenced package");
}

int sh_mpkg_degrade_absent_references(char **json, size_t *length, sh_mpkg_stock_fn stock, void *stock_context)
{
    unsigned char trees[32][32];
    sh_package_provenance record = {0}, kept = {0};
    sh_mpkg_reference *refs = NULL;
    mpref_absent_filter filter = {&record, (const unsigned char (*)[32])trees, 0, NULL, NULL, 0, ""};
    unsigned char *absent = NULL, *dropped = NULL, *removed = NULL;
    signed char *resolvable = NULL;
    char error[512] = "", line[640], *text, *next;
    size_t missing, ref_count = 0, next_length = 0, dropped_uses = 0, renamed = 0, text_length, entity_count = SIZE_MAX;
    size_t removed_count = 0, wires = 0;
    int present = 0, ok = 0;
    if (!json || !*json || !length) return 0;
    text = *json; text_length = *length;
    missing = sh_mpkg_reference_missing(*json, *length, trees, 32);
    if (!missing) return 1;
    if (missing == SIZE_MAX) { backend_log("MPKG: the map's package references are malformed"); return 0; }
    if (missing > 32) missing = 32;
    filter.missing = missing;
    if (!sh_map_provenance_read(*json, *length, &record, &present, error, sizeof error)) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: the map's binding record could not be read before opening without a referenced package: %s", error);
        backend_log(line); return 0;
    }
    if (!present) { sh_package_provenance_free(&record); return 1; }
    ref_count = sh_mpkg_reference_scan(*json, *length, NULL, 0);
    if (ref_count && ref_count != SIZE_MAX && ref_count <= SIZE_MAX / sizeof(*refs) && (refs = calloc(ref_count, sizeof(*refs))) != NULL &&
        sh_mpkg_reference_scan(*json, *length, refs, ref_count) != ref_count) { free(refs); refs = NULL; ref_count = 0; }
    if (!refs) ref_count = 0;
    absent = calloc(record.use_count + 1u, 1); dropped = calloc(record.binding_count + 1u, 1);
    resolvable = calloc(record.binding_count + 1u, 1);
    if (!absent || !dropped || !resolvable) goto done;
    /* 1. The uses whose contexts name a tree the player lacks. */
    for (size_t u = 0; u < record.use_count; u++) {
        const sh_package_provenance_use *use = record.uses + u;
        for (size_t c = 0; c < use->context_count && !absent[u]; c++) {
            const sh_package_provenance_context *scope;
            if (use->contexts[c] >= record.context_count) goto done;
            scope = record.contexts + use->contexts[c];
            for (size_t t = 0; t < scope->tree_count && !absent[u]; t++) for (size_t i = 0; i < missing; i++)
                if (!memcmp(scope->trees[t], trees[i], 32)) { absent[u] = 1; break; }
        }
        if (absent[u] && use->binding < record.binding_count) dropped[use->binding] = 1;
    }
    if (record.semantics == SH_PACKAGE_PROVENANCE_HISTORICAL) for (size_t b = 0; b < record.binding_count; b++) {
        int absent_binding = mpref_context_absent(&filter, record.bindings[b].contexts, record.bindings[b].context_count);
        if (absent_binding < 0) goto done;
        dropped[b] = absent_binding != 0;
    }
    /* 2. Whether the authored identity behind a dropped binding still has a
     * supplier here: the stock game, or another present context. A shared
     * registration key is not a declaration and always resolves. */
    for (size_t b = 0; b < record.binding_count; b++) {
        const sh_package_provenance_binding *binding = record.bindings + b;
        int found = 0;
        if (!dropped[b]) continue;
        if (!binding->family || !binding->original || !binding->original[0] || !strcmp(binding->family, SH_PACKAGE_KEY_FAMILY)) { resolvable[b] = 1; continue; }
        for (size_t u = 0; u < record.use_count && !found; u++) {
            const sh_package_provenance_binding *other;
            if (absent[u] || record.uses[u].binding >= record.binding_count) continue;
            other = record.bindings + record.uses[u].binding;
            found = other->family && other->original && !strcmp(other->family, binding->family) && !strcmp(other->original, binding->original);
        }
        if (!found) {
            int status = stock ? stock(stock_context, binding->family, binding->original) : -1;
            if (status < 0) {
                _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: cannot tell whether the game ships %s %s; the map is not opened without its package",
                    binding->family, binding->original);
                backend_log(line); goto done;
            }
            found = status > 0;
        }
        resolvable[b] = found ? 1 : 0;
    }
    /* 3. An entity whose use names an identity nobody here supplies leaves the
     * document; such a use outside an entity cannot be removed. */
    for (size_t u = 0; u < record.use_count; u++) {
        const sh_package_provenance_use *use = record.uses + u;
        size_t entity;
        char label[SH_PACKAGE_DISPLAY_CAP + SH_PACKAGE_ID_CAP + 8];
        if (!absent[u] || use->binding >= record.binding_count || resolvable[use->binding]) continue;
        entity = mpref_owner_entity(use->owner);
        if (entity_count == SIZE_MAX) {
            entity_count = mpref_count_entities(text, text_length);
            if (entity_count == SIZE_MAX) { backend_log("MPKG: the map's entities could not be counted; the map is not opened without its package"); goto done; }
            removed = calloc(entity_count + 1u, 1);
            if (!removed) goto done;
        }
        label[0] = 0;
        for (size_t c = 0; c < use->context_count && !label[0]; c++) {
            const sh_package_provenance_context *scope = record.contexts + use->contexts[c];
            for (size_t t = 0; t < scope->tree_count && !label[0]; t++) for (size_t i = 0; i < missing; i++)
                if (!memcmp(scope->trees[t], trees[i], 32)) { mpref_tree_label(refs, ref_count, trees[i], label, sizeof label); break; }
        }
        if (entity == SIZE_MAX || entity >= entity_count) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: %s %s, which only %s supplies, is used by the map itself (%s %s) and cannot be left out; "
                "the map is not opened without that package", record.bindings[use->binding].family, record.bindings[use->binding].original,
                label, use->owner ? use->owner : "", use->reference ? use->reference : "");
            backend_log(line); goto done;
        }
        if (!removed[entity]) {
            removed[entity] = 1; removed_count++;
            _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: left out entity %zu (%s %s), which only %s supplies",
                entity, record.bindings[use->binding].family, record.bindings[use->binding].original, label);
            backend_log(line);
        }
    }
    /* 4. A saved name private to the compile that made it goes back to its authored name. */
    for (size_t b = 0; b < record.binding_count; b++) {
        const sh_package_provenance_binding *binding = record.bindings + b;
        size_t replaced_length = 0;
        char *replaced;
        if (!dropped[b] || !binding->original || !binding->original[0] || !binding->name || !binding->name[0] ||
            !strcmp(binding->original, binding->name)) continue;
        replaced = mpref_replace_quoted(text, text_length, binding->name, binding->original, &replaced_length);
        if (!replaced) continue;
        if (text != *json) HeapFree(GetProcessHeap(), 0, text);
        text = replaced; text_length = replaced_length; renamed++;
    }
    /* 5. The entities leave, with every reference to them. */
    if (removed_count) {
        char why[256] = "";
        if (text == *json) {
            char *copy = HeapAlloc(GetProcessHeap(), 0, text_length + 1);
            if (!copy) goto done;
            memcpy(copy, text, text_length); copy[text_length] = 0; text = copy;
        }
        if (!mprm_remove_entities(&text, &text_length, removed, entity_count, &wires, why, sizeof why)) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: the entities that need the missing package could not be left out (%s); "
                "the map is not opened without it", why);
            backend_log(line); goto done;
        }
    }
    /* 6. The record keeps the uses of what remains, renumbered. */
    filter.absent = absent; filter.removed = removed; filter.entity_count = entity_count;
    if (!sh_package_provenance_select_uses(&record, mpref_absent_keep, &filter, &kept)) goto done;
    dropped_uses = record.use_count - kept.use_count;
    next = sh_map_provenance_write(text, text_length, &kept, &next_length, error, sizeof error);
    if (!next) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: the map's binding record could not be rewritten for opening without a referenced package: %s", error);
        backend_log(line); goto done;
    }
    if (text != *json) HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, *json);
    *json = next; *length = next_length; text = next;
    _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: opening without %zu referenced package(s) the player lacks: %zu use(s) left "
                "the binding record, %zu private name(s) went back to their authored names, %zu entit%s and %zu logic wire(s) were left out",
                missing, dropped_uses, renamed, removed_count, removed_count == 1 ? "y" : "ies", wires);
    backend_log(line);
    ok = 1;
done:
    if (text != *json) HeapFree(GetProcessHeap(), 0, text);
    free(absent); free(dropped); free(resolvable); free(removed); free(refs);
    sh_package_provenance_free(&record); sh_package_provenance_free(&kept);
    return ok;
}

int sh_mpkg_request_reference_consent(const char *json, size_t len,
    sh_mpkg_install_completion completion, void *context, char *error, size_t capacity)
{
    sh_mpkg_reference *refs = NULL;
    mpref_pending *pending = NULL;
    size_t count, i, asked = 0;
    int result = -1, mode;
    if (error && capacity) error[0] = 0;
    if (!json || !len || !sh_shard_find(json, len, SH_MPREF_MAGIC, MPREF_MAGIC_LEN)) return 1;
    count = sh_mpkg_reference_scan(json, len, NULL, 0);
    if (count == SIZE_MAX || count > SIZE_MAX / sizeof(*refs) ||
        (count && !(refs = (sh_mpkg_reference *)calloc(count, sizeof(*refs)))) ||
        (count && sh_mpkg_reference_scan(json, len, refs, count) != count)) {
        mpkg_err(error, capacity, "The map's package references are malformed."); goto done;
    }
    if (!count) { result = 1; goto done; }
    for (i = 0; i < count; i++) {
        char line[SH_PACKAGE_DISPLAY_CAP + SH_PACKAGE_ID_CAP + 128];
        mpkg_session_entry *e;
        int installed, opened, busy;
        mpkg_lock();
        installed = refs[i].tree_known && mpkg_installed_kind(refs[i].tree) != 0;
        e = mpkg_session_find(refs[i].key, refs[i].digest);
        opened = e && e->outcome == MPREF_OPENED;
        busy = e && e->outcome == 2;
        mpkg_unlock();
        if (busy) { mpkg_err(error, capacity, "The package reference notice is already waiting for an answer."); goto done; }
        if (installed || opened) {
            _snprintf_s(line, sizeof line, _TRUNCATE, installed ?
                "MPKG: referenced package '%s' (%s) is installed locally; continuing silently" :
                "MPKG: referenced package '%s' (%s) was already opened without this session; continuing",
                refs[i].name, refs[i].id);
            backend_log(line);
            continue;
        }
        refs[asked++] = refs[i];
    }
    if (!asked) { result = 1; goto done; }
    if (InterlockedCompareExchange(&g_reference_state, MPKG_CONSENT_RAISE, MPKG_CONSENT_IDLE) != MPKG_CONSENT_IDLE) {
        mpkg_err(error, capacity, "A package notice is already waiting for an answer."); goto done;
    }
    pending = (mpref_pending *)calloc(1, sizeof(*pending));
    if (!pending) {
        InterlockedExchange(&g_reference_state, MPKG_CONSENT_IDLE);
        mpkg_err(error, capacity, "Cannot retain the map's package references."); goto done;
    }
    pending->items = refs; refs = NULL; pending->count = asked;
    pending->completion = completion; pending->completion_context = context;
    mpkg_lock();
    for (i = 0; i < asked; i++) {
        mpkg_session_entry *e = mpkg_session_find(pending->items[i].key, pending->items[i].digest);
        if (!e) e = mpkg_session_add(pending->items[i].key, pending->items[i].digest, 3);
        if (e) e->outcome = 2;
    }
    mode = g_consent_mode;
    mpkg_unlock();
    {
        char line[SH_PACKAGE_DISPLAY_CAP + SH_PACKAGE_ID_CAP + 160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MPKG: the map references package '%s' (%s), which is not installed and not carried; asking before "
            "opening without it", pending->items[0].name, pending->items[0].id);
        backend_log(line);
    }
    g_reference_pending = pending; pending = NULL;
    InterlockedExchange(&g_reference_waited, 0);
    result = 0;
    /* Tests decide synchronously, as installation consent does. */
    if (mode == 0 || mode == 1) mpref_finish(mode);
done:
    free(refs); mpref_pending_free(pending);
    return result;
}

int sh_mpkg_gate(const char *json, size_t len)
{
    sh_mpkg_decl *decls = NULL;
    int *installed = NULL, result = 1;
    size_t count;
    if (!json || !len) return 1;
    if (sh_shard_find(json, len, MPKG_HEADER_MAGIC, MPKG_MAGIC_LEN)) {
        result = 0;
        count = mpkg_scan_internal(json, len, &decls);
        if (count && count != SIZE_MAX && sh_package_runtime_serving_disabled()) {
            /* Nothing is served this launch (sh_user_overrides 0): a map that
             * declares packages is refused as such and never offered an install. */
            static const char reason[] =
                "this map uses packages, and package serving is disabled for this launch (sh_user_overrides 0)";
            mpkg_set_refusal(reason);
            sh_mpkg_report_error(SH_MPKG_PROBLEM_MODS_DISABLED, reason);
        } else if (count == SIZE_MAX || count > SIZE_MAX / sizeof(*installed) ||
            (count && !(installed = (int *)calloc(count, sizeof(*installed))))) {
            mpkg_set_refusal("cannot allocate the complete map package inventory");
        } else result = !count || mpkg_gate_declared(json, len, decls, count, installed);
        free(installed); free(decls);
    }
    /* References never refuse a map on their own: one that names only
     * installed packages passes, and an unmatched one is asked about. */
    if (result && sh_shard_find(json, len, SH_MPREF_MAGIC, MPREF_MAGIC_LEN)) {
        char error[SH_MPKG_ERR_CAP] = "";
        int admitted = sh_mpkg_request_reference_consent(json, len, NULL, NULL, error, sizeof error);
        if (admitted < 0) { mpkg_set_refusal(error[0] ? error : "the map's package references are malformed"); result = 0; }
        else if (!admitted) {
            mpkg_set_refusal("map references a package that is not installed; the player is asked before "
                             "the map opens without it, and a later load then passes");
            result = 0;
        }
    }
    return result;
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
    sh_mpkg_shared_trees_release();
    if (g_consent_ticket) sh_engine_dialog_release(g_consent_ticket);
    if (g_notice_ticket) sh_engine_dialog_release(g_notice_ticket);
    if (g_reference_ticket) sh_engine_dialog_release(g_reference_ticket);
    mpkg_staged_free(g_consent_staged); g_consent_staged = NULL;
    mpref_pending_free(g_reference_pending); g_reference_pending = NULL;
    g_consent_ticket = g_notice_ticket = g_reference_ticket = 0;
    g_consent_state = g_consent_waited = 0;
    g_reference_state = g_reference_waited = 0;
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
