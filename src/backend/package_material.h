/* Source-bound adapter for ordered native material composition. */
#ifndef SH_PACKAGE_MATERIAL_H
#define SH_PACKAGE_MATERIAL_H
#include "package_compiler.h"

char *sh_package_material_compose(sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, const sh_package_source_view *view,
    size_t *length, char *error, size_t capacity, sh_decl_conflict *conflict);
#endif
