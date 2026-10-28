/* Compact, versioned settings carried by one native map string variable. */
#include "map_render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void sh_map_render_default(sh_map_render *s)
{
    s->value[SH_RENDER_DISTANCE]=SH_RENDER_NATIVE_DISTANCE;
}
int sh_map_render_distance_override(const sh_map_render *s)
{
    return s&&s->value[SH_RENDER_DISTANCE]!=SH_RENDER_NATIVE_DISTANCE;
}
float sh_map_render_distance(const sh_map_render *s)
{
    float d;if(!s)return SH_RENDER_NATIVE_DISTANCE;
    d=s->value[SH_RENDER_DISTANCE];
    return d==0?SH_RENDER_MAX_DISTANCE:d;
}
float sh_map_render_step(float previous,float value)
{
    /* A native step moves by one small or large step (4096 at most); a wrap
     * crosses nearly the whole range, so half the range separates them. */
    const float half=SH_RENDER_MAX_DISTANCE*0.5f;
    if(!isfinite(value))return previous;
    if(!isfinite(previous))return value;
    if(value-previous>half)return previous<=0?SH_RENDER_MAX_DISTANCE:0;
    if(previous-value>half)return SH_RENDER_MAX_DISTANCE;
    return value;
}
int sh_map_render_valid(const sh_map_render *s)
{
    float d;if(!s)return 0;
    d=s->value[SH_RENDER_DISTANCE];
    return isfinite(d)&&(d==0||(d>=SH_RENDER_MIN_DISTANCE&&d<=SH_RENDER_MAX_DISTANCE));
}
/* Version 4 stores the distance only. Versions 1 and 3 stored distance
 * followed by six fog values, and version 2 added an enable flag; fog is
 * no longer a map setting, so only their distance is kept. */
int sh_map_render_decode(const char *text,sh_map_render *out)
{
    sh_map_render s;float legacy[7];int end=0,n,skip,native=0,i;
    if(!text||!out||strlen(text)>192)return 0;
    if(!strncmp(text,"4;",2)) {
        n=sscanf_s(text+2,"%f%n",&s.value[SH_RENDER_DISTANCE],&end);
        if(n!=1||!end||text[2+end]||!sh_map_render_valid(&s))return 0;
        *out=s;return 1;
    }
    if(!strncmp(text,"1;",2)||!strncmp(text,"3;",2))skip=2;
    else if(!strncmp(text,"2;0;",4)||!strncmp(text,"2;1;",4)){skip=4;native=text[2]=='0';}
    else return 0;
    n=sscanf_s(text+skip,"%f;%f;%f;%f;%f;%f;%f%n",
        &legacy[0],&legacy[1],&legacy[2],&legacy[3],&legacy[4],&legacy[5],&legacy[6],&end);
    if(n!=7||!end||text[skip+end])return 0;
    for(i=0;i<7;i++)if(!isfinite(legacy[i]))return 0;
    s.value[SH_RENDER_DISTANCE]=legacy[0];
    if(!sh_map_render_valid(&s))return 0;
    /* An old disabled map must not silently activate its dormant distance
     * when the former UI switch disappears. */
    if(native)sh_map_render_default(&s);
    *out=s;return 1;
}
int sh_map_render_encode(const sh_map_render *s,char *out,size_t cap)
{
    int n;if(!out||!cap||!sh_map_render_valid(s))return 0;
    n=snprintf(out,cap,"4;%.9g",s->value[SH_RENDER_DISTANCE]);
    return n>0&&(size_t)n<cap;
}
void sh_map_render_adjust(sh_map_render *s,unsigned field,float value)
{
    if(!s||field!=SH_RENDER_DISTANCE||!isfinite(value))return;
    value=fminf(SH_RENDER_MAX_DISTANCE,fmaxf(0,value));
    if(value>0&&value<SH_RENDER_MIN_DISTANCE)value=SH_RENDER_MIN_DISTANCE;
    s->value[field]=value;
}
