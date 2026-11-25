#pragma once
#include "safe_read.h"
#include <string.h>

enum { SH_RIP_RAX = 1, SH_RIP_RCX = 2, SH_RIP_ANY_REG = 255 };
enum { SH_RIP_LEA, SH_RIP_MOV_OR_LEA };

/* Decode the first matching 64-bit RIP-relative reference in the accessor prologue. */
static __inline const uint8_t *sh_rip_slot(const uint8_t *fn, int registers, int opcodes)
{
    uint8_t bytes[64];
    if (!sh_safe_read(fn, bytes, sizeof bytes)) return NULL;
    for (size_t i = 0; i + 7 <= sizeof bytes; i++) {
        uint8_t opcode = bytes[i+1], modrm = bytes[i+2];
        if (bytes[i] != 0x48 || (modrm & 0xc7) != 0x05 || !(registers & (1 << ((modrm >> 3) & 7)))) continue;
        if (opcode != 0x8d && !(opcodes == SH_RIP_MOV_OR_LEA && opcode == 0x8b)) continue;
        int32_t disp;
        memcpy(&disp, bytes + i + 3, sizeof disp);
        return fn + i + 7 + disp;
    }
    return NULL;
}
