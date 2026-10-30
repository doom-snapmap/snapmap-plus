/* modpublish_test -- the publish side of the mod system (modpublish.c)
 * against a temporary local store and a loopback stub of the mod store
 * (mod_store_stub.h), with the engine replaced by fakes:
 *   a save without rows passes through; a text over the service bound is
 *   refused through PublishFail; the first-time question: No cancels and
 *   leaves the setting, Yes sets it, uploads and resumes; the toast route
 *   cancels once and agrees for the next publish; a failed upload cancels;
 *   the editor leaving abandons, another save cancels; the feed post claims
 *   under the item id (a new claim, then a re-claim with the stored secret
 *   that replaces the set) and releases the provisional claim; a republish
 *   without mods and an unpublish release the item's claim; the copy gate on
 *   both sides of the bound; the launch step releases leftover provisional
 *   claims and retries the queue.
 */
#include "mod_store_stub.h"
#include "../src/backend/modpublish.h"
#include "../src/backend/modmanifest.h"
#include "../src/backend/modnet.h"
#include "../src/backend/modsync.h"
#include "../src/backend/modstore.h"
#include "../src/backend/config.h"
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

/* The load path's activation (modserve_engine.c) is not part of this test. */
int modserve_activate_bundles(const char *const *bundle_hashes, int n) { (void)bundle_hashes; (void)n; return 0; }

/* ------------------------------------------------------------ the fake engine */

static struct {
    unsigned long long token;     /* the editor's live edit object */
    int fails, resumes, asks, ask_buttons, answer;
    char asked[256], toast[256];
    int toast_route;
    /* the search tag: calls, the tag asked for, what the fake returns, and
     * whether the fake model already has it */
    int tag_calls, tag_result, tags_present;
    char tag[32];
    /* the published edit's saved-map ids, and the published-items queries */
    char cloud[64], local[64];
    int queries, query_start;
    unsigned query_ticket;
    char query_cloud[64];
} F;

static void *const EDITOR = (void *)(uintptr_t)0x1000;

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
static void f_resume(void *c, void *editor)
{
    (void)c;
    if (editor != EDITOR) return;
    F.resumes++;
}
/* The saved-map model the local save writes (a fake address). */
static void *const MODEL = (void *)(uintptr_t)0x2000;
#define LOCAL_ID "LOCAL0001"

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
static int f_toast_question(void *c, void *editor, size_t count, unsigned long long bytes)
{
    char text[256];
    sh_modpublish_compose_toast_question(text, sizeof text, count, bytes);
    f_notice(c, editor, text);
    return SH_MODPUBLISH_ASKED_BY_TOAST;
}

static void set_ops(int toast_route)
{
    sh_modpublish_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.on_main = f_on_main; ops.can_ask = f_can_ask; ops.ask = f_ask; ops.poll = f_poll; ops.release = f_release;
    ops.editor_token = f_token; ops.publish_fail = f_fail; ops.resume = f_resume; ops.editor_notice = f_notice;
    ops.upload_question = toast_route ? f_toast_question : NULL;
    ops.log = f_log;
    ops.add_tag = f_add_tag;
    ops.edit_ids = f_edit_ids;
    ops.published_query = f_published_query;
    sh_modpublish_test_set_ops(&ops);
}

/* Tick until the condition holds (the worker runs meanwhile). */
#define PUMP_UNTIL(cond) do {                                                     \
    int pump_i_;                                                                  \
    for (pump_i_ = 0; pump_i_ < 3000 && !(cond); pump_i_++) { sh_modpublish_poll(); Sleep(5); } \
} while (0)

static int idle_all(void) { return !sh_modpublish_holding() && !sh_modpublish_test_background_busy(); }

/* ------------------------------------------------------------ fixtures */

static void make_mod(const char *dir, const char *id, const char *const *files, const char *const *bytes)
{
    char text[512];
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"1.0.0\",\"name\":\"%s\",\"author\":\"tester\"}", id, id);
    mt_write(dir, "mod.json", text);
    for (int i = 0; files[i]; i++) {
        char rel[MAX_PATH];
        strcpy_s(rel, sizeof rel, files[i]);
        for (char *c = rel; *c; c++) if (*c == '/') *c = '\\';
        mt_write(dir, rel, bytes[i]);
    }
}

