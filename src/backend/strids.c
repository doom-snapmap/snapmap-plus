/* Inject bundle and baked #str_ mappings, then sort the live
 * dictionary by hash. Installation performs the usual injection; the sort
 * hook is a fallback.
 *
 * Native 32-byte record: +0x00 lowercased FNV-1a hash, +0x04 padding, +0x08
 * interned key handle, +0x10 interned value handle, and value lengths at
 * +0x18/+0x1c. The engine owns the string pool and destination list.
 */
#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strids.h"
#include "strids_baked.h"   /* the compiled-in canonical #str_ set (baked strids) */
#include "hook.h"
#include "backend_log.h"
#include "config_json.h"
#include "modserve.h"   /* sh_modserve_each_bundle_extra: the bundle string layer */
#include "strkeys.h"    /* string edits: keys the game or the product defines (DECISIONS 9) */
#include "process_heap_scope.h"   /* a runtime pass grows the table in the process heap */

/* StridsSortBody prologue steal window (DIRECT, disasm of 0x1a2b490):
 *   48 89 5C 24 18        mov [rsp+0x18],rbx        (5)
 *   55                    push rbp                  (1)
 *   56                    push rsi                  (1)
 *   57                    push rdi                  (1)
 *   48 8D AC 24 10 F8..   lea rbp,[rsp-0x7f0]       (8)  -- rsp-relative, position-independent
 * = 16 bytes of whole, register/rsp-only, position-independent instructions (no RIP-rel, no rel jmp). */
#define SORT_STOLEN 16

/* Native sort ABI: void sort(ctx, arr, uint32 count, uint32 radix). */
typedef void (*sort_fn_t)(void *ctx, void *arr, uint32_t count, uint32_t radix);

/* The native wrapper at pinned Vulkan RVA 0x1a2b480 sets radix=0x20 before
 * entering the body, which sorts the 32-bit hash eight bits at a time.
 */
#define SORT_RADIX 0x20

/* The engine fns the inject calls. All resolved by the signature scanner; never hardcoded RVAs. */
typedef int   (*insert_fn_t)(void *table_desc, void *record32);   /* idList<StridEntry>::Append */
typedef uint32_t (*hash_fn_t)(const char *s);                     /* idStr::Hash (FNV-1a, lowercased) */
typedef void  (*idstr_ctor_fn_t)(void *out_handle, const char *s);/* *out_handle = pool_intern(s) */

static sort_fn_t       g_sort_orig   = NULL;   /* trampoline -> the real engine sort body */
static void           *g_table_desc  = NULL;   /* the idLangDict table descriptor (.data global) */
static insert_fn_t     g_insert      = NULL;
static hash_fn_t       g_hash        = NULL;
static idstr_ctor_fn_t g_idstr_ctor  = NULL;

static volatile LONG g_refresh_busy;
static volatile LONG g_refresh_pending;   /* a serving index was published since the last pass */
static int g_pass_failed;
static volatile LONG g_injected      = 0;      /* one-shot latch (0 = not yet injected) */
static volatile LONG g_in_sort       = 0;      /* recursion guard (>0 = inside the sort already) */
static volatile LONG g_inject_count  = 0;      /* rows appended (observability) */
static int g_table_changed;                     /* the last pass wrote to the live table */

/* Decode the first LEA RCX,[rip+disp32] in StridsTableLea to find the
 * dictionary descriptor. A later matching LEA references the error string, so
 * it must not be selected.
 */
#define LEA_SCAN_WINDOW 0x40

