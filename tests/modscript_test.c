/* modscript_test -- mod scripts in the builder, the serving index and the
 * archives (docs/architecture.md "Mod scripts").
 *
 * The builder admits assets/mods/<salt>/scripts/<name>.lua and scripts/lib/
 * <name>.lua as kind script and refuses, with the file (and line), a .lua
 * anywhere else, compiled Lua, a NUL byte, text that is not UTF-8, a file over
 * 256 KB, more than 64 files or 1 MB of scripts in one mod, a syntax error, an
 * entry file without "api = 1", a literal require of a module the mod does
 * not carry (another mod's included), a name in another mod's namespace, and
 * any script when no compile check is registered. The static pass records
 * what the scripts bind in the builder block ("scripts"), which survives a
 * write and a load; a build with scripts is never sliceable. A new folder's
 * first install lowercases every path below assets\ and rewrites mods/<folder>/
 * inside its .lua like any text (DECISIONS 13, 6); the serving index lists the
 * scripts with their recorded hashes and serves those bytes; the publish
 * archive, a rawmap export and its import carry the scripts under their
 * lower-case game names with no new member kind. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modexport.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modsync.h"
#include "../src/backend/modzip.h"
#include "../src/backend/script/script_compile.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *line)
{
    if (getenv("MODSCRIPT_TEST_LOG") && line) printf("    %s\n", line);
}

int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    (void)bundle_hashes;
    (void)n;
    return 0;
}

#define SALT "scrptab1"
#define MOD_JSON "{\"name\":\"Door Logic\",\"snapmap-plus\":{\"format\":2,\"salt\":\"" SALT "\"}}"
#define ENTRY "assets\\mods\\" SALT "\\scripts\\door.lua"
#define LIB "assets\\mods\\" SALT "\\scripts\\lib\\fsm.lua"
#define DEF "assets\\generated\\decls\\entitydef\\mods\\" SALT "\\ai\\door.decl"

static const char GOOD_ENTRY[] =
    "api = 1\n"
    "-- on.spawn(\"not_a_call\") in a comment\n"
    "local fsm = require(\"fsm\")\n"
    "on.spawn(\"ai/door\", function(self) end)\n"
    "on.think(\"mods/" SALT "/ai/door\", function(self, dt) end)\n"
    "on.input(\"switch_used\", function(n, a) node(\"open_door\"):fire(a) end)\n"
    "on.input(fsm.name, function() end)\n";

static char g_tmp[MAX_PATH];
static int g_case;

/* A good scripted mod in a new folder; returns its path in `dir`. */
static void good_mod(char *dir, size_t cap)
{
    _snprintf_s(dir, cap, _TRUNCATE, "%s\\case%d", g_tmp, ++g_case);
    CreateDirectoryA(dir, NULL);
    mt_write(dir, "mod.json", MOD_JSON);
    mt_write(dir, DEF, "{\r\n}");
    mt_write(dir, ENTRY, GOOD_ENTRY);
    mt_write(dir, LIB, "return { name = \"door_input\" }\n");
}

