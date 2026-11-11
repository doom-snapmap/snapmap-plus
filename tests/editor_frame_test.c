/* Exercise deferred saves across editor lifecycle changes without game code. */
#include "heap_out.h"
#include "../src/backend/editor_frame.c"

static unsigned long generation;
static int writes, choices, failed, ticks, loads, done_rc = -1, done_faulted = -1, tells, fault_load;
static char current[4096] = "C:\\maps\\current.json", written[4096];
static unsigned char editor[0x24000];
static int map_a, map_b;
#define CHECK(x) do { if (!(x)) { ++failed; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

void backend_log(const char *message) { (void)message; }
void sh_world_area_cell_editor_tick(void) {}
void sh_fly_to_tick(void *editor) { (void)editor; ++ticks; }
void sh_module_prefab_tick(void *editor) { (void)editor; }
void sh_timeline_tile_tick(void *editor) { (void)editor; }
void sh_budget_table_tick(void *editor) { (void)editor; }
void sh_editor_zoom_tick(void) {}
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
/* LoadMap reads the slot it is named, as the engine does. */
static int load_map(void *ed, void *id)
{
    const char *name = ((const ef_idstr *)id)->data;
    int got;
    (void)ed;
    ++loads;
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

int main(void)
{
    char msg[HO_TEXT];
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

    /* A reload opens through the map it was asked for, and never reads the
     * saved map whose id it borrows: that read gets a placeholder. */
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
    sh_editor_frame_detour(editor, NULL);
    CHECK(ticks == 1);
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
