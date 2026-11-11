/* editor_frame.c -- see editor_frame.h.
 *
 * The hook has the EXACT prototype of the target, so the stolen-prologue trampoline preserves the
 * engine's calling convention. We add nothing to the frame except a bounded check on most frames,
 * and, on the one frame a request is serviced, a single engine call.
 *
 * ORDER: the engine's own Think runs FIRST, unconditionally, and our work runs after it returns.
 * That is the whole point of the file -- servicing before it would put our call ahead of the state
 * the frame is about to establish, which is the nested-context mistake this exists to avoid.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>

#include "editor_frame.h"
#include "world_area_cell.h"
#include "fly_to.h"
#include "module_prefab.h"
#include "timeline_tile.h"
#include "budget_table.h"
#include "editor_zoom.h"
#include "hook.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "rawmap.h"       /* the rawmap the reload opens, and the save it writes */
#include "../fault_shield/recovery.h"   /* a failed reload's dialogs are the product's */
#include "../common/text.h"

/* EditorFrame prologue steal window. The signature's own 33 bytes, every one of them whole and
 * position-independent (register/rsp/rbp only; no RIP-relative operand and no relative branch), so
 * the window can be taken at either of two instruction boundaries. 25 is the smaller: through
 * `sub rsp,0x90`, stopping before the `mov qword [rbp-0x39],-2`. We take 25 rather than 33 because
 * a steal only has to be big enough for the jump the hook writes, and a shorter window is a smaller
 * bet on the disassembly being right about every byte after it. */
#define EDITOR_FRAME_STOLEN 25

/* Editor object field offsets. Read from the pinned build's decompile of the frame function itself
 * (the state id is `param_1[0x46c3]`, i.e. +0x23618) and cross-checked against an independent
 * corpus that live-drove this same reload. Field LAYOUTS are identical between the two shipped
 * executables -- only code and data addresses move -- so these do not need deriving, unlike the
 * singleton's address. Every read through them is SEH-guarded regardless. */
#define ED_MAP_PTR_OFF    0x204C8   /* the live idSnapMap*; null until a map is open */
#define ED_DEACT_OFF      0x2120D   /* byte: a deactivate/shutdown is deferred to the frame loop */
#define ED_STATE_OFF      0x23618   /* int: current editor state id; 0 = no state running */
#define ED_SUBSTATE_OFF   0x224DC   /* int: 5 = mid-transition, do not disturb */

typedef void (*editor_frame_fn)(void *editor, void *arg);
typedef int  (*ef_load_map_fn)(void *editor, void *id_str);

static editor_frame_fn g_frame_orig  = NULL;
static ef_load_map_fn  g_load_map    = NULL;
/* The entry as resolved, kept because installing the hook overwrites the prologue that
 * identifies it. The fault shield needs the same address and scans later. */
static void           *g_frame_entry = NULL;
/* The INLINE idSnapEditorLocal OBJECT, not a pointer to one -- it is in-place constructed at a data
 * global, exactly as iface_engine.c documents. So this address IS the `this` LoadMap wants; there is
 * no dereference step, and adding one would read the object's first field as a pointer. */
static uintptr_t       g_editor_obj  = 0;

/* ReadLocalSavedMapText(localId, idStr *text, ...) and IdStrAssignCStr(idStr *, const char *). */
typedef int  (*ef_read_text_fn)(const char *id, void *text, void *a3, void *a4);
typedef void (*ef_assign_cstr_fn)(void *idstr, const char *text);
static ef_read_text_fn   g_read_text_orig = NULL;
static ef_assign_cstr_fn g_assign_cstr    = NULL;
/* The id LoadMap borrows, set on the main thread for exactly the duration of that call. */
static const char *g_borrowed_id = NULL;
static DWORD       g_borrow_thread = 0;
#define EF_READ_TEXT_STOLEN 17

/* A LoadMap fault turns off the reload only: a later call would meet the same engine state. */
static volatile LONG  g_reload_faulted = 0;

/* One queued request of each kind, with the map and load generation it was made for. */
typedef struct ef_save_request {
    void *map;
    unsigned long generation;
    char destination[MAX_PATH];
    int choose_target;
} ef_save_request;
static volatile LONG  g_save_pending = 0;
static SRWLOCK g_save_lock = SRWLOCK_INIT;
static ef_save_request g_save_request;

