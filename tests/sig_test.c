/* Offline signature-resolution checks against a PE mapped by RVA.
 * The image is bound to its SHA-256 identity. On a build 13954591 executable
 * every pattern must match exactly once in the whole image, at the recorded RVA
 * for its renderer, and resolve there; "portable" (or any other image)
 * requires uniqueness only. Run via run-tests.ps1. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "signatures.h"
#include "host_image.h"
#include "companion_engine.h"
#include "modscene_decl.h"

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
    /* Uniqueness is required on each image; recorded-RVA equality only on a
     * supported executable, for its own renderer. */
    int pinned = 1;
    if (argc < 2) {
        fprintf(stderr, "usage: sig_test <DOOM_unpacked.exe> [portable]\n");
        return 2;
    }
    size_t image_sz = 0;
    uint8_t *base = map_pe_by_rva(argv[1], &image_sz);
    if (!base) return 2;

    wchar_t wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wide, MAX_PATH);
    const sh_build_identity *identity = sh_host_test_bind_image(base, wide);
    if (!identity) pinned = 0;
    if (argc >= 3 && strcmp(argv[2], "portable") == 0) pinned = 0;
    printf("image %s: %s, %s mode\n", argv[1],
           identity ? identity->label : "not a supported executable (cross-check only)",
           pinned ? "recorded-RVA" : "portable");

    /* Every entry: the whole-image scan must find exactly one match, on a
     * supported executable at the recorded RVA for its renderer, and the
     * run-time resolution (recorded RVA only) must agree. Both passes are
     * timed: the scan is what a boot used to cost, the recorded check what it
     * costs now. */
    size_t total = sig_db_count();
    sig_result results[SIG_RESULTS_MAX], scanned[SIG_RESULTS_MAX];
    LARGE_INTEGER freq, t0, t1, t2;
    if (total > SIG_RESULTS_MAX) {
        printf("SIGNATURE DB OVERFLOW: %zu entries > SIG_RESULTS_MAX %d -- raise it\n",
               total, (int)SIG_RESULTS_MAX);
        return 1;
    }
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (size_t i = 0; i < total; i++) sig_scan_one(base, &BACKEND_ENGINE_SIGNATURES[i], &scanned[i]);
    QueryPerformanceCounter(&t1);
    size_t ok = sig_resolve_all(base, results, SIG_RESULTS_MAX);
    QueryPerformanceCounter(&t2);
    printf("timing: whole-image scan of %zu signatures %.1f ms; recorded-RVA check %.3f ms\n", total,
           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart,
           (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart);
    if (!pinned) ok = 0;

    int bad = 0;
    for (size_t i = 0; i < total; i++) {
        const char *st = scanned[i].status == SIG_OK ? "" :
                         scanned[i].status == SIG_NOT_FOUND ? "NOTFOUND" :
                         scanned[i].status == SIG_AMBIGUOUS ? "AMBIG" : "BAD";
        uint32_t known = identity ? (identity->vulkan ? BACKEND_ENGINE_SIGNATURES[i].rva_vk
                                                      : BACKEND_ENGINE_SIGNATURES[i].rva_gl) : 0;
        int unique = (scanned[i].status == SIG_OK);
        int good = pinned ? (unique && known && scanned[i].rva == known &&
                             results[i].status == SIG_OK && results[i].rva == known) : unique;
        if (!good) bad++;
        printf("%s %-20s scanned=0x%-9x recorded=0x%-9x %s\n", good ? "OK " : "BAD",
               scanned[i].name, scanned[i].rva, known, st);
    }
    /* The private tables the companion and scene-spawn features resolve. */
    {
        const sig_entry *tables[2] = { SH_COMPANION_SIGNATURES, SH_MODSCENE_SPAWN_SIGNATURES };
        for (int t = 0; t < 2; t++) {
            for (const sig_entry *e = tables[t]; e->name; e++) {
                sig_result scan, rec;
                uint32_t known = identity ? (identity->vulkan ? e->rva_vk : e->rva_gl) : 0;
                int good;
                sig_scan_one(base, e, &scan);
                sig_resolve_one(base, e, &rec);
                good = pinned ? (scan.status == SIG_OK && known && scan.rva == known && rec.status == SIG_OK)
                              : scan.status == SIG_OK;
                if (!good) bad++;
                printf("%s %-20s scanned=0x%-9x recorded=0x%-9x\n", good ? "OK " : "BAD",
                       e->name, scan.rva, known);
            }
        }
    }

    /* Check the registry anchor and +0x38/+0x58 vtable entries against independently
     * resolved functions. Translate preferred-base PE pointers into the mapped image. */
    {
        const sig_result *anchor = NULL, *type_method = NULL, *register_method = NULL;
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        uint64_t preferred = nt->OptionalHeader.ImageBase;
        size_t i;
        for (i = 0; i < total; i++) {
            if (strcmp(scanned[i].name, "DeclRegistryAnchor") == 0) anchor = &scanned[i];
            else if (strcmp(scanned[i].name, "DeclTypeByName") == 0) type_method = &scanned[i];
            else if (strcmp(scanned[i].name, "DeclRegisterFile") == 0) register_method = &scanned[i];
        }
        if (!anchor || !type_method || !register_method || anchor->status != SIG_OK ||
            type_method->status != SIG_OK || register_method->status != SIG_OK) {
            printf("BAD decl-registry ABI: prerequisite signature missing\n");
            bad++;
        } else {
            const uint8_t *mov = base + anchor->rva + 0x10;
            int32_t disp = 0;
            const uint8_t *slot;
            uint64_t registry_va = 0, vtable_va = 0, type_va = 0, register_va = 0;
            int abi_ok = mov[0] == 0x48 && mov[1] == 0x8B && mov[2] == 0x0D;
            memcpy(&disp, mov + 3, sizeof(disp));
            slot = mov + 7 + disp;
            if (abi_ok && slot >= base && slot + 8 <= base + image_sz)
                memcpy(&registry_va, slot, sizeof(registry_va));
            else abi_ok = 0;
            if (abi_ok && registry_va >= preferred && registry_va - preferred + 8 <= image_sz)
                memcpy(&vtable_va, base + (size_t)(registry_va - preferred), sizeof(vtable_va));
            else abi_ok = 0;
            if (abi_ok && vtable_va >= preferred && vtable_va - preferred + 0x60 <= image_sz) {
                const uint8_t *vtable = base + (size_t)(vtable_va - preferred);
                memcpy(&register_va, vtable + 0x38, sizeof(register_va));
                memcpy(&type_va, vtable + 0x58, sizeof(type_va));
            } else abi_ok = 0;
            if (!abi_ok || type_va != preferred + type_method->rva ||
                register_va != preferred + register_method->rva) {
                printf("BAD decl-registry ABI: registry=0x%llx vtable=0x%llx register=0x%llx type=0x%llx\n",
                       (unsigned long long)registry_va, (unsigned long long)vtable_va,
                       (unsigned long long)register_va, (unsigned long long)type_va);
                bad++;
            } else {
                printf("OK  decl-registry ABI  anchor->registry; vtable +0x38/+0x58 match resolved methods\n");
            }
        }
    }
    {
        sig_result target;
        sig_status status = pinned ? sig_resolve_one(base, &NAV_RENDER_TARGET_GL_SIGNATURE, &target)
                                   : sig_scan_one(base, &NAV_RENDER_TARGET_GL_SIGNATURE, &target);
        int is_gl = identity ? !identity->vulkan : strstr(argv[1], "DOOMx64.exe") != NULL;
        if ((is_gl && (status != SIG_OK ||
                       (pinned && target.rva != NAV_RENDER_TARGET_GL_SIGNATURE.rva_gl))) ||
            (!is_gl && status != SIG_NOT_FOUND)) {
            printf("BAD OpenGL navigation render-target helper: status=%d RVA=0x%x\n", status, target.rva);
            bad++;
        } else printf("OK  renderer-specific navigation target helper\n");
    }
    /* Grid-room tree cleanup: GridReapplyEntityTree must call the resolved
     * GridEntityTree (+0x17) and GridApplyEntityTree (+0x25) before its destructor
     * (+0x38) and sized delete (+0x45) calls, which grid_room_native.c binds. */
    if (identity) {
        const sig_result *re = NULL, *tree = NULL, *apply = NULL;
        for (size_t i = 0; i < total; i++) {
            if (!strcmp(scanned[i].name, "GridReapplyEntityTree")) re = &scanned[i];
            else if (!strcmp(scanned[i].name, "GridEntityTree")) tree = &scanned[i];
            else if (!strcmp(scanned[i].name, "GridApplyEntityTree")) apply = &scanned[i];
        }
        int ok_tree = 0;
        if (re && tree && apply && re->status == SIG_OK && tree->status == SIG_OK && apply->status == SIG_OK) {
            const uint8_t *p = base + re->rva;
            int32_t d1, d2;
            memcpy(&d1, p + 0x18, 4);
            memcpy(&d2, p + 0x26, 4);
            ok_tree = p[0x17] == 0xE8 && p[0x25] == 0xE8 && p[0x38] == 0xE8 && p[0x45] == 0xE8 &&
                      (uint32_t)(re->rva + 0x1c + d1) == tree->rva &&
                      (uint32_t)(re->rva + 0x2a + d2) == apply->rva;
        }
        if (!ok_tree) { printf("BAD grid-room tree cleanup: GridReapplyEntityTree calls do not match\n"); bad++; }
        else printf("OK  grid-room tree cleanup decodes from GridReapplyEntityTree\n");
    }
    printf("======================================================================\n");
    printf("signatures [%s]: %zu/%zu resolved at their recorded RVAs; %d bad\n",
           pinned ? "recorded-RVA" : "portable", ok, total, bad);
    free(base);
    return ((ok == total || !pinned) && bad == 0) ? 0 : 1;
}
