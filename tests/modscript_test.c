/* modscript_test -- mod scripts in the builder, the serving index and the archives (docs/architecture.md "Mod
 * scripts"). The builder admits scripts/<name>.lua and scripts/lib/<name>.lua and refuses misplaced or compiled
 * .lua, a NUL byte, non-UTF-8 text, a syntax error, an entry without "api = 1" and a require of a module the mod
 * does not carry; the index, publish archive and rawmap export carry the scripts, and an import with scripts off
 * refuses with SH_MODUI_SCRIPTS_OFF. A script that registers an input is served as a Script node row.
 *
 *   modscript_test.exe <tests\fixtures\scripts> */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modexport.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modui.h"
#include "../src/backend/modcache.h"
#include "../src/backend/modpin.h"
#include "../src/backend/modunit.h"
#include "../src/backend/modzip.h"
#include "../src/backend/script/script_compile.h"
#include "../src/backend/script/script_source.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* The player's setting "Play maps whose mods include scripts" (modui.c). */
static int g_scripts_on = 1;
int sh_modui_scripted_maps_allowed(void) { return g_scripts_on; }

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
#define ENTRY "scripts\\door.lua"
#define LIB "scripts\\lib\\fsm.lua"
#define DEF "entities\\ai\\door.decl"

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
    printf("  refused: %s\n", sh_mb_first_cause(&b));
    CHECK(!ok && strstr(sh_mb_first_cause(&b), want) != NULL);
    if (ok || !strstr(sh_mb_first_cause(&b), want)) fprintf(stderr, "    wanted \"%s\"\n", want);
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
        "on.animation(\"ai/brute\", \"throw\", f)\n"                         /* 6 */
        "on.input(\"switch_used\", function(n) node(\"open_door\"):fire() end)\n"   /* 7 */
        "on.spawn(name, f) on.damage(\"a\" .. b, f) on.map_start(f)\n"  /* 8 */
        "local m = require \"fsm\" local u = require(\"util\") require(mod)\n"       /* 9 */
        "on.input(\"esc\\\"aped\", f) on . death ( [[ai/long]] , f)\n"   /* 10 */
        "on.pain('ai/demon/tyrant', f) on.projectile(\"ai/demon/tyrant\", f)\n"   /* 11 */
        "on.frame_event('ai/demon/tyrant', 'blackboard', f) on.frame_event(def, 'x', f)\n";  /* 12 */
    static const char want[] =
        "5:bind.spawn:ai/door|5:bind.think:ai/door|6:bind.animation:ai/brute|7:input:switch_used|7:output:open_door|"
        "8:bind.spawn?:|8:bind.damage?:|9:require:fsm|9:require:util|9:require?:|10:input?:|10:bind.death:ai/long|"
        "11:bind.pain:ai/demon/tyrant|11:bind.projectile:ai/demon/tyrant|12:bind.frame_event:ai/demon/tyrant|"
        "12:bind.frame_event?:|";
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
    HOS(file, sh_mb_script_file_name("mods/" SALT "/scripts/lib/fsm.lua"));
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
    if (b.errors) fprintf(stderr, "  %s\n", sh_mb_first_cause(&b));
    /* the entry spells the engine's name of its own mod once (line 5): one warning, and it still builds */
    CHECK(b.script_files == 2 && b.warnings == 1 && b.first_warning && strstr(b.first_warning, "line 5 writes mods/" SALT "/ai/door"));
    CHECK(b.scripts && !strcmp(b.scripts, "{\"dynamic\":1,\"entities\":[{\"events\":[\"spawn\",\"think\"],\"name\":\"ai/door\"}],"
                                         "\"files\":2,\"inputs\":[\"switch_used\"],\"node\":true,\"outputs\":[\"open_door\"]}"));
    if (b.scripts) printf("  scripts: %s\n", b.scripts);
    f = find_file(&b, "mods/" SALT "/scripts/door.lua");
    CHECK(f && f->ptype && !strcmp(f->ptype, "script") && f->cls == SH_MB_CLASS_ADD && !f->stock);
    CHECK(HO(rel, sh_mb_write(&b, dir, HO_P(0))));
    memset(&back, 0, sizeof back);
    if (!sh_mb_load_folder(dir, &back)) fprintf(stderr, "  load: %s\n", sh_mb_first_cause(&back));
    CHECK(back.errors == 0 && back.script_files == 2 && back.scripts && b.scripts &&
          !strcmp(back.scripts, b.scripts) && !strcmp(back.hash, b.hash));
    sh_mb_free(&back);
    sh_mb_free(&b);

    printf("-- a mod without scripts records none\n");
    good_mod(dir, sizeof dir);
    mt_delete(dir, ENTRY);
    mt_delete(dir, LIB);
    CHECK(build(dir, &b) && !b.scripts && b.script_files == 0);
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

    printf("-- no count or size of the scripts is checked: 150 files, 6 MB, a 2 MB file, a long name\n");
    good_mod(dir, sizeof dir);
    for (i = 0; i < 148; i++) {
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "scripts\\lib\\m%03d.lua", i);
        mt_write(dir, rel, "return 1\n");
    }
    for (i = 0; i < 3; i++) {
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "scripts\\lib\\big%d.lua", i);
        put_big(dir, rel, 2u << 20);
    }
    mt_write(dir, "scripts\\lib\\"
                  "a_module_name_well_past_the_sixty_four_characters_the_builder_once_allowed_for_a_script.lua",
             "return 1\n");
    {
        sh_mb_bundle big;
        int ok = build(dir, &big);
        CHECK(ok && big.script_files == 154);
        if (!ok) fprintf(stderr, "    refused: %s\n", sh_mb_first_cause(&big));
        sh_mb_free(&big);
    }

    good_mod(dir, sizeof dir);
    mt_write(dir, "materials\\door.lua", "api = 1\n");
    /* Named in the author's words, by its path in the mod folder; never the salt. */
    {
        sh_mb_bundle b;
        const char *want = "materials/door.lua: this folder holds .decl files; move it to the folder for what it is";
        CHECK(!build(dir, &b) && !strstr(sh_mb_first_cause(&b), SALT));
        CHECK(!strcmp(sh_mb_first_cause(&b), want));
        if (strcmp(sh_mb_first_cause(&b), want)) fprintf(stderr, "    got: %s%c", sh_mb_first_cause(&b), 10);
        sh_mb_free(&b);
    }

    good_mod(dir, sizeof dir);
    mt_write(dir, "scripts\\more\\b.lua", "api = 1\n");
    refused(dir, "scripts\\more\\b.lua: a script goes in the mod's scripts folder");

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
    mt_write(dir, "decls\\snapeditorentitydef\\script.decl", "{\r\n}");
    refused(dir, "decls/snapeditorentitydef/script.decl: " SH_MB_SCRIPT_NODE_RESERVED);

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
    if (!HO(cause, sh_zip_reader_open(&r, zip, HO_P(0)))) return;
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

