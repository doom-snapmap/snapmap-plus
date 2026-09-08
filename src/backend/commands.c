/* commands.c -- see commands.h. The console-command surface: the AddCommand spine cloned from OG
 * FUN_1800229b1, plus the handlers. Tier B/C handlers are stubs that print the OG help.
 *
 * sh_rawmaps_on/off (OG snapHak_rawmaps_on/off) drive the shipped sh_rawmap_swap_arm gate.
 * sh_target_any is the editor-decl visibility toggle in target_any.c, ported from OG FUN_180021EE0.
 * snaphak_algo (cs_dontuse [18] + sh_alginfo) lives in algo.c, extern-declared near CMD_TABLE.
 *
 * Clean-room: ported from our own RE, with the command names and help read from the OG XINPUT1_3.dll
 * string table. Zero OG SnapHak bytes.
 */
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
#include "typeinfo.h"          /* sh_typeinfo_get_declmgr() -- shared declMgr accessor for [12] */
#include "entlist_classes.h"
#include "patch.h"
#include "signatures.h"
#include "engine_globals.h"   /* glb_resolve -- the portable data-global resolver */
#include "host_image.h"       /* sh_host_is_pinned_rva_build -- gates the last-resort RVAs */
#include "rawmap.h"
#include "editor_frame.h"   /* sh_editor_frame_request_rawmap_save -- sh_rawmaps save */
#include "ui_bridge.h"   /* sh_ui_get_iface() -- the `sh` dispatcher gates on the interface */
#include "hook.h"        /* install_inline_hook -- the AddCommand detour for the command unlock */
#include "backend_log.h"
#include "engine_dialog.h"
#include "navmesh.h"      /* sh_navmesh -- what the current map's bake is serving */
#include "nav_bake.h"

/* ------------------------------------------------------------------------ engine fn typedefs ------ */

/* idCmdSystemLocal::AddCommand(cmdsys, name, handler, help, argComp, flags) [DIRECT, the AddCommand
 * decompile @0x1aa3630 + the OG registrar @0x229b1]. We pass argComp=NULL and flags=2; see register_cmd
 * for what the engine does with the 2.
 *
 * SLOT MAP: the engine stores cmd[0]=name=param_2, cmd[1]=handler=param_3, cmd[2]=param_5,
 * cmd[3]=param_4, cmd[4]=flags. Help goes in param_4, matching the OG registrar, whose commands display
 * their help in-game -- so cmd[3] is the slot the engine reads for help. (apply_engine.c's
 * clone_bss_apply uses the other order for an internal command never shown to users, and stays as-is.) */
typedef void (*add_command_fn)(void *cmdsys, const char *name, void *handler,
                               const char *help, void *argComp, unsigned int flags);

/* idCommon message dispatch (Printf sig 0x1A08E80). OG's wrapper FUN_180006380 calls it as
 * (level=1, fmt, &va) -- a POINTER to the spilled varargs. We pre-format with _vsnprintf then call the
 * safe fixed-arg form dispatch(1, "%s", &bufptr) so we never re-derive the engine va layout. */
typedef void (*printf_dispatch_fn)(int level, const char *fmt, void *vaptr);

/* GetDeclsOfType(typeName) -> the typed decl-manager node (same engine fn sh_listres uses; sig
 * "GetDeclsOfType" @0x1800D20). Returns NULL for an unknown type. */
typedef void *(*get_decls_fn)(const char *type_name);

/* idCmdArgs + the shared SEH accessors + sh_printf + the global-decode scanner are declared in
 * commands.h (entity.c's moved handlers reuse them). */

/* ------------------------------------------------------------------------- module state ----------- */

static add_command_fn     g_add_command = NULL;
static int                g_dialogtest_ticket = 0;
static void              *g_cmdsys      = NULL;
static printf_dispatch_fn g_printf      = NULL;
static void              *g_get_decls   = NULL;   /* cached for sh_listres + the material lookups */
static const uint8_t     *g_module_base = NULL;   /* DOOM module base (devmode resolves its sig at FIRE) */
static volatile LONG      g_installed   = 0;      /* one-shot install latch */

/* [15][16] devmode: ONE static restore-handle, gated on g_devmode_handle.live (static zero-init => .live==0
 * => "not currently disabled"). disable_devmode code_patch_sig's the SessionDevModeGetter head to
 * `xor eax,eax; ret`; reenable_devmode code_unpatch's it. The patch layer owns all the SEH/verify. */
static sh_patch_handle    g_devmode_handle;       /* zero-init: .live == 0 (not patched) */

/* ------------------------------------------------------------------------- Printf wrapper ---------
 * sh_printf(fmt, ...) -- format into a stack buffer, then dispatch(1, "%s", &bufptr). The engine's
 * idCommon::dispatch reads a POINTER to the args, so we pass the address of a single (char*) holding
 * our pre-formatted buffer -- exactly one %s consumed, no engine-va guesswork. */
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
        /* a console dispatch fault must never take down the editor */
    }
}

/* SEH-guarded idCmdArgs accessors (the engine hands us the args object; never trust its shape).
 * Non-static -- declared in commands.h so entity.c's moved handlers share the SAME accessors. */
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

/* ----------------------------------------------------------------- cmdSystem-global decode --------
 * The CmdSystemLea accessor (the engine bot_add/bot_remove registrar) loads the idCmdSystemLocal*
 * global in its prologue via `MOV RCX,[rip+cmdSystem]` (48 8B 0D). Decode the first RIP-relative load
 * -- four forms, 48 8D 0D / 48 8B 0D / 48 8D 05 / 48 8B 05, this one the MOV rather than the LEA
 * sh_strids decodes -- to the global slot, then deref once for the live object. On a failed decode,
 * glb_resolve("cmd_system_slot") signs a second, independent site and decodes its displacement. The
 * pinned literal below is only considered if both miss, and only on the build it came from. */
#define CMDSYS_KNOWN_RVA   0x55b7280u   /* the cmdSystem slot's RVA on the pinned Vulkan build -- kept for
                                         * audit and per-build re-derivation. Dereferenced only when
                                         * sh_host_is_pinned_rva_build() says we ARE that build. */

/* SHARED with entity.c (declared in commands.h). The gameMgr-global decode reuses the EXACT same
 * 4-opcode RIP-relative scanner so the two globals decode through ONE code path (no duplicate-and-drift). */
int sh_safe_read(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Decode a build-specific .data global SLOT address from a sig'd accessor fn whose prologue carries a
 * RIP-relative MOV/LEA to that global. Scans the first B2_RIP_SCAN_WINDOW bytes for the FIRST of the four
 * decode-target opcodes (48 8B 0D / 48 8B 05 / 48 8D 0D / 48 8D 05) and returns rip_next + disp32. Used by
 * BOTH sh_resolve_cmdsys (CmdSystemLea, the MOV-RCX form 48 8B 0D at offset 6) and sh_resolve_gamemgr
 * (GameMgrLea, the MOV-RAX form 48 8B 05 at offset 0). Returns the SLOT, NOT the dereferenced object. */
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
    /* Primary: decode the slot from the sig'd accessor, then deref once for the live object. */
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
    /* Second portable path: a signed code site whose RIP displacement names the same slot. Survives an
     * inline hook on the CmdSystemLea prologue, which the decode above does not. */
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
    /* Last resort: the pinned literal, and only on the build it was extracted from. On the other shipped
     * image that RVA is unrelated memory whose contents we would publish as idCmdSystemLocal*. */
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

/* ----------------------------------------------------------------------------- handlers ----------
 * The trivial handlers (wired to shipped ops); all others are faithful stubs. Each is __fastcall with
 * a single idCmdArgs* arg. Handlers run as a Cbuf callback on the engine main thread (console exec). */

/* [1] sh_rawmaps_on (OG snapHak_rawmaps_on) -> the SHIPPED sh_rawmap_swap_arm(1) gate (single source of
 *     truth). Prints the OG RUNTIME message "Enabling raw snapmap save/load." (the OG handler @0x21050),
 *     NOT the AddCommand help. */
static void h_rawmaps_on(idCmdArgs *a)
{
    char load_path[MAX_PATH] = "", save_path[MAX_PATH] = "", why[192] = "";
    int  readable;

    (void)a;
    sh_rawmap_swap_arm(1);
    sh_printf("Enabling raw snapmap save/load.\n");

    /* Name the files being armed: this switch applies them to every later map load and save, and
     * both may have been set by earlier clicks. */
    sh_rawmap_get_paths(load_path, (int)sizeof load_path, save_path, (int)sizeof save_path);

    /* Legacy name, kept because the published guide teaches it. Deliberately no behaviour of its
     * own: two near-identical command names differing invisibly is a worse trap. */
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
        /* Say WHY the destination is what it is. The path alone cannot distinguish "this is the
         * default", "this follows the loaded rawmap" and "this is pinned here", and those three
         * answer very different questions about what the next save will do. */
        char fixed[MAX_PATH] = "";
        int  mode = sh_rawmap_dest_mode(fixed, (int)sizeof fixed);
        const char *tag = (mode == SH_RAWMAP_DEST_RAWMAP) ? "   (following the loaded rawmap)"
                        : (mode == SH_RAWMAP_DEST_FIXED)  ? "   (pinned by 'sh_rawmaps savepath')"
                        : "   (the default)";
        sh_printf("  save to:   %s%s\n", save_path[0] ? save_path : "(none)", tag);
    }
    sh_printf("Every map you open now loads that file, and every save is mirrored to that one.\n");
    sh_printf("(legacy name -- 'sh_rawmaps' shows and changes everything, including both paths.)\n");
}
/* [2] sh_rawmaps_off (OG snapHak_rawmaps_off) -> sh_rawmap_swap_arm(0). Prints OG RUNTIME "Disabling raw
 *     snapmap save/load." (the OG handler @0x21070), NOT the AddCommand help. */
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
/* [2b] sh_rawmaps -- the whole rawmap surface, and the bare form answers without changing anything.
 *
 *   sh_rawmaps                  both paths (changes nothing)
 *   sh_rawmaps list [folder]    the rawmaps in a folder
 *   sh_rawmaps on | off         the shared gate
 *   sh_rawmaps load [path]      stage a file, or open the staged one now
 *   sh_rawmaps save [path]      write the OPEN map, optionally somewhere named
 *   sh_rawmaps savepath ...     the durable destination
 *   sh_rawmaps default          both paths back to the default
 *
 * sh_rawmaps_on / sh_rawmaps_off stay: they are the original SnapHak names and are in circulation. */

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

    /* Off is the normal setting and changes nothing, so it gets no line -- naming it reads as "the
     * feature is off", which is false. On changes every later action without asking again. */
    if (sh_rawmap_swap_is_armed())
        sh_printf("  GATE ON - every map you open and save goes through a rawmap.\n");
    sh_printf("  load from: %s%s\n", load_path[0] ? load_path : "(none)",
              readable ? "" : "   <-- cannot be read right now");
    if (!readable && why[0]) sh_printf("             %s\n", why);
    {
        /* Say WHY the destination is what it is. The path alone cannot distinguish "this is the
         * default", "this follows the loaded rawmap" and "this is pinned here", and those three
         * answer very different questions about what the next save will do. */
        char fixed[MAX_PATH] = "";
        int  mode = sh_rawmap_dest_mode(fixed, (int)sizeof fixed);
        const char *tag = (mode == SH_RAWMAP_DEST_RAWMAP) ? "   (following the loaded rawmap)"
                        : (mode == SH_RAWMAP_DEST_FIXED)  ? "   (pinned by 'sh_rawmaps savepath')"
                        : "   (the default)";
        sh_printf("  save to:   %s%s\n", save_path[0] ? save_path : "(none)", tag);
    }
    if (sh_rawmap_load_oneshot_pending())
        sh_printf("  a rawmap is staged for the NEXT map you open.\n");
    if (sh_rawmap_save_oneshot_pending())
        sh_printf("  waiting for your next save in DOOM to write the rawmap.\n");
}

