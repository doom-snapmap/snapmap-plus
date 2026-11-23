/* companion_sites.h -- the byte checks companion_engine.c makes on the engine's code, kept in one header so the
 * install and the image test (tests/companion_test.c, on each build 13954591 executable) run the same code. */
#ifndef SH_COMPANION_SITES_H
#define SH_COMPANION_SITES_H
#include <stdint.h>
#include <string.h>

#define SH_CALLSPAWN_STOLEN 14

/* CallSpawn's first 14 bytes, whole instructions the detour steals: push rbx; sub rsp,0x40; mov rax,[rcx];
 * mov rbx,rcx; call [rax] (GetType). */
static const uint8_t SH_CALLSPAWN_PROLOGUE[SH_CALLSPAWN_STOLEN] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9, 0xFF, 0x10
};

/* 1 when `callspawn` still begins with the prologue the detour steals (no other detour is over it). */
static __inline int sh_companion_callspawn_prologue_ok(const uint8_t *callspawn)
{
    return memcmp(callspawn, SH_CALLSPAWN_PROLOGUE, SH_CALLSPAWN_STOLEN) == 0;
}

/* The idEntity cast the SpawnEntityDef wrapper calls second: an E8 at +0x21 whose rel32 is at +0x22. Returns the
 * cast's address, or NULL when there is no call at +0x21. */
static __inline const uint8_t *sh_companion_spawndef_cast(const uint8_t *wrapper)
{
    int32_t rel;
    if (wrapper[0x21] != 0xE8) return NULL;
    memcpy(&rel, wrapper + 0x22, 4);
    return wrapper + 0x26 + rel;
}
#endif
