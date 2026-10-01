/* AI behaviors and their state machines through the source link, the linker and
 * the library's web table (package_fsm.h, fsm_web.h): changed declarations a
 * behavior's configuration selects split it into a private web, unchanged
 * ones leave it native, and the web carries the configuration it builds. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_library.h"
#include "package_native_assets.h"
#include "package_fixture.h"
#include "../src/backend/virtual_material_runtime.c"

void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; assert(0); return B2_PATCH_OK; }
void backend_log(const char *message) { (void)message; }

/* ---- Native reflection: two declaration managers and their classes ------ */
typedef struct region { void *address; size_t length; } region;
typedef struct fixture { region regions[96]; size_t count; void *registry, *reflection; } fixture;
static void *allocate(fixture *f, size_t length)
{
    void *p = calloc(1, length); assert(p && f->count < 96);
    f->regions[f->count++] = (region){p, length}; return p;
}
static void pointer(void *p, size_t offset, const void *value) { memcpy((char *)p + offset, &value, sizeof(value)); }
static void number(void *p, size_t offset, int value) { memcpy((char *)p + offset, &value, 4); }
static char *text(fixture *f, const char *value)
{ char *copy = allocate(f, strlen(value) + 1); strcpy(copy, value); return copy; }
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    fixture *f = context;
    for (size_t i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].address;
        if (address >= start && address - start <= f->regions[i].length &&
            length <= f->regions[i].length - (size_t)(address - start)) {
            memcpy(out, (const void *)address, length); return 1;
        }
    }
    return 0;
}
static void fields(fixture *f, void *record, size_t count,
    const char *const *types, const char *const *ops, const char *const *names)
{
    void *rows = allocate(f, (count + 1) * 0x48); pointer(record, 0x20, rows);
    for (size_t i = 0; i < count; i++) {
        pointer(rows, i * 0x48, text(f, types[i]));
        pointer(rows, i * 0x48 + 8, text(f, ops[i]));
        pointer(rows, i * 0x48 + 0x10, text(f, names[i])); number(rows, i * 0x48 + 0x1c, 8);
    }
}
enum { C_BEHAVIOR, C_FSM, C_TYPEINFO, C_GRAPH, C_STR, C_GRAPH_FSM, C_ENTITY, C_SUBGRAPH, C_COUNT };
static void setup(fixture *f)
{
    static const char *classes[C_COUNT] = {"idDeclAIBehavior", "idDeclAIFSMManager", "idDeclTypeInfo",
        "idDeclTypeInfoGraph", "idStr", "idAIFSMGraphFSM", "idDeclEntityDef", "idTypeInfoSubGraph"};
    static const char *parents[C_COUNT] = {"idDeclTypeInfo", "idDeclTypeInfoGraph", "", "idDeclTypeInfo", "",
        "idTypeInfoSubGraph", "", ""};
    static const char *types[] = {"aiBehavior", "aiFSMManager"};
    f->registry = allocate(f, 0x18);
    void *managers = allocate(f, 2 * 8);
    pointer(f->registry, 8, managers); number(f->registry, 0x10, 2);
    for (size_t i = 0; i < 2; i++) {
        void *manager = allocate(f, 0x90); pointer(managers, i * 8, manager);
        pointer(manager, 8, text(f, types[i])); pointer(manager, 0x10, text(f, classes[i]));
    }
    f->reflection = allocate(f, 0xb0);
    void *container = allocate(f, 0x30), *records = allocate(f, (C_COUNT + 1) * 0x38);
    void *objects = allocate(f, (C_COUNT + 1) * 16), *pointers = allocate(f, (C_COUNT + 1) * 16);
    pointer(f->reflection, 0, container); pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, C_COUNT + 1);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, C_COUNT + 1);
    for (size_t i = 0; i < C_COUNT; i++) {
        pointer(records, i * 0x38, text(f, classes[i]));
        pointer(records, i * 0x38 + 8, text(f, parents[i]));
    }
    /* Declaration pointers, the string reader and the graph object reader. */
    pointer(pointers, C_ENTITY * 16 + 8, (void *)0x10); pointer(pointers, C_BEHAVIOR * 16 + 8, (void *)0x10);
    pointer(pointers, C_FSM * 16 + 8, (void *)0x10);
    pointer(objects, C_STR * 16 + 8, (void *)0x70);
    pointer(objects, C_GRAPH * 16 + 8, (void *)0x30); pointer(objects, C_FSM * 16 + 8, (void *)0x30);
    const char *bt[] = {"idStr"}, *bo[] = {""}, *bn[] = {"aiFSMManagerDeclLayersConfig"};
    fields(f, (char *)records + C_BEHAVIOR * 0x38, 1, bt, bo, bn);
    const char *gn[] = {"name"};
    fields(f, (char *)records + C_GRAPH_FSM * 0x38, 1, bt, bo, gn);
}

