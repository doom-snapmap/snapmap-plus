/* Exercise native refresh ordering, identity selection, recovery and scopes. */
#include "../src/backend/resource_resident.c"
static int vmr_failed, vmr_collected;
int sh_virtual_material_runtime_status(char *error, size_t capacity)
{
    if (vmr_failed && error && capacity) snprintf(error, capacity, "synthetic atlas lookup failure");
    return !vmr_failed;
}
static char g_log[4096];
void backend_log(const char *message)
{
    size_t used = strlen(g_log), length = message ? strlen(message) : 0;
    if (!length || used + length + 2 >= sizeof(g_log)) return;
    memcpy(g_log + used, message, length); g_log[used + length] = 10; g_log[used + length + 1] = 0;
}

static int failures, rebuilds, loads, updates, strings, defaults, fault, lookup_leaves_pending, source_mode = 3;
static int render_fault, heap_pop_fault, expected_render_holds = 1;
/* What the virtual-texture rebind observed: the heap on top of the scope stack,
 * how many reloads had finished, whether the consumer update had already run
 * and whether the renderer was still held. */
static int rebinds, rebind_heap, rebind_loads, rebind_updates, rebind_held;
static int font_resolves, fresh_rebinds;
static int retained_alias;
int sh_package_runtime_serves_native_identity(const char *type, const char *name)
{ return retained_alias && !strcmp(type, "model") && !strcmp(name, "body.lwo"); }
/* Bitmask of the objects a case expects the pass to reconstruct. */
static int expect_reconstructed = 3;
static unsigned char heap_object[0xd0], renderer_object[0x20], manager_object[0x30], parm_manager[0x30];
static unsigned char font_list[0x30];
/* Wide enough for every field the pass reads, including the implicit-text
 * flag at +0x48. */
static unsigned char objects[3][0x800];
static void *render_table[35], *resource_table[12], *entries[3], *head, *renderer;
static DWORD main_thread;
static const char *paths[3] = {"cooked/model/body.bmodel", "cooked/material/skin.bmaterial", "cooked/model/unrelated.bmodel"};
static int counts[3];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
void sh_virtual_material_runtime_collect(void)
{
    CHECK(*(int *)(renderer_object + 0x14) == expected_render_holds);
    vmr_collected++;
}
static int vmr_providers, vmr_provider_failed, vmr_provider_loads = -1;
int sh_virtual_material_runtime_providers(char *error, size_t capacity)
{
    /* Private page providers are built inside the render scope, before any
     * reconstructed material can parse. */
    CHECK(*(int *)(renderer_object + 0x14) >= 1);
    vmr_providers++; vmr_provider_loads = loads;
    if (vmr_provider_failed && error && capacity) snprintf(error, capacity, "synthetic provider failure");
    return !vmr_provider_failed;
}
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
const sh_resource_catalog *sh_package_runtime_catalog(void) { return NULL; }
int sh_typeinfo_class_derives(const char *name, const char *base)
{
    CHECK(!strcmp(base, "idDeclTypeInfo"));
    return !strcmp(name, "TestInheritedDecl");
}
size_t sh_resource_catalog_find_path(const sh_resource_catalog *catalog, const char *path,
    const sh_resource_catalog_entry *const **rows)
{
    static sh_resource_catalog_entry alias = {"model", "body.lwo", "shaders/body.fspv", 0};
    static const sh_resource_catalog_entry *found[] = {&alias};
    (void)catalog; *rows = NULL;
    if (!strcmp(path, alias.path)) { *rows = found; return 1; }
    return 0;
}
size_t sh_resource_catalog_find(const sh_resource_catalog *catalog, const char *type, const char *name,
    const sh_resource_catalog_entry *const **rows)
{ (void)catalog; (void)type; (void)name; *rows = NULL; return 0; }
static void *heap_get(void) { return heap_object; }
static void heap_push(void *self, int index)
{
    int *depth = (int *)((unsigned char *)self + 0xc4);
    *(int *)((unsigned char *)self + 0x44 + 4 * (*depth)++) = index;
}
static void heap_pop(void *self)
{
    if (heap_pop_fault == 1) RaiseException(0xe0800010, 0, 0, NULL);
    --*(int *)((unsigned char *)self + 0xc4);
    if (heap_pop_fault == 2) RaiseException(0xe0800011, 0, 0, NULL);
}
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
static void suspend_adjust(void *self, int delta)
{
    if ((delta > 0 && render_fault == 1) || (delta < 0 && render_fault == 3))
        RaiseException(0xe0800012, 0, 0, NULL);
    *(int *)((unsigned char *)self + 0x14) += delta;
    if ((delta > 0 && render_fault == 2) || (delta < 0 && render_fault == 4))
        RaiseException(0xe0800013, 0, 0, NULL);
}
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
static void rebind_material(void *material)
{
    CHECK(material == objects[2]);
    CHECK(*(int *)(heap_object + 0xc4) == 1 && *(int *)(heap_object + 0x44) == 0);
    CHECK(*(int *)(renderer_object + 0x14) == 1);
    CHECK(!rebinds && !rebuilds && !loads && !updates);
    fresh_rebinds++;
    if (fault == 10) RaiseException(0xe0800015, 0, 0, NULL);
}
static void *lookup(void *manager, const char *name);
static void world_text_fonts(void *self)
{
    void **fonts = self;
    int depth = *(int *)(heap_object + 0xc4);
    CHECK(depth && *(int *)(heap_object + 0x44 + 4 * (depth - 1)) == 0);
    CHECK(!updates && !rebinds && *(int *)(renderer_object + 0x14) == 1);
    CHECK(!(objects[0][0x2c] & RR_PENDING));
    font_resolves++;
    if (fault == 8) RaiseException(0xe0800008, 0, 0, NULL);
    fonts[0] = objects[0]; fonts[1] = objects[1];
    fonts[2] = fault == 9 ? NULL : objects[2];
}
/* The engine default construction scope (discrete_default.c), as the backend wires it. The
 * depth may go negative here, so an unbalanced close is caught. A case can make the next
 * enters fail, or the next leaves report an out-of-order end. */
static int scope_depth, scoped_defaults, unscoped_loads, enter_fails, leave_misorders;
static const char *unready_family;
static int default_enter(sh_discrete_scope *scope)
{
    (void)scope;
    if (enter_fails) { enter_fails--; return 0; }
    scope_depth++; return 1;
}
static int default_leave(sh_discrete_scope *scope)
{
    (void)scope; scope_depth--;
    if (leave_misorders) { leave_misorders--; return 0; }
    return 1;
}
static int default_ready(const char *family) { return !unready_family || _stricmp(family, unready_family); }
/* R-321 dependency observation (dependency_cases): the image names a retirement default reads through the
 * hooked finds, how many it found and missed, and an object whose provider has no source, so its load leaves
 * the engine default. */
