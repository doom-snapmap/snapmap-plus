/* modexport_test -- rawmap and prefab exports in the one shape (ONE-SHAPE
 * rules 4 and 6).
 *
 * Two mods built at the root of a store (one in an organizing folder) and a
 * map whose rows name them as "<salt>@<build hash>" are written to one ZIP:
 * <zip name>.json and mods/<folder name>/mod.json and assets/ for each, the
 * mod.json and carried files byte for byte, the same bytes on a second write.
 * Imported into an empty store, each carried folder lands in the ZIP's place,
 * mods\maps\<zip name>\<mod name>\, the serving index holds both and the map
 * text comes back recording that place (DECISIONS 11), while what the loaded
 * map serves is unchanged; a second import installs nothing; a store whose
 * root holds a salt at the row's build installs only the other one; an
 * import that would replace another build of a salt in the place takes a
 * place of its own and leaves the earlier one as it was. A damaged entry,
 * and a folder whose files do not build to the hash its mod.json records,
 * refuse the import, leave nothing installed and change nothing. A map naming a build the export does not carry counts
 * it as absent. A row pinning another build of a root salt is exported as the
 * root folder's build, its row moved. Every earlier shape is refused with one
 * sentence: a bundle.canonical.json, an export.json index, a carried
 * mod.json with a format-1 block, a map with old rows. A prefab's export has
 * the same layout; its mods install at the root (DECISIONS 12); each
 * importer refuses the other's ZIP. The rawmap hooks return process-heap
 * text. Pure file work: no network. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modexport.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modzip.h"
#include "../src/backend/map_carrier.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static int g_echo;
void backend_log(const char *line)
{
    if (g_echo && line) printf("    %s\n", line);
}

/* The map manifest links the serving layer's activation (modserve_engine.c). */
int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    (void)bundle_hashes;
    (void)n;
    return 0;
}

static const char BASE_MAP[] = "{\"~type\":\"idSnapMap\",\"entities\":[],\"name\":\"EXPORT_MARK\"}";

/* A new mod by hand: an author part only, files under mods/<leaf>/. */
static void make_mod(const char *dir, const char *name, const char *leaf, const char *extra)
{
    char text[512], rel[MAX_PATH];
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"name\":\"%s\",\"version\":\"1.0.0\"}", name);
    mt_write(dir, "mod.json", text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\a.bimage", leaf);
    _snprintf_s(text, sizeof text, _TRUNCATE, "image bytes of %s%s", name, extra ? extra : "");
    mt_write(dir, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\b.bimage", leaf);
    mt_write(dir, rel, "second image");
}

/* Install `src` into the current store root; its row (salt, build, name). */
static int install(const char *src, char *name, size_t cap, sh_modmanifest_row *row)
{
    sh_ms_install_stats st;
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX] = "";
    int ok;
    memset(&b, 0, sizeof b);
    ok = sh_ms_install_from_folder(src, NULL, name, cap, &st, &b, cause, sizeof cause);
    if (!ok) fprintf(stderr, "install %s: %s\n", src, cause);
    memset(row, 0, sizeof *row);
    if (ok) {
        strcpy_s(row->salt, sizeof row->salt, b.id);
        strcpy_s(row->build, sizeof row->build, b.hash);
        if (b.name) strncpy_s(row->name, sizeof row->name, b.name, _TRUNCATE);
        row->size = b.total_size;
    }
    sh_mb_free(&b);
    return ok;
}

/* BASE_MAP with `n` rows (malloc'd, NUL-terminated). */
static char *map_with(const sh_modmanifest_row *rows, size_t n)
{
    sh_modmanifest_list l;
    char err[160] = "", *heap, *out = NULL;
    size_t len = 0, i;
    memset(&l, 0, sizeof l);
    for (i = 0; i < n; i++) sh_modmanifest_list_add(&l, &rows[i]);
    heap = sh_modmanifest_write(BASE_MAP, strlen(BASE_MAP), &l, &len, err, sizeof err);
    sh_modmanifest_list_free(&l);
    if (!heap) { fprintf(stderr, "map_with: %s\n", err); return NULL; }
    out = (char *)malloc(len + 1);
    if (out) { memcpy(out, heap, len); out[len] = 0; }
    HeapFree(GetProcessHeap(), 0, heap);
    return out;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    return sh_mb_read_file(path, 64u << 20, len);
}

