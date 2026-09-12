/* Capture pre-build navigation snapshots and route per-instance AAS loads. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nav_play.h"
#include "nav_bake.h"
#include "hook.h"
#include "patch.h"
#include "config.h"
#include "nav_heap_queue.h"
#include "map_render.h"

void backend_log(const char *message);

/* MOV RAX,RSP / PUSH RBP / PUSH R12..R15 / MOV RBP,RSP -- 15 bytes, ending on a
 * whole-instruction boundary, and every one of them position-independent (no
 * RIP-relative operand, no relative jmp or call). The installer needs >= 14. */
#define SNAPBUILD_STOLEN 15

/* Three register arguments, no stack arguments; int return must pass through.
 * The pinned Vulkan call site at 0x4EE428 sets RCX=R14, RDX=[RBP+0x900],
 * R8=RDI and consumes EAX.
 */
typedef int (*snapbuild_fn_t)(void *a, void *b, void *c);

static snapbuild_fn_t g_orig;
static __declspec(thread) const unsigned char *g_build_map;
static __declspec(thread) char g_instance_resource[384];
typedef void *(*nav_find_fn)(void *,const char *,unsigned char);
typedef void *(*nav_load_fn)(void *,const char *,unsigned char,unsigned char);
static nav_find_fn g_find;
static nav_load_fn g_load;
static sh_patch_handle g_instance_patches[2];
static void *g_instance_relays[2];
static sh_patch_handle g_volume_contents_patch;
static void *g_volume_contents_relay;
static int g_volume_contents_ready, g_instances_ready;

typedef void (*nav_heap_push_fn)(void *, const sh_nav_heap_node *, sh_nav_heap_list *);
static nav_heap_push_fn g_heap_push;
static LONG g_heap_refused;

static void nav_heap_push(void *self, const sh_nav_heap_node *item, sh_nav_heap_list *list)
{
    sh_nav_heap_node pending = *item;
    int result = sh_nav_heap_admit(list, pending);
    if (result == SH_NAV_HEAP_NATIVE) g_heap_push(self, &pending, list);
    else if (result == SH_NAV_HEAP_INVALID && !InterlockedExchange(&g_heap_refused, 1))
        backend_log("NAV: refused an invalid path-search queue insertion");
}

static int nav_heap_install(const sig_result *results, size_t count)
{
    size_t i;
    if (g_heap_push) {
        if (hook_is_installed((void *)g_heap_push)) return 1;
        if (!hook_unpatch((void *)g_heap_push)) return 0;
        g_heap_push = NULL;
    }
    for (i = 0; i < count; ++i) {
        if (!results[i].name || strcmp(results[i].name, "NavSearchHeapPush") ||
            results[i].status != SIG_OK) continue;
        /* Three register saves: 15 whole bytes without relative operands. */
        g_heap_push = (nav_heap_push_fn)hook_prepare((void *)results[i].addr,
                                                     (void *)nav_heap_push, 15);
        if (!g_heap_push) break;
        if (hook_commit((void *)g_heap_push) == B2_PATCH_OK) {
            backend_log("NAV: path-search queue capacity safeguard installed");
            return 1;
        }
        if (hook_unpatch((void *)g_heap_push)) g_heap_push = NULL;
        break;
    }
    backend_log("NAV: path-search queue capacity safeguard unavailable");
    return 0;
}

/* The marker is stored independently, but walkable solid geometry must not be
 * registered as an avoidance obstacle. Keep native physical collision and the
 * native flag's behavior; add the derived policy only to marked Blocking Boxes.
 * This executes inside their contents update, including spawn and copied boxes. */
static unsigned char nav_volume_clear_obstacle(const unsigned char *entity)
{
    int enabled=0;
    if(entity[0xc8e])return 1;
    return entity[0xc89]&&(entity[0x3ea]&0x40)&&
        sh_config_get_bool("navmesh.enabled",&enabled,NULL)&&enabled;
}

