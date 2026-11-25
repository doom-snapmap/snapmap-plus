/* Resolves engine globals from RIP-relative references in signed code; see engine_globals.h. */
#include "safe_read.h"
#include "image_view.h"
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "engine_globals.h"
#include "signatures.h"
#include "backend_log.h"
#include "engine_globals_table.gen.h"   /* defines BACKEND_ENGINE_GLOBALS; generated, never hand-edited */

/* Each cache address is published last; readers skip unpublished slots and may resolve them again. Lock-free because
 * the fault shield also reads this cache from exception handlers. */
#define GLB_CACHE_MAX 64

static struct {
    const char *volatile name;
    volatile uintptr_t   addr;
} g_cache[GLB_CACHE_MAX];
static volatile LONG g_reserved = 0;

size_t glb_db_count(void)
{
    size_t n = 0;
    while (BACKEND_ENGINE_GLOBALS[n].name) n++;
    return n;
}

static uintptr_t cache_get(const char *name)
{
    LONG n = g_reserved;
    LONG i;
    if (n > GLB_CACHE_MAX) n = GLB_CACHE_MAX;
    for (i = 0; i < n; i++) {
        uintptr_t   a = g_cache[i].addr;   /* zero means not published yet */
        const char *nm;
        if (!a) continue;
        nm = g_cache[i].name;
        if (nm && strcmp(nm, name) == 0) return a;
    }
    return 0;
}

static void cache_put(const char *name, uintptr_t addr)
{
    LONG i = InterlockedIncrement(&g_reserved) - 1;
    if (i < 0 || i >= GLB_CACHE_MAX) return;   /* table outgrew the cache: resolve every time */
    g_cache[i].name = name;
    MemoryBarrier();
    g_cache[i].addr = addr;                    /* publish last */
}

uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{
    const global_entry *e;
    sig_entry   anchor_sig;
    sig_result  r;
    sig_status  st;
    int32_t     disp = 0;
    uintptr_t   decoded;
    size_t      img_sz;
    uintptr_t   cached;

    if (out_status) *out_status = GLB_UNKNOWN_NAME;
    if (module_base == NULL || name == NULL) return 0;

    cached = cache_get(name);
    if (cached) { if (out_status) *out_status = GLB_OK; return cached; }

    for (e = BACKEND_ENGINE_GLOBALS; e->name; e++)
        if (strcmp(e->name, name) == 0) break;
    if (!e->name) return 0;

    /* The anchor is checked at its recorded code site only; changed bytes there locate nothing. */
    memset(&anchor_sig, 0, sizeof anchor_sig);
    anchor_sig.name      = e->name;
    anchor_sig.pattern   = e->anchor;
    anchor_sig.rva_vk    = e->site_vk;
    anchor_sig.rva_gl    = e->site_gl;

    st = sig_resolve_one(module_base, &anchor_sig, &r);
    if (st != SIG_OK) {
        if (out_status)
            *out_status = GLB_ANCHOR_NOT_FOUND;
        return 0;
    }

    if (!sh_read_i32((const uint8_t *)r.addr + e->disp_slot, &disp)) {
        if (out_status) *out_status = GLB_UNREADABLE;
        return 0;
    }

    /* RIP is measured from the instruction end, including any trailing immediate. */
    decoded = r.addr + e->disp_slot + 4 + e->disp_tail + (intptr_t)disp + (intptr_t)e->delta;

    img_sz = sh_pe_image_size(module_base);
    if (img_sz == 0 || decoded < (uintptr_t)module_base ||
        decoded >= (uintptr_t)module_base + img_sz) {
        if (out_status) *out_status = GLB_OUT_OF_RANGE;
        return 0;
    }

    cache_put(e->name, decoded);
    if (out_status) *out_status = GLB_OK;
    return decoded;
}

size_t glb_resolve_all(const uint8_t *module_base)
{
    const global_entry *e;
    size_t ok = 0;
    char line[200];

    for (e = BACKEND_ENGINE_GLOBALS; e->name; e++) {
        glb_status st = GLB_UNKNOWN_NAME;
        uintptr_t  a  = glb_resolve(module_base, e->name, &st);
        if (a) {
            ok++;
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "glb %-24s rva=0x%-9x (recorded vk 0x%x / gl 0x%x)", e->name,
                (unsigned)(a - (uintptr_t)module_base), (unsigned)e->rva_vk, (unsigned)e->rva_gl);
        } else {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "glb %-24s UNRESOLVED status=%d -- dependent features will decline",
                e->name, (int)st);
        }
        backend_log(line);
    }
    return ok;
}
