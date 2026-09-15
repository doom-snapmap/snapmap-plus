/* Profile reads retain exact bytes and obey native buffer/allocator ownership. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../src/backend/map_source.c"

static struct { unsigned char prefix[0x44]; int heaps[32], depth; } allocator;
static DWORD owner_thread;
static unsigned char image[256];
static void *file_table[40], *memory_table[12];
static void **file_object = file_table, **memory_object = memory_table;
static char *input;
static size_t input_length;
static int borrowed, mode, reads, releases, pushes, pops;
static void *get_heap(void) { return &allocator; }
static void push_heap(void *self, int heap)
{
    assert(self == &allocator && heap == 0); pushes++;
    if (GetCurrentThreadId() != owner_thread) return;
    allocator.heaps[allocator.depth++] = heap;
}
static void pop_heap(void *self)
{
    assert(self == &allocator && allocator.depth == 2); pops++; allocator.depth--;
    if (mode == 5) RaiseException(0xe0422100, 0, 0, NULL);
}
static int file_size(void *self, const char *path, int flags)
{
    assert(self == &file_object && path && flags == 4);
    return mode == 6 ? INT_MAX : (int)input_length;
}
static void read_file(void *self, const char *path, map_source_buffer *out, int zero, int flags)
{
    assert(self == &file_object && !strcmp(path, "doomsnapmaps/published-maps/fixture_map.decl"));
    assert(!zero && flags == 4 && allocator.depth == 2 && !allocator.heaps[1]); reads++;
    if (mode == 1) return;
    out->length = (int)input_length + 1; out->owned = !borrowed;
    if (borrowed) out->data = input;
    else { out->data = HeapAlloc(GetProcessHeap(), 0, input_length + 1); assert(out->data); memcpy(out->data, input, input_length + 1); }
    if (mode == 2) out->length = -1;
    if (mode == 3) RaiseException(0xe0422101, 0, 0, NULL);
    if (mode == 7) out->length--;
}
static void release_file(void *self, void *data, int alignment)
{
    assert(self == &memory_object && alignment == 0x10 && allocator.depth == 2);
    assert(data != input); releases++; HeapFree(GetProcessHeap(), 0, data);
    if (mode == 4) RaiseException(0xe0422102, 0, 0, NULL);
}
static void emit_slot(size_t offset, size_t target)
{
    int32_t displacement = (int32_t)(target - offset - 7);
    memcpy(image + offset, "\x48\x8b\x0d", 3);
    memcpy(image + offset + 3, &displacement, 4);
}
static void reset(void)
{
    memset(&allocator, 0, sizeof(allocator)); allocator.depth = 1; allocator.heaps[0] = 2;
    owner_thread = GetCurrentThreadId(); reads = releases = pushes = pops = mode = borrowed = 0;
    file_table[0xc0 / 8] = read_file; file_table[0x130 / 8] = file_size; memory_table[2] = release_file;
    *(void **)(image + 128) = &memory_object;
    *(void **)(image + 136) = &file_object;
}
static void binding(void)
{
    const uint8_t *base = (const uint8_t *)GetModuleHandleA(NULL);
    sig_result results[5] = {0};
    const char *names[] = {"SnapMapReadSizeLimit", "MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap"};
    void *addresses[] = {image + 0x43, get_heap, push_heap, pop_heap};
    emit_slot(0x1a, 128); emit_slot(0x2a, 136);
    for (int i = 0; i < 4; i++) {
        results[i].name = names[i]; results[i].addr = (uintptr_t)addresses[i];
        results[i].rva = (uint32_t)((const uint8_t *)addresses[i] - base); results[i].status = SIG_OK;
    }
    assert(sh_map_source_install(results, 4, base));
    assert(g_source_files == (void **)(image + 136) && g_source_memory == (void **)(image + 128));
    results[4] = results[0]; assert(!sh_map_source_install(results, 5, base));
    results[1].status = SIG_OK_HOOKED; assert(!sh_map_source_install(results, 4, base));
    results[1].status = SIG_OK; image[0x2a] = 0;
    assert(!sh_map_source_install(results, 4, base)); emit_slot(0x2a, 136);
    assert(sh_map_source_install(results, 4, base));
}
static char *read_source(size_t *length, char error[256])
{ return sh_map_source_read("doomsnapmaps/published-maps/fixture_map.decl", length, error, 256); }
static DWORD WINAPI wrong_thread(void *unused)
{
    char error[256]; size_t length = 99; (void)unused;
    assert(!read_source(&length, error) && !length && error[0]); return 0;
}
int main(void)
{
    char error[256], *copy; size_t length;
    assert(offsetof(map_source_buffer, length) == 8 && offsetof(map_source_buffer, owned) == 12);
    assert((unsigned char *)&allocator.depth - (unsigned char *)&allocator == 0xc4);
    binding(); input_length = 11u * 1024u * 1024u;
    input = HeapAlloc(GetProcessHeap(), 0, input_length + 1); assert(input); memset(input, ' ', input_length); input[input_length] = 0;
    input[0] = '{'; input[input_length - 1] = '}';
    for (int own = 0; own < 2; own++) {
        reset(); borrowed = own; copy = read_source(&length, error);
        assert(copy && length == input_length && copy != input && !error[0]);
        assert(!memcmp(copy, input, length) && !copy[length]); HeapFree(GetProcessHeap(), 0, copy);
        assert(releases == !borrowed && reads == 1 && pops == 1 && allocator.depth == 1);
    }
    for (int fail = 1; fail <= 7; fail++) {
        reset(); mode = fail; length = 99;
        assert(!read_source(&length, error) && !length && error[0]);
        assert(reads == (fail != 6) && releases == (fail != 1 && fail != 6) && pops == 1 && allocator.depth == 1);
    }
    reset(); input[3] = 0;
    assert(!read_source(&length, error) && !length && releases == 1 && allocator.depth == 1);
    input[3] = ' '; reset();
    {
        HANDLE thread = CreateThread(NULL, 0, wrong_thread, NULL, 0, NULL);
        assert(thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0); CloseHandle(thread);
        assert(!reads && !releases && !pops && allocator.depth == 1);
    }
    reset(); copy = read_source(&length, error); assert(copy); HeapFree(GetProcessHeap(), 0, copy);
    HeapFree(GetProcessHeap(), 0, input);
    puts("map_source_test: native ownership, large source, faults, wrong thread and retry passed"); return 0;
}
