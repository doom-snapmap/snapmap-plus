/* prefab_mods_test -- a prefab's details and the mods it needs (backend/prefab_mods.c,
 * backend/prefab_block.c), and its export and import in the unit layout (SPEC-distribution 5.16, D35).
 *
 * Over a real store and serving index (a mod built and installed into a temporary folder), covered:
 *   the block: put after the engine's members, replaced, stripped back to the engine's exact bytes (formatting,
 *     comments, an empty object, a first member), found in a tail, a text that is not an object refused; a view
 *     composed into a block and read back;
 *   comments: the serializer's comments removed outside strings, and Create's file written as plain JSON with its block;
 *   recording: Create writes an object prefab's closure into the file's block and no sidecar; description and tag edits
 *     rewrite the block and keep the rows; a prefab without mods gets a block naming none; a module prefab's file
 *     carries its summary and rows; a prefab an earlier version saved (a sidecar and no block, or rows naming "id" and
 *     "bundle") is not read, its placement and export are refused and an edit writes nothing;
 *   placement (resolve): nothing is ever held or fetched, and the poll always says 0. A local row nothing serves, or a
 *     pinned row whose units are not all in the content cache, refuses the placement with "This prefab was not placed:
 *     it needs <mods>, which is/are not on this computer. Ask its author for the prefab's .zip export." (printed and
 *     toasted); pinned rows whose units are here go to the open map through the pinned-rows op (a refusal reads "Prefab
 *     <name> was not placed: <cause>."), local rows are activated and attached as before; a DLC module no row provides
 *     is refused first; the slot's rows from the block, META_READ from the file or a tail;
 *   the ZIP: <name>.json with its rows pinned (modpin, format 2), units/<hash>.json and files/<sha256>, no mods/ folder
 *     and no index, byte-identical on a second export, also for a prefab without mods; a row no folder here serves, or
 *     content this computer lacks, refuses the export with the mod's name; the map importer refuses it and the prefab
 *     importer refuses a map's ZIP and every earlier shape;
 *   import: the units and files go into the content cache (each checked against its name), the prefab text into the
 *     library as it is, nothing installed (no mod folder, no refresh); "Imported prefab X with its mod content.",
 *     "Prefab X is already in your library; its mod content was added.", "Prefab X and its mod content are already
 *     here; nothing was changed."; a different prefab of that name kept beside it; an extracted export folder; content
 *     the export does not carry named in the message; a damaged entry, a cache that cannot be written, and a unit that
 *     carries scripts while scripts are off each refuse and take the prefab they added out again;
 *   the console verbs and the slot's outputs; prefab names.
 * argv[1] is unused; -v echoes the backend log. */
#define WIN32_LEAN_AND_MEAN
#include "../src/common/text.h"
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/prefab_mods.h"
#include "../src/backend/prefab_block.h"
#include "../src/backend/modpublished.h"
#include "../src/backend/modcache.h"
#include "../src/backend/modpin.h"
#include "../src/backend/modzip.h"
#include "../src/backend/modui.h"

#include "../src/backend/modexport.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modauthor.h"
#include "../src/backend/modbundle.h"
#include "../src/backend/module_prefab_map.h"
#include "../src/backend/modnet.h"
#include "../src/common/snapmap_plus_iface.h"
#include "mod_test_util.h"

/* The slot's answer (owned interface text) into the test's buffer, whole or the test ends. */
static int pm_slot(sh_module_prefab_call *call, char *out, size_t cap)
{
    int r;
    call->out = NULL;
    r = sh_prefab_mods_slot(call);
    if (call->out && strlen(call->out) >= cap) { fprintf(stderr, "a slot answer longer than the test's buffer\n"); abort(); }
    out[0] = 0;
    if (call->out) strcpy_s(out, cap, call->out);
    sh_iface_text_free(call->out);
    call->out = NULL;
    return r;
}


static int g_failed, g_checks, g_echo;
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
    if (g_echo && line) printf("    %s\n", line);
}

static int g_activations;
int modserve_activate_bundles(const char *const *bundle_hashes, int n)
{
    (void)bundle_hashes;
    if (n > 0) g_activations++;
    return 0;
}

#define HASH_UNIT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define GHOST "9999999999999999999999999999999999999999999999999999999999999999"

/* ---- the game and the folder, as the unit proof sees them -------------------------------------- */

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

/* ---- engine doubles -------------------------------------------------------------------------- */

typedef struct fake {
    char listed[4][65];
    int listed_n;
    int attaches;
    char attached[4][65];
    int activations;
    int prints, toasts, refreshes, main_calls;
    char last_print[1200], prints_all[8192], last_toast[512];
    int pinned_calls, pinned_rows;   /* use_pinned: calls, and the rows of the last one */
    int pinned_answer;               /* 1 used, -1 refused */
    char pinned_cause[200];
} fake;
static fake F;

/* The asset rule: the real serving index answers every row but the ghost's, which nothing serves. */
static int f_row_state(const sh_modmanifest_row *row, char **cause)
{
    if (cause) *cause = NULL;
    if (!strcmp(row->build, GHOST) || !strcmp(row->salt, "ghostmod")) {
        if (cause) ho_setf(cause, "This prefab uses %s, which is not installed.", row->name);
        return SH_MODPUB_ROW_MISSING;
    }
    return sh_modserve_row_served_for(row, "prefab", cause);
}
static int f_listed(const sh_modmanifest_row *row)
{
    int i;
    for (i = 0; i < F.listed_n; i++) if (!strcmp(F.listed[i], row->build)) return 1;
    return 0;
}
static int f_attach(const sh_modmanifest_row *row, char **note)
{
    (void)note; if (F.attaches < 4) strcpy_s(F.attached[F.attaches], sizeof F.attached[0], row->build);
    F.attaches++;
    if (F.listed_n < 4) strcpy_s(F.listed[F.listed_n++], sizeof F.listed[0], row->build);
    return 1;
}
static int f_activate(const sh_modmanifest_list *rows) { if (rows->count) F.activations++; return (int)rows->count; }
static int f_module_known(const char *name)
{
    if (!_stricmp(name, "maps/modules/ind/stock_room.decl")) return 1;
    return 0;
}
static void f_print(const char *line)
{
    F.prints++;
    strcpy_s(F.last_print, sizeof F.last_print, line);
    strncat_s(F.prints_all, sizeof F.prints_all, line, _TRUNCATE);
    strncat_s(F.prints_all, sizeof F.prints_all, "\n", _TRUNCATE);
}
static void f_toast(const char *text) { F.toasts++; strcpy_s(F.last_toast, sizeof F.last_toast, text); }
static int f_refresh(void) { F.refreshes++; return 1; }
static int f_call_main(int (*fn)(void *), void *ctx) { F.main_calls++; return fn(ctx) ? 1 : 1; }
/* The pinned rows join the open map (the product composes them): the double answers as the test says. */
static int f_use_pinned(const sh_modmanifest_list *pinned, char **cause)
{
    F.pinned_calls++;
    F.pinned_rows = (int)pinned->count;
    if (F.pinned_answer < 0) { ho_set(cause, F.pinned_cause); return -1; }
    return 1;
}

static const sh_prefab_mods_ops PM_OPS = {
    f_row_state, f_listed, f_attach, f_activate, f_module_known, f_print, f_toast, f_refresh, f_call_main, f_use_pinned
};
/* The same without the pinned rows' op. */
static const sh_prefab_mods_ops PM_OPS_NO_PINNED = {
    f_row_state, f_listed, f_attach, f_activate, f_module_known, f_print, f_toast, f_refresh, f_call_main, NULL
};

static void reset_fakes(void)
{
    memset(&F, 0, sizeof F);
    F.pinned_answer = 1;
    sh_prefab_mods_test_reset();
    sh_prefab_mods_bind(&PM_OPS);
}

/* ---- fixtures -------------------------------------------------------------------------------- */

static const char DOOR_DECL[] = "{\r\n\tinherit = \"snapmaps/doors/industrial_door_vertical\";\r\n}";

/* An object prefab: one modded door, one stock prop. */
static const char OBJECT_PREFAB[] =
    "{\"cameraYaw\":0,\"entities\":["
    "{\"displayName\":\"Door\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/testdoor/door\",\"name\":\"\"}},"
    "{\"displayName\":\"Crate\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/props/new/crate01\",\"name\":\"\"}}],"
    "\"grabDistance\":256,\"~type\":\"idSnapEntityPrefab\"}";
static const char STOCK_PREFAB[] =
    "{\"cameraYaw\":0,\"entities\":["
    "{\"displayName\":\"Crate\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/props/new/crate01\",\"name\":\"\"}}],"
    "\"grabDistance\":256,\"~type\":\"idSnapEntityPrefab\"}";

#define MODULE_PREFAB(module) \
    "{\"doorsAndCaps\":{\"portalDoors\":[],\"portalFrames\":[],\"~type\":\"idSnapDoorsAndCaps\"},\"entities\":[]," \
    "\"instanceEntities\":{\"entityIds\":[],\"offsets\":[0,0]}," \
    "\"instances\":[{\"moduleName\":\"" module "\",\"origin\":[0,0,0],\"orientation\":0}]," \
    "\"variables\":{\"allocCount\":[0,0,0,0,0]},\"~type\":\"idSnapMap\"}"

/* A mod given its salt already (a builder block holding the salt only), its
 * files under mods/<salt>/, installed at the store's root. */
static int install_mod(const char *src, const char *salt, const char *name, const char *decl_rel, const char *decl,
                       sh_modmanifest_row *row)
{
    char json[256], key[HO_TEXT + 1], cause[HO_TEXT];
    sh_ms_install_stats st;
    sh_mb_bundle b;
    int ok;
    _snprintf_s(json, sizeof json, _TRUNCATE,
                "{\"name\":\"%s\",\"snapmap-plus\":{\"format\":2,\"salt\":\"%s\"}}", name, salt);
    mt_write(src, "mod.json", json);
    mt_write(src, decl_rel, decl);
    memset(&b, 0, sizeof b);
    ok = HO2(key, cause, sh_ms_install_from_folder(src, NULL, HO_P(0), &st, &b, HO_P(1)));
    if (!ok) fprintf(stderr, "install %s: %s\n", src, cause);
    memset(row, 0, sizeof *row);
    if (ok) {
        strcpy_s(row->salt, sizeof row->salt, b.id);
        if (b.name) sh_modmanifest_row_set_name(row, b.name);
        strcpy_s(row->build, sizeof row->build, b.hash);
        row->size = b.total_size;
    }
    sh_mb_free(&b);
    return ok;
}

static int memmem_find(const char *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    if (!hay || k > n) return 0;
    for (i = 0; i + k <= n; i++) if (!memcmp(hay + i, needle, k)) return 1;
    return 0;
}

static int put_file(const char *path, const char *text)
{
    return sh_prefab_mods_write_file(path, text, strlen(text));
}

static char *slurp(const char *path, size_t *len) { return sh_prefab_mods_read_file(path, len); }

static int write_time(const char *path, FILETIME *out)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
    *out = fa.ftLastWriteTime;
    return 1;
}

/* ---- the tests ------------------------------------------------------------------------------- */

static void names(void)
{
    printf("-- prefab names: device names, separators and the sidecar suffix are refused\n");
    CHECK(sh_prefab_mods_valid_name("Door"));
    CHECK(sh_prefab_mods_valid_name("Door (2)"));
    CHECK(sh_prefab_mods_valid_name("console"));
    CHECK(!sh_prefab_mods_valid_name("con") && !sh_prefab_mods_valid_name("NUL") && !sh_prefab_mods_valid_name("aux.json"));
    CHECK(!sh_prefab_mods_valid_name("COM1") && !sh_prefab_mods_valid_name("lpt9.zip"));
    /* One rule with the Prefabs tab: COM0 and LPT0 are device names too, ".."
     * is refused anywhere, a device name before spaces and an extension too,
     * and the length is the tab's, in bytes. */
    CHECK(!sh_prefab_mods_valid_name("com0") && !sh_prefab_mods_valid_name("LPT0") && !sh_prefab_mods_valid_name("a..b"));
    CHECK(!sh_prefab_mods_valid_name("nul .txt") && sh_prefab_mods_valid_name("con2") && sh_prefab_mods_valid_name("v1.2"));
    {
        char longest[SH_PREFAB_NAME_MAX + 2];
        memset(longest, 'a', SH_PREFAB_NAME_MAX);
        longest[SH_PREFAB_NAME_MAX] = 0;
        CHECK(sh_prefab_mods_valid_name(longest));
        longest[SH_PREFAB_NAME_MAX] = 'a';
        longest[SH_PREFAB_NAME_MAX + 1] = 0;
        CHECK(!sh_prefab_mods_valid_name(longest));
    }
    CHECK(sh_prefab_mods_valid_name("J\xc3\xb6rg's crates"));

    CHECK(!sh_prefab_mods_valid_name("a/b") && !sh_prefab_mods_valid_name("a\\b") && !sh_prefab_mods_valid_name("a:b"));
    CHECK(!sh_prefab_mods_valid_name(" x") && !sh_prefab_mods_valid_name("x.") && !sh_prefab_mods_valid_name(""));
    CHECK(!sh_prefab_mods_valid_name("door.meta") && !sh_prefab_mods_valid_name(".."));
}

/* The block's text in a prefab file, or "" (static buffer). */
static const char *block_of(const char *text, size_t len)
{
    static char out[8192];
    sh_prefab_block_span span;
    out[0] = 0;
    if (text && sh_prefab_block_find(text, len, &span) == 1 && span.value_len < sizeof out) {
        memcpy(out, text + span.value_at, span.value_len);
        out[span.value_len] = 0;
    }
    return out;
}

/* 1 when the file at path, stripped of its block, is exactly `engine`. */
static int engine_bytes_are(const char *path, const char *engine)
{
    size_t len = 0, slen = 0;
    char *text = slurp(path, &len), *strip;
    int same;
    strip = sh_prefab_mods_strip_copy(text, len, &slen);
    same = strip && slen == strlen(engine) && !memcmp(strip, engine, slen);
    free(text);
    free(strip);
    return same;
}

