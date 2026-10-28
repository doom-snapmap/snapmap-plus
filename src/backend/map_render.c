/* Compact, versioned settings carried by one native map string variable. */
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
/* A distance the former numeric row could store: 0 (the maximum) or
 * 256..200000. */
static int stored_distance(float d)
{
    return isfinite(d)&&(d==0||(d>=SH_RENDER_MIN_DISTANCE&&d<=SH_RENDER_MAX_DISTANCE));
}
int sh_map_render_decode(const char *text,sh_map_render *out)
{
    float d,legacy[7];int end=0,n,skip,native=0,i;
    if(!text||!out||strlen(text)>192)return 0;
    if(!strncmp(text,"4;",2)) {
        n=sscanf_s(text+2,"%f%n",&d,&end);
        if(n!=1||!end||text[2+end]||!stored_distance(d))return 0;
        out->unlocked=d!=SH_RENDER_NATIVE_DISTANCE;return 1;
    }
    if(!strncmp(text,"1;",2)||!strncmp(text,"3;",2))skip=2;
    else if(!strncmp(text,"2;0;",4)||!strncmp(text,"2;1;",4)){skip=4;native=text[2]=='0';}
    else return 0;
    n=sscanf_s(text+skip,"%f;%f;%f;%f;%f;%f;%f%n",
        &legacy[0],&legacy[1],&legacy[2],&legacy[3],&legacy[4],&legacy[5],&legacy[6],&end);
    if(n!=7||!end||text[skip+end])return 0;
    for(i=0;i<7;i++)if(!isfinite(legacy[i]))return 0;
    if(!stored_distance(legacy[0]))return 0;
    /* An old disabled map must not silently activate its dormant distance
     * when the former UI switch disappears. */
    out->unlocked=!native&&legacy[0]!=SH_RENDER_NATIVE_DISTANCE;
    return 1;
}
int sh_map_render_encode(const sh_map_render *s,char *out,size_t cap)
{
    if(!out||!cap||!sh_map_render_unlocked(s)||cap<sizeof SH_RENDER_UNLOCKED_TEXT)return 0;
    memcpy(out,SH_RENDER_UNLOCKED_TEXT,sizeof SH_RENDER_UNLOCKED_TEXT);
    return 1;
}
int sh_map_render_own_variable(const char *name,int length)
{
    return name&&length>4&&!memcmp(name,"smp.",4);
}
