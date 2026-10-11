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
static void movie_row(void *widget,int action,int index)
{ CHECK(widget==movie_widget && action==8 && index==7); }
static void test_panel_layout(void)
{
    int items[]={1,3,9,4,5,6,7,8},i;
    menu_actions a={items,8,8,0};
    unsigned char original[14][0x58];
    g_child=movie_child;g_resolve=movie_resolve;g_entry=movie_entry;g_row=movie_row;
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
    memcpy(original,movie_entries,sizeof original);
    finish_layout(movie_widget,&a);
    CHECK(g_layout_parent==movie_parent);
    CHECK(*(float*)(movie_entries[11]+0x1c)==-221.0f);
    CHECK(*(float*)(movie_entries[13]+0x1c)==150.0f);
    CHECK(*(float*)(movie_entries[1]+0x1c)==172.0f);
    CHECK(*(float*)(movie_entries[3]+0x1c)==-385.0f);
    finish_layout(movie_widget,&a);CHECK(*(float*)(movie_entries[11]+0x1c)==-221.0f);
    restore_layout();CHECK(!g_layout_parent);
    CHECK(!memcmp(original,movie_entries,13*0x58));
    /* A bind without a timeline rewind must not accumulate row displacement. */
    finish_layout(movie_widget,&a);CHECK(*(float*)(movie_entries[13]+0x1c)==150.0f);
    restore_layout();
    movie_parent[0x40]=1;finish_layout(movie_widget,&a);CHECK(!g_layout_parent);
    movie_parent[0x40]=0;a.count=7;finish_layout(movie_widget,&a);CHECK(!g_layout_parent);
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
    test_actions();test_modal();test_panel_layout();test_install_refuses_before_patching();
    printf("nav_menu_test: %d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
