/* modsync_test -- one archive per build between a temporary local store and
 * a loopback stub of the mod store (mod_store_stub.h), ONE-SHAPE rule 5:
 *   upload: HEAD first, an archive written and PUT only for a build the
 *   store lacks (mod.json whole, assets/<request> per carried file, stock
 *   pointers left out), one claim per ref kept in the claims section and
 *   re-claimed with its secret, a mod the machine lacks or rebuilt since the
 *   map was saved refused with the next step, a file changed since its build
 *   refused, the 96 MiB cap refused before any request with the plain
 *   sentence, a 503 queued in the pending section and retried, a 422 not
 *   queued, a cancelled push queued, a release that deletes what no claim
 *   holds, a release queued on a 503 and retried after the next success;
 *   the gate's probe (found with size, 404, 451, 503, nothing listening);
 *   download into a map's place: the archive unpacked to
 *   mods\maps\<title>\<mod name>\ with mod.json byte-identical, the place
 *   noted and the serving index rescanned, nothing fetched again, the root at
 *   the row's hash serving it with nothing fetched, the root at another hash
 *   not, a republish replacing the place's build of the salt, two maps
 *   unpacking their own copies, and refusals with nothing placed: an archive
 *   of another build or salt, a corrupted transfer, an extra entry, and a
 *   borrow the game lacks (MODNET_E_GAME); an editor-tweaks build installed
 *   like any other; a download for the root installed there directly.
 *
 * The local store and the serving index are stubbed below (the walk, the
 * places, the rescan) so the suite tests this lane's code alone.
 */
#include "mod_store_stub.h"
#include "../src/backend/modnet.h"
#include "../src/backend/modsync.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modsalt.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static char g_log[256 * 1024];
static size_t g_log_n;
void backend_log(const char *line)
{
    size_t n = line ? strlen(line) : 0;
    if (n && g_log_n + n + 2 < sizeof g_log) { memcpy(g_log + g_log_n, line, n); g_log_n += n; g_log[g_log_n++] = '\n'; g_log[g_log_n] = 0; }
}

/* ------------------------------------------------ the local store, stubbed */

static char g_root[MAX_PATH * 2];
static CRITICAL_SECTION g_store_cs;
static int g_rescans, g_notes;
static char g_note_folder[512], g_note_title[256];

void modstore_lock(void) { EnterCriticalSection(&g_store_cs); }
void modstore_unlock(void) { LeaveCriticalSection(&g_store_cs); }
int sh_ms_root(char *out, size_t cap) { strncpy_s(out, cap, g_root, _TRUNCATE); return g_root[0] != 0; }
int sh_modserve_index_rescan(void) { g_rescans++; return 0; }
int sh_ms_entry_usable(const sh_ms_entry *e) { return e && e->loaded && e->bundle.validated; }

void sh_ms_list_free(sh_ms_list *l)
{
    size_t i;
    for (i = 0; i < l->count; i++) { free(l->items[i].folder); sh_mb_free(&l->items[i].bundle); }
    free(l->items);
    memset(l, 0, sizeof *l);
}

static void st_add(sh_ms_list *l, const char *folder, const char *name)
{
    sh_ms_entry *e;
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (sh_ms_entry *)realloc(l->items, sizeof *l->items * l->cap);
    }
    e = &l->items[l->count++];
    memset(e, 0, sizeof *e);
    e->folder = _strdup(folder);
    strncpy_s(e->name, sizeof e->name, name, _TRUNCATE);
    e->loaded = sh_mb_load_folder(folder, &e->bundle);
}

