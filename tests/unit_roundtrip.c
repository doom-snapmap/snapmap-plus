/* unit_roundtrip.c -- drives the product's map-content client (modcache.c, modnet.c, modkey.c, modunit.c) through a
 * publish, download, remix, second-map, missing-file report and unpublish round trip against a mod store. It is
 * read-only against the pack; every byte it writes is under <work>.
 *
 *   unit_roundtrip store=<url> work=<folder> mod=<built pack folder> doom=<install root> names=<placed A>,<placed B>
 *                  placed=<the entity map 1 places, below the namespace>
 *
 * The pack is built in place (its mod.json block) as unit_cli does. Prints one line per step and a JSON summary. */
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
#include "modmanifest.h"
#include "modcache.h"
#include "modkey.h"
#include "modnet.h"
#include "text.h"
#include "../src/backend/script/script_compile.h"
#include "modserve.h"
#include "../src/backend/nav_bake.h"
#include "../src/backend/script/script_runtime.h"

/* The archive lines modcache logs: how many archive requests a step made and what the last one brought. */
static int g_archives;
static char g_archive_line[512];
void backend_log(const char *message)
{
    if (!message || !strstr(message, "content")) return;
    printf("  log: %s\n", message);
    if (strstr(message, "'s archive: ")) { g_archives++; strcpy_s(g_archive_line, sizeof g_archive_line, message); }
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
/* The serving layer's activation (not under SH_MODSERVE_PURE): nothing is activated here. */
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
        strcpy_s(source, cap, "product");
        return 1;
    }
    got = sh_modstock_exists(request);
    if (got != 1) return got;
    if (sh_modstock_identify(request, sha256, &size) != SH_MB_STOCK_OK) return -1;
    if (sh_modstock_container(request, sha256, source, cap) != SH_MB_STOCK_OK) strcpy_s(source, cap, "loose");
    return 1;
}

/* The author's local mod: the pack folder's files by SHA-256 (what the default local index finds in mods\). */
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

static double now_s(void) { return (double)GetTickCount64() / 1000.0; }

static void report(void *ctx, unsigned long long done, unsigned long long total) { (void)ctx; (void)done; (void)total; }