/* ---- Installed originals ---------------------------------------------- */
#define LAYERS "layers = { layer = \"common\"; layer = \"imp_combat\"; layer = \"cyberdemon_hell\"; layer = \"disabled\"; }"
#define CONFIGS(solo) "layersConfigs = { common = { layer = \"common\"; } cyberdemon_hell = { layer = \"common\"; " \
    "layer = \"cyberdemon_hell\"; } imp = { layer = \"common\"; layer = \"imp_combat\"; } solo = { " solo " } }"
#define GRAPH(name, layer) "subGraphs = { subGraph = { object = { className = \"idAIFSMGraphFSM\"; object = { " \
    "name = \"" name "\"; } } layers = { " layer " } nodes = { } links = { } } }"
#define FSM(configs, graph) "{ edit = { " LAYERS " " configs " " graph " } }"
static const char ALERTCYCLE[] = FSM(CONFIGS("layer = \"cyberdemon_hell\";"), GRAPH("FSM_alertCycle", "layer = \"common\";"));
/* A configuration-only edit: solo gains imp_combat; no graph changes. */
static const char ALERTCYCLE_SOLO_EDIT[] = FSM(CONFIGS("layer = \"imp_combat\"; layer = \"cyberdemon_hell\";"),
    GRAPH("FSM_alertCycle", "layer = \"common\";"));
static const char BEHAVIORS[] = FSM("", GRAPH("FSM_behaviors", "layer = \"common\";"));
static const char HELL[] = FSM("", GRAPH("FSM_hell", "layer = \"cyberdemon_hell\";"));
static const char HELL_EDIT[] = FSM("", GRAPH("FSM_hell_fixed", "layer = \"cyberdemon_hell\";"));
static const char HELL_OTHER[] = FSM("", GRAPH("FSM_hell_other", "layer = \"cyberdemon_hell\";"));
static const char IMP[] = FSM("", GRAPH("FSM_imp", "layer = \"imp_combat\";"));
static const char GENERIC[] = FSM("", GRAPH("FSM_generic", "layer = \"common\"; layer = \"cyberdemon_hell\";"));
static const char BASE[] = "{ edit = { aiFSMManagerDeclLayersConfig = \"cyberdemon_hell\"; } }";
static const char BOSS[] = "{ inherit = \"behaviors/base\"; edit = { } }";
static const char SOLO[] = "{ edit = { aiFSMManagerDeclLayersConfig = \"solo\"; } }";
/* A behavior its package changes itself (private before selection existed). */
static const char BOSS_OWN[] = "{ inherit = \"behaviors/base\"; edit = { aiFSMManagerDeclLayersConfig = \"cyberdemon_hell\"; } }";
typedef struct stock { const char *type, *name, *path, *text; } stock;
static const stock STOCK[] = {
    {"aiFSMManager", "states/alertcycle", "generated/decls/aifsmmanager/states/alertcycle.decl", ALERTCYCLE},
    {"aiFSMManager", "states/behaviors", "generated/decls/aifsmmanager/states/behaviors.decl", BEHAVIORS},
    {"aiFSMManager", "states/behavior_generic", "generated/decls/aifsmmanager/states/behavior_generic.decl", GENERIC},
    {"aiFSMManager", "states/cyberdemon_hell", "generated/decls/aifsmmanager/states/cyberdemon_hell.decl", HELL},
    {"aiFSMManager", "states/impcombat", "generated/decls/aifsmmanager/states/impcombat.decl", IMP},
    {"aiBehavior", "behaviors/base", "generated/decls/aibehavior/behaviors/base.decl", BASE},
    {"aiBehavior", "behaviors/boss", "generated/decls/aibehavior/behaviors/boss.decl", BOSS},
    {"aiBehavior", "behaviors/solo", "generated/decls/aibehavior/behaviors/solo.decl", SOLO}};
