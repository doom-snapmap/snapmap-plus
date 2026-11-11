/* vtpack_test.c -- mods' lit pages packed into square 16 (vtpack.c).
 *
 *   vtpack_test.exe [<dump folder>]
 *
 * Synthetic members only (no game bytes): a stock-like square 16 (levels 1 to
 * 8 one row at the top edge, levels 9 to 11 one cell) and mods in the relative
 * form. Checked: the member checks and the relative form's refusals, the
 * placement (no overlap, clear of every stock cell, deterministic for a set
 * in any order, held places kept, the coarse levels carried, a full square),
 * the merged member (it opens; every stock cell keeps its page bytes; every
 * mod cell lands at its node; levels 9 to 11 stock), and the material rewrites
 * against texts the reference (Doomforge tools/vt/vmtr_square.py) wrote. With
 * a folder, the members and the merged member are written there so the
 * reference can rebuild the merged member and compare the bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/vtpack.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

#include "vt_fixture.h"

static void dump(const char *dir, const char *name, const uint8_t *p, size_t n)
{
    char path[600];
    FILE *f;
    if (!dir) return;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, name);
    if (fopen_s(&f, path, "wb") || !f) { fprintf(stderr, "cannot write %s\n", path); g_failed++; return; }
    fwrite(p, 1, n, f);
    fclose(f);
}

/* ------------------------------------------------------------------ tests */

static void test_member_checks(void)
{
    size_t n;
    uint8_t *m = lit_member(20, 12, 11, 1, 0, &n);
    sh_vtp_member v;
    sh_vtp_lit lit;
    char why[300];
    CHECK(sh_vtp_open(&v, m, n, why, sizeof why));
    CHECK(v.square == 1);
    CHECK(sh_vtp_lit_info(&v, &lit, why, sizeof why));
    CHECK(lit.w == 20 && lit.h == 12 && lit.node == 32);
    m[0] ^= 1;
    CHECK(!sh_vtp_open(&v, m, n, why, sizeof why) && strstr(why, "magic"));
    m[0] ^= 1;
    m[0x4C] ^= 1;
    CHECK(!sh_vtp_open(&v, m, n, why, sizeof why) && strstr(why, "cell count"));
    m[0x4C] ^= 1;
    {   /* a cell naming a page past the count */
        uint64_t idx;
        memcpy(&idx, m + 0x40, 8);
        w32(m + idx, 0x7FFFFFFF);
        CHECK(!sh_vtp_open(&v, m, n, why, sizeof why) && strstr(why, "names page"));
    }
    free(m);
    m = lit_member(300, 10, 11, 1, 0, &n);
    CHECK(sh_vtp_open(&v, m, n, why, sizeof why));
    CHECK(!sh_vtp_lit_info(&v, &lit, why, sizeof why) && strstr(why, "300 x 10"));
    free(m);
    m = lit_member(20, 12, 11, 2, 0, &n);
    CHECK(sh_vtp_open(&v, m, n, why, sizeof why) && v.square == 2);
    CHECK(!sh_vtp_lit_info(&v, &lit, why, sizeof why) && strstr(why, "square 2"));
    free(m);
    m = lit_member(20, 12, 11, 1, 1, &n);
    CHECK(sh_vtp_open(&v, m, n, why, sizeof why));
    CHECK(!sh_vtp_lit_info(&v, &lit, why, sizeof why) && strstr(why, "no level 1 page above"));
    free(m);
    {   /* a page whose header disagrees with its size */
        uint8_t pg[40] = { 0x1E, 0x1E, 0x1E, 0x04, 0, 10, 0, 10, 0, 10, 0, 0, 0, 0, 0x60, 0xFF };
        CHECK(!sh_vtp_page_ok(pg, sizeof pg, why, sizeof why));
        pg[5] = 4; pg[7] = 10; pg[9] = 10;
        CHECK(sh_vtp_page_ok(pg, 40, why, sizeof why));
        pg[14] = 0xE0;
        CHECK(!sh_vtp_page_ok(pg, 40, why, sizeof why) && strstr(why, "lightmap"));
    }
}

