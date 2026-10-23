/* Transport for compiler-owned bindings, outside authored package archives. */
#ifndef SH_MAP_PROVENANCE_H
#define SH_MAP_PROVENANCE_H
#include "package_provenance.h"

/* Absence is successful and reports present == 0: the caller retains historical
 * composition. Never infer isolated bindings from today's installed packages.
 * Parse only our native string-variable envelope. Map text stays byte-exact.
 * Failure clears both output and present; malformed metadata is not absence. */
/* SMPBIND0 reads as HISTORICAL binding-level ownership. SMPBIND1 retains exact
 * document-local use keys and selected sources; neither format supplies the
 * native object/reference remap or keeps source snapshot files alive itself. */
int sh_map_provenance_read(const char *json, size_t length,
    sh_package_provenance *out, int *present, char *error, size_t capacity);

/* Return an owned NUL-terminated process-heap buffer, including a byte-exact
 * copy for no change, or NULL on failure. Write replaces this family's prior
 * envelope atomically. An empty binding set or explicit empty use table removes
 * it rather than tagging a vanilla map. In USES mode, only recorded live uses
 * and their contexts are emitted, never the binding's full supplier union.
 * Strip is required before native play/network serialization. */
char *sh_map_provenance_write(const char *json, size_t length,
    const sh_package_provenance *value, size_t *out_length, char *error, size_t capacity);
char *sh_map_provenance_strip(const char *json, size_t length,
    size_t *out_length, char *error, size_t capacity);
#endif
