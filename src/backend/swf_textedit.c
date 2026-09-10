/* Ctrl+C/Ctrl+V for focused SWF text fields. Copy reads the selected text; paste
 * splices through engine idStr assignment and leaves numeric validation to the
 * editor. Missing assignment support disables paste only. The stock key handler
 * always runs after our guarded clipboard work. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "swf_textedit.h"
#include "signatures.h"
#include "patch.h"
#include "clipboard.h"
#include "backend_log.h"

#define SIG_SWF_ONKEY     "SwfTextOnKeyCall"
#define SWF_ONKEY_STOLEN  15u   /* push rbp/rsi/rdi/r14/r15 (8) + sub rsp,imm32 (7) = 15 whole, PIC bytes */

/* Recheck object offsets against the signed onKey::Call when porting builds. */
#define TF_TEXTINST_OFF   0xC0    /* "TextField" script object -> its idSWFTextInstance (param_3[0x18]) */
#define TI_TEXT_OFF       0x38    /* idSWFTextInstance -> the live text idStr */
#define TI_LEN_OFF        0x40    /* idSWFTextInstance -> character count (caret/END bound) */
#define TI_SELSTART_OFF   0x124   /* selection anchor (== caret unless shift-selecting) */
#define TI_SELEND_OFF     0x128   /* caret / selection end */

/* idStr.data@+0x10 is always a pointer, including short strings whose data
 * points to self+0x1C. Do not substitute a length-based inline-data branch. */
#define IDSTR_LEN_OFF     0x08    /* int len (chars, excl NUL) */
#define IDSTR_DATA_OFF    0x10    /* char* data -- always a pointer (inline base buffer @ +0x1c, or heap) */

/* idSWFScriptValue stores type@+0, payload@+8, with a 0x10-byte stride. */
#define SWFV_STRIDE       0x10
#define SWFV_TYPE_OFF     0x00
#define SWFV_VAL_OFF      0x08
#define SWFV_T_DOUBLE     4
#define SWFV_T_INT        5
#define SWFV_T_BOOL       7

/* DirectInput scancodes. */
/* Read Ctrl from the OS when C/V arrives; this stream can miss modifier releases. */
#define SC_C              0x2e
#define SC_V              0x2f


#define TI_MULTILINE_OFF  0x140   /* nonzero = ENTER inserts a newline (gates the stock 0x1c/0x9c case) */
#define TI_MAXCHARS_OFF   0x280   /* character cap; the stock focus path truncates when text exceeds it */

/* idStr size -- +0x1c inline base buffer + STR_ALLOC_BASE(20), matching the ctor's 0x80000014 flags. */
#define IDSTR_SIZE        0x30
#define IDSTR_FLAGS_OFF   0x18

/* Refuse implausibly large text-field reads. */
#define TEXT_SANE_MAX     (256 * 1024)
/* Clipboard read cap. Heap-allocated (see paste_at_selection) -- never put this on the hook's stack. */
#define CLIP_MAX          (64 * 1024)

/* The handler also receives non-text objects. Its type check compares an interned
 * pointer, so our own "TextField" literal would fail. Decode the engine pointer
 * from MOV RDX,[RIP+disp32] at onKey::Call+0x4B and validate the opcode first. */
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

/* Proceed only when the engine confirms the script object is a TextField. */
static int is_textfield(void *obj)
{
    if (obj == NULL || g_textfield_id == NULL) return 0;
    void **vtbl = *(void ***)obj;
    if (vtbl == NULL) return 0;
    is_type_fn is_type = (is_type_fn)vtbl[SWFOBJ_ISTYPE_SLOT];
    if (is_type == NULL) return 0;
    return is_type(obj, g_textfield_id) != 0;
}

/* Convert expected numeric script types; unsupported types return 0. */
static int swfv_int(const uint8_t *v, int64_t *out)
{
    int32_t t = *(const int32_t *)(v + SWFV_TYPE_OFF);
    const void *p = v + SWFV_VAL_OFF;
    if (t == SWFV_T_INT)    { *out = *(const int64_t *)p; return 1; }
    if (t == SWFV_T_DOUBLE) { *out = (int64_t)*(const double *)p; return 1; }
    if (t == SWFV_T_BOOL)   { *out = (*(const char *)p != 0); return 1; }
    return 0;
}

