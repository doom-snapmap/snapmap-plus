/* Multiplayer snapshot templates for package rows a map does not reach (R-398 perf17); see the header. */
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snapshot_templates.h"
#include "package_compiler.h"
#include "package_runtime.h"
#include "signatures.h"
#include "host_image.h"
#include "hook.h"

void backend_log(const char *message);

#pragma intrinsic(_ReturnAddress)

/* ---- Private names and the reach set ---- */
static int st_hex(int c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1; }
static int st_digest(const char *text, unsigned char out[32])
{
    for (int i = 0; i < 32; i++) {
        int high = st_hex((unsigned char)text[2 * i]), low = st_hex((unsigned char)text[2 * i + 1]);
        if (high < 0 || low < 0) return 0;
        out[i] = (unsigned char)(high << 4 | low);
    }
    return 1;
}
int sh_snapshot_templates_private_name(const char *name)
{
    unsigned char digest[32];
    return name && !strncmp(name, "smp/", 4) && strlen(name) == 68 && st_digest(name + 4, digest);
}
static int st_compare(const void *a, const void *b) { return memcmp(a, b, 32); }
static int st_find(unsigned char (*sorted)[32], size_t count, const unsigned char digest[32])
{ return count && bsearch(digest, sorted, count, 32, st_compare) != NULL; }
int sh_snapshot_reach_contains(const sh_snapshot_reach *reach, const char *name)
{
    unsigned char digest[32];
    if (!reach || !sh_snapshot_templates_private_name(name)) return 0;
    st_digest(name + 4, digest);
    return st_find(reach->digests, reach->count, digest);
}
void sh_snapshot_reach_free(sh_snapshot_reach *reach)
{ if (reach) { free(reach->digests); memset(reach, 0, sizeof(*reach)); } }

/* The served rows carrying each private name, sorted by digest (one name may carry several families). */
typedef struct st_row { unsigned char digest[32]; size_t index; } st_row;
static int st_row_compare(const void *a, const void *b) { return memcmp(a, b, 32); }
typedef struct st_build {
    const sh_package_compilation *compiled;
    st_row *rows; size_t row_count;
    unsigned char (*seen)[32]; size_t seen_count, seen_capacity; /* unsorted while building */
    unsigned char *marked;      /* per private row: queued */
    size_t *queue; size_t queue_count;
    size_t scanned;
    int failed;
} st_build;
static void st_mark(st_build *b, const unsigned char digest[32])
{
    st_row key, *row;
    memcpy(key.digest, digest, 32);
    row = bsearch(&key, b->rows, b->row_count, sizeof(*b->rows), st_row_compare);
    if (!row) return;
    while (row > b->rows && !memcmp(row[-1].digest, digest, 32)) row--;
    if (b->marked[row - b->rows]) return;
    if (b->seen_count == b->seen_capacity) {
        size_t next = b->seen_capacity ? b->seen_capacity * 2 : 256;
        void *grown = realloc(b->seen, next * 32);
        if (!grown) { b->failed = 1; return; }
        b->seen = grown; b->seen_capacity = next;
    }
    memcpy(b->seen[b->seen_count++], digest, 32);
    for (st_row *at = row; at < b->rows + b->row_count && !memcmp(at->digest, digest, 32); at++) {
        b->marked[at - b->rows] = 1;
        b->queue[b->queue_count++] = at->index;
    }
}
/* Every "smp/<64 hex>" the served text names. */
static void st_scan(st_build *b, size_t index)
{
    const sh_compiled_resource *r = b->compiled->resources + index;
    const unsigned char *text = r->presented ? r->presented : r->body;
    size_t length = r->presented ? r->presented_length : r->body_length;
    unsigned char *read = NULL;
    char error[160];
    b->scanned++;
    if (!text) {
        read = sh_package_compilation_read_served(b->compiled, r, SIZE_MAX, &length, error, sizeof(error));
        if (!read) return; /* unreadable text names nothing the engine could read either */
        text = read;
    }
    for (size_t i = 0; i + 68 <= length; i++) {
        unsigned char digest[32];
        if (text[i] != 's' || memcmp(text + i, "smp/", 4)) continue;
        if (!st_digest((const char *)text + i + 4, digest) || (i + 68 < length && st_hex(text[i + 68]) >= 0)) continue;
        st_mark(b, digest);
        if (b->failed) break;
        i += 67;
    }
    free(read);
}
static int st_map_delivered(const sh_package_compilation *c, const sh_compiled_resource *r)
{
    return c->map_overlay && (!sh_package_owners_within(&r->owners, c->map_owner_begin) ||
        !sh_package_owners_within(&r->gameplay_owners, c->map_owner_begin));
}
int sh_snapshot_reach_build(const sh_package_compilation *compiled, sh_snapshot_reach *out)
{
    st_build b;
    size_t privates = 0, roots = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!compiled) return 1;
    memset(&b, 0, sizeof(b));
    b.compiled = compiled;
    for (size_t i = 0; i < compiled->resource_count; i++)
        if (compiled->resources[i].type && sh_snapshot_templates_private_name(compiled->resources[i].name)) privates++;
    b.rows = privates ? malloc(privates * sizeof(*b.rows)) : NULL;
    b.marked = privates ? calloc(privates, 1) : NULL;
    b.queue = privates ? malloc(privates * sizeof(*b.queue)) : NULL;
    if (privates && (!b.rows || !b.marked || !b.queue)) goto failed;
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (!r->type || !sh_snapshot_templates_private_name(r->name)) continue;
        st_digest(r->name + 4, b.rows[b.row_count].digest); b.rows[b.row_count++].index = i;
    }
    qsort(b.rows, b.row_count, sizeof(*b.rows), st_row_compare);
    /* Roots: every served declaration an engine name reaches, and every row the current map delivers. */
    for (size_t i = 0; i < compiled->resource_count && !b.failed; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (!r->type) continue;
        if (!sh_snapshot_templates_private_name(r->name)) { roots++; st_scan(&b, i); }
        else if (st_map_delivered(compiled, r)) {
            unsigned char digest[32];
            st_digest(r->name + 4, digest); roots++; st_mark(&b, digest);
        }
    }
    while (b.queue_count && !b.failed) st_scan(&b, b.queue[--b.queue_count]);
    if (b.failed) goto failed;
    qsort(b.seen, b.seen_count, 32, st_compare);
    out->digests = b.seen; out->count = out->capacity = b.seen_count;
    out->roots = roots; out->scanned = b.scanned;
    free(b.rows); free(b.marked); free(b.queue);
    return 1;
