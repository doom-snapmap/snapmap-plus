/* parity_test.c -- one map, every mode: a placed object resolves through the same data in editor, editor-play,
 * launch, pub-author, kept-author, pub-player and kept-player (docs/architecture.md, "Mode parity"). For every
 * file the placed objects reach, each mode's served SHA-256 must match; each mode also hides undeclared
 * namespaces' entityDefs (sh_modserve_level_hides) and serves every image its materials draw with, else the
 * surface DRAWS BLACK. Each mode starts from fresh process state. */
/* Usage: parity_test.exe runs the fixture cases; with mods=<folder> placed=<a>,<b>[,...] [map=<text>] [doom=<install>]
 * [out=<report>] [attach=<salt>,...] it checks a copy of a real mod set and exits 1 on any divergence. PARITY_DEBUG,
 * PARITY_TOKENS, PARITY_REG and PARITY_LOG (set to 1) and PARITY_DUMP=<request part> print diagnostics. */
#include "heap_out.h"
#include "cache_paths.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modserve.h"
#include "modbundle.h"
#include "modmanifest.h"
#include "modcache.h"
#include "modunit.h"
#include "modpin.h"
#include "modlook.h"
#include "modprog.h"
#include "modstock.h"
#include "mp_json.h"
#include "mod_test_util.h"
#include "../src/backend/script/script_compile.h"
#include "../src/backend/script/script_runtime.h"
#include "../src/backend/nav_bake.h"

/* links the serving layer needs offline */
void backend_log(const char *message)
{
    if (message && getenv("PARITY_LOG")) printf("  log: %s\n", message);
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

static int g_doom;   /* the installed game is registered (a real mod set run with doom=) */
static int stock_exists(void *ctx, const char *request)
{
    (void)ctx;
    if (g_doom) return sh_modstock_exists(request) == 1 || sh_modprog_owns(request) ? 1 : sh_modstock_exists(request);
    return strstr(request, "mods/") ? 0 : 1;   /* the fixtures: every name outside a mod is DOOM's */
}
static sh_mb_resolver g_resolver = { NULL, NULL, stock_exists, NULL };
static int fixture_game(void *ctx, const char *request, char sha[SH_MB_HASH_HEX + 1], char *source, size_t cap)
{
    (void)ctx;
    if (strstr(request, "mods/")) return 0;
    memset(sha, 'a', SH_MB_HASH_HEX);
    sha[SH_MB_HASH_HEX] = 0;
    strcpy_s(source, cap, "snap_gameresources");
    return 1;
}

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

/* small sets */
/* A set of strings: insertion order in v, a hash index beside it. */
typedef struct pset { char **v; size_t n, cap; size_t *slot; size_t nslot; } pset;
static size_t ps_hash(const char *x) { size_t h = 1469598103934665603ull; for (; *x; x++) h = (h ^ (unsigned char)*x) * 1099511628211ull; return h; }
static int ps_has(const pset *s, const char *x)
{
    size_t i;
    if (!s->nslot || !x) return 0;
    for (i = ps_hash(x) & (s->nslot - 1); s->slot[i]; i = (i + 1) & (s->nslot - 1))
        if (!strcmp(s->v[s->slot[i] - 1], x)) return 1;
    return 0;
}
static void ps_index(pset *s, size_t k)
{
    size_t i = ps_hash(s->v[k]) & (s->nslot - 1);
    while (s->slot[i]) i = (i + 1) & (s->nslot - 1);
    s->slot[i] = k + 1;
}
static int ps_add(pset *s, const char *x)
{
    if (!x || ps_has(s, x)) return 0;
    if (s->n == s->cap) {
        size_t c = s->cap ? s->cap * 2 : 256;
        char **g = (char **)realloc(s->v, c * sizeof *g);
        if (!g) return 0;
        s->v = g;
        s->cap = c;
    }
    if ((s->n + 1) * 2 > s->nslot) {
        size_t ns = s->nslot ? s->nslot * 2 : 1024, k;
        size_t *g = (size_t *)calloc(ns, sizeof *g);
        if (!g) return 0;
        free(s->slot);
        s->slot = g;
        s->nslot = ns;
        for (k = 0; k < s->n; k++) ps_index(s, k);
    }
    s->v[s->n] = _strdup(x);
    ps_index(s, s->n);
    s->n++;
    return 1;
}
static void ps_free(pset *s) { size_t i; for (i = 0; i < s->n; i++) free(s->v[i]); free(s->v); free(s->slot); memset(s, 0, sizeof *s); }
/* The files the last run served in the editor mode (the fixtures look into it). */
static pset g_last_served;
/* The DOOM files the installed mods change (native/ replacements, edited decls, the conductor): the engine reads
 * them in every map that declares their mod. A surface (DOOM's own bytes at DOOM's name) changes nothing until a
 * name asks for it, so it is no seed; the closure reaches it by the names served bytes spell. */
static pset g_doom_seeds, g_surfaces;
/* The DOOM decls a mod edits (a registry): the product serves DOOM's text with the mod's edits, and a unit's copy
 * keeps the edits for what the unit carries. */
static pset g_registries;

/* the name universe: every installed file, by the names that reach it */
typedef struct uentry { char *key; char *request; } uentry;
typedef struct universe { uentry *v; size_t n, cap; int sorted; } universe;
static universe g_u;
static pset g_requests, g_scripts;   /* every request of the universe; the scripts among them */

static void u_lower(char *s) { for (; *s; s++) { if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a'); if (*s == '\\') *s = '/'; } }
static void u_add(const char *key, const char *request)
{
    char *k, pair[1300];
    static pset seen;
    if (!key || !key[0] || !request || strlen(key) + strlen(request) + 2 > sizeof pair) return;
    _snprintf_s(pair, sizeof pair, _TRUNCATE, "%s\t%s", key, request);
    u_lower(pair);
    if (!ps_add(&seen, pair)) return;
    if (g_u.n == g_u.cap) {
        size_t c = g_u.cap ? g_u.cap * 2 : 4096;
        uentry *g = (uentry *)realloc(g_u.v, c * sizeof *g);
        if (!g) return;
        g_u.v = g;
        g_u.cap = c;
    }
    k = _strdup(key);
    u_lower(k);
    g_u.v[g_u.n].key = k;
    g_u.v[g_u.n].request = _strdup(request);
    u_lower(g_u.v[g_u.n].request);
    ps_add(&g_requests, g_u.v[g_u.n].request);
    if (strstr(g_u.v[g_u.n].request, "/scripts/")) ps_add(&g_scripts, g_u.v[g_u.n].request);
    g_u.n++;
    g_u.sorted = 0;
}
/* A name without its extension(s) after the last '/': images and models are named across extensions. */
static void u_stem(const char *in, char *out, size_t cap)
{
    const char *slash = strrchr(in, '/'), *dot;
    strcpy_s(out, cap, in);
    dot = strchr(slash ? slash : in, '.');
    if (dot) out[dot - in] = 0;
}
static void u_add_file(const char *request, const char *pname)
{
    char stem[600];
    u_add(request, request);
    if (pname) u_add(pname, request);
    u_stem(request, stem, sizeof stem);
    if (strcmp(stem, request)) u_add(stem, request);
    /* a decl's request names its decl: generated/decls/<type>/<name>.decl is <name> to a text */
    if (!_strnicmp(request, "generated/decls/", 16)) {
        const char *t = strchr(request + 16, '/');
        if (t) { char n[600]; strcpy_s(n, sizeof n, t + 1); u_stem(n, n, sizeof n); u_add(n, request); }
    }
}
static int u_cmp(const void *a, const void *b) { return strcmp(((const uentry *)a)->key, ((const uentry *)b)->key); }
static void u_sort(void) { if (!g_u.sorted) { qsort(g_u.v, g_u.n, sizeof *g_u.v, u_cmp); g_u.sorted = 1; } }
/* Every request `name` reaches; `lua`: the name is a whole literal of a script (a word is a module only there). */
static void u_requests(const char *name, pset *out, int lua)
{
    uentry key;
    char low[600], stem[600];
    size_t lo, hi;
    int pass;
    if (!name || strlen(name) < 3 || strlen(name) >= sizeof low) return;
    strcpy_s(low, sizeof low, name);
    u_lower(low);
    u_sort();
    for (pass = 0; pass < 3; pass++) {
        if (pass == 1) { u_stem(low, stem, sizeof stem); if (!strcmp(stem, low)) continue; key.key = stem; }
        else if (pass == 2) {
            /* a name require takes: scripts/lib/<name>.lua (any namespace's), the one file require loads
             * (script_lib_base.c sb_require); an entry script in scripts/ is never required */
            size_t i, sl = strlen(low);
            if (!lua || !sh_mb_script_module_name(low, sl)) continue;
            for (i = 0; i < g_scripts.n; i++) {
                const char *r = g_scripts.v[i], *s = strstr(r, "/scripts/lib/");
                if (s && !strncmp(s + 13, low, sl) && !strcmp(s + 13 + sl, ".lua")) ps_add(out, r);
            }
            continue;
        } else key.key = low;
        lo = 0; hi = g_u.n;
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (strcmp(g_u.v[mid].key, key.key) < 0) lo = mid + 1; else hi = mid; }
        for (; lo < g_u.n && !strcmp(g_u.v[lo].key, key.key); lo++) ps_add(out, g_u.v[lo].request);
    }
    {
        /* A model name (renderModelInfo.model, a .lwo or .bmodel path) is asked for at its cooked request: the
         * engine's naming of models, as the builder records it (modbundle.h sh_mb_model_keys). */
        char *keys[2];
        int nk = sh_mb_model_keys(low, keys, 2), m;
        for (m = 0; m < nk; m++) {
            size_t j;
            if (!keys[m]) continue;
            u_lower(keys[m]);
            if (ps_has(&g_requests, keys[m])) ps_add(out, keys[m]);
            (void)j;
        }
        sh_mb_requests_free(keys, nk);
    }
}
static void u_free(void)
{
    size_t i;
    for (i = 0; i < g_u.n; i++) { free(g_u.v[i].key); free(g_u.v[i].request); }
    free(g_u.v);
    memset(&g_u, 0, sizeof g_u);
}

/* Every file of a build joins the universe; its DOOM-name files are the seeds every mode reads. */
static void u_from_build(const sh_mb_bundle *b)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        char low[600];
        u_add_file(f->request, f->pname);
        if (!f->request || strlen(f->request) >= sizeof low) continue;
        strcpy_s(low, sizeof low, f->request);
        u_lower(low);
        if (f->cls == SH_MB_CLASS_SURFACE) { ps_add(&g_surfaces, low); continue; }
        if (f->edit || f->edited) ps_add(&g_registries, low);
        if (!strstr(low, "/mods/") && strncmp(low, "mods/", 5)) ps_add(&g_doom_seeds, low);
    }
}
static void u_served(void *ctx, const char *request, uint64_t size, const char *bundle_id, const char *bundle_hash)
{
    (void)ctx; (void)size; (void)bundle_id; (void)bundle_hash;
    u_add_file(request, NULL);
}

