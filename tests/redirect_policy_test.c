/* redirect_policy_test.c -- when the fault shield may redirect a fault into
 * Error(6) (src/fault_shield/redirect_policy.h). Pure; no game needed. */
#include <stdio.h>
#include <string.h>
#include "../src/fault_shield/redirect_policy.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

int main(void)
{
    int a, b, c, d;
    /* An ordinary editor-frame fault: main thread, no loading screen, first
     * redirect of the frame, Frame on the stack. Redirected as before. */
    CHECK(shield_redirect_policy(1, 0, 0, 1) == SHIELD_REDIRECT_OK);
    /* Unknown inputs never decline: the behaviour before each input existed. */
    CHECK(shield_redirect_policy(-1, -1, -1, -1) == SHIELD_REDIRECT_OK);
    CHECK(shield_redirect_policy(1, -1, 0, 1) == SHIELD_REDIRECT_OK);
    CHECK(shield_redirect_policy(1, 0, -1, 1) == SHIELD_REDIRECT_OK);
    CHECK(shield_redirect_policy(1, 0, 0, -1) == SHIELD_REDIRECT_OK);
    /* issue 132: the shell-map load faulted in idMenuShellLocal::SetupModelList
     * with the loading-screen thread running. */
    CHECK(shield_redirect_policy(1, 1, 0, 1) == SHIELD_DECLINE_LOAD_SCREEN);
    /* the loading-screen thread's own SWF flush fault is off the main thread */
    CHECK(shield_redirect_policy(0, 1, 0, 0) == SHIELD_DECLINE_OFF_MAIN);
    /* a second fault inside the first redirect's catch recovery */
    CHECK(shield_redirect_policy(1, 0, 1, 1) == SHIELD_DECLINE_IN_RECOVERY);
    /* a main-thread fault outside Frame (before it, in the Frame detour's own work) */
    CHECK(shield_redirect_policy(1, 0, 0, 0) == SHIELD_DECLINE_NO_FRAME);
    /* precedence: off-main, then loading screen, then recovery, then no Frame */
    CHECK(shield_redirect_policy(0, 1, 1, 0) == SHIELD_DECLINE_OFF_MAIN);
    CHECK(shield_redirect_policy(1, 1, 1, 0) == SHIELD_DECLINE_LOAD_SCREEN);
    CHECK(shield_redirect_policy(1, 0, 1, 0) == SHIELD_DECLINE_IN_RECOVERY);
    /* exhaustive: OK exactly when no input is a definite blocker */
    for (a = -1; a <= 1; a++) for (b = -1; b <= 1; b++) for (c = -1; c <= 1; c++) for (d = -1; d <= 1; d++) {
        int blocked = a == 0 || b == 1 || c == 1 || d == 0;
        shield_redirect_verdict v = shield_redirect_policy(a, b, c, d);
        CHECK((v == SHIELD_REDIRECT_OK) == !blocked);
        CHECK(v == SHIELD_REDIRECT_OK ? shield_redirect_decline_reason(v)[0] == 0
                                      : strlen(shield_redirect_decline_reason(v)) > 20);
    }
    CHECK(strstr(shield_redirect_decline_reason(SHIELD_DECLINE_LOAD_SCREEN), "loading-screen thread") != NULL);
    if (g_fail) { printf("redirect_policy_test: FAILED\n"); return 1; }
    printf("redirect_policy_test: all passed\n");
    return 0;
}
