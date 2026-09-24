/* DOOM manager names and physical paths for linked source identities. */
#ifndef SH_PACKAGE_NATIVE_NAMES_H
#define SH_PACKAGE_NATIVE_NAMES_H
#include "package_link_inputs.h"

/* sh_package_link_adapter.assign callback; context is the borrowed, complete
 * sh_package_link_inputs snapshot used by the linker. Certified original
 * graphs retain every native name/path. Other identities use a compact graph
 * naming token, with the native family suffix and material naming semantics retained.
 * Declaration paths are validated through the ordinary declaration server.
 * Font members retain their native 48/64_df/12 file form under one new name;
 * their generated material names and language fallback need consumer binding.
 * Leading underscores do not identify builtins: source-free native admission
 * and fixed-family ABI rules establish which identities must retain a name.
 * Opaque permutations without a verified naming rule are not guessed.
 *
 * This assigns names/paths, not native activation. The source/native readers
 * must account for implicit state in unit.semantics before linking, and the
 * runtime must bind virtual material rows/page providers before publication.
 * Renderer ABI names cannot be independently relocated by this callback.
 * Output follows the linker's malloc ownership contract, including failure. */
int sh_package_native_names_assign(void *context, size_t unit,
    const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count,
    char **name, char ***paths, char *error, size_t capacity);

/* Same assignment with consumer capacities INCLUDING the trailing NUL. The
 * generated token is shortened only at a hex boundary, never by truncating
 * prefixes/suffixes or paths. At least 64 token bits must fit. Short names still
 * require the linker's native registry collision checks; neither full nor
 * shortened tokens prove native-ID uniqueness. Original names are never cut. */
int sh_package_native_names_assign_bounded(void *context, size_t unit,
    const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count,
    size_t name_capacity, size_t path_capacity,
    char **name, char ***paths, char *error, size_t capacity);

/* Paired movie file/image names for sh_package_link_adapter.assign_members.
 * Context is the same captured inputs. Assignment proves the constructor's
 * derived atlas/movie filenames; it does not prove complete movie dependencies
 * or native activation. Other coupled formats need their own naming contract. */
int sh_package_native_names_assign_members(void *context, size_t unit, const char *name,
    const sh_compiled_resource *const *files, const char *const *paths, size_t count,
    char ***names, char *error, size_t capacity);
#endif
