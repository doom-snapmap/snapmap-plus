/* nav_entitydef.h -- the collision size a Blocking Box inherits from its
 * entityDef, for reads that have only map text.
 *
 * Native serialization omits every member equal to the declaration's value,
 * so a box the author never resized carries no clipModelInfo.size at all. The
 * editor snapshot resolves that through the live entity; the map text read at
 * load and after Save has no entity, and asks here instead.
 *
 * Shared by the custom-navigation reads (nav_bake) and any other map-text
 * consumer that needs the same answer. No engine calls: the optional reader
 * reads installed decl text, and the built-in table carries the shipped
 * snapmaps/volume/blocking size so the common case needs no I/O at all.
 */
#ifndef SNAPMAP_PLUS_NAV_ENTITYDEF_H
#define SNAPMAP_PLUS_NAV_ENTITYDEF_H

#include <stddef.h>

/* Read the unresolved decl text of entityDef `name` into a malloc'd buffer
 * the caller frees with free(). Returns 0 when it has none. */
typedef int (*sh_nav_entitydef_reader)(const char *name, unsigned char **out,
                                       size_t *out_len);
void sh_nav_entitydef_set_reader(sh_nav_entitydef_reader reader);

/* The edit.clipModelInfo.size an entity of entityDef `name` inherits, walking
 * `inherit` up to 16 levels. Returns 1 with all three components, or 0 when
 * any is unknown or the clip model is not a box. Answers are cached until
 * sh_nav_entitydef_reset. Thread-safe. */
int sh_nav_entitydef_box_size(const char *name, float size[3]);

/* sh_navr_default_size adapter for sh_nav_regions_read_defaults. */
int sh_nav_entitydef_default_size(const char *inherit, float size[3], void *ctx);

/* Forget cached answers, e.g. after the installed decls change. */
void sh_nav_entitydef_reset(void);

#endif /* SNAPMAP_PLUS_NAV_ENTITYDEF_H */
