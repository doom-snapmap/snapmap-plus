/* script_sandbox_test.c -- the mod script runtime (src/backend/script), with
 * no engine: an in-memory file source stands in for the serving layer and a
 * handle set for the engine's entity table.
 *
 * The engine hooks are stood in for as they behave: a spawn is recorded in
 * the handle table under its mod with its entityDef chain
 * (sh_script_hooks_record), a removal is the resolver no longer finding the
 * spawn id, and damage, death, animation and input records are posted to
 * the mod's queue (sh_script_queue_post). Test ids are spawn ids.
 *
 * Covers: each forbidden facility absent; bytecode refused (an entry file
 * and a required one, real lua_dump output); a runaway loop, also inside
 * pcall and inside run(), stopped by the step budget; an allocation bomb
 * stopped by the memory cap, also when pcall catches it; a backtracking
 * pattern stopped by the pattern-step bound and a long subject refused; work
 * the count hook cannot see (sort, long comparisons) bounded; the api
 * declaration; the plain stop line with file and line; two mods isolated;
 * registration, the bound-name and owner queries, and dispatch of every
 * event kind; the re-entrancy queue and its passes; queue overflow; timers
 * and coroutines; deterministic random; teardown with nothing left; and
 * the loader fed malformed text. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script/script_runtime.h"
#include "script/script_reason.h"
#include "script/script_hooks.h"
#include "../src/third_party/lua/lauxlib.h"
#include "../src/third_party/lua/lualib.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* ------------------------------------------------------------ the source */

typedef struct { char salt[16]; char path[64]; char *text; size_t len; } tfile;
static tfile g_files[256];
static int g_nfiles;
static int g_list_fail;

static void files_clear(void)
{
    int i;
    for (i = 0; i < g_nfiles; i++) free(g_files[i].text);
    g_nfiles = 0;
    g_list_fail = 0;
}

static void file_add_bytes(const char *salt, const char *path, const char *text, size_t len)
{
    tfile *f = &g_files[g_nfiles++];
    strcpy(f->salt, salt);
    strcpy(f->path, path);
    f->text = (char *)malloc(len + 1);
    memcpy(f->text, text, len);
    f->text[len] = 0;
    f->len = len;
}

static void file_add(const char *salt, const char *path, const char *text)
{
    file_add_bytes(salt, path, text, strlen(text));
}

static int src_list(void *ctx, const char *salt, sh_script_file_fn fn, void *fnctx)
{
    int i, n = 0;
    (void)ctx;
    if (g_list_fail) return -1;
    for (i = 0; i < g_nfiles; i++) if (!strcmp(g_files[i].salt, salt)) { fn(fnctx, g_files[i].path); n++; }
    return n;
}

static int src_read(void *ctx, const char *salt, const char *rel, char **text, size_t *len)
{
    int i;
    (void)ctx;
    for (i = 0; i < g_nfiles; i++)
        if (!strcmp(g_files[i].salt, salt) && !_stricmp(g_files[i].path, rel)) {
            *text = (char *)malloc(g_files[i].len + 1);
            memcpy(*text, g_files[i].text, g_files[i].len);
            *len = g_files[i].len;
            return 1;
        }
    return 0;
}

static void src_release(void *ctx, char *text) { (void)ctx; free(text); }

static const sh_script_source g_src = { NULL, src_list, src_read, src_release };

/* ------------------------------------------------------------ entities */

static uint64_t g_dead[64];
static int g_ndead;
static int g_live_token;

/* The engine's lookup: every spawn id resolves until the test marks it dead. */
static void *fake_resolve(uint32_t spawn_id)
{
    int i;
    for (i = 0; i < g_ndead; i++) if (g_dead[i] == spawn_id) return NULL;
    return &g_live_token;
}

#define TEV_SPAWN  1
#define TEV_DAMAGE 2
#define TEV_DEATH  3
#define TEV_ANIM   4
#define TEV_INPUT  5

/* What a hook knows about an event, before it becomes a record. */
typedef struct {
    int      kind;
    uint64_t entity, other, inflictor;
    double   amount;
    char     names[SH_SCRIPT_NAMES_CAP];
    char     text[SH_SCRIPT_EVENT_TEXT];
    char     location[SH_SCRIPT_EVENT_TEXT];
} tev;

/* The handle a hook hands over for a spawn id: the mod's record, else a
 * reference no mod owns. */
static sh_script_handle hid(uint64_t id)
{
    sh_script_handle h;
    if (!id) return 0;
    h = sh_script_handles_find((uint32_t)id, NULL);
    return h ? h : sh_script_handles_ref((uint32_t)id);
}

/* Do what the hooks do with the event; 1 when a running mod's queue or
 * handle table took it. */
static int t_enqueue(const char *salt, const tev *ev)
{
    static const int kinds[] = { 0, 0, SH_SCRIPT_EV_DAMAGE, SH_SCRIPT_EV_DEATH, SH_SCRIPT_EV_ANIM, SH_SCRIPT_EV_INPUT };
    int slot = sh_script_hooks_mod_find(salt);
    if (slot < 0 || sh_script_status(salt) != SH_SCRIPT_RUNNING || ev->kind < TEV_SPAWN || ev->kind > TEV_INPUT) return 0;
    if (ev->kind == TEV_SPAWN) {
        const char *names[16];
        char buf[SH_SCRIPT_NAMES_CAP], *p, *next = NULL;
        int n = 0;
        strcpy_s(buf, sizeof buf, ev->names);
        for (p = strtok_s(buf, "\n", &next); p && n < 16; p = strtok_s(NULL, "\n", &next)) names[n++] = p;
        return sh_script_hooks_record((uint32_t)ev->entity, names, n, NULL) != 0;
    }
    return sh_script_queue_post(slot, (sh_script_event_kind)kinds[ev->kind], hid(ev->entity), hid(ev->other),
                                hid(ev->inflictor), (float)ev->amount,
                                ev->kind == TEV_INPUT ? ev->names : ev->text, ev->location) == 1;
}

static int t_owner(uint64_t id, char *salt, size_t cap)
{
    sh_script_handle h = id ? sh_script_handles_find((uint32_t)id, NULL) : 0;
    return sh_script_entity_owner(h, salt, cap);
}

/* ---------------------------------------------------------------- logs */

