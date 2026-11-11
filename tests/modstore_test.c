/* modstore_test.c -- the mods\ store in the one shape (ONE-SHAPE rules 1 to
 * 4): install with real copies, enumeration and verification, the salt (a
 * new folder salted at the scan with its own references rewritten and its
 * name kept; a binary naming the folder, files under another mod and a
 * refused swap leave it untouched; a renamed folder keeps its salt; a copied
 * folder gets a new one by "built", ties by path; an installed copy is the
 * newer), earlier shapes refused with their reason and nothing written, the
 * places under mods\maps\<map>\<mod> (one folder per salt per place, never
 * built), New mod, the rebuild rule from the block's tree, the store lock,
 * the boot scan, crash leftovers, nested folders, removal through the Recycle
 * Bin.
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
#include "store_lock.h"

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
static void log_dump(const char *what)
{
    unsigned i, n = g_logged < 64 ? g_logged : 64;
    fprintf(stderr, "  log (%s):\n", what);
    for (i = 0; i < n; i++) fprintf(stderr, "    %s\n", g_log[i]);
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
    "generated/decls/entitydef/prop/prop_base.decl",
    "generated/decls/snapeditorentitydef/prop/prop_base.decl",
    "models/snapmaps/props/new/crate01.lwo",
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
static int game_has(void *ctx, const char *request) { (void)ctx; (void)request; return 1; }
static const sh_mb_resolver GAME = { NULL, NULL, game_has, NULL };

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

static int same_text(const char *a, const char *b) { return a && b && !strcmp(a, b); }

/* The entry of `name` after a fresh enumeration: its salt and build hash. */
static int entry_of(const char *name, char salt[SH_SALT_LEN + 1], char hash[65], int *usable)
{
    sh_ms_list l;
    const sh_ms_entry *e;
    int found = 0;
    if (salt) salt[0] = 0;
    if (hash) hash[0] = 0;
    if (usable) *usable = 0;
    if (!sh_ms_enumerate(&l)) return 0;
    e = find_entry(&l, name);
    if (e) {
        found = 1;
        if (salt) strcpy_s(salt, SH_SALT_LEN + 1, e->salt);
        if (hash) strcpy_s(hash, 65, e->loaded ? e->bundle.hash : "");
        if (usable) *usable = sh_ms_entry_usable(e);
        if (!sh_ms_entry_usable(e)) fprintf(stderr, "  entry %s: %s\n", name, e->cause);
    }
    sh_ms_list_free(&l);
    return found;
}

/* A new mod by hand: an author part only, one file under mods/<leaf>/. */
static void make_small_mod(const char *dir, const char *leaf)
{
    char text[256], rel[MAX_PATH];
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"name\":\"%s\",\"version\":\"1.0.0\",\"author\":\"tester\"}", leaf);
    mt_write(dir, "mod.json", text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\a.bimage", leaf);
    _snprintf_s(text, sizeof text, _TRUNCATE, "bytes of %s for the store test", leaf);
    mt_write(dir, rel, text);
}

#define FB_BASE "assets\\generated\\decls\\entitydef\\mods\\%s\\probe_base.decl"
#define FB_ROW  "assets\\generated\\decls\\entitydef\\mods\\%s\\probe.decl"

/* A new mod named `leaf`: two decls under mods/<leaf>/, one inheriting the
 * other, and a mod.json value naming mods/<leaf>/. (Its strings cannot be
 * keyed before it has a salt: a key is <salt>_<name>, DECISIONS 5.) */
static void fb_make(const char *dir, const char *leaf)
{
    char rel[MAX_PATH], text[600];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\r\n  \"name\": \"Cyber probe\",\r\n  \"version\": \"1.0.0\",\r\n"
                "  \"description\": \"A probe (mods/%s/probe).\"\r\n}\r\n", leaf);
    mt_write(dir, "mod.json", text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_BASE, leaf);
    mt_write(dir, rel, "{\r\n\tinherit = \"monster/imp\";\r\n}");
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, leaf);
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\r\n\tinherit = \"mods/%s/probe_base\";\r\n}", leaf);
    mt_write(dir, rel, text);
}

/* Every entry of `parent` (files and folders, names only) as one line. */
static void dir_names(const char *parent, char *out, size_t cap)
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

/* --------------------------------------------------------- install, list */

static void test_install(const char *fixture, const char *tmp)
{
    char root[MAX_PATH], src[MAX_PATH], path[MAX_PATH * 2], name[SH_MS_NAME_MAX + 1], cause[600] = "";
    char h1[65], salt[SH_SALT_LEN + 1];
    sh_ms_install_stats st;
    sh_ms_verify_stats vs;
    sh_ms_list list;
    sh_ms_hit hit;
    sh_mb_bundle b;
    const sh_ms_entry *e;
    char *before;
    printf("-- install: real copies under the folder's own name, by salt and build\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\install\\mods", tmp);
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\install\\authoring\\alex.r2-imp", tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\install\\authoring", tmp);
    mt_mkdirs_for(src);
    CreateDirectoryA(path, NULL);
    CHECK(mt_copy_tree(fixture, src));
    CHECK(sh_ms_set_root(root));
    CHECK(sh_ms_enumerate(&list) && list.count == 0);
    sh_ms_list_free(&list);
    before = slurp(src, "mod.json", NULL);

    CHECK(sh_ms_install_from_folder(src, &RESOLVER, name, sizeof name, &st, &b, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  install: %s\n", cause);
    CHECK(!strcmp(name, "alex.r2-imp") && !strcmp(b.id, "alexrimp"));
    CHECK(st.copied == 13 && !st.in_place && !st.replaced && !st.present && !st.resalted);
    { char *after = slurp(src, "mod.json", NULL); CHECK(same_text(before, after)); free(after); }   /* the source is not written */
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp\\assets\\generated\\decls\\md6def\\mods\\alexrimp\\imp_copy.md6.decl", root);
    CHECK(links_of(path) == 1 && !(GetFileAttributesA(path) & FILE_ATTRIBUTE_READONLY));
    strcpy_s(h1, sizeof h1, b.hash);
    sh_mb_free(&b);

    CHECK(sh_ms_enumerate(&list) && list.count == 1);
    e = find_entry(&list, "alex.r2-imp");
    CHECK(e && sh_ms_entry_usable(e) && e->installed_at > 0 && !strcmp(e->salt, "alexrimp") && !e->place[0]);
    CHECK(e && e->state == SH_MB_DESC_BUILT && !strcmp(e->key, "alexrimp") && e->bundle.file_count == 13);
    CHECK(e && sh_ms_entry_matches(e, "alex.r2-imp") && sh_ms_entry_matches(e, "ALEXRIMP") && !sh_ms_entry_matches(e, "alex"));
    CHECK(sh_ms_resolve(&list, h1) == e && sh_ms_find_salt(&list, "alexrimp", NULL, NULL) == e);
    CHECK(sh_ms_find_salt(&list, "alexrimp", NULL, h1) == e && !sh_ms_find_salt(&list, "alexrimp", "*", NULL));
    CHECK(!sh_ms_resolve(&list, "0000000000000000000000000000000000000000000000000000000000000000"));
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/alexrimp/imp_copy.md6", &hit) == SH_MS_HIT && hit.providers == 1);
    CHECK(sh_ms_resolve_need(&list, "MD6DEF", "Mods/AlexRimp/Imp_Copy.md6", &hit) == SH_MS_HIT);
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/nobody/x.md6", &hit) == SH_MS_MISS);
    CHECK(sh_ms_request_exists(&list, "generated/decls/entitydef/mods/alexrimp/imp_copy.decl") == 1);
    CHECK(sh_ms_request_exists(&list, "generated/decls/entitydef/mods/alexrimp/none.decl") == 0);
    sh_ms_list_free(&list);

    CHECK(sh_ms_verify("alexrimp", &vs, cause, sizeof cause) && vs.entries == 1 && vs.files == 13);
    CHECK(!sh_ms_verify("nobody", &vs, cause, sizeof cause) && strstr(cause, "no installed mod"));

    /* The same build again: present, nothing copied. */
    g_logged = 0;
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause));
    CHECK(!strcmp(name, "alex.r2-imp") && st.present && st.copied == 0);
    CHECK(log_has("is already installed as mods\\alex.r2-imp; nothing was copied"));

    /* Another build of the salt from elsewhere is the newer copy (R9): it
     * lands beside the first under its own name and gets a new salt. */
    mt_append(src, "assets\\generated\\decls\\entitydef\\mods\\alexrimp\\imp_copy.decl", "\r\n// edited\r\n");
    g_logged = 0;
    cause[0] = 0;
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, name, sizeof name, &st, &b, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  copy install: %s\n", cause);
    CHECK(!strcmp(name, "alex.r2-imp (2)") && st.resalted && strcmp(b.id, "alexrimp") && sh_salt_valid(b.id, strlen(b.id)));
    CHECK(log_has("mods\\alex.r2-imp (2) is a copy of mods\\alex.r2-imp; it is a new mod now"));
    strcpy_s(salt, sizeof salt, b.id);
    sh_mb_free(&b);
    {
        char rel[MAX_PATH];
        char *t;
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "alex.r2-imp (2)\\assets\\generated\\decls\\entitydef\\mods\\%s\\imp_copy.decl", salt);
        t = slurp(root, rel, NULL);
        CHECK(t && strstr(t, "// edited") && !strstr(t, "mods/alexrimp/"));
        free(t);
    }
    CHECK(entry_of("alex.r2-imp", NULL, NULL, &st.present) && st.present);   /* the first is untouched */

    /* Corruption: detected, recorded in mod.json, and that folder stops serving. */
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp\\assets\\generated\\basemodel\\md6\\mods\\alexrimp\\imp_copy\\assets\\mesh\\imp_copy.bmd6model", root);
    { FILE *f = NULL; if (fopen_s(&f, path, "r+b") == 0 && f) { fputc('X', f); fclose(f); } }
    CHECK(!sh_ms_verify("alex.r2-imp", &vs, cause, sizeof cause) && strstr(cause, "imp_copy.bmd6model does not match mod.json"));
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp");
    CHECK(e && e->loaded && e->sizes_ok && !e->validated && !sh_ms_entry_usable(e) && strstr(e->cause, "does not match"));
    sh_ms_list_free(&list);
    { char *t = slurp(root, "alex.r2-imp\\mod.json", NULL); CHECK(t && strstr(t, "\"cause\": \"assets/")); free(t); }
    /* A missing file shows at enumeration without hashing. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp\\assets\\generated\\image\\mods\\alexrimp\\imp_icon.bimage", root);
    DeleteFileA(path);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp");
    CHECK(e && !e->sizes_ok && strstr(e->cause, "missing"));
    sh_ms_list_free(&list);
}

/* -------------------------------------------------------------- the salt */

