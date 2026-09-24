/* Native resource units discovered from immutable authored scopes. */
#ifndef SH_PACKAGE_LINK_INPUTS_H
#define SH_PACKAGE_LINK_INPUTS_H

#include "package_linker.h"
#include "resource_catalog.h"

typedef struct sh_package_link_inputs {
    sh_package_link_unit *units;
    size_t *scopes; /* unit -> input compilation index, independent of package ID */
    size_t count;
} sh_package_link_inputs;

/* Discover native identities without an author-supplied file map. Declaration
 * identities come from the compiler; opaque identities/permutations come from
 * the installed catalog. New cooked models use the verified manager naming
 * rules. Every supplied resource must be accounted for; an unknown physical
 * path fails the candidate instead of silently dropping that file.
 *
 * Units are sorted by scope/family/name, with unique physical files sorted by
 * path. Duplicate catalog rows do not duplicate files. Different native names
 * sharing one physical path are retained as separate identities. The linker
 * subsequently decides equivalence, never package metadata or folder order.
 * All compilations retain original inventory indices and out borrows them.
 * This establishes supplied units, not complete dependency/native admission:
 * missing originals, implicit dependencies, ABI and relocation still require
 * the native reader. Initialize out to zero; failure/replacement clears it. */
int sh_package_link_inputs_open(const sh_package_compilation *const *scopes,
    size_t count, const sh_resource_catalog *catalog, sh_package_link_inputs *out,
    char *error, size_t capacity);
void sh_package_link_inputs_free(sh_package_link_inputs *inputs);

/* Exact scoped lookup. SIZE_MAX means absent/invalid, never another package's
 * resource. Inputs are native canonical identities already emitted by readers. */
size_t sh_package_link_inputs_find(const sh_package_link_inputs *inputs,
    size_t scope, const char *family, const char *name);

/* Shared forward/inverse native cooked naming contract. Forward accepts the
 * native source suffix or its suffixless manager spelling. Images and discrete
 * models accept resolved native names with option suffixes, or explicit cooked
 * paths; engine-created underscore identities have no file path. Material image
 * option inference is a separate reader obligation. Inverse applies
 * only when no catalog identity exists: it recognizes the whole provider path,
 * not an arbitrary file extension. Images and discrete models retain their
 * explicit cooked path, which their managers accept directly, without guessing
 * a source spelling. Source-link discovery defers uncatalogued files in these
 * families until their authored consumers have supplied exact native names.
 * Static particle meshes use filesystem GenerateFilename instead: generated/
 * plus a replaced .pmodel suffix, with native long-basename partitioning.
 * Their inverse identity retains the path relative to generated/, including
 * the cooked suffix; their consumers likewise take precedence over that fallback.
 * Owned results; no filesystem/engine calls. */
char *sh_package_cooked_path(const char *family, const char *name);
int sh_package_cooked_identity(const char *path, const char **family, char **name);

#endif
