/* modshadow_test -- the shadow safety rules (modserve.c) measured against a
 * player's real mod store in the one shape (the store tool's conversion of
 * their data, read in place and never written): the serving index resolves
 * the conflicts between the mods (one provider serves, list decls are
 * composed), and the editor plan and every bundle's gameplay plan run
 * against a registry stand-in that knows exactly the decls the game's
 * SnapMap archive holds. The report names, per bundle, every conflict
 * it takes part in and every animWeb a registration pass withdraws because its
 * models block names a model nothing provides.
 *
 * Inputs (the test is skipped without the first two; the tree holds game
 * files, so it is not in the repository):
 *   SH_MODSHADOW_DATA        a data root holding mods\ in the one shape (a copy
 *                            the store tool converted; nothing is written)
 *   SH_MODSHADOW_GAME_NAMES  "type<TAB>name" lines, lower case, one per decl
 *                            and resource of snap_gameresources (the decl
 *                            registry a SnapMap session has)
 *   SH_MODSHADOW_DOOM        the DOOM install the builds read the game's files
 *                            from (default D:\SteamLibrary\steamapps\common\DOOM)
 *   SH_MODSHADOW_ECHO        set to echo every MODS: line */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modserve.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modstock.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ---- log capture: every line kept for the report ------------------------------------------- */

static int g_echo;
static char *g_log;
static size_t g_log_len, g_log_cap;
void backend_log(const char *line)
{
    size_t n = line ? strlen(line) : 0;
    if (!line) return;
    if (g_echo) printf("    %s\n", line);
    if (g_log_len + n + 2 > g_log_cap) {
        size_t cap = g_log_cap ? g_log_cap * 2 : (1u << 20);
        char *grown;
        while (cap < g_log_len + n + 2) cap *= 2;
        grown = (char *)realloc(g_log, cap);
        if (!grown) return;
        g_log = grown;
        g_log_cap = cap;
    }
    memcpy(g_log + g_log_len, line, n);
    g_log_len += n;
    g_log[g_log_len++] = '\n';
    g_log[g_log_len] = 0;
}
static void log_mark(size_t *at) { *at = g_log_len; }

/* ---- the game's names --------------------------------------------------------------------- */

static char **g_names;
static size_t g_nnames;
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int load_names(const char *path)
{
    size_t len = 0, i, start = 0, cap = 0;
    unsigned char *text = sh_mb_read_file(path, 64u << 20, &len);
    if (!text) return 0;
    for (i = 0; i <= len; i++) {
        if (i < len && text[i] != '\n') continue;
        if (i > start) {
            char *line = (char *)malloc(i - start + 1);
            size_t k;
            if (!line) return 0;
            memcpy(line, text + start, i - start);
            line[i - start] = 0;
            for (k = 0; line[k]; k++) {
                if (line[k] == '\r') { line[k] = 0; break; }
                if (line[k] >= 'A' && line[k] <= 'Z') line[k] = (char)(line[k] + 32);
            }
            if (g_nnames == cap) {
                cap = cap ? cap * 2 : 65536;
                g_names = (char **)realloc(g_names, cap * sizeof(char *));
                if (!g_names) return 0;
            }
            g_names[g_nnames++] = line;
        }
        start = i + 1;
    }
    free(text);
    qsort(g_names, g_nnames, sizeof(char *), name_cmp);
    return 1;
}

static int game_has(const char *type, const char *name)
{
    char key[1200], *k = key;
    size_t i;
    _snprintf_s(key, sizeof key, _TRUNCATE, "%s\t%s", type, name);
    for (i = 0; key[i]; i++) if (key[i] >= 'A' && key[i] <= 'Z') key[i] = (char)(key[i] + 32);
    return bsearch(&k, g_names, g_nnames, sizeof(char *), name_cmp) != NULL;
}

/* ---- builder entry (modcmd.c in the product) and the registry stand-in ------------------------ */

/* The game's files, from the install's resource reader (modstock.c), as the
 * store tool's conversion builds against the DOOM folder. */
static int stock_exists(void *ctx, const char *request) { (void)ctx; return sh_modstock_exists(request); }

int modbundle_build_folder(const char *folder, char *first_cause, size_t n)
{
    sh_mb_resolver r = { NULL, NULL, stock_exists, NULL };
    sh_mb_bundle b;
    char cause[SH_MB_CAUSE_MAX];
    int ok = 0;
    if (!sh_mb_build(folder, NULL, &r, &b)) strncpy_s(first_cause, n, b.first_cause, _TRUNCATE);
    else if (!sh_mb_write(&b, folder, cause, sizeof cause)) strncpy_s(first_cause, n, cause, _TRUNCATE);
    else ok = 1;
    sh_mb_free(&b);
    return ok;
}

