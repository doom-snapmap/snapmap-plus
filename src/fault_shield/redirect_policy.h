/* redirect_policy.h -- when the shield may turn a fault into Error(6).
 *
 * The Class-B redirect throws an idException that only idCommonLocal::Frame's
 * catch handles (build 13954591: VK FUN_14159d930, catch funclet 0x141f5ecd7).
 * That catch is a full teardown: it syncs the renderer, then CancelMapChange
 * (common vtable +0x28) unloads the game and loads game/shell_map, all inside
 * the catch funclet and so outside Frame's try. The redirect is declined, and
 * the fault left to the fatal handlers that write the crash report, when:
 *   - the thread is not the engine main thread (no Frame catch at all);
 *   - the loading-screen thread is running (idLoadScreen thread byte +0x4b2) and
 *     the load-screen stop (load_screen_stop.h) is not armed: only
 *     idLoadScreen::LevelLoadCompleted stops that thread, the throw skips that call,
 *     and without the stop the thread keeps rendering the menu SWF while the main
 *     thread recovers (an engine render race on the shared GUI batch; a deadlock in
 *     a Play load). With the stop armed, CancelMapChange stops the thread first and
 *     the fault is recovered like any other;
 *   - a redirect already happened in this Frame call, so the fault is inside
 *     that catch's own recovery, where a second throw escapes to the game-loop
 *     catch that ends the process;
 *   - the stack does not unwind to Frame, so no engine catch can receive it.
 * An input of -1 (unknown) never declines: it keeps the behaviour the shield
 * had before the input existed. Pure; unit-tested in tests/redirect_policy_test.c. */
#ifndef SHIELD_REDIRECT_POLICY_H
#define SHIELD_REDIRECT_POLICY_H

typedef enum shield_redirect_verdict {
    SHIELD_REDIRECT_OK = 0,
    SHIELD_DECLINE_OFF_MAIN,
    SHIELD_DECLINE_LOAD_SCREEN,
    SHIELD_DECLINE_IN_RECOVERY,
    SHIELD_DECLINE_NO_FRAME
} shield_redirect_verdict;

/* Each input is 1, 0 or -1 (unknown); load_screen_stop_armed is 1 or 0. */
static __inline shield_redirect_verdict shield_redirect_policy(int on_main, int load_screen_running,
                                                               int load_screen_stop_armed,
                                                               int redirected_this_frame, int frame_on_stack)
{
    if (on_main == 0) return SHIELD_DECLINE_OFF_MAIN;
    if (load_screen_running == 1 && load_screen_stop_armed != 1) return SHIELD_DECLINE_LOAD_SCREEN;
    if (redirected_this_frame == 1) return SHIELD_DECLINE_IN_RECOVERY;
    if (frame_on_stack == 0) return SHIELD_DECLINE_NO_FRAME;
    return SHIELD_REDIRECT_OK;
}

static __inline const char *shield_redirect_decline_reason(shield_redirect_verdict v)
{
    switch (v) {
    case SHIELD_DECLINE_OFF_MAIN:
        return "the fault is off the engine main thread, which has no Frame catch";
    case SHIELD_DECLINE_LOAD_SCREEN:
        return "the loading-screen thread is running and the load-screen stop is not armed; recovering "
               "would leave it rendering beside the recovery";
    case SHIELD_DECLINE_IN_RECOVERY:
        return "an earlier redirect is still being recovered in this frame; a second throw would end the process";
    case SHIELD_DECLINE_NO_FRAME:
        return "the stack does not unwind to idCommonLocal::Frame, so no engine catch would receive Error(6)";
    default:
        return "";
    }
}

#endif /* SHIELD_REDIRECT_POLICY_H */
