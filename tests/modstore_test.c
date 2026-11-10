/* modstore_test.c -- the mods\ store (Contract 5): install as mods\<id> with
 * real copies, enumeration, verification recorded in mod.json, detected
 * corruption, folder names for a second version, the rebuild rule (tree hash
 * and build cache), resolve_need hit, miss, dedupe and conflict, the store
 * lock (a second installer waits, two installers at once both succeed), and
 * the upgrade of the retired layout (bundle.json, .state.json, blobs\ with
 * hard links, legacy\, the product files) to mod.json folders, and the
 * aliases a rebuild leaves (every earlier build of a folder resolves to the
 * current one in one hop; removal drops them only when nothing of the id is left),
 * and removal through the Windows Recycle Bin (a folder of the store only, links
 * not followed; the outright delete when the shell refuses or the path is too
 * long for it, and the log line saying so).
 *
 *   modstore_test.exe <tests\fixtures\mods\alex.r2-imp>
 */
#include <windows.h>
#include <shellapi.h>
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
static int log_count(const char *needle)
{
    unsigned i, n = g_logged < 64 ? g_logged : 64;
    int c = 0;
    for (i = 0; i < n; i++) if (strstr(g_log[i], needle)) c++;
    return c;
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

/* ------------------------------------------------------------ first-build keys */

#define FB_BASE "assets\\generated\\decls\\entitydef\\mods\\%s\\probe_base.decl"
#define FB_ROW  "assets\\generated\\decls\\entitydef\\mods\\%s\\probe.decl"

/* A hand-made folder named `leaf`: two decls under mods/<leaf>/, one
 * inheriting the other, and a mod.json of name, version and description
 * with a string under the key. */
static void fb_make(const char *dir, const char *leaf)
{
    char rel[MAX_PATH], text[600];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\r\n  \"name\": \"Cyber probe\",\r\n  \"version\": \"1.0.0\",\r\n  \"description\": \"A probe.\",\r\n"
                "  \"strings\": { \"en\": { \"mods/%s/probe_name\": \"Probe\" } }\r\n}\r\n", leaf);
    mt_write(dir, "mod.json", text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_BASE, leaf);
    mt_write(dir, rel, "{\r\n\tinherit = \"monster/imp\";\r\n}");
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, leaf);
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\r\n\tinherit = \"mods/%s/probe_base\";\r\n}", leaf);
    mt_write(dir, rel, text);
}

/* The one folder of `parent` whose name starts with `prefix`, else "". */
static int fb_find(const char *parent, const char *prefix, char *out, size_t cap)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    int n = 0;
    out[0] = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\%s*", parent, prefix);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!n) _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", parent, fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* Every entry of `parent` (files and folders, names only) as one line. */
static void fb_names(const char *parent, char *out, size_t cap)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    size_t w = 0;
    out[0] = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", parent);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        w += (size_t)_snprintf_s(out + w, cap - w, _TRUNCATE, "%s;", fd.cFileName);
    } while (FindNextFileA(h, &fd) && w < cap);
    FindClose(h);
}

