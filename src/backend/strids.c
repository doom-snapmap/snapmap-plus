/* Injects bundle and baked #str_ mappings, then sorts the live dictionary by hash; install injects, the sort
 * hook is a fallback. Native 32-byte record: +0x00 lowercased FNV-1a hash, +0x08 interned key handle,
 * +0x10 interned value handle, value lengths at +0x18/+0x1c; the engine owns the string pool and list. */
#include "rip_slot.h"
#include "safe_read.h"
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
#include "editor_ids.h"
#include "modserve.h"   /* sh_modserve_each_bundle_extra: the bundle string layer */
#include "strkeys.h"    /* string edits: keys the game or the product defines */
#include "process_heap_scope.h"   /* a runtime pass grows the table in the process heap */

/* StridsSortBody prologue steal window: mov [rsp+0x18],rbx; push rbp/rsi/rdi;
 * lea rbp,[rsp-0x7f0] = 16 bytes of whole, rsp-only, position-independent instructions (no RIP-rel, no rel jmp). */
#define SORT_STOLEN 16

/* Native sort ABI: void sort(ctx, arr, uint32 count, uint32 radix). */
typedef void (*sort_fn_t)(void *ctx, void *arr, uint32_t count, uint32_t radix);

/* The native wrapper sets radix=0x20 before the body, which sorts the 32-bit hash
 * eight bits at a time. */
#define SORT_RADIX 0x20

/* The engine functions the inject calls, all resolved by the signature scanner. */
typedef int   (*insert_fn_t)(void *table_desc, void *record32);   /* idList<StridEntry>::Append */
typedef uint32_t (*hash_fn_t)(const char *s);                     /* idStr::Hash (FNV-1a, lowercased) */
typedef void  (*idstr_ctor_fn_t)(void *out_handle, const char *s);/* *out_handle = pool_intern(s) */

static sort_fn_t       g_sort_orig   = NULL;   /* trampoline -> the real engine sort body */
static void           *g_table_desc  = NULL;   /* the idLangDict table descriptor (.data global) */
static insert_fn_t     g_insert      = NULL;
static hash_fn_t       g_hash        = NULL;
static idstr_ctor_fn_t g_idstr_ctor  = NULL;

static volatile LONG g_refresh_busy;
static volatile LONG g_refresh_pending;   /* a serving index was published */
static int g_pass_failed;
static volatile LONG g_injected      = 0;      /* one-shot latch (0 = not yet injected) */
static volatile LONG g_in_sort       = 0;      /* recursion guard (>0 = inside the sort already) */
static volatile LONG g_inject_count  = 0;      /* rows appended (observability) */
static int g_table_changed;                     /* the last pass wrote to the live table */

/* Keys deduplicate case-insensitively within a pass (the native hash lowercases; duplicate rows would make
 * lookup ambiguous): first writer wins, admitted bundles in modserve's order, then baked defaults. */
/* Who supplied a row: a bundle owner is its index in g_bundle_names (so a conflict can name both sides);
 * the baked backstop never conflict-reports. */
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
    void *value_handle;
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

char *sh_strids_label(const char *tag)
{
    unsigned char *base;
    uint32_t hash, count, lo = 0, hi;
    char *text = NULL;
    if (!tag || !g_table_desc || !g_hash) return NULL;
    __try {
        if (!strids_table(&base, &count)) return NULL;
        hash = g_hash(tag);
        hi = count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (*(uint32_t *)(base + (size_t)mid * 32) < hash) lo = mid + 1;
            else hi = mid;
        }
        for (; lo < count; ++lo) {
            const unsigned char *row = base + (size_t)lo * 32;
            const char *key = *(const char *const *)(row + 8), *value = *(const char *const *)(row + 16);
            if (*(const uint32_t *)row != hash) break;
            if (key && value && !_stricmp(key, tag)) { text = _strdup(value); break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return text;
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

/* Where an owned row sits in the live table: 1 with its index, 0 when the table no longer holds it (an engine
 * dictionary rebuild drops every injected row), -1 when the table is unreadable or two rows claim it. */
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
            /* A new native language table can reuse the interned key with a different stock value. */
            if (*(void **)(entry + 16) != row->value_handle) continue;
            if (found) return -1; /* Ambiguous ownership must not touch a row. */
            found = 1;
            *at = g_index[i].index;
        }
    }
    return found;
}

/* An existing game row with `key`: 1 with its index, 0 none, -1 ambiguous or unreadable. Adopting it keeps
 * one row per key, and keeping its original record permits exact restoration (native string assignment and
 * dictionary copies do not transfer ownership of the pool's text). */
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

