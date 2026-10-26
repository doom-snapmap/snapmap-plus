/* Minimal x64 instruction-length decoder for prologue stolen windows. */
#include "x64_len.h"

/* ModRM (+SIB, +displacement) length starting at p; flags RIP-relative. */
static size_t modrm_len(const uint8_t *p, size_t avail, unsigned *flags)
{
    uint8_t mod, rm;
    size_t n = 1;
    if (avail < 1) return 0;
    mod = (uint8_t)(p[0] >> 6);
    rm = (uint8_t)(p[0] & 7);
    if (mod == 3) return 1;
    if (rm == 4) {
        if (avail < 2) return 0;
        n = 2;
        if (mod == 0 && (p[1] & 7) == 5) n += 4;
    } else if (mod == 0 && rm == 5) {
        n += 4;
        *flags |= SH_X64_RIP_RELATIVE;
    }
    if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n <= avail ? n : 0;
}

/* Two-byte (0F xx) opcodes that take a ModRM and no immediate. */
static int twobyte_modrm(uint8_t op)
{
    return (op >= 0x10 && op <= 0x18) || op == 0x1F ||
           (op >= 0x28 && op <= 0x2F) || (op >= 0x40 && op <= 0x6F) ||
           (op >= 0x74 && op <= 0x76) || op == 0x7E || op == 0x7F ||
           (op >= 0x90 && op <= 0x9F) || op == 0xA3 || op == 0xA5 ||
           op == 0xAB || op == 0xAD || op == 0xAF || op == 0xB0 || op == 0xB1 ||
           op == 0xB3 || op == 0xB6 || op == 0xB7 || (op >= 0xBB && op <= 0xBF) ||
           op == 0xC0 || op == 0xC1 || op == 0xC3 || op == 0xC7 ||
           (op >= 0xD0 && op <= 0xFE);
}

/* Two-byte opcodes that take a ModRM and an imm8. */
static int twobyte_modrm_imm8(uint8_t op)
{
    return (op >= 0x70 && op <= 0x73) || op == 0xA4 || op == 0xAC ||
           op == 0xBA || op == 0xC2 || (op >= 0xC4 && op <= 0xC6);
}