static const char *default_reads[3];
static int default_found, default_missing;
static void *unsupplied;
static void make_default(void *self)
{
    defaults++; CHECK(((unsigned char *)self)[0x2c] & RR_DEFAULT);
    CHECK(!(((unsigned char *)self)[0x2c] & RR_PENDING));
    for (int i = 0; i < 3; i++) if (expect_reconstructed & (1 << i)) CHECK(counts[i]);
    if (scope_depth > 0) scoped_defaults++;
    /* An engine error raised while the default is built (Error(6) on Vulkan before R-321). */
    if (fault == 7) RaiseException(0xe0800007, 0, 0, NULL);
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
    if (scope_depth <= 0) unscoped_loads++;
    if (fault == 3 && index == 1) RaiseException(0xe0800003, 0, 0, NULL);
    if (!index && (objects[1][0x2c] & RR_PENDING) && !lookup_leaves_pending)
        lookup(manager_object, "skin");
    if (fault == 4 && index == 1) object[0x2c] |= RR_DEFAULT;
    if (self == unsupplied) object[0x2c] |= RR_DEFAULT;
}
static void *lookup(void *manager, const char *name)
{
    CHECK(manager == manager_object || manager == parm_manager || manager == font_list);
    for (int i = 0; i < 3; i++) if (!strcmp(*(const char **)(objects[i] + 8), name)) {
        if ((objects[i][0x2c] & RR_PENDING) && !lookup_leaves_pending) {
            objects[i][0x2c] &= (unsigned char)~RR_PENDING; load(objects[i]);
        }
        return objects[i];
    }
    return NULL;
}
static void ledger_clear(void)
{
    while (g_rr_retirement_count) {
        rr_retirement *row = g_rr_retirements + --g_rr_retirement_count;
        free(row->type); free(row->name);
    }
}
static void reset(void)
{
    CHECK(!g_rr_scope_recovery && !g_rr_active);
    ledger_clear();
    CHECK(!scope_depth);
    scope_depth = scoped_defaults = unscoped_loads = enter_fails = leave_misorders = 0; unready_family = NULL;
    sh_resource_resident_set_default_construction(default_enter, default_leave, default_ready);
    sh_resource_resident_set_default_observation(NULL);
    sh_resource_resident_set_default_probe(NULL, NULL);
    sh_resource_resident_set_default_invoke(NULL);
    sh_resource_resident_set_default_origins(NULL,NULL);
    sh_resource_resident_set_instance_refresh(NULL);
    sh_resource_resident_set_instance_admit(NULL);
    sh_resource_resident_set_instance_transfer(NULL,NULL,NULL);
    memset(default_reads, 0, sizeof(default_reads)); default_found = default_missing = 0; unsupplied = NULL;
    render_fault = heap_pop_fault = 0; expected_render_holds = 1;
    vmr_failed = vmr_collected = 0;
    vmr_providers = vmr_provider_failed = 0; vmr_provider_loads = -1;
    retained_alias = 0;
    sh_resource_graph_frame frame;
    rr_free(g_rr_recovery); g_rr_recovery = NULL;
    memset(heap_object, 0, sizeof(heap_object)); memset(objects, 0, sizeof(objects));
    memset(manager_object, 0, sizeof(manager_object)); memset(renderer_object, 0, sizeof(renderer_object));
    memset(counts, 0, sizeof(counts)); rebuilds = loads = updates = strings = defaults = fault = lookup_leaves_pending = 0;
    rebinds = 0; rebind_heap = rebind_loads = rebind_updates = rebind_held = -1;
    font_resolves = fresh_rebinds = 0;
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
    g_rr.rebind_material = rebind_material;
    g_rr.world_text_fonts = world_text_fonts;
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
    return sh_resource_resident_begin(&changes, restoring, 0, error, 256);
}
static void check_restored(void)
{
    CHECK(!strings && !*(int *)(heap_object + 0xc4) && !*(int *)(renderer_object + 0x14));
    for (int i = 0; i < 3; i++) CHECK(!(objects[i][0x2c] & RR_PENDING) && *(int *)(objects[i] + 0x28) == 4);
    CHECK(source_mode == 3);
}
static void success_cases(void)
{
    char error[256];
    for (int recovery = 0; recovery < 2; recovery++) {
        /* Constructor-only changes have no physical path row. Select both
         * logical aliases and their recorded consumers without retiring them. */
        reset(); expect_reconstructed = 7;
        sh_package_native_identity rebound[] = {{"model", "body.lwo"}, {"model", "unrelated"}};
        sh_package_changes changes = {0}; changes.identities_complete = 1;
        changes.rebound = rebound; changes.rebound_count = 2;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, recovery, 0, error, sizeof(error));
        CHECK(pass && pass->count == 3);
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 3 && !defaults);
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        }
    }
    {
        /* A secondary native alias can outlive one of its physical paths.
         * The current provider's full identity set owns that lifetime, even
         * when the removed path's primary identity names something else. */
        reset(); retained_alias = 1;
        sh_package_change row = {(char *)paths[0], SH_PACKAGE_RESOURCE_REMOVED, "model", "primary"};
        sh_package_changes changes = {&row, 1}; changes.identities_complete = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && pass->count == 2);
        if (pass) {
            CHECK(!pass->items[0].retired);
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 2 && !defaults);
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        }
    }
    for (int retired = 0; retired < 2; retired++) {
        /* A missing physical member does not retire a still-supplied native
         * identity. Removing the complete identity does, including consumers. */
        reset();
        sh_package_change row = {(char *)paths[0], SH_PACKAGE_RESOURCE_REMOVED, "model", "body.lwo"};
        sh_package_native_identity removed = {"model", "body.lwo"};
        sh_package_changes changes = {&row, 1};
        changes.identities_complete = 1; changes.removed = retired ? &removed : NULL; changes.removed_count = retired;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && pass->count == 2); if (!pass) continue;
        CHECK(pass->items[0].retired == retired);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(loads == (retired ? 1 : 2) && defaults == retired);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    for (int physical = 0; physical < 2; physical++) {
        /* Equal path bytes rebound to a different native object must select
         * both identities, including the old one's dependent cached object,
         * even when there is no physical file change at all. */
        reset(); expect_reconstructed = 7;
        sh_package_change row = {"compiled/shared.bin", SH_PACKAGE_RESOURCE_REPLACED,
            "model", "unrelated", "model", "body.lwo"};
        sh_package_native_identity added = {"model", "unrelated"}, removed = {"model", "body.lwo"};
        sh_package_changes changes = {physical ? &row : NULL, physical};
        changes.added = &added; changes.added_count = 1;
        changes.removed = &removed; changes.removed_count = 1; changes.identities_complete = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && pass->count == 3);
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 1 && loads == 2);
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        }
    }
    for (int removed = 0; removed < 2; removed++) {
        /* A package-only declaration has no catalog row; its native path can
         * differ from the compiled source. Both publication and removal must
         * select its identity and consumers. Only removal permits defaulting. */
        reset();
        sh_package_change row = {"generated/decls/model/body.lwo.decl",
            removed ? SH_PACKAGE_RESOURCE_REMOVED : SH_PACKAGE_RESOURCE_ADDED,
            "model", "body.lwo"};
        sh_package_changes changes = {&row, 1};
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && pass->count == 2); if (!pass) continue;
        CHECK(pass->items[0].retired == removed);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(loads == (removed ? 1 : 2) && defaults == removed);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    for (int family = 0; family < 2; family++) {
        /* A removed blocking volume render model inspector copy keeps its
         * parsed content (Play's precache walks every instance): it is
         * neither rebuilt nor defaulted, unlike an equal removal elsewhere. */
        char *type = family ? "snapPropertyInspector_BlockingVolumeRenderModel" : "model";
        reset(); expect_reconstructed = family ? 0 : 3;
        *(const char **)(manager_object + 8) = type;
        sh_package_change row = {(char *)paths[0], SH_PACKAGE_RESOURCE_REMOVED, type, "body.lwo"};
        sh_package_native_identity removed = {type, "body.lwo"};
        sh_package_changes changes = {&row, 1};
        changes.identities_complete = 1; changes.removed = &removed; changes.removed_count = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass); if (!pass) continue;
        CHECK(sh_resource_resident_selected(pass, objects[0]) == !family);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(counts[0] == !family && defaults == !family);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    for (int alternate = 0; alternate < 2; alternate++) {
        sh_resource_resident *pass;
        reset(); lookup_leaves_pending = alternate;
        pass = begin(alternate ? "shaders/body.fspv" : paths[0], 0, error);
        CHECK(pass && pass->count == 2 && !rebuilds && !loads);
        if (!pass) continue;
        CHECK(sh_resource_resident_selected(pass, objects[0]));
        CHECK(sh_resource_resident_selected(pass, objects[1]));
        CHECK(!sh_resource_resident_selected(pass, objects[2]));
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
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && pass->count == 2); if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && defaults == 0);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 1 && defaults == 1);
        CHECK(rebinds == 1 && rebind_heap == 0);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        CHECK(objects[0][0x2c] & RR_DEFAULT);
    }
    reset();
    {
        /* A reload that comes back as the engine default when the provider does
         * supply the resource is a failed refresh, and a forward activation must
         * not accept it. */
        sh_resource_resident *pass;
        fault = 4;   /* the consumer reload marks the object defaulted */
        pass = begin(paths[0], 0, error);
        CHECK(pass && pass->count == 2); if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(strstr(error, "failed native refresh") && strstr(error, "newly defaulted"));
        CHECK(strstr(error, "skin") != NULL);
        CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)));
        fault = 0; check_restored();
    }
    reset();
    {
        /* The same outcome during recovery is the accepted one. The provider
         * being restored has no source for an identity the failed attempt
         * created, so the engine default is correct for it: the pass reports and
         * counts it and completes, because failing here would leave the session
         * on the attempt's half-published state with no way back. */
        sh_resource_resident *pass;
        fault = 3;   /* the consumer reload throws: a real failed activation */
        pass = begin(paths[0], 0, error);
        CHECK(pass != NULL); if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)));
        check_restored(); CHECK(g_rr_recovery != NULL);
        fault = 4; g_log[0] = 0;
        pass = begin(paths[0], 1, error);
        CHECK(pass && pass->count == 2); if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_recovered_defaults(pass) == 1);
        CHECK(strstr(g_log, "at the engine default") && strstr(g_log, "model:skin"));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)) && !g_rr_recovery);
        fault = 0; check_restored();
    }
    for (int restoring = 0; restoring < 2; restoring++) {
        /* A default that raises an engine error is not a clean retirement
         * (R-321). Every remaining retirement still gets its default, inside
         * the scope, and every load still runs, in a restoration too; then the
         * drain, a repeated drain and the end fail with the first failure, and
         * the pass stays for the (next) restoration. */
        /* The change list is searched, so it stays in sorted path order. */
        sh_package_change rows[3] = {{(char *)paths[1], SH_PACKAGE_RESOURCE_REMOVED},
                                     {(char *)paths[0], SH_PACKAGE_RESOURCE_REMOVED},
                                     {(char *)paths[2], SH_PACKAGE_RESOURCE_REPLACED}};
        sh_package_changes changes = {rows, 3};
        sh_resource_resident *pass;
        reset(); fault = 7; g_log[0] = 0;
        pass = sh_resource_resident_begin(&changes, restoring, 0, error, sizeof(error));
        CHECK(pass && pass->count == 3); if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_defaults(pass, error, sizeof(error)) && defaults == 2 && scoped_defaults == 2);
        CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 2 && loads == 1);
        CHECK(strstr(error, "raised an engine error for model:body.lwo") &&
            strstr(error, "(2 retirement default(s) failed)"));
        CHECK(strstr(g_log, "raised an engine error for model:skin") != NULL);
        CHECK((objects[0][0x2c] & RR_DEFAULT) && (objects[1][0x2c] & RR_DEFAULT));
        /* A restoration reports each identity it leaves at the default; the failure still fails it. */
        CHECK(sh_resource_resident_recovered_defaults(pass) == (restoring ? 2u : 0u));
        error[0] = 0;
        CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && strstr(error, "raised an engine error"));
        error[0] = 0;
        CHECK(!sh_resource_resident_end(pass, 1, error, sizeof(error)) && strstr(error, "raised an engine error"));
        CHECK(g_rr_recovery == pass); check_restored();
        /* The restoration rebuilds what the provider it restores supplies. */
        fault = 0; defaults = loads = 0;
        pass = begin(paths[0], 1, error);
        CHECK(pass && pass->count == 3); if (!pass) return;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && !defaults && loads == 3);
        CHECK(!(objects[0][0x2c] & RR_DEFAULT) && !(objects[1][0x2c] & RR_DEFAULT));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)) && !g_rr_recovery); check_restored();
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
static void world_text_cases(void)
{
    char error[256];
    for (int external = 0; external < 2; external++) for (int fail = 0; fail < 3; fail++) {
        sh_resource_resident *pass;
        reset(); expect_reconstructed = 1;
        *(const char **)(manager_object + 8) = "snapeditorsettings";
        pass = begin(paths[0], 0, error);
        CHECK(pass && pass->count == 1); if (!pass) continue;
        if (external) sh_resource_resident_external(pass, objects[0]);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        fault = fail ? fail + 7 : 0;
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) == !fail);
        CHECK(font_resolves == 1);
        if (!fail) {
            CHECK(*(void **)(objects[0] + 0x688) == objects[0]);
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && font_resolves == 1);
        }
        CHECK(sh_resource_resident_end(pass, !fail, error, sizeof(error)) == !fail);
        check_restored();
        if (fail) {
            CHECK(g_rr_recovery != NULL); fault = 0;
            sh_resource_graph_test_reset();
            pass = begin(paths[0], 1, error);
            CHECK(pass && sh_resource_resident_selected(pass, objects[0]));
            if (!pass) continue;
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
            CHECK(font_resolves == 2 && *(void **)(objects[0] + 0x698) == objects[2]);
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)) && !g_rr_recovery);
            check_restored();
        }
    }
}
static void initial_publication_cases(void)
{
    char error[256];
    for (int initial = 0; initial < 2; initial++) {
        sh_package_change row = {"shaders/body.fspv", SH_PACKAGE_RESOURCE_REPLACED};
        sh_package_changes changes = {&row, 1};
        sh_resource_resident *pass;
        reset();
        *(const char **)(manager_object + 0x10) = "TestInheritedDecl";
        *(void **)(objects[1] + 0x58) = objects[0];
        /* The source alias differs from the native cooked path. Its object
         * and inherited child both predate startup lifetime promotion. */
        *(unsigned *)(objects[0] + 0x28) = 1;
        *(unsigned *)(objects[1] + 0x28) = 1;
        pass = sh_resource_resident_begin(&changes, 0, initial, error, sizeof(error));
        CHECK(pass && sh_resource_resident_selected(pass, objects[0]));
        CHECK(sh_resource_resident_selected(pass, objects[1]) == initial);
        CHECK(!sh_resource_resident_selected(pass, objects[2]));
        if (!pass) continue;
        expect_reconstructed = initial ? 3 : 1;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(counts[0] == 1 && counts[1] == initial && !counts[2]);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        CHECK(*(unsigned *)(objects[0] + 0x28) == 1 && *(unsigned *)(objects[1] + 0x28) == 1);
        *(unsigned *)(objects[0] + 0x28) = *(unsigned *)(objects[1] + 0x28) = 4;
        check_restored();
    }
    reset();
    {
        sh_package_change row = {"shaders/body.fspv", SH_PACKAGE_RESOURCE_REPLACED};
        sh_package_changes changes = {&row, 1};
        sh_resource_resident *pass;
        /* Ordinary startup consumers keep initialized caches; they do not
         * acquire an inheritance relationship from sharing a dependency. */
        *(unsigned *)(objects[1] + 0x28) = 1;
        *(void **)(objects[1] + 0x58) = objects[0];
        pass = sh_resource_resident_begin(&changes, 0, 1, error, sizeof(error));
        CHECK(pass && pass->count == 1);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        *(unsigned *)(objects[1] + 0x28) = 4;
        check_restored();
    }
    reset();
    {
        sh_resource_resident *pass;
        /* Native parent chains retain coverage if the original parse happened
         * before graph instrumentation, including a grandchild. */
        sh_resource_graph_test_reset();
        *(const char **)(manager_object + 0x10) = "TestInheritedDecl";
        *(void **)(objects[1] + 0x58) = objects[0];
        *(void **)(objects[2] + 0x58) = objects[1];
        pass = begin(paths[0], 0, error);
        CHECK(pass && pass->count == 3);
        expect_reconstructed = 7;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(counts[0] == 1 && counts[1] == 1 && counts[2] == 1);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        check_restored();
        /* Malformed or dangling parent chains fail before native mutation. */
        *(void **)(objects[1] + 0x58) = objects[2];
        CHECK(!begin(paths[0], 0, error)); check_restored();
        *(void **)(objects[1] + 0x58) = (void *)1;
        CHECK(!begin(paths[0], 0, error)); check_restored();
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
        CHECK(!sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error))); check_restored();
    }
    {
        sh_package_changes changes = {0};
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && !pass->count && pass->render.active == 2); /* Policy rearm still needs renderer ownership. */
        CHECK(!sh_resource_resident_selected(pass, objects[0]));
        CHECK(!sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error))); /* Nested capture cannot replace its owner. */
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
}
static void atlas_failure_cases(void)
{
    char error[256];
    {
        /* A private page provider that cannot be built fails the pass before
         * any reload; the render and heap scopes are released and collected. */
        sh_resource_resident *pass;
        reset(); vmr_provider_failed = 1;
        pass = begin(paths[0], 0, error);
        CHECK(!pass && vmr_providers == 1 && vmr_collected == 1 && strstr(error, "virtual texture page providers"));
        CHECK(!loads && !g_rr_scope_recovery); check_restored();
        vmr_provider_failed = 0;
        pass = begin(paths[0], 0, error); CHECK(pass && vmr_providers == 2 && vmr_provider_loads == 0 && !rebinds);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads && rebinds);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    for (int stage = 0; stage < 3; stage++) {
        sh_resource_resident *pass;
        reset(); pass = begin(paths[0], 0, error); CHECK(pass && vmr_providers == 1);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        if (stage) CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        vmr_failed = 1;
        if (stage < 2) CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(!sh_resource_resident_end(pass, 1, error, sizeof(error)));
        CHECK(vmr_collected == 1 && g_rr_recovery);
        check_restored(); vmr_failed = 0;
        pass = begin(paths[0], 1, error); CHECK(pass);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)) && !g_rr_recovery);
        CHECK(vmr_collected == 2); check_restored();
    }
}
static void scope_failure_cases(void)
{
    char error[256];
    for (int stage = 1; stage <= 4; stage++) {
        sh_package_changes changes = {0};
        sh_resource_resident *pass;
        reset(); render_fault = stage < 3 ? stage : 0;
        pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        if (stage < 3) {
            CHECK(!pass && !g_rr_scope_recovery && !g_rr_recovery);
            check_restored(); continue;
        }
        CHECK(pass); render_fault = stage;
        CHECK(!sh_resource_resident_end(pass, 1, error, sizeof(error)));
        if (stage == 3) {
            CHECK(g_rr_scope_recovery == pass && *(int *)(renderer_object + 0x14) == 1);
            CHECK(!sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error)));
            CHECK(g_rr_scope_recovery == pass && !*(int *)(heap_object + 0xc4));
        } else CHECK(!g_rr_scope_recovery && !g_rr_recovery);
        render_fault = 0;
        pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && !g_rr_scope_recovery);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        check_restored();
    }
    for (int stage = 1; stage <= 2; stage++) {
        sh_package_changes changes = {0};
        sh_resource_resident *pass;
        reset(); pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass); heap_pop_fault = stage;
        CHECK(!sh_resource_resident_end(pass, 1, error, sizeof(error)));
        CHECK((g_rr_scope_recovery != NULL) == (stage == 1));
        CHECK(!*(int *)(renderer_object + 0x14));
        heap_pop_fault = 0;
        pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && !g_rr_scope_recovery);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        check_restored();
    }
    {
        sh_resource_resident *pass;
        reset(); pass = begin(paths[0], 0, error); CHECK(pass);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        render_fault = 3;
        CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)));
        CHECK(g_rr_scope_recovery == pass && !g_rr_recovery);
        render_fault = 0;
        /* Even when the native scopes could not initially release, retain the
         * affected identities for the restored provider's reconstruction. */
        sh_resource_graph_test_reset();
        pass = begin(paths[0], 1, error); CHECK(pass && pass->count == 2);
        CHECK(!g_rr_scope_recovery && g_rr_recovery);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        check_restored();
    }
    {
        sh_package_changes changes = {0};
        sh_render_scope outer = {0};
        sh_resource_resident *pass;
        reset(); CHECK(sh_render_scope_enter(&renderer, &main_thread, &outer));
        pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && *(int *)(renderer_object + 0x14) == 2);
        expected_render_holds = 2;
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        CHECK(*(int *)(renderer_object + 0x14) == 1);
        CHECK(sh_render_scope_leave(&outer)); check_restored();
    }
}
static void scope_ownership_cases(void)
{
    char error[256]; sh_package_changes changes = {0};
    reset();
    CHECK(!sh_resource_resident_owned(NULL));
    sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(pass && sh_resource_resident_owned(pass));
    CHECK(sh_resource_resident_selected_count(pass) == 0);
    (*(int *)(heap_object + 0xc4))++;
    CHECK(!sh_resource_resident_owned(pass));
    (*(int *)(heap_object + 0xc4))--;
    CHECK(sh_resource_resident_owned(pass));
    (*(int *)(renderer_object + 0x14))++;
    CHECK(!sh_resource_resident_owned(pass));
    (*(int *)(renderer_object + 0x14))--;
    DWORD owner = main_thread; main_thread++;
    CHECK(!sh_resource_resident_owned(pass)); main_thread = owner;
    CHECK(sh_resource_resident_owned(pass));
    CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();
    pass = begin(paths[0], 0, error);
    CHECK(pass && sh_resource_resident_selected_count(pass) > 0);
    CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();
}
static void additive_finalization_cases(void)
{
    char error[256];
    void *existing[] = {objects[0], objects[1]};
    sh_package_changes empty = {0};
    for (int failure = 0; failure < 2; failure++) {
        reset();
        *(const char **)(manager_object + 8) = "material";
        *(int *)(manager_object + 0x28) = 2;
        sh_resource_resident *pass = sh_resource_resident_begin(&empty, 0, 0, error, sizeof(error));
        CHECK(pass && !sh_resource_resident_selected_count(pass));
        *(int *)(manager_object + 0x28) = 3;
        *(int *)(objects[2] + 0x28) = 1;
        objects[0][0x100] = 19; objects[1][0x100] = 37;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        fault = failure ? 10 : 0;
        CHECK(sh_resource_resident_drain_additions(pass, existing, 2, error, sizeof(error)) == !failure);
        CHECK(fresh_rebinds == 1 && !rebinds && !loads && !rebuilds && updates == !failure);
        CHECK(objects[0][0x100] == 19 && objects[1][0x100] == 37);
        CHECK(*(int *)(objects[2] + 0x28) == 4);
        CHECK(sh_resource_resident_end(pass, !failure, error, sizeof(error)) == !failure);
        check_restored();
    }
    reset();
    *(const char **)(manager_object + 8) = "snapeditorsettings";
    *(int *)(manager_object + 0x28) = 2;
    sh_resource_resident *pass = sh_resource_resident_begin(&empty, 0, 0, error, sizeof(error));
    CHECK(pass && !sh_resource_resident_selected_count(pass));
    *(int *)(manager_object + 0x28) = 3;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    CHECK(sh_resource_resident_drain_additions(pass, existing, 2, error, sizeof(error)));
    CHECK(font_resolves == 1 && !rebinds && !fresh_rebinds);
    CHECK(!*(void **)(objects[0] + 0x688) && !*(void **)(objects[1] + 0x688));
    CHECK(*(void **)(objects[2] + 0x688) == objects[0]);
    CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    reset();
    pass = begin(paths[0], 0, error);
    CHECK(pass && sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    CHECK(!sh_resource_resident_drain_additions(pass, existing, 2, error, sizeof(error)));
    CHECK(strstr(error, "pre-existing object") && !rebinds && !fresh_rebinds && !updates);
    CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)));
    check_restored();
}
/* A renderer parameter or program flag naming a changed dependency holds it
 * as a live object and owns a native slot. Only one the provider supplies is
 * rebuilt. */
