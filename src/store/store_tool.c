/* store_tool.c -- see store_tool.h. */
#include <windows.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store_tool.h"
#include "store_recycle.h"
#include "modstore.h"
#include "modmigrate.h"
#include "modlegacy.h"
#include "modstock.h"
#include "modbundle.h"
#include "backend_log.h"

#define ST_PATH_MAX (MAX_PATH * 4)

static sh_store_out_fn g_out;
static void *g_out_ctx;
static sh_ms_discard_fn g_discard;
static void *g_discard_ctx;
static const char *g_discard_verb;
static int (*g_running)(void);
static int g_steam = 1;
static int g_log_on;
static sh_store_recycle_stats g_rc;

void sh_store_tool_set_output(sh_store_out_fn fn, void *context) { g_out = fn; g_out_ctx = context; }
void sh_store_tool_set_discard(sh_ms_discard_fn fn, void *context, const char *verb)
{
    g_discard = fn;
    g_discard_ctx = context;
    g_discard_verb = verb;
}
void sh_store_tool_set_doom_running(int (*fn)(void)) { g_running = fn; }
void sh_store_tool_set_steam(int steam) { g_steam = steam; }

/* ------------------------------------------------------------ output ---- */

static void st_emit(int err, const char *line)
{
    if (g_out) { g_out(g_out_ctx, err, line); return; }
    fputs(line, err ? stderr : stdout);
    fputs("\n", err ? stderr : stdout);
    fflush(err ? stderr : stdout);
}

static void st_say(int err, const char *fmt, ...)
{
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    st_emit(err, line);
}

/* The backend's log lines are the tool's action lines while a step runs. */
void backend_log(const char *msg)
{
    if (!g_log_on || !msg || !msg[0]) return;
    if (!strncmp(msg, "MODS: ", 6)) msg += 6;
    st_say(0, "  %s", msg);
}

/* ------------------------------------------------------------- paths ---- */

static char *st_utf8(const wchar_t *w, UINT cp, int *lossy)
{
    BOOL used = FALSE;
    int n = WideCharToMultiByte(cp, cp == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS, w, -1, NULL, 0, NULL,
                                cp == CP_UTF8 ? NULL : &used);
    char *out;
    if (n <= 0) return NULL;
    out = (char *)malloc((size_t)n);
    if (!out) return NULL;
    WideCharToMultiByte(cp, cp == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS, w, -1, out, n, NULL, cp == CP_UTF8 ? NULL : &used);
    if (lossy) *lossy = used != FALSE;
    return out;
}

/* An absolute path without a trailing separator. */
static wchar_t *st_full(const wchar_t *path)
{
    DWORD need = GetFullPathNameW(path, 0, NULL, NULL);
    wchar_t *out;
    size_t n;
    if (!need) return NULL;
    out = (wchar_t *)malloc(sizeof(wchar_t) * (need + 1));
    if (!out || !GetFullPathNameW(path, need + 1, out, NULL)) { free(out); return NULL; }
    n = wcslen(out);
    while (n > 3 && (out[n - 1] == L'\\' || out[n - 1] == L'/')) out[--n] = 0;
    return out;
}

static wchar_t *st_join(const wchar_t *dir, const wchar_t *leaf)
{
    size_t a = wcslen(dir), b = wcslen(leaf);
    wchar_t *p = (wchar_t *)malloc(sizeof(wchar_t) * (a + b + 2));
    if (!p) return NULL;
    memcpy(p, dir, sizeof(wchar_t) * a);
    p[a] = L'\\';
    memcpy(p + a + 1, leaf, sizeof(wchar_t) * (b + 1));
    return p;
}

