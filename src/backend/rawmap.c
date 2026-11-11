#include "map_bounds.h"
#include "map_render.h"
#include "map_settings.h"
#include "world_area_cell.h"
/* DeserializeFromJson detour: prepare every map's JSON, then call the native
 * parser. Inside the editor reload that opens a rawmap (editor_frame.c), the
 * editor's parse reads the rawmap's checked text instead of the borrowed saved
 * map's; no other parse is ever substituted. Paths are UTF-8 throughout and
 * every file call is the wide Windows one.
 */
#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathW */
#include "rawmap.h"
#include "feature_health.h"
#include "hook.h"
#include "backend_log.h"
#include "map_carrier.h"
#include "modmanifest.h"
#include "modexport.h"   /* a .zip rawmap is an export that carries its mods */
#include "plain_reason.h"
#include "modpublished.h"
#include "modserve.h"     /* the loaded map's place */
#include "modui.h"        /* SH_MODUI_SCRIPTS_OFF */
#include "signatures.h"
#include "config.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "nav_regions.h"
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
static volatile LONG g_swap_count = 0;
static volatile LONG g_load_generation = 0;
static SRWLOCK g_paths_lock = SRWLOCK_INIT;

/* Load source override; otherwise %LOCALAPPDATA%/snapmap-plus/rawmap.json. */
static char g_src_path[SH_RAWMAP_PATH_MAX] = {0};

int sh_rawmap_path_wide(const char *utf8, wchar_t *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = L'\0';
    if (!utf8 || !utf8[0] || cap > INT_MAX) return 0;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, (int)cap) > 0;
}

int sh_rawmap_path_utf8(const wchar_t *wide, char *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!wide || !wide[0] || cap > INT_MAX) return 0;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) > 0) return 1;
    out[0] = '\0';
    return 0;
}

static DWORD path_attributes(const char *utf8)
{
    wchar_t w[MAX_PATH];
    return sh_rawmap_path_wide(utf8, w, MAX_PATH) ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
}

static HANDLE path_open_read(const char *utf8)
{
    wchar_t w[MAX_PATH];
    if (!sh_rawmap_path_wide(utf8, w, MAX_PATH)) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* Both default paths are the same file in the shared application data root. */
static void default_rawmap_path(char *out, size_t cap)
{
    wchar_t base[MAX_PATH];
    char root[SH_RAWMAP_PATH_MAX];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)) &&
        sh_rawmap_path_utf8(base, root, sizeof root))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\rawmap.json", root);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snapmap-plus\\rawmap.json");
}

static void resolve_source_path(char *out, size_t cap)
{
    AcquireSRWLockShared(&g_paths_lock);
    strncpy_s(out, cap, g_src_path, _TRUNCATE);
    ReleaseSRWLockShared(&g_paths_lock);
    if (!out[0]) default_rawmap_path(out, cap);
}

static void rawmap_tell(const char *text);

/* A held load's text: the bytes the hold checked (for a .zip, the map its
 * import returned), kept so the load the hold's resume starts parses them
 * instead of reading the source again. A ZIP is imported once, at the hold,
 * not again at the resume. It is ready only once the hold resumed
 * (sh_rawmap_reopen_after_mods); any other read of the source drops it. */
static SRWLOCK g_held_lock = SRWLOCK_INIT;
static char *g_held;                   /* process heap */
static size_t g_held_len;
static char g_held_path[SH_RAWMAP_PATH_MAX];
static int g_held_ready;

static void held_keep(char *text, size_t len, const char *path)
{
    char *old;
    AcquireSRWLockExclusive(&g_held_lock);
    old = g_held;
    g_held = text;
    g_held_len = len;
    strncpy_s(g_held_path, sizeof g_held_path, path, _TRUNCATE);
    g_held_ready = 0;
    ReleaseSRWLockExclusive(&g_held_lock);
    if (old) HeapFree(GetProcessHeap(), 0, old);
}

static void held_mark_ready(const char *path)
{
    AcquireSRWLockExclusive(&g_held_lock);
    if (g_held && !_stricmp(g_held_path, path)) g_held_ready = 1;
    ReleaseSRWLockExclusive(&g_held_lock);
}

/* The held text of `path` once its hold resumed (owned by the caller); any
 * other held text is dropped: this read is a new load. */
static char *held_take(const char *path, size_t *out_len)
{
    char *p, *drop = NULL;
    AcquireSRWLockExclusive(&g_held_lock);
    p = g_held;
    if (p && g_held_ready && !_stricmp(g_held_path, path)) *out_len = g_held_len;
    else { drop = p; p = NULL; }
    g_held = NULL;
    g_held_len = 0;
    g_held_path[0] = '\0';
    g_held_ready = 0;
    ReleaseSRWLockExclusive(&g_held_lock);
    if (drop) HeapFree(GetProcessHeap(), 0, drop);
    return p;
}

static int transport_start_import(const char *path, char *why, size_t why_cap);

/* Read a NUL-terminated process-heap buffer; caller frees it. Return NULL on
 * failure, with the reason and next step in `why` when one is given. A .zip
 * not imported yet starts its import off the game's thread (*importing set,
 * NULL returned); the load opens again when it is done.
 */