static void block(void)
{
    static const char WITH[] = "{\"a\":1,\"~type\":\"idSnapEntityPrefab\"}";
    sh_prefab_block_span span;
    char *put, *again, type[64];
    size_t pl = 0, al = 0, n;
    printf("-- the block: inserted after the engine's members, stripped back to the engine's bytes\n");
    CHECK(sh_prefab_block_find(WITH, sizeof WITH - 1, &span) == 0);
    put = sh_prefab_block_put(WITH, sizeof WITH - 1, "{\"format\":1}", 12, &pl);
    CHECK(put && !strcmp(put, "{\"a\":1,\"~type\":\"idSnapEntityPrefab\",\n\"snapmapPlus\":{\"format\":1}}") && pl == strlen(put));
    CHECK(put && sh_prefab_block_find(put, pl, &span) == 1 && span.value_len == 12 && !memcmp(put + span.value_at, "{\"format\":1}", 12));
    CHECK(put && sh_prefab_block_top_type(put, pl, type, sizeof type) == 1 && !strcmp(type, "idSnapEntityPrefab"));
    /* Replacing it keeps one block. */
    again = sh_prefab_block_put(put, pl, "{\"format\":1,\"x\":[]}", 19, &al);
    CHECK(again && !strcmp(again, "{\"a\":1,\"~type\":\"idSnapEntityPrefab\",\n\"snapmapPlus\":{\"format\":1,\"x\":[]}}"));
    n = again ? sh_prefab_block_strip(again, al) : 0;
    CHECK(again && n == sizeof WITH - 1 && !strcmp(again, WITH));
    free(put); free(again);
    /* The engine's own formatting, comments and a trailing NUL survive. */
    {
        static const char PRETTY[] = "{\n\t\"a\": [1, {\"snapmapPlus\": 2}],\n\t/* c */ \"~type\": \"x\"\n}\n";
        char *p2 = sh_prefab_block_put(PRETTY, sizeof PRETTY - 1, "{}", 2, &pl);
        CHECK(p2 && sh_prefab_block_find(p2, pl, &span) == 1 && span.value_len == 2);
        n = p2 ? sh_prefab_block_strip(p2, pl) : 0;
        CHECK(p2 && n == sizeof PRETTY - 1 && !strcmp(p2, PRETTY));
        free(p2);
        CHECK(sh_prefab_block_find(PRETTY, sizeof PRETTY, &span) == 0);   /* the NUL after it is not an error */
    }
    /* An empty object, a block as the first member, and text that is not an object. */
    put = sh_prefab_block_put("{}", 2, "{}", 2, &pl);
    CHECK(put && !strcmp(put, "{\"snapmapPlus\":{}}"));
    n = put ? sh_prefab_block_strip(put, pl) : 0;
    CHECK(put && !strcmp(put, "{}") && n == 2);
    free(put);
    {
        char first[] = "{\"snapmapPlus\":{\"format\":1}, \"a\":1}";
        n = sh_prefab_block_strip(first, sizeof first - 1);
        CHECK(!strcmp(first, "{\"a\":1}") && n == 7);
    }
    CHECK(sh_prefab_block_find("[1]", 3, &span) == -1 && sh_prefab_block_find("{\"a\":", 5, &span) == -1);
    CHECK(sh_prefab_block_put("not json", 8, "{}", 2, &pl) == NULL);
    /* A tail: the last block that closes the object. */
    {
        static const char TAIL[] = "1.5]},\"~type\":\"idSnapMap\",\n\"snapmapPlus\":{\"format\":1,\"d\":\"\\\"snapmapPlus\\\"\"}}\n";
        CHECK(sh_prefab_block_find_tail(TAIL, sizeof TAIL - 1, &span) == 1 && TAIL[span.value_at] == '{' &&
              span.value_at + span.value_len == sizeof TAIL - 3);
        CHECK(sh_prefab_block_find_tail("1.5]},\"~type\":\"idSnapMap\"}", 26, &span) == 0);
    }
    /* Block and view: the view is flat, the summary goes under "summary". */
    {
        static const char VIEW[] = "{\"desc\":\"Hall\",\"tags\":[\"a\",3],\"kind\":\"modules\",\"moduleCount\":2,"
                                   "\"mods\":[{\"mod\":\"xxyy0000@" GHOST "\",\"name\":\"x.y\",\"size\":1,\"names\":[]}]}";
        char *b = sh_prefab_mods_block_compose(VIEW, sizeof VIEW - 1, NULL, &pl), *v;
        CHECK(b && !strcmp(b, "{\"format\":1,\"description\":\"Hall\",\"tags\":[\"a\"],\"mods\":[{\"mod\":\"xxyy0000@" GHOST "\","
                              "\"name\":\"x.y\",\"size\":1,\"names\":[]}],\"summary\":{\"kind\":\"modules\",\"moduleCount\":2}}"));
        v = b ? sh_prefab_mods_block_view(b, pl, NULL) : NULL;
        CHECK(v && !strcmp(v, "{\"desc\":\"Hall\",\"tags\":[\"a\"],\"kind\":\"modules\",\"moduleCount\":2,\"mods\":[{\"mod\":\"xxyy0000@" GHOST "\","
                              "\"name\":\"x.y\",\"size\":1,\"names\":[]}]}"));
        free(b); free(v);
        b = sh_prefab_mods_block_compose(NULL, 0, NULL, NULL);
        CHECK(b && !strcmp(b, "{\"format\":1,\"description\":\"\",\"tags\":[],\"mods\":[]}"));
        v = b ? sh_prefab_mods_block_view(b, strlen(b), NULL) : NULL;
        CHECK(v && !strcmp(v, "{\"desc\":\"\",\"tags\":[]}"));
        free(b); free(v);
        CHECK(sh_prefab_mods_block_view("[]", 2, NULL) == NULL);
    }
}

static void comments(const char *lib)
{
    size_t n;
    printf("-- comments: the serializer's comments go, strings keep every byte\n");
    {
        char t[] = "{/*camera yaw*/\n   \"cameraYaw\" : 280.5,/*entities*/\n   \"entities\" : [\n      {/*custom icon*/\n"
                   "         \"x\" : 1\n      }\n   ]\n}";
        n = sh_prefab_block_strip_comments(t, sizeof t - 1);
        CHECK(!strcmp(t, "{\n   \"cameraYaw\" : 280.5,\n   \"entities\" : [\n      {\n         \"x\" : 1\n      }\n   ]\n}") &&
              n == strlen(t));
    }
    {
        /* Comment openers inside strings, an escaped quote, and an escaped
         * backslash right before a string's closing quote. */
        char t[] = "{\"a\":\"x /* not */ y // nor\",\"b\":\"q\\\"/*s*/\",\"c\":\"\\\\\"/*gone*/,\"d\":\"http://h/*p*/\"}";
        n = sh_prefab_block_strip_comments(t, sizeof t - 1);
        CHECK(!strcmp(t, "{\"a\":\"x /* not */ y // nor\",\"b\":\"q\\\"/*s*/\",\"c\":\"\\\\\",\"d\":\"http://h/*p*/\"}") &&
              n == strlen(t));
    }
    {
        /* Comments do not nest: one ends at the first star-slash after it. */
        char t[] = "{/* a /* b */\"k\":1/* c // d */,/* e */ /* f */\"m\":[/**/]}";
        n = sh_prefab_block_strip_comments(t, sizeof t - 1);
        CHECK(!strcmp(t, "{\"k\":1, \"m\":[]}") && n == strlen(t));
    }
    {
        char t[] = "{/* a /* b */ */\"k\":1}";
        n = sh_prefab_block_strip_comments(t, sizeof t - 1);
        CHECK(!strcmp(t, "{ */\"k\":1}"));
    }
    {
        /* A line comment ends before its newline; a block opener inside it is not one. */
        char t[] = "{// line /* not a block\r\n\"k\":1,// x\n\"m\":2/2}// end";
        n = sh_prefab_block_strip_comments(t, sizeof t - 1);
        CHECK(!strcmp(t, "{\n\"k\":1,\n\"m\":2/2}") && n == strlen(t));
    }
    {
        char t[] = "{\"k\":1}/* never closed";
        char u[] = "{\"k\":\"open string /* kept";
        char v[] = "{\"k\":1}";
        CHECK(sh_prefab_block_strip_comments(t, sizeof t - 1) == 7 && !strcmp(t, "{\"k\":1}"));
        CHECK(sh_prefab_block_strip_comments(u, sizeof u - 1) == sizeof u - 1 && !strcmp(u, "{\"k\":\"open string /* kept"));
        CHECK(sh_prefab_block_strip_comments(v, sizeof v - 1) == sizeof v - 1 && !strcmp(v, "{\"k\":1}"));
        CHECK(sh_prefab_block_strip_comments(NULL, 4) == 0 && sh_prefab_block_strip_comments(v, 0) == 0);
    }

    printf("-- Create's file is plain JSON: a commented serializer text is written without its comments\n");
    {
        static const char COMMENTED[] =
            "{/*camera yaw*/\"cameraYaw\":0,/*entities*/\"entities\":["
            "{/*in editor display name*/\"displayName\":\"Door\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/testdoor/door\",\"name\":\"\"}},"
            "{\"displayName\":\"Crate\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/props/new/crate01\",\"name\":\"\"}}],"
            "/*grab distance*/\"grabDistance\":256,\"~type\":\"idSnapEntityPrefab\"}";
        char p[MAX_PATH * 2], *text;
        size_t len = 0;
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\Commented.json", lib);
        CHECK(put_file(p, COMMENTED));
        CHECK(sh_prefab_mods_record(COMMENTED, sizeof COMMENTED - 1, p, NULL) == 1);
        text = slurp(p, &len);
        CHECK(text && !strstr(text, "/*") && strstr(block_of(text, len), "\"mods\":[{\"mod\":\"testdoor\""));
        CHECK(engine_bytes_are(p, OBJECT_PREFAB));
        free(text);
        DeleteFileA(p);
    }
}

/* A local row is written without a build: it follows its folder (D6a). */
#define LOCAL_ROW "\"local\":true"

static const char BLOCK_HEAD[] = "{\"format\":1,\"description\":\"\",\"tags\":[],\"mods\":[{\"mod\":\"testdoor\"";