/* The folder that serves the local row on this computer (the installed mod), and the game as the unit proof sees it. */
static char g_pin_folder[MAX_PATH * 2];
static char *pin_folder(void *ctx, const sh_modmanifest_row *row)
{
    (void)ctx;
    (void)row;
    return g_pin_folder[0] ? _strdup(g_pin_folder) : NULL;
}
static char *pin_read(void *ctx, const char *request, size_t *len) { (void)ctx; (void)request; (void)len; return NULL; }
static int pin_game(void *ctx, const char *request, char sha[SH_MB_HASH_HEX + 1], char *source, size_t cap)
{
    (void)ctx;
    if (strstr(request, "mods/")) return 0;
    memset(sha, 'a', SH_MB_HASH_HEX);
    sha[SH_MB_HASH_HEX] = 0;
    strcpy_s(source, cap, "snap_gameresources");
    return 1;
}

/* A map placing the mod's door, with one local row of the mod (an export pins it). */
static char *map_with(const char *salt, const char *name)
{
    sh_modmanifest_row row;
    sh_modmanifest_list l;
    char base[400], placed[160], err[160] = "", *heap, *out = NULL;
    size_t len = 0;
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, salt);
    strcpy_s(row.build, sizeof row.build, SH_MODMANIFEST_LOCAL_BUILD);
    sh_modmanifest_row_set_name(&row, name);
    _snprintf_s(placed, sizeof placed, _TRUNCATE, "mods/%s/ai/door", salt);
    sh_modmanifest_row_add_name(&row, placed);
    _snprintf_s(base, sizeof base, _TRUNCATE,
                "{\"~type\":\"idSnapMap\",\"entities\":[{\"inherit\":\"%s\"}],\"name\":\"SCRIPT_MAP\"}", placed);
    memset(&l, 0, sizeof l);
    sh_modmanifest_list_add(&l, &row);
    sh_modmanifest_row_free(&row);
    heap = sh_modmanifest_write(base, strlen(base), &l, &len, err, sizeof err);
    sh_modmanifest_list_free(&l);
    if (!heap) return NULL;
    out = (char *)malloc(len + 1);
    if (out) { memcpy(out, heap, len); out[len] = 0; }
    HeapFree(GetProcessHeap(), 0, heap);
    return out;
}

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
        n += (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? count_files(child) : 1;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* Units and files in the content cache under the mods folder `root`. */
static void cache_counts(const char *root, size_t *units, size_t *files)
{
    char dir[MAX_PATH * 2];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\.content\\units", root);
    *units = count_files(dir);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\.content\\files", root);
    *files = count_files(dir);
}

/* What the mods folder holds besides its dot folders. */
static int mod_folders(const char *root)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    int n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (fd.cFileName[0] != '.') n++; } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* The bytes of the ZIP's entry (malloc'd, NUL-terminated), or NULL. */
static unsigned char *zip_entry_bytes(const char *zip, const char *entry, size_t *len)
{
    sh_zip_reader r;
    char cause[256];
    long at;
    unsigned char *b = NULL;
    if (!HO(cause, sh_zip_reader_open(&r, zip, HO_P(0)))) return NULL;
    at = sh_zip_reader_find(&r, entry);
    if (at >= 0) b = HOT(unsigned char *, cause, sh_zip_reader_read(&r, at, 64u << 20, len, HO_P(0)));
    sh_zip_reader_close(&r);
    return b;
}

