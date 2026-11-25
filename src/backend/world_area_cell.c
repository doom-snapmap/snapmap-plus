/* Play's SnapMap world-area cell size (see world_area_cell.h). BuildRenderWorld (VK 0x14114fd60 / GL
 * 0x14114ef60) passes the shared 512.0 constant (VK 0x1420275a8 / GL 0x142041378) as the subdivision's
 * fifth argument via MOVSS xmm0,[rip+disp32] at VK 0x14114ff66 / GL 0x14114f166. Only that instruction's
 * disp32 is repointed, at a product-owned float that holds 512 except while SnapMapEditToSnapBuild
 * builds a map needing a coarser size. The frontend reads area extents from the built areas' bounds,
 * never from this value. */
#include "safe_read.h"
#include "world_area_cell.h"
#include <string.h>

int sh_world_area_cell_site(const unsigned char *site,uintptr_t *constant)
{
    static const unsigned char load[4]={0xf3,0x0f,0x10,0x05},store[6]={0xf3,0x0f,0x11,0x44,0x24,0x20};
    int32_t disp;
    if(!site||!constant||memcmp(site,load,4)||memcmp(site+8,store,6))return 0;
    memcpy(&disp,site+4,4);
    *constant=(uintptr_t)site+8+(intptr_t)disp;
    return 1;
}
int sh_world_area_cell_disp(uintptr_t site,uintptr_t dest,int32_t *disp)
{
    intptr_t distance=(intptr_t)dest-(intptr_t)(site+8);
    if(!disp||distance<INT32_MIN||distance>INT32_MAX)return 0;
    *disp=(int32_t)distance;return 1;
}

#ifndef SH_WORLD_AREA_CELL_PURE
#include "grid_room_areas.h"
#include "grid_room_native.h"
#include "patch.h"
#include "near_page.h"
#include "ui_bridge.h"
#include <windows.h>
#include <stdio.h>

void backend_log(const char *message);

/* volatile: the engine reads it through the redirected instruction. */
typedef struct cell_page {volatile float cell;} cell_page;
static cell_page *g_page;
static sh_patch_handle g_patch;
static int g_live;
/* The open map, followed through its module count: settle counts the frames
 * one count has held (-1 once that count is told), opened says the first
 * answer is still owed to the log, warned is the count already told. */
static volatile LONG g_watch;
static int g_settle,g_last_n,g_opened;
static long long g_warned;

/* The bind's byte checks, the install's and the image test's one function: the load's bytes, then its constant. */
const char *sh_world_area_cell_check(const unsigned char *site,uintptr_t *constant)
{
    float native=0;
    if(!sh_world_area_cell_site(site,constant))return "the cell-size load has unexpected bytes";
    if(!sh_read_float(*constant,&native)||native!=SH_RENDER_AREA_CELL)return "the cell-size constant is not 512";
    return NULL;
}
int sh_world_area_cell_install(const sig_result *results,size_t count,const uint8_t *base)
{
    const sig_result *site;uintptr_t constant=0;int32_t old_disp,new_disp;
    const char *why;char line[320];
    (void)base;
    if(g_live)return 1;
    site=sig_ok(results,count,"WorldAreaCellLoad");
    if(!site){why="the cell-size load was not found";goto unavailable;}
    if((why=sh_world_area_cell_check((const unsigned char*)site->addr,&constant)))goto unavailable;
    /* The load's disp32 is relative to the instruction's end, site + 8. */
    if(!g_page&&!(g_page=(cell_page*)sh_near_page_alloc(site->addr+8))){why="no page within reach of the cell-size load";goto unavailable;}
    /* Written before the patch: the redirected load reads 512 from the start. */
    g_page->cell=SH_RENDER_AREA_CELL;
    memcpy(&old_disp,(const void*)(site->addr+4),4);
    if(!sh_world_area_cell_disp(site->addr,(uintptr_t)&g_page->cell,&new_disp)){why="the page is out of reach";goto release;}
    if(code_patch((void*)(site->addr+4),(const uint8_t*)&old_disp,(const uint8_t*)&new_disp,4,&g_patch)!=B2_PATCH_OK){
        why="the cell-size load could not be patched";goto release;}
    g_live=1;sh_grid_native_set_adaptive_cells(1);
    snprintf(line,sizeof line,"RENDER: world-area cell size follows the map at %p (512 unless a map needs more than %d world areas or area nodes)",
        (void*)site->addr,SH_RENDER_AREA_MAX);
    backend_log(line);
    return 1;
release:
    if(!g_patch.live){VirtualFree(g_page,0,MEM_RELEASE);g_page=NULL;}
unavailable:
    sh_grid_native_set_adaptive_cells(0);
    snprintf(line,sizeof line,"RENDER: world-area cell size redirect unavailable (%s); Play keeps 512-unit world areas and Grid Room resizes stay limited to %d world areas",
        why,SH_RENDER_AREA_MAX);
    backend_log(line);
    return 0;
}
int sh_world_area_cell_live(void){return g_live;}
float sh_world_area_cell_current(void){return g_live&&g_page?g_page->cell:SH_RENDER_AREA_CELL;}
static void describe(char *line,size_t cap,const char *prefix,const sh_render_cell_choice *c)
{
    if(c->cell==SH_RENDER_AREA_CELL&&c->fits)
        snprintf(line,cap,"%sworld-area cell size 512 for %lld areas / %lld nodes",prefix,c->count.areas,c->count.nodes);
    else if(c->fits)
        snprintf(line,cap,"%sworld-area cell size %.0f for %lld areas / %lld nodes (512 would give %lld areas / %lld nodes)",
            prefix,c->cell,c->count.areas,c->count.nodes,c->native.areas,c->native.nodes);
    else
        snprintf(line,cap,"%sworld-area cell size %.0f gives %lld areas / %lld nodes (512: %lld / %lld), %s the engine maximum of %d%s",
            prefix,c->cell,c->count.areas,c->count.nodes,c->native.areas,c->native.nodes,
            sh_render_areas_fit(&c->count)?"close to":"over",SH_RENDER_AREA_MAX,
            sh_render_areas_fit(&c->count)?"":"; this map cannot load");
}
/* The world areas the map needs at the coarsest size: the larger of areas and nodes. */
static long long needed(const sh_render_cell_choice *c)
{
    return c->count.areas>c->count.nodes?c->count.areas:c->count.nodes;
}
/* Tell the player, before Play, that the map is too large to load. Once per
 * size: an edit that changes the count tells again. */
