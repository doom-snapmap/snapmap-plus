#include "decl_native_registry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct dr_entry { char *name; uintptr_t address; } dr_entry;
typedef struct dr_manager {
    uintptr_t address, parent_address;
    size_t parent;
    char *type, *class_name;
    dr_entry *entries;
    size_t count;
    int captured;
} dr_manager;
struct sh_decl_native_registry {
    sh_decl_registry_source source;
    dr_manager *managers;
    size_t count;
};

static int dr_read(sh_decl_native_registry *r, uintptr_t address, size_t offset,
                   void *out, size_t length)
{
    return address && offset <= UINTPTR_MAX - address &&
        length <= UINTPTR_MAX - (address + offset) &&
        r->source.read(r->source.context, address + offset, out, length);
}
static char *dr_string(sh_decl_native_registry *r, uintptr_t address)
{
    char *text = NULL;
    size_t length = 0, capacity = 0;
    if (!address) return NULL;
    for (;;) {
        char chunk[64];
        size_t i, amount = sizeof(chunk);
        if (!dr_read(r, address, length, chunk, amount)) {
            amount = 1;
            if (!dr_read(r, address, length, chunk, amount)) break;
        }
        for (i = 0; i < amount && chunk[i]; i++) {}
        amount = i < amount ? i + 1 : amount;
        if (amount > SIZE_MAX - length) break;
        if (length + amount > capacity) {
            size_t next = capacity ? capacity : 64;
            char *grown;
            while (next < length + amount) {
                if (next > SIZE_MAX / 2) { next = length + amount; break; }
                next *= 2;
            }
            grown = (char *)realloc(text, next);
            if (!grown) break;
            text = grown; capacity = next;
        }
        memcpy(text + length, chunk, amount); length += amount;
        if (!text[length - 1]) return text;
    }
    free(text); return NULL;
}
static char *dr_member_string(sh_decl_native_registry *r, uintptr_t address, size_t offset)
{
    uintptr_t text;
    return dr_read(r, address, offset, &text, sizeof(text)) ? dr_string(r, text) : NULL;
}
static unsigned char dr_lower(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c; }
static int dr_equal(const char *a, const char *b)
{
    while (*a && dr_lower((unsigned char)*a) == dr_lower((unsigned char)*b)) { a++; b++; }
    return dr_lower((unsigned char)*a) == dr_lower((unsigned char)*b);
}
static char *dr_name(const char *name)
{
    size_t i, length = strlen(name);
    char *copy;
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        /* Non-ASCII native case conversion is not part of this adapter. */
        if (c >= 128) { free(copy); return NULL; }
        copy[i] = c == '\\' ? '/' : (char)dr_lower(c);
    }
    copy[length] = 0;
    /* Native normalization preserves a double-slash prefix. */
    if (length && copy[0] == '/' && copy[1] != '/') memmove(copy, copy + 1, length);
    return copy;
}
static int dr_capture(sh_decl_native_registry *r, dr_manager *m)
{
    uintptr_t array, after_array;
    int32_t count, after_count;
    size_t i;
    if (m->captured) return m->captured > 0;
    m->captured = -1;
    if (!dr_read(r, m->address, 0x20, &array, sizeof(array)) ||
        !dr_read(r, m->address, 0x28, &count, sizeof(count)) || count < 0 ||
        (count && !array) || (size_t)count > SIZE_MAX / sizeof(dr_entry)) return 0;
    m->entries = count ? (dr_entry *)calloc((size_t)count, sizeof(dr_entry)) : NULL;
    if (count && !m->entries) return 0;
    m->count = (size_t)count;
    for (i = 0; i < m->count; i++) {
        dr_entry *entry = &m->entries[i];
        if (!dr_read(r, array, i * sizeof(uintptr_t), &entry->address, sizeof(entry->address)) ||
            !entry->address) return 0;
        entry->name = dr_member_string(r, entry->address, 8);
        if (!entry->name) return 0;
    }
    if (!dr_read(r, m->address, 0x20, &after_array, sizeof(after_array)) ||
        !dr_read(r, m->address, 0x28, &after_count, sizeof(after_count)) ||
        array != after_array || count != after_count) return 0;
    m->captured = 1; return 1;
}
static int dr_loaded(sh_decl_native_registry *r, size_t manager, const char *name,
                      uintptr_t *resource)
{
    dr_manager *m = &r->managers[manager];
    size_t i;
    if (!dr_capture(r, m)) return -1;
    for (i = 0; i < m->count; i++) if (!strcmp(m->entries[i].name, name)) {
        *resource = m->entries[i].address; return 1;
    }
    return 0;
}
static int dr_descendant(sh_decl_native_registry *r, size_t child, size_t parent)
{
    size_t at = r->managers[child].parent;
    while (at != SIZE_MAX) {
        if (at == parent) return 1;
        at = r->managers[at].parent;
    }
    return 0;
}
static int dr_source(sh_decl_native_registry *r, size_t manager, const char *name)
{
    dr_manager *m = &r->managers[manager];
    int result = r->source.source_exists ?
        r->source.source_exists(r->source.context, m->address, m->type, name) : -1;
    return result > 0 ? 1 : result < 0 ? -1 : 0;
}
static int dr_lookup(sh_decl_native_registry *r, size_t manager, const char *name,
                      size_t *chosen, uintptr_t *resource)
{
    size_t i;
    int result = dr_loaded(r, manager, name, resource);
    *chosen = manager;
    if (result) return result;
    /* Explicit .decl paths have a separate native filesystem/default branch.
     * Existing identities above are usable; don't guess an unloaded identity. */
    {
        const char *suffix = strstr(name, ".decl");
        if (suffix && !suffix[5]) return -1;
    }
    for (i = 0; i < r->count; i++) if (dr_descendant(r, i, manager)) {
        result = dr_loaded(r, i, name, resource);
        if (result) { *chosen = i; return result; }
    }
    result = dr_source(r, manager, name);
    if (result) return result;
    for (i = 0; i < r->count; i++) if (dr_descendant(r, i, manager)) {
        result = dr_source(r, i, name);
        if (result) { *chosen = i; return result; }
    }
    return 0;
}

