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
#include <string.h>
#include <shlobj.h>

#include "editor_frame.h"
#include "hook.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "rawmap.h"       /* sh_rawmap_source_ok + the arm the reload brackets its own call with */

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
typedef void (*ef_add_branch_tag_fn)(void *map);

static editor_frame_fn g_frame_orig  = NULL;
static ef_load_map_fn  g_load_map    = NULL;
static ef_add_branch_tag_fn g_add_branch_tag = NULL;
/* The INLINE idSnapEditorLocal OBJECT, not a pointer to one -- it is in-place constructed at a data
 * global, exactly as iface_engine.c documents. So this address IS the `this` LoadMap wants; there is
 * no dereference step, and adding one would read the object's first field as a pointer. */
static uintptr_t       g_editor_obj  = 0;

static volatile LONG  g_faulted   = 0;
static volatile LONG  g_pending   = 0;      /* 1 = a reload is queued for the next good frame */
static volatile LONG  g_save_pending = 0;   /* 1 = a live rawmap save is queued for the next frame */
static volatile LONG  g_state     = SH_RELOAD_IDLE;
static volatile LONG  g_ticks     = 0;

static CRITICAL_SECTION g_msg_lock;
static int  g_msg_ready = 0;
static char g_msg[192]  = "";

/* The 20-hex save-directory id the queued reload will hand to LoadMap. Captured when the request is
 * made (a disk walk is not something to do on a frame) and consumed on the frame that services it. */
static char g_reload_id[64] = "";

static void ef_set_msg(const char *s)
{
    if (!g_msg_ready || !s) return;
    EnterCriticalSection(&g_msg_lock);
    strncpy_s(g_msg, sizeof g_msg, s, _TRUNCATE);
    LeaveCriticalSection(&g_msg_lock);
}

/* ------------------------------------------------------------------ the saved-map id ------------
 * LoadMap names a map by its save-directory id. Local SnapMap saves live one folder per map at
 *
 *     <Saved Games>\id Software\DOOM\base\savegame.user\<steam id>\SNAPMAPS<20 hex>
 *
 * and that 20-hex suffix IS the id. Two things here were wrong on the first attempt and both
 * produced the same unhelpful "no local saved map was found":
 *
 *   - THE STEAM-ID LEVEL. The maps are not directly under savegame.user; each account gets its own
 *     numeric folder in between, alongside PROFILE and the GAME-AUTOSAVE slots. Missing that level
 *     means the glob matches nothing on every machine, not just an unusual one.
 *   - THE ROOT. "Saved Games" is a known folder in its own right and can be relocated, and deriving
 *     it as the parent of Documents is wrong the moment Documents is redirected -- which OneDrive
 *     does by default. Ask for the folder itself, and only fall back to the profile root.
 *
 * The reload does NOT name one of these. It names a scratch map we mint ourselves -- see the
 * scratch-map note below for why, and for the reasoning this file used to carry here and had wrong.
 * The walk stays because minting copies an existing save, and because Save Rawmap reads one. */

/* The id must be exactly 20 hex digits. Checked because it is about to be handed to an engine
 * function as a name: anything else means we misread the directory layout, and finding that out
 * here is much better than finding it out inside the engine's loader. */
static int valid_map_id(const char *s)
{
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        int hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!hex || n >= 20) return 0;
    }
    return n == 20;
}

static int saved_games_root(char *out, size_t cap)
{
    PWSTR wide = NULL;
    /* FOLDERID_SavedGames. Spelled out rather than pulled from knownfolders.h's extern so this
     * compiles the same regardless of which SDK headers the build picks up. */
    static const GUID kSavedGames =
        { 0x4C5C32FFu, 0xBB9D, 0x43b0, { 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4 } };

    if (SUCCEEDED(SHGetKnownFolderPath(&kSavedGames, 0, NULL, &wide)) && wide) {
        int n = WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)cap, NULL, NULL);
        CoTaskMemFree(wide);
        if (n > 0) return 1;
    }
    /* Fallback: the profile root plus the default name. Correct on any machine that has not moved
     * the folder, which is the overwhelming majority. */
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, out))) {
        strncat_s(out, cap, "\\Saved Games", _TRUNCATE);
        return 1;
    }
    return 0;
}

