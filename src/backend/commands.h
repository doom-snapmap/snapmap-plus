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

/* Resolve cmdSystem via the accessor, an independent global anchor, then the
 * Vulkan-name-gated pinned slot. Return NULL when no usable object is found. */
void *sh_resolve_cmdsys(const sig_result *results, size_t n, const uint8_t *module_base);

/* Register the command table once and cache dependencies. Missing AddCommand,
 * cmdsys, or Printf returns 0; get_decls may be NULL and its handlers then refuse.
 * module_base supports on-demand signature lookups. Return the registered count. */
int sh_commands_install(void *add_command, void *cmdsys, void *printf_disp, void *get_decls,
                        const uint8_t *module_base);

#endif /* BACKEND_B2_COMMANDS_H */
