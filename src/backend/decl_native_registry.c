#include "decl_native_registry.h"
#include "native_resource_id.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct dr_entry {
    char *name;
    uintptr_t address, name_address;
    uint32_t id;
} dr_entry;
typedef struct dr_manager {
    uintptr_t address, parent_address, next_address, array;
    size_t parent;
    char *type, *class_name;
    dr_entry *entries;
    size_t count;
    dr_entry **ids;
    size_t id_count;
    int captured, ids_captured;
} dr_manager;
struct sh_decl_native_registry {
    sh_decl_registry_source source;
    dr_manager *managers;
    size_t count;
    uintptr_t array;
    int linked, identity_failed;
};
struct sh_native_resource_registry { sh_decl_native_registry view; };

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
{ return sh_native_resource_name(name); }
static int dr_capture(sh_decl_native_registry *r, dr_manager *m)
{
    uintptr_t array, after_array;
    int32_t count, after_count;
    size_t i;
    if (m->captured) return m->captured > 0;
    m->captured = -1;
    if (!dr_read(r, m->address, 0x20, &array, sizeof(array)) ||
        !dr_read(r, m->address, 0x28, &count, sizeof(count)) || count < 0 ||
        (count && !array) || (size_t)count > SIZE_MAX / sizeof(dr_entry) ||
        (r->linked && count >= 262144)) return 0;
    m->entries = count ? (dr_entry *)calloc((size_t)count, sizeof(dr_entry)) : NULL;
    if (count && !m->entries) return 0;
    m->count = (size_t)count; m->array = array;
    for (i = 0; i < m->count; i++) {
        dr_entry *entry = &m->entries[i];
        if (!dr_read(r, array, i * sizeof(uintptr_t), &entry->address, sizeof(entry->address))) return 0;
        if (!entry->address) { if (r->linked) continue; return 0; }
        if (!dr_read(r, entry->address, 8, &entry->name_address, sizeof(entry->name_address))) return 0;
        entry->name = dr_string(r, entry->name_address);
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
    r->count = (size_t)count; r->array = array;
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
        free(m->entries); free(m->ids); free(m->type); free(m->class_name);
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

int sh_decl_native_registry_source_candidates(sh_decl_native_registry *r,
    const char *class_name, const char *name, sh_decl_source_candidate candidate, void *context)
{
    if (!r || !class_name || !*class_name || !name || !candidate) return 0;
    if (!*name) return 1;
    char *canonical = dr_name(name);
    if (!canonical || !*canonical) { free(canonical); return 0; }
    size_t manager = SIZE_MAX;
    for (size_t i = 0; i < r->count; i++) if (dr_equal(r->managers[i].class_name, class_name)) {
        manager = i; break;
    }
    int ok = 1;
    if (manager != SIZE_MAX) ok = candidate(context, r->managers[manager].type, canonical);
    for (size_t i = 0; ok && i < r->count; i++)
        if (manager == SIZE_MAX || dr_descendant(r, i, manager))
            ok = candidate(context, r->managers[i].type, canonical);
    free(canonical); return ok;
}

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

static int dr_identity_error(sh_decl_native_registry *r, char *error, size_t capacity, const char *reason)
{
    if (r) r->identity_failed = 1;
    if (error && capacity) snprintf(error, capacity, "%s", reason);
    return SH_NATIVE_RESOURCE_ID_ERROR;
}

static int dr_string_matches(sh_decl_native_registry *r, uintptr_t address, size_t offset, const char *expected)
{
    char *current = dr_member_string(r, address, offset);
    int equal = current && expected && !strcmp(current, expected);
    free(current); return equal;
}

static int dr_registry_header(sh_decl_native_registry *r)
{
    uintptr_t array;
    int32_t count;
    if (r->linked) return dr_read(r, r->source.registry, 0, &array, sizeof(array)) && array == r->array;
    return dr_read(r, r->source.registry, 8, &array, sizeof(array)) && array == r->array &&
        dr_read(r, r->source.registry, 0x10, &count, sizeof(count)) && count >= 0 && (size_t)count == r->count;
}

static int dr_identity_metadata(sh_decl_native_registry *r)
{
    if (!dr_registry_header(r)) return 0;
    for (size_t i = 0; i < r->count; i++) {
        dr_manager *m = r->managers + i;
        uintptr_t address;
        if (r->linked) {
            if ((i ? r->managers[i - 1].next_address : r->array) != m->address ||
                !dr_read(r, m->address, 0x18, &address, sizeof(address)) || address != m->next_address) return 0;
        } else {
            if (!dr_read(r, r->array, i * sizeof(uintptr_t), &address, sizeof(address)) || address != m->address ||
                !dr_read(r, m->address, 0x88, &address, sizeof(address)) || address != m->parent_address ||
                !dr_string_matches(r, m->address, 0x10, m->class_name)) return 0;
        }
        if (!dr_string_matches(r, m->address, 8, m->type)) return 0;
    }
    return (!r->linked || !r->count || !r->managers[r->count - 1].next_address) && dr_registry_header(r);
}

static int dr_identity_header(sh_decl_native_registry *r, dr_manager *m)
{
    uintptr_t array;
    int32_t count;
    return dr_registry_header(r) && dr_string_matches(r, m->address, 8, m->type) &&
        dr_read(r, m->address, 0x20, &array, sizeof(array)) && array == m->array &&
        dr_read(r, m->address, 0x28, &count, sizeof(count)) && count >= 0 && (size_t)count == m->count;
}

static int dr_identity_entry(sh_decl_native_registry *r, dr_manager *m, size_t index)
{
    dr_entry *entry = m->entries + index;
    uintptr_t address, name, owner;
    uint32_t id;
    if (!dr_read(r, m->array, index * sizeof(uintptr_t), &address, sizeof(address)) || address != entry->address) return 0;
    if (!address) return r->linked;
    if (!dr_read(r, address, 8, &name, sizeof(name)) || name != entry->name_address ||
        !dr_read(r, address, 0x10, &owner, sizeof(owner)) || owner != m->address ||
        !dr_read(r, address, 0x20, &id, sizeof(id)) || !id || id != entry->id ||
        !dr_string_matches(r, address, 8, entry->name)) return 0;
    /* Read the fields again after copying the name, so a mutation during that
     * read cannot certify metadata assembled from different object states. */
    return dr_read(r, address, 8, &name, sizeof(name)) && name == entry->name_address &&
        dr_read(r, address, 0x10, &owner, sizeof(owner)) && owner == m->address &&
        dr_read(r, address, 0x20, &id, sizeof(id)) && id == entry->id &&
        dr_read(r, m->array, index * sizeof(uintptr_t), &address, sizeof(address)) && address == entry->address;
}

static int dr_identity_entries(sh_decl_native_registry *r, dr_manager *m)
{
    if (!dr_identity_header(r, m)) return 0;
    for (size_t i = 0; i < m->count; i++) if (!dr_identity_entry(r, m, i)) return 0;
    return dr_identity_header(r, m);
}

static int dr_id_order(const void *left, const void *right)
{
    const dr_entry *a = *(dr_entry *const *)left, *b = *(dr_entry *const *)right;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return strcmp(a->name, b->name);
}

static int dr_capture_ids(sh_decl_native_registry *r, dr_manager *m)
{
    if (m->ids_captured) return m->ids_captured > 0;
    m->ids_captured = -1;
    if (!dr_capture(r, m) || m->count > SIZE_MAX / sizeof(*m->ids)) return 0;
    m->ids = calloc(m->count ? m->count : 1, sizeof(*m->ids));
    if (!m->ids) return 0;
    for (size_t i = 0; i < m->count; i++) {
        dr_entry *entry = m->entries + i;
        uint32_t expected;
        char *canonical;
        int valid;
        if (!entry->address) continue;
        canonical = sh_native_resource_name(entry->name);
        valid = canonical && !strcmp(canonical, entry->name);
        free(canonical);
        if (!valid || !sh_native_resource_id(m->type, entry->name, &expected) ||
            !dr_read(r, entry->address, 0x20, &entry->id, sizeof(entry->id)) || entry->id != expected) return 0;
        m->ids[m->id_count++] = entry;
    }
    if (!dr_identity_entries(r, m)) return 0;
    if (m->id_count > 1) qsort(m->ids, m->id_count, sizeof(*m->ids), dr_id_order);
    for (size_t i = 1; i < m->id_count; i++) if (!dr_id_order(m->ids + i - 1, m->ids + i)) return 0;
    m->ids_captured = 1; return 1;
}

void sh_native_resource_identity_clear(sh_native_resource_identity *identity)
{
    if (identity) { free(identity->name); memset(identity, 0, sizeof(*identity)); }
}

int sh_decl_native_registry_identity(sh_decl_native_registry *r,
    const char *family, const char *name, sh_native_resource_identity *out, char *error, size_t capacity)
{
    dr_manager *manager = NULL;
    dr_entry *same = NULL, *collision = NULL;
    sh_native_resource_identity result = {0};
    int status = SH_NATIVE_RESOURCE_ID_FREE;
    if (error && capacity) *error = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!r || r->identity_failed || !family || !*family || !name || !out)
        return dr_identity_error(r, error, capacity, "native resource identity snapshot is unavailable");
    for (size_t i = 0; i < r->count; i++) if (dr_equal(family, r->managers[i].type)) {
        if (manager) return dr_identity_error(r, error, capacity, "native resource family is ambiguous");
        manager = r->managers + i;
    }
    if (!manager) return SH_NATIVE_RESOURCE_ID_UNREGISTERED;
    if (!dr_capture_ids(r, manager) || !dr_identity_header(r, manager))
        return dr_identity_error(r, error, capacity, "native resource IDs are inconsistent, changing or unreadable");
    if (!(result.name = sh_native_resource_name(name)) || !sh_native_resource_id(manager->type, result.name, &result.id)) {
        sh_native_resource_identity_clear(&result);
        return dr_identity_error(r, error, capacity, "native resource name has unsupported identity semantics");
    }
    result.type = manager->type; result.manager = manager->address;
    size_t low = 0, high = manager->id_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (manager->ids[mid]->id < result.id) low = mid + 1; else high = mid;
    }
    for (size_t i = low; i < manager->id_count && manager->ids[i]->id == result.id; i++) {
        dr_entry *entry = manager->ids[i];
        if (!dr_identity_entry(r, manager, (size_t)(entry - manager->entries))) goto changed;
        if (!strcmp(entry->name, result.name)) same = entry;
        else if (!collision) collision = entry;
    }
    if (!dr_identity_header(r, manager)) goto changed;
    if (collision || same) {
        dr_entry *entry = collision ? collision : same;
        result.resource = entry->address; result.resident_id = entry->id; result.resident_name = entry->name;
        status = collision ? SH_NATIVE_RESOURCE_ID_COLLISION : SH_NATIVE_RESOURCE_ID_SAME_NAME;
    }
    *out = result; return status;
changed:
    sh_native_resource_identity_clear(&result);
    return dr_identity_error(r, error, capacity, "native resource identity snapshot changed during lookup");
}

