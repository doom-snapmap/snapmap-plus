/* modexport_test -- exports that carry their mods as installed folders.
 *
 * A map whose smpmod1 rows name two installed mods is written to one ZIP:
 * <zip name>.json and mods/<folder>/mod.json and assets/ for each, the
 * mod.json and files byte for byte as the store holds them, no index, no
 * bundle.json, no canonical manifest, the same bytes on a second write. It is
 * probed and imported into an empty store as the ordinary install (each
 * folder built and verified under its own name, the serving index holding
 * both, the map text returned unchanged); a second import installs nothing
 * and rewrites nothing. A damaged entry and a folder whose files do not build
 * to the bundle its row names are refused and leave nothing installed; a
 * mod installed at another build (another version, or the same version with
 * other files) takes the carried build beside it as <key>@<hash16>, origin
 * "imported", and that folder name travels in a later export; nothing is
 * refused for a mod reason; a mod that needs another carried one installs
 * after it. A map naming a bundle it does not carry imports what it carries
 * and counts the rest as absent. Export by hash: a map pinning one build of
 * two installed carries exactly that build and round-trips on a second store;
 * a failed folder's build is carried from its last good build snapshot, a
 * build whose folder is gone from its snapshot alone, and a build rebuilt in
 * place through its alias, naming the current one. The rawmap hooks take
 * ANSI paths and return process-heap text. A prefab's export has the same
 * layout (also without mods); each importer refuses the other's ZIP; the
 * prefab importer installs only the carried folders the prefab's rows name,
 * reads an extracted export folder and a lone .json, and refuses a map's
 * JSON, a non-prefab and two prefabs in one ZIP. Both importers refuse the
 * retired bundle form (a bundle.canonical.json or an export.json index).
 * A hand-made folder installed under its generated key, named by a map in a
 * legacy smpmod1 row, exports and imports under that key whatever the carried
 * folder is called, and the load removes the legacy row.
 * Pure file work: no network.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modexport.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
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

static void make_mod_v(const char *dir, const char *id, const char *version, const char *const *files,
                       const char *const *bytes)
{
    char text[512];
    int i;
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"%s\",\"name\":\"%s\"}", id, version, id);
    mt_write(dir, "mod.json", text);
    for (i = 0; files[i]; i++) {
        char rel[MAX_PATH];
        char *c;
        strcpy_s(rel, sizeof rel, files[i]);
        for (c = rel; *c; c++) if (*c == '/') *c = '\\';
        mt_write(dir, rel, bytes[i]);
    }
}

static void make_mod(const char *dir, const char *id, const char *const *files, const char *const *bytes)
{
    make_mod_v(dir, id, "1.0.0", files, bytes);
}

static int install(const char *folder, char key[SH_MS_KEY_MAX + 1], sh_modmanifest_row *row)
{
    sh_ms_install_stats st;
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX];
    int ok = sh_ms_install_from_folder(folder, NULL, key, SH_MS_KEY_MAX + 1, &st, &b, cause, sizeof cause);
    if (!ok) fprintf(stderr, "install %s: %s\n", folder, cause);
    memset(row, 0, sizeof *row);
    if (ok) {
        strcpy_s(row->id, sizeof row->id, b.id);
        strcpy_s(row->version, sizeof row->version, b.version);
        strcpy_s(row->bundle, sizeof row->bundle, b.hash);
        row->size = b.total_size;
    }
    sh_mb_free(&b);
    return ok;
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

/* Byte offset of a stored entry's data, found through its local header name. */
static size_t entry_data(const unsigned char *zip, size_t len, const char *name)
{
    size_t nl = strlen(name), i;
    for (i = 0; i + 30 + nl <= len; i++) {
        if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 3 && zip[i + 3] == 4 &&
            (size_t)(zip[i + 26] | (zip[i + 27] << 8)) == nl && memcmp(zip + i + 30, name, nl) == 0)
            return i + 30 + nl + (size_t)(zip[i + 28] | (zip[i + 29] << 8));
    }
    return 0;
}

static size_t entry_size(const unsigned char *zip, size_t at, const char *name)
{
    const unsigned char *h = zip + at - strlen(name) - 30;
    return (size_t)h[18] | ((size_t)h[19] << 8) | ((size_t)h[20] << 16) | ((size_t)h[21] << 24);
}

/* 1 when the ZIP's entry `name` holds exactly the bytes of the file at path. */
static int entry_is_file(const unsigned char *zip, size_t zl, const char *name, const char *path)
{
    size_t at = entry_data(zip, zl, name), fl = 0;
    unsigned char *disk = slurp(path, &fl);
    int same = at && disk && entry_size(zip, at, name) == fl && !memcmp(zip + at, disk, fl);
    free(disk);
    return same;
}

/* Replace text of equal length inside a stored entry and fix its CRC in the
 * local header and the central directory, so only the content is wrong. */
static int patch_entry(unsigned char *zip, size_t len, const char *name, const char *from, const char *to)
{
    size_t nl = strlen(name), at = entry_data(zip, len, name), size, i;
    unsigned long crc;
    unsigned char *hit = NULL;
    if (!at || strlen(from) != strlen(to)) return 0;
    size = entry_size(zip, at, name);
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

/* Rewrite one byte in every entry name (local headers and the central
 * directory) that starts with `prefix`: the byte at offset `at` of the name
 * becomes `to`. The data and CRCs are untouched. */
static unsigned patch_names(unsigned char *zip, size_t len, const char *prefix, size_t at, char to)
{
    size_t i, pl = strlen(prefix);
    unsigned n = 0;
    for (i = 0; i + 46 <= len; i++) {
        size_t nl, off;
        if (zip[i] != 'P' || zip[i + 1] != 'K') continue;
        if (zip[i + 2] == 3 && zip[i + 3] == 4) { nl = (size_t)(zip[i + 26] | (zip[i + 27] << 8)); off = i + 30; }
        else if (zip[i + 2] == 1 && zip[i + 3] == 2) { nl = (size_t)(zip[i + 28] | (zip[i + 29] << 8)); off = i + 46; }
        else continue;
        if (off + nl > len || nl <= at || nl < pl || memcmp(zip + off, prefix, pl)) continue;
        zip[off + at] = (unsigned char)to;
        n++;
    }
    return n;
}

/* Entries whose name contains `part`. */
static unsigned count_entries_with(const unsigned char *zip, size_t len, const char *part)
{
    unsigned n = 0;
    size_t i, k, pl = strlen(part);
    for (i = 0; i + 46 <= len; i++) {
        if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 1 && zip[i + 3] == 2) {
            size_t nl = (size_t)(zip[i + 28] | (zip[i + 29] << 8));
            if (i + 46 + nl > len) continue;
            for (k = 0; k + pl <= nl; k++) if (!memcmp(zip + i + 46 + k, part, pl)) { n++; break; }
        }
    }
    return n;
}

static unsigned count_entries(const unsigned char *zip, size_t len) { return count_entries_with(zip, len, ""); }

