/* modnet_test -- the WinHTTP store client against a loopback stub of the store (mod_store_stub.h): URL and argument
 * rules, a unit's archive request, injected failures, cancel, and the job helper. The optional --idle-timeout and
 * --slow-progress cases exercise the real 30-second file download timeout. */
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
static HANDLE wrote_prefix, release_server;
static void progress(void *ctx, unsigned long long done, unsigned long long total)
{
    (void)ctx;
    g_pr_calls++;
    g_pr_done = done;
    g_pr_total = total;
    if (wrote_prefix && done == 5) SetEvent(wrote_prefix);
}

static const char stream_hash[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char stream_body[] = "abcdefghijklmno";
static int stream_end, stream_wrong, stream_ignore, stream_limit;

static void ranged_reply(SOCKET s, const char *headers)
{
    int start = 0, total = sizeof stream_body - 1, end = stream_end;
    const char *range = strstr(headers, "Range: bytes=");
    char head[256];
    if (range) sscanf_s(range, "Range: bytes=%d-", &start);
    if (stream_ignore) start = 0;
    if (start < 0 || start > end || end > total) return;
    if (range && !stream_ignore) {
        int last = total - 1;
        if (stream_limit && start + stream_limit < total) last = start + stream_limit - 1;
        if (end > last + 1) end = last + 1;
        snprintf(head, sizeof head, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %d-%d/%d\r\n"
                 "Content-Length: %d\r\nConnection: close\r\n\r\n",
                 start + stream_wrong, last, total, last-start+1);
    } else snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", total);
    send(s, head, (int)strlen(head), 0);
    send(s, stream_body + start, end - start, 0);
    if (release_server) WaitForSingleObject(release_server, 40000);
}

static void stalled_chunk_reply(SOCKET s, const char *headers)
{
    static const char first[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                                "5\r\nabcde\r\nA;x=slow";
    (void)headers;
    send(s, first, sizeof first - 1, 0);
    /* The connection carries traffic but the next chunk never delivers any payload. */
    for (int i = 0; i < 40 && WaitForSingleObject(release_server, 1000) == WAIT_TIMEOUT; i++)
        if (send(s, "x", 1, 0) != 1) break;
}

static void slow_reply(SOCKET s, const char *headers)
{
    static const char first[] = "HTTP/1.1 200 OK\r\nContent-Length: 15\r\nConnection: close\r\n\r\nabc";
    (void)headers;
    send(s, first, sizeof first - 1, 0);
    for (int i = 3; i < 15; i += 3) {
        Sleep(8000);
        if (send(s, stream_body + i, 3, 0) != 3) break;
    }
}

static unsigned char bulk_block[16384];
#define BULK_BLOCKS 256

static void bulk_reply(SOCKET s, const char *headers)
{
    char head[128];
    int n = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     sizeof bulk_block * BULK_BLOCKS);
    (void)headers;
    send(s, head, n, 0);
    for (int block = 0; block < BULK_BLOCKS; block++) {
        for (size_t off = 0; off < sizeof bulk_block; off += n) {
            n = send(s, (const char *)bulk_block + off, (int)(sizeof bulk_block - off), 0);
            if (n <= 0) return;
        }
    }
}

static void test_bulk_download(const char *tmp)
{
    char out[MAX_PATH * 2];
    unsigned char block[sizeof bulk_block];
    modnet_result result;
    modnet_io io = {NULL, progress, NULL};
    FILE *f = NULL;
    int blocks = 0;
    for (size_t i = 0; i < sizeof bulk_block; i++) bulk_block[i] = (unsigned char)(i * 41);
    snprintf(out, sizeof out, "%s\\bulk.part", tmp);
    g_stub.stream = bulk_reply;
    CHECK(modnet_get_blob(stream_hash, out, 0, &io, &result) == MODNET_OK && result.requests == 1);
    CHECK(result.received == sizeof bulk_block * BULK_BLOCKS && g_pr_done == result.received && g_pr_total == g_pr_done);
    CHECK(fopen_s(&f, out, "rb") == 0);
    if (f) {
        while (fread(block, 1, sizeof block, f) == sizeof block && !memcmp(block, bulk_block, sizeof block)) blocks++;
        CHECK(blocks == BULK_BLOCKS && feof(f));
        fclose(f);
    }
    printf("4 MiB in %.1f ms\n", result.ms);
    g_stub.stream = NULL;
}

static void test_resume_progress(const char *tmp)
{
    char out[MAX_PATH * 2], bytes[16] = {0};
    modnet_result r;
    modnet_io io = {NULL, progress, NULL};
    FILE *f = NULL;
    snprintf(out, sizeof out, "%s\\resume-test.part", tmp);
    g_stub.stream = ranged_reply;
    stream_end = 5;
    CHECK(modnet_get_blob(stream_hash, out, 0, &io, &r) != MODNET_OK);
    CHECK(g_pr_done == 5 && g_pr_total == 15);
    stream_end = 10;
    CHECK(modnet_get_blob(stream_hash, out, 5, &io, &r) != MODNET_OK);
    CHECK(g_pr_done == 10 && g_pr_total == 15);
    stream_end = 15;
    stream_wrong = 1;
    CHECK(modnet_get_blob(stream_hash, out, 10, &io, &r) == MODNET_E_PROTOCOL);
    CHECK(g_pr_done == 10);
    stream_wrong = 0;
    CHECK(modnet_get_blob(stream_hash, out, 10, &io, &r) == MODNET_OK && r.received == 5);
    CHECK(g_pr_done == 15 && g_pr_total == 15);
    stream_limit = 3;
    CHECK(modnet_get_blob(stream_hash, out, 5, &io, &r) == MODNET_OK && r.requests == 4 && r.received == 10);
    CHECK(g_pr_done == 15 && g_pr_total == 15);
    stream_limit = 0;
    stream_ignore = 1;
    CHECK(modnet_get_blob(stream_hash, out, 10, &io, &r) == MODNET_OK && r.requests == 1);
    CHECK(g_pr_done == 15 && g_pr_total == 15);
    CHECK(fopen_s(&f, out, "rb") == 0);
    if (f) { CHECK(fread(bytes, 1, sizeof bytes, f) == 15 && !memcmp(bytes, stream_body, 15)); fclose(f); }
    stream_ignore = 0;
    g_stub.stream = NULL;
}

typedef struct stalled_get {
    const char *out;
    volatile LONG cancel;
    int code;
    modnet_result result;
} stalled_get;

static const unsigned char selection[] = {0x05};

static DWORD WINAPI read_stalled(LPVOID context)
{
    stalled_get *get = context;
    modnet_io io = {&get->cancel, progress, NULL};
    get->code = modnet_get_unit_archive(stream_hash, selection, sizeof selection, get->out, 0, &io, &get->result);
    return 0;
}

static void test_stalled_download(const char *tmp, int idle_timeout)
{
    char out[MAX_PATH * 2], bytes[16] = {0};
    HANDLE worker;
    stalled_get get = {0};
    modnet_result resumed;
    FILE *f = NULL;
    ULONGLONG started, elapsed;
    snprintf(out, sizeof out, "%s\\stalled.part", tmp);
    get.out = out;
    wrote_prefix = CreateEventW(NULL, TRUE, FALSE, NULL);
    release_server = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(wrote_prefix && release_server);
    g_stub.stream = idle_timeout ? stalled_chunk_reply : ranged_reply;
    stream_end = 5;
    worker = CreateThread(NULL, 0, read_stalled, &get, 0, NULL);
    CHECK(worker && WaitForSingleObject(wrote_prefix, 2500) == WAIT_OBJECT_0);
    /* Let the worker enter its next WinHTTP read while the server remains open. */
    CHECK(WaitForSingleObject(worker, 250) == WAIT_TIMEOUT);
    started = GetTickCount64();
    if (!idle_timeout) InterlockedExchange(&get.cancel, 1);
    CHECK(WaitForSingleObject(worker, idle_timeout ? 35000 : 1500) == WAIT_OBJECT_0);
    elapsed = GetTickCount64() - started;
    SetEvent(release_server);
    WaitForSingleObject(worker, INFINITE);
    CHECK(get.code == (idle_timeout ? MODNET_E_TIMEOUT : MODNET_E_CANCELLED));
    CHECK(!idle_timeout || elapsed >= 28000);
    CHECK(get.result.received == 5);
    printf("%s after %llu ms, retained %llu bytes\n", idle_timeout ? "idle timeout" : "cancel", elapsed,
           get.result.received);
    CloseHandle(worker);
    EnterCriticalSection(&g_stub.cs);
    CloseHandle(wrote_prefix); CloseHandle(release_server);
    wrote_prefix = release_server = NULL;
    g_stub.stream = ranged_reply;
    stream_end = 15;
    LeaveCriticalSection(&g_stub.cs);
    CHECK(modnet_get_unit_archive(stream_hash, selection, sizeof selection, out, 5, NULL, &resumed) == MODNET_OK);
    CHECK(resumed.status == 206 && resumed.received == 10 && strstr(g_stub.last_get, "/archive?want=BQ"));
    CHECK(fopen_s(&f, out, "rb") == 0);
    if (f) { CHECK(fread(bytes, 1, sizeof bytes, f) == 15 && !memcmp(bytes, stream_body, 15)); fclose(f); }
    g_stub.stream = NULL;
}

static void test_slow_progress(const char *tmp)
{
    char out[MAX_PATH * 2], bytes[16] = {0};
    modnet_result result;
    modnet_io io = {NULL, progress, NULL};
    ULONGLONG started = GetTickCount64();
    FILE *f = NULL;
    snprintf(out, sizeof out, "%s\\slow.part", tmp);
    g_stub.stream = slow_reply;
    g_pr_calls = 0;
    CHECK(modnet_get_blob(stream_hash, out, 0, &io, &result) == MODNET_OK && result.received == 15);
    CHECK(GetTickCount64() - started >= 31000);
    CHECK(g_pr_calls >= 5 && g_pr_done == 15 && g_pr_total == 15);
    CHECK(fopen_s(&f, out, "rb") == 0);
    if (f) { CHECK(fread(bytes, 1, sizeof bytes, f) == 15 && !memcmp(bytes, stream_body, 15)); fclose(f); }
    printf("active download finished after %llu ms\n", GetTickCount64() - started);
    g_stub.stream = NULL;
}

int main(int argc, char **argv)
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

    test_bulk_download(tmp);
    if (argc == 2 && !strcmp(argv[1], "--bulk")) goto done;
    test_resume_progress(tmp);
    test_stalled_download(tmp, 0);
    if (argc == 2 && !strcmp(argv[1], "--idle-timeout")) test_stalled_download(tmp, 1);
    if (argc == 2 && !strcmp(argv[1], "--slow-progress")) test_slow_progress(tmp);

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
        _snprintf_s(out, sizeof out, _TRUNCATE, "%s\\unit-long.zip", tmp);
        code = modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r);
        CHECK(code != MODNET_E_CONFIG && code != MODNET_E_ARG && r.status != 0);   /* the request reached the store */
        free(longer);
    }
    CHECK(modnet_set_base_url(url));

    /* ---- injected failures, through a unit's archive ---- */
    {
        static const char unit[] = "abababababababababababababababababababababababababababababababab";
        char out[MAX_PATH * 2];
        _snprintf_s(out, sizeof out, _TRUNCATE, "%s\\unit-fail.zip", tmp);
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
        stub_fail_next("GET /mods/unit/", 416);
        CHECK(modnet_get_unit_archive(unit, NULL, 0, out, 1, NULL, &r) == MODNET_OK && r.status == 416);
        stub_fail_next("GET /mods/unit/", 416);
        CHECK(modnet_get_unit_archive(unit, NULL, 0, out, 0, NULL, &r) != MODNET_OK && r.status == 416);
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

done:
    modnet_shutdown();
    stub_stop();
    mt_remove_tree(tmp);
    printf("modnet_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
