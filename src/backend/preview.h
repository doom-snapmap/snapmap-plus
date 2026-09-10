/* Thread-safe asset-preview transport: request generations, RGBA-to-PNG
 * encoding and a consumable base64 data URI. Producers take staged requests
 * and publish pixels; the UI requests through iface ext 14 (+0x2d8) and
 * consumes through ext 13 (+0x2d0). Generations reject stale results.
 */
#ifndef BACKEND_PREVIEW_H
#define BACKEND_PREVIEW_H

#include <stddef.h>

/* ------------------------------------------------------------------ consumer side (the UI thread) --*/

/* Consume the latest published preview as a `data:image/png;base64,...` URI. Returns the length on
 * success, 0 if nothing has been published yet (including immediately after a request), or
 * -(required size) if `cap` is too small so the caller can re-ask with a bigger buffer. A successful
 * copy releases the backend buffer; an undersized probe leaves it available for the required retry.
 * Safe from any thread. Backs iface ext 13 (+0x2D0). */
int  sh_preview_get(char *out, size_t cap);

/* Stage a name and invalidate the current preview. Production is
 * asynchronous; poll sh_preview_get for completion. Backs iface ext 14
 * (+0x2d8).
 */
void sh_preview_request(const char *name);

/* Stage a request with its catalog kind when the caller has one. AUTO preserves the original
 * Material-first behavior; a typed Image request lets the producer bypass material/atlas lookup. */
#define SH_PREVIEW_KIND_AUTO (-1)
void sh_preview_request_kind(const char *name, int kind);

/* Invalidate any staged or published preview. An in-flight producer's generation becomes stale,
 * and an encoded buffer not yet consumed by the UI is released immediately. */
void sh_preview_cancel(void);

/* ------------------------------------------------------------- producer side (whoever makes pixels) */

/* Read the staged request. Copies the name and catalog kind into `out` / `kind`, and returns 1 if a
 * request is pending and has not been published for yet; returns 0 otherwise. Does NOT clear the
 * request -- a producer that fails may legitimately want to retry, and publishing is what marks it
 * served. `kind` is SH_PREVIEW_KIND_AUTO for an untyped legacy request. */
int  sh_preview_take_request(char *out, size_t cap, unsigned long *generation, int *kind);

/* Encode `w` x `h` RGBA8 pixels (row-major, top row first, 4 bytes/pixel) and publish them as the
 * current preview only if `generation` is still the latest request. Thread-safe: the previous buffer
 * is swapped out under the lock and only freed once no reader can still hold it. */
#define SH_PREVIEW_STALE     (-1)
#define SH_PREVIEW_FAILED      0
#define SH_PREVIEW_PUBLISHED   1
int sh_preview_publish(unsigned long generation, const unsigned char *rgba, unsigned w, unsigned h);

#endif /* BACKEND_PREVIEW_H */
