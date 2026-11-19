/* modcache_test.c -- the map content cache (modcache.h) and the publishing key (modkey.h).
 *
 * Units composed from the cache into what the serving index serves; one copy of each file on disk however many maps
 * and namespaces use it (hard links); compositions deterministic across machines; a local mod serving a unit by exact
 * hash only, checked again after it changes; stand-ins and the notice for an absent object, then the full composition
 * once it arrives; the sweep (kept maps, maps only played, the open map); the notice sentences; format 2 rows; the
 * key's signature, claim secrets and DPAPI file.
 *
 *   modcache_test.exe
 */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modauthor.h"
#include "modbundle.h"
#include "modunit.h"
#include "modmanifest.h"
#include "modserve.h"
#include "modcache.h"
#include "modkey.h"
#include "saved_maps.h"
#include "mod_test_util.h"

#define SALT  "pk7salt1"
#define SALT2 "qw3erty1"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

void backend_log(const char *message) { (void)message; }

/* ------------------------------------------------------------ serving-layer doubles modmanifest.c links */
static sh_modmanifest_list g_current_rows;
int sh_modserve_active(void) { return 1; }
int modserve_activate_bundles(const char *const *bundle_hashes, int count) { (void)bundle_hashes; (void)count; return 0; }
void sh_modserve_bundle_ref_free(sh_modserve_bundle_ref *ref) { (void)ref; }
int sh_modserve_row_build(const sh_modmanifest_row *row, sh_modserve_bundle_ref *out) { (void)row; if (out) memset(out, 0, sizeof *out); return 0; }
size_t modserve_bundles_for_names_visit(const char *const *names, size_t count, sh_modserve_closure_fn visit, void *context)
{ (void)names; (void)count; (void)visit; (void)context; return 0; }
int sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute)
{ (void)slots; (void)keys; (void)n; (void)recompute; return 0; }

static int game_exists(void *ctx, const char *request) { (void)ctx; return strstr(request, "mods/") ? 0 : 1; }
static const sh_mb_resolver g_game = { NULL, NULL, game_exists, NULL };
static int game_where(void *ctx, const char *request, char sha[SH_MB_HASH_HEX + 1], char *source, size_t cap)
{
    (void)ctx;
    if (strstr(request, "mods/")) return 0;
    memset(sha, 'a', SH_MB_HASH_HEX);
    sha[SH_MB_HASH_HEX] = 0;
    strcpy_s(source, cap, "snap_gameresources");
    return 1;
}
static int script_ok(const char *file, int entry, const char *text, size_t len, int *line, char **what, char **todo)
{ (void)file; (void)entry; (void)text; (void)len; (void)line; (void)what; (void)todo; return 1; }

/* ------------------------------------------------------------ fixtures (modunit_test.c's pack) */

static void put_binary(const char *folder, const char *rel, const char *name, size_t pad, unsigned seed)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    size_t i;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") || !f) return;
    for (i = 0; i < pad; i++) fputc((int)((i * 7 + seed) % 251), f);
    if (name) { fputc(0, f); fwrite(name, 1, strlen(name), f); fputc(0, f); }
    for (i = 0; i < 64; i++) fputc(0, f);
    fclose(f);
}

static void add_object(const char *folder, const char *n, size_t pad, unsigned seed)
{
    char rel[MAX_PATH], text[1024], spell[128];
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "entities\\%s.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t\trenderModelInfo = {\n"
                "\t\t\tmodel = \"mods/" SALT "/models/%s.lwo\";\n\t\t}\n\t}\n}\n", n);
    mt_write(folder, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "meshes\\models\\%s.bmodel", n);
    _snprintf_s(spell, sizeof spell, _TRUNCATE, "mods/" SALT "/%s_skin", n);
    put_binary(folder, rel, spell, pad, seed);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "materials\\%s_skin.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\ndiffusemap\t\"mods/" SALT "/%s_d.tga\"\n}\n", n);
    mt_write(folder, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "images\\%s_d.bimage", n);
    put_binary(folder, rel, NULL, pad, seed + 2);
    /* the shared gore kit: one file both objects' materials name */
    mt_write(folder, "images\\shared_gore.bimage", "gore gore gore gore gore");
}

static void write_pack(const char *folder)
{
    mt_write(folder, "mod.json",
             "{\n  \"name\": \"Crates and barrels\",\n"
             "  \"strings\": { \"en\": { \"crate_name\": \"Crate\", \"barrel_name\": \"Barrel\" } }\n}\n");
    add_object(folder, "crate", 200, 1);
    add_object(folder, "barrel", 3000, 5);
}

static int build(const char *folder, sh_mb_bundle *b)
{
    memset(b, 0, sizeof *b);
    if (!sh_mb_build(folder, SALT, &g_game, b) || b->errors) {
        fprintf(stderr, "build of %s: %s\n", folder, sh_mb_first_cause(b) ? sh_mb_first_cause(b) : "(no cause)");
        failures++;
        return 0;
    }
    return 1;
}

static int unit(const sh_mb_bundle *b, const char *folder, const char *name, sh_unit *u)
{
    sh_unit_env env;
    char *why = NULL;
    int ok;
    memset(&env, 0, sizeof env);
    env.game = game_where;
    ok = sh_unit_of(b, folder, name, &env, u, &why);
    if (!ok) { fprintf(stderr, "unit %s: %s\n", name, why ? why : "?"); failures++; }
    free(why);
    return ok;
}

/* What a download brings: the unit's manifest and every file (from the author's folder) into the cache. */
static void download(const sh_unit *u, const sh_mb_bundle *b, const char *folder, int files)
{
    size_t i;
    CHECK(sh_cache_put_manifest(u->hash, u->manifest, u->manifest_len));
    for (i = 0; files && i < b->file_count; i++) {
        char from[MAX_PATH * 2], tmp[MAX_PATH * 2], *p;
        if (!u->keep[i] || b->files[i].stock || b->files[i].absent) continue;
        _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\%s", folder, b->files[i].path);
        for (p = from; *p; p++) if (*p == '/') *p = '\\';
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.dl", from);
        CHECK(CopyFileA(from, tmp, FALSE));
        CHECK(sh_cache_put_blob(tmp, b->files[i].blob, b->files[i].size));
    }
}

