/* rawmap.c -- see rawmap.h. The rawmap LOAD swap (port of OG FUN_180023ad0).
 *
 * Detours idSnapMap::DeserializeFromJson(const char* json, idSnapMap* out) [variant A, buffer-first].
 * When armed, our detour reads the file-backed rawmap source into a heap buffer and calls the engine
 * ORIGINAL (via the trampoline) with OUR buffer as arg0 -- the native equivalent of OG's "overwrite
 * param_1" and our reference reimplementation's "args[0] = _rawmapBuf". The engine's own deserialize then parses our
 * bytes. When the gate is off / no source / a read fails, we pass the engine's json through untouched
 * (OG's bVar2==false fallback). We free our buffer after the call.
 *
 * Why this is safe to slot in front of the engine fn: the detour has the EXACT prototype of the target
 * (int(const char*, void*)), so the stolen-prologue trampoline preserves the engine's calling
 * convention; the only thing we change is the first argument's pointer, and only when armed. Every file
 * op is failure-tolerant -- a swap failure degrades to a vanilla load, never a crash.
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

/* Gate (OG DAT_18003e819 / the reference impl _gate). Default DISARMED -- the test harness arms for the test. */
static volatile LONG g_gate = 0;
static volatile LONG g_swap_count = 0;
static volatile LONG g_swap_complete_count = 0;

/* File-backed rawmap source. Default %LOCALAPPDATA%\snapmap-plus\rawmap.json (the OG read
 * %USERPROFILE%\snaphak\rawmap.json). The test harness may override via sh_rawmap_swap_set_source. */
static char g_src_path[MAX_PATH] = {0};

static void default_source_path(char *out, size_t cap)
{
    /* OG: SHGetFolderPathA(CSIDL_PROFILE) + "\snaphak\rawmap.json" (the shared path builder FUN_180023780);
     * ours lives in the consolidated %LOCALAPPDATA%\snapmap-plus\ data root. */
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\rawmap.json", base);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snapmap-plus\\rawmap.json");
}

/* Resolve the EFFECTIVE rawmap source path (the same logic the swap reads from): g_src_path if set,
 * else the default %LOCALAPPDATA%\snapmap-plus\rawmap.json. Writes into `out` (caller-provided MAX_PATH buf).
 * Centralized so the flag-file path tracks set_source() exactly -- both derive from one resolver. */
static void resolve_source_path(char *out, size_t cap)
{
    if (g_src_path[0]) strncpy_s(out, cap, g_src_path, _TRUNCATE);
    else default_source_path(out, cap);
}

/* Derive the TEST arm flag-file path: a sibling of the rawmap source named "arm.flag" (i.e. the source
 * dir + "\arm.flag"). Tracks set_source() because it is built from resolve_source_path. If the source
 * has no directory component, the flag is "arm.flag" relative to the cwd (matches default_source_path's
 * relative fallback). */
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

/* TEST arm trigger: is the sibling arm.flag file present? A single GetFileAttributes per interception
 * (deserializes are infrequent map-loads, so this is cheap). The test harness creates/deletes this file to
 * arm/disarm the swap for a controlled live test, with no console/RPC needed. Additive to the explicit
 * sh_rawmap_swap_arm() gate (they are OR'd in the detour). */