static void recording(const char *lib, const sh_modmanifest_row *door)
{
    char prefab[MAX_PATH * 2], meta[MAX_PATH * 2], *view, *text;
    const char *blk;
    sh_modmanifest_list rows;
    size_t len = 0;
    int n, source;
    printf("-- Create writes the closure into the file's block; the engine's bytes stay as they were\n");
    n = sh_prefab_mods_closure(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows);
    CHECK(n == 1 && rows.count == 1 && !strcmp(rows.rows[0].build, door->build) && !strcmp(rows.rows[0].salt, "testdoor") && !strcmp(rows.rows[0].name, "test.door") && rows.rows[0].size == door->size);
    CHECK(rows.count == 1 && rows.rows[0].name_count == 1 && !strcmp(rows.rows[0].names[0], "mods/testdoor/door"));
    sh_modmanifest_list_free(&rows);
    CHECK(sh_prefab_mods_closure(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, &rows) == 0);
    sh_modmanifest_list_free(&rows);
    CHECK(sh_prefab_mods_closure("{\"entities\":[", 13, &rows) == -1);

    _snprintf_s(prefab, sizeof prefab, _TRUNCATE, "%s\\Door.json", lib);
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Door.meta.json", lib);
    CHECK(put_file(prefab, OBJECT_PREFAB));
    CHECK(sh_prefab_mods_record(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, prefab, &view) == 1);
    CHECK(view && strstr(view, "\"desc\":\"\"") && strstr(view, LOCAL_ROW) && !strstr(view, door->build));
    free(view);
    CHECK(engine_bytes_are(prefab, OBJECT_PREFAB) && !mt_exists(meta));
    text = slurp(prefab, &len);
    CHECK(text && len > sizeof OBJECT_PREFAB && !memcmp(text, OBJECT_PREFAB, sizeof OBJECT_PREFAB - 2));   /* engine keys first */
    blk = block_of(text, len);
    CHECK(!strncmp(blk, BLOCK_HEAD, sizeof BLOCK_HEAD - 1) && strstr(blk, LOCAL_ROW) && !strstr(blk, door->build) &&
          !strstr(blk, "summary"));
    CHECK(sh_prefab_mods_rows_of(text, len, NULL, &rows) == 1 && !strcmp(rows.rows[0].build, SH_MODMANIFEST_LOCAL_BUILD) &&
          rows.rows[0].name_count == 1);
    sh_modmanifest_list_free(&rows);
    /* The block's own strings are not placed names. */
    CHECK(sh_prefab_mods_closure(text, len, &rows) == 1);
    sh_modmanifest_list_free(&rows);
    free(text);

    printf("-- description and tag edits rewrite the block in place and keep the rows; no sidecar\n");
    {
        static const char BODY[] = "{\"desc\":\"Blast door\",\"tags\":[\"door\",\"x\"],\"kind\":\"ignored\",\"mods\":[]}";
        CHECK(sh_prefab_mods_meta_write(BODY, sizeof BODY - 1, prefab, meta) == 1);
    }
    text = slurp(prefab, &len);
    blk = block_of(text, len);
    CHECK(strstr(blk, "\"description\":\"Blast door\",\"tags\":[\"door\",\"x\"]") && strstr(blk, LOCAL_ROW) && !strstr(blk, "ignored"));
    CHECK(engine_bytes_are(prefab, OBJECT_PREFAB) && !mt_exists(meta));
    view = sh_prefab_mods_view_of(text, len, 0, meta, &source, NULL);
    CHECK(view && source == SH_PREFAB_META_BLOCK && strstr(view, "\"desc\":\"Blast door\"") && strstr(view, "\"mods\":["));
    free(view);
    free(text);
    /* Clearing both fields keeps the block and its rows. */
    CHECK(sh_prefab_mods_meta_write("", 0, prefab, meta) == 1);
    text = slurp(prefab, &len);
    blk = block_of(text, len);
    CHECK(strstr(blk, "\"description\":\"\",\"tags\":[]") && strstr(blk, LOCAL_ROW) && !mt_exists(meta));
    free(text);
    CHECK(sh_prefab_mods_meta_write("{\"desc\":\"An airlock\",\"tags\":[\"door\"]}", 37, prefab, meta) == 1);

    /* A prefab that needs no mods still gets its block, naming none. */
    {
        char p2[MAX_PATH * 2];
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\Crate.json", lib);
        CHECK(put_file(p2, STOCK_PREFAB));
        CHECK(sh_prefab_mods_record(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, p2, NULL) == 0);
        text = slurp(p2, &len);
        CHECK(!strcmp(block_of(text, len), "{\"format\":1,\"description\":\"\",\"tags\":[],\"mods\":[]}"));
        CHECK(sh_prefab_mods_rows_of(text, len, NULL, &rows) == 0);
        free(text);
        /* A text that is not JSON is left as the engine wrote it. */
        CHECK(put_file(p2, "{\"entities\":["));
        CHECK(sh_prefab_mods_record("{\"entities\":[", 13, p2, NULL) == -1);
        text = slurp(p2, &len);
        CHECK(text && !strcmp(text, "{\"entities\":["));
        free(text);
        DeleteFileA(p2);
    }

    printf("-- a module prefab's file carries its summary and rows in the block\n");
    {
        static const char MOD_WITH_DOOR[] =
            "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/testdoor/door\",\"name\":\"\"}}],"
            "\"instances\":[{\"moduleName\":\"maps/modules/ind/stock_room.decl\"}],\"~type\":\"idSnapMap\"}";
        int count = 0;
        size_t fl = 0;
        char *file = sh_prefab_mods_module_file(MOD_WITH_DOOR, sizeof MOD_WITH_DOOR - 1,
                                                "{\"desc\":\"\",\"tags\":[],\"kind\":\"modules\",\"moduleCount\":1}", &fl, &view, &count);
        CHECK(file && count == 1 && sh_mp_is_module_file(file, fl));
        blk = block_of(file, fl);
        CHECK(strstr(blk, "\"summary\":{\"kind\":\"modules\",\"moduleCount\":1}") && strstr(blk, LOCAL_ROW));
        CHECK(view && strstr(view, "\"kind\":\"modules\"") && strstr(view, "\"moduleCount\":1") && strstr(view, LOCAL_ROW) && !strstr(view, door->build));
        {
            size_t sl = 0;
            char *s = sh_prefab_mods_strip_copy(file, fl, &sl);
            CHECK(s && sl == sizeof MOD_WITH_DOOR - 1 && !memcmp(s, MOD_WITH_DOOR, sl));
            free(s);
        }
        free(file); free(view);
        file = sh_prefab_mods_module_file(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, "{}", &fl, NULL, &count);
        CHECK(file && count == 0 && strstr(file, "\"mods\":[]"));
        free(file);
    }

    printf("-- a prefab an earlier version saved (a sidecar and no block) is not read, and an edit writes nothing\n");
    {
        static const char SIDE[] = "{\"desc\":\"Old hall\",\"tags\":[\"old\"],\"kind\":\"modules\",\"moduleCount\":3,"
                                   "\"mods\":[{\"id\":\"test.door\",\"version\":\"1.0.0\",\"bundle\":\"%s\",\"size\":1,\"names\":[\"mods/testdoor/door\"]}]}";
        char p3[MAX_PATH * 2], m3[MAX_PATH * 2], side[600], *disk;
        size_t dl = 0;
        _snprintf_s(p3, sizeof p3, _TRUNCATE, "%s\\Old hall.json", lib);
        _snprintf_s(m3, sizeof m3, _TRUNCATE, "%s\\Old hall.meta.json", lib);
        _snprintf_s(side, sizeof side, _TRUNCATE, SIDE, door->build);
        CHECK(put_file(p3, OBJECT_PREFAB) && put_file(m3, side));
        text = slurp(p3, &len);
        view = sh_prefab_mods_view_of(text, len, 0, m3, &source, NULL);
        CHECK(!view && source == SH_PREFAB_META_EARLIER);
        CHECK(sh_prefab_mods_rows_of(text, len, m3, &rows) == SH_PREFAB_ROWS_EARLIER && rows.count == 0);
        view = sh_prefab_mods_view_of(text, len, 0, NULL, &source, NULL);
        CHECK(!view && source == SH_PREFAB_META_NONE);
        free(text);
        CHECK(sh_prefab_mods_meta_write("{\"desc\":\"New hall\",\"tags\":[\"old\"]}", 34, p3, m3) == -1);
        text = slurp(p3, &len);
        CHECK(text && !strcmp(text, OBJECT_PREFAB));   /* nothing written */
        free(text);
        disk = slurp(m3, &dl);
        CHECK(disk && !strcmp(disk, side));
        free(disk);
        /* A block whose rows name "id" and "bundle" is an earlier shape too. */
        {
            size_t wl = 0;
            char *with = sh_prefab_mods_embed(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, side, strlen(side), NULL, &wl);
            CHECK(with && sh_prefab_mods_rows_of(with, wl, NULL, &rows) == SH_PREFAB_ROWS_EARLIER);
            free(with);
        }
    }

    /* A view naming no mods has none; a malformed one is refused. */
    CHECK(sh_prefab_mods_view_rows("{\"desc\":\"x\",\"tags\":[]}", 22, &rows) == 0);
    CHECK(sh_prefab_mods_view_rows("not json", 8, &rows) == -1);
    CHECK(sh_prefab_mods_view_set("not json", 8, NULL, NULL) == NULL);
}

static void status(const sh_modmanifest_row *door)
{
    sh_modmanifest_list rows;
    sh_modmanifest_row ghost;
    size_t missing = 9;
    char *json, out[2048];
    sh_module_prefab_call call;
    printf("-- the status names each mod and whether it is installed\n");
    memset(&rows, 0, sizeof rows);
    memset(&ghost, 0, sizeof ghost);
    strcpy_s(ghost.salt, sizeof ghost.salt, "ghostmod");
        sh_modmanifest_row_set_name(&ghost, "ghost.mod");
    strcpy_s(ghost.build, sizeof ghost.build, GHOST);
    ghost.size = 1048576;
    CHECK(sh_modmanifest_list_add(&rows, door) && sh_modmanifest_list_add(&rows, &ghost));
    json = sh_prefab_mods_status_json(&rows, &missing);
    CHECK(json && missing == 1 && strstr(json, "\"count\":2,\"missing\":1") && strstr(json, "\"id\":\"test.door\"") &&
          strstr(json, "\"installed\":true") && strstr(json, "\"id\":\"ghost.mod\",\"salt\":\"ghostmod\""));
    free(json);
    json = sh_prefab_mods_view_set(NULL, 0, &rows, NULL);
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_MODS_STATUS;
    call.text = json;
    call.text_len = json ? strlen(json) : 0;
    CHECK(pm_slot(&call, out, sizeof out) == 1 && strstr(out, "\"missing\":1"));
    free(json);
    sh_modmanifest_list_free(&rows);
}

/* The player's mods folder: its store root, its serving index and the content cache under it. */
static void use_store(const char *root)
{
    CreateDirectoryA(root, NULL);
    CHECK(sh_ms_set_root(root));
    (void)sh_modserve_index_build(root);
    sh_cache_set_mods_root(root);
    sh_cache_local_changed();
}

/* What a mods folder holds besides its dot folders: an import installs nothing. */
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

/* A unit with no files, in the content cache of the current mods folder: a pinned row naming it is "here". */
static void present_unit(char hash[65])
{
    static const char MANIFEST[] =
        "{\"files\":[],\"format\":1,\"game\":[],\"mod\":{},\"root\":{\"kind\":\"entity\",\"name\":\"entities/present\"}}";
    sh_mb_sha256_hex(MANIFEST, sizeof MANIFEST - 1, hash);
    CHECK(sh_cache_put_manifest(hash, MANIFEST, sizeof MANIFEST - 1));
    CHECK(sh_cache_unit_state(hash, NULL, NULL, NULL) == 1);
}

/* A pinned row of the mod "presentm" naming `unit`. */
static void pinned_row(sh_modmanifest_list *rows, const char *salt, const char *name, const char *unit)
{
    sh_modmanifest_row row;
    const char *roots[1] = { "entities/present" }, *units[1];
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, salt);
    sh_modmanifest_row_set_name(&row, name);
    sh_modmanifest_row_add_name(&row, "mods/presentm/present");
    units[0] = unit;
    CHECK(sh_modmanifest_row_pin(&row, roots, units, 1));
    CHECK(sh_modmanifest_list_add(rows, &row));
    sh_modmanifest_row_free(&row);
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

static const char *const REFUSE_TAIL = " Ask its author for the prefab's .zip export.";

static void rows_of(sh_modmanifest_list *rows, const sh_modmanifest_row *door, int with_ghost)
{
    sh_modmanifest_row ghost;
    memset(rows, 0, sizeof *rows);
    if (door) sh_modmanifest_list_add(rows, door);
    if (with_ghost) {
        memset(&ghost, 0, sizeof ghost);
        strcpy_s(ghost.salt, sizeof ghost.salt, "ghostmod");
        sh_modmanifest_row_set_name(&ghost, "ghost.mod");
        strcpy_s(ghost.build, sizeof ghost.build, GHOST);
        ghost.size = 1048576;
        sh_modmanifest_row_add_name(&ghost, "maps/modules/mods/ghostmod/room.decl");
        sh_modmanifest_list_add(rows, &ghost);
        sh_modmanifest_row_free(&ghost);
    }
}

/* D1, D35 (manager, from R-UI1): a prefab of one Tyrant from a two-demon pack brings the pack into the map the way
 * placed objects do: no map-level row, so a publish pins the Tyrant's unit alone. Only a mod the prefab uses whole
 * is attached. */