static char *read_source_file_why(size_t *out_len, char *why, size_t why_cap, int *importing)
{
    char buf_path[SH_RAWMAP_PATH_MAX];
    char *held;
    resolve_source_path(buf_path, sizeof buf_path);
    const char *path = buf_path;
    if (why && why_cap) why[0] = '\0';
    held = held_take(path, out_len);
    if (held) {
        char line[SH_RAWMAP_PATH_MAX + 96];
        _snprintf_s(line, sizeof line, _TRUNCATE, "RAWMAP: %s: the resumed load uses the map its hold read; not read again", path);
        backend_log(line);
        return held;
    }
    if (modexport_is_zip_path(path)) {
        *importing = transport_start_import(path, why, why_cap);
        return NULL;
    }

    HANDLE h = path_open_read(path);
    if (h == INVALID_HANDLE_VALUE) {
        if (why && why_cap) strncpy_s(why, why_cap, sh_reason_win32(GetLastError(), path, 0), _TRUNCATE);
        return NULL;
    }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)INT_MAX) {
        CloseHandle(h);
        if (why && why_cap)
            strncpy_s(why, why_cap, "the file is empty or larger than 2 GB, so it is not a saved map; save the map again",
                      _TRUNCATE);
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);   /* Reserve the trailing NUL. */
    if (!buf) {
        CloseHandle(h);
        if (why && why_cap) strncpy_s(why, why_cap, "the game ran out of memory reading it; restart the game and try again", _TRUNCATE);
        return NULL;
    }

    size_t got = 0;
    while (got < n) {
        DWORD chunk = (DWORD)((n - got) > 0x10000000 ? 0x10000000 : (n - got));
        DWORD rd = 0;
        if (!ReadFile(h, buf + got, chunk, &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    if (got != n) {
        HeapFree(GetProcessHeap(), 0, buf);
        if (why && why_cap) strncpy_s(why, why_cap, "the file could not be read to its end; try again", _TRUNCATE);
        return NULL;
    }
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

/* The refusal of a map whose Snapmap+ data a check could not finish: the
 * engine never gets a map that was not checked. */
#define RAWMAP_CHECK_FAULTED "Snapmap+ could not check this map, so it was not opened. Restart DOOM and open it again."

/* A map in an earlier shape (map_carrier.h, sh_map_old_shape) is refused
 * before anything reads it: one line, and the plain sentence held as the
 * load's refusal (sh_modmanifest_load_refuse). A check that faults refuses
 * too. Returns 1 when refused. */
static int old_shape_refused(const char *json, size_t len)
{
    int shape;
    __try {
        shape = sh_map_old_shape(json, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("CARRIER: the earlier-shape check FAULTED; the map is refused");
        sh_modmanifest_load_refuse(RAWMAP_CHECK_FAULTED);
        return 1;
    }
    if (shape == SH_MAP_OLD_NONE) return 0;
    backend_log(shape == SH_MAP_OLD_ROWS
                    ? "CARRIER: map refused: its mod rows name mods the way earlier builds did; nothing was read"
                    : "CARRIER: map refused: it carries the variables earlier builds wrote; nothing was read");
    sh_modmanifest_load_refuse(SH_MAP_OLD_REFUSAL);
    return 1;
}

/* Read the map manifest, keep it for the load, strip its rows before the
 * engine parses and activate the bundles it names. Runs on every parse.
 * Returns an owned process-heap buffer, or NULL to keep the input; a read
 * that faults sets *faulted and the map is refused (its mods were not
 * activated, and its placed entities would fail the editor's validator). */
static char *manifest_prepare_guarded(const char *json, size_t len, int *faulted)
{
    *faulted = 0;
    __try {
        size_t out_len = 0;
        return sh_modmanifest_prepare_load(json, len, &out_len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: map manifest read FAULTED; the map is refused");
        sh_modmanifest_load_refuse(RAWMAP_CHECK_FAULTED);
        *faulted = 1;
        return NULL;
    }
}

/* The map's place (modserve.h, DECISIONS 11): read from its member and made
 * the loaded map's before anything resolves its rows; a map without one is
 * the player's own and finds its mods at the root. */
static void place_prepare_guarded(const char *json, size_t len)
{
    __try {
        char place[SH_CARRIER_PLACE_CAP];
        if (sh_carrier_place(json, len, place, sizeof place) != 1) place[0] = 0;
        (void)sh_modserve_place_load(place);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: reading the map's place FAULTED; the map finds its mods at the root");
        (void)sh_modserve_place_load(NULL);
    }
}

/* Read the map's settings rows into the settings of the map being parsed and
 * strip them (map_settings.h). Returns an owned process-heap buffer, or NULL
 * to keep the input. */
static char *settings_prepare_guarded(const char *json, size_t len)
{
    __try {
        size_t out_len = 0;
        return sh_map_settings_prepare_load(json, len, &out_len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("SETTINGS: map settings read FAULTED; the map loads with both settings off");
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
/* The open map's identity after a swap: a new map, not saved yet, opened from
 * this file (its base name). The reload that opens it borrows the saved map
 * the editor had open, whose id the edit keeps until the first save, so the
 * Mods tab and `sh_mods map` ask this, not the edit's id (sh_rawmap_open_unsaved). */
static SRWLOCK g_open_lock = SRWLOCK_INIT;
static char g_open_name[SH_RAWMAP_PATH_MAX];
static volatile LONG g_open_unsaved = 0;
/* The saved-map id the swap's map borrowed, read from the edit the first time
 * the branch answer is asked; "" until then. While the edit still holds it the
 * open map is the rawmap's and Save asks for a name. Once a save gave the map
 * its own id it is a saved map like any other: the engine answers, so the next
 * Save writes that map instead of asking again. */
static char g_borrowed_id[64];
static sh_rawmap_edit_id_fn g_edit_id;

void sh_rawmap_set_edit_id_reader(sh_rawmap_edit_id_fn fn) { g_edit_id = fn; }

static void open_identity_set(const char *name)
{
    AcquireSRWLockExclusive(&g_open_lock);
    if (name) strncpy_s(g_open_name, sizeof g_open_name, name, _TRUNCATE);
    else g_open_name[0] = '\0';
    InterlockedExchange(&g_open_unsaved, name ? 1 : 0);
    ReleaseSRWLockExclusive(&g_open_lock);
}

/* A swap opened a map: its borrowed id is read again at the next answer. */
static void open_borrow_reset(void)
{
    AcquireSRWLockExclusive(&g_open_lock);
    g_borrowed_id[0] = '\0';
    ReleaseSRWLockExclusive(&g_open_lock);
}

/* 1 while the open map is the swap's and its edit still holds the borrowed id
 * (or the edit cannot be read). The first read records the id. */
static int open_still_borrowed(void)
{
    char id[64] = "";
    sh_rawmap_edit_id_fn fn = g_edit_id;
    int same = 1;
    if (!fn || fn(id, sizeof id) != 1 || !id[0]) return 1;
    AcquireSRWLockExclusive(&g_open_lock);
    if (!g_borrowed_id[0]) strncpy_s(g_borrowed_id, sizeof g_borrowed_id, id, _TRUNCATE);
    else same = !strcmp(g_borrowed_id, id);
    ReleaseSRWLockExclusive(&g_open_lock);
    return same;
}

int sh_rawmap_open_unsaved(char *name, size_t cap)
{
    int unsaved;
    AcquireSRWLockShared(&g_open_lock);
    unsaved = InterlockedCompareExchange(&g_open_unsaved, 0, 0) != 0;
    if (name && cap) strncpy_s(name, cap, unsaved ? g_open_name : "", _TRUNCATE);
    ReleaseSRWLockShared(&g_open_lock);
    return unsaved;
}

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
        InterlockedCompareExchange(&g_branch_tag, 0, 0) != 0) {
        if (open_still_borrowed()) return 1;
        /* Saved under its own id since: no longer the rawmap's borrowed map. */
        InterlockedExchange(&g_open_is_rawmap, 0);
        backend_log("RAWMAP: the map opened from a rawmap has its own saved-map id now; Save writes it");
    }
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
        backend_log("RAWMAP: branch answer SKIPPED -- IsBranchMap not found; rawmaps open only with "
                    "'sh_rawmaps overwrite on' on this build");
        return 0;
    }

    tramp = hook_prepare(is_branch_fn, (void *)sh_isbranch_detour, ISBRANCH_STOLEN);
    if (tramp == NULL) {
        backend_log("RAWMAP: branch answer FAIL -- trampoline preparation failed");
        return 0;
    }
    g_isbranch_orig = (is_branch_map_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_isbranch_orig = NULL;
        backend_log("RAWMAP: branch answer commit failed");
        return 0;
    }
    backend_log("RAWMAP: branch answer installed -- a map opened from a rawmap saves as a new map");
    return 1;
}

/* Remove the map's "snapmap-plus" member (map_carrier.h) once every part has
 * been read from it. Returns an owned process-heap buffer, or NULL to keep
 * the input. */
static char *carrier_strip_guarded(const char *json, size_t len)
{
    __try {
        size_t out_len = 0;
        return sh_carrier_strip(json, len, &out_len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("CARRIER: strip FAULTED; the engine parses the map with its member, which it ignores");
        return NULL;
    }
}

/* One load line naming the member's parts, or why it was refused. */
static void carrier_describe_guarded(const char *json, size_t len)
{
    __try {
        sh_carrier_view view;
        char what[200], line[260];
        if (!sh_carrier_view_read(json, len, &view) || view.state == SH_CARRIER_ABSENT) return;
        sh_carrier_describe(&view, what, sizeof what);
        _snprintf_s(line, sizeof line, _TRUNCATE, "CARRIER: map read: %s", what);
        backend_log(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("CARRIER: map read FAULTED");
    }
}

/* Prepare the chosen JSON before native parsing: refuse a map in an earlier
 * shape (*refused set, NULL returned, nothing read), else rebuild map-scoped
 * navigation state, read the map's Snapmap+ data from the "snapmap-plus"
 * member and remove the member last. Each failed step retains the preceding
 * buffer. Returns an owned process-heap buffer or NULL to use the original.
 */
static char *prepare_map_buffer(const char *json, int *refused)
{
    char *manifest, *settings, *current = NULL, *bare;
    size_t len = json ? strlen(json) : 0;
    int faulted = 0;

    *refused = 0;
    if (len == 0) return NULL;
    if (old_shape_refused(json, len)) { *refused = 1; return NULL; }
    carrier_describe_guarded(json, len);

    sh_navmesh_build_from_map(json, len);
    /* The regions the author marked, read from the same bytes and cleared the
     * same way. A map with no marked volume must not inherit the last map's. */
    sh_nav_bake_set_map(json, len);

    place_prepare_guarded(json, len);
    manifest = manifest_prepare_guarded(json, len, &faulted);
    if (faulted) { *refused = 1; return NULL; }
    if (manifest) current = manifest;
    /* The map's Snapmap+ settings (map_settings.h), held for the map. */
    settings = settings_prepare_guarded(current ? current : json, current ? strlen(current) : len);
    if (settings) {
        if (current) HeapFree(GetProcessHeap(), 0, current);
        current = settings;
    }
    /* Last: every part has been read; the engine never sees the member. */
    bare = carrier_strip_guarded(current ? current : json, current ? strlen(current) : len);
    if (!bare) return current;
    if (current) HeapFree(GetProcessHeap(), 0, current);
    return bare;
}

/* What kind of read the current parse is (modpublished.h), classified at the
 * detour's entry by the engine caller's return address. */
static __declspec(thread) int g_parse_kind;

/* ------------------------------------------------------------ opening a rawmap
 * A rawmap opens only through the editor reload (editor_frame.c): the reload
 * checks the file on its own frame, before LoadMap (sh_rawmap_prepare_reload),
 * and keeps the text it read, so the parse uses exactly the bytes that were
 * checked and a .zip is imported once. LoadMap borrows a saved map's id; only
 * the editor's parse inside that one call reads the rawmap's text instead
 * (sh_deser_detour). Every other parse -- a published map, the lobby, a map
 * opened from My Maps -- is the engine's own.
 *
 * A rawmap whose rows name a mod this machine does not have never reaches the
 * engine: SnapMap's validator rejects the placed entities as corruption and
 * the player is thrown out. Such a map goes to the rawmap hold
 * (modpublished.c), which offers the download when the mod store has the mods
 * and refuses with a notice when it does not. */
#ifdef SH_DIAG
/* The diagnostic build's record of the immediate-open path (sh_rawmap_diag_status). */
static void rawmap_diag_note(int new_seq, const char *state, const char *path, int rc,
                             const char *reason, size_t bytes, int mods);
#define RAWMAP_DIAG(seq, state, path, rc, reason, bytes, mods) \
    rawmap_diag_note((seq), (state), (path), (rc), (reason), (bytes), (mods))
#else
#define RAWMAP_DIAG(seq, state, path, rc, reason, bytes, mods) ((void)0)
#endif

static char *g_prepared;               /* process heap; the text the reload checked */
static size_t g_prepared_len;
static char g_prepared_path[SH_RAWMAP_PATH_MAX];
static volatile LONG g_in_reload;      /* 1 between sh_rawmap_prepare_reload and _done */

static void rawmap_base_name(const char *path, char *out, size_t cap)
{
    const char *b = path, *p;
    if (!out || !cap) return;
    out[0] = '\0';
    if (!path) return;
    for (p = path; *p; ++p) if (*p == '\\' || *p == '/') b = p + 1;
    strncpy_s(out, cap, b, _TRUNCATE);
}

/* The rows the rawmap names that nothing serves, or (size_t)-1 when the
 * check faulted: the map is then refused, never handed to the engine. */
#define RAWMAP_MISSING_FAULTED ((size_t)-1)
static size_t rawmap_missing_guarded(const char *text, size_t len, sh_modmanifest_list *rows, char *place,
                                     size_t place_cap)
{
    memset(rows, 0, sizeof *rows);
    __try {
        return sh_modpub_rawmap_missing(text, len, rows, place, place_cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: the rawmap's mod check FAULTED; the map is refused");
        memset(rows, 0, sizeof *rows);   /* leaked on purpose: its state is unknown */
        return RAWMAP_MISSING_FAULTED;
    }
}

static int rawmap_scripts_off_guarded(const sh_modmanifest_list *rows, const char *place)
{
    __try {
        return sh_modpub_rawmap_scripts_off(rows, place);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: the rawmap's scripts check FAULTED; the map is refused");
        return -1;
    }
}

static int rawmap_hold_guarded(const sh_modmanifest_list *rows, const char *name, const char *place)
{
    __try {
        return sh_modpub_rawmap_hold(rows, name, place);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: the rawmap hold FAULTED; the map was not loaded");
        return -1;
    }
}

static void prepared_drop(void)
{
    char *p = (char *)InterlockedExchangePointer((PVOID volatile *)&g_prepared, NULL);
    if (p) HeapFree(GetProcessHeap(), 0, p);
    g_prepared_len = 0;
    g_prepared_path[0] = '\0';
}

int sh_rawmap_prepare_reload(char *why, int cap)
{
    char path[SH_RAWMAP_PATH_MAX], name[SH_RAWMAP_PATH_MAX], cause[MODEXPORT_CAUSE_MAX], sentence[MODEXPORT_CAUSE_MAX + MAX_PATH];
    char place[SH_MODPUB_PLACE_CAP];
    sh_modmanifest_list rows;
    size_t len = 0, missing;
    char *text;
    int held, importing = 0;

    if (why && cap > 0) why[0] = '\0';
    prepared_drop();
    resolve_source_path(path, sizeof path);
    rawmap_base_name(path, name, sizeof name);
    /* A .zip is imported off the game's thread first: its mods install before
     * the rows are counted, and the load opens again when the import is done. */
    text = read_source_file_why(&len, cause, sizeof cause, &importing);
    if (text == NULL && importing) {
        _snprintf_s(sentence, sizeof sentence, _TRUNCATE, "Importing %s and its mods; it opens when that is done.", name);
        rawmap_tell(sentence);
        RAWMAP_DIAG(0, "importing", path, 0, sentence, 0, 0);
        if (why && cap > 0) strncpy_s(why, (size_t)cap, sentence, _TRUNCATE);
        return 0;
    }
    if (text == NULL) {
        char line[SH_RAWMAP_PATH_MAX + MODEXPORT_CAUSE_MAX + 96];
        if (!cause[0]) strcpy_s(cause, sizeof cause, "the file could not be read; try again");
        _snprintf_s(line, sizeof line, _TRUNCATE, "RAWMAP: open refused -- %s was not opened: %s", path, cause);
        backend_log(line);
        /* The importer's or Windows' own reason and next step reach the player
         * (console and toast): the page's Load Rawmap toast was shown before
         * this frame ran. */
        _snprintf_s(sentence, sizeof sentence, _TRUNCATE, "%s was not opened: %s.", name, cause);
        rawmap_tell(sentence);
        RAWMAP_DIAG(0, "refused", path, 0, sentence, 0, 0);
        if (why && cap > 0) strncpy_s(why, (size_t)cap, sentence, _TRUNCATE);
        return 0;
    }
    missing = rawmap_missing_guarded(text, len, &rows, place, sizeof place);
    if (missing == RAWMAP_MISSING_FAULTED) {
        HeapFree(GetProcessHeap(), 0, text);
        _snprintf_s(sentence, sizeof sentence, _TRUNCATE, "%s was not opened: %s", name, RAWMAP_CHECK_FAULTED);
        rawmap_tell(sentence);
        RAWMAP_DIAG(0, "refused", path, 0, sentence, len, 0);
        if (why && cap > 0) strncpy_s(why, (size_t)cap, sentence, _TRUNCATE);
        return 0;
    }
    /* A rawmap that came with its mods follows "Play maps whose mods include
     * scripts" at every open, however its mods got here. */
    if ((held = rawmap_scripts_off_guarded(&rows, place)) != 0) {
        sh_modmanifest_list_free(&rows);
        HeapFree(GetProcessHeap(), 0, text);
        _snprintf_s(sentence, sizeof sentence, _TRUNCATE, held < 0 ? "%s was not opened: %s" : "%s was not opened. %s",
                    name, held < 0 ? RAWMAP_CHECK_FAULTED : SH_MODUI_SCRIPTS_OFF);
        rawmap_tell(sentence);
        RAWMAP_DIAG(0, "refused", path, 0, sentence, len, 0);
        if (why && cap > 0) strncpy_s(why, (size_t)cap, sentence, _TRUNCATE);
        return 0;
    }
    if (missing) {
        char line[SH_RAWMAP_PATH_MAX + 128];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "RAWMAP: open held -- %s names %u mod(s) that are not installed; the engine never sees it",
                    path, (unsigned)missing);
        backend_log(line);
        held = rawmap_hold_guarded(&rows, name, place);
        sh_modmanifest_list_free(&rows);
        /* Its resume parses these bytes (a ZIP is not imported again). */
        if (held > 0) held_keep(text, len, path);
        else HeapFree(GetProcessHeap(), 0, text);
        if (why && cap > 0)
            _snprintf_s(why, (size_t)cap, _TRUNCATE, held > 0
                        ? "%s needs %u mod(s) you do not have; the game is checking whether it can download them"
                        : "%s needs %u mod(s) you do not have, so it was not opened", name, (unsigned)missing);
        RAWMAP_DIAG(0, "refused", path, 0, why && cap > 0 ? why : "", len, (int)missing);
        return 0;
    }
    RAWMAP_DIAG(0, "opening", path, 0, "", len, (int)rows.count);
    sh_modmanifest_list_free(&rows);
    g_prepared_len = len;
    strncpy_s(g_prepared_path, sizeof g_prepared_path, path, _TRUNCATE);
    InterlockedExchangePointer((PVOID volatile *)&g_prepared, text);
    InterlockedExchange(&g_in_reload, 1);
    return 1;
}

void sh_rawmap_reload_done(int rc, int faulted)
{
    char name[SH_RAWMAP_PATH_MAX], text[SH_RAWMAP_PATH_MAX + 160];
    rawmap_base_name(g_prepared_path, name, sizeof name);
    InterlockedExchange(&g_in_reload, 0);
    RAWMAP_DIAG(0, faulted ? "faulted" : rc ? "failed" : "opened", g_prepared_path, rc, "", g_prepared_len, -1);
    if (faulted) {
        _snprintf_s(text, sizeof text, _TRUNCATE,
                    "Opening %s failed with an engine error. Rawmaps cannot be opened again until you restart DOOM.",
                    name);
        rawmap_tell(text);
    } else if (rc != 0) {
        _snprintf_s(text, sizeof text, _TRUNCATE,
                    "DOOM did not open %s (error %d). Try again; restart DOOM if it keeps happening.", name, rc);
        rawmap_tell(text);
    }
    prepared_drop();
}

int sh_rawmap_reopen_after_mods(char *note, size_t cap)
{
    char path[SH_RAWMAP_PATH_MAX], name[SH_RAWMAP_PATH_MAX], msg[192] = "", line[SH_RAWMAP_PATH_MAX + 256];
    if (note && cap) note[0] = '\0';
    resolve_source_path(path, sizeof path);
    rawmap_base_name(path, name, sizeof name);
    held_mark_ready(path);   /* the reload below parses the bytes the hold checked */
    if (sh_editor_frame_request_reload(msg, (int)sizeof msg)) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "RAWMAP: %s opens again now that its mods are installed", path);
        backend_log(line);
        return 1;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "RAWMAP: %s not reopened after its mods were installed: %s", path, msg);
    backend_log(line);
    if (note && cap)
        _snprintf_s(note, cap, _TRUNCATE,
                    "The mods are installed. Open a map in the editor, then load %s again from File > Load Rawmap.",
                    name);
    return 1;
}

/* Parse the chosen bytes after preparation and record the map's render settings. */
static int deser_prepared(const char *json, void *out_map)
{
    char *prepared;
    int refused = 0;
    /* A read that is not a published map's is the editor's (My Maps, an opened
     * map, a rawmap): a row whose build is gone is served by another build of
     * its salt. A rawmap's missing mods were held before this parse. */
    sh_modmanifest_set_editor_load(g_parse_kind == SH_MODPUB_PARSE_OTHER);
    prepared = prepare_map_buffer(json, &refused);
    sh_modmanifest_set_editor_load(0);
    if (refused) {
        /* The engine never parses it: the open map keeps its settings, and the
         * gates see a failed parse whose refusal note they say. */
        sh_map_settings_loaded(0);
        sh_modpublished_parse_seen(g_parse_kind, json, out_map, 0);
        return 0;
    }
    int rc = g_deser_orig(prepared ? prepared : json, out_map);
    /* The parsed map's settings become the open map's before anything selects them. */
    sh_map_settings_loaded(rc);
    /* A parse that failed leaves the open map's place as it was. */
    if (!rc) (void)sh_modserve_place_restore();
    /* Beside the manifest the preparation kept: which read this was, the map it
     * filled, and the text, for the published-map and offline-play gates. */
    sh_modpublished_parse_seen(g_parse_kind, json, out_map, rc);
    if (rc) sh_map_render_loaded(out_map);
    if (rc) sh_map_bounds_loaded(out_map);
    if (rc) sh_world_area_cell_map_opened();
    if (prepared) HeapFree(GetProcessHeap(), 0, prepared);
    return rc;
}

/* 1 when this parse is the editor's parse inside a rawmap reload: the only
 * parse whose text is replaced. */
static int rawmap_parse_is_reload(int parse_kind)
{
    return InterlockedCompareExchange(&g_in_reload, 0, 0) != 0 && parse_kind == SH_MODPUB_PARSE_OTHER;
}

static int sh_deser_detour(const char *json, void *out_map)
{
    if (g_deser_orig == NULL) return 0;   /* defensive: should never happen once installed */
    /* The detour is entered by the prologue jump, so this is the engine caller's
     * return address: PublishedMapRead+0x12A or SnapMapReadSizeLimit+0x5D for a
     * published read. */
    g_parse_kind = sh_modpublished_classify(_ReturnAddress());
    sh_map_render_loaded(NULL);
    sh_map_bounds_loaded(NULL);

    /* A map is being opened, so the save file picked for the last one stops
     * applying. */
    InterlockedIncrement(&g_load_generation);
    sh_rawmap_clear_save_target_for_new_map();
    /* The editor may reuse the map object, so the placement grid's cut models
     * cannot be keyed on its pointer; they go with every parse. */
    sh_nav_bake_map_opened();

    if (rawmap_parse_is_reload(g_parse_kind)) {
        char line[SH_RAWMAP_PATH_MAX + 96], name[SH_RAWMAP_PATH_MAX];
        unsigned long n;
        int rc;
        if (g_prepared == NULL) {
            /* The reload borrowed another saved map's id: that map must not
             * open in the rawmap's place. */
            backend_log("RAWMAP: the reload's checked text is gone; the reload parses nothing");
            return 0;
        }
        n = (unsigned long)InterlockedIncrement(&g_swap_count);
        _snprintf_s(line, sizeof line, _TRUNCATE, "RAWMAP: opening %s (%zu bytes) [#%lu]",
                    g_prepared_path, g_prepared_len, n);
        backend_log(line);
        /* The checked text stays held until the reload ends, so a second parse
         * inside the same LoadMap reads the same bytes. */
        rc = deser_prepared(g_prepared, out_map);
        /* The open map came from a rawmap, which is what the branch answer keys off. */
        open_borrow_reset();
        InterlockedExchange(&g_open_is_rawmap, 1);
        rawmap_base_name(g_prepared_path, name, sizeof name);
        open_identity_set(rc ? name : NULL);
        return rc;
    }
    InterlockedExchange(&g_open_is_rawmap, 0);   /* the engine's own map: let the engine answer */
    open_identity_set(NULL);
    return deser_prepared(json, out_map);
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
        backend_log("RAWMAP: load detour SKIPPED -- DeserializeFromJson not resolved");
        return 0;
    }
    if (!deser_status_ok) {
        /* Refuse an already-hooked prologue; its detour bytes cannot be
         * stolen as native instructions.
         */
        backend_log("RAWMAP: load detour SKIPPED -- DeserializeFromJson resolved via hook-tolerant "
                    "fallback (prologue already hooked); not installing over an existing detour");
        return 0;
    }
    void *tramp = hook_prepare(deser_fn, (void *)sh_deser_detour, DESER_STOLEN);
    if (tramp == NULL) {
        backend_log("RAWMAP: load detour FAIL -- trampoline preparation failed");
        return 0;
    }
    g_deser_orig = (deser_fn_t)tramp;

    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_deser_orig = NULL;
        backend_log("RAWMAP: load detour commit failed; retained callbacks require restoration");
        return 0;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "RAWMAP: load detour installed at %p (trampoline %p, stolen %d)", deser_fn, tramp, DESER_STOLEN);
    backend_log(line);
    return 1;
}

int sh_rawmap_swap_set_source(const char *path)
{
    if (path && strlen(path) >= SH_RAWMAP_PATH_MAX) return 0;
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
/* Save processing. */

/* Call native SerializeToJson and preserve its bool result. Navigation
 * embedding may replace the output JSON before the caller consumes it.
 * Save Rawmap writes the open map through sh_rawmap_write_from_live; sh_pretty_on
 * changes only that file's layout.
 */
#include "cvars.h"        /* sh_cvar_value_int / B2_CVAR_SH_PRETTY_ON -- the rawmap file's pretty switch */
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
static sh_rawmap_save_observer volatile g_save_observer;   /* modpublish.c: the text a publish sends */

void sh_rawmap_set_save_observer(sh_rawmap_save_observer observer)
{
    g_save_observer = observer;
}
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

static volatile LONG     g_save_count = 0;
static volatile LONGLONG g_last_bytes   = 0;

/* What the last rawmap save wrote: the JSON, and the export beside it when the
 * map names mods. Read by the File menu's status and `sh_rawmaps`. */
static SRWLOCK g_report_lock = SRWLOCK_INIT;
static sh_rawmap_save_report g_report;

/* Rows in the text the last engine save produced, and the map it belonged to:
 * how the File menu knows to offer .zip first. */
static volatile LONG g_saved_rows = -1;
static volatile LONG g_saved_rows_generation = 0;

static void (*volatile g_notify_toast)(const char *title, const char *text);
static void (*volatile g_notify_print)(const char *line);

void sh_rawmap_set_notify(void (*toast)(const char *title, const char *text), void (*print)(const char *line))
{
    g_notify_toast = toast;
    g_notify_print = print;
}

/* The sentence waits for the main tick (sh_rawmap_poll_main): a save is
 * reported from inside the engine's serializer, which is no place to raise a
 * toast. One slot; a newer save's sentence replaces an unshown one. */
static SRWLOCK g_tell_lock = SRWLOCK_INIT;
static char g_tell[MODEXPORT_CAUSE_MAX + MAX_PATH];
static volatile LONG g_tell_pending;
static volatile LONG g_tell_console_only;   /* the page shows its own toast for this one */
static const char *volatile g_tell_title = "Rawmap";

static void tell_titled(const char *title, const char *text)
{
    char line[sizeof g_tell + 32];
    _snprintf_s(line, sizeof line, _TRUNCATE, "%s: %s", title, text);
    backend_log(line);
    AcquireSRWLockExclusive(&g_tell_lock);
    strncpy_s(g_tell, sizeof g_tell, text, _TRUNCATE);
    g_tell_title = title;
    ReleaseSRWLockExclusive(&g_tell_lock);
    InterlockedExchange(&g_tell_console_only, 0);
    InterlockedExchange(&g_tell_pending, 1);
}

static void rawmap_tell(const char *text) { tell_titled("Rawmap", text); }

void sh_rawmap_tell(const char *text)
{
    if (text && text[0]) rawmap_tell(text);
}

/* File > Load Rawmap refused a file: the backend log now, the console line on
 * the next main tick (the page already shows the toast). Mirrors the prefab
 * Import's "The prefab was not imported: ..." line. */
static void rawmap_tell_load_refused(const char *path, const char *reason)
{
    char line[900];
    _snprintf_s(line, sizeof line, _TRUNCATE, "RAWMAP: load of %s refused: %s", path ? path : "", reason ? reason : "");
    backend_log(line);
    AcquireSRWLockExclusive(&g_tell_lock);
    _snprintf_s(g_tell, sizeof g_tell, _TRUNCATE, "The rawmap was not loaded: %s.", reason && reason[0] ? reason : "it cannot be read");
    g_tell_title = "Rawmap";
    ReleaseSRWLockExclusive(&g_tell_lock);
    InterlockedExchange(&g_tell_console_only, 1);
    InterlockedExchange(&g_tell_pending, 1);
}

static void transport_poll(void);

void sh_rawmap_poll_main(void)
{
    char text[sizeof g_tell];
    const char *title;
    void (*toast)(const char *, const char *) = g_notify_toast;
    void (*print)(const char *) = g_notify_print;
    transport_poll();
    if (!InterlockedExchange(&g_tell_pending, 0)) return;
    AcquireSRWLockShared(&g_tell_lock);
    strncpy_s(text, sizeof text, g_tell, _TRUNCATE);
    title = g_tell_title;
    ReleaseSRWLockShared(&g_tell_lock);
    if (print) print(text);
    if (toast && !InterlockedCompareExchange(&g_tell_console_only, 0, 0)) toast(title, text);
}

void sh_rawmap_last_save(sh_rawmap_save_report *out)
{
    if (!out) return;
    AcquireSRWLockShared(&g_report_lock);
    *out = g_report;
    ReleaseSRWLockShared(&g_report_lock);
}

int sh_rawmap_open_map_mods(void)
{
    sh_modmanifest_list rows;
    size_t current = sh_modmanifest_current(&rows);
    LONG saved = InterlockedCompareExchange(&g_saved_rows, 0, 0);
    sh_modmanifest_list_free(&rows);
    /* A save of this map since it opened knows what it places now. */
    if (saved >= 0 && (unsigned long)InterlockedCompareExchange(&g_saved_rows_generation, 0, 0) ==
                      sh_rawmap_load_generation())
        return (int)saved;
    return (int)current;
}

int sh_rawmap_companion_zip_path(const char *json_path, char *out, size_t cap)
{
    size_t n;
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!json_path || !json_path[0] || modexport_is_zip_path(json_path)) return 0;
    n = strlen(json_path);
    if (n > 5 && _stricmp(json_path + n - 5, ".json") == 0) n -= 5;
    if (n + 5 > cap) return 0;
    memcpy(out, json_path, n);
    memcpy(out + n, ".zip", 5);
    return 1;
}

/* ------------------------------------------------------------ transport off the game's thread
 * A rawmap ZIP's import, a .zip save and the companion .zip beside a .json
 * save read and write every file of the map's mods: they run on a thread of
 * their own, one at a time, never on DOOM's. The main tick
 * (sh_rawmap_poll_main) takes the result: an import holds its text for the
 * load it opens again; an export records the save report and says which
 * file to share. Neither changes what the open map serves. */
enum { RT_IDLE = 0, RT_RUNNING = 1, RT_DONE = 2 };
enum { RT_IMPORT = 1, RT_EXPORT_DIRECT = 2, RT_EXPORT_BESIDE = 3 };
static struct {
    volatile LONG state;
    int kind;
    char path[SH_RAWMAP_PATH_MAX];
    char *data;                       /* process heap: an export's map, an import's result */
    size_t len;
    unsigned mods;
    unsigned long long json_bytes, bytes;
    char json[SH_RAWMAP_PATH_MAX];    /* a companion export's .json */
    char cause[MODEXPORT_CAUSE_MAX];
} g_rt;

static DWORD WINAPI transport_main(void *unused)
{
    (void)unused;
    if (g_rt.kind == RT_IMPORT) {
        g_rt.data = modexport_read_text_report(g_rt.path, &g_rt.len, g_rt.cause, sizeof g_rt.cause);
    } else {
        g_rt.bytes = modexport_write_text_report(g_rt.data, g_rt.len, g_rt.path, NULL, g_rt.cause, sizeof g_rt.cause);
    }
    MemoryBarrier();
    InterlockedExchange(&g_rt.state, RT_DONE);
    return 0;
}

int sh_rawmap_transport_busy(void)
{
    return InterlockedCompareExchange(&g_rt.state, 0, 0) != RT_IDLE;
}

/* Start a job on its thread (main thread). 1 started; 0 with `why` when one
 * is running or the thread could not start. `data` (process heap) is the
 * job's from here. */
static int transport_start(int kind, const char *path, char *data, size_t len, char *why, size_t why_cap)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_rt.state, RT_RUNNING, RT_IDLE) != RT_IDLE) {
        if (data) HeapFree(GetProcessHeap(), 0, data);
        if (why && why_cap)
            strncpy_s(why, why_cap, "Snapmap+ is still importing or exporting another map; try again when it is done",
                      _TRUNCATE);
        return 0;
    }
    g_rt.kind = kind;
    strncpy_s(g_rt.path, sizeof g_rt.path, path, _TRUNCATE);
    g_rt.data = data;
    g_rt.len = len;
    g_rt.bytes = 0;
    g_rt.cause[0] = 0;
    t = CreateThread(NULL, 0, transport_main, NULL, 0, NULL);
    if (!t) {
        if (data) HeapFree(GetProcessHeap(), 0, data);
        g_rt.data = NULL;
        InterlockedExchange(&g_rt.state, RT_IDLE);
        if (why && why_cap) strncpy_s(why, why_cap, "the game could not start the work; restart DOOM and try again", _TRUNCATE);
        return 0;
    }
    CloseHandle(t);
    return 1;
}