static void test_fresh_folder_salted(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH], salt[SH_SALT_LEN + 1], hash[65], rel[MAX_PATH], *t;
    sh_ms_sync_stats ss;
    int usable = 0;
    printf("-- salt: a new folder gets its salt at the scan, keeps its name, its references follow\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\fresh\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\cyberprobe", root);
    fb_make(dir, "cyberprobe");
    CHECK(entry_of("cyberprobe", salt, hash, &usable) && !salt[0] && !usable);
    CHECK(sh_ms_sync_pending() == 1);
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.salted == 1 && ss.rebuilt == 1 && ss.failed == 0);
    if (ss.failed || !ss.rebuilt) log_dump("fresh");
    CHECK(log_has("mods\\cyberprobe is a new mod: salt "));
    CHECK(entry_of("cyberprobe", salt, hash, &usable) && sh_salt_valid(salt, strlen(salt)) && usable);
    t = slurp(dir, "mod.json", NULL);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "(mods/%s/probe)", salt);
    CHECK(t && strstr(t, rel) && !strstr(t, "mods/cyberprobe/") && strstr(t, "\"salt\""));
    free(t);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, salt);
    t = slurp(dir, rel, NULL);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "inherit = \"mods/%s/probe_base\"", salt);
    CHECK(t && strstr(t, rel));
    free(t);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_BASE, "cyberprobe");
    CHECK(!mt_has(dir, rel));
    /* Unchanged: the next scan builds nothing. */
    CHECK(sh_ms_sync_pending() == 0 && sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 0 && ss.salted == 0);
    /* An edit under assets\ is a new tree: built again, the same salt. */
    {
        char h2[65], s2[SH_SALT_LEN + 1];
        _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_BASE, salt);
        mt_append(dir, rel, "\r\n// edited\r\n");
        CHECK(sh_ms_sync_pending() == 1);
        CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1);
        CHECK(entry_of("cyberprobe", s2, h2, &usable) && usable && !strcmp(s2, salt) && strcmp(h2, hash));
        strcpy_s(hash, sizeof hash, h2);
    }
    /* Renaming the folder changes nothing: same salt, same build. */
    {
        char moved[MAX_PATH], h2[65], s2[SH_SALT_LEN + 1];
        _snprintf_s(moved, sizeof moved, _TRUNCATE, "%s\\Cyber Probe (renamed)", root);
        CHECK(MoveFileA(dir, moved));
        CHECK(sh_ms_sync_pending() == 0 && sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 0 && ss.salted == 0);
        CHECK(entry_of("Cyber Probe (renamed)", s2, h2, &usable) && usable && !strcmp(s2, salt) && !strcmp(h2, hash));
        CHECK(MoveFileA(moved, dir));
    }
    /* A failed build serves nothing and records its cause in the block. */
    {
        char *mj;
        _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, salt);
        mt_write(dir, rel, "{\r\n\tinherit = \"mods/nobody/missing\";\r\n}");
        g_logged = 0;
        CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 1 && ss.rebuilt == 0);
        CHECK(entry_of("cyberprobe", NULL, NULL, &usable) && !usable);
        mj = slurp(dir, "mod.json", NULL);
        CHECK(mj && strstr(mj, "\"cause\"") && !strstr(mj, "\"build\"") && strstr(mj, salt));
        free(mj);
        CHECK(sh_ms_sync_pending() == 1);   /* tried again at every scan */
        /* Fixed: it builds again under the same salt. */
        _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, salt);
        _snprintf_s(hash, sizeof hash, _TRUNCATE, "{\r\n\tinherit = \"mods/%s/probe_base\";\r\n}", salt);
        mt_write(dir, rel, hash);
        CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1 && ss.failed == 0);
        CHECK(entry_of("cyberprobe", NULL, NULL, &usable) && usable);
    }
}

/* DECISIONS 6: a new folder's plain strings keys gain the salt's prefix at
 * its first scan, and its #str_ references follow; a copy's keys follow its
 * new salt; after the first scan a key without the prefix is refused. */
static void test_fresh_folder_strings(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH], copy[MAX_PATH], salt[SH_SALT_LEN + 1], s2[SH_SALT_LEN + 1], rel[MAX_PATH];
    char want[256], *t;
    sh_ms_sync_stats ss;
    int usable = 0;
    printf("-- salt: a new folder's strings keys gain <salt>_ and its #str_ references follow; a copy's follow its salt\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\strings\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\talker", root);
    fb_make(dir, "talker");
    mt_write(dir, "mod.json",
             "{\r\n  \"name\": \"Talker\",\r\n  \"version\": \"1.0.0\",\r\n  \"strings\": {\r\n    \"en\": {\r\n"
             "      \"probe_name\": \"Probe\",\r\n      \"probe_desc\": \"Says #str_probe_name.\",\r\n"
             "      \"snapentity_demon_baron\": \"Big Baron\",\r\n      \"sh_bv_navigation_desc\": \"Mine\"\r\n"
             "    }\r\n  }\r\n}\r\n");
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, "talker");
    mt_write(dir, rel, "{\r\n\tinherit = \"mods/talker/probe_base\";\r\n\tdisplayName = \"#str_probe_name\";\r\n"
                       "\tdescription = \"#str_probe_desc\";\r\n\tother = \"#str_probe_names\";\r\n"
                       "\tbaron = \"#str_snapentity_demon_baron\";\r\n}");
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.salted == 1 && ss.rebuilt == 1 && ss.failed == 0);
    if (ss.failed || !ss.salted) log_dump("strings");
    CHECK(log_has("2 strings key(s) named "));
    CHECK(entry_of("talker", salt, NULL, &usable) && usable && sh_salt_valid(salt, strlen(salt)));
    t = slurp(dir, "mod.json", NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s_probe_name\": \"Probe\"", salt);
    CHECK(t && strstr(t, want));
    _snprintf_s(want, sizeof want, _TRUNCATE, "Says #str_%s_probe_name.", salt);
    CHECK(t && strstr(t, want) && !strstr(t, "\"probe_desc\""));
    /* DECISIONS 9: a key the game (snapentity_demon_baron) or Snapmap+
     * (sh_bv_navigation_desc) defines is a string edit and keeps its name. */
    CHECK(t && strstr(t, "\"snapentity_demon_baron\": \"Big Baron\"") && strstr(t, "\"sh_bv_navigation_desc\": \"Mine\""));
    free(t);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, salt);
    t = slurp(dir, rel, NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "displayName = \"#str_%s_probe_name\"", salt);
    CHECK(t && strstr(t, want));
    _snprintf_s(want, sizeof want, _TRUNCATE, "description = \"#str_%s_probe_desc\"", salt);
    CHECK(t && strstr(t, want));
    CHECK(t && strstr(t, "other = \"#str_probe_names\""));   /* not one of its keys: a name boundary */
    CHECK(t && strstr(t, "baron = \"#str_snapentity_demon_baron\""));   /* a string edit's reference stays */
    free(t);
    /* A copy: its keys and references follow its new salt. */
    _snprintf_s(copy, sizeof copy, _TRUNCATE, "%s\\talker - Copy", root);
    CHECK(mt_copy_tree(dir, copy));
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.resalted == 1 && ss.failed == 0);
    CHECK(entry_of("talker - Copy", s2, NULL, &usable) && usable && strcmp(s2, salt));
    t = slurp(copy, "mod.json", NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s_probe_name\"", s2);
    CHECK(t && strstr(t, want));
    _snprintf_s(want, sizeof want, _TRUNCATE, "%s_", salt);
    CHECK(t && !strstr(t, want));
    free(t);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, s2);
    t = slurp(copy, rel, NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "#str_%s_probe_desc", s2);
    CHECK(t && strstr(t, want));
    free(t);
    /* A key added without the prefix after the first scan gains it at the
     * next scan, and the folder builds (DECISIONS 20). */
    t = slurp(dir, "mod.json", NULL);
    if (t) {
        char *at = strstr(t, "\"en\": {");
        if (at) {
            size_t n = strlen(t) + 64;
            char *edited = (char *)malloc(n);
            if (edited) {
                _snprintf_s(edited, n, _TRUNCATE, "%.*s\"en\": {\r\n      \"late_key\": \"Late\",%s", (int)(at - t), t, at + 7);
                mt_write(dir, "mod.json", edited);
                free(edited);
            }
        }
        free(t);
    }
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0);
    t = slurp(dir, "mod.json", NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s_late_key\": \"Late\"", salt);
    CHECK(t && strstr(t, want) && strstr(t, "\"build\""));
    free(t);
}

