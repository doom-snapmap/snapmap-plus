/* vt_harness.c -- the sh_vt diagnostic (see vt_harness.h).
 *
 * The engine surface is the one modvt.c binds for the product's own family
 * registration (VtSetSource, VtForMaterial and what is decoded from them);
 * the decoder is the existing Mega2PageDecode signature. Registration and
 * release run on DOOM's main thread inside process heap 0, under SEH, through
 * modvt.c. Decoding is pure CPU work on a private thread with an 8 MB stack,
 * as megapreview.c runs the same decoder.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "vt_harness.h"
#include "modvt.h"
#include "signatures.h"
#include "commands.h"
#include "backend_log.h"
#include "apply_engine.h"
#include "modserve.h"

#define OWNER_COUNT     0x140
#define REFERENCED_OFF  0x118   /* from the table: count, then 100 x 256-byte paths at +0x11C */

#define PAGE_HDR        0x170u
#define PAGE_OUT        0x50000u
#define PAGE_MAX        0x40000u
#define PAGE_SLACK      0x40000u
#define DECODE_STACK    (8u << 20)

typedef void (*decode_fn)(const unsigned char *hdr, const unsigned char *payload, void *unused,
                          unsigned char *out);

static decode_fn g_decode;
static uint32_t g_mine;            /* bit k: record k registered with sh_vt register */

/* ------------------------------------------------------------------ pure helpers */

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
    if (!rd_ptr(slot, (void **)&s) || !s) return;
    __try { strncpy_s(out, cap, s, _TRUNCATE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, cap, "(unreadable)"); }
}

/* ------------------------------------------------------------------ records */

static int rec_free(uint8_t *r)
{
    char path[8], name[8];
    rd_cstr(r + SH_VT_REC_PATH, path, sizeof path);
    rd_cstr(r + SH_VT_REC_NAME, name, sizeof name);
    return !path[0] && !name[0];
}

static int rec_find(const char *name)
{
    int k;
    char n[260];
    for (k = 1; k < SH_MODVT_SLOTS; ++k) {
        uint8_t *r = sh_modvt_engine_record(k);
        if (!r) continue;
        rd_cstr(r + SH_VT_REC_NAME, n, sizeof n);
        if (n[0] && !strcmp(n, name)) return k;
    }
    return -1;
}

static void rec_print(int k, uint8_t *r)
{
    char name[260], path[300];
    int type = 0, grid = 0, texels = 0, flavor = -1, rows = -1, refd = 0, magic = 0;
    short levels = 0;
    const char *tag = sh_modvt_record_owner(k) ? "(product) " : (g_mine >> k) & 1 ? "(sh_vt) " : "";
    void *owner = NULL, *pages = NULL, *reader = NULL, *mega2 = NULL, *pt = NULL, *ptimg = NULL, *host = NULL;
    if (!r) { vt_line("  [%2d] (null slot)", k); return; }
    rd_cstr(r + SH_VT_REC_NAME, name, sizeof name);
    rd_cstr(r + SH_VT_REC_PATH, path, sizeof path);
    rd_i32(r + SH_VT_REC_TYPE, &type);
    rd_i32(r + SH_VT_REC_MAGIC, &magic);
    rd_i32(r + SH_VT_REC_GRID, &grid);
    rd_i16(r + SH_VT_REC_LEVELS, &levels);
    rd_i32(r + SH_VT_REC_TEXELS, &texels);
    rd_i32(r + SH_VT_REC_STREAMED, &refd);
    rd_ptr(r + SH_VT_REC_OWNER, &owner);
    rd_ptr(r + SH_VT_REC_PAGESFILE, &pages);
    rd_ptr(r + SH_VT_REC_READER, &reader);
    rd_ptr(r + SH_VT_REC_MEGA2, &mega2);
    rd_ptr(r + SH_VT_REC_PAGETABLE, &pt);
    rd_ptr(r + SH_VT_REC_PTIMAGE, &ptimg);
    rd_ptr(r + SH_VT_REC_HOSTPT, &host);
    if (pt) rd_i32(pt, &flavor);
    if (owner) rd_i32((uint8_t *)owner + OWNER_COUNT, &rows);
    if (!name[0] && !path[0] && !type) { vt_line("  [%2d] free", k); return; }
    vt_line("  [%2d] %s'%s' type %d flavor %d path '%s'", k, tag, name, type, flavor, path);
    vt_line("       reader %s (mega2 %p, pages file %p, reader %p); header magic 0x%08X grid %d levels %d "
            "texels %d; page table %p image %p host %p; table rows %d; streamed %d",
            mega2 ? "mega2" : pages ? ".pages" : reader ? "other" : "none", mega2, pages, reader,
            (unsigned)magic, grid, (int)levels, texels, pt, ptimg, host, rows, refd);
}

/* Host page-table bytes SetSource allocates for `levels`. */
static unsigned long long host_bytes(int levels)
{
    unsigned long long sum = 0;
    int i;
    for (i = levels - 1; i >= 0; --i) sum += 2ull << (2 * i);
    return sum;
}

/* ------------------------------------------------------------------ main thread */

typedef struct vt_call {
    char name[256];
    char path[300];
    int flavor;
    int verb;      /* 1 register, 2 release, 3 sync */
} vt_call;

