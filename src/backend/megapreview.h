/* Preview VMTR-backed materials by reading installed atlas shards and calling
 * the native CPU page decoder. Page lookup follows rectangle, shard, mip
 * cell, page ID and file offset. It does not depend on the currently loaded
 * map. Publish decoded RGBA through preview.h.
 */
#ifndef BACKEND_MEGAPREVIEW_H
#define BACKEND_MEGAPREVIEW_H

#include <stdint.h>

#include "signatures.h"

/* Resolve Mega2PageDecode and start a low-priority worker. module_base
 * locates virtualtextures beside the executable. Requests wake the worker;
 * atlas scratch is allocated on demand and released after an idle interval.
 * Returns 1 when started, 0 on failure or repeat installation.
 */
int sh_megapreview_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* Wake the producer after sh_preview_request stages a name. A no-op when installation failed. */
void sh_megapreview_wake(void);

/* Write the VMTR rectangle as {x,y,w,h} in atlas pixels. Returns 1 when
 * found, otherwise 0. A virtualmapping renderParm uses {w,h,x,y}, with each
 * component divided by 245760.
 */
int sh_megapreview_rect(const char *name, int *out_xywh);

/* Return atlas row i in file order, or NULL past the end. Names may repeat
 * across shards. Atlas-only materials can be applied by rectangle without a
 * material decl; the browser merges these names on demand.
 */
const char *sh_megapreview_name_at(int i);

#endif /* BACKEND_MEGAPREVIEW_H */
