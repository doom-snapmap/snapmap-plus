/* preview.h -- the asset-preview TRANSPORT: RGBA pixels in, `data:image/bmp;base64,...` out.
 *
 * This module is deliberately ROUTE-INDEPENDENT. It knows nothing about where the pixels came from --
 * it owns the request/publish handshake, the encode, and the cross-thread buffer, and nothing else.
 * That separation is the lesson of the retired route: the *acquisition* half changed completely (see
 * below) while every line of this half kept working unmodified.
 *
 *   PRODUCER (whatever can make pixels)          CONSUMER (the WebView UI)
 *   ------------------------------------          -------------------------
 *   sh_preview_take_request(name, cap)   <-----   sh_preview_request(name)     iface ext 14 (+0x2D8)
 *   sh_preview_publish(rgba, w, h)       ----->   sh_preview_get(out, cap)     iface ext 13 (+0x2D0)
 *
 * HISTORY -- read this before adding a producer. The first producer was `rendercap.c`: it detoured the
 * engine's render-target setter, built its own render target, let the engine draw a material into it and
 * copied the result back over Vulkan. That worked end to end and is preserved on the branch
 * `experimental/asset-preview-render` (commits f84b66c..8b4a6b5). It was RETIRED 2026-08-03 for one
 * disqualifying reason: **the engine can only render a material the loaded map already renders**, so
 * coverage depended on which map was open -- useless for a browser that must show all ~9,805 materials.
 *
 * The replacement producer decodes the megatexture pages directly on the CPU, by calling DOOM's own
 * page decoder (`FUN_14196E140`, a pure function: bytes in, 5 x 128x128 RGBA out -- no renderer, no GPU,
 * no map residency). See the doom-re campaign `revenant-asset-index-and-viewport`, evidence
 * `06-mega2-codec-via-doom-transcoder.md`. That producer is NOT WRITTEN YET -- page addressing (which
 * file offset holds a given material's page) is the open question. Until it lands, sh_preview_request
 * stages a name and no pixels are ever published, so the UI's poll simply times out.
 */
#ifndef BACKEND_PREVIEW_H
#define BACKEND_PREVIEW_H

#include <stddef.h>

/* ------------------------------------------------------------------ consumer side (the UI thread) --*/

/* Latest published preview as a `data:image/bmp;base64,...` URI. Returns the length on success, 0 if
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
int  sh_preview_take_request(char *out, size_t cap);

/* Encode `w` x `h` RGBA8 pixels (row-major, top row first, 4 bytes/pixel) and publish them as the
 * current preview. Thread-safe: the previous buffer is swapped out under the lock and only freed once
 * no reader can still hold it. Any size is accepted -- 128x128 for one megatexture page, larger for a
 * stitched rect. */
void sh_preview_publish(const unsigned char *rgba, unsigned w, unsigned h);

#endif /* BACKEND_PREVIEW_H */
