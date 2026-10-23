#include "package_warm.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A tripwire, not schema protection: a field added to a published struct usually changes its size, which
 * stops this build until the encoder, the decoder and the canonical texts below name it. A same-size change
 * (a replaced or swapped field) passes; the explicit field lists and the exact comparison of every rebuilt
 * output are the actual protection. */
#if defined(_WIN64)
typedef char pw_resource_fields[sizeof(sh_compiled_resource) == 344 ? 1 : -1];
typedef char pw_compilation_fields[sizeof(sh_package_compilation) == 224 ? 1 : -1];
typedef char pw_policy_fields[sizeof(sh_package_policy) == 136 ? 1 : -1];
typedef char pw_provenance_fields[sizeof(sh_package_provenance) == 56 ? 1 : -1];
typedef char pw_provenance_use_fields[sizeof(sh_package_provenance_use) == 56 ? 1 : -1];
typedef char pw_webs_fields[sizeof(sh_package_fsm_webs) == 48 && sizeof(sh_package_fsm_web) == 64 ? 1 : -1];
typedef char pw_key_fields[sizeof(sh_package_key_groups) == 37392 ? 1 : -1];
#endif

/* ---- Writer ------------------------------------------------------------ */

void sh_package_warm_writer_free(sh_package_warm_writer *w)
{
    if (!w) return;
    free(w->bytes); memset(w, 0, sizeof(*w));
}
static unsigned char *pw_room(sh_package_warm_writer *w, size_t extra)
{
    if (w->failed) return NULL;
    if (extra > SIZE_MAX - w->length) { w->failed = 1; return NULL; }
    if (w->length + extra > w->capacity) {
        size_t next = w->capacity ? w->capacity : 65536;
        while (next < w->length + extra) {
            if (next > SIZE_MAX / 2) { w->failed = 1; return NULL; }
            next *= 2;
        }
        unsigned char *grown = realloc(w->bytes, next);
        if (!grown) { w->failed = 1; return NULL; }
        w->bytes = grown; w->capacity = next;
    }
    unsigned char *at = w->bytes + w->length;
    w->length += extra; return at;
}
enum { PW_CHUNK = 1u << 20 };
int sh_package_warm_writer_flush(sh_package_warm_writer *w)
{
    if (!w->failed && w->sink && w->buffered) {
        if (!w->sink(w->sink_context, w->bytes, w->buffered)) w->failed = 1;
        w->buffered = 0;
    }
    return !w->failed;
}
/* Every byte goes through here: counted, streamed through the bounded buffer, or appended. */
static void pw_put(sh_package_warm_writer *w, const void *bytes, size_t length)
{
    if (w->failed || !length) return;
    if (length > SIZE_MAX - w->length) { w->failed = 1; return; }
    if (w->count) { w->length += length; return; }
    if (w->sink) {
        if (!w->bytes && !(w->bytes = malloc(PW_CHUNK))) { w->failed = 1; return; }
        w->capacity = PW_CHUNK;
        if (w->buffered + length > PW_CHUNK && !sh_package_warm_writer_flush(w)) return;
        if (length >= PW_CHUNK) {
            if (!w->sink(w->sink_context, bytes, length)) { w->failed = 1; return; }
        } else { memcpy(w->bytes + w->buffered, bytes, length); w->buffered += length; }
        w->length += length;
        return;
    }
    unsigned char *at = pw_room(w, length);
    if (at) memcpy(at, bytes, length);
}
void sh_package_warm_raw(sh_package_warm_writer *w, const void *bytes, size_t length) { pw_put(w, bytes, length); }
void sh_package_warm_u8(sh_package_warm_writer *w, unsigned value)
{
    unsigned char b = (unsigned char)value;
    pw_put(w, &b, 1);
}
void sh_package_warm_u32(sh_package_warm_writer *w, uint32_t value)
{
    unsigned char b[4];
    for (int i = 0; i < 4; i++) b[i] = (unsigned char)(value >> (8 * i));
    pw_put(w, b, 4);
}
void sh_package_warm_u64(sh_package_warm_writer *w, uint64_t value)
{
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(value >> (8 * i));
    pw_put(w, b, 8);
}
void sh_package_warm_i32(sh_package_warm_writer *w, int value) { sh_package_warm_u32(w, (uint32_t)value); }
static void pw_size(sh_package_warm_writer *w, size_t value)
{ sh_package_warm_u64(w, value == SIZE_MAX ? UINT64_MAX : (uint64_t)value); }
void sh_package_warm_blob(sh_package_warm_writer *w, const void *bytes, size_t length)
{
    pw_size(w, length); sh_package_warm_raw(w, bytes, length);
}
void sh_package_warm_text(sh_package_warm_writer *w, const char *text)
{
    if (!text) { sh_package_warm_u64(w, 0); return; }
    size_t length = strlen(text);
    sh_package_warm_u64(w, (uint64_t)length + 1); sh_package_warm_raw(w, text, length);
}
void sh_package_warm_owners(sh_package_warm_writer *w, const sh_package_owners *owners)
{
    sh_package_warm_u64(w, owners->bits); pw_size(w, owners->words);
    for (size_t i = 0; i < owners->words; i++) sh_package_warm_u64(w, owners->more[i]);
}
void sh_package_warm_texts(sh_package_warm_writer *w, char *const *items, size_t count)
{
    pw_size(w, count);
    for (size_t i = 0; i < count; i++) sh_package_warm_text(w, items[i]);
}

/* ---- Reader ------------------------------------------------------------ */

