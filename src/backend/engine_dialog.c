/* engine_dialog.c -- see engine_dialog.h.
 *
 * Three engine facts this file is built on, each read out of the binary rather
 * than assumed:
 *
 *   1. `ShowDialog(mgr, desc)` prefers `desc`'s own embedded idStr over the text
 *      it derives from the GDM id. Populating that string is the whole trick.
 *   2. The engine NEVER calls `AddDialog` directly. It goes through a shell-level
 *      wrapper that also sets a flag on the shell's screen object
 *      (shell+0x18 -> +0xA8 = 1). Raising without that flag produces a dialog
 *      that draws perfectly and ignores every keypress -- measured live, with
 *      verified key delivery, at both the hub and the map browser. So we raise
 *      through the wrapper and let the engine set its own flag, and `shell` is
 *      captured by watching the engine raise one of its own.
 *   3. The dialog queue is an inline array of 0x1B0-byte descriptors at
 *      mgr+0x900 with its count at mgr+0x908, which is how a raised dialog is
 *      found again to read its answer.
 *
 * The descriptor's string is ALREADY CONSTRUCTED when the detour sees it, so it
 * is assigned (idStr::operator=) and never constructed over: constructing into a
 * live idStr would leak its buffer and hand the SWF a dangling pointer.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "backend_log.h"
#include "engine_dialog.h"
#include "hook.h"

/* Descriptor layout (RE: copy ctor 0xE640D0, dtor 0xE63CE0, ShowDialog 0xE6A260). */
#define ED_DESC_STRIDE        0x1B0u
#define ED_DESC_GDM_ID        0x00u   /* int   */
#define ED_DESC_BUTTON_SET    0x04u   /* int   */
#define ED_DESC_CLEARED       0x08u   /* byte: set once the dialog is answered/dismissed */
#define ED_DESC_RESULT        0x09u   /* byte: the answer. Dismiss-all writes 0 here.     */
#define ED_DESC_TEXT          0x18u   /* idStr, by value                                  */
#define ED_DESC_TEXT_LENGTH   0x20u   /* int, the field ShowDialog tests                  */

/* Menu-manager layout (RE: AddDialogInternal 0xE65C20, ClearDialog 0xE663C0). */
#define ED_MGR_QUEUE_PTR      0x900u
#define ED_MGR_QUEUE_COUNT    0x908u

/* How much of a descriptor to dump when it is removed. Covers the id, the button
 * set, both flag bytes and the three flags AddDialog writes from its params. */
#define ED_DESC_HEAD_BYTES    0x20

/* `AddDialog`'s parameter block. Only these fields are read for an id with no
 * switch case, which is every id this product raises. The block is zeroed and
 * generously oversized rather than sized exactly -- the engine writes nothing
 * into it, so slack is free and a short block would not be. */
#define ED_PARAMS_BYTES       0x100u
#define ED_PARAM_GDM_ID       0u      /* int index */
#define ED_PARAM_BUTTON_SET   1u      /* int index */
#define ED_PARAM_SOURCE_FILE  0x26u   /* int index; a const char * spans 0x26..0x27 */
#define ED_PARAM_SOURCE_LINE  0x28u   /* int index; log formatting only */

/* ShowDialog prologue steal window (disasm of 0xE6A260):
 *     48 8B C4              mov  rax, rsp          (3)
 *     57                    push rdi               (1)
 *     48 81 EC 80 00 00 00  sub  rsp, 0x80         (7)
 *     48 C7 40 B8 FE FF FF FF  mov qword [rax-0x48], -2   (8)
 * = 19 bytes of whole, register/immediate-only instructions. No RIP-relative
 * operand and no relative branch, so the bytes relocate unchanged. */
#define ED_SHOWDIALOG_STOLEN  19

/* RemoveDialog prologue steal window (disasm of 0xE678F0):
 *     40 56                 push rsi                        (2)
 *     48 83 EC 20           sub  rsp, 0x20                  (4)
 *     48 8B F1              mov  rsi, rcx                   (3)
 *     3B 91 08 09 00 00     cmp  edx, [rcx+0x908]           (6)
 * = 15 bytes, all register/immediate, no RIP-relative operand. */
#define ED_REMOVEDIALOG_STOLEN 15

/* AddDialogWrapper prologue steal window (disasm of 0x17363A0):
 *     40 57                       push rdi                 (2)
 *     48 83 EC 30                 sub  rsp, 0x30           (4)
 *     48 C7 44 24 20 FE FF FF FF  mov  qword [rsp+0x20],-2 (9)
 * = 15 bytes, register/rsp-relative only, no RIP-relative operand. */
