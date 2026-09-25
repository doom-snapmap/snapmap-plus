/* Real typed map walks select exact occurrences and complete retained sources. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_map_uses.h"
#include "package_keys.h"
#include "map_embed.h"
#include "package_runtime.h"
#include "resource_graph_prepared.h"
#include "virtual_material_runtime.h"
#include "hook.h"
#include "package_fixture.h"

/* These paths must use the supplied immutable reader and delivery rows. */
const sh_package_compilation *sh_package_runtime_acquire(void) { assert(0); return NULL; }
void sh_package_runtime_release(void) { assert(0); }
const sh_resource_catalog *sh_package_runtime_catalog(void) { assert(0); return NULL; }
const sh_package_provenance *sh_package_runtime_bindings(void) { assert(0); return NULL; }
sh_package_bundle *sh_package_runtime_source_bundle(size_t owner, const unsigned char tree[32])
{ (void)owner; (void)tree; assert(0); return NULL; }
int sh_decl_server_registry_source(sh_decl_registry_source *source) { (void)source; assert(0); return 0; }
void *sh_typeinfo_get_reflect(void) { assert(0); return NULL; }
sh_resource_graph_preparation *sh_resource_graph_prepare_open(sh_decl_registry_source source, uintptr_t reflect)
{ (void)source; (void)reflect; assert(0); return NULL; }
int sh_resource_graph_prepare_entity(sh_resource_graph_preparation *p, const char *name, sh_decl_dependency_result *result)
{ (void)p; (void)name; (void)result; assert(0); return 0; }
int sh_resource_graph_prepare_inline(sh_resource_graph_preparation *p, const char *class_name,
    const char *inherit, const char *json, size_t length,
    int (*visitor)(void *, const char *, const char *), void *context, sh_decl_dependency_result *result)
{ (void)p; (void)class_name; (void)inherit; (void)json; (void)length; (void)visitor; (void)context; (void)result; assert(0); return 0; }
void sh_resource_graph_prepare_close(sh_resource_graph_preparation *p) { (void)p; assert(0); }
/* Linked through the virtual material runtime; never installed here. */
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; assert(0); return B2_PATCH_OK; }
void backend_log(const char *message) { (void)message; }
sh_virtual_material_view *sh_virtual_material_runtime_prepare(
    const sh_virtual_material_runtime_binding *bindings, size_t count, char *error, size_t capacity)
{ (void)bindings; (void)count; (void)error; (void)capacity; assert(0); return NULL; }
int sh_virtual_material_runtime_retire(sh_virtual_material_view **view) { assert(!view || !*view); return 1; }