static void fixed_renderer_cases(void)
{
    char error[256];
    static void *parm_entries[1];
    for (int run = 0; run < 4; run++) {
        int supplied = run & 1;
        const char *family = run & 2 ? "renderProgFlag" : "renderParm";
        sh_package_change rows[] = {{(char *)paths[0], SH_PACKAGE_RESOURCE_REPLACED},
            {(char *)paths[2], SH_PACKAGE_RESOURCE_REPLACED}};
        sh_package_changes changes = {rows, supplied ? 2u : 1u};
        sh_resource_graph_frame frame;
        sh_resource_resident *pass;
        reset(); expect_reconstructed = supplied ? 7 : 3;
        memset(parm_manager, 0, sizeof(parm_manager));
        *(const char **)(parm_manager + 8) = family;
        parm_entries[0] = objects[2];
        *(void ***)(parm_manager + 0x20) = parm_entries; *(int *)(parm_manager + 0x28) = 1;
        *(void **)(manager_object + 0x18) = parm_manager; *(int *)(manager_object + 0x28) = 2;
        *(void **)(objects[2] + 0x10) = parm_manager; *(const char **)(objects[2] + 8) = "transmap";
        sh_resource_graph_begin(&frame, family, "transmap");
        sh_resource_graph_reference("model", "body.lwo");
        sh_resource_graph_end(&frame, 1);
        pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && pass->count == (supplied ? 3u : 2u)); if (!pass) continue;
        CHECK(sh_resource_resident_selected(pass, objects[2]) == supplied);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && counts[2] == supplied);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
}
/* A rebuilt leaf reloads before declarations are re-parsed; other rebuilt
 * resources wait for the drain, which still verifies every object. */
static int leaf_family(const char *family) { return !strcmp(family, "skeleton"); }
static void leaf_cases(void)
{
    char error[256];
    static void *leaf_entries[1];
    sh_package_change rows[] = {{(char *)paths[0], SH_PACKAGE_RESOURCE_REPLACED},
        {(char *)paths[2], SH_PACKAGE_RESOURCE_REPLACED}};
    sh_package_changes changes = {rows, 2};
    sh_resource_resident *pass;
    reset(); expect_reconstructed = 7;
    memset(parm_manager, 0, sizeof(parm_manager));
    *(const char **)(parm_manager + 8) = "skeleton";
    leaf_entries[0] = objects[2];
    *(void ***)(parm_manager + 0x20) = leaf_entries; *(int *)(parm_manager + 0x28) = 1;
    *(void **)(manager_object + 0x18) = parm_manager; *(int *)(manager_object + 0x28) = 2;
    *(void **)(objects[2] + 0x10) = parm_manager; *(const char **)(objects[2] + 8) = "bones";
    pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(pass && pass->count == 3); if (!pass) return;
    CHECK(!sh_resource_resident_drain_leaves(pass, leaf_family, error, sizeof(error)));
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    lookup_leaves_pending = 1; /* The native lookup can leave the mark set. */
    CHECK(sh_resource_resident_drain_leaves(pass, leaf_family, error, sizeof(error)));
    CHECK(loads == 1 && !(objects[2][0x2c] & RR_PENDING) && (objects[0][0x2c] & RR_PENDING));
    CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 3);
    CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
}
/* M115: a prefab carrying newer's context of a shared parent is pasted into a
 * map carrying older's. The P session registered newer's variant entitydef and
 * its consumer; loading A retired both, leaving defaulted shells; older's
 * variant is live in A. The admission adds the variant again (by path and by
 * identity). Before the revival rule its resident pass selected both shells and
 * refused: "additive resource admission selected an existing native consumer". */
static const char *const revival_names[3] = {
    "smp/df62a6dd59ccd8249d7685deaa9f97f2e5d28ddab68b1763187395bc62d73a93",  /* newer variant, retired */
    "smp/newer-variant-consumer",                                            /* its consumer, retired */
    "smp/4861308f1b99eff36b9a5cd98436cd5445d6d4387c7e5209f660a41f29050291",  /* older variant, live in A */
};
static void revival_reset(unsigned char state0, unsigned char state1)
{
    sh_resource_graph_frame frame;
    reset(); g_log[0] = 0;
    *(const char **)(manager_object + 8) = "entitydef";
    for (int i = 0; i < 3; i++) *(const char **)(objects[i] + 8) = revival_names[i];
    objects[0][0x2c] = state0; objects[1][0x2c] = state1;
    sh_resource_graph_test_reset();
    sh_resource_graph_begin(&frame, "entitydef", revival_names[0]); sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "entitydef", revival_names[1]);
    sh_resource_graph_reference("entitydef", revival_names[0]); sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "entitydef", revival_names[2]); sh_resource_graph_end(&frame, 1);
}
/* Loading A: the provider difference removes newer's variant and its consumer.
 * This provider retires both; older's variant is untouched. */
static void revival_retire(void)
{
    char error[256];
    static sh_package_native_identity removed[2];
    sh_package_changes changes;
    sh_resource_resident *pass;
    removed[0] = (sh_package_native_identity){"entitydef", (char *)revival_names[0]};
    removed[1] = (sh_package_native_identity){"entitydef", (char *)revival_names[1]};
    memset(&changes, 0, sizeof(changes));
    changes.removed = removed; changes.removed_count = 2; changes.identities_complete = 1;
    expect_reconstructed = 3;
    pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(pass && pass->count == 2);
    if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 2);
    CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    CHECK((objects[0][0x2c] & RR_DEFAULT) && (objects[1][0x2c] & RR_DEFAULT) && !(objects[2][0x2c] & RR_DEFAULT));
    CHECK(g_rr_retirement_count == 2 && !*(void **)(objects[0] + 0x18) && !*(void **)(objects[1] + 0x18));
    memset(counts, 0, sizeof(counts)); rebuilds = loads = defaults = 0;
}
/* Retirement replaced the consumer's recorded references with its default's,
 * which name nothing. A consumer whose current parse still references newer's
 * variant records the edge again. */
static void revival_edge(void)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_begin(&frame, "entitydef", revival_names[1]);
    sh_resource_graph_reference("entitydef", revival_names[0]); sh_resource_graph_end(&frame, 1);
}
static sh_resource_resident *revival_begin(int both, int restoring, char error[256])
{
    static sh_package_native_identity added[2];
    static sh_package_change item;
    static sh_package_changes changes;
    added[0] = (sh_package_native_identity){"entitydef", (char *)revival_names[0]};
    added[1] = (sh_package_native_identity){"entitydef", (char *)revival_names[1]};
    item = (sh_package_change){(char *)paths[0], SH_PACKAGE_RESOURCE_ADDED, "entitydef", (char *)revival_names[0]};
    memset(&changes, 0, sizeof(changes));
    changes.items = &item; changes.count = 1; changes.identities_complete = 1;
    changes.added = added; changes.added_count = both ? 2 : 1;
    return sh_resource_resident_begin(&changes, restoring, 0, error, 256);
}
static void revival_cases(void)
{
    char error[256], first[300];
    void *existing[3];
    sh_resource_resident *pass;
    /* The live failure, then the addition that owns both shells. */
    for (int bad_parse = 0; bad_parse < 2; bad_parse++) {
        revival_reset(0, 0); revival_retire(); expect_reconstructed = 3;
        pass = revival_begin(1, 0, error);
        CHECK(pass && sh_resource_resident_selected_count(pass) == 2);
        CHECK(sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 0 && !first[0]);
        for (int i = 0; i < 3; i++) existing[i] = objects[i];
        CHECK(sh_resource_resident_release_revived(pass, existing, 3) == 1 && existing[0] == objects[2]);
        /* The admission's source guard asks about one object as one identity. */
        CHECK(sh_resource_resident_revives(pass, objects[0], "EntityDef", revival_names[0]) &&
            sh_resource_resident_revives(pass, objects[1], "entitydef", revival_names[1]));
        CHECK(!sh_resource_resident_revives(pass, objects[2], "entitydef", revival_names[2]) &&
            !sh_resource_resident_revives(pass, objects[1], "entitydef", revival_names[0]) &&
            !sh_resource_resident_revives(pass, objects[0], "snapeditorentitydef", revival_names[0]) &&
            !sh_resource_resident_revives(pass, objects[0], "entitydef", revival_names[1]) &&
            !sh_resource_resident_revives(NULL, objects[0], "entitydef", revival_names[0]));
        CHECK(strstr(g_log, revival_names[0]) && strstr(g_log, "revives retired"));
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && counts[0] == 1 && counts[1] == 1 && !counts[2]);
        CHECK(g_rr_retirement_count == 0);  /* supplied again: no longer retired shells */
        fault = bad_parse ? 4 : 0;  /* the consumer's load leaves it at the engine default */
        CHECK(sh_resource_resident_drain_additions(pass, existing, 1, error, sizeof(error)) == !bad_parse);
        CHECK(!bad_parse || strstr(error, "did not parse its addition"));
        CHECK(loads == 2 && !(objects[0][0x2c] & RR_DEFAULT));
        CHECK(sh_resource_resident_end(pass, !bad_parse, error, sizeof(error)) == !bad_parse);
        rr_free(g_rr_recovery); g_rr_recovery = NULL;
        check_restored();
    }
    /* A revived shell is still a pre-existing object to an unreleased list. */
    revival_reset(0, 0); revival_retire(); expect_reconstructed = 3;
    pass = revival_begin(1, 0, error);
    for (int i = 0; i < 3; i++) existing[i] = objects[i];
    CHECK(pass && sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    CHECK(pass && !sh_resource_resident_drain_additions(pass, existing, 3, error, sizeof(error)));
    if (pass) { sh_resource_resident_end(pass, 0, error, sizeof(error)); rr_free(g_rr_recovery); g_rr_recovery = NULL; }
    /* Provenance, not the default state, decides. A retired identity whose
     * object retains a native record (a parse that failed) is source-bearing. */
    revival_reset(0, 0); revival_retire();
    *(void **)(objects[0] + 0x18) = objects[2];
    pass = revival_begin(1, 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 1 &&
        strstr(first, revival_names[0]) && strstr(first, "native record"));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    *(void **)(objects[0] + 0x18) = NULL;
    /* An engine default this provider never retired: a missing declaration an
     * existing consumer looked up. */
    revival_reset(RR_DEFAULT, RR_DEFAULT);
    pass = revival_begin(1, 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 2 &&
        strstr(first, revival_names[0]) && strstr(first, "never retired"));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    /* Retired, then supplied again by a later provider, then left at the
     * default by a failed load: the supply took the variant out of the ledger.
     * Its consumer, a shell no provider supplies, is retired again instead of
     * loaded: its default is rebuilt and it stays revivable (R-321). */
    revival_reset(0, 0); revival_retire(); revival_edge(); expect_reconstructed = 3;
    pass = begin(paths[0], 0, error);
    CHECK(pass && pass->count == 2);
    if (pass) {
        CHECK(sh_resource_resident_describe(pass, objects[1], first, sizeof(first)) &&
            strstr(first, "retired by this provider"));
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && g_rr_retirement_count == 1);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 1 && defaults == 1);
        CHECK(!(objects[0][0x2c] & RR_DEFAULT) && (objects[1][0x2c] & RR_DEFAULT));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    objects[0][0x2c] = RR_DEFAULT;
    pass = revival_begin(1, 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 1 &&
        strstr(first, revival_names[0]) && strstr(first, "never retired"));
    CHECK(pass && sh_resource_resident_revives(pass, objects[1], "entitydef", revival_names[1]));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    /* Still refused with the ledger satisfied: a live consumer the addition does
     * not name, a live object of the same identity, a pending shell, and
     * recovery. A retired consumer it does not name is refreshed instead: the
     * addition owns it and rebuilds its default once the addition's
     * declarations exist, as the provider path retires it again. */
    revival_reset(0, 0); revival_retire(); revival_edge(); objects[1][0x2c] = 0;
    pass = revival_begin(0, 0, error);
    CHECK(pass && sh_resource_resident_selected_count(pass) == 2);
    CHECK(sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 1 && strstr(first, revival_names[1]));
    /* A live object is no retired shell: it is rebuilt as a consumer, not retired again. */
    CHECK(pass && sh_resource_resident_describe(pass, objects[1], first, sizeof(first)) &&
        !strstr(first, "retired by this provider"));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    revival_reset(0, 0); revival_retire(); revival_edge();
    pass = revival_begin(0, 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 0 && !first[0]);
    CHECK(pass && sh_resource_resident_describe(pass, objects[1], first, sizeof(first)) &&
        strstr(first, "retired by this provider"));
    if (pass) {
        void *owned[3] = {objects[0], objects[1], objects[2]};
        qsort(owned, 3, sizeof(*owned), rr_pointer_compare); g_log[0] = 0;
        CHECK(sh_resource_resident_release_revived(pass, owned, 3) == 1 && owned[0] == objects[2] &&
            strstr(g_log, "revives retired") && strstr(g_log, "refreshes retired"));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    }
    revival_reset(0, 0); revival_retire(); objects[0][0x2c] = 0;
    pass = revival_begin(1, 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 1 &&
        strstr(first, revival_names[0]) && strstr(first, "live"));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    revival_reset(0, 0); revival_retire(); objects[0][0x2c] |= RR_PENDING;
    pass = revival_begin(1, 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, NULL, 0) == 1);
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    objects[0][0x2c] &= (unsigned char)~RR_PENDING;
    revival_reset(0, 0); revival_retire();
    pass = revival_begin(1, 1, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 2 && strstr(first, "recovery"));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    CHECK(sh_resource_resident_blocking_count(NULL, NULL, 0) == SIZE_MAX);
    check_restored();
    revival_reset(0, 0);
}
/* R-309 in-place image recovery. objects[2] is the image env/default in an "image" list;
 * objects[0] (body.lwo) and objects[1] (skin) stay models. skin's recorded parse names the image,
 * so it is the image's consumer: a permanent one is rebuilt, a live non-permanent one refuses, and
 * an image no recorded parse names refuses because its consumers cannot be established. A fixed
 * renderer consumer holds the pointer and is neither returned nor rebuilt (the barrier). */
