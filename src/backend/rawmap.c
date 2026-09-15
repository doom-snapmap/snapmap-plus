#include "map_render.h"
/* DeserializeFromJson detour: optionally substitute a file-backed rawmap,
 * prepare the chosen JSON, then call the native parser. The temporary buffer
 * lives through the call. An unavailable source falls back to engine JSON.
 */
#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
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
#include "package_runtime.h"
#include "decl_server.h"
#include "map_transition.h"
#include "map_native.h"
#include "weapon_hud.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "nav_regions.h"
#include "map_shards.h"
#include "editor_frame.h"   /* sh_editor_frame_request_reload -- the +0x338 load-now slot */

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
static char *g_preflight_json;
static DWORD g_preflight_thread;
static int g_preflight_busy, g_preflight_consumed, g_preflight_rawmap;
static sh_mpkg_context *g_map_context;
static sh_mpkg_context *g_source_cleanup;
static int g_map_context_active;
static int g_map_context_await_departure;
static int g_map_cleanup_reported;
static __declspec(thread) unsigned g_inspection_depth;
static __declspec(thread) sh_rawmap_read_scope *g_initial_read;

void sh_rawmap_inspection_enter(void) { g_inspection_depth++; }
void sh_rawmap_inspection_leave(void)
{
    if (g_inspection_depth) g_inspection_depth--;
    else backend_log("MPKG: unbalanced map inspection scope");
}

void sh_rawmap_read_enter(sh_rawmap_read_scope *scope, const void *return_address)
{
    scope->previous = g_initial_read;
    scope->return_address = return_address;
    g_initial_read = scope;
    sh_rawmap_inspection_enter();
}
void sh_rawmap_read_leave(sh_rawmap_read_scope *scope)
{
    g_initial_read = scope->previous;
    sh_rawmap_inspection_leave();
}