static void warn_if_over(const sh_render_cell_choice *c)
{
    sh_iface *iface;char text[200];long long n=needed(c);
    if(sh_render_areas_fit(&c->count)){g_warned=0;return;}
    if(n==g_warned)return;
    g_warned=n;
    snprintf(text,sizeof text,"This map needs %lld world areas and Play loads at most %d, so it would "
        "stop on the loading screen. Make the map smaller before choosing Play.",n,SH_RENDER_AREA_MAX);
    backend_log(text);
    iface=sh_ui_get_iface();
    if(iface&&iface->vtbl&&iface->vtbl->toast)iface->vtbl->toast(iface,"Map Too Large",text);
}
void sh_world_area_cell_build(void *map)
{
    sh_render_cell_choice c;char line[320];
    if(!g_live)return;
    if(!sh_grid_native_render_cell(map,&c)){
        g_page->cell=SH_RENDER_AREA_CELL;
        backend_log("RENDER: world areas could not be counted for this map; Play uses world-area cell size 512");
        return;
    }
    g_page->cell=c.cell;
    describe(line,sizeof line,"RENDER: ",&c);
    backend_log(line);
}
void sh_world_area_cell_build_end(void)
{
    if(g_live)g_page->cell=SH_RENDER_AREA_CELL;
}
void sh_world_area_cell_map_opened(void)
{
    g_settle=g_last_n=0;g_opened=1;g_warned=0;
    InterlockedExchange(&g_watch,1);
}
/* Engine thread, every editor frame: once the map's module count has held for about a second, count its
 * world areas, log the cell size Play will use, and tell the player when the map is too large to load. */
void sh_world_area_cell_editor_tick(void)
{
    sh_render_cell_choice c;void *map;int n=0;char line[320];
    if(!InterlockedCompareExchange(&g_watch,0,0))return;
    map=sh_grid_native_editor_map();
    if(map){__try {n=*(int*)((unsigned char*)map+0x758);} __except(EXCEPTION_EXECUTE_HANDLER) {n=0;}}
    if(!map||n<1||n!=g_last_n){g_last_n=n;g_settle=0;return;}
    if(g_settle<0||++g_settle<60)return;
    g_settle=-1;
    if(!sh_grid_native_render_cell(map,&c))return;
    if(g_opened&&(g_live||!c.fits)) {
        describe(line,sizeof line,g_live?"RENDER: map opened; Play will use ":"RENDER: map opened; without the adaptive cell size Play uses ",&c);
        backend_log(line);
    }
    g_opened=0;
    warn_if_over(&c);
}
#endif
