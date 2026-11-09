#include "../src/backend/map_render_native.c"
#include "../src/backend/map_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static int no_metadata_logs,unlocked_logs,stored_logs,off_logs;
void backend_log(const char *s)
{
    if(strstr(s,"no valid map rendering metadata"))no_metadata_logs++;
    if(strstr(s,"unlocked view distance; far clip 200000, environment fog stretched to match"))unlocked_logs++;
    if(strstr(s,"Unlock View Distance on for this map"))stored_logs++;
    if(strstr(s,"Unlock View Distance off for this map"))off_logs++;
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
/* Parse a map whose string variables are an author's and, when value is not
 * NULL, one smp.render.v1 row (twice when twice is set): the open map's
 * settings are then that map's, as after a real parse. */
static void parse_map(const char *value,int twice)
{
    char json[1024];size_t n=0;char *out;
    const char *pre=",{\"info\":{\"name\":\"" SH_RENDER_VARIABLE "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"";
    const char *post="\",\"~type\":\"snapVarString_t\"}";
    _snprintf_s(json,sizeof json,_TRUNCATE,
        "{\"variables\":{\"allocCount\":[0,0,0,0,%d],\"string\":["
        "{\"info\":{\"name\":\"author\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"x\",\"~type\":\"snapVarString_t\"}"
        "%s%s%s%s%s%s],\"~type\":\"idSnapVariables\"}}",
        value?(twice?3:2):1,
        value?pre:"",value?value:"",value?post:"",
        twice?pre:"",twice?value:"",twice?post:"");
    out=sh_map_settings_prepare_load(json,strlen(json),&n);
    if(value)assert(out&&!strstr(out,SH_RENDER_VARIABLE));
    if(out)HeapFree(GetProcessHeap(),0,out);
    sh_map_settings_loaded(1);
}
int main(void)
{
    unsigned char saved[0x770]={0},edit[0x800]={0},block[48]={0},ops[72]={0};
    const char *names[]={"unrelated","maxviewdistance","fogcolor","fogscale","fogend","fogstart",
        "fogdensity","fogmaxopacity","heightfogscale"};
    float values[36];sh_map_render settings;int game=1,i;
    sh_map_render_default(&settings);
    /* Before install nothing is written to r_zfar, but selection works. */
    parse_map("1;60000;100;1500;6500;0.35;0.4;0.45",0);
    sh_map_render_loaded(saved);assert(g_ready&&g_runtime.unlocked&&zfar_sets==0&&far_floor==SH_RENDER_MAX_DISTANCE);
    {
        sh_map_render retained;
        retained.unlocked=0;sh_map_render_select(&retained);
        assert(g_ready&&!g_runtime.unlocked&&far_floor==0);
        retained.unlocked=3;sh_map_render_select(&retained);assert(!g_ready);
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
    parse_map(NULL,0);fresh_block(values);
    sh_map_render_build(edit);assert(g_ready&&!g_runtime.unlocked&&far_floor==0);
    assert(render_clip(block,NULL,0)==46);unchanged(values);
    /* Off leaves whatever the blend produced: module layers, Set Environment
     * nodes and campaign environments stay in control, with their fog. */
    zfar_sets=0;*(float*)(zfar+0x34)=8192;
    parse_map("2;0;200000;80;500;1500;.1;.2;.3",0);
    sh_map_render_loaded(saved);assert(g_ready&&!g_runtime.unlocked);
    render_clip(block,NULL,0);unchanged(values);
    parse_map("4;8192",0);
    sh_map_render_loaded(saved);values[4]=15000;assert(render_clip(block,NULL,0)==15000);values[4]=46;
    unchanged(values);assert(zfar_sets==0&&zfar_value()==8192);
    /* Every former distance other than 8192 reads as on, 0 and 200000 too. */
    parse_map("4;0",0);
    unlocked_logs=0;sh_map_render_loaded(saved);assert(unlocked_logs==1&&zfar_sets==1);
    fresh_native(values,15000);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);unlocked_block(values,15000);
    /* An environment already beyond the maximum is never shrunk or thickened:
     * r stays 1, only the distance literal changes. */
    fresh_native(values,256000);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);unlocked_block(values,256000);
    for(i=0;i<36;i++)if(i/4!=1)assert(values[i]==(float)(i+42));
    parse_map("4;60000",0);sh_map_render_loaded(saved);
    fresh_block(values);values[4]=100000;assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);
    parse_map("4;200000",0);sh_map_render_loaded(saved);
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
    parse_map(NULL,0);sh_map_render_build(edit);blueprint=1;assert(render_clip(block,NULL,0)==66000);
    parse_map("4;0",0);sh_map_render_loaded(saved);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE);
    blueprint=0;
    /* Duplicated or malformed metadata reads as off, as earlier builds refused it. */
    parse_map("4;0",1);sh_map_render_loaded(saved);assert(g_ready&&!g_runtime.unlocked&&far_floor==0);
    fresh_block(values);assert(render_clip(block,NULL,0)==46);
    parse_map("malformed",0);sh_map_render_loaded(saved);assert(g_ready&&!g_runtime.unlocked);
    /* The clear before every load is silent. */
    no_metadata_logs=0;sh_map_render_loaded(NULL);assert(!g_ready&&no_metadata_logs==0);
    /* An incomplete parameter block is not written, but the maximum is still returned. */
    parse_map("4;0",0);sh_map_render_loaded(saved);
    *(int*)(block+32)=8;fresh_block(values);assert(render_clip(block,NULL,0)==SH_RENDER_MAX_DISTANCE&&values[4]==46);
    *(int*)(block+32)=9;
    /* Apply writes the open map's setting, never the engine's variable list,
     * and Play publishes it from the build copy. */
    parse_map(NULL,0);
    assert(sh_map_render_read(edit,&settings)&&!settings.unlocked);
    sh_map_render_from_toggle(1,&settings);stored_logs=0;
    assert(sh_map_render_write(edit,&settings)&&stored_logs==1);
    {static const unsigned char zero[0x800];assert(!memcmp(edit,zero,sizeof edit));}
    {sh_map_settings cur;sh_map_settings_current(&cur);assert(cur.render.unlocked==1);}
    assert(sh_map_render_read(saved,&settings)&&settings.unlocked);
    sh_map_render_build(edit);assert(g_ready&&g_runtime.unlocked);
    sh_map_render_default(&settings);off_logs=0;
    assert(sh_map_render_write(edit,&settings)&&off_logs==1);
    sh_map_render_build(edit);assert(g_ready&&!g_runtime.unlocked);
    assert(!sh_map_render_read(NULL,&settings));
    settings.unlocked=2;assert(!sh_map_render_write(edit,&settings));
    puts("map_render_native_test: passed");return 0;
}
