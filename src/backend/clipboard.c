/* Win32 Unicode clipboard access for console commands and SWF text fields.
 * Callers speak UTF-8; the clipboard holds CF_UNICODETEXT, so text copied here
 * pastes correctly elsewhere and accented or non-Latin text pastes in. */
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include "clipboard.h"

/* This module owns the user32 dependency for clipboard access. */
#pragma comment(lib, "user32.lib")

int sh_clipboard_set(const char *text)
{
    if (text == NULL) return 0;

    int wide = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wide <= 0) return 0;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wide * sizeof(wchar_t));
    if (mem == NULL) return 0;
    wchar_t *dst = (wchar_t *)GlobalLock(mem);
    if (dst == NULL) { GlobalFree(mem); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, dst, wide);
    GlobalUnlock(mem);

    if (!OpenClipboard(NULL)) { GlobalFree(mem); return 0; }
    int copied = 0;
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, mem) != NULL) copied = 1;   /* the clipboard owns mem now */
    else GlobalFree(mem);
    CloseClipboard();
    return copied;
}

int sh_clipboard_utf8_fit(const char *text, int len, int cap)
{
    if (len <= cap) return len;
    /* Step back over continuation bytes (10xxxxxx) so a character is never cut in two. */
    while (cap > 0 && ((unsigned char)text[cap] & 0xC0u) == 0x80u) cap--;
    return cap;
}

/* Copy clipboard-owned CF_UNICODETEXT as UTF-8 without taking ownership of its handle. */
int sh_clipboard_get(char *out, int cap)
{
    if (out == NULL || cap <= 0) return 0;
    out[0] = '\0';
    if (!OpenClipboard(NULL)) return 0;

    int got = 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *src = h ? (const wchar_t *)GlobalLock(h) : NULL;
    if (src != NULL) {
        /* The handle's size bounds the read even if the text lacks its terminator. */
        size_t chars = GlobalSize(h) / sizeof(wchar_t);
        size_t n = 0;
        while (n < chars && src[n]) n++;
        int need = n ? WideCharToMultiByte(CP_UTF8, 0, src, (int)n, NULL, 0, NULL, NULL) : 0;
        char *all = need > 0 ? (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need) : NULL;
        if (all) {
            WideCharToMultiByte(CP_UTF8, 0, src, (int)n, all, need, NULL, NULL);
            int keep = sh_clipboard_utf8_fit(all, need, cap - 1);
            memcpy(out, all, (size_t)keep);
            out[keep] = '\0';
            HeapFree(GetProcessHeap(), 0, all);
            got = 1;
        } else if (n == 0) {
            got = 1;                                    /* empty text is still text */
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return got;
}