static int newest_saved_map_id(char *out, size_t cap)
{
    char root[MAX_PATH], base[MAX_PATH], glob[MAX_PATH];
    WIN32_FIND_DATAA acct;
    HANDLE ha;
    FILETIME best;
    int found = 0;

    if (!saved_games_root(root, sizeof root)) return 0;
    _snprintf_s(base, sizeof base, _TRUNCATE,
                "%s\\id Software\\DOOM\\base\\savegame.user", root);

    memset(&best, 0, sizeof best);

    /* Walk the account folders. Usually one, but a shared machine can have several, and the newest
     * map across all of them is a better answer than picking the first account we happen to see. */
    _snprintf_s(glob, sizeof glob, _TRUNCATE, "%s\\*", base);
    ha = FindFirstFileA(glob, &acct);
    if (ha == INVALID_HANDLE_VALUE) return 0;
    do {
        WIN32_FIND_DATAA fd;
        HANDLE hm;
        if (!(acct.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (acct.cFileName[0] == '.') continue;

        _snprintf_s(glob, sizeof glob, _TRUNCATE, "%s\\%s\\SNAPMAPS*", base, acct.cFileName);
        hm = FindFirstFileA(glob, &fd);
        if (hm == INVALID_HANDLE_VALUE) continue;
        do {
            char probe[MAX_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!valid_map_id(fd.cFileName + 8)) continue;   /* skip "SNAPMAPS" */

            /* A correctly-named folder is NOT a saved map. Deleting a map in DOOM removes
             * game.details and map.decl but LEAVES both .verify sidecars, so the tree fills with
             * folders that pass every name check and hold no data -- 194 of them out of 249 on the
             * machine this was found on. Picking one of those as "the newest saved map" made Save
             * Rawmap fail with a nonsense reason and would have made a mint copy an empty folder.
             * Require the two files that actually constitute a save. */
            _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s\\%s\\%s\\map.decl",
                        base, acct.cFileName, fd.cFileName);
            if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) continue;
            _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s\\%s\\%s\\game.details",
                        base, acct.cFileName, fd.cFileName);
            if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) continue;

            if (found && CompareFileTime(&fd.ftLastWriteTime, &best) <= 0) continue;
            best = fd.ftLastWriteTime;
            strncpy_s(out, cap, fd.cFileName + 8, _TRUNCATE);
            found = 1;
        } while (FindNextFileA(hm, &fd));
        FindClose(hm);
    } while (FindNextFileA(ha, &acct));
    FindClose(ha);

    return found && out[0] != '\0';
}

