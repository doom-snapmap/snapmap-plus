/* modvt_test.c -- mod virtual-texture families (modvt.c, pure half).
 *
 *   modvt_test.exe [<DOOMx64vk.exe> [<DOOMx64.exe>]]
 *
 * Bundles are built by the real builder and admitted by the real serving
 * index; the engine is a fake 32-slot record table. Checked: the member path
 * rule, which bundles register (one record per family, several families per
 * bundle), the index listener, rescan idempotence, release on removal, a
 * changed build, the hold after a registration pass, a full table (stock
 * records and record 31 untouched, one line), a name another record holds, a
 * record the engine took back, OpenGL without sparse page tables and a VT
 * system that is not up. With executables: VtSetSource and VtForMaterial
 * resolve on each, and the VT system, Release, the record table and (OpenGL)
 * the sparse globals decode to the recorded addresses. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modvt.h"
#include "../src/backend/signatures.h"
#include "../src/backend/host_image.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ------------------------------------------------------------------ log capture */

static char g_log[64][1024];
static int g_nlog;
static int g_echo_log;

void backend_log(const char *line)
{
    if (g_nlog < 64) strncpy_s(g_log[g_nlog++], sizeof g_log[0], line, _TRUNCATE);
    if (g_echo_log) printf("    | %s\n", line);
}

static int log_count(const char *needle)
{
    int i, n = 0;
    for (i = 0; i < g_nlog; ++i) n += strstr(g_log[i], needle) != NULL;
    return n;
}

/* ------------------------------------------------------------------ fake engine */

typedef struct fake {
    int ready, flavor;
    char name[SH_MODVT_SLOTS][SH_MODVT_NAME_MAX];
    char path[SH_MODVT_SLOTS][SH_MODVT_PATH_MAX];
    int fl[SH_MODVT_SLOTS];
    int sets, releases, refuse_set;
} fake;

static int f_ready(void *c) { return ((fake *)c)->ready; }
static int f_flavor(void *c) { return ((fake *)c)->flavor; }
static int f_find(void *c, const char *name)
{
    fake *f = (fake *)c;
    int k;
    for (k = 1; k < SH_MODVT_SLOTS; ++k) if (f->name[k][0] && !strcmp(f->name[k], name)) return k;
    return -1;
}
static int f_is_free(void *c, int k) { fake *f = (fake *)c; return !f->name[k][0] && !f->path[k][0]; }
static int f_set(void *c, int k, const char *name, const char *path, int flavor)
{
    fake *f = (fake *)c;
    f->sets++;
    if (k < 1 || k > 30 || f->name[k][0]) { printf("    fake: set_source on a used or forbidden record %d\n", k); return -1; }
    if (f->refuse_set) return 0;   /* the engine clears a record whose open fails */
    strcpy_s(f->name[k], sizeof f->name[k], name);
    strcpy_s(f->path[k], sizeof f->path[k], path);
    f->fl[k] = flavor;
    return 1;
}
static int f_release(void *c, int k)
{
    fake *f = (fake *)c;
    f->releases++;
    f->name[k][0] = 0;
    f->path[k][0] = 0;
    return 1;
}

static void fake_init(fake *f)
{
    memset(f, 0, sizeof *f);
    f->ready = 1;
    f->flavor = 2;
    strcpy_s(f->name[1], sizeof f->name[1], "_vmtr");
    strcpy_s(f->path[1], sizeof f->path[1], "virtualtextures/_vmtr.pages");
    strcpy_s(f->name[2], sizeof f->name[2], "maps/game/sp/shell/shell");      /* the hub's unique family */
    strcpy_s(f->path[2], sizeof f->path[2], "virtualtextures/maps/game/sp/shell/shell.pages");
    strcpy_s(f->name[3], sizeof f->name[3], "maps/modules/palettes/mega_blessed/mega");
    strcpy_s(f->path[3], sizeof f->path[3], "virtualtextures/maps/modules/palettes/mega_blessed/mega.pages");
}