#define ED_WRAPPER_STOLEN      15

typedef void (*ed_add_wrapper_fn)(void *shell, void *params);
typedef void (*ed_assign_cstr_fn)(void *idstr, const char *text);
typedef void (*ed_remove_dialog_fn)(void *mgr, int index);

static ed_add_wrapper_fn  g_add_wrapper;      /* the real shell-level raise */
static ed_add_wrapper_fn  g_wrapper_original;  /* trampoline for the capture hook */
static ed_assign_cstr_fn  g_assign_cstr;
static ed_remove_dialog_fn g_remove_original;
static void * volatile    g_shell;
static int                g_installed;

/* The answer, captured at removal. -1 = nothing captured yet. */
static volatile LONG g_answer = -1;

/* One tracked dialog at a time. The install flow asks one question and waits for
 * it, so a queue here would be capacity nothing uses and state that can go
 * stale. `g_ticket` is monotonic so a released ticket can never be confused
 * with a later one that happens to reuse the slot. */
static volatile LONG g_ticket;
static volatile LONG g_pending_id = -1;
static char          g_pending_key[256];
static volatile LONG g_injected;

static const sig_result *ed_result(const sig_result *results, size_t count,
                                   const char *name)
{
    size_t i;
    if (!results || !name) return NULL;
    for (i = 0; i < count; i++)
        if (results[i].name && strcmp(results[i].name, name) == 0)
            return &results[i];
    return NULL;
}

/* A clean SIG_OK resolve only. These calls have a data-layout contract -- the
 * descriptor offsets and the queue shape -- so a hook-tolerant resolve is not
 * good enough even though it would be callable. */
static void *ed_clean(const sig_result *results, size_t count, const char *name,
                      const uint8_t *module_base, uint32_t expect_rva)
{
    const sig_result *r = ed_result(results, count, name);
    if (!r || r->status != SIG_OK || !r->addr) return NULL;
    if (r->rva != expect_rva) return NULL;
    if (r->addr != (uintptr_t)module_base + expect_rva) return NULL;
    return (void *)r->addr;
}

