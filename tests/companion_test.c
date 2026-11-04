/* companion_test.c -- owner-bound companions (companion.c, companion_sigs.c).
 *
 * Offline: the "companions" grammar and its refusals, the namespace rule, the
 * first-bundle-wins rule, lookup, and the owner-local to world transform. With
 * build 13954591 executables as arguments: every private signature resolves
 * uniquely at its recorded RVA for that renderer, CallSpawn starts with the 14
 * whole position-independent bytes the detour steals, the SpawnEntityDef
 * wrapper's second call is the idEntity cast, and Bind names its
 * "Tried to bind an object to itself." error. The spawn pose (ADD-L6 G): the
 * twelve floats written at spawnPosition are the composed world transform,
 * the create and finish calls resolve, finish reaches CallSpawn, and entity
 * SetOrigin is the physics-only tail jump that never updates the visuals. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "companion.h"
#include "companion_engine.h"
#include "modscene_decl.h"
#include "signatures.h"
#include "host_image.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int g_lines;
static void count_line(void *context, const char *line) { (void)context; (void)line; g_lines++; }

static int add(sh_companion_table *t, const char *id, const char *json)
{
    return sh_companion_add_bundle(t, id, json, strlen(json), count_line, NULL);
}

static void test_grammar(void)
{
    sh_companion_table t = {0};
    const sh_companion_owner *o;
    static const char ok[] =
        "{\"name\":\"x\",\"companions\":{\"mods/alex.tiles/panel\":["
        "{\"entityDef\":\"mods/alex.tiles/gui_a\",\"origin\":[1,2,3]},"
        "{\"entityDef\":\"func/gui\",\"axis\":[[0,1,0],[-1,0,0],[0,0,1]],\"activate\":false,\"bind\":false}]}}";
    CHECK(add(&t, "alex.tiles", ok) == 1);
    o = sh_companion_find(&t, "MODS\\alex.tiles\\PANEL");
    CHECK(o && o->count == 2);
    if (o) {
        CHECK(!strcmp(o->entries[0].entitydef, "mods/alex.tiles/gui_a"));
        CHECK(o->entries[0].origin[2] == 3.0f && o->entries[0].axis[0] == 1.0f && o->entries[0].axis[8] == 1.0f);
        CHECK(o->entries[0].activate == 1 && o->entries[0].bind == 1);
        CHECK(o->entries[1].activate == 0 && o->entries[1].bind == 0 && o->entries[1].axis[1] == 1.0f);
        CHECK(!strcmp(o->bundle, "alex.tiles"));
    }
    CHECK(add(&t, "other", "{\"strings\":{}}") == 0);
    CHECK(t.count == 1);
    sh_companion_table_free(&t);
}

static void test_refusals(void)
{
    static const char *bad[] = {
        "{\"companions\":[]}",                                                             /* not an object */
        "{\"companions\":{\"mods/other.mod/panel\":[{\"entityDef\":\"func/gui\"}]}}",      /* foreign owner */
        "{\"companions\":{\"maps/x/panel\":[{\"entityDef\":\"func/gui\"}]}}",              /* stock owner */
        "{\"companions\":{\"mods/a.b/panel\":[]}}",                                        /* empty list */
        "{\"companions\":{\"mods/a.b/panel\":[{\"origin\":[0,0,0]}]}}",                    /* no entityDef */
        "{\"companions\":{\"mods/a.b/panel\":[{\"entityDef\":\"func/gui\",\"orign\":[0,0,0]}]}}", /* typo */
        "{\"companions\":{\"mods/a.b/panel\":[{\"entityDef\":\"func gui\"}]}}",            /* space */
        "{\"companions\":{\"mods/a.b/panel\":[{\"entityDef\":\"func/gui\",\"origin\":[0,0]}]}}",
        "{\"companions\":{\"mods/a.b/panel\":[{\"entityDef\":\"func/gui\",\"origin\":[1e9,0,0]}]}}",
        "{\"companions\":{\"mods/a.b/panel\":[{\"entityDef\":\"func/gui\",\"axis\":[[2,0,0],[0,1,0],[0,0,1]]}]}}",
        "{\"companions\":{\"mods/a.b/panel\":[{\"entityDef\":\"func/gui\",\"axis\":[[1,0,0],[0,1,0],[0,0,-1]]}]}}",
        "{\"companions\":{\"mods/a.b/panel\":[{\"entityDef\":\"func/gui\",\"bind\":1}]}}",
        "{\"companions\":{\"mods/a.b/panel\":[{\"entityDef\":\"mods/a.b/PANEL\"}]}}",      /* itself */
        "{\"companions\":{\"mods/a.b/p\":[{\"entityDef\":\"func/gui\"}],\"mods/a.b/P\":[{\"entityDef\":\"func/gui\"}]}}",
    };
    size_t i;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        sh_companion_table t = {0};
        int before = g_lines;
        int r = add(&t, "a.b", bad[i]);
        if (r != -1 || t.count != 0 || g_lines != before + 1) printf("case %u\n", (unsigned)i);
        CHECK(r == -1 && t.count == 0 && g_lines == before + 1);
        sh_companion_table_free(&t);
    }
    /* Seventeen companions is one too many. */
    {
        char json[4096];
        size_t n;
        int k;
        sh_companion_table t = {0};
        n = (size_t)_snprintf_s(json, sizeof json, _TRUNCATE, "{\"companions\":{\"mods/a.b/p\":[");
        for (k = 0; k < 17; k++)
            n += (size_t)_snprintf_s(json + n, sizeof json - n, _TRUNCATE, "%s{\"entityDef\":\"func/gui\"}", k ? "," : "");
        _snprintf_s(json + n, sizeof json - n, _TRUNCATE, "]}}");
        CHECK(add(&t, "a.b", json) == -1 && t.count == 0);
        sh_companion_table_free(&t);
    }
    /* A refused block leaves nothing of itself, including owners before the bad one. */
    {
        sh_companion_table t = {0};
        CHECK(add(&t, "a.b", "{\"companions\":{\"mods/a.b/ok\":[{\"entityDef\":\"func/gui\"}],"
                             "\"mods/z.z/bad\":[{\"entityDef\":\"func/gui\"}]}}") == -1);
        CHECK(t.count == 0 && !sh_companion_find(&t, "mods/a.b/ok"));
        sh_companion_table_free(&t);
    }
}

