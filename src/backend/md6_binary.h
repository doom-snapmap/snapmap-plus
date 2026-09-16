#ifndef SH_MD6_BINARY_H
#define SH_MD6_BINARY_H

#include <stddef.h>

/* Cooked MD6 mesh reader, for dependency inspection only.
 *
 * A .md6mesh names the skeleton the engine loads with it and one material per
 * mesh, so a package that supplies a mesh also needs those. This reader walks
 * the envelope far enough to report those identities and to prove the file is
 * the format it claims; it does not decode geometry and it never writes.
 *
 * Layout (verified against every shipped md6mesh): magic "MM" 0x2b024d4d, two
 * timestamps, idStr skeleton, bounds, remap flag, idStr aux, joint remap,
 * second bounds, a variable named-shape block, then numMeshes entries each
 * beginning with idStr name and idStr material, then the material record table
 * and the trailing magic. Scalars are big-endian; idStr is a little-endian
 * length followed by unterminated bytes.
 *
 * Types passed to the visitor are native catalog families: "skeleton" for the
 * rig and "material" for each mesh material. Names are the file's own bytes,
 * NUL-terminated for the visitor. Return 0 from the visitor to abort.
 * Returns 1 when the whole envelope was read and every visit was accepted;
 * 0 otherwise, with the reason in error. */
typedef int (*sh_md6_binary_visitor)(void *context, const char *type, const char *name);

int sh_md6_mesh_references(const unsigned char *body, size_t length,
    sh_md6_binary_visitor visitor, void *context, char *error, size_t capacity);

#endif