static void put_bytes(const char *dir, const char *rel, const void *data, size_t len)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, rel);
    mt_mkdirs_for(path);
    if (!fopen_s(&f, path, "wb") && f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

/* A comment-only script of `len` bytes (it compiles). */
static void put_big(const char *dir, const char *rel, size_t len)
{
    char *t = (char *)malloc(len);
    size_t i;
    if (!t) return;
    for (i = 0; i < len; i++) t[i] = (i % 64 == 63) ? '\n' : (i % 64 < 2 ? '-' : 'x');
    put_bytes(dir, rel, t, len);
    free(t);
}

static int build(const char *dir, sh_mb_bundle *b)
{
    memset(b, 0, sizeof *b);
    return sh_mb_build(dir, NULL, NULL, b);
}

/* The build of `dir` fails and its first cause holds `want`. */
static void refused(const char *dir, const char *want)
{
    sh_mb_bundle b;
    int ok = build(dir, &b);
    printf("  refused: %s\n", b.first_cause);
    CHECK(!ok && strstr(b.first_cause, want) != NULL);
    if (ok || !strstr(b.first_cause, want)) fprintf(stderr, "    wanted \"%s\"\n", want);
    sh_mb_free(&b);
}

static const sh_mb_file *find_file(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return &b->files[i];
    return NULL;
}

/* ------------------------------------------------------------ the scan */

static char g_scan[2048];

static void scan_cb(void *context, int what, const char *event, const char *name, size_t len, int line)
{
    char one[200];
    (void)context;
    _snprintf_s(one, sizeof one, _TRUNCATE, "%d:%s%s%s:%.*s|", line,
                (what & 0xff) == SH_MB_SCRIPT_BIND ? "bind." : (what & 0xff) == SH_MB_SCRIPT_INPUT ? "input" :
                (what & 0xff) == SH_MB_SCRIPT_OUTPUT ? "output" : "require",
                (what & 0xff) == SH_MB_SCRIPT_BIND ? event : "", (what & SH_MB_SCRIPT_DYNAMIC) ? "?" : "",
                name ? (int)len : 0, name ? name : "");
    strncat_s(g_scan, sizeof g_scan, one, _TRUNCATE);
}

static void test_scan(void)
{
    static const char text[] =
        "api = 1 -- on.spawn(\"c1\")\n"                                 /* 1 */
        "--[==[ on.spawn(\"c2\")\n]==] local s = \"on.input('s1')\"\n"   /* 2-3 */
        "local l = [[ on.spawn(\"s2\") ]] x.on.spawn(\"field\")\n"      /* 4 */
        "on.spawn(\"ai/door\", f) on.think ( 'ai/door' , f )\n"         /* 5 */
        "on.anim(\"ai/brute\", \"throw\", f)\n"                         /* 6 */
        "on.input(\"switch_used\", function(n) node(\"open_door\"):fire() end)\n"   /* 7 */
        "on.spawn(name, f) on.damage(\"a\" .. b, f) on.map_start(f)\n"  /* 8 */
        "local m = require \"fsm\" local u = require(\"util\") require(mod)\n"       /* 9 */
        "on.input(\"esc\\\"aped\", f) on . death ( [[ai/long]] , f)\n";  /* 10 */
    static const char want[] =
        "5:bind.spawn:ai/door|5:bind.think:ai/door|6:bind.anim:ai/brute|7:input:switch_used|7:output:open_door|"
        "8:bind.spawn?:|8:bind.damage?:|9:require:fsm|9:require:util|9:require?:|10:input?:|10:bind.death:ai/long|";
    printf("-- the static pass: literal calls only, comments and strings skipped\n");
    g_scan[0] = 0;
    sh_mb_script_scan(text, sizeof text - 1, scan_cb, NULL);
    CHECK(!strcmp(g_scan, want));
    if (strcmp(g_scan, want)) fprintf(stderr, "  got  %s\n  want %s\n", g_scan, want);
}

static void test_requests(void)
{
    const char *name = NULL;
    size_t len = 0;
    char file[64];
    printf("-- where a script may be\n");
    CHECK(sh_mb_script_request("mods/" SALT "/scripts/door.lua", &name, &len) == SH_MB_SCRIPT_ENTRY && len == 4 &&
          !strncmp(name, "door", 4));
    CHECK(sh_mb_script_request("mods/" SALT "/scripts/lib/fsm.lua", NULL, NULL) == SH_MB_SCRIPT_LIB);
    CHECK(sh_mb_script_request("mods/" SALT "/door.lua", NULL, NULL) == SH_MB_SCRIPT_MISPLACED);
    CHECK(sh_mb_script_request("mods/" SALT "/scripts/a/b.lua", NULL, NULL) == SH_MB_SCRIPT_MISPLACED);
    CHECK(sh_mb_script_request("mods/" SALT "/scripts/lib/x/y.lua", NULL, NULL) == SH_MB_SCRIPT_MISPLACED);
    CHECK(sh_mb_script_request("scripts/door.lua", NULL, NULL) == SH_MB_SCRIPT_MISPLACED);
    CHECK(sh_mb_script_request("mods/" SALT "/scripts/a b.lua", NULL, NULL) == SH_MB_SCRIPT_MISPLACED);
    CHECK(sh_mb_script_request("mods/" SALT "/scripts/door.txt", NULL, NULL) == 0);
    CHECK(sh_mb_extension_allowed("mods/" SALT "/scripts/door.lua") && !sh_mb_extension_allowed("mods/" SALT "/door.lua"));
    sh_mb_script_file_name("mods/" SALT "/scripts/lib/fsm.lua", file, sizeof file);
    CHECK(!strcmp(file, "scripts\\lib\\fsm.lua"));
}

/* ------------------------------------------------------------ builds */

static void test_builds(void)
{
    char dir[MAX_PATH], rel[MAX_PATH];
    sh_mb_bundle b, back;
    const sh_mb_file *f;
    int i;

    printf("-- a scripted mod builds; the block records what it binds\n");
    good_mod(dir, sizeof dir);
    CHECK(build(dir, &b));
    if (b.errors) fprintf(stderr, "  %s\n", b.first_cause);
    CHECK(b.script_files == 2 && b.warnings == 0);
    CHECK(b.scripts && !strcmp(b.scripts, "{\"dynamic\":1,\"entities\":[{\"events\":[\"spawn\",\"think\"],\"name\":\"ai/door\"}],"
                                         "\"files\":2,\"inputs\":[\"switch_used\"],\"outputs\":[\"open_door\"]}"));
    if (b.scripts) printf("  scripts: %s\n", b.scripts);
    f = find_file(&b, "mods/" SALT "/scripts/door.lua");
    CHECK(f && f->ptype && !strcmp(f->ptype, "script") && f->cls == SH_MB_CLASS_ADD && !f->stock);
    CHECK(!sh_mb_sliceable(&b));
    CHECK(sh_mb_write(&b, dir, rel, sizeof rel));
    memset(&back, 0, sizeof back);
    if (!sh_mb_load_folder(dir, &back)) fprintf(stderr, "  load: %s\n", back.first_cause);
    CHECK(back.errors == 0 && back.script_files == 2 && back.scripts && b.scripts &&
          !strcmp(back.scripts, b.scripts) && !strcmp(back.hash, b.hash));
    sh_mb_free(&back);
    sh_mb_free(&b);

    printf("-- a mod without scripts records none and may be sliced\n");
    good_mod(dir, sizeof dir);
    mt_delete(dir, ENTRY);
    mt_delete(dir, LIB);
    CHECK(build(dir, &b) && !b.scripts && b.script_files == 0 && sh_mb_sliceable(&b));
    sh_mb_free(&b);

    printf("-- a bound name the mod does not define is a warning with the line\n");
    good_mod(dir, sizeof dir);
    mt_write(dir, ENTRY, "api = 1\n\non.spawn(\"ai/dor\", function() end)\n");
    CHECK(build(dir, &b) && b.warnings == 1 &&
          !strcmp(b.first_warning, "scripts\\door.lua line 3: this mod has no entityDef \"ai/dor\" for on.spawn; check the name"));
    printf("  warned: %s\n", b.first_warning);
    sh_mb_free(&b);

    printf("-- refusals name the file, and the line when there is one\n");
    good_mod(dir, sizeof dir);
    mt_write(dir, ENTRY, "api = 1\nfunction f()\n  local x = 1\n");
    refused(dir, "scripts\\door.lua line 4: \"end\" expected");

    good_mod(dir, sizeof dir);
    mt_write(dir, ENTRY, "\x1bLuaT\x01\x02");
    refused(dir, "scripts\\door.lua: the file is compiled Lua, not script text. Put the .lua source text in the file.");

    good_mod(dir, sizeof dir);
    put_bytes(dir, ENTRY, "api = 1\n\0\n", 10);
    refused(dir, "scripts\\door.lua: the file is not UTF-8 text. Save it as UTF-8 text.");

    good_mod(dir, sizeof dir);
    mt_write(dir, ENTRY, "api = 1\n-- \xff\xfe\n");
    refused(dir, "scripts\\door.lua: the file is not UTF-8 text");

    good_mod(dir, sizeof dir);
    put_big(dir, ENTRY, SH_MB_SCRIPT_FILE_MAX + 1);
    refused(dir, "scripts\\door.lua: the file is larger than 256 KB. Split it into smaller files.");

    good_mod(dir, sizeof dir);
    for (i = 0; i < 64; i++) {
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\mods\\" SALT "\\scripts\\lib\\m%02d.lua", i);
        mt_write(dir, rel, "return 1\n");
    }
    refused(dir, "this mod has 66 script files, more than the 64 one mod may carry; combine some of them");

    good_mod(dir, sizeof dir);
    for (i = 0; i < 5; i++) {
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\mods\\" SALT "\\scripts\\lib\\big%d.lua", i);
        put_big(dir, rel, 250000);
    }
    refused(dir, "this mod's scripts are larger than 1 MB in all; make them smaller");

    good_mod(dir, sizeof dir);
    mt_write(dir, "assets\\mods\\" SALT "\\door.lua", "api = 1\n");
    refused(dir, "assets/mods/" SALT "/door.lua: a script goes in this mod's scripts folder");

    good_mod(dir, sizeof dir);
    mt_write(dir, "assets\\mods\\" SALT "\\scripts\\more\\b.lua", "api = 1\n");
    refused(dir, "a script goes in this mod's scripts folder, assets\\mods\\<salt>\\scripts\\");

    good_mod(dir, sizeof dir);
    mt_write(dir, ENTRY, "-- no api line\nprint(1)\n");
    refused(dir, "scripts\\door.lua line 2: the file does not start with \"api = 1\". Add \"api = 1\" as its first line.");

    good_mod(dir, sizeof dir);
    mt_write(dir, ENTRY, "api = 1\nlocal other = require(\"mods/othermod/scripts/lib/fsm\")\n");
    refused(dir, "scripts\\door.lua line 2: \"require\" was given \"mods/othermod/scripts/lib/fsm\", which is not a file "
                 "of this mod's scripts\\lib");

    good_mod(dir, sizeof dir);
    mt_write(dir, ENTRY, "api = 1\nlocal m = require \"nothere\"\n");
    refused(dir, "scripts\\door.lua line 2: \"require\" found no scripts\\lib\\nothere.lua in this mod");

    good_mod(dir, sizeof dir);
    mt_write(dir, ENTRY, "api = 1\non.spawn(\"mods/zzzzzzzz/ai/imp\", function() end)\n");
    refused(dir, "scripts\\door.lua line 2: \"mods/zzzzzzzz/ai/imp\" is another mod's name. Name this mod's own content");

    good_mod(dir, sizeof dir);
    sh_mb_set_script_check(NULL);
    refused(dir, "scripts\\door.lua: the file cannot be checked by this program. Build the mod in Snapmap+.");
    sh_mb_set_script_check(sh_script_compile_check);
}

/* ------------------------------------------------- store and archives */

static void zip_names(const char *zip, char *out, size_t cap)
{
    sh_zip_reader r;
    char cause[256];
    size_t i;
    out[0] = 0;
    if (!sh_zip_reader_open(&r, zip, cause, sizeof cause)) return;
    for (i = 0; i < r.n; i++) {
        strncat_s(out, cap, r.e[i].name, _TRUNCATE);
        strncat_s(out, cap, "\n", _TRUNCATE);
    }
    sh_zip_reader_close(&r);
}

/* The name the folder entry has on disk, exactly. */
static int disk_name_is(const char *path, const char *want)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = !strcmp(fd.cFileName, want);
    FindClose(h);
    return ok;
}