/* The walk: root folders, then mods\maps\<place>\<mod>. */
static void st_walk(sh_ms_list *l, const char *dir, const char *prefix, int places)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char sub[MAX_PATH * 2], name[512], mj[MAX_PATH * 2];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        if (strstr(fd.cFileName, ".staging-") || strstr(fd.cFileName, ".old-")) continue;
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        _snprintf_s(name, sizeof name, _TRUNCATE, "%s%s", prefix, fd.cFileName);
        if (!prefix[0] && !_stricmp(fd.cFileName, "maps")) {
            WIN32_FIND_DATAA pd;
            char pp[MAX_PATH * 2];
            HANDLE ph;
            _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s\\*", sub);
            ph = FindFirstFileA(pp, &pd);
            if (ph == INVALID_HANDLE_VALUE) continue;
            do {
                char place[MAX_PATH * 2], pre[512];
                if (!(pd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || pd.cFileName[0] == '.') continue;
                _snprintf_s(place, sizeof place, _TRUNCATE, "%s\\%s", sub, pd.cFileName);
                _snprintf_s(pre, sizeof pre, _TRUNCATE, "maps\\%s\\", pd.cFileName);
                st_walk(l, place, pre, 1);
            } while (FindNextFileA(ph, &pd));
            FindClose(ph);
            continue;
        }
        _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\mod.json", sub);
        if (mt_exists(mj)) st_add(l, sub, name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    (void)places;
}

int sh_ms_enumerate(sh_ms_list *out)
{
    memset(out, 0, sizeof *out);
    st_walk(out, g_root, "", 0);
    return 1;
}

/* A place: mods\maps\<title>\, its folder of the salt, else <name> free. */
int sh_ms_place_folder(const sh_ms_place *place, const char *name, const char *salt, char *out, size_t cap)
{
    sh_ms_list l;
    char pre[512];
    size_t i, n;
    int k;
    _snprintf_s(pre, sizeof pre, _TRUNCATE, "maps\\%s\\", place->title);
    n = strlen(pre);
    sh_ms_enumerate(&l);
    for (i = 0; i < l.count; i++)
        if (!_strnicmp(l.items[i].name, pre, n) && l.items[i].loaded && !strcmp(l.items[i].bundle.id, salt)) {
            strncpy_s(out, cap, l.items[i].name, _TRUNCATE);
            sh_ms_list_free(&l);
            return 2;
        }
    sh_ms_list_free(&l);
    for (k = 1; k < 10; k++) {
        char full[MAX_PATH * 2];
        if (k == 1) _snprintf_s(out, cap, _TRUNCATE, "%s%s", pre, name);
        else _snprintf_s(out, cap, _TRUNCATE, "%s%s (%d)", pre, name, k);
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", g_root, out);
        if (!mt_exists(full)) {
            char parent[MAX_PATH * 2];
            _snprintf_s(parent, sizeof parent, _TRUNCATE, "%s\\maps\\%s\\x", g_root, place->title);
            mt_mkdirs_for(parent);
            return 1;
        }
    }
    return 0;
}

int sh_ms_title_segment(const char *title, char *out, size_t cap)
{
    strncpy_s(out, cap, title, _TRUNCATE);
    return out[0] != 0;
}

/* The store's install at the root, as modstore.c does it: a built folder
 * copied to mods\<its leaf> under its salt; a root folder of the salt is the
 * install at this build and a refusal at another. */
int sh_ms_install_to_root(const char *folder, const sh_mb_resolver *resolver, char *name_out, size_t name_cap,
                          sh_ms_install_stats *stats, sh_mb_bundle *bundle_out, char *cause, size_t cap)
{
    sh_mb_bundle b;
    sh_ms_list l;
    size_t i;
    const char *leaf = strrchr(folder, '\\') ? strrchr(folder, '\\') + 1 : folder;
    char to[MAX_PATH * 2];
    int ok = 0;
    (void)resolver;
    (void)bundle_out;
    if (stats) memset(stats, 0, sizeof *stats);
    memset(&b, 0, sizeof b);
    if (!sh_mb_load_folder(folder, &b)) { strncpy_s(cause, cap, "it was never built", _TRUNCATE); return 0; }
    sh_ms_enumerate(&l);
    for (i = 0; i < l.count; i++) {
        const sh_ms_entry *e = &l.items[i];
        if (strchr(e->name, '\\') || !e->loaded || strcmp(e->bundle.id, b.id)) continue;
        if (!strcmp(e->bundle.hash, b.hash)) {
            strncpy_s(name_out, name_cap, e->name, _TRUNCATE);
            if (stats) stats->present = 1;
            ok = 1;
        } else _snprintf_s(cause, cap, _TRUNCATE, "your mods already hold %s in mods\\%s", b.name ? b.name : b.id, e->name);
        sh_ms_list_free(&l);
        sh_mb_free(&b);
        return ok;
    }
    sh_ms_list_free(&l);
    _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\%s", g_root, leaf);
    ok = !mt_exists(to) && mt_copy_tree(folder, to);
    if (ok) strncpy_s(name_out, name_cap, leaf, _TRUNCATE);
    else strncpy_s(cause, cap, "it could not be copied", _TRUNCATE);
    sh_mb_free(&b);
    return ok;
}

int sh_ms_place_note(const sh_ms_place *place, const char *folder)
{
    g_notes++;
    strncpy_s(g_note_folder, sizeof g_note_folder, folder, _TRUNCATE);
    strncpy_s(g_note_title, sizeof g_note_title, place && place->title ? place->title : "", _TRUNCATE);
    return 1;
}

/* ------------------------------------------------------------ fixtures -- */

#define ROCK_SOURCE "generated/image/stock/rock.bimage"
static const char ROCK[] = "ROCK-GAME-BYTES-0123456789";
static int g_game_lacks;

static int rock_same(unsigned long long size, const char *sha)
{
    char h[65];
    sh_mb_sha256_hex(ROCK, strlen(ROCK), h);
    return size == strlen(ROCK) && !strcmp(h, sha);
}
static int rock_match(void *ctx, const char *request, unsigned long long size, const char *sha, char *source, size_t cap)
{
    (void)ctx; (void)request;
    if (g_game_lacks || !rock_same(size, sha)) return SH_MB_STOCK_ABSENT;
    strcpy_s(source, cap, ROCK_SOURCE);
    return SH_MB_STOCK_OK;
}
static int rock_identify(void *ctx, const char *source, char sha[65], unsigned long long *size)
{
    (void)ctx;
    if (g_game_lacks || strcmp(source, ROCK_SOURCE)) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex(ROCK, strlen(ROCK), sha);
    *size = strlen(ROCK);
    return SH_MB_STOCK_OK;
}
static int rock_verify(void *ctx, const char *source, const char *sha, unsigned long long *size)
{
    char h[65];
    (void)ctx;
    if (strcmp(source, ROCK_SOURCE)) return SH_MB_STOCK_ABSENT;
    if (g_game_lacks) return SH_MB_STOCK_MISMATCH;
    sh_mb_sha256_hex(ROCK, strlen(ROCK), h);
    if (strcmp(h, sha)) return SH_MB_STOCK_MISMATCH;
    if (size) *size = strlen(ROCK);
    return SH_MB_STOCK_OK;
}
static int rock_read(void *ctx, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{
    unsigned long long size = 0;
    int r = rock_verify(ctx, source, sha, &size);
    if (r != SH_MB_STOCK_OK) return r;
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)size + 1);
    if (!*bytes) return SH_MB_STOCK_UNAVAILABLE;
    memcpy(*bytes, ROCK, (size_t)size);
    *len = (size_t)size;
    return SH_MB_STOCK_OK;
}
static const sh_mb_stock_ops GAME = { NULL, rock_match, rock_identify, rock_verify, rock_read };