typedef struct ef_reload_request {
    void *map;
    unsigned long generation;
    /* The 20-hex save-directory id LoadMap is handed. Captured when the request is made (a disk
     * walk is not something to do on a frame) and consumed on the frame that services it. */
    char id[64];
} ef_reload_request;
static volatile LONG  g_reload_pending = 0;
static SRWLOCK g_reload_lock = SRWLOCK_INIT;
static ef_reload_request g_reload_request;

/* ------------------------------------------------------------------ the saved-map id ------------
 * LoadMap names a map by its save-directory id. Local SnapMap saves live one folder per map at
 *
 *     <Saved Games>\id Software\DOOM\base\savegame.user\<account id>\SNAPMAPS<20 hex>
 *
 * and that 20-hex suffix IS the id. Each account has its own numeric folder under savegame.user,
 * and "Saved Games" is a known folder in its own right that can be relocated, so it is asked for,
 * with the profile root as the fallback. The walk is wide: a profile path need not be ASCII.
 *
 * A copied save folder is NOT a second map: the engine keys a save by the `localId` inside its
 * game.details, so both list entries open the same map. The reload borrows an existing slot and
 * relies on the branch answer (rawmap.c) to keep Save from writing over it. */

/* The id must be exactly 20 hex digits. Checked because it is about to be handed to an engine
 * function as a name: anything else means we misread the directory layout. */
static int valid_map_id(const wchar_t *s)
{
    int n = 0;
    for (; s[n]; n++) {
        wchar_t c = s[n];
        int hex = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f');
        if (!hex || n >= 20) return 0;
    }
    return n == 20;
}

static int saved_games_root(wchar_t *out, size_t cap)
{
    PWSTR wide = NULL;
    /* FOLDERID_SavedGames. Spelled out rather than pulled from knownfolders.h's extern so this
     * compiles the same regardless of which SDK headers the build picks up. */
    static const GUID kSavedGames =
        { 0x4C5C32FFu, 0xBB9D, 0x43b0, { 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4 } };

    if (SUCCEEDED(SHGetKnownFolderPath(&kSavedGames, 0, NULL, &wide)) && wide) {
        int ok = wcscpy_s(out, cap, wide) == 0;
        CoTaskMemFree(wide);
        if (ok) return 1;
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, out))) {
        wcsncat_s(out, cap, L"\\Saved Games", _TRUNCATE);
        return 1;
    }
    return 0;
}

static int newest_saved_map_id(char *out, size_t cap)
{
    wchar_t root[MAX_PATH], base[MAX_PATH], glob[MAX_PATH];
    WIN32_FIND_DATAW acct;
    HANDLE ha;
    FILETIME best;
    int found = 0;

    if (!saved_games_root(root, MAX_PATH)) return 0;
    _snwprintf_s(base, MAX_PATH, _TRUNCATE, L"%s\\id Software\\DOOM\\base\\savegame.user", root);
    memset(&best, 0, sizeof best);

    /* Walk the account folders. Usually one, but a shared machine can have several, and the newest
     * map across all of them is a better answer than picking the first account we happen to see. */
    _snwprintf_s(glob, MAX_PATH, _TRUNCATE, L"%s\\*", base);
    ha = FindFirstFileW(glob, &acct);
    if (ha == INVALID_HANDLE_VALUE) return 0;
    do {
        WIN32_FIND_DATAW fd;
        HANDLE hm;
        if (!(acct.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (acct.cFileName[0] == L'.') continue;

        _snwprintf_s(glob, MAX_PATH, _TRUNCATE, L"%s\\%s\\SNAPMAPS*", base, acct.cFileName);
        hm = FindFirstFileW(glob, &fd);
        if (hm == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t probe[MAX_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!valid_map_id(fd.cFileName + 8)) continue;   /* skip "SNAPMAPS" */

            /* A correctly-named folder is NOT a saved map. Deleting a map in DOOM removes
             * game.details and map.decl but leaves both .verify sidecars, so the tree fills with
             * folders that pass every name check and hold no data. Require the two files that
             * actually constitute a save. */
            _snwprintf_s(probe, MAX_PATH, _TRUNCATE, L"%s\\%s\\%s\\map.decl", base, acct.cFileName, fd.cFileName);
            if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) continue;
            _snwprintf_s(probe, MAX_PATH, _TRUNCATE, L"%s\\%s\\%s\\game.details", base, acct.cFileName, fd.cFileName);
            if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES) continue;

            if (found && CompareFileTime(&fd.ftLastWriteTime, &best) <= 0) continue;
            best = fd.ftLastWriteTime;
            /* The id is 20 hex digits, so this conversion is exact. */
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName + 8, -1, out, (int)cap, NULL, NULL)) continue;
            found = 1;
        } while (FindNextFileW(hm, &fd));
        FindClose(hm);
    } while (FindNextFileW(ha, &acct));
    FindClose(ha);

    return found && out[0] != '\0';
}

