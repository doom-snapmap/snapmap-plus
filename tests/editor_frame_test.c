/* Exercise deferred saves across editor lifecycle changes without game code. */
#include "heap_out.h"
#include "../src/backend/editor_frame.c"
#include "../src/common/wide_path.h"
#include "saved_map_fixture.h"

static unsigned long generation;
static int writes, choices, failed, ticks, loads, done_rc = -1, done_faulted = -1, tells, fault_load;
static char current[4096] = "C:\\maps\\current.json", written[4096];
static unsigned char editor[0x24000];
static int map_a, map_b;
#define CHECK(x) do { if (!(x)) { ++failed; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

static const char *edit_id;
static char loaded_id[64];
int sh_rawmap_edit_local_id(char *out, size_t cap)
{
    return edit_id && strcpy_s(out, cap, edit_id) == 0;
}
void backend_log(const char *message) { (void)message; }
void sh_world_area_cell_editor_tick(void) {}
void sh_fly_to_tick(void *editor) { (void)editor; ++ticks; }
void sh_module_prefab_tick(void *editor) { (void)editor; }
void sh_timeline_tile_tick(void *editor) { (void)editor; }
void sh_budget_table_tick(void *editor) { (void)editor; }
void sh_editor_zoom_tick(void) {}
static int publish_ticks;
void sh_modpublish_editor_tick(void *editor) { (void)editor; ++publish_ticks; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; return NULL; }
int hook_commit(void *trampoline) { (void)trampoline; return B2_PATCH_OK; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 1; }
int sh_rawmap_live_serialize_ready(void) { return 1; }
unsigned long sh_rawmap_load_generation(void) { return generation; }
int sh_rawmap_load_is_safe(void) { return 1; }
int sh_rawmap_source_ok(char **msg) { if (msg) *msg = NULL; return 1; }
int sh_rawmap_prepare_reload(char **why) { if (why) *why = NULL; return 1; }
void sh_rawmap_reload_done(int rc, int faulted) { done_rc = rc; done_faulted = faulted; }
void sh_rawmap_tell(const char *text) { (void)text; ++tells; }
char *sh_rawmap_save_path(void) { return _strdup(current); }
int sh_rawmap_dest_writable_now(const char *path, char **msg)
{ (void)path; (void)msg; return 1; }
int sh_rawmap_set_save_target(const char *path)
{ ++choices; strcpy_s(current, sizeof current, path); return 1; }
int sh_rawmap_write_from_live(void *map, const char *path, char **msg, unsigned long long *bytes)
{ (void)map; (void)bytes; if (msg) *msg = NULL; ++writes; strcpy_s(written, sizeof written, path); return 1; }
static int refusals, disk_reads, load_rc;
static char placeholder[16];
void shield_note_refused_load(void) { ++refusals; }
static void original_frame(void *ed, void *arg) { (void)ed; (void)arg; }
/* The engine's ReadLocalSavedMapText: reading a slot off disk. */
static int disk_read(const char *id, void *text, void *a3, void *a4)
{ (void)id; (void)text; (void)a3; (void)a4; ++disk_reads; return 4; }
static void assign_cstr(void *idstr, const char *text) { (void)idstr; strcpy_s(placeholder, sizeof placeholder, text); }
/* A read that succeeds, and the read filter that sees it. */
static int good_read(const char *id, void *text, void *a3, void *a4) { (void)id; (void)text; (void)a3; (void)a4; return 0; }
static int filtered;
void sh_rawmap_local_read(const void *caller, const char *slot, const void *text, int status)
{ (void)caller; (void)slot; (void)text; (void)status; }
static const void *filter_caller;
static void t_filter(const char *id, void *text, const void *caller) { (void)id; (void)text; filtered++; filter_caller = caller; }
/* LoadMap reads the slot it is named, as the engine does. */
static int load_map(void *ed, void *id)
{
    const char *name = ((const ef_idstr *)id)->data;
    int got;
    (void)ed;
    CHECK(!sh_editor_frame_map_context());
    ++loads;
    strcpy_s(loaded_id, sizeof loaded_id, name);
    if (fault_load) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    got = ef_read_text_detour(name, NULL, NULL, NULL);
    if (got != 0) return got;
    return load_rc;
}
static void queue_reload(void *map)
{
    memset(&g_reload_request, 0, sizeof g_reload_request);
    g_reload_request.map = map;
    g_reload_request.generation = generation;
    strcpy_s(g_reload_request.id, sizeof g_reload_request.id, "0123456789abcdef0123");
    g_reload_pending = 1;
}