int main(int argc, char **argv)
{
    const char *store = NULL, *work = NULL, *mod = NULL, *doom = NULL, *names = NULL, *placed_name = "";
    char root_a[MAX_PATH], root_b[MAX_PATH], ref1[64], ref2[64], remix[64], sa1[65], sa2[65], sb[65], *why = NULL;
    char name_a[512] = "", name_b[512] = "";
    sh_unit ua, ub;
    sh_unit_env env;
    sh_key *ka = NULL, *kb = NULL;
    modnet_signer sig_a, sig_b;
    modnet_io io;
    modnet_result r;
    sh_cache_upload_stats st;
    sh_modmanifest_row row1, row2;
    sh_modmanifest_list list;
    long long rev = 0;
    char (*removed)[MODNET_HASH_HEX + 1] = NULL;
    size_t nremoved = 0;
    double t0;
    int k, complete = 0, code;
    char build_a[65] = "", build_b[65] = "";

    for (k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "store=", 6)) store = argv[k] + 6;
        else if (!strncmp(argv[k], "work=", 5)) work = argv[k] + 5;
        else if (!strncmp(argv[k], "mod=", 4)) mod = argv[k] + 4;
        else if (!strncmp(argv[k], "doom=", 5)) doom = argv[k] + 5;
        else if (!strncmp(argv[k], "names=", 6)) names = argv[k] + 6;
        else if (!strncmp(argv[k], "placed=", 7)) placed_name = argv[k] + 7;
    }
    if (!store || !work || !mod || !doom || !names || !strchr(names, ',')) {
        fprintf(stderr, "usage: unit_roundtrip store=<url> work=<folder> mod=<pack> doom=<install> names=<a>,<b>\n");
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
    io.progress = report;
    _snprintf_s(root_a, sizeof root_a, _TRUNCATE, "%s\\author\\mods", work);
    _snprintf_s(root_b, sizeof root_b, _TRUNCATE, "%s\\player\\mods", work);
    { char p[MAX_PATH]; _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\author", work); CreateDirectoryA(work, NULL); CreateDirectoryA(p, NULL);
      CreateDirectoryA(root_a, NULL); _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\player", work); CreateDirectoryA(p, NULL); CreateDirectoryA(root_b, NULL); }
    _snprintf_s(ref1, sizeof ref1, _TRUNCATE, "rt-map1-%llu", (unsigned long long)GetTickCount64());
    _snprintf_s(ref2, sizeof ref2, _TRUNCATE, "rt-map2-%llu", (unsigned long long)GetTickCount64());
    _snprintf_s(remix, sizeof remix, _TRUNCATE, "rt-remix-%llu", (unsigned long long)GetTickCount64());

    /* The author: the pack built, the units of two placed objects, a publishing key. */
    t0 = now_s();
    if (!sh_mb_build(mod, NULL, &g_resolver, &g_pack)) { fprintf(stderr, "the pack does not build: %s\n", sh_mb_first_cause(&g_pack)); return 1; }
    g_pack_dir = mod;
    memset(&env, 0, sizeof env);
    env.game = game;
    if (!sh_unit_of(&g_pack, mod, name_a, &env, &ua, &why) || !sh_unit_of(&g_pack, mod, name_b, &env, &ub, &why)) {
        fprintf(stderr, "a unit: %s\n", why ? why : "?");
        return 1;
    }
    printf("units  %s %.16s (%zu files, %.1f MiB)  %s %.16s (%zu files, %.1f MiB)  in %.1f s\n", name_a, ua.hash, ua.files,
           (double)ua.bytes / 1048576.0, name_b, ub.hash, ub.files, (double)ub.bytes / 1048576.0, now_s() - t0);
    sh_cache_set_mods_root(root_a);
    sh_cache_set_local(pack_file, NULL);
    step(sh_cache_put_manifest(ua.hash, ua.manifest, ua.manifest_len) && sh_cache_put_manifest(ub.hash, ub.manifest, ub.manifest_len),
         "author: the manifests in the author's cache", NULL);
    step(sh_key_create(&ka, &why) && sh_key_create(&kb, &why), "two publishing keys", why);
    sh_key_set_name(ka, "RoundTrip Author");
    sh_key_set_name(kb, "RoundTrip Player");
    sig_a = sh_key_signer(ka);
    sig_b = sh_key_signer(kb);

    /* 1. The author publishes map 1 with the first object. */
    t0 = now_s();
    code = sh_cache_publish_unit(ua.hash, &sig_a, &io, &st, &why);
    {
        char d[256];
        _snprintf_s(d, sizeof d, _TRUNCATE, "%.1f MiB sent in %u archive(s) and %u part(s), %u requests, %.1f s",
                    (double)st.bytes_sent / 1048576.0, st.archives, st.parts, st.requests, now_s() - t0);
        step(code == MODNET_OK && st.held, "author: publish map 1's unit (begin, archives, complete)", code == MODNET_OK ? d : why);
    }
    {
        const char *units[1] = { ua.hash };
        sh_key_claim_secret(ka, ref1, sa1);
        code = modnet_map_claim(ref1, units, 1, sa1, 1, &sig_a, &rev, &removed, &nremoved, &io, &r);
        step(code == MODNET_OK && rev == 1 && r.status == 201, "author: claim map 1 (rev 1, signed)", r.message);
        code = modnet_map_claim(ref1, units, 1, sa1, 1, &sig_a, &rev, &removed, &nremoved, &io, &r);
        step(code == MODNET_E_CONFLICT && rev == 1, "author: a stale rev is refused and names the stored rev", r.message);
    }
    {
        const char *units[1] = { ua.hash };
        int state = 9;
        code = modnet_unit_status(units, 1, &state, &io, &r);
        step(code == MODNET_OK && state == MODNET_UNIT_HELD, "the store holds the unit", r.message);
    }
    /* The author composes the row from their own folder (what their game serves). */
    memset(&row1, 0, sizeof row1);
    strcpy_s(row1.salt, sizeof row1.salt, g_pack.id);
    {
        const char *roots[1] = { ua.root }, *units[1] = { ua.hash };
        char path[MAX_PATH * 2];
        sh_mb_bundle c;
        sh_modmanifest_row_pin(&row1, roots, units, 1);
        sh_modmanifest_row_set_name(&row1, g_pack.name ? g_pack.name : "the pack");
        memset(&list, 0, sizeof list);
        sh_modmanifest_list_add(&list, &row1);
        sh_cache_on_load(&list, NULL, 0);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s", root_a, row1.build);
        memset(&c, 0, sizeof c);
        step(sh_cache_composed(row1.build, &complete) && complete && sh_mb_load_folder(path, &c), "author: map 1's row composed from the folder", NULL);
        strcpy_s(build_a, sizeof build_a, c.hash);
        sh_mb_free(&c);
        sh_modmanifest_list_free(&list);
    }

    /* 2. A second player: a clean cache, the map's row fetched and composed (no question: no local mod). */
    sh_cache_set_local(NULL, NULL);
    sh_cache_set_mods_root(root_b);
    {
        char path[MAX_PATH * 2];
        sh_mb_bundle c;
        memset(&list, 0, sizeof list);
        sh_modmanifest_list_add(&list, &row1);
        sh_cache_note_rows(&list, NULL, 0);
        sh_modmanifest_list_free(&list);
        t0 = now_s();
        g_archives = 0;
        code = sh_cache_prepare(row1.build, &io);
        {
            char want[96];
            _snprintf_s(want, sizeof want, _TRUNCATE, " of %zu in the unit", ua.files);   /* stored: its distinct blobs */
            step(g_archives == 1 && strstr(g_archive_line, want) && strstr(g_archive_line, "every file"),
                 "player: one archive request brought every file of the unit", g_archive_line);
        }
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s", root_b, row1.build);
        memset(&c, 0, sizeof c);
        step(code == 0 && sh_cache_composed(row1.build, &complete) && complete && sh_mb_load_folder(path, &c),
             "player: map 1's unit downloaded into a clean cache and composed", NULL);
        printf("      %.1f s; composition %.16s against the author's %.16s\n", now_s() - t0, c.hash, build_a);
        strcpy_s(build_b, sizeof build_b, c.hash);
        step(build_a[0] && !strcmp(build_a, build_b), "player: the composition is the author's, byte for byte (its build hash)", NULL);
        sh_mb_free(&c);
        {
            char *n = sh_cache_take_notice();
            step(!n, "player: nothing stood in, no notice", n);
            free(n);
        }
    }

    /* 2a. A player who has most of the files already: the rest arrives in one archive request naming only the files
     * this computer lacks; a part file left by a closed game is not read. */
    {
        char root_d[MAX_PATH], from[MAX_PATH * 2], to[MAX_PATH * 2], dir[MAX_PATH * 2];
        size_t i, skipped = 0, kept = 0;
        _snprintf_s(root_d, sizeof root_d, _TRUNCATE, "%s\\partial", work);
        CreateDirectoryA(root_d, NULL);
        _snprintf_s(root_d, sizeof root_d, _TRUNCATE, "%s\\partial\\mods", work);
        CreateDirectoryA(root_d, NULL);
        sh_cache_set_mods_root(root_d);
        sh_cache_put_manifest(ua.hash, ua.manifest, ua.manifest_len);
        for (i = 0; i < g_pack.file_count; i++) {
            const sh_mb_file *f = &g_pack.files[i];
            if (!ua.keep[i] || f->stock || f->absent) continue;
            _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\.content\\files\\%.2s\\%s", root_b, f->blob, f->blob);
            if (skipped < 3 && f->size > 100000) {
                if (skipped == 0) {
                    /* half of it, as a broken-off download leaves it */
                    HANDLE src = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL), dst;
                    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\.content\\partial", root_d);
                    { char c[MAX_PATH * 2]; _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\.content", root_d); CreateDirectoryA(c, NULL); }
                    CreateDirectoryA(dir, NULL);
                    _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\%s.part", dir, f->blob);
                    dst = CreateFileA(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                    if (src != INVALID_HANDLE_VALUE && dst != INVALID_HANDLE_VALUE) {
                        DWORD want = (DWORD)(f->size / 2), got = 0, put = 0;
                        unsigned char *buf = (unsigned char *)malloc(want);
                        if (buf && ReadFile(src, buf, want, &got, NULL)) WriteFile(dst, buf, got, &put, NULL);
                        free(buf);
                    }
                    if (src != INVALID_HANDLE_VALUE) CloseHandle(src);
                    if (dst != INVALID_HANDLE_VALUE) CloseHandle(dst);
                }
                skipped++;
                continue;
            }
            _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\.content\\files\\%.2s", root_d, f->blob);
            { char c[MAX_PATH * 2]; _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\.content", root_d); CreateDirectoryA(c, NULL);
              _snprintf_s(c, sizeof c, _TRUNCATE, "%s\\.content\\files", root_d); CreateDirectoryA(c, NULL); }
            CreateDirectoryA(dir, NULL);
            _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\%s", dir, f->blob);
            if (CopyFileA(from, to, FALSE)) kept++;
        }
        memset(&list, 0, sizeof list);
        sh_modmanifest_list_add(&list, &row1);
        sh_cache_note_rows(&list, NULL, 0);
        sh_modmanifest_list_free(&list);
        g_archives = 0;
        code = sh_cache_prepare(row1.build, &io);
        {
            char d[160];
            _snprintf_s(d, sizeof d, _TRUNCATE, "%zu files here, 3 in one archive request", kept);
            step(code == 0 && sh_cache_composed(row1.build, &complete) && complete, "player: the files it lacks arrive in one request", d);
            step(g_archives == 1 && strstr(g_archive_line, "(a mask)") && !strstr(g_archive_line, ": 0 file(s) stored"),
                 "player: the one archive request was a mask of what this computer lacked", g_archive_line);
        }
    }

    /* 2b. A third player with no connection to the store: the map loads with a stand-in for the object and one
     * notice at a play, and an editor open repeats it. */
    {
        char root_c[MAX_PATH], path[MAX_PATH * 2], *n, *placed = sh_textf("mods/%s/%s", g_pack.id, placed_name);
        int complete_c = 1;
        _snprintf_s(root_c, sizeof root_c, _TRUNCATE, "%s\\offline", work);
        CreateDirectoryA(root_c, NULL);
        _snprintf_s(root_c, sizeof root_c, _TRUNCATE, "%s\\offline\\mods", work);
        CreateDirectoryA(root_c, NULL);
        sh_cache_set_mods_root(root_c);
        modnet_set_base_url("http://127.0.0.1:9");
        memset(&list, 0, sizeof list);
        sh_modmanifest_list_add(&list, &row1);
        sh_cache_note_rows(&list, placed ? &placed : NULL, placed ? 1 : 0);
        sh_modmanifest_list_free(&list);
        code = sh_cache_prepare(row1.build, &io);
        step(code == 0 && sh_cache_composed(row1.build, &complete_c) && !complete_c, "offline player: the map composes with a stand-in", NULL);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.content\\served\\%s\\entities\\%s.decl", root_c, row1.build, placed_name);
        {
            size_t len = 0;
            unsigned char *text = sh_mb_read_file(path, 1u << 20, &len);
            step(text && strstr((const char *)text, "snapmaps/unknown"), "offline player: the placed entity stands in as snapmaps/unknown", path);
            free(text);
        }
        n = sh_cache_take_notice();
        step(n && strstr(n, "the mod store could not be reached"), "offline player: one notice at a play (S4)", n);
        free(n);
        code = sh_cache_compose_now(row1.build, SH_CACHE_ABSENT_OFFLINE, &why);
        n = sh_cache_take_notice();
        /* The open says what the play's fetch learned (a load that asks nothing repeats the recorded cause). */
        step(code && n && strstr(n, "the mod store could not be reached"), "offline player: one notice at an editor open (S4, as the play learned)", n);
        free(n);
        free(placed);
        modnet_set_base_url(store);
    }

    sh_cache_set_mods_root(root_b);
    /* 3. The player remixes map 1: the unit is held, nothing uploads; a claim of their own, their key. */
    {
        const char *units[1] = { ua.hash };
        sh_cache_upload_stats rs;
        code = sh_cache_publish_unit(ua.hash, &sig_b, &io, &rs, &why);
        step(code == MODNET_OK && rs.held && rs.bytes_sent == 0, "player: remix: the store holds the unit, nothing uploads", why);
        sh_key_claim_secret(kb, remix, sb);
        code = modnet_map_claim(remix, units, 1, sb, 1, &sig_b, &rev, &removed, &nremoved, &io, &r);
        step(code == MODNET_OK && rev == 1, "player: claim the remix (a second claim of the same unit)", r.message);
    }

    /* 4. The author publishes map 2 with the second object: only the files the store lacks upload. */
    sh_cache_set_mods_root(root_a);
    sh_cache_set_local(pack_file, NULL);
    t0 = now_s();
    code = sh_cache_publish_unit(ub.hash, &sig_a, &io, &st, &why);
    {
        char d[256];
        _snprintf_s(d, sizeof d, _TRUNCATE, "%.1f MiB sent of a %.1f MiB unit, %.1f s", (double)st.bytes_sent / 1048576.0,
                    (double)ub.bytes / 1048576.0, now_s() - t0);
        step(code == MODNET_OK && st.held && st.bytes_sent < ub.bytes + (1u << 20), "author: publish map 2 (shared files not sent again)", code == MODNET_OK ? d : why);
    }
    {
        const char *units[1] = { ub.hash };
        sh_key_claim_secret(ka, ref2, sa2);
        code = modnet_map_claim(ref2, units, 1, sa2, 1, &sig_a, &rev, &removed, &nremoved, &io, &r);
        step(code == MODNET_OK && rev == 1, "author: claim map 2", r.message);
    }

    /* 5. A player's game asked for a file the unit lacks; the author reads it at their next publish. */
    {
        const char *refs[1] = { ref1 }, *secrets[1] = { sa1 };
        char *json = NULL;
        code = modnet_unit_asked(ua.hash, "images/roundtrip_asked.bimage", &io, &r);
        step(code == MODNET_OK, "a player reports a missing file of the unit", r.message);
        code = modnet_asked_units(refs, secrets, 1, &sig_a, &json, &io, &r);
        step(code == MODNET_OK && json && strstr(json, "images/roundtrip_asked.bimage"), "author: reads the asked path by the claim", json);
        free(json);
    }

    /* 6. Unpublish: the author releases map 1 (the remix keeps the unit), then the player releases the remix and
     * the author map 2: the units are unclaimed and still served through the deletion delay. */
    {
        const char *units[2] = { ua.hash, ub.hash };
        int states[2] = { 9, 9 };
        char served[MAX_PATH * 2];
        code = modnet_map_release(ref1, sa1, 2, &sig_a, &rev, &io, &r);
        step(code == MODNET_OK, "author: unpublish map 1", r.message);
        code = modnet_unit_status(units, 2, states, &io, &r);
        step(code == MODNET_OK && states[0] == MODNET_UNIT_HELD, "the remix's claim keeps the unit", NULL);
        code = modnet_map_release(remix, sb, 2, &sig_b, &rev, &io, &r);
        step(code == MODNET_OK, "player: unpublish the remix", r.message);
        code = modnet_map_release(ref2, sa2, 2, &sig_a, &rev, &io, &r);
        step(code == MODNET_OK, "author: unpublish map 2", r.message);
        code = modnet_unit_status(units, 2, states, &io, &r);
        step(code == MODNET_OK && states[0] == MODNET_UNIT_HELD && states[1] == MODNET_UNIT_HELD,
             "unclaimed units are still held during the 24-hour deletion delay", NULL);
        _snprintf_s(served, sizeof served, _TRUNCATE, "%s\\served-after-release.zip", work);
        code = modnet_get_unit_archive(ua.hash, NULL, 0, served, 0, &io, &r);
        step(code == MODNET_OK && r.status == 200, "an unclaimed unit is still served (a transfer in flight never meets a delete)", r.message);
        DeleteFileA(served);
        code = modnet_map_release(ref1, sa1, 3, &sig_a, &rev, &io, &r);
        step(code == MODNET_E_NOT_FOUND, "a released claim is gone", NULL);
    }
    printf("{\"done\":true,\"failed\":%d,\"unitA\":\"%s\",\"unitB\":\"%s\",\"composition\":\"%s\"}\n", failures, ua.hash, ub.hash, build_b);
    free(removed);
    free(why);
    sh_key_free(ka);
    sh_key_free(kb);
    sh_modmanifest_row_free(&row1);
    (void)row2;
    sh_unit_free(&ua);
    sh_unit_free(&ub);
    sh_mb_free(&g_pack);
    return failures ? 1 : 0;
}
