#ifndef SH_PACKAGE_INSTALLED_OPEN_H
#define SH_PACKAGE_INSTALLED_OPEN_H

#include <stddef.h>
#include "map_package.h"
#include "package_provenance.h"
#include "package_runtime.h"

/* R-401 step 2 (register-once only): bind a map whose every carried delivery is an installed package tree, and
 * whose binding record names only identities the library serves, over the registered library without compiling a
 * map context. 1 with *json replaced and *out the document's variant plan (NULL when it needs none); 0 when the
 * open does not qualify (the caller compiles as before). */
int sh_package_installed_open(const char *root, const sh_mpkg_context *sources, const sh_package_provenance *saved,
    char **json, size_t *length, sh_package_map_plan **out, char *error, size_t capacity);

#endif
