#include "../src/backend/editor_settings.h"
#include "../src/backend/editor_panel_schema.h"
#include "../src/backend/settings_tab.h"
#include "../src/backend/modserve.h"
#include <assert.h>

static sh_settings_group groups[2];
static int ng, dirty, config_writes, map_writes, row_count, *draft, *base, listed = 1;
static const char *expect_name;
static const char * const extra = "{\"editor\":{\"settings\":{"
    "\"night\":{\"scope\":\"map\",\"title\":\"night_name\",\"description\":\"help\",\"default\":true},"
    "\"guides\":{\"scope\":\"editor\",\"title\":\"guide_name\"}}},"
    "\"strings\":{\"en\":{\"aaaaaaaa_night_name\":\"Night mode\",\"aaaaaaaa_guide_name\":\"Guides\"},"
    "\"it\":{\"aaaaaaaa_guide_name\":\"Guide\"}}}";

void backend_log(const char *msg) { puts(msg); }
int sh_editor_mark_modified_no_undo(void) { ++dirty; return 1; }
int sh_modserve_scope_enabled(void) { return 1; }
int sh_modserve_bundle_scope(const char *hash, sh_modserve_scope_info *out)
{
    (void)hash;
    memset(out, 0, sizeof *out);
    out->listed = listed;
    return 1;
}
static int refreshes;
int sh_modserve_editor_preferences_changed(void) { assert(config_writes); return 1; }
int sh_modserve_refresh_after_rescan(sh_modserve_refresh *out) { (void)out; ++refreshes; return 1; }
void sh_editor_sheets_set_preferences(char *(*read)(void)) { char *p = read(); assert(p && !strcmp(p, "{}")); free(p); }
const char *sh_strids_locale(void) { return "it"; }
char *sh_strids_label(const char *tag) { (void)tag; return NULL; }
int sh_settings_tab_register_group(const sh_settings_group *group)
{
    assert(ng < 2);
    groups[ng++] = *group;
    return 1;
}
int sh_modserve_each_local_editor(sh_modserve_extra_fn cb, void *ctx)
{
    cb(ctx, "aaaaaaaa", "unused", 0, extra);
    cb(ctx, "bbbbbbbb", "ignored", 1, extra);
    return 1;
}
int sh_config_get_json(const char *key, char *out, int cap, unsigned int *flags)
{
    (void)flags;
    assert(!strcmp(key, "editor.settings"));
    if (out && cap >= 3) strcpy(out, "{}");
    return 2;
}
int sh_map_settings_bool(const char *salt, const char *key, int def)
{
    assert(!strcmp(salt,"aaaaaaaa") && !strcmp(key,"night"));
    return def;
}
int sh_map_settings_apply_bools(const sh_json_object *updates)
{
    assert(updates->count == 1 && !strcmp(sh_json_object_get(updates,"aaaaaaaa/night"),"false"));
    ++map_writes;
    return 1;
}
int sh_config_apply_editor_settings(const sh_json_object *updates)
{
    assert(updates->count == 1 && !strcmp(sh_json_object_get(updates,"aaaaaaaa/guides"),"true"));
    ++config_writes;
    return 1;
}
int sh_settings_tab_add_bool(sh_settings_rows *unused, const sh_settings_bool *r, int *d, int *b)
{
    (void)unused;
    assert(!strcmp(expect_name, r->name) && !strncmp(r->label, "#str_aaaaaaaa_", 14));
    assert(!strncmp(r->help, "#str_aaaaaaaa_", 14));
    draft = d;
    base = b;
    ++row_count;
    return 1;
}
int main(void)
{
    char cause[160];
    int i;
    sh_editor_settings_prepare();
    assert(sh_editor_settings_register() && ng == 2);
    for (i = 0; i < ng; ++i) {
        sh_settings_group *g = &groups[i];
        expect_name = i ? "Night mode" : "Guide";
        assert(g->begin(g->user, NULL, NULL));
        g->populate(NULL, g->user);
        assert(*draft == i && *base == i);
        *draft = !i;
        g->end(g->user);
        assert(config_writes + map_writes == i);
        assert(g->begin(g->user, NULL, NULL));
        g->populate(NULL, g->user);
        assert(*draft == i);
        *draft = !i;
        assert(g->commit(g->user, NULL));
        assert(dirty == i);
        g->end(g->user);
    }
    listed = 0;
    assert(!groups[1].begin(groups[1].user, NULL, NULL));
    assert(groups[0].begin(groups[0].user, NULL, NULL));
    groups[0].end(groups[0].user);
    assert(map_writes == 1 && config_writes == 1 && row_count == 4 && !refreshes);
    sh_editor_settings_after_close();
    sh_editor_settings_after_close();
    assert(refreshes == 1);
    assert(!sh_editor_settings_validate("{\"bad\":{\"title\":\"name\",\"scope\":\"user\"}}", cause, sizeof cause));
    assert(!sh_editor_settings_validate("{\"bad\":{\"title\":\"name\",\"scope\":\"map\",\"default\":1}}", cause, sizeof cause));
    puts("editor_settings_test passed");
    return 0;
}
