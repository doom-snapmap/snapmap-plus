/* modstore_test.c -- the mods\ store (Contract 5): install as mods\<id> with
 * real copies, enumeration, verification recorded in mod.json, detected
 * corruption, folder names for a second version, the rebuild rule (tree hash
 * and build cache), resolve_need hit, miss, dedupe and conflict, the store
 * lock (a second installer waits, two installers at once both succeed), and
 * the upgrade of the retired layout (bundle.json, .state.json, blobs\ with
 * hard links, legacy\, the product files) to mod.json folders.
 *
 *   modstore_test.exe <tests\fixtures\mods\alex.r2-imp>
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modstore.h"
#include "mod_test_util.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

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

static const char *const STOCK[] = {
    "generated/decls/material/textures/guis/snapmaps/entity_icons/demons/imp_enc.decl",
    "generated/decls/entitydef/ai/demon/imp_coop.decl",
    "generated/decls/renderprog/skinvmtr.decl",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/rage_taunt.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/meleeforward01.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/meleeforward02.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/idle.bmd6anim",
    "generated/decls/entitydef/monster/imp.decl",
    NULL
};
static int stock_exists(void *ctx, const char *request)
{
    int i;
    (void)ctx;
    for (i = 0; STOCK[i]; i++) if (!strcmp(STOCK[i], request)) return 1;
    return 0;
}
static const sh_mb_resolver RESOLVER = { NULL, NULL, stock_exists, NULL };

static DWORD links_of(const char *path)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (GetFileInformationByHandle(h, &info)) n = info.nNumberOfLinks;
    CloseHandle(h);
    return n;
}

static const sh_ms_entry *find_entry(const sh_ms_list *l, const char *name)
{
    size_t i;
    for (i = 0; i < l->count; i++) if (!strcmp(l->items[i].name, name)) return &l->items[i];
    return NULL;
}

static char *slurp(const char *folder, const char *rel, size_t *len)
{
    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    return (char *)sh_mb_read_file(path, 16u << 20, len);
}

/* ------------------------------------------------------------ lock contention */

typedef struct lock_job {
    char folder[MAX_PATH * 2];
    int ok;
    ULONGLONG done_at;
    char cause[512];
} lock_job;

static DWORD WINAPI install_thread(void *p)
{
    lock_job *j = (lock_job *)p;
    sh_ms_install_stats st;
    char key[160];
    j->ok = sh_ms_install_from_folder(j->folder, &RESOLVER, key, sizeof key, &st, NULL, j->cause, sizeof j->cause);
    j->done_at = GetTickCount64();
    return 0;
}

typedef struct hold_job { DWORD ms; volatile LONG held; ULONGLONG released_at; } hold_job;

/* Stands for a migration folder: holds the store lock for a while. */
static DWORD WINAPI hold_thread(void *p)
{
    hold_job *h = (hold_job *)p;
    modstore_lock();
    modstore_lock();   /* recursive, as a migration step around an install is */
    InterlockedExchange(&h->held, 1);
    Sleep(h->ms);
    h->released_at = GetTickCount64();
    modstore_unlock();
    modstore_unlock();
    return 0;
}

static void make_small_mod(const char *dir, const char *id)
{
    char text[256], rel[MAX_PATH];
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"1.0.0\",\"name\":\"%s\",\"author\":\"tester\"}", id, id);
    mt_write(dir, "mod.json", text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\a.bimage", id);
    _snprintf_s(text, sizeof text, _TRUNCATE, "bytes of %s for the lock test", id);
    mt_write(dir, rel, text);
}

/* ------------------------------------------------------------ the retired layout */

/* Lay out `src` the way the retired store did: blobs\<sha256> (read-only),
 * <folder>\bundle.json, bundle.canonical.json, .state.json and assets\
 * hard-linked to the blobs (with_links) or copied. `descriptor` keeps the
 * folder's own mod.json (a folder built in place). Returns the bundle hash. */
