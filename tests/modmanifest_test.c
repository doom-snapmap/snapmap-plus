/* modmanifest_test -- the map manifest: the salt and row grammar, the row round trip, the rows in the map's
 * "snapmap-plus" member (map_carrier.h), refused unsupported rows, placed names, the save-time closure, the load
 * step and map-level rows. The serving layer is replaced by doubles defined here. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modmanifest.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modsalt.h"
#include "../src/backend/mp_json.h"
#include "../src/backend/map_carrier.h"
#include "../src/backend/modsound.h"
#include "../src/backend/modpin.h"

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

/* `place`: where the build serves from (modserve.h sh_modserve_bundle_ref): NULL or "" a local root folder, else the
 * content cache (a composition, a local row's stand-ins). */
typedef struct installed { const char *salt, *name, *version, *hash; unsigned long long size; const char *names[4];
                           const char *place; } installed;
static const installed *g_installed;
static size_t g_installed_count;
static int g_lookup_calls;

/* The build that serves a row (sh_modserve_row_build): its own build when
 * installed, else the installed (root) build of its salt. */
void sh_modserve_bundle_ref_free(sh_modserve_bundle_ref *ref)
{
    if (!ref) return;
    free(ref->name);
    free(ref->folder);
    free(ref->place);
    ref->name = ref->folder = ref->place = NULL;
}

