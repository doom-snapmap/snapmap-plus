/* The Timeline palette tile: its baked decl text, the prototype record text
 * and the rebind decision. No game needed. */
#include <stdio.h>
#include <string.h>

#include "overrides_baked.h"
#include "timeline_tile.h"
#include "image_sites.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define BAKED_COUNT (sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0])

static const ov_baked_decl_t *baked(const char *name)
{
    size_t i;
    for (i = 0; i < BAKED_COUNT; ++i)
        if (!strcmp(g_ov_baked_decls[i].name, name)) return &g_ov_baked_decls[i];
    return NULL;
}

/* Copy a baked decl into a terminated buffer. */
static const char *text_of(const ov_baked_decl_t *d, char *buf, size_t cap)
{
    if (!d || d->len + 1 > cap) return "";
    memcpy(buf, d->text, d->len);
    buf[d->len] = 0;
    return buf;
}

/* The text from `from` to the end of the edit block (the last "\t}"). */
static size_t tail_len(const char *from)
{
    const char *end = strstr(from, "\n\t}\n}");
    return end ? (size_t)(end - from) : 0;
}

static void test_table(void)
{
    size_t i;
    for (i = 0; i < BAKED_COUNT; ++i) {
        size_t j;
        CHECK(strstr(g_ov_baked_decls[i].name, "placeholder_target") == NULL);
        CHECK(strncmp(g_ov_baked_decls[i].name, "generated/decls/entitydef/", 26) != 0);
        for (j = 0; j < g_ov_baked_decls[i].len; ++j) CHECK(g_ov_baked_decls[i].text[j] != '\r');
    }
    CHECK(BAKED_COUNT == 4);
    CHECK(baked("generated/decls/snapeditorentitydef/custom/timeline.decl") != NULL);
    CHECK(baked("generated/decls/snapeditorentitydef/unknown/unknown.decl") != NULL);
    CHECK(baked("generated/decls/snapeditorentitydef/func/lift.decl") != NULL);
    CHECK(baked("generated/decls/snapeditorentitydef/volume/blocking.decl") != NULL);
    CHECK(!strcmp(SH_TL_TILE_PATH, "snapeditorentitydef/" SH_TL_TILE_SEDEF ".decl"));
}

static void test_tile(void)
{
    static char tl_buf[8192], un_buf[8192];
    const char *tl = text_of(baked("generated/decls/snapeditorentitydef/custom/timeline.decl"), tl_buf, sizeof tl_buf);
    const char *un = text_of(baked("generated/decls/snapeditorentitydef/unknown/unknown.decl"), un_buf, sizeof un_buf);
    const char *tl_ps = strstr(tl, "\t\tpropertySheets = {");
    const char *un_ps = strstr(un, "\t\tpropertySheets = {");
    const char *flags = strstr(tl, "\t\tflags = {\n");
    CHECK(strstr(tl, "\t\tentityDef = \"snapmaps/unknown\";\n") != NULL);
    CHECK(strstr(tl, "displayNameTag = \"#str_sh_timeline\";") != NULL);
    CHECK(strstr(tl, "displayDescriptionTag = \"#str_sh_timeline_desc\";") != NULL);
    CHECK(strstr(tl, "metaCategoryNameTag = \"#str_snapentity_metacategory_toybox\";") != NULL);
    CHECK(strstr(tl, "categoryNameTag = \"#str_sh_category_1\";") != NULL);
    CHECK(strstr(tl, "placementFlags = \"SNAP_ENTITY_PLACE_AIR\";") != NULL);
    CHECK(strstr(tl, "alwaysDrawBounds = true;") != NULL);
    CHECK(strstr(tl, "alwaysDrawLabel = true;") != NULL);
    CHECK(strstr(tl, "checkRenderModelForBounds = true;") != NULL);
    CHECK(strstr(tl, "moduleAutomation = \"SNAP_MODULE_AUTO_EDIT\";") != NULL);
    CHECK(strstr(tl, "class") == NULL);           /* a sedef carries no class */
    CHECK(strstr(tl, "placeholder") == NULL);
    /* Flag bits 0..23 must match the Unknown row's: the same four set, nothing else. */
    CHECK(flags != NULL);
    if (flags) {
        static const char want[] = "\t\tflags = {\n\t\t\teditable = true;\n\t\t\tisMoveable = true;\n"
                                   "\t\t\tisDeletable = true;\n\t\t\tshowPlacementArrow = true;\n\t\t}\n";
        CHECK(!strncmp(flags, want, sizeof want - 1));
    }
    /* Same propertySheets and inputs as the Unknown tile, so the reference-slot
     * counts and wiring lists agree; no outputs on either. */
    CHECK(tl_ps != NULL && un_ps != NULL);
    if (tl_ps && un_ps) {
        size_t n = tail_len(un_ps);
        CHECK(n > 0 && n == tail_len(tl_ps));
        CHECK(!strncmp(tl_ps, un_ps, n));
        CHECK(strstr(tl_ps, "inspector = \"display_name_inspector\";") != NULL);
        CHECK(strstr(tl_ps, "inputs = {") != NULL);
    }
    CHECK(strstr(tl, "outputs") == NULL && strstr(un, "outputs") == NULL);
    /* The Unknown tile draws its label; its placement line is load-bearing. */
    CHECK(strstr(un, "alwaysDrawLabel = true;") != NULL);
    CHECK(strstr(un, "alwaysDrawLabel = false;") == NULL);
    CHECK(strstr(un, "placementFlags = \"SNAP_ENTITY_PLACE_AIR_UNALIGNED\";") != NULL);
    CHECK(strstr(un, "entityDef = \"snapmaps/unknown\";") != NULL);
}

