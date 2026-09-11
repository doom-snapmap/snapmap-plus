#include "webview_json.h"
/* WebView2 frontend host. sh_ui_init (ordinal 10) receives loop state in arg[0]
 * and the backend interface in arg[3]. A single STA thread handles the page,
 * drains UI requests at about 30 Hz, and polls change-gated editor state.
 * Engine mutations use backend interface slots declared in snapmap_plus_iface.h. */
#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <map>
#include <utility>

#include "WebView2.h"
#include "snapmap_plus_iface.h"
#include "mockup_html.h"
#include "config_message.h"
#include "growing_text_buffer.h"
#include "serialization_buffer.h"
#include "theme_bootstrap.h"
#include "report_scrub.h"   /* pure anonymization scrub + tail for the crash-report log attachment */
#include "crash_pending.h"
#include "log_rotate.h"     /* the UI log is append-only too; bound it like the backend's */
#include "host_image.h"     /* sh_host_renderer_name -- which renderer the player is actually running */
#include "../sh_entity_desc.h"/* generated entity descriptions */
#include "../sh_event_catalog.h"/* generated event names and argument types */
#include "../sh_entity_asset_lists.h"/* generated per-class model and animation choices */
#include "../sh_event_docs.h"/* generated event and argument descriptions */

using namespace Microsoft::WRL;

struct ShLoopState { CRITICAL_SECTION mtx; uint64_t flags; };

static ShLoopState *g_loop  = nullptr;
static sh_iface    *g_iface = nullptr;

static HWND                     g_hwnd         = nullptr;
static ICoreWebView2Controller *g_controller   = nullptr;
static ICoreWebView2           *g_webview      = nullptr;
static ComPtr<ICoreWebView2Environment12> g_webview_environment12;
static ComPtr<ICoreWebView2_17>            g_webview17;
static bool                     g_webview_ready = false;
static std::wstring             g_html;
static std::string              g_version = "dev";
static unsigned int             g_config_status_flags = 0;
static bool                     g_config_status_posted = false;

#define POC_CONFIG_KEY_CAP 128u
#define POC_CONFIG_VALUE_CAP (64u * 1024u)

static bool          g_sync_on       = false;   /* "Synchronize with editor" checkbox */
static int           g_displayed_eid = -1;      /* entity the state panel is showing */
static int           g_last_editor_sel = -1;    /* reverse-apply guard */
static uint64_t      g_last_list_sig  = 0;
static uint64_t      g_last_state_sig = 0;
static uint64_t      g_last_sel_sig   = 0;       /* forward-sync: last editor-selection signature */

/* Retain a growing read buffer for large declarations. At the 32 MiB cap,
 * report truncation instead of exposing partial JSON as editable text. */
#define POC_DECL_INITIAL_CAP (64u * 1024u)
#define POC_DECL_MAX_CAP     (32u * 1024u * 1024u)
static std::vector<char> g_state_decl;

static volatile bool g_pending_save = false;
static int           g_save_eid     = -1;
static std::string   g_save_decl, g_save_class, g_save_inherit, g_save_dname;
static int           g_save_result  = 0;

static volatile bool g_pending_delete = false;
static std::vector<int> g_delete_eids;

static volatile bool g_pending_select = false;   /* list -> editor selection push ("Select in editor") */
static std::vector<int> g_select_eids;
static volatile bool g_pending_deselect = false;  /* UI blank-space deselect -- clear_selection convenience */
static volatile bool g_select_refused   = false;  /* last selection push was refused (editor mid-grab/hold) */
static char g_enumbuf[262144];                   /* packed-string scratch for enum_inherits / enum_valid_classes */

/* Camera-manipulation state for the editable footer controls. camSet queues a one-shot write; camLock
 * holds the supplied target through the per-frame writer until the page releases it. */
static volatile bool g_cam_lock = false;
static float         g_cam_xyz[3] = {0.0f, 0.0f, 0.0f};
static volatile bool g_cam_write_once = false;
static volatile bool g_cam_read_published = false;  /* force one readout after each page navigation */

static volatile bool g_pending_create_prefab = false;
static std::string   g_create_prefab_name;
static int           g_create_result = 0;        /* 1 ok; 0 empty editor selection; 2 not hovering an entity in selection; -1 resolve/serialize/write failure */
static int           g_last_selcount = -1;       /* last broadcast editor-selection count (Create-button gating) */

static volatile bool g_pending_delete_prefab = false;
static std::string   g_delete_prefab_name, g_delete_prefab_folder;   /* folder="" -> root (prefabs/) */
static int           g_delete_result = 0;        /* 1 ok; 0 DeleteFile failed (missing/locked); -1 resolve failed */

static volatile bool g_pending_rename_prefab = false;
static std::string   g_rename_prefab_old, g_rename_prefab_new, g_rename_prefab_folder;
static int           g_rename_result = 0;        /* 1 ok; 0 MoveFile failed (dest exists/missing/locked); -1 resolve failed */

static volatile bool g_pending_load_prefab = false;
static std::string   g_load_prefab_name, g_load_prefab_folder;
/* New-entity requests supply prefab JSON directly to the Load/Place apply path. */
static volatile bool g_pending_new_entity = false;
static std::string   g_new_entity_json, g_new_entity_label;
/* The rawmap file picker, run off the UI thread. `busy` keeps a second dialog from
 * opening behind the first; `done` is the handoff, written last, so the interlocked
 * write is the barrier and no lock is needed. */
static volatile LONG g_pick_busy = 0;
static volatile LONG g_pick_done = 0;
static int           g_pick_kind = 0;        /* 0 = Load Rawmap, 1 = Save Rawmap As */
static bool          g_pick_ok   = false;    /* false = cancelled */
static std::wstring  g_pick_path;
/* Keep the latest sound request until the drain; an empty name stops playback.
 * The backend marshals live audio changes to the engine thread. */
static volatile bool g_pending_sound_preview = false;
static std::string   g_sound_preview_name;
static int           g_sound_preview_result = 0;
/* Preview-mode session, held while the asset browser is on screen. Separate pending flag from the
 * play above so opening the browser and clicking Play in the same frame still arrive in order. */
static volatile bool g_pending_sound_session = false;
static volatile int  g_sound_session_on = 0;
static int           g_new_entity_result = -1;
static int           g_load_result = 0;          /* 1 queued for stage/place; 0/-1 = resolve, read, or queue failure.
                                                   * Queue acceptance does not confirm placement. */

/* Folders: one real level of subdirectories under %LOCALAPPDATA%\snapmap-plus\prefabs\ (no nested-within-nested).
 * folder="" always means the root prefabs\ dir. The folder/file IS the truth -- no separate manifest. */
static volatile bool g_pending_create_folder = false;
static std::string   g_create_folder_name;
static int           g_create_folder_result = 0;   /* 1 ok; 0 already exists / empty name; -1 CreateDirectory failed */

static volatile bool g_pending_rename_folder = false;
static std::string   g_rename_folder_old, g_rename_folder_new;
static int           g_rename_folder_result = 0;   /* 1 ok; 0 MoveFile failed (dest exists/locked); -1 resolve failed */

static volatile bool g_pending_delete_folder = false;
static std::string   g_delete_folder_name;
static int           g_delete_folder_result = 0;   /* 1 ok (removed, any contents moved to root); 0 RemoveDirectory failed
                                                     * (a name collision at root left a file behind); -1 resolve failed */

static volatile bool g_pending_move_prefab = false;
static std::string   g_move_prefab_name, g_move_prefab_from, g_move_prefab_to;
static int           g_move_prefab_result = 0;      /* 1 ok; 0 MoveFile failed (dest name collision); -1 resolve failed */

/* Defer timeline serialization to the UI drain, then send its JSON to the page. */
static volatile bool g_pending_open_timeline = false;
static int           g_open_timeline_eid = -1;
static int           g_tl_json_len = 0;             /* bytes serialized into g_tl_json this drain (0 = failed) */

/* Resolve event-target inheritance separately so it cannot overwrite an open timeline. */
static volatile bool g_pending_resolve_entity = false;
static int           g_resolve_entity_eid = -1;
static int           g_resolve_json_len = 0;

/* Save the complete entity JSON after the page refreshes and patches its timeline.
 * kind=0 targets an entity; kind=1/2 are prefab staging operations. */
static volatile bool g_pending_save_timeline = false;
static int           g_save_timeline_eid = -1;
static std::string   g_save_timeline_json;          /* UTF-8; the page's JSON.stringify output, already fully patched */
static int           g_save_timeline_result = 0;     /* 1 ok, 0 apply_edit refused/failed */

#define POC_MAX_ENTS 8192
#define POC_ID_CAP   384
#define POC_NAME_CAP 192
struct PocEnt { int eid; char id[POC_ID_CAP]; char name[POC_NAME_CAP]; int hidden; };
static PocEnt *g_ents = nullptr;

/* Rebuild the Timeline and Encounter Manager list when the entity signature changes. */
#define POC_MAX_TLS 2048
struct PocTl { int eid; char id[POC_ID_CAP]; char name[POC_NAME_CAP]; };
static PocTl *g_tls = nullptr;
static int    g_tl_count = 0;
static unsigned long g_list_seq = 0;

struct PocCollectPerf {
    unsigned calls, state_calls;
    unsigned long long collect_total_us, collect_max_us;
    unsigned long long poll_total_us, poll_max_us;
    unsigned long long selection_total_us, selection_max_us;
    unsigned long long state_total_us, state_max_us;
    int min_entities, max_entities, max_selection_count;
};
static PocCollectPerf g_collect_perf = {};

/* File logging. */
/* Keep the log name recognizable beside the backend log. */
static const char *kUiLogPath = "snapmap-plus\\logs\\snapmap-plus-ui.log";

static void poc_log(const char *msg)
{
    static bool rolled = false;
    CreateDirectoryA("snapmap-plus", nullptr);   /* one level at a time; both idempotent */
    CreateDirectoryA("snapmap-plus\\logs", nullptr);
    FILE *f = nullptr;
    if (!rolled) {
        rolled = true;
        log_rotate_if_large(kUiLogPath, LOG_ROTATE_CAP_BYTES);
        /* Remove the obsolete log filename after the rename. */
        DeleteFileA("snapmap-plus\\logs\\webview_poc.log");
    }
    if (fopen_s(&f, kUiLogPath, "a") == 0 && f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
        fclose(f);
    }
}
static void poc_logf(const char *fmt, unsigned long a) { char l[256]; _snprintf_s(l, sizeof l, _TRUNCATE, fmt, a); poc_log(l); }

static unsigned long long poc_perf_now_us()
{
    static LARGE_INTEGER freq = []() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0) return 0;
    unsigned long long ticks = (unsigned long long)now.QuadPart;
    unsigned long long hz = (unsigned long long)freq.QuadPart;
    return (ticks / hz) * 1000000ull + ((ticks % hz) * 1000000ull) / hz;
}

static void poc_perf_flush_collect(const char *reason)
{
    if (g_collect_perf.calls == 0) return;
    char l[512];
    _snprintf_s(l, sizeof l, _TRUNCATE,
                "perf poll-window: end=%s calls=%u entities=%d..%d selected_max=%d total_avg_us=%llu total_max_us=%llu collect_avg_us=%llu collect_max_us=%llu selection_avg_us=%llu selection_max_us=%llu state_calls=%u state_avg_us=%llu state_max_us=%llu",
                reason ? reason : "unknown", g_collect_perf.calls,
                g_collect_perf.min_entities, g_collect_perf.max_entities,
                g_collect_perf.max_selection_count,
                g_collect_perf.poll_total_us / g_collect_perf.calls, g_collect_perf.poll_max_us,
                g_collect_perf.collect_total_us / g_collect_perf.calls, g_collect_perf.collect_max_us,
                g_collect_perf.selection_total_us / g_collect_perf.calls, g_collect_perf.selection_max_us,
                g_collect_perf.state_calls,
                g_collect_perf.state_calls ? g_collect_perf.state_total_us / g_collect_perf.state_calls : 0,
                g_collect_perf.state_max_us);
    poc_log(l);
    g_collect_perf = {};
}

static void poc_perf_note_collect(unsigned long long us, int entities, const char *reason)
{
    if (!reason || strcmp(reason, "poll") != 0) {
        char l[192];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "perf collect: reason=%s entities=%d duration_us=%llu",
                    reason ? reason : "unknown", entities, us);
        poc_log(l);
        return;
    }

    if (g_collect_perf.calls == 0) {
        g_collect_perf.min_entities = entities;
        g_collect_perf.max_entities = entities;
    } else {
        if (entities < g_collect_perf.min_entities) g_collect_perf.min_entities = entities;
        if (entities > g_collect_perf.max_entities) g_collect_perf.max_entities = entities;
    }
    g_collect_perf.calls++;
    g_collect_perf.collect_total_us += us;
    if (us > g_collect_perf.collect_max_us) g_collect_perf.collect_max_us = us;
    if (us >= 5000) {
        char l[160];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "perf collect-slow: entities=%d duration_us=%llu", entities, us);
        poc_log(l);
    }
}

static void poc_perf_note_poll(unsigned long long total_us, unsigned long long collect_us,
                               unsigned long long selection_us, unsigned long long state_us,
                               bool state_called, int entities, int selection_count)
{
    g_collect_perf.poll_total_us += total_us;
    if (total_us > g_collect_perf.poll_max_us) g_collect_perf.poll_max_us = total_us;
    g_collect_perf.selection_total_us += selection_us;
    if (selection_us > g_collect_perf.selection_max_us) g_collect_perf.selection_max_us = selection_us;
    if (state_called) {
        g_collect_perf.state_calls++;
        g_collect_perf.state_total_us += state_us;
        if (state_us > g_collect_perf.state_max_us) g_collect_perf.state_max_us = state_us;
    }
    if (selection_count > g_collect_perf.max_selection_count)
        g_collect_perf.max_selection_count = selection_count;
    if (total_us >= 10000) {
        char l[256];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "perf poll-slow: entities=%d selected=%d total_us=%llu collect_us=%llu selection_us=%llu state_us=%llu",
                    entities, selection_count, total_us, collect_us, selection_us,
                    state_called ? state_us : 0);
        poc_log(l);
    }
    if (g_collect_perf.calls >= 90) poc_perf_flush_collect("30-second-window");
}

/* Change signatures. */
static uint64_t hstr(uint64_t h, const char *s) { while (*s) { h = (h ^ (unsigned char)*s) * 1099511628211ull; s++; } return h; }
static uint64_t hint(uint64_t h, int v) { for (int i = 0; i < 4; i++) { h = (h ^ (unsigned char)(v & 0xff)) * 1099511628211ull; v >>= 8; } return h; }

/* Validate each name component before resolve_prefab_path concatenates it.
 * Reject separators and traversal here as well as in the page. */
static bool poc_valid_name(const std::string &n)
{
    if (n.empty() || n.size() > 200) return false;
    if (n.find("..") != std::string::npos) return false;
    if (n.find('/') != std::string::npos || n.find('\\') != std::string::npos) return false;
    if (n.find(':') != std::string::npos) return false;
    return true;
}
/* Keep the user's asset pins in pinned.json, separate from validated settings.
 * A damaged pin list must not reset unrelated preferences. The host transports
 * opaque bytes; the page owns parsing, validation, and the empty-list fallback. */
static std::string poc_pins_path()
{
    char *la = nullptr; size_t n = 0;
    if (_dupenv_s(&la, &n, "LOCALAPPDATA") != 0 || !la) return std::string();
    std::string dir = std::string(la) + "\\snapmap-plus";
    free(la);
    SHCreateDirectoryExA(nullptr, dir.c_str(), nullptr);   /* no-op when it already exists */
    return dir + "\\pinned.json";
}

static void poc_send_pins()
{
    std::string data;
    std::string path = poc_pins_path();
    if (!path.empty()) {
        FILE *f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
            char buf[4096]; size_t r;
            while ((r = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, r);
            fclose(f);
        }
    }
    /* Escape file contents as a string so malformed JSON cannot break the envelope. */
    std::wstring m = L"{\"kind\":\"pins\",\"doc\":\"";
    m += sh_webview_json::escape_wide(data.c_str());
    m += L"\"}";
    if (g_webview) g_webview->PostWebMessageAsJson(m.c_str());
}

/* Replace the full pin document via a temporary file and atomic move. */
static void poc_save_pins(const std::string &doc)
{
    std::string path = poc_pins_path();
    if (path.empty()) return;
    std::string tmp = path + ".tmp";
    FILE *f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) { poc_log("pins: could not open the temp file"); return; }
    size_t w = doc.empty() ? 0 : fwrite(doc.data(), 1, doc.size(), f);
    fclose(f);
    if (w != doc.size()) { DeleteFileA(tmp.c_str()); poc_log("pins: short write, kept the previous file"); return; }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        poc_log("pins: could not replace pinned.json");
    }
}

static void poc_read_version()
{
    char *la = nullptr; size_t n = 0;
    if (_dupenv_s(&la, &n, "LOCALAPPDATA") != 0 || !la) return;
    std::string path = std::string(la) + "\\snapmap-plus\\install.json";
    free(la);
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return;
    std::string data; char buf[4096]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, r);
    fclose(f);
    size_t k = data.find("\"version\""); if (k == std::string::npos) return;
    k = data.find(':', k); if (k == std::string::npos) return;
    k = data.find('"', k);  if (k == std::string::npos) return; k++;
    size_t e = data.find('"', k); if (e == std::string::npos) return;
    g_version = data.substr(k, e - k);
}

