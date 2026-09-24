#include "package_provenance.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char pp_magic[8] = {'S','M','P','B','I','N','D',0};
static const unsigned char pp_use_magic[8] = {'S','M','P','B','I','N','D',1};
typedef struct pp_buffer { unsigned char *body; size_t length, capacity; int failed; } pp_buffer;
typedef struct pp_cursor { const unsigned char *body; size_t length, at; int failed; } pp_cursor;
typedef struct pp_context { const sh_package_provenance_context *value; size_t index; } pp_context;
typedef struct pp_binding { const sh_package_provenance_binding *value; size_t index; } pp_binding;
typedef struct pp_use { const sh_package_provenance_use *value; size_t binding, *contexts; } pp_use;

static int pp_fail(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }
static int pp_tree_order(const void *a, const void *b) { return memcmp(a, b, 32); }
static int pp_index_order(const void *a, const void *b)
{ size_t x = *(const size_t *)a, y = *(const size_t *)b; return x < y ? -1 : x > y; }
static int pp_text(const char *text, int empty)
{
    if (!text || (!empty && !*text)) return 0;
    /* Native canonical identities, not display strings or prose. Component
     * paths may contain UTF-8; preserve those bytes and exact case. */
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 32 || *p == 127) return 0;
    return strlen(text) <= INT_MAX;
}
static int pp_component(const char *path)
{
    if (!pp_text(path, 1)) return 0;
    if (!*path) return 1;
    if (*path == '/' || strchr(path, '\\') || strchr(path, ':')) return 0;
    for (const char *p = path; *p;) {
        const char *end = strchr(p, '/'); size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.') ||
            (end && !end[1])) return 0;
        p += n; if (*p) p++;
    }
    return 1;
}
static int pp_context_compare(const sh_package_provenance_context *a,
    const sh_package_provenance_context *b)
{
    int cmp;
    if (a->mode != b->mode) return a->mode < b->mode ? -1 : 1;
    if (a->tree_count != b->tree_count) return a->tree_count < b->tree_count ? -1 : 1;
    cmp = memcmp(a->trees, b->trees, a->tree_count * 32);
    return cmp ? cmp : strcmp(a->component ? a->component : "", b->component ? b->component : "");
}
static int pp_context_order(const void *a, const void *b)
{ return pp_context_compare(((const pp_context *)a)->value, ((const pp_context *)b)->value); }
static int pp_binding_order(const void *a, const void *b)
{
    const sh_package_provenance_binding *x = ((const pp_binding *)a)->value, *y = ((const pp_binding *)b)->value;
    int cmp = strcmp(x->family, y->family); return cmp ? cmp : strcmp(x->name, y->name);
}
static int pp_use_order(const void *a, const void *b)
{
    const pp_use *x = a, *y = b;
    int cmp;
    if (x->value->kind != y->value->kind) return x->value->kind < y->value->kind ? -1 : 1;
    cmp = strcmp(x->value->owner, y->value->owner); if (cmp) return cmp;
    cmp = strcmp(x->value->reference, y->value->reference); if (cmp) return cmp;
    if (x->binding != y->binding) return x->binding < y->binding ? -1 : 1;
    if (x->value->context_count != y->value->context_count)
        return x->value->context_count < y->value->context_count ? -1 : 1;
    for (size_t i = 0; i < x->value->context_count; i++)
        if (x->contexts[i] != y->contexts[i])
            return x->contexts[i] < y->contexts[i] ? -1 : 1;
    return 0;
}
static void pp_context_free(sh_package_provenance_context *context)
{ free(context->trees); free(context->component); memset(context, 0, sizeof(*context)); }
static void pp_binding_free(sh_package_provenance_binding *binding)
{
    free(binding->family); free(binding->original); free(binding->name); free(binding->contexts);
    memset(binding, 0, sizeof(*binding));
}
static void pp_use_free(sh_package_provenance_use *use)
{ free(use->owner); free(use->reference); free(use->contexts); memset(use, 0, sizeof(*use)); }
void sh_package_provenance_free(sh_package_provenance *value)
{
    if (!value) return;
    for (size_t i = 0; i < value->context_count; i++) pp_context_free(value->contexts + i);
    for (size_t i = 0; i < value->binding_count; i++) pp_binding_free(value->bindings + i);
    for (size_t i = 0; i < value->use_count; i++) pp_use_free(value->uses + i);
    free(value->contexts); free(value->bindings); free(value->uses); memset(value, 0, sizeof(*value));
}

