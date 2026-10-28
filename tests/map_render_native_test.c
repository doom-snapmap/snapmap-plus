#include "../src/backend/map_render_native.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static int no_metadata_logs,converted_logs,removed_logs,kept_logs,unlocked_logs,stored_logs,off_logs;
void backend_log(const char *s)
{
    if(strstr(s,"no valid map rendering metadata"))no_metadata_logs++;
    if(strstr(s,"read as Unlock View Distance on; stored as 4;0"))converted_logs++;
    if(strstr(s,"native view distance variable removed"))removed_logs++;
    if(strstr(s,"native view distance variable kept")||strstr(s,"stays as the native 4;8192"))kept_logs++;
    if(strstr(s,"unlocked view distance; far clip 200000, environment fog stretched to match"))unlocked_logs++;
    if(strstr(s,"Unlock View Distance stored"))stored_logs++;
    if(strstr(s,"no Snapmap+ view variable in this map"))off_logs++;
}
/* editor_zoom.c owns the Blueprint condition; here it is a switch. */
static int blueprint;static float blueprint_far=66000,far_floor=-1;
float sh_editor_zoom_far_plane(float v){return blueprint&&v<blueprint_far?blueprint_far:v;}
void sh_editor_zoom_set_far_floor(float v){far_floor=v;}
uintptr_t glb_resolve(const uint8_t *b,const char *n,glb_status *s)
{(void)b;(void)n;(void)s;return 0;}
/* r_zfar: idCVar with its float value at +0x34; Set(cvar, text, force). */
static unsigned char zfar[0x40];static int zfar_sets;static char zfar_text[16];
static unsigned char set_cvar(void *cvar,const char *text,unsigned char force)
{
    assert(cvar==zfar&&force==1&&strlen(text)<sizeof zfar_text);
    strcpy_s(zfar_text,sizeof zfar_text,text);*(float*)(zfar+0x34)=(float)atof(text);zfar_sets++;return 1;
}
static float zfar_value(void){return *(float*)(zfar+0x34);}
static unsigned char resources[9][16];
static void *lookup(void *op){return resources[*(unsigned short*)((unsigned char*)op+2)];}
static float get_float(void *b,void *d,uintptr_t c)
{(void)d;(void)c;return (*(float**)((unsigned char*)b+24))[4];}
static void row(unsigned char *r,const char *name,const char *value)
{
    *(int*)(r+8)=(int)strlen(name);*(const char**)(r+16)=name;
    *(int*)(r+64)=(int)strlen(value);*(const char**)(r+72)=value;
}
static const char *row_name(unsigned char *list,int i){return *(char**)(*(unsigned char**)list+(size_t)i*104+16);}
static const char *row_value(unsigned char *list,int i){return *(char**)(*(unsigned char**)list+(size_t)i*104+72);}
static unsigned char heap_state[0xc8],storage[8*104];
static char strings[32][193];
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
    assert(count<=8);if(n)memmove(storage,*(void**)list,(size_t)n*104);
    *(void**)list=storage;*(int*)(list+12)=count;return 1;
}
static void assign_string(void *p,const char *s)
{
    unsigned char *str=p;assert(assigns<32&&strlen(s)<193&&*(int*)(heap_state+0xc4)==2);
    strcpy_s(strings[assigns],193,s);*(int*)(str+8)=(int)strlen(s);*(char**)(str+16)=strings[assigns++];
}
static void fresh_block(float *values){int i;for(i=0;i<36;i++)values[i]=(float)(i+42);}
static void unchanged(const float *values){int i;for(i=0;i<36;i++)assert(values[i]==(float)(i+42));}
static void fresh_native(float *values,float native){fresh_block(values);values[4]=native;}
/* Unlocked: the distance literal (op 1) is 200000; fogend (op 4) and fogstart
 * (op 5) are stretched by r = 200000 / native and fogscale (op 3) and
 * fogdensity (op 6) divided by it. The unrelated parm, fogcolor,
 * fogmaxopacity and heightfogscale keep their values. */