/* Guarded engine access. */
static int poc_editor_ready()
{
    int r = 0;
    __try {
        if (g_iface && g_iface->vtbl) {
            if (g_iface->vtbl->editor_ready_poll) r = g_iface->vtbl->editor_ready_poll(g_iface) ? 1 : 0;
            else if (g_iface->vtbl->entity_count)  r = g_iface->vtbl->entity_count(g_iface) > 0 ? 1 : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}
static int poc_collect(int *out_ready)
{
    int n = 0; *out_ready = 0;
    __try {
        if (!g_iface || !g_iface->vtbl) return 0;
        if (g_iface->vtbl->editor_ready_poll) *out_ready = g_iface->vtbl->editor_ready_poll(g_iface) ? 1 : 0;
        int count = g_iface->vtbl->entity_count ? g_iface->vtbl->entity_count(g_iface) : 0;
        for (int id = 0; id < count && n < POC_MAX_ENTS; id++) {
            if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) continue;
            char idbuf[POC_ID_CAP]; idbuf[0] = 0; const char *s = idbuf;
            if (g_iface->vtbl->id_to_string) { const char *r = g_iface->vtbl->id_to_string(g_iface, id, idbuf, sizeof idbuf); if (r) s = r; }
            if (!s || s[0] == 0) continue;
            if (s[0]=='N' && s[1]=='U' && s[2]=='L' && s[3]=='L' && s[4]=='_') continue;
            char nmbuf[POC_NAME_CAP]; nmbuf[0] = 0; const char *nm = nmbuf;
            if (g_iface->vtbl->get_displayname) { const char *r = g_iface->vtbl->get_displayname(g_iface, id, nmbuf, sizeof nmbuf); if (r) nm = r; }
            int hidden = 0;
            if (g_iface->vtbl->id_dev_layer_hidden) hidden = g_iface->vtbl->id_dev_layer_hidden(g_iface, id) ? 1 : 0;
            g_ents[n].eid = id;
            strncpy_s(g_ents[n].id, POC_ID_CAP, s, _TRUNCATE);
            strncpy_s(g_ents[n].name, POC_NAME_CAP, nm ? nm : "", _TRUNCATE);
            g_ents[n].hidden = hidden;
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return n;
}
static int poc_collect_timed(int *out_ready, const char *reason,
                             unsigned long long *elapsed_us = nullptr)
{
    unsigned long long started = poc_perf_now_us();
    int n = poc_collect(out_ready);
    unsigned long long finished = poc_perf_now_us();
    unsigned long long elapsed = finished >= started ? finished - started : 0;
    if (elapsed_us) *elapsed_us = elapsed;
    poc_perf_note_collect(elapsed, n, reason);
    return n;
}
/* Rebuild timeline entries only when the entity signature changes. Check every
 * visible entity's class so Encounter Managers are included alongside Timelines.
 * Guard each class read so one bad entity cannot abort the rescan. */
static void poc_rescan_timelines(int n)
{
    int tn = 0;
    if (!g_tls || !g_iface || !g_iface->vtbl || !g_iface->vtbl->get_classname_copy) { g_tl_count = 0; return; }
    for (int i = 0; i < n && tn < POC_MAX_TLS; i++) {
        if (g_ents[i].hidden) continue;   /* dev-layer hidden -> excluded from BOTH lists, matching the OG quirk */
        char clsbuf[128]; clsbuf[0] = 0;
        const char *c = NULL;
        __try {
            c = g_iface->vtbl->get_classname_copy(g_iface, g_ents[i].eid, clsbuf, sizeof clsbuf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            c = NULL;
            poc_logf("poc_rescan_timelines: get_classname_copy FAULTED for id=%lu (skipped)", (unsigned long)g_ents[i].eid);
        }
        if (c && (strcmp(c, "idTarget_Timeline") == 0 || strcmp(c, "idEncounterManager") == 0)) {
            /* Replace the palette placeholder inherit with the portable Timeline inherit.
             * The backend slot is idempotent and cheap on nonmatches; the state poll
             * refreshes an open panel after this normalization. */
            if (strcmp(c, "idTarget_Timeline") == 0 && g_iface->vtbl->normalize_timeline_inherit) {
                __try { g_iface->vtbl->normalize_timeline_inherit(g_iface, g_ents[i].eid); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            g_tls[tn].eid = g_ents[i].eid;
            strncpy_s(g_tls[tn].id, POC_ID_CAP, g_ents[i].id, _TRUNCATE);
            strncpy_s(g_tls[tn].name, POC_NAME_CAP, g_ents[i].name, _TRUNCATE);
            tn++;
        }
    }
    g_tl_count = tn;
}
static void poc_rescan_timelines_timed(int n, const char *reason)
{
    unsigned long long started = poc_perf_now_us();
    poc_rescan_timelines(n);
    unsigned long long finished = poc_perf_now_us();
    char l[224];
    _snprintf_s(l, sizeof l, _TRUNCATE,
                "perf timeline-rescan: reason=%s entities=%d timelines=%d duration_us=%llu",
                reason ? reason : "unknown", n, g_tl_count,
                finished >= started ? finished - started : 0);
    poc_log(l);
}
static bool poc_collect_state(int id, char *decl, int dcap, char *cls, int ccap, char *inh, int icap, char *dnm, int ncap)
{
    bool ok = false; decl[0] = cls[0] = inh[0] = dnm[0] = 0;
    __try {
        if (!g_iface || !g_iface->vtbl) return false;
        if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) return false;
        if (g_iface->vtbl->get_declsource_copy) { const char *r = g_iface->vtbl->get_declsource_copy(g_iface, id, decl, dcap); if (r && r != decl) strncpy_s(decl, dcap, r, _TRUNCATE); }
        if (g_iface->vtbl->get_classname_copy)  { const char *r = g_iface->vtbl->get_classname_copy(g_iface, id, cls, ccap);  if (r && r != cls)  strncpy_s(cls,  ccap, r, _TRUNCATE); }
        if (g_iface->vtbl->get_inherit_copy)    { const char *r = g_iface->vtbl->get_inherit_copy(g_iface, id, inh, icap);    if (r && r != inh)  strncpy_s(inh,  icap, r, _TRUNCATE); }
        if (g_iface->vtbl->get_displayname)     { const char *r = g_iface->vtbl->get_displayname(g_iface, id, dnm, ncap);     if (r && r != dnm)  strncpy_s(dnm,  ncap, r, _TRUNCATE); }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}
static bool poc_collect_state_growing(int id, char *cls, int ccap, char *inh, int icap,
                                      char *dnm, int ncap, bool *truncated)
{
    return sh_read_growing_text(
        g_state_decl, POC_DECL_INITIAL_CAP, POC_DECL_MAX_CAP,
        [&](char *decl, int dcap) {
            return poc_collect_state(id, decl, dcap, cls, ccap, inh, icap, dnm, ncap);
        },
        truncated);
}
static void poc_post_json(const wchar_t *json);   /* fwd */

/* Camera Origin: write path (+0x00). Used every frame while Lock position is enabled and once per
 * committed camSet edit. SEH-guarded. */
static void poc_cam_write()
{
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->set_editor_vec3)
            g_iface->vtbl->set_editor_vec3(g_iface, g_cam_xyz);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
/* Camera Origin: read the live vec3 (+0x08); if it moved, cache + push it to the footer. SEH-guarded. */
static void poc_cam_read_send()
{
    float cam[3] = {0.0f, 0.0f, 0.0f};
    int ok = 0;
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->get_editor_vec3) { g_iface->vtbl->get_editor_vec3(g_iface, cam); ok = 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) return;
    if (g_cam_read_published &&
        fabsf(cam[0]-g_cam_xyz[0]) < 1e-4f &&
        fabsf(cam[1]-g_cam_xyz[1]) < 1e-4f &&
        fabsf(cam[2]-g_cam_xyz[2]) < 1e-4f) return;
    g_cam_xyz[0] = cam[0]; g_cam_xyz[1] = cam[1]; g_cam_xyz[2] = cam[2];
    wchar_t m[192];
    _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"camera\",\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}", cam[0], cam[1], cam[2]);
    poc_post_json(m);
    g_cam_read_published = true;
}

static int poc_get_selection(int *out, int max)
{
    int n = 0;
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->get_selection) {
            n = g_iface->vtbl->get_selection(g_iface, out, max);
            if (n < 0) n = 0;
            if (n > max) n = max;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    return n;
}
static void poc_apply_save()
{
    g_save_result = -2;
    __try {
        if (!g_iface || !g_iface->vtbl) { g_save_result = -2; return; }
        int id = g_save_eid;
        if (id < 0) { g_save_result = -2; return; }
        if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) { g_save_result = -2; return; }
        int r = -1;
        if (g_iface->vtbl->apply_class_inherit) r = g_iface->vtbl->apply_class_inherit(g_iface, id, g_save_class.c_str(), g_save_inherit.c_str());
        if (r == -1) {
            if (g_iface->vtbl->set_classname) g_iface->vtbl->set_classname(g_iface, id, g_save_class.c_str());
            if (g_iface->vtbl->set_inherit)   g_iface->vtbl->set_inherit(g_iface, id, g_save_inherit.c_str());
        }
        /* Apply the display name only after the class/inherit pair is accepted. */
        if (r == 1 || r == -1) {
            if (g_iface->vtbl->rebuild_set_declsource) g_iface->vtbl->rebuild_set_declsource(g_iface, id, g_save_decl.c_str());
            int r2 = -1;
            if (g_iface->vtbl->apply_class_inherit) r2 = g_iface->vtbl->apply_class_inherit(g_iface, id, g_save_class.c_str(), g_save_inherit.c_str());
            if (r2 == -1) {
                if (g_iface->vtbl->set_classname) g_iface->vtbl->set_classname(g_iface, id, g_save_class.c_str());
                if (g_iface->vtbl->set_inherit)   g_iface->vtbl->set_inherit(g_iface, id, g_save_inherit.c_str());
            }
            if (r2 != 1 && r2 != -1) { g_save_result = -2; return; }
            if (g_iface->vtbl->set_entity_0x170) g_iface->vtbl->set_entity_0x170(g_iface, id, g_save_dname.c_str());
            g_save_result = 1;
        } else g_save_result = r == 0 ? 0 : -2;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_save_result = -2; }
}
static void poc_apply_deletes()
{
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->selection_guard)
            for (size_t i = 0; i < g_delete_eids.size(); i++) {
                int id = g_delete_eids[i];
                if (id >= 0) g_iface->vtbl->selection_guard(g_iface, id);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
/* Serialize into caller-owned storage under SEH. Timeline-open and target-inherit
 * requests use separate buffers so requests in one drain cannot overwrite each other. */
/* A clean zero can mean insufficient capacity. An exception is a terminal
 * failure, so keep its negative result distinct and do not retry it as a size issue. */
static int poc_serialize_entity_into(int id, char *buf, int cap)
{
    int n = 0;
    if (cap > 0) buf[0] = 0;
    if (!(g_iface && g_iface->vtbl && g_iface->vtbl->serialize_entity && id >= 0))
        return SH_SERIALIZE_UNAVAILABLE;
    __try {
        n = g_iface->vtbl->serialize_entity(g_iface, id, buf, cap - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return SH_SERIALIZE_THREW; }
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    buf[n] = 0;
    return n;
}
/* Retry zero or cap-filling results with a larger retained buffer. The engine
 * does not report required size; an exact fit needs one extra read to confirm it. */
#define POC_SERIALIZE_INITIAL_CAP (1u * 1024 * 1024)    /* initial capacity; grows for larger timelines */
#define POC_SERIALIZE_MAX_CAP     (32u * 1024 * 1024)   /* transport safety cap */

static int poc_serialize_entity_grow(int id, std::vector<char> &buf, const char *what)
{
    char l[256];
    int n = sh_serialize_growing_buffer(
        buf, POC_SERIALIZE_INITIAL_CAP, POC_SERIALIZE_MAX_CAP,
        [&](char *out, int cap) { return poc_serialize_entity_into(id, out, cap); },
        [&](size_t cap, int result, bool terminal) {
        /* Negative results are terminal failures, not capacity problems. */
        if (result == SH_SERIALIZE_UNAVAILABLE) {
            _snprintf_s(l, sizeof l, _TRUNCATE, "%s: entity %d NOT ATTEMPTED -- interface/vtable missing or bad id",
                        what, id);
            poc_log(l);
            return;
        }
        if (result == SH_SERIALIZE_THREW) {
            _snprintf_s(l, sizeof l, _TRUNCATE, "%s: entity %d RAISED inside serialize_entity at %lu bytes -- "
                        "engine fault, not a buffer size", what, id, (unsigned long)cap);
            poc_log(l);
            return;
        }
        /* Log each retry to distinguish persistent zero results from truncating writes. */
        _snprintf_s(l, sizeof l, _TRUNCATE, "%s: entity %d attempt at %lu bytes returned %d",
                    what, id, (unsigned long)cap, result);
        poc_log(l);
        if (terminal) {
            _snprintf_s(l, sizeof l, _TRUNCATE, "%s: entity %d does not fit in %lu bytes (last result %d); refusing",
                        what, id, (unsigned long)cap, result);
            poc_log(l);
        }
    });
    if (n > 0 && buf.size() > POC_SERIALIZE_INITIAL_CAP) {
        _snprintf_s(l, sizeof l, _TRUNCATE, "%s: entity %d serialized %d bytes (buffer grown to %lu)",
                    what, id, n, (unsigned long)buf.size());
        poc_log(l);
    }
    return n > 0 ? n : 0;
}
/* Serialize the timeline with reusable storage that can grow for generated content. */
static std::vector<char> g_tl_json;
static int poc_serialize_entity_raw(int id) { return poc_serialize_entity_grow(id, g_tl_json, "timeline-open"); }
/* Post {kind:"timelineData", eid, ok, json:"<the serialized entity JSON, escaped>"}. The page JSON.parses
 * `json` and walks entityDef.state.edit.componentTimeLine / encounterComponent itself (the engine's
 * serialized entity is valid JSON). */
static void poc_emit_timeline_data(int eid, int json_len)
{
    if (!g_webview) return;
    bool ok = json_len > 0;
    std::wstring m = L"{\"kind\":\"timelineData\",\"eid\":"; m += std::to_wstring(eid);
    m += L",\"ok\":"; m += ok ? L"true" : L"false";
    m += L",\"json\":\""; if (ok) m += sh_webview_json::escape_wide(g_tl_json.data()); m += L"\"}";
    g_webview->PostWebMessageAsJson(m.c_str());
}
/* Serialize the event's target entity into its own buffer. The page reads
 * entityDef.inherit to select the appropriate model/animation choices. */
static std::vector<char> g_resolve_json;
static int poc_serialize_entity_resolve(int id) { return poc_serialize_entity_grow(id, g_resolve_json, "entity-resolve"); }
static void poc_emit_entity_inherit(int eid, int json_len)
{
    if (!g_webview) return;
    bool ok = json_len > 0;
    std::wstring m = L"{\"kind\":\"entityInherit\",\"eid\":"; m += std::to_wstring(eid);
    m += L",\"ok\":"; m += ok ? L"true" : L"false";
    m += L",\"json\":\""; if (ok) m += sh_webview_json::escape_wide(g_resolve_json.data()); m += L"\"}";
    g_webview->PostWebMessageAsJson(m.c_str());
}
/* Replace editor selection from the list. Log each call to locate a stalled slot. */
static void poc_apply_select_in_editor()
{
    poc_logf("select-in-editor: apply start ids=%lu", (unsigned long)g_select_eids.size());
    /* Manipulation snapshots index the selection positionally. Changing it during
     * a grab can corrupt Escape restoration; the backend enforces this gate too. */
    if (g_iface && g_iface->vtbl && g_iface->vtbl->manipulation_in_progress
        && g_iface->vtbl->manipulation_in_progress(g_iface)) {
        poc_log("select-in-editor: REFUSED -- editor is mid-manipulation (grab/hold)");
        g_select_refused = true;
        return;
    }
    __try {
        if (g_iface && g_iface->vtbl) {
            if (g_iface->vtbl->clear_selection) { poc_log("select-in-editor: clear"); g_iface->vtbl->clear_selection(g_iface); }
            if (g_iface->vtbl->add_to_selection)
                for (size_t i = 0; i < g_select_eids.size(); i++) {
                    int id = g_select_eids[i];
                    if (id < 0) continue;
                    if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) { poc_logf("select-in-editor: skip invalid id=%lu", (unsigned long)id); continue; }
                    poc_logf("select-in-editor: add id=%lu", (unsigned long)id);
                    g_iface->vtbl->add_to_selection(g_iface, id);
                }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { poc_log("select-in-editor: SEH in apply"); }
    poc_log("select-in-editor: apply done");
}
/* Mirror a page blank-space click by clearing the editor selection. */
static void poc_apply_deselect()
{
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->clear_selection) g_iface->vtbl->clear_selection(g_iface);
    } __except (EXCEPTION_EXECUTE_HANDLER) { poc_log("deselect: SEH in apply"); }
}
/* __try can't share a function with a C++ object needing unwinding (/EHsc, C2712) -- this leaf has only
 * PODs in scope, so the SEH guard around the engine call is safe here. */
static int poc_serialize_selection_raw(char *buf, int cap)
{
    int n = 0;
    __try { n = g_iface->vtbl->serialize_selection(g_iface, buf, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    return n;
}
/* Save the current selection as prefab JSON. The engine requires the hovered
 * entity to be selected; report that refusal separately from an empty selection.
 * Results: 1 = saved, 0 = empty, 2 = hover refused, -1 = resolve/serialize/write failure. */
static void poc_apply_create_prefab()
{
    g_create_result = -1;
    { char l[300]; _snprintf_s(l, sizeof l, _TRUNCATE, "create-prefab: START name='%s'", g_create_prefab_name.c_str()); poc_log(l); }
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path || !g_iface->vtbl->serialize_selection || g_create_prefab_name.empty()) {
        poc_log("create-prefab: ABORT (iface/slot/name missing)");
        return;
    }
    if (g_iface->vtbl->hovered_id && g_iface->vtbl->hovered_id(g_iface) < 0) {
        poc_log("create-prefab: ABORT (not hovering an entity in the selection)");
        g_create_result = 2;
        return;
    }
    if (!poc_valid_name(g_create_prefab_name)) {
        poc_log("create-prefab: ABORT (name rejected -- '..' or path separator)");
        return;
    }
    char path[1024]; path[0] = '\0';
    std::string fname = g_create_prefab_name + ".json";
    if (!g_iface->vtbl->resolve_prefab_path(g_iface, "prefabs/", fname.c_str(), path, (int)sizeof path) || !path[0]) {
        poc_log("create-prefab: ABORT (resolve_prefab_path failed)");
        return;
    }
    { char l[1200]; _snprintf_s(l, sizeof l, _TRUNCATE, "create-prefab: path='%s' -- about to serialize_selection (+0xb0)", path); poc_log(l); }
    static char buf[4 * 1024 * 1024];
    int n = poc_serialize_selection_raw(buf, (int)sizeof buf);
    poc_logf("create-prefab: serialize_selection returned n=%lu", (unsigned long)n);
    if (n <= 0) { g_create_result = 0; return; }
    FILE *fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) { poc_log("create-prefab: ABORT (fopen failed)"); return; }
    fwrite(buf, 1, (size_t)n, fp);
    fclose(fp);
    poc_log("create-prefab: WROTE file ok");
    g_create_result = 1;
}
/* folder="" -> the root prefabs\ dir; else prefabs\<folder>\ (one real level, no nesting). Shared by every
 * prefab/folder file op below so they all agree on where a folder actually lives on disk. */
static bool poc_prefab_dir(const std::string &folder, char *out, int cap)
{
    if (cap) out[0] = '\0';
    if (!folder.empty() && !poc_valid_name(folder)) return false;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path) return false;
    std::string prefix = folder.empty() ? "prefabs/" : ("prefabs/" + folder + "/");
    return g_iface->vtbl->resolve_prefab_path(g_iface, prefix.c_str(), "", out, cap) && out[0] != '\0';
}
static bool poc_prefab_file_path(const std::string &folder, const std::string &name, char *out, int cap)
{
    if (cap) out[0] = '\0';
    if (!folder.empty() && !poc_valid_name(folder)) return false;
    if (!poc_valid_name(name)) return false;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path) return false;
    std::string prefix = folder.empty() ? "prefabs/" : ("prefabs/" + folder + "/");
    std::string fname = name + ".json";
    return g_iface->vtbl->resolve_prefab_path(g_iface, prefix.c_str(), fname.c_str(), out, cap) && out[0] != '\0';
}
/* Keep descriptions and tags in <name>.meta.json beside the prefab. The prefab
 * stays engine JSON; the validated path helper handles both filenames. */
static bool poc_prefab_meta_path(const std::string &folder, const std::string &name, char *out, int cap)
{
    return poc_prefab_file_path(folder, name + ".meta", out, cap);
}
static void poc_strip_trailing_sep(char *s)
{
    size_t n = strlen(s);
    if (n && (s[n - 1] == '\\' || s[n - 1] == '/')) s[n - 1] = '\0';
}
/* list the *.json stems directly inside a resolved directory (non-recursive), sorted. */
static void poc_list_json_dir(const std::string &dirPath, std::vector<std::string> &names)
{
    std::string pattern = dirPath + "*.json";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fn = fd.cFileName;
        if (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".json") == 0) {
            std::string stem = fn.substr(0, fn.size() - 5);
            /* "<name>.meta.json" is a metadata sidecar, not a prefab -- never list it as one */
            if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".meta") == 0) continue;
            names.push_back(stem);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
}

