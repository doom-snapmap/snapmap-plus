/* modscene_decl_test.c -- a mod's "scenes" block (modscene_decl.c, modscene_sigs.c).
 *
 * Offline: the grammar, its defaults and refusals, the namespace and
 * first-bundle rules, the yaw-only pose algebra and where a scene plays (at the
 * node; at the player keeping their feet, with and without the turn toward the
 * anchor), and the queue of spawns that wait for a node's sync to start. With
 * build 13954591 executables as arguments: the two spawn signatures and the
 * sync-start detour's resolve uniquely at their recorded RVAs (its stolen
 * prologue as modscene_place.c counts it), the game's vtable slots +0x448 and
 * +0x458 hold exactly those functions, and the argument-less PostEventMS stub
 * the spawn posts "show" with is where the backend calls it. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "modscene_decl.h"
#include "signatures.h"
#include "host_image.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int g_lines;
static void count_line(void *context, const char *line) { (void)context; (void)line; g_lines++; }

static int add(sh_scene_table *t, const char *id, const char *json)
{
    return sh_scene_add_bundle(t, id, json, strlen(json), count_line, NULL);
}

static int approx(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static float yaw_diff(float a, float b)
{
    float d = fmodf(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return fabsf(d);
}

static void test_grammar(void)
{
    sh_scene_table t = {0};
    const sh_scene_entry *e;
    static const char ok[] =
        "{\"name\":\"x\",\"scenes\":{"
        "\"mods/alex.scenes/imp_arrival\":{\"anchor\":[563.09,16.7,0,180],\"player\":[-33.68,0,0,0],"
        "\"spawn\":[{\"entityDef\":\"mods/alex.scenes/entrance/imp\"}]},"
        "\"mods/alex.scenes/slam\":{\"plays\":\"at_node\",\"anchor\":[-1530.1,-524.7,0,0],"
        "\"spawn\":[{\"entityDef\":\"ai/demon/imp_coop\",\"at\":[1,2,3,90]},{\"entityDef\":\"func/x\"}],\"return\":true},"
        "\"mods/alex.scenes/plain\":{\"reaim\":false}}}";
    CHECK(add(&t, "alex.scenes", ok) == 3);
    e = sh_scene_find(&t, "MODS\\alex.scenes\\IMP_ARRIVAL");
    CHECK(e != NULL);
    if (e) {
        CHECK(!e->at_node && e->has_player && e->reaim && !e->return_player);
        CHECK(e->spawn_count == 1 && !strcmp(e->spawns[0].entitydef, "mods/alex.scenes/entrance/imp"));
        CHECK(approx(e->spawns[0].at.x, 563.09f, 1e-3f) && approx(e->spawns[0].at.yaw, 180.0f, 1e-4f));
        CHECK(approx(e->player.x, -33.68f, 1e-4f) && !strcmp(e->bundle, "alex.scenes"));
    }
    e = sh_scene_find(&t, "mods/alex.scenes/slam");
    CHECK(e && e->at_node && !e->has_player && !e->reaim && e->return_player && e->spawn_count == 2);
    if (e) {
        CHECK(approx(e->spawns[0].at.z, 3.0f, 0) && approx(e->spawns[0].at.yaw, 90.0f, 0));
        CHECK(approx(e->spawns[1].at.x, -1530.1f, 1e-3f));           /* "at" defaults to the anchor */
    }
    e = sh_scene_find(&t, "mods/alex.scenes/plain");
    CHECK(e && !e->at_node && !e->has_player && !e->reaim && e->return_player && !e->spawn_count);
    CHECK(e && e->anchor.x == 0 && e->anchor.yaw == 0);
    CHECK(add(&t, "other", "{\"strings\":{}}") == 0);
    CHECK(sh_scene_find(&t, "mods/alex.scenes/nothing") == NULL);
    sh_scene_table_free(&t);
}

