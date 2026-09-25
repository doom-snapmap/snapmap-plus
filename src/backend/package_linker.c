#include "package_linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pl_file {
    const sh_compiled_resource *resource;
    const char *family, *name;
    sh_package_file_identity identity;
    size_t edge_begin, edge_count;
} pl_file;
typedef struct pl_unit {
    pl_file *files;
    pl_file *witnesses;
    const sh_package_link_witness **witness_order;
    sh_package_link_edge *edges;
    size_t edge_count, edge_capacity;
    sh_package_link_edge *implicit;
    size_t implicit_count, implicit_begin;
    char **paths;
    char **names;
} pl_unit;
typedef struct pl_work {
    const sh_package_link_unit *units;
    const sh_package_link_adapter *adapter;
    sh_package_link_output output;
    pl_unit *work;
    sh_package_link_node *nodes;
    size_t *member_offsets, *members;
    unsigned char *originals;
    size_t count, unit, file, next_edge;
    int replay, failed;
    char *error;
    size_t capacity;
} pl_work;
typedef struct pl_assignment {
    const char *family, *name;
    const unsigned char *identity;
    size_t group;
    int original;
} pl_assignment;
typedef struct pl_native_identity {
    const char *family, *name, *original_name;
    uint32_t id;
    int original, primary;
    size_t unit;
} pl_native_identity;

typedef struct pl_registry {
    size_t *names, *ids, capacity;
    const char **paths;
    size_t *path_units, *path_files;
    size_t path_capacity;
    pl_native_identity *identities;
    size_t identity_count;
} pl_registry;
typedef struct pl_input {
    const unsigned char *body;
    FILE *stream;
    uint64_t length, position;
} pl_input;

static void *pl_alloc(size_t count, size_t size)
{ return size && count <= SIZE_MAX / size ? calloc(count ? count : 1, size) : NULL; }
static int pl_fail(pl_work *w, const char *reason)
{
    w->failed = 1;
    if (!*w->error) snprintf(w->error, w->capacity, "%s", reason);
    return 0;
}
static int pl_file_order(const void *a, const void *b)
{ return strcmp(((const pl_file *)a)->resource->engine_path, ((const pl_file *)b)->resource->engine_path); }
static int pl_witness_order(const void *a, const void *b)
{
    const sh_package_link_witness *x = *(const sh_package_link_witness *const *)a;
    const sh_package_link_witness *y = *(const sh_package_link_witness *const *)b;
    int order = strcmp(x->role, y->role);
    return order ? order : strcmp(x->resource->engine_path, y->resource->engine_path);
}
static int pl_input_open_resource(pl_work *w, size_t unit, const pl_file *captured, pl_input *out);
static int pl_assignment_order(const void *a, const void *b)
{
    const pl_assignment *x = a, *y = b;
    int order = y->original - x->original;
    if (!order) order = strcmp(x->family, y->family);
    if (!order) order = memcmp(x->identity, y->identity, 32);
    return order ? order : strcmp(x->name, y->name);
}

void sh_package_link_output_free(sh_package_link_output *out)
{
    if (!out) return;
    sh_package_compilation_free(out->compiled);
    if (out->names) for (size_t i = 0; i < out->classes.count; i++) free(out->names[i]);
    for (size_t i = 0; i < out->implicit_count; i++) free(out->implicit[i].slot);
    free(out->implicit);
    free(out->names); sh_package_link_classes_free(&out->classes); memset(out, 0, sizeof(*out));
}

static void pl_clear(pl_work *w)
{
    if (w->work) for (size_t i = 0; i < w->count; i++) {
        pl_unit *u = &w->work[i];
        for (size_t j = 0; j < u->edge_count; j++) free((char *)u->edges[j].slot);
        for (size_t j = 0; j < u->implicit_count; j++) free((char *)u->implicit[j].slot);
        free(u->implicit);
        if (u->paths) for (size_t j = 0; j < w->units[i].file_count; j++) free(u->paths[j]);
        if (u->names) for (size_t j = 0; j < w->units[i].file_count; j++) free(u->names[j]);
        free(u->names); free(u->paths); free(u->edges); free(u->files); free(u->witnesses); free(u->witness_order);
    }
    free(w->work); free(w->nodes); free(w->member_offsets); free(w->members); free(w->originals);
    sh_package_link_output_free(&w->output);
}

/* One unit's capture, kept between identity passes while the unit is valid. */
typedef struct pl_reuse_unit {
    int captured, invalid;
    pl_file *files, *witnesses;
    const sh_package_link_witness **witness_order;
    sh_package_link_edge *edges, *implicit;
    size_t file_count, witness_count, edge_count, implicit_count, implicit_begin;
    uint64_t length;
    unsigned char content[32];
} pl_reuse_unit;
struct sh_package_link_reuse {
    pl_reuse_unit *units;
    size_t count;
    uint64_t *proven; /* open addressing: (left + 1) << 32 | (right + 1), 0 empty */
    size_t proven_count, proven_capacity;
};
static sh_package_link_edge *pl_edges_copy(const sh_package_link_edge *edges, size_t count)
{
    sh_package_link_edge *out = pl_alloc(count, sizeof(*out));
    for (size_t i = 0; out && i < count; i++) {
        out[i].target = edges[i].target;
        if (!(out[i].slot = _strdup(edges[i].slot))) {
            while (i) free((char *)out[--i].slot);
            free(out); out = NULL;
        }
    }
    return out;
}
static void pl_edges_free(sh_package_link_edge *edges, size_t count)
{
    for (size_t i = 0; edges && i < count; i++) free((char *)edges[i].slot);
    free(edges);
}
static void pl_reuse_unit_clear(pl_reuse_unit *u)
{
    int invalid = u->invalid;
    pl_edges_free(u->edges, u->edge_count); pl_edges_free(u->implicit, u->implicit_count);
    free(u->files); free(u->witnesses); free(u->witness_order);
    memset(u, 0, sizeof(*u)); u->invalid = invalid;
}
sh_package_link_reuse *sh_package_link_reuse_open(size_t count)
{
    sh_package_link_reuse *reuse = calloc(1, sizeof(*reuse));
    if (!reuse || count >= UINT32_MAX || !(reuse->units = pl_alloc(count, sizeof(*reuse->units)))) {
        free(reuse); return NULL;
    }
    reuse->count = count; return reuse;
}
void sh_package_link_reuse_invalidate(sh_package_link_reuse *reuse, size_t unit)
{
    if (!reuse || unit >= reuse->count) return;
    pl_reuse_unit_clear(reuse->units + unit); reuse->units[unit].invalid = 1;
}
void sh_package_link_reuse_close(sh_package_link_reuse *reuse)
{
    if (!reuse) return;
    for (size_t i = 0; i < reuse->count; i++) pl_reuse_unit_clear(reuse->units + i);
    free(reuse->units); free(reuse->proven); free(reuse);
}
static int pl_reuse_valid(const sh_package_link_reuse *reuse, size_t unit)
{ return reuse && unit < reuse->count && reuse->units[unit].captured && !reuse->units[unit].invalid; }
static uint64_t pl_reuse_pair(size_t left, size_t right)
{
    if (left > right) { size_t swap = left; left = right; right = swap; }
    return (uint64_t)(left + 1) << 32 | (uint64_t)(right + 1);
}
static size_t pl_reuse_slot(const sh_package_link_reuse *reuse, uint64_t key)
{
    uint64_t mixed = key * UINT64_C(0x9e3779b97f4a7c15);
    size_t at = (size_t)(mixed ^ (mixed >> 29)) & (reuse->proven_capacity - 1);
    while (reuse->proven[at] && reuse->proven[at] != key) at = (at + 1) & (reuse->proven_capacity - 1);
    return at;
}
/* Exact bytes of two valid captures were compared equal on an earlier pass. */
static int pl_reuse_proven(const sh_package_link_reuse *reuse, size_t left, size_t right)
{
    if (!pl_reuse_valid(reuse, left) || !pl_reuse_valid(reuse, right) || !reuse->proven_capacity) return 0;
    uint64_t key = pl_reuse_pair(left, right);
    return reuse->proven[pl_reuse_slot(reuse, key)] == key;
}
static void pl_reuse_prove(sh_package_link_reuse *reuse, size_t left, size_t right)
{
    if (!pl_reuse_valid(reuse, left) || !pl_reuse_valid(reuse, right)) return;
    if (reuse->proven_count + 1 > reuse->proven_capacity / 2) {
        size_t capacity = reuse->proven_capacity ? reuse->proven_capacity * 2 : 64;
        uint64_t *old = reuse->proven, *grown = capacity <= SIZE_MAX / sizeof(*grown) ? calloc(capacity, sizeof(*grown)) : NULL;
        size_t old_capacity = reuse->proven_capacity;
        if (!grown) return; /* Reuse is optional: the next pass compares again. */
        reuse->proven = grown; reuse->proven_capacity = capacity;
        for (size_t i = 0; i < old_capacity; i++) if (old[i]) reuse->proven[pl_reuse_slot(reuse, old[i])] = old[i];
        free(old);
    }
    uint64_t key = pl_reuse_pair(left, right);
    size_t at = pl_reuse_slot(reuse, key);
    if (!reuse->proven[at]) { reuse->proven[at] = key; reuse->proven_count++; }
}

