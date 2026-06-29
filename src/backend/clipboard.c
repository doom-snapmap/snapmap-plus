/* clipboard.c -- see clipboard.h. Clean-room CF_TEXT clipboard-set, port of OG
 * XINPUT1_3 FUN_1800053f0 (clipboard_0x53f0.txt).
 *
 * OG decompile (DIRECT):
 *   if (OpenClipboard(NULL)) {
 *       len = strlen(text);
 *       hMem = GlobalAlloc(GMEM_MOVEABLE, len+1);   // includes NUL
 *       dst  = GlobalLock(hMem);                    // OG does NOT null-check
 *       memcpy(dst, text, len+1);
 *       GlobalUnlock(dst);                          // OG passes the locked ptr
 *       EmptyClipboard();
 *       SetClipboardData(CF_TEXT, hMem);            // ownership transfers to the clipboard
 *       CloseClipboard();
 *   }
 *
 * Hardening over OG (faithful + safe -- clipboard_0x53f0.txt clean-room notes):
 *   - guard GlobalAlloc==NULL and GlobalLock==NULL (OG checks neither);
 *   - on SetClipboardData failure GlobalFree(hMem) to avoid the leak OG would have;
 *   - on SUCCESS do NOT free (the clipboard owns hMem);
 *   - pass the HANDLE (not the locked ptr) to GlobalUnlock (Win32-correct/portable);
 *   - the whole body is SEH-guarded so a clipboard fault can never take down the editor.
 *
 * Clean-room: ported from our own RE. Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include "clipboard.h"

/* The clipboard APIs (OpenClipboard/EmptyClipboard/SetClipboardData/CloseClipboard) live in user32.lib;
 * GlobalAlloc/Lock/Unlock/Free are kernel32 (auto-linked). Pull user32 in from the module that needs it
 * so build.ps1 stays generic. */
#pragma comment(lib, "user32.lib")

int sh_clipboard_set(const char *text)
{
    if (text == NULL) return 0;

    int copied = 0;
    __try {
        if (!OpenClipboard(NULL)) return 0;

        size_t  n    = strlen(text) + 1;            /* include the terminating NUL */
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n);
        if (hMem != NULL) {
            void *dst = GlobalLock(hMem);
            if (dst != NULL) {
                memcpy(dst, text, n);
                GlobalUnlock(hMem);                 /* unlock the HANDLE, not the ptr */
                EmptyClipboard();                   /* clears the prior owner (post-Open) */
                if (SetClipboardData(CF_TEXT, hMem) != NULL) {
                    copied = 1;                     /* SUCCESS: clipboard now owns hMem */
                } else {
                    GlobalFree(hMem);               /* failure: reclaim the handle (OG leaks) */
                }
            } else {
                GlobalFree(hMem);                   /* lock failed: nothing was placed */
            }
        }
        CloseClipboard();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A clipboard fault must never destabilize the editor. */
        copied = 0;
    }
    return copied;
}

/* CF_TEXT GET -- the inverse of sh_clipboard_set. sh_spawninfo runs the engine `getviewpos` console
 * command (which writes the view pos/orientation to the clipboard) then reads it back with this. The
 * GetClipboardData handle is OWNED BY THE CLIPBOARD -- we GlobalLock/copy/GlobalUnlock but never free
 * it. Hardened: guards every Win32 step, truncates to cap-1 + always NUL-terminates, whole body SEH-
 * guarded. */
int sh_clipboard_get(char *out, int cap)
{
    if (out == NULL || cap <= 0) return 0;

    int got = 0;
    __try {
        out[0] = '\0';
        if (!OpenClipboard(NULL)) return 0;

        HANDLE h = GetClipboardData(CF_TEXT);   /* clipboard-owned; do NOT free */
        if (h != NULL) {
            const char *src = (const char *)GlobalLock(h);
            if (src != NULL) {
                int i = 0;
                for (; i < cap - 1 && src[i] != '\0'; i++) out[i] = src[i];
                out[i] = '\0';
                GlobalUnlock(h);
                got = 1;
            }
        }
        CloseClipboard();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A clipboard fault must never destabilize the editor. */
        out[0] = '\0';
        got = 0;
    }
    return got;
}
