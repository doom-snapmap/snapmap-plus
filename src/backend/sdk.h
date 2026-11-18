/* sdk.h -- the Graphics SDK's compilers, one entry point (docs/graphics-sdk.md).
 *
 * A mod's decl is read through the SDK when its first key says what it is: a material with `type` (modlook.h), an fx
 * decl with `type effect` (modeffect.h), an env decl with `type environment` (modenv.h), a gorewounds decl with
 * `type wound` (modwound.h). Any material may also write values that change over time (sdkvalues.h). The build checks each file with sh_sdk_compile (a mistake fails the
 * build with its sentence; the needs it returns join the file's own) and the server serves what it compiles
 * (sh_sdk_serve_compile), so both read the same text.
 *
 * Results are modlook.h's: SH_LOOK_SAME, SH_LOOK_CHANGED, SH_LOOK_MISTAKE, SH_LOOK_OOM. */
#ifndef SNAPMAP_PLUS_SDK_H
#define SNAPMAP_PLUS_SDK_H

#include <stddef.h>

#include "modlook.h"

/* `path` is the file's path or request (its type folder decides: generated/decls/material|material2|fx|env/...).
 * *notes (when notes is not NULL): NULL or a malloc'd text of one line per thing drawn differently from asked.
 * *needs (when needs is not NULL): NULL or a malloc'd text of one `type name` line per decl the compiled text reaches
 * that its keys do not name (an effect's emitter, an environment's fx and emitter). */
int sh_sdk_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                   size_t why_len, char **notes, char **needs);

/* The same without notes and needs: the server's filter (modserve.h sh_modserve_set_material_filter). */
int sh_sdk_serve_compile(const char *path, const char *text, size_t len, char **out, size_t *out_len, char *why,
                         size_t why_len);

/* The decl type an SDK path names: "material", "fx", "env", "gorewounds", or NULL for any other. */
const char *sh_sdk_decl_type(const char *path);

/* The decl name a path names (after generated/decls/<type>/, without .decl), malloc'd, or NULL. */
char *sh_sdk_decl_name(const char *path);

#endif /* SNAPMAP_PLUS_SDK_H */
