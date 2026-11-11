/* saved_map_guard.c -- see saved_map_guard.h.
 *
 * DeleteBadSaveSlots (VK 0xFCEE30, GL 0xFCE320) takes the browser helper and
 * the idList<idStr> of slot names the savegame manager marked unreadable while
 * it listed My Maps; it deletes each slot and then queues GDM 0x8D ("removed
 * corrupt data"). It is the only caller of the per-slot delete, so replacing it
 * leaves the player's own Delete alone. The detour never calls the original. */
#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../common/wide_path.h"
#include "fault_record.h"
#include "hook.h"
#include "recovery.h"
#include "saved_map_guard.h"

/* idList<idStr>: element array, then the count. An idStr is 0x30 bytes with
 * its length at +0x08 and its text at +0x10 (the engine's own layout, the same
 * one editor_frame.c builds for LoadMap). */
#define SMG_LIST_COUNT_OFF 0x08
#define SMG_IDSTR_STRIDE   0x30
#define SMG_IDSTR_LEN_OFF  0x08
#define SMG_IDSTR_DATA_OFF 0x10

/* The prologue through `sub rsp,0xF0`: mov rax,rsp; push rbp/r14/r15;
 * lea rbp,[rax-0x5F]; sub rsp,0xF0. Every byte is position-independent. */
#define SMG_STOLEN 19

typedef void (*delete_bad_slots_fn)(void *helper, void *names);
static delete_bad_slots_fn g_original;

/* Where saves are read and where copies go: Saved Games\id Software\DOOM\base\
 * savegame.user and %LOCALAPPDATA%\snapmap-plus\kept-maps, both long form. */
static wchar_t g_saves_root[SH_WPATH_MAX];
static wchar_t g_kept_root[SH_WPATH_MAX];

/* Slots already told about this session. */
#define SMG_TOLD_MAX 64
static char g_told[SMG_TOLD_MAX][29];
static int  g_told_count;

typedef enum smg_keep_result {
    SMG_KEPT_COPY,     /* every file copied and compared byte for byte */
    SMG_KEPT_IN_PLACE, /* not copied, but every file is still in the saved games folder */
    SMG_NOT_FOUND      /* no slot folder of that name */
} smg_keep_result;

typedef struct smg_kept {
    smg_keep_result result;
    char folder[29];                 /* SNAPMAPS<20 hex> */
    char title[96];                  /* the map's name, or the folder name */
    char where[SH_U8PATH_MAX / 64];  /* the copy's folder, or the slot's own folder */
} smg_kept;

/* "SNAPMAPS<20 hex>" from a slot name with or without the prefix; 0 when the
 * name is anything else (it is then never touched). */
static int smg_folder_name(const char *name, size_t len, char out[29])
{
    size_t i, skip = 0;
    if (len == 28 && !strncmp(name, "SNAPMAPS", 8)) skip = 8;
    else if (len != 20) return 0;
    for (i = skip; i < len; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return 0;
    }
    memcpy(out, "SNAPMAPS", 8);
    memcpy(out + 8, name + skip, 20);
    out[28] = '\0';
    return 1;
}

static int smg_roots(void)
{
    if (!g_saves_root[0]) {
        PWSTR known = NULL;
        static const GUID kSavedGames =
            { 0x4C5C32FFu, 0xBB9D, 0x43b0, { 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4 } };
        if (FAILED(SHGetKnownFolderPath(&kSavedGames, 0, NULL, &known)) || !known) {
            if (known) CoTaskMemFree(known);
            return 0;
        }
        if (wcscpy_s(g_saves_root, SH_WPATH_MAX, known) != 0) g_saves_root[0] = L'\0';
        CoTaskMemFree(known);
        if (!g_saves_root[0] || !sh_wpath_long(g_saves_root, SH_WPATH_MAX)
            || !sh_wpath_append(g_saves_root, SH_WPATH_MAX, L"id Software\\DOOM\\base\\savegame.user")) {
            g_saves_root[0] = L'\0';
            return 0;
        }
    }
    if (!g_kept_root[0]) {
        if (!sh_wpath_local_appdata(g_kept_root, SH_WPATH_MAX)
            || !sh_wpath_append(g_kept_root, SH_WPATH_MAX, L"snapmap-plus\\kept-maps")) {
            g_kept_root[0] = L'\0';
            return 0;
        }
    }
    return 1;
}