size_t sh_package_provenance_context_add(sh_package_provenance *out, unsigned mode,
    const unsigned char (*trees)[32], size_t count, const char *component)
{
    sh_package_provenance_context value = {0}, *grown;
    size_t unique = 0;
    if (!out || !count || !trees || count > SIZE_MAX / 32 ||
        out->context_count >= SIZE_MAX / sizeof(*grown) ||
        (mode != SH_PACKAGE_CONTEXT_ISOLATED && mode != SH_PACKAGE_CONTEXT_COMPOSED) ||
        (mode == SH_PACKAGE_CONTEXT_ISOLATED && (count != 1 || !pp_component(component))) ||
        (mode == SH_PACKAGE_CONTEXT_COMPOSED && component)) return SIZE_MAX;
    value.mode = mode; value.trees = malloc(count * 32);
    value.component = component ? _strdup(component) : NULL;
    if (!value.trees || (component && !value.component)) goto failed;
    memcpy(value.trees, trees, count * 32); qsort(value.trees, count, 32, pp_tree_order);
    for (size_t i = 0; i < count; i++) if (!unique || memcmp(value.trees[unique - 1], value.trees[i], 32))
        memcpy(value.trees[unique++], value.trees[i], 32);
    value.tree_count = unique;
    for (size_t i = 0; i < out->context_count; i++) if (!pp_context_compare(out->contexts + i, &value)) {
        pp_context_free(&value); return i;
    }
    grown = realloc(out->contexts, (out->context_count + 1) * sizeof(*grown));
    if (!grown) goto failed;
    out->contexts = grown; out->contexts[out->context_count] = value; return out->context_count++;
failed:
    pp_context_free(&value); return SIZE_MAX;
}

int sh_package_provenance_bind(sh_package_provenance *out, const char *family,
    const char *original, const char *name, const size_t *contexts, size_t count)
{
    sh_package_provenance_binding value = {0}, *existing = NULL, *grown;
    size_t total, unique = 0;
    if (!out || !pp_text(family, 0) || !pp_text(original, 0) || !pp_text(name, 0) || !count || !contexts ||
        out->binding_count >= SIZE_MAX / sizeof(*grown)) return 0;
    for (size_t i = 0; i < count; i++) if (contexts[i] >= out->context_count) return 0;
    for (size_t i = 0; i < out->binding_count; i++) if (!strcmp(out->bindings[i].family, family) &&
        !strcmp(out->bindings[i].name, name)) { existing = out->bindings + i; break; }
    if (existing && strcmp(existing->original, original)) return 0;
    if (existing && count > SIZE_MAX - existing->context_count) return 0;
    total = count + (existing ? existing->context_count : 0);
    if (total > SIZE_MAX / sizeof(*value.contexts)) return 0;
    value.contexts = malloc(total * sizeof(*value.contexts));
    if (!value.contexts) return 0;
    memcpy(value.contexts, contexts, count * sizeof(*contexts));
    if (existing) memcpy(value.contexts + count, existing->contexts, existing->context_count * sizeof(*contexts));
    qsort(value.contexts, total, sizeof(*value.contexts), pp_index_order);
    for (size_t i = 0; i < total; i++) if (!unique || value.contexts[unique - 1] != value.contexts[i])
        value.contexts[unique++] = value.contexts[i];
    value.context_count = unique;
    if (existing) {
        free(existing->contexts); existing->contexts = value.contexts; existing->context_count = unique; return 1;
    }
    value.family = _strdup(family); value.original = _strdup(original); value.name = _strdup(name);
    if (!value.family || !value.original || !value.name) goto failed;
    grown = realloc(out->bindings, (out->binding_count + 1) * sizeof(*grown));
    if (!grown) goto failed;
    out->bindings = grown; out->bindings[out->binding_count++] = value; return 1;
failed:
    pp_binding_free(&value); return 0;
}

static int pp_has_context(const size_t *contexts, size_t count, size_t context)
{ if (contexts) for (size_t i = 0; i < count; i++) if (contexts[i] == context) return 1; return 0; }
static int pp_same_contexts(const sh_package_provenance_use *use, const size_t *contexts, size_t count)
{
    if (use->context_count != count) return 0;
    for (size_t i = 0; i < count; i++) if (!pp_has_context(use->contexts, count, contexts[i])) return 0;
    return 1;
}
static int pp_site_valid(const sh_package_provenance_site *site)
{
    if (!site) return 0;
    if (site->kind == SH_PACKAGE_USE_HISTORICAL)
        return (!site->owner || !*site->owner) && (!site->reference || !*site->reference);
    return (site->kind == SH_PACKAGE_USE_OBJECT || site->kind == SH_PACKAGE_USE_WORLD) &&
        pp_text(site->owner, 0) && pp_text(site->reference, 0);
}
static sh_package_provenance_use *pp_find_use(sh_package_provenance *value,
    const sh_package_provenance_site *site, size_t binding, const size_t *contexts, size_t count)
{
    for (size_t i = 0; i < value->use_count; i++) {
        sh_package_provenance_use *use = value->uses + i;
        if (site->kind != use->kind) continue;
        if (site->kind == SH_PACKAGE_USE_HISTORICAL) {
            if (binding == use->binding && pp_same_contexts(use, contexts, count)) return use;
        } else if (!strcmp(site->owner, use->owner) && !strcmp(site->reference, use->reference)) return use;
    }
    return NULL;
}
int sh_package_provenance_use_set(sh_package_provenance *out,
    const sh_package_provenance_site *site, size_t binding, const size_t *contexts, size_t count)
{
    sh_package_provenance_use value = {0}, *existing, *grown;
    const sh_package_provenance_binding *b; size_t unique = 0;
    if (!out || out->semantics == SH_PACKAGE_PROVENANCE_HISTORICAL ||
        out->semantics > SH_PACKAGE_PROVENANCE_USES || !pp_site_valid(site) ||
        binding >= out->binding_count || !out->bindings || !out->contexts ||
        (out->use_count && !out->uses) || !count || !contexts || count > SIZE_MAX / sizeof(*contexts) ||
        out->use_count >= SIZE_MAX / sizeof(*grown)) return 0;
    b = out->bindings + binding;
    for (size_t i = 0; i < count; i++) if (contexts[i] >= out->context_count ||
        !pp_has_context(b->contexts, b->context_count, contexts[i])) return 0;
    value.kind = site->kind; value.binding = binding;
    value.owner = _strdup(site->owner ? site->owner : "");
    value.reference = _strdup(site->reference ? site->reference : "");
    value.contexts = malloc(count * sizeof(*contexts));
    if (!value.owner || !value.reference || !value.contexts) goto failed;
    memcpy(value.contexts, contexts, count * sizeof(*contexts));
    qsort(value.contexts, count, sizeof(*contexts), pp_index_order);
    for (size_t i = 0; i < count; i++) if (!unique || value.contexts[unique - 1] != value.contexts[i])
        value.contexts[unique++] = value.contexts[i];
    value.context_count = unique;
    existing = pp_find_use(out, site, binding, value.contexts, unique);
    if (existing) { pp_use_free(existing); *existing = value; }
    else {
        grown = realloc(out->uses, (out->use_count + 1) * sizeof(*grown));
        if (!grown) goto failed;
        out->uses = grown; out->uses[out->use_count++] = value;
    }
    out->semantics = SH_PACKAGE_PROVENANCE_USES; return 1;
failed:
    pp_use_free(&value); return 0;
}