void sh_package_warm_fail(sh_package_warm_reader *r, int why)
{ if (!r->failed) r->failed = why; }
static const unsigned char *pw_take(sh_package_warm_reader *r, size_t length)
{
    if (r->failed) return NULL;
    if ((size_t)(r->end - r->at) < length) { r->failed = SH_PACKAGE_WARM_MALFORMED; return NULL; }
    const unsigned char *at = r->at; r->at += length; return at;
}
const unsigned char *sh_package_warm_get_raw(sh_package_warm_reader *r, size_t length) { return pw_take(r, length); }
unsigned sh_package_warm_get_u8(sh_package_warm_reader *r)
{
    const unsigned char *at = pw_take(r, 1);
    return at ? *at : 0;
}
uint32_t sh_package_warm_get_u32(sh_package_warm_reader *r)
{
    const unsigned char *at = pw_take(r, 4); uint32_t value = 0;
    for (int i = 0; at && i < 4; i++) value |= (uint32_t)at[i] << (8 * i);
    return value;
}
uint64_t sh_package_warm_get_u64(sh_package_warm_reader *r)
{
    const unsigned char *at = pw_take(r, 8); uint64_t value = 0;
    for (int i = 0; at && i < 8; i++) value |= (uint64_t)at[i] << (8 * i);
    return value;
}
int sh_package_warm_get_i32(sh_package_warm_reader *r) { return (int)sh_package_warm_get_u32(r); }
size_t sh_package_warm_get_index(sh_package_warm_reader *r, size_t limit, int allow_none)
{
    uint64_t value = sh_package_warm_get_u64(r);
    if (r->failed) return SIZE_MAX;
    if (allow_none && value == UINT64_MAX) return SIZE_MAX;
    if (value >= (uint64_t)limit) { r->failed = SH_PACKAGE_WARM_MALFORMED; return SIZE_MAX; }
    return (size_t)value;
}
size_t sh_package_warm_get_count(sh_package_warm_reader *r, size_t element_bytes)
{
    uint64_t value = sh_package_warm_get_u64(r);
    size_t remaining = (size_t)(r->end - r->at);
    if (r->failed) return 0;
    if (!element_bytes) element_bytes = 1;
    if (value > (uint64_t)(remaining / element_bytes) || value > SIZE_MAX / 64) {
        r->failed = SH_PACKAGE_WARM_MALFORMED; return 0;
    }
    return (size_t)value;
}
const unsigned char *sh_package_warm_get_blob(sh_package_warm_reader *r, size_t *length)
{
    size_t n = sh_package_warm_get_count(r, 1);
    const unsigned char *at = r->failed ? NULL : pw_take(r, n);
    *length = at ? n : 0;
    return at ? at : (r->failed ? NULL : (const unsigned char *)"");
}
unsigned char *sh_package_warm_get_bytes(sh_package_warm_reader *r, size_t *length)
{
    size_t n = 0;
    const unsigned char *at = sh_package_warm_get_blob(r, &n);
    *length = 0;
    if (!at) return NULL;
    unsigned char *out = malloc(n + 1);
    if (!out) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return NULL; }
    if (n) memcpy(out, at, n);
    out[n] = 0; *length = n; return out;
}
char *sh_package_warm_get_text(sh_package_warm_reader *r, int *present)
{
    uint64_t value = sh_package_warm_get_u64(r);
    *present = 0;
    if (r->failed || !value) return NULL;
    size_t remaining = (size_t)(r->end - r->at);
    if (value - 1 > (uint64_t)remaining) { r->failed = SH_PACKAGE_WARM_MALFORMED; return NULL; }
    size_t n = (size_t)(value - 1);
    const unsigned char *at = pw_take(r, n);
    if (!at) return NULL;
    if (memchr(at, 0, n)) { r->failed = SH_PACKAGE_WARM_MALFORMED; return NULL; }
    char *out = malloc(n + 1);
    if (!out) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return NULL; }
    if (n) memcpy(out, at, n);
    out[n] = 0; *present = 1; return out;
}
char *sh_package_warm_get_string(sh_package_warm_reader *r)
{
    int present = 0;
    char *text = sh_package_warm_get_text(r, &present);
    if (!present && !r->failed) r->failed = SH_PACKAGE_WARM_MALFORMED;
    return text;
}
int sh_package_warm_get_owners(sh_package_warm_reader *r, sh_package_owners *out, size_t package_limit)
{
    memset(out, 0, sizeof(*out));
    out->bits = sh_package_warm_get_u64(r);
    size_t words = sh_package_warm_get_count(r, 8);
    if (r->failed) return 0;
    if (words) {
        if (!(out->more = malloc(words * sizeof(*out->more)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return 0; }
        out->words = words;
        for (size_t i = 0; i < words; i++) out->more[i] = sh_package_warm_get_u64(r);
    }
    if (!r->failed && !sh_package_owners_within(out, package_limit)) r->failed = SH_PACKAGE_WARM_MALFORMED;
    if (r->failed) { sh_package_owners_free(out); return 0; }
    return 1;
}
void sh_package_warm_texts_free(char **items, size_t count)
{
    for (size_t i = 0; items && i < count; i++) free(items[i]);
    free(items);
}
/* Strings that may be NULL individually are not used by any published list; a NULL entry is malformed. */
char **sh_package_warm_get_texts(sh_package_warm_reader *r, size_t *count)
{
    size_t n = sh_package_warm_get_count(r, 8);
    char **items = NULL;
    *count = 0;
    if (r->failed || !n) return NULL;
    if (!(items = calloc(n, sizeof(*items)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return NULL; }
    for (size_t i = 0; i < n && !r->failed; i++) items[i] = sh_package_warm_get_string(r);
    if (r->failed) { sh_package_warm_texts_free(items, n); return NULL; }
    *count = n; return items;
}
static size_t *pw_get_indices(sh_package_warm_reader *r, size_t *count, size_t limit)
{
    size_t n = sh_package_warm_get_count(r, 8), *items = NULL;
    *count = 0;
    if (r->failed || !n) return NULL;
    if (!(items = malloc(n * sizeof(*items)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return NULL; }
    for (size_t i = 0; i < n && !r->failed; i++) items[i] = sh_package_warm_get_index(r, limit, 0);
    if (r->failed) { free(items); return NULL; }
    *count = n; return items;
}
static void pw_put_indices(sh_package_warm_writer *w, const size_t *items, size_t count)
{
    pw_size(w, count);
    for (size_t i = 0; i < count; i++) pw_size(w, items[i]);
}

/* ---- JSON objects -------------------------------------------------------- */

void sh_package_warm_put_json(sh_package_warm_writer *w, const sh_json_object *object)
{
    pw_size(w, object->count);
    for (size_t i = 0; i < object->count; i++) {
        sh_package_warm_blob(w, object->members[i].key, object->members[i].key_length);
        sh_package_warm_text(w, object->members[i].value_json);
    }
}
int sh_package_warm_get_json(sh_package_warm_reader *r, sh_json_object *out)
{
    memset(out, 0, sizeof(*out));
    size_t n = sh_package_warm_get_count(r, 16);
    if (r->failed) return 0;
    if (n && !(out->members = calloc(n, sizeof(*out->members)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return 0; }
    out->capacity = n;
    for (size_t i = 0; i < n && !r->failed; i++) {
        sh_json_member *member = out->members + i;
        size_t length = 0;
        member->key = (char *)sh_package_warm_get_bytes(r, &length);
        member->key_length = length;
        if (r->failed) break;
        out->count++;
        member->value_json = sh_package_warm_get_string(r);
        if (r->failed) break;
        sh_json_kind kind;
        if (!sh_native_json_validate(member->value_json, strlen(member->value_json), 64, &kind, NULL))
            r->failed = SH_PACKAGE_WARM_MALFORMED;
    }
    if (r->failed) { sh_json_object_free(out); return 0; }
    return 1;
}

/* ---- Compilation ----------------------------------------------------------- */

static void pw_put_policy(sh_package_warm_writer *w, const sh_package_policy *policy)
{
    sh_package_warm_put_json(w, &policy->requirements);
    sh_package_warm_put_json(w, &policy->strings);
    sh_package_warm_put_json(w, &policy->hud);
    pw_size(w, policy->localized_count);
    for (size_t i = 0; i < policy->localized_count; i++) {
        sh_package_warm_text(w, policy->localized[i].key); sh_package_warm_text(w, policy->localized[i].mapping);
        sh_package_warm_owners(w, &policy->localized[i].owners);
    }
    sh_package_warm_owners(w, &policy->legacy_string_owners);
    pw_size(w, policy->hud_binding_count);
    for (size_t i = 0; i < policy->hud_binding_count; i++) {
        sh_package_warm_text(w, policy->hud_bindings[i].name); sh_package_warm_i32(w, policy->hud_bindings[i].weapon_mode);
        sh_package_warm_owners(w, &policy->hud_bindings[i].owners);
    }
    sh_package_warm_i32(w, policy->hud_bound);
}
static int pw_get_policy(sh_package_warm_reader *r, sh_package_policy *policy, size_t packages)
{
    memset(policy, 0, sizeof(*policy));
    if (!sh_package_warm_get_json(r, &policy->requirements) || !sh_package_warm_get_json(r, &policy->strings) ||
        !sh_package_warm_get_json(r, &policy->hud)) goto failed;
    size_t n = sh_package_warm_get_count(r, 40);
    if (r->failed) goto failed;
    if (n && !(policy->localized = calloc(n, sizeof(*policy->localized)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); goto failed; }
    for (size_t i = 0; i < n && !r->failed; i++) {
        sh_package_localized_string *value = policy->localized + policy->localized_count++;
        value->key = sh_package_warm_get_string(r); value->mapping = sh_package_warm_get_string(r);
        if (!r->failed) sh_package_warm_get_owners(r, &value->owners, packages);
    }
    if (r->failed || !sh_package_warm_get_owners(r, &policy->legacy_string_owners, packages)) goto failed;
    n = sh_package_warm_get_count(r, 36);
    if (r->failed) goto failed;
    if (n && !(policy->hud_bindings = calloc(n, sizeof(*policy->hud_bindings)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); goto failed; }
    for (size_t i = 0; i < n && !r->failed; i++) {
        sh_package_hud_binding *value = policy->hud_bindings + policy->hud_binding_count++;
        value->name = sh_package_warm_get_string(r); value->weapon_mode = sh_package_warm_get_i32(r);
        if (!r->failed) sh_package_warm_get_owners(r, &value->owners, packages);
    }
    policy->hud_bound = sh_package_warm_get_i32(r);
    if (r->failed) goto failed;
    return 1;
failed:
    sh_package_policy_free(policy); return 0;
}

/* Body owners by pointer, for the shared-body table. */
typedef struct pw_owner_map { const sh_package_body **keys; size_t *values, capacity, count; } pw_owner_map;
static size_t pw_owner_slot(const pw_owner_map *map, const sh_package_body *key)
{
    uintptr_t h = (uintptr_t)key; h ^= h >> 17; h *= (uintptr_t)0x9E3779B97F4A7C15ull;
    size_t slot = (size_t)h & (map->capacity - 1);
    while (map->keys[slot] && map->keys[slot] != key) slot = (slot + 1) & (map->capacity - 1);
    return slot;
}
static int pw_owner_map_open(pw_owner_map *map, size_t entries)
{
    size_t capacity = 64;
    while (capacity < entries * 2 + 2) { if (capacity > SIZE_MAX / 4) return 0; capacity *= 2; }
    map->keys = calloc(capacity, sizeof(*map->keys)); map->values = calloc(capacity, sizeof(*map->values));
    map->capacity = capacity; map->count = 0;
    return map->keys && map->values;
}
static void pw_owner_map_close(pw_owner_map *map) { free((void *)map->keys); free(map->values); memset(map, 0, sizeof(*map)); }

static int pw_fail(char *error, size_t capacity, const char *reason, const char *subject)
{
    if (error && capacity) snprintf(error, capacity, "%s%s%s", reason, subject ? ": " : "", subject ? subject : "");
    return 0;
}
/* A cache file name: one path component, never a traversal or a drive. */
static int pw_leaf_valid(const char *leaf)
{
    return leaf && *leaf && strlen(leaf) < 200 && !strpbrk(leaf, "/\\:") && strcmp(leaf, ".") && strcmp(leaf, "..");
}
static const char *pw_leaf(const char *path, const char *directory)
{
    size_t n = directory ? strlen(directory) : 0;
    if (!path || !n || strncmp(path, directory, n) || (path[n] != '/' && path[n] != '\\')) return NULL;
    const char *leaf = path + n + 1;
    return pw_leaf_valid(leaf) ? leaf : NULL;
}
int sh_package_warm_put_compilation(sh_package_warm_writer *w, const sh_package_compilation *c,
    const char *cache_directory, sh_package_warm_body_origin origin, void *origin_context, char *error, size_t capacity)
{
    pw_owner_map owners = {0};
    const sh_package_sources *sources = c->sources;
    size_t files = sources ? sources->file_count : 0;
    int ok = 0;
    if (c->owns_sources) return pw_fail(error, capacity, "a published library compilation owns no inventory", NULL);
    if (!pw_owner_map_open(&owners, c->resource_count)) return pw_fail(error, capacity, "cannot index shared bodies", NULL);
    pw_size(w, c->resource_count); pw_size(w, c->duplicate_count); pw_size(w, c->composed_count);
    sh_package_warm_i32(w, c->map_overlay); pw_size(w, c->map_owner_begin);
    sh_package_warm_i32(w, c->independent_sources); sh_package_warm_i32(w, c->map_independent_sources);
    pw_put_policy(w, &c->policy);
    /* The shared-body table, in first-use order. */
    for (size_t i = 0; i < c->resource_count; i++) {
        const sh_compiled_resource *r = c->resources + i;
        if (!r->body_owner) continue;
        /* Attached to its own bytes, or detached from a backed body (an archive-backed image after publication). */
        if ((r->body ? r->body != sh_package_body_bytes(r->body_owner) : !sh_package_body_backed(r->body_owner)) ||
            r->body_length != sh_package_body_length(r->body_owner)) {
            pw_fail(error, capacity, "a published resource holds a detached or foreign shared body", r->engine_path); goto done;
        }
        size_t slot = pw_owner_slot(&owners, r->body_owner);
        if (!owners.keys[slot]) { owners.keys[slot] = r->body_owner; owners.values[slot] = owners.count++; }
    }
    pw_size(w, owners.count);
    {
        /* Each entry: 0 and the bytes, or 1 and the recorded original read a backed body holds exactly. */
        size_t next = 0;
        for (size_t i = 0; i < c->resource_count; i++) {
            const sh_compiled_resource *r = c->resources + i;
            if (!r->body_owner) continue;
            size_t slot = pw_owner_slot(&owners, r->body_owner);
            if (owners.values[slot] != next) continue;
            if (sh_package_body_backed(r->body_owner)) {
                size_t read = origin ? origin(origin_context, r->body_owner) : SIZE_MAX;
                if (read == SIZE_MAX) {
                    pw_fail(error, capacity, "a published original's body has no recorded original read", r->engine_path);
                    goto done;
                }
                sh_package_warm_u8(w, 1); pw_size(w, read);
            } else {
                sh_package_warm_u8(w, 0); sh_package_warm_blob(w, r->body, r->body_length);
            }
            next++;
        }
    }
    for (size_t i = 0; i < c->resource_count; i++) {
        const sh_compiled_resource *r = c->resources + i;
        sh_package_warm_text(w, r->engine_path);
        sh_package_warm_u8(w, r->owns_engine_path != 0); sh_package_warm_i32(w, r->restored_original);
        if (!r->owns_engine_path && !r->restored_original) {
            /* A borrowed engine path is the inventory's: name the file whose string it is. */
            size_t owner = SIZE_MAX;
            for (size_t f = 0; f < files && owner == SIZE_MAX; f++)
                if (sources->files[f].engine_path == r->engine_path) owner = f;
            if (owner == SIZE_MAX) {
                pw_fail(error, capacity, "a published resource borrows its engine path from no inventoried file", r->engine_path);
                goto done;
            }
            pw_size(w, owner);
        }
        sh_package_warm_text(w, r->type); sh_package_warm_text(w, r->name);
        pw_put_indices(w, r->sources, r->source_count);
        sh_package_warm_owners(w, &r->owners); sh_package_warm_owners(w, &r->gameplay_owners);
        sh_package_warm_i32(w, r->baseline_known); sh_package_warm_i32(w, r->composed);
        sh_package_warm_i32(w, r->native_original); sh_package_warm_i32(w, r->semantic_coverage);
        if (r->body_owner) {
            sh_package_warm_u8(w, 2); pw_size(w, owners.values[pw_owner_slot(&owners, r->body_owner)]);
        } else if (r->body) {
            sh_package_warm_u8(w, 1); sh_package_warm_blob(w, r->body, r->body_length);
        } else {
            if (r->body_length) { pw_fail(error, capacity, "a published resource has a length without a body", r->engine_path); goto done; }
            sh_package_warm_u8(w, 0);
        }
        sh_package_warm_u8(w, r->presented != NULL);
        if (r->presented) sh_package_warm_blob(w, r->presented, r->presented_length);
        pw_size(w, r->source);
        if (!r->cache_file) {
            if (r->cache_path) { pw_fail(error, capacity, "a cache path without its file", r->engine_path); goto done; }
            sh_package_warm_u8(w, SH_PACKAGE_WARM_CACHE_NONE);
        } else {
            sh_package_file_identity identity;
            const char *leaf = pw_leaf(r->cache_path, cache_directory);
            if (!leaf || !sh_package_file_get_identity(r->cache_file, &identity)) {
                pw_fail(error, capacity, "a cache file outside the package cache", r->engine_path); goto done;
            }
            int from_source = !r->body && r->source < files && sources->files[r->source].length == identity.length &&
                !memcmp(sources->files[r->source].digest, identity.digest, 32);
            sh_package_warm_u8(w, from_source ? SH_PACKAGE_WARM_CACHE_SOURCE : SH_PACKAGE_WARM_CACHE_GENERATED);
            sh_package_warm_text(w, leaf); sh_package_warm_u64(w, identity.length); sh_package_warm_raw(w, identity.digest, 32);
        }
        sh_package_warm_i32(w, r->generated); sh_package_warm_i32(w, r->generated_layer);
        sh_package_warm_texts(w, r->generated_inputs, r->generated_input_count);
        pw_put_indices(w, r->generated_sources, r->generated_source_count);
        sh_package_warm_text(w, r->original_path);
        sh_package_warm_text(w, r->native_type); sh_package_warm_text(w, r->native_name);
        sh_package_warm_texts(w, r->bound_dependencies, r->bound_dependency_count);
        sh_package_warm_u32(w, r->sound_authored_event); sh_package_warm_u32(w, r->sound_event);
        pw_size(w, r->native_alias_count);
        for (size_t a = 0; a < r->native_alias_count; a++) {
            sh_package_warm_text(w, r->native_aliases[a].type); sh_package_warm_text(w, r->native_aliases[a].name);
        }
        sh_package_warm_i32(w, r->rebound_original);
        /* An installed-original alias keeps no bytes of its own: the record carries the identity the linker certified,
         * and the loader checks every read through its installed reader against it (sh_package_compilation_read). */
        sh_package_warm_i32(w, r->installed_alias);
        if (r->installed_alias) {
            if (r->body || r->body_owner || r->cache_file || r->cache_path || r->source != SIZE_MAX) {
                pw_fail(error, capacity, "an installed-original alias holds bytes of its own", r->engine_path); goto done;
            }
            sh_package_warm_u64(w, r->installed_identity.length); sh_package_warm_raw(w, r->installed_identity.digest, 32);
        }
    }
    ok = !w->failed;
    if (!ok) pw_fail(error, capacity, "cannot encode the published library", NULL);
done:
    pw_owner_map_close(&owners);
    return ok;
}

void sh_package_warm_caches_free(sh_package_warm_cache *caches, size_t count)
{
    for (size_t i = 0; caches && i < count; i++) free(caches[i].leaf);
    free(caches);
}
/* Release one resource the decoder built: the compiler's own field ownership (pc_resource_free). */
static void pw_resource_clear(sh_compiled_resource *r)
{
    if (r->restored_original || r->owns_engine_path) free((void *)r->engine_path);
    sh_package_owners_free(&r->owners); sh_package_owners_free(&r->gameplay_owners);
    if (r->body_owner) sh_package_body_drop(r->body_owner, r->body); else free(r->body);
    free(r->presented); free(r->sources); free(r->type); free(r->name); free(r->cache_path); free(r->original_path);
    free(r->native_type); free(r->native_name);
    for (size_t i = 0; i < r->native_alias_count; i++) { free(r->native_aliases[i].type); free(r->native_aliases[i].name); }
    free(r->native_aliases);
    sh_package_file_release(r->cache_file);
    sh_package_warm_texts_free(r->generated_inputs, r->generated_input_count); free(r->generated_sources);
    sh_package_warm_texts_free(r->bound_dependencies, r->bound_dependency_count);
    memset(r, 0, sizeof(*r));
}
sh_package_compilation *sh_package_warm_get_compilation(sh_package_warm_reader *r,
    const sh_package_sources *sources, char *(*canonical_path)(const char *),
    sh_package_warm_body_original original, void *original_context,
    sh_package_warm_cache **caches_out, size_t *cache_count_out)
{
    sh_package_compilation *c = calloc(1, sizeof(*c));
    sh_package_body **bodies = NULL; size_t body_count = 0;
    sh_package_warm_cache *caches = NULL; size_t cache_count = 0;
    size_t files = sources ? sources->file_count : 0, packages = sources ? sources->package_count : 0;
    *caches_out = NULL; *cache_count_out = 0;
    if (!c) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return NULL; }
    c->sources = sources; c->canonical_path = canonical_path;
    size_t count = sh_package_warm_get_count(r, 64);
    c->duplicate_count = (size_t)sh_package_warm_get_u64(r); c->composed_count = (size_t)sh_package_warm_get_u64(r);
    c->map_overlay = sh_package_warm_get_i32(r); c->map_owner_begin = (size_t)sh_package_warm_get_u64(r);
    c->independent_sources = sh_package_warm_get_i32(r); c->map_independent_sources = sh_package_warm_get_i32(r);
    if (r->failed || !pw_get_policy(r, &c->policy, packages)) goto failed;
    body_count = sh_package_warm_get_count(r, 8);
    if (r->failed) goto failed;
    if (body_count && !(bodies = calloc(body_count, sizeof(*bodies)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); goto failed; }
    for (size_t i = 0; i < body_count && !r->failed; i++) {
        unsigned kind = sh_package_warm_get_u8(r);
        if (kind == 1) {
            uint64_t read = sh_package_warm_get_u64(r);
            if (r->failed) break;
            if (!original || read >= SIZE_MAX || !(bodies[i] = original(original_context, (size_t)read)))
                sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED);
            continue;
        }
        if (kind != 0) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED); break; }
        size_t length = 0;
        unsigned char *bytes = sh_package_warm_get_bytes(r, &length);
        if (!bytes) break;
        if (!(bodies[i] = sh_package_body_adopt(bytes, length))) { free(bytes); sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); }
    }
    if (r->failed) goto failed;
    if (count && !(c->resources = calloc(count, sizeof(*c->resources)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); goto failed; }
    if (count && !(caches = calloc(count, sizeof(*caches)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); goto failed; }
    for (size_t i = 0; i < count && !r->failed; i++) {
        sh_compiled_resource *res = c->resources + c->resource_count++;
        int present = 0;
        char *path = sh_package_warm_get_string(r);
        unsigned owns = sh_package_warm_get_u8(r);
        int restored = sh_package_warm_get_i32(r);
        if (r->failed || owns > 1) { free(path); sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED); break; }
        if (owns || restored) {
            /* Owned by the resource (pc_resource_free frees it under either flag). */
            res->engine_path = path; res->owns_engine_path = (int)owns; res->restored_original = restored;
        } else {
            /* Borrowed from the inventoried file the writer named, which must spell it exactly. */
            size_t owner = sh_package_warm_get_index(r, files, 0);
            int same = !r->failed && sources->files[owner].engine_path && !strcmp(sources->files[owner].engine_path, path);
            free(path);
            if (!same) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED); break; }
            res->engine_path = sources->files[owner].engine_path;
        }
        res->type = sh_package_warm_get_text(r, &present); res->name = sh_package_warm_get_text(r, &present);
        res->sources = pw_get_indices(r, &res->source_count, files);
        if (r->failed || !sh_package_warm_get_owners(r, &res->owners, packages) ||
            !sh_package_warm_get_owners(r, &res->gameplay_owners, packages)) break;
        res->baseline_known = sh_package_warm_get_i32(r); res->composed = sh_package_warm_get_i32(r);
        res->native_original = sh_package_warm_get_i32(r); res->semantic_coverage = sh_package_warm_get_i32(r);
        unsigned kind = sh_package_warm_get_u8(r);
        if (kind == 2) {
            size_t index = sh_package_warm_get_index(r, body_count, 0);
            if (r->failed) break;
            res->body_owner = sh_package_body_retain(bodies[index]);
            res->body = (unsigned char *)sh_package_body_bytes(res->body_owner);
            res->body_length = sh_package_body_length(res->body_owner);
        } else if (kind == 1) {
            res->body = sh_package_warm_get_bytes(r, &res->body_length);
        } else if (kind != 0) r->failed = SH_PACKAGE_WARM_MALFORMED;
        if (r->failed) break;
        if (sh_package_warm_get_u8(r)) res->presented = sh_package_warm_get_bytes(r, &res->presented_length);
        res->source = sh_package_warm_get_index(r, files, 1);
        unsigned cache = sh_package_warm_get_u8(r);
        if (r->failed) break;
        if (cache == SH_PACKAGE_WARM_CACHE_SOURCE || cache == SH_PACKAGE_WARM_CACHE_GENERATED) {
            sh_package_warm_cache *entry = caches + cache_count++;
            entry->resource = i; entry->kind = (int)cache;
            entry->leaf = sh_package_warm_get_string(r);
            entry->identity.length = sh_package_warm_get_u64(r);
            const unsigned char *digest = sh_package_warm_get_raw(r, 32);
            if (digest) memcpy(entry->identity.digest, digest, 32);
            if (r->failed) break;
            if (res->body || !pw_leaf_valid(entry->leaf) ||
                (cache == SH_PACKAGE_WARM_CACHE_SOURCE && (res->source >= files ||
                 sources->files[res->source].length != entry->identity.length ||
                 memcmp(sources->files[res->source].digest, entry->identity.digest, 32)))) {
                r->failed = SH_PACKAGE_WARM_MALFORMED; break;
            }
        } else if (cache != SH_PACKAGE_WARM_CACHE_NONE) { r->failed = SH_PACKAGE_WARM_MALFORMED; break; }
        res->generated = sh_package_warm_get_i32(r); res->generated_layer = sh_package_warm_get_i32(r);
        res->generated_inputs = sh_package_warm_get_texts(r, &res->generated_input_count);
        res->generated_sources = pw_get_indices(r, &res->generated_source_count, files);
        res->original_path = sh_package_warm_get_text(r, &present);
        int typed = 0, named = 0;
        res->native_type = sh_package_warm_get_text(r, &typed); res->native_name = sh_package_warm_get_text(r, &named);
        if (!r->failed && typed != named) r->failed = SH_PACKAGE_WARM_MALFORMED;
        res->bound_dependencies = sh_package_warm_get_texts(r, &res->bound_dependency_count);
        res->sound_authored_event = sh_package_warm_get_u32(r); res->sound_event = sh_package_warm_get_u32(r);
        size_t aliases = sh_package_warm_get_count(r, 16);
        if (r->failed) break;
        if (aliases && !(res->native_aliases = calloc(aliases, sizeof(*res->native_aliases)))) {
            sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); break;
        }
        for (size_t a = 0; a < aliases && !r->failed; a++) {
            sh_package_native_identity *alias = res->native_aliases + res->native_alias_count++;
            alias->type = sh_package_warm_get_string(r); alias->name = sh_package_warm_get_string(r);
        }
        res->rebound_original = sh_package_warm_get_i32(r);
        res->installed_alias = sh_package_warm_get_i32(r);
        if (res->installed_alias) {
            res->installed_identity.length = sh_package_warm_get_u64(r);
            const unsigned char *digest = sh_package_warm_get_raw(r, 32);
            if (digest) memcpy(res->installed_identity.digest, digest, 32);
            /* Contradictory: a flag the compiler never sets, or an alias with bytes of its own. */
            if (!r->failed && (res->installed_alias != 1 || res->body || res->body_owner ||
                cache != SH_PACKAGE_WARM_CACHE_NONE || res->source != SIZE_MAX)) r->failed = SH_PACKAGE_WARM_MALFORMED;
        }
        if (!r->failed && !res->body && !res->body_owner && !res->installed_alias && cache == SH_PACKAGE_WARM_CACHE_NONE &&
            (res->source >= files)) r->failed = SH_PACKAGE_WARM_MALFORMED; /* nothing would serve it */
        if (!r->failed && i && strcmp(c->resources[i - 1].engine_path, res->engine_path) >= 0)
            r->failed = SH_PACKAGE_WARM_MALFORMED; /* published resources are sorted and unique by path */
    }
    if (r->failed) goto failed;
    for (size_t i = 0; i < body_count; i++) sh_package_body_release(bodies[i]);
    free(bodies);
    *caches_out = caches; *cache_count_out = cache_count;
    return c;
failed:
    for (size_t i = 0; c && c->resources && i < c->resource_count; i++) pw_resource_clear(c->resources + i);
    if (c) { free(c->resources); sh_package_policy_free(&c->policy); free(c); }
    for (size_t i = 0; bodies && i < body_count; i++) sh_package_body_release(bodies[i]);
    free(bodies);
    sh_package_warm_caches_free(caches, cache_count);
    return NULL;
}

/* ---- Provenance ------------------------------------------------------------- */

void sh_package_warm_put_provenance(sh_package_warm_writer *w, const sh_package_provenance *p)
{
    sh_package_warm_u32(w, p->semantics);
    pw_size(w, p->context_count);
    for (size_t i = 0; i < p->context_count; i++) {
        const sh_package_provenance_context *context = p->contexts + i;
        sh_package_warm_u32(w, context->mode); pw_size(w, context->tree_count);
        sh_package_warm_raw(w, context->trees, context->tree_count * 32);
        sh_package_warm_text(w, context->component);
    }
    pw_size(w, p->binding_count);
    for (size_t i = 0; i < p->binding_count; i++) {
        const sh_package_provenance_binding *binding = p->bindings + i;
        sh_package_warm_text(w, binding->family); sh_package_warm_text(w, binding->original);
        sh_package_warm_text(w, binding->name); pw_put_indices(w, binding->contexts, binding->context_count);
    }
    pw_size(w, p->use_count);
    for (size_t i = 0; i < p->use_count; i++) {
        const sh_package_provenance_use *use = p->uses + i;
        sh_package_warm_u32(w, use->kind); sh_package_warm_text(w, use->owner); sh_package_warm_text(w, use->reference);
        pw_size(w, use->binding); pw_put_indices(w, use->contexts, use->context_count);
        sh_package_warm_text(w, use->row);
    }
}
int sh_package_warm_get_provenance(sh_package_warm_reader *r, sh_package_provenance *out)
{
    memset(out, 0, sizeof(*out));
    int present = 0;
    out->semantics = sh_package_warm_get_u32(r);
    size_t contexts = sh_package_warm_get_count(r, 20);
    if (r->failed) goto failed;
    if (contexts && !(out->contexts = calloc(contexts, sizeof(*out->contexts)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); goto failed; }
    for (size_t i = 0; i < contexts && !r->failed; i++) {
        sh_package_provenance_context *context = out->contexts + out->context_count++;
        context->mode = sh_package_warm_get_u32(r);
        size_t trees = sh_package_warm_get_count(r, 32);
        if (r->failed) break;
        if (trees) {
            const unsigned char *bytes = sh_package_warm_get_raw(r, trees * 32);
            if (!bytes) break;
            if (!(context->trees = malloc(trees * 32))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); break; }
            memcpy(context->trees, bytes, trees * 32); context->tree_count = trees;
        }
        context->component = sh_package_warm_get_text(r, &present);
    }
    size_t bindings = r->failed ? 0 : sh_package_warm_get_count(r, 32);
    if (r->failed) goto failed;
    if (bindings && !(out->bindings = calloc(bindings, sizeof(*out->bindings)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); goto failed; }
    for (size_t i = 0; i < bindings && !r->failed; i++) {
        sh_package_provenance_binding *binding = out->bindings + out->binding_count++;
        binding->family = sh_package_warm_get_string(r); binding->original = sh_package_warm_get_string(r);
        binding->name = sh_package_warm_get_string(r);
        binding->contexts = pw_get_indices(r, &binding->context_count, out->context_count);
    }
    size_t uses = r->failed ? 0 : sh_package_warm_get_count(r, 44);
    if (r->failed) goto failed;
    if (uses && !(out->uses = calloc(uses, sizeof(*out->uses)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); goto failed; }
    for (size_t i = 0; i < uses && !r->failed; i++) {
        sh_package_provenance_use *use = out->uses + out->use_count++;
        use->kind = sh_package_warm_get_u32(r);
        use->owner = sh_package_warm_get_text(r, &present); use->reference = sh_package_warm_get_text(r, &present);
        use->binding = sh_package_warm_get_index(r, out->binding_count, 0);
        use->contexts = pw_get_indices(r, &use->context_count, out->context_count);
        use->row = sh_package_warm_get_text(r, &present);
    }
    if (r->failed) goto failed;
    return 1;
failed:
    sh_package_provenance_free(out); return 0;
}

/* ---- Consumers, state-machine webs, key groups ------------------------------------ */

void sh_package_warm_put_consumers(sh_package_warm_writer *w, const sh_package_consumers *consumers)
{
    size_t count = sh_package_consumers_count(consumers);
    sh_package_warm_u8(w, consumers != NULL);
    if (!consumers) return;
    pw_size(w, count);
    for (size_t i = 0; i < count; i++) {
        const sh_package_consumer *owner = sh_package_consumers_at(consumers, i);
        sh_package_warm_text(w, owner->family); sh_package_warm_text(w, owner->name); pw_size(w, owner->count);
        for (size_t k = 0; k < owner->count; k++) {
            sh_package_warm_text(w, owner->targets[k].slot); sh_package_warm_text(w, owner->targets[k].family);
            sh_package_warm_text(w, owner->targets[k].name);
        }
    }
}
static int pw_text_same(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static void pw_rows_free(sh_package_consumer *rows, size_t count)
{
    for (size_t i = 0; rows && i < count; i++) {
        free((void *)rows[i].family); free((void *)rows[i].name);
        for (size_t k = 0; k < rows[i].count; k++) {
            free((void *)rows[i].targets[k].slot); free((void *)rows[i].targets[k].family);
            free((void *)rows[i].targets[k].name);
        }
        free((void *)rows[i].targets);
    }
    free(rows);
}
int sh_package_warm_get_consumers(sh_package_warm_reader *r, sh_package_consumers **out)
{
    sh_package_consumer *rows = NULL; size_t count = 0;
    char error[256];
    *out = NULL;
    unsigned present = sh_package_warm_get_u8(r);
    if (r->failed || present > 1) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED); return 0; }
    if (!present) return 1;
    count = sh_package_warm_get_count(r, 24);
    if (r->failed) return 0;
    if (count && !(rows = calloc(count, sizeof(*rows)))) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return 0; }
    for (size_t i = 0; i < count && !r->failed; i++) {
        sh_package_consumer *row = rows + i;
        row->family = sh_package_warm_get_string(r); row->name = sh_package_warm_get_string(r);
        size_t targets = sh_package_warm_get_count(r, 24);
        if (r->failed) break;
        sh_package_consumer_target *items = targets ? calloc(targets, sizeof(*items)) : NULL;
        if (targets && !items) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); break; }
        row->targets = items;
        for (size_t k = 0; k < targets && !r->failed; k++) {
            items[k].slot = sh_package_warm_get_string(r); items[k].family = sh_package_warm_get_string(r);
            items[k].name = sh_package_warm_get_string(r); row->count = k + 1;
        }
    }
    if (!r->failed) {
        /* Rows the catalog refuses (an empty name, conflicting duplicates) are a damaged record; only a failed
         * allocation is exhaustion. */
        errno = 0;
        *out = sh_package_consumers_create(rows, count, error, sizeof(error));
        if (!*out) sh_package_warm_fail(r, errno == ENOMEM ? SH_PACKAGE_WARM_MEMORY : SH_PACKAGE_WARM_MALFORMED);
        /* The writer emitted a created catalog in its own order: creating it again reproduces exactly that
         * order and content, else the record is not one this writer produced. */
        else if (sh_package_consumers_count(*out) != count) sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED);
        for (size_t i = 0; *out && !r->failed && i < count; i++) {
            const sh_package_consumer *made = sh_package_consumers_at(*out, i);
            int same = pw_text_same(made->family, rows[i].family) && pw_text_same(made->name, rows[i].name) &&
                made->count == rows[i].count;
            for (size_t k = 0; same && k < rows[i].count; k++)
                same = pw_text_same(made->targets[k].slot, rows[i].targets[k].slot) &&
                    pw_text_same(made->targets[k].family, rows[i].targets[k].family) &&
                    pw_text_same(made->targets[k].name, rows[i].targets[k].name);
            if (!same) sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED);
        }
        if (r->failed) { sh_package_consumers_free(*out); *out = NULL; }
    }
    pw_rows_free(rows, count);
    return !r->failed;
}