/* ------------------------------------------------------------------ editor state --------------- */

/* Is the editor LIVE and holding a map? The map pointer alone SURVIVES leaving the editor, still
 * holding the last map at the main menu; ED_STATE_OFF == 0 is what separates "a map is open" from
 * "a map was open". */
static int ef_editor_live_with_map(void *editor)
{
    __try {
        const unsigned char *e = (const unsigned char *)editor;
        if (*(void *const *)(e + ED_MAP_PTR_OFF) == NULL) return 0;
        if (*(const char *)(e + ED_DEACT_OFF) != 0)       return 0;
        if (*(const int *)(e + ED_STATE_OFF) == 0)        return 0;
        if (*(const int *)(e + ED_SUBSTATE_OFF) == 5)     return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

#define EF_OPEN_A_MAP "Open a map in the editor and wait until it has loaded, then try again"

/* The open map, or NULL when the editor is not live. */
static void *ef_live_map(void *editor)
{
    void *map = NULL;
    if (!editor || !ef_editor_live_with_map(editor)) return NULL;
    __try { map = *(void *const *)((const unsigned char *)editor + ED_MAP_PTR_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { map = NULL; }
    return map;
}

/* ------------------------------------------------------------------ the rawmap save ------------- */

static void ef_service_rawmap_save(void *editor)
{
    ef_save_request request;
    char msg[MAX_PATH + 192] = "";
    void *map;

    AcquireSRWLockExclusive(&g_save_lock);
    if (!InterlockedExchange(&g_save_pending, 0)) {
        ReleaseSRWLockExclusive(&g_save_lock);
        return;
    }
    request = g_save_request;
    ReleaseSRWLockExclusive(&g_save_lock);

    /* A click can outlive its map or editor state. Never export the next map
     * through a destination selected for the previous one. */
    map = ef_live_map(editor);
    if (map == NULL || map != request.map || sh_rawmap_load_generation() != request.generation) {
        backend_log("EF: rawmap save cancelled -- the map changed before the save");
        sh_rawmap_tell("The rawmap was not saved: the map changed before the save. Save it again.");
        return;
    }
    if (request.choose_target && !sh_rawmap_set_save_target(request.destination)) return;
    if (!sh_rawmap_write_from_live(map, request.destination, msg, (int)sizeof msg, NULL)) {
        char text[sizeof msg + 48];
        _snprintf_s(text, sizeof text, _TRUNCATE, "The rawmap was not saved: %s.", msg);
        sh_rawmap_tell(text);
    }
}

/* Why a rawmap save cannot happen right now, or NULL if it can. NO SIDE EFFECTS and no logging. */
static const char *ef_rawmap_save_blocker(void)
{
    if (g_frame_orig == NULL || g_editor_obj == 0)  return "Saving rawmaps is unavailable on this game build";
    if (!sh_rawmap_live_serialize_ready())           return "Saving rawmaps is unavailable on this game build";
    if (InterlockedCompareExchange(&g_save_pending, 0, 0) != 0)
                                                     return "A rawmap save is already in progress";
    if (!ef_live_map((void *)g_editor_obj))          return EF_OPEN_A_MAP;
    return NULL;
}

/* ------------------------------------------------------------------ the reload ------------------ */

/* The idStr LoadMap actually needs. It reads ONLY the data pointer at +0x10; the length at +0x08 is
 * set too. A 0x30-byte zeroed block is the engine's own idStr footprint; nothing here constructs or
 * frees an engine string. */
typedef struct ef_idstr {
    unsigned char pad0[8];
    int           len;
    int           pad1;
    const char   *data;
    unsigned char pad2[0x30 - 0x18];
} ef_idstr;

/* Run the queued reload. Called from the frame hook only, after the engine's own Think returned. */
static void ef_service_reload(void *editor)
{
    ef_reload_request request;
    ef_idstr s;
    char *reason = NULL;   /* the rawmap layer's refusal (malloc'd): a file name and the importer's cause */
    char line[160];
    void *map;
    int rc = 0;

    map = ef_live_map(editor);
    if (map == NULL) return;   /* mid-transition: stay pending, try again next frame */

    AcquireSRWLockExclusive(&g_reload_lock);
    if (!InterlockedExchange(&g_reload_pending, 0)) {   /* consume before the call: never retry a fault */
        ReleaseSRWLockExclusive(&g_reload_lock);
        return;
    }
    request = g_reload_request;
    ReleaseSRWLockExclusive(&g_reload_lock);

    /* The request belongs to the map that was open when it was made. */
    if (map != request.map || sh_rawmap_load_generation() != request.generation) {
        backend_log("EF: rawmap open cancelled -- a different map was opened first");
        sh_rawmap_tell("The rawmap was not opened: a different map opened first. Load it again.");
        return;
    }

    /* The mods the rawmap names, checked on this frame and before the engine sees a byte: a map
     * whose mods are missing goes to the rawmap hold (the download question, or a refusal naming
     * them) instead of LoadMap, where SnapMap's validator would throw the player out. This reads
     * the file, so a file moved or replaced since the click is refused here. */
    if (!sh_rawmap_prepare_reload(&reason)) {
        char *said = sh_textf("EF: reload not started -- %s", reason ? reason : "");
        backend_log(said ? said : "EF: reload not started");
        free(said);
        free(reason);
        return;
    }

    memset(&s, 0, sizeof s);
    s.data = request.id;
    s.len  = (int)strlen(request.id);

    g_borrow_thread = GetCurrentThreadId();
    g_borrowed_id = request.id;
    __try {
        rc = g_load_map(editor, &s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_borrowed_id = NULL;
        InterlockedExchange(&g_reload_faulted, 1);
        backend_log("EF: reload FAULTED -- opening rawmaps is off for the session");
        shield_note_refused_load();
        sh_rawmap_reload_done(0, 1);
        return;
    }
    g_borrowed_id = NULL;
    /* The borrowed map is sound: whatever failed, the engine's notices about it are not true. */
    if (rc != 0) shield_note_refused_load();
    /* LoadMap returns 0 on the success path (it returns 9 when the editor asked to deactivate
     * mid-load, and passes its inner loader's code through otherwise). */
    sh_rawmap_reload_done(rc, 0);
    _snprintf_s(line, sizeof line, _TRUNCATE, "EF: reload returned %d (id=%s)", rc, request.id);
    backend_log(line);
}

/* Why the editor cannot open a rawmap now, or NULL. NO SIDE EFFECTS. */
static const char *ef_reload_blocker(void)
{
    if (g_frame_orig == NULL || g_load_map == NULL || g_editor_obj == 0)
        return "Opening rawmaps is unavailable on this game build";
    if (InterlockedCompareExchange(&g_reload_faulted, 0, 0) != 0)
        return "Opening rawmaps stopped working this session after an engine error; restart DOOM";
    if (g_read_text_orig == NULL)
        return "Opening rawmaps is unavailable on this game build";
    if (InterlockedCompareExchange(&g_reload_pending, 0, 0) != 0)
        return "A rawmap is already opening";
    if (!ef_live_map((void *)g_editor_obj)) return EF_OPEN_A_MAP;
    return NULL;
}

/* ------------------------------------------------------------------ the detour ------------------ */

static void sh_editor_frame_detour(void *editor, void *arg)
{
    g_frame_orig(editor, arg);     /* the engine's own frame, first and unconditionally */

    if (editor == NULL) return;

    /* One read unless a map was just opened (logs its world-area cell size). */
    sh_world_area_cell_editor_tick();

    /* A queued fly-to advances one phase per frame; idle costs one read. */
    sh_fly_to_tick(editor);
    /* A staged module prefab installs, then follows its grab, one step per frame. */
    sh_module_prefab_tick(editor);
    /* The Timeline tile's prototype record, and entity rows after list changes. */
    sh_timeline_tile_tick(editor);
    /* The budget usage snapshot, and the Prefabs tab's table after a palette build. */
    sh_budget_table_tick(editor);
    /* Whether the editor camera forces the Blueprint far plane this frame. */
    sh_editor_zoom_tick();

    /* Service only explicit save requests after the engine has settled this frame. */
    if (InterlockedCompareExchange(&g_save_pending, 0, 0) != 0) ef_service_rawmap_save(editor);

    if (InterlockedCompareExchange(&g_reload_pending, 0, 0) == 0) return;   /* the common frame */
    if (g_load_map == NULL || InterlockedCompareExchange(&g_reload_faulted, 0, 0) != 0) return;

    ef_service_reload(editor);
}

/* ------------------------------------------------------------------ install + API --------------- */

int sh_editor_frame_install(void *frame_fn, int status_ok, void *load_map_fn,
                            const uint8_t *module_base)
{
    glb_status st = GLB_OK;
    char line[256];
    void *tramp;

    /* Published before the patch and regardless of it: a skipped hook still leaves a
     * usable address for anything that only wants to know where the function is. */
    g_frame_entry = frame_fn;

    if (frame_fn == NULL) {
        backend_log("EF: editor-frame hook SKIPPED -- EditorFrame not resolved");
        return 0;
    }
    if (!status_ok) {
        /* Same policy as the rawmap detours: the hook-tolerant fallback means the live prologue is
         * already somebody else's detour, and stealing detour bytes corrupts both. */
        backend_log("EF: editor-frame hook SKIPPED -- EditorFrame resolved via the hook-tolerant "
                    "fallback (prologue already hooked); not installing over an existing detour");
        return 0;
    }
    if (g_frame_orig != NULL) {
        backend_log("EF: editor-frame hook already installed");
        return 1;
    }

    /* The editor singleton's ADDRESS moves between the two shipped executables by nearly 0x1000000,
     * so it is derived, never baked. A failure here is not fatal to the hook -- the frame hook is a
     * useful execution point on its own -- but it does disable the reload, which needs the object. */
    if (module_base) {
        g_editor_obj = glb_resolve(module_base, "editor_singleton", &st);
        if (!g_editor_obj) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "EF: editor singleton UNRESOLVED (glb status %d) -- reload unavailable", (int)st);
            backend_log(line);
        }
    }

    tramp = hook_prepare(frame_fn, (void *)sh_editor_frame_detour, EDITOR_FRAME_STOLEN);
    if (tramp == NULL) {
        backend_log("EF: editor-frame hook FAIL -- trampoline preparation failed");
        return 0;
    }
    /* The detour's original callback must carry the trampoline before the commit. */
    g_frame_orig = (editor_frame_fn)tramp;
    g_load_map   = (ef_load_map_fn)load_map_fn;

    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_frame_orig = NULL;
        backend_log("EF: editor-frame hook commit failed; retained callbacks require restoration");
        return 0;
    }

    _snprintf_s(line, sizeof line, _TRUNCATE,
        "EF: editor-frame hook installed at %p (trampoline %p, stolen %d); loadmap=%p; editor=%p",
        frame_fn, tramp, EDITOR_FRAME_STOLEN, load_map_fn, (void *)g_editor_obj);
    backend_log(line);
    return 1;
}

/* The read the reload's LoadMap makes of the borrowed id gets a placeholder: the slot's files are
 * never opened, so nothing about them can fail and mark the slot unreadable. Every other read is
 * the engine's own. */
static int ef_read_text_detour(const char *id, void *text, void *a3, void *a4)
{
    const char *borrowed = g_borrowed_id;
    if (borrowed && GetCurrentThreadId() == g_borrow_thread && id && strcmp(id, borrowed) == 0) {
        g_assign_cstr(text, "{}");   /* the load detour parses the rawmap's text in its place */
        return 0;
    }
    return g_read_text_orig(id, text, a3, a4);
}

int sh_editor_frame_carrier_install(void *read_text_fn, int status_ok, void *assign_cstr_fn)
{
    void *tramp;
    if (g_read_text_orig != NULL) return 1;
    if (read_text_fn == NULL || !status_ok || assign_cstr_fn == NULL) {
        backend_log("EF: saved-map read detour SKIPPED -- ReadLocalSavedMapText or IdStrAssignCStr unresolved; "
                    "opening rawmaps is off");
        return 0;
    }
    g_assign_cstr = (ef_assign_cstr_fn)assign_cstr_fn;
    tramp = hook_prepare(read_text_fn, (void *)ef_read_text_detour, EF_READ_TEXT_STOLEN);
    if (tramp == NULL) {
        backend_log("EF: saved-map read detour FAIL -- trampoline preparation failed; opening rawmaps is off");
        return 0;
    }
    g_read_text_orig = (ef_read_text_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_read_text_orig = NULL;
        backend_log("EF: saved-map read detour commit failed; opening rawmaps is off");
        return 0;
    }
    backend_log("EF: saved-map read detour installed -- a rawmap reload never reads the map it borrows");
    return 1;
}

void *sh_editor_frame_target(void)
{
    return g_frame_entry;
}

int sh_editor_frame_can_rawmap_save(char *out_msg, int msg_capacity)
{
    const char *why = ef_rawmap_save_blocker();
    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    if (why) {
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, why, _TRUNCATE);
        return 0;
    }
    return 1;
}

int sh_editor_frame_request_rawmap_save_to(const char *destination, char *out_msg, int msg_capacity)
{
    const char *why;
    ef_save_request request = {0};
    int accepted = 0;
    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    AcquireSRWLockExclusive(&g_save_lock);
    why = ef_rawmap_save_blocker();
    if (why) goto done;
    if (destination && strlen(destination) >= sizeof request.destination) {
        why = "That save path is too long to use";
        goto done;
    }
    request.choose_target = destination && destination[0];
    if (request.choose_target) strcpy_s(request.destination, sizeof request.destination, destination);
    else sh_rawmap_get_paths(NULL, 0, request.destination, (int)sizeof request.destination);
    if (!sh_rawmap_dest_writable_now(request.destination, out_msg, msg_capacity)) goto done;
    request.generation = sh_rawmap_load_generation();
    request.map = ef_live_map((void *)g_editor_obj);
    if (!request.map) { why = EF_OPEN_A_MAP; goto done; }
    g_save_request = request;
    InterlockedExchange(&g_save_pending, 1);
    accepted = 1;
done:
    ReleaseSRWLockExclusive(&g_save_lock);
    if (out_msg && msg_capacity > 0 && why)
        strncpy_s(out_msg, (size_t)msg_capacity, why, _TRUNCATE);
    else if (out_msg && msg_capacity > 0 && accepted)
        _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE, "Saving the open map to %s.", request.destination);
    return accepted;
}

