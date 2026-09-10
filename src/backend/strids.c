/* Inject user, package and baked #str_ mappings once, then sort the live
 * dictionary by hash. Installation performs the usual injection; the sort
 * hook is a fallback.
 *
 * Native 32-byte record: +0x00 lowercased FNV-1a hash, +0x04 padding, +0x08
 * interned key handle, +0x10 interned value handle, and value lengths at
 * +0x18/+0x1c. The engine owns the string pool and destination list.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "strids.h"
#include "strids_baked.h"   /* the compiled-in canonical #str_ set (baked strids) */
#include "hook.h"
#include "backend_log.h"
#include "overrides.h"
#include "packages.h"

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

/* Scan bounded string:string pairs from a flat JSON object and unescape
 * values. Malformed pairs are skipped; parsing may retain a valid subset.
 */

/* Read a whole file into a fresh NUL-terminated heap buffer (caller HeapFrees), or NULL if it is
 * absent, unreadable, empty or implausibly large. Shared by the user's document and every package's. */
static char *read_file(const char *path, size_t *out_len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)(16 * 1024 * 1024)) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    size_t got = 0;
    while (got < n) {
        DWORD rd = 0;
        if (!ReadFile(h, buf + got, (DWORD)(n - got), &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    if (got != n) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
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

/* Scan one JSON "string" starting at *p (which must point AT the opening quote). Copies the unescaped
 * content into out[0..cap) NUL-terminated, advances *p past the closing quote. Returns out length, or
 * -1 if no well-formed string is found. */
static int scan_json_string(const char **p, char *out, size_t cap)
{
    const char *s = *p;
    if (*s != '"') return -1;
    s++;
    size_t o = 0;
    while (*s && *s != '"') {
        char c = *s++;
        if (c == '\\' && *s) {
            char e = *s++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                default:  c = e;    break;   /* unknown escape -> literal (engine warns; we keep it) */
            }
        }
        if (o + 1 < cap) out[o++] = c;
    }
    if (*s != '"') return -1;   /* unterminated */
    s++;
    out[o] = '\0';
    *p = s;
    return (int)o;
}

/* Deduplicate case-insensitively within an injection pass because the native
 * hash lowercases keys. First writer wins: user document, packages in
 * precedence order, then baked defaults. Duplicate native rows would make
 * hash lookup ambiguous.
 */
#define STRIDS_DEDUP_CAP 1024

/* Who supplied a row, so a cross-package disagreement can name both sides. The user's file and the
 * baked defaults are not packages and never conflict-report: the user outranks a package by design and
 * the baked set is only a backstop, so both use STRIDS_OWNER_NONE. */
#define STRIDS_OWNER_NONE (-1)

typedef struct strids_row {
    char         id[96];
    unsigned int text_hash;   /* FNV-1a of the text: "same value?" without storing every value */
    int          owner;       /* index into g_str_packages, or STRIDS_OWNER_NONE */
} strids_row;

static strids_row g_injected_ids[STRIDS_DEDUP_CAP];
static int        g_injected_n;

/* Keep the package snapshot in static storage to limit this engine callback's
 * stack use.
 */
static sh_package g_str_packages[SH_PACKAGES_MAX];
static size_t     g_str_package_count;

static unsigned int strids_text_hash(const char *text)
{
    unsigned int h = 2166136261u;
    for (; text && *text; text++) { h ^= (unsigned char)*text; h *= 16777619u; }
    return h;
}

static strids_row *find_injected(const char *id)
{
    for (int i = 0; i < g_injected_n; i++)
        if (_stricmp(g_injected_ids[i].id, id) == 0) return &g_injected_ids[i];
    return NULL;
}

static const char *strids_owner_name(int owner)
{
    if (owner < 0 || (size_t)owner >= g_str_package_count) return "<user>";
    return g_str_packages[owner].name;
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
    if (g_injected_n < STRIDS_DEDUP_CAP) {
        strncpy_s(g_injected_ids[g_injected_n].id, sizeof g_injected_ids[0].id, id, _TRUNCATE);
        g_injected_ids[g_injected_n].text_hash = strids_text_hash(text);
        g_injected_ids[g_injected_n].owner = owner;
        g_injected_n++;
    }

    char key[256];
    _snprintf_s(key, sizeof key, _TRUNCATE, "#str_%s", id);

    /* 32-byte record: { u32 hash; u32 pad; ptr keyHandle; ptr valHandle; u32 len; u32 len } */
    uint8_t rec[32];
    memset(rec, 0, sizeof rec);
    uint32_t h = g_hash(key);
    memcpy(rec + 0x00, &h, 4);
    g_idstr_ctor(rec + 0x08, key);    /* keyHandle = intern("#str_<id>") */
    g_idstr_ctor(rec + 0x10, text);   /* valHandle = intern(text)        */
    uint32_t vl = (uint32_t)text_len;
    memcpy(rec + 0x18, &vl, 4);
    memcpy(rec + 0x1c, &vl, 4);

    g_insert(g_table_desc, rec);      /* idList<StridEntry>::Append(tableDesc, &record) */
    InterlockedIncrement(&g_inject_count);
}

static void inject_row(const char *id, const char *text, size_t text_len)
{
    inject_row_owned(id, text, text_len, STRIDS_OWNER_NONE);
}

/* Scan one flat string:string document for this owner. Skip malformed pairs
 * one character at a time.
 */
static void inject_pairs(const char *buf, int owner)
{
    const char *p = buf;
    char id[256], text[4096];
    while (*p) {
        if (*p != '"') { p++; continue; }
        int idlen = scan_json_string(&p, id, sizeof id);
        if (idlen < 0) { p++; continue; }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != ':') continue;     /* not a key:value pair -- resume scanning from here */
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != '"') continue;
        int vlen = scan_json_string(&p, text, sizeof text);
        if (vlen < 0) continue;
        if (idlen > 0) inject_row_owned(id, text, (size_t)vlen, owner);
    }
}