/* ------------------------------------------------------------------ helpers */

static int sync_index(fake *f, const char *phase, sh_modvt_result *res)
{
    sh_modvt_family fams[SH_MODVT_MAX];
    sh_modvt_ops ops = { f, f_ready, f_flavor, f_find, f_is_free, f_set, f_release };
    int n = sh_modvt_collect(fams, SH_MODVT_MAX);
    return sh_modvt_sync(fams, n, &ops, phase, res);
}

static int stock_exists(void *ctx, const char *request) { (void)ctx; return strstr(request, "mods/") == NULL; }

static int build_folder(const char *folder, char *hash_out)
{
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX];
    int ok = sh_mb_build(folder, &r, &b) == 1 && b.errors == 0 && sh_mb_write(&b, folder, cause, sizeof cause) == 1;
    if (!ok) printf("    build %s: %u error(s): %s\n", folder, b.errors, b.errors ? b.first_cause : "");
    if (hash_out) strcpy_s(hash_out, 65, b.hash);
    sh_mb_free(&b);
    return ok;
}

static const char *build_cause(const char *folder, char *cause, size_t cap)
{
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_mb_bundle b;
    sh_mb_build(folder, &r, &b);
    strcpy_s(cause, cap, b.errors ? b.first_cause : "");
    sh_mb_free(&b);
    return cause;
}

static void put_mod(const char *folder, const char *id)
{
    char json[256];
    _snprintf_s(json, sizeof json, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"1.0.0\",\"name\":\"%s\",\"author\":\"t\"}", id, id);
    mt_write(folder, "mod.json", json);
}

static int g_listener_calls;
static void listener(void) { g_listener_calls++; }

/* ------------------------------------------------------------------ cases */

static void member_rule(void)
{
    char fam[SH_MODVT_NAME_MAX];
    int m = 0;
    printf("-- the member path rule\n");
    CHECK(sh_mb_vt_member("virtualtextures/mods/rctest/fam_sq1.mega2", fam, sizeof fam, &m) == 1 &&
          !strcmp(fam, "mods/rctest/fam") && m == 1);
    CHECK(sh_mb_vt_member("virtualtextures/mods/alex.pink-imp/skin/body_sq16.mega2", fam, sizeof fam, &m) == 1 &&
          !strcmp(fam, "mods/alex.pink-imp/skin/body") && m == 16);
    CHECK(sh_mb_vt_member("VirtualTextures/Mods/rctest/Fam_SQ3.MEGA2", NULL, 0, &m) == 1 && m == 3);
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest/fam_sq17.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest/fam_sq0.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest/fam_sq01.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest/fam.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest/_sq1.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest/fam_q1.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/fam_sq1.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/_vmtr_sq1.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest/my_vmtr_sq1.mega2", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("virtualtextures/mods/rctest/fam_sq1.pages", NULL, 0, NULL));
    CHECK(!sh_mb_vt_member("models/mods/rctest/fam_sq1.mega2", NULL, 0, NULL));
    CHECK(sh_mb_extension_allowed("virtualtextures/mods/rctest/fam_sq2.mega2"));
    CHECK(!sh_mb_extension_allowed("models/mods/rctest/fam_sq2.mega2"));
    CHECK(!sh_mb_extension_allowed("virtualtextures/mods/rctest/fam.pages"));
}

