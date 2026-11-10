/* modmanifest_test -- the map manifest: grammar, row round trip, the rows in
 * the map's "snapmap-plus" member (map_carrier.h) and the legacy smpmod1 rows
 * it replaces (read, converted, stripped with allocCount), placed names, the
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
#include "../src/backend/map_carrier.h"

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

/* The build that serves a row: its hash when installed, else the installed
 * build of its id (the asset rule's activation). */
int sh_modserve_bundle_serving(const char *bundle_hash, const char *id, sh_modserve_bundle_ref *out)
{
    size_t b;
    if (out) memset(out, 0, sizeof *out);
    for (b = 0; b < g_installed_count; b++)
        if (bundle_hash && !strcmp(g_installed[b].hash, bundle_hash)) break;
    if (b == g_installed_count)
        for (b = 0; b < g_installed_count; b++)
            if (id && !strcmp(g_installed[b].id, id)) break;
    if (b == g_installed_count) return 0;
    if (out) {
        strcpy_s(out->id, sizeof out->id, g_installed[b].id);
        strcpy_s(out->version, sizeof out->version, g_installed[b].version);
        strcpy_s(out->hash, sizeof out->hash, g_installed[b].hash);
        out->size = g_installed[b].size;
    }
    return 1;
}

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

/* The text the engine parses after a load: the family's legacy rows gone
 * (prepare_load's result, or the input when it removed none) and the member
 * stripped last, as rawmap.c prepare_map_buffer does. Process heap. */
static char *engine_text(char *prepared, const char *input, size_t input_len, size_t *out_len)
{
    const char *src = prepared ? prepared : input;
    size_t n = prepared ? strlen(prepared) : input_len, w = 0;
    char *bare = sh_carrier_strip(src, n, &w);
    if (!bare) {
        bare = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
        memcpy(bare, src, n);
        bare[n] = 0;
        w = n;
    }
    if (prepared) HeapFree(GetProcessHeap(), 0, prepared);
    if (out_len) *out_len = w;
    return bare;
}

