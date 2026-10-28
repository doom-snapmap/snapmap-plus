/* Extend the engine-owned Start menu; the engine owns every widget and sprite. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "nav_menu.h"
#include "apply_engine.h"
#include "nav_bake.h"
#include "hook.h"

void backend_log(const char *message);

#define NAV_ACTION 9
#define STOCK_ROWS 7
#define MENU_ROWS 8
#define ED_SCREEN 0x21088
#define ED_MAP 0x204c8
#define ED_STATE 0x23618
#define ED_START_MENU 0x23260
#define SCREEN_MENU 0x938
#define SCREEN_SHOWN 0xa2c
#define SCREEN_ALLOWED 0xa28
#define MENU_SHOWN 0x40
#define WIDGET_SET_VISIBLE 0xa0
#define BUSY_FLAG 0x400

typedef struct menu_actions {
    int *items;
    int count, capacity;
    unsigned flags;
} menu_actions;
typedef union native_string { uint64_t align; unsigned char bytes[48]; } native_string;
typedef union swf_value { uint64_t align; unsigned char bytes[16]; } swf_value;
typedef void (*bind_fn)(void *,menu_actions *);
typedef unsigned char (*action_fn)(void *,void *,int);
typedef void (*state_fn)(void *,void *);
typedef void (*string_fn)(void *,const char *);
typedef void (*destroy_fn)(void *);
typedef void (*message_fn)(void *,void *,void *);
typedef void (*row_fn)(void *,int,int);
typedef unsigned char (*resolve_fn)(void *);
typedef void *(*child_fn)(void *,int);
typedef void *(*clone_fn)(void *,int,void *);
typedef void *(*entry_fn)(void *,int);
typedef void *(*lookup_fn)(void *,void *,const char *,const char *,void *,void *,void *,void *);
typedef void *(*text_fn)(void *);
typedef int (*label_fn)(void *,const char *);
typedef void (*play_fn)(void *,int);
typedef void (*visible_fn)(void *,char);

static bind_fn g_bind;
static action_fn g_action;
static state_fn g_think,g_end;
static string_fn g_string,g_set_text;
static destroy_fn g_destroy,g_value_destroy,g_clear_busy;
static message_fn g_busy,g_toast;
static row_fn g_row;
static resolve_fn g_resolve;
static child_fn g_child;
static clone_fn g_clone;
static entry_fn g_entry;
static lookup_fn g_lookup;
static text_fn g_text;
static label_fn g_label;
static play_fn g_play;
static int g_enabled,g_failed;
static unsigned char *g_busy_editor,*g_busy_screen;
static void *g_busy_map;
static int g_wait_frames,g_requested;
static ULONGLONG g_busy_since;
static sh_patch_handle g_limits[4];
static unsigned char *g_layout_parent;
/* Per stock entry: the parent timeline's own matrix and the one written over it. */
static float g_layout_matrices[13][6],g_layout_written[13][6];
static unsigned short g_layout_depths[13];
/* The clone shows its "off" frame while the panel stays rolled off. */
static int g_clone_hidden;

/* Undo only displacements that are still ours; a matrix the timeline has
 * since rewritten is already the stock value. */
static void restore_layout(void)
{
    int i;
    if (!g_layout_parent) return;
    if (*(int*)(g_layout_parent+0x88)<=0) { g_layout_parent=NULL;return; }
    for (i=0;i<13;i++) {
        unsigned char *entry=(unsigned char*)g_entry(g_layout_parent,g_layout_depths[i]);
        if (entry && !memcmp(entry+8,g_layout_written[i],sizeof g_layout_written[i]))
            memcpy(entry+8,g_layout_matrices[i],sizeof g_layout_matrices[i]);
    }
    g_layout_parent=NULL;
}

static int actions_valid(const menu_actions *a)
{
    return a && a->items && a->count > 0 && a->count <= MENU_ROWS &&
        a->capacity >= a->count && a->capacity <= 4096;
}

static int action_index(const menu_actions *a)
{
    int i;
    if (!actions_valid(a)) return -1;
    for (i=0;i<a->count;i++) if (a->items[i]==NAV_ACTION) return i;
    return -1;
}