/* A local row's folder that is here but cannot be used (modserve.h): none here. */
int sh_modserve_salt_installed(const char *salt) { (void)salt; return 0; }
int sh_modserve_refusal_version(void) { return 0; }
int sh_modserve_root_lacks(const char *salt, const char *name) { (void)salt; (void)name; return 0; }
int sh_modserve_row_build(const sh_modmanifest_row *row, sh_modserve_bundle_ref *out)
{
    size_t b;
    if (out) memset(out, 0, sizeof *out);
    if (!row) return 0;
    for (b = 0; b < g_installed_count; b++)
        if (!strcmp(g_installed[b].hash, row->build)) break;
    if (b == g_installed_count)
        for (b = 0; b < g_installed_count; b++)
            if (!strcmp(g_installed[b].salt, row->salt)) break;
    if (b == g_installed_count) return 0;
    if (out) {
        strcpy_s(out->hash, sizeof out->hash, g_installed[b].hash);
        out->size = g_installed[b].size;
        out->place = _strdup(g_installed[b].place ? g_installed[b].place : "");
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
                    visit(context, g_installed[b].salt, g_installed[b].hash, g_installed[b].size, names[i]);
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
    { "alexpink", "Pink Imp", "1.0.0", HASH_A, 11782, { "mods/alexpink/imp", "mods/alexpink/skin", NULL, NULL } },
    { "bobroom1", "Room", "2", HASH_B, 4096, { "maps/modules/mods/bobroom1/hall.decl", NULL, NULL, NULL } },
};

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"

static const char MAP[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alexpink/imp\","
    "\"name\":\"\",\"state\":{\"edit\":{\"props\":{\"capDecl\":\"doors/industrial_cap_05\"},"
    "\"skins\":[\"MODS/ALEXPINK/SKIN\"],\"count\":3}}}},"
    "{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/visblockers/industrial/cap_01\","
    "\"name\":\"\",\"state\":{\"other\":\"not/a/name\"}}}],"
    "\"instances\":[{\"moduleName\":\"maps/modules/ind_dlc/ind_totally_blank_room_4x.decl\"}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
    VAR("door_label", "Open") "," VAR("lamp_label", "QUJD")
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
    mpj *root = mpj_parse(json, strlen(json));
    const mpj *bucket;
    unsigned count;
    if (!root) return (unsigned)-1;
    bucket = mpj_get(mpj_get(root, "variables"), "string");
    count = bucket && bucket->type == MPJ_ARR ? (unsigned)bucket->n : 0;
    mpj_free(root);
    return count;
}

/* The text the engine parses after a load: the family's legacy rows gone
 * (prepare_load's result, or the input when it removed none) and the member
 * stripped last, as rawmap.c prepare_map_buffer does. Process heap. */
static char *engine_text(const char *src, size_t n, size_t *out_len)
{
    size_t w = 0;
    sh_modmanifest_prepare_load(src, n);
    char *bare = sh_carrier_strip(src, n, &w);
    if (!bare) {
        bare = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
        memcpy(bare, src, n);
        bare[n] = 0;
        w = n;
    }
    if (out_len) *out_len = w;
    return bare;
}

static void make_row(sh_modmanifest_row *row, const char *salt, const char *version, const char *hash,
                     unsigned long long size)
{
    (void)version;   /* a mod has no version; the callers' labels are ignored */
    memset(row, 0, sizeof *row);
    strcpy_s(row->salt, sizeof row->salt, salt);
    strcpy_s(row->build, sizeof row->build, hash);
    row->size = size;
}

/* ---- tests --------------------------------------------------------------------------------- */

static void grammar(void)
{
    printf("-- salt and hash grammar\n");
    CHECK(sh_salt_valid("alexpink", 8) && sh_salt_valid("a2345678", 8) &&
          sh_salt_valid("zzzzzzzz", 8));
    CHECK(!sh_salt_valid("alexpin", 7) && !sh_salt_valid("alexpinkk", 9) &&
          !sh_salt_valid("1lexpink", 8) && !sh_salt_valid("Alexpink", 8) &&
          !sh_salt_valid("alex-ink", 8) && !sh_salt_valid("alex_ink", 8) &&
          !sh_salt_valid("alex.ink", 8) && !sh_salt_valid("", 0) && !sh_salt_valid(NULL, 8));
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
    make_row(&row, "alexpink", "1.0.0", HASH_A, 11782);
    sh_modmanifest_row_set_name(&row, "Pink Imp");
    CHECK(sh_modmanifest_row_add_name(&row, "mods/alexpink/imp"));
    CHECK(sh_modmanifest_row_add_name(&row, "MODS/ALEXPINK/IMP"));   /* duplicate, folded */
    CHECK(sh_modmanifest_row_add_name(&row, "a \"quoted\" \\ name"));
    CHECK(row.name_count == 2);
    value = sh_modmanifest_format_value(&row, &n);
    CHECK(value && n == strlen(value));
    {
        static const char expect[] = "{\"mod\":\"alexpink\",\"name\":\"Pink Imp\","
                                     "\"size\":11782,\"local\":true,\"names\":[\"mods/alexpink/imp\",";
        CHECK(value && !strncmp(value, expect, sizeof expect - 1) && !strstr(value, "\"build\""));
    }
    CHECK(value && sh_modmanifest_parse_value(value, n, &back, err, sizeof err));
    /* A local row follows its folder: its build is not written, and reads back as the placeholder. */
    CHECK(!strcmp(back.salt, row.salt) && !strcmp(back.build, SH_MODMANIFEST_LOCAL_BUILD) &&
          !strcmp(sh_modmanifest_row_name(&back), "Pink Imp"));
    CHECK(back.size == 11782 && back.name_count == 2 && !strcmp(back.names[1], "a \"quoted\" \\ name"));
    sh_modmanifest_row_free(&back);
    free(value);

    /* Refusals: wrong hash, bad salt, no "@", missing size, names not a list, a format 1 row ("<salt>@<build>");
     * an unknown member is kept out, a missing name is empty, a "build" member of a local row is not read. */
    {
        static const char short_hash[] = "{\"mod\":\"alexpink@abc\",\"size\":1}";
        static const char bad_salt[] = "{\"mod\":\"Alexpink@" HASH_A "\",\"size\":1}";
        static const char no_at[] = "{\"mod\":\"alexpink:" HASH_A "\",\"size\":1}";
        static const char no_size[] = "{\"mod\":\"alexpink\",\"local\":true}";
        static const char bad_names[] = "{\"mod\":\"alexpink\",\"local\":true,\"size\":1,\"names\":[1,}";
        static const char extra[] = "{\"mod\":\"alexpink\",\"local\":true,\"size\":1,\"future\":true}";
        static const char with_build[] = "{\"mod\":\"alexpink\",\"local\":true,\"build\":\"" HASH_A "\",\"size\":1}";
        static const char format1[] = "{\"mod\":\"alexpink@" HASH_A "\",\"size\":1,\"names\":[]}";
        CHECK(!sh_modmanifest_parse_value(short_hash, sizeof short_hash - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(bad_salt, sizeof bad_salt - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(no_at, sizeof no_at - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(format1, sizeof format1 - 1, &back, err, sizeof err) && strstr(err, "earlier Snapmap+"));
        CHECK(sh_modmanifest_parse_value(with_build, sizeof with_build - 1, &back, err, sizeof err) &&
              !strcmp(back.build, SH_MODMANIFEST_LOCAL_BUILD) && !strcmp(back.salt, "alexpink"));
        sh_modmanifest_row_free(&back);
        CHECK(!sh_modmanifest_parse_value(no_size, sizeof no_size - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(bad_names, sizeof bad_names - 1, &back, err, sizeof err));
        CHECK(sh_modmanifest_parse_value(extra, sizeof extra - 1, &back, err, sizeof err) && !sh_modmanifest_row_name(&back)[0] &&
              !strcmp(back.salt, "alexpink") && !strcmp(back.build, SH_MODMANIFEST_LOCAL_BUILD));
        sh_modmanifest_row_free(&back);
    }
    /* A row that names an "id" or a "bundle" is in an unsupported shape. */
    {
        static const char old_row[] = "{\"id\":\"alex.pink-imp\",\"version\":\"1\",\"bundle\":\"" HASH_A "\",\"size\":1}";
        static const char mixed[] = "{\"mod\":\"alexpink@" HASH_A "\",\"bundle\":\"" HASH_A "\",\"size\":1}";
        static const char with_id[] = "{\"mod\":\"alexpink@" HASH_A "\",\"id\":\"alexpink\",\"size\":1}";
        CHECK(!sh_modmanifest_parse_value(old_row, sizeof old_row - 1, &back, err, sizeof err) &&
              strstr(err, "earlier Snapmap+"));
        CHECK(!sh_modmanifest_parse_value(mixed, sizeof mixed - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(with_id, sizeof with_id - 1, &back, err, sizeof err));
    }
    /* A row without a valid salt or build is not formatted. */
    {
        sh_modmanifest_row bad;
        make_row(&bad, "bad-salt", "1", HASH_A, 1);
        CHECK(sh_modmanifest_format_value(&bad, &n) == NULL);
        make_row(&bad, "alexpink", "1", "abc", 1);
        CHECK(sh_modmanifest_format_value(&bad, &n) == NULL);
    }

    /* No row budget: every name is kept, however long the row. */
    {
        int i;
        char name[64];
        for (i = 0; i < 200; i++) {
            _snprintf_s(name, sizeof name, _TRUNCATE, "mods/alexpink/a/rather/long/name_%03d", i);
            sh_modmanifest_row_add_name(&row, name);
        }
        value = sh_modmanifest_format_value(&row, &n);
        CHECK(value && n > 4096 && strstr(value, "mods/alexpink/imp") && strstr(value, "name_199"));
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
    unsigned count = 0, rejected = 0;
    printf("-- rows written into the member, read; the variables untouched\n");

    make_row(&row, "alexpink", "1.0.0", HASH_A, 11782);
    sh_modmanifest_row_add_name(&row, "mods/alexpink/imp");
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    make_row(&row, "bobroom1", "2", HASH_B, 4096);
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);

    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    CHECK(written != NULL);
    if (!written) { fprintf(stderr, "write: %s\n", err); sh_modmanifest_list_free(&rows); return; }
    CHECK(wlen == strlen(written));
    /* The author's variables and allocCount are the engine's; the member is last at the root. */
    CHECK(string_rows(written) == 2 && alloc_string_count(written, &count) && count == 2);
    CHECK(!memcmp(written, MAP, sizeof MAP - 2));
    CHECK(!strcmp(written + sizeof MAP - 2,
                  ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"alexpink\",\"name\":\"\","
                  "\"size\":11782,\"local\":true,\"names\":[\"mods/alexpink/imp\"]},{\"mod\":\"bobroom1\","
                  "\"name\":\"\",\"size\":4096,\"local\":true,\"names\":[]}]}}"));

    CHECK(sh_modmanifest_read(written, wlen, &back, &rejected) == 2 && rejected == 0 && back.count == 2);
    CHECK(back.count == 2 && !strcmp(back.rows[0].salt, "alexpink") && !strcmp(back.rows[1].build, SH_MODMANIFEST_LOCAL_BUILD) &&
          back.rows[0].size == 11782 && back.rows[0].name_count == 1);
    sh_modmanifest_list_free(&back);

    /* The member strip gives the map back byte for byte. */
    stripped = sh_carrier_strip(written, wlen, &slen);
    CHECK(stripped && slen == sizeof MAP - 1 && strcmp(stripped, MAP) == 0);

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
    HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_list_free(&rows);
}

/* The save chain on one text: settings, then navigation, then the manifest,
 * each putting its part; the member carries all three in its order and the
 * load reads the rows back while the other parts are left to their readers. */
/* A member whose rows name an "id" and a "bundle" is in an unsupported shape (the parse detour refuses such a map
 * first, sh_map_old_shape): its rows do not read, and the refusal note is held as the load's refusal. */
static void old_shape_rows(void)
{
    static const char OLD[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
        "\"snapmap-plus\":{\"format\":1,\"mods\":[{\"id\":\"alex.pink-imp\",\"version\":\"1.0.0\",\"bundle\":\""
        HASH_A "\",\"size\":11782,\"names\":[\"mods/alex.pink-imp/imp\"]}]}}";
    sh_modmanifest_list rows = {0};
    unsigned rejected = 0;
    char refusal[200];
    printf("-- rows in an earlier shape: not read; a refused load holds its sentence\n");
    CHECK(sh_map_old_shape(OLD, sizeof OLD - 1) == SH_MAP_OLD_FORMAT);
    CHECK(sh_modmanifest_read(OLD, sizeof OLD - 1, &rows, &rejected) == 0 && rows.count == 0 && rejected == 0);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_reset();
    sh_modmanifest_load_refuse(SH_MAP_OLD_REFUSAL);
    CHECK(sh_modmanifest_load_notes_pending() == 1);
    CHECK(HO(refusal, sh_modmanifest_load_refusal(HO_P(0))) &&
          !strcmp(refusal, "This map was saved by an earlier Snapmap+; run the Snapmap+ installer to update it."));
    CHECK(sh_modmanifest_load_notes_flush(0) == 1 && !HO(refusal, sh_modmanifest_load_refusal(HO_P(0))));
    sh_modmanifest_reset();
}

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
        CHECK(!memcmp(saved, MAP, sizeof MAP - 2) && string_rows(saved) == 2);
        CHECK(strstr(saved, ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"alexpink\"") != NULL && strstr(saved, "\"build\":") == NULL);
        CHECK(sh_carrier_part(saved, sn, "settings", &from, &to) == 1 && to - from == sizeof SETTINGS - 1 &&
              !memcmp(saved + from, SETTINGS, to - from));
        CHECK(sh_carrier_part(saved, sn, "nav", &from, &to) == 1 && to - from == sizeof NAV - 1);
        CHECK(sh_modmanifest_read(saved, sn, &back, NULL) == 1 && !strcmp(back.rows[0].build, SH_MODMANIFEST_LOCAL_BUILD));
        sh_modmanifest_list_free(&back);
        engine = engine_text(saved, sn, &en);
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
    make_row(&row, "alexpink", "1.0.0", HASH_A, 1);
    for (i = 0; i < 100; i++) {
        char name[96];
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/alexpink/decls/a_fairly_long_entity_name_%03d", i);
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
        engine = engine_text(written, wlen, &elen);
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
        if (!strcmp(names[i], "mods/alexpink/imp")) has_imp = 1;
        if (!_stricmp(names[i], "mods/alexpink/skin")) has_skin = 1;
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

/* The substitute op as modpublished binds it: alexgone is installed at HASH_A's build. */
static int g_subst_calls;
static int fake_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed, char **why)
{
    (void)placed;
    g_subst_calls++;
    if (!strcmp(row->salt, "alexgone")) { strcpy_s(hash_out, hash_cap, HASH_A); return 1; }
    ho_set(why, "This map needs nobodyhr, which is not installed, and the mod store does not have it.");
    return 0;
}

static void editor_substitution(void)
{
    sh_modmanifest_list rows = {0};
    sh_modmanifest_row row;
    char err[256], sub[80], *text;
    size_t tl = 0;
    static const char gone[] = "00000000000000000000000000000000000000000000000000000000000000ab";
    static const char none[] = "00000000000000000000000000000000000000000000000000000000000000cd";
    printf("-- an editor open serves a build no place has through another build of its salt\n");
    g_installed = INSTALLED;
    g_installed_count = 2;
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, "alexgone");
    strcpy_s(row.build, sizeof row.build, gone);
    row.size = 10;
    CHECK(sh_modmanifest_list_add(&rows, &row));
    strcpy_s(row.salt, sizeof row.salt, "nobodyhr");
    strcpy_s(row.build, sizeof row.build, none);
    CHECK(sh_modmanifest_list_add(&rows, &row));
    text = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &tl, err, sizeof err);
    CHECK(text != NULL);
    sh_modmanifest_set_substitute(fake_substitute);
    /* A published map's parse: never substituted. */
    g_subst_calls = 0;
    if (text) sh_modmanifest_prepare_load(text, tl);
    CHECK(g_subst_calls == 0 && !sh_modmanifest_substituted(SH_MODMANIFEST_LOCAL_BUILD, sub, sizeof sub));
    /* The editor's parse: the installed build serves. A local row declares no build (the placeholder), which is what
     * the substitution is recorded against. */
    sh_modmanifest_set_editor_load(1);
    if (text) sh_modmanifest_prepare_load(text, tl);
    sh_modmanifest_set_editor_load(0);
    CHECK(g_subst_calls == 2 && sh_modmanifest_substituted(SH_MODMANIFEST_LOCAL_BUILD, sub, sizeof sub) && !strcmp(sub, HASH_A));
    CHECK(!sh_modmanifest_substituted(none, sub, sizeof sub));
    CHECK(g_activate_count == 2 && (!strcmp(g_activated[0], HASH_A) || !strcmp(g_activated[1], HASH_A)));
    CHECK(strstr(g_last_log, "MODS: map manifest names 2 build(s)") != NULL);
    {
        sh_modmanifest_list cur;
        CHECK(sh_modmanifest_current(&cur) == 2 && !strcmp(cur.rows[0].build, SH_MODMANIFEST_LOCAL_BUILD));
        sh_modmanifest_list_free(&cur);
    }
    /* The next load starts over. */
    sh_modmanifest_set_editor_load(1);
    if (text) sh_modmanifest_prepare_load(MAP, sizeof MAP - 1);
    sh_modmanifest_set_editor_load(0);
    CHECK(!sh_modmanifest_substituted(SH_MODMANIFEST_LOCAL_BUILD, sub, sizeof sub));
    sh_modmanifest_set_substitute(NULL);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_reset();
    g_installed_count = 0;
}

static int label_requests;

static void count_labels(const char *const *names, size_t count, char **labels)
{
    (void)names; (void)count; (void)labels;
    label_requests++;
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
    {
        const char stock[] = "{\"entities\":[{\"entityDef\":{\"inherit\":\"stock/player_start\"}}]}";
        sh_modmanifest_set_labeler(count_labels);
        CHECK(sh_modmanifest_on_save(stock, sizeof stock - 1, &n) == NULL && label_requests == 0);
        g_lookup_calls = 0;
    }
    out = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n);
    CHECK(label_requests == 1);
    sh_modmanifest_set_labeler(NULL);
    CHECK(out != NULL && g_lookup_calls == 1);
    /* The open map's record follows the save (a map loaded with no rows, from a rawmap). */
    {
        sh_modmanifest_list cur;
        CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].build, HASH_A) && cur.rows[0].name_count == 2);
        sh_modmanifest_list_free(&cur);
    }
    if (out) {
        CHECK(sh_modmanifest_read(out, n, &back, NULL) == 1 && back.count == 1);
        CHECK(back.count == 1 && !strcmp(back.rows[0].build, SH_MODMANIFEST_LOCAL_BUILD) && back.rows[0].name_count == 2 &&
              back.rows[0].size == 11782 && !strcmp(back.rows[0].salt, "alexpink"));
        CHECK(string_rows(out) == 2 && strstr(out, ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"alexpink\""));
        sh_modmanifest_list_free(&back);

        /* Saving the saved text again gives the same bytes. */
        {
            size_t n2 = 0;
            char *again = sh_modmanifest_on_save(out, n, &n2);
            CHECK(again && n2 == n && !strcmp(again, out));
            if (again) HeapFree(GetProcessHeap(), 0, again);
        }
        /* No installed build serves the placed imp any more (its mod failed to
         * build, or its folder is gone): the save keeps the row the map has,
         * so the next load still declares what it places. */
        g_installed_count = 0;
        {
            size_t n2 = 0;
            char *kept = sh_modmanifest_on_save(out, n, &n2);
            sh_modmanifest_list cur;
            CHECK(kept && strstr(kept, "\"mod\":\"alexpink\"") != NULL && strstr(kept, "\"build\":") == NULL);
            CHECK(sh_modmanifest_current(&cur) == 1 && cur.count == 1 && !strcmp(cur.rows[0].salt, "alexpink"));
            sh_modmanifest_list_free(&cur);
            if (kept) HeapFree(GetProcessHeap(), 0, kept);
        }
        /* Nothing of it is placed any more: the row goes, no row is added. */
        {
            static const char bare[] = "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\","
                                       "\"inherit\":\"snapmaps/visblockers/industrial/cap_01\",\"name\":\"\"}}],"
                                       "\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
            size_t n2 = 0;
            char *clean = sh_modmanifest_on_save(bare, sizeof bare - 1, &n2);
            sh_modmanifest_list cur;
            (void)cur;
            CHECK(!clean || !strstr(clean, "snapmap-plus"));
            if (clean) HeapFree(GetProcessHeap(), 0, clean);
        }
        /* A save step that fails (the serving layer names an invalid mod) never
         * writes the map without its rows: the open map's rows are kept. */
        {
            static const installed BAD[] = {
                { "alexpink", "Pink Imp", "1.0.0", HASH_A, 11782, { "mods/alexpink/imp", NULL, NULL, NULL } },
                { "NOT A SALT", "Bad", "1", HASH_B, 1, { "mods/alexpink/skin", NULL, NULL, NULL } },
            };
            size_t n2 = 0;
            char *first, *kept;
            g_installed = INSTALLED;
            g_installed_count = 1;
            first = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n2);
            CHECK(first && strstr(first, "\"mod\":\"alexpink\"") != NULL);
            g_installed = BAD;
            g_installed_count = 2;
            kept = first ? sh_modmanifest_on_save(first, n2, &n2) : NULL;
            CHECK(kept && strstr(kept, "\"mod\":\"alexpink\"") != NULL && strstr(kept, "\"build\":") == NULL);
            if (first) HeapFree(GetProcessHeap(), 0, first);
            if (kept) HeapFree(GetProcessHeap(), 0, kept);
            g_installed = INSTALLED;
            sh_modmanifest_reset();
        }
        HeapFree(GetProcessHeap(), 0, out);
    }
    /* No bundle and no stale row: the engine's bytes are saved unchanged. */
    g_installed_count = 0;
    CHECK(sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n) == NULL);
    /* A placed module provided by a bundle. */
    {
        static const char room[] = "{\"instances\":[{\"moduleName\":\"maps/modules/mods/bobroom1/hall.decl\"}],"
                                   "\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
        unsigned count = 9;
        g_installed_count = 2;
        out = sh_modmanifest_on_save(room, sizeof room - 1, &n);
        CHECK(out && strstr(out, "\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"bobroom1\"") &&
              string_rows(out) == 0 && alloc_string_count(out, &count) && count == 0);
        if (out) HeapFree(GetProcessHeap(), 0, out);
    }
}

