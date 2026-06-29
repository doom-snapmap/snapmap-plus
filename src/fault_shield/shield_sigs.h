/* shield_sigs.h -- the fault-shield's version-portability sig layer.
 *
 * The PORTABILITY DISCIPLINE:
 * the clone must survive DOOM patches, so an engine FUNCTION is resolved by masked-byte SIGNATURE, never
 * a hardcoded base+RVA literal (OG SnapHak hardcodes base+RVA and breaks on every patch). The fault-shield
 * is a SEPARATE proxy from the backend, but it reuses the backend's self-contained resolver verbatim
 * (signatures.c/.h -- no backend-DLL dependency, see signatures.h header) and carries its OWN small sig DB
 * of the 6 engine functions the shield calls into.
 *
 * Resolved ONCE at install time (after g_doom_base is known, before veh_install/recovery_install). Each
 * resolved address is cached in g_eng (below); the VEH / recovery call sites read g_eng.<fn> instead of a
 * literal g_doom_base+RVA. If a single sig fails to resolve on a shifted build, that one falls back to its
 * documented known_rva (so a partial-resolve build still arms) and the miss is logged -- the scan stays
 * primary, the RVA is the recipe-tagged backstop.
 *
 * DATA globals (the editor singleton, the throw-gate suppressors) are NOT sig-able (non-unique .data) and
 * stay as recipe-tagged base+RVA literals in engine_layout.h -- see the re-derive comments there.
 *
 * sigs proven scan-unique -> known_rva on the pinned build (recipe):
 *   tools/re/.venv/Scripts/python.exe tools/re/sig_extract.py <DOOM_unpacked.exe> \
 *       Error6=0x1a089a0 SetState=0x5298A0 Frame=0x17CE360 EditorPump=0x523140 Resolver=0x5E0AD0
 *   (Toast/IdStrCtor/IdStrDtor reused verbatim from backend/signatures.c, same proof.)
 */
#ifndef SHIELD_SIGS_H
#define SHIELD_SIGS_H

#include <stdint.h>

/* The engine addresses the shield needs, resolved by signature (function entries) at install time. A
 * resolved value is module_base + the live RVA; 0 means unresolved (the caller then falls back to the
 * known_rva literal, logged). The frame-hook target (Frame) is resolved here too so recovery_install
 * hooks the sig-found entry, not a literal.
 *
 * Two of these (editor_pump, resolver) are used by the VEH only as RVA *RANGE* bounds (the Class-A
 * classifier compares (rip - module_base) against [LO,HI)); for those we also expose the resolved RVA
 * (editor_pump_rva / resolver_rva) and derive HI = LO + a recipe-tagged span (engine_layout.h). The
 * visibility leaf (a frameless tiny leaf, sig-marginal) is NOT sig-resolved -- it stays recipe-tagged in
 * engine_layout.h as a build-derived range off the resolver. */
typedef struct shield_engine {
    uintptr_t error6;        /* idCommon::Error(level 6)  -- the Class-B recoverable-error funnel */
    uintptr_t fatalerror7;   /* idCommon::FatalError(level 7) -- Layer-2 downgrades its level byte 7->6 */
    uintptr_t setstate;      /* idSnapEditorLocal::SetState(editor*, int) */
    uintptr_t frame;         /* idCommonLocal::Frame      -- the recovery frame-hook target */
    uintptr_t editor_pump;   /* the per-frame editor Think -- the Class-A unwind-target range LO (addr) */
    uintptr_t resolver;      /* the connection resolver   -- the in-editor fault-site range LO (addr) */
    uintptr_t idstr_ctor;    /* idStr-from-cstring ctor   -- the editor-native toast title/text */
    uintptr_t idstr_dtor;    /* idStr dtor */
    uintptr_t toast_show;    /* editor toast-show */
    uint32_t  editor_pump_rva; /* resolved RVA of editor_pump (Class-A unwind-range LO) */
    uint32_t  resolver_rva;    /* resolved RVA of resolver    (in-editor fault-range LO) */
} shield_engine;

extern shield_engine g_eng;

/* Resolve the shield's engine functions by signature over the live DOOM module at `module_base`. Fills
 * g_eng; for any sig that does not resolve UNIQUELY, falls back to module_base+known_rva and logs the
 * miss. Returns the count that resolved by signature (the rest are on their recipe-tagged RVA backstop).
 * SEH-guarded internally (the resolver's scan + every fallback deref). */
int shield_resolve_engine(const uint8_t *module_base);

#endif /* SHIELD_SIGS_H */