static int ed_read_int(const void *base, unsigned offset, int *out)
{
    __try {
        *out = *(const int *)((const uint8_t *)base + offset);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int ed_read_byte(const void *base, unsigned offset, unsigned char *out)
{
    __try {
        *out = *(const unsigned char *)((const uint8_t *)base + offset);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* The injection itself, factored out so the test seam drives the same code the
 * detour does rather than an approximation of it. Returns 1 when this
 * descriptor was ours and its text was replaced. */
static void *ed_find_descriptor(int gdm_id);

static int ed_inject(void *descriptor)
{
    int id = 0;

    if (!descriptor) return 0;
    if (InterlockedCompareExchange(&g_pending_id, 0, 0) < 0) return 0;
    if (!ed_read_int(descriptor, ED_DESC_GDM_ID, &id)) return 0;
    if (id != (int)InterlockedCompareExchange(&g_pending_id, 0, 0)) return 0;
    if (!g_pending_key[0] || !g_assign_cstr) return 0;

    __try {
        g_assign_cstr((uint8_t *)descriptor + ED_DESC_TEXT, g_pending_key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("engine-dialog: assigning our text into the descriptor raised "
                    "an exception; the engine's own text stands");
        return 0;
    }
    InterlockedExchange(&g_injected, 1);
    return 1;
}

/* The engine removes an answered dialog in the same frame it is answered, so
 * this is the only place the answer and the descriptor coexist. Read it here,
 * before the entry is shifted out of the queue, and let the poll report what was
 * captured rather than racing the engine for it. */
static void ed_remove_detour(void *mgr, int index)
{
    LONG want = InterlockedCompareExchange(&g_pending_id, 0, 0);

    if (want >= 0 && mgr && index >= 0) {
        __try {
            void *queue = *(void **)((uint8_t *)mgr + ED_MGR_QUEUE_PTR);
            int count = *(const int *)((const uint8_t *)mgr + ED_MGR_QUEUE_COUNT);
            if (queue && index < count) {
                const uint8_t *desc = (const uint8_t *)queue + (size_t)index * ED_DESC_STRIDE;
                if (*(const int *)(desc + ED_DESC_GDM_ID) == (int)want) {
                    char line[320];
                    char hex[3 * ED_DESC_HEAD_BYTES + 1];
                    unsigned char cleared = desc[ED_DESC_CLEARED];
                    unsigned char result  = desc[ED_DESC_RESULT];
                    int k;
                    InterlockedExchange(&g_answer, (LONG)result);
                    /* Dump the descriptor head as well as the two named bytes.
                     * The byte that records WHICH button was pressed is not
                     * identified yet -- the obvious candidate reads 0 both for a
                     * real answer and for an external dismiss -- so log the whole
                     * head and let a yes-versus-no comparison show which byte
                     * actually moves, instead of guessing another offset. */
                    for (k = 0; k < ED_DESC_HEAD_BYTES; k++)
                        _snprintf_s(hex + k * 3, 4, _TRUNCATE, "%02X ", desc[k]);
                    _snprintf_s(line, sizeof line, _TRUNCATE,
                                "engine-dialog: gdm %d removed -- cleared=%u resultByte=%u head=%s\n",
                                (int)want, (unsigned)cleared, (unsigned)result, hex);
                    backend_log(line);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* nothing readable; the poll falls back to "gone means declined" */
        }
    }
    if (g_remove_original) g_remove_original(mgr, index);
}

/* The dialog manager lives at shell+8; the wrapper reads it there to call
 * AddDialog, and the queue we search for our descriptor belongs to it. */
static void *ed_manager(void)
{
    void *shell = g_shell;
    if (!shell) return NULL;
    __try {
        return *(void **)((uint8_t *)shell + 0x08);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* Capture the shell from the engine's own traffic. Every dialog the game raises
 * comes through here, including the stay-offline notice during boot, so the
 * pointer is available long before anything of ours needs it -- and it is the
 * engine's own value rather than a singleton we guessed at. */
static void ed_wrapper_detour(void *shell, void *params)
{
    if (shell && !g_shell) {
        g_shell = shell;
        backend_log("engine-dialog: shell observed; the engine dialog surface is ready");
    }
    if (g_wrapper_original) g_wrapper_original(shell, params);
}

int sh_engine_dialog_install(const sig_result *results, size_t count,
                             const uint8_t *module_base)
{
    void *wrapper, *assign, *remove, *tramp;

    if (g_installed) return 1;
    if (!module_base) return 0;

    wrapper = ed_clean(results, count, "AddDialogWrapper", module_base, 0x17363A0u);
    assign  = ed_clean(results, count, "IdStrAssignCStr",  module_base, 0x19FD5F0u);
    remove  = ed_clean(results, count, "RemoveDialog",     module_base, 0xE678F0u);
    if (!wrapper || !assign || !remove) {
        backend_log("engine-dialog REFUSED: AddDialogWrapper/RemoveDialog/IdStrAssignCStr all "
                    "require a clean exact-address resolve");
        return 0;
    }

    g_add_wrapper = (ed_add_wrapper_fn)wrapper;
    g_assign_cstr = (ed_assign_cstr_fn)assign;

    /* Watch the wrapper to learn the shell. ShowDialog is deliberately NOT
     * hooked: the text goes into the queued descriptor right after the raise,
     * which needs no detour on the engine's own render path. */
    tramp = install_inline_hook(wrapper, (void *)ed_wrapper_detour, ED_WRAPPER_STOLEN);
    if (!tramp) {
        g_add_wrapper = NULL;
        g_assign_cstr = NULL;
        backend_log("engine-dialog REFUSED: the AddDialogWrapper detour could not be installed");
        return 0;
    }
    g_wrapper_original = (ed_add_wrapper_fn)tramp;

    tramp = install_inline_hook(remove, (void *)ed_remove_detour, ED_REMOVEDIALOG_STOLEN);
    if (!tramp) {
        backend_log("engine-dialog REFUSED: the RemoveDialog detour could not be installed, so a "
                    "dialog's answer could never be read");
        g_add_wrapper = NULL;
        g_assign_cstr = NULL;
        g_wrapper_original = NULL;
        return 0;
    }
    g_remove_original = (ed_remove_dialog_fn)tramp;
    g_installed = 1;
    backend_log("engine-dialog installed: dialogs can now carry our own text");
    return 1;
}

int sh_engine_dialog_ready(void)
{
    return g_installed && g_shell != NULL;
}

int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text)
{
    void *shell = g_shell;
    uint32_t params[ED_PARAMS_BYTES / sizeof(uint32_t)];
    const char *source = "snapmap-plus";
    char line[256];
    void *desc;

    if (!g_installed || !shell || !g_add_wrapper || !text || !text[0]) return 0;

    /* One at a time -- but a claim only counts while its dialog is actually on
     * screen. Anything can clear a dialog without going through us: the player
     * answering it, the engine tearing the menu down, another tool dismissing
     * it. If the claim's descriptor is gone from the queue, the claim is dead
     * and holding it would wedge this surface for the rest of the session with
     * no way back. Reclaim it instead of refusing forever. */
    {
        LONG held = InterlockedCompareExchange(&g_pending_id, 0, 0);
        if (held >= 0) {
            if (ed_find_descriptor((int)held) != NULL) return 0;   /* genuinely busy */
            backend_log("engine-dialog: the previous dialog left the queue without being polled; "
                        "reclaiming the surface");
            InterlockedExchange(&g_pending_id, -1);
        }
    }

    /* The descriptor's inline idStr is 256 bytes and the copy is by value, so
     * anything longer would be truncated by the engine rather than by us. */
    if (strlen(text) >= sizeof(g_pending_key)) return 0;
    strcpy_s(g_pending_key, sizeof(g_pending_key), text);
    InterlockedExchange(&g_injected, 0);
    InterlockedExchange(&g_answer, -1);
    InterlockedExchange(&g_pending_id, (LONG)gdm_id);

    memset(params, 0, sizeof(params));
    params[ED_PARAM_GDM_ID]     = gdm_id;
    params[ED_PARAM_BUTTON_SET] = button_set;
    memcpy(&params[ED_PARAM_SOURCE_FILE], &source, sizeof(source));
    params[ED_PARAM_SOURCE_LINE] = __LINE__;

    /* Through the WRAPPER, never AddDialog directly: the wrapper takes the
     * engine's own lock and, crucially, marks the shell's screen as having a
     * dialog pending. Without that mark the dialog draws and cannot be
     * answered. */
    __try {
        g_add_wrapper(shell, params);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_pending_id, -1);
        backend_log("engine-dialog: the raise faulted; no dialog was shown");
        return 0;
    }

    /* AddDialogInternal copies the descriptor into the queue synchronously, so
     * by here it exists and the text can be written straight into it. Doing it
     * this way means no detour on the engine's dialog render path at all. */
    desc = ed_find_descriptor((int)gdm_id);
    if (!desc) {
        InterlockedExchange(&g_pending_id, -1);
        backend_log("engine-dialog: the raise returned but no descriptor was queued");
        return 0;
    }
    if (!ed_inject(desc)) {
        backend_log("engine-dialog: the queued descriptor would not take our text; the engine's "
                    "own wording stands");
    }

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "engine-dialog: raised gdm %u with buttons %u carrying '%s'",
                gdm_id, button_set, text);
    backend_log(line);
    return (int)InterlockedIncrement(&g_ticket);
}

/* Find our descriptor in the live queue. NULL means it is no longer there. */
static void *ed_find_descriptor(int gdm_id)
{
    void *mgr = ed_manager();
    void *queue = NULL;
    int queue_count = 0, i;

    if (!mgr) return NULL;
    __try {
        queue = *(void **)((uint8_t *)mgr + ED_MGR_QUEUE_PTR);
        queue_count = *(const int *)((const uint8_t *)mgr + ED_MGR_QUEUE_COUNT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    if (!queue || queue_count <= 0 || queue_count > 64) return NULL;

    for (i = 0; i < queue_count; i++) {
        void *desc = (uint8_t *)queue + (size_t)i * ED_DESC_STRIDE;
        int id = 0;
        if (!ed_read_int(desc, ED_DESC_GDM_ID, &id)) return NULL;
        if (id == gdm_id) return desc;
    }
    return NULL;
}

int sh_engine_dialog_poll(int ticket)
{
    int id = (int)InterlockedCompareExchange(&g_pending_id, 0, 0);
    void *desc;
    unsigned char cleared = 0, result = 0;
    char line[192];

    if (ticket <= 0 || ticket != (int)InterlockedCompareExchange(&g_ticket, 0, 0))
        return SH_ENGINE_DIALOG_LOST;
    if (id < 0) return SH_ENGINE_DIALOG_LOST;

    desc = ed_find_descriptor(id);
    if (!desc) {
        /* Gone from the queue. The removal detour reads the answer out of the
         * descriptor at the last instant it exists, so prefer what it captured.
         * With nothing captured, the dialog left by some path we did not see and
         * the answer is unknown -- which is a decline, because consenting to
         * install third-party content on an answer nobody observed is the one
         * outcome worth being wrong in the safe direction about. */
        LONG answer = InterlockedCompareExchange(&g_answer, 0, 0);
        InterlockedExchange(&g_pending_id, -1);
        if (answer < 0) {
            backend_log("engine-dialog: the dialog left the queue with no answer captured; "
                        "treating that as declined");
            return SH_ENGINE_DIALOG_DECLINED;
        }
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "engine-dialog: gdm %d answered with result byte %ld", id, (long)answer);
        backend_log(line);
        return answer ? SH_ENGINE_DIALOG_ACCEPTED : SH_ENGINE_DIALOG_DECLINED;
    }
    if (!ed_read_byte(desc, ED_DESC_CLEARED, &cleared)) return SH_ENGINE_DIALOG_PENDING;
    if (!cleared) return SH_ENGINE_DIALOG_PENDING;
    if (!ed_read_byte(desc, ED_DESC_RESULT, &result)) result = 0;

    InterlockedExchange(&g_pending_id, -1);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "engine-dialog: gdm %d answered, result byte %u (0 = declined/dismissed)",
                id, (unsigned)result);
    backend_log(line);
    /* The dismiss-all primitive writes result 0, so 0 is the negative answer and
     * anything else is affirmative. */
    return result ? SH_ENGINE_DIALOG_ACCEPTED : SH_ENGINE_DIALOG_DECLINED;
}

void sh_engine_dialog_dump(int (*printf_fn)(const char *fmt, ...))
{
    void *mgr = ed_manager();
    void *queue = NULL;
    int queue_count = 0, i;

    if (!printf_fn) return;
    if (!mgr) { printf_fn("engine-dialog: no shell observed yet.\n"); return; }
    __try {
        queue = *(void **)((uint8_t *)mgr + ED_MGR_QUEUE_PTR);
        queue_count = *(const int *)((const uint8_t *)mgr + ED_MGR_QUEUE_COUNT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf_fn("engine-dialog: the dialog queue was unreadable.\n");
        return;
    }
    printf_fn("engine-dialog queue: %d entr%s (tracking gdm %d)\n",
              queue_count, queue_count == 1 ? "y" : "ies",
              (int)InterlockedCompareExchange(&g_pending_id, 0, 0));
    if (!queue || queue_count <= 0 || queue_count > 64) return;
    for (i = 0; i < queue_count; i++) {
        void *desc = (uint8_t *)queue + (size_t)i * ED_DESC_STRIDE;
        int id = 0, buttons = 0, text_len = 0;
        unsigned char cleared = 0, result = 0;
        if (!ed_read_int(desc, ED_DESC_GDM_ID, &id)) break;
        (void)ed_read_int(desc, ED_DESC_BUTTON_SET, &buttons);
        (void)ed_read_int(desc, ED_DESC_TEXT_LENGTH, &text_len);
        (void)ed_read_byte(desc, ED_DESC_CLEARED, &cleared);
        (void)ed_read_byte(desc, ED_DESC_RESULT, &result);
        printf_fn("  [%d] gdm=%d buttons=%d textLen=%d cleared=%u resultByte=%u\n",
                  i, id, buttons, text_len, (unsigned)cleared, (unsigned)result);
    }
}

void sh_engine_dialog_release(int ticket)
{
    if (ticket > 0 && ticket == (int)InterlockedCompareExchange(&g_ticket, 0, 0))
        InterlockedExchange(&g_pending_id, -1);
}

#ifdef SH_ENGINE_DIALOG_TESTING
void sh_engine_dialog_test_reset(void)
{
    g_wrapper_original = NULL;
    g_add_wrapper = NULL;
    g_assign_cstr = NULL;
    g_shell = NULL;
    g_installed = 0;
    g_pending_key[0] = '\0';
    InterlockedExchange(&g_ticket, 0);
    InterlockedExchange(&g_pending_id, -1);
    InterlockedExchange(&g_injected, 0);
}

void sh_engine_dialog_test_bind(void *shell, void *add_wrapper, void *assign_cstr)
{
    g_add_wrapper = (ed_add_wrapper_fn)add_wrapper;
    g_assign_cstr = (ed_assign_cstr_fn)assign_cstr;
    g_installed = 1;
    g_shell = shell;
}

int sh_engine_dialog_test_inject(void *descriptor)
{
    return ed_inject(descriptor);
}

int sh_engine_dialog_test_pending_id(void)
{
    return (int)InterlockedCompareExchange(&g_pending_id, 0, 0);
}
#endif
