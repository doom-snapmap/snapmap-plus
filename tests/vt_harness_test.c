/* vt_harness_test.c -- the sh_vt diagnostic's pure helpers, and the
 * reference family bundle end to end.
 *
 *   vt_harness_test.exe [<rctest mod folder>]
 *
 * Without an argument: the page-header swap and the member cell lookup (the
 * record decoding is modvt_test.c's). With the folder made by snaphak-re's
 * lane RC make_family.py (or SH_VT_RCTEST set to it): a copy is built by the
 * real bundle builder with a stock stand-in, admitted by the real serving
 * index, its request paths looked up, its family collected as modvt.c
 * registers it, and the editor plan's order checked (the material before the
 * entityDef that names it through customMaterial, the sedef last). The folder
 * holds stock game bytes, so it is not part of the repository. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modserve.h"
#include "../src/backend/vt_harness.h"
#include "../src/backend/modvt.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *line) { (void)line; }

static void helpers(void)
{
    unsigned char page[16] = { 0x1E, 0x1E, 0x1E, 0x0C, 0x2A, 0xE5, 0x24, 0x43, 0x2B, 0x06, 0x04, 0xE6, 0x00, 0x00, 0x60, 0x00 };
    unsigned char h[0x170];
    int i;
    unsigned run = 0;

    printf("-- pure helpers\n");

    sh_vt_swap_page_header(page);
    CHECK(page[4] == 0xE5 && page[5] == 0x2A && page[0xC] == 0 && page[0xE] == 0x60 && page[0] == 0x1E);
    sh_vt_swap_page_header(page);
    CHECK(page[4] == 0x2A && page[5] == 0xE5);

    /* Member sq4 (column 3, row 0): the level table of lane RB 1.2. */
    memset(h, 0, sizeof h);
    *(unsigned *)h = 0xA63FBB21u;
    *(unsigned *)(h + 4) = 2;
    *(unsigned *)(h + 0x0C) = 12;
    for (i = 0; i < 12; ++i) {
        unsigned *e = (unsigned *)(h + 0x50 + 0x18 * i);
        unsigned w = 512u >> i ? 512u >> i : 1u;
        e[0] = i <= 9 ? (3u * 512u) >> i : (i == 10 ? 1u : 0u);
        e[1] = 0;
        e[2] = e[3] = w;
        e[4] = run;
        e[5] = w * w;
        run += w * w;
    }
    CHECK(run == 349527u);
    CHECK(sh_vt_member_cell(h, 0, 1856, 64) == 64 * 512 + (1856 - 1536));   /* the barrel2 cell of Q1 */
    CHECK(sh_vt_member_cell(h, 0, 1535, 0) == -1);
    CHECK(sh_vt_member_cell(h, 11, 0, 0) == 349526);
    CHECK(sh_vt_member_cell(h, 12, 0, 0) == -1);
    h[4] = 3;
    CHECK(sh_vt_member_cell(h, 0, 1856, 64) == -1);
}

/* The game: anything outside mods/ exists. */
static int stock_exists(void *ctx, const char *request) { (void)ctx; return strstr(request, "mods/") == NULL; }

