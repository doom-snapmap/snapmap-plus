/* Snapmap+ tab of the native Settings / Properties panel.
 *
 * Build 13954591 evidence (snaphak-re editor-fixes-13954591 R-004):
 *  - Populate(panel, category) VK 0x1282db0 builds category 0 (editor
 *    settings) or 1 (object states). Any other value adds no native rows and
 *    falls through to the shared tail, which divides by the row count
 *    (FUN 0x1284810: index % rows), so a category without rows must not reach
 *    the tail. Rows are therefore injected right after the panel clear call
 *    inside Populate (VK 0x1282e23), before the tail runs.
 *  - The count 2 is compiled into the category cycle (VK 0x118f2d0, LB/RB),
 *    the notch select (VK 0x118f360, '< 2' and 'and 0x80000001') and the
 *    notch loops in Populate (notch_0..1 shown, notch_2..16 hidden). The SWF
 *    holds 17 notch sprites and hitboxes (panel init VK 0x127ecb0), so a third
 *    diamond only needs showing. The settings state keeps its category at
 *    state+0xc and passes it to Populate on Enter.
 *  - Apply / Reset copy the whole profile controller (editor+0x204d8); they
 *    are not scoped per category, so map-owned drafts are committed here.
 *  - The panel has no label-only row. RenderAddTitle (VK 0x12807b0, GL
 *    0x127fd60) instantiates "title_group_template" at y = rows * 0x50, where
 *    rows is the panel's list-row count (+0x200), and keeps it in a separate
 *    title list (+0x228); rows (RenderAddFloat, VK 0x127dbe0) are placed by
 *    the same count. Native Populate adds exactly one title before any row,
 *    which is the category header. A title added after rows is drawn over a
 *    row and a second title before rows over the header (R-006, R-007), so the
 *    tab adds one header title and its groups add rows only.
 *  - The float step (idMenuPropertySliderInput VK 0x12a4370) adds the small
 *    or large step to the value at +0x120 and, past a limit, goes to the
 *    other limit when the row's wrap flag (+0x10c, RenderAddFloat's last
 *    argument) is set, else stops at the limit. Native rows pass 1; the tab
 *    passes 0 so a step can never land on the opposite end (R-009).
 *  - Toggle rows: RenderAddToggle (VK 0x12808f0, GL 0x127fea0) creates an
 *    idMenuPropertyToggle whose value is the byte at +0xd0; its vtable +0x20
 *    stores the property id at +0x40 and +0x28 sets the value and redraws the
 *    true/false text. The widget flips +0xd0 itself before the panel's
 *    property-change dispatcher runs (VK 0x1282450 reads it for ids 7, 0xc..). */
#include "settings_tab.h"
#include <math.h>

float sh_settings_tab_unwrap(float previous,float value,float min,float max,float small_step,float large_step)
{
    float step=fmaxf(fabsf(small_step),fabsf(large_step));
    if(!isfinite(previous)||!isfinite(value)||!isfinite(min)||!isfinite(max)||!(max>min))return value;
    /* A real step moves at most one large step; a wrap jumps across the
     * range from one end to the other. Ranges too small to tell are left. */
    if(!(step>0)||max-min<=2*step||fabsf(value-previous)<=step)return value;
    if(previous<=min+step&&value>=max-step&&value>previous)return min;
    if(previous>=max-step&&value<=min+step&&value<previous)return max;
    return value;
}

int sh_settings_tab_wrap(int category,int delta,int count)
{
    long long c;
    if(count<1)return 0;
    c=((long long)category+delta)%count;
    if(c<0)c+=count;
    return (int)c;
}

#ifndef SH_SETTINGS_TAB_PURE
#include "grid_room_editor.h"
#include "hook.h"
#include "patch.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

void backend_log(const char *message);

#define ROW_ID_BASE 0x534d5400u
#define TAB_CATEGORY 2

typedef void (*state_fn)(void *,void *);
typedef void (*populate_fn)(void *,int);
typedef void (*controller_fn)(void *);
typedef unsigned char (*dirty_fn)(void *);
typedef void *(*float_fn)(void *,const char *,const uint32_t *,const uint32_t *,
    unsigned char,float,float,float,float,float,unsigned char);
typedef void (*title_fn)(void *,const char *);
typedef void (*toggle_fn)(void *,unsigned,const char *,const uint32_t *,const uint32_t *,
    unsigned char,unsigned char,const char *,const char *);