static int transport_start_import(const char *path, char *why, size_t why_cap)
{
    return transport_start(RT_IMPORT, path, NULL, 0, why, why_cap);
}

/* A copy of `data` for an export job, or NULL. */
static char *transport_copy(const char *data, size_t len)
{
    char *copy = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (copy) { memcpy(copy, data, len); copy[len] = 0; }
    return copy;
}

static void report_set(sh_rawmap_save_report *r)
{
    AcquireSRWLockExclusive(&g_report_lock);
    r->sequence = g_report.sequence + 1;
    g_report = *r;
    ReleaseSRWLockExclusive(&g_report_lock);
}

/* The main tick's part: a finished job's result. */
static void transport_poll(void)
{
    char name[SH_RAWMAP_PATH_MAX], text[MODEXPORT_CAUSE_MAX + SH_RAWMAP_PATH_MAX + 160], msg[192] = "";
    if (InterlockedCompareExchange(&g_rt.state, 0, 0) != RT_DONE) return;
    MemoryBarrier();
    rawmap_base_name(g_rt.path, name, sizeof name);
    if (g_rt.kind == RT_IMPORT) {
        if (!g_rt.data) {
            if (!g_rt.cause[0]) strcpy_s(g_rt.cause, sizeof g_rt.cause, "the file could not be read; try again");
            _snprintf_s(text, sizeof text, _TRUNCATE, "%s was not opened: %s.", name, g_rt.cause);
            rawmap_tell(text);
        } else {
            /* The load opens again and parses exactly what was imported. */
            held_keep(g_rt.data, g_rt.len, g_rt.path);
            held_mark_ready(g_rt.path);
            g_rt.data = NULL;
            if (!sh_editor_frame_request_reload(msg, (int)sizeof msg)) {
                _snprintf_s(text, sizeof text, _TRUNCATE,
                            "%s and its mods are imported, but it could not be opened now (%s). Load it again from "
                            "File > Load Rawmap.", name, msg[0] ? msg : "the editor is busy");
                rawmap_tell(text);
            }
        }
    } else {
        sh_rawmap_save_report r;
        memset(&r, 0, sizeof r);
        r.mods = g_rt.mods;
        strncpy_s(r.zip, sizeof r.zip, g_rt.path, _TRUNCATE);
        r.zip_bytes = g_rt.bytes;
        if (g_rt.kind == RT_EXPORT_BESIDE) {
            strncpy_s(r.json, sizeof r.json, g_rt.json, _TRUNCATE);
            r.json_bytes = g_rt.json_bytes;
        }
        if (!g_rt.bytes) {
            strncpy_s(r.zip_error, sizeof r.zip_error, g_rt.cause[0] ? g_rt.cause : "the export could not be written",
                      _TRUNCATE);
            if (g_rt.kind == RT_EXPORT_BESIDE)
                _snprintf_s(text, sizeof text, _TRUNCATE,
                            "This map uses %u mod%s, but %s could not be written (%s). The .json alone will not load "
                            "for others.", r.mods, r.mods == 1 ? "" : "s", name, r.zip_error);
            else
                _snprintf_s(text, sizeof text, _TRUNCATE, "%s could not be written: %s.", name, r.zip_error);
        } else if (g_rt.kind == RT_EXPORT_BESIDE)
            _snprintf_s(text, sizeof text, _TRUNCATE,
                        "This map uses %u mod%s; share %s, the .json alone will not load for others.", r.mods,
                        r.mods == 1 ? "" : "s", name);
        else if (r.mods)
            _snprintf_s(text, sizeof text, _TRUNCATE, "This map uses %u mod%s; %s carries %s, so share that one file.",
                        r.mods, r.mods == 1 ? "" : "s", name, r.mods == 1 ? "it" : "them");
        else
            _snprintf_s(text, sizeof text, _TRUNCATE, "Saved the open map to %s.", name);
        report_set(&r);
        rawmap_tell(text);
        if (g_rt.data) HeapFree(GetProcessHeap(), 0, g_rt.data);
        g_rt.data = NULL;
    }
    InterlockedExchange(&g_rt.state, RT_IDLE);
}

