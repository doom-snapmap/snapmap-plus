/* engine_dialog_test.c -- the text-injection gate and the one-at-a-time claim.
 *
 * What is testable offline is exactly the part that does not need DOOM: which
 * descriptors get our text and which are left alone, that both buttons are
 * named with the action ids the answer is later read from, and that a second
 * question cannot displace one already on screen. The one engine-side unknown
 * left -- whether the Flash layer renders a literal string -- is settled by
 * `sh_dialogtest` against a running game, because no amount of mocking can
 * answer it.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "engine_dialog.h"

#define ED_DESC_STRIDE      0x1B0u
#define ED_DESC_GDM_ID      0x00u
#define ED_DESC_TEXT        0x18u
#define ED_MGR_QUEUE_PTR    0x900u
#define ED_MGR_QUEUE_COUNT  0x908u

/* The params block indices the two button action ids are written to, and the
 * ids themselves. Duplicated from engine_dialog.c on purpose: a test that
 * imported the constants could not catch them being changed. */
#define ED_PARAM_ACTION_0   3
#define ED_PARAM_ACTION_1   4
#define ED_ACTION_ACCEPT    0x4A
#define ED_ACTION_DECLINE   0x4B

/* A stand-in shell and dialog manager. The shell holds the manager at +0x08 and
 * the manager holds the queue at +0x900 with its count at +0x908, exactly as the
 * engine lays them out -- because the code under test reads the queue back after
 * a raise, and a fake that skipped it would not exercise that at all. */
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

static int failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failed++;                                                               \
    }                                                                           \
} while (0)

void backend_log(const char *message) { (void)message; }

/* The detour installer is the one thing here that needs a real engine to mean
 * anything, and no test calls sh_engine_dialog_install, so it is stubbed rather
 * than dragging the inline-hook machinery into a pure-logic test. */
void *install_inline_hook(void *target, void *detour, size_t stolen)
{
    (void)target; (void)detour; (void)stolen;
    return NULL;
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
    g_last_target = idstr;
    strncpy_s(g_last_text, sizeof g_last_text, text ? text : "", _TRUNCATE);
}

static void reset(void)
{
    sh_engine_dialog_test_reset();
    queue_reset();
    sh_engine_dialog_test_bind(g_shell_obj, (void *)fake_add_dialog, (void *)fake_assign_cstr);
    g_add_calls = 0;
    g_assign_calls = 0;
    g_last_params = NULL;
    g_last_target = NULL;
    g_last_text[0] = '\0';
}

int main(void)
{
    unsigned char descriptor[ED_DESC_STRIDE];
    int ticket, other;

    /* A raised question puts its text into a descriptor carrying the SAME gdm
     * id, and into that descriptor's embedded string rather than its head. */
    reset();
    ticket = sh_engine_dialog_ask(0x6Du, 1u, "install this package?");
    CHECK(ticket > 0);
    CHECK(g_add_calls == 1);
    CHECK(sh_engine_dialog_test_pending_id() == 0x6D);

    /* Both buttons are named on the way in. Without these two ids AddDialog
     * builds both buttons with action 0, the dialog answers correctly on screen
     * and reports nothing at all -- so this is the whole answer path, asserted
     * at the only point it is visible offline. */
    CHECK(((const int *)g_last_params)[ED_PARAM_ACTION_0] == ED_ACTION_ACCEPT);
    CHECK(((const int *)g_last_params)[ED_PARAM_ACTION_1] == ED_ACTION_DECLINE);
    CHECK(ED_ACTION_ACCEPT != ED_ACTION_DECLINE);

    /* The raise itself writes the text into the queued descriptor. There is no
     * detour on the engine render path any more, so if it does not happen during
     * the raise it never happens. */
    CHECK(g_assign_calls == 1);
    CHECK(g_last_target == (void *)((unsigned char *)queue_entry(0) + ED_DESC_TEXT));
    CHECK(strcmp(g_last_text, "install this package?") == 0);

    /* Every OTHER dialog the game raises must pass through untouched. Rewriting
     * a stranger's text would corrupt unrelated engine UI, and the detour sees
     * all of them. */
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

    /* A claim whose dialog has left the queue must NOT wedge the surface. The
     * test binding reports an empty queue, so the second ask reclaims rather
     * than refusing -- otherwise one externally-dismissed dialog would disable
     * every prompt for the rest of the session. */
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

    /* A raise that queues no descriptor is a FAILED raise: handing back a ticket
     * for a dialog that does not exist would strand the caller waiting forever
     * for an answer that can never arrive. */
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
