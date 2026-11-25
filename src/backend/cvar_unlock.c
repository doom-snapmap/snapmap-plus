/* Exposes registered cvars to console and startup +cvar lookup: a deferred worker aliases the developer
 * lookup to the full table and marks cvars settable, only after engine registration has finished. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "engine_build.h"
#include "cvar_unlock.h"
#include "backend_log.h"
#include "host_image.h"
#include "engine_globals.h"

/* idCVarSystemLocal::RegisterStaticVars (VK 0x1402953a0, GL 0x1402955c0) is the only writer of either
 * table; it walks the static chain (idCVar +0x90), appending each cvar to the full hash and list and,
 * with CVAR_FLAG_EXPOSE, to the developer ones, reallocating and freeing the arrays as they grow.
 * Aliasing mid-loop would leave the developer header on freed arrays, so the alias waits for it. */

typedef enum { CVU_REG_NOT_STARTED, CVU_REG_RUNNING, CVU_REG_DONE } cvu_reg_state;

/* Reads where registration stands; *out_count and *out_chain receive the full list's count and the
 * chain length from its first entry (later-constructed cvars link in front and are never registered).
 * Done means both lists hold their whole chain; counts are stored after entries, so a final count is visible. */
static cvu_reg_state cvu_registration_state(const uint8_t *cvarSys, uint32_t *out_count, uint32_t *out_chain)
{
    *out_count = 0;
    *out_chain = 0;
    __try {
        void *const *full = *(void *const *const *)(cvarSys + CVARSYS_FULL_LIST_OFF);
        uint32_t n = *(const uint32_t *)(cvarSys + CVARSYS_FULL_LIST_OFF + IDLIST_NUM_OFF);
        void *const *dev = *(void *const *const *)(cvarSys + CVARSYS_DEV_LIST_OFF);
        uint32_t dn = *(const uint32_t *)(cvarSys + CVARSYS_DEV_LIST_OFF + IDLIST_NUM_OFF);
        const uint8_t *node, *tail = NULL, *last_exposed = NULL;
        uint32_t len = 0, exposed = 0;
        if (full == NULL || n == 0 || n > CVAR_LIST_SANITY_MAX)
            return CVU_REG_NOT_STARTED;
        *out_count = n;
        for (node = (const uint8_t *)full[0]; node != NULL; node = *(const uint8_t *const *)(node + SH_IDCVAR_NEXT)) {
            if (++len > CVAR_LIST_SANITY_MAX)
                return CVU_REG_NOT_STARTED;
            tail = node;
            if (*(const uint32_t *)(node + SH_IDCVAR_FLAGS) & CVAR_FLAG_EXPOSE) {
                exposed++;
                last_exposed = node;
            }
        }
        *out_chain = len;
        if (len != n || full[n - 1] != tail)
            return CVU_REG_RUNNING;
        /* Once aliased the developer header is the full one. */
        if (dev == full && dn == n)
            return CVU_REG_DONE;
        if (dn != exposed || (exposed != 0 && (dev == NULL || dev[dn - 1] != last_exposed)))
            return CVU_REG_RUNNING;
        return CVU_REG_DONE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return CVU_REG_NOT_STARTED;
    }
}

static void store64(uint8_t *dst, const uint8_t *src, size_t off)
{
    *(volatile uint64_t *)(dst + off) = *(const volatile uint64_t *)(src + off);
    MemoryBarrier();
}

/* Copies the full table's headers over the developer table's; returns 0 if unreadable or the full hash
 * is smaller. idCVarSystem::Find (VK 0x1402943f0) reads them lock-free meanwhile, so each field is one
 * aligned 8-byte store in an order keeping every old/new mix in bounds: list before count, list and
 * index chain before buckets, then masks (old masks fit the larger new bucket array). */
