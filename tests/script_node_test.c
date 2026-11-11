/* script_node_test.c -- the Script node and map variables (src/backend/script/script_node.c).
 *
 * Without the game:
 *   - which scripts give their mod a Script node (on.input outside comments
 *     and strings), the node's entityDef name, the generated row (its two
 *     decls, its strings, the files an author may not write);
 *   - the played map's names: Script nodes by uniqueId from the map JSON,
 *     variables by type and list position, a name two variables share;
 *   - the door example (tests\fixtures\scripts\door-logic) end to end through
 *     the runtime: three inputs into Script nodes named "switch_used" fire
 *     every Script node named "open_door" of that mod once, with the
 *     activator, and no other node; an input no script handles and a fire
 *     that finds no node each write one plain line and the mod keeps running;
 *   - vars.get and vars.set on the four variable types, clamped writes, the
 *     changed notification only on change, and the plain refusals.
 * With the executables (run-tests passes them): every private signature
 * resolves once at its recorded RVA, the Relay window is the bytes the
 * installer compares and whole instructions, the Relay calls FireOutput with
 * event 8, idGameLocal vtable +0x720 returns gameLocal + 0x49C510, and the
 * stock Integer Operation calls the variable-changed notification.
 *
 *   script_node_test.exe <tests\fixtures\scripts> [DOOMx64vk.exe] [DOOMx64.exe] */
#include "heap_out.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "script/script_runtime.h"
#include "script/script_hooks.h"
#include "script/script_source.h"
#include "script/script_node.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static char g_base[MAX_PATH];

/* ================================================================ pure */

static void test_registers_input(void)
{
    static const struct { const char *text; int want; } cases[] = {
        { "api = 1\non.input(\"a\", function() end)", 1 },
        { "on . input ( 'a', f )", 1 },
        { "on.input\n(\"a\", f)", 1 },
        { "on.input \"a\"", 1 },
        { "-- on.input(\"a\", f)\napi = 1", 0 },
        { "--[[ on.input(\"a\", f) ]] api = 1", 0 },
        { "--[==[ on.input( ]] still a comment ]==] x = 1", 0 },
        { "s = \"on.input(\\\"a\\\")\"", 0 },
        { "s = [[on.input(]]", 0 },
        { "t.on.input(\"a\", f)", 0 },
        { "won.input(\"a\", f)", 0 },
        { "on.inputs(\"a\", f)", 0 },
        { "on.spawn(\"ai/x\", f)", 0 },
        { "local x = 1 on.input(\"door\", f)", 1 },
    };
    int i;
    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
        int got = sh_script_node_registers_input(cases[i].text, strlen(cases[i].text));
        if (got != cases[i].want) printf("  case %d: \"%s\" gave %d\n", i, cases[i].text, got);
        CHECK(got == cases[i].want);
    }
    CHECK(!sh_script_node_registers_input(NULL, 0));
}

