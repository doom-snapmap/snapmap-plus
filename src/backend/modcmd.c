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
#include "modnet.h"
#include "modsync.h"
#include "modmanifest.h"
#include "modexport.h"
#include "rawmap.h"
#include "editor_frame.h"
#include "apply_engine.h"
#include "modstock.h"
#include "modui.h"
#include "../common/text.h"

/* A stock resource every 13954591 SnapMap install ships; reading it proves the
 * installed-resource reader works before its answers are trusted. */
#define MC_STOCK_PROBE "generated/decls/md6def/zion/characters/monsters/imp/base/imp_snapeditor.md6.decl"

static void mc_line(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (!line) return;
    backend_log(line);
    sh_printf("%s\n", line);
    free(line);
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

/* The engine provider first, then the installed archives themselves (the
 * stock bridge): SnapMap's provider does not see campaign-only content in
 * gameresources, which is still stock to a bundle that shadows it. */
static int mc_stock_exists(void *ctx, const char *request)
{
    mc_stock *s = (mc_stock *)ctx;
    int got;
    if (s->state == 0) s->state = mc_read_stock(MC_STOCK_PROBE) == 1 ? 1 : -1;
    if (s->state < 0) return sh_modstock_exists(request);
    s->asked++;
    got = mc_read_stock(request);
    if (got == 0 && sh_modstock_exists(request) == 1) got = 1;
    return got;
}

/* One line per build or install that holds stock pointers. */
static void mc_stock_line(const char *verb, const char *folder, const sh_mb_bundle *b)
{
    if (b->stock_count)
        mc_line("MODS: %s %s: %u file(s) are installed game content (stock pointers), %llu byte(s) not stored", verb,
                folder, b->stock_count, b->stock_bytes);
}

/* Rejoin a path the console split into several tokens (malloc'd; "" when
 * there is none, NULL when memory runs out). */
static char *mc_join_args(idCmdArgs *a, int from)
{
    int n = cmd_argc(a), i;
    size_t w = 0, cap = 1;
    char *out;
    for (i = from; i < n; i++) cap += (cmd_argv(a, i) ? strlen(cmd_argv(a, i)) : 0) + 1;
    if (!(out = (char *)malloc(cap))) return NULL;
    out[0] = 0;
    for (i = from; i < n; i++) {
        const char *t = cmd_argv(a, i);
        size_t len;
        if (!t) continue;
        len = strlen(t);
        /* No separator around a ':' token: "C" ":" "\x" is one drive path. */
        if (w && t[0] != ':' && out[w - 1] != ':') out[w++] = ' ';
        memcpy(out + w, t, len);
        w += len;
        out[w] = 0;
    }
    return out;
}

/* The folder `arg` names: itself when absolute, else under the store root
 * (malloc'd); NULL when the root cannot be resolved. */
static char *mc_resolve_folder(const char *arg)
{
    const char *root;
    int absolute = (arg[0] && arg[1] == ':') || arg[0] == '\\' || arg[0] == '/';
    if (absolute) return _strdup(arg);
    if (!(root = sh_ms_root())) return NULL;
    return sh_textf("%s\\%s", root, arg);
}

/* The serving index reads mod.json at boot and on rescan only, so a bundle
 * written or installed now is invisible until the table is rebuilt. */
static void mc_rescan(const char *why)
{
    int n = sh_modserve_index_rescan();
    if (n < 0) mc_line("MODS: %s: the serving index could not be rebuilt; run 'sh_mods rescan'", why);
    else {
        sh_modserve_refresh rf;
        mc_line("MODS: %s: serving index rebuilt (%d bundle(s) active)", why, n);
        /* What the build changed under the running game (modserve.h). */
        (void)sh_modserve_refresh_after_rescan(&rf);
        sh_printf("sh_mods: refresh: %s\n", rf.note);
    }
    sh_modui_note_changed();   /* an open Mods tab gets a fresh document (the window's pushed refresh) */
}

/* 1 when `folder` is inside the store root (a bundle the index will read). */
static int mc_in_store(const char *folder)
{
    const char *root = sh_ms_root();
    size_t n;
    if (!root) return 0;
    n = strlen(root);
    return n && !_strnicmp(folder, root, n) && (folder[n] == '\\' || folder[n] == '/');
}

/* A built mod as the console names it: its name, else its folder. */
static const char *mc_name(const sh_mb_bundle *b, const char *folder)
{
    return b->name && b->name[0] ? b->name : folder;
}

static void mc_needs_line(const char *verb, const char *folder, const sh_mb_bundle *b)
{
    mc_line("MODS: %s %s: needs %u (in the mod %u, installed mods %u, game %u, unresolved %u)",
            verb, mc_name(b, folder), b->needs.total, b->needs.local, b->needs.installed,
            b->needs.stock, b->needs.unresolved);
    if (b->warnings)
        mc_line("MODS: %s %s: %u warning(s), first: %s", verb, mc_name(b, folder), b->warnings, sh_mb_first_warning(b));
}

/* The classes and the builder's report lines (additive mods). */
static void mc_classes_line(const char *verb, const char *folder, const sh_mb_bundle *b)
{
    char classes[160];
    size_t i;
    sh_mb_classes_text(&b->classes, classes, sizeof classes);
    mc_line("MODS: %s %s: %s", verb, folder, classes);
    for (i = 0; i < b->note_count; i++) mc_line("MODS: %s %s: %s", verb, folder, b->notes[i]);
}

/* The folder's own name (its last path segment), malloc'd. */
static char *mc_leaf(const char *folder)
{
    const char *leaf = folder, *p;
    char *out;
    size_t n;
    for (p = folder; *p; p++)
        if ((*p == '\\' || *p == '/') && p[1] && p[1] != '\\' && p[1] != '/') leaf = p + 1;
    if (!(out = _strdup(leaf))) return NULL;
    n = strlen(out);
    while (n && (out[n - 1] == '\\' || out[n - 1] == '/')) out[--n] = 0;
    return out;
}

/* A folder with no builder block is a new mod: it gets its salt before its
 * first build (sh_ms_assign_salt), its references to mods/<its folder name>/
 * rewritten to mods/<salt>/; the folder keeps its name. A folder that has its
 * salt is left as it is. 0 when refused. */
static int mc_assign_salt(const char *verb, const char *folder)
{
    char *leaf = mc_leaf(folder), salt[9], *cause = NULL;
    int r = 1;
    salt[0] = 0;
    if (!leaf) { mc_line("MODS: %s %s: refused: out of memory", verb, folder); return 0; }
    switch (sh_ms_assign_salt(folder, leaf, salt, &cause)) {
    case -1:
        mc_line("MODS: %s %s: refused: %s", verb, folder, cause ? cause : "out of memory");
        r = 0;
        break;
    case 1:
        mc_line("MODS: %s %s: a new mod: salt %s; its files under mods/%s/ now read mods/%s/", verb, folder, salt, leaf,
                salt);
        break;
    default:
        break;
    }
    free(cause);
    free(leaf);
    return r;
}

static void mc_build(const char *folder_in)
{
    sh_ms_list list;
    sh_mb_resolver r;
    sh_mb_bundle b;
    mc_stock stock = { 0, 0 };
    char *cause = NULL;
    const char *folder = folder_in;
    int listed;
    if (!mc_assign_salt("build", folder)) return;
    listed = sh_ms_enumerate(&list);
    if (!listed) memset(&list, 0, sizeof list);
    mc_line("MODS: build %s: reading the descriptor and assets/ (%zu installed bundle(s) consulted)", folder, list.count);
    r.installed = sh_ms_request_exists;
    r.installed_context = &list;
    r.stock = mc_stock_exists;
    r.stock_context = &stock;
    if (!sh_mb_build(folder, NULL, &r, &b)) {
        if (b.id[0]) {
            mc_line("MODS: build %s: %s, %zu file(s), %llu byte(s) hashed", folder, mc_name(&b, folder), b.file_count,
                    b.total_size);
            mc_needs_line("build", folder, &b);
            mc_classes_line("build", folder, &b);
        }
        if (stock.state < 0) mc_line("MODS: build %s: the game's resource reader is unavailable; stock names were not checked", folder);
        /* A folder that fails to build serves nothing until it is fixed. */
        mc_line("MODS: build %s: does not build (%u error(s)), first: %s; the folder serves nothing until it builds",
                folder, b.errors, sh_mb_first_cause(&b));
        if (mc_in_store(folder)) sh_ms_note_build_failure(folder, sh_mb_first_cause(&b));
        sh_mb_free(&b);
        sh_ms_list_free(&list);
        return;
    }
    mc_line("MODS: build %s: %s, %zu file(s), %llu byte(s) hashed", folder, mc_name(&b, folder), b.file_count,
            b.total_size);
    mc_needs_line("build", folder, &b);
    mc_stock_line("build", folder, &b);
    mc_classes_line("build", folder, &b);
    if (!sh_mb_write(&b, folder, &cause))
        mc_line("MODS: build %s: valid but not written: %s", folder, cause ? cause : "out of memory");
    else {
        mc_line("MODS: build %s: the build is written to mod.json's \"snapmap-plus\" block", folder);
        if (mc_in_store(folder)) {
            sh_ms_note_build(folder, &b);
            mc_rescan("build");
        }
    }
    free(cause);
    sh_mb_free(&b);
    sh_ms_list_free(&list);
}

static void mc_install_one(const char *folder, int rescan)
{
    sh_mb_resolver r;
    sh_ms_install_stats st;
    sh_mb_bundle b;
    mc_stock stock = { 0, 0 };
    char *name = NULL, *cause = NULL;
    int ok;
    mc_line("MODS: install %s: building", folder);
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mc_stock_exists;
    r.stock_context = &stock;
    ok = sh_ms_install_from_folder(folder, &r, &name, &st, &b, &cause);
    if (b.id[0]) {
        mc_line("MODS: install %s: %s, %zu file(s), %llu byte(s)%s", folder, mc_name(&b, folder), b.file_count,
                b.total_size, b.errors ? "" : ", mod.json written");
        mc_needs_line("install", folder, &b);
    }
    if (b.id[0]) mc_stock_line("install", folder, &b);
    if (st.copied)
        mc_line("MODS: install %s: %u file(s) copied into the store%s", folder, st.copied,
                st.replaced ? ", replacing the earlier build" : "");
    if (st.in_place && ok) mc_line("MODS: install %s: already in the store; built in place", folder);
    if (!ok) mc_line("MODS: install %s: refused: %s", folder, cause ? cause : "out of memory");
    else {
        mc_line("MODS: install %s: installed and verified as mods\\%s", folder, name);
        if (rescan) mc_rescan("install");
    }
    free(name);
    free(cause);
    sh_mb_free(&b);
}

/* `sh_mods install "<folder>"`: a mod folder installs as before; a folder
 * without mod.json installs every mod folder found below it, with one rescan. */
static void mc_install(const char *folder)
{
    char **mods = NULL;
    size_t count = 0, i, done = 0;
    if (sh_ms_folder_is_mod(folder)) { mc_install_one(folder, 1); return; }
    if (!sh_ms_find_mods(folder, &mods, &count) || !count) {
        mc_line("MODS: install %s: refused: no mod.json in it or in any folder below it", folder);
        sh_ms_find_free(mods, count);
        return;
    }
    mc_line("MODS: install %s: a folder of %zu mod(s)", folder, count);
    for (i = 0; i < count; i++) { mc_install_one(mods[i], 0); done++; }
    sh_ms_find_free(mods, count);
    if (done) mc_rescan("install");
}

/* The rebuild rule (modstore.h): every folder whose tree changed since its
 * last build is built again, with the game's resource reader. */
static void mc_sync(void)
{
    sh_ms_sync_stats st;
    sh_mb_resolver r;
    mc_stock stock = { 0, 0 };
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mc_stock_exists;
    r.stock_context = &stock;
    if (!sh_ms_sync(&r, &st)) { mc_line("MODS: rescan: the store cannot be read"); return; }
    mc_line("MODS: rescan: %u folder(s) checked, %u have a new current bundle, %u refused (%llu ms)", st.folders, st.rebuilt,
            st.failed, st.ms);
}

static void mc_list(void)
{
    sh_ms_list list;
    const char *root = sh_ms_root();
    size_t i;
    if (!root) { mc_line("MODS: list: the store folder cannot be resolved"); return; }
    if (!sh_ms_enumerate(&list)) { mc_line("MODS: list: %s cannot be read", root); return; }
    mc_line("MODS: list: %zu mod folder(s) under %s", list.count, root);
    for (i = 0; i < list.count; i++) {
        const sh_ms_entry *e = &list.items[i];
        const char *state = sh_ms_entry_usable(e) ? "installed" : "does not build";
        if (e->loaded) {
            char classes[160];
            sh_mb_classes_text(&e->bundle.classes, classes, sizeof classes);
            mc_line("MODS: list: mods\\%s  %s  %zu file(s)  %llu byte(s)  %s%s%s", e->name,
                    e->bundle.name && e->bundle.name[0] ? e->bundle.name : e->name, e->bundle.file_count,
                    e->bundle.total_size, state, e->cause[0] ? ": " : "", e->cause);
            mc_line("MODS: list: mods\\%s  %s", e->name, classes);
        } else
            mc_line("MODS: list: mods\\%s  %s: %s", e->name, state, e->cause);
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
    char *cause = NULL;
    int ok;
    mc_line("MODS: verify %s: rehashing", key);
    ok = sh_ms_verify(key, &st, &cause);
    mc_line("MODS: verify %s: %u folder(s), %u file(s) (%u stock pointer(s)), %u MiB checked", key, st.entries,
            st.files, st.pointers, st.bytes_mb);
    if (ok) mc_line("MODS: verify %s: pass", key);
    else mc_line("MODS: verify %s: FAILED: %s", key, cause ? cause : "out of memory");
    free(cause);
}

/* ------------------------------------------------------- store client ---- */

/* Console lines from the network worker. Each is logged at once; the
 * console prints them on DOOM's main thread (the main-thread call slot),
 * or at the next sh_mods command when that slot is busy or unavailable.
 * The queue holds every line until it is printed. */
static CRITICAL_SECTION g_mc_out_cs;
static INIT_ONCE g_mc_out_once = INIT_ONCE_STATIC_INIT;
static char **g_mc_out;
static size_t g_mc_out_n, g_mc_out_cap;

static BOOL CALLBACK mc_out_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_mc_out_cs);
    return TRUE;
}