static void test_namespace_and_order(void)
{
    sh_companion_table t = {0};
    const sh_companion_owner *o;
    /* '-' in an id may be spelled '_' in its names. */
    CHECK(add(&t, "campaign-interactables", "{\"companions\":{\"mods/campaign_interactables/panel\":"
                                            "[{\"entityDef\":\"func/gui\"}]}}") == 1);
    /* A later bundle cannot take an owner over; it keeps its other owners. */
    CHECK(add(&t, "campaign-interactables", "{\"companions\":{\"mods/Campaign_Interactables/PANEL\":"
                                            "[{\"entityDef\":\"func/static\"}],"
                                            "\"mods/campaign-interactables/desk\":[{\"entityDef\":\"func/gui\"}]}}") == 1);
    o = sh_companion_find(&t, "mods/campaign_interactables/panel");
    CHECK(o && !strcmp(o->entries[0].entitydef, "func/gui"));
    CHECK(sh_companion_find(&t, "mods/campaign-interactables/desk") != NULL);
    CHECK(t.refused == 1);
    CHECK(sh_companion_owner_namespace("Mods\\x/y") && !sh_companion_owner_namespace("mod/x") &&
          !sh_companion_owner_namespace("12_module/mods/x_1") && !sh_companion_owner_namespace(NULL));
    sh_companion_table_free(&t);
}

static int close_to(float a, float b) { return fabs((double)a - b) < 1e-4; }