static char g_log[65536];
static void log_cat(void *ctx, const char *line)
{
    (void)ctx;
    if (strlen(g_log) + strlen(line) + 2 < sizeof g_log) { strcat(g_log, line); strcat(g_log, "\n"); }
}
static const char *log_of(const char *salt)
{
    g_log[0] = 0;
    sh_script_log_read(salt, log_cat, NULL);
    return g_log;
}
static char g_stop[SH_SCRIPT_LINE_CAP];
static const char *stop_of(const char *salt)
{
    sh_script_stop_line(salt, g_stop, sizeof g_stop);
    return g_stop;
}

static int start1(const char *salt, const char *name, uint64_t seed)
{
    sh_script_mod_desc d;
    d.salt = salt;
    d.name = name;
    return sh_script_map_start(&d, 1, &g_src, seed, 0.0);
}

/* One mod with one entry file holding `body`. Returns its status after map start. */
static int run_one(const char *body)
{
    files_clear();
    file_add("aaaaaaaa", "main.lua", body);
    start1("aaaaaaaa", "Test Mod", 7);
    return sh_script_status("aaaaaaaa");
}

/* ------------------------------- test API (as another module would add it) */

static int t_resolve(lua_State *L)
{
    char out[SH_SCRIPT_DEFNAME_CAP];
    int game = 0;
    sh_script_check_name(L, 1, "resolve", lua_toboolean(L, 2), out, sizeof out, &game);
    lua_pushstring(L, out);
    lua_pushboolean(L, game);
    return 2;
}

/* poke(salt, kind, handle, names): a hook firing while Lua runs */
static int t_poke(lua_State *L)
{
    tev ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = (int)luaL_checkinteger(L, 2);
    ev.entity = (uint64_t)luaL_checkinteger(L, 3);
    strncpy(ev.names, luaL_optstring(L, 4, ""), sizeof ev.names - 1);
    strncpy(ev.text, luaL_optstring(L, 5, ""), sizeof ev.text - 1);
    lua_pushboolean(L, t_enqueue(luaL_checkstring(L, 1), &ev));
    return 1;
}

static int t_end_map(lua_State *L) { (void)L; sh_script_map_end(); return 0; }

/* handle(e): the entity's spawn id, 0 once it is removed */
static int t_handle(lua_State *L)
{
    uint64_t h = sh_script_check_entity(L, 1, "handle");
    lua_pushinteger(L, h ? (lua_Integer)sh_script_handle_spawn_id((sh_script_handle)h) : 0);
    return 1;
}

static int t_owns(lua_State *L)
{
    lua_pushboolean(L, sh_script_owns_entity(L, sh_script_check_entity(L, 1, "owns")));
    return 1;
}

/* ---------------------------------------------------------------- tests */

static void test_absent(void)
{
    int st = run_one(
        "api = 1\n"
        "assert(io == nil and os == nil and debug == nil and package == nil)\n"
        "assert(load == nil and loadfile == nil and dofile == nil and loadstring == nil)\n"
        "assert(string.dump == nil and math.randomseed == nil)\n"
        "assert(getmetatable('') == false)\n"
        "assert(type(collectgarbage('count')) == 'number')\n"
        "assert(type(require) == 'function' and type(print) == 'function')\n"
        "print('absent ok')\n");
    CHECK(st == SH_SCRIPT_RUNNING);
    CHECK(strstr(log_of("aaaaaaaa"), "absent ok") != NULL);
    sh_script_map_end();

    st = run_one("api = 1\ncollectgarbage('collect')\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "\"collectgarbage\" only answers \"count\" here") != NULL);
    sh_script_map_end();

    st = run_one("api = 1\nsetmetatable({}, {__gc = function() while true do end end})\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "\"__gc\"") != NULL);
    CHECK(strstr(stop_of("aaaaaaaa"), "scripts\\main.lua line 2") != NULL);
    sh_script_map_end();

    /* the "on" table cannot be replaced or edited */
    st = run_one("api = 1\non.spawn = nil\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    sh_script_map_end();
}

static size_t g_dump_len;
static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    (void)L;
    memcpy((char *)ud + g_dump_len, p, sz);
    g_dump_len += sz;
    return 0;
}

static void test_bytecode(void)
{
    static char code[65536];
    lua_State *L = luaL_newstate();
    g_dump_len = 0;
    CHECK(luaL_loadstring(L, "api = 1 print('compiled ran')") == LUA_OK);
    CHECK(lua_dump(L, dump_writer, code, 0) == 0);
    lua_close(L);
    CHECK(g_dump_len > 4 && code[0] == 0x1b);

    files_clear();
    file_add_bytes("aaaaaaaa", "main.lua", code, g_dump_len);
    start1("aaaaaaaa", "Test Mod", 1);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "the file is compiled Lua, not script text") != NULL);
    CHECK(strstr(log_of("aaaaaaaa"), "compiled ran") == NULL);
    sh_script_map_end();

    files_clear();
    file_add("aaaaaaaa", "main.lua", "api = 1\nrequire('bin')\n");
    file_add_bytes("aaaaaaaa", "lib/bin.lua", code, g_dump_len);
    start1("aaaaaaaa", "Test Mod", 1);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "compiled Lua, not script text") != NULL);
    CHECK(strstr(log_of("aaaaaaaa"), "compiled ran") == NULL);
    sh_script_map_end();
}