/* A legacy row as an earlier build wrote it (escaped JSON in initialValue). */
#define ROW_A_LEGACY "{\\\"id\\\":\\\"alex.pink-imp\\\",\\\"version\\\":\\\"1.0.0\\\",\\\"bundle\\\":\\\"" HASH_A \
                     "\\\",\\\"size\\\":11782,\\\"names\\\":[\\\"mods/alex.pink-imp/imp\\\"]}"

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

    /* No row budget: every name is kept, however long the row. */
    {
        int i;
        char name[64];
        for (i = 0; i < 200; i++) {
            _snprintf_s(name, sizeof name, _TRUNCATE, "mods/alex.pink-imp/a/rather/long/name_%03d", i);
            sh_modmanifest_row_add_name(&row, name);
        }
        value = sh_modmanifest_format_value(&row, &n);
        CHECK(value && n > 4096 && strstr(value, "mods/alex.pink-imp/imp") && strstr(value, "name_199"));
        CHECK(value && sh_modmanifest_parse_value(value, n, &back, err, sizeof err) &&
              back.name_count == row.name_count);
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
    printf("-- rows written into the member, read, stripped; the variables untouched\n");

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
    /* The author's variables and allocCount are the engine's; the member is last at the root. */
    CHECK(string_rows(written) == 2 && alloc_string_count(written, &count) && count == 2);
    CHECK(!strstr(written, "smpmod1."));
    CHECK(!memcmp(written, MAP, sizeof MAP - 2));
    CHECK(!strcmp(written + sizeof MAP - 2,
                  ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"alex.pink-imp\",\"version\":\"1.0.0\",\"bundle\":\""
                  HASH_A "\",\"size\":11782,\"names\":[\"mods/alex.pink-imp/imp\"]},{\"id\":\"bob.room\",\"version\":\"2\","
                  "\"bundle\":\"" HASH_B "\",\"size\":4096,\"names\":[]}]}}"));

    CHECK(sh_modmanifest_read(written, wlen, &back, &rejected) == 2 && rejected == 0 && back.count == 2);
    CHECK(back.count == 2 && !strcmp(back.rows[0].id, "alex.pink-imp") && !strcmp(back.rows[1].bundle, HASH_B) &&
          back.rows[0].size == 11782 && back.rows[0].name_count == 1);
    sh_modmanifest_list_free(&back);

    /* No legacy row to strip; the member strip gives the map back byte for byte. */
    CHECK(sh_modmanifest_strip(written, wlen, &slen, &removed, &failed) == NULL && !failed);
    stripped = sh_carrier_strip(written, wlen, &slen);
    CHECK(stripped && slen == sizeof MAP - 1 && strcmp(stripped, MAP) == 0);
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);

    /* A rewrite replaces the member's rows instead of stacking them. */
    {
        char *again = sh_modmanifest_write(written, wlen, &rows, &slen, err, sizeof err);
        CHECK(again && strcmp(again, written) == 0);
        if (again) HeapFree(GetProcessHeap(), 0, again);
    }
    /* Writing no rows removes the member. */
    {
        char *none = sh_modmanifest_write(written, wlen, NULL, &slen, err, sizeof err);
        CHECK(none && !strcmp(none, MAP));
        if (none) HeapFree(GetProcessHeap(), 0, none);
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

/* Maps an earlier build saved carry smpmod1 string variables: they are read,
 * removed (allocCount[4] back to the author's count) and a write converts them
 * to the member. When both forms are present the member wins. */
static void legacy_rows(void)
{
    static const char LEGACY[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
        VAR("door_label", "Open") "," VAR("smpmod1.0.1", ROW_A_LEGACY) "]}}";
    static const char BARE[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" VAR("door_label", "Open") "]}}";
    sh_modmanifest_list rows = {0};
    sh_modmanifest_row row;
    char err[192] = "", *out, *engine;
    size_t n = 0, en = 0;
    unsigned rejected = 0, count = 0;
    printf("-- legacy smpmod1 rows: read, removed, converted; the member wins over them\n");
    CHECK(sh_modmanifest_read(LEGACY, sizeof LEGACY - 1, &rows, &rejected) == 1 && !rejected &&
          !strcmp(rows.rows[0].id, "alex.pink-imp") && rows.rows[0].name_count == 1);
    out = sh_modmanifest_write(LEGACY, sizeof LEGACY - 1, &rows, &n, err, sizeof err);
    CHECK(out && !strstr(out, "smpmod1.") && string_rows(out) == 1 && alloc_string_count(out, &count) && count == 1);
    CHECK(out && !memcmp(out, BARE, sizeof BARE - 2) &&
          strstr(out, ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"alex.pink-imp\""));
    sh_modmanifest_list_free(&rows);
    /* A load of the legacy text: the rows activate and leave the engine's text. */
    sh_modmanifest_reset();
    g_activate_calls = 0;
    engine = engine_text(sh_modmanifest_prepare_load(LEGACY, sizeof LEGACY - 1, &en), LEGACY, sizeof LEGACY - 1, &en);
    CHECK(engine && !strcmp(engine, BARE) && g_activate_calls == 1 && g_activate_count == 1);
    CHECK(strstr(g_last_log, "from older-format smpmod1 rows; 1 older-format row(s) migrated") != NULL);
    HeapFree(GetProcessHeap(), 0, engine);
    /* Both forms: the member (bob.room) is read; the legacy row is only removed. */
    if (out) {
        static const char BOB[] = ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"bob.room\",\"version\":\"2\","
                                  "\"bundle\":\"" HASH_B "\",\"size\":4096,\"names\":[]}]}}";
        char both[4096];
        sh_modmanifest_list cur;
        _snprintf_s(both, sizeof both, _TRUNCATE, "%.*s%s", (int)(sizeof LEGACY - 2), LEGACY, BOB);
        CHECK(sh_modmanifest_read(both, strlen(both), &rows, NULL) == 1 && !strcmp(rows.rows[0].id, "bob.room"));
        sh_modmanifest_list_free(&rows);
        engine = engine_text(sh_modmanifest_prepare_load(both, strlen(both), &en), both, strlen(both), &en);
        CHECK(engine && !strcmp(engine, BARE));
        CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].bundle, HASH_B));
        CHECK(strstr(g_last_log, "from the snapmap-plus member; 1 older-format row(s) migrated") != NULL);
        sh_modmanifest_list_free(&cur);
        HeapFree(GetProcessHeap(), 0, engine);
        HeapFree(GetProcessHeap(), 0, out);
    }
    /* A malformed member is no member: the legacy row is read. */
    {
        char bad[4096];
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%.*s%s", (int)(sizeof LEGACY - 2), LEGACY,
                    ",\"snapmap-plus\":{\"format\":2,\"mods\":[]}}");
        CHECK(sh_modmanifest_read(bad, strlen(bad), &rows, NULL) == 1 && !strcmp(rows.rows[0].id, "alex.pink-imp"));
        sh_modmanifest_list_free(&rows);
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%.*s%s", (int)(sizeof BARE - 2), BARE,
                    ",\"snapmap-plus\":{\"format\":1,\"mods\":{\"not\":\"an array\"}}}");
        CHECK(sh_modmanifest_read(bad, strlen(bad), &rows, &rejected) == 0 && rows.count == 0);
        engine = engine_text(sh_modmanifest_prepare_load(bad, strlen(bad), &en), bad, strlen(bad), &en);
        CHECK(engine && !strcmp(engine, BARE));
        HeapFree(GetProcessHeap(), 0, engine);
        /* A row in the member that does not read is dropped and counted. */
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%.*s%s", (int)(sizeof BARE - 2), BARE,
                    ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"Bad\"},7,{\"id\":\"bob.room\",\"version\":\"2\","
                    "\"bundle\":\"" HASH_B "\",\"size\":6,\"names\":[]}]}}");
        CHECK(sh_modmanifest_read(bad, strlen(bad), &rows, &rejected) == 1 && rejected == 2 &&
              !strcmp(rows.rows[0].id, "bob.room"));
        sh_modmanifest_list_free(&rows);
    }
    (void)row;
    sh_modmanifest_reset();
}

