/* modnet_test -- the WinHTTP store client against a loopback stub of the
 * store (mod_store_stub.h): base URL rules, HEAD, PUT and GET of a build
 * archive (streamed from and to a file, Content-Length and Content-Type on
 * the PUT, idempotent re-upload, the store's 422 cause carried back, a build
 * larger than one request sent in parts and resumed after a failed part, the
 * scripts count of a HEAD, a failed GET leaving no file),
 * claims with "builds" and releases (a release deletes what no claim holds),
 * injected 5xx, 429 and 451, a refused connection, cancel, progress, and the
 * job helper's progress, cancel and one-job rule. No real store is contacted.
 */
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

/* A built mod folder under `dir` with one carried file of `n` bytes, and its
 * archive at `zip` as the product writes it (mod.json, assets/<request>).
 * Returns 1 with the build hash and the archive's size. */
static int make_build(const char *dir, const char *salt, size_t n, const char *zip, char hash[65], unsigned long long *size)
{
    char rel[160], cause[SH_MB_CAUSE_MAX], mj[MAX_PATH * 2];
    unsigned char *bytes = (unsigned char *)malloc(n + 1), *text;
    size_t i, len = 0;
    sh_mb_bundle b;
    sh_zip_writer w;
    wchar_t *wzip;
    int ok;
    FILE *f;
    if (!bytes) return 0;
    for (i = 0; i < n; i++) bytes[i] = (unsigned char)(i * 13 + salt[0]);
    mt_write(dir, "mod.json", "{\"name\":\"Net test\",\"version\":\"1.0.0\"}");
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\image\\mods\\%s\\a.bimage", salt);
    {
        char full[MAX_PATH * 2];
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", dir, rel);
        mt_mkdirs_for(full);
        if (fopen_s(&f, full, "wb") || !f) { free(bytes); return 0; }
        fwrite(bytes, 1, n, f);
        fclose(f);
    }
    memset(&b, 0, sizeof b);
    ok = sh_mb_build(dir, salt, NULL, &b) && sh_mb_write(&b, dir, cause, sizeof cause);
    if (!ok) { fprintf(stderr, "build %s: %s\n", dir, b.first_cause); sh_mb_free(&b); free(bytes); return 0; }
    strcpy_s(hash, 65, b.hash);
    _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\mod.json", dir);
    text = sh_mb_read_file(mj, 1 << 20, &len);
    memset(&w, 0, sizeof w);
    wzip = sh_mb_wpath(zip);
    ok = text && wzip && sh_zip_writer_open(&w, wzip) && sh_zip_writer_add(&w, "mod.json", text, len);
    if (ok) {
        char entry[200];
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "assets/%s", b.files[0].request);
        ok = sh_zip_writer_add(&w, entry, bytes, n) && sh_zip_writer_finish(&w);
    }
    if (ok) *size = w.off;
    sh_zip_writer_free(&w);
    free(wzip);
    free(text);
    free(bytes);
    sh_mb_free(&b);
    return ok;
}

static unsigned char *read_all(const char *path, size_t *len)
{
    return sh_mb_read_file(path, (size_t)-1, len);
}

