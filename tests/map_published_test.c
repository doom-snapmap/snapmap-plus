/* Published inspection, retained launch, stale selection and grouped hook failure. */
#include <assert.h>
#include "../src/backend/map_published.c"

struct sh_map_native_selection { unsigned char metadata[0x60]; char json[64]; };
static unsigned char manager[0xa40], shell[0x20], cache[0x24470], metadata[0x60];
static unsigned char history_record[0x38], history_object[0x590], host_parameters[0x680];
static void *system_table[8], *cache_table[64], *session_table[104], *lobby_table[88], *progress_table[8];
static void **system_object = system_table, **session_object = session_table, **lobby_object = lobby_table, **progress_object = progress_table;
static void *shell_slot = shell, *session_slot = &session_object, *history_slot = history_record;
static DWORD thread_id;
static int history_count = 1, boundary, mode, allocated, released, errors, prepared, decoded, calls[4], hidden;
static int inspection, read_action, throws_read, hook_prepares, hook_commits, hook_removes, fail_prepare, fail_commit, fail_remove;
static unsigned char bound_sites[9][512];
static __declspec(thread) sh_rawmap_read_scope *read_scope;
static sh_rawmap_request retained;
static sh_rawmap_loading retained_loading;
static char retained_json[64];
static void *call_callbacks[PUB_HOOKS];

