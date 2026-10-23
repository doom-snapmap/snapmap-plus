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
#include "engine_dialog.h"
#include "package_runtime.h"
#include "engine_globals.h"
#include "backend_log.h"
#include "hook.h"

enum { PUB_OFFLINE, PUB_LOBBY, PUB_DIRECT, PUB_READY, PUB_READ, PUB_COMPLETE, PUB_HOOKS };
/* What a launch gate decided: refused with a notice, retained for the package
 * route, or handed to the untouched native launch. */
enum { PUB_REFUSED, PUB_RETAINED, PUB_NATIVE };
#define MAP_STRING_SIZE_CAP 256
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
/* The published item (id, revision) whose lobby was created at the player's
 * press, until its boundary is handled. A native build of that map with no
 * activation would run against the library palette, where the map's private
 * identities do not exist. */
static char g_pub_armed_id[MAP_STRING_SIZE_CAP], g_pub_armed_revision[MAP_STRING_SIZE_CAP];

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
/* The session's current lobby parameters: the published item they name sits at
 * +0x550 (id) and +0x600 (revision), where the lobby launch wrote it. */
static const unsigned char *pub_session_parameters(void)
{
    void *session, *lobby, *parameters;
    __try {
        session = g_pub_session_slot ? *g_pub_session_slot : NULL;
        if (!session) return NULL;
        lobby = ((void *(*)(void *))(*(void ***)session)[0x338 / 8])(session);
        if (!lobby) return NULL;
        parameters = ((void *(*)(void *))(*(void ***)lobby)[0x2b0 / 8])(lobby);
        return parameters;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static int pub_host_matches(const sh_map_native_selection *selection)
{
    const unsigned char *parameters = pub_session_parameters();
    return parameters && sh_map_native_matches_ids(selection, parameters + 0x550, parameters + 0x600);
}
/* The published item the session's lobby names, when readable. */
static int pub_session_identity(char *id, size_t id_capacity, char *revision, size_t revision_capacity)
{
    const unsigned char *parameters = pub_session_parameters();
    return parameters && sh_map_native_string_copy(parameters + 0x550, id, id_capacity) &&
        sh_map_native_string_copy(parameters + 0x600, revision, revision_capacity);
}
static int pub_manager_current(const void *manager)
{
    __try {
        return manager && g_pub_thread && *g_pub_thread == GetCurrentThreadId() &&
            g_pub_shell_slot && *g_pub_shell_slot &&
            *(void **)((unsigned char *)*g_pub_shell_slot + 0x18) == manager;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Why a retained request no longer stands, or NULL while it does. The reason
 * names the first failed check with the menu values it read, so a refusal in
 * the backend log says which boundary moved. */
static const char *pub_invalid_reason(const pub_request *request)
{
    static char why[160];
    void *shell, *cache;
    int screen, next, state;
    __try {
        if (!g_pub_ready) return "the published launch gates are not installed";
        if (!g_pub_thread || *g_pub_thread != GetCurrentThreadId()) return "not on the engine thread";
        if (request->sequence != g_pub_sequence) return "a newer published request replaced it";
        if (!(shell = *g_pub_shell_slot) || *(void **)((unsigned char *)shell + 0x18) != request->manager)
            return "the menu manager changed";
        screen = *(int *)((unsigned char *)request->manager + 8);
        next = *(int *)((unsigned char *)request->manager + 12);
        state = *(int *)((unsigned char *)request->manager + 0x910);
        if (screen < 46 || screen > 65 || next < 46 || next > 65 || state == 9 || state == 10) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "the menu moved (screen %d, next %d, state %d)", screen, next, state);
            return why;
        }
        if (request->kind == PUB_READY)
            return pub_host_matches(request->selection) ? NULL : "the host's lobby names another map";
        if (request->kind == PUB_LOBBY && pub_history() != request->history) return "the lobby history moved";
        cache = pub_cache();
        if (!cache) return "the published cache is unavailable";
        return sh_map_native_matches_ids(request->selection, (unsigned char *)cache + 0x24408,
            (unsigned char *)cache + 0x24438) ? NULL : "the browser cache names another map";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "reading the menu state faulted"; }
}
static int pub_valid(void *context)
{
    const char *why = pub_invalid_reason(context);
    if (why) {
        char line[240];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: the retained published request no longer stands: %s", why);
        backend_log(line);
    }
    return why == NULL;
}
int sh_map_published_level_armed(const void *level_parameters)
{
    char id[96], revision[96];
    if (!g_pub_armed_id[0]) return 0;
    if (sh_map_native_level_is_snapmap(level_parameters) != 1) return 0;
    if (!pub_session_identity(id, sizeof id, revision, sizeof revision)) return 1;   /* cannot be told apart */
    return !strcmp(id, g_pub_armed_id) && !strcmp(revision, g_pub_armed_revision);
}
void sh_map_published_disarm(void)
{
    g_pub_armed_id[0] = 0; g_pub_armed_revision[0] = 0;
}
/* The engine's own launch returned false. It explains itself with a dialog
 * (sign-in, rate limit) or an engine warning; record which it did. */
static void pub_log_declined(const char *route)
{
    char line[200];
    int idle = 1;
    __try { idle = sh_engine_dialog_queue_idle(g_pub_shell_slot ? *g_pub_shell_slot : NULL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { idle = -1; }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: the engine declined the published %s launch (returned false); %s",
        route, idle == 1 ? "no native dialog is queued" :
        idle == 0 ? "a native dialog is queued with its reason" : "the dialog queue could not be read");
    backend_log(line);
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
/* The spawn info FinalizeMapChange receives names every SnapMap level
 * "DynamicSnapMap" (flag 0x2); which map it is lives in the session's lobby
 * parameters, which the published launch filled with the item's id and
 * revision at the press. A level still spelled "snapmap/<name>" is accepted as
 * well. */
static int pub_matches_level(void *context, const void *parameters)
{
    pub_request *request = context;
    if (!request->launched) return 0;
    if (sh_map_native_level_is_snapmap(parameters) == 1 && pub_host_matches(request->selection)) return 1;
    return sh_map_native_matches_level(request->selection, parameters);
}
/* Positively another level: one that is not a SnapMap level at all, or a
 * SnapMap level whose session item is readable and names another published
 * map. An unreadable level or session identity is not evidence of anything. */
static int pub_differs_level(void *context, const void *parameters)
{
    pub_request *request = context;
    char name[96] = "", id[96] = "", revision[96] = "", line[320];
    int kind = sh_map_native_level_is_snapmap(parameters);
    if (kind == 0) return 1;
    if (kind == 1 && pub_session_identity(id, sizeof id, revision, sizeof revision) &&
        !pub_host_matches(request->selection)) return 1;
    sh_map_native_level_name_of(parameters, name, sizeof name);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MPKG: loading level '%s' (snapmap %d) with session item '%s'/'%s' cannot be told from the retained published map",
        name, kind, id, revision);
    backend_log(line);
    return 0;
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
static int pub_select(void *context, const char *json)
{
    pub_request *request = context;
    char error[512];
    int ok;
    sh_rawmap_inspection_enter();
    __try { ok = sh_map_native_enter(request->selection, json,
        pub_select_decoded, request, error, sizeof(error)); }
    __finally { sh_rawmap_inspection_leave(); }
    if (!ok && error[0]) backend_log(error);
    return ok;
}
static int pub_enter_native(void *context, const void *metadata, void *snapshot)
{
    pub_request *request = context;
    pub_request *previous = g_pub_entering;
    const char *why = pub_invalid_reason(request);
    int ok = 1;
    if (why) {
        char line[240];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MPKG: the retained published map is not entered: %s", why);
        backend_log(line);
        return 0;
    }
    request->launched = 1;
    g_pub_entering = request;
    g_pub_delegating++;
    __try {
        if (request->kind == PUB_LOBBY) {
            void *cache = pub_cache();
            if (!cache || !sh_map_native_matches(request->selection, (unsigned char *)pub_history() + 8)) {
                backend_log("MPKG: the retained published map is not entered: the lobby history names another map");
                ok = 0;
            } else {
                g_pub_cache_set(cache, metadata, snapshot);
                ok = ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(request->manager, request->option) != 0;
                if (!ok) pub_log_declined("lobby");
            }
        } else if (request->kind == PUB_READY) {
            /* This callback never reads its consumed download-request argument.
             * It checks the host selection again before publishing readiness. */
            ((pub_ready_fn)g_pub_original[PUB_READY])(request->manager, NULL, metadata, snapshot);
        } else ((pub_launch_fn)g_pub_original[request->kind])(request->manager, metadata, snapshot);
    } __finally { g_pub_delegating--; g_pub_entering = previous; }
    return ok;
}
/* A lobby whose map needs no consent is created now, inside the player's own
 * press, exactly when the engine would have created it: its launch reads the
 * history record and the browser cache the engine filled a moment ago, and the
 * session state is the one the press found. Deferring it to a later frame let
 * the engine's own sign-in, rate-limit and party checks fail after a long
 * preparation. The map's packages still activate at the native allocation
 * boundary through the retained request. */
static int pub_launch_lobby_now(pub_request *request)
{
    pub_request *previous = g_pub_entering;
    int ok = 0;
    request->launched = 1;
    g_pub_entering = request;
    g_pub_delegating++;
    __try { ok = ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(request->manager, request->option) != 0; }
    __finally { g_pub_delegating--; g_pub_entering = previous; }
    if (ok) {
        char name[96] = "", line[400];
        if (!sh_map_native_identity(request->selection, g_pub_armed_id, sizeof g_pub_armed_id,
                g_pub_armed_revision, sizeof g_pub_armed_revision))
            sh_map_published_disarm();
        sh_map_native_level_name(request->selection, name, sizeof name);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MPKG: published lobby created at the player's press for item '%s' revision '%s' (map '%s'); "
            "its packages activate at the native map boundary", g_pub_armed_id, g_pub_armed_revision, name);
        backend_log(line);
    } else pub_log_declined("lobby");
    return ok;
}
static int pub_enter(void *context, const char *json)
{
    pub_request *request = context;
    char error[512];
    int ok;
    /* A lobby created at the press has nothing left to enter natively. */
    if (request->launched) return 0;
    /* This preliminary snapshot contains only lobby settings. Full gameplay
     * decoding receives the prepared source through pub_select at activation. */
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

/* A published map whose text carries no package is the engine's own case: it
 * never enters the package context, the retained request or the transition
 * handoff, and its native launch runs exactly as it did before these gates
 * existed. Only a map that carries smpkg deliveries or package references
 * takes the retained route; a text that cannot be read keeps the guarded
 * route and its notices. */
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
        sh_rawmap_loading loading = {pub_waiting, pub_matches_level, pub_differs_level, pub_select};
        if (!pub_manager_current(manager)) goto done;
        /* One retained request per selection. A press repeated while the map is
         * still being prepared, waits for consent or sits in its lobby must not
         * cancel that request, prepare again or create a second lobby. */
        {
            const pub_request *pending = sh_rawmap_pending_map_context();
            if (pending && pending->manager == manager && sh_map_native_matches(pending->selection, metadata)) {
                backend_log("MPKG: the published map is already prepared or waiting in its lobby; the repeated launch is ignored");
                accepted = PUB_RETAINED; goto done;
            }
        }
        request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request));
        if (!request) { strcpy_s(error, sizeof(error), "Published-map request allocation failed."); goto done; }
        request->kind = kind; request->manager = manager; request->option = option;
        request->history = kind == PUB_LOBBY ? pub_history() : NULL;
        request->selection = sh_map_native_capture(metadata, error, sizeof(error));
        if (!request->selection) goto done;
        json = sh_map_native_source(request->selection, &length);
        /* A reference shard names a package the map needs as surely as an
         * embedded delivery does; either keeps the map on the retained route. */
        if (json && length && sh_mpkg_scan(json, length, NULL, 0) == 0 &&
            sh_mpkg_reference_scan(json, length, NULL, 0) == 0) {
            /* Nothing of ours may outlive this launch; a retained request that
             * cannot be canceled is left to its own validity checks. */
            if (!sh_rawmap_cancel_pending_map())
                backend_log("MPKG: a previous retained map request could not be canceled before a native published launch");
            accepted = PUB_NATIVE; goto done;
        }
        if (release_progress) {
            void *progress = *(void **)((unsigned char *)manager + 0xa38);
            if (progress) ((void (*)(void *, int))(*(void ***)progress)[0x30 / 8])(progress, 0);
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
        if (sh_rawmap_preflight_loading_request(json, length, &launch, &loading, error, sizeof(error))) {
            accepted = PUB_RETAINED;
            if (kind == PUB_LOBBY && sh_rawmap_pending_map_covered() && !pub_launch_lobby_now(request)) {
                /* The retained request owns the selection now; canceling it
                 * releases both. The engine has already said why. */
                request = NULL;
                if (!sh_rawmap_cancel_pending_map())
                    backend_log("MPKG: the declined lobby's retained request could not be canceled");
                accepted = PUB_REFUSED;
            }
        }
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, sizeof(error), "Published-map preparation raised exception %08lx.", GetExceptionCode());
    }
    if (accepted != PUB_RETAINED && request) pub_release(request);
    if (accepted == PUB_REFUSED && error[0]) sh_mpkg_report_error(SH_MPKG_PROBLEM_OPEN, error);
    return accepted;
}