int sh_editor_frame_saved_map_dir(char *out, size_t cap, char *out_id, size_t id_cap)
{
    char root[MAX_PATH], id[64];

    if (out && cap) out[0] = '\0';
    if (out_id && id_cap) out_id[0] = '\0';
    if (!out || cap == 0) return 0;

    if (!newest_saved_map_id(id, sizeof id)) return 0;
    if (!saved_games_root(root, sizeof root)) return 0;

    /* One account folder holds the map, and newest_saved_map_id already walked every account to find
     * it -- but it reports the id, not which account it came from. Re-derive the folder by asking the
     * filesystem which account has THIS id, rather than by threading the account name back out: the
     * id is 20 hex digits, so a collision across accounts is not a case worth designing for. */
    {
        char base[MAX_PATH], glob[MAX_PATH];
        WIN32_FIND_DATAA acct;
        HANDLE ha;
        int found = 0;

        _snprintf_s(base, sizeof base, _TRUNCATE,
                    "%s\\id Software\\DOOM\\base\\savegame.user", root);
        _snprintf_s(glob, sizeof glob, _TRUNCATE, "%s\\*", base);

        ha = FindFirstFileA(glob, &acct);
        if (ha == INVALID_HANDLE_VALUE) return 0;
        do {
            char cand[MAX_PATH];
            if (!(acct.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (acct.cFileName[0] == '.') continue;
            _snprintf_s(cand, sizeof cand, _TRUNCATE, "%s\\%s\\SNAPMAPS%s",
                        base, acct.cFileName, id);
            if (GetFileAttributesA(cand) == INVALID_FILE_ATTRIBUTES) continue;
            strncpy_s(out, cap, cand, _TRUNCATE);
            found = 1;
            break;
        } while (FindNextFileA(ha, &acct));
        FindClose(ha);
        if (!found) return 0;
    }

    if (out_id && id_cap) strncpy_s(out_id, id_cap, id, _TRUNCATE);
    return 1;
}

/* The scratch-map machinery that used to sit here is GONE, and deliberately not kept behind an
 * #if 0. It minted a map id and copied a save folder so a reload could target a slot that was ours
 * rather than one of the person's. The premise was wrong: DISCONFIRMED 2026-09-07, a copied folder
 * is not a second map, because the engine keys a save by the `localId` INSIDE its game.details and
 * a copy still names its source. Both list entries opened the same map.
 *
 * It is deleted rather than parked because it was ~200 lines encoding a false belief, and the thing
 * worth keeping from it is the disconfirmation, which lives in campaign rawmap-io-contract T2. The
 * engine mints its own slot now -- see the tag note below -- so nothing here needs to.
 *
 * `map_dir_by_id`, `mint_map_id`, `copy_save_folder` and the scratch-id file went with it. */

/* ------------------------------------------------- mark a substituted map as new ----------------
 * THE POINT OF THIS WHOLE FILE, in one call.
 *
 * A rawmap parsed into an open map inherits that map's IDENTITY -- LoadMap named an existing save, so
 * the editor holds a real map and the next Save writes over it with no prompt. Two earlier attempts
 * to fix that failed on the same wrong assumption: that identity is something we can supply. It is
 * not. Minting a save needs a game.details `checksum=` and a 40-byte .verify sidecar, and this
 * project can generate neither (13 hash algorithms x 6 byte ranges against 54 real saves: no match;
 * the community .verify KDF did not reproduce over 864 combinations).
 *
 * The engine already has the behaviour we want and gates it on ONE BIT OF MAP STATE. The Save
 * command asks the open map for the tag "map:new" or "map:branch"; if either is present, Save routes
 * into SAVE AS -- prompt for a name, then CreateLocalSavedMapInternal mints a fresh slot and the
 * ENGINE writes the checksum and the sidecars. That is the path Branch and New-from-Template take.
 *
 * So we stop trying to forge identity and just tell the truth: this map is derived from another one.
 * Set the tag, and the person is asked to name it, and their original is never written.
 *
 * WHY ON A FRAME, not in the swap detour: the detour runs inside DeserializeFromJson, which is only
 * part of the load. Whether the record's tags are applied to the map before or after that parse is
 * not established, and a tag set mid-load could be overwritten by the rest of it. A frame after the
 * load has finished has no such ordering question -- and the frame hook is already this project's
 * one sanctioned main-thread execution point.
 *
 * WHY map:branch rather than map:new: both gate the same prompt, and only map:branch has a
 * ready-made single-argument add-if-absent function in the engine. Reaching for map:new would mean
 * hand-building an idStr into an idStrList, i.e. this project's own allocator handling on an engine
 * object, for no behavioural difference. */
static unsigned long g_seen_swaps  = 0;   /* substituted parses already accounted for */
static volatile LONG g_tag_faulted = 0;

/* Called from the frame hook, after the engine's own Think returned. Cheap on every frame: one
 * counter compare, and nothing else unless a substituted load actually landed. */
static void ef_mark_substituted_map(void *editor)
{
    unsigned long now;
    void *map = NULL;

    if (g_add_branch_tag == NULL) return;
    if (InterlockedCompareExchange(&g_tag_faulted, 0, 0) != 0) return;

    now = sh_rawmap_swap_complete_count();
    if (now == g_seen_swaps) return;          /* the common case: nothing new */

    __try {
        map = *(void *const *)((const unsigned char *)editor + ED_MAP_PTR_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_tag_faulted, 1);
        backend_log("EF: tag-as-new ABANDONED -- could not read the editor map pointer");
        return;
    }

    /* No map yet means the load has not finished installing it. Leave the counter alone and try
     * again next frame rather than consuming the event against a map that is not there. */
    if (map == NULL) return;

    __try {
        g_add_branch_tag(map);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_tag_faulted, 1);
        backend_log("EF: tag-as-new FAULTED inside the engine tag add; disabled for this session");
        return;
    }

    g_seen_swaps = now;
    {
        char line[160];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "EF: substituted map tagged map:branch -- Save will ask for a new name [swap #%lu]",
                    now);
        backend_log(line);
    }
}

