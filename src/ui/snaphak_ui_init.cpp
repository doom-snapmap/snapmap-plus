/* snaphak_ui_init.cpp -- the snaphakui.dll FRONTEND entry (minimal bring-up).
 *
 * A clean-room, FAITHFUL port of the OG snaphakui.dll entry `snaphak_ui_init` @ RVA 0x129d0 (export
 * ord 10) -- the 3-object model -- plus the manual 30 Hz think-loop @ RVA 0x15c04. RE-confirmed this
 * session against the OG decompiles (see report). Zero OG bytes; our own C++ on Qt 5.9.
 *
 * THE 3-OBJECT MODEL (DIRECT, 0x129d0):
 *   - window : a QMainWindow (OG FUN_18000eeb8 ctor -> setupUi). an EMPTY shell (NO tabs).
 *   - WIN    : the controller object (OG `local_258` -- holds qFindChild'd widget ptrs + the WIN[0]
 *              flag-word + WIN[4]=interface). a minimal struct carrying {flagword, window, app,
 *              interface}; the widget ptrs are populated later.
 *   - iface  : the shared interface object (backend-owned), arrives as param_1[3]; cached at WIN[4].
 *
 * THE THINK-LOOP (DIRECT, 0x15c04) -- the MANUAL 30 Hz pump, NOT QApplication::exec():
 *   loop {
 *     _Mtx_lock(loop_mtx);
 *     ... drain the deferred-record RB-tree (the WIN[0] flag protocol) ...
 *     (*(WIN[4]->vtbl + 0x1a0))(WIN[4]);   // DRAIN the backend work-queue on THIS (UI) thread
 *     ... EntityMode key-poll (gated) ...
 *     ... if editor-ready -> FUN_180014e7c (the WIN[0] dispatch) ...
 *     QCoreApplication::processEvents(0);
 *     _Mtx_unlock(loop_mtx);
 *     Sleep(0x21);                          // ~33ms => ~30 Hz
 *   }                                        // NEVER returns
 *
 * The manual pump + the +0x1a0 drain are LOAD-BEARING (the deferred main-thread apply rides on them) --
 * the spec forbids QApplication::exec(). This ships the pump + the drain; the flag-word dispatch + the
 * key-poll + the tab population come later.
 */
#include <windows.h>
#include <cstdint>
#include <cstdio>

#include <QApplication>
#include <QMainWindow>
#include <QString>

#include "snaphak_iface.h"
#include "sh_controller.h"

/* ------------------------------------------------------------------ the controller (WIN) -----------
 * The OG WIN (`local_258`) holds the flag-word at WIN[0], the QApplication at WIN[1], the QMainWindow at
 * WIN[2], the interface at WIN[4], plus the qFindChild'd widget pointers (the Ui[] array) + the timeline
 * TL (WIN[0x3b]). the bring-up struct {flagword, app, window, iface} is promoted to sh_controller.h's
 * ShWinController (which adds the ui[] array + timeline_tl) so setupUi can cache every widget ptr. The
 * member ORDER mirrors the OG index map (WIN[0]/[1]/[2]/[4]). */

/* ------------------------------------------------------------------ the loop-mutex/state obj -------
 * OG: `DAT_180031858 = FUN_18000f554(operator_new(0x58))` -- a 0x58-byte object wrapping a CRT _Mtx +
 * the deferred-record RB-tree roots (DAT_180031800/808/818/820/830). The bring-up needs the MUTEX (the think-loop
 * locks it each frame) + the out-slot write (`*param_1[0] = this`). The deferred-record stores come later.
 * We model it as a CRITICAL_SECTION-backed object; the address written to the out-slot lets later slices
 * (the WIN[0] deferred apply) reach the same lock. */
struct ShLoopState {
    CRITICAL_SECTION mtx;       /* the per-frame loop mutex (OG _Mtx) */
    uint64_t         flags;     /* OG DAT_180031858[10] activate-window bit etc. */
    /* the deferred-record RB-tree roots (DAT_180031800/808/818/820/830) */
};

