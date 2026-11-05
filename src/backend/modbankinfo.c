/* modbankinfo.c -- see modbankinfo.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modbankinfo.h"

#define MBI_MAX_FILE (128u << 20)   /* the largest soundbanksinfo.xml read */

typedef struct mbi_bank { char leaf[SH_MBI_LEAF_MAX]; uint32_t id; int skipped; } mbi_bank;
typedef struct mbi_event { const char *name; uint64_t banks; } mbi_event;

enum { MBI_NEW = 0, MBI_READY = 1, MBI_FAILED = 2 };

static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_state;
static char g_root[MAX_PATH * 4];
static char g_cause[160];
static mbi_bank g_banks[SH_MBI_BANKS_MAX];
static unsigned g_nbanks;
static mbi_event *g_events;
static unsigned g_nevents;
static char *g_names;
static double g_load_ms;

/* ------------------------------------------------------------ pure rules */

static char mbi_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int sh_mbi_snapmap_loads(const char *leaf)
{
    char stem[SH_MBI_LEAF_MAX];
    const char *p, *s;
    size_t n, i;
    if (!leaf || !leaf[0]) return 0;
    for (p = s = leaf; *p; p++) if (*p == '\\' || *p == '/') s = p + 1;
    n = strlen(s);
    if (n >= 4 && !_stricmp(s + n - 4, ".bnk")) n -= 4;
    if (!n || n >= sizeof stem) return 0;
    for (i = 0; i < n; i++) stem[i] = mbi_lc(s[i]);
    stem[n] = 0;
    if (!strcmp(stem, "init") || !strcmp(stem, "doom_initial")) return 1;
    return !strstr(stem, "_sp") && !strstr(stem, "initial");
}

/* ------------------------------------------------------------ the parse */

static const char *mbi_find(const char *p, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    while (p && p + n <= end) {
        const char *q = (const char *)memchr(p, needle[0], (size_t)(end - p) - n + 1);
        if (!q) return NULL;
        if (!memcmp(q, needle, n)) return q;
        p = q + 1;
    }
    return NULL;
}

static void mbi_free_table(void)
{
    free(g_events);
    free(g_names);
    g_events = NULL;
    g_names = NULL;
    g_nevents = 0;
    g_nbanks = 0;
    memset(g_banks, 0, sizeof g_banks);
}

typedef struct mbi_pair { uint32_t name; uint32_t len; int bank; } mbi_pair;
static const char *g_sort_names;
static int mbi_pair_cmp(const void *a, const void *b)
{
    const mbi_pair *x = (const mbi_pair *)a, *y = (const mbi_pair *)b;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(g_sort_names + x->name, g_sort_names + y->name, n);
    if (c) return c;
    return x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
}
static int mbi_bank_cmp(const void *a, const void *b) { return strcmp(((const mbi_bank *)a)->leaf, ((const mbi_bank *)b)->leaf); }