/* A resized Grid Room and a navigation-marked Blocking Box are saved with no row and no tag. What a map uses of
 * Snapmap+ itself is the product's own entry of the rows: written by a save while the map uses it, gone when it
 * does not, never a row (nothing to download). */
static unsigned g_uses_answer;
static unsigned fake_uses(const char *json, size_t length, const char *const *names, size_t count)
{
    (void)json; (void)length; (void)names; (void)count;
    return g_uses_answer;
}

static int stock_actor_use(const char *json, size_t length)
{
    (void)length;
    return strstr(json, "\"idAI2\"") != NULL;
}

static void product_entry(void)
{
    static const char room[] = "{\"instances\":[{\"moduleName\":\"maps/modules/smpgrid/v1/m/1536_1536_512.decl\"}],"
                               "\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
    sh_modmanifest_list back = {0};
    unsigned rejected = 9;
    size_t n = 0, n2 = 0;
    char *out, *again;
    printf("-- what a map uses of Snapmap+ itself: the product's entry, written and removed by the save\n");
    g_installed_count = 0;
    sh_modmanifest_reset();
    sh_modmanifest_set_uses(fake_uses);
    g_uses_answer = SH_MODMANIFEST_USES_MODULE_SIZES | SH_MODMANIFEST_USES_NAVIGATION;
    out = sh_modmanifest_on_save(room, sizeof room - 1, &n);
    CHECK(out && strstr(out, "\"mods\":[{\"mod\":\"snapmap-plus\",\"name\":\"Snapmap+\",\"uses\":[\"module-sizes\",\"navigation\"]}]"));
    if (out) {
        CHECK(sh_modmanifest_read(out, n, &back, &rejected) == 0 && rejected == 0 && back.count == 0);   /* not a row */
        CHECK(back.uses == (SH_MODMANIFEST_USES_MODULE_SIZES | SH_MODMANIFEST_USES_NAVIGATION));
        sh_modmanifest_list_free(&back);
        /* The room back at its stock size and the box unmarked: the entry goes. */
        g_uses_answer = 0;
        again = sh_modmanifest_on_save(out, n, &n2);
        CHECK(again && !strstr(again, "snapmap-plus\",\"name\"") && !strstr(again, "\"mods\""));
        if (again) HeapFree(GetProcessHeap(), 0, again);
        HeapFree(GetProcessHeap(), 0, out);
    }
    /* With a mod's row the entry follows the rows. */
    g_installed = INSTALLED;
    g_installed_count = 2;
    g_uses_answer = SH_MODMANIFEST_USES_NAVIGATION;
    out = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &n);
    CHECK(out && strstr(out, "{\"mod\":\"alexpink\"") && strstr(out, ",{\"mod\":\"snapmap-plus\",\"name\":\"Snapmap+\",\"uses\":[\"navigation\"]}]"));
    if (out) {
        memset(&back, 0, sizeof back);
        CHECK(sh_modmanifest_read(out, n, &back, &rejected) == 1 && rejected == 0 && back.uses == SH_MODMANIFEST_USES_NAVIGATION);
        sh_modmanifest_list_free(&back);
        HeapFree(GetProcessHeap(), 0, out);
    }
    /* Vanilla: nothing used, nothing installed serves the map: the engine's bytes, unchanged. */
    g_installed_count = 0;
    g_uses_answer = 0;
    CHECK(sh_modmanifest_on_save(room, sizeof room - 1, &n) == NULL);
    sh_modmanifest_set_uses(NULL);
    {
        const char actor[] = "{\"entities\":[{\"entityDef\":{\"inherit\":\"ai/demon/baron_coop\",\"className\":\"idAI2\"}}]}";
        sh_modmanifest_set_actor_rows(NULL, stock_actor_use, NULL);
        out = sh_modmanifest_on_save(actor, sizeof actor-1, &n);
        CHECK(out && strstr(out, "\"uses\":[\"direct-actors\"]"));
        CHECK(out && sh_modmanifest_read(out,n,&back,&rejected)==0 && !rejected &&
              back.uses==SH_MODMANIFEST_USES_DIRECT_ACTORS && !back.count);
        sh_modmanifest_list_free(&back);
        if (out) HeapFree(GetProcessHeap(),0,out);
        CHECK(sh_modmanifest_on_save(room,sizeof room-1,&n)==NULL);
        sh_modmanifest_set_actor_rows(NULL,NULL,NULL);
    }
    g_installed = INSTALLED;
    sh_modmanifest_reset();
}

