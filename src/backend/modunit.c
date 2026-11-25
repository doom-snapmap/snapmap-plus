/* modunit.c -- see modunit.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "backend_log.h"
#include "modunit.h"
#include "modauthor.h"
#include "modsalt.h"
#include "mp_json.h"
#include "../common/text.h"

/* ------------------------------------------------------------ names ---- */

static int uw_lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

/* A character a name in decl text or in a binary is spelled with. */
static int uw_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '/' || c == '\\' || c == '$' || c == '-' || c == '+' || c == '@' || c == '~' || c == '(' || c == ')';
}

/* "mods/<ns>" then '/' or '\' at s (n bytes left), any case. */
static int uw_ns_at(const char *s, size_t n, const char *ns, size_t nl)
{
    size_t i;
    if (n < 6 + nl) return 0;
    if (uw_lower((unsigned char)s[0]) != 'm' || uw_lower((unsigned char)s[1]) != 'o' ||
        uw_lower((unsigned char)s[2]) != 'd' || uw_lower((unsigned char)s[3]) != 's' || (s[4] != '/' && s[4] != '\\'))
        return 0;
    for (i = 0; i < nl; i++) if (uw_lower((unsigned char)s[5 + i]) != uw_lower((unsigned char)ns[i])) return 0;
    return s[5 + nl] == '/' || s[5 + nl] == '\\';
}

/* The key of the run at s (it starts with "mods/<ns>/"), written under mods/<salt>/: lower case, '\' as '/', cut at
 * '$', every character but [a-z0-9/._] as '_', the last segment's extensions cut. NULL when nothing follows. */
static char *uw_key_run(const char *s, size_t n, size_t nl, const char *salt)
{
    char *k;
    size_t i, len, last = 0, sl = strlen(salt), head = 6 + sl;
    for (i = 0; i < n && uw_name_char((unsigned char)s[i]) && s[i] != '$'; i++) {}
    n = i;
    if (n <= 6 + nl) return NULL;
    if (!(k = (char *)malloc(n - (6 + nl) + head + 1))) return NULL;
    memcpy(k, "mods/", 5);
    memcpy(k + 5, salt, sl);
    k[5 + sl] = '/';
    len = head;
    last = head;
    for (i = 6 + nl; i < n; i++) {
        int c = uw_lower((unsigned char)s[i]);
        if (c == '\\') c = '/';
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_')) c = '_';
        k[len++] = (char)c;
        if (c == '/') last = len;
    }
    k[len] = 0;
    while (len > last && k[len - 1] == '/') k[--len] = 0;
    {
        char *dot = strchr(k + last, '.');
        if (dot) *dot = 0;
    }
    len = strlen(k);
    if (len <= head || k[len - 1] == '/') { free(k); return NULL; }
    return k;
}

char *sh_unit_key(const char *s, size_t n, const char *ns, const char *salt)
{
    size_t i, nl = ns ? strlen(ns) : 0;
    if (!s || !nl) return NULL;
    if (!salt) salt = ns;
    for (i = 0; i + 6 + nl <= n; i++)
        if (uw_ns_at(s + i, n - i, ns, nl)) return uw_key_run(s + i, n - i, nl, salt);
    return NULL;
}

