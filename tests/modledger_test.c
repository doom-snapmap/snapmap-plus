/* modledger_test.c -- the served-bytes ledger (modledger.c) and the refresh
 * decision it feeds: identities, records and updates, binding, the bound,
 * concurrent serves, the KEEP / REFRESH / GAME rule, and, against a fixture
 * mods tree served by modserve.c, the anim refresh kind: one key at two
 * hashes marks the anim whose bytes differ and not the one both builds carry,
 * a rescan counts a changed anim as reloadable and a changed skeleton as a
 * restart, and the ledger's record of a serve matches what the index serves
 * until another build is declared. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modledger.h"
#include "../src/backend/modserve.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *line) { (void)line; }
int modserve_activate_bundles(const char *const *bundle_hashes, int n) { (void)bundle_hashes; (void)n; return 0; }

#define SHA_A "aaaaaaaaaaaaaaaa11111111111111111111111111111111111111111111111111"
#define SHA_B "bbbbbbbbbbbbbbbb22222222222222222222222222222222222222222222222222"
#define BUNDLE_1 "1111111111111111111111111111111111111111111111111111111111111111"
#define BUNDLE_2 "2222222222222222222222222222222222222222222222222222222222222222"

/* ------------------------------------------------------------------ identities */

static void test_identity(void)
{
    sh_modledger_ident a, b, c;
    printf("-- ledger: identities\n");
    CHECK(sh_modledger_ident_of(BUNDLE_1, SHA_A, NULL, 100, &a) && a.kind == SH_MODLEDGER_ID_SHA);
    CHECK(a.file == 0xaaaaaaaaaaaaaaaaull && a.bundle == 0x1111111111111111ull && a.length == 100);
    /* The same file from another build is the same bytes; a stock pointer's size may read 0. */
    CHECK(sh_modledger_ident_of(BUNDLE_2, SHA_A, NULL, 0, &b) && sh_modledger_same(&a, &b));
    CHECK(sh_modledger_ident_of(BUNDLE_1, SHA_B, NULL, 100, &c) && !sh_modledger_same(&a, &c));
    /* No recorded hash: the bytes. */
    CHECK(sh_modledger_ident_of(BUNDLE_1, NULL, (const unsigned char *)"abc", 3, &b) && b.kind == SH_MODLEDGER_ID_FNV);
    CHECK(sh_modledger_ident_of(BUNDLE_2, NULL, (const unsigned char *)"abc", 3, &c) && sh_modledger_same(&b, &c));
    CHECK(sh_modledger_ident_of(BUNDLE_1, NULL, (const unsigned char *)"abd", 3, &c) && !sh_modledger_same(&b, &c));
    CHECK(sh_modledger_ident_of(BUNDLE_1, NULL, (const unsigned char *)"abcd", 3, &c) && sh_modledger_same(&b, &c));
    CHECK(!sh_modledger_same(&a, &b));                /* a hash and bytes never compare */
    /* A malformed hash falls back to the bytes; neither: no identity. */
    CHECK(sh_modledger_ident_of(BUNDLE_1, "zz", (const unsigned char *)"abc", 3, &c) && c.kind == SH_MODLEDGER_ID_FNV);
    CHECK(!sh_modledger_ident_of(BUNDLE_1, NULL, NULL, 10, &c) && c.kind == SH_MODLEDGER_ID_NONE);
    CHECK(!sh_modledger_same(&c, &c));
}

/* ------------------------------------------------------------------ records */