static void one_demon_of_a_pack(const char *tmp, const char *store)
{
    static const char TYRANT_PREFAB[] =
        "{\"cameraYaw\":0,\"entities\":["
        "{\"displayName\":\"Tyrant\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/twodemon/tyrant\",\"name\":\"\"}}],"
        "\"grabDistance\":256,\"~type\":\"idSnapEntityPrefab\"}";
    char src[MAX_PATH], msg[600], *manifest;
    sh_modmanifest_row pack, row;
    sh_modmanifest_list rows;
    sh_modpin_row_result res;
    size_t len = 0, k;
    int seq = 0, r, whole = 0;
    printf("-- a prefab of one Tyrant from a two-demon pack: its placed object carries the pack, a publish pins the Tyrant alone\n");
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\twodemon-src", tmp);
    CreateDirectoryA(src, NULL);
    mt_write(src, "entities\\marauder.decl", "{\r\n\tinherit = \"snapmaps/doors/industrial_door_vertical\";\r\n}");
    CHECK(install_mod(src, "twodemon", "Two Demons", "entities\\tyrant.decl", DOOR_DECL, &pack));
    CHECK(sh_modserve_index_build(store) >= 2);

    /* The prefab's row places the Tyrant: activated, never attached. */
    reset_fakes();
    row = pack;
    row.name = NULL;
    memset(&rows, 0, sizeof rows);
    CHECK(sh_modmanifest_row_set_name(&row, "Two Demons") && sh_modmanifest_row_add_name(&row, "mods/twodemon/tyrant"));
    CHECK(sh_modmanifest_list_add(&rows, &row));
    r = HO(msg, sh_prefab_mods_resolve_main(TYRANT_PREFAB, sizeof TYRANT_PREFAB - 1, &rows, "Tyrant", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_PLACE && F.activations == 1 && F.attaches == 0);
    sh_modmanifest_list_free(&rows);

    /* The save writes that row from what is placed; a publish pins it: the Tyrant's unit, no whole-mod unit, and no
     * file of the Marauder. */
    memset(&res, 0, sizeof res);
    CHECK(sh_modpin_row(&row, NULL, 0, NULL, &res) == SH_MODPIN_PINNED && row.pinned && row.unit_count == 1);
    for (k = 0; k < row.unit_count; k++) if (!strcmp(row.roots[k], "*")) whole = 1;
    CHECK(!whole && row.unit_count && !strcmp(row.roots[0], "entities/tyrant"));
    manifest = row.unit_count ? sh_cache_manifest(row.units[0], &len) : NULL;
    CHECK(manifest && strstr(manifest, "tyrant") && !strstr(manifest, "marauder"));
    free(manifest);

    free(res.why);
    sh_modmanifest_row_free(&row);

    /* The same mod used by the prefab as a whole (its map-level row): attached, and pinned whole. */
    reset_fakes();
    row = pack;
    row.name = NULL;
    row.attached = 1;
    memset(&rows, 0, sizeof rows);
    CHECK(sh_modmanifest_row_set_name(&row, "Two Demons"));
    CHECK(sh_modmanifest_list_add(&rows, &row));
    r = HO(msg, sh_prefab_mods_resolve_main(TYRANT_PREFAB, sizeof TYRANT_PREFAB - 1, &rows, "Tyrant", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_PLACE && F.attaches == 1);
    sh_modmanifest_list_free(&rows);
    memset(&res, 0, sizeof res);
    CHECK(sh_modpin_row(&row, NULL, 0, NULL, &res) == SH_MODPIN_PINNED && row.pinned);
    for (whole = 0, k = 0; k < row.unit_count; k++) if (!strcmp(row.roots[k], "*")) whole = 1;
    CHECK(whole);
    free(res.why);
    sh_modmanifest_row_free(&row);
    sh_modmanifest_row_free(&pack);
}

/* Manager Q8, D6a, end to end in one process: a map whose local row names a mod this computer does not have loads
 * with its objects stood in from the real content cache and serving index, the row is served, the load's one sentence
 * names the placed entity only (not a name a value spells), and the save writes the row back as one local row with
 * every name. */
static void absent_local_row_stands_in(void)
{
    static const char MAP_ABSENT[] =
        "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/absnt001/crate\","
        "\"name\":\"\",\"state\":{\"edit\":{\"skins\":[\"mods/absnt001/skin\"]}}},\"uniqueId\":0}],"
        "\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"absnt001\",\"name\":\"Absent Pack\",\"size\":10,"
        "\"local\":true,\"names\":[\"mods/absnt001/crate\",\"mods/absnt001/skin\"]}]}}";
    sh_modmanifest_list cur, back;
    sh_modserve_bundle_ref ref;
    char *t, *cause = NULL, *saved;
    size_t n = 0, k;
    int crate = 0, skin = 0;
    printf("-- a map whose local row's mod is not here loads with stand-ins, says one sentence, and saves its row (Q8)\n");
    (void)sh_cache_take_notice();
    sh_modmanifest_set_content(sh_cache_on_load);
    sh_cache_set_rescan(sh_modserve_index_rescan);
    (void)sh_modmanifest_prepare_load(MAP_ABSENT, sizeof MAP_ABSENT - 1, NULL);
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && cur.count == 1 && !cur.rows[0].pinned);
    memset(&ref, 0, sizeof ref);
    CHECK(cur.count && sh_modserve_row_build(&cur.rows[0], &ref) && ref.place && !strcmp(ref.place, SH_MODSERVE_CONTENT));
    sh_modserve_bundle_ref_free(&ref);
    CHECK(cur.count && sh_modserve_row_served_for(&cur.rows[0], NULL, &cause) == SH_MODSERVE_ROW_SERVED);
    free(cause);
    sh_modmanifest_list_free(&cur);
    t = sh_cache_take_notice();
    CHECK(t && !strcmp(t, "This map is playing without the Crate from Absent Pack, which is not on this computer."));
    free(t);
    saved = sh_modmanifest_on_save(MAP_ABSENT, sizeof MAP_ABSENT - 1, &n);
    memset(&back, 0, sizeof back);
    CHECK(saved && sh_modmanifest_read(saved, n, &back, NULL) == 1 && back.count == 1 && !back.rows[0].pinned &&
          !strcmp(back.rows[0].salt, "absnt001"));
    for (k = 0; back.count && k < back.rows[0].name_count; k++) {
        if (!strcmp(back.rows[0].names[k], "mods/absnt001/crate")) crate = 1;
        if (!strcmp(back.rows[0].names[k], "mods/absnt001/skin")) skin = 1;
    }
    CHECK(crate && skin);
    CHECK(saved && strstr(saved, "\"local\":true") && !strstr(saved, "\"objects\""));
    sh_modmanifest_list_free(&back);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    sh_modmanifest_set_content(NULL);
    sh_cache_set_rescan(NULL);
    sh_modmanifest_reset();
}

/* The publish's pin step as modpublish.c binds it (sh_modpublish_pin_save): every local row of the save pinned from
 * its folder's current build through the real unit walk. */
static int g_publish_pins;
static int publish_pin(sh_modmanifest_list *rows, const char *json, size_t length)
{
    size_t i;
    g_publish_pins++;
    for (i = 0; i < rows->count; i++) {
        sh_modpin_row_result res;
        int st;
        if (rows->rows[i].pinned) continue;
        memset(&res, 0, sizeof res);
        st = sh_modpin_row(&rows->rows[i], json, length, NULL, &res);
        free(res.why);
        if (st != SH_MODPIN_PINNED && st != SH_MODPIN_WHOLE) return 0;
    }
    return 1;
}

/* The map scope as modui.c binds it (mu_scope_hook, mu_listing_set): a load's or a save's rows set the listing the
 * serving index serves the open map from. */
static void t_scope(const sh_modmanifest_list *rows, const char *const *names, size_t name_count, int when)
{
    char (*hashes)[SH_MODMANIFEST_HASH_CAP] = NULL;
    const char *list[16];
    int n = sh_modmanifest_listing(rows, names, name_count, &hashes), i;
    (void)when;
    for (i = 0; i < n && i < 16; i++) list[i] = hashes[i];
    (void)sh_modserve_scope_set(list, n < 0 ? 0 : n > 16 ? 16 : n, 1);
    free(hashes);
}
static int t_scope_armed(void) { return 1; }

/* The installed folder of `salt`'s mod, or "" (absolute). */
static void folder_of(const char *salt, char *out, size_t cap)
{
    sh_ms_list l;
    size_t i;
    out[0] = 0;
    if (!sh_ms_enumerate(&l)) return;
    for (i = 0; i < l.count; i++)
        if (!_stricmp(l.items[i].salt, salt) && l.items[i].folder) strcpy_s(out, cap, l.items[i].folder);
    sh_ms_list_free(&l);
}

/* R-LB1 F6, owner ruling 2026-11-18 Q1 (a published map is pinned to its units): with the real unit walk, serving index
 * and content cache, a row pinned from a folder compares equal until a file of its object changes and the folder is
 * built again, and then differs, with no reload. A publish's save in the session makes the open map take the pinned
 * rows it wrote, still served, so that comparison (Update) follows the rebuild in the same session; a later plain save
 * keeps them pinned. */
static void published_map_follows_rebuild(const char *tmp, const char *store)
{
    static const char MAP_PUB[] =
        "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/pubtyr01/tyrant\","
        "\"name\":\"\",\"state\":{}},\"uniqueId\":0}]}";
    static const char TYRANT_V2[] = "{\r\n\tinherit = \"snapmaps/doors/industrial_door_horizontal\";\r\n}";
    char src[MAX_PATH], folder[MAX_PATH * 2], *saved, *loaded, *cause = NULL, err[192];
    sh_modmanifest_row mod, row;
    sh_modmanifest_list rows, cur, back;
    sh_modpin_row_result res;
    sh_modserve_bundle_ref ref;
    size_t n = 0, ln = 0;
    char *text;
    int scoped;
    printf("-- a row pinned from a folder differs once the folder is built again; a publish in the session pins the open map (F6)\n");
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\pubtyr-src", tmp);
    CreateDirectoryA(src, NULL);
    CHECK(install_mod(src, "pubtyr01", "Pub Tyrant", "entities\\tyrant.decl", DOOR_DECL, &mod));
    CHECK(sh_modserve_index_build(store) >= 1);
    folder_of("pubtyr01", folder, sizeof folder);
    CHECK(folder[0] != 0);
    sh_modmanifest_set_content(sh_cache_on_load);
    sh_cache_set_rescan(sh_modserve_index_rescan);
    scoped = sh_modserve_scope_enabled();
    sh_modserve_scope_enable(1);
    sh_modmanifest_set_scope(t_scope, t_scope_armed);

    /* The author's map, unpublished: a local row of the installed build, served by the folder. */
    memset(&rows, 0, sizeof rows);
    row = mod;
    row.name = NULL;
    CHECK(sh_modmanifest_row_set_name(&row, "Pub Tyrant") && sh_modmanifest_row_add_name(&row, "mods/pubtyr01/tyrant"));
    CHECK(sh_modmanifest_list_add(&rows, &row));
    sh_modmanifest_row_free(&row);
    text = sh_modmanifest_write(MAP_PUB, sizeof MAP_PUB - 1, &rows, &ln, err, sizeof err);
    sh_modmanifest_list_free(&rows);
    CHECK(text != NULL);
    if (!text) goto done;
    loaded = sh_modmanifest_prepare_load(text, ln, NULL);
    if (loaded) HeapFree(GetProcessHeap(), 0, loaded);
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && !cur.rows[0].pinned);
    sh_modmanifest_list_free(&cur);

    /* The real comparison: pinned from the folder, it is equal. */
    memset(&row, 0, sizeof row);
    row = mod;
    row.name = NULL;
    CHECK(sh_modmanifest_row_add_name(&row, "mods/pubtyr01/tyrant"));
    memset(&res, 0, sizeof res);
    CHECK(sh_modpin_row(&row, NULL, 0, NULL, &res) == SH_MODPIN_PINNED && row.pinned);
    free(res.why);
    CHECK(sh_modpin_row_differs(&row) == 0);

    /* The publish's save: the pinned rows are written and the open map takes them, served by their composition. */
    g_publish_pins = 0;
    sh_modmanifest_set_pin(publish_pin);
    saved = sh_modmanifest_on_save(text, ln, &n);
    sh_modmanifest_set_pin(NULL);
    CHECK(saved && g_publish_pins == 1);
    memset(&back, 0, sizeof back);
    CHECK(saved && sh_modmanifest_read(saved, n, &back, NULL) == 1 && back.rows[0].pinned);
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && cur.rows[0].pinned);
    CHECK(cur.count && back.count && !strcmp(cur.rows[0].build, back.rows[0].build));
    memset(&ref, 0, sizeof ref);
    CHECK(cur.count && sh_modserve_row_build(&cur.rows[0], &ref) && ref.place && !strcmp(ref.place, SH_MODSERVE_CONTENT));
    sh_modserve_bundle_ref_free(&ref);
    CHECK(cur.count && sh_modserve_row_served_for(&cur.rows[0], NULL, &cause) == SH_MODSERVE_ROW_SERVED);
    free(cause);
    cause = NULL;
    CHECK(cur.count && sh_modpin_row_differs(&cur.rows[0]) == 0);
    sh_modmanifest_list_free(&back);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);

    /* The author changes the Tyrant in the folder and presses Refresh: the folder is built again. */
    mt_write(folder, "entities\\tyrant.decl", TYRANT_V2);
    CHECK(sh_ms_sync(NULL, NULL));
    CHECK(sh_modserve_index_rescan() >= 1);
    CHECK(sh_modpin_row_differs(&row) == 1);                         /* the real comparison, no reload */
    CHECK(cur.count && sh_modpin_row_differs(&cur.rows[0]) == 1);    /* the open map's row: Update is offered */
    sh_modmanifest_list_free(&cur);
    memset(&cur, 0, sizeof cur);
    CHECK(sh_modmanifest_current(&cur) == 1 && cur.rows[0].pinned &&
          sh_modserve_row_served_for(&cur.rows[0], NULL, &cause) == SH_MODSERVE_ROW_SERVED);   /* still served */
    free(cause);
    cause = NULL;

    /* A later plain save keeps the pinned row as it is: no pin step runs, the build does not move. */
    saved = sh_modmanifest_on_save(text, ln, &n);
    memset(&back, 0, sizeof back);
    CHECK(saved && sh_modmanifest_read(saved, n, &back, NULL) == 1 && back.rows[0].pinned && cur.count &&
          !strcmp(back.rows[0].build, cur.rows[0].build) && g_publish_pins == 1);
    sh_modmanifest_list_free(&back);
    sh_modmanifest_list_free(&cur);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    HeapFree(GetProcessHeap(), 0, text);
done:
    sh_modmanifest_row_free(&row);
    sh_modmanifest_row_free(&mod);
    sh_modmanifest_set_content(NULL);
    sh_cache_set_rescan(NULL);
    sh_modmanifest_set_scope(NULL, NULL);
    (void)sh_modserve_scope_set(NULL, 0, 0);
    sh_modserve_scope_enable(scoped);
    sh_modmanifest_reset();
    sh_cache_local_changed();   /* the local folders' snapshot is read again, as the next map load reads it */
}

/* Add to my mods end to end: a played map's row staged from the content cache, made a mod of the player's own with a
 * new salt by the store, built by the scan, and served from the root. */
static void add_to_my_mods_end_to_end(const char *tmp, const char *store)
{
    static const char decl[] = "{\r\n\tinherit = \"snapmaps/unknown\";\r\n}";
    char sha[65], man[600], mh[65], src[MAX_PATH], played[MAX_PATH], saved[MAX_PATH], path[MAX_PATH], err[128];
    char *mtext, *made = NULL, *cause = NULL, *why = NULL;
    const char *roots[1], *units[1];
    sh_modmanifest_row pr;
    sh_modmanifest_list one;
    sh_cache_copy copy;
    sh_ms_list l;
    size_t mlen = 0, i;
    int ml, found = 0;
    FILE *f = NULL;
    printf("-- Add to my mods: a map's row becomes a mod of your own with a new salt, built and served (5.15)\n");
    sh_mb_sha256_hex(decl, sizeof decl - 1, sha);
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\copy-src.decl", tmp);
    CHECK(spit(src, decl, sizeof decl - 1));
    CHECK(sh_cache_put_blob(src, sha, sizeof decl - 1));
    ml = _snprintf_s(man, sizeof man, _TRUNCATE, "{\"files\":[{\"path\":\"entities/copied.decl\",\"sha256\":\"%s\","
                     "\"size\":%u}],\"format\":1,\"game\":[],\"mod\":{},\"root\":{\"kind\":\"entity\","
                     "\"name\":\"entities/copied\"}}", sha, (unsigned)(sizeof decl - 1));
    sh_mb_sha256_hex(man, (size_t)ml, mh);
    CHECK(sh_cache_put_manifest(mh, man, (size_t)ml));
    memset(&pr, 0, sizeof pr);
    strcpy_s(pr.salt, sizeof pr.salt, "cpysrc01");
    roots[0] = "entities/copied"; units[0] = mh;
    CHECK(sh_modmanifest_row_pin(&pr, roots, units, 1) && sh_modmanifest_row_set_name(&pr, "Copied Pack"));
    memset(&one, 0, sizeof one);
    CHECK(sh_modmanifest_list_add(&one, &pr));
    mtext = sh_modmanifest_write("{\"entities\":[]}", 15, &one, &mlen, err, sizeof err);
    _snprintf_s(played, sizeof played, _TRUNCATE, "%s\\copy-played", tmp);
    _snprintf_s(saved, sizeof saved, _TRUNCATE, "%s\\copy-saved", tmp);
    CreateDirectoryA(played, NULL);
    CreateDirectoryA(saved, NULL);
    sh_cache_set_map_folders(saved, played);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\cpy1_cpy2_map.decl", played);
    if (mtext && !fopen_s(&f, path, "wb") && f) { fwrite(mtext, 1, mlen, f); fclose(f); }
    free(mtext);
    CHECK(sh_cache_stage_copy(pr.build, &copy, &why) == 1 && !why);
    CHECK(sh_ms_add_copy(copy.dir, copy.modjson, copy.modjson_len, copy.salt, copy.name, &made, &cause) == 1);
    CHECK(made && !strcmp(made, "Copied Pack (my copy)"));
    sh_cache_copy_free(&copy);
    CHECK(sh_ms_sync(NULL, NULL) >= 0);
    CHECK(sh_ms_enumerate(&l));
    for (i = 0; i < l.count; i++) {
        const sh_ms_entry *e = &l.items[i];
        if (made && !strcmp(e->name, made)) {
            found = 1;
            CHECK(sh_ms_entry_usable(e) && e->salt[0] && strcmp(e->salt, "cpysrc01"));
            CHECK(e->bundle.name && !strcmp(e->bundle.name, "Copied Pack (my copy)"));
        }
    }
    CHECK(found);
    sh_ms_list_free(&l);
    CHECK(sh_modserve_index_build(store) >= 1);
    free(made);
    free(cause);
    free(why);
    sh_modmanifest_list_free(&one);
    sh_modmanifest_row_free(&pr);
    sh_cache_set_map_folders(NULL, NULL);
}

/* D29 as built (manager): Add to my mods of one Tyrant from a two-demon pack whose model binary spells the pack's
 * namespace. The copy gets a new salt, its texts follow, its binary is kept as written and its build records the old
 * namespace as "spelled", and the serving index serves the binary with the copy's namespace (D12). */