/* ------------------------------------------------- save the OPEN map as a rawmap ----------------
 * Serialising the live map is an engine touch: it reads editor state and allocates through the
 * engine's allocator, so it belongs on a frame, not on the UI thread that the click arrives on.
 * Same discipline as the reload -- the click queues, the frame does it. */
static void ef_service_rawmap_save(void *editor)
{
    void *map = NULL;
    char msg[192] = "";

    __try {
        map = *(void *const *)((const unsigned char *)editor + ED_MAP_PTR_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("EF: rawmap save ABANDONED -- could not read the editor map pointer");
        return;
    }

    if (map == NULL) {
        backend_log("EF: rawmap save ABANDONED -- no map is open");
        return;
    }

    /* The result is reported through the log and the status readout the page refreshes afterwards.
     * There is no way to hand it back to the click: that returned a frame ago. */
    (void)sh_rawmap_write_from_live(map, msg, (int)sizeof msg, NULL);
}

/* ------------------------------------------------------------------ the reload ------------------ */

/* Read the live editor pointer. The slot holds the object; the object is null before the editor
 * subsystem comes up. */
static void *ef_editor(void)
{
    void *ed = NULL;
    if (!g_editor_obj) return NULL;
    __try {
        ed = (void *)g_editor_obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    return ed;
}

/* Is this frame a safe one to reload on? Checked on the frame itself, never at request time: the
 * editor can leave a usable state between the click and the frame that services it. */
static int ef_can_reload(void *editor, const char **why)
{
    __try {
        const unsigned char *e = (const unsigned char *)editor;
        if (*(void *const *)(e + ED_MAP_PTR_OFF) == NULL) { *why = "no map is open"; return 0; }
        if (*(const char *)(e + ED_DEACT_OFF) != 0)       { *why = "the editor is shutting down"; return 0; }
        if (*(const int *)(e + ED_STATE_OFF) == 0)        { *why = "the editor has no state running"; return 0; }
        if (*(const int *)(e + ED_SUBSTATE_OFF) == 5)     { *why = "the editor is mid-transition"; return 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "the editor object could not be read";
        return 0;
    }
    return 1;
}

/* The idStr LoadMap actually needs.
 *
 * It reads ONLY the data pointer at +0x10. We still set the length at +0x08, because a future
 * engine path that does look at it would otherwise read whatever was on the stack, and a wrong
 * length is the kind of bug that shows up months later as a truncated name. A 0x30-byte zeroed
 * block is the engine's own idStr footprint; nothing here constructs or frees an engine string, so
 * there is no allocator interaction to get wrong. */
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
    ef_idstr s;
    const char *why = "";
    char reason[128];
    int rc = 0;
    int prev_armed;
    char line[256];

    if (!ef_can_reload(editor, &why)) return;   /* stay pending; try again next frame */

    /* Re-check the SOURCE on the frame, not the arm. The request may have sat in the queue while the
     * staged file was moved or replaced, and substituting bytes we have not just seen accepted is
     * the case worth refusing. (The arm is ours to set -- see the window below.) */
    if (!sh_rawmap_source_ok(reason, (int)sizeof reason)) {
        InterlockedExchange(&g_pending, 0);
        InterlockedExchange(&g_state, SH_RELOAD_FAILED);
        ef_set_msg(reason);
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "EF: reload ABANDONED -- the staged rawmap is no longer usable (%s)", reason);
        backend_log(line);
        return;
    }

    memset(&s, 0, sizeof s);
    s.data = g_reload_id;
    s.len  = (int)strlen(g_reload_id);

    InterlockedExchange(&g_pending, 0);   /* consume before the call: never retry a call that faults */

    /* THE ARM WINDOW. The swap is armed for the duration of this one LoadMap call and put back
     * exactly as we found it, so nobody has to tick a checkbox and no unrelated map load is caught.
     *
     * A window, not a one-shot count, because it assumes nothing about how many parses LoadMap
     * makes. Measured it makes exactly one (six reloads, two sessions, each a clean arm -> one fire
     * -> disarm), so a one-shot would work today and would break silently the day that changed.
     * Bracketing the call covers however many parses it makes and no more.
     *
     * Restored on EVERY path out, the fault path included. Leaving the gate on after a fault would
     * silently substitute the person's next ordinary map load, which is the mode this removes. */
    prev_armed = sh_rawmap_swap_is_armed();
    if (!prev_armed) sh_rawmap_swap_arm(1);

    __try {
        rc = g_load_map(editor, &s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!prev_armed) sh_rawmap_swap_arm(0);
        InterlockedExchange(&g_faulted, 1);
        InterlockedExchange(&g_state, SH_RELOAD_FAILED);
        ef_set_msg("the reload faulted; the frame hook is off for this session");
        backend_log("EF: reload FAULTED -- frame hook disabled for the session");
        return;
    }

    if (!prev_armed) sh_rawmap_swap_arm(0);

    /* LoadMap returns 0 on the success path (it returns 9 when the editor asked to deactivate
     * mid-load, and passes its inner loader's code through otherwise). */
    if (rc == 0) {
        InterlockedExchange(&g_state, SH_RELOAD_DONE);
        ef_set_msg("the map was reloaded");
    } else {
        InterlockedExchange(&g_state, SH_RELOAD_FAILED);
        _snprintf_s(line, sizeof line, _TRUNCATE, "the engine refused the reload (code %d)", rc);
        ef_set_msg(line);
    }
    _snprintf_s(line, sizeof line, _TRUNCATE, "EF: reload returned %d (id=%s)", rc, g_reload_id);
    backend_log(line);
}

/* ------------------------------------------------------------------ the detour ------------------ */

static void sh_editor_frame_detour(void *editor, void *arg)
{
    g_frame_orig(editor, arg);     /* the engine's own frame, first and unconditionally */

    InterlockedIncrement(&g_ticks);

    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0) return;
    if (editor == NULL) return;

    /* Runs on EVERY frame, unlike the reload below, because the map it has to mark arrives from a
     * load the PERSON started -- there is no request to wait for. It is a counter compare on the
     * frames where nothing happened. */
    ef_mark_substituted_map(editor);

    if (InterlockedExchange(&g_save_pending, 0) != 0) ef_service_rawmap_save(editor);

    if (InterlockedCompareExchange(&g_pending, 0, 0) == 0) return;   /* the common frame: one read */
    if (g_load_map == NULL) return;

    ef_service_reload(editor);
}