/* The save chain on one text: settings, then navigation, then the manifest,
 * each putting its part; the member carries all three in its order and the
 * load reads the rows back while the other parts are left to their readers. */
static void all_parts(void)
{
    static const char SETTINGS[] = "{\"render\":\"4;0\",\"bounds\":\"1;32671\"}";
    static const char NAV[] = "[{\"module\":\"a/b\",\"class\":\"monster48\",\"digest\":\"0123456789abcdef\","
                              "\"geom\":\"fedcba9876543210\",\"data\":[\"QUJD\"]}]";
    char err[160] = "", *a, *b, *saved, *engine;
    size_t an = 0, bn = 0, sn = 0, en = 0, from = 0, to = 0;
    sh_modmanifest_list back = {0};
    printf("-- all three parts in one member: settings, navigation and the rows\n");
    g_installed = INSTALLED;
    g_installed_count = 2;
    sh_modmanifest_reset();
    a = sh_carrier_put(MAP, sizeof MAP - 1, "settings", SETTINGS, sizeof SETTINGS - 1, &an, err, sizeof err);
    b = a ? sh_carrier_put(a, an, "nav", NAV, sizeof NAV - 1, &bn, err, sizeof err) : NULL;
    saved = b ? sh_modmanifest_on_save(b, bn, &sn) : NULL;
    CHECK(saved != NULL);
    if (saved) {
        CHECK(!memcmp(saved, MAP, sizeof MAP - 2) && string_rows(saved) == 2 && !strstr(saved, "smpmod1."));
        CHECK(strstr(saved, ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"alex.pink-imp\"") != NULL);
        CHECK(sh_carrier_part(saved, sn, "settings", &from, &to) == 1 && to - from == sizeof SETTINGS - 1 &&
              !memcmp(saved + from, SETTINGS, to - from));
        CHECK(sh_carrier_part(saved, sn, "nav", &from, &to) == 1 && to - from == sizeof NAV - 1);
        CHECK(sh_modmanifest_read(saved, sn, &back, NULL) == 1 && !strcmp(back.rows[0].bundle, HASH_A));
        sh_modmanifest_list_free(&back);
        engine = engine_text(sh_modmanifest_prepare_load(saved, sn, &en), saved, sn, &en);
        CHECK(engine && !strcmp(engine, MAP) && g_activate_count == 1);
        if (engine) HeapFree(GetProcessHeap(), 0, engine);
        HeapFree(GetProcessHeap(), 0, saved);
    }
    if (a) HeapFree(GetProcessHeap(), 0, a);
    if (b) HeapFree(GetProcessHeap(), 0, b);
    sh_modmanifest_reset();
    g_lookup_calls = 0;
    g_installed_count = 0;
}

static void member_carries_everything(void)
{
    sh_modmanifest_list rows = {0};
    sh_modmanifest_row row;
    char err[192], *written, *engine;
    size_t wlen = 0, elen = 0;
    int i;
    printf("-- a long row stays whole in the member; the engine parses the map's own text\n");
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
        sh_modmanifest_list back = {0};
        CHECK(sh_modmanifest_read(written, wlen, &back, NULL) == 1 && back.rows[0].name_count == 100);
        sh_modmanifest_list_free(&back);
        sh_modmanifest_reset();
        engine = engine_text(sh_modmanifest_prepare_load(written, wlen, &elen), written, wlen, &elen);
        CHECK(engine && !strstr(engine, "snapmap-plus") && string_rows(engine) == 2);
        CHECK(engine && !strcmp(engine, MAP));
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

/* The substitute op as modpublished binds it: alex.gone is installed at HASH_A's build. */
static int g_subst_calls;
static int fake_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, char *why, size_t why_cap)
{
    g_subst_calls++;
    if (!strcmp(row->id, "alex.gone")) { strcpy_s(hash_out, hash_cap, HASH_A); return 1; }
    strcpy_s(why, why_cap, "This map needs nobody.here, which is not installed and was never published.");
    return 0;
}

