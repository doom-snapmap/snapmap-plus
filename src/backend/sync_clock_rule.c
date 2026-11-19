/* sync_clock_rule.c -- the pure half of the sync clock (sync_clock.h): which animators of a sync run on the
 * player's clock, and the precision one re-anchor runs with. No engine, no global state. The namespace test of a
 * decl name is clip events' (clip_events_rule.c). */
#include <stddef.h>
#include "sync_clock.h"
#include "clip_events.h"

int sh_sc_sync_of_mod(const sh_sc_participant *p, int n)
{
    int i;
    for (i = 0; p && i < n; i++)
        if (p[i].target && sh_ce_decl_under_mods(p[i].def)) return 1;
    return 0;
}

int sh_sc_player_clock(const sh_sc_participant *p, int n, const void **out, int cap)
{
    int i, k, m = 0;
    if (!out || cap <= 0 || !sh_sc_sync_of_mod(p, n)) return 0;
    for (i = 0; i < n && m < cap; i++) {
        if (!p[i].animator) continue;
        for (k = 0; k < m && out[k] != p[i].animator; k++) {}
        if (k == m) out[m++] = p[i].animator;
    }
    return m;
}

int sh_sc_precision(int precise, int on_player_clock, int mode)
{
    if (!on_player_clock) return precise;
    return mode == SH_SC_MODE_PRECISE_ALL;
}
