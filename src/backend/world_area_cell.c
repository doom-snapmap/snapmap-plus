/* Play's SnapMap world-area cell size (see world_area_cell.h).
 *
 * Build 13954591 evidence (snaphak-re editor-fixes-13954591 R-017, R-018, R-019):
 *   BuildRenderWorld VK 0x14114fd60 / GL 0x14114ef60 is called only from
 *     SnapMapEditToSnapBuild (VK 0x141153590), which is called only from
 *     VK 0x14114efb0; neither has a data reference. It calls the subdivision
 *     VK FUN_141153a30 / GL FUN_141152cf0 once, at VK 0x14114ff85 / GL
 *     0x14114f185; the subdivision's only other caller is itself.
 *   WorldAreaCellLoad VK 0x14114ff66 / GL 0x14114f166: MOVSS xmm0,[rip+disp32]
 *     (F3 0F 10 05, the shared 512.0 constant VK 0x1420275a8 / GL 0x142041378,
 *     69 readers on VK) stored by MOVSS [rsp+0x20],xmm0 as the subdivision's
 *     fifth argument. The subdivision uses it only as "halve while wider than
 *     this" on X then Y and passes it unchanged to its recursion.
 *   The frontend reads area extents from the built areas' bounds, never from
 *     this value; the only extent test seen is a half-diagonal^2 < 7.86432e7
 *     "small area" heuristic in the area-flag pass (VK FUN_141a35f20).
 * Only this instruction's disp32 is repointed, at a product-owned float that
 * holds 512 except while SnapMapEditToSnapBuild runs for a map that needs a
 * coarser size. */
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
#include <windows.h>
#include <stdio.h>

void backend_log(const char *message);

/* volatile: the engine reads it through the redirected instruction. */
typedef struct cell_page {volatile float cell;} cell_page;
static cell_page *g_page;
static sh_patch_handle g_patch;
static int g_live;
static volatile LONG g_pending;
static int g_settle,g_waited,g_last_n;

static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}
/* One page within rip-relative reach of the site. */
static cell_page *near_page(uintptr_t site)
{
    SYSTEM_INFO info;uintptr_t step,center,delta;
    GetSystemInfo(&info);step=info.dwAllocationGranularity;center=site&~(step-1);
    for(delta=step;delta<0x7fff0000u;delta+=step) {
        uintptr_t candidates[2]={center>=delta?center-delta:0,center+delta};int i;
        for(i=0;i<2;i++) {
            MEMORY_BASIC_INFORMATION mbi;void *page;uintptr_t a=candidates[i];int32_t disp;
            if(a<(uintptr_t)info.lpMinimumApplicationAddress||a>(uintptr_t)info.lpMaximumApplicationAddress)continue;
            if(!sh_world_area_cell_disp(site,a,&disp)||!sh_world_area_cell_disp(site,a+0x1000,&disp))continue;
            if(!VirtualQuery((void*)a,&mbi,sizeof mbi)||mbi.State!=MEM_FREE)continue;
            page=VirtualAlloc((void*)a,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            if(page)return (cell_page*)page;
        }
    }
    return NULL;
}
static int read_float(uintptr_t at,float *out)
{
    __try {*out=*(const volatile float*)at;return 1;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
int sh_world_area_cell_install(const sig_result *results,size_t count,const uint8_t *base)
{
    const sig_result *site;uintptr_t constant=0;float native=0;int32_t old_disp,new_disp;
    const char *why;char line[320];
    (void)base;
    if(g_live)return 1;
    site=binding(results,count,"WorldAreaCellLoad");
    if(!site){why="the cell-size load was not found";goto unavailable;}
    if(!sh_world_area_cell_site((const unsigned char*)site->addr,&constant)){why="the cell-size load has unexpected bytes";goto unavailable;}
    if(!read_float(constant,&native)||native!=SH_RENDER_AREA_CELL){why="the cell-size constant is not 512";goto unavailable;}
    if(!g_page&&!(g_page=near_page(site->addr))){why="no page within reach of the cell-size load";goto unavailable;}
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
    g_settle=g_waited=g_last_n=0;
    InterlockedExchange(&g_pending,1);
}
/* Engine thread, every editor frame: once the opened map's module count has
 * held for a second's worth of frames, log the cell size Play will use. */
void sh_world_area_cell_editor_tick(void)
{
    sh_render_cell_choice c;void *map;int n=0;char line[320];
    if(!InterlockedCompareExchange(&g_pending,0,0))return;
    if(++g_waited>36000){InterlockedExchange(&g_pending,0);return;}
    map=sh_grid_native_editor_map();
    if(map){__try {n=*(int*)((unsigned char*)map+0x758);} __except(EXCEPTION_EXECUTE_HANDLER) {n=0;}}
    if(!map||n<1||n!=g_last_n){g_last_n=n;g_settle=0;return;}
    if(++g_settle<60)return;
    InterlockedExchange(&g_pending,0);
    if(!sh_grid_native_render_cell(map,&c))return;
    if(!g_live&&c.fits)return;
    describe(line,sizeof line,g_live?"RENDER: map opened; Play will use ":"RENDER: map opened; without the adaptive cell size Play uses ",&c);
    backend_log(line);
}
#endif