static void load_step(void)
{
    sh_modmanifest_list rows = {0}, current = {0};
    sh_modmanifest_row row;
    char err[192], *written, *engine;
    size_t wlen = 0, elen = 0;
    printf("-- load step: keep, strip, activate\n");
    make_row(&row, "alexpink", "1.0.0", HASH_A, 5);
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    make_row(&row, "bobroom1", "2", HASH_B, 6);
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    CHECK(written != NULL);
    g_activate_calls = 0;
    engine = written ? engine_text(written, wlen, &elen) : NULL;
    CHECK(engine && !strcmp(engine, MAP) && g_activate_calls == 1 && g_activate_count == 2);
    CHECK(!strcmp(g_activated[0], HASH_A) && !strcmp(g_activated[1], HASH_B));
    CHECK(sh_modmanifest_current(&current) == 2 && !strcmp(current.rows[1].salt, "bobroom1"));
    sh_modmanifest_list_free(&current);
    if (engine) HeapFree(GetProcessHeap(), 0, engine);

    /* A map without rows: the engine's bytes, activation with nothing, the manifest forgotten. */
    g_activate_calls = 0;
    sh_modmanifest_prepare_load(MAP, sizeof MAP - 1);
    CHECK(g_activate_calls == 1 && g_activate_count == 0);
    CHECK(sh_modmanifest_current(&current) == 0);

    /* Unreadable rows are dropped (a bad salt, a format 1 "<salt>@<build>" row), the readable one still activates. */
    {
        static const char mixed[] = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
            "\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"Bad\"},"
            "{\"mod\":\"alexpink@" HASH_A "\",\"size\":5,\"names\":[]},"
            "{\"mod\":\"bobroom1\",\"size\":6,\"local\":true,\"names\":[]}]}}";
        g_activate_calls = 0;
        sh_modmanifest_prepare_load(mixed, sizeof mixed - 1);
        CHECK(g_activate_count == 1 && !strcmp(g_activated[0], HASH_B));
        CHECK(strstr(g_last_log, "2 manifest row(s) did not parse") != NULL);
    }
    if (written) HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_reset();
}

static void mod_json(void)
{
    static const char text[] =
        "\xef\xbb\xbf{\n  \"version\": \"1.0.0\", \"name\": \"Pink Imp\", \"x-note\": [1],\n"
        "  \"snapmap-plus\": {\"format\": 2, \"salt\": \"alexpink\", \"rules\": 8, \"build\": \"" HASH_A "\",\n"
        "  \"files\": [\n"
        "    {\"path\": \"assets/generated/decls/md6def/mods/alexpink/imp.md6.decl\", \"sha256\": \"" HASH_B "\","
        " \"size\": 11782, \"provides\": {\"type\": \"md6Def\", \"name\": \"mods/alexpink/imp.md6\"}, \"needs\": []},\n"
        "    {\"path\": \"assets/x.bin\", \"sha256\": \"" HASH_B "\", \"size\": 18, \"provides\": {\"type\": \"file\", \"name\": \"x.bin\"}}\n"
        "  ],\n  \"stock\": [{\"path\": \"assets/maps/a.sbsp\", \"sha256\": \"" HASH_B "\", \"source\": \"maps/a.sbsp\","
        " \"provides\": {\"type\": \"file\", \"name\": \"maps/a.sbsp\"}, \"needs\": []}]}\n}\n";
    sh_modmanifest_row info;
    char err[160];
    printf("-- mod.json reader\n");
    CHECK(sh_modmanifest_read_mod_json(text, sizeof text - 1, &info, err, sizeof err));
    CHECK(!strcmp(info.salt, "alexpink") && !strcmp(info.build, HASH_A) &&
          !strcmp(info.name, "Pink Imp"));
    CHECK(info.size == 11800 && info.name_count == 3 && !strcmp(info.names[0], "mods/alexpink/imp.md6"));
    sh_modmanifest_row_free(&info);
    {
        static const char unbuilt[] = "{\"version\":\"1\",\"name\":\"a\"}";
        static const char failing[] = "{\"name\":\"a\",\"snapmap-plus\":{\"format\":2,\"salt\":\"alexpink\","
                                      "\"cause\":\"it does not build\",\"files\":[]}}";
        static const char nosalt[] = "{\"name\":\"a\",\"snapmap-plus\":{\"format\":1,\"bundle\":\"" HASH_A "\",\"files\":[]}}";
        static const char noname[] = "{\"snapmap-plus\":{\"format\":2,\"salt\":\"alexpink\",\"build\":\"" HASH_A "\",\"files\":[]}}";
        CHECK(!sh_modmanifest_read_mod_json(unbuilt, sizeof unbuilt - 1, &info, err, sizeof err));
        CHECK(!sh_modmanifest_read_mod_json(failing, sizeof failing - 1, &info, err, sizeof err) && strstr(err, "not been built"));
        CHECK(!sh_modmanifest_read_mod_json(nosalt, sizeof nosalt - 1, &info, err, sizeof err) && strstr(err, "not been scanned"));
        CHECK(!sh_modmanifest_read_mod_json(noname, sizeof noname - 1, &info, err, sizeof err));
    }
    /* A 2,000-character name (four-byte characters, 8,000 bytes), padded
     * with spaces as an author may write it: read whole, written into a row
     * and read back whole, never cut. No length bound applies. */
    {
        static char name[8001], text[9000];
        char *row;
        size_t i, len = 0;
        sh_modmanifest_row back;
        for (i = 0; i < 2000; i++) memcpy(name + i * 4, "\xf0\x9f\x98\x80", 4);
        name[8000] = 0;
        _snprintf_s(text, sizeof text, _TRUNCATE, "{\"name\":\"   %s  \",\"snapmap-plus\":{\"format\":2,\"salt\":\"alexpink\","
                    "\"build\":\"" HASH_A "\",\"files\":[]}}", name);
        CHECK(sh_modmanifest_read_mod_json(text, strlen(text), &info, err, sizeof err) && !strcmp(info.name, name));
        row = sh_modmanifest_format_value(&info, &len);
        CHECK(row && sh_modmanifest_parse_value(row, len, &back, err, sizeof err) && !strcmp(sh_modmanifest_row_name(&back), name));
        free(row);
        sh_modmanifest_row_free(&back);
        sh_modmanifest_row_free(&info);
    }
}

/* Activation: a row is activated through the build sh_modserve_row_build
 * answers (here the installed build of its salt at another hash); a row
 * nothing serves keeps its own hash (the activation reports it unknown). */
