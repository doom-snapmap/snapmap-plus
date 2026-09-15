#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map_native.h"
#include "map_source.h"
#include "map_session_json.h"
#include "process_heap_scope.h"

#define MAP_METADATA_SIZE 0x588
#define MAP_SNAPSHOT_SIZE 0x770
#define MAP_STRING_SIZE 0x30
typedef void *(*native_construct_fn)(void *);
typedef void *(*native_copy_fn)(void *, const void *);
typedef void (*native_destroy_fn)(void *);
typedef void (*native_path_fn)(const void *, void *);
typedef unsigned char (*native_parse_fn)(const char *, void *);
typedef unsigned char (*native_serialize_fn)(const void *, void *, unsigned char);
static struct {
    sh_process_heap_api heap;
    native_construct_fn snapshot, string;
    native_copy_fn metadata;
    native_destroy_fn snapshot_free, metadata_free, string_free;
    native_path_fn path;
    native_parse_fn parse;
    native_serialize_fn serialize;
} g_map_native;

struct sh_map_native_selection {
    unsigned char *metadata;
    char *json;
    size_t length;
    DWORD thread;
};

static int native_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return 0;
}

static const uint8_t *native_site(const sig_result *results, size_t count,
    const uint8_t *base, const char *name)
{
    const uint8_t *site = NULL;
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name)) {
        if (site || results[i].status != SIG_OK || !results[i].addr ||
            results[i].addr != (uintptr_t)base + results[i].rva) return NULL;
        site = (const uint8_t *)results[i].addr;
    }
    return site;
}

static void *native_call(const uint8_t *site)
{
    int32_t displacement;
    if (*site != 0xe8) return NULL;
    memcpy(&displacement, site + 1, sizeof(displacement));
    return (void *)(site + 5 + displacement);
}

int sh_map_native_bind(const sig_result *results, size_t count, const uint8_t *base)
{
    const uint8_t *complete, *selection, *destroy, *parse, *serialize;
    sh_process_heap_api heap;
    void *calls[6];
    static const size_t offsets[] = {0x44, 0x4f, 0x5e, 0xb9, 0xc4};
    if (!results || !base || !sh_process_heap_bind(&heap, results, count, base)) return 0;
    complete = native_site(results, count, base, "PublishedMapComplete");
    selection = native_site(results, count, base, "PublishedMapSelectionConstruct");
    destroy = native_site(results, count, base, "PublishedMapMetadataDestroy");
    parse = native_site(results, count, base, "DeserializeFromJson");
    serialize = native_site(results, count, base, "SerializeToJson");
    if (!complete || !selection || !destroy || !parse || !serialize) return 0;
    __try {
        /* The completion constructs its stack snapshot and idStr, builds the
         * profile path, then destroys both. The selection record constructs a
         * deep metadata copy after its eight-byte reference-count prefix. */
        if (memcmp(selection + 0x45, "\xb9\x90\x05\x00\x00", 5)) return 0;
        for (size_t i = 0; i < 5; i++) if (!(calls[i] = native_call(complete + offsets[i]))) return 0;
        if (!(calls[5] = native_call(selection + 0x69))) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_map_native.heap = heap;
    g_map_native.snapshot = (native_construct_fn)calls[0];
    g_map_native.string = (native_construct_fn)calls[1];
    g_map_native.path = (native_path_fn)calls[2];
    g_map_native.string_free = (native_destroy_fn)calls[3];
    g_map_native.snapshot_free = (native_destroy_fn)calls[4];
    g_map_native.metadata = (native_copy_fn)calls[5];
    g_map_native.metadata_free = (native_destroy_fn)destroy;
    g_map_native.parse = (native_parse_fn)parse;
    g_map_native.serialize = (native_serialize_fn)serialize;
    return 1;
}

static const char *native_string(const void *string, int *length)
{
    const unsigned char *s = string;
    const char *data;
    if (!s) return NULL;
    *length = *(const int *)(s + 8);
    data = *(const char *const *)(s + 0x10);
    if (*length < 0 || !data || data[*length] || memchr(data, 0, (size_t)*length)) return NULL;
    return data;
}

sh_map_native_selection *sh_map_native_capture(const void *metadata, char *error, size_t capacity)
{
    sh_process_heap_scope scope = {0};
    sh_map_native_selection *owned = NULL;
    unsigned char path[MAP_STRING_SIZE];
    int initialized = 0, path_initialized = 0, ok = 0;
    if (error && capacity) error[0] = 0;
    if (!metadata || !g_map_native.metadata) {
        native_error(error, capacity, "The native published-map metadata is unavailable."); return NULL;
    }
    __try {
        const char *name;
        int length;
        if (!sh_process_heap_enter(&g_map_native.heap, &scope)) {
            native_error(error, capacity, "The published-map allocator scope is unavailable."); goto done;
        }
        owned = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owned));
        if (!owned || !(owned->metadata = HeapAlloc(GetProcessHeap(), 0, MAP_METADATA_SIZE))) {
            native_error(error, capacity, "Published-map metadata allocation failed."); goto done;
        }
        owned->thread = GetCurrentThreadId();
        g_map_native.metadata(owned->metadata, metadata); initialized = 1;
        g_map_native.string(path); path_initialized = 1;
        g_map_native.path(owned->metadata, path);
        name = native_string(path, &length);
        if (!name || !length) {
            native_error(error, capacity, "The published map has no valid cached source path."); goto done;
        }
        owned->json = sh_map_source_read(name, &owned->length, error, capacity);
        ok = owned->json != NULL;
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        native_error(error, capacity, "Retaining the published map raised an exception."); ok = 0;
    }
    if (path_initialized) {
        __try { g_map_native.string_free(path); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = native_error(error, capacity, "Native map path cleanup failed."); }
    }
    if (!ok && owned && initialized) {
        __try { g_map_native.metadata_free(owned->metadata); }
        __except (EXCEPTION_EXECUTE_HANDLER) { native_error(error, capacity, "Native map metadata cleanup failed."); }
        initialized = 0;
    }
    if (!sh_process_heap_leave(&scope)) {
        ok = native_error(error, capacity, "The published-map allocator scope could not be restored.");
        /* Do not run another native destructor on an unknown heap stack. */
    }
    if (!ok) {
        if (owned) {
            if (owned->json) HeapFree(GetProcessHeap(), 0, owned->json);
            if (owned->metadata) HeapFree(GetProcessHeap(), 0, owned->metadata);
            HeapFree(GetProcessHeap(), 0, owned);
        }
        return NULL;
    }
    return owned;
}

