/* modlook_old_test.c -- a material that writes the old keys compiles to the bytes the look branch writes for the
 * equivalent look (modlook.c lk_old_to_look).
 *
 * Two emits of one mod (the same salt) exist on a computer that has built Doomforge's Tyrant: one that writes the old
 * keys (eternalparticle*, the 28 layered vectors) and one that writes `look particle` / `look layered`. For every
 * material that differs between them, both are compiled and the compiled texts must be byte-identical, and so must
 * every derived image the compile registered (the layer parameter image, the constant images).
 *
 *   modlook_old_test.exe [old=<mod folder>] [looks=<mod folder>]
 *
 * The folders default to %LOCALAPPDATA%\doomforge\mods\tyrant-ef1d and ...\tyrant. When either is missing the test says so
 * and skips (exit 0). */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modlook.h"
#include "modprog.h"
#include "modbundle.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *message) { (void)message; }

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *t;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    t = (char *)malloc((size_t)sz + 1);
    if (t) { *n = fread(t, 1, (size_t)sz, f); t[*n] = 0; }
    fclose(f);
    return t;
}

/* ---------------------------------------------------------------------------- the images a compile registered */

typedef struct img { char path[300]; unsigned char *bytes; size_t n; } img;
typedef struct imgs { img *v; size_t n, cap; } imgs;

static void collect_cb(void *context, const char *path)
{
    imgs *s = (imgs *)context;
    unsigned char *b = NULL;
    size_t n = 0;
    if (sh_modprog_read(path, &b, &n) != SH_MB_STOCK_OK) return;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = (img *)realloc(s->v, s->cap * sizeof *s->v);
    }
    snprintf(s->v[s->n].path, sizeof s->v[s->n].path, "%s", path);
    s->v[s->n].bytes = (unsigned char *)malloc(n ? n : 1);
    memcpy(s->v[s->n].bytes, b, n);
    s->v[s->n].n = n;
    s->n++;
    HeapFree(GetProcessHeap(), 0, b);
}

static void free_imgs(imgs *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) free(s->v[i].bytes);
    free(s->v);
    memset(s, 0, sizeof *s);
}

/* Compiles one file in a clean registry: the text, and the images it registered. 1 on success. */
static int compile_file(const char *path, char **out, size_t *out_n, imgs *im)
{
    size_t n = 0;
    char *text = slurp(path, &n), why[400];
    int r;
    *out = NULL;
    memset(im, 0, sizeof *im);
    if (!text) return 0;
    sh_modprog_reset();
    r = sh_look_compile(path, text, n, out, out_n, why, sizeof why);
    if (r == SH_LOOK_SAME) { *out = text; *out_n = n; text = NULL; }
    free(text);
    if (r != SH_LOOK_SAME && r != SH_LOOK_CHANGED) return 0;
    sh_modprog_list("generated/image/", collect_cb, im);
    return 1;
}

/* ------------------------------------------------------------------------------------------ the two folders */

static int g_pairs, g_layered, g_particle, g_same;

static int is_file_diff(const char *a, const char *b)
{
    size_t an = 0, bn = 0;
    char *x = slurp(a, &an), *y = slurp(b, &bn);
    int diff = !x || !y || an != bn || memcmp(x, y, an);
    free(x);
    free(y);
    return diff;
}

static void compare_one(const char *old_path, const char *look_path, const char *rel)
{
    char *a = NULL, *b = NULL;
    size_t an = 0, bn = 0, i;
    imgs ia, ib;
    int okA = compile_file(old_path, &a, &an, &ia), okB = compile_file(look_path, &b, &bn, &ib);
    g_pairs++;
    if (b && strstr(b, "layerparms")) g_layered++; else g_particle++;
    CHECK(okA && okB);
    if (okA && okB) {
        int same = an == bn && !memcmp(a, b, an);
        if (!same) fprintf(stderr, "compiled text differs: %s\n", rel);
        CHECK(same);
        CHECK(ia.n == ib.n);
        for (i = 0; i < ia.n && i < ib.n; i++) {
            int s = !strcmp(ia.v[i].path, ib.v[i].path) && ia.v[i].n == ib.v[i].n &&
                    !memcmp(ia.v[i].bytes, ib.v[i].bytes, ia.v[i].n);
            if (!s) fprintf(stderr, "derived image differs: %s (%s)\n", rel, ia.v[i].path);
            CHECK(s);
        }
        if (same) g_same++;
    }
    free(a);
    free(b);
    free_imgs(&ia);
    free_imgs(&ib);
}

static void walk(const char *old_dir, const char *look_dir, const char *rel)
{
    char pat[MAX_PATH], op[MAX_PATH], lp[MAX_PATH], r[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\%s*", look_dir, rel);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(r, sizeof r, "%s%s%s", rel, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "\\" : "");
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walk(old_dir, look_dir, r); continue; }
        snprintf(op, sizeof op, "%s\\%s", old_dir, r);
        snprintf(lp, sizeof lp, "%s\\%s", look_dir, r);
        if (GetFileAttributesA(op) == INVALID_FILE_ATTRIBUTES) continue;
        if (is_file_diff(op, lp)) compare_one(op, lp, r);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int main(int argc, char **argv)
{
    char old_dir[MAX_PATH] = "", look_dir[MAX_PATH] = "", base[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    int i;
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (n && n < sizeof base) {
        snprintf(old_dir, sizeof old_dir, "%s\\doomforge\\mods\\tyrant-ef1d", base);
        snprintf(look_dir, sizeof look_dir, "%s\\doomforge\\mods\\tyrant", base);
    }
    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "old=", 4)) snprintf(old_dir, sizeof old_dir, "%s", argv[i] + 4);
        else if (!strncmp(argv[i], "looks=", 6)) snprintf(look_dir, sizeof look_dir, "%s", argv[i] + 6);
    }
    snprintf(a, sizeof a, "%s\\assets\\generated\\decls\\material", old_dir);
    snprintf(b, sizeof b, "%s\\assets\\generated\\decls\\material", look_dir);
    if (!old_dir[0] || GetFileAttributesA(a) == INVALID_FILE_ATTRIBUTES || GetFileAttributesA(b) == INVALID_FILE_ATTRIBUTES) {
        printf("skipped -- no old-key and look emits of one mod on this computer (%s, %s)\n", a, b);
        return 0;
    }
    walk(a, b, "");
    CHECK(g_pairs > 0);
    printf("%s -- %d materials differ between the emits (%d particle, %d layered), %d compile byte-identical; %d checks, %d failed\n",
           g_failed ? "FAILED" : "ok", g_pairs, g_particle, g_layered, g_same, g_checks, g_failed);
    return g_failed ? 1 : 0;
}
