/* modpublish_test -- the publish side of the mod system (modpublish.c) with the engine replaced by fakes and the mod
 * store by a scripted one at the modclaim.h seam (modclaim_fake.c; modclaim itself runs against the Worker in
 * unit_roundtrip and the store's contract test):
 *   a map without mod rows publishes as the engine would; a map with rows is held at PublishStart while its content
 *   is prepared, under the pacifier; units the store holds go straight on; units it lacks are asked about once (S7),
 *   No leaves the map unpublished, Yes uploads with progress on the pacifier and then starts the engine's publish; the
 *   resumed publish's save pins its rows (and only that save); a refused upload, a retired client (410) and a key that
 *   cannot be made refuse with one sentence; a busy store lets the publish go on with its claim queued (N27); an
 *   object neither here nor in the store (S8) and one moderation removed are said after the publish; a mod that is not
 *   on this computer refuses the publish; the editor leaving abandons; the feed post claims the save's units for the
 *   item, a claim the store could not take is queued, a republish without mods and an unpublish release; a text over
 *   the service bound and an unreadable mod list are refused at PublishAfterSave; the search tag; the delete checks;
 *   the copy gate on both sides of the bound; the launch step.
 */
#include "heap_out.h"
#include "../src/backend/modpublish.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modnet.h"
#include "../src/backend/modcache.h"
#include "../src/backend/modpin.h"
#include "../src/backend/modstore.h"
#include "../src/backend/config.h"
#include "../src/backend/nav_bake.h"
#include "../src/backend/script/script_runtime.h"
#include "modclaim_fake.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static char g_log[512 * 1024];
static size_t g_log_n;
static int g_echo;
void backend_log(const char *line)
{
    size_t n = line ? strlen(line) : 0;
    if (g_echo && line) printf("    %s\n", line);
    if (n && g_log_n + n + 2 < sizeof g_log) { memcpy(g_log + g_log_n, line, n); g_log_n += n; g_log[g_log_n++] = '\n'; g_log[g_log_n] = 0; }
}

/* ------------------------------------------------------------ links the serving layer needs offline */
void sh_nav_bake_source_update_begin(void) {}
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped, unsigned char **out_bytes, size_t *out_len)
{ (void)name; (void)read_shipped; if (out_bytes) *out_bytes = NULL; if (out_len) *out_len = 0; return 0; }
int sh_nav_bake_overlay_open(const char *name, unsigned char **out, size_t *length)
{ (void)name; if (out) *out = NULL; if (length) *length = 0; return 0; }
int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{ (void)name; *out_bytes = NULL; *out_len = 0; return 0; }
int sh_navmesh_validate_aas(const unsigned char *p, size_t n, char *e, size_t cap)
{ (void)p; (void)n; if (e && cap) e[0] = 0; return 0; }
void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context) { (void)fn; (void)context; }
int modserve_activate_bundles(const char *const *bundle_hashes, int count) { (void)bundle_hashes; (void)count; return 0; }

/* ------------------------------------------------------------ the fake engine */

static struct {
    unsigned long long token;     /* the editor's live edit object */
    int fails, starts, asks, ask_buttons, answer, resumed_pass, overwrite;
    char asked[256], toast[256];
    int busy_on, busy_calls;
    char busy[256];
    char busy_seen[2048];         /* every pacifier line shown, one per line */
    /* the search tag: calls, what the fake returns, whether the fake model has it */
    int tag_calls, tag_result, tags_present, untag_calls;
    char tag[32];
    /* the published edit's saved-map ids, and the published-items queries */
    char cloud[64], local[64];
    int queries, query_start;
    unsigned query_ticket;
    char query_cloud[64];
} F;

static void *const EDITOR = (void *)(uintptr_t)0x1000;
static void *const MODEL = (void *)(uintptr_t)0x2000;
#define LOCAL_ID "LOCAL0001"

