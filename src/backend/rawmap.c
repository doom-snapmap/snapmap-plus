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
            int rc = g_deser_orig(ours, out_map);   /* engine parses OUR bytes (overwrite of param_1) */
            InterlockedIncrement(&g_swap_complete_count); /* only after the substituted parse returns */
            HeapFree(GetProcessHeap(), 0, ours);     /* OG frees its substitute buffer too */
            return rc;
        }
        /* armed but no readable source -> fall through to a vanilla load (OG bVar2==false). */
    }
    if (!mpkg_gate_guarded(json)) return 0;   /* refused: engine json never parsed */
    return g_deser_orig(json, out_map);
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

/* The detour. Same prototype as the engine target. Call the engine ORIGINAL first (the real save fills
 * `out`), then read `out` and mirror it to rawmap.json. The shadow write is best-effort + fully guarded:
 * the real save has already happened by the time we touch disk. */
static unsigned char sh_ser_detour(void *map, void *out_idstr, unsigned char compact)
{
    if (g_ser_orig == NULL) return 0;   /* defensive: should never happen once installed */

    /* 1) the engine's own serialize -- the real save, untouched. This runs UNCONDITIONALLY and BEFORE the
     *    gate check below: the player's save must complete identically whether the shadow is on or off.
     *
     *    LATCH THE VERDICT IMMEDIATELY. `rc` is the save's success flag (see the typedef comment): the
     *    caller reads AL the instant we return, so every exit below returns `rc` and nothing else. The
     *    shadow is a bystander to the save -- it may never decide whether the save succeeded. */
    const unsigned char rc = g_ser_orig(map, out_idstr, compact);

    /* 2) the shadow is the SAVE half of the rawmaps switch, so it obeys the same arm the LOAD swap does.
     *    Ungated, this overwrote rawmap.json on every map save even with rawmaps off -- silently discarding
     *    a rawmap the user had put there deliberately. Checked AFTER the real save so the gate can never
     *    affect what the engine writes. */
    if (!rawmap_armed(NULL)) return rc;

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
