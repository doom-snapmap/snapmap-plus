#include "package_virtual_material.h"
#include "decl_native_lex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pv_table {
    char *path;
    size_t scope; /* SIZE_MAX is the captured original. */
    sh_virtual_material_table table;
} pv_table;
typedef struct pv_binding {
    size_t scope;
    char *material, *atlas, *source;
    const sh_virtual_material_record *record;
    unsigned char pages[32], semantics[32];
    int original, page_original, slot;
    sh_virtual_page_source *page_source;
    int page_kind, page_mode;
    sh_compiled_resource *row_input;
} pv_binding;
struct sh_package_virtual_materials {
    const sh_package_compilation **scopes;
    size_t count;
    sh_package_baseline_reader original;
    void *context;
    pv_table **tables; size_t table_count, table_capacity;
    pv_binding *bindings; size_t binding_count, binding_capacity;
    /* Open addressing over binding index + 1: by scope and stored material
     * name, and the first page-owning binding of each resolved atlas name. */
    size_t *binding_slots, binding_slot_count;
    size_t *atlas_slots, atlas_slot_count, atlas_count;
};
static uint64_t pv_hash(size_t scope, const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037) ^ (uint64_t)scope;
    for (; *text; text++) { hash ^= (unsigned char)*text; hash *= UINT64_C(1099511628211); }
    return hash;
}
/* Index + 1 of the binding of scope and material as stored, else 0: the row
 * a scan for the first equal scope and name would find. */
static size_t pv_binding_find(const sh_package_virtual_materials *m, size_t scope, const char *material)
{
    if (!m->binding_slot_count) return 0;
    size_t at = (size_t)pv_hash(scope, material) & (m->binding_slot_count - 1);
    for (; m->binding_slots[at]; at = (at + 1) & (m->binding_slot_count - 1)) {
        const pv_binding *b = m->bindings + m->binding_slots[at] - 1;
        if (b->scope == scope && !strcmp(b->material, material)) return m->binding_slots[at];
    }
    return 0;
}
/* Index + 1 of the first page-owning binding (a table row, no private pages)
 * of this resolved atlas name, else 0. */
static size_t pv_atlas_find(const sh_package_virtual_materials *m, const char *atlas)
{
    if (!m->atlas_slot_count) return 0;
    size_t at = (size_t)pv_hash(0, atlas) & (m->atlas_slot_count - 1);
    for (; m->atlas_slots[at]; at = (at + 1) & (m->atlas_slot_count - 1))
        if (!strcmp(m->bindings[m->atlas_slots[at] - 1].atlas, atlas)) return m->atlas_slots[at];
    return 0;
}
static void pv_index_insert(sh_package_virtual_materials *m, size_t i)
{
    const pv_binding *b = m->bindings + i;
    size_t at = (size_t)pv_hash(b->scope, b->material) & (m->binding_slot_count - 1);
    while (m->binding_slots[at]) at = (at + 1) & (m->binding_slot_count - 1);
    m->binding_slots[at] = i + 1;
    if (b->record && !b->page_source && !pv_atlas_find(m, b->atlas)) {
        at = (size_t)pv_hash(0, b->atlas) & (m->atlas_slot_count - 1);
        while (m->atlas_slots[at]) at = (at + 1) & (m->atlas_slot_count - 1);
        m->atlas_slots[at] = i + 1; m->atlas_count++;
    }
}
/* Index the binding just appended (index count - 1). A table past half full
 * doubles, and both indexes are rebuilt from the rows in order. */