typedef void (*toggle_set_fn)(void *,unsigned char);
typedef void (*float_set_fn)(void *,float);
typedef uint32_t (*hash_fn)(const char *);
typedef void (*clear_fn)(void *);
typedef void (*category_fn)(void *,void *,void *,int);
typedef void (*repopulate_fn)(void *,int);
typedef void (*ui_event_fn)(void *,const int *,unsigned char);
typedef void *(*lookup_fn)(void *,void *,const char *,const char *,const char *,void *,void *,void *);
typedef void *(*sprite_fn)(void *);
typedef void (*destroy_fn)(void *);
typedef int (*label_fn)(void *,const char *);
typedef void (*play_fn)(void *,int);
typedef void (*visible_fn)(void *,unsigned char,unsigned char);

static state_fn g_enter,g_exit;
static populate_fn g_populate;
static controller_fn g_apply,g_reset;
static dirty_fn g_dirty;
static float_fn g_add;
static title_fn g_title;
static toggle_fn g_toggle;
static hash_fn g_hash;
static clear_fn g_clear;
static void *g_cycle_tramp,*g_select_tramp;
static repopulate_fn g_repopulate;
static ui_event_fn g_ui_event;
static lookup_fn g_lookup;
static sprite_fn g_sprite;
static destroy_fn g_destroy;
static label_fn g_label;
static play_fn g_play;
static visible_fn g_visible;
static sh_patch_handle g_clear_patch;
static void *g_clear_relay;

static sh_settings_tab_mode g_mode;
static LONG g_faulted;
static void *g_editor,*g_map,*g_panel,*g_manager;
static int g_session,g_count=2,g_inject;

typedef struct group_slot {sh_settings_group desc;int active;} group_slot;
static group_slot g_groups[SH_SETTINGS_TAB_MAX_GROUPS];
static int g_group_count;

enum {ROW_FLOAT,ROW_BOOL};
typedef struct row_slot {
    int kind;
    sh_settings_float desc;float *draft;float *base;
    int *bool_draft,*bool_base;
    void (*adjust)(void *,float *,float);int group;void *widget;
} row_slot;
static row_slot g_rows[SH_SETTINGS_TAB_MAX_ROWS];
static int g_row_count;
struct sh_settings_rows {void *panel;int group;};

sh_settings_tab_mode sh_settings_tab_current_mode(void){return g_mode;}

int sh_settings_tab_register_group(const sh_settings_group *group)
{
    if(!group||!group->populate||g_group_count>=SH_SETTINGS_TAB_MAX_GROUPS)return 0;
    g_groups[g_group_count].desc=*group;g_groups[g_group_count].active=0;g_group_count++;
    return 1;
}
int sh_settings_tab_register(const char *title,sh_settings_populate_fn populate)
{
    sh_settings_group group;memset(&group,0,sizeof group);
    group.title=title;group.populate=populate;
    return sh_settings_tab_register_group(&group);
}

static int active(void)
{
    unsigned char *e=(unsigned char*)g_editor;
    return g_mode!=SH_SETTINGS_TAB_OFF&&g_session&&!InterlockedCompareExchange(&g_faulted,0,0)&&e&&
        e[8]&&*(void**)(e+0x204c8)==g_map&&*(int*)(e+0x23618)==5;
}
static void fault(const char *where)
{
    if(!InterlockedExchange(&g_faulted,1)) {
        char line[128];snprintf(line,sizeof line,"SETTINGS: Snapmap+ tab disabled after an engine exception in %s",where);
        backend_log(line);
    }
}
static int row_changed(const row_slot *r)
{
    return r->kind==ROW_BOOL?(*r->bool_draft!=0)!=(*r->bool_base!=0):*r->draft!=*r->base;
}
static void row_keep(row_slot *r){if(r->kind==ROW_BOOL)*r->bool_base=*r->bool_draft;else *r->base=*r->draft;}
static void row_revert(row_slot *r){if(r->kind==ROW_BOOL)*r->bool_draft=*r->bool_base;else *r->draft=*r->base;}
static void refresh_values(void)
{
    int i;
    for(i=0;i<g_row_count;i++)if(g_rows[i].widget) {
        unsigned char *v=(unsigned char*)g_rows[i].widget;
        if(g_rows[i].kind==ROW_BOOL) {
            unsigned char on=(unsigned char)(*g_rows[i].bool_draft!=0);
            /* vtable +0x28: set the value and redraw the true/false text. */
            if(v[0xd0]!=on)(*(toggle_set_fn*)(*(uintptr_t*)v+0x28))(v,on);
        } else if(*(float*)(v+0x120)!=*g_rows[i].draft) {
            /* vtable +0xb8 (VK 0x12a7860): store +0x120 and reformat the
             * row text. A bare +0x120 write left the old text on screen, so a
             * clamped or snapped draft was shown as the native value (R-009). */
            (*(float_set_fn*)(*(uintptr_t*)v+0xb8))(v,*g_rows[i].draft);
        }
        v[0xc8]=(unsigned char)row_changed(&g_rows[i]);
    }
}