static void activate_by_id(void)
{
    sh_modmanifest_list rows;
    sh_modmanifest_row row;
    printf("-- activation: through the build that serves the row\n");
    memset(&rows, 0, sizeof rows);
    memset(&row, 0, sizeof row);
    g_installed = INSTALLED;
    g_installed_count = 2;
    strcpy_s(row.salt, sizeof row.salt, INSTALLED[1].salt);
    strcpy_s(row.build, sizeof row.build, "9999999999999999999999999999999999999999999999999999999999999999");
    CHECK(sh_modmanifest_list_add(&rows, &row));
    strcpy_s(row.salt, sizeof row.salt, "nobodyht");
    strcpy_s(row.build, sizeof row.build, "8888888888888888888888888888888888888888888888888888888888888888");
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

/* alexhand serves as a newer build (HASH_D); alexpink and bobroom1 serve as
 * installed; HASH_E is not installed. */
static int fake_resolve(const char *build, const char *salt, sh_modmanifest_row *out)
{
    size_t b;
    if (salt && !strcmp(salt, "alexhand")) {
        strcpy_s(out->salt, sizeof out->salt, "alexhand");
        sh_modmanifest_row_set_name(out, "Bare Hands");
        strcpy_s(out->build, sizeof out->build, HASH_D);
        out->size = 777;
        return 1;
    }
    for (b = 0; b < g_installed_count; b++)
        if (salt && build && !strcmp(g_installed[b].salt, salt) && !strcmp(g_installed[b].hash, build)) {
            strcpy_s(out->salt, sizeof out->salt, salt);
            sh_modmanifest_row_set_name(out, g_installed[b].name);
            strcpy_s(out->build, sizeof out->build, build);
            out->size = g_installed[b].size;
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
    printf("-- map-level rows: the attached member, load, save, attach, remove\n");
    /* The key round-trips and is written only for a map-level row. */
    make_row(&row, "alexhand", "1.0.0", HASH_C, 512);
    value = sh_modmanifest_format_value(&row, &vlen);
    CHECK(value && !strstr(value, "attached"));
    free(value);
    row.attached = 1;
    value = sh_modmanifest_format_value(&row, &vlen);
    CHECK(value && !strcmp(value, "{\"mod\":\"alexhand\",\"name\":\"\","
                                  "\"size\":512,\"local\":true,\"names\":[],\"attached\":true}"));
    CHECK(value && sh_modmanifest_parse_value(value, vlen, &back, err, sizeof err) && back.attached == 1);
    sh_modmanifest_row_free(&back);
    free(value);
    CHECK(sh_modmanifest_parse_value("{\"mod\":\"aaaabbbb\",\"size\":1,\"local\":true,\"attached\":false}",
                                     strlen("{\"mod\":\"aaaabbbb\",\"size\":1,\"local\":true,\"attached\":false}"),
                                     &back, err, sizeof err) && back.attached == 0);
    sh_modmanifest_row_free(&back);

    /* A map with a placed mod's row and a map-level row. */
    memset(&rows, 0, sizeof rows);
    {
        sh_modmanifest_row placed;
        make_row(&placed, "alexpink", "1.0.0", HASH_A, 11782);
        sh_modmanifest_row_add_name(&placed, "mods/alexpink/imp");
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
    engine = engine_text(written, wlen, &elen);
    CHECK(engine != NULL && !strcmp(engine, MAP));
    /* The load hook sees both rows and the placed names, before activation. */
    CHECK(g_scope_calls == 1 && g_scope_when == SH_MODSERVE_AT_LOAD && g_scope_rows == 2 && g_scope_attached == 1 &&
          g_scope_names > 0);
    CHECK(g_activate_count == 2);
    CHECK(sh_modmanifest_current(&current) == 2 && sh_modmanifest_current_attached(&rows) == 1 &&
          !strcmp(rows.rows[0].build, SH_MODMANIFEST_LOCAL_BUILD));
    sh_modmanifest_list_free(&current);
    sh_modmanifest_list_free(&rows);

    /* Armed 0: no placed names are read at a load. */
    g_scope_armed_value = 0;
    if (engine) { HeapFree(GetProcessHeap(), 0, engine); engine = NULL; }
    engine = engine_text(written, wlen, &elen);
    CHECK(g_scope_calls == 2 && g_scope_names == 0 && g_scope_rows == 2);
    g_scope_armed_value = 1;

    /* The save keeps the map-level row (nothing places it), renamed to the
     * build that serves; the closure's row is written as before. */
    {
        const char *placed[] = {"mods/alexpink/imp"};
        CHECK(sh_modmanifest_rows_for(placed, 1, &rows) == 2 && !strcmp(rows.rows[0].salt, "alexpink"));
        sh_modmanifest_list_free(&rows);
    }
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, "alexpink") && strstr(saved, "alexhand") && !strstr(saved, "\"build\":") &&
          strstr(saved, "\"name\":\"Bare Hands\"") && strstr(saved, "\"name\":\"Pink Imp\""));
    CHECK(saved && strstr(saved, "\"attached\":true"));
    CHECK(g_scope_when == SH_MODSERVE_AT_SAVE && g_scope_rows == 2 && g_scope_attached == 1);
    CHECK(sh_modmanifest_current_attached(&rows) == 1 && !strcmp(rows.rows[0].build, HASH_D));
    sh_modmanifest_list_free(&rows);
    if (saved) {
        /* Reloaded, the row is map-level again. */
        sh_modmanifest_prepare_load(saved, slen);
        CHECK(sh_modmanifest_current(&rows) == 2 && !strcmp(rows.rows[0].salt, "alexpink"));
        sh_modmanifest_list_free(&rows);
        CHECK(sh_modmanifest_current_attached(&rows) == 1 && rows.rows[0].attached);
        sh_modmanifest_list_free(&rows);

        HeapFree(GetProcessHeap(), 0, saved);
    }

    /* Attach: a new row, then again (already attached), then a row of a mod
     * not installed, which a save keeps as it is. */
    make_row(&back, "bobroom1", "2", HASH_B, 4096);
    CHECK(sh_modmanifest_current_attach(&back) == 1 && sh_modmanifest_current_attach(&back) == 0);
    sh_modmanifest_row_free(&back);
    make_row(&back, "eringone", "3", HASH_E, 9);
    sh_modmanifest_row_add_name(&back, "x/y");
    CHECK(sh_modmanifest_current_attach(&back) == 1);
    sh_modmanifest_row_free(&back);
    make_row(&back, "Bad", "3", HASH_E, 9);
    CHECK(sh_modmanifest_current_attach(&back) == -1);
    sh_modmanifest_row_free(&back);
    CHECK(sh_modmanifest_current_attached(&rows) == 3);
    sh_modmanifest_list_free(&rows);
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, "bobroom1") && strstr(saved, "eringone") && !strstr(saved, "\"build\":"));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* Remove from map drops the row; the next save writes none for it. */
    CHECK(sh_modmanifest_current_remove(HASH_D) == 1);
    CHECK(sh_modmanifest_current_remove(HASH_E) == 1);
    CHECK(sh_modmanifest_current_remove(HASH_B) == 1);
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && !strstr(saved, "alexhand") && !strstr(saved, "bobroom1") && strstr(saved, "alexpink"));
    CHECK(g_scope_attached == 0 && g_scope_rows == 1);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* A map-level row (nothing served for it) is still written, beside the placed imp's row,
     * which no build serves and is kept as written. */
    g_installed_count = 0;
    make_row(&back, "eringone", "3", HASH_E, 9);
    CHECK(sh_modmanifest_current_attach(&back) == 1);
    sh_modmanifest_row_free(&back);
    saved = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &slen);
    CHECK(saved && strstr(saved, "{\"mod\":\"eringone\"") && !strstr(saved, "\"build\":") && strstr(saved, "{\"mod\":\"alexpink\""));
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
/* The ported demons' types the serving layer gives (mm_choices_save pins them): set by a test. */
static const char *g_pin_salt[4], *g_pin_slot[4];
static int g_npins;
int sh_modserve_spawn_pins(void (*fn)(void *context, const char *salt, const char *slot), void *context)
{
    int i;
    for (i = 0; i < g_npins; i++) if (fn) fn(context, g_pin_salt[i], g_pin_slot[i]);
    return g_npins;
}

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
    char err[192], *value, *written, *engine, *saved, key[80], lines[HO_TEXT];
    size_t len, wlen, elen, slen, i;
    static const char *slots[] = { "weapons/weapon/zion/player/sp/chainsaw", "weapons/weapon/zion/player/sp/pistol",
                                   "weapons/weapon/zion/player/sp/gauss_rifle", "player.hands", "player.body" };

    /* The member round trip, entries in text order. */
    make_row(&row, "alexpink", "1.0.0", HASH_A, 11782);
    free(row.choices);
    row.choices = _strdup("player.hands\talexpink\nweapons/weapon/zion/player/sp/pistol\talexpink\n");
    value = sh_modmanifest_format_value(&row, &len);
    CHECK(value && strstr(value, ",\"choices\":{\"player.hands\":\"alexpink\",\"weapons/weapon/zion/player/sp/pistol\":\"alexpink\"}"));
    CHECK(value && sh_modmanifest_parse_value(value, len, &back, err, sizeof err) && !strcmp(back.choices, row.choices));
    sh_modmanifest_row_free(&back);
    free(value);
    /* Entries that do not read are skipped; the row stays. */
    {
        static const char odd[] = "{\"mod\":\"aaaabbbb\",\"local\":true,\"size\":1,"
                                  "\"choices\":{\"player.hands\":\"Bad Id\",\"bad slot\":\"aaaabbbb\",\"player.body\":\"aaaabbbb\",\"x\":3},"
                                  "\"names\":[]}";
        CHECK(sh_modmanifest_parse_value(odd, sizeof odd - 1, &back, err, sizeof err) && !strcmp(back.choices, "player.body\taaaabbbb\n"));
        sh_modmanifest_row_free(&back);
    }
    /* No budget: names and choices are all kept. */
    for (i = 0; i < 40; i++) {
        char name[96];
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/alexpink/a_rather_long_placed_name_number_%02zu", i);
        sh_modmanifest_row_add_name(&row, name);
    }
    {
        char lines[1024] = "";
        for (i = 0; i < 5; i++) {
            size_t at = strlen(lines);
            _snprintf_s(lines + at, sizeof lines - at, _TRUNCATE, "%s\talexpink\n", slots[i]);
        }
        free(row.choices);
        row.choices = _strdup(lines);
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
    make_row(&row, "alexpink", "1.0.0", HASH_A, 11782);
    row.attached = 1;
    free(row.choices);
    row.choices = _strdup("player.hands\talexpink\n");
    CHECK(sh_modmanifest_list_add(&rows, &row));
    sh_modmanifest_row_free(&row);
    make_row(&row, "bobroom1", "2", HASH_B, 4096);
    row.attached = 1;
    free(row.choices);
    row.choices = _strdup("player.hands\tbobroom1\nplayer.body\tbobroom1\n");
    CHECK(sh_modmanifest_list_add(&rows, &row));
    sh_modmanifest_row_free(&row);
    CHECK(HOZ(lines, sh_modmanifest_rows_choices(&rows, HO_P(0))) == 2 &&
          !strcmp(lines, "player.hands\talexpink\nplayer.body\tbobroom1\n"));
    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    sh_modmanifest_list_free(&rows);
    CHECK(written != NULL);
    if (!written) return;
    sh_modmanifest_reset();
    g_installed_count = 0;
    g_choice_calls = 0;
    engine = engine_text(written, wlen, &elen);
    CHECK(g_choice_calls == 1 && g_choice_n == 2 && g_choice_recompute == 0 &&
          !strcmp(g_choice_last, "player.hands=alexpink;player.body=bobroom1;"));
    CHECK(sh_modmanifest_current_choice("player.hands", key, sizeof key) == 1 && !strcmp(key, "alexpink"));
    CHECK(sh_modmanifest_current_choice("player.handsD1", key, sizeof key) == 0 && !key[0]);

    /* A choice: recomputed at once; a cleared one is gone; an invalid one refused. */
    CHECK(sh_modmanifest_current_choose("player.hands", "bobroom1") == 1 && g_choice_recompute == 1 &&
          !strcmp(g_choice_last, "player.body=bobroom1;player.hands=bobroom1;"));
    CHECK(sh_modmanifest_current_choose("weapons/weapon/zion/player/sp/pistol", "alexpink") == 1 && g_choice_n == 3);
    CHECK(sh_modmanifest_current_choose("player.body", NULL) == 1 && g_choice_n == 2);
    CHECK(sh_modmanifest_current_choose("player.body", "Bad Id") == -1 && sh_modmanifest_current_choose("bad slot", "aaaabbbb") == -1);
    CHECK(sh_modmanifest_current_choose("player.handsD1", "carolgon") == 1);
    CHECK(HOZ(lines, sh_modmanifest_current_choices(HO_P(0))) == 3);

    /* The save writes each choice on the row of its key and drops one naming
     * a key the map declares no build of, with a line. */
    g_mods_lines = 0;
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, "\"choices\":{\"player.hands\":\"bobroom1\"}"));
    CHECK(saved && strstr(saved, "\"choices\":{\"weapons/weapon/zion/player/sp/pistol\":\"alexpink\"}"));
    CHECK(saved && !strstr(saved, "carolgon"));
    CHECK(g_mods_lines >= 1);
    /* The save pins each ported demon's type on its mod's row: a stale pin gives way to the type the listing
     * reports, and a type of a mod the map has no row of pins nothing. */
    {
        char *pinned;
        size_t plen = 0;
        CHECK(sh_modmanifest_current_choose("spawnAIType_t/20/mods/bobroom1/single_x", "bobroom1") == 1);
        g_pin_salt[0] = "bobroom1"; g_pin_slot[0] = "spawnAIType_t/22/mods/bobroom1/single_x";
        g_pin_salt[1] = "carolgon"; g_pin_slot[1] = "spawnAIType_t/23/mods/carolgon/single_y";
        g_npins = 2;
        pinned = engine ? sh_modmanifest_on_save(engine, elen, &plen) : NULL;
        CHECK(pinned && strstr(pinned, "\"spawnAIType_t/22/mods/bobroom1/single_x\":\"bobroom1\"") &&
              !strstr(pinned, "spawnAIType_t/20/") && !strstr(pinned, "spawnAIType_t/23/"));
        if (pinned) HeapFree(GetProcessHeap(), 0, pinned);
        g_npins = 0;
        CHECK(sh_modmanifest_current_choose("spawnAIType_t/20/mods/bobroom1/single_x", NULL) == 1);
    }
    if (saved) {
        /* Reloaded: the same choices (minus the dropped one). */
        char *again;
        g_choice_calls = 0;
        sh_modmanifest_prepare_load(saved, slen);
        CHECK(g_choice_calls == 1 && g_choice_n == 2 && sh_modmanifest_current_choice("player.hands", key, sizeof key) &&
              !strcmp(key, "bobroom1"));

        HeapFree(GetProcessHeap(), 0, saved);
    }
    /* A map without rows clears them. */
    {
        sh_modmanifest_prepare_load(MAP, sizeof MAP - 1);
        CHECK(g_choice_n == 0 && HOZ(lines, sh_modmanifest_current_choices(HO_P(0))) == 0);

    }
    if (engine) HeapFree(GetProcessHeap(), 0, engine);
    HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_reset();
    g_installed_count = 2;
}

