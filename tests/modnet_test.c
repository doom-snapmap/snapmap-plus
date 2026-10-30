/* modnet_test -- the WinHTTP store client against a loopback stub of the
 * store (mod_store_stub.h): base URL rules, every route's success and
 * refusal codes, hash verification of downloads, Content-Length on every
 * request with a body, the missing-blob split at 4096 hashes, claims and
 * releases, injected 5xx and 429, a refused connection, and the job helper's
 * progress, cancel and one-job rule. No real store is contacted.
 */
#include "mod_store_stub.h"
#include "../src/backend/modnet.h"

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

static void sha_of(const char *text, char out[65]) { sh_mb_sha256_hex(text, strlen(text), out); }

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

static unsigned long long g_up_done, g_up_total;
static int g_up_calls;
static void upload_progress(void *ctx, unsigned long long done, unsigned long long total)
{
    (void)ctx;
    g_up_calls++;
    g_up_done = done;
    g_up_total = total;
}

int main(void)
{
    char url[64], h_hello[65], h_other[65], h_empty[65], h_missing[65], h_bundle[65], h_bundle2[65];
    char claim[33], secret[65], claim2[33], secret2[65], text[512], text2[512];
    modnet_result r;
    int code, n = 0, before;
    char (*missing)[MODNET_HASH_HEX + 1] = NULL;

    CHECK(stub_start() > 0);
    stub_url(url, sizeof url);

    /* ---- base URL rules ---- */
    CHECK(!modnet_set_base_url("http://example.com"));
    CHECK(!modnet_set_base_url("ftp://127.0.0.1/"));
    CHECK(!modnet_set_base_url("https://store.test/x?y=1"));
    CHECK(modnet_set_base_url("https://store.test/base/"));
    {
        char got[MODNET_URL_MAX];
        CHECK(modnet_base_url(got, sizeof got) && strcmp(got, "https://store.test/base/") == 0);
        CHECK(modnet_set_base_url(NULL));
        /* No settings service in this test: the documented default applies. */
        CHECK(modnet_base_url(got, sizeof got) && strcmp(got, MODNET_DEFAULT_STORE) == 0);
    }
    CHECK(modnet_set_base_url(url));

    /* ---- argument rules ---- */
    CHECK(modnet_valid_ref("item-41"));
    CHECK(modnet_valid_ref("export:My Map (2)"));
    CHECK(!modnet_valid_ref(""));
    CHECK(!modnet_valid_ref("tab\tref"));
    CHECK(!modnet_valid_ref("0123456789012345678901234567890123456789012345678901234567890123x"));   /* 65 */
    CHECK(modnet_valid_ref("0123456789012345678901234567890123456789012345678901234567890123"));    /* 64 */
    CHECK(!modnet_valid_hash("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"));

    sha_of("hello", h_hello);
    sha_of("other", h_other);
    sha_of("", h_empty);
    sha_of("never stored", h_missing);

    /* ---- blob upload: created, then idempotent; Content-Length always ---- */
    code = modnet_put_blob(h_hello, "hello", 5, NULL, &r);
    CHECK(code == MODNET_OK && r.status == 201 && r.created == 1 && r.sent == 5);
    CHECK(g_stub.last_cl_headers == 1 && g_stub.last_cl_value == 5);
    CHECK(strcmp(g_stub.last_ctype, "application/octet-stream") == 0);
    code = modnet_put_blob(h_hello, "hello", 5, NULL, &r);
    CHECK(code == MODNET_OK && r.status == 200 && r.created == 0);
    CHECK(g_stub.blob_created == 1 && g_stub.blob_puts == 2);
    code = modnet_put_blob(h_empty, "", 0, NULL, &r);
    CHECK(code == MODNET_OK && r.created == 1);
    CHECK(g_stub.last_cl_headers == 1 && g_stub.last_cl_value == 0);   /* a zero-length body still says so */
    before = g_stub.requests;
    CHECK(modnet_put_blob(h_other, "hello", 5, NULL, &r) == MODNET_E_HASH);   /* refused before sending */
    CHECK(g_stub.requests == before);
    CHECK(modnet_put_blob("nothex", "x", 1, NULL, &r) == MODNET_E_ARG);

    /* ---- missing negotiation ---- */
    {
        const char *ask[] = { h_hello, h_other, h_other, h_empty };
        code = modnet_missing(ask, 4, &missing, &n, NULL, &r);
        CHECK(code == MODNET_OK && n == 1 && strcmp(missing[0], h_other) == 0);
        CHECK(g_stub.last_cl_headers == 1 && g_stub.last_cl_value > 0);
        free(missing);
        missing = NULL;
    }
    {
        /* 5000 distinct hashes: two requests (4096 + 904), every one missing. */
        static char many[5000][65];
        static const char *ptr[5000];
        int i, calls = g_stub.missing_calls;
        for (i = 0; i < 5000; i++) {
            char seed[32];
            _snprintf_s(seed, sizeof seed, _TRUNCATE, "seed-%d", i);
            sha_of(seed, many[i]);
            ptr[i] = many[i];
        }
        code = modnet_missing(ptr, 5000, &missing, &n, NULL, &r);
        CHECK(code == MODNET_OK && n == 5000);
        CHECK(g_stub.missing_calls == calls + 2 && r.requests == 2);
        free(missing);
        missing = NULL;
    }

    /* ---- blob download: verified ---- */
    {
        unsigned char *bytes = NULL;
        size_t len = 0;
        code = modnet_get_blob(h_hello, 5, &bytes, &len, NULL, &r);
        CHECK(code == MODNET_OK && len == 5 && memcmp(bytes, "hello", 5) == 0);
        free(bytes);
        bytes = NULL;
        CHECK(modnet_get_blob(h_hello, 6, &bytes, &len, NULL, &r) == MODNET_E_HASH);   /* not the size the manifest names */
        CHECK(bytes == NULL);
        g_stub.corrupt_blob_gets = 1;
        code = modnet_get_blob(h_hello, 0, &bytes, &len, NULL, &r);
        CHECK(code == MODNET_E_HASH && bytes == NULL && len == 0);
        CHECK(modnet_transient(code));
        g_stub.corrupt_blob_gets = 0;
        code = modnet_get_blob(h_missing, 0, &bytes, &len, NULL, &r);
        CHECK(code == MODNET_E_NOT_FOUND && r.status == 404 && strstr(r.message, "not found") != NULL);
    }

    /* ---- bundle upload and download ---- */
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"build\":1,\"files\":[{\"blob\":\"%s\"}],\"format\":1,\"id\":\"t.one\"}", h_hello);
    sha_of(text, h_bundle);
    code = modnet_put_bundle(h_bundle, text, strlen(text), NULL, &r);
    CHECK(code == MODNET_OK && r.status == 201 && r.created);
    CHECK(strcmp(g_stub.last_ctype, "application/json") == 0 && g_stub.last_cl_headers == 1 &&
          g_stub.last_cl_value == (long long)strlen(text));
    code = modnet_put_bundle(h_bundle, text, strlen(text), NULL, &r);
    CHECK(code == MODNET_OK && r.status == 200 && !r.created);
    CHECK(modnet_put_bundle(h_hello, text, strlen(text), NULL, &r) == MODNET_E_HASH);
    _snprintf_s(text2, sizeof text2, _TRUNCATE, "{\"build\":1,\"files\":[{\"blob\":\"%s\"}],\"format\":1,\"id\":\"t.two\"}", h_missing);
    sha_of(text2, h_bundle2);
    code = modnet_put_bundle(h_bundle2, text2, strlen(text2), NULL, &r);
    CHECK(code == MODNET_E_CONFLICT && r.status == 409 && strstr(r.message, "missing blobs") != NULL);
    {
        char extra[256], h_extra[65];
        _snprintf_s(extra, sizeof extra, _TRUNCATE, "{\"build\":1,\"extra\":{\"x\":1},\"files\":[{\"blob\":\"%s\"}],\"format\":1}", h_hello);
        sha_of(extra, h_extra);
        code = modnet_put_bundle(h_extra, extra, strlen(extra), NULL, &r);
        CHECK(code == MODNET_E_REFUSED && r.status == 422 && strstr(r.message, "unknown manifest key extra") != NULL);
        CHECK(!modnet_transient(code));
    }
    {
        char *got = NULL;
        size_t len = 0;
        code = modnet_get_bundle(h_bundle, &got, &len, NULL, &r);
        CHECK(code == MODNET_OK && len == strlen(text) && strcmp(got, text) == 0);
        free(got);
        got = NULL;
        g_stub.serve_bundle_member = 1;   /* the stored body carries "bundle" */
        code = modnet_get_bundle(h_bundle, &got, &len, NULL, &r);
        CHECK(code == MODNET_OK && got && strcmp(got, text) == 0);
        free(got);
        got = NULL;
        g_stub.serve_bundle_member = 0;
        CHECK(modnet_get_bundle(h_missing, &got, &len, NULL, &r) == MODNET_E_NOT_FOUND && !got);
    }
    {
        /* A manifest with a name, altered in transit, is refused. */
        char named[256], h_named[65], *got = NULL;
        size_t len = 0;
        _snprintf_s(named, sizeof named, _TRUNCATE, "{\"build\":1,\"files\":[{\"blob\":\"%s\"}],\"format\":1,\"name\":\"Named\"}", h_hello);
        sha_of(named, h_named);
        CHECK(modnet_put_bundle(h_named, named, strlen(named), NULL, &r) == MODNET_OK);
        strcpy_s(g_stub.corrupt_bundle, sizeof g_stub.corrupt_bundle, h_named);
        code = modnet_get_bundle(h_named, &got, &len, NULL, &r);
        CHECK(code == MODNET_E_HASH && got == NULL);
        g_stub.corrupt_bundle[0] = 0;
    }

    /* ---- claims ---- */
    {
        const char *set1[] = { h_bundle };
        code = modnet_claim("item-41", set1, 1, NULL, claim, secret, NULL, &r);
        CHECK(code == MODNET_OK && r.status == 201 && r.created && strlen(claim) == 32 && modnet_valid_hash(secret));
        code = modnet_claim("item-41", set1, 1, secret, claim2, secret2, NULL, &r);
        CHECK(code == MODNET_OK && r.status == 200 && !r.created && strcmp(claim2, claim) == 0 && strcmp(secret2, secret) == 0);
        CHECK(g_stub.claim_with_secret == 1);
        code = modnet_claim("item-41", set1, 1, h_other, claim2, secret2, NULL, &r);
        CHECK(code == MODNET_E_FORBIDDEN && r.status == 403 && claim2[0] == 0);
        {
            const char *unknown[] = { h_missing };
            CHECK(modnet_claim("item-42", unknown, 1, NULL, claim2, secret2, NULL, &r) == MODNET_E_CONFLICT);
        }
        CHECK(modnet_claim("bad\nref", set1, 1, NULL, claim2, secret2, NULL, &r) == MODNET_E_ARG);
        CHECK(modnet_release(claim, h_other, NULL, &r) == MODNET_E_FORBIDDEN);
        code = modnet_release(claim, secret, NULL, &r);
        CHECK(code == MODNET_OK && r.status == 204);
        CHECK(modnet_release(claim, secret, NULL, &r) == MODNET_E_NOT_FOUND);
        /* Nothing the client logged carries the secret. */
        CHECK(strstr(g_log, secret) == NULL);
    }

    /* ---- injected failures ---- */
    stub_fail_next("POST /mods/missing", 503);
    {
        const char *ask[] = { h_hello };
        code = modnet_missing(ask, 1, &missing, &n, NULL, &r);
        CHECK(code == MODNET_E_UNAVAILABLE && r.status == 503 && modnet_transient(code));
        CHECK(strstr(r.message, "injected failure 503") != NULL);
        free(missing);
        missing = NULL;
    }
    stub_fail_next("PUT /mods/blob/", 429);
    CHECK(modnet_put_blob(h_hello, "hello", 5, NULL, &r) == MODNET_E_RATE);
    stub_fail_next("PUT /mods/blob/", 451);
    code = modnet_put_blob(h_hello, "hello", 5, NULL, &r);
    CHECK(code == MODNET_E_BLOCKED && !modnet_transient(code));
    {
        /* Cancelled before the request is made. */
        volatile long cancel = 1;
        modnet_io io = { &cancel, NULL, NULL };
        before = g_stub.requests;
        CHECK(modnet_put_blob(h_hello, "hello", 5, &io, &r) == MODNET_E_CANCELLED);
        CHECK(g_stub.requests == before);
    }
    {
        /* Upload progress reaches the body length. */
        modnet_io io = { NULL, upload_progress, NULL };
        static unsigned char big[600 * 1024];
        char h_big[65];
        size_t i;
        for (i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 7 + 3);
        sh_mb_sha256_hex(big, sizeof big, h_big);
        g_up_done = g_up_total = 0;
        g_up_calls = 0;
        CHECK(modnet_put_blob(h_big, big, sizeof big, &io, &r) == MODNET_OK && r.sent == sizeof big);
        CHECK(g_up_calls >= 3 && g_up_done == sizeof big && g_up_total == sizeof big);
        CHECK(g_stub.last_cl_headers == 1 && g_stub.last_cl_value == (long long)sizeof big);
    }

    /* ---- a refused connection ---- */
    CHECK(modnet_set_base_url("http://127.0.0.1:1"));
    code = modnet_put_blob(h_hello, "hello", 5, NULL, &r);
    CHECK(code == MODNET_E_NET || code == MODNET_E_TIMEOUT);
    CHECK(modnet_transient(code) && r.status == 0);
    CHECK(modnet_set_base_url(url));

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
    printf("modnet_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
