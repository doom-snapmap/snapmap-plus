/* attach_tags_sigs_test.c -- the tag-info lookup detour (src/backend/attach_tags_sigs.c, attach_tags.c) on each
 * build 13954591 executable named on the command line: it resolves at its recorded RVA with a unique 48-byte pattern,
 * and its 15-byte window is the three register saves then the idList walk (count at +0x08 compared against zero),
 * whole instructions with no RIP-relative operand or relative branch. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "attach_tags.h"

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

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, hits[4], rva, w = 0;
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    const sig_entry *s = &SH_ATTACH_TAGS_SIGNATURES[0];
    sig_result r;
    static const uint8_t saves[SH_AT_STOLEN] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                                 0x48, 0x89, 0x74, 0x24, 0x18 };
    static const uint8_t walk[6] = { 0x8B, 0xEF, 0x39, 0x79, 0x08, 0x7E };   /* mov ebp,edi; cmp [rcx+8],edi; jle */
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    printf("image %s: %s\n", path, identity->label);
    CHECK(SH_ATTACH_TAGS_SIGNATURES[1].name == NULL);
    rva = identity->vulkan ? s->rva_vk : s->rva_gl;
    CHECK(rva == (identity->vulkan ? 0x16CE1E0u : 0x16CEE90u));
    CHECK(sig_resolve_one(base, s, &r) == SIG_OK && r.rva == rva);
    CHECK(scan(base, s->pattern, hits, 4) == 1 && hits[0] == rva);
    /* the prologue is the window's three saves, then the walk of the first argument's idList */
    CHECK(!memcmp(base + rva, saves, sizeof saves));
    CHECK(!memcmp(base + rva + 41, walk, sizeof walk));
    /* the window */
    while (w < SH_AT_STOLEN) {
        unsigned flags = 0;
        size_t len = sh_x64_insn_len(base + rva + w, 32, &flags);
        if (!len) break;
        CHECK(!(flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)));
        w += (uint32_t)len;
    }
    CHECK(w == SH_AT_STOLEN);
    printf("  %-18s rva=0x%X window=%u\n", s->name, rva, (unsigned)SH_AT_STOLEN);
    free(base);
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("attach_tags_sigs_test: no images given; skipped\n"); return 0; }
    /* each image in its own process: the host binding is the first image's */
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "attach_tags_sigs_test: FAIL\n" : "attach_tags_sigs_test: PASS\n");
    return g_fail;
}