static char *pl_slot(const char *path, const char *slot)
{
    size_t p = strlen(path), s = strlen(slot);
    char *out;
    if (p > SIZE_MAX - 32 || s > SIZE_MAX - 32 - p) return NULL;
    out = malloc(p + s + 32);
    if (out) snprintf(out, p + s + 32, "%zu:%s%s", p, path, slot);
    return out;
}

/* A file can assign one field more than once; the native reflected reader
 * resolves each assignment (loading its target) and keeps the last. Number the
 * repeats so every binding keeps a unique, replayable slot. Owns key. */
static char *pl_occurrence(const pl_unit *u, size_t begin, size_t end, char *key)
{
    size_t seen = 0, n = strlen(key);
    for (size_t i = begin; i < end; i++)
        if (!strncmp(u->edges[i].slot, key, n) && (!u->edges[i].slot[n] || u->edges[i].slot[n] == '#')) seen++;
    if (!seen) return key;
    char *out = n <= SIZE_MAX - 24 ? malloc(n + 24) : NULL;
    if (out) snprintf(out, n + 24, "%s#%zu", key, seen + 1);
    free(key); return out;
}
static int pl_edge_add(pl_work *w, char *key, size_t target)
{
    pl_unit *u = &w->work[w->unit];
    if (u->edge_count == u->edge_capacity) {
        size_t next = u->edge_capacity ? u->edge_capacity * 2 : 8;
        sh_package_link_edge *grown;
        if (next < u->edge_capacity || next > SIZE_MAX / sizeof(*grown) ||
            !(grown = realloc(u->edges, next * sizeof(*grown)))) {
            free(key); return pl_fail(w, "cannot allocate resource bindings");
        }
        u->edges = grown; u->edge_capacity = next;
    }
    u->edges[u->edge_count++] = (sh_package_link_edge){key, target};
    return 1;
}
static int pl_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_work *w = context;
    pl_unit *u = &w->work[w->unit];
    pl_file *file = &u->files[w->file];
    size_t target = SIZE_MAX;
    char *key = NULL;
    if (replacement) *replacement = NULL;
    if (w->failed) return -1;
    if (!replacement || !slot || !*slot || !family || !*family || !name || !*name ||
        !w->adapter->resolve(w->adapter->context, w->unit, family, name, &target, w->error, w->capacity) ||
        target >= w->count || strcmp(w->units[target].family, family) || strcmp(w->units[target].name, name) ||
        !(key = pl_slot(file->resource->engine_path, slot)) ||
        !(key = pl_occurrence(u, file->edge_begin, w->replay ? w->next_edge : u->edge_count, key))) {
        pl_fail(w, "resource binding did not resolve to its scoped native identity"); return -1;
    }
    if (w->replay) {
        size_t end = file->edge_begin + file->edge_count;
        if (w->next_edge >= end || u->edges[w->next_edge].target != target ||
            strcmp(u->edges[w->next_edge].slot, key)) {
            free(key); pl_fail(w, "resource bindings changed during linking"); return -1;
        }
        w->next_edge++; free(key);
        *replacement = w->output.names[w->output.classes.nodes[target]];
        return strcmp(*replacement, name) != 0;
    }
    return pl_edge_add(w, key, target) ? 0 : -1;
}

static int pl_implicit_order(const void *a, const void *b)
{
    const sh_package_link_edge *x = a, *y = b;
    int order = strcmp(x->slot, y->slot);
    return order ? order : x->target == y->target ? 0 : x->target < y->target ? -1 : 1;
}
static void pl_implicit_free(sh_package_link_edge *edges, size_t count)
{
    for (size_t i = 0; i < count; i++) free((char *)edges[i].slot);
    free(edges);
}
static int pl_implicit_read(pl_work *w)
{
    const sh_package_link_unit *source = w->units + w->unit;
    pl_unit *unit = w->work + w->unit;
    sh_package_link_edge *edges = NULL;
    size_t kept = 0;
    int ok = 0;
    if (source->implicit_count && !source->implicit)
        return pl_fail(w, "implicit resource bindings are missing");
    if (source->implicit_count) {
        edges = pl_alloc(source->implicit_count, sizeof(*edges));
        if (!edges) return pl_fail(w, "cannot capture implicit resource bindings");
    }
    for (size_t i = 0; i < source->implicit_count; i++) {
        const sh_package_link_edge *edge = source->implicit + i;
        if (!edge->slot || !*edge->slot || edge->target >= w->count) {
            pl_fail(w, "implicit resource binding has an invalid slot or target"); goto done;
        }
        edges[i].slot = _strdup(edge->slot); edges[i].target = edge->target;
        if (!edges[i].slot) { pl_fail(w, "cannot capture implicit resource binding slot"); goto done; }
    }
    if (source->implicit_count > 1) qsort(edges, source->implicit_count, sizeof(*edges), pl_implicit_order);
    for (size_t i = 0; i < source->implicit_count; i++) {
        if (kept && !strcmp(edges[kept - 1].slot, edges[i].slot)) {
            if (edges[kept - 1].target != edges[i].target) {
                pl_fail(w, "implicit resource slot has contradictory captured targets"); goto done;
            }
            free((char *)edges[i].slot); edges[i].slot = NULL;
        } else {
            if (kept != i) { edges[kept] = edges[i]; edges[i].slot = NULL; }
            kept++;
        }
    }
    if (w->replay) {
        if (kept != unit->implicit_count) { pl_fail(w, "implicit resource bindings changed during linking"); goto done; }
        for (size_t i = 0; i < kept; i++)
            if (strcmp(edges[i].slot, unit->implicit[i].slot) || edges[i].target != unit->implicit[i].target) {
                pl_fail(w, "implicit resource bindings changed during linking"); goto done;
            }
    } else {
        unit->implicit_begin = unit->edge_count;
        for (size_t i = 0; i < kept; i++) {
            char *key = pl_slot("", edges[i].slot);
            if (!key) { pl_fail(w, "cannot retain implicit resource binding key"); goto done; }
            if (!pl_edge_add(w, key, edges[i].target)) goto done;
        }
        unit->implicit = edges; unit->implicit_count = kept; edges = NULL;
    }
    ok = 1;
done:
    if (edges) pl_implicit_free(edges, source->implicit_count);
    return ok;
}

static unsigned char *pl_bind(pl_work *w, size_t *length, int *unchanged)
{
    const sh_package_link_unit *source = &w->units[w->unit];
    pl_unit *u = &w->work[w->unit];
    pl_file *file = &u->files[w->file];
    size_t size = 0;
    unsigned char *body = sh_package_compilation_read(source->compilation,
        file->resource, SIZE_MAX, &size, w->error, w->capacity), *bound = NULL;
    sh_package_file_identity identity;
    if (unchanged) *unchanged = 0;
    if (!body || !sh_package_bytes_identity(body, size, &identity)) goto done;
    if (w->replay) {
        if (identity.length != file->identity.length || memcmp(identity.digest, file->identity.digest, 32)) {
            pl_fail(w, "resource bytes changed during linking"); goto done;
        }
        w->next_edge = file->edge_begin;
    } else {
        file->identity = identity; file->edge_begin = u->edge_count;
    }
    bound = w->adapter->bind(w->adapter->context, w->unit, file->resource,
        body, size, pl_reference, w, length, w->error, w->capacity);
    if (!bound || w->failed) goto done;
    if (unchanged) *unchanged = size == *length && !memcmp(body, bound, size);
    if (w->replay) {
        if (w->next_edge != file->edge_begin + file->edge_count) {
            pl_fail(w, "resource bindings disappeared during linking"); goto done;
        }
    } else {
        if (size != *length || memcmp(body, bound, size)) {
            pl_fail(w, "resource inspection changed source bytes"); goto done;
        }
        file->edge_count = u->edge_count - file->edge_begin;
    }
done:
    free(body);
    if (!bound || w->failed) {
        free(bound); bound = NULL;
        pl_fail(w, "resource binding inspection is incomplete");
    }
    return bound;
}

static void pl_u64(unsigned char **cursor, uint64_t value)
{
    for (size_t i = 0; i < 8; i++) (*cursor)[i] = (unsigned char)(value >> (i * 8));
    *cursor += 8;
}

/* Restore a valid earlier capture of this unit. Return 1 when restored, 0 to
 * capture afresh; an incomplete copy is discarded and captured afresh. */
