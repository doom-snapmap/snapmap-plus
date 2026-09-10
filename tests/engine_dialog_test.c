/* Offline descriptor text, button-action and single-claim checks. These
 * doubles cannot verify Flash rendering; that requires sh_dialogtest in game. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "engine_dialog.h"
#include "hook.h"

#define ED_DESC_STRIDE      0x1B0u
#define ED_DESC_GDM_ID      0x00u
#define ED_DESC_TEXT        0x18u
#define ED_MGR_QUEUE_PTR    0x900u
#define ED_MGR_QUEUE_COUNT  0x908u
#define ED_MGR_ACTIVE       0x8F0u

/* Keep button indices and action IDs independent of engine_dialog.c. */
#define ED_PARAM_ACTION_0   3
#define ED_PARAM_ACTION_1   4
#define ED_ACTION_ACCEPT    0x4A
#define ED_ACTION_DECLINE   0x4B

/* Model the shell manager at +0x08 and its queue/count at +0x900/+0x908
 * because the raise path reads the queued descriptor back. */
static unsigned char g_mgr[0x1000];
static void         *g_shell_obj[8];
static unsigned char g_queue[4 * ED_DESC_STRIDE];

static void queue_reset(void)
{
    memset(g_mgr, 0, sizeof g_mgr);
    memset(g_queue, 0, sizeof g_queue);
    memset(g_shell_obj, 0, sizeof g_shell_obj);
    *(void **)(g_mgr + ED_MGR_QUEUE_PTR) = g_queue;
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 0;
    g_shell_obj[1] = g_mgr;              /* shell + 0x08 -> manager */
}

static void *queue_entry(int index)
{
    return g_queue + (size_t)index * ED_DESC_STRIDE;
}

static int   g_add_calls;
static void *g_last_params;
static int   g_assign_calls;
static int   g_swallow_raise;
static void *g_last_target;
static char  g_last_text[512];
static int g_clear_calls, g_fail_assign, g_fail_clear;

static int failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failed++;                                                               \
    }                                                                           \
} while (0)

void backend_log(const char *message) { (void)message; }

typedef struct test_hook { void *target, *detour; int installed; } test_hook;
static test_hook g_hooks[2];
static int g_prepare_calls, g_commit_calls, g_fail_prepare, g_fail_commit, g_fail_unpatch;
static int g_action_calls;
static void fake_add_dialog(void *shell, void *params);

static void invoke_hook(test_hook *hook)
{
    if (hook->target == (void *)fake_add_dialog)
        ((void (*)(void *, void *))hook->detour)(g_shell_obj, NULL);
    else
        ((void (*)(void *, void *, int, void *, int))hook->detour)(NULL, NULL, 0, NULL, 0);
}

void *hook_prepare(void *target, void *detour, size_t stolen)
{
    (void)stolen;
    if (++g_prepare_calls == g_fail_prepare) return NULL;
    for (int i = 0; i < 2; i++) if (!g_hooks[i].target) {
        g_hooks[i].target = target; g_hooks[i].detour = detour;
        return target;
    }
    return NULL;
}

sh_patch_status hook_commit(void *tramp)
{
    for (int i = 0; i < 2; i++) if (g_hooks[i].target == tramp) {
        invoke_hook(&g_hooks[i]); /* The production original callback must already be published. */
        if (++g_commit_calls == g_fail_commit) return B2_PATCH_FAIL_ROLLBACK;
        g_hooks[i].installed = 1;
        return B2_PATCH_OK;
    }
    return B2_PATCH_REFUSED_BADARG;
}

int hook_is_installed(void *tramp)
{
    for (int i = 0; i < 2; i++) if (tramp && g_hooks[i].target == tramp) return g_hooks[i].installed;
    return 0;
}

int hook_unpatch(void *tramp)
{
    for (int i = 0; i < 2; i++) if (tramp && g_hooks[i].target == tramp) {
        g_hooks[i].installed = 0;
        if (g_fail_unpatch) return 0;
        memset(&g_hooks[i], 0, sizeof g_hooks[i]);
        return 1;
    }
    return 0;
}