static int safe_read_n(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void *decode_table_global(const uint8_t *table_lea_fn)
{
    uint8_t b[LEA_SCAN_WINDOW];
    if (!safe_read_n(table_lea_fn, b, sizeof b)) return NULL;
    for (int i = 0; i + 7 <= LEA_SCAN_WINDOW; i++) {
        if (b[i] == 0x48 && b[i + 1] == 0x8D && b[i + 2] == 0x0D) {     /* LEA RCX,[rip+disp32] */
            int32_t disp;
            memcpy(&disp, &b[i + 3], 4);
            const uint8_t *rip_next = table_lea_fn + i + 7;
            return (void *)(rip_next + disp);
        }
    }
    return NULL;
}

/* Deduplicate case-insensitively within an injection pass because the native
 * hash lowercases keys. First writer wins: admitted bundles in modserve's
 * order, then baked defaults. Duplicate native rows would make hash lookup
 * ambiguous. (The user document of earlier releases, strings\strids.json, is
 * the store tool's to move into the mod My overrides; it is not read here.)
 */
/* Who supplied a row. The baked defaults are not a bundle and never
 * conflict-report: the baked set is only a backstop. A bundle owner is its index in this pass's
 * g_bundle_names, so a cross-bundle disagreement can name both sides. */
#define STRIDS_OWNER_NONE (-1)

enum { STRIDS_LAYER_BUNDLE, STRIDS_LAYER_BAKED, STRIDS_LAYERS };

typedef struct strids_row {
    const char  *id;          /* borrows the stable owned ID allocation */
    unsigned int text_hash;   /* FNV-1a of the text: "same value?" without storing every value */
    int          owner;       /* STRIDS_OWNER_NONE or a bundle of this pass */
    int          layer;       /* STRIDS_LAYER_* */
} strids_row;

/* The salt of each bundle that supplied strings in this pass. */
static char **g_bundle_names;
static size_t g_bundle_names_n, g_bundle_names_capacity;
static size_t g_layer_rows[STRIDS_LAYERS];

static strids_row *g_injected_ids;
static size_t g_injected_n, g_injected_capacity;

typedef struct strids_owned_row {
    char *id;
    void *key_handle;
    unsigned int hash;
    char *text;
    unsigned char original[32]; /* borrowed native pointers, restored on retirement */
    int replaced;
} strids_owned_row;

/* Native sorting moves rows, so retain the interned key, never an array
 * index or pointer. Existing keys update in place during main-thread rearm. */
static strids_owned_row *g_owned_rows;
static size_t g_owned_n, g_owned_capacity;
typedef struct strids_index_row {
    uint32_t hash, index;
    const char *key;
} strids_index_row;
static strids_index_row *g_index;
static size_t g_index_n, g_index_capacity;

static int strids_reserve(void **rows, size_t *capacity, size_t count, size_t stride)
{
    size_t next;
    void *grown;
    if (count <= *capacity) return 1;
    if (count > SIZE_MAX / stride) return 0;
    next = *capacity ? *capacity : 32;
    while (next < count) {
        if (next > SIZE_MAX / stride / 2) { next = count; break; }
        next *= 2;
    }
    grown = realloc(*rows, next * stride);
    if (!grown) return 0;
    *rows = grown; *capacity = next;
    return 1;
}

/* idList uses signed 32-bit counts and capacity; this is a native layout
 * check, not an application quota. The sort ABI receives the same count. */
static int strids_table(unsigned char **array, uint32_t *count)
{
    uint32_t capacity;
    *array = *(unsigned char **)g_table_desc;
    *count = *(uint32_t *)((unsigned char *)g_table_desc + 8);
    capacity = *(uint32_t *)((unsigned char *)g_table_desc + 12);
    return *count <= capacity && capacity <= INT_MAX && (!*count || *array);
}

static int strids_index_compare(const void *a, const void *b)
{
    const strids_index_row *x = a, *y = b;
    return x->hash < y->hash ? -1 : x->hash > y->hash;
}

static int strids_index_begin(void)
{
    unsigned char *array;
    uint32_t count, i;
    uintptr_t cursor, end;
    if (!strids_table(&array, &count) || (size_t)count > SIZE_MAX / 32u) return 0;
    cursor = (uintptr_t)array;
    if ((size_t)count * 32u > UINTPTR_MAX - cursor) return 0;
    end = cursor + (size_t)count * 32u;
    /* A claimed native extent is not proof of an allocated table. Check the
     * entire readable range before indexing any entries. */
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region;
        uintptr_t next;
        if (!VirtualQuery((void *)cursor, &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
            region.RegionSize > UINTPTR_MAX - (uintptr_t)region.BaseAddress) return 0;
        next = (uintptr_t)region.BaseAddress + region.RegionSize;
        if (next <= cursor) return 0;
        cursor = next;
    }
    g_index_n = 0;
    for (i = 0; i < count; i++) {
        unsigned char *row = array + (size_t)i * 32u;
        const char *key = *(const char **)(row + 8);
        if (!key) continue;
        if (!strids_reserve((void **)&g_index, &g_index_capacity, g_index_n + 1, sizeof(*g_index))) return 0;
        g_index[g_index_n++] = (strids_index_row){*(uint32_t *)row, i, key};
    }
    if (g_index_n > 1) qsort(g_index, g_index_n, sizeof(*g_index), strids_index_compare);
    return 1;
}

static size_t strids_index_first(uint32_t hash)
{
    size_t low = 0, high = g_index_n;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (g_index[middle].hash < hash) low = middle + 1; else high = middle;
    }
    return low;
}

static strids_owned_row *find_owned(const char *id)
{
    size_t i;
    for (i = 0; i < g_owned_n; i++)
        if (_stricmp(g_owned_rows[i].id, id) == 0) return &g_owned_rows[i];
    return NULL;
}

/* Where an owned row sits in the live table: 1 with its index, 0 when the
 * table no longer holds it (the engine rebuilt its dictionary, which drops
 * every row this injector added), -1 when the table cannot be read or two
 * rows claim it. */
static int find_live_owned(const strids_owned_row *row, uint32_t *at)
{
    unsigned char *array;
    uint32_t count;
    size_t i;
    int found = 0;
    if (!strids_table(&array, &count)) return -1;
    for (i = strids_index_first(row->hash); i < g_index_n && g_index[i].hash == row->hash; i++) {
        if (g_index[i].key == row->key_handle) {
            unsigned char *entry;
            if (g_index[i].index >= count) return -1;
            entry = array + (size_t)g_index[i].index * 32u;
            if (*(unsigned int *)entry != row->hash || *(void **)(entry + 8) != row->key_handle) return -1;
            if (found) return -1; /* Ambiguous ownership must not touch a row. */
            found = 1;
            *at = g_index[i].index;
        }
    }
    return found;
}

/* An existing game row with `key`: 1 with its index, 0 none, -1 ambiguous or
 * unreadable. Adopting it instead of appending keeps one row per key; native
 * AtomicString assignment and dictionary copies do not transfer ownership of
 * the pool's text, so keeping the original record permits exact restoration. */
static int find_existing_key(unsigned int hash, const char *key, uint32_t *at)
{
    unsigned char *array;
    uint32_t count;
    size_t i;
    int found = 0;
    if (!strids_table(&array, &count)) return -1;
    for (i = strids_index_first(hash); i < g_index_n && g_index[i].hash == hash; i++) {
        if (!_stricmp(g_index[i].key, key)) {
            unsigned char *row;
            if (g_index[i].index >= count) return -1;
            row = array + (size_t)g_index[i].index * 32u;
            if (*(uint32_t *)row != hash || *(const char **)(row + 8) != g_index[i].key) return -1;
            if (found) return -1;
            found = 1;
            *at = g_index[i].index;
        }
    }
    return found;
}

