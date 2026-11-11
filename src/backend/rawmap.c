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
#include <knownfolders.h>
#pragma comment(lib, "shell32.lib")   /* SHGetKnownFolderPath */
#pragma comment(lib, "ole32.lib")     /* CoTaskMemFree */
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
#include "../common/text.h"
#include "../fault_shield/recovery.h"   /* shield_note_refused_load: a refused map is sound */

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

/* Load source override (malloc'd), or NULL for %LOCALAPPDATA%/snapmap-plus/rawmap.json. */
static char *g_src_path;

/* Every path string in this file is malloc'd and sized to its path. The
 * backend's C runtime allocates from the process heap, never from an engine
 * heap, so a path made on a game thread is freed by this code on any thread. */
wchar_t *sh_rawmap_path_wide(const char *utf8)
{
    wchar_t *w, *out, *body;
    int n;
    DWORD need, got;
    if (!utf8 || !utf8[0]) { SetLastError(ERROR_INVALID_NAME); return NULL; }
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) { SetLastError(ERROR_INVALID_NAME); return NULL; }
    w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (!w) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n);
    if (!wcsncmp(w, L"\\\\?\\", 4)) return w;                     /* already the long form */
    need = GetFullPathNameW(w, 0, NULL, NULL);
    if (!need) { free(w); SetLastError(ERROR_INVALID_NAME); return NULL; }
    /* Room for the full path behind the longest prefix, "\\?\UNC\" (8). */
    out = (wchar_t *)malloc(((size_t)need + 8) * sizeof *out);
    if (!out) { free(w); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    body = out + 8;
    got = GetFullPathNameW(w, need, body, NULL);
    free(w);
    if (!got || got >= need) { free(out); SetLastError(ERROR_INVALID_NAME); return NULL; }
    if (body[0] == L'\\' && body[1] == L'\\') {
        if (body[2] == L'.' || body[2] == L'?') {                   /* a device path stays as it is */
            memmove(out, body, ((size_t)got + 1) * sizeof *out);
            return out;
        }
        memmove(out + 8, body + 2, ((size_t)got - 1) * sizeof *out);   /* \\server\share -> \\?\UNC\server\share */
        memcpy(out, L"\\\\?\\UNC\\", 8 * sizeof *out);
        return out;
    }
    memmove(out + 4, body, ((size_t)got + 1) * sizeof *out);
    memcpy(out, L"\\\\?\\", 4 * sizeof *out);
    return out;
}

char *sh_rawmap_path_utf8(const wchar_t *wide)
{
    const wchar_t *p = wide;
    const char *lead = "";
    char *out;
    int n;
    if (!wide || !wide[0]) return NULL;
    if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) { p += 8; lead = "\\\\"; }
    else if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    n = WideCharToMultiByte(CP_UTF8, 0, p, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    out = (char *)malloc(strlen(lead) + (size_t)n);
    if (!out) return NULL;
    memcpy(out, lead, strlen(lead));
    WideCharToMultiByte(CP_UTF8, 0, p, -1, out + strlen(lead), n, NULL, NULL);
    return out;
}

static DWORD path_attributes(const char *utf8)
{
    wchar_t *w = sh_rawmap_path_wide(utf8);
    DWORD attrs = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return attrs;
}

static HANDLE path_open_read(const char *utf8)
{
    wchar_t *w = sh_rawmap_path_wide(utf8);
    HANDLE h;
    DWORD err;
    if (!w) return INVALID_HANDLE_VALUE;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    err = GetLastError();
    free(w);
    SetLastError(err);
    return h;
}

/* Both default paths are the same file in the shared application data root. */
static char *default_rawmap_path(void)
{
    PWSTR known = NULL;
    char *root = NULL, *out;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known)) && known)
        root = sh_rawmap_path_utf8(known);
    if (known) CoTaskMemFree(known);
    out = root ? sh_textf("%s\\snapmap-plus\\rawmap.json", root) : sh_text("snapmap-plus\\rawmap.json");
    free(root);
    return out;
}

/* The load path in force (malloc'd), or NULL when memory ran out. */
static char *resolve_source_path(void)
{
    char *out = NULL;
    int set;
    AcquireSRWLockShared(&g_paths_lock);
    set = g_src_path != NULL;
    if (set) out = sh_text(g_src_path);
    ReleaseSRWLockShared(&g_paths_lock);
    return set ? out : default_rawmap_path();
}

char *sh_rawmap_load_path(void) { return resolve_source_path(); }
char *sh_rawmap_default_path(void) { return default_rawmap_path(); }

static void rawmap_tell(const char *text);

/* A held load's text: the bytes the hold checked (for a .zip, the map its
 * import returned), kept so the load the hold's resume starts parses them
 * instead of reading the source again. A ZIP is imported once, at the hold,
 * not again at the resume. It is ready only once the hold resumed
 * (sh_rawmap_reopen_after_mods); any other read of the source drops it. */
static SRWLOCK g_held_lock = SRWLOCK_INIT;
static char *g_held;                   /* process heap */
static size_t g_held_len;
static char *g_held_path;             /* malloc'd */
static int g_held_ready;

/* Hold `text` (process heap, taken) read from `path`. When the path cannot be
 * kept for want of memory, nothing is held: the resume reads the file again. */
static void held_keep(char *text, size_t len, const char *path)
{
    char *old, *old_path, *kept = sh_text(path);
    if (!kept) { HeapFree(GetProcessHeap(), 0, text); text = NULL; len = 0; }
    AcquireSRWLockExclusive(&g_held_lock);
    old = g_held;
    old_path = g_held_path;
    g_held = text;
    g_held_len = len;
    g_held_path = kept;
    g_held_ready = 0;
    ReleaseSRWLockExclusive(&g_held_lock);
    if (old) HeapFree(GetProcessHeap(), 0, old);
    free(old_path);
}

static void held_mark_ready(const char *path)
{
    AcquireSRWLockExclusive(&g_held_lock);
    if (g_held && g_held_path && path && !_stricmp(g_held_path, path)) g_held_ready = 1;
    ReleaseSRWLockExclusive(&g_held_lock);
}

/* The held text of `path` once its hold resumed (owned by the caller); any
 * other held text is dropped: this read is a new load. */
static char *held_take(const char *path, size_t *out_len)
{
    char *p, *drop = NULL, *old_path;
    AcquireSRWLockExclusive(&g_held_lock);
    p = g_held;
    if (p && g_held_ready && g_held_path && path && !_stricmp(g_held_path, path)) *out_len = g_held_len;
    else { drop = p; p = NULL; }
    old_path = g_held_path;
    g_held = NULL;
    g_held_len = 0;
    g_held_path = NULL;
    g_held_ready = 0;
    ReleaseSRWLockExclusive(&g_held_lock);
    if (drop) HeapFree(GetProcessHeap(), 0, drop);
    free(old_path);
    return p;
}

static int transport_start_import(const char *path, char **why);

/* The text, or "" for none. */
static const char *rawmap_s(const char *text) { return text ? text : ""; }

/* Set *why (malloc'd, the old one freed) when why is not NULL. */
static void rawmap_why(char **why, const char *text)
{
    if (why) { free(*why); *why = sh_text(text); }
}

