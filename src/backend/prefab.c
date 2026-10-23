#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prefab.h"
#include "config_json.h"
#include "map_package.h"
#include "map_provenance.h"
#include "map_shards.h"

static int pf_error(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }

static int pf_delivery(const char *json, size_t length)
{
    const char *header, *chunk;
    size_t at = 0, header_length, chunk_length;
    return sh_shard_next(json, length, "smpkg.", 6, &at,
        &header, &header_length, &chunk, &chunk_length);
}

static char *pf_native(const char *json, size_t length, int *delivery,
    size_t *output_length, char *error, size_t capacity)
{
    sh_json_kind kind;
    sh_json_error problem = {0};
    char *stripped = NULL, *out = NULL;
    size_t stripped_length = 0;
    if (!json || length > INT_MAX || !sh_native_json_validate(json, length, 128, &kind, &problem) ||
        kind != SH_JSON_OBJECT) {
        pf_error(error, capacity, "prefab JSON is unavailable or structurally invalid"); return NULL;
    }
    *delivery = pf_delivery(json, length);
    if (*delivery) {
        stripped = sh_mpkg_strip(json, length, &stripped_length);
        /* An unrecognized/malformed carrier must not become successful vanilla
         * input merely because the package header parser declined it. */
        if (!stripped || pf_delivery(stripped, stripped_length)) {
            pf_error(error, capacity, "prefab delivery cannot be removed completely"); goto done;
        }
    }
    out = sh_map_provenance_strip(stripped ? stripped : json,
        stripped ? stripped_length : length, output_length, error, capacity);
done:
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    return out;
}

void sh_prefab_carrier_free(sh_prefab_carrier *value)
{
    if (!value) return;
    if (value->native_json) HeapFree(GetProcessHeap(), 0, value->native_json);
    sh_package_provenance_free(&value->bindings); memset(value, 0, sizeof(*value));
}

int sh_prefab_carrier_read(const char *json, size_t length, sh_prefab_carrier *out,
    char *error, size_t capacity)
{
    sh_prefab_carrier next = {0};
    if (error && capacity) *error = 0;
    if (!out) return pf_error(error, capacity, "prefab carrier output is unavailable");
    next.native_json = pf_native(json, length, &next.deliveries_present,
        &next.native_length, error, capacity);
    if (!next.native_json || !sh_map_provenance_read(json, length, &next.bindings,
        &next.bindings_present, error, capacity)) {
        sh_prefab_carrier_free(&next); return 0;
    }
    sh_prefab_carrier_free(out); *out = next; return 1;
}

typedef struct pf_source {
    sh_package_bundle *bundle;
    char key[SH_PACKAGE_BUNDLE_KEY_CAP];
} pf_source;
static int pf_source_order(const void *a, const void *b)
{ return strcmp(((const pf_source *)a)->key, ((const pf_source *)b)->key); }

char *sh_prefab_carrier_write(const char *json, size_t length,
    const sh_package_occurrence_snapshot *document, size_t *output_length,
    char *error, size_t capacity)
{
    const sh_package_provenance *uses = sh_package_occurrence_provenance(document);
    const sh_package_bundle_binding *rows;
    pf_source *sources = NULL;
    sh_mpkg_delivery *items = NULL;
    size_t count = 0, current_length = 0;
    char *current = NULL, *out = NULL;
    int delivery = 0;
    if (error && capacity) *error = 0;
    if (output_length) *output_length = 0;
    if (!uses || uses->semantics != SH_PACKAGE_PROVENANCE_USES || !output_length) {
        pf_error(error, capacity, "prefab export requires completed occurrence ownership"); return NULL;
    }
    rows = sh_package_occurrence_bundles(document, &count);
    if (count > SIZE_MAX / sizeof(*sources) || (count && !(sources = calloc(count, sizeof(*sources)))))
        goto failed;
    for (size_t i = 0; i < count; i++) {
        if (!rows[i].original) goto failed;
        sources[i].bundle = rows[i].original;
        sh_package_bundle_delivery_key(rows[i].original, sources[i].key);
    }
    if (count > 1) qsort(sources, count, sizeof(*sources), pf_source_order);
    current = pf_native(json, length, &delivery, &current_length, error, capacity);
    if (!current) goto done;
    if (count) {
        /* Read every distinct delivery, then embed them as one set so equal
         * bytes between them are stored once (map_package_share.h). */
        size_t used = 0, next_length = 0;
        char *next = NULL;
        if (count > SIZE_MAX / sizeof(*items) || !(items = calloc(count, sizeof(*items)))) goto failed;
        for (size_t i = 0; i < count; i++) {
            unsigned char *payload;
            if (i && !strcmp(sources[i-1].key, sources[i].key)) continue;
            payload = sh_package_bundle_read(sources[i].bundle, &items[used].length, error, capacity);
            if (!payload) goto done;
            items[used].id = sources[i].key; items[used++].archive = payload;
        }
        next = sh_mpkg_embed_set(current, current_length, items, used, &next_length, NULL, error, capacity);
        if (!next) goto done;
        HeapFree(GetProcessHeap(), 0, current); current = next; current_length = next_length;
    }
    out = sh_map_provenance_write(current, current_length, uses, output_length, error, capacity);
    goto done;
failed:
    pf_error(error, capacity, "prefab's exact complete sources are unavailable");
done:
    if (current) HeapFree(GetProcessHeap(), 0, current);
    for (size_t i = 0; items && i < count; i++) free((void *)items[i].archive);
    free(items); free(sources); return out;
}
