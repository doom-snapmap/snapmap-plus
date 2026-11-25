/* engine_dialog.h -- raise DOOM's own modal dialog, carrying our own text. */
#ifndef BACKEND_ENGINE_DIALOG_H
#define BACKEND_ENGINE_DIALOG_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Shows custom text through the native shell dialog wrapper: the queued descriptor's idStr overrides the GDM default
 * text. Button action IDs report the answer, not descriptor flags. One ticket is tracked at a time. */

/* Resolves the dialog functions (including native cancellation) and the shell's global slot, and hooks DialogAction.
 * Install once at startup; returns 1 when armed, 0 on refusal. */
int sh_engine_dialog_install(const sig_result *results, size_t count,
                             const uint8_t *module_base);

/* 1 once installed and the engine's menu shell exists. */
int sh_engine_dialog_ready(void);

/* Main-thread read: 1 only for a readable queue with no pending descriptors; unknown or malformed state returns 0.
 * The shell's visible byte can stay stale after dismissal and is not consulted. Needs no dialog hook. */
int sh_engine_dialog_queue_idle(const void *shell);

/* Main-thread admission check: 1 when the hooks are ready, the native queue is empty and its active widget is hidden.
 * Cleared descriptors awaiting removal count as busy. */
int sh_engine_dialog_can_ask(void);

/* Raises a modal with literal text or a published #str_ key; gdm_id selects a supported native dialog (one whose default
 * path accepts the supplied button actions) and button_set its layout. Returns a positive ticket, or 0 when
 * unavailable, busy or the text assignment failed (which cancels the dialog). Main thread only. */
int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text);

enum sh_engine_dialog_result {
    SH_ENGINE_DIALOG_PENDING = 0,   /* still on screen */
    SH_ENGINE_DIALOG_ACCEPTED,      /* the affirmative button, as the engine reported it */
    SH_ENGINE_DIALOG_DECLINED,      /* the negative button, or gone without one */
    SH_ENGINE_DIALOG_LOST           /* the ticket is not the one being tracked */
};


/* Polls a ticket. Buttons carry distinct close-only action IDs that the dispatcher reports as accepted or
 * declined; a dialog that disappears without either action is declined. Engine main thread only. */
int sh_engine_dialog_poll(int ticket);

/* Logs the queued descriptors (GDM IDs, button sets, flags). Main thread only. */
void sh_engine_dialog_dump(void (*printf_fn)(const char *fmt, ...));

/* Main thread: forgets this ticket and clears its still-pending native question; polled answers and stale tickets
 * cannot clear another dialog. */
void sh_engine_dialog_release(int ticket);

#ifdef SH_ENGINE_DIALOG_TESTING
void sh_engine_dialog_test_reset(void);
void sh_engine_dialog_test_bind(void *shell, void *add_wrapper, void *clear_wrapper,
                                void *assign_cstr);
/* Runs the production descriptor-text assignment on a supplied descriptor. */
int  sh_engine_dialog_test_inject(void *descriptor);
int  sh_engine_dialog_test_pending_id(void);
void sh_engine_dialog_test_action(int gdm_id, int action);
#endif

#endif /* BACKEND_ENGINE_DIALOG_H */