int sh_editor_frame_can_reload(char *out_msg, int msg_capacity)
{
    const char *why = ef_reload_blocker();
    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    if (!why && !sh_rawmap_load_is_safe())
        why = "Opening rawmaps is unavailable on this game build: Snapmap+ cannot stop DOOM's Save from "
              "overwriting the map a rawmap opens over";
    if (why) {
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, why, _TRUNCATE);
        return 0;
    }
    return 1;
}

int sh_editor_frame_request_reload(char *out_msg, int msg_capacity)
{
    ef_reload_request request;
    char msg[MAX_PATH + 192] = "";
    char rl[MAX_PATH + 256];

    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    memset(&request, 0, sizeof request);

    if (!sh_editor_frame_can_reload(msg, (int)sizeof msg) || !sh_rawmap_source_ok(msg, (int)sizeof msg)) {
        _snprintf_s(rl, sizeof rl, _TRUNCATE, "EF: rawmap open REFUSED -- %s", msg);
        backend_log(rl);
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, msg, _TRUNCATE);
        return 0;
    }

    /* WHICH map is borrowed does not matter: its files are never read (ef_read_text_detour), and
     * the branch answer stops the slot being WRITTEN. The newest complete save is taken because
     * the engine has certainly listed it. */
    if (!newest_saved_map_id(request.id, sizeof request.id)) {
        backend_log("EF: no saved map found under <Saved Games>\\id Software\\DOOM\\base\\savegame.user");
        if (out_msg && msg_capacity > 0)
            strncpy_s(out_msg, (size_t)msg_capacity,
                      "To open a rawmap, DOOM needs one saved map of yours: save any map once, then try again",
                      _TRUNCATE);
        return 0;
    }

    request.generation = sh_rawmap_load_generation();
    request.map = ef_live_map((void *)g_editor_obj);
    AcquireSRWLockExclusive(&g_reload_lock);
    if (request.map == NULL || InterlockedCompareExchange(&g_reload_pending, 0, 0) != 0) {
        ReleaseSRWLockExclusive(&g_reload_lock);
        if (out_msg && msg_capacity > 0)
            strncpy_s(out_msg, (size_t)msg_capacity, request.map ? "A rawmap is already opening" : EF_OPEN_A_MAP,
                      _TRUNCATE);
        return 0;
    }
    g_reload_request = request;
    InterlockedExchange(&g_reload_pending, 1);
    ReleaseSRWLockExclusive(&g_reload_lock);

    _snprintf_s(rl, sizeof rl, _TRUNCATE, "EF: rawmap open ACCEPTED -- queued through saved map %s", request.id);
    backend_log(rl);
    if (out_msg && msg_capacity > 0)
        strncpy_s(out_msg, (size_t)msg_capacity, "Opening the rawmap as a new map; Save will ask you to name it",
                  _TRUNCATE);
    return 1;
}
