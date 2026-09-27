/* Portable native resource-name normalization and 32-bit manager identity. */
#ifndef SH_NATIVE_RESOURCE_ID_H
#define SH_NATIVE_RESOURCE_ID_H
#include <stdint.h>

/* Return a malloc-owned normalized ASCII name. Fold uppercase, replace '\\'
 * with '/', and remove a single initial slash while preserving a double-slash
 * prefix. Reject empty results and non-ASCII bytes: their native case rules
 * are not established. This is native naming, not filesystem path validation. */
char *sh_native_resource_name(const char *name);

/* Hash the EXACT registered native type spelling, then the normalized name,
 * with the verified little-endian MurmurHash2A/0x566f8ad7 contract. Map a final
 * zero to one. No engine memory or code is accessed. Return 1 on success and
 * clear *identity on failure. Do not pass a lowercased logical family unless
 * that is the actual registered spelling. This is not a content fingerprint. */
int sh_native_resource_id(const char *native_type, const char *name, uint32_t *identity);

/* The same identity for an empty name, which only the engine itself creates
 * (a font request with no face opens "fonts//<size>.dat"; live font ID
 * 0xec305aad). Package names are never empty. */
int sh_native_resource_unnamed_id(const char *native_type, uint32_t *identity);
#endif
