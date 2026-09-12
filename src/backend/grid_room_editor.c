/* Extend DOOM's native module properties, including Blueprint selection. */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "grid_room_editor.h"
#include "hook.h"

void backend_log(const char *message);

#define GRID_PROPERTY_SIZE 0x53475244
#define GRID_PROPERTIES_ACTION 0x50

typedef void (*populate_fn)(void *,int,void *,void *,void *);
typedef void (*change_fn)(void *,void *,int);
typedef void (*add_vec_fn)(void *,int,const char *,const uint32_t *,const uint32_t *,
    unsigned char,const float *,float,float,float,float,unsigned char,void *,const char *,const void *);
typedef void (*set_vec_fn)(void *,const float *);
typedef uint32_t (*hash_fn)(const char *);
typedef void (*blueprint_update_fn)(void *,void *,void *);
typedef void (*blueprint_help_fn)(void *,void *);
typedef unsigned char (*pressed_fn)(void *,int);
typedef unsigned char (*blocked_fn)(void *,unsigned);
typedef void (*set_state_fn)(void *,int);
typedef void (*action_help_fn)(void *,int,int);

static populate_fn g_populate;
static change_fn g_change;
static add_vec_fn g_add_vec;
static set_vec_fn g_set_vec;
static hash_fn g_hash;
static sh_grid_editor_read_fn g_read;
static sh_grid_editor_apply_fn g_apply;
static LONG g_failed;
static blueprint_update_fn g_blueprint_update;
static blueprint_help_fn g_blueprint_help;
static pressed_fn g_pressed;
static blocked_fn g_blocked;
static set_state_fn g_set_state;
static action_help_fn g_action_help;
static sh_native_property_handler g_property_handler;

int sh_grid_editor_set_property_handler(sh_native_property_handler handler)
{
    if(!g_change||!hook_is_installed((void*)g_change)||g_property_handler)return 0;
    g_property_handler=handler;return 1;
}

static int blueprint_grid(void *normal,void *editor)
{
    unsigned char *e=(unsigned char*)editor,*data;sh_grid_size size;int instance;
    if(InterlockedCompareExchange(&g_failed,0,0)||*(int*)(e+0x23618)!=1||
       *(int*)(e+0x212b8+0x2c)!=1||(*(unsigned char*)((char*)normal+0x10)&0x10))return -1;
    data=*(unsigned char**)(e+0x204d0);if(!data)return -1;
    instance=*(int*)(data+0x20);
    if(instance<0||!g_read(data,instance,&size)||g_blocked(e+0x20990,0x1000))return -1;
    return instance;
}

static int grid_open_blueprint(unsigned char *e,int instance)
{
    int count=*(int*)(e+0x21078),*stack=*(int**)(e+0x21070);
    unsigned capacity=*(unsigned*)(e+0x2107c)&0x3fffffff;
    if(count<0||count>1024||(count&&(!stack||(unsigned)count>capacity)))return 0;
    /* Match the native properties selection and replace its stack top, then
     * run the native Exit/Enter lifecycle. OpenProperties also requests the
     * Object camera; Blueprint must retain its existing camera instead. An
     * empty stack closes through the native camera-dependent default state. */
    *(int*)(e+0x22ef8)=instance;
    if(count)stack[count-1]=4;
    g_set_state(e,4);
    return *(int*)(e+0x23618)==4;
}

static void grid_blueprint_update(void *normal,void *mode,void *editor)
{
    unsigned char *e=(unsigned char*)editor;int instance;
    /* This one-frame refresh flag is consumed by the native handler. Do not
     * open a panel in a frame native selection processing deliberately skips. */
    unsigned char refresh=e[0x2120c];
    g_blueprint_update(normal,mode,editor);
    if(refresh)return;
    __try {
        instance=blueprint_grid(normal,editor);
        if(instance>=0&&g_pressed(e+0x20,GRID_PROPERTIES_ACTION)) {
            /* Runs before EditorFrame clears the engine's input edges. */
            if(grid_open_blueprint(e,instance))backend_log("GRID: Blueprint opened selected room properties");
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_failed,1);
        backend_log("GRID: Blueprint properties shortcut disabled after an engine exception");
    }
}