static int image_leaf(const char *family) { return !strcmp(family, "image"); }
static int availability_history = 1; /* The size-read history began (the native size getters are hooked). */
static void availability_setup(int consumer, int fixed)
{
    static void *image_entries[1];
    sh_resource_graph_frame frame;
    reset();
    if (availability_history) sh_resource_graph_size_history_begin();
    memset(parm_manager, 0, sizeof(parm_manager));
    *(const char **)(parm_manager + 8) = "image";
    image_entries[0] = objects[2];
    *(void ***)(parm_manager + 0x20) = image_entries; *(int *)(parm_manager + 0x28) = 1;
    *(void **)(manager_object + 0x18) = parm_manager; *(int *)(manager_object + 0x28) = 2;
    *(void **)(objects[2] + 0x10) = parm_manager; *(const char **)(objects[2] + 8) = "env/default";
    if (consumer) {
        sh_resource_graph_begin(&frame, "model", "skin"); sh_resource_graph_reference("image", "env/default");
        sh_resource_graph_end(&frame, 1);
    }
    if (fixed) {
        sh_resource_graph_begin(&frame, "renderParm", "userenvmap");
        sh_resource_graph_reference("image", "env/default"); sh_resource_graph_end(&frame, 1);
    }
}
static void availability_cases(void)
{
    char error[256], why[256];
    for (int recover = 0; recover < 2; recover++) {
        /* A permanent recorded consumer is rebuilt with the image; the image reloads as a leaf. */
        availability_setup(1, 1); expect_reconstructed = 6;
        sh_package_availability_change row = {"image", "env/default", recover};
        sh_package_changes changes = {0}; changes.availability = &row; changes.availability_count = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        if (!pass) fprintf(stderr, "availability %d: %s\n", recover, error);
        CHECK(pass && pass->count == 2); if (!pass) continue;
        CHECK(sh_resource_resident_selected(pass, objects[2]) && sh_resource_resident_selected(pass, objects[1]));
        CHECK(!sh_resource_resident_selected(pass, objects[0]));
        CHECK(sh_resource_resident_describe(pass, objects[2], why, sizeof(why)) && strstr(why, recover ?
            "a declared image requirement recovers it in place" : "its in-place recovery ends"));
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
        CHECK(!(objects[2][0x2c] & RR_PENDING) && (objects[1][0x2c] & RR_PENDING));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && counts[2] == 1 && counts[1] == 1);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    {
        /* Only a fixed renderer object names the image: nothing else is rebuilt. */
        availability_setup(0, 1); expect_reconstructed = 4;
        sh_package_availability_change row = {"image", "env/default", 1};
        sh_package_changes changes = {0}; changes.availability = &row; changes.availability_count = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && pass->count == 1);
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && counts[2] == 1 && !counts[1]);
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        }
    }
    {
        /* No recorded parse names the image: its consumers cannot be established. */
        availability_setup(0, 0);
        sh_package_availability_change row = {"image", "env/default", 1};
        sh_package_changes changes = {0}; changes.availability = &row; changes.availability_count = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(!pass && strstr(error, "cannot establish its consumers: image:env/default (no recorded node)"));
        CHECK(!rebuilds && !loads); check_restored();
    }
    for (int holder = 0; holder < 2; holder++) {
        /* Known image, incompletely observed consumer: one complete recorded edge shows only that a
         * consumer exists. A consumer whose recorded parse did not complete, or a fixed renderer holder
         * whose parse did not, leaves the closure unestablished. */
        sh_resource_graph_frame frame;
        availability_setup(1, holder);
        sh_resource_graph_begin(&frame, holder ? "renderParm" : "model", holder ? "userenvmap" : "body.lwo");
        sh_resource_graph_reference("image", "env/default");
        sh_resource_graph_end(&frame, 0);
        sh_package_availability_change row = {"image", "env/default", 1};
        sh_package_changes changes = {0}; changes.availability = &row; changes.availability_count = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        if (pass || !strstr(error, holder ? "renderparm:userenvmap is not completely observed" :
                "model:body.lwo is not completely observed")) fprintf(stderr, "incomplete consumer %d: %s\n", holder, error);
        CHECK(!pass && strstr(error, holder ? "renderparm:userenvmap is not completely observed" :
            "model:body.lwo is not completely observed"));
        if (pass) sh_resource_resident_end(pass, 1, error, sizeof(error));
        CHECK(!rebuilds && !loads);
        check_restored();
    }
    for (int read = 0; read < 2; read++) {
        /* Root's missing-edge control: a fully observed skin -> image edge, plus a live, incompletely
         * observed body.lwo whose read of the image was never recorded. The recorded walk cannot see
         * body.lwo. The rebuild keeps the image object, so an unrecorded holder of its pointer stays
         * valid. A copy of its size found so far comes only through the hooked size getters, whose read
         * is recorded against the image and is never replaced by a consumer's reparse: with that read the
         * transition refuses; without it the walk's result stands. */
        sh_resource_graph_frame frame;
        availability_setup(1, 0); expect_reconstructed = 6;
        sh_resource_graph_begin(&frame, "model", "body.lwo"); sh_resource_graph_end(&frame, 0);
        if (read) sh_resource_graph_history_mark("image", "env/default", SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
        sh_package_availability_change row = {"image", "env/default", 1};
        sh_package_changes changes = {0}; changes.availability = &row; changes.availability_count = 1;
        g_log[0] = 0;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        if (read) {
            /* The recorded size read isolates the image: the pass begins, the image keeps its object and
             * sizes, its recorded consumer is still rebuilt, and nothing reloads the image. */
            if (!pass) fprintf(stderr, "missing edge, read: %s\n", error);
            CHECK(pass && sh_resource_resident_isolated(pass, objects[2]) && sh_resource_resident_isolated_count(pass) == 1);
            CHECK(strstr(g_log, "native resident isolates image:env/default: an in-place rebuild cannot show it free of copied "
                "sizes (a material size getter read its size") != NULL);
            if (!pass) continue;
            expect_reconstructed = 2;
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
            CHECK(!counts[2]); check_restored();
            continue;
        }
        if (!pass) fprintf(stderr, "missing edge, no read: %s\n", error);
        CHECK(pass && sh_resource_resident_selected(pass, objects[2]) && !sh_resource_resident_selected(pass, objects[0]));
        if (!pass) continue;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    for (int kind = 0; kind < 3; kind++) {
        /* The size-read history refuses a transition it cannot clear, in both directions: no history in
         * this process, an image registered before the history began, or a recorded size read. */
        for (int recover = 0; recover < 2; recover++) {
            availability_history = kind != 0;
            availability_setup(1, 1);
            availability_history = 1;
            if (kind == 1) sh_resource_graph_history_mark("image", "env/default", SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE);
            if (kind == 2) sh_resource_graph_history_mark("image", "env/default", SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
            const char *want = kind == 0 ? "no size-read history is recorded in this process" :
                kind == 1 ? "it was registered before the size-read history began" :
                "a material size getter read its size";
            sh_package_availability_change row = {"image", "env/default", recover};
            sh_package_changes changes = {0}; changes.availability = &row; changes.availability_count = 1;
            g_log[0] = 0;
            sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
            if (kind == 0) {
                /* No history at all: nothing can be cleared, the pass still refuses. */
                if (pass || !strstr(error, want)) fprintf(stderr, "history %d/%d: %s\n", kind, recover, pass ? "began" : error);
                CHECK(!pass && strstr(error, "cannot show image:env/default free of copied sizes") && strstr(error, want));
                if (pass) sh_resource_resident_end(pass, 1, error, sizeof(error));
                CHECK(!rebuilds && !loads); check_restored();
                continue;
            }
            /* A pre-history image or a recorded read isolates that image; the pass proceeds without it
             * (the 2026-10-22 OpenGL boot refused its whole library on textures/ca/fx/ca_mp_control_panel_grid.tga). */
            if (!pass) fprintf(stderr, "history %d/%d: %s\n", kind, recover, error);
            CHECK(pass && sh_resource_resident_isolated(pass, objects[2]) &&
                sh_resource_resident_isolated_identity(pass, "image", "env/default") &&
                !sh_resource_resident_isolated_identity(pass, "image", "env/other"));
            CHECK(strstr(g_log, "native resident isolates image:env/default") && strstr(g_log, want));
            if (!pass) continue;
            expect_reconstructed = 2;
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
            CHECK(!counts[2]); check_restored();
        }
    }
    {
        /* A live consumer below permanent lifetime would keep what it copied: refuse. */
        availability_setup(1, 0); *(int *)(objects[1] + 0x28) = 1;
        sh_package_availability_change row = {"image", "env/default", 1};
        sh_package_changes changes = {0}; changes.availability = &row; changes.availability_count = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(!pass && strstr(error, "live consumer this pass cannot rebuild: model:skin (level 1)"));
        CHECK(!rebuilds && !loads);
        *(int *)(objects[1] + 0x28) = 4; check_restored();
    }
    {
        /* A shell this provider retired among the image's recorded consumers is
         * retired again with it, not refused and not loaded: its default is
         * rebuilt and it stays revivable (R-321). */
        rr_entry shell = {0};
        availability_setup(1, 0); expect_reconstructed = 6;
        shell.resource = objects[1]; shell.type = "model"; shell.name = "skin"; shell.retired = 1;
        rr_retirement_note(&shell); objects[1][0x2c] = RR_DEFAULT;
        sh_package_availability_change row = {"image", "env/default", 1};
        sh_package_changes changes = {0}; changes.availability = &row; changes.availability_count = 1;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        if (!pass) fprintf(stderr, "retired image consumer: %s\n", error);
        CHECK(pass && pass->count == 2 && sh_resource_resident_selected(pass, objects[1]));
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && counts[1] == 1 && counts[2] == 1);
            CHECK(defaults == 1 && loads == 1 && (objects[1][0x2c] & RR_DEFAULT) && rr_retired_by_provider(&shell));
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
        }
    }
}
/* R-321: the default construction scope around the pass's own defaults and loads, and a retirement whose
 * default this process cannot build. */
static int any_leaf(const char *family) { (void)family; return 1; }
static void default_scope_cases(void)
{
    char error[256];
    /* The change list is searched, so it stays in sorted path order. */
    sh_package_change rows[3] = {{(char *)paths[1], SH_PACKAGE_RESOURCE_REMOVED},
                                 {(char *)paths[0], SH_PACKAGE_RESOURCE_REMOVED},
                                 {(char *)paths[2], SH_PACKAGE_RESOURCE_REPLACED}};
    sh_package_changes changes = {rows, 3};
    sh_resource_resident *pass;
    /* Both retirement defaults and the reload are built inside the scope, and it closes. */
    reset();
    pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(pass && pass->count == 3); if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    CHECK(sh_resource_resident_defaults(pass, error, sizeof(error)) && defaults == 2 && scoped_defaults == 2);
    CHECK(!scope_depth);
    CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 2 && loads == 1);
    CHECK(!unscoped_loads && !scope_depth);
    CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    /* Leaf reloads too. */
    reset();
    pass = begin(paths[0], 0, error);
    CHECK(pass != NULL); if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    CHECK(sh_resource_resident_drain_leaves(pass, any_leaf, error, sizeof(error)) && loads == 2);
    CHECK(!unscoped_loads && !scope_depth);
    CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && !scope_depth);
    CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    /* A family whose default this process cannot build: its retirement is refused before anything
     * changes, and a pass that retires nothing of it proceeds. */
    reset(); unready_family = "MODEL";
    pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(!pass && strstr(error, "retirement cannot build the engine default for model:body.lwo in this process"));
    CHECK(!rebuilds && !loads && !defaults && !g_rr_recovery); check_restored();
    pass = begin(paths[0], 0, error);
    CHECK(pass && sh_resource_resident_reconstruct(pass, error, sizeof(error)) &&
        sh_resource_resident_drain(pass, error, sizeof(error)));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();
    /* Unavailable only after the pass began: no default is attempted for it, the reload still runs,
     * and the pass fails and stays for the restoration. */
    reset(); g_log[0] = 0;
    pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(pass != NULL); if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    unready_family = "model";
    CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && !defaults && loads == 1);
    CHECK(strstr(error, "default construction is unavailable in this process for model:body.lwo") &&
        strstr(error, "(2 retirement default(s) failed)"));
    CHECK(!sh_resource_resident_end(pass, 1, error, sizeof(error)) && g_rr_recovery == pass);
    check_restored();
    /* A scope that cannot be opened: the drain fails before any default or load. */
    reset();
    pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(pass != NULL); if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    enter_fails = 1;
    CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && strstr(error, "scope could not be opened"));
    CHECK(!defaults && !loads);
    CHECK(!sh_resource_resident_end(pass, 1, error, sizeof(error)) && g_rr_recovery == pass);
    check_restored();
    /* Each retirement default whose scope cannot be opened is recorded and not attempted; the load runs. */
    reset();
    pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(pass != NULL); if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    enter_fails = 2;
    CHECK(sh_resource_resident_defaults(pass, error, sizeof(error)) && !defaults);
    CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && loads == 1);
    CHECK(strstr(error, "scope could not be opened for model:body.lwo") &&
        strstr(error, "(2 retirement default(s) failed)"));
    CHECK(!sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();
    /* A default's scope that does not end in order fails the pass after its work. */
    reset();
    pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
    CHECK(pass != NULL); if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    leave_misorders = 1;   /* the first default's scope; the drain's own scope opened before it */
    CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 2 && loads == 1);
    CHECK(strstr(error, "scope did not end in order for model:body.lwo") &&
        strstr(error, "(1 retirement default(s) failed)"));
    CHECK(!sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();
    /* The drain's own scope ending out of order fails a drain that otherwise succeeded. */
    reset();
    pass = begin(paths[0], 0, error);
    CHECK(pass != NULL); if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    leave_misorders = 1;
    CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && strstr(error, "scope did not end in order"));
    error[0] = 0;
    CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && strstr(error, "scope did not end in order"));
    error[0] = 0;
    CHECK(loads == 2 && !sh_resource_resident_end(pass, 1, error, sizeof(error)) &&
        strstr(error, "scope did not end in order"));
    check_restored();
    reset();
}

/* R-321 copied-size owners through the real size_owners module. objects[0] becomes the font "owned rg" in a
 * font list; the image env/default is rebuilt in place. */
