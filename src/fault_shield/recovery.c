/* Recover a failed editor load by driving the normal exit to the map browser.
 * The Frame catch alone can resume with an incomplete render world and fault
 * again. The main-thread Frame hook opens StartMenu, requests exit, and waits
 * for editor teardown. It also runs the damaged-save guard. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "engine_layout.h"
#include "../backend/engine_globals.h" /* locate DOOM's data globals by signing the code that computes them */
#include "../backend/apply_engine.h"
#include "../backend/engine_dialog.h"
#include "../backend/patch.h"
#include "fault_record.h"
#include "hook.h"
#include "recovery.h"
#include "shield_sigs.h"

extern uint8_t *g_doom_base;

const char SHIELD_RECOVERED_TEXT[] =
    "Snapmap+ caught a problem in the game and returned you to the menu. "
    "Unsaved changes to this map were lost.";

typedef void    (*setstate_t)(void *editor, int state);   /* idSnapEditorLocal::SetState (synchronous) */
typedef int64_t (*frame_t)(void *self);                   /* idCommonLocal::Frame */
static frame_t orig_frame = NULL;
typedef void (*clear_dialog_t)(void *shell, void *params);
static clear_dialog_t g_clear_dialog;

/* Cache resolved global addresses before the frame hook runs. Unresolved
 * globals disable their dependent operations; avoid scanning in the frame path. */
static uint8_t *g_editor_at      = NULL;   /* the inline idSnapEditorLocal object */
static uint8_t *g_load_state_at  = NULL;   /* load_state -- 3 == RUNNING */
static uint8_t *g_last_err_at    = NULL;   /* the engine's last formatted Error/FatalError text */
static uint8_t *g_shell_slot_at  = NULL;   /* .data slot holding idMenuShellLocal* */
static uint8_t *g_suppr_a_at     = NULL;   /* throw-gate suppressor A */

/* Resolve suppressor B through its instruction anchor; skip an unresolved
 * address. The caller guards this access with SEH. */
static void write_suppressor_b(void)
{
    uintptr_t b;
    if (g_doom_base == NULL) return;
    b = glb_resolve(g_doom_base, "throw_suppressor_b", NULL);
    if (!b) return;                     /* unlocatable on this build -- skip, never guess */
    if (*(volatile int32_t *)b != 0)
        *(volatile int32_t *)b = 0;
}

/* Frame's first 15 bytes on both build 13954591 executables: five pushes and
 * MOV EAX,0x11DC0, all position-independent. They are compared before the
 * detour, so a function another program changed is never patched. */
#define FRAME_STOLEN 15
static const uint8_t k_frame_prologue[FRAME_STOLEN] = {
    0x40, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0xC0, 0x1D, 0x01, 0x00
};
#define RECOVER_BUDGET_FRAMES 600   /* ~10s @ 60fps backstop */

static volatile LONG g_armed = 0;
/* Odd while a hooked Frame call runs: incremented on entry and on return. */
static volatile LONG g_frame_seq = 0;
static volatile LONG g_redirect_seq = -1;

void recovery_note_redirect(void)
{
    LONG seq = InterlockedCompareExchange(&g_frame_seq, 0, 0);
    InterlockedExchange(&g_redirect_seq, (seq & 1) ? seq : -1);
}

int recovery_redirected_this_frame(void)
{
    LONG seq = InterlockedCompareExchange(&g_frame_seq, 0, 0);
    if (seq == 0) return -1;
    return ((seq & 1) && seq == InterlockedCompareExchange(&g_redirect_seq, 0, 0)) ? 1 : 0;
}
static int g_state  = 0;            /* 1 = recover, 2 = wait-exit */
static int g_frames = 0;

/* No resolved singleton means no editor operations. */
static uint8_t *editor(void) { return g_editor_at; }
static int ed_state(void)
{
    uint8_t *ed = editor();
    return ed ? *(volatile int32_t *)(ed + ED_STATE) : 0;
}

static int in_editor(void)
{
    uint8_t *ed = editor();
    if (!ed) return 0;
    return *(void **)(ed + ED_MAP_PTR) != NULL
        && *(volatile int32_t *)(ed + ED_DEACT_REASON) == 0
        && *(volatile int32_t *)(ed + ED_STATE) != 0;
}

void recovery_arm(void)
{
    if (InterlockedExchange(&g_armed, 1) == 0) { g_state = 1; g_frames = 0; }
}

