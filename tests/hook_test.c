/* Offline two-phase hook publication, execution, and failed-restoration checks. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
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

static uintptr_t (*lea_original)(void);
static uintptr_t lea_detour(void) { return lea_original() + 1; }

static void rip_load_preserves_values(void)
{
    unsigned char *page=VirtualAlloc(NULL,1024,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    CHECK(page!=NULL); if(!page) return;
    const unsigned char pushes[]={0x53,0x56,0x57,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    const unsigned char pops[]={0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5d,0x5f,0x5e,0x5b,0xc3};
    for(unsigned reg=0;reg<16;reg++) for(int direction=-1;direction<=1;direction+=2) {
        unsigned char original[64], *code=page+(direction<0?512:0);
        uint64_t *value=(uint64_t *)(page+256); *value=UINT64_C(0x1234567887654321)+reg;
        int32_t displacement=(int32_t)((unsigned char *)value-(code+21));
        memset(original,0x90,sizeof(original)); memcpy(original,pushes,sizeof(pushes));
        original[14]=(unsigned char)(reg>=8?0x4c:0x48); original[15]=0x8b;
        original[16]=(unsigned char)(5|((reg&7)<<3)); memcpy(original+17,&displacement,4);
        original[21]=original[14]; original[22]=0x89; original[23]=(unsigned char)(0xc0|((reg&7)<<3));
        memcpy(original+24,pops,sizeof(pops)); memcpy(code,original,sizeof(original));
        FlushInstructionCache(GetCurrentProcess(),page,1024);
        if(reg==4) { CHECK(!hook_prepare_rip_load(code,lea_detour,21,14)); continue; }
        CHECK(((uintptr_t (*)(void))code)()==*value);
        CHECK(!hook_prepare_rip_load(code,lea_detour,21,SIZE_MAX));
        CHECK(!hook_prepare_rip_load(code,lea_detour,21,15));
        lea_original=(uintptr_t (*)(void))hook_prepare_rip_load(code,lea_detour,21,14);
        CHECK(lea_original!=NULL); if(!lea_original) continue;
        CHECK(lea_original()==*value);
        *value+=17; CHECK(lea_original()==*value);
        CHECK(hook_commit((void *)lea_original)==B2_PATCH_OK);
        CHECK(((uintptr_t (*)(void))code)()==*value+1);
        CHECK(hook_unpatch((void *)lea_original)); lea_original=NULL;
        CHECK(!memcmp(code,original,sizeof(original)));
    }
    VirtualFree(page,0,MEM_RELEASE);
}
static int (*branch_original)(int);
static int branch_detour(int value) { return branch_original(value)+100; }
static void conditional_branch_preserves_paths(void)
{
    unsigned char *page=VirtualAlloc(NULL,512,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    CHECK(page!=NULL); if(!page) return;
    for(int near_branch=0;near_branch<2;near_branch++) for(int condition=4;condition<=5;condition++)
    for(int direction=-1;direction<=1;direction+=2) {
        unsigned char *code=page+(direction<0?256:0), *target=page+(direction<0?192:64), original[48];
        size_t length=near_branch?6:2, stolen=12+length;
        memset(original,0x90,sizeof(original)); memcpy(original,"\xb8\x07\0\0\0",5);
        memcpy(original+9,"\x83\xf9\0",3);
        if(near_branch) { original[12]=0x0f; original[13]=(unsigned char)(0x80|condition);
            int32_t displacement=(int32_t)(target-(code+stolen)); memcpy(original+14,&displacement,4); }
        else { original[12]=(unsigned char)(0x70|condition); original[13]=(unsigned char)(int8_t)(target-(code+stolen)); }
        memcpy(original+stolen,"\x83\xc0\x03\xc3",4); memcpy(target,"\x83\xc0\x09\xc3",4);
        memcpy(code,original,sizeof(original)); FlushInstructionCache(GetCurrentProcess(),page,512);
        CHECK(!hook_prepare_conditional_jump(code,branch_detour,stolen,SIZE_MAX));
        CHECK(!hook_prepare_conditional_jump(code,branch_detour,stolen,11));
        branch_original=(int (*)(int))hook_prepare_conditional_jump(code,branch_detour,stolen,12);
        CHECK(branch_original!=NULL); if(!branch_original) continue;
        for(int value=0;value<2;value++) {
            int expected=((condition==4)==(value==0))?16:10;
            CHECK(((int (*)(int))code)(value)==expected); CHECK(branch_original(value)==expected);
        }
        CHECK(hook_commit((void *)branch_original)==B2_PATCH_OK);
        for(int value=0;value<2;value++) CHECK(((int (*)(int))code)(value)==100+(((condition==4)==(value==0))?16:10));
        CHECK(hook_unpatch((void *)branch_original)); branch_original=NULL;
        CHECK(!memcmp(code,original,sizeof(original)));
        if(near_branch) { int32_t internal=-6; memcpy(code+14,&internal,4); }
        else code[13]=0xfe;
        CHECK(!hook_prepare_conditional_jump(code,branch_detour,stolen,12));
    }
    VirtualFree(page,0,MEM_RELEASE);
}
static uintptr_t (*compare_original)(void);
static uintptr_t compare_detour(void) { return compare_original()+100; }
static void rip_compare_preserves_register_and_flags(void)
{
    unsigned char *page=VirtualAlloc(NULL,512,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    CHECK(page!=NULL); if(!page) return;
    for(int direction=-1;direction<=1;direction+=2) for(int immediate=-1;immediate<=1;immediate++) {
        unsigned char original[48], *code=page+(direction<0?256:0);
        int32_t *value=(int32_t *)(page+128), displacement=(int32_t)((unsigned char *)value-(code+13));
        memset(original,0x90,sizeof(original)); memcpy(original,"\xb8\x34\x12\0\0",5);
        original[6]=0x83; original[7]=0x3d; memcpy(original+8,&displacement,4); original[12]=(unsigned char)immediate;
        /* SETNE observes the relocated compare's flags; LEA keeps RAX's
         * earlier value visible without changing those flags. */
        memcpy(original+16,"\x0f\x95\xc1\x0f\xb6\xc9\x48\x8d\x04\x08\xc3",11);
        memcpy(code,original,sizeof(original)); FlushInstructionCache(GetCurrentProcess(),page,512);
        CHECK(!hook_prepare_rip_cmp32_imm8(code,compare_detour,16,SIZE_MAX));
        CHECK(!hook_prepare_rip_cmp32_imm8(code,compare_detour,16,7));
        compare_original=(uintptr_t (*)(void))hook_prepare_rip_cmp32_imm8(code,compare_detour,16,6);
        CHECK(compare_original!=NULL); if(!compare_original) continue;
        for(int change=0;change<2;change++) { *value=immediate+change;
            CHECK(((uintptr_t (*)(void))code)()==0x1234u+change); CHECK(compare_original()==0x1234u+change); }
        CHECK(hook_commit((void *)compare_original)==B2_PATCH_OK);
        for(int change=0;change<2;change++) { *value=immediate+change;
            CHECK(((uintptr_t (*)(void))code)()==0x1234u+100+change); }
        CHECK(hook_unpatch((void *)compare_original)); compare_original=NULL;
        CHECK(!memcmp(code,original,sizeof(original)));
    }
    VirtualFree(page,0,MEM_RELEASE);
}