static void h_sh_rawmaps(idCmdArgs *a)
{
    const char *verb = cmd_argv(a, 1);
    const char *arg  = cmd_argv(a, 2);

    if (verb == NULL || verb[0] == '\0') { rawmap_print_state(); return; }

    if (_stricmp(verb, "list") == 0)    { rawmap_print_list(arg); return; }

    if (_stricmp(verb, "on") == 0)      { sh_rawmap_swap_arm(1); rawmap_print_state(); return; }
    if (_stricmp(verb, "off") == 0)     { sh_rawmap_swap_arm(0); rawmap_print_state(); return; }

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

    /* `load <path>` sets the load path and stages it. `load` with no path OPENS the staged file now.
     *
     * A bare `load` DISCARDS unsaved editor edits -- there is no prompt to raise from a console
     * callback, so it says so and does it. It cannot damage a SAVED map: the rawmap opens as a new
     * map that has to be named, the interlock sh_editor_frame_request_reload enforces. */
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

            /* ARM FIRST, THEN TRY TO OPEN IT NOW. With no live editor -- before a map is open --
             * the reload declines, and the staged arm still applies to the next map opened. Arming
             * first costs nothing when the reload succeeds: the swap spends the one-shot either way. */
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

        /* VALIDATE BEFORE STAGING, the same function and rule the File menu uses. A load path aimed
         * at a file that is not there has no later step that creates it -- that is the save side --
         * so remembering it only leaves something that looks armed and substitutes nothing. */
        if (!sh_rawmap_validate_source(arg, why, (int)sizeof why)) {
            sh_printf("Cannot use %s\n", arg);
            sh_printf("  %s\n", why[0] ? why : "that file cannot be read");
            sh_printf("Nothing was staged. The load path is unchanged.\n");
            return;
        }
        if (!sh_rawmap_swap_set_source(arg)) { sh_printf("That path could not be used.\n"); return; }
        /* The same rule the File menu follows, and the same function it calls -- staging a rawmap
         * to LOAD is a read, and a read does not inherit the destination an earlier export chose. */
        sh_rawmap_reset_dest_for_new_load();
        sh_printf("Staged. Open any map -- or run 'sh_rawmaps load' with no path -- and it\n"
                  "becomes a new map you name.\n");
        sh_rawmap_load_arm_once();
        rawmap_print_state();
        return;
    }

    /* The console half of the File menu's "Use Rawmap as Save Path" tick. Bare form reports.
     *
     * THREE VALUES, AND ONLY THREE: `rawmap`, `default`, or a path. No `on` / `off` aliases -- this
     * setting names a destination and has no enabled state, so "savepath on" cannot say on what. */
    if (_stricmp(verb, "savepath") == 0) {
        char fixed[MAX_PATH] = "";
        char why[192] = "";
        int  mode;

        if (arg != NULL && arg[0] != '\0') {
            /* Anything that is not one of the two words is taken as a PATH: this is the durable
             * "always save here" setting, and it is a different verb from `save <path>` precisely so
             * a one-off export cannot quietly become one.
             *
             * The fall-through is CHECKED now. It used to pin whatever was typed, so `savepath
             * banana` aimed every later save at a file called "banana" in DOOM's own install folder
             * and said nothing about it. A word is not a destination. */
            if (_stricmp(arg, "rawmap") == 0)
                sh_rawmap_set_dest_follows_source(1);
            else if (_stricmp(arg, "default") == 0)
                sh_rawmap_set_dest_follows_source(0);
            else if (!sh_rawmap_dest_path_is_usable(arg, why, (int)sizeof why)) {
                sh_printf("Cannot save to %s\n", arg);
                sh_printf("  %s\n", why[0] ? why : "that is not a usable save path");
                sh_printf("Use 'rawmap', 'default', or a full path. Nothing was changed.\n");
                return;
            }
            else if (!sh_rawmap_set_dest_fixed(arg)) {
                sh_printf("That path could not be used as a save path.\n");
                return;
            }
        }

        mode = sh_rawmap_dest_mode(fixed, (int)sizeof fixed);
        if (mode == SH_RAWMAP_DEST_RAWMAP)
            sh_printf("Saves go back over the rawmap you loaded.\n");
        else if (mode == SH_RAWMAP_DEST_FIXED)
            sh_printf("Saves always go to %s\n", fixed);
        else
            sh_printf("Saves go to the default rawmap.json, so a loaded rawmap is left alone.\n");
        rawmap_print_state();
        return;
    }

    if (_stricmp(verb, "save") == 0) {
        char why[192] = "";
        char save_path[MAX_PATH] = "";

        /* ASK BEFORE CHANGING ANYTHING -- the probe has no side effects, so a refusal here cannot
         * leave the save path moved. */
        if (!sh_editor_frame_can_rawmap_save(why, (int)sizeof why)) {
            sh_printf("Cannot save: %s\n", why[0] ? why : "the editor is not ready");
            sh_printf("Nothing was changed. Open a map in the editor first.\n");
            return;
        }

        /* WORK OUT THE DESTINATION WITHOUT AIMING AT IT YET. With an argument it is that argument,
         * without one whatever the setting resolves to -- a candidate until every check has passed,
         * so a refused path never becomes the destination. */
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

        /* sh_rawmap_choose_dest, NOT sh_rawmap_save_set_dest. The setter alone left the follow
         * toggle on, and resolve_dest_path consults that first -- so the write went to the loaded
         * rawmap instead of the file named right here. */
        if (arg != NULL && arg[0] != '\0' && !sh_rawmap_choose_dest(arg)) {
            sh_printf("That path could not be used as a destination.\n");
            return;
        }

        /* Queued onto the editor frame -- serializing the open map touches engine state. A console
         * handler runs as a Cbuf callback on the main thread, but not inside the editor's frame, so
         * it queues like every other engine touch this project makes.
         *
         * Only ever the OPEN map: no disk fallback, unlike the File menu's ladder. A fallback that
         * reads the newest save off disk is how a never-saved map silently exports a DIFFERENT map,
         * and a console command that writes the wrong map is worse than one that says no. */
        if (sh_editor_frame_request_rawmap_save(why, (int)sizeof why)) {
            /* Present tense: the write lands on the next editor frame, one of about thirty a
             * second, and the checks above have already refused anything unwritable. */
            sh_printf("Writing the open map to %s\n", save_path);
        } else {
            /* Reachable despite the probe: the editor can leave a live state between the two calls,
             * and the queue slot can be taken. Reported, not asserted. */
            sh_printf("Cannot save: %s\n", why[0] ? why : "the editor is not ready");
        }
        return;
    }

    sh_printf("sh_rawmaps -- raw JSON map files.\n");
    sh_printf("  sh_rawmaps                 show the state and both paths\n");
    sh_printf("  sh_rawmaps list [folder]   rawmap files: the default folder, the folder your\n");
    sh_printf("                             load path points at, or one you name\n");
    sh_printf("  sh_rawmaps on | off        optional: apply rawmaps to EVERY map load and save\n");
    sh_printf("  sh_rawmaps load <path>     point the load path at a file and stage it\n");
    sh_printf("  sh_rawmaps load            open the load path as a new map -- or stage it,\n");
    sh_printf("                             if the editor is not up yet, so the next map\n");
    sh_printf("                             you open becomes it\n");
    sh_printf("  sh_rawmaps save            write the open map to the save path\n");
    sh_printf("  sh_rawmaps save <path>     export it THERE once, then back to the save path\n");
    sh_printf("  sh_rawmaps savepath [rawmap|default|<path>]\n");
    sh_printf("                             where saves ALWAYS go: over the loaded rawmap,\n");
    sh_printf("                             the default rawmap.json, or one file you name\n");
    sh_printf("  sh_rawmaps default         put both paths back to the default\n");
}

