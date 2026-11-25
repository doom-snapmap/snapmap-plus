/* modnet_test -- the WinHTTP store client against a loopback stub of the store (mod_store_stub.h): URL and argument
 * rules, a unit's archive request, injected failures, cancel, and the job helper. No real store is contacted. */
#include "heap_out.h"
#include "mod_store_stub.h"
#include "../src/backend/modnet.h"
#include "../src/backend/modsalt.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* Log sink: every line is kept so the test can check no secret is logged. */
static char g_log[64 * 1024];
static size_t g_log_n;
void backend_log(const char *line)
{
    size_t n = line ? strlen(line) : 0;
    if (n && g_log_n + n + 2 < sizeof g_log) { memcpy(g_log + g_log_n, line, n); g_log_n += n; g_log[g_log_n++] = '\n'; g_log[g_log_n] = 0; }
}

/* ---- job helper ---- */

static volatile LONG g_job_progress_calls, g_job_done_result = 12345, g_job_ran;

static void job_progress(void *ctx, unsigned long long done, unsigned long long total)
{
    (void)ctx; (void)done; (void)total;
    InterlockedIncrement(&g_job_progress_calls);
}

static int job_body(void *ctx, const modnet_io *io)
{
    int i;
    (void)ctx;
    InterlockedExchange(&g_job_ran, 1);
    if (io->progress) io->progress(io->context, 1, 2);
    for (i = 0; i < 500; i++) {
        if (io->cancel && InterlockedCompareExchange(io->cancel, 0, 0)) return MODNET_E_CANCELLED;
        Sleep(10);
    }
    return MODNET_OK;
}

static void job_done(void *ctx, int result)
{
    (void)ctx;
    InterlockedExchange(&g_job_done_result, result);
}

static int quick_body(void *ctx, const modnet_io *io) { (void)ctx; (void)io; return 7; }

static unsigned long long g_pr_done, g_pr_total;
static int g_pr_calls;
static void progress(void *ctx, unsigned long long done, unsigned long long total)
{
    (void)ctx;
    g_pr_calls++;
    g_pr_done = done;
    g_pr_total = total;
}

