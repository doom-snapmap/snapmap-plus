/* unit_cli.c -- builds one mod folder, computes the units of the names given, and composes each into a folder of its
 * own.
 *
 *   unit_cli mod=<mod folder> out=<folder> doom=<install root> names=<name>[,<name>...] [compose=1]
 *   (names=@objects lists every object of the mod, one "object <name>\t<type>" line each before its unit line)
 *
 * Each name gets one line "unit <name>\t<hash>\t<kind>\t<files>\t<bytes>\t<pointers>" on stdout and its manifest at
 * <out>\units\<hash>.json; compose=1 composes the unit at <out>\<n>-unit for served_cli. Exit 0, 1 when a unit or a
 * composition failed, 2 on a usage error. load=1 uses the folder's existing build without writing it. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modbundle.h"
#include "modunit.h"
#include "modlook.h"
#include "modprog.h"
#include "modstock.h"
#include "../src/backend/script/script_compile.h"

#include "modserve.h"
#include "../src/backend/nav_bake.h"
#include "../src/backend/script/script_runtime.h"

void backend_log(const char *message) { (void)message; }
/* What the serving layer linked here would call: nothing of it runs. */
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
        strcpy_s(source, cap, SH_MODPROG_CONTAINER);
        return 1;
    }
    got = sh_modstock_exists(request);
    if (got != 1) return got;
    if (sh_modstock_identify(request, sha256, &size) != SH_MB_STOCK_OK) return -1;
    if (sh_modstock_container(request, sha256, source, cap) != SH_MB_STOCK_OK) strcpy_s(source, cap, "loose");
    return 1;
}

typedef struct src_ctx { const char *folder; } src_ctx;
static char *source_of(void *ctx, const char *path)
{
    const src_ctx *s = (const src_ctx *)ctx;
    size_t n = strlen(s->folder) + strlen(path) + 2;
    char *out = (char *)malloc(n), *p;
    if (!out) return NULL;
    _snprintf_s(out, n, _TRUNCATE, "%s\\%s", s->folder, path);
    for (p = out; *p; p++) if (*p == '/') *p = '\\';
    return out;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f;
    if (fopen_s(&f, path, "wb") || !f) return 0;
    size_t n = fwrite(data, 1, len, f);
    return fclose(f) == 0 && n == len;
}