int modserve_activate_bundles(const char *const *bundle_hashes, int n) { (void)bundle_hashes; (void)n; return 0; }

#define MAX_REG 65536
static char *g_reg[MAX_REG];
static int g_nreg;
static int registered(const char *type, const char *name)
{
    int i;
    for (i = 0; i < g_nreg; i++) {
        const char *r = g_reg[i];
        size_t t = strlen(type);
        if (!_strnicmp(r, type, t) && r[t] == '\t' && !_stricmp(r + t + 1, name)) return 1;
    }
    return 0;
}
static int op_source(void *c, const char *type, const char *name)
{
    (void)c;
    return game_has(type, name) || registered(type, name);
}
static int op_register(void *c, const char *type, const char *name, const char *reg)
{
    char buf[1200];
    (void)c; (void)reg;
    if (g_nreg >= MAX_REG) return 0;
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s\t%s", type, name);
    g_reg[g_nreg++] = _strdup(buf);
    return 1;
}
static int op_load(void *c, const char *type, const char *name, int persist, unsigned *state, unsigned *lifetime)
{
    (void)c; (void)type; (void)name;
    *state = 0;
    *lifetime = persist ? 4 : 1;
    return 1;
}
static const sh_modserve_engine_ops OPS = { NULL, op_source, op_register, op_load };

/* ---- the report ---------------------------------------------------------------------------- */

typedef struct mb_row { char id[80], hash[65]; int admitted, files, place; } mb_row;
static mb_row g_rows[512];
static int g_nrows;
static void each_bundle(void *context, const char *id, const char *hash, int admitted, const char *cause, int files,
                        int place)
{
    mb_row *r;
    (void)context; (void)cause;
    if (g_nrows >= 512) return;
    r = &g_rows[g_nrows++];
    strncpy_s(r->id, sizeof r->id, id ? id : "", _TRUNCATE);
    strncpy_s(r->hash, sizeof r->hash, hash ? hash : "", _TRUNCATE);
    r->admitted = admitted;
    r->files = files;
    r->place = place;
}

/* Print every line from `from` that contains all of the needles. */
static int print_lines(size_t from, const char *a, const char *b, const char *prefix)
{
    const char *p = g_log + from;
    int n = 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[2048];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        if (strstr(line, a) && (!b || strstr(line, b))) { if (prefix) printf("%s%s\n", prefix, line); n++; }
        if (!e) break;
        p = e + 1;
    }
    return n;
}

static int g_conductors;
static void count_conductor(void *context, const char *id, const char *hash, const char *registries)
{
    (void)context; (void)id; (void)hash;
    if (registries && strstr(registries, "\"conductor\"")) g_conductors++;
}