sh_decl_native_registry *sh_decl_native_registry_open(sh_decl_registry_source source,
    char *error, size_t error_capacity)
{
    sh_decl_native_registry *r;
    uintptr_t array, after_array;
    int32_t count, after_count;
    size_t i, j;
    if (error && error_capacity) error[0] = 0;
    r = (sh_decl_native_registry *)calloc(1, sizeof(*r));
    if (!r) goto failed;
    r->source = source;
    if (!source.read || !dr_read(r, source.registry, 8, &array, sizeof(array)) ||
        !dr_read(r, source.registry, 0x10, &count, sizeof(count)) || count < 0 ||
        (count && !array) || (size_t)count > SIZE_MAX / sizeof(dr_manager)) goto failed;
    r->managers = count ? (dr_manager *)calloc((size_t)count, sizeof(dr_manager)) : NULL;
    if (count && !r->managers) goto failed;
    r->count = (size_t)count;
    for (i = 0; i < r->count; i++) {
        dr_manager *m = &r->managers[i];
        if (!dr_read(r, array, i * sizeof(uintptr_t), &m->address, sizeof(m->address)) ||
            !m->address || !dr_read(r, m->address, 0x88, &m->parent_address,
                                    sizeof(m->parent_address))) goto failed;
        m->type = dr_member_string(r, m->address, 8);
        m->class_name = dr_member_string(r, m->address, 0x10);
        if (!m->type || !*m->type || !m->class_name || !*m->class_name) goto failed;
        for (j = 0; j < i; j++) if (r->managers[j].address == m->address) goto failed;
    }
    for (i = 0; i < r->count; i++) {
        dr_manager *m = &r->managers[i];
        m->parent = SIZE_MAX;
        if (!m->parent_address) continue;
        for (j = 0; j < r->count; j++) if (r->managers[j].address == m->parent_address) {
            m->parent = j; break;
        }
        if (m->parent == SIZE_MAX) goto failed;
    }
    for (i = 0; i < r->count; i++) {
        size_t at = i, traversed = 0;
        while (at != SIZE_MAX) {
            if (traversed++ == r->count) goto failed;
            at = r->managers[at].parent;
        }
    }
    if (!dr_read(r, source.registry, 8, &after_array, sizeof(after_array)) ||
        !dr_read(r, source.registry, 0x10, &after_count, sizeof(after_count)) ||
        array != after_array || count != after_count) goto failed;
    return r;
failed:
    if (error && error_capacity) snprintf(error, error_capacity,
        "Native declaration registry is unavailable, changing or malformed");
    sh_decl_native_registry_close(r); return NULL;
}
void sh_decl_native_registry_close(sh_decl_native_registry *r)
{
    size_t i, j;
    if (!r) return;
    for (i = 0; i < r->count; i++) {
        dr_manager *m = &r->managers[i];
        for (j = 0; j < m->count; j++) free(m->entries[j].name);
        free(m->entries); free(m->type); free(m->class_name);
    }
    free(r->managers); free(r);
}
int sh_decl_native_registry_type_class(sh_decl_native_registry *r,
    const char *type, const char **class_name)
{
    size_t i;
    const char *found = NULL;
    if (!class_name) return -1;
    *class_name = NULL;
    if (!r || !type || !*type) return -1;
    for (i = 0; i < r->count; i++) if (dr_equal(r->managers[i].type, type)) {
        if (found) return -1;
        found = r->managers[i].class_name;
    }
    *class_name = found; return found != NULL;
}

