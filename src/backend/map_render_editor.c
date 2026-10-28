/* "Unlock View Distance" as an on/off row of the Snapmap+ settings tab
 * (settings_tab.c owns the native Settings / Properties hooks). The draft
 * belongs to the map open in the editor; native Apply commits it, Cancel /
 * Revert restores it. On stores the setting in the map; off removes it. */
#include "map_render.h"
#include "settings_tab.h"
#include <windows.h>
#include <string.h>

void backend_log(const char *message);
static void *g_map;
static int g_draft,g_base;
static int g_installed;
static LONG g_faulted;

static int render_begin(void *user,void *editor,void *map)
{
    sh_map_render s;(void)user;(void)editor;g_map=NULL;
    if(!g_installed||InterlockedCompareExchange(&g_faulted,0,0))return 0;
    __try {
        if(!sh_map_render_read(map,&s))return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);return 0;}
    g_map=map;g_base=g_draft=sh_map_render_unlocked(&s);
    return 1;
}
static void render_populate(sh_settings_rows *rows,void *user)
{
    static const sh_settings_bool row={"Unlock View Distance","#str_smp_render_unlock",
        "#str_smp_render_unlock_help"};
    (void)user;
    if(!g_map||InterlockedCompareExchange(&g_faulted,0,0))return;
    if(!sh_settings_tab_add_bool(rows,&row,&g_draft,&g_base)) {
        InterlockedExchange(&g_faulted,1);
        backend_log("RENDER: Unlock View Distance row unavailable (toggle row creator missing)");
    }
}
static int render_commit(void *user,void *map)
{
    sh_map_render s;int ok=0;(void)user;
    if(!map||map!=g_map||InterlockedCompareExchange(&g_faulted,0,0))return 0;
    sh_map_render_from_toggle(g_draft,&s);
    __try {
        /* The editor view renders this map too, so the applied setting
         * takes effect now instead of at the next Save, Play or load. */
        if(sh_map_render_write(map,&s)) {
            sh_map_render_select(&s);
            ok=1;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&g_faulted,1);}
    return ok;
}
static void render_end(void *user){(void)user;g_map=NULL;}

int sh_map_render_editor_install(const sig_result *results,size_t count)
{
    sh_settings_group group={SH_RENDER_SETTINGS_TITLE,render_begin,render_populate,render_commit,render_end,NULL};
    (void)results;(void)count;
    if(g_installed)return 1;
    if(!sh_settings_tab_register_group(&group)) {
        backend_log("RENDER: Unlock View Distance could not join the Snapmap+ settings tab");return 0;
    }
    g_installed=1;
    backend_log("RENDER: Unlock View Distance registered in the Snapmap+ settings tab");return 1;
}