static void test_first_build(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH], got[MAX_PATH * 2], got2[MAX_PATH * 2], rel[MAX_PATH], from[300], cause[600];
    char key[SH_MS_NAME_MAX + 1], before[2048], after[2048];
    const char *leaf;
    sh_ms_sync_stats ss;
    sh_ms_install_stats st;
    sh_ms_list list;
    const sh_ms_entry *e;
    unsigned refs = 0;
    char *t;
    size_t n1 = 0, n2 = 0;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\firstkey\\mods", tmp);
    CHECK(sh_ms_set_root(root));

    /* A hand-made folder: renamed to a generated key at its first build, its
     * references rewritten once, the key and origin in its product block. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\cyberprobe", root);
    fb_make(dir, "cyberprobe");
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1 && ss.failed == 0);
    if (ss.failed) fprintf(stderr, "  first build: %s\n", g_log[(g_logged - 1) % 64]);
    CHECK(!mt_exists(dir));
    CHECK(fb_find(root, "cyberprobe_", got, sizeof got) == 1);
    leaf = strrchr(got, '\\') ? strrchr(got, '\\') + 1 : got;
    CHECK(strlen(leaf) == 15 && !strchr(leaf, '-'));
    CHECK(log_has("MODS: store: cyberprobe renamed to cyberprobe_") && log_has("2 reference(s) rewritten in 2 file(s)"));
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, leaf);
    t = slurp(got, rel, NULL);
    _snprintf_s(from, sizeof from, _TRUNCATE, "inherit = \"mods/%s/probe_base\";", leaf);
    CHECK(t && strstr(t, from));
    free(t);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_BASE, leaf);
    CHECK(mt_has(got, rel));
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\cyberprobe");
    CHECK(!mt_has(got, rel));
    t = slurp(got, "mod.json", NULL);
    _snprintf_s(from, sizeof from, _TRUNCATE, "\"mods/%s/probe_name\": \"Probe\"", leaf);
    CHECK(t && strstr(t, from) && strstr(t, "\"origin\": \"by-hand\"") && strstr(t, "\"description\": \"A probe.\""));
    _snprintf_s(from, sizeof from, _TRUNCATE, "\"key\": \"%s\"", leaf);
    CHECK(t && strstr(t, from) && !strstr(t, "\"id\""));
    free(t);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, leaf);
    CHECK(e && sh_ms_entry_usable(e) && !strcmp(e->bundle.id, leaf) && e->bundle.key_source == SH_MB_KEY_FROM_BLOCK &&
          !e->bundle.origin[0] && e->bundle.build == 1);
    sh_ms_list_free(&list);
    CHECK(sh_ms_renamed_from(got, from, sizeof from, &refs) && !strcmp(from, "cyberprobe") && refs == 2);
    /* Afterwards it is a folder like any other: nothing renamed again, the cache answers. */
    CHECK(sh_ms_first_key(got, got2, sizeof got2, cause, sizeof cause) == 0 && !got2[0]);
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 0 && ss.failed == 0);

    /* A second hand-made "cyberprobe" gets its own salt; both serve. */
    fb_make(dir, "cyberprobe");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1 && ss.failed == 0);
    CHECK(fb_find(root, "cyberprobe_", got2, sizeof got2) == 2 && !mt_exists(dir));
    CHECK(sh_ms_enumerate(&list));
    {
        size_t i;
        int usable = 0, alias_left = 0;
        char live1[SH_MB_HASH_HEX + 1] = "", live2[SH_MB_HASH_HEX + 1] = "";
        for (i = 0; i < list.count; i++)
            if (!strncmp(list.items[i].name, "cyberprobe_", 11) && sh_ms_entry_usable(&list.items[i])) {
                strcpy_s(usable ? live2 : live1, SH_MB_HASH_HEX + 1, list.items[i].bundle.hash);
                usable++;
            }
        CHECK(usable == 2);
        /* An alias from a build a folder still builds to (a copied folder whose
         * product block named the original's build) does not stand: the prune
         * after each pass drops it, and the original keeps answering for itself. */
        CHECK(strcmp(live1, live2) != 0 && sh_ms_alias_note(live1, live2) == 1);
        g_logged = 0;
        CHECK(sh_ms_alias_prune() == 1);
        CHECK(log_has("dropped: a folder builds to"));
        sh_ms_list_free(&list);
        CHECK(sh_ms_enumerate(&list));
        for (i = 0; i < list.alias_count; i++) if (!strcmp(list.aliases[i].from, live1)) alias_left++;
        CHECK(alias_left == 0);
    }
    sh_ms_list_free(&list);

    /* Nested organizing folders: the leaf alone is renamed; a name that is
     * already a generated key's shape gets a new salt. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\probe_ab12", root);
    fb_make(dir, "probe_ab12");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s\\demon-mods", root);
    CHECK(!mt_exists(dir) && fb_find(rel, "probe_", got2, sizeof got2) == 1 && !strstr(got2, "probe_ab12"));
    CHECK(sh_ms_renamed_from(got2, from, sizeof from, &refs) && !strcmp(from, "probe_ab12") && refs == 2);

    /* The cooked '_' spelling of a dashed folder name: raw paths and decl text. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\cyber-eye", root);
    fb_make(dir, "cyber-eye");
    mt_write(dir, "assets\\generated\\decls\\material2\\mods\\cyber-eye\\glow.decl",
             "{\r\n\tstage { transmap mods/cyber_eye/glow_img }\r\n}");
    mt_write(dir, "assets\\generated\\image\\mods\\cyber_eye\\glow_img.txt", "an image's stand-in, no reference");
    t = NULL;
    sh_ms_first_key(dir, got2, sizeof got2, cause, sizeof cause);
    CHECK(got2[0] && strstr(got2, "\\cyber_eye_") && !mt_exists(dir));
    if (got2[0]) {
        const char *l2 = strrchr(got2, '\\') + 1;
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\material2\\mods\\%s\\glow.decl", l2);
        t = slurp(got2, rel, NULL);
        _snprintf_s(from, sizeof from, _TRUNCATE, "transmap mods/%s/glow_img", l2);
        CHECK(t && strstr(t, from));
        free(t);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\glow_img.txt", l2);
        CHECK(mt_has(got2, rel));
        CHECK(sh_ms_renamed_from(got2, from, sizeof from, &refs) && refs == 3);
        CHECK(sh_ms_remove_folder(got2));   /* its stand-in image would not build */
    }

    /* Refused: a binary file that names the folder. Nothing is touched, the
     * cause is the folder's, and no staging folder is left. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\badprobe", root);
    fb_make(dir, "badprobe");
    {
        static const char bin[] = "BIN\0\0\0mods/badprobe/skin\0\0";
        char path[MAX_PATH * 2];
        FILE *f;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\generated\\model\\mods\\badprobe\\probe.bmodel", dir);
        mt_mkdirs_for(path);
        if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(bin, 1, sizeof bin - 1, f); fclose(f); }
    }
    t = slurp(dir, "mod.json", &n1);
    fb_names(root, before, sizeof before);
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 1);
    CHECK(log_has("badprobe refused: ") && log_has("is a binary file that names mods/badprobe/"));
    fb_names(root, after, sizeof after);
    CHECK(!strcmp(before, after) && !strstr(after, ".staging-") && !strstr(after, ".old-"));
    {
        char *t2 = slurp(dir, "mod.json", &n2);
        CHECK(t && t2 && n1 == n2 && !memcmp(t, t2, n1));
        free(t2);
    }
    free(t);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, "badprobe");
    CHECK(mt_has(dir, rel));
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "badprobe");
    CHECK(e && !e->loaded && strstr(e->cause, "binary file that names mods/badprobe/"));
    sh_ms_list_free(&list);
    CHECK(sh_ms_first_key(dir, got2, sizeof got2, cause, sizeof cause) == -1 && strstr(cause, "nothing was renamed"));
    CHECK(sh_ms_remove_folder(dir));

    /* Refused: Windows will not rename a folder with a file open in it. The
     * folder stays whole; closing the file lets the next rescan do it. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\heldprobe", root);
    fb_make(dir, "heldprobe");
    {
        char path[MAX_PATH * 2];
        HANDLE h;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" FB_BASE, dir, "heldprobe");
        h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(h != INVALID_HANDLE_VALUE);
        fb_names(root, before, sizeof before);
        CHECK(sh_ms_first_key(dir, got2, sizeof got2, cause, sizeof cause) == -1 && strstr(cause, "Windows refused"));
        fb_names(root, after, sizeof after);
        CHECK(!strcmp(before, after) && mt_exists(dir));
        _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, "heldprobe");
        t = slurp(dir, rel, NULL);
        CHECK(t && strstr(t, "\"mods/heldprobe/probe_base\""));
        free(t);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0 && !mt_exists(dir) && fb_find(root, "heldprobe_", got2, sizeof got2) == 1);

    /* Never renamed: a folder with an id, or with a key in its block. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\legacy-probe", root);
    make_small_mod(dir, "alex.legacy-probe");
    CHECK(sh_ms_first_key(dir, got2, sizeof got2, cause, sizeof cause) == 0 && !got2[0]);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\keyed", root);
    mt_write(dir, "mod.json", "{\"name\":\"Keyed\",\"snapmap-plus\":{\"format\":1,\"key\":\"keyed_ab12\",\"origin\":\"by-hand\"}}");
    mt_write(dir, "assets\\generated\\decls\\entitydef\\mods\\keyed_ab12\\k.decl", "{\r\n\tinherit = \"monster/imp\";\r\n}");
    CHECK(sh_ms_first_key(dir, got2, sizeof got2, cause, sizeof cause) == 0);
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\legacy-probe", root);
    CHECK(mt_exists(dir));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\keyed", root);
    CHECK(mt_exists(dir));
    /* Outside the store nothing is renamed. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\firstkey\\elsewhere\\outprobe", tmp);
    fb_make(dir, "outprobe");
    CHECK(sh_ms_first_key(dir, got2, sizeof got2, cause, sizeof cause) == 0 && mt_exists(dir));

    /* An install from elsewhere: copied in under a generated key with its
     * references rewritten; the source is only read. */
    t = slurp(dir, "mod.json", &n1);
    CHECK(sh_ms_install_from_folder(dir, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    if (!key[0]) fprintf(stderr, "  install hand-made: %s\n", cause);
    CHECK(!strncmp(key, "outprobe_", 9) && strlen(key) == 13 && !st.in_place && st.copied == 2);
    {
        char *t2 = slurp(dir, "mod.json", &n2);
        CHECK(t && t2 && n1 == n2 && !memcmp(t, t2, n1));   /* the source is not written */
        free(t2);
    }
    free(t);
    _snprintf_s(got2, sizeof got2, _TRUNCATE, "%s\\%s", root, key);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, key);
    t = slurp(got2, rel, NULL);
    _snprintf_s(from, sizeof from, _TRUNCATE, "\"mods/%s/probe_base\"", key);
    CHECK(t && strstr(t, from));
    free(t);
    t = slurp(got2, "mod.json", NULL);
    CHECK(t && strstr(t, "\"origin\": \"install\""));
    free(t);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, key);
    CHECK(e && sh_ms_entry_usable(e) && !strcmp(e->bundle.origin, "install"));
    sh_ms_list_free(&list);
    /* An uninstalled folder's record is pruned at the next rescan. */
    CHECK(sh_ms_remove_folder(got));
    CHECK(sh_ms_sync(&RESOLVER, &ss));
    CHECK(!sh_ms_renamed_from(got, from, sizeof from, &refs));
    t = slurp(root, "snapmap-plus.json", NULL);
    CHECK(t && strstr(t, "\"renamed\"") && !strstr(t, leaf));
    free(t);
}