static void test_fresh_folder_refusals(const char *tmp)
{
    char root[MAX_PATH], bin[MAX_PATH], foreign[MAX_PATH], good[MAX_PATH], before[2048], after[2048], cause[600];
    char *mj0, *mj1, salt[SH_SALT_LEN + 1];
    sh_ms_sync_stats ss;
    sh_ms_list list;
    const sh_ms_entry *e;
    int usable;
    printf("-- salt: a binary naming the folder, or files under another mod, refuse that folder alone, untouched\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\refusals\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(bin, sizeof bin, _TRUNCATE, "%s\\binprobe", root);
    fb_make(bin, "binprobe");
    {
        /* A cooked binary (NUL bytes) that names mods/binprobe/. */
        char path[MAX_PATH * 2];
        FILE *f = NULL;
        static const char bytes[] = "MD6\0\0\0material mods/binprobe/skin\0";
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\generated\\basemodel\\md6\\mods\\binprobe\\m.bmd6model", bin);
        mt_mkdirs_for(path);
        if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(bytes, 1, sizeof bytes, f); fclose(f); }
    }
    _snprintf_s(foreign, sizeof foreign, _TRUNCATE, "%s\\foreignprobe", root);
    fb_make(foreign, "someoneelse");
    mt_write(foreign, "mod.json", "{\"name\":\"Foreign\"}");
    _snprintf_s(good, sizeof good, _TRUNCATE, "%s\\goodprobe", root);
    fb_make(good, "goodprobe");
    dir_names(bin, before, sizeof before);
    mj0 = slurp(bin, "mod.json", NULL);
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.salted == 1 && ss.rebuilt == 1 && ss.failed == 2);
    dir_names(bin, after, sizeof after);
    mj1 = slurp(bin, "mod.json", NULL);
    CHECK(!strcmp(before, after) && same_text(mj0, mj1));   /* byte for byte */
    free(mj0);
    free(mj1);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "binprobe");
    CHECK(e && !sh_ms_entry_usable(e) &&
          strstr(e->cause, "m.bmd6model names mods/binprobe/ inside a binary file, which Snapmap+ cannot rename") &&
          strstr(e->cause, "New mod"));
    if (e) printf("   %s\n", e->cause);
    e = find_entry(&list, "foreignprobe");
    CHECK(e && !sh_ms_entry_usable(e) && strstr(e->cause, "are under mods/someoneelse/") &&
          strstr(e->cause, "not this folder's name"));
    sh_ms_list_free(&list);
    CHECK(entry_of("goodprobe", salt, NULL, &usable) && usable && salt[0]);
    /* Called directly: refused with the cause; an already salted folder answers 0. */
    CHECK(sh_ms_assign_salt(bin, "binprobe", salt, cause, sizeof cause) == -1 && strstr(cause, "binary file"));
    CHECK(sh_ms_assign_salt(good, "goodprobe", salt, cause, sizeof cause) == 0 && sh_salt_valid(salt, strlen(salt)));
    /* A swap the system refuses: a file held open without delete sharing
     * makes the rename of the folder fail; nothing changes. */
    {
        char path[MAX_PATH * 2], rel[MAX_PATH * 2], *t;
        HANDLE h;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\heldprobe", root);
        fb_make(path, "heldprobe");
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s\\assets\\generated\\decls\\entitydef\\mods\\heldprobe\\probe_base.decl", path);
        h = CreateFileA(rel, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(h != INVALID_HANDLE_VALUE);
        dir_names(path, before, sizeof before);
        CHECK(sh_ms_assign_salt(path, "heldprobe", salt, cause, sizeof cause) == -1);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        dir_names(path, after, sizeof after);
        t = slurp(path, "mod.json", NULL);
        CHECK(!strcmp(before, after) && t && !strstr(t, "salt"));
        free(t);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "heldprobe.staging-%lu", GetCurrentProcessId());
        CHECK(!mt_has(root, rel));
        mt_remove_tree(path);
    }
    /* A read failure: a file no one may read leaves the folder as it was. */
    {
        char path[MAX_PATH * 2], rel[MAX_PATH * 2], *t;
        HANDLE h;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\lockedprobe", root);
        fb_make(path, "lockedprobe");
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s\\assets\\generated\\decls\\entitydef\\mods\\lockedprobe\\probe.decl", path);
        h = CreateFileA(rel, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(h != INVALID_HANDLE_VALUE);
        CHECK(sh_ms_assign_salt(path, "lockedprobe", salt, cause, sizeof cause) == -1 && strstr(cause, "could not be read"));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        t = slurp(path, "mod.json", NULL);
        CHECK(t && !strstr(t, "salt"));
        free(t);
        mt_remove_tree(path);
    }
}

/* An outside converter (Doomforge) writes a new folder already salted: its
 * builder part is only {"format":2,"salt":"<salt>"}, and its binaries name
 * that salt. The first scan keeps the salt and builds it; a second such folder
 * of the same salt is a copy of the first, and as its binaries name the salt
 * it cannot be given another: it is refused with a plain cause, untouched. */
static void conv_make(const char *dir, const char *salt)
{
    char rel[MAX_PATH * 2], text[600], path[MAX_PATH * 2];
    FILE *f = NULL;
    static const char bytes[] = "MD6\0\0\0material mods/cvsalt00/skin\0";
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\r\n  \"name\": \"Converted demon\",\r\n  \"snapmap-plus\": {\"format\": 2, \"salt\": \"%s\"}\r\n}\r\n", salt);
    mt_write(dir, "mod.json", text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_BASE, salt);
    mt_write(dir, rel, "{\r\n\tinherit = \"monster/imp\";\r\n}");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\generated\\basemodel\\md6\\mods\\%s\\m.bmd6model", dir, salt);
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(bytes, 1, sizeof bytes, f); fclose(f); }
}

static void test_converter_salt(const char *tmp)
{
    char root[MAX_PATH], a[MAX_PATH], b[MAX_PATH], salt[SH_SALT_LEN + 1], before[2048], after[2048], *mj0, *mj1;
    sh_ms_sync_stats ss;
    sh_ms_list list;
    const sh_ms_entry *e;
    int usable;
    printf("-- salt: a new folder an outside converter salted keeps its salt; a second one of that salt is refused\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\converted\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\Converted demon", root);
    conv_make(a, "cvsalt00");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1 && ss.failed == 0 && ss.resalted == 0);
    CHECK(entry_of("Converted demon", salt, NULL, &usable) && usable && !strcmp(salt, "cvsalt00"));
    /* The same converter output again, in another folder. */
    _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\Converted demon again", root);
    conv_make(b, "cvsalt00");
    dir_names(b, before, sizeof before);
    mj0 = slurp(b, "mod.json", NULL);
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.resalted == 0);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "Converted demon again");
    CHECK(e && !sh_ms_entry_usable(e) && strstr(e->cause, "inside a binary file"));
    if (e) printf("   %s\n", e->cause);
    CHECK(sh_ms_entry_usable(find_entry(&list, "Converted demon")));
    sh_ms_list_free(&list);
    dir_names(b, after, sizeof after);
    mj1 = slurp(b, "mod.json", NULL);
    CHECK(!strcmp(before, after) && same_text(mj0, mj1));
    free(mj0);
    free(mj1);
    CHECK(entry_of("Converted demon", salt, NULL, &usable) && usable && !strcmp(salt, "cvsalt00"));
}

