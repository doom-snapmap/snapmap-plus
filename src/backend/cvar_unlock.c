/* Expose registered cvars to console and startup +cvar lookup.
 * A deferred worker waits for decrypted code, points every cvar-system gate
 * selection at the full table, then marks cvars settable and repeats the
 * marking briefly while boot registration continues. Engine-memory accesses
 * are guarded because initialization can race the worker. */
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cvar_unlock.h"
#include "backend_log.h"
#include "host_image.h"
#include "patch.h"

/* Cvar flags. */

/* Guard each flags access because registration can replace cvar pointers.
 * Only a missing bit is written, atomically, so no engine flag update is lost. */
static void set_settable_one(void *cvar_v)
{
    __try {
        uint8_t *cvar = (uint8_t *)cvar_v;
        if (cvar == NULL)
            return;
        volatile LONG *flags = (volatile LONG *)(cvar + CVAR_FLAGS_OFF);
        if (!(*flags & CVAR_FLAG_NOCHEAT))
            InterlockedOr(flags, CVAR_FLAG_NOCHEAT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A later pass retries entries still being initialized. */
    }
}

/* Resolve CmdSystemLea independently of backend bootstrap, then decode its
 * singleton slot. The adjacent cvar-system slot is 0x10 bytes later. A match
 * shows SteamStub has decrypted CmdSystemLea; later .text can still be
 * encrypted at that moment. */

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
        backend_log("B2: cvar-unlock CmdSystemLea sig RESOLVED -> portable cvarSys path live (SteamStub has decrypted it)");
    }
    uint8_t *cvarSys = NULL;
    __try { cvarSys = *(uint8_t **)(g_cvu_cmdsys_slot + CVU_CMDSYS_TO_CVARSYS); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return cvarSys;
}

/* Gate selections. */

/* The eleven cvar-system gate selections, identical in both renderer images
 * (OpenGL / Vulkan RVAs noted). Copying the full table's list and hash headers
 * over the developer table raced boot registration: two lists shared one
 * buffer, and a lookup processing startup +cvar options read a NULL entry
 * (OpenGL 0x19f7d7d in FindVar) before the launcher could start Vulkan.
 * Rewriting each selection's one-byte immediate never exposes a shared or
 * half-copied table. FindVar reads its hash selection before its list
 * selection, so rewriting list selections first can pair at worst a developer
 * hash with the full list: a name mismatch, never an index past the smaller
 * developer list. The worker rewrites them once .text is decrypted, before
 * boot registration fills either table, so the one function that reads its
 * list selection first (third and fourth sites) has nothing to index yet. */
typedef struct cvu_gate_site { const char *sig; int imm; uint8_t developer, full; } cvu_gate_site;
static const cvu_gate_site CVU_GATE_SITES[SH_CVAR_UNLOCK_GATE_SITES] = {
    {"FF 50 10 B9 08 00 00 00 41 B9 20 00 00 00 85 C0 44 0F 44 C9", 10,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF},                                              /* 0x19f785b / 0x1a0560b */
    {"FF 50 10 B9 08 00 00 00 41 B8 20 00 00 00 85 C0 44 0F 44 C1 49 03", 10,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF},                                              /* 0x19f79c2 / 0x1a05772 */
    {"FF 50 10 B9 08 00 00 00 BE 20 00 00 00 85 C0 0F 44 F1 48 8B", 9,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF},                                              /* 0x19f7be6 / 0x1a05996 */
    {"FF 50 10 BA 38 00 00 00 41 BE 60 00 00 00 85 C0 44 0F 44 F2", 10,
     CVARSYS_DEV_HASH_OFF, CVARSYS_FULL_HASH_OFF},                                              /* 0x19f7c05 / 0x1a059b5 */
    {"FF 50 10 B9 38 00 00 00 BE 60 00 00 00 85 C0 0F 44 F1 48 8B", 9,
     CVARSYS_DEV_HASH_OFF, CVARSYS_FULL_HASH_OFF},                                              /* 0x19f7d12 / 0x1a05ac2 */
    {"20 BA 08 00 00 00 85 C0 41 BE 20 00 00 00 48 63 46 1C 44 0F", 10,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF},                                              /* 0x19f7d44 / 0x1a05af4 */
    {"FF 50 10 B9 08 00 00 00 41 B8 20 00 00 00 85 C0 44 0F 44 C1 49 8B 04 38 48 8B 04 D8", 10,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF},                                              /* 0x19f7df2 / 0x1a05ba2 */
    {"FF 51 10 B9 08 00 00 00 41 BE 20 00 00 00 85 C0 44 0F 44 F1", 10,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF},                                              /* 0x19f7e5f / 0x1a05c0f */
    {"8B FA 48 8B 01 FF 50 10 BE 20 00 00 00 B9 08 00 00 00 85 C0", 9,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF},                                              /* 0x19f8b1d / 0x1a068dd */
    {"00 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 10 BE 20 00 00 00 B9 08 00 00 00 85 C0", 15,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF},                                              /* 0x19f8eab / 0x1a06c6b */
    {"FF 50 10 B9 08 00 00 00 41 B8 20 00 00 00 85 C0 44 0F 44 C1 49 8B 04 38 48 8B 1C D8", 10,
     CVARSYS_DEV_LIST_OFF, CVARSYS_FULL_LIST_OFF}                                               /* 0x19f8fec / 0x1a06dac */
};

