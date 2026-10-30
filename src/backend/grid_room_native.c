/* Private native palettes keep dimension variants out of the stock palette's
 * movable arrays. Each cache entry owns a one-module palette for the process. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "grid_room_native.h"
#include "grid_room_asset.h"
#include "grid_room_edit.h"
#include "grid_room_decl.h"
#include "typeinfo.h"
#include <math.h>
#include <stdarg.h>
#include "engine_globals.h"
#include "overrides.h"
#include "process_heap_scope.h"
#include "hook.h"
#include "grid_room_areas.h"
#include "engine_build.h"
#include "engine_cvar_read.h"

void backend_log(const char *message);
typedef void *(*find_module_fn)(void *,const char *);
typedef unsigned char (*load_module_fn)(void *,void *,void *,void *,void *);
typedef void *(*str_ctor_fn)(void *,const char *);
typedef void (*str_dtor_fn)(void *);
typedef void *(*type_fn)(void *,const char *);
typedef unsigned char (*register_fn)(void *,const void *,void *);
typedef void *(*source_fn)(void *,const char *);
typedef void *(*find_decl_fn)(void *,const char *,unsigned char);
typedef void (*generic_load_fn)(void *);
typedef void (*replace_instance_fn)(void *,unsigned,const void *);
typedef void (*reconnect_fn)(void *);
typedef void *(*edit_entity_fn)(void *,int);
typedef void (*set_transform_fn)(void *,const float *);
typedef float *(*portal_bounds_fn)(void *,float *,int);
typedef float *(*world_bounds_fn)(void *,float *,const float *);
typedef unsigned char (*build_collision_fn)(void *,void *,int);
typedef void (*refresh_surfaces_fn)(void *,void *,void *);
typedef int (*containing_module_fn)(void *,const float *);
typedef struct grid_ray_hit {
    float point[3],normal[3];
    int module,unused;
    float fraction;
} grid_ray_hit;
typedef grid_ray_hit *(*module_ray_fn)(void *,grid_ray_hit *,const float *,const float *,int,void *,int);
typedef void **(*entity_tree_fn)(void *,void **);
typedef void (*entity_tree_apply_fn)(void *,void *);
typedef unsigned char (*tree_vec_fn)(void *,const char *,const float *,void *);
typedef unsigned char (*tree_float_fn)(void *,const char *,float,void *);
typedef void (*tree_destroy_fn)(void *);
typedef void (*tree_free_fn)(void *,size_t);
typedef union native_string {void *align;unsigned char bytes[48];} native_string;
typedef struct grid_native_entry {
    sh_grid_size size; /* kind 0 for a bundle module; xyz[2] its height */
    int state; /* 1 constructing, 2 ready, -1 retained failed native allocation. */
    void *record;
    union {void *align;unsigned char bytes[128];} palette;
    char module[160]; /* a bundle module's decl name, else empty */
} grid_native_entry;
#define GRID_NATIVE_CAP 64
static grid_native_entry g_entries[GRID_NATIVE_CAP];
static unsigned g_count;
static LONG g_busy,g_faulted;
static LONG g_edit_busy,g_edit_faulted;
static unsigned char *g_editor;
static const unsigned char *g_anchor;
static find_module_fn g_find;
static void *g_has_hook;
static load_module_fn g_load;
static str_ctor_fn g_ctor;
static str_dtor_fn g_dtor;
static type_fn g_type;
static register_fn g_register;
static source_fn g_source;
static find_decl_fn g_decl;
static generic_load_fn g_generic_load;
static replace_instance_fn g_replace;
static reconnect_fn g_reconnect;
static edit_entity_fn g_edit_entity;
static set_transform_fn g_set_transform;
static portal_bounds_fn g_portal_bounds;
static world_bounds_fn g_world_bounds;
static build_collision_fn g_build_collision;
static refresh_surfaces_fn g_refresh_surfaces;
static containing_module_fn g_containing_module;
static module_ray_fn g_module_ray;
static const float *g_environment_bounds;
static const void *g_cvar_slot;
static void *g_portal_cvar;
static entity_tree_fn g_entity_tree;
static entity_tree_apply_fn g_apply_tree,g_read_properties;
static tree_vec_fn g_tree_vec;
static tree_float_fn g_tree_float;
static tree_destroy_fn g_tree_destroy;
static tree_free_fn g_tree_free;
static sh_process_heap_api g_heap;
static sh_grid_module_served_fn g_module_served;

static int grid_containing_module(void *collision,const float *point)
{
    int result=g_containing_module(collision,point),i,n,k;unsigned j;
    unsigned char *map,*records,*record,*wrapper,*module;
    float bounds[6],end[3],reach;grid_ray_hit up={0},down={0};
    if(result!=-1||!g_editor||collision!=g_editor+0x20550||!g_editor[8])return result;
    for(k=0;k<3;++k)if(!isfinite(point[k]))return result;
    map=*(unsigned char**)(g_editor+0x204c8);if(!map)return result;
    n=*(int*)(map+0x758);records=*(unsigned char**)(map+0x750);
    if(n<1||n>4096||!records)return result;
    for(i=0;i<n;++i){
        record=records+(size_t)i*0x98;if(!record[0x30])continue;
        wrapper=*(unsigned char**)record;
        for(j=0;j<g_count;++j)if(g_entries[j].state==2&&
            *(void**)g_entries[j].record==wrapper)break;
        if(j==g_count||g_entries[j].size.xyz[2]<=6000)continue;
        module=*(unsigned char**)wrapper;if(!module)continue;
        g_world_bounds(record,bounds,(const float*)(module+0xa0));
        for(k=0;k<3;++k)if(!isfinite(bounds[k])||!isfinite(bounds[k+3])||
            point[k]<bounds[k]||point[k]>bounds[k+3])break;
        if(k!=3)continue;
        /* Native containment requires opposing hits on the same room, but its
         * fixed 6000-unit rays cannot reach a tall room's ceiling or floor.
         * Keep that collision test and extend it only for an admitted variant. */
        reach=fmaxf(6000,bounds[5]-bounds[2]+16);
        memcpy(end,point,sizeof end);end[2]+=reach;
        g_module_ray(collision,&up,point,end,-1,*(void**)collision,*(int*)((char*)collision+8));
        if(up.module!=i)continue;
        end[2]=point[2]-reach;
        g_module_ray(collision,&down,point,end,-1,*(void**)collision,*(int*)((char*)collision+8));
        if(down.module==i)return i;
    }
    return result;
}

static unsigned char grid_build_collision(void *collision,void *map,int instance)
{
    unsigned char *m=map,*wrapper=NULL,result;unsigned i;int own=0;
    sh_process_heap_scope heap={0};
    if(instance>=0&&instance<*(int*)(m+0x758)){
        wrapper=*(unsigned char**)(*(unsigned char**)(m+0x750)+(size_t)instance*0x98);
        for(i=0;i<g_count;++i)if(g_entries[i].state==2&&
            *(void**)g_entries[i].record==wrapper){own=1;break;}
    }
    if(!own)return g_build_collision(collision,map,instance);
    /* These two resources are cached by a process-lifetime private palette.
     * Build their native allocations on that same heap and retain only this
     * palette's collision resources through the map-transition purge. */
    if(!sh_process_heap_enter(&g_heap,&heap))return 0;
    __try {
        result=g_build_collision(collision,map,instance);
        for(i=0;i<2;++i){unsigned char *resource=*(unsigned char**)(wrapper+0x68+8*i);
            if(resource&&*(unsigned*)(resource+0x28)!=4){
                *(unsigned*)(resource+0x28)=4;
                backend_log("GRID: private collision resource retained across Play");
            }
        }
    } __finally {sh_process_heap_leave(&heap);}
    return result;
}

static void *clean(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;++i)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)
        return (void*)r[i].addr;return NULL;
}
static unsigned char *read_asset(void *context,const char *name,size_t *length)
{(void)context;return sh_overrides_read_engine_resource(name,length);}
static void release_asset(void *context,void *bytes)
{(void)context;HeapFree(GetProcessHeap(),0,bytes);}
static int preflight(const char *name)
{
    unsigned char *bytes=NULL;size_t length=0;
    int ok=sh_grid_asset_open(name,read_asset,release_asset,NULL,&bytes,&length)==1;
    free(bytes);return ok;
}
static void *registry(void)
{
    int32_t displacement;void *r;void **vt;
    if(!g_anchor||memcmp(g_anchor+0x10,"\x48\x8b\x0d",3))return NULL;
    memcpy(&displacement,g_anchor+0x13,4);
    r=*(void**)(g_anchor+0x17+displacement);if(!r)return NULL;
    vt=*(void***)r;
    if(!vt||vt[7]!=(void*)g_register||vt[11]!=(void*)g_type)return NULL;
    return r;
}
static int catalog(const char *logical,int grid)
{
    void *r=registry(),*manager,*decl;native_string source;char path[160];unsigned char state;
    if(!r){backend_log("GRID: declaration registry unavailable");return 0;}
    if((manager=g_type(r,"snapModuleInfo"))==NULL){backend_log("GRID: module catalog type unavailable");return 0;}
    if(!g_source(manager,logical)){
        unsigned char registered;
        snprintf(path,sizeof path,"snapmoduleinfo/%s.decl",logical);
        g_ctor(&source,path);registered=g_register(r,&source,NULL);g_dtor(&source);
        if(!registered||!g_source(manager,logical)){backend_log("GRID: module catalog source registration failed");return 0;}
    }
    /* Runtime source registration does not guarantee a live object after the
     * engine switches to its compiled-resource existence probe. The provider
     * preflight and registered source authorize materializing this identity. */
    decl=g_decl(manager,logical,0);if(!decl)decl=g_decl(manager,logical,1);
    if(!decl){backend_log("GRID: module catalog lookup failed");return 0;}
    state=*((unsigned char*)decl+0x2c);
    /* Create-if-missing can leave an empty default object at runtime. This
     * identity belongs to a new private palette with no consumers yet, so it
     * can safely take the native generic source load before admission. Never
     * tear down an accepted catalog or a stock room's catalog here. */
    if(!(state&1)&&*(int*)((unsigned char*)decl+0x118)==0&&
       *(int*)((unsigned char*)decl+0x130)==0){
        *(unsigned int*)((unsigned char*)decl+0x28)=4;
        *((unsigned char*)decl+0x2c)=(unsigned char)(state&~2);
        g_generic_load(decl);
        state=*((unsigned char*)decl+0x2c);
        backend_log("GRID: private module catalog source load completed");
    }
    if(state&3){
        char message[128];snprintf(message,sizeof message,"GRID: module catalog state 0x%02x is not ready",state);
        backend_log(message);return 0;}
    if(grid&&(*(int*)((unsigned char*)decl+0x118)!=1||*(int*)((unsigned char*)decl+0x130)!=4)){
        char message[160];snprintf(message,sizeof message,
            "GRID: module catalog has %d floors and %d walls (state 0x%02x); variant refused",
            *(int*)((unsigned char*)decl+0x118),*(int*)((unsigned char*)decl+0x130),state);
        backend_log(message);return 0;}
    *(unsigned int*)((unsigned char*)decl+0x28)=4;
    return 1;
}
static void *end_record(void *palette)
{
    unsigned char *p=(unsigned char*)palette;int n=*(int*)(p+0x18);
    return *(unsigned char**)(p+0x10)+(size_t)n*0x98;
}
/* Native FindModule (13954591 VK 0x1212D20) is a lower_bound over the whole
 * record list followed by one exact comparison, so it assumes the list is in
 * name order. The Blueprint Add tool sorts that same list in place by theme,
 * portal type and its ribbon mode, and the order persists until a map load
 * sorts it by name again. Find this adapter's own records by exact name over
 * every record instead, so the editor's sort state can never hide a room. */
