/* Offline data-global resolution and layout checks on an RVA-mapped PE.
 * Run both supported renderers: pinned mode checks Vulkan RVAs; portable mode
 * checks resolution and relative layout without fixed addresses.
 *
 *   globals_test <unpacked.exe>
 *   globals_test <unpacked.exe> portable */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine_globals.h"
#include "decl_native_schema.h"
#include "map_source.h"
#include "map_native.h"
#include "map_published.h"
#include "map_transition.h"
#include "resource_resident.h"

typedef struct metadata_image { const uint8_t *base; size_t size; uintptr_t last; } metadata_image;
static int metadata_read(void *context, uintptr_t address, void *out, size_t length)
{
    metadata_image *image = (metadata_image *)context;
    uintptr_t base = (uintptr_t)image->base;
    image->last = address;
    if (address < base || address - base > image->size || length > image->size - (address - base)) return 0;
    memcpy(out, (const void *)address, length); return 1;
}

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
    if (argc >= 3 && strcmp(argv[2], "portable") == 0) pinned = 0;

    size_t image_sz = 0;
    uint8_t *base = map_pe_by_rva(argv[1], &image_sz);
    if (!base) return 2;

    size_t total = glb_db_count();
    size_t ok = 0;
    int bad = 0;

    /* The native profile reader derives its filesystem/allocator slots from
     * instructions adjacent to the signed-length guard on each renderer. */
    {
        const char *names[] = {"SnapMapReadSizeLimit", "MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap",
            "PublishedMapComplete", "PublishedMapSelectionConstruct", "PublishedMapMetadataDestroy", "DeserializeFromJson",
            "PublishedMapOfflineLaunch", "PublishedMapLobbyLaunch", "PublishedMapDirectLaunch", "PublishedMapCacheReady",
            "PublishedMapRead", "PublishedMapCacheSet", "SerializeToJson"};
        sig_result source[15] = {0};
        int bound = 1;
        for (size_t i = 0; i < 15; i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) {
                printf("BAD published source signature %s\n", names[i]); bound = 0;
            }
        }
        if (!bound || !sh_map_source_install(source, 15, base) || !sh_map_native_bind(source, 15, base) ||
            !sh_map_published_bind(source, 15, base)) {
            printf("BAD native profile source binding\n"); bad++;
        } else printf("OK  native profile source binding\n");
    }

    {
        const char *names[] = {"AllocateGameResources", "UnloadGameResources", "FinalizeMapChange", "CancelMapChange"};
        sig_result source[4] = {0};
        int bound = 1;
        for (size_t i = 0; i < 4; i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) bound = 0;
        }
        if (!bound || !sh_map_transition_bind(source, 4, base)) {
            printf("BAD native map transition binding\n"); bad++;
        } else printf("OK  native map transition binding\n");
    }
    {
        const char *names[] = {"ResourceReloadRenderScope", "PublishedMapComplete", "DeclSourceModeCall",
            "ResourceReconstruct", "ResourceGenericLoad", "ResourceLookup", "MemLocalGet", "MemLocalPushHeap", "MemLocalPopHeap",
            "MaterialVirtualTextureRebind"};
        sig_result source[10] = {0};
        int bound = 1;
        for (size_t i = 0; i < 10; i++) {
            const sig_entry *entry = NULL;
            for (size_t j = 0; BACKEND_ENGINE_SIGNATURES[j].name; j++)
                if (!strcmp(BACKEND_ENGINE_SIGNATURES[j].name, names[i])) entry = &BACKEND_ENGINE_SIGNATURES[j];
            if (!entry || sig_resolve_one(base, entry, &source[i]) != SIG_OK) bound = 0;
        }
        if (!bound || !sh_resource_resident_bind(source, 10, base)) {
            printf("BAD native resident refresh binding\n"); bad++;
        } else printf("OK  native resident refresh binding\n");
    }
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
        if (pinned && rva != e->pinned_rva) {
            printf("BAD %-24s resolved=0x%-9x pinned=0x%x\n", e->name, rva, e->pinned_rva);
            bad++;
        } else if (pinned) {
            printf("OK  %-24s resolved=0x%-9x\n", e->name, rva);
        } else {
            printf("OK  %-24s resolved=0x%-9x (pinned 0x%x)\n", e->name, rva, e->pinned_rva);
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

    /* The runtime metadata binder must locate reflection without executing the
     * lazy singleton accessor. Static PE state has not initialized its readers. */
    {
        metadata_image image = {base, image_sz, 0};
        sh_decl_native_source source = {&image, metadata_read, 0};
        uintptr_t accessor = glb_resolve(base, "declmgr_accessor", NULL);
        uintptr_t container = glb_resolve(base, "type_container", NULL);
        uintptr_t rva = container - (uintptr_t)base;
        uintptr_t expected = rva == 0x3082b10u ? (uintptr_t)base + 0x6012120u :
            rva == 0x30c78e0u ? (uintptr_t)base + 0x4910430u : 0;
        int state = sh_decl_native_source_bind(&source, accessor, container);
        if (state != 0 || source.reflection || image.last != expected) {
            printf("BAD read-only metadata binder state=%d read=0x%llx expected=0x%llx\n",
                state, (unsigned long long)(image.last - (uintptr_t)base),
                (unsigned long long)(expected - (uintptr_t)base));
            bad++;
        } else printf("OK  read-only metadata binder locates uninitialized reflection at 0x%llx\n",
            (unsigned long long)(expected - (uintptr_t)base));

        /* Use the actual export directory, as runtime GetProcAddress does.
         * The getter's relative target and native list layout must agree on
         * both images; static reader allocation alone is not readiness. */
        {
            uintptr_t getter = image_export(base, image_sz, "GetGameSystemInterface");
            uintptr_t system = rva == 0x3082b10u ? (uintptr_t)base + 0x2dfa1b0u :
                rva == 0x30c78e0u ? (uintptr_t)base + 0x2e3f050u : 0;
            source.reflection = 1;
            state = sh_decl_native_source_ready(&source, accessor, container, getter);
            if (!getter || state != 0 || source.reflection || image.last != system + 8) {
                printf("BAD native reader readiness state=%d getter=0x%llx read=0x%llx\n", state,
                    (unsigned long long)(getter - (uintptr_t)base),
                    (unsigned long long)(image.last - (uintptr_t)base));
                bad++;
            } else printf("OK  native reader readiness waits at system 0x%llx through exported getter 0x%llx\n",
                (unsigned long long)(system - (uintptr_t)base),
                (unsigned long long)(getter - (uintptr_t)base));
        }
    }

    /* Layout invariants -- true on any build, so they run in both modes. */
    check_adjacent(base, "cmd_system_slot",      "cvar_system_slot",     0x10, &bad);
    check_adjacent(base, "main_thread_id",       "load_state",           0x08, &bad);
    check_adjacent(base, "main_thread_id",       "error_state",          0x0C, &bad);
    check_adjacent(base, "resource_manager_ctx", "material_manager_ctx", 0xE0, &bad);

    printf("======================================================================\n");
    printf("global resolver [%s]: %zu/%zu resolved; %d failures\n",
           pinned ? "pinned" : "portable", ok, total, bad);
    free(base);
    return (ok == total && bad == 0) ? 0 : 1;
}