static int stock_yes(void *context, const char *request) { (void)context; (void)request; return 1; }

/* A mod folder: mod.json's author part, files under assets\ (relative paths
 * with '/'), built with `salt` and written. Returns 1 with the build hash. */
static int make_mod(const char *dir, const char *salt, const char *name, const char *const *files,
                    const char *const *bytes, const sh_mb_resolver *resolver, char hash[65])
{
    char text[512], cause[SH_MB_CAUSE_MAX];
    sh_mb_bundle b;
    int i, ok;
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"name\":\"%s\",\"version\":\"1.0.0\",\"author\":\"tester\"}", name);
    mt_write(dir, "mod.json", text);
    for (i = 0; files[i]; i++) {
        char rel[MAX_PATH], *c;
        strcpy_s(rel, sizeof rel, files[i]);
        for (c = rel; *c; c++) if (*c == '/') *c = '\\';
        mt_write(dir, rel, bytes[i]);
    }
    memset(&b, 0, sizeof b);
    ok = sh_mb_build(dir, salt, resolver, &b) && sh_mb_write(&b, dir, cause, sizeof cause);
    if (!ok) fprintf(stderr, "build %s: %s %s\n", dir, b.first_cause, cause);
    if (ok) strcpy_s(hash, 65, b.hash);
    sh_mb_free(&b);
    return ok;
}

static void mod_of(char *out, const char *salt, const char *hash) { _snprintf_s(out, MODSYNC_MOD_MAX + 1, _TRUNCATE, "%s@%s", salt, hash); }

static void row_of(sh_modmanifest_row *row, const char *salt, const char *hash)
{
    memset(row, 0, sizeof *row);
    strcpy_s(row->salt, sizeof row->salt, salt);
    strcpy_s(row->build, sizeof row->build, hash);
    row->size = 64;
}

static sh_ms_place place_of(const char *title, const char *map_id)
{
    sh_ms_place p;
    memset(&p, 0, sizeof p);
    p.title = title;
    p.map_id = map_id;
    return p;
}

static void use_root(const char *root) { strcpy_s(g_root, sizeof g_root, root); }

static int file_has(const char *path, const char *needle)
{
    size_t len = 0;
    unsigned char *t = sh_mb_read_file(path, 1 << 20, &len);
    int hit = t && strstr((const char *)t, needle) != NULL;
    free(t);
    return hit;
}

static int same_file(const char *a, const char *b)
{
    size_t al = 0, bl = 0;
    unsigned char *x = sh_mb_read_file(a, 1 << 20, &al), *y = sh_mb_read_file(b, 1 << 20, &bl);
    int same = x && y && al == bl && !memcmp(x, y, al);
    free(x);
    free(y);
    return same;
}