/* Print every queued line. Console thread or main thread only. */
static void mc_outbox_flush(void)
{
    size_t i;
    InitOnceExecuteOnce(&g_mc_out_once, mc_out_init, NULL, NULL);
    EnterCriticalSection(&g_mc_out_cs);
    for (i = 0; i < g_mc_out_n; i++) { sh_printf("%s\n", g_mc_out[i]); free(g_mc_out[i]); }
    g_mc_out_n = 0;
    LeaveCriticalSection(&g_mc_out_cs);
}

static int mc_outbox_flush_main(void *ctx)
{
    (void)ctx;
    mc_outbox_flush();
    return 1;
}

static void mc_async_line(const char *fmt, ...)
{
    char *line;
    va_list ap;
    sh_run_on_main_fn run = NULL;
    int res = 0;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (!line) return;
    backend_log(line);
    InitOnceExecuteOnce(&g_mc_out_once, mc_out_init, NULL, NULL);
    EnterCriticalSection(&g_mc_out_cs);
    if (g_mc_out_n == g_mc_out_cap) {
        size_t cap = g_mc_out_cap ? g_mc_out_cap * 2 : 64;
        char **grown = (char **)realloc(g_mc_out, cap * sizeof *grown);
        if (grown) { g_mc_out = grown; g_mc_out_cap = cap; }
    }
    if (g_mc_out_n < g_mc_out_cap) { g_mc_out[g_mc_out_n++] = line; line = NULL; }
    LeaveCriticalSection(&g_mc_out_cs);
    free(line);   /* only when the queue could not grow: the line is in the backend log */
    /* Printed on the main thread when its call slot takes the request within
     * 1.5 s; otherwise the next sh_mods command prints it. */
    sh_apply_engine_get_run_on_main(&run);
    if (run) run(NULL, mc_outbox_flush_main, NULL, 1500, &res);
}