int main(void)
{
    char url[64], tmp[MAX_PATH];
    modnet_result r;
    int code;

    /* A publishing key that cannot be used is its own code: not the store's address, not the store being away. */
    CHECK(MODNET_E_KEY < MODNET_E_RETIRED && MODNET_E_KEY != MODNET_E_CONFIG);
    CHECK(!modnet_transient(MODNET_E_KEY) && !modnet_unreachable(MODNET_E_KEY));
    CHECK(strcmp(modnet_code_text(MODNET_E_KEY), "error") != 0 && strstr(modnet_code_text(MODNET_E_KEY), "key") != NULL);
    CHECK(stub_start() > 0);
    stub_url(url, sizeof url);
    CHECK(mt_temp_dir("modnet", tmp, sizeof tmp));

    /* ---- base URL rules ---- */
    CHECK(!modnet_set_base_url("http://example.com"));
    CHECK(!modnet_set_base_url("ftp://127.0.0.1/"));
    CHECK(!modnet_set_base_url("https://store.test/x?y=1"));
    CHECK(modnet_set_base_url("https://store.test/base/"));
    {
        char *base = modnet_base_url();
        CHECK(base && strcmp(base, "https://store.test/base/") == 0);
        free(base);
        CHECK(modnet_set_base_url(NULL));
        /* No settings service in this test: the documented default applies. */
        base = modnet_base_url();
        CHECK(base && strcmp(base, MODNET_DEFAULT_STORE) == 0);
        free(base);
    }
    CHECK(modnet_set_base_url(url));

    /* ---- a unit's archive: one request, every file, or the files a mask names as ?want=<base64url> ---- */
    {
        static const unsigned char mask[2] = { 0x05, 0x80 };   /* files 0, 2 and 15 */
        static const char unit[] = "abababababababababababababababababababababababababababababababab";
        char out[MAX_PATH * 2];
        _snprintf_s(out, sizeof out, _TRUNCATE, "%s\\unit-archive.zip", tmp);
        (void)modnet_get_unit_archive(unit, mask, sizeof mask, out, 0, NULL, &r);
        CHECK(!strcmp(g_stub.last_get, "/mods/unit/" "abababababababababababababababababababababababababababababababab" "/archive?want=BYA"));
        (void)modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r);
        CHECK(!strcmp(g_stub.last_get, "/mods/unit/abababababababababababababababababababababababababababababababab/archive"));
        CHECK(modnet_get_unit_archive(unit, NULL, 3, out, 0, NULL, &r) == MODNET_E_ARG);   /* a length with no mask */
        DeleteFileA(out);
    }

    /* ---- argument rules ---- */
    CHECK(modnet_valid_ref("item-41"));
    CHECK(modnet_valid_ref("export:My Map (2)"));
    CHECK(!modnet_valid_ref(""));
    CHECK(!modnet_valid_ref("tab\tref"));
    {
        /* A ref of any length is carried whole; the store refuses what it cannot keep. */
        static char longref[2001];
        memset(longref, 'r', 2000);
        CHECK(modnet_valid_ref(longref));
    }
    CHECK(modnet_valid_ref("0123456789012345678901234567890123456789012345678901234567890123"));    /* 64 */
    CHECK(!modnet_valid_hash("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"));
    /* ---- a store address is any length: it is kept and used whole ---- */
    {
        static const char unit[] = "abababababababababababababababababababababababababababababababab";
        char out[MAX_PATH * 2];
        size_t n = strlen(url);
        char *longer = (char *)malloc(n + 6002), *base;
        CHECK(longer != NULL);
        memcpy(longer, url, n);
        longer[n] = '/';
        memset(longer + n + 1, 'p', 6000);
        longer[n + 6001] = 0;
        CHECK(modnet_set_base_url(longer));
        base = modnet_base_url();
        CHECK(base && strcmp(base, longer) == 0);
        free(base);
        _snprintf_s(out, sizeof out, _TRUNCATE, "%s\unit-long.zip", tmp);
        code = modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r);
        CHECK(code != MODNET_E_CONFIG && code != MODNET_E_ARG && r.status != 0);   /* the request reached the store */
        free(longer);
    }
    CHECK(modnet_set_base_url(url));

    /* ---- injected failures, through a unit's archive ---- */
    {
        static const char unit[] = "abababababababababababababababababababababababababababababababab";
        char out[MAX_PATH * 2];
        _snprintf_s(out, sizeof out, _TRUNCATE, "%s\unit-fail.zip", tmp);
        code = modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r);
        CHECK(code == MODNET_E_NOT_FOUND && r.status == 404);   /* the stub holds nothing */
        stub_fail_next("GET /mods/unit/", 503);
        code = modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r);
        CHECK(code == MODNET_E_UNAVAILABLE && modnet_transient(code) && modnet_unreachable(code));
        CHECK(strstr(r.message, "injected failure 503") != NULL);
        stub_fail_next("GET /mods/unit/", 451);
        code = modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r);
        CHECK(code == MODNET_E_BLOCKED && !modnet_transient(code) && !modnet_unreachable(code));
        stub_fail_next("GET /mods/unit/", 429);
        code = modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r);
        CHECK(code == MODNET_E_RATE && modnet_transient(code));
        {
            /* Cancelled before the request is made. */
            volatile long cancel = 1;
            modnet_io io = { &cancel, NULL, NULL };
            int before = g_stub.requests;
            CHECK(modnet_get_unit_archive(unit, NULL, 0, out, 0, &io, &r) == MODNET_E_CANCELLED);
            CHECK(g_stub.requests == before);
        }

        /* ---- a refused connection ---- */
        CHECK(modnet_set_base_url("http://127.0.0.1:1"));
        code = modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r);
        CHECK(code == MODNET_E_NET || code == MODNET_E_TIMEOUT);
        CHECK(modnet_transient(code) && modnet_unreachable(code) && r.status == 0);
        CHECK(modnet_set_base_url(url));
    }

    /* ---- the job helper ---- */
    CHECK(modnet_job_start("slow", job_body, job_done, job_progress, NULL, NULL) == 1);
    CHECK(modnet_job_start("second", quick_body, NULL, NULL, NULL, NULL) == 0);   /* one at a time */
    {
        char name[64];
        CHECK(modnet_job_busy(name, sizeof name) && strcmp(name, "slow") == 0);
    }
    Sleep(50);
    CHECK(modnet_job_cancel() == 1);
    CHECK(modnet_job_wait(5000));
    CHECK(g_job_ran == 1 && g_job_progress_calls >= 1 && g_job_done_result == MODNET_E_CANCELLED);
    CHECK(!modnet_job_busy(NULL, 0));
    CHECK(modnet_job_start("again", quick_body, job_done, NULL, NULL, NULL) == 1);
    CHECK(modnet_job_wait(5000) && g_job_done_result == 7);
    CHECK(modnet_job_cancel() == 0);

    modnet_shutdown();
    stub_stop();
    mt_remove_tree(tmp);
    printf("modnet_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
