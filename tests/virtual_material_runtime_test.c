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
    puts("virtual material runtime tests passed"); return 0;
}
