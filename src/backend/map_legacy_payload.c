/* Removal of retired first-generation package payloads from map JSON. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "map_legacy_payload.h"
#include "map_shards.h"
#include "backend_log.h"

#define LEGACY_MAGIC     "smpkg."
#define LEGACY_MAGIC_LEN 6
/* The retired writer split payloads into 8192-character shards of at most
 * INT_MAX bytes; any map carries far fewer variables than this bound. */
#define LEGACY_MAX_CUTS  (1u << 20)

/* Read one dot-terminated decimal field backwards from `end`. */
static int legacy_digits_back(const char *start, const char **end)
{
    const char *p = *end;
    size_t digits = 0;
    while (p > start && sh_shard_is_digit(p[-1]) && digits < 8) { p--; digits++; }
    if (!digits || digits >= 8 || p <= start || p[-1] != '.') return 0;
    *end = p - 1;
    return 1;
}

int sh_map_legacy_payload_header(const char *header, size_t length)
{
    const char *start, *end;
    size_t i;
    if (!header || length <= LEGACY_MAGIC_LEN + SH_SHARD_DIGEST_CHARS + 5 ||
        memcmp(header, LEGACY_MAGIC, LEGACY_MAGIC_LEN)) return 0;
    start = header + LEGACY_MAGIC_LEN;
    end = header + length;
    for (i = 0; i < SH_SHARD_DIGEST_CHARS; i++) {
        char c = end[-1 - (ptrdiff_t)i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    end -= SH_SHARD_DIGEST_CHARS;
    if (end <= start || end[-1] != '.') return 0;
    end--;                                           /* before ".<digest>" */
    if (!legacy_digits_back(start, &end)) return 0;  /* total */
    if (!legacy_digits_back(start, &end)) return 0;  /* index */
    return end > start;                              /* a non-empty package id */
}

static int legacy_filter(const char *header, size_t length, void *context)
{
    (void)context;
    return sh_map_legacy_payload_header(header, length);
}

char *sh_map_legacy_payload_strip(const char *json, size_t length, size_t *out_length)
{
    unsigned elements = 0, runs = 0;
    int doc_failed = 0;
    size_t written = 0;
    char *out, line[256];
    if (out_length) *out_length = 0;
    if (!json || !length || !sh_shard_find(json, length, LEGACY_MAGIC, LEGACY_MAGIC_LEN)) return NULL;
    out = sh_shard_strip(json, length, LEGACY_MAGIC, LEGACY_MAGIC_LEN, legacy_filter, NULL,
                         LEGACY_MAX_CUTS, &written, &elements, &runs, &doc_failed);
    if (!out) {
        if (doc_failed)
            backend_log("MAP: a retired package payload could not be removed (the map's JSON did not "
                        "read cleanly); the map loads with it as inert string variables");
        return NULL;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MAP: removed a retired package payload (%u string variable(s), %zu -> %zu bytes); packages "
        "are not supported in this release and the map loads without them", elements, length, written);
    backend_log(line);
    if (out_length) *out_length = written;
    return out;
}
