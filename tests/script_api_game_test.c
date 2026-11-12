/* script_api_game_test.c -- the calls that reach the whole game
 * (src/backend/script/script_api_game.c) against a fake engine: engine
 * events by name with every kind of value and result, SnapMap events fired
 * and heard, cvars set and put back at map end, decls read and written
 * through the reflection of reflect_fake.h and put back at map end, and each
 * refusal's sentence. */
#include "heap_out.h"
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script/script_runtime.h"
#include "script/script_hooks.h"
#include "script/script_api_common.h"
#include "script/script_api_reflect.h"
#include "script/script_api_game.h"
#include "reflect_fake.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
#define SALT "gametst1"

static char g_text[16384];
static int src_list(void *ctx, const char *salt, sh_script_file_fn fn, void *fnctx)
{
    (void)ctx;
    if (strcmp(salt, SALT)) return 0;
    fn(fnctx, "main.lua");
    return 1;
}
static int src_read(void *ctx, const char *salt, const char *rel, char **text, size_t *len)
{
    (void)ctx;
    if (strcmp(salt, SALT) || _stricmp(rel, "main.lua")) return 0;
    *len = strlen(g_text);
    *text = (char *)malloc(*len + 1);
    memcpy(*text, g_text, *len + 1);
    return 1;
}
static void src_release(void *ctx, char *text) { (void)ctx; free(text); }
static const sh_script_source g_src = { NULL, src_list, src_read, src_release };

/* ---- the world ---- */
#define ID_OWN   0x10000u
#define ID_OTHER 77u
static demon g_own, g_other;
static decl g_enc = { NULL, "mods/" SALT "/enc/tyrant", 9 }, g_enc2 = { NULL, "enc/baron", 4 };
static char g_events[8192];

static void *f_resolve(uint32_t id) { return id == ID_OWN ? (void *)&g_own : id == ID_OTHER ? (void *)&g_other : NULL; }
static uint32_t f_spawn_id(void *ent) { return ent == &g_own ? ID_OWN : ent == &g_other ? ID_OTHER : SH_SCRIPT_SPAWN_ID_NONE; }
static int f_slots(void) { return 2; }
static uint32_t f_slot_spawn_id(int n) { return n == 0 ? ID_OWN : n == 1 ? ID_OTHER : SH_SCRIPT_SPAWN_ID_NONE; }
static int f_kind(void *ent) { (void)ent; return SH_SCRIPT_KIND_AI; }

static const struct { const char *name, *format; } k_formats[] = {
    { "setOrigin", "v" }, { "activate", "e" }, { "damage", "daffeb" }, { "getHealth", "" }, { "getName", "" },
    { "getEnemy", "" }, { "getDamageDecl", "" }, { "getAngles", "" }, { "ae_sound", "4d" }, { "setColor", "c" },
    { "notice_hit", "ri" }, { "ae_playAnim", "41s2" }, { "notice_PlayerDeath", "riii7" }, { "isOn", "" },
};
static int f_event_format(const char *name, char *format, size_t cap)
{
    size_t i;
    for (i = 0; i < sizeof k_formats / sizeof k_formats[0]; i++)
        if (!strcmp(k_formats[i].name, name)) { strcpy_s(format, cap, k_formats[i].format); return 1; }
    return 0;
}
static int f_event(void *ent, const char *name, const sh_script_earg *a, int n, sh_script_earg *ret, int post)
{
    char line[512];
    size_t off = (size_t)_snprintf_s(line, sizeof line, _TRUNCATE, "%s%s@%s(", post ? "post " : "", name,
                                     ent == &g_own ? "own" : "other");
    int i;
    for (i = 0; i < n && off < sizeof line; i++) {
        const char *sep = i ? "," : "";
        switch (a[i].type) {
        case 'i': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%d", sep, a[i].i); break;
        case 'f': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%g", sep, a[i].f); break;
        case 'v': case 'a':
            off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%c%g %g %g", sep, a[i].type, a[i].v[0], a[i].v[1], a[i].v[2]);
            break;
        case 'c':
            off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%sc%g %g %g %g", sep, a[i].v[0], a[i].v[1], a[i].v[2], a[i].w);
            break;
        case 's': case '1': case '2':
            off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%c'%s'", sep, a[i].type, a[i].s);
            break;
        case 'E': case 'r':
            off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s%c:%s", sep, a[i].type,
                               a[i].p == &g_own ? "own" : a[i].p == &g_other ? "other" : "none");
            break;
        case 'd': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%sd:%s", sep, a[i].p ? ((decl *)a[i].p)->name : "none"); break;
        case '4': off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s-", sep); break;
        default: off += _snprintf_s(line + off, sizeof line - off, _TRUNCATE, "%s?", sep); break;
        }
    }
    strcat_s(g_events, sizeof g_events, line);
    strcat_s(g_events, sizeof g_events, ");");
    if (ret) {
        memset(ret, 0, sizeof *ret);
        if (!strcmp(name, "getHealth")) { ret->type = 'f'; ret->f = 75.5f; }
        else if (!strcmp(name, "getName")) { ret->type = 's'; ret->s = "boss"; }
        else if (!strcmp(name, "getEnemy")) { ret->type = 'e'; ret->i = (int)ID_OTHER; }
        else if (!strcmp(name, "getDamageDecl")) { ret->type = 'd'; ret->p = &g_enc2; }
        else if (!strcmp(name, "getAngles")) { ret->type = 'a'; ret->v[0] = 1; ret->v[1] = 90; ret->v[2] = 0; }
        else if (!strcmp(name, "isOn")) { ret->type = 'i'; ret->i = 1; }
    }
    return 1;
}
static sh_script_engine g_engine_fake;

