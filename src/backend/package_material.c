#include "package_material.h"
#include "decl_material_compose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pm_entry {
    char *path;
    sh_decl_composition_role role;
    size_t index;
    sh_package_resource_inputs inputs;
    sh_decl_source selected;
    int status, kind;
} pm_entry;
typedef struct pm_context {
    const sh_package_source_view *view;
    pm_entry *entries;
    size_t count, capacity;
    char error[1024];
} pm_context;

static int pm_same(sh_decl_source a, sh_decl_source b)
{ return a.length == b.length && (!a.length || !memcmp(a.text, b.text, a.length)); }

/* Renderparm is still a whole native declaration. Never compose its unknown
 * defaults/buffer grammar by treating it as an assignment-style declaration. */
static int pm_select(pm_context *c, pm_entry *entry)
{
    sh_package_resource_inputs *in = &entry->inputs;
    int changed = 0;
    entry->selected = in->original;
    for (size_t i = 0; i < in->count; i++) {
        if (in->original_scope && pm_same(in->original, in->contributions[i])) continue;
        if (changed && !pm_same(entry->selected, in->contributions[i])) {
            snprintf(c->error, sizeof(c->error), "%s: incompatible source schemas for material interpretation", entry->path);
            return -1;
        }
        entry->selected = in->contributions[i]; changed = 1;
    }
    return entry->selected.text ? 1 : 0;
}

static pm_entry *pm_read(pm_context *c, sh_decl_composition_role role, size_t index,
    const char *family, sh_decl_source name)
{
    char *raw, *path; size_t capacity, i;
    pm_entry *entry;
    if (!name.text || !name.length || memchr(name.text, 0, name.length) ||
        name.length > SIZE_MAX - strlen(family) - 24) goto failed;
    capacity = name.length + strlen(family) + 24;
    raw = malloc(capacity);
    if (!raw) goto failed;
    /* Names are slices, potentially longer than printf's int precision. */
    i = (size_t)snprintf(raw, capacity, "generated/decls/%s/", family);
    memcpy(raw + i, name.text, name.length); i += name.length;
    memcpy(raw + i, ".decl", 6);
    path = sh_package_engine_path(raw); free(raw);
    if (!path) goto failed;
    if (role != SH_DECL_COMPOSITION_CONTRIBUTION) index = 0;
    for (i = 0; i < c->count; i++) {
        entry = &c->entries[i];
        if (entry->role == role && entry->index == index && !strcmp(entry->path, path)) {
            free(path); return entry;
        }
    }
    if (c->count == c->capacity) {
        size_t next = c->capacity ? c->capacity * 2 : 16;
        pm_entry *grown = next > c->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(c->entries, next * sizeof(*grown)) : NULL;
        if (!grown) { free(path); goto failed; }
        c->entries = grown; c->capacity = next;
    }
    entry = &c->entries[c->count++]; memset(entry, 0, sizeof(*entry));
    entry->path = path; entry->role = role; entry->index = index; entry->kind = -1;
    entry->status = c->view->read(c->view->context, role, index, path,
        &entry->inputs, c->error, sizeof(c->error));
    /* A new schema may be supplied by a peer dependency. Only proven absence
     * of BOTH original and owner input permits this lookup. Existing schemas
     * always retain the author's original/own view, even if a peer edits them. */
    if (!entry->status && role == SH_DECL_COMPOSITION_CONTRIBUTION) {
        sh_package_resource_inputs_free(&entry->inputs);
        entry->status = c->view->read(c->view->context, SH_DECL_COMPOSITION_RESULT, 0,
            path, &entry->inputs, c->error, sizeof(c->error));
    }
    if (entry->status > 0) entry->status = pm_select(c, entry);
    if (entry->status > 0 && !strcmp(family, "renderparm") &&
        !sh_decl_material_parameter_kind(entry->selected, &entry->kind, c->error, sizeof(c->error)))
        entry->status = -1;
    return entry;
failed:
    snprintf(c->error, sizeof(c->error), "invalid material schema path or allocation failure"); return NULL;
}

static int pm_resolve(void *context, sh_decl_composition_role role, size_t index,
    const char *family, sh_decl_source name, int *kind, sh_decl_source *binding)
{
    pm_context *c = context;
    pm_entry *entry;
    *binding = (sh_decl_source){0};
    /* These are typed dependency records; their native paths and availability
     * belong to dependency admission, not renderparm schema projection. */
    if (!strcmp(family, "image") || !strcmp(family, "renderprog") || !strcmp(family, "sampler")) return 1;
    if (strcmp(family, "renderparm") && strcmp(family, "table")) return -1;
    entry = pm_read(c, role, index, family, name);
    if (!entry) return -1;
    if (entry->status > 0) { *kind = entry->kind; *binding = entry->selected; }
    return entry->status;
}

char *sh_package_material_compose(sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, const sh_package_source_view *view,
    size_t *length, char *error, size_t capacity, sh_decl_conflict *conflict)
{
    pm_context c = {0}; char *body;
    sh_decl_material_composition_schema schema = {&c, pm_resolve};
    if (length) *length = 0;
    if (!view || !view->read) {
        if (error && capacity) snprintf(error, capacity, "material composition needs verified source schemas");
        return NULL;
    }
    c.view = view;
    body = sh_decl_material_compose(baseline, sources, count, &schema, length, error, capacity, conflict);
    if (!body && c.error[0] && error && capacity) snprintf(error, capacity, "%s", c.error);
    for (size_t i = 0; i < c.count; i++) {
        free(c.entries[i].path); sh_package_resource_inputs_free(&c.entries[i].inputs);
    }
    free(c.entries); return body;
}