#define STOCK_COUNT (sizeof(STOCK) / sizeof(STOCK[0]))
static int original(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    for (size_t i = 0; i < STOCK_COUNT; i++) if (!strcmp(path, STOCK[i].path)) {
        *length = strlen(STOCK[i].text); *body = malloc(*length + 1); assert(*body);
        memcpy(*body, STOCK[i].text, *length + 1); return 1;
    }
    return 0;
}

typedef struct bytes { unsigned char data[4096]; size_t length; } bytes;
static void raw(bytes *b, const void *data, size_t length)
{ assert(length <= sizeof(b->data) - b->length); memcpy(b->data + b->length, data, length); b->length += length; }
static void be(bytes *b, uint32_t n)
{ unsigned char p[] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n}; raw(b, p, 4); }
static void le(bytes *b, uint32_t n)
{ unsigned char p[] = {(unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16), (unsigned char)(n >> 24)}; raw(b, p, 4); }
static void string(bytes *b, const char *s) { le(b, (uint32_t)strlen(s)); raw(b, s, strlen(s)); }
static void binary_file(const char *relative, const unsigned char *body, size_t length)
{
    char path[4096]; create(relative, ""); snprintf(path, sizeof(path), "%s/%s", root, relative);
    FILE *file = fopen(path, "wb"); assert(file); assert(fwrite(body, 1, length, file) == length); assert(!fclose(file));
}
static void catalog_file(const char *relative, int rows)
{
    bytes b = {0}; unsigned char head[0x20] = {5, 'S', 'E', 'R'}, tail[21] = {0};
    raw(&b, head, sizeof(head)); be(&b, rows ? (uint32_t)STOCK_COUNT : 0u);
    for (size_t i = 0; rows && i < STOCK_COUNT; i++) {
        be(&b, (uint32_t)i); string(&b, STOCK[i].type); string(&b, STOCK[i].name); string(&b, STOCK[i].path);
        raw(&b, tail, sizeof(tail));
    }
    uint32_t size = (uint32_t)b.length - 0x20;
    b.data[4] = (unsigned char)(size >> 24); b.data[5] = (unsigned char)(size >> 16);
    b.data[6] = (unsigned char)(size >> 8); b.data[7] = (unsigned char)size;
    binary_file(relative, b.data, b.length);
}
static void package(const char *folder, const char *name)
{
    char path[1024], descriptor[256]; snprintf(descriptor, sizeof(descriptor), "{\"id\":\"%s\",\"name\":\"%s\"}", folder, name);
    snprintf(path, sizeof(path), "overrides/%s/package.json", folder); create(path, descriptor);
}
static void supply(const char *folder, const char *file, const char *body)
{
    char path[1024]; snprintf(path, sizeof(path), "overrides/%s/assets/%s", folder, file); create(path, body);
}

