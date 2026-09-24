#include "package_native_registry.h"
#include "native_resource_id.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct pn_entry { uint32_t id; char *name; } pn_entry;
typedef struct pn_family {
    char *family, *type;
    pn_entry *entries;
    size_t count;
} pn_family;
typedef struct pn_shared { char *family, *name; } pn_shared;
struct sh_package_native_registry {
    sh_native_resource_registry *native;
    const sh_resource_catalog *catalog;
    pn_family *families;
    size_t family_count;
    pn_shared *shared;
    size_t shared_count;
};

static int pn_error(char *error, size_t capacity, const char *reason, const char *family)
{
    if (error && capacity) snprintf(error, capacity, "%s: %s", reason, family ? family : "");
    return SH_PACKAGE_LINK_REGISTRY_ERROR;
}
static int pn_order(const void *a, const void *b)
{
    const pn_entry *x = a, *y = b;
    return x->id != y->id ? (x->id < y->id ? -1 : 1) : strcmp(x->name, y->name);
}
static void pn_family_free(pn_family *family)
{
    for (size_t i = 0; i < family->count; i++) free(family->entries[i].name);
    free(family->entries); free(family->family); free(family->type);
    memset(family, 0, sizeof(*family));
}
void sh_package_native_registry_close(sh_package_native_registry *r)
{
    if (!r) return;
    for (size_t i = 0; i < r->family_count; i++) pn_family_free(r->families + i);
    for (size_t i = 0; i < r->shared_count; i++) {
        free(r->shared[i].family); free(r->shared[i].name);
    }
    free(r->shared); free(r->families);
    sh_native_resource_registry_close(r->native); free(r);
}
sh_package_native_registry *sh_package_native_registry_open(
    sh_native_resource_registry_source source, const sh_resource_catalog *catalog,
    char *error, size_t capacity)
{
    sh_package_native_registry *r = calloc(1, sizeof(*r));
    if (r) { r->catalog = catalog; r->native = sh_native_resource_registry_open(source, error, capacity); }
    if (!r || !r->native) { sh_package_native_registry_close(r); return NULL; }
    return r;
}
int sh_package_native_registry_share(sh_package_native_registry *r,
    const char *family, const char *name)
{
    pn_shared item = {0}, *grown;
    if (!r || !family || !*family || !name || !*name) return 0;
    for (size_t i = 0; i < r->shared_count; i++)
        if (!_stricmp(r->shared[i].family, family) && !strcmp(r->shared[i].name, name)) return 1;
    item.family = _strdup(family); item.name = _strdup(name);
    if (!item.family || !item.name || r->shared_count >= SIZE_MAX / sizeof(*grown) ||
        !(grown = realloc(r->shared, (r->shared_count + 1) * sizeof(*grown)))) {
        free(item.family); free(item.name); return 0;
    }
    r->shared = grown; r->shared[r->shared_count++] = item; return 1;
}

/* Reserve the installed identities even when they have not been materialized.
 * Companion/permutation rows collapse by identity. Only the small path/name
 * catalog is inspected; no baseline asset contents are read or hashed. */
