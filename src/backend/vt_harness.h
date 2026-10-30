/* vt_harness.h -- sh_vt, the virtual-texture diagnostic: the page-source
 * records (including the ones modvt.c registers for installed mods), manual
 * registration for format experiments, and the native mega2 page decoder.
 *
 *   sh_vt status                                  bindings, and the product's family status line
 *   sh_vt list                                    records 1..31; (product) marks an installed mod's family
 *   sh_vt sync                                    run the product's family registration now
 *   sh_vt register <name> <pagesPath> [flavor]    SetSource(free record in 1..30, name, path, flavor, 2, 0, NULL)
 *   sh_vt release <name>                          release a record made by sh_vt register
 *   sh_vt decode <family> <level> <x> <y> <out>   decode one stored page to five planes
 *
 * The record surface is modvt.c's (modvt.h lists the addresses and fields);
 * decode uses the Mega2PageDecode signature, decode(header16, payload, NULL,
 * out 0x50000), VK 0x1A4BC80, GL 0x1A64C70, after swapping the big-endian
 * stream lengths as the engine's page job does. A record made by sh_vt
 * register counts as foreign to the product: a mod family of the same name is
 * then refused, and sh_vt release never touches a product record.
 */
#ifndef SH_VT_HARNESS_H
#define SH_VT_HARNESS_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"
#include "commands.h"

#define SH_VT_CMD_NAME "sh_vt"
#define SH_VT_CMD_HELP "Virtual-texture diagnostic: 'sh_vt status', 'sh_vt list', 'sh_vt sync', " \
                       "'sh_vt register <name> <pagesPath> [flavor]', 'sh_vt release <name>', " \
                       "'sh_vt decode <family> <level> <x> <y> <out>'."

/* Bind the decoder. Returns 1 when decode is available (the record verbs
 * follow modvt.c's binding). */
int sh_vt_harness_install(const sig_result *results, size_t count, const uint8_t *module_base);

void h_sh_vt(idCmdArgs *args);

/* Pure helpers, exposed for the unit test. */

/* Swap the five big-endian u16 stream lengths at +4..+0xC of a stored page
 * header in place, as the engine's page job setup does before decoding. */
void sh_vt_swap_page_header(uint8_t *page16);

/* Locate the index cell of (level, x, y) in a member header (0x170 bytes):
 * returns the cell number, or -1 when the member does not cover it. */
long sh_vt_member_cell(const uint8_t *header, int level, unsigned x, unsigned y);

#endif
