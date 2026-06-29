/* unhide.h -- the editor UNHIDE, native C (port of OG FUN_180021EE0).
 *
 * SnapHak's `sh_target_any` toggle un-hides the campaign-only / normally-hidden placeable entities in
 * the SnapMap editor palette: it walks every `idDeclSnapEditorEntity` decl and flips bits 7-6 (0xC0) of
 * the editor-visibility flags byte at `decl+0x3CD`. UNHIDE sets the pair (decl becomes browsable);
 * RE-HIDE clears it (sparing `idInfoPath`, which the engine keeps visible). The only engine CALL is
 * GetDeclsOfType (signature-resolved by the signature resolver); the flag flips are pure memory writes.
 *
 * Clean-room: ported from our own RE of the OG XINPUT1_3 unhide + the live-validated JS reimpl
 * (the reference implementation doUnhide). Zero OG SnapHak bytes.
 */
#ifndef BACKEND_B1_UNHIDE_H
#define BACKEND_B1_UNHIDE_H

#include <stdint.h>
#include <stddef.h>

/* Result of one unhide/re-hide pass. ok==0 means the op could not run (see `error`). */
typedef struct sh_unhide_result {
    int          ok;        /* 1 = the pass ran (count may still be 0 if the registry was empty) */
    int          show;      /* 1 = UNHIDE pass (set 0xC0); 0 = RE-HIDE pass (clear, spare idInfoPath) */
    unsigned int count;     /* total idDeclSnapEditorEntity decls walked */
    unsigned int touched;   /* decls whose flag byte we actually changed */
    unsigned int spared;    /* (re-hide only) idInfoPath decls left visible */
    const char  *error;     /* NULL on ok; a static reason string otherwise */
} sh_unhide_result;

/* Apply one unhide (show!=0) or re-hide (show==0) pass over the live snapEditorEntityDef registry.
 * `get_decls_of_type` is the resolved engine GetDeclsOfType (void*(const char* typeName)); pass the
 * address from the signature resolver. MUST be called on the DOOM main thread (it invokes the engine
 * fn) and only once the decl registry is populated -- see sh_unhide_apply_when_ready. */
sh_unhide_result sh_unhide_apply(void *get_decls_of_type, int show);

/* Bootstrap entry: poll GetDeclsOfType("idDeclSnapEditorEntity") until the registry is populated
 * (non-null list with count>0) or the budget expires, then apply ONE unhide pass and log the result as
 * "B1: unhide applied N/174" (or a failure form). `get_decls_of_type` is the resolved engine fn (0 =>
 * not resolved, logs and returns 0). Returns 1 if an unhide pass ran. */
int sh_unhide_apply_when_ready(void *get_decls_of_type);

#endif /* BACKEND_B1_UNHIDE_H */