/* ---- Queries ------------------------------------------------------------- */
static size_t owner(const sh_package_sources *sources, const char *name)
{
    for (size_t i = 0; i < sources->component_count; i++) if (!*sources->components[i].relative &&
        !strcmp(sources->components[i].descriptor.name, name)) return sources->components[i].owner;
    assert(0); return SIZE_MAX;
}
/* The linked name a package's scope binds, or NULL while it keeps the native name. */
static const char *linked(const sh_package_library *library, const sh_package_sources *sources,
    const char *package_name, const char *family, const char *original_name)
{
    const unsigned char *tree = sh_package_source_tree(sources, owner(sources, package_name));
    const char *result = NULL;
    for (size_t i = 0; i < library->bindings.binding_count; i++) {
        const sh_package_provenance_binding *b = library->bindings.bindings + i;
        if (strcmp(b->family, family) || strcmp(b->original, original_name)) continue;
        for (size_t j = 0; j < b->context_count; j++) {
            const sh_package_provenance_context *c = library->bindings.contexts + b->contexts[j];
            if (c->mode == SH_PACKAGE_CONTEXT_ISOLATED && c->tree_count == 1 && !memcmp(c->trees[0], tree, 32) &&
                !*c->component) { assert(!result || !strcmp(result, b->name)); result = b->name; }
        }
    }
    return result && strcmp(result, original_name) ? result : NULL;
}
static const sh_package_fsm_web *web_of(const sh_package_library *library, const char *behavior)
{
    const sh_package_fsm_webs *webs = library->fsm_webs;
    for (size_t i = 0; behavior && webs && i < webs->count; i++)
        for (size_t k = 0; k < webs->webs[i].behavior_count; k++)
            if (!strcmp(webs->webs[i].behaviors[k], behavior)) return webs->webs + i;
    return NULL;
}
static const sh_compiled_resource *declaration(const sh_package_library *library, const char *family, const char *name)
{
    for (size_t i = 0; library->compiled && i < library->compiled->resource_count; i++)
        if (sh_package_resource_native_matches(library->compiled->resources + i, family, name)) return library->compiled->resources + i;
    return NULL;
}
static char *read_text(const sh_package_library *library, const sh_compiled_resource *r, size_t *length)
{
    char error[512];
    unsigned char *body = r ? sh_package_compilation_read(library->compiled, r, SIZE_MAX, length, error, sizeof(error)) : NULL;
    return (char *)body;
}
/* The mask the web's private configuration selects in its generated first declaration. */
static uint32_t web_mask(const sh_package_library *library, const sh_package_fsm_web *web)
{
    size_t length = 0; uint32_t mask = 0; char error[256];
    char *first = web ? read_text(library, declaration(library, "aifsmmanager", web->to[0]), &length) : NULL;
    int found = first ? sh_package_fsm_config_mask((sh_decl_source){first, length}, web->config, &mask, error, sizeof(error)) : -1;
    free(first);
    return found == 1 ? mask : 0xffffffffu;
}
static int replaces(const sh_package_fsm_web *web, const char *from, const char *to)
{
    for (size_t i = 0; web && i < web->count; i++) if (!strcmp(web->from[i], from) && (!to || !strcmp(web->to[i], to))) return 1;
    return 0;
}
static int owns(const sh_package_library *library, const char *name)
{
    for (size_t i = 0; library->fsm_webs && i < library->fsm_webs->owned_count; i++)
        if (!strcmp(library->fsm_webs->owned[i], name)) return 1;
    return 0;
}
static int depends(const sh_package_library *library, const char *behavior, const char *target)
{
    const sh_compiled_resource *r = declaration(library, "aibehavior", behavior), *t = declaration(library, "aifsmmanager", target);
    for (size_t i = 0; r && t && i < r->bound_dependency_count; i++) if (!strcmp(r->bound_dependencies[i], t->engine_path)) return 1;
    return 0;
}