static void test_refusals(void)
{
    static const char *bad[] = {
        "{\"scenes\":[]}",                                                                    /* not an object */
        "{\"scenes\":{\"mods/other.mod/n\":{}}}",                                             /* foreign owner */
        "{\"scenes\":{\"maps/x/n\":{}}}",                                                     /* stock owner */
        "{\"scenes\":{\"mods/a.b/n\":[]}}",                                                   /* entry not an object */
        "{\"scenes\":{\"mods/a.b/n\":{\"plays\":\"elsewhere\"}}}",
        "{\"scenes\":{\"mods/a.b/n\":{\"anchr\":[0,0,0,0]}}}",                                /* typo */
        "{\"scenes\":{\"mods/a.b/n\":{\"anchor\":[0,0,0]}}}",
        "{\"scenes\":{\"mods/a.b/n\":{\"anchor\":[1e9,0,0,0]}}}",
        "{\"scenes\":{\"mods/a.b/n\":{\"plays\":\"at_node\",\"player\":[0,0,0,0]}}}",         /* no player at the node */
        "{\"scenes\":{\"mods/a.b/n\":{\"reaim\":true}}}",                                     /* reaim without player */
        "{\"scenes\":{\"mods/a.b/n\":{\"reaim\":1,\"player\":[0,0,0,0]}}}",
        "{\"scenes\":{\"mods/a.b/n\":{\"return\":\"no\"}}}",
        "{\"scenes\":{\"mods/a.b/n\":{\"spawn\":[]}}}",
        "{\"scenes\":{\"mods/a.b/n\":{\"spawn\":[{\"at\":[0,0,0,0]}]}}}",                     /* no entityDef */
        "{\"scenes\":{\"mods/a.b/n\":{\"spawn\":[{\"entityDef\":\"a b\"}]}}}",                /* space */
        "{\"scenes\":{\"mods/a.b/n\":{\"spawn\":[{\"entityDef\":\"x\",\"origin\":[0,0,0]}]}}}",
        "{\"scenes\":{\"mods/a.b/n\":{\"spawn\":[{\"entityDef\":\"x\"},{\"entityDef\":\"x\"},{\"entityDef\":\"x\"},"
        "{\"entityDef\":\"x\"},{\"entityDef\":\"x\"}]}}}",                                    /* five */
        "{\"scenes\":{\"mods/a.b/n\":{\"spawn\":[{\"entityDef\":\"mods/a.b/N\"}]}}}",         /* itself */
        "{\"scenes\":{\"mods/a.b/n\":{},\"mods/a.b/N\":{}}}",                                  /* twice */
    };
    size_t i;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        sh_scene_table t = {0};
        int before = g_lines;
        int r = add(&t, "a.b", bad[i]);
        if (r != -1 || t.count != 0 || g_lines != before + 1) printf("case %u\n", (unsigned)i);
        CHECK(r == -1 && t.count == 0 && g_lines == before + 1);
        sh_scene_table_free(&t);
    }
}

static void test_namespace_and_order(void)
{
    sh_scene_table t = {0};
    const sh_scene_entry *e;
    CHECK(add(&t, "campaign-interactables-2", "{\"scenes\":{\"mods/campaign_interactables-2/scene_x\":{\"plays\":\"at_node\"}}}") == 1);
    CHECK(add(&t, "late", "{\"scenes\":{\"mods/late/scene_y\":{}}}") == 1);
    CHECK(add(&t, "campaign-interactables-2", "{\"scenes\":{\"MODS/campaign_interactables-2/SCENE_X\":{}}}") == 0);
    CHECK(t.refused == 1);
    e = sh_scene_find(&t, "mods/campaign_interactables-2/scene_x");
    CHECK(e && e->at_node);
    CHECK(sh_scene_owner_namespace("Mods/x") && !sh_scene_owner_namespace("ai/demon/imp"));
    sh_scene_table_free(&t);
}

