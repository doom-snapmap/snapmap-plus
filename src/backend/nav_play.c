/* nav_play.c -- see nav_play.h for what this is and why it hooks where it does. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nav_play.h"
#include "nav_bake.h"
#include "hook.h"
#include "patch.h"

void backend_log(const char *message);

/* MOV RAX,RSP / PUSH RBP / PUSH R12..R15 / MOV RBP,RSP -- 15 bytes, ending on a
 * whole-instruction boundary, and every one of them position-independent (no
 * RIP-relative operand, no relative jmp or call). The installer needs >= 14. */
#define SNAPBUILD_STOLEN 15

/* int(void *, void *, void *) -- three register args and no stack args, read off
 * the call site at 0x4EE428 (RCX=R14, RDX=[RBP+0x900], R8=RDI) and off the
 * prologue, which homes RBX/RSI/RDI into the caller's shadow space rather than
 * reading anything above the frame. The return value IS used: the call site does
 * MOV EBX,EAX immediately after, so the detour must pass it through. */
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
            if(instance){relay[at++]=0x4d;relay[at++]=0x8b;relay[at++]=0xce;}
            relay[at++]=0xff;relay[at++]=0x25;
            memset(relay+at,0,4);at+=4;memcpy(relay+at,&handler,8);at+=8;
            if(!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&old)){
                VirtualFree(relay,0,MEM_RELEASE);return NULL;}
            FlushInstructionCache(GetCurrentProcess(),relay,at);return relay;
        }
    }
    return NULL;
}

int sh_nav_play_install_instances(const sig_result *results,size_t count)
{
    const sig_result *sites[2]={NULL,NULL};size_t i;int k;
    if(!g_orig)return 0;
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
    backend_log("NAV: instance-specific temporary AAS loads installed");return 1;
failed:
    for(k=1;k>=0;k--) {
        if(g_instance_patches[k].live)code_unpatch(&g_instance_patches[k]);
        if(g_instance_relays[k]){VirtualFree(g_instance_relays[k],0,MEM_RELEASE);g_instance_relays[k]=NULL;}
    }
    backend_log("NAV: instance-specific AAS loads unavailable");return 0;
}

static int nav_snapbuild_detour(void *a, void *b, void *c)
{
    /* The whole reason this hook exists. Guarded and non-fatal: the marks are a
     * convenience over the map as loaded, and failing to read them must never be
     * worse than not having read them. A fault here would otherwise land in the
     * middle of the engine's map build, which is the exact shape of the bug this
     * replaces. */
    __try {
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

    if (snapbuild_fn == NULL) {
        backend_log("NAV: pre-build live read SKIPPED -- SnapMapEditToSnapBuild not resolved");
        return 0;
    }
    if (!status_ok) {
        /* Same rule the rawmap swap follows: a prologue that only resolved through
         * the hook-tolerant fallback is already detoured by somebody else, and
         * installing over it steals detour bytes rather than the real prologue. */
        backend_log("NAV: pre-build live read SKIPPED -- SnapMapEditToSnapBuild resolved via "
                    "hook-tolerant fallback (prologue already hooked)");
        return 0;
    }
    if (g_orig != NULL) return 1;

    tramp = install_inline_hook(snapbuild_fn, (void *)nav_snapbuild_detour, SNAPBUILD_STOLEN);
    if (tramp == NULL) {
        backend_log("NAV: pre-build live read FAIL -- install_inline_hook returned NULL");
        return 0;
    }
    g_orig = (snapbuild_fn_t)tramp;

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: pre-build live read installed at %p (trampoline %p, stolen %d) -- "
                "a volume ticked this session is baked on Play without saving first",
                snapbuild_fn, tramp, SNAPBUILD_STOLEN);
    backend_log(line);
    return 1;
}
