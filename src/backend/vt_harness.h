/* sh_vt, the virtual-texture diagnostic (status, list, sync, register, release, decode; see SH_VT_CMD_HELP),
 * built only with SH_DIAG. A record made by sh_vt register is foreign to the product: a mod board of the
 * same name is refused, and sh_vt release never touches a product record. */
#ifndef SH_VT_HARNESS_H
#define SH_VT_HARNESS_H

#include <stdint.h>
#include <stddef.h>

#ifdef SH_DIAG
#include "commands.h"

#define SH_VT_CMD_NAME "sh_vt"
#define SH_VT_CMD_HELP "Virtual-texture diagnostic: 'sh_vt status', 'sh_vt list', 'sh_vt sync', " \
                       "'sh_vt register <name> <pagesPath> [flavor]', 'sh_vt release <name>', " \
                       "'sh_vt decode <family> <level> <x> <y> <out>'."

void h_sh_vt(idCmdArgs *args);
#endif

/* Pure helper, exposed for the unit test. Locate the index cell of (level, x, y) in a member header (0x170 bytes):
 * returns the cell number, or -1 when the member does not cover it. */
long sh_vt_member_cell(const uint8_t *header, int level, unsigned x, unsigned y);

#endif
