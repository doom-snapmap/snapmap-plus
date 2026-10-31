/* modstore_test.c -- the mods\ store (Contract 5): install as mods\<id> with
 * real copies, enumeration, verification recorded in mod.json, detected
 * corruption, folder names for a second version, the rebuild rule (tree hash
 * and build cache), resolve_need hit, miss, dedupe and conflict, the store
 * lock (a second installer waits, two installers at once both succeed), and
 * the upgrade of the retired layout (bundle.json, .state.json, blobs\ with
 * hard links, legacy\, the product files) to mod.json folders, and the
 * aliases a rebuild leaves (every earlier build of a folder resolves to the
 * current one in one hop; removal drops them only when nothing of the id is left).
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

/* ------------------------------------------------------------ aliases */

static const char *alias_of(const sh_ms_list *l, const char *from)
{
    size_t i;
    for (i = 0; i < l->alias_count; i++) if (!strcmp(l->aliases[i].from, from)) return l->aliases[i].to;
    return NULL;
}

static int alias_targets(const sh_ms_list *l, const char *to)
{
    size_t i;
    int n = 0;
    for (i = 0; i < l->alias_count; i++) if (!strcmp(l->aliases[i].to, to)) n++;
    return n;
}

/* The section stays flat: a chain of notes re-points every earlier alias. */
static void test_alias_note(void)
{
    sh_ms_list l;
    const char *A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *C = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    CHECK(sh_ms_alias_note(A, B) == 1);
    CHECK(sh_ms_alias_note(B, C) == 2);
    CHECK(sh_ms_enumerate(&l));
    CHECK(alias_of(&l, A) && !strcmp(alias_of(&l, A), C));
    CHECK(alias_of(&l, B) && !strcmp(alias_of(&l, B), C));
    CHECK(!alias_of(&l, C));
    sh_ms_list_free(&l);
    /* Back to A's content under A's hash: A is a bundle again, not an alias. */
    CHECK(sh_ms_alias_note(C, A) == 2);
    CHECK(sh_ms_enumerate(&l));
    CHECK(!alias_of(&l, A) && alias_of(&l, B) && !strcmp(alias_of(&l, B), A) && !strcmp(alias_of(&l, C), A));
    sh_ms_list_free(&l);
    CHECK(sh_ms_alias_note(A, A) == 0 && sh_ms_alias_note("nothex", A) == 0);
    /* No folder serves A and none declares the id: its aliases go. */
    CHECK(sh_ms_alias_forget("test.nobody", A) == 2);
    CHECK(sh_ms_enumerate(&l));
    CHECK(!alias_of(&l, B) && !alias_of(&l, C));
    sh_ms_list_free(&l);
    /* An alias whose target no folder builds to (a folder deleted or replaced by
     * hand) is stale; the prune after each pass drops it. */
    CHECK(sh_ms_alias_note(B, C) == 1);
    CHECK(sh_ms_alias_prune() == 1);
    CHECK(sh_ms_enumerate(&l));
    CHECK(!alias_of(&l, B) && l.alias_count == 0);
    sh_ms_list_free(&l);
    CHECK(sh_ms_alias_prune() == 0);
}


/* ------------------------------------------------------------ the second step */