/* Called with g_lock held exclusively. */
static int mbi_parse_locked(const char *xml, size_t len)
{
    const char *p = xml, *end = xml + len;
    mbi_pair *pairs = NULL;
    size_t npairs = 0, cpairs = 0, names_len = 0, names_cap = 0, i, j;
    char *names = NULL;
    int remap[SH_MBI_BANKS_MAX];
    mbi_bank sorted[SH_MBI_BANKS_MAX];
    mbi_free_table();
    while ((p = mbi_find(p, end, "<SoundBank ")) != NULL) {
        const char *bend = mbi_find(p, end, "</SoundBank>"), *tag_end, *id, *path, *path_end, *q, *leaf;
        char lf[SH_MBI_LEAF_MAX];
        unsigned bank, k;
        size_t n;
        if (!bend) bend = end;
        tag_end = (const char *)memchr(p, '>', (size_t)(bend - p));
        id = tag_end ? mbi_find(p, tag_end, " Id=\"") : NULL;
        path = mbi_find(p, bend, "<Path>");
        path_end = path ? mbi_find(path, bend, "</Path>") : NULL;
        if (!tag_end || !id || !path || !path_end) { p = bend; continue; }
        path += 6;
        for (leaf = q = path; q < path_end; q++) if (*q == '\\' || *q == '/') leaf = q + 1;
        n = (size_t)(path_end - leaf);
        if (!n || n >= sizeof lf) { p = bend; continue; }
        for (k = 0; k < n; k++) lf[k] = mbi_lc(leaf[k]);
        lf[n] = 0;
        for (bank = 0; bank < g_nbanks && strcmp(g_banks[bank].leaf, lf); bank++) {}
        if (bank == g_nbanks) {
            if (g_nbanks == SH_MBI_BANKS_MAX) { strcpy_s(g_cause, sizeof g_cause, "more than 64 banks in soundbanksinfo.xml"); goto fail; }
            memcpy(g_banks[bank].leaf, lf, n + 1);
            g_banks[bank].id = (uint32_t)strtoul(id + 5, NULL, 10);
            g_banks[bank].skipped = !sh_mbi_snapmap_loads(lf);
            g_nbanks++;
        }
        for (q = tag_end; (q = mbi_find(q, bend, "<Event Id=\"")) != NULL; ) {
            const char *nm = mbi_find(q, bend, "\" Name=\""), *ne, *gt = (const char *)memchr(q, '>', (size_t)(bend - q));
            q += 11;
            if (!nm || !gt || nm > gt) continue;
            nm += 8;
            ne = (const char *)memchr(nm, '"', (size_t)(gt - nm));
            if (!ne || ne == nm || (size_t)(ne - nm) >= SH_MBI_EVENT_MAX) continue;
            n = (size_t)(ne - nm);
            if (names_len + n + 1 > names_cap) {
                size_t nc = names_cap ? names_cap * 2 : (1u << 18);
                char *nn;
                while (nc < names_len + n + 1) nc *= 2;
                nn = (char *)realloc(names, nc);
                if (!nn) goto oom;
                names = nn;
                names_cap = nc;
            }
            if (npairs == cpairs) {
                size_t nc = cpairs ? cpairs * 2 : 4096;
                mbi_pair *np = (mbi_pair *)realloc(pairs, nc * sizeof *pairs);
                if (!np) goto oom;
                pairs = np;
                cpairs = nc;
            }
            for (k = 0; k < n; k++) names[names_len + k] = mbi_lc(nm[k]);
            names[names_len + n] = 0;
            pairs[npairs].name = (uint32_t)names_len;
            pairs[npairs].len = (uint32_t)n;
            pairs[npairs].bank = (int)bank;
            npairs++;
            names_len += n + 1;
        }
        p = bend;
    }
    if (!g_nbanks) { strcpy_s(g_cause, sizeof g_cause, "soundbanksinfo.xml names no SoundBank with a Path"); goto fail; }
    /* Banks in byte order, so "the first skipped bank" is stable. */
    memcpy(sorted, g_banks, sizeof sorted);
    qsort(sorted, g_nbanks, sizeof *sorted, mbi_bank_cmp);
    for (i = 0; i < g_nbanks; i++)
        for (j = 0; j < g_nbanks; j++) if (!strcmp(g_banks[i].leaf, sorted[j].leaf)) remap[i] = (int)j;
    memcpy(g_banks, sorted, sizeof sorted);
    g_sort_names = names;
    if (npairs > 1) qsort(pairs, npairs, sizeof *pairs, mbi_pair_cmp);
    g_events = (mbi_event *)malloc(sizeof *g_events * (npairs ? npairs : 1));
    if (!g_events) goto oom;
    for (i = 0; i < npairs; i++) {
        uint64_t bit = 1ull << remap[pairs[i].bank];
        if (g_nevents && !strcmp(g_events[g_nevents - 1].name, names + pairs[i].name)) {
            g_events[g_nevents - 1].banks |= bit;
            continue;
        }
        g_events[g_nevents].name = names + pairs[i].name;
        g_events[g_nevents].banks = bit;
        g_nevents++;
    }
    g_names = names;
    free(pairs);
    return 1;
oom:
    strcpy_s(g_cause, sizeof g_cause, "out of memory reading soundbanksinfo.xml");
    free(pairs);
    free(names);
    mbi_free_table();
    return -1;
fail:
    free(pairs);
    free(names);
    mbi_free_table();
    return 0;
}

int sh_mbi_load_text(const char *xml, size_t len)
{
    int r;
    if (!xml) return 0;
    AcquireSRWLockExclusive(&g_lock);
    g_cause[0] = 0;
    r = mbi_parse_locked(xml, len);
    InterlockedExchange(&g_state, r == 1 ? MBI_READY : MBI_FAILED);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

/* ------------------------------------------------------------ the file */

static void mbi_default_root(char *out, size_t cap)
{
    wchar_t exe[MAX_PATH * 2], *slash;
    DWORD n = GetModuleFileNameW(NULL, exe, (DWORD)(sizeof exe / sizeof exe[0]));
    out[0] = 0;
    if (!n || n >= sizeof exe / sizeof exe[0]) return;
    slash = wcsrchr(exe, L'\\');
    if (!slash) return;
    *slash = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, exe, -1, out, (int)cap, NULL, NULL)) out[0] = 0;
}

