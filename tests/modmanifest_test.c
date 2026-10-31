/* modmanifest_test -- the smpmod1 map manifest: grammar, row round trip,
 * strip and allocCount, ordering, the playtest bound, placed names, the
 * save-time closure through the serving layer, the load step, and map-level
 * rows (attached mods) with the map scope's hook and the row resolver.
 *
 * The serving layer (modserve_activate_bundles and
 * modserve_bundles_for_names_visit, declared in modserve.h) is replaced by
 * doubles defined here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modmanifest.h"
#include "../src/backend/modserve.h"
#include "../src/backend/map_shards.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ---- doubles ------------------------------------------------------------------------------- */

static char g_last_log[1024];
static int g_mods_lines;
void backend_log(const char *line)
{
    if (!line) return;
    strncpy_s(g_last_log, sizeof g_last_log, line, _TRUNCATE);
    if (!strncmp(line, "MODS:", 5)) g_mods_lines++;
}

/* The serving index holds bundles (the save closure runs). */
int sh_modserve_active(void) { return 1; }

static int g_activate_calls;
static size_t g_activate_count;
static char g_activated[8][SH_MOD_HASH_CHARS + 1];
int modserve_activate_bundles(const char *const *bundle_hashes, int count)
{
    int i;
    g_activate_calls++;
    g_activate_count = (size_t)(count > 0 ? count : 0);
    for (i = 0; i < count && i < 8; i++) strcpy_s(g_activated[i], sizeof g_activated[i], bundle_hashes[i]);
    return count;
}

typedef struct installed { const char *id, *version, *hash; unsigned long long size; const char *names[4]; } installed;
static const installed *g_installed;
static size_t g_installed_count;
static int g_lookup_calls;

size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context)
{
    size_t i, b, k, distinct = 0;
    g_lookup_calls++;
    for (b = 0; b < g_installed_count; b++) {
        int any = 0;
        for (i = 0; i < count; i++)
            for (k = 0; k < 4 && g_installed[b].names[k]; k++)
                if (!_stricmp(names[i], g_installed[b].names[k])) {
                    visit(context, g_installed[b].id, g_installed[b].version, g_installed[b].hash,
                          g_installed[b].size, names[i]);
                    any = 1;
                }
        distinct += any;
    }
    return distinct;
}

/* ---- fixtures ------------------------------------------------------------------------------ */

#define HASH_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define HASH_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static const installed INSTALLED[] = {
    { "alex.pink-imp", "1.0.0", HASH_A, 11782, { "mods/alex.pink-imp/imp", "mods/alex.pink-imp/skin", NULL, NULL } },
    { "bob.room", "2", HASH_B, 4096, { "maps/modules/mods/bob.room/hall.decl", NULL, NULL, NULL } },
};

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"

