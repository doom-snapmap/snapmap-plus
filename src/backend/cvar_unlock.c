/* cvar_unlock.c -- clean-room cvar-unlock, MERGED INTO THE BACKEND XINPUT1_3.
 * Was the standalone dinput8.dll proxy; the cvar-unlock logic now rides in the backend DLL
 * (one fewer shipped file + no System32 dinput8 shadow). The dinput8 runtime-forwarder is
 * dropped -- DOOM loads the real System32 dinput8 directly. Logic is otherwise verbatim.
 *
 * cvar_unlock.c -- clean-room native cvar-unlock for our open-source SnapHak.
 *
 * WHAT THIS IS
 *   The open-source, clean-room replacement for the effect DoomLegacyMod (DLM) provides as
 *   dinput8.dll: it "opens the cvar list" so DOOM 2016 honors `+<cvar> <value>` LAUNCH OPTIONS at
 *   startup (and the ~console recognizes production cvars). Loaded as a pre-main proxy DLL so the
 *   unlock is in place BEFORE the engine's startup +cvar apply runs.
 *
 * WHY LAUNCH OPTIONS NEED THIS (the bug it fixes)
 *   The engine resolves a startup `+cvar` at the DEVELOPER gate, which reads a cvar table that only
 *   contains CVAR_EXPOSE-flagged cvars. A normal cvar (e.g. hydra_signInWhenOnline) isn't in it, so
 *   FindCvar misses and the launch option is SILENTLY DROPPED. (Proven by our RE: a vanilla boot with
 *   `+hydra_signInWhenOnline 0` on the command line still signs in ONLINE.) DLM fixes this by flagging
 *   every cvar EXPOSE before main; we achieve the same result.
 *
 * SCOPE: ALL LOCKED CVARS, NOT ONE.
 *   The alias points the developer lookup at the FULL table (= every registered cvar); the settability
 *   pass walks the ENTIRE cvar list. hydra_signInWhenOnline was only the test case -- every locked
 *   `+cvar` launch option becomes applicable.
 *
 * CLEAN-ROOM
 *   This is OUR mechanism, live-validated in the reference implementation (unlockCvars + setCvarsSettable,
 *   2026-06-14) and ported here verbatim. It contains ZERO DLM bytes. We mirror DLM's STRATEGY
 *   (expose all cvars before the boot apply); we do NOT copy its IMPLEMENTATION (DLM inline-detours the
 *   registration fn behind a QPC/IAT hook -- see README "Alias vs detour"). The alias is simpler and is
 *   our documented clean-room design.
 *
 * STATUS: MERGED INTO THE BACKEND XINPUT1_3 (2026-06-24). Was a standalone dinput8.dll proxy whose
 *   DllMain forwarded to the real System32 dinput8 + spawned the unlock thread; now the cvar-unlock rides
 *   in the backend DLL and the forwarder is DROPPED (DOOM loads the real System32 dinput8 directly, no
 *   proxy). sh_cvar_unlock_start() spawns the deferred unlock thread from the backend DllMain. The
 *   deferred-init TIMING vs the engine's boot +cvar apply is handled by the thread's poll-then-reassert
 *   window (see unlock_thread); this is the first in-game test of the cvar-unlock.
 *
 * SEH: every engine-memory access (the two table-alias memcpys + the per-cvar flags OR) is SEH-guarded,
 *   matching the house style of the sibling backend modules (cvars.c register_one/verify_one,
 *   unhide.c the per-element flag walk). The thread re-asserts the alias during the early-boot
 *   registration race; a torn/stale idCVar* there becomes a skipped element + retry, never an
 *   unhandled access violation that would take the game down.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "cvar_unlock.h"
#include "backend_log.h"   /* now in the backend: log the resolution path to snaphak_backend.log */

/* ---- the unlock (applied to ALL cvars) ------------------------------------------------------- */

/* SEH-guarded table alias (the two memcpys). Returns 1 if both copies completed, 0 on access violation
 * (a torn/half-constructed cvarSys during the boot race -> caller retries). Matches the house pattern of
 * unhide.c's safe_read_* / cvars.c's register_one. */
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

/* SEH-guarded per-cvar settability OR. The flags write dereferences a per-cvar pointer that, during the
 * early-boot registration race the re-assert thread runs against, may be torn/stale; guard each so a bad
 * pointer becomes a skipped element, never an unhandled AV (mirrors unhide.c's per-element __try). */
static void set_settable_one(void *cvar_v)
{
    __try {
        uint8_t *cvar = (uint8_t *)cvar_v;
        if (cvar == NULL)
            return;
        uint32_t *flags = (uint32_t *)(cvar + CVAR_FLAGS_OFF);
        *flags |= CVAR_FLAG_NOCHEAT;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* unreadable/stale idCVar* during the boot race -> skip; the re-assert pass catches it later */
    }
}

