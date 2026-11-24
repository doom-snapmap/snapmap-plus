/* level_templates_sigs_test.c -- the engine sites level_templates detours (src/backend/level_templates_sigs.c,
 * level_templates.c), checked on each build 13954591 executable given on the command line: each walk sequence
 * resolves uniquely at its recorded RVA, both walk calls reach one function (the resource list copy) with the same
 * list manager, and that copy's 22-byte detour window is whole instructions with no RIP-relative operand or relative
 * branch. Run via run-tests.ps1 with -Doom / -DoomAlt; each image in its own process. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "level_templates.h"

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

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t parse(const char *p, uint8_t *bytes, uint8_t *mask, size_t cap)
{
    size_t n = 0;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (n >= cap) return 0;
        if (*p == '?') { bytes[n] = 0; mask[n++] = 0; p += p[1] == '?' ? 2 : 1; continue; }
        bytes[n] = (uint8_t)(hexval(p[0]) * 16 + hexval(p[1]));
        mask[n++] = 1;
        p += 2;
    }
    return n;
}

static int scan(const uint8_t *base, const char *pattern, uint32_t *hits, int cap)
{
    uint8_t bytes[256], mask[256];
    size_t n = parse(pattern, bytes, mask, sizeof bytes), k;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int i, found = 0;
    if (!n) return -1;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, size = sec[i].Misc.VirtualSize;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < n) continue;
        for (k = 0; k + n <= size; k++) {
            const uint8_t *at = base + va + k;
            size_t j;
            if (at[0] != bytes[0]) continue;
            for (j = 0; j < n && (!mask[j] || at[j] == bytes[j]); j++) {}
            if (j < n) continue;
            if (found < cap) hits[found] = (uint32_t)(va + k);
            found++;
        }
    }
    return found;
}

/* The lea rcx,[rip+disp32] seven bytes before a walk call: the list manager it passes. */
static uintptr_t manager_of(const uint8_t *call)
{
    int32_t disp;
    const uint8_t *lea = call - 7;
    if (lea[0] != 0x48 || lea[1] != 0x8D || lea[2] != 0x0D) return 0;
    memcpy(&disp, lea + 3, sizeof disp);
    return (uintptr_t)(lea + 7) + (intptr_t)disp;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, hits[4], w = 0;
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    static const uint8_t prologue[SH_LT_STOLEN] = { 0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
                                                    0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20,
                                                    0xFE, 0xFF, 0xFF, 0xFF };
    sig_result r[2];
    uintptr_t copy, again;
    int i;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    printf("image %s: %s\n", path, identity->label);
    CHECK(SH_LEVEL_TEMPLATES_SIGNATURES[2].name == NULL);
    for (i = 0; i < 2; i++) {
        const sig_entry *s = &SH_LEVEL_TEMPLATES_SIGNATURES[i];
        uint32_t rva = identity->vulkan ? s->rva_vk : s->rva_gl;
        CHECK(sig_resolve_one(base, s, &r[i]) == SIG_OK && r[i].rva == rva);
        CHECK(scan(base, s->pattern, hits, 4) == 1 && hits[0] == rva);
        printf("  %-24s rva=0x%X\n", s->name, rva);
    }
    copy = sh_level_templates_call_target(base + r[0].rva + SH_LT_WALK_CALL);
    again = sh_level_templates_call_target(base + r[1].rva + SH_LT_REWALK_CALL);
    CHECK(copy && copy == again);
    CHECK(manager_of(base + r[0].rva + SH_LT_WALK_CALL) &&
          manager_of(base + r[0].rva + SH_LT_WALK_CALL) == manager_of(base + r[1].rva + SH_LT_REWALK_CALL));
    CHECK(copy - (uintptr_t)base == (identity->vulkan ? 0x1572330u : 0x15734C0u));
    CHECK(!memcmp((const uint8_t *)copy, prologue, sizeof prologue));
    while (w < SH_LT_STOLEN) {
        unsigned flags = 0;
        size_t len = sh_x64_insn_len((const uint8_t *)copy + w, 32, &flags);
        if (!len) break;
        CHECK(!(flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)));
        w += (uint32_t)len;
    }
    CHECK(w == SH_LT_STOLEN);
    printf("  list copy rva=0x%X window=%u\n", (unsigned)(copy - (uintptr_t)base), (unsigned)SH_LT_STOLEN);
    free(base);
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("level_templates_sigs_test: no images given; skipped\n"); return 0; }
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "level_templates_sigs_test: FAIL\n" : "level_templates_sigs_test: PASS\n");
    return g_fail;
}