static void test_compose(void)
{
    /* Owner yawed +90 degrees: its forward (row 0) is world +Y, its left (row 1) world -X. */
    const float origin[3] = { 100.0f, 200.0f, 10.0f };
    const float axis[9] = { 0, 1, 0,  -1, 0, 0,  0, 0, 1 };
    sh_companion_entry e;
    float at[3], rot[9];
    memset(&e, 0, sizeof e);
    e.origin[0] = 5.0f; e.origin[1] = 2.0f; e.origin[2] = 3.0f;
    e.axis[0] = e.axis[4] = e.axis[8] = 1.0f;
    sh_companion_compose(origin, axis, &e, at, rot);
    CHECK(close_to(at[0], 98.0f) && close_to(at[1], 205.0f) && close_to(at[2], 13.0f));
    CHECK(close_to(rot[0], 0) && close_to(rot[1], 1) && close_to(rot[3], -1) && close_to(rot[8], 1));
    /* A local yaw of +90 composes to 180 degrees. */
    memset(e.axis, 0, sizeof e.axis);
    e.axis[1] = 1.0f; e.axis[3] = -1.0f; e.axis[8] = 1.0f;
    sh_companion_compose(origin, axis, &e, at, rot);
    CHECK(close_to(rot[0], -1) && close_to(rot[1], 0) && close_to(rot[4], -1) && close_to(rot[8], 1));
    /* R245's left Olivia display: its panel-local rows and the source panel basis
     * give back the source GUI basis. */
    {
        const float panel[9] = { 5.96e-08f, -1.0f, 0.0f,  1.0f, 5.96e-08f, 0.0f,  0.0f, 0.0f, 1.0f };
        const float zero[3] = { 0, 0, 0 };
        sh_companion_entry left;
        memset(&left, 0, sizeof left);
        left.axis[0] = -0.7071063940f; left.axis[1] = -0.7071071442f;
        left.axis[3] = 0.7071072038f;  left.axis[4] = -0.7071064536f;
        left.axis[8] = 1.0f;
        sh_companion_compose(zero, panel, &left, at, rot);
        CHECK(close_to(rot[0], -0.7071071863f) && close_to(rot[1], 0.7071063519f));
        CHECK(close_to(rot[3], -0.7071064115f) && close_to(rot[4], -0.7071072459f));
    }
}

/* The spawn pose block: spawnPosition (+0x3A8), then spawnOrientation rows
 * (+0x3B4), as compose gives them. The v3-companions case: an owner yawed
 * +90 at (200, 0, 16), its companion 64 above it. */
static void test_spawn_pose(void)
{
    const float origin[3] = { 200.0f, 0.0f, 16.0f };
    const float axis[9] = { 0, 1, 0,  -1, 0, 0,  0, 0, 1 };
    sh_companion_entry e;
    float pose[SH_COMPANION_SPAWN_POSE_FLOATS], at[3], rot[9];
    int i;
    CHECK(SH_COMPANION_SPAWN_POSE_FLOATS * sizeof(float) == 0x3B4 - 0x3A8 + 9 * sizeof(float));
    memset(&e, 0, sizeof e);
    e.origin[2] = 64.0f;
    e.axis[0] = e.axis[4] = e.axis[8] = 1.0f;
    for (i = 0; i < SH_COMPANION_SPAWN_POSE_FLOATS; i++) pose[i] = -12345.0f;
    sh_companion_spawn_pose(origin, axis, &e, pose);
    sh_companion_compose(origin, axis, &e, at, rot);
    CHECK(close_to(pose[0], 200.0f) && close_to(pose[1], 0.0f) && close_to(pose[2], 80.0f));
    for (i = 0; i < 3; i++) CHECK(pose[i] == at[i]);
    for (i = 0; i < 9; i++) CHECK(pose[3 + i] == rot[i]);
    CHECK(close_to(pose[3], 0) && close_to(pose[4], 1) && close_to(pose[6], -1) && close_to(pose[11], 1));
}

/* ---- runtime arming (sh_companions_ensure's checks) -------------------- */

