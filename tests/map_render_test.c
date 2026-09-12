#include "../src/backend/map_render.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    sh_map_render a,b,untouched;char text[193];
    sh_map_render_default(&a);assert(a.value[0]==8192&&a.value[1]==0&&sh_map_render_valid(&a));
    assert(!sh_map_render_distance_override(&a)&&!sh_map_render_fog_override(&a));
    assert(sh_map_render_encode(&a,text,sizeof text)&&sh_map_render_decode(text,&b));
    assert(!memcmp(&a,&b,sizeof a));
    assert(!strncmp(text,"3;",2));
    assert(sh_map_render_decode("1;60000;70;1500;6500;.35;.4;.45",&b));
    assert(b.value[0]==60000&&b.value[1]==70&&sh_map_render_distance_override(&b)&&sh_map_render_fog_override(&b));
    assert(sh_map_render_decode("2;1;60000;70;1500;6500;.35;.4;.45",&b));
    assert(b.value[0]==60000&&b.value[1]==70);
    assert(sh_map_render_decode("2;0;60000;70;1500;6500;.35;.4;.45",&b));
    assert(!memcmp(&a,&b,sizeof a));
    sh_map_render_adjust(&a,0,200000);sh_map_render_adjust(&a,1,100);
    sh_map_render_adjust(&a,4,.123456789f);sh_map_render_adjust(&a,5,1);sh_map_render_adjust(&a,6,0);
    assert(sh_map_render_encode(&a,text,sizeof text)&&sh_map_render_decode(text,&b)&&!memcmp(&a,&b,sizeof a));
    untouched=b;
    assert(!sh_map_render_decode("2;60000;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("2;2;60000;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("4;60000;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("3;100;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("2;0;60000;0;1500;6500;.35;.4;.45extra",&b));
    assert(!sh_map_render_decode("1;nan;0;1500;6500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("1;60000;0;6500;1500;.35;.4;.45",&b));
    assert(!sh_map_render_decode("1;60000;0;1500;6500;.35;.4;.45junk",&b));
    assert(!sh_map_render_decode("1;60000;0;1500;6500;.35;.4",&b));
    assert(!memcmp(&b,&untouched,sizeof b));
    assert(!sh_map_render_encode(&a,text,4));
    sh_map_render_adjust(&a,2,200000);assert(a.value[2]==199999&&a.value[3]==200000);
    sh_map_render_adjust(&a,3,0);assert(a.value[2]==0&&a.value[3]==1);
    sh_map_render_adjust(&a,0,0);assert(a.value[0]==0&&!sh_map_render_distance_override(&a));
    sh_map_render_adjust(&a,0,1);assert(a.value[0]==256&&sh_map_render_distance_override(&a));
    sh_map_render_adjust(&a,0,8192);assert(!sh_map_render_distance_override(&a));
    sh_map_render_adjust(&a,1,INFINITY);assert(a.value[1]==100);
    sh_map_render_adjust(&a,1,0);assert(!sh_map_render_fog_override(&a));
    assert(!sh_map_render_distance_override(&a));
    /* Dormant fog range/color do not keep an override active. */
    assert(a.value[3]==1&&a.value[4]!=.35f&&!sh_map_render_fog_override(&a));
    assert(sh_map_render_valid(&a));puts("map_render_test: passed");return 0;
}
