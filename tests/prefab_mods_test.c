/* prefab_mods_test -- a prefab's details and the mods it needs (backend/prefab_mods.c,
 * backend/prefab_block.c).
 *
 * Over a real store and serving index (a mod built and installed into a
 * temporary folder), covered:
 *   the block: put after the engine's members, replaced, stripped back to the
 *     engine's exact bytes (formatting, comments, an empty object, a first
 *     member), found in a tail, a text that is not an object refused; a view
 *     composed into a block and read back;
 *   comments: the serializer's comments removed outside strings (strings
 *     holding comment openers, escaped quotes and backslashes, comments that
 *     look nested, line comments, an unterminated one), and Create's file
 *     written as plain JSON with its block;
 *   recording: Create writes an object prefab's closure (one row naming the
 *     bundle that provides its inherit, stock names nothing) into the file's
 *     block and no sidecar; description and tag edits rewrite the block and
 *     keep the rows; a prefab without mods gets a block naming none; a module
 *     prefab's file carries its summary and rows; a prefab saved before the
 *     block reads its sidecar, and the first edit writes the block and leaves
 *     the sidecar as it was;
 *   resolution through the real hold (modpublished.c, engine doubles): every
 *     mod installed (activated, attached to the open map with one console
 *     line, or left alone when the map lists it); the store has the missing
 *     one (the prefab's question, Yes downloads, the ready step, then ready);
 *     the store lacks it; the store cannot be reached; No; no gates bound;
 *     a DLC module no row provides, with and without a block; a second
 *     placement while one waits; the slot's rows from the block or a sidecar,
 *     META_READ from the file, a tail or a sidecar;
 *   the ZIP: <name>.json as on disk and the mod's installed folder
 *     (mods/<id>/mod.json and assets/ byte for byte), no index, also for a
 *     prefab without mods; refused by the map importer and refusing a map's
 *     ZIP and the retired bundle form; imported into an empty store and
 *     library, imported again without a write, a differing prefab of the same
 *     name kept beside it; an extracted export folder imported; a lone .json
 *     naming a mod it does not carry; a
 *     prefab whose row names an uninstalled bundle refused; a prefab saved
 *     before the block exported with a block written for the ZIP only; the
 *     console verbs and the slot's outputs; prefab names.
 * argv[1] is unused; -v echoes the backend log. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/prefab_mods.h"
#include "../src/backend/prefab_block.h"
#include "../src/backend/modpublished.h"
#include "../src/backend/modexport.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modbundle.h"
#include "../src/backend/module_prefab_map.h"
#include "../src/common/snapmap_plus_iface.h"
#include "mod_test_util.h"

static int g_failed, g_checks, g_echo;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

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

#define GHOST "9999999999999999999999999999999999999999999999999999999999999999"

/* ---- engine doubles -------------------------------------------------------------------------- */

typedef struct fake {
    char listed[4][65];
    int listed_n;
    int attaches;
    char attached[4][65];
    int activations;
    int prints, toasts, refreshes, main_calls;
    char last_print[1200], prints_all[8192], last_toast[512];
    int ghost_installed;        /* the download installed GHOST */
    int store;                  /* store_has for GHOST: 1, 0, -1 */
    int asks, ask_buttons, answer, answered;
    char last_ask[512];
    int fetches;
} fake;
static fake F;

/* The asset rule: the real serving index answers every row but the ghost's,
 * which only a download installs. */