static pn_family *pn_baseline(sh_package_native_registry *r, const char *family,
    const char *type, char *error, size_t capacity)
{
    pn_family value = {0}, *grown;
    size_t count = sh_resource_catalog_count(r->catalog), matching = 0;
    for (size_t i = 0; i < r->family_count; i++) if (!_stricmp(r->families[i].family, family)) {
        if (strcmp(r->families[i].type, type)) {
            pn_error(error, capacity, "native family spelling changed", family); return NULL;
        }
        return r->families + i;
    }
    for (size_t i = 0; i < count; i++) {
        const sh_resource_catalog_entry *entry = sh_resource_catalog_at(r->catalog, i);
        if (!entry || !entry->type || !entry->name) goto failed;
        if (!_stricmp(entry->type, family)) matching++;
    }
    if (matching > SIZE_MAX / sizeof(*value.entries)) goto failed;
    value.family = _strdup(family); value.type = _strdup(type);
    value.entries = calloc(matching ? matching : 1, sizeof(*value.entries));
    if (!value.family || !value.type || !value.entries) goto failed;
    for (size_t i = 0; i < count; i++) {
        const sh_resource_catalog_entry *entry = sh_resource_catalog_at(r->catalog, i);
        if (_stricmp(entry->type, family)) continue;
        pn_entry *row = value.entries + value.count++;
        row->name = sh_native_resource_name(entry->name);
        if (!row->name || !sh_native_resource_id(type, row->name, &row->id)) goto failed;
    }
    qsort(value.entries, value.count, sizeof(*value.entries), pn_order);
    size_t kept = 0;
    for (size_t i = 0; i < value.count; i++) {
        pn_entry *row = value.entries + i;
        if (kept && !pn_order(value.entries + kept - 1, row)) free(row->name);
        else value.entries[kept++] = *row;
    }
    value.count = kept;
    if (r->family_count >= SIZE_MAX / sizeof(*grown) ||
        !(grown = realloc(r->families, (r->family_count + 1) * sizeof(*grown)))) goto failed;
    r->families = grown; r->families[r->family_count++] = value;
    return r->families + r->family_count - 1;
failed:
    pn_family_free(&value); pn_error(error, capacity, "cannot reserve installed resource identities", family);
    return NULL;
}
static int pn_check(void *context, const char *family, const char *name,
    const unsigned char binding[32], int original_binding, uint32_t *id,
    char *error, size_t capacity)
{
    sh_package_native_registry *r = context;
    sh_native_resource_identity native = {0};
    int status, result = SH_PACKAGE_LINK_REGISTRY_ERROR, shared = 0;
    (void)binding; /* Content equality is the source linker's separate proof. */
    if (!r || !id) return pn_error(error, capacity, "native identity registry is unavailable", family);
    *id = 0;
    status = sh_native_resource_registry_identity(r->native, family, name, &native, error, capacity);
    if (status <= SH_NATIVE_RESOURCE_ID_UNREGISTERED) {
        if (!status) pn_error(error, capacity, "resource family has no registered native identity", family);
        goto done;
    }
    *id = native.id;
    if (status == SH_NATIVE_RESOURCE_ID_COLLISION) { result = SH_PACKAGE_LINK_REGISTRY_COLLISION; goto done; }
    pn_family *base = pn_baseline(r, family, native.type, error, capacity);
    if (!base) goto done;
    size_t low = 0, high = base->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (base->entries[mid].id < native.id) low = mid + 1; else high = mid;
    }
    for (size_t i = low; i < base->count && base->entries[i].id == native.id; i++) {
        if (strcmp(base->entries[i].name, native.name)) { result = SH_PACKAGE_LINK_REGISTRY_COLLISION; goto done; }
        if (!original_binding) shared = -1;
    }
    for (size_t i = 0; i < r->shared_count; i++)
        if (!_stricmp(r->shared[i].family, family) && !strcmp(r->shared[i].name, native.name)) { shared = 1; break; }
    if (!original_binding && shared == 1) result = SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED;
    else if (original_binding || (status == SH_NATIVE_RESOURCE_ID_FREE && !shared)) result = SH_PACKAGE_LINK_REGISTRY_AVAILABLE;
    else result = SH_PACKAGE_LINK_REGISTRY_COLLISION;
done:
    sh_native_resource_identity_clear(&native); return result;
}
sh_package_link_registry sh_package_native_registry_adapter(sh_package_native_registry *r)
{ sh_package_link_registry out = {r, pn_check}; return out; }
int sh_package_native_registry_validate(sh_package_native_registry *r, char *error, size_t capacity)
{ return r && sh_native_resource_registry_validate(r->native, error, capacity); }