static int pl_reuse_restore(pl_work *w, const sh_package_link_unit *source, pl_unit *u, sh_package_link_node *node)
{
    const sh_package_link_reuse *reuse = w->adapter->reuse;
    if (!pl_reuse_valid(reuse, w->unit)) return 0;
    const pl_reuse_unit *c = reuse->units + w->unit;
    if (c->file_count != source->file_count || c->witness_count != source->witness_count) return 0;
    u->files = pl_alloc(c->file_count, sizeof(*u->files));
    u->edges = pl_edges_copy(c->edges, c->edge_count);
    u->implicit = pl_edges_copy(c->implicit, c->implicit_count);
    if (c->witness_count) {
        u->witnesses = pl_alloc(c->witness_count, sizeof(*u->witnesses));
        u->witness_order = pl_alloc(c->witness_count, sizeof(*u->witness_order));
    }
    if (!u->files || !u->edges || !u->implicit || (c->witness_count && (!u->witnesses || !u->witness_order))) {
        pl_edges_free(u->edges, c->edge_count); pl_edges_free(u->implicit, c->implicit_count);
        free(u->files); free(u->witnesses); free(u->witness_order);
        memset(u, 0, sizeof(*u)); return 0;
    }
    memcpy(u->files, c->files, c->file_count * sizeof(*u->files));
    if (c->witness_count) {
        memcpy(u->witnesses, c->witnesses, c->witness_count * sizeof(*u->witnesses));
        memcpy((void *)u->witness_order, c->witness_order, c->witness_count * sizeof(*u->witness_order));
    }
    u->edge_count = u->edge_capacity = c->edge_count;
    u->implicit_count = c->implicit_count; u->implicit_begin = c->implicit_begin;
    node->family = source->family; node->path = source->name; node->length = c->length;
    memcpy(node->content, c->content, 32); memcpy(node->semantics, source->semantics, 32);
    node->edges = u->edges; node->edge_count = u->edge_count;
    return 1;
}
/* Keep this unit's completed capture for later passes, unless invalidated. */
static void pl_reuse_store(pl_work *w, const sh_package_link_unit *source, const pl_unit *u,
    const sh_package_link_node *node)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    pl_reuse_unit next = {0};
    if (!reuse || w->unit >= reuse->count || reuse->units[w->unit].invalid) return;
    next.files = pl_alloc(source->file_count, sizeof(*next.files));
    next.edges = pl_edges_copy(u->edges, u->edge_count);
    next.implicit = pl_edges_copy(u->implicit, u->implicit_count);
    if (source->witness_count) {
        next.witnesses = pl_alloc(source->witness_count, sizeof(*next.witnesses));
        next.witness_order = pl_alloc(source->witness_count, sizeof(*next.witness_order));
    }
    next.edge_count = u->edge_count; next.implicit_count = u->implicit_count;
    if (!next.files || !next.edges || !next.implicit ||
        (source->witness_count && (!next.witnesses || !next.witness_order))) {
        pl_reuse_unit_clear(&next); return; /* Reuse is optional: capture again next pass. */
    }
    memcpy(next.files, u->files, source->file_count * sizeof(*next.files));
    if (source->witness_count) {
        memcpy(next.witnesses, u->witnesses, source->witness_count * sizeof(*next.witnesses));
        memcpy((void *)next.witness_order, u->witness_order, source->witness_count * sizeof(*next.witness_order));
    }
    next.file_count = source->file_count; next.witness_count = source->witness_count;
    next.implicit_begin = u->implicit_begin; next.length = node->length;
    memcpy(next.content, node->content, 32); next.captured = 1;
    pl_reuse_unit_clear(reuse->units + w->unit); reuse->units[w->unit] = next;
}

static int pl_collect(pl_work *w)
{
    for (w->unit = 0; w->unit < w->count; w->unit++) {
        const sh_package_link_unit *source = &w->units[w->unit];
        pl_unit *u = &w->work[w->unit];
        sh_package_link_node *node = &w->nodes[w->unit];
        if (pl_reuse_restore(w, source, u, node)) continue;
        size_t manifest_size = 8, companions = 0, primary_members = 0;
        unsigned char *manifest, *cursor;
        sh_package_file_identity identity;
        if (!source->family || !*source->family || !source->name || !*source->name ||
            (source->file_count && (!source->files || !source->compilation)) ||
            (source->witness_count && (!source->witnesses || !source->file_count)) ||
            (!source->file_count && (source->implicit_owners || source->implicit_gameplay_owners)))
            return pl_fail(w, "incomplete resource unit");
        u->files = pl_alloc(source->file_count, sizeof(*u->files));
        if (!u->files) return pl_fail(w, "cannot allocate source resource units");
        for (size_t i = 0; i < source->file_count; i++) {
            if (!source->files[i] || !source->files[i]->engine_path || !*source->files[i]->engine_path)
                return pl_fail(w, "resource unit has an unnamed physical file");
            u->files[i].resource = source->files[i];
            const sh_package_link_file_identity *member = source->file_identities ? source->file_identities + i : NULL;
            if (member && (member->family || member->name) &&
                (!member->family || !*member->family || !member->name || !*member->name))
                return pl_fail(w, "resource member has an incomplete native identity");
            u->files[i].family = member && member->family ? member->family : source->family;
            u->files[i].name = member && member->name ? member->name : source->name;
            if (!strcmp(u->files[i].family, source->family) && !strcmp(u->files[i].name, source->name)) primary_members++;
        }
        if (source->file_count && !primary_members)
            return pl_fail(w, "resource unit has no primary native member");
        if (source->file_count > 1) qsort(u->files, source->file_count, sizeof(*u->files), pl_file_order);
        for (w->file = 0; w->file < source->file_count; w->file++) {
            pl_file *f = &u->files[w->file];
            const char *path = f->resource->engine_path;
            size_t length = 0, path_size = strlen(path);
            unsigned char *body;
            if (w->file && !strcmp(path, u->files[w->file - 1].resource->engine_path))
                return pl_fail(w, "resource unit repeats a physical file");
            body = pl_bind(w, &length, NULL);
            if (!body) return 0;
            free(body);
            if (path_size > SIZE_MAX - 48 || manifest_size > SIZE_MAX - 48 - path_size ||
                f->identity.length > UINT64_MAX - node->length)
                return pl_fail(w, "resource unit identity exceeds addressable size");
            manifest_size += 48 + path_size; node->length += f->identity.length;
            if (strcmp(f->family, source->family) || strcmp(f->name, source->name)) {
                size_t family = strlen(f->family), name = strlen(f->name);
                if (family > SIZE_MAX - 24 || name > SIZE_MAX - 24 - family ||
                    manifest_size > SIZE_MAX - 24 - family - name)
                    return pl_fail(w, "resource member identity exceeds addressable size");
                manifest_size += 24 + family + name; companions++;
            }
        }
        if (companions) {
            if (manifest_size > SIZE_MAX - 16) return pl_fail(w, "resource member identity exceeds addressable size");
            manifest_size += 16;
        }
        if (source->witness_count) {
            if (manifest_size > SIZE_MAX - 16) return pl_fail(w, "resource witness identity exceeds addressable size");
            manifest_size += 16;
            u->witnesses = pl_alloc(source->witness_count, sizeof(*u->witnesses));
            u->witness_order = pl_alloc(source->witness_count, sizeof(*u->witness_order));
            if (!u->witnesses || !u->witness_order) return pl_fail(w, "cannot retain resource witnesses");
            for (size_t i = 0; i < source->witness_count; i++) {
                const sh_package_link_witness *witness = source->witnesses + i;
                if (!witness->role || !*witness->role || !witness->resource ||
                    !witness->resource->engine_path || !*witness->resource->engine_path)
                    return pl_fail(w, "resource witness has no exact role or source path");
                u->witness_order[i] = witness;
            }
            qsort(u->witness_order, source->witness_count, sizeof(*u->witness_order), pl_witness_order);
            for (size_t i = 0; i < source->witness_count; i++) {
                const sh_package_link_witness *witness = u->witness_order[i];
                pl_file *file = u->witnesses + i; pl_input stream = {0};
                size_t role = strlen(witness->role), path = strlen(witness->resource->engine_path);
                if (i && !strcmp(witness->role, u->witness_order[i - 1]->role))
                    return pl_fail(w, "resource witness repeats a native role");
                file->resource = witness->resource;
                if (!sh_package_compilation_identity(source->compilation, file->resource, &file->identity, w->error, w->capacity))
                    return pl_fail(w, "resource witness identity is unavailable");
                int available = pl_input_open_resource(w, w->unit, file, &stream);
                if (stream.stream) fclose(stream.stream);
                if (!available) return 0;
                if (role > SIZE_MAX - 56 || path > SIZE_MAX - 56 - role ||
                    manifest_size > SIZE_MAX - 56 - role - path || file->identity.length > UINT64_MAX - node->length)
                    return pl_fail(w, "resource witness identity exceeds addressable size");
                manifest_size += 56 + role + path; node->length += file->identity.length;
            }
        }
        if (!pl_implicit_read(w)) return 0;
        manifest = cursor = malloc(manifest_size);
        if (!manifest) return pl_fail(w, "cannot allocate resource unit identity");
        pl_u64(&cursor, source->file_count);
        for (size_t i = 0; i < source->file_count; i++) {
            const pl_file *f = &u->files[i];
            size_t size = strlen(f->resource->engine_path);
            pl_u64(&cursor, size); memcpy(cursor, f->resource->engine_path, size); cursor += size;
            pl_u64(&cursor, f->identity.length); memcpy(cursor, f->identity.digest, 32); cursor += 32;
        }
        if (companions) {
            memcpy(cursor, "members\0", 8); cursor += 8; pl_u64(&cursor, companions);
            for (size_t i = 0; i < source->file_count; i++) {
                const pl_file *f = u->files + i;
                if (!strcmp(f->family, source->family) && !strcmp(f->name, source->name)) continue;
                size_t family = strlen(f->family), name = strlen(f->name);
                pl_u64(&cursor, i);
                pl_u64(&cursor, family); memcpy(cursor, f->family, family); cursor += family;
                pl_u64(&cursor, name); memcpy(cursor, f->name, name); cursor += name;
            }
        }
        if (source->witness_count) {
            memcpy(cursor, "witness\0", 8); cursor += 8; pl_u64(&cursor, source->witness_count);
            for (size_t i = 0; i < source->witness_count; i++) {
                const pl_file *file = u->witnesses + i;
                const sh_package_link_witness *witness = u->witness_order[i];
                size_t role = strlen(witness->role), path = strlen(file->resource->engine_path);
                pl_u64(&cursor, role); memcpy(cursor, witness->role, role); cursor += role;
                pl_u64(&cursor, path); memcpy(cursor, file->resource->engine_path, path); cursor += path;
                pl_u64(&cursor, file->identity.length); memcpy(cursor, file->identity.digest, 32); cursor += 32;
            }
        }
        int ok = sh_package_bytes_identity(manifest, manifest_size, &identity); free(manifest);
        if (!ok) return pl_fail(w, "cannot identify resource unit bytes");
        node->family = source->family; node->path = source->name;
        memcpy(node->content, identity.digest, 32); memcpy(node->semantics, source->semantics, 32);
        node->edges = u->edges; node->edge_count = u->edge_count;
        pl_reuse_store(w, source, u, node);
    }
    return 1;
}

