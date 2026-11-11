/* store_lock.h -- the named lock on a mods folder and its record, shared by
 * every process that writes them (DECISIONS 19): the game DLL and the
 * installer's store tool include this one header.
 *
 * Name. "Local\snapmap-plus-store-<16 hex>", where the hex is the 64-bit
 * FNV-1a (offset 0xcbf29ce484222325, prime 0x100000001b3) over the UTF-16LE
 * code units, two bytes each, low byte first, of the mods folder's path:
 * GetFullPathNameW of it, without a "\\?\" prefix ("\\?\UNC\" read as "\\"),
 * without trailing separators, every '/' as '\', lowercased with
 * LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE). The data folder's
 * mods\ and a copy of it elsewhere have different locks, so a test on a copy
 * never waits for the player's game.
 *
 * Protocol. A writer opens the mutex (CreateMutexW, not initially owned) and
 * waits for it; WAIT_ABANDONED also gives it (the last holder ended without
 * releasing, and the store's own work-folder recovery puts its leftovers
 * right). It holds it around one whole store step: the game around each
 * install, download, salting, scan build or record write; the store tool
 * around its whole migrate run. The mutex is recursive within a thread.
 * A reader that only lists the folder takes no lock. */
#ifndef SNAPMAP_PLUS_STORE_LOCK_H
#define SNAPMAP_PLUS_STORE_LOCK_H

#include <windows.h>
#include <wchar.h>

#define SH_STORE_LOCK_PREFIX L"Local\\snapmap-plus-store-"

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

/* Take the lock on `mods`, waiting up to `ms` milliseconds (INFINITE: no
 * limit). Returns the held mutex, or NULL when it could not be had in time
 * (GetLastError: WAIT_TIMEOUT) or opened. */
static __inline HANDLE sh_store_lock_take(const wchar_t *mods, DWORD ms)
{
    wchar_t name[64];
    HANDLE m;
    DWORD w;
    if (!sh_store_lock_name(mods, name, 64)) return NULL;
    m = CreateMutexW(NULL, FALSE, name);
    if (!m) return NULL;
    w = WaitForSingleObject(m, ms);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) return m;
    CloseHandle(m);
    SetLastError(WAIT_TIMEOUT);
    return NULL;
}

/* Release a lock sh_store_lock_take returned. */
static __inline void sh_store_lock_give(HANDLE m)
{
    if (!m) return;
    ReleaseMutex(m);
    CloseHandle(m);
}

#endif /* SNAPMAP_PLUS_STORE_LOCK_H */