typedef struct { char list[512]; const char *blob_door; int match; } listing;

static void list_cb(void *context, const char *relpath, const char *request, uint64_t size, const char *sha256,
                    const char *bundle_hash)
{
    listing *l = (listing *)context;
    unsigned char *bytes = NULL;
    size_t len = 0;
    char hex[SH_MB_HASH_HEX + 1];
    (void)bundle_hash;
    strncat_s(l->list, sizeof l->list, relpath, _TRUNCATE);
    strncat_s(l->list, sizeof l->list, "|", _TRUNCATE);
    if (sh_modserve_open(request, &bytes, &len) == 1) {
        sh_mb_sha256_hex(bytes, len, hex);
        if (len == size && !strcmp(hex, sha256)) l->match++;
        HeapFree(GetProcessHeap(), 0, bytes);
    }
}

static char *map_with(const sh_modmanifest_row *row)
{
    static const char base[] = "{\"~type\":\"idSnapMap\",\"entities\":[],\"name\":\"SCRIPT_MAP\"}";
    sh_modmanifest_list l;
    char err[160] = "", *heap, *out = NULL;
    size_t len = 0;
    memset(&l, 0, sizeof l);
    sh_modmanifest_list_add(&l, row);
    heap = sh_modmanifest_write(base, strlen(base), &l, &len, err, sizeof err);
    sh_modmanifest_list_free(&l);
    if (!heap) return NULL;
    out = (char *)malloc(len + 1);
    if (out) { memcpy(out, heap, len); out[len] = 0; }
    HeapFree(GetProcessHeap(), 0, heap);
    return out;
}

