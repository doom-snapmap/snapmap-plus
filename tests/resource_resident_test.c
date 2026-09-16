/* Exercise native refresh ordering, identity selection, recovery and scopes. */
#include "../src/backend/resource_resident.c"
static int failures, rebuilds, loads, updates, strings, defaults, fault, lookup_leaves_pending, source_mode = 3;
/* What the virtual-texture rebind observed: the heap on top of the scope stack,
 * how many reloads had finished, whether the consumer update had already run
 * and whether the renderer was still held. */
static int rebinds, rebind_heap, rebind_loads, rebind_updates, rebind_held;
/* Bitmask of the objects a case expects the pass to reconstruct. */
static int expect_reconstructed = 3;
static unsigned char heap_object[0xd0], renderer_object[0x20], manager_object[0x30];
/* Wide enough for every field the pass reads, including the implicit-text
 * flag at +0x48. */
static unsigned char objects[3][0x80];
static void *render_table[35], *resource_table[12], *entries[3], *head, *renderer;
static DWORD main_thread;
static const char *paths[3] = {"cooked/model/body.bmodel", "cooked/material/skin.bmaterial", "cooked/model/unrelated.bmodel"};
static int counts[3];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
const sh_resource_catalog *sh_package_runtime_catalog(void) { return NULL; }
size_t sh_resource_catalog_find_path(const sh_resource_catalog *catalog, const char *path,
    const sh_resource_catalog_entry *const **rows)
{
    static sh_resource_catalog_entry alias = {"model", "body.lwo", "shaders/body.fspv", 0};
    static const sh_resource_catalog_entry *found[] = {&alias};
    (void)catalog; *rows = NULL;
    if (!strcmp(path, alias.path)) { *rows = found; return 1; }
    return 0;
}
static void *heap_get(void) { return heap_object; }
static void heap_push(void *self, int index)
{
    int *depth = (int *)((unsigned char *)self + 0xc4);
    *(int *)((unsigned char *)self + 0x44 + 4 * (*depth)++) = index;
}
static void heap_pop(void *self) { --*(int *)((unsigned char *)self + 0xc4); }
static int mode_get(void) { return source_mode; }
static int index_of(void *self) { return (int)(((unsigned char *)self - objects[0]) / sizeof(objects[0])); }
static void *path_get(void *self, void *out, int mode)
{
    const char *path = mode ? "unused" : paths[index_of(self)];
    memset(out, 0, 0x30);
    *(int *)((unsigned char *)out + 8) = (int)strlen(path);
    *(char **)((unsigned char *)out + 0x10) = _strdup(path); strings++;
    if (fault == 1 && index_of(self) == 1) *(int *)((unsigned char *)out + 8) = -1;
    return out;
}
static void string_free(void *self) { free(*(void **)((unsigned char *)self + 0x10)); strings--; }
static void suspend_adjust(void *self, int delta) { *(int *)((unsigned char *)self + 0x14) += delta; }
static void synchronize(void *self, unsigned char a, unsigned char b, unsigned char c)
{ (void)self; CHECK(a == 0 && b == 0 && c == 1); }
static void update(void *self)
{ (void)self; updates++; if (fault == 5) RaiseException(0xe0800005, 0, 0, NULL); }
static void reconstruct(void *self)
{
    unsigned char *object = self;
    /* Permanent lifetime is released for the synchronous destructor; a
     * map-scoped identity is already below it. */
    CHECK(*(int *)(object + 0x28) != 4);
    CHECK(*(int *)(renderer_object + 0x14) == 1);
    rebuilds++; counts[index_of(self)]++; object[0x2c] = 0;
    if (fault == 2 && index_of(self) == 1) RaiseException(0xe0800002, 0, 0, NULL);
}
/* Reconstruction drops a material's virtual-texture parm binding. The engine
 * rebuilds it only in its next map load, inside the map/persist heap scope, so a
 * permanent material ends up owning storage that ResetPersistHeap frees on the
 * way out of Play. The pass must rebind while it still holds process heap 0. */
