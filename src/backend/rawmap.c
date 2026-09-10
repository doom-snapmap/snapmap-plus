/* DeserializeFromJson detour: optionally substitute a file-backed rawmap,
 * prepare the chosen JSON, then call the native parser. The temporary buffer
 * lives through the call. An unavailable source falls back to engine JSON.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "rawmap.h"
#include "hook.h"
#include "backend_log.h"
#include "map_package.h"
#include "signatures.h"
#include "config.h"
#include "overrides.h"
#include "map_embed.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "nav_regions.h"
#include "map_shards.h"
#include "editor_frame.h"   /* sh_editor_frame_request_reload -- the +0x338 load-now slot */
#include "raw_deflate.h"    /* sh_inflate_raw_upto -- map.decl holds zlib(rawmap JSON) */

/* DeserializeFromJson prologue steal window. Decoded from the signature DB pattern
 *   40 55            push rbp                      (2)
 *   56               push rsi                      (1)
 *   57               push rdi                      (1)
 *   48 8D 6C 24 90   lea  rbp,[rsp-0x70]           (5)  -- rsp-relative, position-independent
 *   48 81 EC 70 01.. sub  rsp,0x170                (7)
 * = 16 bytes of whole, register/rsp-only, position-independent instructions (no RIP-rel, no rel
 * jmp/call). Same 16-byte window the smoke self-test exercises. */
#define DESER_STOLEN 16

/* The engine target's prototype: int DeserializeFromJson(const char* json, idSnapMap* out). Variant A
 * (buffer-first). */
typedef int (*deser_fn_t)(const char *json, void *out_map);

static deser_fn_t g_deser_orig = NULL;   /* the trampoline -> the real engine DeserializeFromJson */

/* Explicit load/save arm; default off. */
static volatile LONG g_gate = 0;
static volatile LONG g_swap_count = 0;
static volatile LONG g_swap_complete_count = 0;

/* Load source override; otherwise %LOCALAPPDATA%/snapmap-plus/rawmap.json. */
static char g_src_path[MAX_PATH] = {0};

static void default_source_path(char *out, size_t cap)
{
    /* Use the shared application data root. */
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\rawmap.json", base);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snapmap-plus\\rawmap.json");
}

/* Resolve the effective source into a MAX_PATH buffer. arm.flag derives from
 * the same path.
 */
static void resolve_source_path(char *out, size_t cap)
{
    if (g_src_path[0]) strncpy_s(out, cap, g_src_path, _TRUNCATE);
    else default_source_path(out, cap);
}

/* Test flag beside the configured source. A source without a directory uses
 * arm.flag relative to the working directory.
 */
static void flag_file_path(char *out, size_t cap)
{
    char src[MAX_PATH];
    resolve_source_path(src, sizeof src);

    /* find the last path separator (back- or forward-slash) to split off the directory. */
    char *sep = NULL, *p;
    for (p = src; *p; ++p) {
        if (*p == '\\' || *p == '/') sep = p;
    }
    if (sep) {
        size_t dirlen = (size_t)(sep - src) + 1;   /* include the separator */
        if (dirlen >= cap) dirlen = cap - 1;
        memcpy(out, src, dirlen);
        out[dirlen] = '\0';
        strncat_s(out, cap, "arm.flag", _TRUNCATE);
    } else {
        strncpy_s(out, cap, "arm.flag", _TRUNCATE);
    }
}

/* Test-only file trigger, additive to the explicit arm state. */
static int flag_file_present(void)
{
    char flag[MAX_PATH];
    flag_file_path(flag, sizeof flag);
    DWORD attrs = GetFileAttributesA(flag);
    return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Shared load/save predicate: explicit arm OR arm.flag. flag_armed_out
 * optionally identifies the file trigger for logging.
 */
static int rawmap_armed(int *flag_armed_out)
{
    int explicit_armed = (InterlockedCompareExchange(&g_gate, 0, 0) != 0);
    int flag_armed     = flag_file_present();
    if (flag_armed_out) *flag_armed_out = flag_armed;
    return explicit_armed || flag_armed;
}

/* One-shot save arm: "Save Rawmap As" authorizes exactly one save, then disarms
 * itself. Additive to the shared gate, so sh_rawmaps_on keeps its meaning. */
static volatile LONG g_shadow_oneshot = 0;

int sh_rawmap_save_arm_once(void)
{
    InterlockedExchange(&g_shadow_oneshot, 1);
    return 1;
}

int sh_rawmap_save_oneshot_pending(void)
{
    return (InterlockedCompareExchange(&g_shadow_oneshot, 0, 0) != 0) ? 1 : 0;
}

/* The load-side counterpart, so opening one rawmap does not substitute every map
 * opened after it. Consumed only on a real substitution: the detour clears it
 * once the source reads, not when it decides to look. */
static volatile LONG g_swap_oneshot = 0;

int sh_rawmap_load_arm_once(void)
{
    InterlockedExchange(&g_swap_oneshot, 1);
    return 1;
}

int sh_rawmap_load_oneshot_pending(void)
{
    return (InterlockedCompareExchange(&g_swap_oneshot, 0, 0) != 0) ? 1 : 0;
}

/* Read a NUL-terminated process-heap buffer; caller frees it. Return NULL on
 * failure.
 */
static char *read_source_file(size_t *out_len)
{
    char buf_path[MAX_PATH];
    resolve_source_path(buf_path, sizeof buf_path);
    const char *path = buf_path;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)(64 * 1024 * 1024)) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);   /* Reserve the trailing NUL. */
    if (!buf) { CloseHandle(h); return NULL; }

    size_t got = 0;
    while (got < n) {
        DWORD chunk = (DWORD)((n - got) > 0x10000000 ? 0x10000000 : (n - got));
        DWORD rd = 0;
        if (!ReadFile(h, buf + got, chunk, &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    if (got != n) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

/* Gate every normal or substituted map before native parsing. Missing
 * declared content can fault during spawn/render, so return 0 to refuse it.
 * Exceptions retain the existing pass-through fallback.
 */
static int mpkg_gate_guarded(const char *json)
{
    __try {
        size_t len = json ? strlen(json) : 0;
        if (len == 0) return 1;
        return sh_mpkg_gate(json, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;   /* gate fault -> vanilla load */
    }
}

/* Strip package delivery variables. Return an owned process-heap buffer, or
 * NULL to retain the original on no change or failure.
 */
static char *mpkg_strip_guarded(const char *json)
{
    __try {
        size_t len = json ? strlen(json) : 0;
        size_t out_len = 0;
        if (len == 0) return NULL;
        return sh_mpkg_strip(json, len, &out_len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* Prepare the chosen JSON before native parsing: migrate markers, rebuild
 * map-scoped navigation state, then strip package and navigation envelopes.
 * Each failed strip retains the preceding buffer. Returns an owned process-
 * heap buffer or NULL to use the original.
 */
static char *prepare_map_buffer(const char *json)
{
    char *pkg, *nav, *migrated;
    size_t len = json ? strlen(json) : 0;
    size_t nav_len = 0;

    if (len == 0) return NULL;

    /* Convert the old bake marker before both the bake and native entity parse.
     * This runs for normal loads as well as explicit rawmap substitution. */
    migrated = sh_nav_regions_migrate(json, len, &len);
    if (migrated) {
        json = migrated;
        backend_log("NAV: migrated Blocking Box navigation markers to flags.noFlood");
    }

    sh_navmesh_build_from_map(json, len);
    /* The regions the author marked, read from the same bytes and cleared the
     * same way. A map with no marked volume must not inherit the last map's. */
    sh_nav_bake_set_map(json, len);

    pkg = mpkg_strip_guarded(json);
    nav = sh_navmesh_strip(pkg ? pkg : json, pkg ? strlen(pkg) : len, &nav_len);
    if (!nav && !pkg) return migrated;
    if (migrated) HeapFree(GetProcessHeap(), 0, migrated);
    if (!nav) return pkg;
    if (pkg) HeapFree(GetProcessHeap(), 0, pkg);
    return nav;
}

/* Use a readable armed source or the engine JSON. Both pass the package gate;
 * refusal returns the native parse-failure value, 0.
 */
static int sh_deser_detour(const char *json, void *out_map)
{
    if (g_deser_orig == NULL) return 0;   /* defensive: should never happen once installed */

    /* The shared predicate includes the explicit switch and test flag. */
    int flag_armed = 0;
    int oneshot    = sh_rawmap_load_oneshot_pending();
    if (rawmap_armed(&flag_armed) || oneshot) {
        size_t len = 0;
        char *ours = read_source_file(&len);
        if (ours != NULL) {
            char line[160];
            unsigned long n;
            /* Spend the arm here, where a substitution is certain -- see the note on the one-shot. */
            if (oneshot) InterlockedExchange(&g_swap_oneshot, 0);
            n = (unsigned long)InterlockedIncrement(&g_swap_count);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: rawmap swap FIRED (orig %s bytes -> ours %zu bytes) [#%lu]%s%s",
                json ? "<engine-json>" : "<null>", len, n,
                flag_armed ? " [flag-armed]" : "",
                oneshot ? " [one-shot]" : "");
            backend_log(line);
            if (!mpkg_gate_guarded(ours)) {
                backend_log("B1: rawmap swap load REFUSED by the map-package gate");
                HeapFree(GetProcessHeap(), 0, ours);
                return 0;   /* refused: the engine never sees the bytes */
            }
            {
                char *prepared = prepare_map_buffer(ours);
                int rc = g_deser_orig(prepared ? prepared : ours, out_map);
                InterlockedIncrement(&g_swap_complete_count); /* only after the substituted parse returns */
                if (prepared) HeapFree(GetProcessHeap(), 0, prepared);
                HeapFree(GetProcessHeap(), 0, ours);
                return rc;
            }
        }
        /* Unreadable substitute: use the engine input. */
    }
    if (!mpkg_gate_guarded(json)) return 0;   /* refused: engine json never parsed */
    {
        char *prepared = prepare_map_buffer(json);
        int rc;
        if (!prepared) return g_deser_orig(json, out_map);
        rc = g_deser_orig(prepared, out_map);
        HeapFree(GetProcessHeap(), 0, prepared);
        return rc;
    }
}

int sh_rawmap_swap_install(void *deser_fn, int deser_status_ok)
{
    char line[200];

    if (deser_fn == NULL) {
        backend_log("B1: rawmap LOAD-swap SKIPPED -- DeserializeFromJson not resolved");
        return 0;
    }
    if (!deser_status_ok) {
        /* Refuse an already-hooked prologue; its detour bytes cannot be
         * stolen as native instructions.
         */
        backend_log("B1: rawmap LOAD-swap SKIPPED -- DeserializeFromJson resolved via hook-tolerant "
                    "fallback (prologue already hooked); not installing over an existing detour");
        return 0;
    }
    if (g_deser_orig != NULL) {
        backend_log("B1: rawmap LOAD-swap already installed");
        return 1;
    }

    void *tramp = install_inline_hook(deser_fn, (void *)sh_deser_detour, DESER_STOLEN);
    if (tramp == NULL) {
        backend_log("B1: rawmap LOAD-swap FAIL -- install_inline_hook returned NULL");
        return 0;
    }
    g_deser_orig = (deser_fn_t)tramp;

    if (!g_src_path[0]) default_source_path(g_src_path, sizeof g_src_path);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: rawmap LOAD-swap installed at %p (trampoline %p, stolen %d); source=%s; gate=DISARMED",
        deser_fn, tramp, DESER_STOLEN, g_src_path);
    backend_log(line);
    /* Log the exact TEST arm flag-file path (create it to arm the swap, delete to disarm). */
    {
        char flag[MAX_PATH];
        flag_file_path(flag, sizeof flag);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B1: rawmap LOAD-swap TEST arm flag-file = %s (create to arm, delete to disarm)", flag);
        backend_log(line);
    }
    return 1;
}

int sh_rawmap_swap_arm(int on)
{
    InterlockedExchange(&g_gate, on ? 1 : 0);
    backend_log(on ? "B1: rawmap LOAD-swap ARMED" : "B1: rawmap LOAD-swap DISARMED");
    return on ? 1 : 0;
}

/* Expose the explicit control state by name. The test flag is excluded
 * because turning the control off does not remove it.
 */
int sh_rawmap_swap_is_armed(void)
{
    return (InterlockedCompareExchange(&g_gate, 0, 0) != 0) ? 1 : 0;
}

/* Expose the effective arm predicate for engine-direct callers. Calling the
 * codec while substitution is enabled can parse the wrong buffer.
 */
int sh_rawmap_swap_will_fire(void)
{
    return rawmap_armed(NULL) ? 1 : 0;
}

int sh_rawmap_swap_set_source(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        default_source_path(g_src_path, sizeof g_src_path);
        return 1;
    }
    strncpy_s(g_src_path, sizeof g_src_path, path, _TRUNCATE);
    return g_src_path[0] != '\0';
}

unsigned long sh_rawmap_swap_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_swap_count, 0, 0);
}

unsigned long sh_rawmap_swap_complete_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_swap_complete_count, 0, 0);
}

/* Save processing. */

/* Call native SerializeToJson and preserve its bool result.
 * Package/navigation embedding may replace the output JSON before the caller
 * consumes it. Armed mirroring writes a separate disk copy; pretty formatting
 * affects that copy only.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "rawmap.h"
#include "hook.h"
#include "backend_log.h"
#include "cvars.h"        /* sh_cvar_value_int / B2_CVAR_SH_PRETTY_ON -- the shadow's pretty switch */
#include "json_pretty.h"  /* json_pretty -- the pure whitespace re-layout that switch selects */

/* SerializeToJson prologue steal window. Decoded from the live engine prologue (DOOM RVA 0x5F2390,
 * ratified 2026-06-20 against the unpacked exe):
 *   40 53            push rbx                      (2)
 *   56               push rsi                      (1)
 *   57               push rdi                      (1)
 *   48 81 EC E0 00.. sub  rsp,0xe0                 (7)
 *   48 C7 44 24 70.. mov  qword ptr [rsp+0x70],-2  (9)
 * = 20 bytes of whole, register/rsp-only, position-independent instructions (no RIP-rel, no rel
 * jmp/call). The NEXT instruction (+0x14 `MOV RAX,[rip-rel]`) IS RIP-relative, so the window stops at 20.
 * These 20 bytes are exactly the SerializeToJson signature's fixed prefix in the sig DB. */
#define SAVE_STOLEN 20

/* Native idStr length and data-pointer offsets. */
#define IDSTR_LEN_OFF   0x08   /* int  len  (character count, excl NUL) */
#define IDSTR_DATA_OFF  0x10   /* char* data (inline baseBuffer for short strings, else heap) */

/* SerializeToJson returns bool in AL, with output idStr in RDX. The pinned
 * save-snapshot caller at 0x59D2F0+0x54 consumes AL with MOVZX EBX,AL.
 * Capture and return that byte on every path; declaring this hook void lets
 * later calls overwrite the save verdict.
 */
typedef unsigned char (*serialize_fn_t)(void *map, void *out_idstr, unsigned char compact);

static serialize_fn_t g_ser_orig = NULL;   /* the trampoline -> the real engine SerializeToJson */
static __declspec(thread) int g_snapshot_depth;

int sh_rawmap_snapshot(void *editor_serializer, void *map, void *out_idstr)
{
    int ok=0;
    if (!g_ser_orig || !editor_serializer || !map || !out_idstr) return 0;
    g_snapshot_depth++;
    __try { ok=((serialize_fn_t)editor_serializer)(map,out_idstr,0)!=0; }
    __finally { g_snapshot_depth--; }
    return ok;
}

static volatile LONG     g_shadow_count = 0;
static volatile LONGLONG g_last_bytes   = 0;

/* A destination for exactly one write, then gone. Empty means no override is in
 * force: a save then follows the save-path setting, or the default. */
static char g_dest_once[MAX_PATH] = {0};

static void default_dest_path(char *out, size_t cap)
{
    /* Use the shared application data root. */
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\rawmap.json", base);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snapmap-plus\\rawmap.json");
}

