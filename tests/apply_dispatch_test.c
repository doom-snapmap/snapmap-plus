/* Exercise production queue ownership and dispatch against a controlled engine. */
#define SH_APPLY_ENGINE_TESTING
#define AE_MARSHAL_WAIT_MS 30
#define AE_MARSHAL_GRACE_MS 30
#include <assert.h>
#include "../src/backend/apply_engine.c"

static DWORD engine_id;
static HANDLE start_event, entered_event, release_event;
static volatile LONG executions;
static int append_fails, delay_execution;
static HANDLE registration_entered, registration_release;
static int registration_fails;
static int maintenance_allowed, maintenance_step;
static int nav_test_allowed, nav_ready, nav_prepare_calls, nav_rooms, nav_queued;

static void snapshot_dimensions_follow_array_order(void)
{
    static sh_nav_map out;
    ae_nav_visit_ctx visit;
    unsigned char snapshot[0x30] = {0}, entities[2 * AE_SNAPSHOT_ENTITY_SIZE] = {0};
    unsigned char str[IDSTR_SIZE] = {0};
    const char *decls[] = {
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=73;y=157;z=91;}}}",
        "edit={clipModelInfo={type=\"CLIPMODEL_BOX\";size={x=83;y=167;z=101;}}}"
    };
    static const char json[] =
        "{\"instances\":[{\"moduleName\":\"maps/modules/test/room.decl\"}],"
        "\"entities\":["
        "{\"uniqueId\":11,\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{\"flags\":{\"noFlood\":true},\"blockDemons\":true}}}},"
        "{\"uniqueId\":11,\"entityDef\":{\"inherit\":\"snapmaps/volume/blocking\",\"state\":{\"edit\":{\"flags\":{\"noFlood\":true},\"blockDemons\":true}}}}],"
        "\"instanceEntities\":{\"keyValues\":[0,1,1],\"values\":[11]}}";
    int i;
    *(void **)(snapshot + AE_SNAPSHOT_ENTITIES_OFF) = entities;
    *(uint32_t *)(snapshot + AE_SNAPSHOT_COUNT_OFF) = 2;
    *(const char **)(str + IDSTR_DATA_OFF) = json;
    *(uint32_t *)(str + IDSTR_LEN_OFF) = sizeof json - 1;
    for(i=0;i<2;i++) {
        unsigned char *resolved = entities + i * AE_SNAPSHOT_ENTITY_SIZE + AE_DECL_RESOLVED_OFF;
        *(const char **)(resolved + IDSTR_DATA_OFF) = decls[i];
        *(uint32_t *)(resolved + IDSTR_LEN_OFF) = (uint32_t)strlen(decls[i]);
    }
    memset(&visit, 0, sizeof visit); visit.out = &out;
    assert(ae_nav_snapshot_visit(snapshot,str,&visit));
    assert(!out.invalid_geometry && out.ids_unusable && out.region_count == 2);
    assert(out.regions[0].depth == 91 && out.regions[1].depth == 101);
    assert(out.regions[0].c[0][0] == -36.5f && out.regions[1].c[0][0] == -41.5f);
    assert(!memcmp(json,*(const char **)(str+IDSTR_DATA_OFF),sizeof json));
    *(uint32_t *)(snapshot + AE_SNAPSHOT_COUNT_OFF) = 1;
    /* The entity cannot be measured, which refuses the read without calling the
     * geometry itself unusable. */
    assert(ae_nav_snapshot_visit(snapshot,str,&visit) && out.unmeasured);
    assert(visit.box.inherited == 0);
    /* During a conversion there is no later read: the unresolved box takes
     * the 16 x 128 x 64 its entityDef gives it, the resolved one keeps its own. */
    visit.box.converting = 1;
    assert(ae_nav_snapshot_visit(snapshot,str,&visit) && !out.unmeasured);
    assert(out.region_count == 2 && visit.box.inherited == 1);
    assert(out.regions[0].depth == 91 && out.regions[1].depth == 64);
    assert(out.regions[1].c[0][0] == -8.0f);
}
void backend_log(const char *text) { (void)text; }
sh_iface *sh_ui_get_iface(void) { return NULL; }