static int nodes_disjoint(const sh_vtp_req *r, int n)
{
    int i, j;
    for (i = 0; i < n; ++i)
        for (j = i + 1; j < n; ++j)
            if (r[i].placed && r[j].placed && r[i].node.x < r[j].node.x + r[j].node.size &&
                r[j].node.x < r[i].node.x + r[i].node.size && r[i].node.y < r[j].node.y + r[j].node.size &&
                r[j].node.y < r[i].node.y + r[i].node.size) return 0;
    return 1;
}

static void test_place(const sh_vtp_member *stock)
{
    sh_vtp_req a[4], b[4];
    int i, n;
    memset(a, 0, sizeof a);
    a[0].key = "tyrant@1"; a[0].size = 64;
    a[1].key = "small@2"; a[1].size = 8;
    n = sh_vtp_place(&stock, 1, a, 2);
    CHECK(n == 2);
    /* Each gets a 256 block of its own, so every level up to 8 is carried. */
    CHECK(a[0].node.x == 0 && a[0].node.y == 256 && a[0].node.size == 64);
    CHECK(a[1].node.x == 256 && a[1].node.y == 256 && a[1].node.size == 8);
    CHECK(sh_vtp_carry(stock, a[0].node, &a[1].node, 1) == 8);
    /* The same set in another order places the same. */
    memset(b, 0, sizeof b);
    b[0] = a[1]; b[1] = a[0];
    b[0].placed = b[1].placed = 0;
    CHECK(sh_vtp_place(&stock, 1, b, 2) == 2);
    CHECK(b[0].node.x == a[1].node.x && b[0].node.y == a[1].node.y && b[1].node.x == a[0].node.x &&
          b[1].node.y == a[0].node.y);
    /* A held place is kept; a newcomer goes around it. */
    memset(b, 0, sizeof b);
    b[0].key = "small@2"; b[0].size = 8; b[0].held = 1; b[0].node.x = 8; b[0].node.y = 40; b[0].node.square = 16;
    b[1].key = "big@3"; b[1].size = 128;
    CHECK(sh_vtp_place(&stock, 1, b, 2) == 2);
    CHECK(b[0].node.x == 8 && b[0].node.y == 40);
    CHECK(b[1].node.y == 256 && nodes_disjoint(b, 2));
    /* Nothing touches the stock row: every placed node fits. */
    for (i = 0; i < 2; ++i) CHECK(sh_vtp_fits(stock, b[i].node.x, b[i].node.y, b[i].node.size));
    CHECK(!sh_vtp_fits(stock, 0, 0, 8));
    /* Two 256 blocks fill the lower half; a third finds no room. */
    memset(b, 0, sizeof b);
    b[0].key = "a"; b[0].size = 256;
    b[1].key = "b"; b[1].size = 256;
    b[2].key = "c"; b[2].size = 256;
    b[3].key = "d"; b[3].size = 128;
    CHECK(sh_vtp_place(&stock, 1, b, 4) == 3);
    CHECK(b[0].placed && b[1].placed && !b[2].placed && b[3].placed);
    CHECK(b[3].node.y == 128 && nodes_disjoint(b, 4));
    /* Fit counts per node size (square 16's free area). */
    {
        static const int sizes[] = { 256, 128, 64, 32, 8 }, want[] = { 2, 12, 56, 240, 4032 };
        int s, x, y;
        for (s = 0; s < 5; ++s) {
            int c = 0;
            for (y = 0; y + sizes[s] <= 512; y += sizes[s])
                for (x = 0; x + sizes[s] <= 512; x += sizes[s]) c += sh_vtp_fits(stock, x, y, sizes[s]);
            CHECK(c == want[s]);
        }
    }
}