void sh_package_warm_put_webs(sh_package_warm_writer *w, const sh_package_fsm_webs *webs)
{
    sh_package_warm_u8(w, webs != NULL);
    if (!webs) return;
    pw_size(w, webs->count);
    for (size_t i = 0; i < webs->count; i++) {
        const sh_package_fsm_web *web = webs->webs + i;
        sh_package_warm_text(w, web->config);
        sh_package_warm_texts(w, web->behaviors, web->behavior_count);
        pw_size(w, web->count);
        for (size_t k = 0; k < web->count; k++) { sh_package_warm_text(w, web->from[k]); sh_package_warm_text(w, web->to[k]); }
        sh_package_warm_texts(w, web->extra, web->extra_count);
    }
    sh_package_warm_texts(w, webs->owned, webs->owned_count);
    pw_size(w, webs->generated_count);
    for (size_t i = 0; i < webs->generated_count; i++) {
        sh_package_warm_text(w, webs->generated[i].name);
        sh_package_warm_blob(w, webs->generated[i].text, webs->generated[i].length);
    }
}
int sh_package_warm_get_webs(sh_package_warm_reader *r, sh_package_fsm_webs **out)
{
    *out = NULL;
    unsigned present = sh_package_warm_get_u8(r);
    if (r->failed || present > 1) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED); return 0; }
    if (!present) return 1;
    sh_package_fsm_webs *webs = calloc(1, sizeof(*webs));
    if (!webs) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return 0; }
    size_t count = sh_package_warm_get_count(r, 32);
    if (!r->failed && count && !(webs->webs = calloc(count, sizeof(*webs->webs)))) sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY);
    for (size_t i = 0; i < count && !r->failed; i++) {
        sh_package_fsm_web *web = webs->webs + webs->count++;
        web->config = sh_package_warm_get_string(r);
        web->behaviors = sh_package_warm_get_texts(r, &web->behavior_count);
        size_t pairs = sh_package_warm_get_count(r, 16);
        if (r->failed) break;
        if (pairs) {
            web->from = calloc(pairs, sizeof(*web->from)); web->to = calloc(pairs, sizeof(*web->to));
            if (!web->from || !web->to) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); }
            else web->count = pairs;
        }
        for (size_t k = 0; k < web->count && !r->failed; k++) {
            web->from[k] = sh_package_warm_get_string(r); web->to[k] = sh_package_warm_get_string(r);
        }
        if (!r->failed) web->extra = sh_package_warm_get_texts(r, &web->extra_count);
    }
    if (!r->failed) webs->owned = sh_package_warm_get_texts(r, &webs->owned_count);
    size_t generated = r->failed ? 0 : sh_package_warm_get_count(r, 16);
    if (!r->failed && generated && !(webs->generated = calloc(generated, sizeof(*webs->generated))))
        sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY);
    for (size_t i = 0; i < generated && !r->failed; i++) {
        sh_package_fsm_declaration *item = webs->generated + webs->generated_count++;
        item->name = sh_package_warm_get_string(r);
        item->text = (char *)sh_package_warm_get_bytes(r, &item->length);
    }
    if (r->failed) { sh_package_fsm_webs_free(webs); return 0; }
    *out = webs; return 1;
}