/* ------------------------------------------------------------ aliases */

static const char *alias_of(const sh_ms_list *l, const char *from)
{
    size_t i;
    for (i = 0; i < l->alias_count; i++) if (!strcmp(l->aliases[i].from, from)) return l->aliases[i].to;
    return NULL;
}

static int ms_test_folder_builds_to(const sh_ms_list *l, const char *hash)
{
    size_t i;
    for (i = 0; i < l->count; i++) if (l->items[i].loaded && !_stricmp(l->items[i].bundle.hash, hash)) return 1;
    return 0;
}

static int alias_targets(const sh_ms_list *l, const char *to)
{
    size_t i;
    int n = 0;
    for (i = 0; i < l->alias_count; i++) if (!strcmp(l->aliases[i].to, to)) n++;
    return n;
}

/* The section stays flat: a chain of notes re-points every earlier alias. */
/* The boot's rebuild rule (sh_ms_boot_sync): a store whose folders have no
 * product block (the first boot after a conversion) is built at boot as a
 * rescan builds it, then the index is rebuilt; nothing pending, nothing runs. */
static volatile LONG g_boot_rescans;
static int boot_rescan(void) { InterlockedIncrement(&g_boot_rescans); return 1; }

static void test_boot_sync(const char *fixture, const char *tmp)
{
    char root[MAX_PATH], a[MAX_PATH * 2], c[MAX_PATH * 2];
    const char *mj;
    int i;
    sh_ms_list list;
    const sh_ms_entry *e;
    printf("-- boot: folders not built yet are built as a rescan builds them, then indexed\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\boot\\mods", tmp);
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\boot", tmp);
    CreateDirectoryA(a, NULL);
    CreateDirectoryA(root, NULL);
    CHECK(sh_ms_set_root(root));
    CHECK(sh_ms_sync_pending() == 0);
    g_boot_rescans = 0;
    CHECK(sh_ms_boot_sync(&RESOLVER, boot_rescan, 0) == 0 && g_boot_rescans == 0);
    /* Two converted folders: author blocks only, as the converter writes them. */
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\alex.r2-imp", root);
    CHECK(mt_copy_tree(fixture, a));
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\campaign\\alex.boot", root);
    mt_write(c, "mod.json", "{\"id\":\"alex.boot\",\"version\":\"0.1.0\",\"name\":\"Boot\"}");
    mt_write(c, "assets\\generated\\decls\\entitydef\\mods\\alex.boot\\thing.decl", "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n}");
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp");
    CHECK(e && !e->loaded && strstr(e->cause, "not built"));
    sh_ms_list_free(&list);
    CHECK(sh_ms_sync_pending() == 2);
    g_logged = 0;
    CHECK(sh_ms_boot_sync(&RESOLVER, boot_rescan, 0) == 2 && g_boot_rescans == 1);
    CHECK(log_has("MODS: store: boot: 2 folder(s) not built yet") && log_has("MODS: store: boot: 2 of 2 folder(s) built (0 refused)"));
    mj = slurp(a, "mod.json", NULL);
    CHECK(mj && strstr(mj, "snapmap-plus"));
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp");
    CHECK(e && sh_ms_entry_usable(e));
    sh_ms_list_free(&list);
    /* The next boot: nothing pending, nothing built, no rescan. */
    CHECK(sh_ms_sync_pending() == 0 && sh_ms_boot_sync(&RESOLVER, boot_rescan, 0) == 0 && g_boot_rescans == 1);
    /* An edit made while the game was closed: built in the background, then indexed. */
    mt_write(c, "assets\\generated\\decls\\entitydef\\mods\\alex.boot\\thing.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n\t// edited\r\n}");
    CHECK(sh_ms_sync_pending() == 1);
    CHECK(sh_ms_boot_sync(&RESOLVER, boot_rescan, 1) == 1);
    sh_ms_boot_release();   /* the startup install is done with the store */
    for (i = 0; i < 400 && InterlockedCompareExchange(&g_boot_rescans, 0, 0) < 2; i++) Sleep(25);
    CHECK(g_boot_rescans == 2);
    for (i = 0; i < 400 && sh_ms_sync_pending() != 0; i++) Sleep(25);
    CHECK(sh_ms_sync_pending() == 0);
}