static void test_store(void)
{
    char src[MAX_PATH], a[MAX_PATH], c[MAX_PATH], d[MAX_PATH], name[HO_TEXT + 1], path[MAX_PATH * 2];
    char names[8192], want[512], salt[16] = "", cause[HO_TEXT] = "", zip[MAX_PATH], *map, *back;
    sh_ms_install_stats st;
    sh_mb_bundle b;
    sh_modmanifest_row row;
    modexport_stats es;
    listing l;
    size_t len = 0;
    unsigned char *text;

    printf("-- a new folder: paths lower-cased at install, the mod's own names in .lua as the author writes them\n");
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\src\\door-logic", g_tmp);
    _snprintf_s(a, sizeof a, _TRUNCATE, "%s\\storeA", g_tmp);
    mt_write(src, "mod.json", "{\"name\":\"Door Logic\"}");
    mt_write(src, "entities\\AI\\Door.decl", "{\r\n}");
    mt_write(src, "Scripts\\Door.lua",
             "api = 1\nlocal fsm = require(\"fsm\")\non.spawn(\"ai/door\", function(self) end)\n");
    mt_write(src, "Scripts\\LIB\\FSM.lua", "return {}\n");
    CreateDirectoryA(a, NULL);
    CHECK(sh_ms_set_root(a));
    memset(&b, 0, sizeof b);
    CHECK(HO2(name, cause, sh_ms_install_from_folder(src, NULL, HO_P(0), &st, &b, HO_P(1))));
    if (cause[0]) fprintf(stderr, "  install: %s\n", cause);
    strncpy_s(salt, sizeof salt, b.id, _TRUNCATE);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\scripts\\door.lua", a, name);
    CHECK(disk_name_is(path, "door.lua"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\scripts", a, name);
    CHECK(disk_name_is(path, "scripts"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\scripts\\door.lua", a, name);
    text = sh_mb_read_file(path, 4096, &len);
    CHECK(text && strstr((const char *)text, "on.spawn(\"ai/door\"") && !strstr((const char *)text, "door-logic"));
    free(text);
    _snprintf_s(want, sizeof want, _TRUNCATE, "scripts/lib/fsm.lua");
    {
        size_t i;
        int found = 0, upper = 0;
        for (i = 0; i < b.file_count; i++) {
            const char *p;
            if (!strcmp(b.files[i].path, want)) found = 1;
            for (p = b.files[i].path; *p; p++) if (*p >= 'A' && *p <= 'Z') upper = 1;
        }
        CHECK(found && !upper && b.script_files == 2 && b.scripts && strstr(b.scripts, "\"ai/door\""));
        /* The block counts every script file, lib included: the store checks it against the archive. */
        CHECK(strstr(b.scripts, "\"files\":2") != NULL);
    }
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, b.id);
    strcpy_s(row.build, sizeof row.build, b.hash);
    sh_modmanifest_row_set_name(&row, "Door Logic");
    row.size = b.total_size;

    printf("-- the serving index lists the scripts and serves their built bytes\n");
    CHECK(sh_modserve_index_build(a) > 0 || 1);
    memset(&l, 0, sizeof l);
    CHECK(sh_modserve_script_files(salt, list_cb, &l) == 2 && !strcmp(l.list, "door.lua|lib/fsm.lua|") && l.match == 2);
    if (strcmp(l.list, "door.lua|lib/fsm.lua|")) fprintf(stderr, "  listed %s\n", l.list);
    CHECK(sh_modserve_script_files("zzzzzzzz", NULL, NULL) == 0);

    printf("-- a rawmap export carries the scripts in the unit that binds them, each file once by its SHA-256\n");
    sh_cache_set_mods_root(a);
    sh_cache_local_changed();
    _snprintf_s(g_pin_folder, sizeof g_pin_folder, _TRUNCATE, "%s\\%s", a, name);
    sh_modpin_set_folder(pin_folder, NULL);
    sh_modpin_set_read(pin_read, NULL);
    sh_modpin_set_game(pin_game, NULL);
    map = map_with(salt, "Door Logic");
    _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\Door Map.zip", g_tmp);
    CHECK(map && HO(cause, modexport_write(map, strlen(map), zip, &es, HO_P(0))));
    if (cause[0]) fprintf(stderr, "  export: %s\n", cause);
    /* one row, one unit (the door), three files: the entity, the entry script and the module it requires */
    CHECK(es.bundles == 1 && es.bundles_installed == 1 && es.files == 3 && es.entries == 5);
    zip_names(zip, names, sizeof names);
    CHECK(strstr(names, "Door Map.json\n") && strstr(names, "units/") && strstr(names, "files/") && !strstr(names, "mods/") &&
          !strstr(names, "scripts/"));
    {
        sh_modmanifest_list pinned;
        unsigned rej = 0;
        unsigned char *mapb, *manifest = NULL, *disk = NULL, *carried = NULL;
        size_t ml = 0, mlen = 0, dl = 0, el = 0;
        char entry[100], hex[SH_MB_HASH_HEX + 1] = "?", fentry[100], on_disk[MAX_PATH * 2];
        mapb = zip_entry_bytes(zip, "Door Map.json", &ml);
        memset(&pinned, 0, sizeof pinned);
        CHECK(mapb && sh_modmanifest_read((const char *)mapb, ml, &pinned, &rej) == 1 && pinned.rows[0].pinned &&
              pinned.rows[0].unit_count == 1);
        if (pinned.count && pinned.rows[0].unit_count == 1) {
            _snprintf_s(entry, sizeof entry, _TRUNCATE, "units/%s.json", pinned.rows[0].units[0]);
            manifest = zip_entry_bytes(zip, entry, &mlen);
        }
        /* the unit's manifest names the scripts by their mod-relative paths */
        CHECK(manifest && strstr((const char *)manifest, "\"path\":\"scripts/door.lua\"") &&
              strstr((const char *)manifest, "\"path\":\"scripts/lib/fsm.lua\"") &&
              strstr((const char *)manifest, "\"path\":\"entities/ai/door.decl\""));
        /* the carried script is the installed script, byte for byte, under its SHA-256 */
        _snprintf_s(on_disk, sizeof on_disk, _TRUNCATE, "%s\\%s\\scripts\\door.lua", a, name);
        disk = sh_mb_read_file(on_disk, 1u << 20, &dl);
        if (disk) sh_mb_sha256_hex(disk, dl, hex);
        _snprintf_s(fentry, sizeof fentry, _TRUNCATE, "files/%s", hex);
        carried = disk ? zip_entry_bytes(zip, fentry, &el) : NULL;
        CHECK(disk && carried && el == dl && !memcmp(disk, carried, dl));
        CHECK(manifest && disk && strstr((const char *)manifest, hex) != NULL);
        free(carried);
        free(disk);
        free(manifest);
        sh_modmanifest_list_free(&pinned);
        free(mapb);
    }

    printf("-- with scripts turned off, the import refuses with the sentence that says what to turn on, and nothing goes into the cache\n");
    _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\storeC", g_tmp);
    CreateDirectoryA(c, NULL);
    sh_cache_set_mods_root(c);
    sh_cache_local_changed();
    g_pin_folder[0] = 0;
    g_scripts_on = 0;
    back = HOT(char *, cause, modexport_read(zip, &len, &es, HO_P(0)));
    CHECK(!back && !strcmp(cause, SH_MODUI_SCRIPTS_OFF) && es.bundles_installed == 0 && es.files == 0);
    {
        size_t cu, cf;
        cache_counts(c, &cu, &cf);
        CHECK(cu == 0 && cf == 0 && mod_folders(c) == 0);
    }
    /* a prefab's export of the same unit is refused the same way */
    {
        static const char PREFAB[] = "{\"entities\":[],\"~type\":\"idSnapEntityPrefab\"}";
        sh_modmanifest_list rows;
        char pzip[MAX_PATH], *text = NULL, *pname = NULL, *pcause = NULL;
        size_t pl = 0, cu, cf;
        unsigned rej = 0;
        modexport_stats ps;
        g_scripts_on = 1;
        sh_cache_set_mods_root(a);
        sh_cache_local_changed();
        _snprintf_s(g_pin_folder, sizeof g_pin_folder, _TRUNCATE, "%s\\%s", a, name);
        memset(&rows, 0, sizeof rows);
        CHECK(sh_modmanifest_read(map, strlen(map), &rows, &rej) == 1);
        _snprintf_s(pzip, sizeof pzip, _TRUNCATE, "%s\\Door Kit.zip", g_tmp);
        CHECK(HO(cause, modexport_write_prefab("Door Kit", PREFAB, sizeof PREFAB - 1, &rows, NULL, NULL, pzip, &ps, HO_P(0))));
        CHECK(ps.bundles == 1 && ps.bundles_installed == 1 && ps.files == 3);
        sh_modmanifest_list_free(&rows);
        sh_cache_set_mods_root(c);
        sh_cache_local_changed();
        g_pin_folder[0] = 0;
        g_scripts_on = 0;
        CHECK(!modexport_read_prefab(pzip, NULL, NULL, &text, &pl, &pname, &ps, &pcause) && pcause &&
              !strcmp(pcause, SH_MODUI_SCRIPTS_OFF_PREFAB) && !text && !pname);
        free(pcause);
        cache_counts(c, &cu, &cf);
        CHECK(cu == 0 && cf == 0 && mod_folders(c) == 0);
    }
    /* a map without scripts is still imported with scripts off */
    {
        static const char PLAIN[] = "{\"~type\":\"idSnapMap\",\"entities\":[],\"name\":\"PLAIN\"}";
        char plain[MAX_PATH];
        modexport_stats ps;
        _snprintf_s(plain, sizeof plain, _TRUNCATE, "%s\\plain.zip", g_tmp);
        CHECK(HO(cause, modexport_write(PLAIN, sizeof PLAIN - 1, plain, &ps, HO_P(0))));
        back = HOT(char *, cause, modexport_read(plain, &len, &ps, HO_P(0)));
        CHECK(back && len == sizeof PLAIN - 1 && !memcmp(back, PLAIN, len));
        free(back);
    }

    printf("-- with scripts allowed, the import puts the unit and its files into the cache and installs nothing\n");
    g_scripts_on = 1;
    back = HOT(char *, cause, modexport_read(zip, &len, &es, HO_P(0)));
    if (!back) fprintf(stderr, "  import: %s\n", cause);
    CHECK(back && es.bundles == 1 && es.bundles_installed == 1 && es.files == 3 && es.bundles_present == 0 &&
          es.bundles_absent == 0);
    {
        size_t cu, cf;
        cache_counts(c, &cu, &cf);
        CHECK(cu == 1 && cf == 3);
        CHECK(mod_folders(c) == 0);   /* no mod folder: nothing is installed */
    }
    free(back);
    free(map);

    printf("-- a built folder spelled in mixed case installs under the game names\n");
    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\storeD", g_tmp);
    CreateDirectoryA(d, NULL);
    {
        char from[MAX_PATH * 2], copy[MAX_PATH * 2], up[MAX_PATH * 2];
        _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\%s", a, name);
        _snprintf_s(copy, sizeof copy, _TRUNCATE, "%s\\mixed", g_tmp);
        CHECK(mt_copy_tree(from, copy));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\scripts\\door.lua", copy);
        _snprintf_s(up, sizeof up, _TRUNCATE, "%s\\scripts\\Door.LUA", copy);
        CHECK(MoveFileA(path, up));
        CHECK(sh_ms_set_root(d));
        sh_mb_free(&b);
        memset(&b, 0, sizeof b);
        CHECK(HO2(name, cause, sh_ms_install_from_folder(copy, NULL, HO_P(0), &st, &b, HO_P(1))));
        if (cause[0]) fprintf(stderr, "  install: %s\n", cause);
        CHECK(!strcmp(b.hash, row.build));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\scripts\\door.lua", d, name);
        CHECK(disk_name_is(path, "door.lua"));
    }
    sh_mb_free(&b);
}

/* ---- the played map's scripts follow the scope's builds (script_source.c) */

/* The serving layer's side of the provider is not used here. */
void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context) { (void)fn; (void)context; }

/* A mod's scripts are read for the build the scope lists: the root's for the player's own map, the row's own
 * for a stranger's (which nothing holds once the root's mod was rebuilt). A script edited since the build is
 * named, not run. */
static void test_map_scripts(void)
{
    static const char OLD[] = "00000000000000000000000000000000000000000000000000000000000000aa";
    char src[MAX_PATH], root[MAX_PATH], name[HO_TEXT + 1], cause[HO_TEXT] = "", path[MAX_PATH * 2];
    const char *hashes[2];
    sh_script_mod_desc mods[4];
    sh_ms_install_stats st;
    sh_mb_bundle b;
    const char *stale;
    int count = 0;
    printf("-- the played map's scripts: the root's build for an own map, strict for a stranger's, a changed file named\n");
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\src\\own-door", g_tmp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\storeScope", g_tmp);
    mt_write(src, "mod.json", MOD_JSON);
    mt_write(src, DEF, "{\r\n}");
    mt_write(src, ENTRY, GOOD_ENTRY);
    mt_write(src, LIB, "return { name = \"door_input\" }\n");
    CreateDirectoryA(root, NULL);
    CHECK(sh_ms_set_root(root));
    memset(&b, 0, sizeof b);
    CHECK(HO2(name, cause, sh_ms_install_from_folder(src, NULL, HO_P(0), &st, &b, HO_P(1))));
    CHECK(sh_modserve_index_build(root) > 0);
    sh_modserve_scope_enable(1);

    /* Own map: the scope lists the root's build (the row recorded OLD). */
    hashes[0] = b.hash;
    CHECK(sh_modserve_scope_set(hashes, 1, 1) >= 0);
    CHECK(sh_script_source_map_mods(mods, 4) == 1 && !strcmp(mods[0].salt, SALT));
    CHECK(sh_script_source_stale(SALT, &count) == NULL && count == 0);
    CHECK(sh_script_source_files()->read != NULL);

    /* A stranger's map: the scope lists the row's own build, which nothing holds. */
    hashes[0] = OLD;
    CHECK(sh_modserve_scope_set(hashes, 1, 1) >= 0);
    CHECK(sh_script_source_map_mods(mods, 4) == 0);

    /* A script edited after the build: listed, not kept, named; the other file is kept. */
    hashes[0] = b.hash;
    CHECK(sh_modserve_scope_set(hashes, 1, 1) >= 0);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\scripts\\door.lua", root, name);
    {
        FILE *f = NULL;
        CHECK(!fopen_s(&f, path, "ab") && f != NULL);
        if (f) { fputs("-- edited after the build\n", f); fclose(f); }
    }
    CHECK(sh_script_source_map_mods(mods, 4) == 1);
    stale = sh_script_source_stale(SALT, &count);
    CHECK(stale != NULL && !strcmp(stale, "door.lua") && count == 1);
    {
        char *text = NULL;
        size_t len = 0;
        const sh_script_source *s = sh_script_source_files();
        CHECK(s->read(s->context, SALT, "door.lua", &text, &len) == -1);
        CHECK(s->read(s->context, SALT, "lib/fsm.lua", &text, &len) == 1 && text != NULL);
        if (text) s->release(s->context, text);
    }
    (void)sh_modserve_scope_set(NULL, 0, 0);
    sh_modserve_scope_enable(0);
    sh_script_source_clear();
    sh_mb_free(&b);
}

/* With scripts off for content from elsewhere, a map's composition (its pinned rows) runs no script whatever
 * path opened the map, while the player's own folder always does. The policy is injected (sh_script_source_set_policy). */
static int g_foreign_on;
static int foreign_allowed(void) { return g_foreign_on; }
static void test_scripts_setting_at_open(void)
{
    static const char COMP[] = "00000000000000000000000000000000000000000000000000000000000000c0";
    char src[MAX_PATH], root[MAX_PATH], comp[MAX_PATH * 2], name[HO_TEXT + 1], cause[HO_TEXT] = "", path[MAX_PATH * 2];
    const char *hashes[1];
    sh_script_mod_desc mods[4];
    sh_ms_install_stats st;
    sh_mb_bundle b;
    char *json, *at;
    size_t len = 0;
    printf("-- the scripts setting at map open: a composition's scripts follow it, the player's own folder's always run\n");
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\src\\comp-door", g_tmp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\storeForeign", g_tmp);
    mt_write(src, "mod.json", MOD_JSON);
    mt_write(src, DEF, "{\r\n}");
    mt_write(src, ENTRY, GOOD_ENTRY);
    mt_write(src, LIB, "return { name = \"door_input\" }\n");
    CreateDirectoryA(root, NULL);
    CHECK(sh_ms_set_root(root));
    memset(&b, 0, sizeof b);
    CHECK(HO2(name, cause, sh_ms_install_from_folder(src, NULL, HO_P(0), &st, &b, HO_P(1))));
    /* the same content as a composition of the content cache (a pinned row's), under a build of its own */
    _snprintf_s(comp, sizeof comp, _TRUNCATE, "%s\\.content\\served\\%s", root, COMP);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\mod.json", root, name);
    json = (char *)sh_mb_read_file(path, 1u << 20, &len);
    CHECK(json != NULL && strstr(json, b.hash) != NULL);
    if (json && (at = strstr(json, b.hash)) != NULL) memcpy(at, COMP, 64);
    if (json) mt_write(comp, "mod.json", json);
    free(json);
    mt_write(comp, DEF, "{\r\n}");
    mt_write(comp, ENTRY, GOOD_ENTRY);
    mt_write(comp, LIB, "return { name = \"door_input\" }\n");
    CHECK(sh_modserve_index_build(root) > 0);
    sh_modserve_scope_enable(1);
    sh_script_source_set_policy(foreign_allowed);

    g_foreign_on = 0;
    hashes[0] = COMP;                         /* a pinned row: content from elsewhere */
    CHECK(sh_modserve_scope_set(hashes, 1, 1) >= 0);
    CHECK(sh_script_source_map_mods(mods, 4) == 0);
    hashes[0] = b.hash;                       /* the player's own folder */
    CHECK(sh_modserve_scope_set(hashes, 1, 1) >= 0);
    CHECK(sh_script_source_map_mods(mods, 4) == 1 && !strcmp(mods[0].salt, SALT));
    g_foreign_on = 1;
    hashes[0] = COMP;
    CHECK(sh_modserve_scope_set(hashes, 1, 1) >= 0);
    CHECK(sh_script_source_map_mods(mods, 4) == 1);

    sh_script_source_set_policy(NULL);
    (void)sh_modserve_scope_set(NULL, 0, 0);
    sh_modserve_scope_enable(0);
    sh_script_source_clear();
    sh_mb_free(&b);
}