static double mc_mib(unsigned long long b) { return (double)b / (1024.0 * 1024.0); }

/* One job at a time; its arguments live here until the job ends. */
typedef enum { MC_JOB_PUSH, MC_JOB_RELEASE, MC_JOB_RETRY } mc_job_kind;

typedef struct mc_job {
    mc_job_kind kind;
    char verb[16];
    char mod[MODSYNC_MOD_MAX + 1];   /* push: "<salt>@<build hash>" */
    char *label;                     /* push: the folder under mods\\ (malloc'd) */
    char *ref;                       /* malloc'd */
    unsigned last_quarter;
    ULONGLONG started;
} mc_job;

static mc_job g_mc_job;

/* A progress line at each quarter of a transfer. */
static void mc_job_progress(void *ctx, unsigned long long done, unsigned long long total)
{
    mc_job *j = (mc_job *)ctx;
    unsigned q;
    if (!total) return;
    q = (unsigned)(done * 4 / total);
    if (q > 4) q = 4;
    if (q <= j->last_quarter || q == 4) return;
    j->last_quarter = q;
    mc_async_line("MODS: %s: %.1f of %.1f MiB", j->verb, mc_mib(done), mc_mib(total));
}

static int mc_job_run(void *ctx, const modnet_io *io)
{
    mc_job *j = (mc_job *)ctx;
    modsync_stats st;
    int ok = 0;
    switch (j->kind) {
    case MC_JOB_PUSH: {
        const char *m = j->mod;
        ok = modsync_upload_builds(&m, 1, j->ref, io, &st);
        /* A release of this build raced the claim (409): its archive went,
         * so it is uploaded again once. */
        if (!ok && st.code == MODNET_E_CONFLICT) {
            mc_async_line("MODS: push %s: the store released the build meanwhile; uploading it again", j->label);
            memset(&st, 0, sizeof st);
            ok = modsync_upload_builds(&m, 1, j->ref, io, &st);
        }
        if (ok)
            mc_async_line("MODS: push %s: done in %.0f ms: the archive %s, %llu byte(s) up, %s", j->label, st.ms,
                          st.builds_sent ? "was sent" : "was already stored", st.bytes_up,
                          st.reclaimed ? "its claim renewed" : "claimed");
        else
            mc_async_line("MODS: push %s: FAILED after %.0f ms (%llu byte(s) up): %s%s", j->label, st.ms, st.bytes_up,
                          modsync_cause(&st), st.queued ? "; queued for 'sh_mods retry'" : "");
        break;
    }
    case MC_JOB_RELEASE:
        ok = modsync_release(j->ref, io, &st);
        if (ok) mc_async_line("MODS: release: released in %.0f ms%s%s", st.ms, modsync_cause(&st)[0] ? "; " : "",
                              modsync_cause(&st));
        else mc_async_line("MODS: release: FAILED: %s", modsync_cause(&st));
        break;
    case MC_JOB_RETRY: {
        unsigned done = 0, dropped = 0;
        int left = modsync_retry_pending(io, &done, &dropped, &st);
        ok = left == 0;
        if (left < 0) mc_async_line("MODS: retry: the pending section of mods\\snapmap-plus.json cannot be read");
        else mc_async_line("MODS: retry: %u upload(s) done, %u dropped, %d still queued (%.0f ms)", done, dropped, left,
                           (double)(GetTickCount64() - j->started));
        break;
    }
    }
    return ok;
}

