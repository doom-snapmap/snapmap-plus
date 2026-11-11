/* map_old_test -- the store tool's reader and writer of the map shapes earlier
 * builds wrote (map_old.h): every retired variable family and the member's
 * "id" rows are read, and the written text is in the current shape (the
 * variables gone or kept as off, mods/<key>/ rewritten to mods/<salt>/, the
 * member carrying rows by salt, the settings and the navigation), which the
 * game's detector then passes. Packages, unknown keys and bad salts refuse. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/map_old.h"
#include "../src/backend/map_carrier.h"
#include "../src/backend/modmanifest.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *line) { (void)line; }

#define HASH_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define HASH_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"
/* A row as earlier builds wrote it in a smpmod1 variable (escaped JSON). */
#define ROW_VAR "{\\\"id\\\":\\\"alex.pink-imp\\\",\\\"version\\\":\\\"1.0.0\\\",\\\"bundle\\\":\\\"" HASH_A \
                "\\\",\\\"size\\\":11782,\\\"choices\\\":{\\\"player.hands\\\":\\\"alex.pink-imp\\\"}," \
                "\\\"names\\\":[\\\"mods/alex.pink-imp/imp\\\"]}"
#define ENTITIES "\"entities\":[{\"entityDef\":{\"inherit\":\"mods/alex.pink-imp/imp\"}}," \
                 "{\"entityDef\":{\"inherit\":\"snapmaps/visblockers/cap_01\"}}]"

static unsigned alloc4(const char *json)
{
    const char *p = strstr(json, "\"allocCount\":[");
    int i;
    if (!p) return 0xffffffffu;
    p += 14;
    for (i = 0; i < 4; i++) { p = strchr(p, ','); if (!p) return 0xffffffffu; p++; }
    return (unsigned)strtoul(p, NULL, 10);
}

static void salt_of(sh_map_old_salt *m, const char *key, const char *salt, const char *build, const char *name,
                    const char *version)
{
    memset(m, 0, sizeof *m);
    strcpy_s(m->key, sizeof m->key, key);
    strcpy_s(m->salt, sizeof m->salt, salt);
    strcpy_s(m->build, sizeof m->build, build);
    strcpy_s(m->name, sizeof m->name, name);
}

/* Every variable family at once: smpmod1 rows, smpprod1, both settings, one
 * smnav1 set; an author variable first, so every product row is removable. */
static void variables(void)
{
    char nav_hdr[200], digest[SH_SHARD_DIGEST_CHARS + 1], map[8192], err[200];
    sh_map_old old;
    sh_map_old_salt m;
    char *out;
    size_t n = 0;
    printf("-- every variable family: read, then written in the current shape\n");
    sh_shard_digest16((const unsigned char *)"ABC", 3, digest);
    _snprintf_s(nav_hdr, sizeof nav_hdr, _TRUNCATE, "smnav1.ind_dlc/room_4x.monster48.0.1.%s.fedcba9876543210", digest);
    _snprintf_s(map, sizeof map, _TRUNCATE,
                "{" ENTITIES ",\"variables\":{\"allocCount\":[0,0,0,0,6],\"string\":["
                VAR("door_label", "Open") "," VAR("smpmod1.0.1", ROW_VAR) "," VAR("smpprod1.v0.2.1-beta.24", "v0.2.1")
                "," VAR("smp.render.v1", "4;0") "," VAR("smp.bounds.v1", "1;32671") ","
                "{\"info\":{\"name\":\"%s\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"QUJD\",\"~type\":\"snapVarString_t\"}"
                "]}}", nav_hdr);
    CHECK(sh_map_old_shape(map, strlen(map)) == SH_MAP_OLD_VARIABLES);
    CHECK(sh_map_old_read(map, strlen(map), &old, err, sizeof err));
    CHECK(old.families == (SH_MAP_OLD_F_ROWS | SH_MAP_OLD_F_PRODUCT | SH_MAP_OLD_F_SETTINGS | SH_MAP_OLD_F_NAV));
    CHECK(old.row_count == 1 && !old.rows_rejected && !strcmp(old.rows[0].key, "alex.pink-imp") &&
          !strcmp(old.rows[0].bundle, HASH_A) && old.rows[0].size == 11782 && old.rows[0].name_count == 1 &&
          !strcmp(old.rows[0].choices, "player.hands\talex.pink-imp\n"));
    CHECK(!strcmp(old.render, "4;0") && !strcmp(old.bounds, "1;32671") && !old.settings_refused);
    CHECK(old.nav_sets == 1 && !old.nav_refused && old.nav && strstr(old.nav, "\"module\":\"ind_dlc/room_4x\"") &&
          strstr(old.nav, "\"data\":[\"QUJD\"]"));

    /* No salt recorded for the row's key: nothing is written. */
    salt_of(&m, "someone.else", "someone1", HASH_B, "", "");
    CHECK(sh_map_old_write(map, strlen(map), &old, &m, 1, &n, err, sizeof err) == NULL &&
          strstr(err, "no salt is recorded for 'alex.pink-imp'"));
    salt_of(&m, "alex.pink-imp", "Bad", HASH_B, "", "");
    CHECK(sh_map_old_write(map, strlen(map), &old, &m, 1, &n, err, sizeof err) == NULL);

    salt_of(&m, "alex.pink-imp", "alexpink", HASH_B, "Pink Imp", "1.2.0");
    out = sh_map_old_write(map, strlen(map), &old, &m, 1, &n, err, sizeof err);
    CHECK(out != NULL);
    if (out) {
        sh_modmanifest_list rows = {0};
        unsigned rejected = 9;
        size_t from = 0, to = 0;
        CHECK(n == strlen(out));
        CHECK(!strstr(out, "smpmod1.") && !strstr(out, "smpprod1.") && !strstr(out, "smp.render.v1") &&
              !strstr(out, "smp.bounds.v1") && !strstr(out, "smnav1."));
        CHECK(strstr(out, "door_label") && alloc4(out) == 1);
        CHECK(strstr(out, "\"inherit\":\"mods/alexpink/imp\"") && !strstr(out, "alex.pink-imp"));
        CHECK(sh_map_old_shape(out, n) == SH_MAP_OLD_NONE);
        CHECK(sh_modmanifest_read(out, n, &rows, &rejected) == 1 && !rejected);
        CHECK(rows.count == 1 && !strcmp(rows.rows[0].salt, "alexpink") && !strcmp(rows.rows[0].build, HASH_B) &&
              !strcmp(rows.rows[0].name, "Pink Imp") &&
              rows.rows[0].size == 11782 && rows.rows[0].name_count == 1 &&
              !strcmp(rows.rows[0].names[0], "mods/alexpink/imp") && !strcmp(rows.rows[0].choices, "player.hands\talexpink\n"));
        sh_modmanifest_list_free(&rows);
        CHECK(sh_carrier_part(out, n, "settings", &from, &to) == 1 &&
              to - from == strlen("{\"render\":\"4;0\",\"bounds\":\"1;32671\"}") &&
              !memcmp(out + from, "{\"render\":\"4;0\",\"bounds\":\"1;32671\"}", to - from));
        CHECK(sh_carrier_part(out, n, "nav", &from, &to) == 1 && to - from == old.nav_len &&
              !memcmp(out + from, old.nav, old.nav_len));
        /* Read again: already in this shape. */
        {
            sh_map_old again;
            CHECK(sh_map_old_read(out, n, &again, err, sizeof err) && again.families == 0 && again.row_count == 0);
            sh_map_old_free(&again);
        }
        HeapFree(GetProcessHeap(), 0, out);
    }
    sh_map_old_free(&old);
}