/* The mods a map's text names. */
static unsigned rawmap_mods_of(const char *data, size_t len)
{
    sh_modmanifest_list rows;
    unsigned rejected = 0, n;
    memset(&rows, 0, sizeof rows);
    sh_modmanifest_read(data, len, &rows, &rejected);
    n = (unsigned)rows.count;
    sh_modmanifest_list_free(&rows);
    return n;
}

/* A .zip save: its export starts off the game's thread. 1 when it started,
 * else 0 with `why`. */
static int rawmap_write_zip(const char *data, size_t len, const char *path, char *why, size_t why_cap)
{
    char *copy = transport_copy(data, len);
    unsigned mods = rawmap_mods_of(data, len);
    if (!copy) { strncpy_s(why, why_cap, "the game ran out of memory; restart DOOM and save again", _TRUNCATE); return 0; }
    if (!transport_start(RT_EXPORT_DIRECT, path, copy, len, why, why_cap)) return 0;
    g_rt.mods = mods;
    return 1;
}

/* After a .json rawmap write to `path` succeeded: record it, and for a map
 * that names mods start the export beside it (said when it is done). A map
 * with no mods writes only the JSON, as it always has. `data` may be engine
 * memory; the caller guards the read. */
static void rawmap_after_write(const char *data, size_t len, const char *path, unsigned long long wrote)
{
    sh_rawmap_save_report r;
    char name[SH_RAWMAP_PATH_MAX], text[600], why[200] = "", *copy;

    memset(&r, 0, sizeof r);
    r.mods = rawmap_mods_of(data, len);
    strncpy_s(r.json, sizeof r.json, path, _TRUNCATE);
    r.json_bytes = wrote;
    if (r.mods && sh_rawmap_companion_zip_path(path, r.zip, sizeof r.zip)) {
        copy = transport_copy(data, len);
        if (copy && transport_start(RT_EXPORT_BESIDE, r.zip, copy, len, why, sizeof why)) {
            g_rt.mods = r.mods;
            strncpy_s(g_rt.json, sizeof g_rt.json, path, _TRUNCATE);
            g_rt.json_bytes = wrote;
            return;   /* the report is the export's, when it is done */
        }
        if (!copy) strcpy_s(why, sizeof why, "the game ran out of memory");
        rawmap_base_name(r.zip, name, sizeof name);
        strncpy_s(r.zip_error, sizeof r.zip_error, why, _TRUNCATE);
        _snprintf_s(text, sizeof text, _TRUNCATE,
                    "This map uses %u mod%s, but %s was not written (%s). The .json alone will not load for others.",
                    r.mods, r.mods == 1 ? "" : "s", name, why);
        rawmap_tell(text);
    }
    report_set(&r);
}

