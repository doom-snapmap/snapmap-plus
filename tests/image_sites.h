/* image_sites.h -- shared by the bind-site tests that run a product check on each build 13954591 executable named
 * on the command line: map an executable by RVA, bind its identity, and resolve a signature by name the way the
 * boot does (sig_resolve_all), requiring its recorded RVA for the renderer. Test-only; include once per test. */
#ifndef SH_TEST_IMAGE_SITES_H
#define SH_TEST_IMAGE_SITES_H
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"

static int g_image_fail = 0;
#define IMG_CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_image_fail = 1; } } while (0)

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

/* Map the executable and bind its identity; NULL (after a FAIL line) when it is not a supported build. */
static uint8_t *image_open(const char *path, const sh_build_identity **identity)
{
    wchar_t wide[MAX_PATH];
    uint8_t *base = map_pe_by_rva(path);
    if (!base) { printf("FAIL cannot map %s\n", path); g_image_fail = 1; return NULL; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    *identity = sh_host_test_bind_image(base, wide);
    if (!*identity) { printf("FAIL %s is not a supported executable\n", path); g_image_fail = 1; free(base); return NULL; }
    printf("image %s: %s\n", path, (*identity)->label);
    return base;
}

/* A signature's site: SIG_OK at the RVA recorded for this renderer, else NULL after a FAIL line. */
static const uint8_t *image_site(const uint8_t *base, const sig_result *results, size_t n, const char *name, int vulkan)
{
    const sig_result *r = sig_lookup(results, n, name);
    const sig_entry *e = NULL;
    size_t i;
    for (i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++)
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name)) { e = &BACKEND_ENGINE_SIGNATURES[i]; break; }
    IMG_CHECK(e != NULL && r != NULL);
    if (!e || !r) return NULL;
    IMG_CHECK(r->status == SIG_OK && r->rva == (vulkan ? e->rva_vk : e->rva_gl));
    if (r->status != SIG_OK) { printf("  %s: %s\n", name, sig_status_reason(r->status)); return NULL; }
    return base + r->rva;
}

/* Run check_image(path) for each image in its own process; the child is `<exe> --one <image>`. */
#define IMAGE_TEST_MAIN(NAME, CHECK_IMAGE, SYNTHETIC) \
int main(int argc, char **argv) \
{ \
    int i, rc_fail = 0; \
    if (argc == 3 && strcmp(argv[1], "--one") == 0) { CHECK_IMAGE(argv[2]); return g_image_fail; } \
    rc_fail = SYNTHETIC(); \
    for (i = 1; i < argc; i++) { \
        intptr_t rc; \
        fflush(stdout); \
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL); \
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); rc_fail = 1; } \
    } \
    if (argc < 2) printf(NAME ": no images given; image checks skipped\n"); \
    else printf(rc_fail ? NAME ": image checks FAIL\n" : NAME ": image checks PASS\n"); \
    return rc_fail; \
}
#endif