static int pl_input_open_resource(pl_work *w, size_t unit, const pl_file *captured, pl_input *out)
{
    const sh_package_link_unit *source = &w->units[unit];
    const sh_compiled_resource *resource = captured->resource;
    memset(out, 0, sizeof(*out));
    if (resource->body) {
        out->body = resource->body; out->length = resource->body_length;
    } else if (resource->cache_file) {
        out->stream = sh_package_file_open(resource->cache_file, &out->length, w->error, w->capacity);
    } else if (source->compilation && source->compilation->sources &&
        source->compilation->sources->files && resource->source < source->compilation->sources->file_count) {
        sh_package_source_file entry = source->compilation->sources->files[resource->source];
        if (resource->cache_path) entry.absolute = resource->cache_path;
        out->length = entry.length;
        out->stream = sh_package_source_open(&entry, w->error, w->capacity);
    }
    if ((!out->body && !out->stream) || out->length != captured->identity.length)
        return pl_fail(w, "resource bytes are unavailable during exact supplier verification");
    return 1;
}
static int pl_input_open(pl_work *w, size_t unit, size_t file, pl_input *out)
{ return pl_input_open_resource(w, unit, &w->work[unit].files[file], out); }

static int pl_input_read(pl_input *input, unsigned char *bytes, size_t length)
{
    if (input->body) memcpy(bytes, input->body + (size_t)input->position, length);
    else if (fread(bytes, 1, length, input->stream) != length) return 0;
    input->position += length; return 1;
}

/* Physical-path sharing is separate from logical resource equivalence. Even
 * original aliases must prove exact source bytes, not merely matching hashes. */
static int pl_original_file_equal(pl_work *w, size_t left, size_t left_file,
    size_t right, size_t right_file)
{
    pl_input a = {0}, b = {0}; unsigned char x[16384], y[16384]; int equal = -1;
    if (!pl_input_open(w, left, left_file, &a) || !pl_input_open(w, right, right_file, &b)) goto done;
    if (a.length != b.length) { equal = 0; goto done; }
    while (a.position < a.length) {
        uint64_t remaining = a.length - a.position;
        size_t length = remaining < sizeof(x) ? (size_t)remaining : sizeof(x);
        if (!pl_input_read(&a, x, length) || !pl_input_read(&b, y, length)) {
            pl_fail(w, "original alias source read failed"); goto done;
        }
        if (memcmp(x, y, length)) { equal = 0; goto done; }
    }
    if ((a.stream && (fgetc(a.stream) != EOF || ferror(a.stream))) ||
        (b.stream && (fgetc(b.stream) != EOF || ferror(b.stream)))) {
        pl_fail(w, "original alias source length changed"); goto done;
    }
    equal = 1;
done:
    if (a.stream) fclose(a.stream);
    if (b.stream) fclose(b.stream);
    return equal;
}

/* Fingerprints select comparison candidates; only exact bytes certify sharing.
 * Streams retain their independent positions and hold the immutable source
 * handles throughout the comparison. No whole opaque file is allocated here. */
static int pl_verify_equal(void *context, size_t left, size_t right, char *error, size_t capacity)
{
    pl_work *w = context;
    unsigned char a[16384], b[16384];
    (void)error; (void)capacity;
    if (pl_reuse_proven(w->adapter->reuse, left, right)) return 1;
    if (w->units[left].file_count != w->units[right].file_count ||
        w->units[left].witness_count != w->units[right].witness_count)
        return pl_fail(w, "resource fingerprint collision: physical file sets differ");
    if (w->units[left].file_count > SIZE_MAX - w->units[left].witness_count)
        return pl_fail(w, "resource comparison count exceeds addressable size");
    for (size_t i = 0; i < w->units[left].file_count + w->units[left].witness_count; i++) {
        pl_input first = {0}, second = {0};
        int ok = 0;
        int witness = i >= w->units[left].file_count;
        size_t index = witness ? i - w->units[left].file_count : i;
        const pl_file *x = witness ? w->work[left].witnesses + index : w->work[left].files + index;
        const pl_file *y = witness ? w->work[right].witnesses + index : w->work[right].files + index;
        if ((witness && strcmp(w->work[left].witness_order[index]->role, w->work[right].witness_order[index]->role)) ||
            strcmp(x->resource->engine_path, y->resource->engine_path))
            return pl_fail(w, "resource fingerprint collision: physical paths differ");
        if (!witness && (strcmp(x->family, y->family) || strcmp(x->name, y->name)))
            return pl_fail(w, "resource fingerprint collision: native member identities differ");
        if (!pl_input_open_resource(w, left, x, &first) || !pl_input_open_resource(w, right, y, &second)) goto done;
        if (first.length != second.length) {
            pl_fail(w, "resource fingerprint collision: exact supplier lengths differ"); goto done;
        }
        while (first.position < first.length) {
            uint64_t remaining = first.length - first.position;
            size_t chunk = remaining < sizeof(a) ? (size_t)remaining : sizeof(a);
            if (!pl_input_read(&first, a, chunk) || !pl_input_read(&second, b, chunk)) {
                pl_fail(w, "resource read failed during exact supplier verification"); goto done;
            }
            if (memcmp(a, b, chunk)) {
                pl_fail(w, "resource fingerprint collision: exact supplier bytes differ"); goto done;
            }
        }
        if ((first.stream && (fgetc(first.stream) != EOF || ferror(first.stream))) ||
            (second.stream && (fgetc(second.stream) != EOF || ferror(second.stream)))) {
            pl_fail(w, "resource length changed during exact supplier verification"); goto done;
        }
        ok = 1;
done:
        if (first.stream) fclose(first.stream);
        if (second.stream) fclose(second.stream);
        if (!ok) return 0;
    }
    pl_reuse_prove(w->adapter->reuse, left, right);
    return 1;
}

static int pl_members(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    size_t *cursor = NULL;
    if (classes->count == SIZE_MAX) return pl_fail(w, "too many resource equivalence classes");
    w->member_offsets = pl_alloc(classes->count + 1, sizeof(*w->member_offsets));
    w->members = pl_alloc(w->count, sizeof(*w->members));
    cursor = pl_alloc(classes->count, sizeof(*cursor));
    if (!w->member_offsets || !w->members || !cursor) {
        free(cursor); return pl_fail(w, "cannot allocate resource equivalence members");
    }
    for (size_t i = 0; i < w->count; i++) w->member_offsets[classes->nodes[i] + 1]++;
    for (size_t i = 0; i < classes->count; i++) {
        w->member_offsets[i + 1] += w->member_offsets[i]; cursor[i] = w->member_offsets[i];
    }
    for (size_t i = 0; i < w->count; i++) w->members[cursor[classes->nodes[i]]++] = i;
    free(cursor); return 1;
}

/* This hash indexes host-side strings only. Native ID computation always
 * belongs to the registry callback, which knows the actual native type name. */
static uint64_t pl_name_hash(const char *family, const char *name)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const char *parts[2] = {family, name};
    for (size_t i = 0; i < 2; i++) {
        const unsigned char *p = (const unsigned char *)parts[i];
        do { hash = (hash ^ *p) * UINT64_C(1099511628211); } while (*p++);
    }
    return hash;
}

static size_t pl_table_capacity(size_t count)
{
    size_t capacity = 8;
    if (count > SIZE_MAX / 2) return 0;
    while (capacity < count * 2) {
        if (capacity > SIZE_MAX / 2) return 0;
        capacity *= 2;
    }
    return capacity;
}

