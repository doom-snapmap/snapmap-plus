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
/* What a launch gate decided: refused with a notice, retained for the package
 * route, or handed to the untouched native launch. */
enum { PUB_REFUSED, PUB_RETAINED, PUB_NATIVE };
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
/* Virtual slots the cache-ready routine itself calls; bound from its code
 * because build 13954591 moved two of them (session -> lobby +0x338 to +0x348,
 * lobby -> launch parameters +0x2B0 to +0x2B8). Offsets into
 * PublishedMapCacheReady, equal on both renderers: */
#define PUB_READY_PROGRESS_CALL  0x49   /* FF 50 30: progress widget hide, +0x30 */
#define PUB_READY_LOBBY_CALL     0x56   /* FF 90 disp32: session -> lobby */
#define PUB_READY_PARAMS_CALL    0x62   /* FF 92 disp32: lobby -> launch parameters */
#define PUB_READY_CACHE_GET_CALL 0xaa   /* 41 FF 51 38: system -> map cache, +0x38 */
#define PUB_READY_CACHE_SET_CALL 0xba   /* 41 FF 91 disp32: cache set, +0x190 */
static size_t g_pub_slot_progress, g_pub_slot_lobby, g_pub_slot_parameters,
              g_pub_slot_cache_get, g_pub_slot_cache_set;
static __declspec(thread) int g_pub_delegating;
static unsigned long long g_pub_sequence;

typedef struct pub_request {
    struct pub_request *next;
    sh_map_native_selection *selection;
    void *manager, *history;
    unsigned long long sequence;
    unsigned char option;
    int kind, launched, departed_browser;
    int selected;           /* activation replaced the cache with the full map */
    const char *reject;     /* why the latest validity check failed (static text) */
} pub_request;
static pub_request *g_pub_retired;
static __declspec(thread) pub_request *g_pub_entering;

/* A launched request lent the native cache a lobby-only snapshot (no entities).
 * If it is dropped before activation, remember the map so the transition can
 * cancel that load rather than build the map from the projection. */
static char g_pub_orphan[256];
static char g_pub_orphan_reason[160];
static ULONGLONG g_pub_orphan_at;
#define PUB_ORPHAN_MS 600000ull
static int pub_reject(pub_request *request, const char *reason)
{
    if (request) request->reject = reason;
    return 0;
}

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
/* The slot index of `call qword ptr [reg+disp]` (optional REX.B, FF /2 with
 * an 8- or 32-bit displacement and no SIB), or 0 when the bytes differ. */
static size_t pub_vcall_slot(const uint8_t *p)
{
    int32_t displacement;
    uint8_t modrm;
    if (p[0] == 0x41) p++;
    if (p[0] != 0xff) return 0;
    modrm = p[1];
    if (((modrm >> 3) & 7) != 2 || (modrm & 7) == 4) return 0;
    if ((modrm >> 6) == 1) displacement = (int8_t)p[2];
    else if ((modrm >> 6) == 2) memcpy(&displacement, p + 2, 4);
    else return 0;
    if (displacement <= 0 || displacement >= 0x800 || displacement % 8) return 0;
    return (size_t)displacement / sizeof(void *);
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
    size_t progress_slot, lobby_slot, parameters_slot, cache_get_slot, cache_set_slot;
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
        progress_slot = pub_vcall_slot(sites[PUB_READY] + PUB_READY_PROGRESS_CALL);
        lobby_slot = pub_vcall_slot(sites[PUB_READY] + PUB_READY_LOBBY_CALL);
        parameters_slot = pub_vcall_slot(sites[PUB_READY] + PUB_READY_PARAMS_CALL);
        cache_get_slot = pub_vcall_slot(sites[PUB_READY] + PUB_READY_CACHE_GET_CALL);
        cache_set_slot = pub_vcall_slot(sites[PUB_READY] + PUB_READY_CACHE_SET_CALL);
        if (!progress_slot || !lobby_slot || !parameters_slot || !cache_get_slot || !cache_set_slot)
            return 0;
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
    g_pub_slot_progress = progress_slot; g_pub_slot_lobby = lobby_slot;
    g_pub_slot_parameters = parameters_slot; g_pub_slot_cache_get = cache_get_slot;
    g_pub_slot_cache_set = cache_set_slot;
    g_pub_read_return = sites[PUB_READ] + 0x12a;
    g_pub_profile_return = profile + 0x5d;
    return 1;
}

