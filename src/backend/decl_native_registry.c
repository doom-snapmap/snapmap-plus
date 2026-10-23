#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
    /* Entries by exact name: bucket heads and chains of entry indices plus
     * one, each chain in increasing index, so the first equal entry in it is
     * the first in the native array. Built with the capture. */
    size_t *buckets, *chain, bucket_count;
} dr_manager;
struct sh_decl_native_registry {
    sh_decl_registry_source source;
    dr_manager *managers;
    size_t count;
    uintptr_t array;
    int linked, identity_failed;
    /* Manager indices ordered by case-insensitive type and class name, then
     * index (dr_range); fixed with the manager list at open. Linked
     * registries read no class names. */
    size_t *by_type, *by_class;
};
struct sh_native_resource_registry { sh_decl_native_registry view; };
/* Work since the last reset, for a caller's timing line (diagnostics only:
 * plain counters, read and reset on the thread that does the work). */
static sh_decl_native_registry_stats g_dr_stats;
static size_t dr_hash(const char *name)
{
    size_t h = (size_t)14695981039346656037ull;
    for (; *name; name++) h = (h ^ (unsigned char)*name) * 1099511628211ull;
    return h;
}
/* Source existence answers inside a settled scope (sh_decl_native_registry_scope):
 * the engine state they read does not change until it ends, so each manager
 * and name is asked once, whichever registry asks. */