static int install(const char *folder, char hash[65])
{
    sh_ms_install_stats st;
    sh_mb_bundle b;
    char key[SH_MS_KEY_MAX + 1], cause[SH_MB_CAUSE_MAX];
    int ok = sh_ms_install_from_folder(folder, NULL, key, sizeof key, &st, &b, cause, sizeof cause);
    if (!ok) fprintf(stderr, "install %s: %s\n", folder, cause);
    if (ok) strcpy_s(hash, 65, b.hash);
    sh_mb_free(&b);
    return ok;
}

static const char EMPTY_MAP[] = "{\"name\":\"t\",\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]}}";

/* A saved map naming these bundles in its manifest rows (HeapFree). */
static char *map_with(const char *const *hashes, const char *const *ids, int n, size_t *len)
{
    sh_modmanifest_list rows;
    char err[192];
    char *text;
    memset(&rows, 0, sizeof rows);
    for (int i = 0; i < n; i++) {
        sh_modmanifest_row row;
        memset(&row, 0, sizeof row);
        strcpy_s(row.id, sizeof row.id, ids[i]);
        strcpy_s(row.version, sizeof row.version, "1.0.0");
        strcpy_s(row.bundle, sizeof row.bundle, hashes[i]);
        row.size = 1000 + (unsigned long long)i;
        sh_modmanifest_list_add(&rows, &row);
    }
    text = sh_modmanifest_write(EMPTY_MAP, strlen(EMPTY_MAP), &rows, len, err, sizeof err);
    if (!text) fprintf(stderr, "map_with: %s\n", err);
    sh_modmanifest_list_free(&rows);
    return text;
}

/* A publish's local save as the engine runs it: PublishStart, the save text
 * through the observer, then the metadata write of the same slot. */
static void publish_save(const char *text, size_t len)
{
    sh_modpublish_publish_started(LOCAL_ID);
    sh_modpublish_save_seen(text, len);
    (void)sh_modpublish_save_tag(LOCAL_ID, MODEL);
}

static int setting_confirmed(void)
{
    int on = -1;
    return sh_config_get_bool("mods.upload_confirmed", &on, NULL) ? on : -1;
}

static int file_contains(const char *path, const char *needle)
{
    size_t len = 0;
    unsigned char *t = sh_mb_read_file(path, 1 << 20, &len);
    int hit = t && strstr((const char *)t, needle) != NULL;
    free(t);
    return hit;
}

