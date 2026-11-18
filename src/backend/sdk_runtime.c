/* sdk_runtime.c -- the Graphics SDK's files as the server holds them, for the script calls that play them
 * (script_api_common.h sh_script_sdk_runtime): an effect's emitter, an environment's.
 *
 * A call opens the mod's fx or env decl through the server (sh_modserve_open), which compiles it the way the engine
 * will read it (sdk.h) and so registers everything it derives before the spawn asks for it; the compiled text's first
 * line says whether it is an SDK file (modeffect.h SH_EFFECT_MARK, modenv.h SH_ENV_MARK). */
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

void sh_sdk_runtime_install(void)
{
    sh_script_sdk_runtime rt;
    rt.effect_emitter = effect_emitter;
    rt.environment_emitter = environment_emitter;
    rt.log = backend_log;
    sh_script_set_sdk_runtime(&rt);
}
