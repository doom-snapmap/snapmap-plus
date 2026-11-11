/* modmanifest_test -- the map manifest: the salt and row grammar, the row
 * round trip, the rows in the map's "snapmap-plus" member (map_carrier.h),
 * rows in an earlier shape refused, placed names, the save-time closure
 * through the serving layer, the load step, and map-level rows (attached
 * mods) with the map scope's hook and the row resolver.
 *
 * The serving layer (modserve_activate_bundles, sh_modserve_row_build and
 * modserve_bundles_for_names_visit, declared in modserve.h) is replaced by
 * doubles defined here. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modmanifest.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modsalt.h"
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

typedef struct installed { const char *salt, *name, *version, *hash; unsigned long long size; const char *names[4]; } installed;
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
        static const char expect[] = "{\"mod\":\"alexpink@" HASH_A "\",\"name\":\"Pink Imp\","
                                     "\"size\":11782,\"names\":[\"mods/alexpink/imp\",";
        CHECK(value && !strncmp(value, expect, sizeof expect - 1));
    }
    CHECK(value && sh_modmanifest_parse_value(value, n, &back, err, sizeof err));
    CHECK(!strcmp(back.salt, row.salt) && !strcmp(back.build, row.build) &&
          !strcmp(sh_modmanifest_row_name(&back), "Pink Imp"));
    CHECK(back.size == 11782 && back.name_count == 2 && !strcmp(back.names[1], "a \"quoted\" \\ name"));
    sh_modmanifest_row_free(&back);
    free(value);

    /* Refusals: wrong hash, bad salt, no "@", missing size, names not a list;
     * an unknown member is kept out, a missing name is empty. */
    {
        static const char short_hash[] = "{\"mod\":\"alexpink@abc\",\"size\":1}";
        static const char bad_salt[] = "{\"mod\":\"Alexpink@" HASH_A "\",\"size\":1}";
        static const char no_at[] = "{\"mod\":\"alexpink:" HASH_A "\",\"size\":1}";
        static const char no_size[] = "{\"mod\":\"alexpink@" HASH_A "\"}";
        static const char bad_names[] = "{\"mod\":\"alexpink@" HASH_A "\",\"size\":1,\"names\":[1,}";
        static const char extra[] = "{\"mod\":\"alexpink@" HASH_A "\",\"size\":1,\"future\":true}";
        CHECK(!sh_modmanifest_parse_value(short_hash, sizeof short_hash - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(bad_salt, sizeof bad_salt - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(no_at, sizeof no_at - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(no_size, sizeof no_size - 1, &back, err, sizeof err));
        CHECK(!sh_modmanifest_parse_value(bad_names, sizeof bad_names - 1, &back, err, sizeof err));
        CHECK(sh_modmanifest_parse_value(extra, sizeof extra - 1, &back, err, sizeof err) && !sh_modmanifest_row_name(&back)[0] &&
              !strcmp(back.salt, "alexpink") && !strcmp(back.build, HASH_A));
        sh_modmanifest_row_free(&back);
    }
    /* A row that names an "id" or a "bundle" is in an earlier shape. */
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
                  ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"mod\":\"alexpink@" HASH_A "\",\"name\":\"\","
                  "\"size\":11782,\"names\":[\"mods/alexpink/imp\"]},{\"mod\":\"bobroom1@" HASH_B
                  "\",\"name\":\"\",\"size\":4096,\"names\":[]}]}}"));

    CHECK(sh_modmanifest_read(written, wlen, &back, &rejected) == 2 && rejected == 0 && back.count == 2);
    CHECK(back.count == 2 && !strcmp(back.rows[0].salt, "alexpink") && !strcmp(back.rows[1].build, HASH_B) &&
          back.rows[0].size == 11782 && back.rows[0].name_count == 1);
    sh_modmanifest_list_free(&back);

    /* The member strip gives the map back byte for byte. */
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
    HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_list_free(&rows);
}

/* The save chain on one text: settings, then navigation, then the manifest,
 * each putting its part; the member carries all three in its order and the
 * load reads the rows back while the other parts are left to their readers. */
