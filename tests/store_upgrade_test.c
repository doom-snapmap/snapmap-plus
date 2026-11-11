/* store_upgrade_test.c -- the store tool's readers of earlier store layouts:
 * the retired recycle bin swept (sh_ms_bin_sweep), the second upgrade step
 * over format-1 folders and a Contract 2 folder (sh_ms_upgrade), and the
 * reader of earlier mod.json shapes (modbundle_old.h).
 *
 *   store_upgrade_test.exe <testsixtures\modslex.r2-imp>
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modstore.h"
#include "modstore_upgrade.h"
#include "modbundle_old.h"
#include "mod_test_util.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)
#define GOLDEN_HASH "bc862690bd8d128d93ced2f8d95ef86afb8bbdabc5e36d83d8abedeebacbadee"

static unsigned g_logged;
static char g_log[64][600];
void backend_log(const char *message)
{
    strncpy_s(g_log[g_logged % 64], sizeof g_log[0], message, _TRUNCATE);
    g_logged++;
}
static int log_has(const char *needle)
{
    unsigned i, n = g_logged < 64 ? g_logged : 64;
    for (i = 0; i < n; i++) if (strstr(g_log[i], needle)) return 1;
    return 0;
}
static void log_dump(const char *what)
{
    unsigned i, n = g_logged < 64 ? g_logged : 64;
    fprintf(stderr, "  log (%s):\n", what);
    for (i = 0; i < n; i++) fprintf(stderr, "    %s\n", g_log[i]);
}

static void make_small_mod(const char *dir, const char *leaf)
{
    char text[256], rel[MAX_PATH];
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"name\":\"%s\",\"version\":\"1.0.0\",\"author\":\"tester\"}", leaf);
    mt_write(dir, "mod.json", text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\a.bimage", leaf);
    _snprintf_s(text, sizeof text, _TRUNCATE, "bytes of %s for the store test", leaf);
    mt_write(dir, rel, text);
}

static char g_sweep[4096];
static int g_sweep_lines;
static void sweep_line(void *context, const char *line)
{
    (void)context;
    g_sweep_lines++;
    strncat_s(g_sweep, sizeof g_sweep, line, _TRUNCATE);
    strncat_s(g_sweep, sizeof g_sweep, "\n", _TRUNCATE);
}

static void test_bin_sweep(const char *tmp)
{
    char root[MAX_PATH], bin[MAX_PATH], keep[MAX_PATH], outside[MAX_PATH], link[MAX_PATH], *rec = NULL;
    printf("-- store tool: the retired bin is swept\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\sweep\\mods", tmp);
    _snprintf_s(bin, sizeof bin, _TRUNCATE, "%s\\sweep\\removed-mods", tmp);
    _snprintf_s(outside, sizeof outside, _TRUNCATE, "%s\\sweep\\outside", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(keep, sizeof keep, _TRUNCATE, "%s\\kept", root);
    make_small_mod(keep, "kept");
    {
        char a[MAX_PATH], b[MAX_PATH], c[MAX_PATH];
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\alex.one", bin);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\alex.two-2", bin);
        _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\stray", bin);
        mt_write(a, "mod.json", "{\"id\":\"alex.one\"}");
        mt_write(b, "mod.json", "{\"id\":\"alex.two\"}");
        mt_write(c, "notes.txt", "moved here by hand");
        mt_write(outside, "precious.txt", "not the bin's");
        _snprintf_s(link, sizeof link, _TRUNCATE, "cmd /c mklink /J \"%s\\link\" \"%s\" >nul 2>&1", a, outside);
        (void)system(link);
    }
    CHECK(sh_mb_record_put(root, "removed", "{\"alex.one\":{\"id\":\"alex.one\",\"hashes\":[]}}"));
    g_sweep[0] = 0;
    g_sweep_lines = 0;
    CHECK(sh_ms_bin_sweep(sweep_line, NULL) == 3);
    CHECK(strstr(g_sweep, "MODS: removed-mods: 3 folder(s) deleted; the bin is retired\n") && g_sweep_lines == 4);
    CHECK(!mt_exists(bin));
    _snprintf_s(link, sizeof link, _TRUNCATE, "%s\\precious.txt", outside);
    CHECK(mt_exists(link));
    CHECK(sh_mb_record_get(root, "removed", &rec) != 1 || !rec);
    free(rec);
    g_sweep[0] = 0;
    g_sweep_lines = 0;
    CHECK(sh_ms_bin_sweep(sweep_line, NULL) == 0 && g_sweep_lines == 0);
}

/* A format-1 folder as an earlier release wrote it. */
static void old_folder(const char *root, const char *leaf, const char *key, const char *origin, const char *bundle, const char *legacy)
{
    char dir[MAX_PATH * 2], text[1400], rel[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, leaf);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\%s\\a.decl", key);
    mt_write(dir, rel, "{\r\n}");
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\"name\":\"%s\",\"version\":\"1.0.0\",\"snapmap-plus\":{\"format\":1,\"key\":\"%s\",\"build\":1,"
                "\"bundle\":\"%s\",\"origin\":\"%s\"%s%s%s,\"files\":[{\"path\":\"assets/generated/decls/entitydef/mods/%s/a.decl\","
                "\"sha256\":\"%s\",\"size\":3}]}}",
                leaf, key, bundle, origin, legacy ? ",\"legacy\":" : "", legacy ? legacy : "", "", key, bundle);
    mt_write(dir, "mod.json", text);
}