static unsigned int strids_text_hash(const char *text)
{
    unsigned int h = 2166136261u;
    for (; text && *text; text++) { h ^= (unsigned char)*text; h *= 16777619u; }
    return h;
}

static strids_row *find_injected(const char *id)
{
    for (size_t i = 0; i < g_injected_n; i++)
        if (_stricmp(g_injected_ids[i].id, id) == 0) return &g_injected_ids[i];
    return NULL;
}

static const char *strids_owner_name(int owner)
{
    if (owner >= 0 && (size_t)owner < g_bundle_names_n) return g_bundle_names[owner];
    return "<baked>";
}

/* ---- one pass: every row it wants is gathered first (the plan), checked
 * and given its memory; only then is the live table changed. A pass that
 * cannot be planned leaves the table exactly as it was. */

typedef struct strids_want {
    char *id;                 /* the key without "#str_" */
    char *text;               /* the text (process heap, handed to the owned row) */
    size_t text_len;
    int owner, layer;
} strids_want;
static strids_want *g_want;
static size_t g_want_n, g_want_capacity;

static void wants_free(void)
{
    size_t i;
    for (i = 0; i < g_want_n; i++) {
        free(g_want[i].id);
        if (g_want[i].text) HeapFree(GetProcessHeap(), 0, g_want[i].text);
    }
    g_want_n = 0;
}

/* Add one #str_<id> row to the pass unless an earlier layer or bundle
 * supplied the key (first writer wins). */
static void want_row(const char *id, const char *text, size_t text_len, int owner, int layer)
{
    strids_row *seen = find_injected(id);
    strids_want *w;
    size_t id_length = strlen(id);
    if (seen != NULL) {
        /* Two bundles with different text: the first stands and one line
         * names both. Equal text composes silently; the baked layer
         * never produces a bundle conflict. */
        if (owner != STRIDS_OWNER_NONE && seen->owner != STRIDS_OWNER_NONE && owner != seen->owner &&
            seen->text_hash != strids_text_hash(text)) {
            char line[400];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "STRINGS: REFUSED '#str_%s' from bundle %s -- bundle %s already defines it with "
                        "different text; the first definition stands",
                        id, strids_owner_name(owner), strids_owner_name(seen->owner));
            backend_log(line);
        }
        return;
    }
    if (!id_length || id_length > INT_MAX - 5u || text_len > INT_MAX || g_injected_n == SIZE_MAX ||
        !strids_reserve((void **)&g_injected_ids, &g_injected_capacity, g_injected_n + 1, sizeof(*g_injected_ids)) ||
        !strids_reserve((void **)&g_want, &g_want_capacity, g_want_n + 1, sizeof(*g_want))) {
        g_pass_failed = 1;
        return;
    }
    w = &g_want[g_want_n];
    memset(w, 0, sizeof *w);
    w->id = _strdup(id);
    w->text = (char *)HeapAlloc(GetProcessHeap(), 0, text_len + 1);
    if (!w->id || !w->text) {
        free(w->id);
        if (w->text) HeapFree(GetProcessHeap(), 0, w->text);
        g_pass_failed = 1;
        return;
    }
    memcpy(w->text, text, text_len);
    w->text[text_len] = 0;
    w->text_len = text_len;
    w->owner = owner;
    w->layer = layer;
    g_want_n++;
    g_injected_ids[g_injected_n].id = w->id;
    g_injected_ids[g_injected_n].text_hash = strids_text_hash(text);
    g_injected_ids[g_injected_n].owner = owner;
    g_injected_ids[g_injected_n].layer = layer;
    g_injected_n++;
    g_layer_rows[layer]++;
}

enum { STRIDS_KEEP, STRIDS_UPDATE, STRIDS_ADOPT, STRIDS_APPEND };

typedef struct strids_step {
    int action;               /* STRIDS_* */
    size_t owned;             /* KEEP, UPDATE: the owned row */
    uint32_t at;              /* UPDATE, ADOPT: the live row's index in the table as read */
    uint32_t hash;            /* ADOPT, APPEND */
} strids_step;

typedef struct strids_retirement {
    uint32_t index;
    size_t owned;
} strids_retirement;

static int retirement_compare(const void *left, const void *right)
{
    const strids_retirement *a = left, *b = right;
    return a->index < b->index ? -1 : a->index > b->index;
}

static void owned_free(strids_owned_row *o)
{
    if (o->text) HeapFree(GetProcessHeap(), 0, o->text);
    free(o->id);
}

/* Bring the live table to exactly the gathered rows (g_want). Everything a
 * pass can fail on is settled before the table is touched: where each owned
 * row is, which wanted rows update, adopt or append, which owned rows retire,
 * and the memory for all of it. A pass that cannot be planned returns 0 with
 * the table exactly as it was. Once applying, only an engine append can
 * refuse: the rows before it stand, each owned, and 0 is returned so the
 * next pass settles the rest. */