static int f_row_state(const sh_modmanifest_row *row, char *cause, size_t cap)
{
    if (cause && cap) cause[0] = 0;
    if (!strcmp(row->bundle, GHOST)) {
        if (F.ghost_installed) return SH_MODPUB_ROW_SERVED;
        if (cause) _snprintf_s(cause, cap, _TRUNCATE, "This prefab uses %s, which is not installed.", row->id);
        return SH_MODPUB_ROW_MISSING;
    }
    return sh_modserve_row_served_for(row, "prefab", cause, cap);
}
static int f_listed(const sh_modmanifest_row *row)
{
    int i;
    for (i = 0; i < F.listed_n; i++) if (!strcmp(F.listed[i], row->bundle)) return 1;
    return 0;
}
static int f_attach(const sh_modmanifest_row *row, char *note, size_t cap)
{
    (void)note; (void)cap;
    if (F.attaches < 4) strcpy_s(F.attached[F.attaches], sizeof F.attached[0], row->bundle);
    F.attaches++;
    if (F.listed_n < 4) strcpy_s(F.listed[F.listed_n++], sizeof F.listed[0], row->bundle);
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

static const sh_prefab_mods_ops PM_OPS = {
    f_row_state, f_listed, f_attach, f_activate, f_module_known, f_print, f_toast, f_refresh, f_call_main
};

/* modpublished's doubles: the hold runs as in the game, its jobs inline. */
static int p_on_main(void *c) { (void)c; return 1; }
static int p_row_state(void *c, const sh_modmanifest_row *row, const char *subject, char *cause, size_t cap)
{
    (void)c; (void)subject;
    return f_row_state(row, cause, cap);
}
static int p_rescan(void *c) { (void)c; return 1; }
static int p_activate(void *c, const sh_modmanifest_list *rows) { (void)c; return (int)rows->count; }
static int p_can_ask(void *c) { (void)c; return 1; }
static int p_ask(void *c, unsigned gdm, unsigned set, const char *text)
{
    (void)c; (void)gdm;
    F.asks++; F.ask_buttons = (int)set;
    strcpy_s(F.last_ask, sizeof F.last_ask, text);
    F.answered = 0;
    return 100 + F.asks;
}
static int p_poll(void *c, int ticket)
{
    (void)c; (void)ticket;
    if (F.answered || !F.answer) return 0;
    F.answered = 1;
    return F.answer;
}
static void p_release(void *c, int ticket) { (void)c; (void)ticket; }
static int p_worker_run(void *c, void (*job)(void *), void *ctx) { (void)c; job(ctx); return 1; }
static int p_fetch(void *c, const char *hash, sh_modpub_progress_fn progress, volatile int *cancel)
{
    (void)c; (void)progress; (void)cancel;
    F.fetches++;
    if (!strcmp(hash, GHOST)) F.ghost_installed = 1;
    return 0;
}
static void p_log(void *c, const char *line) { (void)c; backend_log(line); }
static int p_store_has(void *c, const char *hash, volatile int *cancel)
{
    (void)c; (void)cancel;
    return !strcmp(hash, GHOST) ? F.store : 1;
}
static void p_print(void *c, const char *line) { (void)c; f_print(line); }

static const sh_modpub_ops PUB_OPS = {
    NULL, p_on_main, p_row_state, p_rescan, p_activate, p_can_ask, p_ask, p_poll, p_release,
    p_worker_run, p_fetch, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, p_log,
    p_store_has, p_print
};

static void reset_fakes(void)
{
    memset(&F, 0, sizeof F);
    sh_modpub_test_reset();
    sh_modpub_test_set_ops(&PUB_OPS);
    sh_prefab_mods_test_reset();
    sh_prefab_mods_bind(&PM_OPS);
}

static void tick(int n) { while (n-- > 0) sh_modpublished_poll(); }

/* ---- fixtures -------------------------------------------------------------------------------- */

static const char DOOR_DECL[] = "{\r\n\tinherit = \"snapmaps/doors/industrial_door_vertical\";\r\n}";

/* An object prefab: one modded door, one stock prop. */
static const char OBJECT_PREFAB[] =
    "{\"cameraYaw\":0,\"entities\":["
    "{\"displayName\":\"Door\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/test.door/door\",\"name\":\"\"}},"
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

static int install_mod(const char *src, const char *id, const char *decl_rel, const char *decl, sh_modmanifest_row *row)
{
    char json[256], key[SH_MS_KEY_MAX + 1], cause[SH_MB_CAUSE_MAX];
    sh_ms_install_stats st;
    sh_mb_bundle b;
    int ok;
    _snprintf_s(json, sizeof json, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"1.0.0\",\"name\":\"%s\"}", id, id);
    mt_write(src, "mod.json", json);
    mt_write(src, decl_rel, decl);
    memset(&b, 0, sizeof b);
    ok = sh_ms_install_from_folder(src, NULL, key, sizeof key, &st, &b, cause, sizeof cause);
    if (!ok) fprintf(stderr, "install %s: %s\n", src, cause);
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
    CHECK(!sh_prefab_mods_valid_name("COM1") && !sh_prefab_mods_valid_name("lpt9.zip") && sh_prefab_mods_valid_name("com0"));
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
    /* Block and view: the view is the sidecar's shape, the summary goes under "summary". */
    {
        static const char VIEW[] = "{\"desc\":\"Hall\",\"tags\":[\"a\",3],\"kind\":\"modules\",\"moduleCount\":2,"
                                   "\"mods\":[{\"id\":\"x.y\",\"version\":\"1\",\"bundle\":\"" GHOST "\",\"size\":1,\"names\":[]}]}";
        char *b = sh_prefab_mods_block_compose(VIEW, sizeof VIEW - 1, NULL, &pl), *v;
        CHECK(b && !strcmp(b, "{\"format\":1,\"description\":\"Hall\",\"tags\":[\"a\"],\"mods\":[{\"id\":\"x.y\",\"version\":\"1\","
                              "\"bundle\":\"" GHOST "\",\"size\":1,\"names\":[]}],\"summary\":{\"kind\":\"modules\",\"moduleCount\":2}}"));
        v = b ? sh_prefab_mods_block_view(b, pl, NULL) : NULL;
        CHECK(v && !strcmp(v, "{\"desc\":\"Hall\",\"tags\":[\"a\"],\"kind\":\"modules\",\"moduleCount\":2,\"mods\":[{\"id\":\"x.y\","
                              "\"version\":\"1\",\"bundle\":\"" GHOST "\",\"size\":1,\"names\":[]}]}"));
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
            "{/*in editor display name*/\"displayName\":\"Door\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/test.door/door\",\"name\":\"\"}},"
            "{\"displayName\":\"Crate\",\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"snapmaps/props/new/crate01\",\"name\":\"\"}}],"
            "/*grab distance*/\"grabDistance\":256,\"~type\":\"idSnapEntityPrefab\"}";
        char p[MAX_PATH * 2], *text;
        size_t len = 0;
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\Commented.json", lib);
        CHECK(put_file(p, COMMENTED));
        CHECK(sh_prefab_mods_record(COMMENTED, sizeof COMMENTED - 1, p, NULL) == 1);
        text = slurp(p, &len);
        CHECK(text && !strstr(text, "/*") && strstr(block_of(text, len), "\"mods\":[{\"id\":\"test.door\""));
        CHECK(engine_bytes_are(p, OBJECT_PREFAB));
        free(text);
        DeleteFileA(p);
    }
}

static const char BLOCK_HEAD[] = "{\"format\":1,\"description\":\"\",\"tags\":[],\"mods\":[{\"id\":\"test.door\"";

static void recording(const char *lib, const sh_modmanifest_row *door)
{
    char prefab[MAX_PATH * 2], meta[MAX_PATH * 2], *view, *text;
    const char *blk;
    sh_modmanifest_list rows;
    size_t len = 0;
    int n, source;
    printf("-- Create writes the closure into the file's block; the engine's bytes stay as they were\n");
    n = sh_prefab_mods_closure(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows);
    CHECK(n == 1 && rows.count == 1 && !strcmp(rows.rows[0].bundle, door->bundle) && !strcmp(rows.rows[0].id, "test.door") &&
          !strcmp(rows.rows[0].version, "1.0.0") && rows.rows[0].size == door->size);
    CHECK(rows.count == 1 && rows.rows[0].name_count == 1 && !strcmp(rows.rows[0].names[0], "mods/test.door/door"));
    sh_modmanifest_list_free(&rows);
    CHECK(sh_prefab_mods_closure(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, &rows) == 0);
    sh_modmanifest_list_free(&rows);
    CHECK(sh_prefab_mods_closure("{\"entities\":[", 13, &rows) == -1);

    _snprintf_s(prefab, sizeof prefab, _TRUNCATE, "%s\\Door.json", lib);
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Door.meta.json", lib);
    CHECK(put_file(prefab, OBJECT_PREFAB));
    CHECK(sh_prefab_mods_record(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, prefab, &view) == 1);
    CHECK(view && strstr(view, "\"desc\":\"\"") && strstr(view, door->bundle));
    free(view);
    CHECK(engine_bytes_are(prefab, OBJECT_PREFAB) && !mt_exists(meta));
    text = slurp(prefab, &len);
    CHECK(text && len > sizeof OBJECT_PREFAB && !memcmp(text, OBJECT_PREFAB, sizeof OBJECT_PREFAB - 2));   /* engine keys first */
    blk = block_of(text, len);
    CHECK(!strncmp(blk, BLOCK_HEAD, sizeof BLOCK_HEAD - 1) && strstr(blk, door->bundle) &&
          !strstr(blk, "summary"));
    CHECK(sh_prefab_mods_rows_of(text, len, NULL, &rows) == 1 && !strcmp(rows.rows[0].bundle, door->bundle) &&
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
    CHECK(strstr(blk, "\"description\":\"Blast door\",\"tags\":[\"door\",\"x\"]") && strstr(blk, door->bundle) && !strstr(blk, "ignored"));
    CHECK(engine_bytes_are(prefab, OBJECT_PREFAB) && !mt_exists(meta));
    view = sh_prefab_mods_view_of(text, len, 0, meta, &source, NULL);
    CHECK(view && source == SH_PREFAB_META_BLOCK && strstr(view, "\"desc\":\"Blast door\"") && strstr(view, "\"mods\":["));
    free(view);
    free(text);
    /* Clearing both fields keeps the block and its rows. */
    CHECK(sh_prefab_mods_meta_write("", 0, prefab, meta) == 1);
    text = slurp(prefab, &len);
    blk = block_of(text, len);
    CHECK(strstr(blk, "\"description\":\"\",\"tags\":[]") && strstr(blk, door->bundle) && !mt_exists(meta));
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
            "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/test.door/door\",\"name\":\"\"}}],"
            "\"instances\":[{\"moduleName\":\"maps/modules/ind/stock_room.decl\"}],\"~type\":\"idSnapMap\"}";
        int count = 0;
        size_t fl = 0;
        char *file = sh_prefab_mods_module_file(MOD_WITH_DOOR, sizeof MOD_WITH_DOOR - 1,
                                                "{\"desc\":\"\",\"tags\":[],\"kind\":\"modules\",\"moduleCount\":1}", &fl, &view, &count);
        CHECK(file && count == 1 && sh_mp_is_module_file(file, fl));
        blk = block_of(file, fl);
        CHECK(strstr(blk, "\"summary\":{\"kind\":\"modules\",\"moduleCount\":1}") && strstr(blk, door->bundle));
        CHECK(view && strstr(view, "\"kind\":\"modules\"") && strstr(view, "\"moduleCount\":1") && strstr(view, door->bundle));
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

    printf("-- a prefab saved before the block reads its sidecar; the first edit moves it into the file\n");
    {
        static const char SIDE[] = "{\"desc\":\"Old hall\",\"tags\":[\"old\"],\"kind\":\"modules\",\"moduleCount\":3,"
                                   "\"mods\":[{\"id\":\"test.door\",\"version\":\"1.0.0\",\"bundle\":\"%s\",\"size\":1,\"names\":[\"mods/test.door/door\"]}]}";
        char p3[MAX_PATH * 2], m3[MAX_PATH * 2], side[600], *disk;
        size_t dl = 0;
        _snprintf_s(p3, sizeof p3, _TRUNCATE, "%s\\Old hall.json", lib);
        _snprintf_s(m3, sizeof m3, _TRUNCATE, "%s\\Old hall.meta.json", lib);
        _snprintf_s(side, sizeof side, _TRUNCATE, SIDE, door->bundle);
        CHECK(put_file(p3, OBJECT_PREFAB) && put_file(m3, side));
        text = slurp(p3, &len);
        view = sh_prefab_mods_view_of(text, len, 0, m3, &source, NULL);
        CHECK(view && source == SH_PREFAB_META_SIDECAR && !strcmp(view, side));
        free(view);
        CHECK(sh_prefab_mods_rows_of(text, len, m3, &rows) == 1);
        sh_modmanifest_list_free(&rows);
        view = sh_prefab_mods_view_of(text, len, 0, NULL, &source, NULL);
        CHECK(!view && source == SH_PREFAB_META_NONE);
        free(text);
        /* The first edit: the block takes the sidecar's details and the new description. */
        CHECK(sh_prefab_mods_meta_write("{\"desc\":\"New hall\",\"tags\":[\"old\"]}", 34, p3, m3) == 1);
        text = slurp(p3, &len);
        blk = block_of(text, len);
        CHECK(strstr(blk, "\"description\":\"New hall\"") && strstr(blk, door->bundle) &&
              strstr(blk, "\"summary\":{\"kind\":\"modules\",\"moduleCount\":3}"));
        view = sh_prefab_mods_view_of(text, len, 0, m3, &source, NULL);
        CHECK(view && source == SH_PREFAB_META_BLOCK && strstr(view, "New hall"));
        free(view);
        free(text);
        CHECK(engine_bytes_are(p3, OBJECT_PREFAB));
        /* The sidecar is left on disk, byte for byte. */
        disk = slurp(m3, &dl);
        CHECK(disk && !strcmp(disk, side));
        free(disk);
    }

    /* An old sidecar names no mods; a malformed one is refused. */
    CHECK(sh_prefab_mods_sidecar_rows("{\"desc\":\"x\",\"tags\":[]}", 22, &rows) == 0);
    CHECK(sh_prefab_mods_sidecar_rows("not json", 8, &rows) == -1);
    CHECK(sh_prefab_mods_sidecar_set("not json", 8, NULL, NULL) == NULL);
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
    strcpy_s(ghost.id, sizeof ghost.id, "ghost.mod");
    strcpy_s(ghost.version, sizeof ghost.version, "2.0");
    strcpy_s(ghost.bundle, sizeof ghost.bundle, GHOST);
    ghost.size = 1048576;
    CHECK(sh_modmanifest_list_add(&rows, door) && sh_modmanifest_list_add(&rows, &ghost));
    json = sh_prefab_mods_status_json(&rows, &missing);
    CHECK(json && missing == 1 && strstr(json, "\"count\":2,\"missing\":1") && strstr(json, "\"id\":\"test.door\"") &&
          strstr(json, "\"installed\":true") && strstr(json, "\"id\":\"ghost.mod\",\"version\":\"2.0\""));
    free(json);
    json = sh_prefab_mods_sidecar_set(NULL, 0, &rows, NULL);
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_MODS_STATUS;
    call.text = json;
    call.text_len = json ? strlen(json) : 0;
    call.out = out;
    call.out_cap = (int)sizeof out;
    CHECK(sh_prefab_mods_slot(&call) == 1 && strstr(out, "\"missing\":1"));
    free(json);
    sh_modmanifest_list_free(&rows);
}

static void rows_of(sh_modmanifest_list *rows, const sh_modmanifest_row *door, int with_ghost)
{
    sh_modmanifest_row ghost;
    memset(rows, 0, sizeof *rows);
    if (door) sh_modmanifest_list_add(rows, door);
    if (with_ghost) {
        memset(&ghost, 0, sizeof ghost);
        strcpy_s(ghost.id, sizeof ghost.id, "ghost.mod");
        strcpy_s(ghost.version, sizeof ghost.version, "2.0");
        strcpy_s(ghost.bundle, sizeof ghost.bundle, GHOST);
        ghost.size = 1048576;
        sh_modmanifest_row_add_name(&ghost, "maps/modules/mods/ghost.mod/room.decl");
        sh_modmanifest_list_add(rows, &ghost);
    }
}

static void resolving(const sh_modmanifest_row *door)
{
    sh_modmanifest_list rows;
    char msg[600];
    int seq = 0, r;

    printf("-- every mod installed: activated, attached to the open map once, then placed\n");
    reset_fakes();
    rows_of(&rows, door, 0);
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    CHECK(r == SH_PREFAB_MODS_PLACE && seq == 0 && F.activations == 1 && F.attaches == 1 && !strcmp(F.attached[0], door->bundle));
    CHECK(!strcmp(F.last_print, "Prefab Door uses test.door 1.0.0; this map did not list it, so it was added to the map "
                                "(the Mods tab's Remove from map takes it out)."));
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    CHECK(r == SH_PREFAB_MODS_PLACE && F.attaches == 1 && F.prints == 1);   /* the map lists it now */
    sh_modmanifest_list_free(&rows);
    /* No rows: placed as before, nothing asked. */
    memset(&rows, 0, sizeof rows);
    CHECK(sh_prefab_mods_resolve_main(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, &rows, "Crate", &seq, msg, sizeof msg) ==
          SH_PREFAB_MODS_PLACE && F.activations == 2);

    printf("-- the store has the missing mod: the prefab's question, Yes downloads, then it is ready\n");
    reset_fakes();
    F.store = 1;
    F.answer = 1;
    rows_of(&rows, door, 1);
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    CHECK(r == SH_PREFAB_MODS_HELD && seq > 0 && sh_prefab_mods_poll(seq) == 2);
    CHECK(!strcmp(msg, "This prefab needs 1 mod you do not have; checking the mod store."));
    CHECK(!strcmp(F.last_print, "Prefab Door needs 1 mod you do not have; checking the mod store."));
    CHECK(F.attaches == 0);   /* nothing reaches the map before the answer */
    tick(3);
    CHECK(F.asks == 1 && F.ask_buttons == 6 &&
          !strcmp(F.last_ask, "This prefab needs 1 mod: ghost.mod (1.0 MB). Download and install now?"));
    tick(3);
    CHECK(F.fetches == 1 && F.ghost_installed && sh_prefab_mods_poll(seq) == 1);
    CHECK(F.attaches == 2 && F.activations == 1 && F.main_calls == 1);   /* both rows reach the open map */
    CHECK(!sh_modpublished_holding());
    /* Placed again: everything is there now. */
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    CHECK(r == SH_PREFAB_MODS_PLACE && F.attaches == 2);
    sh_modmanifest_list_free(&rows);

    printf("-- the store lacks it: refused, naming the mod and the ZIP, never placed\n");
    reset_fakes();
    F.store = 0;
    rows_of(&rows, door, 1);
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    CHECK(r == SH_PREFAB_MODS_HELD);
    tick(3);
    CHECK(sh_prefab_mods_poll(seq) == 0 && F.fetches == 0 && F.attaches == 0);
    CHECK(!strcmp(F.last_print, "Prefab Door was not placed: it needs ghost.mod 2.0, which you do not have and the mod "
                                "store does not hold. Ask the prefab's author for its .zip export, which carries the mods."));
    CHECK(F.asks == 1 && F.ask_buttons == 0 &&
          !strcmp(F.last_ask, "This prefab uses ghost.mod, which is not installed. The prefab was not placed. Ask its "
                              "author for the .zip export."));
    sh_modmanifest_list_free(&rows);

    printf("-- the store cannot be reached: said so, nothing downloaded\n");
    reset_fakes();
    F.store = -1;
    rows_of(&rows, NULL, 1);
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    tick(3);
    CHECK(r == SH_PREFAB_MODS_HELD && sh_prefab_mods_poll(seq) == 0 && F.fetches == 0);
    CHECK(!strcmp(F.last_print, "Prefab Door was not placed: it needs ghost.mod 2.0, which you do not have, and the mod "
                                "store could not be reached. Try again, or ask the prefab's author for its .zip export, "
                                "which carries the mods."));
    CHECK(!strcmp(F.last_ask, "This prefab uses ghost.mod, which is not installed. The prefab was not placed. The mod store "
                              "could not be reached; try again, or ask its author for the .zip export."));
    sh_modmanifest_list_free(&rows);

    printf("-- No: nothing downloaded, the prefab is not placed\n");
    reset_fakes();
    F.store = 1;
    F.answer = 2;
    rows_of(&rows, NULL, 1);
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    tick(4);
    CHECK(r == SH_PREFAB_MODS_HELD && sh_prefab_mods_poll(seq) == 0 && F.fetches == 0 && F.attaches == 0);
    CHECK(F.asks == 2 && !strcmp(F.last_ask, "This prefab uses ghost.mod, which is not installed. The prefab was not placed."));
    sh_modmanifest_list_free(&rows);

    printf("-- no gates bound: refused with the console line and the toast, the store not asked\n");
    reset_fakes();
    sh_modpub_test_set_ops(NULL);
    rows_of(&rows, door, 1);
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    CHECK(r == SH_PREFAB_MODS_REFUSED && seq == 0 && F.attaches == 0);
    CHECK(!strcmp(F.last_print, "Prefab Door was not placed: it needs ghost.mod 2.0, which you do not have. Ask the "
                                "prefab's author for its .zip export, which carries the mods."));
    CHECK(F.toasts == 1 && !strcmp(F.last_toast, "This prefab was not placed. It needs mods you do not have: ghost.mod 2.0. "
                                                 "Ask the prefab's author for its .zip export."));
    CHECK(!strcmp(msg, F.last_toast));
    sh_modmanifest_list_free(&rows);

    printf("-- a module prefab: DLC no row provides is refused first; a module a row provides is a mod\n");
    reset_fakes();
    F.store = 1;
    rows_of(&rows, NULL, 1);
    {
        static const char DLC[] = MODULE_PREFAB("maps/modules/ind_dlc/dlc_room.decl");
        static const char MODDED[] = MODULE_PREFAB("maps/modules/mods/ghost.mod/room.decl");
        static const char STOCK[] = MODULE_PREFAB("maps/modules/ind/stock_room.decl");
        r = sh_prefab_mods_resolve_main(DLC, sizeof DLC - 1, &rows, "Rooms", &seq, msg, sizeof msg);
        CHECK(r == SH_PREFAB_MODS_NEEDS_MODULE && F.asks == 0 && !sh_modpublished_holding());
        CHECK(!strcmp(msg, "Not placed: this prefab needs module ind_dlc/dlc_room, which is not in your palette "
                           "(DLC or a mod you do not have)"));
        CHECK(!strcmp(F.last_print, "Module prefab: needs module maps/modules/ind_dlc/dlc_room.decl; this room is not in "
                                    "your palette (DLC or a mod you do not have)"));
        {
            /* The same with the product's block in the file: its strings are not modules. */
            static const char BLK[] = "{\"format\":1,\"summary\":{\"modules\":[{\"name\":\"maps/modules/x.decl\"}]}}";
            size_t bl = 0;
            char *with = sh_prefab_block_put(DLC, sizeof DLC - 1, BLK, sizeof BLK - 1, &bl);
            CHECK(with && sh_prefab_mods_resolve_main(with, bl, &rows, "Rooms", &seq, msg, sizeof msg) == SH_PREFAB_MODS_NEEDS_MODULE &&
                  strstr(msg, "ind_dlc/dlc_room") && !strstr(msg, "maps/modules/x"));
            free(with);
        }
        r = sh_prefab_mods_resolve_main(MODDED, sizeof MODDED - 1, &rows, "Rooms", &seq, msg, sizeof msg);
        CHECK(r == SH_PREFAB_MODS_HELD && sh_modpub_prefab_holding());
        printf("-- a second placement while one waits is refused\n");
        {
            int seq2 = 0;
            r = sh_prefab_mods_resolve_main(STOCK, sizeof STOCK - 1, &rows, "Other", &seq2, msg, sizeof msg);
            CHECK(r == SH_PREFAB_MODS_REFUSED && seq2 == 0 &&
                  !strcmp(msg, "Another prefab is waiting for its mods. Answer that first, then place this one."));
        }
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
    printf("-- the slot: rows from the block (else a sidecar); a prefab naming none is placed without asking the game\n");
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
    call.out = out;
    call.out_cap = (int)sizeof out;
    CHECK(sh_prefab_mods_slot(&call) == SH_PREFAB_MODS_PLACE && F.main_calls == 0);
    text = slurp(path, &len);   /* Door.json carries its block */
    call.text = text;
    call.text_len = len;
    call.meta_path = meta;
    call.name = "Door";
    CHECK(sh_prefab_mods_slot(&call) == SH_PREFAB_MODS_PLACE && F.main_calls == 1 && F.attaches == 1 &&
          !strcmp(F.attached[0], door->bundle));

    printf("-- META_READ: the view from the block, from a tail of the file, or from a sidecar\n");
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_META_READ;
    call.text = text;
    call.text_len = len;
    call.meta_path = meta;
    call.out = out;
    call.out_cap = (int)sizeof out;
    CHECK(sh_prefab_mods_slot(&call) == SH_PREFAB_META_BLOCK && strstr(out, "\"desc\":\"An airlock\"") && strstr(out, door->bundle));
    call.text = text + 40;
    call.text_len = len - 40;
    call.tail = 1;
    CHECK(sh_prefab_mods_slot(&call) == SH_PREFAB_META_BLOCK && strstr(out, "\"desc\":\"An airlock\""));
    call.text = OBJECT_PREFAB;
    call.text_len = sizeof OBJECT_PREFAB - 1;
    call.tail = 0;
    call.meta_path = none;
    CHECK(sh_prefab_mods_slot(&call) == SH_PREFAB_META_NONE && out[0] == 0);
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Old hall.meta.json", lib);
    call.meta_path = meta;
    CHECK(sh_prefab_mods_slot(&call) == SH_PREFAB_META_SIDECAR && strstr(out, "Old hall"));
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
        call.out = out;
        call.out_cap = (int)sizeof out;
        CHECK(sh_prefab_mods_slot(&call) == 1 && strstr(out, door->bundle));
        memset(&call, 0, sizeof call);
        call.op = SH_MODULE_PREFAB_META_WRITE;
        call.text = "{\"desc\":\"s\",\"tags\":[]}";
        call.text_len = strlen(call.text);
        call.path = p2;
        call.meta_path = none;
        CHECK(sh_prefab_mods_slot(&call) == 1 && !mt_exists(none));
        text = slurp(p2, &len);
        CHECK(strstr(block_of(text, len), "\"description\":\"s\"") && strstr(block_of(text, len), door->bundle));
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

static void zip(const char *tmp, const char *libA, const char *storeA, const sh_modmanifest_row *door)
{
    char zipA[MAX_PATH * 2], libB[MAX_PATH * 2], storeB[MAX_PATH * 2], prefab[MAX_PATH * 2], meta[MAX_PATH * 2];
    char msg[600], name[160], path[MAX_PATH * 2], cause[300];
    modexport_stats st;
    size_t len = 0, zl = 0, el = 0;
    char *text, *z, *disk;
    const char *e;
    FILETIME before_p, after_p, before_m, after_m;

    printf("-- export: always one ZIP, <name>.json as it is on disk and the mod's installed folder, nothing else\n");
    reset_fakes();
    _snprintf_s(zipA, sizeof zipA, _TRUNCATE, "%s\\Door export.zip", tmp);
    _snprintf_s(libB, sizeof libB, _TRUNCATE, "%s\\libB", tmp);
    _snprintf_s(storeB, sizeof storeB, _TRUNCATE, "%s\\storeB", tmp);
    _snprintf_s(prefab, sizeof prefab, _TRUNCATE, "%s\\Door.json", libA);
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Door.meta.json", libA);
    CHECK(sh_prefab_mods_export(prefab, meta, "Door", zipA, msg, sizeof msg) == 1);
    CHECK(strstr(msg, "Exported prefab Door with 1 mod to ") != NULL);
    z = slurp(zipA, &zl);
    disk = slurp(prefab, &len);
    {
        char mj[MAX_PATH * 2], *disk_mj;
        size_t ml = 0;
        e = zip_entry(z, zl, "Door.json", &el);
        CHECK(e && disk && el == len && !memcmp(e, disk, len));
        /* The mod as it is installed: mods/test.door/mod.json and its asset, byte for byte. */
        _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\test.door\\mod.json", storeA);
        disk_mj = slurp(mj, &ml);
        e = zip_entry(z, zl, "mods/test.door/mod.json", &el);
        CHECK(e && disk_mj && el == ml && !memcmp(e, disk_mj, ml));
        free(disk_mj);
        e = zip_entry(z, zl, "mods/test.door/assets/generated/decls/entitydef/mods/test.door/door.decl", &el);
        CHECK(e && el == sizeof DOOR_DECL - 1 && !memcmp(e, DOOR_DECL, el));
        CHECK(!memmem_find(z, zl, "bundle.canonical.json") && !memmem_find(z, zl, "bundle.json") && !memmem_find(z, zl, "mods/legacy/") &&
              !memmem_find(z, zl, "test.door@"));
        CHECK(!memmem_find(z, zl, "export.json") && !memmem_find(z, zl, "prefab.meta.json") && !memmem_find(z, zl, "prefab.json"));
    }
    free(z);
    free(disk);
    CHECK(sh_prefab_mods_export(prefab, meta, "Door", "C:\\x\\Door.json", msg, sizeof msg) == -1 &&
          strstr(msg, "must end in .zip"));
    /* A map's importer refuses it. */
    CHECK(modexport_read(zipA, &len, &st, cause, sizeof cause) == NULL && strstr(cause, "prefab's export, not a map's"));

    printf("-- a prefab without mods exports a ZIP too\n");
    {
        char p0[MAX_PATH * 2], z0[MAX_PATH * 2];
        _snprintf_s(p0, sizeof p0, _TRUNCATE, "%s\\Crate.json", libA);
        _snprintf_s(z0, sizeof z0, _TRUNCATE, "%s\\Crate.zip", tmp);
        CHECK(put_file(p0, STOCK_PREFAB));
        CHECK(sh_prefab_mods_record(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, p0, NULL) == 0);
        CHECK(sh_prefab_mods_export(p0, NULL, "Crate", z0, msg, sizeof msg) == 1 && strstr(msg, "it uses no mods"));
        z = slurp(z0, &zl);
        disk = slurp(p0, &len);
        e = zip_entry(z, zl, "Crate.json", &el);
        CHECK(e && disk && el == len && !memcmp(e, disk, len) && !memmem_find(z, zl, "mods/"));
        free(z); free(disk);
    }

    printf("-- the prefab importer refuses a map's ZIP\n");
    {
        static const char MAP[] = "{\"entities\":[],\"name\":\"idSnapMap\"}";
        char zm[MAX_PATH * 2];
        _snprintf_s(zm, sizeof zm, _TRUNCATE, "%s\\map.zip", tmp);
        CHECK(modexport_write(MAP, sizeof MAP - 1, zm, &st, cause, sizeof cause));
        CHECK(sh_prefab_mods_import(zm, libB, name, sizeof name, msg, sizeof msg) == -1 &&
              !strcmp(msg, "The prefab was not imported: this is a map's export, not a prefab's."));
    }

    printf("-- the prefab importer refuses the retired bundle form of earlier releases\n");
    {
        static const char *const OLD_N[] = { "Door.json", "mods/test.door@1.0.0+1/bundle.canonical.json" };
        const char *old_d[2];
        char zo[MAX_PATH * 2];
        old_d[0] = STOCK_PREFAB;
        old_d[1] = "{\"id\":\"test.door\"}";
        _snprintf_s(zo, sizeof zo, _TRUNCATE, "%s\\old form.zip", tmp);
        CHECK(mt_zip_stored(zo, 2, OLD_N, old_d));
        CHECK(sh_prefab_mods_import(zo, libB, name, sizeof name, msg, sizeof msg) == -1 &&
              !strcmp(msg, "The prefab was not imported: this export was made by an earlier release; export it again."));
    }

    printf("-- import into an empty store and library: the mod installed, the prefab added, no sidecar\n");
    CHECK(sh_ms_set_root(storeB));
    CHECK(sh_modserve_index_build(storeB) == 0);
    CHECK(sh_modserve_bundle_state(door->bundle, NULL, 0) == 0);
    CHECK(sh_prefab_mods_import(zipA, libB, name, sizeof name, msg, sizeof msg) == 1);
    CHECK(!strcmp(name, "Door") && !strcmp(msg, "Imported prefab Door: 1 mod installed, 0 already installed."));
    CHECK(sh_modserve_bundle_state(door->bundle, NULL, 0) == 1 && F.refreshes == 1);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libB);
    text = slurp(path, &len);
    disk = slurp(prefab, &el);
    CHECK(text && disk && len == el && !memcmp(text, disk, len));
    CHECK(text && strstr(block_of(text, len), "An airlock") && strstr(block_of(text, len), door->bundle));
    free(text); free(disk);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.meta.json", libB);
    CHECK(!mt_exists(path));

    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.door\\mod.json", storeB);
    CHECK(mt_exists(path));   /* installed under its own name, as a folder from elsewhere is */

    printf("-- importing it again writes nothing\n");
    {
        char mj[MAX_PATH * 2], pj[MAX_PATH * 2];
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door.json", libB);
        _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\test.door\\mod.json", storeB);
        CHECK(write_time(pj, &before_p) && write_time(mj, &before_m));
        Sleep(30);
        CHECK(sh_prefab_mods_import(zipA, libB, name, sizeof name, msg, sizeof msg) == 0);
        CHECK(!strcmp(msg, "Prefab Door and its mods are already here; nothing was changed.") && F.refreshes == 1);
        CHECK(write_time(pj, &after_p) && write_time(mj, &after_m));
        CHECK(CompareFileTime(&before_p, &after_p) == 0 && CompareFileTime(&before_m, &after_m) == 0);
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door (2).json", libB);
        CHECK(!mt_exists(pj));
    }

    printf("-- a different prefab of that name is kept; the import takes the next name\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libB);
    CHECK(put_file(path, STOCK_PREFAB));
    CHECK(sh_prefab_mods_import(zipA, libB, name, sizeof name, msg, sizeof msg) == 1 && !strcmp(name, "Door (2)"));
    text = slurp(path, &len);
    CHECK(text && !strcmp(text, STOCK_PREFAB));
    free(text);

    printf("-- an extracted export folder imports as the ZIP does\n");
    {
        char ex[MAX_PATH * 2], pj[MAX_PATH * 2], storeC[MAX_PATH * 2], libD[MAX_PATH * 2];
        _snprintf_s(ex, sizeof ex, _TRUNCATE, "%s\\Downloads\\Door export", tmp);
        _snprintf_s(storeC, sizeof storeC, _TRUNCATE, "%s\\storeC", tmp);
        _snprintf_s(libD, sizeof libD, _TRUNCATE, "%s\\libD", tmp);
        CHECK(mt_unzip_stored(zipA, ex) > 2);
        _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door.json", ex);
        CHECK(sh_ms_set_root(storeC));
        CHECK(sh_modserve_index_build(storeC) == 0);
        CHECK(sh_prefab_mods_import(pj, libD, name, sizeof name, msg, sizeof msg) == 1);
        CHECK(!strcmp(name, "Door") && !strcmp(msg, "Imported prefab Door: 1 mod installed, 0 already installed."));
        CHECK(sh_modserve_bundle_state(door->bundle, NULL, 0) == 1);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libD);
        text = slurp(path, &len);
        disk = slurp(prefab, &el);
        CHECK(text && disk && len == el && !memcmp(text, disk, len));
        free(text); free(disk);
        /* Without the mods folder: the prefab is added and the message says what it still needs. */
        CHECK(sh_ms_set_root(storeB));
        CHECK(sh_modserve_index_build(storeB) >= 0);
        F.ghost_installed = 0;
        {
            char lone[MAX_PATH * 2], *with;
            const char *side;
            size_t wl = 0;
            _snprintf_s(lone, sizeof lone, _TRUNCATE, "%s\\Ghost door.json", tmp);
            side = "{\"desc\":\"\",\"tags\":[],\"mods\":[{\"id\":\"ghost.mod\",\"version\":\"2.0\",\"bundle\":\"" GHOST
                   "\",\"size\":1,\"names\":[]}]}";
            with = sh_prefab_mods_embed(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, side, strlen(side), NULL, &wl);
            CHECK(with && sh_prefab_mods_write_file(lone, with, wl));
            free(with);
            CHECK(sh_prefab_mods_import(lone, libD, name, sizeof name, msg, sizeof msg) == 1);
            CHECK(!strcmp(name, "Ghost door") &&
                  !strcmp(msg, "Imported prefab Ghost door: 0 mods installed, 0 already installed. It also needs ghost.mod 2.0, "
                               "which the export does not carry."));
        }
    }

    printf("-- the console: mods, export and import of a ZIP and of a folder\n");
    {
        const char *mods_argv[] = { "mods", "Door" };
        const char *exp_argv[3];
        const char *imp_argv[2];
        char zip2[MAX_PATH * 2], libC[MAX_PATH * 2];
        sh_prefab_mods_set_root(libB);
        F.prints_all[0] = 0;
        mods_argv[1] = "Door (2)";
        sh_prefab_mods_command(2, mods_argv);
        CHECK(strstr(F.prints_all, "Prefab Door (2) needs 1 mod:") && strstr(F.prints_all, "test.door 1.0.0 (bundle ") &&
              strstr(F.prints_all, "): installed"));
        mods_argv[1] = "Door";
        sh_prefab_mods_command(2, mods_argv);
        CHECK(!strcmp(F.last_print, "Prefab Door records no mods."));
        mods_argv[1] = "Nope";
        sh_prefab_mods_command(2, mods_argv);
        CHECK(!strcmp(F.last_print, "sh_prefab mods: no prefab named \"Nope\"."));
        _snprintf_s(zip2, sizeof zip2, _TRUNCATE, "%s\\console.zip", tmp);
        exp_argv[0] = "export"; exp_argv[1] = "Door (2)"; exp_argv[2] = zip2;
        sh_prefab_mods_command(3, exp_argv);
        CHECK(strstr(F.last_print, "Exported prefab Door (2) with 1 mod to ") && mt_exists(zip2));
        z = slurp(zip2, &zl);
        CHECK(zip_entry(z, zl, "Door (2).json", &el) != NULL);
        free(z);
        _snprintf_s(libC, sizeof libC, _TRUNCATE, "%s\\libC", tmp);
        sh_prefab_mods_set_root(libC);
        imp_argv[0] = "import"; imp_argv[1] = zip2;
        sh_prefab_mods_command(2, imp_argv);
        CHECK(!strcmp(F.last_print, "Imported prefab Door (2): 0 mods installed, 1 already installed."));
        {
            char ex2[MAX_PATH * 2], pj[MAX_PATH * 2];
            _snprintf_s(ex2, sizeof ex2, _TRUNCATE, "%s\\console export", tmp);
            CHECK(mt_unzip_stored(zip2, ex2) > 2);
            _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s\\Door (2).json", ex2);
            imp_argv[1] = pj;
            sh_prefab_mods_command(2, imp_argv);
            CHECK(!strcmp(F.last_print, "Prefab Door (2) and its mods are already here; nothing was changed."));
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
        call.out = out;
        call.out_cap = (int)sizeof out;
        CHECK(sh_prefab_mods_slot(&call) == 0 && strstr(out, "{\"ok\":true,\"added\":false,\"name\":\"Door (2)\",\"message\":\""));
    }

    printf("-- a prefab whose row names a bundle that is not installed is not exported\n");
    CHECK(sh_ms_set_root(storeA));
    CHECK(sh_modserve_index_build(storeA) >= 0);
    {
        char p3[MAX_PATH * 2], z3[MAX_PATH * 2], *with;
        const char *side;
        size_t wl = 0;
        _snprintf_s(p3, sizeof p3, _TRUNCATE, "%s\\Ghost.json", libA);
        _snprintf_s(z3, sizeof z3, _TRUNCATE, "%s\\ghost.zip", tmp);
        side = "{\"desc\":\"\",\"tags\":[],\"mods\":[{\"id\":\"ghost.mod\",\"version\":\"2.0\",\"bundle\":\"" GHOST
               "\",\"size\":1,\"names\":[]}]}";
        with = sh_prefab_mods_embed(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, side, strlen(side), NULL, &wl);
        CHECK(with && sh_prefab_mods_write_file(p3, with, wl));
        free(with);
        CHECK(sh_prefab_mods_export(p3, NULL, "Ghost", z3, msg, sizeof msg) == -1 &&
              strstr(msg, "Prefab Ghost was not exported: the prefab needs ghost.mod@2.0") && strstr(msg, "not installed") &&
              !mt_exists(z3));
    }

    printf("-- a prefab saved before the block exports with one written for it; the file on disk is unchanged\n");
    {
        char p4[MAX_PATH * 2], m4[MAX_PATH * 2], z4[MAX_PATH * 2];
        _snprintf_s(p4, sizeof p4, _TRUNCATE, "%s\\Old door.json", libA);
        _snprintf_s(m4, sizeof m4, _TRUNCATE, "%s\\Old door.meta.json", libA);
        _snprintf_s(z4, sizeof z4, _TRUNCATE, "%s\\old.zip", tmp);
        CHECK(put_file(p4, OBJECT_PREFAB) && !mt_exists(m4));
        CHECK(sh_prefab_mods_export(p4, m4, "Old door", z4, msg, sizeof msg) == 1 && strstr(msg, "with 1 mod"));
        z = slurp(z4, &zl);
        e = zip_entry(z, zl, "Old door.json", &el);
        CHECK(e && strstr(block_of(e, el), door->bundle) && memmem_find(z, zl, door->bundle));
        free(z);
        text = slurp(p4, &len);
        CHECK(text && !strcmp(text, OBJECT_PREFAB));
        free(text);
        /* With a sidecar: its description travels in the exported block. */
        CHECK(put_file(m4, "{\"desc\":\"From the sidecar\",\"tags\":[\"old\"]}"));
        CHECK(sh_prefab_mods_export(p4, m4, "Old door", z4, msg, sizeof msg) == 1);
        z = slurp(z4, &zl);
        e = zip_entry(z, zl, "Old door.json", &el);
        CHECK(e && strstr(block_of(e, el), "\"description\":\"From the sidecar\"") && strstr(block_of(e, el), door->bundle));
        free(z);
    }
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], src[MAX_PATH * 2], storeA[MAX_PATH * 2], libA[MAX_PATH * 2];
    sh_modmanifest_row door;
    g_echo = argc > 1 && !strcmp(argv[argc - 1], "-v");

    CHECK(mt_temp_dir("prefabmods", tmp, sizeof tmp));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\src\\door", tmp);
    _snprintf_s(storeA, sizeof storeA, _TRUNCATE, "%s\\storeA", tmp);
    _snprintf_s(libA, sizeof libA, _TRUNCATE, "%s\\libA", tmp);
    CreateDirectoryA(libA, NULL);
    CHECK(sh_ms_set_root(storeA));
    CHECK(install_mod(src, "test.door", "assets\\generated\\decls\\entitydef\\mods\\test.door\\door.decl", DOOR_DECL, &door));
    CHECK(sh_modserve_index_build(storeA) >= 1);
    CHECK(sh_modserve_bundle_state(door.bundle, NULL, 0) == 1);
    reset_fakes();

    names();
    block();
    comments(libA);
    recording(libA, &door);
    status(&door);
    resolving(&door);
    resolve_through_slot(libA, &door);
    zip(tmp, libA, storeA, &door);

    sh_modpub_test_reset();
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    printf("prefab_mods_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
