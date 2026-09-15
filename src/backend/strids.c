/* Inject user, package and baked #str_ mappings, then sort the live
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
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "strids.h"
#include "strids_baked.h"   /* the compiled-in canonical #str_ set (baked strids) */
#include "hook.h"
#include "backend_log.h"
#include "overrides.h"
#include "packages.h"
#include "package_runtime.h"
#include "user_overrides.h"
#include "config_json.h"

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
static int g_pass_failed;
static volatile LONG g_injected      = 0;      /* one-shot latch (0 = not yet injected) */
static volatile LONG g_in_sort       = 0;      /* recursion guard (>0 = inside the sort already) */
static volatile LONG g_inject_count  = 0;      /* rows appended (observability) */

/* User document path; default %LOCALAPPDATA%\snapmap-
 * plus\strings\strids.json.
 */
static char g_src_path[MAX_PATH] = {0};

static void default_source_path(char *out, size_t cap)
{
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\strings\\strids.json", base);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snapmap-plus\\strings\\strids.json");
}

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

/* Read the local document in chunks. Only a missing optional file is ignored;
 * allocation, size and I/O failures must not masquerade as an absent override. */
static char *read_file(const char *path, size_t *out_len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            g_pass_failed = 1;
        return NULL;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || (uint64_t)sz.QuadPart >= SIZE_MAX) {
        g_pass_failed = 1;
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (!buf) { g_pass_failed = 1; CloseHandle(h); return NULL; }
    size_t got = 0;
    while (got < n) {
        DWORD rd = 0;
        DWORD chunk = n - got > 1024u * 1024u ? 1024u * 1024u : (DWORD)(n - got);
        if (!ReadFile(h, buf + got, chunk, &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    if (got != n) { g_pass_failed = 1; HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

/* The user's own document: %LOCALAPPDATA%\snapmap-plus\strings\strids.json unless redirected. */
static char *read_source_file(size_t *out_len)
{
    char path[MAX_PATH];
    if (g_src_path[0]) strncpy_s(path, sizeof path, g_src_path, _TRUNCATE);
    else default_source_path(path, sizeof path);
    return read_file(path, out_len);
}

/* Deduplicate case-insensitively within an injection pass because the native
 * hash lowercases keys. First writer wins: user document, packages in
 * precedence order, then baked defaults. Duplicate native rows would make
 * hash lookup ambiguous.
 */
/* Who supplied a row, so a cross-package disagreement can name both sides. The user's file and the
 * baked defaults are not packages and never conflict-report: the user outranks a package by design and
 * the baked set is only a backstop, so both use STRIDS_OWNER_NONE. */
#define STRIDS_OWNER_NONE (-1)
#define STRIDS_OWNER_PACKAGES (-2)

typedef struct strids_row {
    const char  *id;          /* borrows the stable owned ID allocation */
    unsigned int text_hash;   /* FNV-1a of the text: "same value?" without storing every value */
    int          owner;       /* compiled packages or user/default layer */
} strids_row;

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

static unsigned char *find_live_owned(const strids_owned_row *row)
{
    unsigned char *array;
    uint32_t count;
    size_t i;
    unsigned char *found = NULL;
    if (!strids_table(&array, &count)) return NULL;
    for (i = strids_index_first(row->hash); i < g_index_n && g_index[i].hash == row->hash; i++) {
        if (g_index[i].key == row->key_handle) {
            unsigned char *entry;
            if (g_index[i].index >= count) return NULL;
            entry = array + (size_t)g_index[i].index * 32u;
            if (*(unsigned int *)entry != row->hash || *(void **)(entry + 8) != row->key_handle) return NULL;
            if (found) return NULL; /* Ambiguous ownership must not remove a row. */
            found = entry;
        }
    }
    return found;
}

/* Adopt an existing game row instead of appending a duplicate key. Native
 * AtomicString assignment and dictionary copies do not transfer ownership of
 * the pool's text; keeping the original record permits exact restoration. */
static int find_existing_key(unsigned int hash, const char *key, unsigned char **found)
{
    unsigned char *array;
    uint32_t count;
    size_t i;
    *found = NULL;
    if (!strids_table(&array, &count)) return 0;
    for (i = strids_index_first(hash); i < g_index_n && g_index[i].hash == hash; i++) {
        if (!_stricmp(g_index[i].key, key)) {
            unsigned char *row;
            if (g_index[i].index >= count) return 0;
            row = array + (size_t)g_index[i].index * 32u;
            if (*(uint32_t *)row != hash || *(const char **)(row + 8) != g_index[i].key) return 0;
            if (*found) return 0;
            *found = row;
        }
    }
    return 1;
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
    return owner == STRIDS_OWNER_PACKAGES ? "<packages>" : "<user>";
}

/* Append one #str_<id> row through native helpers unless this pass already
 * supplied the key.
 */
static void inject_row_owned(const char *id, const char *text, size_t text_len, int owner)
{
    strids_row *seen = find_injected(id);
    if (seen != NULL) {
        /* For differing values from two packages, retain the first and log
         * both owners. Equal values compose silently; user and baked
         * precedence does not produce a package conflict.
         */
        if (owner != STRIDS_OWNER_NONE && seen->owner != STRIDS_OWNER_NONE &&
            seen->text_hash != strids_text_hash(text)) {
            char line[320];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B1: strids REFUSED '#str_%s' from package '%s' -- package '%s' already defines "
                        "it with different text; the first definition stands",
                        id, strids_owner_name(owner), strids_owner_name(seen->owner));
            backend_log(line);
        }
        return;                                             /* first-writer-wins: never append twice */
    }
    size_t id_length = strlen(id);
    if (!id_length || id_length > INT_MAX - 5u || text_len > INT_MAX ||
        g_injected_n == SIZE_MAX ||
        !strids_reserve((void **)&g_injected_ids, &g_injected_capacity,
                        g_injected_n + 1, sizeof(*g_injected_ids))) {
        g_pass_failed = 1;
        return;
    }
    {
        strids_owned_row *owned = find_owned(id);
        unsigned char *live = NULL;
        unsigned char rec[32] = {0};
        unsigned char original[32] = {0};
        int replaced = 0;
        char *key = NULL, *saved_id = NULL;
        char *saved_text;
        uint32_t length = (uint32_t)text_len;
        if (owned) {
            live = find_live_owned(owned);
            if (!live) { g_pass_failed = 1; return; }
            if (strcmp(owned->text, text) == 0 && *(char **)(live + 16) &&
                strcmp(*(char **)(live + 16), text) == 0) goto record_seen;
            memcpy(rec, live, sizeof(rec));
        } else {
            unsigned char *array;
            uint32_t count;
            if (!strids_table(&array, &count) ||
                g_owned_n == SIZE_MAX ||
                !strids_reserve((void **)&g_owned_rows, &g_owned_capacity,
                                g_owned_n + 1, sizeof(*g_owned_rows))) {
                g_pass_failed = 1; return;
            }
            key = (char *)malloc(id_length + 6);
            saved_id = _strdup(id);
            if (!key || !saved_id) { free(key); free(saved_id); g_pass_failed = 1; return; }
            memcpy(key, "#str_", 5); memcpy(key + 5, id, id_length + 1);
            *(uint32_t *)rec = g_hash(key);
            if (!find_existing_key(*(uint32_t *)rec, key, &live) || (!live && count == INT_MAX)) {
                free(key); free(saved_id); g_pass_failed = 1; return;
            }
            if (live) {
                memcpy(original, live, sizeof(original));
                memcpy(rec, live, sizeof(rec)); replaced = 1;
            } else g_idstr_ctor(rec + 8, key);
            free(key);
        }
        saved_text = (char *)HeapAlloc(GetProcessHeap(), 0, text_len + 1);
        if (!saved_text) { free(saved_id); g_pass_failed = 1; return; }
        memcpy(saved_text, text, text_len + 1);
        /* AtomicString assignment interns text and writes its borrowed pointer.
         * It does not release the old value; dictionary rows own no pool memory. */
        g_idstr_ctor(live ? live + 16 : rec + 16, text);
        memcpy(live ? live + 24 : rec + 24, &length, 4);
        memcpy(live ? live + 28 : rec + 28, &length, 4);
        if (owned) {
            HeapFree(GetProcessHeap(), 0, owned->text);
        } else {
            if (!live) {
                uint32_t before = *(uint32_t *)((unsigned char *)g_table_desc + 8);
                g_insert(g_table_desc, rec);
                if (*(uint32_t *)((unsigned char *)g_table_desc + 8) != before + 1u) {
                    HeapFree(GetProcessHeap(), 0, saved_text);
                    free(saved_id);
                    g_pass_failed = 1;
                    return;
                }
                InterlockedIncrement(&g_inject_count);
            }
            owned = &g_owned_rows[g_owned_n++];
            memset(owned, 0, sizeof(*owned));
            owned->id = saved_id;
            owned->key_handle = *(void **)(rec + 8);
            owned->hash = *(uint32_t *)rec;
            owned->replaced = replaced;
            memcpy(owned->original, original, sizeof(original));
        }
        owned->text = saved_text;
record_seen:
        g_injected_ids[g_injected_n].id = owned->id;
        g_injected_ids[g_injected_n].text_hash = strids_text_hash(text);
        g_injected_ids[g_injected_n].owner = owner;
        g_injected_n++;
    }
}

static void inject_row(const char *id, const char *text, size_t text_len)
{
    inject_row_owned(id, text, text_len, STRIDS_OWNER_NONE);
}

typedef struct strids_retirement {
    uint32_t index;
    size_t owner;
} strids_retirement;

static int retirement_compare(const void *left, const void *right)
{
    const strids_retirement *a = left, *b = right;
    return a->index < b->index ? -1 : a->index > b->index;
}

/* At the main-thread refresh boundary, remove only absent product-owned rows.
 * Preflight the complete set before changing the list; compact once, retaining
 * unrelated rows and order. Native list resize/clear copies/frees only records,
 * never their AtomicString pointers. The engine pool remains untouched. */
static void retire_absent_rows(void)
{
    strids_retirement *retired;
    unsigned char *array;
    uint32_t count, read, written = 0;
    size_t i, n = 0, next = 0, kept = 0;
    if (g_pass_failed || !g_owned_n) return;
    if (g_owned_n > SIZE_MAX / sizeof(*retired) ||
        !(retired = malloc(g_owned_n * sizeof(*retired)))) { g_pass_failed = 1; return; }
    if (!strids_table(&array, &count)) { g_pass_failed = 1; goto done; }
    for (i = 0; i < g_owned_n; i++) if (!find_injected(g_owned_rows[i].id)) {
        unsigned char *row = find_live_owned(&g_owned_rows[i]);
        if (!row) { g_pass_failed = 1; goto done; }
        retired[n++] = (strids_retirement){(uint32_t)((row - array) / 32u), i};
    }
    if (!n) goto done;
    qsort(retired, n, sizeof(*retired), retirement_compare);
    for (i = 1; i < n; i++) if (retired[i - 1].index == retired[i].index) {
        g_pass_failed = 1; goto done;
    }
    for (read = 0; read < count; read++) {
        const unsigned char *source = array + (size_t)read * 32u;
        if (next < n && retired[next].index == read) {
            const strids_owned_row *owned = &g_owned_rows[retired[next++].owner];
            if (!owned->replaced) continue;
            source = owned->original;
        }
        memmove(array + (size_t)written++ * 32u, source, 32u);
    }
    *(uint32_t *)((unsigned char *)g_table_desc + 8) = written;
    for (i = 0; i < g_owned_n; i++) {
        if (!find_injected(g_owned_rows[i].id)) {
            HeapFree(GetProcessHeap(), 0, g_owned_rows[i].text);
            free(g_owned_rows[i].id);
        } else g_owned_rows[kept++] = g_owned_rows[i];
    }
    g_owned_n = kept;
done:
    free(retired);
}

/* Decode the whole flat dictionary before publishing any of its rows. The
 * shared JSON reader handles Unicode escapes and rejects malformed input.
 * Native text handles are NUL-terminated, so embedded NUL is unsupported.
 */
static void inject_pairs(const char *buf, size_t length, int owner)
{
    sh_json_object pairs = {0};
    size_t i;
    /* Windows text writers may prefix an otherwise ordinary UTF-8 document. */
    if (length >= 3 && memcmp(buf, "\xef\xbb\xbf", 3) == 0) {
        buf += 3;
        length -= 3;
    }
    if (!buf || !sh_json_parse_object(buf, length, 1, &pairs)) goto invalid;
    for (i = 0; i < pairs.count; i++) {
        sh_json_member *member = &pairs.members[i];
        size_t encoded_length = strlen(member->value_json), decoded_length = 0;
        if (!member->key_length || member->key_length > INT_MAX - 5u ||
            strlen(member->key) != member->key_length ||
            !sh_json_decode_string(member->value_json, encoded_length,
                                   member->value_json, encoded_length + 1, &decoded_length) ||
            strlen(member->value_json) != decoded_length || decoded_length > INT_MAX)
            goto invalid;
    }
    for (i = 0; i < pairs.count; i++)
        inject_row_owned(pairs.members[i].key, pairs.members[i].value_json,
                         strlen(pairs.members[i].value_json), owner);
    sh_json_object_free(&pairs);
    return;
invalid:
    g_pass_failed = 1;
    backend_log("B1: strids REFUSED: expected a JSON object of valid text strings and nonempty IDs");
    sh_json_object_free(&pairs);
}

/* Policies are already composed and checked before any native insertion. */
static void inject_packages(void)
{
    size_t length = 0;
    char *json;
    sh_json_object locales = {0};
    const char *english;
    const sh_package_compilation *compiled;
    if (!sh_user_overrides_enabled_for_launch()) return;
    compiled = sh_package_runtime_acquire();
    /* No provider means local/baked text only, including recovery of a failed
     * first activation. Package admission is the runtime transaction's job. */
    if (!compiled) { sh_package_runtime_release(); return; }
    /* A failed refresh may still have a valid prior snapshot. Copy it before
     * calling the native string pool; latest-refresh readiness is not ownership. */
    json = compiled ? sh_json_serialize_object(&compiled->policy.strings, 0, &length) : NULL;
    sh_package_runtime_release();
    if (!json || !sh_json_parse_object(json, length, 4, &locales)) {
        g_pass_failed = 1; free(json); return;
    }
    english = sh_json_object_get(&locales, "en");
    if (english) inject_pairs(english, strlen(english), STRIDS_OWNER_PACKAGES);
    sh_json_object_free(&locales); free(json);
}

/* Inject the user, package and baked layers. Return the appended row count. */
static long inject_layers(void)
{
    if (g_table_desc == NULL || g_insert == NULL || g_hash == NULL || g_idstr_ctor == NULL)
        return 0;

    g_injected_n = 0;   /* fresh dedup set for this inject pass */

    /* User values have highest precedence. */
    size_t len = 0;
    char *buf = read_source_file(&len);
    if (buf != NULL) {
        inject_pairs(buf, len, STRIDS_OWNER_NONE);
        HeapFree(GetProcessHeap(), 0, buf);
    } else {
        backend_log(g_pass_failed ? "B1: strids REFUSED: could not read the local string document" :
                    "B1: strids -- no user strids.json (optional); packages and baked defaults still apply");
    }

    /* Package strings fill keys the user did not supply. */
    inject_packages();

    /* Baked defaults fill remaining keys. */
    for (size_t bi = 0; bi < B1_STRIDS_BAKED_COUNT; bi++)
        inject_row(g_strids_baked[bi].id, g_strids_baked[bi].text, strlen(g_strids_baked[bi].text));

    retire_absent_rows();
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

    long n = do_inject();
    resort_table();

    char line[96];
    _snprintf_s(line, sizeof line, _TRUNCATE, "B1: strids injected %ld #str_ entries", n);
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
        long n = do_inject();
        char line[96];
        _snprintf_s(line, sizeof line, _TRUNCATE, "B1: strids injected %ld #str_ entries", n);
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
        backend_log("B1: strids injector SKIPPED -- StridsSortBody not resolved");
        return 0;
    }
    if (!sort_status_ok) {
        backend_log("B1: strids injector SKIPPED -- StridsSortBody resolved via hook-tolerant fallback "
                    "(prologue already hooked); not installing over an existing detour");
        return 0;
    }
    if (table_lea_fn == NULL || insert_fn == NULL || hash_fn == NULL || idstr_ctor_fn == NULL) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B1: strids injector SKIPPED -- missing engine fn (lea=%p insert=%p hash=%p ctor=%p)",
            table_lea_fn, insert_fn, hash_fn, idstr_ctor_fn);
        backend_log(line);
        return 0;
    }

    g_table_desc = decode_table_global((const uint8_t *)table_lea_fn);
    if (g_table_desc == NULL) {
        backend_log("B1: strids injector SKIPPED -- could not decode the table-global LEA "
                    "(StridsTableLea layout shifted?)");
        return 0;
    }
    g_insert     = (insert_fn_t)insert_fn;
    g_hash       = (hash_fn_t)hash_fn;
    g_idstr_ctor = (idstr_ctor_fn_t)idstr_ctor_fn;

    void *tramp = hook_prepare(sort_body_fn, (void *)sh_sort_detour, SORT_STOLEN);
    if (tramp == NULL) {
        backend_log("B1: strids injector FAIL -- could not prepare the original callback");
        g_table_desc = NULL; g_insert = NULL; g_hash = NULL; g_idstr_ctor = NULL;
        return 0;
    }
    g_sort_orig = (sort_fn_t)tramp;

    if (!g_src_path[0]) default_source_path(g_src_path, sizeof g_src_path);
    InterlockedExchange(&g_injected, 0);
    if (hook_commit(tramp) != B2_PATCH_OK) {
        InterlockedExchange(&g_injected, 1);
        if (hook_unpatch(tramp)) {
            g_sort_orig = NULL;
            g_table_desc = NULL; g_insert = NULL; g_hash = NULL; g_idstr_ctor = NULL;
        }
        backend_log("B1: strids injector FAIL -- hook commit failed; pending restoration retains the original callback");
        return 0;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: strids injector installed at %p (trampoline %p, stolen %d); table=%p; source=%s",
        sort_body_fn, tramp, SORT_STOLEN, g_table_desc, g_src_path);
    backend_log(line);

    /* Built-in declarations can load before compilation. Supply their text
     * now; the publication pass adds package policy after it becomes available. */
    inject_and_resort_once();
    return 1;
}

int sh_strids_rearm(void)
{
    int ok;
    if (!g_sort_orig || !hook_is_installed((void *)g_sort_orig) ||
        !g_table_desc || !g_insert || !g_hash || !g_idstr_ctor ||
        InterlockedCompareExchange(&g_refresh_busy, 1, 0)) return 0;
    g_pass_failed = 0;
    __try {
        __try { do_inject(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_pass_failed = 1; }
        /* Even a partial native append must leave the lookup table sorted. */
        resort_table();
        ok = !g_pass_failed;
    } __finally {
        InterlockedExchange(&g_refresh_busy, 0);
    }
    backend_log(ok ? "B1: strids runtime refresh complete" : "B1: strids runtime refresh FAILED");
    return ok;
}

int sh_strids_set_source(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        default_source_path(g_src_path, sizeof g_src_path);
        return 1;
    }
    strncpy_s(g_src_path, sizeof g_src_path, path, _TRUNCATE);
    return g_src_path[0] != '\0';
}

unsigned long sh_strids_injected_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_inject_count, 0, 0);
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

/* How the injector attributed each row this pass: the id, and the owning package name ("<user>" for
 * the user's own document and for the baked defaults). Returns 0 past the end. */
int sh_strids_test_row(int index, const char **id_out, const char **owner_out)
{
    if (index < 0 || (size_t)index >= g_injected_n) return 0;
    if (id_out)    *id_out    = g_injected_ids[index].id;
    if (owner_out) *owner_out = strids_owner_name(g_injected_ids[index].owner);
    return 1;
}
#endif /* SH_STRIDS_TESTING */
