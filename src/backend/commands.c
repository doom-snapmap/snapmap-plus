/* Register console commands and implement dispatch, resource listings, developer
 * utilities, and command exposure. Entity, reflection, and math handlers live
 * in their respective modules; SnapStack dispatch uses the shared command map. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "cmd_lists.h"
#include "text.h"
#include "cvars.h"
#include "overrides.h"
#include "clipboard.h"
#include "typeinfo.h"
#include "entlist_classes.h"
#include "patch.h"
#include "signatures.h"
#include "engine_globals.h"
#include "host_image.h"
#include "rawmap.h"
#include "modexport.h"   /* sh_rawmaps list: .zip exports */
#include "editor_frame.h"
#include "ui_bridge.h"
#include "hook.h"
#include "backend_log.h"
#include "engine_dialog.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "perf.h"
#include "apply_engine.h"
#include "engine_build.h"
#include "grid_room_native.h"
#include "modcmd.h"
#include "vt_harness.h"
#include "prefab_mods.h"
#include "script/script_console.h"

/* Engine call contracts. */

/* AddCommand(self,name,handler,help,argComp,flags). Help is argument 4;
 * argument completion is 5. flags=2 becomes stored flags=6, exposing commands
 * in full/developer tables and exempting them from the developer cheat guard. */
typedef void (*add_command_fn)(void *cmdsys, const char *name, void *handler,
                               const char *help, void *argComp, unsigned int flags);

/* Dispatch takes a pointer to varargs. Preformat text, then supply one %s argument. */
typedef void (*printf_dispatch_fn)(int level, const char *fmt, void *vaptr);

/* Return a typed resource-registry node, or NULL for an unknown class. */
typedef void *(*get_decls_fn)(const char *type_name);



/* Cached state. */

static add_command_fn     g_add_command = NULL;
static int                g_dialogtest_ticket = 0;
static void              *g_cmdsys      = NULL;
static printf_dispatch_fn g_printf      = NULL;
static void              *g_get_decls   = NULL;
static const uint8_t     *g_module_base = NULL;
static volatile LONG      g_installed   = 0;
static volatile LONG      g_register_pending = 0;   /* set by install, cleared by the first main frame */

/* Keep the original getter bytes while devmode is forced off. */
static sh_patch_handle    g_devmode_handle;
static int               g_devmode_patch_complete;