static void rip_lea_preserves_addresses(void)
{
    const unsigned registers[] = {0, 1, 2, 8, 9, 10, 11};
    unsigned char original[18], *code;
    size_t i;
    int sign, before = hook_owned_count();
    code = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(code != NULL); if (!code) return;
    for (i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        unsigned reg = registers[i];
        for (sign = -1; sign <= 1; sign += 2) {
            int32_t displacement = sign * 8;
            uintptr_t expected = (uintptr_t)(code + 14) + displacement;
            memset(original, 0x90, sizeof(original));
            original[7] = (unsigned char)(reg >= 8 ? 0x4c : 0x48);
            original[8] = 0x8d; original[9] = (unsigned char)(5 | ((reg & 7) << 3));
            memcpy(original + 10, &displacement, 4);
            original[14] = original[7]; original[15] = 0x89;
            original[16] = (unsigned char)(0xc0 | ((reg & 7) << 3)); original[17] = 0xc3;
            memcpy(code, original, sizeof(original));
            FlushInstructionCache(GetCurrentProcess(), code, 64);
            CHECK(((uintptr_t (*)(void))code)() == expected);
            CHECK(!hook_prepare_rip_lea(code, lea_detour, 14, SIZE_MAX));
            CHECK(!hook_prepare_rip_lea(code, lea_detour, 14, 8));
            CHECK(!hook_prepare_rip_lea(code, lea_detour, 14, 0));
            lea_original = (uintptr_t (*)(void))hook_prepare_rip_lea(code, lea_detour, 14, 7);
            CHECK(lea_original != NULL);
            if (!lea_original) continue;
            CHECK(lea_original() == expected);
            CHECK(!memcmp(code, original, sizeof(original)));
            CHECK(hook_commit((void *)lea_original) == B2_PATCH_OK);
            CHECK(((uintptr_t (*)(void))code)() == expected + 1);
            CHECK(hook_unpatch((void *)lea_original)); lea_original = NULL;
            CHECK(!memcmp(code, original, sizeof(original)));
        }
    }
    CHECK(hook_owned_count() == before);
    VirtualFree(code, 0, MEM_RELEASE);
}