static void reset(void)
{
    memset(editor, 0, sizeof editor);
    *(void **)(editor + ED_MAP_PTR_OFF) = &map_a;
    *(int *)(editor + ED_STATE_OFF) = 1;
    g_editor_obj = (uintptr_t)editor;
    g_frame_orig = original_frame;
    g_load_map = load_map;
    g_read_text_orig = disk_read;
    g_assign_cstr = assign_cstr;
    g_borrowed_id = NULL;
    refusals = disk_reads = load_rc = 0;
    placeholder[0] = '\0';
    g_save_pending = g_reload_pending = g_reload_faulted = 0;
    generation = 0;
    writes = choices = ticks = loads = tells = fault_load = 0;
    done_rc = done_faulted = -1;
    strcpy_s(current, sizeof current, "C:\\maps\\current.json");
}

/* Build complete and deleted slots entirely under the runner's private temporary directory. */
static wchar_t *test_dir(const wchar_t *parent, const wchar_t *name)
{
    wchar_t *path = sh_wtextf(L"%s\\%s", parent, name);
    if (!path || !CreateDirectoryW(path, NULL)) { printf("test directory failed: %lu\n", GetLastError()); exit(1); }
    return path;
}

static void test_slot(const wchar_t *profile, const wchar_t *name, int complete, DWORD time)
{
    wchar_t *slot = test_dir(profile, name);
    const wchar_t *files[] = { L"game.details", L"map.decl" };
    for (int i = 0; i < (complete ? 2 : 1); ++i) {
        wchar_t *path = sh_wtextf(L"%s\\%s", slot, files[i]);
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            /* A genuine map.decl contains a four-byte checksum and zlib-compressed JSON. */
            static const unsigned char map[] = { 0, 0, 0, 0, 120, 156, 171, 86, 202, 79, 202, 74, 77, 46, 41, 86, 178, 138, 142, 173, 5, 0, 38, 109, 5, 25 };
            const char *account = wcsrchr(profile, L'\\') && !wcscmp(wcsrchr(profile, L'\\') + 1, L"111")
                                ? "itemId=first" : "itemId=other";
            DWORD wrote = 0;
            CHECK(WriteFile(file, i ? (const void *)map : (const void *)account,
                            i ? sizeof map : (DWORD)strlen(account), &wrote, NULL));
            CloseHandle(file);
        }
        free(path);
    }
    HANDLE dir = CreateFileW(slot, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    FILETIME stamp = { time, 30000000 };
    CHECK(dir != INVALID_HANDLE_VALUE && SetFileTime(dir, NULL, NULL, &stamp));
    if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
    free(slot);
}

static void count_slot(void *context, const char *local_id, const char *item, const char *json, size_t len)
{
    int *counts = context;
    if (!strcmp(local_id, "5C70EFF070704720BCA6") && !strcmp(item, "first")) counts[0]++;
    if (!strcmp(local_id, "2342E40A735B43FB99A7") && !strcmp(item, "other")) counts[1]++;
    CHECK(len == strlen(json) && strstr(json, "objects"));
}