/* tokens a file's bytes spell */
static int tok_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/' ||
           c == '.' || c == '-' || c == '$' || c == '\\';
}
static void tokens(const unsigned char *p, size_t n, pset *names)
{
    size_t i = 0;
    while (i < n) {
        size_t s;
        while (i < n && !tok_char(p[i])) i++;
        s = i;
        while (i < n && tok_char(p[i])) i++;
        if (i - s >= 3 && i - s < 500) {
            char t[512];
            memcpy(t, p + s, i - s);
            t[i - s] = 0;
            u_lower(t);
            while (strlen(t) && (t[strlen(t) - 1] == '.' || t[strlen(t) - 1] == '/')) t[strlen(t) - 1] = 0;
            if (strlen(t) >= 3) ps_add(names, t);
        }
    }
}

/* What a script spells, as its runtime reads it (sh_mb_script_literals): the tokens of its string literals in `names`
 * (a comment is never read), and in `modules` each literal that is a name require takes. */
typedef struct script_spell { pset *names, *modules; } script_spell;
static int script_literal(void *ctx, const char *s, size_t n)
{
    script_spell *sp = (script_spell *)ctx;
    tokens((const unsigned char *)s, n, sp->names);
    if (sp->modules && n < 500 && sh_mb_script_module_name(s, n)) {
        char t[512];
        memcpy(t, s, n);
        t[n] = 0;
        u_lower(t);
        ps_add(sp->modules, t);
    }
    return 1;
}
static void script_names(const unsigned char *p, size_t n, pset *names, pset *modules)
{
    script_spell sp;
    sp.names = names;
    sp.modules = modules;
    (void)sh_mb_script_literals((const char *)p, n, script_literal, &sp);
}

/* one mode's answers */
typedef struct answer { char *request; char sha[SH_MB_HASH_HEX + 1]; } answer;   /* sha "" = nothing served */
typedef struct mode_run {
    const char *name;
    answer *a;
    size_t n, cap;
    pset reached;
    pset hidden_wrong;    /* entityDefs a Play mode does not declare that the level would still template */
    pset black;           /* "<material> -> <image>": an image a served material draws with that nothing answers */
    size_t derived;       /* the images the product derives (a look's) that this mode's materials drew with, served */
} mode_run;

static const char *serve_sha(const char *request, char sha[SH_MB_HASH_HEX + 1], unsigned char **bytes_out, size_t *len_out)
{
    unsigned char *bytes = NULL;
    size_t len = 0;
    sh_modserve_hit hit;
    sha[0] = 0;
    if (bytes_out) *bytes_out = NULL;
    if (!sh_modserve_lookup(request, &hit)) return sha;
    if (sh_modserve_open(request, &bytes, &len) != 1 || !bytes) { strcpy_s(sha, SH_MB_HASH_HEX + 1, "unreadable"); return sha; }
    sh_mb_sha256_hex(bytes, len, sha);
    if (bytes_out) { *bytes_out = bytes; *len_out = len; }
    else HeapFree(GetProcessHeap(), 0, bytes);
    return sha;
}

/* Open a served file once, as the engine reads it. */
static void open_once(void *ctx, const char *request, uint64_t size, const char *bundle_id, const char *bundle_hash)
{
    unsigned char *bytes = NULL;
    size_t len = 0;
    (void)ctx; (void)size; (void)bundle_id; (void)bundle_hash;
    if (sh_modserve_open(request, &bytes, &len) == 1 && bytes) HeapFree(GetProcessHeap(), 0, bytes);
}

/* What the engine reads of a declared map before anything else asks (a load's registration pass): every served file and
 * every DOOM file the mods change (the registries are generated when first read). */
static void warm(void)
{
    size_t i;
    sh_modserve_each_served_here(open_once, NULL);
    for (i = 0; i < g_doom_seeds.n; i++) open_once(NULL, g_doom_seeds.v[i], 0, NULL, NULL);
}

/* A served file at one of DOOM's own names (not under a mod's namespace). */
static void shadow_seed(void *ctx, const char *request, uint64_t size, const char *bundle_id, const char *bundle_hash)
{
    char low[600];
    (void)size; (void)bundle_id; (void)bundle_hash;
    if (!request || strlen(request) >= sizeof low) return;
    strcpy_s(low, sizeof low, request);
    u_lower(low);
    if (strstr(low, "/mods/") || !strncmp(low, "mods/", 5) || ps_has(&g_surfaces, low)) return;
    ps_add((pset *)ctx, low);
}

static void print_line(void *ctx, const char *line) { (void)ctx; printf("     %s\n", line); }

/* What a served material draws with: every image it names must be answered in the mode that serves the material,
 * by a file the mode serves (including images the product derives, which no build lists) or the game's own. The
 * mode comparison cannot see a hole every mode shares, and a name no build lists never joins the universe. */
typedef struct draw_ctx { pset *images; } draw_ctx;
static void draw_need(void *ctx, const char *type, const char *name, size_t n)
{
    char t[600];
    if (!type || _stricmp(type, "image") || !n || n >= sizeof t) return;
    memcpy(t, name, n);
    t[n] = 0;
    ps_add(((draw_ctx *)ctx)->images, t);
}

/* 1 when the mode answers image `name`: a request it serves; else the game's own (outside every mod's namespace, not a
 * name the product derives, and, with the installed game, a file it holds or an engine image). */
static int draw_answered(const char *name, int *derived)
{
    char *cand[SH_MB_NEED_REQUESTS];
    int n = sh_mb_need_requests("image", name, cand, SH_MB_NEED_REQUESTS), i, ok = 0, ours = 0;
    *derived = 0;
    if (name[0] == '_') { sh_mb_requests_free(cand, n); return 1; }   /* _black, _white, _default: the engine's */
    for (i = 0; i < n && !ok; i++) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        if (sh_mb_product_hashed(cand[i])) *derived = ours = 1;
        if (strstr(cand[i], "mods/")) ours = 1;
        if (sh_modserve_open(cand[i], &bytes, &len) == 1 && bytes) { HeapFree(GetProcessHeap(), 0, bytes); ok = 1; }
    }
    if (!ok && !ours) {
        if (!g_doom) ok = 1;   /* the fixtures: every other name is DOOM's */
        else
            for (i = 0; i < n && !ok; i++)
                ok = !sh_modprog_owns(cand[i]) && sh_modstock_exists(cand[i]) == 1;
    }
    sh_mb_requests_free(cand, n);
    return ok;
}

static void mode_draws(mode_run *m)
{
    size_t i, k;
    for (i = 0; i < m->reached.n; i++) {
        const char *r = m->reached.v[i];
        unsigned char *bytes = NULL;
        size_t len = 0;
        pset images;
        draw_ctx c;
        if (strncmp(r, "generated/decls/material/", 25) && strncmp(r, "generated/decls/material2/", 26)) continue;
        if (sh_modserve_open(r, &bytes, &len) != 1 || !bytes) continue;
        memset(&images, 0, sizeof images);
        c.images = &images;
        sh_mb_scan_needs("material", bytes, len, draw_need, &c);
        HeapFree(GetProcessHeap(), 0, bytes);
        for (k = 0; k < images.n; k++) {
            int derived = 0;
            if (draw_answered(images.v[k], &derived)) { if (derived) m->derived++; continue; }
            {
                char line[1300];
                _snprintf_s(line, sizeof line, _TRUNCATE, "%s -> %s", r, images.v[k]);
                ps_add(&m->black, line);
            }
        }
        ps_free(&images);
    }
}

