/* modhud_scene.c -- see modhud_scene.h. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "modhud_scene.h"
#include "modserve.h"
#include "patch.h"
#include "backend_log.h"

/* idEventArg *Dispatch(idClass *this, idEventArg *ret, const idEventDef *ev, const idEventArg *args).
 * Microsoft x64; the function returns its second argument. */
typedef void *(*ms_dispatch_fn)(void *self, void *ret, const void *ev, const void *args);

#define MS_EV_NUMBER_OFF 0x34
/* push rbp; push rbx; push rsi; push rdi; push r14; push r15; lea rbp,[rsp-0x2f]: fourteen
 * whole, position-independent bytes (the SUB RSP after them is also independent). */
#define MS_DISPATCH_STOLEN 14

static ms_dispatch_fn g_tramp;
static volatile LONG g_hidden;
static volatile LONG g_hides, g_shows;
static volatile LONG g_logged_hide, g_logged_show;

static void ms_apply(int show)
{
    if (!sh_modserve_engine_set_cvar("g_showHud", show ? "1" : "0")) {
        backend_log(show ? "scene-hud: g_showHud could not be set back to 1"
                         : "scene-hud: g_showHud could not be set to 0");
        return;
    }
    InterlockedExchange(&g_hidden, show ? 0 : 1);
    if (show) {
        if (!InterlockedExchange(&g_logged_show, 1)) backend_log("scene-hud: shown again");
    } else {
        if (!InterlockedExchange(&g_logged_hide, 1)) backend_log("scene-hud: hidden by a scene");
    }
}

static void *ms_dispatch_detour(void *self, void *ret, const void *ev, const void *args)
{
    int number = -1;
    __try {
        if (ev) number = *(const int *)((const unsigned char *)ev + MS_EV_NUMBER_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { number = -1; }
    if (number == SH_MODHUD_SCENE_EV_HIDE || number == SH_MODHUD_SCENE_EV_SHOW) {
        int want;
        InterlockedIncrement(number == SH_MODHUD_SCENE_EV_HIDE ? &g_hides : &g_shows);
        want = sh_modhud_scene_decide(number, sh_modserve_engine_in_play());
        if (want >= 0) ms_apply(want);
    }
    return g_tramp ? g_tramp(self, ret, ev, args) : ret;
}

int sh_modhud_scene_install(const sig_result *results, size_t n)
{
    const sig_result *r = NULL;
    void *tramp;
    size_t i;
    for (i = 0; i < n && !r; i++)
        if (results[i].name && !strcmp(results[i].name, "EventDispatchBase")) r = &results[i];
    if (!r || r->status != SIG_OK) {
        backend_log("scene-hud SKIPPED -- scenes cannot hide the HUD on this build");
        return 0;
    }
    tramp = sh_prepare_detour_sig(r, (void *)ms_dispatch_detour, MS_DISPATCH_STOLEN);
    if (!tramp) {
        backend_log("scene-hud SKIPPED -- the event dispatcher prologue could not be relocated");
        return 0;
    }
    g_tramp = (ms_dispatch_fn)tramp;
    if (sh_commit_detour(tramp) != B2_PATCH_OK) {
        g_tramp = NULL;
        sh_uninstall_detour(tramp);
        backend_log("scene-hud SKIPPED -- the event dispatcher detour could not be committed");
        return 0;
    }
    backend_log("scene-hud installed: HUD follows a scene's hide and show events during Play");
    return 1;
}

void sh_modhud_scene_play_begin(void)
{
    InterlockedExchange(&g_logged_hide, 0);
    InterlockedExchange(&g_logged_show, 0);
    if (InterlockedCompareExchange(&g_hidden, 0, 0)) {
        backend_log("scene-hud: a scene left the HUD hidden; showing it for this Play");
        ms_apply(1);
    }
}

int sh_modhud_scene_restore(void)
{
    if (sh_modserve_engine_in_play() != 1 || !InterlockedCompareExchange(&g_hidden, 0, 0)) return 0;
    ms_apply(1);
    return !InterlockedCompareExchange(&g_hidden, 0, 0);
}

void sh_modhud_scene_stats(unsigned *hides, unsigned *shows, int *hidden)
{
    if (hides) *hides = (unsigned)InterlockedCompareExchange(&g_hides, 0, 0);
    if (shows) *shows = (unsigned)InterlockedCompareExchange(&g_shows, 0, 0);
    if (hidden) *hidden = (int)InterlockedCompareExchange(&g_hidden, 0, 0);
}
