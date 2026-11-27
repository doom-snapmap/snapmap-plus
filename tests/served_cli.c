/* served_cli.c -- builds every mod folder of a mods root (dot folders skipped) with the builder, the Graphics SDK's
 * compiler and the script check as dllmain.c installs them, builds the serving index over the root, and lists what the
 * engine is handed.   served_cli root=<mods root> out=<list file> [doom=<install root>]
 * The list has one sorted line per request, "<request>\t<sha256 of the served bytes>\t<size>" (a sound bank under the
 * stem the index serves it as), and "!<folder>\t<cause>" per folder that does not build; two lists compare the engine's
 * view of two folders. Exit 0, 1 when a folder failed or a request did not open; nothing outside the root is written. */
/* served_cli owners=<built mods root> <object> prints the builds that placing the object adds to a map. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modserve.h"
#include "modbundle.h"
#include "modlook.h"
#include "modprog.h"
#include "sdk.h"
#include "modstock.h"
#include "sha256.h"
#include "../src/backend/script/script_compile.h"
#include "../src/backend/script/script_runtime.h"

void backend_log(const char *message) { (void)message; }

/* The served request of a bank whose request begins `prefix` ("sound/soundbanks/pc/<stem>."). */
typedef struct bank_find { const char *prefix; char *found; } bank_find;
static void bank_found(void *ctx, const char *request, uint64_t size, const char *bundle_id, const char *bundle_hash)
{
    bank_find *q = (bank_find *)ctx;
    (void)size; (void)bundle_id; (void)bundle_hash;
    if (!q->found && request && !_strnicmp(request, q->prefix, strlen(q->prefix))) q->found = _strdup(request);
}

typedef struct lack_count { int checked, lacking; } lack_count;
static void lack_check(void *ctx, const char *request, uint64_t size, const char *bundle_id, const char *bundle_hash)
{
    lack_count *c = (lack_count *)ctx;
    char type[SH_MB_TYPE_CAP], *name = NULL, own[64];
    (void)size; (void)bundle_hash;
    if (!request || !bundle_id || strncmp(request, "generated/decls/", 16)) return;
    _snprintf_s(own, sizeof own, _TRUNCATE, "mods/%s/", bundle_id);
    if (!sh_mb_logical_name(request, type, &name) || !name) return;
    if (!_strnicmp(name, own, strlen(own))) {
        c->checked++;
        if (sh_modserve_root_lacks(bundle_id, name)) { c->lacking++; fprintf(stderr, "lacking: %s (%s)\n", name, request); }
    }
    free(name);
}

#include "../src/backend/nav_bake.h"
void sh_nav_bake_source_update_begin(void) {}
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped, unsigned char **out_bytes, size_t *out_len)
{
    (void)name; (void)read_shipped;
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    return 0;
}
int sh_nav_bake_overlay_open(const char *name, unsigned char **out, size_t *length)
{
    (void)name;
    if (out) *out = NULL;
    if (length) *length = 0;
    return 0;
}
int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{
    (void)name;
    *out_bytes = NULL;
    *out_len = 0;
    return 0;
}
int sh_navmesh_validate_aas(const unsigned char *p, size_t n, char *e, size_t cap)
{ (void)p; (void)n; if (e && cap) e[0] = 0; return 0; }
void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context) { (void)fn; (void)context; }

static int g_doom_registered;

static int stock_exists(void *ctx, const char *request)
{
    (void)ctx;
    if (g_doom_registered) return sh_modstock_exists(request);
    return sh_modprog_owns(request);
}
static sh_mb_resolver g_resolver = { NULL, NULL, stock_exists, NULL };

static void report_owner(void *context, const char *salt, const char *hash, unsigned long long size, const char *object)
{
    (void)context; (void)size;
    printf("%s\t%s\t%s\n", salt, hash, object ? object : "(dependency)");
}

typedef struct line { char *text; } line;
static line *g_lines;
static size_t g_n, g_cap;

static void add(char *text)
{
    if (!text) return;
    if (g_n == g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 1024;
        line *g = (line *)realloc(g_lines, cap * sizeof *g);
        if (!g) { free(text); return; }
        g_lines = g;
        g_cap = cap;
    }
    g_lines[g_n++].text = text;
}