static void decode_helpers(void)
{
    /* SetSource +0x50 (VK) and +0x4E (GL): LEA RCX,[sys]; CALL; MOV RCX,RSI; CALL Release. */
    static const unsigned char vk[] = { 0x48, 0x89, 0xF1, 0x48, 0x8D, 0x0D, 0xB9, 0x5C, 0xE8, 0x04, 0xE8, 0x64, 0xAD,
                                        0xF8, 0xFF, 0x48, 0x8B, 0xCE, 0xE8, 0xEC, 0xE7, 0xFF, 0xFF, 0x48, 0x8D };
    static const unsigned char gl[] = { 0x48, 0x8D, 0x0D, 0x3B, 0xA4, 0x78, 0x03, 0xE8, 0x86, 0xAB, 0xF8, 0xFF, 0x48,
                                        0x8B, 0xCE, 0xE8, 0x6E, 0xE7, 0xFF, 0xFF };
    /* VirtualTextureForMaterial +0x61 (VK) / +0x81 (GL): MOV RAX,[R13+table+8]; JMP. */
    static const unsigned char tvk[] = { 0x75, 0x0C, 0x49, 0x8B, 0x85, 0x08, 0x8B, 0x00, 0x00, 0xE9, 0x38, 0x01 };
    static const unsigned char tgl[] = { 0x49, 0x8B, 0x85, 0x08, 0xA4, 0x00, 0x00, 0xE9, 0x38, 0x01, 0x00, 0x00 };
    static const unsigned char rel[] = { 0x48, 0x8B, 0xC4, 0x55, 0x90, 0x83, 0xB9, 0xC0, 0x02, 0x00, 0x00, 0x00 };
    printf("-- decoding helpers\n");
    CHECK(sh_modvt_find_setsource_head(vk, sizeof vk) == 3);
    CHECK(sh_modvt_find_setsource_head(gl, sizeof gl) == 0);
    CHECK(sh_modvt_find_setsource_head(gl, 10) == -1);
    CHECK(sh_modvt_find_table_disp(tvk, sizeof tvk) == 0x8B08u);
    CHECK(sh_modvt_find_table_disp(tgl, sizeof tgl) == 0xA408u);
    CHECK(sh_modvt_find_table_disp(tgl, 7) == 0);
    CHECK(sh_modvt_release_head_ok(rel, sizeof rel));
    CHECK(!sh_modvt_release_head_ok(rel, 8));
}