static void fixture(const char *src)
{
    char root[MAX_PATH], mods[MAX_PATH], folder[MAX_PATH], cause[SH_MB_CAUSE_MAX];
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_mb_bundle b;
    sh_modserve_hit hit;
    sh_modserve_plan plan;
    int i, mat = -1, ent = -1, sed = -1, mega = 0;
    size_t k;

    printf("-- the rctest family bundle: build, admit, serve, plan (%s)\n", src);
    CHECK(mt_temp_dir("vt_harness", root, sizeof root));
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\rctest", mods);
    CreateDirectoryA(mods, NULL);
    CHECK(mt_copy_tree(src, folder));
    CHECK(sh_mb_build(folder, &r, &b) == 1);
    printf("    build: %u error(s) %s; %u warning(s) %s; %zu files, %llu bytes; needs %u (bundle %u, game %u, unresolved %u)\n",
           b.errors, b.errors ? b.first_cause : "", b.warnings, b.warnings ? b.first_warning : "", b.file_count,
           b.total_size, b.needs.total, b.needs.local, b.needs.stock, b.needs.unresolved);
    CHECK(!strcmp(b.id, "rctest") && b.file_count == 4);
    for (k = 0; k < b.file_count; ++k) {
        const sh_mb_file *f = &b.files[k];
        size_t n;
        printf("    file %s (%s %s) %llu bytes, %zu need(s)\n", f->request, f->ptype, f->pname, f->size, f->need_count);
        for (n = 0; n < f->need_count; ++n) printf("        needs %s %s\n", f->needs[n].type, f->needs[n].name);
        if (!strcmp(f->request, "virtualtextures/mods/rctest/fam_sq1.mega2")) mega = 1;
        if (!strcmp(f->request, "generated/decls/entitydef/mods/rctest/vtprop.decl")) {
            int named = 0;
            for (n = 0; n < f->need_count; ++n)
                named |= !strcmp(f->needs[n].type, "material") && !strcmp(f->needs[n].name, "mods/rctest/vtprop");
            CHECK(named);
        }
    }
    CHECK(mega);
    CHECK(sh_mb_write(&b, folder, cause, sizeof cause) == 1);
    printf("    bundle %s\n", b.hash);

    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(sh_modserve_bundle_state(b.hash, NULL, 0) == 1);
    CHECK(sh_modserve_lookup("virtualtextures/mods/rctest/fam_sq1.mega2", &hit) == 1 && hit.size > 0x170);
    CHECK(sh_modserve_lookup("VirtualTextures\\Mods\\RCTest\\fam_sq1.mega2", &hit) == 1);
    CHECK(sh_modserve_lookup("virtualtextures/mods/rctest/fam_sq2.mega2", &hit) == 0);
    CHECK(sh_modserve_lookup("virtualtextures/mods/rctest/fam.pages", &hit) == 0);
    CHECK(sh_modserve_lookup("generated/decls/material/mods/rctest/vtprop.decl", &hit) == 1 && hit.is_decl);
    {
        sh_modvt_family fam[4];
        int nf = sh_modvt_collect(fam, 4);
        CHECK(nf == 1 && !strcmp(fam[0].name, "mods/rctest/fam") && fam[0].members == 1u &&
              !strcmp(fam[0].path, "virtualtextures/mods/rctest/fam.pages") &&
              sh_modserve_lookup("virtualtextures/mods/rctest/fam_sq1.mega2", &hit) == 1 && fam[0].bytes == hit.size);
        if (nf == 1) printf("    family %s (%s): members 0x%X, %llu bytes\n", fam[0].name, fam[0].path, fam[0].members,
                            (unsigned long long)fam[0].bytes);
    }

    CHECK(sh_modserve_plan_editor(&plan) == 3 && plan.blocked == 0);
    for (i = 0; i < plan.count; ++i) {
        printf("    step %d %s '%s' rank %d %s\n", i, plan.steps[i].type, plan.steps[i].name, plan.steps[i].rank,
               plan.steps[i].reg_path);
        if (!_stricmp(plan.steps[i].type, "material")) mat = i;
        if (!_stricmp(plan.steps[i].type, "entityDef")) ent = i;
        if (!_stricmp(plan.steps[i].type, "snapEditorEntityDef")) sed = i;
    }
    CHECK(mat >= 0 && ent > mat && sed > ent);
    sh_modserve_plan_free(&plan);
    sh_mb_free(&b);
    mt_remove_tree(root);
}

int main(int argc, char **argv)
{
    char env[MAX_PATH];
    const char *src = argc > 1 ? argv[1] : NULL;
    helpers();
    if (!src && GetEnvironmentVariableA("SH_VT_RCTEST", env, sizeof env) && env[0]) src = env;
    if (src) fixture(src);
    else printf("[skip] the rctest family bundle (pass the folder or set SH_VT_RCTEST)\n");
    printf("vt_harness_test: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
