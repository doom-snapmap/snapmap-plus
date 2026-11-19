/* sync_clock_sigs_test.c -- the engine site the sync clock detours (src/backend/sync_clock_sigs.c, sync_clock.c),
 * checked on each build 13954591 executable given on the command line:
 *   - the re-anchor resolves at its recorded RVA and its 49-byte pattern is the only match in the executable;
 *   - the shape the detour relies on: the null test and its near jz (the one relative instruction of the window,
 *     relocated), then position-independent pushes; the window ends on an instruction boundary;
 *   - its only callers are the eight calls of the adaptive-tick loop, which pass the precision as the fifth
 *     argument (a byte on the stack).
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
#include "sync_clock.h"

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

/* Every RVA in the executable sections where the pattern matches (up to cap). */
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

/* Calls (E8 rel32) to rva from anywhere in the executable sections. */
static int callers(const uint8_t *base, uint32_t rva, uint32_t *from, int cap)
{
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int i, found = 0;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, size = sec[i].Misc.VirtualSize, k;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < 5) continue;
        for (k = 0; k + 5 <= size; k++) {
            int32_t rel;
            if (base[va + k] != 0xE8) continue;
            memcpy(&rel, base + va + k + 1, 4);
            if ((uint32_t)(va + k + 5 + rel) != rva) continue;
            if (found < cap) from[found] = va + k;
            found++;
        }
    }
    return found;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, hits[4], from[16], rva, loop, w = 0;
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    const sig_entry *s = &SH_SYNC_CLOCK_SIGNATURES[0];
    sig_result r;
    int n, i, rel = 0;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    printf("image %s: %s\n", path, identity->label);
    CHECK(SH_SYNC_CLOCK_SIGNATURES[1].name == NULL);
    rva = identity->vulkan ? s->rva_vk : s->rva_gl;
    loop = identity->vulkan ? 0x38E920u : 0x38E1C0u;
    CHECK(rva == (identity->vulkan ? 0x15D1940u : 0x15D2D60u));
    CHECK(sig_resolve_one(base, s, &r) == SIG_OK && r.rva == rva);
    CHECK(scan(base, s->pattern, hits, 4) == 1 && hits[0] == rva);
    /* the null test and its near jz at the relocated offset */
    CHECK(base[rva] == 0x48 && base[rva + 1] == 0x85 && base[rva + 2] == 0xC9);
    CHECK(base[rva + SH_SC_JCC_OFFSET] == 0x0F && base[rva + SH_SC_JCC_OFFSET + 1] == 0x84);
    /* the window: whole instructions, the jz the only relative one */
    while (w < SH_SC_STOLEN) {
        unsigned flags = 0;
        size_t len = sh_x64_insn_len(base + rva + w, 32, &flags);
        if (!len) break;
        CHECK(!(flags & (SH_X64_RIP_RELATIVE | SH_X64_LEAVES)));
        if (flags & SH_X64_BRANCH_REL) { CHECK(w == SH_SC_JCC_OFFSET); rel++; }
        w += (uint32_t)len;
    }
    CHECK(w == SH_SC_STOLEN && rel == 1);
    /* its callers: the eight calls of the adaptive-tick loop, nothing else */
    n = callers(base, rva, from, 16);
    CHECK(n == 8);
    for (i = 0; i < n && i < 16; i++) CHECK(from[i] > loop && from[i] < loop + 0x300);
    printf("  %-18s rva=0x%X window=%u callers=%d\n", s->name, rva, (unsigned)SH_SC_STOLEN, n);
    free(base);
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("sync_clock_sigs_test: no images given; skipped\n"); return 0; }
    /* each image in its own process: the host binding is the first image's */
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "sync_clock_sigs_test: FAIL\n" : "sync_clock_sigs_test: PASS\n");
    return g_fail;
}
