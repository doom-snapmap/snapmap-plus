/* modsync_test -- upload, download, claims and the retry queue between a
 * temporary local store (modstore.c) and a loopback stub of the mod store
 * (mod_store_stub.h):
 *   missing negotiation (a blob two bundles share is sent once), blob
 *   upload idempotence, the exact canonical manifest as the bundle body, a
 *   409 at the manifest renegotiated, claim and re-claim with the secret
 *   kept in the claims section of mods\snapmap-plus.json, release, a 503
 *   queued in its pending section and retried, a 422 not queued, a cancelled
 *   push queued, a map pinning an earlier build of a folder rebuilt in place
 *   uploading its current build, claims named by hash, the gate's probe
 *   (found, 404, 451, 503, nothing listening, a borrow the game lacks), a
 *   second build of an installed key downloaded beside it as <key>@<hash16>,
 *   and downloads into empty stores: installed as mods\<id> with
 *   mod.json, verified and in the serving index, the same bundle again
 *   fetched from nowhere, a file an installed mod already holds copied
 *   instead of fetched, and a blob or manifest whose bytes do not match its
 *   hash refused with nothing installed.
 */
#include "mod_store_stub.h"
#include "../src/backend/modnet.h"
#include "../src/backend/modsync.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modmanifest.h"
#include "mod_test_util.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static char g_log[256 * 1024];
static size_t g_log_n;
static int g_echo;
void backend_log(const char *line)
{
    size_t n = line ? strlen(line) : 0;
    if (g_echo && line) printf("    %s\n", line);
    if (n && g_log_n + n + 2 < sizeof g_log) { memcpy(g_log + g_log_n, line, n); g_log_n += n; g_log[g_log_n++] = '\n'; g_log[g_log_n] = 0; }
}

/* A mod folder with raw image files (no needs, so no stock resolver). */
static void make_mod(const char *dir, const char *id, const char *extra_json, const char *const *files, const char *const *bytes)
{
    char text[512];
    int i;
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"id\":\"%s\",\"version\":\"1.0.0\",\"name\":\"%s\",\"author\":\"tester\"%s}",
                id, id, extra_json ? extra_json : "");
    mt_write(dir, "mod.json", text);
    for (i = 0; files[i]; i++) {
        char rel[MAX_PATH];
        char *c;
        strcpy_s(rel, sizeof rel, files[i]);
        for (c = rel; *c; c++) if (*c == '/') *c = '\\';
        mt_write(dir, rel, bytes[i]);
    }
}

static int install(const char *folder, char key[SH_MS_KEY_MAX + 1], char hash[65])
{
    sh_ms_install_stats st;
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX];
    int ok = sh_ms_install_from_folder(folder, NULL, key, SH_MS_KEY_MAX + 1, &st, &b, cause, sizeof cause);
    if (!ok) fprintf(stderr, "install %s: %s\n", folder, cause);
    if (ok) strcpy_s(hash, 65, b.hash);
    sh_mb_free(&b);
    return ok;
}