int main(void)
{
    char url[64], tmp[MAX_PATH], dir[MAX_PATH * 2], zip[MAX_PATH * 2], zip2[MAX_PATH * 2], got[MAX_PATH * 2];
    char ha[65], hb[65], h_none[65], claim[33], secret[65], claim2[33], secret2[65];
    unsigned long long size_a = 0, size_b = 0, head = 0;
    modnet_result r;
    int code, before;

    CHECK(stub_start() > 0);
    stub_url(url, sizeof url);
    CHECK(mt_temp_dir("modnet", tmp, sizeof tmp));

    /* ---- base URL rules ---- */
    CHECK(!modnet_set_base_url("http://example.com"));
    CHECK(!modnet_set_base_url("ftp://127.0.0.1/"));
    CHECK(!modnet_set_base_url("https://store.test/x?y=1"));
    CHECK(modnet_set_base_url("https://store.test/base/"));
    {
        char base[MODNET_URL_MAX];
        CHECK(modnet_base_url(base, sizeof base) && strcmp(base, "https://store.test/base/") == 0);
        CHECK(modnet_set_base_url(NULL));
        /* No settings service in this test: the documented default applies. */
        CHECK(modnet_base_url(base, sizeof base) && strcmp(base, MODNET_DEFAULT_STORE) == 0);
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
    CHECK(modnet_head_build("nothex", NULL, NULL, NULL, &r) == MODNET_E_ARG);

    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\a", tmp);
    _snprintf_s(zip, sizeof zip, _TRUNCATE, "%s\\a.zip", tmp);
    CHECK(make_build(dir, "netaaaa1", 700 * 1024, zip, ha, &size_a));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\b", tmp);
    _snprintf_s(zip2, sizeof zip2, _TRUNCATE, "%s\\b.zip", tmp);
    CHECK(make_build(dir, "netbbbb2", 100, zip2, hb, &size_b));
    sh_mb_sha256_hex("never stored", 12, h_none);

    /* ---- HEAD of a build the store does not hold ---- */
    code = modnet_head_build(ha, &head, NULL, NULL, &r);
    CHECK(code == MODNET_E_NOT_FOUND && r.status == 404 && r.requests == 1 && head == 0);
    CHECK(!modnet_unreachable(code) && !modnet_transient(code));

    /* ---- PUT: streamed from the file, created, then idempotent ---- */
    {
        modnet_io io = { NULL, progress, NULL };
        g_pr_calls = 0;
        code = modnet_put_build(ha, zip, size_a, &io, &r);
        CHECK(code == MODNET_OK && r.status == 201 && r.created == 1 && r.sent == size_a);
        CHECK(g_stub.last_cl_headers == 1 && g_stub.last_cl_value == (long long)size_a);
        CHECK(strcmp(g_stub.last_ctype, "application/zip") == 0 && strcmp(g_stub.last_method, "PUT") == 0);
        CHECK(g_pr_calls >= 3 && g_pr_done == size_a && g_pr_total == size_a);   /* 256 KiB chunks */
    }
    code = modnet_put_build(ha, zip, size_a, NULL, &r);
    CHECK(code == MODNET_OK && r.status == 200 && r.created == 0);
    CHECK(g_stub.build_created == 1 && g_stub.build_puts == 2);
    code = modnet_head_build(ha, &head, NULL, NULL, &r);
    CHECK(code == MODNET_OK && r.status == 200 && head == size_a && r.received == 0);

    /* ---- PUT refusals ---- */
    /* The store's own check of the archive: carried back as its cause. */
    code = modnet_put_build(hb, zip, size_a, NULL, &r);
    CHECK(code == MODNET_E_REFUSED && r.status == 422 && strstr(r.message, "build does not match the path"));
    CHECK(!stub_has_build(hb) && !modnet_transient(code));
    /* The size given must be the file's. */
    before = g_stub.requests;
    CHECK(modnet_put_build(hb, zip2, size_b + 1, NULL, &r) == MODNET_E_ARG && g_stub.requests == before);
    CHECK(modnet_put_build(hb, zip2, size_b, NULL, &r) == MODNET_OK && r.created);

    /* ---- a build larger than one request goes in parts, resumably ---- */
    {
        char hc[65], zip3[MAX_PATH * 2], got3[MAX_PATH * 2];
        unsigned long long size_c = 0, head_c = 0;
        unsigned scripts = 99;
        modnet_io io = { NULL, progress, NULL };
        unsigned char *x, *y;
        size_t xl = 0, yl = 0;
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\c", tmp);
        _snprintf_s(zip3, sizeof zip3, _TRUNCATE, "%s\c.zip", tmp);
        CHECK(make_build(dir, "netcccc3", (size_t)MODNET_PART_SIZE + 4096, zip3, hc, &size_c));
        CHECK(size_c > MODNET_PART_SIZE);
        /* Part 2 fails once: the upload starts again and sends only part 2. */
        g_stub.fail_part = 2;
        memset(g_stub.part_sent, 0, sizeof g_stub.part_sent);
        g_pr_calls = 0;
        code = modnet_put_build(hc, zip3, size_c, &io, &r);
        CHECK(code == MODNET_OK && r.created == 1);
        CHECK(g_stub.part_starts == 2 && g_stub.part_sent[1] == 1 && g_stub.part_sent[2] == 2 && g_stub.part_completes == 1);
        CHECK(strcmp(g_stub.last_ctype, "application/json") == 0);   /* the last request: complete */
        CHECK(g_pr_done == size_c && g_pr_total == size_c);
        /* Sent again: the store holds it, nothing is uploaded. */
        before = g_stub.part_puts;
        CHECK(modnet_put_build(hc, zip3, size_c, NULL, &r) == MODNET_OK && r.created == 0 && g_stub.part_puts == before);
        /* Downloaded whole, streamed to the file, byte for byte. */
        _snprintf_s(got3, sizeof got3, _TRUNCATE, "%s\got3.zip", tmp);
        g_pr_calls = 0;
        code = modnet_get_build(hc, got3, &io, &r);
        CHECK(code == MODNET_OK && r.received == size_c && g_pr_done == size_c && g_pr_total == size_c && g_pr_calls > 100);
        x = sh_mb_read_file(got3, (size_t)-1, &xl);
        y = sh_mb_read_file(zip3, (size_t)-1, &yl);
        CHECK(x && y && xl == yl && xl == size_c && !memcmp(x, y, xl));
        free(x);
        free(y);
        /* HEAD says how many scripts a build carries. */
        CHECK(modnet_head_build(hc, &head_c, &scripts, NULL, &r) == MODNET_OK && head_c == size_c && scripts == 0);
        EnterCriticalSection(&g_stub.cs);
        stub_find(g_stub.builds, g_stub.nbuilds, hc)->scripts = 15;
        LeaveCriticalSection(&g_stub.cs);
        CHECK(modnet_head_build(hc, NULL, &scripts, NULL, &r) == MODNET_OK && scripts == 15);
        mt_remove_tree(dir);
        DeleteFileA(zip3);
        DeleteFileA(got3);
    }

    /* ---- GET into a file ---- */
    _snprintf_s(got, sizeof got, _TRUNCATE, "%s\\got.zip", tmp);
    {
        modnet_io io = { NULL, progress, NULL };
        unsigned char *x, *y;
        size_t xl = 0, yl = 0;
        g_pr_calls = 0;
        code = modnet_get_build(ha, got, &io, &r);
        CHECK(code == MODNET_OK && r.status == 200 && r.received == size_a);
        CHECK(g_pr_calls >= 1 && g_pr_done == size_a && g_pr_total == size_a);
        x = read_all(got, &xl);
        y = read_all(zip, &yl);
        CHECK(x && y && xl == yl && xl == size_a && !memcmp(x, y, xl));
        free(x);
        free(y);
    }
    code = modnet_get_build(h_none, got, NULL, &r);
    CHECK(code == MODNET_E_NOT_FOUND && r.status == 404 && !mt_exists(got));
    stub_fail_next("GET /mods/build/", 503);
    code = modnet_get_build(ha, got, NULL, &r);
    CHECK(code == MODNET_E_UNAVAILABLE && modnet_transient(code) && modnet_unreachable(code) && !mt_exists(got));
    CHECK(strstr(r.message, "injected failure 503") != NULL);

    /* ---- claims ---- */
    {
        const char *set1[] = { ha, hb };
        code = modnet_claim("item-41", set1, 2, NULL, claim, secret, NULL, &r);
        CHECK(code == MODNET_OK && r.status == 201 && r.created && strlen(claim) == 32 && modnet_valid_hash(secret));
        CHECK(strcmp(g_stub.last_path, "/mods/claim") == 0 && stub_claim_builds("item-41", NULL) == 2);
        code = modnet_claim("item-41", set1, 1, secret, claim2, secret2, NULL, &r);
        CHECK(code == MODNET_OK && r.status == 200 && !r.created && strcmp(claim2, claim) == 0 && strcmp(secret2, secret) == 0);
        CHECK(g_stub.claim_with_secret == 1 && stub_claim_builds("item-41", NULL) == 1);   /* the set is replaced */
        code = modnet_claim("item-41", set1, 1, h_none, claim2, secret2, NULL, &r);
        CHECK(code == MODNET_E_FORBIDDEN && r.status == 403 && claim2[0] == 0);
        {
            const char *unknown[] = { h_none };
            CHECK(modnet_claim("item-42", unknown, 1, NULL, claim2, secret2, NULL, &r) == MODNET_E_CONFLICT);
        }
        CHECK(modnet_claim("bad\nref", set1, 1, NULL, claim2, secret2, NULL, &r) == MODNET_E_ARG);
        CHECK(modnet_release(claim, h_none, NULL, &r) == MODNET_E_FORBIDDEN);
        /* The release deletes every build no live claim names: both here. */
        code = modnet_release(claim, secret, NULL, &r);
        CHECK(code == MODNET_OK && r.status == 204);
        CHECK(!stub_has_build(ha) && !stub_has_build(hb) && g_stub.builds_deleted == 3);   /* and the unclaimed large one */
        CHECK(modnet_head_build(ha, NULL, NULL, NULL, &r) == MODNET_E_NOT_FOUND);
        CHECK(modnet_release(claim, secret, NULL, &r) == MODNET_E_NOT_FOUND);
        /* Nothing the client logged carries the secret. */
        CHECK(strstr(g_log, secret) == NULL);
    }

    /* ---- one claim names any number of builds ---- */
    {
        static char many[600][65];
        static const char *list[600];
        int i, deleted = g_stub.builds_deleted;
        for (i = 0; i < 600; i++) {
            char seed[16];
            _snprintf_s(seed, sizeof seed, _TRUNCATE, "many-%d", i);
            sh_mb_sha256_hex(seed, strlen(seed), many[i]);
            stub_put_raw(many[i], "x", 1);
            list[i] = many[i];
        }
        code = modnet_claim("a map naming 600 mods", list, 600, NULL, claim, secret, NULL, &r);
        CHECK(code == MODNET_OK && r.status == 201 && stub_claim_builds("a map naming 600 mods", NULL) == 600);
        CHECK(modnet_release(claim, secret, NULL, &r) == MODNET_OK && g_stub.builds_deleted == deleted + 600);
    }

    /* ---- injected failures ---- */
    stub_fail_next("HEAD /mods/build/", 451);
    code = modnet_head_build(ha, NULL, NULL, NULL, &r);
    CHECK(code == MODNET_E_BLOCKED && !modnet_transient(code) && !modnet_unreachable(code));
    stub_fail_next("PUT /mods/build/", 429);
    code = modnet_put_build(ha, zip, size_a, NULL, &r);
    CHECK(code == MODNET_E_RATE && modnet_transient(code));
    stub_fail_next("PUT /mods/build/", 413);
    CHECK(modnet_put_build(ha, zip, size_a, NULL, &r) == MODNET_E_TOO_LARGE);
    {
        /* Cancelled before the request is made. */
        volatile long cancel = 1;
        modnet_io io = { &cancel, NULL, NULL };
        before = g_stub.requests;
        CHECK(modnet_put_build(ha, zip, size_a, &io, &r) == MODNET_E_CANCELLED);
        CHECK(modnet_head_build(ha, NULL, NULL, &io, &r) == MODNET_E_CANCELLED);
        CHECK(g_stub.requests == before);
    }

    /* ---- a refused connection ---- */
    CHECK(modnet_set_base_url("http://127.0.0.1:1"));
    code = modnet_head_build(ha, NULL, NULL, NULL, &r);
    CHECK(code == MODNET_E_NET || code == MODNET_E_TIMEOUT);
    CHECK(modnet_transient(code) && modnet_unreachable(code) && r.status == 0);
    CHECK(modnet_get_build(ha, got, NULL, &r) != MODNET_OK && !mt_exists(got));
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
    mt_remove_tree(tmp);
    printf("modnet_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