static int run_cli(int argc, char **argv)
{
    const char *mod = NULL, *out = NULL, *doom = NULL, *names = NULL;
    int compose = 0, loaded = 0, fallback = 0, bad = 0, k, n = 0;
    const char *stock_sha = NULL, *stock_source = NULL, *separator = ",";
    char *list, *ctxp = NULL, *name, path[MAX_PATH], *cause = NULL;
    sh_mb_bundle b;
    sh_unit_env env;
    if (argc > 1 && !strncmp(argv[1], "verify=", 7)) {
        size_t len;
        unsigned char *text = sh_mb_read_file("original.json", (size_t)-1, &len);
        int valid = text && sh_mb_verify_earlier_assets_text((const char *)text, len, argv[1] + 7);
        free(text);
        return !valid;
    }
    for (k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "mod=", 4)) mod = argv[k] + 4;
        else if (!strncmp(argv[k], "out=", 4)) out = argv[k] + 4;
        else if (!strncmp(argv[k], "doom=", 5)) doom = argv[k] + 5;
        else if (!strncmp(argv[k], "names=", 6)) names = argv[k] + 6;
        else if (!strcmp(argv[k], "load=1")) loaded = 1;
        else if (!strcmp(argv[k], "fallback=1")) fallback = 1;
        else if (!strncmp(argv[k], "stock=", 6)) stock_sha = argv[k] + 6;
        else if (!strncmp(argv[k], "source=", 7)) stock_source = argv[k] + 7;
        else if (!strcmp(argv[k], "compose=1")) compose = 1;
    }
    if (doom && stock_sha && stock_source) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        sh_modstock_set_install(doom);
        if (sh_modstock_read(stock_source, stock_sha, &bytes, &len) != SH_MB_STOCK_OK || !bytes) return 1;
        k = write_file("stock.bin", bytes, len);
        HeapFree(GetProcessHeap(), 0, bytes);
        return !k;
    }
    if (!mod || !out || !doom || !names) {
        fprintf(stderr, "usage: unit_cli mod=<folder> out=<folder> doom=<install root> names=<a,b,...> [compose=1]\n");
        return 2;
    }
    sh_modstock_set_install(doom);
    sh_modstock_register();
    sh_mb_set_script_check(sh_script_compile_check);
    sh_mb_set_look_compiler(sh_look_compile_notes);
    CreateDirectoryA(out, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\units", out);
    CreateDirectoryA(path, NULL);
    if (!(loaded ? sh_mb_load_folder(mod, &b) && b.validated : sh_mb_build(mod, NULL, &g_resolver, &b))) { fprintf(stderr, "the mod does not build: %s\n", sh_mb_first_cause(&b)); return 1; }
    if (!loaded && !sh_mb_write(&b, mod, &cause)) { fprintf(stderr, "not written: %s\n", cause ? cause : "?"); return 1; }
    printf("build %s\t%s\t%zu files\t%llu bytes\tspelled %s\n", b.id, b.hash, b.file_count, b.total_size,
           b.spelled[0] ? b.spelled : "-");
    memset(&env, 0, sizeof env);
    env.game = game;
    if (!strcmp(names, "@objects")) {
        /* Every object a map can place of this mod: the walk's root types (modunit.c uw_root_file) that the mod adds
         * under its namespace (a DOOM entityDef it replaces is a change of the mod, placed as DOOM's own). */
        static const char *const types[] = { "snapModule", "snapEditorEntityDef", "entityDef" };
        size_t i, t, len = 1;
        for (i = 0; i < b.file_count; i++) if (b.files[i].pname) len += strlen(b.files[i].pname) + 1;
        list = (char *)calloc(len, 1);
        for (t = 0; list && t < 3; t++)
            for (i = 0; i < b.file_count; i++) {
                const sh_mb_file *f = &b.files[i];
                if (f->absent || f->cls != SH_MB_CLASS_ADD || !f->pname || !f->ptype || _stricmp(f->ptype, types[t]))
                    continue;
                printf("object %s\t%s\n", f->pname, types[t]);
                if (list[0]) strcat_s(list, len, ",");
                strcat_s(list, len, f->pname);
            }
        if (!list) return 1;
    } else if (!strcmp(names, "@lines")) {
        size_t length = 0;
        unsigned char *data = sh_mb_read_file("roots.txt", (size_t)-1, &length);
        list = data ? (char *)realloc(data, length + 1) : NULL;
        if (!list) { free(data); return 1; }
        list[length] = 0;
        separator = "\n";
    } else list = _strdup(names);
    for (name = strtok_s(list, separator, &ctxp); name; name = strtok_s(NULL, separator, &ctxp), n++) {
        sh_unit u;
        char *why = NULL;
        size_t i;
        FILE *keep;
        if (fallback && !bad && !strcmp(name, SH_UNIT_WHOLE)) continue;
        if (!sh_unit_of(&b, mod, name, &env, &u, &why)) {
            printf("unit %s\tFAILED\t%s\n", name, why ? why : "?");
            free(why);
            bad = 1;
            continue;
        }
        printf("unit %s\t%s\t%d\t%zu\t%llu\t%zu\t%s\n", name, u.hash, u.kind, u.files, u.bytes, u.pointers, u.root);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\units\\%s.json", out, u.hash);
        write_file(path, u.manifest, u.manifest_len);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\units\\%s.keep.txt", out, u.hash);
        if (!fopen_s(&keep, path, "wb") && keep) {
            for (i = 0; i < b.file_count; i++)
                if (u.keep[i])
                    fprintf(keep, "%s\t%s\t%llu\t%s\n", b.files[i].stock ? "pointer" : "file", b.files[i].path, b.files[i].size,
                            u.from[i] >= 0 ? b.files[u.from[i]].path : "(seed)");
            fclose(keep);
        }
        if (compose) {
            const char *m[1];
            size_t len[1];
            src_ctx sc = { mod };
            sh_mb_bundle c;
            m[0] = u.manifest;
            len[0] = u.manifest_len;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%d-unit", out, n);
            if (!sh_unit_compose(m, len, 1, source_of, &sc, b.id, b.name, &g_resolver, path, &c, &why)) {
                printf("compose %s\tFAILED\t%s\n", name, why ? why : "?");
                bad = 1;
            } else {
                printf("compose %s\t%s\t%zu files\t%llu bytes\n", name, path, c.file_count, c.total_size);
                sh_mb_free(&c);
            }
            free(why);
        }
        sh_unit_free(&u);
    }
    free(list);
    sh_mb_free(&b);
    return bad;
}

int wmain(int argc, wchar_t **args)
{
    int i, result = 2;
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return 2;
    for (i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, args[i], -1, NULL, 0, NULL, NULL);
        if (!n || !(argv[i] = malloc(n)) || !WideCharToMultiByte(CP_UTF8, 0, args[i], -1, argv[i], n, NULL, NULL))
            goto done;
    }
    result = run_cli(argc, argv);
done:
    for (i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return result;
}
