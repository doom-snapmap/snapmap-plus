/* Object Mode entry drop length (see object_entry.h).
 *
 * Build 13954591 evidence (snaphak-re editor-fixes-13954591 R-020):
 *   TAB: the Module Mode focus ray (VK FUN_141257870 / GL FUN_141256d70, the
 *     ModulePickFocus reader) stores the nearest module hit as the FPS view's
 *     pending point (VK FUN_1412b48c0 / GL FUN_1412b3fa0: view+0x84 flag,
 *     +0x88 point). idSnapFPSView vtable slot 1 (VK FUN_1412b3f30 / GL
 *     FUN_1412b3610), called by the camera's mode switch (VK FUN_1412400f0),
 *     hands that point to the entry solver VK FUN_1412b3780 / GL FUN_1412b2e60.
 *   The solver starts at (point.x, point.y, union of module world bounds max z
 *     + 100) and box-traces (-16,-16,-87)..(16,16,3) down the editor collision
 *     world in segments of 18364, restarting 1 unit below each hit, until a
 *     segment misses. It keeps the last hit (then a 256-unit placement-surface
 *     check, a +/-2048 surface search or the module's start entity, +87), or
 *     the camera's previous FPS position when the first segment misses.
 *   ObjectEntryDrop VK 0x1412b3823 / GL 0x1412b2f03: MOVSS xmm9,[rip+disp32]
 *     reading 18364.0 (VK 0x14238e388 / GL 0x1423a80d8). It is that
 *     constant's only reader on both executables, and the only place the
 *     solver gets its segment length.
 * Only this instruction's disp32 is repointed, at a product-owned float. */
#include "object_entry.h"
#include <string.h>

int sh_object_entry_site(const unsigned char *site,uintptr_t *constant)
{
    static const unsigned char load[5]={0xf3,0x44,0x0f,0x10,0x0d},
        next[8]={0x0f,0x57,0xff,0xf3,0x44,0x0f,0x10,0x05};
    int32_t disp;
    if(!site||!constant||memcmp(site,load,5)||memcmp(site+9,next,8))return 0;
    memcpy(&disp,site+5,4);
    *constant=(uintptr_t)site+9+(intptr_t)disp;
    return 1;
}
int sh_object_entry_disp(uintptr_t site,uintptr_t dest,int32_t *disp)
{
    intptr_t distance=(intptr_t)dest-(intptr_t)(site+9);
    if(!disp||distance<INT32_MIN||distance>INT32_MAX)return 0;
    *disp=(int32_t)distance;return 1;
}

#ifndef SH_OBJECT_ENTRY_PURE
#include "patch.h"
#include <windows.h>
#include <stdio.h>

void backend_log(const char *message);

/* volatile: the engine reads it through the redirected instruction. */
typedef struct drop_page {volatile float drop;} drop_page;
static drop_page *g_page;
static sh_patch_handle g_patch;
static int g_live;

static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}
/* One page within rip-relative reach of the site. */
static drop_page *near_page(uintptr_t site)
{
    SYSTEM_INFO info;uintptr_t step,center,delta;
    GetSystemInfo(&info);step=info.dwAllocationGranularity;center=site&~(step-1);
    for(delta=step;delta<0x7fff0000u;delta+=step) {
        uintptr_t candidates[2]={center>=delta?center-delta:0,center+delta};int i;
        for(i=0;i<2;i++) {
            MEMORY_BASIC_INFORMATION mbi;void *page;uintptr_t a=candidates[i];int32_t disp;
            if(a<(uintptr_t)info.lpMinimumApplicationAddress||a>(uintptr_t)info.lpMaximumApplicationAddress)continue;
            if(!sh_object_entry_disp(site,a,&disp)||!sh_object_entry_disp(site,a+0x1000,&disp))continue;
            if(!VirtualQuery((void*)a,&mbi,sizeof mbi)||mbi.State!=MEM_FREE)continue;
            page=VirtualAlloc((void*)a,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            if(page)return (drop_page*)page;
        }
    }
    return NULL;
}
static int read_float(uintptr_t at,float *out)
{
    __try {*out=*(const volatile float*)at;return 1;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
int sh_object_entry_install(const sig_result *results,size_t count)
{
    const sig_result *site;uintptr_t constant=0;float native=0;int32_t old_disp,new_disp;
    const char *why;char line[320];
    if(g_live)return 1;
    site=binding(results,count,"ObjectEntryDrop");
    if(!site){why="the entry drop load was not found";goto unavailable;}
    if(!sh_object_entry_site((const unsigned char*)site->addr,&constant)){why="the entry drop load has unexpected bytes";goto unavailable;}
    if(!read_float(constant,&native)||native!=SH_OBJECT_ENTRY_NATIVE_DROP){why="the entry drop constant is not 18364";goto unavailable;}
    if(!g_page&&!(g_page=near_page(site->addr))){why="no page within reach of the entry drop load";goto unavailable;}
    /* Written before the patch: the redirected load never reads an empty page. */
    g_page->drop=SH_OBJECT_ENTRY_DROP;
    memcpy(&old_disp,(const void*)(site->addr+5),4);
    if(!sh_object_entry_disp(site->addr,(uintptr_t)&g_page->drop,&new_disp)){why="the page is out of reach";goto release;}
    if(code_patch((void*)(site->addr+5),(const uint8_t*)&old_disp,(const uint8_t*)&new_disp,4,&g_patch)!=B2_PATCH_OK){
        why="the entry drop load could not be patched";goto release;}
    g_live=1;
    snprintf(line,sizeof line,"EDITOR: Object Mode entry drop %.0f at %p (native 18364), so TAB reaches the floor of a tall Grid Room",
        SH_OBJECT_ENTRY_DROP,(void*)site->addr);
    backend_log(line);
    return 1;
release:
    if(!g_patch.live){VirtualFree(g_page,0,MEM_RELEASE);g_page=NULL;}
unavailable:
    snprintf(line,sizeof line,"EDITOR: Object Mode entry drop redirect unavailable (%s); TAB into a room taller than about 18000 lands under its roof",why);
    backend_log(line);
    return 0;
}
int sh_object_entry_live(void){return g_live;}
float sh_object_entry_drop(void){return g_live&&g_page?g_page->drop:SH_OBJECT_ENTRY_NATIVE_DROP;}
#endif
