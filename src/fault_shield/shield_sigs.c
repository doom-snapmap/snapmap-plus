/* shield_sigs.c -- the fault-shield's engine-function sig DB + the resolve-at-install step.
 *
 * Reuses the backend's self-contained resolver (sig_resolve_one in backend/signatures.c) verbatim -- it
 * scans the live DOOM module's executable sections for a masked byte pattern, enforces UNIQUENESS (an
 * ambiguous or absent match is a failure, never a guess), is SEH-guarded across uncommitted section tails,
 * and tolerates an inline-hooked prologue via the known_rva fallback. See backend/signatures.h.
 *
 * This DB holds ONLY the engine FUNCTIONS the shield calls into. The 6 unique sigs:
 *   Error6 / SetState / Frame / EditorPump / Resolver  -- authored here (sig_extract.py, proven
 *     scan-unique -> known_rva on the pinned build).
 *   Toast / IdStrCtor / IdStrDtor  -- REUSED VERBATIM from backend/signatures.c (same byte strings).
 * The DATA globals (editor singleton, throw-gate suppressors) are non-sig-able and stay recipe-tagged in
 * engine_layout.h. The visibility leaf (frameless tiny leaf) is sig-marginal -> recipe-tagged range.
 *
 * RE-DERIVE on a DOOM patch (the recipe these sigs were authored + proven by):
 *   tools/re/.venv/Scripts/python.exe tools/re/sig_extract.py <DOOM_unpacked.exe> \
 *       Error6=0x1a089a0 SetState=0x5298A0 Frame=0x17CE360 EditorPump=0x523140 Resolver=0x5E0AD0
 *   -> re-emits the minimal-unique pattern + asserts resolved==known on the new build.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "engine_layout.h"
#include "fault_record.h"
#include "shield_sigs.h"
#include "../backend/signatures.h"   /* the shared, self-contained resolver (no backend-DLL dep) */

shield_engine g_eng = { 0 };

/* The shield-local sig DB. `known_rva` is the pinned-build RVA: documentation for the sig + the
 * recipe-tagged fallback if the scan ever misses on a shifted build (logged). NULL-terminated. */
static const sig_entry SHIELD_ENGINE_SIGNATURES[] = {
    /* idCommon::Error(level 6) 0x1a089a0 -- the recoverable error funnel the VEH's Class-B path resumes
     * INTO (rcx=fmt). Register-spill prologue ending `mov ecx,6` (B9 06 00 00 00, the level marker) ->
     * unique. NOT the dispatcher 0x1a08e80 (that is the backend "Printf"); Error6 is the level-6 wrapper. */
    { "Error6",
      "48 89 4C 24 08 48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 48 83 EC 28 "
      "E8 ?? ?? ?? ?? 48 8B 54 24 30 4C 8D 44 24 38 B9 06 00 00 00",
      0x1A089A0u },
    /* idCommon::FatalError(level 7) 0x1a089e0 -- byte-identical to Error6 except the level marker `mov ecx,7`
     * (B9 07). LAYER 2 rewrites that 07->06 (recovery.c) so FatalError takes the RECOVERABLE level-6 throw
     * (idException) not the terminal idFatalException. The trailing B9 07 makes the sig unique vs Error6. */
    { "FatalError7",
      "48 89 4C 24 08 48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 48 83 EC 28 "
      "E8 ?? ?? ?? ?? 48 8B 54 24 30 4C 8D 44 24 38 B9 07 00 00 00",
      0x1A089E0u },
    /* idSnapEditorLocal::SetState(editor*, int) 0x5298A0 -- synchronous state transition (recovery drives
     * SetState(ed,0xB) to open the StartMenu). Prologue loads *(rcx+0x23618) (8B 91 18 36 02 00). */
    { "SetState",
      "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 8B 91 18 36 02 00",
      0x5298A0u },
    /* idCommonLocal::Frame 0x17CE360 -- the recovery frame-hook target (collision-free; the daemon hooks
     * the editor/menu pumps, NOT Frame). Prologue = 5 pushes (rdi,r12-r15) + `mov eax,0x119c0`
     * (40 57 41 54 41 55 41 56 41 57 B8 C0 19 01 00) = the 15 position-independent stolen bytes
     * recovery.c's FRAME_STOLEN steals. Unique at 15 bytes. */
    { "Frame",
      "40 57 41 54 41 55 41 56 41 57 B8 C0 19 01 00",
      0x17CE360u },
    /* the per-frame editor Think (idSnapEditorLocal) 0x523140 -- the Class-A unwind-target range LO (the
     * editor frame the VEH unwinds back to). Large-frame prologue, zero wildcards, unique. */
    { "EditorPump",
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 90 00 00 00 "
      "48 C7 45 C7 FE FF FF FF",
      0x523140u },
    /* the connection resolver FUN_1405e0ad0 0x5E0AD0 -- the in-editor fault-site range LO (the Class-A
     * CSR-revert region). Spill+5-push prologue, zero wildcards, unique. */
    { "Resolver",
      "44 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 "
      "41 54 41 55 41 56 41 57 48 8D 6C 24 E9 48 81 EC 88 00 00 00",
      0x5E0AD0u },
    /* --- REUSED VERBATIM from backend/signatures.c (byte-identical) --- */
    { "Toast",      /* editor toast-show FUN_140cfa0b0 */
      "40 57 48 83 EC 20 48 8B F9 48 8B 89 F0 08 00 00",
      0xCFA0B0u },
    { "IdStrCtor",  /* idStr from a C-string */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B DA 48 89 01 48 8B F9",
      0x19FCEF0u },
    { "IdStrDtor",  /* idStr dtor */
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 51 10",
      0x19FD120u },
    { NULL, NULL, 0 }
};

