/* Map-owned variables and the native blended-environment rendering boundary. */
#include "engine_build.h"
#include "map_render.h"
#include "editor_zoom.h"
#include "engine_globals.h"
#include "process_heap_scope.h"
#include "patch.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

void backend_log(const char *message);
typedef unsigned char (*resize_fn)(void *,int);
typedef void (*assign_fn)(void *,const char *);
typedef float (*parm_float_fn)(void *,void *,uintptr_t);
typedef void *(*parm_decl_fn)(void *);
static resize_fn g_resize;
static assign_fn g_assign;
static parm_float_fn g_float;
static parm_decl_fn g_decl;
static sh_process_heap_api g_heap;
static const int *g_game_type;
static SRWLOCK g_lock=SRWLOCK_INIT;
static sh_map_render g_runtime;
static int g_ready;
static LONG g_faulted;
static sh_patch_handle g_patch;
static void *g_relay;

static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}

/* idSnapMapEdit owns idSnapVariables at +0x48. Its string list is +0x60
 * within that object, and allocCount[4] is +0x580. Native conversion copies
 * both fields into the serialized idSnapMap without an external sidecar. */
static unsigned char *variable(unsigned char *list,int *index,int *count)
{
    unsigned char *rows;
    int i,n=*(int*)(list+8),cap=*(int*)(list+12),found=-1;
    if(n<0||n>4096||cap<n||cap>65536)return NULL;
    rows=*(unsigned char**)list;if(n&&!rows)return NULL;
    for(i=0;i<n;i++) {
        unsigned char *row=rows+(size_t)i*104;
        int len=*(int*)(row+8);const char *name=*(const char**)(row+16);
        if(len==(int)strlen(SH_RENDER_VARIABLE)&&name&&!memcmp(name,SH_RENDER_VARIABLE,(size_t)len)) {
            if(found>=0)return NULL;found=i;
        }
    }
    *index=found;*count=n;return list;
}
static int read_list(unsigned char *source,sh_map_render *settings)
{
    int index,count;unsigned char *list,*row;const char *text;char copy[193];int len;
    if(!source||!settings)return 0;
    __try {
        list=variable(source,&index,&count);if(!list)return 0;
        if(index<0){sh_map_render_default(settings);return 1;}
        row=*(unsigned char**)list+(size_t)index*104+56;
        len=*(int*)(row+8);text=*(const char**)(row+16);
        if(len<1||len>192||!text)return 0;
        memcpy(copy,text,(size_t)len);copy[len]=0;
        return sh_map_render_decode(copy,settings);
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
int sh_map_render_read(void *map,sh_map_render *settings)
{
    return map&&read_list((unsigned char*)map+0xa8,settings);
}
/* Store the canonical text in one string list, keeping every existing row's
 * index. alloc_count is the owner's allocCount[4]; NULL forbids appending a
 * row (the list is left as it is when the variable is absent). Returns 0 on
 * failure, 1 when the list already held the text or was left alone, 2 when it
 * was changed. Allocation runs on the persistent heap and the caller's heap
 * stack is restored before returning. */
static int store_list(unsigned char *source,int *alloc_count,const sh_map_render *settings)
{
    char text[193];int index,count,result=0;unsigned char *list,*row;
    sh_process_heap_scope scope={0};
    if(!g_resize||!g_assign||!source||!sh_map_render_encode(settings,text,sizeof text))return 0;
    __try {
        __try {
            list=variable(source,&index,&count);if(!list)__leave;
            if(index>=0) {
                row=*(unsigned char**)list+(size_t)index*104;
                if(*(int*)(row+64)==(int)strlen(text)&&*(const char**)(row+72)&&
                   !memcmp(*(const char**)(row+72),text,strlen(text))){result=1;__leave;}
            } else if(!alloc_count){result=1;__leave;}
            if(!sh_process_heap_enter(&g_heap,&scope))__leave;
            if(index<0) {
                if(count==4096)__leave;
                if(count==*(int*)(list+12)&&!g_resize(list,count+1))__leave;
                row=*(unsigned char**)list+(size_t)count*104;
                /* Resize constructs each idStr. Assign copies into native
                 * storage before publishing the new variable's index. */
                g_assign(row,SH_RENDER_VARIABLE);
                g_assign(row+56,text);
                *(void**)(row+48)=NULL;
                *(int*)(list+8)=count+1;
                if(*alloc_count<count+1)*alloc_count=count+1;
            } else {
                g_assign(row+56,text);
            }
            {sh_map_render check;result=read_list(source,&check)&&!memcmp(&check,settings,sizeof check)?2:0;}
        } __finally {if(!sh_process_heap_leave(&scope))result=0;}
    } __except(EXCEPTION_EXECUTE_HANDLER){result=0;}
    return result;
}
int sh_map_render_write(void *map,const sh_map_render *settings)
{
    int ok=map&&store_list((unsigned char*)map+0xa8,(int*)((unsigned char*)map+0x5c8),settings)!=0;
    backend_log(ok?"RENDER: map rendering settings stored in native map variables":
                   "RENDER: native map setting write failed");
    return ok;
}
static void publish(const sh_map_render *settings,int valid,int quiet)
{
    AcquireSRWLockExclusive(&g_lock);
    if(valid)g_runtime=*settings;
    g_ready=valid;
    ReleaseSRWLockExclusive(&g_lock);
    if(quiet)return;
    backend_log(valid?(sh_map_render_distance_override(settings)?
        "RENDER: selected this map's view distance":
        "RENDER: selected this map's original module environments"):
        "RENDER: no valid map rendering metadata; native environment retained");
}
/* Native conversion (SnapMapEditToSnapBuild) serves Play only; its map is a
 * build copy, not the editable map, and Save does not pass through it. It
 * therefore only publishes the settings for the build being played. */
void sh_map_render_build(void *map)
{
    sh_map_render settings;int valid=sh_map_render_read(map,&settings);
    publish(&settings,valid,0);
}
/* An older or non-canonical value in a freshly deserialized map is rewritten
 * in place as version 4. The editor constructs its editable map from this
 * snapshot, so every later Save stores version 4 without the settings tab
 * being opened. The row keeps its index and no row is ever added here. */
static void migrate_snapshot(void *snapshot,const sh_map_render *settings)
{
    int result;
    if(!g_assign)return;
    result=store_list((unsigned char*)snapshot+0x1a0,NULL,settings);
    if(result==2)backend_log("RENDER: older map rendering metadata converted to version 4");
    else if(!result)backend_log("RENDER: older map rendering metadata kept; it is still read correctly");
}
void sh_map_render_loaded(void *map)
{
    /* Serialized idSnapMap owns its string list at +0x1a0. This path also
     * covers playing a saved/downloaded map without first entering the editor.
     * NULL clears the previous map before a load, including failed loads; that
     * clear precedes every load and is not logged. */
    sh_map_render settings;int valid;
    if(!map){publish(NULL,0,1);return;}
    valid=sh_map_render_capture(map,&settings)&&sh_map_render_valid(&settings);
    if(valid)migrate_snapshot(map,&settings);
    publish(&settings,valid,0);
}
int sh_map_render_capture(void *snapshot,sh_map_render *settings)
{
    return snapshot&&settings&&read_list((unsigned char*)snapshot+0x1a0,settings);
}
void sh_map_render_select(const sh_map_render *settings)
{
    publish(settings,settings&&sh_map_render_valid(settings),0);
}

/* This call reads the blended environment's maxViewDistance, after every
 * environment source has been merged: module environment layers, Set
 * Environment and Player Set Environment blends, and view renderparm
 * overrides. The engine clamps the result to 256 and writes it to r_zfar.
 * A non-default map distance therefore wins over all of them. The view's
 * existing literal is also changed so the block stays consistent; shared
 * environment declarations and unrelated render passes stay native. */
static float render_clip_value(void *block,void *decl,uintptr_t context)
{
    sh_map_render settings;int ready;
    if(!g_game_type||*g_game_type!=1||InterlockedCompareExchange(&g_faulted,0,0))
        return g_float(block,decl,context);
    AcquireSRWLockShared(&g_lock);ready=g_ready;settings=g_runtime;ReleaseSRWLockShared(&g_lock);
    if(!ready||!sh_map_render_distance_override(&settings))return g_float(block,decl,context);
    __try {
        unsigned char *b=(unsigned char*)block,*ops=*(unsigned char**)b;
        float *values=*(float**)(b+24);int i,j,n=*(int*)(b+8);
        if(n>=0&&n<=4096&&n<=*(int*)(b+32)&&ops&&values) {
            for(i=0;i<n;i++) {
                void *resource;const char *name;
                if((*(unsigned short*)(ops+(size_t)i*8)&31)!=0)continue;
                resource=g_decl(ops+(size_t)i*8);if(!resource)continue;
                /* The resource registry canonicalizes declaration names to lowercase. */
                name=*(const char**)((unsigned char*)resource+8);
                if(name&&!strcmp(name,"maxviewdistance"))
                    for(j=0;j<4;j++)values[(size_t)i*4+j]=settings.value[SH_RENDER_DISTANCE];
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(!InterlockedExchange(&g_faulted,1))backend_log("RENDER: environment override disabled after an engine exception");
        return g_float(block,decl,context);
    }
    /* Return the map distance even when the block has no literal to change. */
    (void)g_float(block,decl,context);
    return settings.value[SH_RENDER_DISTANCE];
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
    const sig_result *resize,*assign,*read,*decl;unsigned char before[5],after[5];int32_t relative;
    intptr_t distance;void **slot;unsigned char *system;void **vars;int i,n;
    if(g_patch.live)return g_relay!=NULL;
    resize=binding(results,count,"RenderVariablesResize");assign=binding(results,count,"IdStrAssignCStr");
    read=binding(results,count,"RenderClipRead");decl=binding(results,count,"RenderParmFromOp");
    if(!resize||!assign||!read||!decl||!sh_process_heap_bind(&g_heap,results,count,base))return 0;
    __try {
        slot=(void**)glb_resolve(base,"cvar_system_slot",NULL);if(!slot||!*slot)return 0;
        system=(unsigned char*)*slot;vars=*(void***)(system+8);n=*(int*)(system+16);
        if(!vars||n<1||n>100000)return 0;
        for(i=0;i<n;i++)if(vars[i]) {
            const char *name=*(const char**)((unsigned char*)vars[i]+SH_IDCVAR_NAME);
            if(name&&!strcmp(name,"com_gameType")){g_game_type=(const int*)((unsigned char*)vars[i]+0x30);break;}
        }
        if(!g_game_type)return 0;
        memcpy(before,(void*)read->addr,5);if(before[0]!=0xe8)return 0;
        memcpy(&relative,before+1,4);g_float=(parm_float_fn)(read->addr+5+relative);
        g_decl=(parm_decl_fn)decl->addr;g_resize=(resize_fn)resize->addr;g_assign=(assign_fn)assign->addr;
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
