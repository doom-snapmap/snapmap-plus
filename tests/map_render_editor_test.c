#include "../src/backend/map_render_editor.c"
#include <assert.h>
#include <stdio.h>
static unsigned char editor[0x23700],menu[0xa00],map_a[8],map_b[8],panel[0x400];
static unsigned char widgets[SH_RENDER_FIELDS][0x140];
static unsigned char bool_widget[0xe0];
static void *panel_rows[1]={bool_widget};
static sh_map_render saved_a,saved_b;
static int row_count,writes,native_applies,native_resets;
void backend_log(const char *s){(void)s;}
int sh_grid_editor_set_property_handler(sh_native_property_handler h){return h!=NULL;}
int sh_map_render_read(void *map,sh_map_render *s){*s=map==map_a?saved_a:saved_b;return 1;}
int sh_map_render_write(void *map,const sh_map_render *s){assert(map==map_a);saved_a=*s;writes++;return 1;}
static void native_state(void *a,void *b){(void)a;(void)b;}
static void native_populate(void *p,int c){assert(p==panel);(void)c;row_count=0;*(int*)(panel+0x200)=0;}
static void native_apply(void *p){assert(p==editor+0x204d8);native_applies++;}
static void native_reset(void *p){assert(p==editor+0x204d8);native_resets++;}
static unsigned char native_dirty(void *m){(void)m;return 0;}
static void native_title(void *p,const char *s){assert(p==panel&&s);}
static uint32_t native_hash(const char *s){assert(s);return 1;}
static void *native_add(void *p,const char *name,const uint32_t *label,const uint32_t *help,
    unsigned char enabled,float value,float min,float max,float step,float big,unsigned char input)
{
    unsigned char *v=widgets[row_count++];
    assert(p==panel&&name&&label&&help&&input&&step>0&&big>0&&value>=min&&value<=max);(void)enabled;
    memset(v,0,0x140);*(float*)(v+0x120)=value;return v;
}
static void native_bool(void *p,unsigned id,const char *name,const uint32_t *label,const uint32_t *help,
    unsigned char enabled,unsigned char value,const char *yes,const char *no)
{
    assert(p==panel&&id==RENDER_CUSTOM&&name&&label&&help&&enabled&&yes&&no);
    memset(bool_widget,0,sizeof bool_widget);*(unsigned*)(bool_widget+0x40)=id;bool_widget[0xd0]=value;
    *(void***)(panel+0x1f8)=panel_rows;*(int*)(panel+0x200)=1;
}
static void native_enable(void *w,unsigned char enabled){((unsigned char*)w)[0x28]=enabled;}
static void native_bool_refresh(void *w){assert(w==bool_widget);}
static void custom(int value){bool_widget[0xd0]=(unsigned char)value;assert(changed(panel,bool_widget,1));}
static void open(void *map)
{
    *(void**)(editor+0x204c8)=map;enter(NULL,editor);populate(panel,0);assert(row_count==SH_RENDER_FIELDS);
}
int main(void)
{
    sh_map_render_default(&saved_a);sh_map_render_default(&saved_b);saved_b.value[0]=8192;
    editor[8]=1;*(int*)(editor+0x23618)=5;*(void**)(editor+0x21088)=menu;
    *(void**)(menu+0x9e0)=menu;g_installed=1;g_enter=native_state;g_exit=native_state;g_populate=native_populate;
    g_apply=native_apply;g_reset=native_reset;g_dirty=native_dirty;g_title=native_title;g_hash=native_hash;g_add=native_add;
    g_add_bool=native_bool;g_enable=native_enable;g_bool_refresh=native_bool_refresh;
    open(map_a);assert(!dirty(menu));
    assert(!widgets[0][0x28]&&!g_draft.custom);
    custom(1);assert(dirty(menu)&&widgets[0][0x28]);
    *(float*)(widgets[1]+0x120)=80;assert(changed(panel,widgets[1],1));
    assert(dirty(menu)&&saved_a.value[1]==0&&writes==0);
    populate(panel,1);assert(row_count==0&&dirty(menu));
    populate(panel,0);assert(*(float*)(widgets[1]+0x120)==80&&dirty(menu));
    reset(editor+0x204d8);assert(!dirty(menu)&&!g_draft.custom&&!bool_widget[0xd0]&&!widgets[0][0x28]&&g_draft.value[1]==0&&writes==0);
    custom(1);
    *(float*)(widgets[1]+0x120)=60;changed(panel,widgets[1],1);
    apply(editor+0x204d8);assert(writes==1&&saved_a.value[1]==60&&!dirty(menu));
    leave(NULL,editor);open(map_a);assert(g_draft.value[1]==60);
    *(float*)(widgets[1]+0x120)=99;changed(panel,widgets[1],1);leave(NULL,editor);
    open(map_b);assert(g_draft.value[0]==8192&&g_draft.value[1]==0&&writes==1);
    leave(NULL,editor);open(map_a);assert(g_draft.value[1]==60);
    custom(0);apply(editor+0x204d8);assert(!saved_a.custom&&saved_a.value[1]==60&&writes==2);
    leave(NULL,editor);open(map_a);assert(!g_draft.custom&&g_draft.value[1]==60);
    assert(native_applies==2&&native_resets==1);puts("map_render_editor_test: passed");return 0;
}