static void fake_action(void *mgr, void *params, int action, void *parms, int flag)
{
    (void)mgr; (void)params; (void)action; (void)parms; (void)flag;
    g_action_calls++;
}

/* Stands in for the shell-level raise: appends a descriptor carrying the gdm id
 * from the parameter block, which is what the engine wrapper ends up doing. */
static void fake_add_dialog(void *shell, void *params)
{
    int count;
    (void)shell;
    g_add_calls++;
    g_last_params = params;
    if (g_swallow_raise) return;
    count = *(int *)(g_mgr + ED_MGR_QUEUE_COUNT);
    if (count < 4) {
        unsigned char *desc = (unsigned char *)queue_entry(count);
        *(int *)(desc + ED_DESC_GDM_ID) = *(const int *)params;
        *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = count + 1;
    }
}

static void fake_assign_cstr(void *idstr, const char *text)
{
    g_assign_calls++;
    if (g_fail_assign) {
        g_mgr[ED_MGR_ACTIVE] = 1;
        RaiseException(0xe0000043, 0, 0, NULL);
    }
    g_last_target = idstr;
    strncpy_s(g_last_text, sizeof g_last_text, text ? text : "", _TRUNCATE);
}

static void fake_clear_dialog(void *shell, void *params)
{
    int id = *(const int *)params;
    g_clear_calls++;
    CHECK(shell == g_shell_obj && id == *(int *)g_queue);
    CHECK(sh_engine_dialog_test_pending_id() == -1);
    /* Even an action reentered during a cancellation fault owns no consent. */
    sh_engine_dialog_test_action(id, ED_ACTION_ACCEPT);
    if (g_fail_clear) RaiseException(0xe0000044, 0, 0, NULL);
    g_queue[8] = 1;
    g_mgr[ED_MGR_ACTIVE] = 0;
}

static void reset(void)
{
    sh_engine_dialog_test_reset();
    queue_reset();
    sh_engine_dialog_test_bind(g_shell_obj, (void *)fake_add_dialog,
                               (void *)fake_clear_dialog, (void *)fake_assign_cstr);
    g_add_calls = 0;
    g_assign_calls = 0;
    g_clear_calls = g_fail_assign = g_fail_clear = 0;
    g_last_params = NULL;
    g_last_target = NULL;
    g_last_text[0] = '\0';
}

static void test_installation(void)
{
    const unsigned char *base = (const unsigned char *)GetModuleHandleW(NULL);
    sig_result results[] = {
        {"AddDialogWrapper", SIG_OK, (uintptr_t)fake_add_dialog, 0},
        {"DialogAction", SIG_OK, (uintptr_t)fake_action, 0},
        {"IdStrAssignCStr", SIG_OK, (uintptr_t)fake_assign_cstr, 0},
        {"ClearDialogWrapper", SIG_OK, (uintptr_t)fake_clear_dialog, 0}
    };
    for (int i = 0; i < 4; i++) results[i].rva = (uint32_t)(results[i].addr - (uintptr_t)base);
    sh_engine_dialog_test_reset();
    queue_reset();
    g_swallow_raise = 1;
    g_add_calls = g_action_calls = 0;

    /* Cancellation must resolve cleanly before any native hooks are prepared. */
    CHECK(!sh_engine_dialog_install(results, 3, base));
    CHECK(!g_prepare_calls && !sh_engine_dialog_ready());
    results[3].status = SIG_OK_HOOKED;
    CHECK(!sh_engine_dialog_install(results, 4, base));
    CHECK(!g_prepare_calls && !sh_engine_dialog_ready());
    results[3].status = SIG_OK;
    results[3].rva++;
    CHECK(!sh_engine_dialog_install(results, 4, base));
    CHECK(!g_prepare_calls && !sh_engine_dialog_ready());
    results[3].rva--;

    /* A second preparation failure must leave neither hook active nor owned. */
    g_fail_prepare = 2;
    CHECK(!sh_engine_dialog_install(results, 4, base));
    CHECK(!g_hooks[0].target && !g_hooks[1].target && !g_commit_calls);
    CHECK(!sh_engine_dialog_ready() && !g_add_calls && !g_action_calls);

    /* A failed second commit and failed cleanup preserve both original callbacks. */
    g_fail_prepare = 0; g_fail_commit = 2; g_fail_unpatch = 1;
    CHECK(!sh_engine_dialog_install(results, 4, base));
    CHECK(g_hooks[0].target && g_hooks[1].target);
    CHECK(g_add_calls == 1 && g_action_calls == 1 && !sh_engine_dialog_ready());
    invoke_hook(&g_hooks[0]); invoke_hook(&g_hooks[1]);
    CHECK(g_add_calls == 2 && g_action_calls == 2);
    {
        int prepared = g_prepare_calls;
        CHECK(!sh_engine_dialog_install(results, 4, base));
        CHECK(prepared == g_prepare_calls && !sh_engine_dialog_ready());
    }

    /* A later retry releases the retained records before installing anew. */
    g_fail_commit = g_fail_unpatch = 0;
    CHECK(sh_engine_dialog_install(results, 4, base));
    CHECK(g_add_calls == 3 && g_action_calls == 3 && sh_engine_dialog_ready());
    CHECK(sh_engine_dialog_install(NULL, 0, NULL));
    g_hooks[0].installed = g_hooks[1].installed = 0;
    CHECK(!sh_engine_dialog_install(NULL, 0, NULL));
    CHECK(!g_hooks[0].target && !g_hooks[1].target);
    g_swallow_raise = 0;
}

