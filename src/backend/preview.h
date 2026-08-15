/* preview.h -- the asset-preview transport: RGBA pixels in, `data:image/png;base64,...` out.
 *
 * This module is route-independent. It knows nothing about where the pixels came from: it owns the
 * request/publish handshake, PNG encoding, and cross-thread buffer, and nothing else. Each request
 * receives a generation so a slow producer can never publish an older image for a newer selection.
 *
 *   PRODUCER (whatever can make pixels)          CONSUMER (the WebView UI)
 *   ------------------------------------          -------------------------
 *   sh_preview_take_request(...)         <-----   sh_preview_request(name)     iface ext 14 (+0x2D8)
 *   sh_preview_publish(...)              ----->   sh_preview_get(out, cap)     iface ext 13 (+0x2D0)
 */
#ifndef BACKEND_PREVIEW_H
#define BACKEND_PREVIEW_H

#include <stddef.h>

/* ------------------------------------------------------------------ consumer side (the UI thread) --*/

/* Latest published preview as a `data:image/png;base64,...` URI. Returns the length on success, 0 if
 * nothing has been published yet (including immediately after a request, until that request's pixels
 * land), or -(required size) if `cap` is too small so the caller can re-ask with a bigger buffer.
 * Safe from any thread. Backs iface ext 13 (+0x2D0). */
int  sh_preview_get(char *out, size_t cap);

/* Stage `name` as the asset the user wants to see, and invalidate whatever is currently published so a
 * poll cannot mistake the previous image for this request's answer. ASYNCHRONOUS by nature: pixels are
 * produced on another thread, so the caller polls sh_preview_get until it returns > 0. Backs iface
 * ext 14 (+0x2D8). */
void sh_preview_request(const char *name);

/* ------------------------------------------------------------- producer side (whoever makes pixels) */

/* Read the staged request. Copies the name into `out` and returns 1 if a request is pending and has not
 * been published for yet; returns 0 otherwise. Does NOT clear the request -- a producer that fails may
 * legitimately want to retry, and publishing is what marks it served. */
int  sh_preview_take_request(char *out, size_t cap, unsigned long *generation);

/* Encode `w` x `h` RGBA8 pixels (row-major, top row first, 4 bytes/pixel) and publish them as the
 * current preview only if `generation` is still the latest request. Thread-safe: the previous buffer
 * is swapped out under the lock and only freed once no reader can still hold it. */
#define SH_PREVIEW_STALE     (-1)
#define SH_PREVIEW_FAILED      0
#define SH_PREVIEW_PUBLISHED   1
int sh_preview_publish(unsigned long generation, const unsigned char *rgba, unsigned w, unsigned h);

#endif /* BACKEND_PREVIEW_H */