/* The reference closure from the placed names under the scope in force. */
static void mode_reach(mode_run *m, const char *const *placed, size_t np)
{
    pset queue, names, modules;
    size_t q = 0, i;
    memset(&queue, 0, sizeof queue);
    memset(&names, 0, sizeof names);
    memset(&modules, 0, sizeof modules);
    for (i = 0; i < np; i++) u_requests(placed[i], &queue, 0);
    if (getenv("PARITY_REG")) { printf("  -- %s registries:\n", m->name); sh_modserve_registry_report(print_line, NULL); sh_modserve_scope_report(print_line, NULL); }
    /* Every DOOM file the map's mods change (a native/ replacement, an edited decl, a registry the product generates:
     * the conductor that pairs an encounter's AI type with its entityDef) is read by the engine whatever the map
     * places. */
    sh_modserve_each_served_here(shadow_seed, &queue);
    for (i = 0; i < g_doom_seeds.n; i++) ps_add(&queue, g_doom_seeds.v[i]);
    for (;;) {
        size_t before;
        while (q < queue.n) {
            char sha[SH_MB_HASH_HEX + 1];
            unsigned char *bytes = NULL;
            size_t len = 0, k;
            const char *r = queue.v[q++];
            int lua = strstr(r, "/scripts/") && strlen(r) > 4 && !strcmp(r + strlen(r) - 4, ".lua");
            ps_add(&m->reached, r);
            serve_sha(r, sha, &bytes, &len);
            if (!bytes) continue;
            ps_free(&names);
            ps_free(&modules);
            if (lua) script_names(bytes, len, &names, &modules);
            else tokens(bytes, len, &names);
            HeapFree(GetProcessHeap(), 0, bytes);
            for (k = 0; k < names.n; k++) {
                if (getenv("PARITY_TOKENS") && strstr(names.v[k], "mods/")) printf("  %s: %s spells %s\n", m->name, r, names.v[k]);
                u_requests(names.v[k], &queue, 0);
            }
            for (k = 0; k < modules.n; k++) u_requests(modules.v[k], &queue, 1);
            ps_free(&names);
            ps_free(&modules);
        }
        /* A script is run for an entity it binds: the runtime starts every served entry script, and the one whose
         * literals name a reached entityDef (its bind) acts for it; what that script requires is then reached through
         * its names. */
        before = queue.n;
        for (i = 0; i < g_scripts.n; i++) {
            const char *r = g_scripts.v[i];
            unsigned char *bytes = NULL;
            size_t len = 0, k;
            char sha[SH_MB_HASH_HEX + 1];
            int binds = 0;
            /* only an entry script runs on its own; a scripts/lib file runs when a script requires it */
            if (ps_has(&queue, r) || sh_mb_script_request(r, NULL, NULL) != SH_MB_SCRIPT_ENTRY) continue;
            serve_sha(r, sha, &bytes, &len);
            if (!bytes) continue;
            ps_free(&names);
            script_names(bytes, len, &names, NULL);
            HeapFree(GetProcessHeap(), 0, bytes);
            for (k = 0; k < names.n; k++) {
                char req[700];
                if (!binds) {
                    const char *ns = strstr(r, "mods/");
                    _snprintf_s(req, sizeof req, _TRUNCATE, "generated/decls/entitydef/%s.decl", names.v[k]);
                    binds = ps_has(&m->reached, req);
                    /* a script names its mod's defs by their author names (ai/demon/tyrant is mods/<salt>/ai/demon/tyrant) */
                    if (!binds && ns && strlen(ns) > 14) {
                        _snprintf_s(req, sizeof req, _TRUNCATE, "generated/decls/entitydef/%.14s%s.decl", ns, names.v[k]);
                        binds = ps_has(&m->reached, req);
                    }
                }
            }
            ps_free(&names);
            if (binds) ps_add(&queue, r);
        }
        if (queue.n == before) break;
    }
    ps_free(&queue);
    ps_free(&names);
    ps_free(&modules);
    mode_draws(m);
}

static void mode_answer(mode_run *m, const char *request)
{
    if (m->n == m->cap) {
        size_t c = m->cap ? m->cap * 2 : 1024;
        answer *g = (answer *)realloc(m->a, c * sizeof *g);
        if (!g) return;
        m->a = g;
        m->cap = c;
    }
    m->a[m->n].request = _strdup(request);
    serve_sha(request, m->a[m->n].sha, NULL, NULL);
    m->n++;
}

/* a world: a mods root, built and indexed */
typedef struct world {
    char root[MAX_PATH * 2];
    sh_mb_bundle *builds;
    char (*folders)[MAX_PATH * 2];
    size_t nb;
} world;

/* The fixtures' folders and their namespaces (a real mod set keeps the salt its folder records). */
static const char *g_fixture_salts[16][2];
static const char *fixture_salt(const char *folder)
{
    int i;
    for (i = 0; i < 16 && g_fixture_salts[i][0]; i++) if (!_stricmp(g_fixture_salts[i][0], folder)) return g_fixture_salts[i][1];
    return NULL;
}

