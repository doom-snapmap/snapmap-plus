#include "../src/backend/map_render.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    sh_map_render a,b,untouched;char text[193];
    assert(SH_RENDER_FIELDS==1);
    sh_map_render_default(&a);assert(a.value[0]==8192&&sh_map_render_valid(&a));
    assert(!sh_map_render_distance_override(&a));
    assert(sh_map_render_encode(&a,text,sizeof text)&&!strcmp(text,"4;8192"));
    assert(sh_map_render_decode(text,&b)&&!memcmp(&a,&b,sizeof a));
    assert(sh_map_render_decode("4;60000",&b)&&b.value[0]==60000&&sh_map_render_distance_override(&b));
    assert(sh_map_render_decode("4;0",&b)&&b.value[0]==0&&!sh_map_render_distance_override(&b));
    /* Older versions keep only their distance; their fog values are ignored. */
    assert(sh_map_render_decode("1;60000;70;1500;6500;.35;.4;.45",&b));
    assert(b.value[0]==60000&&sh_map_render_distance_override(&b));
    assert(sh_map_render_decode("3;16000;60;500;1500;.1;.2;.3",&b)&&b.value[0]==16000);
    assert(sh_map_render_decode("3;8192;70;500;1500;.1;.2;.3",&b)&&!sh_map_render_distance_override(&b));
    assert(sh_map_render_decode("1;60000;0;6500;1500;.35;.4;.45",&b)&&b.value[0]==60000);
    assert(sh_map_render_decode("2;1;60000;70;1500;6500;.35;.4;.45",&b)&&b.value[0]==60000);
    /* A disabled version-2 map yields the default, not its dormant distance. */
    assert(sh_map_render_decode("2;0;60000;70;1500;6500;.35;.4;.45",&b));
    assert(!memcmp(&a,&b,sizeof a));
    sh_map_render_adjust(&a,0,200000);
    assert(sh_map_render_encode(&a,text,sizeof text)&&!strcmp(text,"4;200000"));
    assert(sh_map_render_decode(text,&b)&&!memcmp(&a,&b,sizeof a));
    untouched=b;
    assert(!sh_map_render_decode("2;60000;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("2;2;60000;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("5;60000",&b));
    assert(!sh_map_render_decode("4;60000;0",&b));
    assert(!sh_map_render_decode("4;60000junk",&b));
    assert(!sh_map_render_decode("4;",&b));
    assert(!sh_map_render_decode("4;100",&b));
    assert(!sh_map_render_decode("4;300000",&b));
    assert(!sh_map_render_decode("4;nan",&b));
    assert(!sh_map_render_decode("3;100;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("2;0;60000;0;1500;6500;.35;.4;.45extra",&b));
    assert(!sh_map_render_decode("1;nan;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("1;60000;inf;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("1;60000;0;1500;6500;.35;.4;.45junk",&b));
    assert(!sh_map_render_decode("1;60000;0;1500;6500;.35;.4",&b));
    assert(!memcmp(&b,&untouched,sizeof b));
    assert(!sh_map_render_encode(&a,text,4));
    sh_map_render_adjust(&a,0,0);assert(a.value[0]==0&&!sh_map_render_distance_override(&a));
    sh_map_render_adjust(&a,0,1);assert(a.value[0]==256&&sh_map_render_distance_override(&a));
    sh_map_render_adjust(&a,0,8192);assert(!sh_map_render_distance_override(&a));
    sh_map_render_adjust(&a,0,INFINITY);assert(a.value[0]==8192);
    sh_map_render_adjust(&a,0,-5);assert(a.value[0]==0);
    sh_map_render_adjust(&a,0,9e9f);assert(a.value[0]==200000);
    /* Former fog field indices are no longer settings. */
    b=a;sh_map_render_adjust(&a,1,70);assert(!memcmp(&a,&b,sizeof a));
    assert(sh_map_render_valid(&a));puts("map_render_test: passed");return 0;
}
