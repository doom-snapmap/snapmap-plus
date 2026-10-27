/* Native menu lifecycle against engine stubs, plus transactional install refusal. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ULONGLONG test_now;
#define GetTickCount64() test_now
#include "../src/backend/nav_menu.c"

static int checks,failures,requests,status,stock_calls,toasts,busy_calls;
static char title_seen[128],body_seen[256];
#define CHECK(x) do { checks++;if(!(x)){failures++;printf("FAIL line %d: %s\n",__LINE__,#x);} } while(0)
void backend_log(const char *s) { (void)s; }
void sh_apply_engine_nav_request_update(void) { requests++;status=SH_NAV_UPDATE_REQUESTED; }
int sh_apply_engine_nav_update_status(void) { return status; }
static void make_string(void *s,const char *text) { *(const char**)s=text; }
static void destroy_string(void *s) { (void)s; }
static void seen(void *title,void *body)
{
    strcpy_s(title_seen,sizeof title_seen,*(const char**)title);
    strcpy_s(body_seen,sizeof body_seen,*(const char**)body);
}
static void show_busy(void *ed,void *title,void *body)
{
    unsigned char *screen=*(unsigned char**)((unsigned char*)ed+ED_SCREEN);
    *(unsigned*)(screen+0xa20)|=BUSY_FLAG;busy_calls++;seen(title,body);
}
static void clear_busy(void *ed)
{
    unsigned char *screen=*(unsigned char**)((unsigned char*)ed+ED_SCREEN);
    *(unsigned*)(screen+0xa20)&=~BUSY_FLAG;
}
static void toast(void *screen,void *title,void *body)
{
    CHECK((*(unsigned*)((unsigned char*)screen+0xa20)&BUSY_FLAG)==0);
    CHECK((*(unsigned*)((unsigned char*)screen+0xa2c)&0xc0)==0xc0);
    toasts++;seen(title,body);
}
static void think(void *state,void *editor) { (void)state;(void)editor; }
static unsigned char stock(void *state,void *editor,int index)
{ (void)state;(void)editor;(void)index;stock_calls++;return 1; }

static unsigned char movie_widget[0x200],movie_parent[0x140],movie_rows[8][0x200];
static unsigned char movie_sprites[8][0x140],movie_entries[14][0x58];
static void *movie_child(void *list,int index)
{ (void)list;return index>=0 && index<8?movie_rows[index]:NULL; }
static unsigned char movie_resolve(void *object)
{ return *(void**)((unsigned char*)object+8)!=NULL; }
static void *movie_entry(void *parent,int depth)
{
    int i;
    for(i=0;i<*(int*)((unsigned char*)parent+0x88);i++)
        if(*(unsigned short*)(movie_entries[i]+2)==depth)return movie_entries[i];
    return NULL;
}
static int row_calls;
static void movie_row(void *widget,int action,int index)
{ CHECK(widget==movie_widget && action==8 && index==7);row_calls++; }
static void movie_fixture(void)
{
    int i;
    memset(movie_parent,0,sizeof movie_parent);memset(movie_entries,0,sizeof movie_entries);
    g_child=movie_child;g_resolve=movie_resolve;g_entry=movie_entry;g_row=movie_row;
    g_layout_parent=NULL;
    *(void**)(movie_widget+8)=movie_parent;*(void**)(movie_widget+0x1c8)=movie_rows;
    *(void**)(movie_parent+0x80)=movie_entries;*(int*)(movie_parent+0x88)=14;
    for(i=0;i<14;i++) {
        *(unsigned short*)(movie_entries[i]+2)=(unsigned short)(i<5?1+i*2:11+(i-5)*10);
        *(float*)(movie_entries[i]+8)=1;*(float*)(movie_entries[i]+0xc)=1;
    }
    *(unsigned short*)(movie_entries[13]+2)=0x4200;
    *(float*)(movie_entries[1]+0x1c)=172;*(float*)(movie_entries[2]+0x1c)=172;
    *(float*)(movie_entries[3]+0x1c)=-332;*(float*)(movie_entries[4]+0x1c)=-332;
    for(i=0;i<8;i++) {
        int n=i<7?11-i:13;
        *(void**)(movie_rows[i]+8)=movie_sprites[i];
        *(void**)(movie_sprites[i]+0x30)=movie_parent;
        *(int*)(movie_sprites[i]+0x38)=*(unsigned short*)(movie_entries[n]+2);
        *(void**)(movie_entries[n]+0x48)=movie_sprites[i];
        *(float*)(movie_entries[n]+0x1c)=-168.0f+53.0f*i;
    }
    /* Row 6's colour transform; the clone must carry it. */
    *(float*)(movie_entries[5]+0x2c)=0.25f;
}
static void test_panel_layout(void)
{
    int items[]={1,3,9,4,5,6,7,8};
    menu_actions a={items,8,8,0};
    unsigned char original[14][0x58];
    movie_fixture();
    memcpy(original,movie_entries,sizeof original);
    sync_layout(movie_widget,&a);
    CHECK(g_layout_parent==movie_parent);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-221.0f);
    CHECK(*(float*)(movie_entries[13]+0x1c)==150.0f);
    CHECK(*(float*)(movie_entries[13]+0x2c)==0.25f);
    CHECK(*(float*)(movie_entries[1]+0x1c)==172.0f);
    CHECK(*(float*)(movie_entries[3]+0x1c)==-385.0f);
    /* A still timeline leaves every pass idempotent. */
    sync_layout(movie_widget,&a);CHECK(*(float*)(movie_entries[11]+0x1c)==-221.0f);
    CHECK(*(float*)(movie_entries[13]+0x1c)==150.0f);
    restore_layout();CHECK(!g_layout_parent);
    CHECK(!memcmp(original,movie_entries,13*0x58));
    sync_layout(movie_widget,&a);CHECK(*(float*)(movie_entries[13]+0x1c)==150.0f);
    /* A matrix the timeline rewrote becomes that entry's stock value. */
    *(float*)(movie_entries[11]+0x1c)=-150.0f;
    sync_layout(movie_widget,&a);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-203.0f && *(float*)(movie_entries[10]+0x1c)==-168.0f);
    restore_layout();
    CHECK(*(float*)(movie_entries[11]+0x1c)==-150.0f && *(float*)(movie_entries[10]+0x1c)==-115.0f);
    /* While the parent plays a roll-off (widget hidden), the stock rows keep
     * the timeline's values and the clone follows the last row one step down. */
    movie_fixture();movie_parent[0x40]=1;movie_widget[0x1b0]=0;
    sync_layout(movie_widget,&a);
    CHECK(g_layout_parent==movie_parent);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-168.0f && *(float*)(movie_entries[5]+0x1c)==150.0f);
    CHECK(*(float*)(movie_entries[13]+0x1c)==203.0f && *(float*)(movie_entries[13]+0x2c)==0.25f);
    *(float*)(movie_entries[5]+0x1c)=140.0f;*(float*)(movie_entries[5]+0x2c)=0.5f;
    sync_layout(movie_widget,&a);
    CHECK(*(float*)(movie_entries[13]+0x1c)==183.0f && *(float*)(movie_entries[13]+0x2c)==0.5f);
    movie_parent[0x40]=0;*(float*)(movie_entries[5]+0x1c)=150.0f;
    sync_layout(movie_widget,&a);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-221.0f && *(float*)(movie_entries[13]+0x1c)==150.0f);
    restore_layout();
    /* The roll-on (parent playing, widget shown) writes only alpha, so the
     * extension is written from its first frame and survives its alpha steps. */
    movie_fixture();movie_parent[0x40]=1;movie_widget[0x1b0]=1;
    *(float*)(movie_entries[5]+0x2c)=0;
    sync_layout(movie_widget,&a);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-221.0f && *(float*)(movie_entries[5]+0x1c)==97.0f);
    CHECK(*(float*)(movie_entries[13]+0x1c)==150.0f && *(float*)(movie_entries[13]+0x2c)==0);
    *(float*)(movie_entries[5]+0x2c)=0.5f;
    sync_layout(movie_widget,&a);
    CHECK(*(float*)(movie_entries[5]+0x1c)==97.0f && *(float*)(movie_entries[13]+0x1c)==150.0f);
    CHECK(*(float*)(movie_entries[13]+0x2c)==0.5f);
    restore_layout();CHECK(*(float*)(movie_entries[11]+0x1c)==-168.0f);
    movie_widget[0x1b0]=0;
    /* A seven-row list or a different asset layout is left alone. */
    movie_fixture();a.count=7;sync_layout(movie_widget,&a);CHECK(!g_layout_parent);
    a.count=8;*(unsigned short*)(movie_entries[13]+2)=0x4300;
    sync_layout(movie_widget,&a);CHECK(!g_layout_parent);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-168.0f);
}

