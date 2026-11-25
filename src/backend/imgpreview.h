/* File-based previews for materials and direct images, decoded on the CPU from installed resource records.
 * Complements the VMTR atlas route; never writes game files. */
#ifndef BACKEND_IMGPREVIEW_H
#define BACKEND_IMGPREVIEW_H

/* Startup log note; always returns 1. Resource indexes parse lazily on the first request. */
int sh_imgpreview_install(void);

/* Resolves a material name through its decl to an image (or takes a direct image name), decodes it to RGBA
 * and publishes via sh_preview_publish. Returns SH_PREVIEW_PUBLISHED, _STALE or _FAILED (reason in *why);
 * call it only after the megatexture route declined a material request. */
int sh_imgpreview_produce(const char *name, unsigned long generation, const char **why);

/* Decodes exactly the named Image record, never a same-named Material; called before any VMTR work for a
 * typed Image selection. */
int sh_imgpreview_produce_image(const char *name, unsigned long generation, const char **why);

/* The SH_ASSET_* type ids live in the shared ABI header -- the UI sends one across the iface. */
#include "../common/snapmap_plus_iface.h"

/* Every installed asset name of one type, one per line, as owned interface text (sh_iface_text_free); NULL
 * for an invalid kind, unavailable data or out of memory, "" when the kind has no names. The Wwise and
 * .vmtr unions load only for their kinds. */
char *sh_imgpreview_list(int kind);

/* Checks a name against the installed catalog without calling the engine; use it before native
 * find-or-create lookups, which raise an engine error for a missing name. Returns 1 when indexed. */
int sh_imgpreview_has(int kind, const char *name);

/* Internal cooked-geometry record kind, outside SH_ASSET_*: baseModel rows sit behind an md6Def and are
 * never offered by the Assets browser. */
#define SH_IMGPREVIEW_BASEMODEL_KIND 250

/* Read one exact installed-resource payload by indexed kind/name. The caller owns *out_bytes and frees
 * it with free(). `max_bytes` is a hard allocation/decompression ceiling; oversize and missing records
 * fail without allocating. Read-only; used by the Prefab Details renderer. */
int sh_imgpreview_read_payload(int kind, const char *name, size_t max_bytes,
                               unsigned char **out_bytes, size_t *out_len);

#endif /* BACKEND_IMGPREVIEW_H */
