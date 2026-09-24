#include "../src/backend/virtual_material_runtime.c"
#include "package_virtual_material.h"
#include "package_native_names.h"
#include <assert.h>

void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *tramp) { (void)tramp; assert(0); return B2_PATCH_OK; }
void backend_log(const char *message) { (void)message; }

typedef struct originals { int reads, fail, product; } originals;
static int read_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    originals *f = context; const char *text = NULL;
    f->reads++; *body = NULL; *length = 0;
    if (f->fail) return -1;
    if (!strcmp(path, "virtualtextures/_vmtr.vmtr")) text = "26\n1\n8 4 16 16 0 1 2 \"surfaces/stone\"\n";
    if (!strcmp(path, "virtualtextures/_vmtr_dlc2.vmtr")) text = "26\n1\n64 4 16 16 0 1 2 \"surfaces/stone\"\n";
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); assert(*body);
    return f->product ? 3 : 1;
}
static int resolve(void *context, size_t unit, const char *family, const char *name,
    size_t *target, char *error, size_t capacity)
{ (void)context; (void)unit; (void)family; (void)name; (void)target; (void)error; (void)capacity; assert(0); return 0; }
static unsigned char *copy(void *context, size_t unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_link_reference reference,
    void *reference_context, size_t *output_length, char *error, size_t capacity)
{
    (void)context; (void)unit; (void)file; (void)reference; (void)reference_context; (void)error; (void)capacity;
    unsigned char *out = malloc(length + 1); assert(out);
    memcpy(out, body, length); out[length] = 0; *output_length = length; return out;
}
static sh_compiled_resource resource(const char *path, const char *body)
{
    sh_compiled_resource r = {0}; r.engine_path = (char *)path;
    r.body = (unsigned char *)body; r.body_length = strlen(body); r.source = SIZE_MAX; return r;
}
int main(void)
{
    char error[1024], *path; unsigned char semantics[3][32]; int original;
    path = sh_package_virtual_material_path("VirtualTextures\\maps.v1\\one.pages", 5);
    assert(path && !strcmp(path, "virtualtextures/maps.v1/one_dlc5.vmtr")); free(path);
    path = sh_package_virtual_material_path("virtualtextures/_vmtr", 0);
    assert(path && !strcmp(path, "virtualtextures/_vmtr.vmtr")); free(path);
    assert(!sh_package_virtual_material_path("../outside", 0));
    assert(!sh_package_virtual_material_path("virtualtextures/_vmtr", 6));
    originals f = {0};
    sh_package_sources inventory = {0}; sh_package_compilation scopes[3] = {0};
    const sh_package_compilation *views[] = {scopes, scopes + 1, scopes + 2};
    sh_compiled_resource files[3][2]; const sh_compiled_resource *unit_files[3];
    size_t scope_ids[] = {0, 1, 2}; sh_package_link_unit units[3] = {0};
    sh_package_link_inputs inputs = {units, scope_ids, 3};
    sh_package_virtual_atlas atlas = {"_vmtr", "virtualtextures/_vmtr.pages", {1}, 1};
    for (size_t i = 0; i < 3; i++) {
        files[i][0] = resource("generated/decls/material/surfaces/stone.decl", "{}");
        files[i][0].type = "material"; files[i][0].name = "surfaces/stone";
        scopes[i].resources = files[i]; scopes[i].resource_count = 1; scopes[i].sources = &inventory;
        unit_files[i] = files[i];
        units[i] = (sh_package_link_unit){"material", "surfaces/stone", scopes + i, unit_files + i, 1};
    }
    files[1][1] = resource("virtualtextures/_vmtr_dlc2.vmtr", "26\n1\n96 4 16 16 0 1 2 \"surfaces/stone\"\n");
    scopes[1].resource_count = 2;
    sh_package_link_adapter adapter = {&inputs, resolve, copy, sh_package_native_names_assign};
    sh_package_link_output linked = {0};
    sh_package_virtual_materials *m = sh_package_virtual_material_open(views, 3, read_original, &f); assert(m);
    for (size_t i = 0; i < 3; i++) {
        assert(sh_package_virtual_material_bind(m, i, units[i].name, &atlas, units[i].semantics,
            &original, error, sizeof(error)) == 1);
        assert(original == (i != 1));
        memcpy(semantics[i], units[i].semantics, 32);
    }
    assert(f.reads == 6 && !memcmp(semantics[0], semantics[2], 32) && memcmp(semantics[0], semantics[1], 32));
    f.fail = 1; /* Every original slot, including absence, is frozen. */
    assert(sh_package_virtual_material_bind(m, 0, units[0].name, &atlas, semantics[0], &original, error, sizeof(error)) == 1);
    assert(f.reads == 6);
    assert(sh_package_link(&inventory, units, 3, &adapter, NULL, NULL, &linked, error, sizeof(error)));
    assert(linked.classes.count == 2 && linked.compiled->resource_count == 2);
    sh_virtual_material_view *view = sh_package_virtual_material_prepare(m, &inputs, &linked, error, sizeof(error));
    assert(view && view->count == 2);
    const char *first = linked.names[linked.classes.nodes[0]], *second = linked.names[linked.classes.nodes[1]];
    assert(vmr_find(view, first)->record.x == 64 && vmr_find(view, first)->dlc == 2);
    assert(vmr_find(view, second)->record.x == 96 && !strcmp(vmr_find(view, second)->atlas, "_vmtr"));
    /* Caller cannot project a graph built with different implicit metadata. */
    units[0].semantics[0] ^= 1;
    assert(!sh_package_virtual_material_prepare(m, &inputs, &linked, error, sizeof(error)));
    units[0].semantics[0] ^= 1;
    sh_package_virtual_atlas changed = atlas; changed.content[0]++;
    assert(sh_package_virtual_material_bind(m, 0, units[0].name, &changed, semantics[0], &original, error, sizeof(error)) == -1);
    assert(strstr(error, "changed its resolved atlas"));
    sh_package_virtual_material_close(m);
    assert(vmr_find(view, first)->record.x == 64); /* Prepared view owns its rows. */
    assert(sh_virtual_material_runtime_release(&view)); sh_package_link_output_free(&linked);
    f.fail = 0; f.reads = 0;
    m = sh_package_virtual_material_open(views, 3, read_original, &f); assert(m);
    changed.original = 0;
    assert(sh_package_virtual_material_bind(m, 0, units[0].name, &atlas,
        semantics[0], &original, error, sizeof(error)) == 1);
    assert(sh_package_virtual_material_bind(m, 2, units[2].name, &changed,
        semantics[2], &original, error, sizeof(error)) == -1 && strstr(error, "different page data"));
    changed.name = "smp/atlas/other"; /* Resolved page-provider alias, not author metadata. */
    for (size_t i = 0; i < 3; i++) assert(sh_package_virtual_material_bind(m, i, units[i].name,
        i == 2 ? &changed : &atlas, units[i].semantics, &original, error, sizeof(error)) == 1);
    assert(!original); /* Equal rectangle, different page bytes: distinct asset. */
    assert(sh_package_link(&inventory, units, 3, &adapter, NULL, NULL, &linked, error, sizeof(error)));
    assert(linked.classes.count == 3 && linked.compiled->resource_count == 3);
    view = sh_package_virtual_material_prepare(m, &inputs, &linked, error, sizeof(error)); assert(view && view->count == 3);
    assert(sh_virtual_material_runtime_release(&view)); sh_package_link_output_free(&linked);
    sh_package_virtual_material_close(m);
    /* Malformed private table fails that candidate; it cannot mutate a peer
     * table or synthesize a valid original from a failed read. */
    files[1][1].body = (unsigned char *)"26\n2\n"; files[1][1].body_length = 5;
    m = sh_package_virtual_material_open(views, 3, read_original, &f); assert(m);
    assert(sh_package_virtual_material_bind(m, 1, units[1].name, &atlas, semantics[1], &original, error, sizeof(error)) == -1 && *error);
    assert(sh_package_virtual_material_bind(m, 0, units[0].name, &atlas, semantics[0], &original, error, sizeof(error)) == 1 && original);
    sh_package_virtual_material_close(m);
    f.fail = 1; f.reads = 0;
    m = sh_package_virtual_material_open(views, 3, read_original, &f); assert(m);
    assert(sh_package_virtual_material_bind(m, 0, "generic/surface", &atlas, semantics[0], &original, error, sizeof(error)) == 0 && original);
    assert(f.reads == 0);
    assert(sh_package_virtual_material_bind(m, 0, units[0].name, &atlas, semantics[0], &original, error, sizeof(error)) == -1 && !original);
    sh_package_virtual_material_close(m); f.fail = 0; f.product = 1;
    m = sh_package_virtual_material_open(views, 3, read_original, &f); assert(m);
    assert(sh_package_virtual_material_bind(m, 0, units[0].name, &atlas, semantics[0], &original, error, sizeof(error)) == -1 && !original);
    sh_package_virtual_material_close(m);
    puts("package_virtual_material_test: PASS"); return 0;
}
