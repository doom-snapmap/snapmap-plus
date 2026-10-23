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
 * Transparency atlas entries use generated/transsortatlas/*.bimage, with the
 * material's atlas and ordinary-image requests captured as separate identities.
 * Owned results; no filesystem/engine calls. */
char *sh_package_cooked_path(const char *family, const char *name);
int sh_package_cooked_identity(const char *path, const char **family, char **name);

/* A product producer's output is the native kind of what it read: when every
 * input is catalogued only under one family, each row named by its own path,
 * the output is that family's resource named by its own path. NULL decides
 * nothing; it precedes the inverse cooked identity and deferral above. */
const char *sh_package_generated_family(const sh_compiled_resource *file, const sh_resource_catalog *catalog);

/* A SnapMap module's name-derived members: resources the engine opens by
 * deriving their path from the module's own path maps/modules/<R> and its
 * basename <B>, which no reference spells. They are, by role:
 *   card               generated/decls/snapmoduleinfo/<R>.decl (the module
 *                      wrapper loader resolves moduleInfo by the module name)
 *   render             maps/modules/palettes/<P>/<R>.bmodel (LoadSnapModule
 *                      formats maps/modules/palettes/%s/%s.bmodel)
 *   collision          maps/modules/<R>/_combo/world.bcm and
 *                      maps/modules/<R>/<B>_floor_collision.bcm
 *   navigation         generated/maps/modules/<R>/<B>.baas_<agent> (BuildAAS
 *                      merges <B>.aas_<agent>)
 *   sound              maps/modules/<R>/<B>.sbsp (BuildSoundWorld VK 0x1404f0670
 *                      / GL 0x1404f0040)
 *   ambient            maps/modules/<R>/<B>.ambientsh (the placed-module walk
 *                      VK 0x1404ec690 / GL 0x1404ec060)
 *   occlusion          maps/modules/<R>/_combo/_world.tome (InitFromMap VK
 *                      0x14174fe70 / GL 0x141742b10)
 *   probe              maps/modules/<R>/lightprobes/<X>.bimage (VK 0x1405b2bf0
 *                      / GL 0x1405b22e0 set explicitLightProbePath)
 *   blueprint          maps/modules/<R>/<B>_single_surface_walls.bmodel,
 *                      <B>_walls_floor.bmodel and <B>_wall_<n>.bmodel (the
 *                      Simple Geo builder VK 0x140d35540 / GL 0x140d34c00)
 *   named              any other maps/modules/<R>/<B>_* or <B>.* file, such as
 *                      the AAS preview models <B>_aas_monster<N>.bmodel
 * Files the module declaration names (its volumes' render and clip models) are
 * ordinary references, not members. Returns 1 and writes the module path maps/modules/<R> (no extension) and the
 * role for a member, 0 otherwise. A caller confirms that the module exists. */
int sh_package_module_member(const char *path, char *module, size_t capacity, const char **role);
/* The same member of another module: path is a member of module from and the
 * result is that member's path for module to, with both <R> and <B> replaced
 * where the engine derives them. Malloc-owned; NULL when path is not a member
 * of from. */
char *sh_package_module_member_rename(const char *path, const char *from, const char *to);

#endif
