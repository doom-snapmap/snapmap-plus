/* Native-shaped synthetic memory exercises production lookup and ownership. */
#include "../src/backend/virtual_material_runtime.c"
#include <assert.h>

static unsigned char provider[0x458], owner[0x178], material[4][0x1b8];
static int calls, logs, prepared, committed;
static void *native_lookup(void *atlas, void *object)
{ (void)atlas; (void)object; calls++; return (void *)0x5678; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{
    assert(target == (void *)0x1234 && detour == vmr_lookup_hook && stolen == 16);
    prepared++; return native_lookup;
}
sh_patch_status hook_commit(void *tramp)
{ assert(tramp == native_lookup); committed++; return B2_PATCH_OK; }
/* Only the virtual texture provider's install failure unpatches, and this suite binds the provider without installing it. */
int hook_unpatch(void *tramp) { (void)tramp; assert(0); return 0; }
void backend_log(const char *message) { assert(message && *message); logs++; }
static void pointer(void *body, size_t offset, const void *value)
{ memcpy((unsigned char *)body + offset, &value, sizeof(value)); }
static void integer(void *body, size_t offset, int32_t value)
{ memcpy((unsigned char *)body + offset, &value, 4); }
static void init_material(size_t i, const char *name)
{ memset(material[i], 0, sizeof(material[i])); pointer(material[i], 8, name); }
static void destroy_material(size_t i)
{
    void *row; memcpy(&row, material[i] + 0x170, sizeof(row));
    if (row) pointer(row, 0x30, NULL); /* Native destructor's verified write. */
    pointer(material[i], 0x170, NULL);
}
static void *parse_material(size_t i)
{
    void *row = vmr_lookup_hook(owner, material[i]);
    pointer(material[i], 0x170, row); /* Native Parse publishes the returned row. */
    return row;
}
static int keep_material(void *context, const char *name)
{ return context ? !strcmp(context, name) : -1; }

/* Private page providers against an engine-shaped provider table. */
enum { VT_STRIDE = 0x458, VT_PAGE = 0x199 };
static unsigned char *vt_system;
static void *vt_head;
static int vt_builds, vt_releases, vt_fail, vt_owner;
static const int vt_modes[3] = {0, 1, 2};
static const char *vt_empty = "";
static char vt_names[32][64], vt_paths[32][96];
static void vt_text(unsigned char *p, size_t at, const char *text) { memcpy(p + at, &text, sizeof(text)); }
static void vt_release(void *object)
{
    unsigned char *p = object; vt_releases++;
    vt_text(p, 0x70, vt_empty); vt_text(p, 0x1a0, vt_empty); memset(p, 0, 16); memset(p + 0x3d8, 0, 8);
}
static void vt_quiesce(void *system) { assert(system == vt_system); }
static unsigned char vt_set_source(void *object, const char *name, const char *path,
    int mode, int kind, unsigned char direct, void *reader)
{
    unsigned char *p = object; int slot = (int)((p - vt_system) / VT_STRIDE);
    const void *mode_object = vt_modes + mode; void *owner = &vt_owner;
    assert(mode == 2 && kind == 2 && !direct && !reader && slot >= 2 && slot <= 30);
    if (vt_fail) return 0;
    vt_builds++;
    snprintf(vt_names[slot], sizeof(vt_names[slot]), "%s", name);
    snprintf(vt_paths[slot], sizeof(vt_paths[slot]), "%s", path);
    vt_text(p, 0x1a0, vt_names[slot]); vt_text(p, 0x70, vt_paths[slot]);
    memcpy(p, &kind, 4); memcpy(p + 8, &owner, 8); memcpy(p + 0x3d8, &mode_object, 8);
    return 1;
}
static int vt_absent(void *context, const char *path, sh_package_file **file, char *error, size_t capacity)
{ (void)context; (void)path; (void)error; (void)capacity; *file = NULL; return 0; }
static void vt_put(unsigned char *p, uint64_t x, unsigned bytes)
{ for (unsigned i = 0; i < bytes; i++) p[i] = (unsigned char)(x >> (i * 8)); }
static void private_providers(void)
{
    static unsigned char mega2[VT_PAGE];
    static const char atlas[] = "smp/vt/0123456789abcdef01234567";
    static const char page_path[] = "virtualtextures/smp/vt/0123456789abcdef01234567_sq1.mega2";
    const char *paths[21] = {page_path};
    sh_compiled_resource member = {0}; sh_package_compilation scope = {0}; sh_package_sources inventory = {0};
    sh_virtual_material_view *view, *previous = NULL; char error[512];
    unsigned char **table;
    /* A minimal valid mega2 member supplies the whole captured family. */
    vt_put(mega2, 0xa63fbb21u, 4); vt_put(mega2 + 4, 2, 4); vt_put(mega2 + 0xc, 12, 4);
    vt_put(mega2 + 0x38, 0x170, 8); vt_put(mega2 + 0x40, 0x180, 8); vt_put(mega2 + 0x48, 1, 4);
    vt_put(mega2 + 0x4c, 2, 4); vt_put(mega2 + 0x58, 2, 4); vt_put(mega2 + 0x5c, 1, 4); vt_put(mega2 + 0x64, 2, 4);
    vt_put(mega2 + 0x170, 0x188, 8); vt_put(mega2 + 0x178, 17, 8); vt_put(mega2 + 0x184, UINT32_MAX, 4);
    memset(mega2 + 0x188, 0x5a, 17);
    member.engine_path = "virtualtextures/_vmtr_sq1.mega2"; member.body = mega2; member.body_length = VT_PAGE;
    member.source = SIZE_MAX; scope.resources = &member; scope.resource_count = 1; scope.sources = &inventory;
    sh_virtual_page_source *pages = sh_virtual_page_source_open(&scope, "virtualtextures/_vmtr", vt_absent, NULL,
        error, sizeof(error));
    assert(pages);
    sh_virtual_material_runtime_binding binding = {"private/stone", atlas, NULL, 0, pages, 2, 2, paths};
    view = sh_virtual_material_runtime_prepare(&binding, 1, error, sizeof(error)); assert(view);
    sh_virtual_page_source_release(pages);
    /* Publication needs the provider layer before any private material parses. */
    assert(!sh_virtual_material_runtime_publish(view, &previous, error, sizeof(error)) && strstr(error, "page providers"));
    vt_system = calloc(1, 32 * VT_STRIDE + 32 * sizeof(void *)); assert(vt_system);
    table = (unsigned char **)(vt_system + 32 * VT_STRIDE);
    for (int slot = 0; slot < 32; slot++) {
        table[slot] = vt_system + slot * VT_STRIDE; memcpy(table[slot] + 0x2c0, &slot, 4);
        vt_text(table[slot], 0x70, vt_empty); vt_text(table[slot], 0x1a0, vt_empty);
    }
    {
        const void *global_mode = vt_modes; void *owner = &vt_owner; int kind = 2;
        vt_text(table[1], 0x1a0, "_vmtr"); vt_text(table[1], 0x70, "virtualtextures/_vmtr.pages");
        memcpy(table[1], &kind, 4); memcpy(table[1] + 8, &owner, 8); memcpy(table[1] + 0x3d8, &global_mode, 8);
    }
    sh_vt_native native = {vt_system, VT_STRIDE, vt_set_source, vt_release, vt_quiesce, NULL, NULL, &vt_head};
    assert(sh_vt_provider_bind(&native));
    assert(sh_virtual_material_runtime_publish(view, &previous, error, sizeof(error)) && !previous);
    /* The active view's private atlas is built once, then re-verified. */
    assert(sh_virtual_material_runtime_providers(error, sizeof(error)) && vt_builds == 1);
    assert(!strcmp(vt_names[2], atlas) && !strcmp(vt_paths[2], "virtualtextures/smp/vt/0123456789abcdef01234567.pages"));
    assert(sh_virtual_material_runtime_providers(error, sizeof(error)) && vt_builds == 1);
    sh_virtual_material_runtime_collect(); assert(!vt_releases); /* The active view keeps it. */
    /* Retiring the view lets collect release its provider. */
    assert(sh_virtual_material_runtime_publish(NULL, &previous, error, sizeof(error)) && previous == view);
    assert(sh_virtual_material_runtime_retire(&previous) && !previous);
    sh_virtual_material_runtime_collect(); assert(vt_releases == 1 && !g_vmr_views && !*vt_names[2] == 0);
    assert(!strcmp(*(const char **)(table[2] + 0x1a0), ""));
    /* A provider that cannot be built faults the active view's status. */
    pages = sh_virtual_page_source_open(&scope, "virtualtextures/_vmtr", vt_absent, NULL, error, sizeof(error)); assert(pages);
    binding.pages = pages;
    view = sh_virtual_material_runtime_prepare(&binding, 1, error, sizeof(error)); assert(view);
    sh_virtual_page_source_release(pages);
    assert(sh_virtual_material_runtime_publish(view, &previous, error, sizeof(error)));
    vt_fail = 1;
    assert(!sh_virtual_material_runtime_providers(error, sizeof(error)) && strstr(error, "construction failed"));
    assert(!sh_virtual_material_runtime_status(error, sizeof(error)) && strstr(error, "construction failed"));
    vt_fail = 0;
    assert(sh_virtual_material_runtime_publish(NULL, &previous, error, sizeof(error)) && previous == view);
    assert(sh_virtual_material_runtime_retire(&previous));
    sh_virtual_material_runtime_collect(); assert(!g_vmr_views);
    free(vt_system);
}
int main(void)
{
    sh_virtual_material_record source = {"surfaces/source", 16, 32, 64, 32, 2, 3, -4}, changed = source;
    sh_virtual_material_runtime_binding bindings[] = {
        {"variants/red", "_vmtr", &source, 0},
        {"variants/green", "_vmtr", &source, 0},
        {"VARIANTS\\RED", "_VMTR", &source, 0}
    };
    sh_virtual_material_view *a, *b, *bad, *previous = (void *)0x1111, *retained;
    void *red, *green, *temporary; char error[512];
    sig_result signature = {"VirtualMaterialLookup", SIG_OK, 0x1234, 0};
    pointer(owner, 0x130, provider); pointer(provider, 0x1a0, "_VMTR"); integer(provider, 0x2fc, 256);
    a = sh_virtual_material_runtime_prepare(bindings, 3, error, sizeof(error));
    assert(a && a->count == 2 && !*error);
    assert(!sh_virtual_material_runtime_reserve(a, error, sizeof(error)) && !g_vmr_views && !g_vmr_active);
    assert(!sh_virtual_material_runtime_publish(a, &previous, error, sizeof(error)));
    assert(previous == (void *)0x1111 && !g_vmr_active);
    assert(sh_virtual_material_runtime_install(&signature, 1) && prepared == 1 && committed == 1);
    assert(sh_virtual_material_runtime_install(&signature, 1) && prepared == 1);
    assert(sh_virtual_material_runtime_publish(a, &previous, error, sizeof(error)) && !previous);
    assert(!sh_virtual_material_runtime_release(&a) && a);
    init_material(0, "Variants/Red"); init_material(1, "variants/green");
    init_material(2, "variants/red"); init_material(3, "unrelated");
    red = parse_material(0); green = parse_material(1); temporary = parse_material(2);
    assert(red && green && temporary && red != green && red != temporary && green != temporary && !calls);
    assert(!memcmp((char *)red + 0x38, (char *)green + 0x38, 16));
    assert(parse_material(0) == red && parse_material(1) == green);
    assert(vmr_lookup_hook(owner, material[3]) == (void *)0x5678 && calls == 1);
    pointer(material[3], 8, "native\x80name");
    assert(vmr_lookup_hook(owner, material[3]) == (void *)0x5678 && calls == 2);
    assert(sh_virtual_material_runtime_status(error, sizeof(error)) && !logs);

    /* Reservation validates and registers without exposing new lookup. An
     * unused prepublication fallback must be removable immediately. */
    b = sh_virtual_material_runtime_prepare(bindings, 3, error, sizeof(error)); assert(b);
    assert(sh_virtual_material_runtime_reserve(b, error, sizeof(error)) && g_vmr_active == a);
    assert(b->published && !b->retired && g_vmr_views == b && parse_material(0) == red);
    assert(sh_virtual_material_runtime_release(&b) && !b && g_vmr_active == a && g_vmr_views == a);
    b = sh_virtual_material_runtime_prepare(bindings, 3, error, sizeof(error)); assert(b);
    assert(sh_virtual_material_runtime_reserve(b, error, sizeof(error)) && g_vmr_active == a);
    sh_virtual_material_runtime_restore(b);
    assert(g_vmr_active == b && parse_material(0) == red);
    sh_virtual_material_runtime_restore(a);
    assert(sh_virtual_material_runtime_release(&b) && !b && g_vmr_active == a);

    /* Same meanings in a new snapshot borrow existing rows and preserve rollback. */
    b = sh_virtual_material_runtime_prepare(bindings, 3, error, sizeof(error)); assert(b);
    assert(sh_virtual_material_runtime_publish(b, &previous, error, sizeof(error)) && previous == a);
    assert(parse_material(0) == red && parse_material(1) == green);
    retained = a;
    assert(!sh_virtual_material_runtime_release(&a) && a == retained);
    assert(sh_virtual_material_runtime_publish(previous, &previous, error, sizeof(error)) && previous == b);
    assert(sh_virtual_material_runtime_release(&b) && !b);
    assert(parse_material(0) == red);

    /* Different meaning under one alias is rejected before publication. */
    changed.x++; bindings[0].record = &changed;
    assert(!sh_virtual_material_runtime_prepare(bindings, 3, error, sizeof(error)) && *error);
    bad = sh_virtual_material_runtime_prepare(bindings, 1, error, sizeof(error)); assert(bad);
    assert(!sh_virtual_material_runtime_reserve(bad, error, sizeof(error)) && g_vmr_active == a && !bad->published);
    previous = (void *)0x2222;
    assert(!sh_virtual_material_runtime_publish(bad, &previous, error, sizeof(error)) && *error);
    assert(previous == (void *)0x2222 && g_vmr_active == a && parse_material(0) == red);
    assert(sh_virtual_material_runtime_release(&bad)); bindings[0].record = &source;

    /* Wrong atlas never calls the native fallback; its candidate remains failed. */
    b = sh_virtual_material_runtime_prepare(bindings, 3, error, sizeof(error)); assert(b);
    assert(sh_virtual_material_runtime_publish(b, &previous, error, sizeof(error)) && previous == a);
    pointer(provider, 0x1a0, "other_atlas");
    assert(!vmr_lookup_hook(owner, material[0]) && calls == 2 && logs == 1);
    assert(!sh_virtual_material_runtime_status(error, sizeof(error)) && strstr(error, "page provider"));
    pointer(provider, 0x1a0, "_vmtr");
    assert(parse_material(0) == red && !sh_virtual_material_runtime_status(error, sizeof(error)));
    assert(sh_virtual_material_runtime_publish(a, &previous, error, sizeof(error)) && previous == b);
    assert(sh_virtual_material_runtime_status(error, sizeof(error)));
    assert(sh_virtual_material_runtime_release(&b));

    /* Exception containment releases the lock and affects only its candidate. */
    b = sh_virtual_material_runtime_prepare(bindings, 3, error, sizeof(error)); assert(b);
    assert(sh_virtual_material_runtime_publish(b, &previous, error, sizeof(error)));
    assert(!vmr_lookup_hook((void *)1, material[0]) && calls == 2);
    assert(!sh_virtual_material_runtime_status(error, sizeof(error)) && strstr(error, "exception"));
    assert(sh_virtual_material_runtime_publish(a, &previous, error, sizeof(error)));
    assert(sh_virtual_material_runtime_release(&b));
    assert(sh_virtual_material_runtime_status(error, sizeof(error)));

    destroy_material(2); sh_virtual_material_runtime_collect();
    assert(parse_material(0) == red && parse_material(1) == green);
    assert(sh_virtual_material_runtime_publish(NULL, &previous, error, sizeof(error)) && previous == a);
    assert(!sh_virtual_material_runtime_release(&a) && a);
    destroy_material(0); destroy_material(1);
    assert(sh_virtual_material_runtime_release(&a) && !a && !g_vmr_views);
    assert(sh_virtual_material_runtime_status(error, sizeof(error)));
    assert(vmr_lookup_hook(NULL, NULL) == (void *)0x5678 && calls == 3);

    a = sh_virtual_material_runtime_prepare(bindings, 3, error, sizeof(error)); assert(a);
    b = sh_virtual_material_runtime_join(a, a, error, sizeof(error)); assert(b && b->count == 2);
    bad = sh_virtual_material_runtime_filter(a, keep_material, "variants/red", error, sizeof(error));
    assert(bad && bad->count == 1 && sh_virtual_material_runtime_binding_equal(a, bad, "variants/red"));
    assert(!sh_virtual_material_runtime_binding_equal(a, bad, "variants/green"));
    assert(sh_virtual_material_runtime_release(&bad));
    assert(!sh_virtual_material_runtime_filter(a, keep_material, NULL, error, sizeof(error)) && *error);
    assert(a->count == 2 && b->count == 2);
    assert(sh_virtual_material_runtime_binding_equal(a, b, "variants/red"));
    assert(!sh_virtual_material_runtime_binding_equal(a, NULL, "variants/red"));
    assert(sh_virtual_material_runtime_publish(a, &previous, error, sizeof(error)));
    assert(!sh_virtual_material_runtime_retire(&a) && a);
    init_material(0, "variants/red"); red = parse_material(0); assert(red);
    assert(sh_virtual_material_runtime_publish(b, &previous, error, sizeof(error)) && previous == a);
    assert(sh_virtual_material_runtime_retire(&a) && !a);
    sh_virtual_material_runtime_collect(); assert(parse_material(0) == red);
    assert(sh_virtual_material_runtime_publish(NULL, &previous, error, sizeof(error)) && previous == b);
    assert(sh_virtual_material_runtime_retire(&b) && !b);
    sh_virtual_material_runtime_collect(); assert(g_vmr_views); /* Retired row remains owned. */
    destroy_material(0); sh_virtual_material_runtime_collect(); assert(!g_vmr_views);
    a = sh_virtual_material_runtime_prepare(NULL, 0, error, sizeof(error)); assert(a && !a->count);
    assert(sh_virtual_material_runtime_release(&a));
    assert(!sh_virtual_material_runtime_prepare(NULL, 1, error, sizeof(error)));
    assert(!sh_virtual_material_runtime_prepare(bindings, SIZE_MAX, error, sizeof(error)));
    changed.x = INT32_MAX; bindings[0].record = &changed;
    assert(!sh_virtual_material_runtime_prepare(bindings, 1, error, sizeof(error)));
    assert(!strcmp(source.name, "surfaces/source") && source.x == 16);
    private_providers();
    puts("virtual material runtime tests passed"); return 0;
}
