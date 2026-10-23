/* Exercise deferred saves across editor lifecycle changes without game code. */
#include "../src/backend/editor_frame.c"

static unsigned long generation;
static int writes, choices, failed;
static char current[MAX_PATH] = "C:\\maps\\current.json", written[MAX_PATH];
static unsigned char editor[0x24000];
static int map_a, map_b;
static int loads, defer_load, load_result, load_without_parse, load_fault, armed;
static int native_frames, transition_frame, frame_refused;
static sh_rawmap_saved_completion load_completion;
static void *load_context;
#define CHECK(x) do { if (!(x)) { ++failed; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
void sh_prefab_import_poll(void *ed) { CHECK(ed == editor && transition_frame); }
static int anchor_frames, anchor_after_native;
/* The anchor observes the hover the native update settled on, outside the transition bracket. */
void sh_prefab_anchor_frame(void *ed)
{ CHECK(ed == editor && !transition_frame); anchor_frames++; anchor_after_native = native_frames; }

void backend_log(const char *message) { (void)message; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; return NULL; }
int hook_commit(void *trampoline) { (void)trampoline; return B2_PATCH_OK; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 1; }
int sh_rawmap_live_serialize_ready(void) { return 1; }
int sh_editor_transition_frame_enter(void *ed)
{ CHECK(ed == editor && !transition_frame); return frame_refused ? 0 : (transition_frame = 1); }
void sh_editor_transition_frame_leave(void *ed)
{ CHECK(ed == editor && transition_frame); transition_frame = 0; }
void sh_rawmap_pending_map_poll(void) { CHECK(transition_frame); }
unsigned long sh_rawmap_load_generation(void) { return generation; }
int sh_rawmap_load_is_safe(void) { return 1; }
int sh_rawmap_source_ok(char *msg, int cap) { (void)msg; (void)cap; return 1; }
int sh_rawmap_swap_is_armed(void) { return armed; }
int sh_rawmap_swap_arm(int on) { return armed = on; }
int sh_rawmap_observe_saved_load(void *ed, const char *id, sh_rawmap_saved_completion completion, void *context)
{
    CHECK(ed == editor && !strcmp(id, "0123456789ABCDEF0123"));
    CHECK(g_reload_waiting && !g_pending && !load_completion);
    if (!defer_load) return 0;
    load_completion = completion; load_context = context; return 1;
}
void sh_rawmap_get_paths(char *load, int load_cap, char *save, int save_cap)
{ (void)load; (void)load_cap; if (save) strcpy_s(save, save_cap, current); }
int sh_rawmap_dest_writable_now(const char *path, char *msg, int cap)
{ (void)path; (void)msg; (void)cap; return 1; }
int sh_rawmap_set_save_target(const char *path)
{ ++choices; strcpy_s(current, sizeof current, path); return 1; }
int sh_rawmap_write_from_live(void *map, const char *path, char *msg, int cap,
                              unsigned long long *bytes)
{ (void)map; (void)msg; (void)cap; (void)bytes; ++writes; strcpy_s(written, sizeof written, path); return 1; }
static void original_frame(void *ed, void *arg)
{ (void)arg; CHECK(ed == editor && !transition_frame); native_frames++; }
static int native_load(void *ed, void *name)
{
    const ef_idstr *text = name;
    CHECK(ed == editor && !strcmp(text->data, "0123456789ABCDEF0123") && text->len == 20);
    CHECK(armed && !g_pending && g_reload_waiting);
    CHECK(transition_frame && !native_frames);
    loads++;
    if (load_fault) RaiseException(0xe0422067, 0, 0, NULL);
    if (!defer_load && !load_without_parse) generation++;
    return load_result;
}

static void reset(void)
{
    memset(editor, 0, sizeof editor);
    *(void **)(editor + ED_MAP_PTR_OFF) = &map_a;
    *(int *)(editor + ED_STATE_OFF) = 1;
    g_editor_obj = (uintptr_t)editor;
    g_frame_orig = original_frame;
    g_save_pending = g_faulted = 0;
    g_pending = g_reload_waiting = 0; g_state = SH_RELOAD_IDLE;
    g_load_map = NULL; load_completion = NULL; load_context = NULL;
    loads = defer_load = load_result = load_without_parse = load_fault = armed = 0;
    native_frames = transition_frame = frame_refused = 0;
    generation = 0;
    writes = choices = 0;
    strcpy_s(current, sizeof current, "C:\\maps\\current.json");
}