static int world_build(world *w, int build_folders)
{
    char pattern[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    w->nb = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", w->root);
    h = FindFirstFileA(pattern, &fd);
    while (build_folders && h != INVALID_HANDLE_VALUE) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && _stricmp(fd.cFileName, "maps")) {
            char folder[MAX_PATH * 2], *cause = NULL;
            sh_mb_bundle b;
            _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\%s", w->root, fd.cFileName);
            if (!sh_mb_build(folder, fixture_salt(fd.cFileName), &g_resolver, &b) || b.errors) {
                fprintf(stderr, "parity: %s does not build: %s\n", fd.cFileName, sh_mb_first_cause(&b) ? sh_mb_first_cause(&b) : "?");
                failures++;
                sh_mb_free(&b);
            } else if (!sh_mb_write(&b, folder, &cause)) {
                fprintf(stderr, "parity: %s not written: %s\n", fd.cFileName, cause ? cause : "?");
                failures++;
                sh_mb_free(&b);
            } else {
                sh_mb_bundle *g = (sh_mb_bundle *)realloc(w->builds, (w->nb + 1) * sizeof *g);
                char (*f)[MAX_PATH * 2] = realloc(w->folders, (w->nb + 1) * sizeof *f);
                if (g) w->builds = g;
                if (f) w->folders = f;
                if (g && f) { w->builds[w->nb] = b; strcpy_s(w->folders[w->nb], sizeof w->folders[w->nb], folder); w->nb++; }
            }
            free(cause);
        }
        if (!FindNextFileA(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    return 1;
}

static int world_index(world *w)
{
    sh_modprog_reset();
    sh_modserve_set_material_filter(sh_look_compile);
    sh_cache_set_mods_root(w->root);
    sh_cache_local_changed();
    if (sh_modserve_index_build(w->root) < 0) { fprintf(stderr, "parity: the index of %s was refused\n", w->root); failures++; return 0; }
    /* every file of the world joins the universe, at its served name */
    sh_modserve_scope_enable(0);
    sh_modserve_each_served(u_served, NULL);
    sh_modserve_scope_enable(1);
    return 1;
}

static void world_free(world *w)
{
    size_t i;
    for (i = 0; i < w->nb; i++) sh_mb_free(&w->builds[i]);
    free(w->builds);
    free(w->folders);
    memset(w, 0, sizeof *w);
}

/* The folder serving a local row in the author's world (modpin's folder, as the product resolves it). */
static world *g_author;
static char *author_folder(void *ctx, const sh_modmanifest_row *row)
{
    size_t i;
    (void)ctx;
    for (i = 0; g_author && i < g_author->nb; i++)
        if (!_stricmp(g_author->builds[i].id, row->salt)) return _strdup(g_author->folders[i]);
    return NULL;
}
static char *author_read(void *ctx, const char *request, size_t *len)
{
    unsigned char *bytes = NULL;
    size_t n = 0;
    char *out;
    (void)ctx;
    if (sh_modserve_open(request, &bytes, &n) != 1 || !bytes) return NULL;
    out = (char *)malloc(n + 1);
    if (out) { memcpy(out, bytes, n); out[n] = 0; *len = n; }
    HeapFree(GetProcessHeap(), 0, bytes);
    return out;
}
static const char *g_attach;   /* attach=: the salts used in the map as a whole */
static size_t g_last_q6;       /* the last run's divergences of own play's whole build (Q6) */
static size_t g_last_derived[7];   /* the last run's derived images drawn and served, per mode */

/* The save's row resolver (what modui.c binds: the build sh_modserve_row_build names). */
static int resolve_row(const char *build, const char *salt, sh_modmanifest_row *out)
{
    sh_modserve_bundle_ref ref;
    sh_modmanifest_row probe;
    int ok;
    memset(&probe, 0, sizeof probe);
    memset(&ref, 0, sizeof ref);
    strcpy_s(probe.salt, sizeof probe.salt, salt);
    strcpy_s(probe.build, sizeof probe.build, build);
    ok = sh_modserve_row_build(&probe, &ref) && strlen(ref.id) < sizeof out->salt && strlen(ref.hash) < sizeof out->build;
    if (ok) {
        strcpy_s(out->salt, sizeof out->salt, ref.id);
        strcpy_s(out->build, sizeof out->build, ref.hash);
        ok = sh_modmanifest_row_set_name(out, ref.name);
        out->size = ref.size;
    }
    sh_modserve_bundle_ref_free(&ref);
    return ok;
}

/* The listing a mode declares (the product's one rule), set with the mode. */
static void declare(const sh_modmanifest_list *rows, const char *const *placed, size_t np, int mode)
{
    char (*hashes)[SH_MODMANIFEST_HASH_CAP] = NULL;
    const char *list[256];
    int n = sh_modmanifest_listing(rows, placed, np, &hashes), i, nc = 0;
    char *choices = NULL, *p;
    const char *slots[256], *keys[256];
    for (i = 0; i < n && i < 256; i++) list[i] = hashes[i];
    /* The map's choices first (its types pinned on its rows), as every load gives them before the listing. */
    if (rows) (void)sh_modmanifest_rows_choices(rows, &choices);
    for (p = choices; p && *p && nc < 256;) {
        char *tab = strchr(p, '\t'), *end = strchr(p, '\n');
        if (!tab || !end || tab > end) break;
        *tab = 0;
        *end = 0;
        slots[nc] = p;
        keys[nc++] = tab + 1;
        p = end + 1;
    }
    (void)sh_modserve_scope_set_choices(slots, keys, nc, 0);
    (void)sh_modserve_scope_set(list, n > 256 ? 256 : (n < 0 ? 0 : n), 1);
    (void)sh_modserve_scope_mode(mode);
    free(hashes);
    free(choices);
}

/* Entities the level would template: every entityDef of a namespace this mode does not declare must be hidden. A DOOM
 * entityDef a mod replaces is DOOM's own name, never hidden: that the mod's text is not served for it where the mod is
 * not declared is the files' comparison. */
static void check_hidden(mode_run *m, const world *w, const sh_modmanifest_list *rows)
{
    size_t i, k, r;
    for (i = 0; i < w->nb; i++) {
        int declared = 0;
        size_t il = strlen(w->builds[i].id);
        for (r = 0; rows && r < rows->count; r++) declared |= !_stricmp(rows->rows[r].salt, w->builds[i].id);
        if (declared) continue;
        for (k = 0; k < w->builds[i].file_count; k++) {
            const sh_mb_file *f = &w->builds[i].files[k];
            if (!f->pname || !f->ptype || _stricmp(f->ptype, "entityDef")) continue;
            if (_strnicmp(f->pname, "mods/", 5) || _strnicmp(f->pname + 5, w->builds[i].id, il) || f->pname[5 + il] != '/')
                continue;
            if (!sh_modserve_level_hides(f->pname)) ps_add(&m->hidden_wrong, f->pname);
        }
    }
}

/* the parity run of one map */
#define NMODES 7
typedef struct parity {
    mode_run m[NMODES];
    pset all;
    size_t divergences;
    size_t q6;            /* divergences of own play's whole build (Q6), counted apart */
    FILE *report;
} parity;

static void say(parity *p, const char *fmt, const char *a, const char *b, const char *c)
{
    char line[4096];
    _snprintf_s(line, sizeof line, _TRUNCATE, fmt, a ? a : "", b ? b : "", c ? c : "");
    printf("%s\n", line);
    if (p->report) fprintf(p->report, "%s\n", line);
}

/* A known divergence: the mode pair and request part it names, and the piece that removes it. */
typedef struct known { const char *mode; const char *part; const char *why; int met; } known;

/* Run every mode for the map (`json`, its placed names) over the author's mods in `author_root` (built here), with a
 * scratch `player_root`. Returns the divergences that no `known` entry names. */
static size_t run_parity(const char *label, const char *author_root, const char *player_root, const char *json,
                         size_t json_len, known *kn, size_t nk, FILE *report)
{
    world A, B;
    parity p;
    char **placed = NULL;
    int failed = 0, i;
    size_t np, k, unexpected = 0, saved_len = 0;
    char *saved;
    sh_modmanifest_list rows, pinned;
    sh_modpin_result pr;
    memset(&p, 0, sizeof p);
    memset(&A, 0, sizeof A);
    memset(&B, 0, sizeof B);
    memset(&rows, 0, sizeof rows);
    memset(&pinned, 0, sizeof pinned);
    p.report = report;
    p.m[0].name = "editor"; p.m[1].name = "editor-play"; p.m[2].name = "launch"; p.m[3].name = "pub-author";
    p.m[4].name = "kept-author"; p.m[5].name = "pub-player"; p.m[6].name = "kept-player";
    say(&p, "parity: %s", label, NULL, NULL);

    strcpy_s(A.root, sizeof A.root, author_root);
    world_build(&A, 1);
    for (k = 0; k < A.nb; k++) u_from_build(&A.builds[k]);
    world_index(&A);
    g_author = &A;
    np = sh_modmanifest_placed_names(json, json_len, &placed, &failed);
    /* attach=: each mod used as a whole, as the Mods tab's Use in this map marks the open map */
    for (k = 0; g_attach && k < A.nb; k++) {
        const char *at = g_attach;
        size_t il = strlen(A.builds[k].id);
        for (; at && *at; at = strchr(at, ',') ? strchr(at, ',') + 1 : NULL) {
            sh_modmanifest_row row;
            if (_strnicmp(at, A.builds[k].id, il) || (at[il] && at[il] != ',')) continue;
            memset(&row, 0, sizeof row);
            strcpy_s(row.salt, sizeof row.salt, A.builds[k].id);
            strcpy_s(row.build, sizeof row.build, A.builds[k].hash);
            row.size = A.builds[k].total_size;
            CHECK(sh_modmanifest_current_attach(&row) == 1);
            say(&p, "  attached: %s", A.builds[k].id, NULL, NULL);
        }
    }

    /* The author saves the map: the save's rows (the product's own save step). */
    sh_modmanifest_set_resolver(resolve_row);
    declare(NULL, (const char *const *)placed, np, SH_MODSERVE_MODE_EDITOR);
    warm();   /* the editor has served the map (its registration pass reads every registry) before it saves */
    saved =sh_modmanifest_on_save(json, json_len, &saved_len);
    if (saved) {
        unsigned rej = 0;
        sh_modmanifest_read(saved, saved_len, &rows, &rej);
    } else {
        unsigned rej = 0;
        sh_modmanifest_read(json, json_len, &rows, &rej);
    }
    for (k = 0; k < rows.count; k++) {
        size_t vl = 0;
        char *v = sh_modmanifest_format_value(&rows.rows[k], &vl);
        say(&p, "  saved row: %.400s", v, NULL, NULL);
        free(v);
    }

    /* Each mode starts from a fresh process state (world_index): a mode met first in its own process must serve
     * what the map draws without the derived files of another mode. */
    world_index(&A);
    declare(&rows, (const char *const *)placed, np, SH_MODSERVE_MODE_EDITOR);
    mode_reach(&p.m[0], (const char *const *)placed, np);
    world_index(&A);
    declare(&rows, (const char *const *)placed, np, SH_MODSERVE_MODE_PLAY);
    mode_reach(&p.m[1], (const char *const *)placed, np);
    check_hidden(&p.m[1], &A, &rows);
    world_index(&A);
    declare(&rows, NULL, 0, SH_MODSERVE_MODE_PLAY);
    mode_reach(&p.m[2], (const char *const *)placed, np);
    check_hidden(&p.m[2], &A, &rows);

    /* The publish pins the rows (modpin) from the author's folders, and the author's machine composes them. */
    for (k = 0; k < rows.count; k++) sh_modmanifest_list_add(&pinned, &rows.rows[k]);
    declare(&rows, (const char *const *)placed, np, SH_MODSERVE_MODE_EDITOR);
    /* The editor has read what the map serves before its author publishes it (the product's derived files, a
     * material's parameter image, exist once their material was read). */
    warm();
    sh_modpin_set_folder(author_folder, NULL);
    sh_modpin_set_read(author_read, NULL);
    memset(&pr, 0, sizeof pr);
    {
        char content[MAX_PATH * 2];
        _snprintf_s(content, sizeof content, _TRUNCATE, "%s\\content", mt_cache_dir(author_root));
        CHECK(sh_modpin_rows(&pinned, saved ? saved : json, saved ? saved_len : json_len, content, &pr) >= 0);
    }
    for (k = 0; k < pr.count; k++) {
        char units[64];
        _snprintf_s(units, sizeof units, _TRUNCATE, "%zu unit(s), %llu bytes", pr.rows[k].units, pr.rows[k].bytes);
        say(&p, "  pin: row %s: %s, %s", pr.rows[k].salt,
            pr.rows[k].state == SH_MODPIN_PINNED ? "proved" : pr.rows[k].state == SH_MODPIN_WHOLE ? "WHOLE" : "ABSENT", units);
        if (pr.rows[k].state != SH_MODPIN_PINNED) say(&p, "  pin: row %s: %s", pr.rows[k].salt, pr.rows[k].why, NULL);
    }
    sh_cache_set_mods_root(author_root);
    sh_cache_on_load(&pinned, placed, np);
    world_index(&A);
    declare(&pinned, NULL, 0, SH_MODSERVE_MODE_PLAY);
    mode_reach(&p.m[3], (const char *const *)placed, np);
    check_hidden(&p.m[3], &A, &pinned);
    world_index(&A);
    declare(&pinned, (const char *const *)placed, np, SH_MODSERVE_MODE_EDITOR);
    mode_reach(&p.m[4], (const char *const *)placed, np);

    /* A player with none of the mods: the units' manifests and files arrive in an empty cache. */
    strcpy_s(B.root, sizeof B.root, player_root);
    CreateDirectoryA(B.root, NULL);
    sh_cache_set_mods_root(player_root);
    for (k = 0; k < pinned.count; k++) {
        size_t u;
        for (u = 0; u < pinned.rows[k].unit_count; u++) {
            char *manifest;
            size_t ml = 0;
            mpj *m, *files;
            int f;
            sh_cache_set_mods_root(author_root);
            manifest = sh_cache_manifest(pinned.rows[k].units[u], &ml);
            sh_cache_set_mods_root(player_root);
            if (!manifest) { say(&p, "  pin: a unit's manifest is not in the author's cache", NULL, NULL, NULL); failures++; continue; }
            CHECK(sh_cache_put_manifest(pinned.rows[k].units[u], manifest, ml));
            m = mpj_parse(manifest, ml);
            files = m ? (mpj *)mpj_get(m, "files") : NULL;
            for (f = 0; files && files->type == MPJ_ARR && f < files->n; f++) {
                const mpj *sha = mpj_get(files->items[f], "sha256");
                size_t b;
                for (b = 0; sha && sha->type == MPJ_STR && b < A.nb; b++) {
                    size_t fi;
                    for (fi = 0; fi < A.builds[b].file_count; fi++) {
                        const sh_mb_file *bf = &A.builds[b].files[fi];
                        char tmp[MAX_PATH * 2];
                        wchar_t *folder, *from, *to;
                        if (bf->stock || bf->absent || strcmp(bf->blob, sha->text)) continue;
                        folder = sh_mb_wpath(A.folders[b]);
                        from = folder ? sh_mb_payload_wpath(folder, bf->path) : NULL;
                        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s\\arrived.tmp", player_root);
                        to = sh_mb_wpath(tmp);
                        CHECK(from && to && CopyFileW(from, to, FALSE) && sh_cache_put_blob(tmp, bf->blob, bf->size));
                        free(folder); free(from); free(to);
                        b = A.nb;
                        break;
                    }
                }
            }
            mpj_free(m);
            free(manifest);
        }
    }
    /* the player's process has derived nothing and serves none of the map's mods when the map's units are composed */
    world_index(&B);
    sh_cache_on_load(&pinned, placed, np);
    world_index(&B);
    declare(&pinned, NULL, 0, SH_MODSERVE_MODE_PLAY);
    mode_reach(&p.m[5], (const char *const *)placed, np);
    check_hidden(&p.m[5], &B, &pinned);
    world_index(&B);
    declare(&pinned, (const char *const *)placed, np, SH_MODSERVE_MODE_EDITOR);
    mode_reach(&p.m[6], (const char *const *)placed, np);

    /* Every file any mode reached, answered by every mode. */
    for (i = 0; i < NMODES; i++) for (k = 0; k < p.m[i].reached.n; k++) ps_add(&p.all, p.m[i].reached.v[k]);
    if (getenv("PARITY_DEBUG")) {
        for (k = 0; k < g_u.n; k++) printf("  universe: %s -> %s\n", g_u.v[k].key, g_u.v[k].request);
        for (i = 0; i < NMODES; i++) for (k = 0; k < p.m[i].reached.n; k++) printf("  %s reached %s\n", p.m[i].name, p.m[i].reached.v[k]);
    }
    {
        static const int order[NMODES] = { 0, 1, 2, 3, 4, 5, 6 };
        for (i = 0; i < NMODES; i++) {
            /* put each mode back, then answer */
            switch (order[i]) {
            /* each from a fresh process state, as in the reach */
            case 0: world_index(&A); declare(&rows, (const char *const *)placed, np, SH_MODSERVE_MODE_EDITOR); break;
            case 1: world_index(&A); declare(&rows, (const char *const *)placed, np, SH_MODSERVE_MODE_PLAY); break;
            case 2: world_index(&A); declare(&rows, NULL, 0, SH_MODSERVE_MODE_PLAY); break;
            case 3: world_index(&A); declare(&pinned, NULL, 0, SH_MODSERVE_MODE_PLAY); break;
            case 4: world_index(&A); declare(&pinned, (const char *const *)placed, np, SH_MODSERVE_MODE_EDITOR); break;
            case 5: sh_cache_set_mods_root(player_root); world_index(&B); declare(&pinned, NULL, 0, SH_MODSERVE_MODE_PLAY); break;
            case 6: world_index(&B); declare(&pinned, (const char *const *)placed, np, SH_MODSERVE_MODE_EDITOR); break;
            }
            /* as the engine reads: every file once (the product's derived files exist once their material was read),
             * then the answers */
            for (k = 0; k < p.all.n; k++) open_once(NULL, p.all.v[k], 0, NULL, NULL);
            for (k = 0; k < p.all.n; k++) {
                mode_answer(&p.m[order[i]], p.all.v[k]);
                if (getenv("PARITY_DUMP") && strstr(p.all.v[k], getenv("PARITY_DUMP"))) {
                    unsigned char *bytes = NULL;
                    size_t len = 0;
                    char path[MAX_PATH * 2];
                    FILE *f = NULL;
                    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\..\\dump-%s.txt", author_root, p.m[order[i]].name);
                    if (sh_modserve_open(p.all.v[k], &bytes, &len) == 1 && bytes && !fopen_s(&f, path, "wb") && f) {
                        fwrite(bytes, 1, len, f);
                        fclose(f);
                    }
                    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
                }
            }
        }
    }
    for (k = 0; k < p.all.n; k++) {
        const char *ref = p.m[0].a[k].sha;
        int anywhere = 0, q6 = 1, unit_reach = 0;
        for (i = 0; i < NMODES; i++) anywhere |= p.m[i].a[k].sha[0] != 0;
        /* Q6: own play serves the whole local build (editor, editor-play, launch), a published map its units
         * (pub-author, pub-player, kept-player). A file whose answer splits along that line, being a DOOM decl the
         * mod edits or one no unit mode reaches, is that divergence and not a unit defect. */
        for (i = 1; i < 3; i++) q6 &= !strcmp(p.m[i].a[k].sha, ref);
        for (i = 5; i < NMODES; i++) q6 &= !strcmp(p.m[i].a[k].sha, p.m[3].a[k].sha);
        q6 &= strcmp(p.m[3].a[k].sha, ref) != 0;
        /* kept-author is the published map in its author's editor: the local mod fills the names the units lack,
         * so it answers as either side */
        q6 &= !strcmp(p.m[4].a[k].sha, ref) || !strcmp(p.m[4].a[k].sha, p.m[3].a[k].sha);
        for (i = 3; i < NMODES; i++) if (i != 4) unit_reach |= ps_has(&p.m[i].reached, p.all.v[k]);
        q6 &= ps_has(&g_registries, p.all.v[k]) || !unit_reach;
        /* A name the bytes spell that nothing serves in any mode is the same in every mode (the engine meets the
         * game's file or none): not a divergence, listed for the reader. */
        if (!anywhere && getenv("PARITY_DEBUG")) say(&p, "  nowhere: %s", p.all.v[k], NULL, NULL);
        if (sh_mb_editor_time(p.all.v[k])) {
            /* The editor's own surface (its pickers, inspectors and palette): only the editor reads it, and what it
             * offers follows what is installed (PLATFORM: installed mods grow SnapMap's lists). Not a placed object's data. */
            continue;
        }
        for (i = 1; i < NMODES; i++) {
            size_t j;
            int listed = 0;
            if (!strcmp(p.m[i].a[k].sha, ref)) continue;
            for (j = 0; j < nk; j++)
                if (!strcmp(kn[j].mode, p.m[i].name) && strstr(p.all.v[k], kn[j].part)) { kn[j].met = 1; listed = 1; }
            p.divergences++;
            if (q6 && !listed) { listed = 2; p.q6++; }
            if (!listed) unexpected++;
            {
                char line[1024];
                _snprintf_s(line, sizeof line, _TRUNCATE, "  %s: %s: editor %.12s, %s %.12s",
                            listed == 2 ? "Q6" : listed ? "known" : "DIVERGES",
                            p.all.v[k], ref[0] ? ref : "(nothing)", p.m[i].name, p.m[i].a[k].sha[0] ? p.m[i].a[k].sha : "(nothing)");
                say(&p, "%s", line, NULL, NULL);
            }
        }
    }
    g_last_q6 = p.q6;
    for (i = 0; i < NMODES; i++)
        for (k = 0; k < p.m[i].hidden_wrong.n; k++) {
            unexpected++;
            say(&p, "  DIVERGES: %s: %s is not hidden from the level's templates", p.m[i].name, p.m[i].hidden_wrong.v[k], NULL);
        }
    /* every mode alike or not, a material drawing with an image nothing answers draws black */
    for (i = 0; i < NMODES; i++) {
        g_last_derived[i] = p.m[i].derived;
        for (k = 0; k < p.m[i].black.n; k++) {
            unexpected++;
            say(&p, "  DRAWS BLACK: %s: %s, which nothing serves", p.m[i].name, p.m[i].black.v[k], NULL);
        }
    }
    {
        char line[256];
        _snprintf_s(line, sizeof line, _TRUNCATE, "  %zu file(s) reached; %zu divergence(s), %zu not known; %zu of own play's whole build (Q6)",
                    p.all.n, p.divergences, unexpected, p.q6);
        say(&p, "%s", line, NULL, NULL);
    }
    ps_free(&g_last_served);
    for (k = 0; k < p.all.n; k++) if (p.m[0].a[k].sha[0]) ps_add(&g_last_served, p.all.v[k]);
    for (i = 0; i < NMODES; i++) {
        for (k = 0; k < p.m[i].n; k++) free(p.m[i].a[k].request);
        free(p.m[i].a);
        ps_free(&p.m[i].reached);
        ps_free(&p.m[i].hidden_wrong);
        ps_free(&p.m[i].black);
    }
    ps_free(&p.all);
    sh_modpin_result_free(&pr);
    sh_modmanifest_list_free(&rows);
    sh_modmanifest_list_free(&pinned);
    sh_modmanifest_names_free(placed, np);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    g_author = NULL;
    world_free(&A);
    world_free(&B);
    return unexpected;
}

/* fixtures */
#define SA "pa1salt1"
#define SB "pb2salt2"
#define SC "pc3salt3"

static void write_mod(const char *root, const char *folder, const char *salt, const char *name)
{
    char dir[MAX_PATH * 2], text[256];
    int i;
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, folder);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\n  \"name\": \"%s\"\n}\n", name);
    mt_write(dir, "mod.json", text);
    for (i = 0; i < 15 && g_fixture_salts[i][0] && _stricmp(g_fixture_salts[i][0], folder); i++) {}
    g_fixture_salts[i][0] = folder;
    g_fixture_salts[i][1] = salt;
}
static void put_bin(const char *root, const char *folder, const char *rel, const char *spell, unsigned seed)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    int i;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\%s", root, folder, rel);
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") || !f) return;
    for (i = 0; i < 300; i++) fputc((i * 7 + (int)seed) % 251, f);
    if (spell) { fputc(0, f); fwrite(spell, 1, strlen(spell), f); fputc(0, f); }
    for (i = 0; i < 32; i++) fputc(0, f);
    fclose(f);
}
static void put_text(const char *root, const char *folder, const char *rel, const char *text)
{
    char dir[MAX_PATH * 2];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, folder);
    mt_write(dir, rel, text);
}