/* A member whose rows name an "id" and a "bundle" is an earlier shape (the
 * parse detour refuses such a map first, sh_map_old_shape): its rows do not
 * read, and the refusal note is held as the load's refusal. */
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
    CHECK(sh_modmanifest_read(OLD, sizeof OLD - 1, &rows, &rejected) == 0 && rows.count == 0 && rejected == 1);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_reset();
    sh_modmanifest_load_refuse(SH_MAP_OLD_REFUSAL);
    CHECK(sh_modmanifest_load_notes_pending() == 1);
    CHECK(HO(refusal, sh_modmanifest_load_refusal(HO_P(0))) &&
          !strcmp(refusal, "This map was saved in a format this version does not read."));
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
        CHECK(strstr(saved, ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"mod\":\"alexpink@" HASH_A "\"") != NULL);
        CHECK(sh_carrier_part(saved, sn, "settings", &from, &to) == 1 && to - from == sizeof SETTINGS - 1 &&
              !memcmp(saved + from, SETTINGS, to - from));
        CHECK(sh_carrier_part(saved, sn, "nav", &from, &to) == 1 && to - from == sizeof NAV - 1);
        CHECK(sh_modmanifest_read(saved, sn, &back, NULL) == 1 && !strcmp(back.rows[0].build, HASH_A));
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
    ho_set(why, "This map needs nobodyhr, which is not installed and was never published.");
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
    CHECK(strstr(g_last_log, "MODS: map manifest names 2 build(s)") != NULL);
    {
        sh_modmanifest_list cur;
        CHECK(sh_modmanifest_current(&cur) == 2 && !strcmp(cur.rows[0].build, gone));
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
        CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].build, HASH_A) && cur.rows[0].name_count == 2);
        sh_modmanifest_list_free(&cur);
    }
    if (out) {
        CHECK(sh_modmanifest_read(out, n, &back, NULL) == 1 && back.count == 1);
        CHECK(back.count == 1 && !strcmp(back.rows[0].build, HASH_A) && back.rows[0].name_count == 2 &&
              back.rows[0].size == 11782 && !strcmp(back.rows[0].salt, "alexpink"));
        CHECK(string_rows(out) == 2 && strstr(out, ",\"snapmap-plus\":{\"format\":1,\"mods\":[{\"mod\":\"alexpink@"));
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
        static const char room[] = "{\"instances\":[{\"moduleName\":\"maps/modules/mods/bobroom1/hall.decl\"}],"
                                   "\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
        unsigned count = 9;
        g_installed_count = 2;
        out = sh_modmanifest_on_save(room, sizeof room - 1, &n);
        CHECK(out && strstr(out, "\"snapmap-plus\":{\"format\":1,\"mods\":[{\"mod\":\"bobroom1@") &&
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
    make_row(&row, "alexpink", "1.0.0", HASH_A, 5);
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    make_row(&row, "bobroom1", "2", HASH_B, 6);
    sh_modmanifest_list_add(&rows, &row);
    sh_modmanifest_row_free(&row);
    written = sh_modmanifest_write(MAP, sizeof MAP - 1, &rows, &wlen, err, sizeof err);
    CHECK(written != NULL);
    g_activate_calls = 0;
    engine = written ? engine_text(sh_modmanifest_prepare_load(written, wlen, &elen), written, wlen, &elen) : NULL;
    CHECK(engine && !strcmp(engine, MAP) && g_activate_calls == 1 && g_activate_count == 2);
    CHECK(!strcmp(g_activated[0], HASH_A) && !strcmp(g_activated[1], HASH_B));
    CHECK(sh_modmanifest_current(&current) == 2 && !strcmp(current.rows[1].salt, "bobroom1"));
    sh_modmanifest_list_free(&current);
    if (engine) HeapFree(GetProcessHeap(), 0, engine);

    /* A map without rows: the engine's bytes, activation with nothing, the manifest forgotten. */
    g_activate_calls = 0;
    CHECK(sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &elen) == NULL);
    CHECK(g_activate_calls == 1 && g_activate_count == 0);
    CHECK(sh_modmanifest_current(&current) == 0);

    /* One unreadable row is dropped, the readable one still activates. */
    {
        static const char mixed[] = "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
            "\"snapmap-plus\":{\"format\":1,\"mods\":[{\"mod\":\"Bad\"},"
            "{\"mod\":\"bobroom1@" HASH_B "\",\"size\":6,\"names\":[]}]}}";
        g_activate_calls = 0;
        engine = sh_modmanifest_prepare_load(mixed, sizeof mixed - 1, &elen);
        CHECK(engine == NULL);
        CHECK(g_activate_count == 1 && !strcmp(g_activated[0], HASH_B));
        CHECK(strstr(g_last_log, "1 manifest row(s) did not parse") != NULL);
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
    CHECK(value && !strcmp(value, "{\"mod\":\"alexhand@" HASH_C "\",\"name\":\"\","
                                  "\"size\":512,\"names\":[],\"attached\":true}"));
    CHECK(value && sh_modmanifest_parse_value(value, vlen, &back, err, sizeof err) && back.attached == 1);
    sh_modmanifest_row_free(&back);
    free(value);
    CHECK(sh_modmanifest_parse_value("{\"mod\":\"aaaabbbb@" HASH_C "\",\"size\":1,\"attached\":false}",
                                     strlen("{\"mod\":\"aaaabbbb@" HASH_C "\",\"size\":1,\"attached\":false}"),
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
    engine = engine_text(sh_modmanifest_prepare_load(written, wlen, &elen), written, wlen, &elen);
    CHECK(engine != NULL && !strcmp(engine, MAP));
    /* The load hook sees both rows and the placed names, before activation. */
    CHECK(g_scope_calls == 1 && g_scope_when == SH_MODSERVE_AT_LOAD && g_scope_rows == 2 && g_scope_attached == 1 &&
          g_scope_names > 0);
    CHECK(g_activate_count == 2);
    CHECK(sh_modmanifest_current(&current) == 2 && sh_modmanifest_current_attached(&rows) == 1 &&
          !strcmp(rows.rows[0].build, HASH_C));
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
    CHECK(saved && strstr(saved, "alexpink") && strstr(saved, HASH_D) && !strstr(saved, HASH_C) &&
          strstr(saved, "\"name\":\"Bare Hands\"") && strstr(saved, "\"name\":\"Pink Imp\""));
    CHECK(saved && strstr(saved, "\"attached\":true"));
    CHECK(g_scope_when == SH_MODSERVE_AT_SAVE && g_scope_rows == 2 && g_scope_attached == 1);
    CHECK(sh_modmanifest_current_attached(&rows) == 1 && !strcmp(rows.rows[0].build, HASH_D));
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
    CHECK(saved && strstr(saved, HASH_B) && strstr(saved, HASH_E) && strstr(saved, "eringone"));
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* Remove from map drops the row; the next save writes none for it. */
    CHECK(sh_modmanifest_current_remove(HASH_D) == 1);
    CHECK(sh_modmanifest_current_remove(HASH_E) == 1);
    CHECK(sh_modmanifest_current_remove(HASH_B) == 1);
    saved = engine ? sh_modmanifest_on_save(engine, elen, &slen) : NULL;
    CHECK(saved && !strstr(saved, HASH_D) && !strstr(saved, HASH_B) && strstr(saved, "alexpink"));
    CHECK(g_scope_attached == 0 && g_scope_rows == 1);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* A map-level row alone (no placed mod, nothing served for it) is still written. */
    g_installed_count = 0;
    make_row(&back, "eringone", "3", HASH_E, 9);
    CHECK(sh_modmanifest_current_attach(&back) == 1);
    sh_modmanifest_row_free(&back);
    saved = sh_modmanifest_on_save(MAP, sizeof MAP - 1, &slen);
    CHECK(saved && strstr(saved, "\"snapmap-plus\":{\"format\":1,\"mods\":[{\"mod\":\"eringone@" HASH_E "\""));
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
        static const char odd[] = "{\"mod\":\"aaaabbbb@" HASH_A "\",\"size\":1,"
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
    engine = engine_text(sh_modmanifest_prepare_load(written, wlen, &elen), written, wlen, &elen);
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
    if (saved) {
        /* Reloaded: the same choices (minus the dropped one). */
        char *again;
        g_choice_calls = 0;
        again = sh_modmanifest_prepare_load(saved, slen, &elen);
        CHECK(g_choice_calls == 1 && g_choice_n == 2 && sh_modmanifest_current_choice("player.hands", key, sizeof key) &&
              !strcmp(key, "bobroom1"));
        if (again) HeapFree(GetProcessHeap(), 0, again);
        HeapFree(GetProcessHeap(), 0, saved);
    }
    /* A map without rows clears them. */
    {
        char *none = sh_modmanifest_prepare_load(MAP, sizeof MAP - 1, &elen);
        CHECK(g_choice_n == 0 && HOZ(lines, sh_modmanifest_current_choices(HO_P(0))) == 0);
        if (none) HeapFree(GetProcessHeap(), 0, none);
    }
    if (engine) HeapFree(GetProcessHeap(), 0, engine);
    HeapFree(GetProcessHeap(), 0, written);
    sh_modmanifest_reset();
    g_installed_count = 2;
}

int main(void)
{
    printf("modmanifest_test\n");
    grammar();
    value_round_trip();
    text_round_trip_and_order();
    old_shape_rows();
    member_carries_everything();
    all_parts();
    placed_names();
    save_closure();
    editor_substitution();
    activate_by_id();
    load_step();
    mod_json();
    map_level();
    choices();
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