int sh_settings_tab_add_float(sh_settings_rows *rows,const sh_settings_float *row,float *draft,
                              float *base,void (*adjust)(void *,float *,float))
{
    uint32_t label,help;unsigned char *v;int i;
    if(!rows||!row||!draft||!base||!g_add)return 0;
    for(i=0;i<g_row_count;i++)if(g_rows[i].kind==ROW_FLOAT&&g_rows[i].draft==draft)break;
    if(i==g_row_count) {
        if(g_row_count>=SH_SETTINGS_TAB_MAX_ROWS)return 0;
        g_row_count++;
    }
    memset(&g_rows[i],0,sizeof g_rows[i]);
    g_rows[i].kind=ROW_FLOAT;g_rows[i].desc=*row;g_rows[i].draft=draft;g_rows[i].base=base;
    g_rows[i].adjust=adjust;g_rows[i].group=rows->group;g_rows[i].widget=NULL;
    label=g_hash(row->label);help=g_hash(row->help);
    /* Last argument 0: the native step stops at min / max instead of wrapping. */
    v=(unsigned char*)g_add(rows->panel,row->name,&label,&help,1,*base,
        row->min,row->max,row->small_step,row->large_step,0);
    if(!v)return 0;
    *(unsigned*)(v+0x40)=ROW_ID_BASE+(unsigned)i;g_rows[i].widget=v;
    return 1;
}

int sh_settings_tab_add_bool(sh_settings_rows *rows,const sh_settings_bool *row,int *draft,int *base)
{
    uint32_t label,help;unsigned char *panel,*v;int i,before,after;unsigned id;
    if(!rows||!row||!draft||!base||!g_toggle||!g_hash)return 0;
    for(i=0;i<g_row_count;i++)if(g_rows[i].kind==ROW_BOOL&&g_rows[i].bool_draft==draft)break;
    if(i==g_row_count) {
        if(g_row_count>=SH_SETTINGS_TAB_MAX_ROWS)return 0;
        g_row_count++;
    }
    memset(&g_rows[i],0,sizeof g_rows[i]);
    g_rows[i].kind=ROW_BOOL;g_rows[i].bool_draft=draft;g_rows[i].bool_base=base;
    g_rows[i].group=rows->group;
    g_rows[i].desc.name=row->name;g_rows[i].desc.label=row->label;g_rows[i].desc.help=row->help;
    g_rows[i].desc.min=0;g_rows[i].desc.max=1;
    label=g_hash(row->label);help=g_hash(row->help);
    panel=(unsigned char*)rows->panel;id=ROW_ID_BASE+(unsigned)i;
    before=*(int*)(panel+0x200);
    /* NULL texts keep the widget's own #str_snapproperty_boolean_true/false. */
    g_toggle(panel,id,row->name,&label,&help,1,(unsigned char)(*base!=0),NULL,NULL);
    after=*(int*)(panel+0x200);
    if(after!=before+1||after<1)return 0;
    v=(*(unsigned char***)(panel+0x1f8))[after-1];
    if(!v||*(unsigned*)(v+0x40)!=id)return 0;
    g_rows[i].widget=v;
    return 1;
}

/* Every active group's rows, in registration order. A header title is only
 * added to an empty panel (the tab's own category): a title placed after rows
 * would be drawn over them. Group titles are not drawn (see the header). */