static void f_log(void *c, const char *line) { (void)c; backend_log(line); }
static int f_on_main(void *c) { (void)c; return 1; }
static int f_can_ask(void *c) { (void)c; return 1; }
static int f_ask(void *c, unsigned gdm, unsigned set, const char *text)
{
    (void)c; (void)gdm;
    F.asks++; F.ask_buttons = (int)set;
    strcpy_s(F.asked, sizeof F.asked, text);
    return 7;
}
static int f_poll(void *c, int ticket) { (void)c; (void)ticket; return F.answer; }
static void f_release(void *c, int ticket) { (void)c; (void)ticket; }
static unsigned long long f_token(void *c, void *editor) { (void)c; return editor == EDITOR ? F.token : 0; }
static void f_fail(void *c, void *editor) { (void)c; if (editor == EDITOR) F.fails++; }
/* The engine's PublishStart, through its detour: the hold passes it while resuming, then the search tag is armed. */
static void f_start(void *c, void *editor, unsigned char overwrite)
{
    (void)c;
    if (editor != EDITOR) return;
    F.starts++;
    F.overwrite = overwrite;
    F.resumed_pass = sh_modpublish_publish_start(editor, overwrite);
    if (F.resumed_pass) sh_modpublish_publish_started(LOCAL_ID);
}
static void f_busy(void *c, void *editor, const char *text)
{
    (void)c;
    if (editor != EDITOR) return;
    F.busy_calls++;
    F.busy_on = text != NULL;
    strcpy_s(F.busy, sizeof F.busy, text ? text : "");
    if (text && strlen(F.busy_seen) + strlen(text) + 2 < sizeof F.busy_seen) {
        strcat_s(F.busy_seen, sizeof F.busy_seen, text);
        strcat_s(F.busy_seen, sizeof F.busy_seen, "\n");
    }
}
static int f_placed_names(void *c, char ***names, size_t *count) { (void)c; *names = NULL; *count = 0; return 0; }
static int f_add_tag(void *c, void *model, const char *tag)
{
    (void)c;
    if (model != MODEL) return -1;
    F.tag_calls++;
    strcpy_s(F.tag, sizeof F.tag, tag);
    if (F.tag_result > 0 && F.tags_present) return 0;   /* already on the list */
    if (F.tag_result > 0) F.tags_present = 1;
    return F.tag_result;
}
static int f_remove_tag(void *c, void *model, const char *tag)
{
    (void)c;
    if (model != MODEL || strcmp(tag, SH_MODPUBLISH_TAG)) return -1;
    F.untag_calls++;
    if (!F.tags_present) return 0;
    F.tags_present = 0;
    return 1;
}
static int f_edit_ids(void *c, void *edit, char *cloud, size_t ccap, char *local, size_t lcap)
{
    (void)c; (void)edit;
    strcpy_s(cloud, ccap, F.cloud);
    strcpy_s(local, lcap, F.local);
    return F.cloud[0] || F.local[0];
}
static int f_published_query(void *c, const char *cloud, unsigned ticket)
{
    (void)c;
    F.queries++;
    F.query_ticket = ticket;
    strcpy_s(F.query_cloud, sizeof F.query_cloud, cloud);
    return F.query_start;
}
static void f_notice(void *c, void *editor, const char *text) { (void)c; (void)editor; strcpy_s(F.toast, sizeof F.toast, text); }
static char g_menu_notice[512];
static void f_menu_notice(void *c, const char *text) { (void)c; strcpy_s(g_menu_notice, sizeof g_menu_notice, text); }
static int f_window_open(void *c) { (void)c; return 1; }
static void set_ops(void)
{
    sh_modpublish_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.on_main = f_on_main; ops.can_ask = f_can_ask; ops.ask = f_ask; ops.poll = f_poll; ops.release = f_release;
    ops.editor_token = f_token; ops.publish_fail = f_fail; ops.start = f_start; ops.busy = f_busy;
    ops.placed_names = f_placed_names; ops.editor_notice = f_notice; ops.log = f_log;
    ops.add_tag = f_add_tag; ops.remove_tag = f_remove_tag; ops.edit_ids = f_edit_ids;
    ops.published_query = f_published_query; ops.notice = f_menu_notice;
    sh_modpublish_test_set_ops(&ops);
}

/* Tick until the condition holds (the worker runs meanwhile). */
#define PUMP_UNTIL(cond) do {                                                     \
    int pump_i_;                                                                  \
    for (pump_i_ = 0; pump_i_ < 3000 && !(cond); pump_i_++) { sh_modpublish_poll(); Sleep(5); } \
} while (0)

static int idle_all(void) { return !sh_modpublish_holding() && !sh_modpublish_test_background_busy(); }

/* ------------------------------------------------------------ the map's rows */

#define UNIT_T "1111111111111111111111111111111111111111111111111111111111111111"
#define UNIT_M "2222222222222222222222222222222222222222222222222222222222222222"
#define UNIT_W "3333333333333333333333333333333333333333333333333333333333333333"

/* What a save of the open map would write now (the publish asks before PublishStart). */
static sh_modmanifest_list g_rows;

static int t_rows_for(const char *const *names, size_t count, sh_modmanifest_list *out)
{
    size_t i;
    (void)names; (void)count;
    memset(out, 0, sizeof *out);
    for (i = 0; i < g_rows.count; i++) if (!sh_modmanifest_list_add(out, &g_rows.rows[i])) return -1;
    return (int)out->count;
}

/* A pinned row of Eternal Demons placing the Tyrant (and the Marauder), as a downloaded or published map has it. */
static void rows_pinned(int with_marauder)
{
    sh_modmanifest_row row;
    const char *roots[2] = { "entities/ai/demon/tyrant", "entities/ai/demon/marauder" };
    const char *units[2] = { UNIT_T, UNIT_M };
    sh_modmanifest_list_free(&g_rows);
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, "eterndem");
    sh_modmanifest_row_set_name(&row, "Eternal Demons");
    sh_modmanifest_row_add_name(&row, "mods/eterndem/entities/ai/demon/tyrant");
    if (with_marauder) sh_modmanifest_row_add_name(&row, "mods/eterndem/entities/ai/demon/marauder");
    CHECK(sh_modmanifest_row_pin(&row, roots, units, with_marauder ? 2 : 1));
    CHECK(sh_modmanifest_list_add(&g_rows, &row));
    sh_modmanifest_row_free(&row);
}

