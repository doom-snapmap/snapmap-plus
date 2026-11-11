/* script_ai_sigs_test.c -- the engine sites the AI calls of mod scripts
 * depend on (src/backend/script/script_ai_sigs.c, script_api_ai.c), checked
 * on each build 13954591 executable given on the command line:
 *   - every private signature resolves to one match at its recorded RVA;
 *   - IsRunningScript's detour window is whole, position-independent
 *     instructions and is the bytes the installer compares;
 *   - the path site's calls and load reach the path key, the path query and
 *     the query's first word at the recorded addresses;
 *   - idAI2's GetType (vtable slot 0) returns the typeinfo the demon check
 *     reads;
 *   - the move FSM status (Shared_MoveFailedAny) and the weapon trigger
 *     (Shared_TriggerPulled) are read at the offsets the calls use.
 * Run via run-tests.ps1 with -Doom / -DoomAlt. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "script/script_api_ai.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static uint8_t *map_pe_by_rva(const char *path, uint32_t *image_size)
{
    FILE *f = fopen(path, "rb");
    long fsz;
    uint8_t *file, *img;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    *image_size = nt->OptionalHeader.SizeOfImage;
    img = (uint8_t *)calloc(1, nt->OptionalHeader.SizeOfImage);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz)
            memcpy(img + va, file + po, rs);
    }
    free(file);
    return img;
}

static uint32_t rel_target(const uint8_t *base, uint32_t at, uint32_t len, uint32_t disp_off)
{
    int32_t d;
    memcpy(&d, base + at + disp_off, 4);
    return (uint32_t)(at + len + d);
}

static uint64_t qword_at(const uint8_t *base, uint32_t rva)
{
    uint64_t v;
    memcpy(&v, base + rva, 8);
    return v;
}

static int window_has(const uint8_t *base, uint32_t from, uint32_t len, const char *bytes, size_t n)
{
    uint32_t k;
    for (k = from; k + n <= from + len; k++) if (!memcmp(base + k, bytes, n)) return 1;
    return 0;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, rva[SH_AI_SIG_COUNT];
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    IMAGE_NT_HEADERS *nt;
    uint64_t image_base;
    int vk, i;

    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    vk = identity->vulkan;
    printf("image %s: %s\n", path, identity->label);
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    image_base = nt->OptionalHeader.ImageBase;

    for (i = 0; SH_AI_SIGNATURES[i].name; i++) {}
    CHECK(i == SH_AI_SIG_COUNT);
    for (i = 0; i < SH_AI_SIG_COUNT; i++) {
        const sig_entry *e = &SH_AI_SIGNATURES[i];
        sig_result r;
        rva[i] = 0;
        if (sig_resolve_one(base, e, &r) != SIG_OK || !r.addr) {
            printf("FAIL %s did not resolve to a unique match\n", e->name);
            g_fail = 1;
            continue;
        }
        CHECK(r.rva == (vk ? e->rva_vk : e->rva_gl));
        rva[i] = r.rva;
        printf("  %-20s rva=0x%X\n", e->name, r.rva);
    }

    /* The detour window. */
    if (rva[SH_AI_SIG_IS_RUNNING_SCRIPT]) {
        unsigned flags = 0;
        size_t b = sh_x64_stolen_boundary(base + rva[SH_AI_SIG_IS_RUNNING_SCRIPT], 64, 14, 48, &flags);
        CHECK(b == SH_AI_IRS_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + rva[SH_AI_SIG_IS_RUNNING_SCRIPT], k_ai_irs_prologue, SH_AI_IRS_STOLEN));
    }

    /* The path site's targets. */
    if (rva[SH_AI_SIG_PATH_SITE]) {
        uint32_t s = rva[SH_AI_SIG_PATH_SITE];
        uint32_t key = rel_target(base, s + SH_AI_PATH_SITE_KEY_CALL, 5, 1);
        uint32_t glob = rel_target(base, s + SH_AI_PATH_SITE_GLOBAL_LOAD, 7, 3);
        uint32_t query = rel_target(base, s + SH_AI_PATH_SITE_QUERY_CALL, 5, 1);
        CHECK(base[s + SH_AI_PATH_SITE_KEY_CALL] == 0xE8 && base[s + SH_AI_PATH_SITE_QUERY_CALL] == 0xE8);
        CHECK(!memcmp(base + s + SH_AI_PATH_SITE_GLOBAL_LOAD, "\x48\x8B\x0D", 3));
        printf("  path key 0x%X, path query 0x%X, query word 0x%X\n", key, query, glob);
        CHECK(key == (vk ? 0x3D54C0u : 0x3D49C0u));
        CHECK(query == (vk ? 0x515300u : 0x514030u));
        CHECK(glob == (vk ? 0x5B64F00u : 0x446A580u));
        /* the path key indexes the list at ai+0x2AA88 */
        CHECK(!memcmp(base + key, "\x40\x53\x48\x83\xEC\x20\x48\x8B\x81\x88\xAA\x02\x00", 13));
        /* the query reads the move FSM's owner origin (+0x808, slot +0x80) */
        CHECK(window_has(base, query, 0x200, "\xFF\x90\x80\x00\x00\x00", 6));
    }

    /* idAI2's GetType: lea rax,[rip+typeinfo]; ret. */
    {
        uint32_t vt = vk ? 0x2035B30u : 0x204F900u, get_type, ti;
        get_type = (uint32_t)(qword_at(base, vt) - image_base);
        CHECK(get_type == (vk ? 0x391970u : 0x391240u));
        CHECK(base[get_type] == 0x48 && base[get_type + 1] == 0x8D && base[get_type + 2] == 0x05 && base[get_type + 7] == 0xC3);
        ti = rel_target(base, get_type, 7, 3);
        printf("  idAI2 typeinfo 0x%X\n", ti);
        CHECK(ti == (vk ? 0x5CF7A90u : 0x45FD110u));
    }

    /* Offsets: Shared_MoveFailedAny reads *(ai+0x23198)+0xD20 against 5;
     * Shared_TriggerPulled reads ai+0x22278, weapon +0x8D4 and +0x8F8. */
    {
        uint32_t mfa = vk ? 0x5EF6F0u : 0x62CD00u, tp = vk ? 0x5F7BD0u : 0x5F6DF0u;
        CHECK(window_has(base, mfa, 0x90, "\x48\x8B\x86\x98\x31\x02\x00\x83\xB8\x20\x0D\x00\x00\x05", 14));
        CHECK(window_has(base, tp, 0x80, "\x49\x8B\x80\x78\x22\x02\x00", 7));
        CHECK(window_has(base, tp, 0x80, "\x48\x63\x81\xD4\x08\x00\x00\x39\xBC\x81\xF8\x08\x00\x00", 14));
    }
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("script_ai_sigs_test: no images given; skipped\n"); return 0; }
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "script_ai_sigs_test: FAIL\n" : "script_ai_sigs_test: PASS\n");
    return g_fail;
}