/* [3] sh_alginfo -> algo.c (h_alginfo: reports our snaphak_algo reimpl PRESENT). [18] cs_dontuse ->
 * algo.c (h_cs_dontuse: the toggle that installs/uninstalls the 4 f64 math overrides). Both are
 * extern-declared near the CMD_TABLE (like the sh_entity / sh_typeinfo handlers). */

/* ----------------------------------------------------------------- decl-walk SEH helpers --------
 * sh_listres walks the decl-manager node layout (LIVE-VERIFIED: the decl-ptr array @
 * node+0x20, the count @ node+0x28. Each decl's NAME is a char* @ *decl+8 (the generic idDecl name slot
 * -- DIRECT from OG behavior: sh_listres passes *(*decl+8) as the Printf %s arg; it is an engine RUNTIME
 * offset not in the source-of-record, so LIVE-CONFIRM at FIRE: `sh_listres idMaterial` must print real
 * names). Every read is SEH-guarded; a wrong/garbage node degrades to a clean no-op, never a crash. */
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

/* A tiny growable byte buffer for the sh_copy_reslist_to_clipboard accumulation (each matched name
 * + '\n'). Heap-backed; freed by the caller. On any OOM the buffer goes "failed" and silently stops
 * accumulating (the console print still happens -- the clipboard copy just won't include the overflow). */
typedef struct lr_buf {
    char  *data;
    size_t len;
    size_t cap;
    int    failed;
} lr_buf;