static unsigned count_files(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    unsigned n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char sub[MAX_PATH * 2];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { n++; continue; }
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        n += count_files(sub);   /* files below, recursively */
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static int file_contains(const char *path, const char *needle)
{
    size_t len = 0;
    unsigned char *t = sh_mb_read_file(path, 1 << 20, &len);
    int hit = t && strstr((const char *)t, needle) != NULL;
    free(t);
    return hit;
}

static const char *const A_FILES[] = { "assets/generated/image/mods/test.alpha/a.bimage", "assets/generated/image/mods/test.alpha/shared.bimage", NULL };
static const char *const A_BYTES[] = { "alpha only bytes", "bytes both bundles carry", NULL };
static const char *const B_FILES[] = { "assets/generated/image/mods/test.beta/shared.bimage", "assets/generated/image/mods/test.beta/b.bimage", NULL };
static const char *const B_BYTES[] = { "bytes both bundles carry", "beta only bytes", NULL };
static const char *const G_FILES[] = { "assets/generated/image/mods/test.gamma/g.bimage", NULL };
static const char *const G_BYTES[] = { "gamma bytes", NULL };
static const char *const D_FILES[] = { "assets/generated/image/mods/test.delta/d.bimage", NULL };
static const char *const D_BYTES[] = { "delta bytes", NULL };
static const char *const D2_BYTES[] = { "delta bytes, edited", NULL };
static const char *const X_FILES[] = { "assets/generated/image/mods/test.extra/x.bimage", NULL };
static const char *const X_BYTES[] = { "extra bytes", NULL };
static const char *const E_FILES[] = { "assets/generated/decls/snapeditorentitydef/func/rotate.decl", NULL };
static const char *const E_BYTES[] = { "{ entityDef \"func/rotate\" }", NULL };

static const char *const G2_BYTES[] = { "gamma bytes, second build", NULL };
static const char *const R_FILES[] = { "assets/generated/image/mods/test.borrow/rock.bimage", "assets/generated/image/mods/test.borrow/own.bimage", NULL };
static const char *const R_BYTES[] = { "ROCK-GAME-BYTES-0123456789", "borrow's own bytes", NULL };
#define ROCK_SOURCE "generated/image/stock/rock.bimage"

/* A fake installed game holding one file (ROCK_SOURCE) with R_BYTES[0], and
 * one of another revision that holds other bytes there. */
static int g_game_lacks;
static int rock_same(unsigned long long size, const char *sha)
{
    char h[65];
    sh_mb_sha256_hex(R_BYTES[0], strlen(R_BYTES[0]), h);
    return size == strlen(R_BYTES[0]) && !strcmp(h, sha);
}
static int rock_match(void *ctx, const char *request, unsigned long long size, const char *sha, char *source, size_t cap)
{
    (void)ctx; (void)request;
    if (g_game_lacks || !rock_same(size, sha)) return SH_MB_STOCK_ABSENT;
    strcpy_s(source, cap, ROCK_SOURCE);
    return SH_MB_STOCK_OK;
}
static int rock_identify(void *ctx, const char *source, char sha[65], unsigned long long *size)
{
    (void)ctx;
    if (g_game_lacks || strcmp(source, ROCK_SOURCE)) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex(R_BYTES[0], strlen(R_BYTES[0]), sha);
    *size = strlen(R_BYTES[0]);
    return SH_MB_STOCK_OK;
}
static int rock_verify(void *ctx, const char *source, const char *sha, unsigned long long *size)
{
    char h[65];
    (void)ctx;
    if (strcmp(source, ROCK_SOURCE)) return SH_MB_STOCK_ABSENT;
    if (g_game_lacks) return SH_MB_STOCK_MISMATCH;
    sh_mb_sha256_hex(R_BYTES[0], strlen(R_BYTES[0]), h);
    if (strcmp(h, sha)) return SH_MB_STOCK_MISMATCH;
    if (size) *size = strlen(R_BYTES[0]);
    return SH_MB_STOCK_OK;
}
static int rock_read(void *ctx, const char *source, const char *sha, unsigned char **bytes, size_t *len)
{
    unsigned long long size = 0;
    int r = rock_verify(ctx, source, sha, &size);
    if (r != SH_MB_STOCK_OK) return r;
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)size + 1);
    if (!*bytes) return SH_MB_STOCK_UNAVAILABLE;
    memcpy(*bytes, R_BYTES[0], (size_t)size);
    *len = (size_t)size;
    return SH_MB_STOCK_OK;
}
static int rock_has_verify(void *ctx, const char *source, const char *sha, unsigned long long *size)
{ g_game_lacks = 0; return rock_verify(ctx, source, sha, size); }
static int rock_lacks_verify(void *ctx, const char *source, const char *sha, unsigned long long *size)
{ g_game_lacks = 1; return rock_verify(ctx, source, sha, size); }
static int rock_has_match(void *ctx, const char *request, unsigned long long size, const char *sha, char *source, size_t cap)
{ g_game_lacks = 0; return rock_match(ctx, request, size, sha, source, cap); }
static const sh_mb_stock_ops GAME_HAS_ROCK = { NULL, rock_has_match, rock_identify, rock_has_verify, rock_read };
static const sh_mb_stock_ops GAME_LACKS_ROCK = { NULL, rock_match, rock_identify, rock_lacks_verify, rock_read };

/* The builder's stock resolver: every shadow names a stock resource. */
static int stock_yes(void *context, const char *request) { (void)context; (void)request; return 1; }