typedef struct dr_answer { uintptr_t manager; char *name; int result; size_t next; } dr_answer;
static struct { int depth; dr_answer *rows; size_t count, capacity, *buckets, bucket_count; } g_dr_scope;
static void dr_scope_clear(void)
{
    for (size_t i = 0; i < g_dr_scope.count; i++) free(g_dr_scope.rows[i].name);
    free(g_dr_scope.rows); free(g_dr_scope.buckets); memset(&g_dr_scope, 0, sizeof(g_dr_scope));
}
static void dr_resolved_clear(void);
void sh_decl_native_registry_scope(int enter)
{
    if (enter) { g_dr_scope.depth++; return; }
    if (g_dr_scope.depth > 0 && --g_dr_scope.depth == 0) { dr_scope_clear(); dr_resolved_clear(); }
}
static dr_answer *dr_scope_find(uintptr_t manager, const char *name, size_t key)
{
    for (size_t at = g_dr_scope.bucket_count ? g_dr_scope.buckets[key & (g_dr_scope.bucket_count - 1)] : 0; at;
         at = g_dr_scope.rows[at - 1].next)
        if (g_dr_scope.rows[at - 1].manager == manager && !strcmp(g_dr_scope.rows[at - 1].name, name))
            return g_dr_scope.rows + (at - 1);
    return NULL;
}
/* Records an answer; a row that cannot be kept is simply asked again. */
static void dr_scope_keep(uintptr_t manager, const char *name, size_t key, int result)
{
    if (g_dr_scope.count == g_dr_scope.capacity) {
        size_t capacity = g_dr_scope.capacity ? g_dr_scope.capacity * 2 : 256, buckets = capacity * 2;
        dr_answer *rows = capacity <= SIZE_MAX / sizeof(*rows) / 2 ? (dr_answer *)realloc(g_dr_scope.rows,
            capacity * sizeof(*rows)) : NULL;
        if (!rows) return;
        g_dr_scope.rows = rows;
        size_t *heads = (size_t *)calloc(buckets, sizeof(*heads));
        if (!heads) return;
        for (size_t i = 0; i < g_dr_scope.count; i++) {
            size_t b = dr_hash(g_dr_scope.rows[i].name) ^ (size_t)g_dr_scope.rows[i].manager;
            b &= buckets - 1;
            g_dr_scope.rows[i].next = heads[b]; heads[b] = i + 1;
        }
        free(g_dr_scope.buckets);
        g_dr_scope.buckets = heads; g_dr_scope.bucket_count = buckets; g_dr_scope.capacity = capacity;
    }
    char *copy = (char *)malloc(strlen(name) + 1);
    if (!copy) return;
    strcpy(copy, name);
    size_t b = key & (g_dr_scope.bucket_count - 1);
    g_dr_scope.rows[g_dr_scope.count] = (dr_answer){manager, copy, result, g_dr_scope.buckets[b]};
    g_dr_scope.buckets[b] = ++g_dr_scope.count;
}
sh_decl_native_registry_stats *sh_decl_native_registry_counters(void) { return &g_dr_stats; }
static double dr_now(void)
{
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
/* Resolutions inside a settled scope over an engine-only source: the class,
 * the canonical name and the mode decide the manager and object an engine
 * registry answers with, so the same registry asked again answers alike. */
typedef struct dr_resolved {
    uintptr_t registry; char *class_name, *name; int source_only, result; size_t chosen; uintptr_t resource; size_t next;
} dr_resolved;
static struct { dr_resolved *rows; size_t count, capacity, *buckets, bucket_count; } g_dr_resolved;
static void dr_resolved_clear(void)
{
    for (size_t i = 0; i < g_dr_resolved.count; i++) { free(g_dr_resolved.rows[i].class_name); free(g_dr_resolved.rows[i].name); }
    free(g_dr_resolved.rows); free(g_dr_resolved.buckets); memset(&g_dr_resolved, 0, sizeof(g_dr_resolved));
}
static size_t dr_resolved_key(uintptr_t registry, const char *class_name, const char *name, int source_only)
{
    size_t h = (size_t)14695981039346656037ull ^ (size_t)registry ^ (size_t)source_only;
    for (const char *at = class_name; *at; at++) h = (h ^ (unsigned char)*at) * 1099511628211ull;
    h = (h ^ 0xffu) * 1099511628211ull;
    for (const char *at = name; *at; at++) h = (h ^ (unsigned char)*at) * 1099511628211ull;
    return h;
}
static const dr_resolved *dr_resolved_find(uintptr_t registry, const char *class_name, const char *name,
    int source_only, size_t key)
{
    for (size_t at = g_dr_resolved.bucket_count ? g_dr_resolved.buckets[key & (g_dr_resolved.bucket_count - 1)] : 0; at;
         at = g_dr_resolved.rows[at - 1].next) {
        const dr_resolved *row = g_dr_resolved.rows + (at - 1);
        if (row->registry == registry && row->source_only == source_only && !strcmp(row->class_name, class_name) &&
            !strcmp(row->name, name)) return row;
    }
    return NULL;
}
/* Keeps a resolution; one that cannot be kept is resolved again. */
static void dr_resolved_keep(uintptr_t registry, const char *class_name, const char *name, int source_only,
    size_t key, int result, size_t chosen, uintptr_t resource)
{
    if (g_dr_resolved.count == g_dr_resolved.capacity) {
        size_t capacity = g_dr_resolved.capacity ? g_dr_resolved.capacity * 2 : 256, buckets = capacity * 2;
        dr_resolved *rows = capacity <= SIZE_MAX / sizeof(*rows) / 2 ? (dr_resolved *)realloc(g_dr_resolved.rows,
            capacity * sizeof(*rows)) : NULL;
        if (!rows) return;
        g_dr_resolved.rows = rows;
        size_t *heads = (size_t *)calloc(buckets, sizeof(*heads));
        if (!heads) return;
        for (size_t i = 0; i < g_dr_resolved.count; i++) {
            const dr_resolved *row = g_dr_resolved.rows + i;
            size_t b = dr_resolved_key(row->registry, row->class_name, row->name, row->source_only) & (buckets - 1);
            g_dr_resolved.rows[i].next = heads[b]; heads[b] = i + 1;
        }
        free(g_dr_resolved.buckets);
        g_dr_resolved.buckets = heads; g_dr_resolved.bucket_count = buckets; g_dr_resolved.capacity = capacity;
    }
    char *klass = (char *)malloc(strlen(class_name) + 1), *copy = (char *)malloc(strlen(name) + 1);
    if (!klass || !copy) { free(klass); free(copy); return; }
    strcpy(klass, class_name); strcpy(copy, name);
    size_t b = key & (g_dr_resolved.bucket_count - 1);
    g_dr_resolved.rows[g_dr_resolved.count] = (dr_resolved){registry, klass, copy, source_only, result, chosen, resource,
        g_dr_resolved.buckets[b]};
    g_dr_resolved.buckets[b] = ++g_dr_resolved.count;
}
void sh_decl_native_registry_stats_take(sh_decl_native_registry_stats *out, int reset)
{
    if (out) *out = g_dr_stats;
    if (reset) memset(&g_dr_stats, 0, sizeof(g_dr_stats));
}

static int dr_read(sh_decl_native_registry *r, uintptr_t address, size_t offset,
                   void *out, size_t length)
{
    return address && offset <= UINTPTR_MAX - address &&
        length <= UINTPTR_MAX - (address + offset) &&
        r->source.read(r->source.context, address + offset, out, length);
}
/* A native string's storage ends at its terminator, and the source copies
 * whatever is requested from mapped memory, so a string is read one byte at a
 * time up to and including its terminator, never past it. A null address has
 * no string (NULL, unlike ""); a byte that cannot be read before the
 * terminator fails the whole string. */
static char *dr_string(sh_decl_native_registry *r, uintptr_t address)
{
    char *text = NULL;
    size_t length = 0, capacity = 0;
    if (!address) return NULL;
    for (;;) {
        char c;
        if (!dr_read(r, address, length, &c, 1)) break;
        if (length == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            char *grown;
            if (next < capacity || !(grown = (char *)realloc(text, next))) break;
            text = grown; capacity = next;
        }
        text[length++] = c;
        if (!c) return text;
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
static int dr_icmp(const char *a, const char *b)
{
    while (*a && dr_lower((unsigned char)*a) == dr_lower((unsigned char)*b)) { a++; b++; }
    return (int)dr_lower((unsigned char)*a) - (int)dr_lower((unsigned char)*b);
}
typedef struct dr_order { const sh_decl_native_registry *r; int by_class; } dr_order;
static const char *dr_key(const sh_decl_native_registry *r, size_t i, int by_class)
{ return by_class ? r->managers[i].class_name : r->managers[i].type; }
static int dr_index_order(void *context, const void *left, const void *right)
{
    const dr_order *o = context;
    size_t a = *(const size_t *)left, b = *(const size_t *)right;
    int order = dr_icmp(dr_key(o->r, a, o->by_class), dr_key(o->r, b, o->by_class));
    return order ? order : a < b ? -1 : a > b;
}
/* Lookups by name are case-insensitive over a fixed manager list; order it
 * once so each lookup is a binary search rather than a scan. */
static int dr_index(sh_decl_native_registry *r)
{
    for (int by_class = 0; by_class < 2 && !(by_class && r->linked); by_class++) {
        size_t *index = malloc((r->count ? r->count : 1) * sizeof(*index));
        dr_order order = {r, by_class};
        if (!index) return 0;
        for (size_t i = 0; i < r->count; i++) index[i] = i;
        if (r->count > 1) qsort_s(index, r->count, sizeof(*index), dr_index_order, &order);
        if (by_class) r->by_class = index; else r->by_type = index;
    }
    return 1;
}
/* The managers whose type (or class name) equals key, case-insensitively:
 * index[begin, *end) in registry order. */
static size_t dr_range(const sh_decl_native_registry *r, int by_class, const char *key, size_t *end)
{
    const size_t *index = by_class ? r->by_class : r->by_type;
    size_t low = 0, high = r->count, begin;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (dr_icmp(dr_key(r, index[mid], by_class), key) < 0) low = mid + 1; else high = mid;
    }
    begin = low; high = r->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (dr_icmp(dr_key(r, index[mid], by_class), key) <= 0) low = mid + 1; else high = mid;
    }
    *end = low; return begin;
}
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
    g_dr_stats.captures++; g_dr_stats.captured += m->count;
    for (i = 0; i < m->count; i++) {
        dr_entry *entry = &m->entries[i];
        if (!dr_read(r, array, i * sizeof(uintptr_t), &entry->address, sizeof(entry->address))) return 0;
        if (!entry->address) { if (r->linked) continue; return 0; }
        if (!dr_read(r, entry->address, 8, &entry->name_address, sizeof(entry->name_address))) return 0;
        entry->name = dr_string(r, entry->name_address);
        if (!entry->name) return 0;
    }
    if (m->count) {
        size_t buckets = 1;
        while (buckets < m->count * 2) buckets <<= 1;
        m->buckets = (size_t *)calloc(buckets, sizeof(*m->buckets));
        m->chain = (size_t *)malloc(m->count * sizeof(*m->chain));
        if (!m->buckets || !m->chain) return 0;
        m->bucket_count = buckets;
        for (i = m->count; i-- > 0;) {
            if (!m->entries[i].name) continue;
            size_t b = dr_hash(m->entries[i].name) & (buckets - 1);
            m->chain[i] = m->buckets[b]; m->buckets[b] = i + 1;
        }
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
    g_dr_stats.scans++;
    if (!m->bucket_count) return 0;
    for (i = m->buckets[dr_hash(name) & (m->bucket_count - 1)]; i; i = m->chain[i - 1])
        if ((g_dr_stats.compared++, !strcmp(m->entries[i - 1].name, name))) {
            *resource = m->entries[i - 1].address; return 1;
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
    size_t key = 0;
    if (g_dr_scope.depth && r->source.source_exists && r->source.settled_exists) {
        key = dr_hash(name) ^ (size_t)m->address;
        const dr_answer *known = dr_scope_find(m->address, name, key);
        if (known) { g_dr_stats.probe_hits++; return known->result; }
    }
    g_dr_stats.probes++;
    double began = dr_now();
    int result = r->source.source_exists ?
        r->source.source_exists(r->source.context, m->address, m->type, name) : -1;
    g_dr_stats.probe_ms += dr_now() - began;
    result = result > 0 ? 1 : result < 0 ? -1 : 0;
    /* An unavailable answer may not hold; it is asked again. */
    if (g_dr_scope.depth && r->source.source_exists && r->source.settled_exists && result >= 0)
        dr_scope_keep(m->address, name, key, result);
    return result;
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
    g_dr_stats.opens++;
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
        array != after_array || count != after_count || !dr_index(r)) goto failed;
    return r;
failed:
    if (error && error_capacity) snprintf(error, error_capacity,
        "Native declaration registry is unavailable, changing or malformed");
    sh_decl_native_registry_close(r); return NULL;
}
int sh_decl_native_registry_digest(const sh_decl_native_registry *r,
    int (*emit)(void *context, const void *bytes, size_t length), void *context)
{
    uint64_t count;
    if (!r || !emit) return 0;
    count = r->count;
    if (!emit(context, &count, sizeof(count))) return 0;
    for (size_t i = 0; i < r->count; i++) {
        const dr_manager *m = &r->managers[i];
        const char *type = m->type ? m->type : "", *class_name = m->class_name ? m->class_name : "";
        uint64_t parent = m->parent == SIZE_MAX ? UINT64_MAX : (uint64_t)m->parent;
        if (!emit(context, type, strlen(type) + 1) || !emit(context, class_name, strlen(class_name) + 1) ||
            !emit(context, &parent, sizeof(parent))) return 0;
    }
    return 1;
}
void sh_decl_native_registry_close(sh_decl_native_registry *r)
{
    size_t i, j;
    if (!r) return;
    for (i = 0; i < r->count; i++) {
        dr_manager *m = &r->managers[i];
        for (j = 0; j < m->count; j++) free(m->entries[j].name);
        free(m->buckets); free(m->chain);
        free(m->entries); free(m->ids); free(m->type); free(m->class_name);
    }
    free(r->managers); free(r->by_type); free(r->by_class); free(r);
}
int sh_decl_native_registry_type_class(sh_decl_native_registry *r,
    const char *type, const char **class_name)
{
    size_t i;
    size_t end;
    if (!class_name) return -1;
    *class_name = NULL;
    if (!r || !type || !*type) return -1;
    i = dr_range(r, 0, type, &end);
    if (end - i > 1) return -1;
    *class_name = i < end ? r->managers[r->by_type[i]].class_name : NULL; return *class_name != NULL;
}

int sh_decl_native_registry_class_type(sh_decl_native_registry *r,
    const char *class_name, const char **type)
{
    if (!type) return -1;
    *type = NULL;
    if (!r || !class_name || !*class_name) return -1;
    size_t end, i = dr_range(r, 1, class_name, &end);
    if (i < end) { *type = r->managers[r->by_class[i]].type; return 1; }
    return 0;
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

static int dr_resolve_now(sh_decl_native_registry *r,
    const char *class_name, const char *name, sh_decl_reference *reference,
    int source_only);
static int dr_resolve(sh_decl_native_registry *r,
    const char *class_name, const char *name, sh_decl_reference *reference,
    int source_only)
{
    double began = dr_now();
    int cached = g_dr_scope.depth && r && r->source.settled_exists && class_name && *class_name && name && *name &&
        reference;
    size_t key = 0;
    if (cached) {
        char *canonical = dr_name(name);
        if (!canonical || !*canonical) { free(canonical); cached = 0; }
        else {
            key = dr_resolved_key(r->source.registry, class_name, canonical, source_only);
            const dr_resolved *row = dr_resolved_find(r->source.registry, class_name, canonical, source_only, key);
            if (row && (row->result <= 0 || row->chosen < r->count)) {
                memset(reference, 0, sizeof(*reference));
                g_dr_stats.resolves++; g_dr_stats.resolve_hits++;
                if (row->result > 0) {
                    reference->type = r->managers[row->chosen].type;
                    reference->name = canonical; reference->resource = row->resource;
                } else free(canonical);
                g_dr_stats.resolve_ms += dr_now() - began;
                return row->result;
            }
            free(canonical);
        }
    }
    int result = dr_resolve_now(r, class_name, name, reference, source_only);
    if (cached && result >= 0) {
        char *canonical = dr_name(name);
        size_t chosen = SIZE_MAX;
        if (result > 0)
            for (size_t i = 0; i < r->count; i++) if (r->managers[i].type == reference->type) { chosen = i; break; }
        if (canonical && (result == 0 || chosen != SIZE_MAX))
            dr_resolved_keep(r->source.registry, class_name, canonical, source_only, key, result, chosen,
                result > 0 ? reference->resource : 0);
        free(canonical);
    }
    g_dr_stats.resolve_ms += dr_now() - began;
    return result;
}
static int dr_resolve_now(sh_decl_native_registry *r,
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
    g_dr_stats.resolves++;
    if (!*name) return 0;
    canonical = dr_name(name);
    if (!canonical) return -1;
    if (!*canonical) { free(canonical); return -1; }
    {
        size_t end, first = dr_range(r, 1, class_name, &end);
        if (first < end) manager = r->by_class[first];
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
    size_t manager = SIZE_MAX, end, first = dr_range(r, 1, class_name, &end);
    if (first < end) manager = r->by_class[first];
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
        if (!entry->name[0]) {
            /* The engine creates some resources unnamed (a font request with
             * no face); none can be a package name, but its ID still occupies
             * the native table, so it stays in the collision index. */
            valid = sh_native_resource_unnamed_id(m->type, &expected);
        } else {
            canonical = sh_native_resource_name(entry->name);
            valid = canonical && !strcmp(canonical, entry->name) && sh_native_resource_id(m->type, entry->name, &expected);
            free(canonical);
        }
        if (!valid || !dr_read(r, entry->address, 0x20, &entry->id, sizeof(entry->id)) || entry->id != expected) return 0;
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
    size_t end, first = dr_range(r, 0, family, &end);
    if (end - first > 1) return dr_identity_error(r, error, capacity, "native resource family is ambiguous");
    if (first == end) return SH_NATIVE_RESOURCE_ID_UNREGISTERED;
    manager = r->managers + r->by_type[first];
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

int sh_decl_native_registry_loaded_at(sh_decl_native_registry *r,
    const char *family, size_t index, sh_decl_reference *out, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!r || r->identity_failed || !family || !*family || !out || !dr_identity_metadata(r))
        return dr_identity_error(r, error, capacity, "native loaded declaration snapshot is unavailable");
    size_t end, first = dr_range(r, 0, family, &end);
    if (end - first > 1) return dr_identity_error(r, error, capacity, "native declaration family is ambiguous");
    if (first == end) return 0;
    dr_manager *manager = r->managers + r->by_type[first];
    if (!dr_capture_ids(r, manager) || !dr_identity_header(r, manager))
        return dr_identity_error(r, error, capacity, "native loaded declaration membership is unreadable or changing");
    if (index >= manager->count) return 0;
    dr_entry *entry = manager->entries + index;
    if (!entry->address || !dr_identity_entry(r, manager, index))
        return dr_identity_error(r, error, capacity, "native loaded declaration identity changed");
    char *name = _strdup(entry->name);
    if (!name || !dr_identity_entry(r, manager, index) || !dr_identity_header(r, manager) || !dr_identity_metadata(r)) {
        free(name);
        return dr_identity_error(r, error, capacity, "native loaded declaration identity cannot be retained");
    }
    *out = (sh_decl_reference){manager->type, name, entry->address}; return 1;
}

/* The manager header (array and count) and every entry sharing the identity's
 * ID are exactly those an identity query read. Registration and removal change
 * the header, so this bounded check can stand in for a full revalidation
 * between queries of one stable snapshot; the full check still ends its use. */
static int dr_identity_bucket(sh_decl_native_registry *r, const sh_native_resource_identity *identity)
{
    dr_manager *m = NULL;
    for (size_t i = 0; i < r->count; i++) if (r->managers[i].address == identity->manager) { m = r->managers + i; break; }
    if (!m || m->ids_captured <= 0 || !dr_identity_header(r, m)) return 0;
    size_t low = 0, high = m->id_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (m->ids[mid]->id < identity->id) low = mid + 1; else high = mid;
    }
    for (size_t i = low; i < m->id_count && m->ids[i]->id == identity->id; i++)
        if (!dr_identity_entry(r, m, (size_t)(m->ids[i] - m->entries))) return 0;
    return dr_identity_header(r, m);
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
    if (!dr_identity_metadata(r) || !dr_index(r)) goto failed;
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

int sh_native_resource_registry_validate_identity(sh_native_resource_registry *registry,
    const sh_native_resource_identity *identity, char *error, size_t capacity)
{
    sh_decl_native_registry *r = registry ? &registry->view : NULL;
    if (error && capacity) *error = 0;
    if (r && !r->identity_failed && identity && identity->manager && dr_identity_bucket(r, identity)) return 1;
    dr_identity_error(r, error, capacity, "native resource identity changed or is unreadable"); return 0;
}