/* ---- reflection, for decls ---- */
static int o_write(void *c, void *addr, const void *in, size_t n) { (void)c; memcpy(addr, in, n); return 1; }
static const char *o_class(void *c, void *ent) { (void)c; (void)ent; return "idAI2"; }
static const char *o_res_name(void *c, const void *object) { (void)c; return ((const decl *)object)->name; }
static const sh_script_reflect_ops k_rops = { NULL, o_class, o_write, NULL, NULL, o_res_name, NULL, NULL, NULL, NULL };

/* ---- the game ---- */
static char g_signals[1024];
static struct { const char *name; char value[64]; int locked; } g_cvars_fake[] = {
    { "g_gravity", "1066", 0 }, { "ai_name", "doom", 0 }, { "g_scale", "0.5", 0 }, { "fs_basepath", "D:", 1 },
};
static const char *g_decl_name_of(void *d) { return ((decl *)d)->name; }
static int g_signal_number(const char *event)
{
    return !strcmp(event, "ai_killed") ? 2 : !strcmp(event, "trigger_touched") ? 53 : -1;
}
static int g_signal_fire(void *ent, int number, void *activator)
{
    char line[96];
    sprintf_s(line, sizeof line, "%s:%d:%s;", ent == &g_own ? "own" : "other", number,
              activator == &g_own ? "own" : activator == &g_other ? "other" : "none");
    strcat_s(g_signals, sizeof g_signals, line);
    return 1;
}
static int g_cvar_get(const char *name, char *out, size_t cap)
{
    size_t i;
    for (i = 0; i < sizeof g_cvars_fake / sizeof g_cvars_fake[0]; i++)
        if (!strcmp(g_cvars_fake[i].name, name)) { strcpy_s(out, cap, g_cvars_fake[i].value); return 1; }
    return 0;
}
static int g_cvar_set(const char *name, const char *value)
{
    size_t i;
    for (i = 0; i < sizeof g_cvars_fake / sizeof g_cvars_fake[0]; i++)
        if (!strcmp(g_cvars_fake[i].name, name)) {
            if (g_cvars_fake[i].locked) return -1;
            strcpy_s(g_cvars_fake[i].value, sizeof g_cvars_fake[i].value, value);
            return 1;
        }
    return 0;
}
static void *g_decl(const char *type, const char *name)
{
    if (strcmp(type, "snapAIEncounter") && strcmp(type, "weird")) return NULL;
    return !strcmp(name, g_enc.name) ? &g_enc : !strcmp(name, g_enc2.name) ? &g_enc2 : NULL;
}
static const char *g_decl_class(const char *type) { return !strcmp(type, "snapAIEncounter") ? "idDeclSnapAIEncounter" : NULL; }
static const sh_script_game_ops k_gops = { g_decl_name_of, g_signal_number, g_signal_fire, g_cvar_get, g_cvar_set, g_decl, g_decl_class };