int main(void)
{
    char msg[192];
    reset();
    CHECK(sh_editor_frame_request_rawmap_save_to("C:\\maps\\chosen.json", msg, sizeof msg));
    CHECK(!choices && !writes);
    CHECK(!sh_editor_frame_request_rawmap_save_to("C:\\maps\\other.json", msg, sizeof msg));
    sh_editor_frame_detour(editor, NULL);
    CHECK(writes == 1 && choices == 1 && !strcmp(written, "C:\\maps\\chosen.json"));
    CHECK(anchor_frames == 1 && anchor_after_native == 1);
    sh_editor_frame_detour(editor, NULL);
    CHECK(writes == 1);

    reset();
    CHECK(sh_editor_frame_request_rawmap_save(msg, sizeof msg));
    strcpy_s(current, sizeof current, "C:\\maps\\later-setting.json");
    sh_editor_frame_detour(editor, NULL);
    CHECK(writes == 1 && !choices && !strcmp(written, "C:\\maps\\current.json"));

    reset();
    CHECK(sh_editor_frame_request_rawmap_save_to("C:\\maps\\chosen.json", msg, sizeof msg));
    *(int *)(editor + ED_STATE_OFF) = 0;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!writes && !choices && !g_save_pending);

    reset();
    CHECK(sh_editor_frame_request_rawmap_save(msg, sizeof msg));
    *(int *)(editor + ED_SUBSTATE_OFF) = 5;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!writes);

    reset();
    CHECK(sh_editor_frame_request_rawmap_save(msg, sizeof msg));
    *(void **)(editor + ED_MAP_PTR_OFF) = &map_b;
    sh_editor_frame_detour(editor, NULL);
    CHECK(!writes);

    reset();
    CHECK(sh_editor_frame_request_rawmap_save(msg, sizeof msg));
    ++generation; /* The next map may reuse exactly the same allocation. */
    sh_editor_frame_detour(editor, NULL);
    CHECK(!writes);
    /* Native entry may return while consent still owns the original load. */
    for (int outcome = 0; outcome < 2; outcome++) {
        reset(); g_load_map = native_load; g_pending = 1; g_state = SH_RELOAD_PENDING;
        strcpy_s(g_reload_id, sizeof(g_reload_id), "0123456789ABCDEF0123"); defer_load = 1;
        sh_editor_frame_detour(editor, NULL);
        CHECK(loads == 1 && load_completion && g_reload_waiting && !g_pending && !armed);
        CHECK(sh_editor_frame_reload(msg, sizeof(msg)) == SH_RELOAD_PENDING);
        CHECK(!sh_editor_frame_request_reload(msg, sizeof(msg)) && strstr(msg, "in progress"));
        for (int frame = 0; frame < 3; frame++) sh_editor_frame_detour(editor, NULL);
        CHECK(loads == 1);
        sh_rawmap_saved_completion completion = load_completion; load_completion = NULL;
        completion(load_context, outcome);
        CHECK(!g_reload_waiting && sh_editor_frame_reload(msg, sizeof(msg)) ==
            (outcome ? SH_RELOAD_DONE : SH_RELOAD_FAILED));
        sh_editor_frame_detour(editor, NULL); CHECK(loads == 1);
    }
    for (int scenario = 0; scenario < 5; scenario++) {
        reset(); g_load_map = native_load; g_pending = 1; g_state = SH_RELOAD_PENDING;
        strcpy_s(g_reload_id, sizeof(g_reload_id), "0123456789ABCDEF0123");
        if (scenario == 1) load_without_parse = 1;
        if (scenario == 2) load_result = 9;
        if (scenario == 3) load_fault = 1;
        if (scenario == 4) armed = 1;
        sh_editor_frame_detour(editor, NULL);
        CHECK(loads == 1 && !g_reload_waiting && !load_completion && !g_pending);
        CHECK(armed == (scenario == 4));
        CHECK(sh_editor_frame_reload(msg, sizeof(msg)) ==
            (scenario == 0 || scenario == 4 ? SH_RELOAD_DONE : SH_RELOAD_FAILED));
        CHECK(g_faulted == (scenario == 3));
        CHECK(native_frames == 1 && !transition_frame);
    }
    reset(); g_load_map = native_load; g_pending = 1; frame_refused = 1;
    sh_editor_frame_detour(editor, NULL);
    CHECK(native_frames == 1 && !loads && g_pending && !transition_frame);
    printf("editor_frame_test: %d failures\n", failed);
    return failed ? 1 : 0;
}
