/* The static-model guard: its surface-table check on authored tables, and the
 * installed detour on an authored update whose head, counter compare and list
 * LEA have the engine's shape (no extracted code). */
#include "../src/fault_shield/static_model_guard.c"

static int failures, g_emits;
#define CHECK(x) do { if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

void backend_log(const char *message) { (void)message; }
void shield_emit(const shield_fault *f) { (void)f; ++g_emits; }
uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (results[i].name && !strcmp(results[i].name, name) && results[i].status == SIG_OK) return results[i].addr;
    return 0;
}

/* An engine-object stand-in: its first word is an address inside this image. */
static const uintptr_t k_vtable_word = 0x1234;
typedef struct fake_material { uintptr_t vtable; uint8_t rest[0x70]; } fake_material;

/* A static model: name at +8, level +0x28, state +0x2C, refs +0x64,
 * refcounted +0x68, table +0x70, count +0x78, capacity +0x7C. */
typedef struct fake_model { uint8_t b[0xD0]; } fake_model;

static void model_init(fake_model *m, const char *name, uint8_t *table, int count, int cap)
{
    memset(m, 0, sizeof *m);
    *(const char **)(m->b + RES_NAME_OFF) = name;
    *(unsigned *)(m->b + RES_LEVEL_OFF) = 4;
    *(uint8_t **)(m->b + SM_SURFACES_OFF) = table;
    *(int *)(m->b + SM_SURFACE_COUNT_OFF) = count;
    *(int *)(m->b + SM_SURFACE_CAP_OFF) = cap;
}

static void surface_init(uint8_t *table, int i, const void *material, const void *geometry)
{
    uint8_t *s = table + (size_t)i * SM_SURFACE_STRIDE;
    memset(s, 0, SM_SURFACE_STRIDE);
    *(const void **)(s + SMS_MATERIAL_OFF) = material;
    *(const void **)(s + SMS_GEOMETRY_OFF) = geometry;
}

static fake_model g_empty_model, g_good, g_bad, g_old;
static const char *g_last_name;
static int g_materialize_calls;

static void *fake_materialize(void *list, const char *name, char make_default, char quiet)
{
    (void)list; (void)make_default; (void)quiet;
    ++g_materialize_calls;
    g_last_name = name;
    if (!strcmp(name, "_empty")) return &g_empty_model;
    if (!strcmp(name, "models/old")) return &g_bad;      /* looked up again: a garbage table */
    if (!strcmp(name, "models/good")) return &g_good;
    return NULL;
}

/* The authored update: the engine's 16-byte head (PUSH RDI; SUB RSP,0x110;
 * MOV RAX,[RIP+cookie]), then it records self and self's backing, and returns.
 * The counter compare and the list LEA sit at the engine's offsets and are
 * only decoded at install. Data lives in the same page from 0x800. */
enum { DATA = 0x800, COOKIE = DATA, SEEN_SELF = DATA + 8, SEEN_BACKING = DATA + 16, LIST = 0x900 };

static void put_rel(uint8_t *code, size_t disp_at, size_t next, size_t target)
{
    int32_t d = (int32_t)((intptr_t)target - (intptr_t)next);
    memcpy(code + disp_at, &d, 4);
}