static void *palette_find(void *palette,const char *name)
{
    unsigned char *p=(unsigned char*)palette,*records,*record,*wrapper,*decl;const char *key;int n,i;
    __try {
        records=*(unsigned char**)(p+0x10);n=*(int*)(p+0x18);
        if(!records||n<0||n>100000)return end_record(palette);
        for(i=0;i<n;++i){record=records+(size_t)i*0x98;
            if((wrapper=*(unsigned char**)record)&&(decl=*(unsigned char**)wrapper)&&
               (key=*(const char**)(decl+8))&&!strcmp(key,name))return record;}
    } __except(EXCEPTION_EXECUTE_HANDLER){}
    return end_record(palette);
}
/* The internal step at which the last variant lookup returned nothing. */
static const char *g_step;
#define REFUSE(step) do{g_step=(step);return NULL;}while(0)
static void log_census(const char *label,const unsigned char *wrapper);
/* Load one module into a private one-module palette with the editor palette's
 * own loader: the native LoadSnapModule path (VK 0x141213860 / GL 0x141212c70)
 * names the render model maps/modules/palettes/<palette_name>/<decl path>.bmodel
 * and finds the snapModuleInfo by the decl path. `grid` 1 for a Grid Room
 * variant (its catalog must have one floor and four walls), 0 for a bundle
 * module named `module`. Engine thread. */
static void *load_private(void *palette,const sh_grid_size *size,const char *module,
    unsigned char *stock_wrapper,const char *palette_name,const char *variant,int grid)
{
    grid_native_entry *entry;unsigned char *p;
    native_string palette_string,module_string;void *loaded,*result=NULL;
    sh_process_heap_scope heap={0};int strings=0;
    if(!sh_process_heap_enter(&g_heap,&heap))REFUSE("heap scope: not on the engine thread");
    entry=&g_entries[g_count++];memset(entry,0,sizeof *entry);entry->size=*size;entry->state=1;
    if(module)strncpy_s(entry->module,sizeof entry->module,module,_TRUNCATE);
    __try {
        char logical[160];size_t k=strlen(variant+13)-5;
        if(k>=sizeof logical){g_step="module name too long";__leave;}
        memcpy(logical,variant+13,k);logical[k]=0;
        g_step="module catalog declaration (see the previous GRID line)";
        if(!catalog(logical,grid))__leave;
        p=entry->palette.bytes;
        /* Same empty list construction as the editor's module palette. The
         * private palette never enters a stock array or a stock destructor. */
        *(void**)p=*(void**)palette;
        *(unsigned*)(p+0x20)=*(unsigned*)(p+0x38)=
            *(unsigned*)(p+0x60)=*(unsigned*)(p+0x78)=0x50000;
        g_step="native module loader";
        g_ctor(&palette_string,palette_name);strings=1;
        g_ctor(&module_string,variant);strings=2;
        if(!g_load(p,*(void**)(stock_wrapper+0x90),&palette_string,&module_string,g_editor+0x20660)){
            backend_log("GRID: native module loader rejected the variant");__leave;}
        if(*(int*)(p+0x18)!=1||*(int*)(p+0x70)!=1){backend_log("GRID: private module palette has unexpected counts");__leave;}
        g_step="private palette: the loaded record has no wrapper";
        result=*(void**)(p+0x10);if(!result||!*(void**)result)__leave;
        loaded=**(void***)result;
        if(!loaded||!*(void**)((unsigned char*)*(void**)result+0x80)||
           !*(void**)((unsigned char*)*(void**)result+0x88)) {
            g_step="private palette: the loaded wrapper lacks its declaration, card or templates";result=NULL;__leave;}
        *(unsigned*)((unsigned char*)loaded+0x28)=4;
        if(!grid){
            /* The local bounds (module +0xa0) give the containment fallback its height. */
            const float *b=(const float*)((unsigned char*)loaded+0xa0);
            float h=b[5]-b[2];
            entry->size.xyz[2]=isfinite(h)&&h>0&&h<65536?(unsigned)h:0;
        }
    } __finally {
        __try {
            __try {if(strings>=2)g_dtor(&module_string);}
            __finally {if(strings>=1)g_dtor(&palette_string);}
        } __finally {if(!sh_process_heap_leave(&heap)){result=NULL;g_step="heap scope restore";}}
    }
    entry->record=result;entry->state=result?2:-1;
    if(!grid){char message[300];
        /* A refusal is logged once by find_variant, with g_step. */
        if(!result)return NULL;
        snprintf(message,sizeof message,"MODS: module %s loaded into a private palette (%s)",module,palette_name);
        backend_log(message);
        log_census("module",*(unsigned char**)result);
        return result;
    }
    backend_log(result?"GRID: independent native module variant loaded":"GRID: native module variant refused");
    if(result){log_census("variant",*(unsigned char**)result);log_census("stock",stock_wrapper);}
    return result;
}
static void *create(void *palette,const sh_grid_size *size)
{
    unsigned i;unsigned char *stock_record,*stock_wrapper;
    const char *render,*slash;char stock[128],variant[128],info[160],palette_name[128];
    size_t len;
    for(i=0;i<g_count;++i)if(g_entries[i].size.kind==size->kind&&
        !memcmp(g_entries[i].size.xyz,size->xyz,sizeof size->xyz)){
        if(g_entries[i].state==2)return g_entries[i].record;
        REFUSE("variant cache: an earlier construction of this size failed in this session");}
    /* Reload admission runs before the editor becomes active. The palette and
     * stock wrapper, plus the main-thread heap scope, establish readiness. */
    if(g_count==GRID_NATIVE_CAP)REFUSE("variant cache full");
    if(palette!=g_editor+0x206c0)REFUSE("lookup is not on the editor module palette");
    snprintf(stock,sizeof stock,"%s.decl",sh_grid_stock_name(size->kind));
    stock_record=(unsigned char*)palette_find(palette,stock);
    if(!stock_record||stock_record==end_record(palette))REFUSE("stock template lookup: the stock room is not in the module palette");
    stock_wrapper=*(unsigned char**)stock_record;if(!stock_wrapper)REFUSE("stock template lookup: the stock record has no wrapper");
    render=*(const char**)(stock_wrapper+0x18);
    if(!render||strncmp(render,"maps/modules/palettes/",22))REFUSE("stock template: unexpected render model path");
    slash=strchr(render+22,'/');if(!slash)REFUSE("stock template: unexpected render model path");
    len=(size_t)(slash-render-22);if(!len||len>=sizeof palette_name)REFUSE("stock template: unexpected palette name");
    memcpy(palette_name,render+22,len);palette_name[len]=0;
    if(!sh_grid_name(size,variant,sizeof variant))REFUSE("variant name");
    snprintf(info,sizeof info,"decltree/snapmoduleinfo/%s.decl",variant+13);
    strcat_s(variant,sizeof variant,".decl");
    if(!preflight(variant))REFUSE("resource preflight: module declaration");
    if(!preflight(info))REFUSE("resource preflight: module catalog declaration");
    {char collision[180],identity[112];const char *leaf;
        if(!sh_grid_name(size,identity,sizeof identity))REFUSE("variant name");
        leaf=strrchr(identity,'/')+1;
        snprintf(collision,sizeof collision,"%s/_combo/world.bcm",identity);
        if(!preflight(collision))REFUSE("resource preflight: world collision");
        snprintf(collision,sizeof collision,"%s/%s_floor_collision.bcm",identity,leaf);
        if(!preflight(collision))REFUSE("resource preflight: floor collision");
    }
    {char nav[180],identity[112];const char *leaf;unsigned agents[]={48,96,128};
        if(!sh_grid_name(size,identity,sizeof identity))REFUSE("variant name");
        leaf=strrchr(identity,'/')+1;
        for(i=0;i<3;++i){snprintf(nav,sizeof nav,"generated/%s/%s.baas_monster%u",identity,leaf,agents[i]);
            if(!preflight(nav)){backend_log("GRID: resized navigation preflight failed");REFUSE("resource preflight: navigation");}}
    }
    return load_private(palette,size,NULL,stock_wrapper,palette_name,variant,1);
}
void sh_grid_native_set_module_source(sh_grid_module_served_fn served){g_module_served=served;}
int sh_grid_module_logical(const char *name,char *out,size_t cap)
{
    size_t n=name?strlen(name):0;
    if(n<19||_strnicmp(name,"maps/modules/",13)||_strnicmp(name,"maps/modules/palettes/",22)==0||
       _stricmp(name+n-5,".decl")||n-18+1>cap)return 0;
    memcpy(out,name+13,n-18);out[n-18]=0;
    return 1;
}
/* A bundle module: a snapModule decl a mod bundle serves (modserve.c). It takes
 * the Grid Room variant route: a private one-module palette loaded with the
 * native loader, never an entry in the stock palette's arrays, retained for the
 * process. The stock palette's first record lends the loader its context
 * (wrapper +0x90) and its palette group: every stock module's render model is
 * under maps/modules/palettes/mega_blessed/, so a bundle module's is
 * maps/modules/palettes/mega_blessed/<decl path without maps/modules/>.bmodel. */
