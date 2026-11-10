/* load_screen_stop_test.c -- the engine sites the load-screen stop
 * (src/fault_shield/load_screen_stop.c) depends on, checked on each build
 * 13954591 executable given on the command line:
 *   - CancelMapChange and LevelLoadCompleted resolve to unique signatures at the
 *     recorded RVAs;
 *   - CancelMapChange's first 15 bytes are the stolen window the installer
 *     verifies, whole and position-independent instructions;
 *   - CancelMapChange is idCommonLocal's vtable slot +0x28, the slot Frame's
 *     catch calls (Frame itself is slot +0x68);
 *   - the engine calls LevelLoadCompleted with the load_screen global in RCX,
 *     the argument the detour passes.
 * Run via run-tests.ps1 with -Doom / -DoomAlt. Each image is checked in its own
 * process because glb_resolve caches the first image's globals. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "engine_globals.h"
#include "x64_len.h"
#include "../src/fault_shield/load_screen_stop.h"

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
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz)
            memcpy(img + va, file + po, rs);
    }
    free(file);
    return img;
}

static uint32_t resolve(const uint8_t *base, const char *name, int vulkan)
{
    const sig_entry *entry = NULL;
    sig_result r;
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++)
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name)) entry = &BACKEND_ENGINE_SIGNATURES[i];
    if (!entry) { printf("FAIL %s missing from the signature DB\n", name); g_fail = 1; return 0; }
    if (sig_resolve_one(base, entry, &r) != SIG_OK || !r.addr) {
        printf("FAIL %s did not resolve to a unique match\n", name); g_fail = 1; return 0;
    }
    CHECK(r.rva == (vulkan ? entry->rva_vk : entry->rva_gl));
    printf("  %-20s rva=0x%X\n", name, r.rva);
    return r.rva;
}

/* Count call sites LEA RCX,[rip+disp] ; CALL rel32 whose call targets callee_rva
 * and whose RCX is arg_rva. */
static int count_calls_with_rcx(const uint8_t *base, uint32_t size, uint32_t callee_rva, uint32_t arg_rva)
{
    int n = 0;
    for (uint32_t i = 7; i + 5 <= size; i++) {
        int32_t rel, disp;
        if (base[i] != 0xE8) continue;
        memcpy(&rel, base + i + 1, 4);
        if ((uint32_t)(i + 5 + rel) != callee_rva) continue;
        if (base[i - 7] != 0x48 || base[i - 6] != 0x8D || base[i - 5] != 0x0D) continue;
        memcpy(&disp, base + i - 4, 4);
        if ((uint32_t)(i + disp) == arg_rva) n++;
    }
    return n;
}

/* Find a qword equal to image_base+slot_rva whose qword at +delta is image_base+other_rva. */
static int vtable_pair(const uint8_t *base, uint32_t size, uint64_t image_base,
                       uint32_t slot_rva, int delta, uint32_t other_rva)
{
    uint64_t want = image_base + slot_rva, other = image_base + other_rva;
    int n = 0;
    for (uint32_t i = 0; i + 8 <= size; i += 8) {
        uint64_t v, w;
        memcpy(&v, base + i, 8);
        if (v != want) continue;
        if ((int64_t)i + delta < 0 || i + delta + 8 > size) continue;
        memcpy(&w, base + i + delta, 8);
        if (w == other) n++;
    }
    return n;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0;
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    IMAGE_NT_HEADERS *nt;
    uint32_t cmc, llc, frame, ls;
    unsigned flags = 0;
    uintptr_t ls_addr;

    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    printf("image %s: %s\n", path, identity->label);
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);

    cmc = resolve(base, "CancelMapChange", identity->vulkan);
    llc = resolve(base, "LevelLoadCompleted", identity->vulkan);
    if (!cmc || !llc) return;

    CHECK(memcmp(base + cmc, k_cancel_map_change_prologue, LOAD_SCREEN_STOP_STOLEN) == 0);
    CHECK(sh_x64_stolen_boundary(base + cmc, 32, 14, 32, &flags) == LOAD_SCREEN_STOP_STOLEN);
    CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);

    /* idCommonLocal::Frame, recorded in the shield's own table (shield_sigs.c). */
    frame = identity->vulkan ? 0x159D930u : 0x159E9A0u;
    CHECK(vtable_pair(base, size, nt->OptionalHeader.ImageBase, cmc, 0x68 - 0x28, frame) == 1);

    ls_addr = glb_resolve(base, "load_screen", NULL);
    CHECK(ls_addr != 0);
    if (!ls_addr) return;
    ls = (uint32_t)(ls_addr - (uintptr_t)base);
    {
        int calls = count_calls_with_rcx(base, size, llc, ls);
        printf("  LevelLoadCompleted(load_screen 0x%X) call sites: %d\n", ls, calls);
        CHECK(calls >= 1);
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("load_screen_stop_test: no images given; skipped\n"); return 0; }
    for (int i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "load_screen_stop_test: FAIL\n" : "load_screen_stop_test: PASS\n");
    return g_fail;
}
