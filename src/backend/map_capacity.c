#include "map_capacity.h"

#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "map_shards.h"

/* Family prefixes, as written by map_package.c, map_provenance.c and
 * navmesh.c. Package deliveries and binding records carry
 * "<prefix><key>.<index>.<total>.<digest16>" and "<prefix><index>.<total>.<digest16>";
 * navigation shards add fields after their digest, so they are counted by
 * prefix alone. Only package and binding variables decide package-free. */
#define CAP_PACKAGE "smpkg."
#define CAP_BINDING "smpbind."
#define CAP_REFERENCE "smpref."   /* "<prefix><key>.<digest16>": a package named, not carried */
#define CAP_NAVIGATION "smnav1."

static volatile LONG64 g_limit; /* 0 selects SH_MAP_TEXT_LIMIT_DEFAULT */

void sh_map_capacity_set_limit(size_t limit)
{
    InterlockedExchange64(&g_limit, (LONG64)(limit > (size_t)LLONG_MAX ? 0 : limit));
}

size_t sh_map_capacity_limit(void)
{
    LONG64 limit = InterlockedCompareExchange64(&g_limit, 0, 0);
    return limit > 0 ? (size_t)limit : SH_MAP_TEXT_LIMIT_DEFAULT;
}

/* "<digits>.<digits>.<16 lowercase hex>" ending the header, preceded by a dot
 * or the prefix itself; `keyed` also requires a non-empty key before it. */
static int cap_tail(const char *header, size_t length, size_t prefix, int keyed)
{
    size_t end = length, digest = 0, fields = 0;
    while (end > prefix && sh_shard_is_hex(header[end - 1]) &&
           !(header[end - 1] >= 'A' && header[end - 1] <= 'F')) { end--; digest++; }
    if (digest != SH_SHARD_DIGEST_CHARS || end <= prefix || header[end - 1] != '.') return 0;
    end--;
    for (fields = 0; fields < 2; fields++) {
        size_t digits = 0;
        while (end > prefix && sh_shard_is_digit(header[end - 1])) { end--; digits++; }
        if (!digits) return 0;
        if (fields == 1 && !keyed) return end == prefix;
        if (end <= prefix || header[end - 1] != '.') return 0;
        end--;
    }
    return end > prefix; /* a non-empty key remains */
}

/* "<key>.<16 lowercase hex>" after the prefix, with a non-empty key. */
static int cap_reference(const char *header, size_t length, void *context)
{
    size_t prefix = sizeof(CAP_REFERENCE) - 1, end = length, digest = 0;
    (void)context;
    while (end > prefix && sh_shard_is_hex(header[end - 1])) { end--; digest++; }
    if (digest < SH_SHARD_DIGEST_CHARS) return 0;
    end = length - SH_SHARD_DIGEST_CHARS;
    return end > prefix + 1 && header[end - 1] == '.';
}

static int cap_package(const char *header, size_t length, void *context)
{ (void)context; return cap_tail(header, length, sizeof(CAP_PACKAGE) - 1, 1); }
static int cap_binding(const char *header, size_t length, void *context)
{ (void)context; return cap_tail(header, length, sizeof(CAP_BINDING) - 1, 0); }
static int cap_navigation(const char *header, size_t length, void *context)
{ (void)header; (void)length; (void)context; return 1; }

/* Strip one family from *text. The family's bytes are the length difference,
 * which includes list separators and the shorter variable count. *owned holds
 * the latest process-heap copy. Returns 0 when the structure did not read. */
static int cap_strip(const char **text, size_t *length, char **owned, const char *prefix,
                     sh_shard_filter_fn filter, size_t *bytes, unsigned *shards)
{
    size_t prefix_length = strlen(prefix), next_length = 0;
    unsigned elements = 0, runs = 0;
    int failed = 0;
    char *next;
    if (!sh_shard_find(*text, *length, prefix, prefix_length)) return 1;
    next = sh_shard_strip(*text, *length, prefix, prefix_length, filter, NULL, UINT_MAX,
                          &next_length, &elements, &runs, &failed);
    if (!next) return !failed;
    *bytes = *length - next_length;
    *shards = elements;
    if (*owned) HeapFree(GetProcessHeap(), 0, *owned);
    *owned = next;
    *text = next;
    *length = next_length;
    return 1;
}

int sh_map_capacity_measure(const char *text, size_t length, sh_map_capacity *out)
{
    char *owned = NULL;
    size_t bindings = 0;
    unsigned binding_shards = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->total = length;
    out->limit = sh_map_capacity_limit();
    out->parts_known = 1;
    if (!text || !length) return 1;
    if (!cap_strip(&text, &length, &owned, CAP_PACKAGE, cap_package, &out->package_bytes, &out->package_shards) ||
        !cap_strip(&text, &length, &owned, CAP_BINDING, cap_binding, &bindings, &binding_shards) ||
        !cap_strip(&text, &length, &owned, CAP_REFERENCE, cap_reference, &out->reference_bytes,
                   &out->reference_shards) ||
        !cap_strip(&text, &length, &owned, CAP_NAVIGATION, cap_navigation, &out->navigation_bytes,
                   &out->navigation_shards)) {
        out->package_bytes = out->reference_bytes = out->navigation_bytes = 0;
        out->package_shards = out->reference_shards = out->navigation_shards = 0;
        out->parts_known = 0;
    } else {
        out->package_bytes += bindings;
        out->package_shards += binding_shards;
    }
    if (owned) HeapFree(GetProcessHeap(), 0, owned);
    return 1;
}

int sh_map_capacity_within(const sh_map_capacity *capacity)
{
    return capacity && capacity->total <= capacity->limit;
}

int sh_map_capacity_carrier_free(const sh_map_capacity *capacity)
{
    return capacity && capacity->parts_known && !capacity->package_shards;
}

void sh_map_capacity_describe(const sh_map_capacity *capacity, char *line, size_t line_capacity)
{
    char parts[224], verdict[200];
    if (!line || !line_capacity) return;
    if (!capacity) { line[0] = '\0'; return; }
    if (capacity->parts_known)
        _snprintf_s(parts, sizeof(parts), _TRUNCATE, "packages %zu bytes in %u shard(s), references %zu bytes in %u, "
                    "navigation %zu bytes in %u", capacity->package_bytes, capacity->package_shards,
                    capacity->reference_bytes, capacity->reference_shards, capacity->navigation_bytes,
                    capacity->navigation_shards);
    else
        strcpy_s(parts, sizeof(parts), "parts not measured: the structure did not read");
    if (sh_map_capacity_within(capacity))
        _snprintf_s(verdict, sizeof(verdict), _TRUNCATE, "within DOOM's %zu-byte map limit%s", capacity->limit,
                    sh_map_capacity_carrier_free(capacity) ? "; carries no packages" : "");
    else
        _snprintf_s(verdict, sizeof(verdict), _TRUNCATE,
                    "%zu bytes over DOOM's %zu-byte map limit: DOOM refuses to publish it or copy it from the "
                    "service, and DOOM without Snapmap+ refuses it as a cloud-saved or published map",
                    capacity->total - capacity->limit, capacity->limit);
    _snprintf_s(line, line_capacity, _TRUNCATE, "MAP-SIZE: saved map text is %zu bytes (%s); %s",
                capacity->total, parts, verdict);
}