static void test_row(void)
{
    char salt[32];
    sh_script_node_row row;
    CHECK(sh_script_node_is_def("mods/doorlogc/script", salt, sizeof salt) && !strcmp(salt, "doorlogc"));
    CHECK(sh_script_node_is_def("MODS\\DoorLogc\\Script", salt, sizeof salt) && !strcmp(salt, "DoorLogc"));
    CHECK(!sh_script_node_is_def("mods/doorlogc/scripts", NULL, 0));
    CHECK(!sh_script_node_is_def("mods/doorlogc/logic/script", NULL, 0));
    CHECK(!sh_script_node_is_def("snapmaps/logic/relay", NULL, 0));
    CHECK(!sh_script_node_is_def("mods//script", NULL, 0));
    {
        /* a node placed in a map: the map's own entityDef for it first, the mod's node second */
        const char *placed[] = { "1_s_ind_player_start/mods/doorlogc/script_404", "mods/doorlogc/script" };
        const char *direct[] = { "mods/doorlogc/script" };
        const char *other[]  = { "1_s_ind_player_start/mods/scprobe1/probe/marker_12", "mods/scprobe1/probe/marker" };
        CHECK(sh_script_node_in_chain(placed, 2));
        CHECK(sh_script_node_in_chain(direct, 1));
        CHECK(!sh_script_node_in_chain(other, 2));
        CHECK(!sh_script_node_in_chain(placed, 1));
        CHECK(!sh_script_node_in_chain(NULL, 0));
        CHECK(sh_script_node_in_chain_text("1_s_ind_player_start/mods/doorlogc/script_404\nmods/doorlogc/script"));
        CHECK(sh_script_node_in_chain_text("mods/doorlogc/script"));
        CHECK(!sh_script_node_in_chain_text("1_s_ind_player_start/mods/doorlogc/script_404"));
        CHECK(!sh_script_node_in_chain_text("1_x/mods/scprobe1/probe/marker_12\nmods/scprobe1/probe/marker"));
        CHECK(!sh_script_node_in_chain_text("") && !sh_script_node_in_chain_text(NULL));
    }

    CHECK(sh_script_node_reserved("doorlogc", "generated/decls/entitydef/mods/doorlogc/script.decl"));
    CHECK(sh_script_node_reserved("doorlogc", "Generated\\Decls\\SnapEditorEntityDef\\mods\\DOORLOGC\\script.decl"));
    CHECK(!sh_script_node_reserved("doorlogc", "generated/decls/entitydef/mods/doorlogc/script2.decl"));
    CHECK(!sh_script_node_reserved("other123", "generated/decls/entitydef/mods/doorlogc/script.decl"));

    CHECK(sh_script_node_row_make("doorlogc", "Door Logic", NULL, &row));
    CHECK(!strcmp(row.def_path, "generated/decls/entitydef/mods/doorlogc/script.decl"));
    CHECK(!strcmp(row.row_path, "generated/decls/snapeditorentitydef/mods/doorlogc/script.decl"));
    CHECK(strstr(row.def_text, "inherit = \"snapmaps/logic/relay\";") != NULL);
    CHECK(strstr(row.row_text, "inherit = \"logic/relay\";") != NULL);
    CHECK(strstr(row.row_text, "entityDef = \"mods/doorlogc/script\";") != NULL);
    CHECK(strstr(row.row_text, "displayNameTag = \"#str_doorlogc_script_node\";") != NULL);
    CHECK(strstr(row.row_text, "categoryNameTag = \"#str_doorlogc_script_category\";") != NULL);
    CHECK(strstr(row.row_text, "path = \"displayName\";") != NULL);
    CHECK(strstr(row.row_text, "onlyFireOnce") == NULL);
    CHECK(row.nstrings == 3);
    CHECK(!strcmp(row.string_key[0], "doorlogc_script_node") && !strcmp(row.string_text[0], "Script"));
    CHECK(!strcmp(row.string_key[2], "doorlogc_script_category") && !strcmp(row.string_text[2], "Door Logic"));
    CHECK(sh_script_node_reserved("doorlogc", row.def_path) && sh_script_node_reserved("doorlogc", row.row_path));
    sh_script_node_row_free(&row);

    /* a mod with its own Toybox category puts the node in it */
    CHECK(sh_script_node_row_make("doorlogc", "Door Logic", "#str_doorlogc_category", &row));
    CHECK(strstr(row.row_text, "categoryNameTag = \"#str_doorlogc_category\";") != NULL);
    CHECK(row.nstrings == 2);
    sh_script_node_row_free(&row);
    CHECK(!sh_script_node_row_make("bad/salt", "x", NULL, &row));
    CHECK(!sh_script_node_row_make("", "x", NULL, &row));
}

/* The played map: Script nodes 61, 62, 66 of doorlogc ("switch_used" twice
 * in two spellings, "open_door"), 63 another "open_door" of doorlogc, 64 a
 * stock Relay named "open_door", 65 a Script node of another mod. */
#define ENT(uid, inherit, name) \
    "{\"displayName\":\"" name "\",\"entityDef\":{\"className\":\"idSnapMapLogic_Relay\",\"inherit\":\"" inherit "\"," \
    "\"state\":{\"edit\":{}}},\"uniqueId\":" #uid ",\"~type\":\"idSnapEntity\"}"
#define VAR(t, name) "{\"info\":{\"name\":\"" name "\"},\"~type\":\"" t "\"}"
static const char k_map[] =
    "{\"entities\":["
    ENT(61, "mods/doorlogc/script", "switch_used") ","
    ENT(62, "mods/doorlogc/script", "open_door") ","
    ENT(63, "mods/doorlogc/script", "open_door") ","
    ENT(64, "snapmaps/logic/relay", "open_door") ","
    ENT(65, "mods/othermod/script", "open_door") ","
    ENT(66, "mods/doorlogc/script", "Switch_Used") ","
    ENT(67, "mods/doorlogc/script", "") ","
    ENT(68, "mods/doorlogc/script", "caf\\u00e9 \\\"door\\\"")
    "],"
    "\"variables\":{\"allocCount\":[2,2,2,0,3,0,0,0,0,0],"
    "\"boolean\":[" VAR("snapVarBoolean_t", "Door Open") "," VAR("snapVarBoolean_t", "dup") "],"
    "\"integer\":[" VAR("snapVarInteger_t", "Lives") "," VAR("snapVarInteger_t", "Score") "],"
    "\"number\":[" VAR("snapVarNumber_t", "speed") "," VAR("snapVarNumber_t", "Gravity") "],"
    "\"string\":[" VAR("snapVarString_t", "smp.render.v1") "," VAR("snapVarString_t", "label") "," VAR("snapVarString_t", "DUP") "],"
    "\"~type\":\"idSnapVariables\"},\"version\":111,\"~type\":\"idSnapMap\"}";