static void test_upgrade_steps(const char *tmp)
{
    char root[MAX_PATH], path[MAX_PATH * 2], *rec = NULL;
    const char *A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    sh_ms_upgrade_stats us;
    sh_ms_upgrade_check uc;
    printf("-- store tool: the second step over format-1 folders; a Contract 2 folder this release cannot read\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\upgrade\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    old_folder(root, "alex.imp", "alex.imp", "install", A, NULL);
    old_folder(root, "alex.imp@legacy-12345678", "alex.imp", "delivery", B, "{\"key\":\"delivery." "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}");
    old_folder(root, "alex.solo@1.0.0", "alex.solo", "install", A, NULL);
    CHECK(sh_ms_upgrade_scan(&uc) >= 1 && uc.tidy_drops == 1 && uc.tidy_renames == 1);
    g_logged = 0;
    CHECK(sh_ms_upgrade(&us) == 1 && us.tidied && us.same_id == 1 && us.tidy_renamed == 1 && !us.tidy_failed);
    if (!us.same_id || !us.tidy_renamed) log_dump("tidy");
    CHECK(!mt_has(root, "alex.imp@legacy-12345678") && mt_has(root, "alex.solo\\mod.json") && mt_has(root, "alex.imp\\mod.json"));
    CHECK(sh_mb_record_get(root, "layout", &rec) == 1 && rec && !strcmp(rec, "2"));
    free(rec);
    CHECK(sh_ms_upgrade(&us) == 0);   /* once */
    /* Contract 2: a bundle.json becomes a format-1 mod.json, the shape the
     * store tool's conversion to the one shape reads next; one without an id
     * is left as it was; the product files still fold into the record. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.c2@1.0.0+1", root);
    mt_write(path, "bundle.json", "{\"format\":1,\"id\":\"alex.c2\",\"version\":\"1.0.0\",\"build\":1,\"files\":[]}");
    {
        char bad[MAX_PATH];
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\noid@1.0.0+1", root);
        mt_write(bad, "bundle.json", "{\"format\":1,\"version\":\"1.0.0\",\"build\":1,\"files\":[]}");
    }
    mt_write(root, "claims.json", "{\"claims\":{\"x\":{\"ref\":\"1\"}}}");
    g_logged = 0;
    CHECK(sh_ms_upgrade(&us) == 1 && us.ran && us.failed == 1 && us.records == 1);
    CHECK(log_has("noid@1.0.0+1: left as it was") && !mt_has(root, "claims.json"));
    CHECK(log_has("alex.c2@1.0.0+1: mod.json written for alex.c2@1.0.0+1"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.c2", root);
    CHECK(mt_has(path, "mod.json") && !mt_has(path, "bundle.json") && sh_mb_old_is_old(path));
    rec = NULL;
    CHECK(sh_mb_record_get(root, "claims", &rec) == 1 && rec && strstr(rec, "\"ref\""));
    free(rec);
}

/* The store tool's reader of earlier shapes (modbundle_old.h): a format-1
 * block with an author id, a never-built folder, package.json. */
static void test_old_reader(const char *fixture, const char *tmp)
{
    char folder[MAX_PATH];
    sh_mb_old o;
    size_t i;
    int saw_pointer = 0;
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\old-shape", tmp);
    CHECK(mt_copy_tree(fixture, folder));
    mt_write(folder, "mod.json",
             "{\"id\":\"alex.r2-imp\",\"version\":\"1.0.0\",\"name\":\"R2 Imp copy\",\"shadows\":[\"a/b.decl\"],"
             "\"strings\":{\"en\":{\"#str_x\":\"X\"}},\"hud\":{\"weapons\":{}},"
             "\"snapmap-plus\":{\"format\":1,\"key\":\"other.key\",\"build\":3,\"bundle\":\"" GOLDEN_HASH "\","
             "\"built\":1700000000,\"validated\":false,\"origin\":\"migration\",\"rules\":7,"
             "\"legacy\":{\"source\":\"overrides\"},\"shadows\":[\"a/b.decl\"],"
             "\"files\":[{\"path\":\"assets/x.decl\",\"sha256\":\"" GOLDEN_HASH "\",\"size\":12}],"
             "\"stock\":[{\"path\":\"assets/y.decl\",\"sha256\":\"" GOLDEN_HASH "\",\"source\":\"y.decl\",\"inAssets\":false}]}}");
    CHECK(sh_mb_old_is_old(folder));
    CHECK(sh_mb_old_load(folder, &o));
    CHECK(!strcmp(o.descriptor, "mod.json") && o.block_format == 1);
    /* The author id won over the block's key before. */
    CHECK(!strcmp(o.key, "alex.r2-imp") && o.key_from == SH_MB_OLD_KEY_FROM_ID);
    CHECK(o.name && !strcmp(o.name, "\"R2 Imp copy\"") && o.version && !strcmp(o.version, "\"1.0.0\""));
    CHECK(o.kept && strstr(o.kept, "\"strings\"") && strstr(o.kept, "\"hud\"") && !strstr(o.kept, "\"id\""));
    CHECK(o.retired && strstr(o.retired, "\"id\":\"alex.r2-imp\"") && strstr(o.retired, "\"shadows\""));
    CHECK(o.build == 3 && o.built == 1700000000 && o.rules == 7 && !o.validated && !strcmp(o.origin, "migration"));
    CHECK(!strcmp(o.bundle, GOLDEN_HASH) && o.block_legacy && strstr(o.block_legacy, "overrides") && o.block_shadows);
    CHECK(o.file_count == 2);
    for (i = 0; i < o.file_count; i++)
        if (o.files[i].pointer) {
            saw_pointer = 1;
            CHECK(!strcmp(o.files[i].path, "assets/y.decl") && o.files[i].absent && o.files[i].source &&
                  !strcmp(o.files[i].source, "y.decl"));
        } else {
            CHECK(!strcmp(o.files[i].path, "assets/x.decl") && o.files[i].size == 12);
        }
    CHECK(saw_pointer);
    sh_mb_old_free(&o);
    /* The block's key when there is no author id; the folder's name when
     * neither (a folder never built: its files are walked). */
    mt_write(folder, "mod.json", "{\"name\":\"x\",\"snapmap-plus\":{\"format\":1,\"key\":\"other.key\"}}");
    CHECK(sh_mb_old_load(folder, &o) && !strcmp(o.key, "other.key") && o.key_from == SH_MB_OLD_KEY_FROM_BLOCK);
    sh_mb_old_free(&o);
    mt_delete(folder, "mod.json");
    mt_write(folder, "package.json", "{\"name\":\"x\",\"version\":\"2\"}");
    CHECK(sh_mb_old_is_old(folder));
    CHECK(sh_mb_old_load(folder, &o) && !strcmp(o.descriptor, "package.json") && !strcmp(o.key, "old-shape") &&
          o.key_from == SH_MB_OLD_KEY_FROM_FOLDER && o.file_count == 13 && !o.block_format);
    sh_mb_old_free(&o);
    /* The current shape is not old. */
    CHECK(!sh_mb_old_is_old(fixture));
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], fixture[MAX_PATH];
    if (!GetFullPathNameA(argc > 1 ? argv[1] : "fixtures\\mods\\alex.r2-imp", sizeof fixture, fixture, NULL)) return 2;
    if (!mt_temp_dir("store_upgrade", tmp, sizeof tmp)) { fprintf(stderr, "no temp dir\n"); return 2; }
    test_bin_sweep(tmp);
    test_upgrade_steps(tmp);
    test_old_reader(fixture, tmp);
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    if (failures) { fprintf(stderr, "store_upgrade_test: %d failure(s)\n", failures); return 1; }
    printf("store_upgrade_test: the retired bin, the second upgrade step and the earlier mod.json reader pass\n");
    return 0;
}
