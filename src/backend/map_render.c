/* Compact, versioned settings carried by one native map string variable. */
#include "map_render.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void sh_map_render_default(sh_map_render *s)
{
    static const float defaults[SH_RENDER_FIELDS]={SH_RENDER_NATIVE_DISTANCE,0,1500,6500,.35f,.4f,.45f};
    memcpy(s->value,defaults,sizeof defaults);
}
int sh_map_render_distance_override(const sh_map_render *s)
{
    return s&&s->value[0]>0&&s->value[0]!=SH_RENDER_NATIVE_DISTANCE;
}
int sh_map_render_fog_override(const sh_map_render *s)
{
    return s&&s->value[1]>0;
}
int sh_map_render_valid(const sh_map_render *s)
{
    unsigned i;if(!s)return 0;
    for(i=0;i<SH_RENDER_FIELDS;i++)if(!isfinite(s->value[i]))return 0;
    return (s->value[0]==0||(s->value[0]>=256&&s->value[0]<=200000))&&
        s->value[1]>=0&&s->value[1]<=100&&
        s->value[2]>=0&&s->value[2]<s->value[3]&&s->value[3]<=200000&&
        s->value[4]>=0&&s->value[4]<=1&&s->value[5]>=0&&s->value[5]<=1&&
        s->value[6]>=0&&s->value[6]<=1;
}
int sh_map_render_decode(const char *text,sh_map_render *out)
{
    sh_map_render s;int end=0,n,native=0;
    if(!text||!out||strlen(text)>192)return 0;
    if(!strncmp(text,"1;",2)||!strncmp(text,"3;",2)) {
        n=sscanf_s(text+2,"%f;%f;%f;%f;%f;%f;%f%n",
            &s.value[0],&s.value[1],&s.value[2],&s.value[3],&s.value[4],&s.value[5],&s.value[6],&end);
        if(end)end+=2;
    } else if(!strncmp(text,"2;0;",4)||!strncmp(text,"2;1;",4)) {
        native=text[2]=='0';
        n=sscanf_s(text+4,"%f;%f;%f;%f;%f;%f;%f%n",
            &s.value[0],&s.value[1],&s.value[2],&s.value[3],&s.value[4],&s.value[5],&s.value[6],&end);
        if(end)end+=4;
    } else return 0;
    if(n!=7||!end||text[end]||!sh_map_render_valid(&s))return 0;
    /* An old disabled map must not silently activate its dormant values when
     * the former UI switch disappears. New saves contain values only. */
    if(native)sh_map_render_default(&s);
    *out=s;return 1;
}
int sh_map_render_encode(const sh_map_render *s,char *out,size_t cap)
{
    int n;if(!out||!cap||!sh_map_render_valid(s))return 0;
    n=snprintf(out,cap,"3;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g;%.9g",
        s->value[0],s->value[1],s->value[2],s->value[3],s->value[4],s->value[5],s->value[6]);
    return n>0&&(size_t)n<cap;
}
void sh_map_render_adjust(sh_map_render *s,unsigned field,float value)
{
    static const float low[SH_RENDER_FIELDS]={0,0,0,1,0,0,0};
    static const float high[SH_RENDER_FIELDS]={200000,100,199999,200000,1,1,1};
    if(!s||field>=SH_RENDER_FIELDS||!isfinite(value))return;
    s->value[field]=fminf(high[field],fmaxf(low[field],value));
    if(field==0&&s->value[0]>0&&s->value[0]<256)s->value[0]=256;
    if(field==2&&s->value[2]>=s->value[3])s->value[3]=s->value[2]+1;
    if(field==3&&s->value[3]<=s->value[2])s->value[2]=s->value[3]-1;
}