static void test_budget(void)
{
    DWORD t0 = GetTickCount();
    int st = run_one("api = 1\non.map_start(function()\n  while true do end\nend)\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(!strcmp(stop_of("aaaaaaaa"),
        "Script stopped: Test Mod, scripts\\main.lua line 3: took too long (more than 2 million steps in one call), "
        "so this mod's scripts stopped for this map. Look for a loop that does not end."));
    CHECK(GetTickCount() - t0 < 3000);
    sh_script_map_end();

    /* pcall and xpcall cannot catch it */
    st = run_one("api = 1\nwhile true do pcall(function() while true do end end) end\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "took too long") != NULL);
    sh_script_map_end();
    st = run_one("api = 1\nwhile true do xpcall(function() while true do end end, function(e) while true do end end) end\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "took too long") != NULL);
    sh_script_map_end();

    /* nor a coroutine, nor coroutine.resume */
    st = run_one("api = 1\nrun(function() while true do end end)\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    sh_script_map_end();
    st = run_one("api = 1\nlocal co = coroutine.create(function() while true do end end)\n"
                 "while true do coroutine.resume(co) end\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "took too long") != NULL);
    sh_script_map_end();

    /* a loop that waits each frame is fine; one that never yields after a wait is not */
    st = run_one("api = 1\nrun(function() local n = 0 while true do n = n + 1 wait() end end)\n");
    CHECK(st == SH_SCRIPT_RUNNING);
    sh_script_tick(0.1, 0.1);
    sh_script_tick(0.2, 0.1);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    sh_script_map_end();

    /* sorting is charged: one sort of 200,000 already crosses the call budget */
    st = run_one("api = 1\nlocal t = {} for i = 1, 200000 do t[i] = -i end\ntable.sort(t)\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "line 3: took too long") != NULL);
    sh_script_map_end();

    /* the per-frame budget across calls */
    files_clear();
    file_add("aaaaaaaa", "main.lua",
             "api = 1\non.think('ai/x', function(self) local n = 0 for i = 1, 600000 do n = n + i end end)\n");
    start1("aaaaaaaa", "Test Mod", 1);
    {
        tev ev;
        int i;
        memset(&ev, 0, sizeof ev);
        ev.kind = TEV_SPAWN;
        strcpy(ev.names, "mods/aaaaaaaa/ai/x");
        for (i = 1; i <= 8; i++) { ev.entity = (uint64_t)i; CHECK(t_enqueue("aaaaaaaa", &ev)); }
    }
    sh_script_tick(0.016, 0.016);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "steps in one frame") != NULL);
    sh_script_map_end();

    /* wall time: comparing long texts is one instruction each */
    {
        sh_script_limits lim;
        lim.mem_cap = SH_SCRIPT_MEM_CAP;
        lim.call_steps = SH_SCRIPT_CALL_STEPS;
        lim.tick_steps = SH_SCRIPT_TICK_STEPS;
        lim.pattern_steps = SH_SCRIPT_PATTERN_STEPS;
        lim.wall_ms = 100;
        sh_script_limits_set(&lim);
        t0 = GetTickCount();
        st = run_one("api = 1\nlocal a = string.rep('x', 2000000) .. 'a'\nlocal b = string.rep('x', 2000000) .. 'b'\n"
                     "while true do local same = (a == b) end\n");
        CHECK(st == SH_SCRIPT_STOPPED);
        CHECK(strstr(stop_of("aaaaaaaa"), "more than 100 ms in one call") != NULL);
        CHECK(GetTickCount() - t0 < 5000);
        sh_script_limits_reset();
        sh_script_map_end();
    }
}

static void test_memory(void)
{
    DWORD t0 = GetTickCount();
    int st = run_one("api = 1\nlocal t = {}\nfor i = 1, 1e9 do t[i] = string.rep('x', 1000) .. i end\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "used more than 32 MB of memory, so this mod's scripts stopped for this map. "
                                      "Look for a table or text that keeps growing.") != NULL);
    CHECK(strstr(stop_of("aaaaaaaa"), "scripts\\main.lua line 3") != NULL);
    CHECK(GetTickCount() - t0 < 5000);
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);

    /* caught by pcall: still the memory stop */
    st = run_one("api = 1\nwhile true do pcall(function() local t = {} for i = 1, 1e9 do t[i] = {i} end end) end\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "more than 32 MB") != NULL);
    sh_script_map_end();

    /* one huge request */
    st = run_one("api = 1\nlocal s = string.rep('x', 100 * 1024 * 1024)\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "more than 32 MB") != NULL);
    sh_script_map_end();

    /* doubling a text: memory or steps, never a hang */
    st = run_one("api = 1\nlocal s = 'x'\nwhile true do s = s .. s end\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);
}

static void test_pattern(void)
{
    DWORD t0 = GetTickCount();
    int st = run_one("api = 1\nlocal s = string.rep('a', 4000)\n"
                     "local r = string.find(s, string.rep('a*', 12) .. 'b')\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "a text pattern took too long to match (more than 1 million steps)") != NULL);
    CHECK(strstr(stop_of("aaaaaaaa"), "scripts\\main.lua line 3") != NULL);
    CHECK(GetTickCount() - t0 < 3000);
    sh_script_map_end();

    /* caught by pcall in a loop: still stopped, quickly */
    t0 = GetTickCount();
    st = run_one("api = 1\nlocal s = string.rep('a', 4000)\n"
                 "while true do pcall(string.match, s, string.rep('a*', 12) .. 'b') end\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(GetTickCount() - t0 < 3000);
    sh_script_map_end();

    /* method form and gsub, gmatch */
    st = run_one("api = 1\nlocal s = string.rep('a', 4000)\nfor w in s:gmatch(string.rep('a*', 10) .. 'b') do end\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "pattern took too long") != NULL);
    sh_script_map_end();

    st = run_one("api = 1\nlocal s = string.rep('a', 5000)\nlocal n = s:find('b')\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"),
                 "scripts\\main.lua line 3: \"string.find\" can only search text of up to 4096 bytes, and this text has 5000") != NULL);
    sh_script_map_end();

    /* ordinary patterns work */
    st = run_one("api = 1\nlocal k, v = ('speed = 42'):match('^(%w+)%s*=%s*(%d+)$')\n"
                 "assert(k == 'speed' and v == '42')\n"
                 "local n = select(2, string.gsub(string.rep('ab ', 1000), '%a+', ''))\nassert(n == 1000)\nprint('patterns ok')\n");
    CHECK(st == SH_SCRIPT_RUNNING);
    CHECK(strstr(log_of("aaaaaaaa"), "patterns ok") != NULL);
    sh_script_map_end();
}

static void test_api_declaration(void)
{
    int st = run_one("print('no api')\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(!strcmp(stop_of("aaaaaaaa"), "Script stopped: Test Mod, scripts\\main.lua line 1: the file does not start "
                                       "with \"api = 1\", so it did not run. Add \"api = 1\" as its first line."));
    CHECK(strstr(log_of("aaaaaaaa"), "no api") == NULL);
    sh_script_map_end();

    st = run_one("-- a door\n--[[ long\ncomment ]]\n\napi = 2\nprint('v2')\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(!strcmp(stop_of("aaaaaaaa"), "Script stopped: Test Mod, scripts\\main.lua line 5: the file asks for scripting "
                                       "version 2, but this Snapmap+ has version 1, so it did not run. Update Snapmap+ to run it."));
    sh_script_map_end();

    st = run_one("\xEF\xBB\xBF-- header\napi = 1 -- ok\nprint('v1')\n");
    CHECK(st == SH_SCRIPT_RUNNING);
    CHECK(strstr(log_of("aaaaaaaa"), "v1") != NULL);
    sh_script_map_end();

    st = run_one("api = 1.5\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    sh_script_map_end();
    st = run_one("apix = 1\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    sh_script_map_end();

    /* lib files need no declaration; entry files run in byte-wise name order */
    files_clear();
    file_add("aaaaaaaa", "b.lua", "api = 1\nprint('b ' .. require('util').x)\n");
    file_add("aaaaaaaa", "Z.lua", "api = 1\nprint('Z')\n");
    file_add("aaaaaaaa", "a.lua", "api = 1\nprint('a')\non.map_start(function() print('started') end)\n");
    file_add("aaaaaaaa", "lib/util.lua", "print('util loaded')\nreturn { x = 5 }\n");
    start1("aaaaaaaa", "Test Mod", 1);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    CHECK(!strcmp(log_of("aaaaaaaa"), "Z\na\nutil loaded\nb 5\nstarted\n"));
    sh_script_map_end();

    files_clear();
    file_add("aaaaaaaa", "main.lua", "api = 1\nrequire('missing')\n");
    start1("aaaaaaaa", "Test Mod", 1);
    CHECK(!strcmp(stop_of("aaaaaaaa"), "Script stopped: Test Mod, scripts\\main.lua line 2: \"require\" found no "
                                       "scripts\\lib\\missing.lua in this mod."));
    sh_script_map_end();

    files_clear();
    file_add("aaaaaaaa", "main.lua", "api = 1\nrequire('a')\n");
    file_add("aaaaaaaa", "lib/a.lua", "require('b')\n");
    file_add("aaaaaaaa", "lib/b.lua", "require('a')\n");
    start1("aaaaaaaa", "Test Mod", 1);
    CHECK(strstr(stop_of("aaaaaaaa"), "scripts\\lib\\b.lua line 1: \"a\" was required while it was still loading") != NULL);
    sh_script_map_end();
}

static void test_error_lines(void)
{
    struct { const char *body, *line; } cases[] = {
        { "api = 1\nlocal t\nprint(t.target)\n",
          "Script stopped: Test Mod, scripts\\main.lua line 3: \"t\" is nothing here. Check that it is set before it is used." },
        { "api = 1\nlocal self = {}\nprint(self.target.x)\n",
          "Script stopped: Test Mod, scripts\\main.lua line 3: \"target\" is nothing here. Check that it is set before it is used." },
        { "api = 1\nmove_to(1)\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: \"move_to\" is not a function here (it is nothing). Check the spelling of the name." },
        { "api = 1\nlocal x = nil + 1\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: a value used here is nothing. Check that it is set before it is used." },
        { "api = 1\nlocal a\nlocal s = 'x' .. a\n",
          "Script stopped: Test Mod, scripts\\main.lua line 3: \"a\" is nothing here, so it cannot be joined to text. Check that it is set before it is used." },
        { "api = 1\nlocal function f()\n  if true then\n",
          "Script stopped: Test Mod, scripts\\main.lua line 4: \"end\" expected to close \"if\" on line 3. Correct it, then press Refresh." },
        { "api = 1\nlocal x = = 2\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: something unexpected near \"=\". Correct it, then press Refresh." },
        { "api = 1\nlocal s = 'open\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: text in quotes is not closed near \"'open\". Correct it, then press Refresh." },
        { "api = 1\nerror('the door has no switch')\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: the door has no switch." },
        { "api = 1\nerror({})\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: the script raised an error whose value is not text. Give error(...) a text that says what went wrong." },
        { "api = 1\nlocal function f() local a, b, c, d, e, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u return f() + 1 end\nf()\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: a function called itself too many times. Look for a function that never stops calling itself." },
        { "api = 1\nlocal x = math.floor('a')\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: \"floor\" needs a number as its first value, got text. Check the value you pass." },
        { "api = 1\nfor _, v in ipairs(nil) do end\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: a value used here as a table is nothing. Check that it is set before it is used." },
        { "api = 1\nlocal x = 1 // 0\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: a whole number was divided by zero. Check the divisor before dividing." },
        { "api = 1\nlocal t = {}\nt[nil] = 1\n",
          "Script stopped: Test Mod, scripts\\main.lua line 3: a table was given nothing as a key. Check the key before using it." },
        { "api = 1\nwait(1)\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: \"wait\" can only be used inside run(function() ... end)." },
        { "api = 1\nlocal x = 1 < {}\n",
          "Script stopped: Test Mod, scripts\\main.lua line 2: a number was compared with a table using < or >. Check both values before comparing them." },
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        run_one(cases[i].body);
        if (strcmp(stop_of("aaaaaaaa"), cases[i].line)) {
            printf("FAIL error line %u:\n  got  %s\n  want %s\n", (unsigned)i, g_stop, cases[i].line);
            g_fail = 1;
        }
        CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_STOPPED);
        sh_script_map_end();
    }
}

static int g_input_handled;

static void test_dispatch(void)
{
    tev ev;
    char salt[16];
    unsigned kinds = 0;
    files_clear();
    file_add("aaaaaaaa", "main.lua",
        "api = 1\n"
        "on.spawn('ai/base', function(self) self.state.hp = 10 print('spawn base') end)\n"
        "on.spawn('AI/Child', function(self) assert(self:owns()) print('spawn child ' .. handle(self)) end)\n"
        "on.think('ai/child', function(self, dt) self.state.hp = self.state.hp + 1 print('think ' .. self.state.hp .. ' ' .. dt) end)\n"
        "on.damage('ai/base', function(self, hit) print('damage ' .. hit.amount .. ' ' .. hit.damage .. ' ' .. hit.location .. ' ' .. handle(hit.attacker) .. ' ' .. tostring(hit.inflictor)) end)\n"
        "on.death('mods/aaaaaaaa/ai/child', function(self, hit) print('death ' .. tostring(hit.attacker)) end)\n"
        "on.anim('ai/child', 'Throw', function(self) print('anim throw') end)\n"
        "on.remove('ai/child', function(self) print('removed ' .. handle(self)) end)\n"
        "on.input('Count_Switch', function(node, activator) print('input ' .. handle(node) .. ' ' .. handle(activator)) end)\n"
        "on.input('door_opened', function(node) end)\n"
        "assert(resolve('ai/x') == 'mods/aaaaaaaa/ai/x')\n"
        "assert(resolve('mods/aaaaaaaa/ai/x') == 'mods/aaaaaaaa/ai/x')\n"
        "assert(select(2, resolve(game('fx/impacts/rocket'), true)) == true)\n");
    file_add("bbbbbbbb", "main.lua", "api = 1\non.think('ai/child', function(self) print('b think') end)\n");
    {
        sh_script_mod_desc d[2] = { { "aaaaaaaa", "Mod A" }, { "bbbbbbbb", "Mod B" } };
        CHECK(sh_script_map_start(d, 2, &g_src, 3, 0.0) == 2);
    }
    CHECK(sh_script_binds("mods/aaaaaaaa/ai/child", salt, sizeof salt, &kinds) == 1);
    CHECK(!strcmp(salt, "aaaaaaaa"));
    CHECK(kinds == (SH_SCRIPT_BIND_SPAWN | SH_SCRIPT_BIND_THINK | SH_SCRIPT_BIND_DEATH | SH_SCRIPT_BIND_ANIM | SH_SCRIPT_BIND_REMOVE));
    CHECK(sh_script_binds("MODS/AAAAAAAA/AI/BASE", salt, sizeof salt, &kinds) == 1 && kinds == (SH_SCRIPT_BIND_SPAWN | SH_SCRIPT_BIND_DAMAGE));
    CHECK(sh_script_binds("mods/bbbbbbbb/ai/child", salt, sizeof salt, &kinds) == 1 && !strcmp(salt, "bbbbbbbb"));
    CHECK(sh_script_binds("mods/aaaaaaaa/ai/other", salt, sizeof salt, &kinds) == 0);

    memset(&ev, 0, sizeof ev);
    ev.kind = TEV_SPAWN;
    ev.entity = 77;
    strcpy(ev.names, "mods/aaaaaaaa/ai/child\nmods/aaaaaaaa/ai/base\nai/stock");
    CHECK(t_enqueue("aaaaaaaa", &ev) == 1);
    CHECK(t_owner(77, salt, sizeof salt) == 1 && !strcmp(salt, "aaaaaaaa"));
    CHECK(t_owner(78, salt, sizeof salt) == 0);
    CHECK(t_enqueue("cccccccc", &ev) == 0);

    sh_script_tick(1.0, 0.5);
    CHECK(strstr(log_of("aaaaaaaa"), "spawn child 77\nspawn base\nthink 11 0.5\n") != NULL);

    memset(&ev, 0, sizeof ev);
    ev.kind = TEV_DAMAGE;
    ev.entity = 77;
    ev.other = 5;
    ev.amount = 12.5;
    strcpy(ev.text, "damage/rocket");
    strcpy(ev.location, "head");
    CHECK(t_enqueue("aaaaaaaa", &ev));
    ev.kind = TEV_ANIM;
    strcpy(ev.text, "THROW");
    CHECK(t_enqueue("aaaaaaaa", &ev));
    ev.kind = TEV_ANIM;
    strcpy(ev.text, "other");
    CHECK(t_enqueue("aaaaaaaa", &ev));
    ev.kind = TEV_DEATH;
    ev.other = 0;
    CHECK(t_enqueue("aaaaaaaa", &ev));
    ev.kind = TEV_INPUT;
    ev.entity = 900;
    ev.other = 5;
    strcpy(ev.names, "count_switch");
    CHECK(t_enqueue("aaaaaaaa", &ev));
    strcpy(ev.names, "nobody");
    CHECK(t_enqueue("aaaaaaaa", &ev));
    CHECK(t_enqueue("aaaaaaaa", &ev));
    ev.kind = TEV_DAMAGE;
    ev.entity = 12345;                       /* not a bound entity: nothing runs */
    CHECK(t_enqueue("aaaaaaaa", &ev));
    sh_script_tick(2.0, 0.25);
    log_of("aaaaaaaa");
    CHECK(strstr(g_log, "damage 12.5 damage/rocket head 5 nil\nanim throw\ndeath nil\ninput 900 5\n") != NULL);
    CHECK(strstr(g_log, "Script node \"nobody\" (Mod A): no script handles it. The script handles: ") != NULL);
    CHECK(strstr(g_log, "count_switch") != NULL && strstr(g_log, "door_opened") != NULL);
    {
        const char *first = strstr(g_log, "Script node \"nobody\"");
        CHECK(first && strstr(first + 1, "Script node \"nobody\"") == NULL);   /* logged once */
    }
    CHECK(strstr(g_log, "think 12 0.25") != NULL);
    CHECK(strstr(log_of("bbbbbbbb"), "b think") == NULL);     /* mod B never saw entity 77 */

    /* removal: once, then the entity is gone from every query */
    g_dead[g_ndead++] = 77;
    sh_script_tick(3.0, 0.25);
    log_of("aaaaaaaa");
    CHECK(strstr(g_log, "removed 0\n") != NULL);              /* a removed entity answers nothing */
    sh_script_tick(4.0, 0.25);
    log_of("aaaaaaaa");
    CHECK(strstr(strstr(g_log, "removed 0\n") + 1, "removed") == NULL);
    CHECK(strstr(g_log, "think 13") == NULL);
    CHECK(t_owner(77, salt, sizeof salt) == 0);
    g_ndead = 0;
    sh_script_map_end();
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_ENDED);
    CHECK(sh_script_binds("mods/aaaaaaaa/ai/child", salt, sizeof salt, &kinds) == 0);

    /* binding another mod's content or the game's is refused */
    run_one("api = 1\non.spawn('mods/bbbbbbbb/ai/x', function() end)\n");
    CHECK(!strcmp(stop_of("aaaaaaaa"), "Script stopped: Test Mod, scripts\\main.lua line 2: \"on.spawn\" can only use "
                                       "this mod's own content; \"mods/bbbbbbbb/ai/x\" belongs to another mod."));
    sh_script_map_end();
    run_one("api = 1\non.think(game('ai/imp'), function() end)\n");
    CHECK(strstr(stop_of("aaaaaaaa"), "\"on.think\" can only use this mod's own content, not the game's \"ai/imp\"") != NULL);
    sh_script_map_end();
    run_one("api = 1\non.spawn('ai/x', 5)\n");
    CHECK(strstr(stop_of("aaaaaaaa"), "\"on.spawn\" needs a function as its second value, got number") != NULL);
    sh_script_map_end();
    run_one("api = 1\non.spawn('../x', function() end)\n");
    CHECK(strstr(stop_of("aaaaaaaa"), "which is not a usable name") != NULL);
    sh_script_map_end();
    (void)g_input_handled;
}

static void test_isolation(void)
{
    tev ev;
    int i;
    files_clear();
    file_add("aaaaaaaa", "main.lua", "api = 1\non.think('e', function(self) if now() > 1.5 then local x = nil; x.y = 1 end print('a') end)\n");
    file_add("bbbbbbbb", "main.lua", "api = 1\non.think('e', function(self) print('b ' .. now()) end)\n"
                                    "every(0, function() print('tick b') end)\n");
    {
        sh_script_mod_desc d[2] = { { "aaaaaaaa", "Mod A" }, { "bbbbbbbb", "Mod B" } };
        CHECK(sh_script_map_start(d, 2, &g_src, 3, 0.0) == 2);
    }
    memset(&ev, 0, sizeof ev);
    ev.kind = TEV_SPAWN;
    ev.entity = 1;
    strcpy(ev.names, "mods/aaaaaaaa/e");
    CHECK(t_enqueue("aaaaaaaa", &ev));
    ev.entity = 2;
    strcpy(ev.names, "mods/bbbbbbbb/e");
    CHECK(t_enqueue("bbbbbbbb", &ev));
    for (i = 1; i <= 4; i++) sh_script_tick((double)i, 1.0);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_STOPPED);
    CHECK(sh_script_status("bbbbbbbb") == SH_SCRIPT_RUNNING);
    CHECK(!strcmp(stop_of("aaaaaaaa"), "Script stopped: Mod A, scripts\\main.lua line 2: \"x\" is nothing here. Check that it is set before it is used."));
    CHECK(!strcmp(stop_of("bbbbbbbb"), ""));
    CHECK(strstr(log_of("bbbbbbbb"), "b 4.0\ntick b\n") != NULL);
    ev.entity = 3;
    strcpy(ev.names, "mods/aaaaaaaa/e");
    CHECK(t_enqueue("aaaaaaaa", &ev) == 0);          /* a stopped mod's queue refuses */
    CHECK(sh_script_binds("mods/aaaaaaaa/e", NULL, 0, NULL) == 0);
    CHECK(t_owner(1, NULL, 0) == 0);
    CHECK(t_owner(2, NULL, 0) == 1);
    sh_script_map_end();
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_STOPPED);
    CHECK(sh_script_status("bbbbbbbb") == SH_SCRIPT_ENDED);
    /* the logs stay readable after the map; the next map start clears them */
    CHECK(strstr(log_of("aaaaaaaa"), "Script stopped: Mod A") != NULL);
    {
        sh_script_mod_desc d = { "aaaaaaaa", "Mod A" };
        files_clear();
        file_add("aaaaaaaa", "main.lua", "api = 1\nprint('again')\n");
        sh_script_map_start(&d, 1, &g_src, 3, 0.0);
    }
    CHECK(!strcmp(log_of("aaaaaaaa"), "again\n"));
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    CHECK(strstr(log_of("bbbbbbbb"), "tick b") != NULL);       /* not in this map: kept */
    sh_script_map_end();
}