static int pp_append_contexts(sh_package_provenance *out, const sh_package_provenance *source,
    size_t binding, const size_t *selected, size_t count, size_t *mapped, size_t *bound)
{
    if (!source->bindings || !source->contexts || !count || !selected || !mapped) return 0;
    const sh_package_provenance_binding *b = source->bindings + binding;
    for (size_t i = 0; i < count; i++) {
        if (selected[i] >= source->context_count || !pp_has_context(b->contexts, b->context_count, selected[i])) return 0;
        const sh_package_provenance_context *c = source->contexts + selected[i];
        mapped[i] = sh_package_provenance_context_add(out, c->mode, c->trees, c->tree_count, c->component);
        if (mapped[i] == SIZE_MAX) return 0;
    }
    if (!sh_package_provenance_bind(out, b->family, b->original, b->name, mapped, count)) return 0;
    if (bound) for (size_t i = 0; i < out->binding_count; i++)
        if (!strcmp(out->bindings[i].family, b->family) && !strcmp(out->bindings[i].name, b->name)) { *bound = i; break; }
    return 1;
}
static int pp_append(sh_package_provenance *out, const sh_package_provenance *source, size_t binding)
{
    const sh_package_provenance_binding *b = source->bindings + binding;
    size_t *mapped = b->context_count && b->context_count <= SIZE_MAX / sizeof(*mapped) ?
        malloc(b->context_count * sizeof(*mapped)) : NULL;
    int ok = mapped && pp_append_contexts(out, source, binding, b->contexts, b->context_count, mapped, NULL);
    free(mapped); return ok;
}
static int pp_append_use(sh_package_provenance *out, const sh_package_provenance *source,
    const sh_package_provenance_use *use, const sh_package_provenance_site *site)
{
    size_t binding = SIZE_MAX, *mapped, count = 0; int ok = 0;
    sh_package_provenance_use *existing;
    if (!pp_site_valid(site) || site->kind != use->kind || use->binding >= source->binding_count ||
        !use->context_count || !use->contexts || use->context_count > SIZE_MAX / sizeof(*mapped)) return 0;
    mapped = malloc(use->context_count * sizeof(*mapped)); if (!mapped) return 0;
    if (!pp_append_contexts(out, source, use->binding, use->contexts, use->context_count, mapped, &binding) || binding == SIZE_MAX) goto done;
    qsort(mapped, use->context_count, sizeof(*mapped), pp_index_order);
    for (size_t i = 0; i < use->context_count; i++) if (!count || mapped[count - 1] != mapped[i]) mapped[count++] = mapped[i];
    existing = pp_find_use(out, site, binding, mapped, count);
    if (existing && (existing->binding != binding || !pp_same_contexts(existing, mapped, count))) goto done;
    ok = sh_package_provenance_use_set(out, site, binding, mapped, count);
done:
    free(mapped); return ok;
}
static int pp_copy_uses(sh_package_provenance *out, const sh_package_provenance *index,
    int (*selected)(void *, const sh_package_provenance_use *, sh_package_provenance_site *), void *context)
{
    if (!index || index->semantics == SH_PACKAGE_PROVENANCE_CATALOG ||
        index->semantics > SH_PACKAGE_PROVENANCE_USES ||
        (index->semantics == SH_PACKAGE_PROVENANCE_HISTORICAL && index->use_count)) return 0;
    size_t count = index->semantics == SH_PACKAGE_PROVENANCE_HISTORICAL ? index->binding_count : index->use_count;
    if (count && !(index->semantics == SH_PACKAGE_PROVENANCE_HISTORICAL ? (void *)index->bindings : (void *)index->uses)) return 0;
    for (size_t i = 0; i < count; i++) {
        sh_package_provenance_use historical = {SH_PACKAGE_USE_HISTORICAL, "", "", i, NULL, 0};
        const sh_package_provenance_use *use;
        if (index->semantics == SH_PACKAGE_PROVENANCE_HISTORICAL) {
            historical.contexts = index->bindings[i].contexts; historical.context_count = index->bindings[i].context_count;
            use = &historical;
        } else use = index->uses + i;
        sh_package_provenance_site site = {use->kind, use->owner, use->reference};
        if (!pp_site_valid(&site)) return 0;
        int keep = selected ? selected(context, use, &site) : 1;
        if (keep < 0 || (keep && !pp_append_use(out, index, use, &site))) return 0;
    }
    return 1;
}
int sh_package_provenance_select_uses(const sh_package_provenance *index,
    int (*selected)(void *, const sh_package_provenance_use *, sh_package_provenance_site *),
    void *context, sh_package_provenance *out)
{
    if (!out || out == index) return 0;
    sh_package_provenance_free(out); out->semantics = SH_PACKAGE_PROVENANCE_USES;
    if (pp_copy_uses(out, index, selected, context)) return 1;
    sh_package_provenance_free(out); return 0;
}
static const sh_package_provenance_binding *pp_find(const sh_package_provenance *index,
    const char *family, const char *name)
{
    if (index) for (size_t i = 0; i < index->binding_count; i++)
        if (!_stricmp(index->bindings[i].family, family) && !_stricmp(index->bindings[i].name, name))
            return index->bindings + i;
    return NULL;
}
static size_t pp_binding_index(const sh_package_provenance *index, const char *family, const char *name)
{
    for (size_t i = 0; i < index->binding_count; i++)
        if (!strcmp(index->bindings[i].family, family) && !strcmp(index->bindings[i].name, name)) return i;
    return SIZE_MAX;
}
int sh_package_provenance_overlay(const sh_package_provenance *local,
    const sh_package_provenance *map, sh_package_provenance *out)
{
    if (!out || out == local || out == map) return 0;
    sh_package_provenance_free(out);
    if (local) for (size_t i = 0; i < local->binding_count; i++)
        if (!pp_find(map, local->bindings[i].family, local->bindings[i].name) && !pp_append(out, local, i)) goto failed;
    if (map) for (size_t i = 0; i < map->binding_count; i++) if (!pp_append(out, map, i)) goto failed;
    /* Provider lookup may prefer a map binding, but that never transfers the
     * source ownership of another recorded occurrence to the map's supplier. */
    if (local && local->semantics != SH_PACKAGE_PROVENANCE_CATALOG) {
        out->semantics = SH_PACKAGE_PROVENANCE_USES;
        if (!pp_copy_uses(out, local, NULL, NULL)) goto failed;
    }
    if (map && map->semantics != SH_PACKAGE_PROVENANCE_CATALOG) {
        out->semantics = SH_PACKAGE_PROVENANCE_USES;
        if (!pp_copy_uses(out, map, NULL, NULL)) goto failed;
    }
    return 1;
failed:
    sh_package_provenance_free(out); return 0;
}
typedef struct pp_selection {
    const sh_package_provenance *index;
    int (*selected)(void *, const char *, const char *);
    void *context;
} pp_selection;
static int pp_select_use(void *opaque, const sh_package_provenance_use *use, sh_package_provenance_site *site)
{
    pp_selection *selection = opaque; (void)site;
    if (use->binding >= selection->index->binding_count) return -1;
    const sh_package_provenance_binding *binding = selection->index->bindings + use->binding;
    return selection->selected(selection->context, binding->family, binding->name);
}
int sh_package_provenance_select(const sh_package_provenance *index,
    int (*selected)(void *, const char *, const char *), void *context, sh_package_provenance *out)
{
    if (!out || out == index) return 0;
    sh_package_provenance_free(out);
    if (!index || !selected) return 0;
    if (index->semantics == SH_PACKAGE_PROVENANCE_USES) {
        pp_selection selection = {index, selected, context};
        return sh_package_provenance_select_uses(index, pp_select_use, &selection, out);
    }
    if (index->semantics > SH_PACKAGE_PROVENANCE_USES || index->use_count) return 0;
    for (size_t i = 0; i < index->binding_count; i++) {
        int result = selected(context, index->bindings[i].family, index->bindings[i].name);
        if (result < 0 || (result && !pp_append(out, index, i))) { sh_package_provenance_free(out); return 0; }
    }
    out->semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
    return 1;
}
static int pp_resolve(const sh_package_provenance *saved, const sh_package_provenance_binding *source,
    const size_t *contexts, size_t count, const sh_package_provenance *current, const char **replacement)
{
    const char *target = NULL;
    if (!count || !contexts) return -1;
    for (size_t c = 0; c < count; c++) {
        if (contexts[c] >= saved->context_count || !pp_has_context(source->contexts, source->context_count, contexts[c])) return -1;
        const sh_package_provenance_context *required = saved->contexts + contexts[c];
        int found = 0;
        for (size_t i = 0; i < current->binding_count; i++) {
            const sh_package_provenance_binding *candidate = current->bindings + i;
            if (strcmp(source->family, candidate->family) || strcmp(source->original, candidate->original)) continue;
            for (size_t k = 0; k < candidate->context_count; k++) {
                if (candidate->contexts[k] >= current->context_count) return -1;
                if (pp_context_compare(required, current->contexts + candidate->contexts[k])) continue;
                if (target && strcmp(target, candidate->name)) return -1;
                found = 1; target = candidate->name;
            }
        }
        if (!found) return -1;
    }
    *replacement = target; return 1;
}
int sh_package_provenance_resolve_use(const sh_package_provenance *saved, size_t use,
    const sh_package_provenance *current, const char **replacement)
{
    if (replacement) *replacement = NULL;
    if (!saved || saved->semantics != SH_PACKAGE_PROVENANCE_USES || use >= saved->use_count ||
        !current || !replacement || saved->uses[use].binding >= saved->binding_count) return -1;
    const sh_package_provenance_use *entry = saved->uses + use;
    return pp_resolve(saved, saved->bindings + entry->binding, entry->contexts, entry->context_count, current, replacement);
}
int sh_package_provenance_resolve(const sh_package_provenance *saved,
    const sh_package_provenance *current, const char *family, const char *name, const char **replacement)
{
    if (replacement) *replacement = NULL;
    if (!saved || !current || !family || !name || !replacement) return -1;
    const sh_package_provenance_binding *source = pp_find(saved, family, name);
    if (!source) return 0;
    if (saved->semantics == SH_PACKAGE_PROVENANCE_USES) {
        const char *target = NULL;
        for (size_t i = 0; i < saved->use_count; i++) if (saved->uses[i].binding == (size_t)(source - saved->bindings)) {
            const char *resolved = NULL;
            if (sh_package_provenance_resolve_use(saved, i, current, &resolved) != 1 ||
                (target && strcmp(target, resolved))) return -1;
            target = resolved;
        }
        *replacement = target; return target ? 1 : 0;
    }
    return pp_resolve(saved, source, source->contexts, source->context_count, current, replacement);
}