/* WHERE SAVES GO.
 *
 * One path, belonging to the map that is open. Empty means the default
 * %LOCALAPPDATA%\snapmap-plus\rawmap.json. "Save Rawmap As" sets it, so a later
 * "Save Rawmap" writes to the same file; opening a different map clears it.
 *
 * The lifetime is the point. A rawmap someone downloads and opens is an archive
 * entry, so nothing about opening one may aim a save at it -- only picking it in
 * Save As does that, for the map in front of them, and not for the next one. */
static char g_save_target[SH_RAWMAP_PATH_MAX] = {0};

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
    char work[SH_RAWMAP_PATH_MAX];
    size_t i, n;
    DWORD attr;

    attr = path_attributes(dir);
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
            wchar_t wide[MAX_PATH];
            if (!sh_rawmap_path_wide(work, wide, MAX_PATH)) return 0;
            attr = GetFileAttributesW(wide);
            if (attr == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(wide, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    return 0;
                }
            } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return 0;                    /* a FILE is sitting where we need a folder */
            }
        }
        work[i] = c;
    }
    attr = path_attributes(dir);
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
    char dir[SH_RAWMAP_PATH_MAX];

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

    attr = path_attributes(path);
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
    char chosen[SH_RAWMAP_PATH_MAX] = "", dflt[SH_RAWMAP_PATH_MAX];
    if (path && path[0]) {
        if (strlen(path) >= SH_RAWMAP_PATH_MAX || !dest_folder_is_usable(path, NULL, 0)) return 0;
        default_rawmap_path(dflt, sizeof dflt);
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
    if (!out[0]) default_rawmap_path(out, cap);
}

void sh_rawmap_clear_save_target_for_new_map(void)
{
    sh_rawmap_set_save_target(NULL);
}

/* Write `len` bytes to `path` through a temporary file renamed over it. Returns the byte count
 * written, or 0 with the Windows error in *out_error. Defined below. */
