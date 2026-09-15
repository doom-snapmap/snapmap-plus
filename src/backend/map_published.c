#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "map_published.h"
#include "map_native.h"
#include "map_render.h"
#include "rawmap.h"
#include "map_package.h"
#include "decl_server.h"
#include "engine_globals.h"
#include "backend_log.h"
#include "hook.h"

enum { PUB_OFFLINE, PUB_LOBBY, PUB_DIRECT, PUB_READY, PUB_READ, PUB_COMPLETE, PUB_HOOKS };
typedef void (*pub_launch_fn)(void *, const void *, void *);
typedef unsigned char (*pub_lobby_fn)(void *, unsigned char);
typedef void (*pub_ready_fn)(void *, void *, const void *, void *);
typedef void *(*pub_read_fn)(void *, const char *, const char *, void *, void *);
typedef void (*pub_complete_fn)(void *);
typedef void *(*pub_get_fn)(void);
typedef void (*pub_cache_fn)(void *, const void *, void *);
static void *g_pub_sites[PUB_HOOKS], *g_pub_original[PUB_HOOKS];
static pub_get_fn g_pub_system;
static pub_cache_fn g_pub_cache_set;
static void **g_pub_shell_slot, **g_pub_session_slot, **g_pub_history_slot;
static int *g_pub_history_count;
static DWORD *g_pub_thread;
static const void *g_pub_read_return, *g_pub_profile_return;
static volatile LONG g_pub_ready;
static __declspec(thread) int g_pub_delegating;
static unsigned long long g_pub_sequence;

typedef struct pub_request {
    struct pub_request *next;
    sh_map_native_selection *selection;
    void *manager, *history;
    unsigned long long sequence;
    unsigned char option;
    int kind, launched, departed_browser;
} pub_request;
static pub_request *g_pub_retired;
static __declspec(thread) pub_request *g_pub_entering;

static const uint8_t *pub_site(const sig_result *results, size_t count, const uint8_t *base, const char *name)
{
    const uint8_t *site = NULL;
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name)) {
        if (site || results[i].status != SIG_OK || !results[i].addr ||
            results[i].addr != (uintptr_t)base + results[i].rva) return NULL;
        site = (const uint8_t *)results[i].addr;
    }
    return site;
}
static void *pub_call(const uint8_t *p)
{
    int32_t displacement;
    if (p[0] != 0xe8) return NULL;
    memcpy(&displacement, p + 1, 4); return (void *)(p + 5 + displacement);
}
static void *pub_slot(const uint8_t *p, const char *opcode, size_t opcode_length)
{
    int32_t displacement;
    if (memcmp(p, opcode, opcode_length)) return NULL;
    memcpy(&displacement, p + opcode_length, 4);
    return (void *)(p + opcode_length + 4 + displacement);
}