/* The number of folders directly under dir. */
static int folder_count(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    int n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') n++;   /* mod folders: not ., .., .builds */
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static int has_folder(const char *store, const char *name)
{
    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\mod.json", store, name);
    return mt_exists(path);
}

/* The prefab export's text callback: carries the given text with a mark, so
 * the test sees it was asked, and records what it was given. */
static int g_text_for_calls, g_text_for_moved;
static size_t g_text_for_rows;
static char *text_for(void *ctx, const sh_modmanifest_list *rows, int moved, size_t *len)
{
    const char *base = (const char *)ctx;
    size_t n = strlen(base);
    char *out = (char *)malloc(n + 1);
    g_text_for_calls++;
    g_text_for_moved = moved;
    g_text_for_rows = rows ? rows->count : 0;
    if (!out) return NULL;
    memcpy(out, base, n + 1);
    *len = n;
    return out;
}

/* The importer's rows: those whose hashes ctx (a NULL-terminated list) names. */
static int rows_named(void *ctx, const char *prefab, size_t len, sh_modmanifest_list *rows)
{
    const sh_modmanifest_row *const *want = (const sh_modmanifest_row *const *)ctx;
    (void)prefab; (void)len;
    memset(rows, 0, sizeof *rows);
    for (; want && *want; want++) sh_modmanifest_list_add(rows, *want);
    return (int)rows->count;
}

/* A map text naming the given rows (process heap). */
static char *map_naming(const char *base, size_t base_len, const sh_modmanifest_row *const *rows, size_t *len)
{
    sh_modmanifest_list list;
    char err[160];
    char *map;
    memset(&list, 0, sizeof list);
    for (; *rows; rows++) sh_modmanifest_list_add(&list, *rows);
    map = sh_modmanifest_write(base, base_len, &list, len, err, sizeof err);
    sh_modmanifest_list_free(&list);
    return map;
}

static int fresh_store(const char *store)
{
    return sh_ms_set_root(store) && sh_modserve_index_build(store) >= 0;
}

static const char *const A_FILES[] = { "assets/generated/image/mods/test.alpha/a.bimage", "assets/generated/image/mods/test.alpha/shared.bimage", NULL };
static const char *const A_BYTES[] = { "alpha only bytes", "bytes both bundles carry", NULL };
static const char *const A2_BYTES[] = { "alpha two bytes", "bytes both bundles carry", NULL };
static const char *const A3_BYTES[] = { "alpha other bytes", "bytes both bundles carry", NULL };
/* A test.alpha that no longer provides a.bimage (a path the rows list). */
static const char *const A4_FILES[] = { "assets/generated/image/mods/test.alpha/z.bimage", "assets/generated/image/mods/test.alpha/shared.bimage", NULL };
static const char *const B_FILES[] = { "assets/generated/image/mods/test.beta/shared.bimage", "assets/generated/image/mods/test.beta/b.bimage", NULL };
static const char *const B_BYTES[] = { "bytes both bundles carry", "beta only bytes", NULL };
static const char *const C_FILES[] = { "assets/generated/image/mods/test.gamma/c.bimage", NULL };
static const char *const C_BYTES[] = { "gamma bytes", NULL };
static const char *const BASE_FILES[] = { "assets/generated/decls/entitydef/mods/test.base/base.decl", NULL };
static const char *const BASE_BYTES[] = { "{\r\n\tinherit = \"snapmaps/doors/industrial_door_vertical\";\r\n}", NULL };
static const char *const DEP_FILES[] = { "assets/generated/decls/entitydef/mods/test.adep/dep.decl", NULL };
static const char *const DEP_BYTES[] = { "{\r\n\tinherit = \"mods/test.base/base\";\r\n}", NULL };

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"

static const char BASE_MAP[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/test.alpha/thing\",\"name\":\"\"}}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" VAR("door_label", "Open") "]},"
    "\"name\":\"idSnapMap\",\"~type\":\"idSnapMap\"}";

#define ALPHA_ASSET "mods/test.alpha/assets/generated/image/mods/test.alpha/a.bimage"
#define BETA_ASSET  "mods/test.beta/assets/generated/image/mods/test.beta/b.bimage"

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], src[MAX_PATH * 2], storeA[MAX_PATH * 2], storeB[MAX_PATH * 2], storeC[MAX_PATH * 2], dir[MAX_PATH * 2];
    char zip[MAX_PATH * 2], zip2[MAX_PATH * 2], bad[MAX_PATH * 2], plain[MAX_PATH * 2], cause[320], path[MAX_PATH * 2];
    char keyA[SH_MS_KEY_MAX + 1], keyB[SH_MS_KEY_MAX + 1], keyC[SH_MS_KEY_MAX + 1];
    sh_modmanifest_row ra, rb, rc;
    sh_modmanifest_list rows;
    modexport_stats st;
    char *map = NULL, err[160];
    size_t map_len = 0;
    g_echo = argc > 1 && !strcmp(argv[1], "-v");

    CHECK(mt_temp_dir("modexport", tmp, sizeof tmp));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\src", tmp);
    _snprintf_s(storeA, sizeof storeA, _TRUNCATE, "%s\\storeA", tmp);
    _snprintf_s(storeB, sizeof storeB, _TRUNCATE, "%s\\storeB", tmp);
    _snprintf_s(storeC, sizeof storeC, _TRUNCATE, "%s\\storeC", tmp);
    _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\out\\my map.zip", tmp);
    _snprintf_s(zip2, sizeof zip2, _TRUNCATE, "%s\\again.zip", tmp);
    _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\bad.zip", tmp);
    _snprintf_s(plain, sizeof plain, _TRUNCATE, "%s\\plain.json", tmp);
    CreateDirectoryA(src, NULL);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\out", tmp);
    CreateDirectoryA(dir, NULL);

    /* ---- the author's store and a map naming two mods ---- */
    CHECK(sh_ms_set_root(storeA));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alpha", src); make_mod(dir, "test.alpha", A_FILES, A_BYTES);
    CHECK(install(dir, keyA, &ra) && !strcmp(keyA, "test.alpha"));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\beta", src); make_mod(dir, "test.beta", B_FILES, B_BYTES);
    CHECK(install(dir, keyB, &rb) && !strcmp(keyB, "test.beta"));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gamma", src); make_mod(dir, "test.gamma", C_FILES, C_BYTES);
    CHECK(install(dir, keyC, &rc));
    memset(&rows, 0, sizeof rows);
    CHECK(sh_modmanifest_list_add(&rows, &ra) && sh_modmanifest_list_add(&rows, &rb));
    /* The path the map uses from test.alpha (the asset rule reads it at import). */
    CHECK(sh_modmanifest_row_add_name(&rows.rows[0], "generated/image/mods/test.alpha/a.bimage"));
    map = sh_modmanifest_write(BASE_MAP, sizeof BASE_MAP - 1, &rows, &map_len, err, sizeof err);
    CHECK(map != NULL && !strstr(map, "smpmod1.") && strstr(map, ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"test.alpha\"") &&
          strstr(map, "{\"id\":\"test.beta\""));
    sh_modmanifest_list_free(&rows);

    /* ---- write: <zip name>.json and each mod's installed folder ---- */
    CHECK(modexport_is_zip_path(zip) && modexport_is_zip_path("X.ZIP") && !modexport_is_zip_path("map.json") && !modexport_is_zip_path(".zip"));
    CHECK(modexport_write(map, map_len, zip, &st, cause, sizeof cause));
    if (g_echo) printf("    write: %s\n", cause);
    CHECK(st.bundles == 2 && st.files == 4 && st.entries == 1 + 2 + 4 && st.zip_bytes > 0 && st.map_bytes == map_len);
    {
        size_t zl = 0, zl2 = 0, at;
        unsigned char *z = slurp(zip, &zl), *z2;
        CHECK(z && zl == st.zip_bytes && z[0] == 'P' && z[1] == 'K');
        at = z ? entry_data(z, zl, "my map.json") : 0;
        CHECK(at && entry_size(z, at, "my map.json") == map_len && !memcmp(z + at, map, map_len));
        /* Each mod as the store holds it: its mod.json and files byte for byte. */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.alpha\\mod.json", storeA);
        CHECK(z && entry_is_file(z, zl, "mods/test.alpha/mod.json", path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.beta\\mod.json", storeA);
        CHECK(z && entry_is_file(z, zl, "mods/test.beta/mod.json", path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.alpha\\assets\\generated\\image\\mods\\test.alpha\\shared.bimage", storeA);
        CHECK(z && entry_is_file(z, zl, "mods/test.alpha/assets/generated/image/mods/test.alpha/shared.bimage", path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.beta\\assets\\generated\\image\\mods\\test.beta\\shared.bimage", storeA);
        CHECK(z && entry_is_file(z, zl, "mods/test.beta/assets/generated/image/mods/test.beta/shared.bimage", path));
        CHECK(z && entry_data(z, zl, BETA_ASSET) != 0);
        /* None of the retired transport form. */
        CHECK(z && count_entries_with(z, zl, "bundle.json") == 0 && count_entries_with(z, zl, "bundle.canonical.json") == 0);
        CHECK(z && count_entries_with(z, zl, "export.json") == 0 && count_entries_with(z, zl, "map.rawmap.json") == 0);
        CHECK(z && count_entries_with(z, zl, "legacy/") == 0 && count_entries_with(z, zl, "@") == 0 && count_entries_with(z, zl, "+1") == 0);
        CHECK(z && count_entries(z, zl) == st.entries && count_entries_with(z, zl, "test.gamma") == 0);
        /* The same content writes the same bytes. */
        CHECK(modexport_write(map, map_len, zip2, NULL, cause, sizeof cause));
        z2 = slurp(zip2, &zl2);
        CHECK(z2 && zl2 > 0 && entry_data(z2, zl2, "again.json") != 0);
        free(z2);
        CHECK(modexport_write(map, map_len, zip, NULL, cause, sizeof cause));
        z2 = slurp(zip, &zl2);
        CHECK(z2 && zl2 == zl && memcmp(z, z2, zl) == 0);
        free(z2);

        /* A damaged copy: one byte of a stored asset changed, its CRC left. */
        at = entry_data(z, zl, BETA_ASSET);
        CHECK(at != 0);
        if (at) z[at] ^= 0x20;
        CHECK(spit(bad, z, zl));
        if (at) z[at] ^= 0x20;
        /* A copy whose asset bytes changed with a correct CRC: beta's folder no
         * longer builds to the bundle its row names. */
        {
            char mis[MAX_PATH * 2];
            _snprintf_s(mis, sizeof mis, _TRUNCATE, "%s\\mismatch.zip", tmp);
            CHECK(patch_entry(z, zl, BETA_ASSET, "beta only", "BETA ONLY"));
            CHECK(spit(mis, z, zl));
            CHECK(patch_entry(z, zl, BETA_ASSET, "BETA ONLY", "beta only"));
            CHECK(fresh_store(storeC));
            {
                size_t ml = 0;
                char *mt = modexport_read(mis, &ml, &st, cause, sizeof cause);
                CHECK(mt == NULL && !strcmp(cause, "test.beta 1.0.0 in the export does not build to the bundle the map names"));
                free(mt);
            }
            /* Exactly what the import copied is gone again: alpha installed first, beta refused. */
            CHECK(folder_count(storeC) == 0 && st.bundles_installed == 0);
            CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 0);
        }
        free(z);
    }
    CHECK(modexport_probe(zip, cause, sizeof cause));
    {
        /* A map naming a bundle the store lacks is not exported. */
        sh_modmanifest_row ghost = ra;
        const sh_modmanifest_row *g1[] = { &ghost, NULL };
        char *gm;
        size_t gl = 0;
        strcpy_s(ghost.bundle, sizeof ghost.bundle, "2222222222222222222222222222222222222222222222222222222222222222");
        gm = map_naming(BASE_MAP, sizeof BASE_MAP - 1, g1, &gl);
        CHECK(gm && !modexport_write(gm, gl, zip2, &st, cause, sizeof cause) && strstr(cause, "not installed") != NULL);
        if (gm) HeapFree(GetProcessHeap(), 0, gm);
    }

    /* ---- import into an empty store: the ordinary install of each folder ---- */
    CHECK(fresh_store(storeB));
    CHECK(sh_modserve_index_build(storeB) == 0);
    {
        size_t len = 0;
        char *text = modexport_read(zip, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "import: %s\n", cause);
        CHECK(text && len == map_len && memcmp(text, map, len) == 0);
        CHECK(st.bundles == 2 && st.bundles_installed == 2 && st.bundles_present == 0 && st.files == 6);
        free(text);
    }
    {
        sh_ms_verify_stats vs;
        char vcause[SH_MB_CAUSE_MAX];
        sh_mb_bundle b;
        CHECK(has_folder(storeB, "test.alpha") && has_folder(storeB, "test.beta") && folder_count(storeB) == 2);
        CHECK(sh_ms_verify("test.alpha", &vs, vcause, sizeof vcause));
        CHECK(sh_ms_verify("test.beta", &vs, vcause, sizeof vcause) && vs.files == 2);
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1 && sh_modserve_bundle_state(rb.bundle, NULL, 0) == 1);
        CHECK(sh_modserve_bundle_state(rc.bundle, NULL, 0) == 0);
        /* Installed as a folder from elsewhere is: the same bundle, its files in assets\. */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.alpha", storeB);
        memset(&b, 0, sizeof b);
        CHECK(sh_mb_load_folder(path, &b) && !strcmp(b.hash, ra.bundle) && b.build == 1);
        sh_mb_free(&b);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.beta\\assets\\generated\\image\\mods\\test.beta\\shared.bimage", storeB);
        CHECK(mt_exists(path));
    }
    {
        /* Again: everything is installed already, so nothing is written. */
        char mj[MAX_PATH * 2], asset[MAX_PATH * 2];
        WIN32_FILE_ATTRIBUTE_DATA before_mj, before_asset, after_mj, after_asset;
        size_t l1 = 0, l2 = 0, len = 0;
        unsigned char *b1, *b2;
        char *text;
        _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\test.alpha\\mod.json", storeB);
        _snprintf_s(asset, sizeof asset, _TRUNCATE, "%s\\test.alpha\\assets\\generated\\image\\mods\\test.alpha\\a.bimage", storeB);
        CHECK(GetFileAttributesExA(mj, GetFileExInfoStandard, &before_mj));
        CHECK(GetFileAttributesExA(asset, GetFileExInfoStandard, &before_asset));
        b1 = slurp(mj, &l1);
        Sleep(20);
        text = modexport_read(zip, &len, &st, cause, sizeof cause);
        CHECK(text && st.bundles_installed == 0 && st.bundles_present == 2 && st.files == 0 && st.bundles_absent == 0);
        free(text);
        b2 = slurp(mj, &l2);
        CHECK(b1 && b2 && l1 == l2 && memcmp(b1, b2, l1) == 0);
        CHECK(GetFileAttributesExA(mj, GetFileExInfoStandard, &after_mj));
        CHECK(GetFileAttributesExA(asset, GetFileExInfoStandard, &after_asset));
        CHECK(CompareFileTime(&before_mj.ftLastWriteTime, &after_mj.ftLastWriteTime) == 0);
        CHECK(CompareFileTime(&before_asset.ftLastWriteTime, &after_asset.ftLastWriteTime) == 0);
        CHECK(folder_count(storeB) == 2);
        free(b1); free(b2);
    }
    {
        /* An export whose map names a bundle it does not carry (beta's row
         * renamed to a build nobody has): alpha installs, beta's folder is
         * carried but no row names it, and the absent row is counted. */
        static const char GHOST[] = "3333333333333333333333333333333333333333333333333333333333333333";
        char part[MAX_PATH * 2], storeE[MAX_PATH * 2];
        size_t zl = 0, len = 0;
        unsigned char *z = slurp(zip, &zl);
        char *text;
        _snprintf_s(part, sizeof part, _TRUNCATE, "%s\\partial.zip", tmp);
        _snprintf_s(storeE, sizeof storeE, _TRUNCATE, "%s\\storeE", tmp);
        CHECK(z && patch_entry(z, zl, "my map.json", rb.bundle, GHOST));
        CHECK(z && spit(part, z, zl));
        free(z);
        CHECK(modexport_probe(part, cause, sizeof cause));
        CHECK(fresh_store(storeE));
        text = modexport_read(part, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "partial import: %s\n", cause);
        CHECK(text && strstr(text, GHOST) != NULL);
        CHECK(st.bundles == 1 && st.bundles_installed == 1 && st.bundles_absent == 1);
        CHECK(has_folder(storeE, "test.alpha") && !has_folder(storeE, "test.beta"));
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1 && sh_modserve_bundle_state(GHOST, NULL, 0) == 0);
        free(text);
    }

    /* ---- refusals on import ---- */
    CHECK(fresh_store(storeC));
    CHECK(sh_modserve_index_build(storeC) == 0);
    {
        size_t len = 0;
        char *text = modexport_read(bad, &len, &st, cause, sizeof cause);
        CHECK(text == NULL && strstr(cause, "checksum") != NULL);
        CHECK(sh_modserve_bundle_state(rb.bundle, NULL, 0) == 0 && folder_count(storeC) == 0);
    }
    {
        /* Not a ZIP at all. */
        size_t len = 0;
        CHECK(spit(plain, "{\"name\":\"idSnapMap\"}", 20));
        CHECK(modexport_read(plain, &len, &st, cause, sizeof cause) == NULL && strstr(cause, "ZIP") != NULL);
        CHECK(!modexport_probe(plain, cause, sizeof cause));
    }
    {
        /* The retired bundle form: an export.json index, or any bundle.canonical.json. */
        static const char *const OLD_N[] = { "map.rawmap.json", "export.json" };
        static const char *const OLD_D[] = { "{\"entities\":[],\"~type\":\"idSnapMap\"}",
                                             "{\"bundles\":[],\"format\":1,\"map\":\"map.rawmap.json\"}" };
        static const char *const CAN_N[] = { "Old.json", "mods/test.alpha@1.0.0+1/bundle.canonical.json" };
        static const char *const CAN_D[] = { "{\"entities\":[],\"~type\":\"idSnapMap\"}", "{\"id\":\"test.alpha\"}" };
        char old[MAX_PATH * 2], name[160], *text = NULL;
        size_t len = 0;
        _snprintf_s(old, sizeof old, _TRUNCATE, "%s\\retired.zip", tmp);
        CHECK(mt_zip_stored(old, 2, OLD_N, OLD_D));
        /* beta.20's map export opens (its bundles: below); a prefab's import still refuses it. */
        text = modexport_read(old, &len, &st, cause, sizeof cause);
        CHECK(text && len == strlen(OLD_D[0]) && st.bundles == 0);
        free(text);
        text = NULL;
        CHECK(modexport_probe(old, cause, sizeof cause));
        CHECK(!strcmp(MODEXPORT_RETIRED_MAP, "this export was made by an earlier release; save the rawmap again"));
        CHECK(!modexport_read_prefab(old, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
              !strcmp(cause, MODEXPORT_RETIRED_PREFAB));
        CHECK(mt_zip_stored(old, 2, CAN_N, CAN_D));
        CHECK(modexport_read(old, &len, &st, cause, sizeof cause) == NULL && !strcmp(cause, MODEXPORT_RETIRED_MAP));
        CHECK(folder_count(storeC) == 0);
    }

    /* ---- the asset rule: an installed folder of the id that provides the listed paths is used,
     * whatever its bytes or version; one that lacks a path gets the carried folder beside it ---- */
    {
        char storeH[MAX_PATH * 2], storeI[MAX_PATH * 2], storeJ[MAX_PATH * 2], before[MAX_PATH * 2], zipH[MAX_PATH * 2];
        char key2[SH_MS_KEY_MAX + 1], *text, beside_a[SH_MS_KEY_MAX + 1];
        sh_modmanifest_row ra2, ra3;
        unsigned char *b1, *b2;
        size_t l1 = 0, l2 = 0, len = 0;
        _snprintf_s(storeH, sizeof storeH, _TRUNCATE, "%s\\storeH", tmp);
        _snprintf_s(storeI, sizeof storeI, _TRUNCATE, "%s\\storeI", tmp);
        _snprintf_s(storeJ, sizeof storeJ, _TRUNCATE, "%s\\storeJ", tmp);
        _snprintf_s(zipH, sizeof zipH, _TRUNCATE, "%s\\two versions.zip", tmp);
        /* storeH holds test.alpha 2.0.0 as test.alpha, without a.bimage: incomplete for the rows. */
        CHECK(fresh_store(storeH));
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alpha2", src); make_mod_v(dir, "test.alpha", "2.0.0", A4_FILES, A2_BYTES);
        CHECK(install(dir, key2, &ra2) && !strcmp(key2, "test.alpha"));
        _snprintf_s(before, sizeof before, _TRUNCATE, "%s\\test.alpha\\mod.json", storeH);
        b1 = slurp(before, &l1);
        CHECK(sh_modserve_index_build(storeH) == 1);
        text = modexport_read(zip, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "beside import: %s\n", cause);
        CHECK(text && st.bundles_installed == 2 && st.bundles_present == 0);
        free(text);
        /* 1.0.0 lands beside 2.0.0 under rule 1: <key>@<hash16> for a later build of a key. */
        _snprintf_s(beside_a, sizeof beside_a, _TRUNCATE, "test.alpha@%.16s", ra.bundle);
        CHECK(has_folder(storeH, beside_a) && has_folder(storeH, "test.beta") && folder_count(storeH) == 3);
        {
            char mj[MAX_PATH * 2];
            _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\%s\\mod.json", storeH, beside_a);
            unsigned char *t = slurp(mj, NULL);
            CHECK(t && strstr((const char *)t, "\"origin\": \"imported\""));
            free(t);
        }
        b2 = slurp(before, &l2);
        CHECK(b1 && b2 && l1 == l2 && !memcmp(b1, b2, l1));   /* the installed folder is not rewritten */
        free(b1); free(b2);
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1 && sh_modserve_bundle_state(ra2.bundle, NULL, 0) == 1);
        /* That folder's name travels: an export from storeH carries mods/test.alpha@<hash16>/. */
        {
            const sh_modmanifest_row *one[] = { &ra, NULL };
            size_t ml = 0, zl = 0;
            char *m1 = map_naming(BASE_MAP, sizeof BASE_MAP - 1, one, &ml);
            unsigned char *z;
            CHECK(m1 && modexport_write(m1, ml, zipH, &st, cause, sizeof cause) && st.bundles == 1);
            z = slurp(zipH, &zl);
            char ename[160];
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\mod.json", storeH, beside_a);
            _snprintf_s(ename, sizeof ename, _TRUNCATE, "mods/%s/mod.json", beside_a);
            CHECK(z && entry_is_file(z, zl, ename, path) && entry_data(z, zl, "two versions.json"));
            CHECK(z && entry_data(z, zl, "mods/test.alpha/mod.json") == 0 && count_entries(z, zl) == 4);
            free(z);
            if (m1) HeapFree(GetProcessHeap(), 0, m1);
            /* ... and an empty store installs it under its key (the first build of the key there). */
            CHECK(fresh_store(storeJ));
            text = modexport_read(zipH, &len, &st, cause, sizeof cause);
            CHECK(text && st.bundles_installed == 1 && has_folder(storeJ, "test.alpha") && folder_count(storeJ) == 1);
            free(text);
        }
        /* storeI holds test.alpha 1.0.0 with other bytes at the same paths:
         * another build of the key, not the one the map pins (additive 2.5),
         * so the carried build is installed beside it, as is test.beta. */
        CHECK(fresh_store(storeI));
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alpha3", src); make_mod(dir, "test.alpha", A_FILES, A3_BYTES);
        CHECK(install(dir, key2, &ra3) && strcmp(ra3.bundle, ra.bundle));
        _snprintf_s(before, sizeof before, _TRUNCATE, "%s\\test.alpha\\mod.json", storeI);
        b1 = slurp(before, &l1);
        CHECK(sh_modserve_index_build(storeI) == 1);
        text = modexport_read(zip, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "served import: %s\n", cause);
        CHECK(text && st.bundles_installed == 2 && st.bundles_present == 0);
        free(text);
        CHECK(folder_count(storeI) == 3 && has_folder(storeI, "test.beta") && has_folder(storeI, beside_a));
        b2 = slurp(before, &l2);
        CHECK(b1 && b2 && l1 == l2 && !memcmp(b1, b2, l1));
        free(b1); free(b2);
        /* The same version without a.bimage: the carried 1.0.0 lands beside it,
         * never over it (the store's own name for that id and version would
         * replace the installed folder). */
        {
            char storeL[MAX_PATH * 2];
            _snprintf_s(storeL, sizeof storeL, _TRUNCATE, "%s\\storeL", tmp);
            CHECK(fresh_store(storeL));
            _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alpha4", src); make_mod(dir, "test.alpha", A4_FILES, A3_BYTES);
            CHECK(install(dir, key2, &ra3) && !strcmp(key2, "test.alpha"));
            _snprintf_s(before, sizeof before, _TRUNCATE, "%s\\test.alpha\\mod.json", storeL);
            b1 = slurp(before, &l1);
            CHECK(sh_modserve_index_build(storeL) == 1);
            text = modexport_read(zip, &len, &st, cause, sizeof cause);
            if (!text) fprintf(stderr, "beside same version: %s\n", cause);
            CHECK(text && st.bundles_installed == 2 && st.bundles_present == 0);
            free(text);
            CHECK(has_folder(storeL, "test.alpha") && has_folder(storeL, beside_a) && folder_count(storeL) == 3);
            b2 = slurp(before, &l2);
            CHECK(b1 && b2 && l1 == l2 && !memcmp(b1, b2, l1));
            free(b1); free(b2);
        }
    }

    /* ---- a mod that needs another carried one installs after it (test.adep
     * comes first in the ZIP and needs test.base) ---- */
    {
        char storeK[MAX_PATH * 2], zipK[MAX_PATH * 2], kb[SH_MS_KEY_MAX + 1], kd[SH_MS_KEY_MAX + 1], *mk, *text;
        sh_modmanifest_row rbase, rdep;
        const sh_modmanifest_row *dep_first[] = { &rdep, &rbase, NULL };
        size_t ml = 0, len = 0;
        _snprintf_s(storeK, sizeof storeK, _TRUNCATE, "%s\\storeK", tmp);
        _snprintf_s(zipK, sizeof zipK, _TRUNCATE, "%s\\needs.zip", tmp);
        CHECK(fresh_store(storeA));
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\base", src); make_mod(dir, "test.base", BASE_FILES, BASE_BYTES);
        CHECK(install(dir, kb, &rbase));
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\dep", src); make_mod(dir, "test.adep", DEP_FILES, DEP_BYTES);
        CHECK(install(dir, kd, &rdep));
        mk = map_naming(BASE_MAP, sizeof BASE_MAP - 1, dep_first, &ml);
        CHECK(mk && modexport_write(mk, ml, zipK, &st, cause, sizeof cause) && st.bundles == 2);
        if (mk) HeapFree(GetProcessHeap(), 0, mk);
        CHECK(fresh_store(storeK));
        text = modexport_read(zipK, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "needs import: %s\n", cause);
        CHECK(text && st.bundles_installed == 2 && has_folder(storeK, "test.base") && has_folder(storeK, "test.adep"));
        CHECK(sh_modserve_bundle_state(rdep.bundle, NULL, 0) == 1);
        free(text);
    }

    /* ---- a mod in an organizing folder (mods\group\test.nested): its
     * entries name it mods/group/test.nested/ with '/' only, and it imports;
     * an export from before that fix (mods/group\test.nested/...) imports
     * too, while a backslash anywhere but under mods/ is still refused ---- */
    {
        static const char *const N_FILES[] = { "assets/generated/image/mods/test.nested/n.bimage", NULL };
        static const char *const N_BYTES[] = { "nested bytes", NULL };
        char storeN[MAX_PATH * 2], storeN2[MAX_PATH * 2], storeN3[MAX_PATH * 2], zipN[MAX_PATH * 2], zipO[MAX_PATH * 2];
        char kn[SH_MS_KEY_MAX + 1], *mn, *text;
        sh_modmanifest_row rn;
        const sh_modmanifest_row *only_n[] = { &rn, NULL };
        size_t ml = 0, len = 0, zl = 0;
        unsigned char *z;
        _snprintf_s(storeN, sizeof storeN, _TRUNCATE, "%s\\storeN", tmp);
        _snprintf_s(storeN2, sizeof storeN2, _TRUNCATE, "%s\\storeN2", tmp);
        _snprintf_s(storeN3, sizeof storeN3, _TRUNCATE, "%s\\storeN3", tmp);
        _snprintf_s(zipN, sizeof zipN, _TRUNCATE, "%s\\nested.zip", tmp);
        _snprintf_s(zipO, sizeof zipO, _TRUNCATE, "%s\\nested-old.zip", tmp);
        CHECK(fresh_store(storeN));
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\group\\test.nested", storeN);
        make_mod(dir, "test.nested", N_FILES, N_BYTES);
        CHECK(install(dir, kn, &rn) && !strcmp(kn, "group\\test.nested"));
        CHECK(sh_modserve_index_build(storeN) >= 0);
        mn = map_naming(BASE_MAP, sizeof BASE_MAP - 1, only_n, &ml);
        CHECK(mn && modexport_write(mn, ml, zipN, &st, cause, sizeof cause) && st.bundles == 1 && st.files == 1);
        if (!mn || st.bundles != 1) fprintf(stderr, "nested export: %s\n", cause);
        z = slurp(zipN, &zl);
        CHECK(z && count_entries_with(z, zl, "\\") == 0);
        CHECK(z && entry_data(z, zl, "mods/group/test.nested/mod.json") != 0);
        CHECK(z && entry_data(z, zl, "mods/group/test.nested/assets/generated/image/mods/test.nested/n.bimage") != 0);
        CHECK(z && count_entries_with(z, zl, "mods/group/test.nested/") == 2);
        /* Imported on an empty store: the key's folder, not the author's group. */
        CHECK(fresh_store(storeN2));
        text = modexport_read(zipN, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "nested import: %s\n", cause);
        CHECK(text && len == ml && st.bundles == 1 && st.bundles_installed == 1);
        CHECK(has_folder(storeN2, "test.nested") && sh_modserve_bundle_state(rn.bundle, NULL, 0) == 1);
        free(text);
        /* The previous build's ZIP: the folder's separator was a backslash. */
        CHECK(z && patch_names(z, zl, "mods/group/test.nested", 10, '\\') == 4);
        CHECK(z && count_entries_with(z, zl, "mods/group\\test.nested/") == 2 && spit(zipO, z, zl));
        CHECK(modexport_probe(zipO, cause, sizeof cause));
        CHECK(fresh_store(storeN3));
        text = modexport_read(zipO, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "old nested import: %s\n", cause);
        CHECK(text && len == ml && st.bundles_installed == 1 && has_folder(storeN3, "test.nested"));
        CHECK(sh_modserve_bundle_state(rn.bundle, NULL, 0) == 1);
        free(text);
        free(z);
        if (mn) HeapFree(GetProcessHeap(), 0, mn);
        /* A backslash outside mods/ is an unsafe name, as before. */
        {
            static const char *const BS_N[] = { "maps\\evil.json" };
            static const char *const BS_D[] = { "{\"entities\":[],\"~type\":\"idSnapMap\"}" };
            CHECK(mt_zip_stored(zipO, 1, BS_N, BS_D));
            text = modexport_read(zipO, &len, &st, cause, sizeof cause);
            CHECK(text == NULL && !strcmp(cause, "the export holds a file with an unsafe name"));
            free(text);
        }
    }

    /* ---- the rawmap hooks: ANSI paths, process-heap text ---- */
    CHECK(fresh_store(storeA));
    CHECK(modexport_write_text(map, map_len, zip2) > 0);
    {
        /* The reporting writer names the bundles and, on refusal, the cause. */
        sh_modmanifest_row ghost = ra;
        const sh_modmanifest_row *g1[] = { &ghost, NULL };
        unsigned bundles = 0;
        char *gm, why[320];
        size_t gl = 0;
        CHECK(modexport_write_text_report(map, map_len, zip2, &bundles, why, sizeof why) > 0 && bundles == 2 && !why[0]);
        strcpy_s(ghost.bundle, sizeof ghost.bundle, "4444444444444444444444444444444444444444444444444444444444444444");
        gm = map_naming(BASE_MAP, sizeof BASE_MAP - 1, g1, &gl);
        CHECK(gm && modexport_write_text_report(gm, gl, zip2, &bundles, why, sizeof why) == 0 && bundles == 1 &&
              strstr(why, "not installed") != NULL);
        if (gm) HeapFree(GetProcessHeap(), 0, gm);
        CHECK(modexport_validate_source(zip2, cause, (int)sizeof cause));   /* the earlier export is untouched */
    }
    {
        char msg[192];
        size_t len = 0;
        char *heap;
        CHECK(modexport_validate_source(zip2, msg, (int)sizeof msg) && strcmp(msg, "ok") == 0);
        CHECK(fresh_store(storeC));
        heap = modexport_read_text(zip2, &len);
        CHECK(heap && len == map_len && memcmp(heap, map, len) == 0);
        if (heap) HeapFree(GetProcessHeap(), 0, heap);
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1);
        CHECK(modexport_read_text(plain, &len) == NULL);
    }

    /* ---- export by hash (DESIGN-additive 4): each declared build at its
     * declared hash, also beside another build of its key, from a failed
     * folder's last good build snapshot, from a snapshot whose folder is gone,
     * and through an alias of a folder rebuilt in place ---- */
    {
        static const char *const C2_BYTES[] = { "gamma bytes, edited", NULL };
        static const char *const C3_BYTES[] = { "gamma bytes, edited in place", NULL };
        sh_modmanifest_row rc2, rc3;
        sh_modmanifest_list got;
        unsigned rejected = 0;
        char zip3[MAX_PATH * 2], storeD[MAX_PATH * 2], *pinned, *text, beside_c[SH_MS_KEY_MAX + 1], gfolder[MAX_PATH * 2];
        char snapdir[MAX_PATH * 4], storeD2[MAX_PATH * 2], storeD3[MAX_PATH * 2], storeD4[MAX_PATH * 2];
        char storeP[MAX_PATH * 2];
        size_t pl = 0, len = 0;
        const sh_modmanifest_row *pin_rows[] = { &ra, &rc, NULL };
        const sh_modmanifest_row *two_builds[] = { &rc, NULL, NULL };
        const sh_modmanifest_row *only_c[] = { &rc, NULL };
        _snprintf_s(zip3, sizeof zip3, _TRUNCATE, "%s\\pinned.zip", tmp);
        _snprintf_s(storeD, sizeof storeD, _TRUNCATE, "%s\\storeD", tmp);
        _snprintf_s(storeD2, sizeof storeD2, _TRUNCATE, "%s\\storeD2", tmp);
        _snprintf_s(storeD3, sizeof storeD3, _TRUNCATE, "%s\\storeD3", tmp);
        _snprintf_s(storeD4, sizeof storeD4, _TRUNCATE, "%s\\storeD4", tmp);
        _snprintf_s(storeP, sizeof storeP, _TRUNCATE, "%s\\storeP", tmp);
        CHECK(fresh_store(storeA));
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gamma", src);
        make_mod(dir, "test.gamma", C_FILES, C2_BYTES);
        CHECK(install(dir, keyC, &rc2) && strcmp(rc2.bundle, rc.bundle));   /* another build of the key */
        _snprintf_s(beside_c, sizeof beside_c, _TRUNCATE, "test.gamma@%.16s", rc2.bundle);
        CHECK(!strcmp(keyC, beside_c) && has_folder(storeA, "test.gamma"));
        /* A map pinning the first build carries exactly that build; its rows are unchanged. */
        pinned = map_naming(BASE_MAP, sizeof BASE_MAP - 1, pin_rows, &pl);
        CHECK(pinned && strstr(pinned, rc.bundle));
        CHECK(pinned && modexport_write(pinned, pl, zip3, &st, cause, sizeof cause));
        if (!pinned || !st.bundles) fprintf(stderr, "pinned export: %s\n", cause);
        CHECK(st.bundles == 2);
        {
            size_t zl = 0;
            unsigned char *z = slurp(zip3, &zl);
            CHECK(z && entry_data(z, zl, "mods/test.gamma/mod.json") && count_entries_with(z, zl, "mods/test.gamma@") == 0);
            free(z);
        }
        /* Imported on a second store, it round-trips: the same hashes, the map as written. */
        CHECK(fresh_store(storeD));
        CHECK(sh_modserve_index_build(storeD) == 0);
        text = modexport_read(zip3, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "pinned import: %s\n", cause);
        CHECK(text && st.bundles_installed == 2);
        memset(&got, 0, sizeof got);
        if (text) sh_modmanifest_read(text, len, &got, &rejected);
        CHECK(got.count == 2 && rejected == 0);
        CHECK(got.count == 2 && (!strcmp(got.rows[0].bundle, rc.bundle) || !strcmp(got.rows[1].bundle, rc.bundle)));
        CHECK(text && !strstr(text, rc2.bundle) && pinned && len == pl && !memcmp(text, pinned, pl));
        CHECK(sh_modserve_bundle_state(rc.bundle, NULL, 0) == 1 && sh_modserve_bundle_state(rc2.bundle, NULL, 0) == 0);
        sh_modmanifest_list_free(&got);
        free(text);
        if (pinned) HeapFree(GetProcessHeap(), 0, pinned);
        /* The second build into the same second store lands beside the first. */
        pinned = map_naming(BASE_MAP, sizeof BASE_MAP - 1, (const sh_modmanifest_row *[]){ &rc2, NULL }, &pl);
        CHECK(sh_ms_set_root(storeA));
        CHECK(pinned && modexport_write(pinned, pl, zip3, &st, cause, sizeof cause) && st.bundles == 1);
        CHECK(sh_ms_set_root(storeD) && sh_modserve_index_build(storeD) >= 0);
        text = modexport_read(zip3, &len, &st, cause, sizeof cause);
        CHECK(text && st.bundles_installed == 1 && has_folder(storeD, "test.gamma") && has_folder(storeD, beside_c));
        CHECK(sh_modserve_bundle_state(rc.bundle, NULL, 0) == 1 && sh_modserve_bundle_state(rc2.bundle, NULL, 0) == 1);
        free(text);
        if (pinned) HeapFree(GetProcessHeap(), 0, pinned);
        /* Two rows naming two builds of one key: two folders. */
        CHECK(fresh_store(storeA));
        CHECK(install(dir, keyC, &rc2) && !strcmp(keyC, beside_c));
        two_builds[1] = &rc2;
        pinned = map_naming(BASE_MAP, sizeof BASE_MAP - 1, two_builds, &pl);
        CHECK(pinned && modexport_write(pinned, pl, zip3, &st, cause, sizeof cause) && st.bundles == 2 &&
              st.entries == 1 + 2 * (1 + 1));
        if (pinned) HeapFree(GetProcessHeap(), 0, pinned);

        /* The folder's latest build failed after its files were edited: the
         * declared build is carried from its last good build snapshot. */
        pinned = map_naming(BASE_MAP, sizeof BASE_MAP - 1, only_c, &pl);
        _snprintf_s(gfolder, sizeof gfolder, _TRUNCATE, "%s\\test.gamma", storeA);
        mt_write(gfolder, "assets\\generated\\image\\mods\\test.gamma\\c.bimage", "edited, not built");
        sh_ms_note_build_failure(gfolder, "test: the edit does not build");
        CHECK(sh_ms_snapshot_of(gfolder, snapdir, sizeof snapdir));
        CHECK(pinned && modexport_write(pinned, pl, zip3, &st, cause, sizeof cause) && st.bundles == 1);
        if (!st.bundles) fprintf(stderr, "snapshot export: %s\n", cause);
        CHECK(fresh_store(storeD2));
        text = modexport_read(zip3, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "snapshot import: %s\n", cause);
        CHECK(text && st.bundles_installed == 1 && sh_modserve_bundle_state(rc.bundle, NULL, 0) == 1);
        free(text);
        /* The folder is gone and only its snapshot holds the build. */
        CHECK(sh_ms_set_root(storeA));
        mt_remove_tree(gfolder);
        CHECK(mt_exists(snapdir));
        CHECK(pinned && modexport_write(pinned, pl, zip3, &st, cause, sizeof cause) && st.bundles == 1);
        if (!st.bundles) fprintf(stderr, "snapshot-only export: %s\n", cause);
        {
            size_t zl = 0;
            unsigned char *z = slurp(zip3, &zl);
            CHECK(z && entry_data(z, zl, "mods/test.gamma/mod.json"));
            free(z);
        }
        CHECK(fresh_store(storeD3));
        text = modexport_read(zip3, &len, &st, cause, sizeof cause);
        CHECK(text && st.bundles_installed == 1 && has_folder(storeD3, "test.gamma") &&
              sh_modserve_bundle_state(rc.bundle, NULL, 0) == 1);
        free(text);
        if (pinned) HeapFree(GetProcessHeap(), 0, pinned);

        /* Rebuilt in place by its author: the earlier hash is an alias, the
         * export carries the build this machine serves and names it. */
        CHECK(fresh_store(storeP));
        make_mod(dir, "test.gamma", C_FILES, C_BYTES);
        CHECK(install(dir, keyC, &rc) && !strcmp(keyC, "test.gamma"));
        _snprintf_s(gfolder, sizeof gfolder, _TRUNCATE, "%s\\test.gamma", storeP);
        mt_write(gfolder, "assets\\generated\\image\\mods\\test.gamma\\c.bimage", C3_BYTES[0]);
        CHECK(install(gfolder, keyC, &rc3) && strcmp(rc3.bundle, rc.bundle));
        pinned = map_naming(BASE_MAP, sizeof BASE_MAP - 1, only_c, &pl);
        CHECK(pinned && modexport_write(pinned, pl, zip3, &st, cause, sizeof cause) && st.bundles == 1);
        CHECK(fresh_store(storeD4));
        text = modexport_read(zip3, &len, &st, cause, sizeof cause);
        CHECK(text && strstr(text, rc3.bundle) && !strstr(text, rc.bundle));
        free(text);
        if (pinned) HeapFree(GetProcessHeap(), 0, pinned);
    }

    /* ---- a prefab's export: <name>.json and its mods' folders ---- */
    {
        static const char PREFAB[] =
            "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/test.alpha/thing\"}}],"
            "\"grabDistance\":256,\"~type\":\"idSnapEntityPrefab\",\n\"snapmapPlus\":{\"format\":1,\"description\":\"\","
            "\"tags\":[],\"mods\":[]}}";
        static const char MAPJSON[] = "{\"entities\":[],\"~type\":\"idSnapMap\"}";
        char pz[MAX_PATH * 2], pz2[MAX_PATH * 2], storeF[MAX_PATH * 2], storeG[MAX_PATH * 2], ex[MAX_PATH * 2];
        char pj[MAX_PATH * 2], name[160];
        const sh_modmanifest_row *only_a[] = { &ra, NULL };
        sh_modmanifest_row ghost = rb;
        const sh_modmanifest_row *a_and_ghost[] = { &ra, &ghost, NULL };
        size_t zl = 0, len = 0, at;
        unsigned char *z;
        char *text = NULL;
        _snprintf_s(pz, sizeof pz, _TRUNCATE, "%s\\Door.zip", tmp);
        _snprintf_s(pz2, sizeof pz2, _TRUNCATE, "%s\\Door2.zip", tmp);
        _snprintf_s(storeF, sizeof storeF, _TRUNCATE, "%s\\storeF", tmp);
        _snprintf_s(storeG, sizeof storeG, _TRUNCATE, "%s\\storeG", tmp);
        _snprintf_s(ex, sizeof ex, _TRUNCATE, "%s\\extracted export", tmp);
        strcpy_s(ghost.bundle, sizeof ghost.bundle, "5555555555555555555555555555555555555555555555555555555555555555");
        CHECK(fresh_store(storeA));
        memset(&rows, 0, sizeof rows);
        CHECK(sh_modmanifest_list_add(&rows, &ra));
        CHECK(modexport_write_prefab("Door", PREFAB, sizeof PREFAB - 1, &rows, NULL, NULL, pz, &st, cause, sizeof cause));
        CHECK(st.bundles == 1 && st.entries == 1 + 1 + 2);
        z = slurp(pz, &zl);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.alpha\\mod.json", storeA);
        CHECK(z && entry_is_file(z, zl, "mods/test.alpha/mod.json", path));
        at = z ? entry_data(z, zl, "Door.json") : 0;
        CHECK(at && entry_size(z, at, "Door.json") == sizeof PREFAB - 1 && !memcmp(z + at, PREFAB, sizeof PREFAB - 1));
        CHECK(z && count_entries_with(z, zl, "export.json") == 0 && count_entries_with(z, zl, ".meta.json") == 0 &&
              count_entries_with(z, zl, "bundle") == 0);
        free(z);
        /* The text callback sees the carried rows. */
        g_text_for_calls = 0;
        CHECK(modexport_write_prefab("Door", PREFAB, sizeof PREFAB - 1, &rows, text_for, (void *)MAPJSON, pz2, &st, cause, sizeof cause));
        CHECK(g_text_for_calls == 1 && g_text_for_rows == 1 && g_text_for_moved == 0);
        z = slurp(pz2, &zl);
        CHECK(z && entry_data(z, zl, "Door.json") && !memcmp(z + entry_data(z, zl, "Door.json"), MAPJSON, sizeof MAPJSON - 1));
        free(z);
        sh_modmanifest_list_free(&rows);
        /* No mods: still one ZIP, the prefab alone. */
        CHECK(modexport_write_prefab("Crate", PREFAB, sizeof PREFAB - 1, NULL, NULL, NULL, pz2, &st, cause, sizeof cause) &&
              st.bundles == 0 && st.entries == 1);
        CHECK(!modexport_write_prefab("a/b", PREFAB, sizeof PREFAB - 1, NULL, NULL, NULL, pz2, &st, cause, sizeof cause) &&
              strstr(cause, "cannot name a file"));

        /* Each importer refuses the other's ZIP. */
        CHECK(modexport_read(pz, &len, &st, cause, sizeof cause) == NULL && !strcmp(cause, "this is a prefab's export, not a map's"));
        CHECK(!modexport_probe(pz, cause, sizeof cause));
        CHECK(!modexport_read_prefab(zip, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
              !strcmp(cause, "this is a map's export, not a prefab's"));
        {
            /* A module prefab (an idSnapMap with a block) is a prefab. */
            static const char MODP[] = "{\"entities\":[],\"~type\":\"idSnapMap\",\n\"snapmapPlus\":{\"format\":1,\"mods\":[]}}";
            CHECK(modexport_write_prefab("Hall", MODP, sizeof MODP - 1, NULL, NULL, NULL, pz2, &st, cause, sizeof cause));
            CHECK(modexport_read(pz2, &len, &st, cause, sizeof cause) == NULL && strstr(cause, "prefab's export, not a map's"));
            CHECK(modexport_read_prefab(pz2, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
                  !strcmp(name, "Hall"));
            free(text); text = NULL;
        }

        /* Read into an empty store: the folder the rows name installs, the text comes back. */
        CHECK(fresh_store(storeF));
        CHECK(sh_modserve_index_build(storeF) == 0);
        CHECK(modexport_read_prefab(pz, rows_named, (void *)a_and_ghost, &text, &len, name, sizeof name, &st, cause, sizeof cause));
        CHECK(text && len == sizeof PREFAB - 1 && !memcmp(text, PREFAB, len) && !strcmp(name, "Door"));
        CHECK(st.bundles == 1 && st.bundles_installed == 1 && st.bundles_absent == 1);
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1 && has_folder(storeF, "test.alpha"));
        free(text); text = NULL;
        /* A carried folder no row names is left out. */
        CHECK(fresh_store(storeG));
        CHECK(sh_modserve_index_build(storeG) == 0);
        {
            const sh_modmanifest_row *none[] = { NULL };
            CHECK(modexport_read_prefab(pz, rows_named, (void *)none, &text, &len, name, sizeof name, &st, cause, sizeof cause));
            CHECK(st.bundles == 0 && st.bundles_installed == 0 && sh_modserve_bundle_state(ra.bundle, NULL, 0) == 0);
            CHECK(folder_count(storeG) == 0);
            free(text); text = NULL;
        }

        /* An extracted export: the .json's folder is the export. */
        CHECK(mt_unzip_stored(pz, ex) == 4);
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door.json", ex);
        mt_write(ex, "mods\\other.mod\\notes.txt", "a stray file in a folder without mod.json");   /* not a mod */
        CHECK(modexport_read_prefab(pj, rows_named, (void *)only_a, &text, &len, name, sizeof name, &st, cause, sizeof cause));
        if (!text) fprintf(stderr, "folder import: %s\n", cause);
        CHECK(text && len == sizeof PREFAB - 1 && !memcmp(text, PREFAB, len) && !strcmp(name, "Door"));
        CHECK(st.bundles == 1 && st.bundles_installed == 1 && sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1);
        CHECK(has_folder(storeG, "test.alpha") && folder_count(storeG) == 1);
        free(text); text = NULL;
        /* An extracted export of the retired form is refused too. */
        mt_write(ex, "mods\\test.alpha@1.0.0+1\\bundle.canonical.json", "{\"id\":\"test.alpha\"}");
        CHECK(!modexport_read_prefab(pj, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
              !strcmp(cause, MODEXPORT_RETIRED_PREFAB));
        CHECK(!strcmp(MODEXPORT_RETIRED_PREFAB, "this export was made by an earlier release; export it again"));
        /* A plain prefab file without mods/ beside it imports alone. */
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Plain.json", tmp);
        CHECK(spit(pj, PREFAB, sizeof PREFAB - 1));
        CHECK(modexport_read_prefab(pj, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
              !strcmp(name, "Plain") && st.bundles == 0);
        free(text); text = NULL;
        /* A map's JSON is not a prefab; neither is an idSnapMap without a block. */
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Map.json", tmp);
        CHECK(spit(pj, MAPJSON, sizeof MAPJSON - 1));
        CHECK(!modexport_read_prefab(pj, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
              strstr(cause, "is a map, not a prefab") && !name[0]);
        CHECK(spit(pj, "{\"~type\":\"idDeclEntityDef\"}", 27));
        CHECK(!modexport_read_prefab(pj, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
              strstr(cause, "is not a prefab"));
        CHECK(spit(pj, "[1,2]", 5));
        CHECK(!modexport_read_prefab(pj, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
              strstr(cause, "not prefab JSON"));
        /* Two prefabs in one ZIP, and a prefab named "export". */
        {
            static const char *const TWO_N[] = { "A.json", "B.json" };
            static const char *const TWO_D[] = { "{\"~type\":\"idSnapEntityPrefab\"}", "{\"~type\":\"idSnapEntityPrefab\"}" };
            static const char *const EXP_N[] = { "export.json" };
            static const char *const EXP_D[] = { "{\"entities\":[],\"~type\":\"idSnapEntityPrefab\"}" };
            CHECK(mt_zip_stored(pz2, 2, TWO_N, TWO_D));
            CHECK(!modexport_read_prefab(pz2, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
                  strstr(cause, "more than one prefab"));
            CHECK(mt_zip_stored(pz2, 1, EXP_N, EXP_D));
            CHECK(modexport_read_prefab(pz2, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
                  !strcmp(name, "export"));
            free(text); text = NULL;
        }
        CHECK(sh_ms_set_root(storeA));
    }

    /* ---- a generated key and a map an earlier build saved travel in a ZIP ---- */
    {
        /* A hand-made folder (no id) is installed under its generated key; a
         * map saved by beta.24 or earlier names it in a legacy smpmod1 string
         * variable. The export carries the folder under its store name; the
         * import installs it under the key whatever the carried folder is
         * called, and hands the load the text with its legacy row, which the
         * parse reads and removes (the next save writes the member). */
        static const char *const S_FILES[] = { "assets/generated/image/mods/probe/p.bimage", NULL };
        static const char *const S_BYTES[] = { "probe bytes", NULL };
        char storeS[MAX_PATH * 2], storeS2[MAX_PATH * 2], storeS3[MAX_PATH * 2], zipS[MAX_PATH * 2], sdir[MAX_PATH * 2];
        char skey[SH_MS_KEY_MAX + 1], entry[MAX_PATH], other[SH_MS_KEY_MAX + 1], esc[1024], legacy[4096];
        sh_modmanifest_row rs;
        char *value, *text, *loaded;
        size_t vl = 0, len = 0, ll = 0, i, n = 0;
        _snprintf_s(storeS, sizeof storeS, _TRUNCATE, "%s\\storeS", tmp);
        _snprintf_s(storeS2, sizeof storeS2, _TRUNCATE, "%s\\storeS2", tmp);
        _snprintf_s(storeS3, sizeof storeS3, _TRUNCATE, "%s\\storeS3", tmp);
        _snprintf_s(zipS, sizeof zipS, _TRUNCATE, "%s\\legacy salted.zip", tmp);
        _snprintf_s(sdir, sizeof sdir, _TRUNCATE, "%s\\probe", src);
        CHECK(fresh_store(storeS));
        mt_write(sdir, "mod.json", "{\"name\":\"Probe\",\"version\":\"1.0.0\"}");
        mt_write(sdir, "assets\\generated\\image\\mods\\probe\\p.bimage", S_BYTES[0]);
        (void)S_FILES;
        CHECK(install(sdir, skey, &rs) && !strncmp(skey, "probe_", 6) && !strcmp(rs.id, skey));
        CHECK(has_folder(storeS, skey) && sh_modmanifest_valid_id(rs.id));
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "generated/image/mods/%s/p.bimage", skey);
        CHECK(sh_modmanifest_row_add_name(&rs, entry));
        value = sh_modmanifest_format_value(&rs, &vl);
        CHECK(value != NULL);
        for (i = 0; value && i < vl && n + 3 < sizeof esc; i++) {
            if (value[i] == '"' || value[i] == '\\') esc[n++] = '\\';
            esc[n++] = value[i];
        }
        esc[n] = 0;
        free(value);
        _snprintf_s(legacy, sizeof legacy, _TRUNCATE,
                    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/%s/thing\",\"name\":\"\"}}],"
                    "\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":[" VAR("door_label", "Open") ","
                    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"},"
                    "\"name\":\"smpmod1.0.1\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"%s\",\"~type\":\"snapVarString_t\"}]},"
                    "\"name\":\"idSnapMap\",\"~type\":\"idSnapMap\"}", skey, esc);
        CHECK(modexport_write(legacy, strlen(legacy), zipS, &st, cause, sizeof cause) && st.bundles == 1);
        {
            size_t zl = 0;
            unsigned char *z = slurp(zipS, &zl);
            _snprintf_s(entry, sizeof entry, _TRUNCATE, "mods/%s/mod.json", skey);
            CHECK(z && entry_data(z, zl, entry) && entry_data(z, zl, "legacy salted.json"));
            /* The map text travels as the earlier build wrote it. */
            CHECK(z && entry_size(z, entry_data(z, zl, "legacy salted.json"), "legacy salted.json") == strlen(legacy));
            free(z);
        }
        /* Import into an empty store: installed under the key, the row served. */
        CHECK(fresh_store(storeS2));
        text = modexport_read(zipS, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "legacy salted import: %s\n", cause);
        CHECK(text && len == strlen(legacy) && !memcmp(text, legacy, len) && st.bundles_installed == 1);
        CHECK(has_folder(storeS2, skey) && folder_count(storeS2) == 1 && sh_modserve_bundle_state(rs.bundle, NULL, 0) == 1);
        {
            sh_modmanifest_list got;
            unsigned rej = 0;
            char why[256] = "";
            memset(&got, 0, sizeof got);
            CHECK(text && sh_modmanifest_read(text, len, &got, &rej) == 1 && rej == 0 && !strcmp(got.rows[0].id, skey) &&
                  !strcmp(got.rows[0].bundle, rs.bundle));
            CHECK(got.count == 1 && sh_modserve_row_served(&got.rows[0], why, sizeof why) == 1);
            sh_modmanifest_list_free(&got);
        }
        /* The load's parse reads and removes the legacy row. */
        loaded = text ? sh_modmanifest_prepare_load(text, len, &ll) : NULL;
        CHECK(loaded && !strstr(loaded, "smpmod1.") && strstr(loaded, "door_label"));
        if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
        free(text);
        /* The carried folder under another name of the same length still
         * installs under the key. */
        strcpy_s(other, sizeof other, skey);
        other[0] = 'q';
        {
            size_t zl = 0;
            unsigned char *z = slurp(zipS, &zl);
            char prefix[MAX_PATH];
            _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "mods/%s/", skey);
            CHECK(z && patch_names(z, zl, prefix, 5, 'q') == 4 && spit(zipS, z, zl));
            free(z);
        }
        CHECK(fresh_store(storeS3));
        text = modexport_read(zipS, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "renamed carried folder import: %s\n", cause);
        CHECK(text && st.bundles_installed == 1 && has_folder(storeS3, skey) && !has_folder(storeS3, other) &&
              folder_count(storeS3) == 1);
        free(text);
        sh_modmanifest_row_free(&rs);
        CHECK(sh_ms_set_root(storeA));
    }

    /* ---- a carried folder built under an earlier builder's rules ---- */
    {
        /* Its block records the hash those rules gave (here: another one) and
         * a lower "rules" number (or none). It is rebuilt under today's rules
         * and installed under the hash they give; the map's row keeps naming
         * the recorded hash, which the other-build rule then answers. The
         * same block with today's "rules" is an edited mod.json: left out. */
        static const char OLD_HASH[] = "3333333333333333333333333333333333333333333333333333333333333333";
        char storeR[MAX_PATH * 2], storeR2[MAX_PATH * 2], storeR3[MAX_PATH * 2], zipR[MAX_PATH * 2];
        char rdir[MAX_PATH * 2], rkey[SH_MS_KEY_MAX + 1], why[300], mpath[MAX_PATH * 2];
        sh_modmanifest_row rr, old;
        const sh_modmanifest_row *r1[] = { &old, NULL };
        sh_modserve_bundle_ref other;
        sh_mb_bundle nb;
        size_t len = 0, rl = 0, ml = 0;
        char *rmap = NULL, *text, *modjson, *at;
        int pass;
        _snprintf_s(storeR, sizeof storeR, _TRUNCATE, "%s\\storeR", tmp);
        _snprintf_s(storeR2, sizeof storeR2, _TRUNCATE, "%s\\storeR2", tmp);
        _snprintf_s(storeR3, sizeof storeR3, _TRUNCATE, "%s\\storeR3", tmp);
        _snprintf_s(zipR, sizeof zipR, _TRUNCATE, "%s\\older rules.zip", tmp);
        _snprintf_s(rdir, sizeof rdir, _TRUNCATE, "%s\\older", src);
        CHECK(fresh_store(storeR));
        mt_write(rdir, "mod.json", "{\"id\":\"test.older\",\"version\":\"1.0.0\",\"name\":\"test.older\"}");
        mt_write(rdir, "assets\\generated\\image\\mods\\test.older\\o.bimage", "older bytes");
        CHECK(install(rdir, rkey, &rr) && !strcmp(rkey, "test.older"));
        _snprintf_s(mpath, sizeof mpath, _TRUNCATE, "%s\\test.older\\mod.json", storeR);
        modjson = (char *)slurp(mpath, &ml);
        /* The earlier build's mod.json: OLD_HASH under rules 1. */
        at = modjson ? strstr(modjson, rr.bundle) : NULL;
        CHECK(at != NULL);
        if (at) memcpy(at, OLD_HASH, 64);
        at = modjson ? strstr(modjson, "\"rules\": 2") : NULL;
        CHECK(at != NULL);
        old = rr;
        strcpy_s(old.bundle, sizeof old.bundle, OLD_HASH);
        rmap = map_naming(BASE_MAP, sizeof BASE_MAP - 1, r1, &rl);
        CHECK(rmap != NULL);
        for (pass = 0; pass < 2 && at && rmap; pass++) {
            const char *names[] = { "older rules.json", "mods/test.older/mod.json",
                                    "mods/test.older/assets/generated/image/mods/test.older/o.bimage" };
            const char *datas[] = { rmap, modjson, "older bytes" };
            at[9] = pass ? '2' : '1';
            CHECK(mt_zip_stored(zipR, 3, names, datas));
            CHECK(fresh_store(pass ? storeR3 : storeR2));
            text = modexport_read(zipR, &len, &st, cause, sizeof cause);
            if (!text) fprintf(stderr, "earlier rules import: %s\n", cause);
            if (pass) {
                /* Today's "rules" with another hash is a mod.json edited by hand. */
                CHECK(text && st.bundles_installed == 0 && st.bundles_absent == 1 && folder_count(storeR3) == 0);
                free(text);
                break;
            }
            CHECK(text && st.bundles == 1 && st.bundles_installed == 1 && st.bundles_absent == 0);
            /* Today's rules number it build 2 (the recorded build's hash is not
             * theirs), as the author's own store does at its rules rebuild. */
            _snprintf_s(mpath, sizeof mpath, _TRUNCATE, "%s\\test.older", storeR2);
            memset(&nb, 0, sizeof nb);
            CHECK(has_folder(storeR2, "test.older") && folder_count(storeR2) == 1 && sh_mb_load_folder(mpath, &nb) &&
                  nb.build == 2 && strcmp(nb.hash, OLD_HASH) && sh_modserve_bundle_state(nb.hash, NULL, 0) == 1);
            {
                sh_modmanifest_list got;
                unsigned rej = 0;
                memset(&got, 0, sizeof got);
                CHECK(text && sh_modmanifest_read(text, len, &got, &rej) == 1 && !strcmp(got.rows[0].bundle, OLD_HASH));
                /* The pinned row is another build of an installed key. */
                CHECK(got.count == 1 && sh_modserve_row_served(&got.rows[0], why, sizeof why) == 0 &&
                      sh_modserve_row_other_build(&got.rows[0], &other) && !strcmp(other.hash, nb.hash));
                sh_modmanifest_list_free(&got);
            }
            free(text);
            /* Again into the same store: today's build is there, nothing is installed. */
            text = modexport_read(zipR, &len, &st, cause, sizeof cause);
            CHECK(text && st.bundles_installed == 0 && st.bundles_present == 1 && folder_count(storeR2) == 1);
            free(text);
            sh_mb_free(&nb);
        }
        free(modjson);
        if (rmap) HeapFree(GetProcessHeap(), 0, rmap);
        CHECK(sh_ms_set_root(storeA));
    }

    /* ---- beta.20's map export: export.json and each bundle's canonical manifest ---- */
    {
        /* The bundle is installed from its canonical manifest as a download
         * is, so it keeps the hash the map pins; a file the ZIP stores once
         * for two paths fills both; a damaged file installs nothing. */
        static const char *const T_FILES[] = { "assets/generated/image/mods/test.twenty/a.bimage",
                                               "assets/generated/image/mods/test.twenty/b.bimage", NULL };
        static const char *const T_BYTES[] = { "twenty bytes", "twenty bytes", NULL };
        char storeT[MAX_PATH * 2], storeT2[MAX_PATH * 2], storeT3[MAX_PATH * 2], zipT[MAX_PATH * 2], tdir[MAX_PATH * 2], tkey[SH_MS_KEY_MAX + 1];
        char export_json[512], why[256];
        sh_modmanifest_row rt;
        const sh_modmanifest_row *t1[] = { &rt, NULL };
        sh_mb_bundle tb;
        size_t len = 0, tl = 0;
        char *tmap, *text, *canon = NULL;
        int round;
        _snprintf_s(storeT, sizeof storeT, _TRUNCATE, "%s\\storeT", tmp);
        _snprintf_s(storeT2, sizeof storeT2, _TRUNCATE, "%s\\storeT2", tmp);
        _snprintf_s(storeT3, sizeof storeT3, _TRUNCATE, "%s\\storeT3", tmp);
        _snprintf_s(zipT, sizeof zipT, _TRUNCATE, "%s\\beta20.zip", tmp);
        _snprintf_s(tdir, sizeof tdir, _TRUNCATE, "%s\\twenty", src);
        CHECK(fresh_store(storeT));
        make_mod(tdir, "test.twenty", T_FILES, T_BYTES);
        CHECK(install(tdir, tkey, &rt) && !strcmp(tkey, "test.twenty"));
        _snprintf_s(tdir, sizeof tdir, _TRUNCATE, "%s\\test.twenty", storeT);
        memset(&tb, 0, sizeof tb);
        CHECK(sh_mb_load_folder(tdir, &tb) && tb.canonical && !strcmp(tb.hash, rt.bundle));
        if (tb.canonical) canon = _strdup(tb.canonical);
        sh_mb_free(&tb);
        tmap = map_naming(BASE_MAP, sizeof BASE_MAP - 1, t1, &tl);
        _snprintf_s(export_json, sizeof export_json, _TRUNCATE,
                    "{\"bundles\":[{\"build\":1,\"bundle\":\"%s\",\"folder\":\"mods/test.twenty@1.0.0+1\",\"id\":\"test.twenty\","
                    "\"version\":\"1.0.0\"}],\"format\":1,\"map\":\"map.rawmap.json\"}", rt.bundle);
        for (round = 0; round < 2 && tmap && canon; round++) {
            const char *names[] = { "map.rawmap.json", "export.json", "mods/test.twenty@1.0.0+1/bundle.canonical.json",
                                    "mods/test.twenty@1.0.0+1/assets/generated/image/mods/test.twenty/a.bimage" };
            const char *datas[] = { tmap, export_json, canon, round ? "twenty bytez" : "twenty bytes" };
            CHECK(mt_zip_stored(zipT, 4, names, datas));
            CHECK(fresh_store(round ? storeT3 : storeT2));
            text = modexport_read(zipT, &len, &st, cause, sizeof cause);
            if (round) {
                CHECK(text == NULL && strstr(cause, "test.twenty@1.0.0+1 in the export does not install") && folder_count(storeT3) == 0);
                free(text);
                break;
            }
            if (!text) fprintf(stderr, "beta.20 import: %s\n", cause);
            CHECK(text && len == tl && !memcmp(text, tmap, len) && st.bundles == 1 && st.bundles_installed == 1);
            CHECK(folder_count(storeT2) == 1 && sh_modserve_bundle_state(rt.bundle, NULL, 0) == 1);
            CHECK(sh_modserve_row_served(&rt, why, sizeof why) == 1);
            free(text);
            /* Again: installed already, nothing written. */
            text = modexport_read(zipT, &len, &st, cause, sizeof cause);
            CHECK(text && st.bundles_installed == 0 && st.bundles_present == 1 && folder_count(storeT2) == 1);
            free(text);
        }
        free(canon);
        if (tmap) HeapFree(GetProcessHeap(), 0, tmap);
        CHECK(sh_ms_set_root(storeA));
    }
    HeapFree(GetProcessHeap(), 0, map);
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    printf("modexport_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
