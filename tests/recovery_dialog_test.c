/* Exercises the production save guard and Frame route with a native dialog
 * lifetime model. The real wrapper's ABI is checked against both PE images. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/fault_shield/recovery.c"

uint8_t *g_doom_base;
shield_engine g_eng;
static int failed, clears, emitted, widget_visible, callback_alive;
static int orig_calls, maintenance_calls, throw_frame, arm_in_frame;
/* Frame's real first bytes, so recovery_install accepts it. */
static unsigned char frame_bytes[FRAME_STOLEN] = {
    0x40, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0xC0, 0x1D, 0x01, 0x00
};
static unsigned char manager[0x1000], shell_manager[0x100], queue[4 * DLG_DESC_STRIDE];
static void *shell[8];
static void *shell_slot;
static void fake_clear(void *self, void *block);

static int dialog_asks, dialog_can_ask, main_frames;
static const char *dialog_text;

int sh_engine_dialog_can_ask(void) { return dialog_can_ask; }
int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text)
{ (void)gdm_id; (void)button_set; dialog_asks++; dialog_text = text; return 7; }
int sh_engine_dialog_poll(int ticket) { (void)ticket; return SH_ENGINE_DIALOG_PENDING; }
void sh_engine_dialog_release(int ticket) { (void)ticket; }
sh_patch_status code_patch(void *at, const uint8_t *expect, const uint8_t *bytes, size_t n, sh_patch_handle *h)
{ (void)at; (void)expect; (void)bytes; (void)n; if (h) h->live = 1; return B2_PATCH_OK; }
void sh_commands_main_frame(void) { main_frames++; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)detour; (void)stolen; return target; }
sh_patch_status hook_commit(void *original) { (void)original; return B2_PATCH_OK; }
int hook_is_installed(void *original) { (void)original; return 0; }
int hook_unpatch(void *original) { (void)original; return 1; }

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); failed++; \
} } while (0)

void shield_emit(const shield_fault *fault)
{
    CHECK(fault && fault->message);
    emitted++;
}

uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{
    (void)base; (void)name; (void)status;
    return 0;
}

void sh_apply_prefab_poll_play(void)
{
    CHECK(orig_calls == maintenance_calls + 1);
    maintenance_calls++;
}

static int64_t fake_frame(void *self)
{
    CHECK(self == shell);
    orig_calls++;
    if (throw_frame) RaiseException(0xe0000042, 0, 0, NULL);
    if (arm_in_frame) InterlockedExchange(&g_armed, 1);
    return 123;
}

/* ClearDialogWrapper hides an active top widget before marking it for removal.
 * No button is dispatched and a non-top clear leaves the current widget alone. */
static void fake_clear(void *self, void *block)
{
    uint32_t *params = block;
    const char *source = NULL;
    int count = *(int *)(manager + DLGQ_COUNT_OFF);
    CHECK(self == shell);
    memcpy(&source, &params[0x26], sizeof source);
    CHECK(source && strcmp(source, "snapmap-plus save guard") == 0);
    CHECK(params[0x28] > 0 && params[1] == 0 && params[3] == 0);
    clears++;
    for (int i = 0; i < count; i++) {
        unsigned char *desc = queue + (size_t)i * DLG_DESC_STRIDE;
        if (*(int *)desc != (int)params[0] || desc[DESC_CLEARFLAG_OFF]) continue;
        desc[DESC_CLEARFLAG_OFF] = 1;
        if (i == 0) {
            manager[DLGMGR_ACTIVE_OFF] = 0;
            widget_visible = 0;
        }
        return;
    }
}

static void reset(void)
{
    memset(manager, 0, sizeof manager);
    memset(shell_manager, 0, sizeof shell_manager);
    memset(queue, 0, sizeof queue);
    memset(shell, 0, sizeof shell);
    shell[1] = manager;
    shell[3] = shell_manager;
    shell_slot = shell;
    *(void **)(manager + DLGQ_ARR_OFF) = queue;
    g_shell_slot_at = (uint8_t *)&shell_slot;
    g_doom_base = manager;
    g_clear_dialog = fake_clear;
    g_editor_at = g_load_state_at = g_last_err_at = g_suppr_a_at = NULL;
    g_armed = 0;
    g_damaged_text = NULL; g_damaged_wait = 0; g_damaged_ticket = 0;
    g_refusal_frames = 0;
    dialog_asks = dialog_can_ask = main_frames = 0;
    dialog_text = NULL;
    memset(&g_eng, 0, sizeof g_eng);
    g_state = g_frames = 0;
    clears = emitted = widget_visible = callback_alive = 0;
    orig_calls = maintenance_calls = throw_frame = arm_in_frame = 0;
    orig_frame = fake_frame;
}