static void test_reentrancy(void)
{
    tev ev;
    files_clear();
    /* each input raises another while Lua runs: delivered after the handler
     * returns, in up to four passes a tick, the rest next tick */
    file_add("aaaaaaaa", "main.lua",
        "api = 1\nlocal n = 0\n"
        "on.input('chain', function(node) n = n + 1 print('in ' .. n .. ' at ' .. now()) "
        "if n < 10 then assert(poke('aaaaaaaa', 5, 1, 'chain')) print('after poke ' .. n) end end)\n"
        "on.input('end', function() end_map() print('still here') end)\n");
    start1("aaaaaaaa", "Test Mod", 1);
    memset(&ev, 0, sizeof ev);
    ev.kind = TEV_INPUT;
    ev.entity = 1;
    strcpy(ev.names, "chain");
    CHECK(t_enqueue("aaaaaaaa", &ev));
    sh_script_tick(1.0, 1.0);
    log_of("aaaaaaaa");
    CHECK(strstr(g_log, "in 1 at 1.0\nafter poke 1\nin 2 at 1.0\n") != NULL);
    CHECK(strstr(g_log, "in 4 at 1") != NULL && strstr(g_log, "in 5") == NULL);
    sh_script_tick(2.0, 1.0);
    log_of("aaaaaaaa");
    CHECK(strstr(g_log, "in 5 at 2") != NULL && strstr(g_log, "in 8 at 2") != NULL && strstr(g_log, "in 9") == NULL);
    sh_script_tick(3.0, 1.0);
    log_of("aaaaaaaa");
    CHECK(strstr(g_log, "in 10 at 3") != NULL && strstr(g_log, "in 11") == NULL);

    /* a map end requested while Lua runs waits until it returns */
    strcpy(ev.names, "end");
    CHECK(t_enqueue("aaaaaaaa", &ev));
    sh_script_tick(4.0, 1.0);
    CHECK(strstr(log_of("aaaaaaaa"), "still here") != NULL);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_ENDED);
    CHECK(sh_script_bytes_in_use() == 0);
    sh_script_map_end();

    /* overflow: 1024 records, then refusals and one line */
    files_clear();
    file_add("aaaaaaaa", "main.lua", "api = 1\non.input('x', function() end)\n");
    start1("aaaaaaaa", "Test Mod", 1);
    {
        int i, ok = 0;
        for (i = 0; i < 1100; i++) ok += t_enqueue("aaaaaaaa", &ev);
        CHECK(ok == SH_SCRIPT_QUEUE_CAP);
        sh_script_tick(1.0, 1.0);
        for (i = 0; i < 1100; i++) t_enqueue("aaaaaaaa", &ev);
        log_of("aaaaaaaa");
        CHECK(strstr(g_log, "more than 1024 arrived in one frame") != NULL);
        CHECK(strstr(strstr(g_log, "more than 1024") + 1, "more than 1024") == NULL);   /* once a map */
    }
    sh_script_tick(2.0, 1.0);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    sh_script_map_end();
}