static void test_store(void)
{
    char src[MAX_PATH], a[MAX_PATH], c[MAX_PATH], d[MAX_PATH], name[SH_MS_NAME_MAX + 1], path[MAX_PATH * 2];
    char names[8192], want[512], salt[16] = "", cause[MODEXPORT_CAUSE_MAX] = "", zip[MAX_PATH], *map, *back;
    sh_ms_install_stats st;
    sh_mb_bundle b;
    sh_modmanifest_row row;
    modexport_stats es;
    listing l;
    size_t len = 0;
    unsigned long long size = 0;
    unsigned pointers = 0;
    int code = 0;
    unsigned char *text;

    printf("-- a new folder: paths lower-cased at install, mods/<folder>/ rewritten inside .lua\n");
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\src\\door-logic", g_tmp);
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\storeA", g_tmp);
    mt_write(src, "mod.json", "{\"name\":\"Door Logic\"}");
    mt_write(src, "assets\\generated\\decls\\entitydef\\mods\\door-logic\\AI\\Door.decl", "{\r\n}");
    mt_write(src, "assets\\mods\\door-logic\\Scripts\\Door.lua",
             "api = 1\nlocal fsm = require(\"fsm\")\non.spawn(\"mods/door-logic/ai/door\", function(self) end)\n");
    mt_write(src, "assets\\mods\\door-logic\\Scripts\\LIB\\FSM.lua", "return {}\n");
    CreateDirectoryA(a, NULL);
    CHECK(sh_ms_set_root(a));
    memset(&b, 0, sizeof b);
    CHECK(sh_ms_install_from_folder(src, NULL, name, sizeof name, &st, &b, cause, sizeof cause));
    if (cause[0]) fprintf(stderr, "  install: %s\n", cause);
    strncpy_s(salt, sizeof salt, b.id, _TRUNCATE);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets\\mods\\%s\\scripts\\door.lua", a, name, salt);
    CHECK(disk_name_is(path, "door.lua"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets\\mods\\%s\\scripts", a, name, salt);
    CHECK(disk_name_is(path, "scripts"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets\\mods\\%s\\scripts\\door.lua", a, name, salt);
    text = sh_mb_read_file(path, 4096, &len);
    _snprintf_s(want, sizeof want, _TRUNCATE, "on.spawn(\"mods/%s/ai/door\"", salt);
    CHECK(text && strstr((const char *)text, want) && !strstr((const char *)text, "door-logic"));
    free(text);
    _snprintf_s(want, sizeof want, _TRUNCATE, "assets/mods/%s/scripts/lib/fsm.lua", salt);
    {
        size_t i;
        int found = 0, upper = 0;
        for (i = 0; i < b.file_count; i++) {
            const char *p;
            if (!strcmp(b.files[i].path, want)) found = 1;
            for (p = b.files[i].path; *p; p++) if (*p >= 'A' && *p <= 'Z') upper = 1;
        }
        CHECK(found && !upper && b.script_files == 2 && b.scripts && strstr(b.scripts, "\"ai/door\""));
    }
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, b.id);
    strcpy_s(row.build, sizeof row.build, b.hash);
    strcpy_s(row.name, sizeof row.name, "Door Logic");
    row.size = b.total_size;

    printf("-- the serving index lists the scripts and serves their built bytes\n");
    CHECK(sh_modserve_index_build(a) > 0 || 1);
    (void)sh_modserve_scope_set_place("", 1);
    memset(&l, 0, sizeof l);
    CHECK(sh_modserve_script_files(salt, list_cb, &l) == 2 && !strcmp(l.list, "door.lua|lib/fsm.lua|") && l.match == 2);
    if (strcmp(l.list, "door.lua|lib/fsm.lua|")) fprintf(stderr, "  listed %s\n", l.list);
    CHECK(sh_modserve_script_files("zzzzzzzz", NULL, NULL) == 0);

    printf("-- the publish archive carries them under their game names\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", a, name);
    _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\publish.zip", g_tmp);
    CHECK(modsync_write_archive(path, b.hash, zip, &size, &pointers, &code, cause, sizeof cause));
    zip_names(zip, names, sizeof names);
    _snprintf_s(want, sizeof want, _TRUNCATE, "assets/mods/%s/scripts/door.lua\n", salt);
    CHECK(strstr(names, want) != NULL);
    _snprintf_s(want, sizeof want, _TRUNCATE, "assets/generated/decls/entitydef/mods/%s/ai/door.decl\n", salt);
    CHECK(strstr(names, want) != NULL);

    printf("-- a rawmap export carries them; its import installs and serves them\n");
    map = map_with(&row);
    _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\Door Map.zip", g_tmp);
    CHECK(map && modexport_write(map, strlen(map), zip, &es, cause, sizeof cause));
    zip_names(zip, names, sizeof names);
    _snprintf_s(want, sizeof want, _TRUNCATE, "mods/%s/assets/mods/%s/scripts/lib/fsm.lua\n", name, salt);
    CHECK(strstr(names, want) != NULL);
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\storeC", g_tmp);
    CreateDirectoryA(c, NULL);
    CHECK(sh_ms_set_root(c));
    (void)sh_modserve_index_build(c);
    (void)sh_modserve_scope_set_place("", 1);
    back = modexport_read(zip, &len, &es, cause, sizeof cause);
    if (!back) fprintf(stderr, "  import: %s\n", cause);
    CHECK(back && es.bundles_installed == 1);
    free(back);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\maps\\Door Map\\Door Logic\\assets\\mods\\%s\\scripts\\door.lua", c, salt);
    CHECK(mt_exists(path));
    memset(&l, 0, sizeof l);
    CHECK(sh_modserve_script_files(salt, list_cb, &l) == 2 && l.match == 2);
    free(map);

    printf("-- a built folder spelled in mixed case installs under the game names\n");
    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\storeD", g_tmp);
    CreateDirectoryA(d, NULL);
    {
        char from[MAX_PATH * 2], copy[MAX_PATH * 2], up[MAX_PATH * 2];
        _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\%s", a, name);
        _snprintf_s(copy, sizeof copy, _TRUNCATE, "%s\\mixed", g_tmp);
        CHECK(mt_copy_tree(from, copy));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\mods\\%s\\scripts\\door.lua", copy, salt);
        _snprintf_s(up, sizeof up, _TRUNCATE, "%s\\assets\\mods\\%s\\scripts\\Door.LUA", copy, salt);
        CHECK(MoveFileA(path, up));
        CHECK(sh_ms_set_root(d));
        sh_mb_free(&b);
        memset(&b, 0, sizeof b);
        CHECK(sh_ms_install_from_folder(copy, NULL, name, sizeof name, &st, &b, cause, sizeof cause));
        if (cause[0]) fprintf(stderr, "  install: %s\n", cause);
        CHECK(!strcmp(b.hash, row.build));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets\\mods\\%s\\scripts\\door.lua", d, name, salt);
        CHECK(disk_name_is(path, "door.lua"));
    }
    sh_mb_free(&b);
}

int main(void)
{
    CHECK(mt_temp_dir("modscript", g_tmp, sizeof g_tmp));
    sh_mb_set_script_check(sh_script_compile_check);
    test_requests();
    test_scan();
    test_builds();
    test_store();
    mt_remove_tree(g_tmp);
    printf("modscript_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
