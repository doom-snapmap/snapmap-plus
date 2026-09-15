/* Native selection ownership outlives callback inputs; failed decode cannot launch. */
#include <assert.h>
#include "../src/backend/map_native.c"

static struct { unsigned char prefix[0x44]; int heaps[32], depth; } allocator;
static DWORD owner;
static int mode, copied, destroyed, strings, snapshots, parses, launches;
static unsigned char complete_code[0x100], selection_code[0x100];
static char source[] = "{\"~type\":\"idSnapMap\",\"name\":\"retained\"}";
static void fail(int at) { if (mode == at) RaiseException(0xe0427300u + (DWORD)at, 0, 0, NULL); }
static void *get_heap(void) { return &allocator; }
static void push_heap(void *self, int heap)
{
    assert(self == &allocator && !heap);
    if (GetCurrentThreadId() == owner) allocator.heaps[allocator.depth++] = 0;
}
static void pop_heap(void *self) { assert(self == &allocator && allocator.depth > 1); allocator.depth--; }
static void *string_ctor(void *out)
{
    memset(out, 0, MAP_STRING_SIZE); strings++;
    *(char **)((char *)out + 0x10) = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1);
    assert(*(char **)((char *)out + 0x10)); return out;
}
static void string_set(void *out, const char *value)
{
    size_t length = strlen(value);
    char *body = HeapAlloc(GetProcessHeap(), 0, length + 1); assert(body); memcpy(body, value, length + 1);
    HeapFree(GetProcessHeap(), 0, *(void **)((char *)out + 0x10));
    *(char **)((char *)out + 0x10) = body; *(int *)((char *)out + 8) = (int)length;
}
static void string_free(void *out)
{
    HeapFree(GetProcessHeap(), 0, *(void **)((char *)out + 0x10)); strings--; fail(5);
}
static void *metadata_copy(void *out, const void *in)
{
    assert(allocator.depth == 2); fail(1);
    memset(out, 0, MAP_METADATA_SIZE);
    for (size_t offset = 0; offset <= 0x60; offset += MAP_STRING_SIZE) {
        string_ctor((char *)out + offset);
        string_set((char *)out + offset, *(char *const *)((const char *)in + offset + 0x10));
    }
    copied++; return out;
}
static void metadata_free(void *out)
{
    for (size_t offset = 0; offset <= 0x60; offset += MAP_STRING_SIZE) string_free((char *)out + offset);
    destroyed++; fail(8);
}
static void build_path(const void *metadata, void *out)
{
    assert(!strcmp(*(char *const *)((const char *)metadata + 0x10), "map-id")); fail(2);
    string_set(out, "doomsnapmaps/published-maps/map-id_revision_map.decl");
}
char *sh_map_source_read(const char *path, size_t *length, char *error, size_t capacity)
{
    char *copy;
    assert(allocator.depth == 2 && !strcmp(path, "doomsnapmaps/published-maps/map-id_revision_map.decl"));
    if (mode == 3) { native_error(error, capacity, "missing source"); return NULL; }
    *length = strlen(source); copy = HeapAlloc(GetProcessHeap(), 0, *length + 1); assert(copy);
    memcpy(copy, source, *length + 1); return copy;
}
static void *snapshot_ctor(void *out)
{
    assert(allocator.depth == 2); fail(4);
    memset(out, 0, MAP_SNAPSHOT_SIZE); *(int *)out = 0x6f; snapshots++; return out;
}
static void snapshot_free(void *out)
{
    assert(*(int *)out == 0x6f && allocator.depth == 2); snapshots--; fail(7);
}
static unsigned char parse_map(const char *json, void *out)
{
    assert(*(int *)out == 0x6f && allocator.depth == 2); parses++; fail(6);
    assert(!strstr(json, "missing/boss"));
    return mode != 9 && (strstr(json, "retained") != NULL || strstr(json, "snapSlotSettings") != NULL);
}
static unsigned char serialize_map(const void *map, void *out, unsigned char compact)
{
    assert(map && compact); fail(13);
    string_set(out, "{\"~type\":\"idSnapMap\",\"~version\":110}"); return mode != 12;
}
static int native_enter(void *context, const void *metadata, void *snapshot)
{
    assert(context == &launches && *(int *)snapshot == 0x6f && allocator.depth == 2);
    assert(!strcmp(*(char *const *)((const char *)metadata + 0x70), "original title"));
    launches++; fail(10); return mode != 11;
}
static void emit_call(unsigned char *site, void *target)
{
    intptr_t distance = (unsigned char *)target - site - 5;
    int32_t displacement = (int32_t)distance;
    assert(distance == displacement); *site = 0xe8; memcpy(site + 1, &displacement, 4);
}
static void binding(void)
{
    const uint8_t *base = (const uint8_t *)GetModuleHandleA(NULL);
    const char *names[] = {"PublishedMapComplete", "PublishedMapSelectionConstruct", "PublishedMapMetadataDestroy",
        "DeserializeFromJson", "MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap", "SerializeToJson"};
    void *addresses[] = {complete_code, selection_code, metadata_free, parse_map, get_heap, push_heap, pop_heap, serialize_map};
    sig_result results[9] = {0};
    emit_call(complete_code + 0x44, snapshot_ctor); emit_call(complete_code + 0x4f, string_ctor);
    emit_call(complete_code + 0x5e, build_path); emit_call(complete_code + 0xb9, string_free);
    emit_call(complete_code + 0xc4, snapshot_free); emit_call(selection_code + 0x69, metadata_copy);
    memcpy(selection_code + 0x45, "\xb9\x90\x05\x00\x00", 5);
    for (int i = 0; i < 8; i++) {
        results[i].name = names[i]; results[i].addr = (uintptr_t)addresses[i];
        results[i].rva = (uint32_t)((const uint8_t *)addresses[i] - base); results[i].status = SIG_OK;
    }
    assert(sh_map_native_bind(results, 8, base));
    assert(g_map_native.metadata == metadata_copy && g_map_native.snapshot_free == snapshot_free);
    results[8] = results[0]; assert(!sh_map_native_bind(results, 9, base));
    results[2].status = SIG_OK_HOOKED; assert(!sh_map_native_bind(results, 8, base)); results[2].status = SIG_OK;
    selection_code[0x45] = 0; assert(!sh_map_native_bind(results, 8, base)); selection_code[0x45] = 0xb9;
    complete_code[0xc4] = 0; assert(!sh_map_native_bind(results, 8, base)); complete_code[0xc4] = 0xe8;
    assert(sh_map_native_bind(results, 8, base));
}
static DWORD WINAPI wrong_thread(void *context)
{
    sh_map_native_selection *selection = context;
    char error[256];
    assert(!sh_map_native_capture(selection->metadata, error, sizeof(error)) && error[0]);
    assert(!sh_map_native_enter(selection, source, native_enter, &launches, error, sizeof(error)) && error[0]);
    assert(!sh_map_native_session(selection, native_enter, &launches, error, sizeof(error)) && error[0]);
    assert(!sh_map_native_read_session(source, (void *)1, error, sizeof(error)) && error[0]);
    assert(!sh_map_native_release(&selection) && selection == context); return 0;
}
int main(void)
{
    unsigned char original[MAP_METADATA_SIZE] = {0};
    char error[256]; size_t length;
    sh_map_native_selection *selection;
    owner = GetCurrentThreadId(); allocator.depth = 1; allocator.heaps[0] = 2;
    binding();
    string_ctor(original); string_set(original, "map-id");
    string_ctor(original + 0x30); string_set(original + 0x30, "revision");
    string_ctor(original + 0x60); string_set(original + 0x60, "original title");
    for (mode = 1; mode <= 3; mode++) {
        int before = strings;
        assert(!sh_map_native_capture(original, error, sizeof(error)) && error[0]);
        assert(strings == before && allocator.depth == 1 && !snapshots);
    }
    mode = 0; selection = sh_map_native_capture(original, error, sizeof(error)); assert(selection && !error[0]);
    assert(sh_map_native_matches(selection, original));
    assert(sh_map_native_source(selection, &length) != source && length == strlen(source));
    string_set(original + 0x30, "changed revision"); assert(!sh_map_native_matches(selection, original));
    string_set(original + 0x30, "revision"); string_set(original + 0x60, "edited title");
    assert(sh_map_native_matches(selection, original));
    metadata_free(original); assert(strings == 3);
    for (int failure = 0; failure <= 11; failure++) {
        if (failure == 1 || failure == 2 || failure == 3 || failure == 5 || failure == 8) continue;
        mode = failure; int before = launches;
        int ok = sh_map_native_enter(selection, sh_map_native_source(selection, NULL), native_enter, &launches, error, sizeof(error));
        assert(ok == (failure == 0)); assert((error[0] != 0) == (failure != 0));
        assert(launches == before + (failure == 0 || failure == 7 || failure == 10 || failure == 11));
        assert(!snapshots && allocator.depth == 1 && strings == 3);
    }
    mode = 0;
    {
        static char session_source[] = "{\"~type\":\"idSnapMap\",\"~version\":91,\"entities\":[{\"inherit\":\"missing/boss\"}],\"snapSlotSettings\":["
            "{\"~type\":\"snapLobbySlotSetting_t\",\"race\":0,\"state\":0,\"team\":0},"
            "{\"~type\":\"snapLobbySlotSetting_t\",\"race\":0,\"state\":0,\"team\":0},"
            "{\"~type\":\"snapLobbySlotSetting_t\",\"race\":0,\"state\":0,\"team\":1},"
            "{\"~type\":\"snapLobbySlotSetting_t\",\"race\":0,\"state\":0,\"team\":1}]}";
        char *saved = selection->json;
        selection->json = session_source;
        for (int failure = 0; failure <= 13; failure++) {
            if (failure == 1 || failure == 2 || failure == 3 || failure == 8) continue;
            mode = failure;
            assert(sh_map_native_session(selection, native_enter, &launches, error, sizeof(error)) == !failure);
            assert(!snapshots && strings == 3 && allocator.depth == 1);
        }
        selection->json = saved; mode = 0;
        {
            unsigned char snapshot[MAP_SNAPSHOT_SIZE] = {0};
            char unchanged[sizeof(session_source)];
            memcpy(unchanged, session_source, sizeof(unchanged));
            *(int *)snapshot = 0x6f;
            /* The native reader, rather than this adapter, owns construction
             * and destruction. No callback may see a half-decoded snapshot. */
            for (int failure = 0; failure <= 13; failure++) {
                if (failure && failure != 5 && failure != 6 && failure != 9 && failure != 12 && failure != 13) continue;
                int before = launches;
                mode = failure;
                assert(sh_map_native_read_session(session_source, snapshot, error, sizeof(error)) == !failure);
                assert((error[0] != 0) == (failure != 0));
                assert(!snapshots && strings == 3 && allocator.depth == 1 && launches == before);
                assert(!memcmp(session_source, unchanged, sizeof(unchanged)));
            }
            mode = 0;
            assert(!sh_map_native_read_session(NULL, snapshot, error, sizeof(error)) && error[0]);
            assert(!sh_map_native_read_session(session_source, NULL, error, sizeof(error)) && error[0]);
            assert(!sh_map_native_read_session("{}", snapshot, error, sizeof(error)) && error[0]);
            assert(!snapshots && strings == 3 && allocator.depth == 1);
        }
    }
    {
        unsigned char level[0x17100] = {0}, meta[MAP_METADATA_SIZE] = {0};
        sh_map_native_selection match = {0};
        const char name[] = "example", path[] = "snapmap/example";
        match.metadata = meta;
        *(int *)(meta + 0x398 + 8) = 7; *(const char **)(meta + 0x398 + 0x10) = name;
        *(int *)(level + 0x20 + 8) = 15; *(const char **)(level + 0x20 + 0x10) = path;
        assert(!sh_map_native_matches_level(&match, level));
        level[0x170f8] = 2; assert(sh_map_native_matches_level(&match, level));
        *(int *)(level + 0x20 + 8) = 8; assert(!sh_map_native_matches_level(&match, level));
    }
    HANDLE thread = CreateThread(NULL, 0, wrong_thread, selection, 0, NULL);
    assert(thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0); CloseHandle(thread);
    assert(sh_map_native_release(&selection) && !selection && !strings);
    assert(sh_map_native_release(&selection));
    assert(allocator.depth == 1 && copied + 1 == destroyed);
    puts("map_native_test: independent metadata/source, decode/refusal/fault cleanup and thread ownership passed");
    return 0;
}