/* Open and close ordering through the hooks, with engine stand-ins that keep
 * the native semantics the order depends on: SetVisible acts only on a change
 * and its roll-on rewind removes the dynamic entry; duplication appends it. */
static int order_step,order_visible,order_clone,order_bind,plays_off,plays_total;
static void *last_play_sprite;
static void movie_visible(void *widget,char visible)
{
    unsigned char *w=(unsigned char*)widget;
    if (w[0x1b0]==(unsigned char)visible) return;
    w[0x1b0]=(unsigned char)visible;
    if (visible) { *(int*)(movie_parent+0x88)=13;movie_parent[0x40]=1; }
    order_visible=++order_step;
}
static void *movie_vtable[0x100/8];
static void *movie_clone(void *source,int depth,void *name)
{
    (void)name;
    CHECK(source==movie_sprites[6] && depth==0x200);
    CHECK(*(int*)(movie_parent+0x88)==13);
    *(int*)(movie_parent+0x88)=14;
    memset(movie_entries[13],0,0x58);
    *(unsigned short*)(movie_entries[13]+2)=0x4200;
    *(void**)(movie_entries[13]+0x48)=movie_sprites[7];
    order_clone=++order_step;
    return movie_sprites[7];
}
static int movie_label(void *sprite,const char *label)
{ (void)sprite;return !strcmp(label,"off")?3:!strcmp(label,"rollOff")?2:1; }
static void movie_play(void *sprite,int frame)
{ plays_total++;last_play_sprite=sprite;if(frame==3)plays_off++; }
static void movie_bind(void *widget,menu_actions *a)
{ CHECK(widget==movie_widget && a->count==8 && a->items[2]==NAV_ACTION);order_bind=++order_step; }
static int text_field;
static void *movie_lookup(void *s,void *v,const char *a,const char *b,void *c,void *d,void *e,void *f)
{ (void)s;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return v; }
static void *movie_text(void *v) { (void)v;return &text_field; }
static void movie_set_text(void *field,const char *text)
{ CHECK(field==&text_field && !strcmp(text,"BUILD NAVMESH")); }
static void test_open_close_order(void)
{
    unsigned char *ed=(unsigned char*)calloc(1,ED_STATE+16),screen[0xa40]={0};
    unsigned char *state=ed+ED_START_MENU;
    int items[8]={1,3,4,5,6,7,8,0};
    menu_actions *a=(menu_actions*)(state+0x10);
    movie_fixture();
    memset(movie_vtable,0,sizeof movie_vtable);
    movie_vtable[WIDGET_SET_VISIBLE/8]=(void*)movie_visible;
    *(void**)movie_widget=movie_vtable;movie_widget[0x1b0]=0;
    *(void**)(state+8)=movie_widget;*a=(menu_actions){items,7,8,0};
    *(void**)(ed+ED_SCREEN)=screen;*(void**)(screen+SCREEN_MENU)=movie_widget;
    *(int*)(ed+ED_STATE)=11;
    /* Begin sets both shown bits before it binds. */
    *(unsigned*)(screen+SCREEN_SHOWN)=0xc0;*(unsigned*)(screen+SCREEN_ALLOWED)=0xffffffff;
    g_enabled=1;g_failed=0;g_bind=movie_bind;g_think=think;g_end=think;
    g_string=make_string;g_destroy=destroy_string;g_clone=movie_clone;
    g_label=movie_label;g_play=movie_play;g_lookup=movie_lookup;g_text=movie_text;
    g_value_destroy=destroy_string;g_set_text=movie_set_text;
    order_step=order_visible=order_clone=order_bind=0;
    menu_bind(movie_widget,a);
    /* The roll-on rewind precedes the clone, and the bind completes the row
     * and its placement without waiting for a think. */
    CHECK(order_visible==1 && order_clone==2 && order_bind==3 && !g_failed);
    CHECK(a->count==8 && items[2]==NAV_ACTION);
    CHECK(*(int*)(movie_parent+0x88)==14 && movie_entry(movie_parent,0x4200));
    CHECK(g_layout_parent==movie_parent);
    /* The roll-on is playing and moves no matrix: the extension is already
     * in place for the first frame, the clone at the original last row. */
    CHECK(*(float*)(movie_entries[11]+0x1c)==-221.0f);
    CHECK(*(float*)(movie_entries[13]+0x1c)==150.0f && *(float*)(movie_entries[13]+0x2c)==0.25f);
    /* The native screen update now finds the panel already shown. */
    movie_visible(movie_widget,1);CHECK(order_step==3);
    /* The roll-on settles: the next think extends the panel. */
    movie_parent[0x40]=0;menu_think(state,ed);
    CHECK(order_step==3 && *(int*)(movie_parent+0x88)==14);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-221.0f && *(float*)(movie_entries[13]+0x1c)==150.0f);
    /* Close: the stock layout comes back and the clone shows its off frame. */
    plays_off=0;
    menu_end(state,ed);
    CHECK(plays_off==1 && last_play_sprite==movie_sprites[7]);
    CHECK(*(unsigned*)(movie_rows[7]+0x1b0)==0x10 && !g_layout_parent);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-168.0f);
    /* A clone that is no longer attached is never addressed. */
    *(int*)(movie_parent+0x88)=13;plays_total=0;
    menu_end(state,ed);CHECK(plays_total==0);
    /* A re-show the native think requests is rolled on, and the clone
     * repaired, inside the same think. */
    movie_widget[0x1b0]=0;*(int*)(movie_parent+0x88)=14;
    order_step=order_visible=order_clone=0;
    menu_think(state,ed);
    CHECK(order_visible==1 && order_clone==2 && movie_entry(movie_parent,0x4200));
    /* The panel is not rolled on when the screen would not show it, or for a
     * widget the screen does not own. */
    movie_widget[0x1b0]=0;order_step=0;
    *(unsigned*)(screen+SCREEN_ALLOWED)=~(unsigned)MENU_SHOWN;
    present_panel(ed,movie_widget);CHECK(order_step==0);
    *(unsigned*)(screen+SCREEN_ALLOWED)=0xffffffff;*(void**)(screen+SCREEN_MENU)=NULL;
    present_panel(ed,movie_widget);CHECK(order_step==0);
    *(void**)(state+8)=NULL;CHECK(bind_editor(movie_widget,a)==NULL);
    CHECK(!g_failed);
    restore_layout();
    g_enabled=0;g_bind=NULL;g_think=NULL;g_end=NULL;
    free(ed);
}

