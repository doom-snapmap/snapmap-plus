/* Composition of the verified MD6 definition envelope and named records. */
#ifndef SH_DECL_MD6_COMPOSE_H
#define SH_DECL_MD6_COMPOSE_H
#include "decl_compose.h"

/* The caller establishes the native md6def family and original provenance.
 * Init setters compose in native order, retaining repeated writes and checking
 * the implicit model/offset/bounds writes made by inheritance. Joint groups,
 * animation event blocks and aliases use their native identities. Alias animation
 * entries preserve original slots and merge appended value/occurrence records.
 * Flag tokens compose as ordered value/occurrence collections.
 * Group/event payloads and other sections remain indivisible native syntax. No source
 * file is modified; successful output is malloc-owned. This adapter does not
 * validate skeleton joints, animation commands or inherited native state. */
char *sh_decl_md6_compose(sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, size_t *length,
    char *error, size_t capacity, sh_decl_conflict *conflict);

/* Visit the resource identities an MD6 definition names. Types are the native
 * catalog families: "md6def" for the inherited definition, "basemodel" for the
 * bound mesh and "anim" for each alias animation. Names are the reader's own
 * strings; alias animations keep their literal bytes. Return 0 to abort.
 * Returns 1 when the whole envelope was read, 0 on a refused visit or a source
 * this reader does not accept, with the reason in error. */
typedef int (*sh_decl_md6_reference_visitor)(void *context, const char *type, const char *name);
int sh_decl_md6_references(sh_decl_source source, sh_decl_md6_reference_visitor visitor,
    void *context, char *error, size_t capacity);
#endif