static void add_groups(void *panel,const char *heading)
{
    int i;
    __try {
        if(heading)g_title(panel,heading);
        for(i=0;i<g_group_count;i++)if(g_groups[i].active) {
            sh_settings_rows rows;rows.panel=panel;rows.group=i;
            g_groups[i].desc.populate(&rows,g_groups[i].desc.user);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("populate");}
}

/* Replaces the panel clear call inside native Populate only. */
static void clear_relay(void *panel)
{
    g_clear(panel);
    if(g_inject==2&&panel==g_panel&&active())add_groups(panel,SH_SETTINGS_TAB_TITLE);
}

static void show_tab_notch(void *panel,int category)
{
    unsigned char value[16];void *sprite;
    memset(value,0,sizeof value);
    __try {
        g_lookup(*(void**)((unsigned char*)panel+8),value,"categories","category_nav","notch_2",NULL,NULL,NULL);
        sprite=g_sprite(value);g_destroy(value);
        if(!sprite)return;
        g_play(sprite,g_label(sprite,category==TAB_CATEGORY?"on":"idle"));
        g_visible(sprite,1,1);
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("notch");}
}

static void populate(void *panel,int category)
{
    int i;
    g_panel=panel;
    for(i=0;i<g_row_count;i++)g_rows[i].widget=NULL;
    g_inject=0;
    if(active()) {
        if(g_mode==SH_SETTINGS_TAB_CATEGORY&&g_count>TAB_CATEGORY&&category==TAB_CATEGORY)g_inject=2;
    }
    g_populate(panel,category);
    g_inject=0;
    __try {
        if(!active())return;
        if(g_mode==SH_SETTINGS_TAB_GROUP_END&&category==0) {
            void *list;
            add_groups(panel,NULL);
            list=*(void**)((unsigned char*)panel+0x258);
            if(list)((void(*)(void*))(*(void***)list)[3])(list);
        }
        if(g_mode==SH_SETTINGS_TAB_CATEGORY&&g_count>TAB_CATEGORY)show_tab_notch(panel,category);
        refresh_values();
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("populate tail");}
}

static void enter(void *state,void *editor)
{
    int i,any=0;
    g_session=0;g_editor=editor;g_panel=NULL;g_row_count=0;g_count=2;
    __try {
        unsigned char *e=(unsigned char*)editor;
        g_map=*(void**)(e+0x204c8);
        g_manager=*(void**)(*(unsigned char**)(e+0x21088)+0x9e0);
        if(g_mode!=SH_SETTINGS_TAB_OFF&&!InterlockedCompareExchange(&g_faulted,0,0)&&g_map)
            for(i=0;i<g_group_count;i++) {
                g_groups[i].active=!g_groups[i].desc.begin||
                    g_groups[i].desc.begin(g_groups[i].desc.user,editor,g_map);
                any|=g_groups[i].active;
            }
        g_session=any;
        if(g_session&&g_mode==SH_SETTINGS_TAB_CATEGORY)g_count=3;
        /* A category remembered from a session with the tab must not reach
         * native Populate when this session has none. */
        if(*(int*)((unsigned char*)state+0xc)<0||*(int*)((unsigned char*)state+0xc)>=g_count)
            *(int*)((unsigned char*)state+0xc)=0;
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("enter");g_session=0;g_count=2;}
    g_enter(state,editor);
}
static void leave(void *state,void *editor)
{
    int i;
    __try {
        for(i=0;i<g_group_count;i++)if(g_groups[i].active&&g_groups[i].desc.end)g_groups[i].desc.end(g_groups[i].desc.user);
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("exit");}
    for(i=0;i<g_group_count;i++)g_groups[i].active=0;
    g_session=0;g_panel=NULL;g_map=NULL;g_row_count=0;g_count=2;
    g_exit(state,editor);
}

static int changed(void *panel,void *widget,int action)
{
    unsigned id;int handled=0;(void)action;
    __try {
        id=*(unsigned*)((unsigned char*)widget+0x40);
        if(id<ROW_ID_BASE||id>=ROW_ID_BASE+SH_SETTINGS_TAB_MAX_ROWS)return 0;
        handled=1;id-=ROW_ID_BASE;
        if(active()&&panel==g_panel&&(int)id<g_row_count&&widget==g_rows[id].widget) {
            row_slot *r=&g_rows[id];
            if(r->kind==ROW_BOOL) {
                /* The toggle flipped its own +0xd0 before this dispatch. */
                *r->bool_draft=((unsigned char*)widget)[0xd0]!=0;
            } else {
                /* Rows are created without the native wrap, so the value is
                 * already inside [min, max]; adjust may snap or clamp it. */
                float value=*(float*)((unsigned char*)widget+0x120);
                if(r->adjust)r->adjust(g_groups[r->group].desc.user,r->draft,value);
                else if(isfinite(value))*r->draft=fminf(r->desc.max,fmaxf(r->desc.min,value));
            }
            refresh_values();
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("change");}
    return handled;
}
static int group_changed(int group)
{
    int i;for(i=0;i<g_row_count;i++)if(g_rows[i].group==group&&row_changed(&g_rows[i]))return 1;
    return 0;
}
static unsigned char dirty(void *manager)
{
    unsigned char result=g_dirty(manager);int i;
    __try {
        if(manager==g_manager&&active())
            for(i=0;i<g_group_count;i++)if(g_groups[i].active&&group_changed(i))return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("dirty");}
    return result;
}
static void apply(void *controller)
{
    int i,j;
    g_apply(controller);
    __try {
        if(!active()||controller!=(unsigned char*)g_editor+0x204d8)return;
        for(i=0;i<g_group_count;i++)if(g_groups[i].active&&group_changed(i)) {
            if(g_groups[i].desc.commit&&!g_groups[i].desc.commit(g_groups[i].desc.user,g_map))continue;
            for(j=0;j<g_row_count;j++)if(g_rows[j].group==i)row_keep(&g_rows[j]);
        }
        refresh_values();
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("apply");}
}
static void reset(void *controller)
{
    int i;
    g_reset(controller);
    __try {
        if(!active()||controller!=(unsigned char*)g_editor+0x204d8)return;
        for(i=0;i<g_row_count;i++)row_revert(&g_rows[i]);
        refresh_values();
    } __except(EXCEPTION_EXECUTE_HANDLER){fault("reset");}
}

/* Replacements for the two category selectors; their trampolines are never
 * executed. Event 0x31 is the native category-change sound. */
static void category_cycle(void *state,void *editor,void *manager,int delta)
{
    static const int event=0x31;int c;
    g_ui_event(editor,&event,0);
    c=sh_settings_tab_wrap(*(int*)((unsigned char*)state+0xc),delta,g_count);
    *(int*)((unsigned char*)state+0xc)=c;
    g_repopulate(manager,c);
}
static void category_select(void *state,void *editor,void *manager,int index)
{
    static const int event=0x31;
    if(index<0||index>=g_count||*(int*)((unsigned char*)state+0xc)==index)return;
    g_ui_event(editor,&event,0);
    *(int*)((unsigned char*)state+0xc)=index;
    g_repopulate(manager,index);
}

static const sig_result *binding(const sig_result *r,size_t n,const char *name)
{
    size_t i;for(i=0;i<n;i++)if(r[i].name&&!strcmp(r[i].name,name)&&r[i].status==SIG_OK)return r+i;
    return NULL;
}
static void *near_relay(uintptr_t call,void *handler)
{
    SYSTEM_INFO info;uintptr_t step,center,delta;
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

/* Redirect Populate's clear call through a relay; verify it targets the clear. */
static int install_injection(const sig_result *results,size_t count)
{
    const sig_result *site=binding(results,count,"SettingsPanelClearCall"),*clear=binding(results,count,"SettingsPanelClear");
    unsigned char before[5],after[5];int32_t rel;intptr_t distance;
    if(!site||!clear)return 0;
    memcpy(before,(void*)site->addr,5);if(before[0]!=0xe8)return 0;
    memcpy(&rel,before+1,4);
    if(site->addr+5+(intptr_t)rel!=clear->addr)return 0;
    g_clear=(clear_fn)clear->addr;
    g_clear_relay=near_relay(site->addr,(void*)clear_relay);if(!g_clear_relay)return 0;
    distance=(intptr_t)g_clear_relay-(intptr_t)(site->addr+5);
    if(distance<INT32_MIN||distance>INT32_MAX)goto fail;
    rel=(int32_t)distance;after[0]=0xe8;memcpy(after+1,&rel,4);
    if(code_patch_call_sig(site,before,after,&g_clear_patch)!=B2_PATCH_OK)goto fail;
    return 1;
fail:
    if(!g_clear_patch.live){VirtualFree(g_clear_relay,0,MEM_RELEASE);g_clear_relay=NULL;}
    return 0;
}

static int install_category(const sig_result *results,size_t count)
{
    static const char *names[]={"SettingsCategoryCycle","SettingsCategorySelect","SettingsRepopulate",
        "EditorUiEvent","SwfNestedValue","SwfSpriteInstance","SwfValueDestroy","SwfFrameLabel",
        "SwfPlayFrame","SwfSpriteSetVisible"};
    const sig_result *r[10];unsigned i;
    for(i=0;i<10;i++){r[i]=binding(results,count,names[i]);if(!r[i])return 0;}
    g_repopulate=(repopulate_fn)r[2]->addr;g_ui_event=(ui_event_fn)r[3]->addr;
    g_lookup=(lookup_fn)r[4]->addr;g_sprite=(sprite_fn)r[5]->addr;g_destroy=(destroy_fn)r[6]->addr;
    g_label=(label_fn)r[7]->addr;g_play=(play_fn)r[8]->addr;g_visible=(visible_fn)r[9]->addr;
    /* Whole-instruction prologues: cycle 5+5+1+4, select 4+2+5+5 bytes. */
    g_cycle_tramp=hook_prepare((void*)r[0]->addr,(void*)category_cycle,15);
    g_select_tramp=g_cycle_tramp?hook_prepare((void*)r[1]->addr,(void*)category_select,16):NULL;
    if(!g_cycle_tramp||!g_select_tramp)goto fail;
    if(hook_commit(g_cycle_tramp)!=B2_PATCH_OK||hook_commit(g_select_tramp)!=B2_PATCH_OK)goto fail;
    return 1;
fail:
    if(g_select_tramp&&hook_unpatch(g_select_tramp))g_select_tramp=NULL;
    if(g_cycle_tramp&&hook_unpatch(g_cycle_tramp))g_cycle_tramp=NULL;
    return 0;
}

int sh_settings_tab_install(const sig_result *results,size_t count)
{
    static const char *names[]={"RenderSettingsEnter","RenderSettingsExit","RenderSettingsPopulate",
        "RenderSettingsApply","RenderSettingsReset","RenderSettingsDirtyCall","RenderAddFloat","RenderAddTitle","StridsHash"};
    const sig_result *r[9];void **originals[]={(void**)&g_enter,(void**)&g_exit,(void**)&g_populate,
        (void**)&g_apply,(void**)&g_reset,(void**)&g_dirty};
    void *handlers[]={(void*)enter,(void*)leave,(void*)populate,(void*)apply,(void*)reset,(void*)dirty};
    const size_t stolen[]={15,16,20,16,20,16};unsigned i;uintptr_t address;int32_t rel;
    static const unsigned char dirty_prefix[]={0x48,0x89,0x5c,0x24,8,0x57,0x48,0x83,0xec,0x20,0x8b,0x51,8,0x48,0x8b,0xf9};
    if(g_mode!=SH_SETTINGS_TAB_OFF)return 1;
    for(i=0;i<6;i++)if(*originals[i])return 0;
    for(i=0;i<9;i++){r[i]=binding(results,count,names[i]);if(!r[i])goto unavailable;}
    g_add=(float_fn)r[6]->addr;g_title=(title_fn)r[7]->addr;g_hash=(hash_fn)r[8]->addr;
    {const sig_result *toggle=binding(results,count,"RenderAddToggle");g_toggle=toggle?(toggle_fn)toggle->addr:NULL;}
    if(!g_toggle)backend_log("SETTINGS: toggle rows unavailable on this build");
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
    /* Without the third category the rows go to the end of category 0: rows
     * injected before its native title would push that title over them. */
    g_mode=SH_SETTINGS_TAB_GROUP_END;
    if(install_injection(results,count)) {
        if(install_category(results,count))g_mode=SH_SETTINGS_TAB_CATEGORY;
        else if(code_unpatch(&g_clear_patch)==B2_PATCH_OK){VirtualFree(g_clear_relay,0,MEM_RELEASE);g_clear_relay=NULL;}
    }
    backend_log(g_mode==SH_SETTINGS_TAB_CATEGORY?"SETTINGS: Snapmap+ tab installed as a third category":
                "SETTINGS: Snapmap+ settings installed at the end of the first category");
    return 1;
fail:
    for(i=6;i-->0;)if(*originals[i]&&hook_unpatch(*originals[i]))*originals[i]=NULL;
unavailable:
    backend_log("SETTINGS: Snapmap+ settings tab unavailable (panel hooks missing or already owned)");
    return 0;
}
#endif