/* EXIT EDITOR: the native action raises its Yes/No and returns 1. The menu
 * state persists behind the dialog with the panel rolled off and no further
 * thinks, so the clone must be hidden by the action hook itself. */
static int exit_calls;
static unsigned char exit_result;
static unsigned char *exit_screen;
static unsigned char exit_action(void *state,void *editor,int index)
{
    (void)state;(void)editor;CHECK(index==7);exit_calls++;
    if (exit_result) *(unsigned*)(exit_screen+SCREEN_SHOWN)|=1;
    return exit_result;
}
static void test_exit_dialog(void)
{
    unsigned char *ed=(unsigned char*)calloc(1,ED_STATE+16),screen[0xa40]={0};
    unsigned char *state=ed+ED_START_MENU;
    int items[8]={1,3,4,5,6,7,8,0};
    menu_actions *a=(menu_actions*)(state+0x10);
    movie_fixture();
    memset(movie_vtable,0,sizeof movie_vtable);
    movie_vtable[WIDGET_SET_VISIBLE/8]=(void*)movie_visible;
    *(void**)movie_widget=movie_vtable;movie_widget[0x1b0]=0;
    *(void**)(state+8)=movie_widget;*a=(menu_actions){items,7,8,0};
    *(void**)(ed+ED_SCREEN)=screen;*(void**)(screen+SCREEN_MENU)=movie_widget;
    *(int*)(ed+ED_STATE)=11;exit_screen=screen;
    *(unsigned*)(screen+SCREEN_SHOWN)=0xc0;*(unsigned*)(screen+SCREEN_ALLOWED)=0xffffffff;
    g_enabled=1;g_failed=0;g_bind=movie_bind;g_think=think;g_end=think;g_action=exit_action;
    g_string=make_string;g_destroy=destroy_string;g_clone=movie_clone;
    g_label=movie_label;g_play=movie_play;g_lookup=movie_lookup;g_text=movie_text;
    g_value_destroy=destroy_string;g_set_text=movie_set_text;
    menu_bind(movie_widget,a);
    movie_parent[0x40]=0;menu_think(state,ed);
    CHECK(a->count==8 && a->items[7]==8 && !g_clone_hidden);
    CHECK(*(float*)(movie_entries[13]+0x1c)==150.0f);
    /* Select EXIT EDITOR: the native think has cleared the shown bits and
     * calls the action before the screen update hides the widget. */
    *(unsigned*)(screen+SCREEN_SHOWN)=0;exit_result=1;exit_calls=0;plays_off=0;row_calls=0;
    CHECK(menu_action(state,ed,7)==1 && exit_calls==1);
    CHECK(plays_off==1 && last_play_sprite==movie_sprites[7]);
    CHECK(*(unsigned*)(movie_rows[7]+0x1b0)==0x10 && g_clone_hidden && !g_failed);
    /* Should a think still run in the state, nothing re-presents the clone:
     * not before the screen update hides the widget... */
    menu_think(state,ed);CHECK(row_calls==0 && g_clone_hidden);
    /* ...nor while the roll-off plays; the stock rows keep the timeline's
     * values and the clone entry follows them as before. */
    movie_visible(movie_widget,0);movie_parent[0x40]=1;
    *(float*)(movie_entries[5]+0x1c)=20.0f;*(float*)(movie_entries[6]+0x1c)=0.0f;
    *(float*)(movie_entries[5]+0x2c)=0;
    menu_think(state,ed);
    CHECK(row_calls==0 && g_clone_hidden);
    CHECK(*(float*)(movie_entries[5]+0x1c)==20.0f && *(float*)(movie_entries[13]+0x1c)==40.0f);
    CHECK(*(float*)(movie_entries[13]+0x2c)==0);
    /* No ends the menu: End hides the clone again and clears the state. */
    plays_off=0;menu_end(state,ed);
    CHECK(plays_off==1 && !g_clone_hidden && !g_layout_parent);
    /* Reopen. A declined action is rolled on again by the native think, and
     * one that completes while the panel is still requested leaves it shown:
     * the clone is untouched in both. */
    movie_fixture();movie_widget[0x1b0]=0;*(unsigned*)(screen+SCREEN_SHOWN)=0xc0;
    *(int*)(movie_parent+0x88)=13;
    menu_bind(movie_widget,a);movie_parent[0x40]=0;
    CHECK(movie_entry(movie_parent,0x4200) && movie_widget[0x1b0]==1);
    *(unsigned*)(screen+SCREEN_SHOWN)=0;exit_result=0;plays_off=0;
    CHECK(menu_action(state,ed,7)==0 && plays_off==0 && !g_clone_hidden);
    *(unsigned*)(screen+SCREEN_SHOWN)=0xc0;exit_result=1;
    CHECK(menu_action(state,ed,7)==1 && plays_off==0 && !g_clone_hidden);
    /* A clone that survives a re-show (no rewind) is presented again by the
     * next think, once. */
    *(unsigned*)(screen+SCREEN_SHOWN)=0;
    CHECK(menu_action(state,ed,7)==1 && plays_off==1 && g_clone_hidden);
    *(unsigned*)(screen+SCREEN_SHOWN)=0xc0;row_calls=0;
    menu_think(state,ed);CHECK(row_calls==1 && !g_clone_hidden);
    menu_think(state,ed);CHECK(row_calls==1);
    /* A re-show that rewinds recreates the clone and presents it once. */
    *(unsigned*)(screen+SCREEN_SHOWN)=0;
    CHECK(menu_action(state,ed,7)==1 && g_clone_hidden);
    movie_visible(movie_widget,0);
    *(unsigned*)(screen+SCREEN_SHOWN)=0xc0;row_calls=0;order_step=order_visible=order_clone=0;
    menu_think(state,ed);
    CHECK(order_visible==1 && order_clone==2 && row_calls==1 && !g_clone_hidden);
    CHECK(movie_entry(movie_parent,0x4200) && !g_failed);
    /* A state without a widget is left to the native action. */
    *(void**)(state+8)=NULL;*(unsigned*)(screen+SCREEN_SHOWN)=0;plays_off=0;
    CHECK(menu_action(state,ed,7)==1 && plays_off==0 && !g_failed);
    restore_layout();g_clone_hidden=0;
    g_enabled=0;g_bind=NULL;g_action=NULL;g_think=NULL;g_end=NULL;
    free(ed);
}