/* Locate every developer immediate in one pass over .text. A site matches
 * with its immediate at either value, so a rewritten selection is still found.
 * Return 1 when every site matches exactly once, 0 when a site is absent, or
 * -1 when one matches more than once; unproven receives each such site's bit. */
static int cvu_gate_targets(const uint8_t *text, size_t size, uint8_t *targets[SH_CVAR_UNLOCK_GATE_SITES],
                            unsigned *unproven)
{
    uint8_t bytes[SH_CVAR_UNLOCK_GATE_SITES][32], mask[SH_CVAR_UNLOCK_GATE_SITES][32];
    int len[SH_CVAR_UNLOCK_GATE_SITES], hits[SH_CVAR_UNLOCK_GATE_SITES] = {0};
    uint16_t first[256] = {0};
    for (int s = 0; s < SH_CVAR_UNLOCK_GATE_SITES; s++) {
        const cvu_gate_site *site = CVU_GATE_SITES + s;
        len[s] = cvu_parse_sig(site->sig, bytes[s], mask[s], (int)sizeof bytes[s]);
        if (len[s] <= site->imm || !mask[s][0] || !mask[s][site->imm] || bytes[s][site->imm] != site->developer) {
            *unproven = (1u << SH_CVAR_UNLOCK_GATE_SITES) - 1;
            return -1;
        }
        mask[s][site->imm] = 0;
        first[bytes[s][0]] |= (uint16_t)(1u << s);
    }
    for (size_t i = 0; i < size; i++) {
        for (unsigned pending = first[text[i]]; pending; pending &= pending - 1) {
            unsigned long s;
            _BitScanForward(&s, pending);
            const cvu_gate_site *site = CVU_GATE_SITES + s;
            if (size - i < (size_t)len[s]) continue;
            int j = 1;
            while (j < len[s] && (!mask[s][j] || text[i + j] == bytes[s][j])) j++;
            if (j < len[s]) continue;
            uint8_t value = text[i + (size_t)site->imm];
            if (value != site->developer && value != site->full) continue;
            hits[s]++;
            targets[s] = (uint8_t *)text + i + site->imm;
        }
    }
    int result = 1;
    for (int s = 0; s < SH_CVAR_UNLOCK_GATE_SITES; s++) {
        if (hits[s] == 1) continue;
        *unproven |= 1u << s;
        result = hits[s] > 1 ? -1 : result < 0 ? -1 : 0;
    }
    return result;
}

int sh_cvar_unlock_rewrite_gates(uint8_t *text, size_t size, unsigned *unproven)
{
    uint8_t *targets[SH_CVAR_UNLOCK_GATE_SITES];
    sh_patch_handle handles[SH_CVAR_UNLOCK_GATE_SITES];
    unsigned ignored;
    int written = 0, found;
    if (unproven == NULL)
        unproven = &ignored;
    *unproven = 0;
    if (text == NULL)
        return 0;
    found = cvu_gate_targets(text, size, targets, unproven);
    if (found <= 0)
        return found;
    memset(handles, 0, sizeof handles);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < SH_CVAR_UNLOCK_GATE_SITES; i++) {
            const cvu_gate_site *site = CVU_GATE_SITES + i;
            if ((site->developer == CVARSYS_DEV_HASH_OFF) != (pass == 1) || *targets[i] == site->full)
                continue;
            if (code_patch(targets[i], &site->developer, &site->full, 1, &handles[written]) != B2_PATCH_OK) {
                /* Undo in reverse, hash selections first, so every step stays a safe pairing. */
                if (handles[written].live)
                    code_unpatch(&handles[written]);
                while (written > 0)
                    code_unpatch(&handles[--written]);
                return -1;
            }
            written++;
        }
    }
    return SH_CVAR_UNLOCK_GATE_SITES;
}