static void test_records(void)
{
    sh_modledger_ident a, b;
    sh_modledger_rec r;
    sh_modledger_stats st;
    int obj = 0;
    printf("-- ledger: records, updates and binding\n");
    sh_modledger_reset();
    CHECK(!sh_modledger_find("cooked/anim/md6/mods/k/x.bmd6anim", &r));
    (void)sh_modledger_ident_of(BUNDLE_1, SHA_A, NULL, 100, &a);
    (void)sh_modledger_ident_of(BUNDLE_2, SHA_B, NULL, 120, &b);
    CHECK(sh_modledger_record("cooked/anim/md6/mods/k/x.bmd6anim", &a) == 1);
    CHECK(sh_modledger_find("COOKED\\anim\\md6\\mods\\K\\x.bmd6anim", &r) && sh_modledger_same(&r.id, &a) && r.serves == 1);
    CHECK(r.obj == NULL);
    CHECK(sh_modledger_record("cooked/anim/md6/mods/k/x.bmd6anim", &b) == 1);
    CHECK(sh_modledger_find("cooked/anim/md6/mods/k/x.bmd6anim", &r) && sh_modledger_same(&r.id, &b) && r.serves == 2);
    CHECK(r.id.bundle == 0x2222222222222222ull && r.seq == 2);
    CHECK(sh_modledger_bind("cooked/anim/md6/mods/k/x.bmd6anim", &obj) == 1);
    CHECK(sh_modledger_find("cooked/anim/md6/mods/k/x.bmd6anim", &r) && r.obj == &obj && r.serves == 2);
    CHECK(!sh_modledger_bind("cooked/anim/md6/mods/k/y.bmd6anim", &obj));
    CHECK(sh_modledger_record("generated/decls/entitydef/mods/k/y.decl", &a) == 1);
    CHECK(sh_modledger_record(NULL, &a) == 0 && sh_modledger_record("", &a) == 0);
    sh_modledger_get_stats(&st);
    CHECK(st.records == 2 && st.serves == 3 && st.overflow == 0 && st.complete && st.cap == SH_MODLEDGER_CAP);
    sh_modledger_set_complete(0);
    CHECK(!sh_modledger_complete());
    sh_modledger_set_complete(1);
    CHECK(sh_modledger_complete());
}

static int g_each;
static void count_each(void *context, const sh_modledger_rec *rec) { (void)context; if (rec->request) g_each++; }

/* At most SH_MODLEDGER_CAP paths; past that a serve is counted and the
 * ledger stops calling itself complete. */
static void test_bound(void)
{
    static char names[SH_MODLEDGER_CAP + 1][48];
    sh_modledger_ident a;
    sh_modledger_stats st;
    unsigned i, ok = 0;
    printf("-- ledger: bounded\n");
    sh_modledger_reset();
    (void)sh_modledger_ident_of(BUNDLE_1, SHA_A, NULL, 1, &a);
    for (i = 0; i <= SH_MODLEDGER_CAP; ++i) {
        _snprintf_s(names[i], sizeof names[i], _TRUNCATE, "generated/image/mods/k/%05u.bimage", i);
        ok += (unsigned)sh_modledger_record(names[i], &a);
    }
    sh_modledger_get_stats(&st);
    CHECK(ok == SH_MODLEDGER_CAP && st.records == SH_MODLEDGER_CAP && st.overflow == 1 && !st.complete);
    CHECK(sh_modledger_record(names[5], &a) == 1);   /* a recorded path still updates */
    g_each = 0;
    CHECK(sh_modledger_each(count_each, NULL) == (int)SH_MODLEDGER_CAP && g_each == (int)SH_MODLEDGER_CAP);
    sh_modledger_reset();
}

/* Serves arrive on the engine's loader threads: many threads, shared paths. */
#define TH_N 8
#define TH_PATHS 1500
static char g_paths[TH_PATHS][48];
static DWORD WINAPI serve_thread(LPVOID p)
{
    int t = (int)(intptr_t)p, i, k;
    sh_modledger_ident a;
    sh_modledger_rec r;
    (void)sh_modledger_ident_of(BUNDLE_1, SHA_A, NULL, (size_t)t + 1, &a);
    for (k = 0; k < 4; ++k)
        for (i = 0; i < TH_PATHS; ++i) {
            (void)sh_modledger_record(g_paths[(i * 7 + t * 13) % TH_PATHS], &a);
            (void)sh_modledger_find(g_paths[i], &r);
        }
    return 0;
}

static void test_threads(void)
{
    HANDLE h[TH_N];
    sh_modledger_stats st;
    sh_modledger_rec r;
    int i, all = 1;
    long serves = 0;
    printf("-- ledger: concurrent serves\n");
    sh_modledger_reset();
    for (i = 0; i < TH_PATHS; ++i) _snprintf_s(g_paths[i], sizeof g_paths[i], _TRUNCATE, "cooked/anim/mods/k/%04d.bmd6anim", i);
    for (i = 0; i < TH_N; ++i) h[i] = CreateThread(NULL, 0, serve_thread, (LPVOID)(intptr_t)i, 0, NULL);
    WaitForMultipleObjects(TH_N, h, TRUE, INFINITE);
    for (i = 0; i < TH_N; ++i) CloseHandle(h[i]);
    sh_modledger_get_stats(&st);
    CHECK(st.records == TH_PATHS && st.serves == (unsigned long)TH_N * 4 * TH_PATHS && st.overflow == 0);
    for (i = 0; i < TH_PATHS; ++i) {
        if (!sh_modledger_find(g_paths[i], &r) || r.id.kind != SH_MODLEDGER_ID_SHA || r.id.length < 1 || r.id.length > TH_N)
            all = 0;
        else serves += r.serves;
    }
    CHECK(all && serves == (long)TH_N * 4 * TH_PATHS);
    sh_modledger_reset();
}

