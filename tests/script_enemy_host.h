/* Included by script_e2e_test.c to drive the same simulated game and real Lua sandbox with a mod folder. */
#include "decl_tree.h"
#include "modauthor.h"
#include "script/script_api_body.h"
#include "../src/common/text.h"
#include "../src/common/wide_path.h"

static const char *eh_folder, *eh_actor;
static int eh_shared, eh_seen[12][4], eh_scans;
static char *eh_other;
typedef struct eh_wound_row {
    struct eh_wound_row *next;
    char *wound, *condition;
    unsigned seen;
} eh_wound_row;
static eh_wound_row *eh_wounds;
static const char *const eh_keys[] = {"spawn", "think", "damage", "ordinary"};
static const char eh_observer[] =
    "api = 1\n"
    "local sdk = require('enemy_sdk')\n"
    "local bind = sdk.bind\n"
    "sdk.bind = function(def, brain)\n"
    "  if probe_binding(def) then\n"
    "    for _, kind in ipairs({'spawn', 'think', 'damage'}) do\n"
    "      local fn = brain[kind]\n"
    "      brain[kind] = function(self, ...)\n"
    "        local result = fn(self, ...)\n"
    "        probe_seen(self, kind)\n"
    "        return result\n"
    "      end\n"
    "    end\n"
    "    on.spawn(def, function(self) probe_seen(self, 'ordinary') end)\n"
    "    if probe_shared() then bind('test/competing_definition', {}) end\n"
    "  end\n"
    "  return bind(def, brain)\n"
    "end\n";

static int eh_list(void *ctx, const char *salt, sh_script_file_fn cb, void *user)
{
    const sh_script_source *src = sh_script_source_files();
    (void)ctx;
    cb(user, "0000_host_probe.lua");
    return 1 + src->list(src->context, salt, cb, user);
}

static int eh_read(void *ctx, const char *salt, const char *rel, char **text, size_t *len)
{
    const sh_script_source *src = sh_script_source_files();
    (void)ctx;
    if (strcmp(rel, "0000_host_probe.lua")) return src->read(src->context, salt, rel, text, len);
    *text = _strdup(eh_observer);
    *len = sizeof eh_observer - 1;
    return *text ? 1 : -1;
}

static void eh_free(void *ctx, char *text) { (void)ctx; free(text); }
static const sh_script_source eh_source = {NULL, eh_list, eh_read, eh_free};

static FILE *eh_open(const char *path)
{
    wchar_t *utf16 = (wchar_t *)malloc(SH_WPATH_MAX * sizeof(wchar_t));
    wchar_t *full = (wchar_t *)malloc(SH_WPATH_MAX * sizeof(wchar_t));
    FILE *file = NULL;
    if (utf16 && full && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, utf16, SH_WPATH_MAX)) {
        DWORD n = GetFullPathNameW(utf16, SH_WPATH_MAX, full, NULL);
        if (n && n < SH_WPATH_MAX && sh_wpath_long(full, SH_WPATH_MAX)) _wfopen_s(&file, full, L"rb");
    }
    free(utf16);
    free(full);
    return file;
}

static int eh_decl(void *context, const char *type, const char *name, sh_dt **body)
{
    const char *salt = (const char *)context;
    char *request, *relative, *path, *text, *served;
    FILE *f = NULL;
    long size;
    int ok = 0;
    size_t len;
    if (eh_shared && eh_other && !strcmp(name, eh_other)) name = eh_actor;
    request = sh_textf("generated/decls/%s/%s.decl", type, name);
    relative = request ? sh_ma_path(salt, request) : NULL;
    free(request);
    path = relative ? sh_textf("%s/%s", eh_folder, relative) : NULL;
    free(relative);
    if (path) f = eh_open(path);
    free(path);
    if (!f) return 0;
    if (!body) { fclose(f); return 1; }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return 0; }
    text = (char *)malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, f) == (size_t)size) {
        text[size] = 0;
        served = sh_ma_text(salt, text, (size_t)size, NULL, NULL, &len);
        if (len != (size_t)-1) {
            *body = sh_dt_parse(served ? served : text, served ? len : (size_t)size, NULL, 0);
            ok = *body != NULL;
        }
        free(served);
    }
    free(text);
    fclose(f);
    return ok;
}