static size_t pl_name_slot(const pl_registry *registry, const pl_native_identity *identity, int native)
{
    size_t *table = native ? registry->ids : registry->names;
    uint64_t hash = native ? pl_name_hash(identity->family, "") ^ identity->id :
        pl_name_hash(identity->family, identity->name);
    size_t slot = (size_t)hash & (registry->capacity - 1);
    while (table[slot]) {
        const pl_native_identity *other = registry->identities + table[slot] - 1;
        if (!strcmp(identity->family, other->family) && (native ? identity->id == other->id :
            !strcmp(identity->name, other->name))) break;
        slot = (slot + 1) & (registry->capacity - 1);
    }
    return slot;
}

static size_t pl_path_slot(const pl_registry *registry, const char *path)
{
    size_t slot = (size_t)pl_name_hash("", path) & (registry->path_capacity - 1);
    while (registry->paths[slot] && strcmp(registry->paths[slot], path))
        slot = (slot + 1) & (registry->path_capacity - 1);
    return slot;
}

static void pl_assignment_clear(pl_work *w, size_t group)
{
    size_t index = w->output.classes.representatives[group];
    pl_unit *unit = &w->work[index];
    free(w->output.names[group]); w->output.names[group] = NULL;
    if (unit->paths) for (size_t i = 0; i < w->units[index].file_count; i++) free(unit->paths[i]);
    free(unit->paths); unit->paths = NULL;
    if (unit->names) for (size_t i = 0; i < w->units[index].file_count; i++) free(unit->names[i]);
    free(unit->names); unit->names = NULL;
}

static int pl_same_original(pl_work *w, const pl_native_identity *a, const pl_native_identity *b)
{
    /* Different coupled groups may reach the same certified baseline object.
     * Never infer this from equal member bytes in an otherwise modified group. */
    if (!a->original || !b->original || (a->primary && b->primary) || strcmp(a->family, b->family) ||
        strcmp(a->name, b->name) || strcmp(a->original_name, b->original_name)) return 0;
    size_t left = 0, right = 0, matched = 0;
    size_t acount = w->units[a->unit].file_count, bcount = w->units[b->unit].file_count;
    const pl_file *afiles = w->work[a->unit].files, *bfiles = w->work[b->unit].files;
    for (;;) {
        while (left < acount && (strcmp(afiles[left].family, a->family) || strcmp(afiles[left].name, a->original_name))) left++;
        while (right < bcount && (strcmp(bfiles[right].family, b->family) || strcmp(bfiles[right].name, b->original_name))) right++;
        if (left == acount || right == bcount) return left == acount && right == bcount && matched != 0;
        if (strcmp(afiles[left].resource->engine_path, bfiles[right].resource->engine_path)) return 0;
        int equal = pl_original_file_equal(w, a->unit, left, b->unit, right);
        if (equal <= 0) return equal;
        left++; right++; matched++;
    }
}

static int pl_assign_members(pl_work *w, size_t index, size_t group,
    const sh_compiled_resource *const *files, pl_native_identity *identities, size_t *used)
{
    const sh_package_link_unit *source = w->units + index;
    pl_unit *unit = w->work + index;
    size_t count = source->file_count;
    int coupled = 0;
    identities[0] = (pl_native_identity){source->family, w->output.names[group], source->name, 0, w->originals[index], 1, index};
    *used = 1;
    for (size_t i = 0; i < count; i++) if (strcmp(unit->files[i].family, source->family) ||
        strcmp(unit->files[i].name, source->name)) coupled = 1;
    if (!coupled) return 1;
    if (!w->adapter->assign_members ||
        !w->adapter->assign_members(w->adapter->context, index, w->output.names[group],
            files, (const char *const *)unit->paths, count, &unit->names, w->error, w->capacity) || !unit->names)
        return pl_fail(w, "coupled resource members require native name assignment");
    for (size_t i = 0; i < count; i++) {
        const pl_file *file = unit->files + i;
        if (!unit->names[i] || !*unit->names[i]) return pl_fail(w, "native resource member has no assigned name");
        size_t j;
        for (j = 0; j < *used; j++) if (!strcmp(file->family, identities[j].family) &&
            !strcmp(file->name, identities[j].original_name)) break;
        if (j < *used) {
            if (strcmp(unit->names[i], identities[j].name))
                return pl_fail(w, "native resource member assignment splits one authored identity");
        } else identities[(*used)++] = (pl_native_identity){file->family, unit->names[i], file->name, 0, w->originals[index], 0, index};
    }
    return 1;
}

static int pl_name_token(const unsigned char identity[32], size_t attempt, unsigned char token[32])
{
    static const char domain[] = "snapmap-plus/native-name-collision";
    unsigned char bytes[sizeof(domain) + 32 + 8], *cursor = bytes;
    sh_package_file_identity hashed;
    if (!attempt) { memcpy(token, identity, 32); return 1; }
    memcpy(cursor, domain, sizeof(domain)); cursor += sizeof(domain);
    memcpy(cursor, identity, 32); cursor += 32; pl_u64(&cursor, attempt);
    if (!sh_package_bytes_identity(bytes, sizeof(bytes), &hashed)) return 0;
    memcpy(token, hashed.digest, 32); return 1;
}

static int pl_assign_one(pl_work *w, pl_registry *registry, size_t group)
{
    const sh_package_link_registry *native = w->adapter->registry;
    sh_package_link_classes *classes = &w->output.classes;
    size_t index = classes->representatives[group], count = w->units[index].file_count;
    pl_unit *unit = &w->work[index];
    const sh_compiled_resource **files = pl_alloc(count, sizeof(*files));
    pl_native_identity *identities = count < SIZE_MAX ? pl_alloc(count + 1, sizeof(*identities)) : NULL;
    int ok = 0;
    if (!files || !identities) { pl_fail(w, "cannot allocate native path assignment"); goto done; }
    for (size_t i = 0; i < count; i++) files[i] = unit->files[i].resource;
    char cause[384] = "";
    for (size_t attempt = 0; attempt < 1024; attempt++) {
        unsigned char token[32];
        size_t identity_count = 0;
        int collision = 0;
        cause[0] = 0;
        *w->error = 0;
        if (!pl_name_token(classes->identities[group], attempt, token) ||
            !w->adapter->assign(w->adapter->context, index, token, w->originals[index],
                files, count, &w->output.names[group], &unit->paths, w->error, w->capacity) ||
            !w->output.names[group] || !*w->output.names[group] || (count && !unit->paths) ||
            (!count && strcmp(w->output.names[group], w->units[index].name))) {
            pl_fail(w, "native resource identity cannot be assigned safely"); goto done;
        }
        for (size_t i = 0; i < count; i++) if (!unit->paths[i] || !*unit->paths[i]) {
            pl_fail(w, "native resource identity is missing a physical path"); goto done;
        }
        if (!pl_assign_members(w, index, group, files, identities, &identity_count)) goto done;
        for (size_t i = 0; i < count; i++) {
            size_t slot = pl_path_slot(registry, unit->paths[i]);
            if (registry->paths[slot]) {
                size_t other = registry->path_units[slot], file = registry->path_files[slot];
                int equal = 0;
                if (w->originals[index] && w->originals[other] &&
                    !strcmp(unit->files[i].resource->engine_path, w->work[other].files[file].resource->engine_path)) {
                    equal = pl_original_file_equal(w, index, i, other, file);
                    if (equal < 0) goto done;
                }
                if (!equal && !collision++) snprintf(cause, sizeof(cause), "path %s is already bound by %s %s",
                    unit->paths[i], w->units[other].family, w->units[other].name);
            }
            for (size_t j = 0; j < i; j++) if (!strcmp(unit->paths[i], unit->paths[j]) && !collision++)
                snprintf(cause, sizeof(cause), "path %s is assigned twice", unit->paths[i]);
        }
        for (size_t i = 0; i < identity_count; i++) {
            pl_native_identity *identity = identities + i;
            size_t occupied = registry->names[pl_name_slot(registry, identity, 0)];
            if (occupied) {
                const pl_native_identity *held = registry->identities + occupied - 1;
                int shared = pl_same_original(w, identity, held);
                if (shared < 0) goto done;
                if (!shared && !collision++) snprintf(cause, sizeof(cause), "%s %s is already held by %s %s",
                    identity->family, identity->name, w->units[held->unit].family, w->units[held->unit].name);
            }
            if (native) {
                int state = native->check(native->context, identity->family, identity->name,
                    classes->identities[group], w->originals[index], &identity->id, w->error, w->capacity);
                if (state < SH_PACKAGE_LINK_REGISTRY_COLLISION ||
                    state > SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED || !identity->id) {
                    pl_fail(w, "native resource registry verification is incomplete"); goto done;
                }
                if (state == SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED &&
                    (w->originals[index] || !count || strcmp(identity->name, identity->original_name))) {
                    pl_fail(w, "native shared replacement requires a changed same-name binding"); goto done;
                }
                occupied = registry->ids[pl_name_slot(registry, identity, 1)];
                if (state == SH_PACKAGE_LINK_REGISTRY_COLLISION && !collision++)
                    snprintf(cause, sizeof(cause), "the native registry holds %s %s differently", identity->family, identity->name);
                if (occupied) {
                    const pl_native_identity *held = registry->identities + occupied - 1;
                    int shared = pl_same_original(w, identity, held);
                    if (shared < 0) goto done;
                    if (!shared && !collision++) snprintf(cause, sizeof(cause), "native id of %s %s is already held by %s %s",
                        identity->family, identity->name, w->units[held->unit].family, w->units[held->unit].name);
                }
            }
            /* Candidates in this group are not in the shared index yet. */
            for (size_t j = 0; j < i; j++) if (!strcmp(identity->family, identities[j].family) &&
                (!strcmp(identity->name, identities[j].name) || (native && identity->id == identities[j].id)) && !collision++)
                snprintf(cause, sizeof(cause), "members share %s %s", identity->family, identity->name);
        }
        if (!collision) {
            for (size_t i = 0; i < identity_count; i++) {
                size_t name_slot = pl_name_slot(registry, identities + i, 0);
                if (registry->names[name_slot]) continue; /* Certified original shared by groups. */
                size_t key = ++registry->identity_count;
                registry->identities[key - 1] = identities[i]; registry->names[name_slot] = key;
                if (native) registry->ids[pl_name_slot(registry, identities + i, 1)] = key;
            }
            for (size_t i = 0; i < count; i++) {
                size_t slot = pl_path_slot(registry, unit->paths[i]);
                if (!registry->paths[slot]) {
                    registry->paths[slot] = unit->paths[i]; registry->path_units[slot] = index; registry->path_files[slot] = i;
                }
            }
            *w->error = 0; ok = 1; break;
        }
        if (w->originals[index] || !count) {
            snprintf(w->error, w->capacity, "certified original resource has a native identity collision: %s %s: %s",
                w->units[index].family, w->units[index].name, cause);
            w->failed = 1; goto done;
        }
        pl_assignment_clear(w, group);
    }
    if (!ok) {
        snprintf(w->error, w->capacity, "native identity collision allocation exhausted: %s %s: %s",
            w->units[index].family, w->units[index].name, cause);
        w->failed = 1;
    }
done:
    free(files); free(identities); return ok;
}