/* ---- the version rule ---- */

static int g_carry_answer;
static int vr_carries(const char *salt, const char *name) { (void)salt; (void)name; return g_carry_answer; }

static int g_notes;
static char g_note[1024];
static void vr_notice(const char *text)
{
    g_notes++;
    strncpy_s(g_note, sizeof g_note, text, _TRUNCATE);
}

#define VR_ENT(n) "{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/alexpink/" n "\"," \
                  "\"name\":\"\",\"state\":{}}}"
#define VR_TAIL ",\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}" \
                ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"alexpink\",\"name\":\"Eternal Demons\"," \
                "\"size\":42,\"objects\":{\"entities/imp\":\"" HASH_B "\"}}]}}"
static const char VR_MAP2[] = "{\"entities\":[" VR_ENT("imp") "," VR_ENT("extra") "]" VR_TAIL;   /* two placed objects */
static const char VR_MAP1[] = "{\"entities\":[" VR_ENT("imp") "]" VR_TAIL;                       /* one */

static installed g_vr[2];
static char g_vr_comp[SH_MOD_HASH_CHARS + 1];

/* The open map holds a pinned row of alexpink (its composition serves "imp"); the installed version is a root build of
 * the same salt that carries "extra" (and "imp" too when `root_has_imp`). */
static void vr_open(const char *map, size_t map_len, int root_has_imp, int carries)
{
    sh_modmanifest_list cur;
    sh_modmanifest_reset();
    g_installed = NULL;
    g_installed_count = 0;
    sh_modmanifest_prepare_load(map, map_len);
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && cur.rows[0].pinned);
    strcpy_s(g_vr_comp, sizeof g_vr_comp, cur.count ? cur.rows[0].build : HASH_B);
    sh_modmanifest_list_free(&cur);
    memset(g_vr, 0, sizeof g_vr);
    g_vr[0] = (installed){ "alexpink", "Eternal Demons", "1", g_vr_comp, 100, { "mods/alexpink/imp", NULL, NULL, NULL } };
    g_vr[1] = (installed){ "alexpink", "Eternal Demons", "1", HASH_A, 200,
                           { "mods/alexpink/extra", root_has_imp ? "mods/alexpink/imp" : NULL, NULL, NULL } };
    g_installed = g_vr;
    g_installed_count = 2;
    g_carry_answer = carries;
    g_notes = 0;
    g_note[0] = 0;
}

static int resolve_installed_root(const char *build, const char *salt, sh_modmanifest_row *out)
{
    (void)build;
    return fake_resolve(HASH_A, salt, out);
}

static void attached_pin_keeps_one_row(void)
{
    static const char *placed[] = { "mods/alexpink/imp" };
    sh_modmanifest_list cur = {0}, rows = {0};
    size_t length = 0;
    char *saved;
    vr_open(VR_MAP1, sizeof VR_MAP1 - 1, 0, 1);
    CHECK(sh_modmanifest_current(&cur) == 1);
    CHECK(sh_modmanifest_current_attach(&cur.rows[0]) == 1);
    sh_modmanifest_set_resolver(resolve_installed_root);
    CHECK(sh_modmanifest_rows_for(placed, 1, &rows) == 1);
    CHECK(rows.count && rows.rows[0].pinned && rows.rows[0].attached && !strcmp(rows.rows[0].build, g_vr_comp));
    saved = sh_modmanifest_on_save(VR_MAP1, sizeof VR_MAP1 - 1, &length);
    if (saved) {
        const char *first = strstr(saved, "\"mod\":\"alexpink\"");
        CHECK(first && !strstr(first + 1, "\"mod\":\"alexpink\""));
    } else CHECK(0);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_list_free(&cur);
    sh_modmanifest_set_resolver(NULL);
    sh_modmanifest_reset();
}