/* ------------------------------------------------------------------ decision */

static void test_decide(void)
{
    sh_modledger_ident a, b;
    sh_modledger_rec r;
    printf("-- ledger: the refresh decision\n");
    (void)sh_modledger_ident_of(BUNDLE_1, SHA_A, NULL, 100, &a);
    (void)sh_modledger_ident_of(BUNDLE_2, SHA_B, NULL, 100, &b);
    memset(&r, 0, sizeof r);
    r.request = "cooked/anim/md6/mods/k/x.bmd6anim";
    r.id = a;
    CHECK(sh_modledger_decide(&r, &a, 0, 1) == SH_MODLEDGER_KEEP);      /* held with the bytes served now */
    CHECK(sh_modledger_decide(&r, &b, 0, 1) == SH_MODLEDGER_REFRESH);   /* another build's bytes */
    CHECK(sh_modledger_decide(&r, NULL, 0, 1) == SH_MODLEDGER_REFRESH); /* the identity served now is unknown */
    CHECK(sh_modledger_decide(NULL, &a, 0, 1) == SH_MODLEDGER_GAME);    /* never given a mod's bytes */
    CHECK(sh_modledger_decide(NULL, &a, 0, 0) == SH_MODLEDGER_REFRESH); /* an incomplete ledger is not trusted */
    CHECK(sh_modledger_decide(&r, &a, 1, 1) == SH_MODLEDGER_REFRESH);   /* a stock path: the marks decide */
    CHECK(sh_modledger_decide(NULL, NULL, 1, 1) == SH_MODLEDGER_REFRESH);
    CHECK(!strcmp(sh_modledger_decision_name(SH_MODLEDGER_GAME), "game"));
}

/* ------------------------------------------------------------------ fixture */

static void mkdirs(const char *path)
{
    char tmp[MAX_PATH];
    char *p;
    strcpy_s(tmp, sizeof tmp, path);
    for (p = tmp + 3; *p; ++p)
        if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    CreateDirectoryA(tmp, NULL);
}

static void write_file(const char *path, const char *body, size_t len)
{
    char dir[MAX_PATH], *slash;
    FILE *f;
    strcpy_s(dir, sizeof dir, path);
    slash = strrchr(dir, '\\');
    if (slash) { *slash = 0; mkdirs(dir); }
    if (fopen_s(&f, path, "wb") || !f) { fprintf(stderr, "cannot write %s\n", path); g_failed++; return; }
    fwrite(body, 1, len, f);
    fclose(f);
}

typedef struct fx { const char *path, *body, *ptype, *pname; char blob; } fx;

static const char *hash64(char c, char *out)
{
    memset(out, c, 64);
    out[64] = 0;
    return out;
}

static void make_bundle(const char *root, const char *folder, const char *id, char hash_digit, int built,
                        const fx *files, int n)
{
    char dir[MAX_PATH], path[MAX_PATH], h[65], b[65];
    static char json[16384];
    size_t at = 0;
    int i;
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, folder);
    mkdirs(dir);
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                      "{\n  \"version\": \"1.0.0\",\n  \"name\": \"%s\",\n"
                      "  \"snapmap-plus\": {\"format\": 2, \"salt\": \"%s\", \"rules\": 8, \"build\": \"%s\", \"built\": %d,\n"
                      "    \"shadows\": [],\n    \"files\": [\n", id, id, hash64(hash_digit, h), built);
    for (i = 0; i < n; ++i) {
        char *p;
        at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE,
                          "      {\"path\": \"assets/%s\", \"sha256\": \"%s\", \"size\": %zu, "
                          "\"provides\": {\"type\": \"%s\", \"name\": \"%s\"}, \"needs\": []}%s\n",
                          files[i].path, hash64(files[i].blob, b), strlen(files[i].body), files[i].ptype,
                          files[i].pname, i + 1 < n ? "," : "");
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\%s", dir, files[i].path);
        for (p = path; *p; ++p) if (*p == '/') *p = '\\';
        write_file(path, files[i].body, strlen(files[i].body));
    }
    at += _snprintf_s(json + at, sizeof json - at, _TRUNCATE, "    ]\n  }\n}\n");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", dir);
    write_file(path, json, at);
}