static void relative_call_preserves_execution(void)
{
    unsigned char *page = VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char original[32];
    int baseline = hook_owned_count();
    CHECK(page != NULL); if (!page) return;
    /* Caller has an aligned stack and normal shadow space. The callee observes
     * the incoming volatile registers, writes through RDX, and returns in RAX. */
    for (int direction = -1; direction <= 1; direction += 2) {
        unsigned char *code = page + (direction < 0 ? 128 : 0);
        unsigned char *callee = page + (direction < 0 ? 0 : 128);
        int32_t displacement = (int32_t)(callee - (code + 12));
        volatile int tag = 0;
        memset(original, 0x90, sizeof(original));
        memcpy(original, "\x48\x83\xec\x28\x8b\xc1\x90\xe8", 8);
        memcpy(original + 8, &displacement, 4);
        memcpy(original + 16, "\x48\x83\xc4\x28\xc3", 5);
        memcpy(callee, "\x89\x0a\x03\xc1\xc3", 5);
        memcpy(code, original, sizeof(original));
        FlushInstructionCache(GetCurrentProcess(), page, 256);
        CHECK(((scratch_fn)code)(21, &tag) == 42 && tag == 21);
        CHECK(!hook_prepare_relative_call(code, detour, 16, SIZE_MAX));
        CHECK(!hook_prepare_relative_call(code, detour, 16, 12));
        CHECK(!hook_prepare_relative_call(code, detour, 16, 6));
        published_original = (scratch_fn)hook_prepare_relative_call(code, detour, 16, 7);
        CHECK(published_original != NULL);
        if (published_original) {
            CHECK(!memcmp(code, original, sizeof(original)));
            CHECK(published_original(37, &tag) == 74 && tag == 37);
            CHECK(hook_commit((void *)published_original) == B2_PATCH_OK);
            CHECK(((scratch_fn)code)(21, &tag) == 1042 && tag == 21);
            CHECK(hook_unpatch((void *)published_original)); published_original = NULL;
            CHECK(!memcmp(code, original, sizeof(original)));
        }
        displacement = -5; memcpy(code + 8, &displacement, 4);
        CHECK(!hook_prepare_relative_call(code, detour, 16, 7));
    }
    CHECK(hook_owned_count() == baseline);
    VirtualFree(page, 0, MEM_RELEASE);
}