/* Delete a prefab: 1 = deleted, 0 = file operation failed, -1 = path refused. */
static void poc_apply_delete_prefab()
{
    g_delete_result = -1;
    if (g_delete_prefab_name.empty()) return;
    char path[1024];
    if (!poc_prefab_file_path(g_delete_prefab_folder, g_delete_prefab_name, path, (int)sizeof path)) return;
    g_delete_result = DeleteFileA(path) ? 1 : 0;
    if (g_delete_result == 1) {   /* the sidecar goes with its prefab; absent is fine */
        char mp[1024];
        if (poc_prefab_meta_path(g_delete_prefab_folder, g_delete_prefab_name, mp, (int)sizeof mp)) DeleteFileA(mp);
    }
}
/* Rename within the folder without overwriting an existing destination.
 * Results: 1 = moved, 0 = file operation failed, -1 = path refused. */
static void poc_apply_rename_prefab()
{
    g_rename_result = -1;
    if (g_rename_prefab_old.empty() || g_rename_prefab_new.empty()) return;
    char oldp[1024], newp[1024];
    if (!poc_prefab_file_path(g_rename_prefab_folder, g_rename_prefab_old, oldp, (int)sizeof oldp)) return;
    if (!poc_prefab_file_path(g_rename_prefab_folder, g_rename_prefab_new, newp, (int)sizeof newp)) return;
    g_rename_result = MoveFileA(oldp, newp) ? 1 : 0;
    if (g_rename_result == 1) {   /* the sidecar follows the rename; absent is fine */
        char oldm[1024], newm[1024];
        if (poc_prefab_meta_path(g_rename_prefab_folder, g_rename_prefab_old, oldm, (int)sizeof oldm) &&
            poc_prefab_meta_path(g_rename_prefab_folder, g_rename_prefab_new, newm, (int)sizeof newm))
            MoveFileA(oldm, newm);
    }
}
/* Load/Place reads prefab JSON and queues kind=2 for staging and native paste.
 * Placement depends on backend editor gates; queue acceptance alone is not placement. */
/* __try can't share a function with a C++ object needing unwinding (/EHsc, C2712) -- these leaves have
 * only PODs in scope, so the SEH guards around the engine calls are safe here. */