/* The startup install never waits on the boot build: the background build
 * starts only once the install has released the store, and while it runs it
 * hands the store to a waiting operation between two folders. */
static HANDLE g_between_seen;
static volatile LONG g_between_calls;
static void boot_between(unsigned built)
{
    unsigned waits = modstore_lock_waits();
    int i;
    (void)built;
    if (InterlockedIncrement(&g_between_calls) != 1) return;
    SetEvent(g_between_seen);
    /* Hold the first hand-over until the main thread is waiting for the store. */
    for (i = 0; i < 3000 && modstore_lock_waits() == waits; i++) Sleep(1);
}

static void test_boot_release(const char *tmp)
{
    char a[MAX_PATH * 2], c[MAX_PATH * 2];
    unsigned waits;
    LONG rescans, at_lock;
    int i, built_at_lock;
    printf("-- boot: the build waits for the startup install, then yields the store between folders\n");
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\boot\\mods\\alex.r2-imp", tmp);
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\boot\\mods\\campaign\\alex.boot", tmp);
    mt_append(a, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\imp_copy.decl", "\r\n// edited at boot\r\n");
    mt_write(c, "assets\\generated\\decls\\entitydef\\mods\\alex.boot\\thing.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n\t// edited twice\r\n}");
    CHECK(sh_ms_sync_pending() == 2);
    sh_ms_test_boot_rearm();
    rescans = InterlockedCompareExchange(&g_boot_rescans, 0, 0);
    CHECK(sh_ms_boot_sync(&RESOLVER, boot_rescan, 1) == 1);
    /* The startup install's own store operations, while the build thread is up: none waits. */
    waits = modstore_lock_waits();
    for (i = 0; i < 20; i++) {
        modstore_lock();
        Sleep(10);
        modstore_unlock();
    }
    CHECK(modstore_lock_waits() == waits);
    CHECK(sh_ms_sync_pending() == 2 && g_boot_rescans == rescans);   /* nothing built before the release */
    CHECK(!sh_ms_boot_released());
    /* The release: the build starts; a store operation between two folders waits one folder at most. */
    g_between_seen = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_between_calls = 0;
    sh_ms_test_between_folders = boot_between;
    g_logged = 0;
    sh_ms_boot_release();
    CHECK(sh_ms_boot_released());
    CHECK(WaitForSingleObject(g_between_seen, 20000) == WAIT_OBJECT_0);
    modstore_lock();   /* the build holds the store: this waits until its hand-over */
    at_lock = InterlockedCompareExchange(&g_boot_rescans, 0, 0);
    built_at_lock = log_count(" built: ");
    modstore_unlock();
    /* It had the store after the first folder's build, before the second's. */
    CHECK(modstore_lock_waits() == waits + 1 && at_lock == rescans && built_at_lock == 1);
    for (i = 0; i < 800 && InterlockedCompareExchange(&g_boot_rescans, 0, 0) == rescans; i++) Sleep(25);
    CHECK(g_boot_rescans == rescans + 1);
    for (i = 0; i < 400 && !log_has("MODS: store: boot: 2 of 2 folder(s) built"); i++) Sleep(25);
    CHECK(log_has("MODS: store: boot: the startup install released the store after"));
    CHECK(log_has("MODS: store: boot: 2 of 2 folder(s) built (0 refused)"));
    CHECK(sh_ms_sync_pending() == 0);   /* the cache kept both folders across the hand-over */
    sh_ms_test_between_folders = NULL;
    CloseHandle(g_between_seen);
}

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
    CHECK(!alias_of(&l, B));
    /* An alias whose target a folder of the fixture builds to survives the prune. */
    { size_t q; for (q = 0; q < l.alias_count; q++) CHECK(ms_test_folder_builds_to(&l, l.aliases[q].to)); }
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
    CHECK(strstr(g_sweep, "MODS: removed-mods: alex.one deleted\n") && strstr(g_sweep, "MODS: removed-mods: alex.two-2 deleted\n") &&
          strstr(g_sweep, "MODS: removed-mods: stray deleted\n"));
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
        CHECK(!sh_ms_delete_mod_folder(outside, cause, sizeof cause) && strstr(cause, "is not a mod folder of the store"));
        CHECK(!sh_ms_delete_mod_folder(root, cause, sizeof cause));
        CHECK(sh_ms_delete_mod_folder(kept_dir, cause, sizeof cause) && !mt_exists(kept_dir));
        CHECK(!sh_ms_delete_mod_folder(kept_dir, cause, sizeof cause) && strstr(cause, "is no longer in the store"));
        CHECK(mt_exists(link));
    }
}

/* Items in the Recycle Bin of the drive `path` is on; -1 when it cannot be asked. */
static long long bin_items(const char *path)
{
    SHQUERYRBINFO q;
    char drive[4] = { path[0], ':', '\\', 0 };
    memset(&q, 0, sizeof q);
    q.cbSize = sizeof q;
    return SHQueryRecycleBinA(drive, &q) == S_OK ? q.i64NumItems : -1;
}

/* Every removal of a mod folder goes to the Recycle Bin; the outright delete
 * only when the shell cannot take it. */
