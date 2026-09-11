#include "grid_room_edit.h"
#include <string.h>
#include <math.h>
static float inverse(const sh_grid_axis *a,float value)
{
    unsigned i;
    if(value<a->target[0])return value+a->source[0]-a->target[0];
    for(i=1;i<a->count;++i)if(value<a->target[i])
        return a->source[i-1]+(value-a->target[i-1])*
            (a->source[i]-a->source[i-1])/(a->target[i]-a->target[i-1]);
    return value+a->source[a->count-1]-a->target[a->count-1];
}
int sh_grid_cap_transform(const sh_grid_size *before,const sh_grid_size *after,
                          const float source[12],float result[12])
{
    sh_grid_warp old,next;unsigned i;
    if(!before||!after||before->kind!=after->kind||!source||!result||
       !sh_grid_warp_init(before,&old)||!sh_grid_warp_init(after,&next))return 0;
    for(i=0;i<12;++i)if(!isfinite(source[i]))return 0;
    /* idSnapEntity stores local origin first, followed by its orientation.
     * Door/frame dimensions and orientation are never deformed. */
    memmove(result,source,12*sizeof(float));
    for(i=0;i<3;++i)result[i]=sh_grid_coordinate(&next,i,inverse(&old.axis[i],source[i]));
    return 1;
}