static int flag_file_present(void)
{
    char flag[MAX_PATH];
    flag_file_path(flag, sizeof flag);
    DWORD attrs = GetFileAttributesA(flag);
    return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* The arm predicate BOTH detours share: the explicit gate (sh_rawmap_swap_arm, i.e. the sh_rawmaps_on
 * command) OR the test flag-file. `flag_armed_out` is optional and reports which arm won, for the log line.
 *
 * Factored because the two halves of ONE switch drifted: the LOAD swap read the gate and the SAVE shadow
 * did not, so `sh_rawmaps_off` stopped substitutions while the shadow kept overwriting rawmap.json on every
 * map save. The command's own help says "Enable raw map save/load" -- one switch, both directions -- and a
 * user who turned it off could still lose a rawmap they had staged there by hand. Sharing the predicate is
 * what stops the two sides disagreeing again. */
static int rawmap_armed(int *flag_armed_out)
{
    int explicit_armed = (InterlockedCompareExchange(&g_gate, 0, 0) != 0);
    int flag_armed     = flag_file_present();
    if (flag_armed_out) *flag_armed_out = flag_armed;
    return explicit_armed || flag_armed;
}

/* ONE-SHOT SAVE ARM.
 *
 * The shadow used to need the shared gate left switched on to catch a save, which is what made the
 * gate feel like a mode: arm it and every map you open is substituted and every save overwrites
 * your staged file. Scoping each operation to itself removes that -- the File menu's "Save Rawmap
 * As" arms the shadow for exactly ONE save and it disarms itself again afterwards.
 *
 * Deliberately NOT a second gate. An earlier plan here was to split the shared switch in two, which
 * is a real change in what `sh_rawmaps_on` means; a one-shot is additive, leaves the console
 * commands behaving exactly as they always have, and is enough because both halves of the feature
 * are now driven from a click rather than waiting around for the user to do something. */
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

/* ONE-SHOT LOAD ARM -- the missing half of the pair above.
 *
 * The save side got a one-shot and the load side did not, so "open a rawmap" still had to leave the
 * shared gate switched ON, and the gate is not scoped to one operation: with it on, EVERY map the
 * person opens afterwards is substituted, not just the one they asked for. That is a mode, and it is
 * the kind of mode that quietly replaces a map you only meant to look at.
 *
 * Same shape as the shadow one-shot: additive, so `sh_rawmaps_on` keeps meaning exactly what it
 * always meant, and a click can scope itself to a single load without touching the switch.
 *
 * CONSUMED ONLY ON A REAL SUBSTITUTION. An arm burned by a load that could not read the staged file
 * would leave the person with the next map silently un-substituted and no way to see why, so the
 * detour clears it after the source reads, not when it decides to look. */
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

/* Read the whole source file into a fresh, NUL-terminated heap buffer (OG: malloc(size+1) + fread).
 * Returns the buffer (caller frees with HeapFree) + sets *out_len, or NULL on any failure. */
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
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);   /* +1 for the trailing NUL (OG size+1) */
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

/* The MAP-PACKAGE LOAD GATE (map_package.c), wrapped in the same SEH
 * discipline the save shadow uses on engine memory. This runs on EVERY buffer
 * the engine is about to parse -- the engine's own json (local, published, and
 * network-downloaded maps all funnel through this one function) and our
 * swapped rawmap alike -- BEFORE the parse. A map that declares an override
 * package the running process does not have must NOT reach the engine: the
 * missing content is fatal at spawn/render (AddRenderModel throws on the NULL
 * model), not degraded. Returns 1 = pass to the engine, 0 = refuse the load.
 * A fault inside the gate passes the buffer through untouched -- vanilla
 * behavior, never a new crash. */
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

/* Strip the delivery payload, guarded like the gate itself.
 *
 * Returns a HeapAlloc'd stripped buffer (caller frees) or NULL meaning "use the original". A
 * fault in here is not allowed to change what the engine parses: on a fault we return NULL and
 * the original buffer is used, which is exactly today's behaviour. */
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

/* Everything that has to happen to a map buffer between the gate and the parse.
 *
 * The navigation table is rebuilt FROM EMPTY here, on every buffer the engine is
 * about to parse, because that is what stops a map with no bake from inheriting
 * the previous map's platforms -- the worst silent failure this feature permits.
 * It is built BEFORE either strip, from the bytes the map arrived in.
 *
 * Then both delivery envelopes come out: packages first, navigation second. A
 * strip that declines returns NULL and the previous buffer is used, so the two
 * chain without either one being able to lose the other's work.
 *
 * Returns a HeapAlloc'd buffer (caller frees) or NULL meaning "use the original". */
static char *prepare_map_buffer(const char *json)
{
    char *pkg, *nav;
    size_t len = json ? strlen(json) : 0;
    size_t nav_len = 0;

    if (len == 0) return NULL;

    sh_navmesh_build_from_map(json, len);
    /* The regions the author marked, read from the same bytes and cleared the
     * same way. A map with no marked volume must not inherit the last map's. */
    sh_nav_bake_set_map(json, len);

    pkg = mpkg_strip_guarded(json);
    nav = sh_navmesh_strip(pkg ? pkg : json, pkg ? strlen(pkg) : len, &nav_len);
    if (!nav) return pkg;
    if (pkg) HeapFree(GetProcessHeap(), 0, pkg);
    return nav;
}

