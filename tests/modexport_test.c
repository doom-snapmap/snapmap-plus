/* modexport_test -- the rawmap ZIP export: a map whose smpmod1 rows name
 * two installed bundles that share a file is written to one ZIP (the
 * shared blob stored once, same bytes on a second write), probed, and
 * imported into an empty store (every bundle installed and verified, the
 * shared file restored from the other bundle's entry, the serving index
 * holding both, the map text returned unchanged); a second import installs
 * nothing; a damaged entry, a mislabelled manifest and a map naming an
 * uninstalled bundle are refused; a map pinning an earlier build of a folder
 * exports the current build and names it in the exported map; the rawmap
 * hooks take ANSI paths and return process-heap text; an export whose map
 * names a bundle it does not carry imports what it carries and counts the
 * rest as absent, which is what the rawmap gate then refuses or offers to
 * download; importing an export whose bundles are installed rewrites
 * nothing. A prefab's export carries <name>.json and its bundles with no
 * index (also without mods), each importer refuses the other's ZIP, the
 * prefab importer installs only the carried bundles the prefab's rows name,
 * reads an extracted export folder and a lone .json, and refuses a map's
 * JSON, a non-prefab, the earlier export.json layout and two prefabs in one
 * ZIP. Pure file work: no network.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modexport.h"
#include "../src/backend/modsync.h"
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

static void make_mod(const char *dir, const char *id, const char *const *files, const char *const *bytes)
{
    char text[512];
    int i;
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"1.0.0\",\"name\":\"%s\"}", id, id);
    mt_write(dir, "mod.json", text);
    for (i = 0; files[i]; i++) {
        char rel[MAX_PATH];
        char *c;
        strcpy_s(rel, sizeof rel, files[i]);
        for (c = rel; *c; c++) if (*c == '/') *c = '\\';
        mt_write(dir, rel, bytes[i]);
    }
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

static unsigned long crc32_of(const unsigned char *p, size_t n)
{
    unsigned long c = 0xFFFFFFFFul;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320ul ^ (c >> 1) : c >> 1;
    }
    return (c ^ 0xFFFFFFFFul) & 0xFFFFFFFFul;
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* Replace text of equal length inside a stored entry and fix its CRC in the
 * local header and the central directory, so only the content is wrong. */
static int patch_entry(unsigned char *zip, size_t len, const char *name, const char *from, const char *to)
{
    size_t nl = strlen(name), at = entry_data(zip, len, name), size, i;
    unsigned long crc;
    unsigned char *hit = NULL;
    if (!at || strlen(from) != strlen(to)) return 0;
    size = (size_t)zip[at - nl - 30 + 18] | ((size_t)zip[at - nl - 30 + 19] << 8) | ((size_t)zip[at - nl - 30 + 20] << 16);
    for (i = at; i + strlen(from) <= at + size && !hit; i++) if (memcmp(zip + i, from, strlen(from)) == 0) hit = zip + i;
    if (!hit) return 0;
    memcpy(hit, to, strlen(to));
    crc = crc32_of(zip + at, size);
    put32(zip + at - nl - 30 + 14, crc);
    for (i = 0; i + 46 + nl <= len; i++)
        if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 1 && zip[i + 3] == 2 &&
            (size_t)(zip[i + 28] | (zip[i + 29] << 8)) == nl && memcmp(zip + i + 46, name, nl) == 0) {
            put32(zip + i + 16, crc);
            return 1;
        }
    return 0;
}

static unsigned count_entries_named(const unsigned char *zip, size_t len, const char *suffix)
{
    unsigned n = 0;
    size_t i, sl = strlen(suffix);
    for (i = 0; i + 46 <= len; i++) {
        if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 1 && zip[i + 3] == 2) {
            size_t nl = (size_t)(zip[i + 28] | (zip[i + 29] << 8));
            if (nl >= sl && i + 46 + nl <= len && memcmp(zip + i + 46 + nl - sl, suffix, sl) == 0) n++;
        }
    }
    return n;
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

