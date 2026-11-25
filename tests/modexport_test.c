/* modexport_test -- rawmap and prefab exports in the unit layout (SPEC-distribution 5.16, D35).
 *
 * An author's mods are two folders built at the root of a mods folder (Crates: three props, two of them placed;
 * Paints: one prop placed) and a map whose two rows name them as local rows. The export pins each local row with
 * modpin (each placed object's unit, written into the content cache) and writes one ZIP:
 *   <zip name>.json      the map text, its rows pinned (format 2: "objects" naming units)
 *   units/<hash>.json    each unit's canonical manifest, its SHA-256 the name
 *   files/<sha256>       each carried file once
 * with no mods/ entry and no index, the same bytes on a second write. A row no folder here serves refuses the
 * export with the mod's name, and so does a unit whose manifest, or one of whose files, is not on this computer.
 *
 * Imported into a player's empty mods folder, every unit and file goes into the content cache (each checked against
 * its name), the map text comes back unchanged and nothing is installed: no mod folder appears. A second import
 * finds every unit present. A damaged entry is refused with the export is damaged sentence and its content is not
 * kept; a row whose content the export does not carry is counted as absent. Every earlier shape (whole mod folders
 * under mods/, bundle.canonical.json, an export.json index, a map in an earlier shape) is refused with one sentence
 * and changes nothing. A prefab's export has the same layout, and each importer refuses the other's ZIP. The rawmap
 * hooks return process-heap text. Pure file work: no network. */
#include "cache_paths.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modexport.h"
#include "../src/backend/modbundle.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modcache.h"
#include "../src/backend/modunit.h"
#include "../src/backend/modpin.h"
#include "../src/backend/modzip.h"
#include "../src/backend/prefab_block.h"
#include "../src/backend/mp_json.h"
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
/* The player's setting "Play maps whose mods include scripts" (modui.c): on. */
int sh_modui_scripted_maps_allowed(void) { return 1; }

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

/* ------------------------------------------------------------ the fixture world */
#define SA "pa1salt1"   /* Crates */
#define SB "pb2salt2"   /* Paints */
#define SG "pg9ghost"   /* a mod no folder here serves */

static int stock_exists(void *ctx, const char *request)
{
    (void)ctx;
    return strstr(request, "mods/") ? 0 : 1;   /* every name outside a mod is DOOM's */
}
static sh_mb_resolver g_resolver = { NULL, NULL, stock_exists, NULL };
static int fixture_game(void *ctx, const char *request, char sha[SH_MB_HASH_HEX + 1], char *source, size_t cap)
{
    (void)ctx;
    if (strstr(request, "mods/")) return 0;
    memset(sha, 'a', SH_MB_HASH_HEX);
    sha[SH_MB_HASH_HEX] = 0;
    strcpy_s(source, cap, "snap_gameresources");
    return 1;
}
static char *no_read(void *ctx, const char *request, size_t *len) { (void)ctx; (void)request; (void)len; return NULL; }

/* The folders that serve a local row on this computer, by salt (the product asks the serving index). */
static struct { const char *salt; char folder[MAX_PATH * 2]; } g_serve[4];
static int g_nserve;
static char *serve_folder(void *ctx, const sh_modmanifest_row *row)
{
    int i;
    (void)ctx;
    for (i = 0; i < g_nserve; i++) if (!_stricmp(g_serve[i].salt, row->salt)) return _strdup(g_serve[i].folder);
    return NULL;
}

/* Use `root` as the mods folder: its .content is the cache and its folders are the local mods. */
static void use_root(const char *root)
{
    CreateDirectoryA(root, NULL);
    sh_cache_set_mods_root(root);
    sh_cache_local_changed();
}

static void put_bin(const char *folder, const char *rel, const char *spell, unsigned seed)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    int i;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") || !f) return;
    for (i = 0; i < 300; i++) fputc((i * 7 + (int)seed) % 251, f);
    for (i = 0; i < 32; i++) fputc(0, f);
    fclose(f);
}

/* A crate prop of the mod with `salt`: entity, model, material, image (four files). */
static void crate(const char *folder, const char *salt, const char *n, unsigned seed)
{
    char rel[MAX_PATH], text[1024], spell[160];
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "entities\\%s.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t\trenderModelInfo = {\n"
                "\t\t\tmodel = \"mods/%s/models/%s.lwo\";\n\t\t}\n\t}\n}\n", salt, n);
    mt_write(folder, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "meshes\\models\\%s.bmodel", n);
    _snprintf_s(spell, sizeof spell, _TRUNCATE, "mods/%s/%s_skin", salt, n);
    put_bin(folder, rel, spell, seed);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "materials\\%s_skin.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\ndiffusemap\t\"mods/%s/%s_d.tga\"\n}\n", salt, n);
    mt_write(folder, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "images\\%s_d.bimage", n);
    put_bin(folder, rel, NULL, seed + 3);
}

/* Build the mod in `folder` as the store does (its bundle.json written). */
static int build_mod(const char *folder, const char *salt)
{
    sh_mb_bundle b;
    char *cause = NULL;
    int ok = sh_mb_build(folder, salt, &g_resolver, &b) && !b.errors && sh_mb_write(&b, folder, &cause);
    if (!ok) fprintf(stderr, "build %s: %s\n", folder, cause ? cause : sh_mb_first_cause(&b));
    free(cause);
    sh_mb_free(&b);
    return ok;
}

/* The author's mods folder: Crates (crate, barrel, lamp) and Paints (red). */
static int make_world(const char *root)
{
    char crates[MAX_PATH * 2], paints[MAX_PATH * 2];
    CreateDirectoryA(root, NULL);
    _snprintf_s(crates, sizeof crates, _TRUNCATE, "%s\\crates", root);
    _snprintf_s(paints, sizeof paints, _TRUNCATE, "%s\\paints", root);
    mt_write(crates, "mod.json", "{\"name\":\"Crates\"}");
    mt_write(paints, "mod.json", "{\"name\":\"Paints\"}");
    crate(crates, SA, "crate", 1);
    crate(crates, SA, "barrel", 9);
    crate(crates, SA, "lamp", 17);
    crate(paints, SB, "red", 4);
    g_serve[0].salt = SA;
    strcpy_s(g_serve[0].folder, sizeof g_serve[0].folder, crates);
    g_serve[1].salt = SB;
    strcpy_s(g_serve[1].folder, sizeof g_serve[1].folder, paints);
    g_nserve = 2;
    return build_mod(crates, SA) && build_mod(paints, SB);
}

static void local_row(sh_modmanifest_row *row, const char *salt, const char *name, const char *const *placed, int n)
{
    int i;
    memset(row, 0, sizeof *row);
    strcpy_s(row->salt, sizeof row->salt, salt);
    strcpy_s(row->build, sizeof row->build, SH_MODMANIFEST_LOCAL_BUILD);
    sh_modmanifest_row_set_name(row, name);
    for (i = 0; i < n; i++) sh_modmanifest_row_add_name(row, placed[i]);
}

static const char BASE_MAP[] =
    "{\"~type\":\"idSnapMap\",\"entities\":[{\"inherit\":\"mods/" SA "/crate\"},{\"inherit\":\"mods/" SA "/barrel\"},"
    "{\"inherit\":\"mods/" SB "/red\"}],\"name\":\"EXPORT_MARK\"}";