/* A crate prop of mod A: entity, model, material, image. */
static void crate(const char *root, const char *folder, const char *salt, const char *n, unsigned seed)
{
    char rel[MAX_PATH], text[1024], spell[160];
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "entities\\%s.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t\trenderModelInfo = {\n"
                "\t\t\tmodel = \"mods/%s/models/%s.lwo\";\n\t\t}\n\t}\n}\n", salt, n);
    put_text(root, folder, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "meshes\\models\\%s.bmodel", n);
    _snprintf_s(spell, sizeof spell, _TRUNCATE, "mods/%s/%s_skin", salt, n);
    put_bin(root, folder, rel, spell, seed);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "materials\\%s_skin.decl", n);
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\ndiffusemap\t\"mods/%s/%s_d.tga\"\n}\n", salt, n);
    put_text(root, folder, rel, text);
    _snprintf_s(rel, sizeof rel, _TRUNCATE, "images\\%s_d.bimage", n);
    put_bin(root, folder, rel, NULL, seed + 3);
}

static const char *map_of(const char *salt, const char *const *names, size_t n, char *buf, size_t cap)
{
    size_t i;
    _snprintf_s(buf, cap, _TRUNCATE, "{\"entities\":[");
    for (i = 0; i < n; i++) {
        char e[256];
        _snprintf_s(e, sizeof e, _TRUNCATE, "%s{\"inherit\":\"mods/%s/%s\",\"edit\":{\"spawnPosition\":{\"x\":%zu}}}",
                    i ? "," : "", salt, names[i], i * 64);
        strcat_s(buf, cap, e);
    }
    strcat_s(buf, cap, "],\"instances\":[]}");
    return buf;
}