/* The native list retains its allocator and capacity. Rebinding a menu does
 * not append a duplicate; maps with fewer stock actions keep all of them. */
static int add_action(menu_actions *a)
{
    int i,at;
    if (!actions_valid(a)) return -1;
    at=action_index(a);
    if (at>=0) return at;
    if (a->count>=MENU_ROWS || a->count>=a->capacity) return -1;
    at=0;
    for (i=0;i<a->count;i++) if (a->items[i]==3) { at=i+1; break; }
    memmove(a->items+at+1,a->items+at,(size_t)(a->count-at)*sizeof(int));
    a->items[at]=NAV_ACTION;a->count++;
    return at;
}

static void remove_action(menu_actions *a)
{
    int at=action_index(a);
    if (at<0) return;
    memmove(a->items+at,a->items+at+1,(size_t)(a->count-at-1)*sizeof(int));
    a->count--;
}

static unsigned char *row_widget(unsigned char *widget,int index)
{
    void *list=*(void**)(widget+0x1c8);
    return list?(unsigned char*)g_child(list,index):NULL;
}

static void hide_extra(unsigned char *widget)
{
    unsigned char *row=row_widget(widget,STOCK_ROWS);
    if (row && g_resolve(row)) {
        void *sprite=*(void**)(row+8);
        g_play(sprite,g_label(sprite,"rollOff"));
        *(unsigned*)(row+0x1b0)=0x10;
    }
}

/* The parent's closing roll-off advances its timeline forward, which never
 * removes a dynamic clip, and animates only timeline-placed entries. Present
 * the clone's own "off" frame, the one the native binder gives unused rows, so
 * nothing of it outlives the stock rows. Only an attached clone is touched. */
static void hide_clone(unsigned char *widget)
{
    unsigned char *row=row_widget(widget,STOCK_ROWS),*parent,*entry,*sprite;
    if (!row || !g_resolve(widget) || !g_resolve(row)) return;
    parent=*(unsigned char**)(widget+8);sprite=*(unsigned char**)(row+8);
    if (!parent || *(int*)(parent+0x88)<=0) return;
    entry=(unsigned char*)g_entry(parent,0x4200);
    if (!entry || *(void**)(entry+0x48)!=sprite) return;
    g_play(sprite,g_label(sprite,"off"));
    *(unsigned*)(row+0x1b0)=0x10;
    g_clone_hidden=1;
}

/* The screen update shows the panel through the widget's SetVisible slot
 * ("rollOn" on the parent; from a rolled-off panel that is a backward seek,
 * which clears the parent's display list and retires the clone).
 * Begin has already requested it; doing it here, under the same condition the
 * screen applies, makes the rewind happen before the eighth row is attached
 * instead of after the first frame is drawn. */
static void present_panel(unsigned char *editor,unsigned char *widget)
{
    unsigned char *screen;
    if (!editor || !widget) return;
    screen=*(unsigned char**)(editor+ED_SCREEN);
    if (!screen || *(unsigned char**)(screen+SCREEN_MENU)!=widget) return;
    if (!(*(unsigned*)(screen+SCREEN_SHOWN)&*(unsigned*)(screen+SCREEN_ALLOWED)&MENU_SHOWN)) return;
    (*(visible_fn*)(*(unsigned char**)widget+WIDGET_SET_VISIBLE))(widget,1);
}

/* The panel is rolled off, or the screen update is about to roll it off:
 * the widget's visibility byte is clear, or the screen no longer requests the
 * panel this widget presents. */
static int panel_hidden(unsigned char *editor,unsigned char *widget)
{
    unsigned char *screen;
    if (!widget[0x1b0]) return 1;
    screen=editor?*(unsigned char**)(editor+ED_SCREEN):NULL;
    if (!screen || *(unsigned char**)(screen+SCREEN_MENU)!=widget) return 0;
    return !(*(unsigned*)(screen+SCREEN_SHOWN)&*(unsigned*)(screen+SCREEN_ALLOWED)&MENU_SHOWN);
}

/* Clone only the SWF presentation. Init's expanded loop has already made the
 * eighth native button with the same press/hover callbacks and row index. */
