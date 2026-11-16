/* particlemotion_sigs_test.c -- the engine functions simulated particle motion binds (particlemotion_sigs.c,
 * particlemotion.c), checked on each build 13954591 executable given on the command line:
 *   - the vertex writer and the origin function resolve at their recorded RVAs, and each pattern is the only
 *     match in the executable;
 *   - the writer's first call is the origin function (the two are the pair the detour relies on);
 *   - the writer's detour window is whole instructions with no RIP-relative operand or relative branch.
 * Run via run-tests.ps1 with -Doom / -DoomAlt; each image in its own process. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "particlemotion.h"

#define WRITE_STOLEN 19      /* particlemotion.c */

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static uint8_t *map_pe_by_rva(const char *path)
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

/* How many places in the executable sections the pattern matches; the first in *first. */
static int scan(const uint8_t *base, const char *p, uint32_t *first)
{
    uint8_t bytes[256], mask[256];
    size_t n = 0, k;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int i, found = 0;
    while (*p && n < sizeof bytes) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { bytes[n] = 0; mask[n++] = 0; p += p[1] == '?' ? 2 : 1; continue; }
        bytes[n] = (uint8_t)(hexval(p[0]) * 16 + hexval(p[1]));
        mask[n++] = 1;
        p += 2;
    }
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, size = sec[i].Misc.VirtualSize;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < n) continue;
        for (k = 0; k + n <= size; k++) {
            const uint8_t *at = base + va + k;
            size_t j;
            if (at[0] != bytes[0]) continue;
            for (j = 0; j < n && (!mask[j] || at[j] == bytes[j]); j++) {}
            if (j < n) continue;
            if (!found) *first = (uint32_t)(va + k);
            found++;
        }
    }
    return found;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint8_t *base = map_pe_by_rva(path);
    const sh_build_identity *identity;
    uint32_t rva[2] = { 0, 0 }, at, w = 0, first_call = 0;
    int vk, i;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    vk = identity->vulkan;
    printf("image %s: %s\n", path, identity->label);
    for (i = 0; i < 2; i++) {
        const sig_entry *s = &SH_PARTICLEMOTION_SIGNATURES[i];
        sig_result r;
        uint32_t hit = 0;
        rva[i] = vk ? s->rva_vk : s->rva_gl;
        CHECK(sig_resolve_one(base, s, &r) == SIG_OK && r.rva == rva[i]);
        CHECK(scan(base, s->pattern, &hit) == 1 && hit == rva[i]);
        printf("  %-20s rva=0x%X\n", s->name, rva[i]);
    }
    CHECK(SH_PARTICLEMOTION_SIGNATURES[2].name == NULL);
    /* the writer's first call is the origin function */
    for (at = rva[0]; at < rva[0] + 0x100 && !first_call; ) {
        unsigned flags = 0;
        size_t len = sh_x64_insn_len(base + at, 16, &flags);
        if (!len) break;
        if (base[at] == 0xE8) {
            int32_t rel;
            memcpy(&rel, base + at + 1, 4);
            first_call = (uint32_t)(at + 5 + rel);
        }
        at += (uint32_t)len;
    }
    CHECK(first_call == rva[1]);
    /* the detour window */
    while (w < WRITE_STOLEN) {
        unsigned flags = 0;
        size_t len = sh_x64_insn_len(base + rva[0] + w, 32, &flags);
        if (!len) break;
        CHECK(!(flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)));
        w += (uint32_t)len;
    }
    CHECK(w == WRITE_STOLEN);
    free(base);
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("particlemotion_sigs_test: no images given; skipped\n"); return 0; }
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "particlemotion_sigs_test: FAIL\n" : "particlemotion_sigs_test: PASS\n");
    return g_fail;
}
