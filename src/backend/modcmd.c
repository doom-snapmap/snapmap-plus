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
#include "modexport.h"
#include "rawmap.h"
#include "editor_frame.h"
#include "apply_engine.h"
#include "modmigrate.h"
#include "modstock.h"
#include "modui.h"

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

/* The serving index reads mod.json at boot and on rescan only, so a bundle
 * written or installed now is invisible until the table is rebuilt. */
static void mc_rescan(const char *why)
{
    int n = sh_modserve_index_rescan();
    if (n < 0) mc_line("MODS: %s: the serving index could not be rebuilt; run 'sh_mods rescan'", why);
    else {
        sh_modserve_refresh rf;
        mc_line("MODS: %s: serving index rebuilt (%d bundle(s) active); new editor rows need 'sh_mods palette rebuild'",
                why, n);
        /* What the build changed under the running game (modserve.h). */
        (void)sh_modserve_refresh_after_rescan(&rf);
        sh_printf("sh_mods: refresh: %s\n", rf.note);
    }
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
    mc_stock_line("build", folder, &b);
    if (!sh_mb_write(&b, folder, cause, sizeof cause))
        mc_line("MODS: build %s: valid but not written: %s", folder, cause);
    else {
        mc_line("MODS: build %s: bundle %s written to mod.json's \"snapmap-plus\" block", folder, b.hash);
        if (mc_in_store(folder)) {
            sh_ms_note_build(folder, &b);
            mc_rescan("build");
        }
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
    char key[SH_MS_NAME_MAX + 1], cause[SH_MB_CAUSE_MAX];
    int ok;
    mc_line("MODS: install %s: building", folder);
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = mc_stock_exists;
    r.stock_context = &stock;
    ok = sh_ms_install_from_folder(folder, &r, key, sizeof key, &st, &b, cause, sizeof cause);
    if (b.id[0]) {
        mc_line("MODS: install %s: %s@%s+%llu from %s, %zu file(s), %llu byte(s)%s", folder, b.id, b.version, b.build,
                b.descriptor, b.file_count, b.total_size, b.errors ? "" : ", mod.json written");
        mc_needs_line("install", &b);
    }
    if (b.id[0]) mc_stock_line("install", folder, &b);
    if (st.copied)
        mc_line("MODS: install %s: %u file(s) copied into the store%s", folder, st.copied,
                st.replaced ? ", replacing the earlier build" : "");
    if (st.in_place && ok) mc_line("MODS: install %s: already in the store; built in place", folder);
    if (!ok) mc_line("MODS: install %s: refused: %s", folder, cause);
    else {
        mc_line("MODS: install %s: installed and verified as mods\\%s (bundle %s)", folder, key, b.hash);
        mc_rescan("install");
    }
    sh_mb_free(&b);
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
    mc_line("MODS: rescan: %u folder(s) checked, %u built again, %u refused (%llu ms)", st.folders, st.rebuilt, st.failed, st.ms);
}

static void mc_list(void)
{
    sh_ms_list list;
    char root[MAX_PATH * 4];
    size_t i;
    if (!sh_ms_root(root, sizeof root)) { mc_line("MODS: list: the store folder cannot be resolved"); return; }
    if (!sh_ms_enumerate(&list)) { mc_line("MODS: list: %s cannot be read", root); return; }
    mc_line("MODS: list: %zu mod folder(s) under %s", list.count, root);
    for (i = 0; i < list.count; i++) {
        const sh_ms_entry *e = &list.items[i];
        const char *state = sh_ms_entry_usable(e) ? "installed" : "needs attention";
        if (e->loaded)
            mc_line("MODS: list: mods\\%s  %s%s  %.16s  %zu file(s)  %llu byte(s)  %s%s%s", e->name, e->key,
                    e->delivered ? " (delivered by a map)" : "", e->bundle.hash, e->bundle.file_count,
                    e->bundle.total_size, state, e->cause[0] ? ": " : "", e->cause);
        else
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
    char cause[SH_MB_CAUSE_MAX];
    int ok;
    mc_line("MODS: verify %s: rehashing", key);
    ok = sh_ms_verify(key, &st, cause, sizeof cause);
    mc_line("MODS: verify %s: %u folder(s), %u file(s) (%u stock pointer(s)), %u MiB checked", key, st.entries,
            st.files, st.pointers, st.bytes_mb);
    if (ok) mc_line("MODS: verify %s: pass", key);
    else mc_line("MODS: verify %s: FAILED: %s", key, cause);
}

/* ------------------------------------------------------- store client ---- */

/* Console lines from the network worker. Each is logged at once; the
 * console prints them on DOOM's main thread (the main-thread call slot),
 * or at the next sh_mods command when that slot is busy or unavailable. */
#define MC_OUTBOX_LINES 64
static CRITICAL_SECTION g_mc_out_cs;
static INIT_ONCE g_mc_out_once = INIT_ONCE_STATIC_INIT;
static char g_mc_out[MC_OUTBOX_LINES][512];
static int  g_mc_out_n, g_mc_out_dropped;

static BOOL CALLBACK mc_out_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_mc_out_cs);
    return TRUE;
}

