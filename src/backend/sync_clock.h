/* sync_clock.h -- runs every participant of a mod demon's sync on the player's animation clock. The adaptive-tick
 * re-anchor loop (VK 0x14038E920, GL 0x14038E1C0) calls the re-anchor (VK 0x1415D1940, GL 0x1415D2D60) with a
 * precision that differs between the player and an idAI2, so the demon drifts about 8 % out of step. While a sync
 * whose TARGET is a mod's entity (entityDef under mods/) is working, every participant is re-anchored with the
 * player's precision; the rule is in sync_clock_rule.c, the signature in sync_clock_sigs.c, the detour in sync_clock.c. */
#ifndef SNAPMAP_PLUS_SYNC_CLOCK_H
#define SNAPMAP_PLUS_SYNC_CLOCK_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Participants read from one sync at most (a ported kill has four: the player, the sync entity, the demon and one
 * helper body). */
#define SH_SC_PARTICIPANTS_MAX 16
/* The precision mode at which the loop re-anchors the player precisely (g_fixupForAdaptiveTick_PrecisionMode). */
#define SH_SC_MODE_PRECISE_ALL 2

typedef struct sh_sc_participant {
    const void *animator;  /* the participant's animator (NULL when it has none) */
    int target;            /* 1 for the sync's TARGET participant */
    const char *def;       /* the participant's entityDef name, or NULL */
} sh_sc_participant;

/* 1 when the sync of participants p[0..n-1] is a mod demon's: a TARGET participant's entityDef is under mods/. */
int sh_sc_sync_of_mod(const sh_sc_participant *p, int n);
/* The animators that run on the player's clock for that sync: every participant's (distinct, not NULL) when the
 * sync is a mod demon's, written to out[0..cap-1]; none for any other sync. Returns the count. */
int sh_sc_player_clock(const sh_sc_participant *p, int n, const void **out, int cap);
/* The precision one re-anchor runs with: the player's (mode == 2) for an animator on the player's clock, the one
 * the loop passed for any other. */
int sh_sc_precision(int precise, int on_player_clock, int mode);

/* The re-anchor, build 13954591: `void f(animator, const int *now, int hz, int old_hz, bool precise, bool is_ai)`.
 * Its prologue opens with `test rcx,rcx; jz <end>`: the 15-byte window (test, jz rel32, mov rax,rsp, push rdi,
 * push r12) relocates the jz at +3 (hook_prepare_relative_jcc). */
#define SH_SC_STOLEN     15
#define SH_SC_JCC_OFFSET 3
extern const sig_entry SH_SYNC_CLOCK_SIGNATURES[];   /* NULL-terminated; [0] is AnimReanchor */

/* Boot: resolve the re-anchor and prepare its detour; find the precision cvar. Commits nothing. 0 (with the feature
 * recorded off) when the re-anchor is not the one recorded in this executable. */
int  sh_sync_clock_install(const uint8_t *module_base);
/* The Play gate (modserve_engine.c, main thread, before the level load): the fields are placed through the type
 * tables, and the detour is committed the first time a Play serves a mod. */
void sh_sync_clock_play_begin(void);
/* The map-load gate and CancelMapChange (main thread): nothing is read until the next Play. */
void sh_sync_clock_map_end(void);
/* The console listing `sh_mods sync-clock`: the state, this Play's mod syncs and the re-anchors moved. */
typedef void (*sh_sc_line_fn)(void *context, const char *line);
void sh_sync_clock_command(sh_sc_line_fn line, void *context);

#ifdef __cplusplus
}
#endif
#endif /* SNAPMAP_PLUS_SYNC_CLOCK_H */