/* Read bounded idStr data through its pointer, including short strings. */
static int idstr_view(const uint8_t *s, const char **out_data, int *out_len)
{
    int len = *(const int *)(s + IDSTR_LEN_OFF);
    if (len < 0 || len > TEXT_SANE_MAX) return 0;
    *out_data = *(const char *const *)(s + IDSTR_DATA_OFF);
    *out_len = len;
    return (*out_data != NULL);
}

/* Copy the selection, or the whole field when empty. Return clipboard success. */
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
    /* Bound selection indices by the string actually being read. */
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

/* Replace the selection and collapse the caret after inserted text. Assignment
 * also updates TI_LEN_OFF, which is the same word as the idStr length. Numeric
 * fields retain the editor's normal commit-time validation. */
static int paste_at_selection(const uint8_t *ti)
{
    if (g_idstr_assign == NULL) return 0;

    /* Keep the large clipboard buffer off the engine thread's stack. */
    char *clip = (char *)HeapAlloc(GetProcessHeap(), 0, CLIP_MAX);
    if (clip == NULL) return 0;
    if (!sh_clipboard_get(clip, (int)CLIP_MAX)) { HeapFree(GetProcessHeap(), 0, clip); return 0; }

    /* Drop CR. Multiline fields keep LF; single-line fields collapse internal
     * newline runs to one space and remove leading/trailing runs. */
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

    /* Nonpositive maxchars means no cap. */
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

    /* Zero flags prevent idStr assignment from stealing this borrowed buffer. */
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

/* Query physical Ctrl state for each chord. SWF focus changes can lose key-up
 * events, so retaining a modifier latch would turn later bare C/V into shortcuts. */
static int ctrl_is_held(void)
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

/* Handle clipboard chords, then always chain to the stock handler. */
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
        /* Continue stock input handling if clipboard access faults. */
    }
    return g_orig_onkey(self, retbuf, thisObject, parms);
}

void sh_swf_textedit_install(const uint8_t *module_base)
{
    if (g_orig_onkey) {
        if (sh_detour_is_installed((void *)g_orig_onkey)) return;
        if (!sh_uninstall_detour((void *)g_orig_onkey)) return;
        g_orig_onkey = NULL;
    }
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

    /* Decode the interned type pointer before any patch. Without it the type
     * check cannot succeed, so leave the handler untouched. */
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

    void *tramp = sh_prepare_detour_sig(&r, (void *)swf_onkey_detour, SWF_ONKEY_STOLEN);
    if (tramp == NULL) {
        backend_log("swf-textedit: NOT armed (onKey detour install refused/failed)");
        return;
    }
    g_orig_onkey = (onkey_fn)tramp;

    /* Paste requires a separate assignment signature; copy can work without it. */
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "IdStrAssignFromStr") != 0) continue;
        sig_result ra;
        sig_status st = sig_resolve_one(module_base, &BACKEND_ENGINE_SIGNATURES[i], &ra);
        if (st == SIG_OK || st == SIG_OK_HOOKED) g_idstr_assign = (idstr_assign_fn)ra.addr;
        break;
    }

    if (sh_commit_detour(tramp) != B2_PATCH_OK) {
        if (sh_uninstall_detour(tramp)) g_orig_onkey = NULL;
        backend_log("swf-textedit: commit failed; retained callbacks require restoration");
        return;
    }
    backend_log(g_idstr_assign != NULL
                ? "swf-textedit: ready (Ctrl+C copies / Ctrl+V pastes the focused SWF text field)"
                : "swf-textedit: ready, COPY ONLY (idStr assign unresolved -- Ctrl+V disabled)");
}

void sh_swf_textedit_uninstall(void)
{
    if (g_orig_onkey == NULL) return;
    if (sh_uninstall_detour((void *)g_orig_onkey)) g_orig_onkey = NULL;
    else backend_log("swf-textedit: restore failed; original callback retained");
}
