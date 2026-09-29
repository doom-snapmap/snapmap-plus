#include "../src/backend/virtual_material_runtime.c"
#include "package_virtual_material.h"
#include "package_native_names.h"
#include "decl_native_lex.h"
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
static int pages_absent(void *context, const char *path, sh_package_file **file, char *error, size_t capacity)
{ (void)context; (void)path; (void)error; (void)capacity; *file = NULL; return 0; }
static void put(unsigned char *p, uint64_t x, unsigned bytes)
{ for (unsigned i = 0; i < bytes; i++) p[i] = (unsigned char)(x >> (i * 8)); }
/* Changed page data links a private material that names its private provider:
 * a 31-character name the engine can record, the whole family projected under
 * it, and landpagefile written into the material's top level. */
static void private_pages(const char *material_text, int selectable)
{
    static unsigned char mega2[0x199];
    char error[1024]; unsigned char semantics[32]; int original;
    put(mega2, 0xa63fbb21u, 4); put(mega2 + 4, 2, 4); put(mega2 + 0xc, 12, 4); put(mega2 + 0x38, 0x170, 8);
    put(mega2 + 0x40, 0x180, 8); put(mega2 + 0x48, 1, 4); put(mega2 + 0x4c, 2, 4); put(mega2 + 0x58, 2, 4);
    put(mega2 + 0x5c, 1, 4); put(mega2 + 0x64, 2, 4); put(mega2 + 0x170, 0x188, 8); put(mega2 + 0x178, 17, 8);
    put(mega2 + 0x184, UINT32_MAX, 4); memset(mega2 + 0x188, 0x3c, 17);
    originals f = {0}; sh_package_sources inventory = {0}; sh_package_compilation scope = {0};
    const sh_package_compilation *views[] = {&scope};
    sh_compiled_resource files[2]; const sh_compiled_resource *unit_files[] = {files};
    files[0] = resource("generated/decls/material/surfaces/stone.decl", material_text);
    files[0].type = "material"; files[0].name = "surfaces/stone";
    files[1] = resource("virtualtextures/_vmtr_sq1.mega2", ""); files[1].body = mega2; files[1].body_length = sizeof(mega2);
    scope.resources = files; scope.resource_count = 2; scope.sources = &inventory;
    size_t scope_id = 0; sh_package_link_unit unit = {"material", "surfaces/stone", &scope, unit_files, 1};
    sh_package_link_inputs inputs = {&unit, &scope_id, 1};
    sh_virtual_page_source *pages = sh_virtual_page_source_open(&scope, "virtualtextures/_vmtr", pages_absent, NULL,
        error, sizeof(error));
    assert(pages);
    sh_package_virtual_atlas atlas = {"_vmtr", "virtualtextures/_vmtr.pages", {0}, 0, pages, 2, 2};
    memcpy(atlas.content, sh_virtual_page_source_identity(pages), 32);
    sh_package_virtual_materials *m = sh_package_virtual_material_open(views, 1, read_original, &f); assert(m);
    assert(sh_package_virtual_material_bind(m, 0, unit.name, &atlas, unit.semantics, &original, error, sizeof(error)) == 1);
    assert(!original); memcpy(semantics, unit.semantics, 32);
    sh_virtual_page_source_release(pages);
    sh_package_link_adapter adapter = {&inputs, resolve, copy, sh_package_native_names_assign};
    sh_package_link_output linked = {0};
    assert(sh_package_link(&inventory, &unit, 1, &adapter, NULL, NULL, &linked, error, sizeof(error)));
    sh_virtual_material_view *view = sh_package_virtual_material_prepare(m, &inputs, &linked, error, sizeof(error));
    if (!selectable) {
        assert(!view && strstr(error, "cannot name its page provider"));
    } else {
        const char *name = linked.names[linked.classes.nodes[0]];
        vmr_entry *entry = vmr_find(view, name);
        assert(view && entry && entry->pages && entry->record.x == 64 && entry->dlc == 2);
        assert(strlen(entry->atlas) == 31 && !strncmp(entry->atlas, "smp/vt/", 7) && !strstr(entry->atlas, "_vmtr"));
        char companion[96], expected[96];
        snprintf(companion, sizeof(companion), "virtualtextures/%s_sq1.mega2", entry->atlas);
        assert(entry->page_paths[0] && !strcmp(entry->page_paths[0], companion));
        const sh_compiled_resource *served = sh_package_compilation_find(linked.compiled, companion);
        assert(served && served->body_length == sizeof(mega2));
        const sh_compiled_resource *text = NULL;
        for (size_t i = 0; i < linked.compiled->resource_count; i++)
            if (sh_package_resource_native_matches(linked.compiled->resources + i, "material", name)) text = linked.compiled->resources + i;
        assert(text && text->body);
        snprintf(expected, sizeof(expected), "landpagefile\t\"%s\"", entry->atlas);
        const char *at = strstr((const char *)text->body, expected);
        assert(at && !strstr(at + 1, "landpagefile") && !strstr((const char *)text->body, "\"_vmtr\""));
        sh_decl_tokens tokens = {0};
        assert(sh_decl_native_lex((sh_decl_source){(const char *)text->body, text->body_length}, &tokens, NULL, 0));
        sh_decl_tokens_free(&tokens);
        assert(sh_virtual_material_runtime_release(&view));
    }
    sh_package_link_output_free(&linked); sh_package_virtual_material_close(m);
}
/* Many materials in several scopes: every lookup (cached, repeated bind,
 * witnesses, the page-data check) finds exactly its own scoped row, and names
 * still match only as stored. */
