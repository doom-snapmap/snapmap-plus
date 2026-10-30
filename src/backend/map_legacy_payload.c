/* Header grammar and removal of retired first-generation package payloads. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "map_legacy_payload.h"
#include "map_shards.h"

/* Every map carries far fewer variables than this bound. */
#define LEGACY_MAX_CUTS  (1u << 20)

/* Read one dot-terminated decimal field backwards from `end`. */
static int legacy_digits_back(const char *start, const char **end, unsigned *value)
{
    const char *p = *end;
    size_t digits = 0;
    unsigned long v = 0, scale = 1;
    while (p > start && sh_shard_is_digit(p[-1]) && digits < 8) { p--; digits++; }
    if (!digits || digits >= 8 || p <= start || p[-1] != '.') return 0;
    {
        const char *q;
        for (q = *end; q > p; q--) { v += (unsigned long)(q[-1] - '0') * scale; scale *= 10; }
    }
    if (value) *value = (unsigned)v;
    *end = p - 1;
    return 1;
}

static int legacy_tail_digest(const char *start, const char *end)
{
    size_t i;
    if ((size_t)(end - start) <= SH_SHARD_DIGEST_CHARS) return 0;
    for (i = 0; i < SH_SHARD_DIGEST_CHARS; i++)
        if (!sh_shard_is_hex(end[-1 - (ptrdiff_t)i])) return 0;
    return end[-1 - SH_SHARD_DIGEST_CHARS] == '.';
}

static int legacy_is_delivery(const char *key, size_t n)
{
    size_t i;
    if (n != 73 || memcmp(key, "delivery.", 9)) return 0;
    for (i = 9; i < 73; i++) if (!sh_shard_is_hex(key[i])) return 0;
    return 1;
}

int sh_map_legacy_payload_parse(const char *header, size_t length, sh_legacy_header *out)
{
    const char *start, *end;
    unsigned index = 0, total = 0;
    size_t n;
    if (!header || length <= SH_LEGACY_MAGIC_LEN + SH_SHARD_DIGEST_CHARS + 5 ||
        memcmp(header, SH_LEGACY_MAGIC, SH_LEGACY_MAGIC_LEN)) return 0;
    start = header + SH_LEGACY_MAGIC_LEN;
    end = header + length;
    if (!legacy_tail_digest(start, end)) return 0;
    end -= SH_SHARD_DIGEST_CHARS + 1;                         /* before ".<digest>" */
    if (!legacy_digits_back(start, &end, &total)) return 0;   /* total */
    if (!legacy_digits_back(start, &end, &index)) return 0;   /* index */
    n = (size_t)(end - start);
    if (n == 0 || n >= SH_LEGACY_KEY_CAP || total == 0) return 0;
    if (memchr(start, '\\', n)) return 0;
    if (out) {
        memcpy(out->key, start, n);
        out->key[n] = 0;
        out->index = index;
        out->total = total;
        memcpy(out->digest, header + length - SH_SHARD_DIGEST_CHARS, SH_SHARD_DIGEST_CHARS);
        out->digest[SH_SHARD_DIGEST_CHARS] = 0;
        out->key_form = legacy_is_delivery(start, n) ? SH_LEGACY_KEY_DELIVERY
                      : memchr(start, '.', n) ? SH_LEGACY_KEY_DESCRIPTOR : SH_LEGACY_KEY_FOLDER;
    }
    return 1;
}

int sh_map_legacy_payload_header(const char *header, size_t length)
{
    return sh_map_legacy_payload_parse(header, length, NULL);
}

const char *sh_map_legacy_key_form_name(int key_form)
{
    switch (key_form) {
    case SH_LEGACY_KEY_FOLDER:     return "folder-id key (beta.6-13)";
    case SH_LEGACY_KEY_DESCRIPTOR: return "descriptor-id key (beta.14-15)";
    case SH_LEGACY_KEY_DELIVERY:   return "delivery key (beta.16-19)";
    default:                       return "unknown key";
    }
}

int sh_map_legacy_unreleased_header(const char *header, size_t length)
{
    size_t magic;
    if (!header) return 0;
    if (length > 8 && !memcmp(header, "smpbind.", 8)) magic = 8;
    else if (length > 7 && !memcmp(header, "smpref.", 7)) magic = 7;
    else return 0;
    if (length <= magic + SH_SHARD_DIGEST_CHARS + 1) return 0;
    return legacy_tail_digest(header + magic, header + length);
}

typedef struct legacy_counts { unsigned packages, unreleased; } legacy_counts;

static int legacy_filter(const char *header, size_t length, void *context)
{
    legacy_counts *counts = (legacy_counts *)context;
    if (sh_map_legacy_payload_header(header, length)) { counts->packages++; return 1; }
    if (sh_map_legacy_unreleased_header(header, length)) { counts->unreleased++; return 1; }
    return 0;
}

char *sh_map_legacy_payload_strip_all(const char *json, size_t length, size_t *out_length,
                                      unsigned *package_rows, unsigned *unreleased_rows,
                                      int *doc_failed)
{
    legacy_counts counts = {0, 0};
    unsigned elements = 0, runs = 0;
    int failed = 0;
    size_t written = 0;
    char *out;
    if (out_length) *out_length = 0;
    if (package_rows) *package_rows = 0;
    if (unreleased_rows) *unreleased_rows = 0;
    if (doc_failed) *doc_failed = 0;
    if (!json || !length) return NULL;
    if (!sh_shard_find(json, length, SH_LEGACY_MAGIC, SH_LEGACY_MAGIC_LEN) &&
        !sh_shard_find(json, length, "smpbind.", 8) && !sh_shard_find(json, length, "smpref.", 7))
        return NULL;
    /* One structural pass for all three families: "smp" starts each. */
    out = sh_shard_strip(json, length, "smp", 3, legacy_filter, &counts, LEGACY_MAX_CUTS,
                         &written, &elements, &runs, &failed);
    if (doc_failed) *doc_failed = failed;
    if (!out) return NULL;
    if (out_length) *out_length = written;
    if (package_rows) *package_rows = counts.packages;
    if (unreleased_rows) *unreleased_rows = counts.unreleased;
    return out;
}