/* THE SAVE PATH SETTING -- `sh_rawmaps savepath <rawmap|default|path>`, and the File menu's
 * "Use Rawmap as Save Path" tick.
 *
 * One durable setting with three values, and it is the ONLY durable way the save destination moves:
 *
 *   DEFAULT  saves go to %LOCALAPPDATA%\snapmap-plus\rawmap.json
 *   RAWMAP   saves go back over whichever rawmap is currently loaded
 *   FIXED    saves go to one named file, always
 *
 * DEFAULT is the default, and that is the whole design: a rawmap you LOAD is an archive entry, not a
 * scratch file, so nothing you do to import one can end up writing over it. Exporting somewhere with
 * "Save Rawmap As" or `sh_rawmaps save <path>` does NOT change this setting -- that is a one-off
 * write (g_dest_once) which is spent as soon as the bytes land. An export that silently became the
 * new home for every later save is the bug this shape exists to make impossible.
 *
 * RAWMAP is held as a MODE consulted at resolve time, never as a path copied in when a rawmap is
 * loaded. A copy would go stale the moment a different rawmap was staged, and switching back to
 * DEFAULT would leave the archive file still wired in -- an opt-out that does not opt out. */
/* Defined further down, next to the status JSON that is its other caller. Needed here to
 * write the save-path setting out as a JSON string. */
static int json_escape_into(char *out, size_t cap, const char *src);

typedef enum {
    RAWMAP_DEST_DEFAULT = 0,
    RAWMAP_DEST_RAWMAP  = 1,
    RAWMAP_DEST_FIXED   = 2
} rawmap_dest_mode;

static rawmap_dest_mode g_dest_mode  = RAWMAP_DEST_DEFAULT;
static char             g_dest_fixed[MAX_PATH] = {0};

/* Persisted, like the theme, so it survives a restart. ONE string key carries all three values --
 * "" for DEFAULT, "rawmap" for RAWMAP, and any other value is the FIXED path -- because two keys
 * (a bool plus a path) can disagree, and a setting that can contradict itself will. */
#define SAVEPATH_CONFIG_KEY "rawmap.save_path"
#define SAVEPATH_RAWMAP_WORD "rawmap"

int sh_rawmap_dest_follows_source(void)
{
    return (g_dest_mode == RAWMAP_DEST_RAWMAP) ? 1 : 0;
}

int sh_rawmap_dest_mode(char *out_fixed, int fixed_cap)
{
    if (out_fixed && fixed_cap > 0) {
        out_fixed[0] = '\0';
        if (g_dest_mode == RAWMAP_DEST_FIXED)
            strncpy_s(out_fixed, (size_t)fixed_cap, g_dest_fixed, _TRUNCATE);
    }
    return (int)g_dest_mode;
}

/* Write the setting out. A failure is not worth refusing the change over -- it still applies for
 * this session, the same degradation every other setting takes when the profile is unwritable. */
static void save_dest_setting(void)
{
    char json[MAX_PATH * 2];
    const char *value = "";
    char esc[MAX_PATH * 2];

    if (g_dest_mode == RAWMAP_DEST_RAWMAP) value = SAVEPATH_RAWMAP_WORD;
    else if (g_dest_mode == RAWMAP_DEST_FIXED) value = g_dest_fixed;

    if (!json_escape_into(esc, sizeof esc, value)) esc[0] = '\0';
    _snprintf_s(json, sizeof json, _TRUNCATE, "\"%s\"", esc);
    if (!sh_config_set_json(SAVEPATH_CONFIG_KEY, json))
        backend_log("B1: rawmap save path changed, but the setting could not be saved");
}

/* Read the persisted setting. Called once from dllmain AFTER sh_config_init, not lazily from the
 * resolver: the resolver runs inside the save detour, and "when is this first read" should not have
 * an answer that depends on which map someone opened. */
void sh_rawmap_config_load(void)
{
    char value[MAX_PATH] = "";
    char line[MAX_PATH + 96];

    if (sh_config_get_string(SAVEPATH_CONFIG_KEY, value, (int)sizeof value) <= 0) return;

    if (value[0] == '\0') {
        g_dest_mode = RAWMAP_DEST_DEFAULT;
    } else if (_stricmp(value, SAVEPATH_RAWMAP_WORD) == 0) {
        g_dest_mode = RAWMAP_DEST_RAWMAP;
        backend_log("B1: rawmap saves follow the loaded rawmap (from settings)");
    } else {
        g_dest_mode = RAWMAP_DEST_FIXED;
        strncpy_s(g_dest_fixed, sizeof g_dest_fixed, value, _TRUNCATE);
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: rawmap saves go to %s (from settings)", g_dest_fixed);
        backend_log(line);
    }
}

