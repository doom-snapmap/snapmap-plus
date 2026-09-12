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
    rooms[1].kind=SH_GRID_NONE;grid_populate(panel,1,rooms,NULL,NULL);assert(rows==1);
    grid_populate(panel,-1,rooms,NULL,NULL);assert(rows==1);
    /* Returning to ordinary Settings must restore its smaller range too. */
    {uint32_t text=1;float offset[3]={0};
        grid_add_vec(panel,11,"Grid Offset",&text,&text,1,offset,0,63,1,10,1,panel+0x280,NULL,NULL);
        assert(input_ranges[0]==0&&input_ranges[1]==63&&*(int*)(panel+0x328)==3);}
    assert(reads>=5);blueprint_tests();puts("grid_room_editor_test: passed");return 0;
}