static stub_claim *stub_live_claim(const char *ref)
{
    for (int i = 0; i < g_stub.nclaims; i++)
        if (g_stub.claims[i].live && !strcmp(g_stub.claims[i].ref, ref)) return &g_stub.claims[i];
    return NULL;
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

static const char *const A_FILES[] = { "assets/generated/image/mods/test.alpha/a.bimage", NULL };
static const char *const A_BYTES[] = { "alpha bytes for the publish test", NULL };
static const char *const B_FILES[] = { "assets/generated/image/mods/test.beta/b.bimage", NULL };
static const char *const B_BYTES[] = { "beta bytes for the publish test", NULL };

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], store[MAX_PATH * 2], dir[MAX_PATH * 2], cache[MAX_PATH * 2], path[MAX_PATH * 2], url[64];
    wchar_t wtmp[MAX_PATH * 2];
    char hA[65], hB[65], provisional[MODNET_REF_MAX * 4 + 1];
    const char *both_h[2], *both_id[2] = { "test.alpha", "test.beta" };
    char *text;
    size_t len = 0;
    g_echo = argc > 1 && !strcmp(argv[1], "-v");

    CHECK(stub_start() > 0);
    stub_url(url, sizeof url);
    CHECK(modnet_set_base_url(url));
    CHECK(mt_temp_dir("modpublish", tmp, sizeof tmp));
    MultiByteToWideChar(CP_ACP, 0, tmp, -1, wtmp, MAX_PATH * 2);
    sh_config_test_set_local_appdata(wtmp);
    CHECK(sh_config_init() == 1);
    _snprintf_s(store, sizeof store, _TRUNCATE, "%s\\mods", tmp);
    _snprintf_s(cache, sizeof cache, _TRUNCATE, "%s\\published-maps", tmp);
    CreateDirectoryA(cache, NULL);
    CHECK(sh_ms_set_root(store));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\src-alpha", tmp); make_mod(dir, "test.alpha", A_FILES, A_BYTES);
    CHECK(install(dir, hA));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\src-beta", tmp); make_mod(dir, "test.beta", B_FILES, B_BYTES);
    CHECK(install(dir, hB));
    both_h[0] = hA; both_h[1] = hB;
    sh_modpublish_test_set_cache_root(cache);
    set_ops(0);
    memset(&F, 0, sizeof F);
    F.token = 0xED17;
    F.tag_result = 1;
    F.query_start = 1;
    CHECK(setting_confirmed() == 0);

    printf("-- the question text\n");
    {
        char q[256];
        sh_modpublish_compose_question(q, sizeof q, 2, 3u * 1024u * 1024u);
        CHECK(!strcmp(q, "This map uses 2 mods (3.0 MB). Upload them to the mod store so other players can download them? (asked once)"));
        sh_modpublish_compose_question(q, sizeof q, 1, 512);
        CHECK(strstr(q, "uses 1 mod (512 bytes). Upload it") != NULL && strlen(q) < 256);
    }

    printf("-- a save without rows passes through\n");
    {
        int claims = g_stub.claim_calls, requests = g_stub.requests;
        sh_modpublish_save_seen(EMPTY_MAP, strlen(EMPTY_MAP));
        CHECK(sh_modpublish_after_save(EDITOR) == 1);
        CHECK(!sh_modpublish_holding() && F.fails == 0 && F.asks == 0);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "r1");
        PUMP_UNTIL(idle_all());
        CHECK(g_stub.claim_calls == claims && g_stub.requests == requests);
    }

    printf("-- a text over the service bound is refused\n");
    {
        char *rows_text = map_with(both_h, both_id, 2, &len), *big;
        size_t big_len = (size_t)SH_MODPUBLISH_TEXT_BOUND + 4096;
        CHECK(rows_text != NULL);
        big = malloc(big_len + 1);
        CHECK(big != NULL);
        if (rows_text && big) {
            /* The rows text, then whitespace inside the top-level object. */
            memset(big, ' ', big_len);
            memcpy(big, rows_text, len - 1);
            big[big_len - 1] = '}';
            big[big_len] = 0;
            sh_modpublish_save_seen(big, big_len);
            CHECK(sh_modpublish_after_save(EDITOR) == 0);
            CHECK(!sh_modpublish_holding() && F.fails == 1 && F.asks == 0);
            CHECK(strstr(F.toast, "over the map service's 10.0 MB limit by 4096 bytes") != NULL);
        }
        free(big);
        if (rows_text) HeapFree(GetProcessHeap(), 0, rows_text);
    }

    text = map_with(both_h, both_id, 2, &len);
    CHECK(text != NULL);

    printf("-- first time: No cancels and leaves the setting\n");
    {
        int puts = g_stub.blob_puts, missing = g_stub.missing_calls;
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        CHECK(sh_modpublish_test_state() == 1);
        F.answer = 0;
        sh_modpublish_poll();
        CHECK(sh_modpublish_test_state() == 2 && F.asks == 1 && F.ask_buttons == 6);
        CHECK(strstr(F.asked, "This map uses 2 mods (2001 bytes). Upload them") != NULL);
        F.answer = 2;   /* declined */
        sh_modpublish_poll();
        CHECK(!sh_modpublish_holding() && F.fails == 2 && F.resumes == 0);
        CHECK(F.tag_calls == 0);   /* a cancelled publish is not tagged */
        CHECK(strstr(F.toast, "only with your agreement") != NULL);
        CHECK(setting_confirmed() == 0);
        CHECK(g_stub.blob_puts == puts && g_stub.missing_calls == missing);
    }

    printf("-- first time: Yes sets the setting, uploads and resumes\n");
    {
        stub_claim *c;
        size_t mark = g_log_n;
        publish_save(text, len);
        /* The search tag is written into the saved metadata, before the hold. */
        CHECK(F.tag_calls == 1 && !strcmp(F.tag, SH_MODPUBLISH_TAG) && F.tags_present);
        CHECK(strstr(g_log + mark, "search tag 'mods' written into the saved map's tags") != NULL);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        CHECK(strstr(g_log + mark, "was not written with this save") == NULL);
        F.answer = 0;
        sh_modpublish_poll();
        F.answer = 1;   /* accepted */
        strcpy_s(provisional, sizeof provisional, sh_modpublish_test_provisional());
        CHECK(!strncmp(provisional, SH_MODPUBLISH_PENDING_PREFIX, strlen(SH_MODPUBLISH_PENDING_PREFIX)) &&
              strlen(provisional) == strlen(SH_MODPUBLISH_PENDING_PREFIX) + 16);
        PUMP_UNTIL(!sh_modpublish_holding());
        CHECK(setting_confirmed() == 1);
        CHECK(F.resumes == 1 && F.fails == 2);
        CHECK(F.tag_calls == 1);   /* nothing more at the resume */
        CHECK(stub_has_bundle(hA) && stub_has_bundle(hB));
        c = stub_live_claim(provisional);
        CHECK(c && c->nb == 2);
        CHECK(!strcmp(sh_modpublish_test_provisional(), provisional));
    }

    printf("-- the feed post claims under the item and releases the provisional claim\n");
    {
        stub_claim *c;
        char claim[33];
        int with_secret = g_stub.claim_with_secret;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", store, MODSYNC_CLAIMS_FILE);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "rev1");
        PUMP_UNTIL(idle_all());
        c = stub_live_claim("aaaaaaaaaaaaaaaaaaaaaaa1");
        CHECK(c && c->nb == 2);
        CHECK(g_stub.claim_with_secret == with_secret);   /* no stored secret: a new claim */
        CHECK(stub_live_claim(provisional) == NULL);
        CHECK(modsync_claim_lookup("aaaaaaaaaaaaaaaaaaaaaaa1", claim, NULL) && !strcmp(claim, c->id));
        CHECK(!modsync_claim_lookup(provisional, NULL, NULL));
        CHECK(file_contains(path, "aaaaaaaaaaaaaaaaaaaaaaa1") && !file_contains(path, provisional));
        CHECK(strstr(g_log, "claimed 2 bundle(s) under item 'aaaaaaaaaaaaaaaaaaaaaaa1'") != NULL);
    }

    printf("-- a republish re-claims with the stored secret and replaces the set\n");
    {
        stub_claim *c;
        char *only_a;
        char before[33];
        size_t a_len = 0;
        int with_secret = g_stub.claim_with_secret, asks = F.asks;
        const char *one_h[1] = { hA }, *one_id[1] = { "test.alpha" };
        strcpy_s(before, sizeof before, stub_live_claim("aaaaaaaaaaaaaaaaaaaaaaa1")->id);
        only_a = map_with(one_h, one_id, 1, &a_len);
        CHECK(only_a != NULL);
        {
            size_t mark = g_log_n;
            publish_save(only_a, a_len);
            CHECK(sh_modpublish_after_save(EDITOR) == 0);
            CHECK(F.tag_calls == 2);   /* asked again; present already, the fake answered 0 */
            CHECK(strstr(g_log + mark, "was not written with this save") == NULL);
        }
        CHECK(sh_modpublish_test_state() == 4);   /* agreed already: straight to the worker */
        PUMP_UNTIL(!sh_modpublish_holding());
        CHECK(F.resumes == 2 && F.asks == asks);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "rev2");
        PUMP_UNTIL(idle_all());
        c = stub_live_claim("aaaaaaaaaaaaaaaaaaaaaaa1");
        CHECK(c && c->nb == 1 && !strcmp(c->bundles[0], hA) && !strcmp(c->id, before));
        CHECK(g_stub.claim_with_secret == with_secret + 1);
        CHECK(strstr(g_log, "replacing the previous set") != NULL);
        if (only_a) HeapFree(GetProcessHeap(), 0, only_a);
    }

    printf("-- a republish without mods releases the item's claim\n");
    {
        publish_save(EMPTY_MAP, strlen(EMPTY_MAP));
        CHECK(sh_modpublish_after_save(EDITOR) == 1);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "aaaaaaaaaaaaaaaaaaaaaaa1", "rev3");
        PUMP_UNTIL(idle_all());
        CHECK(stub_live_claim("aaaaaaaaaaaaaaaaaaaaaaa1") == NULL);
        CHECK(!modsync_claim_lookup("aaaaaaaaaaaaaaaaaaaaaaa1", NULL, NULL));
        CHECK(F.tag_calls == 2);   /* no mods, no tag */
    }

    printf("-- unpublish releases the item's claim\n");
    {
        int releases;
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        PUMP_UNTIL(!sh_modpublish_holding());
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "bbbbbbbbbbbbbbbbbbbbbbb2", "rev1");
        PUMP_UNTIL(idle_all());
        CHECK(stub_live_claim("bbbbbbbbbbbbbbbbbbbbbbb2") != NULL);
        sh_modpublish_unpublished("bbbbbbbbbbbbbbbbbbbbbbb2");
        PUMP_UNTIL(idle_all());
        CHECK(stub_live_claim("bbbbbbbbbbbbbbbbbbbbbbb2") == NULL);
        CHECK(!modsync_claim_lookup("bbbbbbbbbbbbbbbbbbbbbbb2", NULL, NULL));
        /* An item this installation never claimed: nothing is sent. */
        releases = g_stub.release_calls;
        sh_modpublish_unpublished("ccccccccccccccccccccccc3");
        PUMP_UNTIL(idle_all());
        CHECK(g_stub.release_calls == releases);
    }

    printf("-- delete from My Maps releases a claim only when its item is no longer published\n");
    {
        const char *still[2] = { "ggggggggggggggggggggggg7", "zzzzzzzzzzzzzzzzzzzzzzz9" };
        const char *other[1] = { "zzzzzzzzzzzzzzzzzzzzzzz9" };
        int queries, releases;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", store, SH_MODPUBLISH_LINKS_FILE);
        /* Publish item g from cloud-saved item cloud-g, and item h from a map
         * whose cloud-saved id did not read. */
        strcpy_s(F.cloud, sizeof F.cloud, "cloud-g");
        strcpy_s(F.local, sizeof F.local, "local-g");
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        PUMP_UNTIL(!sh_modpublish_holding());
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "ggggggggggggggggggggggg7", "rev1");
        PUMP_UNTIL(idle_all());
        CHECK(stub_live_claim("ggggggggggggggggggggggg7") != NULL);
        CHECK(file_contains(path, "\"ggggggggggggggggggggggg7\":{\"cloud\":\"cloud-g\",\"local\":\"local-g\",\"at\":"));
        F.cloud[0] = 0;
        strcpy_s(F.local, sizeof F.local, "local-h");
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        PUMP_UNTIL(!sh_modpublish_holding());
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "hhhhhhhhhhhhhhhhhhhhhhh8", "rev1");
        PUMP_UNTIL(idle_all());
        CHECK(file_contains(path, "\"hhhhhhhhhhhhhhhhhhhhhhh8\":{\"local\":\"local-h\",\"at\":"));
        F.local[0] = 0;
        CHECK(file_contains(path, "\"at\":"));

        /* Published within the hour: the search may not list it yet; kept. */
        queries = F.queries;
        sh_modpublish_deleted("local-g", "cloud-g");
        sh_modpublish_poll();
        CHECK(F.queries == queries && sh_modpublish_test_delete_checks() == 0);
        CHECK(strstr(g_log, "published less than 60 minute(s) ago") != NULL);
        sh_modpublish_test_set_link_min_age(0);

        /* A map nothing was published from: no query. */
        queries = F.queries;
        sh_modpublish_deleted("local-x", "cloud-x");
        CHECK(F.queries == queries && sh_modpublish_test_delete_checks() == 0);
        /* Published from a map without a cloud-saved id: cannot be checked, kept. */
        sh_modpublish_deleted("local-h", "");
        CHECK(F.queries == queries && strstr(g_log, "no cloud-saved id") != NULL);
        CHECK(stub_live_claim("hhhhhhhhhhhhhhhhhhhhhhh8") != NULL);

        /* Still published: kept. The query starts on the next tick. */
        sh_modpublish_deleted("local-g", "cloud-g");
        CHECK(F.queries == queries && sh_modpublish_test_delete_checks() == 1);
        sh_modpublish_poll();
        CHECK(F.queries == queries + 1 && !strcmp(F.query_cloud, "cloud-g") && sh_modpublish_test_delete_checks() == 1);
        sh_modpublish_published_result(F.query_ticket, 1, still, 2);
        sh_modpublish_poll();
        PUMP_UNTIL(idle_all());
        CHECK(sh_modpublish_test_delete_checks() == 0 && stub_live_claim("ggggggggggggggggggggggg7") != NULL);
        CHECK(strstr(g_log, "0 claim(s) released, 1 kept (still published)") != NULL);

        /* A full page may be only the first page: kept. */
        {
            const char *page[30];
            for (int i = 0; i < 30; i++) page[i] = "zzzzzzzzzzzzzzzzzzzzzzz9";
            sh_modpublish_deleted("", "cloud-g");
            sh_modpublish_poll();
            sh_modpublish_published_result(F.query_ticket, 1, page, 30);
            sh_modpublish_poll();
            CHECK(strstr(g_log, "answered a full page of 30 published item(s); 1 claim(s) kept") != NULL);
        }

        /* A failed answer, a late answer, and a check that could not start: kept. */
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
        sh_modpublish_published_result(F.query_ticket, 1, other, 1);   /* too late: ignored */
        sh_modpublish_poll();
        F.query_start = 0;
        sh_modpublish_deleted("", "cloud-g");
        sh_modpublish_poll();
        CHECK(sh_modpublish_test_delete_checks() == 0 && strstr(g_log, "could not start") != NULL);
        F.query_start = 1;
        PUMP_UNTIL(idle_all());
        CHECK(stub_live_claim("ggggggggggggggggggggggg7") != NULL);

        /* No longer published: released, and its link forgotten. */
        releases = g_stub.release_calls;
        sh_modpublish_deleted("local-g", "cloud-g");
        sh_modpublish_poll();
        sh_modpublish_published_result(F.query_ticket, 1, other, 1);
        PUMP_UNTIL(idle_all() && stub_live_claim("ggggggggggggggggggggggg7") == NULL);
        CHECK(stub_live_claim("ggggggggggggggggggggggg7") == NULL && g_stub.release_calls == releases + 1);
        CHECK(!modsync_claim_lookup("ggggggggggggggggggggggg7", NULL, NULL));
        CHECK(strstr(g_log, "1 claim(s) released, 0 kept") != NULL);
        CHECK(!file_contains(path, "ggggggggggggggggggggggg7") && file_contains(path, "hhhhhhhhhhhhhhhhhhhhhhh8"));
        /* Deleting it again: nothing linked any more. */
        queries = F.queries;
        sh_modpublish_deleted("local-g", "cloud-g");
        CHECK(F.queries == queries && sh_modpublish_test_delete_checks() == 0);
        /* An unpublish forgets the link too. */
        sh_modpublish_unpublished("hhhhhhhhhhhhhhhhhhhhhhh8");
        PUMP_UNTIL(idle_all());
        CHECK(!file_contains(path, "hhhhhhhhhhhhhhhhhhhhhhh8"));
    }

    printf("-- the search tag goes only into the armed publish's own save\n");
    {
        int calls = F.tag_calls;
        size_t mark;
        F.tags_present = 0;
        /* A plain save: never armed. */
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 0 && F.tag_calls == calls);
        /* Another slot's write does not take the arm; the publish's own does. */
        sh_modpublish_publish_started(LOCAL_ID);
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_save_tag("OTHER0002", MODEL) == 0 && F.tag_calls == calls);
        CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 1 && F.tag_calls == calls + 1);
        CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 0 && F.tag_calls == calls + 1);   /* one shot */
        /* A map without a local id yet: the next write is taken. */
        F.tags_present = 0;
        sh_modpublish_publish_started("");
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_save_tag("NEW0003", MODEL) == 1 && F.tag_calls == calls + 2);
        /* An arm older than the save window is dropped. */
        F.tags_present = 0;
        sh_modpublish_publish_started(LOCAL_ID);
        sh_modpublish_test_age_tag_arm(20000);
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_save_tag(LOCAL_ID, MODEL) == 0 && F.tag_calls == calls + 2);
        /* A publish whose save was not tagged says so at the hold. */
        mark = g_log_n;
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        CHECK(strstr(g_log + mark, "'mods' was not written with this save; the published map will not carry it") != NULL);
        PUMP_UNTIL(!sh_modpublish_holding());
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "jjjjjjjjjjjjjjjjjjjjjj11", "rev1");
        PUMP_UNTIL(idle_all());
    }

    printf("-- a search tag that cannot be written does not stop the publish\n");
    {
        int resumes = F.resumes;
        F.tag_result = -1;
        F.tags_present = 0;
        publish_save(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        PUMP_UNTIL(!sh_modpublish_holding());
        CHECK(F.resumes == resumes + 1);
        CHECK(strstr(g_log, "could not be written; the map publishes without it") != NULL);
        sh_modpublish_feed_post((void *)(uintptr_t)F.token, "iiiiiiiiiiiiiiiiiiiiii10", "rev1");
        PUMP_UNTIL(idle_all());
        F.tag_result = 1;
    }

    printf("-- a failed upload cancels the publish\n");
    {
        int resumes = F.resumes, fails = F.fails;
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        strcpy_s(provisional, sizeof provisional, sh_modpublish_test_provisional());
        stub_fail_next("POST /mods/missing", 503);
        PUMP_UNTIL(!sh_modpublish_holding());
        CHECK(F.resumes == resumes && F.fails == fails + 1);
        CHECK(strstr(F.toast, "could not be uploaded") != NULL && strstr(F.toast, "It was not published.") != NULL);
        PUMP_UNTIL(idle_all());
        /* modsync queued the transient failure under the provisional ref. */
        CHECK(modsync_pending_count() == 1);
    }

    printf("-- the editor leaving abandons; another save cancels\n");
    {
        int fails = F.fails, resumes = F.resumes;
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        F.token = 0;
        sh_modpublish_poll();
        CHECK(!sh_modpublish_holding() && F.fails == fails && F.resumes == resumes);
        F.token = 0xED18;
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        sh_modpublish_save_seen(text, len);
        sh_modpublish_poll();
        CHECK(!sh_modpublish_holding() && F.fails == fails + 1 && F.resumes == resumes);
        CHECK(strstr(F.toast, "saved again") != NULL);
        PUMP_UNTIL(idle_all());
    }

    printf("-- the toast route cancels once; the next publish is the agreement\n");
    {
        int fails = F.fails, resumes = F.resumes, asks = F.asks;
        CHECK(sh_config_set_json("mods.upload_confirmed", "false"));
        set_ops(1);
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0);
        sh_modpublish_poll();
        CHECK(!sh_modpublish_holding() && F.fails == fails + 1 && F.asks == asks);
        CHECK(strstr(F.toast, "Publishing this map uploads its 2 mods") != NULL && strstr(F.toast, "Publish again to agree.") != NULL);
        CHECK(setting_confirmed() == 1);
        sh_modpublish_save_seen(text, len);
        CHECK(sh_modpublish_after_save(EDITOR) == 0 && sh_modpublish_test_state() == 4);
        PUMP_UNTIL(!sh_modpublish_holding());
        CHECK(F.resumes == resumes + 1);
        /* Its context never sees a feed post: the editor leaves, and the
         * provisional claim is released. */
        strcpy_s(provisional, sizeof provisional, sh_modpublish_test_provisional());
        CHECK(stub_live_claim(provisional) != NULL);
        F.token = 0xED19;
        PUMP_UNTIL(idle_all() && stub_live_claim(provisional) == NULL);
        CHECK(stub_live_claim(provisional) == NULL);
        set_ops(0);
    }

    printf("-- the copy gate on both sides of the bound\n");
    {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\dddddddddddddddddddddd44_eeeeeeeeeeeeeeeeeeeeee55_map.decl", cache);
        CHECK(sh_modpublish_copy_allowed("dddddddddddddddddddddd44", "eeeeeeeeeeeeeeeeeeeeee55") == 1);   /* not cached */
        CHECK(make_sized(path, SH_MODPUBLISH_TEXT_BOUND));
        CHECK(sh_modpublish_copy_allowed("dddddddddddddddddddddd44", "eeeeeeeeeeeeeeeeeeeeee55") == 1);
        CHECK(sh_modpublish_test_notice()[0] == 0);
        CHECK(make_sized(path, SH_MODPUBLISH_TEXT_BOUND + 1));
        CHECK(sh_modpublish_copy_allowed("dddddddddddddddddddddd44", "eeeeeeeeeeeeeeeeeeeeee55") == 0);
        CHECK(strstr(sh_modpublish_test_notice(), "over the map service's 10.0 MB limit by 1 byte") != NULL);
        F.asks = 0;
        F.answer = 1;
        sh_modpublish_poll();
        CHECK(F.asks == 1 && F.ask_buttons == 0 && strstr(F.asked, "cannot be copied to My Maps") != NULL);
        sh_modpublish_poll();
        CHECK(sh_modpublish_copy_allowed("../x", "y") == 1);   /* an id that is not a file name: native */
        DeleteFileA(path);
    }

    printf("-- launch: leftover provisional claims released, the queue retried\n");
    {
        modsync_stats st;
        const char *one[1] = { hB };
        int left;
        /* A provisional claim a closed game left behind. */
        CHECK(modsync_upload_bundles(one, 1, SH_MODPUBLISH_PENDING_PREFIX "00000000deadbeef", NULL, &st));
        /* An item claim that failed after its upload: queued for retry. */
        stub_fail_next("POST /mods/claim", 503);
        CHECK(!modsync_upload_bundles(one, 1, "fffffffffffffffffffffff6", NULL, &st) && st.queued);
        CHECK(modsync_pending_count() == 2);   /* plus the failed publish's provisional upload */
        sh_modpublish_launch();
        PUMP_UNTIL(idle_all());
        CHECK(stub_live_claim(SH_MODPUBLISH_PENDING_PREFIX "00000000deadbeef") == NULL);
        CHECK(stub_live_claim("fffffffffffffffffffffff6") != NULL);
        left = modsync_pending_count();
        CHECK(left == 0);
        CHECK(strstr(g_log, "MODS: launch: 2 provisional ref(s) settled; queued uploads: 1 retried, 0 dropped, 0 left") != NULL);
        /* Nothing to settle: no job. */
        sh_modpublish_launch();
        CHECK(!sh_modpublish_test_background_busy());
    }

    printf("-- secrets never reach the log\n");
    {
        int leaked = 0;
        for (int i = 0; i < g_stub.nclaims; i++) if (strstr(g_log, g_stub.claims[i].secret)) leaked = 1;
        CHECK(!leaked);
    }

    if (text) HeapFree(GetProcessHeap(), 0, text);
    sh_modpublish_test_reset();
    sh_modpublish_test_set_ops(NULL);
    modnet_shutdown();
    stub_stop();
    mt_remove_tree(tmp);
    printf("modpublish_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
