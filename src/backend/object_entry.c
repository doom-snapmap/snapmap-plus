/* Object Mode entry drop length (see object_entry.h), build 13954591. The Module Mode focus ray stores the
 * nearest module hit as the FPS view's pending point (view+0x84 flag, +0x88 point), which the camera mode switch
 * hands to the entry solver (VK FUN_1412b3780 / GL FUN_1412b2e60). The solver box-traces down in segments of
 * 18364 until one misses; ObjectEntryDrop (VK 0x1412b3823 / GL 0x1412b2f03, MOVSS xmm9,[rip+disp32]) is the
 * only reader of that constant (VK 0x14238e388 / GL 0x1423a80d8), so only its disp32 is repointed. */
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
#include "near_page.h"
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
static int read_float(uintptr_t at,float *out)
{
    __try {*out=*(const volatile float*)at;return 1;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
/* The bind's byte checks, the install's and the image test's one function: the load's bytes, then its constant. */
const char *sh_object_entry_check(const unsigned char *site,uintptr_t *constant)
{
    float native=0;
    if(!sh_object_entry_site(site,constant))return "the entry drop load has unexpected bytes";
    if(!read_float(*constant,&native)||native!=SH_OBJECT_ENTRY_NATIVE_DROP)return "the entry drop constant is not 18364";
    return NULL;
}
int sh_object_entry_install(const sig_result *results,size_t count)
{
    const sig_result *site;uintptr_t constant=0;int32_t old_disp,new_disp;
    const char *why;char line[320];
    if(g_live)return 1;
    site=binding(results,count,"ObjectEntryDrop");
    if(!site){why="the entry drop load was not found";goto unavailable;}
    if((why=sh_object_entry_check((const unsigned char*)site->addr,&constant)))goto unavailable;
    /* The load's disp32 is relative to the instruction's end, site + 9. */
    if(!g_page&&!(g_page=(drop_page*)sh_near_page_alloc(site->addr+9))){why="no page within reach of the entry drop load";goto unavailable;}
    /* Written before the patch so the redirected load never reads an empty page. */
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