static void row_of(sh_modmanifest_row *r, const char *salt, const sh_unit *const *units, size_t n)
{
    const char *roots[8], *hashes[8];
    size_t i;
    memset(r, 0, sizeof *r);
    strcpy_s(r->salt, sizeof r->salt, salt);
    for (i = 0; i < n; i++) { roots[i] = units[i]->root; hashes[i] = units[i]->hash; }
    CHECK(sh_modmanifest_row_pin(r, roots, hashes, n));
    CHECK(sh_modmanifest_row_set_name(r, "Crates and barrels"));
}

static void load_rows(sh_modmanifest_row *rows, size_t n, const char *const *placed, size_t nplaced)
{
    sh_modmanifest_list list;
    size_t i;
    memset(&list, 0, sizeof list);
    for (i = 0; i < n; i++) CHECK(sh_modmanifest_list_add(&list, &rows[i]));
    sh_cache_on_load(&list, (char *const *)placed, nplaced);
    sh_modmanifest_list_free(&list);
}

static size_t count_files(const char *dir)
{
    char pattern[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    while (h != INVALID_HANDLE_VALUE) {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
            char sub[MAX_PATH * 2];
            _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += count_files(sub);
            else n++;
        }
        if (!FindNextFileA(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    return n;
}

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

static char *read_text(const char *path)
{
    size_t len = 0;
    return (char *)sh_mb_read_file(path, 1u << 24, &len);
}

/* ------------------------------------------------------------ tests */

/* A local save slot as the game writes it: <saves>\<profile>\SNAPMAPS<id>\map.decl (a 4-byte checksum, then zlib of
 * the map's JSON, written here as stored DEFLATE blocks) and game.details naming its cloud item. `text` NULL writes
 * a map.decl that does not decode. */
static void write_slot(const char *saves, const char *id, const char *item, const char *text, size_t len)
{
    char dir[MAX_PATH * 2], path[MAX_PATH * 2];
    unsigned char *z;
    size_t at = 0, off = 0;
    unsigned long a = 1, bsum = 0;
    FILE *f = NULL;
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\76561198000000001", saves);
    CreateDirectoryA(saves, NULL);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\76561198000000001\\SNAPMAPS%s", saves, id);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\game.details", dir);
    if (!fopen_s(&f, path, "wb") && f) { fprintf(f, "checksum=1\nitemId=%s\nmetadata={\"name\":\"t\"}\nmodified=0\n", item); fclose(f); }
    z = (unsigned char *)malloc(4 + 2 + len + (len / 65535 + 1) * 5 + 4 + 16);
    if (!z) return;
    memset(z, 0, 4);
    at = 4;
    z[at++] = 0x78;
    z[at++] = text ? 0x01 : 0x02;   /* a header the reader refuses: the slot does not decode */
    do {
        size_t n = len - off > 65535 ? 65535 : len - off;
        z[at++] = (unsigned char)(off + n == len ? 1 : 0);
        z[at++] = (unsigned char)(n & 0xff); z[at++] = (unsigned char)(n >> 8);
        z[at++] = (unsigned char)(~n & 0xff); z[at++] = (unsigned char)((~n >> 8) & 0xff);
        if (text) memcpy(z + at, text + off, n);
        at += n;
        off += n;
    } while (off < len);
    for (off = 0; text && off < len; off++) { a = (a + (unsigned char)text[off]) % 65521; bsum = (bsum + a) % 65521; }
    a |= bsum << 16;
    z[at++] = (unsigned char)(a >> 24); z[at++] = (unsigned char)(a >> 16); z[at++] = (unsigned char)(a >> 8); z[at++] = (unsigned char)a;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\map.decl", dir);
    if (!fopen_s(&f, path, "wb") && f) { fwrite(z, 1, at, f); fclose(f); }
    free(z);
}

static void test_pointer_sources(void)
{
    printf("-- a unit's pointers into the product's container are not checked against DOOM's files\n");
    CHECK(sh_cache_pointer_checked("snap_gameresources") == 1);
    CHECK(sh_cache_pointer_checked("gameresources") == 1);
    CHECK(sh_cache_pointer_checked("snapmapplus") == 0);   /* what the walker writes (modprog.h) */
    CHECK(sh_cache_pointer_checked("product") == 0);
}

static void test_net_retry(void)
{
    printf("-- a unit file stopped by the network is tried again, four times, the pause doubling\n");
    CHECK(sh_cache_net_retry_ms(MODNET_E_NET, 0) == 250);   /* winhttp 12030: the connection reset (R-LV4) */
    CHECK(sh_cache_net_retry_ms(MODNET_E_NET, 3) == 2000);
    CHECK(sh_cache_net_retry_ms(MODNET_E_NET, 4) == -1);    /* the tries are spent */
    CHECK(sh_cache_net_retry_ms(MODNET_E_TIMEOUT, 1) == 500);
    CHECK(sh_cache_net_retry_ms(MODNET_E_RATE, 0) == 250);
    CHECK(sh_cache_net_retry_ms(MODNET_E_UNAVAILABLE, 2) == 1000);
    CHECK(sh_cache_net_retry_ms(MODNET_E_NOT_FOUND, 0) == -1);   /* an answer, not a failure */
    CHECK(sh_cache_net_retry_ms(MODNET_E_BLOCKED, 0) == -1);
    CHECK(sh_cache_net_retry_ms(MODNET_E_CANCELLED, 0) == -1);   /* the player said no */
    CHECK(sh_cache_net_retry_ms(MODNET_OK, 0) == -1);
}

static void test_composition_hash(void)
{
    const char *a[] = { "1111111111111111111111111111111111111111111111111111111111111111",
                        "2222222222222222222222222222222222222222222222222222222222222222" };
    const char *b[] = { a[1], a[0], a[1] };
    char x[65], y[65], z[65];
    CHECK(sh_modmanifest_composition_hash(SALT, a, 2, x));
    CHECK(sh_modmanifest_composition_hash(SALT, b, 3, y));
    CHECK(sh_modmanifest_composition_hash(SALT2, a, 2, z));
    CHECK(!strcmp(x, y));           /* order and repeats do not matter */
    CHECK(strcmp(x, z) != 0);       /* the namespace does */
}

static void test_rows_format2(void)
{
    const char *roots[] = { "entities/crate", "*" };
    const char *units[] = { "1111111111111111111111111111111111111111111111111111111111111111",
                            "2222222222222222222222222222222222222222222222222222222222222222" };
    sh_modmanifest_row r, back, local;
    char *v, err[128];
    size_t len = 0;
    memset(&r, 0, sizeof r);
    strcpy_s(r.salt, sizeof r.salt, SALT);
    CHECK(sh_modmanifest_row_pin(&r, roots, units, 2));
    CHECK(sh_modmanifest_row_set_name(&r, "Eternal Demons"));
    r.size = 42;
    v = sh_modmanifest_format_value(&r, &len);
    CHECK(v && !strcmp(v, "{\"mod\":\"" SALT "\",\"name\":\"Eternal Demons\",\"size\":42,\"objects\":{\"entities/crate\":\""
                          "1111111111111111111111111111111111111111111111111111111111111111\"},\"whole\":\""
                          "2222222222222222222222222222222222222222222222222222222222222222\"}"));
    CHECK(v && sh_modmanifest_parse_value(v, len, &back, err, sizeof err));
    CHECK(back.pinned && back.unit_count == 2 && !strcmp(back.build, r.build) && back.attached);
    free(v);
    sh_modmanifest_row_free(&back);
    /* A local row names no unit. */
    memset(&local, 0, sizeof local);
    strcpy_s(local.salt, sizeof local.salt, SALT);
    strcpy_s(local.build, sizeof local.build, "3333333333333333333333333333333333333333333333333333333333333333");
    CHECK(sh_modmanifest_row_add_name(&local, "mods/" SALT "/crate"));
    v = sh_modmanifest_format_value(&local, &len);
    CHECK(v && !strcmp(v, "{\"mod\":\"" SALT "\",\"name\":\"\",\"size\":0,\"local\":true,\"build\":\"3333333333333333333333333333333333333333333333333333333333333333\",\"names\":[\"mods/" SALT "/crate\"]}"));
    CHECK(v && sh_modmanifest_parse_value(v, len, &back, err, sizeof err) && !back.pinned &&
          !strcmp(back.build, "3333333333333333333333333333333333333333333333333333333333333333"));
    free(v);
    sh_modmanifest_row_free(&back);
    /* A format 1 row is a local row at its build (D7). */
    {
        const char *old = "{\"mod\":\"" SALT "@3333333333333333333333333333333333333333333333333333333333333333\",\"name\":\"x\",\"size\":1,\"names\":[]}";
        CHECK(sh_modmanifest_parse_value(old, strlen(old), &back, err, sizeof err) && !back.pinned &&
              !strcmp(back.build, "3333333333333333333333333333333333333333333333333333333333333333"));
        sh_modmanifest_row_free(&back);
    }
    sh_modmanifest_row_free(&local);
    sh_modmanifest_row_free(&r);
}

static void test_notices(void)
{
    sh_cache_absent one = { "Tyrant", "Eternal Demons", SH_CACHE_ABSENT_REMOVED };
    sh_cache_absent many[] = { { "Tyrant", "Eternal Demons", SH_CACHE_ABSENT_REMOVED },
                               { "Marauder", "Eternal Demons", SH_CACHE_ABSENT_UNREACHABLE },
                               { "Hell Guard", "Hell Guards", SH_CACHE_ABSENT_NOT_IN_STORE } };
    sh_cache_absent five[] = { { "A", "M", SH_CACHE_ABSENT_OFFLINE }, { "B", "M", SH_CACHE_ABSENT_OFFLINE },
                               { "C", "M", SH_CACHE_ABSENT_OFFLINE }, { "D", "M", SH_CACHE_ABSENT_OFFLINE },
                               { "E", "M", SH_CACHE_ABSENT_OFFLINE } };
    char *t;
    int c;
    static const char *const singles[] = {
        "",
        "This map is playing without the Tyrant from Eternal Demons, which is not on this computer. Connect to the internet and open the map again to get it.",
        "This map is playing without the Tyrant from Eternal Demons: the mod store could not be reached. Open the map again later to get it.",
        "This map is playing without the Tyrant from Eternal Demons, which needs a newer Snapmap+; run the Snapmap+ installer to update.",
        "This map is playing without the Tyrant from Eternal Demons, which needs a different version of DOOM's own files.",
        "This map is playing without the Tyrant from Eternal Demons, which is not in the mod store yet.",
        "This map is playing without the Tyrant from Eternal Demons, which was removed from the mod store.",
        "This map is playing without the Tyrant from Eternal Demons, because there is not enough free disk space.",
    };
    for (c = SH_CACHE_ABSENT_OFFLINE; c <= SH_CACHE_ABSENT_DISK; c++) {
        one.cause = c;
        t = sh_cache_notice_text(&one, 1);
        CHECK(t && !strcmp(t, singles[c]));
        if (t && strcmp(t, singles[c])) fprintf(stderr, "  got: %s\n", t);
        free(t);
    }
    /* Several causes: the most actionable cause's sentence names every object (SPEC 8, S4's example). */
    t = sh_cache_notice_text(many, 3);
    CHECK(t && !strcmp(t, "This map is playing without 3 objects from 2 mods (the Tyrant, the Marauder and the Hell Guard): the mod "
                          "store could not be reached. Open it again later to get them."));
    if (t) printf("  %s\n", t);
    free(t);
    t = sh_cache_notice_text(five, 5);
    CHECK(t && !strcmp(t, "This map is playing without 5 objects from M (the A, the B, the C and 2 more), which are not on this "
                          "computer. Connect to the internet and open the map again to get them."));
    free(t);
    t = sh_cache_object_name("entities/ai/demon/hell_guard");
    CHECK(t && !strcmp(t, "Hell Guard"));
    free(t);
}

/* The module decls a pinning step reads: the hall only. */
static const char *g_module_decl;
static char *read_module(void *context, const char *request, size_t *len)
{
    char *t;
    (void)context;
    if (!g_module_decl || !strstr(request, "/rooms/hall.decl")) return NULL;
    *len = strlen(g_module_decl);
    if ((t = (char *)malloc(*len + 1)) != NULL) memcpy(t, g_module_decl, *len + 1);
    return t;
}

static void test_cache(const char *top)
{
    char pack[MAX_PATH * 2], mods[MAX_PATH * 2], mods2[MAX_PATH * 2], path[MAX_PATH * 3], chash[65], chash2[65], *t;
    sh_mb_bundle b, back1, back2;
    sh_unit crate, barrel;
    const sh_unit *both[2];
    sh_modmanifest_row rows[2];
    const char *placed[] = { "mods/" SALT "/crate", "mods/" SALT "/barrel" };
    int complete = 0;
    size_t blobs;

    _snprintf_s(pack, sizeof pack, _TRUNCATE, "%s\\author\\crates", top);
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\player\\mods", top);
    _snprintf_s(mods2, sizeof mods2, _TRUNCATE, "%s\\player2\\mods", top);
    write_pack(pack);
    if (!build(pack, &b)) return;
    if (!unit(&b, pack, "mods/" SALT "/crate", &crate) || !unit(&b, pack, "mods/" SALT "/barrel", &barrel)) { sh_mb_free(&b); return; }
    both[0] = &crate;
    both[1] = &barrel;

    /* A second player: the units downloaded into a clean cache, then composed (no local mod of the namespace). */
    CreateDirectoryA(mods, NULL);
    sh_cache_set_mods_root(mods);
    download(&crate, &b, pack, 1);
    download(&barrel, &b, pack, 1);
    CHECK(sh_cache_unit_state(crate.hash, NULL, NULL, NULL) == 1);
    row_of(&rows[0], SALT, both, 2);
    strcpy_s(chash, sizeof chash, rows[0].build);
    load_rows(rows, 1, placed, 2);
    CHECK(sh_cache_composed(chash, &complete) && complete);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\mod.json", mods, chash);
    CHECK(mt_exists(path));
    /* Every file of the composition is a link to its blob: no second copy on disk. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\images\\crate_d.bimage", mods, chash);
    CHECK(links_of(path) >= 2);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\files", mods);
    blobs = count_files(path);
    CHECK(blobs > 0);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s", mods, chash);
    memset(&back1, 0, sizeof back1);
    CHECK(sh_mb_load_folder(path, &back1) && back1.validated && back1.units_of_count == 2);

    /* Ten maps and two compositions (the crate alone, both objects): one copy of each file however many rows use it. */
    {
        int k;
        row_of(&rows[1], SALT, both, 1);
        strcpy_s(chash2, sizeof chash2, rows[1].build);
        CHECK(strcmp(chash, chash2) != 0);
        for (k = 0; k < 10; k++) load_rows(rows, 2, placed, 2);
        CHECK(sh_cache_composed(chash2, &complete) && complete);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\files", mods);
        CHECK(count_files(path) == blobs);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\images\\crate_d.bimage", mods, chash2);
        CHECK(links_of(path) >= 3);   /* the blob and both compositions */
    }

    /* Deterministic: another machine composes the same bytes from the same units (D22a). */
    CreateDirectoryA(mods2, NULL);
    sh_cache_set_mods_root(mods2);
    download(&crate, &b, pack, 1);
    download(&barrel, &b, pack, 1);
    load_rows(rows, 1, placed, 2);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s", mods2, chash);
    memset(&back2, 0, sizeof back2);
    CHECK(sh_mb_load_folder(path, &back2) && !strcmp(back1.hash, back2.hash));
    sh_mb_free(&back1);
    sh_mb_free(&back2);

    /* An absent object: a stand-in and one notice; the map's own row is kept. Then it arrives: composed in full. */
    {
        char mods3[MAX_PATH * 2];
        _snprintf_s(mods3, sizeof mods3, _TRUNCATE, "%s\\player3\\mods", top);
        CreateDirectoryA(mods3, NULL);
        sh_cache_set_mods_root(mods3);
        download(&crate, &b, pack, 1);
        (void)sh_cache_take_notice();
        load_rows(rows, 1, placed, 2);
        CHECK(sh_cache_composed(chash, &complete) && !complete);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\entities\\barrel.decl", mods3, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "snapmaps/unknown"));
        free(t);
        t = sh_cache_take_notice();
        CHECK(t && !strcmp(t, "This map is playing without the Barrel from Crates and barrels, which is not on this computer. "
                              "Connect to the internet and open the map again to get it."));
        if (t) printf("  %s\n", t);
        free(t);
        CHECK(!sh_cache_take_notice());   /* once */
        /* R-LV4: the load after a fetch says the fetch's answer, not "not on this computer": the store could not be
         * reached (a closed port on this computer), the composition records it, and the load that asks nothing
         * repeats it. */
        {
            modnet_io io;
            memset(&io, 0, sizeof io);
            CHECK(modnet_set_base_url("http://127.0.0.1:9/"));
            CHECK(sh_cache_prepare(chash, &io) == 0);
            CHECK(modnet_set_base_url(NULL));
            t = sh_cache_take_notice();
            CHECK(t && strstr(t, "the mod store could not be reached"));
            free(t);
            load_rows(rows, 1, placed, 2);
            t = sh_cache_take_notice();
            CHECK(t && strstr(t, "the mod store could not be reached") && !strstr(t, "not on this computer"));
            free(t);
        }
        download(&barrel, &b, pack, 1);
        load_rows(rows, 1, placed, 2);
        CHECK(sh_cache_composed(chash, &complete) && complete);
        t = read_text(path);
        CHECK(t && !strstr(t, "snapmaps/unknown") && strstr(t, "renderModelInfo"));
        free(t);
        CHECK(!sh_cache_take_notice());
    }

    /* R-DS5: the stand-ins keep what the map saved of each placed entity. Its definition keeps the entity's class, and
     * a palette row names it with the map's reference slot counts, inheriting DOOM's own row of that class and those
     * counts (or the class's row with the counts written over it), so the map validator finds the entity in the
     * palette and its slots match: the map loads. */
    {
        char mods5[MAX_PATH * 2];
        static const char map[] =
            "{\"entities\":["
            "{\"entityDef\":{\"className\":\"idInteractable\",\"inherit\":\"mods/" SALT "/barrel\"},\"uniqueId\":0},"
            "{\"entityDef\":{\"className\":\"idPlaceableSnapAIEncounter\",\"inherit\":\"mods/" SALT "/demons/lamp\"},\"uniqueId\":1},"
            "{\"entityDef\":{\"className\":\"idPlaceableSnapAIEncounter\",\"inherit\":\"mods/" SALT "/demons/lamp2\"},\"uniqueId\":2},"
            "{\"entityDef\":{\"className\":\"idEntity\",\"inherit\":\"mods/" SALT "/plain\"},\"uniqueId\":3},"
            "{\"entityDef\":{\"className\":\"bad\\\"class\",\"inherit\":\"mods/" SALT "/odd\"},\"uniqueId\":4}],"
            "\"references\":{\"entityEntRefs\":{\"keyValues\":[0,1,1,3,3,3],\"values\":[5,0,0]},"
            "\"entityVarRefs\":{\"keyValues\":[0,4,4,4,4,4],\"values\":[1,2,3,4]}}}";
        sh_modmanifest_list list;
        _snprintf_s(mods5, sizeof mods5, _TRUNCATE, "%s\\player5\\mods", top);
        CreateDirectoryA(mods5, NULL);
        sh_cache_set_mods_root(mods5);
        download(&crate, &b, pack, 1);
        (void)sh_cache_take_notice();
        memset(&list, 0, sizeof list);
        CHECK(sh_modmanifest_list_add(&list, &rows[0]));
        sh_modmanifest_set_content(sh_cache_on_load);
        sh_modmanifest_content_for(map, sizeof map - 1, &list);
        sh_modmanifest_set_content(NULL);
        sh_modmanifest_list_free(&list);
        CHECK(sh_cache_composed(chash, &complete) && !complete);
        CHECK(!sh_modmanifest_content_text(NULL));   /* only inside the step */

        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\entities\\barrel.decl", mods5, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "inherit = \"snapmaps/unknown\";") && strstr(t, "class = \"idInteractable\";"));
        free(t);
        _snprintf_s(path, sizeof path, _TRUNCATE,
                    "%s\\.content\\served\\%s\\decls\\snapeditorentitydef\\snapmapplus_standin\\barrel.decl", mods5, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "inherit = \"interactives/tripod_panel\";") && strstr(t, "entityDef = \"entities/barrel\";") &&
              strstr(t, "placeable = false;") && strstr(t, "isMoveable = true;") && !strstr(t, "entRefs"));
        free(t);

        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\entities\\demons\\lamp.decl", mods5, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "class = \"idPlaceableSnapAIEncounter\";"));
        free(t);
        _snprintf_s(path, sizeof path, _TRUNCATE,
                    "%s\\.content\\served\\%s\\decls\\snapeditorentitydef\\snapmapplus_standin\\demons\\lamp.decl", mods5, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "inherit = \"demons/demon_base\";") && strstr(t, "entityDef = \"entities/demons/lamp\";") &&
              !strstr(t, "entRefs"));
        free(t);
        /* Counts DOOM gives no row of the class: the class's row, with the map's counts written over it. */
        _snprintf_s(path, sizeof path, _TRUNCATE,
                    "%s\\.content\\served\\%s\\decls\\snapeditorentitydef\\snapmapplus_standin\\demons\\lamp2.decl", mods5, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "inherit = \"demons/demon_base\";") && strstr(t, "entRefs = {\n\t\t\tnum = 2;") &&
              strstr(t, "varRefs = {\n\t\t\tnum = 0;"));
        free(t);
        /* idEntity: the bare stock stand-in; a class the decl text cannot carry: the same. */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\entities\\plain.decl", mods5, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "snapmaps/unknown") && !strstr(t, "class ="));
        free(t);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\entities\\odd.decl", mods5, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "snapmaps/unknown") && !strstr(t, "class ="));
        free(t);
        /* The present crate is served from its unit, not stood in for. */
        _snprintf_s(path, sizeof path, _TRUNCATE,
                    "%s\\.content\\served\\%s\\decls\\snapeditorentitydef\\snapmapplus_standin\\crate.decl", mods5, chash);
        CHECK(!mt_exists(path));
        t = sh_cache_take_notice();
        CHECK(t != NULL);
        free(t);
        /* The composition built and validated with the rows: the builder admits them as the bundle's editor rows. */
        {
            sh_mb_bundle sb;
            size_t r;
            int rows_found = 0;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s", mods5, chash);
            memset(&sb, 0, sizeof sb);
            CHECK(sh_mb_load_folder(path, &sb) && sb.validated);
            for (r = 0; r < sb.file_count; r++)
                if (sb.files[r].ptype && !_stricmp(sb.files[r].ptype, "snapEditorEntityDef") && sb.files[r].pname &&
                    !_strnicmp(sb.files[r].pname, "mods/" SALT "/snapmapplus_standin/", strlen("mods/" SALT "/snapmapplus_standin/")))
                    rows_found++;
            CHECK(rows_found == 5);
            sh_mb_free(&sb);
        }
    }

    /* D-A30: a placed mod module whose unit cannot be had (its height under the grid room's least, 432, is raised to it). The row records its bounds and portals (read from its decl
     * by sh_cache_module_record when the row is pinned); the composition serves the product's blank room of that
     * size at the module's name with those bounds and portals (the map's door slots keep their count), and a module
     * info for the module palette. The record survives the row's write and read, so a save keeps it. */
    {
        char mods6[MAX_PATH * 2], *rec, *arr, *room = NULL;
        static const char decl[] =
            "{\n\tedit = {\n\t\tmodule = {\n\t\t\tname = \"modules/hall.map\";\n"
            "\t\t\tbounds = {\n\t\t\t\tb = {\n\t\t\t\t\tb[0] = {\n\t\t\t\t\t\tx = -512;\n\t\t\t\t\t\ty = -256;\n\t\t\t\t\t}\n"
            "\t\t\t\t\tb[1] = {\n\t\t\t\t\t\tx = 512;\n\t\t\t\t\t\ty = 768;\n\t\t\t\t\t\tz = 384;\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n"
            "\t\t\tportals = {\n\t\t\t\tnum = 2;\n"
            "\t\t\t\titem[0] = {\n\t\t\t\t\tbounds = {\n\t\t\t\t\t\tb = {\n\t\t\t\t\t\t\tb[0] = {\n\t\t\t\t\t\t\t\tx = -128;\n"
            "\t\t\t\t\t\t\t\ty = 768;\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t\tb[1] = {\n\t\t\t\t\t\t\t\tx = 128;\n\t\t\t\t\t\t\t\ty = 768;\n"
            "\t\t\t\t\t\t\t\tz = 256;\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t}\n\t\t\t\t\t}\n\t\t\t\t\tdir = \"PORTAL_DIR_POS_X\";\n\t\t\t\t}\n"
            "\t\t\t\titem[1] = {\n\t\t\t\t\tbounds = {\n\t\t\t\t\t\tb = {\n\t\t\t\t\t\t\tb[0] = {\n\t\t\t\t\t\t\t\tx = 512;\n"
            "\t\t\t\t\t\t\t\ty = 0;\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t\tb[1] = {\n\t\t\t\t\t\t\t\tx = 512;\n\t\t\t\t\t\t\t\ty = 256;\n"
            "\t\t\t\t\t\t\t\tz = 256;\n\t\t\t\t\t\t\t}\n\t\t\t\t\t\t}\n\t\t\t\t\t}\n\t\t\t\t\tdir = \"PORTAL_DIR_POS_Y\";\n\t\t\t\t}\n"
            "\t\t\t}\n\t\t}\n\t}\n}\n";
        rec = sh_cache_module_record("maps/modules/mods/" SALT "/rooms/hall.decl", decl, sizeof decl - 1);
        CHECK(rec && !strcmp(rec, "{\"name\":\"maps/modules/mods/" SALT "/rooms/hall.decl\",\"bounds\":[-512,-256,0,512,768,384],"
                                  "\"portals\":[{\"bounds\":[-128,768,0,128,768,256],\"dir\":\"PORTAL_DIR_POS_X\"},"
                                  "{\"bounds\":[512,0,0,512,256,256],\"dir\":\"PORTAL_DIR_POS_Y\"}]}"));
        if (rec) printf("  module record: %s\n", rec);
        CHECK(!sh_cache_module_record("x", "{\n\tedit = {\n\t}\n}\n", 18));   /* not a module */
        t = rec ? sh_cache_module_standin_text(rec, strlen(rec), &room) : NULL;
        CHECK(t && room && !strcmp(room, "maps/modules/smpgrid/v1/m/1024_1024_432.decl") &&
              strstr(t, "inherit = \"maps/modules/smpgrid/v1/m/1024_1024_432.decl\";") && strstr(t, "num = 2;") &&
              strstr(t, "dir = \"PORTAL_DIR_POS_Y\";") && strstr(t, "x = -128;"));
        if (room && strcmp(room, "maps/modules/smpgrid/v1/m/1024_1024_432.decl")) printf("  room: %s\n", room);
        free(t);
        free(room);

        /* The pinning step records each module of the namespace the map places, once; a stock module is not one. */
        {
            static const char map[] =
                "{\"instances\":[{\"moduleName\":\"maps/modules/mods/" SALT "/rooms/hall.decl\"},"
                "{\"moduleName\":\"maps/modules/ind_dlc/ind_totally_blank_room_4x.decl\"},"
                "{\"moduleName\":\"maps/modules/mods/" SALT "/rooms/hall.decl\"},"
                "{\"moduleName\":\"maps/modules/mods/" SALT "/rooms/gone.decl\"}]}";
            g_module_decl = decl;
            CHECK(sh_cache_row_record_modules(&rows[0], map, sizeof map - 1, read_module, NULL) == 1);
            arr = rows[0].modules;
            CHECK(arr && rec && arr[0] == '[' && !strncmp(arr + 1, rec, strlen(rec)) && !strcmp(arr + 1 + strlen(rec), "]"));
        }
        /* The row carries the records whole through a write and a read. */
        {
            size_t vl = 0;
            char *v = sh_modmanifest_format_value(&rows[0], &vl);
            CHECK(v && strstr(v, ",\"modules\":[{\"name\":\"maps/modules/mods/" SALT "/rooms/hall.decl\""));
            {
                sh_modmanifest_row back;
                sh_modmanifest_list copy;
                char err[200];
                memset(&back, 0, sizeof back);
                memset(&copy, 0, sizeof copy);
                CHECK(v && sh_modmanifest_parse_value(v, vl, &back, err, sizeof err) && back.modules &&
                      !strcmp(back.modules, arr));
                CHECK(sh_modmanifest_list_add(&copy, &back) && copy.rows[0].modules && !strcmp(copy.rows[0].modules, arr));
                sh_modmanifest_list_free(&copy);
                sh_modmanifest_row_free(&back);
            }
            free(v);
        }

        _snprintf_s(mods6, sizeof mods6, _TRUNCATE, "%s\\player6\\mods", top);
        CreateDirectoryA(mods6, NULL);
        sh_cache_set_mods_root(mods6);
        download(&crate, &b, pack, 1);
        (void)sh_cache_take_notice();
        load_rows(rows, 1, placed, 2);
        CHECK(sh_cache_composed(chash, &complete) && !complete);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\map-modules\\rooms\\hall.decl", mods6, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "smpgrid/v1/m/1024_1024_432.decl") && strstr(t, "PORTAL_DIR_POS_X"));
        free(t);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\decls\\snapmoduleinfo\\rooms\\hall.decl", mods6, chash);
        t = read_text(path);
        CHECK(t && strstr(t, "inherit = \"ind_dlc/ind_totally_blank_room_4x\";"));
        free(t);
        {
            sh_mb_bundle mb;
            size_t r;
            int module_found = 0;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s", mods6, chash);
            memset(&mb, 0, sizeof mb);
            CHECK(sh_mb_load_folder(path, &mb) && mb.validated);
            for (r = 0; r < mb.file_count; r++)
                if (mb.files[r].request && !_stricmp(mb.files[r].request, "maps/modules/mods/" SALT "/rooms/hall.decl")) module_found = 1;
            CHECK(module_found);
            sh_mb_free(&mb);
        }
        t = sh_cache_take_notice();
        CHECK(t != NULL);
        free(t);
        rows[0].modules = NULL;
        free(arr);
        free(rec);
    }

    /* A local mod serves a stranger's unit only by exact hash (D21), checked again when the file changes (D51). */
    {
        char mods4[MAX_PATH * 2], local[MAX_PATH * 2], img[MAX_PATH * 3];
        sh_mb_bundle lb;
        _snprintf_s(mods4, sizeof mods4, _TRUNCATE, "%s\\player4\\mods", top);
        _snprintf_s(local, sizeof local, _TRUNCATE, "%s\\crates", mods4);
        { char p4[MAX_PATH * 2]; _snprintf_s(p4, sizeof p4, _TRUNCATE, "%s\\player4", top); CreateDirectoryA(p4, NULL); CreateDirectoryA(mods4, NULL); }
        CHECK(mt_copy_tree(pack, local));
        if (build(local, &lb)) {
            char *w = NULL;
            CHECK(sh_mb_write(&lb, local, &w));
            free(w);
            sh_mb_free(&lb);
        }
        sh_cache_set_mods_root(mods4);
        download(&crate, &b, pack, 0);    /* the manifests only: the files are the local mod's */
        download(&barrel, &b, pack, 0);
        CHECK(sh_cache_unit_state(crate.hash, NULL, NULL, NULL) == 1);
        _snprintf_s(img, sizeof img, _TRUNCATE, "%s\\images\\barrel_d.bimage", local);
        mt_write(local, "images\\barrel_d.bimage", "the author edited this image");
        CHECK(sh_cache_unit_state(barrel.hash, NULL, NULL, NULL) == 0);   /* another SHA-256: inert */
        CHECK(sh_cache_unit_state(crate.hash, NULL, NULL, NULL) == 1);
        (void)img;
    }

    /* The sweep: what no kept map names goes; a map only played keeps its content until "Remove content of maps
     * you only played". */
    {
        char saved[MAX_PATH * 2], played[MAX_PATH * 2], saves[MAX_PATH * 2], err[128], *map;
        const char *body = "{\"entities\":[]}";
        sh_modmanifest_list one;
        sh_cache_sweep_stats st;
        sh_modmanifest_row crate_row;
        const sh_unit *only[1] = { &crate };
        size_t len = 0;
        sh_cache_set_mods_root(mods);
        _snprintf_s(saved, sizeof saved, _TRUNCATE, "%s\\player\\saved-maps", top);
        _snprintf_s(played, sizeof played, _TRUNCATE, "%s\\player\\published-maps", top);
        CreateDirectoryA(saved, NULL);
        CreateDirectoryA(played, NULL);
        sh_cache_set_map_folders(saved, played);
        /* The local saves are this test's own folder, empty for now. */
        _snprintf_s(saves, sizeof saves, _TRUNCATE, "%s\\player\\saves", top);
        CreateDirectoryA(saves, NULL);
        sh_saved_maps_set_root(saves);
        row_of(&crate_row, SALT, only, 1);
        memset(&one, 0, sizeof one);
        CHECK(sh_modmanifest_list_add(&one, &crate_row));
        map = sh_modmanifest_write(body, strlen(body), &one, &len, err, sizeof err);
        CHECK(map != NULL);
        if (map) {
            FILE *f = NULL;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\aaaa_bbbb_map.decl", saved);
            if (!fopen_s(&f, path, "wb") && f) { fwrite(map, 1, len, f); fclose(f); }
        }
        sh_modmanifest_list_free(&one);
        free(map);
        /* The both-objects map, only played. */
        memset(&one, 0, sizeof one);
        CHECK(sh_modmanifest_list_add(&one, &rows[0]));
        map = sh_modmanifest_write(body, strlen(body), &one, &len, err, sizeof err);
        if (map) {
            FILE *f = NULL;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\cccc_dddd_map.decl", played);
            if (!fopen_s(&f, path, "wb") && f) { fwrite(map, 1, len, f); fclose(f); }
        }
        sh_modmanifest_list_free(&one);
        free(map);
        CHECK(sh_cache_sweep(0, NULL, 0, &st));
        CHECK(st.units_removed == 0 && st.served_removed == 0);   /* both compositions are some kept map's */
        CHECK(sh_cache_unit_state(barrel.hash, NULL, NULL, NULL) == 1);
        t = sh_cache_storage_line();
        CHECK(t && !strncmp(t, "Map content: ", 13) && strstr(t, "for 2 maps."));
        if (t) printf("  %s\n", t);
        free(t);
        CHECK(sh_cache_sweep(1, NULL, 0, &st));      /* Remove content of maps you only played */
        CHECK(st.units_removed == 1 && st.served_removed == 1 && sh_cache_unit_state(barrel.hash, NULL, NULL, NULL) == -1);
        CHECK(sh_cache_unit_state(crate.hash, NULL, NULL, NULL) == 1);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\files", mods);
        CHECK(count_files(path) < blobs);            /* the barrel's own files went; the shared one stayed */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\files\\%.2s", mods, crate.hash);
        /* An upload in progress keeps its unit (held). */
        {
            const char *held[1] = { barrel.hash };
            download(&barrel, &b, pack, 1);
            CHECK(sh_cache_sweep(1, held, 1, &st) && sh_cache_unit_state(barrel.hash, NULL, NULL, NULL) == 1);
        }
        /* R-DS6: a map saved offline exists only in its local slot (My Maps), and its unit stays; its cloud copy in
         * saved-maps is the same map, reported once. A slot that does not decode may use anything: nothing goes. */
        {
            sh_modmanifest_row barrel_row;
            const sh_unit *bonly[1] = { &barrel };
            row_of(&barrel_row, SALT, bonly, 1);
            memset(&one, 0, sizeof one);
            CHECK(sh_modmanifest_list_add(&one, &barrel_row));
            map = sh_modmanifest_write(body, strlen(body), &one, &len, err, sizeof err);
            CHECK(map != NULL);
            if (map) write_slot(saves, "0123456789ABCDEF0123", "aaaa", map, len);
            free(map);
            sh_modmanifest_list_free(&one);
            sh_modmanifest_row_free(&barrel_row);
            CHECK(sh_cache_sweep(1, NULL, 0, &st));
            CHECK(st.unreadable == 0 && sh_cache_unit_state(barrel.hash, NULL, NULL, NULL) == 1);   /* failed before */
            CHECK(sh_cache_unit_state(crate.hash, NULL, NULL, NULL) == 1);
            t = sh_cache_storage_line();
            /* The slot, its cloud copy aaaa_bbbb (the slot's item is aaaa: not counted again) and the map played. */
            CHECK(t && strstr(t, "for 2 maps."));
            if (t) printf("  %s\n", t);
            free(t);
            write_slot(saves, "FEDCBA98765432100123", "", NULL, 64);
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\aaaa_bbbb_map.decl", saved);
            CHECK(DeleteFileA(path));   /* the crate is now no map's ... */
            CHECK(sh_cache_sweep(1, NULL, 0, &st));
            CHECK(st.unreadable == 1 && st.units_removed == 0 && sh_cache_unit_state(crate.hash, NULL, NULL, NULL) == 1);
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\76561198000000001\\SNAPMAPSFEDCBA98765432100123\\map.decl", saves);
            CHECK(DeleteFileA(path));   /* ... and once every kept map reads, it goes */
            CHECK(sh_cache_sweep(1, NULL, 0, &st));
            CHECK(st.unreadable == 0 && st.units_removed == 1 && sh_cache_unit_state(crate.hash, NULL, NULL, NULL) == -1);
            CHECK(sh_cache_unit_state(barrel.hash, NULL, NULL, NULL) == 1);
        }
        sh_saved_maps_set_root(NULL);
        sh_modmanifest_row_free(&crate_row);
    }
    sh_modmanifest_row_free(&rows[0]);
    sh_modmanifest_row_free(&rows[1]);
    sh_unit_free(&crate);
    sh_unit_free(&barrel);
    sh_mb_free(&b);
}

