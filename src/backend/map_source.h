/* Read a cached map through the game's own profile filesystem. */
#ifndef BACKEND_MAP_SOURCE_H
#define BACKEND_MAP_SOURCE_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Bind only; no native objects are read or allocated during installation. */
int sh_map_source_install(const sig_result *results, size_t count, const uint8_t *module_base);
/* Main engine thread only. Returns an independent NUL-terminated process-heap
 * copy, freed with HeapFree(GetProcessHeap(), 0, ...). No deserialization,
 * package activation or native map-state changes. The native buffer is always
 * released according to its ownership bit. No fixed payload-size ceiling. */
char *sh_map_source_read(const char *profile_path, size_t *length, char *error, size_t capacity);
#endif
