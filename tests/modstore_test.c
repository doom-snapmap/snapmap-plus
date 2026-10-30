/* modstore_test.c -- the mods\ store: install, hard links and the copy
 * fallback, enumeration, verification and detected corruption, in-place
 * builds, build-numbered folders, and resolve_need hit, miss, dedupe and
 * conflict, and the store lock (a second installer waits, two installers at
 * once both succeed).
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
void backend_log(const char *message) { (void)message; g_logged++; }

static const char *const STOCK[] = {
    "generated/decls/material/textures/guis/snapmaps/entity_icons/demons/imp_enc.decl",
    "generated/decls/entitydef/ai/demon/imp_coop.decl",
    "generated/decls/renderprog/skinvmtr.decl",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/rage_taunt.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/meleeforward01.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/meleeforward02.bmd6anim",
    "cooked/anim/md6/characters/monsters/imp/base/motion/combat/idle.bmd6anim",
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

static unsigned count_blobs(const char *root)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    unsigned n = 0, ro = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\blobs\\*", root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        n++;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ro++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    CHECK(ro == n);   /* every blob is read-only */
    return n;
}

static const sh_ms_entry *find_entry(const sh_ms_list *l, const char *key)
{
    size_t i;
    for (i = 0; i < l->count; i++) if (!strcmp(l->items[i].key, key)) return &l->items[i];
    return NULL;
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

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], root[MAX_PATH], src[MAX_PATH], path[MAX_PATH * 2], key[160], cause[512] = "", fixture[MAX_PATH];
    char hand[MAX_PATH];
    sh_ms_install_stats st;
    sh_ms_verify_stats vs;
    sh_ms_list list;
    sh_ms_hit hit;
    sh_mb_bundle b;
    const sh_ms_entry *e;
    unsigned blobs_after_first;

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

    /* An empty or absent store lists nothing. */
    CHECK(sh_ms_enumerate(&list) && list.count == 0);
    sh_ms_list_free(&list);

    /* Install from outside the store: hard links. */
    cause[0] = 0;
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, &b, cause, sizeof cause));
    if (b.errors || cause[0]) fprintf(stderr, "  install: %s\n", b.errors ? b.first_cause : cause);
    CHECK(!strcmp(key, "alex.r2-imp@1.0.0+1"));
    CHECK(st.blobs_written == 13 && st.blobs_reused == 0 && st.linked == 13 && st.copied == 0 && !st.in_place && !st.replaced);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.json", src);
    CHECK(mt_exists(path));   /* built in place */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp@1.0.0+1\\bundle.json", root);
    CHECK(mt_exists(path));   /* the store folder name carries the build */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.r2-imp@1.0.0+1\\.state.json", root);
    CHECK(mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp@1.0.0+1\\assets\\generated\\decls\\md6def\\mods\\alex.r2-imp\\imp_copy.md6.decl", root);
    CHECK(links_of(path) >= 2);
    CHECK(GetFileAttributesA(path) & FILE_ATTRIBUTE_READONLY);
    blobs_after_first = count_blobs(root);
    CHECK(blobs_after_first == 13);
    sh_mb_free(&b);

    /* Enumeration trusts bundle.json and checks sizes. */
    CHECK(sh_ms_enumerate(&list) && list.count == 1);
    e = find_entry(&list, "alex.r2-imp@1.0.0+1");
    CHECK(e && e->loaded && e->sizes_ok && e->has_state && e->validated && sh_ms_entry_usable(e) && e->installed_at > 0);
    CHECK(e && e->bundle.file_count == 13 && e->bundle.build == 1);

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
    CHECK(sh_ms_verify("alex.r2-imp@1.0.0+1", &vs, cause, sizeof cause));
    CHECK(vs.entries == 1 && vs.files == 13 && vs.blobs_checked == 0);   /* hard links are the blobs */
    CHECK(!sh_ms_verify("alex.r2-imp@9.9.9+1", &vs, cause, sizeof cause) && strstr(cause, "no installed bundle"));

    /* Reinstalling unchanged content keeps build 1 and replaces the folder. */
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    CHECK(!strcmp(key, "alex.r2-imp@1.0.0+1") && st.replaced && st.blobs_reused == 13 && st.blobs_written == 0);
    CHECK(count_blobs(root) == 13);   /* blobs survive, still read-only, after the old tree is removed */
    CHECK(sh_ms_verify("alex.r2-imp@1.0.0+1", &vs, cause, sizeof cause));

    /* Changed content takes build 2, laid out by the copy fallback. */
    mt_append(src, "assets\\generated\\decls\\entitydef\\mods\\alex.r2-imp\\imp_copy.decl", "\r\n// edited\r\n");
    sh_ms_test_force_copy(1);
    CHECK(sh_ms_install_from_folder(src, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    sh_ms_test_force_copy(0);
    CHECK(!strcmp(key, "alex.r2-imp@1.0.0+2"));
    CHECK(st.blobs_written == 1 && st.blobs_reused == 12 && st.copied == 13 && st.linked == 0);
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp@1.0.0+2\\assets\\generated\\decls\\md6def\\mods\\alex.r2-imp\\imp_copy.md6.decl", root);
    CHECK(links_of(path) == 1);
    CHECK(count_blobs(root) == 14);
    CHECK(sh_ms_verify("alex.r2-imp@1.0.0+2", &vs, cause, sizeof cause));
    CHECK(vs.files == 13 && vs.blobs_checked == 13);   /* copies: each blob hashed too */

    /* Two builds installed: a decl both provide is a conflict; a raw file
     * with equal bytes in both is a dedupe. */
    CHECK(sh_ms_enumerate(&list) && list.count == 2);
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/alex.r2-imp/imp_copy.md6", &hit) == SH_MS_CONFLICT && hit.providers == 2);
    CHECK(sh_ms_resolve_need(&list, "baseModel", "md6/mods/alex.r2-imp/imp_copy/assets/mesh/imp_copy.md6mesh", &hit) == SH_MS_DEDUPE);
    CHECK(!strcmp(list.items[hit.entry].key, "alex.r2-imp@1.0.0+1"));   /* first match wins */
    sh_ms_list_free(&list);
    /* Verifying id@version covers every build. */
    CHECK(sh_ms_verify("alex.r2-imp@1.0.0", &vs, cause, sizeof cause) && vs.entries == 2);

    /* Corruption in the copied tree is detected and disables that build. */
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp@1.0.0+2\\assets\\generated\\basemodel\\md6\\mods\\alex.r2_imp\\imp_copy\\assets\\mesh\\imp_copy.bmd6model",
                root);
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    {
        FILE *f = NULL;
        if (fopen_s(&f, path, "r+b") == 0 && f) { fputc('X', f); fclose(f); }
    }
    CHECK(!sh_ms_verify("alex.r2-imp@1.0.0+2", &vs, cause, sizeof cause));
    CHECK(strstr(cause, "imp_copy.bmd6model does not match bundle.json") != NULL);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp@1.0.0+2");
    CHECK(e && e->loaded && e->sizes_ok && e->has_state && !e->validated && !sh_ms_entry_usable(e));
    CHECK(e && strstr(e->cause, "does not match"));
    CHECK(sh_ms_resolve_need(&list, "md6Def", "mods/alex.r2-imp/imp_copy.md6", &hit) == SH_MS_HIT);
    sh_ms_list_free(&list);
    /* A missing file shows at enumeration without hashing. */
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s\\alex.r2-imp@1.0.0+2\\assets\\generated\\image\\mods\\alex.r2_imp\\imp_icon.bimage", root);
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(path);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.r2-imp@1.0.0+2");
    CHECK(e && !e->sizes_ok && strstr(e->cause, "missing"));
    sh_ms_list_free(&list);

    /* A folder dropped into mods\ by hand is built in place. */
    _snprintf_s(hand, sizeof hand, _TRUNCATE, "%s\\dropped", root);
    CHECK(mt_copy_tree(fixture, hand));
    mt_write(hand, "mod.json", "{\"id\":\"alex.dropped\",\"name\":\"Dropped\"}");
    /* Its own namespace: a minimal decl. */
    {
        char rm[MAX_PATH * 2];
        _snprintf_s(rm, sizeof rm, _TRUNCATE, "%s\\assets", hand);
        mt_remove_tree(rm);
    }
    mt_write(hand, "assets\\generated\\decls\\entitydef\\mods\\alex.dropped\\thing.decl",
             "{\r\n\tinherit = \"mods/alex.r2-imp/imp_copy\";\r\n}");
    /* Unbuilt, it is listed as such. */
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "dropped");
    CHECK(e && !e->loaded && strstr(e->cause, "not built"));
    sh_ms_list_free(&list);
    cause[0] = 0;
    CHECK(sh_ms_install_from_folder(hand, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  dropped: %s\n", cause);
    CHECK(!strcmp(key, "alex.dropped@0.0.0+1") && st.in_place && st.linked == 0 && st.copied == 0 && st.blobs_written == 1);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.json", hand);
    CHECK(mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.dropped@0.0.0+1", root);
    CHECK(!mt_exists(path));   /* no second copy of an in-place bundle */
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.dropped@0.0.0+1");
    CHECK(e && sh_ms_entry_usable(e));
    sh_ms_list_free(&list);
    CHECK(sh_ms_verify("alex.dropped@0.0.0+1", &vs, cause, sizeof cause) && vs.blobs_checked == 1);

    /* A refused build leaves the state on an in-store folder and installs nothing. */
    mt_write(hand, "assets\\generated\\decls\\entitydef\\other\\x.decl", "{\r\n}");
    CHECK(!sh_ms_install_from_folder(hand, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    CHECK(strstr(cause, "outside mods/alex.dropped/") != NULL);
    CHECK(sh_ms_enumerate(&list));
    e = find_entry(&list, "alex.dropped@0.0.0+1");
    CHECK(e && e->has_state && !e->validated && !sh_ms_entry_usable(e));
    sh_ms_list_free(&list);

    /* A corrupted blob shared with a hard-linked tree is caught too. */
    {
        char blob[MAX_PATH * 2];
        CHECK(sh_ms_enumerate(&list));
        e = find_entry(&list, "alex.r2-imp@1.0.0+1");
        CHECK(e != NULL);
        if (e) {
            _snprintf_s(blob, sizeof blob, _TRUNCATE, "%s\\blobs\\%s", root, e->bundle.files[0].blob);
            SetFileAttributesA(blob, FILE_ATTRIBUTE_NORMAL);
            {
                FILE *f = NULL;
                if (fopen_s(&f, blob, "r+b") == 0 && f) { fputc('Y', f); fclose(f); }
            }
            CHECK(!sh_ms_verify("alex.r2-imp@1.0.0+1", &vs, cause, sizeof cause));
        }
        sh_ms_list_free(&list);
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
        CHECK(sh_ms_verify("test.lockone@1.0.0", &vs, cause, sizeof cause));
        CHECK(sh_ms_verify("test.locktwo@1.0.0", &vs, cause, sizeof cause));
        /* The lock is recursive on one thread: an install inside a held step. */
        modstore_lock();
        CHECK(sh_ms_verify("test.locktwo@1.0.0", &vs, cause, sizeof cause));
        modstore_unlock();
    }

    /* The store itself is not a mod. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\blobs", root);
    CHECK(!sh_ms_install_from_folder(path, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));
    CHECK(!sh_ms_install_from_folder(root, &RESOLVER, key, sizeof key, &st, NULL, cause, sizeof cause));

    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    if (failures) { fprintf(stderr, "modstore_test: %d failure(s)\n", failures); return 1; }
    printf("modstore_test: install, hard link and copy, verify, corruption, in-place, build folders and resolve_need pass\n");
    return 0;
}
