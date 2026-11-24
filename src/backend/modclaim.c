/* modclaim.c -- this computer as a publisher at the mod store (modclaim.h). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "modclaim.h"
#include "modkey.h"
#include "modcache.h"
#include "modbundle.h"
#include "mp_json.h"
#include "backend_log.h"
#include "../common/text.h"

#ifndef SH_MODCLAIM_PURE
#include "modstore.h"
#endif

static SRWLOCK g_lock = SRWLOCK_INIT;      /* the records and the key */
static sh_key *g_key;
static char *g_key_folder;
static char *g_name;                      /* the account name the game showed last (NULL: not read yet) */
#define MC_OLD_KEYS 8
static sh_key *g_old[MC_OLD_KEYS];        /* inactive keys read for their claims' secrets */
#define MC_KEY_SECTION "publishing-key"    /* {"offer": "pending"}: the backup offer after the first publish */
static char *g_root;
static ULONGLONG g_deadline;              /* the busy store's waits end here (GetTickCount64), 0 none */

static void mc_logf(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void mc_why(char **why, const char *fmt, ...)
{
    va_list ap;
    if (!why) return;
    free(*why);
    va_start(ap, fmt);
    *why = sh_vtextf(fmt, ap);
    va_end(ap);
}

/* Forget the loaded keys (lock held): the next use reads the active one again. */
int modclaim_signer_loaded(char id[17]);

static void mc_keys_drop(void)
{
    int i;
    if (g_key) { sh_key_free(g_key); g_key = NULL; }
    for (i = 0; i < MC_OLD_KEYS; i++) if (g_old[i]) { sh_key_free(g_old[i]); g_old[i] = NULL; }
}

void modclaim_set_key_folder(const char *folder)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_key_folder);
    g_key_folder = folder ? sh_text(folder) : NULL;
    mc_keys_drop();
    ReleaseSRWLockExclusive(&g_lock);
}

void modclaim_set_root(const char *mods_root)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_root);
    g_root = mods_root ? sh_text(mods_root) : NULL;
    ReleaseSRWLockExclusive(&g_lock);
}

void modclaim_set_budget(unsigned long long ms) { g_deadline = ms ? GetTickCount64() + ms : 0; }

static const char *mc_root(void)
{
    if (g_root) return g_root;
#ifndef SH_MODCLAIM_PURE
    return sh_ms_root();
#else
    return NULL;
#endif
}

/* The key's folder: <data>\publishing beside <data>\mods (malloc'd). */
static char *mc_key_folder(void)
{
    const char *root;
    char *out, *slash;
    if (g_key_folder) return sh_text(g_key_folder);
    if (!(root = mc_root()) || !(out = sh_textf("%s", root))) return NULL;
    slash = strrchr(out, '\\');
    if (!slash) { free(out); return NULL; }
    *slash = 0;
    {
        char *full = sh_textf("%s\\" SH_KEY_DIR, out);
        free(out);
        return full;
    }
}

int modclaim_signer(modnet_signer *out, int *made, char **why)
{
    int ok = 1;
    if (made) *made = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_key) {
        char *folder = mc_key_folder(), *pattern;
        WIN32_FIND_DATAA fd;
        HANDLE h;
        int had = 0;
        if (!folder) { ReleaseSRWLockExclusive(&g_lock); mc_why(why, "the publishing key's folder is not known"); return 0; }
        CreateDirectoryA(folder, NULL);
        pattern = sh_textf("%s\\*.key", folder);
        h = pattern ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) { had = 1; FindClose(h); }
        free(pattern);
        ok = sh_key_load_or_create(folder, &g_key, why);
        if (ok && !had && made) *made = 1;
        if (ok && g_name) (void)sh_key_set_name(g_key, g_name[0] ? g_name : NULL);
        if (ok) mc_logf("MODS: publish: %s the publishing key %.16s", had ? "using" : "made", sh_key_public(g_key));
        free(folder);
    }
    if (ok && out) *out = sh_key_signer(g_key);
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

static mpj *mc_section(const char *section);

/* The id of the key whose secret authorises the claim of `ref`: the record's "key" when it names one, else the
 * active key's (a new claim). */
static void mc_claim_key(const char *ref, char out[17])
{
    mpj *m;
    const mpj *c, *k;
    out[0] = 0;
    AcquireSRWLockShared(&g_lock);
    m = mc_section(MODCLAIM_CLAIMS_SECTION);
    c = m ? mpj_get(m, ref) : NULL;
    k = c ? mpj_get(c, "key") : NULL;
    if (k && k->type == MPJ_STR && strlen(k->text) == 16) memcpy(out, k->text, 17);
    else if (g_key) sh_key_claim_id(g_key, out);
    ReleaseSRWLockShared(&g_lock);
    mpj_free(m);
}

/* The secret of the claim of `ref`, from the key that made the claim (a restore keeps the former key as inactive). A
 * claim whose key is on no file here is claimed anew with the active key. `key_id` gets the id of the key used. */
