/* Offline data-global resolution and layout checks on an RVA-mapped PE.
 * Run both build 13954591 renderers: each global must resolve to the RVA recorded
 * for that renderer; portable mode checks resolution and relative layout only.
 *
 *   globals_test <DOOMx64vk.exe | DOOMx64.exe>
 *   globals_test <image> portable */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine_globals.h"
#include "host_image.h"
#include "engine_build.h"

static uint8_t *map_pe_by_rva(const char *path, size_t *image_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)file;
    IMAGE_NT_HEADERS *nt  = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
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

static uint32_t rva_of(const uint8_t *base, const char *name, int *bad)
{
    glb_status st = GLB_UNKNOWN_NAME;
    uintptr_t a = glb_resolve(base, name, &st);
    if (!a) {
        printf("BAD %-24s UNRESOLVED status=%d\n", name, (int)st);
        (*bad)++;
        return 0;
    }
    return (uint32_t)(a - (uintptr_t)base);
}

static uintptr_t image_export(const uint8_t *base, size_t size, const char *name)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const IMAGE_EXPORT_DIRECTORY *exports;
    const DWORD *names, *functions;
    const WORD *ordinals;
    DWORD i;
    if (!directory.VirtualAddress || directory.VirtualAddress > size ||
        sizeof(*exports) > size - directory.VirtualAddress) return 0;
    exports = (const IMAGE_EXPORT_DIRECTORY *)(base + directory.VirtualAddress);
    if (exports->AddressOfNames > size || exports->NumberOfNames > (size - exports->AddressOfNames) / sizeof(DWORD) ||
        exports->AddressOfNameOrdinals > size || exports->NumberOfNames > (size - exports->AddressOfNameOrdinals) / sizeof(WORD) ||
        exports->AddressOfFunctions > size || exports->NumberOfFunctions > (size - exports->AddressOfFunctions) / sizeof(DWORD)) return 0;
    names = (const DWORD *)(base + exports->AddressOfNames);
    ordinals = (const WORD *)(base + exports->AddressOfNameOrdinals);
    functions = (const DWORD *)(base + exports->AddressOfFunctions);
    for (i = 0; i < exports->NumberOfNames; i++) {
        size_t length = strlen(name);
        if (names[i] >= size || length >= size - names[i] || memcmp(base + names[i], name, length + 1)) continue;
        if (ordinals[i] >= exports->NumberOfFunctions || functions[ordinals[i]] >= size) return 0;
        return (uintptr_t)base + functions[ordinals[i]];
    }
    return 0;
}

static void check_adjacent(const uint8_t *base, const char *lo, const char *hi,
                           uint32_t gap, int *bad)
{
    int local = 0;
    uint32_t a = rva_of(base, lo, &local);
    uint32_t b = rva_of(base, hi, &local);
    if (local) { *bad += local; return; }
    if (b - a != gap) {
        printf("BAD invariant %s + 0x%x != %s (0x%x + 0x%x != 0x%x)\n",
               lo, gap, hi, a, gap, b);
        (*bad)++;
    } else {
        printf("OK  invariant %s + 0x%-4x == %s\n", lo, gap, hi);
    }
}