void sh_package_warm_put_key_groups(sh_package_warm_writer *w, const sh_package_key_groups *groups)
{
    sh_package_warm_u8(w, groups != NULL);
    if (!groups) return;
    pw_size(w, groups->count);
    sh_package_warm_raw(w, groups->defined, sizeof(groups->defined));
    for (size_t i = 0; i < SH_PACKAGE_KEY_PATTERNS; i++)
        for (size_t k = 0; k < sizeof(groups->members[i].words) / sizeof(groups->members[i].words[0]); k++)
            sh_package_warm_u64(w, groups->members[i].words[k]);
    sh_package_warm_u32(w, groups->historical);
}
int sh_package_warm_get_key_groups(sh_package_warm_reader *r, sh_package_key_groups **out)
{
    *out = NULL;
    unsigned present = sh_package_warm_get_u8(r);
    if (r->failed || present > 1) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED); return 0; }
    if (!present) return 1;
    sh_package_key_groups *groups = calloc(1, sizeof(*groups));
    if (!groups) { sh_package_warm_fail(r, SH_PACKAGE_WARM_MEMORY); return 0; }
    groups->count = (size_t)sh_package_warm_get_u64(r);
    const unsigned char *defined = sh_package_warm_get_raw(r, sizeof(groups->defined));
    if (defined) memcpy(groups->defined, defined, sizeof(groups->defined));
    for (size_t i = 0; i < SH_PACKAGE_KEY_PATTERNS && !r->failed; i++)
        for (size_t k = 0; k < sizeof(groups->members[i].words) / sizeof(groups->members[i].words[0]); k++)
            groups->members[i].words[k] = sh_package_warm_get_u64(r);
    groups->historical = sh_package_warm_get_u32(r);
    if (!r->failed && groups->count > SH_PACKAGE_KEY_PATTERNS) sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED);
    for (size_t i = 0; i < SH_PACKAGE_KEY_PATTERNS && !r->failed; i++)
        if (groups->defined[i] > 1) sh_package_warm_fail(r, SH_PACKAGE_WARM_MALFORMED);
    if (r->failed) { free(groups); return 0; }
    *out = groups; return 1;
}