static void tidy_mod(const char *tmp, const char *root, const char *as_name, const char *id, const char *version,
                     const char *block, const char *const *rels, const char *const *bodies, char hash_out[65])
{
    char src[MAX_PATH * 2], desc[1024], key[160], cause[512] = "";
    sh_ms_install_stats st;
    sh_mb_bundle b;
    int i;
    (void)root;
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\tidy-src\\%s", tmp, as_name);
    _snprintf_s(desc, sizeof desc, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"%s\",\"name\":\"%s\",%s}", id, version, id, block);
    mt_write(src, "mod.json", desc);
    for (i = 0; rels[i]; i++) mt_write(src, rels[i], bodies[i]);
    memset(&b, 0, sizeof b);
    CHECK(sh_ms_install_as(src, as_name, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  tidy fixture %s: %s\n", as_name, cause);
    CHECK(!strcmp(key, as_name));
    strcpy_s(hash_out, 65, b.hash);
    sh_mb_free(&b);
}

static int any_name_has(const char *root, const char *needle)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    int hit = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do hit |= strstr(fd.cFileName, needle) != NULL; while (!hit && FindNextFileA(h, &fd));
    FindClose(h);
    return hit;
}

/* A store that went through the first upgrade: an installed mod and a copy a
 * map delivered under its id with other content; a copy delivered under
 * another id whose paths the installed mod provides; a delivered package
 * with nothing installed of it (named with its generated version); the
 * installed doors pack named doors@0.0.0 because of the copies; a completed
 * migration record. The second step runs once. */
static void test_tidy(const char *tmp)
{
    char root[MAX_PATH], h_doors[65], h_doors_copy[65], h_cyber[65], h_cyber_copy[65], h_female[65], *section = NULL;
    sh_ms_upgrade_stats us;
    sh_ms_list list;
    const sh_ms_entry *e;
    static const char T0[] = "0000000000000000000000000000000000000000000000000000000000000a0a";
    static const char T1[] = "1111111111111111111111111111111111111111111111111111111111111111";
    static const char T2[] = "2222222222222222222222222222222222222222222222222222222222222222";
    static const char *const DOOR[] = { "assets\\generated\\decls\\entitydef\\mods\\doors\\a.decl", NULL };
    static const char *const DOOR_BODY[] = { "{\r\n}", NULL };
    static const char *const DOOR2[] = { "assets\\generated\\decls\\entitydef\\mods\\doors\\b.decl", NULL };
    static const char *const DOOR2_BODY[] = { "{\r\n\tsize = \"2\";\r\n}", NULL };
    static const char *const CYB[] = { "assets\\generated\\decls\\entitydef\\monster\\imp.decl",
                                       "assets\\generated\\decls\\entitydef\\mods\\local.cyberdemon\\boss.decl", NULL };
    static const char *const CYB_BODY[] = { "{\r\n}", "{\r\n}", NULL };
    static const char *const CYB2[] = { "assets\\generated\\decls\\entitydef\\monster\\imp.decl", NULL };
    static const char *const CYB2_BODY[] = { "{\r\n\tsize = \"3\";\r\n}", NULL };
    static const char *const FEM[] = { "assets\\generated\\decls\\entitydef\\mods\\female.models\\f.decl", NULL };
    char block[512];

    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\tidy\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(block, sizeof block, _TRUNCATE, "\"legacy\":{\"source\":\"overrides\",\"folder\":\"all doors\",\"tree\":\"%s\"},"
                "\"snapmap-plus\":{\"origin\":\"migration\"}", T0);
    tidy_mod(tmp, root, "doors@0.0.0", "doors", "0.0.0", block, DOOR, DOOR_BODY, h_doors);
    _snprintf_s(block, sizeof block, _TRUNCATE, "\"legacy\":{\"keyForm\":\"delivery\",\"key\":\"delivery.%s\"},"
                "\"snapmap-plus\":{\"origin\":\"delivery\"}", T1);
    tidy_mod(tmp, root, "doors@legacy-aaaaaaaaaaaaaaaa", "doors", "legacy-aaaaaaaaaaaaaaaa", block, DOOR2, DOOR2_BODY, h_doors_copy);
    tidy_mod(tmp, root, "local.cyberdemon", "local.cyberdemon", "0.0.0",
             "\"shadows\":[\"generated/decls/entitydef/monster/imp.decl\"],"
             "\"legacy\":{\"source\":\"overrides\",\"folder\":\"cyberdemon\"},\"snapmap-plus\":{\"origin\":\"migration\"}",
             CYB, CYB_BODY, h_cyber);
    _snprintf_s(block, sizeof block, _TRUNCATE, "\"shadows\":[\"generated/decls/entitydef/monster/imp.decl\"],"
                "\"legacy\":{\"keyForm\":\"delivery\",\"key\":\"delivery.%s\"},\"snapmap-plus\":{\"origin\":\"delivery\"}", T2);
    tidy_mod(tmp, root, "cyberdemon", "cyberdemon", "legacy-cccccccccccccccc", block, CYB2, CYB2_BODY, h_cyber_copy);
    tidy_mod(tmp, root, "female.models@legacy-bbbbbbbbbbbbbbbb", "female.models", "legacy-bbbbbbbbbbbbbbbb",
             "\"legacy\":{\"keyForm\":\"delivery\",\"key\":\"delivery.x\"},\"snapmap-plus\":{\"origin\":\"delivery\"}",
             FEM, DOOR_BODY, h_female);
    {
        char mig[1024];
        _snprintf_s(mig, sizeof mig, _TRUNCATE,
                    "{\"format\":1,\"complete\":true,\"migratedAt\":5,\"source\":\"C:\\\\o\",\"entries\":3,\"built\":2,"
                    "\"folders\":[{\"folder\":\"all doors\",\"kind\":\"package\",\"outcome\":\"built\",\"store\":\"doors@0.0.0\"},"
                    "{\"folder\":\"map-1/doors\",\"kind\":\"delivered\",\"outcome\":\"reused\",\"store\":\"doors@legacy-aaaaaaaaaaaaaaaa\"},"
                    "{\"folder\":\"z.zip\",\"kind\":\"zip\",\"outcome\":\"skipped\",\"reason\":\"a ZIP archive\"}],"
                    "\"trees\":{\"%s\":\"doors@0.0.0\",\"%s\":\"doors@legacy-aaaaaaaaaaaaaaaa\"}}", T0, T1);
        CHECK(sh_mb_record_put(root, "migration", mig));
    }

    g_logged = 0;
    CHECK(sh_ms_upgrade(&us) == 1);
    CHECK(!us.ran && us.tidied && us.same_id == 1 && us.covered == 1 && us.tidy_renamed == 2 && !us.tidy_failed);
    CHECK(us.migration_collapsed);
    CHECK(log_has("MODS: store: doors@legacy-aaaaaaaaaaaaaaaa: dropped, delivered by a map under the id of the installed doors@0.0.0"));
    CHECK(log_has("MODS: store: cyberdemon: dropped, delivered by a map as cyberdemon; the installed local.cyberdemon "
                  "(local.cyberdemon) provides all 1 of its path(s)"));
    CHECK(log_has("MODS: store: doors@0.0.0 -> doors"));
    CHECK(log_has("MODS: store: female.models@legacy-bbbbbbbbbbbbbbbb -> female.models"));
    CHECK(log_has("MODS: store: tidied in "));
    CHECK(!any_name_has(root, "legacy"));
    CHECK(sh_ms_enumerate(&list) && list.count == 3);
    e = find_entry(&list, "doors");
    CHECK(e && !strcmp(e->bundle.hash, h_doors));
    CHECK(sh_ms_resolve(&list, h_doors_copy) == e);
    e = find_entry(&list, "local.cyberdemon");
    CHECK(e && sh_ms_resolve(&list, h_cyber_copy) == e && !strcmp(e->bundle.hash, h_cyber));
    e = find_entry(&list, "female.models");
    CHECK(e && sh_ms_entry_delivered(e) && !strcmp(e->bundle.origin, "delivery") && !strcmp(e->bundle.hash, h_female));
    sh_ms_list_free(&list);
    /* The migration record keeps what idempotence and the Migrate button need. */
    CHECK(sh_mb_record_get(root, "migration", &section) == 1 && section);
    if (section) {
        char want[160];
        CHECK(strstr(section, "\"complete\":true") && strstr(section, "\"migratedAt\":5") && strstr(section, "\"source\":"));
        CHECK(strstr(section, "\"z.zip\"") && !strstr(section, "\"all doors\"") && !strstr(section, "map-1/doors"));
        CHECK(!strstr(section, "\"entries\"") && !strstr(section, "\"built\"") && !strstr(section, "\"format\""));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s\":\"doors\"", T0);
        CHECK(strstr(section, want) != NULL);
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s\":\"doors\"", T1);
        CHECK(strstr(section, want) != NULL);
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s\":\"local.cyberdemon\"", T2);
        CHECK(strstr(section, want) != NULL);
        CHECK(!strstr(section, "legacy-"));
    }
    free(section);
    /* Once: a second start does nothing. */
    g_logged = 0;
    CHECK(sh_ms_upgrade(&us) == 0 && !us.tidied && g_logged == 0);
    /* A download or install of a delivered package without a version is
     * never named after its generated version. */
    {
        char name[SH_MS_NAME_MAX + 1];
        CHECK(sh_mb_folder_name(root, "female.models", "legacy-bbbbbbbbbbbbbbbb", "abcdef0123456789", name, sizeof name) == 1);
        CHECK(!strcmp(name, "female.models-abcdef01"));
    }
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
        /* Completed: the names follow the folders, the finished rows are gone. */
        CHECK(sh_mb_record_get(root, "migration", &section) == 1 && section && strstr(section, "\"abc\":\"local.mig\"") &&
              strstr(section, "\"def\":\"alex.r2-imp@1.0.0\"") && strstr(section, "\"folders\":[]") &&
              !strstr(section, "\"format\""));
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