static void editor_substitution(void)
{
    sh_modmanifest_list rows = {0};
    sh_modmanifest_row row;
    char err[256], sub[80], *text, *loaded;
    size_t tl = 0, ll = 0;
    static const char gone[] = "00000000000000000000000000000000000000000000000000000000000000ab";
    static const char none[] = "00000000000000000000000000000000000000000000000000000000000000cd";
    printf("-- an editor open serves a gone build's key through its installed build\n");
    g_installed = INSTALLED;
    g_installed_count = 2;
    memset(&row, 0, sizeof row);
    strcpy_s(row.id, sizeof row.id, "alex.gone");
    strcpy_s(row.version, sizeof row.version, "0.9.0");
    strcpy_s(row.bundle, sizeof row.bundle, gone);
    row.size = 10;
    CHECK(sh_modmanifest_list_add(&rows, &row));
    strcpy_s(row.id, sizeof row.id, "nobody.here");
    strcpy_s(row.bundle, sizeof row.bundle, none);
    CHECK(sh_modmanifest_list_add(&rows, &row));
    text = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &tl, err, sizeof err);
    CHECK(text != NULL);
    sh_modmanifest_set_substitute(fake_substitute);
    /* A published map's parse: never substituted. */
    g_subst_calls = 0;
    loaded = text ? sh_modmanifest_prepare_load(text, tl, &ll) : NULL;
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    CHECK(g_subst_calls == 0 && !sh_modmanifest_substituted(gone, sub, sizeof sub));
    /* The editor's parse: the installed build serves; the row keeps its declared build. */
    sh_modmanifest_set_editor_load(1);
    loaded = text ? sh_modmanifest_prepare_load(text, tl, &ll) : NULL;
    sh_modmanifest_set_editor_load(0);
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    CHECK(g_subst_calls == 2 && sh_modmanifest_substituted(gone, sub, sizeof sub) && !strcmp(sub, HASH_A));
    CHECK(!sh_modmanifest_substituted(none, sub, sizeof sub));
    CHECK(g_activate_count == 2 && (!strcmp(g_activated[0], HASH_A) || !strcmp(g_activated[1], HASH_A)));
    CHECK(strstr(g_last_log, "MODS: map manifest names 2 bundle(s)") != NULL);
    {
        sh_modmanifest_list cur;
        CHECK(sh_modmanifest_current(&cur) == 2 && !strcmp(cur.rows[0].bundle, gone));
        sh_modmanifest_list_free(&cur);
    }
    /* The next load starts over. */
    sh_modmanifest_set_editor_load(1);
    loaded = text ? sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &ll) : NULL;
    sh_modmanifest_set_editor_load(0);
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    CHECK(!sh_modmanifest_substituted(gone, sub, sizeof sub));
    sh_modmanifest_set_substitute(NULL);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_reset();
    g_installed_count = 0;
}

/* The follow op as modui binds it: OWN_OLD is an earlier hash of a folder
 * this machine made, now HASH_A; DL_OLD is a downloaded map's pin, which an
 * alias of a received folder answers but never rewrites. */
#define OWN_OLD "00000000000000000000000000000000000000000000000000000000000000e1"
#define DL_OLD  "00000000000000000000000000000000000000000000000000000000000000e2"
static int g_follow_calls;
static int fake_follow(const char *bundle_hash, const char *id, sh_modmanifest_row *out)
{
    (void)id;
    g_follow_calls++;
    if (strcmp(bundle_hash, OWN_OLD)) return 0;
    strcpy_s(out->id, sizeof out->id, "alex.own");
    strcpy_s(out->version, sizeof out->version, "1.1.0");
    strcpy_s(out->bundle, sizeof out->bundle, HASH_A);
    out->size = 4321;
    return 1;
}