static int execute(int kind, int id, const char *text)
{
    (void)kind; (void)id;
    assert(GetCurrentThreadId() == engine_id);
    assert(strcmp(text, "original") == 0);
    InterlockedIncrement(&executions);
    if (delay_execution) {
        SetEvent(entered_event);
        assert(WaitForSingleObject(release_event, 3000) == WAIT_OBJECT_0);
    }
    return 1;
}

static void append_command(void *sys, const char *text)
{
    (void)sys; (void)text;
    if (append_fails) RaiseException(0xe0000001, 0, 0, NULL);
    if (start_event) SetEvent(start_event);
}

static DWORD WINAPI drain(LPVOID unused)
{
    (void)unused;
    assert(WaitForSingleObject(start_event, 3000) == WAIT_OBJECT_0);
    ae_clone_bss_apply_cmd();
    return 0;
}

static void register_command(void *sys, const char *name, void *cb, void *arg,
                             const char *help, unsigned flags)
{
    (void)sys; (void)name; (void)cb; (void)arg; (void)help; (void)flags;
    if (registration_fails) RaiseException(0xe0000001, 0, 0, NULL);
    if (registration_entered) {
        SetEvent(registration_entered);
        assert(WaitForSingleObject(registration_release, 3000) == WAIT_OBJECT_0);
    }
}

static DWORD WINAPI register_first(LPVOID unused)
{
    (void)unused;
    return (DWORD)ae_ensure_command();
}

static DWORD WINAPI poll_from_worker(LPVOID unused)
{
    (void)unused;
    assert(GetCurrentThreadId() != engine_id);
    sh_apply_prefab_poll_play();
    return 0;
}

static void test_maintenance_thread(void)
{
    int load_state = LOAD_STATE_RUNNING;
    HANDLE worker;
    assert(!g_doom_base && !g_cmdsys);
    g_load_state_at = (const uint8_t *)&load_state;
    g_main_thread_at = NULL;
    sh_apply_prefab_poll_play();
    g_main_thread_at = (const uint8_t *)(uintptr_t)1;
    sh_apply_prefab_poll_play();
    g_main_thread_at = (const uint8_t *)&engine_id;
    engine_id = 0;
    sh_apply_prefab_poll_play();
    engine_id = GetCurrentThreadId() + 1;
    sh_apply_prefab_poll_play();
    assert(maintenance_step == 0);

    engine_id = GetCurrentThreadId();
    worker = CreateThread(NULL, 0, poll_from_worker, NULL, 0, NULL);
    assert(worker && WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0);
    CloseHandle(worker);
    assert(maintenance_step == 0);

    /* On the engine thread the poll runs only the navigation refresh; the
     * retired package maintenance (decl rearm, consent, pending maps) is gone.
     * Null host/command pointers prevent native writes. */
    maintenance_allowed = 1;
    for (int i = 0; i < 2; i++) {
        maintenance_step = 0;
        sh_apply_prefab_poll_play();
        assert(maintenance_step == 0);
        assert(!g_nav_refresh_queued && !g_nav_refresh_registered);
        assert(g_last_load_state == -1);
    }
    maintenance_allowed = 0;
    g_load_state_at = NULL;
}

static void nav_append(void *sys, const char *text)
{ (void)sys; assert(!strcmp(text,"sh_nav_refresh_internal\n")); nav_queued++; }
static void nav_register(void *sys, const char *name, void *cb, void *arg,
                         const char *help, unsigned flags)
{ (void)sys; (void)cb; (void)arg; (void)help; (void)flags; assert(!strcmp(name,"sh_nav_refresh_internal")); }