static int pv_index(sh_package_virtual_materials *m)
{
    int rebuild = 0;
    if (m->binding_count > m->binding_slot_count / 2) {
        size_t next = m->binding_slot_count ? m->binding_slot_count * 2 : 256;
        size_t *grown = next < SIZE_MAX / sizeof(*grown) ? calloc(next, sizeof(*grown)) : NULL;
        if (!grown) return 0;
        free(m->binding_slots); m->binding_slots = grown; m->binding_slot_count = next; rebuild = 1;
    }
    if (m->atlas_count + 1 > m->atlas_slot_count / 2) {
        size_t next = m->atlas_slot_count ? m->atlas_slot_count * 2 : 16;
        size_t *grown = next < SIZE_MAX / sizeof(*grown) ? calloc(next, sizeof(*grown)) : NULL;
        if (!grown) return 0;
        free(m->atlas_slots); m->atlas_slots = grown; m->atlas_slot_count = next; rebuild = 1;
    }
    if (!rebuild) { pv_index_insert(m, m->binding_count - 1); return 1; }
    memset(m->binding_slots, 0, m->binding_slot_count * sizeof(*m->binding_slots));
    memset(m->atlas_slots, 0, m->atlas_slot_count * sizeof(*m->atlas_slots));
    m->atlas_count = 0;
    for (size_t i = 0; i < m->binding_count; i++) pv_index_insert(m, i);
    return 1;
}
static int pv_fail(char *error, size_t capacity, const char *reason, const char *name)
{
    if (error && capacity) snprintf(error, capacity, "%s%s%s", reason, name ? ": " : "", name ? name : "");
    return -1;
}
typedef struct pv_stream { const unsigned char *body; FILE *file; uint64_t length; } pv_stream;
static int pv_stream_read(void *context, uint64_t offset, void *bytes, size_t count)
{
    pv_stream *stream = context;
    if (offset > stream->length || count > stream->length - offset) return 0;
    if (stream->body) { memcpy(bytes, stream->body + (size_t)offset, count); return 1; }
    return stream->file && offset <= INT64_MAX && !_fseeki64(stream->file, (int64_t)offset, SEEK_SET) &&
        fread(bytes, 1, count, stream->file) == count;
}
int sh_package_virtual_material_auxiliary(const sh_package_compilation *scope,
    const sh_compiled_resource *file, char *error, size_t capacity)
{
    int kind = file ? sh_package_virtual_page_path(file->engine_path, NULL, NULL) : SH_VIRTUAL_PAGE_NONE;
    if (!kind) return 0;
    if (!scope || file->type || sh_package_resource_native_count(file))
        return pv_fail(error, capacity, "virtual texture companion has an unrelated manager identity", file->engine_path);
    if (kind == SH_VIRTUAL_PAGE_TABLE) {
        size_t length = 0; sh_virtual_material_table table = {0};
        unsigned char *body = sh_package_compilation_read(scope, file, SIZE_MAX, &length, error, capacity);
        int ok = body && sh_virtual_material_read((sh_decl_source){(char *)body, length}, &table, error, capacity);
        free(body); sh_virtual_material_free(&table); return ok ? 1 : -1;
    }
    pv_stream stream = {0}; sh_virtual_page_info info;
    if (file->body) { stream.body = file->body; stream.length = file->body_length; }
    else if (file->cache_file) stream.file = sh_package_file_open(file->cache_file, &stream.length, error, capacity);
    else if (scope->sources && file->source < scope->sources->file_count) {
        sh_package_source_file input = scope->sources->files[file->source];
        if (file->cache_path) input.absolute = file->cache_path;
        stream.length = input.length; stream.file = sh_package_source_open(&input, error, capacity);
    }
    int ok = (stream.body || stream.file) && sh_package_virtual_page_inspect(kind, stream.length,
        pv_stream_read, &stream, &info, error, capacity);
    if (stream.file) fclose(stream.file);
    if (!ok && (!error || !capacity || !*error)) pv_fail(error, capacity, "cannot capture virtual texture companion", file->engine_path);
    return ok ? 1 : -1;
}
static int pv_grow(void **items, size_t *capacity, size_t count, size_t stride)
{
    if (count < *capacity) return 1;
    size_t next = *capacity ? *capacity * 2 : 16;
    void *grown;
    if (next <= *capacity || next > SIZE_MAX / stride || !(grown = realloc(*items, next * stride))) return 0;
    *items = grown; *capacity = next; return 1;
}
static void pv_row_input_free(sh_compiled_resource *row)
{
    if (!row) return;
    free((void *)row->engine_path); free(row->body);
    sh_package_owners_free(&row->owners); sh_package_owners_free(&row->gameplay_owners); free(row);
}
char *sh_package_virtual_material_path(const char *source, unsigned slot)
{
    char *path = sh_package_engine_path(source), *out = NULL;
    if (!path || slot > 5) goto done;
    size_t n = strlen(path), end = n;
    for (size_t i = n; i && path[i - 1] != '/'; i--) if (path[i - 1] == '.') { end = i - 1; break; }
    if (!end || path[end - 1] == '/' || end > SIZE_MAX - 24 || !(out = malloc(end + 24))) goto done;
    memcpy(out, path, end);
    if (slot) snprintf(out + end, 24, "_dlc%u.vmtr", slot);
    else memcpy(out + end, ".vmtr", 6);
done:
    free(path); return out;
}
sh_package_virtual_materials *sh_package_virtual_material_open(
    const sh_package_compilation *const *scopes, size_t count,
    sh_package_baseline_reader original, void *context)
{
    if ((count && !scopes) || count > SIZE_MAX / sizeof(*scopes)) return NULL;
    sh_package_virtual_materials *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->scopes = calloc(count ? count : 1, sizeof(*m->scopes));
    if (!m->scopes) { free(m); return NULL; }
    for (size_t i = 0; i < count; i++) {
        if (!scopes[i]) { sh_package_virtual_material_close(m); return NULL; }
        m->scopes[i] = scopes[i];
    }
    m->count = count; m->original = original; m->context = context; return m;
}
void sh_package_virtual_material_close(sh_package_virtual_materials *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->table_count; i++) {
        pv_table *t = m->tables[i]; sh_virtual_material_free(&t->table); free(t->path); free(t);
    }
    for (size_t i = 0; i < m->binding_count; i++) {
        free(m->bindings[i].material); free(m->bindings[i].atlas); free(m->bindings[i].source);
        sh_virtual_page_source_release(m->bindings[i].page_source);
        pv_row_input_free(m->bindings[i].row_input);
    }
    free(m->tables); free(m->bindings); free(m->scopes);
    free(m->binding_slots); free(m->atlas_slots); free(m);
}
static pv_table *pv_read(sh_package_virtual_materials *m, size_t scope,
    const char *path, char *error, size_t capacity)
{
    const sh_compiled_resource *file = NULL;
    unsigned char *body = NULL; size_t length = 0;
    int status = 0;
    for (size_t i = 0; i < m->table_count; i++)
        if (m->tables[i]->scope == scope && !strcmp(m->tables[i]->path, path)) return m->tables[i];
    if (scope != SIZE_MAX) file = sh_package_compilation_find(m->scopes[scope], path);
    if (scope != SIZE_MAX && !file) return pv_read(m, SIZE_MAX, path, error, capacity);
    pv_table *t = calloc(1, sizeof(*t));
    if (!t || !(t->path = _strdup(path))) goto failed;
    t->scope = scope;
    if (file) {
        body = sh_package_compilation_read(m->scopes[scope], file, SIZE_MAX, &length, error, capacity);
        status = body ? 1 : -1;
    } else status = m->original ? m->original(m->context, path, &body, &length) : 0;
    if (status < 0 || status > 2 || (status && !body) || length == SIZE_MAX) {
        pv_fail(error, capacity, "virtual material source could not be captured", path); goto failed;
    }
    if (status && !sh_virtual_material_read((sh_decl_source){(char *)body, length}, &t->table, error, capacity)) goto failed;
    if (!pv_grow((void **)&m->tables, &m->table_capacity, m->table_count, sizeof(*m->tables))) goto failed;
    m->tables[m->table_count++] = t; free(body); return t;
failed:
    free(body);
    if (t) { sh_virtual_material_free(&t->table); free(t->path); free(t); }
    if (!error || !capacity || !*error) pv_fail(error, capacity, "cannot retain virtual material source", path);
    return NULL;
}
static int pv_row_equal(const sh_virtual_material_record *a, const sh_virtual_material_record *b)
{
    return a && b ? a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height &&
        a->flags == b->flags && a->timestamp == b->timestamp && a->checksum == b->checksum : a == b;
}
static int pv_lookup(sh_package_virtual_materials *m, size_t scope, const char *source,
    const char *material, const sh_virtual_material_record **record, int *slot, char *error, size_t capacity)
{
    *record = NULL; *slot = 0;
    for (unsigned i = 0; i < 6; i++) {
        char *path = sh_package_virtual_material_path(source, i);
        if (!path) return pv_fail(error, capacity, "invalid native atlas source path", source);
        pv_table *t = pv_read(m, scope, path, error, capacity); free(path);
        if (!t) return -1;
        const sh_virtual_material_record *r = sh_virtual_material_find(&t->table, material);
        if (r) { *record = r; *slot = (int)i; }
    }
    return 1;
}
static int pv_row_input(sh_package_virtual_materials *m, pv_binding *binding,
    char *error, size_t capacity)
{
    sh_compiled_resource *row = calloc(1, sizeof(*row));
    if (!row) return 0;
    binding->row_input = row; row->source = SIZE_MAX;
    row->engine_path = sh_package_virtual_material_path(binding->source, 0);
    row->body = calloc(1, 36); row->body_length = 36;
    if (!row->engine_path || !row->body) return 0;
    row->body[0] = binding->record != NULL;
    if (binding->record) {
        const sh_virtual_material_record *r = binding->record;
        int32_t values[] = {r->x,r->y,r->width,r->height,r->flags,r->timestamp,r->checksum,binding->slot};
        for (size_t i = 0; i < 8; i++) for (size_t j = 0; j < 4; j++)
            row->body[4 + i * 4 + j] = (unsigned char)((uint32_t)values[i] >> (j * 8));
    }
    for (unsigned i = 0; i < 6; i++) {
        char *path = sh_package_virtual_material_path(binding->source, i);
        const sh_compiled_resource *file = path ? sh_package_compilation_find(m->scopes[binding->scope], path) : NULL;
        pv_table *local = file ? pv_read(m, binding->scope, path, error, capacity) : NULL;
        pv_table *base = file ? pv_read(m, SIZE_MAX, path, error, capacity) : NULL;
        free(path);
        if (file && (!local || !base)) return 0;
        if (file && (sh_virtual_material_find(&local->table, binding->material) ||
            sh_virtual_material_find(&base->table, binding->material)) &&
            (!sh_package_owners_union(&row->owners, &file->owners) ||
             !sh_package_owners_union(&row->gameplay_owners, &file->gameplay_owners))) return 0;
    }
    return 1;
}
static int pv_digest(pv_binding *b)
{
    /* Explicit little-endian fields, with no structure padding/pointers or
     * unrelated table rows. The page key identifies the resolved data binding. */
    unsigned char fields[72] = {0}; sh_package_file_identity identity;
    if (b->record) {
        const sh_virtual_material_record *r = b->record;
        int32_t values[] = {r->x, r->y, r->width, r->height, r->flags, r->timestamp, r->checksum, b->slot};
        for (size_t i = 0; i < 8; i++) for (size_t j = 0; j < 4; j++)
            fields[i * 4 + j] = (unsigned char)((uint32_t)values[i] >> (j * 8));
    }
    if (b->record || b->page_source) memcpy(fields + 32, b->pages, 32);
    fields[64] = (unsigned char)b->page_kind; fields[65] = (unsigned char)b->page_mode;
    size_t name_length = b->record || b->page_source ? strlen(b->atlas) : 0;
    size_t field_size = b->page_source ? sizeof(fields) : 64;
    if (name_length > SIZE_MAX - field_size) return 0;
    unsigned char *bytes = malloc(field_size + name_length);
    if (!bytes) return 0;
    memcpy(bytes, fields, field_size);
    if (name_length) memcpy(bytes + field_size, b->atlas, name_length);
    int ok = sh_package_bytes_identity(bytes, field_size + name_length, &identity);
    free(bytes); if (!ok) return 0;
    memcpy(b->semantics, identity.digest, 32); return 1;
}
int sh_package_virtual_material_bind(sh_package_virtual_materials *m, size_t scope,
    const char *material, const sh_package_virtual_atlas *atlas,
    unsigned char semantics[32], int *original, char *error, size_t capacity)
{
    pv_binding b = {0}; const sh_virtual_material_record *base = NULL; int base_slot = 0;
    if (error && capacity) *error = 0;
    if (semantics) memset(semantics, 0, 32);
    if (original) *original = 0;
    if (!m || scope >= m->count || !material || !atlas || !semantics || !original ||
        !(b.material = sh_virtual_material_name(material, strlen(material))) ||
        !(b.atlas = sh_virtual_material_name(atlas->name, atlas->name ? strlen(atlas->name) : 0)) ||
        !(b.source = sh_package_engine_path(atlas->source))) goto failed;
    b.scope = scope; b.page_original = atlas->original; memcpy(b.pages, atlas->content, 32);
    b.page_source = sh_virtual_page_source_retain(atlas->pages);
    b.page_kind = atlas->page_kind; b.page_mode = atlas->page_mode;
    if (b.page_source && (b.page_original || (b.page_kind != 1 && b.page_kind != 2) ||
        b.page_mode < 0 || b.page_mode > 2)) goto failed;
    for (size_t found = pv_binding_find(m, scope, b.material); found; found = 0) {
        const pv_binding *old = m->bindings + found - 1;
        if (strcmp(old->atlas, b.atlas) || strcmp(old->source, b.source) ||
            memcmp(old->pages, b.pages, 32) || old->page_original != b.page_original ||
            old->page_kind != b.page_kind || old->page_mode != b.page_mode ||
            (old->page_source && b.page_source ?
                sh_virtual_page_source_equal(old->page_source, b.page_source, error, capacity) != 1 :
                old->page_source != b.page_source)) {
            pv_fail(error, capacity, "scoped material changed its resolved atlas", material); goto failed;
        }
        memcpy(semantics, old->semantics, 32); *original = old->original;
        free(b.material); free(b.atlas); free(b.source); sh_virtual_page_source_release(b.page_source); return old->record != NULL;
    }
    if (strncmp(b.material, "generic", 7)) {
        if (pv_lookup(m, scope, b.source, b.material, &b.record, &b.slot, error, capacity) < 0 ||
            pv_lookup(m, SIZE_MAX, b.source, b.material, &base, &base_slot, error, capacity) < 0) goto failed;
    }
    b.original = pv_row_equal(b.record, base) && b.slot == base_slot &&
        (!b.record || atlas->original) && !b.page_source;
    if ((!pv_row_equal(b.record, base) || b.slot != base_slot) && !pv_row_input(m, &b, error, capacity)) goto failed;
    /* Every earlier page-owning row of this atlas carries the same pages (a
     * row that disagreed was refused), so the first of them decides. */
    for (size_t found = b.record && !b.page_source ? pv_atlas_find(m, b.atlas) : 0; found; found = 0) {
        const pv_binding *old = m->bindings + found - 1;
        if (memcmp(old->pages, b.pages, 32)) {
            pv_fail(error, capacity, "resolved native atlas name has different page data", b.atlas); goto failed;
        }
    }
    if (!pv_digest(&b) || !pv_grow((void **)&m->bindings, &m->binding_capacity, m->binding_count, sizeof(*m->bindings))) goto failed;
    memcpy(semantics, b.semantics, 32); *original = b.original;
    m->bindings[m->binding_count++] = b;
    if (!pv_index(m)) {
        /* Keep the row owned by the table and report an environment failure. */
        pv_fail(error, capacity, "cannot index scoped virtual material", material); return -1;
    }
    return b.record != NULL;
failed:
    free(b.material); free(b.atlas); free(b.source); sh_virtual_page_source_release(b.page_source);
    pv_row_input_free(b.row_input);
    if (!error || !capacity || !*error) pv_fail(error, capacity, "cannot bind scoped virtual material", material);
    return -1;
}
int sh_package_virtual_material_cached(const sh_package_virtual_materials *m, size_t scope,
    const char *material, unsigned char semantics[32], int *original)
{
    if (!m || !material || !semantics || !original) return 0;
    size_t found = pv_binding_find(m, scope, material);
    if (!found) return 0;
    memcpy(semantics, m->bindings[found - 1].semantics, 32); *original = m->bindings[found - 1].original; return 1;
}
int sh_package_virtual_material_witnesses(sh_package_virtual_materials *m, size_t scope,
    const char *material, int (*visit)(void *, const char *, const sh_compiled_resource *),
    void *context, char *error, size_t capacity)
{
    if (!m || !material || !visit) return 0;
    for (size_t found = pv_binding_find(m, scope, material); found; found = 0) {
        const pv_binding *binding = m->bindings + found - 1;
        /* This witness is the decoded selected row, not the unrelated table's
         * full byte envelope. Its owners nominate changed rows during reverse
         * discovery without changing every other material in that table. */
        if (binding->row_input && !visit(context, "virtual-atlas/selected-row", binding->row_input)) return 0;
        for (unsigned slot = 0; binding->page_source && slot < 21; slot++) {
            const sh_compiled_resource *file = sh_virtual_page_source_member(binding->page_source, slot);
            char role[32];
            if (slot < 16) snprintf(role, sizeof(role), "virtual-pages/sq%u", slot + 1);
            else if (slot < 20) snprintf(role, sizeof(role), "virtual-pages/q%u", slot - 15);
            else snprintf(role, sizeof(role), "virtual-pages/pages");
            if (file && !visit(context, role, file)) return 0;
        }
        return 1;
    }
    pv_fail(error, capacity, "material companion source has not been captured", material); return 0;
}
int sh_package_virtual_material_seeds(sh_package_virtual_materials *m, size_t scope,
    int (*visit)(void *, const char *), void *context, char *error, size_t capacity)
{
    if (!m || scope >= m->count || !visit) return 0;
    const sh_package_compilation *compiled = m->scopes[scope];
    for (size_t f = 0; f < compiled->resource_count; f++) {
        char *source = NULL;
        int kind = sh_package_virtual_page_path(compiled->resources[f].engine_path, &source, NULL);
        if (kind < 0) return 0;
        if (!kind) continue;
        size_t length = strlen(source);
        if (kind == SH_VIRTUAL_PAGE_TABLE && length > 5 && !memcmp(source + length - 5, "_dlc", 4) &&
            source[length - 1] >= '1' && source[length - 1] <= '5') source[length - 5] = 0;
        int ok = 1;
        for (unsigned slot = 0; ok && slot < 6; slot++) {
            char *path = sh_package_virtual_material_path(source, slot);
            pv_table *local = path ? pv_read(m, scope, path, error, capacity) : NULL;
            pv_table *base = path ? pv_read(m, SIZE_MAX, path, error, capacity) : NULL;
            free(path);
            if (!local || !base) { ok = 0; break; }
            const pv_table *tables[] = {local, base};
            for (size_t t = 0; ok && t < 2; t++) for (size_t r = 0; ok && r < tables[t]->table.count; r++)
                ok = visit(context, tables[t]->table.records[r].name);
        }
        free(source); if (!ok) return 0;
    }
    return 1;
}
static int pv_resource_order(const void *left, const void *right)
{ return strcmp(((const sh_compiled_resource *)left)->engine_path, ((const sh_compiled_resource *)right)->engine_path); }
static char *pv_private_name(const pv_binding *binding)
{
    const char *origin = sh_virtual_page_source_origin(binding->page_source);
    size_t length = origin ? strlen(origin) : SIZE_MAX;
    sh_package_file_identity key;
    if (length > SIZE_MAX - 34) return NULL;
    unsigned char *bytes = malloc(length + 34);
    char *name = malloc(72);
    if (!bytes || !name) { free(bytes); free(name); return NULL; }
    memcpy(bytes, origin, length); memcpy(bytes + length, binding->pages, 32);
    bytes[length + 32] = (unsigned char)binding->page_kind;
    bytes[length + 33] = (unsigned char)binding->page_mode;
    int ok = sh_package_bytes_identity(bytes, length + 34, &key); free(bytes);
    if (!ok) { free(name); return NULL; }
    /* The engine records provider names in 32-byte fields: 31 characters,
     * 96 identity bits. Equal names still compare their page data exactly. */
    memcpy(name, "smp/vt/", 7);
    for (size_t i = 0; i < 12; i++) snprintf(name + 7 + i * 2, 3, "%02x", key.digest[i]);
    return name;
}
/* Native materials select their page provider by the top-level String parm
 * landpagefile, written as name<TAB>"value". Replace one existing top-level
 * value or insert it after the opening brace. Nested uses are unsupported. */