static void mc_job_launch(const mc_job *spec)
{
    char busy[64];
    int started;
    if (modnet_job_busy(busy, sizeof busy)) {
        mc_line("MODS: %s: '%s' is still running; wait for it or run 'sh_mods cancel'", spec->verb, busy);
        return;
    }
    free(g_mc_job.label);
    free(g_mc_job.ref);
    g_mc_job = *spec;
    g_mc_job.label = spec->label ? _strdup(spec->label) : NULL;
    g_mc_job.ref = spec->ref ? _strdup(spec->ref) : NULL;
    g_mc_job.last_quarter = 0;
    g_mc_job.started = GetTickCount64();
    started = modnet_job_start(spec->verb, mc_job_run, NULL, mc_job_progress, &g_mc_job, &g_mc_job);
    if (started == 1) mc_line("MODS: %s: started in the background; the result prints here when it is done", spec->verb);
    else if (started == 0) mc_line("MODS: %s: another job is running", spec->verb);
    else mc_line("MODS: %s: the worker thread could not be started", spec->verb);
}

/* A usable built mod named by its folder under mods\, its salt, or
 * "<salt>@<build hash>": "<salt>@<hash>" in mod, its folder in label. A root
 * folder answers first. */
static int mc_find_build(const char *name, char mod[MODSYNC_MOD_MAX + 1], char **label)
{
    char salt[MODSYNC_SALT_HEX + 1], hash[MODNET_HASH_HEX + 1];
    sh_ms_list l;
    size_t i;
    int found = 0, pass, exact = modsync_parse_mod(name, salt, hash);
    modsync_store_lock();
    if (sh_ms_enumerate(&l)) {
        for (pass = 0; pass < 2 && !found; pass++)
            for (i = 0; i < l.count && !found; i++) {
                const sh_ms_entry *e = &l.items[i];
                if (!sh_ms_entry_usable(e) || (pass == 0) != (e->place[0] == 0)) continue;
                if (exact ? (!strcmp(e->salt, salt) && !strcmp(e->bundle.hash, hash)) : sh_ms_entry_matches(e, name)) {
                    _snprintf_s(mod, MODSYNC_MOD_MAX + 1, _TRUNCATE, "%s@%s", e->salt, e->bundle.hash);
                    found = (*label = _strdup(e->name)) != NULL;
                }
            }
        sh_ms_list_free(&l);
    }
    modsync_store_unlock();
    return found;
}

