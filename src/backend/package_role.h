/* Native role identity (R-155b). Rows sharing an entityDef key are one
 * gameplay identity to every native consumer, so the two native families that
 * compare a declaration with a role slot by pointer are widened to that
 * relation and to nothing else: the map identity scan (player start presence)
 * and the noWeapon default marker of the weapon-list serializers. Unrelated
 * declarations keep pointer identity. */
#ifndef SH_PACKAGE_ROLE_H
#define SH_PACKAGE_ROLE_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Palette view and row key (see sh_package_editor_rows_api). */
typedef struct sh_package_role_api {
    int (*view)(void *context, void *palette, const unsigned char **entries, size_t *count, size_t *stride);
    const char *(*key)(void *context, const void *entry);
    void *context;
} sh_package_role_api;
void sh_package_role_bind(const sh_package_role_api *api);
/* decl == slot, or both rows are in palette with equal keys. Absent rows or
 * unreadable keys fail closed to pointer equality. */
int sh_package_role_equal(void *palette, const void *decl, const void *slot);
/* Identity scan widened: 1 when a map entity's row (entity+8) is role-equal
 * to decl on the live editor palette, read at the call; any number of rows may
 * share the key. The native pointer result is checked first. */
int sh_package_role_contains(const void *map, const void *decl);
/* Default-marker lookup: the slot's own row when decl is role-equal to it,
 * otherwise found unchanged. */
const void *sh_package_role_canonical(void *palette, const void *decl, const void *found, const void *slot);
/* The two weapon-list serializer sites of a renderer that call the
 * by-declaration lookup and then compare with bank+0x148, each verified byte
 * for byte; both share one lazy-init flag, bank holder and bank getter.
 * Returns 1 for exactly two. */
#define SH_PACKAGE_ROLE_SITES 2
typedef struct sh_package_role_sites {
    uintptr_t returns[SH_PACKAGE_ROLE_SITES], flag, holder, getter;
    size_t count;
} sh_package_role_sites;
int sh_package_role_find_sites(const uint8_t *base, uintptr_t by_decl, sh_package_role_sites *out);
/* Both families or neither within a family; returns the number installed. */
int sh_package_role_install(const sig_result *results, size_t count, const uint8_t *base);
#ifdef SH_PACKAGE_ROLE_TESTING
void sh_package_role_test_sites(const sh_package_role_sites *sites, const void *(*by_decl)(void *, const void *));
const void *sh_package_role_test_by_decl(void *palette, const void *decl, uintptr_t return_address);
#endif
#endif