static void old_layout(const char *root, const char *src, const char *folder, const char *extra_desc, int with_links,
                       int descriptor, int validated, char hash_out[65])
{
    sh_mb_bundle b;
    char dir[MAX_PATH * 2], path[MAX_PATH * 2], blob[MAX_PATH * 2], state[200];
    size_t i, n = 0;
    char *pretty;
    if (extra_desc) mt_write(src, "mod.json", extra_desc);
    CHECK(sh_mb_build(src, &RESOLVER, &b));
    if (b.errors) fprintf(stderr, "  old layout build: %s\n", b.first_cause);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, folder);
    CreateDirectoryA(root, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\blobs", root);
    CreateDirectoryA(path, NULL);
    mt_mkdirs_for(dir);
    CreateDirectoryA(dir, NULL);
    for (i = 0; i < b.file_count; i++) {
        if (b.files[i].stock) continue;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", src, b.files[i].path);
        _snprintf_s(blob, sizeof blob, _TRUNCATE, "%s\\blobs\\%s", root, b.files[i].blob);
        for (char *q = path; *q; q++) if (*q == '/') *q = '\\';
        if (GetFileAttributesA(blob) == INVALID_FILE_ATTRIBUTES) {
            CopyFileA(path, blob, FALSE);
            SetFileAttributesA(blob, FILE_ATTRIBUTE_READONLY);
        }
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, b.files[i].path);
        for (char *q = path; *q; q++) if (*q == '/') *q = '\\';
        mt_mkdirs_for(path);
        if (!(with_links && CreateHardLinkA(path, blob, NULL))) CopyFileA(blob, path, TRUE);
    }
    pretty = sh_mb_bundle_json(&b, &n);
    mt_write(dir, "bundle.json", pretty ? pretty : "");
    free(pretty);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.canonical.json", dir);
    {
        FILE *f = NULL;
        if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(b.canonical, 1, b.canonical_len, f); fclose(f); }
    }
    _snprintf_s(state, sizeof state, _TRUNCATE, "{\"firstCause\":\"%s\",\"installedAt\":1793344545,\"validated\":%s}\n",
                validated ? "" : "edited", validated ? "true" : "false");
    mt_write(dir, ".state.json", state);
    if (descriptor) {
        char *desc = slurp(src, "mod.json", NULL);
        mt_write(dir, "mod.json", desc ? desc : "{}");
        free(desc);
    }
    strcpy_s(hash_out, 65, b.hash);
    sh_mb_free(&b);
}

