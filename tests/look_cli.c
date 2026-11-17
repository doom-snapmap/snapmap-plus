/* look_cli.c -- compiles a material decl with the look compiler (modlook.c) and writes what the engine would read.
 *
 *   look_cli <in.decl> <out_dir>
 *
 * Writes <out_dir>\<name>.decl, the compiled text (the original text when the material is served as it is), and each
 * image the compile registered (the layered parameter image, the constant colour images) as <path with / replaced by
 * _>. Exit 0 on success, 2 with the sentence on stderr when the look has a mistake, 1 otherwise. The notes of a
 * compile (what did not fit the game's shared texture places) go to stdout. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modlook.h"
#include "modprog.h"
#include "modbundle.h"

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

static int spit(const char *dir, const char *name, const void *data, size_t n)
{
    char path[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "look_cli: cannot write %s\n", path); return 0; }
    fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

static void image_cb(void *context, const char *path)
{
    const char *dir = (const char *)context;
    unsigned char *b = NULL;
    size_t n = 0;
    char name[400], *p;
    if (sh_modprog_read(path, &b, &n) != SH_MB_STOCK_OK) return;
    snprintf(name, sizeof name, "%s", path);
    for (p = name; *p; p++) if (*p == '/') *p = '_';
    spit(dir, name, b, n);
    HeapFree(GetProcessHeap(), 0, b);
}

int main(int argc, char **argv)
{
    size_t n = 0, out_n = 0;
    char *text, *out = NULL, *notes = NULL, why[400], name[MAX_PATH], *dot;
    const char *base;
    int r;
    if (argc != 3) { fprintf(stderr, "usage: look_cli <in.decl> <out_dir>\n"); return 1; }
    if (!(text = slurp(argv[1], &n))) { fprintf(stderr, "look_cli: cannot read %s\n", argv[1]); return 1; }
    CreateDirectoryA(argv[2], NULL);
    r = sh_look_compile_notes(argv[1], text, n, &out, &out_n, why, sizeof why, &notes);
    if (r == SH_LOOK_MISTAKE) { fprintf(stderr, "%s\n", why); return 2; }
    if (r != SH_LOOK_SAME && r != SH_LOOK_CHANGED) { fprintf(stderr, "look_cli: out of memory\n"); return 1; }
    base = strrchr(argv[1], '\\');
    if (!base) base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    snprintf(name, sizeof name, "%s", base);
    if ((dot = strrchr(name, '.')) != NULL) *dot = 0;
    strcat(name, ".decl");
    if (!spit(argv[2], name, r == SH_LOOK_CHANGED ? out : text, r == SH_LOOK_CHANGED ? out_n : n)) return 1;
    sh_modprog_list("generated/image/", image_cb, argv[2]);
    if (notes) printf("%s", notes);
    printf("%s -> %s\\%s (%s)\n", argv[1], argv[2], name, r == SH_LOOK_CHANGED ? "compiled" : "served as it is");
    free(out);
    free(notes);
    free(text);
    return 0;
}
