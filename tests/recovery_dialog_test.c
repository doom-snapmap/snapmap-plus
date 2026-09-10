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
static unsigned char manager[0x1000], shell_manager[0x100], queue[4 * DLG_DESC_STRIDE];
static void *shell[8];
static void *shell_slot;
static sig_status resolve_status;
static void fake_clear(void *self, void *block);

const sig_entry BACKEND_ENGINE_SIGNATURES[] = {
    {"ClearDialogWrapper", "40 57", 0}, {NULL, NULL, 0}
};

int sh_host_is_pinned_rva_build(void) { return 0; }
sig_status sig_resolve_one(const uint8_t *base, const sig_entry *entry, sig_result *result)
{
    (void)base;
    result->name = entry->name;
    result->status = resolve_status;
    result->addr = (uintptr_t)fake_clear;
    result->rva = 0;
    return resolve_status;
}
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
    g_armed = g_notice_armed = 0;
    g_state = g_frames = 0;
    clears = emitted = widget_visible = callback_alive = 0;
    orig_calls = maintenance_calls = throw_frame = arm_in_frame = 0;
    orig_frame = fake_frame;
}

static void test_mixed_queue_retirement(void)
{
    reset();
    *(int *)(manager + DLGQ_COUNT_OFF) = 2;
    *(int *)queue = GDM_CORRUPT_CONTINUE;
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
    const int ids[] = {GDM_LOAD_DAMAGED_FILE, GDM_CORRUPT_CONTINUE,
                      GDM_SNAPMAP_DETECTED_CORRUPT, GDM_SNAPMAP_REMOVED_CORRUPT};
    for (int i = 0; i < 4; i++) *(int *)(queue + (size_t)i * DLG_DESC_STRIDE) = ids[i];
    g_clear_dialog = NULL;
    save_guard_tick();
    CHECK(!clears && !queue[DESC_CLEARFLAG_OFF]);
    g_clear_dialog = fake_clear;
    *(int *)(manager + DLGQ_COUNT_OFF) = 5;
    save_guard_tick();
    CHECK(!clears && !queue[DESC_CLEARFLAG_OFF]);
    *(int *)(manager + DLGQ_COUNT_OFF) = 4;
    save_guard_tick();
    CHECK(clears == 4);
    for (int i = 0; i < 4; i++) CHECK(queue[(size_t)i * DLG_DESC_STRIDE + DESC_CLEARFLAG_OFF] == 1);
    save_guard_tick();
    CHECK(clears == 4);

    reset();
    *(int *)(manager + DLGQ_COUNT_OFF) = 2;
    *(int *)queue = 0x29;
    *(int *)(queue + DLG_DESC_STRIDE) = GDM_CORRUPT_CONTINUE;
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

static void test_clean_clear_resolution(void)
{
    const sig_status refused[] = {SIG_NOT_FOUND, SIG_AMBIGUOUS, SIG_OK_HOOKED, SIG_BAD_MODULE};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        reset();
        g_eng.frame = (uintptr_t)fake_frame;
        resolve_status = refused[i];
        CHECK(recovery_install());
        CHECK(!g_clear_dialog);
    }
    reset();
    g_eng.frame = (uintptr_t)fake_frame;
    resolve_status = SIG_OK;
    CHECK(recovery_install());
    CHECK(g_clear_dialog == fake_clear);
}

int main(void)
{
    test_mixed_queue_retirement();
    test_guard_refusals_and_selection();
    test_frame_route();
    test_clean_clear_resolution();
    if (failed) return 1;
    puts("recovery dialog lifetime and Frame route tests passed");
    return 0;
}