/* BASE_MAP with `n` rows (malloc'd, NUL-terminated). */
static char *map_with(const sh_modmanifest_row *rows, size_t n, const char *base)
{
    sh_modmanifest_list l;
    char err[160] = "", *heap, *out = NULL;
    size_t len = 0, i;
    memset(&l, 0, sizeof l);
    for (i = 0; i < n; i++) sh_modmanifest_list_add(&l, &rows[i]);
    heap = sh_modmanifest_write(base, strlen(base), &l, &len, err, sizeof err);
    sh_modmanifest_list_free(&l);
    if (!heap) { fprintf(stderr, "map_with: %s\n", err); return NULL; }
    out = (char *)malloc(len + 1);
    if (out) { memcpy(out, heap, len); out[len] = 0; }
    HeapFree(GetProcessHeap(), 0, heap);
    return out;
}

/* ------------------------------------------------------------ ZIPs in memory */
typedef struct zent { char *name; unsigned char *data; size_t len; } zent;
typedef struct zset { zent *e; size_t n; } zset;

static int zset_add(zset *z, const char *name, const void *data, size_t len)
{
    zent *g = (zent *)realloc(z->e, (z->n + 1) * sizeof *g);
    if (!g) return 0;
    z->e = g;
    g[z->n].name = _strdup(name);
    g[z->n].data = (unsigned char *)malloc(len + 1);
    if (!g[z->n].name || !g[z->n].data) return 0;
    if (len) memcpy(g[z->n].data, data, len);
    g[z->n].data[len] = 0;
    g[z->n].len = len;
    z->n++;
    return 1;
}
static void zset_free(zset *z)
{
    size_t i;
    for (i = 0; i < z->n; i++) { free(z->e[i].name); free(z->e[i].data); }
    free(z->e);
    memset(z, 0, sizeof *z);
}
static int zset_load(zset *z, const char *path)
{
    sh_zip_reader r;
    char cause[256];
    size_t i;
    memset(z, 0, sizeof *z);
    if (!HO(cause, sh_zip_reader_open(&r, path, HO_P(0)))) return 0;
    for (i = 0; i < r.n; i++) {
        size_t bl = 0;
        unsigned char *b = HOT(unsigned char *, cause, sh_zip_reader_read(&r, (long)i, 64u << 20, &bl, HO_P(0)));
        if (!b || !zset_add(z, r.e[i].name, b, bl)) { free(b); sh_zip_reader_close(&r); return 0; }
        free(b);
    }
    sh_zip_reader_close(&r);
    return 1;
}
static zent *zset_find(const zset *z, const char *name)
{
    size_t i;
    for (i = 0; i < z->n; i++) if (!strcmp(z->e[i].name, name)) return &z->e[i];
    return NULL;
}
static void zset_drop(zset *z, const char *name)
{
    size_t i;
    for (i = 0; i < z->n; i++)
        if (!strcmp(z->e[i].name, name)) {
            free(z->e[i].name);
            free(z->e[i].data);
            memmove(&z->e[i], &z->e[i + 1], (z->n - i - 1) * sizeof *z->e);
            z->n--;
            return;
        }
}
static int zset_save(const zset *z, const char *path)
{
    sh_zip_writer w;
    wchar_t *wp = sh_mb_wpath(path);
    size_t i;
    int ok;
    memset(&w, 0, sizeof w);
    w.h = INVALID_HANDLE_VALUE;
    if (!wp || !sh_zip_writer_open(&w, wp)) { free(wp); return 0; }
    for (i = 0; i < z->n; i++) sh_zip_writer_add(&w, z->e[i].name, z->e[i].data, z->e[i].len);
    ok = sh_zip_writer_finish(&w) && !w.failed;
    sh_zip_writer_free(&w);
    free(wp);
    return ok;
}

static unsigned char *slurp(const char *path, size_t *len) { return sh_mb_read_file(path, 256u << 20, len); }

/* 1 when the two files hold the same bytes. */
static int same_file(const char *a, const char *b)
{
    size_t al = 0, bl = 0;
    unsigned char *x = slurp(a, &al), *y = slurp(b, &bl);
    int same = x && y && al == bl && !memcmp(x, y, al);
    free(x);
    free(y);
    return same;
}

/* The zset's entries named under `prefix`. */
static size_t count_prefix(const zset *z, const char *prefix)
{
    size_t i, n = 0;
    for (i = 0; i < z->n; i++) if (!strncmp(z->e[i].name, prefix, strlen(prefix))) n++;
    return n;
}

/* Every units/ and files/ entry holds the bytes its name says (the SHA-256). */
static int entries_named_by_hash(const zset *z)
{
    size_t i;
    int ok = 1;
    for (i = 0; i < z->n; i++) {
        const char *n = z->e[i].name;
        char hex[SH_MB_HASH_HEX + 1];
        int unit = !strncmp(n, "units/", 6), file = !strncmp(n, "files/", 6);
        if (!unit && !file) continue;
        sh_mb_sha256_hex(z->e[i].data, z->e[i].len, hex);
        if (strncmp(hex, n + 6, SH_MB_HASH_HEX) || strlen(n) != (size_t)(6 + SH_MB_HASH_HEX + (unit ? 5 : 0))) ok = 0;
        if (unit && strcmp(n + 6 + SH_MB_HASH_HEX, ".json")) ok = 0;
    }
    return ok;
}

