/* Engine function addresses for the fault shield, taken once from the bootstrap's signature results; an
 * unresolved function stays zero and its dependent operation declines. */
#ifndef SHIELD_SIGS_H
#define SHIELD_SIGS_H

#include <stdint.h>
#include <stddef.h>
#include "../backend/signatures.h"

/* Cached addresses; zero disables the dependent operation. The two range RVAs combine with the body spans
 * in engine_layout.h for Class-A classification. */
typedef struct shield_engine {
    uintptr_t error6;        /* idCommon::Error(level 6), the Class-B recoverable-error funnel */
    uintptr_t fatalerror7;   /* idCommon::FatalError(level 7); its level byte is downgraded 7->6 */
    uintptr_t setstate;      /* idSnapEditorLocal::SetState(editor*, int) */
    uintptr_t frame;         /* idCommonLocal::Frame, the recovery frame-hook target */
    uintptr_t editor_pump;   /* the per-frame editor Think; Class-A unwind-target range start */
    uintptr_t resolver;      /* the connection resolver; in-editor fault-site range start */
    uintptr_t clear_dialog;  /* the shell's ClearDialog wrapper, used by the save guard */
    uint32_t  editor_pump_rva; /* RVA of editor_pump */
    uint32_t  resolver_rva;    /* RVA of resolver */
} shield_engine;

extern shield_engine g_eng;

/* Fill g_eng from the bootstrap's results; log each function that is missing.
 * Returns the number resolved. */
int shield_resolve_engine(const sig_result *results, size_t count);

#endif /* SHIELD_SIGS_H */