static int t_ent(lua_State *L)
{
    uint32_t id = luaL_checkinteger(L, 1) ? ID_OTHER : ID_OWN;
    sh_script_handle h = sh_script_handles_find(id, NULL);
    if (!h) h = sh_script_handles_ref(id);
    sh_script_push_entity(L, h);
    return 1;
}

static char g_log[65536];
static void log_cat(void *ctx, const char *line)
{
    (void)ctx;
    if (strlen(g_log) + strlen(line) + 2 < sizeof g_log) { strcat_s(g_log, sizeof g_log, line); strcat_s(g_log, sizeof g_log, "\n"); }
}
static const char *log_now(void)
{
    g_log[0] = 0;
    sh_script_log_read(SALT, log_cat, NULL);
    return g_log;
}
static char g_stop[4096];
static const char *stop_now(void)
{
    g_stop[0] = 0;
    HOS(g_stop, sh_script_stop_line_dup(SALT));
    return g_stop;
}
static int has(const char *hay, const char *needle)
{
    int ok = strstr(hay, needle) != NULL;
    if (!ok) printf("  missing \"%s\" in:\n%s\n", needle, hay);
    return ok;
}

static void run(const char *body)
{
    sh_script_mod_desc d;
    const char *names[1];
    sh_script_map_end();
    memset(&g_own, 0, sizeof g_own);
    memset(&g_other, 0, sizeof g_other);
    g_events[0] = 0;
    g_signals[0] = 0;
    strcpy_s(g_text, sizeof g_text, body);
    d.salt = SALT;
    d.name = "Game Test";
    sh_script_map_start(&d, 1, &g_src, 3, 5.0);
    names[0] = "mods/" SALT "/ai/tyrant";
    sh_script_hooks_record(ID_OWN, names, 1, NULL);
    names[0] = "ai/demon/imp";
    sh_script_hooks_record(ID_OTHER, names, 1, NULL);
    sh_script_api_frame();
    sh_script_tick(5.0, 0.016);
}

static void refused(const char *body, const char *want)
{
    run(body);
    CHECK(sh_script_status(SALT) == SH_SCRIPT_STOPPED);
    CHECK(has(stop_now(), want));
}