static void test_actions(void)
{
    int stock_ids[]={1,3,4,5,6,7,8},slots[9],short_ids[]={1,3,5,6,7,8};
    menu_actions a={slots,7,8,0};
    memcpy(slots,stock_ids,sizeof stock_ids);slots[8]=12345;
    CHECK(add_action(&a)==2 && a.count==8 && slots[2]==NAV_ACTION);
    CHECK(slots[0]==1 && slots[1]==3 && !memcmp(slots+3,stock_ids+2,5*sizeof(int)));
    CHECK(add_action(&a)==2 && a.count==8 && slots[8]==12345);
    remove_action(&a);CHECK(a.count==7 && !memcmp(slots,stock_ids,sizeof stock_ids));
    a.capacity=7;CHECK(add_action(&a)==-1 && a.count==7 && !memcmp(slots,stock_ids,sizeof stock_ids));
    a.capacity=8;a.count=6;memcpy(slots,short_ids,sizeof short_ids);
    CHECK(add_action(&a)==2 && a.count==7);
    remove_action(&a);CHECK(!memcmp(slots,short_ids,sizeof short_ids));
    a.count=-1;CHECK(add_action(&a)==-1);
    a.count=9;CHECK(add_action(&a)==-1);
}

static void request_tick(void *s,void *ed)
{
    menu_think(s,ed);menu_think(s,ed);
    CHECK(!g_requested);
    menu_think(s,ed);CHECK(g_requested);
}
static void test_modal(void)
{
    unsigned char *ed=(unsigned char*)calloc(1,ED_STATE+16),screen[0xa40]={0},s[64]={0};
    int items[]={1,3,9,4,5,6,7,8};
    menu_actions *a=(menu_actions*)(s+0x10);
    *a=(menu_actions){items,8,8,0};
    *(void**)(ed+ED_SCREEN)=screen;*(void**)(ed+ED_MAP)=(void*)1;*(int*)(ed+ED_STATE)=11;
    g_enabled=1;g_think=think;g_end=think;g_action=stock;g_string=make_string;
    g_destroy=destroy_string;g_busy=show_busy;g_clear_busy=clear_busy;g_toast=toast;
    CHECK(menu_action(s,ed,1)==1 && stock_calls==1 && requests==0);
    CHECK(menu_action(s,ed,2)==1 && busy_calls==1 && requests==0);
    CHECK(!strcmp(title_seen,"Building Navmesh"));
    CHECK(!strcmp(body_seen,"Building the map's navmesh..."));
    CHECK(menu_action(s,ed,2)==0 && busy_calls==1);
    request_tick(s,ed);CHECK(requests==1 && toasts==0);
    status=SH_NAV_UPDATE_BUILDING;
    menu_think(s,ed);CHECK(toasts==0 && g_busy_editor==ed);
    status=SH_NAV_UPDATE_READY;
    menu_think(s,ed);CHECK(toasts==0 && g_busy_editor==ed);
    test_now+=600;
    menu_think(s,ed);CHECK(toasts==1 && !g_busy_editor && !strcmp(title_seen,"Navmesh Built"));
    menu_think(s,ed);CHECK(toasts==1 && requests==1);
    CHECK(menu_action(s,ed,2)==1);
    request_tick(s,ed);status=SH_NAV_UPDATE_FAILED;test_now+=600;menu_think(s,ed);
    CHECK(toasts==2 && !strcmp(title_seen,"Navmesh Build Failed"));
    CHECK(menu_action(s,ed,2)==1);
    menu_end(s,ed);CHECK(!g_busy_editor && !(*(unsigned*)(screen+0xa20)&BUSY_FLAG));
    status=SH_NAV_UPDATE_READY;menu_think(s,ed);CHECK(toasts==2);
    CHECK(menu_action(s,ed,2)==1);
    *(void**)(ed+ED_MAP)=(void*)2;menu_think(s,ed);
    CHECK(!g_busy_editor && toasts==2 && !(*(unsigned*)(screen+0xa20)&BUSY_FLAG));
    /* A different native operation's busy flag is not ours to dismiss. */
    *(unsigned*)(screen+0xa20)=BUSY_FLAG;
    CHECK(menu_action(s,ed,2)==0 && !g_busy_editor);
    CHECK(*(unsigned*)(screen+0xa20)==BUSY_FLAG);
    g_enabled=0;g_bind=NULL;g_action=NULL;g_think=NULL;g_end=NULL;
    free(ed);
}