static int spit(const char *path, const void *data, size_t len)
{
    FILE *f = NULL;
    int ok;
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (fopen_s(&f, path, "wb") || !f) return 0;
    ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

/* The ZIP's entry names, one per line. */
static void zip_names(const char *zip, char *out, size_t cap)
{
    sh_zip_reader r;
    char cause[256];
    size_t i, k;
    out[0] = 0;
    if (!sh_zip_reader_open(&r, zip, cause, sizeof cause)) return;
    for (i = 0; i < r.n; i++) {
        k = r.sorted ? r.sorted[i] : i;
        strncat_s(out, cap, r.e[k].name, _TRUNCATE);
        strncat_s(out, cap, "\n", _TRUNCATE);
    }
    sh_zip_reader_close(&r);
}

/* 1 when the ZIP's entry `name` holds exactly the bytes of the file at path. */
static int entry_is_file(const char *zip, const char *name, const char *path)
{
    sh_zip_reader r;
    char cause[256];
    unsigned char *a = NULL, *b;
    size_t al = 0, bl = 0;
    long at;
    int same = 0;
    if (!sh_zip_reader_open(&r, zip, cause, sizeof cause)) return 0;
    at = sh_zip_reader_find(&r, name);
    if (at >= 0) a = sh_zip_reader_read(&r, at, 64u << 20, &al, cause, sizeof cause);
    b = slurp(path, &bl);
    same = a && b && al == bl && !memcmp(a, b, al);
    free(a);
    free(b);
    sh_zip_reader_close(&r);
    return same;
}

/* Replace text of equal length inside a stored entry and fix its CRC in the
 * local header and the central directory, so only the content is wrong. */
static int patch_entry(unsigned char *zip, size_t len, const char *name, const char *from, const char *to)
{
    size_t nl = strlen(name), at = 0, size, i;
    unsigned long crc;
    unsigned char *hit = NULL;
    for (i = 0; i + 30 + nl <= len && !at; i++)
        if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 3 && zip[i + 3] == 4 &&
            (size_t)(zip[i + 26] | (zip[i + 27] << 8)) == nl && memcmp(zip + i + 30, name, nl) == 0)
            at = i + 30 + nl + (size_t)(zip[i + 28] | (zip[i + 29] << 8));
    if (!at || strlen(from) != strlen(to)) return 0;
    {
        const unsigned char *h = zip + at - nl - 30;
        size = (size_t)h[18] | ((size_t)h[19] << 8) | ((size_t)h[20] << 16) | ((size_t)h[21] << 24);
    }
    for (i = at; i + strlen(from) <= at + size && !hit; i++) if (memcmp(zip + i, from, strlen(from)) == 0) hit = zip + i;
    if (!hit) return 0;
    memcpy(hit, to, strlen(to));
    crc = mt_crc32(zip + at, size);
    mt_put32(zip + at - nl - 30 + 14, crc);
    for (i = 0; i + 46 + nl <= len; i++)
        if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 1 && zip[i + 3] == 2 &&
            (size_t)(zip[i + 28] | (zip[i + 29] << 8)) == nl && memcmp(zip + i + 46, name, nl) == 0) {
            mt_put32(zip + i + 16, crc);
            return 1;
        }
    return 0;
}

static size_t count_entries(void)
{
    sh_ms_list l;
    size_t n = 0;
    if (sh_ms_enumerate(&l)) { n = l.count; sh_ms_list_free(&l); }
    return n;
}

/* The store entry named `name` (below mods\) is usable at `hash`. */
static int entry_at(const char *name, const char *hash)
{
    sh_ms_list l;
    size_t i;
    int ok = 0;
    if (!sh_ms_enumerate(&l)) return 0;
    for (i = 0; i < l.count && !ok; i++)
        ok = !_stricmp(l.items[i].name, name) && sh_ms_entry_usable(&l.items[i]) && !strcmp(l.items[i].bundle.hash, hash);
    sh_ms_list_free(&l);
    return ok;
}

/* A fresh store at `root`, its serving index built, the loaded map one
 * without a place. */
