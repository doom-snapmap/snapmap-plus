/* swf_textedit.c -- see swf_textedit.h. Ctrl+C / Ctrl+V for the editor's SWF text fields.
 *
 * COPY is pure reads: the focused idSWFTextInstance's text + selection range -> the Windows clipboard.
 * Confirmed live across text, int, float, vec3 and size inspectors (they are all SWF-text-backed).
 *
 * PASTE splices the clipboard into the live text idStr, mirroring EXACTLY what the stock
 * BACKSPACE/DELETE case does -- rebuild as left + inserted + right, assign back, collapse the
 * selection -- so it inherits the engine's own semantics rather than inventing new ones. It is the
 * only thing here that WRITES, and it is gated on its own separately-resolved idStr assignment: if
 * that does not resolve, copy still works and paste simply stays dark rather than guessing.
 *
 * We deliberately do NOT filter what may be pasted into numeric fields. The editor already accepts
 * arbitrary typed text there and resolves it at commit (non-numeric input commits as 0, confirmed
 * live), so matching that is more faithful than inventing a restriction the editor does not have.
 *
 * Every engine touch is SEH-guarded and range-checked, and we ALWAYS chain to the stock handler --
 * we never swallow a key.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "swf_textedit.h"
#include "signatures.h"
#include "patch.h"        /* sh_install_detour_sig / sh_uninstall_detour */
#include "clipboard.h"    /* sh_clipboard_set */
#include "backend_log.h"

#define SIG_SWF_ONKEY     "SwfTextOnKeyCall"
#define SWF_ONKEY_STOLEN  15u   /* push rbp/rsi/rdi/r14/r15 (8) + sub rsp,imm32 (7) = 15 whole, PIC bytes */

/* ---- build-specific offsets (RE-DERIVE per DOOM build: decompile the onKey::Call this detours) ---
 * All recovered DIRECT from that one function: it dereferences exactly these. */
#define TF_TEXTINST_OFF   0xC0    /* "TextField" script object -> its idSWFTextInstance (param_3[0x18]) */
#define TI_TEXT_OFF       0x38    /* idSWFTextInstance -> the live text idStr */
#define TI_LEN_OFF        0x40    /* idSWFTextInstance -> character count (caret/END bound) */
#define TI_SELSTART_OFF   0x124   /* selection anchor (== caret unless shift-selecting) */
#define TI_SELEND_OFF     0x128   /* caret / selection end */

/* idStr ABI. CONFIRMED DIRECT from the engine's own idStr::Left (FUN_14033e640), which reads
 * `*(char **)(str + 0x10) + offset` with NO short-string branch, and whose freshly-constructed result
 * sets `data = self + 0x1c`. So +0x10 is ALWAYS a real char* -- for a short string it simply points at
 * that same object's inline base buffer at +0x1c, never at the pointer field itself.
 * (Do NOT reintroduce a `len < 0x10 ? inline : heap` branch here: that copies the pointer's own bytes
 * as text for every short string -- observed live as garbled clipboard output.) */
#define IDSTR_LEN_OFF     0x08    /* int len (chars, excl NUL) */
#define IDSTR_DATA_OFF    0x10    /* char* data -- always a pointer (inline base buffer @ +0x1c, or heap) */

/* idSWFScriptValue: { int32 type @ +0; value @ +8 }, stride 0x10. Type ids read off the engine's own
 * ToInteger/ToBool converters (FUN_1417438c0 / FUN_1417437e0). */
#define SWFV_STRIDE       0x10
#define SWFV_TYPE_OFF     0x00
#define SWFV_VAL_OFF      0x08
#define SWFV_T_DOUBLE     4
#define SWFV_T_INT        5
#define SWFV_T_BOOL       7

/* DirectInput scancodes -- consistent with every case in the stock handler (0x0e BACKSPACE,
 * 0x2a/0x36 SHIFT, 0xcb LEFT, 0xcd RIGHT, 0xc7 HOME, 0xcf END, 0xd3 DELETE). */
/* No SC_LCTRL/SC_RCTRL here on purpose: Ctrl is read from the OS at the moment C or V arrives
 * (ctrl_is_held), never tracked from this key stream. See the note above swf_onkey_detour. */