/* push, release, retry, export, import, cancel. Returns 1 when the verb is
 * one of them. */
static int mc_store_command(idCmdArgs *a, const char *verb)
{
    mc_job spec;
    char *arg;
    int handled = 1;
    memset(&spec, 0, sizeof spec);
    strncpy_s(spec.verb, sizeof spec.verb, verb, _TRUNCATE);
    _strlwr_s(spec.verb, sizeof spec.verb);
    if (!_stricmp(verb, "cancel")) {
        char busy[64];
        if (modnet_job_busy(busy, sizeof busy) && modnet_job_cancel()) mc_line("MODS: cancel: asked '%s' to stop", busy);
        else mc_line("MODS: cancel: nothing is running");
        return 1;
    }
    if (!_stricmp(verb, "push")) {
        const char *name = cmd_argv(a, 2);
        spec.ref = mc_join_args(a, 3);
        if (!name || !name[0] || !spec.ref || !spec.ref[0])
            sh_printf("usage: sh_mods push \"<mod folder>\" \"<ref>\"\n");
        else if (!modnet_valid_ref(spec.ref))
            mc_line("MODS: push: %s", MODNET_REF_RULE);
        else if (!mc_find_build(name, spec.mod, &spec.label))
            mc_line("MODS: push %s: no built mod has that name ('sh_mods list' shows them)", name);
        else {
            spec.kind = MC_JOB_PUSH;
            mc_line("MODS: push mods\\%s under ref '%s'", spec.label, spec.ref);
            mc_job_launch(&spec);
        }
        free(spec.ref);
        free(spec.label);
        return 1;
    }
    if (!_stricmp(verb, "release")) {
        spec.ref = mc_join_args(a, 2);
        if (!spec.ref || !spec.ref[0]) sh_printf("usage: sh_mods release \"<ref>\"\n");
        else {
            spec.kind = MC_JOB_RELEASE;
            mc_job_launch(&spec);
        }
        free(spec.ref);
        return 1;
    }
    if (!_stricmp(verb, "retry")) {
        int n = modsync_pending_count();
        if (n == 0) { mc_line("MODS: retry: no upload is queued"); return 1; }
        spec.kind = MC_JOB_RETRY;
        mc_line("MODS: retry: %d queued upload(s)", n);
        mc_job_launch(&spec);
        return 1;
    }
    if (!_stricmp(verb, "export") || !_stricmp(verb, "import")) {
        arg = mc_join_args(a, 2);
        if (!arg || !arg[0] || !modexport_is_zip_path(arg)) {
            sh_printf("usage: sh_mods %s \"<path.zip>\"\n", spec.verb);
            free(arg);
            return 1;
        }
        if (!_stricmp(verb, "export")) {
            char *why = NULL;
            /* The open map through the rawmap writer, which writes a .zip
             * destination as an export (as 'sh_rawmaps save <path.zip>'). */
            if (sh_editor_frame_request_rawmap_save_to(arg, &why))
                mc_line("MODS: export: writing the open map and its mods to %s; the console says when it is done", arg);
            else
                mc_line("MODS: export: cannot save: %s", why && why[0] ? why : "the editor is not ready");
            free(why);
            free(arg);
            return 1;
        }
        {
            /* Opened as File > Load Rawmap opens it: the import runs off the
             * game's thread and the map opens when it is done. */
            char *why = NULL;
            if (sh_rawmap_open_now(arg, &why)) mc_line("MODS: import: opening %s", arg);
            else mc_line("MODS: import: %s was not opened: %s", arg, why && why[0] ? why : "the editor cannot open it now");
            free(why);
        }
        free(arg);
        return 1;
    }
    handled = 0;
    return handled;
}