static void *nav_instance_find(void *self,const char *name,unsigned char flags,
                               const unsigned char *record)
{
    g_instance_resource[0]=0;
    __try {
        if(g_build_map) {
            const unsigned char *base=*(const unsigned char *const *)(g_build_map+0x750);
            int count=*(const int *)(g_build_map+0x758);
            uintptr_t offset=(uintptr_t)record-(uintptr_t)base;
            if(base&&count>0&&count<=256&&offset<(uintptr_t)count*0x98&&offset%0x98==0 &&
               sh_nav_bake_instance_name((int)(offset/0x98),name,g_instance_resource,
                                        sizeof g_instance_resource))return NULL;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { g_instance_resource[0]=0; }
    return g_find(self,name,flags);
}

static void *nav_instance_load(void *self,const char *name,unsigned char a,unsigned char b)
{
    void *result;
    const char *selected=g_instance_resource[0]?g_instance_resource:name;
    __try { result=g_load(self,selected,a,b); }
    __finally { g_instance_resource[0]=0; }
    return result;
}

/* Leaf relays preserve the original call frame. The find-site relay supplies
 * its live R14 instance record as the fourth argument; the load relay leaves
 * all four original arguments intact. */
static void *nav_near_relay(uintptr_t call,void *handler,int instance)
{
    SYSTEM_INFO info;uintptr_t step,center,delta;
    GetSystemInfo(&info);step=info.dwAllocationGranularity;center=call&~(step-1);
    for(delta=step;delta<0x7fff0000u;delta+=step) {
        uintptr_t candidates[2]={center>=delta?center-delta:0,center+delta};int i;
        for(i=0;i<2;i++) {
            MEMORY_BASIC_INFORMATION mbi;unsigned char *relay;DWORD old;int at=0;
            uintptr_t address=candidates[i];
            if(address<(uintptr_t)info.lpMinimumApplicationAddress||
               address>(uintptr_t)info.lpMaximumApplicationAddress||
               !VirtualQuery((void*)address,&mbi,sizeof mbi)||mbi.State!=MEM_FREE)continue;
            relay=(unsigned char*)VirtualAlloc((void*)address,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            if(!relay)continue;
            if(instance==1){relay[at++]=0x4d;relay[at++]=0x8b;relay[at++]=0xce;}
            if(instance==2){relay[at++]=0x48;relay[at++]=0x8b;relay[at++]=0xcb;}
            relay[at++]=0xff;relay[at++]=0x25;
            memset(relay+at,0,4);at+=4;memcpy(relay+at,&handler,8);at+=8;
            if(!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&old)){
                VirtualFree(relay,0,MEM_RELEASE);return NULL;}
            FlushInstructionCache(GetCurrentProcess(),relay,at);return relay;
        }
    }
    return NULL;
}

int sh_nav_play_install_volume_contents(const sig_result *results,size_t count)
{
    const unsigned char expected[9]={0x80,0xbb,0x8e,0x0c,0,0,0,0x74,0x0a};
    unsigned char patch[9]={0xe8,0,0,0,0,0x84,0xc0,0x74,0x0a};
    size_t i;intptr_t distance;int32_t relative;
    nav_heap_install(results, count);
    if(g_volume_contents_patch.live) {
        if(g_volume_contents_ready)return 1;
        if(code_unpatch(&g_volume_contents_patch)!=B2_PATCH_OK)return 0;
        VirtualFree(g_volume_contents_relay,0,MEM_RELEASE);g_volume_contents_relay=NULL;
    }
    g_volume_contents_ready=0;
    for(i=0;i<count;i++)if(results[i].name&&
        !strcmp(results[i].name,"BlockingVolumeObstacleGate")&&results[i].status==SIG_OK) {
        g_volume_contents_relay=nav_near_relay(results[i].addr,
                                              (void*)nav_volume_clear_obstacle,2);
        if(!g_volume_contents_relay)break;
        distance=(intptr_t)g_volume_contents_relay-(intptr_t)(results[i].addr+5);
        if(distance>=INT32_MIN&&distance<=INT32_MAX) {
            relative=(int32_t)distance;memcpy(patch+1,&relative,4);
            if(code_patch_sig(&results[i],expected,patch,sizeof patch,
                              &g_volume_contents_patch)==B2_PATCH_OK) {
                g_volume_contents_ready=1;
                backend_log("NAV: marked-volume obstacle policy installed");return 1;
            }
        }
        if(!g_volume_contents_patch.live) {
            VirtualFree(g_volume_contents_relay,0,MEM_RELEASE);g_volume_contents_relay=NULL;
        } else backend_log("NAV: obstacle patch rollback incomplete; relay retained");
        break;
    }
    backend_log("NAV: marked-volume obstacle policy unavailable");return 0;
}

int sh_nav_play_install_instances(const sig_result *results,size_t count)
{
    const sig_result *sites[2]={NULL,NULL};size_t i;int k;
    if(g_instances_ready)return 1;
    if(g_instance_patches[0].live||g_instance_patches[1].live)goto failed;
    if(!hook_is_installed((void *)g_orig))return 0;
    for(i=0;i<count;i++)if(results[i].name) {
        if(!strcmp(results[i].name,"BuildAASFindCall"))sites[0]=&results[i];
        if(!strcmp(results[i].name,"BuildAASLoadCall"))sites[1]=&results[i];
    }
    for(k=0;k<2;k++)if(!sites[k]||sites[k]->status!=SIG_OK)goto failed;
    for(k=0;k<2;k++) {
        unsigned char expected[5],patch[5]={0xe8};int32_t relative;intptr_t distance;
        memcpy(expected,(void*)sites[k]->addr,5);if(expected[0]!=0xe8)goto failed;
        memcpy(&relative,expected+1,4);
        if(k==0)g_find=(nav_find_fn)(sites[k]->addr+5+relative);
        else g_load=(nav_load_fn)(sites[k]->addr+5+relative);
        g_instance_relays[k]=nav_near_relay(sites[k]->addr,
            k==0?(void*)nav_instance_find:(void*)nav_instance_load,k==0);
        if(!g_instance_relays[k])goto failed;
        distance=(intptr_t)g_instance_relays[k]-(intptr_t)(sites[k]->addr+5);
        if(distance<INT32_MIN||distance>INT32_MAX)goto failed;
        relative=(int32_t)distance;memcpy(patch+1,&relative,4);
        if(code_patch_sig(sites[k],expected,patch,5,&g_instance_patches[k])!=B2_PATCH_OK)goto failed;
    }
    sh_nav_bake_enable_instances(1);
    g_instances_ready=1;
    backend_log("NAV: instance-specific temporary AAS loads installed");return 1;
failed:
    for(k=1;k>=0;k--) {
        if(g_instance_patches[k].live)code_unpatch(&g_instance_patches[k]);
        if(g_instance_relays[k]&&!g_instance_patches[k].live){VirtualFree(g_instance_relays[k],0,MEM_RELEASE);g_instance_relays[k]=NULL;}
    }
    backend_log("NAV: instance-specific AAS loads unavailable");return 0;
}

static int nav_snapbuild_detour(void *a, void *b, void *c)
{
    /* Capture the final snapshot before conversion; contain faults at the
     * hook boundary.
     */
    __try {
        sh_map_render_build(b);
        sh_nav_bake_build_begin();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("NAV: the pre-build editor snapshot faulted");
    }
    g_build_map=(const unsigned char*)b;
    __try {
        return g_orig(a, b, c);
    } __finally {
        g_build_map=NULL;g_instance_resource[0]=0;
        sh_nav_bake_build_end();
    }
}

int sh_nav_play_install(void *snapbuild_fn, int status_ok)
{
    char line[200];
    void *tramp;

    if (g_orig) {
        if (hook_is_installed((void *)g_orig)) return 1;
        if (!hook_unpatch((void *)g_orig)) return 0;
        g_orig = NULL;
    }
    if (snapbuild_fn == NULL) {
        backend_log("NAV: pre-build live read SKIPPED -- SnapMapEditToSnapBuild not resolved");
        return 0;
    }
    if (!status_ok) {
        /* Hook-tolerant resolution can point at an existing detour; its bytes
         * are not a usable prologue.
         */
        backend_log("NAV: pre-build live read SKIPPED -- SnapMapEditToSnapBuild resolved via "
                    "hook-tolerant fallback (prologue already hooked)");
        return 0;
    }
    tramp = hook_prepare(snapbuild_fn, (void *)nav_snapbuild_detour, SNAPBUILD_STOLEN);
    if (tramp == NULL) {
        backend_log("NAV: pre-build live read FAIL -- trampoline preparation failed");
        return 0;
    }
    g_orig = (snapbuild_fn_t)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_orig = NULL;
        backend_log("NAV: pre-build hook commit failed; retained callbacks require restoration");
        return 0;
    }

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: pre-build live read installed at %p (trampoline %p, stolen %d) -- "
                "a volume ticked this session is baked on Play without saving first",
                snapbuild_fn, tramp, SNAPBUILD_STOLEN);
    backend_log(line);
    return 1;
}