static void case_one_mod(void)
{
    char work[MAX_PATH], author[MAX_PATH * 2], player[MAX_PATH * 2], json[4096];
    const char *names[] = { "crate", "barrel" };
    CHECK(mt_temp_dir("parity1", work, sizeof work));
    _snprintf_s(author, sizeof author, _TRUNCATE, "%s\\author", work);
    _snprintf_s(player, sizeof player, _TRUNCATE, "%s\\player", work);
    CreateDirectoryA(author, NULL);
    write_mod(author, "crates", SA, "Crates");
    crate(author, "crates", SA, "crate", 1);
    crate(author, "crates", SA, "barrel", 9);
    crate(author, "crates", SA, "lamp", 17);   /* not placed: in no unit, served only to the editor's Toybox */
    map_of(SA, names, 2, json, sizeof json);
    CHECK(run_parity("one mod, two placed props of three", author, player, json, strlen(json), NULL, 0, NULL) == 0);
    mt_remove_tree(work);
}

/* Two mods placed in one map: two rows, each composed on its own; a third installed mod the map does not use stays
 * out of every Play (its entities hidden from the level's templates). */
static void case_two_mods(void)
{
    char work[MAX_PATH], author[MAX_PATH * 2], player[MAX_PATH * 2], json[4096];
    CHECK(mt_temp_dir("parity2", work, sizeof work));
    _snprintf_s(author, sizeof author, _TRUNCATE, "%s\\author", work);
    _snprintf_s(player, sizeof player, _TRUNCATE, "%s\\player", work);
    CreateDirectoryA(author, NULL);
    write_mod(author, "crates", SA, "Crates");
    write_mod(author, "paints", SB, "Paints");
    write_mod(author, "others", SC, "Others");
    crate(author, "crates", SA, "crate", 1);
    crate(author, "paints", SB, "red", 4);
    crate(author, "others", SC, "stool", 7);
    _snprintf_s(json, sizeof json, _TRUNCATE, "{\"entities\":[{\"inherit\":\"mods/" SA "/crate\"},"
                "{\"inherit\":\"mods/" SB "/red\"}],\"instances\":[]}");
    CHECK(run_parity("two mods placed in one map, a third installed", author, player, json, strlen(json), NULL, 0, NULL) == 0);
    CHECK(ps_has(&g_last_served, "generated/image/mods/" SA "/crate_d.bimage"));
    CHECK(ps_has(&g_last_served, "generated/image/mods/" SB "/red_d.bimage"));
    mt_remove_tree(work);
}

/* A demon's head drawn with a layered look (the Graphics SDK's `type layered`): the served material names the
 * look's parameter and colour images, which the product derives and no build lists. Every mode, each from a fresh
 * process, must serve them. */
static void case_look(void)
{
    char work[MAX_PATH], author[MAX_PATH * 2], player[MAX_PATH * 2], json[4096];
    const char *names[] = { "head" };
    int i;
    CHECK(mt_temp_dir("parityl", work, sizeof work));
    _snprintf_s(author, sizeof author, _TRUNCATE, "%s\\author", work);
    _snprintf_s(player, sizeof player, _TRUNCATE, "%s\\player", work);
    CreateDirectoryA(author, NULL);
    write_mod(author, "heads", SA, "Heads");
    crate(author, "heads", SA, "head", 3);
    put_text(author, "heads", "materials\\head_skin.decl",
             "{\ntype\tlayered\ndiffusemap\t\"mods/" SA "/head_d.tga\"\nlayermask\t\"mods/" SA "/head_mask.tga\"\n"
             "layer1\t\"mods/" SA "/pores.tga\"\nlayer1repeat\t20\nlayer1weights\t{ -0.1, 0.3, 0, 0, 1 }\n}\n");
    put_bin(author, "heads", "images\\head_mask.bimage", NULL, 5);
    put_bin(author, "heads", "images\\pores.bimage", NULL, 7);
    map_of(SA, names, 1, json, sizeof json);
    CHECK(run_parity("a head drawn with a layered look", author, player, json, strlen(json), NULL, 0, NULL) == 0);
    for (i = 0; i < NMODES; i++) CHECK(g_last_derived[i] > 0);   /* the look's images were drawn with, and served */
    mt_remove_tree(work);
}

/* The fixture DOOM: the files below are DOOM's with these bytes (the builder identifies a native/ copy against them and
 * derives a decl copy's edits from them); every other name outside mods/ is DOOM's with no bytes to read. */
#define K_CONDUCTOR "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl"
#define K_PLAYER    "generated/decls/entitydef/player/coop.decl"
#define K_ROCK_IMG  "generated/image/models/rock_kit_d.bimage"
#define K_HUD_IMG   "generated/image/guis/hud/ammo.bimage"
static const char k_conductor[] =
    "{\n\tinherit = \"ai/base/conductor\";\n\tedit = {\n\t\taiTypeList = {\n\t\t\tnum = 1;\n"
    "\t\t\titem[0] = {\n\t\t\t\taiType = \"1\";\n\t\t\t\tentityDef = \"ai/demon/imp\";\n\t\t\t}\n\t\t}\n\t}\n}\n";
static const char k_player[] = "{\n\tinherit = \"player/base\";\n\tedit = {\n\t\thealth = 100;\n\t}\n}\n";
static const char k_rock_img[] = "ROCKIMAGE-DOOM-BYTES-0123456789";
static const char k_hud_img[] = "HUDIMAGE-DOOM-BYTES-0123456789";
static const struct { const char *request; const char *bytes; } k_doom[] = {
    { K_CONDUCTOR, k_conductor }, { K_PLAYER, k_player }, { K_ROCK_IMG, k_rock_img }, { K_HUD_IMG, k_hud_img },
};
static const char *k_find(const char *source)
{
    size_t i;
    for (i = 0; i < sizeof k_doom / sizeof k_doom[0]; i++) if (!_stricmp(source, k_doom[i].request)) return k_doom[i].bytes;
    return NULL;
}
static int k_identify(void *c, const char *source, char sha[SH_MB_HASH_HEX + 1], unsigned long long *size)
{
    const char *t = k_find(source);
    (void)c;
    if (!t) return SH_MB_STOCK_ABSENT;
    sh_mb_sha256_hex(t, strlen(t), sha);
    *size = strlen(t);
    return SH_MB_STOCK_OK;
}
static int k_match(void *c, const char *request, unsigned long long size, const char *sha256, char **source)
{
    char sha[SH_MB_HASH_HEX + 1];
    unsigned long long n = 0;
    (void)c;
    if (k_identify(NULL, request, sha, &n) != SH_MB_STOCK_OK || n != size || strcmp(sha, sha256)) return SH_MB_STOCK_ABSENT;
    *source = _strdup(request);
    return *source ? SH_MB_STOCK_OK : SH_MB_STOCK_UNAVAILABLE;
}
static int k_verify(void *c, const char *source, const char *sha256, unsigned long long *size)
{
    char sha[SH_MB_HASH_HEX + 1];
    (void)c;
    if (k_identify(NULL, source, sha, size) != SH_MB_STOCK_OK) return SH_MB_STOCK_ABSENT;
    return strcmp(sha, sha256) ? SH_MB_STOCK_MISMATCH : SH_MB_STOCK_OK;
}
static int k_read(void *c, const char *source, const char *sha256, unsigned char **bytes, size_t *len)
{
    const char *t = k_find(source);
    (void)c; (void)sha256;
    if (!t) return SH_MB_STOCK_ABSENT;
    *len = strlen(t);
    if (!(*bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, *len + 1))) return SH_MB_STOCK_UNAVAILABLE;
    memcpy(*bytes, t, *len);
    return SH_MB_STOCK_OK;
}
static int k_container(void *c, const char *source, const char *sha256, char *out, size_t cap)
{
    (void)c; (void)sha256;
    if (!k_find(source)) return SH_MB_STOCK_ABSENT;
    strcpy_s(out, cap, "gameresources");
    return SH_MB_STOCK_OK;
}
static const sh_mb_stock_ops k_ops = { NULL, k_match, k_identify, k_verify, k_read, k_container, NULL };

