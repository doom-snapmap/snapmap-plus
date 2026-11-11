/* modlock.h -- the named locks on a mods folder, shared by every process that
 * writes it: the game (modstore.c, modbundle.c) and the store tool, which
 * compiles the same sources.
 *
 * Two locks per mods folder:
 *   store   every write under mods\ (install, download, scan, build, New mod,
 *           export, conversion): modstore_lock (modstore.h)
 *   record  mods\snapmap-plus.json, read-modify-write of one section:
 *           sh_mb_record_lock (modbundle.h)
 * Order: a thread that needs both takes the store lock first.
 *
 * Name: L"Local\\snapmap-plus-<kind>-<16 lowercase hex>", the hex the 64-bit
 * FNV-1a of the mods folder's full UTF-8 path with ASCII letters lower-cased,
 * '/' read as '\\', no "\\?\" prefix and no trailing '\\'. Two processes on
 * one mods folder therefore share the lock; a test's scratch folder has its
 * own.
 *
 * Protocol: a Win32 mutex (CreateMutexW, not initially owned), taken with
 * WaitForSingleObject and released with ReleaseMutex by the thread that took
 * it; recursive per thread. WAIT_ABANDONED (the holder's process ended) counts
 * as taken: every write under the lock is a temporary file and a rename, so
 * nothing is left half written.
 */
#ifndef SNAPMAP_PLUS_MODLOCK_H
#define SNAPMAP_PLUS_MODLOCK_H

#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>

#define SH_MODLOCK_STORE  L"store"
#define SH_MODLOCK_RECORD L"record"
#define SH_MODLOCK_NAME_MAX 64

static __inline unsigned long long sh_modlock_key(const char *root)
{
    unsigned long long h = 1469598103934665603ull;
    size_t n;
    if (!root) root = "";
    if (!strncmp(root, "\\\\?\\", 4)) root += 4;
    n = strlen(root);
    while (n && (root[n - 1] == '\\' || root[n - 1] == '/')) n--;
    for (; n; n--, root++) {
        unsigned char c = (unsigned char)*root;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (c == '/') c = '\\';
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

static __inline void sh_modlock_name(const wchar_t *kind, const char *root, wchar_t out[SH_MODLOCK_NAME_MAX])
{
    _snwprintf_s(out, SH_MODLOCK_NAME_MAX, _TRUNCATE, L"Local\\snapmap-plus-%s-%016llx", kind, sh_modlock_key(root));
}

/* The mutex of `kind` for `root`, opened once and kept for the process (each
 * including file keeps its own handles); NULL when Windows cannot create it
 * (out of handles). */
static __inline HANDLE sh_modlock_open(const wchar_t *kind, const char *root)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static struct { unsigned long long key; HANDLE h; } open[32];
    static unsigned next;
    unsigned long long key = sh_modlock_key(root);
    wchar_t name[SH_MODLOCK_NAME_MAX];
    HANDLE h = NULL;
    unsigned i;
    AcquireSRWLockExclusive(&lock);
    for (i = 0; i < 32 && !h; i++) if (open[i].h && open[i].key == key) h = open[i].h;
    if (!h) {
        sh_modlock_name(kind, root, name);
        h = CreateMutexW(NULL, FALSE, name);
        if (h) {
            i = next++ % 32;
            /* Only a test walks more than 32 folders; an older handle is
             * left open, since a thread may still hold it. */
            open[i].key = key;
            open[i].h = h;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return h;
}

#endif /* SNAPMAP_PLUS_MODLOCK_H */