/* ------------------------------------------------------------------ install + API --------------- */

int sh_editor_frame_install(void *frame_fn, int status_ok, void *load_map_fn,
                            void *add_branch_tag_fn, const uint8_t *module_base)
{
    glb_status st = GLB_OK;
    char line[256];
    void *tramp;

    if (!g_msg_ready) { InitializeCriticalSection(&g_msg_lock); g_msg_ready = 1; }

    /* Independent of the frame hook succeeding: it is only read from inside the hook, so a null
     * here costs the tag and nothing else. Logged either way -- a silently missing tag would look
     * exactly like the engine ignoring it, which is the wrong thing to go debugging. */
    g_add_branch_tag = (ef_add_branch_tag_fn)add_branch_tag_fn;
    backend_log(add_branch_tag_fn != NULL
                ? "EF: map:branch tagger resolved -- substituted maps will save as new maps"
                : "EF: map:branch tagger NOT resolved -- a substituted map would overwrite its slot");

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

    tramp = install_inline_hook(frame_fn, (void *)sh_editor_frame_detour, EDITOR_FRAME_STOLEN);
    if (tramp == NULL) {
        backend_log("EF: editor-frame hook FAIL -- install_inline_hook returned NULL");
        return 0;
    }
    g_frame_orig = (editor_frame_fn)tramp;
    g_load_map   = (ef_load_map_fn)load_map_fn;

    _snprintf_s(line, sizeof line, _TRUNCATE,
        "EF: editor-frame hook installed at %p (trampoline %p, stolen %d); loadmap=%p; editor=%p",
        frame_fn, tramp, EDITOR_FRAME_STOLEN, load_map_fn, (void *)g_editor_obj);
    backend_log(line);

    /* Baseline the swap counter at install. Without this, a swap that fired before the editor came
     * up would read as "new" on the first frame and tag whatever map happened to be open. */
    g_seen_swaps = sh_rawmap_swap_complete_count();
    return 1;
}