static int pl_assign(pl_work *w)
{
    sh_package_link_classes *classes = &w->output.classes;
    pl_assignment *order = NULL;
    pl_registry registry = {0};
    size_t path_count = 0, identity_count = classes->count;
    int ok = 0;
    w->output.names = pl_alloc(classes->count, sizeof(*w->output.names));
    if (!w->output.names) return pl_fail(w, "cannot allocate linked resource identities");
    w->originals = pl_alloc(w->count, sizeof(*w->originals));
    if (!w->originals) return pl_fail(w, "cannot allocate original binding certification");
    for (size_t c = 0; c < classes->count; c++) {
        unsigned char original = 1;
        for (size_t i = w->member_offsets[c]; i < w->member_offsets[c + 1]; i++)
            if (!w->units[w->members[i]].original_binding) original = 0;
        for (size_t i = w->member_offsets[c]; i < w->member_offsets[c + 1]; i++)
            w->originals[w->members[i]] = original;
    }
    /* Campaign-only data a scope supplies (original_binding 2) keeps the name
     * the renderer or audio derives from it, but it is not SnapMap's: it never
     * makes a dependent original, and keeps its own name only while every
     * dependency it binds keeps its name too. */
    unsigned char *own = pl_alloc(w->count, 1);
    if (!own) return pl_fail(w, "cannot allocate original binding certification");
    memcpy(own, w->originals, w->count);
    for (size_t i = 0; i < w->count; i++) if (w->units[i].original_binding == 2) w->originals[i] = 0;
    if (!sh_package_link_originals(w->nodes, w->count, w->originals, w->originals, w->error, w->capacity)) {
        free(own); return 0;
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < w->count; i++) {
            if (w->units[i].original_binding != 2 || !own[i]) continue;
            int kept = 1;
            for (size_t e = 0; kept && e < w->nodes[i].edge_count; e++) {
                size_t target = w->nodes[i].edges[e].target;
                kept = w->units[target].original_binding == 2 ? own[target] : w->originals[target];
            }
            if (!kept) { own[i] = 0; changed = 1; }
        }
    }
    for (size_t i = 0; i < w->count; i++) if (w->units[i].original_binding == 2) w->originals[i] = own[i];
    free(own);
    order = pl_alloc(classes->count, sizeof(*order));
    if (!order) goto allocation;
    for (size_t c = 0; c < classes->count; c++) {
        size_t index = classes->representatives[c];
        if (w->units[index].file_count > SIZE_MAX - path_count) goto allocation;
        path_count += w->units[index].file_count;
        for (size_t i = 0; i < w->units[index].file_count; i++) {
            const pl_file *file = w->work[index].files + i;
            if (strcmp(file->family, w->units[index].family) || strcmp(file->name, w->units[index].name)) {
                if (identity_count == SIZE_MAX) goto allocation;
                identity_count++;
            }
        }
        order[c] = (pl_assignment){w->units[index].family, w->units[index].name,
            classes->identities[c], c, w->originals[index]};
    }
    registry.capacity = pl_table_capacity(identity_count); registry.path_capacity = pl_table_capacity(path_count);
    if (!registry.capacity || !registry.path_capacity) goto allocation;
    registry.names = pl_alloc(registry.capacity, sizeof(*registry.names));
    registry.ids = pl_alloc(registry.capacity, sizeof(*registry.ids));
    registry.paths = pl_alloc(registry.path_capacity, sizeof(*registry.paths));
    registry.path_units = pl_alloc(registry.path_capacity, sizeof(*registry.path_units));
    registry.path_files = pl_alloc(registry.path_capacity, sizeof(*registry.path_files));
    registry.identities = pl_alloc(identity_count, sizeof(*registry.identities));
    if (!registry.names || !registry.ids || !registry.paths || !registry.path_units || !registry.path_files || !registry.identities) goto allocation;
    if (classes->count > 1) qsort(order, classes->count, sizeof(*order), pl_assignment_order);
    for (size_t i = 1; i < classes->count; i++) if (!pl_assignment_order(order + i - 1, order + i)) {
        pl_fail(w, "distinct resource bindings have the same binding fingerprint"); goto done;
    }
    for (size_t i = 0; i < classes->count; i++) if (!pl_assign_one(w, &registry, order[i].group)) goto done;
    ok = 1; goto done;
allocation:
    pl_fail(w, "cannot allocate native identity index");
done:
    free(order); free(registry.names); free(registry.ids); free(registry.paths); free(registry.identities);
    free(registry.path_units); free(registry.path_files);
    return ok;
}