static const char MAP[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alex.pink-imp/imp\","
    "\"name\":\"\",\"state\":{\"edit\":{\"props\":{\"capDecl\":\"doors/industrial_cap_05\"},"
    "\"skins\":[\"MODS/ALEX.PINK-IMP/SKIN\"],\"count\":3}}}},"
    "{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/visblockers/industrial/cap_01\","
    "\"name\":\"\",\"state\":{\"other\":\"not/a/name\"}}}],"
    "\"instances\":[{\"moduleName\":\"maps/modules/ind_dlc/ind_totally_blank_room_4x.decl\"}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
    VAR("door_label", "Open") "," VAR("smnav1.x", "QUJD")
    "]}}";

static int alloc_string_count(const char *json, unsigned *value)
{
    const char *p = strstr(json, "\"allocCount\":[");
    int i;
    if (!p) return 0;
    p += 14;
    for (i = 0; i < 4; i++) { p = strchr(p, ','); if (!p) return 0; p++; }
    *value = (unsigned)strtoul(p, NULL, 10);
    return 1;
}

static unsigned string_rows(const char *json)
{
    sh_shard_doc doc;
    int vars, bucket;
    unsigned n = 0;
    if (!sh_shard_doc_build(json, strlen(json), &doc)) return 0xffffffffu;
    vars = sh_shard_doc_member(json, strlen(json), &doc, 0, "variables");
    bucket = vars >= 0 ? sh_shard_doc_member(json, strlen(json), &doc, vars, "string") : -1;
    if (bucket >= 0) n = sh_shard_doc_array_count(json, &doc, bucket);
    sh_shard_doc_free(&doc);
    return n;
}

static void make_row(sh_modmanifest_row *row, const char *id, const char *version, const char *hash,
                     unsigned long long size)
{
    memset(row, 0, sizeof *row);
    strcpy_s(row->id, sizeof row->id, id);
    strcpy_s(row->version, sizeof row->version, version);
    strcpy_s(row->bundle, sizeof row->bundle, hash);
    row->size = size;
}

/* ---- tests --------------------------------------------------------------------------------- */

static void grammar(void)
{
    unsigned i = 9, n = 9;
    printf("-- header and identity grammar\n");
    CHECK(sh_modmanifest_header("smpmod1.0.1", 11, &i, &n) && i == 0 && n == 1);
    CHECK(sh_modmanifest_header("smpmod1.12.13", 13, &i, &n) && i == 12 && n == 13);
    CHECK(!sh_modmanifest_header("smpmod1.1.1", 11, NULL, NULL));        /* index below count */
    CHECK(!sh_modmanifest_header("smpmod1.0.0", 11, NULL, NULL));
    CHECK(!sh_modmanifest_header("smpmod1.0", 9, NULL, NULL));
    CHECK(!sh_modmanifest_header("smpmod1.0.1.x", 13, NULL, NULL));
    CHECK(!sh_modmanifest_header("smpmod1.a.1", 11, NULL, NULL));
    CHECK(!sh_modmanifest_header("smpmod1.0.12345678", 18, NULL, NULL));  /* 8 digits */
    CHECK(!sh_modmanifest_header("smpmod2.0.1", 11, NULL, NULL));
    CHECK(sh_modmanifest_valid_id("alex.pink-imp") && sh_modmanifest_valid_id("a") &&
          sh_modmanifest_valid_id("local.sp_pistol2"));
    CHECK(!sh_modmanifest_valid_id("Alex") && !sh_modmanifest_valid_id(".a") &&
          !sh_modmanifest_valid_id("a..b") && !sh_modmanifest_valid_id("a.") &&
          !sh_modmanifest_valid_id("") && !sh_modmanifest_valid_id("a/b") &&
          !sh_modmanifest_valid_id("_a"));
    CHECK(!sh_modmanifest_valid_id("a234567890123456789012345678901234567890123456789012345678901234x"));
    CHECK(sh_modmanifest_valid_hash(HASH_A, 64) && !sh_modmanifest_valid_hash(HASH_A, 63));
    CHECK(!sh_modmanifest_valid_hash("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 64));
}

static void value_round_trip(void)
{
    sh_modmanifest_row row, back;
    char *value;
    size_t n = 0;
    char err[128];
    printf("-- row value round trip\n");
    make_row(&row, "alex.pink-imp", "1.0.0", HASH_A, 11782);
    CHECK(sh_modmanifest_row_add_name(&row, "mods/alex.pink-imp/imp"));
    CHECK(sh_modmanifest_row_add_name(&row, "MODS/ALEX.PINK-IMP/IMP"));   /* duplicate, folded */
    CHECK(sh_modmanifest_row_add_name(&row, "a \"quoted\" \\ name"));
    CHECK(row.name_count == 2);
    value = sh_modmanifest_format_value(&row, &n);
    CHECK(value && n == strlen(value));
    {
        static const char expect[] = "{\"id\":\"alex.pink-imp\",\"version\":\"1.0.0\",\"bundle\":\"" HASH_A
                                     "\",\"size\":11782,\"names\":[\"mods/alex.pink-imp/imp\",";
        CHECK(value && !strncmp(value, expect, sizeof expect - 1));
    }
    CHECK(value && sh_modmanifest_parse_value(value, n, &back, err, sizeof err));
    CHECK(!strcmp(back.id, row.id) && !strcmp(back.version, row.version) && !strcmp(back.bundle, row.bundle));
    CHECK(back.size == 11782 && back.name_count == 2 && !strcmp(back.names[1], "a \"quoted\" \\ name"));
    sh_modmanifest_row_free(&back);
    free(value);

    /* Refusals: wrong hash, bad id, missing size, names not a list. */
    {
        static const char short_hash[] = "{\"id\":\"a\",\"version\":\"1\",\"bundle\":\"x\",\"size\":1}";
        CHECK(!sh_modmanifest_parse_value(short_hash, sizeof short_hash - 1, &back, err, sizeof err));
    }
    {
        static const char bad_id[] = "{\"id\":\"A\",\"version\":\"1\",\"bundle\":\"" HASH_A "\",\"size\":1}";
        static const char no_size[] = "{\"id\":\"a\",\"version\":\"1\",\"bundle\":\"" HASH_A "\"}";
        static const char bad_names[] = "{\"id\":\"a\",\"version\":\"1\",\"bundle\":\"" HASH_A "\",\"size\":1,\"names\":[1,}";
        static const char extra[] = "{\"id\":\"a\",\"version\":\"1\",\"bundle\":\"" HASH_A "\",\"size\":1,\"future\":true}";
        CHECK(!sh_modmanifest_parse_value(bad_id, sizeof bad_id - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(no_size, sizeof no_size - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(bad_names, sizeof bad_names - 1, &back, err, sizeof err));
        CHECK(sh_modmanifest_parse_value(extra, sizeof extra - 1, &back, err, sizeof err));
        sh_modmanifest_row_free(&back);
    }

    /* The row budget: names drop from the end, the row stays. */
    {
        int i;
        char name[64];
        for (i = 0; i < 200; i++) {
            _snprintf_s(name, sizeof name, _TRUNCATE, "mods/alex.pink-imp/a/rather/long/name_%03d", i);
            sh_modmanifest_row_add_name(&row, name);
        }
        value = sh_modmanifest_format_value(&row, &n);
        CHECK(value && n <= SH_MODMANIFEST_ROW_MAX && strstr(value, "mods/alex.pink-imp/imp"));
        CHECK(value && sh_modmanifest_parse_value(value, n, &back, err, sizeof err) &&
              back.name_count > 2 && back.name_count < row.name_count);
        sh_modmanifest_row_free(&back);
        free(value);
    }
    sh_modmanifest_row_free(&row);
}

static void text_round_trip_and_order(void)
{
    sh_modmanifest_list rows = {0}, back = {0};
    sh_modmanifest_row row;
    char err[192] = "", *written, *stripped;
    size_t wlen = 0, slen = 0;
    unsigned count = 0, removed = 0, rejected = 0;
    int failed = 0;
    const char *door, *nav, *first, *second;
    printf("-- rows written, read, ordered last, stripped with allocCount\n");

    make_row(&row, "alex.pink-imp", "1.0.0", HASH_A, 11782);
    sh_modmanifest_row_add_name(&row, "mods/alex.pink-imp/imp");
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    make_row(&row, "bob.room", "2", HASH_B, 4096);
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);

    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    CHECK(written != NULL);
    if (!written) { fprintf(stderr, "write: %s\n", err); sh_modmanifest_list_free(&rows); return; }
    CHECK(wlen == strlen(written));
    CHECK(string_rows(written) == 4 && alloc_string_count(written, &count) && count == 4);
    /* Existing variables keep their (type, index); the rows come last, in index order. */
    door = strstr(written, "door_label");
    nav = strstr(written, "smnav1.x");
    first = strstr(written, "\"smpmod1.0.2\"");
    second = strstr(written, "\"smpmod1.1.2\"");
    CHECK(door && nav && first && second && door < nav && nav < first && first < second);
    CHECK(strstr(written, "\\\"bundle\\\":\\\"" HASH_A "\\\"") != NULL);   /* escaped JSON in the value */
    CHECK(strstr(written, "\"entities\":[{\"entityDef\"") == written + 1);   /* the rest is untouched */

    CHECK(sh_modmanifest_read(written, wlen, &back, &rejected) == 2 && rejected == 0 && back.count == 2);
    CHECK(back.count == 2 && !strcmp(back.rows[0].id, "alex.pink-imp") && !strcmp(back.rows[1].bundle, HASH_B) &&
          back.rows[0].size == 11782 && back.rows[0].name_count == 1);
    sh_modmanifest_list_free(&back);

    stripped = sh_modmanifest_strip(written, wlen, &slen, &removed, &failed);
    CHECK(stripped && removed == 2 && !failed && !strstr(stripped, "smpmod1."));
    CHECK(stripped && strcmp(stripped, MAP) == 0);   /* byte-identical to the map before the write */
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);

    /* A rewrite replaces old rows instead of stacking them. */
    {
        char *again = sh_modmanifest_write(written, wlen, &rows, &slen, err, sizeof err);
        CHECK(again && string_rows(again) == 4 && alloc_string_count(again, &count) && count == 4);
        CHECK(again && strcmp(again, written) == 0);
        if (again) HeapFree(GetProcessHeap(), 0, again);
    }
    /* A lookalike name is an ordinary variable and stays. */
    {
        static const char look[] = "{\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
            VAR("smpmod1.notes", "x") "," VAR("smpmod1.0.1.extra", "y") "]}}";
        CHECK(sh_modmanifest_strip(look, sizeof look - 1, &slen, &removed, &failed) == NULL && !failed);
    }
    HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_list_free(&rows);
}

static void playtest_bound(void)
{
    sh_modmanifest_list rows = {0};
    sh_modmanifest_row row;
    char err[192], *written, *engine;
    size_t wlen = 0, elen = 0;
    int i;
    printf("-- the 4 KiB playtest message never sees a row\n");
    make_row(&row, "alex.pink-imp", "1.0.0", HASH_A, 1);
    for (i = 0; i < 100; i++) {
        char name[96];
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/alex.pink-imp/decls/a_fairly_long_entity_name_%03d", i);
        sh_modmanifest_row_add_name(&row, name);
    }
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    CHECK(written != NULL);
    if (written) {
        /* Each row's decoded value is at most 1 KiB even for clients that keep it. */
        const char *v = strstr(written, "\"smpmod1.0.1\"");
        const char *iv = v ? strstr(v, "\"initialValue\":\"") : NULL;
        const char *end = iv ? strstr(iv + 16, "\",\"~type\"") : NULL;
        CHECK(iv && end && (size_t)(end - (iv + 16)) < SH_MODMANIFEST_ROW_MAX + 400);
        /* What the engine parses: no row, string count back to the map's own. */
        sh_modmanifest_reset();
        engine = sh_modmanifest_prepare_load(written, wlen, &elen);
        CHECK(engine && !strstr(engine, "smpmod1.") && string_rows(engine) == 2);
        CHECK(engine && strlen(engine) == sizeof MAP - 1);
        if (engine) HeapFree(GetProcessHeap(), 0, engine);
        HeapFree(GetProcessHeap(), 0, written);
    }
    sh_modmanifest_list_free(&rows);
}

static void placed_names(void)
{
    char **names = NULL;
    size_t n, i;
    int failed = 0, has_imp = 0, has_skin = 0, has_cap = 0, has_vis = 0, has_module = 0, has_other = 0, has_key = 0;
    printf("-- placed names\n");
    n = sh_modmanifest_placed_names(MAP, sizeof MAP - 1, &names, &failed);
    CHECK(!failed && n == 5);
    for (i = 0; i < n; i++) {
        if (!strcmp(names[i], "mods/alex.pink-imp/imp")) has_imp = 1;
        if (!_stricmp(names[i], "mods/alex.pink-imp/skin")) has_skin = 1;
        if (!strcmp(names[i], "doors/industrial_cap_05")) has_cap = 1;
        if (!strcmp(names[i], "snapmaps/visblockers/industrial/cap_01")) has_vis = 1;
        if (!strcmp(names[i], "maps/modules/ind_dlc/ind_totally_blank_room_4x.decl")) has_module = 1;
        if (!strcmp(names[i], "not/a/name")) has_other = 1;
        if (!strcmp(names[i], "capDecl") || !strcmp(names[i], "props")) has_key = 1;
    }
    CHECK(has_imp && has_skin && has_cap && has_vis && has_module && !has_other && !has_key);
    sh_modmanifest_names_free(names, n);

    /* Case-insensitive dedupe and escaped values. */
    {
        static const char dup[] = "{\"a\":[{\"inherit\":\"X/Y\"},{\"inherit\":\"x/y\"},{\"inherit\":\"q\\/r\"},"
                                  "{\"inherit\":\"\"}]}";
        n = sh_modmanifest_placed_names(dup, sizeof dup - 1, &names, &failed);
        CHECK(!failed && n == 2);
        CHECK(n == 2 && (!strcmp(names[0], "q/r") || !strcmp(names[1], "q/r")));
        sh_modmanifest_names_free(names, n);
    }
    /* Malformed structure is a failure, not a partial list. */
    n = sh_modmanifest_placed_names("{\"inherit\":\"a\"", 14, &names, &failed);
    CHECK(failed && n == 0 && names == NULL);
}

static void save_closure(void)
{
    char *out;
    size_t n = 0;
    sh_modmanifest_list back = {0};
    printf("-- save closure through the serving layer\n");
    g_installed = INSTALLED;
    g_installed_count = 2;

    /* The imp is placed (inherit and an edit value); the room module is not. */
    out = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n);
    CHECK(out != NULL && g_lookup_calls == 1);
    if (out) {
        CHECK(sh_modmanifest_read(out, n, &back, NULL) == 1 && back.count == 1);
        CHECK(back.count == 1 && !strcmp(back.rows[0].bundle, HASH_A) && back.rows[0].name_count == 2 &&
              back.rows[0].size == 11782 && !strcmp(back.rows[0].version, "1.0.0"));
        CHECK(strstr(out, "\"smpmod1.0.1\"") && !strstr(out, "smpmod1.1."));
        sh_modmanifest_list_free(&back);

        /* Saving the saved text again gives the same bytes. */
        {
            size_t n2 = 0;
            char *again = sh_modmanifest_on_save(out, n, &n2);
            CHECK(again && n2 == n && !strcmp(again, out));
            if (again) HeapFree(GetProcessHeap(), 0, again);
        }
        /* Nothing placed needs a bundle any more: the stale row goes, no row is added. */
        g_installed_count = 0;
        {
            size_t n2 = 0;
            char *clean = sh_modmanifest_on_save(out, n, &n2);
            CHECK(clean && !strstr(clean, "smpmod1.") && !strcmp(clean, MAP));
            if (clean) HeapFree(GetProcessHeap(), 0, clean);
        }
        HeapFree(GetProcessHeap(), 0, out);
    }
    /* No bundle and no stale row: the engine's bytes are saved unchanged. */
    g_installed_count = 0;
    CHECK(sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n) == NULL);
    /* A placed module provided by a bundle. */
    {
        static const char room[] = "{\"instances\":[{\"moduleName\":\"maps/modules/mods/bob.room/hall.decl\"}],"
                                   "\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
        unsigned count = 9;
        g_installed_count = 2;
        out = sh_modmanifest_on_save(room, sizeof room - 1, &n);
        CHECK(out && strstr(out, "smpmod1.0.1") && string_rows(out) == 1 && alloc_string_count(out, &count) && count == 1);
        if (out) HeapFree(GetProcessHeap(), 0, out);
    }
}

static void load_step(void)
{
    sh_modmanifest_list rows = {0}, current = {0};
    sh_modmanifest_row row;
    char err[192], *written, *engine;
    size_t wlen = 0, elen = 0;
    printf("-- load step: keep, strip, activate\n");
    make_row(&row, "alex.pink-imp", "1.0.0", HASH_A, 5);
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    make_row(&row, "bob.room", "2", HASH_B, 6);
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    CHECK(written != NULL);
    g_activate_calls = 0;
    engine = written ? sh_modmanifest_prepare_load(written, wlen, &elen) : NULL;
    CHECK(engine && g_activate_calls == 1 && g_activate_count == 2);
    CHECK(!strcmp(g_activated[0], HASH_A) && !strcmp(g_activated[1], HASH_B));
    CHECK(sh_modmanifest_current(&current) == 2 && !strcmp(current.rows[1].id, "bob.room"));
    sh_modmanifest_list_free(&current);
    if (engine) HeapFree(GetProcessHeap(), 0, engine);

    /* A map without rows: the engine's bytes, activation with nothing, the manifest forgotten. */
    g_activate_calls = 0;
    CHECK(sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &elen) == NULL);
    CHECK(g_activate_calls == 1 && g_activate_count == 0);
    CHECK(sh_modmanifest_current(&current) == 0);

    /* One unreadable row is dropped, the readable one still activates. */
    {
        static const char mixed[] = "{\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
            VAR("smpmod1.0.2", "{\\\"id\\\":\\\"Bad\\\"}") ","
            VAR("smpmod1.1.2", "{\\\"id\\\":\\\"bob.room\\\",\\\"version\\\":\\\"2\\\",\\\"bundle\\\":\\\"" HASH_B
                "\\\",\\\"size\\\":6,\\\"names\\\":[]}") "]}}";
        g_activate_calls = 0;
        engine = sh_modmanifest_prepare_load(mixed, sizeof mixed - 1, &elen);
        CHECK(engine && !strstr(engine, "smpmod1.") && string_rows(engine) == 0);
        CHECK(g_activate_count == 1 && !strcmp(g_activated[0], HASH_B));
        if (engine) HeapFree(GetProcessHeap(), 0, engine);
    }
    if (written) HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_reset();
}