static void test_reload_slots(void)
{
    static wchar_t temp[SH_WPATH_MAX];
    static char redirected[SH_U8PATH_MAX], windows_saves[SH_U8PATH_MAX];
    const char *active = "5C70EFF070704720BCA6", *other = "2342E40A735B43FB99A7";
    char id[21], msg[HO_TEXT];
    DWORD n = GetTempPathW(SH_WPATH_MAX, temp);
    if (!n || n >= SH_WPATH_MAX || !sh_wpath_long(temp, SH_WPATH_MAX)) exit(1);
    if (temp[wcslen(temp) - 1] == L'\\') temp[wcslen(temp) - 1] = 0;
    wchar_t *name = sh_wtextf(L"reload-%lu-%llu-\x2603", GetCurrentProcessId(), GetTickCount64());
    wchar_t *root = test_dir(temp, name), *base = test_dir(root, L"base");
    wchar_t *profiles = test_dir(base, L"savegame.user"), *first = test_dir(profiles, L"111");
    wchar_t *windows = test_dir(root, L"Saved Games"), *vendor = test_dir(windows, L"id Software");
    wchar_t *doom = test_dir(vendor, L"DOOM"), *stock_base = test_dir(doom, L"base");
    wchar_t *default_root = test_dir(stock_base, L"savegame.user"), *win = test_dir(default_root, L"222");
    free(name);
    CHECK(sh_wpath_utf8(root, redirected, sizeof redirected) && sh_wpath_utf8(default_root, windows_saves, sizeof windows_saves));
    test_slot(first, L"SNAPMAPS5C70EFF070704720BCA6", 1, 100);
    test_slot(first, L"SNAPMAPS11111111111111111111", 1, 50);
    test_slot(first, L"SNAPMAPS22222222222222222222", 0, 200);
    test_slot(first, L"SNAPMAPSZZZZZZZZZZZZZZZZZZZZ", 1, 300);
    test_slot(win, L"SNAPMAPS2342E40A735B43FB99A7", 1, 400);
    sh_saved_maps_set_root(windows_saves);
    bind_save_session("", 0);
    CHECK(sh_saved_map_fallback(id, sizeof id) && !strcmp(id, other));
    bind_save_session(NULL, 0);
    CHECK(!sh_saved_map_fallback(id, sizeof id) && sh_saved_maps_each(NULL, NULL, NULL) == -1);
    char *missing = sh_textf("%s/missing", redirected);
    bind_save_session(missing, 0);
    CHECK(!sh_saved_map_fallback(id, sizeof id) && sh_saved_maps_each(NULL, NULL, NULL) == -1);
    free(missing);

    reset();
    bind_save_session(redirected, 0);
    edit_id = NULL;
    int single[2] = {0};
    CHECK(sh_saved_maps_each(count_slot, single, NULL) > 0 && single[0] == 1 && !single[1]);
    CHECK(HO(msg, sh_editor_frame_request_reload(HO_P(0))));
    sh_editor_frame_detour(editor, NULL);
    CHECK(loads == 1 && !strcmp(loaded_id, active));
    wchar_t *second = test_dir(profiles, L"222");
    test_slot(second, L"SNAPMAPS2342E40A735B43FB99A7", 1, 500);
    bind_save_session(redirected, 111);
    int counts[2] = {0};
    CHECK(sh_saved_maps_each(count_slot, counts, NULL) >= 2 && counts[0] == 1 && counts[1] == 1);
    char *details = sh_saved_map_details(active);
    CHECK(details && !strcmp(details, "itemId=first"));
    free(details);
    CHECK(sh_saved_map_details(other) == NULL);
    CHECK(sh_saved_map_fallback(id, sizeof id) && !strcmp(id, active));
    bind_save_session(redirected, 222);
    CHECK(sh_saved_map_fallback(id, sizeof id) && !strcmp(id, other));
    bind_save_session(redirected, 333);
    CHECK(!sh_saved_map_fallback(id, sizeof id));
    bind_save_session(redirected, 0);
    CHECK(!sh_saved_map_fallback(id, sizeof id));

    reset();
    edit_id = active;
    CHECK(HO(msg, sh_editor_frame_request_reload(HO_P(0))));
    sh_editor_frame_detour(editor, NULL);
    CHECK(loads == 1 && done_rc == 0 && !strcmp(loaded_id, active) && !disk_reads);
    edit_id = "123456789012345678901234";   /* A cloud item is not a local slot. */
    CHECK(!HO(msg, sh_editor_frame_request_reload(HO_P(0))) && !g_reload_pending);
    edit_id = NULL;
    bind_save_session(NULL, 0);
    sh_saved_maps_set_root(NULL);
    free(second); free(win); free(default_root); free(stock_base); free(doom); free(vendor); free(windows);
    free(first); free(profiles); free(base); free(root);
}