static char *mbi_read_file(const char *path_utf8, size_t *len)
{
    wchar_t wpath[MAX_PATH * 4];
    HANDLE h;
    LARGE_INTEGER sz;
    char *buf;
    DWORD got = 0;
    size_t at = 0;
    *len = 0;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8, -1, wpath, (int)(sizeof wpath / sizeof *wpath)))
        return NULL;
    h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || (unsigned long long)sz.QuadPart > MBI_MAX_FILE) { CloseHandle(h); return NULL; }
    buf = (char *)malloc((size_t)sz.QuadPart + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    while (at < (size_t)sz.QuadPart) {
        DWORD want = (DWORD)(((size_t)sz.QuadPart - at) > (1u << 24) ? (1u << 24) : ((size_t)sz.QuadPart - at));
        if (!ReadFile(h, buf + at, want, &got, NULL) || !got) { free(buf); CloseHandle(h); return NULL; }
        at += got;
    }
    CloseHandle(h);
    buf[at] = 0;
    *len = at;
    return buf;
}

static double mbi_now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int sh_mbi_ready(void)
{
    LONG s = InterlockedCompareExchange(&g_state, 0, 0);
    char path[MAX_PATH * 4 + 64];
    char *text;
    size_t len = 0;
    double t0;
    if (s != MBI_NEW) return s == MBI_READY;
    AcquireSRWLockExclusive(&g_lock);
    if (InterlockedCompareExchange(&g_state, 0, 0) == MBI_NEW) {
        int r = 0;
        t0 = mbi_now_ms();
        if (!g_root[0]) mbi_default_root(g_root, sizeof g_root);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_root, SH_MBI_INFO_PATH);
        text = mbi_read_file(path, &len);
        g_cause[0] = 0;
        if (!text) _snprintf_s(g_cause, sizeof g_cause, _TRUNCATE, "%s cannot be read", SH_MBI_INFO_PATH);
        else r = mbi_parse_locked(text, len);
        free(text);
        g_load_ms = mbi_now_ms() - t0;
        InterlockedExchange(&g_state, r == 1 ? MBI_READY : MBI_FAILED);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return InterlockedCompareExchange(&g_state, 0, 0) == MBI_READY;
}

void sh_mbi_set_install(const char *doom_root)
{
    AcquireSRWLockExclusive(&g_lock);
    if (doom_root && doom_root[0]) strncpy_s(g_root, sizeof g_root, doom_root, _TRUNCATE);
    else g_root[0] = 0;
    mbi_free_table();
    g_cause[0] = 0;
    InterlockedExchange(&g_state, MBI_NEW);
    ReleaseSRWLockExclusive(&g_lock);
}

/* ------------------------------------------------------------ questions */