typedef struct region { void *address; size_t length; } region;
typedef struct fixture { region regions[48]; size_t count; void *registry, *reflection; } fixture;
static void *allocation(fixture *f, size_t n)
{ void *p = calloc(1, n); assert(p && f->count < 48); f->regions[f->count++] = (region){p,n}; return p; }
static void pointer(void *p, size_t at, const void *value)
{ uintptr_t v = (uintptr_t)value; memcpy((char *)p + at, &v, 8); }
static void number(void *p, size_t at, int n) { memcpy((char *)p + at, &n, 4); }
static char *string(fixture *f, const char *s)
{ char *p = allocation(f, strlen(s) + 1); strcpy(p, s); return p; }
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    fixture *f = context;
    for (size_t i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].address;
        if (address >= start && address - start <= f->regions[i].length &&
            length <= f->regions[i].length - (size_t)(address - start)) {
            memcpy(out, (void *)address, length); return 1;
        }
    }
    return 0;
}
static void setup(fixture *f)
{
    static const char *classes[] = {"idDeclEntityDef", "Entity", "idStr"};
    void *managers = allocation(f, 8), *manager = allocation(f, 0x90);
    void *container = allocation(f, 0x30), *records = allocation(f, 4 * 0x38);
    void *objects = allocation(f, 4 * 16), *pointers = allocation(f, 4 * 16);
    void *fields = allocation(f, 4 * 0x48);
    f->registry = allocation(f, 0x18); f->reflection = allocation(f, 0xb0);
    pointer(f->registry, 8, managers); number(f->registry, 0x10, 1); pointer(managers, 0, manager);
    pointer(manager, 8, string(f, "entityDef")); pointer(manager, 0x10, string(f, classes[0]));
    pointer(f->reflection, 0, container); pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, 4);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, 4);
    for (size_t i = 0; i < 3; i++) {
        pointer(records, i * 0x38, string(f, classes[i])); pointer(records, i * 0x38 + 8, string(f, ""));
    }
    pointer(pointers, 8, (void *)0x10); pointer(objects, 2 * 16 + 8, (void *)0x70);
    pointer(records, 0x38 + 0x20, fields);
    pointer(fields, 0, string(f, classes[0])); pointer(fields, 8, string(f, "*"));
    pointer(fields, 0x10, string(f, "target")); number(fields, 0x1c, 8);
    pointer(fields, 0x48, string(f, classes[2])); pointer(fields, 0x48 + 8, string(f, ""));
    pointer(fields, 0x48 + 0x10, string(f, "text")); number(fields, 0x48 + 0x1c, 8);
    /* A spawn-type field and its enum: IMP (bit 2) and the all-bits composite. */
    pointer(fields, 0x90, string(f, SH_PACKAGE_KEY_TYPE)); pointer(fields, 0x90 + 8, string(f, ""));
    pointer(fields, 0x90 + 0x10, string(f, "spawnType")); number(fields, 0x90 + 0x1c, 8);
    void *enums = allocation(f, 2 * 0x18), *readers = allocation(f, 2 * 16), *members = allocation(f, 3 * 0x10);
    pointer(enums, 0, string(f, SH_PACKAGE_KEY_TYPE)); pointer(enums, 0x10, members);
    pointer(members, 0, string(f, "IMP")); number(members, 8, 4);
    pointer(members, 0x10, string(f, "ALL")); number(members, 0x18, -1);
    pointer(container, 0x10, enums); pointer(f->reflection, 0x58, readers); number(f->reflection, 0x60, 2);
}
static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/entitydef/vanilla.decl") && strcmp(path, "generated/decls/entitydef/root.decl")) return 0;
    *body = (unsigned char *)_strdup("{ class = \"Entity\"; edit = {} }");
    if (!*body) return -1;
    *length = strlen((char *)*body); return 1;
}
static sh_compiled_resource declaration(const char *name, const char *path, const char *body)
{
    sh_compiled_resource out = {0};
    out.name = out.native_name = (char *)name; out.type = out.native_type = "entitydef";
    out.engine_path = (char *)path; out.body = (unsigned char *)body; out.body_length = strlen(body);
    out.gameplay_owners.bits = 7; return out;
}
static sh_package_bundle *bundle(const char *label, unsigned char **expected, size_t *length)
{
    char descriptor[200], error[512];
    snprintf(descriptor, sizeof(descriptor), " {\"id\":\"same\",\"name\":\"%s\"} \r\n", label);
    sh_package_archive_file files[] = {
        {"package.json", (unsigned char *)descriptor, strlen(descriptor), 0},
        {"notes/untouched.txt", (unsigned char *)label, strlen(label), 0},
        {"assets/unused.bin", (unsigned char *)"unused\xff\0payload", 15, 0},
        {"empty", NULL, 0, 1}
    };
    sh_package_archive_files archive = {files, 4};
    *expected = sh_package_archive_write(&archive, length, error, sizeof(error)); assert(*expected);
    sh_package_bundle *out = sh_package_bundle_from_archive(*expected, *length, root, error, sizeof(error)); assert(out);
    return out;
}
static size_t isolated(sh_package_provenance *value, const sh_package_bundle_binding *row)
{
    size_t c = sh_package_provenance_context_add(value, SH_PACKAGE_CONTEXT_ISOLATED,
        (const unsigned char (*)[32])sh_package_bundle_fingerprint(row->original), 1, "");
    assert(c != SIZE_MAX); return c;
}
static void bind(sh_package_provenance *value, const char *original, const char *name,
    const size_t *contexts, size_t count)
{ assert(sh_package_provenance_bind(value, "entitydef", original, name, contexts, count)); }
static void use(sh_package_provenance *value, const char *owner, const char *reference, size_t binding, size_t context)
{
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, owner, reference};
    assert(sh_package_provenance_use_set(value, &site, binding, &context, 1));
}
static const sh_package_provenance_context *site_context(const sh_package_provenance *value,
    const char *owner, const char *reference)
{
    for (size_t i = 0; i < value->use_count; i++) if (!strcmp(value->uses[i].owner, owner) &&
        !strcmp(value->uses[i].reference, reference)) {
        assert(value->uses[i].context_count == 1); return value->contexts + value->uses[i].contexts[0];
    }
    assert(0); return NULL;
}
static const char root_site[] = "/k:entityDef/k:inherit";
static const char value_site[] = "/k:entityDef/k:state/k:edit/k:target";
typedef struct retained_fixture { int mode, calls; } retained_fixture;
static int retained_native(void *context, const sh_package_provenance *saved,
    size_t index, const char **replacement)
{
    retained_fixture *f = context; f->calls++;
    assert(saved && index < saved->use_count);
    if (f->mode <= 0) return f->mode;
    *replacement = f->mode == 2 ? "wrong-native-name" : saved->bindings[saved->uses[index].binding].name;
    return 1;
}
static const char repeated[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
    "\"inherit\":\"saved-root\",\"state\":{\"edit\":{\"target\":\"saved-value\",\"text\":\"saved-root\x97\"}}}},"
    "{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"saved-root\","
    "\"state\":{\"edit\":{\"target\":\"saved-value\"}}}}],\"note\":\"saved-value\x97\","
    "\"editorVars\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"saved-root\"}}";
static const char rebound[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
    "\"inherit\":\"current-root\",\"state\":{\"edit\":{\"target\":\"current-value\",\"text\":\"saved-root\x97\"}}}},"
    "{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"current-root\","
    "\"state\":{\"edit\":{\"target\":\"current-value\"}}}}],\"note\":\"saved-value\x97\","
    "\"editorVars\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"saved-root\"}}";

static void run(sh_package_source_reader *reader, sh_package_compilation *compiled,
    sh_package_bundle_binding rows[3], unsigned char *archives[3], const size_t lengths[3])
{
    sh_package_provenance catalog = {0}, saved = {0}, out = {0};
    sh_package_references references = {0}, resources = {0};
    size_t contexts[3], saved_contexts[2], length; char error[1024];
    sh_package_source_binding inspection; sh_mpkg_used *deliveries = NULL;
    sh_mpkg_use_requirements requirements = {compiled, NULL};
    for (size_t i = 0; i < 3; i++) contexts[i] = isolated(&catalog, rows + i);
    bind(&catalog, "root", "current-root", contexts, 2);
    bind(&catalog, "value", "current-value", contexts, 2);
    bind(&catalog, "new", "new-root", contexts + 2, 1);
    /* Class lookup visits this ancestor; it must never become a map use. */
    bind(&catalog, "ancestor", "ancestor", contexts + 2, 1);
    for (size_t i = 0; i < 2; i++) saved_contexts[i] = isolated(&saved, rows + i);
    bind(&saved, "root", "saved-root", saved_contexts, 2);
    bind(&saved, "value", "saved-value", saved_contexts, 2);
    use(&saved, "entity/0", root_site, 0, saved_contexts[0]);
    use(&saved, "entity/0", value_site, 1, saved_contexts[0]);
    use(&saved, "entity/1", root_site, 0, saved_contexts[1]);
    use(&saved, "entity/1", value_site, 1, saved_contexts[1]);
    char *text = sh_package_map_uses_select(reader, repeated, strlen(repeated), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "typed selection: %s\n", error);
    assert(text && length == strlen(rebound) && !memcmp(text, rebound, length + 1));
    assert(out.use_count == 4 && out.context_count == 2 && out.binding_count == 2 && inspection.changed == 4);
    assert(references.count == 2);
    assert(!memcmp(site_context(&out, "entity/0", root_site)->trees[0], sh_package_bundle_fingerprint(rows[0].original), 32));
    assert(!memcmp(site_context(&out, "entity/1", root_site)->trees[0], sh_package_bundle_fingerprint(rows[1].original), 32));
    assert(sh_mpkg_used_packages_for_uses(compiled, NULL, text, length, &references, &out,
        rows, 3, &deliveries, &resources, error, sizeof(error)) == 2);
    assert(resources.incomplete); /* No captured dynamic graph is claimed here. */
    for (size_t i = 0; i < 2; i++) {
        unsigned char *bytes; size_t n, original;
        for (original = 0; original < 3; original++) if (deliveries[i].bundle == rows[original].original) break;
        assert(original < 2); bytes = sh_mpkg_pack_used(deliveries + i, &n, error, sizeof(error));
        assert(bytes && n == lengths[original] && !memcmp(bytes, archives[original], n)); HeapFree(GetProcessHeap(), 0, bytes);
    }
    /* Missing captured payloads fail, even while its complete original archive
     * remains retained. A probe error (-1) is not a readable resource. */
    unsigned char *body = compiled->resources[1].body;
    compiled->resources[1].body = NULL;
    assert(sh_mpkg_used_packages_for_uses(compiled, NULL, text, length, &references, &out,
        rows, 3, &deliveries, &resources, error, sizeof(error)) == SIZE_MAX && !deliveries && !resources.count);
    compiled->resources[1].body = body;
    free(text); sh_mpkg_used_free(deliveries); deliveries = NULL;
    /* Already rebound text retains both distinct saved source choices. */
    text = sh_package_map_uses_select(reader, rebound, strlen(rebound), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !inspection.changed && out.use_count == 4); free(text);
    const char *deleted = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"current-root\"}}]}";
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.use_count == 1 && out.context_count == 1 && out.binding_count == 1);
    assert(!memcmp(out.contexts[0].trees[0], sh_package_bundle_fingerprint(rows[0].original), 32)); free(text);
    const char *changed = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"new-root\"}}]}";
    /* The same source context can acquire a new native alias after shared
     * editor composition changes. Native capture preserves the old alias only
     * with a positive lease witness; portable documents always recompile it. */
    sh_package_provenance retained = {0}; retained_fixture retained_state = {1, 0};
    size_t retained_context = isolated(&retained, rows);
    bind(&retained, "root", "current-root", &retained_context, 1);
    use(&retained, "entity/0", root_site, 0, retained_context);
    char *current_name = catalog.bindings[0].name;
    catalog.bindings[0].name = _strdup("new-root"); assert(catalog.bindings[0].name);
    text = sh_package_map_uses_select_retained(reader, deleted, strlen(deleted), &retained, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, retained_native, &retained_state,
        &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !strcmp(text, deleted) && !inspection.changed && retained_state.calls &&
        out.use_count == 1 && !strcmp(out.bindings[0].name, "current-root")); free(text);
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), &retained, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !strcmp(text, changed) && !strcmp(out.bindings[0].name, "new-root")); free(text);
    retained_state.mode = 0;
    text = sh_package_map_uses_select_retained(reader, deleted, strlen(deleted), &retained, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, retained_native, &retained_state,
        &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !strcmp(text, changed)); free(text);
    for (int mode = -1; mode <= 2; mode += 3) {
        retained_state.mode = mode;
        assert(!sh_package_map_uses_select_retained(reader, deleted, strlen(deleted), &retained, &catalog, rows, 3,
            sh_mpkg_new_use_required, &requirements, retained_native, &retained_state,
            &length, &out, &references, &inspection, error, sizeof(error)) && !length);
        assert(out.use_count == 1 && !strcmp(out.bindings[0].name, "new-root"));
    }
    free(catalog.bindings[0].name); catalog.bindings[0].name = current_name;
    sh_package_provenance_free(&retained);
    text = sh_package_map_uses_select(reader, changed, strlen(changed), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.use_count == 1 && out.context_count == 1);
    assert(!memcmp(out.contexts[0].trees[0], sh_package_bundle_fingerprint(rows[2].original), 32)); free(text);
    /* A missing exact delivery cannot borrow another source with equal bytes. */
    assert(!sh_package_map_uses_select(reader, repeated, strlen(repeated), &saved, &catalog, rows + 1, 2,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error)) && !length);
    assert(out.use_count == 1 && !memcmp(out.contexts[0].trees[0], sh_package_bundle_fingerprint(rows[2].original), 32));
    sh_package_bundle_binding partial[] = {rows[0], rows[2]};
    assert(!sh_package_map_uses_select(reader, repeated, strlen(repeated), &saved, &catalog, partial, 2,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error)) && !length && !inspection.changed);
    assert(out.use_count == 1 && !memcmp(out.contexts[0].trees[0], sh_package_bundle_fingerprint(rows[2].original), 32));
    size_t held = catalog.bindings[0].contexts[0]; catalog.bindings[0].contexts[0] = contexts[1];
    assert(!sh_package_map_uses_select(reader, repeated, strlen(repeated), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error)) && strstr(error, "saved source"));
    catalog.bindings[0].contexts[0] = held;

    /* Newly authored deduped assets choose one source independent of catalog
     * insertion order or delivery-row order, without recording lookup parents. */
    sh_package_provenance reverse = {0}; size_t reversed[2];
    reversed[0] = isolated(&reverse, rows + 1); reversed[1] = isolated(&reverse, rows);
    bind(&reverse, "root", "current-root", reversed, 2);
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error)); assert(text); free(text);
    unsigned char selected_tree[32]; memcpy(selected_tree, out.contexts[0].trees[0], 32);
    assert(out.use_count == 1 && out.context_count == 1);
    sh_package_bundle_binding reordered[] = {rows[2], rows[1], rows[0]};
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), NULL, &reverse, reordered, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.use_count == 1 && out.context_count == 1 && !memcmp(selected_tree, out.contexts[0].trees[0], 32)); free(text);
    assert(sh_mpkg_used_packages_for_uses(compiled, NULL, deleted, strlen(deleted), &references, &out,
        rows, 3, &deliveries, &resources, error, sizeof(error)) == 1);
    sh_mpkg_used_free(deliveries); deliveries = NULL;
    sh_package_provenance empty_catalog = {0};
    assert(!sh_package_map_uses_select(reader, deleted, strlen(deleted), NULL, &empty_catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error)) &&
        strstr(error, "source catalog"));
    const unsigned char *a = sh_package_bundle_fingerprint(rows[0].original), *b = sh_package_bundle_fingerprint(rows[1].original);
    assert(!memcmp(selected_tree, memcmp(a,b,32) < 0 ? a : b, 32));
    /* Compiled baseline/gameplay proof decides NEW ownership. The same native
     * name in an authored catalog is insufficient, while a saved use survives. */
    compiled->resources[1].gameplay_owners.bits = 0; compiled->resources[1].baseline_known = 1;
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !out.use_count); free(text);
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.use_count == 1 && !memcmp(out.contexts[0].trees[0], a, 32)); free(text);
    compiled->resources[1].baseline_known = 2; compiled->resources[1].source_count = 1;
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.use_count == 1); free(text);
    compiled->resources[1].baseline_known = 3;
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !out.use_count); free(text);
    compiled->resources[1].original_path = "generated/decls/entitydef/authored-root.decl";
    text = sh_package_map_uses_select(reader, deleted, strlen(deleted), NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.use_count == 1); free(text);
    compiled->resources[1].original_path = NULL; compiled->resources[1].source_count = 0;
    compiled->resources[1].baseline_known = 0; compiled->resources[1].gameplay_owners.bits = 7;
    const char *ordinary[] = {"unknown", "timeline", "idTarget", "lift"};
    for (size_t i = 0; i < 4; i++)
        assert(!sh_mpkg_new_use_required(&requirements, "entitydef", ordinary[i], error, sizeof(error)));
    const char *vanilla = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"vanilla\"}}]}";
    text = sh_package_map_uses_select(reader, vanilla, strlen(vanilla), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !out.use_count && out.semantics == SH_PACKAGE_PROVENANCE_USES && references.count == 1); free(text);
    assert(sh_mpkg_used_packages_for_uses(compiled, NULL, vanilla, strlen(vanilla), &references, &out,
        rows, 3, &deliveries, &resources, error, sizeof(error)) == 0); sh_mpkg_used_free(deliveries); deliveries = NULL;
    const char *authored_alias = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\"}}]}";
    text = sh_package_map_uses_select(reader, authored_alias, strlen(authored_alias), NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !strcmp(text, authored_alias) && !out.use_count); free(text);
    const char *world = "{\"worldA\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"current-value\"}}";
    text = sh_package_map_uses_select(reader, world, strlen(world), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.use_count == 1 && out.uses[0].kind == SH_PACKAGE_USE_WORLD &&
        !strcmp(out.uses[0].owner, "world") && !strcmp(out.uses[0].reference, "/k:worldA/k:value")); free(text);

    /* Historical compiler identity explicitly maps to BOTH distinct originals.
     * The resulting modern site retains the complete composed context. */
    sh_package_provenance historical = {0}, historical_catalog = {0};
    unsigned char compiler_tree[1][32] = {{77}}, originals[2][32];
    memcpy(rows[0].compiler_tree, compiler_tree[0], 32); memcpy(rows[1].compiler_tree, compiler_tree[0], 32);
    memcpy(originals[0], a, 32); memcpy(originals[1], b, 32);
    size_t hc = sh_package_provenance_context_add(&historical, SH_PACKAGE_CONTEXT_COMPOSED, compiler_tree, 1, NULL);
    size_t cc = sh_package_provenance_context_add(&historical_catalog, SH_PACKAGE_CONTEXT_COMPOSED, originals, 2, NULL);
    bind(&historical, "root", "saved-root", &hc, 1); historical.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
    bind(&historical_catalog, "root", "current-root", &cc, 1);
    const char *old = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"saved-root\"}}]}";
    text = sh_package_map_uses_select(reader, old, strlen(old), &historical, &historical_catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "historical selection: %s\n", error);
    assert(text && out.semantics == SH_PACKAGE_PROVENANCE_USES && out.use_count == 1);
    assert(out.uses[0].kind == SH_PACKAGE_USE_OBJECT && !strcmp(out.uses[0].owner, "entity/0"));
    assert(out.context_count == 1 && out.contexts[0].mode == SH_PACKAGE_CONTEXT_COMPOSED && out.contexts[0].tree_count == 2);
    assert(sh_mpkg_used_packages_for_uses(compiled, NULL, text, length, &references, &out,
        rows, 3, &deliveries, &resources, error, sizeof(error)) == 2);
    free(text);
    /* Catalog contexts already carrying the old recorded keys are supported. */
    memcpy(historical_catalog.contexts[0].trees[0], compiler_tree[0], 32);
    historical_catalog.contexts[0].tree_count = 1;
    text = sh_package_map_uses_select(reader, old, strlen(old), &historical, &historical_catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.contexts[0].tree_count == 2); free(text);
    /* The binding-only historical format also supports isolated source sets.
     * Two original rows sharing a migration key expand to both contexts. */
    sh_package_provenance old_isolated = {0};
    size_t old_context = sh_package_provenance_context_add(&old_isolated, SH_PACKAGE_CONTEXT_ISOLATED, compiler_tree, 1, "");
    bind(&old_isolated, "root", "saved-root", &old_context, 1); old_isolated.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
    text = sh_package_map_uses_select(reader, old, strlen(old), &old_isolated, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && out.use_count == 1 && out.uses[0].context_count == 2 && out.context_count == 2); free(text);
    sh_package_provenance_free(&old_isolated);
    historical.semantics = SH_PACKAGE_PROVENANCE_CATALOG;
    use(&historical, "entity/0", root_site, 0, hc);
    assert(!sh_package_map_uses_select(reader, old, strlen(old), &historical, &historical_catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error))); /* Modern is strict. */
    sh_package_provenance_free(&historical); sh_package_provenance_free(&historical_catalog);
    sh_package_provenance_free(&reverse); sh_package_provenance_free(&saved); sh_package_provenance_free(&catalog);
    /* Selected complete source handles survive destruction of every input row. */
    for (size_t i = 0; i < 3; i++) { sh_package_bundle_release(rows[i].original); rows[i].original = NULL; }
    for (size_t i = 0; i < 2; i++) {
        unsigned char *bytes; size_t n;
        bytes = sh_mpkg_pack_used(deliveries + i, &n, error, sizeof(error));
        assert(bytes && ((n == lengths[0] && !memcmp(bytes, archives[0], n)) ||
            (n == lengths[1] && !memcmp(bytes, archives[1], n)))); HeapFree(GetProcessHeap(), 0, bytes);
    }
    sh_mpkg_used_free(deliveries); sh_package_provenance_free(&out);
    sh_package_references_free(&references); sh_package_references_free(&resources);
}
/* A map-authored spawn-type bit keeps the package it was chosen from: it is
 * bound per bit at "<site>#<bit>" and rewritten through the bit that package
 * holds in the current library, back and forth across differently keyed
 * libraries. Keys are not resource references; a bit no current package
 * registers binds nothing and stays as authored. */