int sh_editor_frame_request_rawmap_save(char *out_msg, int msg_capacity)
{
    const char *why = NULL;
    void *ed;

    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';

    if (g_frame_orig == NULL)                                    why = "this build has no editor-frame hook";
    else if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0)  why = "the frame hook faulted earlier this session";
    else if (g_editor_obj == 0)                                  why = "the editor could not be located";
    else if (!sh_rawmap_live_serialize_ready())                   why = "this build cannot serialize the open map";
    else if (InterlockedCompareExchange(&g_save_pending, 0, 0) != 0) why = "a rawmap save is already waiting for the next frame";
    if (why == NULL) {
        ed = ef_editor();
        if (ed == NULL) why = "the editor is not ready";
        else {
            void *map = NULL;
            __try {
                map = *(void *const *)((const unsigned char *)ed + ED_MAP_PTR_OFF);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                map = NULL;
            }
            if (map == NULL) why = "no map is open";
        }
    }
    if (why) {
        char rl[256];
        _snprintf_s(rl, sizeof rl, _TRUNCATE, "EF: rawmap save REFUSED -- %s", why);
        backend_log(rl);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, why, _TRUNCATE);
        return 0;
    }

    InterlockedExchange(&g_save_pending, 1);
    if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                           "saving the open map as a rawmap", _TRUNCATE);
    return 1;
}