static void test_copy_gets_new_salt(const char *tmp)
{
    char root[MAX_PATH], a[MAX_PATH], b[MAX_PATH], c[MAX_PATH], sa[SH_SALT_LEN + 1], sb[SH_SALT_LEN + 1], sc[SH_SALT_LEN + 1];
    char ha[65], hb[65], rel[MAX_PATH], *mj0, *mj1, *t;
    sh_ms_sync_stats ss;
    sh_ms_list list;
    const sh_ms_entry *e;
    int usable;
    printf("-- copy: two root folders of one salt; the one built first keeps it, ties by path\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\copies\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\guards", root);
    fb_make(a, "guards");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 1);
    CHECK(entry_of("guards", sa, ha, &usable) && usable);
    /* Copied in Explorer: same block, same built, same files. */
    _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\guards - Copy", root);
    CHECK(mt_copy_tree(a, b));
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "guards - Copy");
    CHECK(e && e->refused && strstr(e->cause, "is a copy of mods\\guards"));
    CHECK(sh_ms_entry_usable(find_entry(&list, "guards")));
    sh_ms_list_free(&list);
    CHECK(sh_ms_sync_pending() == 1);
    mj0 = slurp(a, "mod.json", NULL);
    g_logged = 0;
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.resalted == 1 && ss.rebuilt == 1 && ss.failed == 0);
    if (ss.failed) log_dump("copy");
    CHECK(log_has("MODS: store: mods\\guards - Copy is a copy of mods\\guards; it is a new mod now"));
    mj1 = slurp(a, "mod.json", NULL);
    CHECK(same_text(mj0, mj1));   /* the original is untouched */
    free(mj0);
    free(mj1);
    CHECK(entry_of("guards", sb, hb, &usable) && usable && !strcmp(sb, sa) && !strcmp(hb, ha));
    CHECK(entry_of("guards - Copy", sb, hb, &usable) && usable && strcmp(sb, sa) && strcmp(hb, ha));
    _snprintf_s(rel, sizeof rel, _TRUNCATE, FB_ROW, sb);
    t = slurp(b, rel, NULL);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "mods/%s/probe_base", sb);
    CHECK(t && strstr(t, rel));
    free(t);
    /* A copy whose name sorts first, with the same "built": the folder the
     * record names keeps the salt (DECISIONS 15). */
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\a copy", root);
    CHECK(mt_copy_tree(a, c));
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "a copy");
    CHECK(e && e->refused && strstr(e->cause, "is a copy of mods\\guards"));
    sh_ms_list_free(&list);
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.resalted == 1);
    CHECK(entry_of("guards", sb, NULL, &usable) && usable && !strcmp(sb, sa));
    CHECK(entry_of("a copy", sc, NULL, &usable) && usable && strcmp(sc, sa));
    /* Renamed and copied before a scan, so the record names neither: the
     * folder created first keeps the salt. */
    _snprintf_s(b, sizeof b, _TRUNCATE, "%s\\zz guards", root);
    CHECK(MoveFileA(a, b));
    Sleep(20);
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\aa guards", root);
    CHECK(mt_copy_tree(b, c));
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.resalted == 1);
    CHECK(entry_of("zz guards", sb, NULL, &usable) && usable && !strcmp(sb, sa));
    CHECK(entry_of("aa guards", sc, NULL, &usable) && usable && strcmp(sc, sa));
    /* The record decides before the creation time: the folder at the path
     * that held the salt keeps it even when it was created later. */
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\older elsewhere", root);
    CHECK(MoveFileA(b, c));
    Sleep(20);
    CHECK(mt_copy_tree(c, b));
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.resalted == 1);
    CHECK(entry_of("zz guards", sb, NULL, &usable) && usable && !strcmp(sb, sa));
    CHECK(entry_of("older elsewhere", sc, NULL, &usable) && usable && strcmp(sc, sa));
}

static void test_earlier_shapes(const char *tmp)
{
    static const struct { const char *leaf, *file, *text, *cause; } cases[] = {
        { "format1", "mod.json", "{\"name\":\"Old\",\"snapmap-plus\":{\"format\":1,\"key\":\"alex.old\",\"build\":3}}",
          "Run the Snapmap+ installer again and it converts it." },
        { "pkg", "package.json", "{\"id\":\"alex.pkg\",\"name\":\"Pkg\"}",
          "Run the Snapmap+ installer again and it converts it." },
        { "withid", "mod.json", "{\"id\":\"alex.withid\",\"name\":\"With id\"}", "Run the Snapmap+ installer again and it converts it." },
        { "withkey", "mod.json", "{\"key\":\"x\",\"name\":\"With key\"}", "Run the Snapmap+ installer again and it converts it." },
        { "withscope", "mod.json", "{\"scope\":\"map\",\"name\":\"Scope\"}", "Run the Snapmap+ installer again and it converts it." },
        { "hudschema", "mod.json", "{\"name\":\"Hud\",\"hud\":{\"schema\":1}}", "Run the Snapmap+ installer again and it converts it." },
        { "notjson", "mod.json", "{\"name\": oops", "mod.json is not a valid JSON object" },
        { NULL, NULL, NULL, NULL } };
    char root[MAX_PATH], dir[MAX_PATH], *before[16], *after;
    sh_ms_sync_stats ss;
    sh_ms_list list;
    int i, n = 0;
    printf("-- earlier shapes: refused with their reason; nothing written\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\earlier\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    for (i = 0; cases[i].leaf; i++, n++) {
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, cases[i].leaf);
        mt_write(dir, cases[i].file, cases[i].text);
        mt_write(dir, "assets\\generated\\image\\mods\\x\\a.bimage", "bytes");
        before[i] = slurp(dir, cases[i].file, NULL);
    }
    CHECK(sh_ms_sync_pending() == 0);   /* nothing the scan can change */
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == (unsigned)n && ss.rebuilt == 0 && ss.salted == 0);
    CHECK(sh_ms_enumerate(&list) && list.count == (size_t)n);
    for (i = 0; cases[i].leaf; i++) {
        const sh_ms_entry *e = find_entry(&list, cases[i].leaf);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, cases[i].leaf);
        after = slurp(dir, cases[i].file, NULL);
        CHECK(same_text(before[i], after));
        CHECK(e && e->refused && !sh_ms_entry_usable(e) && strstr(e->cause, cases[i].cause));
        if (e && !strstr(e->cause, cases[i].cause)) fprintf(stderr, "  %s: %s\n", cases[i].leaf, e->cause);
        free(after);
        free(before[i]);
    }
    {
        const sh_ms_entry *e = find_entry(&list, "format1");
        CHECK(e && !strncmp(e->cause, SH_MB_SHAPE, strlen(SH_MB_SHAPE)));
    }
    sh_ms_list_free(&list);
}

/* ------------------------------------------------------------- places */