/* Drive exit only with a live editor, RUNNING load state, and a menu screen.
 * Play transitions tear down the screen; calling StartMenu Begin then would
 * dereference null. Waits are bounded by RECOVER_BUDGET_FRAMES. The exit
 * prompt is answered Yes: the editor's state is not trusted after the fault,
 * and the sentence the player was shown (SHIELD_RECOVERED_TEXT) says that
 * unsaved changes are lost. */
static void recovery_tick(void)
{
    if (!g_armed) return;
    if (++g_frames > RECOVER_BUDGET_FRAMES) {
        shield_fault f = { "load", -1, "recovery timed out (no editor exit)", 0, 0 };
        shield_emit(&f);
        InterlockedExchange(&g_armed, 0);
        return;
    }
    uint8_t *ed = editor();
    setstate_t SetState = (setstate_t)g_eng.setstate;
    if (!ed || !SetState) {
        shield_fault f = { "load", -1,
            "recovery: the editor singleton or SetState is unresolved -- editor-exit drive declined", 0, 0 };
        shield_emit(&f);
        InterlockedExchange(&g_armed, 0);
        return;
    }

    switch (g_state) {
    case 1: /* open StartMenu (synchronous), then arm the EXIT (pending + GDM result = Yes) */
        if (!in_editor()) {
            /* Outside the editor, there is no editor-exit drive to run. */
            shield_fault f = { "load", -1,
                "recovery: no live editor context (play/boot transition) -- editor-exit drive not needed", 0, 0 };
            shield_emit(&f);
            InterlockedExchange(&g_armed, 0);
            break;
        }
        /* Wait if a play/boot load is active or its state is unknown. */
        if (!g_load_state_at || *(volatile int32_t *)g_load_state_at != 3)
            break;   /* a play/boot load is in flight (or unknowable) -- wait (budget-bounded) */
        {
            void *ms = *(void **)(ed + ED_MENU_SCREEN);
            if (ms == NULL)
                break;   /* the StartMenu Begin writes through this object -- wait until it exists */
            if (ed_state() != EDITOR_STATE_STARTMENU)
                SetState(ed, EDITOR_STATE_STARTMENU);
            if (ed_state() == EDITOR_STATE_STARTMENU && *(volatile uint8_t *)(ed + ED_EXITING) == 0) {
                *(volatile int32_t *)(ed + ED_EXIT_PENDING) = 1;
                ms = *(void **)(ed + ED_MENU_SCREEN);   /* re-read: SetState can rebuild the screen */
                if (ms) {
                    void *gdm = *(void **)((uint8_t *)ms + MENUSCREEN_GDM);
                    if (gdm) *(volatile int32_t *)((uint8_t *)gdm + GDM_RESULT) = 0;
                }
                g_state = 2;
            }
        }
        break;
    case 2: /* the StartMenu Think calls ExitEditor in-frame; done once we're out of the editor */
        if (!in_editor()) {
            shield_fault f = { "load", -1, "recovered -> exited editor to browser", 0, 0 };
            shield_emit(&f);
            InterlockedExchange(&g_armed, 0);
        }
        break;
    }
}

/* Read the engine's last formatted error. Return 1 for a nonempty bounded
 * C string; unreadable or unavailable text returns 0. */
static int harvest_engine_msg(char *out, size_t n)
{
    if (!out || n == 0 || g_last_err_at == NULL) return 0;
    out[0] = '\0';
    __try {
        const char *p = (const char *)g_last_err_at;
        if (p[0] == '\0') return 0;
        lstrcpynA(out, p, (int)n);                  /* bounded copy; always NUL-terminates */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return 0;
    }
    return out[0] != '\0';
}

/* Public wrapper for the crash-record writer: same SEH-guarded read, callable from crash contexts. */
int shield_last_engine_msg(char *out, size_t n)
{
    return harvest_engine_msg(out, n);
}

/* Log engine error text. Called from the C++-throw path; reads are guarded
 * and logging is capped. */
static volatile LONG g_harvest_logged = 0;
void log_engine_error_text(void)
{
    char msgbuf[HARVEST_MSG_MAX];
    if (g_harvest_logged >= 16) return;
    if (harvest_engine_msg(msgbuf, sizeof msgbuf)) {
        InterlockedIncrement(&g_harvest_logged);
        shield_fault hf = { "load", -1, msgbuf, 0, 0 };
        shield_emit(&hf);
    }
}

/* Keep resolved throw suppressors clear so Error(6) can throw to Frame instead
 * of exiting. Read before writing; both are normally zero. */
