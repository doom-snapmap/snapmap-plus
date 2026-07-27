/* swf_textedit.c -- see swf_textedit.h. Ctrl+C copy out of the editor's SWF text fields.
 *
 * Stage 1 is deliberately READ-ONLY on the engine side: we read the focused idSWFTextInstance's text
 * and selection range and push the result to the Windows clipboard, then ALWAYS chain to the stock
 * handler. Nothing engine-side is mutated, so a wrong offset can at worst fault into our SEH guard
 * instead of corrupting a map. Paste (which must splice the text buffer) comes after this half is
 * confirmed working against a live build.
 *
 * Every engine read is SEH-guarded AND range-checked -- none of these offsets has been validated
 * against a running process yet, so the code treats them as untrusted until proven.
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
#define SC_LCTRL          0x1d
#define SC_RCTRL          0x9d
#define SC_C              0x2e

/* Sanity ceiling for a text field's length. A datapad body is far below this; anything above means we
 * are reading garbage (wrong object / stale offset) and must bail rather than trust it. */
#define TEXT_SANE_MAX     (256 * 1024)

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

static onkey_fn      g_orig_onkey   = NULL;
static const void   *g_textfield_id = NULL;   /* the engine's interned "TextField" string pointer */
static volatile LONG g_ctrl_down    = 0;

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

/* The detour. Tracks Ctrl exactly the way the stock handler tracks Shift (from the isDown it is
 * handed), acts on Ctrl+C, and ALWAYS chains -- we never swallow a key in this stage. */
static void *swf_onkey_detour(void *self, void *retbuf, void *thisObject, void *parms)
{
    __try {
        const uint8_t *pv = (parms != NULL) ? *(const uint8_t *const *)parms : NULL;
        int64_t key = 0, down = 0;
        if (pv != NULL && swfv_int(pv, &key) && swfv_int(pv + SWFV_STRIDE, &down)) {
            if (key == SC_LCTRL || key == SC_RCTRL) {
                InterlockedExchange(&g_ctrl_down, down ? 1 : 0);
            } else if (down && key == SC_C && g_ctrl_down && is_textfield(thisObject)) {
                const uint8_t *ti = *(const uint8_t *const *)((const uint8_t *)thisObject + TF_TEXTINST_OFF);
                if (ti != NULL) copy_selection(ti);
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
    backend_log("swf-textedit: ready (Ctrl+C copies the focused SWF text field)");
}

void sh_swf_textedit_uninstall(void)
{
    if (g_orig_onkey == NULL) return;
    sh_uninstall_detour((void *)g_orig_onkey);
    g_orig_onkey = NULL;
}
