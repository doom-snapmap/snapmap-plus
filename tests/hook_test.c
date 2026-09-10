/* Offline two-phase hook publication, execution, and failed-restoration checks. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "hook.h"

void backend_log(const char *line) { (void)line; }

#define STOLEN 16
typedef int (*scratch_fn)(int, volatile int *);
static const unsigned char ORIG_CODE[] = {
    0xC7,0x02,0x01,0,0,0, /* mov dword [rdx], 1 */
    0x8B,0xC1,            /* mov eax, ecx */
    0x03,0xC0,            /* add eax, eax */
    0x48,0x87,0xC0,0x48,0x87,0xC0, /* two position-independent nops */
    0x90,0x90,0xC3
};
static int failures, observed_writes;
static scratch_fn published_original;

#define CHECK(expr) do { if (!(expr)) { \
    printf("line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

static int detour(int value, volatile int *tag)
{
    return published_original(value, tag) + 1000;
}

static void before_target_write(void *target)
{
    volatile int tag = 0;
    (void)target;
    observed_writes++;
    CHECK(published_original != NULL);
    if (published_original) {
        CHECK(published_original(9, &tag) == 18 && tag == 1);
        CHECK(detour(9, &tag) == 1018);
    }
}

int main(void)
{
    unsigned char *code = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    scratch_fn target = (scratch_fn)code;
    volatile int tag = 0;
    int baseline = hook_owned_count();
    if (!code) return 1;
    memcpy(code, ORIG_CODE, sizeof ORIG_CODE);
    FlushInstructionCache(GetCurrentProcess(), code, 64);
    CHECK(target(21, &tag) == 42 && tag == 1);
    CHECK(hook_commit(NULL) == B2_PATCH_REFUSED_BADARG);

    /* Preparation leaves the target untouched; the callback is published before commit. */
    published_original = (scratch_fn)hook_prepare(code, detour, STOLEN);
    CHECK(published_original && hook_owned_count() == baseline + 1);
    CHECK(!hook_is_installed((void *)published_original));
    CHECK(!memcmp(code, ORIG_CODE, sizeof ORIG_CODE));
    CHECK(target(7, &tag) == 14);
    CHECK(hook_prepare(code, detour, STOLEN) == NULL);
    sh_patch_test_observe_write(before_target_write);
    CHECK(hook_commit((void *)published_original) == B2_PATCH_OK);
    CHECK(observed_writes == 1 && hook_is_installed((void *)published_original));
    CHECK(target(7, &tag) == 1014);

    /* Failed removal is not an installed state. Its live detour can still call through. */
    sh_patch_test_faults(1, 0, 0);
    CHECK(!hook_unpatch((void *)published_original));
    CHECK(!hook_is_installed((void *)published_original));
    CHECK(hook_owned_count() == baseline + 1 && target(7, &tag) == 1014);
    CHECK(hook_commit((void *)published_original) == B2_PATCH_FAIL_ROLLBACK);
    sh_patch_test_faults(2, 0, 0);
    CHECK(!hook_unpatch((void *)published_original));
    CHECK(hook_owned_count() == baseline + 1 && target(7, &tag) == 14);
    CHECK(published_original(9, &tag) == 18);
    sh_patch_test_faults(0, 0, 0);
    CHECK(hook_unpatch((void *)published_original));
    published_original = NULL;
    CHECK(hook_owned_count() == baseline && hook_installed_count() == 0);

    /* Commit refusal retains the prepared callback and permits a safe retry. */
    published_original = (scratch_fn)hook_prepare(code, detour, STOLEN);
    sh_patch_test_faults(1, 0, 0);
    CHECK(hook_commit((void *)published_original) == B2_PATCH_FAIL_PROTECT);
    CHECK(published_original(9, &tag) == 18 && target(7, &tag) == 14);
    CHECK(hook_owned_count() == baseline + 1 && !hook_is_installed((void *)published_original));
    sh_patch_test_faults(0, 0, 0);
    CHECK(hook_commit((void *)published_original) == B2_PATCH_OK);
    CHECK(target(7, &tag) == 1014);
    CHECK(hook_unpatch((void *)published_original));
    published_original = NULL;

    /* A partial write can roll back completely without discarding caller ownership. */
    published_original = (scratch_fn)hook_prepare(code, detour, STOLEN);
    sh_patch_test_faults(0, 1, 8);
    CHECK(hook_commit((void *)published_original) == B2_PATCH_FAIL_SEH);
    CHECK(!memcmp(code, ORIG_CODE, sizeof ORIG_CODE));
    CHECK(hook_owned_count() == baseline + 1 && !hook_is_installed((void *)published_original));
    CHECK(hook_unpatch((void *)published_original));
    published_original = NULL;

    /* Failed commit plus failed rollback leaves a reachable detour with a callable original. */
    published_original = (scratch_fn)hook_prepare(code, detour, STOLEN);
    sh_patch_test_faults(2 | 4, 0, 0);
    CHECK(hook_commit((void *)published_original) == B2_PATCH_FAIL_ROLLBACK);
    CHECK(!hook_is_installed((void *)published_original));
    CHECK(hook_owned_count() == baseline + 1 && target(7, &tag) == 1014);
    CHECK(published_original(9, &tag) == 18);
    CHECK(hook_commit((void *)published_original) == B2_PATCH_FAIL_ROLLBACK);
    CHECK(hook_prepare(code, detour, STOLEN) == NULL);
    sh_patch_test_faults(0, 0, 0);
    CHECK(hook_unpatch((void *)published_original));
    published_original = NULL;
    CHECK(hook_owned_count() == baseline && !memcmp(code, ORIG_CODE, sizeof ORIG_CODE));

    /* Recheck the prepared bytes at commit, then reuse the released slot. */
    published_original = (scratch_fn)hook_prepare(code, detour, STOLEN);
    code[0] ^= 1;
    CHECK(hook_commit((void *)published_original) == B2_PATCH_REFUSED_VERIFY);
    CHECK(code[0] == (ORIG_CODE[0] ^ 1));
    CHECK(hook_unpatch((void *)published_original));
    published_original = NULL;
    code[0] = ORIG_CODE[0];
    for (int i = 0; i < 80; i++) {
        void *prepared = hook_prepare(code, detour, STOLEN);
        CHECK(prepared != NULL);
        CHECK(hook_unpatch(prepared));
    }
    CHECK(hook_owned_count() == baseline && hook_unpatch_all() == 0);
    sh_patch_test_observe_write(NULL);
    VirtualFree(code, 0, MEM_RELEASE);
    printf("hook_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