int main(void)
{
    char temporary[MAX_PATH], path[4096], error[1024]; fixture f = {0}; setup(&f);
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-fsm-link-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    catalog_file("base/snap_gameresources.pindex", 1); catalog_file("base/gameresources.pindex", 0);
    create("base/snap_gameresources.resources", ""); create("base/snap_gameresources.patch", "");
    create("base/gameresources.resources", ""); create("base/gameresources.patch", "");
    snprintf(path, sizeof(path), "%s/base", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(path, error, sizeof(error)); assert(catalog);
    const char *boss = "generated/decls/aibehavior/behaviors/boss.decl", *solo = "generated/decls/aibehavior/behaviors/solo.decl";
    const char *hell = "generated/decls/aifsmmanager/states/cyberdemon_hell.decl";
    const char *alert = "generated/decls/aifsmmanager/states/alertcycle.decl";
    /* A graph edit; its exact copy; a different graph edit; a configuration-only
     * edit; the stock behaviors alone. */
    package("hell", "Hell"); supply("hell", boss, BOSS); supply("hell", hell, HELL_EDIT);
    package("hell_copy", "Hell Copy"); supply("hell_copy", boss, BOSS); supply("hell_copy", hell, HELL_EDIT);
    package("hell_other", "Hell Other"); supply("hell_other", boss, BOSS); supply("hell_other", hell, HELL_OTHER);
    package("config", "Config"); supply("config", solo, SOLO); supply("config", alert, ALERTCYCLE_SOLO_EDIT);
    package("plain", "Plain"); supply("plain", boss, BOSS); supply("plain", solo, SOLO);
    package("carrier_a", "Carrier A"); supply("carrier_a", boss, BOSS_OWN);
    package("carrier_b", "Carrier B"); supply("carrier_b", boss, BOSS_OWN); supply("carrier_b", hell, HELL_EDIT);
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = &f;
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry};
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    sh_package_library library = {0};
    int ok = sh_package_library_compile(sources, &env, catalog, original, &f, registry,
        (uintptr_t)f.reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "fsm link: %s\n", error); assert(ok);
    CHECK(library.fsm_webs != NULL);

    /* Unchanged webs leave their behaviors native: the configuration source
     * edge (to states/alertcycle, whatever it selects) and the graph edges
     * vanish while every target is original, also for a configuration
     * without common. */
    CHECK(!linked(&library, sources, "Plain", "aibehavior", "behaviors/boss"));
    CHECK(!linked(&library, sources, "Plain", "aibehavior", "behaviors/solo"));
    CHECK(!web_of(&library, "behaviors/boss") && !web_of(&library, "behaviors/solo"));

    /* A graph edit: a private behavior and web that replaces the changed
     * declaration and the first one (with the native configuration's mask). */
    const char *hell_boss = linked(&library, sources, "Hell", "aibehavior", "behaviors/boss");
    const char *hell_decl = linked(&library, sources, "Hell", "aifsmmanager", "states/cyberdemon_hell");
    const sh_package_fsm_web *hell_web = web_of(&library, hell_boss);
    CHECK(hell_boss && hell_decl && hell_web);
    CHECK(replaces(hell_web, "states/cyberdemon_hell", hell_decl) && replaces(hell_web, "states/alertcycle", NULL));
    CHECK(hell_web && !strncmp(hell_web->to[0], SH_PACKAGE_FSM_FIRST_PREFIX, strlen(SH_PACKAGE_FSM_FIRST_PREFIX)));
    CHECK(hell_web && hell_web->count == 2 && !hell_web->extra_count && web_mask(&library, hell_web) == 5);
    CHECK(depends(&library, hell_boss, hell_decl));
    CHECK(!replaces(hell_web, "states/behaviors", NULL) && !replaces(hell_web, "states/impcombat", NULL));
    /* An exact copy dedupes to the same behavior and web; a different edit
     * is another behavior and web. */
    CHECK(hell_boss && linked(&library, sources, "Hell Copy", "aibehavior", "behaviors/boss") &&
        !strcmp(hell_boss, linked(&library, sources, "Hell Copy", "aibehavior", "behaviors/boss")));
    const char *other_boss = linked(&library, sources, "Hell Other", "aibehavior", "behaviors/boss");
    const sh_package_fsm_web *other_web = web_of(&library, other_boss);
    CHECK(other_boss && hell_boss && strcmp(other_boss, hell_boss) && other_web && other_web != hell_web);
    CHECK(other_web && hell_web && strcmp(other_web->config, hell_web->config) && strcmp(other_web->to[0], hell_web->to[0]));

    /* A configuration-only edit of the first declaration, for a configuration
     * without common: the behavior splits and its web carries the edited
     * mask under its own private configuration, though no graph file it
     * selects changed bytes. */
    const char *config_solo = linked(&library, sources, "Config", "aibehavior", "behaviors/solo");
    const char *config_first = linked(&library, sources, "Config", "aifsmmanager", "states/alertcycle");
    const sh_package_fsm_web *config_web = web_of(&library, config_solo);
    CHECK(config_solo && config_first && config_web);
    CHECK(config_web && config_web->count == 1 && replaces(config_web, "states/alertcycle", NULL));
    CHECK(web_mask(&library, config_web) == 6);
    CHECK(config_first && owns(&library, config_first) && depends(&library, config_solo, config_first));
    CHECK(!linked(&library, sources, "Config", "aifsmmanager", "states/cyberdemon_hell"));
    CHECK(config_web && hell_web && strcmp(config_web->config, hell_web->config));

    /* Every product state machine is owned; the placeholder selects nothing. */
    CHECK(hell_decl && owns(&library, hell_decl) && owns(&library, SH_PACKAGE_FSM_PLACEHOLDER));
    for (size_t i = 0; library.fsm_webs && i < library.fsm_webs->count; i++) {
        CHECK(owns(&library, library.fsm_webs->webs[i].to[0]));
        CHECK(declaration(&library, "aifsmmanager", library.fsm_webs->webs[i].to[0]) != NULL);
    }
    {
        size_t length = 0; uint32_t mask = 1; char reason[256];
        char *placeholder = read_text(&library, declaration(&library, "aifsmmanager", SH_PACKAGE_FSM_PLACEHOLDER), &length);
        CHECK(placeholder && sh_package_fsm_graph_mask((sh_decl_source){placeholder, length}, &mask, reason, sizeof(reason)) == 1 && !mask);
        free(placeholder);
    }
    CHECK(!owns(&library, "states/alertcycle") && !owns(&library, "states/cyberdemon_hell"));

    /* A map saved before state-machine selection: one private behavior both
     * carrier contexts shared (equal text, no selection edges). Selection
     * splits only the context whose scope changes a state machine the behavior
     * selects; the other keeps the saved identity and name. Each saved use
     * resolves in its own context, and the split one binds the private web. */
    {
        sh_package_library before = {0}, after = {0};
        sh_package_provenance saved = {0};
        const char *replacement = NULL;
        sh_package_source_link_fsm(0);
        ok = sh_package_library_compile(sources, &env, catalog, original, &f, registry,
            (uintptr_t)f.reflection, &native, &before, error, sizeof(error));
        sh_package_source_link_fsm(1);
        if (!ok) fprintf(stderr, "pre-selection: %s\n", error);
        CHECK(ok);
        const char *a0 = ok ? linked(&before, sources, "Carrier A", "aibehavior", "behaviors/boss") : NULL;
        const char *b0 = ok ? linked(&before, sources, "Carrier B", "aibehavior", "behaviors/boss") : NULL;
        CHECK(a0 && b0 && !strcmp(a0, b0));
        CHECK(!before.fsm_webs || !before.fsm_webs->count);
        CHECK(ok && !linked(&before, sources, "Hell", "aibehavior", "behaviors/boss"));
        size_t contexts[2];
        contexts[0] = sh_package_provenance_context_add(&saved, SH_PACKAGE_CONTEXT_ISOLATED,
            (const unsigned char (*)[32])sh_package_source_tree(sources, owner(sources, "Carrier A")), 1, "");
        contexts[1] = sh_package_provenance_context_add(&saved, SH_PACKAGE_CONTEXT_ISOLATED,
            (const unsigned char (*)[32])sh_package_source_tree(sources, owner(sources, "Carrier B")), 1, "");
        CHECK(contexts[0] != SIZE_MAX && contexts[1] != SIZE_MAX && a0 &&
            sh_package_provenance_bind(&saved, "aibehavior", "behaviors/boss", a0, contexts, 2));
        sh_package_provenance_site first = {SH_PACKAGE_USE_OBJECT, "entity/0", "behavior"};
        sh_package_provenance_site second = {SH_PACKAGE_USE_OBJECT, "entity/1", "behavior"};
        CHECK(sh_package_provenance_use_set(&saved, &first, 0, contexts, 1));
        CHECK(sh_package_provenance_use_set(&saved, &second, 0, contexts + 1, 1));
        ok = sh_package_library_compile_contexts(sources, &saved, &env, catalog, original, &f, registry,
            (uintptr_t)f.reflection, &native, &after, error, sizeof(error));
        if (!ok) fprintf(stderr, "pre-selection map: %s\n", error);
        CHECK(ok);
        CHECK(ok && sh_package_provenance_resolve_use(&saved, 0, &after.bindings, &replacement) == 1 &&
            replacement && a0 && !strcmp(replacement, a0));
        CHECK(ok && !web_of(&after, a0));
        replacement = NULL;
        CHECK(ok && sh_package_provenance_resolve_use(&saved, 1, &after.bindings, &replacement) == 1 &&
            replacement && a0 && strcmp(replacement, a0));
        const sh_package_fsm_web *split = replacement ? web_of(&after, replacement) : NULL;
        CHECK(split && replaces(split, "states/cyberdemon_hell", NULL) && web_mask(&after, split) == 5);
        /* The unsplit context delivers the same behavior bytes it did before. */
        size_t old_length = 0, new_length = 0;
        char *old_text = a0 ? read_text(&before, declaration(&before, "aibehavior", a0), &old_length) : NULL;
        char *new_text = a0 ? read_text(&after, declaration(&after, "aibehavior", a0), &new_length) : NULL;
        CHECK(old_text && new_text && old_length == new_length && !memcmp(old_text, new_text, old_length));
        free(old_text); free(new_text);
        sh_package_provenance_free(&saved);
        sh_package_library_free(&after); sh_package_library_free(&before);
    }

    sh_package_library_free(&library); sh_package_native_assets_close(assets); sh_package_sources_free(sources);
    sh_resource_catalog_close(catalog); for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup(); assert(!failures); puts("package fsm link: PASS"); return 0;
}
