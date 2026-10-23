/* Native editor ownership, activation timing and interrupted reload cleanup. */
#include <assert.h>
#include "../src/backend/editor_transition.c"

static unsigned char editor[0x24000], common[0x10000], render[64], sound[64], heap[0x100];
static void *editor_table[16], *render_table[40], *sound_table[8];
static void *render_slot, *sound_slot, *common_slot;
static DWORD main_thread;
static int mode, map_heap, native_loads, native_reads, activations, commits, full_load;
static int clears, frees, starts, stops, updates, pushes, pops, release_faults;
static const char save_id[] = "0123456789abcdef0123";
enum { NORMAL, ACTIVATE_FAIL, READ_FAIL, READ_THROW, COMMIT_FAIL, NO_READ,
    LOAD_FAIL, ACTIVATE_THROW, STOP_THROW, CLEAR_THROW, START_THROW, UPDATE_THROW,
    HOLD_FAIL, RELEASE_FAIL, WRONG_ID, WRONG_ARGUMENT, REPEAT_READ, COMMIT_THROW,
    RELEASE_STUCK };
#define FIELD(offset) (*(void **)(editor + (offset)))
#define COUNT (*(int *)(render + 0x14))
#define DEPTH (*(int *)(heap + 0xc4))

void backend_log(const char *message) { (void)message; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; assert(0); return 0; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline)
{ (void)trampoline; assert(0); return B2_PATCH_REFUSED_BADARG; }
int hook_unpatch(void *trampoline) { (void)trampoline; assert(0); return 0; }
int hook_is_installed(void *trampoline) { (void)trampoline; assert(0); return 0; }
static void fail(void) { RaiseException(0xe0530061, 0, 0, NULL); }
static void *get_heap(void) { return heap; }
static void push_heap(void *self, int id)
{
    assert(self == heap && !id && DEPTH == 0);
    *(int *)(heap + 0x44) = id; DEPTH++; pushes++;
}
static void pop_heap(void *self)
{ assert(self == heap && DEPTH == 1); DEPTH--; pops++; }
static void synchronize(void *self, unsigned char a, unsigned char b, unsigned char c)
{ assert(self == render && !a && !b && c); if (mode == HOLD_FAIL) fail(); }
static void adjust(void *self, int delta)
{
    assert(self == render && (delta == 1 || delta == -1));
    if (delta < 0 && (mode == RELEASE_STUCK ||
        (mode == RELEASE_FAIL && release_faults++ < 2))) fail();
    COUNT += delta;
}
static void update(void *self)
{ assert(self == render && COUNT == 1 && full_load); updates++; if (mode == UPDATE_THROW) fail(); }
static void make_worlds(void)
{
    static const size_t required[] = {0x30, 0x38, 0x98, 0xa8, 0x2a8};
    FIELD(ET_WORLD) = (void *)11; FIELD(ET_SOUND) = (void *)12; FIELD(ET_UI) = (void *)13;
    for (size_t i = 0; i < sizeof(required)/sizeof(required[0]); i++)
        FIELD(ET_SCENE + required[i]) = (void *)14;
}
static void clear_scene(void *scene)
{
    assert(scene == editor + ET_SCENE && DEPTH == 1 && COUNT == 1 && FIELD(ET_WORLD));
    clears++; if (mode == CLEAR_THROW) fail(); FIELD(ET_WORLD) = NULL;
}
static void free_sound(void *self, void *world)
{ assert(self == sound && world == FIELD(ET_SOUND) && DEPTH == 1 && COUNT == 1); frees++; }
static void set_active(void *self, unsigned char active)
{
    assert(self == editor && COUNT == 1);
    if (!!editor[9] == !!active) return;
    if (active) {
        starts++;
        assert(!FIELD(ET_MAP) && !FIELD(ET_EDIT) && !FIELD(ET_UI) && !FIELD(ET_WORLD) && !FIELD(ET_SOUND));
        if (mode == START_THROW) fail();
        make_worlds(); editor[9] = 1;
    } else {
        stops++;
        if (mode == STOP_THROW) fail();
        FIELD(ET_UI) = NULL;
        if (map_heap) {
            push_heap(heap, 0);
            if (FIELD(ET_WORLD)) clear_scene(editor + ET_SCENE);
            if (FIELD(ET_SOUND)) { free_sound(sound, FIELD(ET_SOUND)); FIELD(ET_SOUND) = NULL; }
            pop_heap(heap);
        }
        editor[9] = 0;
    }
}
static int read_saved(const char *id, void **map, void *modules, void *palette, void *progress)
{
    native_reads++;
    if (!g_et.phase) return 7; /* Calls outside the owned reload keep native behavior. */
    assert(!strcmp(id, save_id) && map == (void **)(editor + ET_MAP));
    assert(modules == editor + 0x206c0 && palette == editor + 0x20660 && progress == editor + 0x236d8);
    assert(activations == 1 && starts == 1 && !commits && editor[9] && COUNT == 1 && !DEPTH);
    assert(!sh_editor_transition_at_boundary());
    if (mode == READ_THROW) fail();
    if (mode == READ_FAIL) return 1;
    *map = (void *)21; return 0;
}
static int load_saved(void *self, const void *name)
{
    int result;
    assert(self == editor && !strcmp(*(const char *const *)((const unsigned char *)name + 0x10), save_id));
    assert(FIELD(ET_MAP) == (void *)1 && FIELD(ET_EDIT) == (void *)2 && COUNT == 1 && !commits);
    native_loads++;
    if (mode == NO_READ) return 0;
    /* This is the native destruction before the verified saved-read callsite. */
    FIELD(ET_MAP) = FIELD(ET_EDIT) = NULL;
    result = et_read_from(mode == WRONG_ID ? "fedcba98765432100123" : save_id,
        (void **)(editor + ET_MAP), editor + 0x206c0,
        editor + 0x20660 + (mode == WRONG_ARGUMENT), editor + 0x236d8, g_et_api.read_return);
    if (!result && mode == REPEAT_READ)
        result = et_read_from(save_id, (void **)(editor + ET_MAP), editor + 0x206c0,
            editor + 0x20660, editor + 0x236d8, g_et_api.read_return);
    if (result) return result;
    FIELD(ET_EDIT) = (void *)22;
    full_load = 1;
    assert(!commits);
    return mode == LOAD_FAIL;
}
static int activate(void *context)
{
    assert(context == editor && et_empty() && COUNT == 1 && !DEPTH);
    assert(sh_editor_transition_at_boundary() && !commits && !native_reads);
    COUNT++; assert(!sh_editor_transition_at_boundary()); COUNT--;
    activations++;
    if (mode == ACTIVATE_THROW) fail();
    return mode != ACTIVATE_FAIL;
}
static int commit(void *context)
{
    assert(context == editor && activations == 1 && full_load && !COUNT && !DEPTH);
    assert(FIELD(ET_MAP) == (void *)21 && FIELD(ET_EDIT) == (void *)22 && updates == 1);
    assert(!sh_editor_transition_at_boundary()); commits++;
    if (mode == COMMIT_THROW) fail();
    return mode != COMMIT_FAIL;
}
static void reset(void)
{
    assert(!g_et.render.active && !g_et.heap.active && !g_et_frame);
    memset(&g_et_api, 0, sizeof(g_et_api)); memset(&g_et, 0, sizeof(g_et));
    memset(editor, 0, sizeof(editor)); memset(common, 0, sizeof(common));
    memset(render, 0, sizeof(render)); memset(sound, 0, sizeof(sound)); memset(heap, 0, sizeof(heap));
    *(void ***)editor = editor_table; *(void ***)render = render_table; *(void ***)sound = sound_table;
    editor_table[0x48/8] = set_active;
    render_table[0x80/8] = synchronize; render_table[0x110/8] = adjust; render_table[0x108/8] = update;
    sound_table[0x28/8] = free_sound;
    render_slot = render; sound_slot = sound; common_slot = common; main_thread = GetCurrentThreadId();
    g_et_api.editor = editor; g_et_api.renderer = &render_slot; g_et_api.sound = &sound_slot;
    g_et_api.common = &common_slot; g_et_api.thread = &main_thread;
    g_et_api.set_active = set_active; g_et_api.clear_scene = clear_scene;
    g_et_api.heap.get = get_heap; g_et_api.heap.push = push_heap; g_et_api.heap.pop = pop_heap;
    g_et_api.read = read_saved; g_et_api.read_return = (void *)123; g_et_api.installed = 1;
    editor[8] = editor[9] = 1; FIELD(ET_MAP) = (void *)1; FIELD(ET_EDIT) = (void *)2; make_worlds();
    mode = NORMAL; map_heap = 1;
    native_loads = native_reads = activations = commits = full_load = clears = frees = 0;
    starts = stops = updates = pushes = pops = release_faults = 0;
}
static int run(void)
{
    unsigned char name[0x30] = {0}; char error[512] = "";
    const sh_editor_transition_callbacks callbacks = {editor, activate, commit};
    int result;
    *(const char **)(name + 0x10) = save_id;
    assert(sh_editor_transition_capture(editor) == (void *)1);
    assert(sh_editor_transition_frame_enter(editor));
    assert(!sh_editor_transition_frame_enter(editor));
    result = sh_editor_transition_load(editor, (void *)1, name, load_saved, &callbacks, error, sizeof(error));
    assert(!!result == !!error[0]);
    assert(!sh_editor_transition_at_boundary());
    sh_editor_transition_frame_leave(editor);
    return result;
}
static DWORD WINAPI wrong_thread(void *unused)
{
    (void)unused; assert(!sh_editor_transition_capture(editor));
    assert(!sh_editor_transition_frame_enter(editor)); return 0;
}
static void admission_cases(void)
{
    HANDLE thread; unsigned char name[0x30] = {0}; char error[128] = "";
    const sh_editor_transition_callbacks callbacks = {editor, activate, commit};
    reset(); *(const char **)(name + 0x10) = save_id;
    assert(sh_editor_transition_load(editor, (void *)1, name, load_saved, &callbacks, error, sizeof(error)));
    assert(!native_loads && FIELD(ET_MAP) == (void *)1 && editor[9]);
    thread = CreateThread(NULL, 0, wrong_thread, NULL, 0, NULL);
    assert(thread && WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0); CloseHandle(thread);
    assert(!sh_editor_transition_capture(editor + 1));
    editor[0x2120d] = 1; assert(!sh_editor_transition_capture(editor)); editor[0x2120d] = 0;
    common[0xf576] = 1; assert(!sh_editor_transition_capture(editor)); common[0xf576] = 0;
    assert(sh_editor_transition_frame_enter(editor));
    error[0] = 0;
    assert(sh_editor_transition_load(editor, (void *)3, name, load_saved, &callbacks, error, sizeof(error)));
    assert(!native_loads && FIELD(ET_MAP) == (void *)1 && editor[9]);
    sh_editor_transition_frame_leave(editor);
    assert(et_read_from("unowned", NULL, NULL, NULL, NULL, (void *)321) == 7 && native_reads == 1);
}
int main(void)
{
    admission_cases();
    for (int allocation = 0; allocation <= 1; allocation++) {
        reset(); map_heap = allocation;
        assert(!run());
        assert(native_loads == 1 && native_reads == 1 && activations == 1 && commits == 1);
        assert(clears == 1 && frees == 1 && starts == 1 && stops == 1 && editor[9]);
        assert(!COUNT && !DEPTH && pushes == pops && !g_et.phase && !g_et_api.faulted);
    }
    for (int failure = ACTIVATE_FAIL; failure <= COMMIT_THROW; failure++) {
        reset(); mode = failure;
        /* Throwing native teardown must not be replayed; use the separately
         * owned process heap for the scene-clear exception. */
        if (mode == CLEAR_THROW) map_heap = 0;
        assert(run());
        if (mode == RELEASE_FAIL) {
            /* Both synchronous releases failed before decrementing. Keep the
             * real hold owned until a later frame can release it; never fake
             * the counter or forget the scope just to report clean failure. */
            assert(COUNT == 1 && !DEPTH && pushes == pops && g_et.phase == ET_RECOVERY);
            assert(!commits && !g_et.callbacks.activate && !g_et.callbacks.commit);
            assert(sh_editor_transition_frame_enter(editor));
            sh_editor_transition_frame_leave(editor);
            assert(activations == 1 && native_loads == 1 && native_reads == 1);
        }
        assert(!COUNT && !DEPTH && pushes == pops && !g_et.phase);
        assert(commits == (mode == COMMIT_FAIL || mode == COMMIT_THROW));
        if (mode == HOLD_FAIL || mode == NO_READ) {
            assert(editor[9] && FIELD(ET_MAP) == (void *)1 && !activations);
        } else if (mode == WRONG_ID || mode == WRONG_ARGUMENT) {
            assert(!activations && !native_reads);
        } else if (mode == STOP_THROW || mode == CLEAR_THROW || mode == START_THROW) {
            assert(g_et_api.faulted && stops == 1 && !sh_editor_transition_capture(editor));
            assert(clears <= 1 && !sh_editor_transition_frame_enter(editor));
        } else {
            assert(!editor[9] && !FIELD(ET_UI) && !FIELD(ET_WORLD) && !FIELD(ET_SOUND));
            assert(!g_et_api.faulted);
        }
    }
    reset(); mode = RELEASE_STUCK;
    assert(run() && COUNT == 1 && !DEPTH && !commits && g_et.phase == ET_RECOVERY);
    for (int frame = 0; frame < 3; frame++) {
        assert(!sh_editor_transition_frame_enter(editor));
        assert(COUNT == 1 && !g_et_frame && !commits && activations == 1);
    }
    mode = NORMAL;
    assert(sh_editor_transition_frame_enter(editor));
    assert(!COUNT && !DEPTH && !g_et.phase && pushes == pops);
    sh_editor_transition_frame_leave(editor);
    puts("editor transition ownership and failure cases passed"); return 0;
}
