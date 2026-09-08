/* nav_play.c -- see nav_play.h for what this is and why it hooks where it does. */
#include <windows.h>
#include <stdio.h>

#include "nav_play.h"
#include "nav_bake.h"
#include "hook.h"

void backend_log(const char *message);

/* MOV RAX,RSP / PUSH RBP / PUSH R12..R15 / MOV RBP,RSP -- 15 bytes, ending on a
 * whole-instruction boundary, and every one of them position-independent (no
 * RIP-relative operand, no relative jmp or call). The installer needs >= 14. */
#define SNAPBUILD_STOLEN 15

/* int(void *, void *, void *) -- three register args and no stack args, read off
 * the call site at 0x4EE428 (RCX=R14, RDX=[RBP+0x900], R8=RDI) and off the
 * prologue, which homes RBX/RSI/RDI into the caller's shadow space rather than
 * reading anything above the frame. The return value IS used: the call site does
 * MOV EBX,EAX immediately after, so the detour must pass it through. */
typedef int (*snapbuild_fn_t)(void *a, void *b, void *c);

static snapbuild_fn_t g_orig;

static int nav_snapbuild_detour(void *a, void *b, void *c)
{
    /* The whole reason this hook exists. Guarded and non-fatal: the marks are a
     * convenience over the map as loaded, and failing to read them must never be
     * worse than not having read them. A fault here would otherwise land in the
     * middle of the engine's map build, which is the exact shape of the bug this
     * replaces. */
    __try {
        sh_nav_bake_refresh_live();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("NAV: the pre-build live read faulted; this Play uses the map as loaded");
    }
    return g_orig(a, b, c);
}

int sh_nav_play_install(void *snapbuild_fn, int status_ok)
{
    char line[200];
    void *tramp;

    if (snapbuild_fn == NULL) {
        backend_log("NAV: pre-build live read SKIPPED -- SnapMapEditToSnapBuild not resolved");
        return 0;
    }
    if (!status_ok) {
        /* Same rule the rawmap swap follows: a prologue that only resolved through
         * the hook-tolerant fallback is already detoured by somebody else, and
         * installing over it steals detour bytes rather than the real prologue. */
        backend_log("NAV: pre-build live read SKIPPED -- SnapMapEditToSnapBuild resolved via "
                    "hook-tolerant fallback (prologue already hooked)");
        return 0;
    }
    if (g_orig != NULL) return 1;

    tramp = install_inline_hook(snapbuild_fn, (void *)nav_snapbuild_detour, SNAPBUILD_STOLEN);
    if (tramp == NULL) {
        backend_log("NAV: pre-build live read FAIL -- install_inline_hook returned NULL");
        return 0;
    }
    g_orig = (snapbuild_fn_t)tramp;

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: pre-build live read installed at %p (trampoline %p, stolen %d) -- "
                "a volume ticked this session is baked on Play without saving first",
                snapbuild_fn, tramp, SNAPBUILD_STOLEN);
    backend_log(line);
    return 1;
}