/* The detour. Same prototype as the engine target. When armed + a source reads, call the engine
 * original through the trampoline with OUR buffer as arg0; else pass the engine's json through.
 * Either way the chosen buffer passes the map-package gate first; a refused load returns 0 (the
 * same "failed parse" contract the engine's callers already handle as a clean bounce). */
static int sh_deser_detour(const char *json, void *out_map)
{
    if (g_deser_orig == NULL) return 0;   /* defensive: should never happen once installed */

    /* armed = explicit-arm (sh_rawmap_swap_arm) OR the TEST flag-file is present. The flag-file is the
     * test harness's no-console arm trigger; the explicit gate is the production-style arm. Either arms. */
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
                HeapFree(GetProcessHeap(), 0, ours);     /* OG frees its substitute buffer too */
                return rc;
            }
        }
        /* armed but no readable source -> fall through to a vanilla load (OG bVar2==false). */
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
        /* Hook-tolerant fallback resolve (SIG_OK_HOOKED): the live prologue is already a detour (e.g.
         * an external instrumentation tool has hooked this fn during testing). Installing our detour over
         * that would steal detour bytes, not the real prologue -> corruption. Refuse; coexistence with an
         * existing hook is handled at test time. */
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

/* Is the LOAD-swap currently armed? Reports the EXPLICIT arm only.
 *
 * Deliberately NOT the same predicate the swap itself uses: that one is
 * `explicit_armed || flag_file_present()`, because the flag-file is a test
 * stand-in that can arm the swap without anyone having said so. A caller
 * asking "is it on" wants the state a person set and can unset -- reporting
 * the file-backed arm here would show ON for a control that turning off does
 * not clear.
 *
 * Exported so a caller can read it BY NAME. `g_gate` is file-static, so the
 * alternative is an address that moves on every rebuild. */
int sh_rawmap_swap_is_armed(void)
{
    return (InterlockedCompareExchange(&g_gate, 0, 0) != 0) ? 1 : 0;
}

/* Whether the swap WILL fire -- the explicit gate OR the test flag-file, which
 * is what the detour itself decides on.
 *
 * Separate from sh_rawmap_swap_is_armed on purpose. That one answers "is the
 * control a person set turned on", and deliberately hides the flag-file so it
 * cannot report ON for something turning the control off would not clear. This
 * one answers "will the detour substitute a buffer", which is the question a
 * tool has to ask before it calls the function we detour: the daemon's
 * engine-direct codec oracle calls idSnapMap::DeserializeFromJson directly, and
 * doing that while the swap will fire faults inside the engine. */
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

/* ==== merged: SAVE shadow (was rawmap.c) ==== */

/* rawmap.c -- see rawmap.h. The rawmap SAVE shadow (port of OG FUN_180023e60, the
 * INVERSE of the LOAD swap rawmap.c).
 *
 * Detours idSnapMap::SerializeToJson(idSnapMap* map, idStr* out, uint8 compact). On an ARMED save our
 * detour FIRST calls the engine ORIGINAL (via the trampoline) so the engine's own serializer fills the
 * out-idStr `out` -- the real save proceeds untouched -- and THEN reads out.len/out.data and mirrors those
 * bytes to %LOCALAPPDATA%\snapmap-plus\rawmap.json. The just-saved map thus becomes a reusable rawmap (the
 * inverse of the LOAD swap, which substitutes rawmap.json INTO a load). See the header for the full RE.
 *
 * ARMED means the same `rawmap_armed()` the LOAD swap uses -- the shadow is the save half of one switch,
 * not an always-on mirror. The engine's own serialize is never gated; only the copy to disk is.
 *
 * `sh_pretty_on` re-lays-out the bytes on their way to disk (json_pretty.h). Same reasoning as the arm:
 * the cvar governs the rawmap this project writes, so it applies where those bytes are chosen -- HERE --
 * and not to the engine's out-idStr, which is what the player's own save is written from.
 *
 * Why this is safe to slot in front of the engine fn: the detour has the EXACT prototype of the target
 * (void(idSnapMap*, idStr*, uint8)), so the stolen-prologue trampoline preserves the engine's calling
 * convention; we change nothing about the serialize itself -- we only READ the engine's output idStr and
 * write a copy to disk. Every file op is failure-tolerant and the WRITE happens AFTER the real save has
 * completed, so a shadow failure degrades to a vanilla save, never a crash and never a corrupted save.
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

/* idStr field offsets (DIRECT: the OG decompile of
 * FUN_180023e60 reads *(int*)(out+8)=len and *(void**)(out+0x10)=data; the reference impl IDSTR_LEN_OFF/DATA_OFF). */
