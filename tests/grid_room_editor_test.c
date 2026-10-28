#include "../src/backend/grid_room_editor.c"
#include <assert.h>
#include <stdio.h>

static sh_grid_size rooms[2];
static unsigned char widget[0x130];
static int rows,changes,native_changes,reads,refuse_apply;
static unsigned placement_max;
static float input_ranges[18];
static unsigned char editor[0x23700],edit_data[0x100];
static int pressed,blocked,opened,opened_index,help_rows,updates,transition;
void backend_log(const char *s){(void)s;}
static void native_populate(void *p,int i,void *m,void *s,void *e)
{(void)s;(void)e;*(void**)((char*)p+0x1c0)=m;*(int*)((char*)p+0x1dc)=i;}
static void native_change(void *p,void *v,int a){(void)p;(void)v;(void)a;++native_changes;}
static uint32_t hash(const char *s){(void)s;return 123;}
static void set_vec(void *p,const float *v){memcpy((char*)p+0x100,v,12);}
static int read_room(void *m,int i,sh_grid_size *s)
{++reads;if((m!=rooms&&m!=edit_data)||i<0||i>1)return 0;*s=rooms[i];return s->kind!=SH_GRID_NONE;}
static int apply_room(void *m,int i,const sh_grid_size *s)
{assert(m==rooms&&i>=0&&i<=1);if(refuse_apply)return 0;
 if(!memcmp(&rooms[i],s,sizeof *s))return 1;++changes;rooms[i]=*s;
 if(placement_max&&rooms[i].xyz[1]>placement_max)rooms[i].xyz[1]=placement_max;return 1;}
static void add(void *p,int id,const char *label,const uint32_t *a,const uint32_t *b,
    unsigned char enabled,const float *v,float min,float max,float small_step,float large_step,
    unsigned char integral,void *context,const char *extra,const void *value)
{
    unsigned char *c=context;int i,n=*(int*)(c+0xa8);
    assert((id==GRID_PROPERTY_SIZE||id==11)&&label);
    assert(a&&b&&enabled&&small_step>0&&large_step>0&&integral);
    assert(context==(char*)p+0x280&&!extra&&!value);
    assert(n==0&&*(void**)(c+0xa0)==input_ranges&&*(int*)(c+0xac)==9);
    for(i=0;i<3;i++){input_ranges[i*2]=min;input_ranges[i*2+1]=max;}
    *(int*)(c+0xa8)=3;
    *(int*)(widget+0x40)=id;set_vec(widget,v);++rows;
}
static unsigned char input_pressed(void *input,int action)
{assert(input==editor+0x20&&action==0x50);return (unsigned char)pressed;}
static unsigned char properties_blocked(void *tutorial,unsigned flag)
{assert(tutorial==editor+0x20990&&flag==0x1000);return (unsigned char)blocked;}
static void set_state(void *e,int state)
{assert(e==editor&&state==4);opened_index=*(int*)(editor+0x22ef8);++opened;
 *(int*)(editor+0x23618)=state;}
