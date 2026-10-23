#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "map_provenance.h"
#include "map_shards.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP_MAGIC "smpbind."
#define MP_MAGIC_LEN 8
#define MP_MAX_PAYLOAD ((size_t)(INT_MAX / 4) * 3u)
#define MP_MAX_SHARDS ((unsigned)INT_MAX / SH_SHARD_CHARS + 1u)
typedef struct mp_header { unsigned index, total; char digest[SH_SHARD_DIGEST_CHARS + 1]; } mp_header;

static int mp_fail(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }
static int mp_number(const char **at, const char *end, unsigned *out)
{
    const char *start = *at; unsigned value = 0;
    while (*at < end && sh_shard_is_digit(**at)) {
        unsigned digit = (unsigned)*(*at)++ - '0';
        if (value > (MP_MAX_SHARDS - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    if (*at == start || *at == end || *(*at)++ != '.') return 0;
    *out = value; return 1;
}
static int mp_parse(const char *body, size_t length, mp_header *out)
{
    const char *at, *end = body + length;
    if (length <= MP_MAGIC_LEN || memcmp(body, MP_MAGIC, MP_MAGIC_LEN)) return 0;
    at = body + MP_MAGIC_LEN;
    if (!mp_number(&at, end, &out->index) || !mp_number(&at, end, &out->total) ||
        !out->total || out->index >= out->total || end - at != SH_SHARD_DIGEST_CHARS) return 0;
    for (size_t i = 0; i < SH_SHARD_DIGEST_CHARS; i++) {
        if (!sh_shard_is_hex(at[i])) return 0;
        out->digest[i] = at[i];
    }
    out->digest[SH_SHARD_DIGEST_CHARS] = 0; return 1;
}

int sh_map_provenance_read(const char *json, size_t length,
    sh_package_provenance *out, int *present, char *error, size_t capacity)
{
    sh_shard_chunk *chunks = NULL; mp_header first = {0}; unsigned seen = 0;
    const char *header, *chunk; size_t at = 0, header_length, chunk_length, decoded_length = 0;
    unsigned char *decoded = NULL; char digest[SH_SHARD_DIGEST_CHARS + 1]; int reason, ok = 0;
    const char *failure = "map binding shards are incomplete or invalid";
    if (error && capacity) error[0] = 0;
    if (present) *present = 0;
    if (!out) return 0;
    sh_package_provenance_free(out);
    if (!json || length > INT_MAX) goto done;
    while (sh_shard_next(json, length, MP_MAGIC, MP_MAGIC_LEN, &at,
        &header, &header_length, &chunk, &chunk_length)) {
        mp_header parsed;
        if (!mp_parse(header, header_length, &parsed) || !chunk || !chunk_length ||
            chunk_length > SH_SHARD_MAX_CHUNK) goto done;
        if (!chunks) {
            /* At least one header byte per shard must exist in the supplied
             * map; a tiny input cannot claim an enormous chunk vector. */
            if (parsed.total > length / (MP_MAGIC_LEN + SH_SHARD_DIGEST_CHARS + 6)) goto done;
            chunks = calloc(parsed.total, sizeof(*chunks));
            if (!chunks) { failure = "cannot retain map binding shards"; goto done; }
            first = parsed;
        }
        if (parsed.total != first.total || strcmp(parsed.digest, first.digest) || chunks[parsed.index].filled) goto done;
        chunks[parsed.index] = (sh_shard_chunk){chunk, chunk_length, 1}; seen++;
    }
    if (!chunks) { ok = 1; goto done; }
    if (seen != first.total) goto done;
    decoded = sh_shard_b64_decode(chunks, first.total, MP_MAX_PAYLOAD, &decoded_length, &reason);
    if (!decoded) goto done;
    sh_shard_digest16(decoded, decoded_length, digest);
    if (strcmp(digest, first.digest)) { failure = "map binding payload checksum does not match"; goto done; }
    if (!sh_package_provenance_decode(decoded, decoded_length, out, error, capacity)) { failure = NULL; goto done; }
    if (present) *present = 1;
    ok = 1;
done:
    free(chunks); if (decoded) HeapFree(GetProcessHeap(), 0, decoded);
    if (!ok && failure) mp_fail(error, capacity, failure);
    return ok;
}

static int mp_filter(const char *header, size_t length, void *context)
{ (void)header; (void)length; (void)context; return 1; }

char *sh_map_provenance_strip(const char *json, size_t length,
    size_t *out_length, char *error, size_t capacity)
{
    char *out; int failed = 0;
    if (out_length) *out_length = 0;
    if (error && capacity) error[0] = 0;
    if (!json || length > INT_MAX) { mp_fail(error, capacity, "map binding input is unavailable"); return NULL; }
    out = sh_shard_strip(json, length, MP_MAGIC, MP_MAGIC_LEN, mp_filter, NULL, MP_MAX_SHARDS,
        out_length, NULL, NULL, &failed);
    if (failed) { mp_fail(error, capacity, "cannot remove map binding envelope completely"); return NULL; }
    if (out) return out;
    out = HeapAlloc(GetProcessHeap(), 0, length + 1);
    if (!out) { mp_fail(error, capacity, "cannot retain map outside binding envelope"); return NULL; }
    memcpy(out, json, length); out[length] = 0;
    if (out_length) *out_length = length;
    return out;
}

char *sh_map_provenance_write(const char *json, size_t length,
    const sh_package_provenance *value, size_t *out_length, char *error, size_t capacity)
{
    char *base = NULL, *encoded = NULL, *out = NULL, (*headers)[96] = NULL;
    unsigned char *payload = NULL; sh_shard_out *shards = NULL;
    size_t base_length, payload_length = 0, encoded_length, count; char digest[SH_SHARD_DIGEST_CHARS + 1];
    if (out_length) *out_length = 0;
    if (error && capacity) error[0] = 0;
    if (!value || value->semantics > SH_PACKAGE_PROVENANCE_USES ||
        (value->semantics != SH_PACKAGE_PROVENANCE_USES && value->use_count) ||
        (value->use_count && !value->binding_count)) {
        mp_fail(error, capacity, "map binding source is unavailable or invalid"); return NULL;
    }
    base = sh_map_provenance_strip(json, length, &base_length, error, capacity);
    if (!base) return NULL;
    if (!value->binding_count || (value->semantics == SH_PACKAGE_PROVENANCE_USES && !value->use_count)) {
        if (out_length) *out_length = base_length; return base;
    }
    payload = sh_package_provenance_encode(value, &payload_length, error, capacity);
    if (!payload) goto done;
    if (payload_length > MP_MAX_PAYLOAD) { mp_fail(error, capacity, "map bindings exceed native string representation"); goto done; }
    encoded_length = ((payload_length + 2) / 3) * 4;
    count = (encoded_length + SH_SHARD_CHARS - 1) / SH_SHARD_CHARS;
    encoded = malloc(encoded_length + 1); headers = calloc(count, sizeof(*headers)); shards = calloc(count, sizeof(*shards));
    if (!encoded || !headers || !shards) { mp_fail(error, capacity, "cannot retain complete map binding envelope"); goto done; }
    sh_shard_b64_encode(payload, payload_length, encoded); sh_shard_digest16(payload, payload_length, digest);
    for (size_t i = 0; i < count; i++) {
        size_t offset = i * SH_SHARD_CHARS, remaining = encoded_length - offset;
        snprintf(headers[i], sizeof(*headers), MP_MAGIC "%zu.%zu.%s", i, count, digest);
        shards[i] = (sh_shard_out){headers[i], encoded + offset, remaining < SH_SHARD_CHARS ? remaining : SH_SHARD_CHARS};
    }
    out = sh_shard_insert(base, base_length, shards, count, out_length, error, capacity);
done:
    HeapFree(GetProcessHeap(), 0, base); free(payload); free(encoded); free(headers); free(shards); return out;
}
