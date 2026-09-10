/* Expose registered cvars to console and startup +cvar lookup.
 * A deferred worker aliases the developer lookup to the full table and marks
 * cvars settable, then repeats briefly while boot registration continues.
 * Engine-memory accesses are guarded because initialization can race the worker. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "cvar_unlock.h"
#include "backend_log.h"
#include "host_image.h"

/* Developer-table alias and cvar flags. */

/* Copy both table headers; return 0 if the boot-time object is not readable. */
static int alias_dev_to_full(uint8_t *cvarSys)
{
    __try {
        memcpy(cvarSys + CVARSYS_DEV_LIST_OFF, cvarSys + CVARSYS_FULL_LIST_OFF, SIZEOF_IDLIST);
        memcpy(cvarSys + CVARSYS_DEV_HASH_OFF, cvarSys + CVARSYS_FULL_HASH_OFF, SIZEOF_IDHASHINDEX);
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
        uint32_t *flags = (uint32_t *)(cvar + CVAR_FLAGS_OFF);
        *flags |= CVAR_FLAG_NOCHEAT;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A later pass retries entries still being initialized. */
    }
}

/* Resolve CmdSystemLea independently of backend bootstrap, then decode its
 * singleton slot. The adjacent cvar-system slot is 0x10 bytes later.
 * A pinned-RVA fallback remains for the encrypted early-boot window. */

/* CmdSystemLea; relative operands are wildcarded. */
static const char *const CVU_CMDSYS_LEA_SIG =
    "40 53 48 83 EC 30 48 8B 0D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 DB 4C 8D 05 ?? ?? ?? ?? "
    "89 5C 24 28 48 8D 15 ?? ?? ?? ?? 48 89 5C 24 20 48 8B 01 FF 50 20 48 8B 0D ?? ?? ?? ?? "
    "4C 8D 0D ?? ?? ?? ??";

#define CVU_RIP_SCAN_WINDOW 0x40   /* mirror B2_RIP_SCAN_WINDOW: first 0x40 bytes of the accessor fn */
#define CVU_CMDSYS_TO_CVARSYS 0x10 /* cvarSys == cmdSystem .data slot + 0x10 (adjacent singletons) */

static const uint8_t *g_cvu_cmdsys_slot = NULL;   /* sig-decoded cmdSystem .data slot, cached (scan runs once) */

/* Parse masked hex bytes; return the byte count or 0 on malformed/oversized input. */
static int cvu_parse_sig(const char *s, uint8_t *bytes, uint8_t *mask, int cap)
{
    int n = 0, hi, lo;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (n >= cap) return 0;
        if (s[0] == '?') {
            bytes[n] = 0; mask[n] = 0;
            s++; if (*s == '?') s++;
        } else {
            hi = (s[0]>='0'&&s[0]<='9')?s[0]-'0':(s[0]>='A'&&s[0]<='F')?s[0]-'A'+10:(s[0]>='a'&&s[0]<='f')?s[0]-'a'+10:-1;
            lo = (s[1]>='0'&&s[1]<='9')?s[1]-'0':(s[1]>='A'&&s[1]<='F')?s[1]-'A'+10:(s[1]>='a'&&s[1]<='f')?s[1]-'a'+10:-1;
            if (hi < 0 || lo < 0) return 0;
            bytes[n] = (uint8_t)((hi << 4) | lo); mask[n] = 1;
            s += 2;
        }
        n++;
    }
    return n;
}

/* Read the PE .text span; return 0 if headers are invalid or unreadable. */
static int cvu_find_text(const uint8_t *base, const uint8_t **out_start, size_t *out_size)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (memcmp(sec[i].Name, ".text", 5) == 0) {
                *out_start = base + sec[i].VirtualAddress;
                *out_size  = sec[i].Misc.VirtualSize;
                return 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}

/* Return the unique masked match, or NULL if absent or ambiguous. */
static const uint8_t *cvu_scan_unique(const uint8_t *hay, size_t haylen,
                                      const uint8_t *bytes, const uint8_t *mask, int len)
{
    if (len <= 0 || (size_t)len > haylen) return NULL;
    const uint8_t *hit = NULL;
    size_t last = haylen - (size_t)len;
    for (size_t i = 0; i <= last; i++) {
        int ok = 1;
        for (int j = 0; j < len; j++)
            if (mask[j] && hay[i + j] != bytes[j]) { ok = 0; break; }
        if (ok) {
            if (hit) return NULL;   /* ambiguous -> refuse rather than guess */
            hit = hay + i;
        }
    }
    return hit;
}