static void test_compose(const char *dir, const sh_vtp_member *stock)
{
    size_t na, nb, nm;
    uint8_t *ma = lit_member(20, 12, 100, 1, 0, &na), *mb = lit_member(5, 3, 900, 1, 0, &nb), *merged = NULL;
    sh_vtp_member va, vb, vm;
    sh_vtp_mod mods[2];
    sh_vtp_req r[2];
    char why[300];
    int L, x, y, k, ok_stock = 1, ok_mod = 1;
    CHECK(sh_vtp_open(&va, ma, na, why, sizeof why));
    CHECK(sh_vtp_open(&vb, mb, nb, why, sizeof why));
    memset(r, 0, sizeof r);
    r[0].key = "aaaa1111@h1"; r[0].size = 32;
    r[1].key = "bbbb2222@h2"; r[1].size = 8;
    CHECK(sh_vtp_place(&stock, 1, r, 2) == 2);
    memset(mods, 0, sizeof mods);
    /* Given out of key order: the member orders them by key. */
    mods[0].key = r[1].key; mods[0].m = &vb; mods[0].node = r[1].node;
    mods[1].key = r[0].key; mods[1].m = &va; mods[1].node = r[0].node;
    CHECK(sh_vtp_compose(stock, mods, 2, &merged, &nm, why, sizeof why));
    CHECK(merged && sh_vtp_open(&vm, merged, nm, why, sizeof why) && vm.square == 16);
    CHECK(vm.page_count == stock->page_count + mods[0].pages + mods[1].pages);
    CHECK(mods[1].levels == 9 && mods[0].levels == 9);
    /* Every stock cell keeps its page id and bytes. */
    for (L = 0; L < 12; ++L)
        for (y = 0; y < side(L); ++y)
            for (x = 0; x < side(L); ++x) {
                uint32_t s = sh_vtp_cell(stock, L, x, y);
                const uint8_t *p1, *p2;
                size_t l1, l2;
                if (s == SH_VTP_EMPTY) continue;
                if (sh_vtp_cell(&vm, L, x, y) != s) { ok_stock = 0; continue; }
                sh_vtp_page(stock, s, &p1, &l1);
                sh_vtp_page(&vm, s, &p2, &l2);
                if (l1 != l2 || memcmp(p1, p2, l1)) ok_stock = 0;
            }
    CHECK(ok_stock);
    /* Every mod cell of levels 0..8 lands at its node with its own bytes; 9..11 stay stock. */
    for (k = 0; k < 2; ++k) {
        const sh_vtp_member *own = mods[k].m;
        for (L = 0; L < 12; ++L) {
            int lim = mods[k].node.size >> L ? mods[k].node.size >> L : 1;
            for (y = 0; y < lim; ++y)
                for (x = 0; x < lim; ++x) {
                    uint32_t o = sh_vtp_cell(own, L, x, y), g;
                    const uint8_t *p1, *p2;
                    size_t l1, l2;
                    if (o == SH_VTP_EMPTY) continue;
                    g = sh_vtp_cell(&vm, L, (mods[k].node.x >> L) + x, (mods[k].node.y >> L) + y);
                    if (L >= 9) { if (g != sh_vtp_cell(stock, L, 0, 0)) ok_mod = 0; continue; }
                    if (g == SH_VTP_EMPTY) { ok_mod = 0; continue; }
                    sh_vtp_page(own, o, &p1, &l1);
                    sh_vtp_page(&vm, g, &p2, &l2);
                    if (l1 != l2 || memcmp(p1, p2, l1)) ok_mod = 0;
                }
        }
    }
    CHECK(ok_mod);
    /* Deterministic: the same inputs give the same bytes. */
    {
        uint8_t *again = NULL;
        size_t n2;
        CHECK(sh_vtp_compose(stock, mods, 2, &again, &n2, why, sizeof why) && n2 == nm && !memcmp(again, merged, nm));
        free(again);
    }
    /* Two mods on one place are refused. */
    {
        uint8_t *bad = NULL;
        size_t n2;
        sh_vtp_mod two[2];
        two[0] = mods[0]; two[1] = mods[1];
        two[0].node = two[1].node;
        two[0].node.size = 8;
        CHECK(!sh_vtp_compose(stock, two, 2, &bad, &n2, why, sizeof why) && strstr(why, "already taken"));
        free(bad);
    }
    /* With nothing served the member is the stock square's own pages and cells. */
    {
        uint8_t *only = NULL;
        size_t n2;
        CHECK(sh_vtp_compose(stock, NULL, 0, &only, &n2, why, sizeof why));
        CHECK(only && n2 == stock->len && !memcmp(only, stock->data, n2));
        free(only);
    }
    dump(dir, "mod_aaaa1111.mega2", ma, na);
    dump(dir, "mod_bbbb2222.mega2", mb, nb);
    dump(dir, "merged.mega2", merged, nm);
    if (dir) printf("  placed aaaa1111@h1 at %d,%d and bbbb2222@h2 at %d,%d\n", r[0].node.x, r[0].node.y, r[1].node.x,
                    r[1].node.y);
    free(merged);
    free(ma);
    free(mb);
}