static void test_place_folders(const char *fixture, const char *tmp)
{
    char root[MAX_PATH], src[MAX_PATH], src2[MAX_PATH], name[SH_MS_NAME_MAX + 1], cause[600] = "", dir[SH_MS_NAME_MAX + 1];
    char title[200], path[MAX_PATH * 2], *t;
    sh_ms_install_stats st;
    sh_ms_sync_stats ss;
    sh_ms_list list;
    const sh_ms_entry *e;
    sh_ms_place arena = { "Hell Arena: Part 1?", "1234567" }, other = { "Hell Arena: Part 1?", "7654321" };
    sh_ms_place zip = { "pack.rawmap.zip", "zip:pack.rawmap.zip" };
    printf("-- places: mods\\maps\\<map>\\<mod>, one folder per salt per place, never built\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\places\\mods", tmp);
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\places\\in\\one", tmp);
    _snprintf_s(src2, sizeof src2, _TRUNCATE, "%s\\places\\in\\two", tmp);
    mt_mkdirs_for(src);
    CHECK(mt_copy_tree(fixture, src));
    CHECK(sh_ms_set_root(root));
    CHECK(sh_ms_place_dir(&arena, dir, sizeof dir) && !strcmp(dir, "maps\\Hell Arena Part 1"));
    CHECK(sh_ms_install_placed(src, &arena, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  placed: %s\n", cause);
    CHECK(!strcmp(name, "maps\\Hell Arena Part 1\\R2 Imp copy") && st.copied == 13 && !st.replaced);
    CHECK(sh_ms_place_of("1234567", dir, sizeof dir, title, sizeof title) && !strcmp(dir, "maps\\Hell Arena Part 1") &&
          !strcmp(title, "Hell Arena: Part 1?"));
    CHECK(sh_ms_place_title("maps\\Hell Arena Part 1\\R2 Imp copy", title, sizeof title) && !strcmp(title, "Hell Arena: Part 1?"));
    CHECK(sh_ms_place_title("maps/Hell Arena Part 1", title, sizeof title));
    CHECK(!sh_ms_place_title("alex.r2-imp", title, sizeof title));
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "maps\\Hell Arena Part 1\\R2 Imp copy");
    CHECK(e && sh_ms_entry_usable(e) && !strcmp(e->place, "maps\\Hell Arena Part 1") && !strcmp(e->salt, "alexrimp"));
    CHECK(sh_ms_find_salt(&list, "alexrimp", "maps\\Hell Arena Part 1", NULL) == e && sh_ms_find_salt(&list, "alexrimp", "*", NULL) == e);
    CHECK(!sh_ms_find_salt(&list, "alexrimp", NULL, NULL));   /* not a root build */
    sh_ms_list_free(&list);
    CHECK(sh_ms_place_folder(&arena, "R2 Imp copy", "alexrimp", dir, sizeof dir) == 2 && !strcmp(dir, "maps\\Hell Arena Part 1\\R2 Imp copy"));
    CHECK(sh_ms_place_folder(&arena, "R2 Imp copy", "otherimp", dir, sizeof dir) == 1 && !strcmp(dir, "maps\\Hell Arena Part 1\\R2 Imp copy (2)"));
    /* F's note: a mod's folder names its place. */
    CHECK(sh_ms_place_note(&arena, "maps/Hell Arena Part 1/R2 Imp copy") && sh_ms_place_of("1234567", dir, sizeof dir, NULL, 0) &&
          !strcmp(dir, "maps\\Hell Arena Part 1"));
    /* Another build of the same salt for the same map replaces the place's folder. */
    mt_append(src, "assets\\generated\\decls\\entitydef\\mods\\alexrimp\\imp_copy.decl", "\r\n// republished\r\n");
    CHECK(sh_ms_install_placed(src, &arena, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause));
    CHECK(!strcmp(name, "maps\\Hell Arena Part 1\\R2 Imp copy") && st.replaced);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\maps\\Hell Arena Part 1", root);
    {
        char names[512];
        dir_names(path, names, sizeof names);
        CHECK(!strcmp(names, "R2 Imp copy;"));
    }
    /* A build whose files are under another salt is refused; nothing placed. */
    CHECK(mt_copy_tree(fixture, src2));
    t = slurp(src2, "mod.json", NULL);
    {
        char *s = t ? strstr(t, "alexrimp") : NULL;
        if (s) memcpy(s, "otherimp", 8);
        if (t) mt_write(src2, "mod.json", t);
    }
    free(t);
    CHECK(!sh_ms_install_placed(src2, &arena, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause) &&
          strstr(cause, "is under mods/alexrimp/, which is neither this mod folder"));
    /* The same title for another map: a place of its own. */
    CHECK(sh_ms_place_dir(&other, dir, sizeof dir) && !strcmp(dir, "maps\\Hell Arena Part 1 (7654321)"));
    CHECK(sh_ms_install_placed(src, &other, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause));
    CHECK(!strcmp(name, "maps\\Hell Arena Part 1 (7654321)\\R2 Imp copy"));
    CHECK(sh_ms_install_placed(src, &zip, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause));
    CHECK(!strcmp(name, "maps\\pack.rawmap.zip\\R2 Imp copy"));
    /* A new folder dropped into a place is refused; place folders are never built. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\maps\\Hell Arena Part 1\\dropped", root);
    fb_make(path, "dropped");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\maps\\Hell Arena Part 1\\R2 Imp copy", root);
    mt_append(path, "assets\\generated\\decls\\entitydef\\mods\\alexrimp\\imp_copy.decl", "\r\n// edited by hand\r\n");
    CHECK(sh_ms_sync_pending() == 0);
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 0 && ss.salted == 0 && ss.folders == 0);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "maps\\Hell Arena Part 1\\dropped");
    CHECK(e && e->refused && strstr(e->cause, "where Snapmap+ builds nothing"));
    sh_ms_list_free(&list);
    {
        char built[SH_MS_NAME_MAX + 1], c2[600], dropped[MAX_PATH * 2];
        sh_ms_install_stats s2;
        _snprintf_s(dropped, sizeof dropped, _TRUNCATE, "%s\\maps\\Hell Arena Part 1\\dropped", root);
        CHECK(sh_ms_assign_salt(dropped, "dropped", NULL, c2, sizeof c2) == -1 && strstr(c2, "where Snapmap+ builds nothing"));
        CHECK(!sh_ms_install_from_folder(path, &RESOLVER, built, sizeof built, &s2, NULL, c2, sizeof c2) &&
              strstr(c2, "came with a downloaded map"));
    }
    /* A hand copy inside one place: the newer one is refused. */
    {
        char copy[MAX_PATH * 2];
        _snprintf_s(copy, sizeof copy, _TRUNCATE, "%s\\maps\\pack.rawmap.zip\\R2 Imp copy (hand)", root);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\maps\\pack.rawmap.zip\\R2 Imp copy", root);
        CHECK(mt_copy_tree(path, copy));
        CHECK(sh_ms_enumerate(&list));
        e = find_entry(&list, "maps\\pack.rawmap.zip\\R2 Imp copy (hand)");
        CHECK(e && e->refused && strstr(e->cause, "Another folder of this map's mods, mods\\maps\\pack.rawmap.zip\\R2 Imp copy, holds the same mod"));
        CHECK(sh_ms_entry_usable(find_entry(&list, "maps\\pack.rawmap.zip\\R2 Imp copy")));
        sh_ms_list_free(&list);
        mt_remove_tree(copy);
    }
    /* Removal: an emptied place goes with its record entry. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\maps\\Hell Arena Part 1 (7654321)\\R2 Imp copy", root);
    CHECK(sh_ms_delete_mod_folder(path, cause, sizeof cause));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\maps\\Hell Arena Part 1 (7654321)", root);
    CHECK(!mt_exists(path) && !sh_ms_place_of("7654321", dir, sizeof dir, NULL, 0));
    CHECK(sh_ms_place_of("1234567", dir, sizeof dir, NULL, 0));
}

/* ------------------------------------------------------------- new mod */

static void test_new_mod(const char *tmp)
{
    char root[MAX_PATH], salt[SH_MB_ID_MAX + 1], folder[MAX_PATH * 2], example[MAX_PATH], cause[600] = "", want[200], *t;
    char s2[SH_MB_ID_MAX + 1];
    sh_ms_sync_stats ss;
    size_t fl;
    int usable;
    printf("-- new mod: a folder named after it, its salt in the block, an example under mods/<salt>/\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\newmod\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    CHECK(sh_ms_new_mod_example("Hell Guards", salt, folder, sizeof folder, example, sizeof example, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  new mod: %s\n", cause);
    fl = strlen(folder);
    CHECK(sh_salt_valid(salt, strlen(salt)) && fl > 11 && !strcmp(folder + fl - 12, "\\Hell Guards"));
    _snprintf_s(want, sizeof want, _TRUNCATE, "assets/generated/decls/snapeditorentitydef/mods/%s/example_crate.decl", salt);
    CHECK(!strcmp(example, want));
    t = slurp(folder, "mod.json", NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s_category\": \"Hell Guards\"", salt);
    CHECK(t && strstr(t, want) && strstr(t, "\"format\": 2") && strstr(t, salt));
    free(t);
    t = slurp(folder, example, NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "categoryNameTag = \"#str_%s_category\"", salt);
    CHECK(t && strstr(t, want));
    free(t);
    CHECK(sh_mb_descriptor_state(folder) == SH_MB_DESC_BUILT);
    CHECK(sh_ms_new_mod("Hell Guards", s2, folder, sizeof folder, cause, sizeof cause) && strstr(folder, "Hell Guards (2)") && strcmp(s2, salt));
    CHECK(sh_ms_new_mod("maps", s2, folder, sizeof folder, cause, sizeof cause) && strstr(folder, "\\maps mod"));
    CHECK(!sh_ms_new_mod("   ", s2, folder, sizeof folder, cause, sizeof cause) && strstr(cause, "needs a name"));
    g_logged = 0;
    /* The example names the game's crate model and prop rows: a resolver
     * that has the whole game. */
    CHECK(sh_ms_sync(&GAME, &ss) && ss.salted == 0);
    if (ss.failed) log_dump("new mod");
    CHECK(entry_of("Hell Guards", s2, NULL, &usable) && !strcmp(s2, salt) && usable);
}

/* ------------------------------------------------------------ the lock */

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
    char name[SH_MS_NAME_MAX + 1];
    j->ok = sh_ms_install_from_folder(j->folder, &RESOLVER, name, sizeof name, &st, NULL, j->cause, sizeof j->cause);
    j->done_at = GetTickCount64();
    return 0;
}

typedef struct hold_job { unsigned waits; volatile LONG held; ULONGLONG released_at; } hold_job;

/* Stands for a long store step: holds the store lock (recursively) until
 * another thread is waiting for it. */
static DWORD WINAPI hold_thread(void *p)
{
    hold_job *h = (hold_job *)p;
    int i;
    modstore_lock();
    modstore_lock();   /* recursive */
    InterlockedExchange(&h->held, 1);
    for (i = 0; i < 60000 && modstore_lock_waits() == h->waits; i++) Sleep(1);
    h->released_at = GetTickCount64();
    modstore_unlock();
    modstore_unlock();
    return 0;
}

/* Stands for another process: takes a named lock through a handle of its
 * own, opened by name (store_lock.h), and lets go once `waits` moved (the store
 * code is waiting for it) or, with no wait count to watch, after `ms`. */
typedef struct named_job {
    wchar_t name[64];
    unsigned waits;
    DWORD ms;
    volatile LONG held;
    ULONGLONG released_at;
} named_job;

static DWORD WINAPI named_holder(void *p)
{
    named_job *j = (named_job *)p;
    HANDLE h = CreateMutexW(NULL, FALSE, j->name);
    int i;
    if (!h || WaitForSingleObject(h, 10000) != WAIT_OBJECT_0) { InterlockedExchange(&j->held, -1); return 0; }
    InterlockedExchange(&j->held, 1);
    if (j->ms) Sleep(j->ms);
    else for (i = 0; i < 60000 && modstore_lock_waits() == j->waits; i++) Sleep(1);
    j->released_at = GetTickCount64();
    ReleaseMutex(h);
    CloseHandle(h);
    return 0;
}

static void test_named_locks(const char *tmp)
{
    char root[MAX_PATH], *text = NULL;
    wchar_t wroot[MAX_PATH], a[64], b[64];
    named_job j;
    HANDLE th;
    ULONGLONG got;
    printf("-- lock: the store and its record share the mods folder's named lock with other processes\n");
    /* One name for one folder however it is spelled; another folder, another name. */
    CHECK(sh_store_lock_name(L"C:\\Users\\A\\AppData\\Local\\snapmap-plus\\mods", a, 64));
    CHECK(sh_store_lock_name(L"\\\\?\\c:/users/a/appdata/local/snapmap-plus/MODS\\", b, 64));
    CHECK(!wcscmp(a, b) && !wcsncmp(a, L"Local\\snapmap-plus-store-", 25) && wcslen(a) == 25 + 16);
    CHECK(sh_store_lock_name(L"C:\\Users\\A\\AppData\\Local\\snapmap-plus\\mods2", b, 64) && wcscmp(a, b) != 0);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\named\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    MultiByteToWideChar(CP_UTF8, 0, root, -1, wroot, MAX_PATH);
    /* The store lock held through another handle: modstore_lock waits for it. */
    memset(&j, 0, sizeof j);
    CHECK(sh_store_lock_name(wroot, j.name, 64));
    j.waits = modstore_lock_waits();
    th = CreateThread(NULL, 0, named_holder, &j, 0, NULL);
    CHECK(th != NULL);
    while (th && !InterlockedCompareExchange(&j.held, 0, 0)) Sleep(1);
    CHECK(j.held == 1);
    modstore_lock();
    got = GetTickCount64();
    modstore_unlock();
    CHECK(modstore_lock_waits() > j.waits && got >= j.released_at);
    if (th) { WaitForSingleObject(th, 60000); CloseHandle(th); }
    /* The record waits for the same lock: a section is read only after the
     * holder let go. */
    memset(&j, 0, sizeof j);
    CHECK(sh_store_lock_name(wroot, j.name, 64));
    j.waits = modstore_lock_waits();
    th = CreateThread(NULL, 0, named_holder, &j, 0, NULL);
    CHECK(th != NULL);
    while (th && !InterlockedCompareExchange(&j.held, 0, 0)) Sleep(1);
    CHECK(j.held == 1);
    (void)sh_mb_record_get(root, "places", &text);
    got = GetTickCount64();
    free(text);
    CHECK(got >= j.released_at && j.released_at != 0);
    if (th) { WaitForSingleObject(th, 60000); CloseHandle(th); }
}

static void test_lock(const char *tmp)
{
    char root[MAX_PATH], cause[600];
    hold_job h = { 0, 0, 0 };
    lock_job a, c;
    HANDLE th, ti[2];
    sh_ms_verify_stats vs;
    unsigned waits = modstore_lock_waits();
    h.waits = waits;
    printf("-- lock: a second installer waits for the holder; two at once both succeed\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\lock\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    memset(&a, 0, sizeof a);
    memset(&c, 0, sizeof c);
    _snprintf_s(a.folder, sizeof a.folder, _TRUNCATE, "%s\\lock\\in\\lockone", tmp);
    _snprintf_s(c.folder, sizeof c.folder, _TRUNCATE, "%s\\lock\\in\\locktwo", tmp);
    make_small_mod(a.folder, "lockone");
    make_small_mod(c.folder, "locktwo");
    th = CreateThread(NULL, 0, hold_thread, &h, 0, NULL);
    CHECK(th != NULL);
    while (th && !InterlockedCompareExchange(&h.held, 0, 0)) Sleep(1);
    ti[0] = CreateThread(NULL, 0, install_thread, &a, 0, NULL);
    CHECK(ti[0] != NULL);
    if (ti[0]) WaitForSingleObject(ti[0], 60000);
    if (th) WaitForSingleObject(th, 60000);
    CHECK(a.ok);
    if (!a.ok) fprintf(stderr, "  waiting install: %s\n", a.cause);
    CHECK(a.done_at >= h.released_at);
    if (ti[0]) CloseHandle(ti[0]);
    if (th) CloseHandle(th);
    _snprintf_s(a.folder, sizeof a.folder, _TRUNCATE, "%s\\lock\\in\\lockthree", tmp);
    make_small_mod(a.folder, "lockthree");
    a.ok = 0;
    ti[0] = CreateThread(NULL, 0, install_thread, &a, 0, NULL);
    ti[1] = CreateThread(NULL, 0, install_thread, &c, 0, NULL);
    CHECK(ti[0] && ti[1]);
    if (ti[0] && ti[1]) WaitForMultipleObjects(2, ti, TRUE, 60000);
    CHECK(a.ok && c.ok);
    if (!a.ok || !c.ok) fprintf(stderr, "  parallel installs: %s / %s\n", a.cause, c.cause);
    if (ti[0]) CloseHandle(ti[0]);
    if (ti[1]) CloseHandle(ti[1]);
    CHECK(sh_ms_verify("lockone", &vs, cause, sizeof cause) && sh_ms_verify("locktwo", &vs, cause, sizeof cause));
    modstore_lock();
    CHECK(sh_ms_verify("lockthree", &vs, cause, sizeof cause));
    modstore_unlock();
    /* The store itself is not a mod, nor a folder inside a mod. */
    {
        char name[SH_MS_NAME_MAX + 1], path[MAX_PATH * 2];
        sh_ms_install_stats st;
        CHECK(!sh_ms_install_from_folder(root, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\lockone\\assets", root);
        CHECK(!sh_ms_install_from_folder(path, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause));
    }
}

/* ---------------------------------------------------------- the boot scan */

static volatile LONG g_boot_rescans;
static int boot_rescan(void) { InterlockedIncrement(&g_boot_rescans); return 1; }

static void test_boot_sync(const char *fixture, const char *tmp)
{
    char root[MAX_PATH], a[MAX_PATH * 2], c[MAX_PATH * 2];
    const char *mj;
    int i;
    sh_ms_list list;
    const sh_ms_entry *e;
    printf("-- boot: folders not built yet are salted and built as a scan does, then indexed\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\boot\\mods", tmp);
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\boot", tmp);
    CreateDirectoryA(a, NULL);
    CreateDirectoryA(root, NULL);
    CHECK(sh_ms_set_root(root));
    CHECK(sh_ms_sync_pending() == 0);
    g_boot_rescans = 0;
    CHECK(sh_ms_boot_sync(&RESOLVER, boot_rescan, 0) == 0 && g_boot_rescans == 0);
    /* A salted folder never built, and a new folder in an organizing folder. */
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\alex.r2-imp", root);
    CHECK(mt_copy_tree(fixture, a));
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\campaign\\alexboot", root);
    mt_write(c, "mod.json", "{\"version\":\"0.1.0\",\"name\":\"Boot\"}");
    mt_write(c, "assets\\generated\\decls\\entitydef\\mods\\alexboot\\thing.decl", "{\r\n\tinherit = \"mods/alexrimp/imp_copy\";\r\n}");
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp");
    CHECK(e && !e->loaded && strstr(e->cause, "not built"));
    sh_ms_list_free(&list);
    CHECK(sh_ms_sync_pending() == 2);
    g_logged = 0;
    CHECK(sh_ms_boot_sync(&RESOLVER, boot_rescan, 0) == 2 && g_boot_rescans == 1);
    CHECK(log_has("MODS: store: boot: 2 folder(s) not built yet") && log_has("MODS: store: boot: 2 of 2 folder(s) built (0 refused)"));
    if (!log_has("2 of 2 folder(s) built")) log_dump("boot");
    mj = slurp(a, "mod.json", NULL);
    CHECK(mj && strstr(mj, "\"build\""));
    CHECK(sh_ms_enumerate(&list));
    CHECK(sh_ms_entry_usable(find_entry(&list, "alex.r2-imp")) && sh_ms_entry_usable(find_entry(&list, "campaign\\alexboot")));
    sh_ms_list_free(&list);
    CHECK(sh_ms_sync_pending() == 0 && sh_ms_boot_sync(&RESOLVER, boot_rescan, 0) == 0 && g_boot_rescans == 1);
    /* An edit made while the game was closed: built in the background, then indexed. */
    {
        char salt[SH_SALT_LEN + 1], rel[MAX_PATH];
        entry_of("campaign\\alexboot", salt, NULL, NULL);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\%s\\thing.decl", salt);
        mt_write(c, rel, "{\r\n\tinherit = \"mods/alexrimp/imp_copy\";\r\n\t// edited\r\n}");
    }
    CHECK(sh_ms_sync_pending() == 1);
    CHECK(sh_ms_boot_sync(&RESOLVER, boot_rescan, 1) == 1);
    sh_ms_boot_release();
    for (i = 0; i < 400 && InterlockedCompareExchange(&g_boot_rescans, 0, 0) < 2; i++) Sleep(25);
    CHECK(g_boot_rescans == 2);
    for (i = 0; i < 400 && sh_ms_sync_pending() != 0; i++) Sleep(25);
    CHECK(sh_ms_sync_pending() == 0);
}

static HANDLE g_between_seen;
static volatile LONG g_between_calls;
static void boot_between(unsigned built)
{
    unsigned waits = modstore_lock_waits();
    int i;
    (void)built;
    if (InterlockedIncrement(&g_between_calls) != 1) return;
    SetEvent(g_between_seen);
    for (i = 0; i < 60000 && modstore_lock_waits() == waits; i++) Sleep(1);
}

static void test_boot_release(const char *tmp)
{
    char a[MAX_PATH * 2], c[MAX_PATH * 2], salt[SH_SALT_LEN + 1], rel[MAX_PATH];
    unsigned waits;
    LONG rescans, at_lock;
    int i, built_at_lock;
    printf("-- boot: the scan waits for the startup install, then yields the store between folders\n");
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\boot\\mods\\alex.r2-imp", tmp);
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\boot\\mods\\campaign\\alexboot", tmp);
    mt_append(a, "assets\\generated\\decls\\entitydef\\mods\\alexrimp\\imp_copy.decl", "\r\n// edited at boot\r\n");
    entry_of("campaign\\alexboot", salt, NULL, NULL);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\%s\\thing.decl", salt);
    mt_write(c, rel, "{\r\n\tinherit = \"mods/alexrimp/imp_copy\";\r\n\t// edited twice\r\n}");
    CHECK(sh_ms_sync_pending() == 2);
    sh_ms_test_boot_rearm();
    rescans = InterlockedCompareExchange(&g_boot_rescans, 0, 0);
    CHECK(sh_ms_boot_sync(&RESOLVER, boot_rescan, 1) == 1);
    waits = modstore_lock_waits();
    for (i = 0; i < 20; i++) { modstore_lock(); Sleep(10); modstore_unlock(); }
    CHECK(modstore_lock_waits() == waits);
    CHECK(sh_ms_sync_pending() == 2 && g_boot_rescans == rescans);   /* nothing built before the release */
    CHECK(!sh_ms_boot_released());
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
    /* The waiting thread got the store between the two folders: one built,
     * not yet indexed; the build then resumed and finished. */
    CHECK(at_lock == rescans && built_at_lock == 1);

    for (i = 0; i < 800 && InterlockedCompareExchange(&g_boot_rescans, 0, 0) == rescans; i++) Sleep(25);
    CHECK(g_boot_rescans == rescans + 1);
    for (i = 0; i < 400 && !log_has("MODS: store: boot: 2 of 2 folder(s) built"); i++) Sleep(25);
    CHECK(log_has("MODS: store: boot: the startup install released the store after"));
    CHECK(log_has("MODS: store: boot: 2 of 2 folder(s) built (0 refused)"));
    CHECK(sh_ms_sync_pending() == 0);
    sh_ms_test_between_folders = NULL;
    CloseHandle(g_between_seen);
}

/* ----------------------------------------------- folders and removal */

static long long bin_items(const char *path)
{
    SHQUERYRBINFO q;
    char drive[4] = { path[0], ':', '\\', 0 };
    memset(&q, 0, sizeof q);
    q.cbSize = sizeof q;
    return SHQueryRecycleBinA(drive, &q) == S_OK ? q.i64NumItems : -1;
}

static void test_recycle(const char *tmp)
{
    char root[MAX_PATH], outside[MAX_PATH], precious[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH * 3], cause[300];
    long long before, after;
    int r, ci = getenv("CI") != NULL;
    printf("-- removal: to the Recycle Bin, links never followed; outright when the shell refuses\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\recycle\\mods", tmp);
    _snprintf_s(outside, sizeof outside, _TRUNCATE, "%s\\recycle\\outside", tmp);
    _snprintf_s(precious, sizeof precious, _TRUNCATE, "%s\\precious.txt", outside);
    CHECK(sh_ms_set_root(root));
    mt_write(outside, "precious.txt", "not the store's");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\binned", root);
    make_small_mod(dir, "binned");
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
        CHECK(log_has("MODS: store: moved mods\\binned to the Recycle Bin"));
        CHECK(before < 0 || after > before);
    }
    CHECK(!sh_ms_delete_mod_folder(outside, cause, sizeof cause) && strstr(cause, "is not a mod folder of the store"));
    CHECK(!sh_ms_remove_folder(outside) && mt_exists(precious));
    CHECK(!sh_ms_remove_folder(root) && mt_exists(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\refused", root);
    make_small_mod(dir, "refused");
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE, "cmd /c mklink /J \"%s\\link\" \"%s\" >nul 2>&1", dir, outside);
    (void)system(cmd);
    sh_ms_recycle_refuse_for_test(1);
    g_logged = 0;
    CHECK(sh_ms_delete_mod_folder(dir, cause, sizeof cause) == SH_MS_DELETED && strstr(cause, "the shell refused it (test)"));
    CHECK(!mt_exists(dir) && mt_exists(precious));
    CHECK(log_has("MODS: store: deleted mods\\refused outright; the Recycle Bin could not take it (the shell refused it (test))"));
    sh_ms_recycle_refuse_for_test(0);
    CHECK(!sh_ms_delete_mod_folder(dir, cause, sizeof cause) && strstr(cause, "is no longer in the store"));
}

static void test_work_folder_names(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH];
    sh_ms_list list;
    printf("-- work folders: only a .staging-<pid> or .old-<pid> suffix is skipped\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\worknames\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\doom.old-school", root);
    make_small_mod(dir, "doom.old-school");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\my.staging-area", root);
    make_small_mod(dir, "my.staging-area");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\top.old-4242", root);
    make_small_mod(dir, "top");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\top.staging-17", root);
    make_small_mod(dir, "top");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\.hidden", root);
    make_small_mod(dir, "hidden");
    CHECK(sh_ms_enumerate(&list) && list.count == 2);
    CHECK(find_entry(&list, "doom.old-school") && find_entry(&list, "my.staging-area"));
    sh_ms_list_free(&list);
}

static void test_crash_leftovers(const char *tmp)
{
    static const unsigned long DEAD = 4294967280ul;
    char root[MAX_PATH], dir[MAX_PATH];
    sh_ms_list list;
    printf("-- crash leftovers: a folder taken away mid-swap is put back, stale work folders go\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\leftovers\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gone.old-%lu", root, DEAD);
    make_small_mod(dir, "gone");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gone.staging-%lu", root, DEAD);
    make_small_mod(dir, "gone");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\maps\\Arena\\nested.old-%lu", root, DEAD);
    make_small_mod(dir, "nested");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\here", root);
    make_small_mod(dir, "here");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\here.old-%lu", root, DEAD);
    make_small_mod(dir, "here");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\live.staging-%lu", root, (unsigned long)GetCurrentProcessId());
    make_small_mod(dir, "live");
    CHECK(sh_ms_enumerate(&list) && list.count == 1);
    sh_ms_list_free(&list);
    g_logged = 0;
    CHECK(sh_ms_recover_work() == 2);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gone", root);
    CHECK(mt_has(dir, "mod.json"));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\maps\\Arena\\nested", root);
    CHECK(mt_has(dir, "mod.json"));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\here.old-%lu", root, DEAD);
    CHECK(!mt_exists(dir));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\live.staging-%lu", root, (unsigned long)GetCurrentProcessId());
    CHECK(mt_exists(dir));
    CHECK(log_has("MODS: store: put gone back from gone.old-4294967280: an earlier session ended while it was being replaced"));
    CHECK(log_has("MODS: store: put maps\\Arena\\nested back from maps\\Arena\\nested.old-4294967280"));
    CHECK(log_has("MODS: store: removed here.old-4294967280, a work folder an earlier session left"));
    CHECK(log_has("MODS: store: removed gone.staging-4294967280, a work folder"));
    CHECK(log_count("MODS: store: ") == 4);
    CHECK(sh_ms_enumerate(&list) && list.count == 3);
    sh_ms_list_free(&list);
    g_logged = 0;
    CHECK(sh_ms_recover_work() == 0 && log_count("MODS: store: ") == 0);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\live.staging-%lu", root, (unsigned long)GetCurrentProcessId());
    mt_remove_tree(dir);
}

/* The scripts' compile check, standing in for script_compile.c. */
static int script_ok(const char *file, int entry, const char *text, size_t len, int *line, char *what, size_t what_cap,
                     char *todo, size_t todo_cap)
{
    (void)file; (void)entry; (void)text; (void)len; (void)what; (void)what_cap; (void)todo; (void)todo_cap;
    if (line) *line = 0;
    return 1;
}

/* A Wwise bank header (version 113) carrying `id`, padded to 32 bytes. */
static void write_bank(const char *folder, const char *rel, unsigned long id)
{
    unsigned char bytes[32];
    char path[MAX_PATH * 2];
    FILE *f;
    memset(bytes, 0, sizeof bytes);
    memcpy(bytes, "BKHD", 4);
    mt_put32(bytes + 4, 24);
    mt_put32(bytes + 8, 0x71);
    mt_put32(bytes + 12, id);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(bytes, 1, sizeof bytes, f); fclose(f); }
}

static unsigned long bank_id(const char *folder, const char *rel)
{
    size_t len = 0;
    char *t = slurp(folder, rel, &len);
    unsigned long id = 0;
    if (t && len >= 16) memcpy(&id, t + 12, 4);
    free(t);
    return id;
}

/* DECISIONS 20 and 16: what an author writes under the folder's name moves
 * under the salt at every scan, not only the first; a rename between scans
 * changes nothing already moved; a bank named for the folder takes the
 * salt's name and id; a README beside mod.json is ignored. */
static void test_rehome(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH * 2], renamed[MAX_PATH * 2], salt[SH_SALT_LEN + 1], s2[SH_SALT_LEN + 1];
    char rel[MAX_PATH], want[200], *t;
    sh_ms_sync_stats ss;
    int usable;
    printf("-- scan: new content under the folder's name moves under the salt at every scan\n");
    sh_mb_set_script_check(script_ok);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\rehome\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\imp", root);
    make_small_mod(dir, "imp");
    mt_write(dir, "README.txt", "notes about mods/imp/ that stay as written");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.salted == 1 && ss.failed == 0);
    CHECK(entry_of("imp", salt, NULL, &usable) && usable && salt[0]);
    t = slurp(dir, "README.txt", NULL);
    CHECK(t && !strcmp(t, "notes about mods/imp/ that stay as written"));
    free(t);
    /* A second file, a second string and a script, all under the folder's name. */
    mt_write(dir, "assets\\generated\\image\\mods\\imp\\b.bimage", "second image of imp");
    mt_write(dir, "assets\\mods\\imp\\scripts\\door.lua",
             "api = 1\r\nlocal image = \"mods/imp/b\"\r\nlocal title = \"#str_imp_title\"\r\n");
    t = slurp(dir, "mod.json", NULL);
    if (t) {
        size_t n = strlen(t) + 128;
        char *edited = (char *)malloc(n);
        if (edited) {
            _snprintf_s(edited, n, _TRUNCATE, "{\r\n  \"strings\": {\"en\": {\"imp_title\": \"Imp\"}},%s", t + 1);
            mt_write(dir, "mod.json", edited);
            free(edited);
        }
        free(t);
    }
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0 && ss.rebuilt == 1);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\b.bimage", salt);
    CHECK(mt_has(dir, rel) && !mt_has(dir, "assets\\generated\\image\\mods\\imp\\b.bimage"));
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\mods\\%s\\scripts\\door.lua", salt);
    t = slurp(dir, rel, NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"mods/%s/b\"", salt);
    CHECK(t && strstr(t, want));
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"#str_%s_imp_title\"", salt);
    CHECK(t && strstr(t, want));
    free(t);
    t = slurp(dir, "mod.json", NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s_imp_title\": \"Imp\"", salt);
    CHECK(t && strstr(t, want) && strstr(t, "\"build\""));
    free(t);
    CHECK(entry_of("imp", s2, NULL, &usable) && usable && !strcmp(s2, salt));
    /* Renamed: what was moved stays; new content under the new name moves,
     * and a bank named for the folder takes the salt's name and id. */
    _snprintf_s(renamed, sizeof renamed, _TRUNCATE, "%s\\beast", root);
    CHECK(MoveFileA(dir, renamed));
    mt_write(renamed, "assets\\generated\\image\\mods\\beast\\c.bimage", "third image");
    write_bank(renamed, "assets\\sound\\soundbanks\\pc\\beast_music.bnk", 12345);
    mt_write(renamed, "assets\\generated\\decls\\sound\\mods\\beast\\m.decl",
             "{\n\tbank = \"beast_music\";\n\tother = \"beast_musical\";\n}\n");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 0 && ss.salted == 0);
    if (ss.failed) log_dump("rehome");
    /* The decl naming the bank follows its new name; a longer word does not. */
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\sound\\mods\\%s\\m.decl", salt);
    t = slurp(renamed, rel, NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "bank = \"%s_music\";", salt);
    CHECK(t && strstr(t, want) && strstr(t, "\"beast_musical\"") && !strstr(t, "\"beast_music\""));
    free(t);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\c.bimage", salt);
    CHECK(mt_has(renamed, rel));
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\b.bimage", salt);
    CHECK(mt_has(renamed, rel));
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\sound\\soundbanks\\pc\\%s_music.bnk", salt);
    CHECK(mt_has(renamed, rel) && !mt_has(renamed, "assets\\sound\\soundbanks\\pc\\beast_music.bnk"));
    {
        char stem[32];
        _snprintf_s(stem, sizeof stem, _TRUNCATE, "%s_music", salt);
        CHECK(bank_id(renamed, rel) == (unsigned long)sh_mb_fnv1_lower(stem, strlen(stem)));
    }
    CHECK(entry_of("beast", s2, NULL, &usable) && usable && !strcmp(s2, salt));
    /* A current folder is not moved again: nothing to do, nothing written. */
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.rebuilt == 0 && ss.failed == 0);
    /* A new folder named like another mod's salt: that name is the other
     * mod's, so nothing is moved for it and it says so. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, salt);
    make_small_mod(dir, salt);
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.failed == 1);
    {
        sh_ms_list list;
        const sh_ms_entry *e;
        CHECK(sh_ms_enumerate(&list));
        e = find_entry(&list, salt);
        CHECK(e && !sh_ms_entry_usable(e) && strstr(e->cause, "is another mod's salt") && strstr(e->cause, "rename this folder"));
        if (e) printf("   %s\n", e->cause);
        sh_ms_list_free(&list);
    }
    sh_mb_set_script_check(NULL);
}

/* Folder names an earlier layout used are ordinary names now. */
static void test_ordinary_names(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH * 2], salt[SH_SALT_LEN + 1];
    sh_ms_sync_stats ss;
    int usable;
    printf("-- names: folders called blobs or legacy are mods like any other\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\ordinary\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\legacy", root);
    make_small_mod(dir, "legacy");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\sorted\\blobs", root);
    make_small_mod(dir, "blobs");
    CHECK(sh_ms_sync(&RESOLVER, &ss) && ss.salted == 2 && ss.failed == 0);
    CHECK(entry_of("legacy", salt, NULL, &usable) && usable && salt[0]);
    CHECK(entry_of("sorted\\blobs", salt, NULL, &usable) && usable && salt[0]);
}