static void run_keys(sh_package_source_reader *reader, sh_package_compilation *compiled,
    sh_package_bundle_binding rows[3])
{
    sh_package_provenance catalog = {0}, alone = {0}, saved = {0}, out = {0}, reopened = {0};
    sh_package_references references = {0};
    sh_package_source_binding inspection; size_t contexts[2], saved_context, alone_context, length;
    sh_mpkg_use_requirements requirements = {compiled, NULL};
    char error[1024], site[160];
    static const char key_site[] = "/k:entityDef/k:state/k:edit/k:spawnType";
    static const char authored[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"IMP 524288\"}}}}]}";
    static const char moved[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"IMP 1048576\"}}}}]}";
    /* This library moved rows[1]'s authored key 524288 to 1048576 and rows[0]
     * kept 524288; rows[1] alone keeps it. */
    for (size_t i = 0; i < 2; i++) contexts[i] = isolated(&catalog, rows + i);
    assert(sh_package_provenance_bind(&catalog, SH_PACKAGE_KEY_FAMILY, "524288", "524288", contexts, 1));
    assert(sh_package_provenance_bind(&catalog, SH_PACKAGE_KEY_FAMILY, "524288", "1048576", contexts + 1, 1));
    alone_context = isolated(&alone, rows + 1);
    assert(sh_package_provenance_bind(&alone, SH_PACKAGE_KEY_FAMILY, "524288", "524288", &alone_context, 1));
    /* Saved where rows[1] alone held its key. */
    saved_context = isolated(&saved, rows + 1);
    assert(sh_package_provenance_bind(&saved, SH_PACKAGE_KEY_FAMILY, "524288", "524288", &saved_context, 1));
    snprintf(site, sizeof(site), "%s#524288", key_site);
    use(&saved, "entity/0", site, 0, saved_context);
    char *text = sh_package_map_uses_select(reader, authored, sizeof(authored) - 1, &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "map key selection: %s\n", error);
    assert(text && !strcmp(text, moved) && inspection.changed == 1);
    /* The occurrence is recorded under the bit it now holds; the only
     * resource reference is the vanilla parent. */
    assert(out.use_count == 1 && !strcmp(out.bindings[out.uses[0].binding].name, "1048576"));
    assert(references.count == 1 && !strcmp(references.items[0].type, "entitydef"));
    snprintf(site, sizeof(site), "%s#1048576", key_site);
    assert(!memcmp(site_context(&out, "entity/0", site)->trees[0], sh_package_bundle_fingerprint(rows[1].original), 32));
    free(text);
    /* A new value binds the package now holding the bit, at the bit it has. */
    text = sh_package_map_uses_select(reader, moved, sizeof(moved) - 1, NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &reopened, &references, &inspection, error, sizeof(error));
    snprintf(site, sizeof(site), "%s#1048576", key_site);
    assert(text && !strcmp(text, moved) && !inspection.changed && reopened.use_count == 1);
    assert(!memcmp(site_context(&reopened, "entity/0", site)->trees[0], sh_package_bundle_fingerprint(rows[1].original), 32));
    free(text);
    /* That record reopened where rows[1] keeps its authored key. */
    text = sh_package_map_uses_select(reader, moved, sizeof(moved) - 1, &reopened, &alone, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !strcmp(text, authored) && out.use_count == 1); free(text);
    /* rows[0]'s kept key rebinds to itself; an unregistered bit stays. */
    static const char dangling[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"2097152\"}}}}]}";
    text = sh_package_map_uses_select(reader, dangling, sizeof(dangling) - 1, NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !strcmp(text, dangling) && !out.use_count); free(text);
    text = sh_package_map_uses_select(reader, authored, sizeof(authored) - 1, NULL, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    snprintf(site, sizeof(site), "%s#524288", key_site);
    assert(text && !strcmp(text, authored) && out.use_count == 1 &&
        !memcmp(site_context(&out, "entity/0", site)->trees[0], sh_package_bundle_fingerprint(rows[0].original), 32));
    free(text);
    sh_package_provenance_free(&catalog); sh_package_provenance_free(&alone); sh_package_provenance_free(&saved);
    sh_package_provenance_free(&out); sh_package_provenance_free(&reopened); sh_package_references_free(&references);
}
int main(void)
{
    fixture f = {0}; sh_package_sources sources = {0}; sh_package_compilation compiled = {0};
    sh_package_bundle_binding rows[3] = {0}; unsigned char *archives[3]; size_t lengths[3];
    char temporary[MAX_PATH], error[1024];
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-map-uses-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 3; i++) {
        snprintf(rows[i].source_name, sizeof(rows[i].source_name), "source-%zu", i);
        rows[i].original = bundle(rows[i].source_name, archives + i, lengths + i);
        memcpy(rows[i].compiler_tree, sh_package_bundle_fingerprint(rows[i].original), 32);
    }
    setup(&f);
    sh_compiled_resource files[] = {
        declaration("ancestor", "generated/decls/entitydef/ancestor.decl", "{ class = \"Entity\"; edit = {} }"),
        declaration("current-root", "generated/decls/entitydef/current-root.decl", "{ inherit = \"ancestor\"; edit = {} }"),
        declaration("current-value", "generated/decls/entitydef/current-value.decl", "{ class = \"Entity\"; edit = {} }"),
        declaration("new-root", "generated/decls/entitydef/new-root.decl", "{ class = \"Entity\"; edit = {} }")
    };
    compiled.sources = &sources; compiled.resources = files; compiled.resource_count = 4;
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    sh_package_source_reader *reader = sh_package_source_reader_open(&compiled, NULL, baseline, NULL,
        registry, (uintptr_t)f.reflection, error, sizeof(error));
    if (!reader) fprintf(stderr, "reader: %s\n", error);
    assert(reader);
    run_keys(reader, &compiled, rows);
    run(reader, &compiled, rows, archives, lengths);
    sh_package_source_reader_close(reader);
    while (f.count) free(f.regions[--f.count].address);
    for (size_t i = 0; i < 3; i++) free(archives[i]);
    cleanup(); assert(!failures); puts("package_map_uses_test: PASS"); return 0;
}