static unsigned char owner_sites[4][8];
static const void *owner_returns[] = {owner_sites[0], owner_sites[1], owner_sites[2]};
static const sh_size_site owner_classes[] = {SH_SIZE_SITE_FONT, SH_SIZE_SITE_SPRITE, SH_SIZE_SITE_WIDGET};
static unsigned char *owner_sprite, owner_vtable[8], owner_movie[8], owner_material[0x190];
static int owner_size = 8, owner_measure_fails, owner_measures;
static int owner_measure(void *material, unsigned axis, int *value, char ***images, size_t *count)
{
    (void)axis; owner_measures++;
    CHECK(material == owner_material);
    if (owner_measure_fails) return 0;
    *value = owner_size;
    *images = malloc(sizeof(char *)); (*images)[0] = _strdup("env/default"); *count = 1;
    return 1;
}
static void owner_setup(int font_level)
{
    static void *font_entries[1];
    availability_setup(1, 1);
    sh_size_owners_test_reset();
    sh_size_owners_test_sites(owner_returns, owner_classes, 3);
    sh_size_owners_enable(owner_measure);
    memset(font_list, 0, sizeof(font_list));
    *(const char **)(font_list + 8) = "font";
    font_entries[0] = objects[0];
    *(void ***)(font_list + 0x20) = font_entries; *(int *)(font_list + 0x28) = 1;
    *(void **)(parm_manager + 0x18) = font_list;
    *(int *)(manager_object + 0x28) = 1;   /* the model list keeps only skin */
    *(void ***)(manager_object + 0x20) = entries + 1;
    *(const char **)(objects[0] + 8) = "owned rg"; *(void **)(objects[0] + 0x10) = font_list;
    *(int *)(objects[0] + 0x28) = font_level;
    owner_size = 8; owner_measure_fails = owner_measures = 0;
}
static void owner_font_copies(const char *image)
{
    sh_size_bracket bracket;
    char *name = (char *)image;
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_FONT, objects[0], "owned rg", NULL, -1, -1);
    CHECK(sh_size_owners_read(owner_returns[0], NULL, owner_material, SH_SIZE_AXIS_WIDTH, &name, 1));
    sh_size_owners_end(&bracket, 1);
}
static void owner_sprite_copies(void)
{
    sh_size_bracket bracket;
    char *name = "env/default";
    /* A page of its own, so a control can make the sprite read-only. */
    if (!owner_sprite) owner_sprite = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(owner_sprite != NULL);
    memset(owner_sprite, 0, 0x130);
    *(void **)owner_sprite = owner_vtable; *(void **)(owner_sprite + 0x20) = owner_movie;
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, owner_sprite, NULL, NULL, -1, 12);
    *(void **)(owner_sprite + 0x60) = owner_material;
    *(short *)(owner_sprite + 0x68) = 8; *(short *)(owner_sprite + 0x6a) = 12;
    CHECK(sh_size_owners_read(owner_returns[1], NULL, owner_material, SH_SIZE_AXIS_WIDTH, &name, 1));
    sh_size_owners_end(&bracket, 1);
}
static sh_resource_resident *owner_pass(int recover, int restoring, char error[256])
{
    static sh_package_availability_change row;
    static sh_package_changes changes;
    row = (sh_package_availability_change){"image", "env/default", recover};
    memset(&changes, 0, sizeof(changes)); changes.availability = &row; changes.availability_count = 1;
    return sh_resource_resident_begin(&changes, restoring, 0, error, 256);
}
static void owner_cases(void)
{
    char error[256], why[256];
    /* A font that copied the image's size is rebuilt with it, in either direction. */
    for (int recover = 0; recover < 2; recover++) {
        owner_setup(4); owner_font_copies("env/default"); expect_reconstructed = 7;
        sh_resource_resident *pass = owner_pass(recover, 0, error);
        if (!pass) fprintf(stderr, "font owner %d: %s\n", recover, error);
        CHECK(pass && sh_resource_resident_selected(pass, objects[0]) && sh_resource_resident_selected(pass, objects[2]));
        if (!pass) continue;
        CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
            strstr(why, "it copied the size of an image this pass rebuilds"));
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && counts[0] == 1 && counts[2] == 1);
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    }
    /* A font the pass cannot rebuild (map-scoped) makes the rebuild wait; a font that copied another image
     * is not selected. */
    owner_setup(1); owner_font_copies("env/default");
    CHECK(!owner_pass(1, 0, error) && strstr(error, "copied-size owner this pass cannot rebuild: font:owned rg (level 1)"));
    CHECK(!rebuilds && !loads);
    owner_setup(4); owner_font_copies("env/other"); expect_reconstructed = 6;
    {
        sh_resource_resident *pass = owner_pass(1, 0, error);
        CHECK(pass && !sh_resource_resident_selected(pass, objects[0]));
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && !counts[0]);
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && sh_resource_resident_end(pass, 1, error, sizeof(error)));
        }
    }
    /* A copy with no native refresh (a movie's script read a derived size) makes it wait for the movie. */
    owner_setup(4); owner_sprite_copies();
    {
        unsigned char script_object[0xc8] = {0};
        *(void **)(script_object + 0xc0) = owner_sprite;
        sh_size_owners_script_read(script_object, SH_SIZE_AXIS_WIDTH);
        CHECK(!owner_pass(1, 0, error) && strstr(error, "in-place image rebuild waits: image:env/default") &&
            strstr(error, "movie script"));
        CHECK(!rebuilds && !loads);
        sh_size_owners_movie_destroyed(owner_movie);
        owner_sprite_copies();
        expect_reconstructed = 6;
        sh_resource_resident *pass = owner_pass(1, 0, error);
        CHECK(pass != NULL);
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            owner_size = 256;
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && owner_measures == 1);
            /* The derived axis follows the rebuilt image; the explicit one keeps its value. */
            CHECK(*(short *)(owner_sprite + 0x68) == 256 && *(short *)(owner_sprite + 0x6a) == 12);
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        }
    }
    /* Return direction, then a refresh that cannot measure. A forward pass fails its drain and the provider is
     * restored; a restoring pass finishes restoring everything else but reports the failure, and a retry
     * completes it. Neither direction reports success over an unrefreshed copy. */
    for (int restoring = 0; restoring < 2; restoring++) {
        owner_setup(4); owner_sprite_copies(); expect_reconstructed = 6;
        sh_resource_resident *pass = owner_pass(0, restoring, error);
        CHECK(pass != NULL);
        if (!pass) continue;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        owner_measure_fails = 1; g_log[0] = 0;
        CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(strstr(error, "copied-size refresh: a size getter could not be measured") != NULL);
        if (restoring) CHECK(strstr(g_log, "native resident recovery left copied sizes stale") != NULL);
        CHECK(*(short *)(owner_sprite + 0x68) == 8);
        CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)));
        owner_measure_fails = 0; owner_size = 8;
        sh_resource_resident *recovery = owner_pass(1, 1, error);
        CHECK(recovery != NULL);
        if (recovery) {
            CHECK(sh_resource_resident_reconstruct(recovery, error, sizeof(error)) &&
                sh_resource_resident_drain(recovery, error, sizeof(error)));
            CHECK(sh_resource_resident_end(recovery, 1, error, sizeof(error)));
        }
    }
    /* Exact rollback (root-journal-recovery.md): the forward pass refreshed the sprite and then failed at another
     * stage. A restoration whose write-back fails reports failure and keeps the obligation (the provider's recovery
     * stays pending); the next restoration writes the value back, without measuring. */
    {
        DWORD old;
        owner_setup(4); owner_sprite_copies(); expect_reconstructed = 6;
        sh_resource_resident *pass = owner_pass(1, 0, error);
        CHECK(pass != NULL);
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            owner_size = 256;
            CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) && *(short *)(owner_sprite + 0x68) == 256);
            CHECK(sh_size_owners_journal_pending() == 1);
            CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)));
            CHECK(sh_size_owners_journal_pending() == 1);
            owner_measure_fails = 1; g_log[0] = 0;
            CHECK(VirtualProtect(owner_sprite, 0x1000, PAGE_READONLY, &old));
            sh_resource_resident *recovery = owner_pass(0, 1, error);
            CHECK(recovery != NULL);
            if (recovery) {
                CHECK(sh_resource_resident_reconstruct(recovery, error, sizeof(error)));
                CHECK(!sh_resource_resident_drain(recovery, error, sizeof(error)) && strstr(error, "stay pending"));
                CHECK(strstr(g_log, "left copied sizes stale") != NULL);
                CHECK(!sh_resource_resident_end(recovery, 0, error, sizeof(error)));
            }
            CHECK(sh_size_owners_journal_pending() == 1 && *(short *)(owner_sprite + 0x68) == 256);
            CHECK(VirtualProtect(owner_sprite, 0x1000, PAGE_READWRITE, &old));
            sh_resource_resident *retry = owner_pass(0, 1, error);
            CHECK(retry != NULL);
            if (retry) {
                CHECK(sh_resource_resident_reconstruct(retry, error, sizeof(error)) &&
                    sh_resource_resident_drain(retry, error, sizeof(error)));
                CHECK(*(short *)(owner_sprite + 0x68) == 8);
                CHECK(sh_resource_resident_end(retry, 1, error, sizeof(error)) && !sh_size_owners_journal_count());
            }
            owner_measure_fails = 0;
        }
    }
    /* Any image the pass rebuilds in place, not only a declared recovery: a rebound image identity whose
     * size a getter read for an unknown caller is isolated by the same history rule. */
    owner_setup(4);
    sh_resource_graph_history_mark("image", "env/default", SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    {
        sh_package_native_identity rebound[] = {{"image", "env/default"}};
        sh_package_changes changes = {0}; changes.rebound = rebound; changes.rebound_count = 1;
        g_log[0] = 0;
        sh_resource_resident *pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && sh_resource_resident_isolated_identity(pass, "image", "env/default") &&
            strstr(g_log, "native resident isolates image:env/default") != NULL);
        if (pass) { CHECK(sh_resource_resident_end(pass, 0, error, sizeof(error)) >= 0); }
    }
    sh_size_owners_test_reset();
    *(void **)(parm_manager + 0x18) = NULL;
    reset();
}

/* R-321 dependency observation of retirement defaults. objects[0] is the discrete animation anim/retired,
 * alone in its list; objects[2] is the image env/default in an image list that objects[1] joins as env/later
 * only when a case registers it. The retirement default reads default_reads through the hooked finds,
 * recorded as resource_graph_native.c records them: a found image as a reference, a name the list does not
 * hold only as a request. */
static void *dependency_images[2];
/* The observation predicate, by object as the backend's is by the default routine an object runs (R-352). It
 * records in the default's frame what the default read through a holder no hook sees (observed_copy, a material)
 * whether or not it accepts, as the backend records a routine's census reads, and accepts observed_object and
 * observed_also. It counts its calls and whether a request frame was current. */
