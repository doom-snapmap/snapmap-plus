/* modcmd.c -- see modcmd.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "modcmd.h"
#include "modbundle.h"
#include "modstore.h"
#include "modserve.h"
#include "overrides.h"
#include "backend_log.h"

/* A stock resource every 13954591 SnapMap install ships; reading it proves the
 * installed-resource reader works before its answers are trusted. */
#define MC_STOCK_PROBE "generated/decls/md6def/zion/characters/monsters/imp/base/imp_snapeditor.md6.decl"

static void mc_line(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
    sh_printf("%s\n", line);
}

/* 1 present, 0 absent, -1 the reader faulted. */
static int mc_read_stock(const char *request)
{
    unsigned char *body = NULL;
    size_t len = 0;
    __try {
        body = sh_overrides_read_engine_resource(request, &len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (!body) return 0;
    HeapFree(GetProcessHeap(), 0, body);
    return 1;
}

typedef struct mc_stock { int state; unsigned asked; } mc_stock;   /* state: 0 unprobed, 1 ok, -1 unavailable */

static int mc_stock_exists(void *ctx, const char *request)
{
    mc_stock *s = (mc_stock *)ctx;
    if (s->state == 0) s->state = mc_read_stock(MC_STOCK_PROBE) == 1 ? 1 : -1;
    if (s->state < 0) return -1;
    s->asked++;
    return mc_read_stock(request);
}

/* Rejoin a path the console split into several tokens. */
static void mc_join_args(idCmdArgs *a, int from, char *out, size_t cap)
{
    int n = cmd_argc(a), i;
    size_t w = 0;
    out[0] = 0;
    for (i = from; i < n; i++) {
        const char *t = cmd_argv(a, i);
        size_t len;
        if (!t) continue;
        len = strlen(t);
        /* No separator around a ':' token: "C" ":" "\x" is one drive path. */
        if (w && t[0] != ':' && out[w - 1] != ':' && w + 1 < cap) out[w++] = ' ';
        if (w + len >= cap) break;
        memcpy(out + w, t, len);
        w += len;
        out[w] = 0;
    }
}

static int mc_resolve_folder(const char *arg, char *out, size_t cap)
{
    char root[MAX_PATH * 4];
    int absolute = (arg[0] && arg[1] == ':') || arg[0] == '\\' || arg[0] == '/';
    if (absolute) { strncpy_s(out, cap, arg, _TRUNCATE); return 1; }
    if (!sh_ms_root(root, sizeof root)) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", root, arg);
    return 1;
}

/* The serving index reads bundle.json at boot and on rescan only, so a bundle
 * written or installed now is invisible until the table is rebuilt. */
static void mc_rescan(const char *why)
{
    int n = sh_modserve_index_rescan();
    if (n < 0) mc_line("MODS: %s: the serving index could not be rebuilt; run 'sh_mods rescan'", why);
    else mc_line("MODS: %s: serving index rebuilt (%d bundle(s) active); new editor rows need 'sh_mods palette rebuild'",
                 why, n);
}

/* 1 when `folder` is inside the store root (a bundle the index will read). */
static int mc_in_store(const char *folder)
{
    char root[MAX_PATH * 4];
    size_t n;
    if (!sh_ms_root(root, sizeof root)) return 0;
    n = strlen(root);
    return n && !_strnicmp(folder, root, n) && (folder[n] == '\\' || folder[n] == '/');
}

static void mc_needs_line(const char *verb, const sh_mb_bundle *b)
{
    mc_line("MODS: %s %s@%s+%llu: needs %u (in bundle %u, installed mods %u, game %u, unresolved %u)",
            verb, b->id, b->version, b->build, b->needs.total, b->needs.local, b->needs.installed,
            b->needs.stock, b->needs.unresolved);
    if (b->warnings)
        mc_line("MODS: %s %s@%s+%llu: %u warning(s), first: %s", verb, b->id, b->version, b->build, b->warnings, b->first_warning);
}

static void mc_build(const char *folder)
{
    sh_ms_list list;
    sh_mb_resolver r;
    sh_mb_bundle b;
    mc_stock stock = { 0, 0 };
    char cause[SH_MB_CAUSE_MAX];
    int listed = sh_ms_enumerate(&list);
    if (!listed) memset(&list, 0, sizeof list);
    mc_line("MODS: build %s: reading the descriptor and assets/ (%zu installed bundle(s) consulted)", folder, list.count);
    r.installed = sh_ms_request_exists;
    r.installed_context = &list;
    r.stock = mc_stock_exists;
    r.stock_context = &stock;
    if (!sh_mb_build(folder, &r, &b)) {
        if (b.id[0]) {
            mc_line("MODS: build %s: %s@%s+%llu from %s, %zu file(s), %llu byte(s) hashed", folder, b.id, b.version, b.build,
                    b.descriptor, b.file_count, b.total_size);
            mc_needs_line("build", &b);
        }
        if (stock.state < 0) mc_line("MODS: build %s: the game's resource reader is unavailable; stock names were not checked", folder);
        mc_line("MODS: build %s: refused, %u error(s), first: %s", folder, b.errors, b.first_cause);
        sh_mb_free(&b);
        sh_ms_list_free(&list);
        return;
    }
    mc_line("MODS: build %s: %s@%s+%llu from %s, %zu file(s), %llu byte(s) hashed", folder, b.id, b.version, b.build,
            b.descriptor, b.file_count, b.total_size);
    mc_needs_line("build", &b);
    if (!sh_mb_write(&b, folder, cause, sizeof cause))
        mc_line("MODS: build %s: valid but not written: %s", folder, cause);
    else {
        mc_line("MODS: build %s: bundle %s written to bundle.json", folder, b.hash);
        if (mc_in_store(folder)) mc_rescan("build");
    }
    sh_mb_free(&b);
    sh_ms_list_free(&list);
}

static void mc_install(const char *folder)
{
    sh_mb_resolver r;
    sh_ms_install_stats st;
    sh_mb_bundle b;
    mc_stock stock = { 0, 0 };
    char key[SH_MS_KEY_MAX + 1], cause[SH_MB_CAUSE_MAX];
    int ok;
    mc_line("MODS: install %s: building", folder);
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mc_stock_exists;
    r.stock_context = &stock;
    ok = sh_ms_install_from_folder(folder, &r, key, sizeof key, &st, &b, cause, sizeof cause);
    if (b.id[0]) {
        mc_line("MODS: install %s: %s@%s+%llu from %s, %zu file(s), %llu byte(s)%s", folder, b.id, b.version, b.build,
                b.descriptor, b.file_count, b.total_size, b.errors ? "" : ", bundle.json written");
        mc_needs_line("install", &b);
    }
    if (st.blobs_written || st.blobs_reused)
        mc_line("MODS: install %s: blobs written %u, already stored %u", folder, st.blobs_written, st.blobs_reused);
    if (st.linked || st.copied)
        mc_line("MODS: install %s: tree laid out, %u hard link(s), %u copie(s)%s", folder, st.linked, st.copied,
                st.replaced ? ", replacing the earlier build" : "");
    if (st.in_place && ok) mc_line("MODS: install %s: already in the store; built in place", folder);
    if (!ok) mc_line("MODS: install %s: refused: %s", folder, cause);
    else {
        mc_line("MODS: install %s: installed and verified as %s (bundle %s)", folder, key, b.hash);
        mc_rescan("install");
    }
    sh_mb_free(&b);
}

static void mc_list(void)
{
    sh_ms_list list;
    char root[MAX_PATH * 4];
    size_t i;
    if (!sh_ms_root(root, sizeof root)) { mc_line("MODS: list: the store folder cannot be resolved"); return; }
    if (!sh_ms_enumerate(&list)) { mc_line("MODS: list: %s cannot be read", root); return; }
    mc_line("MODS: list: %zu bundle folder(s) under %s", list.count, root);
    for (i = 0; i < list.count; i++) {
        const sh_ms_entry *e = &list.items[i];
        const char *state = sh_ms_entry_usable(e) ? (e->has_state ? "verified" : "built, not installed")
                                                  : "unavailable";
        if (e->loaded)
            mc_line("MODS: list: %s%s  %.16s  %zu file(s)  %llu byte(s)  %s%s%s", e->legacy ? "legacy " : "", e->key,
                    e->bundle.hash, e->bundle.file_count, e->bundle.total_size, state,
                    e->cause[0] ? ": " : "", e->cause);
        else
            mc_line("MODS: list: %s%s  %s: %s", e->legacy ? "legacy " : "", e->key, state, e->cause);
    }
    sh_ms_list_free(&list);
}

/* Write a stock resource's bytes to a file, so an author can place a copy of
 * an installed file under a new name in a mod's assets/ tree. The bytes are
 * read through the resource layer's installed-resource reader (the game's
 * own archives, never another mod). An existing file is not replaced. */
static void mc_copystock(const char *request, const char *dest)
{
    unsigned char *body = NULL;
    size_t len = 0;
    wchar_t *w;
    HANDLE h;
    DWORD wrote = 0;
    int faulted = 0;
    __try {
        body = sh_overrides_read_engine_resource(request, &len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = 1;
        body = NULL;
    }
    if (!body) {
        mc_line("MODS: copystock %s: %s", request,
                faulted ? "the game's resource reader faulted" : "not readable through the game's resource layer");
        return;
    }
    w = sh_mb_wpath(dest);
    if (w) {
        /* Create every missing parent folder below the drive. */
        wchar_t *p = w + 4;
        if (p[0] && p[1] == L':') p += 3;
        for (; *p; p++) {
            if (*p != L'\\') continue;
            *p = 0;
            CreateDirectoryW(w, NULL);
            *p = L'\\';
        }
    }
    h = w ? CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        mc_line("MODS: copystock %s: %s cannot be created (%s)", request, dest,
                e == ERROR_FILE_EXISTS ? "it already exists" : "error");
    } else {
        int ok = WriteFile(h, body, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len;
        CloseHandle(h);
        if (ok) mc_line("MODS: copystock %s: %zu byte(s) written to %s", request, len, dest);
        else {
            DeleteFileW(w);
            mc_line("MODS: copystock %s: writing %s failed", request, dest);
        }
    }
    free(w);
    HeapFree(GetProcessHeap(), 0, body);
}

static void mc_verify(const char *key)
{
    sh_ms_verify_stats st;
    char cause[SH_MB_CAUSE_MAX];
    int ok;
    mc_line("MODS: verify %s: rehashing", key);
    ok = sh_ms_verify(key, &st, cause, sizeof cause);
    mc_line("MODS: verify %s: %u folder(s), %u file(s), %u MiB, %u separate blob(s) checked", key, st.entries,
            st.files, st.bytes_mb, st.blobs_checked);
    if (ok) mc_line("MODS: verify %s: pass", key);
    else mc_line("MODS: verify %s: FAILED: %s", key, cause);
}

void h_sh_mods(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
    char arg[MAX_PATH * 4], folder[MAX_PATH * 4];
    if (verb && (!_stricmp(verb, "help") || !strcmp(verb, "?"))) {
        sh_printf("%s\n", SH_MODCMD_HELP);
        return;
    }
    /* Serving verbs (modserve_engine.c): status (also the bare command),
     * rescan, palette rebuild. */
    if (!verb || !verb[0]) verb = "status";
    if (sh_modserve_command(a, verb)) return;
    if (!_stricmp(verb, "list")) { mc_list(); return; }
    mc_join_args(a, 2, arg, sizeof arg);
    if (!_stricmp(verb, "verify")) {
        if (!arg[0]) { sh_printf("usage: sh_mods verify \"<id@version+build>\"\n"); return; }
        mc_verify(arg);
        return;
    }
    if (!_stricmp(verb, "copystock")) {
        const char *request = cmd_argv(a, 2);
        char dest_arg[MAX_PATH * 4];
        mc_join_args(a, 3, dest_arg, sizeof dest_arg);
        if (!request || !request[0] || !dest_arg[0]) {
            sh_printf("usage: sh_mods copystock \"<engine path>\" \"<file>\"\n");
            return;
        }
        if (!mc_resolve_folder(dest_arg, folder, sizeof folder)) { mc_line("MODS: copystock: the store folder cannot be resolved"); return; }
        mc_copystock(request, folder);
        return;
    }
    if (_stricmp(verb, "build") && _stricmp(verb, "install")) {
        sh_printf("sh_mods: unknown verb '%s'. %s\n", verb, SH_MODCMD_HELP);
        return;
    }
    if (!arg[0]) { sh_printf("usage: sh_mods %s \"<folder>\"\n", verb); return; }
    if (!mc_resolve_folder(arg, folder, sizeof folder)) { mc_line("MODS: %s: the store folder cannot be resolved", verb); return; }
    if (!_stricmp(verb, "build")) mc_build(folder);
    else mc_install(folder);
}

int modbundle_build_folder(const char *folder_acp, char *first_cause, size_t n)
{
    sh_ms_list list;
    sh_mb_resolver r;
    sh_mb_bundle b;
    mc_stock stock = { 0, 0 };
    char cause[SH_MB_CAUSE_MAX], folder[MAX_PATH * 4];
    wchar_t wide[MAX_PATH * 2];
    int listed, ok = 0;
    if (first_cause && n) first_cause[0] = 0;
    /* The legacy converter's paths are ANSI (SHGetFolderPathA); the builder
     * takes UTF-8. */
    if (!folder_acp || !folder_acp[0] ||
        !MultiByteToWideChar(CP_ACP, 0, folder_acp, -1, wide, (int)(sizeof wide / sizeof wide[0])) ||
        !WideCharToMultiByte(CP_UTF8, 0, wide, -1, folder, (int)sizeof folder, NULL, NULL)) {
        if (first_cause && n) strncpy_s(first_cause, n, "no usable folder path", _TRUNCATE);
        return 0;
    }
    listed = sh_ms_enumerate(&list);
    if (!listed) memset(&list, 0, sizeof list);
    r.installed = sh_ms_request_exists;
    r.installed_context = &list;
    r.stock = mc_stock_exists;
    r.stock_context = &stock;
    if (!sh_mb_build(folder, &r, &b)) {
        if (first_cause && n) strncpy_s(first_cause, n, b.first_cause, _TRUNCATE);
        mc_line("MODS: build %s: refused, %u error(s), first: %s", folder, b.errors, b.first_cause);
    } else if (!sh_mb_write(&b, folder, cause, sizeof cause)) {
        if (first_cause && n) strncpy_s(first_cause, n, cause, _TRUNCATE);
        mc_line("MODS: build %s: valid but not written: %s", folder, cause);
    } else {
        ok = 1;
        mc_line("MODS: build %s: %s@%s+%llu, %zu file(s), %llu byte(s); bundle %s written", folder, b.id,
                b.version, b.build, b.file_count, b.total_size, b.hash);
        mc_needs_line("build", &b);
    }
    if (stock.state < 0) mc_line("MODS: build %s: the game's resource reader is unavailable; stock names were not checked", folder);
    sh_mb_free(&b);
    sh_ms_list_free(&list);
    return ok;
}

int sh_modcmd_register(void)
{
    char root[MAX_PATH * 4];
    if (sh_ms_root(root, sizeof root)) mc_line("MODS: sh_mods ready; store %s", root);
    else mc_line("MODS: sh_mods ready; the store folder cannot be resolved");
    return 1;
}
