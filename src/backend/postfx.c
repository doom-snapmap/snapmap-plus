/* postfx.c -- DOOM 2016's radial blur fx item (FX_BLUR_RADIAL), drawn in the game's own post process (postfx.h).
 *
 * The engine already plays the item: its start (the fx item update, case 13) hands the item's centre, start time,
 * fade times and radius to each player view (SetBlurRadial, a relay 0 -> radius -> 0 over its life), and the view
 * copies centre and radius into its render view, which nothing reads. Snapmap+ draws it:
 *   - a five-byte call at the item's start (the `movss xmm0, [r14 + 0x468]` that loads the radius, VK 0x16F40DA,
 *     GL 0x16F4A5A) runs a relay that records the item (r14) before the same load; the SetBlurRadial detour then
 *     keeps a blur from the item: its centre, times and radius, and its shape from the item's own fields
 *     (postfx.h); an item that sets no cap (extents.x) is the game's and stays undrawn. On that game-thread call
 *     the render parm decls are found and the game clock is sampled;
 *   - the post process (RenderPostProcess, its detour, on the render thread) writes the blurs that are on into the
 *     render parms radialblur0..12 of the parm state it draws with (the frame's, or on Vulkan's asynchronous compute
 *     path the async context's), at the game time the sample advanced by the time since;
 *   - the post programs read them (modprog.h, shaders/radialblur.frag).
 * The blurs kept and their parameters are postfx_blurs.c. */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "postfx.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"
#include "host_image.h"
#include "signatures.h"
#include "script/script_api_common.h"

enum { PF_SITE, PF_SETBLUR, PF_RPP_VK, PF_RPP_GL, PF_SETVEC4, PF_COUNT };

/* An FX_BLUR_RADIAL action (idFXSingleAction, build 13954591). */
#define ACT_TYPE      0x14
#define ACT_SIZE      0x38
#define ACT_EXTENTS   0x48
#define ACT_COLOR     0x54
#define ACT_RADIUS    0x468
#define FX_BLUR_RADIAL 13

#define SITE_STOLEN   9
#define SETBLUR_STOLEN 14
#define RPP_STOLEN    14
#define RPP_ASYNC_LOAD 0x86     /* VK: mov r11, [rip + x], the async context (RenderPostProcess, 0x14186c9f6) */


typedef int (*set_blur_fn)(void *view, const float *centre, int start, int fade_in, int fade_out, int duration,
                           float radius);
typedef void (*rpp_vk_fn)(void **frame, void *view, float exposure, char async);
typedef void (*rpp_gl_fn)(void **frame, void *view, float exposure);
typedef void (*set_vec4_fn)(void *state, void *parm, const float *v);
typedef void *(*find_decl_fn)(void *manager, const char *name, unsigned char make_default);
typedef void *(*type_fn)(void *registry, const char *type);

static unsigned char *g_relay;               /* the relay page: code, then the recorded action at +32 */
static sh_patch_handle g_site_patch;
static set_blur_fn g_set_blur;
static rpp_vk_fn g_rpp_vk;
static rpp_gl_fn g_rpp_gl;
static set_vec4_fn g_set_vec4;
static void *g_parms[13];
static volatile int g_parms_ready;
static int g_gl, g_logged, g_parms_missing;
static const uint8_t *g_anchor;
static void *g_register;
static type_fn g_type;
static find_decl_fn g_decl;
static long g_drawn, g_kept;
static void **g_async_slot;               /* Vulkan: the async compute context's pointer */
/* The game clock seen from the render thread: the game time read on the game thread at the last item start, and the
 * performance counter then. The game's time getter is not safe off the game thread (it fails there). */
static SRWLOCK g_clock_lock = SRWLOCK_INIT;
static int g_clock_game;
static LONGLONG g_clock_qpc, g_qpc_hz;

static void pf_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void *clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK) return (void *)r[i].addr;
    return NULL;
}

