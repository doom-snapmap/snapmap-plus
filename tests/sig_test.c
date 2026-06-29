/* sig_test.c -- offline equivalence test for the backend signature resolver (signatures.c).
 *
 * Builds an in-memory IMAGE laid out by RVA from a PE file on disk (each section's raw bytes copied to
 * image[VirtualAddress], exactly how the Windows loader maps it), then runs sig_resolve_all over it --
 * so this exercises the SAME code path the live DLL takes (mapped-image section walk), against the real
 * unpacked DOOM, with no game running. Confirms the C port matches the Python resolver's verdict
 * (25/25 unique, RVAs == known_rva). NOT shipped in the DLL -- a build-time check.
 *
 *   cl /nologo /O2 /MT sig_test.c signatures.c /Fe:sig_test.exe
 *   sig_test.exe <DOOM_unpacked.exe>          # exit 0 iff all sigs resolve uniquely to known RVA
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "signatures.h"

static uint8_t *map_pe_by_rva(const char *path, size_t *image_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)file;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    uint32_t hdr_size   = nt->OptionalHeader.SizeOfHeaders;

    uint8_t *img = (uint8_t *)calloc(1, image_size);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, hdr_size);

    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress;
        uint32_t rs = sec[i].SizeOfRawData;
        uint32_t po = sec[i].PointerToRawData;
        if (rs && va + rs <= image_size && po + rs <= (uint32_t)fsz)
            memcpy(img + va, file + po, rs);
    }
    free(file);
    *image_sz = image_size;
    return img;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: sig_test <DOOM_unpacked.exe>\n"); return 2; }
    size_t image_sz = 0;
    uint8_t *base = map_pe_by_rva(argv[1], &image_sz);
    if (!base) return 2;

    size_t total = sig_db_count();
    sig_result results[64];
    size_t ok = sig_resolve_all(base, results, 64);

    int bad = 0;
    for (size_t i = 0; i < total; i++) {
        const char *st = results[i].status == SIG_OK ? "OK " :
                         results[i].status == SIG_NOT_FOUND ? "NOTFOUND" :
                         results[i].status == SIG_AMBIGUOUS ? "AMBIG" : "BAD";
        uint32_t known = BACKEND_ENGINE_SIGNATURES[i].known_rva;
        int rva_ok = (results[i].status == SIG_OK && results[i].rva == known);
        if (!rva_ok) bad++;
        printf("%s %-20s resolved=0x%-9x known=0x%-9x %s\n",
               rva_ok ? "OK " : "BAD", results[i].name, results[i].rva, known,
               results[i].status == SIG_OK ? "" : st);
    }
    printf("======================================================================\n");
    printf("C resolver: %zu/%zu unique; %d RVA-mismatches\n", ok, total, bad);
    free(base);
    return (ok == total && bad == 0) ? 0 : 1;
}
