/* nav_preview.c -- draw baked surfaces in the editor's native render pass. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nav_preview.h"
#include "hook.h"
#include "backend_log.h"

#define PREVIEW_LINES 8192
#define PREVIEW_VIEWS 16
#define WORLD_DEBUG_OFFSET 0x43218
#define VIEW_MVP_OFFSET 4720
#define DRAW_MATRIX_OFFSET 0xc84

/* The native consumer reads a 48-byte line and this small list header. It
 * never calls methods on either header. Keep these private: the engine's
 * ordinary debug list is disabled by default and shared with unrelated tools. */
typedef struct preview_line {
    float color[4], start[3], end[3];
    unsigned char depth_test, padding[3];
    int lifetime;
} preview_line;
typedef struct preview_list {
    void *unused;
    preview_line *data;
    int count, capacity;
} preview_list;
typedef struct preview_view { const void *view, *world; } preview_view;

typedef void (*render_stage_fn)(void *, const unsigned char *, unsigned char,
                                unsigned char, void *, void *);
typedef void (*render_post_fn)(void **);
typedef unsigned char (*draw_begin_fn)(void *, void *, void *);
typedef void (*draw_end_fn)(void *);
typedef void (*draw_matrix_fn)(void *, const void *);
typedef void (*draw_lines_fn)(void **);
typedef void (*draw_target_fn)(void *, void *, int, int);

static SRWLOCK g_preview_lock = SRWLOCK_INIT;
static preview_list g_lines;
static preview_line *g_pending_lines;
static int g_pending_count, g_pending_overflow;
static const void *g_world, *g_pending_world;
static preview_view g_views[PREVIEW_VIEWS];
static unsigned g_next_view;
static unsigned char *g_draw_world;
static void *g_immediate;
static size_t g_world_backend_offset;
static render_stage_fn g_stage;
static render_post_fn g_post;
static draw_begin_fn g_begin;
static draw_end_fn g_end;
static draw_matrix_fn g_matrix;
static draw_lines_fn g_draw;
static draw_target_fn g_target;
static volatile LONG g_ready, g_faulted;

void sh_nav_preview_clear(void)
{
    AcquireSRWLockExclusive(&g_preview_lock);
    g_lines.count = 0;
    g_world = NULL;
    ReleaseSRWLockExclusive(&g_preview_lock);
}

void sh_nav_preview_begin(void *world)
{
    g_pending_count = 0;
    g_pending_overflow = 0;
    /* idRenderWorld owns the renderer-side world captured by render jobs.
     * Its member offset differs between Vulkan and OpenGL. */
    g_pending_world = g_ready && world ?
        *(void **)((unsigned char *)world+g_world_backend_offset) : NULL;
}

void sh_nav_preview_add_line(const float start[3], const float end[3], void *unused)
{
    preview_line *line;
    (void)unused;
    if (!g_ready || g_faulted || !g_pending_lines) return;
    if (g_pending_count == PREVIEW_LINES) { g_pending_overflow = 1; return; }
    line = &g_pending_lines[g_pending_count++];
    memset(line, 0, sizeof *line);
    line->color[0] = 0.15f; line->color[1] = 1.0f;
    line->color[2] = 0.25f; line->color[3] = 1.0f;
    memcpy(line->start, start, sizeof line->start);
    memcpy(line->end, end, sizeof line->end);
    line->depth_test = 1;
}

void sh_nav_preview_publish(void)
{
    preview_line *old;
    if (!g_ready || g_faulted || !g_pending_lines || !g_lines.data) {
        sh_nav_preview_clear();
        return;
    }
    AcquireSRWLockExclusive(&g_preview_lock);
    old = g_lines.data;
    g_lines.data = g_pending_lines;
    g_pending_lines = old;
    g_lines.count = g_pending_overflow ? 0 : g_pending_count;
    g_world = g_pending_world;
    ReleaseSRWLockExclusive(&g_preview_lock);
}

static void preview_stage(void *self, const unsigned char *frame,
                          unsigned char a, unsigned char b, void *c, void *d)
{
    /* The renderer passes the exact world and render-view copy to this stage.
     * A view's optional owningWorld field is null in the SnapMap editor. */
    __try {
        const void *view = *(const void *const *)(frame + 40);
        const void *world = *(const void *const *)(frame + 24);
        unsigned i;
        AcquireSRWLockExclusive(&g_preview_lock);
        for (i = 0; i < PREVIEW_VIEWS; i++) if (g_views[i].view == view) break;
        if (i == PREVIEW_VIEWS) i = g_next_view++ % PREVIEW_VIEWS;
        g_views[i].view = view; g_views[i].world = world;
        ReleaseSRWLockExclusive(&g_preview_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted, 1); }
    g_stage(self, frame, a, b, c, d);
}

static void preview_draw(void **job, void *target)
{
    void *context[5], *command, *view;
    unsigned i;
    int begun = 0, rebound = 0;
    if (!g_ready || g_faulted || !TryAcquireSRWLockShared(&g_preview_lock)) return;
    __try {
        if (!g_world || !g_lines.count) __leave;
        view = job[5]; command = job[0];
        for (i = 0; i < PREVIEW_VIEWS; i++)
            if (g_views[i].view == view && g_views[i].world == g_world) break;
        if (i == PREVIEW_VIEWS || !view || !command) __leave;
        if (target) {
            rebound = 1;
            g_target(command, target, 0, 0);
        }
        context[0] = command; context[1] = view; context[2] = job[4];
        context[3] = g_draw_world;
        /* The consumer requests its matrix at +0xc84. Supply the actual
         * world-space MVP from this frame's view without copying stale camera
         * matrices or retaining the job after its renderer-owned lifetime. */
        context[4] = (unsigned char *)view + VIEW_MVP_OFFSET - DRAW_MATRIX_OFFSET;
        g_matrix(*(void **)command, (unsigned char *)view + VIEW_MVP_OFFSET);
        if (!g_begin(g_immediate, NULL, NULL)) __leave;
        begun = 1;
        g_draw(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        backend_log("NAV: editor preview drawing disabled after a renderer fault");
    }
    __try { if (begun) g_end(g_immediate); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted, 1); }
    __try { if (rebound) g_target(command, job[13], 0, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted, 1); }
    ReleaseSRWLockShared(&g_preview_lock);
}