static void copy_a_demon_whose_binary_spells(const char *tmp, const char *store)
{
    static const char tyrant[] =
        "{\r\n\tinherit = \"snapmaps/props/crate\";\r\n\tedit = {\r\n\t\trenderModelInfo = {\r\n"
        "\t\t\tmodel = \"mods/spdemon1/models/tyrant.lwo\";\r\n\t\t}\r\n\t}\r\n}\r\n";
    static const char mesh[] = "BMODEL\0\0\0material mods/spdemon1/tyrant_skin\0\0\0\0";
    char src[MAX_PATH], path[MAX_PATH], played[MAX_PATH], saved[MAX_PATH], err[128], *mtext, *made = NULL, *cause = NULL;
    char *why = NULL, req[400] = "";
    sh_modmanifest_row pack, row;
    sh_modmanifest_list one;
    sh_modpin_row_result res;
    sh_cache_copy copy;
    sh_ms_list l;
    size_t mlen = 0, i, k, blen = 0;
    unsigned char *bytes = NULL;
    int found = 0, newsp = 0, oldsp = 0;
    FILE *f = NULL;
    char newsalt[16] = "";
    printf("-- Add to my mods of a Tyrant whose model spells its pack's namespace: copied, served with the copy's (D29, D12)\n");
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\spdemon-src", tmp);
    CreateDirectoryA(src, NULL);
    mt_write(src, "entities\\marauder.decl", "{\r\n\tinherit = \"snapmaps/doors/industrial_door_vertical\";\r\n}");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\meshes\\models\\tyrant.bmodel", src);
    mt_mkdirs_for(path);
    CHECK(spit(path, mesh, sizeof mesh - 1));
    CHECK(install_mod(src, "spdemon1", "Spelling Demons", "entities\\tyrant.decl", tyrant, &pack));
    CHECK(sh_modserve_index_build(store) >= 2);
    /* The map's row: the Tyrant pinned to its unit, which carries the model and spells the namespace. */
    row = pack;
    row.name = NULL;
    CHECK(sh_modmanifest_row_set_name(&row, "Spelling Demons") && sh_modmanifest_row_add_name(&row, "mods/spdemon1/tyrant"));
    memset(&res, 0, sizeof res);
    CHECK(sh_modpin_row(&row, NULL, 0, NULL, &res) == SH_MODPIN_PINNED && row.unit_count == 1);
    free(res.why);
    {
        size_t ml = 0;
        char *m = row.unit_count ? sh_cache_manifest(row.units[0], &ml) : NULL;
        CHECK(m && strstr(m, "tyrant.bmodel") && strstr(m, "\"spelled\":\"spdemon1\"") && !strstr(m, "marauder"));
        free(m);
    }
    memset(&one, 0, sizeof one);
    CHECK(sh_modmanifest_list_add(&one, &row));
    mtext = sh_modmanifest_write("{\"entities\":[]}", 15, &one, &mlen, err, sizeof err);
    _snprintf_s(played, sizeof played, _TRUNCATE, "%s\\spell-played", tmp);
    _snprintf_s(saved, sizeof saved, _TRUNCATE, "%s\\spell-saved", tmp);
    CreateDirectoryA(played, NULL);
    CreateDirectoryA(saved, NULL);
    sh_cache_set_map_folders(saved, played);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\spl1_spl2_map.decl", played);
    if (mtext && !fopen_s(&f, path, "wb") && f) { fwrite(mtext, 1, mlen, f); fclose(f); }
    free(mtext);
    /* Staged, copied with a new salt, built by the scan. */
    CHECK(sh_cache_stage_copy(row.build, &copy, &why) == 1 && !why);
    if (why) fprintf(stderr, "stage copy: %s\n", why);
    CHECK(sh_ms_add_copy(copy.dir, copy.modjson, copy.modjson_len, copy.salt, copy.name, &made, &cause) == 1);
    if (cause) fprintf(stderr, "add copy: %s\n", cause);
    sh_cache_copy_free(&copy);
    CHECK(sh_ms_sync(NULL, NULL) >= 0);
    CHECK(sh_ms_enumerate(&l));
    for (i = 0; i < l.count; i++) {
        const sh_ms_entry *e = &l.items[i];
        if (!made || strcmp(e->name, made)) continue;
        found = 1;
        CHECK(sh_ms_entry_usable(e) && e->salt[0] && strcmp(e->salt, "spdemon1") && !strcmp(e->bundle.spelled, "spdemon1"));
        strcpy_s(newsalt, sizeof newsalt, e->salt);
        for (k = 0; k < e->bundle.file_count; k++)
            if (e->bundle.files[k].request && strstr(e->bundle.files[k].request, "tyrant.bmodel"))
                strcpy_s(req, sizeof req, e->bundle.files[k].request);
    }
    CHECK(found && req[0] && strstr(req, newsalt));
    sh_ms_list_free(&l);
    /* Served with the copy's namespace, length for length. */
    CHECK(sh_modserve_index_build(store) >= 3);
    sh_modserve_scope_enable(0);
    CHECK(req[0] && sh_modserve_open(req, &bytes, &blen) == 1 && blen == sizeof mesh - 1);
    for (i = 0; bytes && newsalt[0] && i + 14 <= blen; i++) {
        if (!memcmp(bytes + i, "mods/", 5) && !memcmp(bytes + i + 5, newsalt, 8)) newsp = 1;
        if (!memcmp(bytes + i, "mods/spdemon1/", 14)) oldsp = 1;
    }
    CHECK(newsp && !oldsp);
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    free(made);
    free(cause);
    sh_modmanifest_list_free(&one);
    sh_modmanifest_row_free(&row);
    sh_modmanifest_row_free(&pack);
    sh_cache_set_map_folders(NULL, NULL);
}

static void resolving(const sh_modmanifest_row *door)
{
    sh_modmanifest_list rows;
    char msg[600], unit[65], want[600];
    int seq = 0, r;

    printf("-- every mod installed: activated, attached to the open map once, then placed\n");
    reset_fakes();
    rows_of(&rows, door, 0);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_PLACE && seq == 0 && F.activations == 1 && F.attaches == 1 && !strcmp(F.attached[0], door->build));
    CHECK(F.prints == 0 && F.pinned_calls == 0);   /* the Mods tab shows the new row: no console note */
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_PLACE && F.attaches == 1 && F.prints == 0);   /* the map lists it now */
    sh_modmanifest_list_free(&rows);
    /* No rows: placed as before, nothing asked. */
    memset(&rows, 0, sizeof rows);
    CHECK(HO(msg, sh_prefab_mods_resolve_main(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, &rows, "Crate", &seq, HO_P(0))) ==
          SH_PREFAB_MODS_PLACE && F.activations == 2);

    printf("-- nothing is held or fetched: a placement never waits, and the poll always says 0\n");
    CHECK(sh_prefab_mods_poll(0) == 0 && sh_prefab_mods_poll(1) == 0 && sh_prefab_mods_poll(101) == 0);

    printf("-- a local row nothing serves: refused at once with the prefab's sentence, printed and toasted\n");
    reset_fakes();
    rows_of(&rows, door, 1);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    _snprintf_s(want, sizeof want, _TRUNCATE, "This prefab was not placed: it needs ghost.mod, which is not on this computer.%s",
                REFUSE_TAIL);
    CHECK(r == SH_PREFAB_MODS_REFUSED && seq == 0 && !strcmp(msg, want));
    CHECK(F.prints == 1 && !strcmp(F.last_print, want) && F.toasts == 1 && !strcmp(F.last_toast, want));
    CHECK(F.attaches == 0 && F.activations == 0 && F.pinned_calls == 0);   /* nothing reaches the map */
    CHECK(sh_prefab_mods_poll(seq) == 0);
    sh_modmanifest_list_free(&rows);

    printf("-- a prefab recording only the unserved row: the same refusal\n");
    reset_fakes();
    rows_of(&rows, NULL, 1);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_REFUSED && !strcmp(msg, want) && F.attaches == 0 && F.toasts == 1);
    sh_modmanifest_list_free(&rows);

    printf("-- two rows missing: both are named, \"are\" for many\n");
    reset_fakes();
    rows_of(&rows, door, 1);
    {
        sh_modmanifest_row phantom;
        memset(&phantom, 0, sizeof phantom);
        strcpy_s(phantom.salt, sizeof phantom.salt, "phantom1");
        strcpy_s(phantom.build, sizeof phantom.build, "7777777777777777777777777777777777777777777777777777777777777777");
        sh_modmanifest_row_set_name(&phantom, "phantom.mod");
        sh_modmanifest_list_add(&rows, &phantom);
        sh_modmanifest_row_free(&phantom);
    }
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    _snprintf_s(want, sizeof want, _TRUNCATE,
                "This prefab was not placed: it needs ghost.mod, phantom.mod, which are not on this computer.%s", REFUSE_TAIL);
    CHECK(r == SH_PREFAB_MODS_REFUSED && !strcmp(msg, want) && !strcmp(F.last_print, want) && !strcmp(F.last_toast, want));
    CHECK(F.attaches == 0);   /* the served door is not attached either: nothing of a refused prefab reaches the map */
    sh_modmanifest_list_free(&rows);

    printf("-- a pinned row whose units are not all in the cache: the same refusal, nothing fetched\n");
    reset_fakes();
    memset(&rows, 0, sizeof rows);
    pinned_row(&rows, "ghostmod", "ghost.mod", HASH_UNIT);   /* no manifest of it here */
    CHECK(sh_cache_unit_state(HASH_UNIT, NULL, NULL, NULL) == -1);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    _snprintf_s(want, sizeof want, _TRUNCATE, "This prefab was not placed: it needs ghost.mod, which is not on this computer.%s",
                REFUSE_TAIL);
    CHECK(r == SH_PREFAB_MODS_REFUSED && !strcmp(msg, want) && !strcmp(F.last_print, want) && !strcmp(F.last_toast, want));
    CHECK(F.pinned_calls == 0 && F.attaches == 0 && sh_prefab_mods_poll(seq) == 0);
    sh_modmanifest_list_free(&rows);

    printf("-- pinned rows whose units are all here go to the open map as pinned rows\n");
    reset_fakes();
    present_unit(unit);
    memset(&rows, 0, sizeof rows);
    pinned_row(&rows, "presentm", "present.mod", unit);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_PLACE && seq == 0 && msg[0] == 0 && F.pinned_calls == 1 && F.pinned_rows == 1);
    CHECK(F.attaches == 0 && F.activations == 0 && F.prints == 0 && F.toasts == 0);   /* the pinned path is not the local one */
    sh_modmanifest_list_free(&rows);

    printf("-- a local row and a pinned row together: the pinned one is used, the local one activated and attached\n");
    reset_fakes();
    rows_of(&rows, door, 0);
    pinned_row(&rows, "presentm", "present.mod", unit);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_PLACE && F.pinned_calls == 1 && F.pinned_rows == 1);
    CHECK(F.activations == 1 && F.attaches == 1 && !strcmp(F.attached[0], door->build));
    /* a local row nothing serves refuses the whole placement before the pinned one is used */
    reset_fakes();
    sh_modmanifest_list_free(&rows);
    rows_of(&rows, NULL, 1);
    pinned_row(&rows, "presentm", "present.mod", unit);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_REFUSED && F.pinned_calls == 0 && strstr(msg, "it needs ghost.mod, which is not on this computer."));
    sh_modmanifest_list_free(&rows);

    printf("-- the open map refuses the pinned rows: the cause, in the placement's sentence\n");
    reset_fakes();
    F.pinned_answer = -1;
    strcpy_s(F.pinned_cause, sizeof F.pinned_cause, "present.mod is already in this map at another version");
    rows_of(&rows, door, 0);
    pinned_row(&rows, "presentm", "present.mod", unit);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_REFUSED && !strcmp(msg, "Prefab Door was not placed: present.mod is already in this map at another "
                                                      "version."));
    CHECK(!strcmp(F.last_print, msg) && F.attaches == 0 && F.activations == 0);   /* the local row did not reach the map */
    sh_modmanifest_list_free(&rows);
    /* no op bound for the pinned rows */
    reset_fakes();
    sh_prefab_mods_bind(&PM_OPS_NO_PINNED);
    memset(&rows, 0, sizeof rows);
    pinned_row(&rows, "presentm", "present.mod", unit);
    r = HO(msg, sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, HO_P(0)));
    CHECK(r == SH_PREFAB_MODS_REFUSED && !strcmp(msg, "Prefab Door was not placed: its mod content cannot be used in this map."));
    sh_modmanifest_list_free(&rows);

    printf("-- a module prefab: DLC no row provides is refused first; a module a row provides is a mod\n");
    reset_fakes();
    rows_of(&rows, NULL, 1);
    {
        static const char DLC[] = MODULE_PREFAB("maps/modules/ind_dlc/dlc_room.decl");
        static const char MODDED[] = MODULE_PREFAB("maps/modules/mods/ghostmod/room.decl");
        static const char STOCK[] = MODULE_PREFAB("maps/modules/ind/stock_room.decl");
        r = HO(msg, sh_prefab_mods_resolve_main(DLC, sizeof DLC - 1, &rows, "Rooms", &seq, HO_P(0)));
        CHECK(r == SH_PREFAB_MODS_NEEDS_MODULE && F.toasts == 0);
        CHECK(!strcmp(msg, "Not placed: this prefab needs module ind_dlc/dlc_room, which is not in your palette "
                           "(DLC or a mod you do not have). Install the DLC or the mod that adds it."));
        CHECK(!strcmp(F.last_print, "Module prefab: needs module maps/modules/ind_dlc/dlc_room.decl; this room is not in "
                                    "your palette (DLC or a mod you do not have)"));
        {
            /* The same with the product's block in the file: its strings are not modules. */
            static const char BLK[] = "{\"format\":1,\"summary\":{\"modules\":[{\"name\":\"maps/modules/x.decl\"}]}}";
            size_t bl = 0;
            char *with = sh_prefab_block_put(DLC, sizeof DLC - 1, BLK, sizeof BLK - 1, &bl);
            CHECK(with && HO(msg, sh_prefab_mods_resolve_main(with, bl, &rows, "Rooms", &seq, HO_P(0))) == SH_PREFAB_MODS_NEEDS_MODULE &&
                  strstr(msg, "ind_dlc/dlc_room") && !strstr(msg, "maps/modules/x"));
            free(with);
        }
        /* The module the ghost row provides is a mod, not a missing room: the row is what is missing now. */
        r = HO(msg, sh_prefab_mods_resolve_main(MODDED, sizeof MODDED - 1, &rows, "Rooms", &seq, HO_P(0)));
        _snprintf_s(want, sizeof want, _TRUNCATE, "This prefab was not placed: it needs ghost.mod, which is not on this computer.%s",
                    REFUSE_TAIL);
        CHECK(r == SH_PREFAB_MODS_REFUSED && !strcmp(msg, want) && seq == 0);
        /* Nothing waits: the next placement is not held up by it. */
        sh_modmanifest_list_free(&rows);
        memset(&rows, 0, sizeof rows);
        sh_modmanifest_list_add(&rows, door);
        r = HO(msg, sh_prefab_mods_resolve_main(STOCK, sizeof STOCK - 1, &rows, "Other", &seq, HO_P(0)));
        CHECK(r == SH_PREFAB_MODS_PLACE && seq == 0 && msg[0] == 0);
    }
    sh_modmanifest_list_free(&rows);
    reset_fakes();
}