/* The retired recycle bin: the sweep deletes every entry of removed-mods\\
 * (links removed, never followed), the folder and the record's section, with
 * one line per entry and a summary; a second start finds nothing. */
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
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\sweep\\mods", tmp);
    _snprintf_s(bin, sizeof bin, _TRUNCATE, "%s\\sweep\\removed-mods", tmp);
    _snprintf_s(outside, sizeof outside, _TRUNCATE, "%s\\sweep\\outside", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(keep, sizeof keep, _TRUNCATE, "%s\\alex.kept", root);
    make_small_mod(keep, "alex.kept");
    {
        char a[MAX_PATH], b[MAX_PATH], c[MAX_PATH];
        _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\alex.one", bin);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\alex.two-2", bin);
        _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\stray", bin);
        mt_write(a, "mod.json", "{\"id\":\"alex.one\"}");
        mt_write(a, "assets\\generated\\decls\\entitydef\\mods\\alex.one\\x.decl", "{\r\n}");
        mt_write(b, "mod.json", "{\"id\":\"alex.two\"}");
        mt_write(c, "notes.txt", "moved here by hand");
        /* A junction inside a binned folder points outside: removed as a link, its target kept. */
        mt_write(outside, "precious.txt", "not the bin's");
        _snprintf_s(link, sizeof link, _TRUNCATE, "cmd /c mklink /J \"%s\\link\" \"%s\" >nul 2>&1", a, outside);
        (void)system(link);
    }
    CHECK(sh_mb_record_put(root, "removed", "{\"alex.one\":{\"id\":\"alex.one\",\"hashes\":[]}}"));
    g_sweep[0] = 0;
    g_sweep_lines = 0;
    CHECK(sh_ms_bin_sweep(sweep_line, NULL) == 3);
    CHECK(strstr(g_sweep, "MODS: removed-mods: deleted alex.one\n") && strstr(g_sweep, "MODS: removed-mods: deleted alex.two-2\n") &&
          strstr(g_sweep, "MODS: removed-mods: deleted stray\n"));
    CHECK(strstr(g_sweep, "MODS: removed-mods: 3 folder(s) deleted; the bin is retired\n") && g_sweep_lines == 4);
    CHECK(!mt_exists(bin));
    _snprintf_s(link, sizeof link, _TRUNCATE, "%s\\precious.txt", outside);
    CHECK(mt_exists(link));                                    /* the link was not followed */
    CHECK(sh_mb_record_get(root, "removed", &rec) != 1 || !rec);
    free(rec);
    _snprintf_s(keep, sizeof keep, _TRUNCATE, "%s\\alex.kept\\mod.json", root);
    CHECK(mt_exists(keep));                                    /* the store is not touched */
    /* The next start: nothing there, nothing said. */
    g_sweep[0] = 0;
    g_sweep_lines = 0;
    CHECK(sh_ms_bin_sweep(sweep_line, NULL) == 0 && g_sweep_lines == 0);

    /* Uninstall's delete: a direct child of the store only. */
    {
        char cause[300], kept_dir[MAX_PATH];
        _snprintf_s(kept_dir, sizeof kept_dir, _TRUNCATE, "%s\\alex.kept", root);
        CHECK(!sh_ms_delete_mod_folder(outside, cause, sizeof cause) && strstr(cause, "is not a folder of the store"));
        CHECK(!sh_ms_delete_mod_folder(root, cause, sizeof cause));
        CHECK(sh_ms_delete_mod_folder(kept_dir, cause, sizeof cause) && !mt_exists(kept_dir));
        CHECK(!sh_ms_delete_mod_folder(kept_dir, cause, sizeof cause) && strstr(cause, "is no longer in the store"));
        CHECK(mt_exists(link));
    }
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
    char h1[65], h0[65], d1[65] = "", d2[65] = "", d3[65] = "";

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
    strcpy_s(h0, sizeof h0, b.hash);
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
    g_logged = 0;
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    CHECK(!strcmp(key, "alex.r2-imp") && st.replaced && b.build == 2 && strcmp(b.hash, h1));
    /* A map pinning build 1 loads build 2: the replaced hash is an alias. */
    CHECK(log_has("1 earlier bundle(s) now resolve to it"));
    CHECK(sh_ms_enumerate(&list));
    CHECK(alias_of(&list, h1) && !strcmp(alias_of(&list, h1), b.hash));
    e = sh_ms_resolve(&list, h1);
    CHECK(e && !strcmp(e->name, "alex.r2-imp") && !strcmp(e->bundle.hash, b.hash));
    CHECK(sh_ms_resolve(&list, b.hash) == e);
    CHECK(!sh_ms_resolve(&list, "0000000000000000000000000000000000000000000000000000000000000000"));
    sh_ms_list_free(&list);
    strcpy_s(h1, sizeof h1, b.hash);
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
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    if (e) strcpy_s(d1, sizeof d1, e->bundle.hash);
    sh_ms_list_free(&list);
    /* Unchanged trees are not built again: the cache answers. */
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 0 && ss.failed == 0);
    /* An edit under assets is a new tree: built again (build 2), and build 1's
     * hash becomes an alias of it, so a map saved against build 1 still loads. */
    mt_write(hand, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n\t// edited\r\n}");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1);
    CHECK(log_has("its tree changed; bundle ") && log_has(" is now an alias of it"));
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    CHECK(e && sh_ms_entry_usable(e) && e->bundle.build == 2);
    if (e) strcpy_s(d2, sizeof d2, e->bundle.hash);
    CHECK(strcmp(d1, d2) && alias_of(&list, d1) && !strcmp(alias_of(&list, d1), d2));
    CHECK(sh_ms_resolve(&list, d1) == e);
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
    /* Another edit: build 3. Chained rebuilds stay one hop: builds 1 and 2
     * both name build 3, and no alias names an alias. */
    mt_write(hand, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n\t// edited twice\r\n}");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1 && ss.failed == 0);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    CHECK(e && e->bundle.build == 3);
    if (e) {
        CHECK(alias_of(&list, d1) && !strcmp(alias_of(&list, d1), e->bundle.hash));
        CHECK(alias_of(&list, d2) && !strcmp(alias_of(&list, d2), e->bundle.hash));
        CHECK(!alias_of(&list, e->bundle.hash) && alias_targets(&list, e->bundle.hash) == 2);
        CHECK(sh_ms_resolve(&list, d1) == e && sh_ms_resolve(&list, d2) == e);
        strcpy_s(d3, sizeof d3, e->bundle.hash);
    }
    sh_ms_list_free(&list);
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

    /* Removing a folder: its aliases stay while a folder of the id remains,
     * and go once none does. */
    {
        char other[MAX_PATH * 2];
        _snprintf_s(other, sizeof other, _TRUNCATE, "%s\\My Dropped Copy", root);
        CHECK(mt_copy_tree(hand, other));
        CHECK(sh_ms_remove_folder(hand));
        CHECK(sh_ms_alias_forget("alex.dropped", d3) == 0);
        CHECK(sh_ms_enumerate(&list));
        CHECK(alias_of(&list, d1) && alias_of(&list, d2));
        e = sh_ms_resolve(&list, d1);
        CHECK(e && !strcmp(e->name, "My Dropped Copy"));
        sh_ms_list_free(&list);
        CHECK(sh_ms_remove_folder(other));
        g_logged = 0;
        CHECK(sh_ms_alias_forget("alex.dropped", d3) == 2);
        CHECK(log_has("2 alias(es) of bundle"));
        CHECK(sh_ms_enumerate(&list));
        CHECK(!alias_of(&list, d1) && !alias_of(&list, d2) && !sh_ms_resolve(&list, d1));
        CHECK(alias_of(&list, h0) != NULL);   /* another mod's aliases are untouched */
        sh_ms_list_free(&list);
    }
    test_alias_note();
    test_tidy(tmp);
    test_bin_sweep(tmp);

    test_upgrade(fixture, tmp);

    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    if (failures) { fprintf(stderr, "modstore_test: %d failure(s)\n", failures); return 1; }
    printf("modstore_test: install, copies, verify, corruption, second version, rebuild rule, resolve_need, lock, rebuild aliases, the second upgrade step and "
           "store upgrade pass\n");
    return 0;
}