int sh_unit_enumerated_type(const char *type)
{
    static const char *const types[] = { "snapAIScore", "snapAIEncounter", "snapAIEncounterTable", NULL };
    int i;
    for (i = 0; type && types[i]; i++) if (!_stricmp(type, types[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------ the walk ---- */

typedef struct uw_pair { const char *key; size_t file; } uw_pair;
typedef struct uw_ptr { char *at, *path, *source; char sha[SH_MB_HASH_HEX + 1]; } uw_ptr;
typedef struct uw_list { char **v; size_t n, cap; } uw_list;
typedef struct uw_ids { uint32_t *v; size_t n; int read; } uw_ids;

typedef struct uw_script {
    size_t file;
    size_t *defs;          /* the mod's entityDefs it binds or names in a literal */
    size_t ndefs, cdefs;
    int dynamic;           /* it binds a name only while it runs */
} uw_script;

typedef struct uw_enum { size_t file; size_t *defs; size_t ndefs; } uw_enum;

typedef struct uw {
    const sh_mb_bundle *b;
    const char *folder;
    const sh_unit_env *env;
    const char *salt;
    size_t sl;
    const char *spell;     /* the namespace the carried binaries spell (the build's "spelled", else the salt) */
    uw_pair *keys;         /* files but board squares, by name key */
    size_t nkeys;
    uw_pair *reqs;         /* every file in the folder or pointed to, by request */
    size_t nreqs;
    uw_pair *fams;         /* board squares, by their family's key */
    size_t nfams;
    char **owned;          /* per file: its key, freed at the end */
    uw_script *scripts;
    size_t nscripts;
    uw_enum *enums;
    size_t nenums;
    uw_ids *bank;          /* per file: a bank's event ids, read on first need */
    unsigned char *keep;
    size_t *queue, qhead, qtail;
    uw_ptr *ptrs;
    size_t nptrs, cptrs;
    uw_list looked;        /* requests asked of the game already (sorted) */
    uw_list strings;       /* #str_ keys the kept texts name (author spelling) */
    uint32_t *events;      /* the kept sound decls' events, two spellings each */
    size_t nevents;
    long *from;            /* per file: the file whose visit reached it, -1 for a seed */
    long current;          /* the file being visited, -1 while seeding */
    int registry;          /* visiting a DOOM file the mod changes: it names every object, and reaches none */
    int spelled;           /* a kept binary spells the build's "spelled" */
    char *fail;
    int oom;
} uw;

static void uw_fail(uw *w, const char *fmt, const char *arg)
{
    if (w->fail) return;
    w->fail = sh_textf(fmt, arg ? arg : "");
    if (!w->fail) w->oom = 1;
}

static int uw_pair_cmp(const void *a, const void *b)
{
    const uw_pair *x = (const uw_pair *)a, *y = (const uw_pair *)b;
    int c = strcmp(x->key, y->key);
    return c ? c : (x->file < y->file ? -1 : x->file > y->file);
}

static size_t uw_lower_bound(const uw_pair *p, size_t n, const char *key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(p[mid].key, key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int uw_is(const sh_mb_file *f, const char *type) { return f->ptype && !_stricmp(f->ptype, type); }

static void uw_reach(uw *w, size_t i)
{
    if (i >= w->b->file_count || w->keep[i]) return;
    /* A DOOM file the mod changes (the conductor's list, a registry) names every object of the mod; what it names
     * is reached from that object's own root, never from the registry. */
    if (w->registry && !w->b->files[i].stock && uw_is(&w->b->files[i], "entityDef")) return;
    w->keep[i] = 1;
    w->from[i] = w->current;
    w->queue[w->qtail++] = i;
}

static void uw_reach_key(uw *w, const char *key)
{
    size_t i = uw_lower_bound(w->keys, w->nkeys, key);
    for (; i < w->nkeys && !strcmp(w->keys[i].key, key); i++) uw_reach(w, w->keys[i].file);
}

/* A spelled name reaches the file of that name and of each of its leading folders below mods/<salt>/. */
static void uw_reach_spelled(uw *w, char *key)
{
    size_t floor = 6 + w->sl, n = strlen(key);
    uw_reach_key(w, key);
    while (n > floor) {
        while (n > floor && key[n - 1] != '/') n--;
        if (n <= floor) break;
        key[--n] = 0;
        uw_reach_key(w, key);
    }
}

/* Every file whose key is `key` or lies below it (a module's own files). */
static void uw_reach_below(uw *w, const char *key)
{
    size_t kl = strlen(key), i = uw_lower_bound(w->keys, w->nkeys, key);
    for (; i < w->nkeys && !strncmp(w->keys[i].key, key, kl); i++)
        if (w->keys[i].key[kl] == 0 || w->keys[i].key[kl] == '/') uw_reach(w, w->keys[i].file);
}

/* Every file whose key begins with `prefix` (a script's literal that continues). */
static void uw_reach_prefix(uw *w, const char *prefix)
{
    size_t kl = strlen(prefix), i = uw_lower_bound(w->keys, w->nkeys, prefix);
    for (; i < w->nkeys && !strncmp(w->keys[i].key, prefix, kl); i++) uw_reach(w, w->keys[i].file);
}

static int uw_reach_request(uw *w, const char *request)
{
    size_t i = uw_lower_bound(w->reqs, w->nreqs, request);
    int hit = 0;
    for (; i < w->nreqs && !strcmp(w->reqs[i].key, request); i++) { uw_reach(w, w->reqs[i].file); hit = 1; }
    return hit;
}

static void uw_reach_name(uw *w, const char *name, size_t n)
{
    char *k = sh_unit_key(name, n, w->salt, w->salt);
    if (k) { uw_reach_spelled(w, k); free(k); }
}

/* Every name the bytes spell under mods/<ns>/, read as the build's own. */
static int uw_scan(uw *w, const unsigned char *t, size_t n, const char *ns)
{
    size_t i, nl = strlen(ns);
    int found = 0;
    for (i = 0; i + 6 + nl <= n; i++) {
        char *k;
        if (uw_lower(t[i]) != 'm' || !uw_ns_at((const char *)t + i, n - i, ns, nl)) continue;
        found = 1;
        k = uw_key_run((const char *)t + i, n - i, nl, w->salt);
        if (k) { uw_reach_spelled(w, k); free(k); }
        i += 4 + nl;
    }
    return found;
}

/* ------------------------------------------------------------ lists ---- */

static int uw_str_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* Add a copy of s (n bytes) to the sorted list; 1 when it was not there. */
static int uw_list_add(uw *w, uw_list *l, const char *s, size_t n)
{
    size_t lo = 0, hi = l->n;
    char *copy = sh_textn(s, n);
    if (!copy) { w->oom = 1; return 0; }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(l->v[mid], copy);
        if (!c) { free(copy); return 0; }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 32;
        char **g = (char **)realloc(l->v, cap * sizeof *g);
        if (!g) { free(copy); w->oom = 1; return 0; }
        l->v = g;
        l->cap = cap;
    }
    memmove(&l->v[lo + 1], &l->v[lo], (l->n - lo) * sizeof *l->v);
    l->v[lo] = copy;
    l->n++;
    return 1;
}

static int uw_list_has(const uw_list *l, const char *s)
{
    return l->n && bsearch(&s, l->v, l->n, sizeof *l->v, uw_str_cmp) != NULL;
}

static void uw_list_free(uw_list *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* ------------------------------------------------------------ files ---- */

static unsigned char *uw_read(const uw *w, const sh_mb_file *f, size_t *len)
{
    char *path = sh_textf("%s\\%s", w->folder, f->path), *p;
    unsigned char *bytes;
    if (!path) return NULL;
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    bytes = sh_mb_read_file(path, (size_t)-1, len);
    free(path);
    return bytes;
}

/* A decl's text as the engine reads it: the mod's names in the author's spelling written as the engine's. */
static int uw_no_key(void *ctx, const char *key, size_t n) { (void)ctx; (void)key; (void)n; return 0; }
static char *uw_engine_text(uw *w, const unsigned char *text, size_t len, size_t *out_len)
{
    size_t n = 0;
    char *t = sh_ma_text(w->salt, (const char *)text, len, uw_no_key, NULL, &n);
    if (t) { *out_len = n; return t; }
    if (n == (size_t)-1) { w->oom = 1; return NULL; }
    if ((t = sh_textn((const char *)text, len)) != NULL) *out_len = len;
    else w->oom = 1;
    return t;
}

/* Every #str_<key> the author's text writes, in the author's spelling. */
static void uw_strings(uw *w, const unsigned char *t, size_t n)
{
    size_t i;
    for (i = 0; i + 5 < n; i++) {
        size_t k;
        if (t[i] != '#' || _strnicmp((const char *)t + i, "#str_", 5)) continue;
        for (k = i + 5; k < n && ((t[k] >= 'a' && t[k] <= 'z') || (t[k] >= 'A' && t[k] <= 'Z') ||
                                  (t[k] >= '0' && t[k] <= '9') || t[k] == '_' || t[k] == '.' || t[k] == '-');
             k++) {}
        if (k > i + 5) uw_list_add(w, &w->strings, (const char *)t + i + 5, k - i - 5);
        i = k;
    }
}

/* ----------------------------------------------------------- pointers ---- */

static void uw_pointer(uw *w, const char *at, const char *path, const char *sha, const char *source)
{
    size_t i;
    for (i = 0; i < w->nptrs; i++)
        if (!strcmp(w->ptrs[i].path, path) && !strcmp(w->ptrs[i].at ? w->ptrs[i].at : "", at ? at : "")) return;
    if (w->nptrs == w->cptrs) {
        size_t cap = w->cptrs ? w->cptrs * 2 : 64;
        uw_ptr *g = (uw_ptr *)realloc(w->ptrs, cap * sizeof *g);
        if (!g) { w->oom = 1; return; }
        w->ptrs = g;
        w->cptrs = cap;
    }
    memset(&w->ptrs[w->nptrs], 0, sizeof w->ptrs[0]);
    w->ptrs[w->nptrs].at = at ? sh_text(at) : NULL;
    w->ptrs[w->nptrs].path = sh_text(path);
    w->ptrs[w->nptrs].source = sh_text(source);
    strcpy_s(w->ptrs[w->nptrs].sha, sizeof w->ptrs[0].sha, sha);
    if (!w->ptrs[w->nptrs].path || !w->ptrs[w->nptrs].source || (at && !w->ptrs[w->nptrs].at)) w->oom = 1;
    w->nptrs++;
}

/* A need the build does not hold: the DOOM or product file it reaches, when one does. Names under the mod's own
 * namespace are the mod's: one the build lacks reaches nothing. */
static void uw_need_outside(uw *w, const sh_mb_need *need, char **req, int n)
{
    int r;
    char sha[SH_MB_HASH_HEX + 1], source[64], *own = sh_unit_key(need->name, strlen(need->name), w->salt, w->salt);
    if (own) { free(own); return; }
    for (r = 0; r < n; r++) {
        int got;
        if (uw_list_has(&w->looked, req[r])) {
            size_t i;
            for (i = 0; i < w->nptrs; i++) if (!w->ptrs[i].at && !strcmp(w->ptrs[i].path, req[r])) return;
            continue;   /* looked up before and absent */
        }
        uw_list_add(w, &w->looked, req[r], strlen(req[r]));
        sha[0] = source[0] = 0;
        got = w->env->game(w->env->game_context, req[r], sha, source, sizeof source);
        if (got < 0) { uw_fail(w, "the game's files cannot be read to look for %s", req[r]); return; }
        if (got == 1) {
            if (strlen(sha) != SH_MB_HASH_HEX || !source[0]) { uw_fail(w, "the game answered %s without its hash", req[r]); return; }
            uw_pointer(w, NULL, req[r], sha, source);
            return;
        }
    }
}

static void uw_needs(uw *w, const sh_mb_file *f)
{
    size_t k;
    for (k = 0; k < f->need_count && !w->fail; k++) {
        char *req[4];
        int n = sh_mb_need_requests(f->needs[k].type, f->needs[k].name, req, 4), r, inside = 0;
        for (r = 0; r < n; r++) inside |= uw_reach_request(w, req[r]);
        uw_reach_name(w, f->needs[k].name, strlen(f->needs[k].name));
        if (!inside) uw_need_outside(w, &f->needs[k], req, n);
        sh_mb_requests_free(req, n);
    }
}

/* -------------------------------------------------------------- banks ---- */

static void uw_bank_ids(uw *w, size_t i)
{
    uw_ids *ids = &w->bank[i];
    unsigned char *bytes;
    size_t len = 0, at = 0, cap = 0;
    if (ids->read) return;
    ids->read = 1;
    if (!(bytes = uw_read(w, &w->b->files[i], &len))) return;
    while (at + 8 <= len) {
        uint32_t size;
        memcpy(&size, bytes + at + 4, 4);
        if (size > len - at - 8) break;
        if (!memcmp(bytes + at, "HIRC", 4) && size >= 4) {
            const size_t end = at + 8 + size;
            size_t o = at + 12;
            uint32_t n, e;
            memcpy(&n, bytes + at + 8, 4);
            for (e = 0; e < n && o + 5 <= end; ++e) {
                uint32_t body, id;
                memcpy(&body, bytes + o + 1, 4);
                if (body > end - o - 5) break;
                if (bytes[o] == 4 && body >= 4) {   /* an Event: its id is the FNV-1 of its name */
                    memcpy(&id, bytes + o + 5, 4);
                    if (ids->n == cap) {
                        size_t grow = cap ? cap * 2 : 64;
                        uint32_t *g = (uint32_t *)realloc(ids->v, grow * sizeof *g);
                        if (!g) { w->oom = 1; break; }
                        ids->v = g;
                        cap = grow;
                    }
                    ids->v[ids->n++] = id;
                }
                o += 5 + (size_t)body;
            }
        }
        at += 8 + (size_t)size;
    }
    free(bytes);
}

/* A kept sound decl's event: play_<leaf> as the author's bank names it, and play_<rest> where the leaf is
 * <salt>_<rest> (modserve.c ms_sound_events renames one to the other at serve). The banks are chosen once the walk
 * settles (uw_banks). */
static int uw_sound_ids(const uw *w, const sh_mb_file *f, uint32_t ev[2]);

static void uw_sound(uw *w, const sh_mb_file *f)
{
    uint32_t ev[2], *g;
    if (!uw_sound_ids(w, f, ev)) return;
    if (!(g = (uint32_t *)realloc(w->events, (w->nevents + 1) * 2 * sizeof *g))) { w->oom = 1; return; }
    w->events = g;
    g[w->nevents * 2] = ev[0];
    g[w->nevents * 2 + 1] = ev[1];
    w->nevents++;
}

static int uw_bank_holds(const uw_ids *ids, const uint32_t ev[2])
{
    size_t k;
    for (k = 0; k < ids->n; k++) if (ids->v[k] == ev[0] || ids->v[k] == ev[1]) return 1;
    return 0;
}

/* The two event ids of the sound decl `f` (uw_sound). 0 when it names none. */
static int uw_sound_ids(const uw *w, const sh_mb_file *f, uint32_t ev[2])
{
    const char *leaf = strrchr(f->request, '/'), *dot;
    char name[600];
    size_t ll;
    leaf = leaf ? leaf + 1 : f->request;
    dot = strrchr(leaf, '.');
    ll = dot ? (size_t)(dot - leaf) : strlen(leaf);
    if (ll + 6 > sizeof name) return 0;
    _snprintf_s(name, sizeof name, _TRUNCATE, "play_%.*s", (int)ll, leaf);
    ev[0] = ev[1] = sh_mb_fnv1_lower(name, strlen(name));
    if (ll > w->sl + 1 && !_strnicmp(leaf, w->salt, w->sl) && leaf[w->sl] == '_') {
        _snprintf_s(name, sizeof name, _TRUNCATE, "play_%.*s", (int)(ll - w->sl - 1), leaf + w->sl + 1);
        ev[1] = sh_mb_fnv1_lower(name, strlen(name));
    }
    return 1;
}

/* A kept bank is whole with the sound decls of its events: the serving index gives a bank's events the namespace
 * of the decls the folder holds (modserve.c ms_sound_events), so a bank is served as the same bytes only beside
 * all of them. */
static void uw_bank_decls(uw *w, size_t bank)
{
    size_t j;
    uw_bank_ids(w, bank);
    for (j = 0; j < w->b->file_count; j++) {
        const sh_mb_file *g = &w->b->files[j];
        uint32_t ev[2];
        if (w->keep[j] || g->stock || g->absent || !uw_is(g, "sound") || !uw_sound_ids(w, g, ev)) continue;
        if (uw_bank_holds(&w->bank[bank], ev)) uw_reach(w, j);
    }
}

/* The banks the kept sound decls need: a bank that alone holds one of their events, then, for an event several
 * banks hold (an event two objects share, stored in each one's bank), one of them unless a kept bank already
 * holds it. Wwise plays an event from whichever loaded bank holds it, so one is enough. */
static void uw_banks(uw *w)
{
    size_t e, i, pass;
    for (pass = 0; pass < 2; pass++)
        for (e = 0; e < w->nevents && !w->oom; e++) {
            size_t holders = 0, first = (size_t)-1;
            int kept = 0;
            for (i = 0; i < w->b->file_count; i++) {
                const sh_mb_file *g = &w->b->files[i];
                char stem[SH_MB_BANK_STEM_MAX + 1];
                if (g->absent || g->stock || !sh_mb_bank_stem(g->request, stem, sizeof stem)) continue;
                uw_bank_ids(w, i);
                if (!uw_bank_holds(&w->bank[i], &w->events[e * 2])) continue;
                holders++;
                if (first == (size_t)-1) first = i;
                kept |= w->keep[i];
            }
            if (!holders || kept) continue;
            if (pass == 0 && holders == 1) uw_reach(w, first);
            else if (pass == 1) uw_reach(w, first);
        }
}

/* ----------------------------------------------------- render programs ---- */

/* A kept render program keeps its permutations, its cooked binaries and its SPIR-V: every file under
 * generated/renderprogs/ or generated/spirv/ that is the program's or spells its name. */
static void uw_program(uw *w, const sh_mb_file *f)
{
    const char *leaf = f->pname ? strrchr(f->pname, '/') : NULL;
    char part[31];
    size_t i;
    leaf = leaf ? leaf + 1 : f->pname;
    if (!leaf || !leaf[0]) return;
    strncpy_s(part, sizeof part, leaf, _TRUNCATE);   /* the SPIR-V request cuts a long name into 30-character parts */
    for (char *c = part; *c; c++) *c = (char)uw_lower((unsigned char)*c);
    for (i = 0; i < w->b->file_count; i++) {
        const sh_mb_file *g = &w->b->files[i];
        char *program = NULL, mask[17];
        int hit = 0;
        if (w->keep[i] || g->absent) continue;
        if (sh_mb_rp_permutation(g->request, &program, mask)) hit = program && !_stricmp(program, f->pname);
        else if (!strncmp(g->request, "generated/renderprogs/", 22) || !strncmp(g->request, "generated/spirv/", 16))
            hit = strstr(g->request, part) != NULL;
        free(program);
        if (hit) uw_reach(w, i);
    }
}

/* -------------------------------------------------------------- boards ---- */

/* The four numbers of `{ a, b, c, d }` after `key` in an engine text. */
static int uw_vec4(const char *t, const char *key, double v[4])
{
    const char *p = t;
    size_t kl = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        const char *q = p + kl;
        int i;
        if ((p > t && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) || isalnum((unsigned char)*q) || *q == '_') {
            p = q;
            continue;
        }
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '{') { p = q; continue; }
        q++;
        for (i = 0; i < 4; i++) {
            char *end;
            while (*q == ' ' || *q == '\t' || *q == ',' || *q == '\r' || *q == '\n') q++;
            v[i] = strtod(q, &end);
            if (end == q) return 0;
            q = end;
        }
        return 1;
    }
    return 0;
}

/* The name after `landpagefile` (quoted or bare), malloc'd. */
static char *uw_landpage(const char *t)
{
    const char *p = t;
    while ((p = strstr(p, "landpagefile")) != NULL) {
        const char *q = p + 12, *e;
        if ((p > t && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) || isalnum((unsigned char)*q) || *q == '_') {
            p = q;
            continue;
        }
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '"') { q++; e = strchr(q, '"'); }
        else for (e = q; *e && !strchr(" \t\r\n;}", *e); e++) {}
        return e && e > q ? sh_textn(q, (size_t)(e - q)) : NULL;
    }
    return NULL;
}

/* A material that draws from one of the mod's boards keeps the squares its virtualmapping covers (w, h, x, y as
 * fractions of the family's side; a family is 4 x 4 squares) and square 1, or the whole family when the mapping
 * cannot be read. */
static void uw_board(uw *w, const char *engine)
{
    char *family = uw_landpage(engine), *key;
    double v[4];
    unsigned want = 0xFFFFu;
    size_t i;
    if (!family) return;
    key = sh_unit_key(family, strlen(family), w->salt, w->salt);
    free(family);
    if (!key) return;
    if (uw_vec4(engine, "virtualmapping", v) && v[0] > 0 && v[1] > 0 && v[2] >= 0 && v[3] >= 0 && v[2] + v[0] <= 1.0000001 &&
        v[3] + v[1] <= 1.0000001) {
        int c0 = (int)(v[2] * 4), c1 = (int)((v[2] + v[0]) * 4 - 1e-9), r0 = (int)(v[3] * 4),
            r1 = (int)((v[3] + v[1]) * 4 - 1e-9), r, c;
        want = 0;
        if (c1 > 3) c1 = 3;
        if (r1 > 3) r1 = 3;
        for (r = r0; r <= r1; r++)
            for (c = c0; c <= c1; c++) want |= 1u << (r * 4 + c);
        /* Square 1 too, always (D-A29): with no _q members, the family's coarsest page (mip 11) is read from square 1
         * alone, so a cut without it has no page there (idMegaTexture2's page lookup, R-LV2 section 4). */
        want |= 1u;
    }
    i = uw_lower_bound(w->fams, w->nfams, key);
    for (; i < w->nfams && !strcmp(w->fams[i].key, key); i++) {
        int member = 0;
        if (sh_mb_vt_member(w->b->files[w->fams[i].file].request, NULL, &member) && member >= 1 && member <= 16 &&
            (want & (1u << (member - 1))))
            uw_reach(w, w->fams[i].file);
    }
    free(key);
}

/* ------------------------------------------------------------- scripts ---- */

/* The engine names a script's literal may stand for (each malloc'd, NULL where none): in the author's spelling, in
 * the engine's, mod-relative, and after a decl type's folder. Returns the count; *prefix 1 when the literal ends in
 * a character a name continues after. */
static size_t uw_literal_names(uw *w, const char *s, size_t n, char *cand[4], int *prefix)
{
    char *lit;
    size_t i, m = 0;
    int letters = 0;
    memset(cand, 0, 4 * sizeof *cand);
    *prefix = 0;
    if (!n || n > 1024) return 0;
    if (!(lit = sh_textn(s, n))) { w->oom = 1; return 0; }
    for (i = 0; i < n; i++) {
        lit[i] = (char)uw_lower((unsigned char)lit[i]);
        if (lit[i] == '\\') lit[i] = '/';
        if (lit[i] >= 'a' && lit[i] <= 'z') letters = 1;
    }
    if (!letters || strchr(lit, ' ') || strchr(lit, '\n')) { free(lit); return 0; }
    *prefix = strchr("/_-.", lit[n - 1]) != NULL;
    cand[m++] = sh_ma_engine_name(w->salt, lit, n);
    if (!strncmp(lit, "mods/", 5)) cand[m++] = sh_text(lit);
    else {
        const char *slash = strchr(lit, '/');
        cand[m++] = sh_textf("mods/%s/%s", w->salt, lit);
        if (slash && slash[1]) cand[m++] = sh_textf("mods/%s/%s", w->salt, slash + 1);
    }
    free(lit);
    return m;
}

static void uw_literal(uw *w, const char *s, size_t n)
{
    char *cand[4];
    size_t i, c;
    int prefix;
    size_t m = uw_literal_names(w, s, n, cand, &prefix);
    for (c = 0; c < m; c++) {
        char *k;
        size_t kl;
        if (!cand[c]) continue;
        if (prefix) {
            /* the key spelling of a prefix: what sh_unit_key gives, without cutting a last segment */
            kl = strlen(cand[c]);
            if (!(k = (char *)malloc(kl + 1))) { w->oom = 1; break; }
            for (i = 0; i <= kl; i++) {
                int ch = (unsigned char)cand[c][i];
                if (ch && !((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '/' || ch == '.' || ch == '_')) ch = '_';
                k[i] = (char)ch;
            }
            if (!strncmp(k, "mods/", 5) && kl > 6 + w->sl && !_strnicmp(k + 5, w->salt, w->sl)) uw_reach_prefix(w, k);
            free(k);
        }
        uw_reach_name(w, cand[c], strlen(cand[c]));
    }
    for (c = 0; c < m; c++) free(cand[c]);
}

/* Every string literal of a Lua text, each passed to fn (sh_mb_script_literals: comments are skipped). */
typedef void (*uw_literal_fn)(uw *w, const char *s, size_t n, void *ctx);
typedef struct uw_literals_ctx { uw *w; uw_literal_fn fn; void *ctx; } uw_literals_ctx;
static int uw_literal_each(void *context, const char *s, size_t n)
{
    uw_literals_ctx *c = (uw_literals_ctx *)context;
    c->fn(c->w, s, n, c->ctx);
    return !c->w->oom;
}
static void uw_literal_reach(uw *w, const char *s, size_t n, void *ctx) { (void)ctx; uw_literal(w, s, n); }
static void uw_lua_literals(uw *w, const char *t, size_t n, uw_literal_fn fn, void *ctx)
{
    uw_literals_ctx c = { w, fn, ctx };
    if (!sh_mb_script_literals(t, n, uw_literal_each, &c)) w->oom = 1;
}

typedef struct uw_scan_ctx { uw *w; uw_script *s; } uw_scan_ctx;

static void uw_require_cb(void *context, int what, const char *event, const char *name, size_t len, int line)
{
    uw_scan_ctx *c = (uw_scan_ctx *)context;
    (void)event;
    (void)line;
    if ((what & 0xFF) == SH_MB_SCRIPT_REQUIRE && name && len) {
        char *req = sh_textf("mods/%s/scripts/lib/%.*s.lua", c->w->salt, (int)len, name);
        if (!req) { c->w->oom = 1; return; }
        for (char *p = req; *p; p++) *p = (char)uw_lower((unsigned char)*p);
        uw_reach_request(c->w, req);
        free(req);
    }
}

/* The mod's entityDef files whose key is that of the engine name `name` (n bytes). */
static void uw_defs_named(uw *w, const char *name, size_t n, uw_script *s)
{
    char *k = sh_unit_key(name, n, w->salt, w->salt);
    size_t at;
    if (!k) return;
    at = uw_lower_bound(w->keys, w->nkeys, k);
    for (; at < w->nkeys && !strcmp(w->keys[at].key, k); at++) {
        size_t j = w->keys[at].file, d;
        const sh_mb_file *f = &w->b->files[j];
        int have = 0;
        if (f->stock || !uw_is(f, "entityDef")) continue;
        for (d = 0; d < s->ndefs && !have; d++) have = s->defs[d] == j;
        if (have) continue;
        if (s->ndefs == s->cdefs) {
            size_t cap = s->cdefs ? s->cdefs * 2 : 4;
            size_t *g = (size_t *)realloc(s->defs, cap * sizeof *g);
            if (!g) { w->oom = 1; break; }
            s->defs = g;
            s->cdefs = cap;
        }
        s->defs[s->ndefs++] = j;
    }
    free(k);
}

static void uw_bind_cb(void *context, int what, const char *event, const char *name, size_t len, int line)
{
    uw_scan_ctx *c = (uw_scan_ctx *)context;
    char *cand[4];
    size_t m, i;
    int prefix;
    (void)event;
    (void)line;
    if ((what & 0xFF) != SH_MB_SCRIPT_BIND) return;
    if (what & SH_MB_SCRIPT_DYNAMIC || !name || !len) { c->s->dynamic = 1; return; }
    m = uw_literal_names(c->w, name, len, cand, &prefix);
    for (i = 0; i < m; i++) if (cand[i]) { uw_defs_named(c->w, cand[i], strlen(cand[i]), c->s); free(cand[i]); }
}

/* An entry script's literal that names one of the mod's entityDefs attaches the script to it (a script binds
 * through the SDK's calls as well as through on.<event>). */
static void uw_literal_def(uw *w, const char *s, size_t n, void *ctx)
{
    char *cand[4];
    int prefix;
    size_t m = uw_literal_names(w, s, n, cand, &prefix), i;
    for (i = 0; i < m; i++) if (cand[i]) { uw_defs_named(w, cand[i], strlen(cand[i]), (uw_script *)ctx); free(cand[i]); }
}

/* A literal spelled as a name require takes is a module the script may require by a name it computes or passes on:
 * require(spec.attacks), pcall(require, "x"), require(cfg.x or "x") (R-LV2: the Tyrant's unit lacked
 * eternal_attacks.lua, which its brain spec names as attacks = "eternal_attacks", and the brain stopped). It reaches
 * scripts/lib/<name>.lua when the mod has it, as a require would, by require's own name rule
 * (sh_mb_script_module_name: a name with a '-' is one, R-UC1). */
static void uw_literal_module(uw *w, const char *s, size_t n, void *ctx)
{
    char *req, *p;
    (void)ctx;
    if (n > 200 || !sh_mb_script_module_name(s, n)) return;
    req = sh_textf("mods/%s/scripts/lib/%.*s.lua", w->salt, (int)n, s);
    if (!req) { w->oom = 1; return; }
    for (p = req; *p; p++) *p = (char)uw_lower((unsigned char)*p);
    uw_reach_request(w, req);
    free(req);
}

static void uw_script_visit(uw *w, const unsigned char *t, size_t n)
{
    uw_scan_ctx c = { w, NULL };
    sh_mb_script_scan((const char *)t, n, uw_require_cb, &c);
    uw_lua_literals(w, (const char *)t, n, uw_literal_reach, NULL);
    uw_lua_literals(w, (const char *)t, n, uw_literal_module, NULL);
}

/* A kept entityDef keeps the entry scripts attached to it, the entry scripts attached to none of the mod's defs
 * (one that binds a name only while it runs, or names none: kept with every object), and the enumerated decls
 * that name it. */
static void uw_def(uw *w, size_t i)
{
    size_t s, k;
    for (s = 0; s < w->nscripts; s++) {
        uw_script *sc = &w->scripts[s];
        int hit = !sc->ndefs || sc->dynamic;
        for (k = 0; k < sc->ndefs && !hit; k++) hit = sc->defs[k] == i;
        if (hit) uw_reach(w, sc->file);
    }
    for (s = 0; s < w->nenums; s++)
        for (k = 0; k < w->enums[s].ndefs; k++)
            if (w->enums[s].defs[k] == i) uw_reach(w, w->enums[s].file);
}

/* ------------------------------------------------------------- visiting ---- */

static void uw_visit_bytes(uw *w, size_t i);

static void uw_visit(uw *w, size_t i)
{
    const sh_mb_file *f = &w->b->files[i];
    w->registry = f->cls == SH_MB_CLASS_REPLACE;
    uw_needs(w, f);
    if (f->stock || f->absent) { w->registry = 0; return; }   /* the game's own bytes: its names are the game's */
    uw_visit_bytes(w, i);
    w->registry = 0;
}

static void uw_visit_bytes(uw *w, size_t i)
{
    const sh_mb_file *f = &w->b->files[i];
    size_t len = 0;
    unsigned char *bytes;
    char sha[SH_MB_HASH_HEX + 1], stem[SH_MB_BANK_STEM_MAX + 1];
    if (!(bytes = uw_read(w, f, &len))) { uw_fail(w, "%s could not be read", f->path); return; }
    sh_mb_sha256_hex(bytes, len, sha);
    if (len != f->size || strcmp(sha, f->blob) != 0) {
        free(bytes);
        uw_fail(w, "%s changed since the mod was built", f->path);
        return;
    }
    if (f->is_module || uw_is(f, "snapModuleInfo")) {
        char *own = sh_unit_key(f->request, strlen(f->request), w->salt, w->salt);
        if (own) { uw_reach_below(w, own); free(own); }
    }
    if (f->is_decl || f->is_module) {
        size_t elen = 0;
        char *engine = uw_engine_text(w, bytes, len, &elen);
        uw_strings(w, bytes, len);
        if (engine) {
            uw_scan(w, (const unsigned char *)engine, elen, w->salt);
            if (uw_is(f, "material") || uw_is(f, "material2")) uw_board(w, engine);
            free(engine);
        }
        if (uw_is(f, "sound")) uw_sound(w, f);
        if (uw_is(f, "renderProg")) uw_program(w, f);
        if (uw_is(f, "entityDef")) uw_def(w, i);
    } else if (sh_mb_script_request(f->request, NULL, NULL) > 0) {
        uw_strings(w, bytes, len);
        uw_script_visit(w, bytes, len);
    } else if (sh_mb_bank_stem(f->request, stem, sizeof stem)) {
        uw_bank_decls(w, i);
    } else if (uw_scan(w, bytes, len, w->spell) && w->b->spelled[0]) {
        w->spelled = 1;   /* a binary written with the namespace the build recorded */
    }
    free(bytes);
}

/* The quoted value after `key =` at p (the key a whole word), malloc'd, or NULL. */
static char *uw_value_at(const char *t, const char *p, const char *key)
{
    const char *q = p + strlen(key), *e;
    if ((p > t && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) || isalnum((unsigned char)*q) || *q == '_') return NULL;
    while (*q == ' ' || *q == '\t') q++;
    if (*q++ != '=') return NULL;
    while (*q == ' ' || *q == '\t') q++;
    if (*q++ != '"' || !(e = strchr(q, '"'))) return NULL;
    return sh_textn(q, (size_t)(e - q));
}

/* An enumerated decl names its AI type by number (aiType, an encounter's memberType); a DOOM file the mod changes
 * (the conductor's list) pairs that type with the mod's entityDef in one block. The decl is attached to every def
 * so paired: it travels with that def, not with every object of the mod. */
static void uw_enum_pairs(uw *w, uw_enum *e)
{
    static const char *const keys[] = { "aiType", "memberType", NULL };
    const sh_mb_file *f = &w->b->files[e->file];
    unsigned char *bytes;
    char *text, *types[16];
    size_t len = 0, tl = 0, nt = 0, i, r;
    int k;
    if (!(bytes = uw_read(w, f, &len))) { uw_fail(w, "%s could not be read", f->path); return; }
    text = uw_engine_text(w, bytes, len, &tl);
    free(bytes);
    for (k = 0; text && keys[k]; k++)
        for (const char *p = text; (p = strstr(p, keys[k])) != NULL && nt < 16; p++) {
            char *v = uw_value_at(text, p, keys[k]);
            if (v) types[nt++] = v;
        }
    free(text);
    for (r = 0; r < w->b->file_count && nt && !w->oom; r++) {
        const sh_mb_file *g = &w->b->files[r];
        char *reg;
        size_t rl = 0;
        if (g->cls != SH_MB_CLASS_REPLACE || g->stock || g->absent || !g->is_decl) continue;
        if (!(bytes = uw_read(w, g, &len))) continue;
        reg = uw_engine_text(w, bytes, len, &rl);
        free(bytes);
        for (const char *p = reg; reg && (p = strstr(p, "aiType")) != NULL; p++) {
            char *v = uw_value_at(reg, p, "aiType");
            const char *open = p, *close = p;
            int depth = 0, match = 0;
            for (i = 0; v && i < nt && !match; i++) match = !strcmp(v, types[i]);
            free(v);
            if (!match) continue;
            /* the innermost block around it */
            while (open > reg && !(*open == '{' && depth == 0)) { if (*open == '}') depth++; else if (*open == '{') depth--; open--; }
            depth = 0;
            while (*close && !(*close == '}' && depth == 0)) { if (*close == '{') depth++; else if (*close == '}') depth--; close++; }
            for (const char *q = open; q < close; q++) {
                char *def, *key;
                if (strncmp(q, "entityDef", 9) || !(def = uw_value_at(reg, q, "entityDef"))) continue;
                if ((key = sh_unit_key(def, strlen(def), w->salt, w->salt)) != NULL) {
                    size_t at = uw_lower_bound(w->keys, w->nkeys, key);
                    for (; at < w->nkeys && !strcmp(w->keys[at].key, key); at++) {
                        size_t j = w->keys[at].file, d;
                        int have = 0;
                        for (d = 0; d < e->ndefs && !have; d++) have = e->defs[d] == j;
                        if (!have && !w->b->files[j].stock && uw_is(&w->b->files[j], "entityDef")) e->defs[e->ndefs++] = j;
                    }
                    free(key);
                }
                free(def);
            }
        }
        free(reg);
    }
    for (i = 0; i < nt; i++) free(types[i]);
}

/* Index the files: by key (a board square under its family's key instead) and by request; the scripts' binds and
 * the enumerated decls' defs. */
static int uw_index(uw *w)
{
    const sh_mb_bundle *b = w->b;
    size_t i;
    w->keys = (uw_pair *)calloc(b->file_count + 1, sizeof *w->keys);
    w->reqs = (uw_pair *)calloc(b->file_count + 1, sizeof *w->reqs);
    w->fams = (uw_pair *)calloc(b->file_count + 1, sizeof *w->fams);
    w->owned = (char **)calloc(b->file_count + 1, sizeof *w->owned);
    w->bank = (uw_ids *)calloc(b->file_count + 1, sizeof *w->bank);
    w->scripts = (uw_script *)calloc(b->file_count + 1, sizeof *w->scripts);
    w->enums = (uw_enum *)calloc(b->file_count + 1, sizeof *w->enums);
    if (!w->keys || !w->reqs || !w->fams || !w->owned || !w->bank || !w->scripts || !w->enums) return 0;
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        char *family = NULL, *k;
        w->reqs[w->nreqs].key = f->request;
        w->reqs[w->nreqs++].file = i;
        if (sh_mb_vt_member(f->request, &family, NULL) && family) {
            k = sh_unit_key(family, strlen(family), w->salt, w->salt);
            free(family);
            if (k) { w->owned[i] = k; w->fams[w->nfams].key = k; w->fams[w->nfams++].file = i; }
            continue;
        }
        free(family);
        if (!(k = sh_unit_key(f->request, strlen(f->request), w->salt, w->salt))) continue;
        w->owned[i] = k;
        w->keys[w->nkeys].key = k;
        w->keys[w->nkeys++].file = i;
    }
    qsort(w->keys, w->nkeys, sizeof *w->keys, uw_pair_cmp);
    qsort(w->reqs, w->nreqs, sizeof *w->reqs, uw_pair_cmp);
    qsort(w->fams, w->nfams, sizeof *w->fams, uw_pair_cmp);
    for (i = 0; i < b->file_count && !w->fail && !w->oom; i++) {
        const sh_mb_file *f = &b->files[i];
        unsigned char *bytes;
        size_t len = 0;
        if (f->stock || f->absent) continue;
        if (sh_mb_script_request(f->request, NULL, NULL) == SH_MB_SCRIPT_ENTRY) {
            uw_scan_ctx c;
            if (!(bytes = uw_read(w, f, &len))) { uw_fail(w, "%s could not be read", f->path); break; }
            c.w = w;
            c.s = &w->scripts[w->nscripts++];
            c.s->file = i;
            sh_mb_script_scan((const char *)bytes, len, uw_bind_cb, &c);
            uw_lua_literals(w, (const char *)bytes, len, uw_literal_def, c.s);
            free(bytes);
        } else if (f->is_decl && sh_unit_enumerated_type(f->ptype) && f->cls != SH_MB_CLASS_EDITOR) {
            /* the mod's entityDefs this decl names: its needs, as the build recorded them */
            uw_enum *e = &w->enums[w->nenums++];
            size_t k, j;
            e->file = i;
            e->defs = (size_t *)calloc(b->file_count + 1, sizeof *e->defs);
            if (!e->defs) { w->oom = 1; break; }
            for (k = 0; k < f->need_count; k++) {
                if (_stricmp(f->needs[k].type, "entityDef")) continue;
                for (j = 0; j < b->file_count; j++)
                    if (!b->files[j].stock && uw_is(&b->files[j], "entityDef") && b->files[j].pname &&
                        !_stricmp(b->files[j].pname, f->needs[k].name))
                        e->defs[e->ndefs++] = j;
            }
            if (!e->ndefs) uw_enum_pairs(w, e);
            /* one def once */
            for (k = 0; k < e->ndefs; k++)
                for (j = k + 1; j < e->ndefs;)
                    if (e->defs[j] == e->defs[k]) e->defs[j] = e->defs[--e->ndefs];
                    else j++;
        }
    }
    return !w->oom;
}

/* The mod's Toybox rows whose texts name the def `key`. */
static void uw_rows_of(uw *w, const char *key)
{
    size_t i;
    for (i = 0; i < w->b->file_count && !w->fail; i++) {
        const sh_mb_file *f = &w->b->files[i];
        unsigned char *bytes;
        char *engine;
        size_t len = 0, elen = 0, p, nl = w->sl;
        int hit = 0;
        if (f->absent || f->stock || !uw_is(f, "snapEditorEntityDef") || w->keep[i]) continue;
        if (!(bytes = uw_read(w, f, &len))) { uw_fail(w, "%s could not be read", f->path); return; }
        engine = uw_engine_text(w, bytes, len, &elen);
        free(bytes);
        for (p = 0; engine && !hit && p + 6 + nl <= elen; p++) {
            char *k;
            if (!uw_ns_at(engine + p, elen - p, w->salt, nl)) continue;
            k = uw_key_run(engine + p, elen - p, nl, w->salt);
            hit = k && !strcmp(k, key);
            free(k);
        }
        free(engine);
        if (hit) uw_reach(w, i);
    }
}

/* The request or name `s` with its first mods/<x>/ read as mods/<salt>/, lower case, malloc'd. */
static char *uw_own_ns(const uw *w, const char *s)
{
    const char *p = s;
    char *out;
    for (; p && *p; p++) {
        const char *seg, *end;
        if ((p != s && p[-1] != '/') || _strnicmp(p, "mods/", 5)) continue;
        seg = p + 5;
        end = strchr(seg, '/');
        if (!end || !sh_salt_valid(seg, (size_t)(end - seg))) continue;
        out = sh_textf("%.*smods/%s%s", (int)(p - s), s, w->salt, end);
        if (out) for (char *c = out; *c; c++) *c = (char)uw_lower((unsigned char)*c);
        return out;
    }
    out = sh_text(s);
    if (out) for (char *c = out; *c; c++) *c = (char)uw_lower((unsigned char)*c);
    return out;
}

/* Whether a kept file is (or provides) `by`. */
static int uw_holds(const uw *w, const char *by)
{
    size_t i;
    for (i = 0; i < w->b->file_count; i++) {
        const sh_mb_file *f = &w->b->files[i];
        if (w->keep[i] && (!_stricmp(f->request, by) || (f->pname && !_stricmp(f->pname, by)))) return 1;
    }
    return 0;
}

static void uw_drain(uw *w)
{
    for (;;) {
        size_t before;
        while (!w->fail && !w->oom && w->qhead < w->qtail) {
            w->current = (long)w->queue[w->qhead];
            uw_visit(w, w->queue[w->qhead++]);
        }
        w->current = -1;
        if (w->fail || w->oom) return;
        before = w->qtail;
        uw_banks(w);
        if (w->qtail == before) return;
    }
}

/* ------------------------------------------------------------- manifest ---- */

typedef struct ub { char *p; size_t n, cap; int oom; } ub;

static void ub_put(ub *o, const char *s, size_t n)
{
    if (o->oom) return;
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 4096;
        char *g;
        while (cap < o->n + n + 1) cap *= 2;
        if (!(g = (char *)realloc(o->p, cap))) { o->oom = 1; return; }
        o->p = g;
        o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}
static void ub_puts(ub *o, const char *s) { ub_put(o, s, strlen(s)); }

/* A JSON string of the n bytes at s (UTF-8), escaping only '"', '\' and control characters, as the build's
 * canonical manifest does. */
static void ub_str(ub *o, const char *s, size_t n)
{
    size_t i;
    ub_put(o, "\"", 1);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; ub_put(o, esc, 2); }
        else if (c < 0x20) {
            const char *named = c == '\b' ? "\\b" : c == '\f' ? "\\f" : c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : NULL;
            if (named) ub_puts(o, named);
            else { _snprintf_s(esc, sizeof esc, _TRUNCATE, "\\u%04x", c); ub_puts(o, esc); }
        } else ub_put(o, (const char *)&c, 1);
    }
    ub_put(o, "\"", 1);
}
static void ub_cstr(ub *o, const char *s) { ub_str(o, s, strlen(s)); }

static unsigned ub_hex4(const char *s)
{
    char four[5];
    memcpy(four, s, 4);
    four[4] = 0;
    return (unsigned)strtoul(four, NULL, 16);
}

/* The UTF-8 bytes an escaped JSON string body stands for (mp_json keeps bodies escaped), malloc'd. */
static char *ub_unescape(const char *body, size_t *out_len)
{
    size_t n = strlen(body), i, o = 0;
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        unsigned cp;
        if (body[i] != '\\' || i + 1 >= n) { out[o++] = body[i]; continue; }
        switch (body[++i]) {
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u':
            if (i + 4 >= n) { out[o++] = 'u'; break; }
            cp = ub_hex4(body + i + 1);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < n && body[i + 1] == '\\' && body[i + 2] == 'u') {
                unsigned lo = ub_hex4(body + i + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
            }
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) {
                out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        default: out[o++] = body[i]; break;
        }
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

static void ub_body(ub *o, const char *body)
{
    size_t n = 0;
    char *raw = ub_unescape(body ? body : "", &n);
    if (!raw) { o->oom = 1; return; }
    ub_str(o, raw, n);
    free(raw);
}

typedef struct ub_key { char *raw; size_t n; int at; } ub_key;
static int ub_key_cmp(const void *a, const void *b)
{
    const ub_key *x = (const ub_key *)a, *y = (const ub_key *)b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = memcmp(x->raw, y->raw, n);
    return c ? c : (x->n < y->n ? -1 : x->n > y->n);
}

/* The canonical text of a parsed value: keys sorted by their bytes, strings re-escaped. */
static void ub_value(ub *o, const mpj *v)
{
    int i;
    if (!v) { ub_puts(o, "null"); return; }
    switch (v->type) {
    case MPJ_NULL: ub_puts(o, "null"); break;
    case MPJ_BOOL: ub_puts(o, v->b ? "true" : "false"); break;
    case MPJ_NUM: ub_puts(o, v->text ? v->text : "0"); break;
    case MPJ_STR: ub_body(o, v->text); break;
    case MPJ_ARR:
        ub_put(o, "[", 1);
        for (i = 0; i < v->n; i++) { if (i) ub_put(o, ",", 1); ub_value(o, v->items[i]); }
        ub_put(o, "]", 1);
        break;
    case MPJ_OBJ: {
        ub_key *k = (ub_key *)calloc((size_t)v->n + 1, sizeof *k);
        if (!k) { o->oom = 1; return; }
        for (i = 0; i < v->n; i++) { k[i].raw = ub_unescape(v->keys[i], &k[i].n); k[i].at = i; if (!k[i].raw) o->oom = 1; }
        if (!o->oom) qsort(k, (size_t)v->n, sizeof *k, ub_key_cmp);
        ub_put(o, "{", 1);
        for (i = 0; i < v->n && !o->oom; i++) {
            if (i) ub_put(o, ",", 1);
            ub_str(o, k[i].raw, k[i].n);
            ub_put(o, ":", 1);
            ub_value(o, v->items[k[i].at]);
        }
        ub_put(o, "}", 1);
        for (i = 0; i < v->n; i++) free(k[i].raw);
        free(k);
        break;
    }
    }
}

/* Whether an author name (a key or entry of mod.json) names one of the unit's kept files. */
static int uw_names_kept(const uw *w, const char *body)
{
    size_t n = 0, i;
    char *raw = ub_unescape(body ? body : "", &n), *engine;
    int hit = 0;
    if (!raw) return 0;
    engine = sh_ma_engine_name(w->salt, raw, n);
    for (i = 0; i < w->b->file_count && !hit; i++) {
        const sh_mb_file *f = &w->b->files[i];
        if (!w->keep[i] || !f->pname) continue;
        hit = !_stricmp(f->pname, engine ? engine : raw);
    }
    free(engine);
    free(raw);
    return hit;
}

/* The unit's share of the author's mod.json (modunit.h): only the keys the player's build and serving read
 * (requirements, strings, hud, companions, scenes, animation_events, attachment_tags), cut to the kept files where
 * entries name defs. Any other key changes nothing served, so it is left out and never moves the unit's hash. */
static void uw_mod_share(uw *w, ub *o)
{
    static const char *const presentation[] = { "name", "description", "author", "homepage", SH_MB_BLOCK_KEY, NULL };
    char *path = sh_textf("%s\\mod.json", w->folder);
    unsigned char *text;
    size_t len = 0;
    mpj *root, *share;
    int i, k, j;
    if (!path) { w->oom = 1; return; }
    text = sh_mb_read_file(path, (size_t)-1, &len);
    free(path);
    if (!text) { uw_fail(w, "mod.json could not be read", NULL); return; }
    root = mpj_parse((const char *)text, len);
    free(text);
    if (!root || root->type != MPJ_OBJ) { mpj_free(root); uw_fail(w, "mod.json is not a JSON object", NULL); return; }
    share = mpj_new(MPJ_OBJ);
    for (i = 0; share && i < root->n; i++) {
        const char *key = root->keys[i];
        mpj *v = root->items[i], *kept = NULL;
        int skip = 0;
        for (k = 0; presentation[k] && !skip; k++) skip = !strcmp(key, presentation[k]);
        if (skip || !v) continue;
        if (!strcmp(key, "strings") && v->type == MPJ_OBJ) {
            kept = mpj_new(MPJ_OBJ);
            for (k = 0; kept && k < v->n; k++) {
                mpj *lang = v->items[k], *keep = NULL;
                if (!lang || lang->type != MPJ_OBJ) continue;
                keep = mpj_new(MPJ_OBJ);
                for (j = 0; keep && j < lang->n; j++) {
                    size_t rn = 0;
                    char *raw = ub_unescape(lang->keys[j], &rn);
                    int want = raw && uw_list_has(&w->strings, raw);
                    free(raw);
                    if (want && !mpj_set(keep, lang->keys[j], mpj_clone(lang->items[j]))) w->oom = 1;
                }
                if (keep && keep->n) { if (!mpj_set(kept, v->keys[k], keep)) w->oom = 1; }
                else mpj_free(keep);
            }
        } else if ((!strcmp(key, "hud") || !strcmp(key, "companions") || !strcmp(key, "scenes")) && v->type == MPJ_OBJ) {
            kept = mpj_new(MPJ_OBJ);
            for (k = 0; kept && k < v->n; k++)
                if (uw_names_kept(w, v->keys[k]) && !mpj_set(kept, v->keys[k], mpj_clone(v->items[k]))) w->oom = 1;
        } else if ((!strcmp(key, "animation_events") || !strcmp(key, "attachment_tags")) && v->type == MPJ_ARR) {
            kept = mpj_new(MPJ_ARR);
            for (k = 0; kept && k < v->n; k++)
                if (v->items[k] && v->items[k]->type == MPJ_STR && uw_names_kept(w, v->items[k]->text) &&
                    !mpj_push(kept, mpj_clone(v->items[k])))
                    w->oom = 1;
        } else if (!strcmp(key, "requirements")) {
            kept = mpj_clone(v);   /* mod-wide: whole */
        } else {
            continue;   /* a key nothing serves ("version", a note): never part of the unit's identity (D-A16) */
        }
        if (!kept) { w->oom = 1; continue; }
        if ((kept->type == MPJ_OBJ || kept->type == MPJ_ARR) && !kept->n && v->n) { mpj_free(kept); continue; }
        if (!mpj_set(share, key, kept)) w->oom = 1;
    }
    if (!share) w->oom = 1;
    else ub_value(o, share);
    mpj_free(share);
    mpj_free(root);
}

static int uw_ptr_cmp(const void *a, const void *b)
{
    const uw_ptr *x = (const uw_ptr *)a, *y = (const uw_ptr *)b;
    int c = strcmp(x->path, y->path);
    return c ? c : strcmp(x->at ? x->at : "", y->at ? y->at : "");
}

static const char *uw_kind_word(int kind)
{
    return kind == SH_UNIT_TOYBOX ? "toybox" : kind == SH_UNIT_MODULE ? "module" : kind == SH_UNIT_MOD ? "mod" : "entity";
}

static void uw_manifest(uw *w, sh_unit *u)
{
    ub o = { 0 };
    size_t i;
    int first = 1;
    /* the build's pointers the unit keeps, with where DOOM holds them */
    for (i = 0; i < w->b->file_count && !w->fail; i++) {
        const sh_mb_file *f = &w->b->files[i];
        char where[32] = "";
        const char *source = f->stock_source ? f->stock_source : f->request;
        if (!w->keep[i] || !f->stock) continue;
        if (!sh_mb_stock_container(source, f->blob, where, sizeof where)) {
            char sha[SH_MB_HASH_HEX + 1];
            if (w->env->game(w->env->game_context, source, sha, where, sizeof where) != 1) {
                uw_fail(w, "the game's files cannot be read to place %s", f->path);
                break;
            }
        }
        /* "at": where the mod's folder holds DOOM's bytes (a borrow, a surface, an unchanged native/ file); a pointer
         * the build derives by itself (the campaign closure) has no file and no "at" */
        uw_pointer(w, f->absent ? NULL : f->path, source, f->blob, where);
    }
    if (w->fail || w->oom) return;
    qsort(w->ptrs, w->nptrs, sizeof *w->ptrs, uw_ptr_cmp);
    ub_puts(&o, "{\"files\":[");
    for (i = 0; i < w->b->file_count; i++) {
        const sh_mb_file *f = &w->b->files[i];
        char num[32];
        if (!w->keep[i] || f->stock || f->absent) continue;
        if (!first) ub_put(&o, ",", 1);
        first = 0;
        ub_puts(&o, "{\"path\":");
        ub_cstr(&o, f->path);
        ub_puts(&o, ",\"sha256\":");
        ub_cstr(&o, f->blob);
        _snprintf_s(num, sizeof num, _TRUNCATE, ",\"size\":%llu}", f->size);
        ub_puts(&o, num);
        u->files++;
        u->bytes += f->size;
    }
    ub_puts(&o, "],\"format\":1,\"game\":[");
    for (i = 0; i < w->nptrs; i++) {
        if (i) ub_put(&o, ",", 1);
        ub_put(&o, "{", 1);
        if (w->ptrs[i].at) { ub_puts(&o, "\"at\":"); ub_cstr(&o, w->ptrs[i].at); ub_put(&o, ",", 1); }
        ub_puts(&o, "\"path\":");
        ub_cstr(&o, w->ptrs[i].path);
        ub_puts(&o, ",\"sha256\":");
        ub_cstr(&o, w->ptrs[i].sha);
        ub_puts(&o, ",\"source\":");
        ub_cstr(&o, w->ptrs[i].source);
        ub_put(&o, "}", 1);
    }
    u->pointers = w->nptrs;
    ub_puts(&o, "],\"mod\":");
    uw_mod_share(w, &o);
    ub_puts(&o, ",\"root\":{\"kind\":");
    ub_cstr(&o, uw_kind_word(u->kind));
    ub_puts(&o, ",\"name\":");
    ub_cstr(&o, u->root);
    ub_put(&o, "}", 1);
    if (w->spelled && w->spell[0]) {
        ub_puts(&o, ",\"spelled\":");
        ub_cstr(&o, w->spell);
        strcpy_s(u->spelled, sizeof u->spelled, w->spell);
    }
    ub_put(&o, "}", 1);
    if (o.oom || w->oom) { free(o.p); w->oom = 1; return; }
    u->manifest = o.p;
    u->manifest_len = o.n;
    sh_mb_sha256_hex(o.p, o.n, u->hash);
}

/* ---------------------------------------------------------------- proof ---- */

/* Every need a kept file resolves inside the build resolves inside the unit. */
static void uw_prove(uw *w)
{
    size_t i, k;
    for (i = 0; i < w->b->file_count && !w->fail; i++) {
        const sh_mb_file *f = &w->b->files[i];
        if (!w->keep[i]) continue;
        for (k = 0; k < f->need_count && !w->fail; k++) {
            char *req[4];
            int n = sh_mb_need_requests(f->needs[k].type, f->needs[k].name, req, 4), r, in_build = 0, in_unit = 0;
            for (r = 0; r < n; r++) {
                size_t at = uw_lower_bound(w->reqs, w->nreqs, req[r]);
                for (; at < w->nreqs && !strcmp(w->reqs[at].key, req[r]); at++) {
                    const sh_mb_file *g = &w->b->files[w->reqs[at].file];
                    /* a registry's entries name every object: only the placed ones' come along (uw_reach) */
                    if (f->cls == SH_MB_CLASS_REPLACE && !g->stock && uw_is(g, "entityDef")) continue;
                    in_build = 1;
                    if (w->keep[w->reqs[at].file]) in_unit = 1;
                }
            }
            sh_mb_requests_free(req, n);
            if (in_build && !in_unit) {
                char *what = sh_textf("%s needs %s %s, which the unit leaves out", f->path, f->needs[k].type,
                                      f->needs[k].name);
                uw_fail(w, "%s", what);
                free(what);
            }
        }
    }
}

/* ---------------------------------------------------------------- entry ---- */

void sh_unit_free(sh_unit *u)
{
    if (!u) return;
    free(u->root);
    free(u->manifest);
    free(u->keep);
    free(u->from);
    memset(u, 0, sizeof *u);
}

/* The root's file for a placed name: the mod's file that provides it (a module, a Toybox row or an entityDef,
 * in that order), by its engine name, its request, or its author name. */
static long uw_root_file(const uw *w, const char *name, int *kind)
{
    static const struct { const char *type; int kind; } order[] = {
        { "snapModule", SH_UNIT_MODULE }, { "snapEditorEntityDef", SH_UNIT_TOYBOX }, { "entityDef", SH_UNIT_ENTITY } };
    char *engine = sh_ma_engine_name(w->salt, name, strlen(name)), *own = uw_own_ns(w, name);
    size_t i;
    int t;
    long hit = -1;
    for (t = 0; t < 3 && hit < 0; t++)
        for (i = 0; i < w->b->file_count && hit < 0; i++) {
            const sh_mb_file *f = &w->b->files[i];
            if (f->absent || f->cls == SH_MB_CLASS_EDITOR || !uw_is(f, order[t].type)) continue;
            if ((f->pname && ((own && !_stricmp(f->pname, own)) || (engine && !_stricmp(f->pname, engine)))) ||
                (own && !_stricmp(f->request, own)) || !_stricmp(f->path, name)) {
                hit = (long)i;
                *kind = order[t].kind;
            }
        }
    free(engine);
    free(own);
    return hit;
}

int sh_unit_of(const sh_mb_bundle *b, const char *folder, const char *name, const sh_unit_env *env, sh_unit *out,
               char **why)
{
    uw w;
    size_t i, k;
    int ok = 0, whole;
    long root = -1;
    if (why) *why = NULL;
    if (out) memset(out, 0, sizeof *out);
    if (!b || !folder || !name || !env || !env->game || !out || !sh_salt_valid(b->id, strlen(b->id))) {
        if (why) *why = sh_text("no mod or no name was given");
        return 0;
    }
    memset(&w, 0, sizeof w);
    w.b = b;
    w.folder = folder;
    w.env = env;
    w.salt = b->id;
    w.sl = strlen(b->id);
    w.spell = b->spelled[0] ? b->spelled : b->id;
    w.current = -1;
    w.keep = (unsigned char *)calloc(b->file_count + 1, 1);
    w.queue = (size_t *)calloc(b->file_count + 1, sizeof *w.queue);
    w.from = (long *)calloc(b->file_count + 1, sizeof *w.from);
    if (!w.keep || !w.queue || !w.from || !uw_index(&w)) { w.oom = 1; goto done; }
    if (w.fail) goto done;

    whole = !strcmp(name, SH_UNIT_WHOLE);
    if (whole) {
        out->kind = SH_UNIT_MOD;
        out->root = sh_text(SH_UNIT_WHOLE);
        for (i = 0; i < b->file_count; i++) if (b->files[i].cls != SH_MB_CLASS_EDITOR) uw_reach(&w, i);
    } else {
        char *rn, *dot;
        if ((root = uw_root_file(&w, name, &out->kind)) < 0) {
            w.fail = sh_textf("%s is none of this mod's objects", name);
            goto done;
        }
        rn = sh_text(b->files[root].path);
        if (rn && (dot = strrchr(rn, '.')) != NULL && !strchr(dot, '/')) *dot = 0;
        out->root = rn;
        uw_reach(&w, (size_t)root);
        if (out->kind == SH_UNIT_ENTITY && w.owned[root]) uw_rows_of(&w, w.owned[root]);
    }
    if (!out->root) { w.oom = 1; goto done; }
    /* What travels wherever the mod is used: the DOOM files it changes, the DOOM files its folder holds unchanged
     * (a surface: DOOM's bytes at DOOM's name, served where the mod is used, as a pointer), and the enumerated decls
     * naming none of its defs. A surface the builder derived from a need (no file in the folder) comes with what
     * needs it. */
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        if (f->absent || f->cls == SH_MB_CLASS_EDITOR) continue;
        if (f->cls == SH_MB_CLASS_REPLACE || f->cls == SH_MB_CLASS_SURFACE) uw_reach(&w, i);
    }
    for (i = 0; i < w.nenums; i++) if (!w.enums[i].ndefs) uw_reach(&w, w.enums[i].file);
    /* The md6Defs mod.json gives DOOM's player body events and tags from (animation_events, attachment_tags):
     * the product adds them to the player of every map that uses the mod. */
    if (b->extra) {
        static const char *const lists[] = { "animation_events", "attachment_tags", NULL };
        mpj *extra = mpj_parse(b->extra, strlen(b->extra));
        int l, e;
        for (l = 0; extra && extra->type == MPJ_OBJ && lists[l]; l++) {
            const mpj *list = mpj_get(extra, lists[l]);
            for (e = 0; list && list->type == MPJ_ARR && e < list->n; e++)
                for (i = 0; list->items[e]->type == MPJ_STR && i < b->file_count; i++)
                    if (uw_is(&b->files[i], "md6Def") && b->files[i].pname && !_stricmp(b->files[i].pname, list->items[e]->text))
                        uw_reach(&w, i);
        }
        mpj_free(extra);
    }
    uw_drain(&w);
    /* What players asked for: with the unit that holds the decl that asked, until nothing more joins. */
    for (;;) {
        size_t before = w.qtail;
        for (k = 0; k < env->asked_count && !w.fail && !w.oom; k++) {
            char *req, *by = NULL;
            if (!env->asked[k].request || !env->asked[k].request[0]) continue;
            if (env->asked[k].by && env->asked[k].by[0]) {
                if (!(by = uw_own_ns(&w, env->asked[k].by))) { w.oom = 1; break; }
                if (!uw_holds(&w, by)) { free(by); continue; }
                free(by);
            }
            if (!(req = uw_own_ns(&w, env->asked[k].request))) { w.oom = 1; break; }
            uw_reach_request(&w, req);
            if (!sh_mb_vt_member(req, NULL, NULL)) uw_reach_name(&w, req, strlen(req));
            free(req);
        }
        uw_drain(&w);
        if (w.qtail == before || w.fail || w.oom) break;
    }
    if (w.fail || w.oom) goto done;
    uw_prove(&w);
    if (w.fail) goto done;
    uw_manifest(&w, out);
    if (w.fail || w.oom) goto done;
    out->keep = w.keep;
    out->from = w.from;
    w.keep = NULL;
    w.from = NULL;
    ok = 1;
done:
    if (!ok) {
        if (why) *why = w.fail ? w.fail : sh_text("out of memory");
        else free(w.fail);
        w.fail = NULL;
        sh_unit_free(out);
    }
    free(w.fail);
    if (w.owned) for (i = 0; i < b->file_count; i++) free(w.owned[i]);
    if (w.bank) for (i = 0; i < b->file_count; i++) free(w.bank[i].v);
    if (w.scripts) for (i = 0; i < w.nscripts; i++) free(w.scripts[i].defs);
    if (w.enums) for (i = 0; i < w.nenums; i++) free(w.enums[i].defs);
    for (i = 0; i < w.nptrs; i++) { free(w.ptrs[i].at); free(w.ptrs[i].path); free(w.ptrs[i].source); }
    free(w.ptrs);
    free(w.owned);
    free(w.bank);
    free(w.scripts);
    free(w.enums);
    free(w.keys);
    free(w.reqs);
    free(w.fams);
    free(w.keep);
    free(w.from);
    free(w.events);
    free(w.queue);
    uw_list_free(&w.looked);
    uw_list_free(&w.strings);
    return ok;
}

/* ---------------------------------------------------------- storing ---- */

static int uw_mkdirs(const wchar_t *path)
{
    wchar_t *p, *copy;
    if (CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    if (!(copy = _wcsdup(path))) return 0;
    p = wcsrchr(copy, L'\\');
    if (p && p > copy) {
        *p = 0;
        if (!uw_mkdirs(copy)) { free(copy); return 0; }
    }
    free(copy);
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int uw_write(const char *path, const void *data, size_t len)
{
    wchar_t *w = sh_mb_wpath(path), *slash;
    HANDLE h;
    DWORD put = 0;
    int ok;
    if (!w) return 0;
    if ((slash = wcsrchr(w, L'\\')) != NULL) { *slash = 0; uw_mkdirs(w); *slash = L'\\'; }
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = len <= 0xFFFFFFFFu && WriteFile(h, data, (DWORD)len, &put, NULL) && put == (DWORD)len;
    CloseHandle(h);
    return ok;
}

int sh_unit_store(const sh_unit *u, const char *content_root, char **why)
{
    char *path, have[SH_MB_HASH_HEX + 1];
    unsigned long long size = 0;
    int ok;
    if (why) *why = NULL;
    if (!u || !u->manifest || !content_root) { if (why) *why = sh_text("no unit"); return 0; }
    if (!(path = sh_textf("%s\\units\\%s.json", content_root, u->hash))) { if (why) *why = sh_text("out of memory"); return 0; }
    ok = (sh_mb_sha256_file(path, &size, have) && !strcmp(have, u->hash)) || uw_write(path, u->manifest, u->manifest_len);
    if (!ok && why) *why = sh_textf("%s could not be written", path);
    free(path);
    return ok;
}

/* --------------------------------------------------------- composing ---- */

static void uw_rmtree(const wchar_t *top)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = sh_wtextf(L"%s\\*", top);
    HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    while (h != INVALID_HANDLE_VALUE) {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
            wchar_t *child = sh_wtextf(L"%s\\%s", top, fd.cFileName);
            if (child) {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    uw_rmtree(child);
                else {
                    SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(child);
                }
                free(child);
            }
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    RemoveDirectoryW(top);
}

/* Merge `from` (a unit's mod share) into `into`: objects by key, recursively; arrays as a union of entries; a scalar
 * keeps the first unit's value (one build: the shares agree). */
static void uw_merge(mpj *into, const mpj *from, int *oom)
{
    int i, k;
    for (i = 0; i < from->n; i++) {
        mpj *have = mpj_get(into, from->keys[i]);
        const mpj *v = from->items[i];
        if (!have) { if (!mpj_set(into, from->keys[i], mpj_clone(v))) *oom = 1; continue; }
        if (have->type == MPJ_OBJ && v->type == MPJ_OBJ) uw_merge(have, v, oom);
        else if (have->type == MPJ_ARR && v->type == MPJ_ARR)
            for (k = 0; k < v->n; k++) {
                int j, dup = 0;
                for (j = 0; j < have->n && !dup; j++) {
                    char *a = mpj_dump(have->items[j], NULL), *c = mpj_dump(v->items[k], NULL);
                    dup = a && c && !strcmp(a, c);
                    free(a);
                    free(c);
                }
                if (!dup && !mpj_push(have, mpj_clone(v->items[k]))) *oom = 1;
            }
    }
}

int sh_unit_compose(const char *const *manifests, const size_t *lens, size_t count, sh_unit_source_fn source,
                    void *source_context, const char *salt, const char *name, const sh_mb_resolver *resolver,
                    const char *dest, sh_mb_bundle *out, char **why)
{
    return sh_unit_compose_ex(manifests, lens, count, source, source_context, NULL, 0, 0, salt, name, resolver, dest, out,
                              why);
}

int sh_unit_compose_ex(const char *const *manifests, const size_t *lens, size_t count, sh_unit_source_fn source,
                       void *source_context, const sh_unit_extra *extras, size_t extra_count, int link,
                       const char *salt, const char *name, const sh_mb_resolver *resolver, const char *dest,
                       sh_mb_bundle *out, char **why)
{
    wchar_t *wdest = NULL;
    mpj *share = NULL, *doc = NULL;
    char *reason = NULL, *modjson = NULL, *one = NULL;
    sh_mb_resolver r;
    size_t u;
    int ok = 0, made = 0, oom = 0;
    if (why) *why = NULL;
    if (out) memset(out, 0, sizeof *out);
    if ((count && (!manifests || !lens)) || (!count && !extra_count) || !source || !salt || !sh_salt_valid(salt, strlen(salt)) ||
        !dest || !out) {
        if (why) *why = sh_text("nothing to compose");
        return 0;
    }
    if (!(wdest = sh_mb_wpath(dest))) { if (why) *why = sh_text("out of memory"); return 0; }
    if (GetFileAttributesW(wdest) != INVALID_FILE_ATTRIBUTES || !uw_mkdirs(wdest)) {
        if (why) *why = sh_textf("its folder %s could not be made", dest);
        free(wdest);
        return 0;
    }
    made = 1;
    share = mpj_new(MPJ_OBJ);
    for (u = 0; u < count && !reason; u++) {
        char hash[SH_MB_HASH_HEX + 1];
        mpj *m = mpj_parse(manifests[u], lens[u]);
        const mpj *files = m ? mpj_get(m, "files") : NULL, *mod = m ? mpj_get(m, "mod") : NULL;
        int i;
        sh_mb_sha256_hex(manifests[u], lens[u], hash);
        if (!files || files->type != MPJ_ARR) { sh_text_set(&reason, "unit %.16s is not a unit manifest", hash); mpj_free(m); break; }
        if (mod && mod->type == MPJ_OBJ && share) uw_merge(share, mod, &oom);
        for (i = 0; i < files->n && !reason; i++) {
            const mpj *p = mpj_get(files->items[i], "path"), *sha = mpj_get(files->items[i], "sha256");
            char *from, *to, have[SH_MB_HASH_HEX + 1];
            unsigned long long size = 0;
            wchar_t *wf, *wt, *slash;
            if (!p || p->type != MPJ_STR || !sha || sha->type != MPJ_STR || strstr(p->text, "..") || strchr(p->text, '\\') ||
                p->text[0] == '/' || strchr(p->text, ':')) {
                sh_text_set(&reason, "unit %.16s names a file at no valid path", hash);
                break;
            }
            to = sh_textf("%s\\%s", dest, p->text);
            if (!to) { oom = 1; break; }
            for (char *c = to; *c; c++) if (*c == '/') *c = '\\';
            if (sh_mb_sha256_file(to, &size, have)) {
                /* One file two units carry. With other bytes (a hand-made or damaged map, or a kept object of an
                 * earlier build, N32), the first unit in object order keeps the path: one log line, no notice. */
                if (strcmp(have, sha->text)) {
                    char *line = sh_textf("MODS: content: two objects of this mod carry %s with other bytes; the first "
                                          "object's file is kept", p->text);
                    backend_log(line ? line : "MODS: content: two objects of this mod carry one file with other bytes");
                    free(line);
                }
                free(to);
                continue;
            }
            if (!(from = source(source_context, p->text))) { sh_text_set(&reason, "%s is not here", p->text); free(to); break; }
            wf = sh_mb_wpath(from);
            wt = sh_mb_wpath(to);
            if ((slash = wt ? wcsrchr(wt, L'\\') : NULL) != NULL) { *slash = 0; uw_mkdirs(wt); *slash = L'\\'; }
            /* A hard link costs no bytes (the cache and the mods folder share a volume); a copy where it cannot be
             * made. Either way the bytes are checked against the unit's SHA-256 (D51). */
            if (!wf || !wt) sh_text_set(&reason, "%s could not be copied", p->text);
            else if (link && CreateHardLinkW(wt, wf, NULL)) {
                if (!sh_mb_sha256_file(to, &size, have) || strcmp(have, sha->text))
                    sh_text_set(&reason, "%s is not the unit's bytes", p->text);
            } else if (!CopyFileW(wf, wt, TRUE)) sh_text_set(&reason, "%s could not be copied", p->text);
            else {
                SetFileAttributesW(wt, FILE_ATTRIBUTE_NORMAL);
                if (!sh_mb_sha256_file(to, &size, have) || strcmp(have, sha->text))
                    sh_text_set(&reason, "%s is not the unit's bytes", p->text);
            }
            free(wf);
            free(wt);
            free(from);
            free(to);
        }
        /* DOOM's bytes where the mod's folder held them: read from the installed game, checked by their hash */
        files = m ? mpj_get(m, "game") : NULL;
        for (i = 0; files && files->type == MPJ_ARR && i < files->n && !reason; i++) {
            const mpj *at = mpj_get(files->items[i], "at"), *p = mpj_get(files->items[i], "path"),
                      *sha = mpj_get(files->items[i], "sha256");
            unsigned char *bytes = NULL;
            size_t blen = 0;
            char *to, *cause = NULL, have[SH_MB_HASH_HEX + 1];
            unsigned long long size = 0;
            if (!at || at->type != MPJ_STR || !p || p->type != MPJ_STR || !sha || sha->type != MPJ_STR) continue;
            if (strstr(at->text, "..") || strchr(at->text, '\\') || at->text[0] == '/' || strchr(at->text, ':')) {
                sh_text_set(&reason, "unit %.16s names a file at no valid path", hash);
                break;
            }
            if (!(to = sh_textf("%s\\%s", dest, at->text))) { oom = 1; break; }
            for (char *c = to; *c; c++) if (*c == '/') *c = '\\';
            if (sh_mb_sha256_file(to, &size, have)) { free(to); continue; }
            if (sh_mb_stock_read(p->text, sha->text, &bytes, &blen, at->text, &cause) != SH_MB_STOCK_OK)
                sh_text_set(&reason, "%s", cause ? cause : "the game's files cannot be read");
            else if (!uw_write(to, bytes, blen)) sh_text_set(&reason, "%s could not be written", at->text);
            if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
            free(cause);
            free(to);
        }
        mpj_free(m);
    }
    /* Files of no unit (stand-ins, D60): written where no unit put a file. */
    for (u = 0; u < extra_count && !reason && !oom; u++) {
        char *to, have[SH_MB_HASH_HEX + 1];
        unsigned long long size = 0;
        wchar_t *wt, *slash;
        if (!extras[u].path || strstr(extras[u].path, "..") || strchr(extras[u].path, '\\') || extras[u].path[0] == '/' ||
            strchr(extras[u].path, ':')) { sh_text_set(&reason, "a stand-in names no valid path"); break; }
        if (!(to = sh_textf("%s\\%s", dest, extras[u].path))) { oom = 1; break; }
        for (char *c = to; *c; c++) if (*c == '/') *c = '\\';
        if (!sh_mb_sha256_file(to, &size, have)) {
            if ((wt = sh_mb_wpath(to)) != NULL && (slash = wcsrchr(wt, L'\\')) != NULL) { *slash = 0; uw_mkdirs(wt); }
            free(wt);
            if (!uw_write(to, extras[u].text, extras[u].len)) sh_text_set(&reason, "%s could not be written", extras[u].path);
        }
        free(to);
    }
    if (reason || oom || !share) goto done;
    if (!mpj_set(share, "name", mpj_new_str(name && name[0] ? name : salt))) { oom = 1; goto done; }
    doc = share;
    share = NULL;
    {
        ub o = { 0 };
        mpj *block = mpj_new(MPJ_OBJ);
        if (!block || !mpj_set(block, "format", mpj_new_int(2)) || !mpj_set(block, "salt", mpj_new_str(salt)) ||
            !mpj_set(doc, SH_MB_BLOCK_KEY, block)) { oom = 1; goto done; }
        ub_value(&o, doc);
        if (o.oom) { free(o.p); oom = 1; goto done; }
        modjson = o.p;
        {
            char *mj = sh_textf("%s\\mod.json", dest);
            int wrote = mj && uw_write(mj, modjson, o.n);
            free(mj);
            if (!wrote) { sh_text_set(&reason, "its mod.json could not be written"); goto done; }
        }
    }
    memset(&r, 0, sizeof r);
    if (resolver) { r.stock = resolver->stock; r.stock_context = resolver->stock_context; }
    if (!sh_mb_build(dest, salt, &r, out)) {
        sh_text_set(&reason, "it does not build on its own: %s", sh_mb_first_cause(out));
        goto done;
    }
    out->units_of = (char **)calloc(count ? count : 1, sizeof *out->units_of);
    if (!out->units_of) { oom = 1; goto done; }
    for (u = 0; u < count; u++) {
        char hash[SH_MB_HASH_HEX + 1];
        sh_mb_sha256_hex(manifests[u], lens[u], hash);
        if (!(out->units_of[out->units_of_count] = sh_text(hash))) { oom = 1; goto done; }
        out->units_of_count++;
    }
    if (!sh_mb_write(out, dest, &one)) { sh_text_set(&reason, "its build could not be saved: %s", one ? one : ""); goto done; }
    ok = 1;
done:
    if (!ok) {
        if (!reason) reason = sh_text("out of memory");
        sh_mb_free(out);
        memset(out, 0, sizeof *out);
        if (made) uw_rmtree(wdest);
    }
    if (why) *why = reason;
    else free(reason);
    mpj_free(share);
    mpj_free(doc);
    free(modjson);
    free(one);
    free(wdest);
    return ok;
}