static void *observed_object, *observed_also;
static const char *observed_copy;
static int observed_calls, observed_in_frame;
static int object_observed(void *resource)
{
    observed_calls++;
    observed_in_frame = sh_resource_graph_recording_requests();
    if (observed_copy) sh_resource_graph_reference("material", observed_copy);
    return resource && (resource == observed_object || resource == observed_also);
}
static void dependency_default(void *self)
{
    for (int i = 0; i < 3 && default_reads[i]; i++) {
        void **items = *(void ***)(parm_manager + 0x20), *found = NULL;
        for (int j = 0; j < *(int *)(parm_manager + 0x28); j++)
            if (!_stricmp(*(const char **)((unsigned char *)items[j] + 8), default_reads[i])) found = items[j];
        if (sh_resource_graph_recording()) {
            if (found) sh_resource_graph_reference("image", *(const char **)((unsigned char *)found + 8));
            else sh_resource_graph_request("image", default_reads[i]);
        }
        if (found) default_found++; else default_missing++;
    }
    make_default(self);
}
static void dependency_setup(int holder)
{
    sh_resource_graph_frame frame;
    reset(); g_log[0] = 0;
    memset(parm_manager, 0, sizeof(parm_manager));
    *(const char **)(parm_manager + 8) = "image";
    dependency_images[0] = objects[2]; dependency_images[1] = objects[1];
    *(void ***)(parm_manager + 0x20) = dependency_images; *(int *)(parm_manager + 0x28) = 1;
    *(void **)(manager_object + 0x18) = parm_manager; *(int *)(manager_object + 0x28) = 1;
    *(const char **)(manager_object + 8) = "discreteAnimation";
    *(const char **)(objects[0] + 8) = "anim/retired";
    *(void **)(objects[1] + 0x10) = parm_manager; *(const char **)(objects[1] + 8) = "env/later";
    *(void **)(objects[2] + 0x10) = parm_manager; *(const char **)(objects[2] + 8) = "env/default";
    resource_table[0x50/8] = dependency_default;
    sh_resource_resident_set_default_observation(object_observed); observed_object = objects[0];
    observed_also = NULL; observed_copy = NULL; observed_calls = observed_in_frame = 0;
    default_reads[0] = "env/default";
    sh_resource_graph_test_reset(); sh_resource_graph_size_history_begin();
    if (holder) {
        /* A fixed renderer holder: the image has a completely observed recorded consumer of its own. */
        sh_resource_graph_begin(&frame, "renderParm", "userenvmap");
        sh_resource_graph_reference("image", "env/default"); sh_resource_graph_end(&frame, 1);
    }
}
static void dependency_counters(int reconstructed)
{
    memset(counts, 0, sizeof(counts));
    rebuilds = loads = defaults = scoped_defaults = default_found = default_missing = 0;
    expect_reconstructed = reconstructed;
}
/* A provider difference that removes anim/retired. */
static sh_resource_resident *dependency_retire(char error[256])
{
    static sh_package_native_identity removed;
    static sh_package_changes changes;
    removed = (sh_package_native_identity){"discreteanimation", "anim/retired"};
    memset(&changes, 0, sizeof(changes));
    changes.removed = &removed; changes.removed_count = 1; changes.identities_complete = 1;
    return sh_resource_resident_begin(&changes, 0, 0, error, 256);
}
/* The image env/default's in-place recovery, or its return to the native image decision (R-309). */
static sh_resource_resident *dependency_recover(int recover, int restoring, char error[256])
{
    static sh_package_availability_change row;
    static sh_package_changes changes;
    row = (sh_package_availability_change){"image", "env/default", recover};
    memset(&changes, 0, sizeof(changes)); changes.availability = &row; changes.availability_count = 1;
    return sh_resource_resident_begin(&changes, restoring, 0, error, 256);
}
/* A provider that adds one identity. */
static sh_resource_resident *dependency_add(const char *type, const char *name, int restoring, char error[256])
{
    static sh_package_native_identity added;
    static sh_package_changes changes;
    added = (sh_package_native_identity){(char *)type, (char *)name};
    memset(&changes, 0, sizeof(changes));
    changes.added = &added; changes.added_count = 1; changes.identities_complete = 1;
    return sh_resource_resident_begin(&changes, restoring, 0, error, 256);
}
/* Reconstruct, rebuilt leaves, the drain and the end of a pass expected to succeed. */
static void dependency_complete(sh_resource_resident *pass, char error[256])
{
    if (!pass) return;
    CHECK(sh_resource_resident_reconstruct(pass, error, 256));
    CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, 256));
    CHECK(sh_resource_resident_drain(pass, error, 256));
    CHECK(sh_resource_resident_end(pass, 1, error, 256));
    check_restored();
}
static int dependency_retired(void)
{
    rr_entry probe = {0};
    probe.resource = objects[0]; probe.type = "discreteanimation"; probe.name = "anim/retired";
    return rr_retired_by_provider(&probe);
}
/* An image's recorded consumer closure as the in-place recovery judges it; named: it reaches anim/retired. */
static int dependency_closure(const char *image, int *named, char first[200])
{
    sh_resource_graph_identity seed = {"image", (char *)image};
    sh_resource_graph_impact impact = {0};
    int status = sh_resource_graph_consumers_observed(&seed, 1, rr_fixed_renderer, NULL, &impact, first, 200);
    *named = 0;
    for (size_t i = 0; i < impact.count; i++)
        if (!strcmp(impact.items[i].type, "discreteanimation") && !strcmp(impact.items[i].name, "anim/retired")) *named = 1;
    sh_resource_graph_impact_free(&impact);
    return status;
}
static int impact_names(const sh_resource_graph_impact *impact, const char *type, const char *name)
{
    for (size_t i = 0; i < impact->count; i++)
        if (!strcmp(impact->items[i].type, type) && !strcmp(impact->items[i].name, name)) return 1;
    return 0;
}
static void dependency_cases(void)
{
    char error[256], why[300], first[200];
    sh_resource_resident *pass;
    int named;

    /* A completed default dependency frame: the default returned in a scope that ended in order, and its
     * family's reads are observed. Its record lists the image it found and the name it asked for and did not
     * find; each one's consumer closure counts the identity as completely observed. */
    dependency_setup(0); dependency_counters(1);
    default_reads[1] = "env/missing";
    pass = dependency_retire(error);
    CHECK(pass && pass->count == 1 && sh_resource_resident_selected(pass, objects[0]));
    if (pass) {
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(defaults == 1 && scoped_defaults == 1 && !loads && default_found == 1 && default_missing == 1);
        CHECK(observed_calls == 1 && observed_in_frame); /* asked once, while the default's frame was current */
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    CHECK(dependency_retired() && g_rr_retirement_count == 1 && (objects[0][0x2c] & RR_DEFAULT));
    CHECK(dependency_closure("env/default", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named && !first[0]);
    CHECK(dependency_closure("env/missing", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named && !first[0]);
    CHECK(dependency_closure("env/later", &named, first) == SH_RESOURCE_GRAPH_INCOMPLETE && !named);

    /* Incomplete frames: no observation predicate, a predicate that does not accept this object's default (it
     * accepts another object of the same family), a default that raised an engine error, a default whose scope
     * did not end in order. The predicate is asked only for a default that returned in order. Each keeps the
     * edges its default recorded, so a closure still reaches the identity, never as completely observed. */
    for (int kind = 0; kind < 4; kind++) {
        dependency_setup(0); dependency_counters(1);
        default_reads[1] = "env/missing";
        if (kind == 0) sh_resource_resident_set_default_observation(NULL);
        if (kind == 1) observed_object = objects[1];
        if (kind == 2) fault = 7;
        pass = dependency_retire(error);
        CHECK(pass != NULL); if (!pass) continue;
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        if (kind == 3) leave_misorders = 1; /* the default's own scope; the drain's scope opened before it */
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)) == (kind < 2));
        CHECK(defaults == 1 && default_found == 1 && default_missing == 1);
        CHECK(observed_calls == (kind == 1));
        CHECK(sh_resource_resident_end(pass, kind < 2, error, sizeof(error)) == (kind < 2));
        fault = 0; check_restored();
        if (dependency_closure("env/default", &named, first) != SH_RESOURCE_GRAPH_INCOMPLETE || !named)
            fprintf(stderr, "incomplete frame %d: %s\n", kind, first);
        CHECK(dependency_closure("env/default", &named, first) == SH_RESOURCE_GRAPH_INCOMPLETE && named &&
            !strcmp(first, "discreteanimation:anim/retired"));
        CHECK(dependency_closure("env/missing", &named, first) == SH_RESOURCE_GRAPH_INCOMPLETE && named);
    }

    /* Recovery selecting a retired default: an in-place image recovery whose recorded consumer closure reaches
     * the identity through its completed default frame retires it again. Its default is rebuilt inside the
     * scope, it is not loaded, and it stays revivable. */
    dependency_setup(0); dependency_counters(1);
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    dependency_counters(5);
    pass = dependency_recover(1, 0, error);
    if (!pass) fprintf(stderr, "recovery of a retired default: %s\n", error);
    CHECK(pass && pass->count == 2 && sh_resource_resident_selected(pass, objects[0]) &&
        sh_resource_resident_selected(pass, objects[2]));
    if (pass) {
        CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
            strstr(why, "retired by this provider; its default is rebuilt"));
        dependency_complete(pass, error);
        CHECK(counts[0] == 1 && counts[2] == 1 && defaults == 1 && scoped_defaults == 1 && loads == 1);
    }
    CHECK(dependency_retired() && g_rr_retirement_count == 1 && (objects[0][0x2c] & RR_DEFAULT));
    CHECK(dependency_closure("env/default", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);
    /* The same recovery refuses before any change when the retired default's frame ended incomplete, even
     * though a predicate is set by the time of the recovery. */
    dependency_setup(0); dependency_counters(1);
    sh_resource_resident_set_default_observation(NULL);
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    sh_resource_resident_set_default_observation(object_observed);
    dependency_counters(5);
    pass = dependency_recover(1, 0, error);
    CHECK(!pass && strstr(error, "cannot establish its consumers: discreteanimation:anim/retired is not completely observed"));
    if (pass) sh_resource_resident_end(pass, 1, error, sizeof(error));
    CHECK(!rebuilds && !loads && !defaults && dependency_retired()); check_restored();
    /* An unrelated recovery: the retired default read another image, which no list holds. The recovery of
     * env/default neither reaches it nor waits on its incomplete frame. */
    dependency_setup(1); dependency_counters(1);
    sh_resource_resident_set_default_observation(NULL);
    default_reads[0] = "env/elsewhere";
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    dependency_counters(4);
    pass = dependency_recover(1, 0, error);
    if (!pass) fprintf(stderr, "unrelated recovery: %s\n", error);
    CHECK(pass && pass->count == 1 && !sh_resource_resident_selected(pass, objects[0]));
    if (pass) {
        dependency_complete(pass, error);
        CHECK(!counts[0] && counts[2] == 1 && !defaults && loads == 1);
    }
    CHECK(dependency_closure("env/elsewhere", &named, first) == SH_RESOURCE_GRAPH_INCOMPLETE && named);

    /* A requested name that becomes available later. The default asked for env/later, which no list held,
     * and the request is recorded. An ordinary live parse making the same missing read records nothing. */
    dependency_setup(0); dependency_counters(1);
    default_reads[0] = "env/later";
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    CHECK(defaults == 1 && default_missing == 1 && !default_found);
    CHECK(dependency_closure("env/later", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);
    {
        sh_resource_graph_frame frame;
        sh_resource_graph_begin(&frame, "discreteanimation", "anim/live");
        sh_resource_graph_request("image", "env/later");
        sh_resource_graph_reference("image", "env/default");
        sh_resource_graph_end(&frame, 1);
    }
    /* An unrelated addition reaches neither, and leaves the request in place. */
    dependency_counters(0);
    pass = dependency_add("image", "env/unrelated", 0, error);
    CHECK(pass && !sh_resource_resident_selected_count(pass));
    dependency_complete(pass, error);
    CHECK(!counts[0] && !defaults && dependency_closure("env/later", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);
    /* The same addition admitted additively is additive_refresh_cases' subject. */
    /* A provider that adds the image: the retired default is its recorded consumer, so the pass retires it
     * again, and its default, rebuilt after the addition registered, finds the image. */
    dependency_counters(1);
    pass = dependency_add("image", "env/later", 0, error);
    CHECK(pass && pass->count == 1 && sh_resource_resident_selected(pass, objects[0]));
    if (pass) {
        sh_resource_graph_impact prior = {0};
        CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
            strstr(why, "retired by this provider; its default is rebuilt"));
        CHECK(sh_resource_resident_take_prior(pass, &prior));
        CHECK(impact_names(&prior, "discreteanimation", "anim/retired") && !impact_names(&prior, "discreteanimation", "anim/live"));
        sh_resource_graph_impact_free(&prior);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        *(int *)(parm_manager + 0x28) = 2; /* the added image registers before the defaults run */
        CHECK(sh_resource_resident_drain(pass, error, sizeof(error)));
        CHECK(counts[0] == 1 && defaults == 1 && !loads && default_found == 1 && !default_missing);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    CHECK(dependency_retired() && g_rr_retirement_count == 1);
    CHECK(dependency_closure("env/later", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);

    /* Repeated retirement. Every withdrawal rebuilds the default inside the scope and replaces its frame, so
     * the identity's requests follow its latest default. The ledger keeps one row and the shell stays
     * revivable; a later incomplete frame is not rescued by an earlier complete one. */
    dependency_setup(0); dependency_counters(1);
    default_reads[1] = "env/later";
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    CHECK(defaults == 1 && g_rr_retirement_count == 1);
    CHECK(dependency_closure("env/later", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);
    default_reads[1] = NULL; dependency_counters(5);
    pass = dependency_recover(1, 0, error);
    CHECK(pass && pass->count == 2);
    dependency_complete(pass, error);
    CHECK(defaults == 1 && scoped_defaults == 1 && g_rr_retirement_count == 1 && dependency_retired());
    CHECK(dependency_closure("env/later", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && !named);
    CHECK(dependency_closure("env/default", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);
    dependency_counters(1);
    pass = dependency_retire(error);
    CHECK(pass && pass->count == 1);
    dependency_complete(pass, error);
    CHECK(defaults == 1 && scoped_defaults == 1 && g_rr_retirement_count == 1 && dependency_retired());
    sh_resource_resident_set_default_observation(NULL); dependency_counters(1);
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    sh_resource_resident_set_default_observation(object_observed);
    CHECK(dependency_closure("env/default", &named, first) == SH_RESOURCE_GRAPH_INCOMPLETE && named &&
        !strcmp(first, "discreteanimation:anim/retired"));
    pass = dependency_recover(1, 0, error);
    CHECK(!pass && strstr(error, "discreteanimation:anim/retired is not completely observed"));
    if (pass) sh_resource_resident_end(pass, 1, error, sizeof(error));
    CHECK(defaults == 1 && g_rr_retirement_count == 1 && dependency_retired());
    pass = dependency_add("discreteanimation", "anim/retired", 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, NULL, 0) == 0 &&
        sh_resource_resident_revives(pass, objects[0], "discreteanimation", "anim/retired"));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();

    /* Recovery after a failed pass. The restoration rebuilds a retired identity the failed pass retained from
     * the provider it restores. When that provider supplies it again, it leaves the ledger (control). */
    dependency_setup(0); dependency_counters(1);
    pass = dependency_retire(error);
    CHECK(pass && sh_resource_resident_reconstruct(pass, error, sizeof(error)));
    if (pass) {
        vmr_failed = 1;
        CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 1);
        CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)) && g_rr_recovery == pass);
        vmr_failed = 0;
    }
    check_restored(); CHECK(dependency_retired());
    dependency_counters(1);
    pass = dependency_add("discreteanimation", "anim/retired", 1, error);
    CHECK(pass && pass->count == 1);
    if (pass) {
        dependency_complete(pass, error);
        CHECK(loads == 1 && !defaults && !(objects[0][0x2c] & RR_DEFAULT) && !g_rr_recovery);
    }
    CHECK(!dependency_retired() && !g_rr_retirement_count);
    /* Retired before the failed pass, retired again by it, and supplied by neither provider: the restoration
     * loads it from no source and it comes back as the engine default. It must stay the retired shell the
     * ledger keeps, revivable by an addition that names it (R-321: a row leaves when a pass supplies the
     * identity again). */
    dependency_setup(0); dependency_counters(1);
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    dependency_counters(5);
    pass = dependency_recover(1, 0, error);
    CHECK(pass && pass->count == 2);
    if (pass) {
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
        vmr_failed = 1;
        CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 1);
        CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)) && g_rr_recovery == pass);
        vmr_failed = 0;
    }
    check_restored(); CHECK(dependency_retired());
    dependency_counters(5); unsupplied = objects[0];
    pass = dependency_recover(0, 1, error);
    if (!pass) fprintf(stderr, "restoration after a re-retirement: %s\n", error);
    CHECK(pass && sh_resource_resident_selected(pass, objects[0]));
    if (pass) {
        CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
            strstr(why, "retained from the failed activation, retired by this provider; its default is rebuilt"));
        dependency_complete(pass, error);
        CHECK((objects[0][0x2c] & RR_DEFAULT) && !g_rr_recovery);
        CHECK(defaults == 1 && scoped_defaults == 1 && loads == 1); /* its default is rebuilt; only the image loads */
    }
    unsupplied = NULL;
    if (!dependency_retired()) fprintf(stderr, "restoration dropped retired discreteanimation:anim/retired from the ledger\n");
    CHECK(dependency_retired() && g_rr_retirement_count == 1);
    CHECK(dependency_closure("env/default", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);
    pass = dependency_add("discreteanimation", "anim/retired", 0, error);
    if (pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)))
        fprintf(stderr, "restored retired shell blocks its revival: %s\n", first);
    CHECK(pass && sh_resource_resident_blocking_count(pass, NULL, 0) == 0);
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();
    /* The rule does not depend on how the failed pass reached the shell or on a complete default frame: a
     * replaced image path selects it as a recorded consumer of an incomplete frame, the pass fails, and its
     * reversal (the same replaced path) retires it again instead of loading it. */
    dependency_setup(0); dependency_counters(1);
    sh_resource_resident_set_default_observation(NULL);
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    dependency_counters(5);
    pass = begin(paths[2], 0, error);
    CHECK(pass && pass->count == 2 && sh_resource_resident_selected(pass, objects[0]));
    if (pass) {
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
        vmr_failed = 1;
        CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 1);
        CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)) && g_rr_recovery == pass);
        vmr_failed = 0;
    }
    check_restored();
    dependency_counters(5); unsupplied = objects[0];
    pass = begin(paths[2], 1, error);
    CHECK(pass && pass->count == 2);
    if (pass) {
        CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
            strstr(why, "retained from the failed activation, retired by this provider; its default is rebuilt"));
        dependency_complete(pass, error);
        CHECK(defaults == 1 && loads == 1 && (objects[0][0x2c] & RR_DEFAULT) && !g_rr_recovery);
    }
    unsupplied = NULL;
    CHECK(dependency_retired() && g_rr_retirement_count == 1);
    pass = dependency_add("discreteanimation", "anim/retired", 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, NULL, 0) == 0);
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();
    /* Identity checks hold in a restoration too. A retained default this provider never retired (a missing
     * declaration a live parse looked up) loads as before and never enters the ledger; a retired identity
     * whose object kept a native record is source-bearing, loads, and leaves it. Both still block a revival. */
    for (int kind = 0; kind < 2; kind++) {
        sh_resource_graph_frame frame;
        dependency_setup(0); dependency_counters(1);
        if (!kind) {
            objects[0][0x2c] = RR_DEFAULT; unsupplied = objects[0];
            sh_resource_graph_begin(&frame, "discreteanimation", "anim/retired");
            sh_resource_graph_reference("image", "env/default"); sh_resource_graph_end(&frame, 1);
        } else {
            pass = dependency_retire(error); CHECK(pass != NULL);
            dependency_complete(pass, error);
        }
        dependency_counters(5);
        pass = dependency_recover(1, 0, error);
        CHECK(pass && pass->count == 2);
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
            vmr_failed = 1;
            CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)));
            CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)) && g_rr_recovery == pass);
            vmr_failed = 0;
        }
        check_restored();
        if (kind) *(void **)(objects[0] + 0x18) = objects[2]; /* the failed pass left it a native record */
        dependency_counters(5); unsupplied = objects[0];
        pass = dependency_recover(0, 1, error);
        CHECK(pass && sh_resource_resident_selected(pass, objects[0]));
        if (pass) {
            CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) && !strstr(why, "retired by this provider"));
            dependency_complete(pass, error);
            CHECK(loads == 2 && !defaults && !g_rr_recovery);
        }
        unsupplied = NULL;
        CHECK(!dependency_retired() && !g_rr_retirement_count);
        pass = dependency_add("discreteanimation", "anim/retired", 0, error);
        CHECK(pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 1 &&
            strstr(first, kind ? "default with a native record" : "default this provider never retired"));
        if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        *(void **)(objects[0] + 0x18) = NULL; check_restored();
    }
    /* What decides is the identity's state as the failed activation began, carried to whichever restoration
     * completes, not that restoration's own changes. A failed pass removes anim/second, retiring it for the
     * first time, and retires anim/retired again; its reversal cannot open its scopes, and a later activation
     * is refused while the recovery is pending. That activation's own restoration names neither identity:
     * anim/second, live before the failed pass and supplied by the restored provider, loads and leaves the
     * ledger, and anim/retired stays the retired shell. */
    {
        static sh_package_native_identity second = {"discreteanimation", "anim/second"};
        static sh_package_availability_change row = {"image", "env/default", 1};
        sh_package_changes changes = {0};
        dependency_setup(0); dependency_counters(1);
        *(void **)(objects[1] + 0x10) = manager_object; *(const char **)(objects[1] + 8) = "anim/second";
        *(int *)(manager_object + 0x28) = 2; observed_also = objects[1];
        pass = dependency_retire(error); CHECK(pass != NULL);
        dependency_complete(pass, error);
        dependency_counters(7);
        changes.removed = &second; changes.removed_count = 1; changes.identities_complete = 1;
        changes.availability = &row; changes.availability_count = 1; row.recover = 1;
        pass = sh_resource_resident_begin(&changes, 0, 0, error, sizeof(error));
        CHECK(pass && pass->count == 3);
        if (pass) {
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
            CHECK(sh_resource_resident_drain_leaves(pass, image_leaf, error, sizeof(error)));
            vmr_failed = 1;
            CHECK(!sh_resource_resident_drain(pass, error, sizeof(error)) && defaults == 2);
            CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)) && g_rr_recovery == pass);
            vmr_failed = 0;
        }
        check_restored(); CHECK(g_rr_retirement_count == 2);
        /* Its reversal, as sh_package_changes_reverse forms it: the removal becomes an addition, the recovery
         * returns. */
        changes.added = &second; changes.added_count = 1; changes.removed = NULL; changes.removed_count = 0;
        row.recover = 0; render_fault = 1;
        CHECK(!sh_resource_resident_begin(&changes, 1, 0, error, sizeof(error)) && g_rr_recovery);
        render_fault = 0; check_restored();
        CHECK(!dependency_recover(1, 0, error) && g_rr_recovery); check_restored();
        dependency_counters(7);
        pass = dependency_recover(0, 1, error);
        if (!pass) fprintf(stderr, "chained restoration: %s\n", error);
        CHECK(pass && pass->count == 3);
        if (pass) {
            CHECK(sh_resource_resident_describe(pass, objects[1], why, sizeof(why)) && !strstr(why, "retired by this provider"));
            CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
                strstr(why, "retained from the failed activation, retired by this provider; its default is rebuilt"));
            dependency_complete(pass, error);
            CHECK(defaults == 1 && loads == 2 && !g_rr_recovery);
            CHECK((objects[0][0x2c] & RR_DEFAULT) && !(objects[1][0x2c] & RR_DEFAULT));
        }
        CHECK(dependency_retired() && g_rr_retirement_count == 1);
    }
    reset();
}

/* R-352: what the predicate records while the default's frame is current is part of the identity's record,
 * complete or not. A default that copied material _tracemodel through a holder no hook sees (the collision model
 * default) is a recorded consumer of it: an addition or a withdrawal of that identity retires the default again and
 * rebuilds it, whether its frame completed or not. An in-place image recovery that reaches it through the material
 * retires it again when its frame completed and refuses when it did not. Without the recorded read none of these
 * reaches it: the missed dependency the census records. */