static void mod_json(void)
{
    static const char text[] =
        "\xef\xbb\xbf{\n  \"id\": \"Alex.Pink-Imp\", \"version\": \"1.0.0\", \"name\": \"Pink Imp\", \"x-note\": [1],\n"
        "  \"snapmap-plus\": {\"format\": 1, \"build\": 2, \"bundle\": \"" HASH_A "\",\n"
        "  \"files\": [\n"
        "    {\"path\": \"assets/generated/decls/md6def/mods/alex.pink-imp/imp.md6.decl\", \"sha256\": \"" HASH_B "\","
        " \"size\": 11782, \"provides\": {\"type\": \"md6Def\", \"name\": \"mods/alex.pink-imp/imp.md6\"}, \"needs\": []},\n"
        "    {\"path\": \"assets/x.bin\", \"sha256\": \"" HASH_B "\", \"size\": 18, \"provides\": {\"type\": \"file\", \"name\": \"x.bin\"}}\n"
        "  ],\n  \"stock\": [{\"path\": \"assets/maps/a.sbsp\", \"sha256\": \"" HASH_B "\", \"source\": \"maps/a.sbsp\","
        " \"provides\": {\"type\": \"file\", \"name\": \"maps/a.sbsp\"}, \"needs\": [], \"inAssets\": false}]}\n}\n";
    sh_modmanifest_row info;
    char err[160];
    printf("-- mod.json reader\n");
    CHECK(sh_modmanifest_read_mod_json(text, sizeof text - 1, &info, err, sizeof err));
    CHECK(!strcmp(info.id, "alex.pink-imp") && !strcmp(info.version, "1.0.0") && !strcmp(info.bundle, HASH_A));
    CHECK(info.size == 11800 && info.name_count == 3 && !strcmp(info.names[0], "mods/alex.pink-imp/imp.md6"));
    sh_modmanifest_row_free(&info);
    {
        static const char unbuilt[] = "{\"id\":\"a\",\"version\":\"1\",\"name\":\"a\"}";
        static const char nohash[] = "{\"id\":\"a\",\"version\":\"1\",\"snapmap-plus\":{\"files\":[]}}";
        CHECK(!sh_modmanifest_read_mod_json(unbuilt, sizeof unbuilt - 1, &info, err, sizeof err));
        CHECK(!sh_modmanifest_read_mod_json(nohash, sizeof nohash - 1, &info, err, sizeof err));
    }
}

