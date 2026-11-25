/* The saved map's render toggle and its fog adjustments. */
#include "map_render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void sh_map_render_default(sh_map_render *s)
{
    if(s)s->unlocked=0;
}
void sh_map_render_from_toggle(int on,sh_map_render *s)
{
    if(s)s->unlocked=on!=0;
}
int sh_map_render_unlocked(const sh_map_render *s)
{
    return sh_map_render_valid(s)&&s->unlocked;
}
float sh_map_render_distance(const sh_map_render *s)
{
    return sh_map_render_unlocked(s)?SH_RENDER_MAX_DISTANCE:SH_RENDER_NATIVE_DISTANCE;
}
const char *const sh_map_render_fog_distance_parms[SH_RENDER_FOG_DISTANCE_COUNT]={
    "fogstart","fogend","fogstartdistance"
};
const char *const sh_map_render_fog_strength_parms[SH_RENDER_FOG_STRENGTH_COUNT]={
    "fogscale","fogdensity"
};
float sh_map_render_fog_stretch(float native)
{
    float r;
    if(!isfinite(native)||native<SH_RENDER_MIN_DISTANCE)native=SH_RENDER_MIN_DISTANCE;
    r=SH_RENDER_MAX_DISTANCE/native;
    return r>1?r:1;
}
float sh_map_render_fog_distance(float value,float r)
{
    return isfinite(r)&&r>1?value*r:value;
}
float sh_map_render_fog_strength(float value,float r)
{
    return isfinite(r)&&r>1?value/r:value;
}
int sh_map_render_valid(const sh_map_render *s)
{
    return s&&(s->unlocked==0||s->unlocked==1);
}
int sh_map_render_decode(const char *text,sh_map_render *out)
{
    if (!out || !text || strcmp(text, SH_RENDER_UNLOCKED_TEXT)) return 0;
    out->unlocked = 1;
    return 1;
}
int sh_map_render_encode(const sh_map_render *s,char *out,size_t cap)
{
    if(!out||!cap||!sh_map_render_unlocked(s)||cap<sizeof SH_RENDER_UNLOCKED_TEXT)return 0;
    memcpy(out,SH_RENDER_UNLOCKED_TEXT,sizeof SH_RENDER_UNLOCKED_TEXT);
    return 1;
}