static void test_timers(void)
{
    int st;
    files_clear();
    file_add("aaaaaaaa", "main.lua",
        "api = 1\n"
        "after(1.5, function() print('after 1.5 at ' .. now()) end)\n"
        "local e = every(1, function() print('every at ' .. now()) end)\n"
        "local c = after(2, function() print('cancelled ran') end)\n"
        "c:cancel()\n"
        "run(function()\n"
        "  print('run start ' .. now())\n"
        "  wait(2)\n"
        "  print('run woke ' .. now())\n"
        "  wait()\n"
        "  print('run next ' .. now())\n"
        "  e:cancel()\n"
        "end)\n"
        "after(0, function() print('after 0 at ' .. now()) end)\n");
    start1("aaaaaaaa", "Test Mod", 1);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_RUNNING);
    sh_script_tick(0.5, 0.5);
    sh_script_tick(1.0, 0.5);
    sh_script_tick(1.5, 0.5);
    sh_script_tick(2.0, 0.5);
    sh_script_tick(2.5, 0.5);
    sh_script_tick(3.0, 0.5);
    sh_script_tick(4.0, 1.0);
    log_of("aaaaaaaa");
    CHECK(!strcmp(g_log,
        "run start 0.0\n"
        "after 0 at 0.5\n"
        "every at 1.0\n"
        "after 1.5 at 1.5\n"
        "every at 2.0\n"
        "run woke 2.0\n"
        "run next 2.5\n"));
    if (g_fail) printf("timers log:\n%s", g_log);
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);

    /* an error inside a waiting coroutine names its own line */
    st = run_one("api = 1\nrun(function()\n  wait(1)\n  local t = nil\n  print(t.x)\nend)\n");
    CHECK(st == SH_SCRIPT_RUNNING);
    sh_script_tick(1.0, 1.0);
    CHECK(!strcmp(stop_of("aaaaaaaa"), "Script stopped: Test Mod, scripts\\main.lua line 5: \"t\" is nothing here. "
                                       "Check that it is set before it is used."));
    sh_script_map_end();

    /* wait inside pcall inside run still waits */
    st = run_one("api = 1\nrun(function() local ok = pcall(function() wait(1) print('woke in pcall') end) print('pcall ' .. tostring(ok)) end)\n");
    CHECK(st == SH_SCRIPT_RUNNING);
    sh_script_tick(1.0, 1.0);
    CHECK(strstr(log_of("aaaaaaaa"), "woke in pcall\npcall true\n") != NULL);
    sh_script_map_end();

    /* wait in a coroutine of the script's own is refused */
    st = run_one("api = 1\nlocal co = coroutine.wrap(function() wait(1) end)\nco()\n");
    CHECK(st == SH_SCRIPT_STOPPED);
    CHECK(strstr(stop_of("aaaaaaaa"), "\"wait\" can only be used inside run(function() ... end)") != NULL);
    sh_script_map_end();

    /* bad arguments */
    st = run_one("api = 1\nafter(-1, function() end)\n");
    CHECK(strstr(stop_of("aaaaaaaa"), "\"after\" needs a number of seconds of 0 or more") != NULL);
    sh_script_map_end();
    st = run_one("api = 1\nevery(1)\n");
    CHECK(strstr(stop_of("aaaaaaaa"), "\"every\" needs a function as its second value, got nothing") != NULL);
    sh_script_map_end();
}