enum { MANY = 300, MANY_SCOPES = 5 };
static char many_table[MANY * 48 + 32];
static int read_many(void *context, const char *path, unsigned char **body, size_t *length)
{
    originals *f = context;
    f->reads++; *body = NULL; *length = 0;
    if (strcmp(path, "virtualtextures/_vmtr.vmtr")) return 0;
    *length = strlen(many_table); *body = (unsigned char *)_strdup(many_table); assert(*body);
    return 1;
}
static int count_witness(void *context, const char *role, const sh_compiled_resource *file)
{ (void)file; assert(!strcmp(role, "virtual-atlas/selected-row")); ++*(int *)context; return 1; }
static void many_bindings(void)
{
    char error[1024], name[32]; unsigned char semantics[32], again[32]; int original, witnesses;
    size_t at = (size_t)snprintf(many_table, sizeof(many_table), "26\n%d\n", MANY);
    for (int i = 0; i < MANY; i++)
        at += (size_t)snprintf(many_table + at, sizeof(many_table) - at, "%d 4 16 16 0 1 2 \"mat/%03d\"\n", i * 16, i);
    originals f = {0}; sh_package_sources inventory = {0}; sh_package_compilation scopes[MANY_SCOPES] = {0};
    const sh_package_compilation *views[MANY_SCOPES];
    for (size_t s = 0; s < MANY_SCOPES; s++) { scopes[s].sources = &inventory; views[s] = scopes + s; }
    sh_package_virtual_atlas atlas = {"_vmtr", "virtualtextures/_vmtr.pages", {1}, 1}, changed = atlas;
    changed.content[0] = 2; changed.original = 0;
    sh_package_virtual_materials *m = sh_package_virtual_material_open(views, MANY_SCOPES, read_many, &f); assert(m);
    static unsigned char bound[MANY_SCOPES - 1][MANY][32];
    for (size_t s = 0; s + 1 < MANY_SCOPES; s++) for (int i = 0; i < MANY; i++) {
        snprintf(name, sizeof(name), "mat/%03d", i);
        assert(sh_package_virtual_material_bind(m, s, name, &atlas, bound[s][i], &original, error, sizeof(error)) == 1 && original);
    }
    for (size_t s = 0; s + 1 < MANY_SCOPES; s++) for (int i = 0; i < MANY; i++) {
        snprintf(name, sizeof(name), "mat/%03d", i);
        assert(sh_package_virtual_material_cached(m, s, name, semantics, &original) && original &&
            !memcmp(semantics, bound[s][i], 32));
        assert(sh_package_virtual_material_bind(m, s, name, &atlas, again, &original, error, sizeof(error)) == 1 &&
            !memcmp(again, bound[s][i], 32));
        witnesses = 0; /* An unchanged stock row carries no selected-row witness. */
        assert(sh_package_virtual_material_witnesses(m, s, name, count_witness, &witnesses, error, sizeof(error)) && !witnesses);
        if (i) assert(memcmp(bound[s][i], bound[s][i - 1], 32)); /* each row its own semantics */
        if (s) assert(!memcmp(bound[s][i], bound[0][i], 32));   /* the same row in every scope */
    }
    /* Only the stored spelling matches, as before; an unbound scope has nothing. */
    assert(!sh_package_virtual_material_cached(m, 0, "MAT/000", semantics, &original));
    assert(!sh_package_virtual_material_cached(m, MANY_SCOPES - 1, "mat/000", semantics, &original));
    assert(!sh_package_virtual_material_witnesses(m, MANY_SCOPES - 1, "mat/000", count_witness, &witnesses, error, sizeof(error)));
    /* A row of the same resolved atlas with other page data is refused after
     * many agreeing rows, and a changed atlas for a bound row stays refused. */
    assert(sh_package_virtual_material_bind(m, MANY_SCOPES - 1, "mat/007", &changed, semantics, &original, error, sizeof(error)) == -1 &&
        strstr(error, "different page data"));
    assert(sh_package_virtual_material_bind(m, 2, "mat/150", &changed, semantics, &original, error, sizeof(error)) == -1 &&
        strstr(error, "changed its resolved atlas"));
    assert(f.reads == 6);
    sh_package_virtual_material_close(m);
}
int main(void)
{
    many_bindings();
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
    f.product = 0;
    private_pages("{\n\tdiffusemap\t\"surfaces/stone\"\n}", 1);
    private_pages("{\n\tlandpagefile\t\"_vmtr\"\n\tdiffusemap\t\"surfaces/stone\"\n}", 1);
    private_pages("{\n\tstage { landpagefile \"_vmtr\" }\n}", 0);
    puts("package_virtual_material_test: PASS"); return 0;
}
