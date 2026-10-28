#include "../src/backend/map_render_native.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int no_metadata_logs,converted_logs;
void backend_log(const char *s)
{
    if(strstr(s,"no valid map rendering metadata"))no_metadata_logs++;
    if(strstr(s,"converted to version 4"))converted_logs++;
}
/* editor_zoom.c owns the Blueprint condition; here it is a switch. */
static int blueprint;static float blueprint_far=66000;
float sh_editor_zoom_far_plane(float v){return blueprint&&v<blueprint_far?blueprint_far:v;}
uintptr_t glb_resolve(const uint8_t *b,const char *n,glb_status *s)
{(void)b;(void)n;(void)s;return 0;}
static unsigned char resources[6][16];
static void *lookup(void *op){return resources[*(unsigned short*)((unsigned char*)op+2)];}
static float get_float(void *b,void *d,uintptr_t c)
{(void)d;(void)c;return (*(float**)((unsigned char*)b+24))[4];}
static void row(unsigned char *r,const char *name,const char *value)
{
    *(int*)(r+8)=(int)strlen(name);*(const char**)(r+16)=name;
    *(int*)(r+64)=(int)strlen(value);*(const char**)(r+72)=value;
}
static unsigned char heap_state[0xc8],storage[8*104];
static char strings[16][193];
static int assigns,fail_resize;
static void *heap_get(void){return heap_state;}
static void heap_push(void *h,int id)
{int *depth=(int*)((unsigned char*)h+0xc4);assert(id==0);*(int*)((unsigned char*)h+0x44+(*depth)++*4)=id;}
static void heap_pop(void *h){--*(int*)((unsigned char*)h+0xc4);}
static unsigned char resize_list(void *p,int count)
{
    unsigned char *list=p;int n=*(int*)(list+8);
    assert(*(int*)(heap_state+0xc4)==2&&*(int*)(heap_state+0x48)==0);
    if(fail_resize)return 0;
    assert(count<=8);if(n)memcpy(storage,*(void**)list,(size_t)n*104);
    *(void**)list=storage;*(int*)(list+12)=count;return 1;
}
static void assign_string(void *p,const char *s)
{
    unsigned char *str=p;assert(assigns<16&&strlen(s)<193&&*(int*)(heap_state+0xc4)==2);
    strcpy_s(strings[assigns],193,s);*(int*)(str+8)=(int)strlen(s);*(char**)(str+16)=strings[assigns++];
}
int main(void)
{
    unsigned char saved[0x770]={0},edit[0x800]={0},rows[208]={0},block[48]={0},ops[48]={0};
    const char *names[]={"unrelated","maxviewdistance","fogcolor","fogscale","fogend","fogstart"};
    float values[24];sh_map_render settings;int game=1,i,j;
    sh_map_render_default(&settings);
    *(void**)(saved+0x1a0)=rows;*(int*)(saved+0x1a8)=1;*(int*)(saved+0x1ac)=2;
    row(rows,SH_RENDER_VARIABLE,"1;60000;100;1500;6500;0.35;0.4;0.45");
    sh_map_render_loaded(saved);assert(g_ready&&g_runtime.value[0]==60000);
    {
        sh_map_render retained;
        row(rows,SH_RENDER_VARIABLE,"4;16000");
        assert(sh_map_render_capture(saved,&retained));
        assert(g_ready&&g_runtime.value[0]==60000);
        row(rows,SH_RENDER_VARIABLE,"malformed");
        sh_map_render_select(&retained);
        assert(g_ready&&g_runtime.value[0]==16000);
        assert(!sh_map_render_capture(saved,&retained));
        assert(g_ready&&g_runtime.value[0]==16000);
        retained.value[0]=-1;sh_map_render_select(&retained);assert(!g_ready);
        row(rows,SH_RENDER_VARIABLE,"1;60000;100;1500;6500;0.35;0.4;0.45");
        sh_map_render_loaded(saved);
    }
    g_game_type=&game;g_float=get_float;g_decl=lookup;
    *(void**)block=ops;*(int*)(block+8)=6;*(void**)(block+24)=values;*(int*)(block+32)=6;
    for(i=0;i<6;i++){
        *(const char**)(resources[i]+8)=names[i];
        *(unsigned short*)(ops+i*8)=15360;*(unsigned short*)(ops+i*8+2)=(unsigned short)i;
    }
    /* A version-1 map with fog: only the distance literal changes. */
    for(i=0;i<24;i++)values[i]=(float)(i+42);
    assert(render_clip(block,NULL,0)==60000);
    for(j=0;j<4;j++)assert(values[4+j]==60000);
    for(i=0;i<24;i++)if(i<4||i>=8)assert(values[i]==(float)(i+42));
    /* A fresh map and a failed load cannot inherit the previous map's distance. */
    for(i=0;i<24;i++)values[i]=(float)(i+42);
    sh_map_render_build(edit);assert(g_ready&&!sh_map_render_distance_override(&g_runtime));
    assert(render_clip(block,NULL,0)==46);for(i=0;i<24;i++)assert(values[i]==(float)(i+42));
    row(rows,SH_RENDER_VARIABLE,"2;0;200000;80;500;1500;.1;.2;.3");
    sh_map_render_loaded(saved);assert(g_ready&&!sh_map_render_distance_override(&g_runtime));
    render_clip(block,NULL,0);for(i=0;i<24;i++)assert(values[i]==(float)(i+42));
    /* Default distance leaves whatever the blend produced: module layers,
     * Set Environment nodes and campaign environments stay in control. */
    row(rows,SH_RENDER_VARIABLE,"4;8192");
    sh_map_render_loaded(saved);values[4]=15000;assert(render_clip(block,NULL,0)==15000);
    /* Zero is the maximum distance: the literal and the returned value are
     * 200000, never the 0 the engine would raise to 256. */
    row(rows,SH_RENDER_VARIABLE,"4;0");
    sh_map_render_loaded(saved);values[4]=15000;
    assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE&&values[4]==SH_RENDER_MAX_DISTANCE);
    for(i=0;i<24;i++)if(i<4||i>=8)assert(values[i]==(float)(i+42));
    for(j=0;j<4;j++)values[4+j]=(float)(4+j+42);
    /* A custom distance wins over any blended value, including one that a
     * Set Environment node or the Lazarus environment produced. */
    row(rows,SH_RENDER_VARIABLE,"4;60000");
    sh_map_render_loaded(saved);values[4]=15000;assert(render_clip(block,NULL,0)==60000&&values[4]==60000);
    values[4]=100000;assert(render_clip(block,NULL,0)==60000&&values[4]==60000);
    /* The returned distance is authoritative even without a literal slot. */
    *(unsigned short*)(ops+8)=15361;values[4]=8192;
    assert(render_clip(block,NULL,0)==60000&&values[4]==8192);
    *(unsigned short*)(ops+8)=15360;
    sh_map_render_loaded(NULL);assert(!g_ready);values[4]=8192;
    assert(render_clip(block,NULL,0)==8192);
    sh_map_render_loaded(saved);game=0;
    assert(render_clip(block,NULL,0)==8192);game=1;
    /* In Blueprint frames the editor and the blend agree on the larger far
     * plane; a larger map distance still wins. Outside them nothing changes. */
    values[4]=8192;blueprint=1;assert(render_clip(block,NULL,0)==66000);
    row(rows,SH_RENDER_VARIABLE,"4;100000");sh_map_render_loaded(saved);
    assert(render_clip(block,NULL,0)==100000);blueprint=0;assert(render_clip(block,NULL,0)==100000);
    row(rows,SH_RENDER_VARIABLE,"4;60000");sh_map_render_loaded(saved);values[4]=8192;
    /* Reject duplicate metadata. An incomplete parameter block is not
     * written, but the map distance is still returned. */
    row(rows+104,SH_RENDER_VARIABLE,"4;8192");
    *(int*)(saved+0x1a8)=2;sh_map_render_loaded(saved);assert(!g_ready);
    assert(render_clip(block,NULL,0)==8192);
    *(int*)(saved+0x1a8)=1;sh_map_render_loaded(saved);
    *(int*)(block+32)=5;values[4]=8192;assert(render_clip(block,NULL,0)==60000&&values[4]==8192);
    *(int*)(block+32)=6;
    /* The clear before every load is silent; a loaded map without valid
     * metadata still says so. */
    no_metadata_logs=0;sh_map_render_loaded(NULL);assert(!g_ready&&no_metadata_logs==0);
    row(rows,SH_RENDER_VARIABLE,"malformed");*(int*)(saved+0x1a8)=1;
    sh_map_render_loaded(saved);assert(!g_ready&&no_metadata_logs==1);
    /* Apply writes the editable map: it appends without moving another
     * string's index and returns to the caller's original heap. */
    g_resize=resize_list;g_assign=assign_string;
    g_heap.get=heap_get;g_heap.push=heap_push;g_heap.pop=heap_pop;
    *(int*)(heap_state+0xc4)=1;*(int*)(heap_state+0x44)=2;
    memset(rows,0,sizeof rows);row(rows,"author_variable","leave this value");
    *(void**)(edit+0xa8)=rows;*(int*)(edit+0xb0)=1;*(int*)(edit+0xb4)=1;
    *(int*)(edit+0x5c8)=12;
    settings.value[0]=8192;assert(sh_map_render_write(edit,&settings)&&assigns==2);
    assert(*(int*)(heap_state+0xc4)==1&&*(int*)(heap_state+0x44)==2);
    assert(*(int*)(edit+0xb0)==2&&*(int*)(edit+0x5c8)==12);
    assert(!strcmp(*(char**)(storage+16),"author_variable")&&!strcmp(*(char**)(storage+72),"leave this value"));
    assert(sh_map_render_read(edit,&settings)&&settings.value[0]==8192);
    assert(!strcmp(*(char**)(storage+104+72),"4;8192"));
    settings.value[0]=60000;assert(sh_map_render_write(edit,&settings)&&assigns==3);
    settings.value[0]=8192;assert(sh_map_render_write(edit,&settings)&&assigns==4);
    assert(sh_map_render_write(edit,&settings)&&assigns==4);
    assert(sh_map_render_read(edit,&settings)&&!sh_map_render_distance_override(&settings));
    /* Play conversion receives a build copy, not the editable map: it only
     * publishes and never writes, even for an older value. */
    row(storage+104,SH_RENDER_VARIABLE,"3;60000;70;500;1500;.1;.2;.3");
    sh_map_render_build(edit);assert(assigns==4&&g_ready&&g_runtime.value[0]==60000);
    assert(!strcmp(*(char**)(storage+104+72),"3;60000;70;500;1500;.1;.2;.3"));
    {
        unsigned char edit2[0x800]={0},list2[208]={0};
        *(void**)(edit2+0xa8)=list2;row(list2,"author_variable","x");*(int*)(edit2+0xb0)=1;*(int*)(edit2+0xb4)=2;
        sh_map_render_build(edit2);assert(assigns==4&&*(int*)(edit2+0xb0)==1&&g_ready&&!sh_map_render_distance_override(&g_runtime));
    }
    /* Loading migrates the deserialized snapshot in place, so the editable
     * map built from it saves version 4. Row indices, the row count, other
     * rows and allocCount[4] (+0x6c0) stay as they were. */
    {
        unsigned char snap[0x770]={0},list[208]={0};
        *(void**)(snap+0x1a0)=list;*(int*)(snap+0x1a8)=2;*(int*)(snap+0x1ac)=2;*(int*)(snap+0x6c0)=7;
        row(list,"author_variable","leave this value");
        row(list+104,SH_RENDER_VARIABLE,"3;60000;70;1500;6500;.35;.4;.45");
        converted_logs=0;sh_map_render_loaded(snap);
        assert(assigns==5&&converted_logs==1&&g_ready&&g_runtime.value[0]==60000);
        assert(!strcmp(*(char**)(list+104+72),"4;60000")&&*(int*)(list+104+64)==7);
        assert(!strcmp(*(char**)(list+72),"leave this value"));
        assert(*(int*)(snap+0x1a8)==2&&*(int*)(snap+0x6c0)==7);
        assert(*(int*)(heap_state+0xc4)==1&&*(int*)(heap_state+0x44)==2);
        /* Already canonical: nothing is assigned or logged. */
        sh_map_render_loaded(snap);assert(assigns==5&&converted_logs==1);
        /* The former disabled mode becomes the default distance. */
        row(list+104,SH_RENDER_VARIABLE,"2;0;60000;70;1500;6500;.35;.4;.45");
        sh_map_render_loaded(snap);
        assert(assigns==6&&!strcmp(*(char**)(list+104+72),"4;8192")&&!sh_map_render_distance_override(&g_runtime));
        /* A non-canonical version-4 spelling is normalized too. */
        row(list+104,SH_RENDER_VARIABLE,"4;60000.0");
        sh_map_render_loaded(snap);assert(assigns==7&&!strcmp(*(char**)(list+104+72),"4;60000"));
        /* A map without the variable stays free of it. */
        row(list+104,"another_variable","1");
        sh_map_render_loaded(snap);assert(assigns==7&&*(int*)(snap+0x1a8)==2&&g_ready);
        /* Malformed metadata is never rewritten. */
        row(list+104,SH_RENDER_VARIABLE,"3;60000");
        sh_map_render_loaded(snap);assert(assigns==7&&!g_ready);
        /* When the persistent heap cannot be entered (not the engine thread),
         * the value is left as it was and still selected. */
        row(list+104,SH_RENDER_VARIABLE,"1;60000;70;1500;6500;.35;.4;.45");
        *(int*)(heap_state+0xc4)=32;converted_logs=0;
        sh_map_render_loaded(snap);
        assert(assigns==7&&converted_logs==0&&g_ready&&g_runtime.value[0]==60000);
        assert(!strcmp(*(char**)(list+104+72),"1;60000;70;1500;6500;.35;.4;.45"));
        *(int*)(heap_state+0xc4)=1;
        /* Before install (no assign binding) loading only selects. */
        g_assign=NULL;sh_map_render_loaded(snap);assert(assigns==7&&g_ready);g_assign=assign_string;
    }
    memset(edit,0,sizeof edit);fail_resize=1;
    assert(!sh_map_render_write(edit,&settings)&&*(int*)(edit+0xb0)==0&&*(int*)(heap_state+0xc4)==1);
    puts("map_render_native_test: passed");return 0;
}