static void test_map_names(void)
{
    char name[128], err[160];
    int type, index, other;
    CHECK(sh_script_node_map_load(k_map, sizeof k_map - 1, err, sizeof err));
    CHECK(HOS(name, sh_script_node_name_dup(61)) && !strcmp(name, "switch_used"));
    CHECK(HOS(name, sh_script_node_name_dup(62)) && !strcmp(name, "open_door"));
    CHECK(HOS(name, sh_script_node_name_dup(65)) && !strcmp(name, "open_door"));
    CHECK(HOS(name, sh_script_node_name_dup(67)) && !strcmp(name, ""));
    CHECK(HOS(name, sh_script_node_name_dup(68)) && !strcmp(name, "caf\xC3\xA9 \"door\""));
    CHECK(!HOS(name, sh_script_node_name_dup(64)));        /* a stock Relay is no Script node */
    CHECK(!HOS(name, sh_script_node_name_dup(99)));

    CHECK(sh_script_node_var_find("Score", &type, &index, &other) == 1 && type == SH_SCRIPT_VAR_INTEGER && index == 1);
    CHECK(sh_script_node_var_find("score", &type, &index, &other) == 1 && index == 1);
    CHECK(sh_script_node_var_find("Gravity", &type, &index, &other) == 1 && type == SH_SCRIPT_VAR_NUMBER && index == 1);
    CHECK(sh_script_node_var_find("door open", &type, &index, &other) == 1 && type == SH_SCRIPT_VAR_BOOLEAN && index == 0);
    CHECK(sh_script_node_var_find("label", &type, &index, &other) == 1 && type == SH_SCRIPT_VAR_STRING && index == 1);
    CHECK(sh_script_node_var_find("dup", &type, &index, &other) == -1 && type == SH_SCRIPT_VAR_BOOLEAN &&
          other == SH_SCRIPT_VAR_STRING);
    CHECK(sh_script_node_var_find("nope", &type, &index, &other) == 0 && type == -1);

    CHECK(sh_script_node_uid_from_name("3_maps/modules/x/room01/mods/doorlogc/script_61") == 61);
    CHECK(sh_script_node_uid_from_name("0_m/snapmaps/logic/relay_0") == 0);
    CHECK(sh_script_node_uid_from_name("player1") == -1);
    CHECK(sh_script_node_uid_from_name("a_") == -1);
    CHECK(sh_script_node_uid_from_name("a_12x") == -1);
    CHECK(sh_script_node_uid_from_name(NULL) == -1);

    /* a map that cannot be read leaves no names behind */
    CHECK(!sh_script_node_map_load("{\"entities\":", 12, err, sizeof err));
    CHECK(strstr(err, "could not be read") != NULL);
    CHECK(!HOS(name, sh_script_node_name_dup(61)));
    /* a map without Script nodes or variables reads as empty */
    {
        static const char plain[] = "{\"entities\":[],\"instances\":[],\"variables\":{}}";
        int type = -1, index = -1, other = -1;
        CHECK(sh_script_node_map_load(plain, sizeof plain - 1, err, sizeof err));
        CHECK(!HOS(name, sh_script_node_name_dup(61)));
        CHECK(!sh_script_node_var_find("door_open", &type, &index, &other));
    }
    CHECK(sh_script_node_map_load(k_map, sizeof k_map - 1, err, sizeof err));
}

/* ================================================== the fake engine */

typedef struct fake_node { sh_script_handle h; int uid; } fake_node;
static fake_node g_nodes[16];
static int g_nnodes;
static struct { sh_script_handle node, activator; } g_fired[32];
static int g_nfired;

static int32_t g_int[2] = { 3, 10 }, g_int_bounds[2][2] = { { 0, 9 }, { 0, 100 } };
static float g_num[2] = { 1.0f, 9.8f }, g_num_bounds[2][2] = { { 0.0f, 10.0f }, { -100.0f, 100.0f } };
static int g_bool[2] = { 0, 0 };
static char *g_str[3] = { "4;0", "start", "" };
static int g_str_owned[3];
static int g_notified[5];
static int g_vars_ready = 1;

static int g_many;   /* when set: the mod has this many Script nodes, each the first recorded one */
static int fake_nodes(void *c, int mod, sh_script_handle *out, int cap)
{
    int i, n = 0;
    (void)c;
    if (g_many) {
        for (i = 0; i < g_many && n < cap; i++) out[n++] = g_nodes[0].h;
        return n;
    }
    for (i = 0; i < g_nnodes && n < cap; i++)
        if (sh_script_handle_mod(g_nodes[i].h) == mod && sh_script_handle_known(g_nodes[i].h)) out[n++] = g_nodes[i].h;
    return n;
}

static int fake_uid(void *c, sh_script_handle node)
{
    int i;
    (void)c;
    for (i = 0; i < g_nnodes; i++) if (g_nodes[i].h == node) return g_nodes[i].uid;
    return -1;
}

static int fake_fire(void *c, sh_script_handle node, sh_script_handle activator)
{
    (void)c;
    if (g_nfired < 32) { g_fired[g_nfired].node = node; g_fired[g_nfired].activator = activator; g_nfired++; }
    return 1;
}