static int g_cvu_gate_scans;

/* Rewrite the gates once CmdSystemLea resolves. Return 1 when done, 0 to
 * retry, or -1 when this build's selections are refused. The cvar code lies
 * well past CmdSystemLea and may still be encrypted, so absent sites stay
 * retryable until registered cvars prove the engine is running decrypted code;
 * a site matching more than once is refused at once. */
static int cvu_rewrite_image_gates(const uint8_t *base)
{
    const uint8_t *text; size_t size;
    uint8_t *cvarSys = cvu_resolve_cvarsys_portable(base);
    uint32_t registered = 0;
    unsigned unproven = 0;
    int rewritten = 0;
    char line[192];
    if (g_cvu_cmdsys_slot == NULL || !cvu_find_text(base, &text, &size))
        return 0;
    if (cvarSys != NULL) {
        __try { registered = *(uint32_t *)(cvarSys + CVARSYS_LIST_COUNT_OFF); }
        __except (EXCEPTION_EXECUTE_HANDLER) { registered = 0; }
    }
    __try { rewritten = sh_cvar_unlock_rewrite_gates((uint8_t *)text, size, &unproven); }
    __except (EXCEPTION_EXECUTE_HANDLER) { rewritten = 0; }
    g_cvu_gate_scans++;
    if (rewritten == 0 && registered == 0)
        return 0;
    if (rewritten != SH_CVAR_UNLOCK_GATE_SITES) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: cvar-unlock gate sites 0x%03x %s after %d scans (%u cvars registered)",
            unproven, rewritten == 0 ? "absent" : "ambiguous or not rewritable", g_cvu_gate_scans, registered);
        backend_log(line);
        return -1;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: cvar-unlock gate selections now use the full table (%d sites after %d scans; %u cvars registered)",
        rewritten, g_cvu_gate_scans, registered);
    backend_log(line);
    return 1;
}

/* Mark cvars settable once the full list looks populated. Return 0 to retry.
 * Only cvar flags are written; each memory access is guarded against faults. */
static int apply_unlock(uint8_t *base)
{
    uint8_t *cvarSys = cvu_resolve_cvarsys_portable(base);
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

    /* NOCHEAT lets the setter accept a cvar while the gate is nonzero.
     * Registration can reallocate the array: read each entry only while it is
     * still the live one, and let the next pass finish a walk it cuts short. */
    for (uint32_t i = 0; i < count; i++) {
        void *cvar_v = NULL, **live = NULL;
        __try { live = *(void ***)(cvarSys + CVARSYS_LIST_ARRAY_OFF); if (live == arr) cvar_v = arr[i]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (live != arr)
            break;
        set_settable_one(cvar_v);
    }
    return 1;
}

/* Wait for decrypted code and the cvar system, then keep marking cvars
 * settable while boot registration continues. */
static DWORD WINAPI unlock_thread(LPVOID param)
{
    (void)param;
    uint8_t *base = (uint8_t *)sh_host_image_base();

    /* Wait up to about 60 seconds for decrypted code and the cvar system. */
    int gates = 0, applied = 0;
    for (int i = 0; i < 6000 && !applied; i++) {
        if (base == NULL)
            base = (uint8_t *)sh_host_image_base();
        if (base != NULL && gates == 0)
            gates = cvu_rewrite_image_gates(base);
        if (gates < 0) {
            backend_log("B2: cvar-unlock REFUSED -- this build's cvar gate selections were not proven; lookups stay native");
            return 0;
        }
        if (gates > 0)
            applied = apply_unlock(base);
        if (!applied)
            Sleep(10);
    }
    if (!applied) {
        backend_log("B2: cvar-unlock NOT APPLIED -- decrypted code or the cvar list did not appear within 60 seconds");
        return 0;
    }
    backend_log("B2: cvar-unlock APPLIED -- all cvars findable+settable (full-table gate selections + NOCHEAT); +cvar launch options now apply");

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
