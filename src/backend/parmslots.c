/* parmslots.c -- the detour of idParmState::AllocParmState that gives every render parm a slot (parmslots.h).
 * `state` begins with 0x93F holder pointers (decl name +0x08, kind +0x6C); a sharing parm is not recorded there,
 * so each later call for it is decided again, to the same slot. */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "parmslots.h"
#include "modprog.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"

#define ALLOC_STOLEN  15      /* mov [rsp+8],rbx; mov [rsp+0x10],rbp; mov [rsp+0x18],rsi */
#define DECL_NAME     0x08
#define DECL_KIND     0x6C
#define LOGGED_MAX    32

typedef int (*alloc_fn)(void *state, void *decl);

static alloc_fn g_alloc;
static volatile LONG g_spill = -1;
static volatile LONG g_shared, g_logged;
static const char *g_reserved[96];
static unsigned g_nreserved;

static void ps_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

/* A slot's holder, read from the engine's table. An unreadable holder counts as held and unnamed. */
static int ps_read(void *context, int slot, const char **name, int *kind)
{
    void *holder = NULL;
    *name = NULL;
    *kind = -1;
    __try {
        holder = ((void **)context)[slot];
        if (!holder) return 0;
        *name = *(const char **)((const uint8_t *)holder + DECL_NAME);
        *kind = *(const int *)((const uint8_t *)holder + DECL_KIND);
    } __except (EXCEPTION_EXECUTE_HANDLER) { *name = NULL; *kind = -1; return 1; }
    return 1;
}

static const char *range_name(int kind)
{
    if (kind == 0) return "vector";
    if (kind >= 1 && kind <= 9) return "texture";
    if (kind == 10) return "sampler";
    if (kind == 11) return "program";
    if (kind == 12 || kind == 14) return "string";
    if (kind == 13) return "buffer";
    if (kind == 15) return "uniform buffer";
    return "image buffer";
}

static int alloc_detour(void *state, void *decl)
{
    const char *name = NULL;
    int kind = -1, becomes = 0, slot;
    if (!state || !decl) return g_alloc(state, decl);
    __try {
        name = *(const char **)((const uint8_t *)decl + DECL_NAME);
        kind = *(const int *)((const uint8_t *)decl + DECL_KIND);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return g_alloc(state, decl); }
    slot = sh_parmslots_decide(kind, name, ps_read, state, g_reserved, g_nreserved,
                               (int)InterlockedCompareExchange(&g_spill, -1, -1), &becomes);
    if (slot == SH_PARMSLOTS_ENGINE) {
        int given = g_alloc(state, decl);
        if (becomes && given >= 0 && given < 0x400) {
            InterlockedExchange(&g_spill, given);
            ps_log("PARMS: the vector range is at its last free slot; 0x%X (first held by '%s') is shared by every "
                   "later parm that is not one of Snapmap+'s own", given, name ? name : "?");
        }
        return given;
    }
    InterlockedIncrement(&g_shared);
    if (InterlockedIncrement(&g_logged) <= LOGGED_MAX)
        ps_log("PARMS: render parm '%s' shares slot 0x%X: the %s range is full", name ? name : "?", slot,
               range_name(kind));
    return slot;
}

int sh_parmslots_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    void *target, *tramp;
    unsigned i;
    (void)module_base;
    if (g_alloc) return 1;
    target = sig_pointer(results, count, "ParmStateAlloc");
    if (!target) {
        ps_log("PARMS: AllocParmState did not resolve on this executable; a full parm range stays the engine's error");
        return 0;
    }
    g_nreserved = 0;
    for (i = 0; g_nreserved < sizeof g_reserved / sizeof g_reserved[0]; i++) {
        const char *name = sh_modprog_vector_parm(i);
        if (!name) break;
        g_reserved[g_nreserved++] = name;
    }
    tramp = hook_prepare(target, (void *)alloc_detour, ALLOC_STOLEN);
    if (!tramp) {
        ps_log("PARMS: AllocParmState could not be detoured (%s); a full parm range stays the engine's error",
               hook_last_failure());
        return 0;
    }
    g_alloc = (alloc_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_alloc = NULL;
        ps_log("PARMS: AllocParmState's detour could not be committed; a full parm range stays the engine's error");
        return 0;
    }
    ps_log("PARMS: render parm slots guarded: %u vector parms of Snapmap+'s programs keep their slots, a parm past a "
           "full range shares one", g_nreserved);
    return 1;
}
