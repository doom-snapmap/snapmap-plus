/* The open map's view distance setting and the native blended-environment
 * rendering boundary. */
#include "engine_build.h"
#include "map_render.h"
#include "map_settings.h"
#include "editor_zoom.h"
#include "engine_globals.h"
#include "patch.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

void backend_log(const char *message);
typedef float (*parm_float_fn)(void *,void *,uintptr_t);
typedef void *(*parm_decl_fn)(void *);
static parm_float_fn g_float;
static parm_decl_fn g_decl;
static const int *g_game_type;
static SRWLOCK g_lock=SRWLOCK_INIT;
static sh_map_render g_runtime;
static int g_ready;
static LONG g_faulted;
static sh_patch_handle g_patch;
static void *g_relay;
/* idCVar::Set(cvar, text, force) and the r_zfar idCVar (value float +0x34). */
typedef unsigned char (*cvar_set_fn)(void *,const char *,unsigned char);
static cvar_set_fn g_set;
static void *g_zfar;

static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}

/* The setting belongs to the open map and is held by map_settings.c: every
 * map parse reads it from the "render" key of the map's member's settings
 * part, which goes before the engine sees it, and every save writes it back. The engine's own
 * variable lists never carry it, so the map argument only names the map the
 * caller means. */
int sh_map_render_read(void *map,sh_map_render *settings)
{
    sh_map_settings current;
    if(!map||!settings)return 0;
    sh_map_settings_current(&current);
    *settings=current.render;
    return sh_map_render_valid(settings);
}
int sh_map_render_write(void *map,const sh_map_render *settings)
{
    if(!map||!sh_map_render_valid(settings)) {
        backend_log("RENDER: map setting write failed");return 0;
    }
    sh_map_settings_set_render(settings);
    backend_log(sh_map_render_unlocked(settings)?
        "RENDER: Unlock View Distance on for this map; its next save stores it":
        "RENDER: Unlock View Distance off for this map; its next save stores no view variable");
    return 1;
}
/* r_zfar is also left as it is in frames where no environment resolves (the
 * blend writes nothing then), so an unlocked map sets it once when selected.
 * Every other writer then agrees: the blend's read returns the maximum and the
 * Blueprint far plane is raised to it (R-024 writer survey). */
static void force_far_clip(void)
{
    if(!g_set||!g_zfar)return;
    __try {
        if(*(const float*)((unsigned char*)g_zfar+0x34)!=SH_RENDER_MAX_DISTANCE)
            (void)g_set(g_zfar,"200000",1);
    } __except(EXCEPTION_EXECUTE_HANDLER){g_set=NULL;backend_log("RENDER: far clip write disabled after an engine exception");}
}
static void publish(const sh_map_render *settings,int valid,int quiet)
{
    int unlocked=valid&&sh_map_render_unlocked(settings);
    AcquireSRWLockExclusive(&g_lock);
    if(valid)g_runtime=*settings;
    g_ready=valid;
    ReleaseSRWLockExclusive(&g_lock);
    sh_editor_zoom_set_far_floor(unlocked?SH_RENDER_MAX_DISTANCE:0);
    if(unlocked)force_far_clip();
    if(quiet)return;
    backend_log(valid?(unlocked?
        "RENDER: selected this map's unlocked view distance; far clip 200000, environment fog stretched to match":
        "RENDER: selected this map's original module environments"):
        "RENDER: no valid map rendering metadata; native environment retained");
}
/* Native conversion (SnapMapEditToSnapBuild) serves Play only; its map is a
 * build copy of the editable map. Play uses the open map's setting. */
void sh_map_render_build(void *map)
{
    sh_map_render settings;int valid=sh_map_render_read(map,&settings);
    publish(&settings,valid,0);
}
/* After a successful parse: select the parsed map's setting, which
 * map_settings.c has just made current. NULL clears the selection silently
 * before every load, including failed loads. */
void sh_map_render_loaded(void *map)
{
    sh_map_render settings;int valid;
    if(!map){publish(NULL,0,1);return;}
    valid=sh_map_render_read(map,&settings);
    publish(&settings,valid,0);
}
void sh_map_render_select(const sh_map_render *settings)
{
    publish(settings,settings&&sh_map_render_valid(settings),0);
}