/* Read first-level strings/*.json from every installed package in precedence
 * order.
 */
static void inject_packages(void)
{
    char root[MAX_PATH], dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA found;
    size_t i;

    g_str_package_count = 0;
    if (!sh_overrides_get_root(root, sizeof root) || !root[0]) return;
    /* Retain the enumerated subset on failure; omitted package strings are
     * not injected.
     */
    (void)sh_packages_enumerate(root, g_str_packages, SH_PACKAGES_MAX, &g_str_package_count);

    for (i = 0; i < g_str_package_count; i++) {
        HANDLE search;
        if (!sh_package_subdir(&g_str_packages[i], "strings", dir, sizeof dir)) continue;
        if (_snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*.json", dir) < 0) continue;
        search = FindFirstFileA(pattern, &found);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            size_t len = 0;
            char *buf;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, found.cFileName) < 0) continue;
            buf = read_file(path, &len);
            if (buf == NULL) continue;
            inject_pairs(buf, (int)i);
            HeapFree(GetProcessHeap(), 0, buf);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
}

/* Inject the user, package and baked layers. Return the appended row count. */
static long do_inject(void)
{
    if (g_table_desc == NULL || g_insert == NULL || g_hash == NULL || g_idstr_ctor == NULL)
        return 0;

    g_injected_n = 0;   /* fresh dedup set for this inject pass */

    /* User values have highest precedence. */
    size_t len = 0;
    char *buf = read_source_file(&len);
    if (buf != NULL) {
        inject_pairs(buf, STRIDS_OWNER_NONE);
        HeapFree(GetProcessHeap(), 0, buf);
    } else {
        backend_log("B1: strids -- no user strids.json (optional); packages and baked defaults still apply");
    }

    /* Package strings fill keys the user did not supply. */
    inject_packages();

    /* Baked defaults fill remaining keys. */
    for (size_t bi = 0; bi < B1_STRIDS_BAKED_COUNT; bi++)
        inject_row(g_strids_baked[bi].id, g_strids_baked[bi].text, strlen(g_strids_baked[bi].text));

    return (long)InterlockedCompareExchange(&g_inject_count, 0, 0);
}

/* Re-sort the entire live table after appending: native lookup binary-
 * searches its +0x00 hash key. Call the original sort body with radix 0x20
 * and the descriptor as context. The descriptor holds its array at +0 and
 * count at +8. Pinned Vulkan lookup comparator: 0x1a2aa90.
 */
static void resort_table(void)
{
    if (g_sort_orig == NULL || g_table_desc == NULL) return;
    __try {
        void    *live_arr = *(void **)g_table_desc;
        uint32_t live_cnt = *(uint32_t *)((uint8_t *)g_table_desc + 8);
        if (live_arr != NULL && live_cnt > 1)
            g_sort_orig(g_table_desc, live_arr, live_cnt, SORT_RADIX);
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* bad descriptor read -> leave the table as-is */ }
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
    if (g_sort_orig != NULL) {
        backend_log("B1: strids injector already installed");
        return 1;
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

    void *tramp = install_inline_hook(sort_body_fn, (void *)sh_sort_detour, SORT_STOLEN);
    if (tramp == NULL) {
        backend_log("B1: strids injector FAIL -- install_inline_hook returned NULL");
        g_table_desc = NULL; g_insert = NULL; g_hash = NULL; g_idstr_ctor = NULL;
        return 0;
    }
    g_sort_orig = (sort_fn_t)tramp;

    if (!g_src_path[0]) default_source_path(g_src_path, sizeof g_src_path);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: strids injector installed at %p (trampoline %p, stolen %d); table=%p; source=%s",
        sort_body_fn, tramp, SORT_STOLEN, g_table_desc, g_src_path);
    backend_log(line);

    /* Inject and re-sort now because the engine's startup language sort
     * precedes deferred installation. The shared latch prevents a later
     * native sort from duplicating rows.
     */
    inject_and_resort_once();
    return 1;
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
    g_table_desc = table_desc;
    g_insert     = (insert_fn_t)insert;
    g_hash       = (hash_fn_t)hash;
    g_idstr_ctor = (idstr_ctor_fn_t)idstr_ctor;
    InterlockedExchange(&g_inject_count, 0);
    g_injected_n = 0;
    return (int)do_inject();
}

/* How the injector attributed each row this pass: the id, and the owning package name ("<user>" for
 * the user's own document and for the baked defaults). Returns 0 past the end. */
int sh_strids_test_row(int index, const char **id_out, const char **owner_out)
{
    if (index < 0 || index >= g_injected_n) return 0;
    if (id_out)    *id_out    = g_injected_ids[index].id;
    if (owner_out) *owner_out = strids_owner_name(g_injected_ids[index].owner);
    return 1;
}
#endif /* SH_STRIDS_TESTING */
