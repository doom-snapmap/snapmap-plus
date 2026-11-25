/* sdk_runtime.c -- the Graphics SDK's files as the server holds them, for script calls that play an effect's or
 * environment's emitter (script_api_common.h sh_script_sdk_runtime). A call opens the fx or env decl through
 * sh_modserve_open, which compiles it (sdk.h); the compiled text's first line marks an SDK file. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modeffect.h"
#include "modenv.h"
#include "modprog.h"
#include "modserve.h"
#include "sdk_runtime.h"
#include "engine_build.h"
#include "entity.h"
#include "backend_log.h"
#include "script/script_api_common.h"
#include "../common/text.h"

/* The text the server would serve for a decl (a copy on the process heap), or NULL: its compiled text, asked without
 * serving it (sh_modserve_serve_identity), so the ledger sees only what the engine reads. */
static unsigned char *served(const char *type, const char *name, size_t *len)
{
    char *req = sh_textf("generated/decls/%s/%s.decl", type, name);
    const char *bundle = NULL, *file = NULL;
    const unsigned char *body = NULL;
    size_t n = 0;
    uint64_t size = 0;
    unsigned char *copy = NULL;
    if (!req) return NULL;
    if (sh_modserve_serve_identity(req, &bundle, &file, &body, &n, &size) && body && n &&
        (copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, n)) != NULL)
        memcpy(copy, body, n);
    free(req);
    if (len) *len = copy ? n : 0;
    return copy;
}

static char *effect_emitter(const char *fx_name)
{
    size_t n = 0, mn = strlen(SH_EFFECT_MARK);
    unsigned char *t = served("fx", fx_name, &n);
    char *em = NULL;
    if (!t) return NULL;
    if (n >= mn && !memcmp(t, SH_EFFECT_MARK, mn)) em = sh_modprog_effect_emitter(fx_name);
    HeapFree(GetProcessHeap(), 0, t);
    return em;
}

static char *environment_emitter(const char *env_name)
{
    size_t n = 0, mn = strlen(SH_ENV_MARK), i;
    unsigned char *t = served("env", env_name, &n);
    char fx[160], *em = NULL;
    if (!t) return NULL;
    if (n > mn + 1 && !memcmp(t, SH_ENV_MARK, mn) && t[mn] == ' ') {
        for (i = 0; mn + 1 + i < n && i < sizeof fx - 1 && t[mn + 1 + i] != '\n' && t[mn + 1 + i] != '\r'; i++)
            fx[i] = (char)t[mn + 1 + i];
        fx[i] = 0;
        if (i) em = sh_modprog_effect_emitter(fx);
    }
    HeapFree(GetProcessHeap(), 0, t);
    return em;
}

typedef struct {
    void *env;
    float bounds[6], planes[16][4];
    int plane_count;
    float volume;
    float distance;
    float origin[3];
} sdk_world_area;

typedef struct { sdk_world_area *areas; int count; } sdk_area_list;
typedef void (*sdk_add_area)(void *, const float *, void *, const void *, int, float, const float *, float);
typedef void (*sdk_erase_area)(sdk_area_list *, int);
typedef char sdk_check_area_size[sizeof(sdk_world_area) == 0x138 ? 1 : -1];

/* DynamicSnapMap uses the game world here. The editor owns a separate render world. */
static sdk_area_list *area_list(void **world, void **scene, sdk_add_area *add)
{
    const unsigned char *game = sh_entity_gamemgr();
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    uint32_t expected = SH_RVA(0x1907ae0, 0x18fd540);
    if (!game || !expected) return NULL;
    *world = *(void **)(game + 0x285bf0);
    if (!*world) return NULL;
    *add = *(sdk_add_area *)(*(unsigned char **)*world + 0x38);
    if ((uintptr_t)*add != base + expected) return NULL;
    *scene = *(void **)((unsigned char *)*world + SH_RVA(0x852c0, 0x85280));
    if (!*scene) return NULL;
    sdk_area_list *list = (sdk_area_list *)((unsigned char *)*scene + 0xc550);
    return list->count >= 0 && list->count <= 128 && (!list->count || list->areas) ? list : NULL;
}

static int area_index(const sdk_area_list *list, const float bounds[6], const void *env)
{
    for (int i = 0; i < list->count; i++) {
        const sdk_world_area *a = &list->areas[i];
        if (env && a->env != env) continue;
        unsigned j = 0;
        while (j < 6 && a->bounds[j] == bounds[j]) j++;
        if (j == 6) return i;
    }
    return -1;
}

static int add_environment_area(const char *name, sh_script_env_area *area)
{
    const sh_script_engine *e = sh_script_api_engine();
    size_t n;
    unsigned char *text = served("env", name, &n);
    if (!text) return -1;
    HeapFree(GetProcessHeap(), 0, text);
    area->decl = e && e->decl ? e->decl("env", name) : NULL;
    if (!area->decl) {
        backend_log("Environment area: could not resolve the selected environment.");
        return 0;
    }
    __try {
        sdk_add_area add;
        static const float origin[3] = { 0, 0, 0 };
        sdk_area_list *list = area_list(&area->world, &area->scene, &add);
        if (!list) {
            backend_log("Environment area: the active render world's environment list is unavailable.");
            return 0;
        }
        if (list->count == 128 || area_index(list, area->bounds, NULL) >= 0) {
            backend_log("Environment area: the environment list is full or these bounds are occupied.");
            return 0;
        }
        add(area->world, area->bounds, area->decl, NULL, 0, 0, origin, -1.0f);
        if (area_index(list, area->bounds, area->decl) >= 0) return 1;
        backend_log("Environment area: the renderer did not retain the selected environment.");
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        backend_log("Environment area: the renderer could not access the environment list.");
        return 0;
    }
}

static int remove_environment_area(sh_script_env_area *area)
{
    __try {
        void *world, *scene;
        sdk_add_area add;
        sdk_area_list *list = area_list(&world, &scene, &add);
        if (!list || world != area->world || scene != area->scene) return 1;
        int index = area_index(list, area->bounds, area->decl);
        if (index >= 0) {
            /* The stock box remover chooses the first equal volume. Keep other boxes of that volume intact. */
            uint32_t rva = SH_RVA(0x1909f00, 0x18ff960);
            if (!rva) return 0;
            ((sdk_erase_area)((uintptr_t)GetModuleHandleW(NULL) + rva))(list, index);
        }
        return area_index(list, area->bounds, area->decl) < 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}

void sh_sdk_runtime_install(void)
{
    sh_script_sdk_runtime rt;
    rt.effect_emitter = effect_emitter;
    rt.environment_emitter = environment_emitter;
    rt.log = backend_log;
    rt.add_area = add_environment_area;
    rt.remove_area = remove_environment_area;
    sh_script_set_sdk_runtime(&rt);
}
