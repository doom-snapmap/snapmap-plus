/* Captured font constructor requests and their precise native lookup roles. */
#ifndef SH_PACKAGE_FONT_H
#define SH_PACKAGE_FONT_H
#include "font_binary.h"
#include "package_consumers.h"

typedef struct sh_package_font_member {
    const char *path;
    const unsigned char *body;
    size_t length;
} sh_package_font_member;
enum { SH_PACKAGE_FONT_MATERIAL_COUNT = 5 };
/* Validate a complete native plan and borrow its material targets in order:
 * 48, 64_df, 12, world-text, label. Missing roles are NULL. At least one glyph
 * material is required. Unknown/duplicate roles and invalid targets fail before
 * a native constructor can observe a partially prepared plan. */
int sh_package_font_plan(const sh_package_consumer *owner,
    const char *materials[SH_PACKAGE_FONT_MATERIAL_COUNT]);
/* Inspect captured glyph files and enumerate their native material requests.
 * The caller resolves every request in the font's source context and retains
 * positive absence for optional requests. visit returns 1 present/captured,
 * 0 positively absent or -1 failed. Absence is accepted only for optional roles.
 * No live resource, filesystem lookup or source rewrite occurs here. */
int sh_package_font_capture(const char *native_name, const sh_package_font_member *members,
    size_t count, int (*visit)(void *, const char *slot, const char *material, int optional),
    void *context, char *error, size_t capacity);

/* Match only the five requests a font loader derives from its own native name.
 * Return 0 unrelated, 1 with a borrowed bound material name, 2 for a captured
 * optional absence, or -1 for an incomplete/contradictory captured plan. A NULL
 * owner is outside the compiled catalog. No allocation or native call occurs.
 * Callers must keep the plan alive and mask it while nested consumers load.
 * A mapped cold lookup requires native materialization, not a cached-only find:
 * the optional wrapper otherwise probes the unbound filename and returns absent. */
int sh_package_font_route(const sh_package_consumer *owner, const char *family,
    const char *request, const char **replacement);
#endif
