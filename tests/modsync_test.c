/* modsync_test -- upload, download, claims and the retry queue between a
 * temporary local store (modstore.c) and a loopback stub of the mod store
 * (mod_store_stub.h):
 *   missing negotiation (a blob two bundles share is sent once), blob
 *   upload idempotence, the exact canonical manifest as the bundle body, a
 *   409 at the manifest renegotiated, claim and re-claim with the secret
 *   kept in claims.json, release, a 503 queued in pending-uploads.json and
 *   retried, a 422 not queued, a cancelled push queued, and downloads into
 *   empty stores: installed and verified and in the serving index, the same
 *   bundle again fetched from nowhere, and a blob or manifest whose bytes do
 *   not match its hash refused with nothing installed.
 */
#include "mod_store_stub.h"
#include "../src/backend/modnet.h"
#include "../src/backend/modsync.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modserve.h"
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
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++; } while (FindNextFileA(h, &fd));
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
static const char *const X_FILES[] = { "assets/generated/image/mods/test.extra/x.bimage", NULL };
static const char *const X_BYTES[] = { "extra bytes", NULL };

int main(int argc, char **argv)
{
    char tmp[MAX_PATH], src[MAX_PATH * 2], storeA[MAX_PATH * 2], storeB[MAX_PATH * 2], storeC[MAX_PATH * 2], dir[MAX_PATH * 2];
    char url[64], keyA[SH_MS_KEY_MAX + 1], keyB[SH_MS_KEY_MAX + 1], keyG[SH_MS_KEY_MAX + 1], keyX[SH_MS_KEY_MAX + 1];
    char hA[65], hB[65], hG[65], hX[65], claim1[33], secret1[65], claim_again[33], path[MAX_PATH * 2];
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

    /* ---- upload: negotiation, blobs, the canonical manifest, a new claim ---- */
    {
        const char *one[] = { hA };
        CHECK(modsync_upload_bundles(one, 1, "map-1", NULL, &st));
        CHECK(st.code == MODNET_OK && st.bundles == 1 && st.blobs_total == 2 && st.blobs_missing == 2 && st.blobs_transferred == 2);
        CHECK(st.bundles_created == 1 && !st.reclaimed && strlen(st.claim) == 32 && st.bytes_up > 0);
        CHECK(stub_has_bundle(hA) && g_stub.blob_created == 2);
        CHECK(modsync_claim_lookup("map-1", claim1, secret1) && strcmp(claim1, st.claim) == 0 && modnet_valid_hash(secret1));
        CHECK(stub_claim_bundles("map-1", claim_again) == 1 && strcmp(claim_again, claim1) == 0);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", storeA, MODSYNC_CLAIMS_FILE);
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
        const char *one[] = { hX };
        CHECK(!modsync_upload_bundles(one, 1, "map-x", NULL, &st));
        CHECK(st.code == MODNET_E_REFUSED && strstr(st.cause, "unknown manifest key extra") != NULL && !st.queued);
        CHECK(modsync_pending_count() == 0);
    }
    {
        const char *one[] = { hG };
        stub_fail_next("PUT /mods/bundle/", 503);
        CHECK(!modsync_upload_bundles(one, 1, "map-3", NULL, &st));
        CHECK(st.code == MODNET_E_UNAVAILABLE && st.queued && strstr(st.cause, "503") != NULL);
        CHECK(modsync_pending_count() == 1);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", storeA, MODSYNC_PENDING_FILE);
        CHECK(file_contains(path, "\"map-3\"") && file_contains(path, hG) && file_contains(path, "\"attempts\":1"));
        /* A second failure counts the attempt. */
        stub_fail_next("POST /mods/missing", 503);
        CHECK(!modsync_upload_bundles(one, 1, "map-3", NULL, &st) && st.queued);
        CHECK(file_contains(path, "\"attempts\":2") && modsync_pending_count() == 1);
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

    /* ---- download into an empty store ---- */
    CHECK(sh_ms_set_root(storeB));
    CHECK(sh_modserve_index_build(storeB) == 0);
    CHECK(sh_modserve_bundle_state(hB, NULL, 0) == 0);
    CHECK(modsync_fetch_bundle(hB, NULL, &st));
    CHECK(st.code == MODNET_OK && !st.already_installed && st.blobs_transferred == 2 && st.bytes_down > 0);
    CHECK(strcmp(st.key, keyB) == 0);
    {
        sh_ms_verify_stats vs;
        char cause[SH_MB_CAUSE_MAX];
        CHECK(sh_ms_verify(keyB, &vs, cause, sizeof cause) && vs.files == 2);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\bundle.canonical.json", storeB, keyB);
        CHECK(mt_exists(path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\blobs", storeB);
        CHECK(count_files(path) == 2);
    }
    CHECK(sh_modserve_bundle_state(hB, NULL, 0) == 1);   /* the serving index was rebuilt */
    {
        int gets = g_stub.blob_gets + g_stub.bundle_gets;
        CHECK(modsync_fetch_bundle(hB, NULL, &st) && st.already_installed);
        CHECK(g_stub.blob_gets + g_stub.bundle_gets == gets);
    }
    {
        /* alpha shares a blob with the installed beta: one blob fetched. The
         * stored body carries "bundle", as the Worker may keep it. */
        g_stub.serve_bundle_member = 1;
        CHECK(modsync_fetch_bundle(hA, NULL, &st));
        g_stub.serve_bundle_member = 0;
        CHECK(st.blobs_transferred == 1 && sh_modserve_bundle_state(hA, NULL, 0) == 1);
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
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\blobs", storeC);
        CHECK(count_files(path) == 0);
        CHECK(sh_ms_enumerate(&l) && l.count == 0);
        sh_ms_list_free(&l);
    }
    strcpy_s(g_stub.corrupt_bundle, sizeof g_stub.corrupt_bundle, hG);
    CHECK(!modsync_fetch_bundle(hG, NULL, &st) && st.code == MODNET_E_HASH);
    g_stub.corrupt_bundle[0] = 0;
    CHECK(!modsync_fetch_bundle("1111111111111111111111111111111111111111111111111111111111111111", NULL, &st) &&
          st.code == MODNET_E_NOT_FOUND);
    CHECK(modsync_fetch_bundle(hG, NULL, &st) && sh_modserve_bundle_state(hG, NULL, 0) == 1);

    modnet_shutdown();
    stub_stop();
    sh_ms_set_root(NULL);
    mt_remove_tree(tmp);
    printf("modsync_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