static void test_nested(const char *tmp)
{
    char root[MAX_PATH], dir[MAX_PATH], cause[300], name[SH_MS_NAME_MAX + 1];
    sh_ms_list list;
    sh_ms_install_stats st;
    sh_ms_verify_stats vs;
    const sh_ms_entry *e;
    int r, ci = getenv("CI") != NULL;
    printf("-- nested: organizing folders are looked into, a mod is not; mods\\maps is the places\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\nested\\mods", tmp);
    CHECK(sh_ms_set_root(root));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\top", root);
    make_small_mod(dir, "top");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\cyber", root);
    make_small_mod(dir, "cyber");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\bosses\\spider", root);
    make_small_mod(dir, "spider");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\top\\inner", root);
    make_small_mod(dir, "inner");
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\maps\\notaplace", root);   /* a mod.json right in a place folder */
    make_small_mod(dir, "notaplace");
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
    CHECK(sh_ms_enumerate(&list) && list.count == 3);
    e = find_entry(&list, "demon-mods\\cyber");
    CHECK(e != NULL && find_entry(&list, "demon-mods\\bosses\\spider") && !find_entry(&list, "top\\inner"));
    CHECK(e && sh_ms_entry_matches(e, "demon-mods\\cyber") && sh_ms_entry_matches(e, "demon-mods/cyber") &&
          sh_ms_entry_matches(e, "cyber") && !sh_ms_entry_matches(e, "top"));
    sh_ms_list_free(&list);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\cyber", root);
    cause[0] = 0;
    CHECK(sh_ms_install_from_folder(dir, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  nested build: %s\n", cause);
    CHECK(st.in_place && !strcmp(name, "demon-mods\\cyber"));
    CHECK(sh_ms_verify("cyber", &vs, cause, sizeof cause) && vs.entries == 1);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "demon-mods\\cyber");
    CHECK(e && e->loaded && sh_salt_valid(e->salt, strlen(e->salt)) && sh_ms_entry_matches(e, e->salt));
    sh_ms_list_free(&list);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods", root);
    CHECK(!sh_ms_install_from_folder(dir, &RESOLVER, name, sizeof name, &st, NULL, cause, sizeof cause) &&
          strstr(cause, "has no mod.json"));
    CHECK(!sh_ms_delete_mod_folder(dir, cause, sizeof cause) && strstr(cause, "is not a mod folder of the store") && mt_exists(dir));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\bosses\\spider", root);
    g_logged = 0;
    r = sh_ms_delete_mod_folder(dir, cause, sizeof cause);
    CHECK(ci ? r != 0 : r == SH_MS_RECYCLED);
    CHECK(!mt_exists(dir));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\demon-mods\\bosses", root);
    CHECK(mt_exists(dir));
    if (r == SH_MS_RECYCLED) CHECK(log_has("MODS: store: moved mods\\demon-mods\\bosses\\spider to the Recycle Bin"));
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], fixture[MAX_PATH], got[MAX_PATH];
    if (!GetFullPathNameA(argc > 1 ? argv[1] : "fixtures\\mods\\alex.r2-imp", sizeof fixture, fixture, NULL)) return 2;
    if (!mt_temp_dir("modstore", tmp, sizeof tmp)) { fprintf(stderr, "no temp dir\n"); return 2; }
    CHECK(sh_ms_set_root(NULL) && sh_ms_root(got, sizeof got) && strstr(got, "\\snapmap-plus\\mods"));
    test_install(fixture, tmp);
    test_fresh_folder_salted(tmp);
    test_fresh_folder_strings(tmp);
    test_fresh_folder_refusals(tmp);
    test_copy_gets_new_salt(tmp);
    test_converter_salt(tmp);
    test_earlier_shapes(tmp);
    test_place_folders(fixture, tmp);
    test_new_mod(tmp);
    test_lock(tmp);
    test_named_locks(tmp);
    test_recycle(tmp);
    test_work_folder_names(tmp);
    test_crash_leftovers(tmp);
    test_nested(tmp);
    test_ordinary_names(tmp);
    test_rehome(tmp);
    test_boot_sync(fixture, tmp);
    test_boot_release(tmp);
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    if (failures) { fprintf(stderr, "modstore_test: %d failure(s)\n", failures); return 1; }
    printf("modstore_test: install, salt, copies, earlier shapes, places, new mod, lock, removal, leftovers, nested, "
           "the store tool's half and the boot scan pass\n");
    return 0;
}