/* Another program holds the record open without read sharing for `ms`. */
typedef struct sync_hold { char path[MAX_PATH * 2]; DWORD ms; HANDLE taken; } sync_hold;
static DWORD WINAPI sync_hold_thread(LPVOID p)
{
    sync_hold *r = (sync_hold *)p;
    HANDLE h = CreateFileA(r->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    SetEvent(r->taken);
    Sleep(r->ms);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(r);
    return 0;
}
static HANDLE sync_hold_record(const char *path, DWORD ms)
{
    sync_hold *r = (sync_hold *)calloc(1, sizeof *r);
    HANDLE t, taken;
    if (!r) return NULL;
    strncpy_s(r->path, sizeof r->path, path, _TRUNCATE);
    r->ms = ms;
    r->taken = taken = CreateEventA(NULL, TRUE, FALSE, NULL);
    t = CreateThread(NULL, 0, sync_hold_thread, r, 0, NULL);
    WaitForSingleObject(taken, 5000);
    CloseHandle(taken);
    return t;
}

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], src[MAX_PATH * 2], storeA[MAX_PATH * 2], storeB[MAX_PATH * 2], storeC[MAX_PATH * 2], dir[MAX_PATH * 2];
    char url[64], keyA[SH_MS_KEY_MAX + 1], keyB[SH_MS_KEY_MAX + 1], keyG[SH_MS_KEY_MAX + 1], keyX[SH_MS_KEY_MAX + 1];
    char keyE[SH_MS_KEY_MAX + 1], keyG2[SH_MS_KEY_MAX + 1], keyR[SH_MS_KEY_MAX + 1], hG2[65], hR[65];
    char hA[65], hB[65], hG[65], hX[65], hE[65], claim1[33], secret1[65], claim_again[33], path[MAX_PATH * 2];
    modsync_stats st;
    int puts_before;
    g_echo = argc > 1 && !strcmp(argv[1], "-v");

    CHECK(stub_start() > 0);
    stub_url(url, sizeof url);
    CHECK(modnet_set_base_url(url));
    CHECK(mt_temp_dir("modsync", tmp, sizeof tmp));
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s\\src", tmp);
    _snprintf_s(storeA, sizeof storeA, _TRUNCATE, "%s\\storeA", tmp);
    _snprintf_s(storeB, sizeof storeB, _TRUNCATE, "%s\\storeB", tmp);
    _snprintf_s(storeC, sizeof storeC, _TRUNCATE, "%s\\storeC", tmp);
    CreateDirectoryA(src, NULL);

    /* ---- the author's store ---- */
    CHECK(sh_ms_set_root(storeA));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\alpha", src); make_mod(dir, "test.alpha", NULL, A_FILES, A_BYTES);
    CHECK(install(dir, keyA, hA));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\beta", src); make_mod(dir, "test.beta", NULL, B_FILES, B_BYTES);
    CHECK(install(dir, keyB, hB));
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gamma", src); make_mod(dir, "test.gamma", NULL, G_FILES, G_BYTES);
    CHECK(install(dir, keyG, hG));
    /* A descriptor key the store's manifest grammar does not know ("extra"). */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\extra", src);
    make_mod(dir, "test.extra", ",\"strings\":{\"english\":{\"#str_x\":\"X\"}}", X_FILES, X_BYTES);
    CHECK(install(dir, keyX, hX));
    {
        /* An author's editor overrides: one shadow of an editor-time decl,
         * nothing that plays. */
        sh_mb_resolver res;
        sh_ms_install_stats ist;
        sh_mb_bundle eb;
        char cause[SH_MB_CAUSE_MAX];
        memset(&res, 0, sizeof res);
        res.stock = stock_yes;
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\editor", src);
        make_mod(dir, "test.editor", ",\"shadows\":[\"generated/decls/snapeditorentitydef/func/rotate.decl\"]", E_FILES, E_BYTES);
        if (!sh_ms_install_from_folder(dir, &res, keyE, sizeof keyE, &ist, &eb, cause, sizeof cause)) {
            fprintf(stderr, "install %s: %s\n", dir, cause);
            CHECK(0);
        }
        strcpy_s(hE, sizeof hE, eb.hash);
        sh_mb_free(&eb);
    }

    /* ---- upload: negotiation, blobs, the canonical manifest, a new claim ---- */
    {
        const char *one[] = { hA };
        CHECK(modsync_upload_bundles(one, 1, "map-1", NULL, &st));
        CHECK(st.code == MODNET_OK && st.bundles == 1 && st.blobs_total == 2 && st.blobs_missing == 2 && st.blobs_transferred == 2);
        CHECK(st.bundles_created == 1 && !st.reclaimed && strlen(st.claim) == 32 && st.bytes_up > 0);
        CHECK(stub_has_bundle(hA) && g_stub.blob_created == 2);
        CHECK(modsync_claim_lookup("map-1", claim1, secret1) && strcmp(claim1, st.claim) == 0 && modnet_valid_hash(secret1));
        CHECK(stub_claim_bundles("map-1", claim_again) == 1 && strcmp(claim_again, claim1) == 0);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", storeA);
        CHECK(file_contains(path, "\"map-1\"") && file_contains(path, secret1) && file_contains(path, hA));
        /* The secret never reaches the log. */
        CHECK(strstr(g_log, secret1) == NULL);
    }
    {
        /* beta shares one file with alpha: only its own blob is sent. */
        const char *one[] = { hB };
        puts_before = g_stub.blob_puts;
        CHECK(modsync_upload_bundles(one, 1, "map-2", NULL, &st));
        CHECK(st.blobs_total == 2 && st.blobs_missing == 1 && st.blobs_transferred == 1);
        CHECK(g_stub.blob_puts == puts_before + 1 && g_stub.blob_created == 3);
    }
    {
        /* Publishing map-1 again: nothing to send, the stored secret re-claims. */
        const char *one[] = { hA };
        int with_secret = g_stub.claim_with_secret;
        puts_before = g_stub.blob_puts;
        CHECK(modsync_upload_bundles(one, 1, "map-1", NULL, &st));
        CHECK(st.blobs_missing == 0 && st.blobs_transferred == 0 && st.bundles_created == 0);
        CHECK(st.reclaimed && strcmp(st.claim, claim1) == 0 && g_stub.claim_with_secret == with_secret + 1);
        CHECK(g_stub.blob_puts == puts_before);
    }
    {
        /* The re-claim replaces the set the store keeps for the ref. */
        const char *two[] = { hA, hB };
        CHECK(modsync_upload_bundles(two, 2, "map-1", NULL, &st));
        CHECK(st.reclaimed && stub_claim_bundles("map-1", NULL) == 2);
    }
    {
        /* A blob the store loses between negotiation and the manifest (409). */
        const char *one[] = { hB };
        char blob_b[65];
        sh_mb_sha256_hex("beta only bytes", strlen("beta only bytes"), blob_b);
        strcpy_s(g_stub.drop_blob_before_bundle, sizeof g_stub.drop_blob_before_bundle, blob_b);
        CHECK(modsync_upload_bundles(one, 1, "map-2", NULL, &st));
        CHECK(st.blobs_transferred == 1 && stub_has_blob(blob_b));
        CHECK(strstr(g_log, "sending them again") != NULL);
    }

    /* ---- refusals and the retry queue ---- */
    {
        /* The store refuses the manifest at /mods/missing, before any blob
         * and without a bundle PUT, in the bundle PUT's wording. */
        const char *one[] = { hX };
        int blobs = g_stub.blob_puts, bundles = g_stub.bundle_puts, checked = g_stub.missing_manifests;
        CHECK(!modsync_upload_bundles(one, 1, "map-x", NULL, &st));
        CHECK(st.code == MODNET_E_REFUSED && strstr(st.cause, "unknown manifest key extra") != NULL && !st.queued);
        CHECK(strstr(st.cause, "bundle: HTTP 422") != NULL);
        CHECK(g_stub.blob_puts == blobs && g_stub.bundle_puts == bundles && g_stub.missing_manifests == checked + 1);
        CHECK(modsync_pending_count() == 0);
    }
    {
        /* A 422 at missing (the Worker's namespace rule, say) stops the
         * publish with its message: no blob PUT, nothing queued. */
        const char *one[] = { hG };
        int blobs = g_stub.blob_puts, bundles = g_stub.bundle_puts;
        strcpy_s(g_stub.refuse_manifest, sizeof g_stub.refuse_manifest,
                 "outside the mods/test.gamma/ namespace and not a declared shadow: cooked/x.bmodel");
        CHECK(!modsync_upload_bundles(one, 1, "map-s", NULL, &st));
        g_stub.refuse_manifest[0] = 0;
        CHECK(st.code == MODNET_E_REFUSED && !st.queued && st.blobs_transferred == 0 && st.manifests_checked == 0);
        CHECK(strstr(st.cause, "bundle: HTTP 422: outside the mods/test.gamma/ namespace and not a declared shadow") != NULL);
        CHECK(g_stub.blob_puts == blobs && g_stub.bundle_puts == bundles && !stub_has_bundle(hG));
        CHECK(modsync_pending_count() == 0 && !modsync_claim_lookup("map-s", NULL, NULL));
    }
    {
        /* A store that checked the manifest names it back; an older one
         * answers the blob list alone and the bundle PUT checks. */
        const char *one[] = { hA };
        CHECK(modsync_upload_bundles(one, 1, "map-1", NULL, &st) && st.manifests_checked == 1);
        g_stub.missing_ignores_manifest = 1;
        CHECK(modsync_upload_bundles(one, 1, "map-1", NULL, &st) && st.manifests_checked == 0);
        g_stub.missing_ignores_manifest = 0;
    }
    {
        const char *one[] = { hG };
        stub_fail_next("PUT /mods/bundle/", 503);
        CHECK(!modsync_upload_bundles(one, 1, "map-3", NULL, &st));
        CHECK(st.code == MODNET_E_UNAVAILABLE && st.queued && strstr(st.cause, "503") != NULL);
        CHECK(modsync_pending_count() == 1);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", storeA);
        CHECK(file_contains(path, "\"map-3\"") && file_contains(path, hG) && file_contains(path, "\"attempts\": 1"));
        /* A second failure counts the attempt. */
        stub_fail_next("POST /mods/missing", 503);
        CHECK(!modsync_upload_bundles(one, 1, "map-3", NULL, &st) && st.queued);
        CHECK(file_contains(path, "\"attempts\": 2") && modsync_pending_count() == 1);
    }
    {
        /* A cancelled push is queued too; it never reached the store. */
        const char *one[] = { hG };
        volatile long cancel = 1;
        modnet_io io = { &cancel, NULL, NULL };
        int before = g_stub.requests;
        CHECK(!modsync_upload_bundles(one, 1, "map-4", &io, &st));
        CHECK(st.code == MODNET_E_CANCELLED && st.queued && g_stub.requests == before);
        CHECK(modsync_pending_count() == 2);
    }
    {
        unsigned done = 0, dropped = 0;
        modsync_stats last;
        int left = modsync_retry_pending(NULL, &done, &dropped, &last);
        CHECK(left == 0 && done == 2 && dropped == 0);
        CHECK(stub_has_bundle(hG) && stub_claim_bundles("map-3", NULL) == 1 && stub_claim_bundles("map-4", NULL) == 1);
        CHECK(modsync_claim_lookup("map-3", NULL, NULL) && modsync_claim_lookup("map-4", NULL, NULL));
    }
    {
        /* A bundle that is not installed is refused without a request. */
        const char *one[] = { "0000000000000000000000000000000000000000000000000000000000000000" };
        int before = g_stub.requests;
        CHECK(!modsync_upload_bundles(one, 1, "map-5", NULL, &st) && st.code == MODNET_E_ARG && !st.queued);
        CHECK(g_stub.requests == before);
        CHECK(!modsync_upload_bundles(one, 1, "", NULL, &st) && st.code == MODNET_E_ARG);
    }

    {
        /* The record held by another program: the publish's claim is not saved,
         * and the cause the publish dialog shows names the file and what to do. */
        const char *one[] = { hA };
        char rec[MAX_PATH * 2];
        HANDLE t;
        _snprintf_s(rec, sizeof rec, _TRUNCATE, "%s\\snapmap-plus.json", storeA);
        t = sync_hold_record(rec, 6000);
        CHECK(!modsync_upload_bundles(one, 1, "map-held", NULL, &st));
        if (!strstr(st.cause, "close the program holding it")) fprintf(stderr, "held-record cause: %s\n", st.cause);
        CHECK(strstr(st.cause, "snapmap-plus.json could not be used (another program has it open); close the program "
                               "holding it (a backup, sync or antivirus tool, or an editor) and try again") != NULL);
        WaitForSingleObject(t, 10000);
        CloseHandle(t);
        CHECK(!modsync_claim_lookup("map-held", NULL, NULL));
    }

    /* ---- a map pinning an earlier build: the current build is uploaded ---- */
    {
        char keyD[SH_MS_KEY_MAX + 1], hD1[65], hD2[65], folder[MAX_PATH * 4];
        const char *old[] = { hD1 }, *both[] = { hD1, hD2 };
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\delta", src);
        make_mod(dir, "test.delta", NULL, D_FILES, D_BYTES);
        CHECK(install(dir, keyD, hD1));
        /* The author edits the installed folder in place: build 2 in the same
         * folder, the earlier hash an alias of it. (An install from elsewhere
         * would land beside it by hash instead.) */
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\test.delta", storeA);
        mt_write(dir, "assets\\generated\\image\\mods\\test.delta\\d.bimage", D2_BYTES[0]);
        CHECK(install(dir, keyD, hD2) && strcmp(hD1, hD2));   /* the same folder, build 2 */
        CHECK(modsync_find_installed(hD1, folder, sizeof folder) && strstr(folder, "test.delta"));
        CHECK(modsync_upload_bundles(old, 1, "map-6", NULL, &st));
        CHECK(st.bundles == 1 && stub_has_bundle(hD2) && !stub_has_bundle(hD1));
        CHECK(strstr(g_log, "was built again; uploading its current build test.delta@1.0.0+2") != NULL);
        /* Both hashes name one bundle: it is sent and claimed once. */
        CHECK(modsync_upload_bundles(both, 2, "map-6", NULL, &st) && st.bundles == 1 && stub_claim_bundles("map-6", NULL) == 1);
    }

    /* ---- release ---- */
    CHECK(modsync_release("map-1", NULL, &st));
    CHECK(strcmp(st.claim, claim1) == 0 && stub_claim_bundles("map-1", NULL) == -1);
    CHECK(!modsync_claim_lookup("map-1", NULL, NULL));
    CHECK(!modsync_release("map-1", NULL, &st) && st.code == MODNET_E_NOT_FOUND);
    {
        /* A claim the store no longer knows is forgotten locally. */
        char c4[33], s4[65];
        CHECK(modsync_claim_lookup("map-4", c4, s4));
        CHECK(modnet_release(c4, s4, NULL, NULL) == MODNET_OK);
        CHECK(modsync_release("map-4", NULL, &st) && strstr(st.cause, "no longer knew") != NULL);
        CHECK(!modsync_claim_lookup("map-4", NULL, NULL));
    }

    {
        /* A release the store does not take is queued, not forgotten, and
         * sent after the next store call that succeeds. */
        const char *one[] = { hG };
        int releases = g_stub.release_calls;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", storeA);
        CHECK(modsync_pending_count() == 0);
        stub_fail_next("POST /mods/release", 503);
        CHECK(!modsync_release("map-6", NULL, &st) && st.code == MODNET_E_UNAVAILABLE && st.queued);
        CHECK(modsync_claim_lookup("map-6", NULL, NULL) && stub_claim_bundles("map-6", NULL) == 1);
        CHECK(modsync_pending_count() == 1 && file_contains(path, "\"kind\": \"release\""));
        CHECK(modsync_upload_bundles(one, 1, "map-3", NULL, &st));
        CHECK(g_stub.release_calls == releases + 1 && stub_claim_bundles("map-6", NULL) == -1);
        CHECK(!modsync_claim_lookup("map-6", NULL, NULL) && modsync_pending_count() == 0);
        CHECK(strstr(g_log, "MODS: retry map-6: released the queued claim") != NULL);
        /* The launch retry sends a queued release too, and a release replaces
         * a queued upload of the same ref. */
        stub_fail_next("POST /mods/missing", 503);
        CHECK(!modsync_upload_bundles(one, 1, "map-3", NULL, &st) && st.queued && modsync_pending_count() == 1);
        stub_fail_next("POST /mods/release", 503);
        CHECK(!modsync_release("map-3", NULL, &st) && st.queued && modsync_pending_count() == 1);
        CHECK(file_contains(path, "\"kind\": \"release\""));
        {
            unsigned done = 0, dropped = 0;
            CHECK(modsync_retry_pending(NULL, &done, &dropped, NULL) == 0 && done == 1 && dropped == 0);
        }
        CHECK(stub_claim_bundles("map-3", NULL) == -1 && !modsync_claim_lookup("map-3", NULL, NULL));
    }
    {
        /* The editor overrides go up like any bundle an author's map names. */
        const char *one[] = { hE };
        CHECK(modsync_upload_bundles(one, 1, "map-e", NULL, &st) && stub_has_bundle(hE));
    }
    /* The denial gate asks whether this machine published a bundle: a claim
     * it holds names it (map-2 claims beta); a released claim does not. */
    CHECK(modsync_claim_names(hB) == 1 && modsync_claim_names(hE) == 1);
    CHECK(modsync_claim_names("1111111111111111111111111111111111111111111111111111111111111111") == 0);

    /* ---- a second build of gamma and a mod that borrows a game file ---- */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\gamma2", src); make_mod(dir, "test.gamma", NULL, G_FILES, G2_BYTES);
    CHECK(install(dir, keyG2, hG2) && strcmp(hG2, hG) && strncmp(keyG2, "test.gamma@", 11) == 0);
    {
        const char *one[] = { hG2 };
        CHECK(modsync_upload_bundles(one, 1, "map-g2", NULL, &st) && stub_has_bundle(hG2));
    }
    sh_mb_set_stock_ops(&GAME_HAS_ROCK);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\borrow", src); make_mod(dir, "test.borrow", NULL, R_FILES, R_BYTES);
    CHECK(install(dir, keyR, hR));
    {
        const char *one[] = { hR };
        int blobs = g_stub.blob_puts;
        CHECK(modsync_upload_bundles(one, 1, "map-r", NULL, &st) && stub_has_bundle(hR));
        CHECK(g_stub.blob_puts == blobs + 1);   /* the borrow's bytes never leave the machine */
    }
    sh_mb_set_stock_ops(NULL);

    /* ---- download into an empty store ---- */
    CHECK(sh_ms_set_root(storeB));
    CHECK(sh_modserve_index_build(storeB) == 0);
    CHECK(sh_modserve_bundle_state(hB, NULL, 0) == 0);
    /* A download for a map lands under mods\\downloads\\<map title>\\<key>
     * (owner 2026-11-10): never as <key>@<hash16> beside the player's own. */
    CHECK(modsync_fetch_bundle_for(hB, "Hell: Gate?", "item-1", NULL, &st));
    CHECK(st.code == MODNET_OK && !st.already_installed && st.blobs_transferred == 2 && st.bytes_down > 0);
    CHECK(strcmp(st.key, "downloads\\Hell Gate\\test.beta") == 0);
    {
        sh_ms_verify_stats vs;
        char cause[SH_MB_CAUSE_MAX], title[128];
        CHECK(sh_ms_verify(st.key, &vs, cause, sizeof cause) && vs.files == 2);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\bundle.canonical.json", storeB, st.key);
        CHECK(!mt_exists(path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\mod.json", storeB, st.key);
        CHECK(mt_exists(path) && file_contains(path, "\"origin\": \"download\""));
        CHECK(sh_ms_downloaded_for(st.key, title, sizeof title) && !strcmp(title, "Hell: Gate?"));
        CHECK(sh_ms_downloaded_for("downloads/Hell Gate/test.beta", title, sizeof title));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\assets", storeB, st.key);
        CHECK(count_files(path) == 2);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\blobs", storeB);
        CHECK(!mt_exists(path));
    }
    CHECK(sh_modserve_bundle_state(hB, NULL, 0) == 1);   /* the serving index was rebuilt */
    {
        int gets = g_stub.blob_gets + g_stub.bundle_gets;
        CHECK(modsync_fetch_bundle(hB, NULL, &st) && st.already_installed);
        CHECK(g_stub.blob_gets + g_stub.bundle_gets == gets);
    }
    {
        /* alpha shares a file with the installed beta: that one is copied from
         * beta's folder, one blob fetched. The stored body carries "bundle", as
         * the Worker may keep it. */
        g_stub.serve_bundle_member = 1;
        CHECK(modsync_fetch_bundle(hA, NULL, &st));
        g_stub.serve_bundle_member = 0;
        CHECK(st.blobs_transferred == 1 && sh_modserve_bundle_state(hA, NULL, 0) == 1);
    }
    {
        /* A bundle of editor-time content only: its manifest is read, no blob
         * is fetched, nothing is installed, and the record's editorOnly
         * section serves its row from now on. */
        int blob_gets = g_stub.blob_gets;
        sh_modmanifest_row row;
        char cause[400];
        unsigned files_before;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s", storeB);
        files_before = count_files(path);
        CHECK(modsync_fetch_bundle(hE, NULL, &st));
        CHECK(st.editor_only && st.code == MODNET_OK && !st.bundles_created && !st.already_installed);
        CHECK(g_stub.blob_gets == blob_gets);
        CHECK(sh_modserve_bundle_state(hE, NULL, 0) == 0);
        CHECK(count_files(path) == files_before);   /* only snapmap-plus.json changed */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", storeB);
        CHECK(file_contains(path, "\"editorOnly\"") && file_contains(path, hE) && file_contains(path, "\"test.editor\""));
        CHECK(sh_modserve_editor_only_recorded(hE));
        memset(&row, 0, sizeof row);
        strcpy_s(row.id, sizeof row.id, "test.editor");
        strcpy_s(row.bundle, sizeof row.bundle, hE);
        CHECK(sh_modserve_row_served(&row, cause, sizeof cause) == SH_MODSERVE_ROW_SERVED);
        CHECK(strstr(g_log, "changes only the editor (1 file(s), no requirements); not installed") != NULL);
        /* Asked again: the same answer, still nothing fetched. */
        CHECK(modsync_fetch_bundle(hE, NULL, &st) && st.editor_only && g_stub.blob_gets == blob_gets);
    }

    /* ---- refusals on download ---- */
    CHECK(sh_ms_set_root(storeC));
    CHECK(sh_modserve_index_build(storeC) == 0);
    g_stub.corrupt_blob_gets = 1;
    CHECK(!modsync_fetch_bundle(hG, NULL, &st));
    g_stub.corrupt_blob_gets = 0;
    CHECK(st.code == MODNET_E_HASH && strstr(st.cause, "hash") != NULL);
    {
        sh_ms_list l;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\test.gamma", storeC);
        CHECK(!mt_exists(path));
        CHECK(sh_ms_enumerate(&l) && l.count == 0);
        sh_ms_list_free(&l);
    }
    strcpy_s(g_stub.corrupt_bundle, sizeof g_stub.corrupt_bundle, hG);
    CHECK(!modsync_fetch_bundle(hG, NULL, &st) && st.code == MODNET_E_HASH);
    g_stub.corrupt_bundle[0] = 0;
    CHECK(!modsync_fetch_bundle("1111111111111111111111111111111111111111111111111111111111111111", NULL, &st) &&
          st.code == MODNET_E_NOT_FOUND);
    CHECK(modsync_fetch_bundle(hG, NULL, &st) && sh_modserve_bundle_state(hG, NULL, 0) == 1);

    /* ---- install by hash: another build of an installed key lands beside it ---- */
    {
        char want[SH_MS_KEY_MAX + 1];
        sh_ms_list l;
        /* A second build of one key fetched with no map: mods\\downloads\\<key>-2,
         * no hash in any visible name. */
        CHECK(modsync_fetch_bundle(hG2, NULL, &st) && !st.already_installed && st.bundles_created == 1);
        strcpy_s(want, sizeof want, "downloads\\test.gamma-2");
        CHECK(!strcmp(st.key, want));
        CHECK(sh_modserve_bundle_state(hG, NULL, 0) == 1 && sh_modserve_bundle_state(hG2, NULL, 0) == 1);
        CHECK(sh_ms_enumerate(&l) && l.count == 2);
        sh_ms_list_free(&l);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\mod.json", storeC, want);
        CHECK(file_contains(path, "\"origin\": \"download\""));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\downloads\\test.gamma\\mod.json", storeC);
        CHECK(mt_exists(path));   /* the first build keeps its folder and name */
        /* Fetched again: present, nothing replaced. */
        CHECK(modsync_fetch_bundle(hG2, NULL, &st) && st.already_installed);
    }
    {
        /* Two maps of one title: the second map's folder carries its id.
         * A build already present anywhere is not stored again: the record
         * notes the map's link to that folder. Uninstalling a downloaded
         * build forgets it and removes the emptied organizing folders. */
        char title[128], cause[SH_MB_CAUSE_MAX], full[MAX_PATH * 2];
        sh_ms_list l;
        CHECK(sh_ms_set_root(storeC));
        CHECK(modsync_fetch_bundle_for(hB, "Arena", "item-a", NULL, &st) && !st.already_installed);
        CHECK(!strcmp(st.key, "downloads\\Arena\\test.beta"));
        CHECK(modsync_fetch_bundle_for(hA, "Arena", "item-b", NULL, &st) && !st.already_installed);
        CHECK(!strcmp(st.key, "downloads\\Arena (item-b)\\test.alpha"));
        CHECK(modsync_fetch_bundle_for(hB, "Third Map", "item-c", NULL, &st) && st.already_installed);
        CHECK(!strcmp(st.key, "downloads\\Arena\\test.beta"));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", storeC);
        CHECK(file_contains(path, "\"item-c\"") && file_contains(path, "\"Third Map\""));
        CHECK(sh_ms_downloaded_for("downloads\\Arena\\test.beta", title, sizeof title) && !strcmp(title, "Arena"));
        /* The same map again: its own title folder, never "(id)". */
        CHECK(sh_ms_placed_folder_name(storeC, &(sh_ms_place){ SH_MS_GROUP_DOWNLOADS, "Arena", "item-a" }, "test.new", hA,
                                       title, sizeof title) == 1 && !strcmp(title, "downloads\\Arena\\test.new"));
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\downloads\\Arena (item-b)\\test.alpha", storeC);
        CHECK(sh_ms_delete_mod_folder(full, cause, sizeof cause) != 0);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\downloads\\Arena (item-b)", storeC);
        CHECK(!mt_exists(path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus.json", storeC);
        CHECK(!file_contains(path, "item-b"));
        CHECK(sh_ms_enumerate(&l));
        sh_ms_list_free(&l);
    }

    /* ---- the gate's probe against the loopback store ---- */
    {
        modsync_probe pr;
        int found = modsync_probe_bundle(hG2, NULL, &pr);
        CHECK(found == 1 && pr.found == 1 && !pr.game_lacks && !strcmp(pr.id, "test.gamma") && !strcmp(pr.name, "test.gamma"));
        CHECK(pr.size == strlen(G2_BYTES[0]));
        /* Never on the store (404): absent. */
        CHECK(modsync_probe_bundle("2222222222222222222222222222222222222222222222222222222222222222", NULL, &pr) == 0 &&
              pr.code == MODNET_E_NOT_FOUND);
        /* On the takedown list (451): as absent as 404 for the player. */
        stub_fail_next("GET /mods/bundle/", 451);
        CHECK(modsync_probe_bundle(hG2, NULL, &pr) == 0 && pr.code == MODNET_E_BLOCKED);
        /* A 503: the store did not answer. */
        stub_fail_next("GET /mods/bundle/", 503);
        CHECK(modsync_probe_bundle(hG2, NULL, &pr) == -1 && pr.code == MODNET_E_UNAVAILABLE);
        /* Nothing listening: unreachable. */
        CHECK(modnet_set_base_url("http://127.0.0.1:1"));
        CHECK(modsync_probe_bundle(hG2, NULL, &pr) == -1 && pr.found == -1 && modnet_unreachable(pr.code));
        CHECK(modnet_set_base_url(url));
        /* Held, but its borrow names a game file this game lacks (another
         * revision): the probe names the source, and the download refuses it
         * before any blob is fetched. */
        sh_mb_set_stock_ops(&GAME_LACKS_ROCK);
        CHECK(modsync_probe_bundle(hR, NULL, &pr) == 1 && pr.game_lacks && !strcmp(pr.source, ROCK_SOURCE));
        {
            int blobs = g_stub.blob_gets;
            CHECK(!modsync_fetch_bundle(hR, NULL, &st) && st.code == MODNET_E_GAME && st.game_lacks &&
                  !strcmp(st.game_source, ROCK_SOURCE) && g_stub.blob_gets == blobs);
            CHECK(sh_modserve_bundle_state(hR, NULL, 0) == 0);
        }
        /* The same bundle where the game holds the file: installs. */
        sh_mb_set_stock_ops(&GAME_HAS_ROCK);
        CHECK(modsync_probe_bundle(hR, NULL, &pr) == 1 && !pr.game_lacks);
        CHECK(modsync_fetch_bundle(hR, NULL, &st) && sh_modserve_bundle_state(hR, NULL, 0) == 1);
        sh_mb_set_stock_ops(NULL);
    }

    modnet_shutdown();
    stub_stop();
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    printf("modsync_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