#define SP "pd4salt4"   /* props: two props, a Toybox row, a DOOM file it changes, a DOOM file it surfaces */
#define SI "pe5salt5"   /* an interactable: a Toybox row, its entity and the script that binds it */
#define SM "pf6salt6"   /* a module and its info */
#define SH "pg7salt7"   /* a player and HUD mod: DOOM's player and an image it changes, nothing placeable */
#define SD "ph8salt8"   /* two demons and the conductor that pairs their AI types */

static void kinds_world(const char *author)
{
    char text[1024];
    /* props */
    write_mod(author, "props", SP, "Props");
    crate(author, "props", SP, "crate", 1);
    crate(author, "props", SP, "lamp", 5);
    put_text(author, "props", "decls\\snapeditorentitydef\\crate_row.decl",
             "{\n\tinherit = \"action/door/open\";\n\tedit = {\n\t\tentityDef = \"entities/crate\";\n\t}\n}\n");
    /* a prop whose material names DOOM's image, which the mod's native/ holds unchanged (a surface) */
    put_text(author, "props", "entities\\rock.decl",
             "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t\trenderModelInfo = {\n"
             "\t\t\tmodel = \"meshes/models/rock.lwo\";\n\t\t}\n\t}\n}\n");
    put_bin(author, "props", "meshes\\models\\rock.bmodel", "mods/" SP "/rock_skin", 11);
    put_text(author, "props", "materials\\rock_skin.decl", "{\ndiffusemap\t\"models/rock_kit_d.tga\"\n}\n");
    put_text(author, "props", "native\\generated\\image\\models\\rock_kit_d.bimage", k_rock_img);
    /* a DOOM image it changes: wherever the mod is used */
    put_text(author, "props", "native\\generated\\image\\guis\\hud\\ammo.bimage", "HUDIMAGE-PROPS-CHANGED");
    /* interactable */
    write_mod(author, "panel", SI, "Panel");
    crate(author, "panel", SI, "panel", 21);
    put_text(author, "panel", "editor\\toybox.json",
             "{\"tiles\":{\"panel_row\":{\"entity\":\"entities/panel\",\"template\":\"editor/templates/panel.decl\","
             "\"tab\":\"#str_snapentity_category_demons\"}}}");
    put_text(author, "panel", "editor\\templates\\panel.decl",
             "{\n\tinherit = \"action/door/open\";\n\tedit = {\n\t\tentityDef = \"entities/panel\";\n\t}\n}\n");
    put_text(author, "panel", "scripts\\panel.lua", "api = 1\nlocal def = \"entities/panel\"\nlocal snd = \"materials/panel_glow\"\n");
    put_text(author, "panel", "materials\\panel_glow.decl", "{\ndiffusemap\t\"images/panel_d.tga\"\n}\n");
    /* module */
    write_mod(author, "room", SM, "Room");
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\n\tedit = {\nmodule = {\n\tname = \"modules/mods/%s/room.map\";\n\tportals = {\n\t\tnum = 0;\n\t}\n"
                "\tentities = {\n\t\tnum = 0;\n\t}\n}\n\t}\n}\n", SM);
    put_text(author, "room", "map-modules\\room.decl", text);
    put_text(author, "room", "decls\\snapmoduleinfo\\room.decl", "{\n\tedit = {\n\t\tmoduleType = \"MODULE_TYPE_C\";\n\t}\n}\n");
    put_bin(author, "room", "map-modules\\room\\_combo\\world.bcm", NULL, 31);
    /* player and HUD */
    write_mod(author, "player", SH, "Player");
    put_text(author, "player", "native\\generated\\decls\\entitydef\\player\\coop.decl",
             "{\n\tinherit = \"player/base\";\n\tedit = {\n\t\thealth = 150;\n\t\thudIcon = \"images/ammo_icon\";\n\t}\n}\n");
    put_bin(author, "player", "images\\ammo_icon.bimage", NULL, 41);
    put_text(author, "player", "native\\generated\\image\\guis\\hud\\ammo.bimage", "HUDIMAGE-PLAYER-CHANGED");
    /* demons */
    write_mod(author, "demons", SD, "Demons");
    crate(author, "demons", SD, "imp_red", 51);
    crate(author, "demons", SD, "imp_blue", 61);
    put_text(author, "demons", "native\\generated\\decls\\entitydef\\ai\\conductor\\coop\\snapmap_default.decl",
             "{\n\tinherit = \"ai/base/conductor\";\n\tedit = {\n\t\taiTypeList = {\n\t\t\tnum = 3;\n"
             "\t\t\titem[0] = {\n\t\t\t\taiType = \"1\";\n\t\t\t\tentityDef = \"ai/demon/imp\";\n\t\t\t}\n"
             "\t\t\titem[1] = {\n\t\t\t\taiType = \"2\";\n\t\t\t\tentityDef = \"entities/imp_red\";\n\t\t\t}\n"
             "\t\t\titem[2] = {\n\t\t\t\taiType = \"3\";\n\t\t\t\tentityDef = \"entities/imp_blue\";\n\t\t\t}\n"
             "\t\t}\n\t}\n}\n");
}

static size_t kinds_run(const char *label, const char *author, const char *player, const char *json)
{
    size_t bad = run_parity(label, author, player, json, strlen(json), NULL, 0, NULL);
    mt_remove_tree(player);
    CreateDirectoryA(player, NULL);
    return bad;
}

static void case_kinds(void)
{
    static const struct { const char *label, *json; } placed[] = {
        { "an entityDef placed alone", "{\"entities\":[{\"inherit\":\"mods/" SP "/crate\"}],\"instances\":[]}" },
        { "a Toybox row placed alone", "{\"entities\":[{\"inherit\":\"mods/" SP "/crate_row\"}],\"instances\":[]}" },
        { "a prop drawn with a DOOM image the mod surfaces", "{\"entities\":[{\"inherit\":\"mods/" SP "/rock\"}],\"instances\":[]}" },
        { "the interactable's row and the script that binds its entity",
          "{\"entities\":[{\"inherit\":\"mods/" SI "/panel_row\"}],\"instances\":[]}" },
        { "a module placed alone", "{\"entities\":[],\"instances\":[{\"moduleName\":\"maps/modules/mods/" SM "/room.decl\"}]}" },
        { "the props placed together, the interactable and the module",
          "{\"entities\":[{\"inherit\":\"mods/" SP "/crate\"},{\"inherit\":\"mods/" SP "/lamp\"},{\"inherit\":\"mods/" SP "/rock\"},"
          "{\"inherit\":\"mods/" SI "/panel\"}],\"instances\":[{\"moduleName\":\"maps/modules/mods/" SM "/room.decl\"}]}" },
    };
    char work[MAX_PATH], author[MAX_PATH * 2], player[MAX_PATH * 2];
    size_t i;
    CHECK(mt_temp_dir("parityk", work, sizeof work));
    _snprintf_s(author, sizeof author, _TRUNCATE, "%s\\author", work);
    _snprintf_s(player, sizeof player, _TRUNCATE, "%s\\player", work);
    CreateDirectoryA(author, NULL);
    CreateDirectoryA(player, NULL);
    sh_mb_set_stock_ops(&k_ops);
    kinds_world(author);
    {
        /* what each native/ file is to the game: the unchanged image a surface, the changed one a replacement */
        char folder[MAX_PATH * 2];
        sh_mb_bundle b;
        size_t f;
        int surface = 0, replace = 0;
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\props", author);
        if (sh_mb_build(folder, SP, &g_resolver, &b) && !b.errors)
            for (f = 0; f < b.file_count; f++) {
                if (!_stricmp(b.files[f].request, K_ROCK_IMG)) surface = b.files[f].cls == SH_MB_CLASS_SURFACE;
                if (!_stricmp(b.files[f].request, K_HUD_IMG)) replace = b.files[f].cls == SH_MB_CLASS_REPLACE;
            }
        CHECK(surface && replace);
        sh_mb_free(&b);
    }
    for (i = 0; i < sizeof placed / sizeof placed[0]; i++) {
        CHECK(kinds_run(placed[i].label, author, player, placed[i].json) == 0);
        CHECK(g_last_q6 == 0);
    }
    CHECK(ps_has(&g_last_served, K_ROCK_IMG));    /* the surface serves where its name is reached */
    /* A player and HUD mod used as a whole (a map-level row): its DOOM files and what they name serve in every mode. */
    g_attach = SH;
    CHECK(kinds_run("a player and HUD mod attached as a map-level row", author, player,
                    "{\"entities\":[],\"instances\":[]}") == 0);
    CHECK(g_last_q6 == 0);
    CHECK(ps_has(&g_last_served, "generated/image/mods/" SH "/ammo_icon.bimage"));
    g_attach = NULL;
    {
        sh_modmanifest_list cur;
        size_t r;
        memset(&cur, 0, sizeof cur);
        sh_modmanifest_current(&cur);
        for (r = 0; r < cur.count; r++) sh_modmanifest_current_remove(cur.rows[r].build);
        sh_modmanifest_list_free(&cur);
    }
    /* One of two demons the conductor pairs: own play serves the whole build's conductor, a published map the unit's
     * cut of it. The difference is Q6's and nothing else diverges. */
    CHECK(kinds_run("one demon of two the mod's conductor pairs", author, player,
                    "{\"entities\":[{\"inherit\":\"mods/" SD "/imp_red\"}],\"instances\":[]}") == 0);
    CHECK(g_last_q6 > 0);
    sh_mb_set_stock_ops(NULL);
    mt_remove_tree(work);
}

/* Two demons of one pack with entry scripts, brains, a shared library and graphs a brain names by a passed-on
 * literal (require(B.SPEC.graph)). A comment or graph data naming the other demon is no name the runtime loads,
 * so one placed demon serves the same in every mode while the reference follows the graph its brain names. */
#define SQ "pi9salt9"
static void case_pack_scripts(void)
{
    static const char *const demons[] = { "alpha", "beta" };
    char work[MAX_PATH], author[MAX_PATH * 2], player[MAX_PATH * 2], rel[MAX_PATH], text[1024];
    size_t d;
    CHECK(mt_temp_dir("paritys", work, sizeof work));
    _snprintf_s(author, sizeof author, _TRUNCATE, "%s\\author", work);
    _snprintf_s(player, sizeof player, _TRUNCATE, "%s\\player", work);
    CreateDirectoryA(author, NULL);
    CreateDirectoryA(player, NULL);
    write_mod(author, "pack", SQ, "Pack");
    put_text(author, "pack", "scripts\\lib\\shared.lua",
             "-- every brain names its graph by a literal (beta_graph for the second demon)\n"
             "local S = {}\nfunction S.bind(def, brain) return brain end\nreturn S\n");
    for (d = 0; d < 2; d++) {
        const char *n = demons[d];
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "entities\\ai\\demon\\%s.decl", n);
        put_text(author, "pack", rel, "{\n\tinherit = \"snapmaps/props/crate\";\n\tedit = {\n\t}\n}\n");
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "scripts\\%s.lua", n);
        _snprintf_s(text, sizeof text, _TRUNCATE,
                    "api = 1\nrequire(\"shared\").bind(\"ai/demon/%s\", require(\"%s_brain\").new())\n", n, n);
        put_text(author, "pack", rel, text);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "scripts\\lib\\%s_brain.lua", n);
        _snprintf_s(text, sizeof text, _TRUNCATE,
                    "local B = {}\nB.SPEC = { graph = \"%s_graph\" }\n"
                    "function B.new() return { graph = require(B.SPEC.graph) } end\nreturn B\n", n);
        put_text(author, "pack", rel, text);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "scripts\\lib\\%s_graph.lua", n);
        put_text(author, "pack", rel, "local M = {}\nM.aiTypes = { \"imp\", \"beta\" }\nreturn M\n");
    }
    CHECK(kinds_run("one demon of a pack whose scripts name the other's", author, player,
                    "{\"entities\":[{\"inherit\":\"mods/" SQ "/ai/demon/alpha\"}],\"instances\":[]}") == 0);
    CHECK(g_last_q6 == 0);
    CHECK(ps_has(&g_last_served, "mods/" SQ "/scripts/alpha.lua"));
    CHECK(ps_has(&g_last_served, "mods/" SQ "/scripts/lib/alpha_graph.lua"));   /* named by the literal passed on */
    CHECK(!ps_has(&g_last_served, "mods/" SQ "/scripts/beta.lua"));
    CHECK(!ps_has(&g_last_served, "mods/" SQ "/scripts/lib/beta_graph.lua"));
    CHECK(kinds_run("both demons of the pack", author, player,
                    "{\"entities\":[{\"inherit\":\"mods/" SQ "/ai/demon/alpha\"},{\"inherit\":\"mods/" SQ "/ai/demon/beta\"}],"
                    "\"instances\":[]}") == 0);
    CHECK(ps_has(&g_last_served, "mods/" SQ "/scripts/lib/beta_graph.lua"));
    mt_remove_tree(work);
}