static void *create_bundle(void *palette,const char *name)
{
    unsigned i;unsigned char *records,*stock_wrapper=NULL;int n,k;
    const char *render=NULL,*slash;char info[220],logical[160],palette_name[128],request[260];
    sh_grid_size none;size_t len;
    for(i=0;i<g_count;++i)if(!g_entries[i].size.kind&&!_stricmp(g_entries[i].module,name)){
        if(g_entries[i].state==2)return g_entries[i].record;
        REFUSE("module cache: an earlier construction of this module failed in this session");}
    if(g_count==GRID_NATIVE_CAP)REFUSE("variant cache full");
    if(palette!=g_editor+0x206c0)REFUSE("lookup is not on the editor module palette");
    if(strlen(name)>=sizeof g_entries[0].module||!sh_grid_module_logical(name,logical,sizeof logical))REFUSE("module name");
    records=*(unsigned char**)((unsigned char*)palette+0x10);n=*(int*)((unsigned char*)palette+0x18);
    if(!records||n<1||n>100000)REFUSE("stock template lookup: the module palette is empty");
    for(k=0;k<n&&!render;++k){unsigned char *w=*(unsigned char**)(records+(size_t)k*0x98);const char *r;
        if(w&&(r=*(const char**)(w+0x18))!=NULL&&!strncmp(r,"maps/modules/palettes/",22)&&strchr(r+22,'/')){stock_wrapper=w;render=r;}}
    if(!render)REFUSE("stock template: no stock module has a palette render model");
    slash=strchr(render+22,'/');len=(size_t)(slash-render-22);
    if(!len||len>=sizeof palette_name)REFUSE("stock template: unexpected palette name");
    memcpy(palette_name,render+22,len);palette_name[len]=0;
    /* Bundle files are checked in the serving index, never read here: the
     * Grid preflight reads through sh_grid_asset_open, which claims only
     * Grid Room names, and a multiplayer map's collision alone is 48 MB. */
    if(!g_module_served)REFUSE("no serving index is bound");
    snprintf(info,sizeof info,"generated/decls/snapmoduleinfo/%s.decl",logical);
    if(g_module_served(info,0)!=1)REFUSE("its snapModuleInfo is not served");
    snprintf(request,sizeof request,"maps/modules/%s/_combo/world.bcm",logical);
    if(g_module_served(request,0)!=1)REFUSE("world collision is not served");
    memset(&none,0,sizeof none);
    return load_private(palette,&none,name,stock_wrapper,palette_name,name,0);
}
static void *find_variant(void *palette,const char *name)
{
    sh_grid_size size;void *record=NULL;int bundle=0;const char *step;
    if(!sh_grid_parse_name(name,&size)){
        bundle=g_module_served&&name&&g_module_served(name,1)==1;
        if(!bundle)return g_find(palette,name);
    }
    if(InterlockedCompareExchange(&g_faulted,0,0)){g_step="variant creation disabled after an earlier fault";return end_record(palette);}
    if(InterlockedCompareExchange(&g_busy,1,0)){g_step="another variant construction is in progress";return end_record(palette);}
    g_step=NULL;
    __try {record=bundle?create_bundle(palette,name):create(palette,&size);}
    __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);g_step="variant construction faulted";
        backend_log("GRID: native variant creation faulted; disabled");}
    InterlockedExchange(&g_busy,0);
    if(bundle&&!record){
        /* One line per module and cause: the membership probe and the lookup
         * ask the same name several times during one map load. */
        static char last[400];char line[400];
        step=g_step?g_step:"the native module loader";
        snprintf(line,sizeof line,"MODS: module %s refused: %s",name,step);
        if(strcmp(line,last)){strcpy_s(last,sizeof last,line);backend_log(line);}
    }
    return record?record:end_record(palette);
}
static unsigned char has_variant(void *palette,const char *name)
{
    /* The native membership probe uses a separate binary search and therefore
     * bypasses FindModule. Use the same lookup for both admission and copying;
     * otherwise reload removes the serialized instance before conversion. */
    return (unsigned char)(find_variant(palette,name)!=end_record(palette));
}
/* Module entity templates (13954591, VK FUN_14120e790): wrapper+0x88 owns four
 * template lists (+0x00/+0x18/+0x30/+0x48; stride 0x6f0, definition at +0x150)
 * keyed by the editor entity palette category (entry +0x3c8 = 0..3), a list of
 * module entities whose inherit has no editor palette entry (+0x60, count
 * +0x68, stride 0xa8) and a bare counter at +0x78 for category-4 entries, which
 * get no template. The modern room's five static fill lights are category 4
 * (live: 41+0+8+3 templates, 0 palette misses, 255 category 4 = 307 entities).
 * Loading a saved map instantiates only the +0x48 list into each room
 * (VK FUN_141200220). Describe it for live diagnosis. */