static const mbi_event *mbi_lookup(const char *lower)
{
    unsigned lo = 0, hi = g_nevents;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(g_events[mid].name, lower);
        if (!c) return &g_events[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static const mbi_event *mbi_event_of(const char *decl_name, char *event, size_t cap)
{
    char lower[SH_MBI_EVENT_MAX + 8];
    const char *last, *p;
    const mbi_event *e;
    size_t n, i;
    if (!decl_name || !decl_name[0]) return NULL;
    n = strlen(decl_name);
    if (n >= SH_MBI_EVENT_MAX) return NULL;
    for (i = 0; i <= n; i++) lower[i] = mbi_lc(decl_name[i]);
    e = mbi_lookup(lower);
    if (!e) {
        for (last = p = decl_name; *p; p++) if (*p == '/') last = p + 1;
        if (!*last || strlen(last) + 5 >= sizeof lower) return NULL;
        memcpy(lower, "play_", 5);
        for (i = 0; last[i]; i++) lower[5 + i] = mbi_lc(last[i]);
        lower[5 + i] = 0;
        e = mbi_lookup(lower);
    }
    if (e && event && cap) strncpy_s(event, cap, e->name, _TRUNCATE);
    return e;
}

int sh_mbi_decl_event(const char *decl_name, char *event, size_t cap)
{
    int r;
    if (!sh_mbi_ready()) return -1;
    AcquireSRWLockShared(&g_lock);
    r = mbi_event_of(decl_name, event, cap) != NULL;
    ReleaseSRWLockShared(&g_lock);
    return r;
}

int sh_mbi_decl_needs(const char *decl_name, char *event, size_t event_cap, char *bank, size_t bank_cap,
                      uint32_t *bank_id)
{
    const mbi_event *e;
    int r = 0;
    unsigned i;
    if (!sh_mbi_ready()) return -1;
    AcquireSRWLockShared(&g_lock);
    e = mbi_event_of(decl_name, event, event_cap);
    if (e) {
        int loaded = 0, first = -1;
        for (i = 0; i < g_nbanks; i++) {
            if (!(e->banks & (1ull << i))) continue;
            if (!g_banks[i].skipped) loaded = 1;
            else if (first < 0) first = (int)i;
        }
        if (!loaded && first >= 0) {
            r = 1;
            if (bank && bank_cap) strncpy_s(bank, bank_cap, g_banks[first].leaf, _TRUNCATE);
            if (bank_id) *bank_id = g_banks[first].id;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

int sh_mbi_skipped_bank(const char *leaf, uint32_t *bank_id)
{
    unsigned i;
    int r = 0;
    if (!leaf || !leaf[0]) return 0;
    if (!sh_mbi_ready()) return -1;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nbanks; i++)
        if (!_stricmp(g_banks[i].leaf, leaf)) {
            r = g_banks[i].skipped;
            if (r && bank_id) *bank_id = g_banks[i].id;
            break;
        }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

int sh_mbi_bundle_need(const char *decl_name, char *bank, size_t cap)
{
    return sh_mbi_decl_needs(decl_name, NULL, 0, bank, cap, NULL) == 1;
}

void sh_mbi_get_stats(sh_mbi_stats *out)
{
    unsigned i, k;
    LONG s;
    if (!out) return;
    memset(out, 0, sizeof *out);
    AcquireSRWLockShared(&g_lock);
    s = InterlockedCompareExchange(&g_state, 0, 0);
    out->loaded = s == MBI_READY ? 1 : s == MBI_FAILED ? -1 : 0;
    out->banks = g_nbanks;
    out->events = g_nevents;
    out->load_ms = g_load_ms;
    strcpy_s(out->cause, sizeof out->cause, g_cause);
    for (i = 0; i < g_nbanks; i++) if (g_banks[i].skipped) out->skipped++;
    for (k = 0; k < g_nevents; k++) {
        int loaded = 0;
        for (i = 0; i < g_nbanks && !loaded; i++) if ((g_events[k].banks & (1ull << i)) && !g_banks[i].skipped) loaded = 1;
        if (!loaded) out->only_skipped++;
    }
    ReleaseSRWLockShared(&g_lock);
}

/* ------------------------------------------------------------ stock-bank states */

typedef struct mbi_st { char leaf[SH_MBI_LEAF_MAX]; int state, result; double ms; char key[96]; } mbi_st;
static SRWLOCK g_st_lock = SRWLOCK_INIT;
static mbi_st g_st[SH_MBI_STATES_MAX];
static int g_nst;

void sh_mbi_state_set(const char *leaf, int state, int result, double ms, const char *key)
{
    int i;
    if (!leaf || !leaf[0]) return;
    AcquireSRWLockExclusive(&g_st_lock);
    for (i = 0; i < g_nst && _stricmp(g_st[i].leaf, leaf); i++) {}
    if (i == g_nst && g_nst < SH_MBI_STATES_MAX) {
        memset(&g_st[i], 0, sizeof g_st[i]);
        strncpy_s(g_st[i].leaf, sizeof g_st[i].leaf, leaf, _TRUNCATE);
        g_nst++;
    }
    if (i < g_nst) {
        g_st[i].state = state;
        g_st[i].result = result;
        if (ms > 0) g_st[i].ms = ms;
        if (key && key[0] && !g_st[i].key[0]) strncpy_s(g_st[i].key, sizeof g_st[i].key, key, _TRUNCATE);
    }
    ReleaseSRWLockExclusive(&g_st_lock);
}

int sh_mbi_state_get(const char *leaf, int *result, double *ms, char *key, size_t key_cap)
{
    int i, st = SH_MBI_ST_NONE;
    if (result) *result = 0;
    if (ms) *ms = 0;
    if (key && key_cap) key[0] = 0;
    if (!leaf) return st;
    AcquireSRWLockShared(&g_st_lock);
    for (i = 0; i < g_nst; i++)
        if (!_stricmp(g_st[i].leaf, leaf)) {
            st = g_st[i].state;
            if (result) *result = g_st[i].result;
            if (ms) *ms = g_st[i].ms;
            if (key && key_cap) strncpy_s(key, key_cap, g_st[i].key, _TRUNCATE);
            break;
        }
    ReleaseSRWLockShared(&g_st_lock);
    return st;
}

const char *sh_mbi_state_word(int state)
{
    switch (state) {
    case SH_MBI_ST_LOADING: return "loading";
    case SH_MBI_ST_LOADED: return "loaded";
    case SH_MBI_ST_FAILED: return "failed";
    case SH_MBI_ST_REFUSED: return "refused";
    default: return "not loaded";
    }
}

void sh_mbi_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    mbi_free_table();
    g_root[0] = 0;
    g_cause[0] = 0;
    g_load_ms = 0;
    InterlockedExchange(&g_state, MBI_NEW);
    ReleaseSRWLockExclusive(&g_lock);
    AcquireSRWLockExclusive(&g_st_lock);
    memset(g_st, 0, sizeof g_st);
    g_nst = 0;
    ReleaseSRWLockExclusive(&g_st_lock);
}