static void families(void)
{
    char root[MAX_PATH], mods[MAX_PATH], a[MAX_PATH], b[MAX_PATH], c[MAX_PATH], cause[SH_MB_CAUSE_MAX];
    char hash_a[65], hash_a2[65];
    sh_modvt_family fams[SH_MODVT_MAX];
    sh_modvt_result res;
    fake f;
    int n, k, sets, rec_fam, rec_other;

    printf("-- families from the serving index, registration and release with a fake engine\n");
    CHECK(mt_temp_dir("modvt", root, sizeof root));
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\vtmod", mods);
    _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\plain", mods);
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\badvt", root);
    CreateDirectoryA(mods, NULL);

    /* Builder refusals: a misplaced or misnamed member. */
    put_mod(c, "badvt");
    mt_write(c, "assets\\virtualtextures\\mods\\badvt\\fam.mega2", "MEGA");
    CHECK(strstr(build_cause(c, cause, sizeof cause), "file type not allowed") != NULL);
    mt_delete(c, "assets\\virtualtextures\\mods\\badvt\\fam.mega2");
    mt_write(c, "assets\\virtualtextures\\mods\\other\\fam_sq1.mega2", "MEGA");
    CHECK(strstr(build_cause(c, cause, sizeof cause), "/mods/badvt/") != NULL);
    mt_delete(c, "assets\\virtualtextures\\mods\\other\\fam_sq1.mega2");
    mt_write(c, "assets\\virtualtextures\\mods\\badvt\\mods\\badvt\\fam_sq1.mega2", "MEGA");
    CHECK(strstr(build_cause(c, cause, sizeof cause), "virtual-texture family member") == NULL);   /* nested family name */
    mt_remove_tree(c);

    /* vtmod: two families (fam: members 1 and 3; other: member 2). plain: none. */
    put_mod(a, "vtmod");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod\\fam_sq1.mega2", "member one");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod\\fam_sq3.mega2", "member three");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod\\other_sq2.mega2", "other two");
    put_mod(b, "plain");
    mt_write(b, "assets\\maps\\mods\\plain\\x.bin", "plain");
    CHECK(build_folder(a, hash_a));
    CHECK(build_folder(b, NULL));

    sh_modserve_set_index_listener(listener);
    CHECK(sh_modserve_index_build(mods) == 2);
    CHECK(g_listener_calls == 1);
    n = sh_modvt_collect(fams, SH_MODVT_MAX);
    CHECK(n == 2);
    CHECK(n == 2 && !strcmp(fams[0].name, "mods/vtmod/fam") && fams[0].members == 0x5u && fams[0].bytes == 22 &&
          !strcmp(fams[0].path, "virtualtextures/mods/vtmod/fam.pages") && !strcmp(fams[0].bundle, "vtmod@1.0.0") &&
          !strcmp(fams[0].bundle_hash, hash_a));
    CHECK(n == 2 && !strcmp(fams[1].name, "mods/vtmod/other") && fams[1].members == 0x2u);
    CHECK(sh_modvt_collect(NULL, 0) == 2);

    /* Not up yet: nothing is called. */
    fake_init(&f);
    f.ready = 0;
    sh_modvt_reset();
    CHECK(sync_index(&f, "boot", &res) == -1 && res.not_ready && f.sets == 0);

    /* Up: one record per family, first free records, flavor 2, type 2 path. */
    f.ready = 1;
    g_nlog = 0;
    CHECK(sync_index(&f, "index", &res) == 2 && res.registered == 2 && f.sets == 2);
    rec_fam = f_find(&f, "mods/vtmod/fam");
    rec_other = f_find(&f, "mods/vtmod/other");
    CHECK(rec_fam == 4 && rec_other == 5);
    CHECK(!strcmp(f.path[4], "virtualtextures/mods/vtmod/fam.pages") && f.fl[4] == 2);
    CHECK(sh_modvt_record_owner(4) && !strcmp(sh_modvt_record_owner(4), "mods/vtmod/fam"));
    CHECK(sh_modvt_record_owner(3) == NULL && sh_modvt_owned_count() == 2);
    CHECK(log_count("registered family 'mods/vtmod/fam' (vtmod@1.0.0, 2 members, 22 bytes) as record 4") == 1);

    /* Rescan: idempotent. */
    CHECK(sh_modserve_index_build(mods) == 2 && g_listener_calls == 2);
    CHECK(sync_index(&f, "index", &res) == 2 && res.kept == 2 && res.registered == 0 && f.sets == 2 && f.releases == 0);
    CHECK(sync_index(&f, "editor", &res) == 2 && f.sets == 2);

    /* A changed build of the same mod, no pass yet: released and registered again. */
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod\\fam_sq1.mega2", "member one, rebaked");
    CHECK(build_folder(a, hash_a2) && strcmp(hash_a, hash_a2) != 0);
    CHECK(sh_modserve_index_build(mods) == 2);
    CHECK(sync_index(&f, "index", &res) == 2 && res.released == 2 && res.registered == 2 && f.releases == 2 && f.sets == 4);
    CHECK(f_find(&f, "mods/vtmod/fam") == 4);

    /* Removal with no pass since registration: released. */
    mt_remove_tree(a);
    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(sh_modvt_collect(NULL, 0) == 0);
    g_nlog = 0;
    CHECK(sync_index(&f, "index", &res) == 0 && res.released == 2 && f.releases == 4);
    CHECK(f_find(&f, "mods/vtmod/fam") < 0 && f_is_free(&f, 4) && f_is_free(&f, 5));
    CHECK(log_count("released record 4, family 'mods/vtmod/fam'") == 1);

    /* Reinstall, a pass runs, then removal: held until exit, logged once. */
    put_mod(a, "vtmod");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod\\fam_sq1.mega2", "member one");
    CHECK(build_folder(a, NULL));
    CHECK(sh_modserve_index_build(mods) == 2);
    CHECK(sync_index(&f, "index", &res) == 1 && res.registered == 1);
    sh_modvt_mark_exposed();
    mt_remove_tree(a);
    CHECK(sh_modserve_index_build(mods) == 1);
    g_nlog = 0;
    sets = f.sets;
    CHECK(sync_index(&f, "index", &res) == 1 && res.held == 1 && res.released == 0 && f.releases == 4);
    CHECK(sync_index(&f, "editor", &res) == 1 && res.held == 1 && f.sets == sets);
    CHECK(log_count("is kept until DOOM exits") == 1);

    /* The engine took the record back: registered again in the same record. */
    put_mod(a, "vtmod");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod\\fam_sq1.mega2", "member one");
    CHECK(build_folder(a, NULL));
    CHECK(sh_modserve_index_build(mods) == 2);
    CHECK(sync_index(&f, "index", &res) == 1 && res.kept == 1);
    k = f_find(&f, "mods/vtmod/fam");
    CHECK(k == 4);
    f.name[k][0] = 0;
    f.path[k][0] = 0;
    strcpy_s(f.name[6], sizeof f.name[6], "maps/x/y");    /* something else took the next free record */
    strcpy_s(f.path[6], sizeof f.path[6], "virtualtextures/maps/x/y.pages");
    CHECK(sync_index(&f, "map", &res) == 1 && res.lost == 1 && res.registered == 1 && f_find(&f, "mods/vtmod/fam") == 4);
    f.name[6][0] = f.path[6][0] = 0;

    /* A name another record already holds is never taken over or released. */
    sh_modvt_reset();
    fake_init(&f);
    strcpy_s(f.name[7], sizeof f.name[7], "mods/vtmod/fam");
    strcpy_s(f.path[7], sizeof f.path[7], "virtualtextures/mods/vtmod/fam.pages");
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", &res) == 0 && res.refused == 1 && f.sets == 0);
    CHECK(sync_index(&f, "map", &res) == 0 && log_count("already holds that name") == 1);
    mt_remove_tree(a);
    CHECK(sh_modserve_index_build(mods) == 1);
    CHECK(sync_index(&f, "index", &res) == 0 && f.releases == 0 && f_find(&f, "mods/vtmod/fam") == 7);

    /* Records 1..30 full: refused once; no stock record and not record 31 touched. */
    put_mod(a, "vtmod");
    mt_write(a, "assets\\virtualtextures\\mods\\vtmod\\fam_sq1.mega2", "member one");
    CHECK(build_folder(a, NULL));
    CHECK(sh_modserve_index_build(mods) == 2);
    fake_init(&f);
    for (k = 4; k <= 30; ++k) {
        _snprintf_s(f.name[k], sizeof f.name[k], _TRUNCATE, "maps/stock/%d", k);
        _snprintf_s(f.path[k], sizeof f.path[k], _TRUNCATE, "virtualtextures/maps/stock/%d.pages", k);
    }
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", &res) == 0 && res.full == 1 && f.sets == 0 && f.releases == 0);
    CHECK(sync_index(&f, "map", &res) == 0 && res.full == 1 && log_count("are all in use") == 1);
    CHECK(f_is_free(&f, 31));
    /* A slot frees up: the family registers in it. */
    f.name[17][0] = f.path[17][0] = 0;
    CHECK(sync_index(&f, "map", &res) == 1 && f_find(&f, "mods/vtmod/fam") == 17);

    /* OpenGL without sparse page tables: never registered, said once. */
    sh_modvt_reset();
    fake_init(&f);
    f.flavor = 0;
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", &res) == 0 && res.refused == 1 && f.sets == 0);
    CHECK(sync_index(&f, "map", &res) == 0 && log_count("cannot be served on this renderer") == 1);
    /* OpenGL with them: flavor 4. */
    f.flavor = 4;
    CHECK(sync_index(&f, "map", &res) == 1 && f.fl[f_find(&f, "mods/vtmod/fam")] == 4);

    /* SetSource refused (no member opened): logged once, retried at later syncs. */
    sh_modvt_reset();
    fake_init(&f);
    f.refuse_set = 1;
    g_nlog = 0;
    CHECK(sync_index(&f, "editor", &res) == 0 && res.refused == 1 && f.sets == 1);
    CHECK(sync_index(&f, "map", &res) == 0 && f.sets == 2 && log_count("SetSource on record 4 refused") == 1);

    sh_modserve_set_index_listener(NULL);
    sh_modvt_reset();
    mt_remove_tree(root);
}