/* Print every queued line. Console thread or main thread only. */
static void mc_outbox_flush(void)
{
    int i;
    InitOnceExecuteOnce(&g_mc_out_once, mc_out_init, NULL, NULL);
    EnterCriticalSection(&g_mc_out_cs);
    if (g_mc_out_dropped) sh_printf("MODS: (%d earlier line(s) are only in the backend log)\n", g_mc_out_dropped);
    for (i = 0; i < g_mc_out_n; i++) sh_printf("%s\n", g_mc_out[i]);
    g_mc_out_n = 0;
    g_mc_out_dropped = 0;
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
    char line[512];
    va_list ap;
    sh_run_on_main_fn run = NULL;
    int res = 0;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
    InitOnceExecuteOnce(&g_mc_out_once, mc_out_init, NULL, NULL);
    EnterCriticalSection(&g_mc_out_cs);
    if (g_mc_out_n == MC_OUTBOX_LINES) {
        memmove(g_mc_out[0], g_mc_out[1], sizeof g_mc_out[0] * (MC_OUTBOX_LINES - 1));
        g_mc_out_n--;
        g_mc_out_dropped++;
    }
    strcpy_s(g_mc_out[g_mc_out_n++], sizeof g_mc_out[0], line);
    LeaveCriticalSection(&g_mc_out_cs);
    /* Printed on the main thread when its call slot takes the request within
     * 1.5 s; otherwise the next sh_mods command prints it. */
    sh_apply_engine_get_run_on_main(&run);
    if (run) run(NULL, mc_outbox_flush_main, NULL, 1500, &res);
}

static double mc_mib(unsigned long long b) { return (double)b / (1024.0 * 1024.0); }

/* One job at a time; its arguments live here until the job ends. */
typedef enum { MC_JOB_PUSH, MC_JOB_PULL, MC_JOB_RELEASE, MC_JOB_RETRY, MC_JOB_IMPORT } mc_job_kind;