void backend_log(const char *text) { (void)text; }
void sh_mpkg_report_error(const char *text) { assert(text && *text); errors++; }
int sh_decl_server_map_boundary_safe(void) { return boundary && thread_id == GetCurrentThreadId(); }
int sh_decl_server_map_preparation_ready(void) { return boundary && thread_id == GetCurrentThreadId(); }
int sh_decl_server_map_browser_present(void)
{ return *(int *)(manager + 8) == 63 && *(int *)(manager + 12) == 63; }
int sh_map_render_capture(void *snapshot, sh_map_render *settings)
{ assert(snapshot && settings); memset(settings, 0, sizeof(*settings)); return 1; }
void sh_map_render_select(const sh_map_render *settings) { assert(settings); }
void sh_rawmap_inspection_enter(void) { inspection++; }
void sh_rawmap_inspection_leave(void) { assert(inspection > 0); inspection--; }
void sh_rawmap_read_enter(sh_rawmap_read_scope *scope, const void *caller)
{
    assert((caller != NULL) == (thread_id == GetCurrentThreadId()));
    scope->previous = read_scope; scope->return_address = caller; read_scope = scope;
    sh_rawmap_inspection_enter();
}
void sh_rawmap_read_leave(sh_rawmap_read_scope *scope)
{
    assert(read_scope == scope); read_scope = scope->previous; sh_rawmap_inspection_leave();
}
sh_map_native_selection *sh_map_native_capture(const void *input, char *error, size_t capacity)
{
    sh_map_native_selection *owned;
    if (mode == 1) { snprintf(error, capacity, "capture refused"); return NULL; }
    owned = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owned)); assert(owned);
    memcpy(owned->metadata, input, sizeof(owned->metadata)); strcpy_s(owned->json, sizeof(owned->json), "retained source");
    allocated++; return owned;
}
const char *sh_map_native_source(const sh_map_native_selection *selection, size_t *length)
{ if (length) *length = strlen(selection->json); return selection->json; }
int sh_map_native_matches_ids(const sh_map_native_selection *selection, const void *id, const void *revision)
{ return selection && id && revision && *(const int *)id == *(int *)selection->metadata && *(const int *)revision == *(int *)(selection->metadata + 0x30); }
int sh_map_native_matches(const sh_map_native_selection *selection, const void *input)
{ return sh_map_native_matches_ids(selection, input, (const unsigned char *)input + 0x30); }
int sh_map_native_matches_level(const sh_map_native_selection *selection, const void *parameters)
{ assert(selection); return parameters == (void *)0x777; }
int sh_map_native_enter(const sh_map_native_selection *selection, const char *json,
    sh_map_native_enter_fn enter, void *context, char *error, size_t capacity)
{
    int snapshot = 123;
    assert(inspection && !strcmp(json, "retained source")); decoded++;
    if (mode == 3) { snprintf(error, capacity, "decode refused"); return 0; }
    return enter(context, selection->metadata, &snapshot);
}
int sh_map_native_session(const sh_map_native_selection *selection,
    sh_map_native_enter_fn enter, void *context, char *error, size_t capacity)
{
    int snapshot = 0;
    assert(inspection); decoded++;
    if (mode == 3) { snprintf(error, capacity, "session refused"); return 0; }
    return enter(context, selection->metadata, &snapshot);
}
int sh_map_native_release(sh_map_native_selection **selection)
{
    if (!selection || !*selection) return 1;
    if (mode == 4) return 0;
    released++; HeapFree(GetProcessHeap(), 0, *selection); *selection = NULL; return 1;
}
int sh_rawmap_preflight_request(const char *json, size_t length, const sh_rawmap_request *request, char *error, size_t capacity)
{
    prepared++;
    assert(request->valid(request->context) && length == strlen(json));
    if (mode == 2) { snprintf(error, capacity, "preflight refused"); return 0; }
    if (retained.release) retained.release(retained.context);
    retained = *request; strcpy_s(retained_json, sizeof(retained_json), json); return 1;
}
int sh_rawmap_preflight_loading_request(const char *json, size_t length, const sh_rawmap_request *request,
    const sh_rawmap_loading *loading, char *error, size_t capacity)
{
    assert(loading && loading->waiting && loading->matches && loading->select);
    retained_loading = *loading;
    return sh_rawmap_preflight_request(json, length, request, error, capacity);
}
static int finish(int consent)
{
    sh_rawmap_request request = retained;
    int result = 0;
    memset(&retained, 0, sizeof(retained));
    if (request.enter && consent && request.valid(request.context)) result = request.enter(request.context, retained_json) == 0;
    if (result) {
        assert(retained_loading.waiting(request.context));
        assert(!retained_loading.matches(request.context, (void *)0x778));
        assert(retained_loading.matches(request.context, (void *)0x777));
        result = retained_loading.select(request.context);
    }
    if (request.release) request.release(request.context);
    return result;
}
int sh_rawmap_cancel_pending_map(void) { if (g_pub_delegating) return 0; (void)finish(0); return 1; }
static void *get_system(void) { return &system_object; }
static void *get_cache(void *self) { assert(self == &system_object); return cache; }
static void set_cache(void *self, const void *input, void *snapshot)
{
    assert(self == cache && snapshot);
    memcpy(cache + 0x24408, input, 0x30); memcpy(cache + 0x24438, (const char *)input + 0x30, 0x30);
}
static void *get_lobby(void *self) { assert(self == &session_object); return &lobby_object; }
static void *get_parameters(void *self) { assert(self == &lobby_object); return host_parameters; }
static void hide_progress(void *self, int zero) { assert(self == &progress_object && !zero); hidden++; }
static void original_offline(void *self, const void *input, void *snapshot)
{
    assert(self == manager && input && snapshot); calls[PUB_OFFLINE]++;
    if (mode == 6) pub_direct(self, input, snapshot);
    if (mode == 7) {
        unsigned char other[0x60]; memcpy(other, input, sizeof(other)); *(int *)other = 43;
        pub_direct(self, other, snapshot);
    }
}
static unsigned char original_lobby(void *self, unsigned char option)
{ assert(self == manager && option == 1); calls[PUB_LOBBY]++; return mode != 5; }
static void original_direct(void *self, const void *input, void *snapshot)
{ assert(self == manager && input && snapshot); calls[PUB_DIRECT]++; }
static void original_ready(void *self, void *download, const void *input, void *snapshot)
{ assert(self == manager && input && snapshot); if (g_pub_ready) assert(!download); calls[PUB_READY]++; }
static void original_complete(void *download)
{
    assert(download == (void *)9);
    if (g_pub_ready) assert(read_scope && read_scope->return_address ==
        (thread_id == GetCurrentThreadId() ? g_pub_profile_return : NULL));
    if (throws_read) RaiseException(0xe0427310, 0, 0, NULL);
    if (read_action == 1) pub_offline(manager, metadata, (void *)1);
    if (read_action == 2) pub_direct(manager, metadata, (void *)1);
}
static void *original_read(void *client, const char *id, const char *revision, void *success, void *failure)
{
    assert(client == (void *)1 && !strcmp(id, "id") && !strcmp(revision, "revision") && success == (void *)2 && failure == (void *)3);
    if (g_pub_ready) assert(read_scope && read_scope->return_address ==
        (thread_id == GetCurrentThreadId() ? g_pub_read_return : NULL));
    {
        sh_rawmap_read_scope *outer = read_scope;
        pub_complete((void *)9); assert(read_scope == outer);
    }
    return (void *)10;
}
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{
    (void)base; if (status) *status = GLB_OK;
    if (!strcmp(name, "shell_ptr_slot")) return (uintptr_t)&shell_slot;
    if (!strcmp(name, "main_thread_id")) return (uintptr_t)&thread_id;
    return 0;
}
void *hook_prepare(void *target, void *detour, size_t stolen)
{
    (void)detour; assert(stolen >= 14 && stolen <= 21); hook_prepares++;
    if (fail_prepare == hook_prepares) return NULL;
    for (size_t i = 0; i < PUB_HOOKS; i++) if (target == bound_sites[i]) return call_callbacks[i];
    assert(0); return NULL;
}
void *hook_prepare_relative_call(void *target, void *detour, size_t stolen, size_t offset)
{ assert(target == bound_sites[PUB_READY] && stolen == 15 && offset == 7); return hook_prepare(target, detour, stolen); }
sh_patch_status hook_commit(void *trampoline)
{ assert(trampoline); hook_commits++; return hook_commits == fail_commit ? B2_PATCH_FAIL_PROTECT : B2_PATCH_OK; }
int hook_unpatch(void *trampoline)
{ assert(trampoline); hook_removes++; return hook_removes != fail_remove; }
static void emit_relative(unsigned char *site, const char *opcode, size_t length, void *target)
{
    intptr_t distance = (unsigned char *)target - site - length - 4;
    int32_t displacement = (int32_t)distance; assert(distance == displacement);
    memcpy(site, opcode, length); memcpy(site + length, &displacement, 4);
}
static void reset(void)
{
    assert(!retained.context && !g_pub_retired && allocated == released);
    memset(manager, 0, sizeof(manager)); memset(cache, 0, sizeof(cache)); memset(calls, 0, sizeof(calls));
    *(void **)cache = cache_table; cache_table[0x190 / 8] = set_cache; system_table[7] = get_cache;
    session_table[0x338 / 8] = get_lobby; lobby_table[0x2b0 / 8] = get_parameters;
    progress_table[0x30 / 8] = hide_progress; *(void **)(manager + 0xa38) = &progress_object;
    *(void **)(shell + 0x18) = manager; thread_id = GetCurrentThreadId();
    *(int *)(manager + 8) = *(int *)(manager + 12) = 63; *(int *)(manager + 0x910) = 1;
    *(int *)metadata = *(int *)(host_parameters + 0x550) = 42;
    *(int *)(metadata + 0x30) = *(int *)(host_parameters + 0x600) = 7;
    memcpy(history_object + 8, metadata, sizeof(metadata)); *(void **)(history_record + 0x18) = history_object;
    set_cache(cache, metadata, (void *)1);
    g_pub_system = get_system; g_pub_cache_set = set_cache; g_pub_shell_slot = &shell_slot;
    g_pub_session_slot = &session_slot; g_pub_history_slot = &history_slot; g_pub_history_count = &history_count; g_pub_thread = &thread_id;
    g_pub_read_return = bound_sites[4] + 0x12a; g_pub_profile_return = bound_sites[8] + 0x5d;
    call_callbacks[0] = original_offline; call_callbacks[1] = original_lobby; call_callbacks[2] = original_direct;
    call_callbacks[3] = original_ready; call_callbacks[4] = original_read; call_callbacks[5] = original_complete;
    memcpy(g_pub_original, call_callbacks, sizeof(g_pub_original));
    boundary = g_pub_ready = 1; g_pub_delegating = inspection = read_action = throws_read = 0;
    mode = errors = prepared = decoded = hidden = 0;
}
static void grouped_installation(void)
{
    const uint8_t *base = (const uint8_t *)GetModuleHandleA(NULL);
    const char *names[] = {"PublishedMapOfflineLaunch", "PublishedMapLobbyLaunch", "PublishedMapDirectLaunch",
        "PublishedMapCacheReady", "PublishedMapRead", "PublishedMapComplete", "PublishedMapCacheSet",
        "DeserializeFromJson", "SnapMapReadSizeLimit"};
    sig_result results[10] = {0};
    for (int i = 0; i < 9; i++) {
        results[i].name = names[i]; results[i].addr = (uintptr_t)bound_sites[i]; results[i].status = SIG_OK;
        results[i].rva = (uint32_t)(bound_sites[i] - base);
    }
    emit_relative(bound_sites[0] + 0x34, "\xe8", 1, get_system);
    emit_relative(bound_sites[1] + 0x87, "\xe8", 1, get_system);
    emit_relative(bound_sites[3] + 0x9f, "\xe8", 1, get_system);
    emit_relative(bound_sites[4] + 0x125, "\xe8", 1, bound_sites[7]);
    emit_relative(bound_sites[8] + 0x58, "\xe8", 1, bound_sites[7]);
    emit_relative(bound_sites[1] + 0x58, "\x8b\x0d", 2, &history_count);
    emit_relative(bound_sites[1] + 0x77, "\x4c\x03\x35", 3, &history_slot);
    emit_relative(bound_sites[3] + 0x4c, "\x48\x8b\x0d", 3, &session_slot);
    memcpy(bound_sites[6] + 0x54, "\x48\x8d\x8e\x08\x44\x02\x00", 7);
    memcpy(bound_sites[6] + 0x67, "\x48\x8d\x8e\x38\x44\x02\x00", 7);
    g_pub_ready = 0;
    assert(sh_map_published_bind(results, 9, base));
    results[9] = results[0]; assert(!sh_map_published_bind(results, 10, base));
    bound_sites[1][0x58] = 0; assert(!sh_map_published_bind(results, 9, base)); bound_sites[1][0x58] = 0x8b;
    bound_sites[4][0x125] = 0; assert(!sh_map_published_bind(results, 9, base)); bound_sites[4][0x125] = 0xe8;
    bound_sites[8][0x58] = 0; assert(!sh_map_published_bind(results, 9, base)); bound_sites[8][0x58] = 0xe8;
    for (int failure = 1; failure <= 6; failure++) {
        memset(g_pub_original, 0, sizeof(g_pub_original)); hook_prepares = hook_commits = hook_removes = 0;
        fail_prepare = failure; fail_commit = fail_remove = 0;
        assert(!sh_map_published_install(results, 9, base) && !g_pub_ready && !hook_commits);
        assert(hook_removes == failure - 1);
        fail_prepare = 0; fail_commit = failure; fail_remove = 1;
        hook_prepares = hook_commits = hook_removes = 0;
        assert(!sh_map_published_install(results, 9, base) && !g_pub_ready);
        /* A surviving detour keeps its original. Inspection is not enabled by
         * partial publication, so calling it remains a normal native read. */
        assert(g_pub_original[PUB_COMPLETE]); original_complete((void *)9); assert(!inspection);
        fail_remove = fail_commit = 0;
        assert(sh_map_published_install(results, 9, base) && g_pub_ready);
        g_pub_ready = 0;
        for (int i = 0; i < PUB_HOOKS; i++) g_pub_original[i] = NULL;
    }
}
static DWORD WINAPI stale_thread(void *unused)
{
    (void)unused; pub_ready(manager, (void *)9, metadata, (void *)1);
    assert(pub_read((void *)1, "id", "revision", (void *)2, (void *)3) == (void *)10 && !read_scope && !inspection);
    assert(!hidden && !prepared && !errors); return 0;
}
int main(void)
{
    reset(); grouped_installation(); reset();
    assert(pub_read((void *)1, "id", "revision", (void *)2, (void *)3) == (void *)10);
    assert(!inspection && !prepared && !retained.context);
    throws_read = 1;
    __try { pub_complete((void *)9); assert(0); }
    __except(EXCEPTION_EXECUTE_HANDLER) { assert(!inspection && !read_scope); }
    throws_read = 0;
    for (int kind = 0; kind < 4; kind++) {
        reset();
        if (kind == PUB_OFFLINE) { read_action = 1; pub_read((void *)1, "id", "revision", (void *)2, (void *)3); }
        if (kind == PUB_LOBBY) assert(pub_lobby(manager, 1));
        if (kind == PUB_DIRECT) { read_action = 2; pub_complete((void *)9); }
        if (kind == PUB_READY) pub_ready(manager, (void *)9, metadata, (void *)1);
        assert(prepared == 1 && !calls[kind] && retained.context && !inspection);
        assert(finish(1) && calls[kind] == 1 && !g_pub_delegating && allocated == released);
    }
    for (int cancellation = 0; cancellation < 5; cancellation++) {
        reset(); pub_offline(manager, metadata, (void *)1);
        if (cancellation == 1) *(int *)(cache + 0x24408) = 43;
        if (cancellation == 2) *(int *)(manager + 8) = 3;
        if (cancellation == 3) *(void **)(shell + 0x18) = NULL;
        if (cancellation == 4) mode = 3;
        assert(!finish(cancellation != 0) && !calls[0] && allocated == released);
    }
    reset(); pub_ready(manager, (void *)9, metadata, (void *)1);
    *(int *)(host_parameters + 0x600) = 8;
    assert(!finish(1) && !calls[3] && hidden == 1 && allocated == released);
    reset(); pub_lobby(manager, 1); *(void **)(history_record + 0x18) = (void *)1;
    assert(!finish(1) && !calls[1] && allocated == released);
    reset(); pub_offline(manager, metadata, (void *)1); pub_direct(manager, metadata, (void *)1);
    assert(finish(1) && !calls[0] && calls[2] == 1 && allocated == released);
    for (int failure = 0; failure <= 2; failure++) {
        reset(); if (!failure) boundary = 0; else mode = failure;
        pub_offline(manager, metadata, (void *)1);
        assert(!retained.context && !calls[0] && errors == 1 && allocated == released);
    }
    reset(); pub_offline(manager, metadata, (void *)1); mode = 4;
    assert(!finish(0) && g_pub_retired && allocated == released + 1);
    sh_map_published_poll(); assert(g_pub_retired);
    mode = 0; sh_map_published_poll(); assert(!g_pub_retired && allocated == released);
    reset(); pub_ready((void *)1, (void *)9, metadata, (void *)1);
    assert(!hidden && !prepared && !errors);
    {
        HANDLE thread = CreateThread(NULL, 0, stale_thread, NULL, 0, NULL);
        assert(thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0); CloseHandle(thread);
    }
    reset(); pub_lobby(manager, 1); mode = 5;
    assert(!finish(1) && calls[1] == 1 && !g_pub_delegating && allocated == released);
    /* Native WaitOnSession pumps callbacks. Only the same retained map may
     * reuse its active entry scope; an unrelated nested map needs preparation. */
    for (int nested = 6; nested <= 7; nested++) {
        reset(); pub_offline(manager, metadata, (void *)1); mode = nested;
        assert(finish(1) && calls[0] == 1 && !g_pub_entering && !g_pub_delegating);
        assert(calls[2] == (nested == 6) && errors == (nested == 7) && allocated == released);
    }
    reset(); g_pub_ready = 0; pub_offline(manager, metadata, (void *)1);
    assert(calls[0] == 1 && !prepared);
    puts("map_published_test: readers, four retained launch routes, cancellation, cleanup and hook-group failure passed");
    return 0;
}
