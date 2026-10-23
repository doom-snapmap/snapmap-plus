/* Common authored-document preparation for maps and portable prefabs. */
#ifndef SH_PACKAGE_DOCUMENT_H
#define SH_PACKAGE_DOCUMENT_H
#include "map_package.h"
#include "package_runtime.h"

/* Interpret the exact retained delivery using its modern or historical recipe,
 * then rebind only typed reference sites. The process-heap JSON input is replaced
 * only after success; *out is a separately owned unpublished plan. An ordinary
 * vanilla document has no plan. This neither admits native objects nor installs
 * anything, and preserves all unrelated native string bytes. */
int sh_package_document_bind(const char *root, const sh_mpkg_context *sources,
    char **json, size_t *length, sh_package_map_plan **out, char *error, size_t capacity);
/* sh_mpkg_stock_fn over the game's own resources: the shared compilation's
 * catalog (or, in a test build, the test baseline). */
int sh_package_document_stock(void *context, const char *family, const char *name);
/* The last bind's phases in milliseconds: the plan (the map context's compile,
 * or the reuse of a departed map's), the rebind and the native map data. */
typedef struct sh_package_document_stats { unsigned long long prepare_ms, rebind_ms, native_ms; } sh_package_document_stats;
void sh_package_document_bind_stats(sh_package_document_stats *out);
#endif
