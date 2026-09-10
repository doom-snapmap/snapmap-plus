/* Recover a failed editor load by driving the normal exit to the map browser.
 * The Frame catch alone can resume with an incomplete render world and fault
 * again. The main-thread Frame hook opens StartMenu, requests exit, and waits
 * for editor teardown. It also services notices and save-dialog protection. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "engine_layout.h"
#include "../backend/host_image.h"     /* sh_host_is_pinned_rva_build -- gates every pinned-RVA backstop */
#include "../backend/engine_globals.h" /* locate DOOM's data globals by signing the code that computes them */
#include "fault_record.h"
#include "hook.h"
#include "recovery.h"
#include "shield_sigs.h"

extern uint8_t *g_doom_base;

typedef void    (*setstate_t)(void *editor, int state);   /* SetState 0x5298A0 (synchronous) */
typedef int64_t (*frame_t)(void *self);                   /* idCommonLocal::Frame 0x17ce360 */
static frame_t orig_frame = NULL;

/* Cache resolved global addresses before the frame hook runs. Unresolved
 * globals disable their dependent operations; avoid scanning in the frame path. */
static uint8_t *g_editor_at      = NULL;   /* the inline idSnapEditorLocal object */
static uint8_t *g_load_state_at  = NULL;   /* load_state -- 3 == RUNNING */
static uint8_t *g_last_err_at    = NULL;   /* the engine's last formatted Error/FatalError text */
static uint8_t *g_shell_slot_at  = NULL;   /* .data slot holding idMenuShellLocal* */
static uint8_t *g_suppr_a_at     = NULL;   /* throw-gate suppressor A */
/* Cache the immutable host-build check for the frame hook. */
static int      g_pinned_build   = 0;

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

/* 5 pushes + `mov eax,0x119c0` => boundary 0x17ce36f, all position-independent (disassembly-verified). */
#define FRAME_STOLEN 15
#define RECOVER_BUDGET_FRAMES 600   /* ~10s @ 60fps backstop */