static void test_queue_idle(void)
{
    SYSTEM_INFO info;
    unsigned char *pages, *queue;
    DWORD old;
    queue_reset();
    CHECK(sh_engine_dialog_queue_idle(g_shell_obj));
    CHECK(!sh_engine_dialog_queue_idle(NULL));
    CHECK(!sh_engine_dialog_queue_idle((void *)1));
    g_shell_obj[1] = NULL;
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
    g_shell_obj[1] = (void *)1;
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
    g_shell_obj[1] = g_mgr;
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 2;
    g_queue[8] = 1;
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
    g_queue[ED_DESC_STRIDE + 8] = 1;
    CHECK(sh_engine_dialog_queue_idle(g_shell_obj));
    g_queue[ED_DESC_STRIDE + 8] = 2;
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = -1;
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 65;
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 0;
    *(void **)(g_mgr + ED_MGR_QUEUE_PTR) = NULL;
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
    *(void **)(g_mgr + ED_MGR_QUEUE_PTR) = (void *)1;
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 2;
    *(void **)(g_mgr + ED_MGR_QUEUE_PTR) = (void *)(UINTPTR_MAX - 4);
    CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));

    /* A readable first descriptor must not hide an inaccessible later entry. */
    GetSystemInfo(&info);
    pages = VirtualAlloc(NULL, (size_t)info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(pages != NULL);
    if (pages) {
        queue = pages + info.dwPageSize - ED_DESC_STRIDE;
        queue[8] = 1;
        CHECK(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
        *(void **)(g_mgr + ED_MGR_QUEUE_PTR) = queue;
        *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 1;
        CHECK(sh_engine_dialog_queue_idle(g_shell_obj));
        *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 2;
        CHECK(!sh_engine_dialog_queue_idle(g_shell_obj));
        VirtualFree(pages, 0, MEM_RELEASE);
    }
    queue_reset();
}

static void test_native_retirement(void)
{
    reset();
    CHECK(sh_engine_dialog_can_ask());
    /* A native corrupt dialog owns the widget and its callback, even though
     * the custom surface has no ticket of its own. */
    *(int *)g_queue = 0x34;
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 1;
    g_mgr[ED_MGR_ACTIVE] = 1;
    CHECK(!sh_engine_dialog_can_ask());
    CHECK(!sh_engine_dialog_ask(0x29, 6, "Install package?"));
    CHECK(!g_add_calls && !g_assign_calls);
    CHECK(sh_engine_dialog_test_pending_id() == -1);

    /* A clear flag alone permits logical idleness but not modal admission. */
    g_queue[8] = 1;
    CHECK(sh_engine_dialog_queue_idle(g_shell_obj));
    CHECK(!sh_engine_dialog_can_ask());
    g_mgr[ED_MGR_ACTIVE] = 0;
    CHECK(!sh_engine_dialog_can_ask());
    CHECK(!sh_engine_dialog_ask(0x29, 6, "Install package?"));
    CHECK(!g_add_calls && sh_engine_dialog_test_pending_id() == -1);

    /* Retirement must complete and the widget must be inactive. */
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 0;
    g_mgr[ED_MGR_ACTIVE] = 1;
    CHECK(!sh_engine_dialog_can_ask());
    g_mgr[ED_MGR_ACTIVE] = 2;
    CHECK(!sh_engine_dialog_can_ask());
    g_mgr[ED_MGR_ACTIVE] = 0;
    CHECK(sh_engine_dialog_can_ask());
    CHECK(sh_engine_dialog_ask(0x29, 6, "Install package?") > 0);
    CHECK(g_add_calls == 1 && g_assign_calls == 1);

    reset();
    g_shell_obj[1] = (void *)1;
    CHECK(!sh_engine_dialog_can_ask());
    CHECK(!sh_engine_dialog_ask(0x29, 6, "Install package?"));
    CHECK(!g_add_calls && sh_engine_dialog_test_pending_id() == -1);
}

static void test_custom_text_failure(void)
{
    reset();
    sh_engine_dialog_test_bind(g_shell_obj, (void *)fake_add_dialog, NULL,
                               (void *)fake_assign_cstr);
    CHECK(!sh_engine_dialog_can_ask());
    CHECK(!sh_engine_dialog_ask(0x29, 6, "Install package?"));
    CHECK(!g_add_calls && !g_assign_calls && !g_clear_calls);

    for (int cancel_faults = 0; cancel_faults < 2; cancel_faults++) {
        int previous, next;
        reset();
        previous = sh_engine_dialog_ask(0x29, 6, "Earlier question");
        CHECK(previous > 0);
        queue_reset(); /* The earlier descriptor has fully retired. */
        g_fail_assign = 1;
        g_fail_clear = cancel_faults;
        CHECK(!sh_engine_dialog_ask(0x29, 6, "Install package?"));
        CHECK(g_add_calls == 2 && g_assign_calls == 2 && g_clear_calls == 1);
        CHECK(sh_engine_dialog_test_pending_id() == -1);
        CHECK(sh_engine_dialog_poll(previous) == SH_ENGINE_DIALOG_LOST);
        CHECK(g_queue[8] == !cancel_faults);
        CHECK(g_mgr[ED_MGR_ACTIVE] == cancel_faults);

        /* Cancellation and later default-wording actions cannot become consent. */
        sh_engine_dialog_test_action(0x29, ED_ACTION_ACCEPT);
        CHECK(sh_engine_dialog_poll(previous) == SH_ENGINE_DIALOG_LOST);
        CHECK(!sh_engine_dialog_can_ask());
        g_fail_assign = g_fail_clear = 0;
        CHECK(!sh_engine_dialog_ask(0x29, 6, "Retry too early"));
        CHECK(g_add_calls == 2);

        queue_reset();
        next = sh_engine_dialog_ask(0x29, 6, "Install after retirement?");
        CHECK(next > previous && g_add_calls == 3 && g_clear_calls == 1);
        sh_engine_dialog_test_action(0x29, ED_ACTION_ACCEPT);
        CHECK(sh_engine_dialog_poll(next) == SH_ENGINE_DIALOG_ACCEPTED);
    }
}

int main(void)
{
    unsigned char descriptor[ED_DESC_STRIDE];
    int ticket;
    test_installation();
    test_queue_idle();
    test_native_retirement();
    test_custom_text_failure();

    /* A raised question puts its text into a descriptor carrying the SAME gdm
     * id, and into that descriptor's embedded string rather than its head. */
    reset();
    ticket = sh_engine_dialog_ask(0x6Du, 1u, "install this package?");
    CHECK(ticket > 0);
    CHECK(g_add_calls == 1);
    CHECK(sh_engine_dialog_test_pending_id() == 0x6D);

    /* Both buttons need distinct action IDs so answers can be read back. */
    CHECK(((const int *)g_last_params)[ED_PARAM_ACTION_0] == ED_ACTION_ACCEPT);
    CHECK(((const int *)g_last_params)[ED_PARAM_ACTION_1] == ED_ACTION_DECLINE);
    CHECK(ED_ACTION_ACCEPT != ED_ACTION_DECLINE);

    /* Raise must populate the queued descriptor before rendering. */
    CHECK(g_assign_calls == 1);
    CHECK(g_last_target == (void *)((unsigned char *)queue_entry(0) + ED_DESC_TEXT));
    CHECK(strcmp(g_last_text, "install this package?") == 0);

    /* Only the claimed dialog may receive replacement text. */
    reset();
    CHECK(sh_engine_dialog_ask(0x6Du, 1u, "ours") > 0);
    g_assign_calls = 0;
    memset(descriptor, 0, sizeof descriptor);
    *(int *)(descriptor + ED_DESC_GDM_ID) = 0x6C;      /* someone else's dialog */
    CHECK(sh_engine_dialog_test_inject(descriptor) == 0);
    CHECK(g_assign_calls == 0);

    /* With nothing pending, no descriptor is ours. */
    reset();
    memset(descriptor, 0, sizeof descriptor);
    *(int *)(descriptor + ED_DESC_GDM_ID) = 0x6D;
    CHECK(sh_engine_dialog_test_inject(descriptor) == 0);
    CHECK(g_assign_calls == 0);
    CHECK(sh_engine_dialog_test_pending_id() == -1);

    /* One at a time: a second question while one is up is refused rather than
     * silently replacing the text of the dialog the player is looking at. */
    reset();
    ticket = sh_engine_dialog_ask(0x6Du, 1u, "first");
    CHECK(ticket > 0);
    CHECK(strcmp(g_last_text, "first") == 0);
    /* A second question while the first is still queued is refused, not allowed
     * to rewrite the text of the dialog the player is looking at. */
    CHECK(sh_engine_dialog_ask(0x6Du, 1u, "second") == 0);
    CHECK(g_add_calls == 1);

    /* Reclaim a dismissed dialog so its stale claim cannot block later prompts. */
    reset();
    CHECK(sh_engine_dialog_ask(0x6Du, 1u, "stranded") > 0);
    CHECK(sh_engine_dialog_test_pending_id() == 0x6D);
    *(int *)(g_mgr + ED_MGR_QUEUE_COUNT) = 0;          /* cleared behind our back */
    CHECK(sh_engine_dialog_ask(0x6Du, 1u, "after the stranded one") > 0);
    CHECK(g_add_calls == 2);

    reset();
    /* Releasing clears the claim so the next question can be asked. */
    sh_engine_dialog_release(ticket);
    CHECK(sh_engine_dialog_test_pending_id() == -1);
    CHECK(sh_engine_dialog_ask(0x6Du, 1u, "third") > 0);

    /* Text longer than the descriptor's inline string is refused outright: the
     * engine would copy 256 bytes by value and truncate mid-message. */
    reset();
    {
        char oversized[600];
        memset(oversized, 'x', sizeof oversized - 1);
        oversized[sizeof oversized - 1] = '\0';
        CHECK(sh_engine_dialog_ask(0x6Du, 1u, oversized) == 0);
        CHECK(g_add_calls == 0);
    }

    /* A raise that queues no descriptor must fail instead of returning a ticket. */
    reset();
    g_swallow_raise = 1;
    CHECK(sh_engine_dialog_ask(0x6Du, 1u, "swallowed") == 0);
    CHECK(sh_engine_dialog_test_pending_id() == -1);
    g_swallow_raise = 0;

    /* Empty text is not a question. */
    reset();
    CHECK(sh_engine_dialog_ask(0x6Du, 1u, "") == 0);
    CHECK(sh_engine_dialog_ask(0x6Du, 1u, NULL) == 0);
    CHECK(g_add_calls == 0);

    if (failed) {
        fprintf(stderr, "%d engine-dialog test(s) failed\n", failed);
        return 1;
    }
    puts("engine dialog tests passed");
    return 0;
}