static sh_resource_resident *observation_withdraw(char error[256])
{
    static sh_package_native_identity removed = {"material", "_tracemodel"};
    static sh_package_changes changes;
    memset(&changes, 0, sizeof(changes));
    changes.removed = &removed; changes.removed_count = 1; changes.identities_complete = 1;
    return sh_resource_resident_begin(&changes, 0, 0, error, 256);
}
static void observation_cases(void)
{
    char error[256], why[300], first[200];
    sh_resource_resident *pass;
    sh_resource_graph_identity seed = {"material", "_tracemodel"};
    sh_resource_graph_impact impact = {0};
    int named;
    for (int complete = 1; complete >= 0; complete--) for (int copy = 1; copy >= 0; copy--) for (int change = 0; change < 3; change++) {
        dependency_setup(0); dependency_counters(1);
        if (!complete) observed_object = NULL;
        observed_copy = copy ? "_tracemodel" : NULL;
        if (change == 2) default_reads[0] = NULL; /* the recovery reaches it only through the material */
        pass = dependency_retire(error); CHECK(pass != NULL);
        dependency_complete(pass, error);
        CHECK(defaults == 1 && observed_calls == 1 && observed_in_frame);
        CHECK(sh_resource_graph_consumers(&seed, 1, &impact) != SH_RESOURCE_GRAPH_ERROR &&
            impact_names(&impact, "discreteanimation", "anim/retired") == copy);
        sh_resource_graph_impact_free(&impact);
        if (change < 2) {
            CHECK(dependency_closure("env/default", &named, first) ==
                (complete ? SH_RESOURCE_GRAPH_COMPLETE : SH_RESOURCE_GRAPH_INCOMPLETE) && named);
            /* An addition, then a withdrawal, of the copied identity. */
            dependency_counters(copy);
            pass = change ? observation_withdraw(error) : dependency_add("material", "_tracemodel", 0, error);
            if (!pass) fprintf(stderr, "observation change %d (complete %d, copy %d): %s\n", change, complete, copy, error);
            CHECK(pass && sh_resource_resident_selected(pass, objects[0]) == copy &&
                sh_resource_resident_selected_count(pass) == (size_t)copy);
            if (pass && copy) CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
                strstr(why, "retired by this provider; its default is rebuilt"));
            dependency_complete(pass, error);
            CHECK(defaults == copy && !loads && observed_calls == 1 + copy);
        } else {
            /* An in-place recovery of an image the material names, with the material completely observed. */
            sh_resource_graph_frame frame;
            sh_resource_graph_begin(&frame, "material", "_tracemodel");
            sh_resource_graph_reference("image", "env/default"); sh_resource_graph_end(&frame, 1);
            dependency_counters(copy ? 5 : 4);
            pass = dependency_recover(1, 0, error);
            if (copy && !complete) {
                CHECK(!pass && strstr(error, "discreteanimation:anim/retired is not completely observed"));
                if (pass) sh_resource_resident_end(pass, 1, error, sizeof(error));
                CHECK(!rebuilds && !defaults); check_restored();
            } else {
                if (!pass) fprintf(stderr, "observation recovery (complete %d, copy %d): %s\n", complete, copy, error);
                CHECK(pass && sh_resource_resident_selected(pass, objects[0]) == copy &&
                    sh_resource_resident_selected(pass, objects[2]));
                dependency_complete(pass, error);
                CHECK(defaults == copy && loads == 1);
            }
        }
        CHECK(dependency_retired() && g_rr_retirement_count == 1);
        /* A rebuilt default recorded the read again. */
        CHECK(sh_resource_graph_consumers(&seed, 1, &impact) != SH_RESOURCE_GRAPH_ERROR &&
            impact_names(&impact, "discreteanimation", "anim/retired") == copy);
        sh_resource_graph_impact_free(&impact);
    }
    reset();
}

/* An additive admission (a prefab placement) that adds the image a retired default asked for. The resident
 * calls follow decl_server's ds_occurrence_activate: begin, the blocking check, the sorted pre-admission
 * ownership list, release, reconstruct before the addition's declarations exist, then the drain once they
 * do (here: the image registers between the two). The refreshed shell is the admission's own; its old
 * default stays intact until the defaults rebuild it, so the rebuilt default finds the added image. */
static size_t additive_existing(void *existing[3])
{
    size_t count = 0;
    void **lists[2] = {*(void ***)(manager_object + 0x20), *(void ***)(parm_manager + 0x20)};
    int counts_in[2] = {*(int *)(manager_object + 0x28), *(int *)(parm_manager + 0x28)};
    for (int l = 0; l < 2; l++) for (int i = 0; i < counts_in[l] && count < 3; i++) existing[count++] = lists[l][i];
    qsort(existing, count, sizeof(*existing), rr_pointer_compare);
    return count;
}
static void additive_refresh_cases(void)
{
    char error[256], why[300], first[200];
    sh_resource_resident *pass;
    void *existing[3];
    size_t existing_count;
    int named;

    /* The previously refused case: the request recorded by the retirement default, then the addition. */
    dependency_setup(0); dependency_counters(1);
    default_reads[0] = "env/later";
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    CHECK(default_missing == 1 && dependency_retired());
    dependency_counters(1); g_log[0] = 0;
    existing_count = additive_existing(existing);
    pass = dependency_add("image", "env/later", 0, error);
    CHECK(pass && pass->count == 1 && sh_resource_resident_selected(pass, objects[0]));
    if (pass) {
        if (sh_resource_resident_blocking_count(pass, first, sizeof(first))) fprintf(stderr, "additive refresh blocks: %s\n", first);
        CHECK(sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 0 && !first[0]);
        CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
            strstr(why, "retired by this provider; its default is rebuilt"));
        existing_count = sh_resource_resident_release_revived(pass, existing, existing_count);
        CHECK(existing_count == 1 && existing[0] == objects[2] && strstr(g_log, "refreshes retired discreteanimation:anim/retired"));
        /* Before the addition's declarations exist, its old default stays intact for any lookup. */
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && !counts[0] && !defaults);
        CHECK(lookup(manager_object, "anim/retired") == objects[0] && (objects[0][0x2c] & RR_DEFAULT) && !counts[0]);
        *(int *)(parm_manager + 0x28) = 2; /* the addition's image registers */
        CHECK(sh_resource_resident_drain_additions(pass, existing, existing_count, error, sizeof(error)));
        CHECK(counts[0] == 1 && defaults == 1 && scoped_defaults == 1 && default_found == 1 && !default_missing && !loads);
        CHECK(sh_resource_resident_drain_additions(pass, existing, existing_count, error, sizeof(error)) && defaults == 1);
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    CHECK(dependency_retired() && g_rr_retirement_count == 1 && (objects[0][0x2c] & RR_DEFAULT));
    CHECK(dependency_closure("env/later", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);

    /* An unrelated addition selects nothing and leaves the shell and its request alone. */
    dependency_setup(0); dependency_counters(0);
    default_reads[0] = "env/later";
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    dependency_counters(0);
    existing_count = additive_existing(existing);
    pass = dependency_add("image", "env/unrelated", 0, error);
    CHECK(pass && !sh_resource_resident_selected_count(pass) && !sh_resource_resident_blocking_count(pass, NULL, 0));
    if (pass) {
        CHECK(sh_resource_resident_release_revived(pass, existing, existing_count) == existing_count);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        CHECK(sh_resource_resident_drain_additions(pass, existing, existing_count, error, sizeof(error)));
        CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error))); check_restored();
    }
    CHECK(!counts[0] && !defaults && dependency_closure("env/later", &named, first) == SH_RESOURCE_GRAPH_COMPLETE && named);

    /* Exclusions: a live object, an engine default this provider never retired and a retired identity whose
     * object keeps a native record are existing consumers and still block, before any change; so does a
     * refreshable shell with a live recorded consumer of its own. */
    for (int kind = 0; kind < 4; kind++) {
        sh_resource_graph_frame frame;
        dependency_setup(0); dependency_counters(1);
        default_reads[0] = "env/later";
        if (kind == 3) {
            *(void **)(objects[1] + 0x10) = manager_object; *(const char **)(objects[1] + 8) = "anim/dependent";
            *(int *)(manager_object + 0x28) = 2;
        }
        pass = dependency_retire(error); CHECK(pass != NULL);
        dependency_complete(pass, error);
        if (kind == 0) { objects[0][0x2c] = 0; ledger_clear(); }
        if (kind == 1) ledger_clear();
        if (kind == 2) *(void **)(objects[0] + 0x18) = objects[2];
        if (kind == 3) {
            sh_resource_graph_begin(&frame, "discreteanimation", "anim/dependent");
            sh_resource_graph_reference("discreteanimation", "anim/retired"); sh_resource_graph_end(&frame, 1);
        }
        dependency_counters(1);
        pass = dependency_add("image", "env/later", 0, error);
        CHECK(pass && sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 1);
        CHECK(strstr(first, kind == 3 ? "discreteanimation:anim/dependent: the addition does not name it" :
            "discreteanimation:anim/retired: the addition does not name it") != NULL);
        if (kind < 3) CHECK(pass && sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) &&
            !strstr(why, "retired by this provider"));
        if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
        CHECK(!rebuilds && !defaults && !loads);
        *(void **)(objects[0] + 0x18) = NULL; check_restored();
    }

    /* A failure after the refresh: the admission's restoration retires the shell again under the restored
     * provider, which removes the added image, and the shell stays revivable. */
    dependency_setup(0); dependency_counters(1);
    default_reads[0] = "env/later";
    pass = dependency_retire(error); CHECK(pass != NULL);
    dependency_complete(pass, error);
    dependency_counters(1);
    existing_count = additive_existing(existing);
    pass = dependency_add("image", "env/later", 0, error);
    CHECK(pass && !sh_resource_resident_blocking_count(pass, NULL, 0));
    if (pass) {
        existing_count = sh_resource_resident_release_revived(pass, existing, existing_count);
        CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)));
        *(int *)(parm_manager + 0x28) = 2;
        vmr_failed = 1;
        CHECK(!sh_resource_resident_drain_additions(pass, existing, existing_count, error, sizeof(error)) && defaults == 1);
        CHECK(!sh_resource_resident_end(pass, 0, error, sizeof(error)) && g_rr_recovery == pass);
        vmr_failed = 0;
    }
    check_restored();
    {
        static sh_package_native_identity removed = {"image", "env/later"};
        sh_package_changes changes = {0};
        changes.removed = &removed; changes.removed_count = 1; changes.identities_complete = 1;
        dependency_counters(3);
        pass = sh_resource_resident_begin(&changes, 1, 0, error, sizeof(error));
        if (!pass) fprintf(stderr, "additive refresh restoration: %s\n", error);
        CHECK(pass && sh_resource_resident_selected(pass, objects[0]) && !sh_resource_resident_selected(pass, objects[2]));
        if (pass) {
            /* A restoration admits no addition: every selection, the shell included, is the recovery's. */
            CHECK(sh_resource_resident_blocking_count(pass, first, sizeof(first)) == 2 && strstr(first, "recovery"));
            CHECK(sh_resource_resident_describe(pass, objects[0], why, sizeof(why)) && strstr(why, "retired by this provider"));
            CHECK(sh_resource_resident_reconstruct(pass, error, sizeof(error)) && counts[0] == 1);
            CHECK(sh_resource_resident_drain_additions(pass, existing, existing_count, error, sizeof(error)));
            CHECK(defaults == 2 && g_rr_recovery != NULL);
            CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)) && !g_rr_recovery); check_restored();
        }
    }
    CHECK(dependency_retired() && (objects[0][0x2c] & RR_DEFAULT));
    pass = dependency_add("discreteanimation", "anim/retired", 0, error);
    CHECK(pass && sh_resource_resident_blocking_count(pass, NULL, 0) == 0 &&
        sh_resource_resident_revives(pass, objects[0], "discreteanimation", "anim/retired"));
    if (pass) CHECK(sh_resource_resident_end(pass, 1, error, sizeof(error)));
    check_restored();
    reset();
}

static int probe_mode, probe_prepares, probe_finishes, probe_native_calls, probe_post_calls, probe_invokes;
static void probe_native(void *resource);
static void probe_prepare(sh_resource_default_read_receipt *receipt, sh_resource_default_read_input *input,
    void *resource, void *actual)
{
    CHECK(sh_resource_graph_recording_requests() && actual == probe_native);
    probe_prepares++; input->resource = (uintptr_t)resource; input->default_callback = (uintptr_t)actual;
    receipt->invocation = *input; receipt->work = ((unsigned char *)resource)[0x68]; receipt->state = 1;
    CHECK(receipt->work == (resource == objects[0] ? 17u : 29u));
    if (probe_mode == 1) RaiseException(0xe0800040u, 0, 0, NULL);
    receipt->complete = probe_mode != 5;
}
static int probe_finish(sh_resource_default_read_receipt *receipt, const sh_resource_default_read_input *input,
    int returned, int ordered)
{
    probe_finishes++; CHECK(sh_resource_graph_recording_requests());
    if (probe_mode == 1) { CHECK(returned && !ordered && !input->resource && !receipt->state); return 0; }
    CHECK(receipt->state == 1 && receipt->invocation.resource == input->resource);
    CHECK(input->default_callback == (uintptr_t)probe_native && !((unsigned char *)input->resource)[0x68]);
    CHECK(receipt->work == (input->resource == (uintptr_t)objects[0] ? 17u : 29u));
    CHECK(returned == (probe_mode != 2 && probe_mode != 8 && probe_mode != 9)); CHECK(ordered == (probe_mode != 4));
    if (probe_mode == 6) CHECK(input->resource == (uintptr_t)objects[probe_finishes == 1 ? 1 : 0]);
    receipt->state = 2;
    if (probe_mode == 3) RaiseException(0xe0800041u, 0, 0, NULL);
    return returned && ordered && receipt->complete;
}
static int probe_post(void *resource) { (void)resource; probe_post_calls++; return 1; }
static void probe_native(void *resource)
{
    probe_native_calls++;
    if (probe_mode == 6 && resource == objects[0]) {
        rr_entry inner = {0}; inner.resource = objects[1]; inner.type = "probe"; inner.name = "inner";
        CHECK(!rr_default(&inner));
    }
    ((unsigned char *)resource)[0x68] = 0; /* native destroys the input before post-observation */
    if (probe_mode == 2) RaiseException(0xe0800042u, 0, 0, NULL);
}
static int probe_invoke(void *resource, void *actual)
{
    probe_invokes++; CHECK(actual == probe_native && probe_prepares == 1 && !probe_finishes);
    CHECK(sh_resource_graph_recording_requests() && scope_depth == 1 && ((unsigned char *)resource)[0x68] == 17);
    if (probe_mode == 7) { ((void (*)(void *))actual)(resource); return 1; }
    ((unsigned char *)resource)[0x68] = 0;
    if (probe_mode == 9) RaiseException(0xe0800043u, 0, 0, NULL);
    return 0;
}
static void scoped_default_probe_cases(void)
{
    for (probe_mode = 0; probe_mode <= 9; probe_mode++) {
        reset(); sh_resource_graph_test_reset(); resource_table[0x50/8] = probe_native;
        probe_prepares = probe_finishes = probe_native_calls = probe_post_calls = 0;
        probe_invokes = 0;
        objects[0][0x68] = 17; objects[1][0x68] = 29;
        sh_resource_resident_set_default_observation(probe_post);
        sh_resource_resident_set_default_probe(probe_prepare, probe_finish);
        if (probe_mode >= 7) sh_resource_resident_set_default_invoke(probe_invoke);
        if (probe_mode == 4) leave_misorders = 1;
        rr_entry entry = {0}; entry.resource = objects[0]; entry.type = "probe"; entry.name = "outer";
        const char *failure = rr_default(&entry);
        CHECK((failure != NULL) == (probe_mode == 2 || probe_mode == 4 || probe_mode == 8 || probe_mode == 9));
        CHECK(probe_prepares == (probe_mode == 6 ? 2 : 1) && probe_finishes == probe_prepares &&
            probe_native_calls == (probe_mode >= 8 ? 0 : probe_prepares) && !probe_post_calls && !scope_depth);
        CHECK(probe_invokes == (probe_mode >= 7));
        size_t nodes, complete, edges; sh_resource_graph_counts(&nodes, &complete, &edges);
        CHECK(nodes == (probe_mode == 6 ? 2u : 1u));
        CHECK(complete == (probe_mode == 6 ? 2u : probe_mode == 0 || probe_mode == 7 ? 1u : 0u));
    }
    reset(); sh_resource_graph_test_reset(); resource_table[0x50/8] = probe_native; probe_mode = 0;
    probe_prepares = probe_finishes = probe_native_calls = probe_post_calls = 0;
    sh_resource_resident_set_default_observation(probe_post);
    /* An incomplete callback pair clears the pair and preserves old setter
     * compatibility; it cannot leave a stale prepare-only singleton. */
    sh_resource_resident_set_default_probe(probe_prepare, NULL);
    rr_entry entry = {0}; entry.resource = objects[0]; entry.type = "probe"; entry.name = "fallback";
    CHECK(!rr_default(&entry) && probe_native_calls == 1 && probe_post_calls == 1 && !probe_prepares && !probe_finishes);
    reset();
}
static int origin_mode,origin_begins,origin_finishes;
static int origin_begin(sh_resource_default_origin_scope *scope,void *resource)
{
    origin_begins++; CHECK(sh_resource_graph_recording_requests() && probe_prepares<origin_begins);
    scope->context=1; scope->serial=resource==objects[0] ? 11 : 12;
    if(origin_mode==2) RaiseException(0xe0800044u,0,0,NULL);
    return origin_mode!=1;
}
static int origin_finish(sh_resource_default_origin_scope *scope,void *resource,int returned,int ordered,int complete)
{
    origin_finishes++; CHECK(probe_finishes==origin_finishes && sh_resource_graph_recording_requests());
    CHECK(scope->serial==(resource==objects[0] ? 11u : 12u));
    CHECK(returned && complete && ordered==(origin_mode!=1 && origin_mode!=2));
    if(origin_mode==3) RaiseException(0xe0800045u,0,0,NULL);
    scope->serial=0; return returned && ordered && complete;
}
static int resident_refresh_mode,resident_refresh_calls;
static int resident_refresh(void *const *resources,size_t count,char *error,size_t capacity)
{
    resident_refresh_calls++;
    CHECK(count==2 && resources && loads==2 && !updates && rebinds==1 && *(int *)(renderer_object+0x14)==1);
    CHECK((resources[0]==objects[0] && resources[1]==objects[1]) || (resources[1]==objects[0] && resources[0]==objects[1]));
    CHECK(*(int *)(heap_object+0xc4)>0);
    if(resident_refresh_mode==2) RaiseException(0xe0800046u,0,0,NULL);
    if(resident_refresh_mode && error && capacity) snprintf(error,capacity,"synthetic holder refresh failure");
    return !resident_refresh_mode;
}
static int admit_calls,admit_fail,admit_throw,admit_any_count;
static int resident_admit(void *const *resources,size_t count,char *error,size_t capacity)
{
    CHECK(resources && (count==2 || (admit_any_count && count==1)) && *(int *)(renderer_object+0x14)==1 && *(int *)(heap_object+0xc4)>0);
    admit_calls++;
    if(admit_calls!=admit_fail) return 1;
    if(admit_throw) RaiseException(0xe0800047u,0,0,NULL);
    if(error && capacity) snprintf(error,capacity,"synthetic unresolved holder"); return 0;
}
/* The report an admission leaves behind: resident_report_input is named for isolation on the first
 * admission only; later admissions report nothing. */
