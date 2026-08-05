/* megapreview.h -- the asset-preview PRODUCER: megatexture pages -> RGBA, on the CPU.
 *
 * This is the replacement for the retired in-engine render-capture route (see preview.h for why
 * that died). Instead of asking the renderer to draw a material -- which only works for materials
 * the loaded map already renders -- this reads the material's megatexture pages straight off disk
 * and decodes them by calling DOOM's OWN page decoder in-process.
 *
 * That decoder is a pure function: bytes in, RGBA out. No renderer, no render thread, no GPU, no
 * virtual-texture state, and critically NO MAP RESIDENCY. So this works for the full ~6,800-material
 * catalog regardless of which map is open, which is exactly what an asset browser needs.
 *
 * Everything here was derived and proven in the doom-re campaign
 * `revenant-asset-index-and-viewport`:
 *   - evidence 06: the codec is id's own DCT + YCoCg-R, not libjpeg -- CALL it, do not reimplement.
 *   - evidence 07: page addressing. `.vmtr` rect -> shard -> mip cell -> page id -> file offset.
 *   - evidence 08: the decoder run offline on 68/68 pages, plus the three rules the port must obey
 *                  (buffer slack, pre-clear the output, plane 0 is albedo).
 *
 * It produces into the transport in preview.h and touches nothing else.
 */
#ifndef BACKEND_MEGAPREVIEW_H
#define BACKEND_MEGAPREVIEW_H

#include <stdint.h>

#include "signatures.h"

/* Start the producer. The decoder is resolved from the shared signature database as
 * `Mega2PageDecode` -- no hardcoded RVA -- so it is found wherever the loader put it, and a build
 * whose bytes do not match simply fails to resolve rather than calling into the wrong code.
 * `module_base` is still needed for the on-disk `virtualtextures` directory next to the exe.
 * Spawns one low-priority worker that serves preview requests staged through sh_preview_request.
 *
 * Returns 1 if the worker started, 0 otherwise (NULL base, unresolved decoder, already installed).
 * Failure is non-fatal and only costs previews. */
int sh_megapreview_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* A material's `.vmtr` atlas rect, written to out_xywh as {x, y, w, h} in atlas pixels. Returns 1
 * if the material is virtual-textured, 0 if it has no rect (which is the answer to "can this take
 * a virtualmapping renderParm?" -- roughly half the catalog cannot). Divide each component by
 * 245760 to get the renderParm's value form. */
int sh_megapreview_rect(const char *name, int *out_xywh);

#endif /* BACKEND_MEGAPREVIEW_H */
