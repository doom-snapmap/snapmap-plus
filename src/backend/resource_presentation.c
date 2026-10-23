#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <intrin.h>
#include "resource_presentation.h"
#include "hook.h"
#include "host_image.h"
#include "native_pin_hash.h"
static int rp_digest(const unsigned char *bytes,size_t length,const unsigned char expected[32])
{
    sh_native_pin_hash hash;unsigned char actual[32];
    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,bytes,length);
    sh_native_pin_hash_finish(&hash,actual);return !memcmp(actual,expected,32);
}


typedef void (*rp_update_fn)(void *,unsigned char);
static rp_update_fn g_update;
static sh_resource_presentation_begin g_begin;
static sh_resource_presentation_end g_end;
static const unsigned char *g_base;
static int g_renderer;
static volatile LONG g_ready;
static const uint32_t g_update_rva[2]={0x17d3170,0x17e0800};
static const uint32_t g_refresh_rva[2]={0x17c9710,0x17d6da0};
static const uint32_t g_sites[2][2]={{0x19fb517,0x19fb55f},{0x1a09307,0x1a0934f}};
static const unsigned char g_refresh[2][37]={
 {0x83,0x3d,0xc9,0x38,0xb2,2,0,0x74,0x1b,0x80,0x79,0x20,0,0x74,0x15,0x48,0x8b,0x49,0x18,0x48,0x85,0xc9,0x74,0x0c,0x48,0x8b,1,0xb2,1,0x48,0xff,0xa0,0xe0,0,0,0,0xc3},
 {0x83,0x3d,0x29,0x7f,0x21,4,0,0x74,0x1b,0x80,0x79,0x20,0,0x74,0x15,0x48,0x8b,0x49,0x18,0x48,0x85,0xc9,0x74,0x0c,0x48,0x8b,1,0xb2,1,0x48,0xff,0xa0,0xe0,0,0,0,0xc3}};
static const unsigned char g_update_head[2][14]={
 {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xe8,0x82,0xb9,0x21,0},
 {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xe8,0x62,0xc0,0x21,0}};
static const unsigned char g_dispatch[2][27]={
 {0x8b,3,0x48,0x8d,0x95,0xb0,5,0,0,0x48,0x8b,0xcb,0xff,0x50,8,0x48,0x8b,0x5b,8,0x48,0x85,0xdb,0x75,0xd0,0x48,0x8d,0x8d},
 {0x8b,3,0x48,0x8d,0x95,0xb0,5,0,0,0x48,0x8b,0xcb,0xff,0x50,8,0x48,0x8b,0x5b,8,0x48,0x85,0xdb,0x75,0xd0,0x41,0xbd,1}};
static const unsigned char g_update_tail_gl[32]={0x53,0xa1,0xb2,0x41,0x3d,0xa1,0x33,0xf5,0x43,0xba,0xc7,0x1c,0x4b,0x71,0x87,0xb9,0x4d,0x65,0xe8,0xcd,0x59,0x29,0x1f,0xfa,0x5e,0x46,0x5f,0xeb,0xeb,0xd6,0x3a,0x4a};
static const unsigned char g_update_tail_vk[32]={0x2d,0xbe,0xee,0x8a,0x8d,0x57,0xbe,0x91,0x42,0x8e,0xcf,0x45,0xc6,0xfb,0x95,0x61,0xf3,0x8f,0x29,0x42,0x5b,0xe6,0x38,0x5f,0x41,0x99,0x02,0x62,0x48,0xe6,0x71,0x00};
static int rp_pins(const unsigned char *base,int renderer,int update)
{
    if(!base || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer)) return 0;
    __try {
        if(memcmp(base+g_refresh_rva[renderer],g_refresh[renderer],37)) return 0;
        for(size_t i=0;i<2;i++) if(memcmp(base+g_sites[renderer][i]-15,g_dispatch[i],27)) return 0;
        if(update && memcmp(base+g_update_rva[renderer],g_update_head[renderer],14)) return 0;
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rp_matches(void *common,const void *caller)
{
    if(!sh_resource_presentation_ready() || !common ||
        (caller!=g_base+g_sites[g_renderer][0] && caller!=g_base+g_sites[g_renderer][1])) return 0;
    __try { return (*(void ***)common)[0xe0/8]==g_base+g_update_rva[g_renderer]; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void rp_call(void *common,unsigned char force,const void *caller)
{
    sh_resource_presentation_scope scope={0}; int attempted=rp_matches(common,caller);
    __try {
        if(attempted) __try { g_begin(&scope); } __except(EXCEPTION_EXECUTE_HANDLER) { }
        g_update(common,force);
    } __finally {
        if(attempted) __try { g_end(&scope); } __except(EXCEPTION_EXECUTE_HANDLER) { }
    }
}
static void rp_update(void *common,unsigned char force) { rp_call(common,force,_ReturnAddress()); }
int sh_resource_presentation_ready(void)
{
    if(!InterlockedCompareExchange(&g_ready,0,0) || !g_update || !hook_is_installed((void *)g_update) ||
        sh_host_is_vulkan()!=g_renderer || !rp_pins(g_base,g_renderer,0)) return 0;
    __try {
        const unsigned char *entry=g_base+g_update_rva[g_renderer];
        static const unsigned char redirect[6]={0xff,0x25,0,0,0,0};
        void *detour=NULL; memcpy(&detour,entry+6,8);
        if(memcmp(entry,redirect,6) || detour!=(void *)rp_update) return 0;
        return g_renderer ? rp_digest(entry+14,131,g_update_tail_vk) :
            rp_digest(entry+14,131,g_update_tail_gl);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int sh_resource_presentation_uninstall(void)
{
    InterlockedExchange(&g_ready,0);
    if(g_update && !hook_unpatch((void *)g_update)) return 0;
    g_update=NULL; g_begin=NULL; g_end=NULL; g_base=NULL; return 1;
}
int sh_resource_presentation_install(const uint8_t *base,sh_resource_presentation_begin begin,sh_resource_presentation_end end)
{
    if(sh_resource_presentation_ready()) return g_base==base && g_begin==begin && g_end==end;
    int renderer=sh_host_is_vulkan();
    if(g_update || !begin || !end || !rp_pins(base,renderer,1)) return 0;
    __try {
        if(renderer ? !rp_digest(base+g_update_rva[renderer]+14,131,g_update_tail_vk) :
            !rp_digest(base+g_update_rva[renderer]+14,131,g_update_tail_gl)) return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_base=base; g_renderer=renderer; g_begin=begin; g_end=end;
    g_update=(rp_update_fn)hook_prepare_relative_call((void *)(base+g_update_rva[renderer]),rp_update,14,9);
    if(!g_update || hook_commit((void *)g_update)!=B2_PATCH_OK) { sh_resource_presentation_uninstall(); return 0; }
    InterlockedExchange(&g_ready,1); return 1;
}