/* One pass gathers every wanted row first (the plan), checks it and allocates its memory, and only then changes
 * the live table; a pass that cannot be planned leaves the table unchanged. */

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

/* Add one #str_<id> row to the pass unless a prior layer or bundle supplied the key (first writer wins). */
static void want_row(const char *id, const char *text, size_t text_len, int owner, int layer)
{
    strids_row *seen = find_injected(id);
    strids_want *w;
    size_t id_length = strlen(id);
    if (seen != NULL) {
        /* Two bundles with different text: the first stands and one line names both; equal text composes silently. */
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

/* Bring the live table to exactly the gathered rows (g_want), settling everything that can fail before touching
 * it: an unplannable pass returns 0 with the table unchanged. Once applying, only an engine append can refuse;
 * earlier rows stand, owned, and 0 is returned so the next pass finishes. */
static int strids_commit(void)
{
    unsigned char *array;
    uint32_t count, appends = 0, *live_at = NULL;
    strids_step *steps = NULL;
    strids_retirement *retire = NULL;
    char **keys = NULL;
    size_t i, nretire = 0, nnew = 0, kept;
    int ok = 0, had_appended = 0, kept_appended = 0;

    g_table_changed = 0;
    if (g_pass_failed || !strids_table(&array, &count)) return 0;
    /* An owned row the table no longer holds went with an engine dictionary rebuild: it is forgotten here and
     * added again below if still wanted. */
    if (g_owned_n && !(live_at = (uint32_t *)calloc(g_owned_n, sizeof *live_at))) return 0;
    for (i = 0, kept = 0; i < g_owned_n; i++) {
        uint32_t at = 0;
        int r = find_live_owned(&g_owned_rows[i], &at);
        if (r < 0) { free(live_at); return 0; }
        if (!g_owned_rows[i].replaced) { had_appended = 1; kept_appended |= r; }
        if (r == 0) { owned_free(&g_owned_rows[i]); continue; }
        live_at[kept] = at;
        g_owned_rows[kept++] = g_owned_rows[i];
    }
    /* A full native reload can intern both the same key and the same edited value in a fresh stock row. */
    if (had_appended && !kept_appended) {
        for (i = 0; i < kept; i++) owned_free(&g_owned_rows[i]);
        kept = 0;
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
    /* Owned rows no layer wants retire: an adopted game row gets its original record back, an added one goes. */
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
    /* Apply in place first (indexes are of the table as read), then appends, then retirements by those indexes
     * (an append never moves an earlier row's index; the array is re-read after the appends, which may move it). */
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
        /* AtomicString assignment interns the text and writes its borrowed pointer, without releasing the
         * previous value; dictionary rows own no pool memory. */
        g_idstr_ctor(live + 16, w->text);
        o->value_handle = *(void **)(live + 16);
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
        o->value_handle = *(void **)(rec + 16);
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
        /* Retired rows leave the owned list (only rows before the appended ones retire, so positions are as planned). */
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

/* Decode the whole flat dictionary before publishing any row, through the shared JSON reader (Unicode escapes,
 * malformed input rejected). Native text is NUL-terminated, so an embedded NUL is refused. Returns 1 with the
 * decoded pairs, or 0 (pairs freed) for a malformed one. */
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


/* mod.json is validated at this depth by the serving index (modserve.c). */
#define STRIDS_BUNDLE_JSON_DEPTH 16u

/* Per-bundle supply lines are logged only when this variable is nonzero; pass totals always are. */
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
    const char *locale;
} strids_bundle_pass;

static char *(*g_read_language)(void);

void sh_strids_set_language_reader(char *(*read_language)(void))
{
    g_read_language = read_language;
    sh_strids_request_refresh();
}

const char *sh_strids_locale(void)
{
    static const struct { const char *table, *locale; } languages[] = {
        { "english", "en" }, { "french", "fr" }, { "german", "de" }, { "italian", "it" },
        { "spanish", "es" }, { "latin_spanish", "es-419" }, { "portuguese", "pt" }, { "polish", "pl" },
        { "russian", "ru" }, { "japanese", "ja" }, { "chinese", "zh-Hant" }
    };
    char *language = g_read_language ? g_read_language() : NULL;
    const char *locale = "en";
    for (size_t i = 0; language && i < sizeof languages / sizeof languages[0]; i++)
        if (!_stricmp(language, languages[i].table)) { locale = languages[i].locale; break; }
    free(language);
    return locale;
}

static int strids_pair_cmp(const void *left, const void *right)
{
    return _stricmp(((const sh_json_member *)left)->key, ((const sh_json_member *)right)->key);
}

/* The bundles with strings, and whether the last pass applied their own keys and their string edits
 * (sh_strids_scope_check). */
typedef struct strids_scoped { char hash[65]; int own_live, edits_live; } strids_scoped;
static strids_scoped *g_scoped;   /* grows with the bundles; under g_scoped_lock */
static size_t g_scoped_count, g_scoped_capacity;
static SRWLOCK g_scoped_lock = SRWLOCK_INIT;

/* A mod's own keys apply where its build serves its names: the build the open map declares, or with no map
 * declaring the salt, the root's build in the editor (modserve.h). Two builds of one salt never compete. */
static int strids_own_live(const char *hash)
{
    sh_modserve_scope_info si;
    if (!sh_modserve_scope_enabled()) return 1;
    return hash && sh_modserve_bundle_scope(hash, &si) && si.names_live;
}

static void strids_tab_labels(const char *salt, const sh_json_object *root, const sh_json_object *english,
                              const sh_json_object *translated, int owner)
{
    sh_json_object editor = {0}, tabs = {0};
    const char *doc = sh_json_object_get(root, "editor");
    if (!doc || !sh_json_parse_object(doc, strlen(doc), STRIDS_BUNDLE_JSON_DEPTH, &editor)) goto done;
    const char *definitions = sh_json_object_get(&editor, "tabs");
    if (!definitions || !sh_json_parse_object(definitions, strlen(definitions), STRIDS_BUNDLE_JSON_DEPTH, &tabs)) goto done;
    for (size_t i = 0; i < tabs.count; i++) {
        const sh_json_member *m = &tabs.members[i];
        sh_json_object entry = {0};
        if (!sh_json_parse_object(m->value_json, strlen(m->value_json), STRIDS_BUNDLE_JSON_DEPTH, &entry)) continue;
        const char *title = sh_json_object_get(&entry, "title");
        if (title) {
            size_t n = strlen(title) + strlen(salt) + strlen(m->key) + 96;
            char *buf = malloc(n), *tag = malloc(n), *key = malloc(n);
            if (buf && tag && key && sh_json_decode_string(title, strlen(title), buf, n, NULL) &&
                sh_editor_tab_tag(salt, m->key, tag, n) && sh_editor_string_tag(salt, buf, key, n)) {
                const char *label = sh_json_object_get(translated, key + 5);
                if (!label) label = sh_json_object_get(english, key + 5);
                if (label) want_row(tag + 5, label, strlen(label), owner, STRIDS_LAYER_BUNDLE);
            }
            free(buf); free(tag); free(key);
        }
        sh_json_object_free(&entry);
    }
done:
    sh_json_object_free(&tabs);
    sh_json_object_free(&editor);
}

/* Each key keeps its identity across languages. Missing translations use that mod's English text. */
static void inject_bundle(void *context, const char *id, const char *hash, int place, const char *extra)
{
    strids_bundle_pass *pass = (strids_bundle_pass *)context;
    sh_json_object root = {0}, languages = {0}, pairs = {0}, translated = {0};
    const char *strings, *english, *selected;
    size_t before, name_cap, i;
    char *name;
    int owner, edits = 0, live = 1, own;
    if (!id) id = "";
    if (!extra || !sh_json_parse_object(extra, strlen(extra), STRIDS_BUNDLE_JSON_DEPTH, &root)) goto done;
    strings = sh_json_object_get(&root, "strings");
    if (!strings) goto done;
    if (*strings != '{' || !sh_json_parse_object(strings, strlen(strings), STRIDS_BUNDLE_JSON_DEPTH, &languages))
        goto refused;
    english = sh_json_object_get(&languages, "en");
    selected = strcmp(pass->locale, "en") ? sh_json_object_get(&languages, pass->locale) : NULL;
    if (!english && !selected) goto done;
    if (english && !decode_pairs(english, strlen(english), &pairs)) goto refused;
    if (selected && !decode_pairs(selected, strlen(selected), &translated)) goto refused;
    if (translated.count > 1)
        qsort(translated.members, translated.count, sizeof *translated.members, strids_pair_cmp);
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
    if (!place) strids_tab_labels(id, &root, &pairs, &translated, owner);
    /* A key the game or the product defines is an edit of that string and applies where the mod's other edits
     * do (an editor-tweak mod's in every map, any other mod's in maps that declare it). */
    for (i = 0; i < pairs.count; i++) edits |= sh_strkey_is_edit(pairs.members[i].key, pairs.members[i].key_length);
    for (i = 0; i < translated.count; i++) edits |= sh_strkey_is_edit(translated.members[i].key, translated.members[i].key_length);
    live = edits ? sh_modserve_bundle_edits_live(hash) : 0;
    own = strids_own_live(hash);
    AcquireSRWLockExclusive(&g_scoped_lock);
    if (strids_reserve((void **)&g_scoped, &g_scoped_capacity, g_scoped_count + 1, sizeof *g_scoped)) {
        strids_scoped *s = &g_scoped[g_scoped_count++];
        strncpy_s(s->hash, sizeof s->hash, hash ? hash : "", _TRUNCATE);   /* a build hash: 64 hex digits */
        s->own_live = own;
        s->edits_live = edits ? live : -1;
    } else {
        g_pass_failed = 1;
    }
    ReleaseSRWLockExclusive(&g_scoped_lock);
    for (i = 0; i < translated.count + pairs.count; i++) {
        sh_json_member *member;
        if (i < translated.count) member = &translated.members[i];
        else {
            member = &pairs.members[i - translated.count];
            if (translated.count && bsearch(member, translated.members, translated.count,
                                            sizeof *translated.members, strids_pair_cmp)) continue;
        }
        int edit = sh_strkey_is_edit(member->key, member->key_length);
        if (edit ? !live : !own) continue;
        want_row(member->key, member->value_json, strlen(member->value_json), owner,
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
                    "STRINGS: REFUSED the strings of bundle %s: expected each selected language to be a JSON "
                    "object of valid text strings and nonempty IDs", id);
        backend_log(line);
    }
done:
    sh_json_object_free(&translated);
    sh_json_object_free(&pairs);
    sh_json_object_free(&languages);
    sh_json_object_free(&root);
}

/* Admitted bundles in sh_modserve_each_bundle_extra's order. The layer is empty until the serving index
 * exists; each index build then requests a refresh (sh_strids_request_refresh). */
static void inject_bundles(void)
{
    strids_bundle_pass pass;
    pass.debug = strids_debug();
    pass.locale = sh_strids_locale();
    if (sh_modserve_each_bundle_extra(inject_bundle, &pass) < 0) {
        g_pass_failed = 1;
        backend_log("STRINGS: REFUSED: the bundle string layer could not be read (out of memory)");
    }
}

/* "N #str_ entries (bundles B, baked D)": the keys each layer won in the last pass. */
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
    AcquireSRWLockExclusive(&g_scoped_lock);
    g_scoped_count = 0;
    ReleaseSRWLockExclusive(&g_scoped_lock);
    free_bundle_names();
    memset(g_layer_rows, 0, sizeof g_layer_rows);

    /* Admitted bundles first. */
    inject_bundles();

    /* Baked defaults fill remaining keys. */
    for (size_t bi = 0; bi < B1_STRIDS_BAKED_COUNT; bi++)
        want_row(g_strids_baked[bi].id, g_strids_baked[bi].text, strlen(g_strids_baked[bi].text), STRIDS_OWNER_NONE,
                 STRIDS_LAYER_BAKED);

    /* The table changes only here, when the whole pass is planned. */
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

/* Re-sort the live table after appending, since native lookup binary-searches the +0x00 hash (Vulkan
 * comparator 0x1a2aa90). Calls the original sort body with radix 0x20 and the descriptor (array +0, count +8) as context. */
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

/* Claim the shared one-shot latch, inject and re-sort. Returns appended rows, or -1 if another path claimed it. */
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

static const sh_process_heap_api *g_heap_api;

/* An engine rebuild of its table (a language load as the editor or a map starts) sorts it and drops every row
 * this injector added, so a later top-level sort re-adds them first, in the process heap where the table lives;
 * when that heap cannot be selected on this thread, the next main-thread pass does it. */
static void reinject_in_sort(void **arr, uint32_t *count)
{
    sh_process_heap_scope heap;
    if (!hook_is_installed((void *)g_sort_orig) || !g_table_desc || !g_insert || !g_hash || !g_idstr_ctor) return;
    if (InterlockedCompareExchange(&g_refresh_busy, 1, 0)) return;   /* our own pass is sorting */
    if (!g_heap_api || !sh_process_heap_enter(g_heap_api, &heap)) {
        InterlockedExchange(&g_refresh_busy, 0);
        sh_strids_request_refresh();
        return;
    }
    __try {
        __try { (void)do_inject(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_pass_failed = 1; }
        __try {
            uint32_t live = *(uint32_t *)((uint8_t *)g_table_desc + 8);
            void *live_arr = *(void **)g_table_desc;
            if (live_arr) *arr = live_arr;
            if (live) *count = live;
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* keep the engine's arguments */ }
    } __finally {
        if (!sh_process_heap_leave(&heap)) backend_log("STRINGS: the game's heap selection could not be restored");
        InterlockedExchange(&g_refresh_busy, 0);
    }
}

/* Whether a sort orders the game's dictionary: its rows are the descriptor's live array. The engine passes a
 * stack comparator object as the first argument (dictionary parser VK 0x140280920, wrapper 0x140281d50), never
 * the descriptor, so the rows decide, not the context. */
static int sort_is_dictionary(const void *arr)
{
    __try {
        return arr != NULL && arr == *(void *const *)g_table_desc;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Sort detour. Install normally injects first; a later top-level dictionary sort re-adds this injector's rows
 * (reinject_in_sort); recursive sorts and sorts of other lists pass through. */
static void sh_sort_detour(void *ctx, void *arr, uint32_t count, uint32_t radix)
{
    if (g_sort_orig == NULL) return;   /* defensive: never happens once installed */

    LONG depth = InterlockedIncrement(&g_in_sort);
    if (depth == 1 && InterlockedCompareExchange(&g_injected, 0, 0) != 0 && g_table_desc && sort_is_dictionary(arr))
        reinject_in_sort(&arr, &count);
    else if (depth == 1 && InterlockedCompareExchange(&g_injected, 1, 0) == 0) {
        /* This top-level sort won the latch: append now and let this native sort order the enlarged table. */
        char summary[128], line[160];
        InterlockedExchange(&g_refresh_pending, 0);   /* this pass reads the published index */
        (void)do_inject();
        layer_summary(summary, sizeof summary);
        _snprintf_s(line, sizeof line, _TRUNCATE, "STRINGS: injected %s", summary);
        backend_log(line);
        /* The table grew: re-read the live array and count (idList: +0 array, +8 num, +0xc capacity) so the sort
         * orders the full table, not the pre-inject count. */
        __try {
            uint32_t live = *(uint32_t *)((uint8_t *)g_table_desc + 8);
            void    *live_arr = *(void **)g_table_desc;
            if (live_arr) arr = live_arr;
            if (live)     count = live;
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* keep the engine's args on any read fault */ }
    }

    g_sort_orig(ctx, arr, count, radix);
    /* The sort returns void; work after the call clobbers EAX, which the caller does not consume. */
    InterlockedDecrement(&g_in_sort);
}

int sh_strids_install(void *sort_body_fn,
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
    if (table_lea_fn == NULL || insert_fn == NULL || hash_fn == NULL || idstr_ctor_fn == NULL) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "STRINGS: injector SKIPPED -- missing engine fn (lea=%p insert=%p hash=%p ctor=%p)",
            table_lea_fn, insert_fn, hash_fn, idstr_ctor_fn);
        backend_log(line);
        return 0;
    }

    g_table_desc = (void *)sh_rip_slot((const uint8_t *)table_lea_fn, SH_RIP_RCX, SH_RIP_LEA);
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

    /* Built-in declarations can load at any time, so supply their text immediately. */
    inject_and_resort_once();
    return 1;
}

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
    /* An append that grows the table allocates from the main thread's current heap (the list's resize asks for
     * heap -1), which in a map or the editor is the map heap, freed with the map while the game's string lookups
     * still read the table. Every runtime pass therefore runs in the process heap where the table lives, or not at all. */
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
        /* A pass that wrote to the table leaves it sorted, a partial one included; an unplanned pass changes nothing. */
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
    size_t i;
    int changed = 0;
    AcquireSRWLockShared(&g_scoped_lock);
    for (i = 0; i < g_scoped_count && !changed; i++)
        changed = strids_own_live(g_scoped[i].hash) != g_scoped[i].own_live ||
                  (g_scoped[i].edits_live >= 0 &&
                   sh_modserve_bundle_edits_live(g_scoped[i].hash) != g_scoped[i].edits_live);
    ReleaseSRWLockShared(&g_scoped_lock);
    if (changed) sh_strids_request_refresh();
    return changed;
}

int sh_strids_refresh_pending(void)
{
    if (!InterlockedCompareExchange(&g_refresh_pending, 0, 0)) return -1;
    /* Before the first injection there is nothing to refresh; that injection reads the published index itself. */
    if (!InterlockedCompareExchange(&g_injected, 0, 0)) return -1;
    InterlockedExchange(&g_refresh_pending, 0);
    return sh_strids_rearm();
}

#ifdef SH_STRIDS_TESTING
/* The table-global decode the install runs, for the image-backed test. */
void *sh_strids_test_decode_table_global(const uint8_t *table_lea_fn)
{
    return (void *)sh_rip_slot(table_lea_fn, SH_RIP_RCX, SH_RIP_LEA);
}

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