int sh_rawmap_set_dest_follows_source(int on)
{
    g_dest_mode = on ? RAWMAP_DEST_RAWMAP : RAWMAP_DEST_DEFAULT;
    if (!on) g_dest_fixed[0] = '\0';
    backend_log(on ? "B1: rawmap saves now FOLLOW the loaded rawmap"
                   : "B1: rawmap saves back to the default rawmap.json");
    save_dest_setting();
    return sh_rawmap_dest_follows_source();
}

/* `sh_rawmaps savepath <path>` -- pin the save destination to one file, durably. Distinct from
 * `sh_rawmaps save <path>`, which exports there once and reverts: this is the "always save here"
 * setting, and saying so takes a different verb precisely so a one-off export cannot become one. */
/* Cut `path` down to its folder part. Returns 0 when it has none. */
static int dest_dir_part(const char *path, char *out, size_t cap)
{
    const char *cut = strrchr(path, '\\');
    const char *fwd = strrchr(path, '/');
    size_t n;

    if (fwd && (!cut || fwd > cut)) cut = fwd;
    if (cut == NULL) return 0;

    n = (size_t)(cut - path);
    if (n == 0) n = 1;                       /* "\file.json" -- the root of the current drive */
    if (n + 2 >= cap) return 0;
    memcpy(out, path, n);
    out[n] = '\0';
    if (n == 2 && out[1] == ':') { out[2] = '\\'; out[3] = '\0'; }   /* "D:" -> "D:\" */
    return 1;
}

/* Create every folder in `dir` that is missing. Walks left to right so the parents come first,
 * because CreateDirectory makes one level at a time. Returns 1 when the folder exists afterwards. */
static int ensure_dir(const char *dir)
{
    char work[MAX_PATH];
    size_t i, n;
    DWORD attr;

    attr = GetFileAttributesA(dir);
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

    strncpy_s(work, sizeof work, dir, _TRUNCATE);
    n = strlen(work);
    if (n == 0) return 0;

    /* Start past the root so "D:\" and "\\server\share" are never handed to CreateDirectory. */
    i = (n >= 2 && work[1] == ':') ? 3 : ((work[0] == '\\' && work[1] == '\\') ? 2 : 1);

    for (; i <= n; ++i) {
        char c = work[i];
        if (c != '\\' && c != '/' && c != '\0') continue;
        work[i] = '\0';
        if (work[0]) {
            attr = GetFileAttributesA(work);
            if (attr == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryA(work, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    return 0;
                }
            } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return 0;                    /* a FILE is sitting where we need a folder */
            }
        }
        work[i] = c;
    }
    attr = GetFileAttributesA(dir);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

/* IS THIS A PLACE WE COULD WRITE A FILE?
 *
 * A save destination cannot be checked the way a load source is: neither the file NOR its folder is
 * supposed to exist yet. A missing folder is fine -- the save creates it, so `savepath
 * D:\archive\2026\mine.json` works on a machine that has never had that folder. What is checked is
 * that the thing is a PATH at all.
 *
 * That is the mistake worth catching. `savepath banana` used to be accepted and pinned, because
 * anything that was not one of the two keywords was taken as a path: every save after it went to a
 * file called "banana" in whatever directory DOOM was started from, and nothing said so. Someone
 * typing a bare word is naming a setting they misremembered, not a file.
 *
 * So: it must have a folder part and a file part. The drive, if one is named, must exist -- a save
 * cannot conjure a Z: drive, and finding that out at save time is finding out too late.
 * Deliberately unchecked: the extension. Where someone keeps their own maps is theirs to decide. */
static int dest_folder_is_usable(const char *path, char *out_msg, int msg_capacity)
{
    char dir[MAX_PATH];

    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    if (path == NULL || path[0] == '\0') return 0;

    if (!dest_dir_part(path, dir, sizeof dir)) {
        if (out_msg && msg_capacity > 0)
            strncpy_s(out_msg, (size_t)msg_capacity,
                      "that is a name, not a path. Give the whole path, "
                      "like D:\\rawmaps\\mine.json", _TRUNCATE);
        return 0;
    }
    {
        const char *last = path + strlen(path) - 1;
        if (*last == '\\' || *last == '/') {
            if (out_msg && msg_capacity > 0)
                strncpy_s(out_msg, (size_t)msg_capacity,
                          "that is a folder. Put a file name on the end", _TRUNCATE);
            return 0;
        }
    }
    if (dir[1] == ':') {
        char root[4]; root[0] = dir[0]; root[1] = ':'; root[2] = '\\'; root[3] = '\0';
        if (GetDriveTypeA(root) <= DRIVE_NO_ROOT_DIR) {
            if (out_msg && msg_capacity > 0)
                _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE,
                            "there is no %s drive on this machine", root);
            return 0;
        }
    }
    return 1;
}

int sh_rawmap_dest_path_is_usable(const char *path, char *out_msg, int msg_capacity)
{
    return dest_folder_is_usable(path, out_msg, msg_capacity);
}

/* CAN WE WRITE THERE RIGHT NOW? Stricter than dest_folder_is_usable, and asked at a different
 * moment: that one vets a SETTING, where the file is not expected to exist and its permissions may
 * change before the next save. This one is asked at the save itself.
 *
 * The gap it closes: a read-only destination was accepted and the console said "Writing the open map
 * to <path>". The write is queued onto a later editor frame, so by the time it failed the command had
 * already reported success and only the log disagreed. Since the writer renames a temp file over the
 * target, a read-only target fails at the rename -- late, silently, and after the console had spoken.
 *
 * Only the attribute is checked, so this is not a guarantee: a full disk, a revoked share or a lock
 * held by another program still fail at the write. It catches the case someone can actually cause by
 * hand, and it never opens the target -- opening it to test would truncate the file it is protecting. */
int sh_rawmap_dest_writable_now(const char *path, char *out_msg, int msg_capacity)
{
    DWORD attr;

    if (!dest_folder_is_usable(path, out_msg, msg_capacity)) return 0;

    attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 1;      /* not there yet: the save creates it */

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (out_msg && msg_capacity > 0)
            strncpy_s(out_msg, (size_t)msg_capacity,
                      "a folder already has that name", _TRUNCATE);
        return 0;
    }
    if (attr & FILE_ATTRIBUTE_READONLY) {
        if (out_msg && msg_capacity > 0)
            strncpy_s(out_msg, (size_t)msg_capacity,
                      "that file is read-only. Untick Read-only in its Properties, "
                      "or save somewhere else", _TRUNCATE);
        return 0;
    }
    return 1;
}

int sh_rawmap_set_dest_fixed(const char *path)
{
    char line[MAX_PATH + 64];

    if (path == NULL || path[0] == '\0') return sh_rawmap_set_dest_follows_source(0) == 0;
    if (!dest_folder_is_usable(path, NULL, 0)) return 0;

    strncpy_s(g_dest_fixed, sizeof g_dest_fixed, path, _TRUNCATE);
    if (g_dest_fixed[0] == '\0') return 0;
    g_dest_mode = RAWMAP_DEST_FIXED;
    _snprintf_s(line, sizeof line, _TRUNCATE, "B1: rawmap saves now go to %s", g_dest_fixed);
    backend_log(line);
    save_dest_setting();
    return 1;
}

/* Forward: the two rules below are stated next to the flag they depend on, above the resolver they
 * need. Declared here so the resolver can stay where it is. */
static void resolve_dest_path(char *out, size_t cap);

/* WHERE THE NEXT SAVE GOES. Four answers, in this order:
 *
 *   1. a one-off destination someone named for this write ("Save Rawmap As", `save <path>`);
 *   2. the loaded rawmap        -- savepath mode RAWMAP;
 *   3. one pinned file          -- savepath mode FIXED;
 *   4. the default rawmap.json  -- savepath mode DEFAULT.
 *
 * The one-off is first because it is the most specific thing anyone said, and it lives only until
 * the write happens. Everything below it is the ONE durable setting, so "where do my saves go" is
 * answerable from a single setting rather than from a history of exports. */
static void resolve_dest_path(char *out, size_t cap)
{
    if (g_dest_once[0]) { strncpy_s(out, cap, g_dest_once, _TRUNCATE); return; }
    if (g_dest_mode == RAWMAP_DEST_RAWMAP) { resolve_source_path(out, cap); return; }
    if (g_dest_mode == RAWMAP_DEST_FIXED && g_dest_fixed[0]) {
        strncpy_s(out, cap, g_dest_fixed, _TRUNCATE);
        return;
    }
    default_dest_path(out, cap);
}

/* Called by the writer once bytes are actually on disk. An export that never happened keeps its
 * destination, so a failed write can be retried at the place it was aimed at. */
static void spend_dest_once(void)
{
    if (g_dest_once[0] == '\0') return;
    g_dest_once[0] = '\0';
    backend_log("B1: rawmap SAVE destination spent -- back to the default (or the loaded rawmap "
                "if 'Use Rawmap as Save Path' is on)");
}

/* ---- the destination rules, stated ONCE ------------------------------------------------------
 *
 * Both the File menu (slot_rawmap_configure) and the console (`sh_rawmaps`) can name a destination,
 * and the first version of this feature spelled the rules out separately in each. They immediately
 * disagreed: the console's `save <path>` moved the destination without regard for the follow toggle,
 * so with the toggle on the write went somewhere other than the file just named. Two copies of a
 * rule is one copy too many, so they live here and both callers call in. */

/* Name a destination for ONE write ("Save Rawmap As", `sh_rawmaps save <path>`).
 *
 * It overrides everything for that write and is spent by the writer, so it cannot outlive the export
 * that asked for it. It deliberately does NOT touch the follow toggle: a one-off export somewhere
 * else is not a statement about where saves belong from now on, and unticking a setting as a side
 * effect of a single export is exactly the kind of quiet state change this rewrite removed.
 *
 * "" or NULL cancels a pending one-off and, because "back to the default" has to mean the default,
 * also clears the toggle. */
int sh_rawmap_choose_dest(const char *path)
{
    return sh_rawmap_save_set_dest((path && path[0]) ? path : NULL);
}