typedef struct mc_job {
    mc_job_kind kind;
    char verb[16];
    char hash[MODNET_HASH_HEX + 1];
    char label[SH_MS_KEY_MAX + 1];
    char ref[4 * MODNET_REF_MAX + 1];
    char path[MAX_PATH * 4];
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
        const char *h = j->hash;
        ok = modsync_upload_bundles(&h, 1, j->ref, io, &st);
        if (ok)
            mc_async_line("MODS: push %s: done in %.0f ms: %u of %u blob(s) were missing and sent, %llu byte(s) up, "
                          "%llu down, bundle %s, claim %s%s", j->label, st.ms, st.blobs_transferred, st.blobs_total,
                          st.bytes_up, st.bytes_down, st.bundles_created ? "uploaded" : "already stored", st.claim,
                          st.reclaimed ? " (re-claimed with the stored secret)" : " (new)");
        else
            mc_async_line("MODS: push %s: FAILED after %.0f ms (%llu byte(s) up): %s%s", j->label, st.ms, st.bytes_up,
                          st.cause, st.queued ? "; queued for 'sh_mods retry'" : "");
        break;
    }
    case MC_JOB_PULL:
        ok = modsync_fetch_bundle(j->hash, io, &st);
        if (ok && st.already_installed)
            mc_async_line("MODS: pull %.16s: already installed; nothing fetched", j->hash);
        else if (ok)
            mc_async_line("MODS: pull %.16s: installed as %s in %.0f ms: %u blob(s) fetched, %llu byte(s) down, %u request(s)",
                          j->hash, st.key, st.ms, st.blobs_transferred, st.bytes_down, st.requests);
        else
            mc_async_line("MODS: pull %.16s: FAILED after %.0f ms (%llu byte(s) down): %s", j->hash, st.ms, st.bytes_down, st.cause);
        break;
    case MC_JOB_RELEASE:
        ok = modsync_release(j->ref, io, &st);
        if (ok) mc_async_line("MODS: release: claim %s released in %.0f ms%s%s", st.claim, st.ms, st.cause[0] ? "; " : "", st.cause);
        else mc_async_line("MODS: release: FAILED: %s", st.cause);
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
    case MC_JOB_IMPORT: {
        modexport_stats es;
        char cause[320], why[192];
        size_t len = 0;
        char *text = modexport_read(j->path, &len, &es, cause, sizeof cause);
        why[0] = 0;
        if (!text) { mc_async_line("MODS: import %s: FAILED: %s", j->path, cause); break; }
        free(text);
        mc_async_line("MODS: import %s: %u mod folder(s) (%u installed, %u already installed), %u file(s) copied out, map %llu byte(s), "
                      "%llu byte(s) read in %.0f ms", j->path, es.bundles, es.bundles_installed, es.bundles_present,
                      es.files, es.map_bytes, es.zip_bytes, es.ms);
        /* Stage it as 'sh_rawmaps load <path>' does: the next map open reads it. */
        if (!sh_rawmap_validate_source(j->path, why, (int)sizeof why) || !sh_rawmap_swap_set_source(j->path)) {
            mc_async_line("MODS: import %s: the mods are installed, but the map could not be staged: %s", j->path,
                          why[0] ? why : "the path is not usable");
            break;
        }
        sh_rawmap_load_arm_once();
        mc_async_line("MODS: import %s: staged; open any map, or run 'sh_rawmaps load', and it opens as a new map", j->path);
        ok = 1;
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
    g_mc_job = *spec;
    g_mc_job.last_quarter = 0;
    g_mc_job.started = GetTickCount64();
    started = modnet_job_start(spec->verb, mc_job_run, NULL, mc_job_progress, &g_mc_job, &g_mc_job);
    if (started == 1) mc_line("MODS: %s: started in the background; the result prints here when it is done", spec->verb);
    else if (started == 0) mc_line("MODS: %s: another job is running", spec->verb);
    else mc_line("MODS: %s: the worker thread could not be started", spec->verb);
}

/* A bundle named by id@version+build (or its 64-hex hash) among the usable
 * installed bundles. */
static int mc_find_bundle(const char *name, char hash[MODNET_HASH_HEX + 1], char *label, size_t label_cap)
{
    sh_ms_list l;
    size_t i;
    int found = 0;
    modsync_store_lock();
    if (sh_ms_enumerate(&l)) {
        for (i = 0; i < l.count && !found; i++) {
            const sh_ms_entry *e = &l.items[i];
            if (!sh_ms_entry_usable(e)) continue;
            if (sh_ms_entry_matches(e, name) || strcmp(e->bundle.hash, name) == 0) {
                strcpy_s(hash, MODNET_HASH_HEX + 1, e->bundle.hash);
                strncpy_s(label, label_cap, e->key, _TRUNCATE);
                found = 1;
            }
        }
        sh_ms_list_free(&l);
    }
    modsync_store_unlock();
    return found;
}

/* push, pull, release, retry, export, import, cancel. Returns 1 when the
 * verb is one of them. */
