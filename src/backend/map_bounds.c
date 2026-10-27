/* "Unlock Map Bounds": one versioned map string variable and the engine cvars
 * that bound module placement, the editor camera and Blueprint floors.
 *
 * Engine evidence (snaphak-re editor-fixes-13954591 R-004, build 13954591):
 * every reader of snapEdit_environmentModuleBounds loads the float at idCVar
 * +0x34 (VK 0x145c3e5d0+0x34, GL 0x144543c50+0x34). Readers: ReconnectPortals
 * (module validity, VK 0x11f95d0), the Blueprint ground plane and grid, the
 * editor camera pan clamps and the Blueprint zoom clamp (limit * 0.5). The
 * idSnapMapEdit constructor runs ReconnectPortals while any client compiles a
 * map for Play, so the limit must be selected before that client builds it. */
#include "map_bounds.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void sh_map_bounds_default(sh_map_bounds *s)
{
    if(s)s->limit=0;
}
int sh_map_bounds_valid(const sh_map_bounds *s)
{
    return s&&isfinite(s->limit)&&
        (s->limit==0||(s->limit>SH_BOUNDS_NATIVE&&s->limit<=SH_BOUNDS_CEILING));
}
float sh_map_bounds_clamp(float requested)
{
    if(!isfinite(requested))return 0;
    requested=floorf(requested);
    if(requested<=SH_BOUNDS_NATIVE)return 0;
    return requested>SH_BOUNDS_CEILING?SH_BOUNDS_CEILING:requested;
}
int sh_map_bounds_decode(const char *text,sh_map_bounds *out)
{
    sh_map_bounds s;int end=0;
    if(!text||!out||strlen(text)>32||strncmp(text,"1;",2))return 0;
    if(sscanf_s(text+2,"%f%n",&s.limit,&end)!=1||!end||text[2+end])return 0;
    /* Older or hand-edited values outside the range keep their intent:
     * anything at or below native is native, anything above clamps. */
    if(!isfinite(s.limit)||s.limit<0)return 0;
    s.limit=sh_map_bounds_clamp(s.limit);
    *out=s;return 1;
}
int sh_map_bounds_encode(const sh_map_bounds *s,char *out,size_t cap)
{
    int n;
    if(!out||!cap||!sh_map_bounds_valid(s))return 0;
    n=snprintf(out,cap,"1;%.0f",s->limit);
    return n>0&&(size_t)n<cap;
}
float sh_map_bounds_effective(const sh_map_bounds *s)
{
    return sh_map_bounds_valid(s)&&s->limit>0?s->limit:SH_BOUNDS_NATIVE;
}
int sh_map_bounds_overrides(const sh_map_bounds *s)
{
    return sh_map_bounds_valid(s)&&s->limit>0;
}
int sh_map_bounds_enabled(const sh_map_bounds *s)
{
    return sh_map_bounds_overrides(s)&&s->limit>SH_BOUNDS_NATIVE;
}
void sh_map_bounds_from_toggle(int on,sh_map_bounds *s)
{
    if(s)s->limit=on?SH_BOUNDS_CEILING:0;
}
int sh_map_bounds_floors(float limit)
{
    float scaled;
    if(!isfinite(limit)||limit<=SH_BOUNDS_NATIVE)return SH_BOUNDS_NATIVE_FLOORS;
    if(limit>SH_BOUNDS_CEILING)limit=SH_BOUNDS_CEILING;
    scaled=ceilf(SH_BOUNDS_NATIVE_FLOORS*limit/SH_BOUNDS_NATIVE);
    return (int)scaled;
}

#ifndef SH_MAP_BOUNDS_PURE
#include "engine_build.h"
#include "engine_cvar_read.h"
#include "engine_globals.h"
#include "editor_zoom.h"
#include "process_heap_scope.h"
#include "settings_tab.h"
#include <windows.h>

void backend_log(const char *message);

/* idCVar float value. ReconnectPortals (VK 0x11f95d0) reads the module bound
 * at object 0x145c3e5d0 + 0x34; GL reads 0x144543c50 + 0x34. The integer
 * value is SH_IDCVAR_VALUE_INT (+0x30): the Blueprint floor clamps read
 * VK 0x145c3e420 / 0x145c3e4c0 = objects 0x145c3e3f0 / 0x145c3e490 + 0x30. */
#define SH_BOUNDS_IDCVAR_VALUE_FLOAT 0x34u

typedef unsigned char (*resize_fn)(void *,int);
typedef void (*assign_fn)(void *,const char *);
/* idCVar::Set(cvar, text, force): VK 0x295780 / GL 0x2959a0. force skips the
 * read-only and developer gates; the value is parsed into the int and float
 * fields and the modified flag is raised. */
typedef unsigned char (*cvar_set_fn)(void *,const char *,unsigned char);

static resize_fn g_resize;
static assign_fn g_assign;
static cvar_set_fn g_set;
static sh_process_heap_api g_heap;
static const uint8_t *g_base;
static int g_installed;