/* ---- Canonical texts for exact comparison ------------------------------------------- */
/* Independent of the encoder above: every field is named again here, and bodies and texts are written whole,
 * so a comparison of two texts compares exact bytes, not digests. */

static void pt_line(sh_package_warm_writer *w, const char *format, ...)
{
    char line[1024];
    va_list args; va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(line)) { w->failed = 1; return; } /* never a truncated piece */
    sh_package_warm_raw(w, line, (size_t)n);
}
static void pt_bytes(sh_package_warm_writer *w, const char *label, const void *bytes, size_t length, int present)
{
    pt_line(w, " %s=%s%zu:", label, present ? "" : "absent/", length);
    if (present && length) sh_package_warm_raw(w, bytes, length);
}
static void pt_text(sh_package_warm_writer *w, const char *label, const char *text)
{ pt_bytes(w, label, text, text ? strlen(text) : 0, text != NULL); }
static void pt_owners(sh_package_warm_writer *w, const char *label, const sh_package_owners *o)
{
    pt_line(w, " %s=%llx/%zu", label, (unsigned long long)o->bits, o->words);
    for (size_t i = 0; i < o->words; i++) pt_line(w, ",%llx", (unsigned long long)o->more[i]);
}
static void pt_json(sh_package_warm_writer *w, const char *label, const sh_json_object *object)
{
    pt_line(w, "\n%s %zu", label, object->count);
    for (size_t i = 0; i < object->count; i++) {
        pt_bytes(w, "key", object->members[i].key, object->members[i].key_length, object->members[i].key != NULL);
        pt_text(w, "value", object->members[i].value_json);
    }
}
char *sh_package_warm_compilation_text(const sh_package_compilation *c, size_t *length)
{
    sh_package_warm_writer w = {0};
    pw_owner_map shared = {0};
    *length = 0;
    if (!c) { pt_line(&w, "compilation none"); goto finish; }
    if (!pw_owner_map_open(&shared, c->resource_count)) { w.failed = 1; goto finish; }
    pt_line(&w, "compilation %zu duplicates %zu composed %zu owns %d overlay %d begin %zu independent %d/%d canonical %d",
        c->resource_count, c->duplicate_count, c->composed_count, c->owns_sources, c->map_overlay, c->map_owner_begin,
        c->independent_sources, c->map_independent_sources, c->canonical_path != NULL);
    pt_json(&w, "requirements", &c->policy.requirements); pt_json(&w, "strings", &c->policy.strings);
    pt_json(&w, "hud", &c->policy.hud);
    for (size_t i = 0; i < c->policy.localized_count; i++) {
        pt_line(&w, "\nlocalized"); pt_text(&w, "key", c->policy.localized[i].key);
        pt_text(&w, "mapping", c->policy.localized[i].mapping); pt_owners(&w, "owners", &c->policy.localized[i].owners);
    }
    pt_line(&w, "\nlegacy"); pt_owners(&w, "owners", &c->policy.legacy_string_owners);
    for (size_t i = 0; i < c->policy.hud_binding_count; i++) {
        pt_line(&w, "\nhud binding %d", c->policy.hud_bindings[i].weapon_mode);
        pt_text(&w, "name", c->policy.hud_bindings[i].name); pt_owners(&w, "owners", &c->policy.hud_bindings[i].owners);
    }
    pt_line(&w, "\nhud bound %d", c->policy.hud_bound);
    for (size_t i = 0; i < c->resource_count; i++) {
        const sh_compiled_resource *r = c->resources + i;
        sh_package_file_identity cache = {0};
        int cached = r->cache_file && sh_package_file_get_identity(r->cache_file, &cache);
        if (r->cache_file && !cached) w.failed = 1; /* an existing file without its identity is no comparable value */
        pt_line(&w, "\nresource");
        pt_text(&w, "path", r->engine_path); pt_text(&w, "type", r->type); pt_text(&w, "name", r->name);
        pt_line(&w, " owns=%d sources=%zu:", r->owns_engine_path, r->source_count);
        for (size_t k = 0; k < r->source_count; k++) pt_line(&w, "%zu,", r->sources[k]);
        pt_owners(&w, "owners", &r->owners); pt_owners(&w, "gameplay", &r->gameplay_owners);
        pt_line(&w, " known=%d composed=%d native=%d restored=%d coverage=%d", r->baseline_known, r->composed,
            r->native_original, r->restored_original, r->semantic_coverage);
        pt_line(&w, " shared=%d", r->body_owner != NULL);
        if (r->body_owner) {
            /* Sharing as structure (the first-use index of the body) and whether a pinned row backs it. */
            size_t slot = pw_owner_slot(&shared, r->body_owner);
            if (!shared.keys[slot]) { shared.keys[slot] = r->body_owner; shared.values[slot] = shared.count++; }
            pt_line(&w, "/%zu backed=%d", shared.values[slot], sh_package_body_backed(r->body_owner));
        }
        if (!r->body && r->body_owner) {
            /* A detached body's bytes, read again exactly: the text states content, not where it is held. */
            size_t length = 0;
            unsigned char *bytes = sh_package_body_read(r->body_owner, &length);
            if (!bytes || length != r->body_length) w.failed = 1;
            pt_bytes(&w, "body", bytes, r->body_length, 1);
            free(bytes);
        } else pt_bytes(&w, "body", r->body, r->body_length, r->body != NULL);
        pt_bytes(&w, "presented", r->presented, r->presented_length, r->presented != NULL);
        pt_line(&w, " source=%zu", r->source);
        pt_text(&w, "cache", r->cache_path);
        pt_line(&w, " file=%d/%llu/", r->cache_file != NULL, (unsigned long long)cache.length);
        for (int b = 0; cached && b < 32; b++) pt_line(&w, "%02x", cache.digest[b]);
        pt_line(&w, " generated=%d layer=%d inputs=%zu:", r->generated, r->generated_layer, r->generated_input_count);
        for (size_t k = 0; k < r->generated_input_count; k++) pt_text(&w, "input", r->generated_inputs[k]);
        pt_line(&w, " input-sources=%zu:", r->generated_source_count);
        for (size_t k = 0; k < r->generated_source_count; k++) pt_line(&w, "%zu,", r->generated_sources[k]);
        pt_text(&w, "original", r->original_path); pt_text(&w, "native-type", r->native_type);
        pt_text(&w, "native-name", r->native_name);
        pt_line(&w, " bound=%zu:", r->bound_dependency_count);
        for (size_t k = 0; k < r->bound_dependency_count; k++) pt_text(&w, "dependency", r->bound_dependencies[k]);
        pt_line(&w, " sound=%u/%u aliases=%zu:", (unsigned)r->sound_authored_event, (unsigned)r->sound_event,
            r->native_alias_count);
        for (size_t k = 0; k < r->native_alias_count; k++) {
            pt_text(&w, "alias-type", r->native_aliases[k].type); pt_text(&w, "alias-name", r->native_aliases[k].name);
        }
        pt_line(&w, " rebound=%d installed=%d/%llu/", r->rebound_original, r->installed_alias,
            (unsigned long long)r->installed_identity.length);
        for (int b = 0; r->installed_alias && b < 32; b++) pt_line(&w, "%02x", r->installed_identity.digest[b]);
    }
finish:
    pw_owner_map_close(&shared);
    pt_line(&w, "\n");
    if (w.failed) { sh_package_warm_writer_free(&w); return NULL; }
    *length = w.length; return (char *)w.bytes;
}
char *sh_package_warm_provenance_text(const sh_package_provenance *p, size_t *length)
{
    sh_package_warm_writer w = {0};
    *length = 0;
    pt_line(&w, "provenance %u contexts %zu bindings %zu uses %zu", p->semantics, p->context_count, p->binding_count,
        p->use_count);
    for (size_t i = 0; i < p->context_count; i++) {
        pt_line(&w, "\ncontext %u", p->contexts[i].mode);
        pt_bytes(&w, "trees", p->contexts[i].trees, p->contexts[i].tree_count * 32, p->contexts[i].trees != NULL);
        pt_text(&w, "component", p->contexts[i].component);
    }
    for (size_t i = 0; i < p->binding_count; i++) {
        pt_line(&w, "\nbinding"); pt_text(&w, "family", p->bindings[i].family);
        pt_text(&w, "original", p->bindings[i].original); pt_text(&w, "name", p->bindings[i].name);
        for (size_t k = 0; k < p->bindings[i].context_count; k++) pt_line(&w, " %zu", p->bindings[i].contexts[k]);
    }
    for (size_t i = 0; i < p->use_count; i++) {
        pt_line(&w, "\nuse %u %zu", p->uses[i].kind, p->uses[i].binding);
        pt_text(&w, "owner", p->uses[i].owner); pt_text(&w, "reference", p->uses[i].reference);
        pt_text(&w, "row", p->uses[i].row);
        for (size_t k = 0; k < p->uses[i].context_count; k++) pt_line(&w, " %zu", p->uses[i].contexts[k]);
    }
    pt_line(&w, "\n");
    if (w.failed) { sh_package_warm_writer_free(&w); return NULL; }
    *length = w.length; return (char *)w.bytes;
}