static char *eh_bound(const char *salt, const char *def)
{
    return sh_dt_enemy_behavior(salt, def, eh_decl, (void *)salt);
}

static const char *eh_live_behavior(void *entity)
{
    bent *e = (bent *)entity;
    return e->kind == SH_SCRIPT_KIND_AI ? e->behavior : NULL;
}

static int eh_binding(lua_State *L)
{
    const char *def = sh_script_check_name(L, 1, "probe_binding", 0, NULL);
    lua_pushboolean(L, !strcmp(def, eh_actor));
    return 1;
}

static int eh_is_shared(lua_State *L) { lua_pushboolean(L, eh_shared); return 1; }
static int eh_yes(lua_State *L) { lua_pushboolean(L, 1); return 1; }
static int eh_none(lua_State *L) { (void)L; return 0; }
static int eh_empty(lua_State *L) { lua_newtable(L); return 1; }

static int eh_wound_effect(lua_State *L)
{
    sh_script_handle h = (sh_script_handle)sh_script_check_entity(L, 1, "wound_effect");
    bent *e = (bent *)sh_script_handle_entity(h);
    const char *wound = luaL_checkstring(L, 2), *condition = luaL_checkstring(L, 3);
    eh_wound_row *r;
    if (!e || e < g_b || e >= g_b + g_bn) return luaL_error(L, "invalid wound effect receiver");
    for (r = eh_wounds; r; r = r->next)
        if (!strcmp(wound, r->wound) && !strcmp(condition, r->condition)) break;
    if (!r) {
        r = (eh_wound_row *)calloc(1, sizeof *r);
        if (!r) return luaL_error(L, "out of memory");
        r->next = eh_wounds; eh_wounds = r;
        r->wound = _strdup(wound); r->condition = _strdup(condition);
        if (!r->wound || !r->condition) return luaL_error(L, "out of memory");
    }
    unsigned bit = 1u << (e - g_b);
    if (r->seen & bit) return luaL_error(L, "wound effect registered twice");
    r->seen |= bit;
    return eh_yes(L);
}

static int eh_check_wounds(unsigned expected, int *count)
{
    int ok = 1;
    *count = 0;
    while (eh_wounds) {
        eh_wound_row *r = eh_wounds;
        eh_wounds = r->next;
        if (r->seen != expected) ok = 0;
        (*count)++;
        free(r->wound); free(r->condition); free(r);
    }
    return ok;
}

static int eh_count(lua_State *L)
{
    sh_script_handle h = (sh_script_handle)sh_script_check_entity(L, 1, "probe_seen");
    bent *e = (bent *)sh_script_handle_entity(h);
    const char *kind = lua_tostring(L, 2);
    int i;
    if (!e || !kind) return 0;
    for (i = 0; i < 4; i++)
        if (!strcmp(kind, eh_keys[i])) eh_seen[e - g_b][i]++;
    return 0;
}

static void eh_native(void)
{
    const char *const settings[] = {"wound_damage_counts", "wound_hides", "rear_footsteps", "posed_legs",
                                    "fk_reference_pose", "eternal_interrupts"};
    int i;
    sh_script_api_set_engine(&k_brute_world);
    sh_script_ai_set_engine(&k_brute_ai);
    sh_script_combat_set_engine(&k_brute_combat);
    sh_script_handles_set_resolver(b_resolve);
    sh_script_api_body_register();
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "probe_binding", eh_binding);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "probe_seen", eh_count);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "probe_shared", eh_is_shared);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "destructible", eh_yes);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "wound_effect", eh_wound_effect);
    for (i = 0; i < (int)(sizeof settings / sizeof settings[0]); i++)
        sh_script_api_add(SH_SCRIPT_API_METHOD, settings[i], eh_yes);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "snapmap_order", eh_none);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "relations", eh_empty);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "weapon_use", eh_none);
}