static int arm(int resolved, int on_main, int installed, int changed, int owners, int prologue_ok)
{
    sh_companion_arm a;
    a.resolved = resolved; a.on_main = on_main; a.installed = installed;
    a.index_changed = changed; a.owners = owners; a.prologue_ok = prologue_ok;
    return sh_companion_arm_decide(&a);
}

static void test_arm(void)
{
    /* No boot resolution: nothing, whatever else holds. */
    CHECK(arm(0, 1, 0, 1, 3, 1) == SH_COMPANION_ARM_UNRESOLVED);
    CHECK(sh_companion_arm_decide(NULL) == SH_COMPANION_ARM_UNRESOLVED);
    /* Already installed: live, from any thread, never re-armed or unhooked. */
    CHECK(arm(1, 0, 1, 1, -1, -1) == SH_COMPANION_ARM_LIVE);
    CHECK(arm(1, 1, 1, 0, 0, 0) == SH_COMPANION_ARM_LIVE);
    /* Off DOOM's main thread, or the main thread unknown: never armed, not even read. */
    CHECK(arm(1, 0, 0, 1, -1, -1) == SH_COMPANION_ARM_OFF_MAIN);
    CHECK(arm(1, -1, 0, 1, 2, 1) == SH_COMPANION_ARM_OFF_MAIN);
    /* The index unchanged since the table was built: nothing to do. */
    CHECK(arm(1, 1, 0, 0, -1, -1) == SH_COMPANION_ARM_UNCHANGED);
    /* Changed: rebuild the table first. */
    CHECK(arm(1, 1, 0, 1, -1, -1) == SH_COMPANION_ARM_BUILD);
    /* The rebuilt table without an owner: the spawn chain stays untouched. */
    CHECK(arm(1, 1, 0, 1, 0, -1) == SH_COMPANION_ARM_NO_OWNER);
    /* With an owner and the expected prologue: commit; another detour there: refused. */
    CHECK(arm(1, 1, 0, 1, 2, 1) == SH_COMPANION_ARM_COMMIT);
    CHECK(arm(1, 1, 0, 1, 2, 0) == SH_COMPANION_ARM_FOREIGN);
    CHECK(!strcmp(sh_companion_arm_name(SH_COMPANION_ARM_FOREIGN), "prologue changed"));
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
    static const uint8_t prologue[14] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9, 0xFF, 0x10 };
    static const uint8_t cast_head[22] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
                                           0x74, 0x21, 0x48, 0x8B, 0x01, 0xFF, 0x10, 0x8B, 0x48, 0x68 };
    wchar_t wide[MAX_PATH];
    const sh_build_identity *id;
    uint8_t *base = map_pe_by_rva(path);
    sig_result r[8];
    int i, n = 0, bad = 0;
    CHECK(base != NULL);
    if (!base) return;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    id = sh_host_test_bind_image(base, wide);
    CHECK(id != NULL);
    printf("image %s: %s\n", path, id ? id->label : "unsupported");
    for (i = 0; SH_COMPANION_SIGNATURES[i].name; i++, n++) {
        uint32_t known = id ? (id->vulkan ? SH_COMPANION_SIGNATURES[i].rva_vk : SH_COMPANION_SIGNATURES[i].rva_gl) : 0;
        sig_status st = sig_resolve_one(base, &SH_COMPANION_SIGNATURES[i], &r[i]);
        int good = st == SIG_OK && r[i].rva == known;
        printf("%s %-24s resolved=0x%-8x known=0x%x\n", good ? "OK " : "BAD", SH_COMPANION_SIGNATURES[i].name,
               r[i].rva, known);
        CHECK(good);
        if (!good) bad++;
    }
    CHECK(n == 5);
    if (bad || n != 5) { free(base); return; }
    CHECK(!memcmp(base + r[0].rva, prologue, sizeof prologue));
    {
        /* The runtime arm's prologue check on this image: as shipped it
         * commits; a foreign 5-byte jump over the head is refused. */
        uint8_t head[14];
        memcpy(head, base + r[0].rva, sizeof head);
        CHECK(arm(1, 1, 0, 1, 1, !memcmp(head, prologue, sizeof prologue)) == SH_COMPANION_ARM_COMMIT);
        head[0] = 0xE9;
        CHECK(arm(1, 1, 0, 1, 1, !memcmp(head, prologue, sizeof prologue)) == SH_COMPANION_ARM_FOREIGN);
    }
    {
        const uint8_t *w = base + r[3].rva;
        int32_t rel;
        memcpy(&rel, w + 0x22, 4);
        CHECK(w[0x21] == 0xE8);
        CHECK(!memcmp(w + 0x26 + rel, cast_head, sizeof cast_head));
        CHECK((uint32_t)(w + 0x26 + rel - base) == (id && id->vulkan ? 0x13B34E0u : 0x13B3370u));
    }
    {
        static const char text[] = "Tried to bind an object to itself.";
        const uint8_t *lea = base + r[1].rva + 0x16E;
        int32_t disp;
        CHECK(lea[0] == 0x48 && lea[1] == 0x8D && lea[2] == 0x0D);
        memcpy(&disp, lea + 3, 4);
        CHECK(!memcmp(lea + 7 + disp, text, sizeof text));
    }
    {
        /* The spawn-pose route: create and finish resolve at their RVAs, and
         * finish's register-then-spawn reaches CallSpawn with one direct call. */
        sig_result create, finish;
        const sig_entry *sc = NULL, *sf = NULL;
        int k, j, calls = 0;
        for (k = 0; SH_MODSCENE_SPAWN_SIGNATURES[k].name; k++) {
            if (!strcmp(SH_MODSCENE_SPAWN_SIGNATURES[k].name, "SceneCreateEntity")) sc = &SH_MODSCENE_SPAWN_SIGNATURES[k];
            if (!strcmp(SH_MODSCENE_SPAWN_SIGNATURES[k].name, "SceneFinishSpawn")) sf = &SH_MODSCENE_SPAWN_SIGNATURES[k];
        }
        CHECK(sc && sf);
        if (sc && sf) {
            CHECK(sig_resolve_one(base, sc, &create) == SIG_OK &&
                  create.rva == (id && id->vulkan ? sc->rva_vk : sc->rva_gl));
            CHECK(sig_resolve_one(base, sf, &finish) == SIG_OK &&
                  finish.rva == (id && id->vulkan ? sf->rva_vk : sf->rva_gl));
            for (j = 0; j < 0xC0; j++) {
                const uint8_t *p = base + finish.rva + j;
                int32_t rel;
                if (p[0] != 0xE8) continue;
                memcpy(&rel, p + 1, 4);
                if ((uint32_t)(finish.rva + j + 5 + rel) == r[0].rva) calls++;
            }
            printf("%s finish reaches CallSpawn: %d direct call(s)\n", calls == 1 ? "OK " : "BAD", calls);
            CHECK(calls == 1);
        }
    }
    {
        /* Entity SetOrigin (vtable +0x338): mov rcx,[rcx+0x808]; or r8d,-1;
         * mov rax,[rcx]; jmp [rax+0x60]. The physics alone moves; nothing
         * updates the visuals, so a placement after the spawn needs one. */
        static const uint8_t set_origin[18] = { 0x48, 0x8B, 0x89, 0x08, 0x08, 0x00, 0x00, 0x41, 0x83, 0xC8, 0xFF,
                                                0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x60 };
        uint32_t at = id && id->vulkan ? 0x82ABB0u : 0x829D70u;
        CHECK(!memcmp(base + at, set_origin, sizeof set_origin));
    }
    free(base);
}

int main(int argc, char **argv)
{
    int i;
    test_grammar();
    test_refusals();
    test_namespace_and_order();
    test_compose();
    test_spawn_pose();
    test_arm();
    for (i = 1; i < argc; i++) test_image(argv[i]);
    if (g_fail) { printf("companion_test: FAILED\n"); return 1; }
    printf("companion_test: OK (%d image%s)\n", argc - 1, argc == 2 ? "" : "s");
    return 0;
}
