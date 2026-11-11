/* vt_harness.h -- sh_vt, the virtual-texture diagnostic: the page-source
 * records (including the ones modvt.c registers for installed mods), manual
 * registration for format experiments, and the native mega2 page decoder.
 * Built only into the diagnostic build (build.ps1 -Diag, SH_DIAG); a release
 * build has no sh_vt.
 *
 *   sh_vt status                                  bindings, and the product's board status line
 *   sh_vt list                                    records 1..31; (product) marks a mod's texture board
 *   sh_vt sync                                    run the product's board sync now, releases included
 *   sh_vt register <name> <pagesPath> [flavor]    SetSource(free record in 1..30, name, path, flavor, 2, 0, NULL)
 *   sh_vt release <name>                          release a record made by sh_vt register
 *   sh_vt decode <family> <level> <x> <y> <out>   decode one stored page to five planes
 *
 * The record surface is modvt.c's (modvt.h lists the addresses and fields);
 * decode goes through megapreview.c's sh_mega2_decode_page, the one place the
 * native decoder is prepared and called. A record made by sh_vt register
 * counts as foreign to the product: a mod board of the same name is then
 * refused, and sh_vt release never touches a product record.
 */
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