/* The slot's folder under any account folder of savegame.user. */
static int smg_find_slot(const char *folder, wchar_t *out, size_t cap)
{
    static wchar_t glob[SH_WPATH_MAX];
    WIN32_FIND_DATAW acct;
    HANDLE h;
    int found = 0;
    wcscpy_s(glob, SH_WPATH_MAX, g_saves_root);
    if (!sh_wpath_append(glob, SH_WPATH_MAX, L"*")) return 0;
    h = FindFirstFileW(glob, &acct);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        DWORD attr;
        if (!(acct.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || acct.cFileName[0] == L'.') continue;
        wcscpy_s(out, cap, g_saves_root);
        if (!sh_wpath_append(out, cap, acct.cFileName) || !sh_wpath_append_utf8(out, cap, folder)) continue;
        attr = GetFileAttributesW(out);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)
            && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) { found = 1; break; }
    } while (FindNextFileW(h, &acct));
    FindClose(h);
    return found;
}

/* 1 when the two files hold the same bytes. */
static int smg_same_file(const wchar_t *a, const wchar_t *b)
{
    HANDLE ha, hb;
    int same = 0;
    ha = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (ha == INVALID_HANDLE_VALUE) return 0;
    hb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hb != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sa, sb;
        if (GetFileSizeEx(ha, &sa) && GetFileSizeEx(hb, &sb) && sa.QuadPart == sb.QuadPart) {
            static unsigned char ba[65536], bb[65536];
            DWORD ra, rb;
            same = 1;
            while (same) {
                if (!ReadFile(ha, ba, sizeof ba, &ra, NULL) || !ReadFile(hb, bb, sizeof bb, &rb, NULL)
                    || ra != rb || memcmp(ba, bb, ra) != 0) { same = 0; break; }
                if (ra == 0) break;
            }
        }
        CloseHandle(hb);
    }
    CloseHandle(ha);
    return same;
}

/* Copy every plain file of slot into dest, each compared after the copy.
 * Returns the number of files, or -1 when any file could not be kept. */
static int smg_copy_slot(const wchar_t *slot, const wchar_t *dest)
{
    static wchar_t glob[SH_WPATH_MAX], src[SH_WPATH_MAX], dst[SH_WPATH_MAX];  /* main thread only */
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, ok = 1;
    wcscpy_s(glob, SH_WPATH_MAX, slot);
    if (!sh_wpath_append(glob, SH_WPATH_MAX, L"*")) return -1;
    h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wcscpy_s(src, SH_WPATH_MAX, slot);
        wcscpy_s(dst, SH_WPATH_MAX, dest);
        if (!sh_wpath_append(src, SH_WPATH_MAX, fd.cFileName) || !sh_wpath_append(dst, SH_WPATH_MAX, fd.cFileName)
            || !CopyFileW(src, dst, FALSE) || !smg_same_file(src, dst)) { ok = 0; break; }
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok && n > 0 ? n : -1;
}

/* 1 when dest already holds exactly the slot's files. */
static int smg_copy_matches(const wchar_t *slot, const wchar_t *dest)
{
    static wchar_t glob[SH_WPATH_MAX], src[SH_WPATH_MAX], dst[SH_WPATH_MAX];  /* main thread only */
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int same = 1, n = 0;
    wcscpy_s(glob, SH_WPATH_MAX, slot);
    if (!sh_wpath_append(glob, SH_WPATH_MAX, L"*")) return 0;
    h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        wcscpy_s(src, SH_WPATH_MAX, slot);
        wcscpy_s(dst, SH_WPATH_MAX, dest);
        if (!sh_wpath_append(src, SH_WPATH_MAX, fd.cFileName) || !sh_wpath_append(dst, SH_WPATH_MAX, fd.cFileName)
            || !smg_same_file(src, dst)) { same = 0; break; }
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return same && n > 0;
}

