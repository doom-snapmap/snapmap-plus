/* Locate engine data globals by decoding RIP-relative references in signed code.
 * Generated anchors must resolve uniquely on both supported executables; their
 * pinned RVAs are audit references and never runtime address fallbacks. */
#ifndef BACKEND_ENGINE_GLOBALS_H
#define BACKEND_ENGINE_GLOBALS_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* RIP-relative offsets start at the instruction end. disp_tail includes any
 * immediate after disp32, such as the byte in cmp [rip+disp32],0. Omitting it
 * produces a plausible but incorrect address. Regenerate entries; do not guess. */
typedef struct global_entry {
    const char *name;        /* stable key, e.g. "editor_singleton" */
    const char *anchor;      /* masked byte pattern of the code site that computes the address */
    uint16_t    disp_slot;   /* byte offset of the RIP-relative disp32 within `anchor`; see above */
    uint8_t     disp_tail;   /* immediate bytes following that disp32 in the anchor instruction */
    int32_t     delta;       /* added to the decoded address; for adjacent-slot globals */
    uint32_t    pinned_rva;  /* the global's RVA on the pinned Vulkan build -- audit only, never used */
} global_entry;

typedef enum glb_status {
    GLB_OK = 0,
    GLB_ANCHOR_NOT_FOUND,   /* the code site did not match -- unknown build, or it was inline-hooked */
    GLB_ANCHOR_AMBIGUOUS,   /* matched more than once: not an identity, refuse rather than guess */
    GLB_UNKNOWN_NAME,
    GLB_OUT_OF_RANGE,       /* decoded address is outside the host image -- a bad decode, not a global */
    GLB_UNREADABLE          /* faulted reading the displacement */
} glb_status;

/* Resolve one named global against the live host image. Returns the address, or 0 on any failure
 * (never a guess, never a fallback to `pinned_rva`). Results are cached after the first success.
 * `out_status` may be NULL. */
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status);

/* The generated table, NULL-terminated (final entry has name == NULL). */
extern const global_entry BACKEND_ENGINE_GLOBALS[];

size_t glb_db_count(void);

/* Resolve and log every entry at installation; return the successful count. */
size_t glb_resolve_all(const uint8_t *module_base);

#endif /* BACKEND_ENGINE_GLOBALS_H */
