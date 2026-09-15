/* Execute the installed instruction guard in an authored x64 function. */
#include "../src/fault_shield/serialized_entities_guard.c"

void backend_log(const char *message) { (void)message; }
static int failures;
#define CHECK(x) do { if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

typedef void (*exercise_fn)(void *entity, uint64_t index, uint64_t *observation);

static void release_guard(void)
{
    sh_patch_test_faults(0, 0, 0);
    if (g_index_patch.live) CHECK(code_unpatch(&g_index_patch) == B2_PATCH_OK);
    CHECK(!g_index_patch.live);
    if (!g_index_patch.live && g_index_relay) {
        VirtualFree(g_index_relay, 0, MEM_RELEASE);
        g_index_relay = NULL;
    }
    g_index_ready = 0;
}

int main(void)
{
    /* Set the native site's inputs, record flags, execute its MOV, record the
     * resulting state and return. This fixture contains no extracted code. */
    const uint8_t before[] = {
        0x55,                           /* push rbp */
        0x48,0x89,0xc8,                 /* mov rax,rcx */
        0x48,0x89,0xd5,                 /* mov rbp,rdx */
        0x83,0xfa,0x07,                 /* cmp edx,7: exercise differing flags */
        0x9c,0x41,0x59,                 /* pushfq; pop r9 */
        0x4d,0x89,0x08                  /* mov [r8],r9 */
    };
    const uint8_t after[] = {
        0x9c,0x41,0x59,                 /* pushfq; pop r9 */
        0x4d,0x89,0x48,0x08,            /* mov [r8+8],r9 */
        0x49,0x89,0x40,0x10,            /* mov [r8+16],rax */
        0x49,0x89,0x68,0x18,            /* mov [r8+24],rbp */
        0x49,0x89,0x48,0x20,            /* mov [r8+32],rcx */
        0x49,0x89,0x50,0x28,            /* mov [r8+40],rdx */
        0x4d,0x89,0x40,0x30,            /* mov [r8+48],r8 */
        0x5d,0xc3                       /* pop rbp; ret */
    };
    uint8_t *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    sig_result site = {"SerializedEntityIndexWrite", SIG_OK, 0, 0};
    SYSTEM_INFO info;
    MEMORY_BASIC_INFORMATION memory;
    DWORD old;
    const uint64_t indices[] = {7, 0, 0x7fffffff, 0xffffffff};
    size_t i;
    if (!code) return 1;
    GetSystemInfo(&info);
    memcpy(code, before, sizeof before);
    memcpy(code + sizeof before, index_write, sizeof index_write);
    memcpy(code + sizeof before + sizeof index_write, after, sizeof after);
    CHECK(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old));
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    site.addr = (uintptr_t)(code + sizeof before);

    CHECK(!sh_serialized_entities_guard_install(NULL, 0));
    site.status = SIG_OK_HOOKED;
    CHECK(!sh_serialized_entities_guard_install(&site, 1));
    CHECK(!g_index_relay && !g_index_patch.live);
    site.status = SIG_OK;
    ++site.addr;
    CHECK(!sh_serialized_entities_guard_install(&site, 1));
    CHECK(!g_index_relay && !g_index_patch.live);
    --site.addr;

    /* Failure before publication releases the unused relay. */
    sh_patch_test_faults(1, 0, 0);
    CHECK(!sh_serialized_entities_guard_install(&site, 1));
    CHECK(!g_index_relay && !g_index_patch.live);
    sh_patch_test_faults(0, 0, 0);

    /* Failed rollback retains its destination until restoration succeeds. */
    sh_patch_test_faults(2 | 8, 0, 0);
    CHECK(!sh_serialized_entities_guard_install(&site, 1));
    CHECK(g_index_relay && g_index_patch.live && !g_index_ready);
    sh_patch_test_faults(1, 0, 0);
    CHECK(!sh_serialized_entities_guard_install(&site, 1));
    CHECK(g_index_relay && g_index_patch.live && !g_index_ready);
    sh_patch_test_faults(0, 0, 0);
    CHECK(sh_serialized_entities_guard_install(&site, 1));
    CHECK(sh_serialized_entities_guard_install(&site, 1));
    CHECK(g_index_ready && g_index_relay && g_index_patch.live);
    if (!g_index_ready) return 1;
    CHECK(VirtualQuery(g_index_relay, &memory, sizeof memory) && memory.Protect == PAGE_EXECUTE_READ);
    CHECK(VirtualQuery(g_index_relay + info.dwPageSize, &memory, sizeof memory) && memory.Protect == PAGE_READWRITE);

    for (i = 0; i < sizeof indices / sizeof indices[0]; ++i) {
        uint8_t entity[0xb80], expected[sizeof entity];
        uint64_t observation[7] = {0};
        uint32_t value = (uint32_t)indices[i];
        memset(entity, 0xa5, sizeof entity);
        memcpy(expected, entity, sizeof entity);
        memcpy(expected + 0xb64, &value, sizeof value);
        ((exercise_fn)code)(entity, indices[i], observation);
        CHECK(!memcmp(entity, expected, sizeof entity));
        CHECK(observation[0] == observation[1]);
        CHECK(observation[2] == (uintptr_t)entity && observation[4] == (uintptr_t)entity);
        CHECK(observation[3] == indices[i] && observation[5] == indices[i]);
        CHECK(observation[6] == (uintptr_t)observation);
        CHECK(*(volatile LONG *)(g_index_relay + info.dwPageSize) == (LONG)i);
        ((exercise_fn)code)(NULL, indices[i], observation);
        CHECK(observation[0] == observation[1]);
        CHECK(observation[2] == 0 && observation[4] == 0);
        CHECK(observation[3] == indices[i] && observation[5] == indices[i]);
        CHECK(observation[6] == (uintptr_t)observation);
        CHECK(*(volatile LONG *)(g_index_relay + info.dwPageSize) == (LONG)i + 1);
    }
    release_guard();
    CHECK(!memcmp((void *)site.addr, index_write, sizeof index_write));
    CHECK(VirtualQuery(code, &memory, sizeof memory) && memory.Protect == PAGE_EXECUTE_READ);
    VirtualFree(code, 0, MEM_RELEASE);
    printf("serialized_entities_guard_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