static volatile LONG g_armed = 0;
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
 * dereference null. Waits are bounded by RECOVER_BUDGET_FRAMES. */
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
    /* Without both the editor and SetState, disarm this exit attempt. */
    setstate_t SetState = (setstate_t)(uintptr_t)(g_eng.setstate ? g_eng.setstate
                            : (g_pinned_build ? (uintptr_t)(g_doom_base + RVA_SETSTATE) : (uintptr_t)0));
    if (!ed || !SetState) {
        shield_fault f = { "load", -1,
            "recovery: the editor singleton or SetState is unresolved on this build -- editor-exit "
            "drive declined (no address is guessed)", 0, 0 };
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

/* Show one transient toast on the editor screen. Using a shell dialog here
 * would activate the browser. Destroy both temporary idStr values after use. */
typedef void (*idstr_ctor_t)(void *buf, const char *s);
typedef void (*idstr_dtor_t)(void *buf);
typedef void (*toast_show_t)(void *screen, void *title, void *text);

static volatile LONG g_notice_armed = 0;   /* 1 = generic toast (Class-A), 2 = harvest engine text (Class-B) */

void notice_request(void)     { InterlockedExchange(&g_notice_armed, 1); }
void notice_request_msg(void) { InterlockedExchange(&g_notice_armed, 2); }

/* Read the engine's last formatted error. Return 1 for a nonempty bounded
 * C string; unreadable or unavailable text leaves callers using a generic notice. */
static int harvest_engine_msg(char *out, size_t n)
{
    if (!out || n == 0 || g_last_err_at == NULL) return 0;
    out[0] = '\0';
    __try {
        const char *p = (const char *)g_last_err_at;
        if (p[0] == '\0') return 0;                 /* engine never stashed a message -> generic */
        lstrcpynA(out, p, (int)n);                  /* bounded copy; always NUL-terminates */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return 0;                                   /* unreadable page -> generic */
    }
    return out[0] != '\0';
}

/* Public wrapper for the crash-record writer: same SEH-guarded read, callable from crash contexts. */
int shield_last_engine_msg(char *out, size_t n)
{
    return harvest_engine_msg(out, n);
}

/* Log engine error text even when no editor screen exists for a toast.
 * Called from the C++-throw path; reads are guarded and logging is capped. */
static volatile LONG g_harvest_logged = 0;
void log_engine_error_text(void)
{
    char msgbuf[HARVEST_MSG_MAX];
    if (g_harvest_logged >= 16) return;                 /* rate-limit the record */
    if (harvest_engine_msg(msgbuf, sizeof msgbuf)) {
        InterlockedIncrement(&g_harvest_logged);
        shield_fault hf = { "load", -1, msgbuf, 0, 0 };
        shield_emit(&hf);                               /* the verbatim engine error, on the load-time path too */
    }
}

static void notice_tick(void)
{
    uint8_t      *ed = editor();
    uint8_t      *screen;
    idstr_ctor_t  mk;
    idstr_dtor_t  rm;
    toast_show_t  toast;
    const char   *text;
    LONG          mode;
    char          msgbuf[HARVEST_MSG_MAX];
    /* idStr OBJECTS are stack locals (the engine uses 48-byte locals; long tokens heap-alloc internally,
     * freed by the dtor). Align for safety. */
    __declspec(align(16)) unsigned char title_buf[IDSTR_BUF_SIZE];
    __declspec(align(16)) unsigned char text_buf[IDSTR_BUF_SIZE];

    mode = g_notice_armed;
    if (!mode) return;
    if (!ed) return;   /* no editor object located on this build -> no editor-native surface to draw on */
    /* The editor screen object exists only in-editor (null elsewhere); also require an in-editor state. */
    if (ed_state() == 0) return;
    screen = *(uint8_t **)(ed + ED_MENU_SCREEN);
    if (!screen) return;

    /* An unresolved function disables the toast; literal fallback is pinned-only. */
    mk    = (idstr_ctor_t)(uintptr_t)(g_eng.idstr_ctor ? g_eng.idstr_ctor
              : (g_pinned_build ? (uintptr_t)(g_doom_base + RVA_IDSTR_CTOR) : (uintptr_t)0));
    rm    = (idstr_dtor_t)(uintptr_t)(g_eng.idstr_dtor ? g_eng.idstr_dtor
              : (g_pinned_build ? (uintptr_t)(g_doom_base + RVA_IDSTR_DTOR) : (uintptr_t)0));
    toast = (toast_show_t)(uintptr_t)(g_eng.toast_show ? g_eng.toast_show
              : (g_pinned_build ? (uintptr_t)(g_doom_base + RVA_TOAST_SHOW) : (uintptr_t)0));
    if (!mk || !rm || !toast) {
        InterlockedExchange(&g_notice_armed, 0);   /* nothing to show; do not re-check every frame */
        return;
    }

    /* Only an engine error populates this buffer. Generic hardware-fault notices
     * must not display stale text from an earlier error. */
    text = NOTICE_TEXT_STR;
    if (mode == 2 && harvest_engine_msg(msgbuf, sizeof msgbuf)) {
        text = msgbuf;
        {
            shield_fault hf = { "load", -1, msgbuf, 0, 0 };
            shield_emit(&hf);   /* record the harvested engine message alongside the toast */
        }
    }

    mk(title_buf, NOTICE_TITLE_STR);
    mk(text_buf,  text);
    toast(screen, title_buf, text_buf);    /* (screen, TITLE, TEXT) -- self-dedups via toast+0x1b0 */
    rm(text_buf);
    rm(title_buf);

    InterlockedExchange(&g_notice_armed, 0);   /* fire once; the toast's own guard prevents dup re-shows */
}

/* Keep resolved throw suppressors clear so Error(6) can throw to Frame instead
 * of exiting. Read before writing; both are normally zero on the pinned image. */
static void keep_throw_gate_open(void)
{
    if (g_doom_base == NULL) return;
    __try {
        if (g_suppr_a_at && *(volatile int32_t *)g_suppr_a_at != 0)
            *(volatile int32_t *)g_suppr_a_at = 0;
        write_suppressor_b();
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* unreadable page -> skip */ }
}

/* Dismiss save-rejection dialogs by setting their clear flag, without running
 * any button action that could delete a save. The shell may be absent; all
 * queue access is guarded. */
static int is_corrupt_save_gdm(int gdm)
{
    /* CORRUPT_CONTINUE acknowledges a failed load; it does not resume loading. */
    return gdm == GDM_LOAD_DAMAGED_FILE || gdm == GDM_CORRUPT_CONTINUE
        || gdm == GDM_SNAPMAP_DETECTED_CORRUPT || gdm == GDM_SNAPMAP_REMOVED_CORRUPT;
}

static void save_guard_tick(void)
{
    /* An unresolved shell slot disables the guard. */
    if (g_doom_base == NULL || g_shell_slot_at == NULL) return;
    __try {
        uint8_t *S = *(uint8_t **)g_shell_slot_at;
        if (S == NULL) return;
        uint8_t *dlg = *(uint8_t **)(S + SHELL_DLGMGR_OFF);
        if (dlg == NULL) return;
        uint8_t *arr = *(uint8_t **)(dlg + DLGQ_ARR_OFF);
        if (arr == NULL) return;
        {
            int cnt = *(volatile int *)(dlg + DLGQ_COUNT_OFF);
            int i, dismissed = 0, pending_left = 0;
            int first_gdm = 0;
            if (cnt < 0) cnt = 0;
            if (cnt > 4) cnt = 4;                /* the dialog queue capacity is 4 */
            for (i = 0; i < cnt; i++) {
                uint8_t *d = arr + (size_t)i * DLG_DESC_STRIDE;
                int gdm;
                if (*(volatile uint8_t *)(d + DESC_CLEARFLAG_OFF) != 0) continue;   /* already cleared */
                gdm = *(volatile int *)(d + DESC_GDMID_OFF);
                if (is_corrupt_save_gdm(gdm)) {
                    *(volatile uint8_t *)(d + DESC_CLEARFLAG_OFF) = 1;   /* DISMISS-A -- runs NO button action */
                    if (!dismissed) first_gdm = gdm;
                    dismissed++;
                } else {
                    pending_left++;
                }
            }
            if (dismissed) {
                if (pending_left == 0) {         /* sync the visible byte once the queue drained */
                    uint8_t *shellMgr = *(uint8_t **)(S + SHELL_SHELLMGR_OFF);
                    if (shellMgr) *(volatile uint8_t *)(shellMgr + SHELLMGR_VISIBLE_OFF) = 0;
                }
                {
                    /* Preserve the dialog ID: damaged files and rejected map content need
                     * different diagnosis even though both are save errors. */
                    char msg[128];
                    const char *which =
                        first_gdm == GDM_LOAD_DAMAGED_FILE        ? "LOAD_DAMAGED_FILE" :
                        first_gdm == GDM_CORRUPT_CONTINUE         ? "CORRUPT_CONTINUE" :
                        first_gdm == GDM_SNAPMAP_DETECTED_CORRUPT ? "SNAPMAP_DETECTED_CORRUPT" :
                        first_gdm == GDM_SNAPMAP_REMOVED_CORRUPT  ? "SNAPMAP_REMOVED_CORRUPT" :
                                                                    "UNKNOWN";
                    _snprintf_s(msg, sizeof msg, _TRUNCATE,
                                "save-guard: dismissed %s (gdm=%d) (save protected)",
                                which, first_gdm);
                    {
                        shield_fault f = { "save", -1, msg, 0, 0 };
                        shield_emit(&f);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* shell not mapped / shifted RVA -> skip this frame */ }
}

static int64_t frame_detour(void *self)
{
    keep_throw_gate_open();      /* LAYER 2: gate clear so engine Error(6)/downgraded-FatalError recovers */
    save_guard_tick();           /* B: resident save-deletion guard (dismiss the corrupt-save dialogs) */
    recovery_tick();             /* advance a pending editor-exit recovery */
    notice_tick();               /* show a pending editor-native notice */
    return orig_frame(self);
}

/* Change FatalError's level immediate from 7 to 6, allowing its throw to use
 * the recoverable engine path. Verify MOV ECX,7 in the identified wrapper before
 * writing; direct dispatcher calls and failures outside Frame may still exit. */
static void patch_fatalerror_downgrade(void)
{
    /* Patch only a resolved wrapper or the exact pinned-build fallback. */
    uint8_t *fe = (uint8_t *)(uintptr_t)(g_eng.fatalerror7 ? g_eng.fatalerror7
                    : (g_pinned_build ? (uintptr_t)(g_doom_base + RVA_FATALERROR7) : (uintptr_t)0));
    if (fe == NULL) {
        shield_fault f = { "sig", -1,
            "FatalError downgrade declined: the wrapper is unresolved and this is not the pinned "
            "extraction build", 0, 0 };
        shield_emit(&f);
        return;
    }
    __try {
        int i, at = -1;
        for (i = 0; i < 0x40; i++) {
            if (fe[i] == 0xB9 && fe[i + 1] == 0x07 &&
                fe[i + 2] == 0 && fe[i + 3] == 0 && fe[i + 4] == 0) { at = i; break; }
        }
        if (at < 0) {
            shield_fault f = { "sig", -1,
                "FatalError downgrade: MOV ECX,7 not found in wrapper (re-derive 0x1a089e0)", 0, 0 };
            shield_emit(&f);
            return;
        }
        {
            DWORD old;
            uint8_t *imm = fe + at + 1;                      /* the level immediate byte (0x07) */
            if (VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
                *imm = 0x06;                                 /* 7 -> 6: throws recoverable idException now */
                VirtualProtect(imm, 1, old, &old);
                FlushInstructionCache(GetCurrentProcess(), imm, 1);
                shield_fault f = { "action", -1,
                    "FatalError(7) downgraded to recoverable Error(6) (level 7->6)", 0, 0 };
                shield_emit(&f);
            } else {
                shield_fault f = { "sig", -1, "FatalError downgrade: VirtualProtect failed", 0, 0 };
                shield_emit(&f);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shield_fault f = { "sig", -1, "FatalError downgrade write faulted (re-derive 0x1a089e0)", 0, 0 };
        shield_emit(&f);
    }
}

int recovery_install(void)
{
    /* Cache globals before installing the frame hook. */
    g_pinned_build = sh_host_is_pinned_rva_build();
    if (g_doom_base) {
        g_editor_at     = (uint8_t *)glb_resolve(g_doom_base, "editor_singleton", NULL);
        g_load_state_at = (uint8_t *)glb_resolve(g_doom_base, "load_state", NULL);
        g_last_err_at   = (uint8_t *)glb_resolve(g_doom_base, "last_error_msg", NULL);
        g_shell_slot_at = (uint8_t *)glb_resolve(g_doom_base, "shell_ptr_slot", NULL);
        g_suppr_a_at    = (uint8_t *)glb_resolve(g_doom_base, "throw_suppressor_a", NULL);
    }

    /* The Frame signature covers the 15 position-independent bytes stolen here.
 * Refuse unresolved targets unless the exact pinned-build fallback applies. */
    void *target = (void *)(uintptr_t)(g_eng.frame ? g_eng.frame
                     : (g_pinned_build ? (uintptr_t)(g_doom_base + RVA_FRAME) : (uintptr_t)0));
    if (target == NULL) {
        shield_fault f = { "sig", -1,
            "recovery REFUSED: idCommonLocal::Frame is unresolved and this is not the pinned extraction "
            "build -- the frame hook is not installed rather than detouring a guessed address", 0, 0 };
        shield_emit(&f);
        return 0;
    }
    orig_frame = (frame_t)install_inline_hook(target, (void *)frame_detour, FRAME_STOLEN);
    if (orig_frame == NULL) return 0;
    patch_fatalerror_downgrade();   /* LAYER 2: FatalError(7) -> recoverable Error(6) (one-byte level patch) */
    return 1;
}