static void test_mixed_queue_retirement(void)
{
    reset();
    *(int *)(manager + DLGQ_COUNT_OFF) = 2;
    *(int *)queue = GDM_LOAD_DAMAGED_FILE;
    *(int *)(queue + DLG_DESC_STRIDE) = 0x29; /* package consent remains queued */
    manager[DLGMGR_ACTIVE_OFF] = 1;
    widget_visible = callback_alive = 1;
    save_guard_tick();
    CHECK(clears == 1 && emitted == 1);
    CHECK(queue[DESC_CLEARFLAG_OFF] == 1);
    CHECK(queue[DLG_DESC_STRIDE + DESC_CLEARFLAG_OFF] == 0);
    CHECK(!manager[DLGMGR_ACTIVE_OFF] && !widget_visible);
    CHECK(callback_alive); /* Native Think has not retired its owner yet. */

    /* Think releases the old owner while consent remains. A raw flag write
     * would leave a visible widget calling this now-dead callback. */
    callback_alive = 0;
    CHECK(!widget_visible || callback_alive);
    memmove(queue, queue + DLG_DESC_STRIDE, DLG_DESC_STRIDE);
    *(int *)(manager + DLGQ_COUNT_OFF) = 1;
    CHECK(*(int *)queue == 0x29 && !manager[DLGMGR_ACTIVE_OFF]);
    save_guard_tick();
    CHECK(clears == 1); /* The guard never clears the consent dialog. */
}

static void test_guard_refusals_and_selection(void)
{
    reset();
    *(int *)(manager + DLGQ_COUNT_OFF) = 4;
    /* The browser scan's "removed corrupt data" notice (0x8D) is never the guard's:
     * the scan itself is replaced (saved_map_guard.c), and the notice says what the
     * engine did. */
    const int ids[] = {GDM_LOAD_DAMAGED_FILE, GDM_CORRUPT_CONTINUE, GDM_SNAP_REVERT_TO_PREVIOUS, 0x8d};
    for (int i = 0; i < 4; i++) *(int *)(queue + (size_t)i * DLG_DESC_STRIDE) = ids[i];
    g_clear_dialog = NULL;
    save_guard_tick();
    CHECK(!clears && !queue[DESC_CLEARFLAG_OFF]);
    g_clear_dialog = fake_clear;
    *(int *)(manager + DLGQ_COUNT_OFF) = 5;
    save_guard_tick();
    CHECK(!clears && !queue[DESC_CLEARFLAG_OFF]);
    /* Without a refusal of Snapmap+'s, only the offer to delete is dismissed. */
    *(int *)(manager + DLGQ_COUNT_OFF) = 4;
    save_guard_tick();
    CHECK(clears == 1 && queue[DESC_CLEARFLAG_OFF] == 1);
    CHECK(!queue[DLG_DESC_STRIDE + DESC_CLEARFLAG_OFF] && !queue[2 * DLG_DESC_STRIDE + DESC_CLEARFLAG_OFF]);
    /* After Snapmap+ refused the load, the "damaged" notice and the offer to
     * replace the map with its published version go too. */
    shield_note_refused_load();
    save_guard_tick();
    CHECK(clears == 3);
    CHECK(queue[DLG_DESC_STRIDE + DESC_CLEARFLAG_OFF] == 1 && queue[2 * DLG_DESC_STRIDE + DESC_CLEARFLAG_OFF] == 1);
    CHECK(!queue[3 * DLG_DESC_STRIDE + DESC_CLEARFLAG_OFF]);
    save_guard_tick();
    CHECK(clears == 3);
    /* The refusal lasts a bounded number of frames. */
    reset();
    *(int *)(manager + DLGQ_COUNT_OFF) = 1;
    *(int *)queue = GDM_CORRUPT_CONTINUE;
    shield_note_refused_load();
    for (int i = 0; i < REFUSAL_FRAMES; i++) { if (i == 0) *(int *)(manager + DLGQ_COUNT_OFF) = 0; save_guard_tick(); }
    *(int *)(manager + DLGQ_COUNT_OFF) = 1;
    save_guard_tick();
    CHECK(!clears);

    reset();
    *(int *)(manager + DLGQ_COUNT_OFF) = 2;
    *(int *)queue = 0x29;
    *(int *)(queue + DLG_DESC_STRIDE) = GDM_LOAD_DAMAGED_FILE;
    manager[DLGMGR_ACTIVE_OFF] = 1;
    widget_visible = callback_alive = 1;
    save_guard_tick();
    CHECK(clears == 1 && !queue[DESC_CLEARFLAG_OFF]);
    CHECK(widget_visible && manager[DLGMGR_ACTIVE_OFF]);
    CHECK(queue[DLG_DESC_STRIDE + DESC_CLEARFLAG_OFF] == 1);
}

