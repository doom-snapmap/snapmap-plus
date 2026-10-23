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
#include "package_archive.h"
#include "package_runtime.h"
#include "resource_graph_prepared.h"
#include "virtual_material_runtime.h"
#include "hook.h"
#include "package_fixture.h"
#include "package_link_inputs.h"

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
int hook_unpatch(void *trampoline) { (void)trampoline; assert(0); return 0; }
void backend_log(const char *message) { (void)message; }
sh_virtual_material_view *sh_virtual_material_runtime_prepare(
    const sh_virtual_material_runtime_binding *bindings, size_t count, char *error, size_t capacity)
{ (void)bindings; (void)count; (void)error; (void)capacity; assert(0); return NULL; }
int sh_virtual_material_runtime_retire(sh_virtual_material_view **view) { assert(!view || !*view); return 1; }

typedef struct region { void *address; size_t length; } region;
typedef struct fixture { region regions[96]; size_t count; void *registry, *reflection; } fixture;
static void *allocation(fixture *f, size_t n)
{ void *p = calloc(1, n); assert(p && f->count < 96); f->regions[f->count++] = (region){p,n}; return p; }
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
    /* A spawn-type field and its enum as the game registers it: single-bit
     * constants for bits 0-18 (IMP is bit 2), the "any" bit 30 and the
     * all-bits composite, leaving bits 19-29 unnamed. */
    pointer(fields, 0x90, string(f, SH_PACKAGE_KEY_TYPE)); pointer(fields, 0x90 + 8, string(f, ""));
    pointer(fields, 0x90 + 0x10, string(f, "spawnType")); number(fields, 0x90 + 0x1c, 8);
    void *enums = allocation(f, 2 * 0x18), *readers = allocation(f, 2 * 16), *members = allocation(f, 22 * 0x10);
    pointer(enums, 0, string(f, SH_PACKAGE_KEY_TYPE)); pointer(enums, 0x10, members);
    for (int i = 0; i < 21; i++) {
        char name[16];
        snprintf(name, sizeof(name), "T%d", i);
        pointer(members, i * 0x10, string(f, i == 2 ? "IMP" : i == 19 ? "ANY" : i == 20 ? "ALL" : name));
        number(members, i * 0x10 + 8, i < 19 ? 1 << i : i == 19 ? 1 << 30 : -1);
    }
    pointer(container, 0x10, enums); pointer(f->reflection, 0x58, readers); number(f->reflection, 0x60, 2);
}
static int g_native_empty; /* M102: the stock catalog has its own entitydef/_emptyName */
static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/entitydef/vanilla.decl") && strcmp(path, "generated/decls/entitydef/root.decl") &&
        (!g_native_empty || strcmp(path, "generated/decls/entitydef/_emptyName.decl"))) return 0;
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
    /* Historical (name-level) uses meet a map only here. One contributor's
     * selection becomes exact typed uses at the sites that reference it, the
     * only shape the occurrence ingress accepts (package_occurrence_test: a
     * record still holding a historical use needs selection), so no exported
     * record carries one forward. Two contributor contexts selected for the same name by
     * historical uses cannot be told apart by name: selection refuses and
     * commits nothing, whatever their order. This is the ingress invariant
     * behind the compile's own-record historical authority. */
    {
        sh_package_provenance history = {0}, typed = {0};
        sh_package_references historic = {0};
        sh_package_source_binding seen;
        size_t h[2], hl;
        sh_package_provenance_site document = {SH_PACKAGE_USE_HISTORICAL, "", ""};
        for (size_t i = 0; i < 2; i++) h[i] = isolated(&history, rows + i);
        bind(&history, "root", "saved-root", h, 2);
        bind(&history, "value", "saved-value", h, 2);
        assert(sh_package_provenance_use_set(&history, &document, 0, h, 1));
        assert(sh_package_provenance_use_set(&history, &document, 1, h, 1));
        history.semantics = SH_PACKAGE_PROVENANCE_USES;
        char *typed_text = sh_package_map_uses_select(reader, repeated, strlen(repeated), &history, &catalog, rows, 3,
            sh_mpkg_new_use_required, &requirements, &hl, &typed, &historic, &seen, error, sizeof(error));
        if (!typed_text) fprintf(stderr, "historical selection: %s\n", error);
        if (typed_text && typed.use_count != 4) fprintf(stderr, "historical selection uses: %zu\n", typed.use_count);
        assert(typed_text && typed.use_count == 4);
        for (size_t i = 0; i < typed.use_count; i++) assert(typed.uses[i].kind == SH_PACKAGE_USE_OBJECT);
        assert(!memcmp(site_context(&typed, "entity/1", root_site)->trees[0], sh_package_bundle_fingerprint(rows[0].original), 32));
        free(typed_text);
        for (int order = 0; order < 2; order++) {
            sh_package_provenance both = {0}, refused = {0};
            sh_package_references none = {0};
            size_t b[2];
            for (size_t i = 0; i < 2; i++) b[i] = isolated(&both, rows + i);
            bind(&both, "root", "saved-root", b, 2);
            bind(&both, "value", "saved-value", b, 2);
            assert(sh_package_provenance_use_set(&both, &document, 0, b + order, 1));
            assert(sh_package_provenance_use_set(&both, &document, 0, b + !order, 1));
            assert(sh_package_provenance_use_set(&both, &document, 1, b, 1));
            both.semantics = SH_PACKAGE_PROVENANCE_USES;
            typed_text = sh_package_map_uses_select(reader, repeated, strlen(repeated), &both, &catalog, rows, 3,
                sh_mpkg_new_use_required, &requirements, &hl, &refused, &none, &seen, error, sizeof(error));
            if (typed_text || !strstr(error, "exact selected contributor context"))
                fprintf(stderr, "divergent historical selection %d: %s\n", order, typed_text ? "selected" : error);
            assert(!typed_text && !refused.use_count && strstr(error, "exact selected contributor context"));
            sh_package_provenance_free(&both); sh_package_provenance_free(&refused); sh_package_references_free(&none);
        }
        sh_package_provenance_free(&history); sh_package_provenance_free(&typed); sh_package_references_free(&historic);
    }
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
    /* A value changed to a native name (as native map data conversion does) is
     * a fresh selection too: its site's saved row must not restore the package
     * name it used to bind. */
    const char *native_value = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"vanilla\"}}]}";
    text = sh_package_map_uses_select(reader, native_value, strlen(native_value), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "native value: %s\n", error);
    assert(text && !strcmp(text, native_value) && !out.use_count); free(text);

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
        "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"524292\"}}}}]}";
    static const char moved[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"1048580\"}}}}]}";
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
    /* Native capture retains objects, and a key is none: the retained-use
     * witness is never asked about it (it would find no served file), so
     * a capture of keys chosen in this session rebinds them as above. */
    {
        retained_fixture witness = {-1, 0};
        sh_package_provenance recaptured = {0};
        text = sh_package_map_uses_select_retained(reader, moved, sizeof(moved) - 1, &reopened, &catalog, rows, 3,
            sh_mpkg_new_use_required, &requirements, retained_native, &witness,
            &length, &recaptured, &references, &inspection, error, sizeof(error));
        if (!text) fprintf(stderr, "retained key capture: %s\n", error);
        assert(text && !strcmp(text, moved) && !witness.calls && recaptured.use_count == 1 &&
            !strcmp(recaptured.bindings[recaptured.uses[0].binding].name, "1048576"));
        free(text); sh_package_provenance_free(&recaptured);
    }
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
    /* A package that changed a stock row holds a private bit for that stock
     * type where others change it too (the library binds the stock bit, 4
     * here, to it). A map value chosen there returns to the stock bit where
     * the package's change is uncontended, and back. */
    {
        sh_package_provenance held = {0}, plain = {0}, record = {0}, back = {0};
        size_t held_context = isolated(&held, rows + 1), plain_context = isolated(&plain, rows + 1);
        static const char variant[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
            "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"1048584\"}}}}]}";
        static const char stock[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
            "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"12\"}}}}]}";
        assert(sh_package_provenance_bind(&held, SH_PACKAGE_KEY_FAMILY, "4", "1048576", &held_context, 1));
        assert(sh_package_provenance_bind(&plain, SH_PACKAGE_KEY_FAMILY, "4", "4", &plain_context, 1));
        text = sh_package_map_uses_select(reader, variant, sizeof(variant) - 1, NULL, &held, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &record, &references, &inspection, error, sizeof(error));
        snprintf(site, sizeof(site), "%s#1048576", key_site);
        assert(text && !strcmp(text, variant) && record.use_count == 1 &&
            !memcmp(site_context(&record, "entity/0", site)->trees[0], sh_package_bundle_fingerprint(rows[1].original), 32));
        free(text);
        text = sh_package_map_uses_select(reader, variant, sizeof(variant) - 1, &record, &plain, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &back, &references, &inspection, error, sizeof(error));
        if (!text || strcmp(text, stock)) fprintf(stderr, "named key reopen: %s\n", text ? text : error);
        assert(text && !strcmp(text, stock) && back.use_count == 1); free(text);
        sh_package_provenance_free(&record);
        text = sh_package_map_uses_select(reader, stock, sizeof(stock) - 1, &back, &held, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &record, &references, &inspection, error, sizeof(error));
        assert(text && !strcmp(text, variant)); free(text);
        sh_package_provenance_free(&held); sh_package_provenance_free(&plain);
        sh_package_provenance_free(&record); sh_package_provenance_free(&back);
    }
    /* A marked private key (bit 28 with bit 20) binds whole: past the single
     * bits a package's authored key 524288 is held as 269484032, and back. */
    {
        sh_package_provenance held = {0}, plain = {0}, record = {0}, back = {0};
        size_t held_context = isolated(&held, rows + 1), plain_context = isolated(&plain, rows + 1);
        static const char coded[] = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
            "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"269484036\"}}}}]}";
        assert(sh_package_provenance_bind(&held, SH_PACKAGE_KEY_FAMILY, "524288", "269484032", &held_context, 1));
        assert(sh_package_provenance_bind(&plain, SH_PACKAGE_KEY_FAMILY, "524288", "524288", &plain_context, 1));
        text = sh_package_map_uses_select(reader, coded, sizeof(coded) - 1, NULL, &held, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &record, &references, &inspection, error, sizeof(error));
        snprintf(site, sizeof(site), "%s#269484032", key_site);
        assert(text && !strcmp(text, coded) && record.use_count == 1 &&
            !memcmp(site_context(&record, "entity/0", site)->trees[0], sh_package_bundle_fingerprint(rows[1].original), 32));
        free(text);
        text = sh_package_map_uses_select(reader, coded, sizeof(coded) - 1, &record, &plain, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &back, &references, &inspection, error, sizeof(error));
        if (!text || strcmp(text, authored)) fprintf(stderr, "marked key reopen: %s\n", text ? text : error);
        assert(text && !strcmp(text, authored) && back.use_count == 1); free(text);
        sh_package_provenance_free(&record);
        text = sh_package_map_uses_select(reader, authored, sizeof(authored) - 1, &back, &held, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &record, &references, &inspection, error, sizeof(error));
        assert(text && !strcmp(text, coded)); free(text);
        sh_package_provenance_free(&held); sh_package_provenance_free(&plain);
        sh_package_provenance_free(&record); sh_package_provenance_free(&back);
    }
    /* A group code: rows[1]'s authored keys 524288 and 1048576 held as the
     * multi-bit keys A (bits 28|19|20) and B (28|19|21), whose OR is neither,
     * and a map value naming both (G, the group code for {A, B}). Saving
     * records each member at "<site>#<member>". Reopened where the keys are A2
     * and B2 the value carries the first code that library's keys do not hold
     * and neither of its tables defines in allocation order (28|19 is held
     * and the table beside defines 28|20, so 28|21), never an OR; and back. */
    {
        const uint32_t a = 270008320u, b = 271056896u, g = 269484032u, a2 = 276824064u, b2 = 285212672u;
        const uint32_t held_there = 268959744u, g2 = 270532608u;
        sh_package_provenance held = {0}, moved_keys = {0}, record = {0}, back = {0}, again = {0};
        sh_package_key_groups author, client, beside, empty;
        sh_package_key_set members = {{0}}, unrelated = {{0}}, author_taken = {{0}}, client_taken = {{0}}, now = {{0}};
        size_t held_context = isolated(&held, rows + 1), moved_context = isolated(&moved_keys, rows + 1);
        char valued[256], expected[256], number[16];
        memset(&author, 0, sizeof(author)); memset(&client, 0, sizeof(client));
        memset(&beside, 0, sizeof(beside)); memset(&empty, 0, sizeof(empty));
        assert(sh_package_key_set_add(&members, a) && sh_package_key_set_add(&members, b));
        assert(sh_package_key_groups_define(&author, g, &members));
        assert(sh_package_key_set_add(&unrelated, 301989888u) && sh_package_key_set_add(&unrelated, 335544320u));
        assert(sh_package_key_groups_define(&beside, g, &unrelated));
        assert(sh_package_key_set_add(&author_taken, a) && sh_package_key_set_add(&author_taken, b));
        assert(sh_package_key_set_add(&client_taken, a2) && sh_package_key_set_add(&client_taken, b2) &&
            sh_package_key_set_add(&client_taken, held_there));
        snprintf(number, sizeof(number), "%u", a);
        assert(sh_package_provenance_bind(&held, SH_PACKAGE_KEY_FAMILY, "524288", number, &held_context, 1));
        snprintf(number, sizeof(number), "%u", b);
        assert(sh_package_provenance_bind(&held, SH_PACKAGE_KEY_FAMILY, "1048576", number, &held_context, 1));
        snprintf(number, sizeof(number), "%u", a2);
        assert(sh_package_provenance_bind(&moved_keys, SH_PACKAGE_KEY_FAMILY, "524288", number, &moved_context, 1));
        snprintf(number, sizeof(number), "%u", b2);
        assert(sh_package_provenance_bind(&moved_keys, SH_PACKAGE_KEY_FAMILY, "1048576", number, &moved_context, 1));
        snprintf(valued, sizeof(valued), "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
            "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"%u\"}}}}]}", g);
        snprintf(expected, sizeof(expected), "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
            "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"%u\"}}}}]}", g2);
        sh_package_source_reader_key_codes(reader, &author, NULL, &author_taken, sh_package_map_uses_saved_keys);
        text = sh_package_map_uses_select(reader, valued, strlen(valued), NULL, &held, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &record, &references, &inspection, error, sizeof(error));
        if (!text) fprintf(stderr, "group save: %s\n", error);
        assert(text && !strcmp(text, valued) && record.use_count == 2); free(text);
        snprintf(site, sizeof(site), "%s#%u", key_site, a);
        assert(site_context(&record, "entity/0", site));
        snprintf(site, sizeof(site), "%s#%u", key_site, b);
        assert(site_context(&record, "entity/0", site));
        sh_package_source_reader_key_codes(reader, &client, &beside, &client_taken, sh_package_map_uses_saved_keys);
        text = sh_package_map_uses_select(reader, valued, strlen(valued), &record, &moved_keys, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &back, &references, &inspection, error, sizeof(error));
        if (!text || strcmp(text, expected)) fprintf(stderr, "group reopen: %s\n", text ? text : error);
        assert(sh_package_key_set_add(&now, a2) && sh_package_key_set_add(&now, b2));
        assert(text && !strcmp(text, expected) && sh_package_key_groups_find(&client, &now) == g2);
        assert(back.use_count == 2 && client.count == 1 && beside.count == 1); free(text);
        /* A saved union of ordinary bits can reopen as marked private keys.
         * Their bitwise OR is a different key, not a representation of both. */
        {
            sh_package_provenance plain_pair = {0}, low_record = {0}, raised = {0};
            size_t plain_context = isolated(&plain_pair, rows + 1);
            assert(sh_package_provenance_bind(&plain_pair, SH_PACKAGE_KEY_FAMILY, "524288", "524288", &plain_context, 1));
            assert(sh_package_provenance_bind(&plain_pair, SH_PACKAGE_KEY_FAMILY, "1048576", "1048576", &plain_context, 1));
            char low[256];
            snprintf(low, sizeof(low), "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
                "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"1572864\"}}}}]}");
            sh_package_source_reader_key_codes(reader, NULL, NULL, NULL, sh_package_map_uses_saved_keys);
            text = sh_package_map_uses_select(reader, low, strlen(low), NULL, &plain_pair, rows, 3,
                sh_mpkg_new_use_required, &requirements, &length, &low_record, &references, &inspection, error, sizeof(error));
            assert(text && !strcmp(text, low) && low_record.use_count == 2); free(text);
            sh_package_source_reader_key_codes(reader, &author, NULL, &author_taken, sh_package_map_uses_saved_keys);
            text = sh_package_map_uses_select(reader, low, strlen(low), &low_record, &held, rows, 3,
                sh_mpkg_new_use_required, &requirements, &length, &raised, &references, &inspection, error, sizeof(error));
            if (!text || strcmp(text, valued)) fprintf(stderr, "low-to-marked group: %s\n", text ? text : error);
            assert(text && !strcmp(text, valued) && raised.use_count == 2); free(text);
            sh_package_provenance_free(&plain_pair); sh_package_provenance_free(&low_record); sh_package_provenance_free(&raised);
        }
        sh_package_source_reader_key_codes(reader, &author, NULL, &author_taken, sh_package_map_uses_saved_keys);
        text = sh_package_map_uses_select(reader, expected, strlen(expected), &back, &held, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &again, &references, &inspection, error, sizeof(error));
        assert(text && !strcmp(text, valued) && again.use_count == 2 && author.count == 1); free(text);
        /* Two values at one site path in different entities: each decodes
         * from its own entity's saved members ({A, B} and {A, C}), and a
         * saved use of another family at "<site>#<n>" is no member. */
        {
            const uint32_t c = 273154048u, c2 = 402653184u, h = 469762048u;
            sh_package_key_groups client2;
            sh_package_key_set pair_ac = {{0}}, now_ab = {{0}}, now_ac = {{0}};
            sh_package_provenance two = {0}, rebound = {0};
            char both[512], rewritten[512];
            size_t entity_binding = SIZE_MAX;
            memset(&client2, 0, sizeof(client2));
            assert(sh_package_key_set_add(&pair_ac, a) && sh_package_key_set_add(&pair_ac, c));
            assert(sh_package_key_groups_define(&author, h, &pair_ac) && sh_package_key_set_add(&author_taken, c));
            assert(sh_package_key_set_add(&client_taken, c2));
            snprintf(number, sizeof(number), "%u", c);
            assert(sh_package_provenance_bind(&held, SH_PACKAGE_KEY_FAMILY, "2097152", number, &held_context, 1));
            snprintf(number, sizeof(number), "%u", c2);
            assert(sh_package_provenance_bind(&moved_keys, SH_PACKAGE_KEY_FAMILY, "2097152", number, &moved_context, 1));
            snprintf(both, sizeof(both), "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
                "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"%u\"}}}},{\"uniqueId\":43,\"entityDef\":"
                "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"%u\"}}}}]}", g, h);
            sh_package_source_reader_key_codes(reader, &author, NULL, &author_taken, sh_package_map_uses_saved_keys);
            text = sh_package_map_uses_select(reader, both, strlen(both), NULL, &held, rows, 3,
                sh_mpkg_new_use_required, &requirements, &length, &two, &references, &inspection, error, sizeof(error));
            if (!text) fprintf(stderr, "two groups save: %s\n", error);
            assert(text && !strcmp(text, both) && two.use_count == 4); free(text);
            snprintf(site, sizeof(site), "%s#%u", key_site, c);
            assert(site_context(&two, "entity/1", site));
            for (size_t i = 0; i < two.binding_count; i++)
                if (!strcmp(two.bindings[i].family, "entitydef")) entity_binding = i;
            if (entity_binding == SIZE_MAX) {
                assert(sh_package_provenance_bind(&two, "entitydef", "vanilla", "vanilla", two.uses[0].contexts, 1));
                entity_binding = two.binding_count - 1;
            }
            use(&two, "entity/0", site, entity_binding, two.uses[0].contexts[0]);
            sh_package_source_reader_key_codes(reader, &client2, NULL, &client_taken, sh_package_map_uses_saved_keys);
            text = sh_package_map_uses_select(reader, both, strlen(both), &two, &moved_keys, rows, 3,
                sh_mpkg_new_use_required, &requirements, &length, &rebound, &references, &inspection, error, sizeof(error));
            if (!text) fprintf(stderr, "two groups reopen: %s\n", error);
            assert(sh_package_key_set_add(&now_ab, a2) && sh_package_key_set_add(&now_ab, b2));
            assert(sh_package_key_set_add(&now_ac, a2) && sh_package_key_set_add(&now_ac, c2));
            snprintf(rewritten, sizeof(rewritten), "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
                "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"%u\"}}}},{\"uniqueId\":43,\"entityDef\":"
                "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"spawnType\":\"%u\"}}}}]}",
                sh_package_key_groups_find(&client2, &now_ab), sh_package_key_groups_find(&client2, &now_ac));
            if (text && strcmp(text, rewritten)) fprintf(stderr, "two groups reopen: %s\n", text);
            assert(text && client2.count == 2 && !strcmp(text, rewritten) && rebound.use_count == 4); free(text);
            sh_package_provenance_free(&two); sh_package_provenance_free(&rebound);
        }
        /* A value set in this session under a code the table beside the
         * serving one defines (the library's, while a map serves) keeps it. */
        sh_package_source_reader_key_codes(reader, &empty, &author, &author_taken, sh_package_map_uses_saved_keys);
        sh_package_provenance_free(&again);
        text = sh_package_map_uses_select(reader, valued, strlen(valued), NULL, &held, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &again, &references, &inspection, error, sizeof(error));
        assert(text && !strcmp(text, valued) && again.use_count == 2 && !empty.count); free(text);
        /* Without a group table a code no binding names stays as authored. */
        sh_package_source_reader_key_codes(reader, NULL, NULL, NULL, NULL);
        sh_package_provenance_free(&again);
        text = sh_package_map_uses_select(reader, valued, strlen(valued), NULL, &held, rows, 3,
            sh_mpkg_new_use_required, &requirements, &length, &again, &references, &inspection, error, sizeof(error));
        assert(text && !strcmp(text, valued) && !again.use_count); free(text);
        sh_package_provenance_free(&held); sh_package_provenance_free(&moved_keys); sh_package_provenance_free(&record);
        sh_package_provenance_free(&back); sh_package_provenance_free(&again);
    }
    sh_package_provenance_free(&catalog); sh_package_provenance_free(&alone); sh_package_provenance_free(&saved);
    sh_package_provenance_free(&out); sh_package_provenance_free(&reopened); sh_package_references_free(&references);
}
static int local_native(void *context, const char *family, const char *name)
{
    assert(!strcmp(family, "entitydef"));
    if (context) return -1;
    return !strcmp(name, "root") || !strcmp(name, "vanilla") || (g_native_empty && !_stricmp(name, "_emptyName"));
}
static void run_local(sh_package_source_reader *reader, sh_package_bundle_binding rows[3])
{
    sh_package_provenance library = {0};
    size_t contexts[3], length; char error[1024];
    for (size_t i = 0; i < 3; i++) contexts[i] = isolated(&library, rows + i);
    bind(&library, "root", "current-root", contexts, 2);
    bind(&library, "value", "current-value", contexts, 2);
    bind(&library, "vanilla", "vanilla-alternative", contexts + 2, 1);
    const char *json = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"root\",\"state\":{\"edit\":{\"target\":\"value\",\"text\":\"value\x97\"}}}},"
        "{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"vanilla\"}}],\"note\":\"value\x97\"}";
    const char *expected = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"root\",\"state\":{\"edit\":{\"target\":\"current-value\",\"text\":\"value\x97\"}}}},"
        "{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"vanilla\"}}],\"note\":\"value\x97\"}";
    char *out = sh_package_map_uses_bind_local(reader, json, strlen(json), &library,
        local_native, NULL, &length, error, sizeof(error));
    if (!out) fprintf(stderr, "local bind: %s\n", error);
    assert(out && length == strlen(expected) && !strcmp(out, expected)); free(out);
    /* Ambiguous old names have a stable choice, independent of discovery order.
     * Exact equal delivery owners already share one compiled name. */
    bind(&library, "value", "new-root", contexts + 2, 1);
    out = sh_package_map_uses_bind_local(reader, json, strlen(json), &library,
        local_native, NULL, &length, error, sizeof(error));
    assert(out);
    sh_package_provenance_binding swap = library.bindings[1];
    library.bindings[1] = library.bindings[3]; library.bindings[3] = swap;
    char *again = sh_package_map_uses_bind_local(reader, json, strlen(json), &library,
        local_native, NULL, &length, error, sizeof(error));
    assert(again && !strcmp(out, again)); free(out); free(again);
    out = sh_package_map_uses_bind_local(reader, json, strlen(json), &library,
        local_native, &library, &length, error, sizeof(error));
    assert(!out && !length);
    /* Native strings alone never select an installed resource. */
    json = "{\"note\":\"value\x97\",\"entities\":[]}";
    out = sh_package_map_uses_bind_local(reader, json, strlen(json), &library,
        local_native, &library, &length, error, sizeof(error));
    assert(out && !strcmp(out, json)); free(out);
    /* M102: a map's "" in a declaration-pointer field is the lookup of
     * _emptyName at that site. A package's own _emptyName, served privately,
     * is the identity it selects, written over the "" itself; an ordinary
     * string "" stays data. A stock _emptyName keeps the native value as any
     * native name does here, and with none anywhere "" stays for the
     * manager's own default. JSON null and "NULL" keep naming nothing. */
    {
        sh_package_provenance empty = {0};
        size_t empty_contexts[3];
        for (size_t i = 0; i < 3; i++) empty_contexts[i] = isolated(&empty, rows + i);
        bind(&empty, "root", "current-root", empty_contexts, 2);
        const char *map = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\","
            "\"state\":{\"edit\":{\"target\":\"\",\"text\":\"\"}}}}]}";
        const char *bound = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\","
            "\"state\":{\"edit\":{\"target\":\"zz-empty\",\"text\":\"\"}}}}]}";
        out = sh_package_map_uses_bind_local(reader, map, strlen(map), &empty, local_native, NULL, &length, error, sizeof(error));
        if (!out) fprintf(stderr, "local empty absent: %s\n", error);
        assert(out && !strcmp(out, map)); free(out);
        bind(&empty, "_emptyName", "zz-empty", empty_contexts, 2);
        out = sh_package_map_uses_bind_local(reader, map, strlen(map), &empty, local_native, NULL, &length, error, sizeof(error));
        if (!out || strcmp(out, bound)) fprintf(stderr, "local empty supplied: %s\n", out ? out : error);
        assert(out && length == strlen(bound) && !strcmp(out, bound)); free(out);
        g_native_empty = 1;
        out = sh_package_map_uses_bind_local(reader, map, strlen(map), &empty, local_native, NULL, &length, error, sizeof(error));
        if (!out || strcmp(out, map)) fprintf(stderr, "local empty native: %s\n", out ? out : error);
        assert(out && !strcmp(out, map)); free(out);
        g_native_empty = 0;
        static const char *const unnamed[] = {
            "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\",\"state\":{\"edit\":{\"target\":null}}}}]}",
            "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\",\"state\":{\"edit\":{\"target\":\"NULL\"}}}}]}"};
        for (size_t i = 0; i < 2; i++) {
            out = sh_package_map_uses_bind_local(reader, unnamed[i], strlen(unnamed[i]), &empty, local_native, NULL,
                &length, error, sizeof(error));
            assert(out && !strcmp(out, unnamed[i])); free(out);
        }
        /* A failed lookup leaves the caller's map: nothing partial is returned. */
        out = sh_package_map_uses_bind_local(reader, map, strlen(map), &empty, local_native, &empty, &length, error, sizeof(error));
        assert(!out && !length);
        sh_package_provenance_free(&empty);
    }
    library.semantics = SH_PACKAGE_PROVENANCE_USES;
    out = sh_package_map_uses_bind_local(reader, json, strlen(json), &library,
        local_native, NULL, &length, error, sizeof(error));
    assert(!out && !length);
    sh_package_provenance_free(&library);
}
/* A saved or historical row that now resolves to the native identity it names
 * is vanilla unless the current provider still supplies that identity (a
 * composition published under the stock name). The site keeps its value. */
static void run_native_identity(sh_package_source_reader *reader, sh_package_compilation *compiled,
    sh_package_bundle_binding rows[3])
{
    const char *json = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\"}}]}";
    sh_package_provenance catalog = {0}, saved = {0}, out = {0};
    sh_package_references references = {0};
    sh_package_source_binding inspection; size_t length; char error[1024];
    sh_mpkg_use_requirements requirements = {compiled, NULL};
    size_t context = isolated(&catalog, rows), saved_context = isolated(&saved, rows);
    bind(&catalog, "root", "root", &context, 1);
    bind(&saved, "root", "root", &saved_context, 1);
    use(&saved, "entity/0", root_site, 0, saved_context);
    char *text = sh_package_map_uses_select(reader, json, strlen(json), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "native identity: %s\n", error);
    assert(text && !strcmp(text, json) && !out.use_count); free(text);
    /* Published under that stock name by this provider: still a package use. */
    sh_compiled_resource *files = compiled->resources; size_t count = compiled->resource_count;
    sh_compiled_resource *more = malloc((count + 1) * sizeof(*more)); assert(more);
    memcpy(more, files, count * sizeof(*more));
    more[count] = declaration("root", "generated/decls/entitydef/root.decl", "{ class = \"Entity\"; edit = { health = 3; } }");
    compiled->resources = more; compiled->resource_count = count + 1;
    text = sh_package_map_uses_select(reader, json, strlen(json), &saved, &catalog, rows, 3,
        sh_mpkg_new_use_required, &requirements, &length, &out, &references, &inspection, error, sizeof(error));
    assert(text && !strcmp(text, json) && out.use_count == 1 && !strcmp(out.bindings[0].name, "root")); free(text);
    compiled->resources = files; compiled->resource_count = count; free(more);
    sh_package_provenance_free(&catalog); sh_package_provenance_free(&saved); sh_package_provenance_free(&out);
    sh_package_references_free(&references);
}
/* A cooked md6Anim header naming its skeleton. */
static size_t cooked_anim(unsigned char *out, const char *skeleton)
{
    size_t n = strlen(skeleton);
    memcpy(out, "\x26\x02\x41\x4d", 4); memset(out + 4, 0, 8);
    out[12] = (unsigned char)n; out[13] = (unsigned char)(n >> 8); out[14] = 0; out[15] = 0;
    memcpy(out + 16, skeleton, n); memset(out + 16 + n, 0, 32); return 16 + n + 32;
}
/* The event argument classes: idImage's generic resource pointer reader also
 * reads idMD6Anim, and Entity gains an idEventCallInfo field. */
static void event_setup(fixture *f)
{
    static const char *added[] = {"idImage", "idMD6Anim", "idEventCallInfo"};
    setup(f);
    void *container = *(void **)f->reflection, *old = *(void **)((char *)container + 0x20);
    void *records = allocation(f, 7 * 0x38), *objects = allocation(f, 7 * 16), *pointers = allocation(f, 7 * 16);
    void *fields = allocation(f, 5 * 0x48);
    memcpy(records, old, 3 * 0x38);
    memcpy(objects, *(void **)((char *)f->reflection + 0x88), 3 * 16);
    memcpy(pointers, *(void **)((char *)f->reflection + 0xa0), 3 * 16);
    memcpy(fields, *(void **)((char *)old + 0x38 + 0x20), 3 * 0x48);
    for (size_t i = 0; i < 3; i++) {
        pointer(records, (3 + i) * 0x38, string(f, added[i])); pointer(records, (3 + i) * 0x38 + 8, string(f, ""));
    }
    pointer(pointers, 3 * 16 + 8, (void *)0x40); pointer(pointers, 4 * 16 + 8, (void *)0x40);
    pointer(objects, 5 * 16 + 8, (void *)0xa0);
    pointer(fields, 3 * 0x48, string(f, added[2])); pointer(fields, 3 * 0x48 + 8, string(f, ""));
    pointer(fields, 3 * 0x48 + 0x10, string(f, "eventCall")); number(fields, 3 * 0x48 + 0x1c, 8);
    pointer(records, 0x38 + 0x20, fields); pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, 7);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, 7);
}
/* One compiled scope supplying a declaration and a cooked clip. */
typedef struct event_scope {
    sh_compiled_resource files[2]; unsigned char cooked[128]; sh_package_sources sources;
    sh_package_compilation compiled; sh_mpkg_use_requirements requirements; sh_package_source_reader *reader;
    size_t clip;
} event_scope;
static void event_scope_open(event_scope *s, fixture *f, const char *decl, const char *path, const char *clip)
{
    char error[1024];
    memset(s, 0, sizeof(*s));
    sh_compiled_resource anim = {0};
    anim.engine_path = sh_package_cooked_path("anim", clip); assert(anim.engine_path);
    anim.type = anim.native_type = "anim"; anim.name = anim.native_name = (char *)clip;
    anim.body = s->cooked; anim.body_length = cooked_anim(s->cooked, "md6/m21/m21.md6skl");
    anim.source = SIZE_MAX; anim.gameplay_owners.bits = 7;
    /* Compilations are sorted by engine path. */
    s->clip = strcmp(anim.engine_path, path) < 0 ? 0 : 1;
    s->files[s->clip] = anim; s->files[!s->clip] = declaration(decl, path, "{ class = \"Entity\"; edit = {} }");
    s->compiled.sources = &s->sources; s->compiled.resources = s->files; s->compiled.resource_count = 2;
    s->requirements.compiled = &s->compiled;
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    s->reader = sh_package_source_reader_open(&s->compiled, NULL, baseline, NULL, registry,
        (uintptr_t)f->reflection, error, sizeof(error));
    if (!s->reader) fprintf(stderr, "event reader: %s\n", error);
    assert(s->reader);
}
/* Rows whose mask bit is set supply both resources under these current names. */
static void event_catalog(sh_package_provenance *catalog, const sh_package_bundle_binding *rows, size_t count,
    unsigned decl_mask, unsigned clip_mask, const char *decl, const char *clip)
{
    size_t contexts[3], d[3], c[3], dn = 0, cn = 0;
    sh_package_provenance_free(catalog); memset(catalog, 0, sizeof(*catalog));
    for (size_t i = 0; i < count; i++) {
        contexts[i] = isolated(catalog, rows + i);
        if (decl_mask >> i & 1) d[dn++] = contexts[i];
        if (clip_mask >> i & 1) c[cn++] = contexts[i];
    }
    if (dn) bind(catalog, "m21-decl", decl, d, dn);
    if (cn) assert(sh_package_provenance_bind(catalog, "anim", "md6/m21/clip.md6anim", clip, c, cn));
}
/* The original row (0-2) whose exact bundle a use's single context names. */
static size_t event_row(const sh_package_provenance *value, const char *family, const sh_package_bundle_binding rows[3])
{
    const sh_package_provenance_use *found = NULL;
    for (size_t i = 0; i < value->use_count; i++)
        if (!strcmp(value->bindings[value->uses[i].binding].family, family)) { assert(!found); found = value->uses + i; }
    assert(found && found->context_count == 1);
    const sh_package_provenance_context *context = value->contexts + found->contexts[0];
    assert(context->tree_count == 1);
    for (size_t i = 0; i < 3; i++)
        if (!memcmp(context->trees[0], sh_package_bundle_fingerprint(rows[i].original), 32)) return i;
    return SIZE_MAX;
}
/* Each delivery repacks to its exact original authored archive bytes. */
static void event_deliveries(const sh_mpkg_used *deliveries, size_t count, const sh_package_bundle_binding *set,
    const sh_package_bundle_binding rows[3], unsigned char *archives[3], const size_t lengths[3], unsigned expected)
{
    unsigned seen = 0; char error[512];
    for (size_t i = 0; i < count; i++) {
        size_t original = 3, n;
        for (size_t r = 0; r < 3; r++)
            if (!memcmp(sh_package_bundle_fingerprint(deliveries[i].bundle), sh_package_bundle_fingerprint(rows[r].original), 32))
                original = r;
        assert(original < 3 && !(seen >> original & 1)); seen |= 1u << original;
        int listed = 0;
        for (size_t r = 0; r < 3; r++) listed |= deliveries[i].bundle == set[r].original;
        unsigned char *bytes = sh_mpkg_pack_used(deliveries + i, &n, error, sizeof(error));
        assert(listed && bytes && n == lengths[original] && !memcmp(bytes, archives[original], n));
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    assert(seen == expected);
}
static int event_native(void *context, const char *family, const char *name)
{ (void)context; return !strcmp(family, "entitydef") && !strcmp(name, "vanilla"); }
static const char event_map[] = "{\"entities\":[{\"uniqueId\":7,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
    "\"inherit\":\"vanilla\",\"state\":{\"edit\":{\"eventCall\":{\"eventDef\":\"playAnim\",\"args\":{\"num\":4,"
    "\"item[0]\":{\"decl\":{\"entityDef\":\"%s\"}},\"item[1]\":{\"md6Anim\":\"%s\"},"
    "\"item[2]\":{\"decl\":{\"entityDef\":\"absent/decl\"}},\"item[3]\":{\"md6Anim\":\"md6/m21/absent.md6anim\"}}}}}}}]}";

/* The map's own eventCall decl and md6Anim arguments through the real typed
 * source graph and map-use selection: capture of exact suppliers, the encoded
 * saved document, a reload whose equal-ID, equal-name competing suppliers are
 * reordered and recompiled under new names, intact original delivery bytes, an
 * admitted embedded copy replacing a removed installed package, a truly
 * missing source with preserved outputs, and recovery. Unbound arguments the
 * engine defaults are never captured uses. */
static void run_events(sh_package_bundle_binding rows[3], unsigned char *archives[3], const size_t lengths[3])
{
    fixture f = {0}; event_scope author, reload;
    const char *clip = "md6/m21/current-clip.md6anim", *moved_clip = "md6/m21/moved-clip.md6anim";
    char authored[1024], expected[1024], unbound[1024], error[1024];
    sh_package_provenance catalog = {0}, none = {0}, captured = {0}, saved = {0}, out = {0}, fresh = {0};
    sh_package_references references = {0}, resources = {0};
    sh_package_source_binding inspection; size_t length; sh_mpkg_used *deliveries = NULL;
    event_setup(&f);
    event_scope_open(&author, &f, "current-decl", "generated/decls/entitydef/current-decl.decl", clip);
    event_scope_open(&reload, &f, "moved-decl", "generated/decls/entitydef/moved-decl.decl", moved_clip);
    snprintf(authored, sizeof(authored), event_map, "current-decl", clip);
    snprintf(expected, sizeof(expected), event_map, "moved-decl", moved_clip);
    snprintf(unbound, sizeof(unbound), event_map, "absent/other", "md6/m21/other.md6anim");

    /* Authoring: row 1 alone supplies the declaration and row 0 alone the clip. */
    event_catalog(&catalog, rows, 3, 2, 1, "current-decl", clip);
    char *text = sh_package_map_uses_select(author.reader, authored, strlen(authored), &none, &catalog, rows, 3,
        sh_mpkg_new_use_required, &author.requirements, &length, &captured, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "event capture: %s\n", error);
    assert(text && !strcmp(text, authored) && !inspection.changed); free(text);
    for (size_t i = 0; i < captured.use_count; i++)
        fprintf(stderr, "event use: %s %s -> %s at %s\n", captured.bindings[captured.uses[i].binding].family,
            captured.bindings[captured.uses[i].binding].original, captured.bindings[captured.uses[i].binding].name,
            captured.uses[i].reference);
    assert(captured.use_count == 2 && event_row(&captured, "entitydef", rows) == 1 && event_row(&captured, "anim", rows) == 0);
    for (size_t i = 0; i < 2; i++) assert(strstr(captured.uses[i].reference, "eventCall"));
    /* Saving writes the canonical encoding; reloading decodes it. */
    unsigned char *encoded = sh_package_provenance_encode(&captured, &length, error, sizeof(error));
    assert(encoded && sh_package_provenance_decode(encoded, length, &saved, error, sizeof(error)) && saved.use_count == 2);
    free(encoded);

    /* Reload: every row now supplies both resources under new compiled names,
     * in reverse order. A fresh selection picks one supplier for both, so the
     * distinct saved suppliers below come only from the saved sites. */
    sh_package_bundle_binding reordered[3] = {rows[2], rows[1], rows[0]};
    event_catalog(&catalog, reordered, 3, 7, 7, "moved-decl", moved_clip);
    text = sh_package_map_uses_select(reload.reader, expected, strlen(expected), &none, &catalog, reordered, 3,
        sh_mpkg_new_use_required, &reload.requirements, &length, &fresh, &references, &inspection, error, sizeof(error));
    assert(text && fresh.use_count == 2 && event_row(&fresh, "entitydef", rows) == event_row(&fresh, "anim", rows)); free(text);
    text = sh_package_map_uses_select(reload.reader, authored, strlen(authored), &saved, &catalog, reordered, 3,
        sh_mpkg_new_use_required, &reload.requirements, &length, &out, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "event reload: %s\n", error);
    assert(text && length == strlen(expected) && !strcmp(text, expected) && inspection.changed == 2);
    assert(out.use_count == 2 && event_row(&out, "entitydef", rows) == 1 && event_row(&out, "anim", rows) == 0);
    size_t count = sh_mpkg_used_packages_for_uses(&reload.compiled, NULL, text, length, &references, &out,
        reordered, 3, &deliveries, &resources, error, sizeof(error));
    if (count == SIZE_MAX) fprintf(stderr, "event deliveries: %s\n", error);
    assert(count == 2); event_deliveries(deliveries, count, reordered, rows, archives, lengths, 3);
    sh_mpkg_used_free(deliveries); deliveries = NULL; free(text);

    /* The installed package of row 1 is removed while the map's embedded copy
     * of that exact authored bundle is admitted: the map stays usable. */
    sh_package_bundle_binding admitted[3] = {rows[2], {0}, rows[0]};
    snprintf(admitted[1].source_name, sizeof(admitted[1].source_name), "embedded");
    admitted[1].original = sh_package_bundle_from_archive(archives[1], lengths[1], root, error, sizeof(error));
    if (!admitted[1].original) fprintf(stderr, "embedded bundle: %s\n", error);
    assert(admitted[1].original && admitted[1].original != rows[1].original && !memcmp(
        sh_package_bundle_fingerprint(admitted[1].original), sh_package_bundle_fingerprint(rows[1].original), 32));
    memcpy(admitted[1].compiler_tree, sh_package_bundle_fingerprint(admitted[1].original), 32);
    event_catalog(&catalog, admitted, 3, 7, 7, "moved-decl", moved_clip);
    text = sh_package_map_uses_select(reload.reader, authored, strlen(authored), &saved, &catalog, admitted, 3,
        sh_mpkg_new_use_required, &reload.requirements, &length, &out, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "event embedded: %s\n", error);
    assert(text && !strcmp(text, expected) && event_row(&out, "entitydef", rows) == 1 && event_row(&out, "anim", rows) == 0);
    count = sh_mpkg_used_packages_for_uses(&reload.compiled, NULL, text, length, &references, &out,
        admitted, 3, &deliveries, &resources, error, sizeof(error));
    assert(count == 2); event_deliveries(deliveries, count, admitted, rows, archives, lengths, 3);
    sh_mpkg_used_free(deliveries); deliveries = NULL; free(text);

    /* Truly missing: one use's exact source is in no admitted route while the
     * other rows still supply the same names. The declaration's row 1 and the
     * clip's row 0 are each removed alone, so the declaration branch and the
     * separate md6Anim default branch must both refuse. No supplier is
     * substituted and the previous outputs survive. A stale catalog naming the
     * removed row fails delivery. */
    for (int gone = 1; gone >= 0; gone--) {
        sh_package_bundle_binding missing[2] = {rows[2], rows[1 - gone]};
        const char *use = gone ? "entitydef m21-decl" : "anim md6/m21/clip.md6anim";
        for (int stale = 0; stale < 2; stale++) {
            if (stale) event_catalog(&catalog, reordered, 3, 7, 7, "moved-decl", moved_clip);
            else event_catalog(&catalog, missing, 2, 3, 3, "moved-decl", moved_clip);
            text = sh_package_map_uses_select(reload.reader, authored, strlen(authored), &saved, &catalog, missing, 2,
                sh_mpkg_new_use_required, &reload.requirements, &length, &out, &references, &inspection, error, sizeof(error));
            fprintf(stderr, "event missing row %d %d: %s\n", gone, stale, error);
            assert(!text && !length && (stale ? !!strstr(error, "no exact retained original delivery") :
                !!strstr(error, "cannot resolve its saved source context") && !!strstr(error, use)));
            assert(out.use_count == 2 && event_row(&out, "entitydef", rows) == 1 && event_row(&out, "anim", rows) == 0);
        }
        /* The exact source returns: the same map recovers unchanged. */
        text = sh_package_map_uses_select(reload.reader, authored, strlen(authored), &saved, &catalog, reordered, 3,
            sh_mpkg_new_use_required, &reload.requirements, &length, &out, &references, &inspection, error, sizeof(error));
        assert(text && !strcmp(text, expected) && event_row(&out, "entitydef", rows) == 1 && event_row(&out, "anim", rows) == 0);
        free(text);
    }

    /* Arguments no source supplies stay engine defaults with no source rows
     * at all, distinct from the captured required uses above. */
    event_catalog(&catalog, rows, 0, 0, 0, "", "");
    text = sh_package_map_uses_select(reload.reader, unbound, strlen(unbound), &none, &catalog, NULL, 0,
        sh_mpkg_new_use_required, &reload.requirements, &length, &fresh, &references, &inspection, error, sizeof(error));
    if (!text) fprintf(stderr, "event unbound: %s\n", error);
    assert(text && !strcmp(text, unbound) && !fresh.use_count && !inspection.changed); free(text);

    /* A plain map naming the authored originals binds through the installed
     * library only while it has bindings (the runtime's rebind predicate). */
    char plain[1024]; snprintf(plain, sizeof(plain), event_map, "m21-decl", "md6/m21/clip.md6anim");
    event_catalog(&catalog, rows, 3, 2, 1, "current-decl", clip);
    text = sh_package_map_uses_bind_local(author.reader, plain, strlen(plain), &catalog,
        event_native, NULL, &length, error, sizeof(error));
    if (!text) fprintf(stderr, "event local: %s\n", error);
    fprintf(stderr, "event local: %s\n", text ? text : "(none)");
    assert(text && !strcmp(text, authored)); free(text);
    event_catalog(&catalog, rows, 0, 0, 0, "", "");
    text = sh_package_map_uses_bind_local(author.reader, plain, strlen(plain), &catalog,
        event_native, NULL, &length, error, sizeof(error));
    assert(text && !strcmp(text, plain)); free(text);

    sh_package_bundle_release(admitted[1].original);
    sh_package_provenance_free(&catalog); sh_package_provenance_free(&captured); sh_package_provenance_free(&saved);
    sh_package_provenance_free(&out); sh_package_provenance_free(&fresh);
    sh_package_references_free(&references); sh_package_references_free(&resources);
    sh_package_source_reader_close(author.reader); sh_package_source_reader_close(reload.reader);
    free(author.files[author.clip].engine_path); free(reload.files[reload.clip].engine_path);
    while (f.count) free(f.regions[--f.count].address);
}

/* D4 selection: a context used only through a nested component asks for a
 * component delivery; without a retained cache root or a compiler inventory
 * naming the component it falls back to the whole tree, which stays the
 * valid delivery. The enclosing-file listing itself is checked on a synthetic
 * inventory: same owner, outside the component, unique, closure order. */
static void run_component(sh_package_compilation *compiled, sh_package_bundle_binding rows[3])
{
    sh_package_provenance uses = {0};
    sh_package_references references = {0}, resources = {0};
    sh_mpkg_used *deliveries = NULL;
    char error[512], key[SH_PACKAGE_BUNDLE_KEY_CAP];
    size_t nested = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_ISOLATED,
        (const unsigned char (*)[32])sh_package_bundle_fingerprint(rows[0].original), 1, "editor");
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/0", root_site};
    static const char text[] = "{\"entities\":[]}";
    assert(nested != SIZE_MAX);
    bind(&uses, "root", "current-root", &nested, 1);
    assert(sh_package_provenance_use_set(&uses, &site, 0, &nested, 1));
    uses.semantics = SH_PACKAGE_PROVENANCE_USES;
    sh_package_bundle_delivery_key(rows[0].original, key);
    /* No cache root: whole tree. */
    assert(sh_mpkg_used_packages_for_uses(compiled, NULL, text, strlen(text), &references, &uses,
        rows, 3, &deliveries, &resources, error, sizeof(error)) == 1);
    assert(deliveries && !deliveries[0].nested && !strcmp(deliveries[0].id, key) && deliveries[0].bundle == rows[0].original);
    sh_mpkg_used_free(deliveries); deliveries = NULL;
    /* A cache root but no inventory naming the component: whole tree. */
    sh_package_bundle_set_cache_root(root);
    assert(sh_mpkg_used_packages_for_uses(compiled, NULL, text, strlen(text), &references, &uses,
        rows, 3, &deliveries, &resources, error, sizeof(error)) == 1);
    assert(deliveries && !deliveries[0].nested && !strcmp(deliveries[0].id, key));
    sh_mpkg_used_free(deliveries); deliveries = NULL;
    /* The same tree used whole and through a component travels whole, once. */
    {
        size_t whole = isolated(&uses, rows);
        sh_package_provenance_site other = {SH_PACKAGE_USE_OBJECT, "entity/1", root_site};
        bind(&uses, "value", "current-value", &whole, 1);
        assert(sh_package_provenance_use_set(&uses, &other, 1, &whole, 1));
        assert(sh_mpkg_used_packages_for_uses(compiled, NULL, text, strlen(text), &references, &uses,
            rows, 3, &deliveries, &resources, error, sizeof(error)) == 1);
        assert(deliveries && !deliveries[0].nested && !strcmp(deliveries[0].id, key));
        sh_mpkg_used_free(deliveries); deliveries = NULL;
    }
    sh_package_bundle_set_cache_root(NULL);
    sh_package_provenance_free(&uses); sh_package_references_free(&resources);
    /* The enclosing files of a component's closure. */
    {
        sh_package_sources inventory = {0};
        sh_package_source_file files[6] = {0};
        sh_compiled_resource fixture_resources[3] = {0};
        size_t first[] = {0, 1, 2}, second[] = {3, 1}, third[] = {4, 5};
        sh_package_compilation view = {0};
        sh_package_references closure = {0};
        char **enclosing = NULL; size_t count = 0;
        files[0].owner = 0; files[0].component = 1; files[0].relative = "cyberdemon/assets/a.decl"; files[0].engine_path = "generated/a.decl";
        files[1].owner = 0; files[1].component = 0; files[1].relative = "assets/shared.decl"; files[1].engine_path = "generated/shared.decl";
        files[2].owner = 1; files[2].component = 0; files[2].relative = "assets/shared.decl"; files[2].engine_path = "generated/shared.decl";
        files[3].owner = 0; files[3].component = 0; files[3].relative = "assets/images/shared.bimage"; files[3].engine_path = "generated/images/shared.bimage";
        files[4].owner = 0; files[4].component = 0; files[4].relative = "assets/dir"; files[4].directory = 1;
        files[5].owner = 0; files[5].component = 2; files[5].relative = "hell_guard/assets/b.decl"; files[5].engine_path = "generated/b.decl";
        inventory.files = files; inventory.file_count = 6;
        /* compilation_find searches resources sorted by engine path. */
        fixture_resources[0].engine_path = "generated/a.decl"; fixture_resources[0].sources = first; fixture_resources[0].source_count = 3;
        fixture_resources[1].engine_path = "generated/b.decl"; fixture_resources[1].sources = third; fixture_resources[1].source_count = 2;
        fixture_resources[2].engine_path = "generated/images/shared.bimage"; fixture_resources[2].sources = second; fixture_resources[2].source_count = 2;
        view.sources = &inventory; view.resources = fixture_resources; view.resource_count = 3;
        assert(sh_package_references_add(&closure, "", "generated/a.decl"));
        assert(sh_package_references_add(&closure, "", "generated/images/shared.bimage"));
        assert(sh_package_references_add(&closure, "", "generated/missing.decl"));
        assert(sh_package_references_add(&closure, "", "generated/b.decl"));
        assert(sh_mpkg_component_enclosing_files(&view, 0, "cyberdemon", &closure, &enclosing, &count));
        assert(count == 3 && !strcmp(enclosing[0], "assets/shared.decl") &&
            !strcmp(enclosing[1], "assets/images/shared.bimage") && !strcmp(enclosing[2], "hell_guard/assets/b.decl"));
        sh_mpkg_strings_free(enclosing, count); enclosing = NULL; count = 0;
        /* Another owner's copy of the shared path and directories never count. */
        assert(sh_mpkg_component_enclosing_files(&view, 1, "cyberdemon", &closure, &enclosing, &count) && count == 1);
        assert(!strcmp(enclosing[0], "assets/shared.decl"));
        sh_mpkg_strings_free(enclosing, count);
        sh_package_references_free(&closure);
    }
}

