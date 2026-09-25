/* Registration keys: the spawn-type bits AI registrations are matched by. */
#ifndef SH_PACKAGE_KEYS_H
#define SH_PACKAGE_KEYS_H

#include <stddef.h>
#include <stdint.h>

#include "decl_native_schema.h"

/* A spawn-type value (spawnAIType_t) is a flag list: constant names or decimal
 * numbers separated by spaces, which the native reader unions. The spawn
 * manager selects every conductor entry whose low 32 bits share a bit with the
 * request and picks one at random (Vulkan 0x141123830, called only from the
 * spawn request constructor 0x14111b160 with spawnRequestData_t.spawnAIType).
 * A bit no single-bit constant names is therefore an opaque key its package
 * chose for its own AI; engine code never names it. */
#define SH_PACKAGE_KEY_TYPE "spawnAIType_t"
#define SH_PACKAGE_KEY_BITS 32u
/* Provenance family of one key bit: original is the bit (decimal) its package
 * authored, name the bit that package holds in a compiled library. */
#define SH_PACKAGE_KEY_FAMILY "spawnaitype"

typedef struct sh_package_key_space {
    uint32_t named;    /* bits a single-bit constant names */
    uint32_t pool;     /* unnamed bits a relocation may allocate */
} sh_package_key_space;

/* Read the key enum's constants. The allocation pool excludes the sign bit,
 * whose decimal spelling exceeds the reader's signed range. Returns 1, 0 when
 * the enum is not registered, -1 for unreadable metadata. */
int sh_package_key_space_read(sh_decl_native_schema *schema, sh_package_key_space *space);

/* Unnamed bits written as decimal numbers in one flag list. Names contribute
 * none: no constant names such a bit. Returns 0 for text that is not a flag
 * list of names and non-negative decimal numbers. */
int sh_package_key_custom_bits(const sh_package_key_space *space, const char *text, size_t length,
    uint32_t *bits);

/* Rewrite the decimal tokens of one flag list through a bit permutation:
 * to[b] < 32 moves bit b there; to[b] >= 32 keeps it. Names and spacing stay
 * exact. Returns a malloc-owned copy, or NULL when unchanged or on failure
 * (*failed set on failure). */
char *sh_package_key_relocate(const char *text, size_t length, const unsigned char to[SH_PACKAGE_KEY_BITS],
    size_t *output_length, int *failed);

/* A bit-flag property inspector (snappropertyinspector_bitflag) whose enumType
 * names the key enum offers its values[].enumValue strings as the choices an
 * author can set (the stock AI filter's spawnaitypebitflaginspector). Those
 * strings are key sites by that inspector contract, not by reflected type.
 * Returns a malloc-owned rewritten declaration, NULL when unchanged or not
 * such an inspector (*failed set on failure). */
char *sh_package_key_relocate_inspector(const char *text, size_t length,
    const unsigned char to[SH_PACKAGE_KEY_BITS], size_t *output_length, int *failed);

#endif