static void test_random(void)
{
    char a[2048], b[2048];
    files_clear();
    file_add("aaaaaaaa", "main.lua", "api = 1\nlocal t = {} for i = 1, 8 do t[i] = math.random(1000) end print(table.concat(t, ','))\n");
    file_add("bbbbbbbb", "main.lua", "api = 1\nlocal t = {} for i = 1, 8 do t[i] = math.random(1000) end print(table.concat(t, ','))\n");
    {
        sh_script_mod_desc d[2] = { { "aaaaaaaa", "Mod A" }, { "bbbbbbbb", "Mod B" } };
        sh_script_map_start(d, 2, &g_src, 12345, 0.0);
    }
    strcpy(a, log_of("aaaaaaaa"));
    strcpy(b, log_of("bbbbbbbb"));
    CHECK(a[0] && !strcmp(a, b));
    sh_script_map_end();
    start1("aaaaaaaa", "Mod A", 12345);
    CHECK(!strcmp(log_of("aaaaaaaa"), a));
    sh_script_map_end();
    start1("aaaaaaaa", "Mod A", 999);
    CHECK(strcmp(log_of("aaaaaaaa"), a) != 0);
    sh_script_map_end();
}

static void test_teardown(void)
{
    int i;
    tev ev;
    files_clear();
    file_add("aaaaaaaa", "main.lua",
        "api = 1\nlocal big = {}\nfor i = 1, 20000 do big[i] = { i, tostring(i) } end\n"
        "on.think('e', function(self) self.state.big = big end)\n"
        "every(0.1, function() end)\nrun(function() while true do wait(1) end end)\n"
        "on.map_end(function() print('map end ' .. #big) end)\n");
    file_add("bbbbbbbb", "main.lua", "api = 1\non.map_end(function() while true do end end)\n");
    {
        sh_script_mod_desc d[2] = { { "aaaaaaaa", "Mod A" }, { "bbbbbbbb", "Mod B" } };
        CHECK(sh_script_map_start(d, 2, &g_src, 3, 0.0) == 2);
    }
    memset(&ev, 0, sizeof ev);
    ev.kind = TEV_SPAWN;
    strcpy(ev.names, "mods/aaaaaaaa/e");
    for (i = 1; i <= 50; i++) { ev.entity = (uint64_t)i; t_enqueue("aaaaaaaa", &ev); }
    for (i = 1; i <= 20; i++) sh_script_tick(i * 0.1, 0.1);
    CHECK(sh_script_bytes_in_use() > 1000000);
    CHECK(sh_script_active());
    sh_script_map_end();
    CHECK(!sh_script_active());
    CHECK(sh_script_bytes_in_use() == 0);
    CHECK(strstr(log_of("aaaaaaaa"), "map end 20000") != NULL);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_ENDED);
    CHECK(sh_script_status("bbbbbbbb") == SH_SCRIPT_STOPPED);      /* its map_end ran too long */
    CHECK(strstr(stop_of("bbbbbbbb"), "took too long") != NULL);
    CHECK(t_owner(1, NULL, 0) == 0);
    /* calls after the map are harmless */
    sh_script_tick(5.0, 0.1);
    CHECK(t_enqueue("aaaaaaaa", &ev) == 0);
    sh_script_map_end();

    /* a mod with no entry files gets no state; a list failure is a stop line */
    files_clear();
    file_add("aaaaaaaa", "lib/only.lua", "return 1\n");
    CHECK(start1("aaaaaaaa", "Lib Only", 1) == 0);
    CHECK(sh_script_status("aaaaaaaa") == SH_SCRIPT_NONE);
    sh_script_map_end();
    g_list_fail = 1;
    CHECK(start1("aaaaaaaa", "Broken", 1) == 0);
    CHECK(strstr(stop_of("aaaaaaaa"), "this mod's scripts could not be read") != NULL);
    g_list_fail = 0;
    sh_script_map_end();
    CHECK(sh_script_bytes_in_use() == 0);
}

