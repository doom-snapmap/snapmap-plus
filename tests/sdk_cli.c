/* sdk_cli.c -- compiles Graphics SDK files (sdk.h) as the build and the server do and writes what the engine reads.
 *   sdk_cli <request path> <in.decl> <out_dir>      (the request path names the decl: generated/decls/fx/<name>.decl)
 *   sdk_cli @<list> <out_dir>                       each line: <request path><TAB><in.decl>
 * Writes <out_dir>\<name with / as _>.decl per file (compiled, or as is when not an SDK file) and each derived decl
 * under <out_dir>\derived\<type>\; prints "ok|same <request>" or "mistake <request>\t<sentence>", then "note <request>\t<line>"
 * per note. Exit 0 unless a file could not be read or written. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk.h"
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
    if (!f) { fprintf(stderr, "sdk_cli: cannot write %s\n", path); return 0; }
    fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

static void flat(char *s) { for (; *s; s++) if (*s == '/' || *s == '\\') *s = '_'; }

static int one(const char *request, const char *in, const char *dir)
{
    size_t n = 0, out_n = 0;
    char *text = slurp(in, &n), *out = NULL, *notes = NULL, *needs = NULL, why[400], *name;
    int r, ok = 1;
    if (!text) { fprintf(stderr, "sdk_cli: cannot read %s\n", in); return 0; }
    r = sh_sdk_compile(request, text, n, &out, &out_n, why, sizeof why, &notes, &needs);
    name = sh_sdk_decl_name(request);
    if (r == SH_LOOK_MISTAKE) printf("mistake %s\t%s\n", request, why);
    else if (r == SH_LOOK_OOM) { printf("mistake %s\tout of memory\n", request); ok = 0; }
    else {
        char file[MAX_PATH];
        snprintf(file, sizeof file, "%s.decl", name ? name : "out");
        flat(file);
        ok = spit(dir, file, r == SH_LOOK_CHANGED ? out : text, r == SH_LOOK_CHANGED ? out_n : n);
        printf("%s %s\n", r == SH_LOOK_CHANGED ? "ok" : "same", request);
        for (char *p = notes; p && *p;) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            printf("note %s\t%s\n", request, p);
            p = e ? e + 1 : NULL;
        }
    }
    free(name);
    free(out);
    free(notes);
    free(needs);
    free(text);
    return ok;
}

static void derived_cb(void *context, const char *path)
{
    const char *dir = (const char *)context;
    unsigned char *b = NULL;
    size_t n = 0;
    char sub[MAX_PATH], name[400];
    const char *t = path + strlen("generated/decls/"), *s = strchr(t, '/');
    if (!s || sh_modprog_read(path, &b, &n) != SH_MB_STOCK_OK) return;
    snprintf(sub, sizeof sub, "%s\\derived\\%.*s", dir, (int)(s - t), t);
    CreateDirectoryA(sub, NULL);
    snprintf(name, sizeof name, "%s", s + 1);
    flat(name);
    spit(sub, name, b, n);
    HeapFree(GetProcessHeap(), 0, b);
}

int main(int argc, char **argv)
{
    const char *dir;
    int ok = 1;
    char sub[MAX_PATH];
    if (argc == 3 && argv[1][0] == '@') {
        size_t n = 0;
        char *list = slurp(argv[1] + 1, &n), *p, *e;
        dir = argv[2];
        if (!list) { fprintf(stderr, "sdk_cli: cannot read %s\n", argv[1] + 1); return 1; }
        CreateDirectoryA(dir, NULL);
        for (p = list; p && *p; p = e ? e + 1 : NULL) {
            char *tab;
            e = strchr(p, '\n');
            if (e) { *e = 0; if (e > p && e[-1] == '\r') e[-1] = 0; }
            if (!*p) continue;
            tab = strchr(p, '\t');
            if (!tab) continue;
            *tab = 0;
            if (!one(p, tab + 1, dir)) ok = 0;
        }
        free(list);
    } else if (argc == 4) {
        dir = argv[3];
        CreateDirectoryA(dir, NULL);
        ok = one(argv[1], argv[2], dir);
    } else {
        fprintf(stderr, "usage: sdk_cli <request path> <in.decl> <out_dir> | sdk_cli @<list> <out_dir>\n");
        return 1;
    }
    snprintf(sub, sizeof sub, "%s\\derived", dir);
    CreateDirectoryA(sub, NULL);
    sh_modprog_list("generated/decls/", derived_cb, (void *)dir);
    return ok ? 0 : 1;
}