static void test_key(const char *top)
{
    sh_key *k = NULL, *back = NULL;
    char *why = NULL, s1[65], s2[65], s3[65], folder[MAX_PATH * 2];
    wchar_t *headers = NULL;
    modnet_signer signer;
    CHECK(sh_key_create(&k, &why));
    if (!k) { fprintf(stderr, "key: %s\n", why ? why : "?"); free(why); return; }
    CHECK(strlen(sh_key_public(k)) == SH_KEY_PUBLIC_B64 && sh_key_public(k)[0] == 'B');   /* 0x04 point */
    CHECK(sh_key_claim_secret(k, "item-1", s1) && sh_key_claim_secret(k, "item-1", s2) && sh_key_claim_secret(k, "item-2", s3));
    CHECK(!strcmp(s1, s2) && strcmp(s1, s3));
    signer = sh_key_signer(k);
    CHECK(sh_key_set_name(k, "\xc3\x85lice B"));
    CHECK(signer.sign(signer.context, "POST", "/mods/map/claim", s1, &headers) && headers &&
          wcsstr(headers, L"X-Publisher-Name: %C3%85lice%20B\r\n") && wcsstr(headers, L"X-Publisher-Signature: "));
    free(headers);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\publishing", top);
    CHECK(sh_key_save(k, folder, &why));
    CHECK(sh_key_load_or_create(folder, &back, &why) && back);
    if (back) {
        CHECK(!strcmp(sh_key_public(back), sh_key_public(k)));
        CHECK(sh_key_claim_secret(back, "item-1", s3) && !strcmp(s1, s3));   /* the key restores its claims (D8) */
    }
    sh_key_free(back);
    sh_key_free(k);
    free(why);
}

int main(void)
{
    char top[MAX_PATH];
    if (!mt_temp_dir("modcache", top, sizeof top)) { fprintf(stderr, "no temp folder\n"); return 1; }
    sh_mb_set_script_check(script_ok);
    sh_cache_set_resolver(&g_game);
    printf("-- composition hash\n");
    test_pointer_sources();
    test_net_retry();
    test_composition_hash();
    printf("-- format 2 rows\n");
    test_rows_format2();
    printf("-- notices\n");
    test_notices();
    printf("-- the cache: compose, one copy, determinism, stand-ins, local mods, sweep\n");
    test_cache(top);
    printf("-- the publishing key\n");
    test_key(top);
    (void)g_current_rows;
    mt_remove_tree(top);
    if (failures) { fprintf(stderr, "modcache_test: %d check(s) failed\n", failures); return 1; }
    printf("modcache_test: all checks passed\n");
    return 0;
}