static ShLoopState *g_loop_state = nullptr;   /* OG DAT_180031858 */
static sh_iface    *g_iface      = nullptr;   /* OG DAT_1800314e8 (set in the think-loop from WIN[4]) */

/* The OG argv vector QApplication keeps a reference to (argc must outlive the QApplication). The backend
 * passes argc/argv in the arg block; we keep a local copy so the int* handed to QApplication is stable. */
static int          g_argc = 0;
static char       **g_argv = nullptr;

/* ------------------------------------------------------------------ the think-loop (0x15c04) -------
 * The manual 30 Hz pump. NEVER returns (OG: followed by int3). The load-bearing skeleton: lock,
 * drain the backend work-queue via vtbl+0x1a0, processEvents(0), unlock, Sleep(33ms). The deferred-record
 * drain + the WIN[0] flag dispatch + the EntityMode key-poll come later -- their slots are pinned in the
 * vtable + the WIN struct so adding them does not move anything. */
static void sh_think_loop(ShWinController *win)
{
    /* OG: DAT_1800314e8 = WIN[4]. */
    g_iface = win->iface;

    /* SnapStack's 20 subcommands are registered by the BACKEND (XINPUT1_3.dll, src/backend/snapstack.c via
     * ui_bridge.c) before any frontend loads -- ONE implementation + ONE store, shared by BOTH the Qt and
     * webview frontends. The Qt-only registrar (the OG FUN_180003c80 port that lived in src/ui/snapstack.cpp)
     * was RETIRED, so the Qt frontend no longer registers its own copy; it reaches the shared backend store
     * through the interface slots (e.g. the Entities-tab "Push to stack 0" -> +0x2A0 push_to_stack). */

    for (;;) {
        EnterCriticalSection(&g_loop_state->mtx);

        /* drain the deferred-record RB-tree here (DAT_180031800/830 walks). */

        /* +0x1a0 DRAIN the backend work-queue on THIS (UI/main) thread -- the load-bearing call. The
         * backend enqueues SnapStack {handler,args} here; no producers yet so this is a
         * cheap no-op, but it PROVES the matched-pair drain is wired + callable. Null-guarded (the
         * interface always exists in the handshake, but a defensive null-check keeps a partial build
         * from faulting). */
        if (win->iface && win->iface->vtbl && win->iface->vtbl->drain_work_queue)
            win->iface->vtbl->drain_work_queue(win->iface);

        /* The editor-ready DISPATCH + WINDOW gate (OG FUN_180014e7c gates both on the interface editor-ready
         * poll +0x88, bound to slot_editor_ready): the SnapHak Studio window OPENS when a map is loaded in the
         * editor and HIDES otherwise (the user-observed OG behavior); the per-frame flag-word dispatch only
         * runs while the editor is live. */
        bool ready = win->iface && win->iface->vtbl && win->iface->vtbl->editor_ready_poll
                     && win->iface->vtbl->editor_ready_poll(win->iface);
        QMainWindow *w = win->window;
        if (ready) {
            if (w && !w->isVisible()) w->show();
            sh_dispatch_flagword(win);
        } else {
            if (w && w->isVisible()) w->hide();
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents);   /* OG processEvents(0); NOT exec() */
        LeaveCriticalSection(&g_loop_state->mtx);
        Sleep(0x21);   /* ~33 ms => ~30 Hz (OG Sleep(0x21)) */
    }
    /* unreachable (OG int3 here) */
}

/* ------------------------------------------------------------------ snaphak_ui_init (0x129d0) ------
 * The export the backend's CreateThread targets. param_1 = the sh_ui_argblock the backend filled
 * (&DAT_18003e5e0): [0]=out-slot, [1]=argc, [2]=argv, [3]=interface. Builds the 3 objects, shows an
 * EMPTY window, then enters the think-loop (never returns).
 *
 * Declared extern "C" (undecorated) so the backend's GetProcAddress("snaphak_ui_init") resolves it +
 * the .def can export it by the OG name (ord 10). The thread-proc shape (one void* arg, returns) matches
 * the OG (CreateThread's LPTHREAD_START_ROUTINE; the OG never returns so the DWORD return is moot). */