#define AN_WALK "cooked/anim/md6/mods/kanim000/walk.bmd6anim"
#define AN_IDLE "cooked/anim/md6/mods/kanim000/idle.bmd6anim"
#define AN_SKEL "generated/skeleton/md6/mods/kanim000/body.bmd6skl"
#define AN_DECL "generated/decls/md6def/mods/kanim000/body.md6.decl"

typedef struct seen { int walk, idle, skel, decl, other, kinds_ok; } seen;
static int on_refresh(void *context, const sh_modserve_refresh_item *it)
{
    seen *s = (seen *)context;
    if (!strcmp(it->request, AN_WALK)) { s->walk++; s->kinds_ok += it->kind == SH_MODSERVE_RF_ANIM; }
    else if (!strcmp(it->request, AN_IDLE)) s->idle++;
    else if (!strcmp(it->request, AN_SKEL)) s->skel++;
    else if (!strcmp(it->request, AN_DECL)) s->decl++;
    else s->other++;
    return 1;
}

/* What the serve listener records, as modserve_engine.c does. */
static void listen(const char *request, const char *bundle_hash, const char *file_hash, const unsigned char *bytes,
                   size_t length)
{
    sh_modledger_ident id;
    if (sh_modledger_ident_of(bundle_hash, file_hash, bytes, length, &id)) (void)sh_modledger_record(request, &id);
}

static int now_ident(const char *request, sh_modledger_ident *out)
{
    const char *bundle = NULL, *file = NULL;
    const unsigned char *body = NULL;
    size_t body_len = 0;
    uint64_t size = 0;
    if (!sh_modserve_serve_identity(request, &bundle, &file, &body, &body_len, &size) || (!file && !body)) return 0;
    return sh_modledger_ident_of(bundle, file, body, body ? body_len : (size_t)size, out);
}

static int serve(const char *request)
{
    unsigned char *bytes = NULL;
    size_t len = 0;
    int r = sh_modserve_open(request, &bytes, &len);
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    return r;
}

