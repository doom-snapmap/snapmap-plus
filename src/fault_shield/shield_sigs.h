/* Engine function addresses for the backend's resident fault shield. Resolve
 * once before installing recovery handlers. The shared scanner is primary;
 * literal RVA fallback is restricted to the exact pinned image and logged. */
#ifndef SHIELD_SIGS_H
#define SHIELD_SIGS_H

#include <stdint.h>

/* Cached addresses; zero disables the dependent operation. The two range RVAs
 * combine with measured body spans in engine_layout.h for Class-A classification.
 * The visibility leaf is resolved separately through an engine_globals call-site anchor. */
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

/* Fill g_eng and return the number resolved by unique signature scans. Logged
 * pinned-build fallbacks are usable addresses but do not count as scan successes. */
int shield_resolve_engine(const uint8_t *module_base);

#endif /* SHIELD_SIGS_H */