static uint8_t *author_update(int good_shape)
{
    uint8_t *c = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    size_t at = 0;
    if (!c) return NULL;
    memset(c, 0xCC, 4096);
    memset(c + DATA, 0, 4096 - DATA);
    { static const uint8_t head[] = { 0x40,0x57, 0x48,0x81,0xEC,0x10,0x01,0x00,0x00, 0x48,0x8B,0x05,0,0,0,0 };
      memcpy(c, head, sizeof head); put_rel(c, 12, 16, COOKIE); at = 16; }
    /* mov [rip+SEEN_SELF],rcx */
    c[at] = 0x48; c[at + 1] = 0x89; c[at + 2] = 0x0D; put_rel(c, at + 3, at + 7, SEEN_SELF); at += 7;
    /* mov rax,[rcx+0xE90] ; mov [rip+SEEN_BACKING],rax */
    { static const uint8_t ld[] = { 0x48,0x8B,0x81,0x90,0x0E,0x00,0x00 }; memcpy(c + at, ld, sizeof ld); at += sizeof ld; }
    c[at] = 0x48; c[at + 1] = 0x89; c[at + 2] = 0x05; put_rel(c, at + 3, at + 7, SEEN_BACKING); at += 7;
    /* mov eax,1 ; add rsp,0x110 ; pop rdi ; ret */
    { static const uint8_t tail[] = { 0xB8,0x01,0x00,0x00,0x00, 0x48,0x81,0xC4,0x10,0x01,0x00,0x00, 0x5F, 0xC3 };
      memcpy(c + at, tail, sizeof tail); }
    /* CMP EAX,[RIP+counter] at +0x3E2 and LEA RCX,[RIP+list] at +0x3FA (never executed). */
    c[SMU_COUNTER_CMP_OFF] = 0x3B; c[SMU_COUNTER_CMP_OFF + 1] = 0x05;
    put_rel(c, SMU_COUNTER_CMP_OFF + 2, SMU_COUNTER_CMP_OFF + 6, LIST + (good_shape ? 0x60 : 0x58));
    c[SMU_LIST_LEA_OFF] = 0x48; c[SMU_LIST_LEA_OFF + 1] = 0x8D; c[SMU_LIST_LEA_OFF + 2] = 0x0D;
    put_rel(c, SMU_LIST_LEA_OFF + 3, SMU_LIST_LEA_OFF + 7, LIST);
    return c;
}

typedef uint64_t (*update_fn)(void *self);

/* A fake idMemLocal: 32 heap ids at +0x44, depth at +0xC4. */
static uint8_t g_mem[0x100];
static void *fake_mem_get(void) { return g_mem; }
static void fake_mem_push(void *self, int heap)
{
    int *depth = (int *)((uint8_t *)self + 0xC4);
    ((int *)((uint8_t *)self + 0x44))[*depth] = heap;
    ++*depth;
}
static void fake_mem_pop(void *self) { --*(int *)((uint8_t *)self + 0xC4); }
static int g_seen_heap = -9, g_increfs;
static void observe_incref(void *model)
{
    (void)model;
    ++g_increfs;
    g_seen_heap = smh_heap_now();
}

/* The authored IncReferenceCount: the engine's 15-byte head, then it calls
 * observe_incref(model) and returns. */
static uint8_t *author_incref(void)
{
    static const uint8_t head[] = { 0x40,0x53, 0x48,0x83,0xEC,0x60, 0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF };
    uint8_t *c = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    uintptr_t fn = (uintptr_t)observe_incref;
    size_t at = sizeof head;
    if (!c) return NULL;
    memset(c, 0xCC, 4096);
    memcpy(c, head, sizeof head);
    c[at++] = 0x48; c[at++] = 0xB8; memcpy(c + at, &fn, 8); at += 8;        /* mov rax,imm64 */
    c[at++] = 0xFF; c[at++] = 0xD0;                                          /* call rax */
    { static const uint8_t tail[] = { 0x48,0x83,0xC4,0x60, 0x5B, 0xC3 }; memcpy(c + at, tail, sizeof tail); }
    return c;
}

