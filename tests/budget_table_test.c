/* The editor budget table: block arithmetic, the module filter and the JSON
 * the Prefabs tab reads. No game needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "budget_table.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int block_eq(const int *a, const int *b)
{
    return !memcmp(a, b, sizeof(int) * SH_BT_FIELDS);
}

static void test_arithmetic(void)
{
    int b[SH_BT_FIELDS], own[SH_BT_FIELDS];
    static const int base[SH_BT_FIELDS] = {0,0,0,0,0,0,0,0,0,0,50,0};
    static const int imp[SH_BT_FIELDS] = {1,1,2,0,1,0,0,0,0,0,1500,1};
    static const int c90_own[SH_BT_FIELDS] = {0,0,33,0,0,0,0,0,0,0,0,0};
    sh_bt_base(b);
    CHECK(block_eq(b, base));
    sh_bt_add(b, imp);
    CHECK(b[10] == 1550 && b[11] == 1 && b[2] == 2);
    /* c_ind_90: 1 floor, 8 walls, 2 portals -> 1 + 24 + 2 * 4 = 33 render models. */
    sh_bt_module_own(1, 8, 2, 2926, 808, 50, own);
    CHECK(own[2] == 33 && own[8] == 2926 && own[9] == 808 && own[10] == 50);
    own[8] = own[9] = own[10] = 0;
    CHECK(block_eq(own, c90_own));
    sh_bt_base(b);
    sh_bt_add_miss(b);
    CHECK(b[1] == 1 && b[2] == 1 && b[4] == 1 && b[10] == 50 && b[0] == 0);
    /* 1 | (decl & sound) | autoView; c_ind_90's decl mask carries the sound bit. */
    CHECK(sh_bt_filter_mask(0xc0000fu, 0x400000u, 0x20000u) == 0x420001u);
    CHECK(sh_bt_filter_mask(0x1u, 0x400000u, 0x20000u) == 0x20001u);
}

static void test_leaf(void)
{
    char out[64];
    sh_bt_leaf("maps/modules/ind_dlc/ind_totally_blank_room_4x.decl", out, sizeof out);
    CHECK(!strcmp(out, "ind_totally_blank_room_4x"));
    sh_bt_leaf("plain", out, sizeof out);
    CHECK(!strcmp(out, "plain"));
    sh_bt_leaf("a/b.declx", out, sizeof out);
    CHECK(!strcmp(out, "b.declx"));
    sh_bt_leaf("maps/modules/x/very_long_name.decl", out, 5);
    CHECK(!strcmp(out, "very"));
    sh_bt_leaf(NULL, out, sizeof out);
    CHECK(out[0] == 0);
}

