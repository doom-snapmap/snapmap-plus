/* Native-shaped VT system: the engine constructor's table and slot indices,
 * SetSource/release contracts and a live material manager list. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "virtual_texture_provider.h"
#include "hook.h"

void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *tramp) { (void)tramp; assert(0); return B2_PATCH_OK; }
static int logs;
void backend_log(const char *message) { assert(message && *message); logs++; }

enum { STRIDE = 0x458 };
static unsigned char *system_object;
static const int mode_objects[5] = {0, 1, 2, 3, 4};
static int owner_object;
static char names[32][64], paths[32][96];
static int constructs, releases, quiesces, fail_at, fail_partial, last_mode, last_kind, order_ok = 1;
static const char *empty = "";
static uint64_t sparse_function; static int32_t sparse_enabled;

static unsigned char *provider(int slot) { return system_object + (size_t)slot * STRIDE; }
static int slot_of(void *object) { return (int)(((unsigned char *)object - system_object) / STRIDE); }
static void set_text(unsigned char *object, size_t offset, const char *text)
{ memcpy(object + offset, &text, sizeof(text)); }
static const char *text(int slot, size_t offset)
{ const char *value; memcpy(&value, provider(slot) + offset, sizeof(value)); return value; }
static void native_release(void *object)
{
    unsigned char *p = object;
    if (quiesces <= releases) order_ok = 0; /* Every release follows a quiesce. */
    releases++;
    set_text(p, 0x70, empty); set_text(p, 0x1a0, empty);
    memset(p, 0, 16); memset(p + 0x3d8, 0, 8);
}
static void native_quiesce(void *object) { assert(object == system_object); quiesces++; }
static unsigned char native_set_source(void *object, const char *name, const char *path,
    int mode, int kind, unsigned char direct, void *reader)
{
    unsigned char *p = object; int slot = slot_of(object); const void *mode_object = mode_objects + mode;
    void *owner = kind == 2 ? &owner_object : NULL;
    /* The engine's SetSource quiesces and releases its target first. */
    quiesces++; native_release(object);
    assert(!direct && !reader && slot > 0 && slot < 32);
    constructs++; last_mode = mode; last_kind = kind;
    snprintf(names[slot], sizeof(names[slot]), "%s", name); snprintf(paths[slot], sizeof(paths[slot]), "%s", path);
    set_text(p, 0x1a0, names[slot]); set_text(p, 0x70, paths[slot]);
    if (fail_at && constructs == fail_at) {
        /* A header failure leaves the name and path set; an open failure
         * clears them. Either way SetSource reports failure. */
        if (!fail_partial) { set_text(p, 0x70, empty); set_text(p, 0x1a0, empty); }
        return 0;
    }
    memcpy(p, &kind, 4); memcpy(p + 8, &owner, 8); memcpy(p + 0x3d8, &mode_object, 8);
    return 1;
}

/* Material manager: +8 type, +0x18 next, +0x20 items, +0x28 count. Materials:
 * +0xd8 provider slot and +0xe0 provider name, cleared together by the engine. */
static unsigned char manager[0x30], other_manager[0x30], materials[3][0x100];
static void *items[3], *head;
static void material(int i, int slot, const char *name)
{ memcpy(materials[i] + 0xd8, &slot, 4); memcpy(materials[i] + 0xe0, &name, 8); }
static int keep_named;
static const char *keep_name;
static int keep(void *context, const char *name)
{ assert(context == &keep_named); return keep_name && !strcmp(keep_name, name); }

