/* nav_entitydef.h -- the collision size a Blocking Box inherits from its entityDef, for map-text reads. Native
 * serialization omits members equal to the declaration's value, so an unresized box has no clipModelInfo.size; no
 * engine calls, no I/O. */
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
