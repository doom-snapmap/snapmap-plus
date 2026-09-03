/* engine_dialog.h -- raise DOOM's own modal dialog, carrying our own text. */
#ifndef BACKEND_ENGINE_DIALOG_H
#define BACKEND_ENGINE_DIALOG_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Why this exists.
 *
 * A mod package install has to ask the player a question, and until now it asked
 * through a Win32 MessageBox: a grey OS box in front of a full-screen game,
 * invisible to anything that captures the game window, and jarring.
 *
 * The engine's own dialog was previously ruled out because `AddDialog` selects
 * body text by GDM id and only 8 of 221 ids carry any text at all, so a custom
 * message looked impossible without touching the Flash menu layer.
 *
 * That reading was incomplete. `idMenuManager_Dialog::ShowDialog` does:
 *
 *     BuildDefaultText(this, tmp, desc->gdmId);   // a "#str_..." key
 *     if (desc->text.length != 0)                 // desc + 0x20
 *         tmp = desc->text;                       // desc + 0x18 WINS
 *     swf->text = tmp;
 *
 * The descriptor carries a 256-byte idStr by value, and when it is non-empty the
 * engine prefers it over its own id-keyed lookup. Supplying that string is
 * therefore the engine's own supported override, not a trick played on it.
 *
 * The engine's own defaults are expressed as `#str_` localisation keys, because
 * that is how shipped text is authored. Whether the menu layer also passes a
 * LITERAL string through unchanged -- the usual idTech behaviour, where a
 * localisation lookup returns its input when it carries no `#str_` prefix -- is
 * the one thing here that cannot be read out of native code, because the
 * resolve happens in the Flash layer.
 *
 * So this surface accepts either, and the caller decides. A literal is far
 * better when it works: the message can name the package, its size and its
 * shard count, which a baked key never can. A `#str_` id published through
 * strids.c is the fallback, at the cost of being fixed wording.
 */

/* Resolve the dialog functions and install the ShowDialog detour. Safe to call
 * once at startup; returns 1 when the surface is usable, 0 when it refused (any
 * missing or ambiguous signature is terminal -- the caller falls back). */
int sh_engine_dialog_install(const sig_result *results, size_t count,
                             const uint8_t *module_base);

/* 1 once the menu manager has been observed, i.e. the engine has raised at least
 * one dialog of its own this session. Nothing can be raised before that. */
int sh_engine_dialog_ready(void);

/* Raise a modal carrying `text`, which is either literal text or a "#str_..."
 * id published through strids. `gdm_id` picks the engine's dialog personality
 * and `button_set` its buttons; both are free parameters -- the button set is
 * NOT a per-id constant, which is what lets any id ask a yes/no question.
 *
 * Returns a positive ticket, or 0 when the surface is not ready or busy. Only
 * one dialog is tracked at a time, which is all the install flow needs. */
int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text);

enum sh_engine_dialog_result {
    SH_ENGINE_DIALOG_PENDING = 0,   /* still on screen */
    SH_ENGINE_DIALOG_ACCEPTED,      /* the affirmative button, as the engine reported it */
    SH_ENGINE_DIALOG_DECLINED,      /* the negative button, or gone without one */
    SH_ENGINE_DIALOG_LOST           /* the ticket is not the one being tracked */
};

/* How the answer is obtained, since it is the one thing here that is not
 * obvious: the descriptor holds no result. A press invokes the pressed button's
 * callback, which hands that button's ACTION ID to the engine's dialog action
 * dispatcher, and for a gdm id with no case of its own AddDialog takes those
 * ids straight out of the params block this surface builds. So the two buttons
 * are named on the way in and the engine names one of them on the way out. Both
 * ids do nothing but close the dialog, so nothing about the engine's own
 * behaviour changes. Anything else that ends the dialog -- a teardown, an
 * external dismiss -- dispatches no action and is reported as declined. */

/* Poll a ticket. Must be called from the engine main thread: it reads the live
 * dialog queue. */
int sh_engine_dialog_poll(int ticket);

/* Print every descriptor currently queued: gdm id, button set and the two flag
 * bytes. The engine's own dialogs share this queue, so a prompt the GAME raises
 * shows which button-set value draws which layout. Neither flag byte carries an
 * answer -- no byte in the descriptor does -- so this is a queue inspector, not
 * a way to read a result. Main thread only. */
void sh_engine_dialog_dump(void (*printf_fn)(const char *fmt, ...));

/* Forget a ticket without waiting for it. */
void sh_engine_dialog_release(int ticket);

#ifdef SH_ENGINE_DIALOG_TESTING
void sh_engine_dialog_test_reset(void);
void sh_engine_dialog_test_bind(void *shell, void *add_wrapper, void *assign_cstr);
/* Feed a descriptor through the same injection the detour performs. */
int  sh_engine_dialog_test_inject(void *descriptor);
int  sh_engine_dialog_test_pending_id(void);
#endif

#endif /* BACKEND_ENGINE_DIALOG_H */
