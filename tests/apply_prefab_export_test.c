/* Extend the dispatch fixture with owned cross-thread prefab export results. */
#include <windows.h>
static void *export_allocation;
static int export_frees;
static BOOL WINAPI export_heap_free(HANDLE heap, DWORD flags, LPVOID memory)
{
    if (memory && memory == export_allocation) { export_frees++; export_allocation = NULL; }
    return HeapFree(heap, flags, memory);
}
#define HeapFree export_heap_free
#define main dispatch_regressions
#include "apply_dispatch_test.c"
#undef main
#undef HeapFree

static int expected_selection, export_calls;
static size_t export_size, export_reported_size;
static char *export_bytes(int selection, size_t *length)
{
    static const char content[] = "{\"prefab\":\"byte \x97\"}";
    assert(GetCurrentThreadId() == engine_id && selection == expected_selection);
    export_calls++;
    if (delay_execution) {
        SetEvent(entered_event);
        assert(WaitForSingleObject(release_event, 3000) == WAIT_OBJECT_0);
    }
    assert(!export_allocation);
    size_t n = export_size ? export_size : sizeof(content)-1;
    char *out = HeapAlloc(GetProcessHeap(), 0, n+1); assert(out);
    if (export_size) { memset(out, 'q', n); out[0] = '{'; out[n-1] = '}'; out[n] = 0; }
    else memcpy(out, content, sizeof(content));
    *length = export_reported_size ? export_reported_size : n; export_allocation = out;
    return out;
}

static size_t large_length;
static int large_calls;
static int large_execute(int kind, int id, const char *text)
{
    (void)id;
    assert(GetCurrentThreadId() == engine_id && kind == 2);
    assert(strlen(text) == large_length && text[0] == '{' && text[large_length-1] == '}');
    large_calls++; return 1;
}
int main(void)
{
    char out[64];
    unsigned char editor[0x20500] = {0};
    assert(dispatch_regressions() == 0);
    g_editor = editor; *(void **)(editor + ED_MAP_OBJ_OFF) = editor;
    InitializeCriticalSection(&g_pending_lock); g_pending_lock_init = 1;
    g_sync_ev = CreateEventA(NULL, TRUE, FALSE, NULL); assert(g_sync_ev);
    memset(&g_sync_req, 0, sizeof(g_sync_req));
    g_prefab_export_test = export_bytes; append_fails = delay_execution = 0;
    g_buffer_cmd = append_command; g_cmdsys = (void *)1; g_cmd_registered = 2;
    engine_id = GetCurrentThreadId(); expected_selection = 0;
    int length = slot_read_prefab(NULL, out, sizeof(out));
    assert(length > 0 && export_calls == 1 && export_frees == 1 && !export_allocation);
    assert(!slot_read_prefab(NULL, out, length) && !out[0] && export_frees == 2);
    assert(slot_read_prefab(NULL, out, length+1) == length && export_frees == 3);
    g_main_thread_at = NULL;
    assert(!slot_serialize_selection(NULL, out, sizeof(out)) && export_calls == 3);
    g_main_thread_at = (const uint8_t *)&engine_id;
    for (int selection = 0; selection < 2; selection++) for (int delayed = 0; delayed < 2; delayed++) {
        expected_selection = selection; delay_execution = delayed;
        start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        entered_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        release_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE worker = CreateThread(NULL, 0, drain, NULL, CREATE_SUSPENDED, &engine_id); assert(worker);
        ResumeThread(worker); int before = export_frees;
        int result = selection ? slot_serialize_selection(NULL, out, sizeof(out)) : slot_read_prefab(NULL, out, sizeof(out));
        assert(result == (delayed ? 0 : length));
        if (delayed) {
            assert(!out[0] && g_sync_req.state == AE_SYNC_ABANDONED);
            assert(!slot_read_prefab(NULL, out, sizeof(out))); SetEvent(release_event);
        }
        assert(WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0);
        assert(g_sync_req.state == AE_SYNC_EMPTY && !g_sync_req.output && !export_allocation && export_frees == before+1);
        CloseHandle(worker); CloseHandle(start_event); CloseHandle(entered_event); CloseHandle(release_event); start_event = NULL;
    }
    delay_execution = 0; engine_id = GetCurrentThreadId()+1;
    int before = export_calls;
    assert(!slot_read_prefab(NULL, out, sizeof(out)) && !out[0]);
    assert(export_calls == before && g_sync_req.state == AE_SYNC_EMPTY);
    engine_id = GetCurrentThreadId(); expected_selection = 1;
    {
        char *owned = NULL; size_t owned_length = 0;
        export_size = 6u * 1024u * 1024u + 7u;
        assert(slot_prefab_export(NULL, 1, &owned, &owned_length));
        assert(owned_length == export_size && owned[0] == '{' && owned[owned_length-1] == '}' && !owned[owned_length]);
        large_length = owned_length; g_apply_test_executor = large_execute;
        sh_apply_item item = {2, -1, owned};
        assert(slot_schedule_apply(NULL, &item, 1, "large prefab"));
        owned[0] = 'X';
        assert(g_pending_items && g_pending_items[0].text[0] == '{');
        ae_clone_bss_apply_cmd();
        assert(large_calls == 1 && !g_pending_items);
        owned[0] = '{';
        start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        HANDLE worker = CreateThread(NULL, 0, drain, NULL, CREATE_SUSPENDED, &engine_id); assert(worker);
        ResumeThread(worker);
        assert(slot_apply_sync(NULL, &item, 1, "large prefab") == 1);
        assert(WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0 && large_calls == 2);
        CloseHandle(worker); CloseHandle(start_event); start_event = NULL;
        export_heap_free(GetProcessHeap(), 0, owned);
        assert(!export_allocation);
        export_size = 0; engine_id = GetCurrentThreadId();
        export_reported_size = (size_t)INT_MAX + 1;
        owned = (char *)1; owned_length = 7;
        assert(!slot_prefab_export(NULL, 1, &owned, &owned_length) && !owned && !owned_length && !export_allocation);
        export_reported_size = 0;
        assert(!slot_prefab_export(NULL, 2, &owned, &owned_length));
    }
    CloseHandle(g_sync_ev); DeleteCriticalSection(&g_pending_lock);
    puts("apply_prefab_export_test OK"); return 0;
}