/* The member of the later earlier builds: rows naming "id" and "bundle" (an
 * attached row among them), its own settings and navigation, and a settings
 * row an author variable follows, which keeps its index and reads as off. */
static void member_rows(void)
{
    static const char map[] =
        "{" ENTITIES ",\"variables\":{\"allocCount\":[0,0,0,0,3],\"string\":["
        VAR("door_label", "Open") "," VAR("smp.render.v1", "4;0") "," VAR("String 2", "bound by index") "]},"
        "\"snapmap-plus\":{\"format\":1,\"mods\":["
        "{\"id\":\"alex.pink-imp\",\"version\":\"1.0.0\",\"bundle\":\"" HASH_A "\",\"size\":5,\"names\":[\"mods/alex.pink-imp/imp\"]},"
        "{\"id\":\"bob.room\",\"version\":\"2\",\"bundle\":\"" HASH_B "\",\"size\":6,\"attached\":true,\"names\":[]}],"
        "\"settings\":{\"bounds\":\"1;20000\"},"
        "\"nav\":[{\"module\":\"a/b\",\"class\":\"monster48\",\"digest\":\"0123456789abcdef\",\"geom\":\"fedcba9876543210\","
        "\"data\":[\"QUJD\"]}]}}";
    sh_map_old old;
    sh_map_old_salt m[2];
    char err[200], *out;
    size_t n = 0;
    printf("-- member rows by id: read, written by salt; a settings row an author variable follows stays as off\n");
    CHECK(sh_map_old_shape(map, sizeof map - 1) == SH_MAP_OLD_VARIABLES);
    CHECK(sh_map_old_read(map, sizeof map - 1, &old, err, sizeof err));
    CHECK(old.families == (SH_MAP_OLD_F_MEMBER_ROWS | SH_MAP_OLD_F_SETTINGS));
    CHECK(old.row_count == 2 && !strcmp(old.rows[1].key, "bob.room") && old.rows[1].attached);
    /* The member's settings win over the row. */
    CHECK(!old.render[0] && !strcmp(old.bounds, "1;20000"));
    CHECK(old.nav && old.nav_sets == 0 && strstr(old.nav, "\"module\":\"a/b\""));
    salt_of(&m[0], "alex.pink-imp", "alexpink", HASH_A, "", "");
    salt_of(&m[1], "bob.room", "bobroom1", HASH_B, "Room", "");
    out = sh_map_old_write(map, sizeof map - 1, &old, m, 2, &n, err, sizeof err);
    CHECK(out != NULL);
    if (out) {
        sh_modmanifest_list rows = {0};
        CHECK(sh_map_old_shape(out, n) == SH_MAP_OLD_VARIABLES);   /* the kept row still names its family */
        CHECK(strstr(out, VAR("smp.render.v1", "4;8192")) && alloc4(out) == 3 && strstr(out, "bound by index"));
        CHECK(sh_modmanifest_read(out, n, &rows, NULL) == 2);
        CHECK(rows.count == 2 && !strcmp(rows.rows[0].name, "alex.pink-imp") &&
              !strcmp(rows.rows[1].salt, "bobroom1") && !strcmp(rows.rows[1].name, "Room") && rows.rows[1].attached);
        CHECK(strstr(out, "\"settings\":{\"bounds\":\"1;20000\"}") && strstr(out, "\"module\":\"a/b\""));
        CHECK(!strstr(out, "\"id\":") && !strstr(out, "\"bundle\":"));
        sh_modmanifest_list_free(&rows);
        HeapFree(GetProcessHeap(), 0, out);
    }
    sh_map_old_free(&old);
}