static void keep_throw_gate_open(void)
{
    if (g_doom_base == NULL) return;
    __try {
        if (g_suppr_a_at && *(volatile int32_t *)g_suppr_a_at != 0)
            *(volatile int32_t *)g_suppr_a_at = 0;
        write_suppressor_b();
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* unreadable page -> skip */ }
}

/* The damaged-save guard.
 *
 * When a save file is damaged the engine offers to delete it. The guard
 * dismisses that offer through the shell's own ClearDialog (no button action
 * runs, so nothing is deleted) and then tells the player, in the engine's own
 * dialog, why the map did not open. */
static int is_corrupt_save_gdm(int gdm)
{
    /* CORRUPT_CONTINUE acknowledges a failed load; it does not resume loading. */
    return gdm == GDM_LOAD_DAMAGED_FILE || gdm == GDM_CORRUPT_CONTINUE
        || gdm == GDM_SNAPMAP_DETECTED_CORRUPT || gdm == GDM_SNAPMAP_REMOVED_CORRUPT;
}

const char *shield_damaged_save_text(int gdm)
{
    if (gdm == GDM_SNAPMAP_DETECTED_CORRUPT || gdm == GDM_SNAPMAP_REMOVED_CORRUPT)
        return "A saved map could not be read because its save file is damaged. "
               "Snapmap+ kept the file instead of letting the game delete it.";
    return "This map could not be opened because its save file is damaged. "
           "Snapmap+ kept the file instead of letting the game delete it.";
}

/* The reason waits until the dismissed dialog has left the queue. */
#define DAMAGED_NOTICE_WAIT_FRAMES 600
static const char *g_damaged_text;
static int         g_damaged_wait;
static int         g_damaged_ticket;

static void damaged_notice_tick(void)
{
    if (g_damaged_ticket) {
        if (sh_engine_dialog_poll(g_damaged_ticket) != SH_ENGINE_DIALOG_PENDING) {
            sh_engine_dialog_release(g_damaged_ticket);
            g_damaged_ticket = 0;
        }
        return;
    }
    if (!g_damaged_text) return;
    if (sh_engine_dialog_can_ask()) {
        g_damaged_ticket = sh_engine_dialog_ask(NOTICE_GDM_ID, NOTICE_BUTTONSET, g_damaged_text);
        if (g_damaged_ticket) { g_damaged_text = NULL; return; }
    }
    if (--g_damaged_wait <= 0) {
        shield_fault f = { "save", -1, "save-guard: the damaged-save notice could not be shown", 0, 0 };
        shield_emit(&f);
        g_damaged_text = NULL;
    }
}