static int dr_lookup_source(sh_decl_native_registry *r, size_t manager,
    const char *name, size_t *chosen, uintptr_t *resource)
{
    size_t i;
    int result;
    const char *suffix = strstr(name, ".decl");
    *resource = 0; *chosen = manager;
    if (suffix && !suffix[5]) return -1;
    result = dr_source(r, manager, name);
    if (result) return result;
    for (i = 0; i < r->count; i++) if (dr_descendant(r, i, manager)) {
        result = dr_source(r, i, name);
        if (result) { *chosen = i; return result; }
    }
    return 0;
}

static int dr_resolve(sh_decl_native_registry *r,
    const char *class_name, const char *name, sh_decl_reference *reference,
    int source_only)
{
    size_t i, manager = SIZE_MAX, chosen = SIZE_MAX;
    uintptr_t resource = 0;
    char *canonical;
    int result = 0;
    if (!reference) return -1;
    memset(reference, 0, sizeof(*reference));
    if (!r || !class_name || !*class_name || !name) return -1;
    if (!*name) return 0;
    canonical = dr_name(name);
    if (!canonical) return -1;
    if (!*canonical) { free(canonical); return -1; }
    for (i = 0; i < r->count; i++) if (dr_equal(r->managers[i].class_name, class_name)) {
        manager = i; break;
    }
    if (manager != SIZE_MAX) result = source_only ?
        dr_lookup_source(r, manager, canonical, &chosen, &resource) :
        dr_lookup(r, manager, canonical, &chosen, &resource);
    else for (i = 0; i < r->count; i++) {
        result = source_only ? dr_lookup_source(r, i, canonical, &chosen, &resource) :
            dr_lookup(r, i, canonical, &chosen, &resource);
        if (result) break;
    }
    if (result > 0) {
        reference->type = r->managers[chosen].type;
        reference->name = canonical;
        reference->resource = resource;
    } else free(canonical);
    return result;
}
int sh_decl_native_registry_resolve(sh_decl_native_registry *r,
    const char *class_name, const char *name, sh_decl_reference *reference)
{ return dr_resolve(r, class_name, name, reference, 0); }

int sh_decl_native_registry_resolve_source(sh_decl_native_registry *r,
    const char *class_name, const char *name, sh_decl_reference *reference)
{ return dr_resolve(r, class_name, name, reference, 1); }

void sh_decl_reference_clear(sh_decl_reference *reference)
{
    if (reference) { free(reference->name); memset(reference, 0, sizeof(*reference)); }
}

int sh_decl_native_registry_entity_state(sh_decl_native_registry *r,
    const sh_decl_reference *reference, sh_decl_prepared_state *state)
{
    uintptr_t object, error, class_name, text, after_class, after_text;
    int32_t length, after_length;
    unsigned char flags, expanded, after_flags, after_expanded;
    if (!state) return 0;
    memset(state, 0, sizeof(*state));
    if (!r || !reference || !reference->type || !dr_equal(reference->type, "entityDef") ||
        !(object = reference->resource) ||
        !dr_read(r, object, 0x18, &error, sizeof(error)) || error ||
        !dr_read(r, object, 0x2c, &flags, 1) || (flags & 7) ||
        !dr_read(r, object, 0x60, &class_name, sizeof(class_name)) ||
        !dr_read(r, object, 0x128, &expanded, 1) ||
        !dr_read(r, object, 0x138, &length, sizeof(length)) || length <= 0 ||
        !dr_read(r, object, 0x140, &text, sizeof(text)) || !text) return 0;
    state->class_name = dr_string(r, class_name);
    state->text = (char *)malloc((size_t)length + 1);
    if (!state->class_name || !*state->class_name || !state->text ||
        !dr_read(r, text, 0, state->text, (size_t)length) ||
        !dr_read(r, object, 0x18, &error, sizeof(error)) || error ||
        !dr_read(r, object, 0x2c, &after_flags, 1) || after_flags != flags ||
        !dr_read(r, object, 0x60, &after_class, sizeof(after_class)) || after_class != class_name ||
        !dr_read(r, object, 0x128, &after_expanded, 1) || after_expanded != expanded ||
        !dr_read(r, object, 0x138, &after_length, sizeof(after_length)) || after_length != length ||
        !dr_read(r, object, 0x140, &after_text, sizeof(after_text)) || after_text != text) {
        sh_decl_prepared_state_clear(state); return 0;
    }
    state->text[length] = 0; state->length = (size_t)length;
    state->expanded_inheritance = expanded != 0; return 1;
}
void sh_decl_prepared_state_clear(sh_decl_prepared_state *state)
{
    if (state) { free(state->class_name); free(state->text); memset(state, 0, sizeof(*state)); }
}
