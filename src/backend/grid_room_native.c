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
#include "engine_globals.h"
#include "overrides.h"
#include "process_heap_scope.h"
#include "hook.h"

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
    sh_grid_size size;
    int state; /* 1 constructing, 2 ready, -1 retained failed native allocation. */
    void *record;
    union {void *align;unsigned char bytes[128];} palette;
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
static entity_tree_fn g_entity_tree;
static entity_tree_apply_fn g_apply_tree,g_read_properties;
static tree_vec_fn g_tree_vec;
static tree_float_fn g_tree_float;
static tree_destroy_fn g_tree_destroy;
static tree_free_fn g_tree_free;
static sh_process_heap_api g_heap;

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
static int catalog(const char *logical)
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
    if(*(int*)((unsigned char*)decl+0x118)!=1||*(int*)((unsigned char*)decl+0x130)!=4){
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
static void *create(void *palette,const sh_grid_size *size)
{
    unsigned i;grid_native_entry *entry;unsigned char *stock_record,*stock_wrapper,*p;
    const char *render,*slash;char stock[128],variant[128],info[160],palette_name[128];
    native_string palette_string,module_string;void *module,*result=NULL;
    sh_process_heap_scope heap={0};size_t len;int strings=0;
    for(i=0;i<g_count;++i)if(g_entries[i].size.kind==size->kind&&
        !memcmp(g_entries[i].size.xyz,size->xyz,sizeof size->xyz))
        return g_entries[i].state==2?g_entries[i].record:NULL;
    /* Reload admission runs before the editor becomes active. The palette and
     * stock wrapper, plus the main-thread heap scope, establish readiness. */
    if(g_count==GRID_NATIVE_CAP||palette!=g_editor+0x206c0)return NULL;
    snprintf(stock,sizeof stock,"%s.decl",sh_grid_stock_name(size->kind));
    stock_record=(unsigned char*)g_find(palette,stock);
    if(!stock_record||stock_record==end_record(palette))return NULL;
    stock_wrapper=*(unsigned char**)stock_record;if(!stock_wrapper)return NULL;
    render=*(const char**)(stock_wrapper+0x18);
    if(!render||strncmp(render,"maps/modules/palettes/",22))return NULL;
    slash=strchr(render+22,'/');if(!slash)return NULL;
    len=(size_t)(slash-render-22);if(!len||len>=sizeof palette_name)return NULL;
    memcpy(palette_name,render+22,len);palette_name[len]=0;
    if(!sh_grid_name(size,variant,sizeof variant))return NULL;
    snprintf(info,sizeof info,"decltree/snapmoduleinfo/%s.decl",variant+13);
    strcat_s(variant,sizeof variant,".decl");
    if(!preflight(variant)||!preflight(info))return NULL;
    {char collision[180],identity[112];const char *leaf;
        if(!sh_grid_name(size,identity,sizeof identity))return NULL;
        leaf=strrchr(identity,'/')+1;
        snprintf(collision,sizeof collision,"%s/_combo/world.bcm",identity);
        if(!preflight(collision))return NULL;
        snprintf(collision,sizeof collision,"%s/%s_floor_collision.bcm",identity,leaf);
        if(!preflight(collision))return NULL;
    }
    {char nav[180],identity[112];const char *leaf;unsigned agents[]={48,96,128};
        if(!sh_grid_name(size,identity,sizeof identity))return NULL;
        leaf=strrchr(identity,'/')+1;
        for(i=0;i<3;++i){snprintf(nav,sizeof nav,"generated/%s/%s.baas_monster%u",identity,leaf,agents[i]);
            if(!preflight(nav)){backend_log("GRID: resized navigation preflight failed");return NULL;}}
    }
    if(!sh_process_heap_enter(&g_heap,&heap))return NULL;
    entry=&g_entries[g_count++];memset(entry,0,sizeof *entry);entry->size=*size;entry->state=1;
    __try {
        char logical[112];size_t k=strlen(variant+13)-5;
        memcpy(logical,variant+13,k);logical[k]=0;
        if(!catalog(logical))__leave;
        p=entry->palette.bytes;
        /* Same empty list construction as the editor's module palette. The
         * private palette never enters a stock array or a stock destructor. */
        *(void**)p=*(void**)palette;
        *(unsigned*)(p+0x20)=*(unsigned*)(p+0x38)=
            *(unsigned*)(p+0x60)=*(unsigned*)(p+0x78)=0x50000;
        g_ctor(&palette_string,palette_name);strings=1;
        g_ctor(&module_string,variant);strings=2;
        if(!g_load(p,*(void**)(stock_wrapper+0x90),&palette_string,&module_string,g_editor+0x20660)){
            backend_log("GRID: native module loader rejected the variant");__leave;}
        if(*(int*)(p+0x18)!=1||*(int*)(p+0x70)!=1){backend_log("GRID: private module palette has unexpected counts");__leave;}
        result=*(void**)(p+0x10);if(!result||!*(void**)result)__leave;
        module=**(void***)result;
        if(!module||!*(void**)((unsigned char*)*(void**)result+0x80)||
           !*(void**)((unsigned char*)*(void**)result+0x88)) {result=NULL;__leave;}
        *(unsigned*)((unsigned char*)module+0x28)=4;
    } __finally {
        __try {
            __try {if(strings>=2)g_dtor(&module_string);}
            __finally {if(strings>=1)g_dtor(&palette_string);}
        } __finally {if(!sh_process_heap_leave(&heap))result=NULL;}
    }
    entry->record=result;entry->state=result?2:-1;
    backend_log(result?"GRID: independent native module variant loaded":"GRID: native module variant refused");
    return result;
}
static void *find_variant(void *palette,const char *name)
{
    sh_grid_size size;void *record=NULL;
    if(!sh_grid_parse_name(name,&size))return g_find(palette,name);
    if(InterlockedCompareExchange(&g_faulted,0,0)||InterlockedCompareExchange(&g_busy,1,0))return end_record(palette);
    __try {record=create(palette,&size);}
    __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);backend_log("GRID: native variant creation faulted; disabled");}
    InterlockedExchange(&g_busy,0);
    return record?record:end_record(palette);
}
static unsigned char has_variant(void *palette,const char *name)
{
    /* The native membership probe uses a separate binary search and therefore
     * bypasses FindModule. Use the same lookup for both admission and copying;
     * otherwise reload removes the serialized instance before conversion. */
    return (unsigned char)(find_variant(palette,name)!=end_record(palette));
}
void *sh_grid_native_wrapper(const sh_grid_size *size)
{
    char name[128];void *record;sh_grid_size stock;
    if(!g_find||!g_editor||!sh_grid_name(size,name,sizeof name))return NULL;
    /* Reverting a stock room's dimensions must restore its original identity,
     * including when the native properties panel cancels its pending edits. */
    if(sh_grid_default(size->kind,&stock)&&!memcmp(stock.xyz,size->xyz,sizeof stock.xyz)){
        snprintf(name,sizeof name,"%s.decl",sh_grid_stock_name(size->kind));
        record=g_find(g_editor+0x206c0,name);
    }else{
        strcat_s(name,sizeof name,".decl");record=find_variant(g_editor+0x206c0,name);
    }
    return record&&record!=end_record(g_editor+0x206c0)?*(void**)record:NULL;
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
#define GRID_LIGHT_CAP 8
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
            if(!c->old_tree||!c->new_tree||!g_tree_vec(c->new_tree,"lightRadius",c->target.radius,reflect)||
               !g_tree_vec(c->new_tree,"lightCenter",c->target.center,reflect))goto done;
            if(c->target.visible_range>0&&!g_tree_float(c->new_tree,"maxVisibleRange",c->target.visible_range,reflect))goto done;
            if(c->target.shadow_range>0&&!g_tree_float(c->new_tree,"maxShadowVisibleRange",c->target.shadow_range,reflect))goto done;
        }
    }
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
        if(fabsf(*(float*)(body+0xa0+4*k)-light->target.radius[k])>.01f||
           fabsf(*(float*)(body+0xac+4*k)-light->target.center[k])>.01f)return 0;
    }
    return 1;
}
typedef struct grid_move {
    unsigned char before[0x98],after[0x98];
    float delta[3],bounds[6],old_bounds[6];
    int assigned;
} grid_move;

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
    if(*(int*)(map+0x7e0)!=n+1||keys[0]!=0||keys[n]!=ports)return 0;
    owners=(int*)malloc((size_t)ports*sizeof(int));queue=(int*)malloc((size_t)n*sizeof(int));
    if((ports&&!owners)||!queue)goto done;
    for(i=0;i<n;++i){
        unsigned char *w,*module;
        if(keys[i]<0||keys[i+1]<keys[i]||keys[i+1]>ports)goto done;
        memcpy(moves[i].before,records+(size_t)i*0x98,0x98);
        memcpy(moves[i].after,moves[i].before,0x98);
        if(!(w=*(unsigned char**)moves[i].before)||!(module=*(unsigned char**)w)||
           *(int*)(module+0xf0)!=keys[i+1]-keys[i])goto done;
        for(j=keys[i];j<keys[i+1];++j)owners[j]=i;
        g_world_bounds(moves[i].before,moves[i].old_bounds,(float*)(module+0xa0));
        for(k=0;k<6;++k)if(!isfinite(moves[i].old_bounds[k]))goto done;
    }
    for(i=0;i<ports;++i){int other=old_connections[i];
        if(other!=-1&&(other<0||other>=ports||other==i||old_connections[other]!=i||owners[i]==owners[other]))goto done;
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
                if(!isfinite(delta[k])||fabsf((a[k+3]-b[k+3])-delta[k])>0.001f)goto done;
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
static int plan_connected(unsigned char *map,int instance,void *wrapper,
                          grid_move *moves,int n,int *connections,int ports,const char **reason)
{
    if(!plan_connected_inner(map,instance,wrapper,moves,n,connections,ports,reason,1))return 0;
    if(!inside_environment(moves,n)){
        *reason="GRID: resized assembly exceeds the native map boundary";return 0;}
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
static int fit_size(unsigned char *map,int instance,const sh_grid_size *before,
                    sh_grid_size *size,const char **reason)
{
    int n=*(int*)(map+0x758),ports=*(int*)(map+0x7c8),*links=*(int**)(map+0x7c0);
    int i,k,axis,ok=0;grid_move *requested=NULL,*minimum=NULL;sh_grid_size smallest,probe;
    float limit=grid_world_limit();
    if(n<1||n>4096||ports<0||ports>16384||!links||!isfinite(limit)||limit<=0||
       !*(void**)(map+0x7d8)||!sh_grid_minimum(size->kind,&smallest))return 0;
    requested=calloc((size_t)n,sizeof *requested);minimum=calloc((size_t)n,sizeof *minimum);
    if(!requested||!minimum)goto done;
    /* With native quarter-turn module rotations, each local dimension affects
     * one world coordinate. Intersect its affine boundary constraints across
     * the entire connected branch, then round down to a valid whole unit. */
    for(axis=0;axis<3;++axis){double low=0,high=1;
        if(!plan_size(map,instance,before,size,requested,n,links,ports,reason))goto done;
        if(inside_environment(requested,n)){ok=1;goto done;}
        probe=*size;probe.xyz[axis]=smallest.xyz[axis];
        if(!plan_size(map,instance,before,&probe,minimum,n,links,ports,reason))goto done;
        for(i=0;i<n;++i)if(requested[i].assigned)for(k=0;k<6;++k){
            double start=minimum[i].bounds[k],delta=requested[i].bounds[k]-start,a,b;
            if(fabs(delta)<.0001)continue;
            a=(-limit-start)/delta;b=(limit-start)/delta;
            low=fmax(low,fmin(a,b));high=fmin(high,fmax(a,b));
        }
        if(high<low||high<0){*reason="GRID: connected assembly cannot fit inside the native map boundary";goto done;}
        size->xyz[axis]=smallest.xyz[axis]+(unsigned)floor(high*(size->xyz[axis]-smallest.xyz[axis])+.00001);
    }
    if(plan_size(map,instance,before,size,requested,n,links,ports,reason)&&inside_environment(requested,n))ok=1;
    else *reason="GRID: connected assembly cannot fit inside the native map boundary";
done:free(requested);free(minimum);return ok;
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
int sh_grid_native_apply(void *edit_data,int instance,const sh_grid_size *size)
{
    sh_grid_size before,accepted;sh_grid_warp warp;sh_process_heap_scope heap={0};
    unsigned char *map=NULL,*records;grid_move *moves=NULL;
    grid_cap_change caps[8]={0};int count=0,attempted=0,replaced=0,ok=0,i,j,k,start,end,n=0,ports;
    grid_light_change lights[GRID_LIGHT_CAP]={0};int light_count=0,light_attempted=0;
    int *old_connections=NULL;
    int *keys,*doors,*frames,*connections;void *wrapper;
    const char *reason="GRID: native size edit refused";
    if(!g_replace||!g_reconnect||!g_edit_entity||!g_set_transform||!g_portal_bounds||!g_world_bounds||!g_refresh_surfaces||
       !g_editor||!size||!sh_grid_warp_init(size,&warp)||
       InterlockedCompareExchange(&g_edit_faulted,0,0)||InterlockedCompareExchange(&g_edit_busy,1,0))return 0;
    __try {
        __try {
            /* The palette factory and entity commits allocate through native
             * helpers. A verified heap push also rejects an off-main caller. */
            if(!sh_process_heap_enter(&g_heap,&heap))__leave;
            if(*(void**)(g_editor+0x204d0)!=edit_data||!g_editor[8]||
               !sh_grid_native_read(edit_data,instance,&before)||before.kind!=size->kind)__leave;
            map=*(unsigned char**)edit_data;
            if(*(void**)(g_editor+0x204c8)!=map)__leave;
            records=*(unsigned char**)(map+0x750);
            n=*(int*)(map+0x758);if(n<1||n>4096||!records)__leave;
            accepted=*size;
            if(!fit_size(map,instance,&before,&accepted,&reason))__leave;
            if(memcmp(accepted.xyz,size->xyz,sizeof accepted.xyz))
                backend_log("GRID: dimensions clamped to the native map boundary");
            size=&accepted;
            if(!memcmp(before.xyz,size->xyz,sizeof before.xyz)){ok=1;__leave;}
            keys=*(int**)(map+0x7d8);doors=*(int**)(map+0x778);
            frames=*(int**)(map+0x790);connections=*(int**)(map+0x7c0);
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
            if(!plan_connected(map,instance,wrapper,moves,n,old_connections,ports,&reason))__leave;
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
            reason="GRID: native placement is invalid; restoring the previous layout";
            records=*(unsigned char**)(map+0x750);
            for(i=0;i<n;++i)if(moves[i].assigned&&!records[(size_t)i*0x98+0x30])__leave;
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
                if(!restored){InterlockedExchange(&g_edit_faulted,1);
                    backend_log("GRID: edit recovery incomplete; further dimension edits disabled");}
            }
            free(moves);free(old_connections);
            __try {
                for(i=0;i<light_count;++i){destroy_tree(&lights[i].old_tree);destroy_tree(&lights[i].new_tree);}
            } __finally {
                if(!sh_process_heap_leave(&heap)){ok=0;InterlockedExchange(&g_edit_faulted,1);}
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_edit_faulted,1);ok=0;
        reason="GRID: native size edit faulted; further dimension edits disabled";}
    InterlockedExchange(&g_edit_busy,0);
    backend_log(ok?"GRID: selected room dimensions updated":reason);return ok;
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
    if(g_entity_tree){const unsigned char *p=(const unsigned char*)g_entity_tree;int32_t displacement;
        /* Both verified getters contain the owning tree's cleanup sequence at
         * these offsets. Use its exact destructor and matching native delete. */
        if(p[0x46]==0xe8&&p[0x53]==0xe8&&!memcmp(p+0x4b,"\xba\x48\0\0\0\x48\x8b\xcf",8)){
            memcpy(&displacement,p+0x47,4);g_tree_destroy=(tree_destroy_fn)(p+0x4b+displacement);
            memcpy(&displacement,p+0x54,4);g_tree_free=(tree_free_fn)(p+0x58+displacement);
        }
    }
    g_editor=(unsigned char*)glb_resolve(base,"editor_singleton",NULL);
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