static void test_math(void)
{
    sh_scene_table t = {0};
    const sh_scene_entry *imp, *slam, *plain;
    sh_scene_pose a = { 10.0f, -5.0f, 2.0f, 30.0f }, b = { 3.0f, 4.0f, 1.0f, 70.0f }, r, inv;
    sh_scene_pose node = { -1248.0f, 1856.0f, 0.0f, 90.0f }, player = { 300.0f, 200.0f, 1.4f, -120.0f };
    sh_scene_pose frame, first, anchor_seen, spawn;
    int reaimed;

    /* a * b * inverse(b) == a, inverse(a) * a == identity */
    r = sh_scene_compose(sh_scene_compose(a, b), sh_scene_inverse(b));
    CHECK(approx(r.x, a.x, 1e-3f) && approx(r.y, a.y, 1e-3f) && approx(r.z, a.z, 1e-4f) && yaw_diff(r.yaw, a.yaw) < 1e-3f);
    inv = sh_scene_compose(sh_scene_inverse(a), a);
    CHECK(approx(inv.x, 0, 1e-3f) && approx(inv.y, 0, 1e-3f) && approx(inv.z, 0, 1e-5f) && yaw_diff(inv.yaw, 0) < 1e-3f);
    {
        sh_scene_pose turn = { 0, 0, 0, 90.0f }, unit = { 1.0f, 0, 0, 0 };
        r = sh_scene_compose(turn, unit);
    }
    CHECK(approx(r.x, 0, 1e-5f) && approx(r.y, 1.0f, 1e-5f));

    CHECK(add(&t, "m",
              "{\"scenes\":{\"mods/m/imp\":{\"anchor\":[563.09,16.7,0,180],\"player\":[-33.68,0,0,0],"
              "\"spawn\":[{\"entityDef\":\"mods/m/entrance/imp\"}]},"
              "\"mods/m/slam\":{\"plays\":\"at_node\",\"anchor\":[-1530.09,-524.68,0,0]},"
              "\"mods/m/plain\":{}}}") == 3);
    imp = sh_scene_find(&t, "mods/m/imp");
    slam = sh_scene_find(&t, "mods/m/slam");
    plain = sh_scene_find(&t, "mods/m/plain");
    CHECK(imp && slam && plain);
    if (!imp || !slam || !plain) { sh_scene_table_free(&t); return; }

    /* No entry: the player's feet and view yaw (R313). */
    frame = sh_scene_play_frame(NULL, node, player, &reaimed);
    CHECK(!reaimed && approx(frame.x, 300.0f, 0) && approx(frame.yaw, -120.0f, 0));
    frame = sh_scene_play_frame(plain, node, player, &reaimed);
    CHECK(!reaimed && approx(frame.y, 200.0f, 0) && approx(frame.yaw, -120.0f, 0));

    /* The spawn lands exactly on the node: the node marks the anchor, at = anchor. */
    spawn = sh_scene_compose(sh_scene_node_frame(imp, node), imp->spawns[0].at);
    CHECK(approx(spawn.x, node.x, 1e-2f) && approx(spawn.y, node.y, 1e-2f) && approx(spawn.z, 0, 1e-4f));
    CHECK(yaw_diff(spawn.yaw, node.yaw) < 1e-3f);

    /* At the player with the turn: the first player pose is on the player's
     * feet, and from it the anchor lies where the node stands. */
    frame = sh_scene_play_frame(imp, node, player, &reaimed);
    CHECK(reaimed);
    first = sh_scene_compose(frame, imp->player);
    CHECK(approx(first.x, player.x, 1e-2f) && approx(first.y, player.y, 1e-2f) && approx(first.z, player.z, 1e-4f));
    anchor_seen = sh_scene_compose(frame, imp->anchor);
    {
        double want = atan2((double)node.y - player.y, (double)node.x - player.x);
        double got = atan2((double)anchor_seen.y - first.y, (double)anchor_seen.x - first.x);
        CHECK(fabs(want - got) < 1e-4);
    }
    /* Without the turn the first pose faces the view yaw. */
    {
        sh_scene_entry still = *imp;
        still.reaim = 0;
        frame = sh_scene_play_frame(&still, node, player, &reaimed);
        first = sh_scene_compose(frame, still.player);
        CHECK(!reaimed && approx(first.x, player.x, 1e-2f) && yaw_diff(first.yaw, player.yaw) < 1e-3f);
    }
    /* A player standing on the node: no direction to turn to. */
    {
        sh_scene_pose on = node;
        on.z = 0;
        on.yaw = 15.0f;
        frame = sh_scene_play_frame(imp, node, on, &reaimed);
        CHECK(!reaimed && yaw_diff(sh_scene_compose(frame, imp->player).yaw, 15.0f) < 1e-3f);
    }

    /* At the node: the scene frame puts the anchor pose on the node; the player stays. */
    frame = sh_scene_play_frame(slam, node, player, &reaimed);
    anchor_seen = sh_scene_compose(frame, slam->anchor);
    CHECK(!reaimed && approx(anchor_seen.x, node.x, 1e-2f) && approx(anchor_seen.y, node.y, 1e-2f));
    CHECK(yaw_diff(anchor_seen.yaw, node.yaw) < 1e-3f);
    sh_scene_table_free(&t);
}

/* ---- images ---------------------------------------------------------- */

static uint8_t *map_pe_by_rva(const char *path)
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
    img = (uint8_t *)calloc(1, nt->OptionalHeader.SizeOfImage);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].SizeOfRawData && sec[i].VirtualAddress + sec[i].SizeOfRawData <= nt->OptionalHeader.SizeOfImage &&
            sec[i].PointerToRawData + sec[i].SizeOfRawData <= (DWORD)fsz)
            memcpy(img + sec[i].VirtualAddress, file + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    free(file);
    return img;
}