/* A ZIP of stored entries, for exports no release writes. */
static int zip_stored(const char *path, int n, const char *const *names, const char *const *datas)
{
    unsigned char buf[16384], *p = buf, *cd0;
    unsigned long offs[8];
    int i;
    for (i = 0; i < n; i++) {
        size_t nl = strlen(names[i]), dl = strlen(datas[i]);
        offs[i] = (unsigned long)(p - buf);
        put32(p, 0x04034b50ul); put16(p + 4, 20); put16(p + 6, 0); put16(p + 8, 0); put16(p + 10, 0); put16(p + 12, 0x21);
        put32(p + 14, crc32_of((const unsigned char *)datas[i], dl)); put32(p + 18, (unsigned long)dl); put32(p + 22, (unsigned long)dl);
        put16(p + 26, (unsigned)nl); put16(p + 28, 0);
        memcpy(p + 30, names[i], nl); memcpy(p + 30 + nl, datas[i], dl);
        p += 30 + nl + dl;
    }
    cd0 = p;
    for (i = 0; i < n; i++) {
        size_t nl = strlen(names[i]), dl = strlen(datas[i]);
        memset(p, 0, 46);
        put32(p, 0x02014b50ul); put16(p + 4, 20); put16(p + 6, 20); put16(p + 14, 0x21);
        put32(p + 16, crc32_of((const unsigned char *)datas[i], dl)); put32(p + 20, (unsigned long)dl); put32(p + 24, (unsigned long)dl);
        put16(p + 28, (unsigned)nl); put32(p + 42, offs[i]);
        memcpy(p + 46, names[i], nl);
        p += 46 + nl;
    }
    memset(p, 0, 22);
    put32(p, 0x06054b50ul); put16(p + 8, (unsigned)n); put16(p + 10, (unsigned)n);
    put32(p + 12, (unsigned long)(p - cd0)); put32(p + 16, (unsigned long)(cd0 - buf));
    p += 22;
    return spit(path, buf, (size_t)(p - buf));
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

static const char *const A_FILES[] = { "assets/generated/image/mods/test.alpha/a.bimage", "assets/generated/image/mods/test.alpha/shared.bimage", NULL };
static const char *const A_BYTES[] = { "alpha only bytes", "bytes both bundles carry", NULL };
static const char *const B_FILES[] = { "assets/generated/image/mods/test.beta/shared.bimage", "assets/generated/image/mods/test.beta/b.bimage", NULL };
static const char *const B_BYTES[] = { "bytes both bundles carry", "beta only bytes", NULL };
static const char *const C_FILES[] = { "assets/generated/image/mods/test.gamma/c.bimage", NULL };
static const char *const C_BYTES[] = { "gamma bytes", NULL };

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"

static const char BASE_MAP[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/test.alpha/thing\",\"name\":\"\"}}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" VAR("door_label", "Open") "]},"
    "\"name\":\"idSnapMap\"}";

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], src[MAX_PATH * 2], storeA[MAX_PATH * 2], storeB[MAX_PATH * 2], storeC[MAX_PATH * 2], dir[MAX_PATH * 2];
    char zip[MAX_PATH * 2], zip2[MAX_PATH * 2], bad[MAX_PATH * 2], plain[MAX_PATH * 2], cause[320];
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

    /* ---- the author's store and a map naming two bundles ---- */
    CHECK(sh_ms_set_root(storeA));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alpha", src); make_mod(dir, "test.alpha", A_FILES, A_BYTES);
    CHECK(install(dir, keyA, &ra));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\beta", src); make_mod(dir, "test.beta", B_FILES, B_BYTES);
    CHECK(install(dir, keyB, &rb));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gamma", src); make_mod(dir, "test.gamma", C_FILES, C_BYTES);
    CHECK(install(dir, keyC, &rc));
    memset(&rows, 0, sizeof rows);
    CHECK(sh_modmanifest_list_add(&rows, &ra) && sh_modmanifest_list_add(&rows, &rb));
    map = sh_modmanifest_write(BASE_MAP, sizeof BASE_MAP - 1, &rows, &map_len, err, sizeof err);
    CHECK(map != NULL && strstr(map, "smpmod1.0.2") && strstr(map, "smpmod1.1.2"));
    sh_modmanifest_list_free(&rows);

    /* ---- write ---- */
    CHECK(modexport_is_zip_path(zip) && modexport_is_zip_path("X.ZIP") && !modexport_is_zip_path("map.json") && !modexport_is_zip_path(".zip"));
    CHECK(modexport_write(map, map_len, zip, &st, cause, sizeof cause));
    if (g_echo) printf("    write: %s\n", cause);
    CHECK(st.bundles == 2 && st.blobs_written == 3 && st.blobs_deduped == 1);
    CHECK(st.entries == 2 + 2 * 2 + 3 && st.zip_bytes > 0 && st.map_bytes == map_len);
    {
        size_t zl = 0, zl2 = 0;
        unsigned char *z = slurp(zip, &zl), *z2;
        char name[MAX_PATH], zkA[SH_MS_KEY_MAX + 1], zkB[SH_MS_KEY_MAX + 1];
        /* The ZIP keeps its own folder names (id@version+build), whatever the
         * store folders are called. */
        _snprintf_s(zkA, sizeof zkA, _TRUNCATE, "%s@%s+1", ra.id, ra.version);
        _snprintf_s(zkB, sizeof zkB, _TRUNCATE, "%s@%s+1", rb.id, rb.version);
        CHECK(z && zl == st.zip_bytes && z[0] == 'P' && z[1] == 'K');
        /* Each bundle folder as in the store; the shared file only under alpha. */
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/bundle.canonical.json", zkA);
        CHECK(entry_data(z, zl, name) != 0);
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/assets/generated/image/mods/test.alpha/shared.bimage", zkA);
        CHECK(entry_data(z, zl, name) != 0);
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/assets/generated/image/mods/test.beta/shared.bimage", zkB);
        CHECK(entry_data(z, zl, name) == 0);
        CHECK(count_entries_named(z, zl, "/bundle.json") == 2 && count_entries_named(z, zl, "export.json") == 1);
        CHECK(entry_data(z, zl, "map.rawmap.json") != 0);
        /* The same content writes the same bytes. */
        CHECK(modexport_write(map, map_len, zip2, NULL, cause, sizeof cause));
        z2 = slurp(zip2, &zl2);
        CHECK(z2 && zl2 == zl && memcmp(z, z2, zl) == 0);
        free(z2);

        /* A damaged copy: one byte of a stored asset changed. */
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/assets/generated/image/mods/test.beta/b.bimage", zkB);
        {
            size_t at = entry_data(z, zl, name);
            CHECK(at != 0);
            if (at) z[at] ^= 0x20;
            CHECK(spit(bad, z, zl));
            if (at) z[at] ^= 0x20;
        }
        /* A mislabelled copy: export.json files beta under alpha's hash. */
        {
            char mis[MAX_PATH * 2], from[80], to[80];
            _snprintf_s(mis, sizeof mis, _TRUNCATE, "%s\\mislabelled.zip", tmp);
            _snprintf_s(from, sizeof from, _TRUNCATE, "\"%s\",\"folder\":\"mods/test.beta", rb.bundle);
            _snprintf_s(to, sizeof to, _TRUNCATE, "\"%s\",\"folder\":\"mods/test.beta", ra.bundle);
            CHECK(patch_entry(z, zl, "export.json", from, to));
            CHECK(spit(mis, z, zl));
            CHECK(sh_ms_set_root(storeC));
            {
                size_t ml = 0;
                char *mt = modexport_read(mis, &ml, NULL, cause, sizeof cause);
                CHECK(mt == NULL && strstr(cause, "does not hash to the bundle") != NULL);
                free(mt);
            }
            CHECK(sh_ms_set_root(storeA));
        }
        free(z);
    }
    CHECK(modexport_probe(zip, cause, sizeof cause));
    {
        /* A map naming a bundle the store lacks is not exported. */
        sh_modmanifest_row ghost = ra;
        char *gm;
        size_t gl = 0;
        memset(&rows, 0, sizeof rows);
        strcpy_s(ghost.bundle, sizeof ghost.bundle, "2222222222222222222222222222222222222222222222222222222222222222");
        CHECK(sh_modmanifest_list_add(&rows, &ghost));
        gm = sh_modmanifest_write(BASE_MAP, sizeof BASE_MAP - 1, &rows, &gl, err, sizeof err);
        sh_modmanifest_list_free(&rows);
        CHECK(gm && !modexport_write(gm, gl, zip2, &st, cause, sizeof cause) && strstr(cause, "not installed") != NULL);
        if (gm) HeapFree(GetProcessHeap(), 0, gm);
    }

    /* ---- import into an empty store ---- */
    CHECK(sh_ms_set_root(storeB));
    CHECK(sh_modserve_index_build(storeB) == 0);
    {
        size_t len = 0;
        char *text = modexport_read(zip, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "import: %s\n", cause);
        CHECK(text && len == map_len && memcmp(text, map, len) == 0);
        CHECK(st.bundles == 2 && st.bundles_installed == 2 && st.bundles_present == 0 && st.blobs_written == 3);
        free(text);
    }
    {
        sh_ms_verify_stats vs;
        char vcause[SH_MB_CAUSE_MAX], path[MAX_PATH * 2];
        CHECK(sh_ms_verify(keyA, &vs, vcause, sizeof vcause));
        CHECK(sh_ms_verify(keyB, &vs, vcause, sizeof vcause) && vs.files == 2);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets\\generated\\image\\mods\\test.beta\\shared.bimage", storeB, keyB);
        CHECK(mt_exists(path));   /* restored from alpha's entry by its hash */
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1 && sh_modserve_bundle_state(rb.bundle, NULL, 0) == 1);
        CHECK(sh_modserve_bundle_state(rc.bundle, NULL, 0) == 0);
    }
    {
        size_t len = 0;
        char *text = modexport_read(zip, &len, &st, cause, sizeof cause);
        CHECK(text && st.bundles_installed == 0 && st.bundles_present == 2 && st.blobs_written == 0);
        CHECK(st.bundles_absent == 0);
        free(text);
    }
    {
        /* Importing an export whose bundles are installed rewrites nothing: the
         * folders' files keep their bytes and their write times. */
        char mj[MAX_PATH * 2], asset[MAX_PATH * 2];
        WIN32_FILE_ATTRIBUTE_DATA before_mj, before_asset, after_mj, after_asset;
        size_t l1 = 0, l2 = 0, len = 0;
        unsigned char *b1, *b2;
        char *text;
        _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\%s\\mod.json", storeB, keyA);
        _snprintf_s(asset, sizeof asset, _TRUNCATE, "%s\\%s\\assets\\generated\\image\\mods\\test.alpha\\a.bimage", storeB, keyA);
        CHECK(GetFileAttributesExA(mj, GetFileExInfoStandard, &before_mj));
        CHECK(GetFileAttributesExA(asset, GetFileExInfoStandard, &before_asset));
        b1 = slurp(mj, &l1);
        Sleep(20);
        text = modexport_read(zip, &len, &st, cause, sizeof cause);
        CHECK(text && st.bundles_installed == 0 && st.bundles_present == 2);
        free(text);
        b2 = slurp(mj, &l2);
        CHECK(b1 && b2 && l1 == l2 && memcmp(b1, b2, l1) == 0);
        CHECK(GetFileAttributesExA(mj, GetFileExInfoStandard, &after_mj));
        CHECK(GetFileAttributesExA(asset, GetFileExInfoStandard, &after_asset));
        CHECK(CompareFileTime(&before_mj.ftLastWriteTime, &after_mj.ftLastWriteTime) == 0);
        CHECK(CompareFileTime(&before_asset.ftLastWriteTime, &after_asset.ftLastWriteTime) == 0);
        free(b1); free(b2);
    }
    {
        /* An export whose map names a bundle it does not carry (here: beta's
         * row renamed to a build nobody has). Everything it carries installs and
         * the map text comes back; the absent bundle is counted, and the rawmap
         * gate refuses or offers it before the engine sees the map. */
        static const char GHOST[] = "3333333333333333333333333333333333333333333333333333333333333333";
        char part[MAX_PATH * 2], storeE[MAX_PATH * 2];
        size_t zl = 0, len = 0;
        unsigned char *z = slurp(zip, &zl);
        char *text;
        _snprintf_s(part, sizeof part, _TRUNCATE, "%s\\partial.zip", tmp);
        _snprintf_s(storeE, sizeof storeE, _TRUNCATE, "%s\\storeE", tmp);
        CHECK(z && patch_entry(z, zl, "map.rawmap.json", rb.bundle, GHOST));
        CHECK(z && spit(part, z, zl));
        free(z);
        CHECK(modexport_probe(part, cause, sizeof cause));
        CHECK(sh_ms_set_root(storeE));
        CHECK(sh_modserve_index_build(storeE) == 0);
        text = modexport_read(part, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "partial import: %s\n", cause);
        CHECK(text && strstr(text, GHOST) != NULL);
        CHECK(st.bundles == 2 && st.bundles_installed == 2 && st.bundles_absent == 1);
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1 && sh_modserve_bundle_state(GHOST, NULL, 0) == 0);
        free(text);
        CHECK(sh_ms_set_root(storeB));
        CHECK(sh_modserve_index_build(storeB) >= 0);   /* back to the store the next steps use */
    }

    /* ---- refusals on import ---- */
    CHECK(sh_ms_set_root(storeC));
    CHECK(sh_modserve_index_build(storeC) == 0);
    {
        size_t len = 0;
        char *text = modexport_read(bad, &len, &st, cause, sizeof cause);
        CHECK(text == NULL && strstr(cause, "checksum") != NULL);
        CHECK(sh_modserve_bundle_state(rb.bundle, NULL, 0) == 0);
    }
    {
        /* Not a ZIP at all. */
        size_t len = 0;
        CHECK(spit(plain, "{\"name\":\"idSnapMap\"}", 20));
        CHECK(modexport_read(plain, &len, &st, cause, sizeof cause) == NULL && strstr(cause, "ZIP") != NULL);
        CHECK(!modexport_probe(plain, cause, sizeof cause));
    }

    /* ---- the rawmap hooks: ANSI paths, process-heap text ---- */
    CHECK(sh_ms_set_root(storeA));
    CHECK(modexport_write_text(map, map_len, zip2) > 0);
    {
        /* The reporting writer names the bundles and, on refusal, the cause. */
        sh_modmanifest_row ghost = ra;
        unsigned bundles = 0;
        char *gm, why[320];
        size_t gl = 0;
        CHECK(modexport_write_text_report(map, map_len, zip2, &bundles, why, sizeof why) > 0 && bundles == 2 && !why[0]);
        memset(&rows, 0, sizeof rows);
        strcpy_s(ghost.bundle, sizeof ghost.bundle, "4444444444444444444444444444444444444444444444444444444444444444");
        CHECK(sh_modmanifest_list_add(&rows, &ghost));
        gm = sh_modmanifest_write(BASE_MAP, sizeof BASE_MAP - 1, &rows, &gl, err, sizeof err);
        sh_modmanifest_list_free(&rows);
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
        CHECK(sh_ms_set_root(storeC));
        heap = modexport_read_text(zip2, &len);
        CHECK(heap && len == map_len && memcmp(heap, map, len) == 0);
        if (heap) HeapFree(GetProcessHeap(), 0, heap);
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1);
        CHECK(modexport_read_text(plain, &len) == NULL);
    }

    /* ---- a map pinning an earlier build of a folder ---- */
    {
        static const char *const C2_BYTES[] = { "gamma bytes, edited", NULL };
        sh_modmanifest_row rc2;
        sh_modmanifest_list got;
        unsigned rejected = 0;
        char zip3[MAX_PATH * 2], storeD[MAX_PATH * 2], *pinned, *text;
        size_t pl = 0, len = 0;
        _snprintf_s(zip3, sizeof zip3, _TRUNCATE, "%s\\pinned.zip", tmp);
        _snprintf_s(storeD, sizeof storeD, _TRUNCATE, "%s\\storeD", tmp);
        CHECK(sh_ms_set_root(storeA));
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gamma", src);
        make_mod(dir, "test.gamma", C_FILES, C2_BYTES);
        CHECK(install(dir, keyC, &rc2) && strcmp(rc2.bundle, rc.bundle));   /* the same folder, build 2 */
        memset(&rows, 0, sizeof rows);
        CHECK(sh_modmanifest_list_add(&rows, &ra) && sh_modmanifest_list_add(&rows, &rc));
        pinned = sh_modmanifest_write(BASE_MAP, sizeof BASE_MAP - 1, &rows, &pl, err, sizeof err);
        sh_modmanifest_list_free(&rows);
        CHECK(pinned && strstr(pinned, rc.bundle));
        CHECK(pinned && modexport_write(pinned, pl, zip3, &st, cause, sizeof cause));
        if (!pinned || !st.bundles) fprintf(stderr, "pinned export: %s\n", cause);
        CHECK(st.bundles == 2);
        CHECK(sh_ms_set_root(storeD));
        CHECK(sh_modserve_index_build(storeD) == 0);
        text = modexport_read(zip3, &len, &st, cause, sizeof cause);
        if (!text) fprintf(stderr, "pinned import: %s\n", cause);
        CHECK(text && st.bundles_installed == 2);
        /* The exported map names the build the export carries. */
        memset(&got, 0, sizeof got);
        if (text) sh_modmanifest_read(text, len, &got, &rejected);
        CHECK(got.count == 2 && rejected == 0);
        CHECK(got.count == 2 && (!strcmp(got.rows[0].bundle, rc2.bundle) || !strcmp(got.rows[1].bundle, rc2.bundle)));
        CHECK(text && !strstr(text, rc.bundle));
        CHECK(sh_modserve_bundle_state(rc2.bundle, NULL, 0) == 1);
        sh_modmanifest_list_free(&got);
        free(text);
        if (pinned) HeapFree(GetProcessHeap(), 0, pinned);
        /* Two rows naming two builds of one folder become one row. */
        CHECK(sh_ms_set_root(storeA));
        memset(&rows, 0, sizeof rows);
        CHECK(sh_modmanifest_list_add(&rows, &rc) && sh_modmanifest_list_add(&rows, &rc2));
        pinned = sh_modmanifest_write(BASE_MAP, sizeof BASE_MAP - 1, &rows, &pl, err, sizeof err);
        sh_modmanifest_list_free(&rows);
        CHECK(pinned && modexport_write(pinned, pl, zip3, &st, cause, sizeof cause) && st.bundles == 1);
        if (pinned) HeapFree(GetProcessHeap(), 0, pinned);
    }

    /* ---- a prefab's export: <name>.json and its bundles, no index ---- */
    {
        static const char PREFAB[] =
            "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/test.alpha/thing\"}}],"
            "\"grabDistance\":256,\"~type\":\"idSnapEntityPrefab\",\n\"snapmapPlus\":{\"format\":1,\"description\":\"\","
            "\"tags\":[],\"mods\":[]}}";
        static const char MAPJSON[] = "{\"entities\":[],\"~type\":\"idSnapMap\"}";
        char pz[MAX_PATH * 2], pz2[MAX_PATH * 2], storeF[MAX_PATH * 2], storeG[MAX_PATH * 2], ex[MAX_PATH * 2];
        char pj[MAX_PATH * 2], name[160], zkA[SH_MS_KEY_MAX + 1], entry[MAX_PATH];
        const sh_modmanifest_row *only_a[] = { &ra, NULL };
        sh_modmanifest_row ghost = rb;
        const sh_modmanifest_row *a_and_ghost[] = { &ra, &ghost, NULL };
        size_t zl = 0, len = 0;
        unsigned char *z;
        char *text = NULL;
        _snprintf_s(pz, sizeof pz, _TRUNCATE, "%s\\Door.zip", tmp);
        _snprintf_s(pz2, sizeof pz2, _TRUNCATE, "%s\\Door2.zip", tmp);
        _snprintf_s(storeF, sizeof storeF, _TRUNCATE, "%s\\storeF", tmp);
        _snprintf_s(storeG, sizeof storeG, _TRUNCATE, "%s\\storeG", tmp);
        _snprintf_s(ex, sizeof ex, _TRUNCATE, "%s\\extracted export", tmp);
        _snprintf_s(zkA, sizeof zkA, _TRUNCATE, "%s@%s+1", ra.id, ra.version);
        strcpy_s(ghost.bundle, sizeof ghost.bundle, "5555555555555555555555555555555555555555555555555555555555555555");
        CHECK(sh_ms_set_root(storeA));
        CHECK(sh_modserve_index_build(storeA) >= 0);
        memset(&rows, 0, sizeof rows);
        CHECK(sh_modmanifest_list_add(&rows, &ra));
        CHECK(modexport_write_prefab("Door", PREFAB, sizeof PREFAB - 1, &rows, NULL, NULL, pz, &st, cause, sizeof cause));
        CHECK(st.bundles == 1);
        z = slurp(pz, &zl);
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "mods/%s/bundle.canonical.json", zkA);
        CHECK(z && entry_data(z, zl, entry) != 0);
        CHECK(z && entry_data(z, zl, "Door.json") != 0 && !memcmp(z + entry_data(z, zl, "Door.json"), PREFAB, sizeof PREFAB - 1));
        CHECK(z && count_entries_named(z, zl, "export.json") == 0 && count_entries_named(z, zl, ".meta.json") == 0);
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
        CHECK(modexport_read(pz, &len, &st, cause, sizeof cause) == NULL && strstr(cause, "prefab's export, not a map's"));
        CHECK(!modexport_probe(pz, cause, sizeof cause));
        CHECK(!modexport_read_prefab(zip, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
              strstr(cause, "map's export, not a prefab's"));

        /* Read into an empty store: the bundle the rows name installs, the text comes back. */
        CHECK(sh_ms_set_root(storeF));
        CHECK(sh_modserve_index_build(storeF) == 0);
        CHECK(modexport_read_prefab(pz, rows_named, (void *)a_and_ghost, &text, &len, name, sizeof name, &st, cause, sizeof cause));
        CHECK(text && len == sizeof PREFAB - 1 && !memcmp(text, PREFAB, len) && !strcmp(name, "Door"));
        CHECK(st.bundles == 1 && st.bundles_installed == 1 && st.bundles_absent == 1);
        CHECK(sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1);
        free(text); text = NULL;
        /* A carried bundle no row names is left out. */
        CHECK(sh_ms_set_root(storeG));
        CHECK(sh_modserve_index_build(storeG) == 0);
        {
            const sh_modmanifest_row *none[] = { NULL };
            CHECK(modexport_read_prefab(pz, rows_named, (void *)none, &text, &len, name, sizeof name, &st, cause, sizeof cause));
            CHECK(st.bundles == 0 && st.bundles_installed == 0 && sh_modserve_bundle_state(ra.bundle, NULL, 0) == 0);
            free(text); text = NULL;
        }

        /* An extracted export: the .json's folder is the export. */
        CHECK(mt_unzip_stored(pz, ex) > 3);
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door.json", ex);
        mt_write(ex, "mods\\other.mod@1.0.0+1\\bundle.canonical.json", "{\"id\":\"other.mod\"}");   /* unrelated, not named */
        CHECK(modexport_read_prefab(pj, rows_named, (void *)only_a, &text, &len, name, sizeof name, &st, cause, sizeof cause));
        if (!text) fprintf(stderr, "folder import: %s\n", cause);
        CHECK(text && len == sizeof PREFAB - 1 && !memcmp(text, PREFAB, len) && !strcmp(name, "Door"));
        CHECK(st.bundles == 1 && st.bundles_installed == 1 && sh_modserve_bundle_state(ra.bundle, NULL, 0) == 1);
        free(text); text = NULL;
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
        /* The earlier layout (export.json without "map") and two prefabs in one ZIP. */
        {
            static const char *const OLD_N[] = { "export.json", "prefab.json", "prefab.meta.json" };
            static const char *const OLD_D[] = { "{\"bundles\":[],\"format\":1,\"kind\":\"prefab\",\"name\":\"Door\",\"prefab\":\"prefab.json\"}",
                                                 "{\"entities\":[],\"~type\":\"idSnapEntityPrefab\"}", "{\"desc\":\"\"}" };
            static const char *const TWO_N[] = { "A.json", "B.json" };
            static const char *const TWO_D[] = { "{\"~type\":\"idSnapEntityPrefab\"}", "{\"~type\":\"idSnapEntityPrefab\"}" };
            static const char *const EXP_N[] = { "export.json" };
            static const char *const EXP_D[] = { "{\"entities\":[],\"~type\":\"idSnapEntityPrefab\"}" };
            CHECK(zip_stored(pz2, 3, OLD_N, OLD_D));
            CHECK(!modexport_read_prefab(pz2, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
                  strstr(cause, "earlier layout"));
            CHECK(modexport_read(pz2, &len, &st, cause, sizeof cause) == NULL && strstr(cause, "has no map.rawmap.json"));
            CHECK(zip_stored(pz2, 2, TWO_N, TWO_D));
            CHECK(!modexport_read_prefab(pz2, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
                  strstr(cause, "more than one prefab"));
            /* A prefab named "export" is a prefab, not an index. */
            CHECK(zip_stored(pz2, 1, EXP_N, EXP_D));
            CHECK(modexport_read_prefab(pz2, NULL, NULL, &text, &len, name, sizeof name, &st, cause, sizeof cause) &&
                  !strcmp(name, "export"));
            free(text); text = NULL;
        }
        CHECK(sh_ms_set_root(storeA));
    }

    HeapFree(GetProcessHeap(), 0, map);
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    printf("modexport_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
