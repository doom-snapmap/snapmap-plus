/* Native renderer synchronization and balanced suspension for resource owners. */
#ifndef SH_RENDER_SCOPE_H
#define SH_RENDER_SCOPE_H
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

/* Decode the renderer used by the verified native reload command. Both image
 * binders use this same instruction/virtual-slot contract without executing it. */
static __inline void **sh_render_scope_renderer(const uint8_t *command)
{
    static const size_t slots[] = {0x1a, 0x39, 0x5e, 0x6e};
    static const size_t calls[] = {0x33, 0x48, 0x68, 0x7b};
    static const uint32_t virtuals[] = {0x80, 0x110, 0x108, 0x110};
    void **renderer = NULL;
    if (!command) return NULL;
    __try {
        for (size_t i = 0; i < 4; i++) {
            int32_t displacement;
            uint32_t slot;
            void **next;
            if (memcmp(command + slots[i], "\x48\x8b\x0d", 3) ||
                memcmp(command + calls[i], "\xff\x90", 2)) return NULL;
            memcpy(&displacement, command + slots[i] + 3, 4);
            memcpy(&slot, command + calls[i] + 2, 4);
            next = (void **)(command + slots[i] + 7 + displacement);
            if (slot != virtuals[i] || (i && next != renderer)) return NULL;
            renderer = next;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return renderer;
}

typedef struct sh_render_scope {
    void **slot, *renderer;
    const DWORD *main_thread;
    DWORD thread;
    void (*adjust)(void *, int);
    void (*update)(void *);
    int count, active, attempted;
} sh_render_scope;

/* The slot must come from the verified ResourceReloadRenderScope binding.
 * These operations establish renderer ownership, not a map-load boundary.
 * Initialize each scope to zero and keep it alive until active becomes zero.
 * Nested scopes must leave in reverse order. Never manufacture a counter value
 * when a native call fails or another owner changes the renderer. */
static __inline int sh_render_scope_owner(const sh_render_scope *scope)
{
    return scope && scope->active && scope->main_thread &&
        scope->thread == GetCurrentThreadId() && *scope->main_thread == scope->thread &&
        scope->slot && *scope->slot == scope->renderer;
}

static __inline int sh_render_scope_leave(sh_render_scope *scope)
{
    int faulted = 0, count;
    if (!scope || !scope->active) return 1;
    __try {
        if (!sh_render_scope_owner(scope)) return 0;
        count = *(const int *)((const unsigned char *)scope->renderer + 0x14);
        if (!scope->attempted) {
            scope->active = 0;
            return count == scope->count;
        }
        /* An acquisition can throw before incrementing, or silently do nothing. */
        if (scope->active == 1 && count == scope->count) {
            scope->active = 0; return 1;
        }
        if (count != scope->count + 1) return 0;
        __try { scope->adjust(scope->renderer, -1); }
        __except (EXCEPTION_EXECUTE_HANDLER) { faulted = 1; }
        /* A throwing decrement may already have released our hold. Observe its
         * result before deciding whether cleanup is still owed. */
        if (!sh_render_scope_owner(scope) ||
            *(const int *)((const unsigned char *)scope->renderer + 0x14) != scope->count) return 0;
        scope->active = 0;
        return !faulted;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __inline int sh_render_scope_enter(void **slot, const DWORD *main_thread,
    sh_render_scope *scope)
{
    void **table;
    void (*synchronize)(void *, unsigned char, unsigned char, unsigned char);
    if (!scope || scope->active) return 0;
    memset(scope, 0, sizeof(*scope));
    __try {
        if (!slot || !main_thread || *main_thread != GetCurrentThreadId() || !*slot) return 0;
        scope->slot = slot; scope->renderer = *slot;
        scope->main_thread = main_thread; scope->thread = GetCurrentThreadId();
        scope->count = *(const int *)((const unsigned char *)scope->renderer + 0x14);
        if (scope->count < 0 || scope->count == INT_MAX) return 0;
        table = *(void ***)scope->renderer;
        synchronize = (void (*)(void *, unsigned char, unsigned char, unsigned char))table[0x80/8];
        scope->adjust = (void (*)(void *, int))table[0x110/8];
        scope->update = (void (*)(void *))table[0x108/8];
        if (!synchronize || !scope->adjust || !scope->update) return 0;
        scope->active = 1;
        synchronize(scope->renderer, 0, 0, 1);
        if (!sh_render_scope_owner(scope) ||
            *(const int *)((const unsigned char *)scope->renderer + 0x14) != scope->count) goto failed;
        scope->attempted = 1;
        scope->adjust(scope->renderer, 1);
        if (sh_render_scope_owner(scope) &&
            *(const int *)((const unsigned char *)scope->renderer + 0x14) == scope->count + 1) {
            scope->active = 2; return 1;
        }
failed:;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    (void)sh_render_scope_leave(scope);
    return 0;
}

static __inline int sh_render_scope_update(sh_render_scope *scope)
{
    __try {
        if (!sh_render_scope_owner(scope) || scope->active != 2 ||
            *(const int *)((const unsigned char *)scope->renderer + 0x14) != scope->count + 1) return 0;
        scope->update(scope->renderer);
        return sh_render_scope_owner(scope) &&
            *(const int *)((const unsigned char *)scope->renderer + 0x14) == scope->count + 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
#endif