/* A local row of a mod whose folder this computer does not have. */
static void rows_local(void)
{
    sh_modmanifest_row row;
    sh_modmanifest_list_free(&g_rows);
    memset(&row, 0, sizeof row);
    strcpy_s(row.salt, sizeof row.salt, "hellguar");
    strcpy_s(row.build, sizeof row.build, SH_MODMANIFEST_LOCAL_BUILD);
    sh_modmanifest_row_set_name(&row, "Hell Guards");
    sh_modmanifest_row_add_name(&row, "mods/hellguar/entities/ai/hellguard");
    CHECK(sh_modmanifest_list_add(&g_rows, &row));
    sh_modmanifest_row_free(&row);
}

static char *no_folder(void *context, const struct sh_modmanifest_row *row) { (void)context; (void)row; return NULL; }

static const char EMPTY_MAP[] = "{\"name\":\"t\",\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";

/* The saved text of the rows as they are (HeapFree). */
static char *rows_text(const sh_modmanifest_list *rows, size_t *len)
{
    char err[192];
    char *text = sh_modmanifest_write(EMPTY_MAP, strlen(EMPTY_MAP), rows, len, err, sizeof err);
    if (!text) fprintf(stderr, "rows_text: %s\n", err);
    return text;
}

/* The engine after the resume: the Save state's save (the pin step, the save observer, the metadata write), then the
 * Save state's callback. Returns what PublishAfterSave's gate said. */
static int engine_save(void)
{
    sh_modmanifest_list rows;
    char *text;
    size_t len = 0;
    int pass;
    memset(&rows, 0, sizeof rows);
    (void)t_rows_for(NULL, 0, &rows);
    text = rows_text(&rows, &len);
    (void)sh_modpublish_pin_save(&rows, text, len);
    sh_modpublish_save_seen(text, len);
    (void)sh_modpublish_save_tag(LOCAL_ID, MODEL);
    pass = sh_modpublish_after_save(EDITOR);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    sh_modmanifest_list_free(&rows);
    return pass;
}

/* The product record holds `needle` in a section (canonical JSON). */
static int section_contains(const char *store, const char *section, const char *needle)
{
    char *text = NULL;
    int hit = sh_mb_record_get(store, section, &text) == 1 && text && strstr(text, needle) != NULL;
    free(text);
    return hit;
}

static int make_sized(const char *path, unsigned long long size)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER at;
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    at.QuadPart = (LONGLONG)size;
    ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