/* ---- PORTABLE cvarSys resolution (the sig layer; portability hardening) ------------------------
 * The standalone dinput8 used to read cvarSys = *(base + RVA_CVAR_SYSTEM_PTR) -- a build-locked literal that
 * SILENTLY no-ops on an RVA-shifted DOOM. We now resolve it the way the backend does (sh_resolve_cvarsys):
 * sig-scan the .text for the CmdSystemLea accessor (the bot_add/bot_remove registrar whose prologue does
 * MOV RCX,[rip+cmdSystem]), decode that RIP-relative load to the cmdSystem .data slot, and cvarSys =
 * *(slot + 0x10) (the two singleton slots are adjacent: cvarSys == cmdSystem + 0x10). The hardcoded
 * RVA_CVAR_SYSTEM_PTR is demoted to a logged fallback. NO backend linkage -- a minimal self-contained PE
 * .text scan + masked match + RIP decode, mirroring signatures.c's resolver for this ONE signature. Pattern
 * + decode opcodes from the backend signature resolver "CmdSystemLea"; on a DOOM patch this is
 * regenerated by the project's signature extractor (the recipe is in engine_layout.h). */

/* CmdSystemLea (engine 0x717a50): 65-byte sig; "??" wildcards the build-volatile rel/RIP operands. */
static const char *const CVU_CMDSYS_LEA_SIG =
    "40 53 48 83 EC 30 48 8B 0D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 DB 4C 8D 05 ?? ?? ?? ?? "
    "89 5C 24 28 48 8D 15 ?? ?? ?? ?? 48 89 5C 24 20 48 8B 01 FF 50 20 48 8B 0D ?? ?? ?? ?? "
    "4C 8D 0D ?? ?? ?? ??";

#define CVU_RIP_SCAN_WINDOW 0x40   /* mirror B2_RIP_SCAN_WINDOW: first 0x40 bytes of the accessor fn */
#define CVU_CMDSYS_TO_CVARSYS 0x10 /* cvarSys == cmdSystem .data slot + 0x10 (adjacent singletons) */

static const uint8_t *g_cvu_cmdsys_slot = NULL;   /* sig-decoded cmdSystem .data slot, cached (scan runs once) */

/* Parse an IDA-style hex sig "40 53 ?? .." -> bytes[]+mask[] (mask 1=fixed, 0=wildcard). Returns the byte
 * count, or 0 on a malformed token / cap overflow. */
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

/* Find the module's .text section [*out_start, *out_size). Returns 1 on success, SEH-guarded. */
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

/* Masked scan of [hay, hay+haylen) for the FIRST AND ONLY match of (bytes,mask,len) -- uniqueness-mandatory
 * like the backend resolver. Returns the match addr, or NULL on 0 or >1 matches. */
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

/* Decode the FIRST RIP-relative MOV/LEA ([rip+disp32]: 48 8B/8D 0D/05) in the accessor's first
 * CVU_RIP_SCAN_WINDOW bytes -> the .data slot. Mirrors backend sh_decode_rip_slot. NULL if none. */
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

/* Resolve cvarSys build-PORTABLY: sig-scan CmdSystemLea -> decode the cmdSystem slot (cached) -> +0x10 ->
 * deref. Returns the cvarSys object pointer, or NULL if the sig path fails OR cvarSys isn't constructed yet
 * (caller distinguishes via g_cvu_cmdsys_slot: set => sig resolved, object pending; clear => sig miss). */
