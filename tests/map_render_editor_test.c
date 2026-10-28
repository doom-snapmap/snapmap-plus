/* "Unlock View Distance" as a Snapmap+ settings-tab toggle: draft, commit,
 * the fault latch. The tab itself is stubbed here; its copy of draft to base
 * after a successful commit and base to draft on Cancel is simulated. */
#include "../src/backend/map_render_editor.c"
#include <assert.h>
#include <stdio.h>
static unsigned char map_a[8],map_b[8];
static sh_map_render saved_a,saved_b,selected;
static int writes,selects,rows_added,fail_add,fail_write;
static sh_settings_group group;static int registered;
static int *row_draft,*row_base;
static const char *row_name,*row_label,*row_help;
void backend_log(const char *s){(void)s;}
int sh_map_render_read(void *map,sh_map_render *s){*s=map==map_a?saved_a:saved_b;return 1;}
int sh_map_render_write(void *map,const sh_map_render *s)
{assert(map==map_a);if(fail_write)return 0;saved_a=*s;writes++;return 1;}
void sh_map_render_select(const sh_map_render *s){selected=*s;selects++;}
int sh_settings_tab_register_group(const sh_settings_group *g){group=*g;registered++;return 1;}
int sh_settings_tab_add_bool(sh_settings_rows *rows,const sh_settings_bool *row,int *draft,int *base)
{
    (void)rows;if(fail_add)return 0;
    row_name=row->name;row_label=row->label;row_help=row->help;row_draft=draft;row_base=base;rows_added++;return 1;
}
static int open(void *map)
{
    int shown=group.begin(group.user,NULL,map);
    if(shown)group.populate(NULL,group.user);
    return shown;
}
static void apply(void){if(*row_draft!=*row_base&&group.commit(group.user,map_a))*row_base=*row_draft;}
int main(void)
{
    sh_map_render_default(&saved_a);sh_map_render_from_toggle(1,&saved_b);
    assert(sh_map_render_editor_install(NULL,0)&&registered==1);
    assert(!strcmp(group.title,SH_RENDER_SETTINGS_TITLE)&&group.begin&&group.populate&&group.commit&&group.end);
    assert(sh_map_render_editor_install(NULL,0)&&registered==1);
    /* One toggle row with its own label and help ids. */
    assert(open(map_a)&&rows_added==1&&!strcmp(row_name,"Unlock View Distance"));
    assert(!strcmp(row_label,"#str_smp_render_unlock")&&!strcmp(row_help,"#str_smp_render_unlock_help"));
    assert(*row_draft==0&&*row_base==0);
    /* Toggling only changes the draft; Cancel copies the base back. */
    *row_draft=1;assert(!saved_a.unlocked&&writes==0);
    *row_draft=*row_base;assert(*row_draft==0&&writes==0);
    /* Apply on stores on and publishes it to the editor view at once. */
    *row_draft=1;apply();
    assert(writes==1&&saved_a.unlocked==1&&*row_base==1&&selects==1&&selected.unlocked==1);
    group.end(group.user);assert(open(map_a)&&*row_draft==1&&*row_base==1);
    /* A map saved on opens with the row on. */
    group.end(group.user);assert(open(map_b)&&*row_draft==1&&writes==1);
    assert(!group.commit(group.user,map_a));
    group.end(group.user);assert(open(map_a));
    /* Apply off removes the setting (sh_map_render_write gets off). */
    *row_draft=0;apply();
    assert(writes==2&&saved_a.unlocked==0&&*row_base==0&&selects==2&&selected.unlocked==0);
    /* Apply with nothing changed neither writes nor republishes. */
    apply();assert(writes==2&&selects==2);
    /* A failed write is not committed and is not published. */
    *row_draft=1;fail_write=1;apply();fail_write=0;
    assert(writes==2&&selects==2&&*row_base==0);
    *row_draft=*row_base;
    /* A failed row latches the fault: the group hides itself afterwards. */
    group.end(group.user);fail_add=1;assert(open(map_a));fail_add=0;
    group.end(group.user);assert(!open(map_a));
    puts("map_render_editor_test: passed");return 0;
}
