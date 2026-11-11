/* Console registration, dispatch, and shared engine-command utilities.
 * AddCommand uses (self,name,handler,help,completion,flags); product commands
 * pass NULL completion and flags=2 for full/developer visibility. */
#ifndef BACKEND_B2_COMMANDS_H
#define BACKEND_B2_COMMANDS_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"

/* Native callback argument layout: argc@0 and argv[N]@+8+8*N. */
typedef struct idCmdArgs {
    int          argc;
    char         _pad[4];
    const char  *argv[1];
} idCmdArgs;

/* SEH-guarded idCmdArgs accessors (the engine hands us the args object; never trust its shape). */
int         cmd_argc(idCmdArgs *a);
const char *cmd_argv(idCmdArgs *a, int n);

/* Console output via the resolved engine Printf dispatch (no-op until sh_commands_install caches it). */
void sh_printf(const char *fmt, ...);

/* Decode the first RIP-relative MOV/LEA to RAX/RCX in the accessor window.
 * Return the slot address, not its object. sh_safe_read guards byte access. */
#define B2_RIP_SCAN_WINDOW 0x40
int            sh_safe_read(const uint8_t *src, uint8_t *dst, size_t n);
const uint8_t *sh_decode_rip_slot(const uint8_t *accessor_fn);

/* Resolve cmdSystem through the signed CmdSystemLea accessor, or return NULL. */
void *sh_resolve_cmdsys(const sig_result *results, size_t n, const uint8_t *module_base);

/* Cache dependencies and install the AddCommand detour that exposes later
 * engine commands; returns 1, or 0 when AddCommand, cmdsys or Printf is
 * missing. get_decls may be NULL and its handlers then refuse. module_base
 * supports on-demand signature lookups. The command table itself is
 * registered by sh_commands_main_frame. */
int sh_commands_install(void *add_command, void *cmdsys, void *printf_disp, void *get_decls,
                        const uint8_t *module_base);

/* Called from the engine's main frame (the fault shield's Frame hook): the
 * first call after sh_commands_install registers the command table and
 * backfills the developer list, since both lists belong to the main thread. */
void sh_commands_main_frame(void);

#endif /* BACKEND_B2_COMMANDS_H */