static int fake_count(void *c, int type)
{
    (void)c;
    if (!g_vars_ready) return -1;
    switch (type) {
    case SH_SCRIPT_VAR_INTEGER: return 2;
    case SH_SCRIPT_VAR_NUMBER:  return 2;
    case SH_SCRIPT_VAR_BOOLEAN: return 2;
    case SH_SCRIPT_VAR_STRING:  return 3;
    }
    return -1;
}

static int fake_get(void *c, int type, int index, sh_script_node_value *out)
{
    (void)c;
    out->type = type;
    switch (type) {
    case SH_SCRIPT_VAR_INTEGER: out->i = g_int[index]; return 1;
    case SH_SCRIPT_VAR_NUMBER:  out->n = g_num[index]; return 1;
    case SH_SCRIPT_VAR_BOOLEAN: out->b = g_bool[index]; return 1;
    case SH_SCRIPT_VAR_STRING:  out->s = _strdup(g_str[index]); return out->s != NULL;
    }
    return 0;
}

static int fake_set(void *c, int type, int index, const sh_script_node_value *in, sh_script_node_value *stored)
{
    int changed = 0;
    switch (type) {
    case SH_SCRIPT_VAR_INTEGER: {
        long long v = in->i;
        if (v < g_int_bounds[index][0]) v = g_int_bounds[index][0];
        if (v > g_int_bounds[index][1]) v = g_int_bounds[index][1];
        if (g_int[index] != (int32_t)v) { g_int[index] = (int32_t)v; changed = 1; }
        break;
    }
    case SH_SCRIPT_VAR_NUMBER: {
        double v = in->n;
        if (v < g_num_bounds[index][0]) v = g_num_bounds[index][0];
        if (v > g_num_bounds[index][1]) v = g_num_bounds[index][1];
        if (g_num[index] != (float)v) { g_num[index] = (float)v; changed = 1; }
        break;
    }
    case SH_SCRIPT_VAR_BOOLEAN:
        if (g_bool[index] != (in->b != 0)) { g_bool[index] = in->b != 0; changed = 1; }
        break;
    case SH_SCRIPT_VAR_STRING:
        if (strcmp(g_str[index], in->s)) {
            char *copy = _strdup(in->s);
            if (!copy) return 0;
            if (g_str_owned[index]) free(g_str[index]);
            g_str[index] = copy;
            g_str_owned[index] = 1;
            changed = 1;
        }
        break;
    default:
        return 0;
    }
    if (changed) g_notified[type]++;
    return fake_get(c, type, index, stored);
}

static const sh_script_node_ops k_fake = { NULL, fake_nodes, fake_uid, fake_fire, fake_count, fake_get, fake_set };

static int g_token;
static void *resolve(uint32_t spawn_id) { (void)spawn_id; return &g_token; }

/* -------------------------------------------------- script sources */

/* Mods given as text: salt -> one entry file. */
typedef struct mem_mod { const char *salt, *name, *file, *text; } mem_mod;
static mem_mod g_mem[8];
static int g_nmem;
static const char *g_folder_salt;     /* the door fixture, read from disk */

static int mem_list(void *c, const char *salt, sh_script_file_fn fn, void *fc)
{
    int i, n = 0;
    (void)c;
    if (g_folder_salt && !_stricmp(salt, g_folder_salt))
        return sh_script_source_files()->list(sh_script_source_files()->context, salt, fn, fc);
    for (i = 0; i < g_nmem; i++) if (!_stricmp(g_mem[i].salt, salt)) { fn(fc, g_mem[i].file); n++; }
    return n;
}

static int mem_read(void *c, const char *salt, const char *rel, char **text, size_t *len)
{
    int i;
    (void)c;
    if (g_folder_salt && !_stricmp(salt, g_folder_salt))
        return sh_script_source_files()->read(sh_script_source_files()->context, salt, rel, text, len);
    for (i = 0; i < g_nmem; i++) {
        if (_stricmp(g_mem[i].salt, salt) || strcmp(g_mem[i].file, rel)) continue;
        *len = strlen(g_mem[i].text);
        *text = (char *)malloc(*len + 1);
        if (!*text) return -1;
        memcpy(*text, g_mem[i].text, *len + 1);
        return 1;
    }
    return 0;
}

/* Both sources hand out malloc'd text. */
static void mem_release(void *c, char *text)
{
    (void)c;
    free(text);
}

static const sh_script_source k_mem = { NULL, mem_list, mem_read, mem_release };

static int provider(void *c, sh_script_mod_desc *out, int cap, const sh_script_source **source, uint64_t *seed)
{
    int i, n = 0;
    (void)c;
    *source = &k_mem;
    *seed = 7;
    if (g_folder_salt && n < cap) { out[n].salt = g_folder_salt; out[n].name = "Door Logic"; n++; }
    for (i = 0; i < g_nmem && n < cap; i++) { out[n].salt = g_mem[i].salt; out[n].name = g_mem[i].name; n++; }
    return n;
}

