/* vt_harness.h -- lane RC research harness: private virtual-texture family
 * records and the native mega2 page decoder, driven from the console.
 * Research branch lane/rc-vt-harness only; never merged into the product.
 *
 *   sh_vt status                                  bindings and their addresses
 *   sh_vt list                                    the 31 page-source records
 *   sh_vt register <name> <pagesPath> [flavor]    SetSource(free record, name, path, flavor, 2, 0, NULL)
 *   sh_vt release <name>                          release a record this harness registered
 *   sh_vt decode <family> <level> <x> <y> <out>   decode one stored page to five planes
 *
 * Engine surface (build 13954591, both renderers):
 *   VtSetSource      idVirtualTexture::SetSource(rec, name, path, flavor, type, direct, reader)
 *                    VK 0x185AC30, GL 0x185BC90. Its head holds LEA RCX,[sys]; CALL; MOV RCX,RSI;
 *                    CALL Release, which give the VT system object and Release (VK 0x1859480,
 *                    GL 0x185A460) without a second signature.
 *   VtForMaterial    VirtualTextureForMaterial(sys, material), VK 0x17E8230, GL 0x17E9100: its
 *                    MOV RAX,[R13+disp32] gives the record pointer table (VK sys+0x8B00, GL
 *                    sys+0xA400; 32 slots, records 1..31), and on OpenGL its sparse test gives
 *                    the two globals that turn a material's nonzero flavor into 4.
 *   Mega2PageDecode  decode(header16, payload, NULL, out 0x50000), VK 0x1A4BC80, GL 0x1A64C70.
 * Record fields used (equal on both renderers): +0 type, +8 table owner (rows at owner+0x138,
 * count +0x140), +0x10 provider header copy (magic, +0x1C page grid, +0x24 levels), +0x70 path,
 * +0x1A0 name, +0x2D0 .pages file, +0x2E8 reader, +0x2F0 mega2 reader, +0x2FC texel size,
 * +0x3D8 page-table object (its first int is the flavor), +0x3E8 page-table image,
 * +0x3F8 host page table.
 */
#ifndef SH_VT_HARNESS_H
#define SH_VT_HARNESS_H

#include <stdint.h>
#include <stddef.h>
#include "signatures.h"
#include "commands.h"

#define SH_VT_CMD_NAME "sh_vt"
#define SH_VT_CMD_HELP "Lane RC virtual-texture harness (research build): 'sh_vt status', 'sh_vt list', " \
                       "'sh_vt register <name> <pagesPath> [flavor]', 'sh_vt release <name>', " \
                       "'sh_vt decode <family> <level> <x> <y> <out>'."

/* Bind the engine surface. Logs one VT: line per binding. Returns 1 when
 * record registration is available (decode is bound independently). */
int sh_vt_harness_install(const sig_result *results, size_t count, const uint8_t *module_base);

void h_sh_vt(idCmdArgs *args);

/* Pure helpers, exposed for the unit test. */

/* Find "48 8D 0D d32 E8 r32 48 8B CE E8 r32" in code[0..len): returns the
 * offset of the LEA, else -1. */
int sh_vt_find_setsource_head(const uint8_t *code, size_t len);

/* Find "49 8B 85 d32 E9" in code[0..len): returns the disp32 (table + 8), else 0. */
uint32_t sh_vt_find_table_disp(const uint8_t *code, size_t len);

/* Swap the five big-endian u16 stream lengths at +4..+0xC of a stored page
 * header in place, as the engine's page job setup does before decoding. */
void sh_vt_swap_page_header(uint8_t *page16);

/* Locate the index cell of (level, x, y) in a member header (0x170 bytes):
 * returns the cell number, or -1 when the member does not cover it. */
long sh_vt_member_cell(const uint8_t *header, int level, unsigned x, unsigned y);

#endif