static void resolve_through_slot(const char *lib, const sh_modmanifest_row *door)
{
    sh_module_prefab_call call;
    char out[2048], meta[MAX_PATH * 2], none[MAX_PATH * 2], path[MAX_PATH * 2];
    char *text;
    size_t len = 0;
    printf("-- the slot: rows from the block; a prefab naming none is placed without asking the game\n");
    reset_fakes();
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Door.meta.json", lib);
    _snprintf_s(none, sizeof none, _TRUNCATE, "%s\\Nothing.meta.json", lib);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", lib);
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_MODS_RESOLVE;
    call.text = OBJECT_PREFAB;   /* no block, no sidecar */
    call.text_len = sizeof OBJECT_PREFAB - 1;
    call.meta_path = none;
    call.name = "Nothing";
    CHECK(pm_slot(&call, out, sizeof out) == SH_PREFAB_MODS_PLACE && F.main_calls == 0);
    text = slurp(path, &len);   /* Door.json carries its block */
    call.text = text;
    call.text_len = len;
    call.meta_path = meta;
    call.name = "Door";
    /* The recorded row places the door: it reaches the map through the placed door, not as a map-level row (D1). */
    CHECK(pm_slot(&call, out, sizeof out) == SH_PREFAB_MODS_PLACE && F.main_calls == 1 && F.attaches == 0 &&
          F.activations == 1);
    {
        /* A prefab an earlier version saved: refused with its sentence. */
        char old[MAX_PATH * 2];
        _snprintf_s(old, sizeof old, _TRUNCATE, "%s\\Old hall.meta.json", lib);
        call.text = OBJECT_PREFAB;
        call.text_len = sizeof OBJECT_PREFAB - 1;
        call.meta_path = old;
        call.name = "Old hall";
        CHECK(pm_slot(&call, out, sizeof out) == SH_PREFAB_MODS_REFUSED && !strcmp(out, SH_PREFAB_EARLIER_TEXT) &&
              !strcmp(F.last_print, "Prefab Old hall was not placed: it was saved by an earlier Snapmap+; run the "
                                     "Snapmap+ installer to update it."));
    }

    printf("-- META_READ: the view from the block or from a tail of the file; an old sidecar is not read\n");
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_META_READ;
    call.text = text;
    call.text_len = len;
    call.meta_path = meta;
    CHECK(pm_slot(&call, out, sizeof out) == SH_PREFAB_META_BLOCK && strstr(out, "\"desc\":\"An airlock\"") && strstr(out, LOCAL_ROW));
    call.text = text + 40;
    call.text_len = len - 40;
    call.tail = 1;
    CHECK(pm_slot(&call, out, sizeof out) == SH_PREFAB_META_BLOCK && strstr(out, "\"desc\":\"An airlock\""));
    call.text = OBJECT_PREFAB;
    call.text_len = sizeof OBJECT_PREFAB - 1;
    call.tail = 0;
    call.meta_path = none;
    CHECK(pm_slot(&call, out, sizeof out) == SH_PREFAB_META_NONE && out[0] == 0);
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Old hall.meta.json", lib);
    call.meta_path = meta;
    CHECK(pm_slot(&call, out, sizeof out) == SH_PREFAB_META_EARLIER && out[0] == 0);
    free(text);

    printf("-- META_WRITE and MODS_RECORD through the slot\n");
    {
        char p2[MAX_PATH * 2];
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\Slot.json", lib);
        CHECK(put_file(p2, OBJECT_PREFAB));
        memset(&call, 0, sizeof call);
        call.op = SH_MODULE_PREFAB_MODS_RECORD;
        call.text = OBJECT_PREFAB;
        call.text_len = sizeof OBJECT_PREFAB - 1;
        call.path = p2;
        CHECK(pm_slot(&call, out, sizeof out) == 1 && strstr(out, LOCAL_ROW));
        memset(&call, 0, sizeof call);
        call.op = SH_MODULE_PREFAB_META_WRITE;
        call.text = "{\"desc\":\"s\",\"tags\":[]}";
        call.text_len = strlen(call.text);
        call.path = p2;
        call.meta_path = none;
        CHECK(pm_slot(&call, out, sizeof out) == 1 && !mt_exists(none));
        text = slurp(p2, &len);
        CHECK(strstr(block_of(text, len), "\"description\":\"s\"") && strstr(block_of(text, len), LOCAL_ROW));
        free(text);
        DeleteFileA(p2);
    }
    reset_fakes();
}

/* The bytes of a stored entry in a ZIP we wrote, or NULL. */
static const char *zip_entry(const char *z, size_t zl, const char *name, size_t *len)
{
    size_t nl = strlen(name), i;
    for (i = 0; z && i + 30 + nl <= zl; i++)
        if (z[i] == 'P' && z[i + 1] == 'K' && z[i + 2] == 3 && z[i + 3] == 4 &&
            (size_t)((unsigned char)z[i + 26] | ((unsigned char)z[i + 27] << 8)) == nl && !memcmp(z + i + 30, name, nl)) {
            const unsigned char *h = (const unsigned char *)z + i;
            *len = (size_t)h[18] | ((size_t)h[19] << 8) | ((size_t)h[20] << 16) | ((size_t)h[21] << 24);
            return z + i + 30 + nl + (size_t)(h[28] | (h[29] << 8));
        }
    return NULL;
}

/* The ZIP's entries named under `prefix`, -1 when it does not open. */
static int zip_count(const char *zipfile, const char *prefix)
{
    sh_zip_reader r;
    char cause[256];
    size_t i;
    int n = 0;
    if (!HO(cause, sh_zip_reader_open(&r, zipfile, HO_P(0)))) return -1;
    for (i = 0; i < r.n; i++) if (!strncmp(r.e[i].name, prefix, strlen(prefix))) n++;
    sh_zip_reader_close(&r);
    return n;
}

/* Replace text of equal length inside a stored entry and fix its CRC in the local header and the central directory,
 * so only the content is wrong. */
static int patch_entry(unsigned char *zipb, size_t len, const char *name, const char *from, const char *to)
{
    size_t nl = strlen(name), at = 0, size, i;
    unsigned long crc;
    unsigned char *hit = NULL;
    for (i = 0; i + 30 + nl <= len && !at; i++)
        if (zipb[i] == 'P' && zipb[i + 1] == 'K' && zipb[i + 2] == 3 && zipb[i + 3] == 4 &&
            (size_t)(zipb[i + 26] | (zipb[i + 27] << 8)) == nl && memcmp(zipb + i + 30, name, nl) == 0)
            at = i + 30 + nl + (size_t)(zipb[i + 28] | (zipb[i + 29] << 8));
    if (!at || strlen(from) != strlen(to)) return 0;
    {
        const unsigned char *h = zipb + at - nl - 30;
        size = (size_t)h[18] | ((size_t)h[19] << 8) | ((size_t)h[20] << 16) | ((size_t)h[21] << 24);
    }
    for (i = at; i + strlen(from) <= at + size && !hit; i++) if (memcmp(zipb + i, from, strlen(from)) == 0) hit = zipb + i;
    if (!hit) return 0;
    memcpy(hit, to, strlen(to));
    crc = mt_crc32(zipb + at, size);
    mt_put32(zipb + at - nl - 30 + 14, crc);
    for (i = 0; i + 46 + nl <= len; i++)
        if (zipb[i] == 'P' && zipb[i + 1] == 'K' && zipb[i + 2] == 1 && zipb[i + 3] == 2 &&
            (size_t)(zipb[i + 28] | (zipb[i + 29] << 8)) == nl && memcmp(zipb + i + 46, name, nl) == 0) {
            mt_put32(zipb + i + 16, crc);
            return 1;
        }
    return 0;
}