static char g_out[65536];
static void cat_line(void *c, const char *line)
{
    (void)c;
    if (strlen(g_out) + strlen(line) + 2 < sizeof g_out) { strcat_s(g_out, sizeof g_out, line); strcat_s(g_out, sizeof g_out, "\n"); }
}
static const char *log_of(const char *salt)
{
    g_out[0] = 0;
    sh_script_log_read(salt, cat_line, NULL);
    return g_out;
}

static sh_script_handle record_node(uint32_t spawn_id, const char *def, int uid)
{
    const char *names[2];
    sh_script_handle h;
    names[0] = def;
    names[1] = "snapmaps/logic/relay";
    h = sh_script_hooks_record(spawn_id, names, 2, NULL);
    if (h && g_nnodes < 16) { g_nodes[g_nnodes].h = h; g_nodes[g_nnodes].uid = uid; g_nnodes++; }
    return h;
}

/* A test-only global that opens `vars` in the running state, as the
 * runtime's opener will at state open. */
static int test_open_vars(lua_State *L)
{
    sh_script_node_open_vars(L);
    return 0;
}

/* A test-only global: the mod has n Script nodes from now on (0: the recorded ones). */
static int test_many(lua_State *L)
{
    g_many = (int)lua_tointeger(L, 1);
    return 0;
}

/* ============================================ the door, end to end */

static void test_door(void)
{
    char dir[MAX_PATH], err[160];
    sh_script_handle sw1, sw2, door1, door2, other, unnamed, player;
    int mod, i;

    sh_script_source_clear();
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\door-logic\\assets\\mods\\doorlogc\\scripts", g_base);
    CHECK(sh_script_source_set_folder("doorlogc", "Door Logic", dir));
    g_folder_salt = "doorlogc";
    g_nmem = 0;
    g_mem[g_nmem].salt = "othermod"; g_mem[g_nmem].name = "Other Mod"; g_mem[g_nmem].file = "other.lua";
    g_mem[g_nmem].text = "api = 1\non.input(\"open_door\", function() print(\"other mod input\") end)\n"; g_nmem++;

    /* the example's own text gives its mod the node */
    {
        char *text = NULL;
        size_t len = 0;
        CHECK(sh_script_source_files()->read(sh_script_source_files()->context, "doorlogc", "door.lua", &text, &len) == 1);
        CHECK(text && sh_script_node_registers_input(text, len));
        sh_script_source_files()->release(sh_script_source_files()->context, text);
    }

    CHECK(sh_script_node_map_load(k_map, sizeof k_map - 1, err, sizeof err));
    CHECK(sh_script_map_open() == 2);
    mod = sh_script_hooks_mod_find("doorlogc");
    CHECK(mod >= 0);
    g_nnodes = 0;
    sw1 = record_node(201, "mods/doorlogc/script", 61);
    sw2 = record_node(206, "mods/doorlogc/script", 66);       /* "Switch_Used": the same input, another spelling */
    door1 = record_node(202, "mods/doorlogc/script", 62);
    door2 = record_node(203, "mods/doorlogc/script", 63);
    other = record_node(205, "mods/othermod/script", 65);     /* another mod's "open_door" */
    unnamed = record_node(207, "mods/doorlogc/script", 67);
    player = sh_script_handles_ref(300);
    CHECK(sw1 && sw2 && door1 && door2 && other && unnamed && player);
    sh_script_tick(0.0, 0.0);
    CHECK(sh_script_status("doorlogc") == SH_SCRIPT_RUNNING);

    /* two switches: nothing opens */
    g_nfired = 0;
    CHECK(sh_script_node_post_input(mod, sw1, player, 61) == 1);
    CHECK(sh_script_node_post_input(mod, sw2, player, 66) == 1);
    sh_script_tick(0.1, 0.1);
    CHECK(g_nfired == 0);
    /* the third: every "open_door" node of this mod fires once, with the
     * player who used the switch; the other mod's node does not */
    CHECK(sh_script_node_post_input(mod, sw1, player, 61) == 1);
    sh_script_tick(0.2, 0.1);
    CHECK(g_nfired == 2);
    for (i = 0; i < g_nfired; i++) {
        CHECK(g_fired[i].node == door1 || g_fired[i].node == door2);
        CHECK(g_fired[i].activator == player);
    }
    CHECK(g_nfired == 2 && g_fired[0].node != g_fired[1].node);
    CHECK(strstr(log_of("othermod"), "other mod input") == NULL);

    /* an input from a node no handler takes: one line naming what is handled */
    CHECK(sh_script_node_post_input(mod, unnamed, player, 67) == 1);
    CHECK(sh_script_node_post_input(mod, door1, player, 62) == 1);
    CHECK(sh_script_node_post_input(mod, door1, player, 62) == 1);
    sh_script_tick(0.3, 0.1);
    log_of("doorlogc");
    printf("%s", g_out);
    CHECK(strstr(g_out, "Script node \"open_door\" (Door Logic): no script handles it. The script handles: switch_used.") != NULL);
    CHECK(strstr(g_out, "Script node \"\" (Door Logic): no script handles it.") != NULL);
    {
        const char *p = strstr(g_out, "Script node \"open_door\"");
        CHECK(p && !strstr(p + 1, "Script node \"open_door\""));   /* once */
    }
    CHECK(sh_script_status("doorlogc") == SH_SCRIPT_RUNNING);

    /* the other mod's input reaches only its own script */
    CHECK(sh_script_node_post_input(sh_script_hooks_mod_find("othermod"), other, 0, 65) == 1);
    sh_script_tick(0.4, 0.1);
    CHECK(strstr(log_of("othermod"), "other mod input") != NULL);

    sh_script_map_close(1);
    g_folder_salt = NULL;
}