static void poc_clear_selection_seh()
{
    if (g_iface && g_iface->vtbl && g_iface->vtbl->clear_selection) {
        __try { g_iface->vtbl->clear_selection(g_iface); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
static int poc_apply_edit_seh(const sh_apply_item *it, int count, const char *op)
{
    __try { return g_iface->vtbl->apply_edit(g_iface, it, count, op) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Preserve an in-progress result so the UI cannot mistake it for a failed save. */
static int poc_apply_sync_seh(const sh_apply_item *it, int count, const char *op)
{
    __try {
        if (g_iface->vtbl->apply_sync) {
            int result = g_iface->vtbl->apply_sync(g_iface, it, count, op);
            return result == SH_APPLY_IN_PROGRESS ? result : result == count ? 1 : 0;
        }
        if (g_iface->vtbl->apply_edit)
            return g_iface->vtbl->apply_edit(g_iface, it, count, op) ? SH_APPLY_IN_PROGRESS : 0;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void poc_apply_new_entity()
{
    g_new_entity_result = -1;
    if (g_new_entity_json.empty()) return;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->apply_edit) {
        poc_log("new-entity: ABORT (iface/slot missing)");
        return;
    }
    /* Native paste requires no hovered entity and an empty selection. Refuse here
     * so the page can explain the gate instead of reporting a queued but unplaced
     * entity. Pasting over a selection can miswire the old-to-new ID mapping. */
    if (g_iface->vtbl->hovered_id && g_iface->vtbl->hovered_id(g_iface) >= 0) {
        poc_log("new-entity: REFUSED (hovering an entity -- the engine will not paste there)");
        g_new_entity_result = -2;
        return;
    }
    static int ne_selids[POC_MAX_ENTS];
    if (poc_get_selection(ne_selids, POC_MAX_ENTS) > 0) {
        poc_log("new-entity: REFUSED (editor selection is not empty)");
        g_new_entity_result = -3;
        return;
    }
    sh_apply_item it; it.kind = 2; it.id = 0; it.text = g_new_entity_json.c_str();
    g_new_entity_result = poc_apply_edit_seh(&it, 1, "new-entity");
    char l[320];
    _snprintf_s(l, sizeof l, _TRUNCATE, "new-entity: '%s' staged=%d (kind=2: stage + auto pick-up), %zu bytes",
                g_new_entity_label.c_str(), g_new_entity_result, g_new_entity_json.size());
    poc_log(l);
}

/* Sound names are validated by the backend catalog before live playback. */
/* Establish the preview session before draining its first play request. */
static void poc_apply_sound_session()
{
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->sound_session) return;
    __try { g_iface->vtbl->sound_session(g_iface, g_sound_session_on); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void poc_apply_sound_preview()
{
    g_sound_preview_result = 0;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->sound_preview) {
        poc_log("sound-preview: ABORT (iface/slot missing -- backend too old?)");
        return;
    }
    const char *n = g_sound_preview_name.empty() ? nullptr : g_sound_preview_name.c_str();
    __try { g_sound_preview_result = g_iface->vtbl->sound_preview(g_iface, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_sound_preview_result = 0; }
}

static void poc_apply_load_prefab()
{
    g_load_result = -1;
    if (g_load_prefab_name.empty()) return;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->apply_edit) { poc_log("load-prefab: ABORT (iface/slot missing)"); return; }
    char path[1024];
    if (!poc_prefab_file_path(g_load_prefab_folder, g_load_prefab_name, path, (int)sizeof path)) {
        poc_log("load-prefab: ABORT (resolve_prefab_path failed)");
        return;
    }
    FILE *fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) { poc_log("load-prefab: ABORT (fopen failed)"); return; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::string body;
    if (sz > 0) { body.resize((size_t)sz); size_t got = fread(&body[0], 1, (size_t)sz, fp); body.resize(got); }
    fclose(fp);
    if (body.empty()) { poc_log("load-prefab: ABORT (empty file)"); return; }

    /* Paste uses selection as its old-to-new ID map. Clear it first; the backend
     * rechecks before placement because a pending manipulation may refuse clearing. */
    poc_clear_selection_seh();

    /* kind=2 stages and requests the engine's native paste action on its main thread.
     * Missing editor prerequisites can leave the prefab staged for manual paste. */
    /* Staged prefab storage must survive map-heap resets; the backend allocates it
     * in heap 0. Keep placement on the native action path rather than directly
     * invoking PasteInstantiate or synthesizing operating-system keystrokes. */
    sh_apply_item it; it.kind = 2; it.id = 0; it.text = body.c_str();
    g_load_result = poc_apply_edit_seh(&it, 1, "load-prefab");
    char l[300]; _snprintf_s(l, sizeof l, _TRUNCATE, "load-prefab: name='%s' staged=%d (kind=2: stage + auto pick-up)",
                             g_load_prefab_name.c_str(), g_load_result);
    poc_log(l);
}
/* Commit the page's refreshed, patched entity JSON with kind=0. Prefer the
 * synchronous applied result; see poc_apply_sync_seh for the older-backend fallback. */
static void poc_apply_save_timeline()
{
    g_save_timeline_result = 0;
    if (g_save_timeline_eid < 0 || g_save_timeline_json.empty()) return;
    if (!g_iface || !g_iface->vtbl || (!g_iface->vtbl->apply_sync && !g_iface->vtbl->apply_edit)) {
        poc_log("save-timeline: ABORT (iface/slot missing)"); return;
    }
    sh_apply_item it; it.kind = 0; it.id = g_save_timeline_eid; it.text = g_save_timeline_json.c_str();
    g_save_timeline_result = poc_apply_sync_seh(&it, 1, "save-timeline");
    char l[128]; _snprintf_s(l, sizeof l, _TRUNCATE, "save-timeline: eid=%d ok=%d", g_save_timeline_eid, g_save_timeline_result);
    poc_log(l);
}
/* Move a prefab from one folder to another (drag-and-drop target): MoveFileA across the two resolved
 * paths. Refuses to overwrite an existing same-named file at the destination (JS pre-checks too). */
static void poc_apply_move_prefab()
{
    g_move_prefab_result = -1;
    if (g_move_prefab_name.empty()) return;
    char oldp[1024], newp[1024];
    if (!poc_prefab_file_path(g_move_prefab_from, g_move_prefab_name, oldp, (int)sizeof oldp)) return;
    if (!poc_prefab_file_path(g_move_prefab_to, g_move_prefab_name, newp, (int)sizeof newp)) return;
    g_move_prefab_result = MoveFileA(oldp, newp) ? 1 : 0;
    if (g_move_prefab_result == 1) {   /* the sidecar follows the move; absent is fine */
        char oldm[1024], newm[1024];
        if (poc_prefab_meta_path(g_move_prefab_from, g_move_prefab_name, oldm, (int)sizeof oldm) &&
            poc_prefab_meta_path(g_move_prefab_to, g_move_prefab_name, newm, (int)sizeof newm))
            MoveFileA(oldm, newm);
    }
}
/* Create a real subdirectory under prefabs\. result: 1 created, 0 empty name / already exists, -1 failed. */
static void poc_apply_create_folder()
{
    g_create_folder_result = -1;
    if (g_create_folder_name.empty()) { g_create_folder_result = 0; return; }
    char dir[1024];
    if (!poc_prefab_dir(g_create_folder_name, dir, (int)sizeof dir)) return;
    poc_strip_trailing_sep(dir);
    if (CreateDirectoryA(dir, nullptr)) { g_create_folder_result = 1; return; }
    g_create_folder_result = (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
}
/* Rename a folder: MoveFileA also renames directories. result: 1 ok, 0 dest exists/locked, -1 resolve failed. */
static void poc_apply_rename_folder()
{
    g_rename_folder_result = -1;
    if (g_rename_folder_old.empty() || g_rename_folder_new.empty()) return;
    char oldd[1024], newd[1024];
    if (!poc_prefab_dir(g_rename_folder_old, oldd, (int)sizeof oldd)) return;
    if (!poc_prefab_dir(g_rename_folder_new, newd, (int)sizeof newd)) return;
    poc_strip_trailing_sep(oldd); poc_strip_trailing_sep(newd);
    g_rename_folder_result = MoveFileA(oldd, newd) ? 1 : 0;
}
/* Move contents to the prefab root before removing a folder. A name collision
 * leaves the source file in place and removal fails without overwriting data. */
static void poc_apply_delete_folder()
{
    g_delete_folder_result = -1;
    if (g_delete_folder_name.empty()) return;
    char dir[1024], rootDir[1024];
    if (!poc_prefab_dir(g_delete_folder_name, dir, (int)sizeof dir)) return;
    if (!poc_prefab_dir("", rootDir, (int)sizeof rootDir)) return;
    std::vector<std::string> items;
    poc_list_json_dir(dir, items);
    for (size_t i = 0; i < items.size(); i++) {
        std::string src = std::string(dir) + items[i] + ".json";
        std::string dst = std::string(rootDir) + items[i] + ".json";
        MoveFileA(src.c_str(), dst.c_str());
    }
    poc_strip_trailing_sep(dir);
    g_delete_folder_result = RemoveDirectoryA(dir) ? 1 : 0;
}
/* Run enum_inherits (+0x278) or enum_valid_classes (+0x270) into buf; returns 1 + *pcount packed strings
 * (consecutive NUL-terminated, double-NUL end). SEH-guarded, no C++ objects. */
static int poc_run_enum(int classes, const char *inherit, char *buf, int cap, int *pcount)
{
    int r = 0; *pcount = 0; if (cap >= 2) { buf[0] = 0; buf[1] = 0; }
    __try {
        if (g_iface && g_iface->vtbl) {
            if (classes) { if (g_iface->vtbl->enum_valid_classes) r = g_iface->vtbl->enum_valid_classes(g_iface, inherit ? inherit : "", buf, cap, pcount); }
            else         { if (g_iface->vtbl->enum_inherits)      r = g_iface->vtbl->enum_inherits(g_iface, buf, cap, pcount); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; *pcount = 0; }
    return r;
}

/* Messages to the page. */
static uint64_t poc_list_sig(int n)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) { h = hint(h, g_ents[i].eid); h = hstr(h, g_ents[i].id); h = hstr(h, g_ents[i].name); h = hint(h, g_ents[i].hidden); }
    return h ^ (uint64_t)n;
}
static void poc_emit_list(int n, int ready, const char *reason)
{
    if (!g_webview || !g_ents) return;
    unsigned long long started = poc_perf_now_us();
    unsigned long seq = ++g_list_seq;
    std::wstring json; json.reserve((size_t)(n + g_tl_count) * 96 + 96);
    json += L"{\"kind\":\"list\",\"seq\":"; json += std::to_wstring(seq);
    json += L",\"version\":\""; json += sh_webview_json::escape_wide(g_version.c_str());
    /* the renderer rides the list message alongside the version, for the same reason: the feedback
     * dialog composes its own payload in the page and needs both there. */
    json += L"\",\"renderer\":\""; json += sh_webview_json::escape_wide(sh_host_renderer_name());
    json += L"\",\"editorReady\":"; json += ready ? L"true" : L"false";
    json += L",\"count\":"; json += std::to_wstring(n);
    json += L",\"entities\":[";
    for (int i = 0; i < n; i++) {
        if (i) json += L",";
        json += L"{\"eid\":"; json += std::to_wstring(g_ents[i].eid);
        json += L",\"id\":\""; json += sh_webview_json::escape_wide(g_ents[i].id);
        json += L"\",\"name\":\""; json += sh_webview_json::escape_wide(g_ents[i].name);
        json += L"\",\"hidden\":"; json += g_ents[i].hidden ? L"true" : L"false";
        json += L"}";
    }
    json += L"],\"timelines\":[";
    for (int i = 0; i < g_tl_count; i++) {
        if (i) json += L",";
        json += L"{\"eid\":"; json += std::to_wstring(g_tls[i].eid);
        json += L",\"id\":\""; json += sh_webview_json::escape_wide(g_tls[i].id);
        json += L"\",\"name\":\""; json += sh_webview_json::escape_wide(g_tls[i].name);
        json += L"\"}";   /* close the "name" string BEFORE the object brace (the missing \" was the empty-list bug) */
    }
    json += L"]}";
    unsigned long long built = poc_perf_now_us();
    HRESULT hr = g_webview->PostWebMessageAsJson(json.c_str());
    unsigned long long posted = poc_perf_now_us();
    char l[320];
    _snprintf_s(l, sizeof l, _TRUNCATE,
                "perf list-emit: seq=%lu reason=%s entities=%d timelines=%d json_chars=%llu build_us=%llu post_us=%llu hr=0x%08lx",
                seq, reason ? reason : "unknown", n, g_tl_count,
                (unsigned long long)json.size(), built >= started ? built - started : 0,
                posted >= built ? posted - built : 0, (unsigned long)hr);
    poc_log(l);
}
static void poc_send_list(const char *reason)
{
    int ready = 0; int n = poc_collect_timed(&ready, reason);
    uint64_t sig = poc_list_sig(n);
    /* Rebuild Timelines only when the entity signature changes. */
    if (sig != g_last_list_sig) poc_rescan_timelines_timed(n, reason);
    g_last_list_sig = sig;
    poc_emit_list(n, ready, reason);
}
static void poc_send_state(int id, bool autoflag)
{
    if (!g_webview) return;
    char cls[512], inh[512], dnm[512]; bool truncated = false;
    bool ok = poc_collect_state_growing(id, cls, sizeof cls, inh, sizeof inh, dnm, sizeof dnm, &truncated);
    const char *decl = g_state_decl.empty() ? "" : g_state_decl.data();
    uint64_t sig = hstr(hstr(hstr(hstr(1469598103934665603ull, decl), cls), inh), dnm) ^ (uint64_t)id;
    if (autoflag && sig == g_last_state_sig) return;   /* nothing changed -> skip the auto push */
    g_last_state_sig = sig;
    std::wstring json = L"{\"kind\":\"state\",\"auto\":"; json += autoflag ? L"true" : L"false";
    json += L",\"eid\":"; json += std::to_wstring(id);
    json += L",\"ok\":"; json += ok ? L"true" : L"false";
    json += L",\"truncated\":"; json += truncated ? L"true" : L"false";
    json += L",\"decl\":\"";        if (!truncated) json += sh_webview_json::escape_wide(decl); json += L"\"";
    json += L",\"classname\":\"";   json += sh_webview_json::escape_wide(cls);  json += L"\"";
    json += L",\"inherit\":\"";     json += sh_webview_json::escape_wide(inh);  json += L"\"";
    json += L",\"displayname\":\""; json += sh_webview_json::escape_wide(dnm);  json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Build + send the valid-values list for a datalist: {kind:"inherits"|"classes", inherit?, items:[...]}. */
static void poc_send_enum(int classes, const char *inherit)
{
    if (!g_webview) return;
    int count = 0;
    poc_run_enum(classes, inherit, g_enumbuf, (int)sizeof g_enumbuf, &count);
    if (count < 0) count = 0;
    std::wstring json = classes ? L"{\"kind\":\"classes\",\"inherit\":\"" : L"{\"kind\":\"inherits\"";
    if (classes) { json += sh_webview_json::escape_wide(inherit ? inherit : ""); json += L"\""; }
    json += L",\"items\":[";
    const char *p = g_enumbuf;
    const char *end = g_enumbuf + sizeof g_enumbuf;
    int emitted = 0;
    for (int i = 0; i < count && p < end; i++) {
        size_t len = strnlen(p, (size_t)(end - p));
        if (len > 0) {
            if (emitted) json += L",";
            std::string s(p, len);
            json += L"\""; json += sh_webview_json::escape_wide(s.c_str()); json += L"\"";
            emitted++;
        }
        p += len + 1;
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Keep SEH in a POD-only helper: MSVC forbids __try alongside C++ locals
 * that require unwinding. */
static int poc_run_arg_resclass(const char *resClass, char *buf, int cap, int *pcount)
{
    int r = 0; *pcount = 0; if (cap >= 2) { buf[0] = 0; buf[1] = 0; }
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->enum_decls_of_resclass && resClass && resClass[0])
            r = g_iface->vtbl->enum_decls_of_resclass(g_iface, resClass, buf, cap, pcount);
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; *pcount = 0; }
    return r;
}
/* Query declaration classes by short name and enum types by catalog name.
 * Empty results leave the page with an editable text field. */
static void poc_send_arg_resclass(const char *resClass)
{
    if (!g_webview) return;
    int count = 0;
    poc_run_arg_resclass(resClass, g_enumbuf, (int)sizeof g_enumbuf, &count);
    if (count < 0) count = 0;
    std::wstring json = L"{\"kind\":\"argResclass\",\"resClass\":\""; json += sh_webview_json::escape_wide(resClass ? resClass : ""); json += L"\",\"items\":[";
    const char *p = g_enumbuf;
    const char *end = g_enumbuf + sizeof g_enumbuf;
    int emitted = 0;
    for (int i = 0; i < count && p < end; i++) {
        size_t len = strnlen(p, (size_t)(end - p));
        if (len > 0) {
            if (emitted) json += L",";
            std::string s(p, len);
            json += L"\""; json += sh_webview_json::escape_wide(s.c_str()); json += L"\"";
            emitted++;
        }
        p += len + 1;
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Look up entity descriptions in the generated table. */
static const ShEntDesc *poc_lookup_desc(const std::string &name)
{
    static std::map<std::string, const ShEntDesc *> *idx = nullptr;
    if (!idx) {
        idx = new std::map<std::string, const ShEntDesc *>();
        for (int i = 0; i < SH_ENTITY_DESCS_N; i++) (*idx)[SH_ENTITY_DESCS[i].name] = &SH_ENTITY_DESCS[i];
    }
    if (name.empty()) return nullptr;
    auto it = idx->find(name);
    return it == idx->end() ? nullptr : it->second;
}
static void poc_json_desc_obj(std::wstring &json, const ShEntDesc *d)
{
    if (!d) { json += L"null"; return; }
    json += L"{\"summary\":\"";    json += sh_webview_json::escape_wide(d->summary);    json += L"\"";
    json += L",\"confidence\":\""; json += sh_webview_json::escape_wide(d->confidence); json += L"\"";
    json += L",\"source\":\"";     json += sh_webview_json::escape_wide(d->source);     json += L"\"}";
}
static void poc_send_desc(const char *inherit, const char *classname)
{
    if (!g_webview) return;
    std::wstring json = L"{\"kind\":\"desc\",\"inherit\":";
    poc_json_desc_obj(json, poc_lookup_desc(inherit ? inherit : ""));
    json += L",\"class\":";
    poc_json_desc_obj(json, poc_lookup_desc(classname ? classname : ""));
    json += L"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* Resolve a material and return the metadata supplied by the backend. */
static void poc_send_material_result(const char *name)
{
    if (!g_webview) return;
    char info[256] = "";
    int found = 0;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->find_material && name && name[0]) {
        found = g_iface->vtbl->find_material(g_iface, name, info, (int)sizeof info);
    }
    std::wstring json = L"{\"kind\":\"materialResult\",\"name\":\"";
    json += sh_webview_json::escape_wide(name ? name : "");
    json += L"\",\"found\":"; json += found ? L"true" : L"false";
    json += L",\"info\":\""; json += sh_webview_json::escape_wide(info); json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* The File menu's rawmap I/O: "Load Rawmap" / "Save Rawmap As", the only caller of
 * the +0x328/+0x330 slots. Load STAGES a file -- the swap substitutes it into the
 * engine's next map-load parse -- so every string below says "staged", not "loaded".
 *
 * The picker runs on its own thread. This handler is dispatched from DispatchMessageW
 * inside poc_think_loop, so a modal Show() here would stop editor polling, selection
 * sync and the queued-work drain for as long as the dialog is open. The result comes
 * back through the same pending-flag handoff every other deferred action uses, so
 * every backend call still happens on the UI thread. */

/* Run the common item dialog. `save` picks the Save-As variant. Returns false when the person
 * cancelled (the overwhelmingly common non-success case, and not an error worth reporting).
 * Call from the picker thread, never from the message handler. */
static bool poc_pick_rawmap_file(bool save, const wchar_t *title, std::wstring &out_path)
{
    out_path.clear();

    /* The UI thread's apartment is already initialized by the WebView host, so this is a nesting
     * no-op that we must still balance -- and RPC_E_CHANGED_MODE means someone else set a different
     * apartment, which is fine for a modal dialog and must NOT be treated as failure. */
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool need_uninit = SUCCEEDED(init);

    IFileDialog *dlg = nullptr;
    HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg) {
        COMDLG_FILTERSPEC filters[] = {
            { L"Rawmap JSON (*.json)", L"*.json" },
            { L"All files (*.*)",      L"*.*"    },
        };
        dlg->SetFileTypes(ARRAYSIZE(filters), filters);
        dlg->SetFileTypeIndex(1);
        dlg->SetDefaultExtension(L"json");
        if (title) dlg->SetTitle(title);
        if (save) dlg->SetFileName(L"rawmap.json");

        /* Keep the dialog's own work down. FORCEFILESYSTEM refuses items with no path (nothing we
         * could open anyway); NOCHANGEDIR keeps it from moving the process's working directory out
         * from under DOOM; DONTADDTORECENT skips a write to the shell's recent-items store. */
        {
            FILEOPENDIALOGOPTIONS opt = 0;
            if (SUCCEEDED(dlg->GetOptions(&opt)))
                dlg->SetOptions(opt | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
        }

        /* Our own slot for the dialog's persisted state, so what it remembers is OURS and cannot be
         * inherited from whatever other dialog in this process was shown last. */
        {
            static const GUID kRawmapPickerGuid =
                { 0x7b3a9c41, 0x5e28, 0x4d6b, { 0x9f, 0x14, 0x2c, 0x8a, 0xde, 0x61, 0x3f, 0x70 } };
            dlg->SetClientGuid(kRawmapPickerGuid);
        }

        /* Open on the rawmap folder rather than wherever the shell last was.
         *
         * SetFolder, NOT SetDefaultFolder -- and this is a correction of the line that used to be
         * here. SetDefaultFolder applies only when the dialog has NO remembered location, and it had
         * one, so the dialog kept opening in Quick Access instead. That was two reported problems
         * with one cause: the folder we set looked ignored, and the dialog was slow to appear,
         * because Quick Access is the slowest possible starting point -- it enumerates cloud
         * providers and network places, and on a machine whose Documents is OneDrive-redirected that
         * is a network round trip before anything gets drawn.
         *
         * SetDefaultFolder is kept too, for the first-ever open and for a path that does not exist
         * yet; SetFolder wins when both are set. Overriding a remembered location is the intent:
         * this is the app's own data folder and it is where rawmaps live. */
        {
            wchar_t dir[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir))) {
                wcsncat_s(dir, L"\\snapmap-plus", _TRUNCATE);
                CreateDirectoryW(dir, nullptr);        /* harmless if it already exists */
                IShellItem *folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(dir, nullptr, IID_PPV_ARGS(&folder))) && folder) {
                    dlg->SetDefaultFolder(folder);
                    dlg->SetFolder(folder);
                    folder->Release();
                }
            }
        }

        hr = dlg->Show(g_hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR wide = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide)) && wide) {
                    out_path = wide;
                    CoTaskMemFree(wide);
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    if (need_uninit) CoUninitialize();
    return !out_path.empty();
}

/* Report the staged paths + arm state. The backend hands back a JSON fragment (paths are escaped
 * there -- a Windows path is full of backslashes), so this forwards it whole rather than re-encoding
 * field by field. An absent slot means an older backend: report it instead of guessing.
 *
 * `confirm_file`, when set, asks the page to put the "load it now, or save your work first?" question
 * up before anything opens. It rides on the status message because the file has ALREADY been staged
 * by the time we ask -- the answer only decides whether the reload runs now or waits for File >
 * Open Rawmap as New Map, so there is nothing to hold on to native-side while the person decides. It must
 * arrive already JSON-escaped (escape_wide), like every other path that crosses this boundary. */
/* Plain UTF-8 -> wide, NO escaping. For text that is already JSON and must be
 * forwarded verbatim. */
static std::wstring poc_widen(const char *utf8)
{
    std::wstring w;
    if (!utf8) return w;
    int wl = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wl > 0) { w.resize(wl - 1); if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], wl); }
    return w;
}

static void poc_send_rawmap_status(const wchar_t *note, const wchar_t *confirm_file = nullptr)
{
    if (!g_webview) return;

    char status[2048] = "";
    bool have = false;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->rawmap_status) {
        have = g_iface->vtbl->rawmap_status(g_iface, status, (int)sizeof status) > 0;
    }

    std::wstring json = L"{\"kind\":\"rawmapStatus\",\"ok\":";
    json += have ? L"true" : L"false";
    if (have) {
        json += L",\"status\":";
        json += poc_widen(status);   /* already a JSON object -- forward it, do not escape it */
    }
    json += L",\"note\":\"";
    json += note ? note : L"";
    json += L"\"";
    if (confirm_file && *confirm_file) {
        json += L",\"confirmLoad\":\"";
        json += confirm_file;
        json += L"\"";
    }
    json += L"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* Ask the backend to point a side at a chosen file. `load_path`/`save_path` NULL = leave alone,
 * "" = restore that side's default. `arm` is 1/0/-1 as the slot documents. */
static void poc_rawmap_configure(const char *load_path, const char *save_path, int arm,
                                 const wchar_t *ok_note)
{
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->rawmap_configure) {
        poc_send_rawmap_status(L"This build's backend has no rawmap file surface.");
        return;
    }

    char msg[192] = "";
    int ok = g_iface->vtbl->rawmap_configure(g_iface, load_path, save_path, arm,
                                             msg, (int)sizeof msg);
    /* The backend's message is the useful one on refusal -- it names WHY the file was rejected
     * (wrong type, too big, unreadable). On success its "ok" is not worth showing, so the caller's
     * own sentence wins -- unless the caller passes no sentence, which is how Save Rawmap As asks for
     * the backend's own: only the backend knows whether it wrote the file there and then or is
     * waiting for an editor save, and how many bytes went out. */
    std::wstring note = ok ? (ok_note ? std::wstring(ok_note) : sh_webview_json::escape_wide(msg))
                           : (L"Refused: " + sh_webview_json::escape_wide(msg));

    /* poc_logf carries exactly one unsigned long, so compose the detail line first. The paths are
     * what makes this log worth having when someone reports "it did not load my file". */
    {
        char line[640];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "rawmap configure: ok=%d arm=%d load=%s save=%s msg=%s",
                    ok, arm,
                    load_path ? load_path : "(unchanged)",
                    save_path ? save_path : "(unchanged)", msg);
        poc_log(line);
    }
    poc_send_rawmap_status(note.c_str());
}

/* The picker thread. Its ONLY job is the modal dialog: it touches no backend slot and posts nothing
 * to the page, because both of those belong on the UI thread. It hands back a path and lets the think
 * loop do the rest. Its own apartment, since a fresh thread has none. */
static DWORD WINAPI poc_pick_thread(LPVOID param)
{
    const int kind = (int)(intptr_t)param;
    std::wstring picked;
    bool ok;

    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ok = poc_pick_rawmap_file(kind == 1, kind == 1 ? L"Save Rawmap As" : L"Load Rawmap", picked);
    if (SUCCEEDED(init)) CoUninitialize();

    g_pick_kind = kind;
    g_pick_ok   = ok;
    g_pick_path = picked;
    InterlockedExchange(&g_pick_done, 1);   /* last: publishes everything written above */
    return 0;
}

/* Open a picker, unless one is already open. Returns with the dialog still to come -- the caller has
 * nothing to report yet, which is the point: the message handler returns immediately and the think
 * loop keeps turning. */
static void poc_begin_pick(int kind)
{
    HANDLE h;

    if (InterlockedExchange(&g_pick_busy, 1) != 0) {
        poc_send_rawmap_status(L"A file dialog is already open.");
        return;
    }
    h = CreateThread(nullptr, 0, poc_pick_thread, (LPVOID)(intptr_t)kind, 0, nullptr);
    if (h == nullptr) {
        InterlockedExchange(&g_pick_busy, 0);
        poc_send_rawmap_status(L"Could not open the file dialog.");
        return;
    }
    CloseHandle(h);
}

/* Act on a finished picker. Runs on the UI thread from the think loop, so every backend call below
 * is where it has always been. */
static void poc_finish_pick()
{
    const int  kind   = g_pick_kind;
    const bool picked = g_pick_ok;
    std::string p8    = picked ? sh_webview_json::to_utf8(g_pick_path) : std::string();

    /* Kept before the clear below, for the confirm prompt: the person should see WHICH file they are
     * about to open, and the full path is too long for a dialog line. */
    std::wstring base;
    if (picked) {
        size_t cut = g_pick_path.find_last_of(L"\\/");
        base = (cut == std::wstring::npos) ? g_pick_path : g_pick_path.substr(cut + 1);
    }

    g_pick_path.clear();
    InterlockedExchange(&g_pick_done, 0);
    InterlockedExchange(&g_pick_busy, 0);

    if (!picked) {
        poc_send_rawmap_status(L"");   /* cancelled -- refresh the readout, say nothing */
        return;
    }

    if (kind == 1) {
        /* Naming a destination IS the request to write it -- the configure slot writes the file there
         * and then from the map's own save on disk, and only falls back to arming one editor save when
         * the map has never been saved. No note of our own: only the backend knows which happened. */
        poc_rawmap_configure(nullptr, p8.c_str(), -1, nullptr);
        return;
    }

    /* STAGE + ARM ONE LOAD (arm = 2). Not the shared gate: setting that left every later map load
     * substituted as well, and the save shadow rides the same gate, so picking a file to LOAD also
     * redirected the next save. A one-shot is what the click actually means -- apply this file to
     * the map I am about to open, and then stop. It spends itself on the substitution, so a load
     * that cannot read the staged file leaves the arm intact rather than silently eating it. */
    {
        char msg[192] = "";
        int staged = 0;
        if (g_iface && g_iface->vtbl && g_iface->vtbl->rawmap_configure) {
            staged = g_iface->vtbl->rawmap_configure(g_iface, p8.c_str(), nullptr, 2,
                                                     msg, (int)sizeof msg);
        }
        if (!staged) {
            poc_send_rawmap_status((L"Refused: " + sh_webview_json::escape_wide(msg)).c_str());
        } else {
            /* Ask before opening: a reload discards whatever is in the editor, and the engine
             * offers no "has unsaved changes" query. Answering no leaves the file staged. The
             * page owns this dialog on purpose -- a native modal here stalls the think loop. */
            poc_send_rawmap_status(L"", base.c_str());
        }
    }
}

/* Fetch the latest worker-published PNG data URI. Probe its size before copying;
 * no rendering or engine access occurs here. */
static void poc_send_preview(const char *name)
{
    if (!g_webview) return;
    std::string uri;
    int rc = 0;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->get_preview) {
        char probe[8] = "";
        rc = g_iface->vtbl->get_preview(g_iface, probe, (int)sizeof probe);
        if (rc < 0) {                       /* -(required size) */
            size_t need = (size_t)(-rc);
            uri.resize(need);
            rc = g_iface->vtbl->get_preview(g_iface, &uri[0], (int)need);
            if (rc > 0) uri.resize((size_t)rc); else uri.clear();
        } else if (rc > 0) {
            uri.assign(probe, (size_t)rc);  /* implausibly small, but handle it */
        }
    }
    std::wstring json = L"{\"kind\":\"previewImage\",\"ok\":";
    json += uri.empty() ? L"false" : L"true";
    json += L",\"uri\":\"";
    json += sh_webview_json::escape_wide(uri.c_str());
    json += L"\",\"name\":\"";
    json += sh_webview_json::escape_wide(name ? name : "");
    json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* Queue preview decoding on the backend worker. The page polls for publication. */
static void poc_request_preview(const char *name, int asset_kind)
{
    if (!g_webview) return;
    int ok = 0;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->request_preview && name && *name) {
        std::string request_name;
        const char *request = name;
        if (asset_kind == SH_ASSET_IMAGE) {
            request_name = SH_PREVIEW_IMAGE_ROUTE_PREFIX;
            request_name += name;
            request = request_name.c_str();
        }
        ok = g_iface->vtbl->request_preview(g_iface, request);
    }

    std::wstring json = L"{\"kind\":\"previewRequested\",\"ok\":";
    json += ok ? L"true" : L"false";
    json += L",\"name\":\"";
    json += sh_webview_json::escape_wide(name ? name : "");
    json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

static void poc_cancel_preview()
{
    if (g_iface && g_iface->vtbl && g_iface->vtbl->request_preview)
        g_iface->vtbl->request_preview(g_iface, "");
}

/* Collect catalog pages into one response for the page's bounded cache.
 * Older backends can still supply materials through list_materials. */
static void poc_send_asset_list(int kind)
{
    if (!g_webview) return;
    std::string all;
    int total = 0;
    if (g_iface && g_iface->vtbl) {
        std::string chunk(64 * 1024, '\0');
        for (int guard = 0; guard < 512; ++guard) {      /* guard: never spin if the backend lies */
            int n = 0;
            if (g_iface->vtbl->list_assets)
                n = g_iface->vtbl->list_assets(g_iface, kind, total, &chunk[0], (int)chunk.size());
            else if (kind == SH_ASSET_MATERIAL && g_iface->vtbl->list_materials)
                n = g_iface->vtbl->list_materials(g_iface, total, &chunk[0], (int)chunk.size());
            if (n <= 0) break;
            all += chunk.c_str();                        /* NUL-terminated, newline-separated */
            total += n;
        }
    }
    std::wstring json = L"{\"kind\":\"assetList\",\"assetKind\":";
    json += std::to_wstring(kind);
    json += L",\"count\":";
    json += std::to_wstring(total);
    json += L",\"names\":\"";
    json += sh_webview_json::escape_wide(all.c_str());
    json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* Send the static event catalog and argument schemas for client-side filtering. */
static void poc_send_events()
{
    if (!g_webview) return;
    std::wstring json; json.reserve((size_t)SH_EVENT_CATALOG_N * 96 + 32);
    json = L"{\"kind\":\"events\",\"items\":[";
    for (int i = 0; i < SH_EVENT_CATALOG_N; i++) {
        if (i) json += L",";
        const ShEvtDef &d = SH_EVENT_CATALOG[i];
        json += L"{\"name\":\""; json += sh_webview_json::escape_wide(d.name); json += L"\",\"args\":[";
        for (int a = 0; a < d.argc; a++) {
            if (a) json += L",";
            const ShEvtArg &arg = d.args[a];
            json += L"{\"name\":\""; json += sh_webview_json::escape_wide(arg.name ? arg.name : ""); json += L"\"";
            json += L",\"type\":\""; json += sh_webview_json::escape_wide(arg.type ? arg.type : ""); json += L"\"}";
        }
        json += L"]}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Send generated event and argument descriptions lazily when requested. The
 * page caches this larger payload separately from the event catalog. */
static void poc_send_event_docs()
{
    if (!g_webview) return;
    std::wstring json; json.reserve((size_t)SH_EVENT_DOCS_N * 192 + 32);
    json = L"{\"kind\":\"eventDocs\",\"items\":[";
    for (int i = 0; i < SH_EVENT_DOCS_N; i++) {
        if (i) json += L",";
        const ShEvtDoc &d = SH_EVENT_DOCS[i];
        json += L"{\"name\":\""; json += sh_webview_json::escape_wide(d.name ? d.name : ""); json += L"\"";
        json += L",\"summary\":\""; json += sh_webview_json::escape_wide(d.summary ? d.summary : ""); json += L"\"";
        json += L",\"confidence\":\""; json += sh_webview_json::escape_wide(d.confidence ? d.confidence : ""); json += L"\"";
        json += L",\"source\":\""; json += sh_webview_json::escape_wide(d.source ? d.source : ""); json += L"\"";
        json += L",\"args\":[";
        for (int a = 0; a < d.nArgs; a++) {
            if (a) json += L",";
            const ShEvtArgDoc &ad = d.args[a];
            json += L"{\"name\":\""; json += sh_webview_json::escape_wide(ad.name ? ad.name : ""); json += L"\"";
            json += L",\"desc\":\""; json += sh_webview_json::escape_wide(ad.desc ? ad.desc : ""); json += L"\"}";
        }
        json += L"]}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
static void poc_json_asset_items(std::wstring &json, const ShAssetItem *items, int n)
{
    json += L"[";
    for (int i = 0; i < n; i++) {
        if (i) json += L",";
        json += L"{\"display\":\""; json += sh_webview_json::escape_wide(items[i].display ? items[i].display : ""); json += L"\"";
        json += L",\"value\":\"";   json += sh_webview_json::escape_wide(items[i].value   ? items[i].value   : ""); json += L"\"}";
    }
    json += L"]";
}
/* Send generated per-class asset choices. Resolving an event target's inherit
 * uses the separate entity-serialization request. */
static void poc_send_entity_assets()
{
    if (!g_webview) return;
    std::wstring json; json.reserve((size_t)SH_ENTITY_ASSETS_N * 512 + 32);
    json = L"{\"kind\":\"entityAssets\",\"items\":[";
    for (int i = 0; i < SH_ENTITY_ASSETS_N; i++) {
        if (i) json += L",";
        const ShEntityAssets &ea = SH_ENTITY_ASSETS[i];
        json += L"{\"slug\":\""; json += sh_webview_json::escape_wide(ea.slug ? ea.slug : ""); json += L"\"";
        json += L",\"models\":";    poc_json_asset_items(json, ea.models,    ea.nModels);
        json += L",\"animWeb\":";   poc_json_asset_items(json, ea.animWeb,   ea.nAnimWeb);
        json += L",\"md6Anim\":";   poc_json_asset_items(json, ea.md6Anim,   ea.nMd6Anim);
        json += L",\"animAlias\":"; poc_json_asset_items(json, ea.animAlias, ea.nAnimAlias);
        json += L",\"tagName\":";   poc_json_asset_items(json, ea.tagName,   ea.nTagName);
        json += L"}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
static void poc_post_json(const wchar_t *json) { if (g_webview) g_webview->PostWebMessageAsJson(json); }

/* Move at most one decoded prefab mesh per UI tick. WebView2 shared buffers avoid base64's extra copy
 * and 4/3 expansion; JavaScript uploads from the ArrayBuffer and releases it immediately. If an older
 * runtime lacks the shared-buffer interfaces, consume the result and keep the honest procedural proxy. */
static void poc_send_prefab_mesh_completion()
{
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->get_prefab_mesh) return;
    int query = g_iface->vtbl->get_prefab_mesh(g_iface, nullptr, 0);
    if (query >= 0) return;
    int needed = -query;
    if (needed <= 0 || needed > 16 * 1024 * 1024) return;

    if (!g_webview_environment12 || !g_webview17) {
        std::vector<unsigned char> discard((size_t)needed);
        g_iface->vtbl->get_prefab_mesh(g_iface, discard.data(), needed);
        poc_post_json(L"{\"kind\":\"prefabMeshTransportUnavailable\"}");
        return;
    }

    ComPtr<ICoreWebView2SharedBuffer> shared;
    if (FAILED(g_webview_environment12->CreateSharedBuffer((UINT64)needed, shared.GetAddressOf())) || !shared) {
        std::vector<unsigned char> discard((size_t)needed);
        g_iface->vtbl->get_prefab_mesh(g_iface, discard.data(), needed);
        poc_post_json(L"{\"kind\":\"prefabMeshTransportUnavailable\"}");
        return;
    }
    BYTE *dst = nullptr;
    if (FAILED(shared->get_Buffer(&dst)) || !dst) {
        std::vector<unsigned char> discard((size_t)needed);
        g_iface->vtbl->get_prefab_mesh(g_iface, discard.data(), needed);
        poc_post_json(L"{\"kind\":\"prefabMeshTransportUnavailable\"}");
        return;
    }
    if (g_iface->vtbl->get_prefab_mesh(g_iface, dst, needed) != needed) {
        poc_post_json(L"{\"kind\":\"prefabMeshTransportUnavailable\"}");
        return;
    }

    std::wstring meta = L"{\"kind\":\"prefabMesh\",\"bytes\":";
    meta += std::to_wstring(needed); meta += L"}";
    if (FAILED(g_webview17->PostSharedBufferToScript(shared.Get(),
                                                     COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
                                                     meta.c_str())))
        poc_post_json(L"{\"kind\":\"prefabMeshTransportUnavailable\"}");
}

static void poc_post_window_state(HWND hwnd)
{
    if (!hwnd) return;
    poc_post_json(IsZoomed(hwnd)
        ? L"{\"kind\":\"windowState\",\"maximized\":true}"
        : L"{\"kind\":\"windowState\",\"maximized\":false}");
}

/* The two append-only config slots transport canonical JSON fragments. The getter is deliberately
 * retried because another process can replace config.json between its size query and copy call. */
static int poc_config_get_json(const std::string &key, std::string &value,
                               unsigned int *out_flags)
{
    unsigned int flags = 0;
    value.clear();
    if (out_flags) *out_flags = 0;
    if (key.empty() || key.size() > POC_CONFIG_KEY_CAP ||
        !g_iface || !g_iface->vtbl || !g_iface->vtbl->config_get_json)
        return -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        int needed = g_iface->vtbl->config_get_json(
            g_iface, key.c_str(), nullptr, 0, &flags);
        if (out_flags) *out_flags = flags;
        if (needed < 0 || (unsigned int)needed > POC_CONFIG_VALUE_CAP)
            return -1;
        std::vector<char> buffer((size_t)needed + 1u, 0);
        int got = g_iface->vtbl->config_get_json(
            g_iface, key.c_str(), buffer.data(), (int)buffer.size(), &flags);
        if (out_flags) *out_flags = flags;
        if (got >= 0 && (size_t)got < buffer.size()) {
            value.assign(buffer.data(), (size_t)got);
            return got;
        }
        if (got < 0 || (unsigned int)got > POC_CONFIG_VALUE_CAP)
            return -1;
    }
    return -1;
}

static void poc_post_config_value(const std::string &key)
{
    std::string value;
    unsigned int flags = 0;
    int got = poc_config_get_json(key, value, &flags);
    g_config_status_flags |= flags;
    std::wstring message = L"{\"kind\":\"configValue\",\"key\":\"";
    message += sh_webview_json::escape_wide(key.c_str());
    message += L"\",\"valueJson\":\"";
    if (got >= 0) message += sh_webview_json::escape_wide(value.c_str());
    message += L"\",\"result\":";
    message += got >= 0 ? L"1}" : L"0}";
    poc_post_json(message.c_str());
}

static void poc_post_config_set_result(const std::string &key, int result)
{
    std::wstring message = L"{\"kind\":\"configSetResult\",\"key\":\"";
    message += sh_webview_json::escape_wide(key.c_str());
    message += L"\",\"result\":";
    message += std::to_wstring(result);
    message += L"}";
    poc_post_json(message.c_str());
}

static void poc_post_config_status()
{
    if (g_config_status_posted) return;
    std::wstring message = L"{\"kind\":\"configStatus\",\"flags\":";
    message += std::to_wstring(g_config_status_flags);
    message += L"}";
    poc_post_json(message.c_str());
    g_config_status_posted = true;
}

/* Count exact idSnapEntity keys without counting the idSnapEntityPrefab root. */
static int poc_count_prefab_entities(const std::string &body)
{
    int entityCount = 0;
    size_t pos = 0;
    while ((pos = body.find("\"idSnapEntity\"", pos)) != std::string::npos) {
        entityCount++;
        pos += 14;
    }
    return entityCount;
}
/* Read a single prefab file (resolved via +0xc0) and push its scene plus aggregate entity count.
 * folder="" selects the prefab root. */
/* Read a bounded auxiliary file; return false for missing, oversized, or unreadable data. */
static bool poc_read_small_file(const char *path, std::string &body)
{
    FILE *fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    bool ok = false;
    if (sz > 0 && sz <= 64 * 1024) {
        body.resize((size_t)sz);
        size_t got = fread(&body[0], 1, (size_t)sz, fp);
        body.resize(got);
        ok = true;
    }
    fclose(fp);
    return ok;
}
static void poc_send_prefab_detail(const std::string &name, const std::string &folder)
{
    if (!g_webview) return;
    int entityCount = 0;
    bool ok = false;
    std::string metaBody;
    std::string sceneBody;
    if (!name.empty()) {
        char path[1024];
        if (poc_prefab_file_path(folder, name, path, (int)sizeof path)) {
            FILE *fp = nullptr;
            if (fopen_s(&fp, path, "rb") == 0 && fp) {
                fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
                /* The scene is sent only for the selected prefab. Cap it independently of the tiny
                 * metadata sidecar so a hand-dropped giant file cannot force a huge WebMessage allocation. */
                if (sz > 0 && sz <= 8 * 1024 * 1024) {
                    sceneBody.resize((size_t)sz);
                    size_t got = fread(&sceneBody[0], 1, (size_t)sz, fp);
                    sceneBody.resize(got);
                    if (got == (size_t)sz) {
                        entityCount = poc_count_prefab_entities(sceneBody);
                        ok = true;
                    }
                }
                fclose(fp);
            }
        }
        char mp[1024];
        /* the sidecar body ships as an ESCAPED STRING (the page JSON.parses it with a try/catch), never
         * spliced raw -- a hand-edited/truncated sidecar must not be able to invalidate this whole
         * message (PostWebMessageAsJson silently drops a malformed payload). */
        if (ok && poc_prefab_meta_path(folder, name, mp, (int)sizeof mp)) poc_read_small_file(mp, metaBody);
    }
    std::wstring json = L"{\"kind\":\"prefabDetail\",\"name\":\""; json += sh_webview_json::escape_wide(name.c_str());
    json += L"\",\"ok\":"; json += ok ? L"true" : L"false";
    json += L",\"meta\":\""; json += sh_webview_json::escape_wide(metaBody.c_str());
    json += L"\",\"scene\":\""; json += sh_webview_json::escape_wide(sceneBody.c_str());
    json += L"\",\"count\":"; json += std::to_wstring(entityCount);
    json += L"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* Extract flat sidecar tags and re-escape each value. Never splice sidecar
 * bytes into an outgoing JSON envelope. */
static bool poc_read_meta_tags(const std::string &folder, const std::string &name, std::vector<std::string> &tags)
{
    char path[1024];
    if (!poc_prefab_meta_path(folder, name, path, (int)sizeof path)) return false;
    std::string body;
    if (!poc_read_small_file(path, body)) return false;
    size_t k = body.find("\"tags\"");
    if (k == std::string::npos) return false;
    size_t lb = body.find('[', k); if (lb == std::string::npos) return false;
    size_t rb = body.find(']', lb); if (rb == std::string::npos) return false;   /* flat string array -- no nesting */
    size_t p = lb + 1;
    while (p < rb) {
        size_t q1 = body.find('"', p); if (q1 == std::string::npos || q1 >= rb) break;
        size_t q2 = q1 + 1;
        while (q2 < rb && body[q2] != '"') q2++;   /* the page strips quotes/backslashes from tags on save */
        if (q2 >= rb) break;
        if (q2 > q1 + 1) tags.push_back(body.substr(q1 + 1, q2 - q1 - 1));
        p = q2 + 1;
    }
    return true;
}

/* Enumerate root prefabs and one level of folders. Include sidecar tags in the
 * list response so filtering needs no per-prefab reads from the page. */
static void poc_send_prefabs()
{
    if (!g_webview) return;
    std::vector<std::string> rootNames;
    std::vector<std::pair<std::string, std::vector<std::string>>> folders;
    char rootDir[1024];
    if (poc_prefab_dir("", rootDir, (int)sizeof rootDir)) {
        poc_list_json_dir(rootDir, rootNames);

        std::vector<std::string> subdirs;
        std::string pattern = std::string(rootDir) + "*";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string dn = fd.cFileName;
                if (dn == "." || dn == "..") continue;
                subdirs.push_back(dn);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(subdirs.begin(), subdirs.end());
        for (size_t i = 0; i < subdirs.size(); i++) {
            std::vector<std::string> items;
            poc_list_json_dir(std::string(rootDir) + subdirs[i] + "\\", items);
            folders.push_back(std::make_pair(subdirs[i], items));
        }
    }
    std::wstring json = L"{\"kind\":\"prefabs\",\"root\":[";
    for (size_t i = 0; i < rootNames.size(); i++) {
        if (i) json += L",";
        json += L"\""; json += sh_webview_json::escape_wide(rootNames[i].c_str()); json += L"\"";
    }
    json += L"],\"folders\":[";
    for (size_t i = 0; i < folders.size(); i++) {
        if (i) json += L",";
        json += L"{\"name\":\""; json += sh_webview_json::escape_wide(folders[i].first.c_str()); json += L"\",\"items\":[";
        const std::vector<std::string> &items = folders[i].second;
        for (size_t j = 0; j < items.size(); j++) {
            if (j) json += L",";
            json += L"\""; json += sh_webview_json::escape_wide(items[j].c_str()); json += L"\"";
        }
        json += L"]}";
    }
    json += L"],\"meta\":{";
    bool firstMeta = true;
    auto emitTags = [&](const std::string &folder, const std::string &name) {
        std::vector<std::string> tags;
        if (!poc_read_meta_tags(folder, name, tags) || tags.empty()) return;
        if (!firstMeta) json += L","; firstMeta = false;
        std::string key = folder.empty() ? name : (folder + "/" + name);
        json += L"\""; json += sh_webview_json::escape_wide(key.c_str()); json += L"\":[";
        for (size_t t = 0; t < tags.size(); t++) {
            if (t) json += L",";
            json += L"\""; json += sh_webview_json::escape_wide(tags[t].c_str()); json += L"\"";
        }
        json += L"]";
    };
    for (size_t i = 0; i < rootNames.size(); i++) emitTags("", rootNames[i]);
    for (size_t i = 0; i < folders.size(); i++)
        for (size_t j = 0; j < folders[i].second.size(); j++) emitTags(folders[i].first, folders[i].second[j]);
    json += L"}}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* write (or, with an empty body, delete) a prefab's metadata sidecar. Runs directly in the message
 * callback like selectPrefab -- pure Win32 file I/O, no engine touch. Posts savePrefabMetaResult, then a
 * fresh prefabs list (the tag map feeds the list filter). */
static void poc_apply_save_prefab_meta(const std::string &name, const std::string &folder, const std::string &body)
{
    int result = -1;
    char path[1024];
    if (!name.empty() && poc_prefab_meta_path(folder, name, path, (int)sizeof path)) {
        if (body.empty()) {
            DeleteFileA(path);   /* no metadata left -> no sidecar (a missing file is already the goal state) */
            result = 1;
        } else {
            FILE *fp = nullptr;
            if (fopen_s(&fp, path, "wb") == 0 && fp) {
                size_t put = fwrite(body.data(), 1, body.size(), fp);
                fclose(fp);
                result = (put == body.size()) ? 1 : -1;
            }
        }
    }
    if (!g_webview) return;
    std::wstring m = L"{\"kind\":\"savePrefabMetaResult\",\"result\":"; m += std::to_wstring(result);
    m += L",\"name\":\""; m += sh_webview_json::escape_wide(name.c_str()); m += L"\"}";
    g_webview->PostWebMessageAsJson(m.c_str());
    if (result == 1) poc_send_prefabs();
}

/* Feedback submission. */
/* Network boundary: Send starts one HTTPS POST to the feedback relay, which
 * creates or updates a public GitHub issue. Payloads contain user-entered text,
 * version, renderer, and optional scrubbed log tails for crash reports.
 * Run the request on a short-lived worker so WinHTTP cannot block the STA UI.
 * The worker owns g_report_* while in flight; the loop posts its result to the page.
 * The relay in feedback/ owns the service contract. */
static const wchar_t *kReportHost = L"snapmap-plus-feedback.doom-snapmap.workers.dev";   /* the deployed relay (feedback/); unreachable -> red toast, nothing else */
static const wchar_t *kReportPath = L"/report";
#define REPORT_PAYLOAD_CAP (64 * 1024)

static volatile bool g_report_inflight = false;   /* one submit at a time (the page disables Send too) */
static volatile bool g_report_done     = false;   /* worker thread -> think loop handoff */
static volatile int  g_report_ok       = 0;
static int           g_report_number   = 0;       /* filed issue number (0 = unknown) */
static char          g_report_mode[16] = "";      /* "created" | "appended" (dedup comment) */
static std::string   g_report_payload;            /* owned by the worker thread while in flight */

/* find `"key":<int>` in the relay's small JSON response (same targeted-scan approach as sh_webview_json::get_int,
 * narrow-string flavor -- no JSON library) */
static int rp_scan_int(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}
static DWORD WINAPI report_thread(LPVOID)
{
    int ok = 0, number = 0; char mode[16] = "";
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    DWORD status = 0;
    do {
        ses = WinHttpOpen(L"SnapmapPlus", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) break;
        WinHttpSetTimeouts(ses, 10000, 10000, 10000, 15000);
        con = WinHttpConnect(ses, kReportHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!con) break;
        req = WinHttpOpenRequest(con, L"POST", kReportPath, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) break;
        if (!WinHttpSendRequest(req, L"Content-Type: application/json\r\n", (DWORD)-1,
                                (LPVOID)g_report_payload.data(), (DWORD)g_report_payload.size(),
                                (DWORD)g_report_payload.size(), 0)) break;
        if (!WinHttpReceiveResponse(req, nullptr)) break;
        DWORD sl = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
        char resp[4096]; DWORD got = 0, total = 0;
        while (total < sizeof resp - 1 &&
               WinHttpReadData(req, resp + total, (DWORD)(sizeof resp - 1 - total), &got) && got)
            total += got;
        resp[total] = 0;
        if (status == 200 && strstr(resp, "\"ok\":true")) {
            ok = 1;
            number = rp_scan_int(resp, "\"number\"");
            strcpy_s(mode, sizeof mode, strstr(resp, "\"mode\":\"appended\"") ? "appended" : "created");
        }
    } while (0);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    poc_logf(ok ? "report: sent ok (http %lu)" : "report: send FAILED (http %lu)", (unsigned long)status);
    g_report_ok = ok; g_report_number = number;
    strcpy_s(g_report_mode, sizeof g_report_mode, mode[0] ? mode : "created");
    g_report_done = true;   /* the think loop posts reportResult to the page */
    return 0;
}

/* Crash-record presentation and reporting. */
/* Poll backend pending records about every two seconds. Terminal/unknown kinds
 * prompt for a report; classB/offthread records receive a notice only if newly
 * observed during this session. These kinds describe handling, not proof of survival.
 * Reports share the feedback relay and may attach scrubbed log tails. Dismissal
 * or successful submission clears pending records; full logs and dumps stay local. */
static const char *kCrashDir  = "snapmap-plus\\crash";   /* CWD = the game dir (poc_log's convention) */
static const char *kCrashGlob = "snapmap-plus\\crash\\pending-*.json";
#define CRASH_RECORD_READ_CAP  16384                      /* a record is ~2 KB; cap the read anyway */
#define CRASH_LOG_TAIL_KEEP    (15 * 1024)                /* per-log tail budget (3 logs ~= 45 KB) */
#define CRASH_RECORDS_KEEP     8                          /* newest records kept; older ones are pruned */

static bool        g_report_is_crash = false;   /* the in-flight relay POST came from the crash dialog */
static bool        g_page_loaded     = false;   /* NavigationCompleted fired -- the page can receive */
static std::string g_crash_last_sent;           /* terminal pending-*.json already raised as the dialog */
static sh_crash_pending::inventory g_crash_last_seen;

/* Sort pending filenames newest-first without opening records on every poll. */
static int crash_scan(std::vector<std::string> &names)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(kCrashGlob, &fd);
    names.clear();
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(), sh_crash_pending::newest_first);

    /* Bound retained diagnostics even when notices need no dismissal or submission. */
    if ((int)names.size() > CRASH_RECORDS_KEEP) {
        for (size_t i = CRASH_RECORDS_KEEP; i < names.size(); i++)
            DeleteFileA((std::string(kCrashDir) + "\\" + names[i]).c_str());
        names.resize(CRASH_RECORDS_KEEP);
    }
    return (int)names.size();
}

/* classB/offthread records describe attempted recovery or declined redirection,
 * so they do not establish process death. Other kinds prompt for a crash report.
 * Fatal capture is separate and best-effort; a missing fatal record cannot prove
 * that the process survived. Unknown or unreadable kinds default to prompting. */
static bool crash_record_is_terminal(const std::string &rec)
{
    const char *p = strstr(rec.c_str(), "\"kind\"");
    if (!p) return true;
    p += 6;
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return true;
    p++;
    return strncmp(p, "classB\"", 7) != 0 &&
           strncmp(p, "offthread\"", 10) != 0;
}

static std::string crash_read_record(const std::string &name)
{
    std::string path = std::string(kCrashDir) + "\\" + name, data;
    FILE *f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return data;
    char buf[4096]; size_t r;
    while (data.size() < CRASH_RECORD_READ_CAP && (r = fread(buf, 1, sizeof buf, f)) > 0)
        data.append(buf, r);
    fclose(f);
    /* the record must be one bare JSON object -- it is spliced verbatim into a crashPending message. */
    if (data.empty() || data.front() != '{' || data.back() != '}') data.clear();
    return data;
}

static void crash_clear_pending()
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(kCrashGlob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string path = std::string(kCrashDir) + "\\" + fd.cFileName;
        DeleteFileA(path.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Read bounded log tails, then scrub account, profile-folder, and machine names
 * before attachment. This targets those identifiers, not arbitrary personal data. */
static std::string crash_collect_logs()
{
    static const char *files[] = { "shield_faults.log", "sh_backend.log", "snapmap-plus-ui.log" };
    char user[64] = "", comp[64] = "", prof[MAX_PATH] = "";
    const char *profleaf = "";
    DWORD un = sizeof user, cn = sizeof comp;
    GetUserNameA(user, &un);
    GetComputerNameA(comp, &cn);
    if (GetEnvironmentVariableA("USERPROFILE", prof, MAX_PATH)) {
        const char *s = strrchr(prof, '\\');
        if (s) profleaf = s + 1;
    }
    std::string out;
    std::vector<char> raw(CRASH_LOG_TAIL_KEEP + 4096), scrub(2 * (CRASH_LOG_TAIL_KEEP + 4096));
    for (const char *fn : files) {
        std::string path = std::string("snapmap-plus\\logs\\") + fn;
        FILE *f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) continue;
        _fseeki64(f, 0, SEEK_END);
        long long size = _ftelli64(f);
        long long start = size > (long long)raw.size() ? size - (long long)raw.size() : 0;
        _fseeki64(f, start, SEEK_SET);
        size_t got = fread(raw.data(), 1, raw.size(), f);
        fclose(f);
        if (got == 0) continue;
        size_t off = rs_tail_offset(raw.data(), got, CRASH_LOG_TAIL_KEEP);
        std::string chunk(raw.data() + off, got - off);
        /* scrub passes: account name, profile-folder leaf (when different), machine name. */
        rs_scrub(scrub.data(), scrub.size(), chunk.c_str(), user, "<user>");
        chunk = scrub.data();
        if (profleaf[0] && _stricmp(profleaf, user) != 0) {
            rs_scrub(scrub.data(), scrub.size(), chunk.c_str(), profleaf, "<user>");
            chunk = scrub.data();
        }
        rs_scrub(scrub.data(), scrub.size(), chunk.c_str(), comp, "<machine>");
        chunk = scrub.data();
        out += "==== ";
        out += fn;
        out += (start > 0) ? " (tail) ====\n" : " ====\n";
        out += chunk;
        if (out.empty() || out.back() != '\n') out += "\n";
        out += "\n";
    }
    return out;
}

/* Native window and WebView2 lifecycle. */
static LRESULT CALLBACK PocWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_controller) { RECT rc; GetClientRect(hwnd, &rc); g_controller->put_Bounds(rc); }
        poc_post_window_state(hwnd);
        return 0;
    case WM_CLOSE: return 0;   /* inert: don't let the user close the UI while in the editor (would need a map reload) */
    case WM_NCCALCSIZE:
        /* frameless: consume the non-client area so the client (WebView2) fills the window and the native
         * title bar is gone. WS_THICKFRAME stays so Aero Snap + maximize work; the default maximize sizing
         * already respects the taskbar. When MAXIMIZED, Windows adds the resize-frame overhang beyond the
         * work area, so inset the client by that frame or the top/bottom clip off-screen. */
        if (wp) {
            if (IsZoomed(hwnd)) {
                NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
                int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx;
                p->rgrc[0].top  += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);   /* MUST be W: ANSI DefWindowProcA truncates the wide caption to "S" */
}
static void poc_create_window()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = PocWndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = L"SnapmapPlusStudioWebView";
    wc.style = CS_NOCLOSE;   /* remove the native close (X) button -- can't close the UI from the editor */
    RegisterClassExW(&wc);
    /* Initial size fits the list and detail panels side by side on a 1080p display. */
    g_hwnd = CreateWindowExW(0, L"SnapmapPlusStudioWebView", L"Snapmap+",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    /* A one-pixel DWM frame preserves shadows and rounded corners after
     * WM_NCCALCSIZE removes the visible caption. */
    if (g_hwnd) {
        MARGINS shadow = {1, 1, 1, 1};
        HRESULT hr = DwmExtendFrameIntoClientArea(g_hwnd, &shadow);
        if (FAILED(hr)) poc_logf("DwmExtendFrameIntoClientArea failed hr=0x%08lx", (unsigned long)hr);
    }
    /* Recalculate before showing so the native caption does not linger until resize. */
    if (g_hwnd)
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    poc_logf("window created (hwnd=%lu)", (unsigned long)(uintptr_t)g_hwnd);
}
static HRESULT on_message(ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    LPWSTR jp = nullptr;
    if (SUCCEEDED(args->get_WebMessageAsJson(&jp)) && jp) {
        sh_config_message config_message = sh_extract_config_message(
            jp, POC_CONFIG_KEY_CAP, POC_CONFIG_VALUE_CAP);
        if (config_message.kind != SH_CONFIG_MESSAGE_OTHER) {
            CoTaskMemFree(jp);
            std::string key, value;
            bool fields_valid = config_message.fields_valid;
            if (fields_valid) {
                key = sh_webview_json::to_utf8(config_message.key);
                if (config_message.kind == SH_CONFIG_MESSAGE_SET)
                    value = sh_webview_json::to_utf8(config_message.value_json);
                fields_valid =
                    !key.empty() && key.size() <= POC_CONFIG_KEY_CAP &&
                    key.find('\0') == std::string::npos;
                if (config_message.kind == SH_CONFIG_MESSAGE_SET)
                    fields_valid =
                        fields_valid &&
                        value.size() <= POC_CONFIG_VALUE_CAP &&
                        value.find('\0') == std::string::npos;
            }

            if (config_message.kind == SH_CONFIG_MESSAGE_GET) {
                if (!fields_valid) key.clear();
                poc_post_config_value(key);
            } else {
                int result = SH_CONFIG_SET_REJECTED;
                if (fields_valid && g_iface && g_iface->vtbl &&
                    g_iface->vtbl->config_set_json) {
                    result = g_iface->vtbl->config_set_json(
                        g_iface, key.c_str(), value.c_str());
                }
                if (!fields_valid) key.clear();
                poc_post_config_set_result(key, result);
            }
            return S_OK;
        }

        std::wstring json(jp); CoTaskMemFree(jp);
        std::wstring cmd;
        if (sh_webview_json::get_string(json, L"cmd", cmd)) {
            if (cmd == L"refresh") {
                poc_send_list("manual-refresh");
            } else if (cmd == L"perfList") {
                int seq = 0, entities = 0, matched = 0, mounted = 0;
                double render_ms = 0.0, handler_ms = 0.0;
                if (sh_webview_json::get_int(json, L"seq", &seq) &&
                    sh_webview_json::get_int(json, L"entities", &entities) &&
                    sh_webview_json::get_int(json, L"matched", &matched) &&
                    sh_webview_json::get_int(json, L"mounted", &mounted) &&
                    sh_webview_json::get_double(json, L"renderMs", &render_ms) &&
                    sh_webview_json::get_double(json, L"handlerMs", &handler_ms) &&
                    seq >= 0 && entities >= 0 && entities <= POC_MAX_ENTS &&
                    matched >= 0 && matched <= entities && mounted >= 0 && mounted <= matched &&
                    std::isfinite(render_ms) && std::isfinite(handler_ms) &&
                    render_ms >= 0.0 && render_ms <= 600000.0 &&
                    handler_ms >= 0.0 && handler_ms <= 600000.0) {
                    char l[256];
                    _snprintf_s(l, sizeof l, _TRUNCATE,
                                "perf ui-list: seq=%d entities=%d matched=%d mounted=%d render_ms=%.3f handler_ms=%.3f",
                                seq, entities, matched, mounted, render_ms, handler_ms);
                    poc_log(l);
                }
            } else if (cmd == L"listPrefabs") {
                poc_send_prefabs();
            } else if (cmd == L"selectPrefab") {
                std::wstring nm, fo; sh_webview_json::get_string(json, L"name", nm); sh_webview_json::get_string(json, L"folder", fo);
                poc_send_prefab_detail(sh_webview_json::to_utf8(nm), sh_webview_json::to_utf8(fo));
            } else if (cmd == L"resolvePrefabModel") {
                int generation = 0; std::wstring inherit;
                sh_webview_json::get_int(json, L"generation", &generation);
                sh_webview_json::get_string(json, L"inherit", inherit);
                std::string inherit8 = sh_webview_json::to_utf8(inherit);
                char model[512] = {0}; float scale[3] = {1.0f, 1.0f, 1.0f}; int flags = 0;
                if (generation >= 0 && inherit8.size() < 512 && g_iface && g_iface->vtbl) {
                    if (g_iface->vtbl->resolve_prefab_defaults)
                        flags = g_iface->vtbl->resolve_prefab_defaults(g_iface, inherit8.c_str(),
                                                                       model, (int)sizeof model,
                                                                       scale, 3);
                    /* Keep ext 20 as an ABI-compatible model-only fallback. New paired builds use
                     * ext 23 so sparse prefab scale can be merged with the installed entityDef. */
                    if (!(flags & SH_PREFAB_DEFAULT_MODEL) &&
                        g_iface->vtbl->resolve_prefab_model &&
                        g_iface->vtbl->resolve_prefab_model(g_iface, inherit8.c_str(),
                                                            model, (int)sizeof model))
                        flags |= SH_PREFAB_DEFAULT_MODEL;
                }
                std::wstring m = L"{\"kind\":\"prefabModelResolved\",\"generation\":";
                m += std::to_wstring(generation);
                m += L",\"inherit\":\""; m += sh_webview_json::escape_wide(inherit8.c_str());
                m += L"\",\"model\":\"";
                if (flags & SH_PREFAB_DEFAULT_MODEL) m += sh_webview_json::escape_wide(model);
                m += L"\",\"scale\":";
                if (flags & SH_PREFAB_DEFAULT_SCALE) {
                    m += L"["; m += std::to_wstring(scale[0]); m += L",";
                    m += std::to_wstring(scale[1]); m += L",";
                    m += std::to_wstring(scale[2]); m += L"]";
                } else m += L"null";
                m += L"}";
                poc_post_json(m.c_str());
            } else if (cmd == L"requestPrefabMesh") {
                int generation = 0; std::wstring model;
                sh_webview_json::get_int(json, L"generation", &generation);
                sh_webview_json::get_string(json, L"model", model);
                std::string model8 = sh_webview_json::to_utf8(model);
                int queued = 0;
                if (generation >= 0 && model8.size() < 512 && g_iface && g_iface->vtbl &&
                    g_iface->vtbl->request_prefab_mesh)
                    queued = g_iface->vtbl->request_prefab_mesh(g_iface,
                                                                (unsigned long)generation,
                                                                model8.c_str());
                if (!queued && !model8.empty()) {
                    std::wstring m = L"{\"kind\":\"prefabMeshUnavailable\",\"generation\":";
                    m += std::to_wstring(generation);
                    m += L",\"model\":\""; m += sh_webview_json::escape_wide(model8.c_str()); m += L"\"}";
                    poc_post_json(m.c_str());
                }
            } else if (cmd == L"savePrefabMeta") {
                std::wstring nm, fo, body; sh_webview_json::get_string(json, L"name", nm); sh_webview_json::get_string(json, L"folder", fo); sh_webview_json::get_string(json, L"body", body);
                poc_apply_save_prefab_meta(sh_webview_json::to_utf8(nm), sh_webview_json::to_utf8(fo), sh_webview_json::to_utf8(body));
            } else if (cmd == L"createPrefab") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                g_create_prefab_name = sh_webview_json::to_utf8(nm);
                g_pending_create_prefab = true;
            } else if (cmd == L"deletePrefab") {
                std::wstring nm, fo; sh_webview_json::get_string(json, L"name", nm); sh_webview_json::get_string(json, L"folder", fo);
                g_delete_prefab_name = sh_webview_json::to_utf8(nm); g_delete_prefab_folder = sh_webview_json::to_utf8(fo);
                g_pending_delete_prefab = true;
            } else if (cmd == L"loadPrefab") {
                std::wstring nm, fo; sh_webview_json::get_string(json, L"name", nm); sh_webview_json::get_string(json, L"folder", fo);
                g_load_prefab_name = sh_webview_json::to_utf8(nm); g_load_prefab_folder = sh_webview_json::to_utf8(fo);
                g_pending_load_prefab = true;
            } else if (cmd == L"renamePrefab") {
                std::wstring o, nn, fo; sh_webview_json::get_string(json, L"oldName", o); sh_webview_json::get_string(json, L"newName", nn); sh_webview_json::get_string(json, L"folder", fo);
                g_rename_prefab_old = sh_webview_json::to_utf8(o); g_rename_prefab_new = sh_webview_json::to_utf8(nn); g_rename_prefab_folder = sh_webview_json::to_utf8(fo);
                g_pending_rename_prefab = true;
            } else if (cmd == L"createFolder") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                g_create_folder_name = sh_webview_json::to_utf8(nm);
                g_pending_create_folder = true;
            } else if (cmd == L"renameFolder") {
                std::wstring o, nn; sh_webview_json::get_string(json, L"oldName", o); sh_webview_json::get_string(json, L"newName", nn);
                g_rename_folder_old = sh_webview_json::to_utf8(o); g_rename_folder_new = sh_webview_json::to_utf8(nn);
                g_pending_rename_folder = true;
            } else if (cmd == L"deleteFolder") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                g_delete_folder_name = sh_webview_json::to_utf8(nm);
                g_pending_delete_folder = true;
            } else if (cmd == L"movePrefabToFolder") {
                std::wstring nm, fromf, tof; sh_webview_json::get_string(json, L"name", nm); sh_webview_json::get_string(json, L"fromFolder", fromf); sh_webview_json::get_string(json, L"toFolder", tof);
                g_move_prefab_name = sh_webview_json::to_utf8(nm); g_move_prefab_from = sh_webview_json::to_utf8(fromf); g_move_prefab_to = sh_webview_json::to_utf8(tof);
                g_pending_move_prefab = true;
            } else if (cmd == L"select") {
                int eid = -1;
                if (sh_webview_json::get_int(json, L"eid", &eid)) { g_displayed_eid = eid; poc_send_state(eid, false); }
            } else if (cmd == L"openTimeline") {
                int eid = -1;
                if (sh_webview_json::get_int(json, L"eid", &eid) && eid >= 0) { g_open_timeline_eid = eid; g_pending_open_timeline = true; }
            } else if (cmd == L"resolveEntityInherit") {
                int eid = -1;
                if (sh_webview_json::get_int(json, L"eid", &eid) && eid >= 0) { g_resolve_entity_eid = eid; g_pending_resolve_entity = true; }
            } else if (cmd == L"saveTimeline") {
                int eid = -1; std::wstring j;
                if (sh_webview_json::get_int(json, L"eid", &eid) && eid >= 0 && sh_webview_json::get_string(json, L"json", j) && !j.empty()) {
                    g_save_timeline_eid = eid; g_save_timeline_json = sh_webview_json::to_utf8(j); g_pending_save_timeline = true;
                }
            } else if (cmd == L"setSync") {
                int on = 0; sh_webview_json::get_int(json, L"on", &on); g_sync_on = (on != 0); g_last_editor_sel = -1; g_last_sel_sig = 0;
            } else if (cmd == L"delete") {
                sh_webview_json::get_int_array(json, L"eids", g_delete_eids);
                if (!g_delete_eids.empty()) g_pending_delete = true;
            } else if (cmd == L"selectInEditor") {
                sh_webview_json::get_int_array(json, L"eids", g_select_eids);
                g_pending_select = true;   /* applied under the loop mutex */
            } else if (cmd == L"deselect") {
                g_pending_deselect = true;
            } else if (cmd == L"enumInherits") {
                poc_send_enum(0, nullptr);
            } else if (cmd == L"enumClasses") {
                std::wstring inh; sh_webview_json::get_string(json, L"inherit", inh);
                std::string i8 = sh_webview_json::to_utf8(inh);
                poc_send_enum(1, i8.c_str());
            } else if (cmd == L"enumEvents") {
                poc_send_events();
            } else if (cmd == L"enumEventDocs") {
                poc_send_event_docs();
            } else if (cmd == L"enumEntityAssets") {
                poc_send_entity_assets();
            } else if (cmd == L"enumArgResclass") {
                std::wstring rc; sh_webview_json::get_string(json, L"resClass", rc);
                std::string rc8 = sh_webview_json::to_utf8(rc);
                poc_send_arg_resclass(rc8.c_str());
            } else if (cmd == L"lookupDesc") {
                std::wstring inh, cls; sh_webview_json::get_string(json, L"inherit", inh); sh_webview_json::get_string(json, L"classname", cls);
                std::string i8 = sh_webview_json::to_utf8(inh), c8 = sh_webview_json::to_utf8(cls);
                poc_send_desc(i8.c_str(), c8.c_str());
            } else if (cmd == L"findMaterial") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                std::string n8 = sh_webview_json::to_utf8(nm);
                poc_send_material_result(n8.c_str());
            } else if (cmd == L"rawmapStatus") {
                poc_send_rawmap_status(L"");
            } else if (cmd == L"rawmapLoadPick") {
                /* Both pickers return immediately -- the dialog runs on its own thread and the think
                 * loop acts on the result (poc_finish_pick). Running Show() here stalled that loop for
                 * as long as the dialog was open, which is the lag this fixed. */
                poc_begin_pick(0);
            } else if (cmd == L"rawmapSaveNow") {
                /* Resolve the current destination in the backend; the page's last
                 * status may belong to a map that has since closed. */
                poc_rawmap_configure(nullptr, nullptr, 5, nullptr);
            } else if (cmd == L"rawmapSavePick") {
                poc_begin_pick(1);
            } else if (cmd == L"rawmapLoadNow") {
                /* Ask the backend's editor-frame hook to reload the map, so the staged file opens
                 * without a trip to the SnapMap menu. The reply is a REQUEST result, not a
                 * completion -- the reload lands a frame later, and the status refresh that follows
                 * is what shows whether the swap actually fired. */
                if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->rawmap_load_now) {
                    poc_send_rawmap_status(L"This build's backend cannot reload the map.");
                } else {
                    char msg[192] = "";
                    int ok = g_iface->vtbl->rawmap_load_now(g_iface, msg, (int)sizeof msg);
                    poc_log(ok ? "rawmap load-now: accepted" : "rawmap load-now: refused");
                    poc_send_rawmap_status(ok ? L"Opening as a new map -- Save will ask you to name it."
                                              : (L"Cannot open: " + sh_webview_json::escape_wide(msg)).c_str());
                }
            } else if (cmd == L"rawmapArm") {
                int on = 0; sh_webview_json::get_int(json, L"on", &on);
                /* Say what it does to the person's maps, not what it does to the detour -- and make
                 * the OFF message say the menu still works, because the tick's whole hazard is
                 * reading as the feature's master switch when it is only its scope. */
                poc_rawmap_configure(nullptr, nullptr, on ? 1 : 0,
                                     on ? L"Rawmaps now apply to every map load and save."
                                        : L"Rawmaps apply to the File menu's own actions only.");
            } else if (cmd == L"rawmapKeepHere") {
                /* "Keep saving to file". Arm codes rather than a new vtable slot: the two
                 * DLLs must match slot for slot, and `arm` was already a verb code. Neither
                 * value writes anything -- 3 pins the path the readout is already showing,
                 * 4 releases it, so the line under the tick is true either way. */
                int on = 0; sh_webview_json::get_int(json, L"on", &on);
                poc_rawmap_configure(nullptr, nullptr, on ? 3 : 4,
                                     on ? L"Saves keep going to this file until you open another map."
                                        : L"Saves go back to the default rawmap file.");
            } else if (cmd == L"newEntity") {
                std::wstring js, lab;
                sh_webview_json::get_string(json, L"json", js); sh_webview_json::get_string(json, L"label", lab);
                g_new_entity_json = sh_webview_json::to_utf8(js); g_new_entity_label = sh_webview_json::to_utf8(lab);
                g_pending_new_entity = true;
            } else if (cmd == L"soundSession") {
                int on = 0; sh_webview_json::get_int(json, L"on", &on);
                g_sound_session_on = on ? 1 : 0;
                g_pending_sound_session = true;
            } else if (cmd == L"soundPreview") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                g_sound_preview_name = sh_webview_json::to_utf8(nm);   /* empty = stop */
                g_pending_sound_preview = true;
            } else if (cmd == L"materialRect") {
                /* The Assets browser asks before offering the Virtual Mapping carrier: only
                 * virtual-textured materials have a rect, and the rect IS the renderParm value. */
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                std::string n8 = sh_webview_json::to_utf8(nm);
                int r[4] = {0,0,0,0}, ok = 0;
                if (g_iface && g_iface->vtbl && g_iface->vtbl->material_rect)
                    ok = g_iface->vtbl->material_rect(g_iface, n8.c_str(), r);
                std::wstring m = L"{\"kind\":\"materialRect\",\"name\":\"";
                m += sh_webview_json::escape_wide(n8.c_str());
                m += L"\",\"ok\":"; m += (ok ? L"true" : L"false");
                m += L",\"x\":" + std::to_wstring(r[0]) + L",\"y\":" + std::to_wstring(r[1]);
                m += L",\"w\":" + std::to_wstring(r[2]) + L",\"h\":" + std::to_wstring(r[3]) + L"}";
                if (g_webview) g_webview->PostWebMessageAsJson(m.c_str());
            } else if (cmd == L"listAssets") {
                int akind = SH_ASSET_MATERIAL; sh_webview_json::get_int(json, L"assetKind", &akind);
                poc_send_asset_list(akind);
            } else if (cmd == L"pinsLoad") {
                poc_send_pins();
            } else if (cmd == L"pinsSave") {
                std::wstring doc; sh_webview_json::get_string(json, L"doc", doc);
                poc_save_pins(sh_webview_json::to_utf8(doc));
            } else if (cmd == L"getPreview") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                std::string n8 = sh_webview_json::to_utf8(nm);
                poc_send_preview(n8.c_str());
            } else if (cmd == L"requestPreview") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                int asset_kind = -1; sh_webview_json::get_int(json, L"assetKind", &asset_kind);
                std::string n8 = sh_webview_json::to_utf8(nm);
                poc_request_preview(n8.c_str(), asset_kind);
            } else if (cmd == L"cancelPreview") {
                poc_cancel_preview();
            /* Camera footer controls: a lock captures its supplied target; an edit queues one write. */
            } else if (cmd == L"camLock") {
                int on = 0; sh_webview_json::get_int(json, L"on", &on);
                g_cam_lock = (on != 0);
                if (g_cam_lock) {   /* capture the target coordinates supplied with the request */
                    double x, y, z;
                    if (sh_webview_json::get_double(json, L"x", &x)) g_cam_xyz[0] = (float)x;
                    if (sh_webview_json::get_double(json, L"y", &y)) g_cam_xyz[1] = (float)y;
                    if (sh_webview_json::get_double(json, L"z", &z)) g_cam_xyz[2] = (float)z;
                }
            } else if (cmd == L"camSet") {
                double x, y, z;
                if (sh_webview_json::get_double(json, L"x", &x)) g_cam_xyz[0] = (float)x;
                if (sh_webview_json::get_double(json, L"y", &y)) g_cam_xyz[1] = (float)y;
                if (sh_webview_json::get_double(json, L"z", &z)) g_cam_xyz[2] = (float)z;
                g_cam_write_once = true;
            } else if (cmd == L"reportSubmit") {
                /* opaque transport: the page composed the full report JSON; this side only size-caps it
                 * and ships it to the relay on a worker thread (see the feedback section above). */
                std::wstring payload;
                if (sh_webview_json::get_string(json, L"payload", payload) && !payload.empty() && !g_report_inflight) {
                    std::string p8 = sh_webview_json::to_utf8(payload);
                    if (p8.size() <= REPORT_PAYLOAD_CAP) {
                        g_report_payload.swap(p8);
                        g_report_inflight = true;
                        HANDLE h = CreateThread(nullptr, 0, report_thread, nullptr, 0, nullptr);
                        if (h) CloseHandle(h);
                        else {
                            g_report_inflight = false;
                            poc_post_json(L"{\"kind\":\"reportResult\",\"ok\":false}");
                        }
                    } else poc_post_json(L"{\"kind\":\"reportResult\",\"ok\":false}");
                }
            } else if (cmd == L"crashSubmit") {
                /* Compose crash reports here to add optional local log tails, then use the
                 * same worker and result message as ordinary feedback. */
                std::wstring title, bodyw, contact, hp, renderer;
                int attach = 0;
                sh_webview_json::get_string(json, L"title", title);
                sh_webview_json::get_string(json, L"body", bodyw);
                sh_webview_json::get_string(json, L"contact", contact);
                sh_webview_json::get_string(json, L"website", hp);
                /* Prefer the recorded renderer: this session may differ from the one that
                 * faulted. Use the live renderer only for older records without the field. */
                sh_webview_json::get_string(json, L"renderer", renderer);
                sh_webview_json::get_int(json, L"attachLogs", &attach);
                if (!title.empty() && !bodyw.empty() && !g_report_inflight) {
                    std::string logs = attach ? crash_collect_logs() : std::string();
                    std::string rend = renderer.empty() ? std::string(sh_host_renderer_name())
                                                        : sh_webview_json::to_utf8(renderer);
                    std::string p;
                    p.reserve(logs.size() + 12288);
                    p += "{\"category\":\"crash\",\"title\":\"";  p += sh_webview_json::escape_utf8(sh_webview_json::to_utf8(title));
                    p += "\",\"body\":\"";                         p += sh_webview_json::escape_utf8(sh_webview_json::to_utf8(bodyw));
                    p += "\",\"contact\":\"";                      p += sh_webview_json::escape_utf8(sh_webview_json::to_utf8(contact));
                    p += "\",\"version\":\"";                      p += sh_webview_json::escape_utf8(g_version);
                    p += "\",\"renderer\":\"";                     p += sh_webview_json::escape_utf8(rend);
                    p += "\",\"website\":\"";                      p += sh_webview_json::escape_utf8(sh_webview_json::to_utf8(hp));
                    p += "\",\"logs\":\"";                         p += sh_webview_json::escape_utf8(logs);
                    p += "\"}";
                    if (p.size() <= REPORT_PAYLOAD_CAP) {
                        g_report_payload.swap(p);
                        g_report_is_crash = true;
                        g_report_inflight = true;
                        HANDLE h = CreateThread(nullptr, 0, report_thread, nullptr, 0, nullptr);
                        if (h) CloseHandle(h);
                        else {
                            g_report_inflight = false; g_report_is_crash = false;
                            poc_post_json(L"{\"kind\":\"reportResult\",\"ok\":false}");
                        }
                    } else poc_post_json(L"{\"kind\":\"reportResult\",\"ok\":false}");
                }
            } else if (cmd == L"crashDismiss") {
                /* dismiss = handled: clear the pending records so the dialog never nags twice. The
                 * full logs + any crash dump stay on disk untouched. */
                crash_clear_pending();
            } else if (cmd == L"winMin") {
                ShowWindow(g_hwnd, SW_MINIMIZE);
            } else if (cmd == L"winMax") {
                ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                poc_post_window_state(g_hwnd);
            } else if (cmd == L"winState") {
                poc_post_window_state(g_hwnd);
            } else if (cmd == L"winDrag") {
                ReleaseCapture();
                SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);   /* start the native move loop */
            } else if (cmd == L"winResize") {
                std::wstring dir; sh_webview_json::get_string(json, L"dir", dir);
                WPARAM ht = 0;
                if      (dir == L"l")  ht = HTLEFT;      else if (dir == L"r")  ht = HTRIGHT;
                else if (dir == L"t")  ht = HTTOP;       else if (dir == L"b")  ht = HTBOTTOM;
                else if (dir == L"tl") ht = HTTOPLEFT;   else if (dir == L"tr") ht = HTTOPRIGHT;
                else if (dir == L"bl") ht = HTBOTTOMLEFT;else if (dir == L"br") ht = HTBOTTOMRIGHT;
                if (ht) { ReleaseCapture(); SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, ht, 0); }
            } else if (cmd == L"tlUseSelection") {
                /* Fetch current selection on demand, using the full entity capacity so the
                 * reported count is not truncated. */
                static int uselids[POC_MAX_ENTS];
                int un = poc_get_selection(uselids, POC_MAX_ENTS);
                wchar_t m[96];
                if (un == 1) _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"tlUseSelectionResult\",\"ok\":true,\"eid\":%d}", uselids[0]);
                else _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"tlUseSelectionResult\",\"ok\":false,\"count\":%d}", un);
                poc_post_json(m);
            } else if (cmd == L"pushStack") {
                std::vector<int> ids; sh_webview_json::get_int_array(json, L"eids", ids);
                bool ok = g_iface && g_iface->vtbl && g_iface->vtbl->push_to_stack && !ids.empty();
                if (ok) g_iface->vtbl->push_to_stack(g_iface, 0, ids.data(), (int)ids.size());
                wchar_t m[160];
                _snwprintf_s(m, _countof(m), _TRUNCATE,
                    L"{\"kind\":\"pushStackResult\",\"result\":%d,\"count\":%d}", ok ? 1 : 0, (int)ids.size());
                poc_post_json(m);
            } else if (cmd == L"clearStack") {
                bool ok = g_iface && g_iface->vtbl && g_iface->vtbl->clear_stack;
                int had = ok ? g_iface->vtbl->clear_stack(g_iface, 0) : 0;
                wchar_t m[128];
                _snwprintf_s(m, _countof(m), _TRUNCATE,
                    L"{\"kind\":\"clearStackResult\",\"result\":%d,\"count\":%d}", ok ? 1 : 0, had);
                poc_post_json(m);
            } else if (cmd == L"save") {
                int eid = -1; sh_webview_json::get_int(json, L"eid", &eid);
                std::wstring decl, cls, inh, dnm;
                sh_webview_json::get_string(json, L"decl", decl); sh_webview_json::get_string(json, L"classname", cls);
                sh_webview_json::get_string(json, L"inherit", inh); sh_webview_json::get_string(json, L"displayname", dnm);
                g_save_eid = eid; g_save_decl = sh_webview_json::to_utf8(decl); g_save_class = sh_webview_json::to_utf8(cls);
                g_save_inherit = sh_webview_json::to_utf8(inh); g_save_dname = sh_webview_json::to_utf8(dnm);
                g_pending_save = true;
            }
        }
    }
    return S_OK;
}
static HRESULT on_nav_completed(ICoreWebView2 *,
                                ICoreWebView2NavigationCompletedEventArgs *args)
{
    BOOL success = FALSE;
    if (!args || FAILED(args->get_IsSuccess(&success)) || !success) {
        COREWEBVIEW2_WEB_ERROR_STATUS status =
            COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        if (args) args->get_WebErrorStatus(&status);
        poc_logf("NavigationCompleted FAILED status=%lu",
                 (unsigned long)status);
        return S_OK;
    }
    /* The hidden native host may be shown only after the fully parsed, pre-themed page can receive
     * messages. This keeps a blank/light controller from becoming the first visible frame. */
    /* A newly navigated page starts with an unchecked lock control. Release any old page's lock and
     * force an initial coordinate publication so native state and the fresh controls cannot diverge. */
    g_cam_lock = false;
    g_cam_write_once = false;
    g_cam_read_published = false;
    g_page_loaded = true;
    g_webview_ready = true;
    poc_post_config_status();
    poc_send_list("navigation-completed");
    return S_OK;
}
static HRESULT on_controller_created(HRESULT result, ICoreWebView2Controller *controller)
{
    if (FAILED(result) || !controller) { poc_logf("controller creation FAILED hr=0x%08lx", (unsigned long)result); return result; }
    g_controller = controller; g_controller->AddRef();
    g_controller->get_CoreWebView2(&g_webview);
    if (!g_webview) { poc_log("get_CoreWebView2 null"); return E_FAIL; }
    g_webview17.Reset();
    g_webview->QueryInterface(__uuidof(ICoreWebView2_17),
                              reinterpret_cast<void **>(g_webview17.GetAddressOf()));
    { /* disable WebView2's default (browser) right-click menu app-wide; our own menus are HTML. */
        ICoreWebView2Settings *settings = nullptr;
        if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->Release();
        }
    }
    RECT rc; GetClientRect(g_hwnd, &rc); g_controller->put_Bounds(rc);
    EventRegistrationToken tok;
    g_webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(on_message).Get(), &tok);
    g_webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(on_nav_completed).Get(), &tok);
    HRESULT navigate = g_webview->NavigateToString(g_html.c_str());
    g_controller->put_IsVisible(TRUE);
    if (FAILED(navigate)) {
        poc_logf("NavigateToString FAILED hr=0x%08lx",
                 (unsigned long)navigate);
        return navigate;
    }
    poc_log("controller ready: navigated to embedded HTML");
    return S_OK;
}
static HRESULT on_environment_created(HRESULT result, ICoreWebView2Environment *env)
{
    if (FAILED(result) || !env) { poc_logf("environment creation FAILED hr=0x%08lx", (unsigned long)result); return result; }
    g_webview_environment12.Reset();
    env->QueryInterface(__uuidof(ICoreWebView2Environment12),
                        reinterpret_cast<void **>(g_webview_environment12.GetAddressOf()));
    return env->CreateCoreWebView2Controller(g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(on_controller_created).Get());
}
static void poc_start_webview()
{
    wchar_t local[MAX_PATH] = {}; std::wstring udf = L".";
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
        udf = std::wstring(local) + L"\\snapmap-plus\\webview2"; SHCreateDirectoryExW(nullptr, udf.c_str(), nullptr);
    }
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(on_environment_created).Get());
    if (FAILED(hr)) poc_logf("Create env FAILED hr=0x%08lx", (unsigned long)hr);
}

