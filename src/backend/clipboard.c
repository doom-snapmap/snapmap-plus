/* Win32 CF_TEXT clipboard access for console commands. */
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include "clipboard.h"

/* This module owns the user32 dependency for clipboard access. */
#pragma comment(lib, "user32.lib")

int sh_clipboard_set(const char *text)
{
    if (text == NULL) return 0;

    int copied = 0;
    __try {
        if (!OpenClipboard(NULL)) return 0;

        size_t  n    = strlen(text) + 1;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n);
        if (hMem != NULL) {
            void *dst = GlobalLock(hMem);
            if (dst != NULL) {
                memcpy(dst, text, n);
                GlobalUnlock(hMem);
                EmptyClipboard();
                if (SetClipboardData(CF_TEXT, hMem) != NULL) {
                    copied = 1;                     /* The clipboard now owns hMem. */
                } else {
                    GlobalFree(hMem);               /* Reclaim the handle if ownership did not transfer. */
                }
            } else {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();
    } __except (EXCEPTION_EXECUTE_HANDLER) {

        copied = 0;
    }
    return copied;
}

/* Copy clipboard-owned CF_TEXT without taking ownership of its handle. */
int sh_clipboard_get(char *out, int cap)
{
    if (out == NULL || cap <= 0) return 0;

    int got = 0;
    __try {
        out[0] = '\0';
        if (!OpenClipboard(NULL)) return 0;

        HANDLE h = GetClipboardData(CF_TEXT);
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

        out[0] = '\0';
        got = 0;
    }
    return got;
}