int main(int argc, char **argv)
{
    const char *mods = NULL, *placed = NULL, *map = NULL, *doom = NULL, *out = NULL;
    int k;
    for (k = 1; k < argc; k++) {
        if (!strncmp(argv[k], "mods=", 5)) mods = argv[k] + 5;
        else if (!strncmp(argv[k], "placed=", 7)) placed = argv[k] + 7;
        else if (!strncmp(argv[k], "map=", 4)) map = argv[k] + 4;
        else if (!strncmp(argv[k], "doom=", 5)) doom = argv[k] + 5;
        else if (!strncmp(argv[k], "out=", 4)) out = argv[k] + 4;
        else if (!strncmp(argv[k], "attach=", 7)) g_attach = argv[k] + 7;
    }
    sh_mb_set_script_check(sh_script_compile_check);
    sh_mb_set_look_compiler(sh_look_compile_notes);
    /* the product's own files, as dllmain.c wires them */
    sh_modserve_set_product_owner(sh_modprog_owns);
    sh_modserve_set_product_reader(sh_modprog_read);
    sh_cache_set_resolver(&g_resolver);
    if (doom) { sh_modstock_set_install(doom); sh_modstock_register(); g_doom = 1; }
    else sh_modpin_set_game(fixture_game, NULL);
    if (mods) {
        /* A real mod set: copied first, so nothing of the folder given is written. */
        char work[MAX_PATH], author[MAX_PATH * 2], player[MAX_PATH * 2], *json = NULL;
        size_t len = 0;
        FILE *rep = NULL;
        size_t bad;
        if (!placed && !map && !g_attach) {
            fprintf(stderr, "usage: parity_test mods=<folder> placed=<a>,<b> [map=<text>] [doom=<install>] [attach=<salt>]\n");
            return 2;
        }
        if (!placed) placed = "";
        if (!mt_temp_dir("parity_real", work, sizeof work)) return 2;
        _snprintf_s(author, sizeof author, _TRUNCATE, "%s\\author", work);
        _snprintf_s(player, sizeof player, _TRUNCATE, "%s\\player", work);
        CreateDirectoryA(author, NULL);
        {
            char pattern[MAX_PATH * 2];
            WIN32_FIND_DATAA fd;
            HANDLE h;
            _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", mods);
            h = FindFirstFileA(pattern, &fd);
            while (h != INVALID_HANDLE_VALUE) {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && _stricmp(fd.cFileName, "maps")) {
                    char s[MAX_PATH * 2], d[MAX_PATH * 2];
                    _snprintf_s(s, sizeof s, _TRUNCATE, "%s\\%s", mods, fd.cFileName);
                    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\%s", author, fd.cFileName);
                    if (!mt_copy_tree(s, d)) { fprintf(stderr, "parity: %s could not be copied\n", s); return 2; }
                }
                if (!FindNextFileA(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
            }
        }
        if (map) {
            FILE *f = NULL;
            if (fopen_s(&f, map, "rb") || !f) { fprintf(stderr, "parity: %s does not read\n", map); return 2; }
            fseek(f, 0, SEEK_END);
            len = (size_t)ftell(f);
            fseek(f, 0, SEEK_SET);
            json = (char *)malloc(len + 1);
            if (json) { len = fread(json, 1, len, f); json[len] = 0; }
            fclose(f);
        } else {
            /* placed names as a map that places each */
            size_t cap = strlen(placed) * 2 + 256, i;
            const char *p = placed;
            json = (char *)malloc(cap);
            strcpy_s(json, cap, "{\"entities\":[");
            for (i = 0; *p; i++) {
                const char *e = strchr(p, ',');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                strcat_s(json, cap, i ? ",{\"inherit\":\"" : "{\"inherit\":\"");
                strncat_s(json, cap, p, n);
                strcat_s(json, cap, "\"}");
                p += n + (e ? 1 : 0);
            }
            strcat_s(json, cap, "],\"instances\":[]}");
            len = strlen(json);
        }
        if (out && (fopen_s(&rep, out, "wb") || !rep)) rep = NULL;
        bad = run_parity(map ? map : placed[0] ? placed : g_attach, author, player, json, len, NULL, 0, rep);
        if (rep) fclose(rep);
        free(json);
        printf("parity: work folder %s (remove it when done)\n", work);
        return bad || failures ? 1 : 0;
    }
    case_one_mod();
    case_two_mods();
    case_look();
    case_kinds();
    case_pack_scripts();
    if (failures) { fprintf(stderr, "parity_test: %d failure(s)\n", failures); return 1; }
    printf("parity_test: every mode served every placed object the same\n");
    return 0;
}