static void test_start_menu_update_lifecycle(void)
{
    unsigned char *editor = calloc(1, ED_ENTITY_MODE_OFF + 16), map[0x400] = {0};
    int load_state = LOAD_STATE_RUNNING;
    const uint8_t *previous = g_editor;
    assert(editor);
    nav_test_allowed = 1;
    g_editor = editor; g_load_state_at = (const uint8_t *)&load_state;
    *(void **)(editor + ED_MAP_OBJ_OFF) = map;
    *(int *)(editor + ED_ENTITY_MODE_OFF) = 11;
    *(int *)(editor + ED_MODE_STATE_OFF) = 7; /* stale drag state must not block Start */
    g_nav_map_obj = map; g_nav_have_hash = 1;
    g_cmdsys = editor; g_buffer_cmd = nav_append; g_add_command = nav_register;
    sh_apply_engine_nav_request_update();
    ae_nav_refresh_poll();
    assert(nav_queued == 1 && !nav_prepare_calls && (sh_apply_engine_nav_view_state() & 2));
    /* The command has now read the snapshot; the worker has not finished. */
    g_nav_update_requested = 0; g_nav_update_status = SH_NAV_UPDATE_BUILDING;
    ae_nav_refresh_poll();
    assert(nav_prepare_calls == 1 && sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_BUILDING);
    nav_ready = 1; ae_nav_refresh_poll();
    assert(nav_prepare_calls == 2 && sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_READY);
    assert(!(sh_apply_engine_nav_view_state() & 2));
    /* A room that keeps its shipped navigation is a finished update that
     * names it, not a failed one. */
    g_nav_update_status = SH_NAV_UPDATE_BUILDING; nav_rooms = 1;
    ae_nav_refresh_poll();
    assert(sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_READY);
    assert(!(sh_apply_engine_nav_view_state() & 4));
    /* A map the read refused is the failure. */
    g_nav_update_status = SH_NAV_UPDATE_BUILDING; nav_ready = -1;
    ae_nav_refresh_poll();
    assert(sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_FAILED);
    assert(sh_apply_engine_nav_view_state() & 4);
    nav_ready = 1;
    g_nav_update_status = SH_NAV_UPDATE_BUILDING;
    *(void **)(editor + ED_MAP_OBJ_OFF) = NULL; ae_nav_refresh_poll();
    assert(sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_FAILED);
    free(editor); g_editor = previous; g_load_state_at = NULL;
    g_cmdsys = NULL; g_buffer_cmd = NULL; g_add_command = NULL;
    g_nav_map_obj = NULL; g_nav_have_hash = 0; g_nav_stale = 0;
    g_nav_refresh_queued = 0; g_nav_refresh_registered = 0; g_nav_update_status = 0;
    nav_test_allowed = 0;
}