static int pl_dependency(pl_work *w, sh_compiled_resource *resource, const char *path)
{
    if (!strcmp(resource->engine_path, path)) return 1;
    for (size_t i = 0; i < resource->bound_dependency_count; i++)
        if (!strcmp(resource->bound_dependencies[i], path)) return 1;
    if (resource->bound_dependency_count == SIZE_MAX / sizeof(*resource->bound_dependencies))
        return pl_fail(w, "too many linked dependency paths");
    char *copy = _strdup(path);
    char **grown = copy ? realloc(resource->bound_dependencies,
        (resource->bound_dependency_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return pl_fail(w, "cannot retain linked dependency paths"); }
    resource->bound_dependencies = grown; grown[resource->bound_dependency_count++] = copy;
    return 1;
}

static int pl_dependencies(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    size_t *queue = pl_alloc(classes->count, sizeof(*queue));
    size_t *seen = pl_alloc(classes->count, sizeof(*seen)), generation = 0;
    int ok = 0;
    if (!queue || !seen) { pl_fail(w, "cannot retain implicit dependency traversal"); goto done; }
    for (size_t i = 0; i < w->output.compiled->resource_count; i++) {
        sh_compiled_resource *resource = w->output.compiled->resources + i;
        for (size_t j = 0; j < resource->bound_dependency_count; j++) free(resource->bound_dependencies[j]);
        free(resource->bound_dependencies); resource->bound_dependencies = NULL; resource->bound_dependency_count = 0;
    }
    for (size_t c = 0; c < classes->count; c++) {
        size_t index = classes->representatives[c], count = w->units[index].file_count;
        const pl_unit *unit = &w->work[index];
        for (size_t f = 0; f < count; f++) {
            sh_compiled_resource *resource = (sh_compiled_resource *)sh_package_compilation_find(
                w->output.compiled, unit->paths[f]);
            if (!resource) { pl_fail(w, "linked dependency owner is absent"); goto done; }
            /* A resource manager unit can contain multiple physical files.
             * One cycle retains all companions without a quadratic edge set. */
            if (count > 1 && !pl_dependency(w, resource, unit->paths[(f + 1) % count])) goto done;
            const pl_file *file = unit->files + f;
            size_t queued = 0, visited = 0;
            if (!++generation) { memset(seen, 0, classes->count * sizeof(*seen)); generation = 1; }
            /* Unit-wide dependencies need one retaining edge per companion
             * cycle. File-local edges retain their original owning member. */
            for (unsigned pass = 0; pass < (f ? 1u : 2u); pass++) {
                size_t begin = pass ? unit->implicit_begin : file->edge_begin;
                size_t end = begin + (pass ? unit->implicit_count : file->edge_count);
                for (size_t e = begin; e < end; e++) {
                    size_t target = classes->nodes[unit->edges[e].target];
                    if (seen[target] != generation) { seen[target] = generation; queue[queued++] = target; }
                }
            }
            while (visited < queued) {
                size_t target = classes->representatives[queue[visited++]];
                if (w->units[target].file_count) {
                    if (!pl_dependency(w, resource, w->work[target].paths[0])) goto done;
                } else {
                    /* An admitted native object may point to supplied data.
                     * Preserve that reachability through source-free nodes,
                     * including cycles, instead of dropping their edges. */
                    const pl_unit *native = w->work + target;
                    for (size_t e = 0; e < native->edge_count; e++) {
                        size_t next = classes->nodes[native->edges[e].target];
                        if (seen[next] != generation) { seen[next] = generation; queue[queued++] = next; }
                    }
                }
            }
        }
    }
    ok = 1;
done:
    free(queue); free(seen); return ok;
}

static int pl_implicit_output(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    size_t count = 0;
    for (size_t c = 0; c < classes->count; c++) {
        size_t n = w->work[classes->representatives[c]].implicit_count;
        if (n > SIZE_MAX - count) return pl_fail(w, "too many linked implicit bindings");
        count += n;
    }
    if (!count) return 1;
    w->output.implicit = pl_alloc(count, sizeof(*w->output.implicit));
    if (!w->output.implicit) return pl_fail(w, "cannot retain linked implicit bindings");
    for (size_t c = 0; c < classes->count; c++) {
        const pl_unit *unit = w->work + classes->representatives[c];
        for (size_t i = 0; i < unit->implicit_count; i++) {
            sh_package_link_implicit_binding *binding = w->output.implicit + w->output.implicit_count++;
            binding->owner = c; binding->target = classes->nodes[unit->implicit[i].target];
            binding->slot = _strdup(unit->implicit[i].slot);
            if (!binding->slot) return pl_fail(w, "cannot retain linked implicit binding slot");
        }
    }
    return 1;
}

/* A placed entity retains the editor template that explicitly describes it.
 * This is a lifetime edge after equivalence/name assignment, not a native
 * source dependency or a reason to merge two otherwise different bindings. */
static int pl_editor_companions(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    for (size_t c = 0; c < classes->count; c++) {
        size_t selector = classes->representatives[c], cursor = selector, target = SIZE_MAX;
        if (strcmp(w->units[selector].family, "snapeditorentitydef") || !w->units[selector].file_count) continue;
        for (size_t depth = 0; depth < classes->count; depth++) {
            const pl_unit *unit = w->work + cursor;
            size_t parent = SIZE_MAX; int direct = 0;
            for (size_t f = 0; f < w->units[cursor].file_count; f++) {
                const pl_file *file = unit->files + f;
                const sh_compiled_resource *bound = sh_package_compilation_find(w->output.compiled, unit->paths[f]);
                if (!bound || !bound->body) return pl_fail(w, "editor companion has no bound declaration body");
                sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)bound->body, bound->body_length}, w->error, w->capacity);
                if (!tree) return 0;
                const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
                direct |= sh_decl_tree_member(edit, "entityDef") != NULL || sh_decl_tree_member(tree, "edit.entityDef") != NULL;
                sh_decl_tree_free(tree);
                for (size_t e = file->edge_begin; e < file->edge_begin + file->edge_count; e++) {
                    const sh_package_link_edge *edge = unit->edges + e;
                    const char *slot = strchr(edge->slot, ':');
                    if (!slot) return pl_fail(w, "editor companion edge lost its typed site");
                    slot += 1 + strlen(file->resource->engine_path);
                    size_t resolved = classes->representatives[classes->nodes[edge->target]];
                    if (!strcmp(slot, "edit.entityDef") && !strcmp(w->units[resolved].family, "entitydef")) target = resolved;
                    if (!strcmp(slot, "inherit") && !strcmp(w->units[resolved].family, "snapeditorentitydef")) parent = resolved;
                }
            }
            if (target != SIZE_MAX || direct || parent == SIZE_MAX) break;
            cursor = parent;
        }
        if (target != SIZE_MAX && w->units[target].file_count) {
            sh_compiled_resource *entity = (sh_compiled_resource *)sh_package_compilation_find(w->output.compiled, w->work[target].paths[0]);
            if (!entity || !pl_dependency(w, entity, w->work[selector].paths[0])) return 0;
        }
    }
    return 1;
}

static int pl_projection_order(const void *a, const void *b)
{
    const sh_package_resource_projection *x = a, *y = b;
    int order = strcmp(x->engine_path, y->engine_path);
    if (!order) order = strcmp(x->native_type, y->native_type);
    return order ? order : strcmp(x->native_name, y->native_name);
}
static int pl_projection_equal(pl_work *w, const sh_package_sources *sources,
    const sh_package_resource_projection *a, const sh_package_resource_projection *b)
{
    /* NULL bodies positively mean unchanged replay. Assignment already proved
     * exact original bytes for every class reusing this physical path. */
    if (!a->body && !b->body) return 1;
    sh_package_compilation source = {0}; source.sources = sources;
    unsigned char *left = NULL, *right = NULL;
    size_t alen = a->length, blen = b->length;
    const unsigned char *x = a->body, *y = b->body;
    if (!x) x = left = sh_package_compilation_read(&source, a->inputs[0], SIZE_MAX, &alen, w->error, w->capacity);
    if (!y) y = right = sh_package_compilation_read(&source, b->inputs[0], SIZE_MAX, &blen, w->error, w->capacity);
    int equal = x && y && alen == blen && !memcmp(x, y, alen);
    free(left); free(right);
    return equal ? 1 : pl_fail(w, "logical resource aliases emit different bytes at one physical path");
}
typedef struct pl_project_alias { const char *path, *type, *name; } pl_project_alias;
static int pl_project(pl_work *w, const sh_package_sources *sources, const sh_package_policy *policy,
    char *(*canonical_path)(const char *))
{
    sh_package_link_classes *classes = &w->output.classes;
    size_t count = 0, used = 0;
    sh_package_resource_projection *projections = NULL;
    pl_project_alias *aliases = NULL;
    int ok = 0;
    for (size_t c = 0; c < classes->count; c++) {
        size_t files = w->units[classes->representatives[c]].file_count;
        if (files > SIZE_MAX - count) return pl_fail(w, "too many linked resource files");
        count += files;
    }
    projections = pl_alloc(count, sizeof(*projections));
    aliases = pl_alloc(count, sizeof(*aliases));
    if (!projections || !aliases) { free(projections); free(aliases); return pl_fail(w, "cannot allocate linked resource provider"); }
    w->replay = 1;
    for (size_t c = 0; c < classes->count; c++) {
        size_t start = w->member_offsets[c], end = w->member_offsets[c + 1];
        size_t members = end - start;
        w->unit = classes->representatives[c];
        pl_unit *u = &w->work[w->unit];
        for (w->file = 0; w->file < w->units[w->unit].file_count; w->file++) {
            sh_package_resource_projection *p = &projections[used++];
            int unchanged = 0;
            const sh_compiled_resource **inputs = pl_alloc(members, sizeof(*inputs));
            p->inputs = inputs;
            if (!inputs) goto done;
            for (size_t i = start; i < end; i++) {
                size_t index = w->members[i];
                if (w->units[index].file_count != w->units[w->unit].file_count ||
                    strcmp(w->work[index].files[w->file].resource->engine_path, u->files[w->file].resource->engine_path))
                    { pl_fail(w, "equivalent resource files do not match"); goto done; }
                inputs[p->input_count++] = w->work[index].files[w->file].resource;
            }
            p->engine_path = u->paths[w->file];
            p->native_type = u->files[w->file].family;
            p->native_name = u->names ? u->names[w->file] : w->output.names[c];
            p->body = pl_bind(w, &p->length, &unchanged);
            if (!p->body) goto done;
            /* Unchanged opaque files keep their immutable source/cache handle;
             * linking must not retain a second in-memory copy of every model. */
            if (unchanged) {
                free((void *)p->body); p->body = NULL; p->length = 0;
            }
        }
    }
    /* Equivalent sources still have independent lifetimes. Do not let a
     * changed duplicate or unstable scope hide behind its representative. */
    for (w->unit = 0; w->unit < w->count; w->unit++)
        if (classes->representatives[classes->nodes[w->unit]] != w->unit)
            for (w->file = 0; w->file < w->units[w->unit].file_count; w->file++) {
                size_t length = 0;
                unsigned char *body = pl_bind(w, &length, NULL);
                if (!body) goto done;
                free(body);
            }
    for (w->unit = 0; w->unit < w->count; w->unit++)
        if (!pl_implicit_read(w)) goto done;
    for (size_t unit = 0; unit < w->count; unit++) for (size_t i = 0; i < w->units[unit].witness_count; i++) {
        const pl_file *captured = w->work[unit].witnesses + i; sh_package_file_identity current;
        pl_input stream = {0};
        if (!sh_package_compilation_identity(w->units[unit].compilation, captured->resource,
            &current, w->error, w->capacity) || current.length != captured->identity.length ||
            memcmp(current.digest, captured->identity.digest, 32)) {
            pl_fail(w, "resource witness bytes changed during linking"); goto done;
        }
        int available = pl_input_open_resource(w, unit, captured, &stream);
        if (stream.stream) fclose(stream.stream);
        if (!available) goto done;
    }
    if (count > 1) qsort(projections, count, sizeof(*projections), pl_projection_order);
    size_t unique = 0;
    for (size_t i = 0; i < count; i++) {
        sh_package_resource_projection *p = projections + i;
        aliases[i] = (pl_project_alias){p->engine_path, p->native_type, p->native_name};
        if (unique && !strcmp(projections[unique - 1].engine_path, p->engine_path)) {
            sh_package_resource_projection *prior = projections + unique - 1;
            if (!pl_projection_equal(w, sources, prior, p) ||
                p->input_count > SIZE_MAX / sizeof(*p->inputs) - prior->input_count) goto done;
            size_t members = prior->input_count + p->input_count;
            const sh_compiled_resource **inputs = realloc((void *)prior->inputs, members * sizeof(*inputs));
            if (!inputs) { pl_fail(w, "cannot retain original alias suppliers"); goto done; }
            memcpy(inputs + prior->input_count, p->inputs, p->input_count * sizeof(*inputs));
            prior->inputs = inputs; prior->input_count = members;
            free((void *)p->inputs); free((void *)p->body); memset(p, 0, sizeof(*p));
        } else {
            if (unique != i) { projections[unique] = *p; memset(p, 0, sizeof(*p)); }
            unique++;
        }
    }
    w->output.compiled = sh_package_compilation_project(sources, projections, unique,
        policy, canonical_path, w->error, w->capacity);
    if (!w->output.compiled) goto done;
    for (size_t i = 0; i < count; i++) {
        sh_compiled_resource *resource = (sh_compiled_resource *)sh_package_compilation_find(w->output.compiled, aliases[i].path);
        if (!resource || !sh_package_resource_native_add(resource, aliases[i].type, aliases[i].name)) {
            pl_fail(w, "cannot retain a shared physical resource's logical identity"); goto done;
        }
    }
    /* Source-record ownership is independent of equality: two equal bindings
     * can have different suppliers. Keep the complete delivery union without
     * pretending a bank/media input is this declaration's physical file. */
    for (size_t i = 0; i < w->count; i++) {
        const sh_package_link_unit *unit = w->units + i;
        if ((unit->implicit_owners && !sh_package_owners_within(unit->implicit_owners, sources->package_count)) ||
            (unit->implicit_gameplay_owners && !sh_package_owners_within(unit->implicit_gameplay_owners, sources->package_count))) {
            pl_fail(w, "implicit resource ownership is outside the captured inventory"); goto done;
        }
        const pl_unit *representative = w->work + classes->representatives[classes->nodes[i]];
        for (size_t f = 0; f < unit->file_count; f++) {
            sh_compiled_resource *resource = (sh_compiled_resource *)sh_package_compilation_find(
                w->output.compiled, representative->paths[f]);
            if (!resource || (unit->implicit_owners && !sh_package_owners_union(&resource->owners, unit->implicit_owners)) ||
                (unit->implicit_gameplay_owners && !sh_package_owners_union(&resource->gameplay_owners, unit->implicit_gameplay_owners))) {
                pl_fail(w, "cannot retain implicit resource ownership"); goto done;
            }
        }
    }
    ok = pl_dependencies(w) && pl_editor_companions(w) && pl_implicit_output(w);
done:
    for (size_t i = 0; i < used; i++) { free((void *)projections[i].inputs); free((void *)projections[i].body); }
    free(projections); free(aliases); return ok;
}