/* sh_modui_doctor's lines: the backend log and the console, as every MODS: line. */
static void mc_doctor_line(void *context, const char *line)
{
    (void)context;
    mc_line("%s", line);
}

void h_sh_mods(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
    char *arg = NULL, *folder = NULL;
    if (verb && (!_stricmp(verb, "help") || !strcmp(verb, "?"))) {
        sh_printf("%s\n", SH_MODCMD_HELP);
        sh_printf("%s\n", SH_MODCMD_HELP_TAB);
        sh_printf("%s\n", SH_MODCMD_HELP_AUTHOR);
        sh_printf("%s\n", SH_MODCMD_HELP_STORE);
        return;
    }
    /* Serving verbs (modserve_engine.c): status (also the bare command),
     * rescan, palette rebuild. */
    if (!verb || !verb[0]) verb = "status";
    mc_outbox_flush();
    if (mc_store_command(a, verb)) return;
    /* The rebuild rule first; then the serving index and the refresh. */
    if (!_stricmp(verb, "rescan") || !_stricmp(verb, "refresh")) mc_sync();
    if (sh_modserve_command(a, verb)) return;
    if (!_stricmp(verb, "list")) { mc_list(); return; }
    if (!_stricmp(verb, "map") || !_stricmp(verb, "uninstall") || !_stricmp(verb, "choose")) {
        /* The Mods tab's map details, Remove from map and Uninstall
         * (modui.c): the same dry runs and actions; --yes performs one. */
        const char *argv[8];
        int argc = 0, i, n = cmd_argc(a);
        for (i = 1; i < n && argc < 8; i++) if (cmd_argv(a, i)) argv[argc++] = cmd_argv(a, i);
        if (sh_modui_command(argc, argv, mc_doctor_line, NULL) < 0)
            mc_line("MODS: %s: refused (not on the game's main thread)", verb);
        return;
    }
    if (!_stricmp(verb, "doctor")) {
        /* What the Mods tab does not show (modui.c): every bundle's state, duplicate ids,
         * conflicts, shadows and, with 'verify', a rehash of every build. */
        const char *flag = cmd_argv(a, 2);
        int verify = flag && !_stricmp(flag, "verify");
        if (flag && flag[0] && !verify) { sh_printf("usage: sh_mods doctor [verify]\n"); return; }
        if (sh_modui_doctor(verify, mc_doctor_line, NULL) < 0)
            mc_line("MODS: doctor: refused (not on the game's main thread, or out of memory)");
        return;
    }
    if (!(arg = mc_join_args(a, 2))) { mc_line("MODS: %s: refused: out of memory", verb); return; }
    if (!_stricmp(verb, "new")) {
        /* New mod: a folder with a working example, salted and built at the next rescan. */
        char key[SH_MB_ID_MAX + 1], *cause = NULL;
        if (!arg[0]) sh_printf("usage: sh_mods new \"<name>\"\n");
        else if (!sh_ms_new_mod(arg, key, &folder, &cause))
            mc_line("MODS: new \"%s\": refused: %s", arg, cause ? cause : "out of memory");
        else
            mc_line("MODS: new \"%s\": salt %s, folder %s; put its decls under assets\\generated\\decls\\<type>\\mods\\%s\\ "
                    "(any other file under its engine path, with mods\\%s\\ in it) and rescan", arg, key, folder, key, key);
        free(cause);
    } else if (!_stricmp(verb, "verify")) {
        if (!arg[0]) sh_printf("usage: sh_mods verify \"<mod folder>\"\n");
        else mc_verify(arg);
    } else if (!_stricmp(verb, "copystock")) {
        const char *request = cmd_argv(a, 2);
        char *dest_arg = mc_join_args(a, 3);
        if (!request || !request[0] || !dest_arg || !dest_arg[0])
            sh_printf("usage: sh_mods copystock \"<engine path>\" \"<file>\"\n");
        else if (!(folder = mc_resolve_folder(dest_arg))) mc_line("MODS: copystock: the store folder cannot be resolved");
        else mc_copystock(request, folder);
        free(dest_arg);
    } else if (_stricmp(verb, "build") && _stricmp(verb, "install") && _stricmp(verb, "rebuild")) {
        sh_printf("sh_mods: unknown verb '%s'. %s\n", verb, SH_MODCMD_HELP);
    } else if (!arg[0]) {
        sh_printf("usage: sh_mods %s \"<folder>\"\n", verb);
    } else if (!(folder = mc_resolve_folder(arg))) {
        mc_line("MODS: %s: the store folder cannot be resolved", verb);
    } else {
        /* Every verb writes the store: it waits for a store write, an import or a
         * download in progress. */
        modstore_lock();
        if (!_stricmp(verb, "build")) mc_build(folder);
        else if (!_stricmp(verb, "rebuild")) {
            /* build, then the rescan the tab's Rebuild runs (a folder in the store rescans in mc_build). */
            mc_build(folder);
            if (!mc_in_store(folder)) mc_rescan("rebuild");
        } else mc_install(folder);
        modstore_unlock();
    }
    free(arg);
    free(folder);
}

/* An export's import builds each carried mod on the thread that imports it
 * (the WebView host's, a rawmap load's worker), so its stock check
 * is the installed-archive bridge, which is thread-safe, not the engine
 * provider. */
static int mc_import_stock(void *ctx, const char *request)
{
    (void)ctx;
    return sh_modstock_exists(request);
}

int sh_modcmd_register(void)
{
    const char *root = sh_ms_root();
    modexport_set_stock(mc_import_stock, NULL);
    sh_modserve_set_build_cause(sh_ms_build_cause);
    if (root) mc_line("MODS: sh_mods ready; store %s", root);
    else mc_line("MODS: sh_mods ready; the store folder cannot be resolved");
    return 1;
}