#define SC_C              0x2e
#define SC_V              0x2f

/* Extra idSWFTextInstance fields the paste path needs (same provenance as the block above). */
#define TI_MULTILINE_OFF  0x140   /* nonzero = ENTER inserts a newline (gates the stock 0x1c/0x9c case) */
#define TI_MAXCHARS_OFF   0x280   /* character cap; the stock focus path truncates when text exceeds it */

/* idStr size -- +0x1c inline base buffer + STR_ALLOC_BASE(20), matching the ctor's 0x80000014 flags. */
#define IDSTR_SIZE        0x30
#define IDSTR_FLAGS_OFF   0x18

/* Sanity ceiling for a text field's length. A datapad body is far below this; anything above means we
 * are reading garbage (wrong object / stale offset) and must bail rather than trust it. */
#define TEXT_SANE_MAX     (256 * 1024)
/* Clipboard read cap. Heap-allocated (see paste_at_selection) -- never put this on the hook's stack. */
#define CLIP_MAX          (64 * 1024)

/* ---- the "is this actually a TextField?" gate -----------------------------------------------------
 * onKey::Call fires for EVERY focused SWF script object, not only text fields, and only reads its
 * +0xC0 after asking the object `vtable[1](obj, "TextField")`. Skipping that check reads +0xC0 off an
 * unrelated object and yields garbage (observed live on non-text inspectors).
 *
 * That check is a POINTER-IDENTITY compare against the engine's own interned type-name string --
 * `return param_2 == PTR_s_Object_...;` -- NOT a strcmp, so passing our own "TextField" literal would
 * always answer false. We must hand it the engine's exact pointer.
 *
 * We recover that pointer from the detoured function's OWN BODY rather than a hardcoded RVA (same
 * LEA/MOV-decode technique the backend already uses for its *Lea signatures): at +0x4B the prologue
 * carries `48 8B 15 <rel32>` = `mov rdx,[rip+rel32]`, whose target holds the interned "TextField"
 * pointer the engine passes. Verified before use; a byte mismatch disarms the feature rather than
 * guessing. BUILD-SPECIFIC -- re-derive this offset if the prologue changes. */
#define TF_STRPTR_INSN_OFF   0x4Bu                  /* offset of the `mov rdx,[rip+rel32]` in onKey::Call */
static const uint8_t TF_STRPTR_INSN[3] = { 0x48, 0x8B, 0x15 };
#define TF_STRPTR_INSN_LEN   7u                     /* 3 opcode + 4 rel32 */
#define SWFOBJ_ISTYPE_SLOT   1                      /* vtable[1](obj, internedTypeName) -> bool */

typedef void *(*onkey_fn)(void *self, void *retbuf, void *thisObject, void *parms);
typedef char  (*is_type_fn)(void *obj, const void *interned_name);
typedef void  (*idstr_assign_fn)(void *dst_idstr, const void *src_idstr);

static onkey_fn        g_orig_onkey   = NULL;
static const void     *g_textfield_id = NULL;   /* the engine's interned "TextField" string pointer */
static idstr_assign_fn g_idstr_assign = NULL;   /* idStr::operator=(const idStr&) -- NULL => paste off */

/* Ask the script object whether it is a TextField, exactly the way the stock handler does. Returns 0
 * on anything unexpected -- we only ever proceed on a positive answer. */
static int is_textfield(void *obj)
{
    if (obj == NULL || g_textfield_id == NULL) return 0;
    void **vtbl = *(void ***)obj;
    if (vtbl == NULL) return 0;
    is_type_fn is_type = (is_type_fn)vtbl[SWFOBJ_ISTYPE_SLOT];
    if (is_type == NULL) return 0;
    return is_type(obj, g_textfield_id) != 0;
}

/* Read one idSWFScriptValue as an integer, mirroring the engine's own ToInteger for the numeric
 * types. Returns 0 for a type we do not expect here (string/object) rather than guessing. */