const char *sh_map_native_source(const sh_map_native_selection *selection, size_t *length)
{
    if (length) *length = selection ? selection->length : 0;
    return selection ? selection->json : NULL;
}

int sh_map_native_matches_ids(const sh_map_native_selection *selection, const void *id, const void *revision)
{
    if (!selection || !id || !revision) return 0;
    __try {
        for (size_t offset = 0; offset <= MAP_STRING_SIZE; offset += MAP_STRING_SIZE) {
            int a_length, b_length;
            const char *a = native_string(selection->metadata + offset, &a_length);
            const char *b = native_string(offset ? revision : id, &b_length);
            if (!a || !b || (!offset && !a_length) || a_length != b_length || memcmp(a, b, (size_t)a_length)) return 0;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int sh_map_native_matches(const sh_map_native_selection *selection, const void *metadata)
{
    return metadata && sh_map_native_matches_ids(selection, metadata, (const unsigned char *)metadata + MAP_STRING_SIZE);
}

int sh_map_native_matches_level(const sh_map_native_selection *selection, const void *parameters)
{
    if (!selection || !parameters) return 0;
    __try {
        int map_length, name_length;
        const char *map = native_string((const unsigned char *)parameters + 0x20, &map_length);
        const char *name = native_string(selection->metadata + 0x398, &name_length);
        return (*(const unsigned char *)((const unsigned char *)parameters + 0x170f8) & 2) &&
            map && name && name_length > 0 && map_length > 8 && map_length - 8 == name_length &&
            !memcmp(map, "snapmap/", 8) && !memcmp(map + 8, name, (size_t)name_length);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* The caller has entered heap zero and owns an empty constructed snapshot. */
static int native_session_parse(const char *json, void *snapshot, char *error, size_t capacity)
{
    unsigned char defaults[MAP_STRING_SIZE];
    char *projection = NULL;
    int initialized = 0, ok = 0;
    __try {
        const char *default_json;
        int default_length;
        g_map_native.string(defaults); initialized = 1;
        if (!g_map_native.serialize(snapshot, defaults, 1) ||
            !(default_json = native_string(defaults, &default_length))) {
            native_error(error, capacity, "The native empty map could not be serialized."); goto done;
        }
        projection = sh_map_session_json(json, strlen(json), default_json,
            (size_t)default_length, error, capacity);
        if (!projection) goto done;
        ok = g_map_native.parse(projection, snapshot) != 0;
        if (!ok) native_error(error, capacity, "The published map's lobby settings could not be decoded.");
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = native_error(error, capacity, "Published-map session decoding raised an exception.");
    }
    free(projection);
    if (initialized) {
        __try { g_map_native.string_free(defaults); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = native_error(error, capacity, "Native empty map text cleanup failed."); }
    }
    return ok;
}

int sh_map_native_read_session(const char *json, void *snapshot, char *error, size_t capacity)
{
    sh_process_heap_scope scope = {0};
    int ok;
    if (error && capacity) error[0] = 0;
    if (!json || !snapshot || !g_map_native.serialize || !g_map_native.parse)
        return native_error(error, capacity, "The initial published-map snapshot is unavailable.");
    if (!sh_process_heap_enter(&g_map_native.heap, &scope))
        return native_error(error, capacity, "The map snapshot allocator scope is unavailable.");
    ok = native_session_parse(json, snapshot, error, capacity);
    if (!sh_process_heap_leave(&scope))
        ok = native_error(error, capacity, "The map snapshot allocator scope could not be restored.");
    return ok;
}

static int native_decode(const sh_map_native_selection *selection, const char *json, int session_only,
    sh_map_native_enter_fn enter, void *context, char *error, size_t capacity)
{
    sh_process_heap_scope scope = {0};
    unsigned char *snapshot = NULL;
    int initialized = 0, ok = 0;
    if (error && capacity) error[0] = 0;
    if (!selection || !json || !enter || selection->thread != GetCurrentThreadId())
        return native_error(error, capacity, "The retained map must enter on its original engine thread.");
    __try {
        if (!sh_process_heap_enter(&g_map_native.heap, &scope)) {
            native_error(error, capacity, "The map snapshot allocator scope is unavailable."); goto done;
        }
        snapshot = HeapAlloc(GetProcessHeap(), 0, MAP_SNAPSHOT_SIZE);
        if (!snapshot) { native_error(error, capacity, "Map snapshot allocation failed."); goto done; }
        g_map_native.snapshot(snapshot); initialized = 1;
        if (session_only ? !native_session_parse(json, snapshot, error, capacity) : !g_map_native.parse(json, snapshot)) {
            if (session_only) goto done;
            native_error(error, capacity, "The retained published map could not be decoded."); goto done;
        }
        ok = enter(context, selection->metadata, snapshot);
        if (!ok) native_error(error, capacity, "The native published-map launch was refused.");
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = native_error(error, capacity, "Published-map decoding or launch raised an exception.");
    }
    if (initialized) {
        __try { g_map_native.snapshot_free(snapshot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = native_error(error, capacity, "Native map snapshot cleanup failed."); }
    }
    if (snapshot) HeapFree(GetProcessHeap(), 0, snapshot);
    if (!sh_process_heap_leave(&scope))
        ok = native_error(error, capacity, "The map snapshot allocator scope could not be restored.");
    return ok;
}

int sh_map_native_enter(const sh_map_native_selection *selection, const char *json,
    sh_map_native_enter_fn enter, void *context, char *error, size_t capacity)
{
    return native_decode(selection, json, 0, enter, context, error, capacity);
}
int sh_map_native_session(const sh_map_native_selection *selection,
    sh_map_native_enter_fn enter, void *context, char *error, size_t capacity)
{
    return native_decode(selection, sh_map_native_source(selection, NULL), 1, enter, context, error, capacity);
}

int sh_map_native_release(sh_map_native_selection **selection)
{
    sh_process_heap_scope scope = {0};
    sh_map_native_selection *owned;
    int ok = 1;
    if (!selection || !*selection) return 1;
    owned = *selection;
    if (owned->thread != GetCurrentThreadId() || !sh_process_heap_enter(&g_map_native.heap, &scope)) return 0;
    /* Clear before destruction: a hardware exception cannot establish which
     * children were freed, so repeating the destructor risks double free. */
    *selection = NULL;
    __try { g_map_native.metadata_free(owned->metadata); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    HeapFree(GetProcessHeap(), 0, owned->metadata);
    HeapFree(GetProcessHeap(), 0, owned->json);
    HeapFree(GetProcessHeap(), 0, owned);
    if (!sh_process_heap_leave(&scope)) ok = 0;
    return ok;
}