static uint8_t *cvu_resolve_cvarsys_portable(const uint8_t *base)
{
    if (g_cvu_cmdsys_slot == NULL) {
        const uint8_t *text; size_t tsize;
        if (!cvu_find_text(base, &text, &tsize)) return NULL;
        uint8_t bytes[96], mask[96];
        int len = cvu_parse_sig(CVU_CMDSYS_LEA_SIG, bytes, mask, (int)sizeof bytes);
        if (len <= 0) return NULL;
        const uint8_t *fn = cvu_scan_unique(text, tsize, bytes, mask, len);
        if (!fn) return NULL;                                   /* 0 or >1 matches -> sig miss */
        const uint8_t *slot = cvu_decode_rip_slot(fn);
        if (!slot) return NULL;
        g_cvu_cmdsys_slot = slot;                               /* cache -- the .text scan runs ONCE */
        backend_log("B2: cvar-unlock CmdSystemLea sig RESOLVED -> portable cvarSys path live (SteamStub .text decrypted)");
    }
    uint8_t *cvarSys = NULL;
    __try { cvarSys = *(uint8_t **)(g_cvu_cmdsys_slot + CVU_CMDSYS_TO_CVARSYS); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return cvarSys;
}

/* Apply the cvar-unlock once. Returns 1 if applied (the cvar system was up + populated), 0 if the
 * engine isn't ready yet (caller should retry). Touches only DATA (the cvarSys object + idCVar
 * structs are in writable engine memory) -- no code patching, so no VirtualProtect needed.
 * Every engine-memory access is SEH-guarded (see the helpers above): a torn read during the boot
 * registration race degrades to "not ready -> retry" / "skip this element", never a crash. */
static int apply_unlock(uint8_t *base)
{
    /* Resolve cvarSys build-PORTABLY (sig-decode the cmdSystem slot, +0x10). The DOOM .text is
     * SteamStub-ENCRYPTED at load and only decrypts ~2s in, so the CmdSystemLea scan MISSES in the early
     * boot window -> we use the build-locked RVA there. The early RVA is made SAFE by the cvar-count
     * VALIDATION below (a wrong RVA on an RVA-shifted build, before the sig resolves, gives an implausible
     * count -> we bail WITHOUT writing). Once .text decrypts the sig resolves and is preferred (and logs). */
    uint8_t *cvarSys = cvu_resolve_cvarsys_portable(base);
    if (cvarSys == NULL && g_cvu_cmdsys_slot == NULL) {
        __try {
            cvarSys = *(uint8_t **)(base + RVA_CVAR_SYSTEM_PTR);   /* sig not resolved yet -> build-locked RVA */
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
    if (cvarSys == NULL)
        return 0; /* cvar system not constructed yet (sig found the slot, object pending) -> retry */

    /* VALIDATE cvarSys BEFORE any write: read the cvar list (array ptr + count) + sanity-check the count.
     * This guards the alias memcpy below against a garbage cvarSys -- the build-locked RVA on an RVA-shifted
     * build (pre-sig), or a torn read during the boot race -- so we NEVER alias to a wrong address. The same
     * read feeds the settability walk. (Moved AHEAD of the alias: validation-before-write.) */
    void   **arr   = NULL;
    uint32_t count = 0;
    __try {
        arr   = *(void ***)(cvarSys + CVARSYS_LIST_ARRAY_OFF);
        count = *(uint32_t *)(cvarSys + CVARSYS_LIST_COUNT_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (arr == NULL || count == 0 || count > CVAR_LIST_SANITY_MAX)
        return 0; /* list not populated yet / implausible cvarSys (wrong/torn) -> not ready -> retry */

    /* (1) FINDABILITY for ALL cvars -- alias the developer (gate!=0) table onto the full (gate==0) table, so
     *     a gate-1 FindCvar resolves every registered cvar (== the reference implementation unlockCvars). Safe: cvarSys validated. */
    if (!alias_dev_to_full(cvarSys))
        return 0; /* half-constructed cvarSys -> not ready, retry */

    /* (2) SETTABILITY for ALL cvars -- OR NOCHEAT into every cvar's flags so idCVar::Set won't FATAL
     *     ("Attempting to set a developer cvar") when the startup +cvar apply sets them at gate!=0
     *     (== the reference implementation setCvarsSettable). Each per-cvar flags write is SEH-guarded inside set_settable_one. */
    for (uint32_t i = 0; i < count; i++) {
        void *cvar_v = NULL;
        __try { cvar_v = arr[i]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; } /* array tail AV during a torn realloc -> stop */
        set_settable_one(cvar_v);
    }
    return 1;
}

/* Deferred init: the engine constructs its cvar system AFTER the SteamStub unpack + early static init,
 * so we cannot touch it from DllMain. Poll until apply_unlock succeeds, then keep re-applying for a
 * short window so (a) the alias snapshot stays fresh as more cvars register and (b) we cover the boot
 * +cvar apply whenever it lands. Bounded; idempotent. */
static DWORD WINAPI unlock_thread(LPVOID param)
{
    (void)param;
    uint8_t *base = (uint8_t *)GetModuleHandleA(DOOM_MODULE_NAME);

    /* Phase 1: wait for the cvar system to come up (up to ~60s @ 10ms). */
    int applied = 0;
    for (int i = 0; i < 6000 && !applied; i++) {
        if (base == NULL)
            base = (uint8_t *)GetModuleHandleA(DOOM_MODULE_NAME);
        if (base != NULL)
            applied = apply_unlock(base);
        if (!applied)
            Sleep(10);
    }
    if (!applied)
        return 0; /* engine never readied the cvar system -- give up quietly */
    backend_log("B2: cvar-unlock APPLIED -- all cvars findable+settable (dev-table alias + NOCHEAT); +cvar launch options now apply");

    /* Phase 2: re-assert for a short window to keep the developer-table alias snapshot current as
     * late cvars register, and to ensure it is in place before the startup +cvar apply. ~10s @ 50ms.
     * (See README "Timing": if the boot apply ever beats this window, switch to the detour variant.) */
    for (int i = 0; i < 200; i++) {
        apply_unlock(base);
        Sleep(50);
    }
    return 0;
}

/* Backend entry: spin the cvar-unlock onto its own thread (loader-lock-safe CreateThread). Called from
 * the backend DllMain at DLL_PROCESS_ATTACH. unlock_thread is self-contained (its own CmdSystemLea sig
 * scan) so it runs independently of the bootstrap thread; the deferred poll handles the +cvar-apply timing. */
void sh_cvar_unlock_start(void)
{
    HANDLE h = CreateThread(NULL, 0, unlock_thread, NULL, 0, NULL);
    if (h != NULL)
        CloseHandle(h);
}