static unsigned long long write_file_atomic(const char *data, size_t len, const char *path, DWORD *out_error);
static char *pretty_copy(const char *data, size_t len, size_t *out_len);

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
 * the signature note. The save detour never had to know that, because it only ever inspects the
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
                    "RAWMAP: live serialize %s (map->json=%p, idStr ctor=%p, dtor=%p)",
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
static void settings_on_save(void *out_idstr);
static void nav_embed_on_save(void *out_idstr);
static void nav_regions_on_save(void *out_idstr);
static void manifest_on_save(void *out_idstr);
static void place_on_save(void *out_idstr);

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
    int laid_out = 0, zip_started = 0;
    char zip_why[200] = "";
    DWORD write_error = 0;

    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    if (out_bytes) *out_bytes = 0;

    if (!sh_rawmap_live_serialize_ready()) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               InterlockedCompareExchange(&g_live_faulted, 0, 0)
                               ? "Saving rawmaps stopped working this session after an error; restart DOOM"
                               : "Saving rawmaps is unavailable on this game build", _TRUNCATE);
        return 0;
    }
    if (map == NULL) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "Open a map in the editor first", _TRUNCATE);
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
            /* The same encoders the save detour runs, in the same order. The engine
             * hands back map state with the shards already stripped at load, so writing
             * this buffer straight out drops the navigation its author baked. Each one
             * no-ops if its own dependencies are unresolved. */
            settings_on_save(blk);
            nav_embed_on_save(blk);
            manifest_on_save(blk);
            place_on_save(blk);
            nav_regions_on_save(blk);

            len  = *(const int *)(blk + IDSTR_LEN_OFF);
            data = *(const char *const *)(blk + IDSTR_DATA_OFF);
            if (rc && data != NULL && len > 0) {
                size_t shaped_len = 0;
                char *shaped = sh_cvar_value_int(B2_CVAR_SH_PRETTY_ON, 0)
                             ? pretty_copy(data, (size_t)len, &shaped_len) : NULL;
                laid_out = (shaped != NULL);
                if (modexport_is_zip_path(destination)) {
                    /* The export reads every mod file: off the game's thread. */
                    zip_started = rawmap_write_zip(data, (size_t)len, destination, zip_why, sizeof zip_why);
                    wrote = zip_started ? (unsigned long long)len : 0;
                } else {
                    wrote = laid_out ? write_file_atomic(shaped, shaped_len, destination, &write_error)
                                     : write_file_atomic(data, (size_t)len, destination, &write_error);
                    if (wrote) rawmap_after_write(data, (size_t)len, destination, wrote);
                }
                if (shaped != NULL) HeapFree(GetProcessHeap(), 0, shaped);
            }
        }
        g_idstr_dtor(blk);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_live_faulted, 1);
        backend_log("RAWMAP: live serialize FAULTED; disabled for this session");
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "Saving the open map failed with an engine error; restart DOOM to save rawmaps again",
                               _TRUNCATE);
        return 0;
    }

    if (!rc) {
        backend_log("RAWMAP: live serialize -- the engine declined to serialize the map");
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity,
                               "DOOM could not write this map out; nothing was saved", _TRUNCATE);
        return 0;
    }
    if (wrote == 0 && modexport_is_zip_path(destination)) {
        if (out_msg) _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE, "%s was not written: %s", destination, zip_why);
        return 0;
    }
    if (wrote == 0) {
        char line[SH_RAWMAP_PATH_MAX + 128];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "RAWMAP: live serialize produced %d bytes but the write to %s failed (error %lu)",
                    len, destination, (unsigned long)write_error);
        backend_log(line);
        if (out_msg)
            _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE, "%s could not be written: %s",
                        destination, sh_reason_win32(write_error ? write_error : ERROR_WRITE_FAULT, destination, 0));
        return 0;
    }

    InterlockedExchange64(&g_last_bytes, (LONGLONG)wrote);
    InterlockedIncrement(&g_save_count);
    {
        char line[SH_RAWMAP_PATH_MAX + 128];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "RAWMAP: saved %llu bytes from the open map -> %s%s", wrote, destination,
                    laid_out ? " [pretty]" : "");
        backend_log(line);
        if (out_msg)
            _snprintf_s(out_msg, (size_t)msg_capacity, _TRUNCATE,
                        zip_started ? "Writing the open map and its mods to %s; Snapmap+ says when it is done."
                                    : "Saved the open map to %s.", destination);
    }
    if (out_bytes) *out_bytes = wrote;
    return 1;
}

/* The open edit map as JSON in a process-heap copy, with no save side effects:
 * no navigation embedding and no file write. MAIN THREAD ONLY. *out is freed with
 * HeapFree(GetProcessHeap()). */
int sh_rawmap_serialize_live_copy(void *edit_map, char **out, size_t *out_len)
{
    unsigned char blk[IDSTR_SIZE];
    unsigned char rc = 0;
    int len = 0, ok = 0;
    const char *data = NULL;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !edit_map || !sh_rawmap_live_serialize_ready()) return 0;
    memset(blk, 0, sizeof blk);
    __try {
        g_idstr_ctor(blk, "");
        g_snapshot_depth++;
        __try { rc = g_map_to_json(edit_map, blk, 1); }
        __finally { g_snapshot_depth--; }
        len  = *(const int *)(blk + IDSTR_LEN_OFF);
        data = *(const char *const *)(blk + IDSTR_DATA_OFF);
        if (rc && data && len > 0) {
            char *copy = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)len + 1);
            if (copy) {
                memcpy(copy, data, (size_t)len);
                copy[len] = 0;
                *out = copy;
                if (out_len) *out_len = (size_t)len;
                ok = 1;
            }
        }
        g_idstr_dtor(blk);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_live_faulted, 1);
        backend_log("RAWMAP: live serialize FAULTED (copy); disabled for this session");
        if (*out) { HeapFree(GetProcessHeap(), 0, *out); *out = NULL; }
        return 0;
    }
    return ok;
}

/* Parse map JSON into an idSnapMap through the native parser only, bypassing
 * the load swap and every per-map-open side effect. `native` is the resolved
 * DeserializeFromJson, used when the detour is not installed. MAIN THREAD. */
int sh_rawmap_deserialize_plain(void *native, const char *json, void *out_map)
{
    deser_fn_t fn = g_deser_orig ? g_deser_orig : (deser_fn_t)native;
    if (!fn || !json || !out_map) return 0;
    return fn(json, out_map);
}

static unsigned long long write_file_atomic(const char *data, size_t len, const char *path, DWORD *out_error)
{
    static volatile LONG sequence;
    char dir[SH_RAWMAP_PATH_MAX];
    wchar_t wpath[MAX_PATH], temp[MAX_PATH];
    size_t total = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    int attempt, flushed = 0;
    DWORD err = 0;

    if (out_error) *out_error = 0;
    if (!data || !len || !path || !path[0]) return 0;
    if (!sh_rawmap_path_wide(path, wpath, MAX_PATH)) { if (out_error) *out_error = ERROR_INVALID_NAME; return 0; }
    if (!dest_dir_part(path, dir, sizeof dir) || !ensure_dir(dir)) {
        if (out_error) *out_error = GetLastError() ? GetLastError() : ERROR_PATH_NOT_FOUND;
        return 0;
    }
    /* CREATE_NEW never truncates someone else's scratch file. Separate writers
     * must own separate temporaries until each publishes its complete result. */
    for (attempt = 0; attempt < 64; ++attempt) {
        if (_snwprintf_s(temp, MAX_PATH, _TRUNCATE, L"%s.smp-%lu-%lu.tmp", wpath,
                         GetCurrentProcessId(), (unsigned long)InterlockedIncrement(&sequence)) < 0) {
            if (out_error) *out_error = ERROR_FILENAME_EXCED_RANGE;
            return 0;
        }
        h = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) { if (out_error) *out_error = err; return 0; }
    }
    if (h == INVALID_HANDLE_VALUE) { if (out_error) *out_error = err; return 0; }
    while (total < len) {
        DWORD wr = 0;
        DWORD chunk = (DWORD)((len - total) > 0x10000000 ? 0x10000000 : len - total);
        if (!WriteFile(h, data + total, chunk, &wr, NULL) || !wr) { err = GetLastError(); break; }
        total += wr;
    }
    if (total == len) { flushed = FlushFileBuffers(h) != 0; if (!flushed) err = GetLastError(); }
    CloseHandle(h);
    if (total != len || !flushed ||
        !MoveFileExW(temp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (!err) err = GetLastError();
        DeleteFileW(temp);
        backend_log("RAWMAP: write failed; the previous destination is unchanged");
        if (out_error) *out_error = err ? err : ERROR_WRITE_FAULT;
        return 0;
    }
    return (unsigned long long)total;
}

#ifdef SH_RAWMAP_TESTING
/* Test-only door onto the writer: the destination rules only mean anything in terms
 * of what actually reaches disk. Compiled out of shipping builds. */
unsigned long long sh_rawmap_test_write(const char *data, size_t len)
{
    char path[SH_RAWMAP_PATH_MAX];
    unsigned long long wrote;
    resolve_dest_path(path, sizeof path);
    if (modexport_is_zip_path(path)) {
        char why[200];
        return rawmap_write_zip(data, len, path, why, sizeof why) ? len : 0;
    }
    wrote = write_file_atomic(data, len, path, NULL);
    if (wrote) rawmap_after_write(data, len, path, wrote);   /* as Save Rawmap does */
    return wrote;
}

#endif

/* Produce an owned pretty-printed copy, or NULL to keep the engine bytes. The
 * caller guards reads of the engine-owned source. Formatting refusal never
 * suppresses the file write.
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

/* ==== embed-on-save: the navigation a map carries travels inside it ==== */

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
        backend_log("NAV: embed-on-save DARK -- no module base to resolve the idStr assignment");
        return;
    }
    /* Resolve assignment before writing engine-owned save output. */
    for (i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        sig_result ra;
        sig_status st;
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "IdStrAssignFromStr") != 0) continue;
        st = sig_resolve_one(module_base, &BACKEND_ENGINE_SIGNATURES[i], &ra);
        if (st == SIG_OK) g_idstr_assign = (idstr_assign_fn)ra.addr;
        break;
    }
    backend_log(g_idstr_assign != NULL
        ? "NAV: embed-on-save ready -- a saved map will carry its navigation"
        : "NAV: embed-on-save DARK -- idStr assignment unresolved; saves are unchanged");
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
    int failed = 0;

    if (!g_idstr_assign || out_idstr == NULL) return;
    if (!read_out_idstr(out_idstr, &data, &len)) return;

    body = sh_navmesh_embed_all(data, (size_t)len, &body_len, &failed);
    if (body != NULL) {
        failed = !replace_out_idstr(out_idstr, body, body_len);
        HeapFree(GetProcessHeap(), 0, body);
    }
    if (failed)
        tell_titled("Navigation", "This save does not carry the map's AI navigation. "
                    "Save again to retry.");
}