/* Overwrite every literal op of the named render parm in a blended block
 * (ops +0, count +8, values +24 as 16-byte vectors, value count +32). The
 * block is the view's own copy: the blend copies the target's ops and values
 * element by element (VK FUN_1415b8260 / GL FUN_1415b9110) and rebuilds it
 * every frame, so a shared environment declaration is never changed and
 * nothing needs restoring. Callers guard with __try. */
static int write_literals(void *block,const char *parm,float value)
{
    unsigned char *b=(unsigned char*)block,*ops=*(unsigned char**)b;
    float *values=*(float**)(b+24);int i,j,n=*(int*)(b+8),written=0;
    if(n<0||n>4096||n>*(int*)(b+32)||!ops||!values)return 0;
    for(i=0;i<n;i++) {
        void *resource;const char *name;
        if((*(unsigned short*)(ops+(size_t)i*8)&31)!=0)continue;
        resource=g_decl(ops+(size_t)i*8);if(!resource)continue;
        /* The resource registry canonicalizes declaration names to lowercase. */
        name=*(const char**)((unsigned char*)resource+8);
        if(name&&!strcmp(name,parm)){for(j=0;j<4;j++)values[(size_t)i*4+j]=value;written++;}
    }
    return written;
}

/* Scale every literal op of the named parm in the same block: a distance
 * is multiplied by r, a strength divided by it. Callers guard with __try. */
static int scale_literals(void *block,const char *parm,float r,int strength)
{
    unsigned char *b=(unsigned char*)block,*ops=*(unsigned char**)b;
    float *values=*(float**)(b+24);int i,j,n=*(int*)(b+8),written=0;
    if(r<=1||n<0||n>4096||n>*(int*)(b+32)||!ops||!values)return 0;
    for(i=0;i<n;i++) {
        void *resource;const char *name;
        if((*(unsigned short*)(ops+(size_t)i*8)&31)!=0)continue;
        resource=g_decl(ops+(size_t)i*8);if(!resource)continue;
        name=*(const char**)((unsigned char*)resource+8);
        if(name&&!strcmp(name,parm)) {
            for(j=0;j<4;j++) {
                float *v=values+(size_t)i*4+j;
                *v=strength?sh_map_render_fog_strength(*v,r):sh_map_render_fog_distance(*v,r);
            }
            written++;
        }
    }
    return written;
}

/* This call reads the blended environment's maxViewDistance, after every
 * environment source has been merged: module environment layers, Set
 * Environment and Player Set Environment blends, and view renderparm
 * overrides. The engine raises the result to at least 256 and writes it to
 * r_zfar. With Unlock View Distance on, the read returns the maximum and the
 * same block's fog is stretched by the same factor as the clip, whatever the
 * environment, in the editor and Play alike (R-024); off returns the
 * engine's own read. The view's literal
 * is changed too so the block stays consistent; shared environment
 * declarations and unrelated render passes stay native. The blend reaches
 * this read only when some environment resolves; publish() covers the frames
 * where none does. */
static float render_clip_value(void *block,void *decl,uintptr_t context)
{
    sh_map_render settings;int ready;float native,r;
    if(!g_game_type||*g_game_type!=1||InterlockedCompareExchange(&g_faulted,0,0))
        return g_float(block,decl,context);
    AcquireSRWLockShared(&g_lock);ready=g_ready;settings=g_runtime;ReleaseSRWLockShared(&g_lock);
    if(!ready||!sh_map_render_unlocked(&settings))return g_float(block,decl,context);
    /* The blended block's own distance, before it is replaced. */
    native=g_float(block,decl,context);
    r=sh_map_render_fog_stretch(native);
    __try {
        int i;
        /* With the far clip at the maximum no environment's fog may saturate
         * inside it, but each Environment FX keeps its look: its fog is
         * stretched by the same factor as its clip. The renderer packs these
         * parms into fogParms/fogParms2 from the parm state this block is
         * applied to right after the blend (VK FUN_1417b3d70 / GL
         * FUN_1417b5000), so the change lasts for this view's frame only. */
        for(i=0;i<SH_RENDER_FOG_DISTANCE_COUNT;i++)
            scale_literals(block,sh_map_render_fog_distance_parms[i],r,0);
        for(i=0;i<SH_RENDER_FOG_STRENGTH_COUNT;i++)
            scale_literals(block,sh_map_render_fog_strength_parms[i],r,1);
        write_literals(block,"maxviewdistance",sh_map_render_distance(&settings));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(!InterlockedExchange(&g_faulted,1))backend_log("RENDER: environment override disabled after an engine exception");
        return native;
    }
    /* Return the map distance even when the block has no literal to change. */
    return sh_map_render_distance(&settings);
}