/* ------------------------------------------- fixtures and the Script node */

static char g_fixtures[MAX_PATH];

typedef struct extra_seek { const char *salt; int found; } extra_seek;

static void extra_cb(void *context, const char *id, const char *hash, int place, const char *extra)
{
    extra_seek *s = (extra_seek *)context;
    char key[64];
    (void)hash;
    (void)place;
    _snprintf_s(key, sizeof key, _TRUNCATE, "\"%s_script_node\":\"Script\"", s->salt);
    if (id && !strcmp(id, s->salt) && extra && strstr(extra, key) && strstr(extra, "\"en\":{")) s->found = 1;
}

static void test_fixtures(void)
{
    static const char *const mods[] = { "door-logic", "spin-heal", "brute", "script-probe", "script-runaway", NULL };
    char src[MAX_PATH * 2], e[MAX_PATH], name[HO_TEXT + 1], cause[HO_TEXT] = "";
    char hashes[4][SH_MODSERVE_HASH_HEX + 1];
    const char *names[1];
    sh_ms_install_stats st;
    sh_mb_bundle b;
    sh_modserve_hit hit;
    unsigned char *bytes = NULL;
    size_t len = 0;
    extra_seek seek = { "doorlogc", 0 };
    int i;

    printf("-- the example and probe fixture mods build without errors or warnings\n");
    for (i = 0; mods[i]; i++) {
        _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\%s", g_fixtures, mods[i]);
        CHECK(build(src, &b) && b.errors == 0 && b.warnings == 0 && b.script_files == 1);
        if (b.errors || b.warnings) fprintf(stderr, "  %s: %s%s\n", mods[i], sh_mb_first_cause(&b), b.first_warning);
        CHECK(!strcmp(mods[i], "door-logic") == (b.scripts && strstr(b.scripts, "\"node\":true") != NULL));
        sh_mb_free(&b);
    }

    printf("-- the serving index serves a scripted mod's Script node row\n");
    _snprintf_s(e, sizeof e, _TRUNCATE, "%s\\storeE", g_tmp);
    CreateDirectoryA(e, NULL);
    CHECK(sh_ms_set_root(e));
    for (i = 0; i < 2; i++) {
        _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\%s", g_fixtures, i ? "script-probe" : "door-logic");
        memset(&b, 0, sizeof b);
        CHECK(HO2(name, cause, sh_ms_install_from_folder(src, NULL, HO_P(0), &st, &b, HO_P(1))));
        if (cause[0]) fprintf(stderr, "  install: %s\n", cause);
        if (!i) strcpy_s(hashes[3], sizeof hashes[3], b.hash);
        sh_mb_free(&b);
    }
    CHECK(sh_modserve_index_build(e) == 2);
    memset(&hit, 0, sizeof hit);
    CHECK(sh_modserve_lookup("generated/decls/snapeditorentitydef/mods/doorlogc/script.decl", &hit) && hit.is_decl);
    CHECK(sh_modserve_open("generated/decls/snapeditorentitydef/mods/doorlogc/script.decl", &bytes, &len) == 1);
    CHECK(bytes && strstr((const char *)bytes, "entityDef = \"mods/doorlogc/script\";") &&
          strstr((const char *)bytes, "inherit = \"logic/relay\";"));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    bytes = NULL;
    CHECK(sh_modserve_open("generated/decls/entitydef/mods/doorlogc/script.decl", &bytes, &len) == 1);
    CHECK(bytes && strstr((const char *)bytes, "inherit = \"snapmaps/logic/relay\";"));
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    /* a placed node names the mod's build; the probe, with no input, has no row */
    names[0] = "mods/doorlogc/script";
    CHECK(modserve_bundles_for_names(names, 1, hashes, 3) == 1 && !strcmp(hashes[0], hashes[3]));
    CHECK(!sh_modserve_lookup("generated/decls/entitydef/mods/scprobe1/script.decl", NULL));
    CHECK(sh_modserve_lookup("generated/decls/snapeditorentitydef/mods/scprobe1/probe/marker.decl", NULL));
    /* the row's title and description reach the string layer */
    CHECK(sh_modserve_each_bundle_extra(extra_cb, &seek) >= 0 && seek.found);
}

int main(int argc, char **argv)
{
    strcpy_s(g_fixtures, sizeof g_fixtures, argc > 1 ? argv[1] : "fixtures\\scripts");
    CHECK(mt_temp_dir("modscript", g_tmp, sizeof g_tmp));
    sh_mb_set_script_check(sh_script_compile_check);
    test_requests();
    test_scan();
    test_builds();
    test_store();
    test_map_scripts();
    test_scripts_setting_at_open();
    test_fixtures();
    mt_remove_tree(g_tmp);
    printf("modscript_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