static int ensure_extra(unsigned char *widget)
{
    unsigned char *last=row_widget(widget,6),*previous=row_widget(widget,5);
    unsigned char *extra=row_widget(widget,7),*source,*prior,*parent,*entry,*copy;
    float x,y,step;
    native_string name;
    if (!last || !previous || !extra || !g_resolve(widget)) return 0;
    if (!g_resolve(last) || !g_resolve(previous)) return 0;
    source=*(unsigned char**)(last+8);prior=*(unsigned char**)(previous+8);
    parent=*(unsigned char**)(source+0x30);
    if (!parent || *(void**)(prior+0x30)!=parent || *(int*)(parent+0x88)<=0) return 0;
    entry=(unsigned char*)g_entry(parent,0x4200);
    if (entry) return g_resolve(extra) && *(void**)(extra+8)==*(void**)(entry+0x48);
    entry=(unsigned char*)g_entry(parent,*(int*)(source+0x38));
    if (!entry || *(void**)(entry+0x48)!=source) return 0;
    x=*(float*)(entry+0x18);y=*(float*)(entry+0x1c);
    entry=(unsigned char*)g_entry(parent,*(int*)(prior+0x38));
    if (!entry || *(void**)(entry+0x48)!=prior) return 0;
    step=y-*(float*)(entry+0x1c);
    if (!_finite(x) || !_finite(y) || !_finite(step) || step<=0 || step>512) return 0;
    /* A dedicated dynamic depth must be unused. Native duplication registers
     * sidebar_button_7 in the movie's path lookup and owns its destruction. */
    memset(&name,0,sizeof name);
    g_string(&name,"sidebar_button_7");
    copy=(unsigned char*)g_clone(source,0x200,&name);
    g_destroy(&name);
    if (!copy) return 0;
    entry=(unsigned char*)g_entry(parent,0x4200);
    if (!entry || *(void**)(entry+0x48)!=copy) return 0;
    *(float*)(entry+0x18)=x;*(float*)(entry+0x1c)=y+step;
    *(float*)(copy+0x74)=x;*(float*)(copy+0x78)=y+step;
    if (!g_resolve(extra) || *(void**)(extra+8)!=copy) return 0;
    hide_extra(widget);
    g_clone_hidden=0;
    backend_log("NAV menu: native eighth row created");
    return 1;
}

/* The parent timeline animates the stock rows' display entries and rewrites
 * any entry it moves; it never touches the dynamic entry. Its roll-on writes
 * only row alpha after the rewind; its roll-offs write scale, position and
 * alpha every frame. Run every think. A matrix the timeline wrote since the
 * last pass is the new stock value. Unless a roll-off is playing (parent
 * playing with the widget hidden), extend the panel upward one row over those
 * values so the eighth row keeps the stock text size and clears the fixed
 * footer. Whatever the stock rows show, the clone takes the last row's
 * matrix and colour one row-step further down, so it is drawn, moves and
 * fades with them from the first frame. Idempotent while the timeline is
 * still. */