static void store_at(const char *root)
{
    CreateDirectoryA(root, NULL);
    sh_ms_set_root(root);
    (void)sh_modserve_index_build(root);
    (void)sh_modserve_place_set(NULL);
}

/* `text` is `map` with the member's place set to `segment` (NULL: none). */
static int recorded_as(const char *text, size_t len, const char *map, const char *segment)
{
    char place[SH_CARRIER_PLACE_CAP], err[96];
    size_t n = 0;
    char *bare;
    int got = sh_carrier_place(text, len, place, sizeof place), ok;
    if (segment ? got != 1 || strcmp(place, segment) : got != 0) return 0;
    if (!segment) return len == strlen(map) && !memcmp(text, map, len);
    bare = sh_carrier_put_place(text, len, NULL, &n, err, sizeof err);
    ok = bare && n == strlen(map) && !memcmp(bare, map, n);
    if (bare) HeapFree(GetProcessHeap(), 0, bare);
    return ok;
}

/* The loaded map's place is still none. */
static int loaded_place_none(void)
{
    char now[SH_MODSERVE_PLACE_CAP];
    sh_modserve_place_get(now, sizeof now);
    return !now[0];
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], a_root[MAX_PATH], b_root[MAX_PATH], c_root[MAX_PATH], src_pink[MAX_PATH], src_room[MAX_PATH];
    char zip[MAX_PATH], zip2[MAX_PATH], path[MAX_PATH * 2], names[4096], cause[MODEXPORT_CAUSE_MAX], name[SH_MS_NAME_MAX + 1];
    sh_modmanifest_row rows[3];
    modexport_stats st;
    unsigned char *z1 = NULL, *z2 = NULL;
    size_t z1l = 0, z2l = 0, len = 0;
    char *map = NULL, *back;
    (void)argc; (void)argv;
    g_echo = getenv("MODEXPORT_TEST_LOG") != NULL;
    CHECK(mt_temp_dir("modexport", tmp, sizeof tmp));
    _snprintf_s(a_root, sizeof a_root, _TRUNCATE, "%s\\a", tmp);
    _snprintf_s(b_root, sizeof b_root, _TRUNCATE, "%s\\b", tmp);
    _snprintf_s(c_root, sizeof c_root, _TRUNCATE, "%s\\c", tmp);
    _snprintf_s(src_pink, sizeof src_pink, _TRUNCATE, "%s\\src\\pink", tmp);
    _snprintf_s(src_room, sizeof src_room, _TRUNCATE, "%s\\src\\room", tmp);
    _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\Pink Arena.zip", tmp);
    _snprintf_s(zip2, sizeof zip2, _TRUNCATE, "%s\\again.zip", tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\src", tmp);
    CreateDirectoryA(path, NULL);
    make_mod(src_pink, "Pink Imp", "pink", NULL);
    make_mod(src_room, "Bob Room", "room", NULL);

    printf("-- export: each row's mod as its folder, byte for byte, the same bytes twice\n");
    store_at(a_root);
    CHECK(install(src_pink, name, sizeof name, &rows[0]) && !strcmp(name, "pink"));
    /* A mod in an organizing folder is carried under its own folder name. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\group", a_root);
    CreateDirectoryA(path, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\group\\room", a_root);
    CHECK(mt_copy_tree(src_room, path));
    {
        sh_ms_sync_stats ss;
        sh_ms_list l;
        const sh_ms_entry *e;
        CHECK(sh_ms_sync(NULL, &ss) && ss.salted == 1);
        CHECK(sh_ms_enumerate(&l));
        e = NULL;
        for (size_t i = 0; i < l.count; i++) if (!strcmp(l.items[i].name, "group\\room")) e = &l.items[i];
        CHECK(e && sh_ms_entry_usable(e));
        memset(&rows[1], 0, sizeof rows[1]);
        if (e) {
            strcpy_s(rows[1].salt, sizeof rows[1].salt, e->salt);
            strcpy_s(rows[1].build, sizeof rows[1].build, e->bundle.hash);
            strcpy_s(rows[1].name, sizeof rows[1].name, "Bob Room");
            rows[1].size = e->bundle.total_size;
        }
        sh_ms_list_free(&l);
    }
    (void)sh_modserve_index_build(a_root);
    map = map_with(rows, 2);
    CHECK(map != NULL);
    CHECK(map && modexport_write(map, strlen(map), zip, &st, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  export: %s\n", cause);
    CHECK(st.bundles == 2 && st.files == 4 && st.entries == 7);
    zip_names(zip, names, sizeof names);
    CHECK(strstr(names, "Pink Arena.json\n") && strstr(names, "mods/pink/mod.json\n") &&
          strstr(names, "mods/room/mod.json\n") && !strstr(names, "group/") && !strstr(names, "bundle"));
    {
        char want[MAX_PATH * 2], entry[300];
        _snprintf_s(want, sizeof want, _TRUNCATE, "%s\\pink\\mod.json", a_root);
        CHECK(entry_is_file(zip, "mods/pink/mod.json", want));
        _snprintf_s(want, sizeof want, _TRUNCATE, "%s\\group\\room\\assets\\generated\\image\\mods\\%s\\a.bimage", a_root,
                    rows[1].salt);
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "mods/room/assets/generated/image/mods/%s/a.bimage", rows[1].salt);
        CHECK(entry_is_file(zip, entry, want));
    }
    z1 = slurp(zip, &z1l);
    CHECK(map && modexport_write(map, strlen(map), zip, &st, cause, sizeof cause));
    z2 = slurp(zip, &z2l);
    CHECK(z1 && z2 && z1l == z2l && !memcmp(z1, z2, z1l));
    free(z2); z2 = NULL;

    printf("-- import into an empty store: each folder in the ZIP's place, the map records it\n");
    store_at(b_root);
    CHECK(modexport_probe(zip, cause, sizeof cause));
    back = modexport_read(zip, &len, &st, cause, sizeof cause);
    if (!back) fprintf(stderr, "  import: %s\n", cause);
    CHECK(back && recorded_as(back, len, map, "Pink Arena"));
    free(back);
    CHECK(st.bundles == 2 && st.bundles_installed == 2 && st.bundles_present == 0 && st.bundles_absent == 0);
    CHECK(entry_at("maps\\Pink Arena\\Pink Imp", rows[0].build) && entry_at("maps\\Pink Arena\\Bob Room", rows[1].build));
    CHECK(count_entries() == 2);
    {
        char dir[MAX_PATH], key[MAX_PATH + 8];
        _snprintf_s(key, sizeof key, _TRUNCATE, "zip:%s", zip);
        CHECK(sh_ms_place_of(key, dir, sizeof dir, NULL, 0) && !strcmp(dir, "maps\\Pink Arena"));
    }
    /* The import changed nothing the loaded map serves; the map opened from
     * the text is served from its place. */
    CHECK(loaded_place_none());
    CHECK(sh_modserve_row_served(&rows[0], NULL, 0) != SH_MODSERVE_ROW_SERVED);
    CHECK(sh_modserve_row_served_in(&rows[0], "Pink Arena", NULL, NULL, 0) == SH_MODSERVE_ROW_SERVED);

    printf("-- a second import installs nothing\n");
    back = modexport_read(zip, &len, &st, cause, sizeof cause);
    CHECK(back && st.bundles_installed == 0 && st.bundles_present == 2 && count_entries() == 2);
    free(back);

    printf("-- the player's own root folder of a salt serves it: only the other is installed\n");
    /* The exporter's own pink folder at this store's root: the same salt. */
    store_at(c_root);
    {
        char from[MAX_PATH * 2], to[MAX_PATH * 2];
        _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\pink", a_root);
        _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\pink", c_root);
        CHECK(mt_copy_tree(from, to));
        (void)sh_modserve_index_build(c_root);
    }
    back = modexport_read(zip, &len, &st, cause, sizeof cause);
    CHECK(back && st.bundles_installed == 1 && st.bundles_present == 1);
    CHECK(entry_at("pink", rows[0].build) && entry_at("maps\\Pink Arena\\Bob Room", rows[1].build));
    CHECK(!entry_at("maps\\Pink Arena\\Pink Imp", rows[0].build));
    free(back);

    printf("-- a damaged entry, or a folder that does not build to its hash, installs nothing\n");
    {
        char d[MAX_PATH], entry[300];
        unsigned char *bad = (unsigned char *)malloc(z1l);
        _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\d", tmp);
        store_at(d);
        memcpy(bad, z1, z1l);
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "mods/room/assets/generated/image/mods/%s/b.bimage", rows[1].salt);
        CHECK(patch_entry(bad, z1l, entry, "second", "SECOND"));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bad.zip", tmp);
        CHECK(spit(path, bad, z1l));
        back = modexport_read(path, &len, &st, cause, sizeof cause);
        CHECK(!back && strstr(cause, "Bob Room"));
        if (g_echo) printf("    damaged: %s\n", cause);
        CHECK(count_entries() == 0 && st.bundles_installed == 0 && loaded_place_none());
        free(back);
        free(bad);
    }

    printf("-- a row the export does not carry is counted as absent\n");
    {
        char e[MAX_PATH], *m3;
        sh_modmanifest_row three[3];
        _snprintf_s(e, sizeof e, _TRUNCATE, "%s\\e", tmp);
        three[0] = rows[0];
        three[1] = rows[1];
        memset(&three[2], 0, sizeof three[2]);
        strcpy_s(three[2].salt, sizeof three[2].salt, "ghostmod");
        memset(three[2].build, '9', 64);
        three[2].build[64] = 0;
        strcpy_s(three[2].name, sizeof three[2].name, "Ghost");
        three[2].size = 1;
        m3 = map_with(three, 3);
        /* Written by hand: the export itself refuses a row it cannot carry. */
        store_at(a_root);
        CHECK(m3 && !modexport_write(m3, strlen(m3), zip2, &st, cause, sizeof cause) &&
              strstr(cause, "needs Ghost (ghostmod@9999999999999999), which is not installed"));
        {
            sh_zip_reader r;
            const char *nm[16];
            const char *data[16];
            unsigned char *bufs[16];
            size_t k, n = 0, bl;
            char err[256];
            CHECK(sh_zip_reader_open(&r, zip, err, sizeof err));
            for (k = 0; k < r.n && n < 15; k++) {
                if (!strcmp(r.e[k].name, "Pink Arena.json")) continue;
                bufs[n] = sh_zip_reader_read(&r, (long)k, 1u << 20, &bl, err, sizeof err);
                nm[n] = r.e[k].name;
                data[n] = (const char *)bufs[n];
                n++;
            }
            nm[n] = "Ghost Arena.json";
            data[n] = m3;
            n++;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Ghost Arena.zip", tmp);
            CHECK(mt_zip_stored(path, (int)n, nm, data));
            for (k = 0; k + 1 < n; k++) free(bufs[k]);
            sh_zip_reader_close(&r);
        }
        store_at(e);
        back = modexport_read(path, &len, &st, cause, sizeof cause);
        if (!back) fprintf(stderr, "  absent: %s\n", cause);
        CHECK(back && st.bundles_installed == 2 && st.bundles_absent == 1);
        free(back);
        free(m3);
    }

    printf("-- a row pinning another build of a root salt exports the root folder's build\n");
    {
        sh_modmanifest_row moved[1];
        sh_modmanifest_list got;
        unsigned rej = 0;
        char *m1;
        store_at(a_root);
        moved[0] = rows[0];
        memset(moved[0].build, 'c', 64);
        m1 = map_with(moved, 1);
        CHECK(m1 && modexport_write(m1, strlen(m1), zip2, &st, cause, sizeof cause));
        {
            sh_zip_reader r;
            char err[256];
            unsigned char *t = NULL;
            size_t tl = 0;
            long at;
            CHECK(sh_zip_reader_open(&r, zip2, err, sizeof err));
            at = sh_zip_reader_find(&r, "again.json");
            if (at >= 0) t = sh_zip_reader_read(&r, at, 1u << 20, &tl, err, sizeof err);
            memset(&got, 0, sizeof got);
            CHECK(t && sh_modmanifest_read((const char *)t, tl, &got, &rej) == 1 &&
                  !strcmp(got.rows[0].build, rows[0].build) && !strcmp(got.rows[0].salt, rows[0].salt));
            sh_modmanifest_list_free(&got);
            free(t);
            sh_zip_reader_close(&r);
        }
        free(m1);
    }

    printf("-- every earlier shape is refused with one sentence and installs nothing\n");
    {
        char f[MAX_PATH], old_modjson[512];
        const char *nm[3], *data[3];
        store_at(c_root);
        _snprintf_s(f, sizeof f, _TRUNCATE, "%s\\old1.zip", tmp);
        nm[0] = "old1.json"; data[0] = BASE_MAP;
        nm[1] = "mods/x/bundle.canonical.json"; data[1] = "{}";
        CHECK(mt_zip_stored(f, 2, nm, data));
        CHECK(!modexport_read(f, &len, &st, cause, sizeof cause) && !strcmp(cause, MODEXPORT_EARLIER));
        CHECK(!modexport_probe(f, cause, sizeof cause) && !strcmp(cause, MODEXPORT_EARLIER));
        nm[1] = "export.json"; data[1] = "{\"format\":1,\"bundles\":[]}";
        CHECK(mt_zip_stored(f, 2, nm, data));
        CHECK(!modexport_read(f, &len, &st, cause, sizeof cause) && !strcmp(cause, MODEXPORT_EARLIER));
        /* A carried mod.json in an earlier shape (a format-1 block). */
        _snprintf_s(old_modjson, sizeof old_modjson, _TRUNCATE,
                    "{\"id\":\"alex.pink-imp\",\"name\":\"Pink\",\"snapmap-plus\":{\"format\":1,\"bundle\":\"%s\",\"files\":[]}}",
                    rows[0].build);
        nm[0] = "old1.json"; data[0] = map;
        nm[1] = "mods/pink/mod.json"; data[1] = old_modjson;
        CHECK(mt_zip_stored(f, 2, nm, data));
        CHECK(!modexport_read(f, &len, &st, cause, sizeof cause) && !strcmp(cause, MODEXPORT_EARLIER));
        /* A map whose rows name "id" and "bundle". */
        {
            char oldmap[600];
            _snprintf_s(oldmap, sizeof oldmap, _TRUNCATE,
                        "{\"~type\":\"idSnapMap\",\"entities\":[],\"snapmap-plus\":{\"format\":1,\"mods\":["
                        "{\"id\":\"alex.pink-imp\",\"version\":\"1\",\"bundle\":\"%s\",\"size\":1}]}}", rows[0].build);
            nm[0] = "old1.json"; data[0] = oldmap;
            CHECK(mt_zip_stored(f, 1, nm, data));
            CHECK(!modexport_read(f, &len, &st, cause, sizeof cause) && !strcmp(cause, MODEXPORT_EARLIER));
        }
        CHECK(count_entries() == 2);   /* c's own two, nothing added */
    }

    printf("-- a prefab's export: the same layout, its mods installed at the root\n");
    {
        sh_modmanifest_list pr;
        char pz[MAX_PATH], g[MAX_PATH], *text = NULL, pname[128];
        size_t tl = 0;
        static const char PREFAB[] = "{\"~type\":\"idSnapEntityPrefab\",\"entities\":[]}";
        memset(&pr, 0, sizeof pr);
        sh_modmanifest_list_add(&pr, &rows[0]);
        store_at(a_root);
        _snprintf_s(pz, sizeof pz, _TRUNCATE, "%s\\door kit export.zip", tmp);
        CHECK(modexport_write_prefab("Door Kit", PREFAB, strlen(PREFAB), &pr, NULL, NULL, pz, &st, cause, sizeof cause));
        zip_names(pz, names, sizeof names);
        CHECK(strstr(names, "Door Kit.json\n") && strstr(names, "mods/pink/mod.json\n") && !strstr(names, "room"));
        _snprintf_s(g, sizeof g, _TRUNCATE, "%s\\g", tmp);
        store_at(g);
        /* The open map has a place; a prefab import never changes it. */
        CHECK(sh_modserve_place_set("maps\\Open map") >= 0);
        CHECK(modexport_read_prefab(pz, NULL, NULL, &text, &tl, pname, sizeof pname, &st, cause, sizeof cause));
        {
            char now[SH_MODSERVE_PLACE_CAP];
            sh_modserve_place_get(now, sizeof now);
            CHECK(!strcmp(now, "maps\\Open map"));
            CHECK(sh_modserve_place_set(NULL) >= 0);
        }
        CHECK(text && !strcmp(pname, "Door Kit") && st.bundles_installed == 1);
        CHECK(!entry_at("maps\\Door Kit\\Pink Imp", rows[0].build) && count_entries() == 1);
        {
            sh_modserve_bundle_ref ref;
            (void)sh_modserve_index_rescan();
            CHECK(sh_modserve_row_build(&rows[0], &ref) && !ref.place[0] && !strcmp(ref.hash, rows[0].build));
        }
        free(text);
        /* Each importer refuses the other's export. */
        CHECK(!modexport_read(pz, &len, &st, cause, sizeof cause) && strstr(cause, "prefab's export, not a map's"));
        CHECK(!modexport_read_prefab(zip, NULL, NULL, &text, &tl, pname, sizeof pname, &st, cause, sizeof cause) &&
              strstr(cause, "map's export, not a prefab's"));
        sh_modmanifest_list_free(&pr);
    }

    printf("-- the rawmap hooks: process-heap text recording the ZIP's place\n");
    {
        char h[MAX_PATH], acp[MAX_PATH * 2];
        unsigned bundles = 0;
        char *heap;
        _snprintf_s(h, sizeof h, _TRUNCATE, "%s\\h", tmp);
        store_at(a_root);
        _snprintf_s(acp, sizeof acp, _TRUNCATE, "%s\\hooked.zip", tmp);
        CHECK(map && modexport_write_text_report(map, strlen(map), acp, &bundles, cause, sizeof cause) > 0 && bundles == 2);
        store_at(h);
        heap = modexport_read_text_report(acp, &len, cause, sizeof cause);
        CHECK(heap && recorded_as(heap, len, map, "hooked"));
        if (heap) HeapFree(GetProcessHeap(), 0, heap);
        CHECK(entry_at("maps\\hooked\\Pink Imp", rows[0].build));
    }

    printf("-- an import that would replace another build in its place takes a place of its own\n");
    {
        char pinkasset[MAX_PATH * 2], dir[MAX_PATH], key[MAX_PATH + 8], *m1, segment[SH_CARRIER_PLACE_CAP];
        sh_ms_sync_stats ss;
        sh_modmanifest_list got;
        unsigned rej = 0;
        /* The author changes Pink Imp and exports the same ZIP again. */
        store_at(a_root);
        _snprintf_s(pinkasset, sizeof pinkasset, _TRUNCATE, "%s\\pink\\assets\\generated\\image\\mods\\%s\\a.bimage",
                    a_root, rows[0].salt);
        CHECK(spit(pinkasset, "changed", 7));
        CHECK(sh_ms_sync(NULL, &ss));
        (void)sh_modserve_index_build(a_root);
        m1 = map_with(rows, 1);
        CHECK(m1 && modexport_write(m1, strlen(m1), zip, &st, cause, sizeof cause));
        /* b's place holds the earlier build: the import goes beside it. */
        store_at(b_root);
        back = modexport_read(zip, &len, &st, cause, sizeof cause);
        if (!back) fprintf(stderr, "  beside: %s\n", cause);
        CHECK(back && st.bundles_installed == 1);
        CHECK(entry_at("maps\\Pink Arena\\Pink Imp", rows[0].build));   /* the earlier import's, kept */
        CHECK(back && sh_carrier_place(back, len, segment, sizeof segment) == 1 && strcmp(segment, "Pink Arena") != 0);
        memset(&got, 0, sizeof got);
        CHECK(back && sh_modmanifest_read(back, len, &got, &rej) == 1);
        if (got.count)
            CHECK(sh_modserve_row_served_in(&got.rows[0], segment, NULL, NULL, 0) == SH_MODSERVE_ROW_SERVED &&
                  strcmp(got.rows[0].build, rows[0].build) != 0);
        sh_modmanifest_list_free(&got);
        _snprintf_s(key, sizeof key, _TRUNCATE, "zip:%s", zip);
        CHECK(sh_ms_place_of(key, dir, sizeof dir, NULL, 0) && !strcmp(dir, "maps\\Pink Arena"));
        CHECK(loaded_place_none());
        free(back);
        free(m1);
    }

    free(z1);
    free(map);
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    printf("modexport_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