/* Staging a rawmap to LOAD cancels any one-off destination still standing.
 *
 * Nearly nothing left to do now that destinations are one-off by construction -- the accident this
 * was written for (an old export's destination catching a newly imported map) cannot happen any
 * more. What it still covers is an export that was named and then never written: `save <path>`
 * refused for want of a live map, say. That aim should not attach itself to whatever rawmap is
 * staged next. The toggle is untouched: surviving a load is its whole job. */
void sh_rawmap_reset_dest_for_new_load(void)
{
    if (g_dest_once[0]) {
        g_dest_once[0] = '\0';
        backend_log("B1: rawmap SAVE one-off destination dropped -- a new rawmap was staged");
    }
}

/* Write `len` bytes from `data` to the shadow destination ("wb", truncate). Returns the byte count
 * written, or 0 on any failure. SEH-free here (pure Win32 file ops on a validated buffer); the engine
 * out-idStr read in the detour is SEH-guarded by the caller. */
/* Defined below, next to the shadow that shares it: one writer for both the shadow and the
 * live path, so the destination is resolved in exactly one place. */
static unsigned long long write_shadow(const char *data, size_t len);

/* Is `path` the very file currently staged for LOADING? Used by the save ladder's disk fallback,
 * which is the only rung whose bytes might not be the open map's. */
static int dest_is_the_staged_source(const char *path)
{
    char src_now[MAX_PATH] = "";
    if (path == NULL || path[0] == '\0') return 0;
    resolve_source_path(src_now, sizeof src_now);
    return (src_now[0] != '\0' && _stricmp(src_now, path) == 0) ? 1 : 0;
}

/* ------------------------------------------------------- serialize the LIVE map ----------------
 * Save Rawmap used to read the newest saved map's map.decl off DISK. That is wrong in two ways the
 * person can hit without doing anything unusual:
 *
 *   - unsaved edits are not in it. It exports the last SAVED state and reports success.
 *   - on a map that has never been saved there is no folder for it, so it exports whichever OTHER
 *     map was saved most recently. Silently. Same class of "borrowed the wrong slot" mistake as the
 *     load bug.
 *
 * Asking the engine to serialize the map that is actually open removes both, and removes the disk
 * entirely from the save path: no newest-folder scan, no dirty flag to consult, nothing to grey out.
 *
 * WHAT TO CALL. SnapMapToJson (0x59D2F0), NOT SerializeToJson (0x5F2390). SerializeToJson's first
 * argument is a temporary snapshot object that SnapMapToJson builds and destroys around it -- see
 * the signature note. The save shadow never had to know that, because it only ever inspects the
 * argument the engine already prepared for it.
 *
 * THE idStr. The engine writes its output into an idStr we supply, so it must be a real one: its
 * own constructor and destructor, taken from the engine, never a zeroed block. A zeroed idStr has a
 * null data pointer and a zero alloced count, and whether the engine's assignment path tolerates
 * that is an assumption this project does not need to make. Both come from decoding the two CALLs
 * inside SnapMapAddBranchTag, which is uniquely signable -- the technique the resolve-address
 * discipline prescribes for functions with identical twins, and which this codebase already uses to
 * reach idList-grow. sizeof(idStr) is 0x30, DIRECT from that same function's tag-list stride
 * (LEA RCX,[RAX+RAX*2]; SHL RCX,4). */

typedef unsigned char (*map_to_json_fn)(void *map, void *out_idstr, unsigned char compact);
typedef void *(*idstr_ctor_fn)(void *self, const char *init);
typedef void  (*idstr_dtor_fn)(void *self);

#define IDSTR_SIZE 0x30

static map_to_json_fn g_map_to_json = NULL;
static idstr_ctor_fn  g_idstr_ctor  = NULL;
static idstr_dtor_fn  g_idstr_dtor  = NULL;
static volatile LONG  g_live_faulted = 0;

/* Offsets of the two CALL instructions inside SnapMapAddBranchTag, from its own base. Checked for an
 * E8 opcode before the displacement is believed: a constant offset into another function is a guess
 * until the byte there agrees, and this project's rule is that addresses come from derivation. */
#define ADDTAG_CALL_IDSTR_CTOR 0x35
#define ADDTAG_CALL_IDSTR_DTOR 0xB2

static void *decode_rel32_call(const unsigned char *at)
{
    int rel = 0;
    if (at == NULL || *at != 0xE8) return NULL;
    memcpy(&rel, at + 1, sizeof rel);
    return (void *)(at + 5 + rel);
}

int sh_rawmap_set_live_serialize(void *map_to_json, void *add_branch_tag_fn)
{
    const unsigned char *tag = (const unsigned char *)add_branch_tag_fn;

    g_map_to_json = (map_to_json_fn)map_to_json;
    g_idstr_ctor  = NULL;
    g_idstr_dtor  = NULL;

    if (tag != NULL) {
        __try {
            g_idstr_ctor = (idstr_ctor_fn)decode_rel32_call(tag + ADDTAG_CALL_IDSTR_CTOR);
            g_idstr_dtor = (idstr_dtor_fn)decode_rel32_call(tag + ADDTAG_CALL_IDSTR_DTOR);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_idstr_ctor = NULL;
            g_idstr_dtor = NULL;
        }
    }

    {
        char line[256];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: rawmap live-serialize %s (map->json=%p, idStr ctor=%p, dtor=%p)",
                    sh_rawmap_live_serialize_ready() ? "READY" : "UNAVAILABLE",
                    (void *)g_map_to_json, (void *)g_idstr_ctor, (void *)g_idstr_dtor);
        backend_log(line);
    }
    return sh_rawmap_live_serialize_ready();
}

int sh_rawmap_live_serialize_ready(void)
{
    if (InterlockedCompareExchange(&g_live_faulted, 0, 0) != 0) return 0;
    return (g_map_to_json != NULL && g_idstr_ctor != NULL && g_idstr_dtor != NULL) ? 1 : 0;
}

/* Serialize `map` and write it to the rawmap destination. MAIN THREAD ONLY -- it reads engine state
 * and allocates through the engine's allocator, so it is called from the editor-frame hook, never
 * from the UI thread. */
int sh_rawmap_write_from_live(void *map, char *out_msg, int msg_capacity,
                              unsigned long long *out_bytes)
{
    unsigned char blk[IDSTR_SIZE];
    unsigned long long wrote = 0;
    int len = 0;
    const char *data = NULL;
    unsigned char rc = 0;

    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    if (out_bytes) *out_bytes = 0;

    if (!sh_rawmap_live_serialize_ready()) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "this build cannot serialize the open map", _TRUNCATE);
        return 0;
    }
    if (map == NULL) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "no map is open", _TRUNCATE);
        return 0;
    }

    memset(blk, 0, sizeof blk);

    /* One guard around the whole engine sequence. A fault after the constructor leaks that one
     * idStr, which is accepted deliberately: the alternative is unwinding engine state we do not
     * own, and the fault also disables this path for the session, so it can happen once. */
    __try {
        g_idstr_ctor(blk, "");
        rc = g_map_to_json(map, blk, 1);
        if (rc) {
            len  = *(const int *)(blk + IDSTR_LEN_OFF);
            data = *(const char *const *)(blk + IDSTR_DATA_OFF);
            if (data != NULL && len > 0) wrote = write_shadow(data, (size_t)len);
        }
        g_idstr_dtor(blk);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_live_faulted, 1);
        backend_log("B1: rawmap live-serialize FAULTED; disabled for this session");
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "serializing the open map faulted", _TRUNCATE);
        return 0;
    }

    if (!rc) {
        backend_log("B1: rawmap live-serialize -- the engine declined to serialize the map");
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "the engine would not serialize this map", _TRUNCATE);
        return 0;
    }
    if (wrote == 0) {
        char path[MAX_PATH] = "";
        resolve_dest_path(path, sizeof path);
        {
            char line[MAX_PATH + 128];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B1: rawmap live-serialize produced %d bytes but the write to %s failed",
                        len, path);
            backend_log(line);
        }
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "the rawmap file could not be written", _TRUNCATE);
        return 0;
    }

    InterlockedExchange64(&g_last_bytes, (LONGLONG)wrote);
    InterlockedIncrement(&g_shadow_count);
    {
        char path[MAX_PATH] = "", line[MAX_PATH + 128];
        resolve_dest_path(path, sizeof path);
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: rawmap SAVE wrote %llu bytes from the OPEN map -> %s [live]", wrote, path);
        backend_log(line);
        if (out_msg) {
            _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE,
                        "Wrote %llu bytes from the open map.", wrote);
        }
    }
    if (out_bytes) *out_bytes = wrote;
    return 1;
}