static int strids_commit(void)
{
    unsigned char *array;
    uint32_t count, appends = 0, *live_at = NULL;
    strids_step *steps = NULL;
    strids_retirement *retire = NULL;
    char **keys = NULL;
    size_t i, nretire = 0, nnew = 0, kept;
    int ok = 0;

    g_table_changed = 0;
    if (g_pass_failed || !strids_table(&array, &count)) return 0;
    /* An owned row the table no longer holds went with the engine's own
     * rebuild of its dictionary (which drops every row this injector added):
     * it is forgotten here and, when still wanted, added again below. */
    if (g_owned_n && !(live_at = (uint32_t *)calloc(g_owned_n, sizeof *live_at))) return 0;
    for (i = 0, kept = 0; i < g_owned_n; i++) {
        uint32_t at = 0;
        int r = find_live_owned(&g_owned_rows[i], &at);
        if (r < 0) { free(live_at); return 0; }
        if (r == 0) { owned_free(&g_owned_rows[i]); continue; }
        live_at[kept] = at;
        g_owned_rows[kept++] = g_owned_rows[i];
    }
    if (kept != g_owned_n) {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE, "STRINGS: the game rebuilt its string table; %zu of Snapmap+'s rows "
                    "are added again", g_owned_n - kept);
        backend_log(line);
    }
    g_owned_n = kept;

    if (g_want_n && (!(steps = (strids_step *)calloc(g_want_n, sizeof *steps)) ||
                     !(keys = (char **)calloc(g_want_n, sizeof *keys))))
        goto out;
    if (g_owned_n && !(retire = (strids_retirement *)calloc(g_owned_n, sizeof *retire))) goto out;
    for (i = 0; i < g_want_n; i++) {
        const strids_want *w = &g_want[i];
        strids_owned_row *o = find_owned(w->id);
        if (o) {
            size_t k = (size_t)(o - g_owned_rows);
            const unsigned char *live = array + (size_t)live_at[k] * 32u;
            const char *now = *(char *const *)(live + 16);
            steps[i].owned = k;
            steps[i].at = live_at[k];
            steps[i].action = !strcmp(o->text, w->text) && now && !strcmp(now, w->text) ? STRIDS_KEEP : STRIDS_UPDATE;
        } else {
            size_t id_length = strlen(w->id);
            int r;
            if (!(keys[i] = (char *)malloc(id_length + 6))) goto out;
            memcpy(keys[i], "#str_", 5);
            memcpy(keys[i] + 5, w->id, id_length + 1);
            steps[i].hash = g_hash(keys[i]);
            r = find_existing_key(steps[i].hash, keys[i], &steps[i].at);
            if (r < 0) goto out;
            steps[i].action = r ? STRIDS_ADOPT : STRIDS_APPEND;
            if (!r) appends++;
            nnew++;
        }
    }
    /* Owned rows no layer wants any more retire (a replaced game row gets its
     * original record back, an added one goes). */
    for (i = 0; i < g_owned_n; i++)
        if (!find_injected(g_owned_rows[i].id)) {
            retire[nretire].index = live_at[i];
            retire[nretire++].owned = i;
        }
    if (nretire > 1) {
        qsort(retire, nretire, sizeof *retire, retirement_compare);
        for (i = 1; i < nretire; i++) if (retire[i - 1].index == retire[i].index) goto out;
    }
    if (appends > (uint32_t)INT_MAX - count) goto out;
    if (nnew > SIZE_MAX - g_owned_n ||
        !strids_reserve((void **)&g_owned_rows, &g_owned_capacity, g_owned_n + nnew, sizeof(*g_owned_rows)))
        goto out;

    g_table_changed = 1;
    /* Apply: in place first (the indexes are of the table as read), then the
     * appends at the end, then the retirements by those same indexes (an
     * append never moves an earlier row's index; the array is read again
     * after the appends, which may move it). */
    for (i = 0; i < g_want_n; i++) {
        strids_want *w = &g_want[i];
        uint32_t length = (uint32_t)w->text_len;
        unsigned char *live;
        strids_owned_row *o;
        if (steps[i].action != STRIDS_UPDATE && steps[i].action != STRIDS_ADOPT) continue;
        live = array + (size_t)steps[i].at * 32u;
        if (steps[i].action == STRIDS_UPDATE) {
            o = &g_owned_rows[steps[i].owned];
            HeapFree(GetProcessHeap(), 0, o->text);
        } else {
            o = &g_owned_rows[g_owned_n++];
            memset(o, 0, sizeof *o);
            memcpy(o->original, live, sizeof o->original);
            o->key_handle = *(void **)(live + 8);
            o->hash = steps[i].hash;
            o->replaced = 1;
            o->id = w->id;
            w->id = NULL;
        }
        /* AtomicString assignment interns text and writes its borrowed
         * pointer. It does not release the old value; dictionary rows own no
         * pool memory. */
        g_idstr_ctor(live + 16, w->text);
        memcpy(live + 24, &length, 4);
        memcpy(live + 28, &length, 4);
        o->text = w->text;
        w->text = NULL;
    }
    for (i = 0; i < g_want_n; i++) {
        strids_want *w = &g_want[i];
        uint32_t length = (uint32_t)w->text_len, before;
        unsigned char rec[32] = {0};
        strids_owned_row *o;
        if (steps[i].action != STRIDS_APPEND) continue;
        *(uint32_t *)rec = steps[i].hash;
        g_idstr_ctor(rec + 8, keys[i]);
        g_idstr_ctor(rec + 16, w->text);
        memcpy(rec + 24, &length, 4);
        memcpy(rec + 28, &length, 4);
        before = *(uint32_t *)((unsigned char *)g_table_desc + 8);
        g_insert(g_table_desc, rec);
        if (*(uint32_t *)((unsigned char *)g_table_desc + 8) != before + 1u) {
            backend_log("STRINGS: the game refused a new string row; the rows before it stand and the next refresh "
                        "adds the rest");
            goto out;
        }
        InterlockedIncrement(&g_inject_count);
        o = &g_owned_rows[g_owned_n++];
        memset(o, 0, sizeof *o);
        o->id = w->id;
        w->id = NULL;
        o->key_handle = *(void **)(rec + 8);
        o->hash = steps[i].hash;
        o->text = w->text;
        w->text = NULL;
    }
    if (nretire) {
        uint32_t read, written = 0;
        size_t next = 0;
        if (!strids_table(&array, &count)) goto out;
        for (read = 0; read < count; read++) {
            const unsigned char *source = array + (size_t)read * 32u;
            if (next < nretire && retire[next].index == read) {
                const strids_owned_row *owned = &g_owned_rows[retire[next++].owned];
                if (!owned->replaced) continue;
                source = owned->original;
            }
            memmove(array + (size_t)written++ * 32u, source, 32u);
        }
        *(uint32_t *)((unsigned char *)g_table_desc + 8) = written;
        /* The retired rows leave the owned list (only rows before the
         * appended ones can retire, so their positions are as planned). */
        for (i = 0, kept = 0; i < g_owned_n; i++) {
            if (!find_injected(g_owned_rows[i].id)) { owned_free(&g_owned_rows[i]); continue; }
            g_owned_rows[kept++] = g_owned_rows[i];
        }
        g_owned_n = kept;
    }
    ok = 1;
