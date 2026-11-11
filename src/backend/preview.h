/* Thread-safe asset-preview transport: request generations, RGBA-to-PNG
 * encoding and a consumable base64 data URI. Producers take staged requests
 * and publish pixels; the UI requests through iface ext 14 (+0x2d8) and
 * consumes through ext 13 (+0x2d0). Generations reject stale results.
 */
#ifndef BACKEND_PREVIEW_H
#define BACKEND_PREVIEW_H

#include <stddef.h>

/* ------------------------------------------------------------------ consumer side (the UI thread) --*/

/* Consume the latest published preview as a `data:image/png;base64,...` URI, owned (interface
 * text: sh_iface_text_free), or NULL if nothing has been published yet (including immediately after
 * a request). Safe from any thread. Backs iface ext 13 (+0x2D0). */
char *sh_preview_take(void);

/* Stage a name and invalidate the current preview. Production is
 * asynchronous; poll sh_preview_take for completion. Backs iface ext 14
 * (+0x2d8).
 */
void sh_preview_request(const char *name);

/* Stage a request with its catalog kind when the caller has one. AUTO looks the name up as a
 * Material first; a typed Image request lets the producer bypass material/atlas lookup. */
#define SH_PREVIEW_KIND_AUTO (-1)
void sh_preview_request_kind(const char *name, int kind);

/* Invalidate any staged or published preview. An in-flight producer's generation becomes stale,
 * and an encoded buffer not yet consumed by the UI is released immediately. */
void sh_preview_cancel(void);

/* ------------------------------------------------------------- producer side (whoever makes pixels) */

/* Read the staged request: its name (malloc'd) and catalog kind when a request is pending, neither
 * published nor failed; else NULL. Does NOT clear the request; publishing or failing it is what
 * ends it. `kind` is SH_PREVIEW_KIND_AUTO for an untyped request. */
char *sh_preview_take_request(unsigned long *generation, int *kind);

/* Encode `w` x `h` RGBA8 pixels (row-major, top row first, 4 bytes/pixel) and publish them as the
 * current preview only if `generation` is still the latest request. Thread-safe: the previous buffer
 * is swapped out under the lock and only freed once no reader can still hold it. */
#define SH_PREVIEW_STALE     (-1)
#define SH_PREVIEW_FAILED      0
#define SH_PREVIEW_PUBLISHED   1
int sh_preview_publish(unsigned long generation, const unsigned char *rgba, unsigned w, unsigned h);

/* End request `generation` without a picture. `reason` is one plain sentence the Assets tab shows in
 * place of the preview ("This material has no image to preview."). A stale generation is ignored.
 * Returns SH_PREVIEW_STALE or SH_PREVIEW_FAILED. */
int sh_preview_fail(unsigned long generation, const char *reason);

/* The reason the latest request failed, owned (interface text), or NULL while the request is
 * pending, published or cancelled. Safe from any thread; the reason stays until the next request or
 * cancel, so a poller that sees sh_preview_take return NULL can ask this to stop waiting. */
char *sh_preview_failure(void);

#endif /* BACKEND_PREVIEW_H */