/* UI request drain and polling loop, about 30 Hz. */
static void poc_think_loop()
{
    bool was_visible = false;
    unsigned frame = 0;
    for (;;) {
        frame++;
        bool did_save = false, did_delete = false, did_create_prefab = false, did_select_refused = false;
        bool did_delete_prefab = false, did_rename_prefab = false, did_load_prefab = false;
        bool did_new_entity = false;
        bool did_sound_preview = false;
        bool did_create_folder = false, did_rename_folder = false, did_delete_folder = false, did_move_prefab = false;
        bool did_open_timeline = false, did_resolve_entity = false, did_save_timeline = false;
        EnterCriticalSection(&g_loop->mtx);
        if (g_iface && g_iface->vtbl && g_iface->vtbl->drain_work_queue) g_iface->vtbl->drain_work_queue(g_iface);
        if (g_pending_save)   { poc_apply_save();    g_pending_save = false;   did_save = true; }
        if (g_pending_delete) { poc_apply_deletes(); g_delete_eids.clear(); g_pending_delete = false; did_delete = true; }
        if (g_pending_select) {
            poc_apply_select_in_editor();
            /* keep forward-sync (editor->list) quiet about the selection WE just pushed (avoid a ping-pong). */
            g_last_editor_sel = (g_select_eids.size() == 1) ? g_select_eids[0] : -1;
            if (g_select_eids.size() == 1) g_displayed_eid = g_select_eids[0];
            g_select_eids.clear(); g_pending_select = false;
        }
        if (g_pending_deselect) {
            poc_apply_deselect();
            g_last_editor_sel = -1; g_last_sel_sig = 0;
            g_pending_deselect = false;
        }
        if (g_select_refused) { g_select_refused = false; did_select_refused = true; }
        if (g_pending_create_prefab) {
            poc_apply_create_prefab();
            g_pending_create_prefab = false;
            did_create_prefab = true;
        }
        if (g_pending_delete_prefab) { poc_apply_delete_prefab(); g_pending_delete_prefab = false; did_delete_prefab = true; }
        if (g_pending_rename_prefab) { poc_apply_rename_prefab(); g_pending_rename_prefab = false; did_rename_prefab = true; }
        if (g_pending_load_prefab)   { poc_apply_load_prefab();   g_pending_load_prefab = false;   did_load_prefab = true; }
        if (g_pending_new_entity)    { poc_apply_new_entity();    g_pending_new_entity = false;    did_new_entity = true; }
        /* A finished file picker. Its thread only carried the dialog; everything that touches the
         * backend happens here, on the UI thread, like every other deferred action above. */
        if (InterlockedCompareExchange(&g_pick_done, 0, 0) != 0) poc_finish_pick();
        if (g_pending_sound_session) { poc_apply_sound_session(); g_pending_sound_session = false; }
        if (g_pending_sound_preview) { poc_apply_sound_preview(); g_pending_sound_preview = false; did_sound_preview = true; }
        if (g_pending_create_folder) { poc_apply_create_folder(); g_pending_create_folder = false; did_create_folder = true; }
        if (g_pending_rename_folder) { poc_apply_rename_folder(); g_pending_rename_folder = false; did_rename_folder = true; }
        if (g_pending_delete_folder) { poc_apply_delete_folder(); g_pending_delete_folder = false; did_delete_folder = true; }
        if (g_pending_move_prefab)   { poc_apply_move_prefab();   g_pending_move_prefab = false;   did_move_prefab = true; }
        if (g_pending_open_timeline) { g_tl_json_len = poc_serialize_entity_raw(g_open_timeline_eid); g_pending_open_timeline = false; did_open_timeline = true; }
        if (g_pending_resolve_entity) { g_resolve_json_len = poc_serialize_entity_resolve(g_resolve_entity_eid); g_pending_resolve_entity = false; did_resolve_entity = true; }
        if (g_pending_save_timeline) { poc_apply_save_timeline(); g_pending_save_timeline = false; did_save_timeline = true; }
        /* Hold the requested camera lock, or flush one committed coordinate edit. */
        if (g_cam_lock || g_cam_write_once) { poc_cam_write(); g_cam_write_once = false; }
        LeaveCriticalSection(&g_loop->mtx);

        if (did_save) {
            wchar_t m[80]; _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"saveResult\",\"result\":%d}", g_save_result);
            poc_post_json(m);
            poc_send_list("save");
            if (g_save_eid >= 0) poc_send_state(g_save_eid, false);
        }
        if (did_delete) poc_send_list("delete");
        if (did_select_refused) poc_post_json(L"{\"kind\":\"selectRefused\"}");
        if (did_create_prefab) {
            std::wstring m = L"{\"kind\":\"createPrefabResult\",\"result\":"; m += std::to_wstring(g_create_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_create_prefab_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_create_result == 1) poc_send_prefabs();
        }
        if (did_delete_prefab) {
            std::wstring m = L"{\"kind\":\"deletePrefabResult\",\"result\":"; m += std::to_wstring(g_delete_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_delete_prefab_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_delete_result == 1) poc_send_prefabs();
        }
        if (did_rename_prefab) {
            std::wstring m = L"{\"kind\":\"renamePrefabResult\",\"result\":"; m += std::to_wstring(g_rename_result);
            m += L",\"oldName\":\""; m += sh_webview_json::escape_wide(g_rename_prefab_old.c_str());
            m += L"\",\"newName\":\""; m += sh_webview_json::escape_wide(g_rename_prefab_new.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_rename_result == 1) poc_send_prefabs();
        }
        if (did_load_prefab) {
            std::wstring m = L"{\"kind\":\"loadPrefabResult\",\"result\":"; m += std::to_wstring(g_load_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_load_prefab_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
        }
        if (did_sound_preview) {
            std::wstring m = L"{\"kind\":\"soundPreviewResult\",\"playing\":";
            m += (g_sound_preview_result ? L"true" : L"false");
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_sound_preview_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
        }
        if (did_new_entity) {
            std::wstring m = L"{\"kind\":\"newEntityResult\",\"result\":"; m += std::to_wstring(g_new_entity_result);
            m += L",\"label\":\""; m += sh_webview_json::escape_wide(g_new_entity_label.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
        }
        if (did_open_timeline) poc_emit_timeline_data(g_open_timeline_eid, g_tl_json_len);
        if (did_resolve_entity) poc_emit_entity_inherit(g_resolve_entity_eid, g_resolve_json_len);
        if (did_save_timeline) {
            std::wstring m = L"{\"kind\":\"saveTimelineResult\",\"eid\":"; m += std::to_wstring(g_save_timeline_eid);
            m += L",\"result\":"; m += std::to_wstring(g_save_timeline_result); m += L"}";
            poc_post_json(m.c_str());
        }
        if (did_create_folder) {
            std::wstring m = L"{\"kind\":\"createFolderResult\",\"result\":"; m += std::to_wstring(g_create_folder_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_create_folder_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_create_folder_result == 1) poc_send_prefabs();
        }
        if (did_rename_folder) {
            std::wstring m = L"{\"kind\":\"renameFolderResult\",\"result\":"; m += std::to_wstring(g_rename_folder_result);
            m += L",\"oldName\":\""; m += sh_webview_json::escape_wide(g_rename_folder_old.c_str());
            m += L"\",\"newName\":\""; m += sh_webview_json::escape_wide(g_rename_folder_new.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_rename_folder_result == 1) poc_send_prefabs();
        }
        if (did_delete_folder) {
            std::wstring m = L"{\"kind\":\"deleteFolderResult\",\"result\":"; m += std::to_wstring(g_delete_folder_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_delete_folder_name.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_delete_folder_result == 1) poc_send_prefabs();
        }
        if (did_move_prefab) {
            std::wstring m = L"{\"kind\":\"movePrefabResult\",\"result\":"; m += std::to_wstring(g_move_prefab_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_move_prefab_name.c_str());
            m += L"\",\"toFolder\":\""; m += sh_webview_json::escape_wide(g_move_prefab_to.c_str()); m += L"\"}";
            poc_post_json(m.c_str());
            if (g_move_prefab_result == 1) poc_send_prefabs();
        }
        if (g_report_done) {   /* feedback/crash POST finished on its worker thread -- relay the result */
            g_report_done = false;
            std::wstring m = L"{\"kind\":\"reportResult\",\"ok\":"; m += g_report_ok ? L"true" : L"false";
            m += L",\"mode\":\""; m += sh_webview_json::escape_wide(g_report_mode);
            m += L"\",\"number\":"; m += std::to_wstring(g_report_number); m += L"}";
            poc_post_json(m.c_str());
            if (g_report_is_crash) {
                /* a successfully-sent crash report is handled -- clear the pending records (same as
                 * Dismiss); a failed send keeps them, so the dialog can retry / reappear next launch. */
                if (g_report_ok) { crash_clear_pending(); g_crash_last_sent.clear(); g_crash_last_seen.clear(); }
                g_report_is_crash = false;
            }
            g_report_inflight = false;
        }

        /* Reclassify changes anywhere in the retained inventory so an older or
         * same-second arrival cannot hide a crash prompt. Nonterminal records show
         * notices only after startup; retained older records remain diagnostic data. */
        if (g_page_loaded && (frame == 1 || frame % 60 == 0)) {
            std::vector<std::string> names;
            int cnt = crash_scan(names);
            if (cnt == 0) {
                /* cleared (Dismiss, a sent report, or by hand) -- forget what we announced so the
                 * directory filling up again is treated as new rather than as already-seen. */
                g_crash_last_seen.clear();
                g_crash_last_sent.clear();
            } else if (g_crash_last_seen.changed(names)) {
                bool startup = (frame == 1);
                int terminal = 0, survived = 0;
                std::string termName, termRec;
                for (size_t i = 0; i < names.size(); i++) {
                    std::string rec = crash_read_record(names[i]);
                    if (rec.empty()) continue;   /* torn/unreadable -- skip it, never block the rest */
                    if (crash_record_is_terminal(rec)) {
                        if (terminal == 0) { termName = names[i]; termRec = rec; }
                        terminal++;
                    } else {
                        survived++;
                    }
                }
                if (terminal > 0 && termName != g_crash_last_sent) {
                    g_crash_last_sent = termName;
                    std::string m = "{\"kind\":\"crashPending\",\"count\":" + std::to_string(terminal) +
                                    ",\"record\":" + termRec + "}";
                    int wl = MultiByteToWideChar(CP_UTF8, 0, m.c_str(), -1, nullptr, 0);
                    if (wl > 0) {
                        std::wstring wm; wm.resize(wl - 1);
                        if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, m.c_str(), -1, &wm[0], wl);
                        poc_post_json(wm.c_str());
                        char l[160];
                        _snprintf_s(l, sizeof l, _TRUNCATE,
                                    "crash: terminal record announced (terminal=%d, survived=%d)",
                                    terminal, survived);
                        poc_log(l);
                    }
                } else if (survived > 0 && terminal == 0 && !startup) {
                    /* nonterminal record observed this session: notice only */
                    std::wstring m = L"{\"kind\":\"faultRecovered\",\"count\":";
                    m += std::to_wstring(survived); m += L"}";
                    poc_post_json(m.c_str());
                    poc_logf("crash: %lu recovered fault record(s) -- toast only, no dialog",
                             (unsigned long)survived);
                }
            }
        }

        if (g_webview_ready) {
            bool ready = poc_editor_ready() != 0;
            if (ready && !was_visible) {
                char l[128];
                _snprintf_s(l, sizeof l, _TRUNCATE,
                            "perf lifecycle: editor-visible frame=%u", frame);
                poc_log(l);
                ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);
                poc_send_list("editor-visible");
                /* The DOM survives Play and map reloads. Close any open timeline when the
                 * editor returns so subsequent edits begin with fresh serialized state. */
                poc_post_json(L"{\"kind\":\"editorReopened\"}");
                was_visible = true;
            }
            else if (!ready && was_visible) {
                poc_perf_flush_collect("editor-hidden");
                poc_log("perf lifecycle: editor-hidden");
                ShowWindow(g_hwnd, SW_HIDE); was_visible = false;
            }

            /* Sample camera coordinates at UI cadence; change gating avoids idle messages. */
            if (was_visible && !g_cam_lock) poc_cam_read_send();

            /* periodic auto tasks (~ every 10 frames = ~330 ms): list change poll, editor-selection sync,
             * displayed-state change poll. All emit only on an actual change. */
            if (was_visible && (frame % 10 == 0)) {
                unsigned long long poll_started = poc_perf_now_us(), collect_us = 0;
                int rdy = 0; int n = poc_collect_timed(&rdy, "poll", &collect_us);
                uint64_t sig = poc_list_sig(n);
                if (sig != g_last_list_sig) {
                    poc_rescan_timelines_timed(n, "entity-change");   /* change-gated, not a fixed timer */
                    g_last_list_sig = sig;
                    poc_emit_list(n, rdy, "entity-change");
                }

                /* Publish selection count regardless of sync direction for prefab creation. */
                unsigned long long selection_started = poc_perf_now_us();
                static int selids[POC_MAX_ENTS];
                int sn = poc_get_selection(selids, POC_MAX_ENTS);
                if (sn != g_last_selcount) {
                    g_last_selcount = sn;
                    wchar_t m[64]; _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"selCount\",\"count\":%d}", sn);
                    poc_post_json(m);
                }
                if (g_sync_on) {
                    /* mirror the WHOLE editor selection (any N) into the list, only when it changes. */
                    std::sort(selids, selids + sn);
                    uint64_t sig = 1469598103934665603ull;
                    for (int a = 0; a < sn; a++) sig = hint(sig, selids[a]);
                    sig ^= (uint64_t)sn;
                    if (sig != g_last_sel_sig) {
                        g_last_sel_sig = sig;
                        std::wstring m = L"{\"kind\":\"editorSelect\",\"eids\":[";
                        for (int a = 0; a < sn; a++) { if (a) m += L","; m += std::to_wstring(selids[a]); }
                        m += L"]}";
                        poc_post_json(m.c_str());
                        if (sn == 1) g_displayed_eid = selids[0];
                    }
                }
                unsigned long long selection_finished = poc_perf_now_us();
                bool state_called = g_displayed_eid >= 0;
                unsigned long long state_started = selection_finished;
                if (state_called) poc_send_state(g_displayed_eid, true);
                unsigned long long state_finished = poc_perf_now_us();
                unsigned long long poll_finished = poc_perf_now_us();
                poc_perf_note_poll(poll_finished >= poll_started ? poll_finished - poll_started : 0,
                                   collect_us,
                                   selection_finished >= selection_started ? selection_finished - selection_started : 0,
                                   state_finished >= state_started ? state_finished - state_started : 0,
                                   state_called, n, sn);
            }
        }

        if (g_webview_ready) poc_send_prefab_mesh_completion();

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(0x21);
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI sh_ui_init(LPVOID param_1)
{
    sh_ui_argblock *args = reinterpret_cast<sh_ui_argblock *>(param_1);
    g_loop = new ShLoopState();
    InitializeCriticalSection(&g_loop->mtx); g_loop->flags = 0;
    if (args && args->out_slot) *reinterpret_cast<void **>(args->out_slot) = g_loop;
    g_iface = args ? args->iface : nullptr;
    g_ents = (PocEnt *)malloc(sizeof(PocEnt) * POC_MAX_ENTS);
    g_tls  = (PocTl  *)malloc(sizeof(PocTl)  * POC_MAX_TLS);
    if (!g_ents || !g_tls) {
        poc_log("sh_ui_init: FATAL -- malloc failed for g_ents/g_tls, aborting init");
        free(g_ents); g_ents = nullptr;
        free(g_tls);  g_tls  = nullptr;
        return 1;
    }
    poc_read_version();

    poc_log("=== sh_ui_init (WebView2 POC, entities-deep) entered ===");
    poc_log(g_iface ? "interface handed over: yes" : "interface handed over: NO (null)");
    { char v[128]; _snprintf_s(v, sizeof v, _TRUNCATE, "installed version: %s", g_version.c_str()); poc_log(v); }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int n = MultiByteToWideChar(CP_UTF8, 0, kMockupHtml, -1, nullptr, 0);
    g_html.resize(n > 0 ? n - 1 : 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, kMockupHtml, -1, &g_html[0], n);
    {
        std::string theme_json;
        int got = poc_config_get_json("theme", theme_json,
                                      &g_config_status_flags);
        const char *seed_value = got >= 0 ? theme_json.c_str() : "\"light\"";
        if (got < 0)
            poc_log("config: theme unavailable; using light for this session");
        if (!sh_theme_seed_html(g_html, seed_value))
            poc_log("config: embedded HTML theme marker missing or duplicated");
    }

    poc_create_window();
    poc_start_webview();
    poc_think_loop();
    return 0;
}