size_t sh_x64_insn_len(const uint8_t *code, size_t avail, unsigned *flags)
{
    size_t i = 0, m, imm = 0;
    int opsize16 = 0, rex_w = 0, prefixes = 0;
    unsigned local = 0;
    uint8_t op;

    if (flags) *flags = 0;
    if (!code) return 0;
    for (;;) {
        if (i >= avail || prefixes > 4) return 0;
        op = code[i];
        if (op == 0x66) opsize16 = 1;
        else if (op == 0x67 || op == 0xF2 || op == 0xF3 || op == 0xF0 ||
                 op == 0x2E || op == 0x3E || op == 0x26 || op == 0x36 ||
                 op == 0x64 || op == 0x65) { /* ignored legacy prefix */ }
        else break;
        i++;
        prefixes++;
    }
    if (op >= 0x40 && op <= 0x4F) {
        rex_w = (op & 8) != 0;
        if (++i >= avail) return 0;
        op = code[i];
    }
    i++;

    if (op == 0x0F) {
        uint8_t op2;
        if (i >= avail) return 0;
        op2 = code[i++];
        if (op2 == 0x38 || op2 == 0x3A) {
            if (i >= avail) return 0;
            i++;
            if (!(m = modrm_len(code + i, avail - i, &local))) return 0;
            i += m;
            if (op2 == 0x3A) imm = 1;
        } else if (op2 >= 0x80 && op2 <= 0x8F) {
            imm = 4;
            local |= SH_X64_BRANCH_REL;
        } else if (op2 == 0x05 || op2 == 0x0B || op2 == 0x31 || op2 == 0xA2 ||
                   (op2 >= 0xC8 && op2 <= 0xCF)) {
            /* syscall, ud2, rdtsc, cpuid, bswap: no further operands */
        } else if (twobyte_modrm(op2)) {
            if (!(m = modrm_len(code + i, avail - i, &local))) return 0;
            i += m;
        } else if (twobyte_modrm_imm8(op2)) {
            if (!(m = modrm_len(code + i, avail - i, &local))) return 0;
            i += m;
            imm = 1;
        } else {
            return 0;
        }
    } else if (op < 0x40) {
        uint8_t low = (uint8_t)(op & 7);
        if (low >= 6) return 0;   /* segment pushes, prefixes, invalid BCD ops */
        if (low < 4) {
            if (!(m = modrm_len(code + i, avail - i, &local))) return 0;
            i += m;
        } else {
            imm = low == 4 ? 1 : (opsize16 ? 2 : 4);
        }
    } else if (op >= 0x50 && op <= 0x5F) {
        /* push/pop reg */
    } else if (op == 0x63 || (op >= 0x84 && op <= 0x8F) || (op >= 0xD0 && op <= 0xD3) ||
               op == 0xFE || op == 0xFF) {
        if (op == 0xFF) {
            uint8_t reg;
            if (i >= avail) return 0;
            reg = (uint8_t)((code[i] >> 3) & 7);
            if (reg == 7) return 0;
            if (reg == 4 || reg == 5) local |= SH_X64_LEAVES;
        }
        if (!(m = modrm_len(code + i, avail - i, &local))) return 0;
        i += m;
    } else if (op == 0x69 || op == 0x81 || op == 0xC7) {
        if (!(m = modrm_len(code + i, avail - i, &local))) return 0;
        i += m;
        imm = opsize16 ? 2 : 4;
    } else if (op == 0x6B || op == 0x80 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6) {
        if (!(m = modrm_len(code + i, avail - i, &local))) return 0;
        i += m;
        imm = 1;
    } else if (op == 0xF6 || op == 0xF7) {
        uint8_t reg;
        if (i >= avail) return 0;
        reg = (uint8_t)((code[i] >> 3) & 7);
        if (!(m = modrm_len(code + i, avail - i, &local))) return 0;
        i += m;
        if (reg < 2) imm = op == 0xF6 ? 1 : (opsize16 ? 2 : 4);
    } else if (op == 0x68) {
        imm = 4;
    } else if (op == 0x6A || op == 0xA8 || (op >= 0xB0 && op <= 0xB7)) {
        imm = 1;
    } else if (op == 0xA9) {
        imm = opsize16 ? 2 : 4;
    } else if (op >= 0xB8 && op <= 0xBF) {
        imm = rex_w ? 8 : (opsize16 ? 2 : 4);
    } else if (op >= 0x70 && op <= 0x7F) {
        imm = 1;
        local |= SH_X64_BRANCH_REL;
    } else if (op == 0xEB) {
        imm = 1;
        local |= SH_X64_BRANCH_REL | SH_X64_LEAVES;
    } else if (op == 0xE8) {
        imm = 4;
        local |= SH_X64_BRANCH_REL;
    } else if (op == 0xE9) {
        imm = 4;
        local |= SH_X64_BRANCH_REL | SH_X64_LEAVES;
    } else if (op == 0xC3 || op == 0xCC) {
        local |= SH_X64_LEAVES;
    } else if (op == 0xC2) {
        imm = 2;
        local |= SH_X64_LEAVES;
    } else if ((op >= 0x90 && op <= 0x99) || op == 0x9C || op == 0x9D ||
               op == 0xFC || op == 0xFD) {
        /* nop/xchg rax, cbw/cwd, pushf/popf, cld/std */
    } else {
        return 0;
    }
    i += imm;
    if (i > avail || i > 15) return 0;
    if (flags) *flags = local;
    return i;
}

size_t sh_x64_stolen_boundary(const uint8_t *code, size_t avail, size_t min_len,
                              size_t max_len, unsigned *window_flags)
{
    size_t at = 0;
    unsigned all = 0;
    if (window_flags) *window_flags = 0;
    if (!code || min_len == 0 || max_len < min_len) return 0;
    if (avail > max_len) avail = max_len;
    while (at < min_len) {
        unsigned f = 0;
        size_t n = sh_x64_insn_len(code + at, avail - at, &f);
        if (!n) return 0;
        all |= f;
        at += n;
        if ((f & SH_X64_LEAVES) && at < min_len) return 0;
    }
    if (window_flags) *window_flags = all;
    return at;
}
