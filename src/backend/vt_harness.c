/* vt_harness.c -- lane RC research harness (see vt_harness.h).
 *
 * Every engine address comes from the signature database (VtSetSource,
 * VtForMaterial, Mega2PageDecode) or is decoded from inside one of those
 * signed functions with its surrounding bytes checked; a missing or hooked
 * signature, or an unexpected byte, refuses the verbs that need it.
 * Registration and release run on DOOM's main thread inside process heap 0,
 * under SEH. Decoding is pure CPU work on a private thread with an 8 MB stack,
 * as megapreview.c runs the same decoder.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "vt_harness.h"
#include "signatures.h"
#include "commands.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "host_image.h"
#include "process_heap_scope.h"
#include "apply_engine.h"
#include "modserve.h"

/* Record fields (both renderers; lane RC Ghidra jobs setsource-*, release-*). */
#define REC_TYPE        0x000
#define REC_OWNER       0x008
#define REC_MAGIC       0x010
#define REC_GRID        0x01C
#define REC_LEVELS      0x024
#define REC_PATH        0x070
#define REC_NAME        0x1A0
#define REC_REFERENCED  0x2C0
#define REC_PAGESFILE   0x2D0
#define REC_READER      0x2E8
#define REC_MEGA2       0x2F0
#define REC_TEXELS      0x2FC
#define REC_PAGETABLE   0x3D8
#define REC_PTIMAGE     0x3E8
#define REC_HOSTPT      0x3F8
#define OWNER_ROWS      0x138
#define OWNER_COUNT     0x140
#define TABLE_SLOTS     32
#define REFERENCED_OFF  0x118   /* from the table: count, then 100 x 256-byte paths at +0x11C */

#define PAGE_HDR        0x170u
#define PAGE_OUT        0x50000u
#define PAGE_MAX        0x40000u
#define PAGE_SLACK      0x40000u
#define DECODE_STACK    (8u << 20)

typedef char (*setsource_fn)(void *rec, const char *name, const char *path, int flavor, int type,
                             char direct, void *reader);
typedef void (*release_fn)(void *rec);
typedef void (*decode_fn)(const unsigned char *hdr, const unsigned char *payload, void *unused,
                          unsigned char *out);

static setsource_fn g_setsource;
static release_fn g_release;
static decode_fn g_decode;
static uint8_t *g_sys;
static uint32_t g_table;           /* offset of the record pointer table in sys */
static const uint64_t *g_sparse_ext;   /* OpenGL: __glewTexStorageSparseAMD */
static const int32_t *g_sparse_cvar;   /* OpenGL: the cvar int the sparse test reads */
static int g_vulkan = -1;
static int g_bound;
static sh_process_heap_api g_heap;
static int g_heap_ok;
static const uint8_t *g_main_thread_at;
static uint32_t g_mine;            /* bit k: record k registered by this harness */

/* ------------------------------------------------------------------ pure helpers */

int sh_vt_find_setsource_head(const uint8_t *code, size_t len)
{
    size_t i;
    if (!code || len < 20) return -1;
    for (i = 0; i + 20 <= len; ++i)
        if (code[i] == 0x48 && code[i + 1] == 0x8D && code[i + 2] == 0x0D && code[i + 7] == 0xE8 &&
            code[i + 12] == 0x48 && code[i + 13] == 0x8B && code[i + 14] == 0xCE && code[i + 15] == 0xE8)
            return (int)i;
    return -1;
}

uint32_t sh_vt_find_table_disp(const uint8_t *code, size_t len)
{
    size_t i;
    uint32_t d;
    if (!code || len < 8) return 0;
    for (i = 0; i + 8 <= len; ++i)
        if (code[i] == 0x49 && code[i + 1] == 0x8B && code[i + 2] == 0x85 && code[i + 7] == 0xE9) {
            memcpy(&d, code + i + 3, 4);
            return d;
        }
    return 0;
}

