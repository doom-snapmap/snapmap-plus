/* Exercise deferred saves across editor lifecycle changes without game code. */
#include "../src/backend/editor_frame.c"

static unsigned long generation;
static int writes, choices, failed;
static char current[MAX_PATH] = "C:\\maps\\current.json", written[MAX_PATH];
static unsigned char editor[0x24000];
static int map_a, map_b;
#define CHECK(x) do { if (!(x)) { ++failed; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

void backend_log(const char *message) { (void)message; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; return NULL; }
int hook_commit(void *trampoline) { (void)trampoline; return B2_PATCH_OK; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 1; }
int sh_rawmap_live_serialize_ready(void) { return 1; }
unsigned long sh_rawmap_load_generation(void) { return generation; }
int sh_rawmap_load_is_safe(void) { return 1; }
int sh_rawmap_source_ok(char *msg, int cap) { (void)msg; (void)cap; return 1; }
int sh_rawmap_swap_is_armed(void) { return 0; }
int sh_rawmap_swap_arm(int on) { return on; }
void sh_rawmap_get_paths(char *load, int load_cap, char *save, int save_cap)
{ (void)load; (void)load_cap; if (save) strcpy_s(save, save_cap, current); }
int sh_rawmap_dest_writable_now(const char *path, char *msg, int cap)
{ (void)path; (void)msg; (void)cap; return 1; }
int sh_rawmap_set_save_target(const char *path)
{ ++choices; strcpy_s(current, sizeof current, path); return 1; }
int sh_rawmap_write_from_live(void *map, const char *path, char *msg, int cap,
                              unsigned long long *bytes)
{ (void)map; (void)msg; (void)cap; (void)bytes; ++writes; strcpy_s(written, sizeof written, path); return 1; }
static void original_frame(void *ed, void *arg) { (void)ed; (void)arg; }

static void reset(void)
{
    memset(editor, 0, sizeof editor);
    *(void **)(editor + ED_MAP_PTR_OFF) = &map_a;
    *(int *)(editor + ED_STATE_OFF) = 1;
    g_editor_obj = (uintptr_t)editor;
    g_frame_orig = original_frame;
    g_save_pending = g_faulted = 0;
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
    printf("editor_frame_test: %d failures\n", failed);
    return failed ? 1 : 0;
}