failed:
    free(b.rows); free(b.marked); free(b.queue); free(b.seen);
    return 0;
}

#define ST_NAME 0x8 /* idResource: its name pointer */
size_t sh_snapshot_templates_filter(void **entries, int count, const sh_snapshot_reach *reach)
{
    size_t cleared = 0;
    for (int i = 0; entries && i < count; i++) {
        const char *name;
        if (!entries[i]) continue;
        __try { name = *(const char *const *)((const unsigned char *)entries[i] + ST_NAME); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (sh_snapshot_templates_private_name(name) && !sh_snapshot_reach_contains(reach, name)) {
            entries[i] = NULL; cleared++;
        }
    }
    return cleared;
}

/* ---- The detour ---- */
typedef void (*st_list_all_fn)(void *list, void *out, char sort);
static st_list_all_fn g_list_all;
static void *g_list_hook;
static void *g_entitydefs;
static const void *g_first_return, *g_second_return;
static volatile LONG g_installed;
/* The reach set of one served view, kept for the client pass of the same map load. */
static SRWLOCK g_reach_lock = SRWLOCK_INIT;
static sh_snapshot_reach g_reach;
static const sh_package_compilation *g_reach_compiled;
static size_t g_reach_resources;
static const sh_compiled_resource *g_reach_rows;
static int g_reach_valid;

static int st_enabled(void)
{
    char *value = NULL; size_t length = 0;
    int off = !_dupenv_s(&value, &length, "SMP_SNAPSHOT_TEMPLATE_FILTER") && value && !strcmp(value, "0");
    free(value); return !off;
}
static void st_filter_list(void *out, int refetch)
{
    void **entries; int count; size_t cleared;
    int held = 0, built = 0;
    ULONGLONG began = GetTickCount64();
    const sh_package_compilation *compiled;
    char line[400];
    __try { entries = *(void ***)out; count = *(int *)((unsigned char *)out + 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!entries || count <= 0) return;
    compiled = sh_package_runtime_try_acquire(&held);
    if (!held) {
        backend_log("MPKG: snapshot templates: the package view is busy; the native entityDef list is used as is");
        return;
    }
    AcquireSRWLockExclusive(&g_reach_lock);
    if (!g_reach_valid || g_reach_compiled != compiled || g_reach_resources != (compiled ? compiled->resource_count : 0) ||
        g_reach_rows != (compiled ? compiled->resources : NULL)) {
        sh_snapshot_reach_free(&g_reach);
        g_reach_valid = sh_snapshot_reach_build(compiled, &g_reach);
        g_reach_compiled = compiled; g_reach_resources = compiled ? compiled->resource_count : 0;
        g_reach_rows = compiled ? compiled->resources : NULL;
        built = 1;
    }
    sh_package_runtime_release();
    if (!g_reach_valid) {
        ReleaseSRWLockExclusive(&g_reach_lock);
        backend_log("MPKG: snapshot templates: the reach of the served view could not be computed; the native entityDef list is used as is");
        return;
    }
    cleared = sh_snapshot_templates_filter(entries, count, &g_reach);
    snprintf(line, sizeof(line), "MPKG: snapshot templates: %zu of %d entityDef(s) left out (package rows this map does not "
        "reach; %zu private name(s) reached from %zu root(s)%s) in %llu ms%s", cleared, count, g_reach.count, g_reach.roots,
        built ? "" : ", kept from the previous pass", GetTickCount64() - began, refetch ? " (list refetched)" : "");
    ReleaseSRWLockExclusive(&g_reach_lock);
    backend_log(line);
}
static void st_list_all(void *list, void *out, char sort)
{
    const void *from = _ReturnAddress();
    g_list_all(list, out, sort);
    if (list == g_entitydefs && (from == g_first_return || from == g_second_return) && st_enabled())
        st_filter_list(out, from == g_second_return);
}

/* "SnapshotTemplateList" (signatures.c) starts at the list copy's setup: MOV [RBP-0x60],0x850000;
 * MOV [RBP-0x70],R12; MOV [RBP-0x68],R12; XOR R8D,R8D; LEA RDX,[RBP-0x70]; LEA RCX,[entityDefs];
 * CALL get-all. The refetch repeats the last three instructions at +0x12d. */
static int st_verify(const unsigned char *site, void **list, void **target, const void **first, const void **second)
{
    int32_t lea, call, lea2, call2;
    const unsigned char *function;
    __try {
        if (memcmp(site + 0x0f, "\x45\x33\xc0\x48\x8d\x55\x90\x48\x8d\x0d", 10) || site[0x1d] != 0xe8 ||
            memcmp(site + 0x12d, "\x45\x33\xc0\x48\x8d\x55\x90\x48\x8d\x0d", 10) || site[0x13b] != 0xe8) return 0;
        memcpy(&lea, site + 0x19, 4); memcpy(&call, site + 0x1e, 4);
        memcpy(&lea2, site + 0x137, 4); memcpy(&call2, site + 0x13c, 4);
        if (site + 0x1d + lea != site + 0x13b + lea2 || site + 0x22 + call != site + 0x140 + call2) return 0;
        function = site + 0x22 + call;
        /* PUSH RSI; PUSH RDI; PUSH R12; PUSH R14; PUSH R15; SUB RSP,0x40; MOV [RSP+0x20],-2: 22 whole bytes. */
        if (memcmp(function, "\x40\x56\x57\x41\x54\x41\x56\x41\x57\x48\x83\xec\x40\x48\xc7\x44\x24\x20\xfe\xff\xff\xff", 22))
            return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    *list = (void *)(site + 0x1d + lea); *target = (void *)function;
    *first = site + 0x22; *second = site + 0x140;
    return 1;
}
int sh_snapshot_templates_install(void)
{
    const uint8_t *base = sh_host_image_base();
    const sig_entry *entry = NULL;
    sig_result result;
    void *list, *target, *trampoline;
    const void *first, *second;
    if (InterlockedCompareExchange(&g_installed, 0, 0)) return 1;
    for (const sig_entry *e = BACKEND_ENGINE_SIGNATURES; e->name; e++) if (!strcmp(e->name, "SnapshotTemplateList")) entry = e;
    if (!base || !entry || sig_resolve_one(base, entry, &result) != SIG_OK ||
        !st_verify((const unsigned char *)result.addr, &list, &target, &first, &second)) {
        backend_log("MPKG: snapshot templates: the map load's template list was not recognized; every registered entityDef keeps its template");
        return 0;
    }
    g_entitydefs = list; g_first_return = first; g_second_return = second;
    trampoline = hook_prepare(target, (void *)st_list_all, 22);
    if (!trampoline) goto failed;
    g_list_all = (st_list_all_fn)trampoline; g_list_hook = trampoline;
    if (hook_commit(trampoline) != B2_PATCH_OK) {
        if (hook_unpatch(trampoline)) { g_list_all = NULL; g_list_hook = NULL; }
        goto failed;
    }
    InterlockedExchange(&g_installed, 1);
    backend_log("MPKG: snapshot templates: multiplayer map loads template only the package rows the map reaches");
    return 1;
failed:
    backend_log("MPKG: snapshot templates: the entityDef list hook could not be installed; every registered entityDef keeps its template");
    return 0;
}

#ifdef SH_SNAPSHOT_TEMPLATES_TESTING
void sh_snapshot_templates_test_bind(sh_snapshot_list_all_fn original, void *list,
    const void *first_return, const void *second_return)
{ g_list_all = original; g_entitydefs = list; g_first_return = first_return; g_second_return = second_return; }
void sh_snapshot_templates_test_call(void *list, void *out, char sort, const void *return_address)
{
    g_list_all(list, out, sort);
    if (list == g_entitydefs && (return_address == g_first_return || return_address == g_second_return) && st_enabled())
        st_filter_list(out, return_address == g_second_return);
}
void sh_snapshot_templates_test_reset(void)
{
    AcquireSRWLockExclusive(&g_reach_lock);
    sh_snapshot_reach_free(&g_reach); g_reach_valid = 0; g_reach_compiled = NULL; g_reach_resources = 0;
    ReleaseSRWLockExclusive(&g_reach_lock);
}
#endif