static unsigned long long write_shadow(const char *data, size_t len)
{
    char path[MAX_PATH];
    char temp[MAX_PATH];
    char dir[MAX_PATH];
    char line[MAX_PATH * 2 + 96];
    unsigned long long written;
    unsigned long long total = 0;
    HANDLE h;

    resolve_dest_path(path, sizeof path);

    /* MAKE THE FOLDER IF IT IS NOT THERE. A save path is allowed to name a folder that does not
     * exist yet -- `savepath D:\archive\2026\mine.json` should work the first time, not fail once
     * and then work after a trip to Explorer. The folder is made HERE, at the write, rather than
     * when the setting is typed: a setting that creates directories the moment you name one turns
     * every typo into a stray folder on the disk. If this fails, CreateFileA below fails too and
     * reports it the same way any other unwritable destination does. */
    if (dest_dir_part(path, dir, sizeof dir)) ensure_dir(dir);

    /* WRITE BESIDE THE TARGET, THEN RENAME OVER IT. The destination file is never opened for
     * writing, so a save that dies partway cannot destroy the rawmap already there.
     *
     * The version before this opened the destination with CREATE_ALWAYS, which EMPTIES the file
     * before the first byte of the new one is written. Three megabytes then went in over perhaps a
     * few hundred milliseconds, and anything that interrupted that window -- a full disk, an
     * unplugged drive, the game going down -- left a fragment where an archive used to be. Deleting
     * the fragment afterwards was honest but no help: the old map was already gone. An archival tool
     * that can eat the thing it is archiving is not one.
     *
     * MoveFileEx with REPLACE_EXISTING is the swap, and on the same volume it is a rename: it either
     * happened or it did not, and no reader ever sees a half-written file under the real name. The
     * temp file sits next to the target rather than in %TEMP% on purpose -- a rename ACROSS volumes
     * degrades to a copy, which would reintroduce the very window this removes.
     *
     * If we cannot write the temp file at all, nothing has been touched yet, so the old file
     * survives even the total failure. */
    if (_snprintf_s(temp, sizeof temp, _TRUNCATE, "%s.tmp", path) < 0) return 0;

    h = CreateFileA(temp, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: rawmap write could not open %s (err %lu) -- %s is untouched",
                    temp, GetLastError(), path);
        backend_log(line);
        return 0;
    }

    while (total < len) {
        size_t remain = len - (size_t)total;
        DWORD chunk = (DWORD)(remain > 0x10000000 ? 0x10000000 : remain);
        DWORD wr = 0;
        if (!WriteFile(h, data + total, chunk, &wr, NULL) || wr == 0) break;
        total += wr;
    }
    /* Flush before the rename, not after. A rename that publishes a name whose bytes are still in
     * the cache is the same broken promise as a partial write, just harder to see. */
    if (total == len) FlushFileBuffers(h);
    CloseHandle(h);

    if (total != len) {
        /* Nothing was published: take the scratch file away, leave the destination alone. */
        DeleteFileA(temp);
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: rawmap write FAILED after %llu of %llu bytes -- %s is unchanged",
                    total, (unsigned long long)len, path);
        backend_log(line);
        return 0;
    }

    if (!MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        DeleteFileA(temp);
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: rawmap write complete but the rename onto %s failed (err %lu) -- "
                    "the previous file is unchanged", path, err);
        backend_log(line);
        return 0;
    }

    written = total;

    /* Spend a one-off destination only on success, and only here: this is where the
     * bytes reach disk, so a failed export keeps its aim for a retry. */
    if (written) spend_dest_once();
    return written;
}

#ifdef SH_RAWMAP_TESTING
/* Test-only door onto the writer: the destination rules only mean anything in terms
 * of what actually reaches disk. Compiled out of shipping builds. */
unsigned long long sh_rawmap_test_write(const char *data, size_t len)
{
    return write_shadow(data, len);
}
#endif

/* Produce an owned pretty-printed copy, or NULL to keep the engine bytes. The
 * caller guards reads of the engine-owned source. Formatting refusal never
 * suppresses the mirror write.
 */
static char *pretty_copy(const char *data, size_t len, size_t *out_len)
{
    size_t need = json_pretty(data, len, NULL, 0);   /* pass 1: measure (no store) */
    char  *buf;
    if (need == 0) return NULL;
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, need);
    if (buf == NULL) return NULL;
    if (json_pretty(data, len, buf, need) != need) { /* Refuse if measured and written lengths disagree. */
        HeapFree(GetProcessHeap(), 0, buf);
        return NULL;
    }
    *out_len = need;
    return buf;
}

/* ==== embed-on-save: the packages a map uses travel inside it ==== */

#define EMBED_CONFIG_KEY   "packages.embed_in_saved_maps"
#define EMBED_MAX_PACKAGES 8

/* idStr layout, the same three offsets swf_textedit.c writes through. */
#ifndef IDSTR_FLAGS_OFF
#define IDSTR_FLAGS_OFF 0x00
#endif
#ifndef IDSTR_SIZE
#define IDSTR_SIZE 0x30
#endif

typedef void (*idstr_assign_fn)(void *dst, const void *src);
static idstr_assign_fn g_idstr_assign = NULL;

void sh_rawmap_embed_install(const void *module_base)
{
    size_t i;
    if (module_base == NULL) {
        backend_log("MPKG: embed-on-save DARK -- no module base to resolve the idStr assignment");
        return;
    }
    /* Resolve assignment before writing engine-owned save output. */
    for (i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        sig_result ra;
        sig_status st;
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "IdStrAssignFromStr") != 0) continue;
        st = sig_resolve_one(module_base, &BACKEND_ENGINE_SIGNATURES[i], &ra);
        if (st == SIG_OK || st == SIG_OK_HOOKED) g_idstr_assign = (idstr_assign_fn)ra.addr;
        break;
    }
    backend_log(g_idstr_assign != NULL
        ? "MPKG: embed-on-save ready -- a saved map will carry the packages it uses"
        : "MPKG: embed-on-save DARK -- idStr assignment unresolved; saves are unchanged");
}

/* Build JSON with detected package payloads, or NULL for no replacement.
 * Reads package files but does not mutate engine objects.
 */
static char *embed_used_packages(const char *json, size_t len, size_t *out_len)
{
    sh_mpkg_used used[EMBED_MAX_PACKAGES];
    char root[MAX_PATH];
    char *cur = NULL;
    size_t cur_len = len, count, i;
    int enabled = 1;

    *out_len = 0;
    (void)sh_config_get_bool(EMBED_CONFIG_KEY, &enabled, NULL);
    if (!enabled) return NULL;
    if (!sh_overrides_get_root(root, sizeof root)) return NULL;

    count = sh_mpkg_used_packages(json, len, root, used, EMBED_MAX_PACKAGES);
    if (count == 0) return NULL;

    for (i = 0; i < count; i++) {
        unsigned char *payload;
        size_t payload_len = 0, next_len = 0;
        char err[SH_MPKG_ERR_CAP];
        char *next;
        char line[SH_MPKG_ERR_CAP + 128];

        payload = sh_mpkg_pack_dir(used[i].root, &payload_len, err, sizeof err);
        if (!payload) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: package '%s' could NOT be packed for this save (%s); the map is "
                        "being saved WITHOUT it", used[i].id, err);
            backend_log(line);
            continue;
        }
        next = sh_mpkg_embed(cur ? cur : json, cur_len, used[i].id,
                             payload, payload_len, &next_len, err, sizeof err);
        HeapFree(GetProcessHeap(), 0, payload);
        if (!next) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: package '%s' could NOT be embedded in this save (%s); the map is "
                        "being saved WITHOUT it", used[i].id, err);
            backend_log(line);
            continue;
        }
        if (cur) HeapFree(GetProcessHeap(), 0, cur);
        cur = next;
        cur_len = next_len;
    }

    if (!cur) return NULL;
    *out_len = cur_len;
    return cur;
}

/* Assign body through the native idStr helper. A zero flags word disables
 * stealing, so the engine copies the temporary bytes.
 */