static int swfv_int(const uint8_t *v, int64_t *out)
{
    int32_t t = *(const int32_t *)(v + SWFV_TYPE_OFF);
    const void *p = v + SWFV_VAL_OFF;
    if (t == SWFV_T_INT)    { *out = *(const int64_t *)p; return 1; }
    if (t == SWFV_T_DOUBLE) { *out = (int64_t)*(const double *)p; return 1; }
    if (t == SWFV_T_BOOL)   { *out = (*(const char *)p != 0); return 1; }
    return 0;
}

/* Resolve an idStr to its bytes (SSO-aware), with a sanity bound. 0 = do not trust it. */
static int idstr_view(const uint8_t *s, const char **out_data, int *out_len)
{
    int len = *(const int *)(s + IDSTR_LEN_OFF);
    if (len < 0 || len > TEXT_SANE_MAX) return 0;
    *out_data = *(const char *const *)(s + IDSTR_DATA_OFF);
    *out_len = len;
    return (*out_data != NULL);
}

/* Copy the focused field's selection (or the whole field when nothing is selected) to the clipboard.
 * Pure reads. Returns 1 if something was placed on the clipboard. */
static int copy_selection(const uint8_t *ti)
{
    const char *data = NULL;
    int len = 0;
    if (!idstr_view(ti + TI_TEXT_OFF, &data, &len)) return 0;
    if (len == 0) return 0;

    int a = *(const int *)(ti + TI_SELSTART_OFF);
    int b = *(const int *)(ti + TI_SELEND_OFF);
    int lo = (a < b) ? a : b;
    int hi = (a < b) ? b : a;
    /* Clamp against the ACTUAL string length, not the instance's own count field -- if the two ever
     * disagree the string is the one we are about to read out of. */
    if (lo < 0) lo = 0;
    if (hi > len) hi = len;
    if (lo > len) lo = len;
    if (lo >= hi) { lo = 0; hi = len; }   /* nothing selected -> copy the whole field */

    int n = hi - lo;
    if (n <= 0 || n > TEXT_SANE_MAX) return 0;

    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n + 1);
    if (buf == NULL) return 0;
    memcpy(buf, data + lo, (size_t)n);
    buf[n] = '\0';
    int ok = sh_clipboard_set(buf);
    HeapFree(GetProcessHeap(), 0, buf);

    if (ok) {
        char msg[96];
        _snprintf_s(msg, sizeof msg, _TRUNCATE,
                    "swf-textedit: copied %d char%s to the clipboard", n, n == 1 ? "" : "s");
        backend_log(msg);
    }
    return ok;
}

/* Splice the clipboard into the focused field at the selection, mirroring EXACTLY what the stock
 * BACKSPACE/DELETE case does: rebuild the string as left + inserted + right, assign it back into the
 * live idStr, then collapse the selection to the end of what was inserted. The engine never touches
 * the instance's own length field there either -- it does not need to, because that field IS the
 * idStr's length word (text @ +0x38, len @ +0x38+0x08 = +0x40), so the assign updates it for free.
 *
 * We do NOT filter what gets pasted into numeric fields: the engine already accepts arbitrary typed
 * text there and resolves it at commit (a non-numeric value simply commits as 0, confirmed live), so
 * matching that behaviour is strictly more faithful than inventing a restriction the editor lacks. */
