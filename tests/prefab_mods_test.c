/* prefab_mods_test -- the mods a prefab needs (backend/prefab_mods.c).
 *
 * Over a real store and serving index (a mod built and installed into a
 * temporary folder), covered:
 *   recording: an object prefab's closure is the map save's (one row naming
 *     the bundle that provides its inherit, stock names nothing), written into
 *     its sidecar beside the description and tags; a prefab without mods
 *     writes no sidecar; a module prefab's summary sidecar gains the rows;
 *     the page's description writes keep the rows; an old sidecar names none;
 *   resolution through the real hold (modpublished.c, engine doubles): every
 *     mod installed (activated, attached to the open map with one console
 *     line, or left alone when the map lists it); the store has the missing
 *     one (the prefab's question, Yes downloads, the ready step, then ready);
 *     the store lacks it; the store cannot be reached; No; no gates bound;
 *     a DLC module no row provides; a second placement while one waits;
 *   the ZIP: export in the rawmap layout, refused by the map importer, imported
 *     into an empty store and library, imported again without a write, a
 *     differing prefab of the same name kept beside it, a prefab whose row
 *     names an uninstalled bundle refused, an old prefab exported from its
 *     closure; the console verbs and the slot's outputs; prefab names.
 * argv[1] is unused; -v echoes the backend log. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/prefab_mods.h"
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

static int f_bundle_state_plain(const char *hash)
{
    if (!strcmp(hash, GHOST)) return F.ghost_installed ? 1 : 0;
    return sh_modserve_bundle_state(hash, NULL, 0);
}
static int f_listed(const char *hash)
{
    int i;
    for (i = 0; i < F.listed_n; i++) if (!strcmp(F.listed[i], hash)) return 1;
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
static int f_activate(const char *const *hashes, int n) { (void)hashes; if (n > 0) F.activations++; return n; }
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
    f_bundle_state_plain, NULL, f_listed, f_attach, f_activate, f_module_known, f_print, f_toast, f_refresh, f_call_main
};

/* modpublished's doubles: the hold runs as in the game, its jobs inline. */
static int p_on_main(void *c) { (void)c; return 1; }
static int p_bundle_state(void *c, const char *hash, char *cause, size_t cap)
{
    (void)c;
    if (cause && cap) cause[0] = 0;
    return f_bundle_state_plain(hash) == 1 ? 1 : 0;
}
static int p_rescan(void *c) { (void)c; return 1; }
static int p_activate(void *c, const char *const *h, int n) { (void)c; (void)h; return n; }
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
    NULL, p_on_main, p_bundle_state, p_rescan, p_activate, p_can_ask, p_ask, p_poll, p_release,
    p_worker_run, p_fetch, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, p_log,
    p_store_has, NULL, p_print
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