static int census(const unsigned char *wrapper,char *out,size_t capacity)
{
    const unsigned char *lists;int counts[5],k,i,hollow=0,lights=0,uninstantiated;size_t used;
    out[0]=0;
    __try {
        if(!wrapper||!(lists=*(const unsigned char**)(wrapper+0x88)))return 0;
        for(k=0;k<5;++k){counts[k]=*(const int*)(lists+0x18*k+8);if(counts[k]<0||counts[k]>100000)return 0;}
        uninstantiated=*(const int*)(lists+0x78);if(uninstantiated<0||uninstantiated>100000)return 0;
        for(i=0;i<counts[3];++i){
            const unsigned char *entry=*(const unsigned char**)(lists+0x48)+(size_t)i*0x6f0;
            const unsigned char *def=*(const unsigned char**)(entry+0x150);
            const char *name=def?*(const char**)(def+8):NULL;
            if(!def)++hollow;
            else if(name&&(strstr(name,"light_")==name))++lights;
        }
        used=(size_t)snprintf(out,capacity,"templates %d/%d/%d/%d, category 4 without template %d, no editor palette entry %d, templates with no definition %d, light templates %d:",
            counts[0],counts[1],counts[2],counts[3],uninstantiated,counts[4],hollow,lights);
        for(i=0;i<counts[3]&&used+40<capacity;++i){
            const unsigned char *entry=*(const unsigned char**)(lists+0x48)+(size_t)i*0x6f0;
            const unsigned char *def=*(const unsigned char**)(entry+0x150);
            const char *name=def?*(const char**)(def+8):NULL;
            if(!def||(name&&(strstr(name,"light_")==name||strstr(name,"speaker_")==name)))
                used+=(size_t)snprintf(out+used,capacity-used," %s",def?(name?name:"?"):"(null)");
        }
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER){out[0]=0;return 0;}
}
static void log_census(const char *label,const unsigned char *wrapper)
{
    char text[480],message[600];
    if(!census(wrapper,text,sizeof text))snprintf(text,sizeof text,"unreadable");
    snprintf(message,sizeof message,"GRID: %s %s",label,text);backend_log(message);
}
int sh_grid_native_templates(void (*print)(const char *line))
{
    unsigned i;char text[480],line[640];unsigned char *stock;
    if(!print||!g_find||!g_editor)return 0;
    {char name[128];snprintf(name,sizeof name,"%s.decl",sh_grid_stock_name(SH_GRID_MODERN));
        stock=(unsigned char*)palette_find(g_editor+0x206c0,name);
        if(stock&&stock!=end_record(g_editor+0x206c0)&&census(*(unsigned char**)stock,text,sizeof text)){
            snprintf(line,sizeof line,"stock modern: %s",text);print(line);}}
    for(i=0;i<g_count;++i){
        if(!g_entries[i].size.kind){
            if(g_entries[i].state!=2||!census(*(unsigned char**)g_entries[i].record,text,sizeof text))
                snprintf(text,sizeof text,"not ready (state %d)",g_entries[i].state);
            snprintf(line,sizeof line,"module %s: %s",g_entries[i].module,text);print(line);continue;}
        if(g_entries[i].state!=2){
            snprintf(line,sizeof line,"variant %c %u x %u x %u: not ready (state %d)",
                g_entries[i].size.kind==SH_GRID_MODERN?'m':'c',g_entries[i].size.xyz[0],
                g_entries[i].size.xyz[1],g_entries[i].size.xyz[2],g_entries[i].state);
            print(line);continue;}
        if(!census(*(unsigned char**)g_entries[i].record,text,sizeof text))snprintf(text,sizeof text,"unreadable");
        snprintf(line,sizeof line,"variant %c %u x %u x %u: %s",g_entries[i].size.kind==SH_GRID_MODERN?'m':'c',
            g_entries[i].size.xyz[0],g_entries[i].size.xyz[1],g_entries[i].size.xyz[2],text);
        print(line);
    }
    return 1;
}
void *sh_grid_native_wrapper(const sh_grid_size *size)
{
    char name[128],message[400];void *record,*wrapper;sh_grid_size stock;
    if(!g_find||!g_editor||!sh_grid_name(size,name,sizeof name)){
        backend_log("GRID: variant lookup failed at binding: the module lookup is not installed or the size has no name");return NULL;}
    g_step=NULL;
    /* Reverting a stock room's dimensions must restore its original identity,
     * including when the native properties panel cancels its pending edits. */
    if(sh_grid_default(size->kind,&stock)&&!memcmp(stock.xyz,size->xyz,sizeof stock.xyz)){
        snprintf(name,sizeof name,"%s.decl",sh_grid_stock_name(size->kind));
        record=palette_find(g_editor+0x206c0,name);
        if(record==end_record(g_editor+0x206c0))g_step="stock template lookup: the stock room is not in the module palette";
    }else{
        strcat_s(name,sizeof name,".decl");record=find_variant(g_editor+0x206c0,name);
    }
    wrapper=record&&record!=end_record(g_editor+0x206c0)?*(void**)record:NULL;
    if(!wrapper){
        snprintf(message,sizeof message,"GRID: variant lookup failed at %s: %s",
            g_step?g_step:"palette record without a wrapper",name);
        backend_log(message);
    }
    return wrapper;
}
int sh_grid_native_read(void *edit_data,int instance,sh_grid_size *size)
{
    unsigned char *map,*records,*wrapper,*module;const char *name;int count;
    if(!g_find||!edit_data||!size||instance<0||InterlockedCompareExchange(&g_faulted,0,0))return 0;
    __try {
        map=*(unsigned char**)edit_data;if(!map)return 0;
        count=*(int*)(map+0x758);if(count<0||count>4096||instance>=count)return 0;
        records=*(unsigned char**)(map+0x750);if(!records)return 0;
        wrapper=*(unsigned char**)(records+(size_t)instance*0x98);if(!wrapper)return 0;
        module=*(unsigned char**)wrapper;if(!module)return 0;
        name=*(const char**)(module+8);if(!name)return 0;
        return sh_grid_parse_name(name,size)||sh_grid_default(sh_grid_stock_kind(name),size);
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}

typedef struct grid_cap_change {int id;float before[12],after[12];} grid_cap_change;
#define GRID_LIGHT_CAP 16
typedef struct grid_light_change {
    int id;float before[12],after[12];void *old_tree,*new_tree;
    sh_grid_builtin_light target;
} grid_light_change;
static void destroy_tree(void **slot)
{
    void *tree=*slot;*slot=NULL;
    if(tree){__try {g_tree_destroy(tree);}__finally {g_tree_free(tree,0x48);}}
}
static int plan_lights(unsigned char *map,int instance,const sh_grid_size *size,
                       grid_light_change changes[GRID_LIGHT_CAP],int *count)
{
    unsigned char **entities=*(unsigned char***)(map+0x6a0),*source=NULL;
    int *keys=*(int**)(map+0x720),*values=*(int**)(map+0x708),start,end,i;size_t length=0,n=0,k;
    sh_grid_builtin_light targets[GRID_LIGHT_CAP];char name[160];void *reflect;int ok=0;
    if(!entities||!keys||!values||*(int*)(map+0x728)<instance+2)return 0;
    start=keys[instance];end=keys[instance+1];
    if(start<0||end<start||end>*(int*)(map+0x710)||end-start>65536)return 0;
    /* Exact original names and inheritance identify built-in lights. Other
     * lights, even ones with identical properties, keep their authored values. */
    snprintf(name,sizeof name,"%s.decl",sh_grid_stock_name(size->kind));
    source=sh_overrides_read_engine_resource(name,&length);
    if(!source||!sh_grid_decl_lights(size,(const char*)source,length,targets,GRID_LIGHT_CAP,&n))goto done;
    for(k=0;k<n;++k){int id=-1;unsigned char *body=NULL;
        for(i=start;i<end;++i){unsigned char *allocation,*def;const char *original,*inherit;int candidate=values[i];
            if(candidate<0||candidate>=*(int*)(map+0x6a8)||!(allocation=entities[candidate]))goto done;
            def=*(unsigned char**)(allocation+0x158);if(!def)continue;
            original=*(const char**)(def+8);inherit=*(const char**)(def+0x58);
            if(!original||!inherit||strcmp(original,targets[k].name)||strcmp(inherit,targets[k].inherit))continue;
            if(id!=-1)goto done; /* Ambiguous ownership must not modify a clone. */
            id=candidate;body=allocation+8;
        }
        if(id!=-1){grid_light_change *c=&changes[(*count)++];
            c->id=id;c->target=targets[k];memcpy(c->before,body+0x288,48);memcpy(c->after,c->before,48);
            memcpy(c->after,c->target.origin,12);
            if(!g_entity_tree||!g_apply_tree||!g_read_properties||!g_tree_vec||!g_tree_float||
               !g_tree_destroy||!g_tree_free||!(reflect=sh_typeinfo_get_reflect()))goto done;
            g_entity_tree(body,&c->old_tree);g_entity_tree(body,&c->new_tree);
            /* Exactly the fields the generated variant declaration writes, so
             * a resized room and the same room after a map load agree. */
            if(!c->old_tree||!c->new_tree||(c->target.has_radius&&!g_tree_vec(c->new_tree,"lightRadius",c->target.radius,reflect))||
               !g_tree_vec(c->new_tree,"lightCenter",c->target.center,reflect))goto done;
            if(c->target.has_visible&&!g_tree_float(c->new_tree,"maxVisibleRange",c->target.visible_range,reflect))goto done;
            if(c->target.has_shadow&&!g_tree_float(c->new_tree,"maxShadowVisibleRange",c->target.shadow_range,reflect))goto done;
        }
    }
    {char message[320];
        /* The editor instantiates only category-3 module templates: the probe
         * and the ceiling light. The modern room's five static fill lights are
         * category-4 palette entries with no template ("category 4 without
         * template" in the census), so they exist only in the room resource and
         * take their scaled values from the generated variant when the map is
         * next loaded or played. A room that owns none of its built-in lights
         * was loaded without its module templates (see "templates" lines). */
        if(*count)snprintf(message,sizeof message,
            "GRID: %d of %u built-in lights are editor entities in this room and were updated; the rest have no editor template (category 4, expected) and follow the room on the next load or Play",
            *count,(unsigned)n);
        else snprintf(message,sizeof message,
            "GRID: the edited room owns none of its %u built-in lights as editor entities; it was loaded without its module templates",
            (unsigned)n);
        backend_log(message);}
    ok=1;
done:if(source)HeapFree(GetProcessHeap(),0,source);return ok;
}
static int apply_light(void *edit_data,grid_light_change *light,int restore)
{
    unsigned char *body=g_edit_entity(edit_data,light->id);void *tree=restore?light->old_tree:light->new_tree;
    int k;if(!body||!tree)return 0;
    g_set_transform(body,restore?light->before:light->after);
    /* GetTypeInfoTree/ApplyTypeInfoTree preserve the complete native definition.
     * ReadProperties refreshes the light's cached render data from that tree. */
    g_apply_tree(body,tree);g_read_properties(body,tree);
    if(!restore)for(k=0;k<3;++k){
        if((light->target.has_radius&&fabsf(*(float*)(body+0xa0+4*k)-light->target.radius[k])>.01f)||
           fabsf(*(float*)(body+0xac+4*k)-light->target.center[k])>.01f){
            char message[200];
            /* The probe and dynamic point readback layout is verified (R-012).
             * Other built-in light kinds are committed through the same tree;
             * a readback difference there is reported, not treated as fatal. */
            if(strcmp(light->target.inherit,"light/probe")&&
               strcmp(light->target.inherit,"snapmaps/light/dynamic_point")){
                snprintf(message,sizeof message,"GRID: built-in light %s committed; cached readback differs",light->target.name);
                backend_log(message);return 1;
            }
            return 0;
        }
    }
    return 1;
}
typedef struct grid_move {
    unsigned char before[0x98],after[0x98];
    float delta[3],bounds[6],old_bounds[6];
    int assigned;
} grid_move;

/* One diagnosable sentence per refused or limited edit. Engine-thread only;
 * g_edit_busy serializes every writer. */
static char g_reason[320],g_note[640];
static void note(const char *message)
{
    backend_log(message);
    if(g_note[0])strncat_s(g_note,sizeof g_note,"; ",_TRUNCATE);
    strncat_s(g_note,sizeof g_note,message,_TRUNCATE);
}
static const char *say(const char *format,...)
{
    va_list args;va_start(args,format);
    _vsnprintf_s(g_reason,sizeof g_reason,_TRUNCATE,format,args);va_end(args);
    return g_reason;
}

/* Solve translations on the existing portal graph. The edited room stays at
 * its origin; each connected branch follows the doorway that moved. A cycle
 * must impose the same translation at every visit, or no edit is committed. */
static int plan_connected_inner(unsigned char *map,int instance,void *wrapper,
                          grid_move *moves,int n,int *old_connections,int ports,
                          const char **reason,int policy)
{
    int *keys=*(int**)(map+0x7d8),*owners=NULL,*queue=NULL;
    unsigned char *records=*(unsigned char**)(map+0x750);
    int i,j,k,head=0,tail=0,ok=0,connected=0;
    /* Module records and their portal ranges are copied together with the
     * map snapshot; native reconnect rebuilds only the connection values. A
     * disagreement means the map changed under the edit: refuse, never guess. */
    if(!keys||*(int*)(map+0x7e0)!=n+1||keys[0]!=0||keys[n]!=ports){
        *reason=say("GRID: module and portal tables disagree (%d modules, %d portal keys, %d portals); dimensions unchanged",
            n,*(int*)(map+0x7e0),ports);return 0;}
    owners=(int*)malloc((size_t)ports*sizeof(int));queue=(int*)malloc((size_t)n*sizeof(int));
    if((ports&&!owners)||!queue)goto done;
    for(i=0;i<n;++i){
        unsigned char *w,*module;
        if(keys[i]<0||keys[i+1]<keys[i]||keys[i+1]>ports){
            *reason=say("GRID: module %d has an invalid portal range; dimensions unchanged",i);goto done;}
        memcpy(moves[i].before,records+(size_t)i*0x98,0x98);
        memcpy(moves[i].after,moves[i].before,0x98);
        if(!(w=*(unsigned char**)moves[i].before)||!(module=*(unsigned char**)w)){
            *reason=say("GRID: module %d has no loaded module record; dimensions unchanged",i);goto done;}
        if(*(int*)(module+0xf0)!=keys[i+1]-keys[i]){
            *reason=say("GRID: module %d has %d portals but the map lists %d; dimensions unchanged",
                i,*(int*)(module+0xf0),keys[i+1]-keys[i]);goto done;}
        for(j=keys[i];j<keys[i+1];++j)owners[j]=i;
        g_world_bounds(moves[i].before,moves[i].old_bounds,(float*)(module+0xa0));
        for(k=0;k<6;++k)if(!isfinite(moves[i].old_bounds[k])){
            *reason=say("GRID: module %d has non-finite bounds; dimensions unchanged",i);goto done;}
    }
    for(i=0;i<ports;++i){int other=old_connections[i];
        if(other!=-1&&(other<0||other>=ports||other==i||old_connections[other]!=i||owners[i]==owners[other])){
            *reason=say("GRID: portal %d has a stale or one-sided connection (%d); dimensions unchanged",i,other);goto done;}
    }
    *(void**)moves[instance].after=wrapper;moves[instance].assigned=1;queue[tail++]=instance;
    while(head<tail){
        i=queue[head++];
        for(j=keys[i];j<keys[i+1];++j){
            int other=old_connections[j],neighbor;float a[6],b[6],delta[3];
            if(other<0)continue;
            connected=1;neighbor=owners[other];
            g_portal_bounds(moves[i].after,a,j-keys[i]);
            g_portal_bounds(moves[neighbor].after,b,other-keys[neighbor]);
            for(k=0;k<3;++k){
                delta[k]=a[k]-b[k];
                if(!isfinite(delta[k])||fabsf((a[k+3]-b[k+3])-delta[k])>0.001f){
                    *reason=say("GRID: doorway between modules %d and %d would change shape; dimensions unchanged",i,neighbor);goto done;}
            }
            if(moves[neighbor].assigned){
                for(k=0;k<3;++k)if(fabsf(delta[k])>0.001f){
                    *reason="GRID: connected loop prevents this size; disconnect one branch first";goto done;}
            }else{
                for(k=0;k<3;++k){float *origin=(float*)(moves[neighbor].after+0xc);
                    moves[neighbor].delta[k]=delta[k];origin[k]+=delta[k];
                    if(!isfinite(origin[k]))goto done;
                }
                memset(moves[neighbor].after+0x24,0,12);
                moves[neighbor].assigned=1;queue[tail++]=neighbor;
            }
        }
    }
    for(i=0;i<n;++i){unsigned char *module=**(unsigned char***)moves[i].after;
        g_world_bounds(moves[i].after,moves[i].bounds,(float*)(module+0xa0));
        for(k=0;k<6;++k)if(!isfinite(moves[i].bounds[k]))goto done;
    }
    /* Native module bounds are axis-aligned after their quarter-turn rotation.
     * Preserve existing overlaps, but never create or deepen one while moving
     * a connected assembly. Isolated room edits keep native placement policy. */
    if(policy&&connected)for(i=0;i<n;++i)for(j=i+1;j<n;++j){
        int overlap=1,deeper=0;
        if(i!=instance&&j!=instance&&!memcmp(moves[i].delta,moves[j].delta,12))continue;
        for(k=0;k<3;++k){
            float depth=fminf(moves[i].bounds[k+3],moves[j].bounds[k+3])-fmaxf(moves[i].bounds[k],moves[j].bounds[k]);
            float old=fminf(moves[i].old_bounds[k+3],moves[j].old_bounds[k+3])-fmaxf(moves[i].old_bounds[k],moves[j].old_bounds[k]);
            if(depth<=0.125f)overlap=0;
            if(depth>old+0.125f)deeper=1;
        }
        if(overlap&&deeper){*reason="GRID: connected resize would overlap another module; move the obstruction first";goto done;}
    }
    ok=1;
done:free(queue);free(owners);return ok;
}
static float grid_world_limit(void)
{
    float limit=g_environment_bounds?*g_environment_bounds:0;
    /* Native assembled navigation uses signed-short area/reachability
     * coordinates too. Reserve the largest admitted Grid Room outset:
     * modern X includes a 32-unit wall plus the 64-unit agent radius. */
    return isfinite(limit)&&limit>0?fminf(limit,32767-96):0;
}
static int inside_environment(const grid_move *moves,int n)
{
    int i,k;float limit=grid_world_limit();
    if(!isfinite(limit)||limit<=0)return 0;
    for(i=0;i<n;++i)if(moves[i].assigned)for(k=0;k<3;++k)
        if(moves[i].bounds[k]<-limit||moves[i].bounds[k+3]>limit)return 0;
    return 1;
}
/* The world box a module may occupy after an edit: the map boundary, widened
 * to wherever that module already reaches. An edit never creates or extends
 * a boundary crossing, but an assembly that already crosses (a lowered cvar,
 * an older map) can still be edited, and shrinking it stays possible. */
static void allowed_boxes(const grid_move *current,int n,float limit,float (*allowed)[6])
{
    int i,k;
    for(i=0;i<n;++i)for(k=0;k<3;++k){
        allowed[i][k]=fminf(-limit,current[i].bounds[k]);
        allowed[i][k+3]=fmaxf(limit,current[i].bounds[k+3]);
    }
}
static int admissible(const grid_move *moves,int n,const float (*allowed)[6],int *module,int *bound)
{
    int i,k;
    for(i=0;i<n;++i)if(moves[i].assigned)for(k=0;k<3;++k){
        if(moves[i].bounds[k]<allowed[i][k]){if(module)*module=i;if(bound)*bound=k;return 0;}
        if(moves[i].bounds[k+3]>allowed[i][k+3]){if(module)*module=i;if(bound)*bound=k+3;return 0;}
    }
    return 1;
}
static int plan_connected(unsigned char *map,int instance,void *wrapper,
                          grid_move *moves,int n,int *connections,int ports,
                          const float (*allowed)[6],const char **reason)
{
    int module=-1,bound=-1;
    if(!plan_connected_inner(map,instance,wrapper,moves,n,connections,ports,reason,1))return 0;
    if(!admissible(moves,n,allowed,&module,&bound)){
        *reason=say("GRID: resized assembly crosses the native map boundary at module %d %s %c; dimensions unchanged",
            module,bound<3?"min":"max","xyz"[bound%3]);return 0;}
    return 1;
}
/* A bounded spatial view lets the native portal/bounds helpers evaluate sizes
 * before any asset is materialized. No engine object retains these views. */
static int plan_size(unsigned char *map,int instance,const sh_grid_size *before,
                     const sh_grid_size *size,grid_move *moves,int n,int *links,int ports,
                     const char **reason)
{
    union {void *align;unsigned char bytes[0x100];} module;
    union {void *align;unsigned char bytes[0x98];} wrapper;
    float portals[4][7],*source;sh_grid_warp warp;int i,k,count;
    unsigned char *original=*(unsigned char**)(*(unsigned char**)(map+0x750)+(size_t)instance*0x98);
    if(!sh_grid_warp_init(size,&warp))return 0;
    if(!original||!*(void**)original){
        *reason=say("GRID: the selected room %d has no loaded module record; dimensions unchanged",instance);return 0;}
    memcpy(wrapper.bytes,original,sizeof wrapper.bytes);
    memcpy(module.bytes,*(void**)original,sizeof module.bytes);
    count=*(int*)(module.bytes+0xf0);source=*(float**)(module.bytes+0xe8);
    if(!source||count<1||count>4)return 0;
    memcpy(portals,source,(size_t)count*sizeof portals[0]);
    for(i=0;i<count;++i){float a[12]={0},b[12];
        for(k=0;k<3;++k)a[k]=(portals[i][k]+portals[i][k+3])*.5f;
        if(!sh_grid_cap_transform(before,size,a,b))return 0;
        for(k=0;k<3;++k){float delta=b[k]-a[k];portals[i][k]+=delta;portals[i][k+3]+=delta;}
    }
    for(k=0;k<3;++k){
        ((float*)(module.bytes+0xa0))[k]=warp.axis[k].target[0];
        ((float*)(module.bytes+0xa0))[k+3]=warp.axis[k].target[warp.axis[k].count-1];
    }
    *(void**)wrapper.bytes=module.bytes;*(void**)(module.bytes+0xe8)=portals;
    memset(moves,0,(size_t)n*sizeof *moves);
    return plan_connected_inner(map,instance,wrapper.bytes,moves,n,links,ports,reason,0);
}
static void describe_limit(char *out,size_t capacity,int instance,int module,int bound,
                           const grid_move *moves,float limit,const float (*allowed)[6])
{
    float reach=moves[module].bounds[bound],edge=allowed[module][bound];
    snprintf(out,capacity,"%s %d reaches %c=%.0f of %s %.0f",
        module==instance?"the edited room":"connected module",module,"xyz"[bound%3],reach,
        fabsf(edge)>limit+.5f?"its existing extent":"the map boundary",edge);
}
/* BuildRenderWorld drops pieces outside their module only while
 * sm_testPortalSystem (default 1, command line only) is set. */
static int area_cull(void)
{
    int value=1;
    if(!g_portal_cvar&&g_cvar_slot)g_portal_cvar=sh_engine_cvar_find(g_cvar_slot,SH_IDCVAR_NAME_FOR_LOOKUP,"sm_testPortalSystem");
    if(g_portal_cvar&&sh_engine_cvar_read_int(g_portal_cvar,&value))return value!=0;
    return 1;
}
/* Set by world_area_cell.c once Play's world-area cell size is redirected:
 * a map over 8192 areas at 512 units then builds with coarser cells, so a
 * resize is limited only when no cell size up to SH_RENDER_AREA_CELL_MAX fits. */
static int g_adaptive_cells;
void sh_grid_native_set_adaptive_cells(int live){g_adaptive_cells=live!=0;}
static float area_cell_max(void){return g_adaptive_cells?SH_RENDER_AREA_CELL_MAX:SH_RENDER_AREA_CELL;}
/* The render areas and nodes the map would have with the room at size, with
 * the cell size Play would choose. 1 counted, 0 refused by the planner (with
 * *reason), -1 not countable. */
static int count_areas(unsigned char *map,int instance,const sh_grid_size *before,const sh_grid_size *size,
                       grid_move *moves,int n,int *links,int ports,const char **reason,
                       const float (*allowed)[6],sh_render_cell_choice *out)
{
    float (*boxes)[6];int i,ok;
    if(!plan_size(map,instance,before,size,moves,n,links,ports,reason))return 0;
    if(allowed&&!admissible(moves,n,allowed,NULL,NULL))return -1;
    boxes=malloc((size_t)n*sizeof *boxes);if(!boxes)return -1;
    for(i=0;i<n;++i)memcpy(boxes[i],moves[i].bounds,sizeof boxes[i]);
    ok=sh_render_area_cell_choose((const float(*)[6])boxes,n,area_cull(),area_cell_max(),out);free(boxes);
    return ok?1:-1;
}
/* Loads at some cell size, or adds no area or node to a map that is already
 * over the limit (compared at the coarsest size, the one Play would use). */
static int areas_admitted(const sh_render_cell_choice *count,const sh_render_cell_choice *base)
{
    return count->fits||(count->coarsest.areas<=base->coarsest.areas&&count->coarsest.nodes<=base->coarsest.nodes);
}
static void describe_cells(char *out,size_t capacity,const sh_render_cell_choice *c)
{
    if(g_adaptive_cells)snprintf(out,capacity," even with %.0f-unit world areas",c->cell);
    else if(capacity)out[0]=0;
}
/* The limit applies in full only when the cell size cannot follow the map. */
static const char *cells_fallback(void)
{
    return g_adaptive_cells?"":"; the world-area cell size cannot follow the map on this build";
}
/* Limit the axis just fitted to the map boundary so the map keeps at most
 * SH_RENDER_AREA_MAX world areas and SH_RENDER_NODE_MAX area nodes at the
 * cell size Play would choose; an over-limit map loads with a ParseAreas or
 * ParseNodes error that leaves the native loading screen waiting forever.
 * With the adaptive cell size this only binds for layouts that do not fit
 * even at the coarsest size. The count is a step function of the size, so
 * the largest admitted value is found by bisection and every value taken is
 * counted. Returns 0 only for a planner refusal. */
static int limit_areas(unsigned char *map,int instance,const sh_grid_size *before,sh_grid_size *accepted,
                       int axis,unsigned start,unsigned want,grid_move *moves,int n,int *links,int ports,
                       const float (*allowed)[6],const char **reason,int *reported)
{
    sh_grid_size probe=*accepted;sh_render_cell_choice base,count,over;
    unsigned value=accepted->xyz[axis],kept=start,span,low=0,high;int grow=value>start,r;
    char message[360],cells[96];
    if(value==start)return 1;
    probe.xyz[axis]=start;
    if((r=count_areas(map,instance,before,&probe,moves,n,links,ports,reason,NULL,&base))<1){
        if(r<0)note("GRID: render areas could not be counted for this layout; the render area limit was not checked");
        return r<0;}
    if(!base.fits&&!*reported){*reported=1;describe_cells(cells,sizeof cells,&base);
        snprintf(message,sizeof message,
            "GRID: this map already has %lld world areas and %lld area nodes%s, over the engine maximum of %d each; Play cannot load it until rooms are made smaller%s",
            base.count.areas,base.count.nodes,cells,SH_RENDER_AREA_MAX,cells_fallback());note(message);}
    probe.xyz[axis]=value;
    if((r=count_areas(map,instance,before,&probe,moves,n,links,ports,reason,NULL,&count))<1){
        if(r<0)note("GRID: render areas could not be counted for this size; the render area limit was not checked");
        return r<0;}
    if(areas_admitted(&count,&base))return 1;
    over=count;span=grow?value-start:start-value;high=span;
    /* low steps are admitted, high steps are not. */
    while(high-low>1){
        unsigned mid=low+(high-low)/2;
        probe.xyz[axis]=grow?start+mid:start-mid;
        r=count_areas(map,instance,before,&probe,moves,n,links,ports,reason,allowed,&count);
        if(!r)return 0;
        if(r>0&&areas_admitted(&count,&base))low=mid;else{high=mid;if(r>0)over=count;}
    }
    kept=grow?start+low:start-low;accepted->xyz[axis]=kept;
    probe.xyz[axis]=grow?start+high:start-high;
    describe_cells(cells,sizeof cells,&over);
    if(kept==start)snprintf(message,sizeof message,
        "GRID: %c already at the render area limit: requested %u, kept %u; %u would give the map %lld world areas and %lld area nodes%s (engine maximum %d each)%s",
        "XYZ"[axis],want,start,probe.xyz[axis],over.count.areas,over.count.nodes,cells,SH_RENDER_AREA_MAX,cells_fallback());
    else snprintf(message,sizeof message,
        "GRID: %c request %u limited to %u by the render area limit: %u would give the map %lld world areas and %lld area nodes%s (engine maximum %d each)%s",
        "XYZ"[axis],want,kept,probe.xyz[axis],over.count.areas,over.count.nodes,cells,SH_RENDER_AREA_MAX,cells_fallback());
    note(message);
    return 1;
}
/* After a size change, say which world-area cell size Play will use when
 * the map needs (or needed) more than the native 512. */
static void report_cells(unsigned char *map,int instance,const sh_grid_size *before,const sh_grid_size *after,
                         grid_move *moves,int n,int *links,int ports)
{
    sh_render_cell_choice was,now;const char *reason=NULL;char message[320];
    if(!g_adaptive_cells||!memcmp(before,after,sizeof *before))return;
    if(count_areas(map,instance,before,after,moves,n,links,ports,&reason,NULL,&now)<1)return;
    if(now.cell==SH_RENDER_AREA_CELL){
        if(count_areas(map,instance,before,before,moves,n,links,ports,&reason,NULL,&was)<1||was.cell==SH_RENDER_AREA_CELL)return;
        snprintf(message,sizeof message,
            "GRID: Play will use the native world-area cell size 512 again: %lld world areas and %lld area nodes",
            now.count.areas,now.count.nodes);
    } else if(now.fits)snprintf(message,sizeof message,
        "GRID: Play will use world-area cell size %.0f: %lld world areas and %lld area nodes (512 would give %lld and %lld; engine maximum %d each)",
        now.cell,now.count.areas,now.count.nodes,now.native.areas,now.native.nodes,SH_RENDER_AREA_MAX);
    else return; /* limit_areas already said why the map cannot load. */
    note(message);
}
/* Play's cell choice for the map as it is: one GridWorldBounds box per
 * placed module record. Engine thread. Returns 0 when it cannot be counted. */
int sh_grid_native_render_cell(void *map_object,sh_render_cell_choice *out)
{
    unsigned char *map=(unsigned char*)map_object,*records,*wrapper,*module;
    float (*boxes)[6]=NULL;int n,i,k,ok=0;
    if(!map||!out||!g_world_bounds)return 0;
    __try {
        n=*(int*)(map+0x758);records=*(unsigned char**)(map+0x750);
        if(n<1||n>4096||!records)__leave;
        boxes=malloc((size_t)n*sizeof *boxes);if(!boxes)__leave;
        for(i=0;i<n;++i){
            if(!(wrapper=*(unsigned char**)(records+(size_t)i*0x98))||!(module=*(unsigned char**)wrapper))__leave;
            g_world_bounds(records+(size_t)i*0x98,boxes[i],(const float*)(module+0xa0));
            for(k=0;k<6;++k)if(!isfinite(boxes[i][k]))__leave;
        }
        ok=sh_render_area_cell_choose((const float(*)[6])boxes,n,area_cull(),area_cell_max(),out);
    } __except(EXCEPTION_EXECUTE_HANDLER) {ok=0;}
    free(boxes);
    return ok;
}
/* The map open in the editor, or NULL. */
void *sh_grid_native_editor_map(void)
{
    void *map=NULL;
    if(!g_editor)return NULL;
    __try {if(g_editor[8])map=*(void**)(g_editor+0x204c8);} __except(EXCEPTION_EXECUTE_HANDLER) {map=NULL;}
    return map;
}
/* Fit a requested size to the map boundary. Only the axes the request
 * changes are ever altered; each moves from its current value toward the
 * request as far as the boundary admits, then as far as the render area
 * limit admits. Every limited axis is logged with the limiting module,
 * coordinate and limit. Returns 0 only for a refusal (with *reason); a
 * request limited to the current size returns 1. */
static int fit_size(unsigned char *map,int instance,const sh_grid_size *before,
                    sh_grid_size *size,const char **reason,float (*allowed)[6])
{
    int n=*(int*)(map+0x758),ports=*(int*)(map+0x7c8),*links=*(int**)(map+0x7c0);
    int i,k,axis,pass,ok=0,reported=0;grid_move *current=NULL,*request=NULL;sh_grid_size accepted=*before;
    float limit=grid_world_limit();char message[320],where[160];
    if(n<1||n>4096||ports<0||ports>16384||!links||!*(void**)(map+0x7d8)){
        *reason=say("GRID: map module tables are unavailable (%d modules, %d portals); dimensions unchanged",n,ports);return 0;}
    if(!isfinite(limit)||limit<=0){
        *reason="GRID: snapEdit_environmentModuleBounds is unavailable; dimensions unchanged";return 0;}
    current=calloc((size_t)n,sizeof *current);request=calloc((size_t)n,sizeof *request);
    if(!current||!request)goto done;
    if(!plan_size(map,instance,before,before,current,n,links,ports,reason))goto done;
    allowed_boxes(current,n,limit,allowed);
    for(i=0;i<n;++i)if(current[i].assigned)for(k=0;k<6;++k)if(fabsf(allowed[i][k])>limit){
        snprintf(message,sizeof message,
            "GRID: %s %d already crosses the map boundary at %c=%.0f (limit %.0f); edits may not extend it",
            i==instance?"the edited room":"connected module",i,"xyz"[k%3],allowed[i][k],limit);
        note(message);
    }
    /* Shrinking axes first: a request that grows one axis while shrinking
     * another is then fitted against the smaller layout, which normally
     * leaves more room (map bounds and render areas) for the grown axis. */
    for(pass=0;pass<6;++pass){
        double t=1;int module=-1,bound=-1,grow;unsigned value,start,want;sh_grid_size probe=accepted;
        axis=pass%3;start=accepted.xyz[axis];want=size->xyz[axis];
        if(want==start||(want>start)!=(pass>=3))continue;
        grow=want>start;probe.xyz[axis]=want;
        if(!plan_size(map,instance,before,&probe,request,n,links,ports,reason))goto done;
        if(admissible(request,n,(const float(*)[6])allowed,NULL,NULL)){accepted=probe;
            if(!limit_areas(map,instance,before,&accepted,axis,start,want,request,n,links,ports,
                (const float(*)[6])allowed,reason,&reported))goto done;
            continue;}
        if(!plan_size(map,instance,before,&accepted,current,n,links,ports,reason))goto done;
        /* Bounds are affine in one dimension between two valid sizes. */
        for(i=0;i<n;++i)if(request[i].assigned)for(k=0;k<6;++k){
            double from=current[i].bounds[k],delta=request[i].bounds[k]-from,edge=allowed[i][k],reach;
            if(fabs(delta)<.0001||(k<3?delta>0:delta<0))continue;
            reach=(edge-from)/delta;
            if(reach<t){t=reach;module=i;bound=k;}
        }
        if(t<0)t=0;
        value=grow?start+(unsigned)floor(t*(want-start)+.00001):start-(unsigned)floor(t*(start-want)+.00001);
        for(;;){
            probe.xyz[axis]=value;
            if(plan_size(map,instance,before,&probe,request,n,links,ports,reason)&&
               admissible(request,n,(const float(*)[6])allowed,NULL,NULL))break;
            if(value==start)break;
            value=grow?value-1:value+1;
        }
        accepted.xyz[axis]=value;
        if(module<0){module=instance;bound=grow?axis+3:axis;}
        probe.xyz[axis]=value;
        if(!plan_size(map,instance,before,&probe,request,n,links,ports,reason))goto done;
        describe_limit(where,sizeof where,instance,module,bound,request,limit,(const float(*)[6])allowed);
        if(!grow)snprintf(message,sizeof message,
            "GRID: %c shrink %u -> %u limited to %u: %s (unexpected unless a connected branch wraps past the room)",
            "XYZ"[axis],start,want,value,where);
        else if(value==start)snprintf(message,sizeof message,
            "GRID: %c already at the map boundary: requested %u, kept %u; %s","XYZ"[axis],want,start,where);
        else snprintf(message,sizeof message,
            "GRID: %c request %u limited to %u by the map boundary: %s","XYZ"[axis],want,value,where);
        note(message);
        if(!limit_areas(map,instance,before,&accepted,axis,start,want,request,n,links,ports,
            (const float(*)[6])allowed,reason,&reported))goto done;
    }
    report_cells(map,instance,before,&accepted,request,n,links,ports);
    *size=accepted;ok=1;
done:free(current);free(request);return ok;
}
static int owned_entity(unsigned char *map,int instance,int id)
{
    int *keys=*(int**)(map+0x720),*values=*(int**)(map+0x708),start,end,i;
    if(!keys||!values||*(int*)(map+0x728)<instance+2)return 0;
    start=keys[instance];end=keys[instance+1];
    if(start<0||end<start||end>*(int*)(map+0x710)||end-start>65536)return 0;
    for(i=start;i<end;++i)if(values[i]==id)return 1;
    return 0;
}
static void refresh_placement_surfaces(unsigned char *map)
{
    void *surfaces=*(void**)(g_editor+0x20548),*world=*(void**)(g_editor+0x198);
    /* The native camera normally rebuilds these on Object Mode entry. A
     * dimension edit replaces module geometry without re-entering that mode.
     * Keep its placement rays and glowing surface display on the same module
     * revision. Blueprint leaves an uninitialized display to the native entry. */
    if(surfaces&&world&&*(int*)((unsigned char*)surfaces+8)>0)
        g_refresh_surfaces(surfaces,map,world);
}
/* The editor map's module-instance index of `record`, or -1 when it is not
 * one of that map's 0x98-byte records. */
int sh_grid_native_record_instance(const void *record)
{
    int index=-1;
    if(!g_editor||!record)return -1;
    __try {
        unsigned char *map=g_editor[8]?*(unsigned char**)(g_editor+0x204c8):NULL,*records;int n;uintptr_t offset;
        if(map&&(records=*(unsigned char**)(map+0x750))!=NULL&&(n=*(int*)(map+0x758))>0&&n<=4096){
            offset=(uintptr_t)record-(uintptr_t)records;
            if(offset<(uintptr_t)n*0x98&&offset%0x98==0)index=(int)(offset/0x98);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){index=-1;}
    return index;
}
/* Rebuild the editor's placement surfaces for the current map, as a dimension
 * edit does, so a display model whose name changed is loaded. Engine thread
 * only: the native rebuild allocates its display list and loads models, so it
 * runs inside a verified process-heap scope, never on a map heap Play frees.
 * 1 rebuilt, 0 nothing built yet (native Object Mode entry builds it), -1 refused. */
int sh_grid_native_refresh_placement(void)
{
    sh_process_heap_scope heap={0};int result=-1;
    if(!g_editor||!g_refresh_surfaces)return -1;
    __try {
        unsigned char *map=g_editor[8]?*(unsigned char**)(g_editor+0x204c8):NULL;
        void *surfaces=*(void**)(g_editor+0x20548),*world=*(void**)(g_editor+0x198);
        if(!map||!surfaces||!world||*(int*)((unsigned char*)surfaces+8)<=0)result=0;
        else if(sh_process_heap_enter(&g_heap,&heap)){
            __try { g_refresh_surfaces(surfaces,map,world);result=1; }
            __finally { if(!sh_process_heap_leave(&heap))result=-1; }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){result=-1;}
    return result;
}
static char g_last[1024];
static void finish(const char *message)
{
    backend_log(message);
    strncpy_s(g_last,sizeof g_last,message,_TRUNCATE);
    if(g_note[0]){strncat_s(g_last,sizeof g_last," (",_TRUNCATE);strncat_s(g_last,sizeof g_last,g_note,_TRUNCATE);
        strncat_s(g_last,sizeof g_last,")",_TRUNCATE);g_note[0]=0;}
}
const char *sh_grid_native_last_message(void){return g_last;}
int sh_grid_native_apply(void *edit_data,int instance,const sh_grid_size *size)
{
    sh_grid_size before,accepted;sh_grid_warp warp;sh_process_heap_scope heap={0};
    unsigned char *map=NULL,*records;grid_move *moves=NULL;
    grid_cap_change caps[8]={0};int count=0,attempted=0,replaced=0,ok=0,i,j,k,start,end,n=0,ports;
    grid_light_change lights[GRID_LIGHT_CAP]={0};int light_count=0,light_attempted=0,unchanged=0;
    int *old_connections=NULL;unsigned char *valid=NULL;float (*allowed)[6]=NULL;
    int *keys,*doors,*frames,*connections;void *wrapper;
    const char *reason="GRID: native size edit refused";
    if(!g_replace||!g_reconnect||!g_edit_entity||!g_set_transform||!g_portal_bounds||!g_world_bounds||!g_refresh_surfaces||!g_editor){
        finish("GRID: dimension edit refused; native editor functions are not bound");return 0;}
    if(!size||!sh_grid_warp_init(size,&warp)){finish("GRID: dimension edit refused; the requested size is outside the room format");return 0;}
    if(InterlockedCompareExchange(&g_edit_faulted,0,0)){
        finish("GRID: dimension edit refused; edits are disabled after an earlier recovery failure in this session");return 0;}
    if(InterlockedCompareExchange(&g_edit_busy,1,0)){finish("GRID: dimension edit refused; another dimension edit is in progress");return 0;}
    __try {
        __try {
            /* The palette factory and entity commits allocate through native
             * helpers. A verified heap push also rejects an off-main caller. */
            g_note[0]=0;
            reason="GRID: dimension edit refused; not on the engine thread";
            if(!sh_process_heap_enter(&g_heap,&heap))__leave;
            reason=say("GRID: dimension edit refused; edit data %p is not the active editor map, or the editor is inactive",edit_data);
            if(*(void**)(g_editor+0x204d0)!=edit_data||!g_editor[8])__leave;
            reason=say("GRID: dimension edit refused; module %d is not a Grid Room of the requested kind",instance);
            if(!sh_grid_native_read(edit_data,instance,&before)||before.kind!=size->kind)__leave;
            map=*(unsigned char**)edit_data;
            reason="GRID: dimension edit refused; the editor map changed during the edit";
            if(*(void**)(g_editor+0x204c8)!=map)__leave;
            records=*(unsigned char**)(map+0x750);
            n=*(int*)(map+0x758);if(n<1||n>4096||!records)__leave;
            /* Native EditorFrame rebuilds every portal connection and placement
             * flag from the current module records once per frame. Rebuild them
             * here too, so a module added or deleted earlier in this frame can
             * never leave a stale neighbour in the plan. */
            g_reconnect(map);records=*(unsigned char**)(map+0x750);
            valid=(unsigned char*)malloc((size_t)n);allowed=calloc((size_t)n,sizeof *allowed);
            reason="GRID: dimension edit refused; out of memory";
            if(!valid||!allowed)__leave;
            for(i=0;i<n;++i)valid[i]=records[(size_t)i*0x98+0x30];
            accepted=*size;reason="GRID: dimension edit refused";
            if(!fit_size(map,instance,&before,&accepted,&reason,allowed))__leave;
            size=&accepted;
            if(!memcmp(before.xyz,size->xyz,sizeof before.xyz)){ok=unchanged=1;__leave;}
            keys=*(int**)(map+0x7d8);doors=*(int**)(map+0x778);
            frames=*(int**)(map+0x790);connections=*(int**)(map+0x7c0);
            reason="GRID: dimension edit refused; the room's doorway tables are inconsistent";
            if(!keys||!doors||!frames||!connections||*(int*)(map+0x7e0)<instance+2)__leave;
            start=keys[instance];end=keys[instance+1];
            if(start<0||end<start||end-start!=(size->kind==SH_GRID_MODERN?2:4)||
               end>*(int*)(map+0x780)||end>*(int*)(map+0x798)||end>*(int*)(map+0x7c8))__leave;
            reason="GRID: invalid portal entity ownership; dimensions unchanged";
            for(i=start;i<end;++i)for(j=0;j<2;++j){
                int id=j?frames[i]:doors[i];grid_cap_change *cap;
                if(id==-1)continue;
                if(id<0||!owned_entity(map,instance,id))__leave;
                for(k=0;k<count;++k)if(caps[k].id==id)break;
                if(k<count)continue;
                if(count==8)__leave;
                cap=&caps[count++];cap->id=id;
                {unsigned char **entities=*(unsigned char***)(map+0x6a0),*entity;
                    if(id>=*(int*)(map+0x6a8)||!entities||!(entity=entities[id])||!*(void**)(entity+8))__leave;
                    memcpy(cap->before,entity+0x290,sizeof cap->before);
                }
                if(!sh_grid_cap_transform(&before,size,cap->before,cap->after))__leave;
            }
            reason="GRID: native variant could not be constructed; dimensions unchanged";
            wrapper=sh_grid_native_wrapper(size);if(!wrapper)__leave;
            reason="GRID: invalid connected module layout; dimensions unchanged";
            ports=*(int*)(map+0x7c8);if(ports<0||ports>16384)__leave;
            moves=(grid_move*)calloc((size_t)n,sizeof *moves);
            old_connections=(int*)malloc((size_t)ports*sizeof(int));
            if(!moves||(ports&&!old_connections))__leave;
            memcpy(old_connections,connections,(size_t)ports*sizeof(int));
            if(!plan_connected(map,instance,wrapper,moves,n,old_connections,ports,(const float(*)[6])allowed,&reason))__leave;
            reason="GRID: built-in light preparation failed; dimensions unchanged";
            if(!plan_lights(map,instance,size,lights,&light_count))__leave;
            reason="GRID: door update failed; restoring the previous room dimensions";
            for(i=0;i<count;++i){
                void *entity=g_edit_entity(edit_data,caps[i].id);if(!entity)__leave;
                attempted=i+1;
                g_set_transform(entity,caps[i].after);
            }
            reason="GRID: built-in light update failed; restoring the previous room dimensions";
            for(i=0;i<light_count;++i){light_attempted=i+1;if(!apply_light(edit_data,&lights[i],0))__leave;}
            /* Native dirty bits make the preview compare its old render name
             * against this wrapper, then destroy/recreate that one display. */
            for(i=0;i<n;++i)if(memcmp(moves[i].before,moves[i].after,0x98)){
                replaced=1;g_replace(map,(unsigned)i,moves[i].after);
            }
            g_reconnect(map);
            records=*(unsigned char**)(map+0x750);
            /* A placement that was valid must stay valid. One that native code
             * already marked invalid (outside the boundary or overlapping)
             * before this edit does not block an edit that does not worsen it. */
            for(i=0;i<n;++i)if(moves[i].assigned&&valid[i]&&!records[(size_t)i*0x98+0x30]){
                reason=say("GRID: native placement of module %d became invalid; restoring the previous layout",i);__leave;}
            reason="GRID: native portal connection changed; restoring the previous layout";
            if(*(int*)(map+0x7c8)!=ports||memcmp(*(void**)(map+0x7c0),old_connections,(size_t)ports*sizeof(int)))__leave;
            reason="GRID: placement surface refresh failed; restoring the previous room dimensions";
            refresh_placement_surfaces(map);
            ok=1;
        } __finally {
            if(!ok&&(attempted||light_attempted||replaced)){
                int restored=1;
                __try {
                    if(*(void**)(g_editor+0x204c8)!=map)restored=0;
                    else {
                        for(i=0;i<attempted;++i){void *entity=g_edit_entity(edit_data,caps[i].id);
                            if(!entity)restored=0;else g_set_transform(entity,caps[i].before);}
                        for(i=0;i<light_attempted;++i)if(!apply_light(edit_data,&lights[i],1))restored=0;
                        if(moves)for(i=0;i<n;++i)if(memcmp(moves[i].before,moves[i].after,0x98))
                            g_replace(map,(unsigned)i,moves[i].before);
                        g_reconnect(map);
                        if(replaced)refresh_placement_surfaces(map);
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER){restored=0;}
                /* Only a failed restoration latches: the map may then disagree
                 * with this adapter. Every refusal before mutation is recoverable. */
                if(!restored){InterlockedExchange(&g_edit_faulted,1);
                    backend_log("GRID: edit recovery incomplete; further dimension edits disabled");}
            }
            free(moves);free(old_connections);free(valid);free(allowed);
            __try {
                for(i=0;i<light_count;++i){destroy_tree(&lights[i].old_tree);destroy_tree(&lights[i].new_tree);}
            } __finally {
                if(!sh_process_heap_leave(&heap)){ok=0;InterlockedExchange(&g_edit_faulted,1);}
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_edit_faulted,1);ok=0;
        reason="GRID: native size edit faulted; further dimension edits disabled";}
    InterlockedExchange(&g_edit_busy,0);
    if(ok&&unchanged){char message[160];
        snprintf(message,sizeof message,"GRID: selected room dimensions unchanged at %u x %u x %u",
            before.xyz[0],before.xyz[1],before.xyz[2]);finish(message);}
    else if(ok){char message[160];
        snprintf(message,sizeof message,"GRID: selected room dimensions updated to %u x %u x %u (%d built-in lights)",
            accepted.xyz[0],accepted.xyz[1],accepted.xyz[2],light_count);finish(message);}
    else finish(reason);
    return ok;
}
int sh_grid_native_selected(int instance,const float *requested,sh_grid_size *out,int *resolved)
{
    void *edit_data;sh_grid_size current,next;
    if(resolved)*resolved=-1;
    if(!g_editor||!g_editor[8]||!(edit_data=*(void**)(g_editor+0x204d0))){
        finish("GRID: no active editor map");return 0;}
    __try {
        /* The Blueprint selection, else the open properties panel's module. */
        if(instance<0)instance=*(int*)((unsigned char*)edit_data+0x20);
        if(instance<0&&*(int*)(g_editor+0x23618)==4)instance=*(int*)(g_editor+0x22ef8);
    } __except(EXCEPTION_EXECUTE_HANDLER){instance=-1;}
    if(resolved)*resolved=instance;
    if(instance<0||!sh_grid_native_read(edit_data,instance,&current)){
        finish(say("GRID: module %d is not a Grid Room; select one or pass its index",instance));return 0;}
    if(requested){
        /* The same entry path as the native inspector: format clamp, apply, read back. */
        if(!sh_grid_clamp(current.kind,requested,&next)){finish("GRID: requested size is not finite");return 0;}
        if(!sh_grid_native_apply(edit_data,instance,&next))return 0;
        if(!sh_grid_native_read(edit_data,instance,&current))return 0;
    }
    if(out)*out=current;
    return 1;
}
int sh_grid_native_install(const sig_result *r,size_t n,const uint8_t *base)
{
    void *find=clean(r,n,"GridFindModule"),*has=clean(r,n,"GridHasModule");
    void *collision=clean(r,n,"GridBuildModuleCollision");
    void *containing=clean(r,n,"GridContainingModule");
    if(g_find)return hook_is_installed((void*)g_find)&&hook_is_installed(g_has_hook)&&
        hook_is_installed((void*)g_containing_module);
    g_module_ray=(module_ray_fn)clean(r,n,"GridModuleRay");
    g_refresh_surfaces=(refresh_surfaces_fn)clean(r,n,"GridRefreshPlacementSurfaces");
    g_load=(load_module_fn)clean(r,n,"GridLoadModule");g_ctor=(str_ctor_fn)clean(r,n,"IdStrCtor");
    g_dtor=(str_dtor_fn)clean(r,n,"IdStrDtor");g_anchor=clean(r,n,"DeclRegistryAnchor");
    g_type=(type_fn)clean(r,n,"DeclTypeByName");g_register=(register_fn)clean(r,n,"DeclRegisterFile");
    g_source=(source_fn)clean(r,n,"DeclSourceFind");g_decl=(find_decl_fn)clean(r,n,"DeclFind");
    g_generic_load=(generic_load_fn)clean(r,n,"ResourceGenericLoad");
    g_replace=(replace_instance_fn)clean(r,n,"GridReplaceInstance");
    g_reconnect=(reconnect_fn)clean(r,n,"GridReconnectPortals");
    if(g_reconnect){const unsigned char *p=(const unsigned char*)g_reconnect;int32_t displacement;
        /* Both native reconnect implementations read this cvar at +0x8f. */
        if(!memcmp(p+0x8f,"\xf3\x0f\x10\x35",4)){
            memcpy(&displacement,p+0x93,4);g_environment_bounds=(const float*)(p+0x97+displacement);
        }
    }
    g_edit_entity=(edit_entity_fn)clean(r,n,"GridEditEntity");
    g_set_transform=(set_transform_fn)clean(r,n,"GridSetEntityTransform");
    g_portal_bounds=(portal_bounds_fn)clean(r,n,"GridPortalBounds");
    g_world_bounds=(world_bounds_fn)clean(r,n,"GridWorldBounds");
    g_entity_tree=(entity_tree_fn)clean(r,n,"GridEntityTree");
    g_apply_tree=(entity_tree_apply_fn)clean(r,n,"GridApplyEntityTree");
    g_read_properties=(entity_tree_apply_fn)clean(r,n,"GridReadEntityProperties");
    g_tree_vec=(tree_vec_fn)clean(r,n,"GridTreeSetVec3");
    g_tree_float=(tree_float_fn)clean(r,n,"GridTreeSetFloat");
    {const unsigned char *p=(const unsigned char*)clean(r,n,"GridReapplyEntityTree");int32_t displacement;
        /* The native re-apply routine owns the same tree: it gets it from GridEntityTree,
         * applies it, then destroys it and deletes its 0x48 bytes. Bind its destructor and
         * delete only when its two leading calls reach the resolved getter and applier.
         * Its live destroy/free check stays open: R-012 could not reach it before the stream-slot fix. */
        if(p&&g_entity_tree&&g_apply_tree&&p[0x17]==0xe8&&p[0x25]==0xe8&&p[0x38]==0xe8&&p[0x45]==0xe8&&
           !memcmp(p+0x3d,"\xba\x48\0\0\0\x48\x8b\xcb",8)){
            memcpy(&displacement,p+0x18,4);
            if((const void*)(p+0x1c+displacement)==(const void*)g_entity_tree){
                memcpy(&displacement,p+0x26,4);
                if((const void*)(p+0x2a+displacement)==(const void*)g_apply_tree){
                    memcpy(&displacement,p+0x39,4);g_tree_destroy=(tree_destroy_fn)(p+0x3d+displacement);
                    memcpy(&displacement,p+0x46,4);g_tree_free=(tree_free_fn)(p+0x4a+displacement);
                }
            }
        }
    }
    g_editor=(unsigned char*)glb_resolve(base,"editor_singleton",NULL);
    g_cvar_slot=(const void*)glb_resolve(base,"cvar_system_slot",NULL);
    if(!find||!has||!collision||!containing||!g_module_ray||!g_refresh_surfaces||!g_environment_bounds||!g_load||!g_ctor||!g_dtor||!g_anchor||!g_type||!g_register||!g_source||!g_decl||
        !g_generic_load||!g_editor||!g_replace||!g_reconnect||!g_edit_entity||!g_set_transform||
        !g_portal_bounds||!g_world_bounds||!g_entity_tree||!g_apply_tree||!g_read_properties||
        !g_tree_vec||!g_tree_float||!g_tree_destroy||!g_tree_free||!sh_process_heap_bind(&g_heap,r,n,base))return 0;
    /* First five instructions are 16 register/stack-only bytes on both images. */
    g_find=(find_module_fn)hook_prepare(find,(void*)find_variant,16);
    /* Membership's first 16 bytes include a RIP-relative comparator address.
     * This ownership trampoline is NEVER executed: has_variant delegates to
     * the original FindModule for stock names instead. */
    g_has_hook=hook_prepare(has,(void*)has_variant,16);
    g_build_collision=(build_collision_fn)hook_prepare(collision,(void*)grid_build_collision,21);
    /* The first 19 bytes save registers and reserve stack on both renderers. */
    g_containing_module=(containing_module_fn)hook_prepare(containing,(void*)grid_containing_module,19);
    if(g_find&&g_has_hook&&g_build_collision&&g_containing_module&&
       hook_commit((void*)g_containing_module)==B2_PATCH_OK&&hook_commit((void*)g_build_collision)==B2_PATCH_OK&&
       hook_commit((void*)g_find)==B2_PATCH_OK&&hook_commit(g_has_hook)==B2_PATCH_OK){
        backend_log("GRID: private module lookup and reload admission installed");return 1;}
    if(g_has_hook&&hook_unpatch(g_has_hook))g_has_hook=NULL;
    if(g_containing_module&&hook_unpatch((void*)g_containing_module))g_containing_module=NULL;
    if(g_build_collision&&hook_unpatch((void*)g_build_collision))g_build_collision=NULL;
    if(g_find&&hook_unpatch((void*)g_find))g_find=NULL;return 0;
}
