#include "map_binding_sites.h"
#include "config_json.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mb_entry { sh_map_binding_span *span; size_t offset; } mb_entry;
typedef struct mb_walk { mb_entry *entries; size_t count, found; const char *json; } mb_walk;

void sh_map_binding_sites_clear(sh_map_binding_span *spans, size_t count)
{
    if (!spans) return;
    for (size_t i = 0; i < count; i++) {
        free((void *)spans[i].site.owner); free((void *)spans[i].site.reference);
        memset(&spans[i].site, 0, sizeof(spans[i].site));
    }
}
static int mb_order(const void *left, const void *right)
{
    const mb_entry *a = left, *b = right;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}
static int mb_plain(unsigned char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }
static int mb_location(const sh_json_path *path, sh_package_provenance_site *site)
{
    const sh_json_path *nodes[128]; size_t count = 0, start = 0, required = 1, at = 0;
    char owner[64]; static const char hex[] = "0123456789abcdef";
    for (; path; path = path->parent) {
        if (count == sizeof(nodes) / sizeof(nodes[0])) return 0;
        nodes[count++] = path;
    }
    for (size_t i = 0; i < count / 2; i++) {
        const sh_json_path *swap = nodes[i]; nodes[i] = nodes[count - i - 1]; nodes[count - i - 1] = swap;
    }
    if (count >= 2 && nodes[0]->key && nodes[0]->key_length == 8 &&
        !memcmp(nodes[0]->key, "entities", 8) && !nodes[1]->key) {
        site->kind = SH_PACKAGE_USE_OBJECT; start = 2;
        snprintf(owner, sizeof(owner), "entity/%zu", nodes[1]->index);
    } else { site->kind = SH_PACKAGE_USE_WORLD; memcpy(owner, "world", sizeof("world")); }
    for (size_t i = start; i < count; i++) {
        size_t extra = 3;
        if (nodes[i]->key) {
            for (size_t j = 0; j < nodes[i]->key_length; j++) {
                size_t n = mb_plain((unsigned char)nodes[i]->key[j]) ? 1 : 3;
                if (extra > SIZE_MAX - n) return 0;
                extra += n;
            }
        } else extra += 20; /* full 64-bit index, not a map-size limit */
        if (required > SIZE_MAX - extra) return 0;
        required += extra;
    }
    char *reference = malloc(required), *owned = _strdup(owner);
    if (!reference || !owned) { free(reference); free(owned); return 0; }
    for (size_t i = start; i < count; i++) {
        reference[at++] = '/'; reference[at++] = nodes[i]->key ? 'k' : 'i'; reference[at++] = ':';
        if (!nodes[i]->key) at += (size_t)snprintf(reference + at, required - at, "%zu", nodes[i]->index);
        else for (size_t j = 0; j < nodes[i]->key_length; j++) {
            unsigned char c = (unsigned char)nodes[i]->key[j];
            if (mb_plain(c)) reference[at++] = (char)c;
            else { reference[at++] = '%'; reference[at++] = hex[c >> 4]; reference[at++] = hex[c & 15]; }
        }
    }
    reference[at] = 0; site->owner = owned; site->reference = reference; return 1;
}
int sh_map_binding_site_at(const sh_json_path *path, sh_package_provenance_site *site)
{
    if (!site) return 0;
    memset(site, 0, sizeof(*site));
    return mb_location(path, site);
}
static int mb_value(void *context, const sh_json_path *path,
    const char *json, size_t length, sh_json_kind kind)
{
    mb_walk *walk = context; size_t offset = (size_t)(json - walk->json), lo = 0, hi = walk->count;
    (void)kind;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2;
        if (walk->entries[mid].offset < offset) lo = mid + 1; else hi = mid; }
    for (; lo < walk->count && walk->entries[lo].offset == offset; lo++) {
        sh_map_binding_span *span = walk->entries[lo].span;
        if (span->length != length || span->site.owner || !mb_location(path, &span->site)) return 0;
        walk->found++;
    }
    return 1;
}
int sh_map_binding_sites_locate(const char *json, size_t length,
    sh_map_binding_span *spans, size_t count, char *error, size_t capacity)
{
    mb_walk walk = {0}; int ok = 0;
    if (error && capacity) *error = 0;
    if ((!spans && count) || !json || count > SIZE_MAX / sizeof(mb_entry)) goto done;
    sh_map_binding_sites_clear(spans, count);
    walk.entries = calloc(count ? count : 1, sizeof(mb_entry)); walk.count = count; walk.json = json;
    if (!walk.entries) goto done;
    for (size_t i = 0; i < count; i++) {
        uintptr_t base = (uintptr_t)json, begin = (uintptr_t)spans[i].json;
        if (begin < base || begin - base > length || !spans[i].length || spans[i].length > length - (size_t)(begin - base)) goto done;
        walk.entries[i] = (mb_entry){spans + i, (size_t)(begin - base)};
    }
    qsort(walk.entries, count, sizeof(mb_entry), mb_order);
    ok = sh_native_json_visit_values(json, length, 128, mb_value, &walk) && walk.found == count;
done:
    free(walk.entries);
    if (!ok) {
        sh_map_binding_sites_clear(spans, count);
        if (error && capacity) snprintf(error, capacity, "typed map reference locations could not be retained");
    }
    return ok;
}