static void recording(const char *lib, const sh_modmanifest_row *door)
{
    char prefab[MAX_PATH * 2], meta[MAX_PATH * 2], *side, *with;
    sh_modmanifest_list rows;
    size_t len = 0;
    int n;
    printf("-- saving records the closure in the sidecar, beside its description and tags\n");
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
    CHECK(put_file(meta, "{\"desc\":\"An airlock\",\"tags\":[\"door\"]}"));
    CHECK(sh_prefab_mods_record(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, meta, &side) == 1);
    CHECK(side && strstr(side, "\"desc\":\"An airlock\"") && strstr(side, "\"tags\":[\"door\"]") && strstr(side, door->bundle));
    free(side);
    side = slurp(meta, &len);
    CHECK(sh_prefab_mods_sidecar_rows(side, len, &rows) == 1 && !strcmp(rows.rows[0].bundle, door->bundle) &&
          rows.rows[0].name_count == 1);
    sh_modmanifest_list_free(&rows);
    free(side);

    /* The page writes description and tags without the rows: the rows stay. */
    {
        static const char BODY[] = "{\"desc\":\"Blast door\",\"tags\":[]}";
        CHECK(sh_prefab_mods_meta_write(BODY, sizeof BODY - 1, meta) == 1);
    }
    side = slurp(meta, &len);
    CHECK(side && strstr(side, "Blast door") && strstr(side, door->bundle));
    free(side);
    /* Clearing both fields keeps a sidecar that names the mods. */
    CHECK(sh_prefab_mods_meta_write("", 0, meta) == 1);
    side = slurp(meta, &len);
    CHECK(side && !strstr(side, "Blast door") && strstr(side, door->bundle));
    free(side);
    CHECK(put_file(meta, "{\"desc\":\"An airlock\",\"tags\":[\"door\"]}"));
    CHECK(sh_prefab_mods_record(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, meta, NULL) == 1);

    /* A prefab that needs no mods writes no sidecar; one saved again without
     * mods loses the rows and keeps the rest. */
    {
        char m2[MAX_PATH * 2];
        _snprintf_s(m2, sizeof m2, _TRUNCATE, "%s\\Crate.meta.json", lib);
        CHECK(sh_prefab_mods_record(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, m2, NULL) == 0 && !mt_exists(m2));
        CHECK(sh_prefab_mods_meta_write("", 0, m2) == 1 && !mt_exists(m2));
        CHECK(put_file(m2, "{\"desc\":\"c\",\"tags\":[],\"mods\":[{\"id\":\"x.y\",\"version\":\"1\",\"bundle\":\"" GHOST "\",\"size\":1,\"names\":[]}]}"));
        CHECK(sh_prefab_mods_record(STOCK_PREFAB, sizeof STOCK_PREFAB - 1, m2, NULL) == 0);
        side = slurp(m2, &len);
        CHECK(side && strstr(side, "\"desc\":\"c\"") && !strstr(side, "\"mods\""));
        free(side);
        DeleteFileA(m2);
    }

    /* A module prefab's summary sidecar gains the rows; the summary is kept. */
    {
        static const char MOD_WITH_DOOR[] =
            "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"mods/test.door/door\",\"name\":\"\"}}],"
            "\"instances\":[{\"moduleName\":\"maps/modules/ind/stock_room.decl\"}],\"~type\":\"idSnapMap\"}";
        int count = 0;
        with = sh_prefab_mods_with_closure("{\"desc\":\"\",\"tags\":[],\"kind\":\"modules\",\"moduleCount\":1}", MOD_WITH_DOOR,
                                           sizeof MOD_WITH_DOOR - 1, &count);
        CHECK(with && count == 1 && strstr(with, "\"kind\":\"modules\"") && strstr(with, "\"moduleCount\":1") && strstr(with, door->bundle));
        free(with);
        CHECK(sh_prefab_mods_with_closure("{}", STOCK_PREFAB, sizeof STOCK_PREFAB - 1, &count) == NULL && count == 0);
    }

    /* An old sidecar names no mods; a malformed one is refused, never overwritten. */
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
          !strcmp(F.last_ask, "This prefab was not placed. It needs mods you do not have and the mod store does not hold: "
                              "ghost.mod 2.0. Ask the prefab's author for its .zip export."));
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
    CHECK(strstr(F.last_ask, "This prefab was not placed.") && strstr(F.last_ask, "could not be reached"));
    sh_modmanifest_list_free(&rows);

    printf("-- No: nothing downloaded, the prefab is not placed\n");
    reset_fakes();
    F.store = 1;
    F.answer = 2;
    rows_of(&rows, NULL, 1);
    r = sh_prefab_mods_resolve_main(OBJECT_PREFAB, sizeof OBJECT_PREFAB - 1, &rows, "Door", &seq, msg, sizeof msg);
    tick(4);
    CHECK(r == SH_PREFAB_MODS_HELD && sh_prefab_mods_poll(seq) == 0 && F.fetches == 0 && F.attaches == 0);
    CHECK(F.asks == 2 && !strcmp(F.last_ask, "The prefab was not placed: it needs ghost.mod."));
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
    char out[700], meta[MAX_PATH * 2], none[MAX_PATH * 2];
    printf("-- the slot: rows from the sidecar; a prefab without one is placed without asking the game\n");
    reset_fakes();
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Door.meta.json", lib);
    _snprintf_s(none, sizeof none, _TRUNCATE, "%s\\Nothing.meta.json", lib);
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_MODS_RESOLVE;
    call.text = OBJECT_PREFAB;
    call.text_len = sizeof OBJECT_PREFAB - 1;
    call.meta_path = none;
    call.name = "Nothing";
    call.out = out;
    call.out_cap = (int)sizeof out;
    CHECK(sh_prefab_mods_slot(&call) == SH_PREFAB_MODS_PLACE && F.main_calls == 0);
    call.meta_path = meta;
    call.name = "Door";
    CHECK(sh_prefab_mods_slot(&call) == SH_PREFAB_MODS_PLACE && F.main_calls == 1 && F.attaches == 1 &&
          !strcmp(F.attached[0], door->bundle));
    reset_fakes();
}