extern "C" __declspec(dllexport) DWORD WINAPI snaphak_ui_init(LPVOID param_1)
{
    sh_ui_argblock *args = reinterpret_cast<sh_ui_argblock *>(param_1);

    /* OG step 1: sh_algo_init(&DAT_1800314f0) -- the optional perf accelerator. The clone backend already
     * builds snaphak_algo; the bring-up does NOT require it (it is a math accelerator, not load-bearing
     * for the window/loop). Omitted to keep the bring-up dependency-free; wired in a later slice if
     * the perf path is exercised. (Faithful: OG calls it but the window works without it.) */

    /* OG step 2: the loop mutex/state object (operator_new(0x58)). */
    g_loop_state = new ShLoopState();
    InitializeCriticalSection(&g_loop_state->mtx);
    g_loop_state->flags = 0;

    /* OG step 5 (done early so the out-slot is valid before the window builds): *param_1[0] = state obj. */
    if (args && args->out_slot)
        *reinterpret_cast<void **>(args->out_slot) = g_loop_state;

    /* OG step 3: QApplication(argc=param_1[1], argv=param_1[2]). QApplication keeps a reference to argc/
     * argv for its lifetime, so stash stable copies. The backend hands real argc/argv; if absent (a thin
     * arg block) fall back to a synthetic argv[0] so QApplication constructs cleanly. */
    static char  prog0[]   = "snaphak";
    static char *fallback[] = { prog0, nullptr };
    if (args && args->argc > 0 && args->argv) {
        g_argc = args->argc;
        g_argv = args->argv;
    } else {
        g_argc = 1;
        g_argv = fallback;
    }
    /* QApplication takes `int&` -- it may modify argc (consuming Qt args); pass our stable copy. */
    QApplication *app = new QApplication(g_argc, g_argv);

    /* OG step 4: the QMainWindow ctor (FUN_18000eeb8 -> setupUi @ 0xcb6c). This builds the FULL faithful
     * widget tree via sh_setupUi (the 6 tabs + the Camera-Origin groupbox + menubar/toolbar/statusbar);
     * 1482x944, title "SnapHak Studio", setCurrentIndex(1). The controller must precede setupUi (it
     * caches every widget ptr into win->ui[]). Zero-init so the ui[] array starts null. */
    QMainWindow *window = new QMainWindow();

    ShWinController *win = new ShWinController();   /* the WIN controller (OG local_258) */
    win->flagword    = 0;
    win->app         = app;
    win->window      = window;
    win->win3        = nullptr;
    win->iface       = args ? args->iface : nullptr;   /* WIN[4] = the shared backend interface */
    win->timeline_tl = nullptr;                        /* OG WIN[0x3b] -- built in C3 */
    win->displayed_id      = -1;                       /* OG *(WIN+0x54) -- nothing synced yet */
    win->pending_select_id = -1;                       /* OG WIN[0x50] -- no pending select */
    for (int i = 0; i < SH_UI_COUNT; ++i)
        win->ui[i] = nullptr;

    /* setupUi (FUN_18000cb6c): the faithful 6-tab widget tree + the WIN[0] stub handler wiring +
     * connectSlotsByName + setCurrentIndex(1). Caches every widget ptr into win->ui[]. */
    sh_setupUi(window, win);

    /* OG step 6: setWindowFlags(FUN_1800162d4(...)) then show(). The flag massage (FUN_1800162d4) tweaks
     * the title-bar/min-max flags; default-flagged window here (the flag massage is cosmetic; a later
     * pass can replicate the exact flag set if a parity diff demands it). The window is NOT shown here -- the
     * think-loop's editor-ready gate shows it on editor-entry + hides it otherwise (OG +0x88 behavior). */

    /* OG step 7: the wire-ctor FUN_180012bac(WIN, app, window, param_1[3]) cached the 3 objects + the
     * interface -- done above (the controller is built before setupUi so the widget ptrs land in it). */

    /* OG step 8: the think-loop FUN_180015c04(WIN) -- the manual 30 Hz pump. NEVER returns. */
    sh_think_loop(win);

    return 0;   /* unreachable */
}
