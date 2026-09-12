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
    for(i=0;i<24;i++)values[i]=(float)(i+42);
    sh_map_render_build(edit);assert(g_ready&&!sh_map_render_distance_override(&g_runtime)&&!sh_map_render_fog_override(&g_runtime));
    render_clip(block,NULL,0);for(i=0;i<24;i++)assert(values[i]==(float)(i+42));
    row(rows,SH_RENDER_VARIABLE,"2;0;200000;80;500;1500;.1;.2;.3");
    sh_map_render_loaded(saved);assert(g_ready&&!sh_map_render_distance_override(&g_runtime)&&!sh_map_render_fog_override(&g_runtime));
    render_clip(block,NULL,0);for(i=0;i<24;i++)assert(values[i]==(float)(i+42));
    /* Distance and fog are independent. Defaults preserve arbitrary native
     * blended environments, including modules with their own fog. */
    row(rows,SH_RENDER_VARIABLE,"3;60000;0;500;1500;.1;.2;.3");
    sh_map_render_loaded(saved);assert(render_clip(block,NULL,0)==60000);
    for(i=8;i<24;i++)assert(values[i]==(float)(i+42));
    for(i=0;i<24;i++)values[i]=(float)(i+42);
    row(rows,SH_RENDER_VARIABLE,"3;8192;70;500;1500;.1;.2;.3");
    sh_map_render_loaded(saved);assert(render_clip(block,NULL,0)==46);
    for(i=0;i<8;i++)assert(values[i]==(float)(i+42));
    assert(values[12]==70*.00002f&&values[20]==500&&values[16]==1500);
    for(i=0;i<24;i++)values[i]=(float)(i+42);
    row(rows,SH_RENDER_VARIABLE,"3;0;0;500;1500;.1;.2;.3");
    sh_map_render_loaded(saved);render_clip(block,NULL,0);
    for(i=0;i<24;i++)assert(values[i]==(float)(i+42));
    row(rows,SH_RENDER_VARIABLE,"1;60000;100;1500;6500;0.35;0.4;0.45");
    sh_map_render_loaded(NULL);assert(!g_ready);values[4]=8192;
    assert(render_clip(block,NULL,0)==8192);
    sh_map_render_loaded(saved);game=0;
    assert(render_clip(block,NULL,0)==8192);game=1;
    /* Reject duplicate metadata and incomplete parameter blocks. */
    row(rows+104,SH_RENDER_VARIABLE,"1;8192;0;1500;6500;0;0;0");
    *(int*)(saved+0x1a8)=2;sh_map_render_loaded(saved);assert(!g_ready);
    *(int*)(saved+0x1a8)=1;sh_map_render_loaded(saved);
    *(int*)(block+32)=5;assert(render_clip(block,NULL,0)==8192);
    /* Save conversion persists default values, appends without moving
     * another string's index, and returns to the caller's original heap. */
    g_resize=resize_list;g_assign=assign_string;
    g_heap.get=heap_get;g_heap.push=heap_push;g_heap.pop=heap_pop;
    *(int*)(heap_state+0xc4)=1;*(int*)(heap_state+0x44)=2;
    memset(rows,0,sizeof rows);row(rows,"author_variable","leave this value");
    *(void**)(edit+0xa8)=rows;*(int*)(edit+0xb0)=1;*(int*)(edit+0xb4)=1;
    *(int*)(edit+0x5c8)=12;
    sh_map_render_build(edit);assert(g_ready&&!sh_map_render_distance_override(&g_runtime)&&!sh_map_render_fog_override(&g_runtime)&&assigns==2);
    assert(*(int*)(heap_state+0xc4)==1&&*(int*)(heap_state+0x44)==2);
    assert(*(int*)(edit+0xb0)==2&&*(int*)(edit+0x5c8)==12);
    assert(!strcmp(*(char**)(storage+16),"author_variable")&&!strcmp(*(char**)(storage+72),"leave this value"));
    assert(sh_map_render_read(edit,&settings)&&settings.value[0]==8192&&settings.value[1]==0);
    assert(!strncmp(*(char**)(storage+104+72),"3;",2));
    settings.value[1]=73;assert(sh_map_render_write(edit,&settings)&&assigns==3);
    settings.value[1]=0;assert(sh_map_render_write(edit,&settings)&&assigns==4);
    assert(sh_map_render_read(edit,&settings)&&!sh_map_render_fog_override(&settings));
    sh_map_render_build(edit);assert(assigns==4&&*(int*)(edit+0xb0)==2);
    /* Saving an old disabled map normalizes its values and removes the old
     * flag without changing other string rows or index high-water marks. */
    row(storage+104,SH_RENDER_VARIABLE,"2;0;200000;80;500;1500;.1;.2;.3");
    sh_map_render_build(edit);assert(assigns==5&&g_runtime.value[0]==8192&&g_runtime.value[1]==0);
    assert(!strncmp(*(char**)(storage+104+72),"3;8192;0;",9));
    assert(*(int*)(edit+0xb0)==2&&*(int*)(edit+0x5c8)==12);
    assert(!strcmp(*(char**)(storage+72),"leave this value"));
    row(storage+104,SH_RENDER_VARIABLE,"2;1;60000;70;500;1500;.1;.2;.3");
    sh_map_render_build(edit);assert(assigns==6&&g_runtime.value[0]==60000&&g_runtime.value[1]==70);
    assert(!strncmp(*(char**)(storage+104+72),"3;60000;70;",11));
    memset(edit,0,sizeof edit);fail_resize=1;
    assert(!sh_map_render_write(edit,&settings)&&*(int*)(edit+0xb0)==0&&*(int*)(heap_state+0xc4)==1);
    puts("map_render_native_test: passed");return 0;
}