int sh_editor_frame_request_reload(char *out_msg, int msg_capacity)
{
    const char *why = NULL;
    char msg[192] = "";
    void *ed;

    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';

    if (g_frame_orig == NULL)                                    why = "this build has no editor-frame hook";
    else if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0)  why = "the frame hook faulted earlier this session";
    else if (g_load_map == NULL)                                 why = "the engine's map loader was not found";
    else if (g_editor_obj == 0)                                  why = "the editor could not be located";
    else if (InterlockedCompareExchange(&g_pending, 0, 0) != 0)  why = "a reload is already waiting for the next frame";
    /* THE SAFETY INTERLOCK, and the only reason this is allowed to borrow a slot at all.
     *
     * The reload hands LoadMap an EXISTING saved map, so the editor adopts that map's identity --
     * that is what made it destructive twice. What makes it safe is the map:branch tag: with it set,
     * the editor's Save cannot write the borrowed slot, because Save routes into Save As, prompts for
     * a name, and mints a new slot through the engine's own CreateLocalSavedMapInternal. The borrowed
     * map is READ and never written.
     *
     * So the tag is not a nicety here, it is the whole safety property. No tagger, no reload -- and
     * refusing is not a hardship, because staging still works and substitutes into a map the person
     * opened themselves. */
    else if (g_add_branch_tag == NULL)                           why = "this build cannot mark a loaded rawmap as a new map, "
                                                                       "so loading one in place could overwrite a saved map";
    else if (InterlockedCompareExchange(&g_tag_faulted, 0, 0) != 0) why = "marking a rawmap as a new map faulted earlier this "
                                                                          "session, so loading one in place is no longer safe";
    if (why) {
        /* LOG EVERY REFUSAL. These used to return the reason to the UI and write nothing, so a
         * person reporting "it did not load" left no trace to read afterwards -- which cost a
         * round trip to find out it was simply "no map is open". */
        char rl[256];
        _snprintf_s(rl, sizeof rl, _TRUNCATE, "EF: open-as-new REFUSED -- %s", why);
        backend_log(rl);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, why, _TRUNCATE);
        return 0;
    }

    if (!sh_rawmap_source_ok(msg, (int)sizeof msg)) {
        char rl[256];
        _snprintf_s(rl, sizeof rl, _TRUNCATE, "EF: open-as-new REFUSED -- staged source: %s", msg);
        backend_log(rl);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, msg, _TRUNCATE);
        return 0;
    }

    ed = ef_editor();
    if (ed == NULL || !ef_can_reload(ed, &why)) {
        {
            char rl[256];
            _snprintf_s(rl, sizeof rl, _TRUNCATE, "EF: open-as-new REFUSED -- editor: %s",
                        why ? why : "not ready");
            backend_log(rl);
        }
        /* Refuse now rather than queue a request that can only be declined: "open a map first" is
         * a useful answer, and a request that sits pending forever is not. */
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, why ? why : "the editor is not ready", _TRUNCATE);
        return 0;
    }

    /* WHICH map is borrowed genuinely does not matter now -- and note that this file once said those
     * words for the wrong reason, so they are worth being exact about. It does not matter because the
     * tag stops the slot being WRITTEN, not because the swap replaces its content. Content was never
     * the damage; identity was. The newest save is used only because it is certain to be loadable. */
    if (!newest_saved_map_id(g_reload_id, sizeof g_reload_id)) {
        char root[MAX_PATH] = "", line[MAX_PATH + 128];
        saved_games_root(root, sizeof root);
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "EF: no saved map found under %s\\id Software\\DOOM\\base\\savegame.user"
                    "\\<account>\\SNAPMAPS*", root);
        backend_log(line);
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "no local saved map was found to load through -- save any map in DOOM once",
                               _TRUNCATE);
        return 0;
    }

    InterlockedExchange(&g_state, SH_RELOAD_PENDING);
    ef_set_msg("waiting for the next editor frame");
    InterlockedExchange(&g_pending, 1);
    {
        char rl[256];
        _snprintf_s(rl, sizeof rl, _TRUNCATE,
                    "EF: open-as-new ACCEPTED -- queued through saved map %s", g_reload_id);
        backend_log(rl);
    }
    if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "opening as a new map", _TRUNCATE);
    return 1;
}

sh_reload_state sh_editor_frame_reload(char *out_msg, int msg_capacity)
{
    if (out_msg && msg_capacity > 0) {
        out_msg[0] = '\0';
        if (g_msg_ready) {
            EnterCriticalSection(&g_msg_lock);
            strncpy_s(out_msg, (size_t)msg_capacity, g_msg, _TRUNCATE);
            LeaveCriticalSection(&g_msg_lock);
        }
    }
    return (sh_reload_state)InterlockedCompareExchange(&g_state, 0, 0);
}

unsigned long sh_editor_frame_ticks(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_ticks, 0, 0);
}
