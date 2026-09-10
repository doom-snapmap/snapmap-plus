/* engine_dialog.h -- raise DOOM's own modal dialog, carrying our own text. */
#ifndef BACKEND_ENGINE_DIALOG_H
#define BACKEND_ENGINE_DIALOG_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Show custom text through the native shell dialog wrapper. The queued
 * descriptor has an idStr that overrides the GDM default text. Button action
 * IDs report the answer; descriptor flags do not. One ticket is tracked at a
 * time.
 */

/* Resolve dialog functions, including native cancellation, and hook
 * AddDialogWrapper and DialogAction.
 * Install once at startup; returns 1 when armed, 0 on refusal. Raising also
 * requires observing a native dialog to capture the shell.
 */
int sh_engine_dialog_install(const sig_result *results, size_t count,
                             const uint8_t *module_base);

/* 1 once the menu manager has been observed, i.e. the engine has raised at least
 * one dialog of its own this session. Nothing can be raised before that. */
int sh_engine_dialog_ready(void);

/* Main-thread read: 1 only for a readable queue with no pending descriptors.
 * Unknown/malformed state returns 0. The shell's visible byte can remain stale
 * after dismissal and is not consulted. No dialog hook needs to be installed. */
int sh_engine_dialog_queue_idle(const void *shell);

/* Main-thread admission check: the hooks are ready, the native queue is empty,
 * and its active widget is hidden. Cleared descriptors awaiting removal remain
 * busy. Keep a pending request and retry on later ticks when this returns 0. */
int sh_engine_dialog_can_ask(void);

/* Raise a modal with literal text or a published #str_ key. gdm_id selects a
 * supported native dialog and button_set selects its layout. Use an ID whose
 * native default path accepts the supplied button actions. Returns a positive
 * ticket, or 0 when unavailable or busy. Main thread only; also enforces the
 * native retirement boundary checked by sh_engine_dialog_can_ask. Failed text
 * assignment cancels the native dialog and never returns a consent ticket.
 */
int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text);

enum sh_engine_dialog_result {
    SH_ENGINE_DIALOG_PENDING = 0,   /* still on screen */
    SH_ENGINE_DIALOG_ACCEPTED,      /* the affirmative button, as the engine reported it */
    SH_ENGINE_DIALOG_DECLINED,      /* the negative button, or gone without one */
    SH_ENGINE_DIALOG_LOST           /* the ticket is not the one being tracked */
};

/* Buttons receive distinct native close-only action IDs, which the dispatcher
 * reports as accepted or declined. Descriptor flags contain no answer. A
 * dialog that disappears without either action is declined.
 */

/* Poll a ticket. Must be called from the engine main thread: it reads the live
 * dialog queue. */
int sh_engine_dialog_poll(int ticket);

/* Log queued descriptors, including native dialogs, to inspect GDM IDs,
 * button sets and flags. These flags do not contain the answer. Main thread
 * only.
 */
void sh_engine_dialog_dump(void (*printf_fn)(const char *fmt, ...));

/* Forget a ticket without waiting for it. */
void sh_engine_dialog_release(int ticket);

#ifdef SH_ENGINE_DIALOG_TESTING
void sh_engine_dialog_test_reset(void);
void sh_engine_dialog_test_bind(void *shell, void *add_wrapper, void *clear_wrapper,
                                void *assign_cstr);
/* Exercise the production descriptor-text assignment with a supplied
 * descriptor.
 */
int  sh_engine_dialog_test_inject(void *descriptor);
int  sh_engine_dialog_test_pending_id(void);
void sh_engine_dialog_test_action(int gdm_id, int action);
#endif

#endif /* BACKEND_ENGINE_DIALOG_H */