static int mc_secret(const char *ref, char out[MODNET_HASH_HEX + 1], char key_id[17])
{
    char want[17], have[17];
    int ok = 0, i;
    mc_claim_key(ref, want);
    AcquireSRWLockExclusive(&g_lock);
    if (g_key) {
        sh_key_claim_id(g_key, have);
        if (want[0] && strcmp(want, have)) {
            sh_key *old = NULL;
            for (i = 0; i < MC_OLD_KEYS && g_old[i] && !old; i++) {
                char id[17];
                sh_key_claim_id(g_old[i], id);
                if (!strcmp(id, want)) old = g_old[i];
            }
            if (!old) {
                char *folder = mc_key_folder();
                sh_key *found = NULL;
                if (folder && sh_key_find(folder, want, &found, NULL)) {
                    for (i = 0; i < MC_OLD_KEYS && g_old[i]; i++) {}
                    if (i == MC_OLD_KEYS) { sh_key_free(g_old[0]); memmove(g_old, g_old + 1, sizeof g_old - sizeof g_old[0]); i--; }
                    g_old[i] = old = found;
                }
                free(folder);
            }
            if (old) {
                ok = sh_key_claim_secret(old, ref, out);
                if (ok && key_id) memcpy(key_id, want, 17);
            } else mc_logf("MODS: publish: the key that claimed item '%s' is not on this computer; it is claimed anew", ref);
        }
        if (!ok) {
            ok = sh_key_claim_secret(g_key, ref, out);
            if (ok && key_id) memcpy(key_id, have, 17);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}


static mpj *mc_section(const char *section)
{
    const char *root = mc_root();
    char *text = NULL;
    mpj *m = NULL;
    if (root && sh_mb_record_get(root, section, &text) == 1 && text) m = mpj_parse(text, strlen(text));
    free(text);
    if (m && m->type != MPJ_OBJ) { mpj_free(m); m = NULL; }
    return m ? m : mpj_new(MPJ_OBJ);
}

static int mc_section_save(const char *section, const mpj *m)
{
    const char *root = mc_root();
    size_t len = 0;
    char *text = m ? mpj_dump(m, &len) : NULL;
    int ok = root && text && sh_mb_record_put(root, section, text);
    free(text);
    return ok;
}

static void mc_remove(mpj *obj, const char *key)
{
    int i;
    for (i = 0; obj && obj->type == MPJ_OBJ && i < obj->n; i++) {
        if (strcmp(obj->keys[i], key)) continue;
        mpj_free(obj->items[i]);
        free(obj->keys[i]);
        memmove(&obj->items[i], &obj->items[i + 1], sizeof(mpj *) * (size_t)(obj->n - i - 1));
        memmove(&obj->keys[i], &obj->keys[i + 1], sizeof(char *) * (size_t)(obj->n - i - 1));
        obj->n--;
        return;
    }
}

/* A ref as a record key: plain tokens only (the engine's item ids are), so nothing needs escaping; any length. */
static int mc_ref_ok(const char *ref)
{
    size_t n = ref ? strlen(ref) : 0;
    if (!n || !modnet_valid_ref(ref)) return 0;
    for (; *ref; ref++)
        if (!((*ref >= '0' && *ref <= '9') || (*ref >= 'a' && *ref <= 'z') || (*ref >= 'A' && *ref <= 'Z') || *ref == '-' ||
              *ref == '_' || *ref == '.' || *ref == ':')) return 0;
    return 1;
}

static mpj *mc_units_json(const char *const *units, size_t n)
{
    mpj *a = mpj_new(MPJ_ARR);
    size_t i;
    for (i = 0; a && i < n; i++)
        if (modnet_valid_hash(units[i]) && !mpj_push(a, mpj_new_str(units[i]))) { a = NULL; }
    return a;
}

static long long mc_rev_of(const char *ref)
{
    mpj *m;
    const mpj *c, *r;
    long long rev = 0;
    AcquireSRWLockShared(&g_lock);
    m = mc_section(MODCLAIM_CLAIMS_SECTION);
    ReleaseSRWLockShared(&g_lock);
    c = m ? mpj_get(m, ref) : NULL;
    r = c ? mpj_get(c, "rev") : NULL;
    if (r && mpj_is_num(r)) rev = mpj_int(r);
    mpj_free(m);
    return rev;
}

/* A copy of an entry's "mods" (the namespaces the claim's units serve, for the Mods tab), or a fresh empty array. */
static mpj *mc_mods_copy(const mpj *entry)
{
    const mpj *old = entry ? mpj_get(entry, "mods") : NULL;
    mpj *a = mpj_new(MPJ_ARR);
    int k;
    for (k = 0; a && old && old->type == MPJ_ARR && k < old->n; k++)
        if (old->items[k] && old->items[k]->type == MPJ_STR && !mpj_push(a, mpj_new_str(old->items[k]->text))) a = NULL;
    return a;
}

static void mc_claim_store(const char *ref, long long rev, const char *const *units, size_t n, const char *fp)
{
    mpj *m, *c;
    AcquireSRWLockExclusive(&g_lock);
    m = mc_section(MODCLAIM_CLAIMS_SECTION);
    c = mpj_new(MPJ_OBJ);
    if (m && c && mpj_set(c, "rev", mpj_new_int(rev)) && mpj_set(c, "units", mc_units_json(units, n)) &&
        mpj_set(c, "key", mpj_new_str(fp)) && mpj_set(c, "mods", mc_mods_copy(mpj_get(m, ref))) &&
        mpj_set(c, "at", mpj_new_int((long long)time(NULL))) && mpj_set(m, ref, c)) {
        if (!mc_section_save(MODCLAIM_CLAIMS_SECTION, m)) mc_logf("MODS: publish: the claim of '%s' could not be recorded", ref);
    } else mpj_free(c);
    mpj_free(m);
    ReleaseSRWLockExclusive(&g_lock);
}

void modclaim_set_mods(const char *ref, const char *const *salts, size_t n)
{
    mpj *m, *c, *a;
    size_t i;
    int k, dup;
    if (!mc_ref_ok(ref)) return;
    AcquireSRWLockExclusive(&g_lock);
    m = mc_section(MODCLAIM_CLAIMS_SECTION);
    c = m ? (mpj *)mpj_get(m, ref) : NULL;
    if (m && !c && (c = mpj_new(MPJ_OBJ)) != NULL && !mpj_set(m, ref, c)) c = NULL;
    a = mpj_new(MPJ_ARR);
    for (i = 0; a && i < n; i++) {
        if (!salts[i] || !salts[i][0]) continue;
        for (k = 0, dup = 0; k < a->n && !dup; k++) dup = !strcmp(a->items[k]->text, salts[i]);
        if (!dup && !mpj_push(a, mpj_new_str(salts[i]))) { mpj_free(a); a = NULL; }
    }
    if (c && a && mpj_set(c, "mods", a)) (void)mc_section_save(MODCLAIM_CLAIMS_SECTION, m);
    else mpj_free(a);
    mpj_free(m);
    ReleaseSRWLockExclusive(&g_lock);
}

static void mc_claim_forget(const char *ref)
{
    mpj *m;
    AcquireSRWLockExclusive(&g_lock);
    m = mc_section(MODCLAIM_CLAIMS_SECTION);
    if (m && mpj_get(m, ref)) { mc_remove(m, ref); (void)mc_section_save(MODCLAIM_CLAIMS_SECTION, m); }
    mpj_free(m);
    ReleaseSRWLockExclusive(&g_lock);
}

int modclaim_queue(const char *ref, const char *const *units, size_t n)
{
    mpj *m, *e;
    int ok = 0;
    if (!mc_ref_ok(ref)) return 0;
    AcquireSRWLockExclusive(&g_lock);
    m = mc_section(MODCLAIM_PENDING_SECTION);
    e = mpj_new(MPJ_OBJ);
    if (m && e && mpj_set(e, "do", mpj_new_str(units ? "claim" : "release")) &&
        (!units || mpj_set(e, "units", mc_units_json(units, n))) && mpj_set(m, ref, e)) {
        ok = mc_section_save(MODCLAIM_PENDING_SECTION, m);
    } else mpj_free(e);
    mpj_free(m);
    ReleaseSRWLockExclusive(&g_lock);
    if (ok) mc_logf("MODS: publish: the %s of item '%s' is queued; it is retried at every start", units ? "claim" : "release", ref);
    return ok;
}

static void mc_pending_drop(const char *ref)
{
    mpj *m;
    AcquireSRWLockExclusive(&g_lock);
    m = mc_section(MODCLAIM_PENDING_SECTION);
    if (m && mpj_get(m, ref)) { mc_remove(m, ref); (void)mc_section_save(MODCLAIM_PENDING_SECTION, m); }
    mpj_free(m);
    ReleaseSRWLockExclusive(&g_lock);
}

int modclaim_known(const char *ref)
{
    mpj *c, *p;
    int known;
    if (!ref) return 0;
    AcquireSRWLockShared(&g_lock);
    c = mc_section(MODCLAIM_CLAIMS_SECTION);
    p = mc_section(MODCLAIM_PENDING_SECTION);
    ReleaseSRWLockShared(&g_lock);
    known = (c && mpj_get(c, ref)) || (p && mpj_get(p, ref) && mpj_str_is(mpj_get(mpj_get(p, ref), "do"), "claim"));
    mpj_free(c);
    mpj_free(p);
    return known;
}

/* Every unit the records name, each once. */
static void mc_collect(const mpj *m, char (**out)[MODNET_HASH_HEX + 1], size_t *n, size_t *cap)
{
    int i, k;
    for (i = 0; m && m->type == MPJ_OBJ && i < m->n; i++) {
        const mpj *u = mpj_get(m->items[i], "units");
        for (k = 0; u && u->type == MPJ_ARR && k < u->n; k++) {
            size_t j;
            int seen = 0;
            if (!u->items[k] || u->items[k]->type != MPJ_STR || !modnet_valid_hash(u->items[k]->text)) continue;
            for (j = 0; j < *n && !seen; j++) seen = !strcmp((*out)[j], u->items[k]->text);
            if (seen) continue;
            if (*n == *cap) {
                size_t c2 = *cap ? *cap * 2 : 32;
                char (*g)[MODNET_HASH_HEX + 1] = realloc(*out, c2 * sizeof *g);
                if (!g) return;
                *out = g;
                *cap = c2;
            }
            strcpy_s((*out)[(*n)++], MODNET_HASH_HEX + 1, u->items[k]->text);
        }
    }
}

size_t modclaim_held(char (**out)[MODNET_HASH_HEX + 1])
{
    mpj *c, *p;
    size_t n = 0, cap = 0;
    *out = NULL;
    AcquireSRWLockShared(&g_lock);
    c = mc_section(MODCLAIM_CLAIMS_SECTION);
    p = mc_section(MODCLAIM_PENDING_SECTION);
    ReleaseSRWLockShared(&g_lock);
    mc_collect(c, out, &n, &cap);
    mc_collect(p, out, &n, &cap);
    mpj_free(c);
    mpj_free(p);
    return n;
}

/* A busy answer (429, 503, unreachable) is retried after its Retry-After while the budget lasts. 1 to retry. */
static int mc_wait(const modnet_result *r, int code, unsigned attempt, const modnet_io *io)
{
    ULONGLONG now = GetTickCount64(), wait;
    if (code != MODNET_E_RATE && code != MODNET_E_UNAVAILABLE && code != MODNET_E_TIMEOUT && code != MODNET_E_NET) return 0;
    if (r && r->offline) return 0;
    if (!g_deadline || now >= g_deadline || attempt >= 6) return 0;
    wait = r && r->retry_after_ms ? r->retry_after_ms : 1000ull << attempt;
    if (now + wait > g_deadline) return 0;
    while (wait) {
        DWORD step = wait > 250 ? 250 : (DWORD)wait;
        if (io && io->cancel && *io->cancel) return 0;
        Sleep(step);
        wait -= step;
    }
    return 1;
}

void modclaim_plan_free(modclaim_plan *plan)
{
    if (!plan) return;
    free(plan->units);
    free(plan->state);
    free(plan->bytes);
    memset(plan, 0, sizeof *plan);
}

/* The bytes of a unit's files whose SHA-256 is among `missing`. */
static unsigned long long mc_missing_bytes(const char *manifest, size_t len, const char (*missing)[MODNET_HASH_HEX + 1],
                                           size_t nmissing)
{
    mpj *m = mpj_parse(manifest, len);
    const mpj *files = m ? mpj_get(m, "files") : NULL;
    unsigned long long total = 0;
    int i;
    for (i = 0; files && files->type == MPJ_ARR && i < files->n; i++) {
        const mpj *sha = mpj_get(files->items[i], "sha256"), *size = mpj_get(files->items[i], "size");
        size_t k;
        for (k = 0; sha && sha->type == MPJ_STR && k < nmissing; k++)
            if (!strcmp(sha->text, missing[k])) { total += (unsigned long long)mpj_int(size); break; }
    }
    mpj_free(m);
    return total;
}

int modclaim_plan_units(const char *const *units, size_t n, modclaim_plan *plan, const modnet_io *io, char **why)
{
    modnet_result r;
    modnet_signer signer;
    int code, *states;
    size_t i;
    unsigned attempt = 0;
    memset(plan, 0, sizeof *plan);
    if (!n) return MODNET_OK;
    plan->units = calloc(n, sizeof *plan->units);
    plan->state = (int *)calloc(n, sizeof *plan->state);
    plan->bytes = (unsigned long long *)calloc(n, sizeof *plan->bytes);
    states = (int *)calloc(n, sizeof *states);
    if (!plan->units || !plan->state || !plan->bytes || !states) { free(states); modclaim_plan_free(plan); return MODNET_E_NOMEM; }
    plan->n = n;
    for (i = 0; i < n; i++) strcpy_s(plan->units[i], MODNET_HASH_HEX + 1, units[i]);
    if (!modclaim_signer(&signer, NULL, why)) { free(states); return MODNET_E_KEY; }
    do code = modnet_unit_status(units, n, states, io, &r);
    while (code != MODNET_OK && mc_wait(&r, code, attempt++, io));
    if (code != MODNET_OK) { mc_why(why, "%s", r.message ? r.message : modnet_code_text(code)); free(states); return code; }
    for (i = 0; i < n; i++) {
        size_t missing = 0, mlen = 0;
        unsigned long long mbytes = 0;
        char *manifest;
        if (states[i] == MODNET_UNIT_HELD) { plan->state[i] = MODCLAIM_HELD; continue; }
        if (states[i] == MODNET_UNIT_REMOVED) { plan->state[i] = MODCLAIM_REMOVED; plan->removed++; continue; }
        /* Missing at the store: here whole (its manifest and every file in the cache or a local mod)? */
        if (sh_cache_unit_state(units[i], &missing, &mbytes, NULL) != 1 || !(manifest = sh_cache_manifest(units[i], &mlen))) {
            plan->state[i] = MODCLAIM_ABSENT;
            plan->absent++;
            continue;
        }
        {
            int held = 0;
            char (*need)[MODNET_HASH_HEX + 1] = NULL;
            size_t nneed = 0;
            attempt = 0;
            do code = modnet_begin_unit(units[i], manifest, mlen, &signer, &held, &need, &nneed, io, &r);
            while (code != MODNET_OK && mc_wait(&r, code, attempt++, io));
            if (code == MODNET_E_BLOCKED) { plan->state[i] = MODCLAIM_REMOVED; plan->removed++; free(manifest); continue; }
            if (code != MODNET_OK) {
                mc_why(why, "%s", r.message ? r.message : modnet_code_text(code));
                free(manifest);
                free(states);
                return code;
            }
            plan->state[i] = held ? MODCLAIM_HELD : MODCLAIM_MISSING;
            if (!held) {
                plan->bytes[i] = mc_missing_bytes(manifest, mlen, (const char (*)[MODNET_HASH_HEX + 1])need, nneed);
                plan->missing_bytes += plan->bytes[i];
                plan->missing++;
            }
            free(need);
            free(manifest);
        }
    }
    free(states);
    return MODNET_OK;
}

/* The caller's progress across the plan: each request reports its own bytes; a request that starts again from zero
 * adds the last one's total to the base. */
typedef struct mc_progress { const modnet_io *outer; unsigned long long base, last_done, last_total, total; } mc_progress;
static void mc_progress_fn(void *context, unsigned long long done, unsigned long long total)
{
    mc_progress *p = (mc_progress *)context;
    if (done < p->last_done) p->base += p->last_total ? p->last_total : p->last_done;
    p->last_done = done;
    p->last_total = total;
    if (p->outer && p->outer->progress) {
        unsigned long long sum = p->base + done;
        p->outer->progress(p->outer->context, sum > p->total ? p->total : sum, p->total);
    }
}

int modclaim_upload(const modclaim_plan *plan, const modnet_io *io, char **why)
{
    modnet_signer signer;
    mc_progress prog;
    modnet_io mine;
    size_t i;
    if (!modclaim_signer(&signer, NULL, why)) return MODNET_E_KEY;
    memset(&prog, 0, sizeof prog);
    memset(&mine, 0, sizeof mine);
    prog.outer = io;
    prog.total = plan ? plan->missing_bytes : 0;
    if (io) mine.cancel = io->cancel;
    mine.progress = mc_progress_fn;
    mine.context = &prog;
    io = &mine;
    for (i = 0; plan && i < plan->n; i++) {
        sh_cache_upload_stats st;
        unsigned attempt = 0;
        int code;
        if (plan->state[i] != MODCLAIM_MISSING) continue;
        for (;;) {
            modnet_result r;
            memset(&st, 0, sizeof st);
            memset(&r, 0, sizeof r);
            code = sh_cache_publish_unit(plan->units[i], &signer, io, &st, why);
            if (code == MODNET_OK || !mc_wait(&r, code, attempt++, io)) break;
        }
        if (code != MODNET_OK) return code;
        prog.base += prog.last_total ? prog.last_total : prog.last_done;   /* the unit's last request is done */
        prog.last_done = prog.last_total = 0;
        mc_logf("MODS: publish: unit %.16s uploaded (%.1f MB in %u request(s))", plan->units[i],
                (double)st.bytes_sent / 1048576.0, st.requests);
    }
    return MODNET_OK;
}

int modclaim_claim(const char *ref, const char *const *units, size_t n, char (**removed)[MODNET_HASH_HEX + 1],
                   size_t *nremoved, const modnet_io *io, char **why)
{
    modnet_signer signer;
    modnet_result r;
    char secret[MODNET_HASH_HEX + 1], kid[17];
    long long rev, got = 0;
    int code, uploaded = 0;
    unsigned attempt = 0, stale = 0;
    const char **kept = NULL;   /* the units left once those on no computer are left out */
    size_t nkept = 0;
    if (removed) *removed = NULL;
    if (nremoved) *nremoved = 0;
    if (!mc_ref_ok(ref)) { mc_why(why, "the published item's id is not one Snapmap+ can use"); return MODNET_E_ARG; }
    if (!modclaim_signer(&signer, NULL, why) || !mc_secret(ref, secret, kid)) return MODNET_E_KEY;
    rev = mc_rev_of(ref) + 1;
    for (;;) {
        char (*gone)[MODNET_HASH_HEX + 1] = NULL;
        size_t ngone = 0;
        code = modnet_map_claim(ref, units, n, secret, rev, &signer, &got, &gone, &ngone, io, &r);
        if (code == MODNET_OK) {
            mc_claim_store(ref, got > 0 ? got : rev, units, n, kid);
            mc_pending_drop(ref);
            if (removed) *removed = gone; else free(gone);
            if (nremoved) *nremoved = ngone;
            mc_logf("MODS: publish: item '%s' claims %zu unit(s) (rev %lld)", ref, n, got > 0 ? got : rev);
            break;
        }
        free(gone);
        if (code == MODNET_E_CONFLICT && got >= 0 && stale++ < 2) { rev = got + 1; continue; }   /* retry with the next rev */
        if (code == MODNET_E_CONFLICT && got < 0 && !uploaded) {
            /* a unit the store lacks: send what this computer has, and leave out of the claim what is on no computer
             * and not in the store, since no claim of it could succeed and no later start could send it */
            modclaim_plan plan;
            uploaded = 1;
            if (modclaim_plan_units(units, n, &plan, io, why) == MODNET_OK) {
                int up = modclaim_upload(&plan, io, why);
                size_t i, k, left = 0;
                for (i = 0; i < plan.n; i++)
                    if (plan.state[i] == MODCLAIM_ABSENT || plan.state[i] == MODCLAIM_REMOVED) left++;
                if (up == MODNET_OK && left && !kept) {
                    kept = (const char **)calloc(n ? n : 1, sizeof *kept);
                    for (i = 0; kept && i < n; i++) {
                        int gone = 0;
                        for (k = 0; k < plan.n && !gone; k++)
                            gone = !strcmp(plan.units[k], units[i]) &&
                                   (plan.state[k] == MODCLAIM_ABSENT || plan.state[k] == MODCLAIM_REMOVED);
                        if (!gone) kept[nkept++] = units[i];
                    }
                    if (kept) {
                        mc_logf("MODS: publish: item '%s': %zu unit(s) are on no computer and not in the mod store; the "
                                "claim names the other %zu", ref, n - nkept, nkept);
                        units = kept;
                        n = nkept;
                    }
                }
                modclaim_plan_free(&plan);
                if (up == MODNET_OK) continue;
            }
        }
        if (mc_wait(&r, code, attempt++, io)) continue;
        mc_why(why, "%s", r.message ? r.message : modnet_code_text(code));
        break;
    }
    free((void *)kept);
    SecureZeroMemory(secret, sizeof secret);
    return code;
}

int modclaim_release(const char *ref, const modnet_io *io, char **why)
{
    modnet_signer signer;
    modnet_result r;
    char secret[MODNET_HASH_HEX + 1];
    long long rev, got = 0;
    int code;
    unsigned attempt = 0, stale = 0;
    if (!mc_ref_ok(ref)) return MODNET_OK;
    if (!modclaim_signer(&signer, NULL, why) || !mc_secret(ref, secret, NULL)) return MODNET_E_KEY;
    rev = mc_rev_of(ref) + 1;
    for (;;) {
        code = modnet_map_release(ref, secret, rev, &signer, &got, io, &r);
        if (code == MODNET_OK || code == MODNET_E_NOT_FOUND) {
            mc_claim_forget(ref);
            mc_pending_drop(ref);
            mc_logf("MODS: publish: the claim of item '%s' is released%s", ref, code ? " (the store had none)" : "");
            code = MODNET_OK;
            break;
        }
        if (code == MODNET_E_CONFLICT && got >= 0 && stale++ < 2) { rev = got + 1; continue; }
        if (mc_wait(&r, code, attempt++, io)) continue;
        mc_why(why, "%s", r.message ? r.message : modnet_code_text(code));
        break;
    }
    SecureZeroMemory(secret, sizeof secret);
    return code;
}


static size_t mc_units_of(const mpj *e, const char ***out)
{
    const mpj *u = e ? mpj_get(e, "units") : NULL;
    size_t n = 0;
    int k;
    *out = NULL;
    if (!u || u->type != MPJ_ARR || !u->n || !(*out = (const char **)calloc((size_t)u->n, sizeof **out))) return 0;
    for (k = 0; k < u->n; k++)
        if (u->items[k] && u->items[k]->type == MPJ_STR && modnet_valid_hash(u->items[k]->text)) (*out)[n++] = u->items[k]->text;
    return n;
}

int modclaim_launch(const modnet_io *io, modclaim_launch_stats *out)
{
    mpj *p, *c;
    int i;
    modclaim_launch_stats st;
    memset(&st, 0, sizeof st);
    AcquireSRWLockShared(&g_lock);
    p = mc_section(MODCLAIM_PENDING_SECTION);
    c = mc_section(MODCLAIM_CLAIMS_SECTION);
    ReleaseSRWLockShared(&g_lock);
    if ((!p || !p->n) && (!c || !c->n)) { mpj_free(p); mpj_free(c); if (out) *out = st; return MODNET_OK; }
    /* The queue: claims (their units uploaded first) and releases. */
    for (i = 0; p && i < p->n; i++) {
        const char *ref = p->keys[i], **units = NULL;
        char *why = NULL;
        size_t n = mc_units_of(p->items[i], &units);
        int code;
        st.retried++;
        if (mpj_str_is(mpj_get(p->items[i], "do"), "release")) code = modclaim_release(ref, io, &why);
        else {
            modclaim_plan plan;
            code = modclaim_plan_units(units, n, &plan, io, &why);
            if (code == MODNET_OK && plan.missing) { code = modclaim_upload(&plan, io, &why); st.uploaded += (unsigned)plan.missing; }
            modclaim_plan_free(&plan);
            if (code == MODNET_OK) code = modclaim_claim(ref, units, n, NULL, NULL, io, &why);
        }
        if (code == MODNET_OK) st.settled++;
        else mc_logf("MODS: launch: the queued work of item '%s' stays queued: %s", ref, why ? why : modnet_code_text(code));
        free((void *)units);
        free(why);
    }
    /* Repair: a claim whose units the store lost gets them back from this computer, and is made again. */
    for (i = 0; c && i < c->n; i++) {
        const char *ref = c->keys[i], **units = NULL;
        char *why = NULL;
        size_t n = mc_units_of(c->items[i], &units);
        modclaim_plan plan;
        if (n && modclaim_plan_units(units, n, &plan, io, &why) == MODNET_OK && plan.missing) {
            if (modclaim_upload(&plan, io, &why) == MODNET_OK && modclaim_claim(ref, units, n, NULL, NULL, io, &why) == MODNET_OK) {
                st.repaired++;
                st.uploaded += (unsigned)plan.missing;
                mc_logf("MODS: launch: item '%s' had lost %zu unit(s) at the store; they were sent again", ref, plan.missing);
            }
            modclaim_plan_free(&plan);
        } else if (n) modclaim_plan_free(&plan);
        free((void *)units);
        free(why);
    }
    mpj_free(p);
    mpj_free(c);
    AcquireSRWLockShared(&g_lock);
    p = mc_section(MODCLAIM_PENDING_SECTION);
    ReleaseSRWLockShared(&g_lock);
    st.left = p ? p->n : 0;
    mpj_free(p);
    if (out) *out = st;
    return MODNET_OK;
}


int modclaim_asked(modclaim_asked_fn fn, void *context, const modnet_io *io, char **why)
{
    mpj *c, *answer;
    const char **refs = NULL, **secrets = NULL;
    char (*held)[MODNET_HASH_HEX + 1] = NULL;
    char *json = NULL;
    modnet_signer signer;
    modnet_result r;
    size_t n = 0;
    int i, code;
    AcquireSRWLockShared(&g_lock);
    c = mc_section(MODCLAIM_CLAIMS_SECTION);
    ReleaseSRWLockShared(&g_lock);
    if (!c || !c->n) { mpj_free(c); return MODNET_OK; }
    if (!modclaim_signer(&signer, NULL, why)) { mpj_free(c); return MODNET_E_KEY; }
    refs = (const char **)calloc((size_t)c->n, sizeof *refs);
    secrets = (const char **)calloc((size_t)c->n, sizeof *secrets);
    held = calloc((size_t)c->n, sizeof *held);
    if (!refs || !secrets || !held) { free((void *)refs); free((void *)secrets); free(held); mpj_free(c); return MODNET_E_NOMEM; }
    for (i = 0; i < c->n; i++) {
        if (!mc_secret(c->keys[i], held[n], NULL)) continue;
        refs[n] = c->keys[i];
        secrets[n] = held[n];
        n++;
    }
    code = modnet_asked_units(refs, secrets, n, &signer, &json, io, &r);
    SecureZeroMemory(held, (size_t)c->n * sizeof *held);
    free(held);
    free((void *)refs);
    free((void *)secrets);
    mpj_free(c);
    if (code != MODNET_OK) { mc_why(why, "%s", r.message ? r.message : modnet_code_text(code)); free(json); return code; }
    answer = json ? mpj_parse(json, strlen(json)) : NULL;
    {
        const mpj *list = answer ? mpj_get(answer, "asked") : NULL;
        for (i = 0; list && list->type == MPJ_ARR && i < list->n; i++) {
            const mpj *u = mpj_get(list->items[i], "unit"), *root = mpj_get(list->items[i], "root"),
                      *path = mpj_get(list->items[i], "path");
            if (u && u->type == MPJ_STR && path && path->type == MPJ_STR && fn)
                fn(context, u->text, root && root->type == MPJ_STR ? root->text : "", path->text);
        }
    }
    mpj_free(answer);
    free(json);
    return MODNET_OK;
}


void modclaim_set_account_name(const char *name)
{
    char *folder = NULL;
    int changed;
    AcquireSRWLockExclusive(&g_lock);
    changed = !g_name || strcmp(g_name, name ? name : "");
    if (changed) {
        free(g_name);
        g_name = sh_text(name ? name : "");
        if (g_key) {
            (void)sh_key_set_name(g_key, g_name && g_name[0] ? g_name : NULL);
            folder = mc_key_folder();
            if (folder && !sh_key_save(g_key, folder, NULL)) mc_logf("MODS: publish: the key's account name could not be kept");
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    free(folder);
    if (changed) mc_logf("MODS: publish: the signed-in account name is %s", name && name[0] ? "read" : "empty (signed out)");
}

/* 1 when this computer has a publishing key (none is made here). */
static int mc_key_exists(char id[17])
{
    char *folder, *pattern;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int had = 0;
    id[0] = 0;
    if (modclaim_signer_loaded(id)) return 1;
    folder = mc_key_folder();
    pattern = folder ? sh_textf("%s\\*.key", folder) : NULL;
    h = pattern ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) { had = 1; FindClose(h); }
    free(pattern);
    free(folder);
    if (had && modclaim_signer(NULL, NULL, NULL)) (void)modclaim_signer_loaded(id);
    return had;
}

int modclaim_signer_loaded(char id[17])
{
    int ok;
    AcquireSRWLockShared(&g_lock);
    ok = g_key != NULL;
    if (ok && id) sh_key_claim_id(g_key, id);
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void modclaim_key_offer(int pending)
{
    mpj *m;
    AcquireSRWLockExclusive(&g_lock);
    m = mc_section(MC_KEY_SECTION);
    if (m && mpj_set(m, "offer", mpj_new_str(pending ? "pending" : "done")))
        (void)mc_section_save(MC_KEY_SECTION, m);
    mpj_free(m);
    ReleaseSRWLockExclusive(&g_lock);
}

int modclaim_key_offer_pending(void)
{
    mpj *m;
    const mpj *o;
    int pending;
    AcquireSRWLockShared(&g_lock);
    m = mc_section(MC_KEY_SECTION);
    ReleaseSRWLockShared(&g_lock);
    o = m ? mpj_get(m, "offer") : NULL;
    pending = o && o->type == MPJ_STR && !strcmp(o->text, "pending");
    mpj_free(m);
    return pending;
}

char *modclaim_key_status_json(void)
{
    char id[17];
    int exists = mc_key_exists(id);
    return sh_textf("{\"exists\":%s,\"id\":\"%s\",\"offer\":%s}", exists ? "true" : "false", exists ? id : "",
                    exists && modclaim_key_offer_pending() ? "true" : "false");
}

int modclaim_key_backup(const char *path, const char *passphrase, char **why)
{
    char id[17];
    int ok;
    if (why) *why = NULL;
    if (!mc_key_exists(id)) {
        mc_why(why, "%s", "You have no publishing key yet. Snapmap+ makes one the first time you publish a map with mods.");
        return 0;
    }
    AcquireSRWLockShared(&g_lock);
    ok = g_key && sh_key_backup(g_key, passphrase, path, why);
    ReleaseSRWLockShared(&g_lock);
    if (ok) {
        modclaim_key_offer(0);
        mc_logf("MODS: publish: the publishing key %s was backed up", id);
    }
    return ok;
}

int modclaim_key_restore(const char *path, const char *passphrase, char **why)
{
    sh_key *k = NULL;
    char *folder;
    char id[17];
    int r;
    if (why) *why = NULL;
    r = sh_key_restore(path, passphrase, &k, why);
    if (r != SH_KEY_RESTORE_OK) return r;
    folder = mc_key_folder();
    AcquireSRWLockExclusive(&g_lock);
    if (g_name) (void)sh_key_set_name(k, g_name[0] ? g_name : NULL);
    if (!folder || !sh_key_make_active(k, folder, why)) r = SH_KEY_RESTORE_DAMAGED;
    else mc_keys_drop();   /* the former key stays on file as an inactive key: its claims keep their secrets */
    ReleaseSRWLockExclusive(&g_lock);
    sh_key_claim_id(k, id);
    sh_key_free(k);
    free(folder);
    if (r == SH_KEY_RESTORE_OK) {
        modclaim_key_offer(0);
        mc_logf("MODS: publish: the publishing key %s was restored from a backup and is the active key", id);
    } else if (why && !*why) mc_why(why, "%s", "The restored key could not be kept on this computer.");
    return r;
}

int modclaim_secret_for(const char *ref, char out[MODNET_HASH_HEX + 1], char key_id[17])
{
    if (!modclaim_signer(NULL, NULL, NULL)) return 0;
    return mc_secret(ref, out, key_id);
}

/* A JSON string of `s` with its quotes (malloc'd). */
static char *mc_jstr(const char *s)
{
    size_t n = s ? strlen(s) : 0, i, o = 0;
    char *out = (char *)malloc(n * 6 + 3);
    if (!out) return NULL;
    out[o++] = '"';
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)_snprintf_s(out + o, 7, _TRUNCATE, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = 0;
    return out;
}

static char *mc_key_answer(int ok, const char *message)
{
    char *st = modclaim_key_status_json(), *msg = mc_jstr(message ? message : ""), *out;
    out = st && msg ? sh_textf("{\"ok\":%s,\"message\":%s,\"key\":%s}", ok ? "true" : "false", msg, st) : NULL;
    free(st);
    free(msg);
    return out;
}

char *modclaim_key_action(const char *arg)
{
    const char *a = arg ? arg : "", *t1 = strchr(a, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    char *why = NULL, *path = NULL, *out;
    int ok;
    if (!strcmp(a, "status")) return mc_key_answer(1, "");
    if (!strcmp(a, "later")) { modclaim_key_offer(0); return mc_key_answer(1, ""); }
    if (!t1 || !t2 || !(path = sh_textn(t1 + 1, (size_t)(t2 - t1 - 1)))) return mc_key_answer(0, "Snapmap+ did not understand that request.");
    if (!strncmp(a, "backup\t", 7)) {
        ok = modclaim_key_backup(path, t2 + 1, &why);
        if (ok) {
            const char *leaf = strrchr(path, '\\');
            char *line = sh_textf("Your publishing key is saved in %s. Keep that file and its passphrase somewhere safe: "
                                  "together they bring your key back on any computer.", leaf ? leaf + 1 : path);
            out = mc_key_answer(1, line ? line : "");
            free(line);
        } else out = mc_key_answer(0, why ? why : "The backup was not written.");
    } else if (!strncmp(a, "restore\t", 8)) {
        ok = modclaim_key_restore(path, t2 + 1, &why) == SH_KEY_RESTORE_OK;
        out = mc_key_answer(ok, ok ? "Snapmap+ now publishes with the key from this backup. Your earlier key stays on "
                                     "this computer, so the maps it published can still be republished and unpublished."
                                   : why ? why : "This file is not a Snapmap+ publishing key backup.");
    } else out = mc_key_answer(0, "Snapmap+ did not understand that request.");
    free(why);
    free(path);
    return out;
}