static int mc_store_command(idCmdArgs *a, const char *verb)
{
    mc_job spec;
    char arg[MAX_PATH * 4];
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
        mc_join_args(a, 3, spec.ref, sizeof spec.ref);
        if (!name || !name[0] || !spec.ref[0]) { sh_printf("usage: sh_mods push \"<id@version+build>\" \"<ref>\"\n"); return 1; }
        if (!modnet_valid_ref(spec.ref)) { mc_line("MODS: push: the ref must be 1-64 characters of text"); return 1; }
        if (!mc_find_bundle(name, spec.hash, spec.label, sizeof spec.label)) {
            mc_line("MODS: push %s: no installed, verified bundle has that name ('sh_mods list' shows them)", name);
            return 1;
        }
        spec.kind = MC_JOB_PUSH;
        mc_line("MODS: push %s (bundle %.16s) under ref '%s'", spec.label, spec.hash, spec.ref);
        mc_job_launch(&spec);
        return 1;
    }
    if (!_stricmp(verb, "pull")) {
        mc_join_args(a, 2, arg, sizeof arg);
        if (!modnet_valid_hash(arg)) { sh_printf("usage: sh_mods pull \"<64-hex bundle hash>\"\n"); return 1; }
        strcpy_s(spec.hash, sizeof spec.hash, arg);
        spec.kind = MC_JOB_PULL;
        mc_job_launch(&spec);
        return 1;
    }
    if (!_stricmp(verb, "release")) {
        mc_join_args(a, 2, spec.ref, sizeof spec.ref);
        if (!spec.ref[0]) { sh_printf("usage: sh_mods release \"<ref>\"\n"); return 1; }
        spec.kind = MC_JOB_RELEASE;
        mc_job_launch(&spec);
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
        mc_join_args(a, 2, arg, sizeof arg);
        if (!arg[0] || !modexport_is_zip_path(arg)) { sh_printf("usage: sh_mods %s \"<path.zip>\"\n", spec.verb); return 1; }
        if (!_stricmp(verb, "export")) {
            char why[192] = "";
            /* The open map through the rawmap writer, which writes a .zip
             * destination as an export (as 'sh_rawmaps save <path.zip>'). */
            if (sh_editor_frame_request_rawmap_save_to(arg, why, (int)sizeof why))
                mc_line("MODS: export: writing the open map and its mods to %s (the result is logged as 'MODS: export')", arg);
            else
                mc_line("MODS: export: cannot save: %s", why[0] ? why : "the editor is not ready");
            return 1;
        }
        strncpy_s(spec.path, sizeof spec.path, arg, _TRUNCATE);
        spec.kind = MC_JOB_IMPORT;
        mc_job_launch(&spec);
        return 1;
    }
    return 0;
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
    char arg[MAX_PATH * 4], folder[MAX_PATH * 4];
    if (verb && (!_stricmp(verb, "help") || !strcmp(verb, "?"))) {
        sh_printf("%s\n", SH_MODCMD_HELP);
        sh_printf("%s\n", SH_MODCMD_HELP_TAB);
        sh_printf("%s\n", SH_MODCMD_HELP_STORE);
        sh_printf("Migration: 'sh_mods migrate' moves the retired overrides folder into the mods store once more (it ran at the first start of this release; the migration section of mods\\snapmap-plus.json records it), 'sh_mods migrate --again' repeats it after you change overrides.\n");
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
    if (!_stricmp(verb, "migrate")) {
        /* The migration of the retired overrides folder (modmigrate.c), on its worker thread. */
        const char *flag = cmd_argv(a, 2);
        int again = flag && !_stricmp(flag, "--again"), r;
        if (flag && flag[0] && !again) { sh_printf("usage: sh_mods migrate [--again]\n"); return; }
        modstore_lock();   /* the pass's scan waits for a store step in progress */
        r = sh_modmigrate_start(again);
        modstore_unlock();
        if (r > 0) mc_line("MODS: migration: started on a background thread%s; its MODS: migration: lines go to the "
                           "backend log, then the serving index is rebuilt ('sh_mods list' shows the result)", again ? " (--again)" : "");
        else if (r < 0) mc_line("MODS: migration: a migration pass is already running or could not start");
        else sh_printf("MODS: migration: nothing to do (no overrides folder, or mods\\snapmap-plus.json says it is done; "
                       "'sh_mods migrate --again' repeats it)\n");
        return;
    }
    if (!_stricmp(verb, "list")) { mc_list(); return; }
    if (!_stricmp(verb, "map") || !_stricmp(verb, "uninstall") || !_stricmp(verb, "removed")) {
        /* The Mods tab's map details, Remove from map, Uninstall and the
         * removed mods (modui.c): the same dry runs and actions; --yes
         * performs one. */
        const char *argv[8];
        int argc = 0, i, n = cmd_argc(a);
        for (i = 1; i < n && argc < 8; i++) if (cmd_argv(a, i)) argv[argc++] = cmd_argv(a, i);
        if (sh_modui_command(argc, argv, mc_doctor_line, NULL) < 0)
            mc_line("MODS: %s: refused (not on the game's main thread)", verb);
        return;
    }
    if (!_stricmp(verb, "doctor")) {
        /* What the Mods tab does not show (modui.c): every bundle's state, duplicate ids,
         * migration, conflicts, shadows and, with 'verify', a rehash of every build. */
        const char *flag = cmd_argv(a, 2);
        int verify = flag && !_stricmp(flag, "verify");
        if (flag && flag[0] && !verify) { sh_printf("usage: sh_mods doctor [verify]\n"); return; }
        if (sh_modui_doctor(verify, mc_doctor_line, NULL) < 0)
            mc_line("MODS: doctor: refused (not on the game's main thread, or out of memory)");
        return;
    }
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
    if (_stricmp(verb, "build") && _stricmp(verb, "install") && _stricmp(verb, "rebuild")) {
        sh_printf("sh_mods: unknown verb '%s'. %s\n", verb, SH_MODCMD_HELP);
        return;
    }
    if (!arg[0]) { sh_printf("usage: sh_mods %s \"<folder>\"\n", verb); return; }
    if (!mc_resolve_folder(arg, folder, sizeof folder)) { mc_line("MODS: %s: the store folder cannot be resolved", verb); return; }
    if (!_stricmp(verb, "build")) mc_build(folder);
    else if (!_stricmp(verb, "rebuild")) {
        /* build, then the rescan the tab's Rebuild runs (a folder in the store rescans in mc_build). */
        mc_build(folder);
        if (!mc_in_store(folder)) mc_rescan("rebuild");
    } else {
        /* Waits for a migration folder or a download in progress. */
        modstore_lock();
        mc_install(folder);
        modstore_unlock();
    }
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
        if (mc_in_store(folder)) sh_ms_note_build(folder, &b);   /* built from its current tree; an earlier build becomes an alias */
        mc_line("MODS: build %s: %s@%s+%llu, %zu file(s), %llu byte(s); bundle %s written", folder, b.id,
                b.version, b.build, b.file_count, b.total_size, b.hash);
        mc_needs_line("build", &b);
        mc_stock_line("build", folder, &b);
    }
    if (stock.state < 0) mc_line("MODS: build %s: the game's resource reader is unavailable; stock names were not checked", folder);
    sh_mb_free(&b);
    sh_ms_list_free(&list);
    return ok;
}

/* An export's import builds each carried mod on the thread that imports it
 * (the WebView host's, a rawmap load's, the job worker's), so its stock check
 * is the installed-archive bridge, which is thread-safe, not the engine
 * provider. */
static int mc_import_stock(void *ctx, const char *request)
{
    (void)ctx;
    return sh_modstock_exists(request);
}

int sh_modcmd_register(void)
{
    char root[MAX_PATH * 4];
    modexport_set_stock(mc_import_stock, NULL);
    if (sh_ms_root(root, sizeof root)) mc_line("MODS: sh_mods ready; store %s", root);
    else mc_line("MODS: sh_mods ready; the store folder cannot be resolved");
    return 1;
}