static void test_recycle(const char *tmp)
{
    char root[MAX_PATH], outside[MAX_PATH], precious[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH * 3], cause[300];
    long long before, after;
    int r, ci = getenv("CI") != NULL;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\recycle\\mods", tmp);
    _snprintf_s(outside, sizeof outside, _TRUNCATE, "%s\\recycle\\outside", tmp);
    _snprintf_s(precious, sizeof precious, _TRUNCATE, "%s\\precious.txt", outside);
    CHECK(sh_ms_set_root(root));
    mt_write(outside, "precious.txt", "not the store's");

    /* To the bin: the folder leaves the store, a junction in it is moved as a link. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alex.binned", root);
    make_small_mod(dir, "alex.binned");
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE, "cmd /c mklink /J \"%s\\link\" \"%s\" >nul 2>&1", dir, outside);
    (void)system(cmd);
    before = bin_items(dir);
    g_logged = 0;
    r = sh_ms_delete_mod_folder(dir, cause, sizeof cause);
    after = bin_items(dir);
    if (r != SH_MS_RECYCLED) fprintf(stderr, "  recycle: %d (%s)\n", r, cause);
    CHECK(ci ? r != 0 : r == SH_MS_RECYCLED);
    CHECK(!mt_exists(dir) && mt_exists(precious));
    if (r == SH_MS_RECYCLED) {
        CHECK(log_has("MODS: store: moved mods\\alex.binned to the Recycle Bin"));
        CHECK(before < 0 || after > before);
    }
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alex.binned-2", root);
    make_small_mod(dir, "alex.binned");
    r = sh_ms_remove_folder(dir);
    CHECK((ci ? r != 0 : r == SH_MS_RECYCLED) && !mt_exists(dir));

    /* Refused: outside the store, the store itself. Nothing is moved. */
    CHECK(!sh_ms_delete_mod_folder(outside, cause, sizeof cause) && strstr(cause, "is not a mod folder of the store"));
    CHECK(!sh_ms_remove_folder(outside) && mt_exists(precious));
    CHECK(!sh_ms_remove_folder(root) && mt_exists(root));
    CHECK(!sh_ms_delete_mod_folder(root, cause, sizeof cause) && mt_exists(root));

    /* The shell refuses: deleted outright, links removed and never followed, and said so. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alex.refused", root);
    make_small_mod(dir, "alex.refused");
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE, "cmd /c mklink /J \"%s\\link\" \"%s\" >nul 2>&1", dir, outside);
    (void)system(cmd);
    sh_ms_recycle_refuse_for_test(1);
    g_logged = 0;
    CHECK(sh_ms_delete_mod_folder(dir, cause, sizeof cause) == SH_MS_DELETED && strstr(cause, "the shell refused it (test)"));
    CHECK(!mt_exists(dir) && mt_exists(precious));
    CHECK(log_has("MODS: store: deleted mods\\alex.refused outright; the Recycle Bin could not take it (the shell refused it (test))"));
    make_small_mod(dir, "alex.refused");
    CHECK(sh_ms_remove_folder(dir) == SH_MS_DELETED && !mt_exists(dir));
    sh_ms_recycle_refuse_for_test(0);

    /* A path the shell cannot take (longer than MAX_PATH): the outright delete. */
    {
        wchar_t wroot[MAX_PATH], wlong[MAX_PATH * 2], wfile[MAX_PATH * 2];
        char longdir[MAX_PATH * 2];
        size_t i;
        HANDLE h;
        char name[201];
        for (i = 0; i < 200; i++) name[i] = 'x';
        name[200] = 0;
        MultiByteToWideChar(CP_ACP, 0, root, -1, wroot, MAX_PATH);
        _snwprintf_s(wlong, MAX_PATH * 2, _TRUNCATE, L"\\\\?\\%s\\long-%S-%S", wroot, name, name + 170);
        _snwprintf_s(wfile, MAX_PATH * 2, _TRUNCATE, L"%s\\mod.json", wlong);
        CHECK(CreateDirectoryW(wlong, NULL));
        h = CreateFileW(wfile, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        _snprintf_s(longdir, sizeof longdir, _TRUNCATE, "%s\\long-%s-%s", root, name, name + 170);
        CHECK(strlen(longdir) >= MAX_PATH);
        CHECK(sh_ms_delete_mod_folder(longdir, cause, sizeof cause) == SH_MS_DELETED &&
              strstr(cause, "its path is too long for the Recycle Bin"));
        CHECK(GetFileAttributesW(wlong) == INVALID_FILE_ATTRIBUTES);
    }
}

/* Mod folders at any depth: an organizing folder without mod.json is looked
 * into, a mod folder is not, and a nested mod is named by its path under
 * mods\. Building in place, matching, verifying and deleting take that path. */
/* Only "<name>.staging-<pid>" and "<name>.old-<pid>" are work folders; a mod
 * whose name merely contains ".old-" is a mod. */
static void test_work_folder_names(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH];
    sh_ms_list list;
    printf("-- work folders: only a .staging-<pid> or .old-<pid> suffix is skipped\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\worknames\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\doom.old-school", root);
    make_small_mod(dir, "alex.oldschool");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\my.staging-area", root);
    make_small_mod(dir, "alex.stagingarea");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alex.top.old-4242", root);
    make_small_mod(dir, "alex.top");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alex.top.staging-17", root);
    make_small_mod(dir, "alex.top");
    CHECK(sh_ms_enumerate(&list) && list.count == 2);
    CHECK(find_entry(&list, "doom.old-school") && find_entry(&list, "my.staging-area"));
    CHECK(!find_entry(&list, "alex.top.old-4242") && !find_entry(&list, "alex.top.staging-17"));
    sh_ms_list_free(&list);
}