/* ------------------------------------------------------------------ executables */

static uint8_t *map_pe_by_rva(const char *path, size_t *image_sz)
{
    FILE *f = NULL;
    long fsz;
    uint8_t *file, *img;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int i;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
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
            sec[i].PointerToRawData + sec[i].SizeOfRawData <= (uint32_t)fsz)
            memcpy(img + sec[i].VirtualAddress, file + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    *image_sz = nt->OptionalHeader.SizeOfImage;
    free(file);
    return img;
}

static void image(const char *path)
{
    size_t sz = 0, total, i;
    uint8_t *base = map_pe_by_rva(path, &sz);
    wchar_t wide[MAX_PATH];
    const sh_build_identity *id;
    sig_result results[SIG_RESULTS_MAX];
    const sig_result *ss = NULL, *vf = NULL;
    sh_modvt_layout l;
    printf("-- %s\n", path);
    CHECK(base != NULL);
    if (!base) return;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    id = sh_host_test_bind_image(base, wide);
    CHECK(id != NULL);
    total = sig_db_count();
    sig_resolve_all(base, results, SIG_RESULTS_MAX);
    for (i = 0; i < total; ++i) {
        if (!strcmp(results[i].name, "VtSetSource")) ss = &results[i];
        if (!strcmp(results[i].name, "VtForMaterial")) vf = &results[i];
    }
    CHECK(ss && ss->status == SIG_OK && vf && vf->status == SIG_OK);
    if (!id || !ss || !vf || ss->status != SIG_OK || vf->status != SIG_OK) { free(base); return; }
    CHECK(ss->rva == (id->vulkan ? 0x185AC30u : 0x185BC90u));
    CHECK(vf->rva == (id->vulkan ? 0x17E8230u : 0x17E9100u));
    CHECK(sh_modvt_decode_layout(base + ss->rva, 0xC0, ss->rva, base + vf->rva, 0xC0, vf->rva, !id->vulkan, &l) == 1);
    CHECK(l.release_rva + 0x50 <= sz && sh_modvt_release_head_ok(base + l.release_rva, 0x50));
    printf("    %s: SetSource 0x%X, VirtualTextureForMaterial 0x%X, VT system 0x%X, Release 0x%X, table sys+0x%X, "
           "sparse ext 0x%X cvar 0x%X\n", id->vulkan ? "Vulkan" : "OpenGL", ss->rva, vf->rva, l.sys_rva, l.release_rva,
           l.table_off, l.sparse_ext_rva, l.sparse_cvar_rva);
    if (id->vulkan) {
        CHECK(l.sys_rva == 0x66E0940u && l.release_rva == 0x1859480u && l.table_off == 0x8B00u);
        CHECK(l.sparse_ext_rva == 0 && l.sparse_cvar_rva == 0);
    } else {
        CHECK(l.sys_rva == 0x4FE6120u && l.release_rva == 0x185A460u && l.table_off == 0xA400u);
        CHECK(l.sparse_cvar_rva == 0x4FE60B0u && l.sparse_ext_rva != 0 && l.sparse_ext_rva < sz);
    }
    free(base);
}

int main(int argc, char **argv)
{
    int i;
    g_echo_log = GetEnvironmentVariableA("MODVT_ECHO", NULL, 0) != 0;
    member_rule();
    decode_helpers();
    families();
    if (argc < 2) printf("[skip] the executables (pass DOOMx64vk.exe and DOOMx64.exe)\n");
    for (i = 1; i < argc; ++i) image(argv[i]);
    printf("modvt_test: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
