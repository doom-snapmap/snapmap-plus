/* The collision size a Blocking Box inherits from its entityDef. */
#include <string.h>

#include "nav_entitydef.h"

/* The shipped declarations navigation reads, with the clip size each one
 * resolves to. snapmaps/volume/blocking is the only entityDef the navigation
 * reader accepts (16 x 128 x 64, CLIPMODEL_BOX; Snapmap+ does not override
 * it). */
static const struct { const char *name; float size[3]; } NED_BUILTIN[] = {
    { "snapmaps/volume/blocking", { 16.0f, 128.0f, 64.0f } },
};

int sh_nav_entitydef_box_size(const char *name, float size[3])
{
    int i;
    if (!name || !size) return 0;
    for (i = 0; i < (int)(sizeof NED_BUILTIN / sizeof NED_BUILTIN[0]); i++)
        if (!_stricmp(name, NED_BUILTIN[i].name)) {
            memcpy(size, NED_BUILTIN[i].size, sizeof NED_BUILTIN[i].size);
            return 1;
        }
    return 0;
}

int sh_nav_entitydef_default_size(const char *inherit, float size[3], void *ctx)
{
    (void)ctx;
    return sh_nav_entitydef_box_size(inherit, size);
}