/* Resolve one sig; on a unique scan hit return its absolute addr + (optionally) its RVA. On any miss
 * (NOT_FOUND / AMBIGUOUS / OK_HOOKED-at-known) fall back to module_base+known_rva and log the miss so a
 * shifted build that needs a re-derive is visible. Returns 1 if resolved by the portable SCAN (SIG_OK),
 * 0 if it fell back to the recipe-tagged RVA. SEH wraps the whole thing (the resolver self-guards, but a
 * torn module during boot must never fault inside the install path). */
static int resolve_fn(const uint8_t *module_base, const char *name,
                      uintptr_t *out_addr, uint32_t *out_rva)
{
    __try {
        const sig_entry *e = SHIELD_ENGINE_SIGNATURES;
        for (; e->name; e++)
            if (strcmp(e->name, name) == 0) break;
        if (!e->name) { if (out_addr) *out_addr = 0; return 0; }

        sig_result r;
        sig_status st = sig_resolve_one(module_base, e, &r);
        if (st == SIG_OK) {
            if (out_addr) *out_addr = r.addr;
            if (out_rva)  *out_rva  = r.rva;
            return 1;   /* portable scan hit */
        }
        /* Scan missed (or only a hooked-known_rva hit): use the recipe-tagged RVA backstop + log it. */
        if (out_addr) *out_addr = (uintptr_t)(module_base + e->known_rva);
        if (out_rva)  *out_rva  = e->known_rva;
        {
            char msg[160];
            _snprintf_s(msg, sizeof msg, _TRUNCATE,
                "sig miss for %s (status=%d) -> fell back to known_rva 0x%x (re-derive on patch)",
                name, (int)st, (unsigned)e->known_rva);
            shield_fault f = { "sig", (int)st, msg, e->known_rva, 0 };
            shield_emit(&f);
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (out_addr) *out_addr = 0;
        return 0;
    }
}

int shield_resolve_engine(const uint8_t *module_base)
{
    int scanned = 0;
    scanned += resolve_fn(module_base, "Error6",      &g_eng.error6,       NULL);
    scanned += resolve_fn(module_base, "FatalError7", &g_eng.fatalerror7,  NULL);
    scanned += resolve_fn(module_base, "SetState",    &g_eng.setstate,     NULL);
    scanned += resolve_fn(module_base, "Frame",      &g_eng.frame,       NULL);
    scanned += resolve_fn(module_base, "EditorPump", &g_eng.editor_pump, &g_eng.editor_pump_rva);
    scanned += resolve_fn(module_base, "Resolver",   &g_eng.resolver,    &g_eng.resolver_rva);
    scanned += resolve_fn(module_base, "Toast",      &g_eng.toast_show,  NULL);
    scanned += resolve_fn(module_base, "IdStrCtor",  &g_eng.idstr_ctor,  NULL);
    scanned += resolve_fn(module_base, "IdStrDtor",  &g_eng.idstr_dtor,  NULL);
    return scanned;
}