static void test_anim_kind(const char *top)
{
    static const fx b1[] = {
        { AN_WALK, "MA-walk-one", "anim", "md6/mods/kanim000/walk.md6anim", '1' },
        { AN_IDLE, "MA-idle", "anim", "md6/mods/kanim000/idle.md6anim", '2' },
        { AN_SKEL, "SK-one", "skeleton", "md6/mods/kanim000/body.md6skl", '5' },
        { AN_DECL, "md6Def body { anim \"md6/mods/kanim000/walk.md6anim\" }", "md6Def", "mods/kanim000/body.md6", '7' },
    };
    static const fx b2[] = {
        { AN_WALK, "MA-walk-two!", "anim", "md6/mods/kanim000/walk.md6anim", '3' },
        { AN_IDLE, "MA-idle", "anim", "md6/mods/kanim000/idle.md6anim", '2' },
        { AN_SKEL, "SK-one", "skeleton", "md6/mods/kanim000/body.md6skl", '5' },
        { AN_DECL, "md6Def body { anim \"md6/mods/kanim000/walk.md6anim\" }", "md6Def", "mods/kanim000/body.md6", '7' },
    };
    static const fx b3[] = {
        { AN_WALK, "MA-walk-three", "anim", "md6/mods/kanim000/walk.md6anim", '4' },
        { AN_IDLE, "MA-idle", "anim", "md6/mods/kanim000/idle.md6anim", '2' },
        { AN_SKEL, "SK-two", "skeleton", "md6/mods/kanim000/body.md6skl", '6' },
        { AN_DECL, "md6Def body { anim \"md6/mods/kanim000/walk.md6anim\" }", "md6Def", "mods/kanim000/body.md6", '7' },
    };
    char root[MAX_PATH], h1[65], h2[65];
    const char *list[1];
    sh_modserve_diff d;
    sh_modserve_hit hit;
    sh_modledger_rec rec;
    sh_modledger_ident now;
    seen s;
    printf("-- refresh: the anim kind, one salt at two hashes (a map's place and the root)\n");
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\anims\\mods", top);
    mkdirs(root);
    make_bundle(root, "maps\\Anim map\\anim-a", "kanim000", 'a', 100, b1, 4);
    make_bundle(root, "anim-b", "kanim000", 'b', 200, b2, 4);
    hash64('a', h1);
    hash64('b', h2);
    sh_modledger_reset();
    sh_modserve_set_serve_listener(listen);
    sh_modserve_scope_enable(0);
    CHECK(sh_modserve_index_build(root) == 2);
    memset(&s, 0, sizeof s);
    sh_modserve_each_refresh(on_refresh, &s);
    sh_modserve_scope_enable(1);
    CHECK(sh_modserve_lookup(AN_WALK, &hit) && !strcmp(hit.bundle_hash, h2));   /* no map: the root build */

    /* Served once with no map open: the ledger holds the root build's bytes. */
    CHECK(serve(AN_WALK) == 1 && serve(AN_IDLE) == 1);
    CHECK(sh_modledger_find(AN_WALK, &rec) && now_ident(AN_WALK, &now));
    CHECK(sh_modledger_decide(&rec, &now, 0, 1) == SH_MODLEDGER_KEEP);
    CHECK(rec.id.bundle == 0xbbbbbbbbbbbbbbbbull && rec.id.file == 0x3333333333333333ull);

    /* A map pinning its place's build: the anim whose bytes differ is marked,
     * as an anim; the one both builds carry is not. */
    CHECK(sh_modserve_place_set("maps\\Anim map") >= 0);
    list[0] = h1;
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) >= 1);
    CHECK(sh_modserve_lookup(AN_WALK, &hit) && !strcmp(hit.bundle_hash, h1));
    /* One anim waits for the map pass; settling it there leaves none (ADD-L6 K). */
    CHECK(sh_modserve_refresh_pending_anims() == 1);
    memset(&s, 0, sizeof s);
    sh_modserve_each_refresh(on_refresh, &s);
    CHECK(s.walk == 1 && s.kinds_ok == 1 && s.idle == 0 && s.decl == 0);
    CHECK(sh_modserve_refresh_pending_anims() == 0);
    /* The ledger agrees: the held walk carries the other build's bytes, idle the same bytes. */
    CHECK(sh_modledger_find(AN_WALK, &rec) && now_ident(AN_WALK, &now));
    CHECK(sh_modledger_decide(&rec, &now, 0, 1) == SH_MODLEDGER_REFRESH);
    CHECK(sh_modledger_find(AN_IDLE, &rec) && now_ident(AN_IDLE, &now));
    CHECK(sh_modledger_decide(&rec, &now, 0, 1) == SH_MODLEDGER_KEEP);
    /* The reload reads through the provider: the ledger follows it. */
    CHECK(serve(AN_WALK) == 1 && sh_modledger_find(AN_WALK, &rec) && now_ident(AN_WALK, &now));
    CHECK(sh_modledger_decide(&rec, &now, 0, 1) == SH_MODLEDGER_KEEP && rec.serves == 2);
    /* A name the provider never served: no mod bytes are held under it. */
    CHECK(!sh_modledger_find(AN_SKEL, &rec) && now_ident(AN_SKEL, &now));
    CHECK(sh_modledger_decide(NULL, &now, 0, sh_modledger_complete()) == SH_MODLEDGER_GAME);

    /* The same listing again: nothing is marked. */
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) >= 0);
    memset(&s, 0, sizeof s);
    sh_modserve_each_refresh(on_refresh, &s);
    CHECK(s.walk == 0 && s.idle == 0 && s.skel == 0 && s.other == 0);

    /* Back to the second build: marked again. */
    list[0] = h2;
    CHECK(sh_modserve_scope_set(list, 1, NULL, 0, 1) >= 1);
    memset(&s, 0, sizeof s);
    sh_modserve_each_refresh(on_refresh, &s);
    CHECK(s.walk == 1 && s.idle == 0);
    CHECK(sh_modserve_scope_set(NULL, 0, NULL, 0, 0) >= 0);
    CHECK(sh_modserve_place_set(NULL) >= 0);
    sh_modserve_scope_enable(0);

    /* A rescan that changes the root build's anim and skeleton: the anim is
     * a pending reload, the skeleton a restart. */
    {
        char dir[MAX_PATH];
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\anim-b", root);
        make_bundle(root, "anim-b", "kanim000", 'c', 300, b3, 4);
        (void)dir;
    }
    memset(&s, 0, sizeof s);
    sh_modserve_each_refresh(on_refresh, &s);
    CHECK(sh_modserve_index_build(root) == 2);
    sh_modserve_last_diff(&d);
    CHECK(d.changed_anims == 1 && d.changed_restart == 1 && strstr(d.restart_first, AN_SKEL) != NULL);
    CHECK(d.changed_images == 0 && d.changed_models == 0);
    memset(&s, 0, sizeof s);
    sh_modserve_each_refresh(on_refresh, &s);
    CHECK(s.walk == 1 && s.kinds_ok == 1 && s.skel == 0 && s.idle == 0);
    sh_modserve_set_serve_listener(NULL);
}

