/* Extend Settings / Properties while retaining native Apply and Cancel. */
#include "map_render.h"
#include "grid_room_editor.h"
#include "hook.h"
#include <windows.h>
#include <string.h>

void backend_log(const char *message);
#define RENDER_PROPERTY 0x534d5000u
typedef void (*state_fn)(void *,void *);
typedef void (*populate_fn)(void *,int);
typedef void (*controller_fn)(void *);
typedef unsigned char (*dirty_fn)(void *);
typedef void *(*float_fn)(void *,const char *,const uint32_t *,const uint32_t *,
    unsigned char,float,float,float,float,float,unsigned char);
typedef void (*title_fn)(void *,const char *);
typedef uint32_t (*hash_fn)(const char *);
static state_fn g_enter,g_exit;
static populate_fn g_populate;
static controller_fn g_apply,g_reset;
static dirty_fn g_dirty;
static float_fn g_add;
static title_fn g_title;
static hash_fn g_hash;
static void *g_editor,*g_map,*g_panel,*g_manager;
static sh_map_render g_base,g_draft;
static int g_active,g_installed;
static LONG g_faulted;
static void *g_widgets[SH_RENDER_FIELDS];

typedef struct field_info {const char *name,*label,*help;float min,max,small_step,large_step;} field_info;
static const field_info fields[SH_RENDER_FIELDS]={
    {"View Distance","#str_smp_render_distance","#str_smp_render_distance_help",0,200000,256,4096},
    {"Fog Strength","#str_smp_render_strength","#str_smp_render_strength_help",0,100,1,10},
    {"Fog Start","#str_smp_render_start","#str_smp_render_start_help",0,199999,100,1000},
    {"Fog End","#str_smp_render_end","#str_smp_render_end_help",1,200000,100,1000},
    {"Fog Red","#str_smp_render_red","#str_smp_render_color_help",0,1,.01f,.1f},
    {"Fog Green","#str_smp_render_green","#str_smp_render_color_help",0,1,.01f,.1f},
    {"Fog Blue","#str_smp_render_blue","#str_smp_render_color_help",0,1,.01f,.1f}
};
static int active(void)
{
    unsigned char *e=(unsigned char*)g_editor;
    return g_installed&&g_active&&!InterlockedCompareExchange(&g_faulted,0,0)&&e&&
        e[8]&&*(void**)(e+0x204c8)==g_map&&*(int*)(e+0x23618)==5;
}
static void refresh_values(void)
{
    unsigned i;
    for(i=0;i<SH_RENDER_FIELDS;i++)if(g_widgets[i]) {
        unsigned char *v=(unsigned char*)g_widgets[i];
        *(float*)(v+0x120)=g_draft.value[i];
        v[0xc8]=(unsigned char)(g_draft.value[i]!=g_base.value[i]);
    }
}
static int changed(void *panel,void *widget,int action)
{
    unsigned id;int handled=0;(void)action;
    __try {
        id=*(unsigned*)((unsigned char*)widget+0x40);
        if(id<RENDER_PROPERTY||id>=RENDER_PROPERTY+SH_RENDER_FIELDS)return 0;
        handled=1;
        if(active()&&panel==g_panel&&widget==g_widgets[id-RENDER_PROPERTY]) {
            sh_map_render_adjust(&g_draft,id-RENDER_PROPERTY,*(float*)((unsigned char*)widget+0x120));
            refresh_values();
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);}
    return handled;
}
static void enter(void *state,void *editor)
{
    g_active=0;g_editor=editor;g_panel=NULL;memset(g_widgets,0,sizeof g_widgets);
    __try {
        unsigned char *e=(unsigned char*)editor;
        g_map=*(void**)(e+0x204c8);
        g_manager=*(void**)(*(unsigned char**)(e+0x21088)+0x9e0);
        if(g_installed&&sh_map_render_read(g_map,&g_base)){g_draft=g_base;g_active=1;}
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);}
    g_enter(state,editor);
}
static void leave(void *state,void *editor)
{
    g_active=0;g_panel=NULL;g_map=NULL;memset(g_widgets,0,sizeof g_widgets);
    g_exit(state,editor);
}
static void populate(void *panel,int category)
{
    unsigned i;
    g_populate(panel,category);
    __try {
        if(!active())return;
        memset(g_widgets,0,sizeof g_widgets);g_panel=panel;
        if(category!=0)return;
        g_title(panel,"Map Rendering");
        for(i=0;i<SH_RENDER_FIELDS;i++) {
            uint32_t label=g_hash(fields[i].label),help=g_hash(fields[i].help);
            unsigned char *v=(unsigned char*)g_add(panel,fields[i].name,&label,&help,1,g_base.value[i],
                fields[i].min,fields[i].max,fields[i].small_step,fields[i].large_step,1);
            if(!v){InterlockedExchange(&g_faulted,1);return;}
            *(unsigned*)(v+0x40)=RENDER_PROPERTY+i;g_widgets[i]=v;
        }
        refresh_values();
        {void *list=*(void**)((unsigned char*)panel+0x258);
            if(list)((void(*)(void*))(*(void***)list)[3])(list);}
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);}
}
static unsigned char dirty(void *manager)
{
    unsigned char result=g_dirty(manager);
    __try {if(manager==g_manager&&active()&&memcmp(&g_base,&g_draft,sizeof g_base))return 1;}
    __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);}
    return result;
}
static void apply(void *controller)
{
    g_apply(controller);
    __try {
        if(active()&&controller==(unsigned char*)g_editor+0x204d8&&
           memcmp(&g_base,&g_draft,sizeof g_base)&&sh_map_render_write(g_map,&g_draft))g_base=g_draft;
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);}
}
static void reset(void *controller)
{
    g_reset(controller);
    __try {if(active()&&controller==(unsigned char*)g_editor+0x204d8){g_draft=g_base;refresh_values();}}
    __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);}
}
static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}
int sh_map_render_editor_install(const sig_result *results,size_t count)
{
    static const char *names[]={"RenderSettingsEnter","RenderSettingsExit","RenderSettingsPopulate",
        "RenderSettingsApply","RenderSettingsReset","RenderSettingsDirtyCall","RenderAddFloat","RenderAddTitle","StridsHash"};
    const sig_result *r[9];void **originals[]={(void**)&g_enter,(void**)&g_exit,(void**)&g_populate,
        (void**)&g_apply,(void**)&g_reset,(void**)&g_dirty};
    void *handlers[]={(void*)enter,(void*)leave,(void*)populate,(void*)apply,(void*)reset,(void*)dirty};
    const size_t stolen[]={15,16,20,16,20,16};unsigned i;uintptr_t address;int32_t rel;
    static const unsigned char dirty_prefix[]={0x48,0x89,0x5c,0x24,8,0x57,0x48,0x83,0xec,0x20,0x8b,0x51,8,0x48,0x8b,0xf9};
    if(g_installed)return 1;
    for(i=0;i<6;i++)if(*originals[i])return 0;
    for(i=0;i<9;i++){r[i]=binding(results,count,names[i]);if(!r[i])return 0;}
    g_add=(float_fn)r[6]->addr;g_title=(title_fn)r[7]->addr;g_hash=(hash_fn)r[8]->addr;
    for(i=0;i<6;i++) {
        address=r[i]->addr;
        if(i==5) {
            if(*(unsigned char*)address!=0xe8)goto fail;
            memcpy(&rel,(void*)(address+1),4);address+=5+rel;
            if(memcmp((void*)address,dirty_prefix,sizeof dirty_prefix))goto fail;
        }
        *originals[i]=hook_prepare((void*)address,handlers[i],stolen[i]);if(!*originals[i])goto fail;
    }
    for(i=0;i<6;i++)if(hook_commit(*originals[i])!=B2_PATCH_OK)goto fail;
    if(!sh_grid_editor_set_property_handler(changed))goto fail;
    g_installed=1;
    backend_log("RENDER: native Settings / Properties controls installed");return 1;
fail:
    for(i=6;i-->0;)if(*originals[i]&&hook_unpatch(*originals[i]))*originals[i]=NULL;
    backend_log("RENDER: native settings controls unavailable");return 0;
}
