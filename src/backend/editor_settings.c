#include "editor_settings.h"
#include "editor_sheets.h"
#include "editor_panel_schema.h"
#include "settings_tab.h"
#include "modserve.h"
#include "config.h"
#include "map_settings.h"
#include "strids.h"
#include "editor_undo.h"

void backend_log(const char *message);


typedef struct setting_row {
    char *key, *label, *help, *caption;
    int draft, base;
} setting_row;

typedef struct settings_group {
    int map_scope, failed;
    const char *locale;
    sh_json_object prefs;
    setting_row *rows;
    size_t count, capacity;
} settings_group;

static settings_group personal, saved = {1};
static int needs_refresh;

void sh_editor_settings_after_close(void)
{
    if (needs_refresh) {
        needs_refresh = 0;
        (void)sh_modserve_refresh_after_rescan(NULL);
    }
}

static void release_rows(void *user)
{
    settings_group *g = (settings_group *)user;
    size_t i;
    for (i = 0; i < g->count; ++i) {
        setting_row *r = &g->rows[i];
        free(r->key);
        free(r->label);
        free(r->help);
        free(r->caption);
    }
    free(g->rows);
    sh_json_object_free(&g->prefs);
    g->rows = NULL;
    g->count = g->capacity = 0;
    g->failed = 0;
}

static int collect_row(settings_group *g, const char *salt, const sh_json_member *m,
                       const sh_json_object *en, const sh_json_object *local)
{
    sh_json_object fields = {0};
    char *title = NULL, *desc = NULL;
    const char *scope, *text, *pref;
    setting_row *r;
    int ok = 0, def;
    if (!ep_object(m->value_json, &fields)) goto done;
    scope = sh_json_object_get(&fields, "scope");
    if (!scope || strcmp(scope, g->map_scope ? "\"map\"" : "\"editor\"")) { ok = 1; goto done; }
    if (g->count == g->capacity) {
        size_t cap = g->capacity ? g->capacity * 2 : 16;
        setting_row *next = (setting_row *)realloc(g->rows, cap * sizeof *next);
        if (!next) goto done;
        g->rows = next;
        g->capacity = cap;
    }
    title = ep_string(sh_json_object_get(&fields, "title"));
    desc = ep_string(sh_json_object_get(&fields, "description"));
    if (!title) goto done;
    r = &g->rows[g->count++];
    memset(r, 0, sizeof *r);
    r->key = sh_textf("%s/%s", salt, m->key);
    r->label = ep_tag(salt, title);
    r->help = ep_tag(salt, desc ? desc : title);
    if (!r->key || !r->label || !r->help) goto done;
    text = sh_json_object_get(local, r->label + 5);
    if (!text) text = sh_json_object_get(en, r->label + 5);
    r->caption = sh_strids_label(r->label);
    if (!r->caption) r->caption = text ? ep_string(text) : _strdup(m->key);
    if (!r->caption) goto done;
    text = sh_json_object_get(&fields, "default");
    def = text && !strcmp(text, "true");
    pref = sh_json_object_get(&g->prefs, r->key);
    r->base = r->draft = g->map_scope ? sh_map_settings_bool(salt, m->key, def) :
        pref && (!strcmp(pref, "true") || !strcmp(pref, "false")) ? !strcmp(pref, "true") : def;
    ok = 1;
done:
    sh_json_object_free(&fields);
    free(title);
    free(desc);
    return ok;
}

static void collect(void *ctx, const char *salt, const char *hash, int place, const char *extra)
{
    settings_group *g = (settings_group *)ctx;
    sh_json_object root = {0}, ui = {0}, settings = {0}, strings = {0}, en = {0}, loc = {0};
    size_t i;
    sh_modserve_scope_info scope;
    if (g->map_scope && sh_modserve_scope_enabled() &&
        (!sh_modserve_bundle_scope(hash, &scope) || !scope.listed)) return;
    if (g->failed || place || !ep_object(extra, &root) ||
        !ep_object(sh_json_object_get(&root, "editor"), &ui) ||
        !ep_object(sh_json_object_get(&ui, "settings"), &settings)) goto done;
    ep_object(sh_json_object_get(&root, "strings"), &strings);
    ep_object(sh_json_object_get(&strings, "en"), &en);
    if (strcmp(g->locale, "en")) ep_object(sh_json_object_get(&strings, g->locale), &loc);
    for (i = 0; i < settings.count; ++i) {
        if (!collect_row(g, salt, &settings.members[i], &en, &loc)) { g->failed = 1; break; }
    }
done:
    sh_json_object_free(&root);
    sh_json_object_free(&ui);
    sh_json_object_free(&settings);
    sh_json_object_free(&strings);
    sh_json_object_free(&en);
    sh_json_object_free(&loc);
}

static char *read_preferences(void)
{
    int n = sh_config_get_json("editor.settings", NULL, 0, NULL);
    char *raw = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
    if (raw && sh_config_get_json("editor.settings", raw, n + 1, NULL) == n) return raw;
    free(raw);
    return NULL;
}

static int begin_group(void *user, void *editor, void *map)
{
    settings_group *g = (settings_group *)user;
    (void)editor;
    (void)map;
    release_rows(g);
    g->locale = sh_strids_locale();
    if (!g->map_scope) {
        char *raw = read_preferences();
        if (!ep_object(raw, &g->prefs)) {
            free(raw);
            return 0;
        }
        free(raw);
    }
    if (sh_modserve_each_local_editor(collect, g) < 0 || g->failed) {
        backend_log("SETTINGS: could not read the installed editor settings");
        release_rows(g);
        return 0;
    }
    if (!g->count) release_rows(g);
    return g->count != 0;
}

static void populate(sh_settings_rows *rows, void *user)
{
    settings_group *g = (settings_group *)user;
    size_t i;
    for (i = 0; i < g->count; ++i) {
        setting_row *r = &g->rows[i];
        sh_settings_bool toggle = {r->caption, r->label, r->help};
        if (!sh_settings_tab_add_bool(rows, &toggle, &r->draft, &r->base)) {
            backend_log("SETTINGS: the native panel could not add an authored toggle");
            break;
        }
    }
}

static int commit(void *user, void *map)
{
    settings_group *g = (settings_group *)user;
    sh_json_object changes = {0};
    size_t i;
    int ok = 0;
    (void)map;
    for (i = 0; i < g->count; ++i) {
        setting_row *r = &g->rows[i];
        if (r->base != r->draft && !sh_json_object_set(&changes, r->key, r->draft ? "true" : "false", 1)) goto done;
    }
    ok = !changes.count || (g->map_scope ? sh_map_settings_apply_bools(&changes) : sh_config_apply_editor_settings(&changes));
    if (ok && changes.count) {
        if (g->map_scope) (void)sh_editor_mark_modified_no_undo();
        else if (sh_modserve_editor_preferences_changed() > 0) needs_refresh = 1;
    }
done:
    if (!ok) backend_log("SETTINGS: the authored toggle changes could not be saved");
    sh_json_object_free(&changes);
    return ok;
}

void sh_editor_settings_prepare(void)
{
    sh_editor_sheets_set_preferences(read_preferences);
}

int sh_editor_settings_register(void)
{
    sh_settings_group editor = {"Editor settings", begin_group, populate, commit, release_rows, &personal};
    sh_settings_group map = {"Map settings", begin_group, populate, commit, release_rows, &saved};
    return sh_settings_tab_register_group(&editor) && sh_settings_tab_register_group(&map);
}