static int do_register(vt_call *c)
{
    int k, slot = -1, called, result = 0;
    unsigned fault = 0;
    double ms = 0;
    uint8_t *r;
    if (rec_find(c->name) >= 0) { vt_line("register: a record named '%s' exists already (sh_vt list)", c->name); return 0; }
    /* Record 31 is released by VirtualTextureForMaterial on every miss: never use it. */
    for (k = 1; k <= SH_MODVT_LAST_USED && slot < 0; ++k) {
        r = sh_modvt_engine_record(k);
        if (r && rec_free(r)) slot = k;
    }
    if (slot < 0) { vt_line("register: no free record among 1..%d", SH_MODVT_LAST_USED); return 0; }
    r = sh_modvt_engine_record(slot);
    called = sh_modvt_engine_set_source(slot, c->name, c->path, c->flavor, &result, &ms, &fault);
    if (called < 0) { vt_line("register: SetSource FAULTED (exception 0x%08X) on record %d", fault, slot); return 0; }
    if (!called) { vt_line("register: process heap 0 could not be entered (or not the main thread); nothing done"); return 0; }
    vt_line("register: SetSource(record %d, '%s', '%s', flavor %d, type 2, direct 0, NULL) -> %d in %.1f ms",
            slot, c->name, c->path, c->flavor, result, ms);
    if (result) {
        short levels = 0;
        g_mine |= 1u << slot;
        rd_i16(r + SH_VT_REC_LEVELS, &levels);
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
    int k = rec_find(c->name), done;
    unsigned fault = 0;
    if (k < 0) { vt_line("release: no record named '%s'", c->name); return 0; }
    if (sh_modvt_record_owner(k)) {
        vt_line("release: record %d '%s' belongs to an installed mod; it is released when that mod is removed", k, c->name);
        return 0;
    }
    if (!((g_mine >> k) & 1)) { vt_line("release: record %d '%s' was not made by 'sh_vt register'; refused", k, c->name); return 0; }
    done = sh_modvt_engine_release(k, &fault);
    if (done < 0) { vt_line("release: FAULTED (exception 0x%08X) on record %d", fault, k); return 0; }
    if (!done) { vt_line("release: process heap 0 could not be entered"); return 0; }
    g_mine &= ~(1u << k);
    vt_line("release: record %d '%s' released (materials that bound it still point at it)", k, c->name);
    rec_print(k, sh_modvt_engine_record(k));
    return 1;
}

static int main_call(void *ctx)
{
    vt_call *c = (vt_call *)ctx;
    if (c->verb == 3) { sh_modvt_console_sync(); return 1; }
    return c->verb == 1 ? do_register(c) : do_release(c);
}

static void run_main(vt_call *c)
{
    sh_run_on_main_fn run = NULL;
    int result = 0;
    if (sh_modvt_engine_on_main() == 1) { (void)main_call(c); return; }
    sh_apply_engine_get_run_on_main(&run);
    if (!run || run(NULL, main_call, c, 5000, &result) != 1)
        vt_line("%s: did not start on the main thread (no frame within 5 s)",
                c->verb == 1 ? "register" : c->verb == 2 ? "release" : "sync");
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

int sh_vt_harness_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    size_t i;
    (void)module_base;
    for (i = 0; i < count; ++i)
        if (results[i].name && !strcmp(results[i].name, "Mega2PageDecode") && results[i].status == SIG_OK) {
            g_decode = (decode_fn)results[i].addr;
            break;
        }
    if (!g_decode) vt_line("sh_vt decode unavailable (Mega2PageDecode is missing or hooked)");
    return g_decode != NULL;
}

/* ------------------------------------------------------------------ console */

static void do_list(void)
{
    int k, ref = 0;
    uint8_t *sys = sh_modvt_engine_sys();
    uint32_t table = sh_modvt_engine_table();
    if (!sh_modvt_engine_bound()) { vt_line("list: not bound (sh_vt status)"); return; }
    vt_line("records (VT system %p, table sys+0x%X; (product) marks an installed mod's family):", sys, table);
    for (k = 1; k < SH_MODVT_SLOTS; ++k) rec_print(k, sh_modvt_engine_record(k));
    rd_i32(sys + table + REFERENCED_OFF, &ref);
    vt_line("referenced virtual textures (VirtualTextureForMaterial's list, listVirtualTextures): %d", ref);
    for (k = 0; k < ref && k < 100; ++k) {
        char p[257];
        __try { strncpy_s(p, sizeof p, (const char *)(sys + table + REFERENCED_OFF + 4 + 0x100 * k), _TRUNCATE); }
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
        int bound = sh_modvt_engine_bound(), vk = sh_modvt_engine_vulkan();
        vt_line("status: records %s, decode %s, renderer %s, VT system %p (%s), table +0x%X, sh_vt records 0x%08X, "
                "flavor auto %d", bound ? "bound" : "unavailable", g_decode ? "bound" : "unavailable",
                vk == 1 ? "Vulkan" : vk == 0 ? "OpenGL" : "unknown", sh_modvt_engine_sys(),
                bound && sh_modvt_engine_ready() ? "up" : "not up", sh_modvt_engine_table(), g_mine,
                bound ? sh_modvt_engine_flavor() : -1);
        sh_modvt_print_status();
        return;
    }
    if (!_stricmp(verb, "list")) { do_list(); return; }
    if (!_stricmp(verb, "sync")) {
        vt_call c;
        memset(&c, 0, sizeof c);
        c.verb = 3;
        run_main(&c);
        return;
    }
    if (!_stricmp(verb, "register") || !_stricmp(verb, "release")) {
        vt_call c;
        const char *name = cmd_argv(a, 2), *path = cmd_argv(a, 3), *fl = cmd_argv(a, 4);
        unsigned flavor = 0;
        int reg = !_stricmp(verb, "register");
        if (!sh_modvt_engine_bound()) { vt_line("%s: not bound on this image; refused (sh_vt status)", verb); return; }
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
            } else c.flavor = sh_modvt_engine_flavor();
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