/* ---------------------------------------------------------------------------------------- the blurs kept ---- */

/* The recorded action when it is the FX_BLUR_RADIAL item whose radius this call was given, else NULL. */
static const uint8_t *pf_action(float radius)
{
    const uint8_t *a = g_relay ? *(const uint8_t *const volatile *)(g_relay + 32) : NULL;
    int ok = 0;
    if (!a) return NULL;
    __try {
        ok = *(const int *)(a + ACT_TYPE) == FX_BLUR_RADIAL && *(const float *)(a + ACT_RADIUS) == radius;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok ? a : NULL;
}

/* ------------------------------------------------------------------------------------------------ hooks ---- */

static int pf_parms_ready(void);

/* The game ms now, on any thread: the last game-thread sample advanced by the time since (a pause or a timescale
 * during a blur is not followed; the blurs last a fraction of a second). 0 before the first sample. */
static int pf_now(int *now)
{
    LARGE_INTEGER q;
    int game;
    LONGLONG at;
    AcquireSRWLockShared(&g_clock_lock);
    game = g_clock_game;
    at = g_clock_qpc;
    ReleaseSRWLockShared(&g_clock_lock);
    if (!at || !g_qpc_hz) return 0;
    QueryPerformanceCounter(&q);
    *now = game + (int)((q.QuadPart - at) * 1000 / g_qpc_hz);
    return 1;
}

static void pf_clock_sample(void)
{
    LARGE_INTEGER q;
    int game = 0;
    if (!sh_script_api_now_ms(&game)) return;
    QueryPerformanceCounter(&q);
    AcquireSRWLockExclusive(&g_clock_lock);
    g_clock_game = game;
    g_clock_qpc = q.QuadPart;
    ReleaseSRWLockExclusive(&g_clock_lock);
}

static int set_blur_detour(void *view, const float *centre, int start, int fade_in, int fade_out, int duration,
                           float radius)
{
    const uint8_t *a = pf_action(radius);
    if (a && centre) {
        float color[4], extents[3], size = 0.0f, c[3];
        int got = 0;
        __try {
            memcpy(color, a + ACT_COLOR, sizeof color);
            memcpy(extents, a + ACT_EXTENTS, sizeof extents);
            size = *(const float *)(a + ACT_SIZE);
            memcpy(c, centre, sizeof c);
            got = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { got = 0; }
        /* the parm decls are found here, on the game thread: a decl lookup from the render thread can wait on the
         * game thread, which waits on the frame (the boot hung at its loading screen that way) */
        if (got && sh_postfx_shape_drawn(color, extents, size, radius) && pf_parms_ready()) {
            int now = 0;
            pf_clock_sample();
            sh_postfx_keep(c, start, fade_in, fade_out, duration, radius, color, extents, size);
            if (InterlockedIncrement(&g_kept) <= 4 && sh_script_api_now_ms(&now))
                pf_log("POSTFX: a radial blur item started at game ms %d (now %d): radius %.1f, %d + %d ms of %d, "
                       "falloff %.2f, strength %.2f", start, now, radius, fade_in, fade_out, duration, color[0], color[1]);
        }
    }
    return g_set_blur(view, centre, start, fade_in, fade_out, duration, radius);
}

static void *renderparm_manager(void)
{
    int32_t displacement;
    void *r = NULL;
    void **vt;
    if (!g_anchor || !g_type || !g_decl) return NULL;
    __try {
        if (memcmp(g_anchor + 0x10, "\x48\x8b\x0d", 3)) return NULL;
        memcpy(&displacement, g_anchor + 0x13, 4);
        r = *(void **)(g_anchor + 0x17 + displacement);
        if (!r) return NULL;
        vt = *(void ***)r;
        if (!vt || vt[7] != g_register || vt[11] != (void *)g_type) return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return g_type(r, "renderParm");
}

/* The radial blur parms' decls (once the post programs have registered them). Game thread only. */
static int pf_parms_ready(void)
{
    void *m;
    int i;
    if (g_parms_ready) return 1;
    if (!(m = renderparm_manager())) return 0;
    for (i = 0; i < 13; i++) {
        char name[16];
        _snprintf_s(name, sizeof name, _TRUNCATE, "radialblur%d", i);
        __try { g_parms[i] = g_decl(m, name, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { g_parms[i] = NULL; }
        if (!g_parms[i]) {
            if (!g_parms_missing++)
                pf_log("POSTFX: the render parm %s is not registered; the radial blur is not drawn", name);
            return 0;
        }
    }
    MemoryBarrier();
    g_parms_ready = 1;
    return 1;
}

/* The parm state the post process draws with: the frame's (**frame), or on Vulkan's asynchronous compute path the
 * async context's (*[slot], the `mov r11, [rip + x]` at RenderPostProcess + 0x86), as RenderPostProcess picks it. */
static void pf_write(void **frame, int async)
{
    float v[13][4];
    int now = 0, i, drawn;
    void *state;
    if (!g_set_vec4 || !frame || !g_parms_ready) return;     /* found on the game thread (set_blur_detour) */
    if (async && !g_async_slot) return;
    if (!pf_now(&now)) return;
    drawn = sh_postfx_parms(now, g_gl ? -1.0f : 1.0f, v);
    __try {
        state = async ? *(void **)*g_async_slot : *(void **)*frame;
        if (!state) return;
        for (i = 0; i < 13; i++) g_set_vec4(state, g_parms[i], v[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (drawn && InterlockedIncrement(&g_drawn) == 1)
        pf_log("POSTFX: radial blur drawn (%d on, first at game ms %d)", drawn, now);
}

static void pf_first(void)
{
    static volatile LONG once;
    if (!InterlockedExchange(&once, 1)) pf_log("POSTFX: the post process ran (first frame)");
}

static void rpp_vk_detour(void **frame, void *view, float exposure, char async)
{
    pf_first();
    pf_write(frame, async != 0);
    g_rpp_vk(frame, view, exposure, async);
}

static void rpp_gl_detour(void **frame, void *view, float exposure)
{
    pf_first();
    pf_write(frame, 0);
    g_rpp_gl(frame, view, exposure);
}

/* The relay the item's start calls: record r14 (the action), then the load the call replaced. */
static unsigned char *near_page(uintptr_t call)
{
    SYSTEM_INFO info;
    uintptr_t step, center, delta;
    GetSystemInfo(&info);
    step = info.dwAllocationGranularity;
    center = call & ~(step - 1);
    for (delta = step; delta < 0x7fff0000u; delta += step) {
        uintptr_t cand[2] = { center >= delta ? center - delta : 0, center + delta };
        int i;
        for (i = 0; i < 2; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            unsigned char *p;
            if (cand[i] < (uintptr_t)info.lpMinimumApplicationAddress || cand[i] > (uintptr_t)info.lpMaximumApplicationAddress ||
                !VirtualQuery((void *)cand[i], &mbi, sizeof mbi) || mbi.State != MEM_FREE)
                continue;
            p = (unsigned char *)VirtualAlloc((void *)cand[i], 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return NULL;
}

static int install_relay(const sig_result *site)
{
    static const unsigned char load[SITE_STOLEN] = { 0xF3, 0x41, 0x0F, 0x10, 0x86, 0x68, 0x04, 0x00, 0x00 };
    unsigned char after[SITE_STOLEN];
    int32_t rel;
    intptr_t distance;
    if (!(g_relay = near_page(site->addr))) return 0;
    /* mov [rip + 25], r14 (the slot at +32); movss xmm0, [r14 + 0x468]; ret */
    g_relay[0] = 0x4C; g_relay[1] = 0x89; g_relay[2] = 0x35;
    rel = 32 - 7;
    memcpy(g_relay + 3, &rel, 4);
    memcpy(g_relay + 7, load, SITE_STOLEN);
    g_relay[16] = 0xC3;
    memset(g_relay + 32, 0, 8);
    FlushInstructionCache(GetCurrentProcess(), g_relay, 40);
    distance = (intptr_t)g_relay - (intptr_t)(site->addr + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) return 0;
    rel = (int32_t)distance;
    after[0] = 0xE8;
    memcpy(after + 1, &rel, 4);
    memset(after + 5, 0x90, SITE_STOLEN - 5);
    return code_patch_sig(site, load, after, SITE_STOLEN, &g_site_patch) == B2_PATCH_OK;
}

static int detour(void *target, void *fn, size_t stolen, void **orig)
{
    void *tramp = hook_prepare(target, fn, stolen);
    if (!tramp) return 0;
    *orig = tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) *orig = NULL;
        return 0;
    }
    return 1;
}

int sh_postfx_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    sig_result own[PF_COUNT];
    int i;
    if (g_set_blur) return 1;
    g_gl = strcmp(sh_host_renderer_name(), "opengl") == 0;
    for (i = 0; i < PF_COUNT; i++) {
        if (i == (g_gl ? PF_RPP_VK : PF_RPP_GL)) { memset(&own[i], 0, sizeof own[i]); continue; }
        if (sig_resolve_one(module_base, &SH_POSTFX_SIGNATURES[i], &own[i]) != SIG_OK) {
            pf_log("POSTFX: %s did not resolve on this executable; the radial blur is not drawn",
                   SH_POSTFX_SIGNATURES[i].name);
            return 0;
        }
    }
    g_anchor = (const uint8_t *)clean(results, count, "DeclRegistryAnchor");
    g_register = clean(results, count, "DeclRegisterFile");
    g_type = (type_fn)clean(results, count, "DeclTypeByName");
    g_decl = (find_decl_fn)clean(results, count, "DeclFind");
    if (!g_anchor || !g_register || !g_type || !g_decl) {
        pf_log("POSTFX: the decl registry is unresolved; the radial blur is not drawn");
        return 0;
    }
    g_set_vec4 = (set_vec4_fn)own[PF_SETVEC4].addr;
    {
        LARGE_INTEGER hz;
        QueryPerformanceFrequency(&hz);
        g_qpc_hz = hz.QuadPart;
    }
    if (!g_gl) {
        const uint8_t *at = (const uint8_t *)own[PF_RPP_VK].addr + RPP_ASYNC_LOAD;
        int32_t disp;
        if (memcmp(at, "\x4c\x8b\x1d", 3)) {
            pf_log("POSTFX: the post process's async context load is not where it was read; the radial blur is not drawn");
            return 0;
        }
        memcpy(&disp, at + 3, 4);
        g_async_slot = (void **)(at + 7 + disp);
    }
    if (!detour((void *)own[g_gl ? PF_RPP_GL : PF_RPP_VK].addr, g_gl ? (void *)rpp_gl_detour : (void *)rpp_vk_detour,
                RPP_STOLEN, g_gl ? (void **)&g_rpp_gl : (void **)&g_rpp_vk)) {
        pf_log("POSTFX: the post process could not be detoured (%s); the radial blur is not drawn", hook_last_failure());
        return 0;
    }
    if (!install_relay(&own[PF_SITE])) {
        pf_log("POSTFX: the radial blur item's start could not be patched; the radial blur is not drawn");
        return 0;
    }
    if (!detour((void *)own[PF_SETBLUR].addr, (void *)set_blur_detour, SETBLUR_STOLEN, (void **)&g_set_blur)) {
        pf_log("POSTFX: SetBlurRadial could not be detoured (%s); the radial blur is not drawn", hook_last_failure());
        return 0;
    }
    pf_log("POSTFX: radial blur installed (%s): the item start relay at %p, SetBlurRadial, the post process",
           g_gl ? "OpenGL" : "Vulkan", (void *)own[PF_SITE].addr);
    return 1;
}