static void test_log_cap(void)
{
    int n;
    run_one("api = 1\nfor i = 1, 500 do print('line ' .. i) end\nerror('done')\n");
    n = sh_script_log_read("aaaaaaaa", NULL, NULL);
    CHECK(n == SH_SCRIPT_LOG_LINES + 2);
    log_of("aaaaaaaa");
    CHECK(strstr(g_log, "line 200\nMore than 200 lines were written in this map; the rest are not kept.\nScript stopped: ") != NULL);
    sh_script_map_end();
}

/* The loader and the runtime fed malformed text: never a crash or a hang,
 * and every stop line plain. */
static unsigned g_rng = 12345;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 16) & 0x7fff; }

static void test_fuzz(void)
{
    static const char *seeds[] = {
        "api = 1\non.think('e', function(self, dt) self.state.n = (self.state.n or 0) + dt end)\n",
        "api = 1\nlocal t = {} for i = 1, 10 do t[#t + 1] = ('x'):rep(i) end print(table.concat(t))\n",
        "api = 1\nrun(function() for i = 1, 3 do wait(0.5) print(i) end end)\n",
        "api = 1\nlocal s = 'a=1, b=2' for k, v in s:gmatch('(%w+)=(%w+)') do print(k, v) end\n",
        "api = 1\nlocal function f(n) if n < 2 then return n end return f(n - 1) + f(n - 2) end print(f(15))\n",
        "api = 1\non.input('x', function(node, a) after(1, function() print('later') end) end)\n",
    };
    char buf[1024];
    int iter, bad = 0;
    DWORD t0 = GetTickCount();
    for (iter = 0; iter < 1500; iter++) {
        size_t len, i;
        int mode = (int)(rnd() % 4);
        const char *s = seeds[rnd() % (sizeof seeds / sizeof seeds[0])];
        if (mode == 0) {                       /* random bytes */
            len = rnd() % 300;
            for (i = 0; i < len; i++) buf[i] = (char)(rnd() & 0xff);
        } else if (mode == 1) {                /* api line, then random printable text */
            strcpy(buf, "api = 1\n");
            len = 8 + rnd() % 300;
            for (i = 8; i < len; i++) buf[i] = (char)(32 + rnd() % 95);
        } else {                               /* a valid script, cut and mutated */
            len = strlen(s);
            memcpy(buf, s, len);
            if (mode == 2) len = 8 + rnd() % (len - 8 + 1);
            for (i = 0; i < 1 + rnd() % 4; i++) {
                size_t at = 8 + rnd() % (len > 8 ? len - 8 : 1);
                if (at < len) buf[at] = (char)("()[]{}=.,'\"end \n\0x9-+*#~"[rnd() % 23]);
            }
        }
        files_clear();
        file_add_bytes("aaaaaaaa", "main.lua", buf, len);
        if (rnd() % 3 == 0) file_add_bytes("aaaaaaaa", "lib/x.lua", buf, len);
        start1("aaaaaaaa", "Fuzz", iter);
        {
            tev ev;
            memset(&ev, 0, sizeof ev);
            ev.kind = TEV_SPAWN;
            ev.entity = 1;
            strcpy(ev.names, "mods/aaaaaaaa/e");
            t_enqueue("aaaaaaaa", &ev);
            ev.kind = TEV_INPUT;
            strcpy(ev.names, "x");
            t_enqueue("aaaaaaaa", &ev);
        }
        sh_script_tick(0.5, 0.5);
        sh_script_tick(1.5, 1.0);
        if (sh_script_status("aaaaaaaa") == SH_SCRIPT_STOPPED) {
            const char *line = stop_of("aaaaaaaa");
            if (strncmp(line, "Script stopped: Fuzz", 20) || strstr(line, "attempt to") || strstr(line, "<eof>") ||
                strstr(line, "nil value") || strstr(line, "[string") || line[strlen(line) - 1] != '.') {
                if (bad < 5) printf("unplain stop line: %s\n", line);
                bad++;
            }
        }
        sh_script_map_end();
        if (sh_script_bytes_in_use() != 0) { printf("bytes left after fuzz case %d\n", iter); bad++; break; }
    }
    CHECK(bad == 0);
    CHECK(GetTickCount() - t0 < 60000);
}