static void action_help(void *context,int action,int alternate)
{assert(context==editor+0x211a8&&action==0x50&&alternate==0x9f);++help_rows;}
static void native_help(void *normal,void *e){(void)normal;assert(e==editor);}
static void native_update(void *normal,void *mode,void *e)
{
    (void)normal;assert(mode==editor+0x212b8&&e==editor);++updates;
    editor[0x2120c]=0;
    if(transition)*(int*)(editor+0x212b8+0x2c)=2;
}
static void blueprint_tests(void)
{
    void *mode=editor+0x212b8,*normal=editor+0x212b8+0x38;
    int stack[]={1,1};
    sh_grid_default(SH_GRID_CLASSIC,&rooms[0]);sh_grid_default(SH_GRID_MODERN,&rooms[1]);
    *(void**)(editor+0x204d0)=edit_data;*(int*)(editor+0x23618)=1;
    *(int*)(editor+0x212b8+0x2c)=1;*(int*)(edit_data+0x20)=1;
    g_blueprint_update=native_update;g_blueprint_help=native_help;g_pressed=input_pressed;
    g_blocked=properties_blocked;g_set_state=set_state;g_action_help=action_help;
    grid_blueprint_help(normal,editor);assert(help_rows==1);
    grid_blueprint_update(normal,mode,editor);assert(opened==0);
    pressed=1;grid_blueprint_update(normal,mode,editor);assert(opened==1&&opened_index==1);
    assert(*(int*)(editor+0x3d8)==0&&*(int*)(editor+0x21078)==0);
    /* The same held input cannot reopen a panel after the native state change. */
    grid_blueprint_update(normal,mode,editor);assert(opened==1);
    *(int*)(editor+0x23618)=1;*(int*)(edit_data+0x20)=0;
    *(void**)(editor+0x21070)=stack;*(int*)(editor+0x21078)=2;
    *(unsigned*)(editor+0x2107c)=0x80000002;*(int*)(editor+0x3d8)=1;
    grid_blueprint_update(normal,mode,editor);assert(opened==2&&opened_index==0);
    assert(stack[0]==1&&stack[1]==4&&*(int*)(editor+0x21078)==2&&*(int*)(editor+0x3d8)==1);
    *(int*)(editor+0x23618)=1;rooms[0].kind=SH_GRID_NONE;
    grid_blueprint_update(normal,mode,editor);grid_blueprint_help(normal,editor);
    assert(opened==2&&help_rows==1);
    sh_grid_default(SH_GRID_CLASSIC,&rooms[0]);blocked=1;
    grid_blueprint_update(normal,mode,editor);grid_blueprint_help(normal,editor);
    assert(opened==2&&help_rows==1);blocked=0;
    *(int*)(edit_data+0x20)=-1;grid_blueprint_update(normal,mode,editor);assert(opened==2);
    *(int*)(edit_data+0x20)=0;editor[0x2120c]=1;
    grid_blueprint_update(normal,mode,editor);assert(opened==2);
    transition=1;grid_blueprint_update(normal,mode,editor);assert(opened==2);
    transition=0;*(int*)(editor+0x212b8+0x2c)=1;
    *((unsigned char*)normal+0x10)=0x10;
    grid_blueprint_update(normal,mode,editor);assert(opened==2&&updates==10);
    *((unsigned char*)normal+0x10)=0;*(int*)(editor+0x21078)=3;
    grid_blueprint_update(normal,mode,editor);assert(opened==2);
}
/* The open X/Y/Z sub-editor keeps its draft in the panel context
 * (panel+0x280: entries +0x58, count +0x60, capacity +0x64, stride 0x38, value
 * +0x30), with two snapshots in the same form: +0x70 (the value the row was
 * built with) and +0x88 (copied from the draft when the sub-editor opens).
 * Native accept (Esc) copies the draft into +0x100 and dispatches 100. Both
 * cancel routes restore the +0x88 snapshot into the draft and then apply the
 * draft: the sub-editor's Delete revert dispatches a per-press change, and the
 * Revert dialog's Yes runs the inspector cancel (+0xe8 into the row, context
 * cancel) and dispatches -100. The panel revert restores +0xf4. None may apply
 * a size the room had before an outside change (R-010 C4: the draft Z 3389
 * undid a console resize to 3000; R-014 S2: both cancel routes restored the
 * size the panel opened with). */
static unsigned char draft[3*0x38],built[3*0x38],opened_draft[3*0x38];
static float draft_value(int i){return *(float*)(draft+i*0x38+0x30);}
static float entry(const unsigned char *list,int i){return *(float*)(list+i*0x38+0x30);}
static void native_accept(void){int i;for(i=0;i<3;++i)*(float*)(widget+0x100+4*i)=draft_value(i);}
static void native_open(void){memcpy(opened_draft,draft,sizeof draft);}
static void native_context_cancel(void){memcpy(draft,opened_draft,sizeof draft);}
static void native_delete_revert(unsigned char *panel)
{native_context_cancel();native_accept();grid_changed(panel,widget,1);}
static void native_dialog_yes(unsigned char *panel)
{memcpy(widget+0x100,widget+0xe8,12);native_context_cancel();native_accept();grid_changed(panel,widget,-100);}
/* The open sub-editor is screen 10 of the menu's Vec context-list manager
 * (menu +0xa10); native code redraws it, and refreshes the row's dirty marker
 * (+0xc8: draft +0x58 differs from +0x70), only after a sub-editor key. */
static unsigned char menu[0xa20],manager[0x10],screen[0x200];
static int redraws,dirty_refreshes,resolve_ok=1;static float drawn[3];
static unsigned char *expected_owner;
static void dirty_refresh(void *v)
{int i,dirty=0;assert(v==widget);++dirty_refreshes;
 for(i=0;i<3;++i)if(draft_value(i)!=entry(built,i))dirty=1;widget[0xc8]=(unsigned char)dirty;}
