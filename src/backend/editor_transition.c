#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include "editor_transition.h"
#include "engine_globals.h"
#include "hook.h"
#include "backend_log.h"
#include "process_heap_scope.h"
#include "render_scope.h"

enum { ET_IDLE, ET_LOADING, ET_ACTIVATING, ET_READING, ET_FINISHING, ET_RECOVERY };
enum { ET_WORLD = 0x198, ET_SOUND = 0x204c0, ET_MAP = 0x204c8,
    ET_EDIT = 0x204d0, ET_UI = 0x21088, ET_SCENE = 0x170 };
typedef int (*et_read_fn)(const char *, void **, void *, void *, void *);
static struct {
    void *editor;
    void **renderer, **sound, **common;
    DWORD *thread;
    void (*set_active)(void *, unsigned char), (*clear_scene)(void *);
    sh_process_heap_api heap;
    const uint8_t *read_site, *read_return;
    et_read_fn read;
    int installed, faulted;
} g_et_api;
static __declspec(thread) void *g_et_frame;
static __declspec(thread) struct {
    int phase, read, activated, native_uncertain;
    void *editor;
    char id[21], error[384];
    sh_editor_transition_callbacks callbacks;
    sh_render_scope render;
    sh_process_heap_scope heap;
} g_et;

static void et_error(const char *message)
{ if (!g_et.error[0]) snprintf(g_et.error, sizeof(g_et.error), "%s", message); }
static const uint8_t *et_site(const sig_result *results, size_t count,
    const uint8_t *base, const char *name)
{
    const uint8_t *out = NULL;
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name)) {
        if (out || results[i].status != SIG_OK || !results[i].addr ||
            results[i].addr != (uintptr_t)base + results[i].rva) return NULL;
        out = (const uint8_t *)results[i].addr;
    }
    return out;
}
static void *et_relative(const uint8_t *at, const char *prefix, size_t size)
{
    int32_t displacement;
    if (memcmp(at, prefix, size)) return NULL;
    memcpy(&displacement, at + size, 4);
    return (void *)(at + size + 4 + displacement);
}
int sh_editor_transition_bind(const sig_result *results, size_t count, const uint8_t *base)
{
    const uint8_t *load, *inner, *read, *active, *deactivate, *command;
    void **renderer, **sound;
    void *clear;
    sh_process_heap_api heap;
    if (!results || !base || g_et_api.installed ||
        !sh_process_heap_bind(&heap, results, count, base)) return 0;
    load = et_site(results, count, base, "EditorLoadMap");
    active = et_site(results, count, base, "EditorSetActive");
    command = et_site(results, count, base, "ResourceReloadRenderScope");
    if (!load || !active || !(renderer = sh_render_scope_renderer(command))) return 0;
    __try {
        inner = et_relative(load + 0xfb, "\xe8", 1);
        if (!inner || memcmp(load + 0xf4, "\x48\x8b\x56\x10\x48\x8b\xcb", 7) ||
            memcmp(inner + 0x127, "\x48\x89\x9f\xd0\x04\x02\x00\x48\x8d\xb7\xc8\x04\x02\x00", 14) ||
            memcmp(inner + 0x152, "\x48\x89\x1e\x48\x8d\xaf\xd8\x36\x02\x00", 10) ||
            memcmp(inner + 0x164, "\x48\x89\x6c\x24\x20\x4c\x8d\x8f\x60\x06\x02\x00"
                "\x4c\x8d\x87\xc0\x06\x02\x00\x48\x8b\xd6\x49\x8b\xcd", 25)) return 0;
        read = et_relative(inner + 0x17d, "\xe8", 1);
        if (!read || memcmp(read, "\x40\x53\x55\x56\x57\x41\x56\x41\x57\x48\x81\xec\x18\x0d\x00\x00"
            "\x48\xc7\x44\x24\x30\xfe\xff\xff\xff", 25) ||
            memcmp(read + 0x2b, "\x4d\x8b\xf9\x49\x8b\xe8\x4c\x8b\xf2\x48\x8b\xf9"
                "\x48\x8b\xb4\x24\x70\x0d\x00\x00", 20)) return 0;
        deactivate = et_relative(active + 0x63, "\xe8", 1);
        if (!deactivate || memcmp(active + 0x68, "\x40\x88\x7b\x09", 4) ||
            memcmp(deactivate + 0x3c9, "\x48\x8d\x8f\x70\x01\x00\x00", 7) ||
            memcmp(deactivate + 0x3e1, "\x48\x8b\x97\xc0\x04\x02\x00\x48\x8b\x01\xff\x50\x28"
                "\x48\x89\xaf\xc0\x04\x02\x00", 20)) return 0;
        clear = et_relative(deactivate + 0x3d0, "\xe8", 1);
        sound = et_relative(deactivate + 0x3da, "\x48\x8b\x0d", 3);
        if (!clear || !sound) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_et_api.editor = (void *)glb_resolve(base, "editor_singleton", NULL);
    g_et_api.thread = (DWORD *)glb_resolve(base, "main_thread_id", NULL);
    g_et_api.common = (void **)glb_resolve(base, "validator_manager", NULL);
    if (!g_et_api.editor || !g_et_api.thread || !g_et_api.common) return 0;
    g_et_api.renderer = renderer; g_et_api.sound = sound; g_et_api.heap = heap;
    g_et_api.set_active = (void (*)(void *, unsigned char))active;
    g_et_api.clear_scene = (void (*)(void *))clear;
    g_et_api.read_site = read; g_et_api.read_return = inner + 0x182;
    return 1;
}
static int et_thread(void)
{ return g_et_api.thread && *g_et_api.thread == GetCurrentThreadId(); }
void *sh_editor_transition_capture(const void *editor)
{
    const unsigned char *ed = editor, *common;
    __try {
        if (!g_et_api.installed || g_et_api.faulted || g_et.phase || !editor ||
            editor != g_et_api.editor || !et_thread() || !ed[8] || !ed[9] || ed[0x2120d] ||
            (*(void ***)editor)[0x48/8] != (void *)g_et_api.set_active ||
            !(common = *g_et_api.common) || common[0xf576] ||
            !*(void *const *)(ed + ET_EDIT) || !*(void *const *)(ed + ET_UI) ||
            !*(void *const *)(ed + ET_WORLD) || !*(void *const *)(ed + ET_SOUND)) return NULL;
        return *(void *const *)(ed + ET_MAP);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static int et_release_scopes(void)
{
    int ok = 1;
    if (!sh_process_heap_leave(&g_et.heap)) ok = 0;
    if (!sh_render_scope_leave(&g_et.render)) ok = 0;
    return ok;
}
int sh_editor_transition_frame_enter(void *editor)
{
    __try {
        if (g_et_frame || editor != g_et_api.editor || !et_thread()) return 0;
        if (g_et.phase == ET_RECOVERY) {
            et_release_scopes();
            if (g_et.render.active || g_et.heap.active) return 0;
            memset(&g_et, 0, sizeof(g_et));
        }
        if (g_et.phase || g_et_api.faulted) return 0;
        g_et_frame = editor; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void sh_editor_transition_frame_leave(void *editor)
{ if (g_et_frame == editor) g_et_frame = NULL; }
int sh_editor_transition_frame_ready(void *editor, void *previous_map)
{ return previous_map && g_et_frame == editor && sh_editor_transition_capture(editor) == previous_map; }
static int et_empty(void)
{
    const unsigned char *ed = g_et.editor;
    return ed && !ed[9] && !*(void *const *)(ed + ET_MAP) && !*(void *const *)(ed + ET_EDIT) &&
        !*(void *const *)(ed + ET_UI) && !*(void *const *)(ed + ET_WORLD) && !*(void *const *)(ed + ET_SOUND);
}
int sh_editor_transition_at_boundary(void)
{
    __try {
        return g_et.phase == ET_ACTIVATING && g_et_frame == g_et.editor && et_thread() &&
            g_et.render.active == 2 && sh_render_scope_owner(&g_et.render) &&
            *(const int *)((const unsigned char *)g_et.render.renderer + 0x14) == g_et.render.count + 1 && et_empty();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* SetActive normally destroys these worlds with com_snapMapUseMapHeap=1.
 * Finish the same native cleanup when that allocation mode retains them.
 * Never change the allocation cvar or call the non-idempotent scene clear twice. */
static int et_finish_worlds(void)
{
    unsigned char *ed = g_et.editor;
    static const size_t required[] = {0x30, 0x38, 0x98, 0xa8, 0x2a8};
    if (ed[9] || *(void **)(ed + ET_UI)) return 0;
    if (g_et.heap.active && !sh_process_heap_leave(&g_et.heap)) return 0;
    if (!sh_process_heap_enter(&g_et_api.heap, &g_et.heap)) return 0;
    if (*(void **)(ed + ET_WORLD)) {
        for (size_t i = 0; i < sizeof(required)/sizeof(required[0]); i++)
            if (!*(void **)(ed + ET_SCENE + required[i])) return 0;
        g_et.native_uncertain = 1;
        g_et_api.clear_scene(ed + ET_SCENE);
        g_et.native_uncertain = 0;
        if (*(void **)(ed + ET_WORLD)) return 0;
    }
    if (*(void **)(ed + ET_SOUND)) {
        void *sound = *g_et_api.sound;
        if (!sound) return 0;
        g_et.native_uncertain = 1;
        ((void (*)(void *, void *))(*(void ***)sound)[0x28/8])(sound, *(void **)(ed + ET_SOUND));
        *(void **)(ed + ET_SOUND) = NULL;
        g_et.native_uncertain = 0;
    }
    return sh_process_heap_leave(&g_et.heap);
}
static int et_read_from(const char *id, void **map, void *modules, void *palette,
    void *progress, const void *caller)
{
    unsigned char *ed = g_et.editor;
    int result;
    if (!g_et.phase || caller != g_et_api.read_return)
        return g_et_api.read(id, map, modules, palette, progress);
    if (g_et.phase != ET_LOADING || g_et.read || !et_thread() || g_et_frame != ed ||
        !id || strcmp(id, g_et.id) || map != (void **)(ed + ET_MAP) ||
        modules != ed + 0x206c0 || palette != ed + 0x20660 || progress != ed + 0x236d8 ||
        *map || *(void **)(ed + ET_EDIT) || !ed[9]) {
        et_error("The native editor reload no longer matches its prepared map."); return 1;
    }
    g_et.read = 1;
    g_et.native_uncertain = 1;
    g_et_api.set_active(ed, 0);
    g_et.native_uncertain = 0;
    if (!et_finish_worlds() || !et_empty()) {
        et_error("The previous editor resources could not finish unloading."); return 1;
    }
    g_et.phase = ET_ACTIVATING;
    if (!g_et.callbacks.activate(g_et.callbacks.context)) {
        et_error("The prepared map resources could not activate."); return 1;
    }
    g_et.activated = 1; g_et.phase = ET_READING;
    g_et.native_uncertain = 1;
    g_et_api.set_active(ed, 1);
    g_et.native_uncertain = 0;
    if (!ed[9] || !*(void **)(ed + ET_UI) || !*(void **)(ed + ET_WORLD) ||
        !*(void **)(ed + ET_SOUND) || *map || *(void **)(ed + ET_EDIT)) {
        et_error("The editor could not initialize the prepared map view."); return 1;
    }
    result = g_et_api.read(id, map, modules, palette, progress);
    g_et.phase = ET_FINISHING;
    return result;
}
static int et_read(const char *id, void **map, void *modules, void *palette, void *progress)
{ return et_read_from(id, map, modules, palette, progress, _ReturnAddress()); }
static int et_cancel_view(void)
{
    if (!g_et.read || g_et.native_uncertain) return !g_et.native_uncertain;
    if (!g_et.render.active &&
        !sh_render_scope_enter(g_et_api.renderer, g_et_api.thread, &g_et.render)) return 0;
    if (g_et.render.active != 2 || !sh_render_scope_owner(&g_et.render) ||
        *(int *)((unsigned char *)g_et.render.renderer + 0x14) != g_et.render.count + 1) return 0;
    if (((unsigned char *)g_et.editor)[9])
        *(int *)((unsigned char *)g_et.editor + 0x2366c) = 1;
    g_et.native_uncertain = 1;
    g_et_api.set_active(g_et.editor, 0);
    g_et.native_uncertain = 0;
    return et_finish_worlds();
}
int sh_editor_transition_install(const sig_result *results, size_t count, const uint8_t *base)
{
    void *trampoline;
    if (g_et_api.read) {
        if (g_et_api.installed && hook_is_installed((void *)g_et_api.read)) return 1;
        if (!hook_unpatch((void *)g_et_api.read)) return 0;
        g_et_api.read = NULL; g_et_api.installed = 0;
    }
    if (!sh_editor_transition_bind(results, count, base)) return 0;
    trampoline = hook_prepare((void *)g_et_api.read_site, et_read, 25);
    if (!trampoline) return 0;
    g_et_api.read = (et_read_fn)trampoline;
    if (hook_commit(trampoline) != B2_PATCH_OK) {
        if (hook_unpatch(trampoline)) g_et_api.read = NULL;
        return 0;
    }
    g_et_api.installed = 1;
    backend_log("MPKG: native editor saved-read handoff installed"); return 1;
}
int sh_editor_transition_load(void *editor, void *previous_map, const void *name,
    sh_editor_transition_load_fn load, const sh_editor_transition_callbacks *callbacks,
    char *error, size_t capacity)
{
    int result = 1, finished = 0, candidate = 0, clean;
    const char *id;
    if (g_et_frame != editor || !previous_map || sh_editor_transition_capture(editor) != previous_map ||
        !name || !load || !callbacks || !callbacks->activate || !callbacks->commit) {
        if (error && capacity && !*error) snprintf(error, capacity, "The original editor load is no longer available.");
        return 1;
    }
    memset(&g_et, 0, sizeof(g_et));
    g_et.editor = editor; g_et.callbacks = *callbacks; g_et.phase = ET_LOADING;
    __try {
        id = *(const char *const *)((const unsigned char *)name + 0x10);
        if (!id || strnlen(id, sizeof(g_et.id)) != 20) { et_error("The native saved-map identity is invalid."); goto done; }
        memcpy(g_et.id, id, sizeof(g_et.id));
        if (!sh_render_scope_enter(g_et_api.renderer, g_et_api.thread, &g_et.render)) {
            et_error("The renderer could not admit the editor reload."); goto done;
        }
        result = load(editor, name);
        if (!result && !g_et.error[0] && g_et.read && g_et.activated &&
            g_et.phase == ET_FINISHING) {
            if (!sh_render_scope_update(&g_et.render)) et_error("The renderer could not finish the editor reload.");
            else candidate = 1;
        }
        if (!candidate && !g_et.error[0]) et_error("The prepared editor map did not finish loading.");
        if (!candidate && !et_cancel_view()) et_error("The failed editor load still owns native resources.");
        clean = et_release_scopes();
        if (!clean) { et_error("Editor reload scope restoration did not complete."); candidate = 0; }
        if (candidate) {
            /* Installation commit is last, after native load, consumer update
             * and verified scope restoration. A rejected commit tears down the
             * completed view under a fresh renderer hold. */
            if (g_et.callbacks.commit(g_et.callbacks.context)) finished = 1;
            else {
                et_error("The prepared map load could not commit.");
                if (!et_cancel_view()) et_error("The uncommitted editor view still owns native resources.");
            }
        }
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(g_et.error, sizeof(g_et.error), "Native editor reload raised exception %08lx.", GetExceptionCode());
        if (g_et.native_uncertain) g_et_api.faulted = 1;
        finished = 0;
    }
    if (!finished && g_et.read && !g_et.native_uncertain) {
        __try {
            if (!et_cancel_view()) et_error("The canceled editor view still owns native resources.");
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_et_api.faulted = 1; }
    }
    clean = et_release_scopes();
    if (!clean) { et_error("Editor reload scope restoration did not complete."); finished = 0; }
    if (error && capacity && !*error && g_et.error[0]) snprintf(error, capacity, "%s", g_et.error);
    if (g_et.render.active || g_et.heap.active) {
        g_et.phase = ET_RECOVERY;
        memset(&g_et.callbacks, 0, sizeof(g_et.callbacks));
    } else memset(&g_et, 0, sizeof(g_et));
    return finished ? 0 : result ? result : 1;
}