/* Texts the reference wrote (tools/vt/vmtr_square.py rewrite_material at node 0,256,64, rewrite_plain). */
static const char g_mat[] =
    "{\nsurfacetype\tsurftype_flesh\ndiffusemap\t\"mods/ab12cd34/x.tga\"\nambientprogram\tskinvmtr\n"
    "shadowprogram\tshadowvmtrclipspheres\nlandpagefile\t\"mods/ab12cd34/vmtr\"\nusevirtualmapping\t1.000000\n"
    "virtualmapping\t{ 0.0039062500, 0.0039062500, 0.0039062500, 0.0156250000 }\n"
    "virtualtexturefeedbackfloat\t{ 1.000000, 2048.000000, 245760.000000, 16.000000 }\n}\n";
static const char g_moved[] =
    "{\nsurfacetype\tsurftype_flesh\ndiffusemap\t\"mods/ab12cd34/x.tga\"\nambientprogram\tskinvmtr\n"
    "shadowprogram\tshadowvmtrclipspheres\nusevirtualmapping\t1.000000\n"
    "virtualmapping\t{ 0.0039062500, 0.0039062500, 0.7539062500, 0.8906250000 }\n"
    "virtualtexturefeedbackfloat\t{ 1.000000, 2048.000000, 245760.000000, 16.000000 }\n"
    "physicalfilterparms\tphysicalvmtrfilterparms\nminlodmap\t_black\npagetablemap\t_vmtrpagetable\n"
    "physicalmappingsmap\t_physicalvmtrmappings\nphysicalpagesmaparray0\t_physicalvmtrpages0\n"
    "physicalpagesmaparray1\t_physicalvmtrpages0\nphysicalpagesmap4\t_black\nphysicalpagesprtmap4\t_black\n}\n";
static const char g_plain[] =
    "{\nsurfacetype\tsurftype_flesh\ndiffusemap\t\"mods/ab12cd34/x.tga\"\nambientprogram\toutsidenonvmtr\n"
    "shadowprogram\tshadownonvmtr\n}\n";

static void test_materials(void)
{
    sh_vtp_node node = { 0, 256, 64, 16 };
    size_t n;
    char *t;
    double v[4];
    CHECK(sh_vtp_material_names(g_mat, sizeof g_mat - 1, "ab12cd34"));
    CHECK(sh_vtp_material_names(g_mat, sizeof g_mat - 1, "AB12CD34"));
    CHECK(!sh_vtp_material_names(g_mat, sizeof g_mat - 1, "zz12cd34"));
    CHECK(sh_vtp_material_mapping(g_mat, sizeof g_mat - 1, v) && v[0] == 0.00390625 && v[3] == 0.015625);
    t = sh_vtp_material_moved(g_mat, sizeof g_mat - 1, node, &n);
    CHECK(t && n == sizeof g_moved - 1 && !strcmp(t, g_moved));
    free(t);
    t = sh_vtp_material_plain(g_mat, sizeof g_mat - 1, &n);
    CHECK(t && !strcmp(t, g_plain));
    free(t);
    {   /* CRLF line ends give the same text; a stock line already present is not doubled */
        char crlf[2048], *o = crlf;
        const char *p;
        for (p = g_mat; *p; ++p) { if (*p == '\n') *o++ = '\r'; *o++ = *p; }
        *o = 0;
        t = sh_vtp_material_moved(crlf, strlen(crlf), node, &n);
        CHECK(t && !strcmp(t, g_moved));
        free(t);
        t = sh_vtp_material_moved(g_moved, sizeof g_moved - 1, node, &n);
        CHECK(t && strstr(t, "pagetablemap\t_vmtrpagetable") &&
              !strstr(strstr(t, "pagetablemap\t_vmtrpagetable") + 1, "pagetablemap\t_vmtrpagetable"));
        free(t);
    }
    CHECK(sh_vtp_material_moved("{\ndiffusemap\tx\n}\n", 16, node, &n) == NULL);
    CHECK(sh_vtp_material_plain("{\ndiffusemap\tx\n", 15, &n) == NULL);
}