static void eh_actor_at(int index, const char *def, const char *behavior, int kind)
{
    bent *e = &g_b[index];
    memset(e, 0, sizeof *e);
    e->id = 601 + (uint32_t)index;
    e->kind = kind;
    e->alive = 1;
    e->ax[0] = e->ax[4] = e->ax[8] = 1;
    e->definition = def;
    e->behavior = behavior;
}

static sh_script_handle eh_spawn(int index, const char *named)
{
    bent *e = &g_b[index];
    return sh_script_record_actor(e, e->id, named ? &named : NULL, named ? 1 : 0, eh_live_behavior, NULL);
}

static void eh_scan(void)
{
    eh_scans++;
    eh_spawn(0, eh_actor);
    eh_spawn(1, NULL);
}

static int eh_drove(int idx, int ordinary)
{
    return eh_seen[idx][0] == 1 && eh_seen[idx][1] && eh_seen[idx][2] == 1 && eh_seen[idx][3] == ordinary;
}

static const char *eh_bool(int condition) { return condition ? "true" : "false"; }

static int enemy_host(const char *folder, const char *salt, const char *actor, const char *behavior)
{
    sh_script_mod_desc mod = {salt, "Enemy Host"};
    sh_script_handle h, pre = 0;
    char *actual, *scripts;
    int named = 0, direct = 0, stock = 0, absent = 0, shared = 0;
    int pass, i, bound, success, wounds[2] = {0, 0};
    eh_folder = folder;
    eh_actor = actor;
    eh_native();
    sh_script_source_clear();
    scripts = sh_textf("%s\\scripts", folder);
    if (scripts) for (char *s = scripts; *s; s++) if (*s == '/') *s = '\\';
    eh_other = sh_textf("mods/%s/test/competing_definition", salt);
    actual = eh_bound(salt, actor);
    bound = actual && !_stricmp(actual, behavior) && scripts && sh_script_source_set_folder(salt, mod.name, scripts);
    if (!bound) fprintf(stderr, "Could not load the declared actor and scripts. Resolved behavior: %s\n",
                        actual ? actual : "(none)");
    free(scripts);
    free(actual);
    sh_script_set_enemy_ops(eh_bound, eh_scan);
    for (pass = 0; bound && pass < 2; pass++) {
        char owner[SH_SCRIPT_SALT_CAP];
        int okay;
        eh_shared = pass;
        eh_scans = 0;
        memset(eh_seen, 0, sizeof eh_seen);
        g_bn = 8;
        eh_actor_at(0, actor, behavior, SH_SCRIPT_KIND_AI);
        eh_actor_at(1, "map/local", behavior, SH_SCRIPT_KIND_AI);
        eh_actor_at(2, "map/later", behavior, SH_SCRIPT_KIND_AI);
        eh_actor_at(3, "stock/actor", "stock/behavior", SH_SCRIPT_KIND_AI);
        eh_actor_at(4, "map/absent", NULL, SH_SCRIPT_KIND_AI);
        eh_actor_at(5, "map/prop", behavior, SH_SCRIPT_KIND_OTHER);
        eh_actor_at(6, "mods/othersalt/actor", behavior, SH_SCRIPT_KIND_AI);
        eh_actor_at(7, "map/unbound", "mods/unused/other", SH_SCRIPT_KIND_AI);
        if (sh_script_map_prepare(&mod, 1, &eh_source, 42) != 1) break;
        if (pre && sh_script_handle_known(pre)) break;
        pre = sh_script_handles_ref(g_b[1].id);
        okay = !eh_spawn(1, NULL);
        brute_frame(16);
        okay &= eh_scans == 1 && sh_script_status(salt) == SH_SCRIPT_RUNNING;
        h = eh_spawn(2, NULL);
        if (!pass) {
            direct = okay && h && pre == eh_spawn(1, NULL) &&
                     sh_script_entity_owner(pre, owner, sizeof owner) && !_stricmp(owner, salt);
            stock = okay && !eh_spawn(3, NULL) && !eh_spawn(5, NULL) && !eh_spawn(6, g_b[6].definition) &&
                    !sh_script_record_actor(&g_b[1], g_b[1].id, NULL, -1, eh_live_behavior, NULL);
            absent = okay && !eh_spawn(4, NULL) && !eh_spawn(7, NULL);
        }
        brute_frame(16);
        for (i = 0; i < 3; i++) {
            int slot;
            h = sh_script_handles_find(g_b[i].id, &slot);
            if (h && slot >= 0) sh_script_queue_post(slot, SH_SCRIPT_EV_DAMAGE, h, 0, 0, 37, "probe/hit", NULL);
        }
        brute_frame(16);
        okay &= sh_script_status(salt) == SH_SCRIPT_RUNNING;
        okay &= eh_check_wounds(pass ? 1u : 7u, &wounds[pass]);
        if (!pass) {
            named = okay && eh_drove(0, 1);
            direct &= okay && eh_drove(1, 0) && eh_drove(2, 0);
        } else {
            shared = okay && eh_drove(0, 1) && !eh_seen[1][0] && !eh_seen[2][0] && !eh_spawn(2, NULL) &&
                     !sh_script_entity_owner(pre, owner, sizeof owner);
        }
        if (!okay) fprintf(stderr, "%s", log_of(salt));
        sh_script_map_close(1);
        if (sh_script_enemy_bindings_ready() || sh_script_entity_owner(pre, owner, sizeof owner)) shared = direct = 0;
    }
    free(eh_other);
    eh_other = NULL;
    eh_shared = 0;
    sh_script_set_enemy_ops(NULL, NULL);
    success = named && direct && stock && absent && shared && wounds[0] == wounds[1];
    printf("{\"ok\":%s,\"named\":%s,\"direct\":%s,\"stock\":%s,\"absent\":%s,\"shared\":%s,\"wound_effects\":%d}\n",
           eh_bool(success), eh_bool(named), eh_bool(direct), eh_bool(stock), eh_bool(absent), eh_bool(shared), wounds[0]);
    return !success;
}