int sh_map_published_bind(const sig_result *results, size_t count, const uint8_t *base)
{
    static const char *names[PUB_HOOKS] = {"PublishedMapOfflineLaunch", "PublishedMapLobbyLaunch",
        "PublishedMapDirectLaunch", "PublishedMapCacheReady", "PublishedMapRead", "PublishedMapComplete"};
    const uint8_t *sites[PUB_HOOKS], *cache, *parse, *profile;
    void *system, *history, *history_count, *session;
    uintptr_t shell, thread;
    if (!results || !base || g_pub_ready) return 0;
    for (size_t i = 0; i < PUB_HOOKS; i++) if (!(sites[i] = pub_site(results, count, base, names[i]))) return 0;
    cache = pub_site(results, count, base, "PublishedMapCacheSet");
    parse = pub_site(results, count, base, "DeserializeFromJson");
    profile = pub_site(results, count, base, "SnapMapReadSizeLimit");
    if (!cache || !parse || !profile) return 0;
    __try {
        system = pub_call(sites[PUB_OFFLINE] + 0x34);
        if (!system || pub_call(sites[PUB_READ] + 0x125) != parse ||
            pub_call(profile + 0x58) != parse || pub_call(sites[PUB_LOBBY] + 0x87) != system ||
            pub_call(sites[PUB_READY] + 0x9f) != system) return 0;
        history_count = pub_slot(sites[PUB_LOBBY] + 0x58, "\x8b\x0d", 2);
        history = pub_slot(sites[PUB_LOBBY] + 0x77, "\x4c\x03\x35", 3);
        session = pub_slot(sites[PUB_READY] + 0x4c, "\x48\x8b\x0d", 3);
        if (!history_count || !history || !session ||
            memcmp(cache + 0x54, "\x48\x8d\x8e\x08\x44\x02\x00", 7) ||
            memcmp(cache + 0x67, "\x48\x8d\x8e\x38\x44\x02\x00", 7)) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    shell = glb_resolve(base, "shell_ptr_slot", NULL);
    thread = glb_resolve(base, "main_thread_id", NULL);
    if (!shell || !thread) return 0;
    for (size_t i = 0; i < PUB_HOOKS; i++) g_pub_sites[i] = (void *)sites[i];
    g_pub_system = (pub_get_fn)system; g_pub_cache_set = (pub_cache_fn)cache;
    g_pub_history_slot = history; g_pub_history_count = history_count;
    g_pub_session_slot = session; g_pub_shell_slot = (void **)shell; g_pub_thread = (DWORD *)thread;
    g_pub_read_return = sites[PUB_READ] + 0x12a;
    g_pub_profile_return = profile + 0x5d;
    return 1;
}

static void *pub_cache(void)
{
    void *system = g_pub_system();
    void *cache = system ? ((void *(*)(void *))(*(void ***)system)[0x38 / 8])(system) : NULL;
    if (!cache || (*(void ***)cache)[0x190 / 8] != (void *)g_pub_cache_set) return NULL;
    return cache;
}
static void *pub_history(void)
{
    int count = *g_pub_history_count;
    unsigned char *records = *g_pub_history_slot;
    if (count <= 0 || !records) return NULL;
    return *(void **)(records + (size_t)(count - 1) * 0x38 + 0x18);
}
static int pub_host_matches(const sh_map_native_selection *selection)
{
    void *session = *g_pub_session_slot, *lobby, *parameters;
    if (!session) return 0;
    lobby = ((void *(*)(void *))(*(void ***)session)[0x338 / 8])(session);
    if (!lobby) return 0;
    parameters = ((void *(*)(void *))(*(void ***)lobby)[0x2b0 / 8])(lobby);
    return parameters && sh_map_native_matches_ids(selection,
        (unsigned char *)parameters + 0x550, (unsigned char *)parameters + 0x600);
}
static int pub_manager_current(const void *manager)
{
    __try {
        return manager && g_pub_thread && *g_pub_thread == GetCurrentThreadId() &&
            g_pub_shell_slot && *g_pub_shell_slot &&
            *(void **)((unsigned char *)*g_pub_shell_slot + 0x18) == manager;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int pub_valid(void *context)
{
    pub_request *request = context;
    void *shell, *cache;
    int screen, next, state;
    __try {
        if (!g_pub_ready || !g_pub_thread || *g_pub_thread != GetCurrentThreadId() ||
            request->sequence != g_pub_sequence || !(shell = *g_pub_shell_slot) ||
            *(void **)((unsigned char *)shell + 0x18) != request->manager) return 0;
        screen = *(int *)((unsigned char *)request->manager + 8);
        next = *(int *)((unsigned char *)request->manager + 12);
        state = *(int *)((unsigned char *)request->manager + 0x910);
        if (screen < 46 || screen > 65 || next < 46 || next > 65 || state == 9 || state == 10) return 0;
        if (request->kind == PUB_READY) return pub_host_matches(request->selection);
        if (request->kind == PUB_LOBBY && pub_history() != request->history) return 0;
        cache = pub_cache();
        return cache && sh_map_native_matches_ids(request->selection,
            (unsigned char *)cache + 0x24408, (unsigned char *)cache + 0x24438);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int pub_same_launch(void *manager, const void *metadata)
{
    return g_pub_entering && g_pub_entering->manager == manager &&
        sh_map_native_matches(g_pub_entering->selection, metadata);
}
static int pub_waiting(void *context)
{
    pub_request *request = context;
    __try {
        void *cache;
        int browser;
        if (!request->launched || !g_pub_ready || request->sequence != g_pub_sequence ||
            !pub_manager_current(request->manager) || !(cache = pub_cache()) ||
            !sh_map_native_matches_ids(request->selection, (unsigned char *)cache + 0x24408,
                (unsigned char *)cache + 0x24438)) return 0;
        if (g_pub_entering == request) return 1;
        browser = sh_decl_server_map_browser_present();
        if (!browser) request->departed_browser = 1;
        else if (request->departed_browser) return 0;
        /* Lobby identity continues to own the request when browser screens
         * close during the native transition into LOADING. */
        return pub_host_matches(request->selection) ||
            (request->kind != PUB_READY && pub_valid(request));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int pub_matches_level(void *context, const void *parameters)
{
    pub_request *request = context;
    return request->launched && sh_map_native_matches_level(request->selection, parameters);
}
static int pub_select_decoded(void *context, const void *metadata, void *snapshot)
{
    pub_request *request = context;
    sh_map_render settings;
    void *cache;
    if (!pub_waiting(request) || !(cache = pub_cache()) || !sh_map_render_capture(snapshot, &settings)) return 0;
    g_pub_cache_set(cache, metadata, snapshot);
    sh_map_render_select(&settings);
    return 1;
}
static int pub_select(void *context)
{
    pub_request *request = context;
    char error[512];
    int ok;
    sh_rawmap_inspection_enter();
    __try { ok = sh_map_native_enter(request->selection, sh_map_native_source(request->selection, NULL),
        pub_select_decoded, request, error, sizeof(error)); }
    __finally { sh_rawmap_inspection_leave(); }
    if (!ok && error[0]) backend_log(error);
    return ok;
}
static int pub_enter_native(void *context, const void *metadata, void *snapshot)
{
    pub_request *request = context;
    pub_request *previous = g_pub_entering;
    int ok = 1;
    if (!pub_valid(request)) return 0;
    request->launched = 1;
    g_pub_entering = request;
    g_pub_delegating++;
    __try {
        if (request->kind == PUB_LOBBY) {
            void *cache = pub_cache();
            if (!cache || !sh_map_native_matches(request->selection, (unsigned char *)pub_history() + 8)) ok = 0;
            else {
                g_pub_cache_set(cache, metadata, snapshot);
                ok = ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(request->manager, request->option) != 0;
            }
        } else if (request->kind == PUB_READY) {
            /* This callback never reads its consumed download-request argument.
             * It checks the host selection again before publishing readiness. */
            ((pub_ready_fn)g_pub_original[PUB_READY])(request->manager, NULL, metadata, snapshot);
        } else ((pub_launch_fn)g_pub_original[request->kind])(request->manager, metadata, snapshot);
    } __finally { g_pub_delegating--; g_pub_entering = previous; }
    return ok;
}
static int pub_enter(void *context, const char *json)
{
    pub_request *request = context;
    char error[512];
    int ok;
    (void)json;
    sh_rawmap_inspection_enter();
    __try { ok = sh_map_native_session(request->selection, pub_enter_native, request, error, sizeof(error)); }
    __finally { sh_rawmap_inspection_leave(); }
    if (ok) return 0;
    if (error[0]) backend_log(error);
    return 1;
}
static void pub_release(void *context)
{
    pub_request *request = context;
    if (!sh_map_native_release(&request->selection)) {
        backend_log("MPKG: published-map metadata cleanup did not complete");
        if (request->selection) {
            request->next = g_pub_retired; g_pub_retired = request; return;
        }
    }
    HeapFree(GetProcessHeap(), 0, request);
}
void sh_map_published_poll(void)
{
    pub_request **link = &g_pub_retired;
    if (!g_pub_thread || *g_pub_thread != GetCurrentThreadId()) return;
    while (*link) {
        pub_request *request = *link;
        int ok = sh_map_native_release(&request->selection);
        if (!ok && request->selection) { link = &request->next; continue; }
        *link = request->next; HeapFree(GetProcessHeap(), 0, request);
    }
}

static int pub_prepare(int kind, void *manager, const void *metadata, void *snapshot, unsigned char option)
{
    pub_request *request = NULL;
    char error[1024] = "";
    int accepted = 0;
    __try {
        const char *json;
        size_t length;
        void *cache;
        sh_rawmap_request launch;
        sh_rawmap_loading loading = {pub_waiting, pub_matches_level, pub_select};
        if (!pub_manager_current(manager)) goto done;
        request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request));
        if (!request) { strcpy_s(error, sizeof(error), "Published-map request allocation failed."); goto done; }
        request->kind = kind; request->manager = manager; request->option = option;
        request->history = kind == PUB_LOBBY ? pub_history() : NULL;
        request->selection = sh_map_native_capture(metadata, error, sizeof(error));
        if (!request->selection) goto done;
        if (kind == PUB_READY && !pub_host_matches(request->selection)) goto done;
        if (!sh_rawmap_cancel_pending_map()) {
            strcpy_s(error, sizeof(error), "The previous map installation could not be canceled."); goto done;
        }
        if (!sh_decl_server_map_preparation_ready()) {
            strcpy_s(error, sizeof(error), "The package compiler is not ready to prepare this map."); goto done;
        }
        cache = pub_cache();
        if (!cache) { strcpy_s(error, sizeof(error), "The native published-map cache is unavailable."); goto done; }
        if (kind != PUB_LOBBY && kind != PUB_READY) g_pub_cache_set(cache, metadata, snapshot);
        request->sequence = ++g_pub_sequence;
        json = sh_map_native_source(request->selection, &length);
        launch.context = request; launch.valid = pub_valid; launch.enter = pub_enter; launch.release = pub_release;
        accepted = sh_rawmap_preflight_loading_request(json, length, &launch, &loading, error, sizeof(error));
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, sizeof(error), "Published-map preparation raised exception %08lx.", GetExceptionCode());
    }
    if (!accepted && request) pub_release(request);
    if (!accepted && error[0]) sh_mpkg_report_error(error);
    return accepted;
}

static void pub_offline(void *manager, const void *metadata, void *snapshot)
{
    if (!g_pub_ready || pub_same_launch(manager, metadata)) { ((pub_launch_fn)g_pub_original[PUB_OFFLINE])(manager, metadata, snapshot); return; }
    (void)pub_prepare(PUB_OFFLINE, manager, metadata, snapshot, 0);
}
static unsigned char pub_lobby(void *manager, unsigned char option)
{
    if (!g_pub_ready) return ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(manager, option);
    __try {
        void *history = pub_history();
        if (history && pub_same_launch(manager, (unsigned char *)history + 8))
            return ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(manager, option);
        if (history) return (unsigned char)pub_prepare(PUB_LOBBY, manager, (unsigned char *)history + 8, NULL, option);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    sh_mpkg_report_error("The selected published map is no longer available."); return 0;
}
static void pub_direct(void *manager, const void *metadata, void *snapshot)
{
    if (!g_pub_ready || pub_same_launch(manager, metadata)) { ((pub_launch_fn)g_pub_original[PUB_DIRECT])(manager, metadata, snapshot); return; }
    (void)pub_prepare(PUB_DIRECT, manager, metadata, snapshot, 0);
}
static void pub_ready(void *manager, void *download, const void *metadata, void *snapshot)
{
    if (!g_pub_ready || pub_same_launch(manager, metadata)) { ((pub_ready_fn)g_pub_original[PUB_READY])(manager, download, metadata, snapshot); return; }
    if (!pub_manager_current(manager)) return;
    __try {
        void *progress = *(void **)((unsigned char *)manager + 0xa38);
        if (progress) ((void (*)(void *, int))(*(void ***)progress)[0x30 / 8])(progress, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sh_mpkg_report_error("The published-map progress display could not be released."); return;
    }
    (void)pub_prepare(PUB_READY, manager, metadata, snapshot, 0);
}
static void *pub_read(void *client, const char *id, const char *revision, void *success, void *failure)
{
    void *result;
    sh_rawmap_read_scope scope;
    if (!g_pub_ready) return ((pub_read_fn)g_pub_original[PUB_READ])(client, id, revision, success, failure);
    sh_rawmap_read_enter(&scope, g_pub_thread && *g_pub_thread == GetCurrentThreadId() ? g_pub_read_return : NULL);
    __try { result = ((pub_read_fn)g_pub_original[PUB_READ])(client, id, revision, success, failure); }
    __finally { sh_rawmap_read_leave(&scope); }
    return result;
}
static void pub_complete(void *download)
{
    sh_rawmap_read_scope scope;
    if (!g_pub_ready) { ((pub_complete_fn)g_pub_original[PUB_COMPLETE])(download); return; }
    sh_rawmap_read_enter(&scope, g_pub_thread && *g_pub_thread == GetCurrentThreadId() ? g_pub_profile_return : NULL);
    __try { ((pub_complete_fn)g_pub_original[PUB_COMPLETE])(download); }
    __finally { sh_rawmap_read_leave(&scope); }
}

int sh_map_published_install(const sig_result *results, size_t count, const uint8_t *base)
{
    static const size_t stolen[PUB_HOOKS] = {17, 18, 18, 15, 21, 20};
    void *detours[PUB_HOOKS] = {pub_offline, pub_lobby, pub_direct, pub_ready, pub_read, pub_complete};
    if (g_pub_ready) return 1;
    for (size_t i = 0; i < PUB_HOOKS; i++) if (g_pub_original[i]) {
        if (!hook_unpatch(g_pub_original[i])) return 0;
        g_pub_original[i] = NULL;
    }
    if (!sh_map_published_bind(results, count, base)) return 0;
    for (size_t i = 0; i < PUB_HOOKS; i++) {
        g_pub_original[i] = i == PUB_READY ? hook_prepare_relative_call(g_pub_sites[i], detours[i], stolen[i], 7) :
            hook_prepare(g_pub_sites[i], detours[i], stolen[i]);
        if (!g_pub_original[i]) goto failed;
    }
    for (size_t i = 0; i < PUB_HOOKS; i++) if (hook_commit(g_pub_original[i]) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_pub_ready, 1);
    backend_log("MPKG: published reads inspect without activation; offline, lobby, direct and join readiness retain their launch sources");
    return 1;
failed:
    for (size_t i = PUB_HOOKS; i > 0; i--) if (g_pub_original[i - 1] && hook_unpatch(g_pub_original[i - 1]))
        g_pub_original[i - 1] = NULL;
    backend_log("MPKG: published-map hook group could not be installed; surviving gates remain pass-through");
    return 0;
}