/* Put the open map's settings into the map's member (map_settings.h). */
static void settings_on_save(void *out_idstr)
{
    const char *data = NULL;
    int len = 0;
    char *body = NULL;
    size_t body_len = 0;

    if (!g_idstr_assign || out_idstr == NULL) return;
    if (!read_out_idstr(out_idstr, &data, &len)) return;
    __try {
        body = sh_map_settings_on_save(data, (size_t)len, &body_len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        body = NULL;
        backend_log("SETTINGS: save FAULTED; the save carries the settings rows it had");
    }
    if (body == NULL) return;
    if (!replace_out_idstr(out_idstr, body, body_len))
        backend_log("SETTINGS: the saved map could not be written back; its settings are not in "
                    "this save");
    HeapFree(GetProcessHeap(), 0, body);
}

/* Put the map manifest into the map's member: one row per bundle that
 * provides a placed name, plus the map-level rows (modmanifest.h). */
static void manifest_on_save(void *out_idstr)
{
    const char *data = NULL;
    int len = 0;
    char *body = NULL;
    size_t body_len = 0;

    if (!g_idstr_assign || out_idstr == NULL) return;
    if (!read_out_idstr(out_idstr, &data, &len)) return;
    __try {
        body = sh_modmanifest_on_save(data, (size_t)len, &body_len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        body = NULL;
        backend_log("MODS: save manifest FAULTED; the save carries no manifest rows");
    }
    if (body == NULL) return;
    if (!replace_out_idstr(out_idstr, body, body_len))
        backend_log("MODS: the saved map could not be written back; its manifest rows are not in "
                    "this save");
    HeapFree(GetProcessHeap(), 0, body);
}

/* Put the open map's place into the map's member (map_carrier.h): its
 * segment of mods\maps\, or no place for a map that finds its mods at the
 * root. */
static void place_on_save(void *out_idstr)
{
    const char *data = NULL;
    int len = 0;
    char *body = NULL, place[SH_MODSERVE_PLACE_CAP], err[96] = "";
    const char *segment;
    size_t body_len = 0;

    if (!g_idstr_assign || out_idstr == NULL) return;
    if (!read_out_idstr(out_idstr, &data, &len)) return;
    sh_modserve_place_get(place, sizeof place);
    segment = place[0] ? place + sizeof SH_MODSERVE_PLACES : NULL;
    __try {
        body = sh_carrier_put_place(data, (size_t)len, segment, &body_len, err, sizeof err);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        body = NULL;
        strcpy_s(err, sizeof err, "it faulted");
    }
    if (body == NULL) {
        if (err[0]) {
            char line[200];
            _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: the map's place was not saved (%s); it reopens finding its mods at the root", err);
            backend_log(line);
        }
        return;
    }
    if (!replace_out_idstr(out_idstr, body, body_len))
        backend_log("MODS: the saved map could not be written back; its place is not in this save");
    HeapFree(GetProcessHeap(), 0, body);
}

/* Serialize, then embed delivery payloads into the output. Preserve
 * the native bool result on every return.
 */
static unsigned char sh_ser_detour(void *map, void *out_idstr, unsigned char compact)
{
    if (g_ser_orig == NULL) return 0;   /* defensive: should never happen once installed */

    /* Call native serialization unconditionally and capture its bool result
     * before any helper can clobber AL.
     */
    const unsigned char rc = g_ser_orig(map, out_idstr, compact);
    if (g_snapshot_depth) {
        if (rc && g_snapshot_visit)
            g_snapshot_visited = g_snapshot_visit(map, out_idstr, g_snapshot_visit_ctx);
        return rc;
    }

    if (!rc) return rc;

    /* Each puts its part into the one "snapmap-plus" member, appended last at
     * the root (map_carrier.h); none writes a variable. Nothing to carry
     * leaves the engine's bytes as they are. */
    settings_on_save(out_idstr);
    /* Restore retained navigation so ordinary saves preserve it. */
    nav_embed_on_save(out_idstr);
    manifest_on_save(out_idstr);
    place_on_save(out_idstr);
    /* A save of a map opened from a rawmap makes it a saved map of its own:
     * the edit's id and name are its from now on. */
    open_identity_set(NULL);
    {
        const char *saved = NULL;
        int saved_len = 0;
        if (read_out_idstr(out_idstr, &saved, &saved_len)) {
            sh_modmanifest_list rows;
            unsigned rejected = 0;
            memset(&rows, 0, sizeof rows);
            __try { sh_modmanifest_read(saved, (size_t)saved_len, &rows, &rejected); }
            __except (EXCEPTION_EXECUTE_HANDLER) { memset(&rows, 0, sizeof rows); }
            InterlockedExchange(&g_saved_rows, (LONG)rows.count);
            InterlockedExchange(&g_saved_rows_generation, (LONG)sh_rawmap_load_generation());
            sh_modmanifest_list_free(&rows);
            if (g_save_observer) g_save_observer(saved, (size_t)saved_len);
        }
    }

    /* Refresh navigation regions from the final serialized map. */
    nav_regions_on_save(out_idstr);
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
        backend_log("RAWMAP: save detour SKIPPED -- SerializeToJson not resolved");
        return 0;
    }
    if (!serialize_status_ok) {
        /* Refuse an already-hooked prologue, matching the load detour policy. */
        backend_log("RAWMAP: save detour SKIPPED -- SerializeToJson resolved via hook-tolerant "
                    "fallback (prologue already hooked); not installing over an existing detour");
        return 0;
    }
    void *tramp = hook_prepare(serialize_fn, (void *)sh_ser_detour, SAVE_STOLEN);
    if (tramp == NULL) {
        backend_log("RAWMAP: save detour FAIL -- trampoline preparation failed");
        return 0;
    }
    g_ser_orig = (serialize_fn_t)tramp;

    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_ser_orig = NULL;
        backend_log("RAWMAP: save detour commit failed; retained callbacks require restoration");
        return 0;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE, "RAWMAP: save detour installed at %p (trampoline %p, stolen %d)",
                serialize_fn, tramp, SAVE_STOLEN);
    backend_log(line);
    return 1;
}

unsigned long sh_rawmap_save_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_save_count, 0, 0);
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

    h = path_open_read(path);
    if (h == INVALID_HANDLE_VALUE) return 0;

    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return 0; }

    /* The engine writes the root "~type" last, so the tail is enough -- unless
     * the product's "snapmap-plus" member follows it (map_carrier.h: appended
     * after "~type", and a navigation bake makes it far longer than the tail).
     * Read the whole file then, and look just before the member. */
    at.QuadPart = (sz.QuadPart > (LONGLONG)tail) ? (sz.QuadPart - (LONGLONG)tail) : 0;
    if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN)) { CloseHandle(h); return 0; }

    buf = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)tail + 1);
    if (buf == NULL) { CloseHandle(h); return 0; }

    if (ReadFile(h, buf, tail, &rd, NULL) && rd > 0) {
        buf[rd] = '\0';                    /* strstr needs the terminator; the +1 above is for it */
        found = (strstr(buf, "\"idSnapMap\"") != NULL) ? 1 : 0;
        /* The tail may sit wholly inside the member's data (a navigation
         * chunk is 1 MiB), so nothing in it says a member is there. */
        if (!found && sz.QuadPart > (LONGLONG)tail && sz.QuadPart <= (LONGLONG)INT_MAX) {
            char *all = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)sz.QuadPart + 1);
            LARGE_INTEGER zero;
            DWORD got = 0;
            zero.QuadPart = 0;
            if (all && SetFilePointerEx(h, zero, NULL, FILE_BEGIN) &&
                ReadFile(h, all, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart) {
                sh_carrier_view view;
                all[got] = '\0';
                if (sh_carrier_view_read(all, (size_t)got, &view) && view.state != SH_CARRIER_ABSENT &&
                    view.cut_from > 0 && view.cut_from <= (size_t)got) {
                    /* The tail of the engine's own text: the bytes before the member. */
                    size_t end = view.cut_from, start = end > (size_t)tail ? end - (size_t)tail : 0;
                    char saved = all[end];
                    all[end] = '\0';
                    found = (strstr(all + start, "\"idSnapMap\"") != NULL) ? 1 : 0;
                    all[end] = saved;
                }
            }
            if (all) HeapFree(GetProcessHeap(), 0, all);
        }
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
    if (modexport_is_zip_path(path)) return modexport_validate_source(path, out_msg, msg_capacity);

    h = path_open_read(path);
    if (h == INVALID_HANDLE_VALUE) {
        if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, sh_reason_win32(GetLastError(), path, 0), _TRUNCATE);
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
                               "that file is over 2 GB, too large to be a map", _TRUNCATE);
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
                               "that file is not a rawmap; choose a .json or .zip written by Save Rawmap", _TRUNCATE);
        return 0;
    }
    return 1;
}

/* The default load and save path: the same file. */
void sh_rawmap_get_default_paths(char *load_out, int load_cap, char *save_out, int save_cap)
{
    if (load_out && load_cap > 0) { load_out[0] = '\0'; default_rawmap_path(load_out, (size_t)load_cap); }
    if (save_out && save_cap > 0) { save_out[0] = '\0'; default_rawmap_path(save_out, (size_t)save_cap); }
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

#define RAWMAP_OPEN_UNSAFE \
    "Opening rawmaps is unavailable on this game build: Snapmap+ cannot stop DOOM's Save from overwriting " \
    "the map a rawmap opens over"

int sh_rawmap_source_ok(char *out_msg, int msg_capacity)
{
    char path[SH_RAWMAP_PATH_MAX];
    if (!sh_rawmap_load_is_safe()) {
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, RAWMAP_OPEN_UNSAFE, _TRUNCATE);
        return 0;
    }
    resolve_source_path(path, sizeof path);
    return sh_rawmap_validate_source(path, out_msg, msg_capacity);
}

int sh_rawmap_open_now(const char *path, char *out_msg, int msg_capacity)
{
    char reason[MODEXPORT_CAUSE_MAX];
    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    if (!sh_rawmap_load_is_safe()) {
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, RAWMAP_OPEN_UNSAFE, _TRUNCATE);
        return 0;
    }
    if (path && path[0]) {
        /* Nothing is remembered for later: a file that cannot open now is refused now. */
        if (!sh_editor_frame_can_reload(out_msg, msg_capacity)) return 0;
        if (!sh_rawmap_validate_source(path, reason, (int)sizeof reason)) {
            if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, reason, _TRUNCATE);
            rawmap_tell_load_refused(path, reason);
            RAWMAP_DIAG(1, "refused", path, 0, reason, 0, 0);
            return 0;
        }
        if (!sh_rawmap_swap_set_source(path)) {
            if (out_msg && msg_capacity > 0)
                strncpy_s(out_msg, (size_t)msg_capacity, "that path is too long to use", _TRUNCATE);
            RAWMAP_DIAG(1, "refused", path, 0, "that path is too long to use", 0, 0);
            return 0;
        }
    }
    {
        int queued = sh_editor_frame_request_reload(out_msg, msg_capacity);
#ifdef SH_DIAG
        char src[SH_RAWMAP_PATH_MAX];
        resolve_source_path(src, sizeof src);
        RAWMAP_DIAG(1, queued ? "queued" : "refused", src, 0, queued || !out_msg ? "" : out_msg, 0, 0);
#endif
        return queued;
    }
}

