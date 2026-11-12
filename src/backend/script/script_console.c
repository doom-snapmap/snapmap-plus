/* script_console.c -- see script_console.h. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script_console.h"
#include "script_runtime.h"
#include "script_hooks.h"
#include "script_source.h"
#ifndef SH_SCRIPT_PURE
#include "../commands.h"
#include "../feature_health.h"
#endif

typedef struct {
    sh_script_console_line_fn line;
    void *context;
    const char *want;          /* a mod's name or salt, NULL for all */
    int shown;
} con;

static void say(con *c, const char *fmt, ...)
{
    char *text;
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = _vscprintf(fmt, ap);
    va_end(ap);
    if (n < 0 || !(text = (char *)malloc((size_t)n + 1))) return;
    va_start(ap, fmt);
    _vsnprintf_s(text, (size_t)n + 1, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (c->line) c->line(c->context, text);
    free(text);
}

static const char *plural(int n, const char *one, const char *many)
{
    return n == 1 ? one : many;
}

/* A mod whose script files no longer match its build (edited since the last
 * Refresh) cannot start: one sentence naming the first file and what to do.
 * 1 when said. The mod is rebuilt by 'sh_mods rescan' or the Mods tab's
 * Refresh and by DOOM's start; the next Play then runs the new files. */
static int say_stale(con *c, const char *salt, const char *name)
{
    int count = 0;
    const char *file = sh_script_source_stale(salt, &count);
    char shown[260];
    size_t k;
    if (!file) return 0;
    _snprintf_s(shown, sizeof shown, _TRUNCATE, "scripts\\%s", file);
    for (k = 0; shown[k]; k++) if (shown[k] == '/') shown[k] = '\\';
    if (count > 1)
        say(c, "%s: %s and %d more %s no longer match the mod's last build, so its scripts will not start; run "
               "'sh_mods rescan' (or press Refresh in the Mods tab) to rebuild it, then play the map again.",
            name && name[0] ? name : salt, shown, count - 1, plural(count - 1, "file", "files"));
    else
        say(c, "%s: %s no longer matches the mod's last build, so its scripts will not start; run 'sh_mods rescan' "
               "(or press Refresh in the Mods tab) to rebuild it, then play the map again.",
            name && name[0] ? name : salt, shown);
    return 1;
}

static void status_one(void *context, const char *salt, const char *name)
{
    con *c = (con *)context;
    sh_script_mod_info i;
    (void)name;
    if (!sh_script_mod_info_get(salt, &i)) return;
    c->shown++;
    if (i.waiting && say_stale(c, salt, i.name)) {
        /* said: the files on disk are not the built ones */
    } else if (i.waiting) {
        say(c, "%s: %d script %s ready; %s at the first frame of Play.", i.name, i.files, plural(i.files, "file", "files"),
            plural(i.files, "it runs", "they run"));
    } else if (i.status == SH_SCRIPT_RUNNING) {
        say(c, "%s: running, %d of %d %s loaded, %d %s registered, %d %s driven.", i.name, i.files_run, i.files,
            plural(i.files, "file", "files"), i.handlers, plural(i.handlers, "handler", "handlers"), i.entities,
            plural(i.entities, "entity", "entities"));
    } else if (i.status == SH_SCRIPT_STOPPED) {
        char *line = i.stopped_line ? sh_script_stop_line_dup(salt) : NULL;
        if (line) say(c, "%s", line);
        else say(c, "%s: stopped for this map.", i.name);
        free(line);
    } else if (i.status == SH_SCRIPT_ENDED) {
        say(c, "%s: ended with the map after loading %d of %d %s and registering %d %s.", i.name, i.files_run, i.files,
            plural(i.files, "file", "files"), i.handlers, plural(i.handlers, "handler", "handlers"));
    } else {
        say(c, "%s: its scripts have not run in this map.", i.name);
    }
    sh_script_mod_info_free(&i);
}

typedef struct { con *c; int n; } log_ctx;

static void log_line(void *context, const char *line)
{
    log_ctx *l = (log_ctx *)context;
    l->n++;
    say(l->c, "  %s", line);
}

static int matches(const con *c, const char *salt, const char *name)
{
    return !c->want || !_stricmp(c->want, salt) || (name && !_stricmp(c->want, name));
}

static void log_one(void *context, const char *salt, const char *name)
{
    con *c = (con *)context;
    log_ctx l;
    if (!matches(c, salt, name)) return;
    c->shown++;
    say(c, "Script log of %s:", name && name[0] ? name : "a mod without a name");
    l.c = c;
    l.n = 0;
    sh_script_log_read(salt, log_line, &l);
    if (!l.n) say(c, "  (nothing was written)");
}

static void check_line(void *context, const char *line)
{
    con *c = (con *)context;
    c->shown++;
    say(c, "%s", line);
}

typedef struct { con *c; int stale; } stale_ctx;

static void stale_one(void *context, const char *salt, const char *name)
{
    stale_ctx *s = (stale_ctx *)context;
    sh_script_mod_info i;
    if (!sh_script_mod_info_get(salt, &i)) return;
    if (i.waiting && say_stale(s->c, salt, name && name[0] ? name : i.name)) s->stale++;
    sh_script_mod_info_free(&i);
}

static void reload(con *c)
{
    sh_script_mod_desc *mods = NULL;
    int n, i;
    if (sh_script_hooks_open()) {
        n = sh_script_hooks_reload();
        if (n > 0) {
            stale_ctx s;
            s.c = c;
            s.stale = 0;
            sh_script_mods(stale_one, &s);   /* each mod whose files are not its build's says so */
            if (n > s.stale) {
                n -= s.stale;
                say(c, "Reloaded the scripts of %d %s from their files; they start again at the next frame.", n,
                    plural(n, "mod", "mods"));
            }
        } else say(c, "No scripts were reloaded: this map's mods carry none now.");
        return;
    }
    n = sh_script_source_map_mods(NULL, 0);           /* the count, then the mods */
    if (n > 0) {
        mods = (sh_script_mod_desc *)calloc((size_t)n, sizeof *mods);
        if (!mods) { say(c, "There was not enough memory to check the scripts; close something and retry."); return; }
        n = sh_script_source_map_mods(mods, n);
    }
    if (n < 0) { free(mods); say(c, "The open map's mods could not be read; open the map again and retry."); return; }
    if (!n) { free(mods); say(c, "The open map's mods carry no scripts."); return; }
    for (i = 0; i < n; i++) {
        int before, ok;
        if (say_stale(c, mods[i].salt, mods[i].name)) continue;
        before = c->shown;
        ok = sh_script_check_files(mods[i].salt, mods[i].name, sh_script_source_files(), check_line, c);
        if (ok < 0) say(c, "%s: its scripts could not be read; press Refresh in the Mods tab.", mods[i].name);
        else if (c->shown == before)
            say(c, "%s: %d script %s compiled without errors; %s at the next Play.", mods[i].name, ok,
                plural(ok, "file", "files"), plural(ok, "it runs", "they run"));
    }
    free(mods);
}

void sh_script_console(int argc, const char *const *argv, sh_script_console_line_fn line, void *context)
{
    con c;
    memset(&c, 0, sizeof c);
    c.line = line;
    c.context = context;
    if (argc <= 0 || !argv[0] || !argv[0][0] || !_stricmp(argv[0], "status")) {
        sh_script_mods(status_one, &c);
        if (!c.shown) say(&c, "No mod's scripts have run in this map or the last one.");
        say(&c, "Scripts run during Play, on the host. 'sh_scripts log [mod]' prints a mod's script log; "
                "'sh_scripts reload' runs them again from their files.");
        return;
    }
    if (!_stricmp(argv[0], "log")) {
        c.want = argc > 1 && argv[1] && argv[1][0] ? argv[1] : NULL;
        sh_script_mods(log_one, &c);
        if (!c.shown) say(&c, c.want ? "No mod named \"%s\" has a script log from this map or the last one."
                                     : "No mod has a script log from this map or the last one.", c.want);
        return;
    }
    if (!_stricmp(argv[0], "reload")) {
        reload(&c);
        return;
    }
    say(&c, "sh_scripts does not know \"%s\". Use sh_scripts, sh_scripts log [mod] or sh_scripts reload.", argv[0]);
}

#ifndef SH_SCRIPT_PURE
static void print_line(void *context, const char *line)
{
    (void)context;
    sh_printf("%s\n", line);
}

void h_sh_scripts(struct idCmdArgs *a)
{
    const char *argv[8];
    int argc = cmd_argc(a) - 1, i;
    if (argc < 0) argc = 0;
    if (argc > 8) argc = 8;
    for (i = 0; i < argc; i++) argv[i] = cmd_argv(a, i + 1);
    {
        char off[160];
        if (sh_feature_refusal(SH_FEATURE_MOD_SCRIPTS, off, sizeof off)) { sh_printf("%s\n", off); return; }
    }
    sh_script_console(argc, argv, print_line, NULL);
}
#endif /* SH_SCRIPT_PURE */