static int paste_at_selection(const uint8_t *ti)
{
    if (g_idstr_assign == NULL) return 0;

    /* Heap, not stack: this runs on the game's main thread inside a detour, where a buffer this size
     * would be a stack-overflow hazard. */
    char *clip = (char *)HeapAlloc(GetProcessHeap(), 0, CLIP_MAX);
    if (clip == NULL) return 0;
    if (!sh_clipboard_get(clip, (int)CLIP_MAX)) { HeapFree(GetProcessHeap(), 0, clip); return 0; }

    /* Normalise line endings. CR is never wanted. A multi-line field keeps LF verbatim -- blank lines
     * are real formatting in a datapad body. A single-line field cannot represent line structure at
     * all, so any RUN of newlines collapses to ONE space, and a run at the very start or very end is
     * dropped outright. (Emitting one space per newline instead just leaks the original's blank lines
     * as runs of spaces -- pasted prose is full of them.) */
    int multiline = (*(const int *)(ti + TI_MULTILINE_OFF) != 0);
    int ci = 0, co = 0, pending_break = 0;
    for (; clip[ci] != '\0'; ci++) {
        char c = clip[ci];
        if (c == '\r') continue;
        if (c == '\n' && !multiline) { pending_break = 1; continue; }
        if (pending_break) { if (co > 0) clip[co++] = ' '; pending_break = 0; }
        clip[co++] = c;
    }
    clip[co] = '\0';   /* a trailing run leaves pending_break set and is intentionally dropped */

    const char *data = NULL;
    int len = 0;
    if (co == 0 || !idstr_view(ti + TI_TEXT_OFF, &data, &len)) {
        HeapFree(GetProcessHeap(), 0, clip);
        return 0;
    }

    int a = *(const int *)(ti + TI_SELSTART_OFF);
    int b = *(const int *)(ti + TI_SELEND_OFF);
    int lo = (a < b) ? a : b;
    int hi = (a < b) ? b : a;
    if (lo < 0) lo = 0;
    if (lo > len) lo = len;
    if (hi < lo) hi = lo;
    if (hi > len) hi = len;

    /* Honour the field's character cap the way the engine's own focus path does. <= 0 means no cap. */
    int maxchars = *(const int *)(ti + TI_MAXCHARS_OFF);
    int keep = lo + (len - hi);
    int ins  = co;
    if (maxchars > 0 && keep + ins > maxchars) ins = maxchars - keep;
    int total = keep + ins;
    char *buf = NULL;
    if (ins > 0 && total >= 0 && total < TEXT_SANE_MAX)
        buf = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)total + 1);
    if (buf == NULL) { HeapFree(GetProcessHeap(), 0, clip); return 0; }
    memcpy(buf, data, (size_t)lo);
    memcpy(buf + lo, clip, (size_t)ins);
    memcpy(buf + lo + ins, data + hi, (size_t)(len - hi));
    buf[total] = '\0';

    /* A stack-built source idStr: the assign only reads its length and data, and a zeroed flags word
     * declines the steal/swap fast path, so nothing engine-owned is aliased or freed. */
    uint8_t src[IDSTR_SIZE];
    memset(src, 0, sizeof src);
    *(int *)(src + IDSTR_LEN_OFF)   = total;
    *(char **)(src + IDSTR_DATA_OFF) = buf;
    *(uint32_t *)(src + IDSTR_FLAGS_OFF) = 0;

    g_idstr_assign((void *)(ti + TI_TEXT_OFF), src);
    HeapFree(GetProcessHeap(), 0, buf);
    HeapFree(GetProcessHeap(), 0, clip);

    int caret = lo + ins;
    *(int *)(ti + TI_SELSTART_OFF) = caret;
    *(int *)(ti + TI_SELEND_OFF)   = caret;

    char msg[96];
    _snprintf_s(msg, sizeof msg, _TRUNCATE,
                "swf-textedit: pasted %d char%s%s", ins, ins == 1 ? "" : "s",
                (ins < co) ? " (truncated to the field's limit)" : "");
    backend_log(msg);
    return 1;
}

/* Is Ctrl PHYSICALLY held, right now?
 *
 * This deliberately does NOT track Ctrl from the onKey stream. The first version did, latching a
 * static flag on the Ctrl key-down and clearing it on the key-up, "the way the stock handler tracks
 * Shift" -- and it shipped a bug that made the editor's text fields unusable: after one Ctrl+C, a
 * bare `c` copied and a bare `v` pasted, forever. Typing the word "variable" pasted the clipboard
 * into the field once per `v`.
 *
 * The cause is that the latch outlives the keystroke. The stock handler reads Shift only while
 * processing the keystroke it was handed, so a Ctrl/Shift release it never sees costs nothing;
 * ours was consulted on LATER keystrokes, so a single missed key-up left it stuck on. Modifier
 * key-ups are not reliably delivered to a focused SWF script object -- the field can lose focus, the
 * window can lose focus, or the engine may simply not dispatch them -- and any one of those arms the
 * bug permanently.
 *
 * So: no state. Ask the OS for the real key state at the instant C or V arrives. There is nothing to
 * go stale, alt-tabbing mid-chord cannot poison it, and it covers both Ctrl keys without caring
 * which scancode the engine reports. GetAsyncKeyState reads physical key state directly rather than
 * this thread's message queue, which matters because the hook runs on the engine's input path. */
