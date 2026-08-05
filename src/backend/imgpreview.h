/* imgpreview.h -- the SECOND preview producer: plain (non-megatexture) materials.
 *
 * megapreview.c serves the 5,033 materials that have a `.vmtr` atlas rect. This serves the rest,
 * which render fine in game but are backed by ordinary image assets in the `.index`/`.resources`
 * containers. Together they cover ~84% of the ~9,805-material catalog; the remainder are decals
 * and particles baked into shared atlases, which need a third route.
 *
 * Read-only against the shipped containers, and no engine call at all: DEFLATE and BCn are public
 * formats, unlike the megatexture page codec which had to be called rather than reimplemented.
 * See the doom-re campaign `revenant-asset-index-and-viewport`, evidence 09.
 */
#ifndef BACKEND_IMGPREVIEW_H
#define BACKEND_IMGPREVIEW_H

/* One-time setup. Cheap: the 15 MB indices are parsed lazily on the first request, so a user who
 * never opens the Assets tab pays nothing. Always returns 1. */
int sh_imgpreview_install(void);

/* Resolve `name` -> material decl -> image asset -> RGBA, and publish through sh_preview_publish.
 * Returns 1 if something was published, 0 otherwise (no material record, decl names no usable
 * image, image missing, or an image format we do not decode). Call it only AFTER the megatexture
 * route has declined, since a VT-backed material has no image asset to find. */
int sh_imgpreview_produce(const char *name);

/* The SH_ASSET_* type ids live in the shared ABI header -- the UI sends one across the iface. */
#include "../common/snapmap_plus_iface.h"

/* Enumerate SnapMap asset names of one type for the Assets browser, newline-separated, starting at
 * index `start`. Returns how many names were written; 0 means "no more" (or the containers are
 * unreadable, or `kind` is out of range). The caller pages by adding the returned count to `start`
 * until it gets 0 -- materials alone are ~9,805 names and roughly 400 KB, too much for one
 * message. Only box 0 (`snap_gameresources`) is listed; see the note at the definition. */
int sh_imgpreview_list(int kind, unsigned start, char *out, size_t cap);

/* Does a decl of this SH_ASSET_* type with this exact name exist in the shipped containers? The
 * answer comes from our own index, never from the engine: the engine's by-name decl find is a
 * find-OR-CREATE that fatals on a bad name, so it can only be called with a name already known to
 * be good. sh_soundpreview_play is the caller that needs this. Returns 1 if present. */
int sh_imgpreview_has(int kind, const char *name);

#endif /* BACKEND_IMGPREVIEW_H */