/* Explicit load/save arm; default off. */
static volatile LONG g_gate = 0;
static volatile LONG g_swap_count = 0;
static volatile LONG g_swap_complete_count = 0;
static volatile LONG g_load_generation = 0;
static SRWLOCK g_paths_lock = SRWLOCK_INIT;

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
    AcquireSRWLockShared(&g_paths_lock);
    strncpy_s(out, cap, g_src_path, _TRUNCATE);
    ReleaseSRWLockShared(&g_paths_lock);
    if (!out[0]) default_source_path(out, cap);
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
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)INT_MAX) {
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
        sh_mpkg_report_error("Map load failed: package verification could not finish. Check the package diagnostic and try again.");
        return 0;
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

/* ------------------------------------------------------------------ "is this a branch map?" -----
 * The editor asks one small function whether the open map is a branch, and answers "Save over the
 * map you opened" or "Save asks for a name" from it. That function reads the map's tag list for
 * "map:branch".
 *
 * We answer it instead of writing the tag. Putting the tag in the map JSON does not work -- the
 * loader ignores the array and the map comes back with no tags -- and adding it to the live map
 * after the load grows that tag list under whatever already holds the array, which faults on a
 * later read. Answering costs the engine nothing: no engine data changes. */

/* Whether a map opened from a rawmap counts as a branch. On, the engine's own Save asks for a
 * name; off, it overwrites the map the rawmap opened over. */
static volatile LONG g_branch_tag    = 1;
static volatile LONG g_open_is_rawmap = 0;   /* 1 = the open map was substituted by the swap */

void sh_rawmap_set_branch_tag(int on) { InterlockedExchange(&g_branch_tag, on ? 1 : 0); }
int  sh_rawmap_branch_tag_enabled(void) { return InterlockedCompareExchange(&g_branch_tag, 0, 0) != 0; }

typedef unsigned char (*is_branch_map_fn)(void *editor);
static is_branch_map_fn g_isbranch_orig = NULL;

int sh_rawmap_load_is_safe(void)
{
    return !sh_rawmap_branch_tag_enabled() ||
           (g_isbranch_orig && hook_is_installed((void *)g_isbranch_orig));
}

static unsigned char sh_isbranch_detour(void *editor)
{
    if (InterlockedCompareExchange(&g_open_is_rawmap, 0, 0) != 0 &&
        InterlockedCompareExchange(&g_branch_tag, 0, 0) != 0)
        return 1;
    return g_isbranch_orig ? g_isbranch_orig(editor) : 0;
}

/* idSnapEditorLocal::IsBranchMap. Two functions in the image share its exact shape -- the same
 * body around a call to a different tag check -- so the pattern alone is ambiguous. The one we
 * want is the one whose callee names "map:branch", which is what this follows:
 *
 *     +0x10  E8 <rel32>          the tag check
 *     callee +0x?? 48 8D 15 ...  lea rdx, [the tag literal]
 */
#define ISBRANCH_STOLEN 14   /* through `test rcx,rcx`, before the short jump at +0x0E */

static int callee_names_branch(const unsigned char *site)
{
    __try {
        const unsigned char *callee, *p;
        int i;

        if (site[0x10] != 0xE8) return 0;
        callee = site + 0x15 + *(const int *)(site + 0x11);

        for (i = 0; i < 0x40; i++) {
            if (callee[i] == 0x48 && callee[i + 1] == 0x8D && callee[i + 2] == 0x15) {
                p = callee + i + 7 + *(const int *)(callee + i + 3);
                return strncmp((const char *)p, "map:branch", 11) == 0;
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* The 39-byte body, with only the call displacement wildcarded. The shared resolver refuses an
 * ambiguous pattern and returns no address, and this one matches twice by design, so the scan is
 * here where both candidates can be told apart. */
static const unsigned char ISBRANCH_PAT[] = {
    0x48,0x83,0xEC,0x28, 0x48,0x8B,0x89,0xC8,0x04,0x02,0x00, 0x48,0x85,0xC9, 0x74,0x10,
    0xE8,0x00,0x00,0x00,0x00, 0x84,0xC0, 0x74,0x07, 0xB0,0x01, 0x48,0x83,0xC4,0x28,0xC3,
    0x32,0xC0, 0x48,0x83,0xC4,0x28,0xC3
};
#define ISBRANCH_WILD_AT 17   /* the four rel32 bytes, which differ between builds */

static void *find_is_branch_map(const unsigned char *base)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        const IMAGE_NT_HEADERS64 *nt;
        const IMAGE_SECTION_HEADER *sec;
        unsigned i, k;

        if (base == NULL || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
        nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
        sec = IMAGE_FIRST_SECTION(nt);

        for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            const unsigned char *p;
            size_t n;
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            p = base + sec[i].VirtualAddress;
            n = sec[i].Misc.VirtualSize;
            if (n < sizeof ISBRANCH_PAT) continue;
            for (k = 0; k + sizeof ISBRANCH_PAT <= n; k++) {
                unsigned j;
                if (p[k] != ISBRANCH_PAT[0]) continue;
                for (j = 0; j < sizeof ISBRANCH_PAT; j++) {
                    if (j >= ISBRANCH_WILD_AT && j < ISBRANCH_WILD_AT + 4) continue;
                    if (p[k + j] != ISBRANCH_PAT[j]) break;
                }
                /* Both candidates match the bytes; only one calls the map:branch check. */
                if (j == sizeof ISBRANCH_PAT && callee_names_branch(p + k)) return (void *)(p + k);
            }
        }
        return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

int sh_rawmap_branch_install(const unsigned char *module_base)
{
    void *is_branch_fn, *tramp;

    if (g_isbranch_orig != NULL) return hook_is_installed((void *)g_isbranch_orig);

    is_branch_fn = find_is_branch_map(module_base);
    if (is_branch_fn == NULL) {
        backend_log("B1: branch answer SKIPPED -- IsBranchMap not found; rawmap loads "
                    "require explicit overwrite permission on this build");
        return 0;
    }

    tramp = hook_prepare(is_branch_fn, (void *)sh_isbranch_detour, ISBRANCH_STOLEN);
    if (tramp == NULL) {
        backend_log("B1: branch answer FAIL -- trampoline preparation failed");
        return 0;
    }
    g_isbranch_orig = (is_branch_map_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_isbranch_orig = NULL;
        backend_log("B1: branch answer commit failed");
        return 0;
    }
    backend_log("B1: branch answer installed -- a map opened from a rawmap saves as a new map");
    return 1;
}

/* Prepare the chosen JSON before native parsing: migrate markers, rebuild
 * map-scoped navigation state, then strip package and navigation envelopes.
 * Each failed strip retains the preceding buffer. Returns an owned process-
 * heap buffer or NULL to use the original.
 */
static char *prepare_map_buffer_mode(const char *json, int publish)
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

    if (publish) sh_navmesh_build_from_map(json, len);
    /* The regions the author marked, read from the same bytes and cleared the
     * same way. A map with no marked volume must not inherit the last map's. */
    if (publish) sh_nav_bake_set_map(json, len);

    pkg = mpkg_strip_guarded(json);
    if (!publish && !pkg && sh_mpkg_scan(json, len, NULL, 0)) {
        if (migrated) HeapFree(GetProcessHeap(), 0, migrated);
        return NULL;
    }
    nav = sh_navmesh_strip(pkg ? pkg : json, pkg ? strlen(pkg) : len, &nav_len);
    if (!nav && !pkg) return migrated;
    if (migrated) HeapFree(GetProcessHeap(), 0, migrated);
    if (!nav) return pkg;
    if (pkg) HeapFree(GetProcessHeap(), 0, pkg);
    return nav;
}
static char *prepare_map_buffer(const char *json)
{ return prepare_map_buffer_mode(json, 1); }

/* Use a readable armed source or the engine JSON. Both pass the package gate;
 * refusal returns the native parse-failure value, 0.
 */
static int select_map_policy(const char *json, size_t length)
{
    char error[1024] = "";
    sh_package_references references = {0};
    int ok = 0;
    __try {
        ok = sh_mpkg_prepare_map(json, length, &references) &&
             sh_package_runtime_select_map(json, length, &references, error, sizeof(error)) &&
             sh_weapon_hud_reload(NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(error, sizeof(error), "map package policy selection raised an exception");
    }
    sh_package_references_free(&references);
    if (!ok) sh_mpkg_report_error(error[0] ? error : "The map's package policy could not be activated.");
    return ok;
}

static int inspection_decode(const char *json, void *out_map, const void *caller)
{
    char *stripped = NULL;
    int result = 0;
    __try {
        size_t length = json ? strlen(json) : 0;
        if (g_initial_read && caller && g_initial_read->return_address == caller) {
            /* Spend before native serialization/parse can invoke nested hooks.
             * Stock maps also spend the scope but keep their normal decode. */
            g_initial_read->return_address = NULL;
            if (sh_mpkg_scan(json, length, NULL, 0)) {
                char error[512] = "";
                result = sh_map_native_read_session(json, out_map, error, sizeof(error));
                if (!result && error[0]) backend_log(error);
                goto done;
            }
        }
        stripped = prepare_map_buffer_mode(json, 0);
        /* A carrier or an inconclusive scan must not enter native metadata
         * with its delivery variables after a failed strip. */
        if (stripped || !sh_mpkg_scan(json, length, NULL, 0))
            result = g_deser_orig(stripped ? stripped : json, out_map);
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MPKG: read-only map decoding raised an exception");
    }
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    return result;
}

static int sh_deser_detour(const char *json, void *out_map)
{
    if (g_deser_orig == NULL) return 0;   /* defensive: should never happen once installed */
    if (g_inspection_depth) return inspection_decode(json, out_map, _ReturnAddress());
    sh_map_render_loaded(NULL);
    if (!select_map_policy(NULL, 0)) return 0;

    /* A map is being opened, so the file the person picked for the last one stops
     * applying. This is the single place that knows a map is actually opening --
     * staging one is not the same event, and clearing there would drop the choice
     * of someone who picked a file and then changed their mind about loading. */
    InterlockedIncrement(&g_load_generation);
    sh_rawmap_clear_save_target_for_new_map();

    if (g_preflight_json && g_preflight_thread == GetCurrentThreadId() && !g_preflight_consumed) {
        char *prepared;
        int rc;
        g_preflight_consumed = 1;
        prepared = prepare_map_buffer(g_preflight_json);
        const char *chosen = prepared ? prepared : g_preflight_json;
        rc = g_deser_orig(chosen, out_map);
        if (rc && !select_map_policy(chosen, strlen(chosen))) rc = 0;
        if (rc) sh_map_render_loaded(out_map);
        InterlockedExchange(&g_open_is_rawmap, g_preflight_rawmap);
        if (g_preflight_rawmap) InterlockedIncrement(&g_swap_complete_count);
        if (prepared) HeapFree(GetProcessHeap(), 0, prepared);
        return rc;
    }

    /* The shared predicate includes the explicit switch and test flag. */
    int flag_armed = 0;
    int oneshot    = sh_rawmap_load_oneshot_pending();
    if (rawmap_armed(&flag_armed) || oneshot) {
        if (!sh_rawmap_load_is_safe()) {
            backend_log("B1: rawmap load REFUSED -- overwrite protection is unavailable");
            return 0;
        }
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
                if (rc && !select_map_policy(prepared ? prepared : ours,
                                              prepared ? strlen(prepared) : len)) rc = 0;
                if (rc) sh_map_render_loaded(out_map);
                /* The open map came from a rawmap, which is what the branch answer keys off. */
                InterlockedExchange(&g_open_is_rawmap, 1);
                InterlockedIncrement(&g_swap_complete_count); /* only after the substituted parse returns */
                if (prepared) HeapFree(GetProcessHeap(), 0, prepared);
                HeapFree(GetProcessHeap(), 0, ours);
                return rc;
            }
        }
        /* Unreadable substitute: use the engine input. */
    }
    if (!mpkg_gate_guarded(json)) return 0;   /* refused: engine json never parsed */
    InterlockedExchange(&g_open_is_rawmap, 0);   /* the engine's own map: let the engine answer */
    {
        char *prepared = prepare_map_buffer(json);
        int rc;
        rc = g_deser_orig(prepared ? prepared : json, out_map);
        if (rc && !select_map_policy(prepared ? prepared : json,
                                      strlen(prepared ? prepared : json))) rc = 0;
        if (rc) sh_map_render_loaded(out_map);
        if (prepared) HeapFree(GetProcessHeap(), 0, prepared);
        return rc;
    }
}

int sh_rawmap_swap_install(void *deser_fn, int deser_status_ok)
{
    char line[200];

    if (g_deser_orig) {
        if (hook_is_installed((void *)g_deser_orig)) return 1;
        if (!hook_unpatch((void *)g_deser_orig)) return 0;
        g_deser_orig = NULL;
    }
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
    void *tramp = hook_prepare(deser_fn, (void *)sh_deser_detour, DESER_STOLEN);
    if (tramp == NULL) {
        backend_log("B1: rawmap LOAD-swap FAIL -- trampoline preparation failed");
        return 0;
    }
    g_deser_orig = (deser_fn_t)tramp;

    /* Keep defaults implicit; the source can already have been configured. */
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_deser_orig = NULL;
        backend_log("B1: rawmap LOAD-swap commit failed; retained callbacks require restoration");
        return 0;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: rawmap LOAD-swap installed at %p (trampoline %p, stolen %d); source=%s; gate=DISARMED",
        deser_fn, tramp, DESER_STOLEN, "configured rawmap source");
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
    if (path && strlen(path) >= MAX_PATH) return 0;
    AcquireSRWLockExclusive(&g_paths_lock);
    strcpy_s(g_src_path, sizeof g_src_path, path ? path : "");
    ReleaseSRWLockExclusive(&g_paths_lock);
    return 1;
}

unsigned long sh_rawmap_load_generation(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_load_generation, 0, 0);
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
static __declspec(thread) sh_rawmap_snapshot_visit g_snapshot_visit;
static __declspec(thread) void *g_snapshot_visit_ctx;
static __declspec(thread) int g_snapshot_visited;

int sh_rawmap_snapshot(void *editor_serializer, void *map, void *out_idstr)
{
    return sh_rawmap_snapshot_inspect(editor_serializer, map, out_idstr, NULL, NULL);
}

int sh_rawmap_snapshot_inspect(void *editor_serializer, void *map, void *out_idstr,
                              sh_rawmap_snapshot_visit visit, void *ctx)
{
    int ok=0;
    sh_rawmap_snapshot_visit previous = g_snapshot_visit;
    void *previous_ctx = g_snapshot_visit_ctx;
    int previous_visited = g_snapshot_visited;
    if (!g_ser_orig || !editor_serializer || !map || !out_idstr) return 0;
    g_snapshot_visit = visit; g_snapshot_visit_ctx = ctx; g_snapshot_visited = 0;
    g_snapshot_depth++;
    __try {
        ok=((serialize_fn_t)editor_serializer)(map,out_idstr,0)!=0;
        if (visit && !g_snapshot_visited) ok = 0;
    }
    __finally {
        g_snapshot_depth--;
        g_snapshot_visit = previous; g_snapshot_visit_ctx = previous_ctx;
        g_snapshot_visited = previous_visited;
    }
    return ok;
}

static volatile LONG     g_shadow_count = 0;
static volatile LONGLONG g_last_bytes   = 0;

/* WHERE SAVES GO.
 *
 * One path, belonging to the map that is open. Empty means the default
 * %LOCALAPPDATA%\snapmap-plus\rawmap.json. "Save Rawmap As" sets it, so a later
 * "Save Rawmap" writes to the same file; opening a different map clears it.
 *
 * The lifetime is the point. A rawmap someone downloads and opens is an archive
 * entry, so nothing about opening one may aim a save at it -- only picking it in
 * Save As does that, for the map in front of them, and not for the next one. */
static char g_save_target[MAX_PATH] = {0};

static void default_dest_path(char *out, size_t cap)
{
    /* Use the shared application data root. */
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\rawmap.json", base);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snapmap-plus\\rawmap.json");
}

/* Defined further down, next to the status JSON that is its other caller. */
static int json_escape_into(char *out, size_t cap, const char *src);

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

/* Point saves at `path`. "" or NULL goes back to the default rawmap.json.
 * Refuses a path whose folder does not exist, so a bare word cannot become a
 * file in DOOM's own install folder. */
int sh_rawmap_set_save_target(const char *path)
{
    char chosen[MAX_PATH] = "", dflt[MAX_PATH];
    if (path && path[0]) {
        if (strlen(path) >= MAX_PATH || !dest_folder_is_usable(path, NULL, 0)) return 0;
        default_dest_path(dflt, sizeof dflt);
        if (_stricmp(path, dflt) != 0) strcpy_s(chosen, sizeof chosen, path);
    }
    AcquireSRWLockExclusive(&g_paths_lock);
    strcpy_s(g_save_target, sizeof g_save_target, chosen);
    ReleaseSRWLockExclusive(&g_paths_lock);
    return 1;
}

void sh_rawmap_get_save_target(char *out, int cap)
{
    if (!out || cap <= 0) return;
    AcquireSRWLockShared(&g_paths_lock);
    strncpy_s(out, (size_t)cap, g_save_target, _TRUNCATE);
    ReleaseSRWLockShared(&g_paths_lock);
}

static void resolve_dest_path(char *out, size_t cap)
{
    sh_rawmap_get_save_target(out, (int)cap);
    if (!out[0]) default_dest_path(out, cap);
}

void sh_rawmap_clear_save_target_for_new_map(void)
{
    sh_rawmap_set_save_target(NULL);
}

/* Write `len` bytes from `data` to the shadow destination ("wb", truncate). Returns the byte count
 * written, or 0 on any failure. SEH-free here (pure Win32 file ops on a validated buffer); the engine
 * out-idStr read in the detour is SEH-guarded by the caller. */
/* Defined below, next to the shadow that shares it: one writer for both the shadow and the
 * live path, so the destination is resolved in exactly one place. */
static unsigned long long write_shadow(const char *data, size_t len);
static unsigned long long write_shadow_to(const char *data, size_t len, const char *path);

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

typedef int (*preflight_load_fn)(void *editor, const void *save_name);
typedef int (*saved_map_text_fn)(const char *save_id, void *out_string, int flags, int slot);
static preflight_load_fn g_preflight_orig;
static saved_map_text_fn g_saved_map_text;
static struct {
    char *json;
    char save_id[21], root[MAX_PATH];
    void *editor;
    DWORD thread;
    int raw, state; /* 1 consent, 2 published, 3 canceled/finished, 4 native waiting, 5 loading */
    ULONGLONG published_at;
    sh_rawmap_request request;
    sh_rawmap_loading loading;
    sh_mpkg_context *sources;
    sh_package_map_plan *plan;
} g_pending_map;
static int g_transition_previous_busy;
static char g_transition_error[2048];

static int preflight_abort_saved_load(void *editor)
{
    if (!editor || !sh_decl_server_map_editor_matches(editor) ||
        GetCurrentThreadId() != g_pending_map.thread) return 0;
    __try {
        unsigned char *native = editor;
        /* EditorLoadMap's own saved-read failure sets this status and leaves
         * through SetActive(false). The same native cleanup cancels a later
         * install-commit failure without issuing another load or a reset. */
        if (native[9]) {
            *(int *)(native + 0x2366c) = 1;
            ((void (*)(void *, int))(*(void ***)editor)[0x48 / sizeof(void *)])(editor, 0);
        }
        return native[9] == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int (*g_preflight_abort)(void *) = preflight_abort_saved_load;

static int preflight_save_id(const void *save_name, char id[21])
{
    __try {
        const char *name = *(const char *const *)((const unsigned char *)save_name + 0x10);
        for (size_t i = 0; i < 20; i++) {
            char c = name[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
            id[i] = c;
        }
        id[20] = 0; return !name[20];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void preflight_install_completed(void *context, int outcome)
{
    if (context != &g_pending_map || !g_pending_map.state) return;
    g_pending_map.state = outcome == 1 ? 2 : 3;
    g_pending_map.published_at = GetTickCount64();
}

static int preflight_pending_discard(void)
{
    if (!g_pending_map.state) return 1;
    sh_mpkg_cancel_map_consent(&g_pending_map);
    if (!sh_mpkg_activation_cancel()) return 0;
    sh_package_map_plan_free(g_pending_map.plan); g_pending_map.plan = NULL;
    if (g_pending_map.sources && !sh_mpkg_context_close(&g_pending_map.sources)) return 0;
    if (g_pending_map.json) HeapFree(GetProcessHeap(), 0, g_pending_map.json);
    if (g_pending_map.request.release) {
        sh_rawmap_request request = g_pending_map.request;
        memset(&g_pending_map.request, 0, sizeof(g_pending_map.request));
        g_pending_map.json = NULL;
        __try { request.release(request.context); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            backend_log("MPKG: retained map request cleanup raised an exception");
        }
    }
    memset(&g_pending_map, 0, sizeof(g_pending_map)); return 1;
}

static void preflight_retire_context(void)
{
    if (!g_map_context) return;
    g_map_context_await_departure = 0;
    if (g_map_context_active) {
        if (!sh_decl_server_activate_map(NULL)) return;
        g_map_context_active = 0;
    }
    if (sh_mpkg_context_close(&g_map_context)) g_map_cleanup_reported = 0;
    else if (!g_map_cleanup_reported) {
        g_map_cleanup_reported = 1;
        backend_log("MPKG: retired map sources remain held; cleanup will retry");
    }
}

int sh_rawmap_cancel_pending_map(void)
{
    if (g_preflight_busy || (g_pending_map.state && g_pending_map.thread != GetCurrentThreadId())) return 0;
    if (g_pending_map.state) g_pending_map.state = 3;
    return preflight_pending_discard();
}

void sh_rawmap_map_context_poll(void)
{
    if (g_preflight_busy || sh_rawmap_defers_install_commit()) return;
    if (g_source_cleanup) (void)sh_mpkg_context_close(&g_source_cleanup);
    if (g_map_context_await_departure) {
        if (sh_decl_server_map_browser_present()) return;
        g_map_context_await_departure = 0;
    }
    if (!sh_decl_server_map_retirement_safe()) return;
    preflight_retire_context();
}

/* Native saved-text reading is read-only and occurs on the same main-thread
 * boundary as the original load request. idStr ownership never crosses calls. */
static char *preflight_saved_json(const void *save_name)
{
    unsigned char text[IDSTR_SIZE] = {0};
    char id[21], *copy = NULL;
    int initialized = 0, valid = 1;
    __try {
        valid = preflight_save_id(save_name, id);
        if (valid) {
            g_idstr_ctor(text, ""); initialized = 1;
            if (g_saved_map_text(id, text, 0, -1) == 0) {
                int length = *(int *)(text + 8);
                const char *body = *(const char **)(text + 0x10);
                if (length > 0 && body && !body[length] && !memchr(body, 0, (size_t)length)) {
                    copy = HeapAlloc(GetProcessHeap(), 0, (size_t)length + 1);
                    if (copy) memcpy(copy, body, (size_t)length + 1);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { valid = 0; }
    if (initialized) {
        __try { g_idstr_dtor(text); }
        __except (EXCEPTION_EXECUTE_HANDLER) { valid = 0; }
    }
    if (!valid && copy) { HeapFree(GetProcessHeap(), 0, copy); copy = NULL; }
    return copy;
}

/* Full supplied-payload coverage is sufficient to avoid reinstalling its
 * packages. Dependency discovery has a separate job: selecting packages and
 * diagnosing assets the author did not supply. A known-path subset cannot
 * establish this result. Return 1 covered, 0 missing, -1 failed inspection. */
static int preflight_resource_availability(sh_package_map_plan *plan, sh_package_missing *missing,
    char *error, size_t capacity)
{
    char line[1400];
    int ok = 0, result = -1;
    __try {
        ok = sh_package_map_plan_payload_missing(plan, missing, error, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, capacity, "payload inspection raised exception %08lx", (unsigned long)GetExceptionCode());
    }
    if (ok) {
        result = missing->count ? 0 : 1;
        snprintf(line, sizeof(line), "MPKG: compiled payload availability: %zu checked, %zu missing, %zu supplying packages",
            missing->checked, missing->count, sh_package_owners_count(&missing->packages));
    }
    else snprintf(line, sizeof(line), "MPKG: resource availability inspection incomplete: %s",
        error[0] ? error : "compiled payload unavailable");
    backend_log(line);
    for (size_t i = 0; i < missing->count; i++) {
        snprintf(line, sizeof(line), "MPKG: missing supplied resource: %s", missing->paths[i]); backend_log(line);
    }
    return result;
}

void sh_rawmap_pending_map_poll(void)
{
    unsigned char name[IDSTR_SIZE] = {0};
    char error[2048] = "";
    int initialized = 0, entered_saved = 0;
    /* Read/completion callbacks can pump the menu while a preview decode is
     * still on the stack. Enter only after that read-only scope has unwound,
     * or the real load would be mistaken for another inspection. */
    if (!g_pending_map.state || g_preflight_busy || g_inspection_depth) return;
    if (g_pending_map.request.valid && g_pending_map.state != 3) {
        int valid = 0;
        __try {
            valid = GetCurrentThreadId() == g_pending_map.thread &&
                (g_pending_map.state == 4 ? g_pending_map.loading.waiting(g_pending_map.request.context) :
                    g_pending_map.request.valid(g_pending_map.request.context));
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        if (!valid) g_pending_map.state = 3;
    }
    if (g_pending_map.state == 3) { preflight_pending_discard(); return; }
    if (g_pending_map.state != 2) return;
    if (g_pending_map.loading.matches) {
        if (!sh_decl_server_map_preparation_ready()) return;
        g_preflight_busy = 1;
        __try {
            if (g_pending_map.plan && !sh_package_map_plan_prepare_inventory(g_pending_map.plan,
                g_pending_map.root, error, sizeof(error))) g_pending_map.state = 3;
            else {
                /* Retain sources and owner through asynchronous lobby waiting.
                 * Native entry decodes a preview; activation owns publication. */
                g_pending_map.state = 4;
                if (g_pending_map.request.enter(g_pending_map.request.context, g_pending_map.json)) {
                    strcpy_s(error, sizeof(error), "The native map request could not start.");
                    g_pending_map.state = 3;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            snprintf(error, sizeof(error), "Native map continuation raised exception %08lx.", GetExceptionCode());
            g_pending_map.state = 3;
        }
        g_preflight_busy = 0;
        if (g_pending_map.state == 3) preflight_pending_discard();
        if (error[0]) sh_mpkg_report_error(error);
        return;
    }
    if (!sh_decl_server_map_boundary_safe()) {
        if (GetTickCount64() - g_pending_map.published_at > 30000) {
            g_pending_map.state = 3; preflight_pending_discard();
        }
        return;
    }
    g_preflight_busy = 1;
    __try {
        if (GetCurrentThreadId() != g_pending_map.thread ||
            (!g_pending_map.request.enter && !sh_decl_server_map_editor_matches(g_pending_map.editor))) {
            strcpy_s(error, sizeof(error), "The original map load is no longer available. Installation canceled."); goto done;
        }
        if (!g_pending_map.request.enter) {
            g_idstr_ctor(name, g_pending_map.save_id); initialized = 1;
        }
        if (g_pending_map.plan) {
            if (!sh_package_map_plan_prepare_inventory(g_pending_map.plan, g_pending_map.root, error, sizeof(error))) goto done;
            if (!sh_decl_server_activate_map(g_pending_map.plan)) {
                sh_package_runtime_error(error, sizeof(error)); goto done;
            }
            g_map_context = g_pending_map.sources; g_pending_map.sources = NULL; g_map_context_active = 1;
        }
        g_preflight_json = g_pending_map.json; g_preflight_thread = GetCurrentThreadId();
        g_preflight_consumed = 0; g_preflight_rawmap = g_pending_map.raw;
        backend_log("MPKG: installation activated; continuing the original map load with its retained source");
        if (g_pending_map.request.enter) {
            if (g_pending_map.request.enter(g_pending_map.request.context, g_pending_map.json))
                strcpy_s(error, sizeof(error), "The prepared map could not finish loading. Check the package log.");
            else if (g_map_context_active)
                g_map_context_await_departure = sh_decl_server_map_browser_present();
        } else {
            entered_saved = 1;
            if (g_preflight_orig(g_pending_map.editor, name))
                strcpy_s(error, sizeof(error), "The installed map could not finish loading. Check the package log.");
        }
        if (!error[0] && !g_preflight_consumed)
            strcpy_s(error, sizeof(error), "The requested map was not loaded. Installation canceled.");
        if (!error[0] && !sh_package_runtime_commit_map(g_pending_map.plan,
            sh_mpkg_activation_commit, error, sizeof(error)) && !error[0])
            strcpy_s(error, sizeof(error), "The map installation could not be committed.");
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, sizeof(error), "Map installation continuation raised exception %08lx.", (unsigned long)GetExceptionCode());
    }
    g_preflight_json = NULL; g_preflight_thread = 0;
    if (error[0] && entered_saved && !g_preflight_abort(g_pending_map.editor))
        backend_log("MPKG: native saved-map cancellation did not complete; its resource context remains held");
    if (initialized) {
        __try { g_idstr_dtor(name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MPKG: saved-load string cleanup failed"); }
    }
    g_pending_map.state = 3; preflight_pending_discard(); g_preflight_busy = 0;
    if (error[0] && entered_saved) sh_rawmap_map_context_poll();
    if (error[0]) sh_mpkg_report_error(error);
}

int sh_rawmap_transition_pending(void *manager, const void *parameters)
{
    int matches = 0, valid = 0;
    (void)manager;
    if (g_pending_map.state != 4 || !g_pending_map.loading.matches ||
        GetCurrentThreadId() != g_pending_map.thread) return 0;
    __try { matches = g_pending_map.loading.matches(g_pending_map.request.context, parameters); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (!matches) return 0;
    g_pending_map.state = 5;
    g_transition_previous_busy = g_preflight_busy; g_preflight_busy = 1;
    g_transition_error[0] = 0;
    __try { valid = g_pending_map.loading.waiting(g_pending_map.request.context); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (!valid) strcpy_s(g_transition_error, sizeof(g_transition_error), "The pending map selection was canceled.");
    return valid ? 1 : -1;
}

int sh_rawmap_transition_activate(void)
{
    char *prepared = NULL;
    int ok = 0;
    if (g_pending_map.state != 5 || GetCurrentThreadId() != g_pending_map.thread ||
        !sh_map_transition_at_boundary()) return 0;
    __try {
        sh_mpkg_context *previous;
        const char *chosen;
        if (g_source_cleanup && !sh_mpkg_context_close(&g_source_cleanup)) {
            strcpy_s(g_transition_error, sizeof(g_transition_error), "Previous package cleanup is still pending."); goto done;
        }
        if (!sh_decl_server_activate_map(g_pending_map.plan)) {
            sh_package_runtime_error(g_transition_error, sizeof(g_transition_error)); goto done;
        }
        /* Replace providers directly. Compiling an intermediate local view
         * could reject unrelated stored variants or expose local map values. */
        previous = g_map_context;
        g_map_context = g_pending_map.sources; g_pending_map.sources = NULL;
        g_map_context_active = g_pending_map.plan != NULL;
        g_map_context_await_departure = 0;
        if (previous && !sh_mpkg_context_close(&previous)) g_source_cleanup = previous;
        prepared = prepare_map_buffer(g_pending_map.json);
        chosen = prepared ? prepared : g_pending_map.json;
        if (!select_map_policy(chosen, strlen(chosen)) ||
            !g_pending_map.loading.select(g_pending_map.request.context)) goto done;
        InterlockedIncrement(&g_load_generation);
        sh_rawmap_clear_save_target_for_new_map();
        InterlockedExchange(&g_open_is_rawmap, 0);
        ok = 1;
done:;
    } __finally {
        if (prepared) HeapFree(GetProcessHeap(), 0, prepared);
    }
    if (!ok && !g_transition_error[0]) strcpy_s(g_transition_error, sizeof(g_transition_error),
        "The map's packages could not activate. The map load was canceled.");
    return ok;
}

int sh_rawmap_transition_commit(void)
{
    if (g_pending_map.state != 5 || GetCurrentThreadId() != g_pending_map.thread) return 0;
    return sh_package_runtime_commit_map(g_pending_map.plan, sh_mpkg_activation_commit,
        g_transition_error, sizeof(g_transition_error));
}

int sh_rawmap_defers_install_commit(void)
{
    return g_pending_map.state != 0;
}

void sh_rawmap_transition_finished(int loaded)
{
    if (g_pending_map.state != 5 || GetCurrentThreadId() != g_pending_map.thread) return;
    g_pending_map.state = 3;
    g_preflight_busy = g_transition_previous_busy;
    if (!g_preflight_busy) preflight_pending_discard();
    if (!loaded) sh_mpkg_report_error(g_transition_error[0] ? g_transition_error :
        "The map load did not complete. Its pending installation was canceled.");
}

static int preflight_request(const char *json, size_t length,
    const sh_rawmap_request *request, const sh_rawmap_loading *loading,
    char *error, size_t capacity)
{
    char root[MAX_PATH];
    char *copy = NULL;
    sh_mpkg_context *sources = NULL;
    sh_package_map_plan *plan = NULL;
    sh_package_missing missing = {0};
    int accepted = 0, covered = 1, published = 0;
    if (!error || !capacity) return 0;
    error[0] = 0;
    if (!request || !request->valid || !request->enter || !request->release ||
        !json || !length || length > INT_MAX || !g_deser_orig || g_preflight_busy ||
        (loading ? (!loading->waiting || !loading->matches || !loading->select ||
            !sh_map_transition_ready() || !sh_decl_server_map_preparation_ready()) :
            !sh_decl_server_map_boundary_safe())) {
        snprintf(error, capacity, "The requested map cannot be prepared at this boundary."); return 0;
    }
    g_preflight_busy = 1;
    __try {
        if (!request->valid(request->context) || memchr(json, 0, length)) {
            snprintf(error, capacity, "The map request or its source is no longer valid."); goto done;
        }
        if (!preflight_pending_discard()) {
            snprintf(error, capacity, "The previous installation could not be canceled."); goto done;
        }
        if (g_source_cleanup && !sh_mpkg_context_close(&g_source_cleanup)) {
            snprintf(error, capacity, "Previous private package cleanup is still pending."); goto done;
        }
        if (!loading) preflight_retire_context();
        if (!loading && g_map_context) {
            snprintf(error, capacity, "The previous map's resources could not be retired."); goto done;
        }
        copy = HeapAlloc(GetProcessHeap(), 0, length + 1);
        if (!copy) { snprintf(error, capacity, "Map source allocation failed."); goto done; }
        memcpy(copy, json, length); copy[length] = 0;
        if (!sh_overrides_get_root(root, sizeof(root))) {
            snprintf(error, capacity, "The package data directory is unavailable."); goto done;
        }
        sources = sh_mpkg_context_open(root, copy, length, error, capacity);
        if (!sources) goto done;
        if (sh_mpkg_context_count(sources)) {
            plan = sh_package_runtime_prepare_map(root, sh_mpkg_context_root(sources), error, capacity);
            if (!plan) goto done;
            covered = preflight_resource_availability(plan, &missing, error, capacity);
            if (covered < 0) goto done;
        }
        if (covered && !sh_mpkg_activation_ready()) {
            snprintf(error, capacity, "The package runtime is not ready for this map."); goto done;
        }
        g_pending_map.json = copy; copy = NULL;
        g_pending_map.plan = plan; plan = NULL;
        g_pending_map.sources = sources; sources = NULL;
        g_pending_map.thread = GetCurrentThreadId(); g_pending_map.request = *request;
        if (loading) g_pending_map.loading = *loading;
        published = 1;
        strcpy_s(g_pending_map.root, sizeof(g_pending_map.root), root);
        g_pending_map.state = covered ? 2 : 1;
        g_pending_map.published_at = GetTickCount64();
        if (!covered && !sh_mpkg_request_map_install(g_pending_map.json, length,
            sh_package_map_plan_compilation(g_pending_map.plan), &missing.packages,
            preflight_install_completed, &g_pending_map, error, capacity)) {
            /* The install service accepted no request. Keep caller ownership. */
            memset(&g_pending_map.request, 0, sizeof(g_pending_map.request));
            g_pending_map.state = 3; preflight_pending_discard(); goto done;
        }
        accepted = 1;
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, capacity, "Map request preparation raised exception %08lx.", (unsigned long)GetExceptionCode());
        if (published && g_pending_map.request.enter) {
            /* Ownership was published before calling an external service. */
            accepted = 1; g_pending_map.state = 3;
        }
    }
    if (copy) HeapFree(GetProcessHeap(), 0, copy);
    sh_package_map_plan_free(plan); sh_package_missing_free(&missing);
    if (sources && !sh_mpkg_context_close(&sources)) g_source_cleanup = sources;
    g_preflight_busy = 0;
    if (accepted && error[0]) sh_mpkg_report_error(error);
    return accepted;
}
int sh_rawmap_preflight_request(const char *json, size_t length,
    const sh_rawmap_request *request, char *error, size_t capacity)
{ return preflight_request(json, length, request, NULL, error, capacity); }
int sh_rawmap_preflight_loading_request(const char *json, size_t length,
    const sh_rawmap_request *request, const sh_rawmap_loading *loading,
    char *error, size_t capacity)
{
    if (!loading) {
        if (error && capacity) snprintf(error, capacity, "The native map continuation is unavailable.");
        return 0;
    }
    return preflight_request(json, length, request, loading, error, capacity);
}

static int preflight_load(void *editor, const void *save_name)
{
    char root[MAX_PATH], error[2048] = "";
    char *json = NULL;
    sh_mpkg_context *sources = NULL;
    sh_package_map_plan *plan = NULL;
    sh_package_missing missing = {0};
    int result = 0, raw = 0, flag = 0, oneshot, covered = 0;
    size_t length = 0;
    if (!g_preflight_orig) return 1;
    if (!g_preflight_busy && g_pending_map.state && !preflight_pending_discard()) return 0;
    if (g_preflight_busy || !sh_decl_server_map_boundary_safe()) return g_preflight_orig(editor, save_name);
    sh_rawmap_map_context_poll();
    if (g_map_context) {
        sh_mpkg_report_error("The previous map's resources could not be retired. Check the package log."); return 0;
    }
    g_preflight_busy = 1;
    __try {
        oneshot = sh_rawmap_load_oneshot_pending();
        if (rawmap_armed(&flag) || oneshot) {
            if (!sh_rawmap_load_is_safe()) {
                strcpy_s(error, sizeof(error), "Rawmap overwrite protection is unavailable."); goto done;
            }
            json = read_source_file(&length);
            if (json) {
                raw = 1;
                if (oneshot) InterlockedExchange(&g_swap_oneshot, 0);
                InterlockedIncrement(&g_swap_count);
            }
        }
        if (!json) json = preflight_saved_json(save_name);
        if (!json) {
            strcpy_s(error, sizeof(error), "The saved map could not be read for package preparation."); goto done;
        }
        length = strlen(json);
        if (!sh_overrides_get_root(root, sizeof(root))) {
            strcpy_s(error, sizeof(error), "The package data directory is unavailable."); goto done;
        }
        sources = sh_mpkg_context_open(root, json, length, error, sizeof(error));
        if (!sources) goto done;
        if (sh_mpkg_context_count(sources)) {
            plan = sh_package_runtime_prepare_map(root, sh_mpkg_context_root(sources), error, sizeof(error));
            if (!plan) goto done;
            covered = preflight_resource_availability(plan, &missing, error, sizeof(error));
            if (covered < 0) goto done;
        }
        if (plan && covered) {
            if (!sh_mpkg_activation_ready()) goto done;
            backend_log("MPKG: all supplied resources are available; activating map values without package installation");
        } else if (plan) {
            if (!sh_decl_server_map_editor_matches(editor) || !preflight_save_id(save_name, g_pending_map.save_id)) {
                strcpy_s(error, sizeof(error), "The original map request cannot be retained for installation."); goto done;
            }
            g_pending_map.json = json; json = NULL;
            g_pending_map.plan = plan; plan = NULL;
            g_pending_map.sources = sources; sources = NULL;
            g_pending_map.editor = editor; g_pending_map.thread = GetCurrentThreadId();
            g_pending_map.raw = raw; g_pending_map.state = 1;
            strcpy_s(g_pending_map.root, sizeof(g_pending_map.root), root);
            if (!sh_mpkg_request_map_install(g_pending_map.json, length,
                sh_package_map_plan_compilation(g_pending_map.plan), &missing.packages,
                preflight_install_completed, &g_pending_map, error, sizeof(error))) preflight_pending_discard();
            goto done;
        } else if (!mpkg_gate_guarded(json)) goto done;
        if (plan) {
            if (!sh_decl_server_activate_map(plan)) {
                sh_package_runtime_error(error, sizeof(error)); goto done;
            }
            g_map_context = sources; sources = NULL; g_map_context_active = 1;
        }
        g_preflight_json = json; g_preflight_thread = GetCurrentThreadId();
        g_preflight_consumed = 0; g_preflight_rawmap = raw;
        backend_log("MPKG: saved-map preflight complete; native load will parse the retained JSON");
        result = g_preflight_orig(editor, save_name);
done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(error, sizeof(error), "Saved-map package preparation raised exception %08lx.", (unsigned long)GetExceptionCode());
        result = 1;
    }
    g_preflight_json = NULL; g_preflight_thread = 0;
    if (json) HeapFree(GetProcessHeap(), 0, json);
    sh_package_map_plan_free(plan);
    sh_package_missing_free(&missing);
    if (sources && !sh_mpkg_context_close(&sources)) g_map_context = sources;
    g_preflight_busy = 0;
    if (error[0]) sh_mpkg_report_error(error);
    return result;
}

int sh_rawmap_preflight_install(void *load_fn, int load_clean, void *read_text_fn, int read_clean)
{
    void *trampoline;
    if (g_preflight_orig) {
        if (hook_is_installed((void *)g_preflight_orig)) return 1;
        if (!hook_unpatch((void *)g_preflight_orig)) return 0;
        g_preflight_orig = NULL; g_saved_map_text = NULL;
    }
    if (!load_fn || !load_clean || !read_text_fn || !read_clean || !g_deser_orig || !g_idstr_ctor || !g_idstr_dtor) {
        backend_log("MPKG: saved-map preflight unavailable; clean load/read signatures and string helpers required"); return 0;
    }
    /* EditorLoadMap's verified signature spans 36 bytes of complete,
     * position-independent register/stack instructions on both renderers. */
    trampoline = hook_prepare(load_fn, (void *)preflight_load, 36);
    if (!trampoline) return 0;
    g_preflight_orig = (preflight_load_fn)trampoline; g_saved_map_text = (saved_map_text_fn)read_text_fn;
    if (hook_commit(trampoline) != B2_PATCH_OK) {
        if (hook_unpatch(trampoline)) { g_preflight_orig = NULL; g_saved_map_text = NULL; }
        return 0;
    }
    backend_log("MPKG: saved-map preflight installed before editor initialization"); return 1;
}

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

/* Defined below, beside the save detour that is their other caller. Both writers put a
 * map through the same encoders, so a rawmap carries what an engine save carries. */
static int mpkg_embed_on_save(void *out_idstr);
static void nav_embed_on_save(void *out_idstr);
static void nav_regions_on_save(void *out_idstr);

/* Serialize `map` and write it to the rawmap destination. MAIN THREAD ONLY -- it reads engine state
 * and allocates through the engine's allocator, so it is called from the editor-frame hook, never
 * from the UI thread. */
int sh_rawmap_write_from_live(void *map, const char *destination, char *out_msg, int msg_capacity,
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
        /* SnapMapToJson calls the hooked serializer. Suppress its save side effects
         * here so the explicit export embeds and writes exactly once. */
        g_snapshot_depth++;
        __try { rc = g_map_to_json(map, blk, 1); }
        __finally { g_snapshot_depth--; }
        if (rc) {
            /* The same three encoders the save detour runs, in the same order. The engine
             * hands back map state with the shards already stripped at load, so writing
             * this buffer straight out drops the packages the map uses and the navigation
             * its author baked. Each one no-ops if its own dependencies are unresolved. */
            rc = (unsigned char)mpkg_embed_on_save(blk);
            nav_embed_on_save(blk);
            nav_regions_on_save(blk);

            len  = *(const int *)(blk + IDSTR_LEN_OFF);
            data = *(const char *const *)(blk + IDSTR_DATA_OFF);
            if (rc && data != NULL && len > 0) wrote = write_shadow_to(data, (size_t)len, destination);
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
        strncpy_s(path, sizeof path, destination, _TRUNCATE);
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
        strncpy_s(path, sizeof path, destination, _TRUNCATE);
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

static unsigned long long write_shadow_to(const char *data, size_t len, const char *path)
{
    static volatile LONG sequence;
    char temp[MAX_PATH], dir[MAX_PATH];
    size_t total = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    int attempt, flushed = 0;

    if (!data || !len || !path || !path[0]) return 0;
    if (!dest_dir_part(path, dir, sizeof dir) || !ensure_dir(dir)) return 0;
    /* CREATE_NEW never truncates someone else's scratch file. Separate writers
     * must own separate temporaries until each publishes its complete result. */
    for (attempt = 0; attempt < 64; ++attempt) {
        if (_snprintf_s(temp, sizeof temp, _TRUNCATE, "%s.smp-%lu-%lu.tmp", path,
                        GetCurrentProcessId(), (unsigned long)InterlockedIncrement(&sequence)) < 0)
            return 0;
        h = CreateFileA(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    }
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (total < len) {
        DWORD wr = 0;
        DWORD chunk = (DWORD)((len - total) > 0x10000000 ? 0x10000000 : len - total);
        if (!WriteFile(h, data + total, chunk, &wr, NULL) || !wr) break;
        total += wr;
    }
    if (total == len) flushed = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (total != len || !flushed ||
        !MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temp);
        backend_log("B1: rawmap write failed; the previous destination is unchanged");
        return 0;
    }
    return (unsigned long long)total;
}

static unsigned long long write_shadow(const char *data, size_t len)
{
    char path[MAX_PATH];
    resolve_dest_path(path, sizeof path);
    return write_shadow_to(data, len, path);
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
static char *embed_used_packages(const char *json, size_t len, size_t *out_len, int *failed)
{
    sh_mpkg_used *used = NULL;
    char root[MAX_PATH];
    char *cur = NULL;
    size_t cur_len = len, count, i;
    *failed = 0;
    *out_len = 0;
    if (!sh_overrides_get_root(root, sizeof root)) return NULL;

    count = sh_mpkg_used_packages(json, len, root, &used);
    if (count == SIZE_MAX) { *failed = 1; sh_mpkg_report_error("Map save failed: package usage could not be resolved completely. Check the package compiler diagnostic."); return NULL; }
    if (count == 0) { free(used); return NULL; }

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
                        "not saved", used[i].id, err);
            sh_mpkg_report_error(line);
            *failed = 1; free(used); if (cur) HeapFree(GetProcessHeap(), 0, cur); return NULL;
        }
        next = sh_mpkg_embed(cur ? cur : json, cur_len, used[i].id,
                             payload, payload_len, &next_len, err, sizeof err);
        HeapFree(GetProcessHeap(), 0, payload);
        if (!next) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: package '%s' could NOT be embedded in this save (%s); the map is "
                        "not saved", used[i].id, err);
            sh_mpkg_report_error(line);
            *failed = 1; free(used); if (cur) HeapFree(GetProcessHeap(), 0, cur); return NULL;
        }
        if (cur) HeapFree(GetProcessHeap(), 0, cur);
        cur = next;
        cur_len = next_len;
    }

    free(used);
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

/* SaveAndPlay is the editor UI command, before it closes menus or requests
 * deactivation. Later allocation/build failures become fatal engine errors.
 * Read the live editor map through SnapMapToJson while refusal can still leave
 * the editor intact. Verified at Vulkan 537070 and OpenGL 536940. */
#define PLAY_MAP_OFF 0x204c8
#define PLAY_STOLEN 19
typedef void (*editor_play_fn_t)(void *editor);
static editor_play_fn_t g_play_orig;

static int activate_play_map(void *snapshot)
{
    unsigned char str[IDSTR_SIZE] = {0};
    char *json = NULL;
    const char *data = NULL;
    int initialized = 0, length = 0, ok = 0;
    if (!snapshot || !g_ser_orig || !g_map_to_json || !g_idstr_ctor || !g_idstr_dtor) {
        sh_mpkg_report_error("Play could not read the current map's package requirements.");
        return 0;
    }
    __try {
        g_idstr_ctor(str, ""); initialized = 1;
        if (sh_rawmap_snapshot((void *)g_map_to_json, snapshot, str) &&
            read_out_idstr(str, &data, &length)) {
            json = (char *)malloc((size_t)length + 1);
            if (json) {
                memcpy(json, data, (size_t)length); json[length] = 0;
                ok = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (initialized) {
        __try { g_idstr_dtor(str); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    /* Release native storage before publishing a selection. Engine callbacks
     * used for dependency preparation also remain outside the compiler lock. */
    if (ok) ok = select_map_policy(json, (size_t)length);
    else sh_mpkg_report_error("Play could not read the current map's package requirements.");
    free(json);
    return ok;
}

static void sh_play_detour(void *editor)
{
    void *map = NULL;
    if (!g_play_orig) return;
    __try { if (editor) map = *(void **)((unsigned char *)editor + PLAY_MAP_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { map = NULL; }
    if (!activate_play_map(map)) {
        backend_log("PACKAGES: Play refused; editor retained");
        return;
    }
    backend_log("PACKAGES: current map policy activated before Play");
    g_play_orig(editor);
}

int sh_rawmap_play_install(void *play_fn, int status_ok)
{
    void *tramp;
    if (g_play_orig) {
        if (hook_is_installed((void *)g_play_orig)) return 1;
        if (!hook_unpatch((void *)g_play_orig)) return 0;
        g_play_orig = NULL;
    }
    if (!play_fn || !status_ok) {
        backend_log("PACKAGES: Play policy gate unavailable -- EditorSaveAndPlay not resolved cleanly");
        return 0;
    }
    tramp = hook_prepare_rip_lea(play_fn, (void *)sh_play_detour, PLAY_STOLEN, 12);
    if (!tramp) return 0;
    g_play_orig = (editor_play_fn_t)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_play_orig = NULL;
        backend_log("PACKAGES: Play policy gate commit failed");
        return 0;
    }
    backend_log("PACKAGES: Play policy gate installed");
    return 1;
}

/* A save is all-or-nothing: never publish map JSON with missing packages. */
static int mpkg_embed_on_save(void *out_idstr)
{
    const char *data = NULL;
    int len = 0, failed = 0;
    char *body = NULL;
    size_t body_len = 0;
    if (!out_idstr || !read_out_idstr(out_idstr, &data, &len)) return 0;
    __try { body = embed_used_packages(data, (size_t)len, &body_len, &failed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { failed = 1; }
    if (failed) { if (body) HeapFree(GetProcessHeap(), 0, body); return 0; }
    /* Embedding's dependency preparation has now observed the edited map.
     * Activate from its native JSON before transport shards are appended. */
    if (!select_map_policy(data, (size_t)len)) {
        if (body) HeapFree(GetProcessHeap(), 0, body);
        return 0;
    }
    if (!body) return 1;
    if (!replace_out_idstr(out_idstr, body, body_len)) {
        HeapFree(GetProcessHeap(), 0, body);
        sh_mpkg_report_error("Map save failed: the complete package payload could not be written to the saved map."); return 0;
    }
    HeapFree(GetProcessHeap(), 0, body); return 1;
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
    if (g_inspection_depth) return rc;
    if (g_snapshot_depth) {
        if (rc && g_snapshot_visit)
            g_snapshot_visited = g_snapshot_visit(map, out_idstr, g_snapshot_visit_ctx);
        return rc;
    }

    /* Embed used packages before mirroring; independent of the rawmap switch. */
    if (!rc || !mpkg_embed_on_save(out_idstr)) return 0;

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

    if (g_ser_orig) {
        if (hook_is_installed((void *)g_ser_orig)) return 1;
        if (!hook_unpatch((void *)g_ser_orig)) return 0;
        g_ser_orig = NULL;
    }
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
    void *tramp = hook_prepare(serialize_fn, (void *)sh_ser_detour, SAVE_STOLEN);
    if (tramp == NULL) {
        backend_log("B1: rawmap SAVE shadow FAIL -- trampoline preparation failed");
        return 0;
    }
    g_ser_orig = (serialize_fn_t)tramp;

    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_ser_orig = NULL;
        backend_log("B1: rawmap SAVE shadow commit failed; retained callbacks require restoration");
        return 0;
    }
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
    return sh_rawmap_set_save_target(path);
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

    /* Match the signed native length used by read_source_file and idStr.
     * Report an unrepresentable file before a later map-load attempt. */
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "that file is empty", _TRUNCATE);
        return 0;
    }
    if (sz.QuadPart > (LONGLONG)INT_MAX) {
        CloseHandle(h);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "that file cannot fit the engine's signed string length", _TRUNCATE);
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
    if (!sh_rawmap_load_is_safe()) {
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
            "rawmap overwrite protection is unavailable on this build", _TRUNCATE);
        return 0;
    }
    resolve_source_path(path, sizeof path);
    return sh_rawmap_validate_source(path, out_msg, msg_capacity);
}

static int slot_rawmap_status(sh_iface *self, char *out_json, int out_capacity)
{
    char load_path[MAX_PATH], save_path[MAX_PATH];
    char load_esc[MAX_PATH * 2], save_esc[MAX_PATH * 2], target_esc[MAX_PATH * 2];
    char target[MAX_PATH] = "";
    int written;

    (void)self;
    if (out_json == NULL || out_capacity <= 0) return 0;

    resolve_source_path(load_path, sizeof load_path);
    resolve_dest_path(save_path, sizeof save_path);
    sh_rawmap_get_save_target(target, (int)sizeof target);

    if (!json_escape_into(load_esc, sizeof load_esc, load_path)) load_esc[0] = '\0';
    if (!json_escape_into(save_esc, sizeof save_esc, save_path)) save_esc[0] = '\0';
    if (!json_escape_into(target_esc, sizeof target_esc, target)) target_esc[0] = '\0';

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
        /* The file Save writes to, or "" for the default. It rides the status rather than
         * being remembered by the page, because the console can set it too. */
        "\"saveTarget\":\"%s\",\"willFire\":%d}",
        load_esc, save_esc, sh_rawmap_swap_is_armed(),
        sh_rawmap_save_count(), sh_rawmap_save_last_bytes(),
        sh_rawmap_swap_count(), sh_rawmap_swap_complete_count(),
        sh_rawmap_save_oneshot_pending(),
        /* `loadPending` is the load half of the same question savePending answers: a staged rawmap
         * is waiting for the next map to open. Without it the menu could only report a COUNT of past
         * substitutions, which told the person nothing about what happens next. */
        sh_rawmap_load_oneshot_pending(),
        target_esc,
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

    if ((arm == 1 || arm == 2) && !sh_rawmap_load_is_safe()) {
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
            "rawmap overwrite protection is unavailable on this build", _TRUNCATE);
        return 0;
    }

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
            /* No clearing here: staging a file is not opening one. The map-load detour
             * does it, at the moment a map actually opens. */
        } else {
            if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, reason, _TRUNCATE);
            return 0;
        }
    }

    /* A save always uses the live editor. An unavailable or busy editor must
     * not export a different saved map or arm a future unrelated save. */
    if ((save_path && save_path[0]) || arm == 5) {
        if (!sh_editor_frame_request_rawmap_save_to(save_path, out_msg, msg_capacity)) return 0;
    } else if (save_path) {
        ok = sh_rawmap_set_save_target(NULL);
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
    /* 3 and 4 are the File menu's "Keep saving to this file" tick, and neither writes
     * anything. 3 pins whatever the save path already resolves to, so ticking never
     * moves the destination -- it only makes the current one stick. 4 releases it. */
    else if (arm == 3) {
        char now[MAX_PATH] = "";
        resolve_dest_path(now, sizeof now);
        if (now[0]) sh_rawmap_set_save_target(now);
    }
    else if (arm == 4) sh_rawmap_set_save_target(NULL);

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