/* The save step's restore hook: asked with the placed names while armed,
 * before the rows are built, so a reinstated bundle gets its row. */
static int g_armed, g_restore_calls, g_restore_when;
static size_t g_restore_names;
static int fake_armed(void) { return g_armed; }
static int fake_restore(const char *const *hashes, int hash_count, const char *const *names, size_t name_count, int when)
{
    (void)hashes; (void)hash_count; (void)names;
    g_restore_calls++;
    g_restore_names = name_count;
    g_restore_when = when;
    g_installed_count = 2;   /* the bundle serves again */
    return 1;
}

static void save_restore(void)
{
    char *out;
    size_t n = 0;
    printf("-- save: an uninstalled mod the map still places is reinstated before the rows are built\n");
    sh_modmanifest_set_restore(fake_restore, fake_armed);
    g_installed = INSTALLED;
    g_installed_count = 0;
    g_armed = 0;
    CHECK(sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n) == NULL && g_restore_calls == 0);   /* not armed */
    g_armed = 1;
    out = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n);
    CHECK(g_restore_calls == 1 && g_restore_names > 0 && g_restore_when == SH_MODSERVE_RESTORE_SAVE);
    CHECK(out && strstr(out, "\"smpmod1.0.1\""));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    sh_modmanifest_set_restore(NULL, NULL);
}

