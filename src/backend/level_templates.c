/* level_templates.c -- see level_templates.h. The engine side: the detour of
 * the resource list copy and its filter for the template walk's two calls. */
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "level_templates.h"
#include "modserve.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"
#include "../common/text.h"

#pragma intrinsic(_ReturnAddress)

/* idList<idResource *>: the array, then the count (left as it is: a cleared
 * entry is skipped by both walk loops). A resource's name is the
 * C string at +0x08 (the walk's own "Adding entityDef %s" line reads it). */
#define LIST_COUNT_OFF   0x08
#define RESOURCE_NAME_OFF 0x08

typedef void (*copy_fn)(void *manager, void *list, char sort);

static copy_fn g_copy;           /* the trampoline */
static void *g_tramp;
static const void *g_walk_ret, *g_rewalk_ret;   /* return addresses of the walk's two calls */

static void lt_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (line) backend_log(line);
    free(line);
}

static const char *lt_name(const void *item)
{
    const char *name = NULL;
    __try {
        name = *(const char *const *)((const uint8_t *)item + RESOURCE_NAME_OFF);
        if (name && !name[0]) name = NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { name = NULL; }
    return name;
}

static void lt_filter(void *list, int again)
{
    void **items = NULL;
    int count = 0, hidden;
    const char *first = NULL;
    __try {
        items = *(void ***)list;
        count = *(int *)((uint8_t *)list + LIST_COUNT_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!items || count <= 0) return;
    hidden = sh_level_templates_filter(items, count, lt_name, sh_modserve_level_hides, &first);
    if (hidden && !again)
        lt_log("MODS: scope: the level load builds no entity template for %d entityDef(s) of mods this map does not "
               "declare (first: %s); %d of %d kept", hidden, first ? first : "?", count - hidden, count);
}

static void copy_detour(void *manager, void *list, char sort)
{
    const void *ret = _ReturnAddress();
    g_copy(manager, list, sort);
    if (ret == g_walk_ret) lt_filter(list, 0);
    else if (ret == g_rewalk_ret) lt_filter(list, 1);
}

int sh_level_templates_install(const uint8_t *module_base)
{
    sig_result walk, rewalk;
    uintptr_t copy, again;
    if (g_tramp) return 1;
    if (sig_resolve_one(module_base, &SH_LEVEL_TEMPLATES_SIGNATURES[0], &walk) != SIG_OK || !walk.addr ||
        sig_resolve_one(module_base, &SH_LEVEL_TEMPLATES_SIGNATURES[1], &rewalk) != SIG_OK || !rewalk.addr) {
        lt_log("MODS: scope: level templates: unavailable: the template walk was not found in this executable (%s, "
               "%s); a level load builds templates for every installed mod's entityDefs",
               sig_status_reason(walk.status), sig_status_reason(rewalk.status));
        return 0;
    }
    copy = sh_level_templates_call_target((const uint8_t *)walk.addr + SH_LT_WALK_CALL);
    again = sh_level_templates_call_target((const uint8_t *)rewalk.addr + SH_LT_REWALK_CALL);
    if (!copy || copy != again) {
        lt_log("MODS: scope: level templates: unavailable: the walk's two list copies do not call one function");
        return 0;
    }
    g_walk_ret = (const void *)(walk.addr + SH_LT_WALK_CALL + 5);
    g_rewalk_ret = (const void *)(rewalk.addr + SH_LT_REWALK_CALL + 5);
    g_tramp = hook_prepare((void *)copy, (void *)copy_detour, SH_LT_STOLEN);
    if (!g_tramp) {
        lt_log("MODS: scope: level templates: unavailable: the list copy could not be detoured (%s)",
               hook_last_failure());
        return 0;
    }
    g_copy = (copy_fn)g_tramp;
    if (hook_commit(g_tramp) != B2_PATCH_OK) {
        lt_log("MODS: scope: level templates: unavailable: the list copy's detour was refused");
        (void)hook_unpatch(g_tramp);
        g_tramp = NULL;
        g_copy = NULL;
        return 0;
    }
    lt_log("MODS: scope: level templates: a level load builds entity templates only for the mods its map declares "
           "(list copy detoured; walk calls at %p and %p)", (void *)(walk.addr + SH_LT_WALK_CALL),
           (void *)(rewalk.addr + SH_LT_REWALK_CALL));
    return 1;
}