static void test_upgrade(const char *fixture, const char *tmp)
{
    char root[MAX_PATH], src[MAX_PATH], src2[MAX_PATH], h_main[65], h_v2[65], h_legacy_same[65], h_legacy_alias[65],
         h_legacy_other[65], h_hand[65], h_mig[65], path[MAX_PATH * 2];
    sh_ms_upgrade_stats us;
    sh_ms_list list;
    const sh_ms_entry *e;
    char *text;
    static const char base[] = "{\"id\":\"alex.r2-imp\",\"version\":\"1.0.0\",\"name\":\"R2 Imp copy\",\"author\":\"alex\","
                               "\"strings\":{\"english\":{\"#str_mod_alex_r2_imp\":\"Imp copy\"}}";
    char desc[1024];
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\upgrade\\mods", tmp);
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\upgrade-src\\one", tmp);
    _snprintf_s(src2, sizeof src2, _TRUNCATE, "%s\\upgrade-src\\two", tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\upgrade", tmp);
    CreateDirectoryA(path, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\upgrade-src", tmp);
    CreateDirectoryA(path, NULL);
    CHECK(mt_copy_tree(fixture, src));
    CHECK(sh_ms_set_root(root));

    /* A laid-out build with hard links, a second version of the same id, a
     * delivered copy of the same content (legacy\, same hash: dropped), a
     * delivered copy with only its legacy record different (alias), a
     * delivered package of another id, a folder built in place by hand with
     * its own descriptor, and a migrated overrides pack. */
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "%s}", base);
    old_layout(root, src, "alex.r2-imp@1.0.0+1", desc, 1, 0, 1, h_main);
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "%s}", base);
    desc[strstr(desc, "1.0.0") - desc + 4] = '1';   /* 1.0.1 */
    old_layout(root, src, "alex.r2-imp@1.0.1+1", desc, 0, 0, 1, h_v2);
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "%s}", base);
    old_layout(root, src, "legacy\\alex.r2-imp@0123456789abcdef", desc, 1, 1, 1, h_legacy_same);
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "%s,\"legacy\":{\"digest16\":\"fedcba9876543210\",\"key\":\"delivery.%064d\","
                "\"keyForm\":\"delivery\"}}", base, 7);
    old_layout(root, src, "legacy\\alex.r2-imp@fedcba9876543210", desc, 0, 1, 1, h_legacy_alias);
    mt_write(src2, "mod.json", "{\"id\":\"bob.thing\",\"version\":\"legacy-1111111111111111\",\"name\":\"Bob\","
                               "\"legacy\":{\"digest16\":\"1111111111111111\",\"key\":\"delivery.x\",\"keyForm\":\"delivery\"},"
                               "\"shadows\":[\"generated/decls/entitydef/monster/imp.decl\"]}");
    mt_write(src2, "assets\\generated\\decls\\entitydef\\monster\\imp.decl", "{\r\n}");
    old_layout(root, src2, "legacy\\bob.thing@1111111111111111", NULL, 0, 1, 1, h_legacy_other);
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "{\"id\":\"alex.hand\",\"name\":\"Hand made\"}");
    {
        char hand_src[MAX_PATH];
        _snprintf_s(hand_src, sizeof hand_src, _TRUNCATE, "%s\\upgrade-src\\hand", tmp);
        mt_write(hand_src, "assets\\generated\\decls\\entitydef\\mods\\alex.hand\\x.decl", "{\r\n}");
        old_layout(root, hand_src, "my hand folder", "{\r\n  \"id\": \"alex.hand\",\r\n  \"name\": \"Hand made\"\r\n}\r\n",
                   0, 1, 0, h_hand);
        mt_write(hand_src, "mod.json", "{\"id\":\"local.mig\",\"version\":\"0.0.0\",\"name\":\"Migrated\","
                                       "\"legacy\":{\"source\":\"overrides\",\"folder\":\"mig\",\"tree\":\"abc\"}}");
        mt_remove_tree(hand_src);
        mt_write(hand_src, "assets\\generated\\decls\\entitydef\\mods\\local.mig\\x.decl", "{\r\n}");
        old_layout(root, hand_src, "local.mig@0.0.0+1",
                   "{\"id\":\"local.mig\",\"version\":\"0.0.0\",\"name\":\"Migrated\","
                   "\"legacy\":{\"source\":\"overrides\",\"folder\":\"mig\",\"tree\":\"abc\"}}", 1, 0, 1, h_mig);
    }
    mt_write(root, "claims.json", "{\"format\":1,\"claims\":{\"map-1\":{\"claim\":\"c1\",\"secret\":\"s1\",\"bundles\":[]}}}");
    mt_write(root, "publish-links.json", "{\"format\":1,\"links\":{\"item-1\":{\"cloud\":\"c\",\"local\":\"l\",\"at\":1}}}");
    mt_write(root, "pending-uploads.json", "{\"format\":1,\"pending\":{}}");
    mt_write(root, ".migrated.json", "{\n  \"format\": 1,\n  \"complete\": true,\n  \"migratedAt\": 1,\n  \"folders\": [\n"
                                     "    {\"folder\": \"mig\", \"kind\": \"package\", \"outcome\": \"built\", \"store\": \"local.mig@0.0.0+1\"}],\n"
                                     "  \"trees\": {\"abc\": \"local.mig@0.0.0+1\", \"def\": \"legacy/alex.r2-imp@fedcba9876543210\"}\n}\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp@1.0.0+1\\assets\\generated\\decls\\md6def\\mods\\alex.r2-imp\\imp_copy.md6.decl", root);
    CHECK(links_of(path) >= 2);

    g_logged = 0;
    CHECK(sh_ms_upgrade(&us) == 1);
    CHECK(us.ran && us.folders == 4 && us.failed == 0);
    CHECK(us.dropped == 2 && us.delivered == 1);
    CHECK(us.unlinked >= 13 && us.blobs_removed && us.records == 4);
    if (us.folders != 4 || us.dropped != 2 || us.delivered != 1 || us.failed)
        fprintf(stderr, "  upgrade: %u folders, %u dropped, %u delivered, %u failed, %u renamed\n", us.folders, us.dropped,
                us.delivered, us.failed, us.renamed);
    CHECK(log_has("MODS: store: alex.r2-imp@1.0.0+1: mod.json written for alex.r2-imp@1.0.0+1"));
    CHECK(log_has("MODS: store: alex.r2-imp@1.0.0+1 -> alex.r2-imp@1.0.0"));   /* two versions of one id */
    CHECK(log_has("MODS: store: legacy\\bob.thing@1111111111111111 -> bob.thing"));
    CHECK(log_has("MODS: store: upgraded to the mod.json layout"));
    CHECK(!mt_has(root, "blobs") && !mt_has(root, "legacy") && !mt_has(root, "claims.json") &&
          !mt_has(root, "publish-links.json") && !mt_has(root, "pending-uploads.json") && !mt_has(root, ".migrated.json"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp@1.0.0\\assets\\generated\\decls\\md6def\\mods\\alex.r2-imp\\imp_copy.md6.decl", root);
    CHECK(links_of(path) == 1);
    CHECK(!(GetFileAttributesA(path) & FILE_ATTRIBUTE_READONLY));
    CHECK(sh_ms_enumerate(&list) && list.count == 5);
    e = find_entry(&list, "alex.r2-imp@1.0.0");
    CHECK(e && sh_ms_entry_usable(e) && !strcmp(e->bundle.hash, h_main) && !strcmp(e->bundle.origin, "install"));
    CHECK(e && e->installed_at == 1793344545);
    e = find_entry(&list, "alex.r2-imp@1.0.1");
    CHECK(e && sh_ms_entry_usable(e) && !strcmp(e->bundle.hash, h_v2));
    e = find_entry(&list, "bob.thing");
    CHECK(e && sh_ms_entry_usable(e) && !strcmp(e->bundle.hash, h_legacy_other) && e->delivered && !strcmp(e->bundle.origin, "delivery"));
    e = find_entry(&list, "my hand folder");
    CHECK(e && e->loaded && !e->validated && !strcmp(e->bundle.hash, h_hand) && !e->bundle.origin[0]);   /* its state kept */
    e = find_entry(&list, "local.mig");
    CHECK(e && sh_ms_entry_usable(e) && !strcmp(e->bundle.hash, h_mig) && !strcmp(e->bundle.origin, "migration"));
    sh_ms_list_free(&list);
    /* The hand-made folder's author block is its own bytes. */
    text = slurp(root, "my hand folder\\mod.json", NULL);
    CHECK(text && !strncmp(text, "{\r\n  \"id\": \"alex.hand\",\r\n  \"name\": \"Hand made\",\r\n\r\n  \"snapmap-plus\": {", 60));
    free(text);
    /* A converted package's legacy record and shadows are in its product block. */
    text = slurp(root, "bob.thing\\mod.json", NULL);
    CHECK(text && strstr(text, "\"legacy\"") && !strstr(strstr(text, "\"legacy\"") + 1, "\"legacy\""));   /* once: the author's */
    free(text);
    /* The record: claims, links, pending, migration (names updated), cache, aliases. */
    {
        char *section = NULL;
        CHECK(sh_mb_record_get(root, "claims", &section) == 1 && section && strstr(section, "\"map-1\""));
        free(section);
        CHECK(sh_mb_record_get(root, "links", &section) == 1 && section && strstr(section, "\"item-1\""));
        free(section);
        CHECK(sh_mb_record_get(root, "migration", &section) == 1 && section && strstr(section, "\"abc\":\"local.mig\"") &&
              strstr(section, "\"def\":\"alex.r2-imp@1.0.0\"") && strstr(section, "\"store\":\"local.mig\""));
        free(section);
        CHECK(sh_mb_record_get(root, "aliases", &section) == 1 && section && strstr(section, h_legacy_alias) &&
              strstr(section, h_main));
        free(section);
        CHECK(sh_mb_record_get(root, "cache", &section) == 1 && section && strstr(section, h_main) && strstr(section, h_hand));
        free(section);
    }
    /* Idempotent: a second pass finds nothing to do. */
    CHECK(sh_ms_upgrade(&us) == 0);
    /* Interrupted after mod.json was written: the leftovers go on the next pass. */
    mt_write(root, "local.mig\\bundle.canonical.json", "{}");
    CHECK(sh_ms_upgrade(&us) == 1 && us.failed == 0 && !mt_has(root, "local.mig\\bundle.canonical.json"));
    CHECK(sh_ms_enumerate(&list) && list.count == 5);
    sh_ms_list_free(&list);
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], root[MAX_PATH], src[MAX_PATH], path[MAX_PATH * 2], key[160], cause[512] = "", fixture[MAX_PATH];
    char hand[MAX_PATH];
    sh_ms_install_stats st;
    sh_ms_verify_stats vs;
    sh_ms_sync_stats ss;
    sh_ms_list list;
    sh_ms_hit hit;
    sh_mb_bundle b;
    const sh_ms_entry *e;
    char h1[65];

    if (!GetFullPathNameA(argc > 1 ? argv[1] : "fixtures\\mods\\alex.r2-imp", sizeof fixture, fixture, NULL)) return 2;
    if (!mt_temp_dir("modstore", tmp, sizeof tmp)) { fprintf(stderr, "no temp dir\n"); return 2; }
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\store\\mods", tmp);
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\authoring\\alex.r2-imp", tmp);
    CreateDirectoryA(tmp, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\authoring", tmp);
    CreateDirectoryA(path, NULL);
    CHECK(mt_copy_tree(fixture, src));
    CHECK(sh_ms_set_root(root));
    {
        char got[MAX_PATH];
        CHECK(sh_ms_root(got, sizeof got) && !strcmp(got, root));
        CHECK(sh_ms_set_root(NULL) && sh_ms_root(got, sizeof got) && strstr(got, "\\snapmap-plus\\mods"));
        CHECK(sh_ms_set_root(root));
    }

    /* An empty or absent store lists nothing and needs no upgrade. */
    CHECK(sh_ms_enumerate(&list) && list.count == 0);
    sh_ms_list_free(&list);
    CHECK(sh_ms_upgrade(NULL) == 0);

    /* Install from outside the store: mods\<id>, real copies, the source untouched. */
    cause[0] = 0;
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    if (b.errors || cause[0]) fprintf(stderr, "  install: %s\n", b.errors ? b.first_cause : cause);
    CHECK(!strcmp(key, "alex.r2-imp"));
    CHECK(st.copied == 13 && !st.in_place && !st.replaced);
    CHECK(!mt_has(src, "bundle.json") && !strstr(slurp(src, "mod.json", NULL), "snapmap-plus"));   /* the source is not written */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp", root);
    CHECK(mt_has(path, "mod.json") && !mt_has(path, "bundle.json") && !mt_has(path, ".state.json") && !mt_has(root, "blobs"));
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp\\assets\\generated\\decls\\md6def\\mods\\alex.r2-imp\\imp_copy.md6.decl", root);
    CHECK(links_of(path) == 1);
    CHECK(!(GetFileAttributesA(path) & FILE_ATTRIBUTE_READONLY));
    strcpy_s(h1, sizeof h1, b.hash);
    sh_mb_free(&b);

    /* Enumeration reads mod.json and checks sizes. */
    CHECK(sh_ms_enumerate(&list) && list.count == 1);
    e = find_entry(&list, "alex.r2-imp");
    CHECK(e && e->loaded && e->sizes_ok && e->validated && sh_ms_entry_usable(e) && e->installed_at > 0);
    CHECK(e && e->bundle.file_count == 13 && e->bundle.build == 1 && !strcmp(e->key, "alex.r2-imp@1.0.0+1"));
    CHECK(e && !strcmp(e->bundle.origin, "install") && !e->delivered);
    CHECK(e && sh_ms_entry_matches(e, "alex.r2-imp") && sh_ms_entry_matches(e, "ALEX.R2-IMP@1.0.0") &&
          sh_ms_entry_matches(e, "alex.r2-imp@1.0.0+1") && !sh_ms_entry_matches(e, "alex.r2-imp@1.0.1"));

    /* resolve_need: by logical name, by derived request path, and a miss. */
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/alex.r2-imp/imp_copy.md6", &hit) == SH_MS_HIT && hit.providers == 1);
    CHECK(sh_ms_resolve_need(&list, "MD6DEF", "Mods/Alex.R2-Imp/Imp_Copy.md6", &hit) == SH_MS_HIT);
    CHECK(sh_ms_resolve_need(&list, "baseModel", "md6/mods/alex.r2-imp/imp_copy/assets/mesh/imp_copy.md6mesh", &hit) == SH_MS_HIT);
    CHECK(!strcmp(list.items[hit.entry].bundle.files[hit.file].ptype, "baseModel"));
    CHECK(sh_ms_resolve_need(&list, "anim", "md6/mods/alex.r2-imp/imp_copy/motion/idle.md6anim", &hit) == SH_MS_HIT);
    CHECK(sh_ms_resolve_need(&list, "image", "mods/alex.r2-imp/imp_icon.tga", &hit) == SH_MS_HIT);
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/nobody/x.md6", &hit) == SH_MS_MISS);
    CHECK(sh_ms_resolve_need(&list, "entityDef", "mods/alex.r2-imp/imp_copy.md6", &hit) == SH_MS_MISS);
    CHECK(sh_ms_request_exists(&list, "generated/decls/entitydef/mods/alex.r2-imp/imp_copy.decl") == 1);
    CHECK(sh_ms_request_exists(&list, "generated/decls/entitydef/mods/alex.r2-imp/none.decl") == 0);
    sh_ms_list_free(&list);

    /* Verification passes on the fresh install. */
    CHECK(sh_ms_verify("alex.r2-imp", &vs, cause, sizeof cause));
    CHECK(vs.entries == 1 && vs.files == 13);
    CHECK(!sh_ms_verify("alex.r2-imp@9.9.9", &vs, cause, sizeof cause) && strstr(cause, "no installed mod"));

    /* Reinstalling unchanged content keeps build 1 and replaces the folder. */
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    CHECK(!strcmp(key, "alex.r2-imp") && st.replaced && st.copied == 13);
    CHECK(sh_ms_verify("alex.r2-imp", &vs, cause, sizeof cause));

    /* Changed content of the same version replaces it as build 2 (no blobs). */
    mt_append(src, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\imp_copy.decl", "\r\n// edited\r\n");
    mt_write(src, "mod.json", slurp(fixture, "mod.json", NULL));
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    CHECK(!strcmp(key, "alex.r2-imp") && st.replaced && b.build == 2 && strcmp(b.hash, h1));
    sh_mb_free(&b);

    /* A second version installs beside it as <id>@<version>. */
    {
        char *desc = slurp(fixture, "mod.json", NULL), *v = desc ? strstr(desc, "1.0.0") : NULL;
        if (v) v[4] = '1';
        mt_write(src, "mod.json", desc ? desc : "");
        free(desc);
    }
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    CHECK(!strcmp(key, "alex.r2-imp@1.0.1") && !st.replaced);

    /* Two folders of one id: a decl both provide is a conflict; a raw file
     * with equal bytes in both is a dedupe. */
    CHECK(sh_ms_enumerate(&list) && list.count == 2);
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/alex.r2-imp/imp_copy.md6", &hit) == SH_MS_CONFLICT && hit.providers == 2);
    CHECK(sh_ms_resolve_need(&list, "baseModel", "md6/mods/alex.r2-imp/imp_copy/assets/mesh/imp_copy.md6mesh", &hit) == SH_MS_DEDUPE);
    sh_ms_list_free(&list);
    /* Verifying the id covers every folder of it. */
    CHECK(sh_ms_verify("alex.r2-imp", &vs, cause, sizeof cause) && vs.entries == 2);

    /* Corruption is detected, recorded in mod.json, and disables that folder. */
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp@1.0.1\\assets\\generated\\basemodel\\md6\\mods\\alex.r2_imp\\imp_copy\\assets\\mesh\\imp_copy.bmd6model",
                root);
    {
        FILE *f = NULL;
        if (fopen_s(&f, path, "r+b") == 0 && f) { fputc('X', f); fclose(f); }
    }
    CHECK(!sh_ms_verify("alex.r2-imp@1.0.1", &vs, cause, sizeof cause));
    CHECK(strstr(cause, "imp_copy.bmd6model does not match mod.json") != NULL);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp@1.0.1");
    CHECK(e && e->loaded && e->sizes_ok && !e->validated && !sh_ms_entry_usable(e));
    CHECK(e && strstr(e->cause, "does not match"));
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/alex.r2-imp/imp_copy.md6", &hit) == SH_MS_HIT);
    sh_ms_list_free(&list);
    {
        char *text = slurp(root, "alex.r2-imp@1.0.1\\mod.json", NULL);
        CHECK(text && strstr(text, "\"validated\": false") && strstr(text, "\"cause\": \"assets/"));
        free(text);
    }
    /* A missing file shows at enumeration without hashing. */
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp@1.0.1\\assets\\generated\\image\\mods\\alex.r2_imp\\imp_icon.bimage", root);
    DeleteFileA(path);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp@1.0.1");
    CHECK(e && !e->sizes_ok && strstr(e->cause, "missing"));
    sh_ms_list_free(&list);

    /* Removed: one folder of the id is left. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp@1.0.1", root);
    CHECK(sh_ms_remove_folder(path) && !mt_has(root, "alex.r2-imp@1.0.1"));

    /* A folder dropped into mods\ by hand keeps its name; unbuilt, it is
     * listed as such, and the rebuild rule builds it in place. */
    _snprintf_s(hand, sizeof hand, _TRUNCATE, "%s\\My Dropped Mod", root);
    mt_write(hand, "package.json", "{\"id\":\"alex.dropped\",\"name\":\"Dropped\"}");
    mt_write(hand, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n}");
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    CHECK(e && !e->loaded && strstr(e->cause, "not built"));
    sh_ms_list_free(&list);
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss));
    CHECK(ss.folders == 2 && ss.failed == 0);
    if (ss.failed) fprintf(stderr, "  sync: %s\n", g_log[(g_logged - 1) % 64]);
    CHECK(ss.rebuilt >= 1 && log_has("MODS: store: My Dropped Mod built: alex.dropped@0.0.0+1"));
    CHECK(mt_has(hand, "mod.json") && !mt_has(hand, "package.json"));   /* package.json became mod.json */
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    CHECK(e && sh_ms_entry_usable(e) && !e->bundle.origin[0]);
    sh_ms_list_free(&list);
    /* Unchanged trees are not built again: the cache answers. */
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 0 && ss.failed == 0);
    /* An edit under assets is a new tree: built again (build 2). */
    mt_write(hand, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n\t// edited\r\n}");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    CHECK(e && sh_ms_entry_usable(e) && e->bundle.build == 2);
    sh_ms_list_free(&list);
    /* A refused rebuild leaves the cause on the folder, which stops serving. */
    mt_write(hand, "assets\\generated\\decls\\entitydef\\other\\x.decl", "{\r\n}");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 1);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    CHECK(e && !sh_ms_entry_usable(e) && strstr(e->cause, "outside mods/alex.dropped/"));
    sh_ms_list_free(&list);
    mt_delete(hand, "assets\\generated\\decls\\entitydef\\other\\x.decl");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1 && ss.failed == 0);
    /* sh_mods install of a store folder builds it in place. */
    CHECK(sh_ms_install_from_folder(hand, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    CHECK(st.in_place && !strcmp(key, "My Dropped Mod"));

    /* The store lock: a second installer waits for the holder (a migration
     * folder), it does not fail; two installers at once both succeed. */
    {
        hold_job h = { 400, 0, 0 };
        lock_job a, c;
        HANDLE th, ti[2];
        unsigned waits = modstore_lock_waits();
        memset(&a, 0, sizeof a);
        memset(&c, 0, sizeof c);
        _snprintf_s(a.folder, sizeof a.folder, _TRUNCATE, "%s\\authoring\\test.lockone", tmp);
        _snprintf_s(c.folder, sizeof c.folder, _TRUNCATE, "%s\\authoring\\test.locktwo", tmp);
        make_small_mod(a.folder, "test.lockone");
        make_small_mod(c.folder, "test.locktwo");
        th = CreateThread(NULL, 0, hold_thread, &h, 0, NULL);
        CHECK(th != NULL);
        while (th && !InterlockedCompareExchange(&h.held, 0, 0)) Sleep(1);
        ti[0] = CreateThread(NULL, 0, install_thread, &a, 0, NULL);
        CHECK(ti[0] != NULL);
        if (ti[0]) WaitForSingleObject(ti[0], 60000);
        if (th) WaitForSingleObject(th, 60000);
        CHECK(a.ok);
        if (!a.ok) fprintf(stderr, "  waiting install: %s\n", a.cause);
        CHECK(a.done_at >= h.released_at);   /* it finished only after the holder let go */
        CHECK(modstore_lock_waits() > waits);
        if (ti[0]) CloseHandle(ti[0]);
        if (th) CloseHandle(th);
        /* Two installers at once (the Mods tab and sh_mods install). */
        memset(&a, 0, sizeof a);
        _snprintf_s(a.folder, sizeof a.folder, _TRUNCATE, "%s\\authoring\\test.lockone", tmp);
        ti[0] = CreateThread(NULL, 0, install_thread, &a, 0, NULL);
        ti[1] = CreateThread(NULL, 0, install_thread, &c, 0, NULL);
        CHECK(ti[0] && ti[1]);
        if (ti[0] && ti[1]) WaitForMultipleObjects(2, ti, TRUE, 60000);
        CHECK(a.ok && c.ok);
        if (!a.ok || !c.ok) fprintf(stderr, "  parallel installs: %s / %s\n", a.cause, c.cause);
        if (ti[0]) CloseHandle(ti[0]);
        if (ti[1]) CloseHandle(ti[1]);
        CHECK(sh_ms_verify("test.lockone", &vs, cause, sizeof cause));
        CHECK(sh_ms_verify("test.locktwo@1.0.0", &vs, cause, sizeof cause));
        /* The lock is recursive on one thread: a verification inside a held step. */
        modstore_lock();
        CHECK(sh_ms_verify("test.locktwo", &vs, cause, sizeof cause));
        modstore_unlock();
    }

    /* The store itself is not a mod, nor a folder inside a mod. */
    CHECK(!sh_ms_install_from_folder(root, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp\\assets", root);
    CHECK(!sh_ms_install_from_folder(path, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));

    test_upgrade(fixture, tmp);

    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    if (failures) { fprintf(stderr, "modstore_test: %d failure(s)\n", failures); return 1; }
    printf("modstore_test: install, copies, verify, corruption, second version, rebuild rule, resolve_need, lock and "
           "store upgrade pass\n");
    return 0;
}