static void version_rule(void)
{
    static const char *NAMES2[] = { "mods/alexpink/imp", "mods/alexpink/extra" };
    sh_modmanifest_list back = {0}, cur, out;
    size_t sn = 0;
    char *saved;
    printf("-- the version rule: a pinned row and a local row of one salt merge when the installed version carries every placed name\n");
    sh_modmanifest_set_notice(vr_notice);
    sh_modmanifest_set_carries(vr_carries);

    /* The merge at a save: one local row naming every object, and S9. */
    vr_open(VR_MAP2, sizeof VR_MAP2 - 1, 0, 1);
    saved = sh_modmanifest_on_save(VR_MAP2, sizeof VR_MAP2 - 1, &sn);
    CHECK(saved != NULL);
    CHECK(g_notes == 1 && !strcmp(g_note, "This map now uses the version of Eternal Demons you have installed for all 2 "
                                          "of its objects."));
    if (saved) {
        CHECK(sh_modmanifest_read(saved, sn, &back, NULL) == 1 && !back.rows[0].pinned && back.rows[0].name_count == 2 &&
              !strcmp(back.rows[0].build, SH_MODMANIFEST_LOCAL_BUILD));
        sh_modmanifest_list_free(&back);
        HeapFree(GetProcessHeap(), 0, saved);
    }
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && !cur.rows[0].pinned);   /* the open map's rows follow the save */
    sh_modmanifest_list_free(&cur);

    /* One object: the singular wording. */
    vr_open(VR_MAP1, sizeof VR_MAP1 - 1, 1, 1);
    saved = sh_modmanifest_on_save(VR_MAP1, sizeof VR_MAP1 - 1, &sn);
    CHECK(saved != NULL);
    CHECK(g_notes == 1 && !strcmp(g_note, "This map now uses the version of Eternal Demons you have installed for its object."));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* D23a: the installed version lacks an object the pinned row places: nothing merges; the notice says which. */
    vr_open(VR_MAP2, sizeof VR_MAP2 - 1, 0, 0);
    saved = sh_modmanifest_on_save(VR_MAP2, sizeof VR_MAP2 - 1, &sn);
    CHECK(saved != NULL);
    CHECK(g_notes == 1 && !strcmp(g_note, "Eternal Demons was not moved to the version you have installed: the Imp is not "
                                          "in it. Objects placed from your version show in the editor only, until your "
                                          "version has the Imp again or you remove it from the map."));
    if (saved) {
        CHECK(sh_modmanifest_read(saved, sn, &back, NULL) == 2);
        CHECK(back.count == 2 && (back.rows[0].pinned != back.rows[1].pinned));   /* a pinned row and a local row */
        sh_modmanifest_list_free(&back);
        HeapFree(GetProcessHeap(), 0, saved);
    }

    /* The rows a save would write merge silently: no notice. */
    vr_open(VR_MAP2, sizeof VR_MAP2 - 1, 0, 1);
    memset(&out, 0, sizeof out);
    CHECK(sh_modmanifest_rows_for(NAMES2, 2, &out) == 1 && !out.rows[0].pinned && out.rows[0].name_count == 2);
    CHECK(g_notes == 0);
    sh_modmanifest_list_free(&out);
    vr_open(VR_MAP2, sizeof VR_MAP2 - 1, 0, 0);
    memset(&out, 0, sizeof out);
    CHECK(sh_modmanifest_rows_for(NAMES2, 2, &out) == 2 && g_notes == 0);   /* D23a: nothing merges, nothing said */
    sh_modmanifest_list_free(&out);

    /* The rule on the open map (before the Mods tab lists it): the current pinned row becomes local; S9 once. */
    vr_open(VR_MAP2, sizeof VR_MAP2 - 1, 0, 1);
    CHECK(sh_modmanifest_version_rule(NAMES2, 2) == 1);
    CHECK(g_notes == 1 && !strcmp(g_note, "This map now uses the version of Eternal Demons you have installed for all 2 "
                                          "of its objects."));
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && !cur.rows[0].pinned && !strcmp(cur.rows[0].build, HASH_A) &&
          cur.rows[0].name_count == 2);
    sh_modmanifest_list_free(&cur);
    /* D23a on the open map: the row stays pinned, with the notice. */
    vr_open(VR_MAP2, sizeof VR_MAP2 - 1, 0, 0);
    CHECK(sh_modmanifest_version_rule(NAMES2, 2) == 0 && g_notes == 1 &&
          !strncmp(g_note, "Eternal Demons was not moved to the version you have installed", 62));
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && cur.rows[0].pinned);
    sh_modmanifest_list_free(&cur);

    /* Unbound: the rule does nothing, not even a notice. */
    sh_modmanifest_set_carries(NULL);
    vr_open(VR_MAP2, sizeof VR_MAP2 - 1, 0, 1);
    saved = sh_modmanifest_on_save(VR_MAP2, sizeof VR_MAP2 - 1, &sn);
    CHECK(saved != NULL && g_notes == 0);
    if (saved) {
        CHECK(sh_modmanifest_read(saved, sn, &back, NULL) == 2);
        sh_modmanifest_list_free(&back);
        HeapFree(GetProcessHeap(), 0, saved);
    }
    sh_modmanifest_set_notice(NULL);
    sh_modmanifest_reset();
    g_installed = NULL;
    g_installed_count = 0;
}

/* A local row whose folder is absent is served by its stand-ins: a save keeps the type pins and size it was loaded
 * with. With the folder here, the listing decides the pins: a demon it no longer types gives its pin up. */
#define PIN_IMP "\"spawnAIType_t/19/mods/alexpink/single_imp\":\"alexpink\""

/* A pinned row composed with stand-ins (its unit not here) keeps its pin the same way; composed whole, the listing
 * decides; unbound (no cache), a pinned row counts as whole. */
static int g_whole_answer;
static int whole_stub(const char *build) { (void)build; return g_whole_answer; }
static void standin_pins_pinned(void)
{
    static const char PMAP[] = "{\"entities\":[" VR_ENT("imp") "],\"instances\":[],"
        "\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"alexpink\","
        "\"name\":\"Eternal Demons\",\"size\":42,\"objects\":{\"entities/imp\":\"" HASH_B "\"},"
        "\"choices\":{\"spawnAIType_t/19/mods/alexpink/single_imp\":\"alexpink\"}}]}}";
    static installed comp[1];
    sh_modmanifest_list cur;
    char *engine, *saved;
    size_t en = 0, slen = 0;
    printf("-- a pinned row composed with stand-ins keeps its pins at a save\n");
    sh_modmanifest_reset();
    g_installed = NULL; g_installed_count = 0;
    engine = engine_text(PMAP, sizeof PMAP - 1, &en);
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && cur.rows[0].pinned);
    strcpy_s(g_vr_comp, sizeof g_vr_comp, cur.count ? cur.rows[0].build : HASH_B);
    sh_modmanifest_list_free(&cur);
    comp[0] = (installed){ "alexpink", "Eternal Demons", "1", g_vr_comp, 100, { "mods/alexpink/imp", NULL, NULL, NULL }, "content" };
    g_installed = comp; g_installed_count = 1;
    g_npins = 0;
    sh_modmanifest_set_whole(whole_stub);
    g_whole_answer = 0;
    saved = engine ? sh_modmanifest_on_save(engine, en, &slen) : NULL;
    CHECK(saved && strstr(saved, PIN_IMP) && strstr(saved, "\"objects\""));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    g_whole_answer = 1;
    saved = engine ? sh_modmanifest_on_save(engine, en, &slen) : NULL;
    CHECK(saved && !strstr(saved, "spawnAIType_t/") && strstr(saved, "\"objects\""));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    sh_modmanifest_set_whole(NULL);
    saved = engine ? sh_modmanifest_on_save(engine, en, &slen) : NULL;
    CHECK(saved && !strstr(saved, "spawnAIType_t/"));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    if (engine) HeapFree(GetProcessHeap(), 0, engine);
    g_installed = NULL; g_installed_count = 0;
    sh_modmanifest_reset();
}

