/* palette_refresh.c -- one-shot native rebuild after new decl registration.
 *
 * The engine's entity palette is a derived editor catalog. Registering a new
 * decl after the initial palette build does not make it appear in that catalog
 * by itself. The native SnapPaletteBuild routine rebuilds the existing palette
 * in place; this service calls that routine once, synchronously from the
 * dynamic decl server's main-thread command after registration succeeds.
 *
 * This is deliberately not a rawmap hook, an editor injection, or a refresh
 * retry loop. A missing/unsupported build, invalid editor object, vtable
 * mismatch, or native exception is terminal for this process. The decl server
 * materializes new editor-entity decls before this call, so the builder is
 * needed for either editor initialization state and is invoked exactly once.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "backend_log.h"
#include "iface_engine.h"
#include "palette_refresh.h"

#define PR_EDITOR_SINGLETON_RVA 0x3056748u
#define PR_EDITOR_PALETTE_OFF   0x20660u
#define PR_PALETTE_VTABLE_RVA   0x20499A0u
#define PR_BUILDER_RVA          0x54AEE0u

enum {
    PR_STATE_IDLE = 0,
    PR_STATE_PENDING,
    PR_STATE_APPLIED,
    PR_STATE_REFUSED
};

typedef void (*palette_build_fn)(void *palette, void *progress);

static volatile LONG g_state = PR_STATE_IDLE;
static const uint8_t *g_module_base;
static palette_build_fn g_builder;

#ifdef SH_PALETTE_REFRESH_TESTING
static volatile LONG g_test_call_count;
#endif

static const sig_result *pr_result(const sig_result *results, size_t count,
                                   const char *name)
{
    size_t i;
    if (!results || !name) return NULL;
    for (i = 0; i < count; i++)
        if (results[i].name && strcmp(results[i].name, name) == 0)
            return &results[i];
    return NULL;
}

/* 1 = non-null, 0 = null, -1 = the object was not readable. */
static int pr_read_ptr(const void *address, void **out)
{
    if (!address || !out) return -1;
    __try {
        *out = *(void *const *)address;
        return *out ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void pr_refuse(const char *reason)
{
    InterlockedExchange(&g_state, PR_STATE_REFUSED);
    backend_log(reason ? reason : "palette-refresh REFUSED");
}

int sh_palette_refresh_install(const sig_result *results, size_t count,
                               const uint8_t *module_base)
{
    const sig_result *builder;

    if (InterlockedCompareExchange(&g_state, PR_STATE_IDLE, PR_STATE_IDLE) != PR_STATE_IDLE ||
        g_builder != NULL)
        return 0;
    builder = pr_result(results, count, "SnapPaletteBuild");
    /* This call has a vtable/data-layout contract, so a hook-tolerant resolve
     * is not acceptable even though it is callable for ordinary leaf calls. */
    if (!builder || builder->status != SIG_OK || !builder->addr || !module_base) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild requires a clean SIG_OK resolve");
        return 0;
    }

    if (builder->rva != PR_BUILDER_RVA ||
        builder->addr != (uintptr_t)module_base + PR_BUILDER_RVA) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild clean resolve is not the pinned exact address");
        return 0;
    }

    g_module_base = module_base;
    g_builder = (palette_build_fn)builder->addr;
    backend_log("palette-refresh installed: waiting for complete native decl registration");
    return 1;
}

int sh_palette_refresh_after_decl_registration(void)
{
    const uint8_t *editor;
    void *palette_vtable = NULL;
    int vtable_status;

    if (InterlockedCompareExchange(&g_state, PR_STATE_PENDING, PR_STATE_IDLE) != PR_STATE_IDLE)
        return 0;
    backend_log("palette-refresh ARMED: complete native decl registration succeeded");

    if (!g_module_base || !g_builder) {
        pr_refuse("palette-refresh REFUSED: clean builder/module dependency unavailable");
        return 0;
    }
    editor = sh_iface_engine_editor_base();
    if (!editor || editor != g_module_base + PR_EDITOR_SINGLETON_RVA) {
        pr_refuse("palette-refresh REFUSED: editor singleton identity validation failed");
        return 0;
    }

    /* Validate the palette object before invoking the engine-owned rebuild. */
    vtable_status = pr_read_ptr(editor + PR_EDITOR_PALETTE_OFF, &palette_vtable);
    if (vtable_status < 0 || vtable_status == 0 ||
        palette_vtable != (void *)(g_module_base + PR_PALETTE_VTABLE_RVA)) {
        pr_refuse("palette-refresh REFUSED: editor palette object/vtable validation failed");
        return 0;
    }

    /* The actual call consumes the state before invoking the engine. */
    if (InterlockedCompareExchange(&g_state, PR_STATE_APPLIED, PR_STATE_PENDING) != PR_STATE_PENDING)
        return 0;
    __try {
#ifdef SH_PALETTE_REFRESH_TESTING
        InterlockedIncrement(&g_test_call_count);
#endif
        g_builder((void *)(editor + PR_EDITOR_PALETTE_OFF), NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pr_refuse("palette-refresh REFUSED: SnapPaletteBuild raised an exception");
        return 0;
    }
    backend_log("palette-refresh FIRED: native SnapPaletteBuild completed once");
    return 1;
}

#ifdef SH_PALETTE_REFRESH_TESTING
void sh_palette_refresh_test_reset(void)
{
    InterlockedExchange(&g_state, PR_STATE_IDLE);
    InterlockedExchange(&g_test_call_count, 0);
    g_module_base = NULL;
    g_builder = NULL;
}

void sh_palette_refresh_test_bind(const uint8_t *module_base, void *builder)
{
    g_module_base = module_base;
    g_builder = (palette_build_fn)builder;
}

int sh_palette_refresh_test_state(void)
{
    return (int)InterlockedCompareExchange(&g_state, 0, 0);
}

int sh_palette_refresh_test_call_count(void)
{
    return (int)InterlockedCompareExchange(&g_test_call_count, 0, 0);
}
#endif