static void sync_layout(unsigned char *widget,menu_actions *actions)
{
    unsigned char *parent,*entries,*last,*prior,*copy,*row6,*row5;
    float step,height,spacing;
    int i,at_last,at_prior,fresh;
    if (!widget) return;
    parent=*(unsigned char**)(widget+8);
    if (!parent || !actions_valid(actions) || actions->count!=MENU_ROWS) return;
    if (g_layout_parent && g_layout_parent!=parent) restore_layout();
    row6=row_widget(widget,6);row5=row_widget(widget,5);
    if (!row6 || !row5 || !g_resolve(row6) || !g_resolve(row5)) return;
    entries=*(unsigned char**)(parent+0x80);
    /* The shipped panel has a backdrop, four corners, seven rows and a title,
     * then the clone. Refuse a different asset layout rather than guessing. */
    if (!entries || *(int*)(parent+0x88)!=14 ||
        *(unsigned short*)(entries+2)!=1 ||
        *(unsigned short*)(entries+12*0x58+2)!=81 ||
        *(unsigned short*)(entries+13*0x58+2)!=0x4200) return;
    last=(unsigned char*)g_entry(parent,*(int*)(*(unsigned char**)(row6+8)+0x38));
    prior=(unsigned char*)g_entry(parent,*(int*)(*(unsigned char**)(row5+8)+0x38));
    if (!last || !prior) return;
    at_last=(int)((last-entries)/0x58);at_prior=(int)((prior-entries)/0x58);
    if (at_last<3 || at_last>11 || at_prior<3 || at_prior>11 ||
        last!=entries+at_last*0x58 || prior!=entries+at_prior*0x58) return;
    fresh=g_layout_parent!=parent;
    for (i=0;i<13;i++) {
        unsigned char *e=entries+i*0x58;
        if (fresh || g_layout_depths[i]!=*(unsigned short*)(e+2) ||
            memcmp(e+8,g_layout_written[i],sizeof g_layout_written[i])) {
            g_layout_depths[i]=*(unsigned short*)(e+2);
            memcpy(g_layout_matrices[i],e+8,sizeof g_layout_matrices[i]);
            memcpy(g_layout_written[i],e+8,sizeof g_layout_written[i]);
        }
    }
    g_layout_parent=parent;
    /* While a roll-off plays, the timeline owns every entry it moves; a
     * displacement written now would be overwritten before some frames and
     * not others. The roll-on leaves the matrices alone, so the extension is
     * part of the first frame drawn. */
    if (!parent[0x40] || widget[0x1b0]) {
        step=g_layout_matrices[at_last][5]-g_layout_matrices[at_prior][5];
        height=g_layout_matrices[1][5]-g_layout_matrices[3][5];
        if (_finite(step) && _finite(height) && step>=1 && step<=128 && height>=step*7)
            for (i=0;i<13;i++) {
                float *m=g_layout_written[i];
                memcpy(m,g_layout_matrices[i],sizeof g_layout_written[i]);
                if (i==0) { m[1]*=(height+step)/height;m[5]-=step*0.5f; }
                else if (i!=1 && i!=2) m[5]-=step;
                memcpy(entries+i*0x58+8,m,sizeof g_layout_written[i]);
            }
    }
    spacing=*(float*)(last+0x1c)-*(float*)(prior+0x1c);
    if (!_finite(spacing) || spacing<0 || spacing>512) return;
    copy=entries+13*0x58;
    memcpy(copy+8,last+8,0x38);
    *(float*)(copy+0x1c)+=spacing;
}

static int set_label(unsigned char *widget,int index)
{
    unsigned char *row=row_widget(widget,index);
    swf_value value;
    void *field;
    if (!row || !g_resolve(row)) return 0;
    memset(&value,0,sizeof value);
    g_lookup(*(void**)(row+8),&value,"button_text","txtField",NULL,NULL,NULL,NULL);
    field=g_text(&value);
    if (field) g_set_text(field,"BUILD NAVMESH");
    g_value_destroy(&value);
    return field!=NULL;
}

/* Reattach a clone the parent rewind retired. Returns 0 after restoring the
 * stock seven-row menu because the clone could not be recreated. */
static int attach_extra(unsigned char *widget,menu_actions *actions)
{
    unsigned char *parent=*(unsigned char**)(widget+8);
    if (!parent || (*(int*)(parent+0x88)>0 && g_entry(parent,0x4200))) return 1;
    if (ensure_extra(widget)) { g_row(widget,actions->items[7],7);return 1; }
    remove_action(actions);g_bind(widget,actions);
    backend_log("NAV menu: dynamic row unavailable; stock menu restored");
    return 0;
}

/* Begin stores the widget in the StartMenu state (editor+0x23260) and binds
 * the state's own action list, so the editor is recoverable only when the
 * state's widget field names this widget. */
static unsigned char *bind_editor(unsigned char *widget,menu_actions *actions)
{
    unsigned char *state;
    if (!actions) return NULL;
    state=(unsigned char*)actions-0x10;
    if (*(unsigned char**)(state+8)!=widget) return NULL;
    return state-ED_START_MENU;
}

/* Begin binds the rows, then the next screen update rolls the panel on. Roll
 * it on first so the eighth row is cloned, presented and laid out after the
 * rewind and is part of the first frame drawn. */
