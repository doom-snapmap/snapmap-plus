/* Exercise real access faults and injected protection/rollback failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "patch.h"

void backend_log(const char *line) { (void)line; }
static int failures;
#define CHECK(test) do { if (!(test)) { printf("FAIL line %d: %s\n", __LINE__, #test); failures++; } } while (0)

int main(void)
{
    unsigned char original[8] = {1,2,3,4,5,6,7,8}, replacement[8] = {8,7,6,5,4,3,2,1};
    unsigned char *target = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    unsigned char *source = VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    sh_patch_handle handle;
    DWORD ignored;
    MEMORY_BASIC_INFORMATION memory;
    if (!target || !source) return 1;
    memcpy(target, original, sizeof original);
    CHECK(VirtualProtect(target, 4096, PAGE_EXECUTE_READ, &ignored));
    memcpy(source + 4092, replacement, 4);
    CHECK(VirtualProtect(source + 4096, 4096, PAGE_NOACCESS, &ignored));
    CHECK(code_patch(target, original, source + 4092, 8, &handle) == B2_PATCH_FAIL_SEH);
    CHECK(!handle.live && !memcmp(target, original, 8));
    CHECK(VirtualQuery(target, &memory, sizeof memory) && memory.Protect == PAGE_EXECUTE_READ);

    sh_patch_test_faults(1, 0, 0);
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_FAIL_PROTECT);
    CHECK(!handle.live && !memcmp(target, original, 8));
    sh_patch_test_faults(2, 0, 0);
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_FAIL_PROTECT);
    CHECK(!handle.live && !memcmp(target, original, 8));
    sh_patch_test_faults(2 | 8, 0, 0);
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_FAIL_ROLLBACK);
    CHECK(handle.live && handle.target == target && handle.len == 8);
    sh_patch_test_faults(0, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK && !handle.live);
    CHECK(VirtualQuery(target, &memory, sizeof memory) && memory.Protect == PAGE_EXECUTE_READ);

    sh_patch_test_faults(0, 1 | 2, 4);
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_FAIL_ROLLBACK);
    CHECK(handle.live);
    sh_patch_test_faults(0, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK && !memcmp(target, original, 8));
    CHECK(code_patch(target, original, replacement, 8, &handle) == B2_PATCH_OK);
    sh_patch_test_faults(1, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_FAIL_PROTECT && handle.live);
    CHECK(!memcmp(target, replacement, 8));
    sh_patch_test_faults(0, 1, 3);
    CHECK(code_unpatch(&handle) == B2_PATCH_FAIL_SEH && handle.live);
    sh_patch_test_faults(0, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK && !memcmp(target, original, 8));

    /* The aligned operand path must also roll back a failed protection restore. */
    CHECK(VirtualProtect(target, 4096, PAGE_EXECUTE_READWRITE, &ignored));
    memcpy(target + 3, "\xe8\x01\x02\x03\x04", 5);
    CHECK(VirtualProtect(target, 4096, PAGE_EXECUTE_READ, &ignored));
    sig_result site = {"call", SIG_OK, (uintptr_t)(target + 3), 3};
    const uint8_t before[5] = {0xe8,1,2,3,4}, after[5] = {0xe8,5,6,7,8};
    sh_patch_test_faults(2 | 8, 0, 0);
    CHECK(code_patch_call_sig(&site, before, after, &handle) == B2_PATCH_FAIL_ROLLBACK);
    CHECK(handle.live && handle.atomic_rel32 && !memcmp(target + 3, before, 5));
    sh_patch_test_faults(0, 0, 0);
    CHECK(code_unpatch(&handle) == B2_PATCH_OK);
    CHECK(VirtualQuery(target, &memory, sizeof memory) && memory.Protect == PAGE_EXECUTE_READ);
    CHECK(sh_patch_selftest());
    VirtualFree(source, 0, MEM_RELEASE);
    VirtualFree(target, 0, MEM_RELEASE);
    printf("patch_test: %d failures\n", failures);
    return failures != 0;
}