/* ---- map-level rows -------------------------------------------------------------------------- */

#define HASH_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define HASH_D "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define HASH_E "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"

static int g_scope_calls, g_scope_when, g_scope_armed_value = 1;
static size_t g_scope_rows, g_scope_names, g_scope_attached;
static void fake_scope(const sh_modmanifest_list *rows, const char *const *names, size_t name_count, int when)
{
    size_t i;
    g_scope_calls++;
    g_scope_when = when;
    g_scope_rows = rows ? rows->count : 0;
    g_scope_names = names ? name_count : 0;
    g_scope_attached = 0;
    for (i = 0; rows && i < rows->count; i++) g_scope_attached += rows->rows[i].attached ? 1u : 0u;
}
static int fake_scope_armed(void) { return g_scope_armed_value; }

/* bare-hands is installed as a newer build (HASH_D); HASH_E is not installed. */
static int fake_resolve(const char *bundle_hash, const char *id, sh_modmanifest_row *out)
{
    if ((bundle_hash && (!strcmp(bundle_hash, HASH_C) || !strcmp(bundle_hash, HASH_D))) || (id && !strcmp(id, "alex.bare-hands"))) {
        strcpy_s(out->id, sizeof out->id, "alex.bare-hands");
        strcpy_s(out->version, sizeof out->version, "1.1.0");
        strcpy_s(out->bundle, sizeof out->bundle, HASH_D);
        out->size = 777;
        return 1;
    }
    return 0;
}