static int alias_dev_to_full(uint8_t *cvarSys)
{
    __try {
        uint8_t *dl = cvarSys + CVARSYS_DEV_LIST_OFF, *dh = cvarSys + CVARSYS_DEV_HASH_OFF;
        const uint8_t *fl = cvarSys + CVARSYS_FULL_LIST_OFF, *fh = cvarSys + CVARSYS_FULL_HASH_OFF;
        if (!memcmp(dl, fl, SIZEOF_IDLIST) && !memcmp(dh, fh, SIZEOF_IDHASHINDEX))
            return 1;
        if (*(const int32_t *)(fh + IDHASH_HASH_SIZE_OFF) < *(const int32_t *)(dh + IDHASH_HASH_SIZE_OFF))
            return 0;
        /* List (array, count), then index chain, then buckets, then masks. */
        store64(dl, fl, 0x00);
        store64(dl, fl, 0x08);
        store64(dl, fl, 0x10);
        store64(dh, fh, 0x08);
        store64(dh, fh, 0x10);
        store64(dh, fh, 0x00);
        store64(dh, fh, 0x18);
        store64(dh, fh, 0x20);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Guard each flags write because registration can replace cvar pointers. */
static void set_settable_one(void *cvar_v)
{
    __try {
        uint8_t *cvar = (uint8_t *)cvar_v;
        if (cvar == NULL)
            return;
        /* Atomic: the main thread may write the same flags word meanwhile. */
        volatile LONG *flags = (volatile LONG *)(cvar + SH_IDCVAR_FLAGS);
        if (!(*flags & CVAR_FLAG_NOCHEAT)) InterlockedOr(flags, CVAR_FLAG_NOCHEAT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A later pass retries entries still being initialized. */
    }
}


/* What the worker saw before its first alias, for the APPLIED log line. */
static int      g_cvu_aliased = 0;
static uint32_t g_cvu_running_polls = 0, g_cvu_running_first = 0, g_cvu_running_chain = 0;
static uint32_t g_cvu_alias_count = 0, g_cvu_changes = 0;

/* The cvar system through the globals table's anchor, checked at its recorded
 * code site; NULL until the engine has constructed it. */
static uint8_t *cvu_cvarsys(uint8_t *base)
{
    uintptr_t slot = glb_resolve(base, "cvar_system_slot", NULL);
    uint8_t *cvarSys = NULL;
    if (!slot) return NULL;
    __try { cvarSys = *(uint8_t **)slot; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return cvarSys;
}

/* Apply the alias and flags once registration has finished. Return 0 to retry.
 * Only engine data is written; each memory access is guarded against faults. */
static int apply_unlock(uint8_t *cvarSys)
{
    uint32_t count = 0, chain = 0;
    if (cvarSys == NULL)
        return 0;
    cvu_reg_state st = cvu_registration_state(cvarSys, &count, &chain);
    if (!g_cvu_aliased && st == CVU_REG_NOT_STARTED)
        return 0;
    if (!g_cvu_aliased && st == CVU_REG_RUNNING) {
        if (g_cvu_running_polls++ == 0) {
            g_cvu_running_first = count;
            g_cvu_running_chain = chain;
        }
        return 0;
    }

    /* Registration never runs again, so a later difference means some other
     * engine code changed the full table; it is logged and aliased again. */
    __try {
        if (g_cvu_aliased && (memcmp(cvarSys + CVARSYS_DEV_LIST_OFF, cvarSys + CVARSYS_FULL_LIST_OFF, SIZEOF_IDLIST) ||
                              memcmp(cvarSys + CVARSYS_DEV_HASH_OFF, cvarSys + CVARSYS_FULL_HASH_OFF, SIZEOF_IDHASHINDEX))) {
            if (g_cvu_changes++ == 0) {
                /* The product's own late cvars (cvars.c) append in place, within capacity. */
                int moved = memcmp(cvarSys + CVARSYS_DEV_LIST_OFF, cvarSys + CVARSYS_FULL_LIST_OFF, 8) ||
                            memcmp(cvarSys + CVARSYS_DEV_HASH_OFF, cvarSys + CVARSYS_FULL_HASH_OFF, 0x10);
                char msg[200];
                _snprintf_s(msg, sizeof msg, _TRUNCATE,
                            "cvars: unlock full cvar table changed after the alias (%u -> %u cvars, arrays %s); "
                            "aliasing again", g_cvu_alias_count, count, moved ? "moved" : "kept");
                backend_log(msg);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!alias_dev_to_full(cvarSys))
        return 0;
    if (!g_cvu_aliased) {
        g_cvu_aliased = 1;
        g_cvu_alias_count = count;
    }

    /* NOCHEAT permits setting cvars through the developer gate. */
    void **arr = NULL;
    __try {
        arr = *(void ***)(cvarSys + CVARSYS_LIST_ARRAY_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    for (uint32_t i = 0; arr != NULL && i < count; i++) {
        void *cvar_v = NULL;
        __try { cvar_v = arr[i]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        set_settable_one(cvar_v);
    }
    return 1;
}

/* Wait for registration, alias once, then keep cvars settable for a while. */
static DWORD WINAPI unlock_thread(LPVOID param)
{
    (void)param;
    uint8_t *base = (uint8_t *)sh_host_image_base();
    char msg[256];

    /* The developer-table alias writes idCVarSystem fields: only on the supported
     * executable, and only once that layout is established. */
    if (!SH_CVARSYS_LAYOUT_ESTABLISHED) {
        backend_log("cvars: unlock off -- the idCVarSystem layout is not established for "
                    "build 13954591");
        return 0;
    }
    if (base == NULL || !sh_host_is_supported_build())
        return 0;

    /* Wait up to about 60 seconds. Once the object is readable, poll every
     * millisecond so the alias lands soon after registration returns. */
    int applied = 0;
    uint8_t *cvarSys = NULL;
    ULONGLONG deadline = GetTickCount64() + 60000;
    while (!applied && GetTickCount64() < deadline) {
        cvarSys = cvu_cvarsys(base);
        applied = apply_unlock(cvarSys);
        if (!applied)
            Sleep(cvarSys != NULL ? 1 : 10);
    }
    if (!applied) {
        backend_log("cvars: unlock off -- the cvar system was not found within 60 s");
        return 0;
    }
    if (g_cvu_running_polls != 0)
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "cvars: unlock APPLIED after registration (%u cvars); deferred %u polls while "
                  "registration ran, first at %u of %u", g_cvu_alias_count, g_cvu_running_polls,
                  g_cvu_running_first, g_cvu_running_chain);
    else
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "cvars: unlock APPLIED after registration (%u cvars)", g_cvu_alias_count);
    backend_log(msg);

    /* Re-mark NOCHEAT for about ten seconds and watch the full table. */
    for (int i = 0; i < 200; i++) {
        Sleep(50);
        apply_unlock(cvu_cvarsys(base));
    }
    _snprintf_s(msg, sizeof msg, _TRUNCATE, "cvars: unlock settled -- full table %s after the alias",
              g_cvu_changes ? "changed" : "unchanged");
    backend_log(msg);
    return 0;
}

/* Called from DllMain; the worker waits until after loader initialization to run.
 * It does not wait for the backend bootstrap. */
void sh_cvar_unlock_start(void)
{
    HANDLE h = CreateThread(NULL, 0, unlock_thread, NULL, 0, NULL);
    if (h != NULL)
        CloseHandle(h);
}
