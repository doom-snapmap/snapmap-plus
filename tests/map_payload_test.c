#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "map_payload.h"
#include "patch.h"

void backend_log(const char *line) { (void)line; }
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)

int main(void)
{
    /* Authored executable fixtures implement both native decisions. They
     * return acceptance directly; no game function bodies are copied here. */
    const unsigned char save_code[] = {0x8d,0x41,0xff, 0x3d,0xff,0xff,0x9f,0x00,
        0x0f,0x96,0xc0, 0x0f,0xb6,0xc0, 0xc3};
    const unsigned char read_code[] = {
        0x8b,0xc1, 0x83,0xf8,0x01, 0x7d,0x0a,
        0x31,0xc0,0xc3, 0x90,0x90,0x90,0x90,0x90,0x90,0x90,
        0x3b,0x05,0x29,0,0,0, 0x7e,0x07,
        0x31,0xc0,0xc3, 0x90,0x90,0x90,0x90,
        0xb8,0x01,0,0,0,0xc3
    };
    unsigned char *page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    typedef int (__fastcall *accept_length)(int);
    accept_length accepts[2];
    sig_result sites[2] = {{"SnapMapSaveSizeLimit", SIG_OK, 0, 0},
                           {"SnapMapReadSizeLimit", SIG_OK, 0, 0}};
    size_t i;
    DWORD previous;
    if (!page) return 1;
    memcpy(page, save_code, sizeof(save_code));
    memcpy(page + 128, read_code, sizeof(read_code));
    *(int *)(page + 192) = 10 * 1024 * 1024;
    CHECK(VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &previous));
    CHECK(FlushInstructionCache(GetCurrentProcess(), page, 256));
    sites[0].addr = (uintptr_t)(page + 3);
    sites[1].addr = (uintptr_t)(page + 130);
    accepts[0] = (accept_length)page; accepts[1] = (accept_length)(page + 128);
    for (i = 0; i < 2; i++) {
        CHECK(accepts[i](1) && accepts[i](10 * 1024 * 1024));
        CHECK(!accepts[i](0) && !accepts[i](-1) && !accepts[i](10 * 1024 * 1024 + 1));
    }
    CHECK(!sh_map_payload_install(NULL, 0));
    CHECK(!sh_map_payload_install(sites, 1)); /* No partial save-only repair. */
    for (i = 0; i < 2; i++) {
        sites[i].status = SIG_AMBIGUOUS;
        CHECK(!sh_map_payload_install(sites, 2));
        sites[i].status = SIG_OK_HOOKED;
        CHECK(!sh_map_payload_install(sites, 2));
        sites[i].status = SIG_OK;
    }
    sh_patch_test_faults(2 | 8, 0, 0);
    CHECK(!sh_map_payload_install(sites, 2));
    sh_patch_test_faults(0, 0, 0);
    CHECK(sh_map_payload_install(sites, 2)); /* Retained rollback is recovered. */
    CHECK(sh_map_payload_install(sites, 2));
    for (i = 0; i < 2; i++) {
        CHECK(accepts[i](1) && accepts[i](10 * 1024 * 1024 + 1));
        CHECK(accepts[i](46300589) && accepts[i](128 * 1024 * 1024) && accepts[i](INT_MAX));
        CHECK(!accepts[i](0) && !accepts[i](-1) && !accepts[i](INT_MIN));
    }
    CHECK(*(int *)(page + 192) == 10 * 1024 * 1024); /* Shared constant untouched. */
    CHECK(sh_map_payload_remove());
    CHECK(!accepts[0](46300589) && !accepts[1](46300589));
    CHECK(!memcmp(page, save_code, sizeof(save_code)));
    CHECK(!memcmp(page + 128, read_code, sizeof(read_code)));
    /* Failing the second write must also undo the successful first patch. */
    sh_patch_test_faults(0, 2, 1);
    CHECK(!sh_map_payload_install(sites, 2));
    sh_patch_test_faults(0, 0, 0);
    CHECK(!memcmp(page, save_code, sizeof(save_code)));
    CHECK(!memcmp(page + 128, read_code, sizeof(read_code)));
    CHECK(sh_map_payload_install(sites, 2));
    CHECK(sh_map_payload_remove());
    /* A changed reader site must refuse before changing the valid save site. */
    CHECK(VirtualProtect(page, 4096, PAGE_READWRITE, &previous));
    page[151] = 0x75;
    CHECK(VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &previous));
    CHECK(!sh_map_payload_install(sites, 2));
    CHECK(page[151] == 0x75 && !memcmp(page, save_code, sizeof(save_code)));
    CHECK(sh_map_payload_remove());
    VirtualFree(page, 0, MEM_RELEASE);
    if (failures) return 1;
    puts("native map payload length checks passed"); return 0;
}
