/* Keep runtime declaration objects and palette storage alive across map unloads. */
#ifndef SH_PROCESS_HEAP_SCOPE_H
#define SH_PROCESS_HEAP_SCOPE_H

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "signatures.h"

typedef struct sh_process_heap_api {
    void *(*get)(void);
    void (*push)(void *, int);
    void (*pop)(void *);
} sh_process_heap_api;

typedef struct sh_process_heap_scope {
    const sh_process_heap_api *api;
    void *self;
    int depth;
    int active; /* 1: push attempted; 2: push verified. */
} sh_process_heap_scope;

static __inline int sh_process_heap_bind(sh_process_heap_api *api,
    const sig_result *results, size_t count, const uint8_t *module_base)
{
    static const char *names[] = { "MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap" };
    uintptr_t addresses[3] = { 0, 0, 0 };
    size_t i, j;
    if (!api) return 0;
    memset(api, 0, sizeof(*api));
    if (!results || !module_base) return 0;
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < count; ++j) {
            if (!results[j].name || strcmp(results[j].name, names[i])) continue;
            if (addresses[i] || results[j].status != SIG_OK || !results[j].addr ||
                results[j].addr != (uintptr_t)module_base + results[j].rva) return 0;
            addresses[i] = results[j].addr;
        }
        if (!addresses[i]) return 0;
    }
    api->get = (void *(*)(void))addresses[0];
    api->push = (void (*)(void *, int))addresses[1];
    api->pop = (void (*)(void *))addresses[2];
    return 1;
}

/* Never pop an unknown or unbalanced stack. The owner must refuse its result
 * if restoration fails; a guessed pop can corrupt another native scope. */
static __inline int sh_process_heap_leave(sh_process_heap_scope *scope)
{
    int depth;
    if (!scope || !scope->active) return 1;
    __try {
        depth = *(const int *)((const uint8_t *)scope->self + 0xc4);
        if (scope->active == 1 && depth == scope->depth) {
            scope->active = 0;
            return 1;
        }
        if (depth != scope->depth + 1 ||
            *(const int *)((const uint8_t *)scope->self + 0x44 + scope->depth * 4) != 0)
            return 0;
        scope->api->pop(scope->self);
        if (*(const int *)((const uint8_t *)scope->self + 0xc4) != scope->depth)
            return 0;
        scope->active = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* PushHeap silently ignores the wrong thread. Verify its effect before any
 * allocation; the stack holds 32 heap IDs at +0x44 and its depth at +0xc4. */
static __inline int sh_process_heap_enter(const sh_process_heap_api *api,
    sh_process_heap_scope *scope)
{
    int depth;
    if (!scope) return 0;
    memset(scope, 0, sizeof(*scope));
    if (!api || !api->get || !api->push || !api->pop) return 0;
    __try {
        scope->self = api->get();
        if (!scope->self) return 0;
        depth = *(const int *)((const uint8_t *)scope->self + 0xc4);
        if (depth < 0 || depth >= 32) return 0;
        scope->api = api;
        scope->depth = depth;
        scope->active = 1;
        api->push(scope->self, 0);
        if (*(const int *)((const uint8_t *)scope->self + 0xc4) == depth + 1 &&
            *(const int *)((const uint8_t *)scope->self + 0x44 + depth * 4) == 0) {
            scope->active = 2;
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    (void)sh_process_heap_leave(scope);
    return 0;
}

#endif
