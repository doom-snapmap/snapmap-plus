/* globals_test.c -- offline test for the engine data-global resolver (engine_globals.c).
 *
 * Maps an unpacked DOOM executable by RVA exactly as the Windows loader would, then resolves
 * every entry in BACKEND_ENGINE_GLOBALS against it. This is the same code path the live DLL
 * takes, with no game running.
 *
 * Run it against BOTH shipped executables. That is the point: a data RVA baked for one build is
 * the one thing that cannot survive the other, so the resolver has to be exercised somewhere it
 * has never seen the answer.
 *
 *   globals_test <DOOM_unpacked.exe>            # pinned mode: also require the pinned Vulkan RVA
 *   globals_test <DOOM_unpacked.exe> portable   # portable mode: resolution + invariants only
 *
 * Portable mode deliberately hardcodes no expected addresses. Instead it checks INVARIANTS that
 * are facts about the engine's data layout rather than about any one link output:
 *
 *   cvar_system_slot  == cmd_system_slot     + 0x10
 *   load_state        == main_thread_id      + 0x08
 *   error_state       == main_thread_id      + 0x0C
 *   material_manager_ctx == resource_manager_ctx + 0xE0
 *
 * Those relationships hold because the globals are adjacent slots the compiler emitted together.
 * If a resolver bug produced a plausible-looking wrong address, the adjacency would break -- which
 * is a far sharper check than comparing against a number we wrote down ourselves.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine_globals.h"

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

    /* The visibility leaf is the one place in the product that SETS the instruction pointer: on a
     * Class-B render-node fault the shield resumes at leaf+0x24, the predicate's own
     * `xor al,al; ret` tail. A wrong address there does not degrade a feature, it sends the
     * faulting thread into whatever happens to live at that offset. So assert the tail really is
     * that instruction pair on whatever image we were handed, rather than trusting that the leaf's
     * internal layout carried across the build. */
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
