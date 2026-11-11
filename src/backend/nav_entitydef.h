/* nav_entitydef.h -- the collision size a Blocking Box inherits from its
 * entityDef, for reads that have only map text.
 *
 * Native serialization omits every member equal to the declaration's value,
 * so a box the author never resized carries no clipModelInfo.size at all. The
 * editor snapshot resolves that through the live entity; the map text read at
 * load and after Save has no entity, and asks here instead.
 *
 * Shared by the custom-navigation reads (nav_bake) and any other map-text
 * consumer that needs the same answer. No engine calls and no I/O: navigation
 * reads only the shipped snapmaps/volume/blocking, whose size is built in.
 */
#ifndef SNAPMAP_PLUS_NAV_ENTITYDEF_H
#define SNAPMAP_PLUS_NAV_ENTITYDEF_H

#include <stddef.h>

/* The edit.clipModelInfo.size an entity of entityDef `name` inherits. Returns
 * 1 with all three components, or 0 for an entityDef navigation does not read.
 * Thread-safe. */
int sh_nav_entitydef_box_size(const char *name, float size[3]);

/* sh_navr_default_size adapter for sh_nav_regions_read_defaults. */
int sh_nav_entitydef_default_size(const char *inherit, float size[3], void *ctx);

#endif /* SNAPMAP_PLUS_NAV_ENTITYDEF_H */
