/* claim_roundtrip.c -- the publisher's side of the mod store (modclaim.c) driven as a publish drives it: key creation
 * and reuse, upload planning, claims, republishes, stale-rev recovery, queued claims, asked-path reads and releases.
 * Read-only against the pack; every byte it writes is under <work>. Run it against the Worker under Miniflare
 * (snapmap-web tests/contract/record_server.js) or the staging store:
 *   claim_roundtrip store=<url> work=<folder> mod=<built pack folder> doom=<install root> names=<placed A>,<placed B> */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modauthor.h"
#include "modbundle.h"
#include "modunit.h"
#include "modlook.h"
#include "modprog.h"
#include "modstock.h"
#include "modcache.h"
#include "modmanifest.h"
#include "modclaim.h"
#include "modnet.h"
#include "mp_json.h"
#include "text.h"
#include "../src/backend/script/script_compile.h"
#include "../src/backend/nav_bake.h"
#include "../src/backend/script/script_runtime.h"

void backend_log(const char *message)
{
    if (message && (strstr(message, "publish") || strstr(message, "claim") || strstr(message, "key")))
        printf("  log: %s\n", message);
}
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

static int stock_exists(void *ctx, const char *request)
{
    (void)ctx;
    return sh_modstock_exists(request) == 1 || sh_modprog_owns(request) ? 1 : sh_modstock_exists(request);
}
static sh_mb_resolver g_resolver = { NULL, NULL, stock_exists, NULL };

static int game(void *ctx, const char *request, char sha256[SH_MB_HASH_HEX + 1], char *source, size_t cap)
{
    unsigned long long size = 0;
    int got;
    (void)ctx;
    if (sh_modprog_owns(request)) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        if (sh_modprog_read(request, &bytes, &len) != SH_MB_STOCK_OK || !bytes) return -1;
        sh_mb_sha256_hex(bytes, len, sha256);
        HeapFree(GetProcessHeap(), 0, bytes);
        strcpy_s(source, cap, "snapmapplus");
        return 1;
    }
    got = sh_modstock_exists(request);
    if (got != 1) return got;
    if (sh_modstock_identify(request, sha256, &size) != SH_MB_STOCK_OK) return -1;
    if (sh_modstock_container(request, sha256, source, cap) != SH_MB_STOCK_OK) strcpy_s(source, cap, "loose");
    return 1;
}

static sh_mb_bundle g_pack;
static const char *g_pack_dir;
static char *pack_file(void *ctx, const char *sha, unsigned long long size)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < g_pack.file_count; i++) {
        const sh_mb_file *f = &g_pack.files[i];
        if (!f->stock && !f->absent && f->size == size && !strcmp(f->blob, sha)) {
            char *out = (char *)malloc(strlen(g_pack_dir) + strlen(f->path) + 2), *p;
            if (!out) return NULL;
            sprintf_s(out, strlen(g_pack_dir) + strlen(f->path) + 2, "%s\\%s", g_pack_dir, f->path);
            for (p = out; *p; p++) if (*p == '/') *p = '\\';
            return out;
        }
    }
    return NULL;
}

static int failures;
static void step(int ok, const char *what, const char *detail)
{
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && detail[0] ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
}

/* The rev this computer's record holds for `ref` (-1 for none). */
static long long record_rev(const char *root, const char *ref)
{
    char *text = NULL;
    long long rev = -1;
    if (sh_mb_record_get(root, MODCLAIM_CLAIMS_SECTION, &text) == 1 && text) {
        mpj *m = mpj_parse(text, strlen(text));
        const mpj *e = m ? mpj_get(m, ref) : NULL, *v = e ? mpj_get(e, "rev") : NULL;
        if (v && mpj_is_num(v)) rev = mpj_int(v);
        mpj_free(m);
    }
    free(text);
    return rev;
}