static void zip(const char *tmp, const char *libA, const char *storeA, const sh_modmanifest_row *door)
{
    char zipA[MAX_PATH * 2], libB[MAX_PATH * 2], storeB[MAX_PATH * 2], prefab[MAX_PATH * 2], meta[MAX_PATH * 2];
    char msg[600], name[160], path[MAX_PATH * 2], cause[300];
    modexport_stats st;
    size_t len = 0, zl = 0;
    char *text, *z;
    FILETIME before_p, after_p, before_m, after_m;

    printf("-- export: the prefab, its sidecar and its mod in the rawmap export's layout\n");
    reset_fakes();
    _snprintf_s(zipA, sizeof zipA, _TRUNCATE, "%s\\Door export.zip", tmp);
    _snprintf_s(libB, sizeof libB, _TRUNCATE, "%s\\libB", tmp);
    _snprintf_s(storeB, sizeof storeB, _TRUNCATE, "%s\\storeB", tmp);
    _snprintf_s(prefab, sizeof prefab, _TRUNCATE, "%s\\Door.json", libA);
    _snprintf_s(meta, sizeof meta, _TRUNCATE, "%s\\Door.meta.json", libA);
    CHECK(sh_prefab_mods_export(prefab, meta, "Door", zipA, msg, sizeof msg) == 1);
    CHECK(strstr(msg, "Exported prefab Door with 1 mod to ") != NULL);
    z = slurp(zipA, &zl);
    {
        char entry[200];
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "mods/%s@%s+1/bundle.canonical.json", door->id, door->version);
        CHECK(z && zl > 0);
        /* stored entries: the names are in the local headers */
        CHECK(z && memmem_find(z, zl, "prefab.json") && memmem_find(z, zl, "prefab.meta.json") &&
              memmem_find(z, zl, "export.json") && memmem_find(z, zl, entry));
        CHECK(z && memmem_find(z, zl, "\"kind\":\"prefab\",\"name\":\"Door\",\"prefab\":\"prefab.json\""));
    }
    free(z);
    CHECK(sh_prefab_mods_export(prefab, meta, "Door", "C:\\x\\Door.json", msg, sizeof msg) == -1 &&
          strstr(msg, "must end in .zip"));
    /* A map's importer refuses it. */
    CHECK(modexport_read(zipA, &len, &st, cause, sizeof cause) == NULL && strstr(cause, "prefab's export, not a map's"));

    printf("-- import into an empty store and library: the mod installed, the prefab added\n");
    CHECK(sh_ms_set_root(storeB));
    CHECK(sh_modserve_index_build(storeB) == 0);
    CHECK(sh_modserve_bundle_state(door->bundle, NULL, 0) == 0);
    CHECK(sh_prefab_mods_import(zipA, libB, name, sizeof name, msg, sizeof msg) == 1);
    CHECK(!strcmp(name, "Door") && !strcmp(msg, "Imported prefab Door: 1 mod installed, 0 already installed."));
    CHECK(sh_modserve_bundle_state(door->bundle, NULL, 0) == 1 && F.refreshes == 1);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.json", libB);
    text = slurp(path, &len);
    CHECK(text && len == sizeof OBJECT_PREFAB - 1 && !memcmp(text, OBJECT_PREFAB, len));
    free(text);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\Door.meta.json", libB);
    text = slurp(path, &len);
    CHECK(text && strstr(text, "An airlock") && strstr(text, door->bundle));
    free(text);

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

    printf("-- the console: mods, export and import\n");
    {
        const char *mods_argv[] = { "mods", "Door" };
        const char *exp_argv[3];
        const char *imp_argv[2];
        char zip2[MAX_PATH * 2], libC[MAX_PATH * 2];
        sh_prefab_mods_set_root(libB);
        F.prints_all[0] = 0;
        sh_prefab_mods_command(2, mods_argv);
        CHECK(strstr(F.prints_all, "Prefab Door needs 1 mod:") && strstr(F.prints_all, "test.door 1.0.0 (bundle ") &&
              strstr(F.prints_all, "): installed"));
        mods_argv[1] = "Nope";
        sh_prefab_mods_command(2, mods_argv);
        CHECK(!strcmp(F.last_print, "sh_prefab mods: no prefab named \"Nope\"."));
        _snprintf_s(zip2, sizeof zip2, _TRUNCATE, "%s\\console.zip", tmp);
        exp_argv[0] = "export"; exp_argv[1] = "Door (2)"; exp_argv[2] = zip2;
        sh_prefab_mods_command(3, exp_argv);
        CHECK(strstr(F.last_print, "Exported prefab Door (2) with 1 mod to ") && mt_exists(zip2));
        _snprintf_s(libC, sizeof libC, _TRUNCATE, "%s\\libC", tmp);
        sh_prefab_mods_set_root(libC);
        imp_argv[0] = "import"; imp_argv[1] = zip2;
        sh_prefab_mods_command(2, imp_argv);
        CHECK(!strcmp(F.last_print, "Imported prefab Door (2): 0 mods installed, 1 already installed."));
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
        char m3[MAX_PATH * 2], p3[MAX_PATH * 2], z3[MAX_PATH * 2];
        _snprintf_s(p3, sizeof p3, _TRUNCATE, "%s\\Ghost.json", libA);
        _snprintf_s(m3, sizeof m3, _TRUNCATE, "%s\\Ghost.meta.json", libA);
        _snprintf_s(z3, sizeof z3, _TRUNCATE, "%s\\ghost.zip", tmp);
        CHECK(put_file(p3, STOCK_PREFAB));
        CHECK(put_file(m3, "{\"desc\":\"\",\"tags\":[],\"mods\":[{\"id\":\"ghost.mod\",\"version\":\"2.0\",\"bundle\":\"" GHOST
                           "\",\"size\":1,\"names\":[]}]}"));
        CHECK(sh_prefab_mods_export(p3, m3, "Ghost", z3, msg, sizeof msg) == -1 &&
              strstr(msg, "Prefab Ghost was not exported: the prefab needs ghost.mod@2.0") && strstr(msg, "not installed") &&
              !mt_exists(z3));
    }

    printf("-- a prefab saved before its mods were recorded exports what its entities use\n");
    {
        char p4[MAX_PATH * 2], m4[MAX_PATH * 2], z4[MAX_PATH * 2];
        _snprintf_s(p4, sizeof p4, _TRUNCATE, "%s\\Old door.json", libA);
        _snprintf_s(m4, sizeof m4, _TRUNCATE, "%s\\Old door.meta.json", libA);
        _snprintf_s(z4, sizeof z4, _TRUNCATE, "%s\\old.zip", tmp);
        CHECK(put_file(p4, OBJECT_PREFAB) && !mt_exists(m4));
        CHECK(sh_prefab_mods_export(p4, m4, "Old door", z4, msg, sizeof msg) == 1 && strstr(msg, "with 1 mod"));
        z = slurp(z4, &zl);
        CHECK(z && memmem_find(z, zl, door->bundle) && memmem_find(z, zl, "prefab.meta.json"));
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
