/* Offline signature-resolution checks against an unpacked PE mapped by RVA.
 * Pinned mode checks unique matches and expected Vulkan RVAs; portable mode
 * checks unique matches without requiring those addresses. Run via run-tests.ps1. */
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
    /* Uniqueness is required on each supported image. known_rva equality is
     * specific to the pinned Vulkan image; portable mode accepts address shifts. */
    int pinned = 1;
    if (argc < 2) {
        fprintf(stderr, "usage: sig_test <DOOM_unpacked.exe> [portable]\n");
        return 2;
    }
    if (argc >= 3 && strcmp(argv[2], "portable") == 0) pinned = 0;

    size_t image_sz = 0;
    uint8_t *base = map_pe_by_rva(argv[1], &image_sz);
    if (!base) return 2;

    /* Size results for the whole signature database to prevent out-of-bounds access. */
    size_t total = sig_db_count();
    sig_result results[SIG_RESULTS_MAX];
    size_t ok = sig_resolve_all(base, results, SIG_RESULTS_MAX);
    if (total > SIG_RESULTS_MAX) {
        printf("SIGNATURE DB OVERFLOW: %zu entries > SIG_RESULTS_MAX %d -- raise it\n",
               total, (int)SIG_RESULTS_MAX);
        return 1;
    }

    int bad = 0;
    for (size_t i = 0; i < total; i++) {
        const char *st = results[i].status == SIG_OK ? "OK " :
                         results[i].status == SIG_NOT_FOUND ? "NOTFOUND" :
                         results[i].status == SIG_AMBIGUOUS ? "AMBIG" : "BAD";
        uint32_t known = BACKEND_ENGINE_SIGNATURES[i].known_rva;
        int unique = (results[i].status == SIG_OK);
        int rva_ok = pinned && known ? (unique && results[i].rva == known) : unique;
        if (!rva_ok) bad++;
        if (pinned) {
            printf("%s %-20s resolved=0x%-9x known=0x%-9x %s\n",
                   rva_ok ? "OK " : "BAD", results[i].name, results[i].rva, known,
                   unique ? "" : st);
        } else {
            printf("%s %-20s resolved=0x%-9x shift=%+d %s\n",
                   rva_ok ? "OK " : "BAD", results[i].name, results[i].rva,
                   unique ? (int)((long)results[i].rva - (long)known) : 0,
                   unique ? "" : st);
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
            if (strcmp(results[i].name, "DeclRegistryAnchor") == 0) anchor = &results[i];
            else if (strcmp(results[i].name, "DeclTypeByName") == 0) type_method = &results[i];
            else if (strcmp(results[i].name, "DeclRegisterFile") == 0) register_method = &results[i];
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
        const sig_result *ctor=NULL, *id=NULL, *name=NULL, *language=NULL;
        int abi=1; int32_t displacement;
        uint64_t table_rva=0, object_rva=0, language_rva=0, id_va=0, name_va=0;
        IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)base;
        IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(base+dos->e_lfanew);
        uint64_t preferred=nt->OptionalHeader.ImageBase;
        for(size_t i=0;i<total;++i) {
            if(!strcmp(results[i].name,"AudioFileResolverInit"))ctor=results+i;
            else if(!strcmp(results[i].name,"AudioFileOpenId"))id=results+i;
            else if(!strcmp(results[i].name,"AudioFileOpenName"))name=results+i;
            else if(!strcmp(results[i].name,"AudioFileLanguage"))language=results+i;
        }
        if(!ctor || !id || !name || !language || ctor->status!=SIG_OK || id->status!=SIG_OK ||
            name->status!=SIG_OK || language->status!=SIG_OK) abi=0;
        if(abi) {
            memcpy(&displacement,base+ctor->rva+7,4);object_rva=(int64_t)ctor->rva+11+displacement;
            memcpy(&displacement,base+ctor->rva+19,4);table_rva=(int64_t)ctor->rva+23+displacement;
            memcpy(&displacement,base+language->rva+3,4);language_rva=(int64_t)language->rva+7+displacement;
            if(table_rva>=image_sz || image_sz-table_rva<24 || object_rva>=image_sz ||
                image_sz-object_rva<0x658 || language_rva>=image_sz || image_sz-language_rva<2)abi=0;
        }
        if(abi) {
            memcpy(&id_va,base+table_rva+8,8);memcpy(&name_va,base+table_rva+16,8);
            abi=id_va==preferred+id->rva && name_va==preferred+name->rva;
        }
        if(!abi){puts("BAD audio resolver ABI: constructor table/open methods/language");++bad;}
        else puts("OK  audio resolver ABI: constructor table +8/+0x10 match numeric/name open methods");
    }
    {
        sig_result target;
        sig_status status = sig_resolve_one(base, &NAV_RENDER_TARGET_GL_SIGNATURE, &target);
        int is_gl = strstr(argv[1], "DOOMx64.exe") != NULL;
        if ((is_gl && (status != SIG_OK || target.rva != 0x19231e0u)) ||
            (!is_gl && status != SIG_NOT_FOUND)) {
            printf("BAD OpenGL navigation render-target helper: status=%d RVA=0x%x\n", status, target.rva);
            bad++;
        } else printf("OK  renderer-specific navigation target helper\n");
    }
    printf("======================================================================\n");
    printf("C resolver [%s]: %zu/%zu unique; %d %s\n",
           pinned ? "pinned" : "portable", ok, total, bad,
           pinned ? "RVA-mismatches" : "non-unique");
    free(base);
    return (ok == total && bad == 0) ? 0 : 1;
}