int sh_decl_native_registry_identity_validate(sh_decl_native_registry *r, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!r || r->identity_failed || !dr_identity_metadata(r)) goto changed;
    for (size_t i = 0; i < r->count; i++) {
        dr_manager *manager = r->managers + i;
        if (manager->ids_captured < 0 || (manager->ids_captured && !dr_identity_entries(r, manager))) goto changed;
    }
    if (!dr_identity_metadata(r)) goto changed;
    return 1;
changed:
    dr_identity_error(r, error, capacity, "native resource identity snapshot changed or is unreadable"); return 0;
}

sh_native_resource_registry *sh_native_resource_registry_open(sh_native_resource_registry_source source,
    char *error, size_t capacity)
{
    sh_native_resource_registry *out = calloc(1, sizeof(*out));
    sh_decl_native_registry *r = out ? &out->view : NULL;
    uintptr_t address;
    if (error && capacity) *error = 0;
    if (!r || !source.read || !source.head_slot) goto failed;
    r->source = (sh_decl_registry_source){source.context, source.read, source.head_slot, NULL}; r->linked = 1;
    if (!dr_read(r, source.head_slot, 0, &address, sizeof(address))) goto failed;
    r->array = address;
    while (address) {
        dr_manager *grown, *manager;
        if (r->count == 4096 || r->count >= SIZE_MAX / sizeof(*r->managers)) goto failed;
        for (size_t i = 0; i < r->count; i++) if (r->managers[i].address == address) goto failed;
        grown = realloc(r->managers, (r->count + 1) * sizeof(*r->managers));
        if (!grown) goto failed;
        r->managers = grown; manager = r->managers + r->count++;
        memset(manager, 0, sizeof(*manager)); manager->address = address; manager->parent = SIZE_MAX;
        manager->type = dr_member_string(r, address, 8);
        if (!manager->type || !*manager->type || !dr_read(r, address, 0x18,
            &manager->next_address, sizeof(manager->next_address))) goto failed;
        address = manager->next_address;
    }
    if (!dr_identity_metadata(r)) goto failed;
    return out;
failed:
    if (error && capacity) snprintf(error, capacity, "native resource manager list is unavailable, changing or malformed");
    sh_native_resource_registry_close(out); return NULL;
}

void sh_native_resource_registry_close(sh_native_resource_registry *registry)
{ if (registry) sh_decl_native_registry_close(&registry->view); }

int sh_native_resource_registry_identity(sh_native_resource_registry *registry,
    const char *family, const char *name, sh_native_resource_identity *out, char *error, size_t capacity)
{
    return sh_decl_native_registry_identity(registry ? &registry->view : NULL, family, name, out, error, capacity);
}

int sh_native_resource_registry_validate(sh_native_resource_registry *registry, char *error, size_t capacity)
{ return sh_decl_native_registry_identity_validate(registry ? &registry->view : NULL, error, capacity); }