int main(int argc, char **argv)
{
    int pinned = 1;
    if (argc < 2) {
        fprintf(stderr, "usage: globals_test <DOOM_unpacked.exe> [portable]\n");
        return 2;
    }
    size_t image_sz = 0;
    uint8_t *base = map_pe_by_rva(argv[1], &image_sz);
    if (!base) return 2;

    wchar_t wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wide, MAX_PATH);
    const sh_build_identity *identity = sh_host_test_bind_image(base, wide);
    if (!identity) {
        printf("BAD %s is not a supported executable (build 13954591)\n", argv[1]);
        free(base);
        return 1;
    }
    if (argc >= 3 && strcmp(argv[2], "portable") == 0) pinned = 0;
    printf("image %s: %s, %s mode\n", argv[1], identity->label, pinned ? "recorded-RVA" : "portable");

    size_t total = glb_db_count();
    size_t ok = 0;
    int bad = 0;

    for (size_t i = 0; i < total; i++) {
        const global_entry *e = &BACKEND_ENGINE_GLOBALS[i];
        glb_status st = GLB_UNKNOWN_NAME;
        uintptr_t a = glb_resolve(base, e->name, &st);
        if (!a) {
            printf("BAD %-24s UNRESOLVED status=%d\n", e->name, (int)st);
            bad++;
            continue;
        }
        ok++;
        uint32_t rva = (uint32_t)(a - (uintptr_t)base);
        uint32_t recorded = identity->vulkan ? e->rva_vk : e->rva_gl;
        if (pinned && rva != recorded) {
            printf("BAD %-24s resolved=0x%-9x recorded=0x%x\n", e->name, rva, recorded);
            bad++;
        } else {
            printf("OK  %-24s resolved=0x%-9x (recorded 0x%x)\n", e->name, rva, recorded);
        }
    }

    /* Class-B visibility recovery resumes at leaf+0x24. Verify the expected
     * xor al,al; ret tail in the supplied image before trusting that offset. */
    {
        glb_status st = GLB_UNKNOWN_NAME;
        uintptr_t lo = glb_resolve(base, "vis_leaf_lo", &st);
        if (!lo) {
            printf("BAD vis_leaf_lo UNRESOLVED status=%d\n", (int)st);
            bad++;
        } else {
            const uint8_t *tail = (const uint8_t *)(lo + 0x24);
            if (tail[0] == 0x32 && tail[1] == 0xC0 && tail[2] == 0xC3) {
                printf("OK  invariant vis_leaf_lo + 0x24 == xor al,al; ret\n");
            } else {
                printf("BAD vis_leaf_lo + 0x24 is %02x %02x %02x, not xor al,al; ret -- "
                       "the shield must not redirect here\n", tail[0], tail[1], tail[2]);
                bad++;
            }
        }
    }

    {
        /* GetGameSystemInterface is no longer exported. The object is behind a
         * relocated data pointer: apply that one relocation to the RVA-mapped copy
         * as the loader would, then read through the slot like the runtime. */
        {
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
            IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
            uintptr_t slot = sh_engine_game_system_slot(base);
            /* idGameSystemLocal: Vulkan 0x2F48970, OpenGL 0x2F90970 (build 13954591). */
            uintptr_t system = (uintptr_t)base + (identity->vulkan ? 0x2F48970u : 0x2F90970u);
            uint64_t va = 0;
            if (slot) memcpy(&va, (const void *)slot, sizeof va);
            if (!slot || va != nt->OptionalHeader.ImageBase + (system - (uintptr_t)base) ||
                image_export(base, image_sz, "GetGameSystemInterface")) {
                printf("BAD game-system slot 0x%llx holds 0x%llx\n",
                    (unsigned long long)(slot - (uintptr_t)base), (unsigned long long)va);
                bad++;
            } else printf("OK  game-system object 0x%llx through data slot 0x%llx\n",
                (unsigned long long)(system - (uintptr_t)base),
                (unsigned long long)(slot - (uintptr_t)base));
        }
    }

    /* Layout invariants of build 13954591; they run in both modes. */
    check_adjacent(base, "cmd_system_slot",      "cvar_system_slot",     0x10, &bad);
    check_adjacent(base, "main_thread_id",       "load_state",           (uint32_t)-4, &bad);
    check_adjacent(base, "main_thread_id",       "error_state",          0x08, &bad);
    check_adjacent(base, "resource_manager_ctx", "material_manager_ctx", 0xE0, &bad);

    printf("======================================================================\n");
    printf("global resolver [%s]: %zu/%zu resolved; %d failures\n",
           pinned ? "pinned" : "portable", ok, total, bad);
    free(base);
    return (ok == total && bad == 0) ? 0 : 1;
}