static void *resident_report_input; static int resident_report_tolerated, report_calls;
static sh_resource_admit_report resident_report_item;
static size_t resident_report(const sh_resource_admit_report **items)
{
    report_calls++;
    if(!resident_report_input || report_calls>1) { *items=NULL; return 0; }
    resident_report_item=(sh_resource_admit_report){(void *)0x1234,0x253f068u,"cloth",0x40,resident_report_input,0,resident_report_tolerated};
    *items=&resident_report_item; return 1;
}
static void isolation_cases(void)
{
    char error[256];
    /* Admission refuses once naming skin for isolation: skin is left as it is, body.lwo and the rest of
     * the pass proceed, the second admission (without skin) is accepted, and the pass succeeds. */
    reset(); admit_calls=0; admit_fail=1; admit_throw=0; report_calls=0; g_log[0]=0; admit_any_count=1; expect_reconstructed=1;
    resident_report_input=objects[1]; resident_report_tolerated=0;
    sh_resource_resident_set_instance_admit(resident_admit); sh_resource_resident_set_instance_report(resident_report);
    sh_resource_resident *pass=begin(paths[0],0,error); CHECK(pass);
    if(pass) {
        CHECK(sh_resource_resident_reconstruct(pass,error,sizeof(error)));
        CHECK(admit_calls==2 && report_calls==2);
        CHECK(sh_resource_resident_isolated_count(pass)==1 && sh_resource_resident_isolated(pass,objects[1]) &&
            !sh_resource_resident_isolated(pass,objects[0]));
        CHECK(rebuilds==1 && counts[0]==1 && counts[1]==0);
        CHECK(strstr(g_log,"native resident isolates model:skin: cloth holder (vtable +0x253f068) reads it through field +0x40") != NULL);
        CHECK(sh_resource_resident_drain(pass,error,sizeof(error)));
        CHECK(sh_resource_resident_end(pass,1,error,sizeof(error)));
        CHECK(counts[1]==0 && !(objects[1][0x2c] & RR_PENDING));
    }
    /* A tolerated report isolates nothing and never changes the admission's answer. */
    reset(); admit_calls=0; admit_fail=0; admit_throw=0; report_calls=0; g_log[0]=0;
    resident_report_input=objects[1]; resident_report_tolerated=1;
    sh_resource_resident_set_instance_admit(resident_admit); sh_resource_resident_set_instance_report(resident_report);
    pass=begin(paths[0],0,error); CHECK(pass);
    if(pass) {
        CHECK(sh_resource_resident_reconstruct(pass,error,sizeof(error)) && admit_calls==1);
        CHECK(sh_resource_resident_isolated_count(pass)==0 && rebuilds==2);
        CHECK(strstr(g_log,"tolerated: the holder is rebuilt from its inputs after the mutation") != NULL);
        CHECK(sh_resource_resident_drain(pass,error,sizeof(error)) && sh_resource_resident_end(pass,1,error,sizeof(error)));
    }
    /* A refusal that names an input outside the pass cannot isolate anything and fails as before. */
    reset(); admit_calls=0; admit_fail=1; admit_throw=0; report_calls=0;
    resident_report_input=error; resident_report_tolerated=0;
    sh_resource_resident_set_instance_admit(resident_admit); sh_resource_resident_set_instance_report(resident_report);
    pass=begin(paths[0],0,error); CHECK(pass);
    if(pass) {
        CHECK(!sh_resource_resident_reconstruct(pass,error,sizeof(error)) && admit_calls==1 && !rebuilds);
        CHECK(sh_resource_resident_isolated_count(pass)==0 && strstr(error,"unresolved holder"));
        CHECK(!sh_resource_resident_end(pass,0,error,sizeof(error))); check_restored();
    }
    resident_report_input=NULL; admit_any_count=0; sh_resource_resident_set_instance_report(NULL); reset();
}
static void admission_cases(void)
{
    for(int stage=1;stage<=3;stage++) for(admit_throw=0;admit_throw<2;admit_throw++) {
        reset(); admit_calls=0; admit_fail=stage;
        sh_resource_resident_set_instance_admit(resident_admit);
        char error[256]; sh_resource_resident *pass=begin(paths[0],0,error); CHECK(pass); if(!pass) continue;
        int rebuilt=sh_resource_resident_reconstruct(pass,error,sizeof(error));
        CHECK(rebuilt==(stage!=1));
        if(stage==1) CHECK(!pass->mutated && !rebuilds && !defaults && !loads && !rebinds);
        else {
            CHECK(!sh_resource_resident_drain(pass,error,sizeof(error)));
            CHECK(rebuilds==2 && !defaults && !loads && !rebinds && !updates);
        }
        CHECK(admit_calls==stage && strstr(error,admit_throw?"raised an engine error":"unresolved holder"));
        CHECK(!sh_resource_resident_end(pass,0,error,sizeof(error))); check_restored();
    }
    /* A later leaf load rechecks admission before its pending bit or bytes
     * change, even when an earlier reconstruction was admitted. */
    reset(); admit_calls=0; admit_fail=2; admit_throw=0; sh_resource_resident_set_instance_admit(resident_admit);
    char error[256]; sh_resource_resident *pass=begin(paths[0],0,error); CHECK(pass);
    if(pass) { CHECK(sh_resource_resident_reconstruct(pass,error,sizeof(error)));
        CHECK(!sh_resource_resident_drain_leaves(pass,any_leaf,error,sizeof(error)) && !loads && !rebinds);
        CHECK(!sh_resource_resident_end(pass,0,error,sizeof(error))); check_restored(); }
    reset();
}
static void origin_and_holder_cases(void)
{
    for(origin_mode=0;origin_mode<5;origin_mode++) {
        reset(); sh_resource_graph_test_reset(); resource_table[0x50/8]=probe_native;
        probe_mode=origin_mode==4 ? 6 : 0;
        probe_prepares=probe_finishes=probe_native_calls=probe_post_calls=origin_begins=origin_finishes=0;
        objects[0][0x68]=17; objects[1][0x68]=29;
        sh_resource_resident_set_default_probe(probe_prepare,probe_finish);
        sh_resource_resident_set_default_origins(origin_begin,origin_finish);
        rr_entry entry={0}; entry.resource=objects[0]; entry.type="probe"; entry.name="origin-outer";
        CHECK(!rr_default(&entry));
        CHECK(origin_begins==(origin_mode==4 ? 2 : 1) && origin_finishes==origin_begins &&
            probe_native_calls==origin_begins && probe_finishes==origin_begins && !scope_depth);
        size_t nodes,complete,edges; sh_resource_graph_counts(&nodes,&complete,&edges);
        CHECK(complete==(origin_mode==4 ? 2u : origin_mode==0 ? 1u : 0u));
    }
    for(resident_refresh_mode=0;resident_refresh_mode<3;resident_refresh_mode++) {
        reset(); resident_refresh_calls=0;
        sh_resource_resident_set_instance_refresh(resident_refresh);
        char error[256]; sh_resource_resident *pass=begin(paths[0],0,error);
        CHECK(pass && pass->count==2); if(!pass) continue;
        CHECK(sh_resource_resident_reconstruct(pass,error,sizeof(error)));
        CHECK(sh_resource_resident_drain(pass,error,sizeof(error))==!resident_refresh_mode);
        CHECK(resident_refresh_calls==1 && updates==!resident_refresh_mode);
        if(resident_refresh_mode) CHECK(strstr(error,resident_refresh_mode==2 ? "raised an engine error" : "holder refresh"));
        CHECK(sh_resource_resident_end(pass,!resident_refresh_mode,error,sizeof(error))==!resident_refresh_mode);
        check_restored();
    }
    reset();
}
typedef struct transfer_snapshot { unsigned char original; } transfer_snapshot;
static int transfer_prepares,transfer_finishes,transfer_disposes,transfer_prior;
static int transfer_prepare_mode,transfer_finish_mode;
static unsigned char transfer_current;
static int transfer_prepare(void *const *resources,size_t count,const void *previous,
    void **out,char *error,size_t capacity)
{
    CHECK(resources && count==2 && sh_resource_resident_owned(g_rr_active));
    CHECK(!g_rr_active->mutated && !g_rr_active->rebuilt);
    transfer_prepares++;
    transfer_snapshot *snapshot=malloc(sizeof(*snapshot)); CHECK(snapshot); if(!snapshot) return 0;
    snapshot->original=previous?((const transfer_snapshot *)previous)->original:objects[0][0x7f0];
    if(previous) { transfer_prior++; CHECK(snapshot->original==0x71 && previous!=snapshot); }
    *out=snapshot;
    if(transfer_prepare_mode==2) RaiseException(0xe0800048u,0,0,NULL);
    if(transfer_prepare_mode && error && capacity) snprintf(error,capacity,"synthetic transfer snapshot failure");
    return !transfer_prepare_mode;
}
static int transfer_finish(void *receipt,void *const *resources,size_t count,char *error,size_t capacity)
{
    transfer_snapshot *snapshot=receipt;
    CHECK(snapshot && snapshot->original==0x71 && objects[0][0x7f0]==transfer_current);
    CHECK(resources && count==2 && loads>=2 && rebinds && !updates && sh_resource_resident_owned(g_rr_active));
    transfer_finishes++;
    if(transfer_finish_mode==2) RaiseException(0xe0800049u,0,0,NULL);
    if(transfer_finish_mode && error && capacity) snprintf(error,capacity,"synthetic transfer verification failure");
    return !transfer_finish_mode;
}
static void transfer_dispose(void *receipt)
{
    CHECK(receipt); transfer_disposes++; free(receipt);
}
static void transfer_cases(void)
{
    char error[256];
    for(int mode=0;mode<3;mode++) {
        reset(); objects[0][0x7f0]=0x71;
        transfer_prepares=transfer_finishes=transfer_disposes=transfer_prior=0;
        transfer_prepare_mode=mode; transfer_finish_mode=0;
        sh_resource_resident_set_instance_transfer(transfer_prepare,transfer_finish,transfer_dispose);
        sh_resource_resident *pass=begin(paths[0],0,error);
        CHECK((pass!=NULL)==!mode && transfer_prepares==1 && !transfer_finishes && !rebuilds && !loads);
        if(mode) {
            CHECK(transfer_disposes==1 && !g_rr_recovery);
            CHECK(strstr(error,mode==2?"prepare raised":"snapshot failure")); check_restored(); continue;
        }
        objects[0][0x7f0]=transfer_current=0x83;
        CHECK(sh_resource_resident_reconstruct(pass,error,sizeof(error)));
        CHECK(sh_resource_resident_drain(pass,error,sizeof(error)) && transfer_finishes==1 && !transfer_disposes);
        CHECK(sh_resource_resident_drain(pass,error,sizeof(error)) && transfer_finishes==1);
        CHECK(sh_resource_resident_end(pass,1,error,sizeof(error)) && transfer_disposes==1); check_restored();
    }
    for(int mode=1;mode<=3;mode++) {
        reset(); objects[0][0x7f0]=0x71;
        transfer_prepares=transfer_finishes=transfer_disposes=transfer_prior=0;
        transfer_prepare_mode=0; transfer_finish_mode=mode<3?mode:0;
        sh_resource_resident_set_instance_transfer(transfer_prepare,transfer_finish,transfer_dispose);
        sh_resource_resident *pass=begin(paths[0],0,error); CHECK(pass); if(!pass) continue;
        objects[0][0x7f0]=transfer_current=0x83;
        CHECK(sh_resource_resident_reconstruct(pass,error,sizeof(error)));
        if(mode==3) fault=5; /* Failure after an already verified transfer. */
        CHECK(!sh_resource_resident_drain(pass,error,sizeof(error)) && transfer_finishes==1);
        CHECK(!sh_resource_resident_end(pass,0,error,sizeof(error)) && !transfer_disposes && g_rr_recovery==pass);
        check_restored();
        objects[0][0x7f0]=0x99; /* Recovery must not recapture this changed layout as the original. */
        transfer_prepare_mode=1;
        CHECK(!begin(paths[0],1,error) && transfer_prior==1 && transfer_disposes==1 && g_rr_recovery==pass);
        transfer_prepare_mode=0;
        sh_resource_resident_set_instance_transfer(NULL,NULL,NULL);
        CHECK(!begin(paths[0],1,error) && strstr(error,"implementation changed") && g_rr_recovery==pass && transfer_prepares==2);
        sh_resource_resident_set_instance_transfer(transfer_prepare,transfer_finish,transfer_dispose);
        sh_resource_resident *recovery=begin(paths[0],1,error);
        CHECK(recovery && transfer_prior==2 && transfer_disposes==1); if(!recovery) continue;
        objects[0][0x7f0]=transfer_current=0xa4; transfer_finish_mode=0; fault=0; updates=0;
        CHECK(sh_resource_resident_reconstruct(recovery,error,sizeof(error)));
        CHECK(sh_resource_resident_drain(recovery,error,sizeof(error)) && transfer_finishes==2 && transfer_disposes==1);
        CHECK(sh_resource_resident_end(recovery,1,error,sizeof(error)) && transfer_disposes==3 && !g_rr_recovery);
        check_restored();
    }
    reset();
}
int main(void)
{
    success_cases(); world_text_cases(); initial_publication_cases(); failure_cases(); atlas_failure_cases(); scope_failure_cases(); scope_ownership_cases();
    additive_finalization_cases(); fixed_renderer_cases(); leaf_cases(); revival_cases(); availability_cases();
    default_scope_cases(); owner_cases(); dependency_cases(); observation_cases(); additive_refresh_cases(); scoped_default_probe_cases(); origin_and_holder_cases(); admission_cases(); isolation_cases(); transfer_cases();
    rr_free(g_rr_recovery); g_rr_recovery = NULL;
    CHECK(!scope_depth);
    sh_resource_graph_test_reset();
    printf("resident refresh: %d failure(s)\n", failures); return failures ? 1 : 0;
}