static int replace_out_idstr(void *out_idstr, const char *body, size_t body_len)
{
    uint8_t src[IDSTR_SIZE];
    if (!g_idstr_assign || !out_idstr || !body) return 0;
    if (body_len > (size_t)INT_MAX) return 0;
    memset(src, 0, sizeof src);
    *(int *)(src + IDSTR_LEN_OFF) = (int)body_len;
    *(const char **)(src + IDSTR_DATA_OFF) = body;
    *(uint32_t *)(src + IDSTR_FLAGS_OFF) = 0;
    __try {
        g_idstr_assign(out_idstr, src);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

/* Read the engine's out-idStr under SEH (the engine fills it; a layout surprise must not fault
 * the save path). Returns 1 with the bytes and length the engine just wrote. */
static int read_out_idstr(void *out_idstr, const char **data, int *len)
{
    __try {
        *len  = *(int *)((unsigned char *)out_idstr + IDSTR_LEN_OFF);
        *data = *(const char **)((unsigned char *)out_idstr + IDSTR_DATA_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return *data != NULL && *len > 0;
}

/* Best-effort package embedding into serialized output before the save caller
 * consumes it.
 */
static void mpkg_embed_on_save(void *out_idstr)
{
    const char *data = NULL;
    int len = 0;
    char *body = NULL;
    size_t body_len = 0;

    if (!g_idstr_assign || out_idstr == NULL) return;
    if (!read_out_idstr(out_idstr, &data, &len)) return;

    __try {
        body = embed_used_packages(data, (size_t)len, &body_len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        body = NULL;
    }
    if (body == NULL) return;

    if (replace_out_idstr(out_idstr, body, body_len)) {
        char line[192];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "MPKG: saved map now CARRIES its packages -- %d -> %zu bytes; a player "
                    "without them can install them from the map itself", len, body_len);
        backend_log(line);
    } else {
        backend_log("MPKG: embed-on-save could not write the map back; the save is unchanged");
    }
    HeapFree(GetProcessHeap(), 0, body);
}

/* Refresh marked volumes from save output. Editor previews and the final Play
 * snapshot separately capture unsaved edits through nav_bake.
 */
static void nav_regions_on_save(void *out_idstr)
{
    const char *data = NULL;
    int len = 0;

    if (!g_idstr_assign || out_idstr == NULL) return;
    if (!read_out_idstr(out_idstr, &data, &len)) return;
    if (data == NULL || len <= 0) return;

    sh_nav_bake_set_map(data, (size_t)len);
}

static void nav_embed_on_save(void *out_idstr)
{
    const char *data = NULL;
    int len = 0;
    char *body = NULL;
    size_t body_len = 0;

    if (!g_idstr_assign || out_idstr == NULL) return;
    if (!read_out_idstr(out_idstr, &data, &len)) return;

    body = sh_navmesh_embed_all(data, (size_t)len, &body_len);
    if (body == NULL) return;

    if (!replace_out_idstr(out_idstr, body, body_len))
        backend_log("NAV: the saved map could not be written back; its navigation shards are "
                    "not in this save");
    HeapFree(GetProcessHeap(), 0, body);
}

/* Serialize, embed delivery payloads, then optionally mirror output. Preserve
 * the native bool result on every return.
 */
static unsigned char sh_ser_detour(void *map, void *out_idstr, unsigned char compact)
{
    int used_oneshot = 0;   /* did a "Save Rawmap As" one-shot authorize this write? */

    if (g_ser_orig == NULL) return 0;   /* defensive: should never happen once installed */

    /* Call native serialization unconditionally and capture its bool result
     * before any helper can clobber AL.
     */
    const unsigned char rc = g_ser_orig(map, out_idstr, compact);
    if (g_snapshot_depth) return rc;

    /* Embed used packages before mirroring; independent of the rawmap switch. */
    mpkg_embed_on_save(out_idstr);

    /* Restore retained navigation shards after package embedding so ordinary
     * saves preserve delivered payloads.
     */
    nav_embed_on_save(out_idstr);

    /* Refresh navigation regions from the final serialized map. */
    nav_regions_on_save(out_idstr);

    /* Gate only the disk mirror; load substitution uses the same predicate. A
     * one-shot from "Save Rawmap As" also passes, and is consumed here rather than
     * after the write, so two racing saves cannot both spend it. */
    used_oneshot = (InterlockedExchange(&g_shadow_oneshot, 0) != 0);
    if (!rawmap_armed(NULL) && !used_oneshot) return rc;

    if (out_idstr == NULL) return rc;

    /* Read output idStr fields under SEH before copying engine-owned memory. */
    const char *data = NULL;
    int         len  = 0;
    __try {
        len  = *(int *)((unsigned char *)out_idstr + IDSTR_LEN_OFF);
        data = *(const char **)((unsigned char *)out_idstr + IDSTR_DATA_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return rc;   /* Unreadable output: skip mirroring and preserve the native result. */
    }
    if (data == NULL || len <= 0) return rc;

    /* Read the current pretty switch for each mirror; default off. */
    int pretty = sh_cvar_value_int(B2_CVAR_SH_PRETTY_ON, 0);

    const char *body     = data;                     /* mirror bytes */
    size_t      body_len = (size_t)len;              /* engine output or formatted copy */
    char       *shaped   = NULL;

    if (pretty) {
        size_t shaped_len = 0;
        __try {
            shaped = pretty_copy(data, (size_t)len, &shaped_len);   /* reads the engine's buffer */
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            shaped = NULL;
        }
        if (shaped != NULL) { body = shaped; body_len = shaped_len; }
    }
    int laid_out = (shaped != NULL);

    unsigned long long wrote = 0;
    __try {
        wrote = write_shadow(body, body_len);   /* may read `data` (engine heap/SSO) -> guard the read */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wrote = 0;
    }
    if (shaped != NULL) HeapFree(GetProcessHeap(), 0, shaped);

    if (wrote > 0) {
        InterlockedExchange64(&g_last_bytes, (LONGLONG)wrote);
        unsigned long n = (unsigned long)InterlockedIncrement(&g_shadow_count);
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B1: rawmap SAVE shadow wrote %llu bytes -> rawmap.json [#%lu]%s%s", wrote, n,
            used_oneshot ? " [one-shot]" : "",
            pretty ? (laid_out ? " [pretty]"
                               : " [pretty requested; JSON did not re-lay-out -- wrote it unchanged]")
                   : "");
        backend_log(line);
    }

    /* The engine's verdict, never the shadow's. A failed mirror is not a failed save. */
    return rc;
}

int sh_rawmap_save_install(void *serialize_fn, int serialize_status_ok)
{
    char line[200];

    if (serialize_fn == NULL) {
        backend_log("B1: rawmap SAVE shadow SKIPPED -- SerializeToJson not resolved");
        return 0;
    }
    if (!serialize_status_ok) {
        /* Refuse an already-hooked prologue, matching the load detour policy. */
        backend_log("B1: rawmap SAVE shadow SKIPPED -- SerializeToJson resolved via hook-tolerant "
                    "fallback (prologue already hooked); not installing over an existing detour");
        return 0;
    }
    if (g_ser_orig != NULL) {
        backend_log("B1: rawmap SAVE shadow already installed");
        return 1;
    }

    void *tramp = install_inline_hook(serialize_fn, (void *)sh_ser_detour, SAVE_STOLEN);
    if (tramp == NULL) {
        backend_log("B1: rawmap SAVE shadow FAIL -- install_inline_hook returned NULL");
        return 0;
    }
    g_ser_orig = (serialize_fn_t)tramp;

    {
        /* Resolve the destination for the log line rather than storing it: writing a
         * default into a variable would make sh_rawmap_paths_are_default answer "no". */
        char dest_now[MAX_PATH] = "";
        resolve_dest_path(dest_now, sizeof dest_now);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B1: rawmap SAVE shadow installed at %p (trampoline %p, stolen %d); dest=%s",
            serialize_fn, tramp, SAVE_STOLEN, dest_now);
    }
    backend_log(line);
    return 1;
}

int sh_rawmap_save_set_dest(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        /* "Back to the default" has to mean the default, so it clears the toggle as well as any
         * pending one-off. Through the setter for the toggle, so the change is persisted --
         * assigning the variable directly here was the version that came back ticked next launch. */
        g_dest_once[0] = '\0';
        if (g_dest_mode != RAWMAP_DEST_DEFAULT) sh_rawmap_set_dest_follows_source(0);
        return 1;
    }
    strncpy_s(g_dest_once, sizeof g_dest_once, path, _TRUNCATE);
    return g_dest_once[0] != '\0';
}

unsigned long sh_rawmap_save_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_shadow_count, 0, 0);
}

unsigned long long sh_rawmap_save_last_bytes(void)
{
    return (unsigned long long)InterlockedCompareExchange64(&g_last_bytes, 0, 0);
}

/* ==== the File-menu file surface (+0x328 status / +0x330 configure) ==== */

/* Copy `src` into a JSON string body, escaping what a Windows path can actually contain. Backslash is
 * the whole reason this exists -- an unescaped "C:\maps\x.json" makes the frontend's JSON.parse throw,
 * so the paths this file reports would break the very menu that shows them. Quote and the C0 range are
 * escaped for completeness. Returns 0 if the result would not fit (caller writes no field). */
static int json_escape_into(char *out, size_t cap, const char *src)
{
    size_t w = 0;
    if (!out || cap == 0) return 0;
    for (; src && *src; ++src) {
        unsigned char c = (unsigned char)*src;
        const char *esc = NULL;
        char ubuf[7];
        if      (c == '\\') esc = "\\\\";
        else if (c == '"')  esc = "\\\"";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) { _snprintf_s(ubuf, sizeof ubuf, _TRUNCATE, "\\u%04x", c); esc = ubuf; }
        if (esc) {
            size_t n = strlen(esc);
            if (w + n >= cap) return 0;
            memcpy(out + w, esc, n);
            w += n;
        } else {
            if (w + 1 >= cap) return 0;
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
    return 1;
}

/* Is this file a SnapMap RAWMAP, as opposed to some other JSON the tool wrote?
 *
 * `sh_rawmaps list` showed every *.json it found, and that is wrong in two directions at once. The
 * default folder is %LOCALAPPDATA%\snapmap-plus\, which also holds config.json, install.json and
 * pinned.json; and prefabs\ is full of *.snapmap.json files that are NOT maps. Offering any of
 * those as something to load is worse than useless -- it invites loading one.
 *
 * sh_rawmap_validate_source cannot answer this: it requires a JSON OBJECT, which all of the above
 * are. It is deliberately left that lenient -- it guards an explicit load of a file someone named,
 * where the honest failure is a parse error rather than a refusal based on a guess. A listing can be
 * pickier than a loader, because guessing wrong here only hides a row.
 *
 * WHAT ACTUALLY DISTINGUISHES THEM is the top-level "~type" the engine's own serializer writes:
 *
 *     a rawmap  ends  ..."version":111,"~type":"idSnapMap","~version":111}
 *     a prefab  says  "~type":"idSnapEntityPrefab"
 *
 * Checked in the TAIL, not the head. Keys come out in sorted order, so "~type" is the second to last
 * of them -- 36 bytes from the end of a 3 MB rawmap on this machine. A tail read costs exactly what
 * a head read costs, and 8 KB of it is a wide margin for a pretty-printed file (sh_pretty_on
 * re-lays these out, which is also why the search cannot assume there is no space after the colon).
 *
 * The quotes in the needle matter: idSnapMapCapEntity begins with idSnapMap, and a PREFAB of map
 * geometry can contain those entities. Searching for the quoted value cannot confuse the two. */
int sh_rawmap_looks_like_rawmap(const char *path)
{
    HANDLE h;
    LARGE_INTEGER sz;
    LARGE_INTEGER at;
    char  *buf;
    DWORD  rd = 0;
    int    found = 0;
    const DWORD tail = 8 * 1024;

    if (path == NULL || path[0] == '\0') return 0;
    /* Cheap gates first: openable, non-empty, under the size ceiling, starts with '{'. */
    if (!sh_rawmap_validate_source(path, NULL, 0)) return 0;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return 0; }

    at.QuadPart = (sz.QuadPart > (LONGLONG)tail) ? (sz.QuadPart - (LONGLONG)tail) : 0;
    if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN)) { CloseHandle(h); return 0; }

    buf = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)tail + 1);
    if (buf == NULL) { CloseHandle(h); return 0; }

    if (ReadFile(h, buf, tail, &rd, NULL) && rd > 0) {
        buf[rd] = '\0';                    /* strstr needs the terminator; the +1 above is for it */
        found = (strstr(buf, "\"idSnapMap\"") != NULL) ? 1 : 0;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(h);
    return found;
}

int sh_rawmap_validate_source(const char *path, char *out_msg, int msg_capacity)
{
    HANDLE h;
    LARGE_INTEGER sz;
    char probe[64];
    DWORD rd = 0;
    int i;

    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    if (path == NULL || path[0] == '\0') {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "no path given", _TRUNCATE);
        return 0;
    }

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "cannot open that file", _TRUNCATE);
        return 0;
    }

    /* The ceiling matches read_source_file's own 64 MB refusal exactly. Rejecting here rather than
     * there is the whole point of this check: the swap's refusal is silent and lands minutes later
     * at a map load, this one lands on the click with a reason attached. */
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "that file is empty", _TRUNCATE);
        return 0;
    }
    if (sz.QuadPart > (LONGLONG)(64 * 1024 * 1024)) {
        CloseHandle(h);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "that file is over the 64 MB limit", _TRUNCATE);
        return 0;
    }

    if (!ReadFile(h, probe, (DWORD)sizeof probe, &rd, NULL)) rd = 0;
    CloseHandle(h);

    /* A rawmap is a JSON object. Skip a UTF-8 BOM and leading whitespace, then require '{'. This
     * catches the realistic mistake -- picking a map.decl (zlib bytes) or some unrelated file -- and
     * says so now instead of substituting unparseable bytes into a real map load. */
    i = 0;
    if (rd >= 3 && (unsigned char)probe[0] == 0xEF
                && (unsigned char)probe[1] == 0xBB
                && (unsigned char)probe[2] == 0xBF) i = 3;
    while (i < (int)rd && (probe[i] == ' ' || probe[i] == '\t' ||
                           probe[i] == '\r' || probe[i] == '\n')) ++i;
    if (i >= (int)rd || probe[i] != '{') {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "that is not rawmap JSON (a compressed map.decl?)", _TRUNCATE);
        return 0;
    }

    if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "ok", _TRUNCATE);
    return 1;
}