static void own_folder_follow(void)
{
    sh_modmanifest_list rows = {0}, cur;
    sh_modmanifest_row row;
    char err[256], *text, *loaded;
    size_t tl = 0, ll = 0;
    printf("-- an editor load follows the author's own rebuilt folder; a downloaded pin stays\n");
    memset(&row, 0, sizeof row);
    strcpy_s(row.id, sizeof row.id, "alex.own");
    strcpy_s(row.version, sizeof row.version, "1.0.0");
    strcpy_s(row.bundle, sizeof row.bundle, OWN_OLD);
    row.size = 10;
    CHECK(sh_modmanifest_row_add_name(&row, "mods/alex.own/thing"));
    CHECK(sh_modmanifest_list_add(&rows, &row));
    sh_modmanifest_row_free(&row);
    memset(&row, 0, sizeof row);
    strcpy_s(row.id, sizeof row.id, "bob.dl");
    strcpy_s(row.version, sizeof row.version, "2.0.0");
    strcpy_s(row.bundle, sizeof row.bundle, DL_OLD);
    row.size = 20;
    CHECK(sh_modmanifest_list_add(&rows, &row));
    text = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &tl, err, sizeof err);
    CHECK(text != NULL);
    sh_modmanifest_set_follow(fake_follow);
    /* A published map's parse is not the author's editor: nothing is asked. */
    g_follow_calls = 0;
    loaded = text ? sh_modmanifest_prepare_load(text, tl, &ll) : NULL;
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    CHECK(g_follow_calls == 0);
    CHECK(sh_modmanifest_current(&cur) == 2 && !strcmp(cur.rows[0].bundle, OWN_OLD));
    sh_modmanifest_list_free(&cur);
    /* The editor's parse: the own row is rewritten, with its names; the downloaded one keeps its pin. */
    g_last_log[0] = 0;
    sh_modmanifest_set_editor_load(1);
    loaded = text ? sh_modmanifest_prepare_load(text, tl, &ll) : NULL;
    sh_modmanifest_set_editor_load(0);
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    CHECK(g_follow_calls == 2);
    CHECK(sh_modmanifest_current(&cur) == 2);
    CHECK(!strcmp(cur.rows[0].bundle, HASH_A) && !strcmp(cur.rows[0].version, "1.1.0") && cur.rows[0].size == 4321 &&
          cur.rows[0].name_count == 1 && !strcmp(cur.rows[0].names[0], "mods/alex.own/thing"));
    CHECK(!strcmp(cur.rows[1].bundle, DL_OLD) && !strcmp(cur.rows[1].version, "2.0.0"));
    sh_modmanifest_list_free(&cur);
    CHECK(g_activate_count == 2 && !strcmp(g_activated[0], HASH_A));
    sh_modmanifest_set_follow(NULL);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_reset();
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
    sh_modmanifest_reset();
    out = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n);
    CHECK(out != NULL && g_lookup_calls == 1);
    /* The open map's record follows the save (a map loaded with no rows, from a rawmap). */
    {
        sh_modmanifest_list cur;
        CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].bundle, HASH_A) && cur.rows[0].name_count == 2);
        sh_modmanifest_list_free(&cur);
    }
    if (out) {
        CHECK(sh_modmanifest_read(out, n, &back, NULL) == 1 && back.count == 1);
        CHECK(back.count == 1 && !strcmp(back.rows[0].bundle, HASH_A) && back.rows[0].name_count == 2 &&
              back.rows[0].size == 11782 && !strcmp(back.rows[0].version, "1.0.0"));
        CHECK(!strstr(out, "smpmod1.") && string_rows(out) == 2 && strstr(out, ",\"snapmap-plus\":{\"format\":1,\"mods\":[{"));
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
            sh_modmanifest_list cur;
            CHECK(clean && !strstr(clean, "snapmap-plus") && !strcmp(clean, MAP));
            CHECK(sh_modmanifest_current(&cur) == 0);   /* and its rows go with them */
            sh_modmanifest_list_free(&cur);
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
        CHECK(out && strstr(out, "\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"bob.room\"") &&
              string_rows(out) == 0 && alloc_string_count(out, &count) && count == 0);
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
    engine = written ? engine_text(sh_modmanifest_prepare_load(written, wlen, &elen), written, wlen, &elen) : NULL;
    CHECK(engine && !strcmp(engine, MAP) && g_activate_calls == 1 && g_activate_count == 2);
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

/* The asset rule at activation: a row whose pinned hash no folder carries is
 * activated through the installed build of its id; a row of an id nothing
 * installed declares keeps its own hash (the activation reports it unknown). */
static void activate_by_id(void)
{
    sh_modmanifest_list rows;
    sh_modmanifest_row row;
    printf("-- activation: a row pinning another build of an installed id activates the installed one\n");
    memset(&rows, 0, sizeof rows);
    memset(&row, 0, sizeof row);
    g_installed = INSTALLED;
    g_installed_count = 2;
    strcpy_s(row.id, sizeof row.id, INSTALLED[1].id);
    strcpy_s(row.version, sizeof row.version, "0.9");
    strcpy_s(row.bundle, sizeof row.bundle, "9999999999999999999999999999999999999999999999999999999999999999");
    CHECK(sh_modmanifest_list_add(&rows, &row));
    strcpy_s(row.id, sizeof row.id, "nobody.has-this");
    strcpy_s(row.bundle, sizeof row.bundle, "8888888888888888888888888888888888888888888888888888888888888888");
    CHECK(sh_modmanifest_list_add(&rows, &row));
    g_activate_calls = 0;
    CHECK(sh_modmanifest_activate(&rows) == 2 && g_activate_calls == 1 && g_activate_count == 2);
    CHECK(!strcmp(g_activated[0], INSTALLED[1].hash));
    CHECK(!strcmp(g_activated[1], "8888888888888888888888888888888888888888888888888888888888888888"));
    sh_modmanifest_list_free(&rows);
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
    engine = engine_text(sh_modmanifest_prepare_load(written, wlen, &elen), written, wlen, &elen);
    CHECK(engine != NULL && !strcmp(engine, MAP));
    /* The load hook sees both rows and the placed names, before activation. */
    CHECK(g_scope_calls == 1 && g_scope_when == SH_MODSERVE_AT_LOAD && g_scope_rows == 2 && g_scope_attached == 1 &&
          g_scope_names > 0);
    CHECK(g_activate_count == 2);
    CHECK(sh_modmanifest_current(&current) == 2 && sh_modmanifest_current_attached(&rows) == 1 &&
          !strcmp(rows.rows[0].bundle, HASH_C));
    sh_modmanifest_list_free(&current);
    sh_modmanifest_list_free(&rows);

    /* Armed 0: no placed names are read at a load. */
    g_scope_armed_value = 0;
    if (engine) { HeapFree(GetProcessHeap(), 0, engine); engine = NULL; }
    engine = engine_text(sh_modmanifest_prepare_load(written, wlen, &elen), written, wlen, &elen);
    CHECK(g_scope_calls == 2 && g_scope_names == 0 && g_scope_rows == 2);
    g_scope_armed_value = 1;

    /* The save keeps the map-level row (nothing places it), renamed to the
     * build that serves; the closure's row is written as before. */
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, "alex.pink-imp") && strstr(saved, HASH_D) && !strstr(saved, HASH_C));
    CHECK(saved && strstr(saved, "\"attached\":true"));
    CHECK(g_scope_when == SH_MODSERVE_AT_SAVE && g_scope_rows == 2 && g_scope_attached == 1);
    CHECK(sh_modmanifest_current_attached(&rows) == 1 && !strcmp(rows.rows[0].bundle, HASH_D) &&
          !strcmp(rows.rows[0].version, "1.1.0"));
    sh_modmanifest_list_free(&rows);
    if (saved) {
        /* Reloaded, the row is map-level again. */
        size_t again_len = 0;
        char *again = sh_modmanifest_prepare_load(saved, slen, &again_len);
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
    CHECK(saved && strstr(saved, "\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"erin.gone\"") && strstr(saved, HASH_E));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    if (engine) HeapFree(GetProcessHeap(), 0, engine);
    HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_row_free(&row);
    sh_modmanifest_set_scope(NULL, NULL);
    sh_modmanifest_set_resolver(NULL);
    sh_modmanifest_reset();
    g_installed_count = 2;
}