static void test_nested(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH], cause[300], key[SH_MS_NAME_MAX + 1];
    sh_ms_list list;
    sh_ms_install_stats st;
    sh_ms_verify_stats vs;
    const sh_ms_entry *e;
    int r, ci = getenv("CI") != NULL;
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\nested\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alex.top", root);
    make_small_mod(dir, "alex.top");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\alex.cyber", root);
    make_small_mod(dir, "alex.cyber");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\bosses\\alex.spider", root);
    make_small_mod(dir, "alex.spider");
    /* A folder inside a mod is that mod's content, and an empty organizing
     * folder holds nothing. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alex.top\\inner", root);
    make_small_mod(dir, "alex.inner");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\empty-group\\deeper", root);
    mt_mkdirs_for(dir);
    CreateDirectoryA(dir, NULL);

    {
        char **mods = NULL;
        size_t count = 0;
        CHECK(sh_ms_find_mods(root, &mods, &count) && count == 3);
        CHECK(sh_ms_folder_is_mod(mods && count ? mods[0] : "") && !sh_ms_folder_is_mod(root));
        sh_ms_find_free(mods, count);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods", root);
        CHECK(sh_ms_find_mods(dir, &mods, &count) && count == 2);
        sh_ms_find_free(mods, count);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\empty-group", root);
        CHECK(sh_ms_find_mods(dir, &mods, &count) && count == 0 && mods == NULL);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\nowhere", root);
        CHECK(!sh_ms_find_mods(dir, &mods, &count));
    }
    CHECK(sh_ms_enumerate(&list));
    CHECK(list.count == 3);
    CHECK(find_entry(&list, "alex.top") != NULL);
    e = find_entry(&list, "demon-mods\\alex.cyber");
    CHECK(e != NULL);
    CHECK(find_entry(&list, "demon-mods\\bosses\\alex.spider") != NULL);
    CHECK(find_entry(&list, "alex.inner") == NULL && find_entry(&list, "alex.top\\inner") == NULL);
    /* A nested folder answers to its path, either slash, and to its own name. */
    CHECK(e && sh_ms_entry_matches(e, "demon-mods\\alex.cyber") && sh_ms_entry_matches(e, "demon-mods/alex.cyber") &&
          sh_ms_entry_matches(e, "alex.cyber") && !sh_ms_entry_matches(e, "alex.top"));
    sh_ms_list_free(&list);

    /* Built in place, by its path under mods\. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\alex.cyber", root);
    cause[0] = 0;
    CHECK(sh_ms_install_from_folder(dir, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  nested build: %s\n", cause);
    CHECK(st.in_place && !strcmp(key, "demon-mods\\alex.cyber"));
    CHECK(sh_ms_verify("alex.cyber", &vs, cause, sizeof cause) && vs.entries == 1);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "demon-mods\\alex.cyber");
    CHECK(e && e->loaded && !strcmp(e->bundle.id, "alex.cyber"));
    sh_ms_list_free(&list);
    /* The organizing folder itself is not a mod. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods", root);
    CHECK(!sh_ms_install_from_folder(dir, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause) &&
          strstr(cause, "has no mod.json"));
    CHECK(!sh_ms_delete_mod_folder(dir, cause, sizeof cause) && strstr(cause, "is not a mod folder of the store") && mt_exists(dir));

    /* Deleting a nested mod takes only its folder. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\bosses\\alex.spider", root);
    g_logged = 0;
    r = sh_ms_delete_mod_folder(dir, cause, sizeof cause);
    if (r != SH_MS_RECYCLED) fprintf(stderr, "  nested recycle: %d (%s)\n", r, cause);
    CHECK(ci ? r != 0 : r == SH_MS_RECYCLED);
    CHECK(!mt_exists(dir));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\bosses", root);
    CHECK(mt_exists(dir));
    if (r == SH_MS_RECYCLED) CHECK(log_has("MODS: store: moved mods\\demon-mods\\bosses\\alex.spider to the Recycle Bin"));
    CHECK(sh_ms_enumerate(&list) && list.count == 2);
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
    char h1[65], h0[65], d1[65] = "", d2[65] = "", d3[65] = "", k101[SH_MS_KEY_MAX + 1] = "";

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

    /* Install by hash (DESIGN-additive 4). Reinstalling unchanged content is
     * the bundle already present: nothing is copied, nothing replaced. */
    g_logged = 0;
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    CHECK(!strcmp(key, "alex.r2-imp") && st.present && !st.replaced && st.copied == 0);
    CHECK(log_has("is already installed as alex.r2-imp; nothing was copied"));
    CHECK(sh_ms_verify("alex.r2-imp", &vs, cause, sizeof cause));

    /* Changed content of the same version is another build of the key: it is
     * installed beside the first as <key>@<hash16>, never over it, and never
     * refused for "same version, different files". Both are listed, each
     * hash resolves to its own folder, and no alias joins them. */
    mt_append(src, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\imp_copy.decl", "\r\n// edited\r\n");
    mt_write(src, "mod.json", slurp(fixture, "mod.json", NULL));
    {
        char beside[SH_MS_KEY_MAX + 1], want[SH_MS_KEY_MAX + 1];
        unsigned char *first_before = (unsigned char *)slurp(root, "alex.r2-imp\\mod.json", NULL), *first_after;
        CHECK(sh_ms_install_from_folder(src, &RESOLVER, beside, sizeof beside, &st, &b, cause, sizeof cause));
        _snprintf_s(want, sizeof want, _TRUNCATE, "alex.r2-imp@%.16s", b.hash);
        CHECK(!strcmp(beside, want) && !st.replaced && !st.present && strcmp(b.hash, h1));
        CHECK(!strcmp(b.version, "1.0.0") && !strcmp(b.origin, "install"));
        first_after = (unsigned char *)slurp(root, "alex.r2-imp\\mod.json", NULL);
        CHECK(first_before && first_after && !strcmp((char *)first_before, (char *)first_after));   /* not rewritten */
        free(first_before); free(first_after);
        CHECK(sh_ms_enumerate(&list) && list.count == 2);
        e = sh_ms_resolve(&list, h1);
        CHECK(e && !strcmp(e->name, "alex.r2-imp") && !strcmp(e->bundle.hash, h1));
        e = sh_ms_resolve(&list, b.hash);
        CHECK(e && !strcmp(e->name, want));
        CHECK(!alias_of(&list, h1) && !alias_of(&list, b.hash));
        CHECK(!sh_ms_resolve(&list, "0000000000000000000000000000000000000000000000000000000000000000"));
        sh_ms_list_free(&list);
        /* Installed again: present, in its own folder. */
        CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
        CHECK(!strcmp(key, want) && st.present);
        /* Uninstalling one build leaves the other. */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", root, want);
        CHECK(sh_ms_remove_folder(path) && !mt_has(root, want));
        CHECK(sh_ms_enumerate(&list) && list.count == 1);
        e = sh_ms_resolve(&list, h1);
        CHECK(e && !strcmp(e->name, "alex.r2-imp") && sh_ms_entry_usable(e));
        CHECK(!sh_ms_resolve(&list, b.hash));
        sh_ms_list_free(&list);
        CHECK(sh_ms_verify("alex.r2-imp", &vs, cause, sizeof cause) && vs.entries == 1);
        sh_mb_free(&b);
    }
    /* The rule names folders: the first build of a key its key, every later
     * one <key>@<hash16>, a folder already holding the hash answers 2. */
    {
        char nm[SH_MS_KEY_MAX + 1];
        CHECK(sh_ms_build_folder_name(root, "alex.r2-imp", h1, 1, nm, sizeof nm) == 1);
        CHECK(!strncmp(nm, "alex.r2-imp@", 12) && strlen(nm) == 12 + 16);
        CHECK(sh_ms_build_folder_name(root, "alex.r2-imp", h1, 0, nm, sizeof nm) == 2 && !strcmp(nm, "alex.r2-imp"));
        CHECK(sh_ms_build_folder_name(root, "alex.fresh", h1, 0, nm, sizeof nm) == 1 && !strcmp(nm, "alex.fresh"));
    }

    /* A second version is another hash of the key: beside it, <key>@<hash16>. */
    {
        char *desc = slurp(fixture, "mod.json", NULL), *v = desc ? strstr(desc, "1.0.0") : NULL;
        if (v) v[4] = '1';
        mt_write(src, "mod.json", desc ? desc : "");
        free(desc);
    }
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    _snprintf_s(k101, sizeof k101, _TRUNCATE, "alex.r2-imp@%.16s", b.hash);
    CHECK(!strcmp(key, k101) && !st.replaced && !strcmp(b.version, "1.0.1"));
    sh_mb_free(&b);

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
                "%s\\%s\\assets\\generated\\basemodel\\md6\\mods\\alex.r2_imp\\imp_copy\\assets\\mesh\\imp_copy.bmd6model",
                root, k101);
    {
        FILE *f = NULL;
        if (fopen_s(&f, path, "r+b") == 0 && f) { fputc('X', f); fclose(f); }
    }
    CHECK(!sh_ms_verify(k101, &vs, cause, sizeof cause));
    CHECK(strstr(cause, "imp_copy.bmd6model does not match mod.json") != NULL);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, k101);
    CHECK(e && e->loaded && e->sizes_ok && !e->validated && !sh_ms_entry_usable(e));
    CHECK(e && strstr(e->cause, "does not match"));
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/alex.r2-imp/imp_copy.md6", &hit) == SH_MS_HIT);
    sh_ms_list_free(&list);
    {
        char mj[SH_MS_KEY_MAX + 16], *text;
        _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\mod.json", k101);
        text = slurp(root, mj, NULL);
        CHECK(text && strstr(text, "\"validated\": false") && strstr(text, "\"cause\": \"assets/"));
        free(text);
    }
    /* A missing file shows at enumeration without hashing. */
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\%s\\assets\\generated\\image\\mods\\alex.r2_imp\\imp_icon.bimage", root, k101);
    DeleteFileA(path);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, k101);
    CHECK(e && !e->sizes_ok && strstr(e->cause, "missing"));
    sh_ms_list_free(&list);

    /* Removed: one folder of the id is left. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", root, k101);
    CHECK(sh_ms_remove_folder(path) && !mt_has(root, k101));

    /* An in-place rebuild of a store folder (the author edits it) keeps its
     * name and makes the earlier hash an alias, so maps pinning it load. */
    mt_append(root, "alex.r2-imp\\assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\imp_copy.decl", "\r\n// in place\r\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp", root);
    CHECK(sh_ms_install_from_folder(path, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    CHECK(st.in_place && !strcmp(key, "alex.r2-imp") && strcmp(b.hash, h0));
    CHECK(sh_ms_enumerate(&list) && alias_of(&list, h0) && !strcmp(alias_of(&list, h0), b.hash));
    sh_ms_list_free(&list);
    strcpy_s(h1, sizeof h1, b.hash);
    sh_mb_free(&b);

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
    /* Snapshot: build 2's carried files and mod.json under .builds; build 1's
     * snapshot was replaced; the entry names it. */
    {
        char s1[MAX_PATH * 2], s2[MAX_PATH * 2], got[MAX_PATH * 4];
        char *t;
        _snprintf_s(s1, sizeof s1, _TRUNCATE, "%s\\.builds\\alex.dropped@%.16s", root, d1);
        _snprintf_s(s2, sizeof s2, _TRUNCATE, "%s\\.builds\\alex.dropped@%.16s", root, d2);
        CHECK(!mt_exists(s1) && mt_has(s2, "bundle.json"));
        t = slurp(s2, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl", NULL);
        CHECK(t && strstr(t, "// edited"));
        free(t);
        t = slurp(s2, "bundle.json", NULL);
        CHECK(t && strstr(t, d2));
        free(t);
        CHECK(sh_ms_snapshot_of(hand, got, sizeof got) && !_stricmp(got, s2));
        CHECK(e && e->snapshot && !_stricmp(e->snapshot, s2));
        t = slurp(root, "snapmap-plus.json", NULL);
        CHECK(t && strstr(t, "\"snapshots\"") && strstr(t, "\"My Dropped Mod\""));
        free(t);
    }
    sh_ms_list_free(&list);
    /* Last good build: a refused rebuild leaves mod.json as it was, so build 2
     * keeps serving, and the failure with its cause is the folder's state. */
    {
        char before[MAX_PATH * 2];
        size_t n1 = 0, n2 = 0;
        unsigned char *t1, *t2;
        _snprintf_s(before, sizeof before, _TRUNCATE, "%s\\mod.json", hand);
        t1 = sh_mb_read_file(before, 1 << 20, &n1);
        mt_write(hand, "assets\\generated\\decls\\entitydef\\other\\x.decl", "{\r\n}");
        /* the author also edits a carried file in place, to another size */
        mt_write(hand, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl",
                 "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n\t// half-way through an edit\r\n}");
        g_logged = 0;
        CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 1);
        CHECK(log_has("My Dropped Mod build failed: ") && log_has("; serving the last good build (bundle "));
        t2 = sh_mb_read_file(before, 1 << 20, &n2);
        CHECK(t1 && t2 && n1 == n2 && !memcmp(t1, t2, n1));   /* not touched, still validated */
        free(t1);
        free(t2);
    }
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    CHECK(e && sh_ms_entry_usable(e) && e->bundle.build == 2 && !strcmp(e->bundle.hash, d2) && !e->cause[0]);
    CHECK(e && strstr(e->build_failed, "outside mods/alex.dropped/"));
    /* The failed folder points at its snapshot, which still holds build 2's
     * bytes (the sizes were checked there, not in the edited folder). */
    {
        char s2[MAX_PATH * 2];
        char *t;
        _snprintf_s(s2, sizeof s2, _TRUNCATE, "%s\\.builds\\alex.dropped@%.16s", root, d2);
        CHECK(e && e->snapshot && !_stricmp(e->snapshot, s2));
        t = slurp(s2, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl", NULL);
        CHECK(t && strstr(t, "// edited") && !strstr(t, "half-way"));
        free(t);
    }
    sh_ms_list_free(&list);
    /* The failure is asked again at every rescan until a build succeeds. */
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 1 && ss.rebuilt == 0);
    mt_delete(hand, "assets\\generated\\decls\\entitydef\\other\\x.decl");
    mt_write(hand, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n\t// edited\r\n}");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1 && ss.failed == 0);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "My Dropped Mod");
    CHECK(e && sh_ms_entry_usable(e) && e->bundle.build == 2 && !e->build_failed[0]);
    sh_ms_list_free(&list);
    /* A folder that never built stays skipped, with its cause. */
    {
        char never[MAX_PATH * 2];
        _snprintf_s(never, sizeof never, _TRUNCATE, "%s\\never-built", root);
        mt_write(never, "mod.json", "{\"name\":\"Never\"}");
        mt_write(never, "assets\\generated\\decls\\entitydef\\other\\y.decl", "{\r\n}");
        CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 1);
        /* Its first build gave it a generated key (never_built_xxxx) before refusing its files. */
        CHECK(!mt_exists(never) && fb_find(root, "never_built_", never, sizeof never) == 1);
        CHECK(sh_ms_enumerate(&list));
        e = find_entry(&list, strrchr(never, '\\') + 1);
        CHECK(e && !e->loaded && !sh_ms_entry_usable(e) && strstr(e->cause, "outside mods/never_built_"));
        sh_ms_list_free(&list);
        CHECK(sh_ms_remove_folder(never));
        CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0);
    }
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
    /* Build 3 replaced build 2's snapshot. */
    {
        char s2[MAX_PATH * 2], s3[MAX_PATH * 2];
        _snprintf_s(s2, sizeof s2, _TRUNCATE, "%s\\.builds\\alex.dropped@%.16s", root, d2);
        _snprintf_s(s3, sizeof s3, _TRUNCATE, "%s\\.builds\\alex.dropped@%.16s", root, d3);
        CHECK(!mt_exists(s2) && mt_has(s3, "bundle.json"));
    }
    /* sh_mods install of a store folder builds it in place. */
    CHECK(sh_ms_install_from_folder(hand, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    CHECK(st.in_place && !strcmp(key, "My Dropped Mod"));

    /* New mod: a folder named by a generated key, which its product block
     * records; the first build keeps it, and the descriptor has no id. */
    {
        char nk[SH_MB_ID_MAX + 1], nf[MAX_PATH * 2], rel[MAX_PATH];
        unsigned char *text;
        CHECK(sh_ms_new_mod("Hell \"Guards\"", nk, nf, sizeof nf, cause, sizeof cause));
        if (!nk[0]) fprintf(stderr, "  new mod: %s\n", cause);
        CHECK(!strncmp(nk, "hell_guards_", 12) && strlen(nk) == 16);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\mods\\%s", nk);
        CHECK(mt_has(nf, "mod.json") && mt_has(nf, rel));
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s\\mod.json", nf);
        text = sh_mb_read_file(rel, 1 << 16, NULL);
        CHECK(text && strstr((const char *)text, "\"name\": \"Hell \\\"Guards\\\"\"") && !strstr((const char *)text, "\"id\""));
        free(text);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\%s\\guard.decl", nk);
        mt_write(nf, rel, "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n}");
        g_logged = 0;
        CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0 && ss.rebuilt == 1);
        CHECK(sh_ms_enumerate(&list));
        e = find_entry(&list, nk);
        CHECK(e && sh_ms_entry_usable(e) && !strcmp(e->bundle.id, nk) && e->bundle.key_source == SH_MB_KEY_FROM_BLOCK &&
              !e->bundle.origin[0] && e->bundle.classes.adds == 1 && !strcmp(e->bundle.name, "Hell \"Guards\""));
        sh_ms_list_free(&list);
        {
            char snap[MAX_PATH * 4];
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s\\%s", root, nk);
            CHECK(sh_ms_snapshot_of(rel, snap, sizeof snap) && strstr(snap, "\\.builds\\") && mt_exists(snap));
            CHECK(sh_ms_remove_folder(rel));
            /* removal takes the snapshot with it */
            CHECK(snap[0] && !mt_exists(snap));
            CHECK(!sh_ms_snapshot_of(rel, snap, sizeof snap));
        }
        CHECK(!sh_ms_new_mod("   ", nk, nf, sizeof nf, cause, sizeof cause) && strstr(cause, "needs a name"));
        CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0);
    }

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
    test_recycle(tmp);
    test_nested(tmp);
    test_work_folder_names(tmp);
    test_first_build(tmp);

    test_upgrade(fixture, tmp);
    test_boot_sync(fixture, tmp);
    test_boot_release(tmp);

    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    if (failures) { fprintf(stderr, "modstore_test: %d failure(s)\n", failures); return 1; }
    printf("modstore_test: install, copies, verify, corruption, second version, rebuild rule, resolve_need, lock, rebuild aliases, the Recycle Bin, the second upgrade step and "
           "store upgrade pass\n");
    return 0;
}