int main(void)
{
    char msg[HO_TEXT];
    test_reload_slots();
    reset();
    CHECK(HO(msg, sh_editor_frame_request_rawmap_save_to("C:\\maps\\chosen.json", HO_P(0))));
    CHECK(!choices && !writes);
    CHECK(!HO(msg, sh_editor_frame_request_rawmap_save_to("C:\\maps\\other.json", HO_P(0))));
    sh_editor_frame_detour(editor, NULL);
    CHECK(writes == 1 && choices == 1 && !strcmp(written, "C:\\maps\\chosen.json"));
    sh_editor_frame_detour(editor, NULL);
    CHECK(writes == 1);

    reset();
    CHECK(HO(msg, sh_editor_frame_request_rawmap_save_to(NULL, HO_P(0))));
    strcpy_s(current, sizeof current, "C:\\maps\\later-setting.json");
    sh_editor_frame_detour(editor, NULL);
    CHECK(writes == 1 && !choices && !strcmp(written, "C:\\maps\\current.json"));

    reset();
    CHECK(HO(msg, sh_editor_frame_request_rawmap_save_to("C:\\maps\\chosen.json", HO_P(0))));
    *(int *)(editor + ED_STATE_OFF) = 0;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!writes && !choices && !g_save_pending);

    reset();
    CHECK(HO(msg, sh_editor_frame_request_rawmap_save_to(NULL, HO_P(0))));
    *(int *)(editor + ED_SUBSTATE_OFF) = 5;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!writes);

    reset();
    CHECK(HO(msg, sh_editor_frame_request_rawmap_save_to(NULL, HO_P(0))));
    *(void **)(editor + ED_MAP_PTR_OFF) = &map_b;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!writes);

    reset();
    CHECK(HO(msg, sh_editor_frame_request_rawmap_save_to(NULL, HO_P(0))));
    ++generation; /* The next map may reuse exactly the same allocation. */
    sh_editor_frame_detour(editor, NULL);
    CHECK(!writes);
    CHECK(tells == 1);   /* the cancelled save is told */

    /* A reload opens through the map it names and never reads the saved map whose id it borrows (that read gets
     * a placeholder). */
    reset();
    queue_reload(&map_a);
    sh_editor_frame_detour(editor, NULL);
    CHECK(loads == 1 && done_rc == 0 && done_faulted == 0 && !g_reload_pending);
    CHECK(!disk_reads && !strcmp(placeholder, "{}") && !g_borrowed_id && !refusals);
    /* Outside the reload, and for any other id, the read is the engine's own. */
    CHECK(ef_read_text_detour("0123456789abcdef0123", NULL, NULL, NULL) == 4 && disk_reads == 1);
    g_borrowed_id = "0123456789abcdef0123";
    g_borrow_thread = GetCurrentThreadId();
    CHECK(ef_read_text_detour("FFFFFFFFFFFFFFFFFFFF", NULL, NULL, NULL) == 4 && disk_reads == 2);
    g_borrowed_id = NULL;

    /* The read filter (the published copy, modpublish.c) sees only a read that succeeded, with its caller, and never
     * the borrowed id's placeholder. */
    reset();
    sh_editor_frame_set_read_filter(t_filter);
    CHECK(sh_editor_frame_read_filter_live());
    CHECK(ef_read_text_detour("0123456789abcdef0123", NULL, NULL, NULL) == 4 && filtered == 0);   /* failed: none */
    g_read_text_orig = good_read;
    {
        char text[8] = "x";
        CHECK(ef_read_text_detour("0123456789abcdef0123", text, NULL, NULL) == 0 && filtered == 1 && filter_caller);
        g_borrowed_id = "0123456789abcdef0123";
        g_borrow_thread = GetCurrentThreadId();
        CHECK(ef_read_text_detour("0123456789abcdef0123", text, NULL, NULL) == 0 && filtered == 1);   /* placeholder */
        g_borrowed_id = NULL;
    }
    sh_editor_frame_set_read_filter(NULL);
    CHECK(!sh_editor_frame_read_filter_live());

    /* A reload that fails anyway marks the engine's notices about the borrowed map untrue. */
    reset();
    load_rc = 4;
    queue_reload(&map_a);
    sh_editor_frame_detour(editor, NULL);
    CHECK(done_rc == 4 && refusals == 1 && !disk_reads);

    /* Without the read detour a rawmap is never opened. */
    reset();
    g_read_text_orig = NULL;
    CHECK(!HO(msg, sh_editor_frame_can_reload(HO_P(0))) && strstr(msg, "unavailable"));

    /* A reload request does not outlive its map: another map, or the same
     * allocation reused by a newer load, cancels it and says so. */
    reset();
    queue_reload(&map_a);
    *(void **)(editor + ED_MAP_PTR_OFF) = &map_b;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!loads && done_rc == -1 && tells == 1 && !g_reload_pending);
    reset();
    queue_reload(&map_a);
    ++generation;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!loads && tells == 1);

    /* A click names its map across the queue, a same-allocation parse, and a close/reopen. */
    reset();
    int first = sh_editor_frame_map_context();
    CHECK(first && sh_editor_frame_map_context() == first);
    queue_reload(&map_a);
    CHECK(!sh_editor_frame_map_context());
    ++generation;
    sh_editor_frame_detour(editor, NULL);
    int next = sh_editor_frame_map_context();
    CHECK(next && next != first);
    *(int *)(editor + ED_STATE_OFF) = 0;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!sh_editor_frame_map_context());
    *(int *)(editor + ED_STATE_OFF) = 1;
    CHECK(sh_editor_frame_map_context() && sh_editor_frame_map_context() != next);

    /* Mid-transition the request waits for a live frame. */
    reset();
    queue_reload(&map_a);
    *(int *)(editor + ED_SUBSTATE_OFF) = 5;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!loads && g_reload_pending);
    *(int *)(editor + ED_SUBSTATE_OFF) = 0;
    sh_editor_frame_detour(editor, NULL);
    CHECK(loads == 1);

    /* A LoadMap fault turns off the reload only; the per-frame features and the
     * rawmap save keep running. */
    reset();
    fault_load = 1;
    queue_reload(&map_a);
    sh_editor_frame_detour(editor, NULL);
    CHECK(loads == 1 && done_faulted == 1 && g_reload_faulted && refusals == 1 && !g_borrowed_id);
    ticks = 0;
    publish_ticks = 0;
    sh_editor_frame_detour(editor, NULL);
    CHECK(ticks == 1);
    CHECK(publish_ticks == 1);   /* a publish held at FINISH resumes in the editor's frame */
    CHECK(!HO(msg, sh_editor_frame_can_reload(HO_P(0))) && strstr(msg, "restart DOOM"));
    CHECK(HO(msg, sh_editor_frame_request_rawmap_save_to(NULL, HO_P(0))));
    sh_editor_frame_detour(editor, NULL);
    CHECK(writes == 1);

    /* The editor at the menu: the map pointer survives, the state does not. */
    reset();
    *(int *)(editor + ED_STATE_OFF) = 0;
    CHECK(!HO(msg, sh_editor_frame_can_reload(HO_P(0))) && strstr(msg, "Open a map"));
    CHECK(!HO(msg, sh_editor_frame_request_reload(HO_P(0))) && strstr(msg, "Open a map"));

    /* A destination of any length is queued and written whole, and the sentence names it whole. */
    {
        static char deep[3200];
        size_t n = 0;
        reset();
        n += (size_t)sprintf_s(deep, sizeof deep, "D:\\maps");
        while (n < 3000) n += (size_t)sprintf_s(deep + n, sizeof deep - n, "\\%s", "folder-of-maps");
        sprintf_s(deep + n, sizeof deep - n, "\\deep.json");
        CHECK(HO(msg, sh_editor_frame_request_rawmap_save_to(deep, HO_P(0))));
        CHECK(strstr(msg, deep) != NULL);
        sh_editor_frame_detour(editor, NULL);
        CHECK(writes == 1 && choices == 1 && !strcmp(written, deep) && !strcmp(current, deep));
    }
    printf("editor_frame_test: %d failures\n", failed);
    return failed ? 1 : 0;
}