static void zip(const char *tmp, const char *libA, const char *storeA, const sh_modmanifest_row *door)
{
    char zipA[MAX_PATH * 2], zipA2[MAX_PATH * 2], libB[MAX_PATH * 2], storeB[MAX_PATH * 2], prefab[MAX_PATH * 2],
         meta[MAX_PATH * 2];
    char msg[600], name[160], path[MAX_PATH * 2], cause[300], unit_entry[120], file_entry[120], want[600];
    char unit[65] = "", file_hex[65] = "";
    modexport_stats st;
    sh_modmanifest_list pinned;
    size_t len = 0, zl = 0, el = 0, ul = 0, dl = 0;
    char *text, *z, *disk, *zip_door = NULL;
    size_t zip_door_len = 0;
    const char *e;
    FILETIME before_p, after_p;

    printf("-- export: <name>.json with its rows pinned, the unit's manifest and its file; no mods/ folder, no index\n");
    reset_fakes();
    use_store(storeA);
    _snprintf_s(zipA, sizeof zipA, _TRUNCATE, "%s\\Door export.zip", tmp);
    _snprintf_s(zipA2, sizeof zipA2, _TRUNCATE, "%s\\Door export again.zip", tmp);
    _snprintf_s(libB, sizeof libB, _TRUNCATE, "%s\\libB", tmp);
    _snprintf_s(storeB, sizeof storeB, _TRUNCATE, "%s\\storeB", tmp);
    _snprintf_s(prefab, sizeof prefab, _TRUNCATE, "%s\\Door.json", libA);
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Door.meta.json", libA);
    disk = slurp(prefab, &dl);
    CHECK(HO(msg, sh_prefab_mods_export(prefab, meta, "Door", zipA, HO_P(0))) == 1);
    CHECK(strstr(msg, "Exported prefab Door with 1 mod to ") != NULL);
    z = slurp(zipA, &zl);
    CHECK(zip_count(zipA, "") == 3 && zip_count(zipA, "units/") == 1 && zip_count(zipA, "files/") == 1 &&
          zip_count(zipA, "mods/") == 0);
    e = zip_entry(z, zl, "Door.json", &el);
    CHECK(e != NULL);
    memset(&pinned, 0, sizeof pinned);
    if (e) {
        zip_door = (char *)malloc(el + 1);
        memcpy(zip_door, e, el);
        zip_door[el] = 0;
        zip_door_len = el;
    }
    /* the rows are pinned: units and their composition, no longer the local build */
    CHECK(zip_door && sh_prefab_mods_rows_of(zip_door, zip_door_len, NULL, &pinned) == 1 && pinned.rows[0].pinned &&
          pinned.rows[0].unit_count == 1 && !strcmp(pinned.rows[0].salt, "testdoor") &&
          !strcmp(pinned.rows[0].name, "test.door"));
    CHECK(zip_door && !strstr(zip_door, "\"local\":true") && strstr(block_of(zip_door, zip_door_len), "\"objects\"") &&
          strstr(block_of(zip_door, zip_door_len), "An airlock") && strcmp(pinned.rows[0].build, door->build) != 0);
    if (pinned.count == 1 && pinned.rows[0].unit_count == 1) {
        const char *units[1];
        char chash[SH_MODMANIFEST_HASH_CAP];
        strcpy_s(unit, sizeof unit, pinned.rows[0].units[0]);
        units[0] = pinned.rows[0].units[0];
        CHECK(sh_modmanifest_composition_hash(pinned.rows[0].salt, units, 1, chash) && !strcmp(chash, pinned.rows[0].build));
    }
    /* the unit's manifest is named by its own SHA-256 and names the file by its SHA-256 */
    _snprintf_s(unit_entry, sizeof unit_entry, _TRUNCATE, "units/%s.json", unit);
    e = zip_entry(z, zl, unit_entry, &ul);
    {
        char hex[65];
        if (e) sh_mb_sha256_hex(e, ul, hex);
        CHECK(e && !strcmp(hex, unit));
    }
    sh_mb_sha256_hex(DOOR_DECL, sizeof DOOR_DECL - 1, file_hex);
    _snprintf_s(file_entry, sizeof file_entry, _TRUNCATE, "files/%s", file_hex);
    CHECK(e && memmem_find(e, ul, "\"path\":\"entities/door.decl\"") && memmem_find(e, ul, file_hex));
    e = zip_entry(z, zl, file_entry, &el);
    CHECK(e && el == sizeof DOOR_DECL - 1 && !memcmp(e, DOOR_DECL, el));   /* the installed folder's bytes */
    CHECK(!memmem_find(z, zl, "bundle.canonical.json") && !memmem_find(z, zl, "mod.json") &&
          !memmem_find(z, zl, "export.json") && !memmem_find(z, zl, "prefab.meta.json") && !memmem_find(z, zl, "test.door@"));
    /* the engine's bytes are the author's, and the file on disk is not changed by the export */
    {
        size_t sl = 0;
        char *strip = zip_door ? sh_prefab_mods_strip_copy(zip_door, zip_door_len, &sl) : NULL;
        CHECK(strip && sl == sizeof OBJECT_PREFAB - 1 && !memcmp(strip, OBJECT_PREFAB, sl));
        free(strip);
        text = slurp(prefab, &len);
        CHECK(text && disk && len == dl && !memcmp(text, disk, len) && strstr(block_of(text, len), LOCAL_ROW) &&
              !strstr(block_of(text, len), "\"objects\""));
        free(text);
    }
    free(z);
    free(disk);
    printf("-- two exports of the same prefab are byte-identical\n");
    CHECK(HO(msg, sh_prefab_mods_export(prefab, meta, "Door", zipA2, HO_P(0))) == 1);
    {
        size_t al = 0, bl = 0;
        char *a = slurp(zipA, &al), *b = slurp(zipA2, &bl);
        CHECK(a && b && al == bl && !memcmp(a, b, al));
        free(a); free(b);
    }
    CHECK(HO(msg, sh_prefab_mods_export(prefab, meta, "Door", "C:\\x\\Door.json", HO_P(0))) == -1 &&
          strstr(msg, "must end in .zip"));
    /* A map's importer refuses it. */
    CHECK(HOT(char *, cause, modexport_read(zipA, &len, &st, HO_P(0))) == NULL && strstr(cause, "prefab's export, not a map's"));

    printf("-- a prefab without mods exports a ZIP too: its file and nothing else\n");
    {
        char p0[MAX_PATH * 2], z0[MAX_PATH * 2];
        _snprintf_s(p0, sizeof p0, _TRUNCATE, "%s\\Crate.json", libA);
        _snprintf_s(z0, sizeof z0, _TRUNCATE, "%s\\Crate.zip", tmp);
        CHECK(put_file(p0, STOCK_PREFAB));
        CHECK(sh_prefab_mods_record(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, p0, NULL) == 0);
        CHECK(HO(msg, sh_prefab_mods_export(p0, NULL, "Crate", z0, HO_P(0))) == 1 && strstr(msg, "it uses no mods"));
        z = slurp(z0, &zl);
        disk = slurp(p0, &len);
        e = zip_entry(z, zl, "Crate.json", &el);
        CHECK(e && disk && el == len && !memcmp(e, disk, len) && zip_count(z0, "") == 1);
        free(z); free(disk);
    }

    printf("-- the prefab importer refuses a map's ZIP\n");
    {
        static const char MAP[] = "{\"entities\":[],\"name\":\"idSnapMap\"}";
        char zm[MAX_PATH * 2];
        _snprintf_s(zm, sizeof zm, _TRUNCATE, "%s\\map.zip", tmp);
        CHECK(HO(cause, modexport_write(MAP, sizeof MAP - 1, zm, &st, HO_P(0))));
        CHECK(HO2(name, msg, sh_prefab_mods_import(zm, libB, HO_P(0), HO_P(1))) == -1 &&
              !strcmp(msg, "The prefab was not imported: this is a map's export, not a prefab's."));
    }

    printf("-- the prefab importer refuses every earlier shape and writes nothing\n");
    {
        static const char *const OLD_N[] = { "Door.json", "mods/test.door/bundle.canonical.json" };
        static const char *const OLD_M[] = { "Door.json", "mods/door/mod.json" };
        static const char *const OLD_I[] = { "Door.json", "export.json" };
        const char *old_d[2];
        char zo[MAX_PATH * 2], door_pinned[8192];
        _snprintf_s(zo, sizeof zo, _TRUNCATE, "%s\\old form.zip", tmp);
        old_d[0] = STOCK_PREFAB;
        old_d[1] = "{\"mod\":\"testdoor@}";
        CHECK(mt_zip_stored(zo, 2, OLD_N, old_d));
        CHECK(HO2(name, msg, sh_prefab_mods_import(zo, libB, HO_P(0), HO_P(1))) == -1 &&
              !strcmp(msg, "The prefab was not imported. " MODEXPORT_EARLIER));
        /* a whole mod folder beside a prefab whose rows are pinned: the earlier layout */
        _snprintf_s(door_pinned, sizeof door_pinned, _TRUNCATE, "%s", zip_door ? zip_door : STOCK_PREFAB);
        old_d[0] = door_pinned;
        old_d[1] = "{\"name\":\"test.door\"}";
        CHECK(mt_zip_stored(zo, 2, OLD_M, old_d));
        CHECK(HO2(name, msg, sh_prefab_mods_import(zo, libB, HO_P(0), HO_P(1))) == -1 &&
              !strcmp(msg, "The prefab was not imported. " MODEXPORT_EARLIER));
        old_d[1] = "{\"format\":1,\"bundles\":[]}";
        CHECK(mt_zip_stored(zo, 2, OLD_I, old_d));
        CHECK(HO2(name, msg, sh_prefab_mods_import(zo, libB, HO_P(0), HO_P(1))) == -1 &&
              !strcmp(msg, "The prefab was not imported. " MODEXPORT_EARLIER));
        /* a prefab whose rows name "id" and "bundle" */
        {
            static const char *const ONE[] = { "Old.json" };
            char old_prefab[2048], *with;
            size_t wl = 0;
            const char *side = "{\"desc\":\"\",\"tags\":[],\"mods\":[{\"id\":\"test.door\",\"version\":\"1.0.0\",\"bundle\":\""
                               GHOST "\",\"size\":1}]}";
            with = sh_prefab_mods_embed(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, side, strlen(side), NULL, &wl);
            CHECK(with != NULL);
            _snprintf_s(old_prefab, sizeof old_prefab, _TRUNCATE, "%s", with ? with : "");
            old_d[0] = old_prefab;
            CHECK(mt_zip_stored(zo, 1, ONE, old_d));
            CHECK(HO2(name, msg, sh_prefab_mods_import(zo, libB, HO_P(0), HO_P(1))) == -1 &&
                  !strcmp(msg, "The prefab was not imported. " MODEXPORT_EARLIER));
            free(with);
        }
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libB);
        CHECK(!mt_exists(path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Old.json", libB);
        CHECK(!mt_exists(path));
    }

    printf("-- import into an empty store and library: the units in the content cache, the prefab added, nothing installed\n");
    use_store(storeB);
    CHECK(sh_modserve_bundle_state(door->build, NULL) == 0);
    {
        size_t cu0, cf0;
        cache_counts(storeB, &cu0, &cf0);
        CHECK(cu0 == 0 && cf0 == 0);
    }
    CHECK(HO2(name, msg, sh_prefab_mods_import(zipA, libB, HO_P(0), HO_P(1))) == 1);
    CHECK(!strcmp(name, "Door") && !strcmp(msg, "Imported prefab Door with its mod content."));
    CHECK(F.refreshes == 0 && mod_folders(storeB) == 0);   /* nothing installed, no mod folder, no refresh */
    {
        size_t cu, cf;
        cache_counts(storeB, &cu, &cf);
        CHECK(cu == 1 && cf == 1 && sh_cache_unit_state(unit, NULL, NULL, NULL) == 1);
        CHECK(sh_cache_blob_path(file_hex) != NULL);
    }
    CHECK(sh_modserve_bundle_state(door->build, NULL) == 0 && sh_modserve_row_served(door, NULL) != SH_MODSERVE_ROW_SERVED);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libB);
    text = slurp(path, &len);
    CHECK(text && zip_door && len == zip_door_len && !memcmp(text, zip_door, len));   /* the export's text, as it is */
    CHECK(text && strstr(block_of(text, len), "An airlock") && strstr(block_of(text, len), "\"objects\""));
    {
        /* the library's prefab names units that are here: the status and the console say so, and placing it works */
        sh_modmanifest_list rows;
        size_t missing = 9;
        char *json;
        CHECK(sh_prefab_mods_rows_of(text, len, NULL, &rows) == 1 && rows.rows[0].pinned);
        json = sh_prefab_mods_status_json(&rows, &missing);
        CHECK(json && missing == 0 && strstr(json, "\"installed\":true") && strstr(json, "\"state\":\"served\""));
        free(json);
        {
            /* placing it hands its pinned rows to the open map */
            char m2[300];
            int seq2 = 0, calls = F.pinned_calls;
            CHECK(HO(m2, sh_prefab_mods_resolve_main(text, len, &rows, "Door", &seq2, HO_P(0))) == SH_PREFAB_MODS_PLACE &&
                  F.pinned_calls == calls + 1 && F.pinned_rows == 1 && m2[0] == 0);
        }
        sh_modmanifest_list_free(&rows);
    }
    free(text);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.meta.json", libB);
    CHECK(!mt_exists(path));

    printf("-- importing it again writes nothing\n");
    {
        char pj[MAX_PATH * 2];
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door.json", libB);
        CHECK(write_time(pj, &before_p));
        Sleep(30);
        CHECK(HO2(name, msg, sh_prefab_mods_import(zipA, libB, HO_P(0), HO_P(1))) == 0);
        CHECK(!strcmp(msg, "Prefab Door and its mod content are already here; nothing was changed.") && F.refreshes == 0);
        CHECK(write_time(pj, &after_p) && CompareFileTime(&before_p, &after_p) == 0);
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door (2).json", libB);
        CHECK(!mt_exists(pj));
    }

    printf("-- a different prefab of that name is kept; the import takes the next name\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libB);
    CHECK(put_file(path, STOCK_PREFAB));
    CHECK(HO2(name, msg, sh_prefab_mods_import(zipA, libB, HO_P(0), HO_P(1))) == 1 && !strcmp(name, "Door (2)"));
    CHECK(!strcmp(msg, "Imported prefab Door (2) with its mod content."));
    text = slurp(path, &len);
    CHECK(text && !strcmp(text, STOCK_PREFAB));
    free(text);

    printf("-- a prefab already in the library whose content is not in the cache: the content is added\n");
    {
        char content[MAX_PATH * 2];
        size_t cu, cf;
        _snprintf_s(content, sizeof content, _TRUNCATE, "%s\\.content", storeB);
        mt_remove_tree(content);
        use_store(storeB);
        cache_counts(storeB, &cu, &cf);
        CHECK(cu == 0 && cf == 0 && sh_cache_unit_state(unit, NULL, NULL, NULL) == -1);
        CHECK(HO2(name, msg, sh_prefab_mods_import(zipA, libB, HO_P(0), HO_P(1))) == 0 && !strcmp(name, "Door (2)"));
        CHECK(!strcmp(msg, "Prefab Door (2) is already in your library; its mod content was added."));
        cache_counts(storeB, &cu, &cf);
        CHECK(cu == 1 && cf == 1 && sh_cache_unit_state(unit, NULL, NULL, NULL) == 1);
        CHECK(HO2(name, msg, sh_prefab_mods_import(zipA, libB, HO_P(0), HO_P(1))) == 0);
        CHECK(!strcmp(msg, "Prefab Door (2) and its mod content are already here; nothing was changed."));
    }

    printf("-- an extracted export folder imports as the ZIP does\n");
    {
        char ex[MAX_PATH * 2], pj[MAX_PATH * 2], storeC[MAX_PATH * 2], libD[MAX_PATH * 2];
        _snprintf_s(ex, sizeof ex, _TRUNCATE, "%s\\Downloads\\Door export", tmp);
        _snprintf_s(storeC, sizeof storeC, _TRUNCATE, "%s\\storeC", tmp);
        _snprintf_s(libD, sizeof libD, _TRUNCATE, "%s\\libD", tmp);
        CHECK(mt_unzip_stored(zipA, ex) == 3);
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door.json", ex);
        use_store(storeC);
        CHECK(HO2(name, msg, sh_prefab_mods_import(pj, libD, HO_P(0), HO_P(1))) == 1);
        CHECK(!strcmp(name, "Door") && !strcmp(msg, "Imported prefab Door with its mod content."));
        CHECK(sh_cache_unit_state(unit, NULL, NULL, NULL) == 1 && mod_folders(storeC) == 0);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libD);
        text = slurp(path, &len);
        CHECK(text && zip_door && len == zip_door_len && !memcmp(text, zip_door, len));
        free(text);
        /* a prefab whose rows name content the export does not carry: added, and the message says what it still needs */
        {
            char lone[MAX_PATH * 2], *with;
            const char *side;
            size_t wl = 0;
            _snprintf_s(lone, sizeof lone, _TRUNCATE, "%s\\Ghost door.json", tmp);
            side = "{\"desc\":\"\",\"tags\":[],\"mods\":[{\"mod\":\"ghostmod\",\"name\":\"ghost.mod\","
                   "\"local\":true,\"size\":1,\"names\":[]}]}";
            with = sh_prefab_mods_embed(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, side, strlen(side), NULL, &wl);
            CHECK(with && sh_prefab_mods_write_file(lone, with, wl));
            free(with);
            CHECK(HO2(name, msg, sh_prefab_mods_import(lone, libD, HO_P(0), HO_P(1))) == 1);
            CHECK(!strcmp(name, "Ghost door") &&
                  !strcmp(msg, "Imported prefab Ghost door. It also needs ghost.mod, which the export "
                               "does not carry."));
        }
        /* the same for a pinned row whose units are not carried, and for the units the prefab names but the ZIP lacks */
        {
            char lone[MAX_PATH * 2], *with;
            sh_modmanifest_list gl;
            size_t wl = 0;
            memset(&gl, 0, sizeof gl);
            pinned_row(&gl, "ghostmod", "ghost.mod", HASH_UNIT);
            _snprintf_s(lone, sizeof lone, _TRUNCATE, "%s\\Pinned ghost.json", tmp);
            with = sh_prefab_mods_embed(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, "{\"desc\":\"\",\"tags\":[]}", 22, &gl, &wl);
            CHECK(with && sh_prefab_mods_write_file(lone, with, wl));
            free(with);
            sh_modmanifest_list_free(&gl);
            CHECK(HO2(name, msg, sh_prefab_mods_import(lone, libD, HO_P(0), HO_P(1))) == 1);
            CHECK(!strcmp(name, "Pinned ghost") &&
                  !strcmp(msg, "Imported prefab Pinned ghost. It also needs ghost.mod, which the export "
                               "does not carry."));
            /* the prefab as the export wrote it, without its units */
            {
                static const char *const ONE[] = { "Door.json" };
                const char *data[1];
                char lonez[MAX_PATH * 2];
                char libF[MAX_PATH * 2];
                _snprintf_s(lonez, sizeof lonez, _TRUNCATE, "%s\\door only.zip", tmp);
                _snprintf_s(libF, sizeof libF, _TRUNCATE, "%s\\libF", tmp);
                data[0] = zip_door ? zip_door : "";
                CHECK(mt_zip_stored(lonez, 1, ONE, data));
                use_store(storeB);
                {
                    char content[MAX_PATH * 2];
                    _snprintf_s(content, sizeof content, _TRUNCATE, "%s\\.content", storeB);
                    mt_remove_tree(content);
                    use_store(storeB);
                }
                CHECK(HO2(name, msg, sh_prefab_mods_import(lonez, libF, HO_P(0), HO_P(1))) == 1);
                CHECK(!strcmp(msg, "Imported prefab Door. It also needs test.door, which the export "
                                   "does not carry."));
            }
        }
    }

    printf("-- a damaged entry refuses the import, and the prefab it added is taken out again\n");
    {
        char libE[MAX_PATH * 2], storeE[MAX_PATH * 2], bad[MAX_PATH * 2], uname[160], fname[160];
        unsigned char *zb = (unsigned char *)slurp(zipA, &zl);
        size_t cu, cf;
        _snprintf_s(libE, sizeof libE, _TRUNCATE, "%s\\libE", tmp);
        _snprintf_s(storeE, sizeof storeE, _TRUNCATE, "%s\\storeE", tmp);
        _snprintf_s(bad, sizeof bad, _TRUNCATE, "%s\\bad prefab.zip", tmp);
        _snprintf_s(uname, sizeof uname, _TRUNCATE, "units/%s.json", unit);
        _snprintf_s(fname, sizeof fname, _TRUNCATE, "files/%s", file_hex);
        /* the unit's manifest: its bytes changed, its name kept */
        use_store(storeE);
        CHECK(zb && patch_entry(zb, zl, uname, "entities/door", "entities/DOOR"));
        CHECK(zb && spit(bad, zb, zl));
        CHECK(HO2(name, msg, sh_prefab_mods_import(bad, libE, HO_P(0), HO_P(1))) == -1);
        _snprintf_s(want, sizeof want, _TRUNCATE, "The prefab was not imported: the export is damaged: one of its mod files is "
                    "not what it says; export it again.");
        CHECK(!strcmp(msg, want));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libE);
        CHECK(!mt_exists(path));   /* the prefab it had added is gone */
        cache_counts(storeE, &cu, &cf);
        CHECK(sh_cache_unit_state(unit, NULL, NULL, NULL) == -1 && cf == 0 && mod_folders(storeE) == 0);
        free(zb);
        /* the file: its bytes changed, its name kept */
        zb = (unsigned char *)slurp(zipA, &zl);
        CHECK(zb && patch_entry(zb, zl, fname, "industrial", "INDUSTRIAL") && spit(bad, zb, zl));
        CHECK(HO2(name, msg, sh_prefab_mods_import(bad, libE, HO_P(0), HO_P(1))) == -1);
        _snprintf_s(want, sizeof want, _TRUNCATE, "The prefab was not imported: the export is damaged: one of its mod files is "
                    "not what it says; export it again.");
        CHECK(!strcmp(msg, want));
        CHECK(!mt_exists(path));
        CHECK(sh_cache_blob_path(file_hex) == NULL && sh_cache_unit_state(unit, NULL, NULL, NULL) != 1);   /* never kept */
        free(zb);
        /* a cache that cannot be written: nothing of the import stays */
        {
            char libG[MAX_PATH * 2], storeG[MAX_PATH * 2];
            _snprintf_s(libG, sizeof libG, _TRUNCATE, "%s\\libG", tmp);
            _snprintf_s(storeG, sizeof storeG, _TRUNCATE, "%s\\storeG", tmp);
            CHECK(put_file(storeG, "not a folder"));   /* the mods folder is a file: no content cache can be made */
            sh_cache_set_mods_root(storeG);
            sh_cache_local_changed();
            CHECK(HO2(name, msg, sh_prefab_mods_import(zipA, libG, HO_P(0), HO_P(1))) == -1);
            CHECK(!strncmp(msg, "The prefab was not imported", 27));
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libG);
            CHECK(!mt_exists(path));
            use_store(storeB);
        }
    }

    printf("-- with scripts turned off, a unit that carries scripts refuses the import and nothing stays\n");
    {
        char libH[MAX_PATH * 2], storeH[MAX_PATH * 2], zs[MAX_PATH * 2], uname[160];
        static const char SCRIPTED[] =
            "{\"files\":[{\"path\":\"scripts/door.lua\",\"sha256\":\"" GHOST "\",\"size\":9}],\"format\":1,\"game\":[],\"mod\":{},"
            "\"root\":{\"kind\":\"entity\",\"name\":\"entities/door\"}}";
        const char *names3[2], *data3[2];
        size_t cu, cf;
        _snprintf_s(libH, sizeof libH, _TRUNCATE, "%s\\libH", tmp);
        _snprintf_s(storeH, sizeof storeH, _TRUNCATE, "%s\\storeH", tmp);
        _snprintf_s(zs, sizeof zs, _TRUNCATE, "%s\\scripted prefab.zip", tmp);
        _snprintf_s(uname, sizeof uname, _TRUNCATE, "units/%s.json", unit);
        names3[0] = "Door.json"; data3[0] = zip_door ? zip_door : "";
        names3[1] = uname; data3[1] = SCRIPTED;
        CHECK(mt_zip_stored(zs, 2, names3, data3));
        use_store(storeH);
        g_scripts_on = 0;
        CHECK(HO2(name, msg, sh_prefab_mods_import(zs, libH, HO_P(0), HO_P(1))) == -1);
        CHECK(!strcmp(msg, "The prefab was not imported. " SH_MODUI_SCRIPTS_OFF_PREFAB));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libH);
        CHECK(!mt_exists(path));
        cache_counts(storeH, &cu, &cf);
        CHECK(cu == 0 && cf == 0 && mod_folders(storeH) == 0);
        /* the same export without scripts is not affected by the setting */
        CHECK(HO2(name, msg, sh_prefab_mods_import(zipA, libH, HO_P(0), HO_P(1))) == 1 &&
              !strcmp(msg, "Imported prefab Door with its mod content."));
        g_scripts_on = 1;
        use_store(storeB);
    }

    printf("-- the console: mods, export and import of a ZIP and of a folder\n");
    {
        const char *mods_argv[] = { "mods", "Door" };
        const char *exp_argv[3];
        const char *imp_argv[2];
        char zip2[MAX_PATH * 2], libC[MAX_PATH * 2];
        size_t cu, cf;
        use_store(storeB);
        CHECK(HO2(name, msg, sh_prefab_mods_import(zipA, libB, HO_P(0), HO_P(1))) == 0);   /* the content is back in storeB */
        sh_prefab_mods_set_root(libB);
        F.prints_all[0] = 0;
        mods_argv[1] = "Door (2)";
        sh_prefab_mods_command(2, mods_argv);
        CHECK(strstr(F.prints_all, "Prefab Door (2) needs 1 mod:") &&
              strstr(F.prints_all, "  test.door: its content is on this computer\n"));
        mods_argv[1] = "Door";
        sh_prefab_mods_command(2, mods_argv);
        CHECK(!strcmp(F.last_print, "Prefab Door records no mods."));
        mods_argv[1] = "Nope";
        sh_prefab_mods_command(2, mods_argv);
        CHECK(!strcmp(F.last_print, "sh_prefab mods: no prefab named \"Nope\"."));
        /* the same prefab on a computer whose cache lacks the content */
        {
            char content[MAX_PATH * 2], other[MAX_PATH * 2];
            _snprintf_s(other, sizeof other, _TRUNCATE, "%s\\storeI", tmp);
            use_store(other);
            F.prints_all[0] = 0;
            mods_argv[1] = "Door (2)";
            sh_prefab_mods_command(2, mods_argv);
            CHECK(strstr(F.prints_all, "  test.door: its content is not on this computer\n"));
            /* and its export is refused with the mod's name: nothing here to carry */
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\nothing here.zip", tmp);
            CHECK(HO(msg, sh_prefab_mods_export(libB[0] ? (_snprintf_s(content, sizeof content, _TRUNCATE, "%s\\Door (2).json", libB), content) : NULL,
                                                NULL, "Door (2)", path, HO_P(0))) == -1);
            CHECK(strstr(msg, "Prefab Door (2) was not exported: the content of test.door that this ") == msg &&
                  strstr(msg, " is not on this computer. Import the prefab's .zip export again") && !mt_exists(path));
            use_store(storeB);
        }
        /* the export of a downloaded prefab's own units, from the cache alone */
        _snprintf_s(zip2, sizeof zip2, _TRUNCATE, "%s\\console.zip", tmp);
        exp_argv[0] = "export"; exp_argv[1] = "Door (2)"; exp_argv[2] = zip2;
        sh_prefab_mods_command(3, exp_argv);
        CHECK(strstr(F.last_print, "Exported prefab Door (2) with 1 mod to ") && mt_exists(zip2));
        z = slurp(zip2, &zl);
        CHECK(zip_entry(z, zl, "Door (2).json", &el) != NULL && zip_count(zip2, "units/") == 1 && zip_count(zip2, "files/") == 1);
        free(z);
        _snprintf_s(libC, sizeof libC, _TRUNCATE, "%s\\libC", tmp);
        sh_prefab_mods_set_root(libC);
        imp_argv[0] = "import"; imp_argv[1] = zip2;
        sh_prefab_mods_command(2, imp_argv);
        CHECK(!strcmp(F.last_print, "Imported prefab Door (2) with its mod content."));
        cache_counts(storeB, &cu, &cf);
        CHECK(cu == 1 && cf == 1);
        {
            char ex2[MAX_PATH * 2], pj[MAX_PATH * 2];
            _snprintf_s(ex2, sizeof ex2, _TRUNCATE, "%s\\console export", tmp);
            CHECK(mt_unzip_stored(zip2, ex2) == 3);
            _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door (2).json", ex2);
            imp_argv[1] = pj;
            sh_prefab_mods_command(2, imp_argv);
            CHECK(!strcmp(F.last_print, "Prefab Door (2) and its mod content are already here; nothing was changed."));
        }
        sh_prefab_mods_command(0, NULL);
        CHECK(!strcmp(F.last_print, SH_PREFAB_CMD_HELP));
        sh_prefab_mods_set_root(NULL);
    }

    printf("-- the slot's import answer is JSON\n");
    {
        sh_module_prefab_call call;
        char out[1024];
        memset(&call, 0, sizeof call);
        call.op = SH_MODULE_PREFAB_IMPORT;
        call.zip_path = zipA;
        call.path = libB;
        CHECK(pm_slot(&call, out, sizeof out) == 0 && strstr(out, "{\"ok\":true,\"added\":false,\"name\":\"Door (2)\",\"message\":\""));
        /* the placement poll through the slot: nothing is ever held */
        memset(&call, 0, sizeof call);
        call.op = SH_MODULE_PREFAB_MODS_POLL;
        call.seq = 5;
        CHECK(pm_slot(&call, out, sizeof out) == 0);
    }

    printf("-- a prefab whose row names a mod not installed is not exported\n");
    use_store(storeA);
    {
        char p3[MAX_PATH * 2], z3[MAX_PATH * 2], *with;
        const char *side;
        size_t wl = 0;
        _snprintf_s(p3, sizeof p3, _TRUNCATE, "%s\\Ghost.json", libA);
        _snprintf_s(z3, sizeof z3, _TRUNCATE, "%s\\ghost.zip", tmp);
        side = "{\"desc\":\"\",\"tags\":[],\"mods\":[{\"mod\":\"ghostmod\",\"name\":\"ghost.mod\","
               "\"local\":true,\"size\":1,\"names\":[]}]}";
        with = sh_prefab_mods_embed(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, side, strlen(side), NULL, &wl);
        CHECK(with && sh_prefab_mods_write_file(p3, with, wl));
        free(with);
        CHECK(HO(msg, sh_prefab_mods_export(p3, NULL, "Ghost", z3, HO_P(0))) == -1 &&
              !strcmp(msg, "Prefab Ghost was not exported: this prefab uses ghost.mod, which is not on this computer. Install "
                           "it in the Mods tab, or remove its objects from the prefab.") &&
              !strstr(msg, "ghostmod@") && !mt_exists(z3));
    }

    printf("-- a prefab saved before the block exports with one written for it; the file on disk is unchanged\n");
    {
        char p4[MAX_PATH * 2], m4[MAX_PATH * 2], z4[MAX_PATH * 2];
        _snprintf_s(p4, sizeof p4, _TRUNCATE, "%s\\Old door.json", libA);
        _snprintf_s(m4, sizeof m4, _TRUNCATE, "%s\\Old door.meta.json", libA);
        _snprintf_s(z4, sizeof z4, _TRUNCATE, "%s\\old.zip", tmp);
        CHECK(put_file(p4, OBJECT_PREFAB) && !mt_exists(m4));
        CHECK(HO(msg, sh_prefab_mods_export(p4, m4, "Old door", z4, HO_P(0))) == 1 && strstr(msg, "with 1 mod"));
        z = slurp(z4, &zl);
        e = zip_entry(z, zl, "Old door.json", &el);
        CHECK(e && strstr(block_of(e, el), "\"objects\"") && !strstr(block_of(e, el), door->build) && !strstr(block_of(e, el), "\"local\""));
        CHECK(zip_count(z4, "units/") == 1 && zip_count(z4, "files/") == 1);
        free(z);
        text = slurp(p4, &len);
        CHECK(text && !strcmp(text, OBJECT_PREFAB));
        free(text);
        /* With an earlier version's sidecar and no block: refused. */
        CHECK(put_file(m4, "{\"desc\":\"From the sidecar\",\"tags\":[\"old\"]}"));
        CHECK(HO(msg, sh_prefab_mods_export(p4, m4, "Old door", z4, HO_P(0))) == -1 &&
              !strcmp(msg, "Prefab Old door was not exported: it was saved by an earlier Snapmap+; run the Snapmap+ "
                           "installer to update it."));

    }
    sh_modmanifest_list_free(&pinned);
    free(zip_door);
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], src[MAX_PATH * 2], storeA[MAX_PATH * 2], libA[MAX_PATH * 2];
    sh_modmanifest_row door;
    g_echo = argc > 1 && !strcmp(argv[argc - 1], "-v");
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash keeps what was said */

    CHECK(mt_temp_dir("prefabmods", tmp, sizeof tmp));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\src\\door", tmp);
    _snprintf_s(storeA, sizeof storeA, _TRUNCATE, "%s\\storeA", tmp);
    _snprintf_s(libA, sizeof libA, _TRUNCATE, "%s\\libA", tmp);
    CreateDirectoryA(libA, NULL);
    CHECK(sh_ms_set_root(storeA));
    CHECK(install_mod(src, "testdoor", "test.door", "entities\\door.decl", DOOR_DECL, &door));
    CHECK(sh_modserve_index_build(storeA) >= 1);
    CHECK(sh_modserve_bundle_state(door.build, NULL) == 1);
    /* The pin step of an export: the game and the folder as the proof sees them, the content cache under the store. */
    sh_modpin_set_game(fixture_game, NULL);
    sh_modpin_set_read(no_read, NULL);
    sh_cache_set_mods_root(storeA);
    reset_fakes();

    names();
    block();
    comments(libA);
    recording(libA, &door);
    status(&door);
    resolving(&door);
    resolve_through_slot(libA, &door);
    zip(tmp, libA, storeA, &door);
    one_demon_of_a_pack(tmp, storeA);
    absent_local_row_stands_in();
    published_map_follows_rebuild(tmp, storeA);
    add_to_my_mods_end_to_end(tmp, storeA);
    copy_a_demon_whose_binary_spells(tmp, storeA);

    sh_cache_set_mods_root(NULL);
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    printf("prefab_mods_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