static int st_is_dir(const wchar_t *path)
{
    DWORD a = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int st_is_file(const wchar_t *dir, const wchar_t *leaf)
{
    wchar_t *p = st_join(dir, leaf);
    DWORD a = p ? GetFileAttributesW(p) : INVALID_FILE_ATTRIBUTES;
    free(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* A DOOM 2016 folder: an executable and the archive index the stock bridge reads. */
static int st_doom_folder(const wchar_t *dir)
{
    wchar_t *base;
    int ok;
    if (!st_is_dir(dir) || !(st_is_file(dir, L"DOOMx64vk.exe") || st_is_file(dir, L"DOOMx64.exe"))) return 0;
    base = st_join(dir, L"base");
    ok = base && st_is_file(base, L"gameresources.pindex");
    free(base);
    return ok;
}

/* DOOM through Steam: SteamPath, then every library in libraryfolders.vdf. */
static wchar_t *st_steam_doom(void)
{
    wchar_t steam[MAX_PATH * 2], *vdf_path, *found = NULL;
    DWORD size = sizeof steam;
    char *text = NULL, *p;
    FILE *f = NULL;
    long len;
    int pass;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, steam, &size) != ERROR_SUCCESS)
        return NULL;
    {
        wchar_t *c;
        for (c = steam; *c; c++) if (*c == L'/') *c = L'\\';
    }
    vdf_path = st_join(steam, L"steamapps\\libraryfolders.vdf");
    if (vdf_path && _wfopen_s(&f, vdf_path, L"rb") == 0 && f) {
        if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && len < (16 << 20) && fseek(f, 0, SEEK_SET) == 0) {
            text = (char *)malloc((size_t)len + 1);
            if (text && fread(text, 1, (size_t)len, f) == (size_t)len) text[len] = 0;
            else { free(text); text = NULL; }
        }
        fclose(f);
    }
    free(vdf_path);
    /* The Steam folder first, then each "path" value (backslashes are doubled in the file). */
    for (pass = 0, p = text; !found; pass++) {
        wchar_t lib[MAX_PATH * 2], *cand;
        if (pass == 0) wcscpy_s(lib, MAX_PATH * 2, steam);
        else {
            char value[MAX_PATH * 4], *q, *end;
            size_t k = 0;
            if (!p || !(q = strstr(p, "\"path\""))) break;
            q += 6;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '"') { p = q; continue; }
            end = ++q;
            while (*end && *end != '"' && k + 1 < sizeof value) {
                if (end[0] == '\\' && end[1] == '\\') end++;
                value[k++] = *end++;
            }
            value[k] = 0;
            p = *end ? end + 1 : end;
            if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, lib, MAX_PATH * 2)) continue;
        }
        cand = st_join(lib, L"steamapps\\common\\DOOM");
        if (cand && st_doom_folder(cand)) found = cand;
        else free(cand);
    }
    free(text);
    return found;
}

static int st_doom_is_running(void)
{
    PROCESSENTRY32W pe;
    HANDLE snap;
    int running = 0;
    if (g_running) return g_running();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, L"DOOMx64vk.exe") || !_wcsicmp(pe.szExeFile, L"DOOMx64.exe")) running = 1;
        } while (!running && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return running;
}

/* -------------------------------------------------------------- scan ---- */

typedef struct st_store {
    wchar_t *dir;                       /* the data folder, absolute */
    char *u8;                           /* the same, UTF-8 */
    char *acp;                          /* the same in the system code page, or NULL when it cannot be spelled there */
    char mods[ST_PATH_MAX];             /* <store>\mods, UTF-8 */
} st_store;

typedef struct st_scan {
    sh_ms_upgrade_check uc;
    int layout_work, bin_work;
    int ov_present, ov_due;
    unsigned ov_entries;
    int ov_changed;                     /* entries added or changed since a completed migration */
    int kinds;
} st_scan;

