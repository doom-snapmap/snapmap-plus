/* sync_clock.h -- every participant of a mod demon's sync runs on the player's animation clock.
 *
 * When the game's tick rate changes (the adaptive tick: nearly every frame of a SnapMap Play), the engine re-anchors
 * every running animation leaf so it keeps its frame. The re-anchor loop (VK 0x14038E920, GL 0x14038E1C0, called
 * from idGameLocal's frame) calls the re-anchor (VK 0x1415D1940, GL 0x1415D2D60) for the local player's animators
 * and for each entity of the game's animated list, with a precision: the player and every entity that is not an
 * idAI2 get `g_fixupForAdaptiveTick_PrecisionMode == 2`, an idAI2 gets `mode == 2 || mode == 1` (the cvar's default
 * is 1, "precise for AI"). The precise path zeroes a negative start fraction before the re-anchor, the other keeps
 * it, and its rounding moves the clip's start earlier at every change: the two clocks drift apart by about 8 %. In a
 * sync the demon (an AI) and the player, the sync entity and the helper bodies (not AI) then play out of step:
 * unseen in DOOM 2016's own kills of about 1.3 s, half a second by the end of a ported demon's six-second kill.
 *
 * The rule: while a sync whose TARGET participant is a mod's entity (its entityDef under mods/) is working, every
 * participant's animator is re-anchored with the precision the loop gives the player. The player and every non-AI
 * participant already have it, so the mod's demon is what changes: it re-anchors as the player does. Nothing of the
 * player is changed, and a sync with a stock target is never looked at past its target's name.
 *
 * Three parts:
 *   - The rule (sync_clock_rule.c, pure): a sync's participants -> the animators on the player's clock; the
 *     precision one re-anchor runs with.
 *   - The site (sync_clock_sigs.c): the re-anchor's signature on both renderers.
 *   - The engine side (sync_clock.c): the re-anchor detoured at the first Play that serves a mod. At the first
 *     re-anchor of a pass whose precision differs from the player's, each player's most recent sync entity
 *     (idPlayer.mostRecentSyncEntity) is read and, while it is working (isCurrentlyWorking), its active
 *     participants (activeParticipants: instance, data -> type) and their animators (the entity's vtable +0x948, the
 *     call the loop makes) are taken; every field is named through the game's own type tables
 *     (script/script_reflect.h) and placed at the Play gate. */
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

/* ------------------------------------------------------------- the rule (pure) */

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

/* -------------------------------------------------------------- engine site */

/* The re-anchor, build 13954591: `void f(animator, const int *now, int hz, int old_hz, bool precise, bool is_ai)`.
 * Its prologue opens with `test rcx,rcx; jz <end>`: the 15-byte window (test, jz rel32, mov rax,rsp, push rdi,
 * push r12) relocates the jz at +3 (hook_prepare_relative_jcc). */
#define SH_SC_STOLEN     15
#define SH_SC_JCC_OFFSET 3
extern const sig_entry SH_SYNC_CLOCK_SIGNATURES[];   /* NULL-terminated; [0] is AnimReanchor */

/* -------------------------------------------------------------- engine side */

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