int sh_package_provenance_resolve_site(const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement)
{
    const char *historical = NULL;
    if (replacement) *replacement = NULL;
    if (!saved || !current || !family || !name || !replacement) return -1;
    if (saved->semantics != SH_PACKAGE_PROVENANCE_USES)
        return sh_package_provenance_resolve(saved, current, family, name, replacement);
    if (site && (!site->owner || !site->reference)) return -1;
    for (size_t i = 0; i < saved->use_count; i++) {
        const sh_package_provenance_use *use = saved->uses + i;
        if (use->binding >= saved->binding_count) return -1;
        const sh_package_provenance_binding *binding = saved->bindings + use->binding;
        int same = !_stricmp(binding->family, family) && !_stricmp(binding->name, name);
        if (use->kind != SH_PACKAGE_USE_HISTORICAL) {
            if (site && use->kind == site->kind && !strcmp(use->owner, site->owner) &&
                !strcmp(use->reference, site->reference))
                return same ? sh_package_provenance_resolve_use(saved, i, current, replacement) : 0;
        } else if (same) {
            const char *resolved = NULL;
            if (sh_package_provenance_resolve_use(saved, i, current, &resolved) != 1 ||
                (historical && strcmp(historical, resolved))) return -1;
            historical = resolved;
        }
    }
    *replacement = historical; return historical ? 1 : 0;
}