static void preview_post(void **job)
{
    void *target = NULL;
    g_post(job);
    /* OpenGL keeps separate post-process and presentation images. In the
     * editor the final composite can read the image behind job[16], even
     * though the post job leaves job[13] bound. Draw once into that image and
     * restore the post target before handing control back to the engine.
     * Vulkan's render-pass attachments remain owned by its native post job. */
    if (g_target) __try {
        target = job[16] ? *(void **)job[16] : NULL;
        if (target == job[13]) target = NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted, 1); }
    preview_draw(job, target);
}

static uintptr_t preview_address(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (r[i].status == SIG_OK && r[i].name && !strcmp(r[i].name, name)) return r[i].addr;
    return 0;
}

int sh_nav_preview_install(const sig_result *r, size_t n)
{
    /* Whole, position-independent prologues on the two supported renderers.
     * The function itself is obtained from the uniquely resolved job slot. */
    static const unsigned char vk_head[] = {
        0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x48,0x83,0xec,0x60,
        0x65,0x48,0x8b,0x04,0x25,0x58,0,0,0 };
    static const unsigned char gl_head[] = {
        0x40,0x53,0x57,0x48,0x83,0xec,0x68,0x65,0x48,0x8b,0x04,0x25,0x58,0,0,0 };
    uintptr_t stage = preview_address(r,n,"NavRenderStage");
    uintptr_t slot = preview_address(r,n,"NavPostProcessSlot");
    unsigned char *post, *draw;
    int32_t rel;
    size_t stolen = 0, i;
    if (g_ready && hook_is_installed((void *)g_stage) && hook_is_installed((void *)g_post)) return 1;
    InterlockedExchange(&g_ready,0);
    if (g_post) {
        if (!hook_unpatch((void *)g_post)) return 0;
        g_post = NULL;
    }
    if (g_stage) {
        if (!hook_unpatch((void *)g_stage)) return 0;
        g_stage = NULL;
    }
    g_begin = (draw_begin_fn)preview_address(r,n,"NavRenderBegin");
    g_end = (draw_end_fn)preview_address(r,n,"NavRenderEnd");
    g_matrix = (draw_matrix_fn)preview_address(r,n,"NavRenderMatrix");
    g_draw = (draw_lines_fn)preview_address(r,n,"NavRenderLines");
    if (!stage || !slot || !g_begin || !g_end || !g_matrix || !g_draw) goto failed;
    __try {
        const unsigned char *load = (const unsigned char *)slot;
        if (memcmp(load,"\x48\x8b\x15",3)) __leave;
        memcpy(&rel,load+3,4);
        post = *(unsigned char **)(slot+7+rel);
        if (!memcmp(post,vk_head,sizeof vk_head)) {
            stolen = sizeof vk_head; g_world_backend_offset = 0x852c0;
        } else if (!memcmp(post,gl_head,sizeof gl_head)) {
            sig_result target;
            stolen = sizeof gl_head; g_world_backend_offset = 0x85280;
            if (sig_resolve_one((const uint8_t *)GetModuleHandleW(NULL),
                                &NAV_RENDER_TARGET_GL_SIGNATURE, &target) != SIG_OK) __leave;
            g_target = (draw_target_fn)target.addr;
        }
        if (!stolen) __leave;
        draw = (unsigned char *)g_draw;
        /* The first RIP-relative LEA RCX in the line consumer selects the
         * native immediate renderer on both verified builds. */
        for (i = 0; i + 7 <= 96; i++) if (!memcmp(draw+i,"\x48\x8d\x0d",3)) {
            memcpy(&rel,draw+i+3,4); g_immediate = draw+i+7+rel; break;
        }
        if (!g_immediate) __leave;
        if (!g_draw_world) g_draw_world = (unsigned char *)calloc(1,WORLD_DEBUG_OFFSET+sizeof(void *));
        if (!g_lines.data) g_lines.data = (preview_line *)calloc(PREVIEW_LINES,sizeof(preview_line));
        if (!g_pending_lines) g_pending_lines = (preview_line *)calloc(PREVIEW_LINES,sizeof(preview_line));
        if (!g_draw_world || !g_lines.data || !g_pending_lines) __leave;
        *(preview_list **)(g_draw_world+WORLD_DEBUG_OFFSET) = &g_lines;
        g_lines.capacity = PREVIEW_LINES;
        g_stage = (render_stage_fn)hook_prepare((void *)stage,preview_stage,16);
        if (!g_stage) __leave;
        g_post = (render_post_fn)hook_prepare(post,preview_post,stolen);
        if (!g_post || hook_commit((void *)g_stage) != B2_PATCH_OK ||
            hook_commit((void *)g_post) != B2_PATCH_OK) __leave;
        InterlockedExchange(&g_ready,1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_faulted,1); }
    if (g_ready) {
        backend_log("NAV: native editor navigation overlay installed"); return 1;
    }
failed:
    if (g_post && hook_unpatch((void *)g_post)) g_post = NULL;
    if (g_stage && hook_unpatch((void *)g_stage)) g_stage = NULL;
    backend_log("NAV: native editor navigation overlay unavailable");
    return 0;
}
