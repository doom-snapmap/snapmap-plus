/* engine_globals.c -- see engine_globals.h. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "engine_globals.h"
#include "signatures.h"
#include "backend_log.h"
#include "engine_globals_table.gen.h"   /* defines BACKEND_ENGINE_GLOBALS; generated, never hand-edited */

#define GLB_CACHE_MAX 64

static struct {
    const char *name;
    uintptr_t   addr;
} g_cache[GLB_CACHE_MAX];
static size_t g_cached = 0;

size_t glb_db_count(void)
{
    size_t n = 0;
    while (BACKEND_ENGINE_GLOBALS[n].name) n++;
    return n;
}

static uintptr_t cache_get(const char *name)
{
    size_t i;
    for (i = 0; i < g_cached; i++)
        if (strcmp(g_cache[i].name, name) == 0) return g_cache[i].addr;
    return 0;
}

static void cache_put(const char *name, uintptr_t addr)
{
    if (g_cached >= GLB_CACHE_MAX) return;
    g_cache[g_cached].name = name;
    g_cache[g_cached].addr = addr;
    g_cached++;
}

/* SEH-guarded 4-byte read: the anchor may sit in an uncommitted section tail. */
static int read_i32(const uint8_t *p, int32_t *out)
{
    __try {
        memcpy(out, p, sizeof(*out));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static size_t image_size_of(const uint8_t *module_base)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS *nt;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        nt = (const IMAGE_NT_HEADERS *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return (size_t)nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
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

    /* known_rva is deliberately 0: the hook-tolerant fallback in sig_resolve_one resolves AT the
     * pinned RVA when the scan misses, which is exactly wrong here. We are about to read a
     * displacement out of the matched bytes, and a detour will have overwritten them. Only a clean,
     * unique scan hit is usable, so give the resolver no fallback to take. */
    anchor_sig.name      = e->name;
    anchor_sig.pattern   = e->anchor;
    anchor_sig.known_rva = 0;

    st = sig_resolve_one(module_base, &anchor_sig, &r);
    if (st != SIG_OK) {
        if (out_status)
            *out_status = (st == SIG_AMBIGUOUS) ? GLB_ANCHOR_AMBIGUOUS : GLB_ANCHOR_NOT_FOUND;
        return 0;
    }

    if (!read_i32((const uint8_t *)r.addr + e->disp_slot, &disp)) {
        if (out_status) *out_status = GLB_UNREADABLE;
        return 0;
    }

    /* RIP-relative: the displacement is measured from the END of the disp32 field. */
    decoded = r.addr + e->disp_slot + 4 + (intptr_t)disp + (intptr_t)e->delta;

    img_sz = image_size_of(module_base);
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
                "glb %-24s rva=0x%-9x (pinned 0x%x)", e->name,
                (unsigned)(a - (uintptr_t)module_base), (unsigned)e->pinned_rva);
        } else {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "glb %-24s UNRESOLVED status=%d -- dependent features will decline",
                e->name, (int)st);
        }
        backend_log(line);
    }
    return ok;
}