static void menu_bind(void *object,menu_actions *actions)
{
    unsigned char *widget=(unsigned char*)object;
    int index=-1,extra=0;
    __try { restore_layout(); } __except(EXCEPTION_EXECUTE_HANDLER) { g_layout_parent=NULL; }
    if (g_enabled && !g_failed) {
        __try {
            present_panel(bind_editor(widget,actions),widget);
            extra=ensure_extra(widget);
            if (extra) index=add_action(actions);
            if (!extra) remove_action(actions);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            g_failed=1;
            backend_log("NAV menu: row creation refused after an engine exception");
            __try { remove_action(actions); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    } else {
        __try { remove_action(actions); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_bind(object,actions);
    if (!extra) return;
    __try {
        /* The stock binder presents the first seven rows. Its full count is
         * already correct; present the last row through the same row method. */
        if (actions->count==MENU_ROWS) { g_row(object,actions->items[7],7);g_clone_hidden=0; }
        else hide_extra(widget);
        if (index>=0 && !set_label(widget,index)) {
            remove_action(actions);
            g_bind(object,actions);
            hide_extra(widget);
            backend_log("NAV menu: label field unavailable; stock menu restored");
        } else sync_layout(widget,actions);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_failed=1;
        backend_log("NAV menu: row presentation faulted");
    }
}

static void message(message_fn show,void *object,const char *title,const char *body)
{
    native_string t,b;
    memset(&t,0,sizeof t);memset(&b,0,sizeof b);
    g_string(&t,title);
    __try {
        g_string(&b,body);
        __try { show(object,&t,&b); }
        __finally { g_destroy(&b); }
    } __finally { g_destroy(&t); }
}

static void release_busy(void)
{
    unsigned char *editor=g_busy_editor,*screen=g_busy_screen;
    g_busy_editor=NULL;g_busy_screen=NULL;g_busy_map=NULL;
    g_wait_frames=0;g_requested=0;
    g_busy_since=0;
    if (editor && screen && *(void**)(editor+ED_SCREEN)==screen)
        g_clear_busy(editor);
}

static unsigned char menu_action(void *state,void *object,int index)
{
    unsigned char *editor=(unsigned char*)object;
    menu_actions *actions=(menu_actions*)((unsigned char*)state+0x10);
    if (!g_enabled || index<0 || !actions_valid(actions) || index>=actions->count ||
        actions->items[index]!=NAV_ACTION) {
        unsigned char done=g_action(state,object,index);
        /* A completed native action leaves the panel rolled off (a declined
         * one is rolled on again by the native think). EXIT EDITOR raises its
         * Yes/No here and the menu state persists behind it without further
         * thinks, so the clone would keep its last frame. Show its "off"
         * frame now; a re-show re-presents it and End hides it anyway. */
        if (done && g_enabled && !g_failed) {
            __try {
                unsigned char *widget=*(unsigned char**)((unsigned char*)state+8);
                if (widget && panel_hidden(editor,widget)) hide_clone(widget);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                g_failed=1;
                backend_log("NAV menu: eighth row hide refused after an engine exception");
            }
        }
        return done;
    }
    __try {
        unsigned char *screen=*(unsigned char**)(editor+ED_SCREEN);
        if (g_busy_editor || !screen || !*(void**)(editor+ED_MAP) ||
            (*(unsigned*)(screen+0xa20)&BUSY_FLAG)) return 0;
        /* This is an independent action: no Save, Save As, map validation,
         * disk write, or dirty-state change is involved. */
        g_busy_editor=editor;g_busy_screen=screen;g_busy_map=*(void**)(editor+ED_MAP);
        g_wait_frames=2;g_requested=0;
        g_busy_since=GetTickCount64();
        message(g_busy,editor,"Building Navmesh","Building the map's navmesh...");
        backend_log("NAV menu: building the map's navmesh");
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        __try { release_busy(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        backend_log("NAV menu: could not open the native progress modal");
        return 0;
    }
}

static void busy_tick(unsigned char *editor)
{
    if (g_busy_editor!=editor) return;
    __try {
        int status;
        unsigned char *screen=g_busy_screen;
        if (*(void**)(editor+ED_SCREEN)!=screen || *(void**)(editor+ED_MAP)!=g_busy_map ||
            *(int*)(editor+ED_STATE)!=11) { release_busy(); return; }
        /* Give the pacifier a rendered frame before gathering engine resources. */
        if (g_wait_frames>0) { g_wait_frames--;return; }
        if (!g_requested) {
            sh_apply_engine_nav_request_update();g_requested=1;return;
        }
        status=sh_apply_engine_nav_update_status();
        if (status==SH_NAV_UPDATE_REQUESTED || status==SH_NAV_UPDATE_BUILDING) return;
        /* Small maps finish in a few frames. Keep the native progress message
         * readable without delaying the worker or blocking the engine thread. */
        if (GetTickCount64()-g_busy_since<600) return;
        release_busy();
        *(unsigned*)(screen+SCREEN_SHOWN)|=0xc0;
        /* Name the room and the reason, and tell a partial build from a
         * failed one; nav_bake composes both within the native row width. */
        {
            sh_nav_bake_summary sum;
            char title[48], body[SH_NAV_TOAST_BODY_CAP];
            sh_nav_bake_summary_get(&sum);
            sh_nav_bake_toast(&sum,status==SH_NAV_UPDATE_READY,title,sizeof title,body,sizeof body);
            message(g_toast,screen,title,body);
            backend_log(body);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        __try { release_busy(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        backend_log("NAV menu: progress cleanup after an engine exception");
    }
}

static void menu_think(void *state,void *object)
{
    unsigned char *editor=(unsigned char*)object;
    unsigned char *widget=*(unsigned char**)((unsigned char*)state+8);
    menu_actions *actions=(menu_actions*)((unsigned char*)state+0x10);
    /* A parent rewind can retire the dynamic sprite between UI frames. Repair
     * its binding before native input handling can address the eighth row. */
    if (g_enabled && !g_failed) {
        __try {
            if (widget && actions_valid(actions) && actions->count==MENU_ROWS)
                attach_extra(widget,actions);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            g_failed=1;
            backend_log("NAV menu: row binding refused after an engine exception");
        }
    }
    g_think(state,object);
    busy_tick(editor);
    /* Native think and busy completion re-show the panel by setting the screen
     * flags; roll it on now so its rewind cannot remove the clone after this
     * pass. Then follow the timeline for this frame. */
    if (g_enabled && !g_failed && *(int*)(editor+ED_STATE)==11) {
        __try {
            if (widget && actions_valid(actions) && actions->count==MENU_ROWS) {
                present_panel(editor,widget);
                if (attach_extra(widget,actions)) {
                    /* A re-show normally rewinds and the clone is recreated;
                     * one that survived it is presented again here. */
                    if (g_clone_hidden && !panel_hidden(editor,widget)) {
                        g_row(widget,actions->items[7],7);g_clone_hidden=0;
                    }
                    sync_layout(widget,actions);
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            g_failed=1;
            backend_log("NAV menu: panel layout refused after an engine exception");
        }
    }
}

static void menu_end(void *state,void *object)
{
    __try { restore_layout(); } __except(EXCEPTION_EXECUTE_HANDLER) { g_layout_parent=NULL; }
    if (g_busy_editor==object) {
        __try { release_busy(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_end(state,object);
    /* End rolls every bound row off and clears the screen flags; the panel's
     * own roll-off follows on the next screen update. Hide the clone now. */
    if (g_enabled && !g_failed) {
        __try {
            unsigned char *widget=*(unsigned char**)((unsigned char*)state+8);
            if (widget) hide_clone(widget);
            g_clone_hidden=0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            g_failed=1;
            backend_log("NAV menu: eighth row hide refused after an engine exception");
        }
    }
}

static const sig_result *find(const sig_result *results,size_t count,const char *name)
{
    size_t i;
    for (i=0;i<count;i++)
        if (results[i].name && !strcmp(results[i].name,name) && results[i].status==SIG_OK)
            return &results[i];
    return NULL;
}

int sh_nav_menu_install(const sig_result *results,size_t count)
{
    const sig_result *bind,*action,*think,*end,*init,*row,*select;
    const sig_result *r;
    struct limit { const sig_result *anchor;size_t offset;unsigned char bytes[3]; } limits[4];
    void *hooks[4]={0};
    int i;
    if (g_bind || g_action || g_think || g_end) return g_enabled;
#define RESOLVE(dst,type,name) do { r=find(results,count,name);if(!r)goto fail;dst=(type)r->addr; } while(0)
    RESOLVE(g_string,string_fn,"IdStrCtor");
    RESOLVE(g_destroy,destroy_fn,"IdStrDtor");
    RESOLVE(g_busy,message_fn,"EditorBusyShow");
    RESOLVE(g_clear_busy,destroy_fn,"EditorBusyClear");
    RESOLVE(g_toast,message_fn,"Toast");
    RESOLVE(g_row,row_fn,"StartMenuRow");
    RESOLVE(g_resolve,resolve_fn,"SwfWidgetResolve");
    RESOLVE(g_child,child_fn,"SwfWidgetChild");
    RESOLVE(g_clone,clone_fn,"SwfDuplicateSprite");
    RESOLVE(g_entry,entry_fn,"SwfDisplayEntry");
    RESOLVE(g_lookup,lookup_fn,"SwfNestedValue");
    RESOLVE(g_text,text_fn,"SwfTextInstance");
    RESOLVE(g_value_destroy,destroy_fn,"SwfValueDestroy");
    RESOLVE(g_set_text,string_fn,"SwfSetText");
    RESOLVE(g_label,label_fn,"SwfFrameLabel");
    RESOLVE(g_play,play_fn,"SwfPlayFrame");
#undef RESOLVE
    bind=find(results,count,"StartMenuBind");action=find(results,count,"StartMenuAction");
    think=find(results,count,"StartMenuThink");end=find(results,count,"StartMenuEnd");
    init=find(results,count,"StartMenuInit");row=find(results,count,"StartMenuRow");
    select=find(results,count,"StartMenuSelect");
    if (!bind||!action||!think||!end||!init||!row||!select) goto fail;
    /* Independently verified instruction offsets and bounds on both renderers.
     * Check every site before making any change; no raw image-RVA fallback. */
    limits[0]=(struct limit){init,0x4ee,{0x83,0xff,7}};
    limits[1]=(struct limit){row,0x36,{0x83,0xfb,6}};
    limits[2]=(struct limit){select,0x1a,{0x83,0xfb,6}};
    limits[3]=(struct limit){select,0x6c,{0x83,0xfe,6}};
    __try {
        for(i=0;i<4;i++) if(memcmp((void*)(limits[i].anchor->addr+limits[i].offset),limits[i].bytes,3)) goto fail;
    } __except(EXCEPTION_EXECUTE_HANDLER) { goto fail; }
    /* Whole, position-independent windows; publish originals before commit. */
    hooks[0]=sh_prepare_detour_sig(bind,(void*)menu_bind,18);g_bind=(bind_fn)hooks[0];
    hooks[1]=sh_prepare_detour_sig(action,(void*)menu_action,19);g_action=(action_fn)hooks[1];
    hooks[2]=sh_prepare_detour_sig(think,(void*)menu_think,19);g_think=(state_fn)hooks[2];
    hooks[3]=sh_prepare_detour_sig(end,(void*)menu_end,16);g_end=(state_fn)hooks[3];
    for(i=0;i<4;i++) if(!hooks[i]) goto rollback;
    for(i=0;i<4;i++) {
        unsigned char replacement[3];
        memcpy(replacement,limits[i].bytes,3);replacement[2]++;
        if(code_patch((void*)(limits[i].anchor->addr+limits[i].offset),
            limits[i].bytes,replacement,3,&g_limits[i])!=B2_PATCH_OK) goto rollback;
    }
    for(i=0;i<4;i++) if(sh_commit_detour(hooks[i])!=B2_PATCH_OK) goto rollback;
    g_enabled=1;
    backend_log("NAV menu: native Build Navmesh action installed");
    return 1;
rollback:
    for(i=3;i>=0;i--) if(hooks[i] && sh_uninstall_detour(hooks[i])) {
        if(i==0)g_bind=NULL;else if(i==1)g_action=NULL;else if(i==2)g_think=NULL;else g_end=NULL;
    }
    for(i=3;i>=0;i--) if(g_limits[i].live) code_unpatch(&g_limits[i]);
fail:
    backend_log("NAV menu: unavailable; engine signatures or row bounds refused");
    return 0;
}