static void test_events(void)
{
    run("api = 1\n"
        "local d, o = ent(0), ent(1)\n"
        "d:event('setOrigin', { x = 1, y = 2, z = 3 })\n"
        "o:event('activate', d)\n"
        "o:event('activate', nil)\n"
        "o:event('damage', decl('snapAIEncounter', game('enc/baron')), { pitch = 0, yaw = 90, roll = 0 }, 12.5, 2, d, true)\n"
        "d:event('ae_sound', decl('snapAIEncounter', 'enc/tyrant'))\n"
        "d:event('ae_sound', nil)\n"
        "d:event('setColor', { r = 1, g = 0.5, b = 0 })\n"
        "d:event('setColor', { 0, 0, 1, 0.25 })\n"
        "d:event('notice_hit', o, 7)\n"
        "d:event('ae_playAnim', 'combat/attack', 'go', 'hand_r')\n"
        "print('posted', d:post('activate', o))\n"
        "print('health', d:event('getHealth'))\n"
        "print('name', d:event('getName'))\n"
        "print('enemy', d:event('getEnemy') == o)\n"
        "print('decl', d:event('getDamageDecl'))\n"
        "local a = d:event('getAngles') print('angles', a.pitch, a.yaw, a.roll)\n"
        "print('on', d:event('isOn'))\n"
        "print('nothing', d:event('setOrigin', { 0, 0, 0 }))\n"
        "print('args', table.concat(event_args('damage'), ','), #event_args('getHealth'), table.concat(event_args('ae_sound'), ','), event_args('nope'))\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(g_events, "setOrigin@own(v1 2 3);"));
    CHECK(has(g_events, "activate@other(E:own);activate@other(E:none);"));
    CHECK(has(g_events, "damage@other(d:enc/baron,a0 90 0,12.5,2,E:own,1);"));
    CHECK(has(g_events, "ae_sound@own(-,d:mods/" SALT "/enc/tyrant);ae_sound@own(-,d:none);"));
    CHECK(has(g_events, "setColor@own(c1 0.5 0 1);setColor@own(c0 0 1 0.25);"));
    CHECK(has(g_events, "notice_hit@own(r:other,7);"));
    CHECK(has(g_events, "ae_playAnim@own(-,1'combat/attack',s'go',2'hand_r');"));
    CHECK(has(g_events, "post activate@own(E:other);"));
    CHECK(has(log_now(), "posted\ttrue"));
    CHECK(has(g_log, "health\t75.5"));
    CHECK(has(g_log, "name\tboss"));
    CHECK(has(g_log, "enemy\ttrue"));
    CHECK(has(g_log, "decl\tenc/baron"));
    CHECK(has(g_log, "angles\t1.0\t90.0\t0.0"));
    CHECK(has(g_log, "on\t1"));
    CHECK(has(g_log, "nothing\n"));
    CHECK(has(g_log, "args\tdecl,angles,number,number,entity,true or false\t0\tdecl\tnil"));

    refused("api = 1\nent(0):event('explodeNow')\n", "\"event\" found no event named explodeNow in the game");
    refused("api = 1\nent(0):event('setOrigin')\n", "\"event\" got 0 values for setOrigin, which takes 1 (position)");
    refused("api = 1\nent(0):event('getHealth', 1, 2)\n", "\"event\" got 2 values for getHealth, which takes 0 (nothing)");
    refused("api = 1\nent(0):event('setOrigin', 'here')\n", "\"event\" needs a position as value 1 of setOrigin, got text");
    refused("api = 1\nent(0):event('activate', 5)\n", "\"event\" needs an entity as value 1 of activate, got a number");
    refused("api = 1\nent(0):event('notice_hit', nil, 1)\n",
            "\"event\" needs an entity that is still in the map as value 1 of notice_hit");
    refused("api = 1\nent(0):event('notice_hit', ent(1), 1.5)\n",
            "\"event\" needs a whole number as value 2 of notice_hit, got a number");
    refused("api = 1\nent(0):event('ae_sound', 'snd')\n", "\"event\" needs a decl as value 1 of ae_sound, got text");
    refused("api = 1\nent(0):post('notice_PlayerDeath', ent(1), 1, 2, 3, 4)\n",
            "\"post\" cannot send notice_PlayerDeath: value 5 is of a kind only the game makes");
}

static void test_signals(void)
{
    run("api = 1\n"
        "local d, o = ent(0), ent(1)\n"
        "print(d:signal('ai_killed', o), o:signal('SNAP_EVENT_TRIGGER_TOUCHED'))\n"
        "on.signal('AI_Killed', function(e, by) print('killed', e == d, by == o) end)\n"
        "on.signal('snap_event_trigger_touched', function(e, by) print('touched', e == o, by) end)\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "true\ttrue"));
    CHECK(has(g_signals, "own:2:other;other:53:none;"));
    sh_script_game_signal_seen(&g_own, "ai_killed", &g_other);
    sh_script_game_signal_seen(&g_other, "trigger_touched", NULL);
    sh_script_game_signal_seen(&g_other, "map_start", NULL);        /* nobody handles it */
    sh_script_api_frame();
    sh_script_tick(5.016, 0.016);
    CHECK(has(log_now(), "killed\ttrue\ttrue"));
    CHECK(has(g_log, "touched\ttrue\tnil"));

    refused("api = 1\nent(0):signal('door_opened')\n", "\"signal\" found no SnapMap event named door_opened in the game");
    refused("api = 1\non.signal('door_opened', function() end)\n",
            "\"on.signal\" found no SnapMap event named door_opened in the game");
    refused("api = 1\non.signal('ai_killed')\n", "\"on.signal\" needs a function as its second value, got nothing");
    refused("api = 1\nent(0):signal('ai_killed', 'me')\n", "\"signal\" needs the activator as an entity, got text");
}

static void test_cvars(void)
{
    run("api = 1\n"
        "print('read', cvar('g_gravity'), cvar('ai_name'), cvar('g_scale'), math.type(cvar('g_gravity')))\n"
        "print('set', cvar('g_gravity', 400), cvar('ai_name', 'tyrant'), cvar('g_scale', true))\n"
        "cvar('g_gravity', 300)\n"
        "print('now', cvar('g_gravity'), cvar('ai_name'), cvar('g_scale'))\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "read\t1066\tdoom\t0.5\tinteger"));
    CHECK(has(g_log, "set\ttrue\ttrue\ttrue"));
    CHECK(has(g_log, "now\t300\ttyrant\t1"));
    CHECK(!strcmp(g_cvars_fake[0].value, "300"));
    sh_script_map_end();                                 /* put back as they were before the map */
    CHECK(!strcmp(g_cvars_fake[0].value, "1066") && !strcmp(g_cvars_fake[1].value, "doom") && !strcmp(g_cvars_fake[2].value, "0.5"));

    refused("api = 1\ncvar('g_nope')\n", "\"cvar\" found no cvar named g_nope in the game");
    refused("api = 1\ncvar('fs_basepath', 'C:')\n", "\"cvar\" cannot set fs_basepath: the game only takes it when it starts");
    refused("api = 1\ncvar('g_gravity', {})\n", "\"cvar\" needs the value as a number, true or false, or text, got a table");
}

static void test_decls(void)
{
    run("api = 1\n"
        "local mine, stock = decl('snapAIEncounter', 'enc/tyrant'), decl('snapAIEncounter', game('enc/baron'))\n"
        "print('names', mine:name(), stock:name(), mine:type())\n"
        "print('values', mine:get('value'), stock:get('value'), mine:get().value, #mine:fields())\n"
        "print('same', mine == decl('snapAIEncounter', 'enc/tyrant'), mine == stock)\n"
        "mine:set('value', 20) stock:set('value', 30) stock:set('value', 31)\n"
        "print('after', mine:get('value'), stock:get('value'))\n");
    CHECK(sh_script_status(SALT) == SH_SCRIPT_RUNNING);
    CHECK(has(log_now(), "names\tmods/" SALT "/enc/tyrant\tenc/baron\tsnapAIEncounter"));
    CHECK(has(g_log, "values\t9\t4\t9\t1"));
    CHECK(has(g_log, "same\ttrue\tfalse"));
    CHECK(has(g_log, "after\t20\t31"));
    CHECK(g_enc.value == 20 && g_enc2.value == 31);
    sh_script_map_end();                                 /* a decl outlives the map: put back */
    CHECK(g_enc.value == 9 && g_enc2.value == 4);

    refused("api = 1\ndecl('snapAIEncounter', 'enc/none')\n",
            "\"decl\" found no snapAIEncounter decl \"enc/none\" in this mod; a decl of the game is decl(type, game(name))");
    refused("api = 1\ndecl('snapAIEncounter', game('enc/none'))\n", "\"decl\" found no snapAIEncounter decl named enc/none in the game");
    refused("api = 1\ndecl('weird', game('enc/baron')):get('value')\n",
            "\"get\" cannot look inside a weird decl: the game does not describe that type");
    refused("api = 1\ndecl('snapAIEncounter', 'enc/tyrant'):set('valu', 1)\n",
            "\"set\" could not write \"valu\": idDeclSnapAIEncounter has no field named valu");
    CHECK(g_enc.value == 9);
}

static void *h_resolve(uint32_t id) { return f_resolve(id); }

int main(void)
{
    g_engine_fake.resolve = f_resolve;
    g_engine_fake.spawn_id = f_spawn_id;
    g_engine_fake.slots = f_slots;
    g_engine_fake.slot_spawn_id = f_slot_spawn_id;
    g_engine_fake.kind = f_kind;
    g_engine_fake.event = f_event;
    g_engine_fake.event_format = f_event_format;
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "ent", t_ent);
    sh_script_api_set_engine(&g_engine_fake);
    sh_script_api_install(NULL, 0, NULL);
    sh_script_api_reflect_register();
    sh_script_api_reflect_set(&H, &k_rops);
    sh_script_api_game_register();
    sh_script_api_game_set(&k_gops);
    sh_script_handles_set_resolver(h_resolve);

    test_events();
    test_signals();
    test_cvars();
    test_decls();
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);
    printf(g_fail ? "script_api_game_test: FAILED\n" : "script_api_game_test: ok\n");
    return g_fail;
}