out:
    for (i = 0; keys && i < g_want_n; i++) free(keys[i]);
    free(keys);
    free(steps);
    free(retire);
    free(live_at);
    return ok;
}

/* Decode the whole flat dictionary before publishing any of its rows. The
 * shared JSON reader handles Unicode escapes and rejects malformed input.
 * Native text handles are NUL-terminated, so embedded NUL is unsupported.
 * Returns 1 with the decoded pairs, or 0 (pairs freed) for a malformed one.
 */
static int decode_pairs(const char *buf, size_t length, sh_json_object *pairs)
{
    size_t i;
    /* Windows text writers may prefix an otherwise ordinary UTF-8 document. */
    if (buf && length >= 3 && memcmp(buf, "\xef\xbb\xbf", 3) == 0) {
        buf += 3;
        length -= 3;
    }
    if (!buf || !sh_json_parse_object(buf, length, 1, pairs)) goto invalid;
    for (i = 0; i < pairs->count; i++) {
        sh_json_member *member = &pairs->members[i];
        size_t encoded_length = strlen(member->value_json), decoded_length = 0;
        if (!member->key_length || member->key_length > INT_MAX - 5u ||
            strlen(member->key) != member->key_length ||
            !sh_json_decode_string(member->value_json, encoded_length,
                                   member->value_json, encoded_length + 1, &decoded_length) ||
            strlen(member->value_json) != decoded_length || decoded_length > INT_MAX)
            goto invalid;
    }
    return 1;
invalid:
    sh_json_object_free(pairs);
    return 0;
}

/* ------------------------------------------------------------------ bundle layer */

/* mod.json is validated at this depth by the serving index (modserve.c). */
#define STRIDS_BUNDLE_JSON_DEPTH 16u

/* Per-bundle supply lines are written only when this environment variable is
 * set to a nonzero value; the pass totals are always logged. */
#define STRIDS_DEBUG_ENV "SNAPMAP_PLUS_STRIDS_DEBUG"

static int strids_debug(void)
{
    char value[8];
    DWORD n = GetEnvironmentVariableA(STRIDS_DEBUG_ENV, value, sizeof value);
    return n > 0 && n < sizeof value && strcmp(value, "0") != 0;
}

static void free_bundle_names(void)
{
    size_t i;
    for (i = 0; i < g_bundle_names_n; i++) free(g_bundle_names[i]);
    g_bundle_names_n = 0;
}

typedef struct strids_bundle_pass {
    int debug;
} strids_bundle_pass;

/* One admitted bundle: extra.strings.en is a flat object of key -> text, keys
 * without the #str_ prefix (a mod's keys are <salt>_<name>; the builder
 * refuses any other, so a decl references #str_<salt>_<name>). Other languages
 * are ignored; the native table holds one. A malformed strings object refuses
 * that bundle's strings only. */
/* The bundles with strings, and whether the last pass applied their own
 * keys and their string edits (sh_strids_scope_check). */
#define STRIDS_SCOPED_BUNDLES 256
static struct { char hash[65]; int own_live, edits_live; } g_scoped[STRIDS_SCOPED_BUNDLES];
static volatile LONG g_scoped_count;

/* A mod's own keys apply where its build serves its names: the build the open
 * map declares (another build of the salt is inert there), and, with no map
 * declaring the salt, the root's build in the editor (modserve.h, map scope).
 * Two builds of one salt with different text therefore never compete. */
static int strids_own_live(const char *hash)
{
    sh_modserve_scope_info si;
    if (!sh_modserve_scope_enabled()) return 1;
    return hash && sh_modserve_bundle_scope(hash, &si) && si.names_live;
}

