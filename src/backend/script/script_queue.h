/* script_queue.h -- the per-mod event queues of mod scripts: hooks post value-only records from any thread, and
 * only the script tick (script_hooks.h) drains them on the main thread, so no Lua runs inside a hook. A record is
 * dropped and counted (sh_script_queue_take_dropped) only when memory fails. Pure C. */
#ifndef SH_SCRIPT_QUEUE_H
#define SH_SCRIPT_QUEUE_H
#include <stdint.h>
#include "script_handles.h"

#ifdef __cplusplus
extern "C" {
#endif


/* What happened. Spawn and removal are not queued; the handle table finds them each tick (script_handles.h). */
typedef enum sh_script_event_kind {
    SH_SCRIPT_EV_NONE = 0,
    SH_SCRIPT_EV_DAMAGE,    /* self took `amount`; other attacker, third inflictor; text decl, text2 location, vec dir */
    SH_SCRIPT_EV_DEATH,     /* self died; other = attacker (0 when none), third = inflictor; text = damage decl */
    SH_SCRIPT_EV_ANIM,      /* self's animation reached `ae_notify "<text>"` */
    SH_SCRIPT_EV_INPUT,     /* Script node self was activated by other; text = the node's name */
    SH_SCRIPT_EV_PAIN,      /* self's pain started; other = last attacker, text = reaction, text2 = hit decl */
    SH_SCRIPT_EV_PROJECTILE,/* self launched projectile other; text = decl, vec = origin then direction */
    SH_SCRIPT_EV_SIGNAL,    /* self fired the SnapMap event `text` (lower case, without SNAP_EVENT_); other = activator */
    SH_SCRIPT_EV_HIT,       /* self dealt damage to other; the remaining fields have the damage record's meaning */
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
    /* The two strings: a hook pushes its own (NULL is ""); the queue keeps a copy that a drained record owns until
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

/* Move up to `cap` records, oldest first, into out[]; each owns its strings until sh_script_event_release. Only the
 * thread that last called sh_script_queue_set_drain_thread may drain; any other thread gets -1 and nothing moves.
 * Returns the number moved; records pushed while a handler runs stay queued for the next drain call. */
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