static void save_guard_tick(void)
{
    /* An unresolved shell slot disables the guard. */
    if (g_doom_base == NULL || g_shell_slot_at == NULL || !g_clear_dialog) return;
    __try {
        uint8_t *S = *(uint8_t **)g_shell_slot_at;
        if (S == NULL) return;
        if (*(void **)(S + SHELL_SHELLMGR_OFF) == NULL) return;
        uint8_t *dlg = *(uint8_t **)(S + SHELL_DLGMGR_OFF);
        if (dlg == NULL) return;
        uint8_t *arr = *(uint8_t **)(dlg + DLGQ_ARR_OFF);
        if (arr == NULL) return;
        {
            int cnt = *(volatile int *)(dlg + DLGQ_COUNT_OFF);
            int i, dismissed = 0;
            int first_gdm = 0;
            int pending_ids[4], pending_count = 0;
            if (cnt < 0 || cnt > 4) return;
            for (i = 0; i < cnt; i++) {
                uint8_t *d = arr + (size_t)i * DLG_DESC_STRIDE;
                int gdm;
                if (*(volatile uint8_t *)(d + DESC_CLEARFLAG_OFF) != 0) continue;   /* already cleared */
                gdm = *(volatile int *)(d + DESC_GDMID_OFF);
                if (is_corrupt_save_gdm(gdm)) pending_ids[pending_count++] = gdm;
            }
            /* Snapshot IDs before calling the engine; no descriptor pointer is
             * retained across native teardown or possible menu reentry. */
            for (i = 0; i < pending_count; i++) {
                uint32_t params[0x100 / sizeof(uint32_t)] = {0};
                const char *source = "snapmap-plus save guard";
                params[0] = (uint32_t)pending_ids[i];
                memcpy(&params[0x26], &source, sizeof source);
                params[0x28] = __LINE__;
                g_clear_dialog(S, params);
                if (!dismissed) first_gdm = pending_ids[i];
                dismissed++;
            }
            if (dismissed) {
                char msg[128];
                _snprintf_s(msg, sizeof msg, _TRUNCATE,
                            "save-guard: dismissed the engine's delete offer (gdm=0x%x); save kept", first_gdm);
                {
                    shield_fault f = { "save", -1, msg, 0, 0 };
                    shield_emit(&f);
                }
                if (!g_damaged_ticket) {
                    g_damaged_text = shield_damaged_save_text(first_gdm);
                    g_damaged_wait = DAMAGED_NOTICE_WAIT_FRAMES;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* shell not mapped -> skip this frame */ }
}

static int64_t frame_detour(void *self)
{
    int64_t result;
    keep_throw_gate_open();      /* gate clear so engine Error(6)/downgraded-FatalError recovers */
    save_guard_tick();           /* dismiss the engine's offer to delete a damaged save */
    damaged_notice_tick();       /* then say why the map did not open */
    recovery_tick();             /* advance a pending editor-exit recovery */
    InterlockedIncrement(&g_frame_seq);
    result = orig_frame(self);
    InterlockedIncrement(&g_frame_seq);
    if (!InterlockedCompareExchange(&g_armed, 0, 0)) sh_apply_prefab_poll_play();
    return result;
}

/* Change FatalError's level immediate (MOV ECX,7 at +0x27; the byte at +0x28)
 * from 7 to 6, so its throw takes the recoverable engine path that Frame's
 * catch handles. The wrapper's whole signature, which ends in that MOV, was
 * checked at its recorded address; code_patch checks the byte again. Direct
 * dispatcher calls and failures outside Frame may still exit. */
#define FATALERROR_LEVEL_OFF 0x28
static sh_patch_handle g_fatal_patch;

static void patch_fatalerror_downgrade(void)
{
    static const uint8_t seven = 0x07, six = 0x06;
    uint8_t *fe = (uint8_t *)g_eng.fatalerror7;
    sh_patch_status st;
    if (fe == NULL) {
        shield_fault f = { "sig", -1, "FatalError downgrade declined: the wrapper is unresolved", 0, 0 };
        shield_emit(&f);
        return;
    }
    if (g_fatal_patch.live) return;
    st = code_patch(fe + FATALERROR_LEVEL_OFF, &seven, &six, 1, &g_fatal_patch);
    {
        shield_fault f = { st == B2_PATCH_OK ? "action" : "sig", -1,
            st == B2_PATCH_OK ? "FatalError(7) downgraded to recoverable Error(6)"
                              : "FatalError downgrade declined: the level byte could not be patched", 0, 0 };
        shield_emit(&f);
    }
}

int recovery_install(void)
{
    if (orig_frame) {
        if (hook_is_installed((void *)orig_frame)) return 1;
        if (!hook_unpatch((void *)orig_frame)) return 0;
        orig_frame = NULL;
    }
    /* Cache globals before installing the frame hook. */
    g_clear_dialog = (clear_dialog_t)g_eng.clear_dialog;
    if (g_doom_base) {
        g_editor_at     = (uint8_t *)glb_resolve(g_doom_base, "editor_singleton", NULL);
        g_load_state_at = (uint8_t *)glb_resolve(g_doom_base, "load_state", NULL);
        g_last_err_at   = (uint8_t *)glb_resolve(g_doom_base, "last_error_msg", NULL);
        g_shell_slot_at = (uint8_t *)glb_resolve(g_doom_base, "shell_ptr_slot", NULL);
        g_suppr_a_at    = (uint8_t *)glb_resolve(g_doom_base, "throw_suppressor_a", NULL);
    }
    if (!g_clear_dialog) {
        shield_fault f = { "sig", -1, "save guard unavailable: ClearDialogWrapper is unresolved", 0, 0 };
        shield_emit(&f);
    }

    void *target = (void *)g_eng.frame;
    int intact = 0;
    if (target) {
        __try { intact = memcmp(target, k_frame_prologue, FRAME_STOLEN) == 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { intact = 0; }
    }
    if (!intact) {
        shield_fault f = { "sig", -1, target
            ? "recovery not installed: idCommonLocal::Frame's first bytes are not the expected prologue"
            : "recovery not installed: idCommonLocal::Frame is unresolved", 0, 0 };
        shield_emit(&f);
        return 0;
    }
    orig_frame = (frame_t)hook_prepare(target, (void *)frame_detour, FRAME_STOLEN);
    if (orig_frame == NULL) return 0;
    if (hook_commit((void *)orig_frame) != B2_PATCH_OK) {
        if (hook_unpatch((void *)orig_frame)) orig_frame = NULL;
        return 0;
    }
    patch_fatalerror_downgrade();
    return 1;
}
