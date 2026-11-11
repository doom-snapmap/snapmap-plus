#include "webview_json.h"
/* WebView2 frontend host. sh_ui_init (ordinal 10) receives loop state in arg[0]
 * and the backend interface in arg[3]. A single STA thread handles the page,
 * drains UI requests at about 30 Hz, and polls change-gated editor state.
 * Engine mutations use backend interface slots declared in snapmap_plus_iface.h. */
#include <windows.h>
#include <lmcons.h>
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
#include "WebView2EnvironmentOptions.h"
#include "snapmap_plus_iface.h"
#include "mockup_html.h"
#include "config_message.h"
#include "theme_bootstrap.h"
#include "rawmap_picker.h"
#include "prefab_host.h"    /* Create's precheck, a prefab's kind by its ~type, the live usage gate */
#include <knownfolders.h>
#include "report_scrub.h"   /* pure anonymization scrub + tail for the crash-report log attachment */
#include "crash_pending.h"
#include "log_rotate.h"     /* the UI log is append-only too; bound it like the backend's */
#include "host_image.h"     /* sh_host_renderer_name -- which renderer the player is actually running */
#include "prefab_grab.h"    /* SH_PREFAB_GRAB_*, and the saved-file check when the backend has no guard */
#include "prefab_block.h"   /* a prefab file's snapmapPlus block: stripped before the engine sees the text */
#include "game_identity.h"  /* the backend-provided game identity (startup argv) */
#include "webview_profile.h" /* the WebView2 profile folder under cache\ */
#include "folder_watch.h"   /* the debounce behind the prefabs folder watcher */
#include "player_text.h"    /* the description text a player sees from the generated tables */
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
/* The release this window was built as (root build.ps1 -Version); "dev" for a local build. */
#ifndef SNAPMAP_PLUS_VERSION
#define SNAPMAP_PLUS_VERSION "dev"
#endif
static const std::string        g_version = SNAPMAP_PLUS_VERSION;
/* Game identity from the backend ("" = backend not attached to a supported build). */
static std::string              g_game;
static unsigned int             g_config_status_flags = 0;
static bool                     g_config_status_posted = false;
/* The backend's interface table is not this window's (an install with files of two releases):
 * the window shows one refusal and touches nothing else. */
static bool                     g_pair_mismatch = false;


static bool          g_sync_on       = false;   /* "Synchronize with editor" checkbox */
static int           g_displayed_eid = -1;      /* entity the state panel is showing */
static int           g_last_editor_sel = -1;    /* reverse-apply guard */
static uint64_t      g_last_list_sig  = 0;
static uint64_t      g_last_state_sig = 0;
static uint64_t      g_last_sel_sig   = 0;       /* forward-sync: last editor-selection signature */


static volatile bool g_pending_save = false;
static int           g_save_eid     = -1;
static std::string   g_save_decl, g_save_class, g_save_inherit, g_save_dname;
static int           g_save_result  = 0;

static volatile bool g_pending_delete = false;
static std::vector<int> g_delete_eids;
/* The page announces nothing until it receives this result. NOT_RUN means the
 * main-thread call never started, so the engine was not touched. */
#define UI_DELETE_NOT_RUN (-1000)
static int              g_entity_delete_status = UI_DELETE_NOT_RUN;
static sh_delete_result g_entity_delete_result;

static volatile bool g_pending_select = false;   /* list -> editor selection push ("Select in editor") */
static std::vector<int> g_select_eids;
static volatile bool g_pending_deselect = false;  /* UI blank-space deselect -- clear_selection convenience */
static volatile int  g_select_refused   = 0;      /* refusal reason: 1 grab/hold, 2 native action finishing */

/* Camera-manipulation state for the editable footer controls. camSet queues a one-shot write; camLock
 * holds the supplied target through the per-frame writer until the page releases it. */
static volatile bool g_cam_lock = false;
static float         g_cam_xyz[3] = {0.0f, 0.0f, 0.0f};
static volatile bool g_cam_write_once = false;
static volatile bool g_cam_read_published = false;  /* force one readout after each page navigation */

static volatile bool g_pending_create_prefab = false;
static std::string   g_create_prefab_name;
static int           g_create_result = 0;        /* 1 ok; 0 empty editor selection; 2 not hovering an entity in selection; 3 no usable grab frame; -1 resolve/serialize/write failure */
static int           g_last_selcount = -1;       /* last broadcast editor-selection count (Create-button gating) */

static volatile bool g_pending_delete_prefab = false;
static std::string   g_delete_prefab_name, g_delete_prefab_folder;   /* folder="" -> root (prefabs/) */
static int           g_delete_result = 0;        /* 1 ok; 0 the file operation failed (g_delete_reason); -1 path refused */
static const wchar_t *g_delete_reason = L"";

static volatile bool g_pending_rename_prefab = false;
/* Timelines tree: one batch of display-name writes (folder moves and renames are
 * entity renames). Applied on the game thread beside the other main-thread edits. */
static volatile bool g_pending_set_names = false;
static std::vector<int>         g_set_names_eids;
static std::vector<std::string> g_set_names_names;
static int           g_set_names_applied = 0, g_set_names_failed = 0;
static std::string   g_rename_prefab_old, g_rename_prefab_new, g_rename_prefab_folder;
static int           g_rename_result = 0;        /* 1 ok; 0 the move failed (g_rename_reason); -1 path refused */
static const wchar_t *g_rename_reason = L"";

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
static int           g_pick_kind = 0;        /* 0 = Load Rawmap, 1 = Save Rawmap As, 2 = Export Prefab, 3 = Import Prefab */
static volatile LONG g_pick_mods = 0;        /* the open map's mods when the picker opened */
static bool          g_pick_ok   = false;    /* false = cancelled */
static std::wstring  g_pick_path;
static std::wstring  g_pick_file_name;       /* Export Prefab: the name the dialog proposes */
static std::string   g_pick_prefab_name, g_pick_prefab_folder;   /* Export Prefab: the prefab */
/* A Load/Place held while its mods are fetched (backend/prefab_mods.c): the
 * placement to poll, and the prefab to place again once they are installed. */
static int           g_mods_wait_seq = 0;
static std::string   g_mods_wait_name, g_mods_wait_folder;
/* Keep the latest sound request until the drain; an empty name stops playback.
 * The backend marshals live audio changes to the engine thread. */
static volatile bool g_pending_sound_preview = false;
static std::string   g_sound_preview_name;
static int           g_sound_preview_result = 0;
static std::string   g_sound_preview_reason;        /* why it was refused, '' when it was not */
/* Preview-mode session, held while the asset browser is on screen. Separate pending flag from the
 * play above so opening the browser and clicking Play in the same frame still arrive in order. */
static volatile bool g_pending_sound_session = false;
static volatile int  g_sound_session_on = 0;
static int           g_new_entity_result = -1;
static int           g_load_result = 0;          /* 1 queued for stage/place; 0/-1 = resolve, read, or queue failure.
                                                   * Queue acceptance does not confirm placement. */
/* Module prefabs: Blueprint modules saved with their entities (~type idSnapMap).
 * Load/Place of one routes to the backend's module_prefab slot instead of the
 * entity paste; its result arrives over later frames and is posted when known. */
static volatile bool g_pending_create_module = false;
static std::string   g_create_module_name;
static int           g_create_module_result = 0;  /* 1 ok; 0 name exists; -1 failed; -2 not in Blueprint; -3 no module selected */
static std::string   g_create_module_summary;     /* the view of the block the backend wrote */
static std::string   g_create_module_message;     /* the backend's text for a refused save */
static bool          g_load_prefab_modules = false;  /* the last Load/Place read a module prefab */
static int           g_module_place_seq = 0;      /* placement being followed, 0 = none */
static std::string   g_load_message;              /* the backend's text for a refused module placement */
static ULONGLONG     g_module_place_deadline = 0;
static std::string   g_module_place_name, g_module_place_folder;
static int           g_editor_mode_last = -1;     /* entityMode | blueprint << 1; -1 posts on the next check */
static int           g_editor_modules_last = -2;  /* modules selected in Blueprint when editorMode was last posted */
/* An object Load / Place waits for the game thread's decision (prefab_watch's place_seq) before the
 * page is told whether the prefab is in hand or only staged. */
static bool          g_place_wait = false;
static int           g_place_wait_seq = 0;
static ULONGLONG     g_place_wait_deadline = 0;
static std::string   g_place_wait_name, g_place_wait_folder;
/* The prefab the player may be holding in the 3D view, by kind; prefab_watch says whether one is. */
static std::string   g_held_obj_name, g_held_obj_folder;
static int           g_held_obj_seq = -1;         /* the paste decision it came from: a later kind-2 paste is not it */
static bool          g_usage_wanted = false;      /* the page has asked for the map's usage */
/* The folder the picker opens in, chosen on the UI thread before the dialog's thread starts. */
static std::wstring  g_pick_folder;

/* Folders: one real level of subdirectories under %LOCALAPPDATA%\snapmap-plus\prefabs\ (no nested-within-nested).
 * folder="" always means the root prefabs\ dir. The folder/file IS the truth -- no separate manifest. */
static volatile bool g_pending_create_folder = false;
static std::string   g_create_folder_name;
static int           g_create_folder_result = 0;   /* 1 ok; 0 not created (g_create_folder_reason); -1 name refused */
static const wchar_t *g_create_folder_reason = L"";

static volatile bool g_pending_rename_folder = false;
static std::string   g_rename_folder_old, g_rename_folder_new;
static int           g_rename_folder_result = 0;   /* 1 ok; 0 the move failed (g_rename_folder_reason); -1 name refused */
static const wchar_t *g_rename_folder_reason = L"";

static volatile bool g_pending_delete_folder = false;
static std::string   g_delete_folder_name;
static int           g_delete_folder_result = 0;   /* 1 ok (removed, any contents moved to root); 0 RemoveDirectory failed
                                                     * (a name collision at root left a file behind); -1 resolve failed */

static volatile bool g_pending_move_prefab = false;
static std::string   g_move_prefab_name, g_move_prefab_from, g_move_prefab_to;
static int           g_move_prefab_result = 0;      /* 1 ok; 0 the move failed (g_move_prefab_reason); -1 path refused */
static const wchar_t *g_move_prefab_reason = L"";
/* A multi-selection drop: every move in one UI tick, one result, one refresh. */
static volatile bool g_pending_move_prefabs = false;
static std::vector<std::string> g_move_prefabs_names, g_move_prefabs_from, g_move_prefabs_moved;
static std::string   g_move_prefabs_to;
static int           g_move_prefabs_failed = 0;
static const wchar_t *g_move_prefabs_reason = L"";   /* the first failure's */
static volatile bool g_pending_delete_prefabs = false;
static std::vector<std::string> g_delete_prefabs_names, g_delete_prefabs_folders;
static int           g_delete_prefabs_deleted = 0, g_delete_prefabs_failed = 0;
static const wchar_t *g_delete_prefabs_reason = L"";   /* the first failure's */

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

/* Every entity the editor holds, its reference and name whole. */
struct UiEnt { int eid; std::string id, name; int hidden; };
static std::vector<UiEnt> g_ents;

/* Rebuild the Timeline and Encounter Manager list when the entity signature changes. */
struct UiTl { int eid; std::string id, name; };
static std::vector<UiTl> g_tls;
static int    g_tl_count = 0;

/* Text a slot handed over (snapmap_plus_iface.h): copied whole, then released
 * through free_text. */
static void ui_free_text(void *text)
{
    if (text && g_iface && g_iface->vtbl && g_iface->vtbl->free_text) g_iface->vtbl->free_text(g_iface, text);
}
static std::string ui_take(char *text)
{
    std::string s(text ? text : "");
    ui_free_text(text);
    return s;
}
/* A packed name list (each name and its NUL, ended by an empty name) as names. */
static std::vector<std::string> ui_take_packed(char *text)
{
    std::vector<std::string> names;
    for (const char *p = text; p && *p; p += strlen(p) + 1) names.push_back(p);
    ui_free_text(text);
    return names;
}
static unsigned long g_list_seq = 0;

struct UiCollectPerf {
    unsigned calls, state_calls;
    unsigned long long collect_total_us, collect_max_us;
    unsigned long long poll_total_us, poll_max_us;
    unsigned long long selection_total_us, selection_max_us;
    unsigned long long state_total_us, state_max_us;
    int min_entities, max_entities, max_selection_count;
};
static UiCollectPerf g_collect_perf = {};

/* File logging. */
/* Keep the log name recognizable beside the backend log. */
static const char *kUiLogPath = "snapmap-plus\\logs\\snapmap-plus-ui.log";

static void ui_log(const char *msg)
{
    static bool rolled = false;
    CreateDirectoryA("snapmap-plus", nullptr);   /* one level at a time; both idempotent */
    CreateDirectoryA("snapmap-plus\\logs", nullptr);
    FILE *f = nullptr;
    if (!rolled) {
        rolled = true;
        log_rotate_if_large(kUiLogPath, LOG_ROTATE_CAP_BYTES);
    }
    if (fopen_s(&f, kUiLogPath, "a") == 0 && f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
        fclose(f);
    }
}
/* A formatted line, whole: sized to what it holds. */
static void ui_logp(const char *fmt, ...)
{
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int n = _vscprintf(fmt, copy);
    va_end(copy);
    std::string line(n > 0 ? (size_t)n : 0, '\0');
    if (n > 0) vsnprintf(&line[0], (size_t)n + 1, fmt, ap);
    va_end(ap);
    ui_log(line.c_str());
}
static void ui_logf(const char *fmt, unsigned long a) { ui_logp(fmt, a); }

static unsigned long long ui_perf_now_us()
{
    static LARGE_INTEGER freq = []() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0) return 0;
    unsigned long long ticks = (unsigned long long)now.QuadPart;
    unsigned long long hz = (unsigned long long)freq.QuadPart;
    return (ticks / hz) * 1000000ull + ((ticks % hz) * 1000000ull) / hz;
}

static void ui_perf_flush_collect(const char *reason)
{
    if (g_collect_perf.calls == 0) return;
    ui_logp("perf poll-window: end=%s calls=%u entities=%d..%d selected_max=%d total_avg_us=%llu total_max_us=%llu collect_avg_us=%llu collect_max_us=%llu selection_avg_us=%llu selection_max_us=%llu state_calls=%u state_avg_us=%llu state_max_us=%llu",
                reason ? reason : "unknown", g_collect_perf.calls,
                g_collect_perf.min_entities, g_collect_perf.max_entities,
                g_collect_perf.max_selection_count,
                g_collect_perf.poll_total_us / g_collect_perf.calls, g_collect_perf.poll_max_us,
                g_collect_perf.collect_total_us / g_collect_perf.calls, g_collect_perf.collect_max_us,
                g_collect_perf.selection_total_us / g_collect_perf.calls, g_collect_perf.selection_max_us,
                g_collect_perf.state_calls,
                g_collect_perf.state_calls ? g_collect_perf.state_total_us / g_collect_perf.state_calls : 0,
                g_collect_perf.state_max_us);
    g_collect_perf = {};
}

static void ui_perf_note_collect(unsigned long long us, int entities, const char *reason)
{
    if (!reason || strcmp(reason, "poll") != 0) {
        ui_logp("perf collect: reason=%s entities=%d duration_us=%llu",
                    reason ? reason : "unknown", entities, us);
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
        ui_logp("perf collect-slow: entities=%d duration_us=%llu", entities, us);
    }
}

static void ui_perf_note_poll(unsigned long long total_us, unsigned long long collect_us,
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
        ui_logp("perf poll-slow: entities=%d selected=%d total_us=%llu collect_us=%llu selection_us=%llu state_us=%llu",
                    entities, selection_count, total_us, collect_us, selection_us,
                    state_called ? state_us : 0);
    }
    if (g_collect_perf.calls >= 90) ui_perf_flush_collect("30-second-window");
}

/* Change signatures. */
static uint64_t hstr(uint64_t h, const char *s) { while (*s) { h = (h ^ (unsigned char)*s) * 1099511628211ull; s++; } return h; }
static uint64_t hint(uint64_t h, int v) { for (int i = 0; i < 4; i++) { h = (h ^ (unsigned char)(v & 0xff)) * 1099511628211ull; v >>= 8; } return h; }

/* Validate each name component before resolve_prefab_path concatenates it
 * (prefab_host.h): a name Windows can hold, with no separator or traversal. */
static bool ui_valid_name(const std::string &n) { return sh_prefab_host::valid_name(n); }

/* Paths are UTF-8 inside the window and across the backend interface, and wide at
 * every Windows call, so any user, folder or prefab name opens. */
static std::wstring ui_widen(const char *utf8)
{
    std::wstring w;
    if (!utf8) return w;
    int wl = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wl > 0) { w.resize(wl - 1); if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], wl); }
    return w;
}
/* A UTF-8 path for the wide file calls, in the long form ("\\?\C:\..." or "\\?\UNC\...")
 * when it is absolute, so a path of any depth Windows accepts opens. */
static std::wstring ui_wpath(const char *utf8)
{
    std::wstring w = ui_widen(utf8);
    for (wchar_t &c : w) if (c == L'/') c = L'\\';
    if (w.compare(0, 4, L"\\\\?\\") == 0) return w;
    if (w.size() >= 3 && w[1] == L':' && w[2] == L'\\') return L"\\\\?\\" + w;
    if (w.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return w;
}
/* %LOCALAPPDATA%\snapmap-plus, whole, or "" when the folder is unknown. */
static std::wstring ui_local_snapmap_dir()
{
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) dir = std::wstring(p) + L"\\snapmap-plus";
    if (p) CoTaskMemFree(p);
    return dir;
}
static FILE *ui_open(const char *utf8_path, const wchar_t *mode)
{
    FILE *f = nullptr;
    if (!utf8_path || _wfopen_s(&f, ui_wpath(utf8_path).c_str(), mode) != 0) return nullptr;
    return f;
}
static bool ui_exists(const char *utf8_path) { return GetFileAttributesW(ui_wpath(utf8_path).c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool ui_move(const char *from, const char *to) { return MoveFileW(ui_wpath(from).c_str(), ui_wpath(to).c_str()) != 0; }
/* Why the file operation just made failed, as the page names it: "exists" (the name is taken),
 * "inUse" (another program holds the file), "missing" (it is gone), "notEmpty" (a folder kept a
 * file), or "" (another Windows error, logged with its code). Call right after the failure. */
static const wchar_t *ui_fs_reason()
{
    DWORD e = GetLastError();
    switch (e) {
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return L"exists";
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_ACCESS_DENIED: return L"inUse";
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return L"missing";
    case ERROR_DIR_NOT_EMPTY: return L"notEmpty";
    default: {
        ui_logp("file operation failed: Windows error %lu", (unsigned long)e);
        return L"";
    }
    }
}
/* %LOCALAPPDATA%\snapmap-plus, created when missing; "" when Windows cannot say. */
static std::wstring ui_data_dir()
{
    PWSTR la = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la)) && la) dir = std::wstring(la) + L"\\snapmap-plus";
    if (la) CoTaskMemFree(la);
    if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);   /* no-op when it already exists */
    return dir;
}
/* Keep the user's asset pins in pinned.json, separate from validated settings.
 * A damaged pin list must not reset unrelated preferences. The host transports
 * opaque bytes; the page owns parsing, validation, and the empty-list fallback. */
static std::wstring ui_pins_path()
{
    std::wstring dir = ui_data_dir();
    return dir.empty() ? dir : dir + L"\\pinned.json";
}

