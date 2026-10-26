/* Minimal x64 instruction-length decoder for function prologues.
 * It covers the legacy one- and two-byte opcode maps that compiled prologues
 * use (prefixes, REX, ModRM/SIB/displacement, immediates, 0F and 0F 38/3A).
 * VEX/EVEX and any unrecognized opcode decode as length 0 so callers refuse
 * instead of cutting through an instruction. */
#ifndef BACKEND_X64_LEN_H
#define BACKEND_X64_LEN_H

#include <stddef.h>
#include <stdint.h>

/* Flags reported for one instruction. */
#define SH_X64_RIP_RELATIVE 1u   /* ModRM mod=00 rm=101: [rip+disp32] */
#define SH_X64_BRANCH_REL   2u   /* rel8/rel32 call or jump */
#define SH_X64_LEAVES       4u   /* ret, int3, unconditional jump: control leaves */

/* Length of the instruction at code (at most avail bytes readable), or 0. */
size_t sh_x64_insn_len(const uint8_t *code, size_t avail, unsigned *flags);

/* First instruction boundary at or beyond min_len, decoding from code.
 * Returns 0 when an instruction is undecodable, the boundary exceeds max_len,
 * or control leaves the function before min_len.
 * *window_flags (optional) receives the OR of every decoded instruction's flags. */
size_t sh_x64_stolen_boundary(const uint8_t *code, size_t avail, size_t min_len,
                              size_t max_len, unsigned *window_flags);

#endif