/* A launch is prepared only when user packages are served this launch;
 * otherwise it runs natively, as without Snapmap+. */
static int pub_prepares(void) { return g_pub_ready && !sh_package_runtime_serving_disabled(); }
static void pub_offline(void *manager, const void *metadata, void *snapshot)
{
    if (!pub_prepares() || pub_same_launch(manager, metadata) ||
        pub_prepare(PUB_OFFLINE, manager, metadata, snapshot, 0, 0) == PUB_NATIVE)
        ((pub_launch_fn)g_pub_original[PUB_OFFLINE])(manager, metadata, snapshot);
}
static unsigned char pub_lobby(void *manager, unsigned char option)
{
    if (!pub_prepares()) return ((pub_lobby_fn)g_pub_original[PUB_LOBBY])(manager, option);
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
    sh_mpkg_report_error(SH_MPKG_PROBLEM_OPEN, "The selected published map is no longer available."); return 0;
}
static void pub_direct(void *manager, const void *metadata, void *snapshot)
{
    if (!pub_prepares() || pub_same_launch(manager, metadata) ||
        pub_prepare(PUB_DIRECT, manager, metadata, snapshot, 0, 0) == PUB_NATIVE)
        ((pub_launch_fn)g_pub_original[PUB_DIRECT])(manager, metadata, snapshot);
}
static void pub_ready(void *manager, void *download, const void *metadata, void *snapshot)
{
    if (!pub_prepares() || pub_same_launch(manager, metadata)) { ((pub_ready_fn)g_pub_original[PUB_READY])(manager, download, metadata, snapshot); return; }
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