/* ------------------------------------------------------------ the folder, the cache */
static size_t count_files(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2], child[MAX_PATH * 2];
    HANDLE h;
    size_t n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        _snprintf_s(child, sizeof child, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += count_files(child);
        else n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* What the mods folder `root` holds besides the dot folders: nothing is installed by an import. */
static size_t mod_folders(const char *root)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    size_t n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* Units and files in the content cache of `root`. */
static void cache_counts(const char *root, size_t *units, size_t *files)
{
    char dir[MAX_PATH * 2];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\content\\units", mt_cache_dir(root));
    *units = count_files(dir);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\content\\files", mt_cache_dir(root));
    *files = count_files(dir);
}

static int cache_empty(const char *root)
{
    size_t u, f;
    cache_counts(root, &u, &f);
    return u == 0 && f == 0;
}

/* The pinned rows of a map text (the export's own). */
static size_t rows_of_text(const char *text, size_t len, sh_modmanifest_list *rows)
{
    unsigned rej = 0;
    memset(rows, 0, sizeof *rows);
    return sh_modmanifest_read(text, len, rows, &rej);
}

/* Every unit of the rows is in the cache with its files (state 1). */
static int units_present(const sh_modmanifest_list *rows)
{
    size_t i, k;
    int ok = rows->count > 0;
    for (i = 0; i < rows->count; i++)
        for (k = 0; k < rows->rows[i].unit_count; k++)
            ok = ok && sh_cache_unit_state(rows->rows[i].units[k], NULL, NULL, NULL) == 1;
    return ok;
}

/* Replace text of equal length inside a stored entry's bytes. */
static int patch(zset *z, const char *name, const char *from, const char *to)
{
    zent *e = zset_find(z, name);
    size_t i, k = strlen(from);
    if (!e || k != strlen(to)) return 0;
    for (i = 0; i + k <= e->len; i++)
        if (!memcmp(e->data + i, from, k)) { memcpy(e->data + i, to, k); return 1; }
    return 0;
}

/* The first entry under `prefix` whose bytes hold `needle`, or NULL. */
static zent *find_holding(const zset *z, const char *prefix, const char *needle)
{
    size_t i, k = strlen(needle), j;
    for (i = 0; i < z->n; i++) {
        if (strncmp(z->e[i].name, prefix, strlen(prefix))) continue;
        for (j = 0; j + k <= z->e[i].len; j++) if (!memcmp(z->e[i].data + j, needle, k)) return &z->e[i];
    }
    return NULL;
}

/* ------------------------------------------------------------ prefab text */
static const char PREFAB[] =
    "{\"cameraYaw\":0,\"entities\":[{\"displayName\":\"Crate\",\"entityDef\":{\"className\":\"idSnapEntity\","
    "\"inherit\":\"mods/" SA "/crate\",\"name\":\"\"}}],\"grabDistance\":256,\"~type\":\"idSnapEntityPrefab\"}";

/* The prefab text the export carries: the engine's text with a block naming the pinned rows (the product's is
 * prefab_mods.c pm_export_text). */
static int g_text_for_calls, g_text_for_moved;
static char *prefab_text_for(void *ctx, const sh_modmanifest_list *rows, int moved, size_t *len)
{
    char *block, *out, *tmp;
    size_t i, bl = 0, cap = 512;
    (void)ctx;
    g_text_for_calls++;
    g_text_for_moved = moved;
    block = (char *)malloc(cap);
    if (!block) return NULL;
    strcpy_s(block, cap, "{\"format\":1,\"description\":\"\",\"tags\":[],\"mods\":[");
    for (i = 0; i < rows->count; i++) {
        size_t vl = 0, need;
        char *v = sh_modmanifest_format_value(&rows->rows[i], &vl);
        if (!v) { free(block); return NULL; }
        need = strlen(block) + vl + 8;
        if (need > cap) { cap = need * 2; tmp = (char *)realloc(block, cap); if (!tmp) { free(v); free(block); return NULL; } block = tmp; }
        if (i) strcat_s(block, cap, ",");
        strcat_s(block, cap, v);
        free(v);
    }
    strcat_s(block, cap, "]}");
    out = sh_prefab_block_put(PREFAB, sizeof PREFAB - 1, block, strlen(block), &bl);
    free(block);
    *len = bl;
    return out;
}

/* The importer's rows: the block's "mods" of the prefab text; -2 for a row in an earlier shape. */
static int prefab_rows_of(void *ctx, const char *prefab, size_t len, sh_modmanifest_list *rows)
{
    sh_prefab_block_span span;
    mpj *block, *mods;
    int i, n = 0, earlier = 0;
    (void)ctx;
    if (sh_prefab_block_find(prefab, len, &span) != 1) return 0;
    block = mpj_parse(prefab + span.value_at, span.value_len);
    mods = block ? mpj_get(block, "mods") : NULL;
    for (i = 0; mods && mods->type == MPJ_ARR && i < mods->n; i++) {
        sh_modmanifest_row row;
        char err[160], *dump;
        size_t dl = 0;
        memset(&row, 0, sizeof row);
        dump = mpj_dump(mods->items[i], &dl);
        if (!dump) continue;
        if (mpj_get(mods->items[i], "id") || mpj_get(mods->items[i], "bundle")) earlier = 1;
        else if (sh_modmanifest_parse_value(dump, dl, &row, err, sizeof err)) { sh_modmanifest_list_add(rows, &row); n++; }
        sh_modmanifest_row_free(&row);
        free(dump);
    }
    mpj_free(block);
    return earlier ? -2 : n;
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], a_root[MAX_PATH], p_root[MAX_PATH], e_root[MAX_PATH], m_root[MAX_PATH], zip[MAX_PATH],
         zip2[MAX_PATH], path[MAX_PATH * 2], cause[HO_TEXT], want[HO_TEXT];
    sh_modmanifest_row rows[3];
    sh_modmanifest_list pinned;
    modexport_stats st;
    zset z;
    const char *crate_names[2] = { "mods/" SA "/crate", "mods/" SA "/barrel" };
    const char *red_names[1] = { "mods/" SB "/red" };
    char *map = NULL, *back = NULL;
    size_t len = 0, i, k, u, f;
    unsigned char *z1 = NULL, *z2 = NULL;
    size_t z1l = 0, z2l = 0;
    (void)argc; (void)argv;
    g_echo = getenv("MODEXPORT_TEST_LOG") != NULL;
    CHECK(mt_temp_dir("modexport", tmp, sizeof tmp));
    _snprintf_s(a_root, sizeof a_root, _TRUNCATE, "%s\\author", tmp);
    _snprintf_s(p_root, sizeof p_root, _TRUNCATE, "%s\\player", tmp);
    _snprintf_s(e_root, sizeof e_root, _TRUNCATE, "%s\\empty", tmp);
    _snprintf_s(m_root, sizeof m_root, _TRUNCATE, "%s\\manifests", tmp);
    _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\Pink Arena.zip", tmp);
    _snprintf_s(zip2, sizeof zip2, _TRUNCATE, "%s\\again.zip", tmp);
    sh_cache_set_resolver(&g_resolver);
    sh_modpin_set_game(fixture_game, NULL);
    sh_modpin_set_read(no_read, NULL);
    sh_modpin_set_folder(serve_folder, NULL);

    CHECK(make_world(a_root));
    use_root(a_root);
    local_row(&rows[0], SA, "Crates", crate_names, 2);
    local_row(&rows[1], SB, "Paints", red_names, 1);
    map = map_with(rows, 2, BASE_MAP);
    CHECK(map != NULL && strstr(map, "\"local\":true") != NULL);

    printf("-- export: each local row pinned with modpin, its units' manifests and files in the new layout\n");
    CHECK(map && HO(cause, modexport_write(map, strlen(map), zip, &st, HO_P(0))));
    if (cause[0]) fprintf(stderr, "  export: %s\n", cause);
    /* bundles the rows, bundles_installed the units carried (crate, barrel, red), files the files carried */
    CHECK(st.bundles == 2 && st.bundles_installed == 3 && st.files == 6);
    CHECK(st.entries == 1 + 3 + 6 && st.map_bytes > 0 && st.zip_bytes > 0);
    CHECK(zset_load(&z, zip));
    CHECK(z.n == st.entries);
    CHECK(zset_find(&z, "Pink Arena.json") != NULL);
    CHECK(count_prefix(&z, "units/") == 3 && count_prefix(&z, "files/") == 6);
    CHECK(count_prefix(&z, "mods/") == 0 && !zset_find(&z, "export.json") && !find_holding(&z, "", "bundle.canonical.json"));
    CHECK(z.n == count_prefix(&z, "units/") + count_prefix(&z, "files/") + 1);   /* nothing else */
    CHECK(entries_named_by_hash(&z));
    {
        zent *m = zset_find(&z, "Pink Arena.json");
        size_t pn = m ? rows_of_text(m ? (const char *)m->data : "", m ? m->len : 0, &pinned) : 0;
        CHECK(pn == 2 && m && !strstr((const char *)m->data, "\"local\":true") && strstr((const char *)m->data, "\"objects\""));
        CHECK(m && strstr((const char *)m->data, "EXPORT_MARK") != NULL);   /* the map is carried as it was */
        /* every row is pinned to the units of its placed objects, and every unit is an entry of the ZIP */
        for (i = 0; i < pinned.count; i++) {
            const sh_modmanifest_row *r = &pinned.rows[i];
            const char *units[8];
            char chash[SH_MODMANIFEST_HASH_CAP];
            CHECK(r->pinned && r->unit_count == (!strcmp(r->salt, SA) ? 2u : 1u));
            CHECK(!strcmp(r->salt, SA) ? !strcmp(sh_modmanifest_row_name(r), "Crates") : !strcmp(sh_modmanifest_row_name(r), "Paints"));
            for (k = 0; k < r->unit_count && k < 8; k++) {
                char entry[100];
                units[k] = r->units[k];
                _snprintf_s(entry, sizeof entry, _TRUNCATE, "units/%s.json", r->units[k]);
                CHECK(zset_find(&z, entry) != NULL);
                CHECK(sh_cache_unit_state(r->units[k], NULL, NULL, NULL) == 1);   /* modpin wrote it into the cache */
            }
            CHECK(sh_modmanifest_composition_hash(r->salt, units, r->unit_count, chash) && !strcmp(chash, r->build));
        }
        /* each unit's files are all entries too, once */
        for (i = 0; i < pinned.count; i++)
            for (k = 0; k < pinned.rows[i].unit_count; k++) {
                char entry[100];
                size_t ml = 0;
                char *mf;
                mpj *mj;
                const mpj *fl;
                int j;
                _snprintf_s(entry, sizeof entry, _TRUNCATE, "units/%s.json", pinned.rows[i].units[k]);
                mf = sh_cache_manifest(pinned.rows[i].units[k], &ml);
                mj = mf ? mpj_parse(mf, ml) : NULL;
                fl = mj ? mpj_get(mj, "files") : NULL;
                CHECK(mf && zset_find(&z, entry) && zset_find(&z, entry)->len == ml && !memcmp(zset_find(&z, entry)->data, mf, ml));
                CHECK(fl && fl->type == MPJ_ARR && fl->n == 2);
                for (j = 0; fl && fl->type == MPJ_ARR && j < fl->n; j++) {
                    const mpj *sha = mpj_get(fl->items[j], "sha256");
                    _snprintf_s(entry, sizeof entry, _TRUNCATE, "files/%s", sha ? sha->text : "?");
                    CHECK(sha && zset_find(&z, entry) != NULL);
                }
                mpj_free(mj);
                free(mf);
            }
        sh_modmanifest_list_free(&pinned);
    }
    /* a carried file is the folder's own bytes, byte for byte */
    {
        zent *e = find_holding(&z, "files/", "inherit = \"snapmaps/props/crate\"");
        char want_path[MAX_PATH * 2];
        _snprintf_s(want_path, sizeof want_path, _TRUNCATE, "%s\\crates\\entities\\crate.decl", a_root);
        CHECK(e != NULL);
        if (e) {
            size_t fl = 0;
            unsigned char *on_disk = slurp(want_path, &fl);
            CHECK(on_disk && fl == e->len && !memcmp(on_disk, e->data, fl));
            free(on_disk);
        }
        /* the unplaced lamp is in no unit: nothing of it travels */
        CHECK(!find_holding(&z, "files/", "lamp"));
        /* nothing names a folder of the author's computer */
        CHECK(!find_holding(&z, "", "crates\\") && !find_holding(&z, "", a_root));
    }
    zset_free(&z);

    printf("-- two exports of the same map are byte-identical\n");
    z1 = slurp(zip, &z1l);
    CHECK(map && HO(cause, modexport_write(map, strlen(map), zip, &st, HO_P(0))));
    z2 = slurp(zip, &z2l);
    CHECK(z1 && z2 && z1l == z2l && !memcmp(z1, z2, z1l));
    free(z2); z2 = NULL;
    /* a ZIP under another name carries the same units; only the map's entry is named after it */
    CHECK(HO(cause, modexport_write(map, strlen(map), zip2, &st, HO_P(0))));
    {
        zset a, b;
        CHECK(zset_load(&a, zip) && zset_load(&b, zip2) && a.n == b.n && zset_find(&b, "again.json") != NULL);
        zset_free(&a);
        zset_free(&b);
    }

    printf("-- the rows of an export of a map without mods: one entry, nothing to pin\n");
    {
        static const char PLAIN[] = "{\"~type\":\"idSnapMap\",\"entities\":[],\"name\":\"PLAIN\"}";
        char plain_zip[MAX_PATH];
        zset pz;
        _snprintf_s(plain_zip, sizeof plain_zip, _TRUNCATE, "%s\\plain.zip", tmp);
        CHECK(HO(cause, modexport_write(PLAIN, sizeof PLAIN - 1, plain_zip, &st, HO_P(0))));
        CHECK(st.bundles == 0 && st.bundles_installed == 0 && st.files == 0 && st.entries == 1);
        CHECK(zset_load(&pz, plain_zip) && pz.n == 1 && !strcmp(pz.e[0].name, "plain.json") && pz.e[0].len == sizeof PLAIN - 1 &&
              !memcmp(pz.e[0].data, PLAIN, pz.e[0].len));
        zset_free(&pz);
    }

    printf("-- export refuses a row no folder here serves, naming the mod\n");
    {
        sh_modmanifest_row three[3];
        char *m3;
        const char *ghost_names[1] = { "mods/" SG "/spirit" };
        three[0] = rows[0];
        three[1] = rows[1];
        local_row(&three[2], SG, "Ghost Mod", ghost_names, 1);
        m3 = map_with(three, 3, BASE_MAP);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\ghost.zip", tmp);
        CHECK(m3 && !HO(cause, modexport_write(m3, strlen(m3), path, &st, HO_P(0))));
        CHECK(!strcmp(cause, "this map uses Ghost Mod, which is not on this computer. Install it in the Mods tab, or remove "
                             "its objects from the map"));
        CHECK(!strstr(cause, SG));
        CHECK(!mt_exists(path));   /* no ZIP, and no half-written one */
        /* the same sentence through the hooks the rawmap layer calls */
        {
            unsigned bundles = 99;
            CHECK(m3 && HOZ(cause, modexport_write_text_report(m3, strlen(m3), path, &bundles, HO_P(0))) == 0 &&
                  !strncmp(cause, "this map uses Ghost Mod, which is not on this computer", 54) && !mt_exists(path));
            CHECK(modexport_write_text(m3, strlen(m3), path) == 0 && !mt_exists(path));
        }
        free(m3);
    }

    printf("-- export refuses a unit whose manifest, or a file of it, is not on this computer\n");
    {
        zset zs;
        zent *m;
        size_t pn;
        char *back2;
        CHECK(zset_load(&zs, zip));
        m = zset_find(&zs, "Pink Arena.json");
        /* a player's computer: the export's content in the cache, then the pinned map text */
        use_root(p_root);
        g_nserve = 0;
        back2 = HOT(char *, cause, modexport_read(zip, &len, &st, HO_P(0)));
        CHECK(back2 != NULL);
        pn = back2 ? rows_of_text(back2, len, &pinned) : 0;
        CHECK(pn == 2);
        /* it exports again from the cache alone (a downloaded map's content is there): the same ZIP */
        CHECK(back2 && HO(cause, modexport_write(back2, len, zip2, &st, HO_P(0))));
        CHECK(st.bundles == 2 && st.bundles_installed == 3 && st.files == 6);
        {
            zset again;
            CHECK(zset_load(&again, zip2) && again.n == 10);
            zset_free(&again);
        }
        /* a computer with none of it: the unit's manifest is not here */
        use_root(e_root);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\nocontent.zip", tmp);
        CHECK(back2 && !HO(cause, modexport_write(back2, len, path, &st, HO_P(0))));
        _snprintf_s(want, sizeof want, _TRUNCATE, "the content of %s that this map uses is not on this computer. Play the map "
                    "once online, or install the mod, then export again", sh_modmanifest_row_name(&pinned.rows[0]));
        CHECK(!strcmp(cause, want));
        CHECK(!mt_exists(path));
        /* a computer with the manifests and not their files */
        use_root(m_root);
        for (i = 0; i < pinned.count; i++)
            for (k = 0; k < pinned.rows[i].unit_count; k++) {
                char entry[100];
                zent *e;
                _snprintf_s(entry, sizeof entry, _TRUNCATE, "units/%s.json", pinned.rows[i].units[k]);
                e = zset_find(&zs, entry);
                CHECK(e && sh_cache_put_manifest(pinned.rows[i].units[k], e->data, e->len));
                CHECK(sh_cache_unit_state(pinned.rows[i].units[k], &u, NULL, NULL) == 0 && u == 2);
            }
        CHECK(back2 && !HO(cause, modexport_write(back2, len, path, &st, HO_P(0))));
        _snprintf_s(want, sizeof want, _TRUNCATE, "some files of %s that this map uses are not on this computer. Play the map "
                    "once online, or install the mod, then export again", sh_modmanifest_row_name(&pinned.rows[0]));
        CHECK(!strcmp(cause, want));
        CHECK(!mt_exists(path));
        free(back2);
        sh_modmanifest_list_free(&pinned);
        zset_free(&zs);
    }

    printf("-- import into a player's empty mods folder: every unit and file into the cache, nothing installed\n");
    use_root(p_root);   /* what the export check above wrote is the import's work; start from empty */
    mt_remove_tree(mt_cache_dir(p_root));
    mt_remove_tree(p_root);
    use_root(p_root);
    CHECK(cache_empty(p_root));
    CHECK(HO(cause, modexport_probe(zip, HO_P(0))));
    back = HOT(char *, cause, modexport_read(zip, &len, &st, HO_P(0)));
    if (!back) fprintf(stderr, "  import: %s\n", cause);
    {
        zset zs;
        zent *m;
        CHECK(zset_load(&zs, zip));
        m = zset_find(&zs, "Pink Arena.json");
        /* the text comes back unchanged */
        CHECK(back && m && len == m->len && !memcmp(back, m->data, len));
        zset_free(&zs);
    }
    CHECK(st.bundles == 2 && st.bundles_installed == 3 && st.files == 6 && st.bundles_present == 0 && st.bundles_absent == 0);
    cache_counts(p_root, &u, &f);
    CHECK(u == 3 && f == 6);
    CHECK(mod_folders(p_root) == 0);   /* no mod folder, no maps folder: nothing was installed */
    CHECK(back && rows_of_text(back, len, &pinned) == 2 && units_present(&pinned));
    {
        /* each carried file is a blob under its SHA-256 */
        zset zs;
        CHECK(zset_load(&zs, zip));
        for (i = 0; i < zs.n; i++)
            if (!strncmp(zs.e[i].name, "files/", 6)) {
                char *blob = sh_cache_blob_path(zs.e[i].name + 6);
                size_t bl = 0;
                unsigned char *bytes = blob ? slurp(blob, &bl) : NULL;
                CHECK(blob != NULL && bytes && bl == zs.e[i].len && !memcmp(bytes, zs.e[i].data, bl));
                free(bytes);
                free(blob);
            }
        zset_free(&zs);
    }
    sh_modmanifest_list_free(&pinned);
    free(back);

    printf("-- a second import finds every unit present and puts nothing new\n");
    back = HOT(char *, cause, modexport_read(zip, &len, &st, HO_P(0)));
    CHECK(back && st.bundles == 2 && st.bundles_installed == 0 && st.bundles_present == 3 && st.files == 0 &&
          st.bundles_absent == 0);
    cache_counts(p_root, &u, &f);
    CHECK(u == 3 && f == 6 && mod_folders(p_root) == 0);
    free(back);
    /* one unit already here: only the other units are new */
    {
        zset zs;
        size_t cu, cf;
        char victim[MAX_PATH * 2];
        CHECK(zset_load(&zs, zip));
        for (i = 0; i < zs.n && strncmp(zs.e[i].name, "units/", 6); i++) {}
        CHECK(i < zs.n);
        _snprintf_s(victim, sizeof victim, _TRUNCATE, "%s\\content\\units\\%s", mt_cache_dir(p_root), zs.e[i < zs.n ? i : 0].name + 6);
        SetFileAttributesA(victim, FILE_ATTRIBUTE_NORMAL);
        CHECK(DeleteFileA(victim));
        back = HOT(char *, cause, modexport_read(zip, &len, &st, HO_P(0)));
        CHECK(back && st.bundles_installed == 1 && st.bundles_present == 2 && st.files == 0 && st.bundles_absent == 0);
        cache_counts(p_root, &cu, &cf);
        CHECK(cu == 3 && cf == 6);
        free(back);
        zset_free(&zs);
    }

    printf("-- an entry that does not hold what its name says is refused; the damaged content is not kept\n");
    {
        zset zs;
        zent *file, *unit;
        char bad[MAX_PATH], fname[100], uname[100], d_root[MAX_PATH];
        size_t cu, cf;
        _snprintf_s(d_root, sizeof d_root, _TRUNCATE, "%s\\damaged", tmp);
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\bad.zip", tmp);
        /* a file: its bytes changed, its name kept */
        CHECK(zset_load(&zs, zip));
        file = find_holding(&zs, "files/", "inherit = \"snapmaps/props/crate\"");
        CHECK(file != NULL);
        strcpy_s(fname, sizeof fname, file ? file->name : "files/?");
        CHECK(patch(&zs, fname, "props/crate", "props/CRATE") && zset_save(&zs, bad));
        use_root(d_root);
        back = HOT(char *, cause, modexport_read(bad, &len, &st, HO_P(0)));
        _snprintf_s(want, sizeof want, _TRUNCATE, "the export is damaged: one of its mod files is not what it says; export it again");
        CHECK(!back && !strcmp(cause, want));
        CHECK(sh_cache_blob_path(fname + 6) == NULL);   /* the damaged bytes are not in the cache */
        CHECK(mod_folders(d_root) == 0);
        {
            sh_modmanifest_list probe;
            zent *m = zset_find(&zs, "Pink Arena.json");
            CHECK(m && rows_of_text((const char *)m->data, m->len, &probe) == 2);
            sh_modmanifest_list_free(&probe);
        }
        zset_free(&zs);
        /* a unit: its manifest changed, its name kept */
        mt_remove_tree(mt_cache_dir(d_root));
        mt_remove_tree(d_root);
        use_root(d_root);
        CHECK(zset_load(&zs, zip));
        unit = find_holding(&zs, "units/", "entities/crate");
        CHECK(unit != NULL);
        strcpy_s(uname, sizeof uname, unit ? unit->name : "units/?");
        CHECK(patch(&zs, uname, "entities/crate", "entities/CRATE") && zset_save(&zs, bad));
        back = HOT(char *, cause, modexport_read(bad, &len, &st, HO_P(0)));
        _snprintf_s(want, sizeof want, _TRUNCATE, "the export is damaged: one of its mod files is not what it says; export it again");
        CHECK(!back && !strcmp(cause, want));
        {
            char hash[SH_MB_HASH_HEX + 1];
            memcpy(hash, uname + 6, SH_MB_HASH_HEX);
            hash[SH_MB_HASH_HEX] = 0;
            CHECK(sh_cache_unit_state(hash, NULL, NULL, NULL) == -1);   /* never accepted as a unit */
        }
        CHECK(mod_folders(d_root) == 0);
        cache_counts(d_root, &cu, &cf);
        CHECK(cu <= 2);   /* at most the units it had checked before the damaged one */
        zset_free(&zs);
        /* a name that is no hash is not one of the export's files */
        mt_remove_tree(mt_cache_dir(d_root));
        mt_remove_tree(d_root);
        use_root(d_root);
        CHECK(zset_load(&zs, zip) && zset_add(&zs, "files/notahash", "x", 1) && zset_save(&zs, bad));
        back = HOT(char *, cause, modexport_read(bad, &len, &st, HO_P(0)));
        CHECK(!back && !strcmp(cause, "the export is damaged: one of its mod files is not what it says; export it again"));
        zset_drop(&zs, "files/notahash");
        CHECK(zset_add(&zs, "units/short.json", "{}", 2) && zset_save(&zs, bad));
        back = HOT(char *, cause, modexport_read(bad, &len, &st, HO_P(0)));
        CHECK(!back && !strcmp(cause, "the export is damaged: one of its mod files is not what it says; export it again"));
        CHECK(mod_folders(d_root) == 0);
        zset_free(&zs);
        use_root(p_root);
    }

    printf("-- a row whose content the export does not carry is counted as absent; the rest is put in the cache\n");
    {
        zset zs;
        char only_a[MAX_PATH], f_root[MAX_PATH], gone[100];
        sh_modmanifest_list prow;
        size_t cu, cf;
        zent *m;
        CHECK(zset_load(&zs, zip));
        m = zset_find(&zs, "Pink Arena.json");
        CHECK(m && rows_of_text((const char *)m->data, m->len, &prow) == 2);
        /* the Paints row's unit and its files are left out of the ZIP */
        {
            const sh_modmanifest_row *paints = !strcmp(prow.rows[0].salt, SB) ? &prow.rows[0] : &prow.rows[1];
            char entry[100], *mf;
            size_t ml = 0;
            mpj *mj;
            const mpj *fl;
            int j;
            _snprintf_s(entry, sizeof entry, _TRUNCATE, "units/%s.json", paints->units[0]);
            mf = (char *)(zset_find(&zs, entry) ? zset_find(&zs, entry)->data : NULL);
            ml = zset_find(&zs, entry) ? zset_find(&zs, entry)->len : 0;
            mj = mf ? mpj_parse(mf, ml) : NULL;
            fl = mj ? mpj_get(mj, "files") : NULL;
            for (j = 0; fl && fl->type == MPJ_ARR && j < fl->n; j++) {
                const mpj *sha = mpj_get(fl->items[j], "sha256");
                char fe[100];
                _snprintf_s(fe, sizeof fe, _TRUNCATE, "files/%s", sha ? sha->text : "?");
                zset_drop(&zs, fe);
            }
            mpj_free(mj);
            zset_drop(&zs, entry);
        }
        _snprintf_s(only_a, sizeof only_a, _TRUNCATE, "%s\\only crates.zip", tmp);
        _snprintf_s(f_root, sizeof f_root, _TRUNCATE, "%s\\absent", tmp);
        CHECK(zset_save(&zs, only_a));
        use_root(f_root);
        back = HOT(char *, cause, modexport_read(only_a, &len, &st, HO_P(0)));
        if (!back) fprintf(stderr, "  absent: %s\n", cause);
        CHECK(back && len == m->len && !memcmp(back, m->data, len));   /* the map is still the author's */
        CHECK(st.bundles == 2 && st.bundles_installed == 2 && st.files == 4 && st.bundles_absent == 1);
        cache_counts(f_root, &cu, &cf);
        CHECK(cu == 2 && cf == 4 && mod_folders(f_root) == 0);
        free(back);
        /* a unit whose manifest came and one of whose files did not: that row is absent too */
        zset_free(&zs);
        CHECK(zset_load(&zs, zip));
        {
            zent *file;
            file = find_holding(&zs, "files/", "mods/" SA "/models/barrel.lwo");
            CHECK(file != NULL);
            if (file) { strcpy_s(gone, sizeof gone, file->name); zset_drop(&zs, gone); }
        }
        CHECK(zset_save(&zs, only_a));
        mt_remove_tree(mt_cache_dir(f_root));
        mt_remove_tree(f_root);
        use_root(f_root);
        back = HOT(char *, cause, modexport_read(only_a, &len, &st, HO_P(0)));
        CHECK(back && st.bundles == 2 && st.bundles_installed == 3 && st.files == 5 && st.bundles_absent == 1);
        free(back);
        sh_modmanifest_list_free(&prow);
        zset_free(&zs);
        /* the missing file arrives later (a second import of the whole export): the row is whole */
        back = HOT(char *, cause, modexport_read(zip, &len, &st, HO_P(0)));
        CHECK(back && st.bundles_absent == 0 && st.files == 1 && st.bundles_present == 3);
        free(back);
        use_root(p_root);
    }

    printf("-- every earlier shape is refused with one sentence and puts nothing in the cache\n");
    {
        char old[MAX_PATH], n_root[MAX_PATH];
        zset zs;
        _snprintf_s(old, sizeof old, _TRUNCATE, "%s\\old.zip", tmp);
        _snprintf_s(n_root, sizeof n_root, _TRUNCATE, "%s\\nothing", tmp);
        use_root(n_root);
        /* whole mod folders under mods/, beside the units: the old layout */
        CHECK(zset_load(&zs, zip) && zset_add(&zs, "mods/crates/mod.json", "{\"name\":\"Crates\"}", 17));
        CHECK(zset_save(&zs, old));
        CHECK(!HOT(char *, cause, modexport_read(old, &len, &st, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        CHECK(!HO(cause, modexport_probe(old, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        CHECK(!HO(cause, modexport_validate_source(old, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        CHECK(!HOT(char *, cause, modexport_read_text_report(old, &len, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        CHECK(cache_empty(n_root) && mod_folders(n_root) == 0);   /* its units were not taken */
        zset_free(&zs);
        /* mods/ alone, and an asset path under it */
        CHECK(zset_add(&zs, "old.json", BASE_MAP, sizeof BASE_MAP - 1) && zset_add(&zs, "mods/x/assets/a.bimage", "x", 1) &&
              zset_save(&zs, old));
        CHECK(!HOT(char *, cause, modexport_read(old, &len, &st, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        zset_free(&zs);
        /* a bundle.canonical.json, at the top and below a folder */
        CHECK(zset_add(&zs, "old.json", BASE_MAP, sizeof BASE_MAP - 1) && zset_add(&zs, "bundle.canonical.json", "{}", 2) &&
              zset_save(&zs, old));
        CHECK(!HOT(char *, cause, modexport_read(old, &len, &st, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        zset_drop(&zs, "bundle.canonical.json");
        CHECK(zset_add(&zs, "x/bundle.canonical.json", "{}", 2) && zset_save(&zs, old));
        CHECK(!HOT(char *, cause, modexport_read(old, &len, &st, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        zset_free(&zs);
        /* an export.json index */
        CHECK(zset_add(&zs, "old.json", BASE_MAP, sizeof BASE_MAP - 1) &&
              zset_add(&zs, "export.json", "{\"format\":1,\"bundles\":[]}", 25) && zset_save(&zs, old));
        CHECK(!HOT(char *, cause, modexport_read(old, &len, &st, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        zset_free(&zs);
        /* a map whose rows name "id" and "bundle" */
        {
            static const char OLDMAP[] =
                "{\"~type\":\"idSnapMap\",\"entities\":[],\"snapmap-plus\":{\"format\":1,\"mods\":["
                "{\"id\":\"alex.pink-imp\",\"version\":\"1\",\"bundle\":\"0000000000000000000000000000000000000000000000000000000000000001\","
                "\"size\":1}]}}";
            CHECK(zset_add(&zs, "old.json", OLDMAP, sizeof OLDMAP - 1) && zset_save(&zs, old));
            CHECK(!HOT(char *, cause, modexport_read(old, &len, &st, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
            CHECK(!HO(cause, modexport_probe(old, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
            /* and it does not export either */
            CHECK(!HO(cause, modexport_write(OLDMAP, sizeof OLDMAP - 1, path, &st, HO_P(0))) && !strcmp(cause, MODEXPORT_EARLIER));
        }
        zset_free(&zs);
        CHECK(cache_empty(n_root) && mod_folders(n_root) == 0);
        use_root(p_root);
    }

    printf("-- a prefab's export has the same layout, with its rows pinned; each importer refuses the other's ZIP\n");
    {
        sh_modmanifest_list pr, got;
        char pz[MAX_PATH], q_root[MAX_PATH], *text = NULL, *pname = NULL, *pcause = NULL;
        size_t tl = 0, cu, cf;
        zset zs;
        zent *pj;
        memset(&pr, 0, sizeof pr);
        sh_modmanifest_list_add(&pr, &rows[0]);
        use_root(a_root);
        g_nserve = 2;
        _snprintf_s(pz, sizeof pz, _TRUNCATE, "%s\\door kit export.zip", tmp);
        g_text_for_calls = 0;
        CHECK(HO(cause, modexport_write_prefab("Door Kit", PREFAB, sizeof PREFAB - 1, &pr, prefab_text_for, NULL, pz, &st, HO_P(0))));
        if (cause[0]) fprintf(stderr, "  prefab export: %s\n", cause);
        CHECK(g_text_for_calls == 1 && g_text_for_moved == 1);   /* the text is written for the pinned rows */
        CHECK(st.bundles == 1 && st.bundles_installed == 2 && st.files == 4 && st.entries == 1 + 2 + 4);
        CHECK(zset_load(&zs, pz));
        pj = zset_find(&zs, "Door Kit.json");
        CHECK(pj && count_prefix(&zs, "units/") == 2 && count_prefix(&zs, "files/") == 4 && count_prefix(&zs, "mods/") == 0 &&
              zs.n == 7 && entries_named_by_hash(&zs));
        CHECK(pj && strstr((const char *)pj->data, "\"objects\"") && !strstr((const char *)pj->data, "\"local\":true"));
        /* the prefab text is the caller's with the pinned rows; the same bytes on a second write */
        {
            zset again;
            CHECK(HO(cause, modexport_write_prefab("Door Kit", PREFAB, sizeof PREFAB - 1, &pr, prefab_text_for, NULL, zip2, &st, HO_P(0))));
            CHECK(zset_load(&again, zip2) && again.n == zs.n && zset_find(&again, "Door Kit.json") &&
                  zset_find(&again, "Door Kit.json")->len == pj->len &&
                  !memcmp(zset_find(&again, "Door Kit.json")->data, pj->data, pj->len));
            zset_free(&again);
        }
        /* a prefab without a text callback carries the text as given */
        {
            char plain[MAX_PATH];
            zset zp;
            sh_modmanifest_list none;
            memset(&none, 0, sizeof none);
            _snprintf_s(plain, sizeof plain, _TRUNCATE, "%s\\Crate kit.zip", tmp);
            CHECK(HO(cause, modexport_write_prefab("Crate kit", PREFAB, sizeof PREFAB - 1, &none, NULL, NULL, plain, &st, HO_P(0))));
            CHECK(st.bundles == 0 && st.entries == 1 && zset_load(&zp, plain) && zp.n == 1 && zp.e[0].len == sizeof PREFAB - 1 &&
                  !memcmp(zp.e[0].data, PREFAB, zp.e[0].len));
            zset_free(&zp);
        }
        /* a row no folder here serves refuses the prefab's export with the mod's name */
        {
            sh_modmanifest_list gl;
            sh_modmanifest_row ghost;
            const char *gn[1] = { "mods/" SG "/spirit" };
            char gz[MAX_PATH];
            memset(&gl, 0, sizeof gl);
            local_row(&ghost, SG, "Ghost Mod", gn, 1);
            sh_modmanifest_list_add(&gl, &ghost);
            _snprintf_s(gz, sizeof gz, _TRUNCATE, "%s\\ghost prefab.zip", tmp);
            CHECK(!HO(cause, modexport_write_prefab("Ghost", PREFAB, sizeof PREFAB - 1, &gl, prefab_text_for, NULL, gz, &st, HO_P(0))));
            CHECK(!strcmp(cause, "this prefab uses Ghost Mod, which is not on this computer. Install it in the Mods tab, or "
                                 "remove its objects from the prefab") && !mt_exists(gz));
            sh_modmanifest_list_free(&gl);
            sh_modmanifest_row_free(&ghost);
        }
        /* import on a player's computer: units and files in the cache, the text unchanged, nothing installed */
        _snprintf_s(q_root, sizeof q_root, _TRUNCATE, "%s\\prefab player", tmp);
        use_root(q_root);
        g_nserve = 0;
        CHECK(modexport_read_prefab(pz, prefab_rows_of, NULL, &text, &tl, &pname, &st, &pcause) == 1);
        CHECK(text && pj && tl == pj->len && !memcmp(text, pj->data, tl) && pname && !strcmp(pname, "Door Kit"));
        CHECK(st.bundles == 1 && st.bundles_installed == 2 && st.files == 4 && st.bundles_present == 0 && st.bundles_absent == 0);
        cache_counts(q_root, &cu, &cf);
        CHECK(cu == 2 && cf == 4 && mod_folders(q_root) == 0);
        CHECK(prefab_rows_of(NULL, text, tl, (memset(&got, 0, sizeof got), &got)) == 1 && units_present(&got) && got.rows[0].pinned);
        sh_modmanifest_list_free(&got);
        free(text); free(pname); free(pcause);
        text = pname = pcause = NULL;
        /* again: present, nothing new */
        CHECK(modexport_read_prefab(pz, prefab_rows_of, NULL, &text, &tl, &pname, &st, &pcause) == 1);
        CHECK(st.bundles_installed == 0 && st.files == 0 && st.bundles_present == 2 && st.bundles_absent == 0);
        free(text); free(pname); free(pcause);
        text = pname = pcause = NULL;
        /* the extracted export: the importer is given the .json, its folder is the export */
        {
            char ex[MAX_PATH * 2], pjson[MAX_PATH * 2], x_root[MAX_PATH];
            _snprintf_s(ex, sizeof ex, _TRUNCATE, "%s\\Downloads\\Door Kit export", tmp);
            _snprintf_s(x_root, sizeof x_root, _TRUNCATE, "%s\\prefab folder", tmp);
            CHECK(mt_unzip_stored(pz, ex) == 7);
            _snprintf_s(pjson, sizeof pjson, _TRUNCATE, "%s\\Door Kit.json", ex);
            use_root(x_root);
            CHECK(modexport_read_prefab(pjson, prefab_rows_of, NULL, &text, &tl, &pname, &st, &pcause) == 1);
            CHECK(text && pname && !strcmp(pname, "Door Kit") && tl == pj->len && !memcmp(text, pj->data, tl));
            CHECK(st.bundles_installed == 2 && st.files == 4 && mod_folders(x_root) == 0);
            free(text); free(pname); free(pcause);
            text = pname = pcause = NULL;
            /* an extracted folder with an old mods/ folder beside is refused as the ZIP is */
            {
                char old_mod[MAX_PATH * 2];
                _snprintf_s(old_mod, sizeof old_mod, _TRUNCATE, "%s\\mods\\crates", ex);
                mt_write(old_mod, "mod.json", "{\"name\":\"Crates\"}");
                CHECK(!modexport_read_prefab(pjson, prefab_rows_of, NULL, &text, &tl, &pname, &st, &pcause) && pcause &&
                      !strcmp(pcause, MODEXPORT_EARLIER) && !text && !pname);
                free(pcause);
                pcause = NULL;
            }
        }
        /* a prefab whose rows are in an earlier shape */
        {
            static const char OLD_PREFAB[] =
                "{\"entities\":[],\"~type\":\"idSnapEntityPrefab\",\n\"snapmapPlus\":{\"format\":1,\"mods\":["
                "{\"id\":\"alex.pink-imp\",\"version\":\"1\",\"bundle\":\"0000000000000000000000000000000000000000000000000000000000000001\"}]}}";
            zset zo;
            char oz[MAX_PATH];
            _snprintf_s(oz, sizeof oz, _TRUNCATE, "%s\\old prefab.zip", tmp);
            memset(&zo, 0, sizeof zo);
            CHECK(zset_add(&zo, "Old.json", OLD_PREFAB, sizeof OLD_PREFAB - 1) && zset_save(&zo, oz));
            CHECK(!modexport_read_prefab(oz, prefab_rows_of, NULL, &text, &tl, &pname, &st, &pcause) && pcause &&
                  !strcmp(pcause, MODEXPORT_EARLIER));
            free(pcause);
            pcause = NULL;
            zset_free(&zo);
        }
        /* each importer refuses the other's export */
        use_root(p_root);
        CHECK(!HOT(char *, cause, modexport_read(pz, &len, &st, HO_P(0))) && !strcmp(cause, "this is a prefab's export, not a map's"));
        CHECK(!HO(cause, modexport_probe(pz, HO_P(0))) && !strcmp(cause, "this is a prefab's export, not a map's"));
        CHECK(!modexport_read_prefab(zip, prefab_rows_of, NULL, &text, &tl, &pname, &st, &pcause) && pcause &&
              !strcmp(pcause, "this is a map's export, not a prefab's") && !text && !pname);
        free(pcause);
        pcause = NULL;
        {
            size_t pu, pf;
            cache_counts(p_root, &pu, &pf);
            CHECK(pu == 3 && pf == 6);   /* what the map's imports put there; the refused prefab import added nothing */
        }
        zset_free(&zs);
        sh_modmanifest_list_free(&pr);
    }

    printf("-- the rawmap hooks: process-heap text, the units in the cache\n");
    {
        char h_root[MAX_PATH], utf8[MAX_PATH * 3];
        unsigned bundles = 0;
        char *heap;
        zset zs;
        _snprintf_s(h_root, sizeof h_root, _TRUNCATE, "%s\\hooks player", tmp);
        use_root(a_root);
        g_nserve = 2;
        /* a name no ANSI code page spells: the hooks take UTF-8 */
        _snprintf_s(utf8, sizeof utf8, _TRUNCATE, "%s\\hook\xc3\xa9 \xe2\x80\x94 \xe6\x97\xa5.zip", tmp);
        CHECK(map && HOZ(cause, modexport_write_text_report(map, strlen(map), utf8, &bundles, HO_P(0))) > 0 && bundles == 2);
        CHECK(modexport_write_text(map, strlen(map), utf8) > 0);
        CHECK(zset_load(&zs, utf8) && zset_find(&zs, "hook\xc3\xa9 \xe2\x80\x94 \xe6\x97\xa5.json") != NULL);
        CHECK(HO(cause, modexport_validate_source(utf8, HO_P(0))) && !strcmp(cause, "ok"));
        use_root(h_root);
        g_nserve = 0;
        heap = HOT(char *, cause, modexport_read_text_report(utf8, &len, HO_P(0)));
        CHECK(heap && zset_find(&zs, "hook\xc3\xa9 \xe2\x80\x94 \xe6\x97\xa5.json") &&
              len == zset_find(&zs, "hook\xc3\xa9 \xe2\x80\x94 \xe6\x97\xa5.json")->len &&
              !memcmp(heap, zset_find(&zs, "hook\xc3\xa9 \xe2\x80\x94 \xe6\x97\xa5.json")->data, len) && heap[len] == 0);
        cache_counts(h_root, &u, &f);
        CHECK(u == 3 && f == 6 && mod_folders(h_root) == 0);
        if (heap) HeapFree(GetProcessHeap(), 0, heap);
        zset_free(&zs);
        /* a path that is no export */
        CHECK(!HO(cause, modexport_validate_source("C:\\no\\such\\file.zip", HO_P(0))) && cause[0]);
        CHECK(modexport_is_zip_path("a.ZIP") && !modexport_is_zip_path("a.json") && !modexport_is_zip_path(NULL));
        use_root(p_root);
    }

    free(z1);
    free(map);
    sh_cache_set_mods_root(NULL);
    mt_remove_tree(tmp);
    printf("modexport_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