static int ctrl_is_held(void)
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

/* The detour. Acts on Ctrl+C / Ctrl+V, and ALWAYS chains -- we never swallow a key. */
static void *swf_onkey_detour(void *self, void *retbuf, void *thisObject, void *parms)
{
    __try {
        const uint8_t *pv = (parms != NULL) ? *(const uint8_t *const *)parms : NULL;
        int64_t key = 0, down = 0;
        if (pv != NULL && swfv_int(pv, &key) && swfv_int(pv + SWFV_STRIDE, &down)) {
            if (down && (key == SC_C || key == SC_V) && ctrl_is_held() && is_textfield(thisObject)) {
                const uint8_t *ti = *(const uint8_t *const *)((const uint8_t *)thisObject + TF_TEXTINST_OFF);
                if (ti != NULL) {
                    if (key == SC_C) copy_selection(ti);
                    else             paste_at_selection(ti);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A bad offset degrades to "clipboard did nothing" -- never to a broken text field. */
    }
    return g_orig_onkey(self, retbuf, thisObject, parms);
}

void sh_swf_textedit_install(const uint8_t *module_base)
{
    if (g_orig_onkey != NULL) return;   /* already armed */
    if (module_base == NULL) { backend_log("swf-textedit: NOT armed (no module base)"); return; }

    const sig_entry *sig = NULL;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++)
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, SIG_SWF_ONKEY) == 0) {
            sig = &BACKEND_ENGINE_SIGNATURES[i];
            break;
        }
    if (sig == NULL) { backend_log("swf-textedit: NOT armed (signature missing from the DB)"); return; }

    sig_result r;
    sig_resolve_one(module_base, sig, &r);

    /* Recover the interned "TextField" pointer from the resolved function's own body BEFORE detouring
     * (the detour overwrites only the first 15 bytes, but decode first anyway so a failure costs no
     * engine write at all). Without it the type gate can never answer true, so refuse to arm. */
    if (r.status == SIG_OK || r.status == SIG_OK_HOOKED) {
        __try {
            const uint8_t *insn = (const uint8_t *)r.addr + TF_STRPTR_INSN_OFF;
            if (memcmp(insn, TF_STRPTR_INSN, sizeof TF_STRPTR_INSN) == 0) {
                int32_t rel = *(const int32_t *)(insn + sizeof TF_STRPTR_INSN);
                const void *const *slot = (const void *const *)(insn + TF_STRPTR_INSN_LEN + rel);
                g_textfield_id = *slot;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_textfield_id = NULL; }
    }
    if (g_textfield_id == NULL) {
        backend_log("swf-textedit: NOT armed (could not decode the interned \"TextField\" id from the "
                    "handler prologue -- re-derive TF_STRPTR_INSN_OFF for this DOOM build)");
        return;
    }

    void *tramp = sh_install_detour_sig(&r, (void *)swf_onkey_detour, SWF_ONKEY_STOLEN);
    if (tramp == NULL) {
        backend_log("swf-textedit: NOT armed (onKey detour install refused/failed)");
        return;
    }
    g_orig_onkey = (onkey_fn)tramp;

    /* Paste additionally needs a real idStr assignment. Resolve it independently: if it is missing,
     * copy still works and only paste stays dark -- never fall back to a guess, since this one WRITES. */
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "IdStrAssignFromStr") != 0) continue;
        sig_result ra;
        sig_status st = sig_resolve_one(module_base, &BACKEND_ENGINE_SIGNATURES[i], &ra);
        if (st == SIG_OK || st == SIG_OK_HOOKED) g_idstr_assign = (idstr_assign_fn)ra.addr;
        break;
    }

    backend_log(g_idstr_assign != NULL
                ? "swf-textedit: ready (Ctrl+C copies / Ctrl+V pastes the focused SWF text field)"
                : "swf-textedit: ready, COPY ONLY (idStr assign unresolved -- Ctrl+V disabled)");
}

void sh_swf_textedit_uninstall(void)
{
    if (g_orig_onkey == NULL) return;
    sh_uninstall_detour((void *)g_orig_onkey);
    g_orig_onkey = NULL;
}