int sh_package_provenance_scope(const sh_package_provenance *value, size_t context,
    const sh_package_sources *sources, sh_package_scope *out, char *error, size_t capacity)
{
    const sh_package_provenance_context *entry;
    size_t *matches = NULL, count;
    int ok = 0, historical = 0, modern = 0;
    if (error && capacity) error[0] = 0;
    if (!out) return 0;
    sh_package_scope_close(out);
    if (!value || context >= value->context_count) return pp_fail(error, capacity, "map source context is unavailable");
    entry = value->contexts + context;
    historical = value->semantics == SH_PACKAGE_PROVENANCE_HISTORICAL;
    if (value->semantics == SH_PACKAGE_PROVENANCE_USES) for (size_t u = 0; u < value->use_count; u++) {
        const sh_package_provenance_use *use = value->uses + u;
        for (size_t c = 0; c < use->context_count; c++) if (use->contexts[c] == context) {
            if (use->kind == SH_PACKAGE_USE_HISTORICAL) historical = 1; else modern = 1;
        }
    }
    historical &= !modern;
    if (entry->mode == SH_PACKAGE_CONTEXT_COMPOSED)
        return historical ? sh_package_scope_composed_keys_legacy(sources, entry->trees, entry->tree_count, out, error, capacity) :
            sh_package_scope_composed_keys(sources, entry->trees, entry->tree_count, out, error, capacity);
    if (entry->mode != SH_PACKAGE_CONTEXT_ISOLATED || entry->tree_count != 1)
        return pp_fail(error, capacity, "map source context is invalid");
    size_t (*find)(const sh_package_sources *, const unsigned char *, const char *, size_t *, size_t) =
        historical ? sh_package_scope_find_legacy : sh_package_scope_find;
    count = find(sources, entry->trees[0], entry->component, NULL, 0);
    if (!count || count == SIZE_MAX || count > SIZE_MAX / sizeof(*matches))
        return pp_fail(error, capacity, "the authored component for this map binding is unavailable");
    matches = malloc(count * sizeof(*matches));
    if (!matches) return pp_fail(error, capacity, "cannot retain authored component matches");
    if (find(sources, entry->trees[0], entry->component, matches, count) == count)
        ok = sh_package_scope_open(sources, matches[0], out, error, capacity);
    else pp_fail(error, capacity, "authored component inventory changed during resolution");
    free(matches); return ok;
}