/* ===================================================== node() and vars */

static void test_api(void)
{
    char err[160], stop[4096];
    sh_script_handle n1;
    int mod;
    static const struct { const char *salt, *text, *want; } refusals[] = {
        { "refuse01", "api = 1\ntest_open_vars()\nvars.get(\"nope\")\n",
          "scripts\\main.lua line 3: \"vars.get\": this map has no Integer, Number, Boolean or String variable named \"nope\"" },
        { "refuse02", "api = 1\ntest_open_vars()\nvars.set(\"Score\", 2.5)\n",
          "scripts\\main.lua line 3: \"vars.set\": \"Score\" is an Integer variable, so it needs a whole number, got 2.5" },
        { "refuse03", "api = 1\ntest_open_vars()\nvars.get(\"dup\")\n",
          "\"vars.get\": this map has more than one variable named \"dup\" (a Boolean and a String variable); rename one of them in the editor" },
        { "refuse04", "api = 1\ntest_open_vars()\nvars.set(\"Door Open\", 1)\n",
          "\"Door Open\" is a Boolean variable, so it needs true or false, got number" },
        { "refuse05", "api = 1\ntest_open_vars()\nvars.set(\"speed\", \"fast\")\n",
          "\"speed\" is a Number variable, so it needs a number, got string" },
        { "refuse06", "api = 1\nnode()\n", "\"node\" needs the name of a Script node, got nothing" },
        { "refuse07", "api = 1\nnode(\"x\").fire()\n", "\"fire\" is called on a Script node, as node(\"name\"):fire(); it was called on nothing" },
        { "refuse08", "api = 1\nnode(\"x\"):fire(5)\n", "\"fire\" needs an entity here, got number" },
        { "refuse09", "api = 1\ntest_open_vars()\nvars.get = nil\n", "it cannot be changed" },
        { "refuse10", "api = 1\ntest_open_vars()\nvars.set(\"Score\", 2^40)\n", "is too large for it" },
    };
    int i;

    CHECK(sh_script_node_map_load(k_map, sizeof k_map - 1, err, sizeof err));
    g_nmem = 0;
    g_mem[g_nmem].salt = "varsmod1"; g_mem[g_nmem].name = "Vars Mod"; g_mem[g_nmem].file = "main.lua";
    g_mem[g_nmem].text =
        "api = 1\n"
        "test_open_vars()\n"
        "on.map_start(function()\n"
        "  print('score', vars.get('Score'), math.type(vars.get('Score')))\n"
        "  print('set score', vars.set('score', 150))\n"           /* clamped to 100 */
        "  print('set lives', vars.set('Lives', 3))\n"             /* unchanged: no notification */
        "  print('speed', vars.set('speed', 2.5), vars.get('Gravity'))\n"
        "  print('flag', vars.set('Door Open', true), vars.get('door open'))\n"
        "  print('text', vars.set('label', 'hello'), vars.set('label', 'hello'))\n"
        "  print('long', #vars.set('label', string.rep('w', 50000)), #vars.get('label'))\n"
        "  vars.set('label', 'hello')\n"
        "  print('n', tostring(node('open_door')), node('open_door').name)\n"
        "  print('fired', node('OPEN_DOOR'):fire(), node('open_door'):fire(nil))\n"
        "  print('missing', node('no_such_node'):fire(), node('no_such_node'):fire())\n"
        "  local nm = string.rep('n', 300)\n"
        "  print('longnode', #node(nm).name, tostring(node(nm)) == 'Script node \"' .. nm .. '\"')\n"
        "  on.input(string.rep('i', 300), function() end)\n"
        "  test_many(300) print('many', node('open_door'):fire()) test_many(0)\n"
        "end)\n";
    g_nmem++;
    CHECK(sh_script_map_open() == 1);
    mod = sh_script_hooks_mod_find("varsmod1");
    g_nnodes = 0;
    n1 = record_node(401, "mods/varsmod1/script", 62);   /* uid 62 is named "open_door" in the map */
    CHECK(n1 != 0 && mod >= 0);
    memset(g_notified, 0, sizeof g_notified);
    g_nfired = 0;
    sh_script_tick(0.0, 0.0);
    log_of("varsmod1");
    printf("%s", g_out);
    CHECK(strstr(g_out, "score\t10\tinteger\n") != NULL);
    CHECK(strstr(g_out, "set score\t100\n") != NULL && g_int[1] == 100);
    CHECK(strstr(g_out, "set lives\t3\n") != NULL);
    CHECK(g_notified[SH_SCRIPT_VAR_INTEGER] == 1);
    CHECK(strstr(g_out, "speed\t2.5\t9.8") != NULL && g_notified[SH_SCRIPT_VAR_NUMBER] == 1);
    CHECK(strstr(g_out, "flag\ttrue\ttrue\n") != NULL && g_bool[0] == 1 && g_notified[SH_SCRIPT_VAR_BOOLEAN] == 1);
    CHECK(strstr(g_out, "text\thello\thello\n") != NULL && g_notified[SH_SCRIPT_VAR_STRING] == 3);
    CHECK(strstr(g_out, "long\t50000\t50000\n") != NULL);   /* a String variable's text of any length */
    CHECK(strstr(g_out, "n\tScript node \"open_door\"\topen_door\n") != NULL);
    CHECK(strstr(g_out, "fired\t1\t1\n") != NULL && g_nfired == 2 && g_fired[0].activator == 0);
    CHECK(strstr(g_out, "missing\t0\t0\n") != NULL);
    CHECK(strstr(g_out, "many\t300\n") != NULL);   /* every node of the mod fires: no count is the limit */
    CHECK(strstr(g_out, "longnode\t300\ttrue\n") != NULL);   /* a node name of any length (the old cap was 127) */
    CHECK(strstr(g_out, "Warning: scripts\\main.lua line 14: node(\"no_such_node\"):fire() found no Script node of Vars Mod "
                        "named \"no_such_node\" in this map, so nothing fired. Place a Script node from Vars Mod and name it "
                        "\"no_such_node\".\n") != NULL);
    {
        const char *p = strstr(g_out, "found no Script node");
        CHECK(p && !strstr(p + 1, "found no Script node"));   /* once per name per map */
    }
    CHECK(sh_script_status("varsmod1") == SH_SCRIPT_RUNNING);
    sh_script_map_close(1);

    /* the map's logic not running: nil and one line, the script goes on */
    g_vars_ready = 0;
    g_nmem = 0;
    g_mem[0].salt = "varsoff1"; g_mem[0].name = "Vars Off"; g_mem[0].file = "main.lua";
    g_mem[0].text = "api = 1\ntest_open_vars()\nprint('off', vars.get('Score'), vars.set('Score', 1), vars.get('Score'))\n";
    g_nmem = 1;
    CHECK(sh_script_map_open() == 1);
    sh_script_tick(0.0, 0.0);
    log_of("varsoff1");
    CHECK(strstr(g_out, "off\tnil\tnil\tnil\n") != NULL);
    CHECK(strstr(g_out, "the map's variable \"Score\" could not be read here, so vars left it alone.") != NULL);
    CHECK(sh_script_status("varsoff1") == SH_SCRIPT_RUNNING);
    sh_script_map_close(1);
    g_vars_ready = 1;

    /* every refusal stops that mod with its plain line */
    for (i = 0; i < (int)(sizeof refusals / sizeof refusals[0]); i++) {
        g_mem[0].salt = refusals[i].salt; g_mem[0].name = "Refusing Mod"; g_mem[0].file = "main.lua";
        g_mem[0].text = refusals[i].text;
        g_nmem = 1;
        CHECK(sh_script_map_open() == 1);
        sh_script_tick(0.0, 0.0);
        CHECK(sh_script_status(refusals[i].salt) == SH_SCRIPT_STOPPED);
        stop[0] = 0;
        HOS(stop, sh_script_stop_line_dup(refusals[i].salt));
        if (!strstr(stop, refusals[i].want)) printf("  refusal %d: %s\n", i, stop);
        CHECK(strstr(stop, refusals[i].want) != NULL);
        CHECK(strncmp(stop, "Script stopped: Refusing Mod, scripts\\main.lua line ", 52) == 0);
        sh_script_map_close(1);
    }
    g_nmem = 0;
}