static void *pub_cache(void)
{
    void *system = g_pub_system();
    void *cache = system ? ((void *(*)(void *))(*(void ***)system)[g_pub_slot_cache_get])(system) : NULL;
    if (!cache || (*(void ***)cache)[g_pub_slot_cache_set] != (void *)g_pub_cache_set) return NULL;
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
    lobby = ((void *(*)(void *))(*(void ***)session)[g_pub_slot_lobby])(session);
    if (!lobby) return 0;
    parameters = ((void *(*)(void *))(*(void ***)lobby)[g_pub_slot_parameters])(lobby);
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
        if (!g_pub_ready || !g_pub_thread || *g_pub_thread != GetCurrentThreadId())
            return pub_reject(request, "not on the engine thread");
        if (request->sequence != g_pub_sequence) return pub_reject(request, "a newer published launch replaced it");
        if (!(shell = *g_pub_shell_slot) || *(void **)((unsigned char *)shell + 0x18) != request->manager)
            return pub_reject(request, "the shell menu manager changed");
        screen = *(int *)((unsigned char *)request->manager + 8);
        next = *(int *)((unsigned char *)request->manager + 12);
        state = *(int *)((unsigned char *)request->manager + 0x910);
        if (screen < 46 || screen > 65 || next < 46 || next > 65)
            return pub_reject(request, "the shell left the SnapMap screens");
        if (state == 9 || state == 10) return pub_reject(request, "the shell is paused or in game");
        if (request->kind == PUB_READY)
            return pub_host_matches(request->selection) ? 1 : pub_reject(request, "the lobby host map differs");
        if (request->kind == PUB_LOBBY && pub_history() != request->history)
            return pub_reject(request, "the published-map history changed");
        cache = pub_cache();
        if (!cache) return pub_reject(request, "the native published-map cache is unavailable");
        return sh_map_native_matches_ids(request->selection,
            (unsigned char *)cache + 0x24408, (unsigned char *)cache + 0x24438)
            ? 1 : pub_reject(request, "the native cache holds a different map");
    } __except (EXCEPTION_EXECUTE_HANDLER) { return pub_reject(request, "the validity check faulted"); }
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
        if (!request->launched) return pub_reject(request, "it was never launched");
        if (!g_pub_ready || request->sequence != g_pub_sequence)
            return pub_reject(request, "a newer published launch replaced it");
        if (!pub_manager_current(request->manager)) return pub_reject(request, "the shell menu manager changed");
        if (!(cache = pub_cache())) return pub_reject(request, "the native published-map cache is unavailable");
        if (!sh_map_native_matches_ids(request->selection, (unsigned char *)cache + 0x24408,
                (unsigned char *)cache + 0x24438)) return pub_reject(request, "the native cache holds a different map");
        if (g_pub_entering == request) return 1;
        browser = sh_decl_server_map_browser_present();
        if (!browser) request->departed_browser = 1;
        else if (request->departed_browser) return pub_reject(request, "the player returned to the map browser");
        /* Lobby identity continues to own the request when browser screens
         * close during the native transition into LOADING. */
        if (pub_host_matches(request->selection)) return 1;
        if (request->kind == PUB_READY) return pub_reject(request, "the lobby host map differs");
        return pub_valid(request);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return pub_reject(request, "the wait check faulted"); }
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
    if (ok) request->selected = 1;
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
    if (request->launched && !request->selected &&
        sh_map_native_level_name(request->selection, g_pub_orphan, sizeof(g_pub_orphan))) {
        char line[512];
        strcpy_s(g_pub_orphan_reason, sizeof(g_pub_orphan_reason),
                 request->reject ? request->reject : "its preparation was canceled");
        g_pub_orphan_at = GetTickCount64();
        snprintf(line, sizeof(line), "MPKG: published map '%s' lost its prepared launch before loading: %s",
                 g_pub_orphan, g_pub_orphan_reason);
        backend_log(line);
    }
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

/* A published map whose text carries no package is the engine's own case: it
 * never enters the package context, the retained request or the transition
 * handoff, and its native launch runs exactly as it did before these gates
 * existed. Only a map that carries smpkg shards takes the retained route; a
 * text that cannot be read keeps the guarded route and its notices. */