static void pp_put(pp_buffer *out, const void *body, size_t length)
{
    size_t next, needed; unsigned char *grown;
    if (out->failed) return;
    if (length > INT_MAX || out->length > (size_t)INT_MAX - length) { out->failed = 1; return; }
    needed = out->length + length;
    if (needed > out->capacity) {
        next = out->capacity ? out->capacity : 256;
        while (next < needed) next = next > INT_MAX / 2 ? needed : next * 2;
        grown = realloc(out->body, next);
        if (!grown) { out->failed = 1; return; }
        out->body = grown; out->capacity = next;
    }
    if (length) memcpy(out->body + out->length, body, length);
    out->length += length;
}
static void pp_number(pp_buffer *out, size_t value)
{
    unsigned char bytes[4];
    if (value > UINT32_MAX) { out->failed = 1; return; }
    for (unsigned i = 0; i < 4; i++) bytes[i] = (unsigned char)(value >> (i * 8));
    pp_put(out, bytes, 4);
}
static void pp_string(pp_buffer *out, const char *value)
{ size_t length = value ? strlen(value) : 0; pp_number(out, length); pp_put(out, value, length); }

unsigned char *sh_package_provenance_encode(const sh_package_provenance *value,
    size_t *length, char *error, size_t capacity)
{
    pp_buffer body = {0}; pp_context *contexts = NULL; pp_binding *bindings = NULL; pp_use *uses = NULL;
    size_t *indices = NULL, *mapped = NULL, *binding_indices = NULL, *binding_mapped = NULL, use_count = 0;
    sh_package_provenance checked = {0}, selected = {0};
    if (length) *length = 0;
    if (error && capacity) error[0] = 0;
    if (!value || value->semantics > SH_PACKAGE_PROVENANCE_USES ||
        (value->semantics != SH_PACKAGE_PROVENANCE_USES && value->use_count) ||
        (value->context_count && !value->contexts) || (value->binding_count && !value->bindings) ||
        value->context_count > SIZE_MAX / sizeof(*contexts) || value->binding_count > SIZE_MAX / sizeof(*bindings)) goto failed;
    if (value->semantics == SH_PACKAGE_PROVENANCE_USES) {
        if (!sh_package_provenance_select_uses(value, NULL, NULL, &selected)) goto failed;
        value = &selected;
    }
    contexts = calloc(value->context_count ? value->context_count : 1, sizeof(*contexts));
    bindings = calloc(value->binding_count ? value->binding_count : 1, sizeof(*bindings));
    indices = calloc(value->context_count ? value->context_count : 1, sizeof(*indices));
    mapped = calloc(value->context_count ? value->context_count : 1, sizeof(*mapped));
    binding_indices = calloc(value->binding_count ? value->binding_count : 1, sizeof(*binding_indices));
    binding_mapped = calloc(value->binding_count ? value->binding_count : 1, sizeof(*binding_mapped));
    if (!contexts || !bindings || !indices || !mapped || !binding_indices || !binding_mapped) goto failed;
    /* Normalize through the same builders as decoding, retaining a mapping
     * from borrowed caller indices to the canonical source contexts. */
    for (size_t i = 0; i < value->context_count; i++) {
        const sh_package_provenance_context *entry = value->contexts + i;
        indices[i] = sh_package_provenance_context_add(&checked, entry->mode, entry->trees, entry->tree_count, entry->component);
        if (indices[i] == SIZE_MAX) goto failed;
    }
    for (size_t i = 0; i < checked.context_count; i++) contexts[i] = (pp_context){checked.contexts + i, i};
    qsort(contexts, checked.context_count, sizeof(*contexts), pp_context_order);
    for (size_t i = 0; i < checked.context_count; i++) mapped[contexts[i].index] = i;
    for (size_t i = 0; i < value->binding_count; i++) {
        const sh_package_provenance_binding *entry = value->bindings + i;
        size_t *keys;
        if (!entry->context_count || !entry->contexts || entry->context_count > SIZE_MAX / sizeof(*keys)) goto failed;
        keys = malloc(entry->context_count * sizeof(*keys));
        if (!keys) goto failed;
        for (size_t j = 0; j < entry->context_count; j++) {
            if (entry->contexts[j] >= value->context_count) { free(keys); goto failed; }
            keys[j] = indices[entry->contexts[j]];
        }
        int ok = sh_package_provenance_bind(&checked, entry->family, entry->original, entry->name, keys, entry->context_count);
        free(keys); if (!ok) goto failed;
        binding_indices[i] = pp_binding_index(&checked, entry->family, entry->name);
    }
    for (size_t i = 0; i < checked.binding_count; i++) bindings[i] = (pp_binding){checked.bindings + i, i};
    qsort(bindings, checked.binding_count, sizeof(*bindings), pp_binding_order);
    for (size_t i = 0; i < checked.binding_count; i++) binding_mapped[bindings[i].index] = i;
    if (value->semantics == SH_PACKAGE_PROVENANCE_USES) {
        for (size_t i = 0; i < value->use_count; i++) {
            const sh_package_provenance_use *entry = value->uses + i;
            sh_package_provenance_site site = {entry->kind, entry->owner, entry->reference};
            size_t *keys = malloc(entry->context_count * sizeof(*keys));
            if (!keys) goto failed;
            for (size_t j = 0; j < entry->context_count; j++) keys[j] = indices[entry->contexts[j]];
            int ok = sh_package_provenance_use_set(&checked, &site, binding_indices[entry->binding], keys, entry->context_count);
            free(keys); if (!ok) goto failed;
        }
        uses = calloc(checked.use_count ? checked.use_count : 1, sizeof(*uses));
        if (!uses) goto failed;
        use_count = checked.use_count;
        for (size_t i = 0; i < use_count; i++) {
            const sh_package_provenance_use *entry = checked.uses + i;
            uses[i] = (pp_use){entry, binding_mapped[entry->binding], malloc(entry->context_count * sizeof(size_t))};
            if (!uses[i].contexts) goto failed;
            for (size_t j = 0; j < entry->context_count; j++) uses[i].contexts[j] = mapped[entry->contexts[j]];
            qsort(uses[i].contexts, entry->context_count, sizeof(size_t), pp_index_order);
        }
        qsort(uses, use_count, sizeof(*uses), pp_use_order);
    }
    pp_put(&body, value->semantics == SH_PACKAGE_PROVENANCE_USES ? pp_use_magic : pp_magic, sizeof(pp_magic));
    pp_number(&body, checked.context_count);
    for (size_t i = 0; i < checked.context_count; i++) {
        const sh_package_provenance_context *entry = contexts[i].value;
        pp_number(&body, entry->mode); pp_number(&body, entry->tree_count);
        pp_put(&body, entry->trees, entry->tree_count * 32); pp_string(&body, entry->component);
    }
    pp_number(&body, checked.binding_count);
    for (size_t i = 0; i < checked.binding_count; i++) {
        const sh_package_provenance_binding *entry = bindings[i].value;
        pp_string(&body, entry->family); pp_string(&body, entry->original); pp_string(&body, entry->name);
        pp_number(&body, entry->context_count);
        for (size_t j = 0; j < entry->context_count; j++) indices[j] = mapped[entry->contexts[j]];
        qsort(indices, entry->context_count, sizeof(*indices), pp_index_order);
        for (size_t j = 0; j < entry->context_count; j++) pp_number(&body, indices[j]);
    }
    if (value->semantics == SH_PACKAGE_PROVENANCE_USES) {
        pp_number(&body, use_count);
        for (size_t i = 0; i < use_count; i++) {
            const sh_package_provenance_use *entry = uses[i].value;
            pp_number(&body, entry->kind); pp_string(&body, entry->owner); pp_string(&body, entry->reference);
            pp_number(&body, uses[i].binding); pp_number(&body, entry->context_count);
            for (size_t j = 0; j < entry->context_count; j++) pp_number(&body, uses[i].contexts[j]);
        }
    }
    if (body.failed) goto failed;
    for (size_t i = 0; i < use_count; i++) free(uses[i].contexts);
    free(uses); free(contexts); free(bindings); free(indices); free(mapped); free(binding_indices); free(binding_mapped);
    sh_package_provenance_free(&checked); sh_package_provenance_free(&selected);
    if (length) *length = body.length; return body.body;
failed:
    for (size_t i = 0; i < use_count; i++) free(uses[i].contexts);
    free(uses); free(contexts); free(bindings); free(indices); free(mapped); free(binding_indices); free(binding_mapped);
    sh_package_provenance_free(&checked); sh_package_provenance_free(&selected); free(body.body);
    pp_fail(error, capacity, "cannot encode complete map binding provenance"); return NULL;
}