static void test_install_refuses_before_patching(void)
{
    const char *names[]={"IdStrCtor","IdStrDtor","EditorBusyShow","EditorBusyClear","Toast",
        "StartMenuRow","SwfWidgetResolve","SwfWidgetChild","SwfDuplicateSprite","SwfDisplayEntry",
        "SwfNestedValue","SwfTextInstance","SwfValueDestroy","SwfSetText","SwfFrameLabel",
        "SwfPlayFrame","StartMenuBind","StartMenuAction","StartMenuThink","StartMenuEnd",
        "StartMenuInit","StartMenuSelect"};
    sig_result r[22]={0};
    unsigned char *code=(unsigned char*)VirtualAlloc(NULL,0x4000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    unsigned char before[0x4000];
    int i;
    CHECK(code!=NULL);if(!code)return;
    memset(code,0x90,0x4000);
    for(i=0;i<22;i++){r[i].name=names[i];r[i].status=SIG_OK;r[i].addr=(uintptr_t)(code+i*0x200);}
    memcpy((void*)(r[20].addr+0x4ee),"\x83\xff\x07",3);
    memcpy((void*)(r[5].addr+0x36),"\x83\xfb\x06",3);
    memcpy((void*)(r[21].addr+0x1a),"\x83\xfb\x06",3);
    /* One unexpected selection bound must refuse all patches, not just itself. */
    memcpy((void*)(r[21].addr+0x6c),"\x83\xfe\x05",3);
    memcpy(before,code,sizeof before);
    CHECK(!sh_nav_menu_install(r,22));
    CHECK(!memcmp(before,code,sizeof before) && hook_owned_count()==0);
    /* A late code-write failure rolls back earlier bounds and all trampolines. */
    memcpy((void*)(r[21].addr+0x6c),"\x83\xfe\x06",3);
    memcpy(before,code,sizeof before);
    sh_patch_test_faults(0,1ULL<<2,1);
    CHECK(!sh_nav_menu_install(r,22));
    sh_patch_test_faults(0,0,0);
    CHECK(!memcmp(before,code,sizeof before) && hook_owned_count()==0);
    for(i=0;i<4;i++) CHECK(!g_limits[i].live);
    VirtualFree(code,0,MEM_RELEASE);
}
int main(void)
{
    test_actions();test_modal();test_panel_layout();test_open_close_order();test_exit_dialog();
    test_install_refuses_before_patching();
    printf("nav_menu_test: %d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
