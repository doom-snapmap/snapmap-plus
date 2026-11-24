/* Locates engine data globals by decoding RIP-relative references in signed code. Each anchor is checked only at its
 * recorded code site for the host's renderer; the global's own recorded RVAs are audit references, never fallbacks. */
#ifndef BACKEND_ENGINE_GLOBALS_H
#define BACKEND_ENGINE_GLOBALS_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* RIP-relative offsets start at the instruction end, so disp_tail includes any immediate after disp32 (the byte in
 * cmp [rip+disp32],0); omitting it yields a plausible but wrong address. Entries are regenerated, not hand-written. */
typedef struct global_entry {
    const char *name;        /* stable key, e.g. "editor_singleton" */
    const char *anchor;      /* masked byte pattern of the code site that computes the address */
    uint16_t    disp_slot;   /* offset of the RIP-relative disp32 within `anchor` */
    uint8_t     disp_tail;   /* immediate bytes after that disp32 */
    int32_t     delta;       /* added to the decoded address */
    uint32_t    rva_vk;      /* resolved RVA in DOOMx64vk.exe 13954591; audit only */
    uint32_t    rva_gl;      /* resolved RVA in DOOMx64.exe 13954591; audit only */
    uint32_t    site_vk;     /* anchor code site in DOOMx64vk.exe 13954591 */
    uint32_t    site_gl;     /* anchor code site in DOOMx64.exe 13954591 */
} global_entry;

typedef enum glb_status {
    GLB_OK = 0,
    GLB_ANCHOR_NOT_FOUND,   /* the code site's bytes differ -- another program patched it */
    GLB_ANCHOR_AMBIGUOUS,   /* matched more than once: not an identity, refuse rather than guess */
    GLB_UNKNOWN_NAME,
    GLB_OUT_OF_RANGE,       /* decoded address is outside the host image -- a bad decode, not a global */
    GLB_UNREADABLE          /* faulted reading the displacement */
} glb_status;

/* Resolves one named global against the live host image. Returns the address, or 0 on any failure (never a
 * fallback to the recorded RVAs); cached after the first success. `out_status` may be NULL. */
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status);

/* The generated table, NULL-terminated (final entry has name == NULL). */
extern const global_entry BACKEND_ENGINE_GLOBALS[];

size_t glb_db_count(void);

/* Resolves and logs every entry at installation; returns the successful count. */
size_t glb_resolve_all(const uint8_t *module_base);

#endif /* BACKEND_ENGINE_GLOBALS_H */