static void boot(void)
{
    unsigned char **table;
    free(system_object);
    system_object = calloc(1, 32 * STRIDE + 32 * sizeof(void *)); assert(system_object);
    table = (unsigned char **)(system_object + 32 * STRIDE);
    for (int slot = 0; slot < 32; slot++) {
        table[slot] = provider(slot); memcpy(provider(slot) + 0x2c0, &slot, 4);
        set_text(provider(slot), 0x70, empty); set_text(provider(slot), 0x1a0, empty);
    }
}
static void initialize_global(void)
{
    const void *mode0 = mode_objects; int kind = 2; void *owner = &owner_object;
    set_text(provider(1), 0x1a0, "_vmtr"); set_text(provider(1), 0x70, "virtualtextures/_vmtr.pages");
    memcpy(provider(1), &kind, 4); memcpy(provider(1) + 8, &owner, 8); memcpy(provider(1) + 0x3d8, &mode0, 8);
}
static sh_vt_native native(void)
{
    sh_vt_native n = {0};
    n.system = system_object; n.stride = STRIDE; n.set_source = native_set_source;
    n.release = native_release; n.quiesce = native_quiesce; n.resources = &head;
    return n;
}
int main(void)
{
    char error[512];
    const char *a = "smp/vt/000000000000000000000001", *b = "smp/vt/000000000000000000000002";
    const char *names_a[] = {a}, *names_ab[] = {a, b};
    assert(strlen(a) == 31);
    assert(sh_vt_provider_name_valid(a) && !sh_vt_provider_name_valid("smp/vt/0000000000000000000000001"));
    assert(!sh_vt_provider_name_valid("") && !sh_vt_provider_name_valid(NULL) && !sh_vt_provider_name_valid("private_vmtr"));
    assert(!sh_vt_provider_name_valid("smp/VT_VMTR") && !sh_vt_provider_name_valid("smp vt") &&
        !sh_vt_provider_name_valid("smp\\vt") && !sh_vt_provider_name_valid("smp\"vt"));

    boot();
    items[0] = materials[0]; items[1] = materials[1]; items[2] = NULL;
    {
        static const char *material_type = "material", *image_type = "image";
        void *next = other_manager;
        memcpy(manager + 8, &material_type, 8); memcpy(other_manager + 8, &image_type, 8);
        memcpy(manager + 0x18, &next, 8);
    }
    { void **list = items; int count = 3; memcpy(manager + 0x20, &list, 8); memcpy(manager + 0x28, &count, 4); }
    head = manager;

    assert(!sh_vt_provider_acquire(names_a, 1, error, sizeof(error)) && strstr(error, "unavailable"));
    sh_vt_native n = native(); assert(sh_vt_provider_bind(&n) && sh_vt_provider_ready());
    /* Before engine VT initialization builds _vmtr, construction waits for
     * the initialization hook instead of writing an unready table. */
    assert(!sh_vt_provider_initialized());
    assert(sh_vt_provider_acquire(names_a, 1, error, sizeof(error)) && !constructs);
    initialize_global(); assert(sh_vt_provider_initialized());

    /* Construction: slot 2, kind 2, mode 2, the private .pages path. */
    assert(sh_vt_provider_acquire(names_a, 1, error, sizeof(error)));
    assert(constructs == 1 && last_kind == 2 && last_mode == 2 && !strcmp(text(2, 0x1a0), a));
    assert(!strcmp(text(2, 0x70), "virtualtextures/smp/vt/000000000000000000000001.pages"));
    /* Reuse re-verifies the construction instead of rebuilding it. */
    assert(sh_vt_provider_acquire(names_a, 1, error, sizeof(error)) && constructs == 1);
    {
        int kind = 1; memcpy(provider(2), &kind, 4);
        assert(!sh_vt_provider_acquire(names_a, 1, error, sizeof(error)) && strstr(error, "no longer matches"));
        kind = 2; memcpy(provider(2), &kind, 4);
    }
    /* A native land provider keeps slot 3; the next private name takes slot 4. */
    set_text(provider(3), 0x1a0, "maps/land"); set_text(provider(3), 0x70, "virtualtextures/maps/land.pages");
    assert(sh_vt_provider_acquire(names_ab, 2, error, sizeof(error)) && constructs == 2 && !strcmp(text(4, 0x1a0), b));
    /* An engine provider already carrying a private name is not adopted. */
    {
        const char *c = "smp/vt/000000000000000000000003", *names_c[] = {c};
        set_text(provider(5), 0x1a0, c); set_text(provider(5), 0x70, "virtualtextures/smp/vt/x.pages");
        assert(!sh_vt_provider_acquire(names_c, 1, error, sizeof(error)) && strstr(error, "already uses"));
        set_text(provider(5), 0x1a0, empty); set_text(provider(5), 0x70, empty);
        /* OpenGL converts a nonzero mode to sparse mode 4 when both are set. */
        n.sparse_function = &sparse_function; n.sparse_enabled = &sparse_enabled;
        assert(sh_vt_provider_bind(&n));
        sparse_function = 0x1234; sparse_enabled = 0;
        assert(sh_vt_provider_acquire(names_c, 1, error, sizeof(error)) && last_mode == 2);
        sh_vt_provider_reinitialized(); assert(!strcmp(text(5, 0x1a0), ""));
        sparse_enabled = 1;
        assert(sh_vt_provider_acquire(names_c, 1, error, sizeof(error)) && last_mode == 4);
        sh_vt_provider_reinitialized(); sparse_function = 0;
        n.sparse_function = NULL; n.sparse_enabled = NULL; assert(sh_vt_provider_bind(&n));
    }
    /* Rebinding forgets ownership; the engine providers above were released. */
    set_text(provider(2), 0x1a0, empty); set_text(provider(2), 0x70, empty);
    set_text(provider(4), 0x1a0, empty); set_text(provider(4), 0x70, empty);
    /* Atomic: when the second construction of one call fails, the first
     * provider built by that call is released; earlier ownership remains. */
    {
        const char *d = "smp/vt/000000000000000000000004", *e = "smp/vt/000000000000000000000005";
        const char *pair[] = {a, e, d};
        assert(sh_vt_provider_acquire(names_a, 1, error, sizeof(error)) && !strcmp(text(2, 0x1a0), a));
        fail_at = constructs + 2; fail_partial = 1;
        assert(!sh_vt_provider_acquire(pair, 3, error, sizeof(error)) && strstr(error, "construction failed"));
        assert(!strcmp(text(2, 0x1a0), a) && !strcmp(text(4, 0x1a0), "") && !strcmp(text(5, 0x1a0), ""));
        fail_at = constructs + 1; fail_partial = 0;
        assert(!sh_vt_provider_acquire(pair, 3, error, sizeof(error)) && !strcmp(text(4, 0x1a0), ""));
        fail_at = 0;
        sh_vt_provider_reinitialized(); assert(!strcmp(text(2, 0x1a0), ""));
    }
    /* Private providers never take slot 31, which the selector releases on
     * every missing name; exhaustion fails explicitly instead. */
    {
        char many[29][32]; const char *list[29];
        for (int i = 0; i < 29; i++) { snprintf(many[i], sizeof(many[i]), "smp/vt/%024x", 0x100 + i); list[i] = many[i]; }
        assert(!strcmp(text(3, 0x1a0), "maps/land"));
        assert(!sh_vt_provider_acquire(list, 29, error, sizeof(error)) && strstr(error, "no empty page provider slot"));
        assert(!strcmp(text(31, 0x1a0), "") && !strcmp(text(30, 0x1a0), ""));
        assert(sh_vt_provider_acquire(list, 28, error, sizeof(error)) && !strcmp(text(30, 0x1a0), many[27]));
        assert(!strcmp(text(31, 0x1a0), ""));
        keep_name = NULL;
        assert(!sh_vt_provider_collect(keep, &keep_named));
        for (int slot = 2; slot <= 30; slot++) assert(slot == 3 || !strcmp(text(slot, 0x1a0), ""));
    }
    /* Retirement: a retained view keeps its provider; otherwise a parsed
     * material selecting the slot keeps it until the engine clears that
     * material; an unverifiable material list keeps everything. */
    assert(sh_vt_provider_acquire(names_ab, 2, error, sizeof(error)));
    keep_name = a; material(0, 4, NULL); material(1, 2, b);
    {
        int slot_b = !strcmp(text(2, 0x1a0), b) ? 2 : 4;
        material(1, slot_b, "smp/vt/whatever");
        assert(sh_vt_provider_collect(keep, &keep_named) == 2);
        { int count = -1; memcpy(manager + 0x28, &count, 4); }
        keep_name = NULL;
        assert(sh_vt_provider_collect(keep, &keep_named) == 2);
        { int count = 3; memcpy(manager + 0x28, &count, 4); }
        int before = releases;
        assert(sh_vt_provider_collect(keep, &keep_named) == 1 && releases == before + 1);
        material(1, slot_b, NULL);
        assert(sh_vt_provider_collect(keep, &keep_named) == 0 && releases == before + 2);
        assert(!strcmp(text(2, 0x1a0), "") && !strcmp(text(4, 0x1a0), ""));
    }
    /* Engine re-initialization: owned providers still carrying their names are
     * released, ownership is cleared and the wanted callback rebuilds them. */
    assert(sh_vt_provider_acquire(names_a, 1, error, sizeof(error)));
    sh_vt_provider_reinitialized();
    assert(!strcmp(text(2, 0x1a0), "") && sh_vt_provider_acquire(names_a, 1, error, sizeof(error)));
    assert(order_ok);
    sh_vt_provider_test_reset(); free(system_object);
    puts("virtual texture provider tests passed"); return 0;
}