#ifdef SH_DIAG
static SRWLOCK g_diag_lock = SRWLOCK_INIT;
static struct {
    unsigned long opens, seq;
    int faulted, rc, mods;
    size_t bytes;
    char state[16], path[SH_RAWMAP_PATH_MAX], reason[MODEXPORT_CAUSE_MAX + MAX_PATH];
} g_diag;

/* new_seq starts a request (sh_rawmap_open_now); later notes update it. mods < 0 keeps the count. */
static void rawmap_diag_note(int new_seq, const char *state, const char *path, int rc,
                             const char *reason, size_t bytes, int mods)
{
    AcquireSRWLockExclusive(&g_diag_lock);
    if (new_seq || !g_diag.seq) {
        g_diag.seq++;
        g_diag.bytes = 0; g_diag.mods = 0; g_diag.rc = 0; g_diag.reason[0] = '\0';
    }
    strncpy_s(g_diag.state, sizeof g_diag.state, state, _TRUNCATE);
    if (path && path[0]) strncpy_s(g_diag.path, sizeof g_diag.path, path, _TRUNCATE);
    g_diag.rc = rc;
    strncpy_s(g_diag.reason, sizeof g_diag.reason, reason ? reason : "", _TRUNCATE);
    if (bytes) g_diag.bytes = bytes;
    if (mods >= 0) g_diag.mods = mods;
    if (!strcmp(state, "opened")) g_diag.opens++;
    if (!strcmp(state, "faulted")) g_diag.faulted = 1;
    ReleaseSRWLockExclusive(&g_diag_lock);
}

int sh_rawmap_diag_status(char *out, size_t cap)
{
    char path[SH_RAWMAP_PATH_MAX * 2], reason[sizeof g_diag.reason * 2];
    int n, pending;
    if (!out || !cap) return -1;
    AcquireSRWLockShared(&g_diag_lock);
    if (!json_escape_into(path, sizeof path, g_diag.path)) path[0] = '\0';
    if (!json_escape_into(reason, sizeof reason, g_diag.reason)) reason[0] = '\0';
    pending = !strcmp(g_diag.state, "queued") || !strcmp(g_diag.state, "opening");
    n = _snprintf_s(out, cap, _TRUNCATE,
        "{\"opens\":%lu,\"generation\":%lu,\"pending\":%d,\"faulted\":%d,\"last\":{\"seq\":%lu,"
        "\"path\":\"%s\",\"state\":\"%s\",\"rc\":%d,\"reason\":\"%s\",\"bytes\":%llu,\"mods\":%d}}",
        g_diag.opens, sh_rawmap_load_generation(), pending, g_diag.faulted, g_diag.seq, path,
        g_diag.seq ? g_diag.state : "", g_diag.rc, reason, (unsigned long long)g_diag.bytes, g_diag.mods);
    ReleaseSRWLockShared(&g_diag_lock);
    return n;
}
#endif

static int slot_rawmap_status(sh_iface *self, char *out_json, int out_capacity)
{
    char load_path[SH_RAWMAP_PATH_MAX], save_path[SH_RAWMAP_PATH_MAX];
    char load_esc[SH_RAWMAP_PATH_MAX * 2], save_esc[SH_RAWMAP_PATH_MAX * 2], target_esc[SH_RAWMAP_PATH_MAX * 2];
    char json_esc[SH_RAWMAP_PATH_MAX * 2], zip_esc[SH_RAWMAP_PATH_MAX * 2], err_esc[512];
    char target[SH_RAWMAP_PATH_MAX] = "";
    sh_rawmap_save_report last;
    int written, tail;

    (void)self;
    if (out_json == NULL || out_capacity <= 0) return 0;

    resolve_source_path(load_path, sizeof load_path);
    resolve_dest_path(save_path, sizeof save_path);
    sh_rawmap_get_save_target(target, (int)sizeof target);

    if (!json_escape_into(load_esc, sizeof load_esc, load_path)) load_esc[0] = '\0';
    if (!json_escape_into(save_esc, sizeof save_esc, save_path)) save_esc[0] = '\0';
    if (!json_escape_into(target_esc, sizeof target_esc, target)) target_esc[0] = '\0';
    sh_rawmap_last_save(&last);
    if (!json_escape_into(json_esc, sizeof json_esc, last.json)) json_esc[0] = '\0';
    if (!json_escape_into(zip_esc, sizeof zip_esc, last.zip)) zip_esc[0] = '\0';
    if (!json_escape_into(err_esc, sizeof err_esc, last.zip_error)) err_esc[0] = '\0';

    /* Paths are UTF-8 (the page reads the status as UTF-8). `saveTarget` is the
     * file Save writes to, or "" for the default; it rides the status because
     * the console can set it too. */
    written = _snprintf_s(out_json, (size_t)out_capacity, _TRUNCATE,
        "{\"load\":\"%s\",\"save\":\"%s\",\"saves\":%lu,\"lastBytes\":%llu,\"loads\":%lu,"
        "\"saveTarget\":\"%s\"}",
        load_esc, save_esc, sh_rawmap_save_count(), sh_rawmap_save_last_bytes(), sh_rawmap_swap_count(),
        target_esc);
    if (written <= 0 || written >= out_capacity - 1) return (written > 0) ? written : 0;

    /* What the last save wrote, and how many mods the open map uses (the File
     * menu offers .zip first when it uses any). Appended inside the object. */
    tail = _snprintf_s(out_json + written - 1, (size_t)(out_capacity - written + 1), _TRUNCATE,
        ",\"lastJson\":\"%s\",\"lastJsonBytes\":%llu,\"lastZip\":\"%s\",\"lastZipBytes\":%llu,"
        "\"lastMods\":%u,\"lastZipError\":\"%s\",\"mapMods\":%d}",
        json_esc, last.json_bytes, zip_esc, last.zip_bytes, last.mods, err_esc, sh_rawmap_open_map_mods());
    if (tail > 0) written += tail - 1;
    else { out_json[written - 1] = '}'; out_json[written] = '\0'; }

    return (written > 0) ? written : 0;
}

/* `arm` is a verb code (the vtable is matched slot for slot between the DLLs):
 *   -1 leave alone   2 make `load_path` the file to open now (it is checked,
 *   and refused when the editor cannot open it now)   3 keep saving to the
 *   current save path   4 saves go back to the default file   5 save now.
 * 0 and 1 named the retired every-load switch and are refused. */
static int slot_rawmap_configure(sh_iface *self, const char *load_path, const char *save_path,
                                 int arm, char *out_msg, int msg_capacity)
{
    char reason[MODEXPORT_CAUSE_MAX];
    int ok = 1;

    (void)self;
    if (out_msg && msg_capacity > 0) out_msg[0] = '\0';
    /* Opening or saving a rawmap needs the map-load and map-save hooks. */
    if ((arm == 2 || arm == 5 || (save_path && save_path[0])) && out_msg && msg_capacity > 0 &&
        sh_feature_refusal(SH_FEATURE_RAWMAPS, out_msg, (size_t)msg_capacity))
        return 0;

    if (load_path != NULL) {
        if (load_path[0] == '\0') {
            sh_rawmap_swap_set_source(NULL);          /* empty = restore the default */
        } else {
            if (arm == 2 && !sh_rawmap_load_is_safe()) {
                if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, RAWMAP_OPEN_UNSAFE, _TRUNCATE);
                return 0;
            }
            if (arm == 2 && !sh_editor_frame_can_reload(out_msg, msg_capacity)) return 0;
            if (!sh_rawmap_validate_source(load_path, reason, (int)sizeof reason)) {
                if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, reason, _TRUNCATE);
                rawmap_tell_load_refused(load_path, reason);
                return 0;
            }
            if (!sh_rawmap_swap_set_source(load_path)) {
                if (out_msg) strncpy_s(out_msg, (size_t)msg_capacity, "that path is too long to use", _TRUNCATE);
                return 0;
            }
        }
    }

    /* A save always uses the live editor. An unavailable or busy editor must
     * not export a different saved map. */
    if ((save_path && save_path[0]) || arm == 5) {
        if (!sh_editor_frame_request_rawmap_save_to(save_path, out_msg, msg_capacity)) return 0;
    } else if (save_path) {
        ok = sh_rawmap_set_save_target(NULL);
    }

    /* 3 pins whatever the save path already resolves to, so ticking never
     * moves the destination -- it only makes the current one stick. 4 releases it. */
    if (arm == 3) {
        char now[SH_RAWMAP_PATH_MAX] = "";
        resolve_dest_path(now, sizeof now);
        if (now[0]) sh_rawmap_set_save_target(now);
    }
    else if (arm == 4) sh_rawmap_set_save_target(NULL);

    if (!ok && out_msg && out_msg[0] == '\0')
        strncpy_s(out_msg, (size_t)msg_capacity, "that save path cannot be used", _TRUNCATE);
    return ok;
}

/* Open the load path now. The engine call and its frame-boundary discipline live in
 * editor_frame.c; this is only the slot. */
static int slot_rawmap_load_now(sh_iface *self, char *out_msg, int msg_capacity)
{
    (void)self;
    if (out_msg && msg_capacity > 0 && sh_feature_refusal(SH_FEATURE_RAWMAPS, out_msg, (size_t)msg_capacity))
        return 0;
    return sh_rawmap_open_now(NULL, out_msg, msg_capacity);
}

void sh_rawmap_get_slots(sh_rawmap_status_fn *status, sh_rawmap_configure_fn *configure,
                         sh_rawmap_load_now_fn *load_now)
{
    if (status)    *status    = slot_rawmap_status;
    if (configure) *configure = slot_rawmap_configure;
    if (load_now)  *load_now  = slot_rawmap_load_now;
}