static void test_record_json(void)
{
    const char *j = sh_tl_record_json;
    int depth = 0, min_depth = 0;
    size_t i;
    CHECK(strstr(j, "\"className\":\"idTarget_Timeline\"") != NULL);
    CHECK(strstr(j, "\"inherit\":\"snapmaps/unknown\"") != NULL);
    CHECK(strstr(j, "\"state\":{\"edit\":{}}") != NULL);
    CHECK(strstr(j, "\"~type\":\"idDeclEntityDef\"") != NULL);
    CHECK(strstr(j, "\"~type\":\"idSnapEntity\"}") != NULL);
    for (i = 0; j[i]; ++i) {
        if (j[i] == '{') ++depth;
        else if (j[i] == '}' && --depth < min_depth) min_depth = depth;
    }
    CHECK(depth == 0 && min_depth == 0);
    CHECK(j[0] == '{' && j[strlen(j) - 1] == '}');
}

static void test_rebind_decision(void)
{
    int tile, unknown, other;
    const void *t = &tile, *u = &unknown, *o = &other;
    /* A timeline on the Unknown row moves to the tile, and stays there. */
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", "idTarget_Timeline", u, t, u) == t);
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", "idTarget_Timeline", t, t, u) == t);
    /* Anything else on snapmaps/unknown belongs on the Unknown row. */
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", "idEntity", t, t, u) == u);
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", "idEncounterManager", t, t, u) == u);
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", "", u, t, u) == u);
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", NULL, t, t, u) == u);
    /* Other inherits, other rows and missing rows are left alone. */
    CHECK(sh_tl_wanted_sedef("snapmaps/editor_only/placeholder_target", "idTarget_Timeline", u, t, u) == NULL);
    CHECK(sh_tl_wanted_sedef("snapmaps/unknownx", "idTarget_Timeline", u, t, u) == NULL);
    CHECK(sh_tl_wanted_sedef(NULL, "idTarget_Timeline", u, t, u) == NULL);
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", "idTarget_Timeline", o, t, u) == NULL);
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", "idTarget_Timeline", u, NULL, u) == NULL);
    CHECK(sh_tl_wanted_sedef("snapmaps/unknown", "idEntity", t, t, NULL) == NULL);
    /* Held placement (4) and wire drag (7) defer the pass. */
    CHECK(sh_tl_substate_blocks_rebind(4) && sh_tl_substate_blocks_rebind(7));
    CHECK(!sh_tl_substate_blocks_rebind(1) && !sh_tl_substate_blocks_rebind(2) && !sh_tl_substate_blocks_rebind(0));
}

/* A stock entity bound to a mod's row on the same entityDef goes to the stock row; an editor tweak (a stock
 * row's own name) and the stock row itself stay; nothing moves without a stock row. */
static void test_stock_row_first(void)
{
    int stock, mod;
    const void *s = &stock, *m = &mod;
    CHECK(sh_tl_stock_row_first("mods/abcdefgh/demons/ed1brk_enc", m, s) == s);
    CHECK(sh_tl_stock_row_first("demons/hellknight_enc", s, s) == NULL);
    CHECK(sh_tl_stock_row_first("demons/hellknight_enc", m, s) == NULL);   /* an editor tweak shadows the stock name */
    CHECK(sh_tl_stock_row_first("mods/abcdefgh/demons/ed1brk_enc", m, NULL) == NULL);
    CHECK(sh_tl_stock_row_first(NULL, m, s) == NULL);
}

static int synthetic_main(void)
{
    test_table();
    test_tile();
    test_record_json();
    test_rebind_decision();
    test_stock_row_first();
    if (g_fail) { printf("timeline_tile_test: FAILED\n"); return 1; }
    printf("timeline_tile_test: all passed\n");
    return 0;
}

/* The sort-guard's byte check (timeline_tile.c sh_timeline_tile_sort_target, the function the install calls) on each
 * build 13954591 executable named on the command line: SnapPaletteSortCall + 37 is an E8 to a sort that starts with
 * the wrapper's prologue. */
static void check_image(const char *path)
{
    const sh_build_identity *identity;
    uint8_t *base = image_open(path, &identity);
    sig_result *results;
    size_t n, cap = sig_db_count();
    uintptr_t lo, hi;
    if (!base) return;
    lo = (uintptr_t)base;
    hi = lo + ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    results = (sig_result *)calloc(cap, sizeof *results);
    if (!results) { g_image_fail = 1; free(base); return; }
    n = sig_resolve_all(base, results, cap);
    const uint8_t *site = image_site(base, results, n, "SnapPaletteSortCall", identity->vulkan);
    if (site) {
        const unsigned char *target = sh_timeline_tile_sort_target(site + SH_TIMELINE_SORT_CALL_OFF);
        printf("  SnapPaletteSortCall +%d E8 and the sort's prologue: %s\n", SH_TIMELINE_SORT_CALL_OFF,
               target ? "holds" : "FAIL");
        IMG_CHECK(target != NULL);
        IMG_CHECK((uintptr_t)target > lo && (uintptr_t)target < hi);
    }
    free(results);
    (void)sh_host_test_bind_image(NULL, NULL);
    free(base);
}

IMAGE_TEST_MAIN("timeline_tile_test", check_image, synthetic_main)
