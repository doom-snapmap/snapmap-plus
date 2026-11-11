/* Preview VMTR-backed materials by reading installed atlas shards and calling
 * the native CPU page decoder. Page lookup follows rectangle, shard, mip
 * cell, page ID and file offset. It does not depend on the currently loaded
 * map. Publish decoded RGBA through preview.h.
 */
#ifndef BACKEND_MEGAPREVIEW_H
#define BACKEND_MEGAPREVIEW_H

#include <stddef.h>
#include <stdint.h>

#include "signatures.h"

/* Resolve Mega2PageDecode and start the low-priority preview worker, which
 * serves every Assets-tab preview: images and decl materials always, atlas
 * materials when the decoder and <game>\virtualtextures are present. Requests
 * wake the worker; atlas scratch is allocated on demand and released after an
 * idle interval. Returns 1 when the worker started, 0 on failure or a repeat.
 */
int sh_megapreview_install(const sig_result *results, size_t n, const uint8_t *module_base);

/* Wake the producer after sh_preview_request stages a name. A no-op before installation. */
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

/* ------------------------------------------------------------ the native page decoder --------
 * Mega2PageDecode(header16, payload, NULL, out): one stored page to five 128x128 RGBA planes
 * (0x10000 bytes apart, plane 0 albedo). Every caller goes through sh_mega2_decode_page, which
 * prepares the page the way the engine's page job does. */
#define SH_MEGA2_PAGE_MAX    0x40000u   /* largest stored page accepted; shipped pages are ~50 KB */
#define SH_MEGA2_PAGE_SLACK  0x40000u   /* zeroed tail the decoder may read past the payload */
#define SH_MEGA2_PAGE_OUT    0x50000u   /* five planes */

/* Swap the five big-endian u16 stream lengths at +4..+0xC of a stored page header in place: the
 * file stores them big-endian and the decoder reads them native. */
void sh_mega2_swap_page_header(unsigned char *page16);

/* Decode `size` stored bytes held at the start of `page` (a buffer of SH_MEGA2_PAGE_MAX +
 * SH_MEGA2_PAGE_SLACK bytes; the header is swapped and the tail zeroed in place) into `out`
 * (SH_MEGA2_PAGE_OUT bytes, cleared first because the decoder skips absent planes). Returns 1
 * decoded, 0 refused (no decoder on this game build, or a size outside 17..SH_MEGA2_PAGE_MAX), -1
 * when the decoder faulted, with the exception code in *fault. Needs a thread with a large stack
 * (8 MB); the preview worker has one. */
int sh_mega2_decode_page(unsigned char *page, size_t size, unsigned char *out, unsigned long *fault);

#endif /* BACKEND_MEGAPREVIEW_H */