static void test_reason_table(void)
{
    char what[300], todo[200], file[64];
    int line;
    const char *rest = sh_script_split_where("scripts\\lib\\fsm.lua:42: boom", file, sizeof file, &line);
    CHECK(!strcmp(file, "scripts\\lib\\fsm.lua") && line == 42 && !strcmp(rest, "boom"));
    rest = sh_script_split_where("the door: open", file, sizeof file, &line);
    CHECK(!file[0] && line == 0 && !strcmp(rest, "the door: open"));
    sh_script_reason("attempt to call a nil value (method 'move_to')", what, sizeof what, todo, sizeof todo);
    CHECK(!strcmp(what, "\"move_to\" is not something this value can do") && !strcmp(todo, "Check the name of the call"));
    sh_script_reason("bad argument #1 to 'move_to' (number expected, got nil)", what, sizeof what, todo, sizeof todo);
    CHECK(!strcmp(what, "\"move_to\" needs a number as its first value, got nil"));
    sh_script_reason("bad argument #2 to 'insert' (position out of bounds)", what, sizeof what, todo, sizeof todo);
    CHECK(!strcmp(what, "\"insert\" cannot use its second value (position out of bounds)"));
    sh_script_reason("calling 'pos' on bad self (entity expected, got table)", what, sizeof what, todo, sizeof todo);
    CHECK(!strcmp(what, "\"pos\" needs an entity as the value before the colon, got a table"));
    sh_script_reason("attempt to load a binary chunk (mode is 't')", what, sizeof what, todo, sizeof todo);
    CHECK(!strcmp(what, "the file is compiled Lua, not script text, so it did not run"));
    sh_script_reason("'=' expected near 'x'", what, sizeof what, todo, sizeof todo);
    CHECK(!strcmp(what, "\"=\" expected near \"x\""));
    sh_script_reason("monster near the door", what, sizeof what, todo, sizeof todo);
    CHECK(!strcmp(what, "monster near the door") && !todo[0]);
}

int main(void)
{
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "resolve", t_resolve);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "poke", t_poke);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "end_map", t_end_map);
    sh_script_api_add(SH_SCRIPT_API_GLOBAL, "handle", t_handle);
    sh_script_api_add(SH_SCRIPT_API_METHOD, "owns", t_owns);
    sh_script_handles_set_resolver(fake_resolve);

    test_reason_table();
    test_absent();
    test_bytecode();
    test_budget();
    test_memory();
    test_pattern();
    test_api_declaration();
    test_error_lines();
    test_dispatch();
    test_isolation();
    test_reentrancy();
    test_timers();
    test_random();
    test_teardown();
    test_log_cap();
    test_fuzz();
    files_clear();
    CHECK(sh_script_bytes_in_use() == 0);
    printf(g_fail ? "script_sandbox_test: FAILED\n" : "script_sandbox_test: ok\n");
    return g_fail;
}