static void inject_bundle(void *context, const char *id, const char *hash, int place, const char *extra)
{
    strids_bundle_pass *pass = (strids_bundle_pass *)context;
    sh_json_object root = {0}, languages = {0}, pairs = {0};
    const char *strings, *english;
    size_t before, name_cap, i;
    char *name;
    int owner, edits = 0, live = 1, own;
    (void)place;   /* the order already places maps' places after the root */
    if (!id) id = "";
    if (!extra || !sh_json_parse_object(extra, strlen(extra), STRIDS_BUNDLE_JSON_DEPTH, &root)) goto done;
    strings = sh_json_object_get(&root, "strings");
    if (!strings) goto done;
    if (*strings != '{' || !sh_json_parse_object(strings, strlen(strings), STRIDS_BUNDLE_JSON_DEPTH, &languages))
        goto refused;
    english = sh_json_object_get(&languages, "en");
    if (!english) goto done;
    if (*english != '{' || !decode_pairs(english, strlen(english), &pairs)) goto refused;
    name_cap = strlen(id) + 1;
    if (g_bundle_names_n >= INT_MAX ||
        !strids_reserve((void **)&g_bundle_names, &g_bundle_names_capacity,
                        g_bundle_names_n + 1, sizeof(*g_bundle_names)) ||
        !(name = (char *)malloc(name_cap))) {
        g_pass_failed = 1;
        goto done;
    }
    _snprintf_s(name, name_cap, _TRUNCATE, "%s", id);
    owner = (int)g_bundle_names_n;
    g_bundle_names[g_bundle_names_n++] = name;
    before = g_layer_rows[STRIDS_LAYER_BUNDLE];
    /* A key the game or the product defines is an edit of that string: it
     * applies where the mod's other edits apply (an editor-tweak mod's in
     * every map, any other mod's in maps that declare it). The mod's own
     * keys always apply. */
    for (i = 0; i < pairs.count; i++) edits |= sh_strkey_is_edit(pairs.members[i].key, pairs.members[i].key_length);
    live = edits ? sh_modserve_bundle_edits_live(hash) : 0;
    own = strids_own_live(hash);
    {
        LONG at = InterlockedCompareExchange(&g_scoped_count, 0, 0);
        if (at < STRIDS_SCOPED_BUNDLES) {
            strncpy_s(g_scoped[at].hash, sizeof g_scoped[at].hash, hash ? hash : "", _TRUNCATE);
            g_scoped[at].own_live = own;
            g_scoped[at].edits_live = edits ? live : -1;
            InterlockedExchange(&g_scoped_count, at + 1);
        }
    }
    for (i = 0; i < pairs.count; i++) {
        int edit = sh_strkey_is_edit(pairs.members[i].key, pairs.members[i].key_length);
        if (edit ? !live : !own) continue;
        want_row(pairs.members[i].key, pairs.members[i].value_json, strlen(pairs.members[i].value_json), owner,
                 STRIDS_LAYER_BUNDLE);
    }
    if (pass && pass->debug) {
        char line[256];
        _snprintf_s(line, sizeof line, _TRUNCATE, "STRINGS: bundle %s supplied %zu string(s)", name,
                    g_layer_rows[STRIDS_LAYER_BUNDLE] - before);
        backend_log(line);
    }
    goto done;
refused:
    {
        char line[256];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "STRINGS: REFUSED the strings of bundle %s: expected extra.strings.en to be a JSON "
                    "object of valid text strings and nonempty IDs", id);
        backend_log(line);
    }
done:
    sh_json_object_free(&pairs);
    sh_json_object_free(&languages);
    sh_json_object_free(&root);
}

/* Admitted bundles, root folders before maps' places, then salt, then
 * folder (sh_modserve_each_bundle_extra). Before the serving index exists this layer
 * is empty; each index build then requests a refresh (sh_strids_request_refresh). */
static void inject_bundles(void)
{
    strids_bundle_pass pass;
    pass.debug = strids_debug();
    if (sh_modserve_each_bundle_extra(inject_bundle, &pass) < 0) {
        g_pass_failed = 1;
        backend_log("STRINGS: REFUSED: the bundle string layer could not be read (out of memory)");
    }
}

/* "N #str_ entries (bundles B, baked D)": the keys each layer won in the last
 * pass. */
static void layer_summary(char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%zu #str_ entries (bundles %zu, baked %zu)",
                g_layer_rows[STRIDS_LAYER_BUNDLE] + g_layer_rows[STRIDS_LAYER_BAKED],
                g_layer_rows[STRIDS_LAYER_BUNDLE], g_layer_rows[STRIDS_LAYER_BAKED]);
}

/* Inject the bundle and baked layers. Return the appended row count. */
static long inject_layers(void)
{
    if (g_table_desc == NULL || g_insert == NULL || g_hash == NULL || g_idstr_ctor == NULL)
        return 0;

    g_injected_n = 0;   /* fresh dedup set for this inject pass */
    wants_free();
    InterlockedExchange(&g_scoped_count, 0);
    free_bundle_names();
    memset(g_layer_rows, 0, sizeof g_layer_rows);

    /* Admitted bundles first. */
    inject_bundles();

    /* Baked defaults fill remaining keys. */
    for (size_t bi = 0; bi < B1_STRIDS_BAKED_COUNT; bi++)
        want_row(g_strids_baked[bi].id, g_strids_baked[bi].text, strlen(g_strids_baked[bi].text), STRIDS_OWNER_NONE,
                 STRIDS_LAYER_BAKED);

    /* The table changes only now, and only when the whole pass is planned. */
    if (!strids_commit()) g_pass_failed = 1;
    return (long)InterlockedCompareExchange(&g_inject_count, 0, 0);
}

static long do_inject(void)
{
    long result = 0;
    if (!g_table_desc || !g_insert || !g_hash || !g_idstr_ctor) return 0;
    __try {
        if (strids_index_begin()) result = inject_layers();
        else g_pass_failed = 1;
    } __finally {
        free(g_index); g_index = NULL; g_index_n = g_index_capacity = 0;
    }
    return result;
}