static unsigned char *pv_page_selection(const char *text, size_t length, const char *provider,
    size_t *out_length)
{
    sh_decl_tokens tokens = {0};
    size_t depth = 0, open = SIZE_MAX, value = SIZE_MAX;
    unsigned char *out = NULL;
    char insert[96];
    if (!sh_decl_native_lex((sh_decl_source){text, length}, &tokens, NULL, 0)) return NULL;
    for (size_t i = 0; i < tokens.count; i++) {
        const sh_decl_token *token = tokens.items + i;
        if (sh_decl_token_is(&tokens, i, "{")) { if (!depth++ && open == SIZE_MAX) open = i; continue; }
        if (sh_decl_token_is(&tokens, i, "}")) { if (depth) depth--; continue; }
        if (token->kind != SH_DECL_TOKEN_NAME || token->end - token->begin != 12 ||
            _strnicmp(text + token->begin, "landpagefile", 12)) continue;
        if (depth != 1 || value != SIZE_MAX || i + 1 >= tokens.count ||
            tokens.items[i + 1].kind != SH_DECL_TOKEN_STRING) goto done;
        value = i + 1;
    }
    if (open == SIZE_MAX) goto done;
    size_t begin, end;
    if (value != SIZE_MAX) {
        begin = tokens.items[value].begin; end = tokens.items[value].end;
        snprintf(insert, sizeof(insert), "\"%s\"", provider);
    } else {
        begin = end = tokens.items[open].end;
        snprintf(insert, sizeof(insert), "\n\tlandpagefile\t\"%s\"", provider);
    }
    size_t added = strlen(insert);
    if (length - (end - begin) > SIZE_MAX - added - 1 || !(out = malloc(length - (end - begin) + added + 1))) goto done;
    memcpy(out, text, begin); memcpy(out + begin, insert, added);
    memcpy(out + begin + added, text + end, length - end);
    *out_length = length - (end - begin) + added; out[*out_length] = 0;
done:
    sh_decl_tokens_free(&tokens);
    return out;
}
static int pv_select_pages(const sh_package_compilation *compiled, sh_compiled_resource *resource,
    const sh_virtual_material_runtime_binding *bindings, size_t used, const char *provider,
    char *error, size_t capacity)
{
    size_t length = 0, selected_length = 0;
    /* Every identity served by these bytes must select the same provider. */
    for (size_t k = 0; k < sh_package_resource_native_count(resource); k++) {
        sh_package_native_identity identity = sh_package_resource_native_at(resource, k);
        size_t b = 0;
        while (b < used && (!identity.name || strcmp(bindings[b].name, identity.name))) b++;
        if (!identity.type || strcmp(identity.type, "material") || b == used ||
            !bindings[b].pages || strcmp(bindings[b].atlas, provider)) {
            pv_fail(error, capacity, "private material bytes serve a different page selection", resource->engine_path);
            return 0;
        }
    }
    unsigned char *body = sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, error, capacity);
    unsigned char *selected = body ? pv_page_selection((const char *)body, length, provider, &selected_length) : NULL;
    free(body);
    if (!selected) { pv_fail(error, capacity, "private material cannot name its page provider", resource->engine_path); return 0; }
    free(resource->body); resource->body = selected; resource->body_length = selected_length;
    resource->native_original = 0;
    return 1;
}
static int pv_dependency(sh_compiled_resource *resource, const char *path)
{
    for (size_t i = 0; i < resource->bound_dependency_count; i++)
        if (!strcmp(resource->bound_dependencies[i], path)) return 1;
    if (resource->bound_dependency_count == SIZE_MAX / sizeof(char *)) return 0;
    char *copy = _strdup(path);
    char **grown = copy ? realloc(resource->bound_dependencies,
        (resource->bound_dependency_count + 1) * sizeof(char *)) : NULL;
    if (!grown) { free(copy); return 0; }
    resource->bound_dependencies = grown;
    grown[resource->bound_dependency_count++] = copy; return 1;
}
typedef struct pv_projection {
    sh_package_resource_projection projection;
    char *path;
} pv_projection;
static int pv_projection_add(pv_projection *projections, size_t *count, const char *path,
    const sh_compiled_resource *input, char *error, size_t capacity)
{
    pv_projection *target = NULL;
    for (size_t i = 0; i < *count; i++) if (!strcmp(projections[i].path, path)) { target = projections + i; break; }
    if (!target) {
        target = projections + (*count)++;
        target->path = _strdup(path); target->projection.engine_path = target->path;
        if (!target->path) return 0;
    } else if (strcmp(target->projection.inputs[0]->engine_path, input->engine_path))
        return pv_fail(error, capacity, "private page path has conflicting source provenance", path) == 1;
    for (size_t i = 0; i < target->projection.input_count; i++)
        if (target->projection.inputs[i] == input) return 1;
    size_t count_before = target->projection.input_count;
    if (count_before == SIZE_MAX / sizeof(input)) return 0;
    const sh_compiled_resource **grown = realloc((void *)target->projection.inputs,
        (count_before + 1) * sizeof(input));
    if (!grown) return 0;
    target->projection.inputs = grown; grown[count_before] = input;
    target->projection.input_count++; return 1;
}
sh_virtual_material_view *sh_package_virtual_material_prepare(sh_package_virtual_materials *m,
    const sh_package_link_inputs *inputs, sh_package_link_output *linked, char *error, size_t capacity)
{
    sh_virtual_material_runtime_binding *bindings = NULL;
    const pv_binding **captured = NULL;
    pv_projection *projections = NULL;
    size_t used = 0, projected = 0;
    sh_package_compilation *pages = NULL;
    sh_virtual_material_view *out = NULL;
    if (error && capacity) *error = 0;
    if (!m || !inputs || !linked || !linked->compiled || inputs->count != linked->classes.node_count ||
        (inputs->count && (!inputs->units || !inputs->scopes || !linked->classes.nodes)) ||
        (linked->classes.count && !linked->names) || inputs->count > SIZE_MAX / sizeof(*bindings) ||
        inputs->count > SIZE_MAX / 21 / sizeof(*projections)) goto failed;
    bindings = calloc(inputs->count ? inputs->count : 1, sizeof(*bindings));
    captured = calloc(inputs->count ? inputs->count : 1, sizeof(*captured));
    projections = calloc(inputs->count ? inputs->count * 21 : 1, sizeof(*projections));
    if (!bindings || !captured || !projections) goto failed;
    for (size_t i = 0; i < inputs->count; i++) {
        const sh_package_link_unit *unit = inputs->units + i;
        if (!unit->family || !unit->name || inputs->scopes[i] >= m->count) goto failed;
        if (strcmp(unit->family, "material")) continue;
        const pv_binding *found = NULL;
        size_t index = pv_binding_find(m, inputs->scopes[i], unit->name);
        if (index) found = m->bindings + index - 1;
        if (!found || linked->classes.nodes[i] >= linked->classes.count || memcmp(unit->semantics, found->semantics, 32)) {
            pv_fail(error, capacity, "linked material has no captured atlas binding", unit->name); goto failed;
        }
        const char *name = linked->names[linked->classes.nodes[i]];
        if (!name || !*name) goto failed;
        if ((!found->record && !found->page_source && !found->row_input) ||
            (found->original && !strcmp(name, unit->name))) continue;
        captured[used] = found;
        sh_virtual_material_runtime_binding *binding = bindings + used++;
        *binding = (sh_virtual_material_runtime_binding){name, found->atlas, found->record, found->slot};
        if (found->page_source) {
            char *private_name = pv_private_name(found);
            char **paths = calloc(21, sizeof(*paths));
            binding->atlas = private_name; binding->page_paths = (const char *const *)paths;
            binding->pages = found->page_source; binding->page_kind = found->page_kind; binding->page_mode = found->page_mode;
            if (!private_name || !paths) goto failed;
            char base[128]; snprintf(base, sizeof(base), "virtualtextures/%s", private_name);
            for (unsigned slot = 0; slot < 21; slot++) {
                const sh_compiled_resource *input = sh_virtual_page_source_member(found->page_source, slot);
                if (!input) continue;
                paths[slot] = sh_package_virtual_page_companion(base, slot);
                if (!paths[slot] || !pv_projection_add(projections, &projected, paths[slot], input, error, capacity)) goto failed;
            }
        }
    }
    if (projected) {
        sh_package_resource_projection *items = calloc(projected, sizeof(*items));
        if (!items) goto failed;
        for (size_t i = 0; i < projected; i++) items[i] = projections[i].projection;
        pages = sh_package_compilation_project(linked->compiled->sources, items, projected, NULL,
            linked->compiled->canonical_path, error, capacity); free(items);
        if (!pages) goto failed;
        size_t count = linked->compiled->resource_count;
        if (pages->resource_count > SIZE_MAX / sizeof(sh_compiled_resource) - count) goto failed;
        for (size_t i = 0; i < pages->resource_count; i++)
            if (sh_package_compilation_find(linked->compiled, pages->resources[i].engine_path)) {
                pv_fail(error, capacity, "private page output collides with a linked resource", pages->resources[i].engine_path); goto failed;
            }
        sh_compiled_resource *grown = realloc(linked->compiled->resources,
            (count + pages->resource_count) * sizeof(*grown));
        if (!grown) goto failed;
        linked->compiled->resources = grown;
        memcpy(grown + count, pages->resources, pages->resource_count * sizeof(*grown));
        linked->compiled->resource_count += pages->resource_count;
        free(pages->resources); pages->resources = NULL; pages->resource_count = 0;
        qsort(grown, linked->compiled->resource_count, sizeof(*grown), pv_resource_order);
    }
    for (size_t i = 0; i < used; i++) for (size_t j = 0; j < linked->compiled->resource_count; j++) {
        sh_compiled_resource *resource = linked->compiled->resources + j;
        if (!sh_package_resource_native_matches(resource, "material", bindings[i].name)) continue;
        if (captured[i]->row_input &&
            (!sh_package_owners_union(&resource->owners, &captured[i]->row_input->owners) ||
             !sh_package_owners_union(&resource->gameplay_owners, &captured[i]->row_input->gameplay_owners))) goto failed;
        for (size_t slot = 0; bindings[i].page_paths && slot < 21; slot++)
            if (bindings[i].page_paths[slot] && !pv_dependency(resource, bindings[i].page_paths[slot])) goto failed;
        if (bindings[i].pages && !pv_select_pages(linked->compiled, resource, bindings, used,
            bindings[i].atlas, error, capacity)) goto failed;
    }
    out = sh_virtual_material_runtime_prepare(bindings, used, error, capacity);
failed:
    sh_package_compilation_free(pages);
    for (size_t i = 0; i < used; i++) if (bindings[i].pages) {
        free((void *)bindings[i].atlas);
        for (size_t slot = 0; bindings[i].page_paths && slot < 21; slot++) free((void *)bindings[i].page_paths[slot]);
        free((void *)bindings[i].page_paths);
    }
    for (size_t i = 0; i < projected; i++) { free(projections[i].path); free((void *)projections[i].projection.inputs); }
    free(projections); free(captured);
    free(bindings);
    if (!out && (!error || !capacity || !*error)) pv_fail(error, capacity, "cannot prepare linked virtual material bindings", NULL);
    return out;
}
