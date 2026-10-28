#include "../src/backend/map_render.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static int reads_on(const char *text)
{
    sh_map_render s;s.unlocked=7;
    assert(sh_map_render_decode(text,&s)&&sh_map_render_valid(&s));
    return sh_map_render_unlocked(&s);
}
static int refused(const char *text)
{
    sh_map_render s;s.unlocked=7;
    return !sh_map_render_decode(text,&s)&&s.unlocked==7;
}
int main(void)
{
    sh_map_render a;char text[193];int i,j;
    /* Off is native: no override, no text (the variable's absence). */
    sh_map_render_default(&a);assert(!a.unlocked&&sh_map_render_valid(&a)&&!sh_map_render_unlocked(&a));
    assert(sh_map_render_distance(&a)==SH_RENDER_NATIVE_DISTANCE);
    assert(!sh_map_render_encode(&a,text,sizeof text));
    /* On is the maximum far clip and is stored as version 4 with 0. */
    sh_map_render_from_toggle(1,&a);assert(a.unlocked==1&&sh_map_render_unlocked(&a));
    assert(sh_map_render_distance(&a)==SH_RENDER_MAX_DISTANCE&&SH_RENDER_MAX_DISTANCE==200000.0f);
    assert(sh_map_render_encode(&a,text,sizeof text)&&!strcmp(text,"4;0")&&!strcmp(text,SH_RENDER_UNLOCKED_TEXT));
    assert(!sh_map_render_encode(&a,text,3)&&sh_map_render_encode(&a,text,4));
    assert(reads_on(text));
    sh_map_render_from_toggle(5,&a);assert(a.unlocked==1);
    sh_map_render_from_toggle(0,&a);assert(a.unlocked==0);
    /* Anything that is neither 0 nor 1 is not a setting. */
    a.unlocked=2;assert(!sh_map_render_valid(&a)&&!sh_map_render_unlocked(&a));
    assert(sh_map_render_distance(&a)==SH_RENDER_NATIVE_DISTANCE&&!sh_map_render_encode(&a,text,sizeof text));
    assert(!sh_map_render_valid(NULL)&&!sh_map_render_unlocked(NULL)&&sh_map_render_distance(NULL)==8192);
    /* Earlier builds stored a distance. 8192 reads as off; every other valid
     * distance, 0 (the former maximum, and before that "follow") included,
     * reads as on. */
    assert(!reads_on("4;8192")&&!reads_on("4;8192.0"));
    assert(reads_on("4;0")&&reads_on("4;-0")&&reads_on("4;256")&&reads_on("4;60000")&&reads_on("4;200000"));
    assert(reads_on("4;106240")&&reads_on("4;60000.0"));
    assert(reads_on("1;60000;70;1500;6500;.35;.4;.45")&&reads_on("1;0;100;1500;6500;0.35;0.4;0.45"));
    assert(!reads_on("1;8192;70;1500;6500;.35;.4;.45"));
    assert(reads_on("3;16000;60;500;1500;.1;.2;.3")&&!reads_on("3;8192;70;500;1500;.1;.2;.3"));
    assert(reads_on("2;1;60000;70;1500;6500;.35;.4;.45")&&!reads_on("2;1;8192;70;1500;6500;.35;.4;.45"));
    /* A disabled version-2 map is off, not its dormant distance. */
    assert(!reads_on("2;0;60000;70;1500;6500;.35;.4;.45")&&!reads_on("2;0;0;80;500;1500;.1;.2;.3"));
    /* Malformed or out-of-range text is refused and leaves the output alone. */
    assert(refused("2;60000;0;1500;6500;.35;.4;.45")&&refused("2;2;60000;0;1500;6500;.35;.4;.45"));
    assert(refused("5;1")&&refused("5;60000")&&refused("4;60000;0")&&refused("4;60000junk")&&refused("4;"));
    assert(refused("4;100")&&refused("4;300000")&&refused("4;nan")&&refused("4;inf")&&refused("4;-5"));
    assert(refused("3;100;0;1500;6500;.35;.4;.45")&&refused("2;0;60000;0;1500;6500;.35;.4;.45extra"));
    assert(refused("1;nan;0;1500;6500;.35;.4;.45")&&refused("1;60000;inf;1500;6500;.35;.4;.45"));
    assert(refused("1;60000;0;1500;6500;.35;.4;.45junk")&&refused("1;60000;0;1500;6500;.35;.4"));
    assert(refused("")&&refused("on")&&!sh_map_render_decode(NULL,&a)&&!sh_map_render_decode("4;0",NULL));
    /* Snapmap+'s own settings are the "smp." variables; author strings and
     * other Snapmap+ carriers are not. */
    assert(sh_map_render_own_variable("smp.bounds.v1",13)&&sh_map_render_own_variable(SH_RENDER_VARIABLE,13));
    assert(!sh_map_render_own_variable("String 1",8)&&!sh_map_render_own_variable("smpkg.x.0.1.abcd",16));
    assert(!sh_map_render_own_variable("smp.",4)&&!sh_map_render_own_variable(NULL,13)&&!sh_map_render_own_variable("Smp.x",5));
    /* The fog stretch: r = 200000 / native, floored like the engine's read
     * (256) and never below 1. */
    assert(fabsf(sh_map_render_fog_stretch(60000)-3.3333333f)<1e-5f);
    assert(fabsf(sh_map_render_fog_stretch(8192)-24.414062f)<1e-4f);
    assert(sh_map_render_fog_stretch(200000)==1&&sh_map_render_fog_stretch(256000)==1);
    assert(sh_map_render_fog_stretch(100)==781.25f&&sh_map_render_fog_stretch(0)==781.25f);
    assert(sh_map_render_fog_stretch(NAN)==781.25f&&sh_map_render_fog_stretch(INFINITY)==781.25f);
    /* snapmap/base at its native 60000: fogStart 32000 -> about 106667,
     * fogScale 0.005 -> 0.0015. */
    {
        float r=sh_map_render_fog_stretch(60000);
        assert(fabsf(sh_map_render_fog_distance(32000,r)-106666.67f)<0.1f);
        assert(fabsf(sh_map_render_fog_strength(0.005f,r)-0.0015f)<1e-7f);
        assert(sh_map_render_fog_distance(0,r)==0&&sh_map_render_fog_strength(0,r)==0);
    }
    /* r of 1 (or anything not above it) changes nothing: no shrinking, no
     * thicker fog for an environment that already reaches the maximum. */
    assert(sh_map_render_fog_distance(500,1)==500&&sh_map_render_fog_strength(0.002f,1)==0.002f);
    assert(sh_map_render_fog_distance(500,0.5f)==500&&sh_map_render_fog_strength(0.002f,0.5f)==0.002f);
    assert(sh_map_render_fog_distance(500,NAN)==500&&sh_map_render_fog_strength(0.002f,NAN)==0.002f);
    /* The stretched parms are the engine's lowercase names, distinct, and
     * none of them is the far clip, a colour or the maximum opacity. */
    {
        const char *all[SH_RENDER_FOG_DISTANCE_COUNT+SH_RENDER_FOG_STRENGTH_COUNT];int n=0;
        assert(SH_RENDER_FOG_DISTANCE_COUNT==3&&SH_RENDER_FOG_STRENGTH_COUNT==2);
        assert(!strcmp(sh_map_render_fog_distance_parms[0],"fogstart"));
        assert(!strcmp(sh_map_render_fog_distance_parms[1],"fogend"));
        assert(!strcmp(sh_map_render_fog_distance_parms[2],"fogstartdistance"));
        assert(!strcmp(sh_map_render_fog_strength_parms[0],"fogscale"));
        assert(!strcmp(sh_map_render_fog_strength_parms[1],"fogdensity"));
        for(i=0;i<SH_RENDER_FOG_DISTANCE_COUNT;i++)all[n++]=sh_map_render_fog_distance_parms[i];
        for(i=0;i<SH_RENDER_FOG_STRENGTH_COUNT;i++)all[n++]=sh_map_render_fog_strength_parms[i];
        for(i=0;i<n;i++) {
            for(j=0;all[i][j];j++)assert(!(all[i][j]>='A'&&all[i][j]<='Z'));
            for(j=i+1;j<n;j++)assert(strcmp(all[i],all[j]));
            assert(strcmp(all[i],"maxviewdistance")&&strcmp(all[i],"fogcolor")&&strcmp(all[i],"fogmaxopacity"));
        }
    }
    puts("map_render_test: passed");return 0;
}