static void grid_blueprint_help(void *normal,void *editor)
{
    g_blueprint_help(normal,editor);
    __try {
        if(blueprint_grid(normal,editor)>=0)
            g_action_help((unsigned char*)editor+0x211a8,GRID_PROPERTIES_ACTION,0x9f);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_failed,1);
        backend_log("GRID: Blueprint properties prompt disabled after an engine exception");
    }
}

static void grid_changed(void *panel,void *inspector,int action)
{
    unsigned char *p=(unsigned char*)panel,*v=(unsigned char*)inspector;
    sh_grid_size current,next;sh_grid_warp check;
    float dims[3],requested[3];unsigned i;int instance,adjusted=0;void *map;
    /* The null guard at the original entry precedes our detour. Other native
     * IDs always reach their original dispatcher with unchanged arguments. */
    if(g_property_handler&&g_property_handler(panel,inspector,action))return;
    if(*(const int*)(v+0x40)!=GRID_PROPERTY_SIZE){g_change(panel,inspector,action);return;}
    if(InterlockedCompareExchange(&g_failed,0,0))return;
    __try {
        map=*(void**)(p+0x1c0);instance=*(int*)(p+0x1dc);
        if(!g_read(map,instance,&current))return;
        memcpy(requested,v+0x100,sizeof requested);
        if(!sh_grid_clamp(current.kind,requested,&next))goto restore;
        for(i=0;i<3;++i){
            dims[i]=(float)next.xyz[i];
            if(dims[i]!=requested[i])adjusted=1;
        }
        {
            if(!sh_grid_warp_init(&next,&check)||!g_apply(map,instance,&next))goto restore;
            if(!g_read(map,instance,&next))goto restore;
            for(i=0;i<3;++i){dims[i]=(float)next.xyz[i];if(dims[i]!=requested[i])adjusted=1;}
        }
        if(adjusted)g_set_vec(inspector,dims);
        return;
restore:
        for(i=0;i<3;++i)dims[i]=(float)current.xyz[i];
        g_set_vec(inspector,dims);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_failed,1);
        backend_log("GRID: native properties disabled after an engine exception");
    }
}

static void grid_populate(void *panel,int instance,void *map,void *settings,void *editor_settings)
{
    unsigned char *p=(unsigned char*)panel;sh_grid_size size;float dims[3];
    uint32_t label,description;unsigned i;
    g_populate(panel,instance,map,settings,editor_settings);
    if(InterlockedCompareExchange(&g_failed,0,0))return;
    __try {
        /* A rejected native panel enter must not expose stale panel storage. */
        if(*(void**)(p+0x1c0)!=map||*(int*)(p+0x1dc)!=instance||!g_read(map,instance,&size))return;
        for(i=0;i<3;++i)dims[i]=(float)size.xyz[i];
        label=g_hash("#str_smp_grid_room_size");
        description=g_hash(size.kind==SH_GRID_MODERN?
            "#str_smp_grid_room_modern_help":"#str_smp_grid_room_classic_help");
        /* +0x280 is the panel's constructed numeric-entry context, reused by
         * native grid-offset XYZ settings. Its owner outlives this inspector.
         * Per-axis door limits are checked by the edit adapter, while this
         * shared native XYZ inspector supplies the overall numeric range. */
        g_add_vec(panel,GRID_PROPERTY_SIZE,"Grid Room Size",&label,&description,
            1,dims,1,65534,1,16,1,p+0x280,NULL,NULL);
        {void *list=*(void**)(p+0x258);
            if(list)((void(*)(void*))(*(void***)list)[3])(list);}
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_failed,1);
        backend_log("GRID: native properties construction failed");
    }
}

