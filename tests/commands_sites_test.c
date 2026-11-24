/* commands_sites_test.c -- the console install's byte decoders (commands_decode.c, the code the DLL runs) against
 * each build 13954591 executable given on the command line:
 *   - AddCommand: sh_decode_idlist_grow finds the LEA RCX,[RSI+8]; CALL in its body and the target lies in the
 *     image's code;
 *   - CmdSystemLea: sh_decode_rip_slot decodes a pointer slot in the image's data;
 *   - SessionDevModeGetter: its head bytes are the ones sh_disable_devmode patches over, and its displacement is
 *     SH_SESSION_DEVMODE_FIELD.
 * Run via run-tests.ps1 with -Doom / -DoomAlt; each image in its own process. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "engine_build.h"
#include "image_sites.h"

void backend_log(const char *message) { (void)message; }

/* The image section holding an RVA: its characteristics, or 0 when none. */
static DWORD section_flags(const uint8_t *base, const void *address)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t rva;
    int i;
    if ((const uint8_t *)address < base) return 0;
    rva = (uintptr_t)((const uint8_t *)address - base);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (rva >= sec[i].VirtualAddress && rva < (uintptr_t)sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
            return sec[i].Characteristics;
    return 0;
}

static int synthetic_main(void)
{
    /* A window with the AddCommand shape decodes; one without it does not. */
    uint8_t code[64], *tgt;
    int bad = 0;
    memset(code, 0x90, sizeof code);
    code[8] = 0x48; code[9] = 0x8D; code[10] = 0x4E; code[11] = 0x08; code[12] = 0xE8;
    tgt = code + 40;
    {
        int32_t rel = (int32_t)(tgt - (code + 17));
        memcpy(code + 13, &rel, 4);
    }
    /* sh_decode_idlist_grow scans 256 bytes: give it a padded buffer */
    {
        uint8_t big[512];
        memset(big, 0x90, sizeof big);
        memcpy(big, code, sizeof code);
        if (sh_decode_idlist_grow(big, big) != big + 40) { printf("FAIL idlist-grow decode of a synthetic body\n"); bad = 1; }
        big[10] = 0x4F;
        if (sh_decode_idlist_grow(big, big) != NULL) { printf("FAIL idlist-grow decode accepted a changed byte\n"); bad = 1; }
    }
    {
        uint8_t acc[0x40] = { 0 };
        int32_t disp = 0x100;
        acc[6] = 0x48; acc[7] = 0x8B; acc[8] = 0x0D; memcpy(acc + 9, &disp, 4);
        if (sh_decode_rip_slot(acc) != acc + 13 + 0x100) { printf("FAIL rip-slot decode of a synthetic accessor\n"); bad = 1; }
        acc[7] = 0x89;
        if (sh_decode_rip_slot(acc) != NULL) { printf("FAIL rip-slot decode accepted a store\n"); bad = 1; }
    }
    printf(bad ? "commands_sites_test: synthetic FAIL\n" : "commands_sites_test: synthetic ok\n");
    return bad;
}

static void check_image(const char *path)
{
    const sh_build_identity *identity;
    uint8_t *base = image_open(path, &identity);
    sig_result *results;
    size_t n, cap = sig_db_count();
    if (!base) return;
    results = (sig_result *)calloc(cap, sizeof *results);
    if (!results) { g_image_fail = 1; free(base); return; }
    n = sig_resolve_all(base, results, cap);
    {
        const uint8_t *add_command = image_site(base, results, n, "AddCommand", identity->vulkan);
        const uint8_t *cmdsys_lea = image_site(base, results, n, "CmdSystemLea", identity->vulkan);
        const uint8_t *devmode = image_site(base, results, n, "SessionDevModeGetter", identity->vulkan);
        if (add_command) {
            const uint8_t *grow = (const uint8_t *)sh_decode_idlist_grow((void *)add_command, base);
            printf("  idList grow decoded from AddCommand: rva 0x%X\n", grow ? (unsigned)(grow - base) : 0u);
            IMG_CHECK(grow != NULL);
            IMG_CHECK(grow && (section_flags(base, grow) & IMAGE_SCN_MEM_EXECUTE));
        }
        if (cmdsys_lea) {
            const uint8_t *slot = sh_decode_rip_slot(cmdsys_lea);
            printf("  command system slot decoded from CmdSystemLea: rva 0x%X\n", slot ? (unsigned)(slot - base) : 0u);
            IMG_CHECK(slot != NULL);
            IMG_CHECK(slot && (section_flags(base, slot) & IMAGE_SCN_MEM_WRITE) && !(section_flags(base, slot) & IMAGE_SCN_MEM_EXECUTE));
        }
        if (devmode) {
            uint32_t field = sh_devmode_getter_field(devmode);
            printf("  dev-mode getter reads session+0x%X\n", (unsigned)field);
            IMG_CHECK(memcmp(devmode, SH_DEVMODE_GETTER_HEAD, sizeof SH_DEVMODE_GETTER_HEAD) == 0);
            IMG_CHECK(field == SH_SESSION_DEVMODE_FIELD);
        }
    }
    free(results);
    (void)sh_host_test_bind_image(NULL, NULL);
    free(base);
}

IMAGE_TEST_MAIN("commands_sites_test", check_image, synthetic_main)