/* Read a NUL-terminated process-heap buffer; caller frees it. Return NULL on
 * failure, with the reason and next step in *why (malloc'd) when why is
 * given. A .zip not imported yet starts its import off the game's thread
 * (*importing set, NULL returned); the load opens again when it is done.
 */
static char *read_source_file_why(size_t *out_len, char **why, int *importing)
{
    char *path = resolve_source_path(), *held, *line;
    char *buf;
    HANDLE h;
    if (!path) {
        rawmap_why(why, "the game ran out of memory; restart the game and try again");
        return NULL;
    }
    held = held_take(path, out_len);
    if (held) {
        line = sh_textf("RAWMAP: %s: the resumed load uses the map its hold read; not read again", path);
        backend_log(line ? line : "RAWMAP: the resumed load uses the map its hold read; not read again");
        free(line);
        free(path);
        return held;
    }
    if (modexport_is_zip_path(path)) {
        *importing = transport_start_import(path, why);
        free(path);
        return NULL;
    }

    h = path_open_read(path);
    if (h == INVALID_HANDLE_VALUE) {
        rawmap_why(why, sh_reason_win32(GetLastError(), path, 0));
        free(path);
        return NULL;
    }
    free(path);

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)INT_MAX) {
        CloseHandle(h);
        rawmap_why(why, "the file is empty or larger than 2 GB, so it is not a saved map; save the map again");
        return NULL;
    }
    size_t n = (size_t)sz.QuadPart;
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);   /* Reserve the trailing NUL. */
    if (!buf) {
        CloseHandle(h);
        rawmap_why(why, "the game ran out of memory reading it; restart the game and try again");
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
        rawmap_why(why, "the file could not be read to its end; try again");
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
    char *place = NULL;
    __try {
        if (sh_carrier_place(json, len, &place) != 1) { free(place); place = NULL; }
        (void)sh_modserve_place_load(place ? place : "");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("MODS: reading the map's place FAULTED; the map finds its mods at the root");
        (void)sh_modserve_place_load(NULL);
    }
    free(place);
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
static char *g_open_name;               /* malloc'd, or NULL */
static volatile LONG g_open_unsaved = 0;
/* The saved-map id the swap's map borrowed, read from the edit the first time
 * the branch answer is asked; "" until then. While the edit still holds it the
 * open map is the rawmap's and Save asks for a name. Once a save gave the map
 * its own id it is a saved map like any other: the engine answers, so the next
 * Save writes that map instead of asking again. The engine's saved-map ids are
 * 20 hex digits; the reader leaves an id that would not fit empty (unread,
 * never cut). */
static char g_borrowed_id[64];
static sh_rawmap_edit_id_fn g_edit_id;

void sh_rawmap_set_edit_id_reader(sh_rawmap_edit_id_fn fn) { g_edit_id = fn; }

/* `name` is the rawmap's file name; NULL clears it. When the copy cannot be
 * made for want of memory the open map is still the rawmap's, named "rawmap". */
static void open_identity_set(const char *name)
{
    char *kept = name ? sh_text(name) : NULL, *old;
    if (name && !kept) kept = sh_text("rawmap");
    AcquireSRWLockExclusive(&g_open_lock);
    old = g_open_name;
    g_open_name = kept;
    InterlockedExchange(&g_open_unsaved, name ? 1 : 0);
    ReleaseSRWLockExclusive(&g_open_lock);
    free(old);
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

int sh_rawmap_open_unsaved(char **name)
{
    int unsaved;
    if (name) *name = NULL;
    AcquireSRWLockShared(&g_open_lock);
    unsaved = InterlockedCompareExchange(&g_open_unsaved, 0, 0) != 0;
    if (name && unsaved && g_open_name) *name = sh_text(g_open_name);
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
        char what[200], *line;   /* map_carrier.h's summary: counts and states, a fixed few words */
        if (!sh_carrier_view_read(json, len, &view) || view.state == SH_CARRIER_ABSENT) return;
        sh_carrier_describe(&view, what, sizeof what);
        line = sh_textf("CARRIER: map read: %s", what);
        backend_log(line ? line : "CARRIER: map read");
        free(line);
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
static char *g_prepared_path;          /* malloc'd; the file the reload checked */
static volatile LONG g_in_reload;      /* 1 between sh_rawmap_prepare_reload and _done */

/* The file name in `path`: a pointer into it ("" for NULL). */
static const char *rawmap_base_name(const char *path)
{
    const char *b = path, *p;
    if (!path) return "";
    for (p = path; *p; ++p) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

/* The rows the rawmap names that nothing serves, or (size_t)-1 when the
 * check faulted: the map is then refused, never handed to the engine. */
#define RAWMAP_MISSING_FAULTED ((size_t)-1)
static size_t rawmap_missing_guarded(const char *text, size_t len, sh_modmanifest_list *rows, char **place)
{
    memset(rows, 0, sizeof *rows);
    __try {
        return sh_modpub_rawmap_missing(text, len, rows, place);
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
    free(g_prepared_path);
    g_prepared_path = NULL;
}

/* Say `sentence` (malloc'd, taken) to the player, note it, and hand it to
 * *why; returns 0 (the reload does not start). */
static int rawmap_reload_refused(char *sentence, const char *state, const char *path, size_t len, int mods, char **why)
{
    const char *said = sentence ? sentence : "The rawmap was not opened.";
    rawmap_tell(said);
    RAWMAP_DIAG(0, state, path, 0, said, len, mods);
    (void)state; (void)len; (void)mods;
    if (why) { free(*why); *why = sentence; }
    else free(sentence);
    return 0;
}

int sh_rawmap_prepare_reload(char **why)
{
    char *path, *place = NULL, *cause = NULL, *line;
    const char *name;
    sh_modmanifest_list rows;
    size_t len = 0, missing;
    char *text;
    int held, importing = 0, rc;

    if (why) *why = NULL;
    prepared_drop();
    path = resolve_source_path();
    if (!path)
        return rawmap_reload_refused(sh_text("The rawmap was not opened: the game ran out of memory. Restart DOOM and "
                                             "open it again."), "refused", "", 0, 0, why);
    name = rawmap_base_name(path);
    /* A .zip is imported off the game's thread first: its mods install before
     * the rows are counted, and the load opens again when the import is done. */
    text = read_source_file_why(&len, &cause, &importing);
    if (text == NULL && importing) {
        rc = rawmap_reload_refused(sh_textf("Importing %s and its mods; it opens when that is done.", name),
                                   "importing", path, 0, 0, why);
        free(path);
        return rc;
    }
    if (text == NULL) {
        if (!cause || !cause[0]) rawmap_why(&cause, "the file could not be read; try again");
        line = sh_textf("RAWMAP: open refused -- %s was not opened: %s", path, rawmap_s(cause));
        backend_log(line ? line : "RAWMAP: open refused");
        free(line);
        /* The importer's or Windows' own reason and next step reach the player
         * (console and toast): the page's Load Rawmap toast was shown before
         * this frame ran. */
        line = sh_textf("%s was not opened: %s.", name, rawmap_s(cause));
        free(cause);
        rc = rawmap_reload_refused(line, "refused", path, 0, 0, why);
        free(path);
        return rc;
    }
    free(cause);
    missing = rawmap_missing_guarded(text, len, &rows, &place);
    if (missing == RAWMAP_MISSING_FAULTED) {
        HeapFree(GetProcessHeap(), 0, text);
        rc = rawmap_reload_refused(sh_textf("%s was not opened: %s", name, RAWMAP_CHECK_FAULTED), "refused", path, len,
                                   0, why);
        free(path);
        return rc;
    }
    /* A rawmap that came with its mods follows "Play maps whose mods include
     * scripts" at every open, however its mods got here. */
    if ((held = rawmap_scripts_off_guarded(&rows, rawmap_s(place))) != 0) {
        sh_modmanifest_list_free(&rows);
        HeapFree(GetProcessHeap(), 0, text);
        free(place);
        rc = rawmap_reload_refused(sh_textf(held < 0 ? "%s was not opened: %s" : "%s was not opened. %s", name,
                                            held < 0 ? RAWMAP_CHECK_FAULTED : SH_MODUI_SCRIPTS_OFF),
                                   "refused", path, len, 0, why);
        free(path);
        return rc;
    }
    if (missing) {
        char *sentence;
        line = sh_textf("RAWMAP: open held -- %s names %u mod(s) that are not installed; the engine never sees it",
                        path, (unsigned)missing);
        backend_log(line ? line : "RAWMAP: open held -- the map names mods that are not installed");
        free(line);
        held = rawmap_hold_guarded(&rows, name, rawmap_s(place));
        sh_modmanifest_list_free(&rows);
        free(place);
        /* Its resume parses these bytes (a ZIP is not imported again). */
        if (held > 0) held_keep(text, len, path);
        else HeapFree(GetProcessHeap(), 0, text);
        sentence = sh_textf(held > 0 ? "%s needs %u mod(s) you do not have; the game is checking whether it can download them"
                                     : "%s needs %u mod(s) you do not have, so it was not opened", name, (unsigned)missing);
        RAWMAP_DIAG(0, "refused", path, 0, rawmap_s(sentence), len, (int)missing);
        free(path);
        if (why) *why = sentence;
        else free(sentence);
        return 0;
    }
    free(place);
    RAWMAP_DIAG(0, "opening", path, 0, "", len, (int)rows.count);
    sh_modmanifest_list_free(&rows);
    g_prepared_len = len;
    g_prepared_path = path;   /* taken; prepared_drop frees it */
    InterlockedExchangePointer((PVOID volatile *)&g_prepared, text);
    InterlockedExchange(&g_in_reload, 1);
    return 1;
}

void sh_rawmap_reload_done(int rc, int faulted)
{
    const char *name = rawmap_base_name(g_prepared_path);
    char *text = NULL;
    InterlockedExchange(&g_in_reload, 0);
    RAWMAP_DIAG(0, faulted ? "faulted" : rc ? "failed" : "opened", rawmap_s(g_prepared_path), rc, "", g_prepared_len, -1);
    if (faulted)
        text = sh_textf("Opening %s failed with an engine error. Rawmaps cannot be opened again until you restart DOOM.",
                        name);
    else if (rc != 0)
        text = sh_textf("DOOM did not open %s (error %d). Try again; restart DOOM if it keeps happening.", name, rc);
    if (text) rawmap_tell(text);
    free(text);
    prepared_drop();
}

int sh_rawmap_reopen_after_mods(char **note)
{
    char *path = resolve_source_path(), *msg = NULL, *line;
    const char *name = rawmap_base_name(path);
    if (note) *note = NULL;
    held_mark_ready(path);   /* the reload below parses the bytes the hold checked */
    if (sh_editor_frame_request_reload(&msg)) {
        line = sh_textf("RAWMAP: %s opens again now that its mods are installed", rawmap_s(path));
        backend_log(line ? line : "RAWMAP: the rawmap opens again now that its mods are installed");
        free(line);
        free(msg);
        free(path);
        return 1;
    }
    line = sh_textf("RAWMAP: %s not reopened after its mods were installed: %s", rawmap_s(path), rawmap_s(msg));
    backend_log(line ? line : "RAWMAP: the rawmap was not reopened after its mods were installed");
    free(line);
    free(msg);
    if (note)
        *note = sh_textf("The mods are installed. Open a map in the editor, then load %s again from File > Load Rawmap.",
                         name[0] ? name : "it");
    free(path);
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
         * gates see a failed parse whose refusal note they say. The map is
         * sound, so the engine's "damaged" notice and its offer to replace
         * the map with its published version are dismissed. */
        shield_note_refused_load();
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
        char *line;
        unsigned long n;
        int rc;
        if (g_prepared == NULL) {
            /* The reload borrowed another saved map's id: that map must not
             * open in the rawmap's place. */
            backend_log("RAWMAP: the reload's checked text is gone; the reload parses nothing");
            return 0;
        }
        n = (unsigned long)InterlockedIncrement(&g_swap_count);
        line = sh_textf("RAWMAP: opening %s (%zu bytes) [#%lu]", rawmap_s(g_prepared_path), g_prepared_len, n);
        backend_log(line ? line : "RAWMAP: opening the rawmap");
        free(line);
        /* The checked text stays held until the reload ends, so a second parse
         * inside the same LoadMap reads the same bytes. */
        rc = deser_prepared(g_prepared, out_map);
        /* The open map came from a rawmap, which is what the branch answer keys off. */
        open_borrow_reset();
        InterlockedExchange(&g_open_is_rawmap, 1);
        open_identity_set(rc ? rawmap_base_name(g_prepared_path) : NULL);
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
    char *kept = path && path[0] ? sh_text(path) : NULL, *old;
    if (path && path[0] && !kept) return 0;   /* out of memory: the load path is unchanged */
    AcquireSRWLockExclusive(&g_paths_lock);
    old = g_src_path;
    g_src_path = kept;
    ReleaseSRWLockExclusive(&g_paths_lock);
    free(old);
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
static char *g_tell;                        /* the sentence, whole (malloc'd), or NULL */
static volatile LONG g_tell_pending;
static volatile LONG g_tell_console_only;   /* the page shows its own toast for this one */
static const char *volatile g_tell_title = "Rawmap";

/* Hold `text` (malloc'd, taken) as the sentence the main tick says. */
static void tell_hold(const char *title, char *text, int console_only)
{
    char *old;
    if (!text) return;
    AcquireSRWLockExclusive(&g_tell_lock);
    old = g_tell;
    g_tell = text;
    g_tell_title = title;
    ReleaseSRWLockExclusive(&g_tell_lock);
    free(old);
    InterlockedExchange(&g_tell_console_only, console_only);
    InterlockedExchange(&g_tell_pending, 1);
}

static void tell_titled(const char *title, const char *text)
{
    char *line = sh_textf("%s: %s", title, text);
    backend_log(line ? line : title);
    free(line);
    tell_hold(title, sh_text(text), 0);
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
    char *line = sh_textf("RAWMAP: load of %s refused: %s", path ? path : "", reason ? reason : "");
    backend_log(line ? line : "RAWMAP: a load was refused");
    free(line);
    tell_hold("Rawmap", sh_textf("The rawmap was not loaded: %s.", reason && reason[0] ? reason : "it cannot be read"), 1);
}

static void transport_poll(void);

void sh_rawmap_poll_main(void)
{
    char *text;
    const char *title;
    void (*toast)(const char *, const char *) = g_notify_toast;
    void (*print)(const char *) = g_notify_print;
    transport_poll();
    if (!InterlockedExchange(&g_tell_pending, 0)) return;
    AcquireSRWLockExclusive(&g_tell_lock);
    text = g_tell;
    g_tell = NULL;
    title = g_tell_title;
    ReleaseSRWLockExclusive(&g_tell_lock);
    if (!text) return;
    if (print) print(text);
    if (toast && !InterlockedCompareExchange(&g_tell_console_only, 0, 0)) toast(title, text);
    free(text);
}

void sh_rawmap_last_save(sh_rawmap_save_report *out)
{
    if (!out) return;
    AcquireSRWLockShared(&g_report_lock);
    *out = g_report;
    out->json = g_report.json ? sh_text(g_report.json) : NULL;
    out->zip = g_report.zip ? sh_text(g_report.zip) : NULL;
    out->zip_error = g_report.zip_error ? sh_text(g_report.zip_error) : NULL;
    ReleaseSRWLockShared(&g_report_lock);
}

void sh_rawmap_save_report_free(sh_rawmap_save_report *r)
{
    if (!r) return;
    free(r->json);
    free(r->zip);
    free(r->zip_error);
    r->json = r->zip = r->zip_error = NULL;
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

char *sh_rawmap_companion_zip_path(const char *json_path)
{
    size_t n;
    char *out;
    if (!json_path || !json_path[0] || modexport_is_zip_path(json_path)) return NULL;
    n = strlen(json_path);
    if (n > 5 && _stricmp(json_path + n - 5, ".json") == 0) n -= 5;
    out = (char *)malloc(n + 5);
    if (!out) return NULL;
    memcpy(out, json_path, n);
    memcpy(out + n, ".zip", 5);
    return out;
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
    char *path;                       /* malloc'd: the .zip */
    char *data;                       /* process heap: an export's map, an import's result */
    size_t len;
    unsigned mods;
    unsigned long long json_bytes, bytes;
    char *json;                       /* malloc'd: a companion export's .json, or NULL */
    char *cause;                      /* the importer's or exporter's refusal (malloc'd), or NULL */
} g_rt;

static DWORD WINAPI transport_main(void *unused)
{
    (void)unused;
    if (g_rt.kind == RT_IMPORT) {
        g_rt.data = modexport_read_text_report(g_rt.path, &g_rt.len, &g_rt.cause);
    } else {
        g_rt.bytes = modexport_write_text_report(g_rt.data, g_rt.len, g_rt.path, NULL, &g_rt.cause);
    }
    MemoryBarrier();
    InterlockedExchange(&g_rt.state, RT_DONE);
    return 0;
}

int sh_rawmap_transport_busy(void)
{
    return InterlockedCompareExchange(&g_rt.state, 0, 0) != RT_IDLE;
}

/* Free the job's own strings and data (the main tick, or a start that failed). */
static void transport_clear(void)
{
    if (g_rt.data) HeapFree(GetProcessHeap(), 0, g_rt.data);
    free(g_rt.path);
    free(g_rt.json);
    free(g_rt.cause);
    g_rt.data = NULL;
    g_rt.path = g_rt.json = g_rt.cause = NULL;
    g_rt.len = 0;
}

/* Start a job on its thread (main thread). 1 started; 0 with *why when one
 * is running or the thread could not start. `data` (process heap) and
 * `json` (malloc'd, or NULL) are the job's from here, whatever the answer. */
static int transport_start(int kind, const char *path, char *data, size_t len, unsigned mods, char *json,
                           unsigned long long json_bytes, char **why)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_rt.state, RT_RUNNING, RT_IDLE) != RT_IDLE) {
        if (data) HeapFree(GetProcessHeap(), 0, data);
        free(json);
        rawmap_why(why, "Snapmap+ is still importing or exporting another map; try again when it is done");
        return 0;
    }
    transport_clear();
    g_rt.kind = kind;
    g_rt.path = sh_text(path);
    g_rt.data = data;
    g_rt.len = len;
    g_rt.mods = mods;
    g_rt.json = json;
    g_rt.json_bytes = json_bytes;
    g_rt.bytes = 0;
    if (!g_rt.path) {
        transport_clear();
        InterlockedExchange(&g_rt.state, RT_IDLE);
        rawmap_why(why, "the game ran out of memory; restart DOOM and try again");
        return 0;
    }
    t = CreateThread(NULL, 0, transport_main, NULL, 0, NULL);
    if (!t) {
        transport_clear();
        InterlockedExchange(&g_rt.state, RT_IDLE);
        rawmap_why(why, "the game could not start the work; restart DOOM and try again");
        return 0;
    }
    CloseHandle(t);
    return 1;
}

static int transport_start_import(const char *path, char **why)
{
    return transport_start(RT_IMPORT, path, NULL, 0, 0, NULL, 0, why);
}

/* A copy of `data` for an export job, or NULL. */
static char *transport_copy(const char *data, size_t len)
{
    char *copy = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (copy) { memcpy(copy, data, len); copy[len] = 0; }
    return copy;
}

/* Record the report; its strings (malloc'd) are taken. */
static void report_set(sh_rawmap_save_report *r)
{
    sh_rawmap_save_report old;
    AcquireSRWLockExclusive(&g_report_lock);
    r->sequence = g_report.sequence + 1;
    old = g_report;
    g_report = *r;
    ReleaseSRWLockExclusive(&g_report_lock);
    sh_rawmap_save_report_free(&old);
    r->json = r->zip = r->zip_error = NULL;
}

/* Say `text` (malloc'd) and free it. */
static void rawmap_tell_free(char *text)
{
    if (text) rawmap_tell(text);
    free(text);
}

/* The main tick's part: a finished job's result. */
static void transport_poll(void)
{
    char *msg = NULL, *text = NULL;
    const char *name;
    if (InterlockedCompareExchange(&g_rt.state, 0, 0) != RT_DONE) return;
    MemoryBarrier();
    name = rawmap_base_name(g_rt.path);
    if (g_rt.kind == RT_IMPORT) {
        if (!g_rt.data) {
            rawmap_tell_free(sh_textf("%s was not opened: %s.", name,
                                      g_rt.cause && g_rt.cause[0] ? g_rt.cause : "the file could not be read; try again"));
        } else {
            /* The load opens again and parses exactly what was imported. */
            held_keep(g_rt.data, g_rt.len, g_rt.path);
            held_mark_ready(g_rt.path);
            g_rt.data = NULL;
            if (!sh_editor_frame_request_reload(&msg))
                rawmap_tell_free(sh_textf("%s and its mods are imported, but it could not be opened now (%s). Load it "
                                          "again from File > Load Rawmap.", name, msg && msg[0] ? msg : "the editor is busy"));
            free(msg);
        }
    } else {
        sh_rawmap_save_report r;
        memset(&r, 0, sizeof r);
        r.mods = g_rt.mods;
        r.zip = sh_text(g_rt.path);
        r.zip_bytes = g_rt.bytes;
        if (g_rt.kind == RT_EXPORT_BESIDE) {
            r.json = g_rt.json;
            g_rt.json = NULL;
            r.json_bytes = g_rt.json_bytes;
        }
        if (!g_rt.bytes) {
            r.zip_error = sh_text(g_rt.cause && g_rt.cause[0] ? g_rt.cause : "the export could not be written");
            if (g_rt.kind == RT_EXPORT_BESIDE)
                text = sh_textf("This map uses %u mod%s, but %s could not be written (%s). The .json alone will not load "
                                "for others.", r.mods, r.mods == 1 ? "" : "s", name, rawmap_s(r.zip_error));
            else
                text = sh_textf("%s could not be written: %s.", name, rawmap_s(r.zip_error));
        } else if (g_rt.kind == RT_EXPORT_BESIDE)
            text = sh_textf("This map uses %u mod%s; share %s, the .json alone will not load for others.", r.mods,
                            r.mods == 1 ? "" : "s", name);
        else if (r.mods)
            text = sh_textf("This map uses %u mod%s; %s carries %s, so share that one file.",
                            r.mods, r.mods == 1 ? "" : "s", name, r.mods == 1 ? "it" : "them");
        else
            text = sh_textf("Saved the open map to %s.", name);
        report_set(&r);
        rawmap_tell_free(text);
    }
    transport_clear();
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
 * else 0 with *why. */
static int rawmap_write_zip(const char *data, size_t len, const char *path, char **why)
{
    char *copy = transport_copy(data, len);
    unsigned mods = rawmap_mods_of(data, len);
    if (!copy) { rawmap_why(why, "the game ran out of memory; restart DOOM and save again"); return 0; }
    return transport_start(RT_EXPORT_DIRECT, path, copy, len, mods, NULL, 0, why);
}

/* After a .json rawmap write to `path` succeeded: record it, and for a map
 * that names mods start the export beside it (said when it is done). A map
 * with no mods writes only the JSON, as it always has. `data` may be engine
 * memory; the caller guards the read. */
static void rawmap_after_write(const char *data, size_t len, const char *path, unsigned long long wrote)
{
    sh_rawmap_save_report r;
    char *why = NULL, *copy, *zip;

    memset(&r, 0, sizeof r);
    r.mods = rawmap_mods_of(data, len);
    r.json = sh_text(path);
    r.json_bytes = wrote;
    if (r.mods && (zip = sh_rawmap_companion_zip_path(path)) != NULL) {
        copy = transport_copy(data, len);
        if (copy) {
            if (transport_start(RT_EXPORT_BESIDE, zip, copy, len, r.mods, sh_text(path), wrote, &why)) {
                free(zip);
                sh_rawmap_save_report_free(&r);
                return;   /* the report is the export's, when it is done */
            }
        } else rawmap_why(&why, "the game ran out of memory");
        r.zip = zip;
        r.zip_error = why;
        rawmap_tell_free(sh_textf("This map uses %u mod%s, but %s was not written (%s). The .json alone will not load "
                                  "for others.", r.mods, r.mods == 1 ? "" : "s", rawmap_base_name(zip), rawmap_s(why)));
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
static char *g_save_target;   /* malloc'd, or NULL for the default */

/* Defined further down, next to the status JSON that is its other caller. */
static char *json_escape(const char *src);

/* The folder part of `path` (malloc'd), or NULL when it has none. */
static char *dest_dir_part(const char *path)
{
    const char *cut = strrchr(path, '\\');
    const char *fwd = strrchr(path, '/');
    size_t n;
    char *out;

    if (fwd && (!cut || fwd > cut)) cut = fwd;
    if (cut == NULL) return NULL;

    n = (size_t)(cut - path);
    if (n == 0) n = 1;                       /* "\file.json" -- the root of the current drive */
    out = (char *)malloc(n + 2);
    if (!out) return NULL;
    memcpy(out, path, n);
    out[n] = '\0';
    if (n == 2 && out[1] == ':') { out[2] = '\\'; out[3] = '\0'; }   /* "D:" -> "D:\" */
    return out;
}

/* Create every folder in `dir` that is missing, through its long form. Walks left to right so the
 * parents come first, because CreateDirectory makes one level at a time. Returns 1 when the folder
 * exists afterwards; 0 leaves the Windows error as the last error. */
static int ensure_dir(const char *dir)
{
    wchar_t *w;
    size_t i, n;
    DWORD attr;
    int ok = 1;

    attr = path_attributes(dir);
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    w = sh_rawmap_path_wide(dir);
    if (!w) return 0;
    n = wcslen(w);

    /* Start past the root so "\\?\D:\" and "\\?\UNC\server\share" are never handed to
     * CreateDirectory. */
    if (!wcsncmp(w, L"\\\\?\\UNC\\", 8)) {
        int seps = 0;
        for (i = 8; i < n && seps < 2; i++) if (w[i] == L'\\') seps++;
    } else {
        i = (n >= 6 && w[5] == L':') ? 7 : 4;
    }

    for (; ok && i <= n; ++i) {
        wchar_t c = w[i];
        if (c != L'\\' && c != L'\0') continue;
        w[i] = L'\0';
        attr = GetFileAttributesW(w);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ok = 0;
        } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            SetLastError(ERROR_DIRECTORY);   /* a FILE is sitting where we need a folder */
            ok = 0;
        }
        w[i] = c;
    }
    if (ok) {
        attr = GetFileAttributesW(w);
        ok = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }
    {
        DWORD err = GetLastError();
        free(w);
        SetLastError(err);
    }
    return ok;
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
static int dest_folder_is_usable(const char *path, char **why)
{
    char *dir;
    int ok = 1;

    if (path == NULL || path[0] == '\0') {
        rawmap_why(why, "no path given");
        return 0;
    }
    dir = dest_dir_part(path);
    if (!dir) {
        rawmap_why(why, "that is a name, not a path. Give the whole path, like D:\\rawmaps\\mine.json");
        return 0;
    }
    if (path[strlen(path) - 1] == '\\' || path[strlen(path) - 1] == '/') {
        rawmap_why(why, "that is a folder. Put a file name on the end");
        ok = 0;
    } else if (dir[1] == ':') {
        char root[4]; root[0] = dir[0]; root[1] = ':'; root[2] = '\\'; root[3] = '\0';
        if (GetDriveTypeA(root) <= DRIVE_NO_ROOT_DIR) {
            if (why) { free(*why); *why = sh_textf("there is no %s drive on this machine", root); }
            ok = 0;
        }
    }
    free(dir);
    return ok;
}

int sh_rawmap_dest_path_is_usable(const char *path, char **why)
{
    return dest_folder_is_usable(path, why);
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
int sh_rawmap_dest_writable_now(const char *path, char **why)
{
    DWORD attr;

    if (!dest_folder_is_usable(path, why)) return 0;

    attr = path_attributes(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 1;      /* not there yet: the save creates it */

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        rawmap_why(why, "a folder already has that name");
        return 0;
    }
    if (attr & FILE_ATTRIBUTE_READONLY) {
        rawmap_why(why, "that file is read-only. Untick Read-only in its Properties, or save somewhere else");
        return 0;
    }
    return 1;
}

/* Point saves at `path`. "" or NULL goes back to the default rawmap.json.
 * Refuses a path whose folder does not exist, so a bare word cannot become a
 * file in DOOM's own install folder. 0 also when memory runs out (the target
 * is then unchanged). */
int sh_rawmap_set_save_target(const char *path)
{
    char *chosen = NULL, *dflt, *old;
    if (path && path[0]) {
        if (!dest_folder_is_usable(path, NULL)) return 0;
        dflt = default_rawmap_path();
        if (!dflt) return 0;
        if (_stricmp(path, dflt) != 0 && (chosen = sh_text(path)) == NULL) { free(dflt); return 0; }
        free(dflt);
    }
    AcquireSRWLockExclusive(&g_paths_lock);
    old = g_save_target;
    g_save_target = chosen;
    ReleaseSRWLockExclusive(&g_paths_lock);
    free(old);
    return 1;
}

char *sh_rawmap_save_target(void)
{
    char *out;
    AcquireSRWLockShared(&g_paths_lock);
    out = sh_text(g_save_target ? g_save_target : "");
    ReleaseSRWLockShared(&g_paths_lock);
    return out;
}

/* Where a save writes (malloc'd): the target, else the default; NULL when memory ran out. */
static char *resolve_dest_path(void)
{
    char *out = NULL;
    int set;
    AcquireSRWLockShared(&g_paths_lock);
    set = g_save_target != NULL;
    if (set) out = sh_text(g_save_target);
    ReleaseSRWLockShared(&g_paths_lock);
    return set ? out : default_rawmap_path();
}

char *sh_rawmap_save_path(void) { return resolve_dest_path(); }

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
int sh_rawmap_write_from_live(void *map, const char *destination, char **msg, unsigned long long *out_bytes)
{
    unsigned char blk[IDSTR_SIZE];
    unsigned long long wrote = 0;
    int len = 0;
    const char *data = NULL;
    unsigned char rc = 0;
    int laid_out = 0, zip_started = 0;
    char *zip_why = NULL, *line;
    DWORD write_error = 0;

    if (msg) *msg = NULL;
    if (out_bytes) *out_bytes = 0;

    if (!sh_rawmap_live_serialize_ready()) {
        rawmap_why(msg, InterlockedCompareExchange(&g_live_faulted, 0, 0)
                        ? "Saving rawmaps stopped working this session after an error; restart DOOM"
                        : "Saving rawmaps is unavailable on this game build");
        return 0;
    }
    if (map == NULL) {
        rawmap_why(msg, "Open a map in the editor first");
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
                    zip_started = rawmap_write_zip(data, (size_t)len, destination, &zip_why);
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
        free(zip_why);
        rawmap_why(msg, "Saving the open map failed with an engine error; restart DOOM to save rawmaps again");
        return 0;
    }

    if (!rc) {
        backend_log("RAWMAP: live serialize -- the engine declined to serialize the map");
        rawmap_why(msg, "DOOM could not write this map out; nothing was saved");
        return 0;
    }
    if (wrote == 0 && modexport_is_zip_path(destination)) {
        if (msg) *msg = sh_textf("%s was not written: %s", destination, rawmap_s(zip_why));
        free(zip_why);
        return 0;
    }
    free(zip_why);
    if (wrote == 0) {
        line = sh_textf("RAWMAP: live serialize produced %d bytes but the write to %s failed (error %lu)",
                        len, destination, (unsigned long)write_error);
        backend_log(line ? line : "RAWMAP: live serialize produced the map but the write failed");
        free(line);
        if (msg)
            *msg = sh_textf("%s could not be written: %s", destination,
                            sh_reason_win32(write_error ? write_error : ERROR_WRITE_FAULT, destination, 0));
        return 0;
    }

    InterlockedExchange64(&g_last_bytes, (LONGLONG)wrote);
    InterlockedIncrement(&g_save_count);
    line = sh_textf("RAWMAP: saved %llu bytes from the open map -> %s%s", wrote, destination, laid_out ? " [pretty]" : "");
    backend_log(line ? line : "RAWMAP: saved the open map");
    free(line);
    if (msg)
        *msg = sh_textf(zip_started ? "Writing the open map and its mods to %s; Snapmap+ says when it is done."
                                    : "Saved the open map to %s.", destination);
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
    char *dir = NULL;
    wchar_t *wpath = NULL, *temp = NULL;
    size_t total = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    int attempt, flushed = 0;
    DWORD err = 0;

    if (out_error) *out_error = 0;
    if (!data || !len || !path || !path[0]) return 0;
    if ((wpath = sh_rawmap_path_wide(path)) == NULL) { err = GetLastError(); goto fail; }
    if ((dir = dest_dir_part(path)) == NULL) { err = ERROR_PATH_NOT_FOUND; goto fail; }
    if (!ensure_dir(dir)) { err = GetLastError() ? GetLastError() : ERROR_PATH_NOT_FOUND; goto fail; }
    /* CREATE_NEW never truncates someone else's scratch file. Separate writers
     * must own separate temporaries until each publishes its complete result. */
    for (attempt = 0; attempt < 64; ++attempt) {
        free(temp);
        temp = sh_wtextf(L"%s.smp-%lu-%lu.tmp", wpath, GetCurrentProcessId(),
                         (unsigned long)InterlockedIncrement(&sequence));
        if (!temp) { err = ERROR_NOT_ENOUGH_MEMORY; goto fail; }
        h = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) break;
        err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) goto fail;
    }
    if (h == INVALID_HANDLE_VALUE) goto fail;
    err = 0;
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
        goto fail;
    }
    free(temp);
    free(wpath);
    free(dir);
    return (unsigned long long)total;
fail:
    if (out_error) *out_error = err ? err : ERROR_WRITE_FAULT;
    free(temp);
    free(wpath);
    free(dir);
    return 0;
}

#ifdef SH_RAWMAP_TESTING
/* Test-only door onto the writer: the destination rules only mean anything in terms
 * of what actually reaches disk. Compiled out of shipping builds. */
unsigned long long sh_rawmap_test_write(const char *data, size_t len)
{
    char *path = resolve_dest_path(), *why = NULL;
    unsigned long long wrote;
    if (!path) return 0;
    if (modexport_is_zip_path(path)) {
        wrote = rawmap_write_zip(data, len, path, &why) ? len : 0;
        free(why);
        free(path);
        return wrote;
    }
    wrote = write_file_atomic(data, len, path, NULL);
    if (wrote) rawmap_after_write(data, len, path, wrote);   /* as Save Rawmap does */
    free(path);
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
    char *body = NULL, *place, err[96] = "";
    const char *segment;
    size_t body_len = 0;

    if (!g_idstr_assign || out_idstr == NULL) return;
    if (!read_out_idstr(out_idstr, &data, &len)) return;
    if (!(place = sh_modserve_place_get())) {
        backend_log("MODS: the map's place was not saved (out of memory); it reopens finding its mods at the root");
        return;
    }
    segment = place[0] ? place + sizeof SH_MODSERVE_PLACES : NULL;
    __try {
        body = sh_carrier_put_place(data, (size_t)len, segment, &body_len, err, sizeof err);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        body = NULL;
        strcpy_s(err, sizeof err, "it faulted");
    }
    free(place);
    if (body == NULL) {
        if (err[0]) {
            char *line = sh_textf("MODS: the map's place was not saved (%s); it reopens finding its mods at the root", err);
            backend_log(line ? line : "MODS: the map's place was not saved; it reopens finding its mods at the root");
            free(line);
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

/* `src` as a JSON string body (malloc'd; NULL reads as ""), escaping what a Windows path can actually
 * contain. Backslash is the whole reason this exists -- an unescaped "C:\maps\x.json" makes the
 * frontend's JSON.parse throw, so the paths this file reports would break the very menu that shows
 * them. Quote and the C0 range are escaped for completeness. NULL when memory runs out. */
static char *json_escape(const char *src)
{
    size_t w = 0, n = src ? strlen(src) : 0;
    char *out = (char *)malloc(n * 6 + 1);   /* every byte escapes to at most six */
    if (!out) return NULL;
    for (; src && *src; ++src) {
        unsigned char c = (unsigned char)*src;
        if      (c == '\\') { out[w++] = '\\'; out[w++] = '\\'; }
        else if (c == '"')  { out[w++] = '\\'; out[w++] = '"'; }
        else if (c == '\n') { out[w++] = '\\'; out[w++] = 'n'; }
        else if (c == '\r') { out[w++] = '\\'; out[w++] = 'r'; }
        else if (c == '\t') { out[w++] = '\\'; out[w++] = 't'; }
        else if (c < 0x20) w += (size_t)sprintf_s(out + w, 7, "\\u%04x", c);
        else out[w++] = (char)c;
    }
    out[w] = '\0';
    return out;
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
    if (!sh_rawmap_validate_source(path, NULL)) return 0;

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

int sh_rawmap_validate_source(const char *path, char **why)
{
    HANDLE h;
    LARGE_INTEGER sz;
    char probe[64];
    DWORD rd = 0;
    int i;

    if (why) *why = NULL;
    if (path == NULL || path[0] == '\0') {
        rawmap_why(why, "no path given");
        return 0;
    }
    if (modexport_is_zip_path(path)) return modexport_validate_source(path, why);

    h = path_open_read(path);
    if (h == INVALID_HANDLE_VALUE) {
        rawmap_why(why, sh_reason_win32(GetLastError(), path, 0));
        return 0;
    }

    /* Match the signed native length used by read_source_file and idStr.
     * Report an unrepresentable file before a later map-load attempt. */
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h);
        rawmap_why(why, "that file is empty");
        return 0;
    }
    if (sz.QuadPart > (LONGLONG)INT_MAX) {
        CloseHandle(h);
        rawmap_why(why, "that file is over 2 GB, too large to be a map");   /* idStr's own int length */
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
        rawmap_why(why, "that file is not a rawmap; choose a .json or .zip written by Save Rawmap");
        return 0;
    }
    return 1;
}

#define RAWMAP_OPEN_UNSAFE \
    "Opening rawmaps is unavailable on this game build: Snapmap+ cannot stop DOOM's Save from overwriting " \
    "the map a rawmap opens over"

int sh_rawmap_source_ok(char **why)
{
    char *path;
    int ok;
    if (why) *why = NULL;
    if (!sh_rawmap_load_is_safe()) {
        rawmap_why(why, RAWMAP_OPEN_UNSAFE);
        return 0;
    }
    path = resolve_source_path();
    if (!path) {
        rawmap_why(why, "the game ran out of memory; restart DOOM and try again");
        return 0;
    }
    ok = sh_rawmap_validate_source(path, why);
    free(path);
    return ok;
}

int sh_rawmap_open_now(const char *path, char **msg)
{
    char *reason = NULL;
    int queued;
    if (msg) *msg = NULL;
    if (!sh_rawmap_load_is_safe()) {
        rawmap_why(msg, RAWMAP_OPEN_UNSAFE);
        return 0;
    }
    if (path && path[0]) {
        /* Nothing is remembered for later: a file that cannot open now is refused now. */
        if (!sh_editor_frame_can_reload(msg)) return 0;
        if (!sh_rawmap_validate_source(path, &reason)) {
            rawmap_tell_load_refused(path, reason);
            RAWMAP_DIAG(1, "refused", path, 0, rawmap_s(reason), 0, 0);
            if (msg) { free(*msg); *msg = reason; }
            else free(reason);
            return 0;
        }
        free(reason);
        if (!sh_rawmap_swap_set_source(path)) {
            rawmap_why(msg, "the game ran out of memory; restart DOOM and try again");
            RAWMAP_DIAG(1, "refused", path, 0, "out of memory", 0, 0);
            return 0;
        }
    }
    queued = sh_editor_frame_request_reload(msg);
#ifdef SH_DIAG
    {
        char *src = resolve_source_path();
        RAWMAP_DIAG(1, queued ? "queued" : "refused", rawmap_s(src), 0, queued || !msg ? "" : rawmap_s(*msg), 0, 0);
        free(src);
    }
#endif
    return queued;
}

#ifdef SH_DIAG
static SRWLOCK g_diag_lock = SRWLOCK_INIT;
static struct {
    unsigned long opens, seq;
    int faulted, rc, mods;
    size_t bytes;
    char state[16];   /* one of the fixed state words below */
    char *path;       /* whole (malloc'd), or NULL */
    char *reason;     /* whole (malloc'd), or NULL */
} g_diag;

/* new_seq starts a request (sh_rawmap_open_now); later notes update it. mods < 0 keeps the count. */
static void rawmap_diag_note(int new_seq, const char *state, const char *path, int rc,
                             const char *reason, size_t bytes, int mods)
{
    char *kept_path = path && path[0] ? sh_text(path) : NULL, *kept_reason = sh_text(reason ? reason : "");
    AcquireSRWLockExclusive(&g_diag_lock);
    if (new_seq || !g_diag.seq) {
        g_diag.seq++;
        g_diag.bytes = 0; g_diag.mods = 0; g_diag.rc = 0;
    }
    strncpy_s(g_diag.state, sizeof g_diag.state, state, _TRUNCATE);
    if (kept_path) { free(g_diag.path); g_diag.path = kept_path; }
    g_diag.rc = rc;
    free(g_diag.reason);
    g_diag.reason = kept_reason;
    if (bytes) g_diag.bytes = bytes;
    if (mods >= 0) g_diag.mods = mods;
    if (!strcmp(state, "opened")) g_diag.opens++;
    if (!strcmp(state, "faulted")) g_diag.faulted = 1;
    ReleaseSRWLockExclusive(&g_diag_lock);
}

char *sh_rawmap_diag_status(void)
{
    char *path, *reason, *out;
    int pending;
    AcquireSRWLockShared(&g_diag_lock);
    path = json_escape(g_diag.path);
    reason = json_escape(g_diag.reason);
    pending = !strcmp(g_diag.state, "queued") || !strcmp(g_diag.state, "opening");
    out = path && reason
        ? sh_textf("{\"opens\":%lu,\"generation\":%lu,\"pending\":%d,\"faulted\":%d,\"last\":{\"seq\":%lu,"
                   "\"path\":\"%s\",\"state\":\"%s\",\"rc\":%d,\"reason\":\"%s\",\"bytes\":%llu,\"mods\":%d}}",
                   g_diag.opens, sh_rawmap_load_generation(), pending, g_diag.faulted, g_diag.seq, path,
                   g_diag.seq ? g_diag.state : "", g_diag.rc, reason, (unsigned long long)g_diag.bytes, g_diag.mods)
        : NULL;
    ReleaseSRWLockShared(&g_diag_lock);
    free(path);
    free(reason);
    return out;
}
#endif

/* Hand `text` to the window as owned interface text in *out (the old one
 * released); `text` (malloc'd) is freed. */
static void rawmap_give(char **out, char *text)
{
    if (out) sh_iface_text_set(out, text);
    free(text);
}

static char *slot_rawmap_status(sh_iface *self)
{
    char *load_path, *save_path, *target, *load_esc, *save_esc, *target_esc, *json_esc, *zip_esc, *err_esc, *text;
    sh_rawmap_save_report last;

    (void)self;
    load_path = resolve_source_path();
    save_path = resolve_dest_path();
    target = sh_rawmap_save_target();
    sh_rawmap_last_save(&last);
    load_esc = json_escape(load_path);
    save_esc = json_escape(save_path);
    target_esc = json_escape(target);
    json_esc = json_escape(last.json);
    zip_esc = json_escape(last.zip);
    err_esc = json_escape(last.zip_error);

    /* Paths are UTF-8 (the page reads the status as UTF-8). `saveTarget` is the
     * file Save writes to, or "" for the default; it rides the status because
     * the console can set it too. What the last save wrote, and how many mods
     * the open map uses (the File menu offers .zip first when it uses any). */
    text = load_esc && save_esc && target_esc && json_esc && zip_esc && err_esc
         ? sh_iface_textf("{\"load\":\"%s\",\"save\":\"%s\",\"saves\":%lu,\"lastBytes\":%llu,\"loads\":%lu,"
                          "\"saveTarget\":\"%s\",\"lastJson\":\"%s\",\"lastJsonBytes\":%llu,\"lastZip\":\"%s\","
                          "\"lastZipBytes\":%llu,\"lastMods\":%u,\"lastZipError\":\"%s\",\"mapMods\":%d}",
                          load_esc, save_esc, sh_rawmap_save_count(), sh_rawmap_save_last_bytes(),
                          sh_rawmap_swap_count(), target_esc, json_esc, last.json_bytes, zip_esc, last.zip_bytes,
                          last.mods, err_esc, sh_rawmap_open_map_mods())
         : NULL;
    sh_rawmap_save_report_free(&last);
    free(load_path); free(save_path); free(target);
    free(load_esc); free(save_esc); free(target_esc); free(json_esc); free(zip_esc); free(err_esc);
    return text;
}

/* `arm` is a verb code (the vtable is matched slot for slot between the DLLs):
 *   -1 leave alone   2 make `load_path` the file to open now (it is checked,
 *   and refused when the editor cannot open it now)   3 keep saving to the
 *   current save path   4 saves go back to the default file   5 save now.
 * 0 and 1 named the retired every-load switch and are refused. */
static int slot_rawmap_configure(sh_iface *self, const char *load_path, const char *save_path,
                                 int arm, char **out_msg)
{
    char *reason = NULL, refusal[128];
    int ok = 1;

    (void)self;
    if (out_msg) *out_msg = NULL;
    /* Opening or saving a rawmap needs the map-load and map-save hooks. */
    if ((arm == 2 || arm == 5 || (save_path && save_path[0])) &&
        sh_feature_refusal(SH_FEATURE_RAWMAPS, refusal, sizeof refusal)) {
        rawmap_give(out_msg, sh_text(refusal));
        return 0;
    }

    if (load_path != NULL) {
        if (load_path[0] == '\0') {
            sh_rawmap_swap_set_source(NULL);          /* empty = restore the default */
        } else {
            if (arm == 2 && !sh_rawmap_load_is_safe()) {
                rawmap_give(out_msg, sh_text(RAWMAP_OPEN_UNSAFE));
                return 0;
            }
            if (arm == 2 && !sh_editor_frame_can_reload(&reason)) {
                rawmap_give(out_msg, reason);
                return 0;
            }
            if (!sh_rawmap_validate_source(load_path, &reason)) {
                rawmap_tell_load_refused(load_path, reason);
                rawmap_give(out_msg, reason);
                return 0;
            }
            free(reason);
            reason = NULL;
            if (!sh_rawmap_swap_set_source(load_path)) {
                rawmap_give(out_msg, sh_text("the game ran out of memory; restart DOOM and try again"));
                return 0;
            }
        }
    }

    /* A save always uses the live editor. An unavailable or busy editor must
     * not export a different saved map. */
    if ((save_path && save_path[0]) || arm == 5) {
        ok = sh_editor_frame_request_rawmap_save_to(save_path, &reason);
        rawmap_give(out_msg, reason);
        if (!ok) return 0;
    } else if (save_path) {
        ok = sh_rawmap_set_save_target(NULL);
    }

    /* 3 pins whatever the save path already resolves to, so ticking never
     * moves the destination -- it only makes the current one stick. 4 releases it. */
    if (arm == 3) {
        char *now = resolve_dest_path();
        if (now && now[0]) sh_rawmap_set_save_target(now);
        free(now);
    }
    else if (arm == 4) sh_rawmap_set_save_target(NULL);

    if (!ok && out_msg && !*out_msg) rawmap_give(out_msg, sh_text("that save path cannot be used"));
    return ok;
}

/* Open the load path now. The engine call and its frame-boundary discipline live in
 * editor_frame.c; this is only the slot. */
static int slot_rawmap_load_now(sh_iface *self, char **out_msg)
{
    char refusal[128], *msg = NULL;
    int ok;
    (void)self;
    if (out_msg) *out_msg = NULL;
    if (sh_feature_refusal(SH_FEATURE_RAWMAPS, refusal, sizeof refusal)) {
        rawmap_give(out_msg, sh_text(refusal));
        return 0;
    }
    ok = sh_rawmap_open_now(NULL, &msg);
    rawmap_give(out_msg, msg);
    return ok;
}

void sh_rawmap_get_slots(sh_rawmap_status_fn *status, sh_rawmap_configure_fn *configure,
                         sh_rawmap_load_now_fn *load_now)
{
    if (status)    *status    = slot_rawmap_status;
    if (configure) *configure = slot_rawmap_configure;
    if (load_now)  *load_now  = slot_rawmap_load_now;
}
