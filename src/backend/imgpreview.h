/* File-based previews for materials and direct images. Resolve installed
 * index/resource records, then decode DEFLATE and BCn on the CPU. This
 * complements the VMTR atlas route and never changes game files.
 */
#ifndef BACKEND_IMGPREVIEW_H
#define BACKEND_IMGPREVIEW_H

/* One-time setup. Cheap: resource indexes are parsed lazily on the first request, and optional
 * Wwise/.vmtr catalog metadata waits for its corresponding category. Always returns 1. */
int sh_imgpreview_install(void);

/* Resolve a material name through its decl to an image, or accept a direct image name, decode it to
 * RGBA, and publish through sh_preview_publish. Returns 1 if something was published, 0 otherwise.
 * Call it only AFTER the megatexture route has declined for a material request. */
int sh_imgpreview_produce(const char *name, unsigned long generation);

/* Decode exactly the named Image record. Unlike the compatibility producer above, this never treats a
 * same-named Material as authoritative and is called before any VMTR work for a typed Image selection. */
int sh_imgpreview_produce_image(const char *name, unsigned long generation);

/* The SH_ASSET_* type ids live in the shared ABI header -- the UI sends one across the iface. */
#include "../common/snapmap_plus_iface.h"

/* Enumerate installed asset names of one type for the Assets browser, newline-separated, starting
 * at index `start`. Returns how many names were written; 0 means "no more" (or unavailable data).
 * The caller pages by adding the returned count to `start`. Base index metadata is compacted after
 * parsing; the Wwise sound union and decl-less .vmtr material union are loaded only for those kinds. */
int sh_imgpreview_list(int kind, unsigned start, char *out, size_t cap);

/* Check a selectable name against the installed catalog without calling the
 * engine. Use this before native find-or-create lookups, whose missing-name
 * behavior can raise an engine error. Returns 1 when indexed.
 */
int sh_imgpreview_has(int kind, const char *name);

/* Internal cooked-geometry record kind. It is intentionally outside SH_ASSET_*: baseModel rows are
 * implementation payloads behind an md6Def, not names the Assets browser should offer directly. */
#define SH_IMGPREVIEW_BASEMODEL_KIND 250

/* Read one exact installed-resource payload by indexed kind/name. The caller owns *out_bytes and frees
 * it with free(). `max_bytes` is a hard allocation/decompression ceiling; oversize and missing records
 * fail without allocating. This is the read-only bridge used by the Prefab Details renderer, which
 * decodes geometry lazily without copying any game asset into Snapmap+ itself. */
int sh_imgpreview_read_payload(int kind, const char *name, size_t max_bytes,
                               unsigned char **out_bytes, size_t *out_len);

#endif /* BACKEND_IMGPREVIEW_H */