/* Set the record's rev of `ref` (another computer of the same key moved the store's on). */
static int record_set_rev(const char *root, const char *ref, long long rev)
{
    char *text = NULL, *out;
    size_t len = 0;
    int ok = 0;
    if (sh_mb_record_get(root, MODCLAIM_CLAIMS_SECTION, &text) == 1 && text) {
        mpj *m = mpj_parse(text, strlen(text));
        mpj *e = m ? (mpj *)mpj_get(m, ref) : NULL;
        if (e && mpj_set(e, "rev", mpj_new_int(rev)) && (out = mpj_dump(m, &len)) != NULL) {
            ok = sh_mb_record_put(root, MODCLAIM_CLAIMS_SECTION, out);
            free(out);
        }
        mpj_free(m);
    }
    free(text);
    return ok;
}

/* The progress a download reports: never more done than its total. */
typedef struct prog_seen { unsigned long long last_done, last_total; int over, calls; } prog_seen;
static void on_progress(void *ctx, unsigned long long done, unsigned long long total)
{
    prog_seen *p = (prog_seen *)ctx;
    p->calls++;
    if (total && done > total) p->over = 1;
    p->last_done = done;
    p->last_total = total;
}

typedef struct asked_seen { const char *unit; const char *path; int hit; } asked_seen;
static void on_asked(void *ctx, const char *unit, const char *root, const char *path)
{
    asked_seen *a = (asked_seen *)ctx;
    (void)root;
    printf("  asked: %.16s %s\n", unit, path);
    if (!strcmp(unit, a->unit) && !strcmp(path, a->path)) a->hit = 1;
}