static int pl_identify(pl_work *w)
{
    if ((w->count && !w->units) || !w->adapter || !w->adapter->resolve || !w->adapter->bind)
        return pl_fail(w, "resource identity inputs require resolved source readers");
    /* Check every identity before a resolver can reference a later unit. */
    for (size_t i = 0; i < w->count; i++) if (!w->units[i].family || !*w->units[i].family ||
        !w->units[i].name || !*w->units[i].name)
        return pl_fail(w, "resource identity inputs contain an unnamed unit");
    w->work = pl_alloc(w->count, sizeof(*w->work));
    w->nodes = pl_alloc(w->count, sizeof(*w->nodes));
    return w->work && w->nodes && pl_collect(w) &&
        sh_package_link_identify_verified(w->nodes, w->count, pl_verify_equal, w,
            &w->output.classes, w->error, w->capacity);
}

int sh_package_link_identify_units(const sh_package_link_unit *units, size_t count,
    const sh_package_link_adapter *adapter, sh_package_link_classes *out,
    char *error, size_t capacity)
{
    char fallback[1024]; pl_work w = {0}; int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!out) { snprintf(error, capacity, "resource identity output is required"); return 0; }
    sh_package_link_classes_free(out);
    w.units = units; w.count = count; w.adapter = adapter; w.error = error; w.capacity = capacity;
    if (pl_identify(&w)) {
        *out = w.output.classes; memset(&w.output.classes, 0, sizeof(w.output.classes)); ok = 1;
    }
    if (!ok && !*error) snprintf(error, capacity, "resource identities could not be captured");
    pl_clear(&w); return ok;
}

int sh_package_link(const sh_package_sources *sources, const sh_package_link_unit *units,
    size_t count, const sh_package_link_adapter *adapter, const sh_package_policy *policy,
    char *(*canonical_path)(const char *), sh_package_link_output *out, char *error, size_t capacity)
{
    char fallback[1024];
    pl_work w = {0};
    int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!out) { snprintf(error, capacity, "linked provider output is required"); return 0; }
    sh_package_link_output_free(out);
    w.units = units; w.count = count; w.adapter = adapter; w.error = error; w.capacity = capacity;
    if (!sources || (count && !units) || !adapter || !adapter->resolve || !adapter->bind || !adapter->assign ||
        (adapter->registry && !adapter->registry->check))
        goto done;
    if (!pl_identify(&w) || !pl_members(&w) || !pl_assign(&w) ||
        !pl_project(&w, sources, policy, canonical_path)) goto done;
    *out = w.output; memset(&w.output, 0, sizeof(w.output)); ok = 1;
done:
    if (!ok && !*error) snprintf(error, capacity, "resource linker input is invalid or unavailable");
    pl_clear(&w); return ok;
}

int sh_package_link_provenance(const sh_package_link_output *linked,
    const sh_package_link_unit *units, size_t count,
    const sh_package_provenance *contexts, const size_t *unit_contexts,
    const size_t *roots, size_t root_count, sh_package_provenance *out,
    char *error, size_t capacity)
{
    sh_package_provenance result = {0}; unsigned char *selected = NULL; int ok = 0;
    const char *reason = "map binding selection does not match its linked source snapshot";
    if (error && capacity) error[0] = 0;
    if (!out || out == contexts) return 0;
    sh_package_provenance_free(out);
    if (!linked || !linked->compiled || !contexts || count != linked->classes.node_count ||
        (count && (!units || !unit_contexts || !linked->classes.nodes)) ||
        (linked->classes.count && (!linked->names || !linked->classes.representatives)) ||
        (root_count && !roots)) goto done;
    selected = calloc(linked->classes.count ? linked->classes.count : 1, 1);
    if (!selected) { reason = "cannot retain selected map binding classes"; goto done; }
    for (size_t i = 0; i < count; i++)
        if (linked->classes.nodes[i] >= linked->classes.count) goto done;
    for (size_t i = 0; i < root_count; i++) {
        if (roots[i] >= count) goto done;
        selected[linked->classes.nodes[roots[i]]] = 1;
    }
    for (size_t i = 0; i < count; i++) {
        size_t c = linked->classes.nodes[i], context = unit_contexts[i], key;
        const sh_package_provenance_context *source;
        if (!selected[c]) continue;
        if (!units[i].family || !units[i].name || !linked->names[c]) goto done;
        if (context == SIZE_MAX) {
            if (units[i].file_count || strcmp(units[i].name, linked->names[c])) goto done;
            continue;
        }
        if (context >= contexts->context_count || !contexts->contexts) goto done;
        source = contexts->contexts + context;
        key = sh_package_provenance_context_add(&result, source->mode, source->trees, source->tree_count, source->component);
        if (key == SIZE_MAX || !sh_package_provenance_bind(&result, units[i].family,
            units[i].name, linked->names[c], &key, 1)) goto done;
    }
    *out = result; memset(&result, 0, sizeof(result)); ok = 1;
done:
    free(selected); sh_package_provenance_free(&result);
    if (!ok && error && capacity) snprintf(error, capacity, "%s", reason);
    return ok;
}