static void test_enemy_bindings(void)
{
    sh_script_mod_desc mods[] = {{"futurea", "First"}, {"futureb", "Second"}};
    sh_script_handle a, b, overridden;
    char salt[SH_SCRIPT_SALT_CAP];
    char *folder = sh_textf("%s/enemy-reuse", g_base);
    CHECK(folder != NULL);
    if (!folder) return;
    CHECK(enemy_host(folder, "futurea", "mods/futurea/actors/placed", "mods/futurea/decisions/advance") == 0);
    CHECK(enemy_host(folder, "futureb", "mods/futureb/actors/placed", "mods/futureb/decisions/advance") == 0);
    eh_folder = folder;
    sh_script_set_enemy_ops(eh_bound, NULL);
    sh_script_source_clear();
    set_folder("futurea", "First", "enemy-reuse");
    set_folder("futureb", "Second", "enemy-reuse");
    CHECK(sh_script_map_start(mods, 2, sh_script_source_files(), 42, 0) == 2);
    g_bn = 3;
    eh_actor_at(0, "map/a", "mods/futurea/decisions/advance", SH_SCRIPT_KIND_AI);
    eh_actor_at(1, "map/b", "mods/futureb/decisions/advance", SH_SCRIPT_KIND_AI);
    eh_actor_at(2, "mods/futurea/actors/override", "mods/futureb/decisions/advance", SH_SCRIPT_KIND_AI);
    a = eh_spawn(0, NULL);
    b = eh_spawn(1, NULL);
    overridden = eh_spawn(2, g_b[2].definition);
    brute_frame(16);
    CHECK(a && b && a != b && sh_script_entity_owner(a, salt, sizeof salt) && !strcmp(salt, "futurea"));
    CHECK(sh_script_entity_owner(b, salt, sizeof salt) && !strcmp(salt, "futureb"));
    CHECK(sh_script_entity_owner(overridden, salt, sizeof salt) && !strcmp(salt, "futurea"));
    for (int i = 0; i < 2; i++) {
        sh_script_mod_info info;
        CHECK(sh_script_mod_info_get(mods[i].salt, &info) && info.entities == 1);
        sh_script_mod_info_free(&info);
    }
    sh_script_map_close(1);
    sh_script_set_enemy_ops(NULL, NULL);
    free(folder);
}