/* ======================================================== the images */

static uint8_t *map_pe_by_rva(const char *path, uint32_t *image_size)
{
    FILE *f = fopen(path, "rb");
    long fsz;
    uint8_t *file, *img;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int i;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    *image_size = nt->OptionalHeader.SizeOfImage;
    img = (uint8_t *)calloc(1, nt->OptionalHeader.SizeOfImage);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz) memcpy(img + va, file + po, rs);
    }
    free(file);
    return img;
}

/* The target of the first E8 call in [from, from + span) that calls `callee`, counted. */
static int calls_to(const uint8_t *base, uint32_t from, uint32_t span, uint32_t callee)
{
    uint32_t i;
    int n = 0;
    for (i = from; i < from + span; i++) {
        int32_t rel;
        if (base[i] != 0xE8) continue;
        memcpy(&rel, base + i + 1, 4);
        if ((uint32_t)(i + 5 + rel) == callee) n++;
    }
    return n;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, rva[SH_SCRIPT_NODE_SIG_COUNT] = { 0 }, relay = 0;
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    uint64_t image_base;
    sig_result r;
    int vk, i;

    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    vk = identity->vulkan;
    image_base = ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.ImageBase;
    printf("image %s: %s\n", path, identity->label);

    for (i = 0; SH_SCRIPT_NODE_SIGNATURES[i].name; i++) {}
    CHECK(i == SH_SCRIPT_NODE_SIG_COUNT);
    for (i = 0; i < SH_SCRIPT_NODE_SIG_COUNT; i++) {
        const sig_entry *e = &SH_SCRIPT_NODE_SIGNATURES[i];
        if (sig_resolve_one(base, e, &r) != SIG_OK || !r.addr) {
            printf("FAIL %s did not resolve to a unique match\n", e->name);
            g_fail = 1;
            continue;
        }
        CHECK(r.rva == (vk ? e->rva_vk : e->rva_gl));
        rva[i] = r.rva;
        printf("  %-22s rva=0x%X\n", e->name, r.rva);
    }

    /* The Relay: the window the installer compares, whole and position independent;
     * its body calls FireOutput with SNAP_EVENT_LOGIC_FIRED. */
    if (sig_resolve_one(base, &SH_SCRIPT_SIGNATURES[SH_SCRIPT_SIG_RELAY_DOACTION], &r) == SIG_OK) {
        unsigned flags = 0;
        relay = r.rva;
        CHECK(memcmp(base + relay, SH_SCRIPT_NODE_RELAY_PROLOGUE, SH_SCRIPT_RELAY_STOLEN) == 0);
        CHECK(sh_x64_stolen_boundary(base + relay, 64, 14, 48, &flags) == SH_SCRIPT_RELAY_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        if (rva[SH_SCRIPT_NODE_SIG_FIRE_OUTPUT]) CHECK(calls_to(base, relay, 0x60, rva[SH_SCRIPT_NODE_SIG_FIRE_OUTPUT]) == 1);
        {
            int found = 0;
            uint32_t k;
            for (k = relay; k < relay + 0x60; k++) if (!memcmp(base + k, "\xBA\x08\x00\x00\x00", 5)) found = 1;
            CHECK(found);
        }
    } else CHECK(!"Relay DoAction resolves");

    /* idGameLocal vtable +0x720: lea rax,[rcx+0x49C510]; ret. */
    {
        uint32_t game_vt = vk ? 0x201E3D8u : 0x20381A8u;
        uint64_t fn;
        memcpy(&fn, base + game_vt + 0x720, 8);
        CHECK(fn > image_base && fn - image_base < size);
        if (fn > image_base && fn - image_base < size)
            CHECK(!memcmp(base + (fn - image_base), "\x48\x8D\x81\x10\xC5\x49\x00\xC3", 8));
    }

    /* The stock Integer Operation and Boolean Set notify through the signed function. */
    if (rva[SH_SCRIPT_NODE_SIG_VAR_CHANGED]) {
        CHECK(calls_to(base, vk ? 0x9DC790u : 0x9DBA80u, 0x300, rva[SH_SCRIPT_NODE_SIG_VAR_CHANGED]) == 1);
        CHECK(calls_to(base, vk ? 0x9D92A0u : 0x9D8590u, 0x200, rva[SH_SCRIPT_NODE_SIG_VAR_CHANGED]) == 1);
    }

    /* The game name of a placed snap entity is built with "%u_%s/%s_%u". */
    {
        static const char fmt[] = "%u_%s/%s_%u";
        uint32_t at = vk ? 0x235C2D8u : 0x2376028u;
        CHECK(at + sizeof fmt <= size && !memcmp(base + at, fmt, sizeof fmt));
    }
    free(base);
}

int main(int argc, char **argv)
{
    int i, images = 0;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    strcpy_s(g_base, sizeof g_base, argc > 1 ? argv[1] : "fixtures\\scripts");
    sh_script_handles_set_resolver(resolve);
    sh_script_set_map_provider(provider, NULL);
    sh_script_node_set_ops(&k_fake);
    CHECK(sh_script_node_api_register());
    CHECK(sh_script_api_add(SH_SCRIPT_API_GLOBAL, "test_open_vars", test_open_vars));
    CHECK(sh_script_api_add(SH_SCRIPT_API_GLOBAL, "test_many", test_many));

    test_registers_input();
    test_row();
    test_map_names();
    test_door();
    test_api();

    for (i = 2; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
        images++;
    }
    if (!images) printf("script_node_test: no images given; the signature checks were skipped\n");
    printf(g_fail ? "script_node_test: FAIL\n" : "script_node_test: PASS\n");
    return g_fail;
}
