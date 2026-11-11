/* script_api_sigs_test.c -- the engine sites the mod-script API calls
 * (src/backend/script/script_api_common.c), checked on each build 13954591
 * executable given on the command line:
 *   - every private signature resolves to one match at its recorded RVA;
 *   - the class vtables the API compares against carry their RTTI names
 *     (idAI2, idPlayer, idAIHealth), and idAIHealth's +0x58 is Heal and +0xD8
 *     the GetHealth that reads a float at +0x1C;
 *   - idGameLocal vtable +0x220 (the game clock) forwards to the time object
 *     at +0x49D210, slot +0xC8;
 *   - the trace query's immediate branch tests the result pointer and the
 *     game's trace manager is the one at idGameLocal +0xE0878 the laser
 *     hazard passes (lea/add rcx,0xE0878 before its call).
 * Run via run-tests.ps1 with -Doom / -DoomAlt; each image in its own process. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "script/script_api_common.h"

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
    int i;
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
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz) memcpy(img + va, file + po, rs);
    }
    free(file);
    return img;
}

static uint64_t qword_at(const uint8_t *base, uint32_t rva)
{
    uint64_t v;
    memcpy(&v, base + rva, 8);
    return v;
}

/* The RTTI type name of the vtable at `vt`. */
static const char *rtti_name(const uint8_t *base, uint64_t image_base, uint32_t vt, uint32_t size)
{
    uint64_t col = qword_at(base, vt - 8) - image_base;
    uint32_t td;
    if (col + 0x10 > size) return "";
    memcpy(&td, base + col + 0x0C, 4);
    return td + 0x20 < size ? (const char *)base + td + 0x10 : "";
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, rva[8];
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    IMAGE_NT_HEADERS *nt;
    uint64_t image_base;
    int vk, i, n;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    vk = identity->vulkan;
    printf("image %s: %s\n", path, identity->label);
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    image_base = nt->OptionalHeader.ImageBase;

    for (n = 0; SH_SCRIPT_API_SIGNATURES[n].name; n++) {
        const sig_entry *e = &SH_SCRIPT_API_SIGNATURES[n];
        sig_result r;
        rva[n] = 0;
        if (sig_resolve_one(base, e, &r) != SIG_OK || !r.addr) {
            printf("FAIL %s did not resolve to a unique match\n", e->name);
            g_fail = 1;
            continue;
        }
        CHECK(r.rva == (vk ? e->rva_vk : e->rva_gl));
        rva[n] = r.rva;
        printf("  %-24s rva=0x%X\n", e->name, r.rva);
    }
    CHECK(n == 8);

    /* class vtables */
    {
        uint32_t ai = vk ? 0x2035B30u : 0x204F900u, player = vk ? 0x22A1298u : 0x22BAFC8u;
        uint32_t health = vk ? 0x2035820u : 0x204F5F0u;
        uint32_t heal = vk ? 0x425640u : 0x4246E0u, get_health = vk ? 0x6C3430u : 0xDD0170u;
        CHECK(!strcmp(rtti_name(base, image_base, ai, size), ".?AVidAI2@@"));
        CHECK(!strcmp(rtti_name(base, image_base, player, size), ".?AVidPlayer@@"));
        CHECK(!strcmp(rtti_name(base, image_base, health, size), ".?AVidAIHealth@@"));
        CHECK(qword_at(base, health + 0x58) == image_base + heal);
        CHECK(qword_at(base, health + 0xD8) == image_base + get_health);
        CHECK(!memcmp(base + get_health, "\xF3\x0F\x10\x41\x1C", 5));   /* movss xmm0,[rcx+0x1C] */
        /* Heal stores the game clock's int: call [rax+0x220]; mov [rbx+0xBC],eax */
        {
            uint32_t k;
            int found = 0;
            for (k = heal; k < heal + 0x180; k++)
                if (!memcmp(base + k, "\xFF\x90\x20\x02\x00\x00\x89\x83\xBC\x00\x00\x00", 12)) found = 1;
            CHECK(found);
        }
    }

    /* the game clock: idGameLocal vtable +0x220 -> add rcx,0x49D210; mov rax,[rcx]; jmp [rax+0xC8] */
    {
        uint32_t game_vt = vk ? 0x201E3D8u : 0x20381A8u;
        uint64_t t = qword_at(base, game_vt + 0x220) - image_base;
        CHECK(t < size && !memcmp(base + t, "\x48\x81\xC1\x10\xD2\x49\x00\x48\x8B\x01\x48\xFF\xA0\xC8\x00\x00\x00", 17));
        /* the difficulty: idGameLocal vtable +0x670 holds ScriptDifficulty, which reads the
         * g_gameDifficulty cvar's int (+0x30) and falls back to currentGameDifficulty (+0x2C3010) */
        if (rva[5]) {
            CHECK(qword_at(base, game_vt + 0x670) - image_base == rva[5]);
            CHECK(!memcmp(base + rva[5] + 0x25, "\x8B\x42\x30", 3));
            CHECK(!memcmp(base + rva[5] + 0x3E, "\x8B\x83\x10\x30\x2C\x00", 6));
        }
    }

    /* the trace: the laser hazard's call site passes idGameLocal + 0xE0878 and
     * a 0x103085 contents mask to the query */
    if (rva[4]) {
        uint32_t site = vk ? 0x5A0C68u : 0x59FE68u;   /* call ScriptTraceQuery in the laser hazard */
        int32_t rel;
        uint32_t k;
        int traces = 0, mask = 0;
        CHECK(base[site] == 0xE8);
        memcpy(&rel, base + site + 1, 4);
        CHECK(site + 5 + rel == rva[4]);
        for (k = site - 0x50; k < site; k++) {
            if (!memcmp(base + k, "\x48\x81\xC1\x78\x08\x0E\x00", 7)) traces = 1;
            if (!memcmp(base + k, "\x85\x30\x10\x00", 4)) mask = 1;
        }
        CHECK(traces && mask);
        /* the immediate branch: test r8,r8 (the result) then jnz */
        CHECK(!memcmp(base + rva[4] + 0x1C, "\x4D\x85\xC0\x0F\x85", 5));
    }
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("script_api_sigs_test: no images given; skipped\n"); return 0; }
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "script_api_sigs_test: FAIL\n" : "script_api_sigs_test: PASS\n");
    return g_fail;
}
