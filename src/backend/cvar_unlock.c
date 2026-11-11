/* Expose registered cvars to console and startup +cvar lookup.
 * A deferred worker aliases the developer lookup to the full table and marks
 * cvars settable. It aliases only after the engine has finished registering
 * cvars, because registration writes both tables and grows the full one.
 * Engine-memory accesses are guarded because initialization can race the worker. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "engine_build.h"
#include "cvar_unlock.h"
#include "backend_log.h"
#include "host_image.h"
#include "engine_globals.h"

/* Registration and the developer-table alias.
 *
 * idCVarSystemLocal::RegisterStaticVars (VK 0x1402953a0, GL 0x1402955c0) is
 * the only code that writes either table. Init calls it once on the main
 * thread and runs the first startup +cvar pass (VK 0x14156f9a0) right after
 * it. It walks the static chain from its head through idCVar +0x90, appending
 * each cvar to the full hash and list and, when the cvar carries
 * CVAR_FLAG_EXPOSE, to the developer hash and list. The lists and the hash's
 * index chain grow by reallocation, freeing the old arrays.
 *
 * Aliasing while that loop ran left the developer header pointing at arrays
 * the full table then freed, and let the loop's developer appends write
 * through the copied header into the full table's arrays. The +cvar pass,
 * which looks names up in the developer table, then read freed memory and
 * faulted in idCVarSystem::Find (snaphak-re boot-time-13954591 R-004). */

typedef enum { CVU_REG_NOT_STARTED, CVU_REG_RUNNING, CVU_REG_DONE } cvu_reg_state;

/* Read where registration stands; *out_count and *out_chain receive the full
 * list's count and the registered chain's length. The full list's first entry
 * is the chain head registration started from; cvars constructed later are
 * linked in front of it and never registered. Registration is done once the
 * full list holds that whole chain, ending with its tail, and the developer
 * list holds every exposed chain entry, ending with the last one. Each count
 * is stored after its entry, so a final count means the appends are visible. */
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

/* Copy the full table's headers over the developer table's; return 0 if the
 * object is unreadable or the full hash is smaller than the developer hash.
 * The main thread may be in idCVarSystem::Find (VK 0x1402943f0) meanwhile: it
 * reads the hash mask, the bucket array, the list, then the index chain and
 * the list again, without a lock. Each field is one aligned 8-byte store, in
 * an order that keeps every mix of old and new fields in bounds: the list
 * before its count, and the list and index chain before the buckets that hold
 * full-table indices, then the masks. Old masks stay in bounds of the new
 * bucket array because it is at least as large. Unchanged headers are left. */
static int alias_dev_to_full(uint8_t *cvarSys)
{
    __try {
        uint8_t *dl = cvarSys + CVARSYS_DEV_LIST_OFF, *dh = cvarSys + CVARSYS_DEV_HASH_OFF;
        const uint8_t *fl = cvarSys + CVARSYS_FULL_LIST_OFF, *fh = cvarSys + CVARSYS_FULL_HASH_OFF;
        if (!memcmp(dl, fl, SIZEOF_IDLIST) && !memcmp(dh, fh, SIZEOF_IDHASHINDEX))
            return 1;
        if (*(const int32_t *)(fh + IDHASH_HASH_SIZE_OFF) < *(const int32_t *)(dh + IDHASH_HASH_SIZE_OFF))
            return 0;
        store64(dl, fl, 0x00);   /* list array */
        store64(dl, fl, 0x08);   /* count, capacity */
        store64(dl, fl, 0x10);   /* granularity, tag, ownership */
        store64(dh, fh, 0x08);   /* index chain */
        store64(dh, fh, 0x10);   /* hash size, index size */
        store64(dh, fh, 0x00);   /* buckets */
        store64(dh, fh, 0x18);   /* granularity, hash mask */
        store64(dh, fh, 0x20);   /* lookup mask, tag */
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