static void standin_pins(void)
{
    static const installed STANDIN[] = {
        { "alexpink", "Pink Imp", "", "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", 462,
          { "mods/alexpink/imp", "mods/alexpink/skin", NULL, NULL }, "content" },
    };
    sh_modmanifest_row row;
    sh_modmanifest_list rows;
    char err[192], *written, *engine, *saved;
    size_t wlen, elen = 0, slen = 0;
    printf("-- a local row whose folder is absent keeps its pins and size at a save\n");
    memset(&rows, 0, sizeof rows);
    make_row(&row, "alexpink", "", HASH_A, 185964430);
    sh_modmanifest_row_add_name(&row, "mods/alexpink/imp");
    sh_modmanifest_row_add_name(&row, "mods/alexpink/skin");
    row.choices = _strdup("spawnAIType_t/19/mods/alexpink/single_imp\talexpink\n");
    CHECK(sh_modmanifest_list_add(&rows, &row));
    sh_modmanifest_row_free(&row);
    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    sh_modmanifest_list_free(&rows);
    CHECK(written && strstr(written, PIN_IMP) && strstr(written, "\"local\":true"));
    if (!written) return;

    /* The folder absent: the closure names the stand-ins' build (462 bytes), the listing types nothing. */
    sh_modmanifest_reset();
    g_installed = STANDIN; g_installed_count = 1;
    g_npins = 0;
    engine = engine_text(written, wlen, &elen);
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, PIN_IMP) && strstr(saved, "\"size\":185964430") && strstr(saved, "\"local\":true"));
    if (saved && !strstr(saved, PIN_IMP)) printf("%s\n", saved);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    /* Saved again in that session: the same. */
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, PIN_IMP) && strstr(saved, "\"size\":185964430"));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* The folder here and the listing types the imp: its pin, the folder's size. */
    g_installed = INSTALLED; g_installed_count = 1;
    g_pin_salt[0] = "alexpink"; g_pin_slot[0] = "spawnAIType_t/19/mods/alexpink/single_imp";
    g_npins = 1;
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && strstr(saved, PIN_IMP) && strstr(saved, "\"size\":11782"));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    /* The folder is here and no longer ports the imp: the pin goes. */
    g_npins = 0;
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && !strstr(saved, "spawnAIType_t/") && strstr(saved, "\"size\":11782"));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    if (engine) HeapFree(GetProcessHeap(), 0, engine);
    HeapFree(GetProcessHeap(), 0, written);
    g_installed = NULL; g_installed_count = 0;
    sh_modmanifest_reset();
    standin_pins_pinned();
}

static int sound_open = 1, pin_calls;
int sh_modserve_scope_map_open(void) { return sound_open; }

static const char sound_a[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char sound_b[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
char *sh_cache_manifest(const char *hash, size_t *len)
{
    const char *json = !strcmp(hash, sound_a) ? "{\"files\":[{\"path\":\"sounds/a.bnk\"},{\"path\":\"sounds/common.bnk\"}]}" :
                                               "{\"files\":[{\"path\":\"sounds/b.bnk\"},{\"path\":\"sounds/common.bnk\"}]}";
    *len = strlen(json);
    return _strdup(json);
}
int sh_modpin_row(sh_modmanifest_row *r, const char *json, size_t len, const char *dir, sh_modpin_row_result *why)
{
    const char *hashes[] = {sound_a, sound_b};
    (void)json; (void)len; (void)dir; (void)why;
    pin_calls++;
    return sh_modmanifest_row_pin(r, (const char *const *)r->names, hashes, r->name_count) ? SH_MODPIN_PINNED : -1;
}

static void sound_scope(void)
{
    installed fake = {0};
    char *names[] = {"entities/a", "entities/b"};
    sh_modsound_bank banks[4] = {0};
    sh_modsound_table t = {0};
    sh_modmanifest_row row = {0};
    sh_modmanifest_list wanted = {0};
    int i;
    fake.salt = "1234abcd";
    fake.name = "Test sounds"; fake.version = ""; fake.hash = sound_a; fake.place = "";
    g_installed = &fake; g_installed_count = 1;
    for (i = 0; i < 4; i++) {
        strcpy_s(banks[i].build, sizeof banks[i].build, i == 3 ? sound_b : sound_a);
        strcpy_s(banks[i].bundle, sizeof banks[i].bundle, i == 3 ? "othermod" : fake.salt);
    }
    banks[0].author_path = "sounds/a.bnk"; banks[1].author_path = "sounds/common.bnk";
    banks[2].author_path = "sounds/b.bnk"; banks[3].author_path = "sounds/else.bnk";
    t.banks = banks; t.count = 4;
    strcpy_s(row.salt, sizeof row.salt, fake.salt);
    strcpy_s(row.build, sizeof row.build, fake.hash);
    row.names = names; row.name_count = 1;
    wanted.rows = &row; wanted.count = 1;
    sh_modsound_select_banks(&t);
    CHECK(!banks[0].wanted && !banks[1].wanted);
    CHECK(sh_modsound_note_rows(&wanted));
    sh_modsound_select_banks(&t);
    CHECK(banks[0].wanted && banks[1].wanted && !banks[2].wanted && !banks[3].wanted && pin_calls == 1);
    CHECK(!sh_modsound_note_rows(&wanted));
    sh_modsound_select_banks(&t);
    CHECK(pin_calls == 1);
    row.name_count = 2;
    CHECK(sh_modsound_note_rows(&wanted)); sh_modsound_select_banks(&t);
    CHECK(banks[0].wanted && banks[1].wanted && banks[2].wanted && !banks[3].wanted);
    row.attached = 1;
    CHECK(sh_modsound_note_rows(&wanted)); sh_modsound_select_banks(&t);
    CHECK(banks[0].wanted && banks[1].wanted && banks[2].wanted && !banks[3].wanted && pin_calls == 2);
    row.attached = 0; row.pinned = 1;
    CHECK(sh_modsound_note_rows(&wanted)); sh_modsound_select_banks(&t);
    CHECK(banks[0].wanted && banks[1].wanted && banks[2].wanted && !banks[3].wanted && pin_calls == 2);
    sound_open = 0; sh_modsound_select_banks(&t);
    CHECK(!banks[0].wanted && !banks[1].wanted && !banks[2].wanted && !banks[3].wanted);
    sh_modsound_note_rows(NULL);
    g_installed = NULL; g_installed_count = 0;
}

static void unavailable_wire(void)
{
    const char *raw = "{\"mod\":\"abcd1234\",\"name\":\"Rooms\",\"size\":9,\"local\":true,\"attached\":true,"
                      "\"names\":[\"mods/abcd1234/room\"],\"labels\":{\"mods/abcd1234/room\":\"South Hall\"},"
                      "\"modules\":[{\"name\":\"maps/modules/mods/abcd1234/room\",\"bounds\":[0,0,0,2,1,1]}]}";
    sh_modmanifest_row row = {0}, back = {0};
    sh_modmanifest_list list = {0};
    char *wire, *again, *label;
    CHECK(sh_modmanifest_parse_value(raw, strlen(raw), &row, NULL, 0));
    CHECK(sh_modmanifest_list_add(&list, &row));
    CHECK(sh_modmanifest_freeze_local_rows(&list, NULL, 0) == 1 && sh_modmanifest_freeze_local_rows(&list, NULL, 0) == 0);
    CHECK(row.attached && !row.pinned && row.modules && row.name_count == 1);
    wire = sh_modmanifest_format_value(&list.rows[0], NULL);
    CHECK(wire && !strstr(wire, "\"local\":") && strstr(wire, "\"objects\":{}") &&
          strstr(wire, "\"unavailable\":[\"mods/abcd1234/room\"]") && strstr(wire, "\"attached\":true"));
    CHECK(wire && sh_modmanifest_parse_value(wire, strlen(wire), &back, NULL, 0));
    CHECK(back.pinned && back.unavailable && back.unit_count == 0 && back.missing_count == 1 &&
          back.attached && back.modules && !strcmp(back.modules, row.modules));
    label = sh_modmanifest_row_label(&back, "mods/abcd1234/room");
    CHECK(label && !strcmp(label, "South Hall"));
    again = sh_modmanifest_format_value(&back, NULL);
    CHECK(again && wire && !strcmp(again, wire));
    free(again); free(label); free(wire);
    sh_modmanifest_row_free(&back);
    const char *bad[] = {
        "{\"mod\":\"abcd1234\",\"size\":0,\"objects\":{}}",
        "{\"mod\":\"abcd1234\",\"size\":0,\"objects\":{},\"unavailable\":false}",
        "{\"mod\":\"abcd1234\",\"size\":0,\"objects\":{},\"unavailable\":[7]}"
    };
    for (size_t i = 0; i < 3; i++) CHECK(!sh_modmanifest_parse_value(bad[i], strlen(bad[i]), &back, NULL, 0));
    sh_modmanifest_row_free(&row);
    sh_modmanifest_list_free(&list);
}

int main(void)
{
    sound_scope();
    unavailable_wire();
    printf("modmanifest_test\n");
    grammar();
    value_round_trip();
    text_round_trip_and_order();
    old_shape_rows();
    member_carries_everything();
    all_parts();
    placed_names();
    save_closure();
    product_entry();
    editor_substitution();
    activate_by_id();
    load_step();
    mod_json();
    map_level();
    choices();
    standin_pins();
    attached_pin_keeps_one_row();
    version_rule();
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
