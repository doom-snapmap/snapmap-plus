/* hook_test.c -- offline self-test for the backend inline-detour installer (hook.c).
 *
 * Exercises install_inline_hook -> call-detoured -> call-trampoline -> hook_unpatch on a hand-laid
 * SCRATCH stub -- the exact path smoke.c runs inside the DLL, but standalone so the installer is
 * proven without the game running. The stub is hand-coded machine bytes (not a C function) so the
 * optimizer can't shrink it below the 16-byte stolen window or emit a RIP-relative prologue; its first
 * 16 bytes are whole, register-only, PI instructions. NOT shipped in the DLL.
 *
 *   cl /nologo /O2 /MT hook_test.c hook.c /Fe:hook_test.exe
 *   hook_test.exe          # exit 0 iff install/detour/trampoline/un-patch all behave
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "hook.h"

#define TAG_ORIG   1
#define TAG_DETOUR 2
#define STOLEN 16
typedef int (*scratch_fn)(int, volatile int *);

static const unsigned char ORIG_CODE[] = {
    0xC7, 0x02, 0x01, 0x00, 0x00, 0x00,   /* mov dword [rdx], 1 */
    0x8B, 0xC1,                           /* mov eax, ecx */
    0x03, 0xC0,                           /* add eax, eax */
    0x48, 0x87, 0xC0,                     /* xchg rax,rax */
    0x48, 0x87, 0xC0,                     /* xchg rax,rax  -- 16 bytes */
    0x90, 0x90, 0xC3                      /* nop nop ret */
};
static const unsigned char DETOUR_CODE[] = {
    0xC7, 0x02, 0x02, 0x00, 0x00, 0x00,   /* mov dword [rdx], 2 */
    0x8B, 0xC1,                           /* mov eax, ecx */
    0x05, 0xE8, 0x03, 0x00, 0x00,         /* add eax, 1000 */
    0xC3                                  /* ret */
};

int main(void)
{
    int fails = 0;
    volatile int tag;

    unsigned char *oc = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *dc = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!oc || !dc) { printf("alloc FAIL\n"); return 1; }
    memcpy(oc, ORIG_CODE, sizeof ORIG_CODE);
    memcpy(dc, DETOUR_CODE, sizeof DETOUR_CODE);
    FlushInstructionCache(GetCurrentProcess(), oc, 64);
    FlushInstructionCache(GetCurrentProcess(), dc, 64);
    scratch_fn orig_fn = (scratch_fn)oc, scratch_detour = (scratch_fn)dc;

    tag = 0;
    int base = orig_fn(21, &tag);
    if (base != 42 || tag != TAG_ORIG) { printf("baseline FAIL (%d tag=%d)\n", base, tag); fails++; }

    int before = hook_installed_count();
    void *tramp = install_inline_hook((void *)orig_fn, (void *)scratch_detour, STOLEN);
    if (!tramp || hook_installed_count() != before + 1) { printf("install FAIL\n"); return 1; }

    tag = 0;
    int patched = orig_fn(7, &tag);
    if (!(tag == TAG_DETOUR && patched == 1007)) {
        printf("detour FAIL (tag=%d ret=%d)\n", tag, patched); fails++;
    }

    tag = 0;
    scratch_fn tramp_fn = (scratch_fn)tramp;
    int via = tramp_fn(9, &tag);
    if (!(tag == TAG_ORIG && via == 18)) {
        printf("trampoline FAIL (tag=%d ret=%d)\n", tag, via); fails++;
    }

    int reverted = hook_unpatch(tramp);
    tag = 0;
    int after = orig_fn(5, &tag);
    if (!(reverted && hook_installed_count() == before && tag == TAG_ORIG && after == 10)) {
        printf("un-patch FAIL (rev=%d cnt=%d tag=%d ret=%d)\n",
               reverted, hook_installed_count(), tag, after); fails++;
    }

    VirtualFree(oc, 0, MEM_RELEASE);
    VirtualFree(dc, 0, MEM_RELEASE);
    printf(fails ? "HOOK SELF-TEST: %d FAIL\n" : "HOOK SELF-TEST: OK (install/detour/trampoline/un-patch)\n", fails);
    return fails ? 1 : 0;
}