static unsigned st_count(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = st_join(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    unsigned n = 0;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) n++; while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static int st_scan_store(const st_store *s, st_scan *out)
{
    wchar_t *ov;
    char *section = NULL;
    int saved = g_log_on;
    memset(out, 0, sizeof *out);
    g_log_on = 0;
    if (sh_ms_upgrade_scan(&out->uc) < 0) { g_log_on = saved; return 0; }
    out->layout_work = out->uc.layout_folders || out->uc.blobs_present || out->uc.legacy_present || out->uc.record_files ||
                       out->uc.tidy_drops || out->uc.tidy_renames || out->uc.tidy_migration;
    out->bin_work = out->uc.bin_present || out->uc.bin_section;
    ov = st_join(s->dir, L"overrides");
    out->ov_present = st_is_dir(ov);
    if (out->ov_present) {
        out->ov_entries = st_count(ov);
        out->ov_due = !(sh_mb_record_get(s->mods, SH_MODMIGRATE_RECORD, &section) == 1 && section &&
                        strstr(section, "\"complete\":true"));
        free(section);
        if (!out->ov_due && s->acp) {
            unsigned entries = 0;
            int pending = sh_modmigrate_pending(&entries);
            out->ov_changed = pending > 0 ? pending : 0;
        }
    }
    free(ov);
    g_log_on = saved;
    out->kinds = (out->uc.layout_folders > 0) + out->uc.blobs_present + out->uc.legacy_present + (out->uc.record_files > 0) +
                 (out->uc.tidy_drops + out->uc.tidy_renames > 0 || out->uc.tidy_migration) + out->uc.bin_present +
                 (out->uc.bin_section && !out->uc.bin_present) + out->ov_due;
    return 1;
}

static void st_size(unsigned long long bytes, char *out, size_t cap)
{
    if (bytes >= (1ull << 20)) _snprintf_s(out, cap, _TRUNCATE, "%llu MiB", bytes >> 20);
    else _snprintf_s(out, cap, _TRUNCATE, "%llu KiB", (bytes + 1023) >> 10);
}

static void st_list(const st_scan *sc)
{
    const sh_ms_upgrade_check *u = &sc->uc;
    char size[32];
    if (u->layout_folders)
        st_say(0, "  mods\\: %u folder(s) in the retired layout (bundle.json and .state.json beside assets\\)", u->layout_folders);
    if (u->blobs_present) {
        st_size(u->blob_bytes, size, sizeof size);
        st_say(0, "  mods\\blobs\\: %u file(s), %s, shared with the mod folders by hard links", u->blob_files, size);
    }
    if (u->legacy_present) st_say(0, "  mods\\legacy\\: %u package(s) a map delivered", u->legacy_folders);
    if (u->record_files) st_say(0, "  mods\\: old record file(s) %s", u->record_names);
    if (u->tidy_drops + u->tidy_renames > 0 || u->tidy_migration) {
        char parts[256] = "";
        if (u->tidy_drops) _snprintf_s(parts, sizeof parts, _TRUNCATE, "%u cop%s of an installed mod that a map delivered",
                                       u->tidy_drops, u->tidy_drops == 1 ? "y" : "ies");
        if (u->tidy_renames) {
            size_t n = strlen(parts);
            _snprintf_s(parts + n, sizeof parts - n, _TRUNCATE, "%s%u folder(s) to rename", n ? ", " : "", u->tidy_renames);
        }
        if (u->tidy_migration) {
            size_t n = strlen(parts);
            _snprintf_s(parts + n, sizeof parts - n, _TRUNCATE, "%sa finished migration record to shorten", n ? ", " : "");
        }
        st_say(0, "  mods\\: %s", parts);
    }
    if (u->bin_present)
        st_say(0, "  removed-mods\\: %u entr%s of the retired recycle bin", u->bin_entries, u->bin_entries == 1 ? "y" : "ies");
    else if (u->bin_section) st_say(0, "  mods\\snapmap-plus.json: the retired recycle bin's record");
    if (sc->ov_due)
        st_say(0, "  overrides\\: %u entr%s not yet moved into mods\\", sc->ov_entries, sc->ov_entries == 1 ? "y" : "ies");
    if (sc->ov_changed)
        st_say(0, "  note: overrides\\ has %d entr%s added or changed since it was migrated; 'sh_mods migrate --again' in "
               "the game moves them", sc->ov_changed, sc->ov_changed == 1 ? "y" : "ies");
}

/* ---------------------------------------------------------- commands ---- */

static void st_usage(int err)
{
    st_emit(err, "snapmap-plus-store -- moves a Snapmap+ store from earlier releases to the current layout");
    st_emit(err, "");
    st_emit(err, "Usage:");
    st_emit(err, "  snapmap-plus-store check <store>");
    st_emit(err, "  snapmap-plus-store migrate <store> [--doom <DOOM folder>]");
    st_emit(err, "");
    st_emit(err, "<store> is the Snapmap+ data folder, usually %LOCALAPPDATA%\\snapmap-plus.");
    st_emit(err, "check lists what needs migrating and changes nothing (exit 0 nothing to do, 1 work to do, 2 error).");
    st_emit(err, "migrate does that work; everything it deletes goes to the Recycle Bin (exit 0 done, 1 partly done,");
    st_emit(err, "2 error). It needs the DOOM folder to move overrides\\ (found through Steam when --doom is not");
    st_emit(err, "given). Close DOOM first.");
}

static void st_store_free(st_store *s)
{
    free(s->dir);
    free(s->u8);
    free(s->acp);
}

static int st_store_open(const wchar_t *arg, st_store *s)
{
    int lossy = 0;
    memset(s, 0, sizeof *s);
    s->dir = st_full(arg);
    if (!s->dir || !st_is_dir(s->dir)) {
        char *u = st_utf8(arg, CP_UTF8, NULL);
        st_say(1, "error: %s is not a folder", u ? u : "the store");
        free(u);
        return 0;
    }
    s->u8 = st_utf8(s->dir, CP_UTF8, NULL);
    if (!s->u8 || strlen(s->u8) + 6 >= sizeof s->mods) { st_say(1, "error: the store path is too long"); return 0; }
    _snprintf_s(s->mods, sizeof s->mods, _TRUNCATE, "%s\\mods", s->u8);
    s->acp = st_utf8(s->dir, CP_ACP, &lossy);
    if (s->acp && (lossy || strlen(s->acp) + 32 >= MAX_PATH)) { free(s->acp); s->acp = NULL; }
    sh_ms_set_root(s->mods);
    /* The overrides migration and the legacy converter take the system code page. */
    sh_modmigrate_set_data_root(s->acp ? s->acp : "");
    sh_modlegacy_set_data_root(s->acp ? s->acp : "");
    return 1;
}

static int st_check(const st_store *s)
{
    st_scan sc;
    if (!st_scan_store(s, &sc)) { st_say(1, "error: the store folder %s cannot be read", s->u8); return SH_STORE_ERROR; }
    st_say(0, "Checking %s", s->u8);
    st_list(&sc);
    if (!sc.kinds) { st_say(0, "Nothing to migrate."); return SH_STORE_OK; }
    st_say(0, "%d kind(s) of work found. Run: snapmap-plus-store migrate \"%s\"", sc.kinds, s->u8);
    return SH_STORE_WORK;
}

static void st_part(char *out, size_t cap, const char *fmt, ...)
{
    size_t n = strlen(out);
    va_list ap;
    if (n && n + 2 < cap) { out[n++] = ','; out[n++] = ' '; out[n] = 0; }
    va_start(ap, fmt);
    _vsnprintf_s(out + n, cap - n, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static int st_migrate(const st_store *s, const wchar_t *doom_arg)
{
    st_scan before, after;
    wchar_t *doom = NULL;
    char *doom_u8 = NULL, done[1024] = "";
    int need_doom = 0, com, result;
    if (!st_scan_store(s, &before)) { st_say(1, "error: the store folder %s cannot be read", s->u8); return SH_STORE_ERROR; }
    if (doom_arg) {
        doom = st_full(doom_arg);
        if (!doom || !st_doom_folder(doom)) {
            char *u = st_utf8(doom_arg, CP_UTF8, NULL);
            st_say(1, "error: %s is not a DOOM 2016 folder (it needs DOOMx64vk.exe or DOOMx64.exe and "
                   "base\\gameresources.pindex); nothing was changed", u ? u : "the --doom folder");
            free(u);
            free(doom);
            return SH_STORE_ERROR;
        }
    }
    if (!before.kinds) {
        st_say(0, "Nothing to migrate.");
        free(doom);
        return SH_STORE_OK;
    }
    if (st_doom_is_running()) {
        st_say(1, "error: DOOM is running; close it and run this again (nothing was changed)");
        free(doom);
        return SH_STORE_ERROR;
    }
    if (!doom && before.ov_due && g_steam) doom = st_steam_doom();
    st_say(0, "Migrating %s", s->u8);
    memset(&g_rc, 0, sizeof g_rc);
    if (g_discard) sh_ms_set_discard(g_discard, g_discard_ctx, g_discard_verb);
    else sh_ms_set_discard(sh_store_recycle, &g_rc, "moved to the Recycle Bin");
    com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    g_log_on = 1;

    /* 1. The retired layout, then the second step (modstore.c). */
    if (before.layout_work) {
        sh_ms_upgrade_stats us;
        int r = sh_ms_upgrade(&us);
        if (r < 0) st_say(0, "  store: the mods folder cannot be read");
        else if (r > 0 && (us.failed || us.tidy_failed))
            st_part(done, sizeof done, "the store layout partly upgraded (%u folder(s) left as they were)", us.failed + us.tidy_failed);
        else if (r > 0) st_part(done, sizeof done, "the store layout upgraded");
    }
    /* 2. The retired recycle bin. */
    if (before.bin_work) {
        int r = sh_ms_bin_sweep(NULL, NULL);
        if (r >= 0) st_part(done, sizeof done, "removed-mods\\ retired");
    }
    /* 3. The retired overrides folder, with the game's archives as the stock bridge's install. */
    if (before.ov_due) {
        if (!s->acp) {
            st_say(0, "  overrides\\: not moved: the store's path cannot be spelled in this system's code page");
        } else if (!doom) {
            need_doom = 1;
            st_say(0, "  overrides\\: not moved yet: it needs the DOOM folder to tell game files from mod files; "
                   "DOOM was not found through Steam");
        } else {
            sh_modmigrate_stats ms;
            int r;
            doom_u8 = st_utf8(doom, CP_UTF8, NULL);
            sh_modstock_set_install(doom_u8);
            sh_modstock_register();
            st_say(0, "  overrides\\: moving into mods\\, reading game files from %s", doom_u8 ? doom_u8 : "?");
            r = sh_modmigrate_run(0, &ms);
            if (r > 0)
                st_part(done, sizeof done, "overrides\\ moved into mods\\ (%u built, %u delivered converted, %u reused, %u skipped, "
                        "%u refused)", ms.built, ms.converted, ms.reused, ms.skipped, ms.refused);
        }
    }
    g_log_on = 0;
    sh_ms_set_discard(NULL, NULL, NULL);
    if (com) CoUninitialize();

    /* Whatever a later run could still do makes this run partial. */
    if (!st_scan_store(s, &after)) {
        memset(&after, 0, sizeof after);
        after.kinds = 1;
    }
    if (!g_discard) st_part(done, sizeof done, "%u item(s) moved to the Recycle Bin", g_rc.recycled);
    if (after.kinds) {
        st_say(0, "Partly done%s%s. %d kind(s) of work are left; run again: snapmap-plus-store migrate \"%s\"%s", done[0] ? ": " : "",
               done, after.kinds, s->u8, need_doom ? " --doom \"<DOOM folder>\"" : "");
        result = SH_STORE_PARTIAL;
    } else {
        st_say(0, "Done: %s.", done[0] ? done : "nothing changed");
        result = SH_STORE_OK;
    }
    free(doom);
    free(doom_u8);
    return result;
}

int sh_store_tool_run(int argc, wchar_t **argv)
{
    const wchar_t *cmd = argc > 1 ? argv[1] : NULL, *store = NULL, *doom = NULL;
    st_store s;
    int i, r;
    if (!cmd || !_wcsicmp(cmd, L"help") || !_wcsicmp(cmd, L"--help") || !_wcsicmp(cmd, L"-h")) {
        st_usage(cmd ? 0 : 1);
        return cmd ? SH_STORE_OK : SH_STORE_ERROR;
    }
    if (_wcsicmp(cmd, L"check") && _wcsicmp(cmd, L"migrate")) {
        char *u = st_utf8(cmd, CP_UTF8, NULL);
        st_say(1, "error: unknown command \"%s\"", u ? u : "?");
        free(u);
        st_usage(1);
        return SH_STORE_ERROR;
    }
    for (i = 2; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"--doom") && !_wcsicmp(cmd, L"migrate")) {
            if (i + 1 >= argc) { st_say(1, "error: --doom needs the DOOM folder"); return SH_STORE_ERROR; }
            doom = argv[++i];
        } else if (argv[i][0] == L'-' && argv[i][1] == L'-') {
            char *u = st_utf8(argv[i], CP_UTF8, NULL);
            st_say(1, "error: unknown option %s", u ? u : "?");
            free(u);
            return SH_STORE_ERROR;
        } else if (!store) {
            store = argv[i];
        } else {
            st_say(1, "error: one store folder only");
            return SH_STORE_ERROR;
        }
    }
    if (!store) { st_say(1, "error: name the store folder, usually %%LOCALAPPDATA%%\\snapmap-plus"); st_usage(1); return SH_STORE_ERROR; }
    if (!st_store_open(store, &s)) { st_store_free(&s); return SH_STORE_ERROR; }
    r = !_wcsicmp(cmd, L"check") ? st_check(&s) : st_migrate(&s, doom);
    st_store_free(&s);
    return r;
}