static void map_level(void)
{
    sh_modmanifest_row row, back;
    sh_modmanifest_list rows, current;
    char *value, *written, *engine, *saved, err[160];
    size_t vlen = 0, wlen = 0, elen = 0, slen = 0;
    printf("-- map-level rows: the attached key, load, save, attach, remove\n");
    /* The key round-trips and is written only for a map-level row. */
    make_row(&row, "alex.bare-hands", "1.0.0", HASH_C, 512);
    value = sh_modmanifest_format_value(&row, &vlen);
    CHECK(value && !strstr(value, "attached"));
    free(value);
    row.attached = 1;
    value = sh_modmanifest_format_value(&row, &vlen);
    CHECK(value && !strcmp(value, "{\"id\":\"alex.bare-hands\",\"version\":\"1.0.0\",\"bundle\":\"" HASH_C
                                  "\",\"size\":512,\"attached\":true,\"names\":[]}"));
    CHECK(value && sh_modmanifest_parse_value(value, vlen, &back, err, sizeof err) && back.attached == 1);
    sh_modmanifest_row_free(&back);
    free(value);
    CHECK(sh_modmanifest_parse_value("{\"id\":\"a.b\",\"version\":\"1\",\"bundle\":\"" HASH_C "\",\"size\":1,\"attached\":false}",
                                     strlen("{\"id\":\"a.b\",\"version\":\"1\",\"bundle\":\"" HASH_C "\",\"size\":1,\"attached\":false}"),
                                     &back, err, sizeof err) && back.attached == 0);
    sh_modmanifest_row_free(&back);

    /* A map with a placed mod's row and a map-level row. */
    memset(&rows, 0, sizeof rows);
    {
        sh_modmanifest_row placed;
        make_row(&placed, "alex.pink-imp", "1.0.0", HASH_A, 11782);
        sh_modmanifest_row_add_name(&placed, "mods/alex.pink-imp/imp");
        CHECK(sh_modmanifest_list_add(&rows, &placed));
        sh_modmanifest_row_free(&placed);
    }
    CHECK(sh_modmanifest_list_add(&rows, &row));
    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    CHECK(written != NULL);
    sh_modmanifest_list_free(&rows);
    if (!written) { sh_modmanifest_row_free(&row); return; }

    sh_modmanifest_reset();
    sh_modmanifest_set_scope(fake_scope, fake_scope_armed);
    sh_modmanifest_set_resolver(fake_resolve);
    g_installed = INSTALLED;
    g_installed_count = 2;
    g_scope_calls = 0;
    engine = sh_modmanifest_prepare_load(written, wlen, &elen);
    CHECK(engine != NULL && !strstr(engine, "smpmod1."));
    /* The load hook sees both rows and the placed names, before activation. */
    CHECK(g_scope_calls == 1 && g_scope_when == SH_MODSERVE_RESTORE_LOAD && g_scope_rows == 2 && g_scope_attached == 1 &&
          g_scope_names > 0);
    CHECK(g_activate_count == 2);
    CHECK(sh_modmanifest_current(&current) == 2 && sh_modmanifest_current_attached(&rows) == 1 &&
          !strcmp(rows.rows[0].bundle, HASH_C));
    sh_modmanifest_list_free(&current);
    sh_modmanifest_list_free(&rows);

    /* Armed 0: no placed names are read at a load. */
    g_scope_armed_value = 0;
    if (engine) { HeapFree(GetProcessHeap(), 0, engine); engine = NULL; }
    engine = sh_modmanifest_prepare_load(written, wlen, &elen);
    CHECK(g_scope_calls == 2 && g_scope_names == 0 && g_scope_rows == 2);
    g_scope_armed_value = 1;

    /* The save keeps the map-level row (nothing places it), renamed to the
     * build that serves; the closure's row is written as before. */
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, "alex.pink-imp") && strstr(saved, HASH_D) && !strstr(saved, HASH_C));
    CHECK(saved && strstr(saved, "\\\"attached\\\":true"));
    CHECK(g_scope_when == SH_MODSERVE_RESTORE_SAVE && g_scope_rows == 2 && g_scope_attached == 1);
    CHECK(sh_modmanifest_current_attached(&rows) == 1 && !strcmp(rows.rows[0].bundle, HASH_D) &&
          !strcmp(rows.rows[0].version, "1.1.0"));
    sh_modmanifest_list_free(&rows);
    if (saved) {
        /* Reloaded, the row is map-level again. */
        char *again = sh_modmanifest_prepare_load(saved, slen, &elen);
        CHECK(sh_modmanifest_current_attached(&rows) == 1 && rows.rows[0].attached);
        sh_modmanifest_list_free(&rows);
        if (again) HeapFree(GetProcessHeap(), 0, again);
        HeapFree(GetProcessHeap(), 0, saved);
    }

    /* Attach: a new row, then again (already attached), then a row of a mod
     * not installed, which a save keeps as it is. */
    make_row(&back, "bob.room", "2", HASH_B, 4096);
    CHECK(sh_modmanifest_current_attach(&back) == 1 && sh_modmanifest_current_attach(&back) == 0);
    sh_modmanifest_row_free(&back);
    make_row(&back, "erin.gone", "3", HASH_E, 9);
    sh_modmanifest_row_add_name(&back, "x/y");
    CHECK(sh_modmanifest_current_attach(&back) == 1);
    sh_modmanifest_row_free(&back);
    make_row(&back, "Bad Id", "3", HASH_E, 9);
    CHECK(sh_modmanifest_current_attach(&back) == -1);
    sh_modmanifest_row_free(&back);
    CHECK(sh_modmanifest_current_attached(&rows) == 3);
    sh_modmanifest_list_free(&rows);
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, HASH_B) && strstr(saved, HASH_E) && strstr(saved, "erin.gone"));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* Remove from map drops the row; the next save writes none for it. */
    CHECK(sh_modmanifest_current_remove(HASH_D) == 1);
    CHECK(sh_modmanifest_current_remove(HASH_E) == 1);
    CHECK(sh_modmanifest_current_remove(HASH_B) == 1);
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && !strstr(saved, HASH_D) && !strstr(saved, HASH_B) && strstr(saved, "alex.pink-imp"));
    CHECK(g_scope_attached == 0 && g_scope_rows == 1);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* A map-level row alone (no placed mod, nothing served for it) is still written. */
    g_installed_count = 0;
    make_row(&back, "erin.gone", "3", HASH_E, 9);
    CHECK(sh_modmanifest_current_attach(&back) == 1);
    sh_modmanifest_row_free(&back);
    saved = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &slen);
    CHECK(saved && strstr(saved, "\"smpmod1.0.1\"") && strstr(saved, HASH_E));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    if (engine) HeapFree(GetProcessHeap(), 0, engine);
    HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_row_free(&row);
    sh_modmanifest_set_scope(NULL, NULL);
    sh_modmanifest_set_resolver(NULL);
    sh_modmanifest_reset();
    g_installed_count = 2;
}

int main(void)
{
    printf("modmanifest_test\n");
    grammar();
    value_round_trip();
    text_round_trip_and_order();
    playtest_bound();
    placed_names();
    save_closure();
    save_restore();
    load_step();
    mod_json();
    map_level();
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