/* Re-sort the entire live table after appending: native lookup binary-
 * searches its +0x00 hash key. Call the original sort body with radix 0x20
 * and the descriptor as context. The descriptor holds its array at +0 and
 * count at +8. Pinned Vulkan lookup comparator: 0x1a2aa90.
 */
static void resort_table(void)
{
    if (g_sort_orig == NULL || g_table_desc == NULL) { g_pass_failed = 1; return; }
    __try {
        unsigned char *live_arr;
        uint32_t live_cnt;
        if (!strids_table(&live_arr, &live_cnt)) {
            g_pass_failed = 1;
            return;
        }
        if (live_arr != NULL && live_cnt > 1)
            g_sort_orig(g_table_desc, live_arr, live_cnt, SORT_RADIX);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_pass_failed = 1; }
}

/* Claim the shared one-shot latch, inject and re-sort. Returns appended rows,
 * or -1 if another path already claimed injection.
 */
static long inject_and_resort_once(void)
{
    if (InterlockedCompareExchange(&g_injected, 1, 0) != 0)
        return -1;   /* already injected by the other path -- do nothing, no double rows */
    /* This pass reads the published index; only a later one needs a refresh. */
    InterlockedExchange(&g_refresh_pending, 0);

    long n = do_inject();
    resort_table();

    char summary[128], line[160];
    layer_summary(summary, sizeof summary);
    _snprintf_s(line, sizeof line, _TRUNCATE, "STRINGS: injected %s", summary);
    backend_log(line);
    return n;
}

/* Fallback sort detour. Installation normally injects first; later and
 * recursive sorts pass through without adding rows.
 */
static void sh_sort_detour(void *ctx, void *arr, uint32_t count, uint32_t radix)
{
    if (g_sort_orig == NULL) return;   /* defensive: never happens once installed */

    LONG depth = InterlockedIncrement(&g_in_sort);
    if (depth == 1 && InterlockedCompareExchange(&g_injected, 1, 0) == 0) {
        /* If this top-level sort wins the latch, append now and let this
         * native sort order the enlarged table. Do not start a nested resort.
         */
        char summary[128], line[160];
        InterlockedExchange(&g_refresh_pending, 0);   /* this pass reads the published index */
        (void)do_inject();
        layer_summary(summary, sizeof summary);
        _snprintf_s(line, sizeof line, _TRUNCATE, "STRINGS: injected %s", summary);
        backend_log(line);
        /* The table grew -- re-read the live count from the descriptor so the original sort orders the
         * FULL augmented table, not the pre-inject count it was called with. The descriptor's count is
         * the dword at +8 (idList layout: +0 array ptr, +8 num, +0xc capacity -- DIRECT, FUN_141a29980). */
        __try {
            uint32_t live = *(uint32_t *)((uint8_t *)g_table_desc + 8);
            void    *live_arr = *(void **)g_table_desc;
            if (live_arr) arr = live_arr;
            if (live)     count = live;
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* keep the engine's args on any read fault */ }
    }

    g_sort_orig(ctx, arr, count, radix);
    /* The modeled sort returns void. Work after its call clobbers EAX; the
     * pinned caller does not consume EAX/AL. Recheck return-value use when
     * porting this hook.
     */
    InterlockedDecrement(&g_in_sort);
}

int sh_strids_install(void *sort_body_fn, int sort_status_ok,
                      void *table_lea_fn, void *insert_fn, void *hash_fn, void *idstr_ctor_fn)
{
    char line[256];

    if (g_sort_orig != NULL) {
        if (hook_is_installed((void *)g_sort_orig)) return 1;
        if (!hook_unpatch((void *)g_sort_orig)) return 0;
        g_sort_orig = NULL;
        g_table_desc = NULL; g_insert = NULL; g_hash = NULL; g_idstr_ctor = NULL;
    }
    if (sort_body_fn == NULL) {
        backend_log("STRINGS: injector SKIPPED -- StridsSortBody not resolved");
        return 0;
    }
    if (!sort_status_ok) {
        backend_log("STRINGS: injector SKIPPED -- StridsSortBody resolved via hook-tolerant fallback "
                    "(prologue already hooked); not installing over an existing detour");
        return 0;
    }
    if (table_lea_fn == NULL || insert_fn == NULL || hash_fn == NULL || idstr_ctor_fn == NULL) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "STRINGS: injector SKIPPED -- missing engine fn (lea=%p insert=%p hash=%p ctor=%p)",
            table_lea_fn, insert_fn, hash_fn, idstr_ctor_fn);
        backend_log(line);
        return 0;
    }

    g_table_desc = decode_table_global((const uint8_t *)table_lea_fn);
    if (g_table_desc == NULL) {
        backend_log("STRINGS: injector SKIPPED -- could not decode the table-global LEA "
                    "(StridsTableLea layout shifted?)");
        return 0;
    }
    g_insert     = (insert_fn_t)insert_fn;
    g_hash       = (hash_fn_t)hash_fn;
    g_idstr_ctor = (idstr_ctor_fn_t)idstr_ctor_fn;

    void *tramp = hook_prepare(sort_body_fn, (void *)sh_sort_detour, SORT_STOLEN);
    if (tramp == NULL) {
        backend_log("STRINGS: injector FAIL -- could not prepare the original callback");
        g_table_desc = NULL; g_insert = NULL; g_hash = NULL; g_idstr_ctor = NULL;
        return 0;
    }
    g_sort_orig = (sort_fn_t)tramp;

    InterlockedExchange(&g_injected, 0);
    if (hook_commit(tramp) != B2_PATCH_OK) {
        InterlockedExchange(&g_injected, 1);
        if (hook_unpatch(tramp)) {
            g_sort_orig = NULL;
            g_table_desc = NULL; g_insert = NULL; g_hash = NULL; g_idstr_ctor = NULL;
        }
        backend_log("STRINGS: injector FAIL -- hook commit failed; pending restoration retains the original callback");
        return 0;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "STRINGS: injector installed at %p (trampoline %p, stolen %d); table=%p",
        sort_body_fn, tramp, SORT_STOLEN, g_table_desc);
    backend_log(line);

    /* Built-in declarations can load at any time. Supply their text now. */
    inject_and_resort_once();
    return 1;
}