static unsigned count_dirs(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    unsigned n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') n++;
    while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* The stub's stored archive for hash: its entry count, and whether `name` is one. */
static int stored_entries(const char *hash, const char *name, int *has)
{
    char path[MAX_PATH * 2], cause[200];
    sh_zip_reader zr;
    stub_obj *o;
    int n = -1;
    FILE *f;
    *has = 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\peek.zip", g_root);
    EnterCriticalSection(&g_stub.cs);
    o = stub_find(g_stub.builds, g_stub.nbuilds, hash);
    if (o && !fopen_s(&f, path, "wb") && f) { fwrite(o->bytes, 1, o->len, f); fclose(f); } else o = NULL;
    LeaveCriticalSection(&g_stub.cs);
    if (o && sh_zip_reader_open(&zr, path, cause, sizeof cause)) {
        n = (int)zr.n;
        *has = sh_zip_reader_find(&zr, name) >= 0;
        sh_zip_reader_close(&zr);
    }
    DeleteFileA(path);
    return n;
}

/* The stub's archive bytes for hash (malloc'd). */
static unsigned char *stored_copy(const char *hash, size_t *len)
{
    stub_obj *o;
    unsigned char *out = NULL;
    EnterCriticalSection(&g_stub.cs);
    o = stub_find(g_stub.builds, g_stub.nbuilds, hash);
    if (o) { out = (unsigned char *)malloc(o->len); memcpy(out, o->bytes, o->len); *len = o->len; }
    LeaveCriticalSection(&g_stub.cs);
    return out;
}

static const char *const A_FILES[] = { "assets/generated/image/mods/alpha001/a.bimage", "assets/generated/image/mods/alpha001/b.bimage", NULL };
static const char *const A_BYTES[] = { "alpha bytes", "alpha second file", NULL };
static const char *const A2_BYTES[] = { "alpha bytes, republished", "alpha second file", NULL };
static const char *const B_FILES[] = { "assets/generated/image/mods/betab002/b.bimage", NULL };
static const char *const B_BYTES[] = { "beta bytes", NULL };
static const char *const R_FILES[] = { "assets/generated/image/mods/rocks003/rock.bimage", "assets/generated/image/mods/rocks003/own.bimage", NULL };
static const char *const R_BYTES[] = { ROCK, "the rock mod's own bytes", NULL };
static const char *const C_FILES[] = { "assets/generated/image/mods/gamma004/c.bimage", NULL };
static const char *const C_BYTES[] = { "gamma bytes", NULL };
static const char *const E_FILES[] = { "assets/generated/decls/snapeditorentitydef/func/rotate.decl", NULL };
static const char *const E_BYTES[] = { "{ entityDef \"func/rotate\" }", NULL };

int main(void)
{
    char url[64], tmp[MAX_PATH], r1[MAX_PATH * 2], r2[MAX_PATH * 2], dir[MAX_PATH * 2], path[MAX_PATH * 2];
    char ha[65], ha2[65], hb[65], hr[65], hc[65], he[65], ma[80], mb[80], mr[80], mc[80], me[80], cause[1024];
    const char *list[4];
    modsync_stats st;
    modnet_result r;
    sh_mb_resolver stock = { NULL, NULL, stock_yes, NULL };
    int before, n;
    unsigned done = 0, dropped = 0;

    InitializeCriticalSection(&g_store_cs);
    sh_mb_set_stock_ops(&GAME);
    CHECK(stub_start() > 0);
    stub_url(url, sizeof url);
    CHECK(modnet_set_base_url(url));
    CHECK(mt_temp_dir("modsync", tmp, sizeof tmp));
    _snprintf_s(r1, sizeof r1, _TRUNCATE, "%s\\author\\mods", tmp);
    _snprintf_s(r2, sizeof r2, _TRUNCATE, "%s\\player\\mods", tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\x", r2);
    mt_mkdirs_for(path);
    use_root(r1);

    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\Alpha Pack", r1);
    CHECK(make_mod(dir, "alpha001", "Alpha", A_FILES, A_BYTES, NULL, ha));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\beta", r1);
    CHECK(make_mod(dir, "betab002", "Beta", B_FILES, B_BYTES, NULL, hb));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\rocks", r1);
    CHECK(make_mod(dir, "rocks003", "Rocks", R_FILES, R_BYTES, NULL, hr));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\editor", r1);
    CHECK(make_mod(dir, "edito005", "Editor tweaks", E_FILES, E_BYTES, &stock, he));
    mod_of(ma, "alpha001", ha);
    mod_of(mb, "betab002", hb);
    mod_of(mr, "rocks003", hr);
    mod_of(me, "edito005", he);
    CHECK(modsync_parse_mod(ma, NULL, NULL) && !modsync_parse_mod(ha, NULL, NULL) && !modsync_parse_mod("Alpha001@x", NULL, NULL));

    /* ---- upload: HEAD, archive, PUT, one claim ---- */
    list[0] = ma; list[1] = mb; list[2] = ma;   /* a build named twice is sent once */
    CHECK(modsync_upload_builds(list, 3, "item-1", NULL, &st));
    CHECK(st.code == MODNET_OK && st.builds == 2 && st.builds_present == 0 && st.builds_sent == 2 && st.builds_created == 2);
    CHECK(g_stub.head_calls == 2 && g_stub.build_puts == 2 && g_stub.claim_calls == 1 && stub_has_build(ha) && stub_has_build(hb));
    CHECK(stub_claim_builds("item-1", NULL) == 2 && strlen(st.claim) == 32 && !st.reclaimed);
    CHECK(modsync_claim_lookup("item-1", NULL, NULL) && modsync_claim_names(ha) && modsync_claim_names(hb));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", r1);
    CHECK(file_has(path, "\"builds\"") && !file_has(path, "\"bundles\""));
    {
        int has = 0;
        CHECK(stored_entries(ha, "assets/generated/image/mods/alpha001/b.bimage", &has) == 3 && has);
        CHECK(stored_entries(ha, "mod.json", &has) == 3 && has);
    }
    /* Again: the store holds both; nothing is written or sent; the secret re-claims. */
    before = g_stub.build_puts;
    CHECK(modsync_upload_builds(list, 2, "item-1", NULL, &st));
    CHECK(st.builds_present == 2 && st.builds_sent == 0 && g_stub.build_puts == before && st.reclaimed && g_stub.claim_with_secret == 1);

    /* Stock pointers never upload. */
    list[0] = mr;
    CHECK(modsync_upload_builds(list, 1, "item-rocks", NULL, &st));
    CHECK(st.stock_files == 1 && st.builds_sent == 1);
    {
        int has = 1;
        CHECK(stored_entries(hr, "assets/generated/image/mods/rocks003/rock.bimage", &has) == 2 && !has);
    }

    /* ---- upload refusals with the next step ---- */
    {
        char unknown[80], h[65];
        sh_mb_sha256_hex("unknown", 7, h);
        mod_of(unknown, "zzzzzzz9", h);
        list[0] = unknown;
        before = g_stub.requests;
        CHECK(!modsync_upload_builds(list, 1, "item-2", NULL, &st));
        CHECK(st.code == MODNET_E_ARG && strstr(st.cause, "not on this computer") && !st.queued && g_stub.requests == before);
    }
    /* Alpha built again: the map's row names the earlier build. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\Alpha Pack", r1);
    CHECK(make_mod(dir, "alpha001", "Alpha", A_FILES, A2_BYTES, NULL, ha2));
    CHECK(strcmp(ha, ha2) != 0);
    list[0] = ma;
    CHECK(!modsync_upload_builds(list, 1, "item-3", NULL, &st));
    CHECK(strstr(st.cause, "Alpha was built again since the map was saved; save the map again") && !st.queued);
    /* A file changed since the build. */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gamma", r1);
    CHECK(make_mod(dir, "gamma004", "Gamma", C_FILES, C_BYTES, NULL, hc));
    mod_of(mc, "gamma004", hc);
    mt_write(dir, "assets\\generated\\image\\mods\\gamma004\\c.bimage", "gamma bytes, edited by hand");
    list[0] = mc;
    before = g_stub.build_puts;
    CHECK(!modsync_upload_builds(list, 1, "item-4", NULL, &st));
    CHECK(strstr(st.cause, "changed since the mod was built; open the Mods tab and Refresh") && g_stub.build_puts == before && !st.queued);
    CHECK(make_mod(dir, "gamma004", "Gamma", C_FILES, C_BYTES, NULL, hc));
    /* Larger than one request: sent in parts, then claimed like any other. */
    {
        static const char *const BIG_FILES[] = { "assets/generated/image/mods/hugee006/one.bimage",
                                                 "assets/generated/image/mods/hugee006/two.bimage", NULL };
        static const char *const BIG_TEXT[] = { "x", "y", NULL };
        char hbig[65], mbig[80], full[MAX_PATH * 2];
        size_t sz = 49u * 1024u * 1024u, k;
        unsigned char *fill = (unsigned char *)malloc(sz);
        FILE *f;
        int i;
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\huge", r1);
        mt_write(dir, "mod.json", "{\"name\":\"Huge\",\"version\":\"1.0.0\"}");
        for (i = 0; BIG_FILES[i] && fill; i++) {
            for (k = 0; k < sz; k++) fill[k] = (unsigned char)(k * 31 + i);
            _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", dir, BIG_FILES[i]);
            { char *c; for (c = full; *c; c++) if (*c == '/') *c = '\\'; }
            mt_mkdirs_for(full);
            if (!fopen_s(&f, full, "wb") && f) { fwrite(fill, 1, sz, f); fclose(f); }
        }
        free(fill);
        (void)BIG_TEXT;
        {
            sh_mb_bundle b;
            char why[SH_MB_CAUSE_MAX];
            memset(&b, 0, sizeof b);
            CHECK(sh_mb_build(dir, "hugee006", NULL, &b) && sh_mb_write(&b, dir, why, sizeof why));
            strcpy_s(hbig, sizeof hbig, b.hash);
            sh_mb_free(&b);
        }
        mod_of(mbig, "hugee006", hbig);
        list[0] = mbig;
        before = g_stub.part_puts;
        CHECK(modsync_upload_builds(list, 1, "item-huge", NULL, &st));
        CHECK(st.code == MODNET_OK && g_stub.part_puts == before + 2 && stub_has_build(hbig) && st.builds_sent == 1);
        CHECK(modsync_release("item-huge", NULL, &st) && !stub_has_build(hbig));
        mt_remove_tree(dir);
    }
    /* No file count: a mod of 20,000 small files builds, uploads and is
     * claimed. */
    {
        char hmany[65], mmany[80], rel[MAX_PATH];
        int i;
        DWORD t0 = GetTickCount();
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\many", r1);
        mt_write(dir, "mod.json", "{\"name\":\"Many\"}");
        for (i = 0; i < 20000; i++) {
            char body[48];
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\manyf007\\f%05d.decl", i);
            _snprintf_s(body, sizeof body, _TRUNCATE, "entityDef f%05d {}\n", i);
            mt_write(dir, rel, body);
        }
        {
            sh_mb_bundle b;
            char why[SH_MB_CAUSE_MAX];
            memset(&b, 0, sizeof b);
            CHECK(sh_mb_build(dir, "manyf007", NULL, &b) && sh_mb_write(&b, dir, why, sizeof why) && b.file_count == 20000);
            if (b.first_cause[0]) fprintf(stderr, "  many: %s\n", b.first_cause);
            strcpy_s(hmany, sizeof hmany, b.hash);
            sh_mb_free(&b);
        }
        mod_of(mmany, "manyf007", hmany);
        list[0] = mmany;
        CHECK(modsync_upload_builds(list, 1, "item-many", NULL, &st) && stub_has_build(hmany));
        if (st.cause[0]) fprintf(stderr, "  many: %s\n", st.cause);
        CHECK(modsync_release("item-many", NULL, &st) && !stub_has_build(hmany));
        printf("modsync_test: 20,000 files built, archived and uploaded in %lu ms\n", GetTickCount() - t0);
        mt_remove_tree(dir);
    }

    /* ---- transient failures are queued and retried; refusals are not ---- */
    list[0] = mc;
    stub_fail_next("PUT /mods/build/", 503);
    CHECK(!modsync_upload_builds(list, 1, "item-5", NULL, &st));
    CHECK(st.code == MODNET_E_UNAVAILABLE && st.queued && modsync_pending_count() == 1);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", r1);
    CHECK(file_has(path, "\"gamma004@"));
    n = modsync_retry_pending(NULL, &done, &dropped, &st);
    CHECK(n == 0 && done == 1 && dropped == 0 && stub_has_build(hc) && stub_claim_builds("item-5", NULL) == 1);
    stub_fail_next("PUT /mods/build/", 422);
    list[0] = ma;   /* the row of the earlier Alpha is no longer anywhere: use the current one */
    {
        char ma2[80];
        mod_of(ma2, "alpha001", ha2);
        list[0] = ma2;
        CHECK(!modsync_upload_builds(list, 1, "item-6", NULL, &st));
        CHECK(st.code == MODNET_E_REFUSED && !st.queued && modsync_pending_count() == 0);
        {
            volatile long cancel = 1;
            modnet_io io = { &cancel, NULL, NULL };
            CHECK(!modsync_upload_builds(list, 1, "item-7", &io, &st));
            CHECK(st.code == MODNET_E_CANCELLED && st.queued && modsync_pending_count() == 1);
            modsync_pending_drop("item-7");
            CHECK(modsync_pending_count() == 0);
        }
        CHECK(modsync_upload_builds(list, 1, "item-6", NULL, &st) && stub_has_build(ha2));
    }

    /* ---- release ---- */
    {
        list[0] = mb;
        CHECK(modsync_upload_builds(list, 1, "item-8", NULL, &st));   /* a second claim on Beta */
        CHECK(modsync_release("item-1", NULL, &st));
        CHECK(!modsync_claim_lookup("item-1", NULL, NULL) && !modsync_claim_names(ha));
        CHECK(!stub_has_build(ha) && stub_has_build(hb));   /* Beta is still claimed by item-8 */
        stub_fail_next("POST /mods/release", 503);
        CHECK(!modsync_release("item-8", NULL, &st) && st.queued && modsync_pending_count() == 1);
        /* The next store call that succeeds retries the queued release. */
        list[0] = mr;
        CHECK(modsync_upload_builds(list, 1, "item-rocks", NULL, &st));
        CHECK(modsync_pending_count() == 0 && !modsync_claim_lookup("item-8", NULL, NULL) && !stub_has_build(hb));
        CHECK(!modsync_release("never-claimed", NULL, &st) && st.code == MODNET_E_NOT_FOUND);
    }

    /* ---- the gate's probe ---- */
    {
        modsync_probe p;
        CHECK(modsync_probe_build(hr, NULL, &p) == 1 && p.size > 0 && p.scripts == 0);
        /* The store's answer says how many scripts the build carries. */
        EnterCriticalSection(&g_stub.cs);
        stub_find(g_stub.builds, g_stub.nbuilds, hr)->scripts = 3;
        LeaveCriticalSection(&g_stub.cs);
        CHECK(modsync_probe_build(hr, NULL, &p) == 1 && p.scripts == 3);
        CHECK(modsync_probe_build(ha, NULL, &p) == 0 && p.code == MODNET_E_NOT_FOUND);
        stub_fail_next("HEAD /mods/build/", 451);
        CHECK(modsync_probe_build(hr, NULL, &p) == 0 && p.code == MODNET_E_BLOCKED);
        stub_fail_next("HEAD /mods/build/", 503);
        CHECK(modsync_probe_build(hr, NULL, &p) == -1 && p.code == MODNET_E_UNAVAILABLE);
        CHECK(modnet_set_base_url("http://127.0.0.1:1"));
        CHECK(modsync_probe_build(hr, NULL, &p) == -1 && modnet_unreachable(p.code));
        CHECK(modnet_set_base_url(url));
    }

    /* ---- download into a map's place ---- */
    list[0] = me;
    CHECK(modsync_upload_builds(list, 1, "item-editor", NULL, &st));
    {
        char ma2[80];
        mod_of(ma2, "alpha001", ha2);
        list[0] = ma2; list[1] = mr;
        CHECK(modsync_upload_builds(list, 2, "item-9", NULL, &st));
    }
    use_root(r2);
    {
        sh_modmanifest_row row;
        sh_ms_place red = place_of("Red Keep", "item-9"), blue = place_of("Blue Keep", "item-10");
        char got[MAX_PATH * 2], want[MAX_PATH * 2];
        int gets, rescans = g_rescans, notes = g_notes;
        row_of(&row, "alpha001", ha2);
        CHECK(modsync_fetch_build_for(&row, &red, NULL, &st));
        CHECK(st.code == MODNET_OK && st.builds_created == 1 && !st.already_installed && !strcmp(st.folder, "maps\\Red Keep\\Alpha"));
        _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\maps\\Red Keep\\Alpha\\mod.json", r2);
        _snprintf_s(want, sizeof want, _TRUNCATE, "%s\\Alpha Pack\\mod.json", r1);
        CHECK(same_file(got, want));
        _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\maps\\Red Keep\\Alpha\\assets\\generated\\image\\mods\\alpha001\\a.bimage", r2);
        CHECK(file_has(got, "alpha bytes, republished"));
        CHECK(g_rescans == rescans + 1 && g_notes == notes + 1 && !strcmp(g_note_folder, "maps\\Red Keep\\Alpha") &&
              !strcmp(g_note_title, "Red Keep"));
        CHECK(modsync_find_installed("alpha001", ha2, &red, NULL, 0) && !modsync_find_installed("alpha001", ha2, &blue, NULL, 0));
        /* Again: present in the place; nothing fetched. */
        gets = g_stub.build_gets;
        CHECK(modsync_fetch_build_for(&row, &red, NULL, &st) && st.already_installed && g_stub.build_gets == gets);
        /* Another map naming the same build unpacks its own copy. */
        CHECK(modsync_fetch_build_for(&row, &blue, NULL, &st) && !st.already_installed && g_stub.build_gets == gets + 1);
        CHECK(!strcmp(st.folder, "maps\\Blue Keep\\Alpha"));

        /* The root at the row's hash serves it: nothing fetched. */
        _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\My Alpha", r2);
        _snprintf_s(want, sizeof want, _TRUNCATE, "%s\\Alpha Pack", r1);
        CHECK(mt_copy_tree(want, got));
        {
            sh_ms_place green = place_of("Green Keep", NULL);
            gets = g_stub.build_gets;
            CHECK(modsync_fetch_build_for(&row, &green, NULL, &st) && st.already_installed && g_stub.build_gets == gets);
            CHECK(!strcmp(st.folder, "My Alpha"));
            /* At another hash it does not: the pinned build comes into the place. */
            mt_write(got, "assets\\generated\\image\\mods\\alpha001\\a.bimage", "the player's own alpha");
            {
                sh_mb_bundle b;
                char why[SH_MB_CAUSE_MAX];
                memset(&b, 0, sizeof b);
                CHECK(sh_mb_build(got, NULL, NULL, &b) && sh_mb_write(&b, got, why, sizeof why) && strcmp(b.hash, ha2));
                sh_mb_free(&b);
            }
            CHECK(modsync_fetch_build_for(&row, &green, NULL, &st) && !st.already_installed && g_stub.build_gets == gets + 1);
            CHECK(!strcmp(st.folder, "maps\\Green Keep\\Alpha"));
        }

        /* A republish replaces the place's build of the salt: one folder per salt. */
        {
            char ha3[65], ma3[80];
            use_root(r1);
            _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\Alpha Pack", r1);
            CHECK(make_mod(dir, "alpha001", "Alpha", A_FILES, A_BYTES, NULL, ha3));
            mod_of(ma3, "alpha001", ha3);
            list[0] = ma3;
            CHECK(modsync_upload_builds(list, 1, "item-9", NULL, &st));
            use_root(r2);
            row_of(&row, "alpha001", ha3);
            CHECK(modsync_fetch_build_for(&row, &red, NULL, &st) && !strcmp(st.folder, "maps\\Red Keep\\Alpha"));
            _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\maps\\Red Keep", r2);
            CHECK(count_dirs(got) == 1);
            _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\maps\\Red Keep\\Alpha\\mod.json", r2);
            CHECK(file_has(got, ha3) && !file_has(got, ha2));
            strcpy_s(ha2, sizeof ha2, ha3);
        }

        /* ---- refusals: nothing placed ---- */
        {
            sh_ms_place grey = place_of("Grey Keep", NULL);
            char hx[65], placedir[MAX_PATH * 2];
            unsigned char *copy;
            size_t len = 0;
            _snprintf_s(placedir, sizeof placedir, _TRUNCATE, "%s\\maps\\Grey Keep", r2);
            /* An archive of another build under this hash. */
            sh_mb_sha256_hex("forged", 6, hx);
            copy = stored_copy(hr, &len);
            CHECK(copy != NULL);
            if (copy) stub_put_raw(hx, copy, len);
            free(copy);
            row_of(&row, "rocks003", hx);
            CHECK(!modsync_fetch_build_for(&row, &grey, NULL, &st));
            CHECK(st.code == MODNET_E_HASH && strstr(st.cause, "damaged on the way"));
            CHECK(count_dirs(placedir) == 0);
            /* The right build of another salt. */
            row_of(&row, "zzzzzzz9", hr);
            CHECK(!modsync_fetch_build_for(&row, &grey, NULL, &st) && st.code == MODNET_E_HASH && count_dirs(placedir) == 0);
            /* A transfer damaged on the way. */
            g_stub.corrupt_build_gets = 1;
            row_of(&row, "rocks003", hr);
            CHECK(!modsync_fetch_build_for(&row, &grey, NULL, &st) && st.code == MODNET_E_HASH && count_dirs(placedir) == 0);
            g_stub.corrupt_build_gets = 0;
            /* An entry its mod.json does not list. */
            {
                char zp[MAX_PATH * 2], mj[MAX_PATH * 2], hy[65];
                unsigned char *text;
                size_t tl = 0;
                sh_zip_writer w;
                wchar_t *wz;
                _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\gamma\\mod.json", r1);
                _snprintf_s(zp, sizeof zp, _TRUNCATE, "%s\\extra.zip", tmp);
                text = sh_mb_read_file(mj, 1 << 20, &tl);
                memset(&w, 0, sizeof w);
                wz = sh_mb_wpath(zp);
                CHECK(text && wz && sh_zip_writer_open(&w, wz) && sh_zip_writer_add(&w, "mod.json", text, tl) &&
                      sh_zip_writer_add(&w, "assets/generated/image/mods/gamma004/c.bimage", C_BYTES[0], strlen(C_BYTES[0])) &&
                      sh_zip_writer_add(&w, "assets/generated/image/mods/gamma004/extra.bimage", "x", 1) &&
                      sh_zip_writer_finish(&w));
                sh_zip_writer_free(&w);
                free(wz);
                free(text);
                strcpy_s(hy, sizeof hy, hc);
                copy = sh_mb_read_file(zp, 1 << 20, &len);
                if (copy) stub_put_raw(hy, copy, len);
                free(copy);
                row_of(&row, "gamma004", hy);
                CHECK(!modsync_fetch_build_for(&row, &grey, NULL, &st));
                CHECK(st.code == MODNET_E_HASH && strstr(st.cause, "holds files its mod.json does not list") && count_dirs(placedir) == 0);
            }
            /* A borrow this game lacks. */
            g_game_lacks = 1;
            row_of(&row, "rocks003", hr);
            CHECK(!modsync_fetch_build_for(&row, &grey, NULL, &st));
            CHECK(st.code == MODNET_E_GAME && st.game_lacks && !strcmp(st.game_source, ROCK_SOURCE) && count_dirs(placedir) == 0);
            g_game_lacks = 0;
            CHECK(modsync_fetch_build_for(&row, &grey, NULL, &st) && !strcmp(st.folder, "maps\\Grey Keep\\Rocks"));
            _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\maps\\Grey Keep\\Rocks\\assets\\generated\\image\\mods\\rocks003", r2);
            {
                char f1[MAX_PATH * 2], f2[MAX_PATH * 2];
                _snprintf_s(f1, sizeof f1, _TRUNCATE, "%s\\own.bimage", got);
                _snprintf_s(f2, sizeof f2, _TRUNCATE, "%s\\rock.bimage", got);
                CHECK(mt_exists(f1) && !mt_exists(f2));   /* the pointer stays the game's */
            }
            /* Missing from the store. */
            row_of(&row, "betab002", hb);
            CHECK(!modsync_fetch_build_for(&row, &grey, NULL, &st) && st.code == MODNET_E_NOT_FOUND);
            CHECK(!modsync_fetch_build_for(&row, NULL, NULL, &st) && st.code == MODNET_E_NOT_FOUND);
        }

        /* ---- the root (a map without a place): installed as the player's own mod, never moved ---- */
        {
            char rootdir[MAX_PATH * 2], placedir[MAX_PATH * 2], *mj;
            int gets0;
            size_t ml = 0;
            /* A map's place holds this very build: installed at the root from there, nothing fetched. */
            row_of(&row, "rocks003", hr);
            gets0 = g_stub.build_gets;
            CHECK(modsync_fetch_build_for(&row, NULL, NULL, &st) && !st.already_installed && g_stub.build_gets == gets0);
            CHECK(!strcmp(st.folder, "Rocks"));
            _snprintf_s(rootdir, sizeof rootdir, _TRUNCATE, "%s\\Rocks", r2);
            _snprintf_s(placedir, sizeof placedir, _TRUNCATE, "%s\\maps\\Grey Keep\\Rocks\\mod.json", r2);
            CHECK(mt_exists(placedir));   /* the place keeps its copy */
            _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\mod.json", rootdir);
            mj = (char *)sh_mb_read_file(got, 1 << 20, &ml);
            CHECK(mj && strstr(mj, "rocks003") && strstr(mj, hr));
            free(mj);
            /* Again: the root serves it. */
            CHECK(modsync_fetch_build_for(&row, NULL, NULL, &st) && st.already_installed && !strcmp(st.folder, "Rocks"));
            /* Nowhere on this machine: downloaded and installed at the root directly (no place is made). */
            mt_remove_tree(rootdir);
            _snprintf_s(placedir, sizeof placedir, _TRUNCATE, "%s\\maps\\Grey Keep", r2);
            mt_remove_tree(placedir);
            _snprintf_s(placedir, sizeof placedir, _TRUNCATE, "%s\\maps", r2);
            {
                int places = count_dirs(placedir);
                gets0 = g_stub.build_gets;
                CHECK(modsync_fetch_build_for(&row, NULL, NULL, &st) && !st.already_installed && g_stub.build_gets == gets0 + 1);
                CHECK(!strcmp(st.folder, "Rocks") && count_dirs(placedir) == places);
            }
            _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\mod.json", rootdir);
            mj = (char *)sh_mb_read_file(got, 1 << 20, &ml);
            CHECK(mj && strstr(mj, "rocks003") && strstr(mj, hr));
            free(mj);
            /* The root holds the salt at another build: the player's own folder stays theirs. */
            row_of(&row, "alpha001", ha2);
            CHECK(!modsync_fetch_build_for(&row, NULL, NULL, &st) && st.code == MODNET_E_REFUSED &&
                  strstr(st.cause, "your mods already hold"));
        }

        /* ---- an editor-tweaks build a map names: installed like any other ---- */
        {
            sh_ms_place ed = place_of("Editor Map", NULL);
            row_of(&row, "edito005", he);
            CHECK(modsync_fetch_build_for(&row, &ed, NULL, &st) && st.builds_created == 1);
            _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\maps\\Editor Map", r2);
            CHECK(count_dirs(got) == 1);
            _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\snapmap-plus.json", r2);
            CHECK(!file_has(got, "\"editorOnly\""));
        }
    }

    /* No secret reaches the log. */
    {
        char secret[65];
        use_root(r1);
        CHECK(modsync_claim_lookup("item-9", NULL, secret) && !strstr(g_log, secret));
    }
    (void)cause;
    (void)r;
    modnet_shutdown();
    stub_stop();
    mt_remove_tree(tmp);
    printf("modsync_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