/* A second square: a mod placed in square 8 is composed into square 8's member
 * only, and its material moves to square 8's origin (column 3, row 1). */
static void test_second_square(const sh_vtp_member *sq16)
{
    size_t n8, nm, n;
    uint8_t *b8 = stock_member_of(8, &n8), *ma = lit_member(20, 12, 300, 1, 0, &nm), *m16 = NULL, *m8 = NULL;
    sh_vtp_member s8, va, v8;
    const sh_vtp_member *only8[1], *both[2];
    sh_vtp_req r[3];
    sh_vtp_mod mod;
    char why[300], *t;
    size_t tl;
    CHECK(sh_vtp_open(&s8, b8, n8, why, sizeof why) && s8.square == 8);
    CHECK(sh_vtp_open(&va, ma, nm, why, sizeof why));
    only8[0] = &s8;
    memset(r, 0, sizeof r);
    r[0].key = "cccc3333@h3"; r[0].size = 128;
    CHECK(sh_vtp_place(only8, 1, r, 1) == 1 && r[0].node.square == 8 && sh_vtp_fits(&s8, r[0].node.x, r[0].node.y, 128));
    /* Square 16 first: two 256 blocks fill its lower half; square 8 has no 256 block clear of its rows. */
    both[0] = sq16; both[1] = &s8;
    memset(r, 0, sizeof r);
    r[0].key = "a"; r[0].size = 256;
    r[1].key = "b"; r[1].size = 256;
    r[2].key = "c"; r[2].size = 256;
    CHECK(sh_vtp_place(both, 2, r, 3) == 2 && r[0].node.square == 16 && r[1].node.square == 16 && !r[2].placed);
    memset(&mod, 0, sizeof mod);
    mod.key = "cccc3333@h3"; mod.m = &va; mod.node.square = 8; mod.node.x = 0; mod.node.y = 128; mod.node.size = 32;
    CHECK(sh_vtp_compose(&s8, &mod, 1, &m8, &n, why, sizeof why) && sh_vtp_open(&v8, m8, n, why, sizeof why) &&
          v8.square == 8 && v8.page_count > s8.page_count && sh_vtp_cell(&v8, 0, 0, 128) != SH_VTP_EMPTY);
    CHECK(sh_vtp_compose(sq16, &mod, 1, &m16, &n, why, sizeof why) && n == sq16->len && !memcmp(m16, sq16->data, n));
    t = sh_vtp_material_moved(g_mat, sizeof g_mat - 1, mod.node, &tl);
    CHECK(t && strstr(t, "virtualmapping\t{ 0.0039062500, 0.0039062500, 0.7539062500, 0.3281250000 }"));
    free(t);
    free(m8); free(m16); free(ma); free(b8);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    size_t ns;
    uint8_t *stock_bytes = stock_member(&ns);
    sh_vtp_member stock;
    char why[300];
    CHECK(sh_vtp_open(&stock, stock_bytes, ns, why, sizeof why) && stock.square == 16);
    dump(dir, "stock_sq16.mega2", stock_bytes, ns);
    test_member_checks();
    test_place(&stock);
    test_compose(dir, &stock);
    test_materials();
    test_second_square(&stock);
    free(stock_bytes);
    printf("vtpack_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