/* Decode the first supported RIP-relative MOV/LEA within the accessor window. */
static const uint8_t *cvu_decode_rip_slot(const uint8_t *fn)
{
    uint8_t b[CVU_RIP_SCAN_WINDOW];
    __try { memcpy(b, fn, sizeof b); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    for (int i = 0; i + 7 <= CVU_RIP_SCAN_WINDOW; i++) {
        if (b[i] == 0x48 && (b[i+1] == 0x8B || b[i+1] == 0x8D) && (b[i+2] == 0x0D || b[i+2] == 0x05)) {
            int32_t disp;
            memcpy(&disp, &b[i+3], 4);
            return fn + i + 7 + disp;
        }
    }
    return NULL;
}

/* Decode and cache the slot, then read cvarSys. A cached slot with no object
 * means engine construction is pending; a missing slot means resolution failed. */
static uint8_t *cvu_resolve_cvarsys_portable(const uint8_t *base)
{
    if (g_cvu_cmdsys_slot == NULL) {
        const uint8_t *text; size_t tsize;
        if (!cvu_find_text(base, &text, &tsize)) return NULL;
        uint8_t bytes[96], mask[96];
        int len = cvu_parse_sig(CVU_CMDSYS_LEA_SIG, bytes, mask, (int)sizeof bytes);
        if (len <= 0) return NULL;
        const uint8_t *fn = cvu_scan_unique(text, tsize, bytes, mask, len);
        if (!fn) return NULL;
        const uint8_t *slot = cvu_decode_rip_slot(fn);
        if (!slot) return NULL;
        g_cvu_cmdsys_slot = slot;
        backend_log("B2: cvar-unlock CmdSystemLea sig RESOLVED -> portable cvarSys path live (SteamStub .text decrypted)");
    }
    uint8_t *cvarSys = NULL;
    __try { cvarSys = *(uint8_t **)(g_cvu_cmdsys_slot + CVU_CMDSYS_TO_CVARSYS); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return cvarSys;
}

/* Apply the alias and flags once the cvar list looks populated. Return 0 to retry.
 * Only engine data is written; each memory access is guarded against faults. */
static int apply_unlock(uint8_t *base)
{
    /* SteamStub may still hide .text during boot. Try the signature first, then
     * the Vulkan-name-gated RVA while it is unavailable. */
    uint8_t *cvarSys = cvu_resolve_cvarsys_portable(base);
    if (cvarSys == NULL && g_cvu_cmdsys_slot == NULL && sh_host_is_pinned_rva_build()) {
        /* The gate excludes OpenGL by name; it does not verify the pinned build hash. */
        __try {
            cvarSys = *(uint8_t **)(base + RVA_CVAR_SYSTEM_PTR);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
    if (cvarSys == NULL)
        return 0;

    /* Check the array and count before writing. These plausibility checks reject
     * uninitialized data but do not establish exact build identity. */
    void   **arr   = NULL;
    uint32_t count = 0;
    __try {
        arr   = *(void ***)(cvarSys + CVARSYS_LIST_ARRAY_OFF);
        count = *(uint32_t *)(cvarSys + CVARSYS_LIST_COUNT_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (arr == NULL || count == 0 || count > CVAR_LIST_SANITY_MAX)
        return 0;

    /* Alias developer lookup to the full registered-cvar table. */
    if (!alias_dev_to_full(cvarSys))
        return 0;

    /* NOCHEAT permits setting cvars through the developer gate. */
    for (uint32_t i = 0; i < count; i++) {
        void *cvar_v = NULL;
        __try { cvar_v = arr[i]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; } /* Registration may reallocate the array during this walk. */
        set_settable_one(cvar_v);
    }
    return 1;
}

/* Wait for construction, then refresh the alias while boot registration continues. */
static DWORD WINAPI unlock_thread(LPVOID param)
{
    (void)param;
    uint8_t *base = (uint8_t *)sh_host_image_base();

    /* Wait up to about 60 seconds for the cvar system. */
    int applied = 0;
    for (int i = 0; i < 6000 && !applied; i++) {
        if (base == NULL)
            base = (uint8_t *)sh_host_image_base();
        if (base != NULL)
            applied = apply_unlock(base);
        if (!applied)
            Sleep(10);
    }
    if (!applied)
        return 0;
    backend_log("B2: cvar-unlock APPLIED -- all cvars findable+settable (dev-table alias + NOCHEAT); +cvar launch options now apply");

    /* Refresh for about ten seconds; startup +cvar timing is still a polling race. */
    for (int i = 0; i < 200; i++) {
        apply_unlock(base);
        Sleep(50);
    }
    return 0;
}

/* Called from DllMain; the worker waits until after loader initialization to run.
 * Its independent signature scan does not require backend bootstrap to finish. */
void sh_cvar_unlock_start(void)
{
    HANDLE h = CreateThread(NULL, 0, unlock_thread, NULL, 0, NULL);
    if (h != NULL)
        CloseHandle(h);
}