int main(void)
{
    snapshot_dimensions_follow_array_order();
    unsigned char editor[0x20500] = {0};
    sh_apply_item item = {0, 1, "original"};
    g_editor = editor;
    *(void **)(editor + ED_MAP_OBJ_OFF) = editor;
    test_maintenance_thread();
    test_start_menu_update_lifecycle();
    g_main_thread_at = (const uint8_t *)&engine_id;
    g_apply_test_executor = execute;
    engine_id = 0;
    assert(slot_apply_sync(NULL, &item, 1, "test") == 0);
    engine_id = GetCurrentThreadId() + 1;
    assert(slot_apply_sync(NULL, &item, 1, "test") == 0 && executions == 0);
    engine_id = GetCurrentThreadId();
    assert(slot_apply_sync(NULL, &item, 1, "test") == 1 && executions == 1);

    InitializeCriticalSection(&g_pending_lock); g_pending_lock_init = 1;
    g_sync_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_cmdsys = editor; g_buffer_cmd = append_command; g_add_command = register_command;
    registration_fails = 1;
    assert(slot_schedule_apply(NULL, &item, 1, "test") == 0 && !g_pending_items);
    assert(g_cmd_registered == 0);
    registration_fails = 0;
    registration_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    registration_release = CreateEventA(NULL, TRUE, FALSE, NULL);
    {
        HANDLE worker = CreateThread(NULL, 0, register_first, NULL, 0, NULL);
        DWORD registered = 0;
        assert(worker && WaitForSingleObject(registration_entered, 3000) == WAIT_OBJECT_0);
        assert(slot_schedule_apply(NULL, &item, 1, "test") == 0 && !g_pending_items);
        engine_id = GetCurrentThreadId() + 1;
        assert(slot_apply_sync(NULL, &item, 1, "test") == 0 && g_sync_req.state == AE_SYNC_EMPTY);
        engine_id = GetCurrentThreadId();
        SetEvent(registration_release);
        assert(WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0);
        assert(GetExitCodeThread(worker, &registered) && registered == 1);
        CloseHandle(worker);
    }
    CloseHandle(registration_entered); CloseHandle(registration_release);
    registration_entered = registration_release = NULL;
    {
        char text[] = "original";
        item.text = text;
        assert(slot_schedule_apply(NULL, &item, 1, "test") == 1);
        text[0] = 'X';
        assert(slot_schedule_apply(NULL, &item, 1, "test") == 0);
        ae_clone_bss_apply_cmd();
        assert(executions == 2 && !g_pending_items);
        item.text = "original";
    }
    append_fails = 1;
    assert(slot_schedule_apply(NULL, &item, 1, "test") == 0 && !g_pending_items);
    append_fails = 0;
    {
        char *oversize = (char *)malloc(APPLY_TEXT_CAP + 1);
        sh_apply_item batch[2] = {item, item};
        memset(oversize, 'x', APPLY_TEXT_CAP); oversize[APPLY_TEXT_CAP] = 0;
        batch[1].text = oversize;
        assert(slot_schedule_apply(NULL, batch, 2, "test") == 0 && !g_pending_items);
        assert(slot_apply_sync(NULL, batch, 2, "test") == 0 && executions == 2);
        free(oversize);
    }
    engine_id = GetCurrentThreadId() + 1;
    assert(slot_apply_sync(NULL, &item, 1, "test") == 0);
    assert(g_sync_req.state == AE_SYNC_EMPTY && executions == 2);
    for (int delayed = 0; delayed < 2; delayed++) {
        HANDLE worker;
        delay_execution = delayed;
        start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        entered_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        release_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        worker = CreateThread(NULL, 0, drain, NULL, CREATE_SUSPENDED, &engine_id);
        assert(worker); ResumeThread(worker);
        int result = slot_apply_sync(NULL, &item, 1, "test");
        assert(result == (delayed ? SH_APPLY_IN_PROGRESS : 1));
        if (delayed) {
            assert(WaitForSingleObject(entered_event, 0) == WAIT_OBJECT_0);
            assert(slot_apply_sync(NULL, &item, 1, "test") == 0);
            SetEvent(release_event);
        }
        assert(WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0);
        assert(g_sync_req.state == AE_SYNC_EMPTY);
        CloseHandle(worker); CloseHandle(start_event); CloseHandle(entered_event); CloseHandle(release_event);
        start_event = NULL;
    }
    assert(executions == 4);
    CloseHandle(g_sync_ev); DeleteCriticalSection(&g_pending_lock);
    puts("apply_dispatch_test OK");
    return 0;
}

