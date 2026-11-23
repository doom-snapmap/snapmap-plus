/* commands_decode.c -- the byte decoders the console install and its handlers run on engine code: the guarded
 * reader, the RIP-slot decode of an accessor, the idList-grow call out of AddCommand, and the dev-mode getter's
 * field. They live apart from commands.c (which links the whole backend) so commands_sites_test runs these same
 * functions against each shipped executable. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "commands.h"
#include "backend_log.h"

/* Guarded byte reads shared by global decoders and entity handlers. */
int sh_safe_read(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Decode the first RIP-relative MOV/LEA to RAX/RCX within the accessor window.
 * Return the pointer slot, not the object it contains. */
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

/* Decode the idList-grow callee out of AddCommand's body. Returns NULL if the call site is not found
 * within the scanned window or the decoded target lands outside the DOOM module. */
void *sh_decode_idlist_grow(void *add_command, const uint8_t *module_base)
{
    if (!add_command || !module_base) return NULL;

    /* Find LEA RCX,[RSI+8]; E8 rel32 in the first 256 bytes. The developer-list
     * variant uses +0x20 and calls the same growth helper. */
    const uint8_t *p = (const uint8_t *)add_command;
    for (unsigned i = 0; i + 9 <= 256; ++i) {
        uint8_t win[9];
        if (!sh_safe_read(p + i, win, sizeof win)) return NULL;
        if (win[0] != 0x48 || win[1] != 0x8D || win[2] != 0x4E || win[3] != 0x08 || win[4] != 0xE8)
            continue;
        int32_t rel;
        memcpy(&rel, win + 5, sizeof rel);
        const uint8_t *tgt = p + i + 9 + rel;

        /* Validate callable addresses against the mapped image. */
        uint8_t probe;
        if (tgt < module_base || !sh_safe_read(tgt, &probe, 1)) return NULL;
        return (void *)tgt;
    }
    backend_log("commands: the list-growth call was not found in AddCommand; no developer-list backfill");
    return NULL;
}

/* SessionDevModeGetter's head: movzx eax, byte [rcx+disp32]. sh_disable_devmode patches over exactly these. */
const uint8_t SH_DEVMODE_GETTER_HEAD[3] = { 0x0F, 0xB6, 0x81 };

/* The session field the getter's displacement names (0 when unreadable). */
uint32_t sh_devmode_getter_field(const uint8_t *getter)
{
    uint32_t field = 0;
    __try { memcpy(&field, getter + 3, sizeof field); }
    __except (EXCEPTION_EXECUTE_HANDLER) { field = 0; }
    return field;
}