/* ------------------------------------------------------- save it NOW, from the map already on disk --
 * "Save Rawmap As" used to only name a destination and then wait for the person to save their map in
 * the editor. For a map that was ALREADY saved that is a pointless errand: the bytes exist, complete,
 * on disk. A locally saved SnapMap keeps them at
 *
 *     <the save folder>\map.decl   =   [4-byte checksum][zlib(rawmap JSON)]
 *
 * and `rawmap.json` is exactly that JSON. Both halves of that are established: the engine's map.decl
 * fetch hands the record payload straight to zlib inflate, and the layout has been decoded end-to-end
 * against real saves on disk. So this path reads, inflates and writes -- no engine call, no thread
 * discipline, no waiting.
 *
 * The 4-byte header is a CHECKSUM, not a length (verified: 0x9D4CF0F1 on a 2,638-byte file that
 * inflates to 23,638), so nothing in the file says how big the JSON is. Hence sh_inflate_raw_upto and
 * a capacity ladder rather than one sized allocation.
 *
 * Only the shadow's one-shot remains for the case this genuinely cannot serve: a map that has never
 * been saved has no map.decl to read, and only the editor can produce its bytes. */
static int save_from_local_map(char *out_msg, int msg_capacity, unsigned long long *out_bytes)
{
    /* 256 KB covers every real map by a wide margin (the ones measured are 20-24 KB); the rungs above
     * exist so an unusually large map still works rather than reporting a false "malformed". Capped at
     * the same 64 MB the load side refuses past. */
    static const size_t ladder[] = { 256u * 1024u, 1024u * 1024u, 4u * 1024u * 1024u,
                                     16u * 1024u * 1024u, 64u * 1024u * 1024u };

    char dir[MAX_PATH], id[64], decl[MAX_PATH];
    HANDLE h;
    LARGE_INTEGER sz;
    unsigned char *raw = NULL;
    char *json = NULL;
    DWORD rd = 0;
    size_t got = 0, i;
    int ok = 0;

    if (out_bytes) *out_bytes = 0;
    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';

    if (!sh_editor_frame_saved_map_dir(dir, sizeof dir, id, sizeof id)) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "no saved map found -- save your map in the editor first", _TRUNCATE);
        return 0;
    }
    _snprintf_s(decl, sizeof decl, _TRUNCATE, "%s\\map.decl", dir);

    h = CreateFileA(decl, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "the saved map's map.decl could not be opened", _TRUNCATE);
        return 0;
    }
    /* 6 = the 4-byte checksum plus zlib's own 2-byte header; 4 more for the trailing Adler-32. A file
     * that cannot hold all of that plus a byte of deflate is not a map.decl. */
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 11 || sz.QuadPart > (LONGLONG)(64 * 1024 * 1024)) {
        CloseHandle(h);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "the saved map's map.decl is not a size we can read", _TRUNCATE);
        return 0;
    }
    raw = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sz.QuadPart);
    if (raw == NULL) { CloseHandle(h); return 0; }
    if (!ReadFile(h, raw, (DWORD)sz.QuadPart, &rd, NULL) || rd != (DWORD)sz.QuadPart) {
        CloseHandle(h);
        HeapFree(GetProcessHeap(), 0, raw);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "the saved map's map.decl could not be read", _TRUNCATE);
        return 0;
    }
    CloseHandle(h);

    for (i = 0; i < sizeof ladder / sizeof ladder[0] && !got; i++) {
        json = (char *)HeapAlloc(GetProcessHeap(), 0, ladder[i]);
        if (json == NULL) break;
        /* Two slices, because the decoder refuses trailing bytes: the deflate data proper (dropping
         * zlib's 4-byte Adler-32 trailer), and failing that the whole tail -- so a variant that does
         * not carry the trailer still decodes instead of being reported malformed. */
        got = sh_inflate_raw_upto(raw + 6, (size_t)rd - 6 - 4, (unsigned char *)json, ladder[i]);
        if (!got) got = sh_inflate_raw_upto(raw + 6, (size_t)rd - 6, (unsigned char *)json, ladder[i]);
        if (!got) { HeapFree(GetProcessHeap(), 0, json); json = NULL; }
    }
    HeapFree(GetProcessHeap(), 0, raw);

    if (!got || json == NULL) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "the saved map's map.decl did not decompress", _TRUNCATE);
        return 0;
    }

    /* Sanity: it must look like the rawmap JSON we claim it is, not merely decompress. A silent write
     * of the wrong bytes is worse than a refusal, because it looks like it worked. */
    if (json[0] != '{') {
        HeapFree(GetProcessHeap(), 0, json);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "the saved map decompressed to something that is not rawmap JSON",
                               _TRUNCATE);
        return 0;
    }

    {
        unsigned long long wrote = write_shadow(json, got);
        HeapFree(GetProcessHeap(), 0, json);
        if (wrote > 0) {
            char line[MAX_PATH * 2 + 128];
            char dest_now[MAX_PATH] = "";
            if (out_bytes) *out_bytes = wrote;
            InterlockedExchange64(&g_last_bytes, (LONGLONG)wrote);
            InterlockedIncrement(&g_shadow_count);
            /* NAME THE DESTINATION. Without it this line said a write succeeded and left the one
             * question that matters -- into WHICH file -- unanswerable, which is exactly the shape
             * a save that "did not stick" takes: it stuck somewhere else. */
            resolve_dest_path(dest_now, sizeof dest_now);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B1: rawmap SAVE wrote %llu bytes from saved map %s -> %s [from-disk]",
                        wrote, id, dest_now[0] ? dest_now : "<unresolved>");
            backend_log(line);
            if (out_msg) {
                _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE,
                            "Wrote %llu bytes from saved map %s.", wrote, id);
            }
            ok = 1;
        } else if (out_msg) {
            strncpy_s(out_msg, (size_t)msg_capacity,
                      "could not write to that location", _TRUNCATE);
        }
    }
    return ok;
}

/* The same verdict, asked about the file the swap would ACTUALLY read.
 *
 * The reload used to refuse unless the shared gate was armed, which is why the File menu needed an
 * "Armed" tick at all: without it the reload declined, and with it every unrelated map load was
 * substituted too. The gate was never the property worth checking. What matters is whether the
 * staged bytes will be accepted, and this asks exactly that -- so the reload can arm the swap around
 * its own call and put the gate back the way the person left it. */
/* Report BOTH effective paths -- what the swap would read and what a save would be mirrored to.
 *
 * Exposed because `sh_rawmaps_on` was a switch whose effect depended on state the person could not
 * see: it arms substitution for every subsequent map load, using whichever file some earlier click
 * staged. The File menu at least prints the two paths; a console user was arming blind. A switch
 * that cannot say what it is about to do is the problem, and this is what fixes it. */
/* The DEFAULT paths, whatever is currently set. Distinct from sh_rawmap_get_paths, which reports the
 * EFFECTIVE ones -- a caller that wants to say "this is not the usual file" needs both. */
void sh_rawmap_get_default_paths(char *load_out, int load_cap, char *save_out, int save_cap)
{
    if (load_out && load_cap > 0) { load_out[0] = '\0'; default_source_path(load_out, (size_t)load_cap); }
    if (save_out && save_cap > 0) { save_out[0] = '\0'; default_dest_path(save_out, (size_t)save_cap); }
}

/* 1 = both effective paths ARE the built-in defaults. Asked by the legacy arm command, whose
 * published documentation names rawmap.json specifically: if something has moved the paths since,
 * that documentation is describing a file the command will not touch, and saying so is cheaper than
 * letting someone find out by opening a map.
 *
 * COMPARED BY VALUE, not by emptiness. The first version tested g_src_path[0] == 0, which is never
 * true once the hooks are installed: both installers materialize the default into their own variable
 * so the log line can print it (see the LOAD-swap and SAVE-shadow install paths). The result was a
 * predicate stuck at "not default", so `sh_rawmaps_on` printed its "these are not the default files"
 * note every single time, including on a perfectly default session. */
int sh_rawmap_paths_are_default(void)
{
    char src_now[MAX_PATH] = "", dst_now[MAX_PATH] = "";
    char src_def[MAX_PATH] = "", dst_def[MAX_PATH] = "";

    /* A savepath setting counts as a moved save path even when nothing was typed here: it points
     * saves somewhere other than rawmap.json, which is exactly what the note warns about. */
    if (g_dest_mode != RAWMAP_DEST_DEFAULT) return 0;

    resolve_source_path(src_now, sizeof src_now);
    resolve_dest_path(dst_now, sizeof dst_now);
    default_source_path(src_def, sizeof src_def);
    default_dest_path(dst_def, sizeof dst_def);

    return (_stricmp(src_now, src_def) == 0 && _stricmp(dst_now, dst_def) == 0) ? 1 : 0;
}

void sh_rawmap_get_paths(char *load_out, int load_cap, char *save_out, int save_cap)
{
    if (load_out && load_cap > 0) {
        load_out[0] = '\0';
        resolve_source_path(load_out, (size_t)load_cap);
    }
    if (save_out && save_cap > 0) {
        save_out[0] = '\0';
        resolve_dest_path(save_out, (size_t)save_cap);
    }
}

int sh_rawmap_source_ok(char *out_msg, int msg_capacity)
{
    char path[MAX_PATH];
    resolve_source_path(path, sizeof path);
    return sh_rawmap_validate_source(path, out_msg, msg_capacity);
}