static void test_table(void)
{
    sh_bt_table t;
    sh_bt_head head;
    static const int limits[SH_BT_FIELDS] = {768,14284,10384,2038,672,1000,8192,8192,0,0,500000,64};
    static const int usage[SH_BT_FIELDS] = {32,67,91,1,30,0,1,0,1552,12,2545,0};
    static const int crate[SH_BT_FIELDS] = {1,1,1,0,1,0,0,0,0,0,25,0};
    static const int box[SH_BT_FIELDS] = {0,1,1,0,1,0,0,0,0,0,10,0};
    static const int unk[SH_BT_FIELDS] = {0,1,2,0,1,0,0,0,0,0,50,0};
    int own[SH_BT_FIELDS], templ[SH_BT_FIELDS], view[SH_BT_FIELDS];
    static const int game[SH_BT_FIELDS] = {0,1,0,0,0,0,0,0,0,0,50,0};
    static const int probe[SH_BT_FIELDS] = {0,1,2,0,0,0,0,0,0,0,50,0};
    static sh_bt_parts parts;
    char *json;
    int i;
    sh_bt_table_init(&t);
    /* The return value names the row that owns its inherit; the palette render-model lookup keys on it. */
    CHECK(sh_bt_table_add_sedef(&t, "unknown/unknown", "snapmaps/unknown", unk, 0, 0) == 1);
    CHECK(sh_bt_table_add_sedef(&t, "custom/timeline", "snapmaps/unknown", unk, 1, 0) == 0);   /* inherit kept by the first row */
    sh_bt_table_add_sedef(&t, "prop/new/crate01", "snapmaps/props/new/crate01", crate, 0, 0);
    sh_bt_table_add_sedef(&t, "volume/blocking", "snapmaps/volume/blocking", box, 0, 1);
    CHECK(sh_bt_table_add_sedef(&t, "volume/blocking", "other", box, 0, 1) == 0);              /* duplicate row name */
    sh_bt_table_add_sedef(&t, "odd\"name", NULL, box, 0, 0);
    /* Many rows exercise the key set's growth. */
    for (i = 0; i < 3000; ++i) {
        char n[32], inh[32];
        sprintf(n, "gen/row%d", i); sprintf(inh, "snapmaps/gen/%d", i % 1500);
        sh_bt_table_add_sedef(&t, n, inh, crate, 0, 0);
    }
    CHECK(t.nsedef == 5 + 3000);
    CHECK(t.ninherit == 3 + 1500);
    sh_bt_module_own(1, 8, 2, 2926, 808, 50, own);
    sh_bt_base(templ); templ[1] = 2; templ[10] = 300;
    memset(view, 0, sizeof view); view[1] = 3; view[2] = 6; view[3] = 1; view[4] = 1; view[10] = 150;
    sh_bt_parts_add(&parts, 0x1u, SH_BT_PART_GAME, game);
    sh_bt_parts_add(&parts, 0x1u, SH_BT_PART_GAME, game);
    sh_bt_parts_add(&parts, 0x20000u, SH_BT_PART_VIEW, probe);
    sh_bt_table_add_module(&t, "maps/modules/ind_connections/c_ind_90.decl", own, templ, view, 0x420001u, &parts);
    sh_bt_table_add_module(&t, "maps/modules/ind_connections/c_ind_90.decl", own, templ, view, 0x420001u, &parts);
    sh_bt_table_add_module(&t, "maps/modules/other/c_ind_90.decl", own, templ, view, 1u, NULL);  /* leaf already aliased */
    CHECK(t.nmodule == 2 && t.nalias == 1);
    CHECK(strstr(t.modules.p, "\"maps/modules/other/c_ind_90.decl\":{\"own\"") != NULL);
    CHECK(strstr(strstr(t.modules.p, "other/c_ind_90"), "parts") == NULL);   /* NULL parts are not emitted */
    memset(&head, 0, sizeof head);
    head.generation = 7; head.limits = limits; head.usage = usage; head.reached = 0;
    head.auto_view_mask = 0x20000; head.sound_mask = 0x400000; head.layers_source = "decl";
    json = sh_bt_table_finish(&t, &head);
    CHECK(json != NULL);
    if (json) {
        CHECK(!strncmp(json, "{\"gen\":7,\"limits\":[768,14284,10384,2038,672,1000,8192,8192,0,0,500000,64],"
                             "\"usage\":[32,67,91,1,30,0,1,0,1552,12,2545,0],\"reached\":0,"
                             "\"layers\":{\"autoView\":131072,\"sound\":4194304,\"source\":\"decl\"},\"sedefs\":{", 0));
        CHECK(strstr(json, "\"volume/blocking\":{\"l\":[0,1,1,0,1,0,0,0,0,0,10,0],\"auto\":0,\"ns\":1}") != NULL);
        CHECK(strstr(json, "\"custom/timeline\":{\"l\":[0,1,2,0,1,0,0,0,0,0,50,0],\"auto\":1,\"ns\":0}") != NULL);
        CHECK(strstr(json, "\"odd\\\"name\":") != NULL);
        CHECK(strstr(json, "\"inherits\":{\"snapmaps/unknown\":\"unknown/unknown\",") != NULL);
        CHECK(strstr(json, "\"other\":") == NULL);
        CHECK(strstr(json, "\"maps/modules/ind_connections/c_ind_90.decl\":{\"own\":[0,0,33,0,0,0,0,0,2926,808,50,0],"
                           "\"templ\":[0,2,0,0,0,0,0,0,0,0,300,0],\"view\":[0,3,6,1,1,0,0,0,0,0,150,0],\"mask\":4325377,"
                           "\"parts\":[[1,0,[0,2,0,0,0,0,0,0,0,0,100,0]],[131072,1,[0,1,2,0,0,0,0,0,0,0,50,0]]]}") != NULL);
        CHECK(strstr(json, "\"aliases\":{\"c_ind_90\":\"maps/modules/ind_connections/c_ind_90.decl\"}}") != NULL);
        CHECK(json[strlen(json) - 1] == '}');
        free(json);
    }
    sh_bt_table_free(&t);
    /* An empty table is still a complete object. */
    sh_bt_table_init(&t);
    memset(&head, 0, sizeof head);
    json = sh_bt_table_finish(&t, &head);
    CHECK(json && strstr(json, "\"source\":\"fallback\"") && strstr(json, "\"sedefs\":{},\"inherits\":{},\"modules\":{},\"aliases\":{}}"));
    free(json);
    sh_bt_table_free(&t);
}

static void test_parts(void)
{
    static sh_bt_parts ps;
    static const int one[SH_BT_FIELDS] = {0,1,1,0,1,0,0,0,0,0,0,0};
    int i;
    memset(&ps, 0, sizeof ps);
    for (i = 0; i < SH_BT_MAX_PARTS; ++i) sh_bt_parts_add(&ps, 1u << (i % 32), i / 32, one);
    CHECK(ps.count == SH_BT_MAX_PARTS && !ps.overflow);
    sh_bt_parts_add(&ps, 1u, 0, one);                   /* merges into an existing group */
    CHECK(ps.count == SH_BT_MAX_PARTS && !ps.overflow && ps.part[0].block[1] == 2);
    sh_bt_parts_add(&ps, 3u, 2, one);                   /* one group too many */
    CHECK(ps.overflow);
}

static void test_usage_and_error(void)
{
    static const int limits[SH_BT_FIELDS] = {768,14284,10384,2038,672,1000,8192,8192,0,0,500000,64};
    static const int usage[SH_BT_FIELDS] = {32,67,91,1,30,0,1,0,1552,12,2545,0};
    char buf[512], tiny[8];
    int n = sh_bt_usage_json(NULL, 0, limits, usage, 1, 3);
    CHECK(n > 0);
    CHECK(sh_bt_usage_json(tiny, (int)sizeof tiny, limits, usage, 1, 3) == -1);
    CHECK(sh_bt_usage_json(buf, (int)sizeof buf, limits, usage, 1, 3) == n);
    CHECK(!strcmp(buf, "{\"limits\":[768,14284,10384,2038,672,1000,8192,8192,0,0,500000,64],"
                       "\"usage\":[32,67,91,1,30,0,1,0,1552,12,2545,0],\"reached\":1,\"gen\":3}"));
    {
        char *e = sh_bt_error_json("module palette unreadable", 2);
        CHECK(e && !strcmp(e, "{\"gen\":2,\"error\":\"module palette unreadable\"}"));
        free(e);
    }
}

int main(void)
{
    test_arithmetic();
    test_leaf();
    test_table();
    test_parts();
    test_usage_and_error();
    if (g_fail) return 1;
    puts("budget_table_test: OK");
    return 0;
}