/* The re-parse log's per-type tally (ADD-L6 I). An anim dependent's type is
 * a stack buffer filled from its request path; V3's line printed the bytes
 * that buffer held later ("re-parsed 1 0;>5 decl(s)"). The tally keeps its
 * own copy. */
static void tally_one(sh_modledger_tally *t, const char *request)
{
    char type[64];
    const char *slash = strchr(request, '/');
    size_t n = slash ? (size_t)(slash - request) : strlen(request);
    memcpy(type, request, n);
    type[n] = 0;
    (void)sh_modledger_tally_add(t, type, slash ? slash + 1 : request);
    memset(type, 0xA5, sizeof type);   /* the frame is reused after the call */
}

static void test_tally(void)
{
    sh_modledger_tally t;
    char longname[64];
    int i;
    memset(&t, 0, sizeof t);
    tally_one(&t, "md6Def/mods/v3-anim/imp.md6");
    tally_one(&t, "animWeb/mods/v3-anim/imp");
    tally_one(&t, "md6Def/mods/v3-anim/other.md6");
    CHECK(t.ntypes == 2);
    CHECK(!strcmp(t.types[0].type, "md6Def") && t.types[0].count == 2);
    CHECK(!strcmp(t.types[0].names, "mods/v3-anim/imp.md6, mods/v3-anim/other.md6"));
    CHECK(!strcmp(t.types[1].type, "animWeb") && t.types[1].count == 1);
    CHECK(!sh_modledger_tally_truncated(&t.types[0]));
    /* Names stop before the row fills; the log then says ", ...". */
    for (i = 0; i < 20; i++) {
        _snprintf_s(longname, sizeof longname, _TRUNCATE, "entityDef/mods/x/a_rather_long_decl_name_%02d", i);
        tally_one(&t, longname);
    }
    CHECK(t.types[2].count == 20 && sh_modledger_tally_truncated(&t.types[2]));
    CHECK(strlen(t.types[2].names) < sizeof t.types[2].names);
    /* A full table counts further types in its last row. */
    memset(&t, 0, sizeof t);
    for (i = 0; i < SH_MODLEDGER_TALLY_TYPES + 3; i++) {
        _snprintf_s(longname, sizeof longname, _TRUNCATE, "type%02d/n", i);
        tally_one(&t, longname);
    }
    CHECK(t.ntypes == SH_MODLEDGER_TALLY_TYPES);
    CHECK(!strcmp(t.types[SH_MODLEDGER_TALLY_TYPES - 1].type, "type11"));
    CHECK(t.types[SH_MODLEDGER_TALLY_TYPES - 1].count == 4);
    CHECK(sh_modledger_tally_add(NULL, "x", "y") == NULL);
}

/* `sh_mods held <filter>` folds names as the product does (ADD-L6 K): the id
 * may be spelled with '-' or '_', in any case, with either slash. */
static void test_name_filter(void)
{
    const char *req = "cooked/anim/md6/mods/v3_anim/loop.bmd6anim";
    CHECK(sh_modledger_name_contains(req, "v3-anim") && sh_modledger_name_contains(req, "v3_anim"));
    CHECK(sh_modledger_name_contains(req, "MODS/V3-ANIM/") && sh_modledger_name_contains(req, "mods\\v3_anim"));
    CHECK(sh_modledger_name_contains(req, "") && sh_modledger_name_contains(req, NULL));
    CHECK(!sh_modledger_name_contains(req, "v3.anim") && !sh_modledger_name_contains(NULL, "x"));
    CHECK(!sh_modledger_name_contains("ab", "abc"));
}

int main(void)
{
    char top[MAX_PATH], tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(top, sizeof top, _TRUNCATE, "%smodledger_test_%lu", tmp, (unsigned long)GetCurrentProcessId());
    mkdirs(top);
    test_identity();
    test_records();
    test_bound();
    test_threads();
    test_decide();
    test_anim_kind(top);
    test_tally();
    test_name_filter();
    printf("modledger_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