/* ---- registry choices (modregistry.h): the row member, the budget, load, choose, save ------- */

static int g_choice_calls, g_choice_n, g_choice_recompute;
static char g_choice_last[512];
int sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute)
{
    int i;
    size_t at = 0;
    g_choice_calls++;
    g_choice_n = n;
    g_choice_recompute = recompute;
    g_choice_last[0] = 0;
    for (i = 0; i < n && at + 2 < sizeof g_choice_last; i++) {
        int w = _snprintf_s(g_choice_last + at, sizeof g_choice_last - at, _TRUNCATE, "%s=%s;", slots[i], keys[i]);
        if (w < 0) break;
        at += (size_t)w;
    }
    return recompute ? 1 : 0;
}

static void choices(void)
{
    sh_modmanifest_row row, back;
    sh_modmanifest_list rows;
    char err[192], *value, *written, *engine, *saved, key[80], lines[SH_MODMANIFEST_CHOICES_CAP];
    size_t len, wlen, elen, slen, i;
    static const char *slots[] = { "weapons/weapon/zion/player/sp/chainsaw", "weapons/weapon/zion/player/sp/pistol",
                                   "weapons/weapon/zion/player/sp/gauss_rifle", "player.hands", "player.body" };

    /* The member round trip, entries in text order. */
    make_row(&row, "alex.pink-imp", "1.0.0", HASH_A, 11782);
    strcpy_s(row.choices, sizeof row.choices, "player.hands\talex.pink-imp\nweapons/weapon/zion/player/sp/pistol\talex.pink-imp\n");
    value = sh_modmanifest_format_value(&row, &len);
    CHECK(value && strstr(value, ",\"choices\":{\"player.hands\":\"alex.pink-imp\",\"weapons/weapon/zion/player/sp/pistol\":\"alex.pink-imp\"}"));
    CHECK(value && sh_modmanifest_parse_value(value, len, &back, err, sizeof err) && !strcmp(back.choices, row.choices));
    sh_modmanifest_row_free(&back);
    free(value);
    /* Entries that do not read are skipped; the row stays. */
    {
        static const char odd[] = "{\"id\":\"a.b\",\"version\":\"1\",\"bundle\":\"" HASH_A "\",\"size\":1,"
                                  "\"choices\":{\"player.hands\":\"Bad Id\",\"bad slot\":\"a.b\",\"player.body\":\"a.b\",\"x\":3},"
                                  "\"names\":[]}";
        CHECK(sh_modmanifest_parse_value(odd, sizeof odd - 1, &back, err, sizeof err) && !strcmp(back.choices, "player.body\ta.b\n"));
        sh_modmanifest_row_free(&back);
    }
    /* No budget: names and choices are all kept. */
    for (i = 0; i < 40; i++) {
        char name[96];
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/alex.pink-imp/a_rather_long_placed_name_number_%02zu", i);
        sh_modmanifest_row_add_name(&row, name);
    }
    row.choices[0] = 0;
    for (i = 0; i < 5; i++) {
        size_t at = strlen(row.choices);
        _snprintf_s(row.choices + at, sizeof row.choices - at, _TRUNCATE, "%s\talex.pink-imp\n", slots[i]);
    }
    value = sh_modmanifest_format_value(&row, &len);
    CHECK(value && len > 1024);
    CHECK(value && sh_modmanifest_parse_value(value, len, &back, err, sizeof err) && !strcmp(back.choices, row.choices) &&
          back.name_count == row.name_count);
    sh_modmanifest_row_free(&back);
    free(value);
    sh_modmanifest_row_free(&row);

    /* A load gives the choices to the serving index before the listing, the
     * first row naming a slot winning; no recompute (the listing follows). */
    memset(&rows, 0, sizeof rows);
    make_row(&row, "alex.pink-imp", "1.0.0", HASH_A, 11782);
    row.attached = 1;
    strcpy_s(row.choices, sizeof row.choices, "player.hands\talex.pink-imp\n");
    CHECK(sh_modmanifest_list_add(&rows, &row));
    sh_modmanifest_row_free(&row);
    make_row(&row, "bob.room", "2", HASH_B, 4096);
    row.attached = 1;
    strcpy_s(row.choices, sizeof row.choices, "player.hands\tbob.room\nplayer.body\tbob.room\n");
    CHECK(sh_modmanifest_list_add(&rows, &row));
    sh_modmanifest_row_free(&row);
    CHECK(sh_modmanifest_rows_choices(&rows, lines, sizeof lines) == 2 &&
          !strcmp(lines, "player.hands\talex.pink-imp\nplayer.body\tbob.room\n"));
    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    sh_modmanifest_list_free(&rows);
    CHECK(written != NULL);
    if (!written) return;
    sh_modmanifest_reset();
    g_installed_count = 0;
    g_choice_calls = 0;
    engine = engine_text(sh_modmanifest_prepare_load(written, wlen, &elen), written, wlen, &elen);
    CHECK(g_choice_calls == 1 && g_choice_n == 2 && g_choice_recompute == 0 &&
          !strcmp(g_choice_last, "player.hands=alex.pink-imp;player.body=bob.room;"));
    CHECK(sh_modmanifest_current_choice("player.hands", key, sizeof key) == 1 && !strcmp(key, "alex.pink-imp"));
    CHECK(sh_modmanifest_current_choice("player.handsD1", key, sizeof key) == 0 && !key[0]);

    /* A choice: recomputed at once; a cleared one is gone; an invalid one refused. */
    CHECK(sh_modmanifest_current_choose("player.hands", "bob.room") == 1 && g_choice_recompute == 1 &&
          !strcmp(g_choice_last, "player.body=bob.room;player.hands=bob.room;"));
    CHECK(sh_modmanifest_current_choose("weapons/weapon/zion/player/sp/pistol", "alex.pink-imp") == 1 && g_choice_n == 3);
    CHECK(sh_modmanifest_current_choose("player.body", NULL) == 1 && g_choice_n == 2);
    CHECK(sh_modmanifest_current_choose("player.body", "Bad Id") == -1 && sh_modmanifest_current_choose("bad slot", "a.b") == -1);
    CHECK(sh_modmanifest_current_choose("player.handsD1", "carol.gone") == 1);
    CHECK(sh_modmanifest_current_choices(lines, sizeof lines) == 3);

    /* The save writes each choice on the row of its key and drops one naming
     * a key the map declares no build of, with a line. */
    g_mods_lines = 0;
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, "\"choices\":{\"player.hands\":\"bob.room\"}"));
    CHECK(saved && strstr(saved, "\"choices\":{\"weapons/weapon/zion/player/sp/pistol\":\"alex.pink-imp\"}"));
    CHECK(saved && !strstr(saved, "carol.gone"));
    CHECK(g_mods_lines >= 1);
    if (saved) {
        /* Reloaded: the same choices (minus the dropped one). */
        char *again;
        g_choice_calls = 0;
        again = sh_modmanifest_prepare_load(saved, slen, &elen);
        CHECK(g_choice_calls == 1 && g_choice_n == 2 && sh_modmanifest_current_choice("player.hands", key, sizeof key) &&
              !strcmp(key, "bob.room"));
        if (again) HeapFree(GetProcessHeap(), 0, again);
        HeapFree(GetProcessHeap(), 0, saved);
    }
    /* A map without rows clears them. */
    {
        char *none = sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &elen);
        CHECK(g_choice_n == 0 && sh_modmanifest_current_choices(lines, sizeof lines) == 0);
        if (none) HeapFree(GetProcessHeap(), 0, none);
    }
    if (engine) HeapFree(GetProcessHeap(), 0, engine);
    HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_reset();
    g_installed_count = 2;
}