static void unlocked_block(const float *values,float native)
{
    int i;float r=sh_map_render_fog_stretch(native);
    for(i=0;i<36;i++) {
        int op=i/4;float orig=i==4?native:(float)(i+42);
        if(op==1)assert(values[i]==SH_RENDER_MAX_DISTANCE);
        else if(op==3||op==6)assert(values[i]==sh_map_render_fog_strength(orig,r));
        else if(op==4||op==5)assert(values[i]==sh_map_render_fog_distance(orig,r));
        else assert(values[i]==orig);
    }
}
int main(void)
{
    unsigned char saved[0x770]={0},edit[0x800]={0},rows[208]={0},block[48]={0},ops[72]={0};
    const char *names[]={"unrelated","maxviewdistance","fogcolor","fogscale","fogend","fogstart",
        "fogdensity","fogmaxopacity","heightfogscale"};
    float values[36];sh_map_render settings;int game=1,i;
    sh_map_render_default(&settings);
    *(void**)(saved+0x1a0)=rows;*(int*)(saved+0x1a8)=1;*(int*)(saved+0x1ac)=2;
    /* Before install nothing is written to r_zfar, but selection works. */
    row(rows,SH_RENDER_VARIABLE,"1;60000;100;1500;6500;0.35;0.4;0.45");
    sh_map_render_loaded(saved);assert(g_ready&&g_runtime.unlocked&&zfar_sets==0&&far_floor==SH_RENDER_MAX_DISTANCE);
    {
        sh_map_render retained;
        row(rows,SH_RENDER_VARIABLE,"4;8192");
        assert(sh_map_render_capture(saved,&retained)&&!retained.unlocked);
        assert(g_ready&&g_runtime.unlocked);
        row(rows,SH_RENDER_VARIABLE,"malformed");
        sh_map_render_select(&retained);
        assert(g_ready&&!g_runtime.unlocked&&far_floor==0);
        assert(!sh_map_render_capture(saved,&retained));
        retained.unlocked=3;sh_map_render_select(&retained);assert(!g_ready);
        row(rows,SH_RENDER_VARIABLE,"1;60000;100;1500;6500;0.35;0.4;0.45");
    }
    g_game_type=&game;g_float=get_float;g_decl=lookup;g_set=set_cvar;g_zfar=zfar;
    *(float*)(zfar+0x34)=8192;
    sh_map_render_loaded(saved);
    /* Selecting an unlocked map sets r_zfar once, for frames in which no
     * environment resolves; an r_zfar already at the maximum is left alone. */
    assert(zfar_sets==1&&!strcmp(zfar_text,"200000")&&zfar_value()==SH_RENDER_MAX_DISTANCE);
    sh_map_render_loaded(saved);assert(zfar_sets==1);
    *(void**)block=ops;*(int*)(block+8)=9;*(void**)(block+24)=values;*(int*)(block+32)=9;
    for(i=0;i<9;i++){
        *(const char**)(resources[i]+8)=names[i];
        *(unsigned short*)(ops+i*8)=15360;*(unsigned short*)(ops+i*8+2)=(unsigned short)i;
    }
    /* A version-1 map with a distance reads as on: maximum, fog stretched by
     * 200000 / 60000 (snapmap/base's own distance). */
    fresh_native(values,60000);
    assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);unlocked_block(values,60000);
    assert(fabsf(values[16]-58*200000.0f/60000)<0.01f&&fabsf(values[12]-54*60000.0f/200000)<1e-4f);
    /* Every frame starts from a freshly blended block; the next one is
     * stretched again, once. A non-literal fog op is left alone. */
    fresh_native(values,60000);*(unsigned short*)(ops+6*8)=15361;
    assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);
    assert(values[24]==(float)(24+42)&&values[20]==sh_map_render_fog_distance(62,200000.0f/60000));
    assert(values[28]==(float)(28+42)&&values[35]==(float)(35+42));
    *(unsigned short*)(ops+6*8)=15360;
    /* Outside SnapMap nothing changes, fog included. */
    fresh_block(values);game=0;assert(render_clip(block,NULL,0)==46);game=1;unchanged(values);
    /* A fresh map (no variable) is off: the blend's own value and fog. */
    fresh_block(values);
    sh_map_render_build(edit);assert(g_ready&&!g_runtime.unlocked&&far_floor==0);
    assert(render_clip(block,NULL,0)==46);unchanged(values);
    /* Off leaves whatever the blend produced: module layers, Set Environment
     * nodes and campaign environments stay in control, with their fog. */
    zfar_sets=0;*(float*)(zfar+0x34)=8192;
    row(rows,SH_RENDER_VARIABLE,"2;0;200000;80;500;1500;.1;.2;.3");
    sh_map_render_loaded(saved);assert(g_ready&&!g_runtime.unlocked);
    render_clip(block,NULL,0);unchanged(values);
    row(rows,SH_RENDER_VARIABLE,"4;8192");
    sh_map_render_loaded(saved);values[4]=15000;assert(render_clip(block,NULL,0)==15000);values[4]=46;
    unchanged(values);assert(zfar_sets==0&&zfar_value()==8192);
    /* Every former distance other than 8192 reads as on, 0 and 200000 too. */
    row(rows,SH_RENDER_VARIABLE,"4;0");
    unlocked_logs=0;sh_map_render_loaded(saved);assert(unlocked_logs==1&&zfar_sets==1);
    fresh_native(values,15000);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);unlocked_block(values,15000);
    /* An environment already beyond the maximum is never shrunk or thickened:
     * r stays 1, only the distance literal changes. */
    fresh_native(values,256000);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);unlocked_block(values,256000);
    for(i=0;i<36;i++)if(i/4!=1)assert(values[i]==(float)(i+42));
    row(rows,SH_RENDER_VARIABLE,"4;60000");sh_map_render_loaded(saved);
    fresh_block(values);values[4]=100000;assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);
    row(rows,SH_RENDER_VARIABLE,"4;200000");sh_map_render_loaded(saved);
    fresh_block(values);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);unlocked_block(values,46);
    /* The returned distance is authoritative even without a literal slot. */
    *(unsigned short*)(ops+8)=15361;fresh_block(values);
    assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE&&values[4]==46);
    *(unsigned short*)(ops+8)=15360;
    /* After an engine exception nothing is changed any more, fog included. */
    fresh_block(values);g_faulted=1;assert(render_clip(block,NULL,0)==46);g_faulted=0;unchanged(values);
    /* The clear before a load and an unselected map return the native read. */
    sh_map_render_loaded(NULL);assert(!g_ready);assert(render_clip(block,NULL,0)==46);unchanged(values);
    /* In Blueprint frames the editor and the blend agree on the larger far
     * plane, and an unlocked map's maximum wins over it. */
    sh_map_render_build(edit);blueprint=1;assert(render_clip(block,NULL,0)==66000);
    sh_map_render_loaded(saved);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);
    blueprint=0;
    /* Reject duplicate metadata. An incomplete parameter block is not
     * written, but the maximum is still returned. */
    row(rows+104,SH_RENDER_VARIABLE,"4;8192");
    *(int*)(saved+0x1a8)=2;sh_map_render_loaded(saved);assert(!g_ready&&far_floor==0);
    fresh_block(values);assert(render_clip(block,NULL,0)==46);
    *(int*)(saved+0x1a8)=1;sh_map_render_loaded(saved);
    *(int*)(block+32)=8;fresh_block(values);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE&&values[4]==46);
    *(int*)(block+32)=9;
    /* The clear before every load is silent; a loaded map without valid
     * metadata still says so. */
    no_metadata_logs=0;sh_map_render_loaded(NULL);assert(!g_ready&&no_metadata_logs==0);
    row(rows,SH_RENDER_VARIABLE,"malformed");*(int*)(saved+0x1a8)=1;
    sh_map_render_loaded(saved);assert(!g_ready&&no_metadata_logs==1);
    /* Apply on the editable map. On appends the row after the author's own
     * string, keeps that string's index and leaves allocCount[4] alone; off
     * removes the row again, so the list is exactly what it was. */
    g_resize=resize_list;g_assign=assign_string;
    g_heap.get=heap_get;g_heap.push=heap_push;g_heap.pop=heap_pop;
    *(int*)(heap_state+0xc4)=1;*(int*)(heap_state+0x44)=2;
    memset(rows,0,sizeof rows);row(rows,"author_variable","leave this value");
    *(void**)(edit+0xa8)=rows;*(int*)(edit+0xb0)=1;*(int*)(edit+0xb4)=1;
    *(int*)(edit+0x5c8)=1;
    sh_map_render_default(&settings);off_logs=0;
    assert(sh_map_render_write(edit,&settings)&&assigns==0&&*(int*)(edit+0xb0)==1&&off_logs==1);
    sh_map_render_from_toggle(1,&settings);stored_logs=0;
    assert(sh_map_render_write(edit,&settings)&&assigns==2&&stored_logs==1);
    assert(*(int*)(heap_state+0xc4)==1&&*(int*)(heap_state+0x44)==2);
    assert(*(int*)(edit+0xb0)==2&&*(int*)(edit+0x5c8)==1);
    assert(!strcmp(row_name(edit+0xa8,0),"author_variable")&&!strcmp(row_value(edit+0xa8,0),"leave this value"));
    assert(!strcmp(row_name(edit+0xa8,1),SH_RENDER_VARIABLE)&&!strcmp(row_value(edit+0xa8,1),"4;0"));
    assert(sh_map_render_read(edit,&settings)&&settings.unlocked);
    /* Already stored: nothing is assigned. */
    assert(sh_map_render_write(edit,&settings)&&assigns==2);
    sh_map_render_default(&settings);
    assert(sh_map_render_write(edit,&settings)&&assigns==2&&*(int*)(edit+0xb0)==1&&*(int*)(edit+0x5c8)==1);
    assert(!strcmp(row_name(edit+0xa8,0),"author_variable")&&!strcmp(row_value(edit+0xa8,0),"leave this value"));
    assert(sh_map_render_read(edit,&settings)&&!settings.unlocked);
    /* Off again is a no-op. */
    assert(sh_map_render_write(edit,&settings)&&assigns==2&&*(int*)(edit+0xb0)==1);
    /* Removing a row followed only by Snapmap+ rows moves them down. */
    {
        unsigned char list[3*104]={0};int before;
        row(list,"String 1","author");row(list+104,SH_RENDER_VARIABLE,"4;0");row(list+208,"smp.bounds.v1","1;32671");
        *(void**)(edit+0xa8)=list;*(int*)(edit+0xb0)=3;*(int*)(edit+0xb4)=3;before=assigns;
        assert(sh_map_render_write(edit,&settings)&&assigns==before+2&&*(int*)(edit+0xb0)==2);
        assert(!strcmp(row_name(edit+0xa8,0),"String 1")&&!strcmp(row_value(edit+0xa8,0),"author"));
        assert(!strcmp(row_name(edit+0xa8,1),"smp.bounds.v1")&&!strcmp(row_value(edit+0xa8,1),"1;32671"));
        assert(*(int*)(heap_state+0xc4)==1);
        /* An author string after it must keep its index: the row stays and
         * holds the native distance, which reads as off. */
        row(list+104,SH_RENDER_VARIABLE,"4;0");row(list+208,"String 2","bound by index");
        *(int*)(edit+0xb0)=3;kept_logs=0;before=assigns;
        assert(sh_map_render_write(edit,&settings)&&assigns==before+1&&*(int*)(edit+0xb0)==3&&kept_logs==1);
        assert(!strcmp(row_value(edit+0xa8,1),"4;8192")&&!strcmp(row_value(edit+0xa8,2),"bound by index"));
        assert(sh_map_render_read(edit,&settings)&&!settings.unlocked);
        assert(sh_map_render_write(edit,&settings)&&assigns==before+1);
        *(void**)(edit+0xa8)=rows;*(int*)(edit+0xb0)=1;*(int*)(edit+0xb4)=1;
    }
    /* Play conversion receives a build copy, not the editable map: it only
     * publishes and never writes, even for an older value. */
    {
        unsigned char edit2[0x800]={0},list2[208]={0};int before=assigns;
        *(void**)(edit2+0xa8)=list2;row(list2,"author_variable","x");
        row(list2+104,SH_RENDER_VARIABLE,"3;60000;70;500;1500;.1;.2;.3");*(int*)(edit2+0xb0)=2;*(int*)(edit2+0xb4)=2;
        sh_map_render_build(edit2);assert(assigns==before&&g_ready&&g_runtime.unlocked);
        assert(!strcmp(row_value(edit2+0xa8,1),"3;60000;70;500;1500;.1;.2;.3"));
        *(int*)(edit2+0xb0)=1;
        sh_map_render_build(edit2);assert(assigns==before&&*(int*)(edit2+0xb0)==1&&g_ready&&!g_runtime.unlocked);
    }
    /* Loading normalizes the deserialized snapshot in place, so the editable
     * map built from it saves the canonical text or nothing. Row indices of
     * other rows and allocCount[4] (+0x6c0) stay as they were. */
    {
        unsigned char snap[0x770]={0},list[208]={0};int before;
        *(void**)(snap+0x1a0)=list;*(int*)(snap+0x1a8)=2;*(int*)(snap+0x1ac)=2;*(int*)(snap+0x6c0)=7;
        row(list,"author_variable","leave this value");
        row(list+104,SH_RENDER_VARIABLE,"3;60000;70;1500;6500;.35;.4;.45");
        converted_logs=0;before=assigns;sh_map_render_loaded(snap);
        assert(assigns==before+1&&converted_logs==1&&g_ready&&g_runtime.unlocked);
        assert(!strcmp(row_value(snap+0x1a0,1),"4;0")&&*(int*)(list+104+64)==3);
        assert(!strcmp(row_value(snap+0x1a0,0),"leave this value"));
        assert(*(int*)(snap+0x1a8)==2&&*(int*)(snap+0x6c0)==7);
        assert(*(int*)(heap_state+0xc4)==1&&*(int*)(heap_state+0x44)==2);
        /* Already canonical: nothing is assigned or logged. */
        sh_map_render_loaded(snap);assert(assigns==before+1&&converted_logs==1);
        /* A non-canonical on spelling is normalized too. */
        row(list+104,SH_RENDER_VARIABLE,"4;60000.0");
        sh_map_render_loaded(snap);assert(assigns==before+2&&!strcmp(row_value(snap+0x1a0,1),"4;0"));
        /* Off values (8192, the former disabled mode) are removed on load,
         * the last row without allocating, even where the persistent heap
         * cannot be entered. */
        row(list+104,SH_RENDER_VARIABLE,"4;8192");removed_logs=0;
        *(int*)(heap_state+0xc4)=32;
        sh_map_render_loaded(snap);
        assert(assigns==before+2&&removed_logs==1&&*(int*)(snap+0x1a8)==1&&*(int*)(snap+0x6c0)==7&&g_ready&&!g_runtime.unlocked);
        *(int*)(heap_state+0xc4)=1;
        row(list+104,SH_RENDER_VARIABLE,"2;0;60000;70;1500;6500;.35;.4;.45");*(int*)(snap+0x1a8)=2;
        sh_map_render_loaded(snap);assert(removed_logs==2&&*(int*)(snap+0x1a8)==1&&!g_runtime.unlocked);
        /* A map without the variable stays free of it. */
        row(list+104,"another_variable","1");*(int*)(snap+0x1a8)=2;
        sh_map_render_loaded(snap);assert(assigns==before+2&&*(int*)(snap+0x1a8)==2&&g_ready);
        /* Malformed metadata is never rewritten. */
        row(list+104,SH_RENDER_VARIABLE,"3;60000");
        sh_map_render_loaded(snap);assert(assigns==before+2&&!g_ready&&*(int*)(snap+0x1a8)==2);
        /* When the persistent heap cannot be entered (not the engine thread),
         * an older on value is left as it was and still selected. */
        row(list+104,SH_RENDER_VARIABLE,"1;60000;70;1500;6500;.35;.4;.45");
        *(int*)(heap_state+0xc4)=32;converted_logs=0;
        sh_map_render_loaded(snap);
        assert(assigns==before+2&&converted_logs==0&&g_ready&&g_runtime.unlocked);
        assert(!strcmp(row_value(snap+0x1a0,1),"1;60000;70;1500;6500;.35;.4;.45"));
        *(int*)(heap_state+0xc4)=1;
        /* Before install (no assign binding) loading only selects. */
        g_assign=NULL;sh_map_render_loaded(snap);assert(assigns==before+2&&g_ready);g_assign=assign_string;
    }
    memset(edit,0,sizeof edit);fail_resize=1;sh_map_render_from_toggle(1,&settings);
    assert(!sh_map_render_write(edit,&settings)&&*(int*)(edit+0xb0)==0&&*(int*)(heap_state+0xc4)==1);
    puts("map_render_native_test: passed");return 0;
}
