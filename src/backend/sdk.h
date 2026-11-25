/* sdk.h -- the Graphics SDK's compilers, one entry point (docs/graphics-sdk.md). A decl is read through the SDK when
 * its first key names its kind (material `type`, fx `type effect`, env `type environment`, gorewounds `type wound`);
 * results are modlook.h's SH_LOOK_SAME, SH_LOOK_CHANGED, SH_LOOK_MISTAKE, SH_LOOK_OOM. */
#ifndef SNAPMAP_PLUS_SDK_H
#define SNAPMAP_PLUS_SDK_H

#include <stddef.h>

#include "modlook.h"

/* Compiles one decl; `path`'s type folder decides the kind. *notes and *needs (when non-NULL) are NULL or malloc'd:
 * one line per thing drawn differently from asked, and one `type name` line per decl reached but not named. */
int sh_sdk_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                   size_t why_len, char **notes, char **needs);

/* The same without notes and needs: the server's filter (modserve.h sh_modserve_set_material_filter). */
int sh_sdk_serve_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                         size_t why_len);


/* The decl name a path names (after generated/decls/<type>/, without .decl), malloc'd, or NULL. */
char *sh_sdk_decl_name(const char *path);

#endif /* SNAPMAP_PLUS_SDK_H */
