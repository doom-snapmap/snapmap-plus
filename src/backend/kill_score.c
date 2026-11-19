/* kill_score.c -- see kill_score.h. The engine side: the detour of the Player
 * Score switch. */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

#include "kill_score.h"
#include "modserve.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"
#include "../common/text.h"

typedef int (*score_fn)(void *rules, unsigned type);

static score_fn g_score;          /* the trampoline */
static void *g_tramp;

static void ks_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (line) backend_log(line);
    free(line);
}

static int score_detour(void *rules, unsigned type)
{
    return g_score(rules, sh_kill_score_type(type, sh_modserve_spawn_type_standin));
}

int sh_kill_score_install(const uint8_t *module_base)
{
    sig_result r;
    if (g_tramp) return 1;
    if (sig_resolve_one(module_base, &SH_KILL_SCORE_SIGNATURES[0], &r) != SIG_OK || !r.addr) {
        ks_log("MODS: kill score: unavailable: the Player Score switch was not found in this executable (%s); a ported "
               "demon's kill scores 0 in the map's Player Score", sig_status_reason(r.status));
        return 0;
    }
    g_tramp = hook_prepare_relative_jcc((void *)r.addr, (void *)score_detour, SH_KS_STOLEN, SH_KS_JCC_OFFSET);
    if (!g_tramp) {
        ks_log("MODS: kill score: unavailable: the Player Score switch could not be detoured (%s)", hook_last_failure());
        return 0;
    }
    g_score = (score_fn)g_tramp;
    if (hook_commit(g_tramp) != B2_PATCH_OK) {
        ks_log("MODS: kill score: unavailable: the Player Score switch's detour was refused");
        (void)hook_unpatch(g_tramp);
        g_tramp = NULL;
        g_score = NULL;
        return 0;
    }
    ks_log("MODS: kill score: a ported demon's kill scores in the map's Player Score as the stock demon it stands in "
           "for (switch detoured at %p)", (void *)r.addr);
    return 1;
}