static void test_image(const char *path)
{
    /* idGameLocal's vtable (RTTI .?AVidGameLocal@@, snaphak-re R301) and the
     * argument-less PostEventMS stub with its target, per renderer. */
    static const uint32_t vtable[2] = { 0x201E3D8u, 0x20381A8u };
    static const uint32_t post0[2] = { 0xB45770u, 0xB44D60u }, post0_target[2] = { 0xB45700u, 0xB44CF0u };
    static const uint8_t stub[4] = { 0x45, 0x33, 0xC9, 0xE9 };
    wchar_t wide[MAX_PATH];
    const sh_build_identity *id;
    uint8_t *base = map_pe_by_rva(path);
    sig_result r[4];
    static const uint8_t syncstart_head[17] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56,
                                                0x41, 0x57, 0x48, 0x83, 0xEC, 0x50 };
    int i, n = 0, bad = 0, gl;
    CHECK(base != NULL);
    if (!base) return;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    id = sh_host_test_bind_image(base, wide);
    CHECK(id != NULL);
    printf("image %s: %s\n", path, id ? id->label : "unsupported");
    if (!id) { free(base); return; }
    gl = !id->vulkan;
    for (i = 0; SH_MODSCENE_SPAWN_SIGNATURES[i].name; i++, n++) {
        uint32_t known = gl ? SH_MODSCENE_SPAWN_SIGNATURES[i].rva_gl : SH_MODSCENE_SPAWN_SIGNATURES[i].rva_vk;
        sig_status st = sig_resolve_one(base, &SH_MODSCENE_SPAWN_SIGNATURES[i], &r[i]);
        int good = st == SIG_OK && r[i].rva == known;
        printf("%s %-20s resolved=0x%-8x known=0x%x\n", good ? "OK " : "BAD", SH_MODSCENE_SPAWN_SIGNATURES[i].name,
               r[i].rva, known);
        CHECK(good);
        if (!good) bad++;
    }
    CHECK(n == 3);
    if (!bad && n == 3) {
        uint64_t create, finish;
        int32_t rel;
        memcpy(&create, base + vtable[gl] + 0x448, 8);
        memcpy(&finish, base + vtable[gl] + 0x458, 8);
        CHECK(create - 0x140000000ull == r[0].rva);
        CHECK(finish - 0x140000000ull == r[1].rva);
        CHECK(!memcmp(base + post0[gl], stub, sizeof stub));
        memcpy(&rel, base + post0[gl] + 4, 4);
        CHECK((uint32_t)(post0[gl] + 8 + rel) == post0_target[gl]);
        /* The sync-start detour steals exactly this position-independent prologue. */
        CHECK(!memcmp(base + r[2].rva, syncstart_head, sizeof syncstart_head));
    }
    free(base);
}

static void test_pending(void)
{
    sh_scene_pending_table t;
    sh_scene_pending got;
    sh_scene_spawn s[2];
    int a, b, i, nodes[SH_SCENE_PENDING_ROWS + 1];
    memset(s, 0, sizeof s);
    strcpy_s(s[0].entitydef, sizeof s[0].entitydef, "mods/m/entrance/imp");
    s[0].at.x = 10.0f;
    s[0].at.yaw = 90.0f;
    strcpy_s(s[1].entitydef, sizeof s[1].entitydef, "mods/m/extra");
    sh_scene_pending_clear(&t);
    CHECK(!sh_scene_pending_set(&t, &a, "n", s, 0) && !sh_scene_pending_set(&t, &a, "n", s, SH_SCENE_MAX_SPAWNS + 1));
    CHECK(!sh_scene_pending_set(&t, NULL, "n", s, 1) && t.count == 0);
    CHECK(sh_scene_pending_set(&t, &a, "mods/m/imp", s, 1) && t.count == 1);
    CHECK(sh_scene_pending_set(&t, &a, "mods/m/imp", s, 2) && t.count == 1);      /* a later Activate replaces */
    CHECK(!sh_scene_pending_take(&t, &b, &got));
    CHECK(sh_scene_pending_take(&t, &a, &got) && t.count == 0);
    CHECK(got.count == 2 && !strcmp(got.node_name, "mods/m/imp") && !strcmp(got.spawns[1].entitydef, "mods/m/extra"));
    CHECK(approx(got.spawns[0].at.x, 10.0f, 0) && approx(got.spawns[0].at.yaw, 90.0f, 0));
    CHECK(!sh_scene_pending_take(&t, &a, &got));                                  /* taken once */
    for (i = 0; i <= SH_SCENE_PENDING_ROWS; i++) CHECK(sh_scene_pending_set(&t, &nodes[i], "n", s, 1));
    CHECK(t.count == SH_SCENE_PENDING_ROWS);
    CHECK(!sh_scene_pending_take(&t, &nodes[0], NULL));                          /* the oldest was evicted */
    CHECK(sh_scene_pending_take(&t, &nodes[SH_SCENE_PENDING_ROWS], NULL));
    sh_scene_pending_clear(&t);
    CHECK(t.count == 0 && !sh_scene_pending_take(&t, &nodes[1], NULL));
}

int main(int argc, char **argv)
{
    int i;
    test_pending();
    test_grammar();
    test_refusals();
    test_namespace_and_order();
    test_math();
    for (i = 1; i < argc; i++) test_image(argv[i]);
    if (g_fail) { printf("modscene_decl_test: FAILED\n"); return 1; }
    printf("modscene_decl_test: OK (%d image%s)\n", argc - 1, argc == 2 ? "" : "s");
    return 0;
}
