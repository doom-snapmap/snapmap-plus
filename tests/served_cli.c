/* served_cli.c -- builds every mod folder of a mods root and lists what the serving index gives the engine.
 *
 *   served_cli root=<mods root> out=<list file> [doom=<install root>]
 *
 * Each folder below the root (a dot folder is skipped) is built by the product's builder with the Graphics SDK's
 * compiler and the script check installed, as dllmain.c installs them, and its block written; then the serving index
 * is built over the root and every request a built folder serves is opened through the provider. The list holds one
 * line per request, sorted: "<request>\t<sha256 of the bytes the engine is handed>\t<size>", a sound bank under the
 * stem the index serves it as; and one line "!<folder>\t<cause>" per folder that does not build. Two lists compare the
 * engine's view of two folders (the served-bytes proof of a translated mod, R-NM1). Exit 0, 1 when a folder failed
 * or a request did not open. Nothing outside the root is written. */
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

int main(int argc, char **argv)
{
    const char *root = NULL, *out = NULL, *doom = NULL;
    char pattern[MAX_PATH], **reqs = NULL;
    size_t nreq = 0, capreq = 0, i;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    FILE *f;
    int bad = 0, k;
    for (k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "root=", 5)) root = argv[k] + 5;
        else if (!strncmp(argv[k], "out=", 4)) out = argv[k] + 4;
        else if (!strncmp(argv[k], "doom=", 5)) doom = argv[k] + 5;
    }
    if (!root || !out) { fprintf(stderr, "usage: served_cli root=<mods root> out=<list> [doom=<install root>]\n"); return 2; }
    if (doom) { sh_modstock_set_install(doom); sh_modstock_register(); g_doom_registered = 1; }
    sh_mb_set_script_check(sh_script_compile_check);
    sh_mb_set_look_compiler(sh_look_compile_notes);
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
    sh_modprog_reset();
    sh_modserve_set_material_filter(sh_look_compile);
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