static void ui_send_pins()
{
    std::string data;
    std::wstring path = ui_pins_path();
    if (!path.empty()) {
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
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
static void ui_save_pins(const std::string &doc)
{
    std::wstring path = ui_pins_path();
    if (path.empty()) return;
    std::wstring tmp = path + L".tmp";
    FILE *f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) { ui_log("pins: could not open the temp file"); return; }
    size_t w = doc.empty() ? 0 : fwrite(doc.data(), 1, doc.size(), f);
    if (fclose(f) != 0) w = 0;
    if (w != doc.size()) { DeleteFileW(tmp.c_str()); ui_log("pins: short write, kept the previous file"); return; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        ui_log("pins: could not replace pinned.json");
    }
}


/* Engine reads and edits run on DOOM's main thread while this thread waits. */
#define UI_MAIN_WAIT_MS      1000   /* a user action; allows for a slow frame */
#define UI_MAIN_CAM_WAIT_MS   100   /* a held camera writes again next pass */
static bool ui_on_main(sh_main_call_fn fn, void *ctx, int timeout_ms)
{
    int r = 0;
    return g_iface && g_iface->vtbl && g_iface->vtbl->run_on_main &&
           g_iface->vtbl->run_on_main(g_iface, fn, ctx, timeout_ms, &r) == 1;
}

/* Guarded engine access. */
static int ui_editor_ready()
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
/* The entity reads, SEH-guarded with PODs only in scope (MSVC forbids __try beside C++
 * locals that unwind). A faulting read reads as absent. */
static int ui_entity_count_seh(int *out_ready)
{
    __try {
        if (!g_iface || !g_iface->vtbl) return 0;
        if (g_iface->vtbl->editor_ready_poll) *out_ready = g_iface->vtbl->editor_ready_poll(g_iface) ? 1 : 0;
        return g_iface->vtbl->entity_count ? g_iface->vtbl->entity_count(g_iface) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ui_valid_id_seh(int id)
{
    __try { return !g_iface->vtbl->is_valid_id || g_iface->vtbl->is_valid_id(g_iface, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static char *ui_id_string_seh(int id)
{
    __try { return g_iface->vtbl->id_to_string ? g_iface->vtbl->id_to_string(g_iface, id) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static char *ui_displayname_seh(int id)
{
    __try { return g_iface->vtbl->get_displayname ? g_iface->vtbl->get_displayname(g_iface, id) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static char *ui_classname_seh(int id)
{
    __try { return g_iface->vtbl->get_classname_copy ? g_iface->vtbl->get_classname_copy(g_iface, id) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static char *ui_inherit_seh(int id)
{
    __try { return g_iface->vtbl->get_inherit_copy ? g_iface->vtbl->get_inherit_copy(g_iface, id) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static char *ui_declsource_seh(int id)
{
    __try { return g_iface->vtbl->get_declsource_copy ? g_iface->vtbl->get_declsource_copy(g_iface, id) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static int ui_dev_hidden_seh(int id)
{
    __try { return g_iface->vtbl->id_dev_layer_hidden && g_iface->vtbl->id_dev_layer_hidden(g_iface, id) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ui_collect(int *out_ready)
{
    *out_ready = 0;
    g_ents.clear();
    if (!g_iface || !g_iface->vtbl) return 0;
    int count = ui_entity_count_seh(out_ready);
    for (int id = 0; id < count; id++) {
        if (!ui_valid_id_seh(id)) continue;
        std::string s = ui_take(ui_id_string_seh(id));
        if (s.empty() || s.compare(0, 5, "NULL_") == 0) continue;
        UiEnt e;
        e.eid = id;
        e.id = s;
        e.name = ui_take(ui_displayname_seh(id));
        e.hidden = ui_dev_hidden_seh(id);
        g_ents.push_back(e);
    }
    return (int)g_ents.size();
}
static int ui_collect_timed(int *out_ready, const char *reason,
                             unsigned long long *elapsed_us = nullptr)
{
    unsigned long long started = ui_perf_now_us();
    int n = ui_collect(out_ready);
    unsigned long long finished = ui_perf_now_us();
    unsigned long long elapsed = finished >= started ? finished - started : 0;
    if (elapsed_us) *elapsed_us = elapsed;
    ui_perf_note_collect(elapsed, n, reason);
    return n;
}
/* Rebuild timeline entries only when the entity signature changes. Check every
 * visible entity's class so Encounter Managers are included alongside Timelines.
 * Guard each class read so one bad entity cannot abort the rescan. */
static void ui_rescan_timelines(int n)
{
    g_tls.clear();
    g_tl_count = 0;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->get_classname_copy) return;
    for (int i = 0; i < n; i++) {
        if (g_ents[i].hidden) continue;   /* dev-layer hidden -> excluded from both lists, as the editor hides it */
        std::string c = ui_take(ui_classname_seh(g_ents[i].eid));
        if (c == "idTarget_Timeline" || c == "idEncounterManager") {
            UiTl t;
            t.eid = g_ents[i].eid;
            t.id = g_ents[i].id;
            t.name = g_ents[i].name;
            g_tls.push_back(t);
        }
    }
    g_tl_count = (int)g_tls.size();
}
static void ui_rescan_timelines_timed(int n, const char *reason)
{
    unsigned long long started = ui_perf_now_us();
    ui_rescan_timelines(n);
    unsigned long long finished = ui_perf_now_us();
    ui_logp("perf timeline-rescan: reason=%s entities=%d timelines=%d duration_us=%llu",
                reason ? reason : "unknown", n, g_tl_count,
                finished >= started ? finished - started : 0);
}
/* The state panel's four texts, whole. */
static bool ui_collect_state(int id, std::string &decl, std::string &cls, std::string &inh, std::string &dnm)
{
    decl.clear(); cls.clear(); inh.clear(); dnm.clear();
    if (!g_iface || !g_iface->vtbl || !ui_valid_id_seh(id)) return false;
    decl = ui_take(ui_declsource_seh(id));
    cls = ui_take(ui_classname_seh(id));
    inh = ui_take(ui_inherit_seh(id));
    dnm = ui_take(ui_displayname_seh(id));
    return true;
}
static void ui_post_json(const wchar_t *json);   /* fwd */

/* Camera Origin: write path (+0x00). Used every frame while Lock position is enabled and once per
 * committed camSet edit. SEH-guarded. */
static void ui_cam_write()
{
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->set_editor_vec3)
            g_iface->vtbl->set_editor_vec3(g_iface, g_cam_xyz);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
/* Camera Origin: read the live vec3 (+0x08); if it moved, cache + push it to the footer. SEH-guarded. */
static void ui_cam_read_send()
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
    ui_post_json(m);
    g_cam_read_published = true;
}

static int ui_get_selection_seh(int *out, int max)
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
/* The whole selection. It never holds more ids than the editor has entities (the
 * engine's own table), so that count sizes the read. */
static int ui_get_selection(std::vector<int> &ids)
{
    int ready = 0, count = ui_entity_count_seh(&ready);
    ids.assign((size_t)(count > 0 ? count : 0) + 1, 0);
    ids.resize((size_t)ui_get_selection_seh(ids.data(), (int)ids.size()));
    return (int)ids.size();
}
static void ui_apply_save()
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
/* Window deletion runs DOOM's own Delete Selection through one backend call
 * (ext 29). A backend without that slot refuses rather than falling back to
 * the old per-ID path, which skipped the engine's hover, selection, undo and
 * eligibility handling. Runs on the main thread inside ui_main_edits. */
static void ui_apply_deletes()
{
    memset(&g_entity_delete_result, 0, sizeof g_entity_delete_result);
    g_entity_delete_status = SH_DELETE_UNAVAILABLE;
    __try {
        if (SH_IFACE_HAS_SLOT(g_iface, delete_entities) && !g_delete_eids.empty())
            g_entity_delete_status = g_iface->vtbl->delete_entities(g_iface, g_delete_eids.data(),
                                                                    (int)g_delete_eids.size(),
                                                                    &g_entity_delete_result);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_entity_delete_status = SH_DELETE_FAULT; }
}
/* Serialize under SEH (PODs only in scope): the entity's JSON as the slot's owned text,
 * NULL when it could not be read, or SH_UI_SERIALIZE_THREW when the engine faulted. */
#define SH_UI_SERIALIZE_THREW ((char *)(intptr_t)-1)
static char *ui_serialize_entity_seh(int id)
{
    __try { return g_iface->vtbl->serialize_entity(g_iface, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return SH_UI_SERIALIZE_THREW; }
}
/* The entity's JSON, whole, into buf; its length, 0 when it could not be read. */
static int ui_serialize_entity_whole(int id, std::string &buf, const char *what)
{
    char l[256];
    buf.clear();
    if (!(g_iface && g_iface->vtbl && g_iface->vtbl->serialize_entity && id >= 0)) {
        _snprintf_s(l, sizeof l, _TRUNCATE, "%s: entity %d NOT ATTEMPTED -- interface/vtable missing or bad id", what, id);
        ui_log(l);
        return 0;
    }
    char *json = ui_serialize_entity_seh(id);
    if (json == SH_UI_SERIALIZE_THREW) {
        _snprintf_s(l, sizeof l, _TRUNCATE, "%s: entity %d RAISED inside serialize_entity -- engine fault", what, id);
        ui_log(l);
        return 0;
    }
    buf = ui_take(json);
    if (buf.empty()) {
        _snprintf_s(l, sizeof l, _TRUNCATE, "%s: entity %d could not be read", what, id);
        ui_log(l);
    }
    return (int)buf.size();
}
/* Serialize an entity on DOOM's main thread, whole. 0 when it failed or the game did not
 * run the request. */
struct UiSerializeMain { int id; std::string *buf; const char *what; int n; };
static int ui_serialize_entity_main(void *ctx)
{
    UiSerializeMain *m = (UiSerializeMain *)ctx;
    m->n = ui_serialize_entity_whole(m->id, *m->buf, m->what);
    return 1;
}
static int ui_serialize_entity_on_main(int id, std::string &buf, const char *what)
{
    UiSerializeMain m = { id, &buf, what, 0 };
    if (!ui_on_main(ui_serialize_entity_main, &m, UI_MAIN_WAIT_MS)) {
        ui_logp("%s: entity %d not read -- the game did not run the request within the wait", what, id);
        return 0;
    }
    return m.n;
}
static std::string g_tl_json;
static int ui_serialize_entity_raw(int id) { return ui_serialize_entity_on_main(id, g_tl_json, "timeline-open"); }
/* Post {kind:"timelineData", eid, ok, json:"<the serialized entity JSON, escaped>"}. The page JSON.parses
 * `json` and walks entityDef.state.edit.componentTimeLine / encounterComponent itself (the engine's
 * serialized entity is valid JSON). */
static void ui_emit_timeline_data(int eid, int json_len)
{
    if (!g_webview) return;
    bool ok = json_len > 0;
    std::wstring m = L"{\"kind\":\"timelineData\",\"eid\":"; m += std::to_wstring(eid);
    m += L",\"ok\":"; m += ok ? L"true" : L"false";
    m += L",\"json\":\""; if (ok) m += sh_webview_json::escape_wide(g_tl_json.c_str()); m += L"\"}";
    g_webview->PostWebMessageAsJson(m.c_str());
}
/* Serialize the event's target entity into its own buffer. The page reads
 * entityDef.inherit to select the appropriate model/animation choices. */
static std::string g_resolve_json;
static int ui_serialize_entity_resolve(int id) { return ui_serialize_entity_on_main(id, g_resolve_json, "entity-resolve"); }
static void ui_emit_entity_inherit(int eid, int json_len)
{
    if (!g_webview) return;
    bool ok = json_len > 0;
    std::wstring m = L"{\"kind\":\"entityInherit\",\"eid\":"; m += std::to_wstring(eid);
    m += L",\"ok\":"; m += ok ? L"true" : L"false";
    m += L",\"json\":\""; if (ok) m += sh_webview_json::escape_wide(g_resolve_json.c_str()); m += L"\"}";
    g_webview->PostWebMessageAsJson(m.c_str());
}
/* Replace editor selection from the list. Log each call to locate a stalled slot. */
static void ui_apply_select_in_editor()
{
    ui_logf("select-in-editor: apply start ids=%lu", (unsigned long)g_select_eids.size());
    /* Manipulation snapshots index the selection positionally, and a native
     * Delete owns the selection until it commits. The backend enforces this
     * gate too; its value says which one refused. */
    int busy = 0;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->manipulation_in_progress)
        busy = g_iface->vtbl->manipulation_in_progress(g_iface);
    if (busy) {
        ui_log(busy == 2 ? "select-in-editor: REFUSED -- a native editor action is finishing"
                          : "select-in-editor: REFUSED -- editor is mid-manipulation (grab/hold)");
        g_select_refused = busy;
        return;
    }
    __try {
        if (g_iface && g_iface->vtbl) {
            if (g_iface->vtbl->clear_selection) { ui_log("select-in-editor: clear"); g_iface->vtbl->clear_selection(g_iface); }
            if (g_iface->vtbl->add_to_selection)
                for (size_t i = 0; i < g_select_eids.size(); i++) {
                    int id = g_select_eids[i];
                    if (id < 0) continue;
                    if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) { ui_logf("select-in-editor: skip invalid id=%lu", (unsigned long)id); continue; }
                    ui_logf("select-in-editor: add id=%lu", (unsigned long)id);
                    g_iface->vtbl->add_to_selection(g_iface, id);
                }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ui_log("select-in-editor: SEH in apply"); }
    ui_log("select-in-editor: apply done");
}
/* Mirror a page blank-space click by clearing the editor selection. */
static void ui_apply_deselect()
{
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->clear_selection) g_iface->vtbl->clear_selection(g_iface);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ui_log("deselect: SEH in apply"); }
}
struct UiMainEdits { bool save, del, select, deselect, cam, names; };
/* Write each display name through the setter slot. PODs only in scope (SEH). */
static int ui_set_one_name_seh(int id, const char *name)
{
    __try {
        if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->set_entity_0x170) return 0;
        if (g_iface->vtbl->is_valid_id && !g_iface->vtbl->is_valid_id(g_iface, id)) return 0;
        g_iface->vtbl->set_entity_0x170(g_iface, id, name);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void ui_apply_set_display_names()
{
    g_set_names_applied = 0; g_set_names_failed = 0;
    for (size_t i = 0; i < g_set_names_eids.size() && i < g_set_names_names.size(); i++) {
        if (ui_set_one_name_seh(g_set_names_eids[i], g_set_names_names[i].c_str())) g_set_names_applied++;
        else g_set_names_failed++;
    }
}
/* Report the backend's delete result. The page changes its selection and
 * announces success only from this message; "timeout" means the request never
 * reached the game thread, so nothing was attempted. */
static void ui_post_delete_result()
{
    const sh_delete_result &r = g_entity_delete_result;
    wchar_t m[320];
    char l[256];
    if (g_entity_delete_status == UI_DELETE_NOT_RUN) {
        ui_post_json(L"{\"kind\":\"deleteResult\",\"status\":\"timeout\"}");
        ui_log("delete: the game did not run the request within the wait; nothing was deleted");
        return;
    }
    _snwprintf_s(m, _countof(m), _TRUNCATE,
                 L"{\"kind\":\"deleteResult\",\"status\":%d,\"busy\":%d,\"requested\":%d,\"targets\":%d,"
                 L"\"released\":%d,\"firstId\":%d,\"deselected\":%d,\"rolledBack\":%d}",
                 g_entity_delete_status, r.busy, r.requested, r.targets, r.released, r.first_id,
                 r.deselected, r.rolled_back);
    ui_post_json(m);
    _snprintf_s(l, sizeof l, _TRUNCATE,
                "delete: status=%d busy=%d requested=%d targets=%d released=%d first=%d "
                "deselected=%d hover_cleared=%d wire_cleared=%d rolled_back=%d",
                g_entity_delete_status, r.busy, r.requested, r.targets, r.released, r.first_id,
                r.deselected, r.hover_cleared, r.wire_cleared, r.rolled_back);
    ui_log(l);
}
static int ui_main_edits(void *ctx)
{
    const UiMainEdits *e = (const UiMainEdits *)ctx;
    if (e->save)     ui_apply_save();
    if (e->del)      ui_apply_deletes();
    if (e->select)   ui_apply_select_in_editor();
    if (e->deselect) ui_apply_deselect();
    if (e->names)    ui_apply_set_display_names();
    if (e->cam)      ui_cam_write();
    return 1;
}
/* __try can't share a function with a C++ object needing unwinding (/EHsc, C2712) -- this leaf has only
 * PODs in scope, so the SEH guard around the engine call is safe here. */
static char *ui_serialize_selection_raw()
{
    __try { return g_iface->vtbl->serialize_selection(g_iface); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static int ui_hovered_id_seh()
{
    __try { return g_iface->vtbl->hovered_id(g_iface); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
/* The Prefabs tab's watch (+0x380): the Blueprint module selection, what is held and the last
 * paste decision. An older backend reads as unknown (modules_selected -1, nothing held). */
static int ui_prefab_watch_seh(sh_prefab_watch *w)
{
    memset(w, 0, sizeof *w);
    w->modules_selected = -1;
    if (!SH_IFACE_HAS_SLOT(g_iface, prefab_watch)) return -1;
    __try { return g_iface->vtbl->prefab_watch(g_iface, w); }
    __except (EXCEPTION_EXECUTE_HANDLER) { memset(w, 0, sizeof *w); w->modules_selected = -1; return -1; }
}
/* The watch, with its reason (owned by the slot) taken whole into reason. */
static int ui_prefab_watch(sh_prefab_watch *w, std::string *reason = nullptr)
{
    int r = ui_prefab_watch_seh(w);
    std::string text = ui_take(w->place_reason);
    w->place_reason = nullptr;
    if (reason) *reason = text;
    return r;
}
/* The backend's grab-frame guard verdict on the serialize just made, or
 * UNCHECKED when this backend has no guard. */
static int ui_prefab_grab_status(sh_prefab_grab_info *info)
{
    memset(info, 0, sizeof *info);
    info->outside = -1.0f;
    if (!SH_IFACE_HAS_SLOT(g_iface, prefab_grab_status)) return SH_PREFAB_GRAB_UNCHECKED;
    __try { return g_iface->vtbl->prefab_grab_status(g_iface, info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return SH_PREFAB_GRAB_UNCHECKED; }
}
/* Save the current selection as prefab JSON. The engine requires the hovered
 * entity to be selected; report that refusal separately from an empty selection.
 * The engine anchors a prefab on the view-centre line, not the cursor; when
 * that line misses the hovered entity the backend rebuilds the frame from it
 * (reported as corrected). Without that guard, a file whose frame missed is
 * refused rather than written with collapsed positions.
 * The selection, hover and serialize run together on DOOM's main thread; the
 * file is written here.
 * Results: 1 = saved, 0 = empty, 2 = nothing hovered, 3 = no usable grab frame,
 * 4 = the hovered entity is not selected, 5 = the copy check refused a selection
 * it was otherwise given, 6 = the file could not be written whole, 7 = the game
 * did not run the request, -1 = the name or its path was refused. */
static bool ui_prefab_meta_path(const std::string &folder, const std::string &name, std::string &out);   /* fwd */
static int ui_module_prefab_call(sh_module_prefab_call *call, std::string *out = nullptr);   /* fwd */
struct UiCreateMain {
    char *json;   /* the slot's owned text */
    int  count, hovered, pre, grab_status;
    sh_prefab_grab_info grab;
};
/* Runs on DOOM's main thread inside run_on_main. */
static int ui_create_prefab_main(void *ctx)
{
    UiCreateMain *c = (UiCreateMain *)ctx;
    /* DOOM's copy check refuses an empty selection and a hovered entity that is not
     * selected alike; tell the page which (prefab_host.h). */
    std::vector<int> sel;
    c->count = ui_get_selection(sel);
    bool have_hover = g_iface->vtbl->hovered_id != nullptr;
    c->hovered = have_hover ? ui_hovered_id_seh() : -1;
    c->pre = sh_prefab_host::create_precheck(sel.data(), c->count, c->hovered, have_hover);
    if (c->pre != sh_prefab_host::CREATE_GO) return 1;
    c->json = ui_serialize_selection_raw();
    c->grab_status = ui_prefab_grab_status(&c->grab);
    return 1;
}
static void ui_apply_create_prefab()
{
    g_create_result = -1;
    { ui_logp("create-prefab: START name='%s'", g_create_prefab_name.c_str()); }
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path || !g_iface->vtbl->serialize_selection || g_create_prefab_name.empty()) {
        ui_log("create-prefab: ABORT (iface/slot/name missing)");
        return;
    }
    if (!ui_valid_name(g_create_prefab_name)) {
        ui_log("create-prefab: ABORT (the name is not one Windows can hold)");
        return;
    }
    std::string fname = g_create_prefab_name + ".json";
    std::string path = ui_take(g_iface->vtbl->resolve_prefab_path(g_iface, "prefabs/", fname.c_str()));
    if (path.empty()) {
        ui_log("create-prefab: ABORT (resolve_prefab_path failed)");
        return;
    }
    UiCreateMain cm;
    memset(&cm, 0, sizeof cm);
    cm.pre = sh_prefab_host::CREATE_GO;
    if (!ui_on_main(ui_create_prefab_main, &cm, UI_MAIN_WAIT_MS)) {
        ui_log("create-prefab: ABORT (the game did not run the request within the wait)");
        g_create_result = 7;
        return;
    }
    if (cm.pre != sh_prefab_host::CREATE_GO) {
        ui_logp("create-prefab: ABORT (%s; %d selected, hovered %d)",
                    cm.pre == sh_prefab_host::CREATE_EMPTY ? "nothing selected"
                    : cm.pre == sh_prefab_host::CREATE_NO_HOVER ? "not hovering an entity in the selection"
                    : "hovering an entity that is not selected", cm.count, cm.hovered);
        g_create_result = cm.pre;
        return;
    }
    std::string buf = ui_take(cm.json);
    int n = (int)buf.size();
    int grab_status = cm.grab_status;
    const sh_prefab_grab_info &grab = cm.grab;
    ui_logf("create-prefab: serialize_selection returned n=%lu", (unsigned long)n);
    if (n <= 0) {
        if (grab_status == SH_PREFAB_GRAB_REFUSED) {
            ui_log("create-prefab: ABORT (no finite grab frame could be built from the hovered entity)");
            g_create_result = 3;
        } else {
            /* A selection and a selected hover: the copy check refused for its other causes
             * (a wired or referenced entity left out, or one the editor does not copy). */
            ui_log("create-prefab: ABORT (the editor's copy check refused the selection)");
            g_create_result = 5;
        }
        return;
    }
    if (grab_status == SH_PREFAB_GRAB_CORRECTED) {
        ui_logp("create-prefab: grab point corrected -- the view-centre line missed the hovered entity "
                    "(native grabDistance=%g, %g units outside it); saved grabDistance=%g from its centre",
                    (double)grab.native_distance, (double)grab.outside, (double)grab.distance);
    } else if (grab_status == SH_PREFAB_GRAB_UNCHECKED) {
        double d = 0.0;
        if (sh_prefab_grab_json_check(buf.c_str(), (size_t)n, &d) == 0) {
            ui_logp("create-prefab: ABORT (grabDistance=%g: the view-centre line missed the hovered entity and "
                        "this backend cannot rebuild the frame)", d);
            g_create_result = 3;
            return;
        }
    }
    /* A prefab file is plain JSON: the serializer's comments before each member go
     * (backend/prefab_block.h); readers still skip them in files earlier builds wrote. */
    {
        size_t before = (size_t)n;
        n = (int)sh_prefab_block_strip_comments(&buf[0], before);
        buf.resize((size_t)n);
        ui_logp("create-prefab: comments removed, %lu byte(s) -> %lu",
                    (unsigned long)before, (unsigned long)n);
    }
    FILE *fp = ui_open(path.c_str(), L"wb");
    if (!fp) { ui_log("create-prefab: ABORT (the file could not be opened for writing)"); return; }
    size_t wrote = fwrite(buf.data(), 1, (size_t)n, fp);
    if (fclose(fp) != 0 || wrote != (size_t)n) {
        /* A short write leaves a damaged prefab: remove it and report the failure. */
        DeleteFileW(ui_wpath(path.c_str()).c_str());
        ui_logf("create-prefab: ABORT (short write: %lu byte(s) written); the partial file was removed", (unsigned long)wrote);
        g_create_result = 6;
        return;
    }
    ui_log("create-prefab: WROTE file ok");
    g_create_result = 1;
    /* One file: the backend writes the prefab's snapmapPlus block into it, with the mods
     * its entities need, found as a map's save finds them. The engine never re-saves it. */
    {
        sh_module_prefab_call call;
        memset(&call, 0, sizeof call);
        call.op = SH_MODULE_PREFAB_MODS_RECORD;
        call.text = buf.c_str();
        call.text_len = (size_t)n;
        call.path = path.c_str();
        int r = ui_module_prefab_call(&call);
        ui_logf("create-prefab: mods recorded in the file's block=%lu", (unsigned long)(r > 0 ? r : 0));
    }
}
/* folder="" -> the root prefabs\ dir; else prefabs\<folder>\ (one real level, no nesting). Shared by every
 * prefab/folder file op below so they all agree on where a folder actually lives on disk. */
static bool ui_prefab_dir(const std::string &folder, std::string &out)
{
    out.clear();
    if (!folder.empty() && !ui_valid_name(folder)) return false;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path) return false;
    std::string prefix = folder.empty() ? "prefabs/" : ("prefabs/" + folder + "/");
    out = ui_take(g_iface->vtbl->resolve_prefab_path(g_iface, prefix.c_str(), ""));
    return !out.empty();
}
static bool ui_prefab_file_path(const std::string &folder, const std::string &name, std::string &out)
{
    out.clear();
    if (!folder.empty() && !ui_valid_name(folder)) return false;
    if (!ui_valid_name(name)) return false;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->resolve_prefab_path) return false;
    std::string prefix = folder.empty() ? "prefabs/" : ("prefabs/" + folder + "/");
    std::string fname = name + ".json";
    out = ui_take(g_iface->vtbl->resolve_prefab_path(g_iface, prefix.c_str(), fname.c_str()));
    return !out.empty();
}
/* A prefab is one file: its description, tags, summary and mods live in its snapmapPlus
 * block (backend/prefab_block.h). <name>.meta.json is only the read-only fallback of a
 * prefab saved before the block; it is never written, and it follows its prefab when the
 * prefab is moved, renamed or deleted. The validated path helper handles both filenames. */
static bool ui_prefab_meta_path(const std::string &folder, const std::string &name, std::string &out)
{
    return ui_prefab_file_path(folder, name + ".meta", out);
}
static bool ui_read_small_file(const char *path, std::string &body);   /* fwd */
/* Save the selected Blueprint modules, with every entity inside them, as a module
 * prefab at the prefab root. The backend writes the file, its summary and mods in its block.
 * Results: 1 saved, 0 the name exists, -1 failed, -2 not in Blueprint, -3 no module selected. */
static void ui_apply_create_module_prefab()
{
    g_create_module_result = -1;
    g_create_module_summary.clear();
    g_create_module_message.clear();
    if (g_create_module_name.empty() || !ui_valid_name(g_create_module_name)) {
        ui_log("create-module-prefab: ABORT (name rejected)");
        return;
    }
    std::string path, out;
    if (!ui_prefab_file_path("", g_create_module_name, path)) {
        ui_log("create-module-prefab: ABORT (resolve_prefab_path failed)");
        return;
    }
    if (ui_exists(path.c_str())) {
        g_create_module_result = 0;
        ui_log("create-module-prefab: REFUSED (a prefab with that name exists)");
        return;
    }
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_SAVE;
    call.path = path.c_str();
    int r = ui_module_prefab_call(&call, &out);
    g_create_module_result = (r == 1 || r == -2 || r == -3) ? r : -1;
    if (r == 1) g_create_module_summary = out;
    else g_create_module_message = out;   /* the page shows it when it is a sentence */
    ui_log(("create-module-prefab: name='" + g_create_module_name + "' result=" + std::to_string(r) + " " +
            (r == 1 ? std::string() : out)).c_str());
}
/* Post the module-prefab placement result once the backend has it. */
static void ui_poll_module_place()
{
    if (!g_module_place_seq) return;
    std::string msg;
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_STATUS;
    call.seq = g_module_place_seq;
    int r = ui_module_prefab_call(&call, &msg);
    bool late = GetTickCount64() > g_module_place_deadline;
    if (r == 2 && !late) return;
    if (r == 2) r = 0;
    std::wstring m = L"{\"kind\":\"loadPrefabResult\",\"result\":"; m += std::to_wstring(r);
    m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_module_place_name.c_str());
    m += L"\",\"prefabKind\":\"modules\",\"message\":\""; m += sh_webview_json::escape_wide(msg.c_str()); m += L"\"}";
    ui_post_json(m.c_str());
    ui_log(("load-prefab: module placement " + std::to_string(g_module_place_seq) + " finished " + std::to_string(r) +
            " (" + msg + ")").c_str());
    g_module_place_seq = 0;
}
/* Tell the page which editor mode is up: Object Mode gates object prefabs, Blueprint
 * gates module prefabs, and in Blueprint the modules selected gate Create (-1 when
 * this backend cannot say). Posts on a change, or always when forced. */
static void ui_post_editor_mode(bool force)
{
    int entity = 0, blueprint = 0, map_open = 0;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->is_entity_mode) {
        __try { entity = g_iface->vtbl->is_entity_mode(g_iface) ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { entity = 0; }
    }
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_STATE;
    if (ui_module_prefab_call(&call) == 1) { blueprint = call.blueprint ? 1 : 0; map_open = call.map_open ? 1 : 0; }
    /* The modules selected in Blueprint gate Create there; the entity count rides selCount. */
    sh_prefab_watch watch;
    ui_prefab_watch(&watch);
    int modules = blueprint ? watch.modules_selected : 0;
    int now = entity | (blueprint << 1) | (map_open << 2);
    if (!force && now == g_editor_mode_last && modules == g_editor_modules_last) return;
    g_editor_mode_last = now;
    g_editor_modules_last = modules;
    wchar_t m[128];
    _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"editorMode\",\"entityMode\":%d,\"blueprint\":%d,\"modules\":%d,\"mapOpen\":%d}",
                 entity, blueprint, modules, map_open);
    ui_post_json(m);
}
static void ui_strip_trailing_sep(std::string &s)
{
    if (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
}
/* list the *.json stems directly inside a resolved directory (non-recursive), sorted. */
static void ui_list_json_dir(const std::string &dirPath, std::vector<std::string> &names)
{
    std::wstring pattern = ui_wpath(dirPath.c_str()) + L"*.json";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fn = sh_webview_json::to_utf8(fd.cFileName);
        if (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".json") == 0) {
            std::string stem = fn.substr(0, fn.size() - 5);
            /* "<name>.meta.json" is a metadata sidecar, not a prefab -- never list it as one */
            if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".meta") == 0) continue;
            names.push_back(stem);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
}

/* Move a file to the Windows Recycle Bin (SHFileOperationW with FOF_ALLOWUNDO,
 * no dialogs). Falls back to an outright delete when the shell refuses, so a
 * file is always gone afterwards when this returns 1. 0 = still there. */
static int ui_recycle_file(const char *path)
{
    /* The shell's recycle call takes a path of at most MAX_PATH (Windows' own limit for
     * SHFileOperation); a longer one is deleted outright, and the log says so. */
    wchar_t full[MAX_PATH + 2];
    std::wstring wide = ui_widen(path);
    if (!wide.empty()) {
        DWORD len = GetFullPathNameW(wide.c_str(), MAX_PATH, full, NULL);
        if (len > 0 && len < MAX_PATH) {
            full[len + 1] = 0;   /* double NUL: one path */
            SHFILEOPSTRUCTW op;
            memset(&op, 0, sizeof op);
            op.wFunc = FO_DELETE;
            op.pFrom = full;
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted &&
                GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return 1;
        }
    }
    ui_log((std::string("recycle: the Recycle Bin did not take ") + path + "; deleting it outright").c_str());
    return DeleteFileW(ui_wpath(path).c_str()) ? 1 : 0;
}

/* Delete a prefab (to the Recycle Bin): 1 = gone, 0 = file operation failed (*reason), -1 = path refused. */
static int ui_delete_one_prefab(const std::string &folder, const std::string &name, const wchar_t **reason)
{
    *reason = L"";
    if (name.empty()) return -1;
    std::string path;
    if (!ui_prefab_file_path(folder, name, path)) return -1;
    int result = ui_recycle_file(path.c_str());
    if (result != 1) *reason = ui_fs_reason();
    if (result == 1) {   /* the sidecar goes with its prefab; absent is fine */
        std::string mp;
        if (ui_prefab_meta_path(folder, name, mp) && ui_exists(mp.c_str()))
            ui_recycle_file(mp.c_str());
    }
    return result;
}
static void ui_apply_delete_prefab()
{
    g_delete_result = ui_delete_one_prefab(g_delete_prefab_folder, g_delete_prefab_name, &g_delete_reason);
}
static void ui_apply_delete_prefabs()
{
    g_delete_prefabs_deleted = 0; g_delete_prefabs_failed = 0; g_delete_prefabs_reason = L"";
    for (size_t i = 0; i < g_delete_prefabs_names.size(); i++) {
        const std::string &folder = i < g_delete_prefabs_folders.size() ? g_delete_prefabs_folders[i] : std::string();
        const wchar_t *why = L"";
        if (ui_delete_one_prefab(folder, g_delete_prefabs_names[i], &why) == 1) g_delete_prefabs_deleted++;
        else { if (!g_delete_prefabs_failed) g_delete_prefabs_reason = why; g_delete_prefabs_failed++; }
    }
}
/* Rename within the folder without overwriting an existing destination.
 * Results: 1 = moved, 0 = file operation failed, -1 = path refused. */
static void ui_apply_rename_prefab()
{
    g_rename_result = -1;
    if (g_rename_prefab_old.empty() || g_rename_prefab_new.empty()) return;
    std::string oldp, newp;
    if (!ui_prefab_file_path(g_rename_prefab_folder, g_rename_prefab_old, oldp)) return;
    if (!ui_prefab_file_path(g_rename_prefab_folder, g_rename_prefab_new, newp)) return;
    g_rename_result = ui_move(oldp.c_str(), newp.c_str()) ? 1 : 0;
    g_rename_reason = g_rename_result == 1 ? L"" : ui_fs_reason();
    if (g_rename_result == 1) {   /* the sidecar follows the rename; absent is fine */
        std::string oldm, newm;
        if (ui_prefab_meta_path(g_rename_prefab_folder, g_rename_prefab_old, oldm) &&
            ui_prefab_meta_path(g_rename_prefab_folder, g_rename_prefab_new, newm))
            ui_move(oldm.c_str(), newm.c_str());
    }
}
/* Load/Place reads prefab JSON and queues kind=2 for staging and native paste.
 * Placement depends on backend editor gates; queue acceptance alone is not placement. */
/* __try can't share a function with a C++ object needing unwinding (/EHsc, C2712) -- these leaves have
 * only PODs in scope, so the SEH guards around the engine calls are safe here. */
static int ui_clear_selection_main(void *)
{
    if (g_iface && g_iface->vtbl && g_iface->vtbl->clear_selection) {
        __try { g_iface->vtbl->clear_selection(g_iface); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return 1;
}
static void ui_clear_selection_seh()
{
    ui_on_main(ui_clear_selection_main, nullptr, UI_MAIN_WAIT_MS);
}
static int ui_apply_edit_seh(const sh_apply_item *it, int count, const char *op)
{
    __try { return g_iface->vtbl->apply_edit(g_iface, it, count, op) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Preserve an in-progress result so the UI cannot mistake it for a failed save. */
static int ui_apply_sync_seh(const sh_apply_item *it, int count, const char *op)
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
static void ui_apply_new_entity()
{
    g_new_entity_result = -1;
    if (g_new_entity_json.empty()) return;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->apply_edit) {
        ui_log("new-entity: ABORT (iface/slot missing)");
        return;
    }
    /* Native paste requires no hovered entity and an empty selection. Refuse here
     * so the page can explain the gate instead of reporting a queued but unplaced
     * entity. Pasting over a selection can miswire the old-to-new ID mapping. */
    if (g_iface->vtbl->hovered_id && g_iface->vtbl->hovered_id(g_iface) >= 0) {
        ui_log("new-entity: REFUSED (hovering an entity -- the engine will not paste there)");
        g_new_entity_result = -2;
        return;
    }
    std::vector<int> ne_selids;
    if (ui_get_selection(ne_selids) > 0) {
        ui_log("new-entity: REFUSED (editor selection is not empty)");
        g_new_entity_result = -3;
        return;
    }
    sh_apply_item it; it.kind = 2; it.id = 0; it.text = g_new_entity_json.c_str();
    g_new_entity_result = ui_apply_edit_seh(&it, 1, "new-entity");
    ui_logp("new-entity: '%s' staged=%d (kind=2: stage + auto pick-up), %zu bytes",
                g_new_entity_label.c_str(), g_new_entity_result, g_new_entity_json.size());
}

/* Sound names are validated by the backend catalog before live playback. */
/* Establish the preview session before draining its first play request. */
static void ui_apply_sound_session()
{
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->sound_session) return;
    __try { g_iface->vtbl->sound_session(g_iface, g_sound_session_on); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static int ui_sound_preview_seh(const char *n)
{
    __try { return g_iface->vtbl->sound_preview(g_iface, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void ui_apply_sound_preview()
{
    g_sound_preview_result = 0;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->sound_preview) {
        ui_log("sound-preview: ABORT (iface/slot missing -- backend too old?)");
        return;
    }
    const char *n = g_sound_preview_name.empty() ? nullptr : g_sound_preview_name.c_str();
    g_sound_preview_result = ui_sound_preview_seh(n);
    g_sound_preview_reason.clear();
    if (!g_sound_preview_result && n && SH_IFACE_HAS_SLOT(g_iface, sound_refusal))
        g_sound_preview_reason = ui_take(g_iface->vtbl->sound_refusal(g_iface));
}

/* The module-prefab slot, SEH-guarded (PODs only in scope). 0 when this backend has no slot. */
static int ui_module_prefab_seh(sh_module_prefab_call *call)
{
    if (!SH_IFACE_HAS_SLOT(g_iface, module_prefab)) return 0;
    __try { return g_iface->vtbl->module_prefab(g_iface, call); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* The call, with its answer (owned by the slot) taken whole into out. */
static int ui_module_prefab_call(sh_module_prefab_call *call, std::string *out)
{
    call->out = nullptr;
    int r = ui_module_prefab_seh(call);
    std::string text = ui_take(call->out);
    call->out = nullptr;
    if (out) *out = text;
    return r;
}
/* Stage a module prefab placement. The result is posted by the think loop once the
 * backend reports it (placed and grabbed, refused, or over budget). */
static void ui_apply_place_module_prefab(const std::string &body)
{
    std::string msg;
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_PLACE;
    call.text = body.c_str();
    call.text_len = body.size();
    int r = ui_module_prefab_call(&call, &msg);
    ui_log(("load-prefab: module prefab '" + g_load_prefab_name + "' place=" + std::to_string(r) + " seq=" +
            std::to_string(call.seq) + " (" + msg + ")").c_str());
    if (r == -6) {                  /* needs a module the editor cannot build; nothing was placed */
        g_load_result = -6;
        g_load_message = msg;
        return;
    }
    if (r == 2 && call.seq > 0) {
        g_module_place_seq = call.seq;
        g_module_place_deadline = GetTickCount64() + 10000;
        g_module_place_name = g_load_prefab_name;
        g_module_place_folder = g_load_prefab_folder;
        g_load_result = 2;          /* pending: posted when the backend finishes */
        return;
    }
    g_load_result = (r == -1 || r == -4) ? r : 0;
    g_load_message = msg;           /* the page shows it when it is the backend's sentence */
}
/* The mods a prefab records come first (backend/prefab_mods.c): the engine never sees a
 * prefab whose mods are missing. Returns true when the placement may go on now; otherwise
 * g_load_result is 3 (held while the download is asked for, followed by ui_poll_prefab_mods),
 * -6 (a module the player cannot get) or -7 (refused), with the message in g_load_message.
 * The rows are the file's block's, else (a prefab saved before the block) its sidecar's; a
 * prefab naming none goes on as before, and the backend answers that without the game. */
static bool ui_resolve_prefab_mods(const std::string &body)
{
    std::string meta, msg;
    if (!SH_IFACE_HAS_SLOT(g_iface, module_prefab)) return true;   /* a backend that records no mods */
    if (!ui_prefab_meta_path(g_load_prefab_folder, g_load_prefab_name, meta)) meta.clear();
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_MODS_RESOLVE;
    call.text = body.c_str();
    call.text_len = body.size();
    call.meta_path = meta.c_str();
    call.name = g_load_prefab_name.c_str();
    int r = ui_module_prefab_call(&call, &msg);
    ui_log(("load-prefab: mods of '" + g_load_prefab_name + "' resolve=" + std::to_string(r) + " seq=" +
            std::to_string(call.seq) + " (" + msg + ")").c_str());
    if (r == 1) return true;                       /* every mod installed and in the open map */
    if (r == 2 && call.seq > 0) {                  /* the download is asked for; Yes downloads, then it is placed */
        g_mods_wait_seq = call.seq;
        g_mods_wait_name = g_load_prefab_name;
        g_mods_wait_folder = g_load_prefab_folder;
        g_load_result = 3;
        g_load_message = msg;
        return false;
    }
    g_load_result = r == -6 ? -6 : -7;
    g_load_message = !msg.empty() ? msg : "This prefab's mods could not be checked, so it was not placed. Try again.";
    return false;
}
/* Follow a held placement: once its mods are installed, place it again; a refusal (the game
 * showed why) is told to the page. */
static void ui_poll_prefab_mods()
{
    if (!g_mods_wait_seq) return;
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_MODS_POLL;
    call.seq = g_mods_wait_seq;
    int r = ui_module_prefab_call(&call);
    if (r == 2) return;
    ui_logp("load-prefab: mods of '%s' (placement %d) finished %d", g_mods_wait_name.c_str(),
                g_mods_wait_seq, r);
    g_mods_wait_seq = 0;
    if (r == 1) {
        /* Placed through the ordinary Load/Place, which now finds every mod installed. */
        g_load_prefab_name = g_mods_wait_name;
        g_load_prefab_folder = g_mods_wait_folder;
        g_pending_load_prefab = true;
        return;
    }
    std::wstring m = L"{\"kind\":\"loadPrefabResult\",\"result\":-7,\"name\":\"";
    m += sh_webview_json::escape_wide(g_mods_wait_name.c_str());
    m += L"\",\"message\":\"\"}";
    ui_post_json(m.c_str());
}
static void ui_apply_load_prefab()
{
    g_load_result = -1;
    g_load_message.clear();
    g_place_wait = false;   /* a newer Load / Place answers for itself */
    if (g_load_prefab_name.empty()) return;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->apply_edit) { ui_log("load-prefab: ABORT (iface/slot missing)"); return; }
    std::string path;
    if (!ui_prefab_file_path(g_load_prefab_folder, g_load_prefab_name, path)) {
        ui_log("load-prefab: ABORT (resolve_prefab_path failed)");
        return;
    }
    FILE *fp = ui_open(path.c_str(), L"rb");
    if (!fp) { ui_log("load-prefab: ABORT (the file could not be opened)"); return; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::string body;
    if (sz > 0) { body.resize((size_t)sz); size_t got = fread(&body[0], 1, (size_t)sz, fp); body.resize(got); }
    fclose(fp);
    if (body.empty()) { ui_log("load-prefab: ABORT (empty file)"); return; }

    /* A module prefab (an idSnapMap) places only in Blueprint, through the backend's
     * merge and install; an object prefab goes to the native paste below. */
    g_load_prefab_modules = false;
    if (sh_webview_json::top_level_type(body) == "idSnapMap") g_load_prefab_modules = true;
    if (g_mods_wait_seq) {
        g_load_result = -7;
        g_load_message = "Another prefab is waiting for its mods. Answer that first, then place this one.";
        return;
    }
    if (!ui_resolve_prefab_mods(body)) return;
    /* The engine and the module merge see the engine's own members only: the product's
     * block is cut out here, so placement never depends on the parser skipping it. */
    body.resize(sh_prefab_block_strip(&body[0], body.size()));
    if (g_load_prefab_modules) {
        ui_apply_place_module_prefab(body);
        return;
    }

    /* Paste uses selection as its old-to-new ID map. Clear it first; the backend
     * rechecks before placement because a pending manipulation may refuse clearing. */
    ui_clear_selection_seh();

    /* kind=2 stages and requests the engine's native paste action on its main thread.
     * Missing editor prerequisites can leave the prefab staged for manual paste. */
    /* Staged prefab storage must survive map-heap resets; the backend allocates it
     * in heap 0. Keep placement on the native action path rather than directly
     * invoking PasteInstantiate or synthesizing operating-system keystrokes. */
    sh_apply_item it; it.kind = 2; it.id = 0; it.text = body.c_str();
    sh_prefab_watch watch;
    bool watched = ui_prefab_watch(&watch) >= 0;
    g_load_result = ui_apply_edit_seh(&it, 1, "load-prefab");
    ui_logp("load-prefab: name='%s' staged=%d (kind=2: stage + auto pick-up)",
                             g_load_prefab_name.c_str(), g_load_result);
    /* The game thread decides on its next frame whether the paste was queued (the prefab arrives
     * in hand) or only staged; the page is told once it has (ui_poll_place_outcome). */
    if (g_load_result == 1 && watched) {
        g_place_wait = true;
        g_place_wait_seq = watch.place_seq;
        g_place_wait_deadline = GetTickCount64() + 2500;
        g_place_wait_name = g_load_prefab_name;
        g_place_wait_folder = g_load_prefab_folder;
    }
}
/* Post an object Load / Place's outcome once the game thread has decided it: "queued" (in hand,
 * Space places it), "staged" (Ctrl+V places it; reason says why it was not queued) or "unknown"
 * when no decision came in time. A queued paste becomes the held prefab the Limits rule names. */
static void ui_poll_place_outcome()
{
    if (!g_place_wait) return;
    sh_prefab_watch watch;
    std::string reason;
    ui_prefab_watch(&watch, &reason);
    bool decided = watch.place_seq != g_place_wait_seq;
    if (!decided && GetTickCount64() < g_place_wait_deadline) return;
    g_place_wait = false;
    const char *placed = !decided ? "unknown"
                       : watch.place_result == SH_PREFAB_PLACE_QUEUED ? "queued"
                       : watch.place_result == SH_PREFAB_PLACE_STAGED ? "staged" : "failed";
    if (decided && watch.place_result == SH_PREFAB_PLACE_QUEUED) {
        g_held_obj_name = g_place_wait_name;
        g_held_obj_folder = g_place_wait_folder;
        g_held_obj_seq = watch.place_seq;
    }
    std::wstring m = L"{\"kind\":\"loadPrefabResult\",\"result\":";
    m += decided && watch.place_result == SH_PREFAB_PLACE_FAILED ? L"-1" : L"1";
    m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_place_wait_name.c_str());
    m += L"\",\"placed\":\""; m += sh_webview_json::escape_wide(placed);
    m += L"\",\"reason\":\""; m += sh_webview_json::escape_wide(decided ? reason.c_str() : "");
    m += L"\"}";
    ui_post_json(m.c_str());
    ui_log(("load-prefab: '" + g_place_wait_name + "' " + placed + (decided && !reason.empty() ? ": " + reason : "")).c_str());
}
/* Commit the page's refreshed, patched entity JSON with kind=0. Prefer the
 * synchronous applied result; see ui_apply_sync_seh for the older-backend fallback. */
static void ui_apply_save_timeline()
{
    g_save_timeline_result = 0;
    if (g_save_timeline_eid < 0 || g_save_timeline_json.empty()) return;
    if (!g_iface || !g_iface->vtbl || (!g_iface->vtbl->apply_sync && !g_iface->vtbl->apply_edit)) {
        ui_log("save-timeline: ABORT (iface/slot missing)"); return;
    }
    sh_apply_item it; it.kind = 0; it.id = g_save_timeline_eid; it.text = g_save_timeline_json.c_str();
    g_save_timeline_result = ui_apply_sync_seh(&it, 1, "save-timeline");
    ui_logp("save-timeline: eid=%d ok=%d", g_save_timeline_eid, g_save_timeline_result);
}
/* Move a prefab from one folder to another (drag-and-drop target): a move across the two resolved
 * paths. Refuses to overwrite an existing same-named file at the destination (JS pre-checks too). */
static int ui_move_one_prefab(const std::string &from, const std::string &name, const std::string &to,
                               const wchar_t **reason)
{
    *reason = L"";
    if (name.empty()) return -1;
    std::string oldp, newp;
    if (!ui_prefab_file_path(from, name, oldp)) return -1;
    if (!ui_prefab_file_path(to, name, newp)) return -1;
    int result = ui_move(oldp.c_str(), newp.c_str()) ? 1 : 0;
    if (result != 1) *reason = ui_fs_reason();
    if (result == 1) {   /* the sidecar follows the move; absent is fine */
        std::string oldm, newm;
        if (ui_prefab_meta_path(from, name, oldm) && ui_prefab_meta_path(to, name, newm))
            ui_move(oldm.c_str(), newm.c_str());
    }
    return result;
}
static void ui_apply_move_prefab()
{
    g_move_prefab_result = ui_move_one_prefab(g_move_prefab_from, g_move_prefab_name, g_move_prefab_to, &g_move_prefab_reason);
}
static void ui_apply_move_prefabs()
{
    g_move_prefabs_moved.clear(); g_move_prefabs_failed = 0; g_move_prefabs_reason = L"";
    for (size_t i = 0; i < g_move_prefabs_names.size(); i++) {
        const std::string &from = i < g_move_prefabs_from.size() ? g_move_prefabs_from[i] : std::string();
        const wchar_t *why = L"";
        if (ui_move_one_prefab(from, g_move_prefabs_names[i], g_move_prefabs_to, &why) == 1) g_move_prefabs_moved.push_back(g_move_prefabs_names[i]);
        else { if (!g_move_prefabs_failed) g_move_prefabs_reason = why; g_move_prefabs_failed++; }
    }
}
/* Create a real subdirectory under prefabs\. result: 1 created, 0 empty name / already exists, -1 failed. */
static void ui_apply_create_folder()
{
    g_create_folder_result = -1;
    g_create_folder_reason = L"";
    if (g_create_folder_name.empty()) return;
    std::string dir;
    if (!ui_prefab_dir(g_create_folder_name, dir)) return;
    ui_strip_trailing_sep(dir);
    if (CreateDirectoryW(ui_wpath(dir.c_str()).c_str(), nullptr)) { g_create_folder_result = 1; return; }
    g_create_folder_reason = ui_fs_reason();
    g_create_folder_result = 0;
}
/* Rename a folder (a move renames a directory too). result: 1 ok, 0 dest exists/locked, -1 resolve failed. */
static void ui_apply_rename_folder()
{
    g_rename_folder_result = -1;
    if (g_rename_folder_old.empty() || g_rename_folder_new.empty()) return;
    std::string oldd, newd;
    if (!ui_prefab_dir(g_rename_folder_old, oldd)) return;
    if (!ui_prefab_dir(g_rename_folder_new, newd)) return;
    ui_strip_trailing_sep(oldd); ui_strip_trailing_sep(newd);
    g_rename_folder_result = ui_move(oldd.c_str(), newd.c_str()) ? 1 : 0;
    g_rename_folder_reason = g_rename_folder_result == 1 ? L"" : ui_fs_reason();
}
/* Move contents to the prefab root before removing a folder. A name collision
 * leaves the source file in place and removal fails without overwriting data. */
static void ui_apply_delete_folder()
{
    g_delete_folder_result = -1;
    if (g_delete_folder_name.empty()) return;
    std::string dir, rootDir;
    if (!ui_prefab_dir(g_delete_folder_name, dir)) return;
    if (!ui_prefab_dir("", rootDir)) return;
    std::vector<std::string> items;
    ui_list_json_dir(dir, items);
    for (size_t i = 0; i < items.size(); i++) {
        std::string src = dir + items[i] + ".json";
        std::string dst = rootDir + items[i] + ".json";
        if (!ui_move(src.c_str(), dst.c_str())) continue;
        std::string oldm, newm;   /* the sidecar follows its prefab; absent is fine */
        if (ui_prefab_meta_path(g_delete_folder_name, items[i], oldm) && ui_prefab_meta_path("", items[i], newm))
            ui_move(oldm.c_str(), newm.c_str());
    }
    ui_strip_trailing_sep(dir);
    g_delete_folder_result = RemoveDirectoryW(ui_wpath(dir.c_str()).c_str()) ? 1 : 0;
}
/* Run enum_inherits (+0x278) or enum_valid_classes (+0x270): the packed list, owned by the slot.
 * SEH-guarded, no C++ objects. */
static char *ui_run_enum(int classes, const char *inherit)
{
    char *out = nullptr;
    int count = 0;
    __try {
        if (g_iface && g_iface->vtbl) {
            if (classes) { if (g_iface->vtbl->enum_valid_classes) g_iface->vtbl->enum_valid_classes(g_iface, inherit ? inherit : "", &out, &count); }
            else         { if (g_iface->vtbl->enum_inherits)      g_iface->vtbl->enum_inherits(g_iface, &out, &count); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; }
    return out;
}

/* Messages to the page. */
static uint64_t ui_list_sig(int n)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) { h = hint(h, g_ents[i].eid); h = hstr(h, g_ents[i].id.c_str()); h = hstr(h, g_ents[i].name.c_str()); h = hint(h, g_ents[i].hidden); }
    return h ^ (uint64_t)n;
}
static void ui_emit_list(int n, int ready, const char *reason)
{
    if (!g_webview) return;
    unsigned long long started = ui_perf_now_us();
    unsigned long seq = ++g_list_seq;
    std::wstring json; json.reserve((size_t)(n + g_tl_count) * 96 + 96);
    json += L"{\"kind\":\"list\",\"seq\":"; json += std::to_wstring(seq);
    json += L",\"version\":\""; json += sh_webview_json::escape_wide(g_version.c_str());
    /* the renderer rides the list message alongside the version, for the same reason: the feedback
     * dialog composes its own payload in the page and needs both there. */
    json += L"\",\"renderer\":\""; json += sh_webview_json::escape_wide(sh_host_renderer_name());
    /* and the game identity: '' means the backend is not attached ("- Game: not attached") */
    json += L"\",\"game\":\""; json += sh_webview_json::escape_wide(g_game.c_str());
    /* the features that are off this session, the one line the status bar shows ('' = none) */
    {
        std::string off;
        if (SH_IFACE_HAS_SLOT(g_iface, features_off)) off = ui_take(g_iface->vtbl->features_off(g_iface));
        json += L"\",\"featuresOff\":\""; json += sh_webview_json::escape_wide(off.c_str());
    }
    json += L"\",\"editorReady\":"; json += ready ? L"true" : L"false";
    json += L",\"count\":"; json += std::to_wstring(n);
    json += L",\"entities\":[";
    for (int i = 0; i < n; i++) {
        if (i) json += L",";
        json += L"{\"eid\":"; json += std::to_wstring(g_ents[i].eid);
        json += L",\"id\":\""; json += sh_webview_json::escape_wide(g_ents[i].id.c_str());
        json += L"\",\"name\":\""; json += sh_webview_json::escape_wide(g_ents[i].name.c_str());
        json += L"\",\"hidden\":"; json += g_ents[i].hidden ? L"true" : L"false";
        json += L"}";
    }
    json += L"],\"timelines\":[";
    for (int i = 0; i < g_tl_count; i++) {
        if (i) json += L",";
        json += L"{\"eid\":"; json += std::to_wstring(g_tls[i].eid);
        json += L",\"id\":\""; json += sh_webview_json::escape_wide(g_tls[i].id.c_str());
        json += L"\",\"name\":\""; json += sh_webview_json::escape_wide(g_tls[i].name.c_str());
        json += L"\"}";   /* close the "name" string before the object brace */
    }
    json += L"]}";
    unsigned long long built = ui_perf_now_us();
    HRESULT hr = g_webview->PostWebMessageAsJson(json.c_str());
    unsigned long long posted = ui_perf_now_us();
    ui_logp("perf list-emit: seq=%lu reason=%s entities=%d timelines=%d json_chars=%llu build_us=%llu post_us=%llu hr=0x%08lx",
                seq, reason ? reason : "unknown", n, g_tl_count,
                (unsigned long long)json.size(), built >= started ? built - started : 0,
                posted >= built ? posted - built : 0, (unsigned long)hr);
}
static void ui_send_list(const char *reason)
{
    int ready = 0; int n = ui_collect_timed(&ready, reason);
    uint64_t sig = ui_list_sig(n);
    /* Rebuild Timelines only when the entity signature changes. */
    if (sig != g_last_list_sig) ui_rescan_timelines_timed(n, reason);
    g_last_list_sig = sig;
    ui_emit_list(n, ready, reason);
}
static void ui_send_state(int id, bool autoflag)
{
    if (!g_webview) return;
    std::string declText, clsText, inhText, dnmText;
    bool ok = ui_collect_state(id, declText, clsText, inhText, dnmText);
    const char *decl = declText.c_str(), *cls = clsText.c_str(), *inh = inhText.c_str(), *dnm = dnmText.c_str();
    uint64_t sig = hstr(hstr(hstr(hstr(1469598103934665603ull, decl), cls), inh), dnm) ^ (uint64_t)id;
    if (autoflag && sig == g_last_state_sig) return;   /* nothing changed -> skip the auto push */
    g_last_state_sig = sig;
    std::wstring json = L"{\"kind\":\"state\",\"auto\":"; json += autoflag ? L"true" : L"false";
    json += L",\"eid\":"; json += std::to_wstring(id);
    json += L",\"ok\":"; json += ok ? L"true" : L"false";
    json += L",\"decl\":\"";        json += sh_webview_json::escape_wide(decl); json += L"\"";
    json += L",\"classname\":\"";   json += sh_webview_json::escape_wide(cls);  json += L"\"";
    json += L",\"inherit\":\"";     json += sh_webview_json::escape_wide(inh);  json += L"\"";
    json += L",\"displayname\":\""; json += sh_webview_json::escape_wide(dnm);  json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Build + send the valid-values list for a datalist: {kind:"inherits"|"classes", inherit?, items:[...]}. */
static void ui_send_enum(int classes, const char *inherit)
{
    if (!g_webview) return;
    std::vector<std::string> names = ui_take_packed(ui_run_enum(classes, inherit));
    std::wstring json = classes ? L"{\"kind\":\"classes\",\"inherit\":\"" : L"{\"kind\":\"inherits\"";
    if (classes) { json += sh_webview_json::escape_wide(inherit ? inherit : ""); json += L"\""; }
    json += L",\"items\":[";
    for (size_t i = 0; i < names.size(); i++) {
        if (i) json += L",";
        json += L"\""; json += sh_webview_json::escape_wide(names[i].c_str()); json += L"\"";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Keep SEH in a POD-only helper: MSVC forbids __try alongside C++ locals
 * that require unwinding. */
static char *ui_run_arg_resclass(const char *resClass)
{
    char *out = nullptr;
    int count = 0;
    __try {
        if (g_iface && g_iface->vtbl && g_iface->vtbl->enum_decls_of_resclass && resClass && resClass[0])
            g_iface->vtbl->enum_decls_of_resclass(g_iface, resClass, &out, &count);
    } __except (EXCEPTION_EXECUTE_HANDLER) { out = nullptr; }
    return out;
}
/* Query declaration classes by short name and enum types by catalog name.
 * Empty results leave the page with an editable text field. */
static void ui_send_arg_resclass(const char *resClass)
{
    if (!g_webview) return;
    std::vector<std::string> names = ui_take_packed(ui_run_arg_resclass(resClass));
    std::wstring json = L"{\"kind\":\"argResclass\",\"resClass\":\""; json += sh_webview_json::escape_wide(resClass ? resClass : ""); json += L"\",\"items\":[";
    for (size_t i = 0; i < names.size(); i++) {
        if (i) json += L",";
        json += L"\""; json += sh_webview_json::escape_wide(names[i].c_str()); json += L"\"";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
/* Look up entity descriptions in the generated table. */
static const ShEntDesc *ui_lookup_desc(const std::string &name)
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
/* {"summary":...}, or null when the entry has no description a player should see. */
static void ui_json_desc_obj(std::wstring &json, const ShEntDesc *d)
{
    std::string text = d ? sh_player_text::summary(d->summary) : std::string();
    if (text.empty()) { json += L"null"; return; }
    json += L"{\"summary\":\""; json += sh_webview_json::escape_wide(text.c_str()); json += L"\"}";
}
static void ui_send_desc(const char *inherit, const char *classname)
{
    if (!g_webview) return;
    std::wstring json = L"{\"kind\":\"desc\",\"inherit\":";
    ui_json_desc_obj(json, ui_lookup_desc(inherit ? inherit : ""));
    json += L",\"class\":";
    ui_json_desc_obj(json, ui_lookup_desc(classname ? classname : ""));
    json += L"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* The File menu's rawmap I/O: "Load Rawmap" / "Save Rawmap As", the only caller of
 * the +0x328/+0x330 slots. Load opens the picked file in the editor at once, after
 * the page's confirm; nothing is kept for a later map. Paths cross to the backend
 * as UTF-8.
 *
 * The picker runs on its own thread. This handler is dispatched from DispatchMessageW
 * inside ui_think_loop, so a modal Show() here would stop editor polling, selection
 * sync and the queued-work drain for as long as the dialog is open. The result comes
 * back through the same pending-flag handoff every other deferred action uses, so
 * every backend call still happens on the UI thread. */

/* Run the common item dialog. `save` picks the Save-As variant. Returns false when the person
 * cancelled (the overwhelmingly common non-success case, and not an error worth reporting).
 * Call from the picker thread, never from the message handler. */
static bool ui_pick_rawmap_file(bool save, int map_mods, const wchar_t *title, std::wstring &out_path,
                                 bool prefab = false, const wchar_t *file_name = nullptr)
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
        /* .json and .zip both: a .zip is the map with its mods (rawmap_picker.h). */
        const sh_rawmap_picker::choice pick = prefab ? sh_rawmap_picker::for_prefab_dialog(save)
                                                     : sh_rawmap_picker::for_dialog(save, map_mods);
        COMDLG_FILTERSPEC filters[4];
        for (unsigned i = 0; i < pick.count; ++i) {
            filters[i].pszName = pick.types[i].label;
            filters[i].pszSpec = pick.types[i].pattern;
        }
        dlg->SetFileTypes(pick.count, filters);
        dlg->SetFileTypeIndex(1);
        dlg->SetDefaultExtension(pick.extension);
        if (title) dlg->SetTitle(title);
        if (save && (file_name || pick.file_name)) dlg->SetFileName(file_name ? file_name : pick.file_name);

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
         * SetFolder, not only SetDefaultFolder: SetDefaultFolder applies only when the dialog has no
         * remembered location, and otherwise it opens in Quick Access, the slowest starting point
         * (it enumerates cloud providers and network places).
         *
         * SetDefaultFolder is kept too, for the first-ever open and for a path that does not exist
         * yet; SetFolder wins when both are set. Overriding a remembered location is the intent:
         * the folder is the one this dialog kind last used, else its default (rawmap_picker.h),
         * chosen by ui_pick_start_folder on the UI thread. */
        if (!g_pick_folder.empty()) {
            IShellItem *folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(g_pick_folder.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
                dlg->SetDefaultFolder(folder);
                dlg->SetFolder(folder);
                folder->Release();
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

/* Report the rawmap paths. The backend hands back a UTF-8 JSON fragment (paths are escaped there --
 * a Windows path is full of backslashes), so this forwards it whole rather than re-encoding field
 * by field.
 *
 * `confirm_file`, when set, asks the page to put the "open it now? unsaved edits are discarded"
 * question up before anything opens. The backend has checked the file and the editor; yes opens it,
 * and no opens nothing. It must arrive already JSON-escaped (escape_wide), like every other path
 * that crosses this boundary. */

/* Tell the page where the navigation view stands. Sent when the View menu opens
 * and again after an update is asked for. */
static void ui_send_navmesh_state()
{
    if (!g_webview) return;
    std::string state;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->navmesh_view)
        state = ui_take(g_iface->vtbl->navmesh_view(g_iface, 0));
    bool have = !state.empty();

    std::wstring json = L"{\"kind\":\"navmeshState\",\"ok\":";
    json += have ? L"true" : L"false";
    if (have) {
        json += L",\"state\":";
        json += ui_widen(state.c_str());   /* already a JSON object -- forward it */
    }
    json += L"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* `error` marks the note as a refusal: the page shows it as an error toast. */
static void ui_send_rawmap_status(const wchar_t *note, const wchar_t *confirm_file = nullptr, bool error = false)
{
    if (!g_webview) return;

    std::string status;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->rawmap_status)
        status = ui_take(g_iface->vtbl->rawmap_status(g_iface));
    bool have = !status.empty();

    std::wstring json = L"{\"kind\":\"rawmapStatus\",\"ok\":";
    json += have ? L"true" : L"false";
    if (have) {
        json += L",\"status\":";
        json += ui_widen(status.c_str());   /* already a JSON object -- forward it, do not escape it */
    }
    json += L",\"note\":\"";
    json += note ? note : L"";
    json += L"\"";
    if (error) json += L",\"error\":true";
    if (confirm_file && *confirm_file) {
        json += L",\"confirmLoad\":\"";
        json += confirm_file;
        json += L"\"";
    }
    json += L"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* Ask the backend to point a side at a chosen file. `load_path`/`save_path` (UTF-8) NULL = leave
 * alone, "" = restore that side's default. `arm` is the verb code the slot documents. */
static void ui_rawmap_configure(const char *load_path, const char *save_path, int arm,
                                 const wchar_t *ok_note)
{
    char *said = nullptr;
    int ok = g_iface->vtbl->rawmap_configure(g_iface, load_path, save_path, arm, &said);
    std::string msg = ui_take(said);
    /* The backend's message is the useful one on refusal -- it names WHY the file was rejected
     * (wrong type, too big, unreadable). On success its "ok" is not worth showing, so the caller's
     * own sentence wins -- unless the caller passes no sentence, which is how Save Rawmap As asks for
     * the backend's own: only the backend knows whether it wrote the file there and then or is
     * waiting for an editor save, and how many bytes went out. */
    std::wstring note = ok ? (ok_note ? std::wstring(ok_note) : sh_webview_json::escape_wide(msg.c_str()))
                           : sh_webview_json::escape_wide(msg.c_str());

    /* ui_logf carries exactly one unsigned long, so compose the detail line first. The paths are
     * what makes this log worth having when someone reports "it did not load my file". */
    ui_log(("rawmap configure: ok=" + std::to_string(ok) + " arm=" + std::to_string(arm) + " load=" +
            (load_path ? load_path : "(unchanged)") + " save=" + (save_path ? save_path : "(unchanged)") +
            " msg=" + msg).c_str());
    ui_send_rawmap_status(note.c_str(), nullptr, !ok);
}

/* The picker thread. Its ONLY job is the modal dialog: it touches no backend slot and posts nothing
 * to the page, because both of those belong on the UI thread. It hands back a path and lets the think
 * loop do the rest. Its own apartment, since a fresh thread has none. */
static DWORD WINAPI ui_pick_thread(LPVOID param)
{
    const int kind = (int)(intptr_t)param;
    std::wstring picked;
    bool ok;

    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (kind >= 2)
        ok = ui_pick_rawmap_file(kind == 2, 0, kind == 2 ? L"Export Prefab" : L"Import Prefab", picked, true,
                                  kind == 2 && !g_pick_file_name.empty() ? g_pick_file_name.c_str() : nullptr);
    else
        ok = ui_pick_rawmap_file(kind == 1, (int)InterlockedCompareExchange(&g_pick_mods, 0, 0),
                                  kind == 1 ? L"Save Rawmap As" : L"Load Rawmap", picked);
    if (SUCCEEDED(init)) CoUninitialize();

    g_pick_kind = kind;
    g_pick_ok   = ok;
    g_pick_path = picked;
    InterlockedExchange(&g_pick_done, 1);   /* last: publishes everything written above */
    return 0;
}

static int ui_config_get_json(const std::string &key, std::string &value, unsigned int *out_flags);   /* fwd */
static bool ui_is_dir(const std::wstring &path)
{
    DWORD a = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
/* The folder a dialog of `kind` opens in: the one kept for it in config.json when it still exists,
 * else Downloads for the prefab dialogs and the rawmaps folder for the rawmap ones. UI thread. */
static std::wstring ui_pick_start_folder(int kind)
{
    const char *key = sh_rawmap_picker::folder_config_key(kind);
    std::string json;
    std::wstring kept;
    if (key && ui_config_get_json(key, json, nullptr) > 0 &&
        sh_rawmap_picker::folder_from_config(json.c_str(), kept) && ui_is_dir(kept))
        return kept;
    if (sh_rawmap_picker::default_folder(kind) == sh_rawmap_picker::FOLDER_DOWNLOADS) {
        PWSTR p = nullptr;
        std::wstring dl;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) dl = p;
        if (p) CoTaskMemFree(p);
        if (ui_is_dir(dl)) return dl;
    }
    std::wstring dir = ui_local_snapmap_dir();
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);        /* harmless if it already exists */
    return dir;
}
/* Keep the folder of a file the player picked, for that dialog kind's next open. UI thread. */
static void ui_pick_keep_folder(int kind, const std::wstring &picked)
{
    const char *key = sh_rawmap_picker::folder_config_key(kind);
    std::wstring folder = sh_rawmap_picker::folder_of(picked);
    if (!key || folder.empty() || !g_iface || !g_iface->vtbl || !g_iface->vtbl->config_set_json) return;
    std::string value = sh_rawmap_picker::folder_to_config(folder);
    int r = g_iface->vtbl->config_set_json(g_iface, key, value.c_str());
    if (r == SH_CONFIG_SET_REJECTED) {
        ui_logp("dialogs: could not keep the folder for %s", key);
    }
}
/* Open a picker, unless one is already open. Returns with the dialog still to come -- the caller has
 * nothing to report yet, which is the point: the message handler returns immediately and the think
 * loop keeps turning. */
static void ui_begin_pick(int kind)
{
    HANDLE h;

    if (InterlockedExchange(&g_pick_busy, 1) != 0) {
        if (kind >= 2)
            ui_post_json(kind == 2 ? L"{\"kind\":\"prefabFileResult\",\"op\":\"export\",\"ok\":false,\"message\":\"A file dialog is already open.\"}"
                                    : L"{\"kind\":\"prefabFileResult\",\"op\":\"import\",\"ok\":false,\"message\":\"A file dialog is already open.\"}");
        else ui_send_rawmap_status(L"A file dialog is already open. Finish it first.", nullptr, true);
        return;
    }
    /* Asked here, on the UI thread: the picker thread touches no backend slot. */
    {
        std::string status;
        if (g_iface && g_iface->vtbl && g_iface->vtbl->rawmap_status)
            status = ui_take(g_iface->vtbl->rawmap_status(g_iface));
        InterlockedExchange(&g_pick_mods, sh_rawmap_picker::map_mods_from_status(status.c_str()));
    }
    g_pick_folder = ui_pick_start_folder(kind);   /* read by the dialog's thread, which starts below */
    h = CreateThread(nullptr, 0, ui_pick_thread, (LPVOID)(intptr_t)kind, 0, nullptr);
    if (h == nullptr) {
        InterlockedExchange(&g_pick_busy, 0);
        ui_send_rawmap_status(L"Windows could not open the file dialog. Try again.", nullptr, true);
        return;
    }
    CloseHandle(h);
}

static void ui_send_prefabs();   /* fwd */
/* Export Prefab (kind 2) and Import Prefab (kind 3), once the dialog has a file: the backend writes
 * the ZIP, or reads a ZIP or the .json of an extracted export folder (backend/prefab_mods.c), and
 * the page gets one prefabFileResult. UI thread. */
static void ui_finish_prefab_pick(int kind, bool picked, const std::string &zip8)
{
    const wchar_t *op = kind == 2 ? L"export" : L"import";
    if (!picked) {
        std::wstring m = L"{\"kind\":\"prefabFileResult\",\"op\":\""; m += op; m += L"\",\"cancelled\":true}";
        ui_post_json(m.c_str());
        return;
    }
    std::string out;
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.zip_path = zip8.c_str();
    std::string path, meta;
    int r;
    if (kind == 2) {
        if (!ui_prefab_file_path(g_pick_prefab_folder, g_pick_prefab_name, path) ||
            !ui_prefab_meta_path(g_pick_prefab_folder, g_pick_prefab_name, meta)) {
            ui_post_json(L"{\"kind\":\"prefabFileResult\",\"op\":\"export\",\"ok\":false,\"message\":\"That prefab cannot be found.\"}");
            return;
        }
        call.op = SH_MODULE_PREFAB_EXPORT;
        call.path = path.c_str();
        call.meta_path = meta.c_str();
        call.name = g_pick_prefab_name.c_str();
        r = ui_module_prefab_call(&call, &out);
    } else {
        if (!ui_prefab_dir("", path)) {
            ui_post_json(L"{\"kind\":\"prefabFileResult\",\"op\":\"import\",\"ok\":false,\"message\":\"The prefabs folder cannot be found.\"}");
            return;
        }
        call.op = SH_MODULE_PREFAB_IMPORT;
        call.path = path.c_str();
        r = ui_module_prefab_call(&call, &out);
    }
    ui_log((std::string("prefab ") + (kind == 2 ? "export" : "import") + ": result=" + std::to_string(r) + " zip='" +
            zip8 + "' " + out).c_str());
    std::wstring m = L"{\"kind\":\"prefabFileResult\",\"op\":\""; m += op;
    m += L"\",\"ok\":"; m += r >= 0 && (kind == 3 || r == 1) ? L"true" : L"false";
    m += L",\"result\":"; m += std::to_wstring(r);
    if (kind == 2) {
        m += L",\"message\":\""; m += sh_webview_json::escape_wide(!out.empty() ? out.c_str() : "The prefab could not be exported. Try again."); m += L"\"";
    } else {
        /* the backend's {"ok","added","name","message"}: only its message and name are forwarded */
        std::string text = out, message, name;
        size_t at = text.find("\"message\":\"");
        if (at != std::string::npos) {
            size_t i = at + 11;
            while (i < text.size() && text[i] != '"') { if (text[i] == '\\' && i + 1 < text.size()) i++; message += text[i]; i++; }
        }
        at = text.find("\"name\":\"");
        if (at != std::string::npos) {
            size_t i = at + 8;
            while (i < text.size() && text[i] != '"') { if (text[i] == '\\' && i + 1 < text.size()) i++; name += text[i]; i++; }
        }
        m += L",\"message\":\""; m += sh_webview_json::escape_wide(message.empty() ? "The prefab could not be imported. Try again." : message.c_str());
        m += L"\",\"name\":\""; m += sh_webview_json::escape_wide(name.c_str()); m += L"\"";
    }
    m += L"}";
    ui_post_json(m.c_str());
    if (kind == 3 && r == 1) ui_send_prefabs();
}
/* Act on a finished picker. Runs on the UI thread from the think loop, so every backend call below
 * is where it has always been. */
static void ui_finish_pick()
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
        ui_pick_keep_folder(kind, g_pick_path);   /* this dialog opens there next time */
    }

    g_pick_path.clear();
    InterlockedExchange(&g_pick_done, 0);
    InterlockedExchange(&g_pick_busy, 0);

    if (kind >= 2) {
        ui_finish_prefab_pick(kind, picked, p8);
        return;
    }
    if (!picked) {
        ui_send_rawmap_status(L"");   /* cancelled -- refresh the readout, say nothing */
        return;
    }

    if (kind == 1) {
        /* Naming a destination IS the request to write it -- the configure slot writes the file there
         * and then from the map's own save on disk, and only falls back to arming one editor save when
         * the map has never been saved. No note of our own: only the backend knows which happened. */
        ui_rawmap_configure(nullptr, p8.c_str(), -1, nullptr);
        return;
    }

    /* arm = 2: the picked file is the one to open now. The backend checks the file and that the
     * editor can open it, and refuses at once otherwise; nothing waits for a later map. */
    {
        char *said = nullptr;
        int accepted = 0;
        if (g_iface && g_iface->vtbl && g_iface->vtbl->rawmap_configure)
            accepted = g_iface->vtbl->rawmap_configure(g_iface, p8.c_str(), nullptr, 2, &said);
        std::string msg = ui_take(said);
        if (!accepted) {
            /* The backend logs the refusal and prints it to the console (rawmap.c); this is the UI
             * log's line, as the prefab Import's refusal has one. */
            ui_log(("rawmap load: refused '" + p8 + "': " + msg).c_str());
            ui_send_rawmap_status(sh_webview_json::escape_wide(msg.c_str()).c_str(), nullptr, true);
        } else {
            /* Ask before opening: a reload discards whatever is in the editor, and the engine
             * offers no "has unsaved changes" query. Answering no opens nothing. The page owns
             * this dialog on purpose -- a native modal here stalls the think loop. */
            ui_send_rawmap_status(L"", base.c_str());
        }
    }
}

/* Fetch the latest worker-published PNG data URI. Probe its size before copying;
 * no rendering or engine access occurs here. */
static void ui_send_preview(const char *name)
{
    if (!g_webview) return;
    std::string uri, reason;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->get_preview) uri = ui_take(g_iface->vtbl->get_preview(g_iface));
    if (uri.empty() && SH_IFACE_HAS_SLOT(g_iface, preview_failure))
        reason = ui_take(g_iface->vtbl->preview_failure(g_iface));
    std::wstring json = L"{\"kind\":\"previewImage\",\"ok\":";
    json += uri.empty() ? L"false" : L"true";
    json += L",\"reason\":\""; json += sh_webview_json::escape_wide(reason.c_str()); json += L"\"";
    json += L",\"uri\":\"";
    json += sh_webview_json::escape_wide(uri.c_str());
    json += L"\",\"name\":\"";
    json += sh_webview_json::escape_wide(name ? name : "");
    json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* Queue preview decoding on the backend worker. The page polls for publication. */
static void ui_request_preview(const char *name, int asset_kind)
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

static void ui_cancel_preview()
{
    if (g_iface && g_iface->vtbl && g_iface->vtbl->request_preview)
        g_iface->vtbl->request_preview(g_iface, "");
}

/* One kind's whole catalog in one response for the page's cache. */
static void ui_send_asset_list(int kind)
{
    if (!g_webview) return;
    std::string all;
    int total = 0;
    if (g_iface && g_iface->vtbl && g_iface->vtbl->list_assets) {
        all = ui_take(g_iface->vtbl->list_assets(g_iface, kind));   /* newline-separated */
        for (char c : all) total += c == '\n';
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
static void ui_send_events()
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
static void ui_send_event_docs()
{
    if (!g_webview) return;
    std::wstring json; json.reserve((size_t)SH_EVENT_DOCS_N * 192 + 32);
    json = L"{\"kind\":\"eventDocs\",\"items\":[";
    for (int i = 0; i < SH_EVENT_DOCS_N; i++) {
        if (i) json += L",";
        const ShEvtDoc &d = SH_EVENT_DOCS[i];
        json += L"{\"name\":\""; json += sh_webview_json::escape_wide(d.name ? d.name : ""); json += L"\"";
        json += L",\"summary\":\""; json += sh_webview_json::escape_wide(sh_player_text::summary(d.summary).c_str()); json += L"\"";
        json += L",\"args\":[";
        for (int a = 0; a < d.nArgs; a++) {
            if (a) json += L",";
            const ShEvtArgDoc &ad = d.args[a];
            json += L"{\"name\":\""; json += sh_webview_json::escape_wide(ad.name ? ad.name : ""); json += L"\"";
            json += L",\"desc\":\""; json += sh_webview_json::escape_wide(sh_player_text::summary(ad.desc).c_str()); json += L"\"}";
        }
        json += L"]}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
static void ui_json_asset_items(std::wstring &json, const ShAssetItem *items, int n)
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
static void ui_send_entity_assets()
{
    if (!g_webview) return;
    std::wstring json; json.reserve((size_t)SH_ENTITY_ASSETS_N * 512 + 32);
    json = L"{\"kind\":\"entityAssets\",\"items\":[";
    for (int i = 0; i < SH_ENTITY_ASSETS_N; i++) {
        if (i) json += L",";
        const ShEntityAssets &ea = SH_ENTITY_ASSETS[i];
        json += L"{\"slug\":\""; json += sh_webview_json::escape_wide(ea.slug ? ea.slug : ""); json += L"\"";
        json += L",\"models\":";    ui_json_asset_items(json, ea.models,    ea.nModels);
        json += L",\"animWeb\":";   ui_json_asset_items(json, ea.animWeb,   ea.nAnimWeb);
        json += L",\"md6Anim\":";   ui_json_asset_items(json, ea.md6Anim,   ea.nMd6Anim);
        json += L",\"animAlias\":"; ui_json_asset_items(json, ea.animAlias, ea.nAnimAlias);
        json += L",\"tagName\":";   ui_json_asset_items(json, ea.tagName,   ea.nTagName);
        json += L"}";
    }
    json += L"]}";
    g_webview->PostWebMessageAsJson(json.c_str());
}
static void ui_post_json(const wchar_t *json) { if (g_webview) g_webview->PostWebMessageAsJson(json); }

/* Move at most one decoded prefab mesh per UI tick. WebView2 shared buffers avoid base64's extra copy
 * and 4/3 expansion; JavaScript uploads from the ArrayBuffer and releases it immediately. If an older
 * runtime lacks the shared-buffer interfaces, consume the result and keep the honest procedural proxy. */
static void ui_send_prefab_mesh_completion()
{
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->get_prefab_mesh) return;
    void *blob = nullptr;
    int needed = g_iface->vtbl->get_prefab_mesh(g_iface, &blob);
    if (needed <= 0 || !blob) { ui_free_text(blob); return; }

    ComPtr<ICoreWebView2SharedBuffer> shared;
    BYTE *dst = nullptr;
    if (!g_webview_environment12 || !g_webview17 ||
        FAILED(g_webview_environment12->CreateSharedBuffer((UINT64)needed, shared.GetAddressOf())) || !shared ||
        FAILED(shared->get_Buffer(&dst)) || !dst) {
        ui_free_text(blob);
        ui_post_json(L"{\"kind\":\"prefabMeshTransportUnavailable\"}");
        return;
    }
    memcpy(dst, blob, (size_t)needed);
    ui_free_text(blob);

    std::wstring meta = L"{\"kind\":\"prefabMesh\",\"bytes\":";
    meta += std::to_wstring(needed); meta += L"}";
    if (FAILED(g_webview17->PostSharedBufferToScript(shared.Get(),
                                                     COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
                                                     meta.c_str())))
        ui_post_json(L"{\"kind\":\"prefabMeshTransportUnavailable\"}");
}

static void ui_post_window_state(HWND hwnd)
{
    if (!hwnd) return;
    ui_post_json(IsZoomed(hwnd)
        ? L"{\"kind\":\"windowState\",\"maximized\":true}"
        : L"{\"kind\":\"windowState\",\"maximized\":false}");
}

/* The two append-only config slots transport canonical JSON fragments, whole. */
static int ui_config_get_json(const std::string &key, std::string &value,
                               unsigned int *out_flags)
{
    unsigned int flags = 0;
    char *json = nullptr;
    value.clear();
    if (out_flags) *out_flags = 0;
    if (key.empty() || !g_iface || !g_iface->vtbl || !g_iface->vtbl->config_get_json)
        return -1;
    int got = g_iface->vtbl->config_get_json(g_iface, key.c_str(), &json, &flags);
    if (out_flags) *out_flags = flags;
    value = ui_take(json);
    return got < 0 ? -1 : (int)value.size();
}

static void ui_post_config_value(const std::string &key)
{
    std::string value;
    unsigned int flags = 0;
    int got = ui_config_get_json(key, value, &flags);
    g_config_status_flags |= flags;
    std::wstring message = L"{\"kind\":\"configValue\",\"key\":\"";
    message += sh_webview_json::escape_wide(key.c_str());
    message += L"\",\"valueJson\":\"";
    if (got >= 0) message += sh_webview_json::escape_wide(value.c_str());
    message += L"\",\"result\":";
    message += got >= 0 ? L"1}" : L"0}";
    ui_post_json(message.c_str());
}

static void ui_post_config_set_result(const std::string &key, int result)
{
    std::wstring message = L"{\"kind\":\"configSetResult\",\"key\":\"";
    message += sh_webview_json::escape_wide(key.c_str());
    message += L"\",\"result\":";
    message += std::to_wstring(result);
    message += L"}";
    ui_post_json(message.c_str());
}

static void ui_post_config_status()
{
    if (g_config_status_posted) return;
    std::wstring message = L"{\"kind\":\"configStatus\",\"flags\":";
    message += std::to_wstring(g_config_status_flags);
    message += L"}";
    ui_post_json(message.c_str());
    g_config_status_posted = true;
}

/* Count exact idSnapEntity keys without counting the idSnapEntityPrefab root. */
static int ui_count_prefab_entities(const std::string &body)
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
/* Read a whole auxiliary file; return false for a missing, empty or unreadable one. */
static bool ui_read_small_file(const char *path, std::string &body)
{
    FILE *fp = ui_open(path, L"rb");
    if (!fp) return false;
    _fseeki64(fp, 0, SEEK_END); long long sz = _ftelli64(fp); _fseeki64(fp, 0, SEEK_SET);
    bool ok = false;
    if (sz > 0) {
        body.resize((size_t)sz);
        size_t got = fread(&body[0], 1, (size_t)sz, fp);
        body.resize(got);
        ok = got == (size_t)sz;
    }
    fclose(fp);
    if (!ok) body.clear();
    return ok;
}
/* A view's top-level "kind" string, or "" (object prefabs have none). */
static std::string ui_meta_kind_of(const std::string &body)
{
    size_t k = body.find("\"kind\"");
    if (k == std::string::npos) return "";
    size_t c = body.find(':', k + 6);
    if (c == std::string::npos) return "";
    size_t q1 = body.find('"', c + 1);
    if (q1 == std::string::npos) return "";
    for (size_t p = c + 1; p < q1; p++) if (body[p] != ' ' && body[p] != '\t' && body[p] != '\r' && body[p] != '\n') return "";
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return body.substr(q1 + 1, q2 - q1 - 1);
}
/* The "entityCount" number in a module prefab's view, or -1. */
static int ui_meta_int(const std::string &body, const char *key)
{
    std::string k = std::string("\"") + key + "\"";
    size_t at = body.find(k);
    if (at == std::string::npos) return -1;
    size_t c = body.find(':', at + k.size());
    if (c == std::string::npos) return -1;
    return atoi(body.c_str() + c + 1);
}
/* The top-level "~type" of a prefab file from its last 256 KB, the block left out
 * (prefab_host.h); "" when that tail names none. */
static std::string ui_file_top_type(const char *path)
{
    FILE *fp = ui_open(path, L"rb");
    if (!fp) return std::string();
    fseek(fp, 0, SEEK_END); long sz = ftell(fp);
    long take = sz < 256 * 1024 ? sz : 256 * 1024;
    std::string tail;
    if (take > 0) {
        tail.resize((size_t)take);
        fseek(fp, sz - take, SEEK_SET);
        tail.resize(fread(&tail[0], 1, (size_t)take, fp));
    }
    fclose(fp);
    return sh_prefab_host::top_type_of_tail(tail.data(), tail.size());
}
/* The prefab file's size and write time, the content signature the page's
 * Limits cache keys on. */
static std::wstring ui_prefab_signature(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(ui_widen(path).c_str(), GetFileExInfoStandard, &fa)) return L"";
    unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    unsigned long long mtime = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    return std::to_wstring(size) + L":" + std::to_wstring(mtime);
}
/* The selected prefab's file text, capped so a hand-dropped giant file cannot
 * force a huge WebMessage allocation. */
static bool ui_read_prefab_scene(const char *path, std::string &body)
{
    FILE *fp = ui_open(path, L"rb");
    bool ok = false;
    body.clear();
    if (!fp) return false;
    _fseeki64(fp, 0, SEEK_END); long long sz = _ftelli64(fp); _fseeki64(fp, 0, SEEK_SET);
    if (sz > 0) {
        body.resize((size_t)sz);
        size_t got = fread(&body[0], 1, (size_t)sz, fp);
        body.resize(got);
        ok = got == (size_t)sz;
    }
    fclose(fp);
    if (!ok) body.clear();
    return ok;
}
/* A prefab's details as the view the page reads (SH_MODULE_PREFAB_META_READ): from the
 * file's snapmapPlus block, else (a prefab saved before the block) its sidecar. `text` is
 * the whole file when the caller has read it; otherwise the file is read from the end, a
 * window at a time and doubling, until the block's start is in it (or the whole file is).
 * Empty when there are none or this backend cannot answer. */
static std::string ui_prefab_view(const char *path, const char *meta_path, const std::string *text)
{
    std::string tail, out;
    bool is_tail = false;
    if (!text) {
        FILE *fp = ui_open(path, L"rb");
        if (fp) {
            _fseeki64(fp, 0, SEEK_END); long long sz = _ftelli64(fp);
            /* The block is the file's last member: read from the end until its key is in the
             * window, so a block of any size is read whole without reading the scene before it. */
            for (long long take = 1024 * 1024;; take *= 2) {
                if (take > sz) take = sz;
                tail.assign((size_t)(take > 0 ? take : 0), '\0');
                if (take > 0) {
                    _fseeki64(fp, sz - take, SEEK_SET);
                    tail.resize(fread(&tail[0], 1, (size_t)take, fp));
                }
                is_tail = take < sz;
                if (!is_tail || tail.find("\"snapmapPlus\"") != std::string::npos) break;
            }
            fclose(fp);
        }
        text = &tail;
    }
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_META_READ;
    call.text = text->c_str();
    call.text_len = text->size();
    call.tail = is_tail ? 1 : 0;
    call.meta_path = meta_path;
    int r = ui_module_prefab_call(&call, &out);
    return r <= 0 ? std::string() : out;
}
/* The backend's module check for a module prefab's text (SH_MODULE_PREFAB_CHECK):
 * its JSON, or empty when this backend or editor cannot answer. The text is the
 * whole file; the backend leaves its block out and reads its mod rows from it. */
static std::string ui_module_prefab_check(const std::string &scene, const char *meta_path)
{
    std::string out;
    sh_module_prefab_call call;
    if (scene.empty()) return std::string();
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_CHECK;
    call.text = scene.c_str();
    call.text_len = scene.size();
    call.meta_path = meta_path;   /* a module one of its mods provides is listed under Mods instead */
    int r = ui_module_prefab_call(&call, &out);
    return r < 0 ? std::string() : out;
}
/* The mods a view records, with whether each is installed (SH_MODULE_PREFAB_MODS_STATUS):
 * its JSON, or empty when the view records none or this backend cannot answer. */
static std::string ui_prefab_mods_status(const std::string &meta)
{
    std::string out;
    if (meta.empty() || meta.find("\"mods\"") == std::string::npos) return std::string();
    sh_module_prefab_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODULE_PREFAB_MODS_STATUS;
    call.text = meta.c_str();
    call.text_len = meta.size();
    int r = ui_module_prefab_call(&call, &out);
    return r < 0 ? std::string() : out;
}
static void ui_send_prefab_detail(const std::string &name, const std::string &folder)
{
    if (!g_webview) return;
    int entityCount = 0;
    bool ok = false, modules = false;
    std::string metaBody, sceneBody, fileBody;
    std::wstring sig;
    std::string pathText, mpText;
    if (!name.empty() && ui_prefab_file_path(folder, name, pathText) && ui_exists(pathText.c_str())) {
        if (!ui_prefab_meta_path(folder, name, mpText)) mpText.clear();
        const char *path = pathText.c_str(), *mp = mpText.c_str();
        /* The scene is sent only for the selected prefab, without the product's block. */
        bool whole = ui_read_prefab_scene(path, fileBody);
        /* the view ships as an ESCAPED STRING (the page JSON.parses it with a try/catch), never
         * spliced raw -- a hand-edited/truncated sidecar must not be able to invalidate this whole
         * message (PostWebMessageAsJson silently drops a malformed payload). */
        metaBody = ui_prefab_view(path, mp, whole ? &fileBody : nullptr);
        if (whole) {
            sceneBody = fileBody;
            sceneBody.resize(sh_prefab_block_strip(&sceneBody[0], sceneBody.size()));
        }
        /* The file's ~type decides the kind, with or without a block (prefab_host.h). */
        std::string kind = sh_prefab_host::kind_of_type(whole ? sh_webview_json::top_level_type(sceneBody)
                                                              : ui_file_top_type(path));
        modules = kind.empty() ? ui_meta_kind_of(metaBody) == "modules" : kind == "modules";
        sig = ui_prefab_signature(path);
        ok = whole || modules;
        entityCount = modules ? ui_meta_int(metaBody, "entityCount") : ui_count_prefab_entities(sceneBody);
        if (entityCount < 0 && modules) entityCount = ui_count_prefab_entities(sceneBody);   /* no summary */
        if (entityCount < 0) entityCount = 0;
    }
    std::wstring json = L"{\"kind\":\"prefabDetail\",\"name\":\""; json += sh_webview_json::escape_wide(name.c_str());
    json += L"\",\"folder\":\""; json += sh_webview_json::escape_wide(folder.c_str());
    json += L"\",\"sig\":\""; json += sig;
    json += L"\",\"ok\":"; json += ok ? L"true" : L"false";
    /* The kind the file's ~type gives (prefab_host.h). A module prefab shows its footprint from
     * its view; its scene still ships, once, for the Limits section's entity count. */
    json += modules ? L",\"prefabKind\":\"modules\"" : L",\"prefabKind\":\"objects\"";
    json += L",\"meta\":\""; json += sh_webview_json::escape_wide(metaBody.c_str());
    json += L"\",\"scene\":\""; json += sh_webview_json::escape_wide(sceneBody.c_str());
    json += L"\",\"count\":"; json += std::to_wstring(entityCount);
    if (modules) {
        /* an escaped string, parsed by the page under try/catch like the view */
        json += L",\"moduleCheck\":\""; json += sh_webview_json::escape_wide(ui_module_prefab_check(fileBody, mpText.c_str()).c_str());
        json += L"\"";
    }
    json += L",\"modsStatus\":\""; json += sh_webview_json::escape_wide(ui_prefab_mods_status(metaBody).c_str());
    json += L"\"}";
    g_webview->PostWebMessageAsJson(json.c_str());
}

/* The Prefabs tab's Limits section. The backend builds its budget table on the
 * game thread after each palette build; the page asks for it once (budgetTable)
 * and is sent it again whenever its generation changes, and asks for the map's
 * current usage (budgetUsage) when the tab opens and on each selection; from then
 * on the think loop sends it again whenever it or the held prefab changes. */
static bool     g_budget_wanted = false;   /* the page has asked for the table */
static unsigned g_budget_sent_gen = 0;     /* table generation last posted */

static int ui_budget_call(sh_budget_call *call)
{
    call->out = nullptr;
    if (!SH_IFACE_HAS_SLOT(g_iface, budget)) return 0;
    __try { return g_iface->vtbl->budget(g_iface, call); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Post {"kind":kind,"data":<backend JSON>} or {"kind":kind,"data":null}. The
 * backend writes the JSON itself (escaped names, integers), so it is spliced. */
static void ui_post_budget(const wchar_t *kind, const std::string &body, const std::wstring &extra = std::wstring())
{
    std::wstring m = L"{\"kind\":\""; m += kind; m += L"\",\"data\":";
    if (body.empty()) m += L"null";
    else {
        int wl = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.c_str(), (int)body.size(), nullptr, 0);
        if (wl <= 0) m += L"null";
        else {
            std::wstring wb((size_t)wl, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.c_str(), (int)body.size(), &wb[0], wl);
            m += wb;
        }
    }
    m += extra;
    m += L"}";
    ui_post_json(m.c_str());
}

/* Copy one budget op's JSON. Returns the generation, or 0 when there is none. */
static unsigned ui_budget_read(int op, std::string &out)
{
    sh_budget_call call;
    out.clear();
    memset(&call, 0, sizeof call);
    call.op = op;
    int r = ui_budget_call(&call);
    out = ui_take(call.out);
    return r == 1 ? call.generation : 0;
}

static void ui_send_budget_table()
{
    std::string body;
    unsigned gen = ui_budget_read(SH_BUDGET_TABLE, body);
    g_budget_wanted = true;
    if (!gen) { ui_post_budget(L"budgetTable", ""); return; }
    g_budget_sent_gen = gen;
    ui_post_budget(L"budgetTable", body);
}

/* The prefab held in the 3D view as the page's ",\"held\":{...}" member, or ",\"held\":null".
 * The editor budget already counts what is held (usage held and after Space are identical for
 * object and module prefabs), so the page must not add it a second time. */
static std::wstring ui_held_json()
{
    sh_prefab_watch watch;
    ui_prefab_watch(&watch);
    const std::string *name = nullptr, *folder = nullptr;
    const wchar_t *kind = L"";
    /* A held paste is this prefab only while it is the last one decided: Create New Timeline and
     * the asset browser paste through the same kind 2. */
    if (watch.held == SH_PREFAB_HELD_OBJECTS && !g_held_obj_name.empty() && watch.place_seq == g_held_obj_seq) {
        name = &g_held_obj_name; folder = &g_held_obj_folder; kind = L"objects";
    } else if (watch.held == SH_PREFAB_HELD_MODULES && !g_module_place_name.empty()) {
        name = &g_module_place_name; folder = &g_module_place_folder; kind = L"modules";
    }
    if (!name) return L",\"held\":null";
    std::wstring h = L",\"held\":{\"name\":\""; h += sh_webview_json::escape_wide(name->c_str());
    h += L"\",\"folder\":\""; h += sh_webview_json::escape_wide(folder->c_str());
    h += L"\",\"kind\":\""; h += kind; h += L"\"}";
    return h;
}

static sh_prefab_host::usage_gate g_usage_gate;

static void ui_send_budget_usage()
{
    std::string body;
    ui_budget_read(SH_BUDGET_USAGE, body);
    std::wstring held = ui_held_json();
    g_usage_wanted = true;
    g_usage_gate.changed(body, sh_webview_json::to_utf8(held));   /* what the page now has */
    ui_post_budget(L"budgetUsage", body, held);
}

/* UI cadence, once the page has asked: post the map's usage again whenever it or the held prefab
 * changed (an edit, a paste picked up, Space or Esc), never an unchanged value on a timer. */
static void ui_budget_usage_poll()
{
    if (!g_usage_wanted) return;
    std::string body;
    ui_budget_read(SH_BUDGET_USAGE, body);
    std::wstring held = ui_held_json();
    if (!g_usage_gate.changed(body, sh_webview_json::to_utf8(held))) return;
    ui_post_budget(L"budgetUsage", body, held);
}

/* UI cadence: re-send the table after a palette rebuild, only once asked. */
static void ui_budget_poll()
{
    if (!g_budget_wanted) return;
    sh_budget_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_BUDGET_TABLE;
    if (ui_budget_call(&call) == 0 && !call.generation) return;
    if (call.generation && call.generation != g_budget_sent_gen) ui_send_budget_table();
}

/* The Mods tab (mods_tab.js; backend/modui.c behind the `mods` slot). The page asks for the
 * document when the tab opens and on Refresh (a modsAction rescan; modsList only from the
 * mockup), and for an action (modsAction); the document is never polled (the script log's
 * modsScripts is, by the page, and answered at once: ui_mods_send_scripts). Each job runs on a short-lived worker
 * thread rather than this one. Install, New mod and Refresh do their file work (copies, salts,
 * builds, which can take minutes) on that worker, off DOOM's main thread, with no time limit; only
 * their game part (APPLY) and every other operation run on DOOM's main thread through
 * run_on_main. The worker touches no page state: it reports its phase through g_mods_phase, which
 * the think loop posts as modsProgress, and hands its documents back through g_mods_done, which
 * the think loop posts (ui_mods_finish), the same handoff as the rawmap picker. Each job ends with
 * a fresh document, so the list follows every action without being asked. */
#define UI_MODS_START_MS   8000   /* how long a job waits for the game to start a main-thread call */

struct UiModsJob {
    int         action;    /* SH_MODS_ACT_*, or 0 for the document only */
    bool        pick;      /* ask for the folder first (install) */
    std::string arg;
    int         run_rc;    /* 1 ran, 0 refused, -1 not run in time, -3 the picker was cancelled */
    int         list_rc;
    unsigned    list_gen;  /* the backend's generation when the document was built */
    bool        pushed;    /* a document the page did not ask for (ui_mods_push_tick) */
    bool        apply_pending;   /* the file work landed; the game did not start its part in time */
    std::string run_doc, list_doc;
};
static UiModsJob    g_mods_job;
static volatile LONG g_mods_busy = 0;
static volatile LONG g_mods_done = 0;
/* The running job's phase for the page: 0 none, 1 the file work, 2 the game's part. */
static volatile LONG g_mods_phase = 0;
static LONG          g_mods_phase_posted = 0;
/* The pushed refresh: once the page has asked for anything (modsList, or any modsAction, which
 * is all the shipped page sends), a change the window did not make
 * (a console verb, `sh_mods map add` or `uninstall`, the undo clear an Uninstall owes) moves the
 * backend's generation past the one of the last document posted, and the think loop posts a fresh
 * one, as each of the page's own actions ends with one. FETCH's size query (any thread, no main-
 * thread call) reads the generation every UI_MODS_PUSH_MS. */
#define UI_MODS_PUSH_MS 500
static bool          g_mods_page_seen = false;
static unsigned      g_mods_doc_gen = 0;
static ULONGLONG     g_mods_push_next = 0;

struct UiModsMain { sh_mods_call call; int rc; };

static int ui_mods_slot_seh(sh_mods_call *call)
{
    if (!SH_IFACE_HAS_SLOT(g_iface, mods)) return 0;
    __try { return g_iface->vtbl->mods(g_iface, call); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* The call, with its text (owned by the slot) taken whole into out. */
static int ui_mods_slot(sh_mods_call *call, std::string *out = nullptr)
{
    call->out = nullptr;
    int r = ui_mods_slot_seh(call);
    std::string text = ui_take(call->out);
    call->out = nullptr;
    if (out) *out = text;
    return r;
}

/* Runs on DOOM's main thread inside run_on_main. */
static int ui_mods_main_call(void *ctx)
{
    UiModsMain *m = (UiModsMain *)ctx;
    m->rc = ui_mods_slot_seh(&m->call);   /* LIST and RUN hand no text over */
    return m->rc;
}

/* run_on_main holds one call at a time and refuses another at once, so retry until the game has
 * started this one or the start budget is spent. Once started, run_on_main waits it out. */
static bool ui_mods_on_main(UiModsMain *m)
{
    ULONGLONG until = GetTickCount64() + UI_MODS_START_MS;
    if (!g_iface || !g_iface->vtbl || !g_iface->vtbl->run_on_main) return false;
    for (;;) {
        int r = 0;
        if (g_iface->vtbl->run_on_main(g_iface, ui_mods_main_call, m, 1000, &r) == 1) return true;
        if (GetTickCount64() >= until) return false;
        Sleep(40);
    }
}

/* Copy the backend's current document (FETCH runs on any thread). */
static bool ui_mods_fetch(std::string &out)
{
    sh_mods_call call;
    out.clear();
    memset(&call, 0, sizeof call);
    call.op = SH_MODS_FETCH;
    return ui_mods_slot(&call, &out) == 1;
}

/* The dialog for Install, on the worker thread (its own apartment): mod folders, or with `zip` mod
 * ZIPs (a Prefab or map export carries its mods so). Several may be picked; they are joined with
 * newlines, which no path contains, and the backend takes each one as a mod folder, a folder of
 * mods, or a ZIP of mods. */
static bool ui_pick_mods(bool zip, std::wstring &out)
{
    IFileOpenDialog *dlg = nullptr;
    bool ok = false;
    out.clear();
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg)
        return false;
    FILEOPENDIALOGOPTIONS opt = 0;
    if (SUCCEEDED(dlg->GetOptions(&opt)))
        dlg->SetOptions(opt | (zip ? FOS_FILEMUSTEXIST : FOS_PICKFOLDERS) | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM |
                        FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | FOS_PATHMUSTEXIST);
    if (zip) {
        static const COMDLG_FILTERSPEC kZip[] = { { L"Mod ZIP", L"*.zip" } };
        dlg->SetFileTypes(1, kZip);
        dlg->SetTitle(L"Install mods: choose mod ZIPs");
    } else dlg->SetTitle(L"Install mods: choose mod folders, or a folder that holds several");
    dlg->SetOkButtonLabel(L"Install");
    {
        static const GUID kModFolderPickerGuid =
            { 0x3c5e8a12, 0x47d1, 0x4b9e, { 0xa6, 0x0f, 0x5b, 0x21, 0xc4, 0x7e, 0x93, 0x08 } };
        dlg->SetClientGuid(kModFolderPickerGuid);
    }
    {
        std::wstring dir = ui_local_snapmap_dir();
        if (!dir.empty()) {
            IShellItem *folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
                dlg->SetDefaultFolder(folder);
                folder->Release();
            }
        }
    }
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItemArray *items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; i++) {
                IShellItem *item = nullptr;
                if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    if (path[0]) { if (!out.empty()) out += L"\n"; out += path; }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            items->Release();
        }
        ok = !out.empty();
    }
    dlg->Release();
    return ok;
}

static bool ui_mods_read_only(int action);   /* fwd: below the action names */

/* The actions whose file work runs on the worker itself (modui.h): never through run_on_main. */
static bool ui_mods_file_action(int action)
{
    return action == SH_MODS_ACT_INSTALL || action == SH_MODS_ACT_NEW_MOD || action == SH_MODS_ACT_RESCAN ||
           action == SH_MODS_ACT_ADD_MINE;
}

static DWORD WINAPI ui_mods_thread(LPVOID)
{
    UiModsJob &job = g_mods_job;
    job.run_rc = 0;
    job.list_rc = 0;
    if (job.pick) {
        std::wstring picked;
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool ok = ui_pick_mods(job.arg == "zip", picked);
        if (SUCCEEDED(init)) CoUninitialize();
        if (ok) job.arg = sh_webview_json::to_utf8(picked);
        else job.run_rc = -3;
    }
    if (job.action && job.run_rc != -3 && ui_mods_file_action(job.action)) {
        sh_mods_call call;
        memset(&call, 0, sizeof call);
        call.op = SH_MODS_RUN;
        call.action = job.action;
        call.arg = job.arg.c_str();
        InterlockedExchange(&g_mods_phase, 1);
        job.run_rc = (ui_mods_slot(&call) == 1 && ui_mods_fetch(job.run_doc)) ? 1 : 0;
        if (job.run_rc == 1 && job.run_doc.find("\"apply\":true") != std::string::npos) {
            /* The game's part: the serving index and the refresh, on DOOM's main thread. */
            UiModsMain m;
            std::string done;
            memset(&m, 0, sizeof m);
            m.call.op = SH_MODS_RUN;
            m.call.action = SH_MODS_ACT_APPLY;
            InterlockedExchange(&g_mods_phase, 2);
            if (ui_mods_on_main(&m) && m.rc == 1 && ui_mods_fetch(done)) job.run_doc = done;
            else job.apply_pending = true;
        }
    } else if (job.action && job.run_rc != -3) {
        UiModsMain m;
        memset(&m, 0, sizeof m);
        m.call.op = SH_MODS_RUN;
        m.call.action = job.action;
        m.call.arg = job.arg.c_str();
        if (!ui_mods_on_main(&m)) job.run_rc = -1;
        else job.run_rc = (m.rc == 1 && ui_mods_fetch(job.run_doc)) ? 1 : 0;
    }
    if (!ui_mods_read_only(job.action)) {
        UiModsMain m;
        memset(&m, 0, sizeof m);
        m.call.op = SH_MODS_LIST;
        if (!ui_mods_on_main(&m)) job.list_rc = -1;
        else job.list_rc = (m.rc == 1 && ui_mods_fetch(job.list_doc)) ? 1 : 0;
        job.list_gen = m.call.generation;
    }
    InterlockedExchange(&g_mods_done, 1);   /* last: publishes the job above */
    return 0;
}

static const wchar_t *ui_mods_action_name(int action)
{
    switch (action) {
    case SH_MODS_ACT_INSTALL:     return L"install";
    case SH_MODS_ACT_RESCAN:      return L"rescan";
    case SH_MODS_ACT_PALETTE:     return L"palette";
    case SH_MODS_ACT_UNINSTALL:   return L"uninstall";
    case SH_MODS_ACT_UNINSTALL_PREVIEW:  return L"uninstallPreview";
    case SH_MODS_ACT_MAP_REMOVE:         return L"mapRemoveMod";
    case SH_MODS_ACT_MAP_REMOVE_PREVIEW: return L"mapRemoveModPreview";
    case SH_MODS_ACT_MAP_ADD:            return L"mapAddMod";
    case SH_MODS_ACT_CHOOSE:             return L"chooseRegistry";
    case SH_MODS_ACT_FILES:              return L"modFiles";
    case SH_MODS_ACT_FILE_TEXT:          return L"modFileText";
    case SH_MODS_ACT_NEW_MOD:            return L"newMod";
    case SH_MODS_ACT_ADD_MINE:           return L"addToMine";
    default:                      return L"";
    }
}

/* The Files view's reads change nothing, so no fresh document follows them. */
static bool ui_mods_read_only(int action)
{
    return action == SH_MODS_ACT_FILES || action == SH_MODS_ACT_FILE_TEXT;
}

static int ui_mods_action_code(const std::wstring &name)
{
    if (name == L"install")    return SH_MODS_ACT_INSTALL;
    if (name == L"rescan")     return SH_MODS_ACT_RESCAN;
    if (name == L"palette")    return SH_MODS_ACT_PALETTE;
    if (name == L"uninstall")  return SH_MODS_ACT_UNINSTALL;
    if (name == L"uninstallPreview")    return SH_MODS_ACT_UNINSTALL_PREVIEW;
    if (name == L"mapRemoveMod")        return SH_MODS_ACT_MAP_REMOVE;
    if (name == L"mapRemoveModPreview") return SH_MODS_ACT_MAP_REMOVE_PREVIEW;
    if (name == L"mapAddMod")           return SH_MODS_ACT_MAP_ADD;
    if (name == L"chooseRegistry")      return SH_MODS_ACT_CHOOSE;
    if (name == L"modFiles")            return SH_MODS_ACT_FILES;
    if (name == L"modFileText")         return SH_MODS_ACT_FILE_TEXT;
    if (name == L"newMod")              return SH_MODS_ACT_NEW_MOD;
    if (name == L"addToMine")           return SH_MODS_ACT_ADD_MINE;
    return 0;
}

/* Splice backend JSON (UTF-8) into an envelope. */
static void ui_mods_post(const std::wstring &head, const std::string &body, const wchar_t *tail)
{
    std::wstring m = head;
    int wl = body.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.c_str(), (int)body.size(), nullptr, 0);
    if (wl <= 0) m += L"null";
    else {
        std::wstring wb((size_t)wl, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.c_str(), (int)body.size(), &wb[0], wl);
        m += wb;
    }
    m += tail;
    ui_post_json(m.c_str());
}

static void ui_mods_post_result(const wchar_t *action, bool ok, const wchar_t *message, const wchar_t *data)
{
    std::wstring m = L"{\"kind\":\"modsActionResult\",\"action\":\"";
    m += action;
    m += L"\",\"ok\":";
    m += ok ? L"true" : L"false";
    m += L",\"message\":\"";
    m += message;
    m += L"\",\"data\":";
    m += data ? data : L"null";
    m += L"}";
    ui_post_json(m.c_str());
}

/* Start a job unless one is running. Returns false (and says so) when busy. */
static bool ui_mods_begin(int action, bool pick, const std::string &arg)
{
    const wchar_t *name = ui_mods_action_name(action);
    if (InterlockedCompareExchange(&g_mods_busy, 1, 0) != 0) {
        /* A running job always ends with a fresh document, so a list request is already answered. */
        if (action) ui_mods_post_result(name, false, L"Another mods action is still running.", nullptr);
        return false;
    }
    g_mods_job.action = action;
    g_mods_job.pick = pick;
    g_mods_job.arg = arg;
    g_mods_job.pushed = false;
    g_mods_job.apply_pending = false;
    g_mods_job.list_gen = 0;
    g_mods_job.run_doc.clear();
    g_mods_job.list_doc.clear();
    HANDLE h = CreateThread(nullptr, 0, ui_mods_thread, nullptr, 0, nullptr);
    if (!h) {
        InterlockedExchange(&g_mods_busy, 0);
        if (action) ui_mods_post_result(name, false, L"Could not start the mods action.", nullptr);
        else ui_post_json(L"{\"kind\":\"mods\",\"data\":null,\"error\":\"Could not read the mods list.\"}");
        return false;
    }
    CloseHandle(h);
    if (action) {
        ui_logp("mods: %ls started", name);
    }
    return true;
}

/* The mod scripts' state and one mod's script log (SH_MODS_SCRIPTS), on the UI thread: the op
 * reads the script runtime on any thread and never touches the job's shared document, so the
 * page may ask while a job runs and while a Play is on. The page asks while the Mods tab shows
 * a mod with scripts. */
static void ui_mods_send_scripts(const std::string &salt)
{
    std::string doc;
    sh_mods_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODS_SCRIPTS;
    call.arg = salt.c_str();
    if (ui_mods_slot(&call, &doc) != 1) doc.clear();
    ui_mods_post(L"{\"kind\":\"modsScripts\",\"data\":", doc, L"}");
}

/* A finished job, on the UI thread. */
static void ui_mods_finish()
{
    UiModsJob &job = g_mods_job;
    const wchar_t *name = ui_mods_action_name(job.action);
    if (job.action) {
        if (job.run_rc == -3)
            ui_mods_post_result(name, false, L"", L"{\"cancelled\":true}");
        else if (job.run_rc == 1)
            ui_mods_post(std::wstring(L"{\"kind\":\"modsActionResult\",\"action\":\"") + name +
                         (job.apply_pending ? L"\",\"applyPending\":true,\"data\":" : L"\",\"data\":"), job.run_doc, L"}");
        else if (job.run_rc == -1)
            ui_mods_post_result(name, false, L"The game did not take the request. Try again with the editor open.", nullptr);
        else
            ui_mods_post_result(name, false, L"Snapmap+ could not do that. Try again; if it keeps failing, use Send feedback.", nullptr);
        ui_logp("mods: %ls finished (run %d, list %d)", name, job.run_rc, job.list_rc);
    }
    if (job.list_rc == 1) {
        g_mods_doc_gen = job.list_gen;
        ui_mods_post(job.pushed ? L"{\"kind\":\"mods\",\"pushed\":true,\"data\":" : L"{\"kind\":\"mods\",\"data\":",
                      job.list_doc, L"}");
    } else if (job.pushed) {
        /* A pushed refresh the game did not answer: the page keeps its document; asked again later. */
        g_mods_push_next = GetTickCount64() + 2000;
    } else if (ui_mods_read_only(job.action)) {
        /* A Files read: the page keeps its document. */
    } else ui_post_json(job.list_rc == -1
        ? L"{\"kind\":\"mods\",\"data\":null,\"error\":\"The game did not answer in time. The list is read while the editor is open.\"}"
        : L"{\"kind\":\"mods\",\"data\":null,\"error\":\"Snapmap+ could not read your mods. Press Refresh; if it stays, use Send feedback.\"}");
    job.run_doc.clear();
    job.list_doc.clear();
    InterlockedExchange(&g_mods_phase, 0);
    g_mods_phase_posted = 0;
    InterlockedExchange(&g_mods_done, 0);
    InterlockedExchange(&g_mods_busy, 0);
}

/* The running job's phase, on the UI thread: the page shows what it is doing (modsProgress). */
static void ui_mods_progress_tick()
{
    LONG phase = InterlockedCompareExchange(&g_mods_phase, 0, 0);
    if (!phase || phase == g_mods_phase_posted || InterlockedCompareExchange(&g_mods_done, 0, 0)) return;
    g_mods_phase_posted = phase;
    std::wstring m = L"{\"kind\":\"modsProgress\",\"action\":\"";
    m += ui_mods_action_name(g_mods_job.action);
    m += phase == 1 ? L"\",\"phase\":\"files\"}" : L"\",\"phase\":\"game\"}";
    ui_post_json(m.c_str());
}

/* The prefabs folder watcher (folder_watch.h). One thread watches the prefabs folder
 * (ReadDirectoryChangesW, subtree: names, writes, sizes) and notes each change in its debounce;
 * the think loop refreshes the Prefabs list 300 ms after the last change. A folder that is not
 * there yet is opened again every few seconds. The mods folder is not watched: the store is
 * scanned at boot, on Refresh, and after an install, a download or New mod. */
#define UI_WATCH_FILTER (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | \
                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE)
#define UI_WATCH_REOPEN_MS 5000

struct UiWatch {
    HANDLE       dir = INVALID_HANDLE_VALUE;
    OVERLAPPED   ov;
    DWORD        buf[4096];     /* DWORD-aligned, as ReadDirectoryChangesW requires */
    ULONGLONG    retry_at = 0;
};
static UiWatch          g_watch;
static CRITICAL_SECTION  g_watch_lock;
static sh_fw_debounce    g_fw_prefabs;
static volatile LONG     g_watch_started = 0;

static bool ui_watch_dir(std::wstring &out)
{
    std::string path;
    out.clear();
    if (!ui_prefab_dir("", path)) return false;
    out = ui_wpath(path.c_str());
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return !out.empty();
}

static bool ui_watch_arm(UiWatch &w)
{
    ResetEvent(w.ov.hEvent);
    return ReadDirectoryChangesW(w.dir, w.buf, (DWORD)sizeof w.buf, TRUE, UI_WATCH_FILTER, nullptr, &w.ov, nullptr) != 0;
}

static void ui_watch_close(UiWatch &w)
{
    if (w.dir != INVALID_HANDLE_VALUE) { CancelIo(w.dir); CloseHandle(w.dir); }
    w.dir = INVALID_HANDLE_VALUE;
    w.retry_at = GetTickCount64() + UI_WATCH_REOPEN_MS;
}

static void ui_watch_open(UiWatch &w)
{
    std::wstring dir;
    if (w.dir != INVALID_HANDLE_VALUE || GetTickCount64() < w.retry_at) return;
    if (!ui_watch_dir(dir)) { w.retry_at = GetTickCount64() + UI_WATCH_REOPEN_MS; return; }
    w.dir = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (w.dir == INVALID_HANDLE_VALUE) { w.retry_at = GetTickCount64() + UI_WATCH_REOPEN_MS; return; }
    if (!ui_watch_arm(w)) { ui_watch_close(w); return; }
    ui_log("watch: following the prefabs folder");
}

static DWORD WINAPI ui_watch_thread(LPVOID)
{
    UiWatch &w = g_watch;
    for (;;) {
        ui_watch_open(w);
        if (w.dir == INVALID_HANDLE_VALUE) { Sleep(UI_WATCH_REOPEN_MS); continue; }
        DWORD r = WaitForSingleObject(w.ov.hEvent, UI_WATCH_REOPEN_MS);
        if (r == WAIT_TIMEOUT) continue;
        if (r != WAIT_OBJECT_0) { Sleep(1000); continue; }
        DWORD bytes = 0;
        if (!GetOverlappedResult(w.dir, &w.ov, &bytes, FALSE)) { ui_watch_close(w); continue; }
        /* bytes 0: the buffer overflowed; something changed all the same. */
        {
            ULONGLONG now = GetTickCount64();
            EnterCriticalSection(&g_watch_lock);
            sh_fw_note(&g_fw_prefabs, now);
            LeaveCriticalSection(&g_watch_lock);
        }
        if (!ui_watch_arm(w)) ui_watch_close(w);
    }
}

static void ui_watch_start()
{
    if (InterlockedCompareExchange(&g_watch_started, 1, 0) != 0) return;
    InitializeCriticalSection(&g_watch_lock);
    sh_fw_init(&g_fw_prefabs, SH_FW_PREFABS_QUIET_MS);
    memset(&g_watch.ov, 0, sizeof g_watch.ov);
    g_watch.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE h = CreateThread(nullptr, 0, ui_watch_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else ui_log("watch: the prefabs folder watcher could not start; the list refreshes only when asked");
}

/* The pushed refresh (g_mods_doc_gen above), on the UI thread: a document only, as the page's
 * Refresh asks for, while no job of the window runs. */
static void ui_mods_push_tick()
{
    ULONGLONG now = GetTickCount64();
    if (!g_mods_page_seen || !g_mods_doc_gen || now < g_mods_push_next) return;
    g_mods_push_next = now + UI_MODS_PUSH_MS;
    if (InterlockedCompareExchange(&g_mods_busy, 0, 0) != 0) return;
    sh_mods_call call;
    memset(&call, 0, sizeof call);
    call.op = SH_MODS_GENERATION;
    if (ui_mods_slot(&call) == 0 && !call.generation) return;
    if (!call.generation || call.generation == g_mods_doc_gen) return;
    if (!ui_mods_begin(0, false, std::string())) return;
    g_mods_job.pushed = true;   /* the worker reads only action, pick and arg; finish reads this */
    ui_logp("mods: the backend changed outside the window (generation %u, page has %u); "
                "pushing a fresh document", call.generation, g_mods_doc_gen);
}

/* The think loop's part, on the UI thread. */
static void ui_watch_tick()
{
    if (!g_watch_started) {
        if (g_iface && g_webview_ready) ui_watch_start();
        return;
    }
    ULONGLONG now = GetTickCount64();
    bool prefabs = false;
    unsigned covered = 0;
    EnterCriticalSection(&g_watch_lock);
    if (sh_fw_due(&g_fw_prefabs, now)) { covered = sh_fw_done(&g_fw_prefabs); prefabs = true; }
    LeaveCriticalSection(&g_watch_lock);
    if (prefabs) {
        ui_logp("watch: the prefabs folder changed (%u change(s)); list refreshed", covered);
        ui_send_prefabs();
    }
}

/* A mod folder's own name under mods\: no separators, no drive, not "." or "..". */
static bool ui_mods_plain_folder(const std::wstring &name)
{
    if (name.empty() || name.size() > 200 || name == L"." || name == L"..") return false;
    for (wchar_t c : name)
        if (c == L'\\' || c == L'/' || c == L':' || c < 0x20 || c == L'*' || c == L'?' || c == L'"' || c == L'<' ||
            c == L'>' || c == L'|') return false;
    return true;
}

/* A mod folder's path under mods\: its organizing folders and its own folder joined with '\', each
 * part a plain folder name (the page sends the row's group and folder). */
static bool ui_mods_relative_folder(const std::wstring &path)
{
    if (path.size() > 400) return false;
    size_t at = 0;
    for (;;) {
        size_t cut = path.find(L'\\', at);
        if (!ui_mods_plain_folder(path.substr(at, cut == std::wstring::npos ? std::wstring::npos : cut - at))) return false;
        if (cut == std::wstring::npos) return true;
        at = cut + 1;
    }
}

/* Open the store folder in Explorer, creating it when it is not there yet; with `folder`, that mod
 * folder inside it instead (it must exist), named by its path under mods\. */
static void ui_mods_open_folder(const std::wstring &folder)
{
    sh_mods_call call;
    std::string root;
    bool ok = false;
    if (!folder.empty() && !ui_mods_relative_folder(folder)) {
        ui_mods_post_result(L"openFolder", false, L"That folder is not a mod folder.", nullptr);
        return;
    }
    memset(&call, 0, sizeof call);
    call.op = SH_MODS_ROOT;
    if (ui_mods_slot(&call, &root) == 1) {
        std::wstring w = ui_widen(root.c_str());
        if (!w.empty()) {
            if (folder.empty()) SHCreateDirectoryExW(nullptr, w.c_str(), nullptr);
            else {
                w += L"\\";
                w += folder;
                DWORD at = GetFileAttributesW(w.c_str());
                if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
                    ui_mods_post_result(L"openFolder", false, L"That mod folder is no longer there.", nullptr);
                    return;
                }
            }
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(w.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
                ok = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0));
                CoTaskMemFree(pidl);
            }
        }
    }
    ui_mods_post_result(L"openFolder", ok, ok ? L"" : L"The mods folder could not be opened.", nullptr);
}

/* A file's path below its mod folder for Open in editor: '/'-separated plain names, no drive, no "."
 * or ".." part. */
static bool ui_mods_relative_file(const std::wstring &path)
{
    if (path.empty()) return false;
    size_t at = 0;
    for (;;) {
        size_t cut = path.find(L'/', at);
        if (!ui_mods_plain_folder(path.substr(at, cut == std::wstring::npos ? std::wstring::npos : cut - at))) return false;
        if (cut == std::wstring::npos) return true;
        at = cut + 1;
    }
}

/* Open in editor (the Files view): one file of a mod folder, "<folder>\t<path below it>", in the
 * program Windows opens it with; a kind with none (a .decl) gets Windows' own Open with dialog. */
static void ui_mods_open_file(const std::wstring &arg)
{
    size_t tab = arg.find(L'\t');
    std::wstring folder = tab == std::wstring::npos ? L"" : arg.substr(0, tab), rel = tab == std::wstring::npos ? L"" : arg.substr(tab + 1);
    sh_mods_call call;
    std::string root;
    bool ok = false;
    if (!ui_mods_relative_folder(folder) || !ui_mods_relative_file(rel)) {
        ui_mods_post_result(L"openFile", false, L"That file is not inside a mod folder.", nullptr);
        return;
    }
    memset(&call, 0, sizeof call);
    call.op = SH_MODS_ROOT;
    if (ui_mods_slot(&call, &root) == 1) {
        std::wstring w = ui_widen(root.c_str()) + L"\\" + folder + L"\\" + rel;
        for (wchar_t &c : w) if (c == L'/') c = L'\\';
        DWORD at = GetFileAttributesW(w.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) {
            ui_mods_post_result(L"openFile", false, L"That file is no longer in the mod's folder; press Refresh.", nullptr);
            return;
        }
        INT_PTR r = (INT_PTR)ShellExecuteW(g_hwnd, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (r == SE_ERR_NOASSOC) r = (INT_PTR)ShellExecuteW(g_hwnd, L"openas", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ok = r > 32;
    }
    ui_mods_post_result(L"openFile", ok, ok ? L"" : L"Windows could not open the file. Open it from the mod's folder instead.", nullptr);
}

/* Extract a view's flat tags and re-escape each value. Never splice view
 * bytes into an outgoing JSON envelope. */
static bool ui_view_tags(const std::string &body, std::vector<std::string> &tags)
{
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

/* Enumerate root prefabs and one level of folders. Each prefab's view (its block, read
 * from the end of its file, else its sidecar) gives the list its tags, kinds and mods, so
 * filtering needs no per-prefab reads from the page. The list never sends a scene. */
static void ui_send_prefabs()
{
    if (!g_webview) return;
    std::vector<std::string> rootNames;
    std::vector<std::pair<std::string, std::vector<std::string>>> folders;
    std::string rootDir;
    if (ui_prefab_dir("", rootDir)) {
        ui_list_json_dir(rootDir, rootNames);

        std::vector<std::string> subdirs;
        std::wstring pattern = ui_wpath(rootDir.c_str()) + L"*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string dn = sh_webview_json::to_utf8(fd.cFileName);
                if (dn == "." || dn == "..") continue;
                subdirs.push_back(dn);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(subdirs.begin(), subdirs.end());
        for (size_t i = 0; i < subdirs.size(); i++) {
            std::vector<std::string> items;
            ui_list_json_dir(rootDir + subdirs[i] + "\\", items);
            folders.push_back(std::make_pair(subdirs[i], items));
        }
    }
    /* One view per prefab, keyed like the page keys them. */
    std::vector<std::pair<std::string, std::string>> views;
    auto readView = [&](const std::string &folder, const std::string &name) {
        std::string path, mp;
        if (!ui_prefab_file_path(folder, name, path)) return;
        if (!ui_prefab_meta_path(folder, name, mp)) mp.clear();
        std::string view = ui_prefab_view(path.c_str(), mp.c_str(), nullptr);
        if (view.empty()) return;
        views.push_back(std::make_pair(folder.empty() ? name : (folder + "/" + name), view));
    };
    for (size_t i = 0; i < rootNames.size(); i++) readView("", rootNames[i]);
    for (size_t i = 0; i < folders.size(); i++)
        for (size_t j = 0; j < folders[i].second.size(); j++) readView(folders[i].first, folders[i].second[j]);

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
    for (size_t v = 0; v < views.size(); v++) {
        std::vector<std::string> tags;
        if (!ui_view_tags(views[v].second, tags) || tags.empty()) continue;
        if (!firstMeta) json += L","; firstMeta = false;
        json += L"\""; json += sh_webview_json::escape_wide(views[v].first.c_str()); json += L"\":[";
        for (size_t t = 0; t < tags.size(); t++) {
            if (t) json += L",";
            json += L"\""; json += sh_webview_json::escape_wide(tags[t].c_str()); json += L"\"";
        }
        json += L"]";
    }
    /* kinds: only module prefabs, keyed like meta. The file's ~type decides, with or without a
     * block; a tail that names no type falls back to the view's "kind". */
    json += L"},\"kinds\":{";
    bool firstKind = true;
    auto kindOf = [&](const std::string &folder, const std::string &name) {
        std::string path;
        if (!ui_prefab_file_path(folder, name, path)) return;
        std::string key = folder.empty() ? name : (folder + "/" + name);
        std::string kind = sh_prefab_host::kind_of_type(ui_file_top_type(path.c_str()));
        if (kind.empty())
            for (size_t v = 0; v < views.size(); v++)
                if (views[v].first == key && ui_meta_kind_of(views[v].second) == "modules") kind = "modules";
        if (kind != "modules") return;
        if (!firstKind) json += L","; firstKind = false;
        json += L"\""; json += sh_webview_json::escape_wide(key.c_str()); json += L"\":\"modules\"";
    };
    for (size_t i = 0; i < rootNames.size(); i++) kindOf("", rootNames[i]);
    for (size_t i = 0; i < folders.size(); i++)
        for (size_t j = 0; j < folders[i].second.size(); j++) kindOf(folders[i].first, folders[i].second[j]);
    /* mods: the mods each view records, keyed like meta, as escaped status JSON the page parses. */
    json += L"},\"mods\":{";
    bool firstMods = true;
    for (size_t v = 0; v < views.size(); v++) {
        std::string status = ui_prefab_mods_status(views[v].second);
        if (status.empty()) continue;
        if (!firstMods) json += L","; firstMods = false;
        json += L"\""; json += sh_webview_json::escape_wide(views[v].first.c_str()); json += L"\":\"";
        json += sh_webview_json::escape_wide(status.c_str()); json += L"\"";
    }
    json += L"}}";
    g_webview->PostWebMessageAsJson(json.c_str());
    ui_post_editor_mode(true);
}

/* Write a prefab's description and tags into its file's snapmapPlus block (the backend
 * keeps its summary and mods; a prefab saved before the block takes its sidecar's, and the
 * sidecar is left as it was). Runs directly in the message callback like selectPrefab --
 * file I/O only, no engine touch. Posts savePrefabMetaResult, then a fresh prefabs list
 * (the tag map feeds the list filter). */
static void ui_apply_save_prefab_meta(const std::string &name, const std::string &folder, const std::string &body)
{
    int result = -1;
    std::string path, mp;
    if (!name.empty() && ui_prefab_file_path(folder, name, path)) {
        if (!ui_prefab_meta_path(folder, name, mp)) mp.clear();
        sh_module_prefab_call call;
        memset(&call, 0, sizeof call);
        call.op = SH_MODULE_PREFAB_META_WRITE;
        call.text = body.c_str();
        call.text_len = body.size();
        call.path = path.c_str();
        call.meta_path = mp.c_str();
        int w = ui_module_prefab_call(&call);
        result = w == 1 ? 1 : -1;   /* 0: this backend cannot write details */
    }
    if (!g_webview) return;
    std::wstring m = L"{\"kind\":\"savePrefabMetaResult\",\"result\":"; m += std::to_wstring(result);
    m += L",\"name\":\""; m += sh_webview_json::escape_wide(name.c_str()); m += L"\"}";
    g_webview->PostWebMessageAsJson(m.c_str());
    if (result == 1) ui_send_prefabs();
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
    ui_logf(ok ? "report: sent ok (http %lu)" : "report: send FAILED (http %lu)", (unsigned long)status);
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
static const char *kCrashDir  = "snapmap-plus\\crash";   /* CWD = the game dir (ui_log's convention) */
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
 * before attachment, in every encoding a log can carry them in (report_scrub.h).
 * This targets those identifiers, not arbitrary personal data. */
static std::string crash_collect_logs()
{
    static const char *files[] = { "shield_faults.log", "sh_backend.log", "snapmap-plus-ui.log" };
    static rs_forms names;
    memset(&names, 0, sizeof names);
    {
        wchar_t user[UNLEN + 1] = L"", comp[MAX_COMPUTERNAME_LENGTH + 1] = L"", prof[MAX_PATH] = L"", shortp[MAX_PATH] = L"";
        DWORD un = UNLEN + 1, cn = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetUserNameW(user, &un)) rs_add_name(&names, user, "<user>");
        if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) {
            rs_add_path_leaf(&names, prof, "<user>");
            if (GetShortPathNameW(prof, shortp, MAX_PATH)) rs_add_path_leaf(&names, shortp, "<user>");
        }
        if (GetComputerNameW(comp, &cn)) rs_add_name(&names, comp, "<machine>");
    }
    std::string out;
    std::vector<char> raw(CRASH_LOG_TAIL_KEEP + 4096), scrub(4 * (CRASH_LOG_TAIL_KEEP + 4096));
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
        std::string chunk(scrub.data(), rs_scrub_forms(scrub.data(), scrub.size(), raw.data() + off, got - off, &names));
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
static LRESULT CALLBACK UiWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_controller) { RECT rc; GetClientRect(hwnd, &rc); g_controller->put_Bounds(rc); }
        ui_post_window_state(hwnd);
        return 0;
    case WM_CLOSE: {
        /* The window lives with the editor; Alt+F4 says so once instead of doing nothing. */
        static bool told = false;
        if (!told && g_webview_ready) { told = true; ui_post_json(L"{\"kind\":\"closeRefused\"}"); }
        return 0;
    }
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
static void ui_create_window()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = UiWndProc; wc.hInstance = GetModuleHandleW(nullptr);
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
        if (FAILED(hr)) ui_logf("DwmExtendFrameIntoClientArea failed hr=0x%08lx", (unsigned long)hr);
    }
    /* Recalculate before showing so the native caption does not linger until resize. */
    if (g_hwnd)
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ui_logf("window created (hwnd=%lu)", (unsigned long)(uintptr_t)g_hwnd);
}
static HRESULT on_message(ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    LPWSTR jp = nullptr;
    if (SUCCEEDED(args->get_WebMessageAsJson(&jp)) && jp) {
        sh_config_message config_message = sh_extract_config_message(
            jp, SIZE_MAX, SIZE_MAX);   /* a setting's key and value have no size of their own */
        if (config_message.kind != SH_CONFIG_MESSAGE_OTHER) {
            CoTaskMemFree(jp);
            std::string key, value;
            bool fields_valid = config_message.fields_valid;
            if (fields_valid) {
                key = sh_webview_json::to_utf8(config_message.key);
                if (config_message.kind == SH_CONFIG_MESSAGE_SET)
                    value = sh_webview_json::to_utf8(config_message.value_json);
                fields_valid =
                    !key.empty() &&
                    key.find('\0') == std::string::npos;
                if (config_message.kind == SH_CONFIG_MESSAGE_SET)
                    fields_valid =
                        fields_valid &&
                        value.find('\0') == std::string::npos;
            }

            if (config_message.kind == SH_CONFIG_MESSAGE_GET) {
                if (!fields_valid) key.clear();
                ui_post_config_value(key);
            } else {
                int result = SH_CONFIG_SET_REJECTED;
                if (fields_valid && g_iface && g_iface->vtbl &&
                    g_iface->vtbl->config_set_json) {
                    result = g_iface->vtbl->config_set_json(
                        g_iface, key.c_str(), value.c_str());
                }
                if (!fields_valid) key.clear();
                ui_post_config_set_result(key, result);
            }
            return S_OK;
        }

        std::wstring json(jp); CoTaskMemFree(jp);
        if (g_pair_mismatch) return S_OK;   /* the window only shows the reinstall notice */
        std::wstring cmd;
        if (sh_webview_json::get_string(json, L"cmd", cmd)) {
            if (cmd == L"perfList") {
                int seq = 0, entities = 0, matched = 0, mounted = 0;
                double render_ms = 0.0, handler_ms = 0.0;
                if (sh_webview_json::get_int(json, L"seq", &seq) &&
                    sh_webview_json::get_int(json, L"entities", &entities) &&
                    sh_webview_json::get_int(json, L"matched", &matched) &&
                    sh_webview_json::get_int(json, L"mounted", &mounted) &&
                    sh_webview_json::get_double(json, L"renderMs", &render_ms) &&
                    sh_webview_json::get_double(json, L"handlerMs", &handler_ms) &&
                    seq >= 0 && entities >= 0 &&
                    matched >= 0 && matched <= entities && mounted >= 0 && mounted <= matched &&
                    std::isfinite(render_ms) && std::isfinite(handler_ms) &&
                    render_ms >= 0.0 && render_ms <= 600000.0 &&
                    handler_ms >= 0.0 && handler_ms <= 600000.0) {
                    ui_logp("perf ui-list: seq=%d entities=%d matched=%d mounted=%d render_ms=%.3f handler_ms=%.3f",
                                seq, entities, matched, mounted, render_ms, handler_ms);
                }
            } else if (cmd == L"listPrefabs") {
                ui_send_prefabs();
            } else if (cmd == L"selectPrefab") {
                std::wstring nm, fo; sh_webview_json::get_string(json, L"name", nm); sh_webview_json::get_string(json, L"folder", fo);
                ui_send_prefab_detail(sh_webview_json::to_utf8(nm), sh_webview_json::to_utf8(fo));
            } else if (cmd == L"budgetTable") {
                ui_send_budget_table();
            } else if (cmd == L"budgetUsage") {
                ui_send_budget_usage();
            } else if (cmd == L"modsList") {
                g_mods_page_seen = true;
                ui_mods_begin(0, false, std::string());
            } else if (cmd == L"modsScripts") {
                std::wstring salt;
                int full = 0;
                sh_webview_json::get_string(json, L"salt", salt);
                sh_webview_json::get_int(json, L"full", &full);
                ui_mods_send_scripts((full ? std::string(SH_MODS_SCRIPTS_FULL) : std::string()) +
                                     sh_webview_json::to_utf8(salt));
            } else if (cmd == L"modsAction") {
                g_mods_page_seen = true;   /* the page's open and Refresh are a rescan: arm the push */
                /* Install always asks for its folders or ZIPs here, in the native picker: the page
                 * never names a path to install, only which picker ("zip" or none). */
                std::wstring act, arg;
                sh_webview_json::get_string(json, L"action", act);
                sh_webview_json::get_string(json, L"arg", arg);
                if (act == L"openFolder") ui_mods_open_folder(arg);
                else if (act == L"openFile") ui_mods_open_file(arg);
                else {
                    int code = ui_mods_action_code(act);
                    if (!code) ui_mods_post_result(L"unknown", false, L"Unknown mods action.", nullptr);
                    else if (code == SH_MODS_ACT_INSTALL) ui_mods_begin(code, true, arg == L"zip" ? "zip" : std::string());
                    else ui_mods_begin(code, false, sh_webview_json::to_utf8(arg));
                }
            } else if (cmd == L"resolvePrefabModel") {
                int generation = 0; std::wstring inherit;
                sh_webview_json::get_int(json, L"generation", &generation);
                sh_webview_json::get_string(json, L"inherit", inherit);
                std::string inherit8 = sh_webview_json::to_utf8(inherit);
                /* scale[0..2] renderModelInfo.scale, scale[3..5] the palette row's renderInfo.size. */
                std::string model; float scale[6] = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}; int flags = 0;
                if (generation >= 0 && g_iface && g_iface->vtbl && g_iface->vtbl->resolve_prefab_defaults) {
                    char *owned = nullptr;
                    flags = g_iface->vtbl->resolve_prefab_defaults(g_iface, inherit8.c_str(), &owned, scale, 6);
                    model = ui_take(owned);
                }
                std::wstring m = L"{\"kind\":\"prefabModelResolved\",\"generation\":";
                m += std::to_wstring(generation);
                m += L",\"inherit\":\""; m += sh_webview_json::escape_wide(inherit8.c_str());
                m += L"\",\"model\":\"";
                if (flags & SH_PREFAB_DEFAULT_MODEL) m += sh_webview_json::escape_wide(model.c_str());
                m += L"\",\"scale\":";
                if (flags & SH_PREFAB_DEFAULT_SCALE) {
                    m += L"["; m += std::to_wstring(scale[0]); m += L",";
                    m += std::to_wstring(scale[1]); m += L",";
                    m += std::to_wstring(scale[2]); m += L"]";
                } else m += L"null";
                m += L",\"editorModel\":";
                m += (flags & SH_PREFAB_DEFAULT_EDITOR) ? L"true" : L"false";
                m += L",\"size\":";
                if (flags & SH_PREFAB_DEFAULT_SIZE) {
                    m += L"["; m += std::to_wstring(scale[3]); m += L",";
                    m += std::to_wstring(scale[4]); m += L",";
                    m += std::to_wstring(scale[5]); m += L"]";
                } else m += L"null";
                m += L"}";
                ui_post_json(m.c_str());
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
                    ui_post_json(m.c_str());
                }
            } else if (cmd == L"savePrefabMeta") {
                std::wstring nm, fo, body; sh_webview_json::get_string(json, L"name", nm); sh_webview_json::get_string(json, L"folder", fo); sh_webview_json::get_string(json, L"body", body);
                ui_apply_save_prefab_meta(sh_webview_json::to_utf8(nm), sh_webview_json::to_utf8(fo), sh_webview_json::to_utf8(body));
            } else if (cmd == L"createPrefab") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                g_create_prefab_name = sh_webview_json::to_utf8(nm);
                g_pending_create_prefab = true;
            } else if (cmd == L"createModulePrefab") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                g_create_module_name = sh_webview_json::to_utf8(nm);
                g_pending_create_module = true;
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
            } else if (cmd == L"movePrefabs") {
                std::vector<std::wstring> names, froms; std::wstring tof;
                sh_webview_json::get_string_array(json, L"names", names); sh_webview_json::get_string_array(json, L"fromFolders", froms); sh_webview_json::get_string(json, L"toFolder", tof);
                g_move_prefabs_names.clear(); g_move_prefabs_from.clear();
                for (size_t i = 0; i < names.size(); i++) { g_move_prefabs_names.push_back(sh_webview_json::to_utf8(names[i])); g_move_prefabs_from.push_back(i < froms.size() ? sh_webview_json::to_utf8(froms[i]) : std::string()); }
                g_move_prefabs_to = sh_webview_json::to_utf8(tof);
                g_pending_move_prefabs = true;
            } else if (cmd == L"setDisplayNames") {
                /* The Timelines tree's folder moves and renames: eids[i] takes names[i]. */
                std::vector<int> eids; std::vector<std::wstring> names;
                sh_webview_json::get_int_array(json, L"eids", eids); sh_webview_json::get_string_array(json, L"names", names);
                EnterCriticalSection(&g_loop->mtx);
                g_set_names_eids.clear(); g_set_names_names.clear();
                for (size_t i = 0; i < eids.size() && i < names.size(); i++) { g_set_names_eids.push_back(eids[i]); g_set_names_names.push_back(sh_webview_json::to_utf8(names[i])); }
                g_pending_set_names = !g_set_names_eids.empty();
                LeaveCriticalSection(&g_loop->mtx);
            } else if (cmd == L"deletePrefabs") {
                std::vector<std::wstring> names, folders;
                sh_webview_json::get_string_array(json, L"names", names); sh_webview_json::get_string_array(json, L"folders", folders);
                g_delete_prefabs_names.clear(); g_delete_prefabs_folders.clear();
                for (size_t i = 0; i < names.size(); i++) { g_delete_prefabs_names.push_back(sh_webview_json::to_utf8(names[i])); g_delete_prefabs_folders.push_back(i < folders.size() ? sh_webview_json::to_utf8(folders[i]) : std::string()); }
                g_pending_delete_prefabs = true;
            } else if (cmd == L"select") {
                int eid = -1;
                if (sh_webview_json::get_int(json, L"eid", &eid)) { g_displayed_eid = eid; ui_send_state(eid, false); }
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
                ui_send_enum(0, nullptr);
            } else if (cmd == L"enumClasses") {
                std::wstring inh; sh_webview_json::get_string(json, L"inherit", inh);
                std::string i8 = sh_webview_json::to_utf8(inh);
                ui_send_enum(1, i8.c_str());
            } else if (cmd == L"enumEvents") {
                ui_send_events();
            } else if (cmd == L"enumEventDocs") {
                ui_send_event_docs();
            } else if (cmd == L"enumEntityAssets") {
                ui_send_entity_assets();
            } else if (cmd == L"enumArgResclass") {
                std::wstring rc; sh_webview_json::get_string(json, L"resClass", rc);
                std::string rc8 = sh_webview_json::to_utf8(rc);
                ui_send_arg_resclass(rc8.c_str());
            } else if (cmd == L"lookupDesc") {
                std::wstring inh, cls; sh_webview_json::get_string(json, L"inherit", inh); sh_webview_json::get_string(json, L"classname", cls);
                std::string i8 = sh_webview_json::to_utf8(inh), c8 = sh_webview_json::to_utf8(cls);
                ui_send_desc(i8.c_str(), c8.c_str());
            } else if (cmd == L"rawmapStatus") {
                ui_send_rawmap_status(L"");
            } else if (cmd == L"rawmapLoadPick") {
                /* Both pickers return immediately -- the dialog runs on its own thread and the think
                 * loop acts on the result (ui_finish_pick); Show() here would stall that loop. */
                ui_begin_pick(0);
            } else if (cmd == L"rawmapSaveNow") {
                /* Resolve the current destination in the backend; the page's last
                 * status may belong to a map that has since closed. */
                ui_rawmap_configure(nullptr, nullptr, 5, nullptr);
            } else if (cmd == L"rawmapSavePick") {
                ui_begin_pick(1);
            } else if (cmd == L"exportPrefab") {
                /* the prefab with its mods, as one ZIP: the Windows Save dialog first */
                std::wstring nm, fo; sh_webview_json::get_string(json, L"name", nm); sh_webview_json::get_string(json, L"folder", fo);
                if (InterlockedCompareExchange(&g_pick_busy, 0, 0) == 0) {
                    g_pick_prefab_name = sh_webview_json::to_utf8(nm);
                    g_pick_prefab_folder = sh_webview_json::to_utf8(fo);
                    g_pick_file_name = nm + L".zip";
                }
                ui_begin_pick(2);
            } else if (cmd == L"importPrefab") {
                ui_begin_pick(3);
            } else if (cmd == L"navmeshState") {
                ui_send_navmesh_state();
            } else if (cmd == L"navmeshUpdate") {
                if (g_iface && g_iface->vtbl && g_iface->vtbl->navmesh_view)
                    ui_free_text(g_iface->vtbl->navmesh_view(g_iface, 1));
                ui_send_navmesh_state();
            } else if (cmd == L"rawmapLoadNow") {
                /* Open the load path now through the backend's editor-frame hook. The reply is a
                 * REQUEST result; the reload lands a frame later, and a failure there is told by the
                 * backend's own toast and console line. */
                {
                    char *said = nullptr;
                    int ok = g_iface->vtbl->rawmap_load_now(g_iface, &said);
                    std::string msg = ui_take(said);
                    ui_log(ok ? "rawmap load-now: accepted" : "rawmap load-now: refused");
                    ui_send_rawmap_status(ok ? L"Opening as a new map -- Save will ask you to name it."
                                              : sh_webview_json::escape_wide(msg.c_str()).c_str(), nullptr, !ok);
                }
            } else if (cmd == L"rawmapKeepHere") {
                /* "Keep saving to file". Arm codes rather than a new vtable slot: the two
                 * DLLs must match slot for slot, and `arm` was already a verb code. Neither
                 * value writes anything -- 3 pins the path the readout is already showing,
                 * 4 releases it, so the line under the tick is true either way. */
                int on = 0; sh_webview_json::get_int(json, L"on", &on);
                ui_rawmap_configure(nullptr, nullptr, on ? 3 : 4,
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
                ui_send_asset_list(akind);
            } else if (cmd == L"pinsLoad") {
                ui_send_pins();
            } else if (cmd == L"pinsSave") {
                std::wstring doc; sh_webview_json::get_string(json, L"doc", doc);
                ui_save_pins(sh_webview_json::to_utf8(doc));
            } else if (cmd == L"getPreview") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                std::string n8 = sh_webview_json::to_utf8(nm);
                ui_send_preview(n8.c_str());
            } else if (cmd == L"requestPreview") {
                std::wstring nm; sh_webview_json::get_string(json, L"name", nm);
                int asset_kind = -1; sh_webview_json::get_int(json, L"assetKind", &asset_kind);
                std::string n8 = sh_webview_json::to_utf8(nm);
                ui_request_preview(n8.c_str(), asset_kind);
            } else if (cmd == L"cancelPreview") {
                ui_cancel_preview();
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
                            ui_post_json(L"{\"kind\":\"reportResult\",\"ok\":false}");
                        }
                    } else ui_post_json(L"{\"kind\":\"reportResult\",\"ok\":false,\"reason\":\"tooLarge\"}");
                }
            } else if (cmd == L"crashSubmit") {
                /* Compose crash reports here to add optional local log tails, then use the
                 * same worker and result message as ordinary feedback. */
                std::wstring title, bodyw, contact, hp, renderer, game;
                int attach = 0;
                sh_webview_json::get_string(json, L"title", title);
                sh_webview_json::get_string(json, L"body", bodyw);
                sh_webview_json::get_string(json, L"contact", contact);
                sh_webview_json::get_string(json, L"website", hp);
                /* Prefer the recorded renderer: this session may differ from the one that
                 * faulted. Use the live renderer only for older records without the field. */
                sh_webview_json::get_string(json, L"renderer", renderer);
                /* Same for the game identity recorded at arm time. */
                sh_webview_json::get_string(json, L"game", game);
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
                    p += "\",\"game\":\"";                         p += sh_webview_json::escape_utf8(
                        game.empty() ? g_game : sh_webview_json::to_utf8(game));
                    p += "\",\"website\":\"";                      p += sh_webview_json::escape_utf8(sh_webview_json::to_utf8(hp));
                    p += "\",\"logs\":\"";
                    /* The logs give way first: their oldest lines go until the report fits the cap. */
                    {
                        size_t room = REPORT_PAYLOAD_CAP > p.size() + 2 ? REPORT_PAYLOAD_CAP - p.size() - 2 : 0;
                        size_t cut = rs_fit_escaped_tail(logs.data(), logs.size(), room);
                        if (cut) ui_logf("crash report: logs trimmed by %lu byte(s) to fit the report", (unsigned long)cut);
                        p += sh_webview_json::escape_utf8(logs.substr(cut));
                    }
                    p += "\"}";
                    if (p.size() <= REPORT_PAYLOAD_CAP) {
                        g_report_payload.swap(p);
                        g_report_is_crash = true;
                        g_report_inflight = true;
                        HANDLE h = CreateThread(nullptr, 0, report_thread, nullptr, 0, nullptr);
                        if (h) CloseHandle(h);
                        else {
                            g_report_inflight = false; g_report_is_crash = false;
                            ui_post_json(L"{\"kind\":\"reportResult\",\"ok\":false}");
                        }
                    } else ui_post_json(L"{\"kind\":\"reportResult\",\"ok\":false,\"reason\":\"tooLarge\"}");
                }
            } else if (cmd == L"crashDismiss") {
                /* dismiss = handled: clear the pending records so the dialog never nags twice. The
                 * full logs + any crash dump stay on disk untouched. */
                crash_clear_pending();
            } else if (cmd == L"winMin") {
                ShowWindow(g_hwnd, SW_MINIMIZE);
            } else if (cmd == L"winMax") {
                ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                ui_post_window_state(g_hwnd);
            } else if (cmd == L"winState") {
                ui_post_window_state(g_hwnd);
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
                std::vector<int> uselids;
                int un = ui_get_selection(uselids);
                wchar_t m[96];
                if (un == 1) _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"tlUseSelectionResult\",\"ok\":true,\"eid\":%d}", uselids[0]);
                else _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"tlUseSelectionResult\",\"ok\":false,\"count\":%d}", un);
                ui_post_json(m);
            } else if (cmd == L"flyTo") {
                /* Fly the editor camera to one entity (apply kind 4, serviced by the editor
                 * frames). A held Lock position would rewrite the eye every frame, so it is released. */
                int eid = -1; sh_webview_json::get_int(json, L"eid", &eid);
                bool ok = eid >= 0 && g_iface && g_iface->vtbl && g_iface->vtbl->apply_edit;
                if (ok) {
                    g_cam_lock = false;
                    sh_apply_item it; it.kind = 4; it.id = eid; it.text = "";
                    ok = ui_apply_edit_seh(&it, 1, "fly-to") != 0;
                }
                wchar_t m[96];
                _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"flyToResult\",\"result\":%d,\"eid\":%d}", ok ? 1 : 0, eid);
                ui_post_json(m);
            } else if (cmd == L"pushStack") {
                std::vector<int> ids; sh_webview_json::get_int_array(json, L"eids", ids);
                bool ok = g_iface && g_iface->vtbl && g_iface->vtbl->push_to_stack && !ids.empty();
                if (ok) g_iface->vtbl->push_to_stack(g_iface, 0, ids.data(), (int)ids.size());
                wchar_t m[160];
                _snwprintf_s(m, _countof(m), _TRUNCATE,
                    L"{\"kind\":\"pushStackResult\",\"result\":%d,\"count\":%d}", ok ? 1 : 0, (int)ids.size());
                ui_post_json(m);
            } else if (cmd == L"clearStack") {
                bool ok = g_iface && g_iface->vtbl && g_iface->vtbl->clear_stack;
                int had = ok ? g_iface->vtbl->clear_stack(g_iface, 0) : 0;
                wchar_t m[128];
                _snwprintf_s(m, _countof(m), _TRUNCATE,
                    L"{\"kind\":\"clearStackResult\",\"result\":%d,\"count\":%d}", ok ? 1 : 0, had);
                ui_post_json(m);
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
        ui_logf("NavigationCompleted FAILED status=%lu",
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
    if (g_pair_mismatch) {
        ui_post_json(L"{\"kind\":\"installMismatch\"}");
        return S_OK;
    }
    ui_post_config_status();
    ui_send_list("navigation-completed");
    return S_OK;
}
static HRESULT on_controller_created(HRESULT result, ICoreWebView2Controller *controller)
{
    if (FAILED(result) || !controller) { ui_logf("controller creation FAILED hr=0x%08lx", (unsigned long)result); return result; }
    g_controller = controller; g_controller->AddRef();
    g_controller->get_CoreWebView2(&g_webview);
    if (!g_webview) { ui_log("get_CoreWebView2 null"); return E_FAIL; }
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
        ui_logf("NavigateToString FAILED hr=0x%08lx",
                 (unsigned long)navigate);
        return navigate;
    }
    ui_log("controller ready: navigated to embedded HTML");
    return S_OK;
}
static HRESULT on_environment_created(HRESULT result, ICoreWebView2Environment *env)
{
    if (FAILED(result) || !env) { ui_logf("environment creation FAILED hr=0x%08lx", (unsigned long)result); return result; }
    g_webview_environment12.Reset();
    env->QueryInterface(__uuidof(ICoreWebView2Environment12),
                        reinterpret_cast<void **>(g_webview_environment12.GetAddressOf()));
    return env->CreateCoreWebView2Controller(g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(on_controller_created).Get());
}
#ifdef SH_DIAG
/* Diagnostic build only (never the release page): a file
 * `%LOCALAPPDATA%\\snapmap-plus\\devtools-port` holding one port number
 * (1024..65535) opens Chromium's remote debugging on 127.0.0.1 at that port for
 * this window, so the development daemon's window step can inspect and drive
 * the page. Absent or malformed, nothing is opened. */
static int ui_devtools_port(const wchar_t *local)
{
    if (!local || !*local) return 0;
    std::wstring path = std::wstring(local) + L"\\snapmap-plus\\devtools-port";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[16] = {}; DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)(sizeof buf - 1), &got, nullptr);
    CloseHandle(h);
    if (!ok) return 0;
    long port = 0; int digits = 0;
    for (DWORD i = 0; i < got; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') { port = port * 10 + (c - '0'); digits++; if (digits > 5) return 0; }
        else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') { if (digits) break; }
        else return 0;
    }
    return (digits && port >= 1024 && port <= 65535) ? (int)port : 0;
}
#endif

static void ui_start_webview()
{
    std::wstring local, udf = L".";
    {
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) local = known;
        if (known) CoTaskMemFree(known);
    }
    bool have_local = !local.empty();
    if (have_local) {
        /* %LOCALAPPDATA%\snapmap-plus\cache\webview2; an older snapmap-plus\webview2 folder is renamed into
         * cache\ once (webview_profile.h). */
        int how = sh_webview_profile::created; DWORD error = 0;
        udf = sh_webview_profile::choose(local, &how, &error);
        if (how == sh_webview_profile::moved) ui_log("webview2 profile moved into cache\\webview2");
        else if (how == sh_webview_profile::kept_old)
            ui_logf("webview2 profile could not be moved into cache\\ (error %lu); using the old folder this time", (unsigned long)error);
    }
    ComPtr<ICoreWebView2EnvironmentOptions> options;
#ifdef SH_DIAG
    int devtools_port = have_local ? ui_devtools_port(local.c_str()) : 0;
    if (devtools_port) {
        auto opts = Make<CoreWebView2EnvironmentOptions>();
        wchar_t args[64]; _snwprintf_s(args, _countof(args), _TRUNCATE, L"--remote-debugging-port=%d", devtools_port);
        if (opts && SUCCEEDED(opts->put_AdditionalBrowserArguments(args))) {
            options = opts;
            ui_logf("devtools: remote debugging on 127.0.0.1:%d (diagnostic build, devtools-port file)", devtools_port);
        }
    }
#endif
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(on_environment_created).Get());
    if (FAILED(hr)) ui_logf("Create env FAILED hr=0x%08lx", (unsigned long)hr);
}

/* UI request drain and polling loop, about 30 Hz. */
static void ui_think_loop()
{
    bool was_visible = false;
    unsigned frame = 0;
    for (;;) {
        frame++;
        if (g_pair_mismatch) {
            /* Shown with the editor like the working window, holding only the notice. */
            bool ready = g_webview_ready && ui_editor_ready() != 0;
            if (ready != was_visible) { ShowWindow(g_hwnd, ready ? SW_SHOW : SW_HIDE); was_visible = ready; }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(0x21);
            continue;
        }
        bool did_save = false, did_delete = false, did_create_prefab = false, did_select_refused = false;
        int  select_refused_reason = 0;
        bool did_delete_prefab = false, did_rename_prefab = false, did_load_prefab = false;
        bool did_new_entity = false, did_create_module = false, did_set_names = false;
        bool did_sound_preview = false;
        bool did_create_folder = false, did_rename_folder = false, did_delete_folder = false, did_move_prefab = false;
        bool did_move_prefabs = false, did_delete_prefabs = false;
        bool did_open_timeline = false, did_resolve_entity = false, did_save_timeline = false;
        EnterCriticalSection(&g_loop->mtx);
        if (g_iface && g_iface->vtbl && g_iface->vtbl->drain_work_queue) g_iface->vtbl->drain_work_queue(g_iface);
        {
            UiMainEdits edits = { g_pending_save, g_pending_delete, g_pending_select, g_pending_deselect,
                                   g_cam_lock || g_cam_write_once, g_pending_set_names };
            bool user = edits.save || edits.del || edits.select || edits.deselect || edits.names || g_cam_write_once;
            if (edits.names) { g_set_names_applied = 0; g_set_names_failed = (int)g_set_names_eids.size(); }
            if (edits.save) g_save_result = -2;
            if (edits.del) g_entity_delete_status = UI_DELETE_NOT_RUN;
            if ((user || edits.cam) &&
                !ui_on_main(ui_main_edits, &edits, user ? UI_MAIN_WAIT_MS : UI_MAIN_CAM_WAIT_MS) && user)
                ui_log("engine edits: the game did not take them within the wait; dropped");
            g_cam_write_once = false;
        }
        if (g_pending_save)   { g_pending_save = false;   did_save = true; }
        if (g_pending_set_names) { g_pending_set_names = false; did_set_names = true; }
        if (g_pending_delete) { g_delete_eids.clear(); g_pending_delete = false; did_delete = true; }
        if (g_pending_select) {
            /* keep forward-sync (editor->list) quiet about the selection WE just pushed (avoid a ping-pong). */
            g_last_editor_sel = (g_select_eids.size() == 1) ? g_select_eids[0] : -1;
            if (g_select_eids.size() == 1) g_displayed_eid = g_select_eids[0];
            g_select_eids.clear(); g_pending_select = false;
        }
        if (g_pending_deselect) {
            g_last_editor_sel = -1; g_last_sel_sig = 0;
            g_pending_deselect = false;
        }
        if (g_select_refused) { select_refused_reason = g_select_refused; g_select_refused = 0; did_select_refused = true; }
        if (g_pending_create_prefab) {
            ui_apply_create_prefab();
            g_pending_create_prefab = false;
            did_create_prefab = true;
        }
        if (g_pending_delete_prefab) { ui_apply_delete_prefab(); g_pending_delete_prefab = false; did_delete_prefab = true; }
        if (g_pending_rename_prefab) { ui_apply_rename_prefab(); g_pending_rename_prefab = false; did_rename_prefab = true; }
        if (g_pending_create_module) { ui_apply_create_module_prefab(); g_pending_create_module = false; did_create_module = true; }
        if (g_pending_load_prefab)   { ui_apply_load_prefab();   g_pending_load_prefab = false;   did_load_prefab = true; }
        if (g_pending_new_entity)    { ui_apply_new_entity();    g_pending_new_entity = false;    did_new_entity = true; }
        /* A finished file picker. Its thread only carried the dialog; everything that touches the
         * backend happens here, on the UI thread, like every other deferred action above. */
        if (InterlockedCompareExchange(&g_pick_done, 0, 0) != 0) ui_finish_pick();
        /* A finished Mods tab job: post its documents from this thread. */
        if (InterlockedCompareExchange(&g_mods_done, 0, 0) != 0) ui_mods_finish();
        if (g_pending_sound_session) { ui_apply_sound_session(); g_pending_sound_session = false; }
        if (g_pending_sound_preview) { ui_apply_sound_preview(); g_pending_sound_preview = false; did_sound_preview = true; }
        if (g_pending_create_folder) { ui_apply_create_folder(); g_pending_create_folder = false; did_create_folder = true; }
        if (g_pending_rename_folder) { ui_apply_rename_folder(); g_pending_rename_folder = false; did_rename_folder = true; }
        if (g_pending_delete_folder) { ui_apply_delete_folder(); g_pending_delete_folder = false; did_delete_folder = true; }
        if (g_pending_move_prefab)   { ui_apply_move_prefab();   g_pending_move_prefab = false;   did_move_prefab = true; }
        if (g_pending_move_prefabs)  { ui_apply_move_prefabs();  g_pending_move_prefabs = false;  did_move_prefabs = true; }
        if (g_pending_delete_prefabs) { ui_apply_delete_prefabs(); g_pending_delete_prefabs = false; did_delete_prefabs = true; }
        if (g_pending_open_timeline) { g_tl_json_len = ui_serialize_entity_raw(g_open_timeline_eid); g_pending_open_timeline = false; did_open_timeline = true; }
        if (g_pending_resolve_entity) { g_resolve_json_len = ui_serialize_entity_resolve(g_resolve_entity_eid); g_pending_resolve_entity = false; did_resolve_entity = true; }
        if (g_pending_save_timeline) { ui_apply_save_timeline(); g_pending_save_timeline = false; did_save_timeline = true; }
        LeaveCriticalSection(&g_loop->mtx);
        /* The prefabs folder watcher: the Prefabs list follows the disk. */
        ui_watch_tick();
        ui_mods_progress_tick();
        ui_mods_push_tick();

        if (did_save) {
            wchar_t m[80]; _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"saveResult\",\"result\":%d}", g_save_result);
            ui_post_json(m);
            ui_send_list("save");
            if (g_save_eid >= 0) ui_send_state(g_save_eid, false);
        }
        if (did_delete) { ui_post_delete_result(); ui_send_list("delete"); }
        if (did_set_names) {
            wchar_t m[128];
            _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"setDisplayNamesResult\",\"applied\":%d,\"failed\":%d}",
                         g_set_names_applied, g_set_names_failed);
            ui_post_json(m);
            ui_logp("set-display-names: applied=%d failed=%d", g_set_names_applied, g_set_names_failed);
            g_set_names_eids.clear(); g_set_names_names.clear();
            g_last_list_sig = 0;            /* names changed: rebuild the Timelines list */
            ui_send_list("set-names");
            if (g_displayed_eid >= 0) ui_send_state(g_displayed_eid, false);
        }
        if (did_select_refused)
            ui_post_json(select_refused_reason == 2 ? L"{\"kind\":\"selectRefused\",\"reason\":\"native\"}"
                                                     : L"{\"kind\":\"selectRefused\",\"reason\":\"hold\"}");
        if (did_create_prefab) {
            std::wstring m = L"{\"kind\":\"createPrefabResult\",\"result\":"; m += std::to_wstring(g_create_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_create_prefab_name.c_str()); m += L"\"";
            m += L"}";
            ui_post_json(m.c_str());
            if (g_create_result == 1) ui_send_prefabs();
        }
        if (did_delete_prefab) {
            std::wstring m = L"{\"kind\":\"deletePrefabResult\",\"result\":"; m += std::to_wstring(g_delete_result);
            m += L",\"reason\":\""; m += g_delete_reason; m += L"\"";
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_delete_prefab_name.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
            if (g_delete_result == 1) ui_send_prefabs();
        }
        if (did_rename_prefab) {
            std::wstring m = L"{\"kind\":\"renamePrefabResult\",\"result\":"; m += std::to_wstring(g_rename_result);
            m += L",\"reason\":\""; m += g_rename_reason; m += L"\"";
            m += L",\"oldName\":\""; m += sh_webview_json::escape_wide(g_rename_prefab_old.c_str());
            m += L"\",\"newName\":\""; m += sh_webview_json::escape_wide(g_rename_prefab_new.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
            if (g_rename_result == 1) ui_send_prefabs();
        }
        if (did_create_module) {
            std::wstring m = L"{\"kind\":\"createModulePrefabResult\",\"result\":"; m += std::to_wstring(g_create_module_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_create_module_name.c_str()); m += L"\",\"summary\":";
            /* the body the backend generated a moment ago, not bytes read back from disk */
            if (g_create_module_result == 1 && !g_create_module_summary.empty() && g_create_module_summary[0] == '{') {
                std::wstring w(g_create_module_summary.begin(), g_create_module_summary.end());
                m += w;
            } else m += L"{}";
            if (g_create_module_result != 1 && !g_create_module_message.empty()) {
                m += L",\"message\":\""; m += sh_webview_json::escape_wide(g_create_module_message.c_str()); m += L"\"";
            }
            m += L"}";
            ui_post_json(m.c_str());
            if (g_create_module_result == 1) ui_send_prefabs();
        }
        /* a module placement posts when the backend has its outcome (ui_poll_module_place), an
         * object paste once the game thread has queued or staged it (ui_poll_place_outcome) */
        if (did_load_prefab && !(g_load_prefab_modules && g_load_result == 2) && !g_place_wait) {
            std::wstring m = L"{\"kind\":\"loadPrefabResult\",\"result\":"; m += std::to_wstring(g_load_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_load_prefab_name.c_str()); m += L"\"";
            if (g_load_prefab_modules) m += L",\"prefabKind\":\"modules\"";
            if (!g_load_message.empty()) {
                m += L",\"message\":\""; m += sh_webview_json::escape_wide(g_load_message.c_str()); m += L"\"";
            }
            m += L"}";
            ui_post_json(m.c_str());
        }
        ui_poll_module_place();
        ui_poll_place_outcome();
        ui_poll_prefab_mods();
        if (did_sound_preview) {
            std::wstring m = L"{\"kind\":\"soundPreviewResult\",\"playing\":";
            m += (g_sound_preview_result ? L"true" : L"false");
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_sound_preview_name.c_str());
            m += L"\",\"reason\":\""; m += sh_webview_json::escape_wide(g_sound_preview_reason.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
        }
        if (did_new_entity) {
            std::wstring m = L"{\"kind\":\"newEntityResult\",\"result\":"; m += std::to_wstring(g_new_entity_result);
            m += L",\"label\":\""; m += sh_webview_json::escape_wide(g_new_entity_label.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
        }
        if (did_open_timeline) ui_emit_timeline_data(g_open_timeline_eid, g_tl_json_len);
        if (did_resolve_entity) ui_emit_entity_inherit(g_resolve_entity_eid, g_resolve_json_len);
        if (did_save_timeline) {
            std::wstring m = L"{\"kind\":\"saveTimelineResult\",\"eid\":"; m += std::to_wstring(g_save_timeline_eid);
            m += L",\"result\":"; m += std::to_wstring(g_save_timeline_result); m += L"}";
            ui_post_json(m.c_str());
        }
        if (did_create_folder) {
            std::wstring m = L"{\"kind\":\"createFolderResult\",\"result\":"; m += std::to_wstring(g_create_folder_result);
            m += L",\"reason\":\""; m += g_create_folder_reason; m += L"\"";
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_create_folder_name.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
            if (g_create_folder_result == 1) ui_send_prefabs();
        }
        if (did_rename_folder) {
            std::wstring m = L"{\"kind\":\"renameFolderResult\",\"result\":"; m += std::to_wstring(g_rename_folder_result);
            m += L",\"reason\":\""; m += g_rename_folder_reason; m += L"\"";
            m += L",\"oldName\":\""; m += sh_webview_json::escape_wide(g_rename_folder_old.c_str());
            m += L"\",\"newName\":\""; m += sh_webview_json::escape_wide(g_rename_folder_new.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
            if (g_rename_folder_result == 1) ui_send_prefabs();
        }
        if (did_delete_folder) {
            std::wstring m = L"{\"kind\":\"deleteFolderResult\",\"result\":"; m += std::to_wstring(g_delete_folder_result);
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_delete_folder_name.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
            if (g_delete_folder_result == 1) ui_send_prefabs();
        }
        if (did_move_prefab) {
            std::wstring m = L"{\"kind\":\"movePrefabResult\",\"result\":"; m += std::to_wstring(g_move_prefab_result);
            m += L",\"reason\":\""; m += g_move_prefab_reason; m += L"\"";
            m += L",\"name\":\""; m += sh_webview_json::escape_wide(g_move_prefab_name.c_str());
            m += L"\",\"toFolder\":\""; m += sh_webview_json::escape_wide(g_move_prefab_to.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
            if (g_move_prefab_result == 1) ui_send_prefabs();
        }
        if (did_move_prefabs) {
            std::wstring m = L"{\"kind\":\"movePrefabsResult\",\"moved\":[";
            for (size_t i = 0; i < g_move_prefabs_moved.size(); i++) { if (i) m += L","; m += L"\""; m += sh_webview_json::escape_wide(g_move_prefabs_moved[i].c_str()); m += L"\""; }
            m += L"],\"failed\":"; m += std::to_wstring(g_move_prefabs_failed);
            m += L",\"reason\":\""; m += g_move_prefabs_reason; m += L"\"";
            m += L",\"toFolder\":\""; m += sh_webview_json::escape_wide(g_move_prefabs_to.c_str()); m += L"\"}";
            ui_post_json(m.c_str());
            if (!g_move_prefabs_moved.empty()) ui_send_prefabs();
        }
        if (did_delete_prefabs) {
            std::wstring m = L"{\"kind\":\"deletePrefabsResult\",\"deleted\":"; m += std::to_wstring(g_delete_prefabs_deleted);
            m += L",\"failed\":"; m += std::to_wstring(g_delete_prefabs_failed);
            m += L",\"reason\":\""; m += g_delete_prefabs_reason; m += L"\"}";
            ui_post_json(m.c_str());
            if (g_delete_prefabs_deleted) ui_send_prefabs();
        }
        if (g_report_done) {   /* feedback/crash POST finished on its worker thread -- relay the result */
            g_report_done = false;
            std::wstring m = L"{\"kind\":\"reportResult\",\"ok\":"; m += g_report_ok ? L"true" : L"false";
            m += L",\"mode\":\""; m += sh_webview_json::escape_wide(g_report_mode);
            m += L"\",\"number\":"; m += std::to_wstring(g_report_number); m += L"}";
            ui_post_json(m.c_str());
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
                        ui_post_json(wm.c_str());
                        ui_logp("crash: terminal record announced (terminal=%d, survived=%d)",
                                    terminal, survived);
                    }
                } else if (survived > 0 && terminal == 0 && !startup) {
                    /* nonterminal record observed this session: notice only */
                    std::wstring m = L"{\"kind\":\"faultRecovered\",\"count\":";
                    m += std::to_wstring(survived); m += L"}";
                    ui_post_json(m.c_str());
                    ui_logf("crash: %lu recovered fault record(s) -- toast only, no dialog",
                             (unsigned long)survived);
                }
            }
        }

        if (g_webview_ready) {
            bool ready = ui_editor_ready() != 0;
            if (ready && !was_visible) {
                ui_logp("perf lifecycle: editor-visible frame=%u", frame);
                ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);
                ui_send_list("editor-visible");
                /* The DOM survives Play and map reloads. Close any open timeline when the
                 * editor returns so subsequent edits begin with fresh serialized state. */
                ui_post_json(L"{\"kind\":\"editorReopened\"}");
                was_visible = true;
            }
            else if (!ready && was_visible) {
                ui_perf_flush_collect("editor-hidden");
                ui_log("perf lifecycle: editor-hidden");
                /* The editor has let go of its map: the mode poll stops while the window is
                 * hidden, so tell the page now (the Mods tab drops its "In this map" pills) and
                 * make the next visible poll post the real state again. */
                ui_post_json(L"{\"kind\":\"editorMode\",\"entityMode\":0,\"blueprint\":0,\"modules\":0,\"mapOpen\":0}");
                g_editor_mode_last = -1;
                g_editor_modules_last = -2;
                ShowWindow(g_hwnd, SW_HIDE); was_visible = false;
            }

            /* Sample camera coordinates at UI cadence; change gating avoids idle messages. */
            if (was_visible && !g_cam_lock) ui_cam_read_send();

            /* periodic auto tasks (~ every 10 frames = ~330 ms): list change poll, editor-selection sync,
             * displayed-state change poll. All emit only on an actual change. */
            if (was_visible && (frame % 10 == 0)) {
                unsigned long long poll_started = ui_perf_now_us(), collect_us = 0;
                int rdy = 0; int n = ui_collect_timed(&rdy, "poll", &collect_us);
                uint64_t sig = ui_list_sig(n);
                if (sig != g_last_list_sig) {
                    ui_rescan_timelines_timed(n, "entity-change");   /* change-gated, not a fixed timer */
                    g_last_list_sig = sig;
                    ui_emit_list(n, rdy, "entity-change");
                }

                /* Publish selection count regardless of sync direction for prefab creation. */
                unsigned long long selection_started = ui_perf_now_us();
                std::vector<int> selids;
                int sn = ui_get_selection(selids);
                if (sn != g_last_selcount) {
                    g_last_selcount = sn;
                    wchar_t m[64]; _snwprintf_s(m, _countof(m), _TRUNCATE, L"{\"kind\":\"selCount\",\"count\":%d}", sn);
                    ui_post_json(m);
                }
                /* Object Mode / Blueprint, for the Prefabs tab's Load and Create gates */
                ui_post_editor_mode(false);
                /* The Limits section's table, after a palette rebuild, and its live usage. */
                ui_budget_poll();
                ui_budget_usage_poll();
                if (g_sync_on) {
                    /* mirror the WHOLE editor selection (any N) into the list, only when it changes. */
                    std::sort(selids.begin(), selids.end());
                    uint64_t sig = 1469598103934665603ull;
                    for (int a = 0; a < sn; a++) sig = hint(sig, selids[a]);
                    sig ^= (uint64_t)sn;
                    if (sig != g_last_sel_sig) {
                        g_last_sel_sig = sig;
                        std::wstring m = L"{\"kind\":\"editorSelect\",\"eids\":[";
                        for (int a = 0; a < sn; a++) { if (a) m += L","; m += std::to_wstring(selids[a]); }
                        m += L"]}";
                        ui_post_json(m.c_str());
                        if (sn == 1) g_displayed_eid = selids[0];
                    }
                }
                unsigned long long selection_finished = ui_perf_now_us();
                bool state_called = g_displayed_eid >= 0;
                unsigned long long state_started = selection_finished;
                if (state_called) ui_send_state(g_displayed_eid, true);
                unsigned long long state_finished = ui_perf_now_us();
                unsigned long long poll_finished = ui_perf_now_us();
                ui_perf_note_poll(poll_finished >= poll_started ? poll_finished - poll_started : 0,
                                   collect_us,
                                   selection_finished >= selection_started ? selection_finished - selection_started : 0,
                                   state_finished >= state_started ? state_finished - state_started : 0,
                                   state_called, n, sn);
            }
        }

        if (g_webview_ready) ui_send_prefab_mesh_completion();

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
    /* The window and the backend are built together: their interface tables must be the
     * same size, or every slot past the shorter one is unknown, and a backend that names its
     * release must name this window's. A mismatched pair runs the window only to say so. */
    std::string backend_version = args ? sh_game_identity::backend_version(args->argc, args->argv) : std::string();
    g_pair_mismatch = g_iface && (sh_iface_vtbl_bytes(g_iface) != sizeof(sh_iface_vtbl) ||
                                  (!backend_version.empty() && backend_version != g_version));
    g_game = args ? sh_game_identity::from_argv(args->argc, args->argv) : std::string();

    ui_log("=== sh_ui_init: the Snapmap+ window is starting ===");
    ui_log(g_iface ? "interface handed over: yes" : "interface handed over: NO (null)");
    if (g_pair_mismatch) {
        ui_logp("install mismatch: the backend is %s with a %u-byte interface, this window "
                    "%s with %u; showing the reinstall notice only", backend_version.empty() ? "(unnamed)" : backend_version.c_str(),
                    (unsigned)sh_iface_vtbl_bytes(g_iface), g_version.c_str(), (unsigned)sizeof(sh_iface_vtbl));
    }
    { ui_logp("window version: %s", g_version.c_str()); }
    { ui_logp("game: %s", g_game.empty() ? "not attached" : g_game.c_str()); }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int n = MultiByteToWideChar(CP_UTF8, 0, kMockupHtml, -1, nullptr, 0);
    g_html.resize(n > 0 ? n - 1 : 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, kMockupHtml, -1, &g_html[0], n);
    {
        std::string theme_json;
        int got = ui_config_get_json("theme", theme_json,
                                      &g_config_status_flags);
        const char *seed_value = got >= 0 ? theme_json.c_str() : "\"light\"";
        if (got < 0)
            ui_log("config: theme unavailable; using light for this session");
        if (!sh_theme_seed_html(g_html, seed_value))
            ui_log("config: embedded HTML theme marker missing or duplicated");
    }

    ui_create_window();
    ui_start_webview();
    ui_think_loop();
    return 0;
}