/* The boss-demons layout (live, M133): an outer descriptor without assets and
 * three components, each shipping its own copy of the stock AI conductor whose
 * aiTypeList registers its demon; a standalone package ships another copy. A
 * map placing one component's encounter has an isolated use in that component
 * and a shared-registration use whose context is the composition of every
 * copy. Exactly that component travels, as its own delivery; the composition
 * adds nothing, so neither the whole outer tree nor the standalone package
 * travels. A historical use of the same composition keeps its whole trees. */
static size_t layout_file(const sh_package_sources *sources, size_t owner, const char *relative)
{
    for (size_t i = 0; i < sources->file_count; i++)
        if (sources->files[i].owner == owner && !strcmp(sources->files[i].relative, relative)) return i;
    assert(0); return 0;
}
static void run_layout(void)
{
    static const char conductor[] = "assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl";
    static const char encounter[] = "assets/generated/decls/entitydef/snapmaps/placeablesnapaiencounter/hell_guard_hammer.decl";
    static const char text[] = "{\"entities\":[]}";
    static const char key_site[] = "/k:entityDef/k:state/k:edit/k:spawnType#1024";
    sh_package_sources *sources;
    sh_package_bundle_binding rows[2] = {0};
    sh_package_compilation view = {0};
    sh_compiled_resource fixture_resources[2] = {0};
    sh_package_provenance uses = {0}, history = {0};
    sh_package_references references = {0}, resources = {0};
    sh_mpkg_used *deliveries = NULL;
    sh_package_provenance_site entity = {SH_PACKAGE_USE_OBJECT, "entity/0", root_site};
    sh_package_provenance_site key = {SH_PACKAGE_USE_OBJECT, "entity/0", key_site};
    sh_package_provenance_site world = {SH_PACKAGE_USE_WORLD, "world", "/k:conductor"};
    sh_package_provenance_site inseparable = {SH_PACKAGE_USE_HISTORICAL, "", ""};
    size_t outer, standalone, conductor_sources[4], encounter_source[1], isolated_context, composed_context, both[2];
    unsigned char composed_trees[2][32];
    char error[512], outer_key[SH_PACKAGE_BUNDLE_KEY_CAP], standalone_key[SH_PACKAGE_BUNDLE_KEY_CAP];
    create("overrides/boss-layout/package.json", "{\"id\":\"local.boss-layout\",\"name\":\"Boss layout\"}");
    create("overrides/boss-layout/cyberdemon/package.json", "{\"id\":\"local.cyberdemon\",\"name\":\"Cyberdemon (Boss)\"}");
    create("overrides/boss-layout/cyberdemon/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 1; item[0] = { spawnType = \"CYBERDEMON\"; entityDef = \"ai/demon/cyberdemon\"; } } } }");
    create("overrides/boss-layout/cyberdemon/assets/generated/decls/entitydef/ai/demon/cyberdemon.decl", "{ edit = { health = 7000; } }");
    create("overrides/boss-layout/hell_guard/package.json", "{\"id\":\"local.hell-guard\",\"name\":\"Hell Guards\"}");
    create("overrides/boss-layout/hell_guard/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 1; item[0] = { spawnType = \"HELL_GUARD\"; entityDef = \"ai/demon/hell_guard\"; } } } }");
    create("overrides/boss-layout/hell_guard/assets/generated/decls/entitydef/snapmaps/placeablesnapaiencounter/hell_guard_hammer.decl",
        "{ edit = { encounter = \"single_hell_guard_hammer\"; } }");
    create("overrides/boss-layout/hell_guard/assets/generated/decls/snapeditorentitydef/demons/hell_guard_hammer_enc.decl", "{ edit = { toybox = 1; } }");
    create("overrides/boss-layout/spider_mastermind/package.json", "{\"id\":\"local.spider-mastermind\",\"name\":\"Spider Mastermind\"}");
    create("overrides/boss-layout/spider_mastermind/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 1; item[0] = { spawnType = \"SPIDER\"; entityDef = \"ai/demon/spider_mastermind\"; } } } }");
    create("overrides/cyberdemon-copy/package.json", "{\"id\":\"local.cyberdemon\",\"name\":\"cyberdemon\"}");
    create("overrides/cyberdemon-copy/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl",
        "{ edit = { aiTypeList = { num = 1; item[0] = { spawnType = \"CYBERDEMON\"; entityDef = \"ai/demon/cyberdemon_hell\"; } } } }");
    sources = sh_package_sources_scan(root, error, sizeof(error));
    if (!sources) fprintf(stderr, "layout scan: %s\n", error);
    assert(sources && sources->package_count == 2);
    outer = strcmp(sources->packages[0].name, "boss-layout") ? 1 : 0; standalone = 1 - outer;
    for (size_t i = 0; i < 2; i++) {
        size_t owner = i ? standalone : outer;
        snprintf(rows[i].source_name, sizeof(rows[i].source_name), "%s", sources->packages[owner].name);
        memcpy(rows[i].compiler_tree, sources->fingerprints[owner], 32);
        rows[i].original = sh_package_bundle_capture(sources, owner, root, error, sizeof(error));
        assert(rows[i].original && !memcmp(sh_package_bundle_fingerprint(rows[i].original), sources->fingerprints[owner], 32));
    }
    sh_package_bundle_delivery_key(rows[0].original, outer_key);
    sh_package_bundle_delivery_key(rows[1].original, standalone_key);
    /* The compiled view: the encounter from the hell_guard component, and the
     * conductor composed from all four copies (sorted by engine path). */
    conductor_sources[0] = layout_file(sources, outer, "cyberdemon/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl");
    conductor_sources[1] = layout_file(sources, outer, "hell_guard/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl");
    conductor_sources[2] = layout_file(sources, outer, "spider_mastermind/assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl");
    conductor_sources[3] = layout_file(sources, standalone, conductor);
    encounter_source[0] = layout_file(sources, outer, "hell_guard/assets/generated/decls/entitydef/snapmaps/placeablesnapaiencounter/hell_guard_hammer.decl");
    fixture_resources[0].engine_path = "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl";
    fixture_resources[0].sources = conductor_sources; fixture_resources[0].source_count = 4;
    fixture_resources[0].baseline_known = 1; /* SnapMap ships the conductor: every copy shadows it */
    fixture_resources[1].engine_path = "generated/decls/entitydef/snapmaps/placeablesnapaiencounter/hell_guard_hammer.decl";
    fixture_resources[1].sources = encounter_source; fixture_resources[1].source_count = 1;
    view.sources = sources; view.resources = fixture_resources; view.resource_count = 2;
    assert(encounter_source[0] < sources->file_count && strstr(sources->files[encounter_source[0]].relative, encounter + 7));
    /* The map's uses: the encounter and its registration key in the hell_guard
     * component; the shared registration list as the composition of both trees. */
    isolated_context = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_ISOLATED,
        (const unsigned char (*)[32])sh_package_bundle_fingerprint(rows[0].original), 1, "hell_guard");
    memcpy(composed_trees[0], sh_package_bundle_fingerprint(rows[0].original), 32);
    memcpy(composed_trees[1], sh_package_bundle_fingerprint(rows[1].original), 32);
    composed_context = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_COMPOSED,
        (const unsigned char (*)[32])composed_trees, 2, NULL);
    assert(isolated_context != SIZE_MAX && composed_context != SIZE_MAX);
    bind(&uses, "snapmaps/placeablesnapaiencounter/hell_guard_hammer", "snapmaps/placeablesnapaiencounter/hell_guard_hammer", &isolated_context, 1);
    assert(sh_package_provenance_bind(&uses, SH_PACKAGE_KEY_FAMILY, "1024", "268435456", &isolated_context, 1));
    bind(&uses, "ai/conductor/coop/snapmap_default", "ai/conductor/coop/snapmap_default", &composed_context, 1);
    assert(sh_package_provenance_use_set(&uses, &entity, 0, &isolated_context, 1));
    assert(sh_package_provenance_use_set(&uses, &key, 1, &isolated_context, 1));
    assert(sh_package_provenance_use_set(&uses, &world, 2, &composed_context, 1));
    uses.semantics = SH_PACKAGE_PROVENANCE_USES;
    sh_package_bundle_set_cache_root(root);
    assert(sh_mpkg_used_packages_for_uses(&view, NULL, text, strlen(text), &references, &uses,
        rows, 2, &deliveries, &resources, error, sizeof(error)) == 1);
    assert(deliveries && deliveries[0].nested && !strcmp(deliveries[0].component, "hell_guard"));
    assert(!memcmp(deliveries[0].source_tree, sh_package_bundle_fingerprint(rows[0].original), 32));
    assert(strcmp(deliveries[0].id, outer_key) && strcmp(deliveries[0].id, standalone_key));
    assert(!strcmp(deliveries[0].name, "Hell Guards"));
    {
        size_t length = 0, regular = 0;
        unsigned char *bytes = sh_package_bundle_read(deliveries[0].bundle, &length, error, sizeof(error));
        sh_package_archive_files files = {0};
        char id[SH_PACKAGE_ID_CAP];
        assert(bytes && sh_package_archive_identity(bytes, length, id, NULL, error, sizeof(error)) && !strcmp(id, "local.hell-guard"));
        assert(sh_package_archive_read(bytes, length, &files, error, sizeof(error)));
        for (size_t i = 0; i < files.count; i++) {
            const char *name = files.items[i].name;
            if (files.items[i].directory) continue;
            regular++;
            assert(!strstr(name, "cyberdemon") && !strstr(name, "spider") && strncmp(name, "hell_guard/", 11));
        }
        /* package.json, the conductor copy, the encounter, its editor row, the record. */
        assert(regular == 5);
        sh_package_archive_files_free(&files); free(bytes);
    }
    sh_mpkg_used_free(deliveries); deliveries = NULL;
    /* A use whose saved name is a private identity exists only in the compile
     * that made it: its tree travels whole, never as a component. */
    {
        sh_package_provenance private_use = {0};
        sh_package_provenance_site variant = {SH_PACKAGE_USE_OBJECT, "entity/9", root_site};
        size_t where = sh_package_provenance_context_add(&private_use, SH_PACKAGE_CONTEXT_ISOLATED,
            (const unsigned char (*)[32])sh_package_bundle_fingerprint(rows[0].original), 1, "hell_guard");
        bind(&private_use, "snapmaps/placeablesnapaiencounter/hell_guard_hammer",
            "smp/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &where, 1);
        assert(sh_package_provenance_use_set(&private_use, &variant, 0, &where, 1));
        private_use.semantics = SH_PACKAGE_PROVENANCE_USES;
        assert(sh_mpkg_used_packages_for_uses(&view, NULL, text, strlen(text), &references, &private_use,
            rows, 2, &deliveries, &resources, error, sizeof(error)) == 1);
        assert(deliveries && !deliveries[0].nested && !strcmp(deliveries[0].id, outer_key));
        sh_mpkg_used_free(deliveries); deliveries = NULL;
        /* The same private name on the all-carried composition: both whole. */
        {
            size_t shared = sh_package_provenance_context_add(&private_use, SH_PACKAGE_CONTEXT_COMPOSED,
                (const unsigned char (*)[32])composed_trees, 2, NULL);
            sh_package_provenance_site other = {SH_PACKAGE_USE_OBJECT, "entity/10", root_site};
            bind(&private_use, "ai/demon/hell_guard", "smp/fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210", &shared, 1);
            assert(sh_package_provenance_use_set(&private_use, &other, 1, &shared, 1));
            assert(sh_mpkg_used_packages_for_uses(&view, NULL, text, strlen(text), &references, &private_use,
                rows, 2, &deliveries, &resources, error, sizeof(error)) == 2);
            assert(deliveries && !deliveries[0].nested && !deliveries[1].nested);
            sh_mpkg_used_free(deliveries); deliveries = NULL;
        }
        sh_package_provenance_free(&private_use);
    }
    /* Were the conductor an identity the game lacks, its composition would be
     * the resource's own and every contributor would travel: the outer tree
     * whole (covering the component) and the standalone copy. */
    fixture_resources[0].baseline_known = 0;
    assert(sh_mpkg_used_packages_for_uses(&view, NULL, text, strlen(text), &references, &uses,
        rows, 2, &deliveries, &resources, error, sizeof(error)) == 2);
    assert(deliveries && !deliveries[0].nested && !deliveries[1].nested);
    sh_mpkg_used_free(deliveries); deliveries = NULL;
    fixture_resources[0].baseline_known = 1;
    /* A map saved before binding records composes every carried package into
     * one context on every use (the live c2v2 fixture): the encounter, its key
     * and the shared conductor all carry {outer, standalone}. The current
     * compile still says which component supplies the encounter, so exactly
     * that component travels and the composition adds nothing. */
    {
        sh_package_provenance recorded = {0};
        size_t shared = sh_package_provenance_context_add(&recorded, SH_PACKAGE_CONTEXT_COMPOSED,
            (const unsigned char (*)[32])composed_trees, 2, NULL);
        bind(&recorded, "snapmaps/placeablesnapaiencounter/hell_guard_hammer", "snapmaps/placeablesnapaiencounter/hell_guard_hammer", &shared, 1);
        assert(sh_package_provenance_bind(&recorded, SH_PACKAGE_KEY_FAMILY, "1024", "268435456", &shared, 1));
        bind(&recorded, "ai/conductor/coop/snapmap_default", "ai/conductor/coop/snapmap_default", &shared, 1);
        assert(sh_package_provenance_use_set(&recorded, &entity, 0, &shared, 1));
        assert(sh_package_provenance_use_set(&recorded, &key, 1, &shared, 1));
        assert(sh_package_provenance_use_set(&recorded, &world, 2, &shared, 1));
        recorded.semantics = SH_PACKAGE_PROVENANCE_USES;
        {
            size_t n = sh_mpkg_used_packages_for_uses(&view, NULL, text, strlen(text), &references, &recorded,
                rows, 2, &deliveries, &resources, error, sizeof(error));
            if (n != 1) {
                fprintf(stderr, "recorded layout: %zu deliveries (%s)\n", n, error);
                for (size_t i = 0; n != SIZE_MAX && i < n; i++)
                    fprintf(stderr, "  %s nested %d component '%s'\n", deliveries[i].id, deliveries[i].nested,
                        deliveries[i].component);
            }
            assert(n == 1);
        }
        assert(deliveries && deliveries[0].nested && !strcmp(deliveries[0].component, "hell_guard"));
        assert(strcmp(deliveries[0].id, outer_key) && strcmp(deliveries[0].id, standalone_key));
        sh_mpkg_used_free(deliveries); deliveries = NULL;
        sh_package_provenance_free(&recorded);
    }
    /* A map whose only use is the shared contribution selects no contributor
     * itself: every contributor's whole tree travels, as before. */
    {
        sh_package_provenance alone = {0};
        size_t shared = sh_package_provenance_context_add(&alone, SH_PACKAGE_CONTEXT_COMPOSED,
            (const unsigned char (*)[32])composed_trees, 2, NULL);
        bind(&alone, "ai/conductor/coop/snapmap_default", "ai/conductor/coop/snapmap_default", &shared, 1);
        assert(sh_package_provenance_use_set(&alone, &world, 0, &shared, 1));
        alone.semantics = SH_PACKAGE_PROVENANCE_USES;
        assert(sh_mpkg_used_packages_for_uses(&view, NULL, text, strlen(text), &references, &alone,
            rows, 2, &deliveries, &resources, error, sizeof(error)) == 2);
        assert(deliveries && !deliveries[0].nested && !deliveries[1].nested);
        sh_mpkg_used_free(deliveries); deliveries = NULL;
        sh_package_provenance_free(&alone);
    }
    /* A historical record of the same composition means those whole trees. */
    both[0] = sh_package_provenance_context_add(&history, SH_PACKAGE_CONTEXT_COMPOSED,
        (const unsigned char (*)[32])composed_trees, 2, NULL);
    bind(&history, "ai/conductor/coop/snapmap_default", "ai/conductor/coop/snapmap_default", both, 1);
    assert(sh_package_provenance_use_set(&history, &inseparable, 0, both, 1));
    history.semantics = SH_PACKAGE_PROVENANCE_USES;
    assert(sh_mpkg_used_packages_for_uses(&view, NULL, text, strlen(text), &references, &history,
        rows, 2, &deliveries, &resources, error, sizeof(error)) == 2);
    assert(deliveries && !deliveries[0].nested && !deliveries[1].nested);
    assert((!strcmp(deliveries[0].id, outer_key) && !strcmp(deliveries[1].id, standalone_key)) ||
        (!strcmp(deliveries[1].id, outer_key) && !strcmp(deliveries[0].id, standalone_key)));
    sh_mpkg_used_free(deliveries); deliveries = NULL;
    /* The same component used whole and through a component: whole, once. */
    both[1] = sh_package_provenance_context_add(&uses, SH_PACKAGE_CONTEXT_ISOLATED,
        (const unsigned char (*)[32])sh_package_bundle_fingerprint(rows[0].original), 1, "");
    bind(&uses, "ai/demon/cyberdemon", "ai/demon/cyberdemon", both + 1, 1);
    {
        sh_package_provenance_site other = {SH_PACKAGE_USE_OBJECT, "entity/1", root_site};
        assert(sh_package_provenance_use_set(&uses, &other, 3, both + 1, 1));
    }
    assert(sh_mpkg_used_packages_for_uses(&view, NULL, text, strlen(text), &references, &uses,
        rows, 2, &deliveries, &resources, error, sizeof(error)) == 1);
    assert(deliveries && !deliveries[0].nested && !strcmp(deliveries[0].id, outer_key));
    sh_mpkg_used_free(deliveries);
    sh_package_bundle_set_cache_root(NULL);
    sh_package_provenance_free(&uses); sh_package_provenance_free(&history);
    sh_package_references_free(&resources);
    for (size_t i = 0; i < 2; i++) sh_package_bundle_release(rows[i].original);
    sh_package_sources_free(sources);
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
        declaration("new-root", "generated/decls/entitydef/new-root.decl", "{ class = \"Entity\"; edit = {} }"),
        declaration("zz-empty", "generated/decls/entitydef/zz-empty.decl", "{ class = \"Entity\"; edit = {} }")
    };
    compiled.sources = &sources; compiled.resources = files; compiled.resource_count = 5;
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    sh_package_source_reader *reader = sh_package_source_reader_open(&compiled, NULL, baseline, NULL,
        registry, (uintptr_t)f.reflection, error, sizeof(error));
    if (!reader) fprintf(stderr, "reader: %s\n", error);
    assert(reader);
    run_local(reader, rows);
    run_keys(reader, &compiled, rows);
    run_native_identity(reader, &compiled, rows);
    /* Before run(), which releases the rows. */
    run_events(rows, archives, lengths);
    run_component(&compiled, rows);
    run_layout();
    run(reader, &compiled, rows, archives, lengths);
    sh_package_source_reader_close(reader);
    while (f.count) free(f.regions[--f.count].address);
    for (size_t i = 0; i < 3; i++) free(archives[i]);
    cleanup(); assert(!failures); puts("package_map_uses_test: PASS"); return 0;
}