static const sig_result *clean(const sig_result *results,size_t count,const char *name)
{
    size_t i;for(i=0;i<count;++i)if(results[i].name&&!strcmp(results[i].name,name)&&
        results[i].status==SIG_OK)return &results[i];return NULL;
}

int sh_grid_editor_install(const sig_result *results,size_t count,
                            sh_grid_editor_read_fn read,sh_grid_editor_apply_fn apply)
{
    const sig_result *populate,*change,*add,*set,*hash,*update,*help,*pressed,*blocked,*state,*action_help;
    if(!read||!apply)return 0;
    if(g_populate&&g_change&&g_blueprint_update&&g_blueprint_help&&
       hook_is_installed((void*)g_populate)&&hook_is_installed((void*)g_change)&&
       hook_is_installed((void*)g_blueprint_update)&&hook_is_installed((void*)g_blueprint_help))return 1;
    if(g_populate||g_change||g_blueprint_update||g_blueprint_help)return 0;
    populate=clean(results,count,"GridModuleProperties");change=clean(results,count,"GridPropertyChanged");
    add=clean(results,count,"GridAddVec3");set=clean(results,count,"GridSetVec3");hash=clean(results,count,"StridsHash");
    update=clean(results,count,"GridBlueprintUpdate");help=clean(results,count,"GridBlueprintHelp");
    pressed=clean(results,count,"GridInputPressed");blocked=clean(results,count,"GridPropertiesBlocked");
    state=clean(results,count,"GridSetEditorState");action_help=clean(results,count,"GridAddActionHelp");
    if(!populate||!change||!add||!set||!hash||!update||!help||!pressed||!blocked||!state||!action_help)return 0;
    g_read=read;g_apply=apply;g_add_vec=(add_vec_fn)add->addr;g_set_vec=(set_vec_fn)set->addr;g_hash=(hash_fn)hash->addr;
    g_pressed=(pressed_fn)pressed->addr;g_blocked=(blocked_fn)blocked->addr;
    g_set_state=(set_state_fn)state->addr;g_action_help=(action_help_fn)action_help->addr;
    /* Populate steals 16 register/stack-only bytes. Change's entry has a null
     * branch, so hook its non-null body at +9 and steal 17 position-independent
     * bytes. The branch itself and its destination remain untouched. */
    g_change=(change_fn)hook_prepare((void*)(change->addr+9),(void*)grid_changed,17);
    g_populate=(populate_fn)hook_prepare((void*)populate->addr,(void*)grid_populate,16);
    /* The normal Blueprint handlers' first 15 bytes are complete instructions
     * without relative operands, independently checked in both renderers. */
    g_blueprint_update=(blueprint_update_fn)hook_prepare((void*)update->addr,(void*)grid_blueprint_update,15);
    g_blueprint_help=(blueprint_help_fn)hook_prepare((void*)help->addr,(void*)grid_blueprint_help,15);
    if(g_change&&g_populate&&g_blueprint_update&&g_blueprint_help&&
        hook_commit((void*)g_change)==B2_PATCH_OK&&hook_commit((void*)g_populate)==B2_PATCH_OK&&
        hook_commit((void*)g_blueprint_update)==B2_PATCH_OK&&hook_commit((void*)g_blueprint_help)==B2_PATCH_OK){
        backend_log("GRID: native module dimension inspector and Blueprint X shortcut installed");return 1;}
    if(g_blueprint_help&&hook_unpatch((void*)g_blueprint_help))g_blueprint_help=NULL;
    if(g_blueprint_update&&hook_unpatch((void*)g_blueprint_update))g_blueprint_update=NULL;
    if(g_populate&&hook_unpatch((void*)g_populate))g_populate=NULL;
    if(g_change&&hook_unpatch((void*)g_change))g_change=NULL;
    return 0;
}