static int constant_detour(int value, volatile int *tag)
{
    *tag = 2;
    return value + 1000;
}

static void many_simultaneous_hooks(void)
{
    enum { COUNT = 193, STRIDE = 64 };
    void *prepared[COUNT] = {0};
    unsigned char *code = VirtualAlloc(NULL, COUNT * STRIDE,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int before = hook_owned_count();
    CHECK(code != NULL); if (!code) return;
    for (int i = 0; i < COUNT; ++i)
        memcpy(code + i * STRIDE, ORIG_CODE, sizeof(ORIG_CODE));
    FlushInstructionCache(GetCurrentProcess(), code, COUNT * STRIDE);
    for (int i = 0; i < COUNT; ++i) {
        volatile int tag = 0;
        prepared[i] = hook_prepare(code + i * STRIDE, constant_detour, STOLEN);
        CHECK(prepared[i] != NULL);
        if (!prepared[i]) continue;
        CHECK(((scratch_fn)prepared[i])(i, &tag) == 2 * i && tag == 1);
        CHECK(hook_commit(prepared[i]) == B2_PATCH_OK);
        CHECK(((scratch_fn)(code + i * STRIDE))(i, &tag) == i + 1000 && tag == 2);
    }
    CHECK(hook_owned_count() == before + COUNT);
    CHECK(hook_installed_count() == COUNT);
    CHECK(!hook_prepare(code, constant_detour, STOLEN));
    /* Registry growth cannot move or invalidate the first callback. */
    if (prepared[0]) {
        volatile int tag = 0;
        CHECK(((scratch_fn)prepared[0])(23, &tag) == 46 && tag == 1);
    }
    /* Bulk teardown retains just the failed record for a later retry. */
    sh_patch_test_faults(1, 0, 0);
    CHECK(hook_unpatch_all() == COUNT - 1);
    CHECK(hook_owned_count() == before + 1 && hook_installed_count() == 0);
    sh_patch_test_faults(0, 0, 0);
    CHECK(hook_unpatch_all() == 1);
    CHECK(hook_owned_count() == before);
    for (int i = 0; i < COUNT; ++i)
        CHECK(!memcmp(code + i * STRIDE, ORIG_CODE, sizeof(ORIG_CODE)));
    VirtualFree(code, 0, MEM_RELEASE);
}

typedef struct preparation_race { void *target, *tramp; HANDLE start; } preparation_race;

static DWORD WINAPI prepare_thread(void *context)
{
    preparation_race *race = context;
    WaitForSingleObject(race->start, INFINITE);
    race->tramp = hook_prepare(race->target, constant_detour, STOLEN);
    return 0;
}

static void concurrent_preparation_has_one_owner(void)
{
    enum { COUNT = 8 };
    HANDLE threads[COUNT], start = CreateEvent(NULL, TRUE, FALSE, NULL);
    preparation_race race[COUNT] = {0};
    unsigned char *code = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int created = 0, owners = 0, before = hook_owned_count();
    CHECK(start && code);
    if (!start || !code) goto done;
    memcpy(code, ORIG_CODE, sizeof(ORIG_CODE));
    for (int i = 0; i < COUNT; ++i) {
        race[i].target = code; race[i].start = start;
        HANDLE thread = CreateThread(NULL, 0, prepare_thread, &race[i], 0, NULL);
        CHECK(thread != NULL);
        if (thread) threads[created++] = thread;
    }
    SetEvent(start);
    if (created) WaitForMultipleObjects(created, threads, TRUE, INFINITE);
    for (int i = 0; i < created; ++i) CloseHandle(threads[i]);
    for (int i = 0; i < COUNT; ++i) if (race[i].tramp) ++owners;
    CHECK(owners == 1 && hook_owned_count() == before + 1);
    for (int i = 0; i < COUNT; ++i) if (race[i].tramp) CHECK(hook_unpatch(race[i].tramp));
    CHECK(hook_owned_count() == before);
done:
    if (start) CloseHandle(start);
    if (code) VirtualFree(code, 0, MEM_RELEASE);
}

/* An observer can hook a target after its clean scan and before preparation.
 * Copying its 5-byte jump would re-evaluate the displacement at the trampoline:
 * the production fault executed trampoline + 5 + disp instead of the observer. */
static void foreign_detour_after_scan_is_refused(void)
{
    static const unsigned char forms[][14] = {
        { 0xE9, 0x00, 0x01, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90 },            /* jmp rel32 */
        { 0xEB, 0x40 },                                                    /* jmp rel8 */
        { 0xFF, 0x25, 0, 0, 0, 0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 },
        { 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF, 0xE0 },                /* mov rax; jmp rax */
        { 0x49, 0xBB, 1, 2, 3, 4, 5, 6, 7, 8, 0x41, 0xFF, 0xE3 },          /* mov r11; jmp r11 */
        { 0x68, 1, 2, 3, 4, 0xC3 },                                        /* push; ret */
        { 0xE8, 0x00, 0x01, 0x00, 0x00 },                                  /* call rel32 */
    };
    static const unsigned char lea_code[] = {
        0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00, 0x48, 0x87, 0xC0, 0x48, 0x87, 0xC0, 0x90, 0xC3
    };
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int baseline = hook_owned_count();
    volatile int tag = 0;
    CHECK(code != NULL); if (!code) return;
    CHECK(!hook_target_is_redirect(NULL));
    memcpy(code, ORIG_CODE, sizeof ORIG_CODE);
    CHECK(!hook_target_is_redirect(code));
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        unsigned char before[STOLEN];
        memcpy(code, ORIG_CODE, sizeof ORIG_CODE);
        memcpy(code, forms[i], sizeof forms[i] < STOLEN ? sizeof forms[i] : STOLEN);
        if (forms[i][0] == 0xEB) code[2] = 0x90;
        memcpy(before, code, STOLEN);
        CHECK(hook_target_is_redirect(code));
        CHECK(hook_prepare(code, detour, STOLEN) == NULL);
        CHECK(hook_prepare_rip_lea(code, detour, STOLEN, 7) == NULL);
        if (forms[i][0] != 0xE8) CHECK(hook_prepare_relative_call(code, detour, STOLEN, 0) == NULL);
        CHECK(!memcmp(code, before, STOLEN) && hook_owned_count() == baseline);
    }
    /* The explicit relocation still owns an entry call; the clean window still prepares. */
    memcpy(code, ORIG_CODE, sizeof ORIG_CODE);
    published_original = (scratch_fn)hook_prepare(code, detour, STOLEN);
    CHECK(published_original != NULL);
    if (published_original) {
        CHECK(published_original(5, &tag) == 10 && tag == 1);
        CHECK(hook_unpatch((void *)published_original));
        published_original = NULL;
    }
    memcpy(code + 64, lea_code, sizeof lea_code);
    {
        void *lea = hook_prepare_rip_lea(code + 64, detour, 14, 0);
        CHECK(lea != NULL);
        if (lea) CHECK(hook_unpatch(lea));
    }
    CHECK(hook_owned_count() == baseline);
    VirtualFree(code, 0, MEM_RELEASE);
}

int main(void)
{
    many_simultaneous_hooks();
    concurrent_preparation_has_one_owner();
    rip_lea_preserves_addresses();
    rip_load_preserves_values();
    conditional_branch_preserves_paths();
    rip_compare_preserves_register_and_flags();
    relative_call_preserves_execution();
    foreign_detour_after_scan_is_refused();
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
