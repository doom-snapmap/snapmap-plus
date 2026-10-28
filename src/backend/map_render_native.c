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
/* idCVar::Set(cvar, text, force) and the r_zfar idCVar (value float +0x34). */
typedef unsigned char (*cvar_set_fn)(void *,const char *,unsigned char);
static cvar_set_fn g_set;
static void *g_zfar;

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
enum {STORE_FAILED=0,STORE_UNCHANGED=1,STORE_CHANGED=2,STORE_KEPT=3};
/* Remove the row at index from a string list of count rows, the way the
 * native list removes an element: every later row moves down one and the
 * count drops. String variables are bound by index (variableType 4,
 * variableIndex), so a row an author made must never move: only Snapmap+'s
 * own "smp." rows may follow. Otherwise the row stays with the native
 * distance, which reads as off. The last row is removed without allocating;
 * moving rows assigns their strings on the persistent heap. */
static int remove_row(unsigned char *list,int index,int count)
{
    unsigned char *rows=*(unsigned char**)list,*row;int j,result=STORE_FAILED;
    sh_process_heap_scope scope={0};
    for(j=index+1;j<count;j++) {
        row=rows+(size_t)j*104;
        if(!sh_map_render_own_variable(*(const char**)(row+16),*(int*)(row+8))) {
            static const char native[]="4;8192";
            row=rows+(size_t)index*104;
            if(*(int*)(row+64)==(int)strlen(native)&&*(const char**)(row+72)&&
               !memcmp(*(const char**)(row+72),native,strlen(native)))return STORE_KEPT;
            if(!sh_process_heap_enter(&g_heap,&scope))return STORE_FAILED;
            __try {g_assign(row+56,native);result=STORE_KEPT;}
            __finally {if(!sh_process_heap_leave(&scope))result=STORE_FAILED;}
            return result;
        }
    }
    if(index==count-1){*(int*)(list+8)=count-1;return STORE_CHANGED;}
    if(!sh_process_heap_enter(&g_heap,&scope))return STORE_FAILED;
    __try {
        for(j=index;j<count-1;j++) {
            unsigned char *to=rows+(size_t)j*104,*from=to+104;
            g_assign(to,*(const char**)(from+16));
            g_assign(to+56,*(const char**)(from+72));
            *(void**)(to+48)=*(void**)(from+48);
        }
        *(int*)(list+8)=count-1;result=STORE_CHANGED;
    } __finally {if(!sh_process_heap_leave(&scope))result=STORE_FAILED;}
    return result;
}
/* Make one string list carry this setting: on stores the canonical text,
 * appending the row when append is set; off removes the row. Every row an
 * author made keeps its index, and allocCount[4] (the engine's own string
 * counter) is never changed, so a map turned on and off again is the map it
 * was. Returns STORE_FAILED, STORE_UNCHANGED, STORE_CHANGED or STORE_KEPT
 * (off, but the row had to stay; it reads as off). */
static int store_list(unsigned char *source,int append,const sh_map_render *settings)
{
    char text[193];int index,count,result=STORE_FAILED;unsigned char *list,*row;
    sh_process_heap_scope scope={0};
    if(!g_assign||!source||!sh_map_render_valid(settings))return STORE_FAILED;
    __try {
        list=variable(source,&index,&count);if(!list)return STORE_FAILED;
        if(!sh_map_render_unlocked(settings)) {
            if(index<0)return STORE_UNCHANGED;
            result=remove_row(list,index,count);
        } else {
            if(!sh_map_render_encode(settings,text,sizeof text))return STORE_FAILED;
            if(index>=0) {
                row=*(unsigned char**)list+(size_t)index*104;
                if(*(int*)(row+64)==(int)strlen(text)&&*(const char**)(row+72)&&
                   !memcmp(*(const char**)(row+72),text,strlen(text)))return STORE_UNCHANGED;
            } else if(!append||!g_resize)return append?STORE_FAILED:STORE_UNCHANGED;
            if(!sh_process_heap_enter(&g_heap,&scope))return STORE_FAILED;
            __try {
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
                } else {
                    g_assign(row+56,text);
                }
                result=STORE_CHANGED;
            } __finally {if(!sh_process_heap_leave(&scope))result=STORE_FAILED;}
        }
        if(result==STORE_CHANGED||result==STORE_KEPT) {
            sh_map_render check;
            if(!read_list(source,&check)||check.unlocked!=settings->unlocked)result=STORE_FAILED;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){result=STORE_FAILED;}
    return result;
}
int sh_map_render_write(void *map,const sh_map_render *settings)
{
    int result=map?store_list((unsigned char*)map+0xa8,1,settings):STORE_FAILED;
    backend_log(result==STORE_FAILED?"RENDER: native map setting write failed":
        sh_map_render_unlocked(settings)?"RENDER: Unlock View Distance stored in this map's variables":
        result==STORE_KEPT?"RENDER: Unlock View Distance off; its variable stays as the native 4;8192 because later string variables follow it":
        "RENDER: Unlock View Distance off; no Snapmap+ view variable in this map");
    return result!=STORE_FAILED;
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
 * build copy, not the editable map, and Save does not pass through it. It
 * therefore only publishes the settings for the build being played. */
void sh_map_render_build(void *map)
{
    sh_map_render settings;int valid=sh_map_render_read(map,&settings);
    publish(&settings,valid,0);
}
/* A freshly deserialized map is normalized in place: a value that reads as on
 * becomes SH_RENDER_UNLOCKED_TEXT and one that reads as off (the native 8192,
 * or a disabled version 2) is removed. The editor constructs its editable map
 * from this snapshot, so the next Save stores the canonical text or nothing
 * without the settings tab being opened. No row is ever added here. */
static void migrate_snapshot(void *snapshot,const sh_map_render *settings)
{
    int result;
    if(!g_assign)return;
    result=store_list((unsigned char*)snapshot+0x1a0,0,settings);
    if(result==STORE_CHANGED)backend_log(sh_map_render_unlocked(settings)?
        "RENDER: older view distance read as Unlock View Distance on; stored as 4;0":
        "RENDER: native view distance variable removed; the map saves as the unmodified game writes it");
    else if(result==STORE_KEPT)backend_log("RENDER: native view distance variable kept; later string variables follow it");
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
    const sig_result *resize,*assign,*read,*decl,*set;unsigned char before[5],after[5];int32_t relative;
    intptr_t distance;void **slot;unsigned char *system;void **vars;int i,n;
    if(g_patch.live)return g_relay!=NULL;
    resize=binding(results,count,"RenderVariablesResize");assign=binding(results,count,"IdStrAssignCStr");
    read=binding(results,count,"RenderClipRead");decl=binding(results,count,"RenderParmFromOp");
    set=binding(results,count,"CvarSetString");
    if(!resize||!assign||!read||!decl||!sh_process_heap_bind(&g_heap,results,count,base))return 0;
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