/* One publish of the rows in g_rows through the hold, answering `answer` if asked; returns with the hold idle. */
static void publish(int answer)
{
    F.answer = answer;
    CHECK(sh_modpublish_publish_start(EDITOR, 1) == 0);
    PUMP_UNTIL(!sh_modpublish_holding());
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], store[MAX_PATH * 2], cache[MAX_PATH * 2], path[MAX_PATH * 2];
    wchar_t wtmp[MAX_PATH * 2];
    g_echo = argc > 1 && !strcmp(argv[1], "-v");

    CHECK(mt_temp_dir("modpublish", tmp, sizeof tmp));
    MultiByteToWideChar(CP_ACP, 0, tmp, -1, wtmp, MAX_PATH * 2);
    sh_config_test_set_local_appdata(wtmp);
    CHECK(sh_config_init() == 1);
    _snprintf_s(store, sizeof store, _TRUNCATE, "%s\\mods", tmp);
    _snprintf_s(cache, sizeof cache, _TRUNCATE, "%s\\published-maps", tmp);
    CreateDirectoryA(cache, NULL);
    CHECK(sh_ms_set_root(store));
    CHECK(modnet_set_base_url("http://127.0.0.1:9/"));   /* nothing here reaches it: the store is the fake */
    sh_modpublish_test_set_cache_root(cache);
    sh_modpublish_test_set_rows_for(t_rows_for);
    sh_modpin_set_folder(no_folder, NULL);
    set_ops();
    fc_reset();
    memset(&F, 0, sizeof F);
    F.token = 0xED17;
    F.tag_result = 1;
    F.query_start = 1;

    printf("-- the sentences\n");
    {
        char q[256];
        sh_modpublish_compose_question(q, sizeof q, 212ull * 1024 * 1024);
        CHECK(!strcmp(q, "Publishing uploads 212 MB of mod content that the mod store does not have yet. Publish?"));
        sh_modpublish_compose_question(q, sizeof q, 300ull * 1024);
        CHECK(!strcmp(q, "Publishing uploads 1 MB of mod content that the mod store does not have yet. Publish?"));
        sh_modpublish_progress_text(q, sizeof q, 84ull * 1024 * 1024, 212ull * 1024 * 1024);
        CHECK(!strcmp(q, "Uploading mod content: 84 of 212 MB."));
        sh_modpublish_progress_text(q, sizeof q, 0, 0);
        CHECK(!strcmp(q, "Preparing this map's mod content."));
        sh_modpublish_size_text(q, sizeof q, 512);
        CHECK(!strcmp(q, "512 bytes"));
    }

    printf("-- a map without mod rows publishes as the engine would\n");
    {
        sh_modmanifest_list_free(&g_rows);
        CHECK(sh_modpublish_publish_start(EDITOR, 0) == 1);
        CHECK(!sh_modpublish_holding() && F.busy_calls == 0 && g_fc.plans == 0 && !sh_modpublish_test_pin_armed());
        sh_modpublish_save_seen(EMPTY_MAP, strlen(EMPTY_MAP));
        CHECK(sh_modpublish_after_save(EDITOR) == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "r1");
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.claims == 0 && g_fc.releases == 0);
    }

    printf("-- units the store holds: held under the pacifier, then the engine's publish; its save pins\n");
    {
        size_t mark = g_log_n;
        rows_pinned(1);
        F.busy_seen[0] = 0;
        CHECK(sh_modpublish_publish_start(EDITOR, 1) == 0);
        CHECK(sh_modpublish_holding() && sh_modpublish_test_state() == 1 && F.starts == 0);
        CHECK(F.busy_on && !strcmp(F.busy, "Preparing this map's mod content."));
        CHECK(sh_modpublish_publish_start(EDITOR, 1) == 0 && F.starts == 0);   /* a second Publish waits */
        PUMP_UNTIL(!sh_modpublish_holding());
        CHECK(F.starts == 1 && F.resumed_pass == 1 && F.overwrite == 1 && !F.busy_on && F.asks == 0);
        CHECK(g_fc.plans == 1 && g_fc.uploads == 0 && g_fc.signers >= 1);
        CHECK(strstr(g_log + mark, "2 unit(s): 2 held by the store, 0 to upload") != NULL);
        CHECK(sh_modpublish_test_pin_armed());
        CHECK(engine_save() == 1);
        CHECK(!sh_modpublish_test_pin_armed());
        CHECK(sh_modpublish_test_context_units() == 2);
        CHECK(F.tag_calls == 1 && F.tags_present);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "rev1");
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.claims == 1 && !strcmp(g_fc.claim_ref, "aaaaaaaaaaaaaaaaaaaaaaa1") && g_fc.claim_n == 2);
        CHECK(!strcmp(g_fc.claim_units[0], UNIT_T) && !strcmp(g_fc.claim_units[1], UNIT_M));
        CHECK(g_menu_notice[0] == 0);
        /* only the resumed publish's save pins: a later plain save does not */
        CHECK(sh_modpublish_pin_save(&g_rows, EMPTY_MAP, strlen(EMPTY_MAP)) == 0);
    }

    printf("-- units the store lacks: asked once; No leaves the map unpublished\n");
    {
        int starts = F.starts, fails = F.fails;
        fc_set_unit(UNIT_M, MODCLAIM_MISSING, 212ull * 1024 * 1024);
        F.toast[0] = 0;
        publish(2);   /* declined */
        CHECK(F.asks == 1 && F.ask_buttons == 6);
        CHECK(!strcmp(F.asked, "Publishing uploads 212 MB of mod content that the mod store does not have yet. Publish?"));
        CHECK(F.starts == starts && F.fails == fails && !F.busy_on && F.toast[0] == 0);
        CHECK(g_fc.uploads == 0 && !sh_modpublish_test_pin_armed());
        CHECK(strstr(g_log, "not started: the author chose not to upload") != NULL);
    }

    printf("-- Yes uploads with progress on the pacifier, then the engine's publish\n");
    {
        int starts = F.starts;
        F.busy_seen[0] = 0;
        g_fc.upload_delay_ms = 400;
        publish(1);
        CHECK(F.asks == 2 && g_fc.uploads == 1 && g_fc.uploaded_bytes == 212ull * 1024 * 1024);
        CHECK(strstr(F.busy_seen, "Uploading mod content: 0 of 212 MB.\n") != NULL);
        CHECK(strstr(F.busy_seen, "of 212 MB.\n") != NULL);
        CHECK(F.starts == starts + 1 && !F.busy_on);
        CHECK(engine_save() == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "rev2");
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.claims == 2 && g_fc.claim_n == 2);
        g_fc.upload_delay_ms = 0;
        /* held now: the next publish asks nothing */
        publish(0);
        CHECK(F.asks == 2 && F.starts == starts + 2);
        CHECK(engine_save() == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "rev3");
        PUMP_UNTIL(idle_all());
    }

    printf("-- a refused upload refuses the publish with the store's sentence\n");
    {
        int starts = F.starts;
        fc_set_unit(UNIT_M, MODCLAIM_MISSING, 5ull * 1024 * 1024);
        g_fc.upload_code = MODNET_E_REFUSED;
        strcpy_s(g_fc.why, sizeof g_fc.why, "This mod content is over the mod store's size limit for one map.");
        publish(1);
        CHECK(F.starts == starts && !F.busy_on);
        CHECK(!strcmp(F.toast, "The map was not published. This mod content is over the mod store's size limit for one map."));
        g_fc.why[0] = 0;
    }

    printf("-- a busy store past the wait: the publish goes on, its claim queued, one notice after\n");
    {
        int starts = F.starts;
        g_menu_notice[0] = 0;
        g_fc.upload_code = MODNET_E_UNAVAILABLE;
        publish(1);
        CHECK(F.starts == starts + 1);
        CHECK(!strcmp(g_menu_notice, "The map is published, but its mod content could not reach the mod store yet; "
                                     "Snapmap+ sends it the next time DOOM starts."));
        CHECK(engine_save() == 1);
        g_fc.claim_code = MODNET_E_UNAVAILABLE;
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "bbbbbbbbbbbbbbbbbbbbbbb2", "rev1");
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.queued == 1 && !strcmp(g_fc.queued_ref, "bbbbbbbbbbbbbbbbbbbbbbb2") && g_fc.queued_n == 2);
        CHECK(strstr(g_log, "(queued; retried at the next start)") != NULL);
        /* the store did not answer the preparation either: the same */
        g_menu_notice[0] = 0;
        g_fc.plan_code = MODNET_E_TIMEOUT;
        publish(0);
        CHECK(F.starts == starts + 2 && strstr(g_menu_notice, "could not reach the mod store yet") != NULL);
        CHECK(engine_save() == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "bbbbbbbbbbbbbbbbbbbbbbb2", "rev2");
        PUMP_UNTIL(idle_all());
        fc_set_unit(UNIT_M, MODCLAIM_HELD, 0);
    }

    printf("-- a retired client and a key that cannot be made refuse with one sentence\n");
    {
        int starts = F.starts;
        g_fc.plan_code = MODNET_E_RETIRED;
        publish(0);
        CHECK(F.starts == starts && !strcmp(F.toast, MODNET_RETIRED_SENTENCE));
        g_fc.signer_fails = 1;
        publish(0);
        CHECK(F.starts == starts &&
              !strcmp(F.toast, "The map was not published: Snapmap+ could not make this computer's publishing key."));
    }

    printf("-- an object neither here nor in the store, and one moderation removed: said after the publish\n");
    {
        int starts = F.starts;
        g_menu_notice[0] = 0;
        fc_set_unit(UNIT_T, MODCLAIM_ABSENT, 0);
        publish(0);
        CHECK(F.starts == starts + 1);
        CHECK(!strcmp(g_menu_notice, "Players will see this map without the Tyrant from Eternal Demons: its content is "
                                     "not on this computer or in the mod store."));
        CHECK(engine_save() == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "ccccccccccccccccccccccc3", "rev1");
        PUMP_UNTIL(idle_all());
        g_menu_notice[0] = 0;
        fc_set_unit(UNIT_T, MODCLAIM_REMOVED, 0);
        publish(0);
        CHECK(!strcmp(g_menu_notice, "Players will see this map without the Tyrant from Eternal Demons, which was "
                                     "removed from the mod store."));
        CHECK(engine_save() == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "ccccccccccccccccccccccc3", "rev2");
        PUMP_UNTIL(idle_all());
        fc_set_unit(UNIT_T, MODCLAIM_HELD, 0);
    }

    printf("-- a mod that is not on this computer refuses the publish\n");
    {
        int starts = F.starts, plans = g_fc.plans;
        rows_local();
        publish(0);
        CHECK(F.starts == starts && g_fc.plans == plans && !F.busy_on);
        CHECK(!strcmp(F.toast, "The map was not published: Snapmap+ cannot find the mod Hell Guards on this computer. "
                               "Build or install it in the Mods tab, or remove its objects, then publish again."));
        rows_pinned(1);
    }

    printf("-- the editor leaving abandons the held publish\n");
    {
        int starts = F.starts;
        fc_set_unit(UNIT_M, MODCLAIM_MISSING, 1024 * 1024);
        F.answer = 0;   /* the question stays open */
        CHECK(sh_modpublish_publish_start(EDITOR, 1) == 0);
        PUMP_UNTIL(sh_modpublish_test_state() == 3);
        F.token = 0;
        sh_modpublish_poll();
        CHECK(!sh_modpublish_holding() && F.starts == starts && strstr(g_log, "abandoned: the editor left the map"));
        F.token = 0xED18;
        PUMP_UNTIL(idle_all());
        fc_set_unit(UNIT_M, MODCLAIM_HELD, 0);
    }

    printf("-- PublishAfterSave refuses a text over the service bound and an unreadable mod list\n");
    {
        static const char BAD[] = "{\"name\":\"t\",\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
            "\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"NOT A SALT\",\"name\":\"Hell Guards\",\"size\":1,"
            "\"names\":[]}]}}";
        size_t len = 0, big_len = (size_t)SH_MODPUBLISH_TEXT_BOUND + 4096;
        char *text = rows_text(&g_rows, &len), *big = malloc(big_len + 1);
        int fails = F.fails;
        sh_modpublish_save_seen(BAD, strlen(BAD));
        CHECK(sh_modpublish_after_save(EDITOR) == 0 && F.fails == fails + 1);
        CHECK(!strcmp(F.toast, "The map was not published: Snapmap+ cannot read the mod Hell Guards in its list. "
                               "Remove that mod in the Mods tab, save, and publish again."));
        CHECK(text && big);
        if (text && big) {
            memset(big, ' ', big_len);
            memcpy(big, text, len - 1);
            big[big_len - 1] = '}';
            big[big_len] = 0;
            sh_modpublish_save_seen(big, big_len);
            CHECK(sh_modpublish_after_save(EDITOR) == 0 && F.fails == fails + 2);
            CHECK(strstr(F.toast, "over the map service's 10.0 MB limit by 4096 bytes") != NULL);
        }
        free(big);
        if (text) HeapFree(GetProcessHeap(), 0, text);
        /* the bound counts the snapmap-plus member: a map with navigation and no mods */
        {
            static const char head[] = "{\"name\":\"t\",\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},"
                                       "\"snapmap-plus\":{\"format\":2,\"nav\":[{\"data\":[\"";
            static const char tail[] = "\"]}]}}";
            size_t n2 = (size_t)SH_MODPUBLISH_TEXT_BOUND + 100;
            char *nav = malloc(n2 + 1);
            CHECK(nav != NULL);
            if (nav) {
                memset(nav, 'A', n2);
                memcpy(nav, head, sizeof head - 1);
                memcpy(nav + n2 - (sizeof tail - 1), tail, sizeof tail - 1);
                nav[n2] = 0;
                sh_modpublish_save_seen(nav, n2);
                CHECK(sh_modpublish_after_save(EDITOR) == 0 && F.fails == fails + 3);
                CHECK(strstr(F.toast, "over the map service's 10.0 MB limit by 100 bytes") != NULL);
                /* without the member the text is the engine's, and so is the check */
                memset(nav, ' ', n2);
                memcpy(nav, EMPTY_MAP, sizeof EMPTY_MAP - 2);
                nav[n2 - 1] = '}';
                nav[n2] = 0;
                sh_modpublish_save_seen(nav, n2);
                CHECK(sh_modpublish_after_save(EDITOR) == 1 && F.fails == fails + 3);
            }
            free(nav);
        }
        F.fails = fails;
    }

    printf("-- a republish without mods and an unpublish release the item's claim\n");
    {
        int releases = g_fc.releases;
        CHECK(modclaim_known("aaaaaaaaaaaaaaaaaaaaaaa1"));
        sh_modmanifest_list_free(&g_rows);
        CHECK(sh_modpublish_publish_start(EDITOR, 1) == 1);
        sh_modpublish_save_seen(EMPTY_MAP, strlen(EMPTY_MAP));
        CHECK(sh_modpublish_after_save(EDITOR) == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "rev4");
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.releases == releases + 1 && !modclaim_known("aaaaaaaaaaaaaaaaaaaaaaa1"));
        sh_modpublish_unpublished("ccccccccccccccccccccccc3");
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.releases == releases + 2 && !strcmp(g_fc.release_ref, "ccccccccccccccccccccccc3"));
        /* an item this computer never claimed: nothing is sent */
        sh_modpublish_unpublished("ddddddddddddddddddddddd4");
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.releases == releases + 2);
        /* a release the store could not take is queued */
        g_fc.release_code = MODNET_E_NET;
        sh_modpublish_unpublished("bbbbbbbbbbbbbbbbbbbbbbb2");
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.queued == 2 && !strcmp(g_fc.queued_ref, "bbbbbbbbbbbbbbbbbbbbbbb2") && g_fc.queued_n == 0);
        rows_pinned(1);
    }

    printf("-- delete from My Maps releases a claim only when its item is no longer published\n");
    {
        const char *still[2] = { "ggggggggggggggggggggggg7", "zzzzzzzzzzzzzzzzzzzzzzz9" };
        const char *other[1] = { "zzzzzzzzzzzzzzzzzzzzzzz9" };
        int queries, releases;
        strcpy_s(F.cloud, sizeof F.cloud, "cloud-g");
        strcpy_s(F.local, sizeof F.local, "local-g");
        publish(0);
        CHECK(engine_save() == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "ggggggggggggggggggggggg7", "rev1");
        PUMP_UNTIL(idle_all());
        CHECK(modclaim_known("ggggggggggggggggggggggg7"));
        CHECK(section_contains(store, "links", "\"ggggggggggggggggggggggg7\":{\"at\":") &&
              section_contains(store, "links", ",\"cloud\":\"cloud-g\",\"local\":\"local-g\"}"));
        F.cloud[0] = 0;
        strcpy_s(F.local, sizeof F.local, "local-h");
        publish(0);
        CHECK(engine_save() == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "hhhhhhhhhhhhhhhhhhhhhhh8", "rev1");
        PUMP_UNTIL(idle_all());
        CHECK(section_contains(store, "links", ",\"local\":\"local-h\"}"));
        F.local[0] = 0;

        queries = F.queries;
        sh_modpublish_deleted("local-g", "cloud-g");
        sh_modpublish_poll();
        CHECK(F.queries == queries && sh_modpublish_test_delete_checks() == 0);
        CHECK(strstr(g_log, "published less than 60 minute(s) ago") != NULL);
        sh_modpublish_test_set_link_min_age(0);

        sh_modpublish_deleted("local-x", "cloud-x");
        CHECK(F.queries == queries && sh_modpublish_test_delete_checks() == 0);
        sh_modpublish_deleted("local-h", "");
        CHECK(F.queries == queries && strstr(g_log, "no cloud-saved id") != NULL);

        sh_modpublish_deleted("local-g", "cloud-g");
        CHECK(F.queries == queries && sh_modpublish_test_delete_checks() == 1);
        sh_modpublish_poll();
        CHECK(F.queries == queries + 1 && !strcmp(F.query_cloud, "cloud-g"));
        sh_modpublish_published_result(F.query_ticket, 1, still, 2);
        sh_modpublish_poll();
        PUMP_UNTIL(idle_all());
        CHECK(sh_modpublish_test_delete_checks() == 0 && modclaim_known("ggggggggggggggggggggggg7"));
        CHECK(strstr(g_log, "0 claim(s) released, 1 kept (still published)") != NULL);
        {
            const char *page[30];
            for (int i = 0; i < 30; i++) page[i] = "zzzzzzzzzzzzzzzzzzzzzzz9";
            sh_modpublish_deleted("", "cloud-g");
            sh_modpublish_poll();
            sh_modpublish_published_result(F.query_ticket, 1, page, 30);
            sh_modpublish_poll();
            CHECK(strstr(g_log, "answered a full page of 30 published item(s); 1 claim(s) kept") != NULL);
        }
        sh_modpublish_deleted("", "cloud-g");
        sh_modpublish_poll();
        sh_modpublish_published_result(F.query_ticket, 0, NULL, 0);
        sh_modpublish_poll();
        CHECK(sh_modpublish_test_delete_checks() == 0 && strstr(g_log, "check for 'cloud-g' failed") != NULL);
        sh_modpublish_deleted("", "cloud-g");
        sh_modpublish_poll();
        sh_modpublish_test_age_delete_checks(SH_MODPUBLISH_DELETE_CHECK_MS + 1000);
        sh_modpublish_poll();
        CHECK(sh_modpublish_test_delete_checks() == 0 && strstr(g_log, "no published-items answer for 'cloud-g'") != NULL);
        F.query_start = 0;
        sh_modpublish_deleted("", "cloud-g");
        sh_modpublish_poll();
        CHECK(sh_modpublish_test_delete_checks() == 0 && strstr(g_log, "could not start") != NULL);
        F.query_start = 1;
        PUMP_UNTIL(idle_all());
        CHECK(modclaim_known("ggggggggggggggggggggggg7"));

        releases = g_fc.releases;
        sh_modpublish_deleted("local-g", "cloud-g");
        sh_modpublish_poll();
        sh_modpublish_published_result(F.query_ticket, 1, other, 1);
        PUMP_UNTIL(idle_all() && !modclaim_known("ggggggggggggggggggggggg7"));
        CHECK(!modclaim_known("ggggggggggggggggggggggg7") && g_fc.releases == releases + 1);
        CHECK(strstr(g_log, "1 claim(s) released, 0 kept") != NULL);
        CHECK(!section_contains(store, "links", "ggggggggggggggggggggggg7") &&
              section_contains(store, "links", "hhhhhhhhhhhhhhhhhhhhhhh8"));
        queries = F.queries;
        sh_modpublish_deleted("local-g", "cloud-g");
        CHECK(F.queries == queries && sh_modpublish_test_delete_checks() == 0);
        sh_modpublish_unpublished("hhhhhhhhhhhhhhhhhhhhhhh8");
        PUMP_UNTIL(idle_all());
        CHECK(!section_contains(store, "links", "hhhhhhhhhhhhhhhhhhhhhhh8"));
    }

    printf("-- every local save makes the search tag match it: present with rows, absent without\n");
    {
        size_t len = 0;
        char *text = rows_text(&g_rows, &len);
        int calls = F.tag_calls, untags = F.untag_calls;
        F.tags_present = 0;
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 1 && F.tag_calls == calls + 1 && F.tags_present);
        CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 0 && F.tag_calls == calls + 2);
        sh_modpublish_save_seen(EMPTY_MAP, strlen(EMPTY_MAP));
        CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 2 && F.untag_calls == untags + 1 && !F.tags_present);
        CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 0 && F.untag_calls == untags + 2);
        {
            static const char RESIZED[] = "{\"entities\":[],\"snapmap-plus\":{\"format\":2,\"mods\":["
                                          "{\"mod\":\"snapmap-plus\",\"name\":\"Snapmap+\",\"uses\":[\"module-sizes\"]}]}}";
            sh_modpublish_save_seen(RESIZED, sizeof RESIZED - 1);
            CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 1 && F.tags_present);
            sh_modpublish_save_seen(EMPTY_MAP, strlen(EMPTY_MAP));
            CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 2 && !F.tags_present);
        }
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_save_tag(LOCAL_ID, NULL) == 0);   /* no model: nothing asked */
        /* a publish whose save was not tagged says so at PublishAfterSave */
        {
            size_t mark = g_log_n;
            sh_modpublish_save_seen(text, len);
            CHECK(sh_modpublish_after_save(EDITOR) == 1);
            CHECK(strstr(g_log + mark, "'mods' was not written with this save; the published map will not carry it") != NULL);
            sh_modpublish_feed_post((void *)(uintptr_t)F.token, "jjjjjjjjjjjjjjjjjjjjjj11", "rev1");
            PUMP_UNTIL(idle_all());
        }
        /* a tag that cannot be written does not stop the publish */
        {
            int starts = F.starts;
            F.tag_result = -1;
            F.tags_present = 0;
            publish(0);
            CHECK(F.starts == starts + 1 && engine_save() == 1);
            CHECK(strstr(g_log, "could not be written; the map publishes without it") != NULL);
            sh_modpublish_feed_post((void *)(uintptr_t)F.token, "iiiiiiiiiiiiiiiiiiiiii10", "rev1");
            PUMP_UNTIL(idle_all());
            F.tag_result = 1;
        }
        if (text) HeapFree(GetProcessHeap(), 0, text);
    }

    printf("-- the copy gate on both sides of the bound\n");
    {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\dddddddddddddddddddddd44_eeeeeeeeeeeeeeeeeeeeee55_map.decl", cache);
        g_menu_notice[0] = 0;
        CHECK(sh_modpublish_copy_allowed("dddddddddddddddddddddd44", "eeeeeeeeeeeeeeeeeeeeee55") == 1);
        CHECK(make_sized(path, SH_MODPUBLISH_TEXT_BOUND));
        CHECK(sh_modpublish_copy_allowed("dddddddddddddddddddddd44", "eeeeeeeeeeeeeeeeeeeeee55") == 1);
        CHECK(g_menu_notice[0] == 0);
        CHECK(make_sized(path, SH_MODPUBLISH_TEXT_BOUND + 1));
        CHECK(sh_modpublish_copy_allowed("dddddddddddddddddddddd44", "eeeeeeeeeeeeeeeeeeeeee55") == 0);
        CHECK(strstr(g_menu_notice, "over the map service's 10.0 MB limit by 1 byte") != NULL &&
              strstr(g_menu_notice, "cannot be copied to My Maps") != NULL);
        CHECK(sh_modpublish_copy_allowed("../x", "y") == 1);
        DeleteFileA(path);
    }

    printf("-- the copy of a map whose pinned rows' content is not complete queues its fetch in the background (D25)\n");
    {
        static const char PINNED[] =
            "{\"entities\":[{\"entityDef\":{\"className\":\"idPlaceableSnapAIEncounter\",\"inherit\":\"mods/pk7salt1/tyrant\","
            "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\"},\"uniqueId\":1,\"~type\":\"idSnapEntity\"}],"
            "\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}"
            ",\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"pk7salt1\",\"name\":\"Eternal Demons\",\"size\":42,"
            "\"objects\":{\"entities/tyrant\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}}]}}";
        static const char VANILLA_TEXT[] =
            "{\"entities\":[],\"instances\":[],\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";
        char mods[MAX_PATH * 2];
        size_t mark;
        FILE *f = NULL;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\dddddddddddddddddddddd44_eeeeeeeeeeeeeeeeeeeeee55_map.decl", cache);
        _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\copy_mods", tmp);
        CreateDirectoryA(mods, NULL);
        sh_cache_set_mods_root(mods);
        sh_modmanifest_set_content(sh_cache_on_load);
        CHECK(modnet_set_base_url("http://127.0.0.1:9/"));   /* nothing answers: the fetch fails fast */
        /* A cached text naming a pinned row whose composition is not here: its content is fetched in the background. */
        if (!fopen_s(&f, path, "wb") && f) { fwrite(PINNED, 1, sizeof PINNED - 1, f); fclose(f); }
        mark = g_log_n;
        CHECK(sh_modpublish_copy_allowed("dddddddddddddddddddddd44", "eeeeeeeeeeeeeeeeeeeeee55") == 1);
        CHECK(strstr(g_log + mark, "the content of 1 of its row(s) is fetched in the background") != NULL);
        PUMP_UNTIL(idle_all());
        /* A vanilla text names no row: nothing is queued. */
        if (!fopen_s(&f, path, "wb") && f) { fwrite(VANILLA_TEXT, 1, sizeof VANILLA_TEXT - 1, f); fclose(f); }
        mark = g_log_n;
        CHECK(sh_modpublish_copy_allowed("dddddddddddddddddddddd44", "eeeeeeeeeeeeeeeeeeeeee55") == 1);
        CHECK(strstr(g_log + mark, "is fetched in the background") == NULL);
        PUMP_UNTIL(idle_all());
        CHECK(modnet_set_base_url(NULL));
        sh_modmanifest_set_content(NULL);
        DeleteFileA(path);
    }

    printf("-- launch: the queue retried and the claims repaired on the worker\n");
    {
        int launches = g_fc.launches;
        sh_modpublish_launch();
        PUMP_UNTIL(idle_all());
        CHECK(g_fc.launches == launches + 1);
        CHECK(strstr(g_log, "MODS: launch: the queue: 1 retried") != NULL);
    }

    printf("-- the publish that makes the key: with the Snapmap+ window closed, the notice says where its backup is\n");
    {
        int k;
        for (k = 0; k < 2; k++) {
            sh_modpublish_ops ops;
            memset(&ops, 0, sizeof ops);
            ops.on_main = f_on_main; ops.can_ask = f_can_ask; ops.ask = f_ask; ops.poll = f_poll; ops.release = f_release;
            ops.editor_token = f_token; ops.publish_fail = f_fail; ops.start = f_start; ops.busy = f_busy;
            ops.placed_names = f_placed_names; ops.editor_notice = f_notice; ops.log = f_log;
            ops.add_tag = f_add_tag; ops.remove_tag = f_remove_tag; ops.edit_ids = f_edit_ids;
            ops.published_query = f_published_query; ops.notice = f_menu_notice;
            ops.window_open = k ? f_window_open : NULL;
            sh_modpublish_test_set_ops(&ops);
            rows_pinned(0);
            fc_set_unit(UNIT_T, MODCLAIM_HELD, 0);
            g_menu_notice[0] = 0;
            g_fc.made_key = 1;
            publish(1);
            CHECK(k ? !strstr(g_menu_notice, SH_MODPUBLISH_KEY_MADE_TEXT)
                    : strstr(g_menu_notice, SH_MODPUBLISH_KEY_MADE_TEXT) != NULL);
            (void)engine_save();
            PUMP_UNTIL(idle_all());
        }
        set_ops();
    }

    sh_modmanifest_list_free(&g_rows);
    sh_modpublish_test_reset();
    sh_modpublish_test_set_ops(NULL);
    modnet_shutdown();
    mt_remove_tree(tmp);
    printf("modpublish_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