static int slot_rawmap_status(sh_iface *self, char *out_json, int out_capacity)
{
    char load_path[MAX_PATH], save_path[MAX_PATH];
    char load_esc[MAX_PATH * 2], save_esc[MAX_PATH * 2];
    int written;

    (void)self;
    if (out_json == NULL || out_capacity <= 0) return 0;

    resolve_source_path(load_path, sizeof load_path);
    resolve_dest_path(save_path, sizeof save_path);

    if (!json_escape_into(load_esc, sizeof load_esc, load_path)) load_esc[0] = '\0';
    if (!json_escape_into(save_esc, sizeof save_esc, save_path)) save_esc[0] = '\0';

    /* `armed` reports the EXPLICIT gate only, matching sh_rawmap_swap_is_armed's reasoning: a menu
     * checkbox must not show ON for a flag-file arm that unticking it cannot clear. `willFire` is
     * the gate OR that flag-file -- what actually happens -- so the menu can enable an item on
     * whether it would work rather than on whether the tick is set. */
    /* `loads` is the question the File menu actually has to answer: the staged file is substituted
     * into the NEXT map load, so "did it work" is unanswerable from the paths alone -- the person
     * needs to see the swap fire. Reporting both counters distinguishes the three outcomes that look
     * identical on screen: never fired (0), fired but the parse did not return (loads > loadsDone),
     * and a completed substituted load. Without this the only way to tell was reading
     * sh_backend.log for "B1: rawmap swap FIRED". */
    /* `savePending` is the same question for the save half: "Save Rawmap As" arms one save and then
     * waits for the person to save their map, and a waiting arm is invisible otherwise. */
    written = _snprintf_s(out_json, (size_t)out_capacity, _TRUNCATE,
        "{\"load\":\"%s\",\"save\":\"%s\",\"armed\":%d,\"saves\":%lu,\"lastBytes\":%llu,"
        "\"loads\":%lu,\"loadsDone\":%lu,\"savePending\":%d,\"loadPending\":%d,"
        /* `saveBack` is the File menu's "Use Rawmap as Save Path" tick. It has to ride the status
         * rather than be remembered by the page, because the console can change it too. */
        "\"saveBack\":%d,\"willFire\":%d}",
        load_esc, save_esc, sh_rawmap_swap_is_armed(),
        sh_rawmap_save_count(), sh_rawmap_save_last_bytes(),
        sh_rawmap_swap_count(), sh_rawmap_swap_complete_count(),
        sh_rawmap_save_oneshot_pending(),
        /* `loadPending` is the load half of the same question savePending answers: a staged rawmap
         * is waiting for the next map to open. Without it the menu could only report a COUNT of past
         * substitutions, which told the person nothing about what happens next. */
        sh_rawmap_load_oneshot_pending(),
        sh_rawmap_dest_follows_source(),
        sh_rawmap_swap_will_fire());

    return (written > 0) ? written : 0;
}

static int slot_rawmap_configure(sh_iface *self, const char *load_path, const char *save_path,
                                 int arm, char *out_msg, int msg_capacity)
{
    char reason[128];
    int ok = 1;

    (void)self;
    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';

    /* Order matters: validate and set the load source BEFORE arming. Arming first would leave a
     * window where the swap is live against whatever the previous source was -- which for someone
     * clicking "Load Rawmap" is the one outcome they did not ask for. */
    if (load_path != NULL) {
        if (load_path[0] == '\0') {
            sh_rawmap_swap_set_source(NULL);          /* empty = restore the default */
        } else if (sh_rawmap_validate_source(load_path, reason, (int)sizeof reason)) {
            if (!sh_rawmap_swap_set_source(load_path)) {
                if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                                       "could not set the load path", _TRUNCATE);
                return 0;
            }
            /* Unless this same call also names a destination -- then that choice is the one the
             * person made, and the reset would fight it. See sh_rawmap_reset_dest_for_new_load. */
            if (save_path == NULL) sh_rawmap_reset_dest_for_new_load();
        } else {
            if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, reason, _TRUNCATE);
            return 0;
        }
    }

    /* The save destination is NOT validated as a readable file -- it does not exist yet, and the
     * shadow itself creates it with CREATE_ALWAYS. A bad directory surfaces as a failed shadow
     * write, which the existing log line already reports. */
    if (save_path != NULL) {
        /* Saving BACK to the file you loaded from is allowed, and is the normal way to work: load a
         * rawmap, edit it, write it out again. An earlier version of this refused that outright.
         * The refusal was aimed at the right accident and the wrong cause -- what destroyed a staged
         * rawmap was Save Rawmap writing the WRONG MAP'S bytes onto it (it read the newest save off
         * disk), not the filenames matching. Serializing the open map fixed the cause, so the
         * round trip is safe and the blanket refusal only got in the way. The one rung that can
         * still write the wrong map guards itself -- see the ladder below. */
        /* REFUSE AN UNWRITABLE DESTINATION BEFORE AIMING ANYTHING AT IT. The ladder's best rung
         * queues onto the next editor frame, so "accepted" here means the write is about to happen,
         * not that it did -- exactly as the console's own save reported success for a read-only file
         * and only the log disagreed. Checked ahead of choose_dest so a refusal leaves the
         * destination untouched. */
        if (save_path[0] != '\0') {
            char wmsg[192] = "";
            if (!sh_rawmap_dest_writable_now(save_path, wmsg, (int)sizeof wmsg)) {
                if (out_msg && msg_capacity > 0)
                    _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE,
                                "Cannot save to %s -- %s", save_path,
                                wmsg[0] ? wmsg : "it cannot be written");
                return 0;
            }
        }

        if (!sh_rawmap_choose_dest(save_path)) ok = 0;

        /* NAMING A DESTINATION IS A REQUEST TO WRITE IT, not to set a preference. "Save Rawmap As"
         * is the only caller that passes one, and a destination that then does nothing is why the
         * save half looked broken. Restoring the default ("") asks for no write, so it is skipped.
         *
         * WRITE IT NOW if the map is already saved -- the bytes are complete in that save's map.decl
         * and need nothing from the engine. Only fall back to arming one editor save when there is no
         * saved map to read, which is the one case where only the editor can produce the bytes.
         * Sending someone off to save a map they have already saved was the wrong half of this. */
        if (save_path[0] != '\0') {
            char why[192];
            unsigned long long wrote = 0;

            /* THE LADDER, best first.
             *
             * 1. THE OPEN MAP. Ask the engine to serialize what is actually in the editor. This is
             *    the only rung that captures UNSAVED edits, and the only one that cannot export the
             *    wrong map. It queues onto the next editor frame, because serializing touches engine
             *    state -- so "accepted" here means it is about to happen, not that it has.
             *
             * 2. THE NEWEST SAVE ON DISK. What this used to do unconditionally. Correct only when
             *    the open map is saved and unmodified, and on a never-saved map it silently exports
             *    a DIFFERENT map -- so it is a fallback now, for when there is no editor to ask
             *    (no map open, or a build that could not resolve the serializer).
             *
             * 3. ARM THE NEXT SAVE. When neither can produce bytes, mirror the person's next
             *    editor save. Costs them one Save and is always the right map. */
            if (sh_editor_frame_request_rawmap_save(why, (int)sizeof why)) {
                if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                                       "Writing the open map to the rawmap file.", _TRUNCATE);
            /* NO "the editor is up but has no map" rung here, and that is a decision, not an
             * omission. It was written and removed: the Snapmap+ window only exists once you are IN
             * the editor, and a map is always live there, so the branch could not be reached from
             * this menu. The residual window it would have covered is a save landing exactly during
             * an editor transition (ED_SUBSTATE_OFF == 5) or shutdown, where rung 1 refuses and rung
             * 2 would read a different map off disk. If that is ever seen, this is the place. */
            } else if (dest_is_the_staged_source(save_path)) {
                /* Rung 2 reads the newest save off DISK, which is not necessarily the map the person
                 * has open -- and here the destination is the rawmap they staged for loading. Those
                 * two together are what destroyed a 214KB staged file once already: the wrong map's
                 * bytes written over the input. Rung 1 may do this freely (those are the open map's
                 * own bytes); rung 2 may not, so it drops through to arming the next real save. */
                backend_log("B1: rawmap SAVE skipped the disk fallback -- it would write the wrong "
                            "map over the staged load source");
                strncpy_s(why, sizeof why,
                          "that file is the rawmap you staged for loading, and no map is open to "
                          "save from", _TRUNCATE);
                sh_rawmap_save_arm_once();
                if (out_msg) {
                    _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE,
                                "%s -- it will be written on your next save.", why);
                }
            } else if (save_from_local_map(why, (int)sizeof why, &wrote)) {
                if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, why, _TRUNCATE);
            } else {
                sh_rawmap_save_arm_once();
                if (out_msg) {
                    _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE,
                                "%s -- it will be written on your next save.", why);
                }
            }
        }
    }

    /* 0/1 set the shared gate, -1 leaves it alone, and 2 arms ONE load and no more. The last is
     * what "Load Rawmap" wants: it scopes the substitution to the map the person is about to open,
     * where setting the gate would also substitute every map they opened afterwards -- and, because
     * the save shadow rides the same gate, redirect their next save too. */
    /* `arm` is a small verb code, not a boolean -- 2 already broke that fiction, and 3/4 extend it
     * rather than widen the ABI vtable, which the frontend and backend have to match slot for slot.
     *   -1 leave alone   0 gate off   1 gate on   2 arm ONE load   3 saves follow the rawmap   4 not */
    if (arm == 0 || arm == 1)  sh_rawmap_swap_arm(arm);
    else if (arm == 2)         sh_rawmap_load_arm_once();
    else if (arm == 3 || arm == 4) sh_rawmap_set_dest_follows_source(arm == 3);

    if (out_msg && out_msg[0] == '\0') {
        strncpy_s(out_msg, (size_t)msg_capacity, ok ? "ok" : "the save path was refused", _TRUNCATE);
    }
    return ok;
}

/* The half staging cannot do: make a map load HAPPEN, so the staged file actually opens. The engine
 * call and its frame-boundary discipline live in editor_frame.c; this is only the slot. */
static int slot_rawmap_load_now(sh_iface *self, char *out_msg, int msg_capacity)
{
    (void)self;
    return sh_editor_frame_request_reload(out_msg, msg_capacity);
}

void sh_rawmap_get_slots(sh_rawmap_status_fn *status, sh_rawmap_configure_fn *configure,
                         sh_rawmap_load_now_fn *load_now)
{
    if (status)    *status    = slot_rawmap_status;
    if (configure) *configure = slot_rawmap_configure;
    if (load_now)  *load_now  = slot_rawmap_load_now;
}
