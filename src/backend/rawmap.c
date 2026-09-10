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
    if (rawmap_armed(&flag_armed)) {
        size_t len = 0;
        char *ours = read_source_file(&len);
        if (ours != NULL) {
            char line[160];
            unsigned long n = (unsigned long)InterlockedIncrement(&g_swap_count);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: rawmap swap FIRED (orig %s bytes -> ours %zu bytes) [#%lu]%s",
                json ? "<engine-json>" : "<null>", len, n,
                flag_armed ? " [flag-armed]" : "");
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

    if (!g_src_path[0]) default_source_path(g_src_path, sizeof g_src_path);
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_deser_orig = NULL;
        backend_log("B1: rawmap LOAD-swap commit failed; retained callbacks require restoration");
        return 0;
    }
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

/* Mirror destination override; the default matches the load source. */
static char g_dest_path[MAX_PATH] = {0};

static void default_dest_path(char *out, size_t cap)
{
    /* Use the shared application data root. */
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\rawmap.json", base);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snapmap-plus\\rawmap.json");
}

static void resolve_dest_path(char *out, size_t cap)
{
    if (g_dest_path[0]) strncpy_s(out, cap, g_dest_path, _TRUNCATE);
    else default_dest_path(out, cap);
}

/* Write `len` bytes from `data` to the shadow destination ("wb", truncate). Returns the byte count
 * written, or 0 on any failure. SEH-free here (pure Win32 file ops on a validated buffer); the engine
 * out-idStr read in the detour is SEH-guarded by the caller. */
static unsigned long long write_shadow(const char *data, size_t len)
{
    char path[MAX_PATH];
    resolve_dest_path(path, sizeof path);

    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    unsigned long long total = 0;
    while (total < len) {
        size_t remain = len - (size_t)total;
        DWORD chunk = (DWORD)(remain > 0x10000000 ? 0x10000000 : remain);
        DWORD wr = 0;
        if (!WriteFile(h, data + total, chunk, &wr, NULL) || wr == 0) break;
        total += wr;
    }
    CloseHandle(h);
    return (total == len) ? total : 0;   /* Report a short write as failure. */
}

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

    /* Gate only the disk mirror; load substitution uses the same predicate. */
    if (!rawmap_armed(NULL)) return rc;

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
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B1: rawmap SAVE shadow wrote %llu bytes -> rawmap.json [#%lu]%s", wrote, n,
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

    if (!g_dest_path[0]) default_dest_path(g_dest_path, sizeof g_dest_path);
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_ser_orig = NULL;
        backend_log("B1: rawmap SAVE shadow commit failed; retained callbacks require restoration");
        return 0;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: rawmap SAVE shadow installed at %p (trampoline %p, stolen %d); dest=%s",
        serialize_fn, tramp, SAVE_STOLEN, g_dest_path);
    backend_log(line);
    return 1;
}

int sh_rawmap_save_set_dest(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        default_dest_path(g_dest_path, sizeof g_dest_path);
        return 1;
    }
    strncpy_s(g_dest_path, sizeof g_dest_path, path, _TRUNCATE);
    return g_dest_path[0] != '\0';
}

unsigned long sh_rawmap_save_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_shadow_count, 0, 0);
}

unsigned long long sh_rawmap_save_last_bytes(void)
{
    return (unsigned long long)InterlockedCompareExchange64(&g_last_bytes, 0, 0);
}