/* The map's name from game.details ("metadata={...\"name\":\"...\"...}"). */
static void smg_title(const wchar_t *slot, char *out, size_t cap, const char *fallback)
{
    static wchar_t path[SH_WPATH_MAX];
    char text[8192];
    DWORD got = 0;
    HANDLE h;
    const char *p;
    size_t n = 0;
    strcpy_s(out, cap, fallback);
    wcscpy_s(path, SH_WPATH_MAX, slot);
    if (!sh_wpath_append(path, SH_WPATH_MAX, L"game.details")) return;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, text, sizeof text - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    text[got] = '\0';
    p = strstr(text, "\"name\":\"");
    if (!p) return;
    p += 8;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        if ((unsigned char)*p >= 0x20) out[n++] = *p;
        p++;
    }
    if (n) out[n] = '\0';
}

/* Keep one slot: never deleted, and copied whole when the copy can be made. */
static void smg_keep(const char *folder, smg_kept *k)
{
    static wchar_t slot[SH_WPATH_MAX], dest[SH_WPATH_MAX];
    int attempt;
    memset(k, 0, sizeof *k);
    strcpy_s(k->folder, sizeof k->folder, folder);
    strcpy_s(k->title, sizeof k->title, folder);
    k->result = SMG_NOT_FOUND;
    if (!smg_roots() || !smg_find_slot(folder, slot, SH_WPATH_MAX)) return;
    smg_title(slot, k->title, sizeof k->title, folder);
    k->result = SMG_KEPT_IN_PLACE;
    sh_wpath_utf8(slot, k->where, sizeof k->where);
    /* kept-maps\SNAPMAPS<id>, or <id>-2, -3 ... when an earlier copy holds other bytes. */
    if (!sh_wpath_ensure_dir(g_kept_root)) {
        static wchar_t parent[SH_WPATH_MAX];
        wchar_t *slash;
        wcscpy_s(parent, SH_WPATH_MAX, g_kept_root);
        slash = wcsrchr(parent, L'\\');
        if (slash) *slash = L'\0';
        if (!sh_wpath_ensure_dir(parent) || !sh_wpath_ensure_dir(g_kept_root)) return;
    }
    for (attempt = 1; attempt <= 9; attempt++) {
        char leaf[40];
        DWORD attr;
        wcscpy_s(dest, SH_WPATH_MAX, g_kept_root);
        if (attempt == 1) strcpy_s(leaf, sizeof leaf, folder);
        else _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s-%d", folder, attempt);
        if (!sh_wpath_append_utf8(dest, SH_WPATH_MAX, leaf)) return;
        attr = GetFileAttributesW(dest);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (!smg_copy_matches(slot, dest)) continue;
        } else if (!sh_wpath_ensure_dir(dest) || smg_copy_slot(slot, dest) < 0) {
            return;
        }
        k->result = SMG_KEPT_COPY;
        sh_wpath_utf8(dest, k->where, sizeof k->where);
        return;
    }
}

static int smg_told(const char *folder)
{
    int i;
    for (i = 0; i < g_told_count; i++) if (!strcmp(g_told[i], folder)) return 1;
    if (g_told_count < SMG_TOLD_MAX) strcpy_s(g_told[g_told_count++], sizeof g_told[0], folder);
    return 0;
}

/* The engine's dialog carries at most 255 bytes (engine_dialog.c), so the title
 * is cut at 40 bytes on a character boundary and a long folder path is named
 * by its known-folder form. */
#define SMG_SENTENCE_MAX 255
#define SMG_TITLE_MAX 40

static void smg_short_title(const char *title, char *out, size_t cap)
{
    size_t n = strlen(title);
    if (n <= SMG_TITLE_MAX) { strcpy_s(out, cap, title); return; }
    n = SMG_TITLE_MAX;
    while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80) n--;   /* not inside a UTF-8 sequence */
    memcpy(out, title, n);
    strcpy_s(out + n, cap - n, "...");
}