static void rebind(void)
{
    int depth = *(int *)(heap_object + 0xc4);
    rebinds++;
    rebind_heap = depth ? *(int *)(heap_object + 0x44 + 4 * (depth - 1)) : -1;
    rebind_loads = loads; rebind_updates = updates;
    rebind_held = *(int *)(renderer_object + 0x14);
    if (fault == 6) RaiseException(0xe0800006, 0, 0, NULL);
}
static void *lookup(void *manager, const char *name);
static void make_default(void *self)
{
    defaults++; CHECK(((unsigned char *)self)[0x2c] & RR_DEFAULT);
    CHECK(!(((unsigned char *)self)[0x2c] & RR_PENDING));
    CHECK(counts[0] && counts[1]);
}
static void load(void *self)
{
    int index = index_of(self);
    unsigned char *object = self;
    CHECK(*(int *)(object + 0x28) == 4);
    /* The consumer may never see an unreconstructed dependency. Only the
     * objects this pass selected are expected to have been rebuilt. */
    for (int i = 0; i < 3; i++) if (expect_reconstructed & (1 << i)) CHECK(counts[i]);
    CHECK(!(object[0x2c] & RR_PENDING)); loads++;
    if (fault == 3 && index == 1) RaiseException(0xe0800003, 0, 0, NULL);
    if (!index && (objects[1][0x2c] & RR_PENDING) && !lookup_leaves_pending)
        lookup(manager_object, "skin");
    if (fault == 4) object[0x2c] |= RR_DEFAULT;
}
static void *lookup(void *manager, const char *name)
{
    CHECK(manager == manager_object);
    for (int i = 0; i < 3; i++) if (!strcmp(*(const char **)(objects[i] + 8), name)) {
        if ((objects[i][0x2c] & RR_PENDING) && !lookup_leaves_pending) {
            objects[i][0x2c] &= (unsigned char)~RR_PENDING; load(objects[i]);
        }
        return objects[i];
    }
    return NULL;
}
static void reset(void)
{
    sh_resource_graph_frame frame;
    rr_free(g_rr_recovery); g_rr_recovery = NULL;
    memset(heap_object, 0, sizeof(heap_object)); memset(objects, 0, sizeof(objects));
    memset(manager_object, 0, sizeof(manager_object)); memset(renderer_object, 0, sizeof(renderer_object));
    memset(counts, 0, sizeof(counts)); rebuilds = loads = updates = strings = defaults = fault = lookup_leaves_pending = 0;
    rebinds = 0; rebind_heap = rebind_loads = rebind_updates = rebind_held = -1;
    expect_reconstructed = 3;
    source_mode = 3; main_thread = GetCurrentThreadId(); head = manager_object; renderer = renderer_object;
    *(const char **)(manager_object + 8) = "model";
    *(void ***)(manager_object + 0x20) = entries; *(int *)(manager_object + 0x28) = 3;
    resource_table[0x38/8] = path_get; resource_table[0x50/8] = make_default;
    render_table[0x80/8] = synchronize; render_table[0x108/8] = update; render_table[0x110/8] = suspend_adjust;
    *(void ***)renderer_object = render_table;
    for (int i = 0; i < 3; i++) {
        entries[i] = objects[i]; *(void ***)objects[i] = resource_table;
        *(const char **)(objects[i] + 8) = i == 0 ? "body.lwo" : i == 1 ? "skin" : "unrelated";
        *(void **)(objects[i] + 0x10) = manager_object; *(int *)(objects[i] + 0x28) = 4;
    }
    g_rr.head = &head; g_rr.renderer = &renderer; g_rr.thread = &main_thread;
    g_rr.heap = (sh_process_heap_api){heap_get, heap_push, heap_pop};
    g_rr.mode = mode_get; g_rr.string_free = string_free;
    g_rr.reconstruct = reconstruct; g_rr.load = load; g_rr.lookup = lookup;
    g_rr.rebind = rebind;
    sh_resource_graph_test_reset();
    /* Identity dependency exists, but the primary file was loaded before
     * instrumentation. Native path/catalog seeds must still find consumers. */
    sh_resource_graph_begin(&frame, "model", "body.lwo"); sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "model", "skin"); sh_resource_graph_reference("model", "body.lwo");
    sh_resource_graph_end(&frame, 1);
}
static sh_resource_resident *begin(const char *path, int restoring, char error[256])
{
    sh_package_change change = {(char *)path, SH_PACKAGE_RESOURCE_REPLACED};
    sh_package_changes changes = {&change, 1};
    return sh_resource_resident_begin(&changes, restoring, error, 256);
}
static void check_restored(void)
{
    CHECK(!strings && !*(int *)(heap_object + 0xc4) && !*(int *)(renderer_object + 0x14));
    for (int i = 0; i < 3; i++) CHECK(!(objects[i][0x2c] & RR_PENDING) && *(int *)(objects[i] + 0x28) == 4);
    CHECK(source_mode == 3 && !counts[2]);
}
static void success_cases(void)
{
    char error[256];
    for (int alternate = 0; alternate < 2; alternate++) {
        sh_resource_resident *pass;
        reset(); lookup_leaves_pending = alternate;
        pass = begin(alternate ? "shaders/body.fspv" : paths[0], 0, error);
        CHECK(pass && pass->count == 2 && !rebuilds && !loads);
        if (!pass) continue;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && rebuilds == 2);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 2 && updates == 1);
        /* One rebind, after every reload, before the consumer update, under
         * process heap 0 with the renderer still held. A repeated drain is
         * idempotent and must not rebind again. */
        CHECK(rebinds == 1 && rebind_heap == 0 && rebind_loads == 2 && rebind_updates == 0 && rebind_held == 1);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && rebinds == 1);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    reset();
    {
        sh_package_change row = {(char *)paths[0], SH_PACKAGE_RESOURCE_REMOVED};
        sh_package_changes changes = {&row, 1};
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, error, sizeof(error));
        CHECK(pass && pass->count == 2); if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && defaults == 0);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 1 && defaults == 1);
        CHECK(rebinds == 1 && rebind_heap == 0);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        CHECK(objects[0][0x2c] & RR_DEFAULT);
    }
    reset();
    {
        sh_resource_resident *pass = begin(paths[0], 0, error);
        CHECK(pass != NULL); if (!pass) return;
        sh_resource_resident_external(pass, objects[0]);
        counts[0] = 1; /* Existing declaration owner reconstructed this one. */
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && rebuilds == 1);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 1);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    reset();
    {
        /* objects[1] is the consumer reached through the recorded graph. With
         * the engine's implicit-text flag set it has no source to re-read, so
         * the pass must leave it alone instead of reconstructing it into a
         * default -- the entitydef:world activation refusal. */
        sh_resource_resident *pass;
        objects[1][0x48] = 1; expect_reconstructed = 1;
        pass = begin(paths[0], 0, error);
        CHECK(pass && pass->count == 1);
        if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && rebuilds == 1 && counts[1] == 0);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 1);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        objects[1][0x48] = 0;
    }
    reset();
    {
        /* The same flag on a resource whose own path changed still refreshes:
         * that provider supplies its bytes. */
        sh_resource_resident *pass;
        objects[0][0x48] = 1;
        pass = begin(paths[0], 0, error);
        CHECK(pass && pass->count == 2);
        if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && rebuilds == 2);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 2);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        objects[0][0x48] = 0;
    }
    reset();
    {
        /* A map-scoped consumer belongs to the map that built it. The pass
         * leaves it alone and the next map load rebuilds it. */
        sh_resource_resident *pass;
        *(int *)(objects[1] + 0x28) = 1; expect_reconstructed = 1;
        pass = begin(paths[0], 0, error);
        CHECK(pass && pass->count == 1);
        if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && rebuilds == 1 && counts[1] == 0);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 1);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        CHECK(*(int *)(objects[1] + 0x28) == 1);   /* still map-scoped */
        *(int *)(objects[1] + 0x28) = 4; check_restored();
    }
    reset();
    {
        /* A map-scoped resource this provider does supply is refreshed, works
         * at permanent lifetime for nested loads, and is handed back at the
         * tier it had so its map still retires it. */
        sh_resource_resident *pass;
        *(int *)(objects[0] + 0x28) = 1;
        pass = begin(paths[0], 0, error);
        CHECK(pass && pass->count == 2);
        if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && counts[0] == 1);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 2);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        CHECK(*(int *)(objects[0] + 0x28) == 1);
        *(int *)(objects[0] + 0x28) = 4; check_restored();
    }
    reset();
    {
        /* Touch keeps a pre-existing map-scoped identity map-scoped; only an
         * identity created during the pass becomes permanent. */
        unsigned char created[0x80] = {0};
        sh_resource_resident *pass;
        *(int *)(objects[2] + 0x28) = 1;
        pass = begin(paths[0], 0, error);
        CHECK(pass != NULL); if (!pass) return;
        sh_resource_resident_touch(objects[2]);
        CHECK(*(int *)(objects[2] + 0x28) == 1);
        *(int *)(created + 0x28) = 1;
        sh_resource_resident_touch(created);
        CHECK(*(int *)(created + 0x28) == 4);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        *(int *)(objects[2] + 0x28) = 4; check_restored();
    }
    reset();
    {
        sh_resource_resident *pass = begin(paths[0], 0, error);
        CHECK(pass != NULL); if (!pass) return;
        *(int *)(objects[2] + 0x28) = 1;
        sh_resource_resident_touch(objects[2]); CHECK(*(int *)(objects[2] + 0x28) == 4);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        *(int *)(objects[2] + 0x28) = 1;
        sh_resource_resident_touch(objects[2]); CHECK(*(int *)(objects[2] + 0x28) == 1);
    }
}
static void failure_cases(void)
{
    char error[256];
    for (int fail = 1; fail <= 6; fail++) {
        sh_resource_resident *pass;
        reset(); fault = fail; pass = begin(paths[0], 0, error);
        if (fail == 1) { CHECK(!pass && !rebuilds); check_restored(); continue; }
        CHECK(pass != NULL); if (!pass) continue;
        int ok = sh_resource_resident_reconstruct(pass, error, sizeof(error));
        if (ok) ok = sh_resource_resident_drain(pass, error, sizeof(error));
        CHECK(!ok && !sh_resource_resident_end(pass, ok, error, sizeof(error)));
        check_restored(); CHECK(g_rr_recovery != NULL);
        /* Failed parsing can erase the graph. Recovery retains both affected
         * identities instead of rediscovering a smaller set from new edges. */
        sh_resource_graph_test_reset(); fault = 0;
        pass = begin(paths[0], 1, error); CHECK(pass && pass->count == 2);
        if (!pass) continue;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)) && !g_rr_recovery);
        check_restored();
    }
    reset(); main_thread++;
    CHECK(!begin(paths[0], 0, error) && !rebuilds); main_thread--; check_restored();
    source_mode = 0; CHECK(!begin(paths[0], 0, error) && !rebuilds); source_mode = 3; check_restored();
    {
        sh_package_change rows[] = {{"z", 0}, {"a", 0}};
        sh_package_changes changes = {rows, 2};
        CHECK(!sh_resource_resident_begin(&changes, 0, error, sizeof(error))); check_restored();
    }
    {
        sh_package_changes changes = {0};
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, error, sizeof(error));
        CHECK(pass && !pass->count && pass->held); /* Declaration/policy rearm still needs renderer ownership. */
        CHECK(!sh_resource_resident_begin(&changes, 0, error, sizeof(error))); /* Nested capture cannot replace its owner. */
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
}
int main(void)
{
    success_cases(); failure_cases(); rr_free(g_rr_recovery); g_rr_recovery = NULL;
    sh_resource_graph_test_reset();
    printf("resident refresh: %d failure(s)\n", failures); return failures ? 1 : 0;
}