static int pub_prepare(int kind, void *manager, const void *metadata, void *snapshot, unsigned char option,
    int release_progress)
{
    pub_request *request = NULL;
    char error[1024] = "";
    int accepted = PUB_REFUSED;
    __try {
        const char *json;
        size_t length;
        void *cache;
        sh_rawmap_request launch;
        sh_rawmap_loading loading = {pub_waiting, pub_matches_level, pub_select};
        if (!pub_manager_current(manager)) goto done;
        g_pub_orphan[0] = 0;   /* a new launch supersedes any dropped one */
        request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request));
        if (!request) { strcpy_s(error, sizeof(error), "Published-map request allocation failed."); goto done; }
        request->kind = kind; request->manager = manager; request->option = option;
        request->history = kind == PUB_LOBBY ? pub_history() : NULL;
        request->selection = sh_map_native_capture(metadata, error, sizeof(error));
        if (!request->selection) goto done;
        json = sh_map_native_source(request->selection, &length);
        if (json && length && sh_mpkg_scan(json, length, NULL, 0) == 0) {
            /* Nothing of ours may outlive this launch; a retained request that
             * cannot be canceled is left to its own validity checks. */
            if (!sh_rawmap_cancel_pending_map())
                backend_log("MPKG: a previous retained map request could not be canceled before a native published launch");
            accepted = PUB_NATIVE; goto done;
        }
        if (release_progress) {
            void *progress = *(void **)((unsigned char *)manager + 0xa38);
            if (progress) ((void (*)(void *, int))(*(void ***)progress)[g_pub_slot_progress])(progress, 0);
        }
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
        launch.context = request; launch.valid = pub_valid; launch.enter = pub_enter; launch.release = pub_release;
        if (sh_rawmap_preflight_loading_request(json, length, &launch, &loading, error, sizeof(error)))
            accepted = PUB_RETAINED;
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, sizeof(error), "Published-map preparation raised exception %08lx.", GetExceptionCode());
    }
    if (accepted != PUB_RETAINED && request) pub_release(request);
    if (accepted == PUB_REFUSED && error[0]) sh_mpkg_report_error(error);
    return accepted;
}

static void pub_offline(void *manager, const void *metadata, void *snapshot)
{
    if (!g_pub_ready || pub_same_launch(manager, metadata) ||
        pub_prepare(PUB_OFFLINE, manager, metadata, snapshot, 0, 0) == PUB_NATIVE)
        ((pub_launch_fn)g_pub_original[PUB_OFFLINE])(manager, metadata, snapshot);
}
static unsigned char pub_lobby(void *manager, unsigned char option)
{
    if (!g_pub_ready) return ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(manager, option);
    __try {
        void *history = pub_history();
        if (history && pub_same_launch(manager, (unsigned char *)history + 8))
            return ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(manager, option);
        if (history) {
            int decided = pub_prepare(PUB_LOBBY, manager, (unsigned char *)history + 8, NULL, option, 0);
            if (decided == PUB_NATIVE) return ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(manager, option);
            return decided == PUB_RETAINED;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    sh_mpkg_report_error("The selected published map is no longer available."); return 0;
}
static void pub_direct(void *manager, const void *metadata, void *snapshot)
{
    if (!g_pub_ready || pub_same_launch(manager, metadata) ||
        pub_prepare(PUB_DIRECT, manager, metadata, snapshot, 0, 0) == PUB_NATIVE)
        ((pub_launch_fn)g_pub_original[PUB_DIRECT])(manager, metadata, snapshot);
}
static void pub_ready(void *manager, void *download, const void *metadata, void *snapshot)
{
    if (!g_pub_ready || pub_same_launch(manager, metadata)) { ((pub_ready_fn)g_pub_original[PUB_READY])(manager, download, metadata, snapshot); return; }
    if (!pub_manager_current(manager)) return;
    /* The retained route releases the native progress display itself; the
     * native launch keeps it and its own download argument. */
    if (pub_prepare(PUB_READY, manager, metadata, snapshot, 0, 1) == PUB_NATIVE)
        ((pub_ready_fn)g_pub_original[PUB_READY])(manager, download, metadata, snapshot);
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

/* One-shot: does this native load name the published map whose prepared launch
 * was dropped after it gave the cache a lobby-only snapshot? */
static int pub_orphan_check(const void *parameters, char *reason, size_t capacity)
{
    if (!g_pub_orphan[0]) return 0;
    if (GetTickCount64() - g_pub_orphan_at > PUB_ORPHAN_MS) { g_pub_orphan[0] = 0; return 0; }
    if (!sh_map_native_level_named(parameters, g_pub_orphan)) return 0;
    if (reason && capacity)
        snprintf(reason, capacity, "'%s': %s", g_pub_orphan, g_pub_orphan_reason);
    g_pub_orphan[0] = 0;
    return 1;
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
    sh_rawmap_set_orphan_check(pub_orphan_check);
    backend_log("MPKG: published reads inspect without activation; offline, lobby, direct and join readiness retain their launch sources");
    return 1;
failed:
    for (size_t i = PUB_HOOKS; i > 0; i--) if (g_pub_original[i - 1] && hook_unpatch(g_pub_original[i - 1]))
        g_pub_original[i - 1] = NULL;
    backend_log("MPKG: published-map hook group could not be installed; surviving gates remain pass-through");
    return 0;
}