int main(void)
{
    static uint8_t good_table[3 * SM_SURFACE_STRIDE], bad_table[10 * SM_SURFACE_STRIDE];
    static uint8_t slot_table[2 * SM_SURFACE_STRIDE], heap_mat_table[SM_SURFACE_STRIDE];
    static uint8_t geometry[SMS_GEOMETRY_SIZE];
    static uint8_t self[0xEC0];
    fake_material mat = { (uintptr_t)&k_vtable_word };
    fake_material *heap_mat = (fake_material *)calloc(1, sizeof *heap_mat);
    fake_model m;
    const uint8_t *base = (const uint8_t *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    uint8_t *reserved = (uint8_t *)VirtualAlloc(NULL, 65536, MEM_RESERVE, PAGE_READWRITE);
    uint8_t *code, *wrong;
    sig_result sigs[2];
    int surface, i;
    long fires;
    int *counter;

    if (!heap_mat || !reserved) return 1;
    heap_mat->vtable = (uintptr_t)heap_mat;      /* a heap word: not inside the image */
    g_image_lo = (uintptr_t)base;
    g_image_hi = g_image_lo + nt->OptionalHeader.SizeOfImage;

    /* The table check. */
    model_init(&m, "models/none", NULL, 0, 0);
    CHECK(smg_table_problem(m.b, &surface) == NULL);
    for (i = 0; i < 3; ++i) surface_init(good_table, i, i == 1 ? NULL : &mat, geometry);
    model_init(&m, "models/good", good_table, 3, 4);
    CHECK(smg_table_problem(m.b, &surface) == NULL && surface == -1);
    /* The crash's shape: surfaces 0..4 empty, surface 5 holds a cache slot of 1,
     * surface 9 a material pointer of 1. */
    for (i = 0; i < 10; ++i) surface_init(bad_table, i, NULL, NULL);
    *(uint64_t *)(bad_table + 9 * SM_SURFACE_STRIDE + SMS_MATERIAL_OFF) = 1;
    model_init(&m, "models/bad", bad_table, 10, 10);
    CHECK(smg_table_problem(m.b, &surface) != NULL && surface == 9);
    *(uint64_t *)(bad_table + 5 * SM_SURFACE_STRIDE + SMS_CACHE_B_OFF) = 1;
    CHECK(smg_table_problem(m.b, &surface) != NULL && surface == 5);
    surface_init(slot_table, 0, &mat, geometry);
    surface_init(slot_table, 1, &mat, geometry);
    *(uint64_t *)(slot_table + SM_SURFACE_STRIDE + SMS_CACHE_A_OFF) = 0x1000;
    model_init(&m, "models/slot", slot_table, 2, 2);
    CHECK(smg_table_problem(m.b, &surface) != NULL && surface == 1);
    *(uint64_t *)(slot_table + SM_SURFACE_STRIDE + SMS_CACHE_A_OFF) = 0;
    *(uint64_t *)(slot_table + SM_SURFACE_STRIDE + SMS_CACHE_C_OFF) = 0x1000;
    CHECK(smg_table_problem(m.b, &surface) != NULL && surface == 1);
    surface_init(heap_mat_table, 0, heap_mat, geometry);
    model_init(&m, "models/heapmat", heap_mat_table, 1, 1);
    CHECK(smg_table_problem(m.b, &surface) != NULL && surface == 0);
    surface_init(heap_mat_table, 0, &mat, reserved);   /* geometry in reserved, uncommitted memory */
    CHECK(smg_table_problem(m.b, &surface) != NULL && surface == 0);
    model_init(&m, "models/neg", good_table, -1, 4);
    CHECK(smg_table_problem(m.b, &surface) != NULL && surface == -1);
    model_init(&m, "models/cap", good_table, 3, 2);
    CHECK(smg_table_problem(m.b, &surface) != NULL);
    model_init(&m, "models/null", NULL, 3, 3);
    CHECK(smg_table_problem(m.b, &surface) != NULL);
    model_init(&m, "models/unmapped", reserved, 3, 3);
    CHECK(smg_table_problem(m.b, &surface) != NULL);

    /* Install refusals: an unresolved site, a wrong head, a counter that is
     * not the list's. */
    model_init(&g_empty_model, "_empty", NULL, 0, 0);
    model_init(&g_good, "models/good", good_table, 3, 4);
    for (i = 0; i < 10; ++i) surface_init(bad_table, i, NULL, NULL);
    *(uint64_t *)(bad_table + 9 * SM_SURFACE_STRIDE + SMS_MATERIAL_OFF) = 1;
    model_init(&g_bad, "models/bad", bad_table, 10, 10);
    model_init(&g_old, "models/old", good_table, 3, 4);
    code = author_update(1);
    wrong = author_update(0);
    if (!code || !wrong) return 1;
    sigs[0].name = "StaticModelUpdate"; sigs[0].status = SIG_OK; sigs[0].addr = 0; sigs[0].rva = 0;
    sigs[1].name = "ResourceMaterialize"; sigs[1].status = SIG_OK; sigs[1].addr = (uintptr_t)fake_materialize;
    sigs[1].rva = 0;
    CHECK(!sh_static_model_guard_install(base, sigs, 2));
    sigs[0].addr = (uintptr_t)wrong;
    CHECK(!sh_static_model_guard_install(base, sigs, 2));
    sigs[0].addr = (uintptr_t)(code + 1);
    CHECK(!sh_static_model_guard_install(base, sigs, 2));
    sigs[0].addr = (uintptr_t)code;
    CHECK(sh_static_model_guard_install(base, sigs, 2));
    CHECK(sh_static_model_guard_install(base, sigs, 2));    /* idempotent */
    CHECK(g_list == code + LIST && g_counter == (const int *)(code + LIST + 0x60));
    counter = (int *)(code + LIST + 0x60);
    *counter = 7;

    /* A healthy new render model builds from its backing; nothing is logged. */
    memset(self, 0, sizeof self);
    *(void **)(self + RMS_BACKING_OFF) = &g_good;
    *(int *)(self + RMS_MARKER_OFF) = -1;
    *(int *)(self + RMS_PARM_SEEN_OFF) = -1;
    fires = sh_static_model_guard_fires();
    g_emits = 0;
    CHECK(((update_fn)code)(self) == 1);
    CHECK(*(void **)(code + SEEN_SELF) == self && *(void **)(code + SEEN_BACKING) == &g_good);
    CHECK(sh_static_model_guard_fires() == fires && g_emits == 0);

    /* A new render model whose backing's table is garbage is built empty. */
    *(void **)(self + RMS_BACKING_OFF) = &g_bad;
    *(int *)(self + RMS_MARKER_OFF) = -1;
    CHECK(((update_fn)code)(self) == 1);
    CHECK(*(void **)(code + SEEN_BACKING) == &g_empty_model);
    CHECK(*(int *)(self + RMS_MARKER_OFF) == -1);
    CHECK(sh_static_model_guard_fires() == fires + 1 && g_emits == 1);

    /* A built model that reads nothing this update is left alone, even on a bad table. */
    *(void **)(self + RMS_BACKING_OFF) = &g_bad;
    *(int *)(self + RMS_MARKER_OFF) = 7;
    *(int *)(self + RMS_PARM_SEEN_OFF) = 0;
    *(int *)(self + RMS_PARM_OFF) = 0;
    CHECK(((update_fn)code)(self) == 1);
    CHECK(*(void **)(code + SEEN_BACKING) == &g_bad && sh_static_model_guard_fires() == fires + 1);

    /* A changed custom material re-reads the table: checked, drawn empty, logged once per backing. */
    *(int *)(self + RMS_PARM_OFF) = 1;
    CHECK(((update_fn)code)(self) == 1);
    CHECK(*(void **)(code + SEEN_BACKING) == &g_empty_model && *(int *)(self + RMS_MARKER_OFF) == -1);
    CHECK(sh_static_model_guard_fires() == fires + 2 && g_emits == 1);

    /* The reload counter moved: the engine looks the backing up again by name.
     * The guard makes the same call; a garbage result is not copied. */
    *(void **)(self + RMS_BACKING_OFF) = &g_old;
    *(int *)(self + RMS_MARKER_OFF) = 6;
    *(int *)(self + RMS_PARM_SEEN_OFF) = 0;
    *(int *)(self + RMS_PARM_OFF) = 0;
    g_materialize_calls = 0;
    CHECK(((update_fn)code)(self) == 1);
    CHECK(g_materialize_calls == 1 && g_last_name && !strcmp(g_last_name, "models/old"));
    CHECK(*(void **)(code + SEEN_BACKING) == &g_empty_model && *(int *)(self + RMS_MARKER_OFF) == -1);
    /* A good result is left to the engine. */
    *(void **)(self + RMS_BACKING_OFF) = &g_good;
    *(int *)(self + RMS_MARKER_OFF) = 6;
    CHECK(((update_fn)code)(self) == 1);
    CHECK(*(void **)(code + SEEN_BACKING) == &g_good && *(int *)(self + RMS_MARKER_OFF) == 6);

    /* No backing: the engine returns at once; the guard does nothing. */
    *(void **)(self + RMS_BACKING_OFF) = NULL;
    *(int *)(self + RMS_MARKER_OFF) = -1;
    fires = sh_static_model_guard_fires();
    CHECK(((update_fn)code)(self) == 1 && sh_static_model_guard_fires() == fires);

    CHECK(hook_unpatch((void *)g_orig_smu));
    g_orig_smu = NULL;

    /* A session static model reloads its surfaces in process heap 0. */
    {
        sig_result hs[4];
        uint8_t *inc = author_incref();
        fake_model cap;
        if (!inc) return 1;
        hs[0].name = "StaticModelIncRef"; hs[0].status = SIG_OK; hs[0].addr = (uintptr_t)inc;
        hs[0].rva = (uint32_t)((uintptr_t)inc - (uintptr_t)base);
        hs[1].name = "MemLocalGet"; hs[2].name = "MemLocalPushHeap"; hs[3].name = "MemLocalPopHeap";
        hs[1].addr = (uintptr_t)fake_mem_get; hs[2].addr = (uintptr_t)fake_mem_push; hs[3].addr = (uintptr_t)fake_mem_pop;
        for (i = 1; i < 4; ++i) { hs[i].status = SIG_OK; hs[i].rva = (uint32_t)(hs[i].addr - (uintptr_t)base); }
        /* Unresolved heap stack: refused. */
        CHECK(!sh_static_model_heap_rule_install(base, hs, 1));
        CHECK(sh_static_model_heap_rule_install(base, hs, 4));
        CHECK(sh_static_model_heap_rule_install(base, hs, 4));
        /* The door cap's shape: level 4, refcounted, no surfaces, 11 declared; a map heap (3) on top. */
        model_init(&cap, "models/snapmaps/door_caps/cap_05.lwo", NULL, 0, 0);
        *(uint8_t *)(cap.b + SM_REFCOUNTED_OFF) = 1;
        *(int *)(cap.b + SM_DECLARED_OFF) = 11;
        memset(g_mem, 0, sizeof g_mem);
        fake_mem_push(g_mem, 3);
        g_emits = 0;
        ((void (*)(void *))inc)(cap.b);
        CHECK(g_increfs == 1 && g_seen_heap == 0);                     /* ran in process heap 0 */
        CHECK(*(int *)(g_mem + 0xC4) == 1 && smh_heap_now() == 3);     /* and the stack is restored */
        CHECK(sh_static_model_heap_moves() == 1 && g_emits == 1);
        /* Not moved: process heap 0 already, a level-1 model, a model with surfaces, not refcounted, nothing declared. */
        fake_mem_pop(g_mem); ((void (*)(void *))inc)(cap.b); CHECK(g_seen_heap == 0 && sh_static_model_heap_moves() == 1);
        fake_mem_push(g_mem, 3);
        *(unsigned *)(cap.b + RES_LEVEL_OFF) = 1;
        ((void (*)(void *))inc)(cap.b); CHECK(g_seen_heap == 3);
        *(unsigned *)(cap.b + RES_LEVEL_OFF) = 4;
        *(int *)(cap.b + SM_SURFACE_COUNT_OFF) = 2;
        ((void (*)(void *))inc)(cap.b); CHECK(g_seen_heap == 3);
        *(int *)(cap.b + SM_SURFACE_COUNT_OFF) = 0;
        *(uint8_t *)(cap.b + SM_REFCOUNTED_OFF) = 0;
        ((void (*)(void *))inc)(cap.b); CHECK(g_seen_heap == 3);
        *(uint8_t *)(cap.b + SM_REFCOUNTED_OFF) = 1;
        *(int *)(cap.b + SM_DECLARED_OFF) = 0;
        ((void (*)(void *))inc)(cap.b); CHECK(g_seen_heap == 3);
        CHECK(sh_static_model_heap_moves() == 1 && g_increfs == 6);
        CHECK(hook_unpatch((void *)g_orig_incref));
        g_orig_incref = NULL;
        CHECK(inc[0] == 0x40 && inc[1] == 0x53 && inc[14] == 0xFF);
        VirtualFree(inc, 0, MEM_RELEASE);
    }
    CHECK(code[0] == 0x40 && code[1] == 0x57 && code[9] == 0x48 && code[10] == 0x8B && code[11] == 0x05);
    VirtualFree(code, 0, MEM_RELEASE);
    VirtualFree(wrong, 0, MEM_RELEASE);
    VirtualFree(reserved, 0, MEM_RELEASE);
    free(heap_mat);
    printf("static_model_guard_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