int main(int argc, char **argv)
{
    const char *store = NULL, *work = NULL, *mod = NULL, *doom = NULL, *names = NULL;
    char root[MAX_PATH], keys[MAX_PATH], ref1[64], ref2[64], *why = NULL, name_a[512] = "", name_b[512] = "";
    sh_unit ua, ub;
    sh_unit_env env;
    modclaim_plan plan;
    modclaim_launch_stats ls;
    modnet_io io;
    modnet_result r;
    char (*removed)[MODNET_HASH_HEX + 1] = NULL;
    size_t nremoved = 0;
    int k, made = -1, code;

    for (k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "store=", 6)) store = argv[k] + 6;
        else if (!strncmp(argv[k], "work=", 5)) work = argv[k] + 5;
        else if (!strncmp(argv[k], "mod=", 4)) mod = argv[k] + 4;
        else if (!strncmp(argv[k], "doom=", 5)) doom = argv[k] + 5;
        else if (!strncmp(argv[k], "names=", 6)) names = argv[k] + 6;
    }
    if (!store || !work || !mod || !doom || !names || !strchr(names, ',')) {
        fprintf(stderr, "usage: claim_roundtrip store=<url> work=<folder> mod=<pack> doom=<install> names=<a>,<b>\n");
        return 2;
    }
    strncpy_s(name_a, sizeof name_a, names, (size_t)(strchr(names, ',') - names));
    strcpy_s(name_b, sizeof name_b, strchr(names, ',') + 1);
    if (!modnet_set_base_url(store)) { fprintf(stderr, "the store URL is not usable\n"); return 2; }
    sh_modstock_set_install(doom);
    sh_modstock_register();
    sh_mb_set_script_check(sh_script_compile_check);
    sh_mb_set_look_compiler(sh_look_compile_notes);
    sh_cache_set_resolver(&g_resolver);
    memset(&io, 0, sizeof io);
    CreateDirectoryA(work, NULL);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\mods", work);
    _snprintf_s(keys, sizeof keys, _TRUNCATE, "%s\\publishing", work);
    CreateDirectoryA(root, NULL);
    _snprintf_s(ref1, sizeof ref1, _TRUNCATE, "cr-map1-%llu", (unsigned long long)GetTickCount64());
    _snprintf_s(ref2, sizeof ref2, _TRUNCATE, "cr-map2-%llu", (unsigned long long)GetTickCount64());

    if (!sh_mb_build(mod, NULL, &g_resolver, &g_pack)) { fprintf(stderr, "the pack does not build: %s\n", sh_mb_first_cause(&g_pack)); return 1; }
    g_pack_dir = mod;
    memset(&env, 0, sizeof env);
    env.game = game;
    if (!sh_unit_of(&g_pack, mod, name_a, &env, &ua, &why) || !sh_unit_of(&g_pack, mod, name_b, &env, &ub, &why)) {
        fprintf(stderr, "a unit: %s\n", why ? why : "?");
        return 1;
    }
    printf("units  %.16s (%zu files)  %.16s (%zu files)\n", ua.hash, ua.files, ub.hash, ub.files);
    sh_cache_set_mods_root(root);
    sh_cache_set_local(pack_file, NULL);
    step(sh_cache_put_manifest(ua.hash, ua.manifest, ua.manifest_len) && sh_cache_put_manifest(ub.hash, ub.manifest, ub.manifest_len),
         "the manifests in this computer's cache", NULL);
    modclaim_set_root(root);
    modclaim_set_key_folder(keys);
    modclaim_set_budget(60000);

    /* The key: made at the first publish, then the same one. */
    step(modclaim_signer(NULL, &made, &why) && made == 1, "the first publish makes this computer's key", why);
    step(modclaim_signer(NULL, &made, &why) && made == 0, "the next one uses it", why);

    /* 1. A plan: the store lacks the unit and this computer has it. */
    {
        const char *units[1] = { ua.hash };
        char d[128];
        code = modclaim_plan_units(units, 1, &plan, &io, &why);
        _snprintf_s(d, sizeof d, _TRUNCATE, "%llu bytes to send", plan.missing_bytes);
        step(code == MODNET_OK && plan.missing == 1 && plan.state[0] == MODCLAIM_MISSING && plan.missing_bytes > 0,
             "plan: the unit is missing and sized", code == MODNET_OK ? d : why);
        code = modclaim_upload(&plan, &io, &why);
        step(code == MODNET_OK, "upload: the plan's unit sent", why);
        modclaim_plan_free(&plan);
        code = modclaim_plan_units(units, 1, &plan, &io, &why);
        step(code == MODNET_OK && plan.missing == 0 && plan.state[0] == MODCLAIM_HELD, "plan: held now, nothing to send", why);
        modclaim_plan_free(&plan);
    }

    /* 2. The claim of the published item, then a republish whose new unit the store lacks. */
    {
        const char *one[1] = { ua.hash }, *two[2] = { ua.hash, ub.hash };
        code = modclaim_claim(ref1, one, 1, &removed, &nremoved, &io, &why);
        step(code == MODNET_OK && record_rev(root, ref1) == 1 && modclaim_known(ref1), "claim: map 1 claims its unit (rev 1)", why);
        free(removed);
        code = modclaim_claim(ref1, two, 2, &removed, &nremoved, &io, &why);
        step(code == MODNET_OK && record_rev(root, ref1) == 2, "republish: the new unit uploaded by the claim (rev 2)", why);
        free(removed);
        {
            int states[2] = { 9, 9 };
            code = modnet_unit_status(two, 2, states, &io, &r);
            step(code == MODNET_OK && states[0] == MODNET_UNIT_HELD && states[1] == MODNET_UNIT_HELD, "the store holds both", r.message);
        }
        /* Another computer of the same key moved the store's rev on: this record is behind. */
        step(record_set_rev(root, ref1, 0), "the record set behind the store", NULL);
        code = modclaim_claim(ref1, two, 2, &removed, &nremoved, &io, &why);
        step(code == MODNET_OK && record_rev(root, ref1) == 3, "stale: the stored rev taken and the claim made (rev 3)", why);
        free(removed);
        {
            char (*held)[MODNET_HASH_HEX + 1] = NULL;
            size_t n = modclaim_held(&held), i;
            int a = 0, b = 0;
            for (i = 0; i < n; i++) { a |= !strcmp(held[i], ua.hash); b |= !strcmp(held[i], ub.hash); }
            free(held);
            step(a && b, "held: the sweep keeps the claimed units", NULL);
        }
    }

    /* 3. A claim that could not reach the store: queued, sent at the next start. */
    {
        const char *one[1] = { ub.hash };
        modnet_set_base_url("http://127.0.0.1:9");
        modclaim_set_budget(0);
        code = modclaim_claim(ref2, one, 1, &removed, &nremoved, &io, &why);
        free(removed);
        step(code != MODNET_OK && modnet_transient(code), "unreachable: the claim fails as transient", why);
        step(modclaim_queue(ref2, one, 1) && modclaim_known(ref2), "queued, and known", NULL);
        modnet_set_base_url(store);
        modclaim_set_budget(60000);
        memset(&ls, 0, sizeof ls);
        code = modclaim_launch(&io, &ls);
        {
            char d[128];
            _snprintf_s(d, sizeof d, _TRUNCATE, "%u retried, %u settled, %d left, %u repaired", ls.retried, ls.settled, ls.left,
                        ls.repaired);
            step(code == MODNET_OK && ls.retried == 1 && ls.left == 0 && record_rev(root, ref2) == 1, "launch: the queue sent", d);
        }
    }

    /* 4. A player's game reports a path the unit lacks; this computer reads it for its claim. */
    {
        asked_seen a = { ua.hash, "materials/claim-roundtrip/asked", 0 };
        code = modnet_unit_asked(ua.hash, a.path, &io, &r);
        step(code == MODNET_OK, "a player reports an asked path", r.message);
        code = modclaim_asked(on_asked, &a, &io, &why);
        step(code == MODNET_OK && a.hit, "asked: the path read back for this computer's claim", why);
    }

    /* 5. A player's clean cache gets a row of both units at once (four at a time) and composes it. */
    {
        char proot[MAX_PATH], pdir[MAX_PATH];
        const char *roots[2] = { ua.root, ub.root }, *units[2] = { ua.hash, ub.hash };
        sh_modmanifest_row row;
        sh_modmanifest_list list;
        prog_seen prog;
        modnet_io pio;
        int complete = 0;
        _snprintf_s(pdir, sizeof pdir, _TRUNCATE, "%s\\player", work);
        _snprintf_s(proot, sizeof proot, _TRUNCATE, "%s\\player\\mods", work);
        CreateDirectoryA(pdir, NULL);
        CreateDirectoryA(proot, NULL);
        sh_cache_set_local(NULL, NULL);
        sh_cache_set_mods_root(proot);
        memset(&row, 0, sizeof row);
        memset(&list, 0, sizeof list);
        memset(&prog, 0, sizeof prog);
        memset(&pio, 0, sizeof pio);
        strcpy_s(row.salt, sizeof row.salt, g_pack.id);
        step(sh_modmanifest_row_pin(&row, roots, units, 2) && sh_modmanifest_list_add(&list, &row), "player: the map's row of two units", NULL);
        sh_cache_on_load(&list, NULL, 0);
        pio.progress = on_progress;
        pio.context = &prog;
        code = sh_cache_prepare(row.build, &pio);
        step(code == MODNET_OK && sh_cache_composed(row.build, &complete) && complete,
             "player: both units fetched at once and the row composed whole", NULL);
        {
            char d[128];
            _snprintf_s(d, sizeof d, _TRUNCATE, "%d progress call(s), last %llu of %llu bytes", prog.calls, prog.last_done,
                        prog.last_total);
            step(prog.calls > 0 && !prog.over, "player: the progress never passed its total", d);
        }
        sh_modmanifest_list_free(&list);
        sh_modmanifest_row_free(&row);
        sh_cache_set_mods_root(root);
        sh_cache_set_local(pack_file, NULL);
    }

    /* 6. Releases: the items, then one already released. */
    code = modclaim_release(ref1, &io, &why);
    step(code == MODNET_OK && !modclaim_known(ref1), "release: map 1", why);
    code = modclaim_release(ref2, &io, &why);
    step(code == MODNET_OK && !modclaim_known(ref2), "release: map 2", why);
    code = modclaim_release(ref2, &io, &why);
    step(code == MODNET_OK, "release: an item already released is fine", why);

    printf("{\"failures\": %d}\n", failures);
    modnet_shutdown();
    return failures ? 1 : 0;
}