static void test_frame_route(void)
{
    reset();
    CHECK(frame_detour(shell) == 123);
    CHECK(orig_calls == 1 && maintenance_calls == 1);

    reset();
    arm_in_frame = 1;
    CHECK(frame_detour(shell) == 123);
    CHECK(orig_calls == 1 && !maintenance_calls);

    reset();
    throw_frame = 1;
    __try {
        frame_detour(shell);
        CHECK(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    CHECK(orig_calls == 1 && !maintenance_calls);
}

static void test_install(void)
{
    /* Without ClearDialog the save guard is off, and the Frame route installs. */
    reset();
    g_clear_dialog = NULL;
    g_eng.frame = (uintptr_t)frame_bytes;
    CHECK(recovery_install());
    CHECK(!g_clear_dialog && emitted >= 1);

    reset();
    g_clear_dialog = NULL;
    g_eng.frame = (uintptr_t)frame_bytes;
    g_eng.clear_dialog = (uintptr_t)fake_clear;
    orig_frame = NULL;
    CHECK(recovery_install());
    CHECK(g_clear_dialog == fake_clear);

    /* A Frame another program changed is never patched. */
    reset();
    orig_frame = NULL;
    frame_bytes[0] = 0xE9;
    g_eng.frame = (uintptr_t)frame_bytes;
    CHECK(!recovery_install());
    frame_bytes[0] = 0x40;
}

/* A dismissed damaged save is explained once the engine can show a dialog. */
static void test_damaged_notice(void)
{
    reset();
    *(int *)(manager + DLGQ_COUNT_OFF) = 1;
    *(int *)queue = GDM_LOAD_DAMAGED_FILE;
    CHECK(frame_detour(shell) == 123);
    CHECK(clears == 1 && !dialog_asks);           /* the dialog is still leaving the queue */
    dialog_can_ask = 1;
    CHECK(frame_detour(shell) == 123);
    CHECK(dialog_asks == 1 && dialog_text && strstr(dialog_text, "did not let the game delete it"));
    CHECK(!strstr(dialog_text, "kept"));   /* nothing the dismissal cannot guarantee */
    CHECK(frame_detour(shell) == 123);
    CHECK(dialog_asks == 1);                       /* one notice per dismissal */
    CHECK(main_frames == 3);                       /* the console registration tick runs every frame */
}

/* After a refusal the dismissed notices are not replaced: Snapmap+ already
 * said why. A sentence handed to shield_tell is shown once. */
static void test_refusal_is_quiet_and_tell_shows(void)
{
    reset();
    *(int *)(manager + DLGQ_COUNT_OFF) = 1;
    *(int *)queue = GDM_CORRUPT_CONTINUE;
    dialog_can_ask = 1;
    shield_note_refused_load();
    CHECK(frame_detour(shell) == 123 && frame_detour(shell) == 123);
    CHECK(clears == 1 && !dialog_asks);
    shield_tell("one sentence");
    CHECK(frame_detour(shell) == 123);
    CHECK(dialog_asks == 1 && !strcmp(dialog_text, "one sentence"));
}

int main(void)
{
    test_mixed_queue_retirement();
    test_guard_refusals_and_selection();
    test_frame_route();
    test_install();
    test_damaged_notice();
    test_refusal_is_quiet_and_tell_shows();
    if (failed) return 1;
    puts("recovery dialog lifetime, damaged-save notice and Frame route tests passed");
    return 0;
}