int main(void)
{
    char root[MAX_PATH], names[MAX_PATH], mods[MAX_PATH];
    sh_modserve_stats ss;
    sh_modserve_plan plan;
    size_t at;
    int i, admitted, total_withdrawn = 0, total_refused = 0;
    printf("modshadow_test\n");
    g_echo = GetEnvironmentVariableA("SH_MODSHADOW_ECHO", NULL, 0) > 0;
    if (!GetEnvironmentVariableA("SH_MODSHADOW_DATA", root, sizeof root) ||
        !GetEnvironmentVariableA("SH_MODSHADOW_GAME_NAMES", names, sizeof names)) {
        printf("  [skip] SH_MODSHADOW_DATA and SH_MODSHADOW_GAME_NAMES do not name a data root and the game's names\n");
        return 0;
    }
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    CHECK(GetFileAttributesA(mods) != INVALID_FILE_ATTRIBUTES);
    CHECK(load_names(names) && g_nnames > 10000);
    printf("  game names: %zu\n", g_nnames);
    {
        char doom[MAX_PATH];
        if (!GetEnvironmentVariableA("SH_MODSHADOW_DOOM", doom, sizeof doom))
            strcpy_s(doom, sizeof doom, "D:\\SteamLibrary\\steamapps\\common\\DOOM");
        sh_modstock_set_install(doom);
        sh_modstock_register();
    }
    sh_ms_set_root(mods);

    /* 1. The serving index over the player's store, as the game reads it at boot. */
    log_mark(&at);
    admitted = sh_modserve_index_build(mods);
    sh_modserve_get_stats(&ss);
    printf("  index: %d bundles admitted of %d, %d files, %d shadows, %d conflicts (%d composed)\n", admitted,
           ss.bundles_seen, ss.files, ss.shadows, ss.conflicts, ss.conflicts_composed);
    CHECK(admitted > 0);
    CHECK(print_lines(at, "serving neither", NULL, NULL) == 0);
    printf("  conflicts:\n");
    print_lines(at, "MODS: conflict:", NULL, "    ");
    /* The boss mods list their encounters for the conductor the product
     * generates per map (registries "conductor"), never as a shadow of it. */
    CHECK(!sh_modserve_lookup("generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl", NULL));
    g_conductors = 0;
    sh_modserve_each_bundle_registries(count_conductor, NULL);
    printf("  bundles listing encounters for the conductor: %d\n", g_conductors);
    CHECK(g_conductors >= 2);
    {
        /* SH_MODSHADOW_DUMP: a folder that receives the served bytes of every conflicted decl. */
        static const char *const dump[] = {
            "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
            "generated/decls/snappropertyinspector_whitelistencounterdecl/single_encounters_white_list_inspector.decl",
            "generated/decls/snappropertyinspector_whitelistencounterdecllist/single_encounters_white_list_list_inspector.decl",
        };
        char dir[MAX_PATH], path[MAX_PATH * 2];
        size_t d;
        if (GetEnvironmentVariableA("SH_MODSHADOW_DUMP", dir, sizeof dir))
            for (d = 0; d < sizeof dump / sizeof dump[0]; d++) {
                unsigned char *bytes = NULL;
                size_t len = 0;
                FILE *f;
                const char *leaf = strrchr(dump[d], '/');
                if (sh_modserve_open(dump[d], &bytes, &len) != 1) continue;
                _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, leaf ? leaf + 1 : dump[d]);
                if (fopen_s(&f, path, "wb") == 0) { fwrite(bytes, 1, len, f); fclose(f); }
                HeapFree(GetProcessHeap(), 0, bytes);
            }
    }

    /* 3. Editor entry: every row and its closure. */
    log_mark(&at);
    CHECK(sh_modserve_plan_editor(&plan) >= 0);
    printf("  editor plan: %d steps, %d blocked, %d ready\n", plan.count, plan.blocked, sh_modserve_plan_run(&plan, &OPS, "editor"));
    CHECK(plan.count > 0 && plan.blocked == 0);
    sh_modserve_plan_free(&plan);
    printf("  boss rows:\n");
    print_lines(at, "snapEditorEntityDef 'demons/cyberdemon", NULL, "    ");
    print_lines(at, "snapEditorEntityDef 'demons/hell_guard", NULL, "    ");
    print_lines(at, "snapEditorEntityDef 'demons/spider_mastermind", NULL, "    ");
    print_lines(at, "WITHDRAWN", NULL, "    ");
    print_lines(at, "NOT READY: it names model", NULL, "    ");

    /* 4. Every bundle's gameplay plan, as a map naming it activates it. */
    sh_modserve_each_bundle(each_bundle, NULL);
    printf("  per bundle (gameplay plan; animWebs withdrawn or refused):\n");
    for (i = 0; i < g_nrows; i++) {
        const char *h[1];
        int unknown = 0, ready, withdrawn, refused;
        if (!g_rows[i].admitted) continue;
        h[0] = g_rows[i].hash;
        log_mark(&at);
        if (sh_modserve_plan_bundles(h, 1, &plan, &unknown) < 0) { CHECK(0); continue; }
        ready = plan.count ? sh_modserve_plan_run(&plan, &OPS, "map") : 0;
        withdrawn = print_lines(at, "shadow WITHDRAWN", NULL, NULL);
        refused = print_lines(at, "NOT READY: it names model", NULL, NULL);
        total_withdrawn += withdrawn;
        total_refused += refused;
        printf("    %s%s: %d steps, %d ready, %d blocked; %d stock animWeb shadow(s) withdrawn, %d new animWeb(s) refused\n",
               g_rows[i].place ? "in a place: " : "", g_rows[i].id, plan.count, ready, plan.blocked,
               withdrawn, refused);
        print_lines(at, "shadow WITHDRAWN", NULL, "        ");
        print_lines(at, "NOT READY: it names model", NULL, "        ");
        sh_modserve_plan_free(&plan);
    }
    printf("  totals: %d stock animWeb shadow(s) withdrawn, %d new animWeb(s) refused\n", total_withdrawn, total_refused);

    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
