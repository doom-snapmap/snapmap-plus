/* store_lock.h -- the named lock on a mods folder and its record, shared by every process that writes them.
 * Name: "Local\snapmap-plus-mods-<16 hex>", the 64-bit FNV-1a (offset 0xcbf29ce484222325, prime 0x100000001b3)
 * over the UTF-16LE bytes, low byte first, of the mods folder's GetFullPathNameW with no "\?\" prefix
 * ("\?\UNC\" read as "\\"), no trailing separators, '/' as '\', lowercased by LCMapStringEx
 * (LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE). A writer holds the mutex (WAIT_ABANDONED also grants it) around one
 * whole store step; it is recursive within a thread, and a reader that only lists the folder takes no lock. */
#ifndef SNAPMAP_PLUS_STORE_LOCK_H
#define SNAPMAP_PLUS_STORE_LOCK_H

#include <windows.h>
#include <wchar.h>

#define SH_STORE_LOCK_PREFIX L"Local\\snapmap-plus-mods-"

/* The lock's name for the mods folder `mods` into `out` (at least 48
 * characters). 1 on success. */
static __inline int sh_store_lock_name(const wchar_t *mods, wchar_t *out, size_t cap)
{
    wchar_t full[32768], *p;
    DWORD n;
    int k, i;
    unsigned long long h = 0xcbf29ce484222325ull;
    if (!mods || !out || cap < 48) return 0;
    n = GetFullPathNameW(mods, 32768, full, NULL);
    if (!n || n >= 32768) return 0;
    p = full;
    if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) { p += 6; p[0] = L'\\'; p[1] = L'\\'; }
    else if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    n = (DWORD)wcslen(p);
    for (i = 0; i < (int)n; i++) if (p[i] == L'/') p[i] = L'\\';
    while (n > 3 && p[n - 1] == L'\\') p[--n] = 0;
    k = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, p, (int)n, p, (int)n, NULL, NULL, 0);
    if (k != (int)n) return 0;
    for (i = 0; i < (int)n; i++) {
        h ^= (unsigned char)(p[i] & 0xff);
        h *= 0x100000001b3ull;
        h ^= (unsigned char)((p[i] >> 8) & 0xff);
        h *= 0x100000001b3ull;
    }
    return _snwprintf_s(out, cap, _TRUNCATE, SH_STORE_LOCK_PREFIX L"%016llx", h) > 0;
}

#endif /* SNAPMAP_PLUS_STORE_LOCK_H */