/* In the editor's Blueprint frames the editor camera also forces r_zfar;
 * editor_zoom raises this value under the editor's own condition so both
 * per-frame writers agree. Elsewhere the value is unchanged. */
static float render_clip(void *block,void *decl,uintptr_t context)
{
    return sh_editor_zoom_far_plane(render_clip_value(block,decl,context));
}

static void *near_relay(uintptr_t call)
{
    SYSTEM_INFO info;uintptr_t step,center,delta;void *handler=(void*)render_clip;
    GetSystemInfo(&info);step=info.dwAllocationGranularity;center=call&~(step-1);
    for(delta=step;delta<0x7fff0000u;delta+=step) {
        uintptr_t candidates[]={center>=delta?center-delta:0,center+delta};int i;
        for(i=0;i<2;i++) {
            MEMORY_BASIC_INFORMATION mbi;unsigned char *relay;DWORD old;uintptr_t address=candidates[i];
            if(address<(uintptr_t)info.lpMinimumApplicationAddress||address>(uintptr_t)info.lpMaximumApplicationAddress||
               !VirtualQuery((void*)address,&mbi,sizeof mbi)||mbi.State!=MEM_FREE)continue;
            relay=(unsigned char*)VirtualAlloc((void*)address,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            if(!relay)continue;
            relay[0]=0xff;relay[1]=0x25;memset(relay+2,0,4);memcpy(relay+6,&handler,8);
            if(!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(relay,0,MEM_RELEASE);return NULL;}
            FlushInstructionCache(GetCurrentProcess(),relay,14);return relay;
        }
    }
    return NULL;
}
int sh_map_render_install(const sig_result *results,size_t count,const uint8_t *base)
{
    const sig_result *read,*decl,*set;unsigned char before[5],after[5];int32_t relative;
    intptr_t distance;void **slot;unsigned char *system;void **vars;int i,n;
    if(g_patch.live)return g_relay!=NULL;
    read=binding(results,count,"RenderClipRead");decl=binding(results,count,"RenderParmFromOp");
    set=binding(results,count,"CvarSetString");
    if(!read||!decl)return 0;
    __try {
        slot=(void**)glb_resolve(base,"cvar_system_slot",NULL);if(!slot||!*slot)return 0;
        system=(unsigned char*)*slot;vars=*(void***)(system+8);n=*(int*)(system+16);
        if(!vars||n<1||n>100000)return 0;
        for(i=0;i<n;i++)if(vars[i]) {
            const char *name=*(const char**)((unsigned char*)vars[i]+SH_IDCVAR_NAME);
            if(name&&!strcmp(name,"com_gameType"))g_game_type=(const int*)((unsigned char*)vars[i]+0x30);
            else if(name&&!strcmp(name,"r_zfar"))g_zfar=vars[i];
        }
        if(!g_game_type)return 0;
        /* Without the setter or r_zfar only frames with no environment keep
         * their previous far clip; everything else still works. */
        if(set&&g_zfar)g_set=(cvar_set_fn)set->addr;
        else backend_log("RENDER: r_zfar setter unavailable; frames without an environment keep their previous far clip");
        memcpy(before,(void*)read->addr,5);if(before[0]!=0xe8)return 0;
        memcpy(&relative,before+1,4);g_float=(parm_float_fn)(read->addr+5+relative);
        g_decl=(parm_decl_fn)decl->addr;
        g_relay=near_relay(read->addr);if(!g_relay)return 0;
        distance=(intptr_t)g_relay-(intptr_t)(read->addr+5);
        if(distance<INT32_MIN||distance>INT32_MAX)goto fail;
        relative=(int32_t)distance;after[0]=0xe8;memcpy(after+1,&relative,4);
        if(code_patch_sig(read,before,after,5,&g_patch)!=B2_PATCH_OK)goto fail;
        backend_log("RENDER: native per-map view distance installed");return 1;
fail:
        if(!g_patch.live){VirtualFree(g_relay,0,MEM_RELEASE);g_relay=NULL;}
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return 0;
}
