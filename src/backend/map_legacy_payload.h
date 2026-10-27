/* Maps saved or published with the retired first-generation package system
 * carry their packages as "smpkg." string variables. Nothing installs or
 * activates them any more; the load path removes them so such a map loads as
 * a plain map. */
#ifndef SNAPMAP_PLUS_MAP_LEGACY_PAYLOAD_H
#define SNAPMAP_PLUS_MAP_LEGACY_PAYLOAD_H

#include <stddef.h>

/* 1 when `header` (quotes excluded) is a retired package shard header:
 * "smpkg.<id>.<index>.<total>.<16 lowercase hex>". */
int sh_map_legacy_payload_header(const char *header, size_t length);

/* Remove every retired package shard variable and log one line. Returns an
 * owned NUL-terminated process-heap buffer (HeapFree), or NULL when the map
 * carries none or its structure cannot be read (the caller keeps the original;
 * the engine treats the variables as inert strings). */
char *sh_map_legacy_payload_strip(const char *json, size_t length, size_t *out_length);

#endif