static void *menu_of(void *owner){assert(owner==expected_owner);return menu;}
static void *screen_by_index(void *m,int index){assert(m==manager&&index==10);return screen;}
static unsigned char widget_resolve(void *w){assert(w==screen);return (unsigned char)resolve_ok;}
static void list_redraw(void *w)
{int i;assert(w==screen);++redraws;for(i=0;i<3;++i)drawn[i]=draft_value(i);}
static void redraw_checks(unsigned char *panel)
{
    unsigned char *ctx=panel+0x280;int before=redraws;
    /* The sub-editor drew a stale Z 2900 and lit the row's dirty marker. */
    rooms[0].xyz[2]=2800;*(float*)(draft+2*0x38+0x30)=2900;drawn[2]=2900;widget[0xc8]=1;
    native_accept();grid_changed(panel,widget,-1);
    assert(rooms[0].xyz[2]==2800&&redraws==before+1&&drawn[0]==5120&&drawn[1]==5120&&drawn[2]==2800);
    assert(widget[0xc8]==0);
    /* Closed sub-editor: the row marker still refreshes, nothing is drawn. */
    widget[0x29]=0;rooms[0].xyz[2]=2700;widget[0xc8]=1;before=redraws;
    {int refreshes=dirty_refreshes;native_accept();grid_changed(panel,widget,-1);
     assert(redraws==before&&drawn[2]==2800&&widget[0xc8]==0&&dirty_refreshes==refreshes+1);}
    widget[0x29]=1;
    /* A screen bound to another context, or one whose sprite does not resolve,
     * is not drawn. */
    *(unsigned char**)(screen+0x1e8)=panel;rooms[0].xyz[2]=2600;
    native_accept();grid_changed(panel,widget,-1);assert(redraws==before&&rooms[0].xyz[2]==2600);
    *(unsigned char**)(screen+0x1e8)=ctx;resolve_ok=0;rooms[0].xyz[2]=2500;
    native_accept();grid_changed(panel,widget,-1);assert(redraws==before&&rooms[0].xyz[2]==2500);
    resolve_ok=1;*(unsigned char**)(menu+0xa10)=NULL;rooms[0].xyz[2]=2400;
    native_accept();grid_changed(panel,widget,-1);assert(redraws==before);
    *(unsigned char**)(menu+0xa10)=manager;
    /* A user step from the redrawn sub-editor applies and redraws its value. */
    *(float*)(draft+2*0x38+0x30)=2401;native_accept();grid_changed(panel,widget,1);
    assert(rooms[0].xyz[2]==2401&&redraws==before);
    /* Back to the C4 sequence's state (5120 x 5120 x 3000, as the console set it). */
    rooms[0].xyz[2]=3000;native_accept();grid_changed(panel,widget,-1);
    assert(redraws==before+1&&drawn[2]==3000&&widget[0xc8]==0);
}
static void sub_editor_draft_checks(unsigned char *panel)
{
    unsigned char *ctx=panel+0x280;unsigned before;int i,saved_rows=rows;
    float shown[3];
    *(void**)(ctx+0x58)=draft;*(int*)(ctx+0x60)=3;*(int*)(ctx+0x64)=3;
    *(void**)(ctx+0x70)=built;*(int*)(ctx+0x78)=3;*(int*)(ctx+0x7c)=3;
    *(void**)(ctx+0x88)=opened_draft;*(int*)(ctx+0x90)=3;*(int*)(ctx+0x94)=4;
    *(void**)(widget+0xd8)=ctx;
    expected_owner=panel;*(void**)(widget+0x48)=panel;widget[0x29]=1;
    *(unsigned char**)(menu+0xa10)=manager;
    *(unsigned char**)(screen+0x1b8)=ctx;*(unsigned char**)(screen+0x1e8)=ctx;
    g_dirty_refresh=dirty_refresh;g_menu_of=menu_of;g_screen_by_index=screen_by_index;
    g_widget_resolve=widget_resolve;g_list_redraw=list_redraw;
    sh_grid_default(SH_GRID_MODERN,&rooms[0]);
    grid_populate(panel,0,rooms,NULL,NULL);
    for(i=0;i<3;++i){*(float*)(draft+i*0x38+0x30)=(float)rooms[0].xyz[i];
        *(float*)(widget+0xe8+4*i)=(float)rooms[0].xyz[i];
        *(float*)(widget+0xf4+4*i)=(float)rooms[0].xyz[i];}
    memcpy(built,draft,sizeof draft);native_open();
    /* Two live steps in the sub-editor apply per press (5120 x 5120 x 3390). */
    *(float*)(draft+2*0x38+0x30)=3390;native_accept();grid_changed(panel,widget,1);
    assert(rooms[0].xyz[2]==3390);
    /* The console resizes the room under the open sub-editor; the next press
     * carries the stale draft 3389 and is refused by the guard. */
    rooms[0].xyz[2]=3000;before=changes;
    *(float*)(draft+2*0x38+0x30)=3389;native_accept();grid_changed(panel,widget,-1);
    assert(changes==before&&rooms[0].xyz[2]==3000);
    for(i=0;i<3;++i){shown[i]=(float)rooms[0].xyz[i];
        assert(draft_value(i)==shown[i]);
        assert(*(float*)(widget+0x100+4*i)==shown[i]&&*(float*)(widget+0xe8+4*i)==shown[i]);
        assert(*(float*)(widget+0xf4+4*i)==shown[i]&&entry(built,i)==shown[i]&&entry(opened_draft,i)==shown[i]);
        assert(drawn[i]==shown[i]);}
    /* The open sub-editor was redrawn with the console size, and the row's
     * dirty marker cleared, by that guard press. */
    assert(redraws==1&&widget[0xc8]==0);
    redraw_checks(panel);before=changes;
    /* After the guard, neither cancel route undoes the console resize. */
    native_delete_revert(panel);
    assert(changes==before&&rooms[0].xyz[2]==3000&&draft_value(2)==3000);
    *(float*)(draft+2*0x38+0x30)=3100;native_accept();grid_changed(panel,widget,1);
    assert(changes==before+1&&rooms[0].xyz[2]==3100);
    native_dialog_yes(panel);
    assert(changes==before+2&&rooms[0].xyz[2]==3000);
    before=changes;
    native_dialog_yes(panel);
    assert(changes==before&&rooms[0].xyz[2]==3000);
    /* Esc accepts the draft: it keeps 3000. */
    native_accept();grid_changed(panel,widget,100);
    assert(changes==before&&rooms[0].xyz[2]==3000);
    /* Esc straight after an outside change, with no press in between. */
    rooms[0].xyz[2]=2500;*(float*)(draft+2*0x38+0x30)=3000;
    native_accept();grid_changed(panel,widget,100);
    assert(changes==before&&rooms[0].xyz[2]==2500&&draft_value(2)==2500&&*(float*)(widget+0xe8+8)==2500);
    native_accept();grid_changed(panel,widget,100);
    assert(changes==before&&rooms[0].xyz[2]==2500);
    /* User edits are still applied after the guard. */
    *(float*)(draft+2*0x38+0x30)=2600;native_accept();grid_changed(panel,widget,1);
    assert(changes==before+1&&rooms[0].xyz[2]==2600);
    /* A limited edit shows the limit in the draft, and cancel still returns to
     * the last accepted size (a user edit, so it is allowed). */
    placement_max=700;*(float*)(draft+1*0x38+0x30)=10000;native_accept();grid_changed(panel,widget,1);
    assert(rooms[0].xyz[1]==700&&draft_value(1)==700&&*(float*)(widget+0xe8+4)==5120);
    assert(*(float*)(widget+0xf4+4)==5120&&entry(built,1)==5120&&entry(opened_draft,1)==5120);
    /* The limited value is drawn, and the row stays dirty (it is a user edit). */
    assert(drawn[1]==700&&widget[0xc8]==1);
    placement_max=0;
    /* Either cancel route then returns to the size before the user's edits:
     * the outside size the guard made the baseline (5120 x 5120 x 2500). */
    native_delete_revert(panel);
    assert(rooms[0].xyz[1]==5120&&rooms[0].xyz[2]==2500);
    placement_max=700;*(float*)(draft+1*0x38+0x30)=10000;*(float*)(draft+2*0x38+0x30)=2600;
    native_accept();grid_changed(panel,widget,1);
    assert(rooms[0].xyz[1]==700&&rooms[0].xyz[2]==2600&&entry(opened_draft,1)==5120);
    placement_max=0;
    native_dialog_yes(panel);
    assert(rooms[0].xyz[1]==5120&&rooms[0].xyz[2]==2500);
    /* A context that is not this panel's, or not a three-entry list, is left alone. */
    rooms[0].xyz[2]=2000;*(float*)(draft+2*0x38+0x30)=2600;*(int*)(ctx+0x60)=2;
    native_accept();grid_changed(panel,widget,1);
    assert(rooms[0].xyz[2]==2000&&*(float*)(widget+0x108)==2000&&draft_value(2)==2600);
    *(int*)(ctx+0x60)=3;*(void**)(widget+0xd8)=panel;rooms[0].xyz[2]=1900;
    grid_changed(panel,widget,1);
    assert(rooms[0].xyz[2]==1900&&draft_value(2)==2600);
    *(void**)(widget+0xd8)=NULL;grid_changed(panel,widget,1);
    *(void**)(ctx+0x58)=NULL;*(int*)(ctx+0x60)=0;*(int*)(ctx+0x64)=0;
    memset(ctx+0x70,0,0x28);
    g_dirty_refresh=NULL;g_menu_of=NULL;g_screen_by_index=NULL;g_widget_resolve=NULL;g_list_redraw=NULL;
    widget[0x29]=0;*(void**)(widget+0x48)=NULL;
    rows=saved_rows; /* the later row-count checks count only the main sequence */
}
int main(void)
{
    unsigned char panel[0x400]={0};sh_grid_size second;
    sh_grid_default(SH_GRID_MODERN,&rooms[0]);sh_grid_default(SH_GRID_MODERN,&rooms[1]);second=rooms[1];
    g_populate=native_populate;g_change=native_change;g_add_vec=add;g_set_vec=set_vec;
    g_hash=hash;g_read=read_room;g_apply=apply_room;
    *(void**)(panel+0x320)=input_ranges;*(int*)(panel+0x328)=3;*(int*)(panel+0x32c)=9;
    input_ranges[1]=input_ranges[3]=input_ranges[5]=63;
    grid_populate(panel,0,rooms,NULL,NULL);assert(rows==1);
    assert(input_ranges[0]==1&&input_ranges[1]==65534&&*(int*)(panel+0x328)==3);
    *(float*)(widget+0x100)=1536;*(float*)(widget+0x104)=1024;*(float*)(widget+0x108)=512;
    grid_changed(panel,widget,100);assert(changes==1&&rooms[0].xyz[0]==1536);
    assert(!memcmp(&rooms[1],&second,sizeof second));
    *(float*)(widget+0x108)=1;grid_changed(panel,widget,1);
    assert(changes==2&&rooms[0].xyz[2]==432&&*(float*)(widget+0x108)==432);
    *(float*)(widget+0x100)=500;*(float*)(widget+0x104)=500;
    *(float*)(widget+0x108)=20000;grid_changed(panel,widget,1);
    assert(changes==3&&rooms[0].xyz[0]==864&&rooms[0].xyz[1]==500&&rooms[0].xyz[2]==20000);
    assert(*(float*)(widget+0x100)==864&&*(float*)(widget+0x104)==500&&*(float*)(widget+0x108)==20000);
    *(float*)(widget+0x100)=1;grid_changed(panel,widget,1);
    assert(changes==3&&*(float*)(widget+0x100)==864);
    refuse_apply=1;*(float*)(widget+0x100)=2048;grid_changed(panel,widget,1);
    assert(changes==3&&*(float*)(widget+0x100)==864);refuse_apply=0;
    assert(!memcmp(&rooms[1],&second,sizeof second));
    *(int*)(widget+0x40)=5;grid_changed(panel,widget,-100);assert(native_changes==1);
    *(int*)(widget+0x40)=GRID_PROPERTY_SIZE;placement_max=700;
    *(float*)(widget+0x104)=10000;grid_changed(panel,widget,1);
    assert(rooms[0].xyz[1]==700&&*(float*)(widget+0x104)==700);placement_max=0;
    /* The room under an open panel changed size elsewhere (an undo, a console
     * edit, or a module index shift after a deletion): show it, apply nothing. */
    {unsigned before=changes;rooms[0].xyz[2]=1000;
        *(float*)(widget+0x108)=3000;grid_changed(panel,widget,1);
        assert(changes==before&&rooms[0].xyz[2]==1000&&*(float*)(widget+0x108)==1000);
        *(float*)(widget+0x108)=3000;grid_changed(panel,widget,1);
        assert(changes==before+1&&rooms[0].xyz[2]==3000);}
    sub_editor_draft_checks(panel);
    rooms[1].kind=SH_GRID_NONE;grid_populate(panel,1,rooms,NULL,NULL);assert(rows==1);
    grid_populate(panel,-1,rooms,NULL,NULL);assert(rows==1);
    /* Returning to ordinary Settings must restore its smaller range too. */
    {uint32_t text=1;float offset[3]={0};
        grid_add_vec(panel,11,"Grid Offset",&text,&text,1,offset,0,63,1,10,1,panel+0x280,NULL,NULL);
        assert(input_ranges[0]==0&&input_ranges[1]==63&&*(int*)(panel+0x328)==3);}
    assert(reads>=5);blueprint_tests();puts("grid_room_editor_test: passed");return 0;
}