void sh_vt_swap_page_header(uint8_t *p)
{
    int k;
    for (k = 4; k <= 0xC; k += 2) {
        uint8_t t = p[k];
        p[k] = p[k + 1];
        p[k + 1] = t;
    }
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

long sh_vt_member_cell(const uint8_t *h, int level, unsigned x, unsigned y)
{
    uint32_t levels, lx, ly, lw, lh, first;
    const uint8_t *e;
    if (!h || rd32(h) != 0xA63FBB21u || rd32(h + 4) != 2) return -1;
    levels = rd32(h + 0x0C);
    if (level < 0 || (uint32_t)level >= levels || levels > 12) return -1;
    e = h + 0x50 + 0x18 * level;
    lx = rd32(e); ly = rd32(e + 4); lw = rd32(e + 8); lh = rd32(e + 12); first = rd32(e + 16);
    if (x < lx || y < ly || x >= lx + lw || y >= ly + lh) return -1;
    return (long)(first + (y - ly) * lw + (x - lx));
}

#ifndef SH_VT_HARNESS_PURE   /* the unit test links only the pure helpers above */

/* ------------------------------------------------------------------ log */

static void vt_line(const char *fmt, ...)
{
    char line[1200];
    va_list ap;
    memcpy(line, "VT: ", 4);
    va_start(ap, fmt);
    _vsnprintf_s(line + 4, sizeof line - 4, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
    sh_printf("%s\n", line);
}

/* ------------------------------------------------------------------ guarded reads */

static int rd_ptr(const void *p, void **v) { __try { *v = *(void *const *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { *v = NULL; return 0; } }
static int rd_i32(const void *p, int *v) { __try { *v = *(const int *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { *v = 0; return 0; } }
static int rd_i16(const void *p, short *v) { __try { *v = *(const short *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { *v = 0; return 0; } }

static void rd_cstr(const void *slot, char *out, size_t cap)
{
    const char *s = NULL;
    out[0] = 0;
    if (!rd_ptr(slot, (void **)&s) || !s) { strcpy_s(out, cap, "(null)"); return; }
    __try { strncpy_s(out, cap, s, _TRUNCATE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, cap, "(unreadable)"); }
}

static int rd_code(const uint8_t *p, uint8_t *out, size_t n)
{
    __try { memcpy(out, p, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* ------------------------------------------------------------------ records */

static uint8_t *rec_at(int k)
{
    void *r = NULL;
    if (!g_sys || !g_table || k < 0 || k >= TABLE_SLOTS) return NULL;
    rd_ptr(g_sys + g_table + 8 * k, &r);
    return (uint8_t *)r;
}

static int rec_free(uint8_t *r)
{
    char path[8];
    rd_cstr(r + REC_PATH, path, sizeof path);
    return path[0] == 0;
}

static int rec_find(const char *name)
{
    int k;
    char n[260];
    for (k = 1; k < TABLE_SLOTS; ++k) {
        uint8_t *r = rec_at(k);
        if (!r) continue;
        rd_cstr(r + REC_NAME, n, sizeof n);
        if (n[0] && !strcmp(n, name)) return k;
    }
    return -1;
}

static void rec_print(int k, uint8_t *r)
{
    char name[260], path[300];
    int type = 0, grid = 0, texels = 0, flavor = -1, rows = -1, refd = 0, magic = 0;
    short levels = 0;
    void *owner = NULL, *pages = NULL, *reader = NULL, *mega2 = NULL, *pt = NULL, *ptimg = NULL, *host = NULL;
    rd_cstr(r + REC_NAME, name, sizeof name);
    rd_cstr(r + REC_PATH, path, sizeof path);
    rd_i32(r + REC_TYPE, &type);
    rd_i32(r + REC_MAGIC, &magic);
    rd_i32(r + REC_GRID, &grid);
    rd_i16(r + REC_LEVELS, &levels);
    rd_i32(r + REC_TEXELS, &texels);
    rd_i32(r + REC_REFERENCED, &refd);
    rd_ptr(r + REC_OWNER, &owner);
    rd_ptr(r + REC_PAGESFILE, &pages);
    rd_ptr(r + REC_READER, &reader);
    rd_ptr(r + REC_MEGA2, &mega2);
    rd_ptr(r + REC_PAGETABLE, &pt);
    rd_ptr(r + REC_PTIMAGE, &ptimg);
    rd_ptr(r + REC_HOSTPT, &host);
    if (pt) rd_i32(pt, &flavor);
    if (owner) rd_i32((uint8_t *)owner + OWNER_COUNT, &rows);
    if (!name[0] && !path[0] && !type) { vt_line("  [%2d] free", k); return; }
    vt_line("  [%2d] %s'%s' type %d flavor %d path '%s'", k, (g_mine >> k) & 1 ? "(harness) " : "", name, type,
            flavor, path);
    vt_line("       reader %s (mega2 %p, pages file %p, reader %p); header magic 0x%08X grid %d levels %d "
            "texels %d; page table %p image %p host %p; table rows %d; streamed %d",
            mega2 ? "mega2" : pages ? ".pages" : reader ? "other" : "none", mega2, pages, reader,
            (unsigned)magic, grid, (int)levels, texels, pt, ptimg, host, rows, refd);
}

/* Host page-table bytes SetSource allocates for `levels` (VirtualTexture.cpp(1607)). */
static unsigned long long host_bytes(int levels)
{
    unsigned long long sum = 0;
    int i;
    for (i = levels - 1; i >= 0; --i) sum += 2ull << (2 * i);
    return sum;
}

/* ------------------------------------------------------------------ main thread */

static int on_main(void)
{
    uint32_t id = 0;
    if (!g_main_thread_at) return -1;
    __try { id = *(const uint32_t *)g_main_thread_at; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!id) return -1;
    return id == GetCurrentThreadId() ? 1 : 0;
}

typedef struct vt_call {
    char name[256];
    char path[300];
    int flavor;
    int verb;      /* 1 register, 2 release */
} vt_call;

static int g_fault;
static int vt_filter(DWORD code) { g_fault = (int)code; return EXCEPTION_EXECUTE_HANDLER; }

static int auto_flavor(void)
{
    uint64_t ext = 0;
    int32_t cvar = 0;
    if (g_vulkan) return 2;
    if (!g_sparse_ext || !g_sparse_cvar) return 2;
    __try { ext = *g_sparse_ext; cvar = *g_sparse_cvar; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 2; }
    return (ext && cvar) ? 4 : 2;
}

static int do_register(vt_call *c)
{
    sh_process_heap_scope heap = { 0 };
    int k, slot = -1, ok = 0;
    char result = 0;
    uint8_t *r;
    double t1;
    LARGE_INTEGER f, a, b;
    if (rec_find(c->name) >= 0) { vt_line("register: a record named '%s' exists already (sh_vt list)", c->name); return 0; }
    /* Record 31 is released by VirtualTextureForMaterial on every miss: never use it. */
    for (k = 1; k < TABLE_SLOTS - 1 && slot < 0; ++k) {
        r = rec_at(k);
        if (r && rec_free(r)) slot = k;
    }
    if (slot < 0) { vt_line("register: no free record among 1..30"); return 0; }
    r = rec_at(slot);
    if (!sh_process_heap_enter(&g_heap, &heap)) { vt_line("register: process heap 0 could not be entered; nothing done"); return 0; }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    g_fault = 0;
    __try {
        __try { result = g_setsource(r, c->name, c->path, c->flavor, 2, 0, NULL); ok = 1; }
        __except (vt_filter(GetExceptionCode())) { ok = 0; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) vt_line("register: the process heap scope did not balance");
    }
    QueryPerformanceCounter(&b);
    t1 = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (!ok) { vt_line("register: SetSource FAULTED (exception 0x%08X) on record %d", (unsigned)g_fault, slot); return 0; }
    vt_line("register: SetSource(record %d, '%s', '%s', flavor %d, type 2, direct 0, NULL) -> %d in %.1f ms",
            slot, c->name, c->path, c->flavor, (int)result, t1);
    if (result) {
        short levels = 0;
        g_mine |= 1u << slot;
        rd_i16(r + REC_LEVELS, &levels);
        vt_line("register: host page table %llu bytes for %d levels (plus the GPU page-table image)",
                host_bytes(levels), (int)levels);
    } else {
        vt_line("register: SetSource refused; the engine cleared the record (look for 'SetSource:' and "
                "'Using mega2 chunk file' lines above)");
    }
    rec_print(slot, r);
    return result ? 1 : 0;
}

static int do_release(vt_call *c)
{
    sh_process_heap_scope heap = { 0 };
    int k = rec_find(c->name), ok = 0;
    uint8_t *r;
    if (k < 0) { vt_line("release: no record named '%s'", c->name); return 0; }
    if (!((g_mine >> k) & 1)) { vt_line("release: record %d '%s' was not registered by this harness; refused", k, c->name); return 0; }
    r = rec_at(k);
    if (!sh_process_heap_enter(&g_heap, &heap)) { vt_line("release: process heap 0 could not be entered"); return 0; }
    g_fault = 0;
    __try {
        __try { g_release(r); ok = 1; }
        __except (vt_filter(GetExceptionCode())) { ok = 0; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) vt_line("release: the process heap scope did not balance");
    }
    if (!ok) { vt_line("release: FAULTED (exception 0x%08X) on record %d", (unsigned)g_fault, k); return 0; }
    g_mine &= ~(1u << k);
    vt_line("release: record %d '%s' released (materials that bound it still point at it)", k, c->name);
    rec_print(k, r);
    return 1;
}

static int main_call(void *ctx)
{
    vt_call *c = (vt_call *)ctx;
    return c->verb == 1 ? do_register(c) : do_release(c);
}

static void run_main(vt_call *c)
{
    sh_run_on_main_fn run = NULL;
    int result = 0, m = on_main();
    if (m == 1) { (void)main_call(c); return; }
    sh_apply_engine_get_run_on_main(&run);
    if (!run || run(NULL, main_call, c, 5000, &result) != 1)
        vt_line("%s: did not start on the main thread (no frame within 5 s)", c->verb == 1 ? "register" : "release");
}

/* ------------------------------------------------------------------ decode */

typedef struct vt_decode_job {
    unsigned char *page;   /* PAGE_MAX + PAGE_SLACK, zero tail */
    unsigned char *out;    /* PAGE_OUT */
    int faulted;
    DWORD code;
} vt_decode_job;

static DWORD WINAPI decode_thread(LPVOID p)
{
    vt_decode_job *j = (vt_decode_job *)p;
    __try { g_decode(j->page, j->page + 16, NULL, j->out); }
    __except (j->code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { j->faulted = 1; }
    return 0;
}

static int game_dir(char *out, size_t cap)
{
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (!n || n >= cap) return 0;
    slash = strrchr(out, '\\');
    if (!slash) return 0;
    slash[1] = 0;
    return 1;
}

static int out_path(const char *arg, char *out, size_t cap)
{
    char base[MAX_PATH];
    DWORD n;
    if ((arg[0] && arg[1] == ':') || arg[0] == '\\' || arg[0] == '/') { strncpy_s(out, cap, arg, _TRUNCATE); return 1; }
    n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!n || n >= sizeof base) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\vt", base);
    CreateDirectoryA(out, NULL);
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\vt\\%s", base, arg);
    return 1;
}

static int read_at(HANDLE h, unsigned long long off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, buf, n, &got, NULL) && got == n;
}

static int write_file(const char *path, const void *data, size_t n)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n;
    CloseHandle(h);
    return ok;
}

static void do_decode(const char *family, int level, unsigned x, unsigned y, const char *out_arg)
{
    char fam[260], request[320], disk[MAX_PATH * 2], dir[MAX_PATH], out[MAX_PATH * 2], side[MAX_PATH * 2 + 8];
    unsigned char hdr[PAGE_HDR], ent[16];
    unsigned char *page = NULL, *outbuf = NULL;
    uint32_t id = 0xFFFFFFFFu;
    unsigned long long off = 0, size = 0;
    int n, member = 0;
    size_t i;
    HANDLE h = INVALID_HANDLE_VALUE;
    vt_decode_job job;
    HANDLE th;
    if (!g_decode) { vt_line("decode: Mega2PageDecode is not a clean signature match on this image; refused"); return; }
    if (!_strnicmp(family, "virtualtextures/", 16)) family += 16;
    strncpy_s(fam, sizeof fam, family, _TRUNCATE);
    for (i = 0; fam[i]; ++i) if (fam[i] == '\\') fam[i] = '/';
    if (!game_dir(dir, sizeof dir)) { vt_line("decode: the game folder cannot be resolved"); return; }
    for (n = 1; n <= 16 && id == 0xFFFFFFFFu; ++n) {
        sh_modserve_hit hit;
        long cell;
        _snprintf_s(request, sizeof request, _TRUNCATE, "virtualtextures/%s_sq%d.mega2", fam, n);
        if (sh_modserve_lookup(request, &hit)) strncpy_s(disk, sizeof disk, hit.disk_path, _TRUNCATE);
        else {
            _snprintf_s(disk, sizeof disk, _TRUNCATE, "%s%s", dir, request);
            for (i = 0; disk[i]; ++i) if (disk[i] == '/') disk[i] = '\\';
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = CreateFileA(disk, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (!read_at(h, 0, hdr, PAGE_HDR)) continue;
        cell = sh_vt_member_cell(hdr, level, x, y);
        if (cell < 0 || (uint32_t)cell >= rd32(hdr + 0x4C)) continue;
        if (!read_at(h, rd64(hdr + 0x40) + 4ull * (unsigned long long)cell, &id, 4)) { id = 0xFFFFFFFFu; continue; }
        if (id == 0xFFFFFFFFu) { vt_line("decode: %s cell %ld is empty", request, cell); continue; }
        if (id >= rd32(hdr + 0x48) || !read_at(h, rd64(hdr + 0x38) + 16ull * id, ent, 16)) {
            vt_line("decode: %s cell %ld names page %u outside the page table", request, cell, id);
            id = 0xFFFFFFFFu;
            continue;
        }
        off = rd64(ent);
        size = rd64(ent + 8);
        member = n;
        vt_line("decode: %s (%s) level %d cell (%u,%u) = index %ld -> page %u at 0x%llx, %llu bytes",
                request, disk, level, x, y, cell, id, off, size);
    }
    if (id == 0xFFFFFFFFu) {
        vt_line("decode: no sq member of '%s' holds level %d cell (%u,%u)", fam, level, x, y);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return;
    }
    if (size < 16 || size > PAGE_MAX) { vt_line("decode: page size %llu outside 16..0x%X; refused", size, PAGE_MAX); CloseHandle(h); return; }
    page = (unsigned char *)calloc(1, PAGE_MAX + PAGE_SLACK);
    outbuf = (unsigned char *)calloc(1, PAGE_OUT);
    if (!page || !outbuf || !read_at(h, off, page, (DWORD)size)) {
        vt_line("decode: %s", page && outbuf ? "the page could not be read" : "out of memory");
        free(page); free(outbuf); CloseHandle(h);
        return;
    }
    CloseHandle(h);
    if (!out_path(out_arg, out, sizeof out)) { vt_line("decode: the output folder cannot be resolved"); free(page); free(outbuf); return; }
    _snprintf_s(side, sizeof side, _TRUNCATE, "%s.page", out);
    if (!write_file(side, page, (size_t)size)) vt_line("decode: could not write %s", side);
    {
        unsigned b0 = page[0], b1 = page[1], b2 = page[2], b3 = page[3], e = page[14], fbyte = page[15];
        unsigned l0 = ((unsigned)page[4] << 8) | page[5], l1 = ((unsigned)page[6] << 8) | page[7];
        unsigned l2 = ((unsigned)page[8] << 8) | page[9], l3 = ((unsigned)page[10] << 8) | page[11];
        unsigned lc = ((unsigned)page[12] << 8) | page[13];
        unsigned cover = ((b3 & 8) && !(e & 0x20)) ? 0x800u : 0u;
        vt_line("decode: header q %u %u %u flags 0x%02X lengths (BE) p0 %u p1 %u p2 %u p3 %u extra %u absent 0x%02X "
                "cover byte %u; 16+streams+cover = %u (stored %llu)%s", b0, b1, b2, b3, l0, l1, l2, l3, lc, e, fbyte,
                16 + l0 + l1 + l2 + l3 + lc + cover, size,
                (16ull + l0 + l1 + l2 + l3 + lc + cover) == size ? "" : " MISMATCH");
    }
    sh_vt_swap_page_header(page);
    memset(&job, 0, sizeof job);
    job.page = page;
    job.out = outbuf;
    th = CreateThread(NULL, DECODE_STACK, decode_thread, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) { vt_line("decode: the decode thread could not start"); free(page); free(outbuf); return; }
    if (WaitForSingleObject(th, 15000) != WAIT_OBJECT_0) {
        /* The thread still owns the buffers: leak them rather than free under it. */
        vt_line("decode: the decoder did not return within 15 s");
        CloseHandle(th);
        return;
    }
    CloseHandle(th);
    if (job.faulted) vt_line("decode: Mega2PageDecode FAULTED (exception 0x%08lX)", (unsigned long)job.code);
    else if (!write_file(out, outbuf, PAGE_OUT)) vt_line("decode: could not write %s", out);
    else {
        int p;
        for (p = 0; p < 5; ++p) {
            const unsigned char *q = outbuf + (size_t)p * 0x10000;
            size_t nz = 0, k;
            for (k = 0; k < 0x10000; ++k) nz += q[k] != 0;
            vt_line("decode: plane %d nonzero bytes %u of 65536; texel (64,64) = %02X %02X %02X %02X", p, (unsigned)nz,
                    q[(64 * 128 + 64) * 4], q[(64 * 128 + 64) * 4 + 1], q[(64 * 128 + 64) * 4 + 2],
                    q[(64 * 128 + 64) * 4 + 3]);
        }
        vt_line("decode: member sq%d; wrote %s (0x50000 bytes, five 0x10000 planes) and %s", member, out, side);
    }
    free(page);
    free(outbuf);
}

/* ------------------------------------------------------------------ install */

static const sig_result *find_clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name)) return r[i].status == SIG_OK ? &r[i] : NULL;
    return NULL;
}

int sh_vt_harness_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    const sig_result *ss = find_clean(results, count, "VtSetSource");
    const sig_result *vf = find_clean(results, count, "VtForMaterial");
    const sig_result *dc = find_clean(results, count, "Mega2PageDecode");
    const sh_build_identity *id = sh_host_identity();
    uint8_t code[0xC0];
    vt_line("harness compiled in (lane RC research build, DO NOT DISTRIBUTE)");
    g_vulkan = id ? id->vulkan : -1;
    g_decode = dc ? (decode_fn)dc->addr : NULL;
    vt_line("Mega2PageDecode %s", dc ? "bound" : "unavailable (signature missing or hooked): decode refused");
    if (dc) vt_line("  Mega2PageDecode rva 0x%X", dc->rva);
    g_main_thread_at = (const uint8_t *)glb_resolve(module_base, "main_thread_id", NULL);
    g_heap_ok = sh_process_heap_bind(&g_heap, results, count, module_base);
    if (!ss || !vf) {
        vt_line("record registration unavailable: %s is not a clean signature match", !ss ? "VtSetSource" : "VtForMaterial");
        return 0;
    }
    if (!g_heap_ok || !g_main_thread_at) {
        vt_line("record registration unavailable: %s", !g_heap_ok ? "the process heap scope is unbound" : "main_thread_id is unresolved");
        return 0;
    }
    /* SetSource head: LEA RCX,[sys]; CALL enter; MOV RCX,RSI; CALL Release. */
    if (rd_code((const uint8_t *)ss->addr, code, sizeof code)) {
        int at = sh_vt_find_setsource_head(code, sizeof code);
        if (at >= 0x30 && at <= 0x80) {
            int32_t d_lea, d_rel;
            const uint8_t *base = (const uint8_t *)ss->addr + at;
            uint8_t rel[0x50];
            memcpy(&d_lea, code + at + 3, 4);
            memcpy(&d_rel, code + at + 16, 4);
            g_sys = (uint8_t *)(base + 7 + d_lea);
            g_release = (release_fn)(base + 20 + d_rel);
            /* Release: MOV RAX,RSP; PUSH RBP, and CMP DWORD [RCX+0x2C0],0 in its head. */
            if (!rd_code((const uint8_t *)g_release, rel, sizeof rel) || memcmp(rel, "\x48\x8B\xC4\x55", 4))
                g_release = NULL;
            else {
                size_t i;
                int seen = 0;
                for (i = 0; i + 7 <= sizeof rel; ++i)
                    if (!memcmp(rel + i, "\x83\xB9\xC0\x02\x00\x00\x00", 7)) seen = 1;
                if (!seen) g_release = NULL;
            }
        }
    }
    if (rd_code((const uint8_t *)vf->addr, code, sizeof code)) {
        uint32_t d = sh_vt_find_table_disp(code, sizeof code);
        g_table = (d >= 8 && d < 0x20000) ? d - 8 : 0;
        if (!g_vulkan) {
            size_t i, j;
            for (i = 0x30; i + 8 <= 0x90 && !g_sparse_ext; ++i)
                if (code[i] == 0x48 && code[i + 1] == 0x83 && code[i + 2] == 0x3D && code[i + 7] == 0x00)
                    for (j = i + 8; j + 7 <= i + 0x30 && j + 7 <= sizeof code; ++j)
                        if (code[j] == 0x83 && code[j + 1] == 0x3D && code[j + 6] == 0x00) {
                            size_t m;
                            int four = 0;
                            for (m = j + 7; m + 5 <= j + 0x18 && m + 5 <= sizeof code; ++m)
                                if (!memcmp(code + m, "\xB8\x04\x00\x00\x00", 5)) four = 1;
                            if (four) {
                                int32_t d1, d2;
                                memcpy(&d1, code + i + 3, 4);
                                memcpy(&d2, code + j + 2, 4);
                                g_sparse_ext = (const uint64_t *)((const uint8_t *)vf->addr + i + 8 + d1);
                                g_sparse_cvar = (const int32_t *)((const uint8_t *)vf->addr + j + 7 + d2);
                            }
                            break;
                        }
        }
    }
    if (!g_sys || !g_release || !g_table) {
        vt_line("record registration unavailable: %s could not be decoded from the signed code",
                !g_sys ? "the VT system object" : !g_release ? "Release" : "the record table");
        return 0;
    }
    g_setsource = (setsource_fn)ss->addr;
    g_bound = 1;
    vt_line("bound on %s: SetSource rva 0x%X, Release rva 0x%X, VirtualTextureForMaterial rva 0x%X, VT system rva 0x%X, "
            "record table sys+0x%X (stride by table 0x%X)%s", g_vulkan == 1 ? "Vulkan" : g_vulkan == 0 ? "OpenGL" : "an unknown image",
            ss->rva, (unsigned)((const uint8_t *)g_release - module_base), vf->rva, (unsigned)(g_sys - module_base), g_table,
            g_table / TABLE_SLOTS, g_vulkan ? "" : (g_sparse_ext ? "; sparse test decoded" : "; sparse test NOT decoded (flavor auto = 2)"));
    return 1;
}

/* ------------------------------------------------------------------ console */

static void do_list(void)
{
    int k, ref = 0;
    if (!g_bound) { vt_line("list: not bound (sh_vt status)"); return; }
    vt_line("records (VT system %p, table sys+0x%X):", g_sys, g_table);
    for (k = 1; k < TABLE_SLOTS; ++k) {
        uint8_t *r = rec_at(k);
        if (!r) { vt_line("  [%2d] (null slot)", k); continue; }
        if ((size_t)(r - g_sys) != (size_t)k * (g_table / TABLE_SLOTS))
            vt_line("  [%2d] note: record at sys+0x%llX, not k*stride", k, (unsigned long long)(r - g_sys));
        rec_print(k, r);
    }
    rd_i32(g_sys + g_table + REFERENCED_OFF, &ref);
    vt_line("referenced virtual textures (VirtualTextureForMaterial's list, listVirtualTextures): %d", ref);
    for (k = 0; k < ref && k < 100; ++k) {
        char p[257];
        __try { strncpy_s(p, sizeof p, (const char *)(g_sys + g_table + REFERENCED_OFF + 4 + 0x100 * k), _TRUNCATE); }
        __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(p, sizeof p, "(unreadable)"); }
        vt_line("  ref %d '%s'", k, p);
    }
}

static int parse_uint(const char *s, unsigned *v)
{
    char *end;
    unsigned long u;
    if (!s || !*s) return 0;
    u = strtoul(s, &end, 0);
    if (*end) return 0;
    *v = (unsigned)u;
    return 1;
}

void h_sh_vt(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
    if (!verb || !*verb || !_stricmp(verb, "help") || !strcmp(verb, "?")) { sh_printf("%s\n", SH_VT_CMD_HELP); return; }
    if (!_stricmp(verb, "status")) {
        vt_line("status: registration %s, decode %s, renderer %s, VT system %p, table +0x%X, harness records 0x%08X, "
                "flavor auto %d", g_bound ? "bound" : "unavailable", g_decode ? "bound" : "unavailable",
                g_vulkan == 1 ? "Vulkan" : g_vulkan == 0 ? "OpenGL" : "unknown", g_sys, g_table, g_mine,
                g_bound ? auto_flavor() : -1);
        return;
    }
    if (!_stricmp(verb, "list")) { do_list(); return; }
    if (!_stricmp(verb, "register") || !_stricmp(verb, "release")) {
        vt_call c;
        const char *name = cmd_argv(a, 2), *path = cmd_argv(a, 3), *fl = cmd_argv(a, 4);
        unsigned flavor = 0;
        int reg = !_stricmp(verb, "register");
        if (!g_bound) { vt_line("%s: not bound on this image; refused (sh_vt status)", verb); return; }
        if (!name || !*name || (reg && (!path || !*path))) {
            sh_printf("usage: sh_vt register <name> <pagesPath> [flavor]  |  sh_vt release <name>\n");
            return;
        }
        memset(&c, 0, sizeof c);
        strncpy_s(c.name, sizeof c.name, name, _TRUNCATE);
        c.verb = reg ? 1 : 2;
        if (reg) {
            strncpy_s(c.path, sizeof c.path, path, _TRUNCATE);
            if (strstr(c.name, "_vmtr")) { vt_line("register: a name containing '_vmtr' resolves to the stock family; refused"); return; }
            if (fl && *fl) {
                if (!parse_uint(fl, &flavor) || (flavor != 1 && flavor != 2 && flavor != 4)) {
                    vt_line("register: flavor must be 1, 2 or 4");
                    return;
                }
                c.flavor = (int)flavor;
            } else c.flavor = auto_flavor();
        }
        run_main(&c);
        return;
    }
    if (!_stricmp(verb, "decode")) {
        const char *fam = cmd_argv(a, 2), *out = cmd_argv(a, 6);
        unsigned level = 0, x = 0, y = 0;
        if (!fam || !out || !*out || !parse_uint(cmd_argv(a, 3), &level) || !parse_uint(cmd_argv(a, 4), &x) ||
            !parse_uint(cmd_argv(a, 5), &y) || level > 11) {
            sh_printf("usage: sh_vt decode <family> <level 0-11> <x> <y> <out.bin>  (x, y in that level's page grid)\n");
            return;
        }
        do_decode(fam, (int)level, x, y, out);
        return;
    }
    sh_printf("sh_vt: unknown verb '%s'. %s\n", verb, SH_VT_CMD_HELP);
}

#endif /* SH_VT_HARNESS_PURE */