/* Pass the address of one preformatted string pointer as the engine varargs. */
void sh_printf(const char *fmt, ...)
{
    if (!g_printf) return;
    /* The whole text: a short one on the stack, a longer one on the heap. */
    char buf[1024], *held = NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_start(ap, fmt);
        held = sh_vtextf(fmt, ap);
        va_end(ap);
    }
    const char *p = held ? held : buf;
    __try {
        g_printf(1, "%s", &p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {

    }
    free(held);
}

/* Shared guarded access to engine-owned command arguments. */
int cmd_argc(idCmdArgs *a)
{
    __try { return a ? a->argc : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
const char *cmd_argv(idCmdArgs *a, int n)
{
    __try { return (a && n >= 0 && n < a->argc) ? a->argv[n] : NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* Guarded byte reads shared by global decoders and entity handlers. */
int sh_safe_read(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Decode the first RIP-relative MOV/LEA to RAX/RCX within the accessor window.
 * Return the pointer slot, not the object it contains. */
const uint8_t *sh_decode_rip_slot(const uint8_t *accessor_fn)
{
    uint8_t b[B2_RIP_SCAN_WINDOW];
    if (!sh_safe_read(accessor_fn, b, sizeof b)) return NULL;
    for (int i = 0; i + 7 <= B2_RIP_SCAN_WINDOW; i++) {
        /* 48 = REX.W; 8B = MOV r64,r/m64; 8D = LEA; modrm 0x0D = [rip+disp32]->RCX, 0x05 = ->RAX */
        if (b[i] == 0x48 && (b[i + 1] == 0x8B || b[i + 1] == 0x8D) &&
            (b[i + 2] == 0x0D || b[i + 2] == 0x05)) {
            int32_t disp;
            memcpy(&disp, &b[i + 3], 4);
            const uint8_t *rip_next = accessor_fn + i + 7;
            return rip_next + disp;
        }
    }
    return NULL;
}

void *sh_resolve_cmdsys(const sig_result *results, size_t n, const uint8_t *module_base)
{
    void *accessor = (void *)sig_addr_by_name(results, n, "CmdSystemLea");
    const uint8_t *slot = accessor ? sh_decode_rip_slot((const uint8_t *)accessor) : NULL;
    void *obj = NULL;
    (void)module_base;
    if (slot && sh_safe_read(slot, (uint8_t *)&obj, sizeof obj) && obj) return obj;
    backend_log("commands: the console command system was not found");
    return NULL;
}

/* Console handlers run at the engine main-thread command-execution point. */

/* sh_rawmaps -- raw JSON map files from the console, the same actions as the File menu.
 *
 *   sh_rawmaps                  state + both paths (changes nothing)
 *   sh_rawmaps list [folder]    the rawmap files in the default folder, the load path's folder, or one
 *   sh_rawmaps load [path]      open a rawmap in the editor now
 *   sh_rawmaps save [path]      write the OPEN map, optionally to a new destination
 *   sh_rawmaps savepath ...     where saves go
 *   sh_rawmaps default          put both paths back to the default location
 *   sh_rawmaps overwrite on|off whether saving an opened rawmap may replace the map it opened over
 *
 * Paths are UTF-8, as everywhere in the backend; the folder walks below use the wide calls. */

/* The folder holding a FILE path (malloc'd). NULL when there is no separator to split on, which
 * is the default resolver's relative fallback and has no folder to speak of. */
static char *rawmap_dir_of(const char *path)
{
    const char *slash = path ? strrchr(path, '\\') : NULL;
    if (slash == NULL || slash == path) return NULL;
    return sh_textn(path, (size_t)(slash - path));
}

/* One folder's rawmaps: *.zip exports holding one map (nothing is installed by looking), then the
 * *.json files that are maps. A .json NAME proves nothing -- the folder also holds config.json and
 * the like, and prefabs\ holds *.snapmap.json prefabs -- so only the file's own "~type" settles it
 * (sh_rawmap_looks_like_rawmap). `prefix` is printed before each name. Returns how many it printed. */
static int rawmap_list_one_dir(const char *dir, const char *prefix)
{
    static const wchar_t *const patterns[] = { L"*.zip", L"*.json" };
    wchar_t *wdir = sh_rawmap_path_wide(dir), *glob;
    WIN32_FIND_DATAW fd;
    int n = 0, k;

    if (!wdir) return 0;
    for (k = 0; k < 2; k++) {
        HANDLE h;
        glob = sh_wtextf(L"%s\\%s", wdir, patterns[k]);
        h = glob ? FindFirstFileW(glob, &fd) : INVALID_HANDLE_VALUE;
        free(glob);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            char *name, *full;
            int ok;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if ((name = sh_rawmap_path_utf8(fd.cFileName)) == NULL) continue;
            full = sh_textf("%s\\%s", dir, name);
            ok = full && (k == 0 ? modexport_validate_source(full, NULL) : sh_rawmap_looks_like_rawmap(full));
            if (ok) {
                sh_printf("  %s%-38s %llu bytes%s\n", prefix ? prefix : "", name,
                          ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                          k == 0 ? "  (the map and its mods)" : "");
                n++;
            }
            free(full);
            free(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(wdir);
    return n;
}

/* A folder and ONE level of subfolders under it. One level, not a walk: a rawmap library is
 * organised a folder deep, and an unbounded recursion pointed at C:\ by a typo would sit there
 * enumerating the disk while the game waits on the console callback. */
static int rawmap_list_dir_tree(const char *dir)
{
    wchar_t *wdir, *glob;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = rawmap_list_one_dir(dir, NULL);

    if ((wdir = sh_rawmap_path_wide(dir)) == NULL) return n;
    glob = sh_wtextf(L"%s\\*", wdir);
    free(wdir);
    h = glob ? FindFirstFileW(glob, &fd) : INVALID_HANDLE_VALUE;
    free(glob);
    if (h == INVALID_HANDLE_VALUE) return n;
    do {
        char *name, *sub, *prefix;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if ((name = sh_rawmap_path_utf8(fd.cFileName)) == NULL) continue;
        sub = sh_textf("%s\\%s", dir, name);
        prefix = sh_textf("%s\\", name);
        if (sub && prefix) n += rawmap_list_one_dir(sub, prefix);
        free(sub);
        free(prefix);
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void rawmap_list_section(const char *dir, const char *what)
{
    int n;
    sh_printf("rawmap files in %s%s:\n", dir, what ? what : "");
    n = rawmap_list_dir_tree(dir);
    if (n == 0) sh_printf("  (none)\n");
}

/* `arg` = a folder to list, or NULL for the default folder and the load path's folder. */
static void rawmap_print_list(const char *arg)
{
    char *def_path, *cur_path, *def_dir, *cur_dir;

    if (arg != NULL && arg[0] != '\0') {
        wchar_t *w = sh_rawmap_path_wide(arg);
        DWORD attrs = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
        free(w);
        if (attrs == INVALID_FILE_ATTRIBUTES) { sh_printf("No such folder: %s\n", arg); return; }
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            sh_printf("That is a file, not a folder: %s\n", arg);
            return;
        }
        rawmap_list_section(arg, NULL);
        return;
    }

    /* The folder rawmaps default to does not move when the load path points elsewhere; the load
     * path's folder is where the person keeps their files. */
    def_path = sh_rawmap_default_path();
    cur_path = sh_rawmap_load_path();
    def_dir = rawmap_dir_of(def_path);
    cur_dir = rawmap_dir_of(cur_path);
    free(def_path);
    free(cur_path);

    if (!def_dir && !cur_dir) {
        sh_printf("Could not work out the rawmap folder.\n");
    } else {
        if (def_dir) rawmap_list_section(def_dir, "   (the default folder)");
        if (cur_dir && (!def_dir || _stricmp(def_dir, cur_dir) != 0)) {
            sh_printf("\n");
            rawmap_list_section(cur_dir, "   (where your load path points)");
        }
        sh_printf("\n'sh_rawmaps list <folder>' lists any other folder.\n");
    }
    free(def_dir);
    free(cur_dir);
}

static const char *rawmap_or(const char *text, const char *none) { return text && text[0] ? text : none; }

static void rawmap_print_state(void)
{
    char *load_path = sh_rawmap_load_path(), *save_path = sh_rawmap_save_path(), *target, *why = NULL;
    int readable = sh_rawmap_source_ok(&why);

    sh_printf("  load from: %s%s\n", rawmap_or(load_path, "(none)"),
              readable ? "" : "   <-- cannot be opened right now");
    if (!readable && why && why[0]) sh_printf("             %s\n", why);
    /* Say whether this is the default file or one the person chose. */
    target = sh_rawmap_save_target();
    sh_printf("  save to:   %s%s\n", rawmap_or(save_path, "(none)"),
              target && target[0] ? "   (you chose this file)" : "   (the default file)");
    free(target);
    free(why);
    free(load_path);
    free(save_path);
    {
        /* What the last save wrote, and which file to share. */
        sh_rawmap_save_report last;
        sh_rawmap_last_save(&last);
        if (last.sequence) {
            if (last.json)
                sh_printf("  last save: %s (%llu bytes)\n", last.json, last.json_bytes);
            if (last.zip && last.zip_bytes)
                sh_printf("  %s %s (%llu bytes) carries the map and its %u mod%s: share that file\n",
                          last.json ? "beside it:" : "last save:", last.zip, last.zip_bytes, last.mods,
                          last.mods == 1 ? "" : "s");
            else if (last.zip)
                sh_printf("  the map uses %u mod%s, but %s was not written: %s\n", last.mods,
                          last.mods == 1 ? "" : "s", last.zip, rawmap_or(last.zip_error, "unknown cause"));
            else if (last.json)
                sh_printf("  the map uses no mods, so the .json is all anyone needs\n");
        }
        sh_rawmap_save_report_free(&last);
    }
    sh_printf("  'sh_rawmaps help' lists what else it can do.\n");
}

static void rawmap_print_usage(void)
{
    sh_printf("sh_rawmaps -- raw JSON map files.\n");
    sh_printf("  sh_rawmaps | status        show both paths and what the last save wrote\n");
    sh_printf("  sh_rawmaps help | ?        this list\n");
    sh_printf("  sh_rawmaps list [folder]   rawmap files: the default folder, the folder your\n");
    sh_printf("                             load path points at, or one you name\n");
    sh_printf("  sh_rawmaps load <path>     open that file (.json or .zip) in the editor now,\n");
    sh_printf("                             as a new map you name\n");
    sh_printf("  sh_rawmaps load            open the load path again\n");
    sh_printf("  sh_rawmaps save            write the open map to the save path\n");
    sh_printf("  sh_rawmaps save <path>     save there, and keep saving there\n");
    sh_printf("  sh_rawmaps savepath        [default|rawmap|<path>]\n");
    sh_printf("                             where saves go: the default file, the rawmap\n");
    sh_printf("                             you opened, or one you name. Opening another\n");
    sh_printf("                             map goes back to the default file\n");
    sh_printf("  sh_rawmaps default         put both paths back to the default files\n");
    sh_printf("  sh_rawmaps overwrite [on|off]\n");
    sh_printf("                             off by default: saving a rawmap you opened makes a\n");
    sh_printf("                             new map. On lets the save land on the map it\n");
    sh_printf("                             opened over, replacing it, until you close DOOM\n");
    sh_printf("Maps that use mods:\n");
    sh_printf("  Every save of such a map also writes <name>.zip beside the .json: the map\n");
    sh_printf("  and its mods in one file. Share the .zip; the .json alone will not load\n");
    sh_printf("  for anyone who does not have the mods. Opening a .zip installs its mods\n");
    sh_printf("  into mods\\maps\\<the .zip's name>\\, and the map, saved anywhere, finds them there.\n");
    sh_printf("  A rawmap whose mods you do not have is never opened as it is: when the mod\n");
    sh_printf("  store has them the game offers to download them, and otherwise it names\n");
    sh_printf("  them and says to ask the map's author for the .zip.\n");
}

static void h_sh_rawmaps(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
    const char *arg  = cmd_argv(a, 2);

    if (verb == NULL || verb[0] == '\0' || _stricmp(verb, "status") == 0) { rawmap_print_state(); return; }
    if (_stricmp(verb, "help") == 0 || strcmp(verb, "?") == 0) { rawmap_print_usage(); return; }
    if (_stricmp(verb, "list") == 0)    { rawmap_print_list(arg); return; }

    /* Named for the thing a person is choosing to allow, so "on" is the permissive one. Never
     * persisted: it destroys a map, and a destructive setting that outlives the session it was
     * turned on in is one someone forgets about and loses work to. */
    if (_stricmp(verb, "overwrite") == 0) {
        if (arg != NULL && arg[0] != '\0') {
            if (_stricmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
                sh_rawmap_set_branch_tag(0);
                sh_printf("Saving now REPLACES the map you opened, and its contents are gone.\n");
                sh_printf("Open a template, or turn this off, to keep saving as a new map.\n");
                sh_printf("This lasts until you close DOOM.\n");
            }
            else if (_stricmp(arg, "off") == 0 || strcmp(arg, "0") == 0) sh_rawmap_set_branch_tag(1);
            else { sh_printf("sh_rawmaps overwrite: say on or off.\n"); return; }
        }
        sh_printf(sh_rawmap_branch_tag_enabled()
                  ? "Saving a rawmap you opened makes a new map; it never lands on the map it opened over.\n"
                  : "Saving a rawmap you opened OVERWRITES the map it opened over.\n");
        return;
    }

    if (_stricmp(verb, "default") == 0) {
        sh_rawmap_swap_set_source(NULL);
        sh_rawmap_set_save_target(NULL);
        sh_printf("Both paths reset to the default location.\n");
        rawmap_print_state();
        return;
    }

    /* `load <path>` opens that file now; bare `load` opens the load path again. Either discards
     * unsaved editor edits, as the File menu's confirm says: the command was typed on purpose, and
     * it cannot damage a SAVED map -- the rawmap opens as a new map that has to be named. Nothing
     * is kept for a later map: a rawmap that cannot open now is refused now. */
    if (_stricmp(verb, "load") == 0) {
        char *why = NULL, *path = arg && arg[0] ? sh_text(arg) : sh_rawmap_load_path();
        if (sh_rawmap_open_now(arg && arg[0] ? arg : NULL, &why)) {
            sh_printf("Opening %s as a new map.\n", rawmap_or(path, "the rawmap"));
            sh_printf("Unsaved edits in the editor are discarded. Save will ask you to name it.\n");
        } else {
            sh_printf("Cannot open %s\n", rawmap_or(path, "(no load path)"));
            sh_printf("  %s.\n", rawmap_or(why, "it cannot be read"));
        }
        free(why);
        free(path);
        return;
    }

    /* `savepath` is the console's way to say where saves go, the same choice "Save Rawmap As"
     * makes in the File menu. Bare form reports and changes nothing. `rawmap` is a shortcut for "the
     * file I have loaded". Opening a different map clears whatever was set, so a downloaded map is
     * never written over by a choice made while some other map was open. */
    if (_stricmp(verb, "savepath") == 0) {
        char *target, *why = NULL;

        if (arg != NULL && arg[0] != '\0') {
            if (_stricmp(arg, "default") == 0) {
                sh_rawmap_set_save_target(NULL);
            } else if (_stricmp(arg, "rawmap") == 0) {
                char *loaded = sh_rawmap_load_path();
                int set = loaded && loaded[0] && sh_rawmap_set_save_target(loaded);
                free(loaded);
                if (!set) {
                    sh_printf("No rawmap is loaded, so there is nothing to save back over.\n");
                    return;
                }
            } else if (!sh_rawmap_dest_path_is_usable(arg, &why)) {
                sh_printf("Cannot save to %s\n", arg);
                sh_printf("  %s\n", rawmap_or(why, "that is not a usable save path"));
                sh_printf("Use 'default', 'rawmap', or a full path. Nothing was changed.\n");
                free(why);
                return;
            } else if (!sh_rawmap_set_save_target(arg)) {
                sh_printf("That path could not be used as a save path.\n");
                return;
            }
        }

        target = sh_rawmap_save_target();
        if (target && target[0])
            sh_printf("Saves go to %s until you open a different map.\n", target);
        else
            sh_printf("Saves go to the default rawmap.json, so a map you opened is left alone.\n");
        free(target);
        rawmap_print_state();
        return;
    }

    if (_stricmp(verb, "save") == 0) {
        char *why = NULL, *save_path;

        /* Ask before changing anything: a refused save must not move the save path. */
        if (!sh_editor_frame_can_rawmap_save(&why)) {
            sh_printf("Cannot save: %s. Nothing was changed.\n", rawmap_or(why, "open a map in the editor first"));
            free(why);
            return;
        }

        /* The candidate destination is only aimed at once every check has passed. */
        save_path = arg && arg[0] ? sh_text(arg) : sh_rawmap_save_path();
        if (!sh_rawmap_dest_writable_now(save_path, &why)) {
            sh_printf("Cannot save to %s\n", rawmap_or(save_path, "(no save path)"));
            sh_printf("  %s\n", rawmap_or(why, "that file cannot be written"));
            sh_printf("Nothing was changed. The save path is untouched.\n");
            free(why);
            free(save_path);
            return;
        }
        free(why);
        why = NULL;
        free(save_path);

        /* Queued onto the editor frame -- serializing the open map touches engine state. Only ever
         * the OPEN map, never a save read off disk. The outcome is told when the write lands. */
        if (sh_editor_frame_request_rawmap_save_to(arg && arg[0] ? arg : NULL, &why))
            sh_printf("%s\n", rawmap_or(why, "Saving the open map."));
        else
            sh_printf("Cannot save: %s.\n", rawmap_or(why, "open a map in the editor first"));
        free(why);
        return;
    }

    rawmap_print_usage();
}

/* Resource registry: array@node+0x20, count@+0x28, name@decl+0x08.
 * Recheck object layout when porting; reads are guarded and count-bounded. */
#define LISTRES_ARRAY_OFF   0x20    /* decl-manager node -> decl-pointer array */
#define LISTRES_COUNT_OFF   0x28    /* decl-manager node -> decl count (uint) */
#define LISTRES_NAME_OFF    0x08    /* decl object -> name char* (*decl + 8) */
#define LISTRES_COUNT_CAP   (1u << 20)  /* stale-node guard */

static int lr_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int lr_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Read the decl name (*decl + 8), SEH-guarded; returns NULL if either hop is unreadable. */
static const char *lr_decl_name(const void *decl)
{
    __try { return *(const char *const *)((const uint8_t *)decl + LISTRES_NAME_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* Accumulate newline-separated names for clipboard copy. Allocation failure
 * stops collection while console output continues. Caller frees the buffer. */
typedef struct lr_buf {
    char  *data;
    size_t len;
    size_t cap;
    int    failed;
} lr_buf;

static void lr_buf_append(lr_buf *b, const char *s)
{
    if (b->failed || s == NULL) return;
    size_t add = strlen(s) + 1;
    if (b->len + add + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + add + 1) ncap *= 2;
        char *nd = (char *)realloc(b->data, ncap);
        if (!nd) { b->failed = 1; return; }
        b->data = nd;
        b->cap  = ncap;
    }
    memcpy(b->data + b->len, s, add - 1);
    b->len += add - 1;
    b->data[b->len++] = '\n';
    b->data[b->len]   = '\0';
}

#ifdef SH_DIAG
/* Print what the resource layer serves for one engine path (a generated Grid
 * Room variant, a built-in default or the installed original): its length and,
 * when a match string is given, every line of those bytes that contains it.
 * Read-only; it changes no state. */
static void h_sh_resread(idCmdArgs *a)
{
    const char *path = cmd_argv(a, 1), *match = cmd_argv(a, 2);
    unsigned char *body = NULL;
    size_t length = 0, shown = 0;
    if (!path) { sh_printf("usage: sh_resread <engine path> [substring]\n"); return; }
    body = sh_overrides_read_engine_resource(path, &length);
    if (!body) {
        sh_printf("sh_resread: %s -- not readable through the resource layer\n", path);
        return;
    }
    sh_printf("sh_resread: %s -- %zu byte(s) served\n", path, length);
    if (match && *match) {
        size_t begin = 0;
        for (size_t i = 0; i <= length; i++) {
            if (i != length && body[i] != '\n') continue;
            size_t end = i;
            while (end > begin && (body[end - 1] == '\r' || body[end - 1] == ' ' ||
                body[end - 1] == '\t')) end--;
            if (end > begin) {
                char line[512];
                size_t n = end - begin < sizeof(line) - 1 ? end - begin : sizeof(line) - 1;
                memcpy(line, body + begin, n); line[n] = 0;
                if (strstr(line, match) && shown < 16) { sh_printf("  %s\n", line); shown++; }
            }
            begin = i + 1;
        }
        if (!shown) sh_printf("  no line contains '%s'\n", match);
    }
    HeapFree(GetProcessHeap(), 0, body);
}
#endif

/* List resource names with optional substring filtering and clipboard copy. */
static void h_sh_listres(idCmdArgs *a)
{
    const char *type   = cmd_argv(a, 1);
    const char *filter = cmd_argv(a, 2);

    if (type == NULL) {
        sh_printf("usage: sh_listres <resource classname (ex:idMaterial)> [filter]\n");
        return;
    }
    if (!g_get_decls) {
        sh_printf("sh_listres is unavailable on this game build.\n");
        return;
    }

    void *list = ((get_decls_fn)g_get_decls)(type);
    if (list == NULL) {
        sh_printf("sh_listres: no decls of type '%s'.\n", type);
        return;
    }

    void    *array = NULL;
    uint32_t count = 0;
    if (!lr_read_ptr((const uint8_t *)list + LISTRES_ARRAY_OFF, &array) ||
        !lr_read_u32((const uint8_t *)list + LISTRES_COUNT_OFF, &count)) {
        sh_printf("sh_listres: decl list array/count unreadable.\n");
        return;
    }
    if (array == NULL || count == 0) {
        sh_printf("sh_listres: 0 decls of type '%s'.\n", type);
        return;
    }
    if (count > LISTRES_COUNT_CAP) {
        sh_printf("sh_listres: decl count implausible (stale manager node?).\n");
        return;
    }

    int clip = sh_cvar_value_int(B2_CVAR_SH_COPY_RESLIST_TO_CLIPBOARD, 0);
    lr_buf buf = { NULL, 0, 0, 0 };

    uint32_t printed = 0;
    for (uint32_t i = 0; i < count; i++) {
        void *decl = NULL;
        if (!lr_read_ptr((const uint8_t *)array + (size_t)i * 8, &decl)) break;
        if (decl == NULL) continue;

        const char *name = lr_decl_name(decl);
        if (name == NULL) continue;
        if (filter != NULL && strstr(name, filter) == NULL) continue;

        sh_printf("%s\n", name);
        printed++;
        if (clip) lr_buf_append(&buf, name);
    }

    if (clip && buf.data != NULL && buf.len > 0) {
        if (sh_clipboard_set(buf.data))
            sh_printf("sh_listres: copied %u name(s) to the clipboard.\n", printed);
    }
    free(buf.data);
}

/* List live idEntity subclasses, including decl-less types and idTarget_Command.
 * Fall back to the static snapshot when reflection is unavailable. */
static void h_sh_entlist(idCmdArgs *a)
{
    const char *filter = cmd_argv(a, 1);

    const char **names = NULL;
    int printed = 0;
    int k = sh_typeinfo_collect_classnames(&names);
    if (k > 0) {
        for (int i = 0; i < k; i++) {
            const char *name = names[i];
            if (name == NULL || strcmp(name, "idEntity") == 0) continue;
            if (sh_typeinfo_class_derives(name, "idEntity") != 1) continue;
            if (filter != NULL && strstr(name, filter) == NULL) continue;
            sh_printf("%s\n", name);
            printed++;
        }
    } else {
        for (int i = 0; i < B2_ENTLIST_CLASS_COUNT; i++) {
            const char *name = B2_ENTLIST_CLASSES[i];
            if (filter != NULL && strstr(name, filter) == NULL) continue;
            sh_printf("%s\n", name);
            printed++;
        }
    }
    sh_printf("(%d entity classes%s%s)\n", printed,
              filter ? " matching " : "", filter ? filter : "");
    free((void *)names);
}

/* Force the session devmode getter to return 0, retaining its original bytes
 * for restoration. Resolve and verify the signature at command invocation. */
#define DEVMODE_SIG_NAME   "SessionDevModeGetter"

/* Find a database entry and resolve it now. Return 0 for no entry/base;
 * callers separately check out->status before using the result. */
static int resolve_sig_by_name(const char *name, sig_result *out)
{
    if (g_module_base == NULL || name == NULL) return 0;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) != 0) continue;
        sig_resolve_one(g_module_base, &BACKEND_ENGINE_SIGNATURES[i], out);
        return 1;
    }
    return 0;
}


static void h_disable_devmode(idCmdArgs *a)
{
    (void)a;
    if (g_devmode_handle.live) {
        sh_printf(g_devmode_patch_complete ? "devmode already disabled\n" :
                  "devmode restoration incomplete; run sh_reenable_devmode to retry\n");
        return;
    }
    sig_result r;
    if (!resolve_sig_by_name(DEVMODE_SIG_NAME, &r)) {
        sh_printf("sh_disable_devmode: %s not in the signature DB -- cannot patch.\n", DEVMODE_SIG_NAME);
        return;
    }

    /* The getter reads one session byte; its displacement must be this build's
     * dev-mode field, or the matched function is not this getter. */
    if (r.status == SIG_OK) {
        uint32_t field = 0;
        __try { memcpy(&field, (const uint8_t *)r.addr + 3, sizeof field); }
        __except (EXCEPTION_EXECUTE_HANDLER) { field = 0; }
        if (field != SH_SESSION_DEVMODE_FIELD) {
            sh_printf("sh_disable_devmode: %s reads session+0x%x, not the dev-mode field -- patch refused\n",
                      DEVMODE_SIG_NAME, (unsigned)field);
            return;
        }
    }

    /* Replace the verified head with xor eax,eax; ret. */
    const uint8_t expect[3]    = { 0x0F, 0xB6, 0x81 };
    const uint8_t new_bytes[3] = { 0x31, 0xC0, 0xC3 };
    sh_patch_status st = code_patch_sig(&r, expect, new_bytes, 3, &g_devmode_handle);
    g_devmode_patch_complete = st == B2_PATCH_OK;
    if (st == B2_PATCH_OK)
        sh_printf("sh_disable_devmode: %s -- devmode disabled (session getter -> 0)\n",
                  sh_patch_status_str(st));
    else if (g_devmode_handle.live)
        sh_printf("sh_disable_devmode: %s -- restoration incomplete; run sh_reenable_devmode to retry\n",
                  sh_patch_status_str(st));
    else
        sh_printf("sh_disable_devmode: %s -- patch refused, devmode unchanged\n",
                  sh_patch_status_str(st));
}


static void h_reenable_devmode(idCmdArgs *a)
{
    (void)a;
    if (!g_devmode_handle.live) {
        sh_printf("devmode not currently disabled\n");
        return;
    }
    sh_patch_status st = code_unpatch(&g_devmode_handle);
    g_devmode_patch_complete = 0;
    if (st == B2_PATCH_OK)
        sh_printf("sh_reenable_devmode: %s -- devmode re-enabled (getter restored)\n",
                  sh_patch_status_str(st));
    else
        sh_printf("sh_reenable_devmode: %s -- restore failed\n", sh_patch_status_str(st));
}

#ifdef SH_DIAG
/* Log render trace varargs through a replacement no-op sink. Start-only: the
 * detour and file remain active for the process lifetime, with no trampoline call. */
#define RENDERLOG_SIG_NAME   "RenderLogStub"
#define RENDERLOG_STOLEN     14   /* hook.c writes a 14-byte FF25 abs-jmp + requires stolen>=14; 14<=16 room */

static FILE *g_renderlog_fp    = NULL;
static void *g_renderlog_tramp = NULL;

/* MSVC cannot combine varargs setup and SEH in one function. Pass the prepared
 * va_list into this guarded writer instead. */
static void renderlog_write(FILE *fp, const char *fmt, va_list ap)
{
    __try {
        vfprintf(fp, fmt, ap);
        fflush(fp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {

    }
}

/* Engine ABI: (context,channel,format,...). Only format and varargs are consumed. */
static void our_renderlog_hook(void *ctx, void *channel, const char *fmt, ...)
{
    (void)ctx;
    (void)channel;
    if (!g_renderlog_fp || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    renderlog_write(g_renderlog_fp, fmt, ap);
    va_end(ap);
}

/* Open the render trace log and install its sink once. */
static void h_cs_start_render_logging(idCmdArgs *a)
{
    (void)a;
    if (g_renderlog_tramp) {
        if (sh_detour_is_installed(g_renderlog_tramp)) {
            sh_printf("render logging already started\n");
            return;
        }
        if (!sh_uninstall_detour(g_renderlog_tramp)) {
            sh_printf("render logging restoration pending; retry this command.\n");
            return;
        }
        g_renderlog_tramp = NULL;
    }
    if (g_renderlog_fp) {
        fclose(g_renderlog_fp);
        g_renderlog_fp = NULL;
    }
    if (fopen_s(&g_renderlog_fp, "renderlog.txt", "w") != 0 || g_renderlog_fp == NULL) {
        g_renderlog_fp = NULL;
        sh_printf("could not open renderlog.txt\n");
        return;
    }
    sh_printf("Opening renderlog renderlog.txt\n");

    sig_result r;
    if (!resolve_sig_by_name(RENDERLOG_SIG_NAME, &r)) {
        sh_printf("cs_start_render_logging: %s not in the signature DB -- cannot hook.\n", RENDERLOG_SIG_NAME);
        fclose(g_renderlog_fp);
        g_renderlog_fp = NULL;
        return;
    }

    g_renderlog_tramp = sh_prepare_detour_sig(&r, (void *)our_renderlog_hook, RENDERLOG_STOLEN);
    if (g_renderlog_tramp == NULL) {
        sh_printf("cs_start_render_logging: detour install refused/failed (%s status=%d) -- logging off.\n",
                  RENDERLOG_SIG_NAME, (int)r.status);
        fclose(g_renderlog_fp);
        g_renderlog_fp = NULL;
        return;
    }
    if (sh_commit_detour(g_renderlog_tramp) != B2_PATCH_OK) {
        if (sh_uninstall_detour(g_renderlog_tramp)) {
            g_renderlog_tramp = NULL;
            fclose(g_renderlog_fp);
            g_renderlog_fp = NULL;
        }
        sh_printf("cs_start_render_logging: commit failed; retained callbacks require restoration.\n");
        return;
    }
    sh_printf("cs_start_render_logging: render-log hook installed.\n");
}
#endif

/* Model compilation and render-debug commands. Engine calls resolve at use.
 * Debug traps, machine-specific dumps, and unsupported mutators are refused. */

/* Model-builder call contracts. */
typedef void *(*default_idstr_ctor_fn)(void *self);
typedef void *(*md6_ctor_fn)(void *md6);
typedef void  (*md6_setoutput_fn)(void *md6, void *output_idstr);
typedef void  (*md6_build_fn)(void *md6);
typedef void  (*idstr_assign2_fn)(void *dstField, const char *cstr);
typedef void *(*idstr_ctor2_fn)(void *self, const char *cstr);
typedef void  (*idstr_dtor2_fn)(void *self);
typedef void  (*bmodel_builder_fn)(void *out208, const char *input,
                                   const char *output, void *opts);

/* Resolve each engine helper on invocation; report missing signatures or faults. */
static void *eng_default_idstr_ctor(void *self)
{
    sig_result r;
    if (!resolve_sig_by_name("DefaultIdStrCtor", &r) ||
        r.status != SIG_OK || r.addr == 0) return NULL;
    __try { return ((default_idstr_ctor_fn)r.addr)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *eng_idstr_ctor_copy(void *self, const char *cstr)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrCtor", &r) ||
        r.status != SIG_OK || r.addr == 0) return NULL;
    __try { return ((idstr_ctor2_fn)r.addr)(self, cstr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static int eng_idstr_assign(void *dstField, const char *cstr)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrAssign", &r) ||
        r.status != SIG_OK || r.addr == 0) return 0;
    __try { ((idstr_assign2_fn)r.addr)(dstField, cstr); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void eng_idstr_dtor(void *self)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrDtor", &r) ||
        r.status != SIG_OK || r.addr == 0) return;
    __try { ((idstr_dtor2_fn)r.addr)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* Leave failed cleanup to the process lifetime. */ }
}
static int eng_md6_ctor(void *md6)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6Ctor", &r) ||
        r.status != SIG_OK || r.addr == 0) return 0;
    __try { ((md6_ctor_fn)r.addr)(md6); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_md6_setoutput(void *md6, void *output_idstr)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6SetOutput", &r) ||
        r.status != SIG_OK || r.addr == 0) return 0;
    __try { ((md6_setoutput_fn)r.addr)(md6, output_idstr); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_md6_build(void *md6)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6Build", &r) ||
        r.status != SIG_OK || r.addr == 0) return 0;
    __try { ((md6_build_fn)r.addr)(md6); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_bmodel_builder(void *out208, const char *input, const char *output, void *opts)
{
    sig_result r;
    if (!resolve_sig_by_name("BModelBuilder", &r) ||
        r.status != SIG_OK || r.addr == 0) return 0;
    __try { ((bmodel_builder_fn)r.addr)(out208, input, output, opts); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Local engine-object storage. These bounds cover the supported layouts;
 * a different engine build still requires validation. */
#define GEN_IDSTR_BYTES   128     /* Scratch storage for an engine idStr. */
#define GEN_MD6_BYTES     1024    /* the idMd6Builder ctx (ctor inits md6+0x38..+0x110; build writes +0xf8) */
#define GEN_BMODEL_BYTES  256     /* the BModelBuilder out208 result struct (OG memsets/uses 208 = 0xD0 bytes) */
#define GEN_BOPTS_BYTES   0xD0    /* BModel result buffer, filled with 0x01. Despite this name, arg4 is an idStr. */

/* Build MD6 output through constructor, input assignment, output setup, and
 * final compile/release. The final operation owns builder teardown; output
 * and scratch option strings retain their separate lifetimes. */
static void h_sh_genmd6model(idCmdArgs *a)
{
    const char *input  = cmd_argv(a, 1);
    const char *output = cmd_argv(a, 2);
    if (cmd_argc(a) <= 2 || input == NULL || output == NULL) {
        sh_printf("sh_genmd6model <input file> <output file> Compiles a .md6model into a bmd6model\n");
        return;
    }

    /* Initialize scratch before any engine constructor runs. */
    unsigned char opts[GEN_IDSTR_BYTES];   memset(opts,   0, sizeof opts);
    unsigned char md6 [GEN_MD6_BYTES];     memset(md6,    0, sizeof md6);
    unsigned char inS [GEN_IDSTR_BYTES];   memset(inS,    0, sizeof inS);
    unsigned char outS[GEN_IDSTR_BYTES];   memset(outS,   0, sizeof outS);

    if (!eng_default_idstr_ctor(opts)) {
        sh_printf("sh_genmd6model is unavailable on this game build.\n");
        return;
    }
    if (!eng_md6_ctor(md6)) {
        sh_printf("sh_genmd6model is unavailable on this game build.\n");
        eng_idstr_dtor(opts);
        return;
    }

    if (!eng_idstr_assign(inS, input) || !eng_idstr_ctor_copy(outS, output)) {
        sh_printf("sh_genmd6model is unavailable on this game build.\n");
        eng_idstr_dtor(opts);
        return;
    }
    if (!eng_md6_setoutput(md6, outS)) {
        sh_printf("sh_genmd6model: the model builder could not be set up; nothing was written.\n");
        eng_idstr_dtor(outS);
        eng_idstr_dtor(opts);
        return;
    }
    eng_idstr_dtor(outS);

    int built = eng_md6_build(md6);
    eng_idstr_dtor(opts);

    if (built)
        sh_printf("sh_genmd6model: compiled '%s' -> '%s'\n", input, output);
    else
        sh_printf("sh_genmd6model: '%s' could not be compiled; nothing was written.\n", input);
}

/* Generate a BModel using a 0xD0 result buffer and a constructed options idStr. */
static void h_sh_genbmodel(idCmdArgs *a)
{
    const char *input  = cmd_argv(a, 1);
    const char *output = cmd_argv(a, 2);
    if (cmd_argc(a) <= 2 || input == NULL || output == NULL) {
        sh_printf("sh_genbmodel <input file> <output file> Generate a bmodel from a .obj/.ase/.lwo file.\n");
        return;
    }

    /* Engine arg1 is the 0xD0 result initialized to 0x01; arg4 is the idStr.
     * Swapping them breaks the builder ABI. */
    unsigned char out208[GEN_BOPTS_BYTES]; memset(out208, 0x01, sizeof out208);
    unsigned char optsS [GEN_IDSTR_BYTES]; memset(optsS,  0,    sizeof optsS);

    if (!eng_default_idstr_ctor(optsS)) {
        sh_printf("sh_genbmodel is unavailable on this game build.\n");
        return;
    }
    int built = eng_bmodel_builder(out208, input, output, optsS);
    eng_idstr_dtor(optsS);

    if (built)
        sh_printf("sh_genbmodel: generated bmodel '%s' -> '%s'\n", input, output);
    else
        sh_printf("sh_genbmodel: '%s' could not be generated; nothing was written.\n", input);
}

#ifdef SH_DIAG
/* Render inspection uses signed render-world anchors; showcursor writes one
 * signed editor-object field. Other toggles are local diagnostics. Unsupported
 * mutators and the original debugger/machine-specific dump paths are refused. */
#define RW_SLOT_KNOWN_RVA         SH_RVA(0x5C43B30u, 0x45491B0u)  /* Pinned Vulkan fallback slot; the host gate checks filename only. */
/* Recheck vtable/field offsets when porting. Derive model-count/model/name
 * accesses from the dumpmodelinfo handler and the cursor byte from showcursor. */
#define RW_VSLOT_MODEL_COUNT      0x188       /* renderWorld vtbl -> GetActiveRenderModelCount() -> uint (BUILD-SPECIFIC) */
#define RW_VSLOT_GET_MODEL        0x190       /* renderWorld vtbl -> GetRenderModel(idx) -> model* (=400; BUILD-SPECIFIC) */
#define RW_MODEL_NAME_OFF         0x10        /* render model -> name char* (model+0x10) (BUILD-SPECIFIC) */
#define ED_SHOWCURSOR_OFF         0x23624u    /* editor -> showcursor byte (OG writes 0) (BUILD-SPECIFIC) */
#define RW_MODEL_COUNT_CAP        1000000u    /* stale-renderWorld guard on the model count */
#define EDITOR_SINGLETON_RVA      SH_RVA(0x2F6AD18u, 0x2FB2C08u)  /* Pinned Vulkan audit reference; the editor global anchor locates the object. */



/* Resolve renderWorld by accessor, independent global anchor, then a
 * Vulkan-name-gated RVA. Return NULL if no readable non-NULL object is found. */
static void *dr_resolve_renderworld(void)
{
    sig_result r;
    if (resolve_sig_by_name("RenderWorldGetter", &r) &&
        r.status == SIG_OK && r.addr != 0) {
        const uint8_t *slot = sh_decode_rip_slot((const uint8_t *)r.addr);
        if (slot) {
            void *rw = NULL;
            if (sh_safe_read(slot, (uint8_t *)&rw, sizeof rw) && rw) return rw;
        }
    }
    if (g_module_base) {                     /* second portable path: the signed data-global anchor */
        uintptr_t slot = glb_resolve(g_module_base, "render_world_slot", NULL);
        if (slot) {
            void *rw = NULL;
            if (sh_safe_read((const uint8_t *)slot, (uint8_t *)&rw, sizeof rw) && rw) return rw;
        }
    }
    /* Last resort: the pinned Vulkan slot behind the host filename gate. */
    if (g_module_base && sh_host_is_supported_build()) {
        void *rw = NULL;
        if (sh_safe_read(g_module_base + RW_SLOT_KNOWN_RVA, (uint8_t *)&rw, sizeof rw) && rw) return rw;
    }
    return NULL;
}

/* SEH-guarded renderWorld vtable-slot calls (the rw shape is engine-owned; never trust it). */
static unsigned dr_model_count(void *rw)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)rw;
        if (!vtbl) return 0;
        unsigned (*fn)(void *) = *(unsigned (* const *)(void *))(vtbl + RW_VSLOT_MODEL_COUNT);
        return fn ? fn(rw) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void *dr_get_model(void *rw, unsigned idx)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)rw;
        if (!vtbl) return NULL;
        void *(*fn)(void *, unsigned) = *(void *(* const *)(void *, unsigned))(vtbl + RW_VSLOT_GET_MODEL);
        return fn ? fn(rw, idx) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static const char *dr_model_name(void *model)
{
    __try { return *(const char * const *)((const uint8_t *)model + RW_MODEL_NAME_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
/* Clear the cursor flag only after the editor singleton anchor resolves. */
static int dr_showcursor(void)
{
    if (!g_module_base) return 0;
    uintptr_t editor = glb_resolve(g_module_base, "editor_singleton", NULL);
    if (!editor) {
        backend_log("sh_debugrender: showcursor declined -- editor_singleton is unresolved");
        return 0;
    }
    __try {
        *(volatile unsigned char *)(editor + ED_SHOWCURSOR_OFF) = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Query material-registry presence without modifying declarations. */
static void *dr_material_decls(void)
{
    if (!g_get_decls) return NULL;
    __try { return ((get_decls_fn)g_get_decls)("idMaterial"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* Diagnostic toggles are local state and do not change engine rendering flags. */
static int g_dr_fps_update = 0;

static void h_sh_debugrender(idCmdArgs *a)
{
    const char *sub = cmd_argv(a, 1);
    if (sub == NULL) {
        sh_printf("Internal renderer-test mutators -- not for normal use.\n");
        return;
    }

    /* Supported inspection and cursor controls. */
    if (strcmp(sub, "dumprenderinfo") == 0 || strcmp(sub, "dumpmodelinfo") == 0) {
        void *rw = dr_resolve_renderworld();
        if (rw == NULL) { sh_printf("sh_debugrender: renderWorld not available (no live render).\n"); return; }
        unsigned count = dr_model_count(rw);
        if (count > RW_MODEL_COUNT_CAP) { sh_printf("sh_debugrender: rendermodel count implausible (stale).\n"); return; }
        sh_printf("Total active rendermodels: %u\n", count);
        for (unsigned i = 0; i < count; i++) {
            void *m = dr_get_model(rw, i);
            if (m == NULL) continue;
            const char *nm = dr_model_name(m);
            sh_printf("Rendermodel idx %u: %s\n", i, nm ? nm : "(unnamed)");
        }
        return;
    }
    if (strcmp(sub, "showcursor") == 0) {
        if (dr_showcursor()) sh_printf("sh_debugrender: showcursor toggled.\n");
        else                 sh_printf("sh_debugrender: showcursor unavailable (editor not live).\n");
        return;
    }
    if (strcmp(sub, "togglefpsupdate") == 0) {
        g_dr_fps_update = !g_dr_fps_update;
        sh_printf("sh_debugrender: fps update %s.\n", g_dr_fps_update ? "ON" : "OFF");
        return;
    }
    if (strcmp(sub, "showmaterial") == 0 || strcmp(sub, "drawmaterial") == 0 ||
        strcmp(sub, "drawmatarg") == 0) {
        void *decls = dr_material_decls();
        if (decls == NULL) { sh_printf("sh_debugrender: idMaterial decls unavailable.\n"); return; }
        sh_printf("sh_debugrender: idMaterial decls resolved%s.\n",
                  cmd_argv(a, 2) ? " (lookup OK)" : "");
        return;
    }

    sh_printf("sh_debugrender: unknown sub-op '%s'.\n", sub);
}
#endif

/* Dispatch sh subcommands inline on the engine main thread. Backend handlers
 * can serialize, edit, commit, select, and toast at this command-execution point;
 * sending them to the frontend worker would make those engine calls off-thread.
 * argv begins at the subcommand name. Borrowed argument strings remain valid
 * for this callback; missing commands and caught handler faults print errors. */
static void h_sh_dispatch(idCmdArgs *a)
{
    sh_iface *iface = sh_ui_get_iface();
    if (iface == NULL) {
        sh_printf("Snapmap+ is still starting; try again in a moment.\n");
        return;
    }

    const char *sub = cmd_argv(a, 1);
    if (sub == NULL) {
        sh_printf("Dispatches a Snapmap+ command\n");
        return;
    }


    sh_cmd_handler handler = NULL;
    void          *ctx     = NULL;
    if (!sh_iface_lookup_cmd(iface, sub, &handler, &ctx) || handler == NULL) {
        sh_printf("There is no Snapmap+ command '%s'. 'sh_help' lists them.\n", sub);
        return;
    }

    /* Borrow the subcommand argument tail for this synchronous callback. */
    int total = cmd_argc(a);
    int sub_argc = total > 1 ? total - 1 : 0;
    const char *sub_argv[64];
    if (sub_argc > 64) sub_argc = 64;
    for (int i = 0; i < sub_argc; i++) {
        const char *v = cmd_argv(a, i + 1);
        sub_argv[i] = v ? v : "";
    }


    __try {
        handler(ctx, sub_argc, sub_argv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sh_printf("sh %s failed with an engine error and did not finish; check the entities it was working on.\n", sub);
    }
}

#ifdef SH_DIAG
/* Export engine event names, numbers, and argument specs as C defines.
 * Reach evMgr through declMgr vtable +0x90, then count/name/find through
 * +0x28/+0x20/+0x10. Event records hold name@+0, fspec@+0x10, eventnum@+0x34.
 * Dense registry indices equal event numbers. Resolve all format arguments;
 * missing specs become empty strings.
 * Recheck slots against the decl-manager constructor vtables when porting;
 * the singleton accessor itself resolves through typeinfo's signed call site. */
#define SS_EVMGR_ACCESSOR_VSLOT   0x90    /* declMgr vtbl -> evMgr sub-object accessor (BUILD-SPECIFIC) */
#define SS_EV_COUNT_VSLOT         0x28    /* evMgr   vtbl -> event count                (BUILD-SPECIFIC) */
#define SS_EV_GETNAME_VSLOT       0x20    /* evMgr   vtbl -> name-by-index (char*)       (BUILD-SPECIFIC) */
#define SS_EV_FINDBYNAME_VSLOT    0x10    /* evMgr   vtbl -> record-by-name              (BUILD-SPECIFIC) */
#define SS_REC_FSPEC_OFF          0x10    /* eventDef record -> fspec char* (arg-spec)   (BUILD-SPECIFIC) */
#define SS_EV_COUNT_CAP           65536u  /* stale/garbage-evMgr guard (registrar caps the table at 0x1000) */
#define SS_DUMP_CAP               0x40000 /* accumulation buffer (~256 KiB; ~1k events * ~200 B each) */

typedef void *(*ss_evmgr_acc_fn)(void *declmgr);
typedef unsigned (*ss_count_fn)(void *evmgr);
typedef const char *(*ss_getname_fn)(void *evmgr, unsigned i);
typedef void *(*ss_findbyname_fn)(void *evmgr, const char *nm);

/* Guarded event-manager vtable calls return NULL/0 on failure. */
static void *ss_call_evmgr_acc(void *declmgr)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)declmgr;
        if (!vtbl) return NULL;
        ss_evmgr_acc_fn fn = *(ss_evmgr_acc_fn const *)(vtbl + SS_EVMGR_ACCESSOR_VSLOT);
        return fn ? fn(declmgr) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static unsigned ss_call_count(void *evmgr)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)evmgr;
        if (!vtbl) return 0;
        ss_count_fn fn = *(ss_count_fn const *)(vtbl + SS_EV_COUNT_VSLOT);
        return fn ? fn(evmgr) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static const char *ss_call_getname(void *evmgr, unsigned i)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)evmgr;
        if (!vtbl) return NULL;
        ss_getname_fn fn = *(ss_getname_fn const *)(vtbl + SS_EV_GETNAME_VSLOT);
        return fn ? fn(evmgr, i) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *ss_call_findbyname(void *evmgr, const char *nm)
{
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)evmgr;
        if (!vtbl) return NULL;
        ss_findbyname_fn fn = *(ss_findbyname_fn const *)(vtbl + SS_EV_FINDBYNAME_VSLOT);
        return fn ? fn(evmgr, nm) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
/* SEH-guarded read of the fspec char* at rec+0x10 (the ';'-delimited arg-spec). NULL on any fault. */
static const char *ss_read_fspec(void *rec)
{
    __try { return *(const char * const *)((const uint8_t *)rec + SS_REC_FSPEC_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* The same truncating SEH-safe buffer-append sh_typeinfo's dump uses (never overruns). */
static void ss_dump_append(char *buf, size_t cap, size_t *len, const char *s)
{
    if (s == NULL || *len >= cap - 1) return;
    size_t room = cap - 1 - *len;
    size_t add  = strlen(s);
    if (add > room) add = room;
    memcpy(buf + *len, s, add);
    *len += add;
    buf[*len] = '\0';
}

/* Copy event definitions as C defines. */
static void h_sh_superscriptop(idCmdArgs *a)
{
    (void)a;

    void *declmgr = sh_typeinfo_get_declmgr();
    if (declmgr == NULL) {
        sh_printf("sh_superscriptop: declMgr unavailable.\n");
        return;
    }
    void *evmgr = ss_call_evmgr_acc(declmgr);
    if (evmgr == NULL) {
        sh_printf("sh_superscriptop: event manager unavailable.\n");
        return;
    }
    unsigned count = ss_call_count(evmgr);
    if (count == 0) {
        sh_printf("sh_superscriptop: no event definitions.\n");
        return;
    }
    if (count > SS_EV_COUNT_CAP) {
        sh_printf("sh_superscriptop: event count implausible (stale event manager?).\n");
        return;
    }

    static char dump[SS_DUMP_CAP];
    size_t dlen = 0;
    dump[0] = '\0';
    char line[1024];

    /* Describe the emitted name/number/spec pairs. */
    ss_dump_append(dump, sizeof dump, &dlen,
        "// snapmap-plus sh_superscriptop -- engine event definitions\n"
        "// EV_<name> = the event number; FSPEC_<name> = its ';'-delimited arg-spec\n");

    unsigned emitted = 0;
    for (unsigned i = 0; i < count; i++) {
        const char *name = ss_call_getname(evmgr, i);
        if (name == NULL || name[0] == '\0') continue;

        const char *fspec = NULL;
        void *rec = ss_call_findbyname(evmgr, name);
        if (rec != NULL) fspec = ss_read_fspec(rec);
        if (fspec == NULL) fspec = "";


        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "#define EV_%s %u\n#define FSPEC_%s \"%s\"\n", name, i, name, fspec);
        ss_dump_append(dump, sizeof dump, &dlen, line);
        emitted++;
    }

    if (sh_clipboard_set(dump))
        sh_printf("sh_superscriptop: %u event defs copied to clipboard.\n", emitted);
    else
        sh_printf("sh_superscriptop: %u event defs generated (clipboard copy failed).\n", emitted);
}

/* Write engine event definitions as an eventdef_ss_t table in eventdefs.txt.
 * Event numbers come from record+0x34, falling back to the dense index.
 * Derive argument counts from semicolon-delimited fspec; record+0x30 is not
 * populated on the supported build. Return type is unavailable and emitted as 0. */
#define CDE_REC_EVENTNUM_OFF  0x34    /* eventDef record -> eventnum (uint)   (BUILD-SPECIFIC, [12] layout) */
#define CDE_OUT_PATH          "eventdefs.txt"   /* Relative to the DOOM working directory. */

/* Read the record event number, or use the caller's dense-index fallback. */
static unsigned cde_read_eventnum(void *rec, unsigned fallback)
{
    __try { return *(const unsigned *)((const uint8_t *)rec + CDE_REC_EVENTNUM_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}

/* The recorded event numbers are trusted only when they form a permutation of
 * 0..count-1 (the event-def catalog is the same 1611 entries on build 13954591);
 * otherwise the dump uses the dense table index for every row. */
static int cde_eventnums_valid(void *evmgr, unsigned count);

/* Guard file output and report write success. */
static int cde_write_file(FILE *fp, const char *buf)
{
    __try { fputs(buf, fp); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int cde_eventnums_valid(void *evmgr, unsigned count)
{
    static unsigned char seen[SS_EV_COUNT_CAP];
    if (count == 0 || count > SS_EV_COUNT_CAP) return 0;
    memset(seen, 0, sizeof seen);
    for (unsigned i = 0; i < count; i++) {
        const char *name = ss_call_getname(evmgr, i);
        void *rec;
        unsigned n;
        if (name == NULL || name[0] == '\0') continue;
        rec = ss_call_findbyname(evmgr, name);
        if (rec == NULL) return 0;
        n = cde_read_eventnum(rec, count);
        if (n >= count || seen[n]) return 0;
        seen[n] = 1;
    }
    return 1;
}

/* Export the event table to a file. */
static void h_cs_dumpeventdefs(idCmdArgs *a)
{
    (void)a;

    void *declmgr = sh_typeinfo_get_declmgr();
    if (declmgr == NULL) {
        sh_printf("cs_dumpeventdefs: declMgr unavailable.\n");
        return;
    }
    void *evmgr = ss_call_evmgr_acc(declmgr);
    if (evmgr == NULL) {
        sh_printf("cs_dumpeventdefs: event manager unavailable.\n");
        return;
    }
    unsigned count = ss_call_count(evmgr);
    if (count == 0) {
        sh_printf("cs_dumpeventdefs: no event definitions.\n");
        return;
    }
    if (count > SS_EV_COUNT_CAP) {
        sh_printf("cs_dumpeventdefs: event count implausible (stale event manager?).\n");
        return;
    }

    static char dump[SS_DUMP_CAP];
    size_t dlen = 0;
    dump[0] = '\0';
    char line[1024];


    ss_dump_append(dump, sizeof dump, &dlen,
        "struct eventdef_ss_t {const char* m_evname;int m_rettype;const char* m_fspec;"
        "unsigned m_numargs; unsigned m_eventnum;};\n"
        "\tstatic const eventdef_ss_t ALLEVENTS[]={\n");

    unsigned emitted = 0;
    int recorded_numbers = cde_eventnums_valid(evmgr, count);
    if (!recorded_numbers)
        sh_printf("cs_dumpeventdefs: recorded event numbers are not a permutation of 0..%u; "
                  "using table order.\n", count - 1);
    for (unsigned i = 0; i < count; i++) {
        const char *name = ss_call_getname(evmgr, i);
        if (name == NULL || name[0] == '\0') continue;

        const char *fspec   = NULL;
        unsigned    numargs = 0;
        unsigned    eventnum = i;                          /* dense-table fallback if rec unreadable */
        void *rec = ss_call_findbyname(evmgr, name);
        if (rec != NULL) {
            fspec    = ss_read_fspec(rec);
            if (recorded_numbers) eventnum = cde_read_eventnum(rec, i);
        }
        if (fspec == NULL) fspec = "";
        /* Count argument tokens from fspec; the reflected count is not populated. */
        for (const char *cp = fspec; *cp; cp++) if (*cp == ';') numargs++;

        /* Return type is unavailable; retain the table's 0 placeholder. */
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "\t{\"%s\", 0, \"%s\", %u, %u},\n", name, fspec, numargs, eventnum);
        ss_dump_append(dump, sizeof dump, &dlen, line);
        emitted++;
    }
    ss_dump_append(dump, sizeof dump, &dlen, "};\n");

    FILE *fp = NULL;
    if (fopen_s(&fp, CDE_OUT_PATH, "w") != 0 || fp == NULL) {
        sh_printf("cs_dumpeventdefs: could not open %s for writing.\n", CDE_OUT_PATH);
        return;
    }
    int wrote = cde_write_file(fp, dump);
    fclose(fp);

    if (wrote)
        sh_printf("cs_dumpeventdefs: %u event defs -> %s\n", emitted, CDE_OUT_PATH);
    else
        sh_printf("cs_dumpeventdefs: %u event defs generated (file write failed).\n", emitted);
}
#endif

/* Entity handlers share this command ABI. */
void h_sh_dumpdef(idCmdArgs *a);
void h_sh_spawninfo(idCmdArgs *a);
void h_sh_spawn(idCmdArgs *a);
void h_sh_smp(idCmdArgs *a);
void h_sh_event(idCmdArgs *a);
void h_sh_dumpmap(idCmdArgs *a);
/* Local-player runtime cheat toggles. */
void h_noclip(idCmdArgs *a);
void h_infinitehealth(idCmdArgs *a);
void h_noplayerdeath(idCmdArgs *a);
void h_noplayerkill(idCmdArgs *a);
void h_notarget(idCmdArgs *a);

/* Reflection handlers bind through typeinfo's signed dependencies. */
void h_cs_fieldinfo(idCmdArgs *a);
void h_sh_type(idCmdArgs *a);
void h_sh_validclasses(idCmdArgs *a);

#ifdef SH_DIAG
/* Engine-math precision overrides (algo.c). */
void h_cs_dontuse(idCmdArgs *a);
void h_alginfo(idCmdArgs *a);
#endif

/* Palette visibility toggle. */
void h_target_any(idCmdArgs *a);

/* Console command names and their displayed help. */
typedef struct cmd_entry {
    const char *name;
    void       *handler;
    const char *help;
} cmd_entry;

static void h_sh_help(idCmdArgs *a);

#ifdef SH_DIAG
/* Raise a diagnostic dialog. Join remaining arguments to preserve message spaces. */
static void h_sh_dialogtest(idCmdArgs *a)
{
    char text[256];
    int argc = cmd_argc(a);
    /* The id the product's own prompts use (GDM_CONFIRM_VIDEO_CHANGES). The
     * old default 0x6D is GDM_SNAPMAP_EDITOR_RESTART_MAP, a prompt the shell
     * acts on. */
    unsigned gdm_id = 0x29u;
    unsigned button_set = 1u;
    int first = 1, i;
    const char *lead = cmd_argv(a, 1);

    if (!sh_engine_dialog_ready()) {
        sh_printf("sh_dialogtest: the engine dialog surface is not ready.\n");
        return;
    }
    /* Optional numeric prefix is GDM id then button set. Both affect layout,
     * so a button-set value alone does not determine which controls appear. */
    if (lead && lead[0] >= '0' && lead[0] <= '9') {
        gdm_id = (unsigned)strtoul(lead, NULL, 0);
        first = 2;
        {
            const char *second = cmd_argv(a, 2);
            if (second && second[0] >= '0' && second[0] <= '9') {
                button_set = (unsigned)strtoul(second, NULL, 0);
                first = 3;
            }
        }
    }
    text[0] = '\0';
    for (i = first; i < argc; i++) {
        const char *w = cmd_argv(a, i);
        if (!w) continue;
        if (text[0]) strncat_s(text, sizeof text, " ", _TRUNCATE);
        strncat_s(text, sizeof text, w, _TRUNCATE);
    }
    if (!text[0])
        strncpy_s(text, sizeof text,
                  "Snapmap+ dialog probe: which buttons are these, and which one did you press?",
                  _TRUNCATE);

    g_dialogtest_ticket = sh_engine_dialog_ask(gdm_id, button_set, text);
    if (!g_dialogtest_ticket) {
        sh_printf("sh_dialogtest: the dialog would not raise (one may already be up).\n");
        return;
    }
    sh_printf("sh_dialogtest: raised ticket %d, gdm %u, button set %u.\n",
              g_dialogtest_ticket, gdm_id, button_set);
}

/* sh_dialogpoll -- read the answer to the dialog sh_dialogtest raised. */
static void h_sh_dialogpoll(idCmdArgs *a)
{
    int r;
    (void)a;
    if (!g_dialogtest_ticket) {
        sh_printf("sh_dialogpoll: nothing raised by sh_dialogtest.\n");
        return;
    }
    r = sh_engine_dialog_poll(g_dialogtest_ticket);
    sh_printf("sh_dialogpoll: ticket %d -> %s\n", g_dialogtest_ticket,
              r == SH_ENGINE_DIALOG_PENDING  ? "PENDING"  :
              r == SH_ENGINE_DIALOG_ACCEPTED ? "ACCEPTED" :
              r == SH_ENGINE_DIALOG_DECLINED ? "DECLINED" : "LOST");
    if (r != SH_ENGINE_DIALOG_PENDING) g_dialogtest_ticket = 0;
}

/* Inspect queued dialog descriptors; button answers arrive through callbacks. */
static void h_sh_dialogdump(idCmdArgs *a)
{
    (void)a;
    sh_engine_dialog_dump(sh_printf);
}
#endif

/* Report served baked navigation and reasons for falling back to shipped data. */
static void h_sh_navmesh(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
    if (verb && _stricmp(verb, "reads") == 0) {
        unsigned reads = 0, noticed = 0;
        unsigned by_edit = 0, by_undo = 0, by_start = 0;
        double read_ms = 0.0;
        int hooked = 0;
        sh_apply_engine_nav_read_stats(&reads, &noticed, &read_ms,
                                      &by_edit, &by_undo, &by_start, &hooked);
        sh_printf("Navigation read %u time(s) this session, %.1f ms total.\n",
                  reads, read_ms);
        sh_printf("Editor actions that put the view out of date: %u\n", noticed);
        sh_printf("What asked for a look: %u edits, %u undo or redo, %u map openings\n",
                  by_edit, by_undo, by_start);
        sh_printf("Editor actions are %s; the map is read when the View menu asks.\n",
                  hooked ? "watched" : "NOT watched -- the edit hook did not install");
        return;
    }
    if (verb && _stricmp(verb, "detail") == 0) {
        sh_nav_bake_report_detail(sh_printf);
        return;
    }
#ifdef SH_DIAG
    if (verb && _stricmp(verb, "marks") == 0) {
        const char *howmany = cmd_argv(a, 2);
        int n = howmany ? atoi(howmany) : 4;
        int marked = sh_nav_bake_show_marks(n);
        if (n <= 0)
            sh_printf("Navigation refusal marks cleared.\n");
        else if (marked > 0)
            sh_printf("Marked %d volume(s) red, the way a refused bake marks "
                      "the ones it could not place. Run sh_navmesh marks 0 to clear.\n",
                      marked);
        else
            sh_printf("Nothing to mark: no volume in this map is marked for AI "
                      "navigation.\n");
        return;
    }
#endif
    sh_navmesh_report(sh_printf);
    sh_printf("navigation: the last Play read its boxes from %s.\n",
              sh_apply_engine_nav_build_source());
    /* Compose stored-shard and marked-volume reports without coupling the modules. */
    sh_nav_bake_report(sh_printf);
}

/* Where frame time goes inside snapmap-plus. */
static void h_sh_perf(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
    if (verb && _stricmp(verb, "reset") == 0) {
        sh_perf_reset();
        sh_printf("Timings cleared. They start again from now.\n");
        return;
    }
    if (verb && _stricmp(verb, "read") == 0) {
        sh_apply_engine_read_probe(sh_printf);
        return;
    }
    sh_perf_report(sh_printf);
}

/* Resize the selected Grid Room through the inspector's own path (format
 * clamp, boundary fit, native edit, read back). Prints the size and the
 * adapter's sentence for the edit: applied, unchanged, limited or refused. */
static void gridsize_print(const char *line)
{
    sh_printf("%s\n", line);
}

static void h_sh_gridsize(idCmdArgs *a)
{
    int argc = cmd_argc(a), instance = -1, resolved = -1, ok, i;
    float requested[3];
    sh_grid_size size;
    if (argc == 2 && !strcmp(cmd_argv(a, 1) ? cmd_argv(a, 1) : "", "templates")) {
        if (!sh_grid_native_templates(gridsize_print))
            sh_printf("sh_gridsize: module palette is not available\n");
        return;
    }
    if (argc != 1 && argc != 2 && argc != 4 && argc != 5) {
        sh_printf("usage: sh_gridsize [instance] | sh_gridsize <x> <y> <z> [instance]\n");
        return;
    }
    if (argc == 2 || argc == 5) {
        const char *text = cmd_argv(a, argc - 1);
        char *end = NULL;
        long value = text ? strtol(text, &end, 10) : -1;
        if (!text || end == text || *end || value < 0 || value > 4095) {
            sh_printf("sh_gridsize: instance must be a module index\n");
            return;
        }
        instance = (int)value;
    }
    for (i = 0; argc >= 4 && i < 3; ++i) {
        const char *text = cmd_argv(a, i + 1);
        char *end = NULL;
        double value = text ? strtod(text, &end) : 0;
        if (!text || end == text || *end) {
            sh_printf("sh_gridsize: dimensions must be numbers\n");
            return;
        }
        requested[i] = (float)value;
    }
    ok = sh_grid_native_selected(instance, argc >= 4 ? requested : NULL, &size, &resolved);
    if (ok)
        sh_printf("Grid Room %d: %u x %u x %u\n", resolved, size.xyz[0], size.xyz[1], size.xyz[2]);
    if (argc >= 4 || !ok)
        sh_printf("%s\n", sh_grid_native_last_message());
}

#ifdef SH_DIAG
/* Take our resource layer back out, so the engine's own cost can be measured
 * without it. One way: the editor's own resources go with it, so the session is
 * for measuring, not for editing. */
static void h_sh_overrides_off(idCmdArgs *a)
{
    (void)a;
    if (sh_overrides_uninstall())
        sh_printf("Resource layer removed. Timings from here are the engine alone.\n"
                  "Restart DOOM to get it back.\n");
    else
        sh_printf("Resource layer was not installed.\n");
}
#endif

static const cmd_entry CMD_TABLE[] = {
    { "sh_perf",              (void *)h_sh_perf,     "Where frame time goes inside snapmap-plus. 'sh_perf reset' starts the counting again. 'sh_perf read' times reading the open map whole and entity by entity." },
    { "sh_rawmaps",           (void *)h_sh_rawmaps,   "Raw JSON map files: state, paths, load, save. Run with no arguments to see what is set, or 'sh_rawmaps help' (or '?') for every verb." },
    { "sh_type",             (void *)h_sh_type,     "Dumps a types (enum/class) fields to the console and copies the text to your clipboard." },
    { "sh_validclasses",     (void *)h_sh_validclasses,"sh_validclasses <inherit> -- lists the engine-valid classNames for an inherit (the classes deriving from its base type Y; the class-dropdown enumerator)." },
    { "sh_entlist",          (void *)h_sh_entlist,  "Dumps the list of idEntity types in the engine" },
    { "sh_disable_devmode",  (void *)h_disable_devmode,  "disable devmode" },
    { "sh_reenable_devmode", (void *)h_reenable_devmode, "re-enable devmode" },
    { "sh_dumpmap",          (void *)h_sh_dumpmap,  "sh_dumpmap <name> dumps the current mapfile, even the generated snapmap mapfile, to <game dir>\\base\\mapdumps\\<name>.map (never overwrites: repeats get _2, _3, ...)" },
    { "sh_spawn",            (void *)h_sh_spawn,    "sh_spawn <entitydef> <entity name after spawning>" },
    { "sh_smp",              (void *)h_sh_smp,      "sh_smp \"smp:<command> ...\" runs a String Set command on player1 (playtest); sh_smp on|off|status switches String Set commands" },
    { "sh_event",            (void *)h_sh_event,    "sh_event info <name> | list [filter] prints the live engine event registry (number, format, argument types and names)" },
    { "sh_dumpdef",          (void *)h_sh_dumpdef,  "sh_dumpdef <entity name>, dumps the entitydef of an existing ingame entity" },
    { "sh_genbmodel",        (void *)h_sh_genbmodel,"sh_genbmodel <input file> <output file> Generate a bmodel from a .obj/.ase/.lwo file. " },
    { "sh_genmd6model",      (void *)h_sh_genmd6model,"sh_genmd6model <input file> <output file> Compiles a .md6model into a bmd6model" },
    { "sh_target_any",       (void *)h_target_any,  "Toggles targetting for entities. Reveals / re-hides the campaign-only and normally-hidden placeable entity decls in the SnapMap editor palette." },
    { "sh_listres",          (void *)h_sh_listres,  "<resource classname (ex:idMaterial)> <optional: filter> list all resources of a given type" },
    { SH_MODCMD_NAME,        (void *)h_sh_mods,     SH_MODCMD_HELP },
    { SH_PREFAB_CMD_NAME,    (void *)h_sh_prefab,   SH_PREFAB_CMD_HELP },
    { SH_SCRIPT_CMD_NAME,    (void *)h_sh_scripts,  SH_SCRIPT_CMD_HELP },
    { "sh_spawninfo",        (void *)h_sh_spawninfo,"Generate spawnOrientation/spawnPosition from current position in map" },
    { "sh",                  (void *)h_sh_dispatch, "Dispatches a Snapmap+ command" },

    { "noClip",              (void *)h_noclip,         "Toggle noclip (no-collision flight) for the local player." },
    { "infiniteHealth",      (void *)h_infinitehealth, "Toggle infinite health for the local player." },
    { "noPlayerDeath",       (void *)h_noplayerdeath,  "Toggle no-death (the player cannot die) for the local player." },
    { "noPlayerKill",        (void *)h_noplayerkill,   "Toggle no-kill (the player cannot be killed) for the local player." },
    { "noTarget",            (void *)h_notarget,       "Toggle notarget (enemies ignore the local player)." },
    { "sh_navmesh",          (void *)h_sh_navmesh,
      "Reports the baked AI navigation the current map is serving -- which modules and nav classes, or why a bake was refused. 'sh_navmesh reads' counts what the editor has cost it; 'sh_navmesh detail' adds the bake's internal state." },

    { "sh_gridsize",         (void *)h_sh_gridsize,
      "sh_gridsize [instance] prints the selected Grid Room's size; 'sh_gridsize templates' lists the module entity templates of the stock modern room and each resized variant; 'sh_gridsize <x> <y> <z> [instance]' resizes it through the same path as the room's properties and prints the result or the reason it was limited or refused." },

#ifdef SH_DIAG
    /* Developer diagnostics: the diagnostic build only. */
    { "sh_overrides_off",     (void *)h_sh_overrides_off, "Remove our resource layer for the rest of this session, to measure the engine without it. Editor resources go with it; restart to get them back." },
    { "cs_fieldinfo",        (void *)h_cs_fieldinfo,"Diagnostic: print the type fields the reflection layer reads." },
    { "sh_dialogtest",       (void *)h_sh_dialogtest, "[gdmid] [buttonset] [text...] raise the engine's own dialog carrying this text (diagnostic)" },
    { "sh_dialogpoll",       (void *)h_sh_dialogpoll, "read the answer to the dialog sh_dialogtest raised (diagnostic)" },
    { "sh_dialogdump",       (void *)h_sh_dialogdump, "print the engine dialog queue: id, button set and flag bytes (diagnostic)" },
    { "sh_resread",          (void *)h_sh_resread,  "<engine path> [substring] print what the resource layer serves for one resource path (diagnostic)" },
    { SH_VT_CMD_NAME,        (void *)h_sh_vt,       SH_VT_CMD_HELP },
    { "sh_alginfo",          (void *)h_alginfo,     "Diagnostic: print the engine-math precision overrides and their state." },
    { "sh_debugrender",      (void *)h_sh_debugrender,"Internal renderer-test mutators -- not for normal use" },
    { "cs_dontuse",          (void *)h_cs_dontuse,  "Diagnostic: switch the engine-math precision overrides on or off." },
    { "sh_superscriptop",    (void *)h_sh_superscriptop,"Internal/dev: dump engine event definitions for SuperScript" },
    { "cs_dumpeventdefs",    (void *)h_cs_dumpeventdefs,"Internal/dev: dumps all eventdefs to a file (for the wiki)" },
    { "cs_start_render_logging", (void *)h_cs_start_render_logging, "Sets up the renderlog hook " },
#endif

    { "sh_help",             (void *)h_sh_help,        "Lists every Snapmap+ console command and cvar with its description." },
};
#define CMD_COUNT ((int)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])))

/* List registered command help and cvar defaults from their source tables. */
static void h_sh_help(idCmdArgs *a)
{
    (void)a;
    sh_printf("Snapmap+ commands (%d):\n", CMD_COUNT);
    for (int i = 0; i < CMD_COUNT; i++)
        sh_printf("  %-28s %s\n", CMD_TABLE[i].name, CMD_TABLE[i].help);
    int ncv = sh_cvar_table_count();
    sh_printf("Snapmap+ cvars (%d):\n", ncv);
    for (int i = 0; i < ncv; i++) {
        const char *nm = NULL, *df = NULL, *ds = NULL;
        if (sh_cvar_table_row(i, &nm, &df, &ds))
            sh_printf("  %-28s (default %s) %s\n", nm, df, ds);
    }
}

/* Every command answers in and out of developer mode: cmd_lists.h has the
 * two lists, the flags, and why their room is reserved off this thread. */
/* Resolve list growth from AddCommand's LEA RCX,[RSI+8]; CALL rel32.
 * Generic idList instantiations share their bytes, so use this relationship
 * rather than a direct signature or raw RVA. */
typedef sh_idlist_grow_fn idlist_grow_fn;
static idlist_grow_fn g_idlist_grow;

/* Decode the idList-grow callee out of AddCommand's body. Returns NULL if the call site is not found
 * within the scanned window or the decoded target lands outside the DOOM module. */
static idlist_grow_fn sh_decode_idlist_grow(void *add_command, const uint8_t *module_base)
{
    if (!add_command || !module_base) return NULL;

    /* Find LEA RCX,[RSI+8]; E8 rel32 in the first 256 bytes. The developer-list
     * variant uses +0x20 and calls the same growth helper. */
    const uint8_t *p = (const uint8_t *)add_command;
    for (unsigned i = 0; i + 9 <= 256; ++i) {
        uint8_t win[9];
        if (!sh_safe_read(p + i, win, sizeof win)) return NULL;
        if (win[0] != 0x48 || win[1] != 0x8D || win[2] != 0x4E || win[3] != 0x08 || win[4] != 0xE8)
            continue;
        int32_t rel;
        memcpy(&rel, win + 5, sizeof rel);
        const uint8_t *tgt = p + i + 9 + rel;

        /* Validate callable addresses against the mapped image. */
        uint8_t probe;
        if (tgt < module_base || !sh_safe_read(tgt, &probe, 1)) return NULL;
        return (idlist_grow_fn)tgt;
    }
    backend_log("commands: the list-growth call was not found in AddCommand; no developer-list backfill");
    return NULL;
}

/* Preserve all six AddCommand arguments while adding exposure flags. */
typedef void (*add_command6_fn)(void *cmdsys, const char *name, void *handler, const char *help,
                                void *argComp, unsigned int flags);
static add_command6_fn g_addcmd_tramp = NULL;
#define ADDCMD_STOLEN 15   /* 3 whole `mov [rsp+N],reg` prologue movs (5B each) -- >=14, no RIP/rel */

static void hook_add_command(void *cmdsys, const char *name, void *handler, const char *help,
                             void *argComp, unsigned int flags)
{
    if (!g_addcmd_tramp) return;
    /* The exposure flags make the engine append the command to the developer
     * list as well. They are added only while that list has room, so the
     * append never grows the list inside a frame (cmd_lists.h); without room
     * the registration goes through as the engine asked for it. */
    if (sh_cmd_lists_room((const uint8_t *)cmdsys, 1)) flags |= SH_CMD_BOTH_FLAGS;
    g_addcmd_tramp(cmdsys, name, handler, help, argComp, flags);
}

/* Later registrations inherit the exposure flags through the AddCommand
 * detour. The detour is committed with the game's other threads suspended
 * (code_patch), so this is safe from the bootstrap thread. */
static int command_unlock_detour(void *add_command)
{
    if (g_addcmd_tramp) return hook_is_installed((void *)g_addcmd_tramp);
    g_addcmd_tramp = (add_command6_fn)hook_prepare(add_command, (void *)hook_add_command, ADDCMD_STOLEN);
    if (g_addcmd_tramp && hook_commit((void *)g_addcmd_tramp) != B2_PATCH_OK &&
        hook_unpatch((void *)g_addcmd_tramp)) g_addcmd_tramp = NULL;
    return g_addcmd_tramp && hook_is_installed((void *)g_addcmd_tramp);
}

/* flags=2 grants cheat exemption; AddCommand adds developer membership (4).
 * This keeps product commands visible before and after a developer command runs. */
static int register_cmd(const cmd_entry *e)
{
    __try {
        g_add_command(g_cmdsys, e->name, e->handler, e->help, NULL, 2u);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sh_commands_install(void *add_command, void *cmdsys, void *printf_disp, void *get_decls,
                        const uint8_t *module_base)
{
    if (!add_command) { backend_log("commands: off -- AddCommand is unresolved"); return 0; }
    if (!printf_disp) { backend_log("commands: off -- Printf is unresolved"); return 0; }
    if (!cmdsys)      { backend_log("commands: off -- the command system is unresolved"); return 0; }
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;

    g_add_command = (add_command_fn)add_command;
    g_cmdsys      = cmdsys;
    g_printf      = (printf_dispatch_fn)printf_disp;
    g_get_decls   = get_decls;
    g_module_base = module_base;
    g_idlist_grow = sh_decode_idlist_grow(add_command, module_base);
    if (!command_unlock_detour(add_command))
        backend_log("commands: the AddCommand detour did not install; later engine commands keep their flags");
    InterlockedExchange(&g_register_pending, 1);
    return 1;
}

/* The engine's command lists are its main thread's: registration and the
 * developer-list backfill both run here, once, on the first frame. Room for
 * them is reserved first, off this thread, so nothing here grows a list. */
void sh_commands_main_frame(void)
{
    if (InterlockedCompareExchange(&g_register_pending, 0, 1) != 1) return;
    int n = 0;
    uint32_t left = 0, walked;
    char line[240];
    if (!sh_cmd_lists_reserve((uint8_t *)g_cmdsys, g_idlist_grow, (uint32_t)CMD_COUNT)) {
        backend_log("commands: off -- DOOM's command lists have no room for Snapmap+'s commands and could not be "
                    "given more");
        return;
    }
    for (int i = 0; i < CMD_COUNT; i++)
        if (register_cmd(&CMD_TABLE[i])) n++;
    (void)sh_modcmd_register();   /* logs the mods store root for sh_mods */
    walked = sh_cmd_lists_unlock((uint8_t *)g_cmdsys, &left);
    if (left)
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "commands: registered %d/%d console commands; %u engine commands usable outside developer mode, %u of "
            "them not inside it (its list had no room)", n, CMD_COUNT, walked, left);
    else
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "commands: registered %d/%d console commands; %u engine commands usable in and out of developer mode",
            n, CMD_COUNT, walked);
    backend_log(line);
}