static const sh_process_heap_api *g_heap_api;

void sh_strids_set_process_heap(const sh_process_heap_api *api)
{
    g_heap_api = api;
}

int sh_strids_rearm(void)
{
    char summary[128], line[192];
    int ok;
    sh_process_heap_scope heap;
    if (!g_sort_orig || !hook_is_installed((void *)g_sort_orig) ||
        !g_table_desc || !g_insert || !g_hash || !g_idstr_ctor ||
        InterlockedCompareExchange(&g_refresh_busy, 1, 0)) return 0;
    /* An append that grows the table allocates its new array from the main
     * thread's current heap (the list's resize asks for heap -1). During a
     * map or the editor that is the map's own heap, which the game frees when
     * the map goes, leaving the game's string lookups reading freed memory.
     * The game's table lives in the process heap; so does every runtime pass,
     * or none runs. */
    if (!g_heap_api || !sh_process_heap_enter(g_heap_api, &heap)) {
        InterlockedExchange(&g_refresh_busy, 0);
        sh_strids_request_refresh();   /* the next pass on the main thread runs it */
        backend_log("STRINGS: runtime refresh waits: the game's process heap could not be selected on this thread");
        return 0;
    }
    g_pass_failed = 0;
    __try {
        __try { do_inject(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_pass_failed = 1; g_table_changed = 1; }
        /* A pass that wrote to the table leaves it sorted, a partial one
         * included; one that could not be planned left it exactly as it was. */
        if (g_table_changed) resort_table();
        ok = !g_pass_failed;
    } __finally {
        if (!sh_process_heap_leave(&heap)) {
            ok = 0;
            backend_log("STRINGS: runtime refresh could not restore the game's heap selection");
        }
        InterlockedExchange(&g_refresh_busy, 0);
    }
    layer_summary(summary, sizeof summary);
    _snprintf_s(line, sizeof line, _TRUNCATE, "STRINGS: runtime refresh %s: %s",
                ok ? "complete" : "FAILED", summary);
    backend_log(line);
    return ok;
}

void sh_strids_request_refresh(void)
{
    InterlockedExchange(&g_refresh_pending, 1);
}

int sh_strids_scope_check(void)
{
    LONG i, n = InterlockedCompareExchange(&g_scoped_count, 0, 0);
    for (i = 0; i < n; i++)
        if (strids_own_live(g_scoped[i].hash) != g_scoped[i].own_live ||
            (g_scoped[i].edits_live >= 0 && sh_modserve_bundle_edits_live(g_scoped[i].hash) != g_scoped[i].edits_live)) {
            sh_strids_request_refresh();
            return 1;
        }
    return 0;
}

int sh_strids_refresh_pending(void)
{
    if (!InterlockedCompareExchange(&g_refresh_pending, 0, 0)) return -1;
    /* Before the first injection there is no pass to refresh; that injection
     * reads the published index itself, so the request waits for it. */
    if (!InterlockedCompareExchange(&g_injected, 0, 0)) return -1;
    InterlockedExchange(&g_refresh_pending, 0);
    return sh_strids_rearm();
}

#ifdef SH_STRIDS_TESTING
/* Bind doubles for the four native helpers and inspect the rows requested by
 * one injection pass.
 */
int sh_strids_test_inject(void *table_desc, void *insert, void *hash, void *idstr_ctor)
{
    size_t i;
    for (i = 0; i < g_owned_n; i++) {
        if (g_owned_rows[i].text) HeapFree(GetProcessHeap(), 0, g_owned_rows[i].text);
        free(g_owned_rows[i].id);
    }
    free(g_owned_rows); g_owned_rows = NULL; g_owned_capacity = 0;
    wants_free();
    free(g_injected_ids); g_injected_ids = NULL; g_injected_capacity = 0;
    g_owned_n = 0;
    g_pass_failed = 0;
    g_table_desc = table_desc;
    g_insert     = (insert_fn_t)insert;
    g_hash       = (hash_fn_t)hash;
    g_idstr_ctor = (idstr_ctor_fn_t)idstr_ctor;
    InterlockedExchange(&g_inject_count, 0);
    g_injected_n = 0;
    return (int)do_inject();
}

void sh_strids_test_set_sort(void *sort)
{
    g_sort_orig = (sort_fn_t)sort;
}

/* How the injector attributed each row this pass: the id, and its owner ("<baked>" for the baked
 * defaults, the salt for a bundle). Returns 0 past the end. */
int sh_strids_test_row(int index, const char **id_out, const char **owner_out)
{
    if (index < 0 || (size_t)index >= g_injected_n) return 0;
    if (id_out)    *id_out    = g_injected_ids[index].id;
    if (owner_out) *owner_out = strids_owner_name(g_injected_ids[index].owner);
    return 1;
}
#endif /* SH_STRIDS_TESTING */