static int cmp(const void *a, const void *b) { return strcmp(((const line *)a)->text, ((const line *)b)->text); }

static char *dupf(const char *fmt, const char *a, const char *b, const char *c)
{
    size_t n = strlen(fmt) + strlen(a) + strlen(b ? b : "") + strlen(c ? c : "") + 1;
    char *o = (char *)malloc(n);
    if (o) _snprintf_s(o, n, _TRUNCATE, fmt, a, b ? b : "", c ? c : "");
    return o;
}

/* Profile the existing build manifests without rebuilding the author's folders. */
static int time_editor_plans(const char *root)
{
    LARGE_INTEGER frequency, start, end;
    sh_modserve_stats stats;
    int run;
    if (sh_modserve_index_build(root) < 0) return 1;
    sh_modserve_scope_enable(1);
    (void)sh_modserve_scope_set(NULL, 0, 0);
    sh_modserve_get_stats(&stats);
    QueryPerformanceFrequency(&frequency);
    for (run = 0; run < 11; ++run) {
        sh_modserve_plan plan;
        uint64_t digest = 14695981039346656037ULL;
        int i, count;
        QueryPerformanceCounter(&start);
        count = sh_modserve_plan_editor(&plan);
        QueryPerformanceCounter(&end);
        if (count < 0) return 1;
        for (i = 0; i < count; ++i) {
            digest ^= (unsigned)plan.steps[i].file;
            digest *= 1099511628211ULL;
        }
        printf("%s %.3f ms, %d indexed files, %d steps, %d blocked, order %016llx\n",
               run ? "warm" : "cold", (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart,
               stats.files, count, plan.blocked, (unsigned long long)digest);
        sh_modserve_plan_free(&plan);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = NULL, *out = NULL, *doom = NULL;
    char pattern[MAX_PATH], **reqs = NULL;
    size_t nreq = 0, capreq = 0, i;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    FILE *f;
    int bad = 0, k;
    if (argc == 2 && !strncmp(argv[1], "plan=", 5)) return time_editor_plans(argv[1] + 5);
    for (k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "root=", 5)) root = argv[k] + 5;
        else if (!strncmp(argv[k], "out=", 4)) out = argv[k] + 4;
        else if (!strncmp(argv[k], "doom=", 5)) doom = argv[k] + 5;
        else if (argc == 3 && k == 1 && !strncmp(argv[k], "owners=", 7)) {
            const char *object = argv[2];
            if (sh_modserve_index_build(argv[k] + 7) < 0) return 1;
            sh_modserve_scope_enable(1);
            (void)sh_modserve_scope_set(NULL, 0, 0);
            return modserve_bundles_for_names_visit(&object, 1, report_owner, NULL) ? 0 : 1;
        }
    }
    if (!root || !out) {
        fprintf(stderr, "usage: served_cli root=<mods root> out=<list> [doom=<install root>]\n"
                        "       served_cli plan=<built mods root>\n");
        return 2;
    }
    if (doom) { sh_modstock_set_install(doom); sh_modstock_register(); g_doom_registered = 1; }
    sh_mb_set_script_check(sh_script_compile_check);
    sh_mb_set_look_compiler(sh_look_compile_notes);
    sh_mb_set_sdk_compiler(sh_sdk_build_compile);
    sh_modserve_set_material_filter(sh_sdk_serve_compile);
    sh_modserve_set_product_owner(sh_modprog_owns);
    sh_modserve_set_product_reader(sh_modprog_read);
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", root);
    h = FindFirstFileA(pattern, &fd);
    while (h != INVALID_HANDLE_VALUE) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            char folder[MAX_PATH], *cause = NULL;
            sh_mb_bundle b;
            _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\%s", root, fd.cFileName);
            if (!sh_mb_build(folder, NULL, &g_resolver, &b)) {
                add(dupf("!%s\t%s", fd.cFileName, sh_mb_first_cause(&b) ? sh_mb_first_cause(&b) : "refused", NULL));
                bad = 1;
            } else if (!sh_mb_write(&b, folder, &cause)) {
                add(dupf("!%s\tnot written: %s", fd.cFileName, cause ? cause : "?", NULL));
                bad = 1;
            } else {
                for (i = 0; i < b.file_count; i++) {
                    const sh_mb_file *fl = &b.files[i];
                    char stem[300], req[400];
                    if (fl->edited) continue;   /* an edited copy is never served: its derived file is */
                    /* a bank: the index names it by the hash of the bytes it serves (modserve.c ms_bank_stems), asked
                     * for once the index is built */
                    if (sh_mb_bank_stem(fl->request, stem, sizeof stem) && !fl->stock)
                        _snprintf_s(req, sizeof req, _TRUNCATE, "\x01sound/soundbanks/pc/%s.", stem);
                    else strcpy_s(req, sizeof req, fl->request);
                    if (nreq == capreq) {
                        size_t cap = capreq ? capreq * 2 : 1024;
                        char **g = (char **)realloc(reqs, cap * sizeof *g);
                        if (!g) break;
                        reqs = g;
                        capreq = cap;
                    }
                    reqs[nreq++] = _strdup(req);
                }
            }
            free(cause);
            sh_mb_free(&b);
        }
        if (!FindNextFileA(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    {
        /* The launch preload's own share (modserve_engine.c sh_modserve_launch_preload): the index over the root and
         * the plan of every editor row and its closure, as at the hub with no map open. The engine's registration of
         * the plan's steps is not offline; its time is in the live logs. */
        LARGE_INTEGER fq, t0, t1, t2;
        sh_modserve_plan plan;
        int steps;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        if (sh_modserve_index_build(root) < 0) { fprintf(stderr, "the serving index was refused\n"); return 1; }
        QueryPerformanceCounter(&t1);
        sh_modserve_scope_enable(1);
        (void)sh_modserve_scope_set(NULL, 0, 0);
        steps = sh_modserve_plan_editor(&plan);
        QueryPerformanceCounter(&t2);
        printf("served_cli: index %.1f ms; editor rows plan (the launch preload's product share) %.1f ms, %d step(s)\n",
               (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart,
               (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)fq.QuadPart, steps);
        if (steps >= 0) sh_modserve_plan_free(&plan);
        sh_modserve_scope_enable(0);
    }
    {
        /* Every decl name a built folder provides is one its folder has (sh_modserve_root_lacks never answers "no
         * longer has" for a healthy mod's own names). */
        lack_count lc = { 0, 0 };
        sh_modserve_each_served(lack_check, &lc);
        printf("served_cli: %d own decl name(s) checked, %d said to be lacking\n", lc.checked, lc.lacking);
        if (lc.lacking) bad = 1;
    }
    for (i = 0; i < nreq; i++) {
        bank_find q = { reqs[i] + 1, NULL };
        if (reqs[i][0] != 1) continue;
        sh_modserve_each_served(bank_found, &q);
        free(reqs[i]);
        reqs[i] = q.found ? q.found : _strdup("sound/soundbanks/pc/(not in the index).bnk");
    }
    /* A file the product derives from another (a material's parameter image) is served once that one has been read,
     * as the engine reads them: every request is opened once first, then listed. */
    for (i = 0; i < nreq; i++) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        if (sh_modserve_open(reqs[i], &bytes, &len) && bytes) HeapFree(GetProcessHeap(), 0, bytes);
    }
    for (i = 0; i < nreq; i++) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        char hex[SH_MB_HASH_HEX + 1], size[32];
        if (!sh_modserve_open(reqs[i], &bytes, &len) || !bytes) {
            add(dupf("%s\tNOT SERVED\t0", reqs[i], NULL, NULL));
            bad = 1;
        } else {
            sh_mb_sha256_hex(bytes, len, hex);
            _snprintf_s(size, sizeof size, _TRUNCATE, "%zu", len);
            add(dupf("%s\t%s\t%s", reqs[i], hex, size));
            HeapFree(GetProcessHeap(), 0, bytes);
        }
        free(reqs[i]);
    }
    free(reqs);
    qsort(g_lines, g_n, sizeof *g_lines, cmp);
    if (fopen_s(&f, out, "wb") || !f) { fprintf(stderr, "cannot write %s\n", out); return 2; }
    for (i = 0; i < g_n; i++) { fprintf(f, "%s\n", g_lines[i].text); free(g_lines[i].text); }
    fclose(f);
    printf("served_cli: %zu line(s) to %s%s\n", g_n, out, bad ? " (with failures)" : "");
    return bad;
}
