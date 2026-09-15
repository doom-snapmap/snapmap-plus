#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "map_source.h"
#include "process_heap_scope.h"

typedef struct map_source_buffer {
    char *data;
    int length;
    unsigned char owned;
} map_source_buffer;
typedef void (*map_read_fn)(void *, const char *, map_source_buffer *, int, int);
typedef int (*map_size_fn)(void *, const char *, int);
typedef void (*map_free_fn)(void *, void *, int);
static sh_process_heap_api g_source_heap;
static void **g_source_files;
static void **g_source_memory;

static int source_error(char *error, size_t capacity, const char *message)
{
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return 0;
}
static void **source_slot(const uint8_t *instruction)
{
    int32_t displacement;
    if (memcmp(instruction, "\x48\x8b\x0d", 3)) return NULL;
    memcpy(&displacement, instruction + 3, sizeof(displacement));
    return (void **)(instruction + 7 + displacement);
}
int sh_map_source_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    const sig_result *reader = NULL;
    sh_process_heap_api heap;
    void **files = NULL, **memory = NULL;
    size_t i;
    if (!results || !module_base) return 0;
    for (i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, "SnapMapReadSizeLimit")) {
        if (reader) return 0;
        reader = &results[i];
    }
    if (!reader || reader->status != SIG_OK || reader->rva < 0x43 ||
        reader->addr != (uintptr_t)module_base + reader->rva ||
        !sh_process_heap_bind(&heap, results, count, module_base)) return 0;
    /* Both independently audited readers use these RIP-relative global loads
     * before the existing positive-length signature. Check the instructions;
     * never read an absolute global from a renderer-specific address table. */
    __try {
        const uint8_t *entry = (const uint8_t *)reader->addr - 0x43;
        memory = source_slot(entry + 0x1a);
        files = source_slot(entry + 0x2a);
        if (!memory || !files || memory == files) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_source_heap = heap; g_source_files = files; g_source_memory = memory;
    return 1;
}

char *sh_map_source_read(const char *path, size_t *length, char *error, size_t capacity)
{
    sh_process_heap_scope scope = {0};
    map_source_buffer buffer = {0};
    void *memory = NULL, *files = NULL;
    map_free_fn release = NULL;
    char *copy = NULL;
    size_t copied = 0;
    int ok = 0, restored = 1, file_length = 0;
    if (length) *length = 0;
    if (error && capacity) error[0] = 0;
    if (!path || !*path || !length || !g_source_files || !g_source_memory) {
        source_error(error, capacity, "The native map source reader is unavailable."); return NULL;
    }
    __try {
        if (!sh_process_heap_enter(&g_source_heap, &scope)) {
            source_error(error, capacity, "The map source allocator scope is unavailable."); goto done;
        }
        files = *g_source_files; memory = *g_source_memory;
        if (!files || !memory) { source_error(error, capacity, "The native map filesystem is unavailable."); goto done; }
        release = (map_free_fn)(*(void ***)memory)[2];
        if (!release || !(*(void ***)files)[0xc0 / sizeof(void *)] ||
            !(*(void ***)files)[0x130 / sizeof(void *)]) {
            source_error(error, capacity, "The native map filesystem methods are unavailable."); goto done;
        }
        file_length = ((map_size_fn)(*(void ***)files)[0x130 / sizeof(void *)])(files, path, 4);
        if (file_length <= 0 || file_length == INT_MAX) {
            source_error(error, capacity, "The cached map is missing or its native buffer length is not representable."); goto done;
        }
        ((map_read_fn)(*(void ***)files)[0xc0 / sizeof(void *)])(files, path, &buffer, 0, 4);
        /* ReadFile's byte-buffer length includes its added terminator. The
         * filesystem length does not. Require the complete advertised source
         * and exactly the native terminator, rejecting partial/changing reads. */
        if (!buffer.data || buffer.length != file_length + 1 ||
            buffer.data[file_length] || memchr(buffer.data, 0, (size_t)file_length)) {
            source_error(error, capacity, "The cached map read is incomplete or contains an invalid terminator."); goto done;
        }
        copied = (size_t)file_length;
        copy = HeapAlloc(GetProcessHeap(), 0, copied + 1);
        if (!copy) { source_error(error, capacity, "Map source allocation failed."); goto done; }
        memcpy(copy, buffer.data, copied); copy[copied] = 0; ok = 1;
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        source_error(error, capacity, "Reading the native map source raised an exception."); ok = 0;
    }
    if (buffer.data && buffer.owned) {
        __try {
            if (release) release(memory, buffer.data, 0x10);
            else ok = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            source_error(error, capacity, "Native map source release raised an exception."); ok = 0;
        }
    }
    restored = sh_process_heap_leave(&scope);
    if (!restored) { source_error(error, capacity, "The map source allocator scope could not be restored."); ok = 0; }
    if (!ok) { if (copy) HeapFree(GetProcessHeap(), 0, copy); return NULL; }
    *length = copied; return copy;
}