enum {CV_MODULE,CV_CAMERA,CV_FLOOR_MIN,CV_FLOOR_MAX,CV_COUNT};
static const char *const cvar_names[CV_COUNT]={
    "snapEdit_environmentModuleBounds","snapEdit_cameraDistanceLimit",
    "snapEdit_minBlueprintFloor","snapEdit_maxBlueprintFloor"};
static void *g_cvars[CV_COUNT];
static char g_native[CV_COUNT][32];
static int g_captured;
static float g_active=SH_BOUNDS_NATIVE;

static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}

/* idSnapVariables string rows: 104 bytes, name idStr +0 (length +8, data
 * +16), initialValue idStr +56. Same layout as map_render_native.c. */
static unsigned char *variable(unsigned char *list,int *index,int *count)
{
    unsigned char *rows;
    int i,n=*(int*)(list+8),cap=*(int*)(list+12),found=-1;
    if(n<0||n>4096||cap<n||cap>65536)return NULL;
    rows=*(unsigned char**)list;if(n&&!rows)return NULL;
    for(i=0;i<n;i++) {
        unsigned char *row=rows+(size_t)i*104;
        int len=*(int*)(row+8);const char *name=*(const char**)(row+16);
        if(len==(int)strlen(SH_BOUNDS_VARIABLE)&&name&&!memcmp(name,SH_BOUNDS_VARIABLE,(size_t)len)) {
            if(found>=0)return NULL;found=i;
        }
    }
    *index=found;*count=n;return list;
}
static int read_list(unsigned char *source,sh_map_bounds *settings)
{
    int index,count,len;unsigned char *list,*row;const char *text;char copy[33];
    if(!source||!settings)return 0;
    __try {
        list=variable(source,&index,&count);if(!list)return 0;
        if(index<0){sh_map_bounds_default(settings);return 1;}
        row=*(unsigned char**)list+(size_t)index*104+56;
        len=*(int*)(row+8);text=*(const char**)(row+16);
        if(len<1||len>32||!text)return 0;
        memcpy(copy,text,(size_t)len);copy[len]=0;
        return sh_map_bounds_decode(copy,settings);
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
int sh_map_bounds_read(void *map,sh_map_bounds *settings)
{
    return map&&read_list((unsigned char*)map+0xa8,settings);
}
int sh_map_bounds_capture(void *snapshot,sh_map_bounds *settings)
{
    return snapshot&&settings&&read_list((unsigned char*)snapshot+0x1a0,settings);
}
int sh_map_bounds_write(void *map,const sh_map_bounds *settings)
{
    char text[33];int index,count,ok=0;unsigned char *list,*row;
    sh_process_heap_scope scope={0};
    if(!g_installed||!map||!sh_map_bounds_encode(settings,text,sizeof text))return 0;
    __try {
        __try {
            if(!sh_process_heap_enter(&g_heap,&scope))__leave;
            list=variable((unsigned char*)map+0xa8,&index,&count);if(!list)__leave;
            if(index<0) {
                /* A native map stays free of the variable until an author
                 * actually unlocks the space. */
                if(!sh_map_bounds_overrides(settings)){ok=1;__leave;}
                if(count==4096)__leave;
                if(count==*(int*)(list+12)&&!g_resize(list,count+1))__leave;
                row=*(unsigned char**)list+(size_t)count*104;
                g_assign(row,SH_BOUNDS_VARIABLE);
                g_assign(row+56,text);
                *(void**)(row+48)=NULL;
                *(int*)(list+8)=count+1;
                if(*(int*)((unsigned char*)map+0x5c8)<count+1)
                    *(int*)((unsigned char*)map+0x5c8)=count+1;
            } else {
                /* Keep the row (variable indices must not shift); native is "1;0". */
                row=*(unsigned char**)list+(size_t)index*104;
                if(*(int*)(row+64)==(int)strlen(text)&&*(const char**)(row+72)&&
                   !memcmp(*(const char**)(row+72),text,strlen(text))){ok=1;__leave;}
                g_assign(row+56,text);
            }
            {sh_map_bounds check;ok=sh_map_bounds_read(map,&check)&&check.limit==settings->limit;}
        } __finally {if(!sh_process_heap_leave(&scope))ok=0;}
    } __except(EXCEPTION_EXECUTE_HANDLER){ok=0;}
    backend_log(ok?"BOUNDS: map bounds setting stored in native map variables":
                   "BOUNDS: native map bounds write failed");
    return ok;
}

static int resolve_cvars(void)
{
    void *slot;int i;
    if(g_cvars[0]&&g_cvars[1]&&g_cvars[2]&&g_cvars[3])return 1;
    slot=(void*)glb_resolve(g_base,"cvar_system_slot",NULL);
    if(!slot)return 0;
    for(i=0;i<CV_COUNT;i++)
        if(!g_cvars[i]&&!(g_cvars[i]=sh_engine_cvar_find(slot,SH_IDCVAR_NAME_FOR_LOOKUP,cvar_names[i])))return 0;
    return 1;
}
static int read_value(int which,char *out,size_t cap)
{
    __try {
        const unsigned char *v=(const unsigned char*)g_cvars[which];
        if(which==CV_MODULE||which==CV_CAMERA) {
            float f=*(const float*)(v+SH_BOUNDS_IDCVAR_VALUE_FLOAT);
            if(!isfinite(f)||f<=0)return 0;
            return snprintf(out,cap,"%.9g",f)>0;
        }
        return snprintf(out,cap,"%d",*(const int*)(v+SH_IDCVAR_VALUE_INT))>0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static int set_value(int which,const char *text)
{
    __try {return g_set(g_cvars[which],text,1)!=0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
void sh_map_bounds_select(const sh_map_bounds *settings)
{
    char values[CV_COUNT][32];float limit=sh_map_bounds_effective(settings);int i,ok=1,floors;
    if(!g_installed||!resolve_cvars()) {
        if(settings&&sh_map_bounds_overrides(settings))
            backend_log("BOUNDS: map requests unlocked bounds but the engine cvars are unavailable");
        return;
    }
    if(!sh_map_bounds_overrides(settings)) {
        if(g_captured) {
            for(i=0;i<CV_COUNT;i++)ok&=set_value(i,g_native[i]);
            g_captured=0;
            backend_log(ok?"BOUNDS: native map bounds restored":"BOUNDS: native map bounds restore failed");
        }
        g_active=SH_BOUNDS_NATIVE;
        sh_editor_zoom_update(g_active);
        return;
    }
    if(!g_captured) {
        for(i=0;i<CV_COUNT;i++)if(!read_value(i,g_native[i],sizeof g_native[i]))return;
        g_captured=1;
    }
    /* The camera follows the space it may need to reach; never below native. */
    floors=sh_map_bounds_floors(limit);
    snprintf(values[CV_MODULE],sizeof values[0],"%.0f",limit);
    snprintf(values[CV_CAMERA],sizeof values[0],"%.0f",limit);
    snprintf(values[CV_FLOOR_MIN],sizeof values[0],"%d",-floors);
    snprintf(values[CV_FLOOR_MAX],sizeof values[0],"%d",floors);
    for(i=0;i<CV_COUNT;i++)ok&=set_value(i,values[i]);
    g_active=limit;
    sh_editor_zoom_update(limit);
    {
        char line[160];
        snprintf(line,sizeof line,"BOUNDS: Snapmap+ map bounds %s at %.0f units (floors +/-%d)",
            ok?"selected":"partially selected",limit,floors);
        backend_log(line);
    }
}
void sh_map_bounds_build(void *map)
{
    sh_map_bounds settings;
    if(sh_map_bounds_read(map,&settings))sh_map_bounds_select(&settings);
}
void sh_map_bounds_loaded(void *snapshot)
{
    sh_map_bounds settings;
    sh_map_bounds_select(snapshot&&sh_map_bounds_capture(snapshot,&settings)?&settings:NULL);
}

int sh_map_bounds_install(const sig_result *results,size_t count,const uint8_t *base)
{
    const sig_result *resize,*assign,*set;
    if(g_installed)return 1;
    resize=binding(results,count,"RenderVariablesResize");
    assign=binding(results,count,"IdStrAssignCStr");
    set=binding(results,count,"CvarSetString");
    if(!resize||!assign||!set||!base||!sh_process_heap_bind(&g_heap,results,count,base)) {
        backend_log("BOUNDS: map bounds unavailable on this build");return 0;
    }
    g_resize=(resize_fn)resize->addr;g_assign=(assign_fn)assign->addr;g_set=(cvar_set_fn)set->addr;
    g_base=base;g_installed=1;
    backend_log("BOUNDS: per-map bounds installed");
    return 1;
}

/* Settings tab group: one on/off row. On stores the ceiling, off stores
 * native ("1;0"); a map saved with any value above native reads as on and
 * keeps its stored value until the row is changed and applied. */
static int g_draft,g_saved;
static void *g_map;
static int bounds_begin(void *user,void *editor,void *map)
{
    sh_map_bounds s;(void)user;(void)editor;
    if(!g_installed||!sh_map_bounds_read(map,&s))return 0;
    g_map=map;g_saved=g_draft=sh_map_bounds_enabled(&s);
    return 1;
}
static void bounds_populate(sh_settings_rows *rows,void *user)
{
    static const sh_settings_bool row={"Unlock Map Bounds","#str_smp_bounds_limit","#str_smp_bounds_limit_help"};
    (void)user;
    if(!sh_settings_tab_add_bool(rows,&row,&g_draft,&g_saved))
        backend_log("BOUNDS: Unlock Map Bounds row unavailable (toggle row creator missing)");
}
static int bounds_commit(void *user,void *map)
{
    sh_map_bounds s;(void)user;
    if(!map||map!=g_map)return 0;
    sh_map_bounds_from_toggle(g_draft,&s);
    if(!sh_map_bounds_write(map,&s))return 0;
    /* The editor's own ReconnectPortals pass reads the cvar every frame. */
    sh_map_bounds_select(&s);
    return 1;
}
static void bounds_end(void *user){(void)user;g_map=NULL;}
int sh_map_bounds_settings_register(void)
{
    sh_settings_group group={"Map Bounds",bounds_begin,bounds_populate,bounds_commit,bounds_end,NULL};
    return sh_settings_tab_register_group(&group);
}
#endif