/* What the writer leaves alone or refuses: a package (converted first), a
 * smnav1 set that does not reassemble (dropped, counted), duplicated settings
 * rows (off), and a map already in this shape. */
static void edges(void)
{
    static const char package[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
        VAR("smpkg.acceptance.cyberdemon.0.1.0123456789abcdef", "QUJD") "]}}";
    static const char unreleased[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,2],\"string\":["
        VAR("door_label", "Open") "," VAR("smpbind.x.0123456789abcdef", "QUJD") "]}}";
    static const char bad_nav[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,3],\"string\":["
        VAR("door_label", "Open") "," VAR("smnav1.a/b.monster48.0.1.0123456789abcdef.fedcba9876543210", "QUJD") ","
        VAR("smp.render.v1", "4;0") "]}}";
    static const char twice[] =
        "{\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,3],\"string\":["
        VAR("door_label", "Open") "," VAR("smp.render.v1", "4;0") "," VAR("smp.render.v1", "4;0") "]}}";
    static const char current[] =
        "{\"entities\":[],\"snapmap-plus\":{\"format\":1,\"mods\":[{\"mod\":\"alexpink@" HASH_A "\",\"size\":1}]}}";
    sh_map_old old;
    char err[200], *out;
    size_t n = 0;
    printf("-- packages refuse; a broken navigation set is dropped; duplicated settings are off\n");
    CHECK(sh_map_old_read(package, sizeof package - 1, &old, err, sizeof err) && (old.families & SH_MAP_OLD_F_PACKAGE));
    CHECK(sh_map_old_write(package, sizeof package - 1, &old, NULL, 0, &n, err, sizeof err) == NULL &&
          strstr(err, "package"));
    sh_map_old_free(&old);

    CHECK(sh_map_old_read(unreleased, sizeof unreleased - 1, &old, err, sizeof err) &&
          old.families == SH_MAP_OLD_F_UNRELEASED);
    out = sh_map_old_write(unreleased, sizeof unreleased - 1, &old, NULL, 0, &n, err, sizeof err);
    CHECK(out && !strstr(out, "smpbind.") && alloc4(out) == 1 && !strstr(out, "snapmap-plus"));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    sh_map_old_free(&old);

    CHECK(sh_map_old_read(bad_nav, sizeof bad_nav - 1, &old, err, sizeof err));
    CHECK((old.families & SH_MAP_OLD_F_NAV) && old.nav_refused == 1 && old.nav == NULL && !strcmp(old.render, "4;0"));
    out = sh_map_old_write(bad_nav, sizeof bad_nav - 1, &old, NULL, 0, &n, err, sizeof err);
    CHECK(out && !strstr(out, "smnav1.") && !strstr(out, "\"nav\"") && alloc4(out) == 1 &&
          strstr(out, "\"settings\":{\"render\":\"4;0\"}") && sh_map_old_shape(out, n) == SH_MAP_OLD_NONE);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    sh_map_old_free(&old);

    CHECK(sh_map_old_read(twice, sizeof twice - 1, &old, err, sizeof err));
    CHECK(!old.render[0] && old.settings_refused == 2);
    out = sh_map_old_write(twice, sizeof twice - 1, &old, NULL, 0, &n, err, sizeof err);
    CHECK(out && alloc4(out) == 1 && !strstr(out, "snapmap-plus"));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    sh_map_old_free(&old);

    CHECK(sh_map_old_read(current, sizeof current - 1, &old, err, sizeof err) && old.families == 0 && !old.row_count);
    out = sh_map_old_write(current, sizeof current - 1, &old, NULL, 0, &n, err, sizeof err);
    CHECK(out && !strcmp(out, current));   /* the current rows are the writer's to keep: none were old */
    if (out) HeapFree(GetProcessHeap(), 0, out);
    sh_map_old_free(&old);
    CHECK(!sh_map_old_read("not json", 8, &old, err, sizeof err));
}

int main(void)
{
    printf("map_old_test\n");
    variables();
    member_rows();
    edges();
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
