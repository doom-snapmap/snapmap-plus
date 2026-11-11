/* Engine function addresses for the backend's resident fault shield, taken
 * once from the bootstrap's signature results before recovery handlers are
 * installed. A function whose signature did not resolve stays zero and its
 * dependent operation declines; no address is ever guessed. */
#ifndef SHIELD_SIGS_H
#define SHIELD_SIGS_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

/* Cached addresses; zero disables the dependent operation. The two range RVAs
 * combine with measured body spans in engine_layout.h for Class-A classification.
 * The visibility leaf is resolved separately through an engine_globals call-site anchor. */
typedef struct shield_engine {
    uintptr_t error6;        /* idCommon::Error(level 6)  -- the Class-B recoverable-error funnel */
    uintptr_t fatalerror7;   /* idCommon::FatalError(level 7) -- its level byte is downgraded 7->6 */
    uintptr_t setstate;      /* idSnapEditorLocal::SetState(editor*, int) */
    uintptr_t frame;         /* idCommonLocal::Frame      -- the recovery frame-hook target */
    uintptr_t editor_pump;   /* the per-frame editor Think -- the Class-A unwind-target range LO (addr) */
    uintptr_t resolver;      /* the connection resolver   -- the in-editor fault-site range LO (addr) */
    uintptr_t clear_dialog;  /* the shell's ClearDialog wrapper -- the save guard's dismissal */
    uint32_t  editor_pump_rva; /* resolved RVA of editor_pump (Class-A unwind-range LO) */
    uint32_t  resolver_rva;    /* resolved RVA of resolver    (in-editor fault-range LO) */
} shield_engine;

extern shield_engine g_eng;

/* Fill g_eng from the bootstrap's results; log each function that is missing.
 * Returns the number resolved. */
int shield_resolve_engine(const sig_result *results, size_t count);

#endif /* SHIELD_SIGS_H */
