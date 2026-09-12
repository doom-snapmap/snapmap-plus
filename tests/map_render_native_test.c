#include "../src/backend/map_render_native.c"
#include <assert.h>
#include <stdio.h>
void backend_log(const char *s){(void)s;}
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
int main(void)
{
    unsigned char saved[0x770]={0},edit[0x800]={0},rows[208]={0},block[48]={0},ops[48]={0};
    const char *names[]={"unrelated","maxviewdistance","fogcolor","fogscale","fogend","fogstart"};
    float values[24];sh_map_render settings;int game=1,i,j;
    sh_map_render_default(&settings);
    *(void**)(saved+0x1a0)=rows;*(int*)(saved+0x1a8)=1;*(int*)(saved+0x1ac)=2;
    row(rows,SH_RENDER_VARIABLE,"1;60000;100;1500;6500;0.35;0.4;0.45");
    sh_map_render_loaded(saved);assert(g_ready&&g_runtime.value[1]==100);
    g_game_type=&game;g_float=get_float;g_decl=lookup;
    *(void**)block=ops;*(int*)(block+8)=6;*(void**)(block+24)=values;*(int*)(block+32)=6;
    for(i=0;i<6;i++){
        *(const char**)(resources[i]+8)=names[i];
        *(unsigned short*)(ops+i*8)=15360;*(unsigned short*)(ops+i*8+2)=(unsigned short)i;
    }
    for(i=0;i<24;i++)values[i]=8192;
    assert(render_clip(block,NULL,0)==60000);
    for(j=0;j<4;j++)assert(values[j]==8192);
    assert(values[12]==100*.00002f&&values[20]==1500&&values[16]==6500);
    assert(values[8]==.35f&&values[9]==.4f&&values[10]==.45f&&values[11]==1);
    /* A fresh map and a failed load cannot inherit the previous map's fog. */
    sh_map_render_build(edit);assert(g_ready&&g_runtime.value[1]==0);
    render_clip(block,NULL,0);assert(values[12]==0);
    sh_map_render_loaded(NULL);assert(!g_ready);values[4]=8192;
    assert(render_clip(block,NULL,0)==8192);
    sh_map_render_loaded(saved);game=0;
    assert(render_clip(block,NULL,0)==8192);game=1;
    /* Reject duplicate metadata and incomplete parameter blocks. */
    row(rows+104,SH_RENDER_VARIABLE,"1;8192;0;1500;6500;0;0;0");
    *(int*)(saved+0x1a8)=2;sh_map_render_loaded(saved);assert(!g_ready);
    *(int*)(saved+0x1a8)=1;sh_map_render_loaded(saved);
    *(int*)(block+32)=5;assert(render_clip(block,NULL,0)==8192);
    puts("map_render_native_test: passed");return 0;
}
