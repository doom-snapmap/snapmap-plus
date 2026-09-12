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
#include "cvars.h"
#include "user_overrides.h"
#include "clipboard.h"
#include "typeinfo.h"
#include "entlist_classes.h"
#include "patch.h"
#include "signatures.h"
#include "engine_globals.h"
#include "host_image.h"
#include "rawmap.h"
#include "editor_frame.h"
#include "ui_bridge.h"
#include "hook.h"
#include "backend_log.h"
#include "engine_dialog.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "perf.h"
#include "apply_engine.h"

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

/* Keep the original getter bytes while devmode is forced off. */
static sh_patch_handle    g_devmode_handle;
static int               g_devmode_patch_complete;

/* Pass the address of one preformatted string pointer as the engine varargs. */
void sh_printf(const char *fmt, ...)
{
    if (!g_printf) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    const char *p = buf;
    __try {
        g_printf(1, "%s", &p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {

    }
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

/* Decode cmdSystem from CmdSystemLea, then an independent global anchor.
 * The last-resort pinned RVA uses the Vulkan filename gate, not a build hash. */
#define CMDSYS_KNOWN_RVA   0x55b7280u   /* Pinned Vulkan audit/fallback slot, gated by host filename. */

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
    if (accessor) {
        const uint8_t *slot = sh_decode_rip_slot((const uint8_t *)accessor);
        if (slot) {
            void *obj = NULL;
            if (sh_safe_read(slot, (uint8_t *)&obj, sizeof obj) && obj) {
                char line[128];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: cmdSystem decoded slot=%p -> obj=%p (portable)", (void *)slot, obj);
                backend_log(line);
                return obj;
            }
        }
        backend_log("B2: cmdSystem portable decode failed -- trying the signed data-global anchor");
    }
    /* Independent anchor survives a detour on the CmdSystemLea prologue. */
    if (module_base) {
        glb_status gst = GLB_UNKNOWN_NAME;
        uintptr_t decoded = glb_resolve(module_base, "cmd_system_slot", &gst);
        if (decoded) {
            void *obj = NULL;
            if (sh_safe_read((const uint8_t *)decoded, (uint8_t *)&obj, sizeof obj) && obj) {
                char line[128];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: cmdSystem glb slot=%p -> obj=%p (portable)", (void *)decoded, obj);
                backend_log(line);
                return obj;
            }
        } else {
            char line[128];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: cmdSystem glb anchor unresolved (status=%d)", (int)gst);
            backend_log(line);
        }
    }
    /* Last resort: pinned Vulkan RVA behind the host filename gate. */
    if (module_base && sh_host_is_pinned_rva_build()) {
        const uint8_t *slot = module_base + CMDSYS_KNOWN_RVA;
        void *obj = NULL;
        if (sh_safe_read(slot, (uint8_t *)&obj, sizeof obj) && obj) {
            char line[128];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: cmdSystem pinned-build fallback *(base+0x55b7280)=%p", obj);
            backend_log(line);
            return obj;
        }
    }
    backend_log("B2: cmdSystem UNRESOLVED -- commands cannot fire");
    return NULL;
}

/* Console handlers run at the engine main-thread command-execution point. */

/* Arm rawmap load swapping. */
static void h_rawmaps_on(idCmdArgs *a)
{
    char load_path[MAX_PATH] = "", save_path[MAX_PATH] = "", why[192] = "";
    int  readable;

    (void)a;
    sh_rawmap_swap_arm(1);
    sh_printf("Enabling raw snapmap save/load.\n");

    /* SAY WHAT WAS JUST ARMED. This switch makes EVERY subsequent map load use the staged file and
     * every save mirror to the destination, and both were set by earlier clicks the person may not
     * remember -- the File menu prints them, the console did not. Arming without naming the files is
     * asking someone to accept a consequence they cannot see. */
    sh_rawmap_get_paths(load_path, (int)sizeof load_path, save_path, (int)sizeof save_path);

    /* LEGACY NAME. Kept because it is what the published guide teaches -- removing it would break
     * written instructions people have already followed. Labelled rather than quietly maintained,
     * and deliberately NOT given behaviour of its own: making it force the default paths was
     * considered and rejected, because two near-identical command names differing invisibly is a
     * worse trap than not knowing what is armed -- and the latter is fixed by printing the paths. */
    if (!sh_rawmap_paths_are_default()) {
        char dflt[MAX_PATH] = "";
        sh_rawmap_get_default_paths(dflt, (int)sizeof dflt, NULL, 0);
        sh_printf("Note: these are not the default files. Older guides describe %s\n", dflt);
        sh_printf("      'sh_rawmaps default' puts both paths back.\n");
    }
    readable = sh_rawmap_source_ok(why, (int)sizeof why);

    sh_printf("  load from: %s%s\n", load_path[0] ? load_path : "(none)",
              readable ? "" : "   <-- cannot be read right now");
    if (!readable && why[0]) sh_printf("             %s\n", why);
    {
        /* Say whether this is the default file or one the person chose. The path alone
         * cannot tell them apart, and they answer different questions about the next save. */
        char target[MAX_PATH] = "";
        sh_rawmap_get_save_target(target, (int)sizeof target);
        sh_printf("  save to:   %s%s\n", save_path[0] ? save_path : "(none)",
                  target[0] ? "   (you chose this file)" : "   (the default file)");
    }
    sh_printf("Every map you open now loads that file, and every save is mirrored to that one.\n");
    sh_printf("(legacy name -- 'sh_rawmaps' shows and changes everything, including both paths.)\n");
}
/* Disarm rawmap load swapping. */
static void h_rawmaps_off(idCmdArgs *a)
{
    (void)a;
    sh_rawmap_swap_arm(0);
    sh_printf("Disabling raw snapmap save/load.\n");
    /* Worth saying, because "off" reads like the feature is gone: the File menu's own actions scope
     * themselves to one operation and keep working with the gate down. Off means "stop applying to
     * everything", not "stop working". */
    sh_printf("The File menu, 'sh_rawmaps save' and 'sh_rawmaps load' still work.\n");
}
/* [2b] sh_rawmaps -- one command that can SAY what it is about to do.
 *
 * sh_rawmaps_on/off arm a switch whose effect depends on state the person cannot see: once on, every
 * map they open is substituted from a file some earlier click staged, and every save is mirrored to
 * a destination set the same way. The File menu prints both paths; the console did not, so arming
 * there meant accepting a consequence you could not read first.
 *
 * The fix is not only to print on arming (that is done too) but to make the console able to ANSWER
 * THE QUESTION FIRST -- bare `sh_rawmaps` shows the state and both paths and changes nothing -- and
 * to make choosing and arming one action, so there is no remembered state to be surprised by.
 *
 *   sh_rawmaps                  state + both paths (changes nothing)
 *   sh_rawmaps list             the rawmap files sitting in the default folder
 *   sh_rawmaps on | off         the shared gate, exactly as sh_rawmaps_on/off
 *   sh_rawmaps load <path>      stage a file for the next map load
 *   sh_rawmaps save [path]      write the OPEN map, optionally to a new destination
 *   sh_rawmaps default          put both paths back to the default location
 *
 * sh_rawmaps_on / sh_rawmaps_off stay as they are. They are the original SnapHak names, they are in
 * circulation, and retiring them buys nothing -- the same reasoning that made the one-shot arms
 * additive rather than a redefinition of the gate. */

/* The folder the default paths live in, derived from the default itself rather than rebuilt, so it
 * cannot drift away from where the files actually are. */
/* Split a FILE path into the folder that holds it. 0 when there is no separator to split on,
 * which is the default resolver's relative fallback and has no folder to speak of. */
static int rawmap_dir_of(const char *path, char *out, size_t cap)
{
    char *slash;
    if (path == NULL || path[0] == '\0') return 0;
    strncpy_s(out, cap, path, _TRUNCATE);
    slash = strrchr(out, '\\');
    if (slash == NULL) return 0;
    *slash = '\0';
    return (out[0] != '\0') ? 1 : 0;
}

static int rawmap_default_dir(char *out, size_t cap)
{
    char probe[MAX_PATH] = "";
    /* The DEFAULT path deliberately, not the effective one: this is "the folder rawmaps ship in",
     * which must not move when someone points the load path at a file somewhere else. Where they
     * ACTUALLY keep them is a separate question, and rawmap_print_list answers both. */
    sh_rawmap_get_default_paths(probe, (int)sizeof probe, NULL, 0);
    return rawmap_dir_of(probe, out, cap);
}

/* The folder the CURRENT load path lives in. This is the one that matters in practice: a listing
 * whose only job is "what could I load" is useless if it cannot see where the person keeps their
 * files, and loading one rawmap is all it takes to teach it. */
static int rawmap_current_load_dir(char *out, size_t cap)
{
    char probe[MAX_PATH] = "";
    sh_rawmap_get_paths(probe, (int)sizeof probe, NULL, 0);
    return rawmap_dir_of(probe, out, cap);
}

/* One folder's *.json files. `prefix` is printed before each name, so a subfolder pass can show
 * "doom\mymap.json" without a second column. Returns how many it printed. */
static int rawmap_list_one_dir(const char *dir, const char *prefix)
{
    char glob[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;

    _snprintf_s(glob, sizeof glob, _TRUNCATE, "%s\\*.json", dir);
    h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char full[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        /* A .json NAME proves nothing. This folder also holds config.json, install.json and
         * pinned.json, and prefabs\ is full of *.snapmap.json files that are prefabs, not maps.
         * Only the file's own "~type" settles it -- see sh_rawmap_looks_like_rawmap. */
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (!sh_rawmap_looks_like_rawmap(full)) continue;
        sh_printf("  %s%-38s %llu bytes\n", prefix ? prefix : "", fd.cFileName,
                  ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* A folder and ONE level of subfolders under it. One level, not a walk: a rawmap library is
 * organised a folder deep ("rawmaps\doom\"), and an unbounded recursion pointed at C:\ by a typo
 * would sit there enumerating the disk while the game waits on the console callback. */
static int rawmap_list_dir_tree(const char *dir)
{
    char glob[MAX_PATH], sub[MAX_PATH], prefix[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n;

    n = rawmap_list_one_dir(dir, NULL);

    _snprintf_s(glob, sizeof glob, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(glob, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "%s\\", fd.cFileName);
            n += rawmap_list_one_dir(sub, prefix);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return n;
}

static void rawmap_list_section(const char *dir, const char *what)
{
    int n;
    sh_printf("rawmap files in %s%s:\n", dir, what ? what : "");
    n = rawmap_list_dir_tree(dir);
    if (n == 0) sh_printf("  (none)\n");
}

/* `arg` = a folder to list, or NULL for the two folders that matter: where rawmaps default to, and
 * where the current load path points. Listing only the default was the first version, and it showed
 * an empty folder to anyone who keeps their rawmaps somewhere of their own -- which reads as a
 * broken command rather than as a question about the folder. */
static void rawmap_print_list(const char *arg)
{
    char def_dir[MAX_PATH] = "", cur_dir[MAX_PATH] = "";
    int have_def, have_cur;
    DWORD attrs;

    if (arg != NULL && arg[0] != '\0') {
        attrs = GetFileAttributesA(arg);
        if (attrs == INVALID_FILE_ATTRIBUTES) { sh_printf("No such folder: %s\n", arg); return; }
        /* Checked as a DIRECTORY, not merely as existing. A file path here would glob
         * "mymap.json\\*.json", match nothing, and report an empty folder -- which tells someone
         * who mistyped a folder for a file that their rawmaps are missing. */
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            sh_printf("That is a file, not a folder: %s\n", arg);
            return;
        }
        rawmap_list_section(arg, NULL);
        return;
    }

    have_def = rawmap_default_dir(def_dir, sizeof def_dir);
    have_cur = rawmap_current_load_dir(cur_dir, sizeof cur_dir);

    if (!have_def && !have_cur) { sh_printf("Could not work out the rawmap folder.\n"); return; }

    if (have_def) rawmap_list_section(def_dir, "   (the default folder)");

    /* Only when it is somewhere else -- the common case is that both are the default, and printing
     * one folder twice would read as two folders with the same contents. */
    if (have_cur && (!have_def || _stricmp(def_dir, cur_dir) != 0)) {
        sh_printf("\n");
        rawmap_list_section(cur_dir, "   (where your load path points)");
    }

    sh_printf("\n'sh_rawmaps list <folder>' lists any other folder.\n");
}

static void rawmap_print_state(void)
{
    char load_path[MAX_PATH] = "", save_path[MAX_PATH] = "", why[192] = "";
    int readable;

    sh_rawmap_get_paths(load_path, (int)sizeof load_path, save_path, (int)sizeof save_path);
    readable = sh_rawmap_source_ok(why, (int)sizeof why);

    /* THE GATE IS ONLY WORTH A LINE WHEN IT IS ON.
     *
     * This started as "raw snapmap save/load is OFF (File menu actions still work)", which reads as
     * "the feature is off" -- false, since nothing here needs the gate. The fix for that was two
     * lines explaining the off state, which was worse: four lines of state where two were wanted,
     * with the explanation on top of the paths a person ran the command to see.
     *
     * Off is the normal setting and changes nothing, so it gets no line at all. On DOES change
     * everything a person does next without being asked for it again, so it gets one. */
    if (sh_rawmap_swap_is_armed())
        sh_printf("  GATE ON - every map you open and save goes through a rawmap.\n");
    sh_printf("  load from: %s%s\n", load_path[0] ? load_path : "(none)",
              readable ? "" : "   <-- cannot be read right now");
    if (!readable && why[0]) sh_printf("             %s\n", why);
    {
        /* Say whether this is the default file or one the person chose. The path alone
         * cannot tell them apart, and they answer different questions about the next save. */
        char target[MAX_PATH] = "";
        sh_rawmap_get_save_target(target, (int)sizeof target);
        sh_printf("  save to:   %s%s\n", save_path[0] ? save_path : "(none)",
                  target[0] ? "   (you chose this file)" : "   (the default file)");
    }
    if (sh_rawmap_load_oneshot_pending())
        sh_printf("  a rawmap is staged for the NEXT map you open.\n");
    if (sh_rawmap_save_oneshot_pending())
        sh_printf("  waiting for your next save in DOOM to write the rawmap.\n");
    sh_printf("  'sh_rawmaps help' lists what else it can do.\n");
}

static void rawmap_print_usage(void)
{
    sh_printf("sh_rawmaps -- raw JSON map files.\n");
    sh_printf("  sh_rawmaps                 show the state and both paths\n");
    sh_printf("  sh_rawmaps help | ?        this list\n");
    sh_printf("  sh_rawmaps list [folder]   rawmap files: the default folder, the folder your\n");
    sh_printf("                             load path points at, or one you name\n");
    sh_printf("  sh_rawmaps on | off        optional: apply rawmaps to EVERY map load and save\n");
    sh_printf("  sh_rawmaps load <path>     point the load path at a file and stage it\n");
    sh_printf("  sh_rawmaps load            open the load path as a new map -- or stage it,\n");
    sh_printf("                             if the editor is not up yet, so the next map\n");
    sh_printf("                             you open becomes it\n");
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
}

static void h_sh_rawmaps(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
    const char *arg  = cmd_argv(a, 2);

    if (verb == NULL || verb[0] == '\0') { rawmap_print_state(); return; }

    /* An unknown verb already falls through to the usage text at the end. Naming
     * these two makes it something a person can ask for rather than stumble on. */
    if (_stricmp(verb, "help") == 0 || strcmp(verb, "?") == 0) { rawmap_print_usage(); return; }

    if (_stricmp(verb, "list") == 0)    { rawmap_print_list(arg); return; }

    if (_stricmp(verb, "on") == 0)      { sh_rawmap_swap_arm(1); rawmap_print_state(); return; }
    if (_stricmp(verb, "off") == 0)     { sh_rawmap_swap_arm(0); rawmap_print_state(); return; }

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
        sh_rawmap_save_set_dest(NULL);
        /* sh_rawmap_save_set_dest(NULL) clears the follow toggle as part of restoring the default,
         * so there is nothing extra to do here -- said out loud because "reset both paths" silently
         * turning a toggle off is the sort of thing a reader should not have to go and check. */
        sh_printf("Both paths reset to the default location.\n");
        rawmap_print_state();
        return;
    }

    /* `load <path>` SETS the load path and stages it. `load` with no path OPENS whatever the load
     * path is set to, right now.
     *
     * The split is deliberate, and it fixes a gap rather than inventing one. Naming a file and
     * opening it are different intents -- "point at this from now on" versus "put it in front of me"
     * -- and the console could only express the first. Someone who wanted the second had to set the
     * path and then go and open a map from the map list to trigger it, which is a strange way to ask
     * a console for something. The File menu already had both (the picker stages, the confirm opens);
     * this is the console catching up.
     *
     * A bare `load` DISCARDS unsaved editor edits, the same as answering yes to the menu's confirm.
     * There is no prompt to raise from a console callback, so it says so and does it: the command
     * was typed on purpose, and it cannot damage a SAVED map -- the rawmap opens as a new map that
     * has to be named, which is the interlock sh_editor_frame_request_reload enforces. */
    if (_stricmp(verb, "load") == 0) {
        char why[192] = "";

        if (arg == NULL || arg[0] == '\0') {
            char load_path[MAX_PATH] = "";

            sh_rawmap_get_paths(load_path, (int)sizeof load_path, NULL, 0);

            /* Say no for the ONE reason worth saying no for: there is nothing readable to load.
             * Everything else below is a route, not a refusal. */
            if (!sh_rawmap_source_ok(why, (int)sizeof why)) {
                sh_printf("Cannot open %s\n", load_path[0] ? load_path : "(no load path)");
                sh_printf("  %s\n", why[0] ? why : "it cannot be read");
                return;
            }

            /* ARM FIRST, THEN TRY TO OPEN IT NOW.
             *
             * The order is the fix. This used to call the reload and, when the reload said no,
             * print "Cannot open it" and stop -- which is a dead end for the commonest case there
             * is: standing at the SnapMap main menu, where there is no live editor for the reload to
             * drive. Staging works perfectly well from there. Opening any map from the list applies
             * the staged rawmap, so staging is not a consolation prize, it is the same outcome one
             * click later. A command that could have done the job and reported failure instead is
             * worse than one that does the job the long way and says so.
             *
             * Arming before the attempt also costs nothing when the attempt succeeds: the swap
             * spends the one-shot on the substitution either way. */
            sh_rawmap_load_arm_once();

            if (sh_editor_frame_request_reload(why, (int)sizeof why)) {
                sh_printf("Opening %s as a new map.\n", load_path);
                sh_printf("Unsaved edits in the editor are discarded. Save will ask you to name it.\n");
            } else {
                sh_printf("Staged %s\n", load_path);
                sh_printf("  Open any map and it opens as this rawmap, and Save will ask you to\n");
                sh_printf("  name it. (Not opened right away because %s.)\n",
                          why[0] ? why : "the editor is not ready");
            }
            return;
        }

        /* VALIDATE BEFORE STAGING, exactly as the File menu does -- same function, same reasons.
         *
         * This used to stage the path whatever it was and merely MENTION that the file could not be
         * read, on the theory that refusing to remember a merely-absent file would be its own
         * surprise. That was wrong twice over. The File menu refuses the same file outright, so one
         * surface accepted what the other rejected. And a load path aimed at a file that is not
         * there is good for nothing: there is no later step that creates it (that is the SAVE side),
         * so all it can do is sit there looking armed and then substitute nothing. A typo
         * remembered is worse than a typo refused. */
        if (!sh_rawmap_validate_source(arg, why, (int)sizeof why)) {
            sh_printf("Cannot use %s\n", arg);
            sh_printf("  %s\n", why[0] ? why : "that file cannot be read");
            sh_printf("Nothing was staged. The load path is unchanged.\n");
            return;
        }
        if (!sh_rawmap_swap_set_source(arg)) { sh_printf("That path could not be used.\n"); return; }
        sh_printf("Staged. Open any map -- or run 'sh_rawmaps load' with no path -- and it\n"
                  "becomes a new map you name.\n");
        sh_rawmap_load_arm_once();
        rawmap_print_state();
        return;
    }

    /* `savepath` is the console's way to say where saves go, the same choice "Save
     * Rawmap As" makes in the File menu. Bare form reports and changes nothing.
     *
     * `rawmap` is a shortcut for "the file I have loaded", so editing a rawmap and
     * putting it back does not mean retyping its path. Opening a different map clears
     * whatever was set, so a downloaded map is never written over by a choice made
     * while some other map was open. */
    if (_stricmp(verb, "savepath") == 0) {
        char target[MAX_PATH] = "";
        char why[192] = "";

        if (arg != NULL && arg[0] != '\0') {
            if (_stricmp(arg, "default") == 0) {
                sh_rawmap_set_save_target(NULL);
            } else if (_stricmp(arg, "rawmap") == 0) {
                char loaded[MAX_PATH] = "";
                sh_rawmap_get_paths(loaded, (int)sizeof loaded, NULL, 0);
                if (loaded[0] == '\0' || !sh_rawmap_set_save_target(loaded)) {
                    sh_printf("No rawmap is loaded, so there is nothing to save back over.\n");
                    return;
                }
            /* A word is not a destination: `savepath banana` would otherwise aim every
             * later save at a file in DOOM's own install folder and say nothing. */
            } else if (!sh_rawmap_dest_path_is_usable(arg, why, (int)sizeof why)) {
                sh_printf("Cannot save to %s\n", arg);
                sh_printf("  %s\n", why[0] ? why : "that is not a usable save path");
                sh_printf("Use 'default', 'rawmap', or a full path. Nothing was changed.\n");
                return;
            } else if (!sh_rawmap_set_save_target(arg)) {
                sh_printf("That path could not be used as a save path.\n");
                return;
            }
        }

        sh_rawmap_get_save_target(target, (int)sizeof target);
        if (target[0])
            sh_printf("Saves go to %s until you open a different map.\n", target);
        else
            sh_printf("Saves go to the default rawmap.json, so a map you opened is left alone.\n");
        rawmap_print_state();
        return;
    }

    if (_stricmp(verb, "save") == 0) {
        char why[192] = "";
        char save_path[MAX_PATH] = "";

        /* ASK BEFORE CHANGING ANYTHING. This used to set the destination first and find out
         * afterwards whether a save was even possible, so `sh_rawmaps save <path>` at the main menu
         * moved the person's save path and then refused -- a failed command with a side effect, and
         * nothing on screen said the path had moved. The probe has no side effects. */
        if (!sh_editor_frame_can_rawmap_save(why, (int)sizeof why)) {
            sh_printf("Cannot save: %s\n", why[0] ? why : "the editor is not ready");
            sh_printf("Nothing was changed. Open a map in the editor first.\n");
            return;
        }

        /* WORK OUT THE DESTINATION WITHOUT AIMING AT IT YET.
         *
         * The order here is the whole point. An earlier version aimed first and vetted afterwards,
         * which meant a REFUSED path still became the destination: `sh_rawmaps save <read-only file>`
         * printed its refusal and left the save pointed at a file that cannot be written, so the
         * next plain `sh_rawmaps save` failed too, at a path the person never chose for keeping.
         *
         * With an argument the destination is that argument; without one it is whatever the setting
         * resolves to. Either way it is only a candidate until every check has passed. */
        if (arg != NULL && arg[0] != '\0')
            strncpy_s(save_path, sizeof save_path, arg, _TRUNCATE);
        else
            sh_rawmap_get_paths(NULL, 0, save_path, (int)sizeof save_path);

        /* Covers both the bare-word case (a name is not a path, and would land in DOOM's install
         * folder) and a target that exists but cannot be written. Nothing has been aimed anywhere
         * yet, so a refusal here changes nothing at all. */
        if (!sh_rawmap_dest_writable_now(save_path, why, (int)sizeof why)) {
            sh_printf("Cannot save to %s\n", save_path);
            sh_printf("  %s\n", why[0] ? why : "that file cannot be written");
            sh_printf("Nothing was changed. The save path is untouched.\n");
            return;
        }

        /* Queued onto the editor frame -- serializing the open map touches engine state. A console
         * handler runs as a Cbuf callback on the main thread, but not inside the editor's frame, so
         * it queues like every other engine touch this project makes.
         *
         * Only ever the OPEN map: no disk fallback, unlike the File menu's ladder. A fallback that
         * reads the newest save off disk is how a never-saved map silently exports a DIFFERENT map,
         * and a console command that writes the wrong map is worse than one that says no. */
        if (sh_editor_frame_request_rawmap_save_to(arg && arg[0] ? arg : NULL, why, (int)sizeof why)) {
            /* Present tense, one line. This briefly said "Queued: the open map will be written to",
             * on the reasoning that the write lands on a later editor frame and so cannot be
             * reported as done -- true, and useless: that frame is one of about thirty a second, and
             * "queued" reads as something that might sit there indefinitely.
             *
             * The problem was never this wording. It was that an unwritable destination got this far
             * at all; the check above refuses that now, so by the time this prints the write is
             * genuinely about to happen. */
            sh_printf("Writing the open map to %s\n", save_path);
        } else {
            /* Reachable despite the probe: the editor can leave a live state between the two calls,
             * and the queue slot can be taken. Reported, not asserted. */
            sh_printf("Cannot save: %s\n", why[0] ? why : "the editor is not ready");
        }
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
        sh_printf("sh_listres: GetDeclsOfType unresolved -- cannot list.\n");
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
#define SH_ENTLIST_MAX  16384   /* Candidate capacity. */
static void h_sh_entlist(idCmdArgs *a)
{
    const char *filter = cmd_argv(a, 1);

    static const char *names[SH_ENTLIST_MAX];   /* main-thread-serial console handler -> static is safe */
    int printed = 0;
    int k = sh_typeinfo_collect_classnames(names, SH_ENTLIST_MAX);
    if (k > 0) {
        for (int i = 0; i < k; i++) {
            const char *name = names[i];
            if (name == NULL || strcmp(name, "idEntity") == 0) continue;
            if (sh_typeinfo_class_derives(name, "idEntity") != 1) continue;
            if (filter != NULL && strstr(name, filter) == NULL) continue;
            sh_printf("%s\n", name);
            printed++;
        }
        if (k >= SH_ENTLIST_MAX)
            sh_printf("(registry list truncated at %d -- raise SH_ENTLIST_MAX)\n", SH_ENTLIST_MAX);
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

/* Shared help/refusal wrapper for unimplemented commands. */
#define STUB_HANDLER(fn, help_text)                                              \
    static void fn(idCmdArgs *a) {                                              \
        (void)a;                                                                \
        sh_printf("%s\n(not yet implemented in clone)\n", help_text);          \
    }

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
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return NULL;
    __try { return ((default_idstr_ctor_fn)r.addr)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *eng_idstr_ctor_copy(void *self, const char *cstr)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrCtor", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return NULL;
    __try { return ((idstr_ctor2_fn)r.addr)(self, cstr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static int eng_idstr_assign(void *dstField, const char *cstr)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrAssign", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((idstr_assign2_fn)r.addr)(dstField, cstr); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void eng_idstr_dtor(void *self)
{
    sig_result r;
    if (!resolve_sig_by_name("IdStrDtor", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return;
    __try { ((idstr_dtor2_fn)r.addr)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* Leave failed cleanup to the process lifetime. */ }
}
static int eng_md6_ctor(void *md6)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6Ctor", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((md6_ctor_fn)r.addr)(md6); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_md6_setoutput(void *md6, void *output_idstr)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6SetOutput", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((md6_setoutput_fn)r.addr)(md6, output_idstr); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_md6_build(void *md6)
{
    sig_result r;
    if (!resolve_sig_by_name("Md6Build", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
    __try { ((md6_build_fn)r.addr)(md6); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int eng_bmodel_builder(void *out208, const char *input, const char *output, void *opts)
{
    sig_result r;
    if (!resolve_sig_by_name("BModelBuilder", &r) ||
        (r.status != SIG_OK && r.status != SIG_OK_HOOKED) || r.addr == 0) return 0;
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
        sh_printf("sh_genmd6model: idStr ctor unresolved -- cannot compile.\n");
        return;
    }
    if (!eng_md6_ctor(md6)) {
        sh_printf("sh_genmd6model: md6 builder unresolved -- cannot compile.\n");
        eng_idstr_dtor(opts);
        return;
    }

    if (!eng_idstr_assign(inS, input) || !eng_idstr_ctor_copy(outS, output)) {
        sh_printf("sh_genmd6model: idStr assign/ctor unresolved -- cannot compile.\n");
        eng_idstr_dtor(opts);
        return;
    }
    if (!eng_md6_setoutput(md6, outS)) {
        sh_printf("sh_genmd6model: md6 SetOutput unresolved/faulted.\n");
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
        sh_printf("sh_genmd6model: md6 build unresolved/faulted ('%s').\n", input);
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
        sh_printf("sh_genbmodel: idStr ctor unresolved -- cannot generate.\n");
        return;
    }
    int built = eng_bmodel_builder(out208, input, output, optsS);
    eng_idstr_dtor(optsS);

    if (built)
        sh_printf("sh_genbmodel: generated bmodel '%s' -> '%s'\n", input, output);
    else
        sh_printf("sh_genbmodel: bmodel builder unresolved/faulted ('%s').\n", input);
}

/* Render inspection uses signed render-world anchors; showcursor writes one
 * signed editor-object field. Other toggles are local diagnostics. Unsupported
 * mutators and the original debugger/machine-specific dump paths are refused. */
#define RW_SLOT_KNOWN_RVA         0x57216f0u  /* Pinned Vulkan fallback slot; the host gate checks filename only. */
/* Recheck vtable/field offsets when porting. Derive model-count/model/name
 * accesses from the dumpmodelinfo handler and the cursor byte from showcursor. */
#define RW_VSLOT_MODEL_COUNT      0x188       /* renderWorld vtbl -> GetActiveRenderModelCount() -> uint (BUILD-SPECIFIC) */
#define RW_VSLOT_GET_MODEL        0x190       /* renderWorld vtbl -> GetRenderModel(idx) -> model* (=400; BUILD-SPECIFIC) */
#define RW_MODEL_NAME_OFF         0x10        /* render model -> name char* (model+0x10) (BUILD-SPECIFIC) */
#define ED_SHOWCURSOR_OFF         0x23624u    /* editor -> showcursor byte (OG writes 0) (BUILD-SPECIFIC) */
#define RW_MODEL_COUNT_CAP        1000000u    /* stale-renderWorld guard on the model count */
#define EDITOR_SINGLETON_RVA      0x3056748u  /* Pinned Vulkan audit reference; the editor global anchor locates the object. */



/* Resolve renderWorld by accessor, independent global anchor, then a
 * Vulkan-name-gated RVA. Return NULL if no readable non-NULL object is found. */
static void *dr_resolve_renderworld(void)
{
    sig_result r;
    if (resolve_sig_by_name("RenderWorldGetter", &r) &&
        (r.status == SIG_OK || r.status == SIG_OK_HOOKED) && r.addr != 0) {
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
    if (g_module_base && sh_host_is_pinned_rva_build()) {
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
        backend_log("B2: sh_debugrender showcursor declined -- editor_singleton unresolved on this build");
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

    /* Deliberately refused legacy operations. */
    if (strcmp(sub, "loadimg_n_break") == 0) {
        sh_printf("sh_debugrender: '%s' not available -- it ends in a debugger INT3 trap (halts the game). "
                  "Refused by the clone.\n", sub);
        return;
    }
    if (strcmp(sub, "dump_megatex") == 0) {
        sh_printf("sh_debugrender: '%s' not available -- it hardcodes a write to C:\\Users\\Chris\\megatex.raw "
                  "(a dev path). Refused by the clone.\n", sub);
        return;
    }

    /* Unsupported engine-mutating debug operations. */
    if (strcmp(sub, "test_rm_commit") == 0 || strcmp(sub, "test_sum_shit") == 0 ||
        strcmp(sub, "testnewgui") == 0) {
        sh_printf("sh_debugrender: '%s' is an internal render mutator of the original tool -- not ported.\n", sub);
        return;
    }

    sh_printf("sh_debugrender: unknown sub-op '%s'.\n", sub);
}

/* Dispatch sh subcommands inline on the engine main thread. Backend handlers
 * can serialize, edit, commit, select, and toast at this command-execution point;
 * sending them to the frontend worker would make those engine calls off-thread.
 * argv begins at the subcommand name. Borrowed argument strings remain valid
 * for this callback; missing commands and caught handler faults print errors. */
static void h_sh_dispatch(idCmdArgs *a)
{
    sh_iface *iface = sh_ui_get_iface();
    if (iface == NULL) {
        sh_printf("Ui interface doesnt exist yet!\n");
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
        sh_printf("Command %s has not been registered yet\n", sub);
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
        sh_printf("sh %s: handler faulted (recovered; the op did not complete)\n", sub);
    }
}

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

/* Guard file output and report write success. */
static int cde_write_file(FILE *fp, const char *buf)
{
    __try { fputs(buf, fp); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
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
    for (unsigned i = 0; i < count; i++) {
        const char *name = ss_call_getname(evmgr, i);
        if (name == NULL || name[0] == '\0') continue;

        const char *fspec   = NULL;
        unsigned    numargs = 0;
        unsigned    eventnum = i;                          /* dense-table fallback if rec unreadable */
        void *rec = ss_call_findbyname(evmgr, name);
        if (rec != NULL) {
            fspec    = ss_read_fspec(rec);
            eventnum = cde_read_eventnum(rec, i);
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

/* Entity handlers share this command ABI. */
void h_sh_dumpdef(idCmdArgs *a);
void h_sh_spawninfo(idCmdArgs *a);
void h_sh_spawn(idCmdArgs *a);
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

/* Optional math overrides and status reporting. */
void h_cs_dontuse(idCmdArgs *a);
void h_alginfo(idCmdArgs *a);

/* Palette visibility toggle. */
void h_target_any(idCmdArgs *a);

/* Console command names and their displayed help. */
typedef struct cmd_entry {
    const char *name;
    void       *handler;
    const char *help;
} cmd_entry;

static void h_sh_help(idCmdArgs *a);

/* Raise a diagnostic dialog. Join remaining arguments to preserve message spaces. */
static void h_sh_dialogtest(idCmdArgs *a)
{
    char text[256];
    int argc = cmd_argc(a);
    unsigned gdm_id = 0x6Du;
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

/* Report served baked navigation and reasons for falling back to shipped data. */
static void h_sh_navmesh(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
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
    sh_navmesh_report(sh_printf);
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

static const cmd_entry CMD_TABLE[] = {
    { "sh_perf",              (void *)h_sh_perf,     "Where frame time goes inside snapmap-plus. 'sh_perf reset' starts the counting again." },
    { "sh_rawmaps",           (void *)h_sh_rawmaps,   "Raw JSON map files: state, paths, load, save. Run with no arguments to see what is set, or 'sh_rawmaps help' (or '?') for every verb." },
    { "sh_rawmaps_on",       (void *)h_rawmaps_on,  "(legacy) Same as 'sh_rawmaps on'. Kept because older guides use it." },
    { "sh_rawmaps_off",      (void *)h_rawmaps_off, "(legacy) Same as 'sh_rawmaps off'. Kept because older guides use it." },
    { "sh_type",             (void *)h_sh_type,     "Dumps a types (enum/class) fields to the console and copies the text to your clipboard." },
    { "sh_validclasses",     (void *)h_sh_validclasses,"sh_validclasses <inherit> -- lists the engine-valid classNames for an inherit (the classes deriving from its base type Y; the class-dropdown enumerator)." },
    { "sh_entlist",          (void *)h_sh_entlist,  "Dumps the list of idEntity types in the engine" },
    { "sh_disable_devmode",  (void *)h_disable_devmode,  "disable devmode" },
    { "sh_reenable_devmode", (void *)h_reenable_devmode, "re-enable devmode" },
    { "sh_dumpmap",          (void *)h_sh_dumpmap,  "sh_dumpmap <name> dumps the current mapfile, even the generated snapmap mapfile, to <game dir>\\base\\mapdumps\\<name>.map (never overwrites: repeats get _2, _3, ...)" },
    { "sh_spawn",            (void *)h_sh_spawn,    "sh_spawn <entitydef> <entity name after spawning>" },
    { "sh_dumpdef",          (void *)h_sh_dumpdef,  "sh_dumpdef <entity name>, dumps the entitydef of an existing ingame entity" },
    { "cs_fieldinfo",        (void *)h_cs_fieldinfo,"Internal type-field diagnostic -- you dont need this" },
    { "sh_genbmodel",        (void *)h_sh_genbmodel,"sh_genbmodel <input file> <output file> Generate a bmodel from a .obj/.ase/.lwo file. " },
    { "sh_genmd6model",      (void *)h_sh_genmd6model,"sh_genmd6model <input file> <output file> Compiles a .md6model into a bmd6model" },
    { "sh_target_any",       (void *)h_target_any,  "Toggles targetting for entities. Reveals / re-hides the campaign-only and normally-hidden placeable entity decls in the SnapMap editor palette." },
    { "sh_dialogtest",       (void *)h_sh_dialogtest, "[gdmid] [buttonset] [text...] raise the engine's own dialog carrying this text (diagnostic)" },
    { "sh_dialogpoll",       (void *)h_sh_dialogpoll, "read the answer to the dialog sh_dialogtest raised (diagnostic)" },
    { "sh_dialogdump",       (void *)h_sh_dialogdump, "print the engine dialog queue: id, button set and flag bytes (diagnostic)" },
    { "sh_listres",          (void *)h_sh_listres,  "<resource classname (ex:idMaterial)> <optional: filter> list all resources of a given type" },
    { "sh_alginfo",          (void *)h_alginfo,     "Prints CPU dispatcher info for the engine-math (algo) override layer." },
    { "sh_debugrender",      (void *)h_sh_debugrender,"Internal renderer-test mutators -- not for normal use" },
    { "cs_dontuse",          (void *)h_cs_dontuse,  "Overrides some calculations in the engine to be more precise, just for shiggles. probably degrades performance and breaks stuff." },
    { "sh_superscriptop",    (void *)h_sh_superscriptop,"Internal/dev: dump engine event definitions for SuperScript" },
    { "cs_dumpeventdefs",    (void *)h_cs_dumpeventdefs,"Internal/dev: dumps all eventdefs to a file (for the wiki)" },
    { "cs_start_render_logging", (void *)h_cs_start_render_logging, "Sets up the renderlog hook " },
    { "sh_spawninfo",        (void *)h_sh_spawninfo,"Generate spawnOrientation/spawnPosition from current position in map" },
    { "sh",                  (void *)h_sh_dispatch, "Dispatches a Snapmap+ command" },

    { "noClip",              (void *)h_noclip,         "Toggle noclip (no-collision flight) for the local player." },
    { "infiniteHealth",      (void *)h_infinitehealth, "Toggle infinite health for the local player." },
    { "noPlayerDeath",       (void *)h_noplayerdeath,  "Toggle no-death (the player cannot die) for the local player." },
    { "noPlayerKill",        (void *)h_noplayerkill,   "Toggle no-kill (the player cannot be killed) for the local player." },
    { "noTarget",            (void *)h_notarget,       "Toggle notarget (enemies ignore the local player)." },
    { "sh_user_overrides", (void *)h_sh_user_overrides,
      "sh_user_overrides [0|1] -- persist whether player override files load on the next DOOM launch; restart required; built-in defaults stay enabled." },
    { "sh_navmesh",          (void *)h_sh_navmesh,
      "Reports the baked AI navigation the current map is serving -- which modules and nav classes, or why a bake was refused." },

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

/* Expose commands to both full/developer lookup tables with flags 0x06.
 * Hook future registrations and backfill existing commands into the developer
 * list using the engine allocator. Never alias the full/developer arrays: their
 * independent counts would make later appends overwrite or duplicate entries.
 * idCommand layout: name@0, handler@8, completion@0x10, help@0x18, flags@0x20. */
#define CMD_FULL_ARRAY_OFF  0x08u
#define CMD_FULL_COUNT_OFF  0x10u
#define CMD_DEV_ARRAY_OFF   0x20u
#define CMD_DEV_COUNT_OFF   0x28u
#define CMD_DEV_CAP_OFF     0x2cu
#define CMD_OBJ_FLAGS_OFF   0x20u
#define CMD_DEV_FLAGS       0x6u        /* 0x2 cheat-exempt | 0x4 dev-table membership */
#define CMD_COUNT_SANITY    100000u

/* Resolve list growth from AddCommand's LEA RCX,[RSI+8]; CALL rel32.
 * Generic idList instantiations share their bytes, so use this relationship
 * rather than a direct signature or raw RVA. */
#define IDLIST_GROW_RVA     0x699a60u   /* pinned-build value -- cross-check only, never used to locate */
typedef void (*idlist_grow_fn)(void *idlist);

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

        char l[160];
        uintptr_t rva = (uintptr_t)(tgt - module_base);
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "B2: idList-grow decoded from AddCommand+0x%X -> rva=0x%llX (pinned 0x%X)%s",
                    i, (unsigned long long)rva, IDLIST_GROW_RVA,
                    rva == IDLIST_GROW_RVA ? "" : " MISMATCH -- trusting the decode");
        backend_log(l);
        return (idlist_grow_fn)tgt;
    }
    backend_log("B2: idList-grow call site NOT found in AddCommand; command-unlock insert skipped");
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
    if (g_addcmd_tramp)
        g_addcmd_tramp(cmdsys, name, handler, help, argComp, flags | CMD_DEV_FLAGS);
}

/* On an unreadable developer list, assume present to avoid a duplicate append. */
static int cmd_in_dev(uint8_t *cmdSys, void *cmd)
{
    __try {
        void   **dev = *(void ***)(cmdSys + CMD_DEV_ARRAY_OFF);
        uint32_t n   = *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF);
        if (dev == NULL) return 0;
        if (n > CMD_COUNT_SANITY) return 1;
        for (uint32_t i = 0; i < n; i++)
            if (dev[i] == cmd) return 1;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

/* Append using the engine's own list allocator; keep the full array separate. */
static void cmd_dev_append(uint8_t *cmdSys, void *cmd, idlist_grow_fn grow)
{
    __try {
        uint32_t count = *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF);
        uint32_t cap   = *(uint32_t *)(cmdSys + CMD_DEV_CAP_OFF);
        if (count >= cap) {
            if (!grow) return;
            grow(cmdSys + CMD_DEV_ARRAY_OFF);        /* Growth can replace the backing array. */
            count = *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF);
            cap   = *(uint32_t *)(cmdSys + CMD_DEV_CAP_OFF);
        }
        if (count < cap) {
            void **dev = *(void ***)(cmdSys + CMD_DEV_ARRAY_OFF);
            if (dev != NULL) {
                dev[count] = cmd;
                *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF) = count + 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {   }
}

/* Expose preexisting commands and add missing developer-list entries. */
static uint32_t command_unlock_pass(uint8_t *cmdSys, idlist_grow_fn grow)
{
    void   **full = NULL;
    uint32_t n = 0;
    __try {
        full = *(void ***)(cmdSys + CMD_FULL_ARRAY_OFF);
        n    = *(uint32_t *)(cmdSys + CMD_FULL_COUNT_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (full == NULL || n == 0 || n > CMD_COUNT_SANITY) return 0;

    for (uint32_t i = 0; i < n; i++) {
        void *cmd = NULL;
        __try { cmd = full[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (cmd == NULL) continue;
        __try { *(uint32_t *)((uint8_t *)cmd + CMD_OBJ_FLAGS_OFF) |= CMD_DEV_FLAGS; }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!cmd_in_dev(cmdSys, cmd))
            cmd_dev_append(cmdSys, cmd, grow);
    }
    return n;
}

/* Hook later registrations and backfill existing commands once. */
static void sh_command_unlock_install(void *cmdsys, void *add_command, const uint8_t *module_base)
{
    if (cmdsys == NULL || add_command == NULL) {
        backend_log("B2: command-unlock SKIPPED -- cmdsys/AddCommand unresolved");
        return;
    }
    idlist_grow_fn grow = sh_decode_idlist_grow(add_command, module_base);

    /* Future registrations inherit exposure flags. */
    if (g_addcmd_tramp && !hook_is_installed((void *)g_addcmd_tramp) &&
        hook_unpatch((void *)g_addcmd_tramp)) g_addcmd_tramp = NULL;
    if (!g_addcmd_tramp) {
        g_addcmd_tramp = (add_command6_fn)hook_prepare(add_command, (void *)hook_add_command, ADDCMD_STOLEN);
        if (g_addcmd_tramp && hook_commit((void *)g_addcmd_tramp) != B2_PATCH_OK &&
            hook_unpatch((void *)g_addcmd_tramp)) g_addcmd_tramp = NULL;
    }
    if (hook_is_installed((void *)g_addcmd_tramp)) {
        backend_log("B2: command-unlock -- AddCommand detour installed (flags|6 on every registration)");
    } else {
        backend_log("B2: command-unlock -- AddCommand detour FAILED (one-time pass still runs)");
    }

    /* Backfill existing commands. */
    uint32_t walked = command_unlock_pass((uint8_t *)cmdsys, grow);
    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: command-unlock APPLIED -- %u commands now in DEV table + cheat-exempt (god/noclip/give stay usable after dev mode toggles)",
        walked);
    backend_log(line);
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
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;

    if (!add_command) { backend_log("B2: commands SKIPPED -- AddCommand unresolved"); return 0; }
    if (!printf_disp) { backend_log("B2: commands SKIPPED -- Printf unresolved"); return 0; }
    if (!cmdsys)      { backend_log("B2: commands SKIPPED -- cmdSystem unresolved"); return 0; }

    g_add_command = (add_command_fn)add_command;
    g_cmdsys      = cmdsys;
    g_printf      = (printf_dispatch_fn)printf_disp;
    g_get_decls   = get_decls;
    g_module_base = module_base;

    int n = 0;
    for (int i = 0; i < CMD_COUNT; i++)
        if (register_cmd(&CMD_TABLE[i])) n++;

    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: registered %d/%d console commands (cmdsys=%p add=%p printf=%p)",
        n, CMD_COUNT, cmdsys, add_command, printf_disp);
    backend_log(line);

    /* Apply exposure to both current commands and future engine registrations. */
    sh_command_unlock_install(g_cmdsys, (void *)g_add_command, g_module_base);
    return n;
}
