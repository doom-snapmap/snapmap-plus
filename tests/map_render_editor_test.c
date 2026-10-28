/* View Distance as a Snapmap+ settings-tab group: draft, adjust, commit and
 * the fault latch. The tab itself is stubbed here; its copy of draft to base
 * after a successful commit and base to draft on Cancel is simulated. */
#include "../src/backend/map_render_editor.c"
#include <assert.h>
#include <stdio.h>
static unsigned char map_a[8],map_b[8];
static sh_map_render saved_a,saved_b,selected;
static int writes,selects,rows_added,fail_add;
static sh_settings_group group;static int registered;
static float *row_draft,*row_base;static void (*row_adjust)(void *,float *,float);
static const char *row_name;
void backend_log(const char *s){(void)s;}
int sh_map_render_read(void *map,sh_map_render *s){*s=map==map_a?saved_a:saved_b;return 1;}
int sh_map_render_write(void *map,const sh_map_render *s){assert(map==map_a);saved_a=*s;writes++;return 1;}
void sh_map_render_select(const sh_map_render *s){selected=*s;selects++;}
int sh_settings_tab_register_group(const sh_settings_group *g){group=*g;registered++;return 1;}
int sh_settings_tab_add_float(sh_settings_rows *rows,const sh_settings_float *row,float *draft,float *base,
    void (*adjust)(void *,float *,float))
{
    (void)rows;if(fail_add)return 0;
    assert(row&&row->min==0&&row->max==SH_RENDER_MAX_DISTANCE&&row->small_step>0&&row->large_step>0);
    /* Created with the native wrap; render_adjust keeps only LEFT at zero. */
    assert(row->wrap);
    row_name=row->name;row_draft=draft;row_base=base;row_adjust=adjust;rows_added++;return 1;
}
static int open(void *map)
{
    int shown=group.begin(group.user,NULL,map);
    if(shown)group.populate(NULL,group.user);
    return shown;
}
static void change(float v){row_adjust(group.user,row_draft,v);}
static void apply(void){if(*row_draft!=*row_base&&group.commit(group.user,map_a))*row_base=*row_draft;}
int main(void)
{
    sh_map_render_default(&saved_a);sh_map_render_default(&saved_b);saved_b.value[0]=60000;
    assert(sh_map_render_editor_install(NULL,0)&&registered==1);
    assert(!strcmp(group.title,SH_RENDER_SETTINGS_TITLE)&&group.begin&&group.populate&&group.commit&&group.end);
    assert(sh_map_render_editor_install(NULL,0)&&registered==1);
    assert(open(map_a)&&rows_added==1&&!strcmp(row_name,"Map View Distance"));
    assert(*row_draft==8192&&*row_base==8192);
    change(60000);assert(*row_draft==60000&&saved_a.value[0]==8192&&writes==0);
    /* Cancel: the tab copies base back to draft. */
    *row_draft=*row_base;assert(*row_draft==8192&&writes==0);
    /* Slider values below the minimum snap to 256; zero stays zero. */
    change(100);assert(*row_draft==256);change(0);assert(*row_draft==0);
    /* LEFT at zero: the native wrap lands on the maximum and stays there. */
    change(SH_RENDER_MAX_DISTANCE);assert(*row_draft==SH_RENDER_MAX_DISTANCE);
    /* RIGHT at the maximum: the native wrap to zero is undone. */
    change(0);assert(*row_draft==SH_RENDER_MAX_DISTANCE);
    /* LEFT from a value above zero: the wrap to the maximum stops at zero. */
    *row_draft=2000;
    change(SH_RENDER_MAX_DISTANCE);assert(*row_draft==0);
    /* A step down to exactly zero is a step, then LEFT again wraps. */
    change(256);change(0);assert(*row_draft==0);
    change(SH_RENDER_MAX_DISTANCE);assert(*row_draft==SH_RENDER_MAX_DISTANCE);
    /* Cancel back to the base before the slider-value cases below. */
    *row_draft=*row_base;
    change(32768);apply();assert(writes==1&&saved_a.value[0]==32768&&*row_base==32768);
    /* Apply publishes to the editor view immediately. */
    assert(selects==1&&selected.value[0]==32768);
    group.end(group.user);assert(open(map_a)&&*row_draft==32768);
    change(99999);group.end(group.user);
    assert(open(map_b)&&*row_draft==60000&&writes==1);
    /* A commit for a map other than the open one is refused. */
    assert(!group.commit(group.user,map_a));
    group.end(group.user);assert(open(map_a)&&*row_draft==32768);
    change(8192);apply();assert(saved_a.value[0]==8192&&writes==2&&selects==2&&selected.value[0]==8192);
    /* Apply with nothing changed neither writes nor republishes. */
    apply();assert(writes==2&&selects==2);
    /* A failed row latches the fault: the group hides itself afterwards. */
    group.end(group.user);fail_add=1;assert(open(map_a));fail_add=0;
    group.end(group.user);assert(!open(map_a));
    puts("map_render_editor_test: passed");return 0;
}