#define IDSTR_LEN_OFF   0x08   /* int  len  (character count, excl NUL) */
#define IDSTR_DATA_OFF  0x10   /* char* data (inline baseBuffer for short strings, else heap) */

/* The engine target's prototype: bool SerializeToJson(idSnapMap* map, idStr* out, uint8 compact). The
 * out-idStr is arg1 (RDX); the JSON lands there after the call (serialize-to-json-rva.md).
 *
 * IT RETURNS A BOOL IN AL, AND THE ONLY CALLER CONSUMES IT [DIRECT, decoded from the pinned build].
 * At the sole call site (the save-snapshot function 0x59D2F0, +0x54) the bytes immediately after the
 * `call` are:
 *     0F B6 D8            MOVZX EBX,AL        ; latch the return value
 *     48 8D 4C 24 30      LEA   RCX,[RSP+0x30]
 *     E8 DA 08 F8 FF      CALL  <idStr dtor>
 *     0F B6 C3            MOVZX EAX,BL        ; and return it as the snapshot's own result
 * so AL IS the save's success flag: a zero there aborts the save with no error, no log and no file.
 *
 * This was typed `void` until 2026-09-01, which made the detour's own last-executed call decide the
 * save's fate. With the shadow DISARMED (the default) the final call before returning was
 * GetFileAttributesA inside rawmap_armed() -> flag_file_present(), whose miss returns 0 -- so every
 * save reported FAILURE, the editor kept the map dirty, re-prompted for a name on exit, and the map
 * never appeared in My Maps. With the shadow ARMED the trailing shadow work happened to leave AL
 * non-zero, which is why `sh_rawmaps_on` "fixed" saving -- by luck, not by design.
 *
 * The detour must therefore latch the engine's verdict and return THAT on every exit path. Nothing
 * this file does after the original returns may be allowed to speak for the engine. */
typedef unsigned char (*serialize_fn_t)(void *map, void *out_idstr, unsigned char compact);

static serialize_fn_t g_ser_orig = NULL;   /* the trampoline -> the real engine SerializeToJson */

static volatile LONG     g_shadow_count = 0;
static volatile LONGLONG g_last_bytes   = 0;

/* Shadow destination. Default matches the LOAD swap's source (%LOCALAPPDATA%\snapmap-plus\rawmap.json)
 * so a save-then-load round-trips (the OG mirrored to %USERPROFILE%\snaphak\rawmap.json). The test
 * harness may override via sh_rawmap_save_set_dest. */
static char g_dest_path[MAX_PATH] = {0};

static void default_dest_path(char *out, size_t cap)
{
    /* OG: SHGetFolderPathA(CSIDL_PROFILE) + "\snaphak\rawmap.json" (the shared path builder FUN_180023780);
     * ours lives in the consolidated %LOCALAPPDATA%\snapmap-plus\ data root. */
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
    return (total == len) ? total : 0;   /* a short write -> report failure (don't leave a partial shadow) */
}

/* sh_pretty_on: lay the engine's one-line JSON out over indented lines for the shadow copy. Returns a
 * fresh heap buffer (caller HeapFrees) + *out_len, or NULL to mean "write the engine bytes unchanged" --
 * which covers both a refusal from json_pretty (see its header: it fails closed rather than truncate)
 * and an allocation failure. NULL is never an error worth failing the shadow over: an unreadable rawmap
 * still loads, so the layout is the part we drop, never the write.
 *
 * Reads `data`, which is the engine's out-idStr buffer -- the caller keeps this inside its SEH guard. */