/* The player's sentence for one kept slot, or 0 when nothing may be claimed. */
static int smg_sentence(const smg_kept *k, char *out, size_t cap)
{
    char title[SMG_TITLE_MAX + 4], where[96];
    int n;
    if (k->result != SMG_KEPT_COPY && k->result != SMG_KEPT_IN_PLACE) return 0;
    smg_short_title(k->title, title, sizeof title);
    n = _snprintf_s(out, cap, _TRUNCATE, k->result == SMG_KEPT_COPY
        ? "DOOM could not read your saved map \"%s\" and was about to delete it; Snapmap+ kept it, with a copy to send in a report in %s."
        : "DOOM could not read your saved map \"%s\" and was about to delete it; Snapmap+ kept it in %s.",
        title, k->where);
    if (n > 0 && n <= SMG_SENTENCE_MAX) return 1;
    if (k->result == SMG_KEPT_COPY)
        _snprintf_s(where, sizeof where, _TRUNCATE, "%%LOCALAPPDATA%%\\snapmap-plus\\kept-maps\\%s", k->folder);
    else
        _snprintf_s(where, sizeof where, _TRUNCATE, "your saved games folder (%s)", k->folder);
    n = _snprintf_s(out, cap, _TRUNCATE, k->result == SMG_KEPT_COPY
        ? "DOOM could not read your saved map \"%s\" and was about to delete it; Snapmap+ kept it, with a copy to send in a report in %s."
        : "DOOM could not read your saved map \"%s\" and was about to delete it; Snapmap+ kept it in %s.",
        title, where);
    return n > 0 && n <= SMG_SENTENCE_MAX;
}

static void smg_delete_bad_slots(void *helper, void *names)
{
    (void)helper;
    __try {
        const uint8_t *arr = *(const uint8_t *const *)names;
        int count = *(const int *)((const uint8_t *)names + SMG_LIST_COUNT_OFF);
        int i;
        if (!arr || count <= 0 || count > 4096) return;
        for (i = 0; i < count; i++) {
            const uint8_t *s = arr + (size_t)i * SMG_IDSTR_STRIDE;
            const char *text = *(const char *const *)(s + SMG_IDSTR_DATA_OFF);
            int len = *(const int *)(s + SMG_IDSTR_LEN_OFF);
            char folder[29], line[SH_U8PATH_MAX / 64 + 256], said[SH_U8PATH_MAX / 64 + 512];
            smg_kept k;
            if (!text || len <= 0 || !smg_folder_name(text, (size_t)len, folder)) continue;
            smg_keep(folder, &k);
            _snprintf_s(line, sizeof line, _TRUNCATE, "saved-map guard: DOOM's scan rejected %s; not deleted, %s%s",
                        folder,
                        k.result == SMG_KEPT_COPY ? "copy verified in " :
                        k.result == SMG_KEPT_IN_PLACE ? "the copy failed; files left in " : "no such folder",
                        k.result == SMG_NOT_FOUND ? "" : k.where);
            {
                shield_fault f = { "save", -1, line, 0, 0 };
                shield_emit(&f);
            }
            if (!smg_told(folder) && smg_sentence(&k, said, sizeof said)) shield_tell(said);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shield_fault f = { "save", -1, "saved-map guard: the engine's list of unreadable slots could not be read; "
                                       "nothing was deleted", 0, 0 };
        shield_emit(&f);
    }
}

int saved_map_guard_install(const sig_result *results, size_t count)
{
    const sig_result *r = sig_lookup(results, count, "DeleteBadSaveSlots");
    void *tramp;
    if (g_original) return 1;
    if (!r || r->status != SIG_OK) {
        shield_fault f = { "sig", r ? (int)r->status : -1,
            "saved-map guard off: DeleteBadSaveSlots is unresolved, so DOOM deletes saved maps it cannot read", 0, 0 };
        shield_emit(&f);
        return 0;
    }
    tramp = hook_prepare((void *)r->addr, (void *)smg_delete_bad_slots, SMG_STOLEN);
    if (!tramp || hook_commit(tramp) != B2_PATCH_OK) {
        shield_fault f = { "sig", -1,
            "saved-map guard off: DeleteBadSaveSlots could not be detoured, so DOOM deletes saved maps it cannot read",
            0, 0 };
        if (tramp) hook_unpatch(tramp);
        shield_emit(&f);
        return 0;
    }
    g_original = (delete_bad_slots_fn)tramp;
    {
        shield_fault f = { "action", -1, "saved-map guard on: DOOM keeps saved maps it cannot read", 0, 0 };
        shield_emit(&f);
    }
    return 1;
}
