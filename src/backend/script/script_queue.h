/* script_queue.h -- the per-mod event queues of mod scripts.
 *
 * Every engine hook that has something for a script (damage, death, an
 * animation notify, a Script node input) puts one fixed-size record in the
 * queue of the mod that owns the entity or node, from whatever thread the hook
 * runs on: the game's main thread, an AI prethink job or an animWeb job. The
 * queues are drained only by the script tick (script_hooks.h), on the main
 * thread, so no Lua code ever runs inside an engine hook and nothing runs
 * re-entrantly. A record carries values, never pointers: entities travel as
 * handles (script_handles.h) and strings are copied whole, of any length.
 *
 * A queue grows with what arrives in a frame and has no count of its own;
 * one per scripted mod, and the queues grow with the map's mods. Only a record
 * the process has no memory for is dropped and counted; the tick reports that
 * once per map for the mod in one plain line (sh_script_queue_take_dropped).
 * Enqueue takes a short spin lock per mod (allocating only when its queue
 * grows), so it is safe on any thread. Pure C, no engine calls;
 * tests/script_hooks_test.c covers it. */
#ifndef SH_SCRIPT_QUEUE_H
#define SH_SCRIPT_QUEUE_H
#include <stdint.h>
#include "script_handles.h"

#ifdef __cplusplus
extern "C" {
#endif


/* What happened. Spawn and removal are not queued: the handle table finds
 * them each tick (script_handles.h). */
typedef enum sh_script_event_kind {
    SH_SCRIPT_EV_NONE = 0,
    SH_SCRIPT_EV_DAMAGE,    /* self took `amount`; other = attacker, third = inflictor; text = damage decl, text2 = location,
                               vec = direction (SH_SCRIPT_EVF_VEC) */
    SH_SCRIPT_EV_DEATH,     /* self died; other = attacker (0 when none), third = inflictor; text = damage decl */
    SH_SCRIPT_EV_ANIM,      /* self's animation reached `ae_notify "<text>"` */
    SH_SCRIPT_EV_INPUT,     /* Script node self was activated by other; text = the node's name */
    SH_SCRIPT_EV_PAIN,      /* self's pain reaction started; other = attacker of its last hit, text = the reaction
                               (PAIN_FALTER ...), text2 = that hit's damage decl */
    SH_SCRIPT_EV_PROJECTILE,/* self launched projectile other; text = the projectile decl, vec = origin then
                               direction (SH_SCRIPT_EVF_VEC) */
    SH_SCRIPT_EV_KINDS
} sh_script_event_kind;

/* sh_script_event.flags */
#define SH_SCRIPT_EVF_VEC 0x0001u   /* vec holds a position (and, for a projectile, a direction) */

typedef struct sh_script_event {
    uint32_t seq;                       /* order of arrival in this mod's queue, from 1 per map */
    uint16_t kind;                      /* sh_script_event_kind */
    uint16_t flags;                     /* hook-defined, 0 by default */
    sh_script_handle self, other, third;
    float amount;
    float vec[6];                       /* damage: direction; projectile: origin, direction */
    /* The two strings. A hook pushes its own (any length; NULL is ""); the
     * queue keeps a copy, and a drained record owns its copy until
     * sh_script_event_release. Never NULL in a drained record. */
    const char *text;
    const char *text2;
} sh_script_event;

/* Free a drained record's strings (text and text2 are then NULL). */
void sh_script_event_release(sh_script_event *ev);

/* Open queue `mod` (from 0; made when new) for a new map: empty, counters
 * reset, accepting records. Returns 1, or 0 when out of memory (the slot then
 * refuses records). Tick thread. */
int  sh_script_queue_open(int mod);

/* Close queue `mod`: records still queued are discarded and new ones refused.
 * Tick thread. */
void sh_script_queue_close(int mod);

/* Close every queue (map end). */
void sh_script_queue_close_all(void);

/* Put a copy of *ev in mod's queue; ev->seq is assigned here. Any thread.
 * Returns 1 queued, 0 dropped (no memory to grow: counted), -1 refused (no
 * such open queue: a hook raced the map end, nothing to report). */
int  sh_script_queue_push(int mod, const sh_script_event *ev);

/* Convenience for hooks: fill and push one record. Strings may be NULL; they
 * are copied whole. */
int  sh_script_queue_post(int mod, sh_script_event_kind kind, sh_script_handle self, sh_script_handle other,
                          sh_script_handle third, float amount, const char *text, const char *text2);

/* Move up to `cap` records, oldest first, into out[]; each owns its strings
 * until sh_script_event_release. Tick thread only (the
 * thread that last called sh_script_queue_set_drain_thread); any other thread
 * gets -1 and nothing moves. Returns the number moved. Records pushed while a
 * handler runs stay queued for the next drain call (the runtime's passes). */
int  sh_script_queue_drain(int mod, sh_script_event *out, int cap);

/* Records waiting in mod's queue (any thread; a snapshot). */
int  sh_script_queue_pending(int mod);

/* The records dropped since the last call, for the tick's one line per map.
 * Returns the count and clears it; *first_time is set to 1 only the first
 * time a nonzero count is taken in this map. */
long sh_script_queue_take_dropped(int mod, int *first_time);

/* The thread allowed to drain: set by the tick before it calls the runtime. */
void sh_script_queue_set_drain_thread(uint32_t thread_id);

#ifdef __cplusplus
}
#endif
#endif