static char *pretty_copy(const char *data, size_t len, size_t *out_len)
{
    size_t need = json_pretty(data, len, NULL, 0);   /* pass 1: measure (no store) */
    char  *buf;
    if (need == 0) return NULL;
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, need);
    if (buf == NULL) return NULL;
    if (json_pretty(data, len, buf, need) != need) { /* pass 2: store. Deterministic -- a mismatch is impossible
                                                     * unless the source moved under us; treat it as a refusal. */
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
    /* Resolved independently and never guessed: this writes into the engine's out-idStr on the
     * save path, so a wrong address would corrupt a player's map rather than merely fail. */
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

/* Build the map JSON with every used package embedded, or NULL for "leave the save alone".
 * Pure: reads the engine's bytes, touches no engine state. */
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

/* Replace the engine's out-idStr with `body`, through the engine's own assignment.
 *
 * The source idStr is built on the stack with a zeroed flags word, which declines the assign's
 * steal/swap fast path -- so the engine COPIES our bytes and nothing of ours is aliased or
 * freed by it, and nothing of the engine's is aliased by us. */
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

/* The save-path entry point. Everything is best-effort: the engine's save has already completed
 * correctly by the time this runs, and any failure here simply leaves it as it was. */
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

/* Put the current map's baked navigation back into the bytes being saved.
 *
 * The shards were stripped on load, so without this a load-then-save with no
 * fresh bake would silently discard the author's navigation. navmesh.c holds
 * every payload that survived delivery -- including ones this client refused to
 * SERVE -- precisely so a save cannot destroy work a different client can use. */
/* Re-read the author's marked volumes from the map being SAVED.
 *
 * Regions were originally captured only on the deserialize funnel, which is
 * wrong for the way authoring actually happens: mark some volumes in the editor,
 * press Play, and no map load occurs in between -- the engine serializes the
 * live map and builds from that. The region table would still hold whatever the
 * map carried when it was last LOADED, so a volume marked this session simply
 * did not exist as far as navigation was concerned, and the author would be told
 * "no volume in this map is marked" moments after ticking one.
 *
 * This covers SAVE. It does NOT cover Play-from-the-editor: pressing Play does
 * not serialize the map at all -- verified live, this hook never runs on that
 * transition -- so the editor builds the play session straight from its live map
 * object. A volume marked and then played in the same session therefore still
 * gets nothing until the map is saved and reloaded. Closing that needs the marks
 * read from the live editor entities rather than from map JSON, which is a
 * different mechanism than anything here. */
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

/* The detour. Same prototype as the engine target. Call the engine ORIGINAL first (the real save fills
 * `out`), then read `out` and mirror it to rawmap.json. The shadow write is best-effort + fully guarded:
 * the real save has already happened by the time we touch disk. */
static unsigned char sh_ser_detour(void *map, void *out_idstr, unsigned char compact)
{
    int used_oneshot = 0;   /* did a "Save Rawmap As" one-shot authorize this write? */

    if (g_ser_orig == NULL) return 0;   /* defensive: should never happen once installed */

    /* 1) the engine's own serialize -- the real save, untouched. This runs UNCONDITIONALLY and BEFORE the
     *    gate check below: the player's save must complete identically whether the shadow is on or off.
     *
     *    LATCH THE VERDICT IMMEDIATELY. `rc` is the save's success flag (see the typedef comment): the
     *    caller reads AL the instant we return, so every exit below returns `rc` and nothing else. The
     *    shadow is a bystander to the save -- it may never decide whether the save succeeded. */
    const unsigned char rc = g_ser_orig(map, out_idstr, compact);

    /* 1b) embed the packages this map uses, so a player who does not have them can install them
     *     from the map itself. This runs BEFORE the shadow so the mirrored copy matches what was
     *     actually saved, and it is independent of the rawmaps switch: it is a product feature,
     *     not a debugging aid. A map that uses no packages is untouched. */
    mpkg_embed_on_save(out_idstr);

    /* 1c) ...and the navigation this map arrived with, or was baked with this
     *     session. Runs AFTER the package embed so it operates on the bytes that
     *     are actually being saved, and like it, is independent of the rawmaps
     *     switch: losing an author's bake on an ordinary save is not a debugging
     *     aid, it is data loss. */
    nav_embed_on_save(out_idstr);

    /* 1d) refresh the marked-volume table from what is being saved, so a volume
     *     ticked in this session takes effect on the very next Play instead of
     *     waiting for a map reload. */
    nav_regions_on_save(out_idstr);

    /* 2) the shadow is the SAVE half of the rawmaps switch, so it obeys the same arm the LOAD swap does.
     *    Ungated, this overwrote rawmap.json on every map save even with rawmaps off -- silently discarding
     *    a rawmap the user had put there deliberately. Checked AFTER the real save so the gate can never
     *    affect what the engine writes. */
    /* ...or a one-shot arm from "Save Rawmap As", which authorizes exactly this save and is cleared
     * once the bytes are on disk (below). CONSUMED here, not after the write: two saves racing in
     * must not both pass on the same one-shot, and InterlockedExchange makes exactly one of them
     * the winner. A failed write therefore spends the one-shot -- the alternative is a one-shot
     * that can fire on some later save the person did not connect to their click, which is worse
     * than making them click again. */
    used_oneshot = (InterlockedExchange(&g_shadow_oneshot, 0) != 0);
    if (!rawmap_armed(NULL) && !used_oneshot) return rc;

    if (out_idstr == NULL) return rc;

    /* 2) read the engine's output idStr (len@+0x8, data@+0x10) under SEH (the engine fills these; a layout
     *    surprise must not fault the save path) and mirror it to disk. */
    const char *data = NULL;
    int         len  = 0;
    __try {
        len  = *(int *)((unsigned char *)out_idstr + IDSTR_LEN_OFF);
        data = *(const char **)((unsigned char *)out_idstr + IDSTR_DATA_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return rc;   /* unreadable out-idStr -> skip the shadow (the real save already completed) */
    }
    if (data == NULL || len <= 0) return rc;

    /* 3) sh_pretty_on chooses the LAYOUT of the copy. Read live per save (it is a cvar a user flips
     *    mid-session, not a startup choice) and default OFF, which is also what a failed cvar register
     *    reports -- so an engine we could not register into simply writes what it always wrote. */
    int pretty = sh_cvar_value_int(B2_CVAR_SH_PRETTY_ON, 0);

    const char *body     = data;                     /* what actually goes to disk ... */
    size_t      body_len = (size_t)len;              /* ... the engine's bytes, or our re-laid-out copy */
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
        /* Hook-tolerant fallback resolve (SIG_OK_HOOKED): the live prologue is already a detour (e.g.
         * an external instrumentation tool has hooked this fn during testing). Installing our detour over
         * that would steal detour bytes, not the real prologue -> corruption. Refuse; coexistence with an
         * existing hook is handled at test time (same conservative policy as the LOAD swap). */
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

    if (!g_dest_path[0]) default_dest_path(g_dest_path, sizeof g_dest_path);
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
     * checkbox must not show ON for a flag-file arm that unticking it cannot clear. */
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
        "\"loads\":%lu,\"loadsDone\":%lu,\"savePending\":%d,\"loadPending\":%d}",
        load_esc, save_esc, sh_rawmap_swap_is_armed(),
        sh_rawmap_save_count(), sh_rawmap_save_last_bytes(),
        sh_rawmap_swap_count(), sh_rawmap_swap_complete_count(),
        sh_rawmap_save_oneshot_pending(),
        /* `loadPending` is the load half of the same question savePending answers: a staged rawmap
         * is waiting for the next map to open. Without it the menu could only report a COUNT of past
         * substitutions, which told the person nothing about what happens next. */
        sh_rawmap_load_oneshot_pending());

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
        if (!sh_rawmap_save_set_dest(save_path[0] == '\0' ? NULL : save_path)) ok = 0;

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
    if (arm == 0 || arm == 1)  sh_rawmap_swap_arm(arm);
    else if (arm == 2)         sh_rawmap_load_arm_once();

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