static size_t pp_read_number(pp_cursor *in)
{
    size_t value = 0;
    if (in->failed || in->length - in->at < 4) { in->failed = 1; return 0; }
    for (unsigned i = 0; i < 4; i++) value |= (size_t)in->body[in->at++] << (i * 8);
    return value;
}
static char *pp_read_string(pp_cursor *in)
{
    size_t count = pp_read_number(in); char *out;
    if (in->failed || count > in->length - in->at || memchr(in->body + in->at, 0, count)) {
        in->failed = 1; return NULL;
    }
    out = malloc(count + 1);
    if (!out) { in->failed = 1; return NULL; }
    memcpy(out, in->body + in->at, count); out[count] = 0; in->at += count; return out;
}
int sh_package_provenance_decode(const unsigned char *body, size_t length,
    sh_package_provenance *out, char *error, size_t capacity)
{
    sh_package_provenance decoded = {0}; pp_cursor in = {body, length, sizeof(pp_magic), 0};
    size_t count, context_count = 0, binding_count = 0, *indices = NULL, *binding_indices = NULL;
    int per_use = 0;
    if (error && capacity) error[0] = 0;
    if (!out) return 0;
    sh_package_provenance_free(out);
    if (!body || length < sizeof(pp_magic) || length > INT_MAX) goto failed;
    if (!memcmp(body, pp_use_magic, sizeof(pp_magic))) per_use = 1;
    else if (memcmp(body, pp_magic, sizeof(pp_magic))) goto failed;
    context_count = pp_read_number(&in);
    /* Every context occupies at least two counts, one tree and a string count.
     * Bound allocations by remaining input before trusting serialized counts. */
    if (in.failed || context_count > (length - in.at) / 44) goto failed;
    indices = calloc(context_count ? context_count : 1, sizeof(*indices));
    if (!indices) goto failed;
    for (size_t i = 0; i < context_count; i++) {
        unsigned mode = (unsigned)pp_read_number(&in); size_t trees = pp_read_number(&in), index;
        const unsigned char (*keys)[32]; char *component;
        if (in.failed || !trees || trees > (length - in.at) / 32) goto failed;
        keys = (const unsigned char (*)[32])(body + in.at); in.at += trees * 32;
        component = pp_read_string(&in);
        if (!component || (mode == SH_PACKAGE_CONTEXT_COMPOSED && *component)) { free(component); goto failed; }
        index = sh_package_provenance_context_add(&decoded, mode, keys, trees,
            mode == SH_PACKAGE_CONTEXT_COMPOSED ? NULL : component);
        free(component); if (index == SIZE_MAX) goto failed;
        indices[i] = index;
    }
    count = pp_read_number(&in);
    if (in.failed || count > (length - in.at) / 23) goto failed;
    binding_count = count;
    binding_indices = calloc(count ? count : 1, sizeof(*binding_indices));
    if (!binding_indices) goto failed;
    for (size_t i = 0; i < count; i++) {
        char *family = pp_read_string(&in), *original = pp_read_string(&in), *name = pp_read_string(&in);
        size_t n = pp_read_number(&in), *keys = NULL; int ok = 0;
        if (!in.failed && n && n <= (length - in.at) / 4) keys = malloc(n * sizeof(*keys));
        if (keys) {
            ok = 1;
            for (size_t j = 0; j < n; j++) {
                size_t index = pp_read_number(&in);
                if (index >= context_count) { ok = 0; break; }
                keys[j] = indices[index];
            }
            if (ok) {
                ok = sh_package_provenance_bind(&decoded, family, original, name, keys, n);
                if (ok) binding_indices[i] = pp_binding_index(&decoded, family, name);
            }
        }
        free(family); free(original); free(name); free(keys); if (!ok) goto failed;
    }
    decoded.semantics = per_use ? SH_PACKAGE_PROVENANCE_USES : SH_PACKAGE_PROVENANCE_HISTORICAL;
    if (per_use) {
        count = pp_read_number(&in);
        if (in.failed || count > (length - in.at) / 24) goto failed;
        for (size_t i = 0; i < count; i++) {
            unsigned kind = (unsigned)pp_read_number(&in);
            char *owner = pp_read_string(&in), *reference = pp_read_string(&in);
            size_t binding = pp_read_number(&in), n = pp_read_number(&in), *keys = NULL, unique = 0;
            int ok = 0;
            if (!in.failed && binding < binding_count && n && n <= (length - in.at) / 4)
                keys = malloc(n * sizeof(*keys));
            if (keys) {
                ok = 1;
                for (size_t j = 0; j < n; j++) {
                    size_t index = pp_read_number(&in);
                    if (index >= context_count) { ok = 0; break; }
                    keys[j] = indices[index];
                }
                if (ok) {
                    qsort(keys, n, sizeof(*keys), pp_index_order);
                    for (size_t j = 0; j < n; j++) if (!unique || keys[unique - 1] != keys[j]) keys[unique++] = keys[j];
                    sh_package_provenance_site site = {kind, owner, reference};
                    sh_package_provenance_use *existing = pp_site_valid(&site) ?
                        pp_find_use(&decoded, &site, binding_indices[binding], keys, unique) : NULL;
                    if (existing && (existing->binding != binding_indices[binding] || !pp_same_contexts(existing, keys, unique))) ok = 0;
                    else ok = sh_package_provenance_use_set(&decoded, &site, binding_indices[binding], keys, unique);
                }
            }
            free(owner); free(reference); free(keys); if (!ok) goto failed;
        }
    }
    if (in.failed || in.at != length) goto failed;
    if (per_use) {
        sh_package_provenance selected = {0};
        if (!sh_package_provenance_select_uses(&decoded, NULL, NULL, &selected)) goto failed;
        sh_package_provenance_free(&decoded); decoded = selected;
    }
    free(indices); free(binding_indices); *out = decoded; return 1;
failed:
    free(indices); free(binding_indices); sh_package_provenance_free(&decoded);
    return pp_fail(error, capacity, "map binding provenance is incomplete or invalid");
}