static void lr_buf_append(lr_buf *b, const char *s)
{
    if (b->failed || s == NULL) return;
    size_t add = strlen(s) + 1;                 /* the name + a '\n' */
    if (b->len + add + 1 > b->cap) {            /* +1 for the final NUL */
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

/* [14] sh_listres <type> [filter] -- GetDeclsOfType(type), walk the decl array, print each name; if
 * argv[2] is present, substring-filter; if sh_copy_reslist_to_clipboard is set, accumulate the
 * matched names and copy the list to the clipboard at the end. Clone of OG FUN_180022000
 * (its decompile @0x22000 + our listres-mechanism notes). */
static void h_sh_listres(idCmdArgs *a)
{
    const char *type   = cmd_argv(a, 1);
    const char *filter = cmd_argv(a, 2);        /* NULL => no filter (OG: argc<3) */

    if (type == NULL) {                          /* OG silently returns; we add a usage line */
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
    if (count > LISTRES_COUNT_CAP) {            /* stale/garbage node guard */
        sh_printf("sh_listres: decl count implausible (stale manager node?).\n");
        return;
    }

    int clip = sh_cvar_value_int(B2_CVAR_SH_COPY_RESLIST_TO_CLIPBOARD, 0);
    lr_buf buf = { NULL, 0, 0, 0 };

    uint32_t printed = 0;
    for (uint32_t i = 0; i < count; i++) {
        void *decl = NULL;
        if (!lr_read_ptr((const uint8_t *)array + (size_t)i * 8, &decl)) break;  /* array tail AV */
        if (decl == NULL) continue;

        const char *name = lr_decl_name(decl);
        if (name == NULL) continue;
        if (filter != NULL && strstr(name, filter) == NULL) continue;            /* substring filter */

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

/* [5] sh_entlist [filter] -- every idEntity-derived class NAME, substring-filtered by argv[1]. Clone of
 * OG FUN_180021b50, which walked its own static snapshot of the idEntity subclass set; that snapshot IS
 * the engine's idEntity-derived reflection walk [DIRECT: the live set reproduces the OG's 892
 * string-for-string]. So this enumerates the live type registry (sh_typeinfo_collect_classnames) and
 * keeps what derives from idEntity, which tracks DOOM patches and surfaces any decl-less class the
 * frozen snapshot missed. Two divergences from the OG: idTarget_Command is listed rather than hidden,
 * and there is a trailing count line. Pre-boot, with the registry unreachable, it falls back to the
 * static B2_ENTLIST_CLASSES snapshot. */
#define SH_ENTLIST_MAX  16384   /* candidate-buffer cap (this build ~10,190 registered types) */
static void h_sh_entlist(idCmdArgs *a)
{
    const char *filter = cmd_argv(a, 1);        /* NULL => no filter (OG: argc<=1) */

    static const char *names[SH_ENTLIST_MAX];   /* main-thread-serial console handler -> static is safe */
    int printed = 0;
    int k = sh_typeinfo_collect_classnames(names, SH_ENTLIST_MAX);
    if (k > 0) {                                 /* LIVE registry: keep idEntity subclasses (excl. idEntity base) */
        for (int i = 0; i < k; i++) {
            const char *name = names[i];
            if (name == NULL || strcmp(name, "idEntity") == 0) continue;          /* list SUBCLASSES (OG-faithful) */
            if (sh_typeinfo_class_derives(name, "idEntity") != 1) continue;       /* keep only idEntity-derived */
            if (filter != NULL && strstr(name, filter) == NULL) continue;         /* substring filter */
            sh_printf("%s\n", name);
            printed++;
        }
        if (k >= SH_ENTLIST_MAX)
            sh_printf("(registry list truncated at %d -- raise SH_ENTLIST_MAX)\n", SH_ENTLIST_MAX);
    } else {                                     /* fallback: the static idEntity-subclass snapshot */
        for (int i = 0; i < B2_ENTLIST_CLASS_COUNT; i++) {
            const char *name = B2_ENTLIST_CLASSES[i];
            if (filter != NULL && strstr(name, filter) == NULL) continue;         /* substring filter */
            sh_printf("%s\n", name);
            printed++;
        }
    }
    sh_printf("(%d entity classes%s%s)\n", printed,
              filter ? " matching " : "", filter ? filter : "");
}

/* ----------------------------------------------------------------- [15][16] devmode -------------
 * SnapHak's snaphak_disable_devmode stomps the idSessionLocal devmode bool getter (engine 0x18a31d0:
 * movzx eax,[rcx+0x34c89]; ret) so it always returns 0; reenable restores it. This rides the sh_patch
 * layer -- code_patch_sig / code_unpatch plus the static restore-handle -- and resolves
 * SessionDevModeGetter by signature at FIRE, so a miss or an ambiguity refuses the write on a shifted
 * build instead of mis-patching.
 *
 * Only the 3-byte head is overwritten (0F B6 81 -> 31 C0 C3, `xor eax,eax; ret`), so code_unpatch
 * restores the whole original instruction and the sig re-resolves on it: disable/reenable is repeatable. */
#define DEVMODE_SIG_NAME   "SessionDevModeGetter"

/* Resolve a named engine site from the shipped sig DB (mirrors sh_cvars' NameHash resolve: iterate
 * BACKEND_ENGINE_SIGNATURES, sig_resolve_one over g_module_base). Fills *out; returns 1 if the entry was
 * found in the DB (then *out carries the resolve status, which the code_patch_sig / install_detour_sig
 * gates check), 0 if the name isn't in the DB or no module base is cached. Used by BOTH the [15][16]
 * devmode patch (SessionDevModeGetter) and the [11] render-logging detour (RenderLogStub). */
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

/* [15] sh_disable_devmode (OG snaphak_disable_devmode) -- patch the session devmode getter to return 0
 * (devmode off). */
static void h_disable_devmode(idCmdArgs *a)
{
    (void)a;
    if (g_devmode_handle.live) {
        sh_printf("devmode already disabled\n");
        return;
    }
    sig_result r;
    if (!resolve_sig_by_name(DEVMODE_SIG_NAME, &r)) {
        sh_printf("sh_disable_devmode: %s not in the signature DB -- cannot patch.\n", DEVMODE_SIG_NAME);
        return;
    }

    /* expect = the 3-byte head the sig already verified (0F B6 81); new = `xor eax,eax; ret` (31 C0 C3).
     * code_patch_sig REFUSES unless the resolve was a clean unique SIG_OK hit. */
    const uint8_t expect[3]    = { 0x0F, 0xB6, 0x81 };
    const uint8_t new_bytes[3] = { 0x31, 0xC0, 0xC3 };
    sh_patch_status st = code_patch_sig(&r, expect, new_bytes, 3, &g_devmode_handle);
    if (st == B2_PATCH_OK)
        sh_printf("sh_disable_devmode: %s -- devmode disabled (session getter -> 0)\n",
                  sh_patch_status_str(st));
    else
        sh_printf("sh_disable_devmode: %s -- patch refused, devmode unchanged\n",
                  sh_patch_status_str(st));
}

/* [16] sh_reenable_devmode (OG snaphak_reenable_devmode) -- restore the session devmode getter (undo the
 * disable patch). */
static void h_reenable_devmode(idCmdArgs *a)
{
    (void)a;
    if (!g_devmode_handle.live) {
        sh_printf("devmode not currently disabled\n");
        return;
    }
    sh_patch_status st = code_unpatch(&g_devmode_handle);
    if (st == B2_PATCH_OK)
        sh_printf("sh_reenable_devmode: %s -- devmode re-enabled (getter restored)\n",
                  sh_patch_status_str(st));
    else
        sh_printf("sh_reenable_devmode: %s -- restore failed\n", sh_patch_status_str(st));
}

/* ----------------------------------------------------------------- [11] cs_start_render_logging ---
 * Port of OG FUN_1800224c0: open renderlog.txt and detour the engine's render-debug trace sink
 * (RenderLogStub @0xd99dc0 = `mov [rsp+0x20],r9; ret`, a no-op while logging is off) with a hook that
 * writes its trace lines to the file. The engine hands the sink a formatted printf fmt plus varargs, so
 * the hook reads no renderer internals -- it vfprintf's fmt+va. The original sink is a no-op, so the
 * hook does not trampoline. Start-only and process-lifetime, as in the OG. RenderLogStub resolves by
 * signature at FIRE through sh_install_detour_sig (SIG_OK-gated, SEH-guarded, reversible). */
#define RENDERLOG_SIG_NAME   "RenderLogStub"
#define RENDERLOG_STOLEN     14   /* hook.c writes a 14-byte FF25 abs-jmp + requires stolen>=14; 14<=16 room */

static FILE *g_renderlog_fp    = NULL;
static void *g_renderlog_tramp = NULL;

/* SEH-guarded vfprintf to the render log -- factored out of our_renderlog_hook because MSVC forbids
 * mixing va_start/va_end with __try/__except in the SAME function (it inserts a frame-unwind filter the
 * varargs prologue conflicts with). This helper takes the already-started va_list; a fault while the
 * engine's varargs/fmt are malformed degrades to a clean no-write, never a crash that takes down the
 * renderer. */
static void renderlog_write(FILE *fp, const char *fmt, va_list ap)
{
    __try {
        vfprintf(fp, fmt, ap);
        fflush(fp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* a malformed engine trace line must never fault the render thread */
    }
}

/* The detour hook installed over RenderLogStub. The engine calls the sink as
 * (ctx=RCX, channel=RDX, fmt=R8, ...va); we IGNORE ctx+channel and write fmt + the varargs to
 * renderlog.txt. Reads ZERO renderer internals (the engine supplies a fully-formatted fmt+varargs); does
 * NOT call a trampoline (the original sink was a no-op). */
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

/* [11] cs_start_render_logging -- open renderlog.txt + detour the engine render-debug trace sink so its
 * printf lines are logged. Start-only (mirrors OG: no stop command). */
static void h_cs_start_render_logging(idCmdArgs *a)
{
    (void)a;
    if (g_renderlog_fp) {
        sh_printf("render logging already started\n");
        return;
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

    g_renderlog_tramp = sh_install_detour_sig(&r, (void *)our_renderlog_hook, RENDERLOG_STOLEN);
    if (g_renderlog_tramp == NULL) {
        sh_printf("cs_start_render_logging: detour install refused/failed (%s status=%d) -- logging off.\n",
                  RENDERLOG_SIG_NAME, (int)r.status);
        fclose(g_renderlog_fp);
        g_renderlog_fp = NULL;
        return;
    }
    sh_printf("cs_start_render_logging: render-log hook installed.\n");
}

/* ---- Tier B/C STUBS: register the NAME, print the OG help + a "not yet implemented" note. ----
 * Each closes over its own help via a small per-command wrapper produced by the X-macro below. The
 * stub is faithful surface (the command STOPS returning "Unknown command") without claiming behavior. */
#define STUB_HANDLER(fn, help_text)                                              \
    static void fn(idCmdArgs *a) {                                              \
        (void)a;                                                                \
        sh_printf("%s\n(not yet implemented in clone)\n", help_text);          \
    }

/* ============================================================ WS-C deferred dev/asset commands ====
 * [20] sh_genmd6model / [19] sh_genbmodel / [17] sh_debugrender -- ports of OG XINPUT1_3 FUN_18000b560
 * / FUN_18000b4a0 / FUN_18001ffe0. Every engine fn resolves by signature off the live DOOM module
 * (resolve_sig_by_name over g_module_base), and every engine touch is SEH-guarded: these are faultable
 * asset compilers and a runtime renderWorld vtable. [17] ports only the read-only sub-ops and refuses
 * loadimg_n_break (an INT3 trap) and dump_megatex (a hardcoded per-machine fwrite). */

/* ---- engine fn typedefs for the asset-gen call-targets (resolved by sig at FIRE) ---------------- */
typedef void *(*default_idstr_ctor_fn)(void *self);                    /* DefaultIdStrCtor 0x19fd040 */
typedef void *(*md6_ctor_fn)(void *md6);                               /* Md6Ctor 0x149b8d0 */
typedef void  (*md6_setoutput_fn)(void *md6, void *output_idstr);      /* Md6SetOutput 0x149c450 */
typedef void  (*md6_build_fn)(void *md6);                              /* Md6Build (final call) 0x149bee0 */
typedef void  (*idstr_assign2_fn)(void *dstField, const char *cstr);   /* IdStrAssign 0x1a03e10 */
typedef void *(*idstr_ctor2_fn)(void *self, const char *cstr);         /* IdStrCtor 0x19fcef0 */
typedef void  (*idstr_dtor2_fn)(void *self);                           /* IdStrDtor 0x19fd120 */
typedef void  (*bmodel_builder_fn)(void *out208, const char *input,    /* BModelBuilder 0x14cf550 */
                                   const char *output, void *opts);

/* ---- SEH-guarded single-call wrappers (the engine fns are heavy/faultable; never let a fault out) --
 * Each resolves the named sig at FIRE (build-portable) and invokes under __try. Returns 1 on a ran call,
 * 0 if the sig is missing/unresolved or the call faulted. MSVC forbids mixing C++ object unwinding with
 * __try in one fn, but these are plain C fn-ptr calls so the guard is clean. */
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
    __except (EXCEPTION_EXECUTE_HANDLER) { /* dtor fault -> leak, never crash */ }
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

/* idStr / md6 / bmodel-result stack-object sizes -- over-allocated vs the OG's exact frame slots so a
 * build-shifted object layout can never overrun. OG frames: [20] opts idStr 288B, input idStr 88B, output
 * idStr 48B, md6 ctx; [19] opts idStr 48B, out208 result 208B. We round each generously. */
#define GEN_IDSTR_BYTES   128     /* an idStr (inline buf cap 0x14 + header) -- OG slots 48..288B; 128 covers it */
#define GEN_MD6_BYTES     1024    /* the idMd6Builder ctx (ctor inits md6+0x38..+0x110; build writes +0xf8) */
#define GEN_BMODEL_BYTES  256     /* the BModelBuilder out208 result struct (OG memsets/uses 208 = 0xD0 bytes) */
#define GEN_BOPTS_BYTES   0xD0    /* the bmodel options struct: OG memset(opts,1,0xD0). RECIPE-TAG (build-
                                   * specific): re-confirm 0xD0 on a build bump by tracing how 0x14cf600 reads
                                   * R9 (BModelBuilder sig comment). The clone memsets a 0xD0 buffer to 0x01. */

/* [20] sh_genmd6model <input> <output> -- compile a .md6model into a bmd6model. Real port of OG
 * FUN_18000b560. OG ORDER (DIRECT, from its decompile @0xb560): gate argc>2 (>=3); DefaultIdStrCtor(opts);
 * Md6Ctor(&md6); IdStrAssign(input, argv[1]); IdStrCtor(output, argv[2]); Md6SetOutput(&md6, output);
 * IdStrDtor(output); Md6Build(&md6); IdStrDtor(opts). The OG ctor's a default opts idStr it never passes
 * to the call chain (a scratch the md6 ctx already owns at md6+0x60) -- we ctor+dtor it faithfully to
 * mirror the OG frame lifecycle. Md6Build is destructor-shaped (compile-then-release, the bmd6 buffer
 * write folded into the dtor -- see the Md6Build sig comment). Every engine call SEH-guarded. */
static void h_sh_genmd6model(idCmdArgs *a)
{
    const char *input  = cmd_argv(a, 1);
    const char *output = cmd_argv(a, 2);
    if (cmd_argc(a) <= 2 || input == NULL || output == NULL) {     /* OG gate: 2 < argc */
        sh_printf("sh_genmd6model <input file> <output file> Compiles a .md6model into a bmd6model\n");
        return;
    }

    /* Zero-init the stack objects so a sig miss / partial ctor leaves defined (dtor-safe) memory. */
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
    /* input/output idStrs. IdStrAssign sets the input field; IdStrCtor copies the output C-string. */
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
    eng_idstr_dtor(outS);                  /* OG dtors output right after SetOutput */

    int built = eng_md6_build(md6);        /* the final md6 call (compile-then-release) */
    eng_idstr_dtor(opts);                  /* OG dtors opts last */

    if (built)
        sh_printf("sh_genmd6model: compiled '%s' -> '%s'\n", input, output);
    else
        sh_printf("sh_genmd6model: md6 build unresolved/faulted ('%s').\n", input);
}

/* [19] sh_genbmodel <input> <output> -- generate a bmodel from a .obj/.ase/.lwo. Real port of OG
 * FUN_18000b4a0. OG ORDER (DIRECT, from its decompile @0xb4a0): gate argc>2; memset(opts,1,0xD0); DefaultIdStrCtor(s);
 * BModelBuilder(out208, argv[1]=input, argv[2]=output, &opts); IdStrDtor(s). The default idStr `s` is a
 * scratch the OG ctor's + dtor's around the call (not passed to BModelBuilder) -- mirror its lifecycle.
 * Every engine call SEH-guarded; the out208 result + the 0xD0 opts buffer are stack-local + zero-init. */
static void h_sh_genbmodel(idCmdArgs *a)
{
    const char *input  = cmd_argv(a, 1);
    const char *output = cmd_argv(a, 2);
    if (cmd_argc(a) <= 2 || input == NULL || output == NULL) {     /* OG gate: 2 < argc */
        sh_printf("sh_genbmodel <input file> <output file> Generate a bmodel from a .obj/.ase/.lwo file.\n");
        return;
    }

    /* OG (cmd_0xb4a0): ONE 0xD0 struct memset to 0x01 is BModelBuilder arg1/RCX (the out/result struct); ONE
     * default-ctor'd idStr is arg4/R9. There is NO separate options buffer -- the 0x01-memset IS arg1. (The
     * first port inverted arg1/arg4: it passed a 0-memset buffer as arg1 + the 0x01 buffer as arg4, leaving
     * the idStr unused. Fixed to match OG byte-for-byte.) */
    unsigned char out208[GEN_BOPTS_BYTES]; memset(out208, 0x01, sizeof out208);  /* OG: memset(auStack_e8, 1, 0xd0) -> arg1 */
    unsigned char optsS [GEN_IDSTR_BYTES]; memset(optsS,  0,    sizeof optsS);    /* OG auStack_118: the idStr -> arg4 */

    if (!eng_default_idstr_ctor(optsS)) {
        sh_printf("sh_genbmodel: idStr ctor unresolved -- cannot generate.\n");
        return;
    }
    int built = eng_bmodel_builder(out208, input, output, optsS);  /* OG: BModelBuilder(auStack_e8, input, output, auStack_118) */
    eng_idstr_dtor(optsS);                  /* OG dtors the idStr after the build */

    if (built)
        sh_printf("sh_genbmodel: generated bmodel '%s' -> '%s'\n", input, output);
    else
        sh_printf("sh_genbmodel: bmodel builder unresolved/faulted ('%s').\n", input);
}

/* ----------------------------------------------------------------- [17] sh_debugrender -------------
 * Port of OG FUN_18001ffe0 (dispatches argv[1] across 9 sub-ops). The OG reads renderWorld from the
 * .data slot *(engineBase+0x57216f0); this resolves the slot instead: the RenderWorldGetter sig anchors a
 * unique window carrying `LEA RCX,[rip+slot]`, sh_decode_rip_slot yields the slot RVA, and one deref
 * gives the live idRenderWorld*. On a miss, glb_resolve("render_world_slot") signs a second, independent
 * site for the same slot. The editor singleton for showcursor comes from glb_resolve("editor_singleton").
 *
 * PORTED, read-only: dumprenderinfo (walk the rendermodel list, Printf each name), showcursor
 * (byte[editor+0x23624]=0), togglefpsupdate (clone-local flag), showmaterial / drawmatarg (idMaterial
 * decl reads). REFUSED with a toast: loadimg_n_break (ends in INT3, halts the game), dump_megatex
 * (hardcoded per-machine fwrite). NOT AVAILABLE, surfaced but not ported: test_rm_commit, test_sum_shit,
 * testnewgui -- render-commit / geoworld-build / GUI-alloc, outside the read-only scope. */
#define RW_SLOT_KNOWN_RVA         0x57216f0u  /* the renderWorld .data slot's RVA on the pinned Vulkan build
                                               * (OG *(engineBase+RVA)) -- kept for audit and per-build
                                               * re-derivation. Dereferenced only when the host IS that build. */
/* RE-DERIVE RECIPE for the 4 build-specific offsets below. They are vtable-slot and struct-field
 * offsets, so no signature reaches them; both recipes are one command-handler decompile:
 *   - The renderWorld vtbl slots and the model-name offset: decompile the OG `dumpmodelinfo` handler
 *     (via the AddCommand("dumpmodelinfo") xref or its Printf format string). It calls
 *     `rw->vtbl[RW_VSLOT_MODEL_COUNT]()`, then loops `rw->vtbl[RW_VSLOT_GET_MODEL](i)` and reads
 *     `*(char**)(m + RW_MODEL_NAME_OFF)` -- two `call qword[rax+0xNN]` offsets and one `mov rcx,[model+0xNN]`.
 *   - ED_SHOWCURSOR_OFF: decompile the OG `showcursor` handler -> `*(uint8*)(editor + 0xNN) = 0`, with
 *     the editor base from EDITOR_SINGLETON_RVA below.
 * A wrong offset degrades to a bad read on a dev-only command, SEH-guarded. */
#define RW_VSLOT_MODEL_COUNT      0x188       /* renderWorld vtbl -> GetActiveRenderModelCount() -> uint (BUILD-SPECIFIC) */
#define RW_VSLOT_GET_MODEL        0x190       /* renderWorld vtbl -> GetRenderModel(idx) -> model* (=400; BUILD-SPECIFIC) */
#define RW_MODEL_NAME_OFF         0x10        /* render model -> name char* (model+0x10) (BUILD-SPECIFIC) */
#define ED_SHOWCURSOR_OFF         0x23624u    /* editor -> showcursor byte (OG writes 0) (BUILD-SPECIFIC) */
#define RW_MODEL_COUNT_CAP        1000000u    /* stale-renderWorld guard on the model count */
#define EDITOR_SINGLETON_RVA      0x3056748u  /* the inline idSnapEditorLocal object's RVA on the pinned Vulkan
                                               * build (in-place ctor 0x51A8E0) -- kept for audit and per-build
                                               * re-derivation, no longer used to locate anything. showcursor
                                               * finds the object via glb_resolve("editor_singleton") and writes
                                               * byte[editor+0x23624]=0 through that. */

/* GetDeclsOfType typedef already declared above (get_decls_fn); reuse it for the material lookups. */

/* Resolve the live idRenderWorld* build-portably: decode the RenderWorldGetter sig's RIP slot, deref once;
 * failing that, the signed "render_world_slot" anchor. The pinned RVA is consulted only on the pinned build.
 * Returns NULL if none yields a readable non-NULL pointer -- the caller prints "renderWorld not available". */
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
    /* Last resort, and only on the build the literal was extracted from: elsewhere it reads unrelated
     * memory and we would hand back a bogus idRenderWorld* the caller cannot distinguish from a real one. */
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
/* SEH-guarded write of byte[editor+0x23624]=0 (showcursor). The editor object is located by the signed
 * "editor_singleton" anchor; if that does not resolve we DECLINE rather than write through a pinned RVA,
 * because on any other build that address is unrelated engine state and this is a WRITE. Returns 1 if the
 * write ran, 0 if we could not locate the editor (the caller then prints "showcursor unavailable"). */
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
/* SEH-guarded GetDeclsOfType("idMaterial") + a no-op presence probe (the OG also did a name lookup we do
 * not need to mutate). Returns the decl-list ptr, or NULL. */
static void *dr_material_decls(void)
{
    if (!g_get_decls) return NULL;
    __try { return ((get_decls_fn)g_get_decls)("idMaterial"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* clone-local cosmetic toggle states (the OG flips engine-side debug flags DAT_18003e789/e78b/e78c; the
 * clone keeps the user-facing toggle behavior without poking undocumented engine globals -- faithful surface,
 * no engine mutation beyond the editor showcursor byte). */
static int g_dr_fps_update = 0;

static void h_sh_debugrender(idCmdArgs *a)
{
    const char *sub = cmd_argv(a, 1);
    if (sub == NULL) {
        sh_printf("Internal renderer-test mutators -- not for normal use.\n");
        return;
    }

    /* ---- SAFE READ-ONLY sub-ops ---- */
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

    /* ---- REFUSED (genuinely harmful; clear toast, NOT bug-for-bug) ---- */
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

    /* ---- NOT-AVAILABLE (heavy dev-only mutators, faithfully surfaced, out of the safe read-only scope) ---- */
    if (strcmp(sub, "test_rm_commit") == 0 || strcmp(sub, "test_sum_shit") == 0 ||
        strcmp(sub, "testnewgui") == 0) {
        sh_printf("sh_debugrender: '%s' is an internal render mutator of the original tool -- not ported.\n", sub);
        return;
    }

    sh_printf("sh_debugrender: unknown sub-op '%s'.\n", sub);
}

/* ----------------------------------------------------------------- [22] sh -- the SnapStack dispatcher
 * Port of OG XINPUT1_3 FUN_180007620 (the `sh` console command). Gates on the shared UI-interface
 * object: with no interface it reports the OG's "Ui interface doesnt exist yet!". Otherwise it looks the
 * subcommand up in the interface's runtime cmd-map (interface+0x58) and runs the handler INLINE.
 *
 * INLINE EXECUTION IS THE POINT (issue #61, a deliberate divergence from the OG -- see
 * docs/fidelity.md). This callback is an engine Cbuf command, so the engine invokes it on DOOM's MAIN
 * thread at ExecuteCommandBuffer -- the decl-safe exec point the clone_bss_apply drain and the decl
 * server also use. A SnapStack op's serialize, JSON patch, decl commit, selection writes and toast then
 * land on the engine's own thread as one unit, and the applied count stays synchronous. The OG enqueued
 * {handler,args} onto the interface work-queue instead, drained by its frontend's worker thread
 * (+0x1a0), because its handlers touched Qt objects owned by that thread; calling engine decl code from
 * there is the defect behind the #56/#59 faults. That worker is a plain CreateThread in ui_bridge.c and
 * the engine treats it as foreign. Our handlers touch no UI-thread-affine state, so nothing needs the
 * bounce.
 *
 * argv keeps the OG shape -- the subcommand's own tail, so argv[0] is the subcommand. The handler call
 * is SEH-guarded, a miss reports the OG "Command %s has not been registered yet", and a bare `sh`
 * mirrors the OG usage hint. */
static void h_sh_dispatch(idCmdArgs *a)
{
    sh_iface *iface = sh_ui_get_iface();
    if (iface == NULL) {
        sh_printf("Ui interface doesnt exist yet!\n");
        return;
    }

    const char *sub = cmd_argv(a, 1);
    if (sub == NULL) {
        sh_printf("Dispatches a Snapmap+ command\n");   /* the OG's usage line said "snaphak" -- renamed */
        return;
    }

    /* Look the subcommand up in the interface's runtime cmd-map (obj+0x58, the registrar-populated map). */
    sh_cmd_handler handler = NULL;
    void          *ctx     = NULL;
    if (!sh_iface_lookup_cmd(iface, sub, &handler, &ctx) || handler == NULL) {
        sh_printf("Command %s has not been registered yet\n", sub);   /* OG miss path */
        return;
    }

    /* Build the SUBCOMMAND's argv from the console idCmdArgs (skip argv[0]="sh"). The engine-owned
     * strings are valid for the duration of this callback, and the handler runs inside it, so no copy
     * is needed. */
    int total = cmd_argc(a);
    int sub_argc = total > 1 ? total - 1 : 0;          /* drop the leading "sh" */
    const char *sub_argv[64];
    if (sub_argc > 64) sub_argc = 64;
    for (int i = 0; i < sub_argc; i++) {
        const char *v = cmd_argv(a, i + 1);            /* a->argv[1..] = the subcommand + its args */
        sub_argv[i] = v ? v : "";
    }

    /* RUN INLINE on this thread -- DOOM's main thread at the command-exec point (see the doc comment
     * above for why this replaced the OG's enqueue-to-worker dispatch). */
    __try {
        handler(ctx, sub_argc, sub_argv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sh_printf("sh %s: handler faulted (recovered; the op did not complete)\n", sub);
    }
}

/* ----------------------------------------------------------------- [12] sh_superscriptop ----------
 * Port of OG XINPUT1_3 FUN_180026450 (cmd_0x26650 gates it on argv[1]=="genevents"). Puts every
 * EV_<name>/<eventnum> and its arg-spec on the clipboard as C #defines. All hops ride sh_typeinfo's one
 * declMgr accessor, so no new signature [DIRECT, this DOOMx64vk build, base 0x140000000]:
 *   declMgr = sh_typeinfo_get_declmgr();                  // accessor @ base+0x17F7030 (lazy-init singleton)
 *   evMgr   = (*(*declMgr + 0x90))(declMgr);              // declMgr vtbl +0x90 -> evMgr sub-object @ declMgr+0x1A0
 *   count   = (*(*evMgr  + 0x28))(evMgr);                 // evMgr vtbl +0x28 -> event count
 *   for i in 0..count-1:
 *     name  = (*(*evMgr + 0x20))(evMgr, i);               // evMgr vtbl +0x20 getByIndex -> rec+0x00, the NAME char*
 *     rec   = (*(*evMgr + 0x10))(evMgr, name);            // evMgr vtbl +0x10 findByName -> the eventDef record
 *     fspec = *(rec + 0x10);                              // the ';'-delimited arg-spec char*
 *
 * eventDef record [DIRECT, registrar @0x17f7140 + slot accessors]: +0x00 name char*, +0x10 fspec char*,
 * +0x2c fspec strlen, +0x30 numArgs, +0x34 eventnum. eventnum == the array index i, because the registrar
 * stores rec at array[eventnum] and increments per registration.
 *
 * The OG sprintf (@0x26450 L45) passes 4 conversions and 2 varargs, so its FSPEC_%s and "%s" read
 * register garbage. This resolves all four (name, i, name, fspec); an unreadable or NULL fspec emits an
 * empty arg-spec.
 *
 * BUILD-SPECIFIC: the accessor RVA, the vtbl slots and the rec+0x10 offset are this build's
 * event-manager layout -- re-derive per build from the declMgr ctor FUN_1417f6c70's two vtables
 * (PTR_FUN_14270b958 / PTR_FUN_14270c2c8). Every hop is SEH-guarded and non-null gated, so a wrong slot
 * degrades to "event manager unavailable" or a per-event skip. */
#define SS_EVMGR_ACCESSOR_VSLOT   0x90    /* declMgr vtbl -> evMgr sub-object accessor (BUILD-SPECIFIC) */
#define SS_EV_COUNT_VSLOT         0x28    /* evMgr   vtbl -> event count                (BUILD-SPECIFIC) */
#define SS_EV_GETNAME_VSLOT       0x20    /* evMgr   vtbl -> name-by-index (char*)       (BUILD-SPECIFIC) */
#define SS_EV_FINDBYNAME_VSLOT    0x10    /* evMgr   vtbl -> record-by-name              (BUILD-SPECIFIC) */
#define SS_REC_FSPEC_OFF          0x10    /* eventDef record -> fspec char* (arg-spec)   (BUILD-SPECIFIC) */
#define SS_EV_COUNT_CAP           65536u  /* stale/garbage-evMgr guard (registrar caps the table at 0x1000) */
#define SS_DUMP_CAP               0x40000 /* accumulation buffer (~256 KiB; ~1k events * ~200 B each) */

typedef void *(*ss_evmgr_acc_fn)(void *declmgr);            /* (*declMgr+0x90)(declMgr) -> evMgr */
typedef unsigned (*ss_count_fn)(void *evmgr);              /* (*evMgr+0x28)(evMgr) -> count */
typedef const char *(*ss_getname_fn)(void *evmgr, unsigned i); /* (*evMgr+0x20)(evMgr,i) -> name char* */
typedef void *(*ss_findbyname_fn)(void *evmgr, const char *nm);/* (*evMgr+0x10)(evMgr,name) -> record */

/* SEH-guarded single vtable-slot call wrappers (the evMgr/declMgr shape is engine-owned; never trust it).
 * Each reads *obj (the vtable), then the fn ptr at vtbl+slot, calls it; NULL/0 on any fault. */
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

/* [12] sh_superscriptop -- dump the engine's event definitions to the clipboard as C #defines. Real
 * port of OG FUN_180026450 (sprintf bug fixed: all 4 fields resolved -- name, eventnum, name, fspec). */
static void h_sh_superscriptop(idCmdArgs *a)
{
    (void)a;   /* OG gates on argv[1]=="genevents"; the clone wires this handler directly to the command */

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
    if (count > SS_EV_COUNT_CAP) {            /* stale/garbage-evMgr guard */
        sh_printf("sh_superscriptop: event count implausible (stale event manager?).\n");
        return;
    }

    static char dump[SS_DUMP_CAP];
    size_t dlen = 0;
    dump[0] = '\0';
    char line[1024];

    /* The OG opened with an eventdef_ss_t struct-comment header (in the OG decompile @0x26450, L38-40); keep a header
     * that documents the emitted #define pair (intended output, not the OG's struct decl which the OG
     * never actually filled in). */
    ss_dump_append(dump, sizeof dump, &dlen,
        "// snapmap-plus sh_superscriptop -- engine event definitions\n"
        "// EV_<name> = the event number; FSPEC_<name> = its ';'-delimited arg-spec\n");

    unsigned emitted = 0;
    for (unsigned i = 0; i < count; i++) {
        const char *name = ss_call_getname(evmgr, i);
        if (name == NULL || name[0] == '\0') continue;   /* a hole in the table -> skip (no garbage) */

        const char *fspec = NULL;
        void *rec = ss_call_findbyname(evmgr, name);      /* the record the OG fetched but discarded */
        if (rec != NULL) fspec = ss_read_fspec(rec);      /* rec+0x10 -> the arg-spec char* */
        if (fspec == NULL) fspec = "";                    /* no-arg / unreadable -> empty spec (faithful) */

        /* INTENDED output -- all 4 conversions resolved (name, eventnum==i, name, fspec). The OG passed
         * only (name, i) for 4 specifiers; we pass all four properly. */
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

/* ----------------------------------------------------------------- [21] cs_dumpeventdefs ----------
 * Port of the INTENT of OG XINPUT1_3 FUN_18000a1b0 (cmd thunk FUN_180022460). The OG walked a
 * SnapHak-internal eventDef vector the clone never builds and wrote a hardcoded per-machine path; this
 * sources the same data from the ENGINE -- the [12] sh_superscriptop walk -- and writes "eventdefs.txt"
 * in the DOOM cwd, the way [11] cs_start_render_logging writes "renderlog.txt".
 *
 * FILE FORMAT is the OG's own eventdef_ss_t table declaration [reported, sibling FUN_180026450 header
 * L38-40; the [21] formatter FUN_18000a4e0 was never decompiled]:
 *   header  "struct eventdef_ss_t {const char* m_evname;int m_rettype;const char* m_fspec;"
 *           "unsigned m_numargs; unsigned m_eventnum;};\n\tstatic const eventdef_ss_t ALLEVENTS[]={\n"
 *   per ev  "\t{\"<name>\", <rettype>, \"<fspec>\", <numargs>, <eventnum>},\n"
 *   footer  "};\n"
 * fopen mode "w" and one fputs of the whole buffer, matching the OG's shape.
 *
 * Record field map [DIRECT, the [12] eventDef layout]: m_evname <- rec+0x00, m_fspec <- rec+0x10,
 * m_numargs <- rec+0x30, m_eventnum <- rec+0x34. m_rettype has no source in the four fields the engine
 * walk exposes, so it emits 0. eventnum comes from the record rather than the loop index, so a sparse
 * table stays correct.
 *
 * The walk and the file IO are SEH-guarded: a garbage evMgr or record degrades to a per-event skip or an
 * "unavailable" line. Reuses the [12] accessor and slot wrappers, so no new signature. */
#define CDE_REC_EVENTNUM_OFF  0x34    /* eventDef record -> eventnum (uint)   (BUILD-SPECIFIC, [12] layout) */
#define CDE_OUT_PATH          "eventdefs.txt"   /* sane path (DOOM cwd); adapts OG's hardcoded chrispy path */

/* SEH-guarded read of the eventnum record field cs_dumpeventdefs emits (the [12] clipboard path never needed
 * it). NOTE m_numargs is NOT read from the record: rec+0x30 reads 0 on this build (the engine derives the arg
 * count from the fspec at use-time), so cs_dumpeventdefs derives m_numargs from the fspec instead. */
static unsigned cde_read_eventnum(void *rec, unsigned fallback)
{
    __try { return *(const unsigned *)((const uint8_t *)rec + CDE_REC_EVENTNUM_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}

/* SEH-guarded single fputs of the accumulated buffer (a malformed buffer/fp must never fault the editor). */
static int cde_write_file(FILE *fp, const char *buf)
{
    __try { fputs(buf, fp); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* [21] cs_dumpeventdefs -- dump the engine event definitions to a FILE as the OG eventdef_ss_t table. Real
 * port of the OG INTENT (engine-sourced, sane path), not the OG's SnapHak-internal-cache walk. */
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
    if (count > SS_EV_COUNT_CAP) {            /* stale/garbage-evMgr guard (same cap as [12]) */
        sh_printf("cs_dumpeventdefs: event count implausible (stale event manager?).\n");
        return;
    }

    static char dump[SS_DUMP_CAP];            /* ~256 KiB; ~1.6k events * ~100 B/row fits */
    size_t dlen = 0;
    dump[0] = '\0';
    char line[1024];

    /* The OG eventdef-table header (verbatim from FUN_180026450 L38-40 -- the OG's own declaration). */
    ss_dump_append(dump, sizeof dump, &dlen,
        "struct eventdef_ss_t {const char* m_evname;int m_rettype;const char* m_fspec;"
        "unsigned m_numargs; unsigned m_eventnum;};\n"
        "\tstatic const eventdef_ss_t ALLEVENTS[]={\n");

    unsigned emitted = 0;
    for (unsigned i = 0; i < count; i++) {
        const char *name = ss_call_getname(evmgr, i);
        if (name == NULL || name[0] == '\0') continue;    /* a hole in the table -> skip (no garbage) */

        const char *fspec   = NULL;
        unsigned    numargs = 0;
        unsigned    eventnum = i;                          /* dense-table fallback if rec unreadable */
        void *rec = ss_call_findbyname(evmgr, name);       /* the record the OG cache walk formatted */
        if (rec != NULL) {
            fspec    = ss_read_fspec(rec);                 /* rec+0x10 -> the ';'-delimited arg-spec */
            eventnum = cde_read_eventnum(rec, i);          /* rec+0x34 (authoritative event number) */
        }
        if (fspec == NULL) fspec = "";                     /* no-arg / unreadable -> empty spec (faithful) */
        /* m_numargs: derive from the fspec (count the ';'-delimited arg tokens) -- rec+0x30 reads 0 on this
         * build (the engine derives it from the fspec at use-time), and the fspec is authoritative. */
        for (const char *cp = fspec; *cp; cp++) if (*cp == ';') numargs++;

        /* m_rettype: not sourceable from the engine record fields the walk exposes -> 0 (faithful
         * placeholder, as the OG struct reserved the slot). */
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "\t{\"%s\", 0, \"%s\", %u, %u},\n", name, fspec, numargs, eventnum);
        ss_dump_append(dump, sizeof dump, &dlen, line);
        emitted++;
    }
    ss_dump_append(dump, sizeof dump, &dlen, "};\n");      /* footer (close the ALLEVENTS[] table) */

    FILE *fp = NULL;
    if (fopen_s(&fp, CDE_OUT_PATH, "w") != 0 || fp == NULL) {    /* "w" -- faithful to the OG fopen mode */
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

/* The entity/spawn handlers live in entity.c -- they need the gameMgr global + the
 * FindEntity/GetOrigin/ExecuteCommandText vtable slots + SpawnByEntityDef (all cached by
 * sh_entity_install). Extern-declared here so CMD_TABLE can reference them without drift; they share
 * sh_commands' idCmdArgs/cmd_argv/sh_printf via commands.h. */
void h_sh_dumpdef(idCmdArgs *a);
void h_sh_spawninfo(idCmdArgs *a);
void h_sh_spawn(idCmdArgs *a);
void h_sh_dumpmap(idCmdArgs *a);   /* T5 -- real port in entity.c (MapGetter+MapWriter, reuses gameMgr) */
/* The 5 player-cheat commands (OG/DLM parity -- DLM's dinput8 adds them; stock SnapMap lacks them) live in
 * entity.c: each toggles one runtime bit on the local idPlayer (FindEntity("player1")). */
void h_noclip(idCmdArgs *a);
void h_infinitehealth(idCmdArgs *a);
void h_noplayerdeath(idCmdArgs *a);
void h_noplayerkill(idCmdArgs *a);
void h_notarget(idCmdArgs *a);

/* The type-introspection handlers live in typeinfo.c -- they reach the reflection/type-info
 * manager via the hardcoded declMgr accessor RVA 0x17F7030 (+vtable+0x80) + FindTypeInfoByName /
 * FindEnumByName (all cached by sh_typeinfo_install). Extern-declared here so CMD_TABLE can reference them
 * without drift; they share sh_commands' idCmdArgs/cmd_argv/sh_printf via commands.h. */
void h_cs_fieldinfo(idCmdArgs *a);
void h_sh_type(idCmdArgs *a);
void h_sh_validclasses(idCmdArgs *a);

/* snaphak_algo handlers live in algo.c -- h_cs_dontuse [18] toggles the 4 f64 math overrides on/off;
 * h_alginfo (sh_alginfo) reports the reimpl PRESENT. Their engine deps (the module base for resolving the
 * 4 AlgoMatMul/Inverse/PackRGBA/CurveEval sigs at FIRE) are cached by sh_algo_install (dllmain). Extern-
 * declared here so CMD_TABLE references them without drift; they share sh_commands' idCmdArgs/sh_printf. */
void h_cs_dontuse(idCmdArgs *a);
void h_alginfo(idCmdArgs *a);

/* sh_target_any: the editor-decl visibility toggle (target_any.c -> h_target_any), a pair-for-pair port of
 * OG SnapHak's own sh_target_any (FUN_180021EE0) -- it flips the visibility pair (bits 7-6 of decl+0x3CD)
 * over every idDeclSnapEditorEntity decl to reveal / re-hide the normally-hidden placeable entity decls.
 * GetDeclsOfType is handed to it by sh_target_any_install (dllmain). Extern-declared here (matching
 * target_any.h) so CMD_TABLE references it without drift; it shares sh_commands' idCmdArgs/sh_printf. */
void h_target_any(idCmdArgs *a);

/* ------------------------------------------------------------------------ the command table -------
 * From the OG XINPUT1_3.dll string table (read 2026-06-21), with two deliberate post-rebrand
 * divergences: the OG's snapHak_/snaphak_ command-name prefixes are renamed to sh_*, and the original
 * author's personal name is scrubbed from the live help strings (the dev-only commands say "internal"
 * instead). sh_target_any carries lightly reworded help but the OG behavior (the editor-decl
 * visibility toggle, target_any.c). Order mirrors the [1]-[22] command numbering. sh_help (at the
 * end) is OUR OWN addition. */
typedef struct cmd_entry {
    const char *name;
    void       *handler;
    const char *help;
} cmd_entry;

static void h_sh_help(idCmdArgs *a);   /* defined after CMD_TABLE (it walks the table) */

/* sh_dialogtest [buttonset] [text...] -- raise the engine's own modal with our text.
 *
 * A diagnostic: which button LAYOUT a button-set value draws lives in the Flash layer and cannot be
 * read from native code. The button set is a free parameter of the raise, not a property of the GDM id,
 * so sweeping it here is how the yes/no value gets identified. Which button was PRESSED needs no
 * sweeping -- the engine reports it through the button's action id, which `sh_dialogpoll` reads.
 *
 * Everything after the button set is joined back into one string, since the tokeniser would otherwise
 * pass only the first word of a message. */
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
    /* <gdmid> <buttonset> <text...>, both numeric and both optional-from-the-left.
     * The GDM id matters as much as the button set: the shell picks a dialog's
     * personality from the id, so a notice-shaped id draws one button no matter
     * what button set it is handed. Sweeping both is the only way to find the
     * pair that asks a real question. */
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

/* sh_dialogdump -- print every descriptor currently in the engine's dialog queue.
 *
 * This is what makes the surface legible: the engine's OWN dialogs pass through
 * the same queue, so a known yes/no prompt raised by the game shows which button
 * set draws that layout, which is otherwise invisible. No byte in a descriptor
 * carries the answer -- that arrives through the button's action id -- so this
 * is a queue inspector and nothing more. */
static void h_sh_dialogdump(idCmdArgs *a)
{
    (void)a;
    sh_engine_dialog_dump(sh_printf);
}

/* sh_navmesh -- what the current map's baked navigation is serving, and for the
 * modules it is not serving, why.
 *
 * Without this the feature is invisible: a refusal is a line in the backend log
 * that nobody reads until after they have chased a phantom AI bug, and a map
 * that is working looks exactly like a map that is silently falling back to its
 * shipped navmesh. */
static void h_sh_navmesh(idCmdArgs *a)
{
    (void)a;
    sh_navmesh_report(sh_printf);
    /* The two halves of the feature reported together: navigation a map CARRIES
     * as shards, then navigation it DESCRIBES through marked volumes. Composed
     * here rather than by either module, so neither has to know about the other. */
    sh_nav_bake_report(sh_printf);
}

static const cmd_entry CMD_TABLE[] = {
    { "sh_rawmaps",           (void *)h_sh_rawmaps,   "Raw JSON map files: state, paths, load, save. Run with no arguments to see what is set." },
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
    /* The 5 player-cheat commands (OG/DLM parity): DLM's dinput8 adds these to SnapMap; we reproduce them
     * clean-room (toggle one runtime bit on the local idPlayer -- entity.c). Match OG's names exactly. */
    { "noClip",              (void *)h_noclip,         "Toggle noclip (no-collision flight) for the local player." },
    { "infiniteHealth",      (void *)h_infinitehealth, "Toggle infinite health for the local player." },
    { "noPlayerDeath",       (void *)h_noplayerdeath,  "Toggle no-death (the player cannot die) for the local player." },
    { "noPlayerKill",        (void *)h_noplayerkill,   "Toggle no-kill (the player cannot be killed) for the local player." },
    { "noTarget",            (void *)h_notarget,       "Toggle notarget (enemies ignore the local player)." },
    { "sh_user_overrides", (void *)h_sh_user_overrides,
      "sh_user_overrides [0|1] -- persist whether player override files load on the next DOOM launch; restart required; built-in defaults stay enabled." },
    { "sh_navmesh",          (void *)h_sh_navmesh,
      "Reports the baked AI navigation the current map is serving -- which modules and nav classes, or why a bake was refused." },
    /* OUR OWN addition (no OG counterpart): one place that lists the whole Snapmap+ console surface. */
    { "sh_help",             (void *)h_sh_help,        "Lists every Snapmap+ console command and cvar with its description." },
};
#define CMD_COUNT ((int)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])))

/* sh_help -- print the full Snapmap+ console surface: every CMD_TABLE command (name + help) and every
 * cvar table row (name + default + description). The help strings are the same ones registered with
 * the engine; this just puts them in ONE listing (the engine's own listCmds buries them among
 * thousands of engine commands). */
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

/* ====================================================================== command unlock ===========
 * Make EVERY console command usable once a developer command (e.g. `god`) flips developer mode on.
 *
 * THE PROBLEM. DOOM splits commands across a two-table developer gate, like cvars: a fresh console
 * scans the FULL list (cmdSys+0x08), but once dev mode is on the console scans the DEV list
 * (cmdSys+0x20) and applies a cheat guard (`ExecuteCommandText` 0x1aa4950 throws unless
 * cmd->flags@+0x20 & 2). The engine's native cheats and the clone's own commands register without the
 * dev flag, so they read "Unknown command" right after `god`.
 *
 * THE FIX, the same one the original mod's dinput8 uses: detour the engine AddCommand (0x1aa3630) and
 * OR flags|6 (0x2 cheat-exempt | 0x4 dev-table membership) into every registration, so the engine's own
 * AddCommand inserts into BOTH tables and grows each list's own buffer. Two halves: (1) the detour, for
 * all future registrations including the gameplay commands that only register on level load; (2) a
 * one-time pass over commands already registered before the detour installed -- OR flags|6 and insert
 * into the DEV list through the engine's own idList grow.
 *
 * The two backing arrays must stay separate. Aliasing the DEV idList onto the FULL array makes
 * AddCommand's DEV-append write at the wrong index and duplicate or lose commands.
 *
 * Offsets DIRECT from the AddCommand (0x1aa3630) + ExecuteCommandText (0x1aa4950) decompiles:
 *   cmdSys: FULL idList {array@+0x08, count@+0x10}, DEV idList {array@+0x20, count@+0x28, cap@+0x2c}.
 *   idCommand (operator_new(0x28)): name@0, handler@8, argComp@0x10, help@0x18, flags@+0x20. */
#define CMD_FULL_ARRAY_OFF  0x08u
#define CMD_FULL_COUNT_OFF  0x10u
#define CMD_DEV_ARRAY_OFF   0x20u
#define CMD_DEV_COUNT_OFF   0x28u
#define CMD_DEV_CAP_OFF     0x2cu
#define CMD_OBJ_FLAGS_OFF   0x20u
#define CMD_DEV_FLAGS       0x6u        /* 0x2 cheat-exempt | 0x4 dev-table membership */
#define CMD_COUNT_SANITY    100000u

/* idList grow (engine FUN_140699a60): ensures room for one more element on the idList at `list`
 * (granularity-or-double then idList::Resize, the engine allocator).
 *
 * NOT signature-resolvable: it is one of 1,560 byte-identical instantiations of the same idList
 * template, differing only in rip-relative and rel32 displacements, so no prologue pattern separates
 * them.
 *
 * Resolved RELATIONALLY off AddCommand, which IS signature-resolved: AddCommand calls this on
 * cmdSys+0x08 before appending, and that call site is the pair `LEA RCX,[RSI+8]` / `CALL rel32`.
 * Scanning AddCommand's body for those five bytes and decoding the displacement finds the callee on any
 * build. A miss degrades to a skipped insert; the caller SEH-guards. */
#define IDLIST_GROW_RVA     0x699a60u   /* pinned-build value -- cross-check only, never used to locate */
typedef void (*idlist_grow_fn)(void *idlist);

/* Decode the idList-grow callee out of AddCommand's body. Returns NULL if the call site is not found
 * within the scanned window or the decoded target lands outside the DOOM module. */
static idlist_grow_fn sh_decode_idlist_grow(void *add_command, const uint8_t *module_base)
{
    if (!add_command || !module_base) return NULL;

    /* `LEA RCX,[RSI+8]` = 48 8D 4E 08, then E8 rel32. The first such pair in AddCommand is the FULL
     * list; the DEV one (LEA RCX,[RSI+0x20]) calls the same function. 256 bytes covers both on the
     * pinned build, where the first pair starts at +0xBF (its CALL opcode is at +0xC3 -- the logged
     * offset below is the start of the 9-byte window, not the call). */
    const uint8_t *p = (const uint8_t *)add_command;
    for (unsigned i = 0; i + 9 <= 256; ++i) {
        uint8_t win[9];
        if (!sh_safe_read(p + i, win, sizeof win)) return NULL;
        if (win[0] != 0x48 || win[1] != 0x8D || win[2] != 0x4E || win[3] != 0x08 || win[4] != 0xE8)
            continue;
        int32_t rel;
        memcpy(&rel, win + 5, sizeof rel);
        const uint8_t *tgt = p + i + 9 + rel;

        /* Range-check against the module before handing back something that will be CALLED. */
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

/* The AddCommand detour: OR flags|6 then call through the trampoline (= the original mod's
 * `or [rsp+0x30],6`). 6-arg passthrough; flags is the 6th (stack) arg. */
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

/* SEH-guarded: is `cmd` already in the DEV idList? (A torn read -> treat as present, i.e. skip.) */
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

/* SEH-guarded: append `cmd` to the DEV idList, growing via the engine's OWN idList grow if full.
 * Mirrors AddCommand's DEV-append exactly (engine-managed buffer; never shares the FULL array). */
static void cmd_dev_append(uint8_t *cmdSys, void *cmd, idlist_grow_fn grow)
{
    __try {
        uint32_t count = *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF);
        uint32_t cap   = *(uint32_t *)(cmdSys + CMD_DEV_CAP_OFF);
        if (count >= cap) {
            if (!grow) return;                       /* can't grow safely -> skip (never corrupt) */
            grow(cmdSys + CMD_DEV_ARRAY_OFF);        /* engine realloc; array + cap move */
            count = *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF);
            cap   = *(uint32_t *)(cmdSys + CMD_DEV_CAP_OFF);
        }
        if (count < cap) {
            void **dev = *(void ***)(cmdSys + CMD_DEV_ARRAY_OFF);   /* re-read after grow */
            if (dev != NULL) {
                dev[count] = cmd;
                *(uint32_t *)(cmdSys + CMD_DEV_COUNT_OFF) = count + 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* skip on fault */ }
}

/* One-time pass: OR flags|6 on every FULL command + insert any not yet in DEV. Catches every command
 * registered BEFORE our detour installed (the engine's core commands). Returns the count walked. */
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

/* Install the command unlock: detour AddCommand (all FUTURE registrations) + a one-time pass over the
 * commands already registered. Idempotent-latched by the caller (one-shot). cmdsys/add_command already
 * resolved; module_base anchors the engine idList-grow. */
static void sh_command_unlock_install(void *cmdsys, void *add_command, const uint8_t *module_base)
{
    if (cmdsys == NULL || add_command == NULL) {
        backend_log("B2: command-unlock SKIPPED -- cmdsys/AddCommand unresolved");
        return;
    }
    idlist_grow_fn grow = sh_decode_idlist_grow(add_command, module_base);

    /* (1) detour AddCommand: every FUTURE registration (incl. gameplay commands on level load) gets
     *     flags|6, so the engine's own AddCommand inserts it into BOTH tables, growing properly. */
    void *tramp = install_inline_hook(add_command, (void *)hook_add_command, ADDCMD_STOLEN);
    if (tramp != NULL) {
        g_addcmd_tramp = (add_command6_fn)tramp;
        backend_log("B2: command-unlock -- AddCommand detour installed (flags|6 on every registration)");
    } else {
        backend_log("B2: command-unlock -- AddCommand detour FAILED (one-time pass still runs)");
    }

    /* (2) one-time pass over already-registered commands (the engine's core set): OR flags|6 + insert
     *     into the DEV list via the engine's own idList grow. */
    uint32_t walked = command_unlock_pass((uint8_t *)cmdsys, grow);
    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: command-unlock APPLIED -- %u commands now in DEV table + cheat-exempt (god/noclip/give stay usable after dev mode toggles)",
        walked);
    backend_log(line);
}

/* Register one command through the 6-arg engine AddCommand. flags=2 is developer-EXEMPT: AddCommand
 * stores it as 6 (0x2|0x4), which appends the command into the FULL table (+0x08) AND the DEV table
 * (+0x20) and passes ExecuteCommandText's cheat guard, so it stays typeable whether or not dev mode is
 * on. The engine's own always-typeable commands (`where`, `getviewpos`) use flags=2 too.
 *   [DIRECT] ExecuteCommandText 0x141aa4950 (gate getter *(cmdSys+0x200a8): 0 => FULL@+0x08, else
 *   DEV@+0x20); AddCommand 0x141aa3630 (`flags|4 if flags&2`); cheat guard 0x1419fcb60.
 *   A divergence from the OG, which passes no flag at all and leaves its own commands dev-gated. */
static int register_cmd(const cmd_entry *e)
{
    __try {
        g_add_command(g_cmdsys, e->name, e->handler, e->help, NULL, 2u);   /* flags=2 -> FULL+DEV + cheat-exempt */
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sh_commands_install(void *add_command, void *cmdsys, void *printf_disp, void *get_decls,
                        const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;   /* one-shot */

    if (!add_command) { backend_log("B2: commands SKIPPED -- AddCommand unresolved"); return 0; }
    if (!printf_disp) { backend_log("B2: commands SKIPPED -- Printf unresolved"); return 0; }
    if (!cmdsys)      { backend_log("B2: commands SKIPPED -- cmdSystem unresolved"); return 0; }

    g_add_command = (add_command_fn)add_command;
    g_cmdsys      = cmdsys;
    g_printf      = (printf_dispatch_fn)printf_disp;
    g_get_decls   = get_decls;
    g_module_base = module_base;   /* devmode [15][16] resolve SessionDevModeGetter at FIRE off this base */

    int n = 0;
    for (int i = 0; i < CMD_COUNT; i++)
        if (register_cmd(&CMD_TABLE[i])) n++;

    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: registered %d/%d console commands (cmdsys=%p add=%p printf=%p)",
        n, CMD_COUNT, cmdsys, add_command, printf_disp);
    backend_log(line);

    /* Command unlock: detour AddCommand (flags|6 on every future registration) + a one-time pass over
     * the already-registered set, so every command stays usable once a dev command flips dev mode.
     * Runs AFTER our own registrations are in the FULL table (the pass mirrors them into DEV too). */
    sh_command_unlock_install(g_cmdsys, (void *)g_add_command, g_module_base);
    return n;
}
