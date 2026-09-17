/* Convert verified beta 13 delivery inputs before normal package compilation. */
#ifndef SH_PACKAGE_LEGACY_H
#define SH_PACKAGE_LEGACY_H
#include "package_archive.h"

/* Resolve the exact campaign type/name/provider triple, never a local override.
 * Return 1 with malloc-owned bytes, 2 when identical SnapMap bytes already ship,
 * or 0 with a diagnostic. A missing reader refuses manifests, not the package
 * formats that contain all their own bytes. When type is NULL, only classify
 * path (name == "directory" for directories): return 1 native, 0 absent,
 * -1 unreadable; body/length are NULL in this mode. */
typedef int (*sh_package_legacy_reader)(void *context, const char *type,
    const char *name, const char *path, unsigned char **body, size_t *length,
    char *error, size_t capacity);

/* Success with *out == NULL means a modern archive, unchanged. Converted bytes
 * are malloc-owned. Failure never returns a partial converted package. */
int sh_package_legacy_convert(const unsigned char *bytes, size_t length,
    const char *delivery_id, sh_package_legacy_reader reader, void *context,
    unsigned char **out, size_t *out_length, char *error, size_t capacity);
#endif