/* Dependencies outside the dispatch boundary must never run in this test. */
uintptr_t sig_addr_by_name(const sig_result *r, size_t n, const char *name) { (void)r; (void)n; (void)name; assert(0); return 0; }
void *sh_typeinfo_get_declmgr(void) { assert(0); return NULL; }
int sh_iface_class_inherit_ok(int id, const char *c, const char *h) { (void)id; (void)c; (void)h; assert(0); return 0; }
const char *ie_resolve_id_string(int id, char *buf, int cap) { (void)id; (void)buf; (void)cap; assert(0); return NULL; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status) { (void)base; (void)name; (void)status; assert(0); return 0; }
int sh_host_is_supported_build(void) { assert(0); return 0; }
uint32_t sh_host_rva(uint32_t vk, uint32_t gl) { (void)vk; (void)gl; return 0; }
unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *len) { (void)name; (void)len; assert(0); return NULL; }
void *sh_prepare_detour_sig(const sig_result *r, void *detour, size_t stolen)
{ (void)r; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status sh_commit_detour(void *tramp) { (void)tramp; assert(0); return B2_PATCH_OK; }
int sh_uninstall_detour(void *tramp) { (void)tramp; assert(0); return 0; }
int sh_config_get_bool(const char *key, int *value, unsigned *flags)
{ (void)flags; assert(nav_test_allowed && !strcmp(key,"navmesh.enabled")); *value=1; return 1; }
int sh_rawmap_snapshot(void *serializer, void *map, void *out) { (void)serializer; (void)map; (void)out; assert(0); return 0; }
int sh_rawmap_snapshot_inspect(void *serializer, void *map, void *out, sh_rawmap_snapshot_visit visit, void *ctx)
{ (void)serializer; (void)map; (void)out; (void)visit; (void)ctx; assert(0); return 0; }
void sh_nav_bake_refresh_live(void) { assert(0); }
void sh_nav_bake_refresh_live_partial(void) { assert(0); }
int sh_nav_bake_prepare(sh_nav_bake_reader reader)
{ (void)reader; assert(nav_test_allowed); nav_prepare_calls++; return nav_ready; }
void sh_nav_bake_conflicts(int *marked, int *rooms)
{ assert(nav_test_allowed); *marked=0; *rooms=nav_rooms; }
void sh_nav_bake_summary_get(sh_nav_bake_summary *s)
{
    assert(nav_test_allowed); memset(s, 0, sizeof *s);
    s->have_map = 1; s->rooms = 2; s->refused = nav_rooms; s->built = 2 - nav_rooms;
}
unsigned long sh_nav_bake_geometry_revision(void) { return 0; }
int sh_nav_bake_preview_pending(void) { assert(0); return 0; }
unsigned long long sh_nav_bake_overlay_signature(void) { return 0; }
void sh_nav_bake_overlay_clear(void) { }
int sh_grid_native_refresh_placement(void) { return 0; }
int sh_nav_preview_published(void *world) { (void)world; assert(0); return 0; }
int sh_nav_bake_refresh_volumes(int *volumes) { (void)volumes; assert(0); return 0; }
int sh_nav_bake_volume_flagged(int uid) { (void)uid; assert(0); return 0; }
int sh_nav_bake_volume_uids(int *out, int cap) { (void)out; (void)cap; return 0; }
int sh_nav_bake_knows_volume(int uid) { (void)uid; return 0; }
int sh_nav_bake_instance_count(void) { return 0; }
int sh_nav_bake_has_map(void) { return 1; }
void sh_nav_bake_volume_counts(int *floors, int *walls)
{ if (floors) *floors = 0; if (walls) *walls = 0; }
const char *sh_nav_bake_volumes_reason(void) { return ""; }
int sh_nav_bake_preview(sh_nav_bake_reader reader, sh_nav_preview_line line, sh_nav_preview_colour_fn colour, void *ctx) { (void)reader; (void)line; (void)colour; (void)ctx; assert(0); return 0; }
int sh_nav_preview_install(const sig_result *r, size_t n) { (void)r; (void)n; assert(0); return 0; }
void sh_nav_preview_begin(void *world) { (void)world; assert(0); }
void sh_nav_preview_add_line(const float start[3], const float end[3], void *unused) { (void)start; (void)end; (void)unused; assert(0); }
void sh_nav_preview_publish(void) { assert(0); }
void sh_nav_preview_clear(void) { assert(nav_test_allowed); }
void sh_nav_preview_colour(float r, float g, float b) { (void)r; (void)g; (void)b; assert(0); }

int sh_iface_engine_copy_paste_enabled(void) { assert(0); return 0; }