/* A stale "smpprod1.<version>" variable an earlier development build
 * recorded: a load and a save remove it (allocCount[4] rewritten); a map
 * without one passes through untouched. */
static void stale_product(void)
{
    static const char STALE[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,3],\"string\":["
        VAR("door_label", "Open") "," VAR("smnav1.x", "QUJD") "," VAR("smpprod1.v0.2.1-beta.24", "v0.2.1-beta.24")
        "]}}";
    static const char PLAIN[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
        VAR("door_label", "Open") "," VAR("smnav1.x", "QUJD")
        "]}}";
    char *loaded, *saved, *plain;
    size_t llen = 0, slen = 0, plen = 0;
    unsigned count = 0;
    printf("-- stale smpprod1: stripped at load and at save, nothing written\n");
    sh_modmanifest_reset();
    loaded = sh_modmanifest_prepare_load(STALE, sizeof STALE - 1, &llen);
    CHECK(loaded && llen == strlen(loaded) && !strstr(loaded, "smpprod1.") && strstr(loaded, "smnav1.x") &&
          strstr(loaded, "door_label") && alloc_string_count(loaded, &count) && count == 2 && string_rows(loaded) == 2);
    saved = sh_modmanifest_on_save(STALE, sizeof STALE - 1, &slen);
    CHECK(saved && slen == strlen(saved) && !strstr(saved, "smpprod1.") && strstr(saved, "smnav1.x") &&
          alloc_string_count(saved, &count) && count == 2 && string_rows(saved) == 2);
    /* A map without one: the strip path returns nothing, the caller keeps the map. */
    plain = sh_modmanifest_on_save(PLAIN, sizeof PLAIN - 1, &plen);
    CHECK(!plain && plen == 0);
    if (plain) HeapFree(GetProcessHeap(), 0, plain);
    plain = sh_modmanifest_prepare_load(PLAIN, sizeof PLAIN - 1, &plen);
    CHECK(!plain && plen == 0);
    if (plain) HeapFree(GetProcessHeap(), 0, plain);
    /* A save of the stripped map writes no variable back. */
    if (loaded) {
        char *again = sh_modmanifest_on_save(loaded, llen, &plen);
        CHECK(!again || !strstr(again, "smpprod1."));
        if (again) HeapFree(GetProcessHeap(), 0, again);
    }
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    sh_modmanifest_reset();
}

int main(void)
{
    printf("modmanifest_test\n");
    grammar();
    value_round_trip();
    text_round_trip_and_order();
    legacy_rows();
    member_carries_everything();
    all_parts();
    placed_names();
    save_closure();
    editor_substitution();
    own_folder_follow();
    activate_by_id();
    load_step();
    mod_json();
    map_level();
    choices();
    stale_product();
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
