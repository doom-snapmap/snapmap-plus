/* Downstream-only packages expose affected native editor roots in their scope. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_library.h"
#include "package_native_assets.h"
#include "package_audio_program.h"
#include "audio_banks.h"
#include "audio_originals.h"
#include "package_usage.h"
#include "package_fixture.h"
#include "../src/backend/virtual_material_runtime.c"

void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; assert(0); return B2_PATCH_OK; }
void backend_log(const char *message) { (void)message; }

typedef struct region { void *address; size_t length; } region;
typedef struct fixture {
    region regions[192]; size_t count;
    void *registry, *reflection;
    int missing_unrelated, absent_target, implicit_root, reader_gap, absent_program, hands;
    int prepare_programs, preparation_calls, program_read_failure;
    int queued_second; /* preparations requested for program "second" */
    int palette_reads; /* stock palette entries read (reverse-root discovery) */
} fixture;
static void *allocate(fixture *f, size_t length)
{
    void *p = calloc(1, length); assert(p && f->count < 192);
    f->regions[f->count++] = (region){p, length}; return p;
}
static void pointer(void *p, size_t offset, const void *value)
{ memcpy((char *)p + offset, &value, sizeof(value)); }
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
static void setup(fixture *f)
{
    static const char *classes[] = {"idDeclEntityDef", "Entity", "idDeclTypeInfo",
        "idDeclSnapEditorEntity", "idImage", "idRenderModel", "idStaticModel", "idStr", "idDeclTypeInfoGraph",
        "idDeclRenderProg", "idDeclAnimWeb", "idDeclWeapon"};
    static const char *types[] = {"entityDef", "snapEditorEntityDef", "image", "renderProg", "animWeb", "weapon"};
    static const size_t indices[] = {0, 3, 4, 9, 10, 11};
    f->registry = allocate(f, 0x18);
    void *managers = allocate(f, 6 * 8);
    pointer(f->registry, 8, managers); number(f->registry, 0x10, 6);
    for (size_t i = 0; i < 6; i++) {
        void *manager = allocate(f, 0x90); pointer(managers, i * 8, manager);
        pointer(manager, 8, text(f, types[i])); pointer(manager, 0x10, text(f, classes[indices[i]]));
    }
    f->reflection = allocate(f, 0xb0);
    void *container = allocate(f, 0x30), *records = allocate(f, 13 * 0x38);
    void *objects = allocate(f, 13 * 16), *pointers = allocate(f, 13 * 16);
    pointer(f->reflection, 0, container); pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, 13);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, 13);
    pointer(pointers, 10 * 16 + 8, (void *)0x10); pointer(pointers, 11 * 16 + 8, (void *)0x10);
    for (size_t i = 0; i < 12; i++) {
        pointer(records, i * 0x38, text(f, classes[i]));
        pointer(records, i * 0x38 + 8, text(f, i == 3 || i == 8 || i == 11 ? "idDeclTypeInfo" : ""));
    }
    pointer(pointers, 8, (void *)0x10); pointer(pointers, 3 * 16 + 8, (void *)0x10);
    pointer(pointers, 4 * 16 + 8, (void *)0x40); pointer(pointers, 5 * 16 + 8, (void *)0x50);
    pointer(pointers, 6 * 16 + 8, (void *)0x60); pointer(objects, 7 * 16 + 8, (void *)0x70);
    pointer(objects, 8 * 16 + 8, (void *)0x30); pointer(pointers, 9 * 16 + 8, (void *)0x10);
    /* idSWF has no registered reader in this schema: a stock coverage gap. */
    const char *et[] = {"idRenderModel", "idImage", "idDeclEntityDef", "idSWF", "idDeclRenderProg", "idDeclAnimWeb"};
    const char *eo[] = {"*", "*", "*", "*", "*", "*"};
    const char *en[] = {"model", "image", "target", "gui", "program", "handsAnimWeb"};
    fields(f, (char *)records + 0x38, 6, et, eo, en);
    const char *st[] = {"idDeclEntityDef", "idStr", "idStr"};
    const char *so[] = {"*", "", ""}, *sn[] = {"entityDef", "category", "displayName"};
    fields(f, (char *)records + 3 * 0x38, 3, st, so, sn);
    /* A weapon names its hands subwebs by string. */
    const char *wt[] = {"idStr", "idStr"}, *wo[] = {"", ""}, *wn[] = {"subweb_normal", "handsHitReactionSubweb"};
    fields(f, (char *)records + 11 * 0x38, 2, wt, wo, wn);
}

typedef struct bytes { unsigned char data[4096]; size_t length; } bytes;
static void raw(bytes *b, const void *data, size_t length)
{ assert(length <= sizeof(b->data) - b->length); memcpy(b->data + b->length, data, length); b->length += length; }
static void be(bytes *b, uint32_t n)
{ unsigned char p[] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n}; raw(b, p, 4); }
static void le(bytes *b, uint32_t n)
{ unsigned char p[] = {(unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16), (unsigned char)(n >> 24)}; raw(b, p, 4); }
static void string(bytes *b, const char *s) { le(b, (uint32_t)strlen(s)); raw(b, s, strlen(s)); }
static bytes model(const char *material)
{
    bytes b = {0}; const unsigned char magic[] = {0x1b, 0x4c, 0x4d, 0x42};
    raw(&b, magic, 4); be(&b, 0x1234); be(&b, 1); string(&b, material);
    be(&b, 0); be(&b, 0); be(&b, 0); be(&b, 0); raw(&b, magic, 4); be(&b, 0); return b;
}
static int original(void *context, const char *path, unsigned char **body, size_t *length)
{
    fixture *f = context; bytes b = {0}; const char *value = NULL;
    *body = NULL; *length = 0;
    if (!strncmp(path, "generated/decls/snapeditorentitydef/", 36)) f->palette_reads++;
    if (!strcmp(path, "generated/decls/snapeditorentitydef/rock.decl"))
        value = "{ edit = { entityDef = \"props/rock\"; category = \"Props\"; displayName = \"Rock\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/unrelated.decl")) {
        if (f->missing_unrelated) return -1;
        value = "{ edit = { entityDef = \"props/unrelated\"; category = \"Unrelated\"; } }";
    } else if (!strcmp(path, "generated/decls/entitydef/props/rock.decl"))
        value = f->prepare_programs ? "{ class = \"Entity\"; edit = { program = \"first\"; } }" :
            f->implicit_root ? "{ class = \"Entity\"; edit = {} }" : f->absent_program ?
            "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; program = \"absentprog\"; } }" :
            "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }";
    else if (!strcmp(path, "generated/decls/entitydef/props/unrelated.decl")) {
        if (f->absent_target) return 0;
        value = f->prepare_programs ? "{ class = \"Entity\"; edit = { program = \"second\"; } }" :
            f->reader_gap ? "{ class = \"Entity\"; edit = { image = \"textures/unrelated\"; "
            "model = \"models/campaign.lwo\"; gui = \"swf/unrelated\"; } }" : f->hands == 2 ?
            "{ class = \"Entity\"; edit = { image = \"textures/unrelated\"; target = \"player-def\"; } }" :
            "{ class = \"Entity\"; edit = { image = \"textures/unrelated\"; } }";
    }
    /* The co-op rules entity names the spawned player (hands 1 and 2); with
     * no such rules the engine's default player/mp is used (hands 3). */
    else if (f->hands && f->hands != 3 && !strcmp(path, "generated/decls/entitydef/mp/modes/coop/game_challenge.decl"))
        value = "{ class = \"Entity\"; edit = { playerSpawnDef = \"player-def\"; } }";
    else if (f->hands && !strcmp(path, f->hands == 3 ? "generated/decls/entitydef/player/mp.decl" :
        "generated/decls/entitydef/player-def.decl"))
        value = "{ class = \"Entity\"; edit = { handsAnimWeb = \"hands\"; } }";
    else if (f->hands && !strcmp(path, "generated/decls/animweb/hands.decl"))
        value = "{ props { modelInfos { \"hands.md6\" \"\" } } states { } subWebs { subWeb \"pistol\" { } } }";
    /* The web's hands model names the web again through an animation event
     * argument: a stock cycle through the one global web. */
    else if (f->hands && !strcmp(path, "generated/decls/md6def/hands.md6.decl"))
        value = "{ init { } jointGroups { } events { anim \"hands/idle.md6anim\" { event \"ae_web\" { "
            "frame 1 row 0 locked 0 animWeb \"hands\" } } } aliases { } props { } }";
    else if (!strcmp(path, "cooked/model/models/campaign.bmodel")) {
        /* Present only in the campaign archive: absent from SnapMap. */
        b = model("generic/rock");
        *length = b.length; *body = malloc(*length + 1); assert(*body);
        memcpy(*body, b.data, *length); (*body)[*length] = 0; return 2;
    }
    else if (!strcmp(path, "cooked/model/models/rock.bmodel")) b = model("generic/rock");
    else if (!strcmp(path, "generated/decls/material/generic/rock.decl")) value = "{ tex \"textures/rock\" }";
    else if (!strcmp(path, "generated/decls/material/generic/other.decl")) value = "{ tex \"textures/other\" }";
    else if (!strcmp(path, "generated/decls/renderparm/tex.decl")) value = "{ Tex2D _white }";
    else if (!strcmp(path, "generated/decls/renderparm/landpagefile.decl")) value = "{ String }";
    else if (!strcmp(path, "generated/image/textures/rock.bimage")) value = "stock-image";
    else if (!strcmp(path, "generated/image/textures/other.bimage")) value = "other-image";
    else if (!strcmp(path, "generated/image/textures/unrelated.bimage")) value = "unrelated-image";
    else if (!strcmp(path, "generated/transsortatlas/textures/fx/glow.bimage")) value = "stock-atlas";
    else if (!strcmp(path, "generated/decls/md6def/gore/chunk.md6.decl")) value = "{ init { } }";
    else if (!strcmp(path, "generated/transsortatlas/textures/fx/campaignglow.bimage")) {
        *length = 14; *body = malloc(15); assert(*body); memcpy(*body, "campaign-atlas", 15); return 2;
    }
    else if (f->prepare_programs && (!strcmp(path,"generated/decls/renderprog/first.decl") ||
        !strcmp(path,"generated/decls/renderprog/second.decl"))) {
        if (f->program_read_failure && strstr(path,"/second.decl")) return -1;
        value = "{ }";
    }
    else return 0;
    *length = value ? strlen(value) : b.length; *body = malloc(*length + 1); assert(*body);
    memcpy(*body, value ? (const void *)value : b.data, *length); (*body)[*length] = 0; return 1;
}
static void binary_file(const char *relative, const unsigned char *body, size_t length)
{
    char path[4096]; create(relative, ""); snprintf(path, sizeof(path), "%s/%s", root, relative);
    FILE *file = fopen(path, "wb"); assert(file); assert(fwrite(body, 1, length, file) == length); assert(!fclose(file));
}
static void catalog_file(const char *relative, int campaign, int images, int alias)
{
    bytes b = {0}; unsigned char head[0x20] = {5, 'S', 'E', 'R'}, tail[21] = {0};
    unsigned count = (campaign ? 2u : images ? 6u : 2u) + (alias ? 1u : 0u);
    raw(&b, head, sizeof(head)); be(&b, count);
    for (unsigned i = 0; i < count; i++) {
        if (alias && i == count - 1) {
            /* A second row for the stock rock image, at a path no original
             * holds: only the catalog names it. */
            be(&b, i); string(&b, "image"); string(&b, "textures/rock");
            string(&b, "generated/image/textures/rock_catalog_alias.bimage"); raw(&b, tail, sizeof(tail)); continue;
        }
        if (i == 5) {
            /* SnapMap registers this model with no data. */
            be(&b, i); string(&b, "model"); string(&b, "models/fileless.lwo");
            string(&b, ""); raw(&b, tail, sizeof(tail)); continue;
        }
        if (campaign && i == 1) {
            /* A transparency atlas entry only the campaign archive lists. */
            be(&b, i); string(&b, "transSortAtlas"); string(&b, "textures/fx/campaignglow");
            string(&b, "generated/transsortatlas/textures/fx/campaignglow.bimage"); raw(&b, tail, sizeof(tail)); continue;
        }
        if (i == 4) {
            /* A transparency atlas entry: an opaque cooked image. */
            be(&b, i); string(&b, "transSortAtlas"); string(&b, "textures/fx/glow");
            string(&b, "generated/transsortatlas/textures/fx/glow.bimage"); raw(&b, tail, sizeof(tail)); continue;
        }
        if (i == 2) {
            be(&b, i); string(&b, "image"); string(&b, "textures/rock");
            string(&b, "generated/image/textures/rock.bimage"); raw(&b, tail, sizeof(tail)); continue;
        }
        if (i == 3) {
            /* The extensionless catalog identity differs from the root's
             * .lwo field; both verified mappings reach the same model file. */
            be(&b, i); string(&b, "model"); string(&b, "models/rock");
            string(&b, "cooked/model/models/rock.bmodel"); raw(&b, tail, sizeof(tail)); continue;
        }
        const char *name = campaign ? "campaign-only" : i ? "unrelated" : "rock";
        char path[256]; snprintf(path, sizeof(path), "generated/decls/snapeditorentitydef/%s.decl", name);
        be(&b, i); string(&b, "snapEditorEntityDef"); string(&b, name); string(&b, path); raw(&b, tail, sizeof(tail));
    }
    uint32_t size = (uint32_t)b.length - 0x20;
    b.data[4] = (unsigned char)(size >> 24); b.data[5] = (unsigned char)(size >> 16);
    b.data[6] = (unsigned char)(size >> 8); b.data[7] = (unsigned char)size;
    binary_file(relative, b.data, b.length);
}
static void package(const char *folder, const char *name, const char *file, const char *body)
{
    char path[1024], descriptor[256]; snprintf(descriptor, sizeof(descriptor), "{\"id\":\"same\",\"name\":\"%s\"}", name);
    snprintf(path, sizeof(path), "overrides/%s/package.json", folder); create(path, descriptor);
    snprintf(path, sizeof(path), "overrides/%s/assets/%s", folder, file); create(path, body);
}
static size_t owner(const sh_package_sources *sources, const char *name)
{
    for (size_t i = 0; i < sources->component_count; i++) if (!*sources->components[i].relative &&
        !strcmp(sources->components[i].descriptor.name, name)) return sources->components[i].owner;
    assert(0); return SIZE_MAX;
}
static const sh_package_provenance_binding *binding(const sh_package_library *library,
    const sh_package_sources *sources, const char *name, const char *component, const char *family, const char *original_name)
{
    const unsigned char *tree = sh_package_source_tree(sources, owner(sources, name));
    const sh_package_provenance_binding *result = NULL;
    for (size_t i = 0; i < library->bindings.binding_count; i++) {
        const sh_package_provenance_binding *b = library->bindings.bindings + i;
        if (strcmp(b->family, family) || strcmp(b->original, original_name)) continue;
        for (size_t j = 0; j < b->context_count; j++) {
            const sh_package_provenance_context *c = library->bindings.contexts + b->contexts[j];
            if (c->mode == SH_PACKAGE_CONTEXT_ISOLATED && c->tree_count == 1 &&
                !memcmp(c->trees[0], tree, 32) && !strcmp(c->component, component)) {
                assert(!result || result == b); result = b;
            }
        }
    }
    return result;
}
static const sh_compiled_resource *resource(const sh_package_library *library, const char *family, const char *name)
{
    const sh_compiled_resource *result = NULL;
    for (size_t i = 0; i < library->compiled->resource_count; i++) {
        const sh_compiled_resource *r = library->compiled->resources + i;
        if (sh_package_resource_native_matches(r, family, name)) {
            assert(!result); result = r;
        }
    }
    return result;
}
static int holds(const sh_package_library *library, const sh_compiled_resource *r, const char *needle)
{
    char error[1024]; size_t length = 0;
    unsigned char *body = r ? sh_package_compilation_read(library->compiled, r, SIZE_MAX, &length, error, sizeof(error)) : NULL;
    assert(body); unsigned char *terminated = realloc(body, length + 1); assert(terminated); terminated[length] = 0;
    int found = strstr((const char *)terminated, needle) != NULL; free(terminated); return found;
}
static void contains(const sh_package_library *library, const sh_compiled_resource *r, const char *needle)
{ assert(holds(library, r, needle)); }
/* Private copies of one family whose text holds the needle. */
static size_t published(const sh_package_library *library, const char *family, const char *needle)
{
    size_t count = 0;
    for (size_t i = 0; i < library->compiled->resource_count; i++) {
        const sh_compiled_resource *r = library->compiled->resources + i;
        if (r->native_type && !strcmp(r->native_type, family) && r->native_name &&
            !strncmp(r->native_name, "smp/", 4) && holds(library, r, needle)) count++;
    }
    return count;
}

typedef struct implicit_adapter {
    sh_package_library_native base;
    size_t observed, admitted, absent, baseline;
    int fail;
} implicit_adapter;
static int implicit_admit(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    implicit_adapter *f = context;
    return f->base.source.admit(f->base.source.context, scope, family, name, semantics, error, capacity);
}
static int implicit_semantics(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    implicit_adapter *f = context;
    return f->base.source.semantics(f->base.source.context, scope, family, name, semantics, error, capacity);
}
static int implicit_atlas(void *context, size_t scope, const sh_package_compilation *compiled,
    const char *material, const char *page_file, sh_package_virtual_atlas *out, char *error, size_t capacity)
{
    implicit_adapter *f = context;
    return f->base.atlas(f->base.source.context, scope, compiled, material, page_file, out, error, capacity);
}
static int implicit_request(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visitor, char *error, size_t capacity)
{
    implicit_adapter *f = context;
    if (observing) {
        if (f->observed) assert(f->baseline == scope);
        f->baseline = scope; f->observed++;
        if (!unit->file_count) {
            assert(!unit->original_binding); f->absent++; return 1;
        }
    } else { assert(unit->file_count || unit->original_binding); f->admitted++; }
    if (!strcmp(unit->family, "entitydef") && !strcmp(unit->name, "props/rock")) {
        if (f->fail) { snprintf(error, capacity, "synthetic constructor dependencies unavailable"); return 0; }
        return visit(visitor, "constructor/image", "image", "textures/rock");
    }
    return 1;
}
static void test_implicit_roots(fixture *game, const sh_package_sources *sources,
    const sh_package_compile_environment *env, const sh_resource_catalog *catalog,
    sh_decl_registry_source registry, const sh_package_library_native *base)
{
    implicit_adapter f = {0}; f.base = *base;
    sh_package_library_native native = *base;
    native.source.context = &f; native.source.admit = implicit_admit;
    native.source.semantics = implicit_semantics; native.source.implicit = implicit_request;
    native.atlas = implicit_atlas;
    sh_package_library out = {0}, saved_library = {0}; char error[1024];
    game->implicit_root = 1; game->absent_target = 1;
    int ok = sh_package_library_compile(sources, env, catalog, original, game, registry,
        (uintptr_t)game->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "implicit reverse root: %s\n", error); assert(ok);
    assert(f.observed && f.admitted && f.absent && f.baseline >= sources->component_count);
    char phases[256]; /* The activation log names every phase of both levels. */
    sh_package_library_phases(&out, phases, sizeof(phases));
    assert(strstr(phases, "scopes ") && strstr(phases, ", source link ") && strstr(phases, ", link ") && strstr(phases, ", audio "));
    assert(strstr(out.source_phases, "readers ") && strstr(out.source_phases, ", discovery ") &&
        strstr(out.source_phases, ", certification ") && strstr(out.source_phases, ", inputs "));
    const sh_package_provenance_binding *red = binding(&out, sources, "Red", "", "entitydef", "props/rock");
    const sh_package_provenance_binding *green = binding(&out, sources, "Green", "", "entitydef", "props/rock");
    const sh_package_provenance_binding *copy = binding(&out, sources, "Red Copy", "", "entitydef", "props/rock");
    assert(red && green && copy && strcmp(red->name, green->name) && !strcmp(red->name, copy->name));
    const sh_package_provenance_binding *selector = binding(&out, sources, "Red", "", "snapeditorentitydef", "rock");
    assert(selector);
    assert(!resource(&out, "entitydef", "props/unrelated"));
    const sh_compiled_resource *root = resource(&out, "entitydef", red->name);
    const sh_package_provenance_binding *image = binding(&out, sources, "Red", "", "image", "textures/rock");
    const sh_compiled_resource *leaf = image ? resource(&out, "image", image->name) : NULL;
    assert(root && leaf);
    const sh_package_consumer *consumer = sh_package_consumers_find(out.consumers, "entitydef", red->name);
    const sh_package_consumer_target *request = sh_package_consumer_find(consumer, "constructor/image");
    assert(request && !strcmp(request->family, "image") && !strcmp(request->name, image->name));
    size_t retained = 0;
    for (size_t i = 0; i < root->bound_dependency_count; i++)
        if (!strcmp(root->bound_dependencies[i], leaf->engine_path)) retained++;
    assert(retained == 1);
    contains(&out, root, "edit = {}"); /* Graph discovery never fabricates file fields. */
    sh_package_provenance saved = {0};
    size_t selected = sh_package_provenance_context_add(&saved, SH_PACKAGE_CONTEXT_ISOLATED,
        (const unsigned char (*)[32])sh_package_source_tree(sources, owner(sources, "Red")), 1, "");
    assert(selected != SIZE_MAX && sh_package_provenance_bind(&saved, "entitydef", "props/rock", red->name, &selected, 1));
    assert(sh_package_provenance_bind(&saved, "snapeditorentitydef", "rock", selector->name, &selected, 1));
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/0", "entityDef"};
    assert(sh_package_provenance_use_set(&saved, &site, 0, &selected, 1));
    site.reference = "editorVars.inherit";
    assert(sh_package_provenance_use_set(&saved, &site, 1, &selected, 1));
    f.observed = f.admitted = f.absent = 0;
    ok = sh_package_library_compile_contexts(sources, &saved, env, catalog, original, game, registry,
        (uintptr_t)game->reflection, &native, &saved_library, error, sizeof(error));
    if (!ok) fprintf(stderr, "saved implicit root: %s\n", error); assert(ok);
    const char *replacement = NULL;
    assert(!f.observed && f.admitted && sh_package_provenance_resolve_use(&saved, 0, &saved_library.bindings, &replacement) == 1);
    assert(replacement && !strcmp(replacement, red->name));
    assert(sh_package_consumers_equal(out.consumers, saved_library.consumers, "entitydef", red->name));
    assert(sh_package_provenance_resolve(&saved, &saved_library.bindings, "snapeditorentitydef", selector->name, &replacement) == 1);
    assert(strcmp(replacement, "rock") && !strcmp(replacement, selector->name));
    sh_package_library_free(&saved_library); sh_package_provenance_free(&saved);
    f.fail = 1;
    assert(!sh_package_library_compile(sources, env, catalog, original, game, registry,
        (uintptr_t)game->reflection, &native, &out, error, sizeof(error)));
    assert(!out.compiled && !out.bindings.binding_count && strstr(error, "synthetic constructor dependencies unavailable"));
    sh_package_library_free(&out); game->implicit_root = game->absent_target = 0;
}

/* A declaration pointer naming a program neither SnapMap nor the scope has
 * ends observation there, like an absent declaration: the stock root keeps
 * its other edges, and the image it reaches still nominates its variant. */
static int program_abi(void *context, const char *name, sh_decl_source source, int *kind,
    uint32_t *index, char *error, size_t capacity)
{ (void)context; (void)name; (void)source; (void)kind; (void)index; (void)error; (void)capacity; return 0; }
static int program_flags(void *context, const char *name, sh_decl_source source, uint64_t *mask,
    char *error, size_t capacity)
{ (void)context; (void)name; (void)source; (void)mask; (void)error; (void)capacity; return 0; }
static void test_absent_program_boundary(fixture *f, const sh_package_sources *sources,
    const sh_package_compile_environment *env, const sh_resource_catalog *catalog,
    sh_decl_registry_source registry, const sh_package_library_native *base)
{
    sh_package_source_shader shader = {SH_PACKAGE_SHADER_VULKAN, f, program_abi, program_flags};
    sh_package_library_native native = *base; native.source.shader = &shader;
    sh_package_library out = {0}; char error[1024];
    f->absent_program = 1;
    int ok = sh_package_library_compile(sources, env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "absent program boundary: %s\n", error); assert(ok);
    if (out.coverage && strstr(out.coverage, "absentprog")) fprintf(stderr, "absent program: %s\n", out.coverage);
    assert(!out.coverage || !strstr(out.coverage, "absentprog"));
    const sh_package_provenance_binding *red = binding(&out, sources, "Red", "", "entitydef", "props/rock");
    assert(red);
    contains(&out, resource(&out, "entitydef", red->name), "program = \"absentprog\"");
    sh_package_library_free(&out); f->absent_program = 0;
}

/* Only content a package supplied can make it responsible for a failed
 * closure: its own refused unit is malformed; content reaching a unit the
 * product cannot link leaves it unserved; product-only chains blame nobody. */
static void test_failure_attribution(fixture *f, const sh_package_compile_environment *base,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *native)
{
    char path[4096], error[2048]; sh_package_library out = {0};
    size_t invalid = SIZE_MAX, unserved = SIZE_MAX;
    sh_package_compile_environment env = *base; env.invalid_package = &invalid; env.unserved_package = &unserved;
    create("attribution/overrides/red/package.json", "{\"id\":\"red\",\"name\":\"Red\"}");
    create("attribution/overrides/red/assets/generated/image/textures/rock.bimage", "red-image");
    create("attribution/overrides/empty/package.json", "{\"id\":\"empty\",\"name\":\"Empty\"}");
    create("attribution/overrides/broken/package.json", "{\"id\":\"broken\",\"name\":\"Broken\"}");
    create("attribution/overrides/broken/assets/generated/decls/entitydef/props/broken.decl",
        "{ class = \"Entity\"; edit = { model = \"models/campaign.lwo\"; } }");
    create("attribution/overrides/malformed/package.json", "{\"id\":\"malformed\",\"name\":\"Malformed\"}");
    /* The native reflected reader expects ';' after a scalar. */
    create("attribution/overrides/malformed/assets/generated/decls/entitydef/props/malformed.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"\r\n target = \"props/rock\"; } }");
    create("attribution/overrides/unsupported/package.json", "{\"id\":\"unsupported\",\"name\":\"Unsupported\"}");
    /* Well-formed text the product has no syntax adapter for: coverage. */
    create("attribution/overrides/unsupported/assets/generated/decls/entitydef/props/unsupported.decl",
        "{ class = \"Entity\"; edit = { model = { path = \"models/rock.lwo\"; } } }");
    snprintf(path, sizeof(path), "%s/attribution", root);
    sh_package_sources *sources = sh_package_sources_scan(path, error, sizeof(error)); assert(sources);
    assert(sh_package_sources_remove(sources, owner(sources, "Malformed")));
    assert(sh_package_sources_remove(sources, owner(sources, "Unsupported")));
    /* The package's own entity reaches a model SnapMap does not ship. That is
     * missing coverage for this delivery, not corruption of its files. */
    assert(!sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error)));
    if (unserved != owner(sources, "Broken") || invalid != SIZE_MAX) fprintf(stderr, "coverage owner: %s\n", error);
    assert(unserved == owner(sources, "Broken") && invalid == SIZE_MAX && !out.compiled);
    /* The chain names how its root entered the capture. */
    if (!strstr(error, "(via model models/campaign.lwo <- entitydef props/broken [scope ")) fprintf(stderr, "chain: %s\n", error);
    assert(strstr(error, "campaign dependency") &&
        strstr(error, "(via model models/campaign.lwo <- entitydef props/broken [scope ") &&
        strstr(error, " supplied generated/decls/entitydef/props/broken.decl])"));
    sh_package_library_free(&out); sh_package_sources_free(sources);
    /* Text the product has no syntax adapter for is coverage, not malformation. */
    sources = sh_package_sources_scan(path, error, sizeof(error)); assert(sources);
    assert(sh_package_sources_remove(sources, owner(sources, "Broken")));
    assert(sh_package_sources_remove(sources, owner(sources, "Malformed")));
    assert(!sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error)));
    if (unserved != owner(sources, "Unsupported") || invalid != SIZE_MAX) fprintf(stderr, "unsupported owner: %s\n", error);
    assert(unserved == owner(sources, "Unsupported") && invalid == SIZE_MAX &&
        strstr(error, "(via entitydef props/unsupported [scope ") &&
        strstr(error, " supplied generated/decls/entitydef/props/unsupported.decl])"));
    sh_package_library_free(&out); sh_package_sources_free(sources);
    /* Text that does not parse in the native grammar is malformed. */
    sources = sh_package_sources_scan(path, error, sizeof(error)); assert(sources);
    assert(sh_package_sources_remove(sources, owner(sources, "Broken")));
    assert(sh_package_sources_remove(sources, owner(sources, "Unsupported")));
    f->palette_reads = 0;
    assert(!sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error)));
    if (invalid != owner(sources, "Malformed") || unserved != SIZE_MAX) fprintf(stderr, "malformed owner: %s\n", error);
    assert(invalid == owner(sources, "Malformed") && unserved == SIZE_MAX);
    /* Supplied content is discovered first: its malformed file stops the
     * attempt before any stock palette entry is read for reverse roots. */
    if (f->palette_reads) fprintf(stderr, "palette reads before the malformed failure: %d\n", f->palette_reads);
    assert(!f->palette_reads);
    sh_package_library_free(&out);
    assert(sh_package_sources_remove(sources, invalid));
    int ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "healthy remainder: %s\n", error);
    assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX);
    assert(binding(&out, sources, "Red", "", "entitydef", "props/rock"));
    sh_package_library_free(&out);
    /* A model field names an md6 definition by its .md6 spelling; composition
     * tokens and linked names keep it, so the entity still binds. */
    create("attribution/overrides/gore/package.json", "{\"id\":\"gore\",\"name\":\"Gore\"}");
    create("attribution/overrides/gore/assets/generated/decls/entitydef/props/gored.decl",
        "{ class = \"Entity\"; edit = { model = \"gore/chunk.md6\"; } }");
    sh_package_sources *gore_sources = sh_package_sources_scan(path, error, sizeof(error)); assert(gore_sources);
    for (const char *const *name = (const char *const[]){"Broken", "Malformed", "Unsupported", NULL}; *name; name++)
        assert(sh_package_sources_remove(gore_sources, owner(gore_sources, *name)));
    ok = sh_package_library_compile(gore_sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "md6 model field: %s\n", error);
    assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX);
    sh_package_library_free(&out); sh_package_sources_free(gore_sources);
    /* A model SnapMap registers with no data loads defaulted natively; a
     * package naming it compiles with that name-only identity. */
    create("attribution/overrides/fileless/package.json", "{\"id\":\"fileless\",\"name\":\"Fileless\"}");
    create("attribution/overrides/fileless/assets/generated/decls/entitydef/props/fileless.decl",
        "{ class = \"Entity\"; edit = { model = \"models/fileless.lwo\"; } }");
    sh_package_sources *fileless_sources = sh_package_sources_scan(path, error, sizeof(error)); assert(fileless_sources);
    for (const char *const *name = (const char *const[]){"Broken", "Malformed", "Unsupported", NULL}; *name; name++)
        assert(sh_package_sources_remove(fileless_sources, owner(fileless_sources, *name)));
    ok = sh_package_library_compile(fileless_sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "fileless model: %s\n", error);
    assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX);
    sh_package_library_free(&out); sh_package_sources_free(fileless_sources);
    /* Transparency entries are native resources backed by dynamic atlas regions.
     * New and changed entries receive private identities like other resources. */
    create("attribution/overrides/atlas/package.json", "{\"id\":\"atlas\",\"name\":\"Atlas\"}");
    create("attribution/overrides/atlas/assets/generated/transsortatlas/textures/fx/campaignglow.bimage", "package-atlas");
    sh_package_sources *atlas_sources = sh_package_sources_scan(path, error, sizeof(error)); assert(atlas_sources);
    for (const char *const *name = (const char *const[]){"Broken", "Malformed", "Unsupported", NULL}; *name; name++)
        assert(sh_package_sources_remove(atlas_sources, owner(atlas_sources, *name)));
    ok = sh_package_library_compile(atlas_sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "atlas entry: %s\n", error);
    assert(ok && (!out.coverage || !strstr(out.coverage, "transparency atlas entry")));
    size_t atlas_count = 0;
    for (size_t i = 0; i < out.compiled->resource_count; i++) {
        const sh_compiled_resource *resource = out.compiled->resources + i;
        if (resource->native_type && !strcmp(resource->native_type, "transsortatlas")) {
            assert(strstr(resource->engine_path, "generated/transsortatlas/smp/")); atlas_count++;
        }
    }
    assert(atlas_count == 1);
    sh_package_library_free(&out); sh_package_sources_free(atlas_sources);
    create("attribution/overrides/atlas/assets/generated/transsortatlas/textures/fx/glow.bimage", "package-atlas");
    atlas_sources = sh_package_sources_scan(path, error, sizeof(error)); assert(atlas_sources);
    for (const char *const *name = (const char *const[]){"Broken", "Malformed", "Unsupported", NULL}; *name; name++)
        assert(sh_package_sources_remove(atlas_sources, owner(atlas_sources, *name)));
    ok = sh_package_library_compile(atlas_sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "replaced atlas entry: %s\n", error);
    assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX &&
        (!out.coverage || !strstr(out.coverage, "transparency atlas entry")));
    /* Nothing replaces the stock entry for other packages or vanilla maps. */
    assert(!sh_package_compilation_find(out.compiled, "generated/transsortatlas/textures/fx/glow.bimage"));
    sh_package_library_free(&out); sh_package_sources_free(atlas_sources);
    /* A product built-in reaching the same gap in every scope is a product
     * defect: the candidate fails and no package, not even an empty one, is
     * selected for exclusion. */
    sh_package_builtin builtin = {"generated/decls/entitydef/props/builtin.decl",
        (const unsigned char *)"{ class = \"Entity\"; edit = { model = \"models/campaign.lwo\"; } }", 0};
    builtin.length = strlen((const char *)builtin.body);
    env.builtins = &builtin; env.builtin_count = 1;
    assert(!sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error)));
    if (invalid != SIZE_MAX || unserved != SIZE_MAX) fprintf(stderr, "product defect: %s\n", error);
    assert(invalid == SIZE_MAX && unserved == SIZE_MAX && !out.compiled);
    assert(strstr(error, "via model models/campaign.lwo <- entitydef props/builtin") &&
        strstr(error, "reached only from product and stock content"));
    sh_package_library_free(&out); sh_package_sources_free(sources);
}

/* The player the engine spawns for co-op (the rules entity's playerSpawnDef,
 * default player/mp) names in handsAnimWeb the one web the hands animator
 * resolves every weapon's subweb in. A package changing that web (adding its
 * weapon's subweb) changes the global web: it is served under its stock name
 * whether or not a placed object reaches the player, and a stock root that
 * reaches it through the player gains no variant. A second such package
 * composes by native records. */
static void test_player_hands(fixture *f, const sh_package_compile_environment *base,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *native)
{
    char error[1024], held[MAX_PATH], home[MAX_PATH];
    sh_package_compile_environment env = *base;
    sh_package_library library = {0};
    /* The earlier Gap Image package changes this root's image; set it aside
     * so only the player reaches the web (observed by reverse discovery). */
    snprintf(home, sizeof(home), "%s\\overrides\\gap", root); snprintf(held, sizeof(held), "%s\\gap-held", root);
    assert(MoveFileA(home, held));
    /* The weapon also changes the stock pistol subweb: even as the web's only
     * supplier its copy aligns against the stock original, so the stock
     * pistol stays for everyone and the weapon owns its changed copy. */
    package("weapon", "Weapon", "generated/decls/animweb/hands.decl",
        "{ props { modelInfos { \"hands.md6\" \"\" } } states { } subWebs { subWeb \"pistol\" { "
        "node \"own\" { props { } blendTrees { } edges { } } } subWeb \"sp_pistol\" { } } }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    for (int hands = 1; hands <= 3; hands++) {
        f->hands = hands;
        int ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
            (uintptr_t)f->reflection, native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "player hands %d: %s\n", hands, error);
        assert(ok);
        const sh_compiled_resource *web = resource(&library, "animweb", "hands");
        if (!web) for (size_t i = 0; i < library.compiled->resource_count; i++)
            fprintf(stderr, "  %s %s\n", library.compiled->resources[i].engine_path, library.compiled->resources[i].name);
        assert(web); contains(&library, web, "sp_pistol");
        contains(&library, web, "subWeb \"pistol\" {"); contains(&library, web, "subWeb \"pistol_");
        assert(!binding(&library, sources, "Weapon", "", "snapeditorentitydef", "unrelated"));
        assert(!binding(&library, sources, "Weapon", "", "entitydef", "props/unrelated"));
        /* Every scope reaches the web by its stock name, so a stock model the
         * web names and that names the web again gains no private copy the
         * web's parse would have to load first. */
        int copied = published(&library, "md6def", "ae_web") || !holds(&library, web, "\"hands.md6\"");
        if (copied) for (size_t i = 0; i < library.compiled->resource_count; i++)
            fprintf(stderr, "  published %s %s\n", library.compiled->resources[i].engine_path,
                library.compiled->resources[i].native_name ? library.compiled->resources[i].native_name : "");
        assert(!copied);
        sh_package_library_free(&library);
    }
    sh_package_sources_free(sources);
    f->hands = 2;
    /* A second weapon package adds its own subweb: both join the stock web. */
    package("weapon2", "Weapon Two", "generated/decls/animweb/hands.decl",
        "{ props { modelInfos { \"hands.md6\" \"\" } } states { } subWebs { subWeb \"pistol\" { } subWeb \"plasma\" { } } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "player hands composed: %s\n", error);
    assert(ok);
    const sh_compiled_resource *web = resource(&library, "animweb", "hands");
    assert(web); contains(&library, web, "sp_pistol"); contains(&library, web, "plasma");
    sh_package_library_free(&library); sh_package_sources_free(sources);
    /* A third package defines sp_pistol differently: whichever package is
     * later in stable source order owns its copy under another name, and each
     * weapon names the subweb carrying its own nodes. */
    create("overrides/weapon/assets/generated/decls/weapon/first.decl",
        "{ edit = { subweb_normal = \"sp_pistol\"; handsHitReactionSubweb = \"pistol\"; } }");
    package("weapon3", "Weapon Three", "generated/decls/animweb/hands.decl",
        "{ props { modelInfos { \"hands.md6\" \"\" } } states { } subWebs { subWeb \"pistol\" { } "
        "subWeb \"sp_pistol\" { node \"third\" { props { } blendTrees { } edges { } } } } }");
    create("overrides/weapon3/assets/generated/decls/weapon/third.decl",
        "{ edit = { subweb_normal = \"sp_pistol\"; handsHitReactionSubweb = \"sp_pistol\"; } }");
    /* Reversing discovery order (renamed folders, same trees) keeps the same
     * package on the contested name. */
    int kept_by_third = -1;
    for (int round = 0; round < 2; round++) {
    if (round) {
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof(from), "%s\\overrides\\weapon", root); snprintf(to, sizeof(to), "%s\\overrides\\zz-weapon", root);
        assert(MoveFileA(from, to));
        snprintf(from, sizeof(from), "%s\\overrides\\weapon3", root); snprintf(to, sizeof(to), "%s\\overrides\\aa-weapon3", root);
        assert(MoveFileA(from, to));
    }
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "player hands variants %d: %s\n", round, error);
    assert(ok);
    web = resource(&library, "animweb", "hands");
    assert(web); contains(&library, web, "plasma");
    size_t weapons = 0;
    for (size_t i = 0; i < library.compiled->resource_count; i++) {
        const sh_compiled_resource *r = library.compiled->resources + i;
        if (!r->native_type || strcmp(r->native_type, "weapon")) continue;
        char text[512], name[128], block[160];
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(library.compiled, r, SIZE_MAX, &length, error, sizeof(error));
        assert(body && length < sizeof(text)); memcpy(text, body, length); text[length] = 0; free(body);
        const char *at = strstr(text, "subweb_normal = \"");
        assert(at); at += strlen("subweb_normal = \"");
        size_t n = strcspn(at, "\""); assert(n < sizeof(name)); memcpy(name, at, n); name[n] = 0;
        snprintf(block, sizeof(block), "subWeb \"%s\" {", name);
        assert(holds(&library, web, block));
        /* The named copy holds this weapon's own node. */
        char *served = NULL; size_t served_length = 0;
        unsigned char *raw_web = sh_package_compilation_read(library.compiled, web, SIZE_MAX, &served_length, error, sizeof(error));
        assert(raw_web); served = realloc(raw_web, served_length + 1); assert(served); served[served_length] = 0;
        const char *mine = strstr(served, block), *end = mine ? strstr(mine + 1, "subWeb \"") : NULL;
        /* The third weapon's hit reaction names its own subweb. */
        const char *hit_at = strstr(text, "handsHitReactionSubweb = \"");
        assert(hit_at); hit_at += strlen("handsHitReactionSubweb = \"");
        int third = !strncmp(hit_at, name, n) && hit_at[n] == '"';
        const char *node = mine ? strstr(mine, "node \"third\"") : NULL;
        if (third) assert(node && (!end || node < end));
        else assert(!node || (end && node > end));
        /* A hit-reaction subweb naming the same subweb follows it too. */
        /* The first weapon's hit reaction names the pistol copy its package
         * owns, never the stock pistol. */
        if (!third) assert(!strncmp(hit_at, "pistol_", 7));
        if (!strcmp(name, "sp_pistol")) {
            if (!round) kept_by_third = third;
            else assert(kept_by_third == third);
        }
        free(served);
        weapons++;
    }
    assert(weapons == 2 && kept_by_third >= 0);
    sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    {
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof(from), "%s\\overrides\\zz-weapon", root); snprintf(to, sizeof(to), "%s\\overrides\\weapon", root);
        assert(MoveFileA(from, to));
        snprintf(from, sizeof(from), "%s\\overrides\\aa-weapon3", root); snprintf(to, sizeof(to), "%s\\overrides\\weapon3", root);
        assert(MoveFileA(from, to));
    }
    f->hands = 0;
    assert(MoveFileA(held, home));
}

static int queue_original_program(void *context, const char *name, sh_decl_source source, char *error, size_t capacity)
{
    fixture *f=context;assert(!strcmp(name,"first") || !strcmp(name,"second"));
    assert(source.length==3 && !memcmp(source.text,"{ }",3));f->preparation_calls++;
    if(!strcmp(name,"second"))f->queued_second++;
    snprintf(error,capacity,"queued original %s",name);return 1;
}
static void reverse_preparation_frontier(fixture *f, const sh_resource_catalog *catalog)
{
    char error[1024];sh_compiled_resource file={0};
    file.engine_path="generated/image/textures/rock.bimage";file.type="image";file.name="textures/rock";
    file.body=(unsigned char *)"changed";file.body_length=7;
    assert(sh_package_owners_add(&file.gameplay_owners,0));
    sh_package_compilation changed={0},empty={0};changed.resources=&file;changed.resource_count=1;
    const sh_package_compilation *scopes[]={&changed,&empty};unsigned char discover[]={1,0};
    sh_package_source_root_plan plan={discover,1,NULL};sh_package_source_failure failure={0};
    sh_package_source_shader shader={SH_PACKAGE_SHADER_OPENGL,f,NULL,NULL,queue_original_program};
    sh_package_source_link_native native={0};native.shader=&shader;
    sh_decl_registry_source registry={f,read_memory,(uintptr_t)f->registry};
    f->prepare_programs=1;f->preparation_calls=0;
    assert(!sh_package_source_link_open_attributed(scopes,2,catalog,original,f,registry,(uintptr_t)f->reflection,
        &native,&plan,&failure,error,sizeof(error)));
    assert(f->preparation_calls==2 && failure.kind==SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT && strstr(error,"queued original first"));
    f->program_read_failure=1;f->preparation_calls=0;
    assert(!sh_package_source_link_open_attributed(scopes,2,catalog,original,f,registry,(uintptr_t)f->reflection,
        &native,&plan,&failure,error,sizeof(error)));
    assert(f->preparation_calls==1 && failure.kind==SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT && !strstr(error,"queued original"));
    f->prepare_programs=f->program_read_failure=0;sh_package_owners_free(&file.gameplay_owners);
}
/* Reverse changes through each scope's own canonicalizer: scope 0 supplies the
 * stock rock image, scope 1 an aliased copy its canonicalizer maps the stock
 * path to, scope 2 an unrelated gameplay file that changes no base unit, and
 * scope 3 the stock image's identity stored at another path, which only its
 * seeded unit (not any candidate path) ties to the stock unit. The indexed
 * decision is compared with the per-unit check for every scope
 * (reverse_oracle_checked), so a key built with the wrong canonicalizer, a
 * missed seeded supply, or a change missed or invented for the quiet scope is
 * a mismatch. */
static char *alias_rock_image(const char *path)
{
    char *key = sh_package_engine_path(path);
    if (key && !strcmp(key, "generated/image/textures/rock.bimage")) {
        free(key); key = _strdup("generated/image/textures/aliased_rock.bimage");
    }
    return key;
}
static void reverse_canonicalizer_scopes(fixture *f, const sh_resource_catalog *catalog)
{
    char error[1024]; size_t checked = 0, mismatches = 0, before = 0;
    sh_compiled_resource files[4] = {0};
    const char *paths[] = {"generated/image/textures/rock.bimage", "generated/image/textures/aliased_rock.bimage",
        "generated/image/textures/unrelated_quiet.bimage", "generated/image/textures/seeded_elsewhere.bimage"};
    const char *names[] = {"textures/rock", "textures/aliased_rock", "textures/unrelated_quiet", "textures/rock"};
    sh_package_compilation scopes_data[5] = {{0}};
    for (int i = 0; i < 4; i++) {
        files[i].engine_path = paths[i]; files[i].type = "image"; files[i].name = names[i];
        files[i].body = (unsigned char *)"changed"; files[i].body_length = 7; files[i].source = SIZE_MAX;
        assert(sh_package_owners_add(&files[i].gameplay_owners, 0));
        scopes_data[i].resources = files + i; scopes_data[i].resource_count = 1;
    }
    scopes_data[1].canonical_path = alias_rock_image;
    const sh_package_compilation *scopes[] = {scopes_data, scopes_data + 1, scopes_data + 2, scopes_data + 3,
        scopes_data + 4};
    unsigned char discover[] = {1, 1, 1, 1, 0};
    sh_package_source_root_plan plan = {discover, 4, NULL};
    sh_package_source_link_native native = {0};
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry};
    sh_package_source_link_test_reverse(&before, &mismatches);
    sh_package_source_link *s = sh_package_source_link_open_roots(scopes, 5, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &plan, error, sizeof(error));
    if (!s) fprintf(stderr, "reverse canonicalizer scopes: %s\n", error);
    assert(s);
    sh_package_source_link_test_reverse(&checked, &mismatches);
    assert(checked > before && !mismatches);
    sh_package_source_link_close(s);
    for (int i = 0; i < 4; i++) sh_package_owners_free(&files[i].gameplay_owners);
}
/* Indexed and per-unit reverse decisions for one direct capture; returns
 * whether the capture opened. The oracle (reverse_oracle_checked) also counts
 * them; here the decisions this capture added must exist and all agree. */
static int reverse_decisions_agree(fixture *f, const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, const sh_package_source_root_plan *plan, sh_package_source_link_native *native,
    const char *label)
{
    char error[1024]; size_t before = 0, checked = 0, mismatches = 0;
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry};
    sh_package_source_link_test_reverse(&before, &mismatches);
    assert(!mismatches);
    sh_package_source_link *s = sh_package_source_link_open_roots(scopes, count, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, plan, error, sizeof(error));
    sh_package_source_link_test_reverse(&checked, &mismatches);
    if (checked <= before || mismatches) fprintf(stderr, "%s: %zu decisions, %zu mismatched (%s)\n", label,
        checked - before, mismatches, s ? "opened" : error);
    assert(checked > before && !mismatches);
    sh_package_source_link_close(s);
    return s != NULL;
}
/* A change reached only through a catalog row: the stock rock image has a
 * second catalog row at a path no original holds, and scope 0 supplies a
 * gameplay-owned file exactly there (the baseline supplies it unowned, so the
 * nominated root still binds there). No file, cooked path or seeded identity
 * ties scope 0's file to the stock image. */
static void reverse_catalog_alias(fixture *f)
{
    char path[4096], error[1024];
    catalog_file("alias/snap_gameresources.pindex", 0, 1, 1); catalog_file("alias/gameresources.pindex", 1, 1, 0);
    create("alias/snap_gameresources.resources", ""); create("alias/snap_gameresources.patch", "");
    create("alias/gameresources.resources", ""); create("alias/gameresources.patch", "");
    snprintf(path, sizeof(path), "%s/alias", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(path, error, sizeof(error)); assert(catalog);
    sh_compiled_resource files[3] = {0};
    const char *paths[] = {"generated/image/textures/rock_catalog_alias.bimage",
        "generated/image/textures/unrelated_quiet.bimage", "generated/image/textures/rock_catalog_alias.bimage"};
    const char *names[] = {"textures/rock_catalog_alias", "textures/unrelated_quiet", "textures/rock_catalog_alias"};
    sh_package_compilation compiled[3] = {{0}};
    for (int i = 0; i < 3; i++) {
        files[i].engine_path = paths[i]; files[i].type = "image"; files[i].name = names[i];
        files[i].body = (unsigned char *)"changed"; files[i].body_length = 7; files[i].source = SIZE_MAX;
        if (i < 2) assert(sh_package_owners_add(&files[i].gameplay_owners, 0));
        compiled[i].resources = files + i; compiled[i].resource_count = 1;
    }
    const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
    unsigned char discover[] = {1, 1, 0};
    sh_package_source_root_plan plan = {discover, 2, NULL};
    sh_package_source_link_native native = {0};
    assert(reverse_decisions_agree(f, scopes, 3, catalog, &plan, &native, "catalog alias"));
    for (int i = 0; i < 2; i++) sh_package_owners_free(&files[i].gameplay_owners);
    sh_resource_catalog_close(catalog);
}
/* One Wwise bank (v113 shape) with one event and no actions. */
#define REVERSE_BANK_BYTES 49
static void reverse_put32(unsigned char *p, uint32_t v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void reverse_bank(unsigned char *bytes, uint32_t id, uint32_t event)
{
    memset(bytes, 0, REVERSE_BANK_BYTES); memcpy(bytes, "BKHD", 4); reverse_put32(bytes + 4, 16); reverse_put32(bytes + 8, 113);
    reverse_put32(bytes + 12, id);
    memcpy(bytes + 24, "HIRC", 4); reverse_put32(bytes + 28, 17); reverse_put32(bytes + 32, 1);
    bytes[36] = 4; reverse_put32(bytes + 37, 8); reverse_put32(bytes + 41, event);
}
/* A change reached only through audio: the stock root is the sound
 * sounds/noisy (a legacy document's candidate), and scope 0 supplies a bank
 * whose event is that sound's, with no file or seeded unit at any of its
 * paths. Whether the nominated sound then binds is not this case's question. */
static void reverse_audio_only(fixture *f, const sh_resource_catalog *catalog)
{
    unsigned char bank[REVERSE_BANK_BYTES]; uint32_t event = 0, id = 2166136261u; char error[1024];
    assert(sh_audio_event_identity("sounds/noisy", &event));
    for (const char *c = "noisy"; *c; c++) { id *= 16777619u; id ^= (unsigned char)*c; } /* the bank file's name */
    reverse_bank(bank, id, event);
    sh_compiled_resource files[2] = {0};
    files[0].engine_path = "sound/soundbanks/pc/noisy.bnk"; files[0].body = bank; files[0].body_length = sizeof(bank);
    files[0].source = SIZE_MAX;
    files[1].engine_path = "generated/image/textures/unrelated_quiet.bimage"; files[1].type = "image";
    files[1].name = "textures/unrelated_quiet"; files[1].body = (unsigned char *)"changed"; files[1].body_length = 7;
    files[1].source = SIZE_MAX; assert(sh_package_owners_add(&files[1].gameplay_owners, 0));
    sh_package_compilation compiled[3] = {{0}};
    compiled[0].resources = files; compiled[0].resource_count = 1;
    compiled[1].resources = files + 1; compiled[1].resource_count = 1;
    const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
    sh_package_references candidates = {0};
    assert(sh_package_references_add(&candidates, "sound", "sounds/noisy"));
    unsigned char discover[] = {1, 1, 0};
    sh_package_source_root_plan plan = {discover, 2, NULL, &candidates};
    /* No installed banks: the scope's bank is all the inventory there is. */
    char base[4096]; create("audio-empty/sound/soundbanks/pc/none.txt", "");
    snprintf(base, sizeof(base), "%s/audio-empty", root);
    sh_audio_originals *originals = sh_audio_originals_open(base, L"English(US)", NULL); assert(originals);
    sh_package_audio_program *audio = sh_package_audio_program_open(scopes, 3, originals, error, sizeof(error));
    assert(audio);
    sh_package_source_link_native native = {0}; native.audio = audio;
    (void)reverse_decisions_agree(f, scopes, 3, catalog, &plan, &native, "audio only");
    sh_package_audio_program_close(audio); sh_audio_originals_close(originals); sh_package_references_free(&candidates);
    sh_package_owners_free(&files[1].gameplay_owners);
}
/* Nine discovering scopes with nine different canonicalizers: the ninth has
 * no index table and takes the per-unit scan, and it is the one that supplies
 * the stock rock image. Each canonicalizer counts its calls so none is folded
 * into another. */
static volatile long canonical_calls[9];
#define REVERSE_CANONICAL(n) static char *reverse_canonical_##n(const char *path) \
    { canonical_calls[n]++; return sh_package_engine_path(path); }
REVERSE_CANONICAL(0) REVERSE_CANONICAL(1) REVERSE_CANONICAL(2) REVERSE_CANONICAL(3) REVERSE_CANONICAL(4)
REVERSE_CANONICAL(5) REVERSE_CANONICAL(6) REVERSE_CANONICAL(7) REVERSE_CANONICAL(8)
static void reverse_canonicalizer_fallback(fixture *f, const sh_resource_catalog *catalog)
{
    char *(*const canonical[9])(const char *) = {reverse_canonical_0, reverse_canonical_1, reverse_canonical_2,
        reverse_canonical_3, reverse_canonical_4, reverse_canonical_5, reverse_canonical_6, reverse_canonical_7,
        reverse_canonical_8};
    sh_compiled_resource files[9] = {0};
    sh_package_compilation compiled[10] = {{0}};
    const sh_package_compilation *scopes[10];
    unsigned char discover[10];
    for (int i = 0; i < 9; i++) {
        files[i].engine_path = i == 8 ? "generated/image/textures/rock.bimage" : "generated/image/textures/unrelated_quiet.bimage";
        files[i].type = "image"; files[i].name = i == 8 ? "textures/rock" : "textures/unrelated_quiet";
        files[i].body = (unsigned char *)"changed"; files[i].body_length = 7; files[i].source = SIZE_MAX;
        assert(sh_package_owners_add(&files[i].gameplay_owners, 0));
        compiled[i].resources = files + i; compiled[i].resource_count = 1; compiled[i].canonical_path = canonical[i];
        scopes[i] = compiled + i; discover[i] = 1;
    }
    scopes[9] = compiled + 9; discover[9] = 0;
    sh_package_source_root_plan plan = {discover, 9, NULL};
    sh_package_source_link_native native = {0};
    assert(reverse_decisions_agree(f, scopes, 10, catalog, &plan, &native, "canonicalizer fallback"));
    assert(canonical_calls[8]);
    for (int i = 0; i < 9; i++) sh_package_owners_free(&files[i].gameplay_owners);
}
/* A change reached only through a seeded witness: scope 0's page family
 * seeds the stock material generic/rock (material_seeds) and its companion is
 * a gameplay-owned page file (material_companions), while the material's own
 * declaration is the unowned stock one. No candidate path of any base unit is
 * the page file, so only the seeded supply's witnesses tie it to the stock
 * material. (Base units themselves never carry witnesses: the reverse base
 * link has no companion adapter.) */
typedef struct seeded_witness_native { const sh_compiled_resource *page; size_t seeds, companions; } seeded_witness_native;
static int seeded_witness_seeds(void *context, size_t scope, int (*visit)(void *, const char *), void *visit_context,
    char *error, size_t capacity)
{
    seeded_witness_native *n = context; (void)error; (void)capacity;
    if (scope) return 1;
    n->seeds++; return visit(visit_context, "generic/rock");
}
static int seeded_witness_companions(void *context, size_t scope, const char *material,
    int (*visit)(void *, const char *, const sh_compiled_resource *), void *visit_context, char *error, size_t capacity)
{
    seeded_witness_native *n = context; (void)error; (void)capacity;
    if (scope || strcmp(material, "generic/rock")) return 1;
    n->companions++; return visit(visit_context, "page", n->page);
}
static void reverse_seeded_witness(fixture *f, const sh_resource_catalog *catalog)
{
    sh_compiled_resource files[2] = {0};
    /* The companion is an image of its own, which no base unit names. */
    files[0].engine_path = "generated/image/textures/rock_page.bimage"; files[0].type = "image";
    files[0].name = "textures/rock_page"; files[0].body = (unsigned char *)"page"; files[0].body_length = 4;
    files[0].source = SIZE_MAX; assert(sh_package_owners_add(&files[0].gameplay_owners, 0));
    files[1].engine_path = "generated/image/textures/unrelated_quiet.bimage"; files[1].type = "image";
    files[1].name = "textures/unrelated_quiet"; files[1].body = (unsigned char *)"changed"; files[1].body_length = 7;
    files[1].source = SIZE_MAX; assert(sh_package_owners_add(&files[1].gameplay_owners, 0));
    sh_package_compilation compiled[3] = {{0}};
    compiled[0].resources = files; compiled[0].resource_count = 1;
    compiled[1].resources = files + 1; compiled[1].resource_count = 1;
    const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
    unsigned char discover[] = {1, 1, 0};
    sh_package_source_root_plan plan = {discover, 2, NULL};
    seeded_witness_native adapter = {files, 0, 0};
    sh_package_source_link_native native = {0};
    native.context = &adapter; native.material_seeds = seeded_witness_seeds;
    native.material_companions = seeded_witness_companions;
    (void)reverse_decisions_agree(f, scopes, 3, catalog, &plan, &native, "seeded witness");
    assert(adapter.seeds && adapter.companions);
    for (int i = 0; i < 2; i++) sh_package_owners_free(&files[i].gameplay_owners);
}
/* An attempt whose supplied content already waits for native preparation
 * fails, but first lets the stock observation queue what it needs too
 * (program "second" is reached only from the stock palette), so one
 * preparation round serves the next attempt. The attempt still reports its
 * own failure: the pending preparation, or a malformed scope, attributed to
 * it. A failure with nothing to prepare stops before any stock root is read. */
static void reverse_preparation_after_failure(fixture *f, const sh_resource_catalog *catalog)
{
    char error[1024];size_t supplied=0;
    sh_compiled_resource files[2]={0},bad={0};
    files[0].engine_path="generated/decls/entitydef/props/rock.decl";files[0].type="entitydef";files[0].name="props/rock";
    files[0].body=(unsigned char *)"{ class = \"Entity\"; edit = { program = \"first\"; } }";
    files[0].body_length=strlen((const char *)files[0].body);
    files[1].engine_path="generated/image/textures/rock.bimage";files[1].type="image";files[1].name="textures/rock";
    files[1].body=(unsigned char *)"changed";files[1].body_length=7;
    for(int i=0;i<2;i++){files[i].source=SIZE_MAX;assert(sh_package_owners_add(&files[i].gameplay_owners,0));}
    /* The native reflected reader expects ';' after a scalar. */
    bad.engine_path="generated/decls/entitydef/props/malformed.decl";bad.type="entitydef";bad.name="props/malformed";
    bad.body=(unsigned char *)"{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"\r\n target = \"props/rock\"; } }";
    bad.body_length=strlen((const char *)bad.body);bad.source=SIZE_MAX;bad.sources=&supplied;bad.source_count=1;
    sh_package_compilation pending={0},image={0},malformed={0},empty={0};
    pending.resources=files;pending.resource_count=2;image.resources=files+1;image.resource_count=1;
    malformed.resources=&bad;malformed.resource_count=1;
    sh_package_source_shader shader={SH_PACKAGE_SHADER_OPENGL,f,NULL,NULL,queue_original_program};
    sh_package_source_link_native native={0};native.shader=&shader;
    sh_decl_registry_source registry={f,read_memory,(uintptr_t)f->registry};
    unsigned char discover[]={1,0,0};
    f->prepare_programs=1;
    for(int round=0;round<3;round++) {
        /* 0: pending preparation; 1: pending and a malformed peer; 2: malformed only. */
        const sh_package_compilation *scopes[]={round==2 ? &image : &pending,&malformed,&empty};
        size_t count=round ? 3 : 2;
        if(!round)scopes[1]=&empty;
        sh_package_source_root_plan plan={discover,count-1,NULL};sh_package_source_failure failure={0};
        f->queued_second=f->palette_reads=f->preparation_calls=0;
        assert(!sh_package_source_link_open_attributed(scopes,count,catalog,original,f,registry,(uintptr_t)f->reflection,
            &native,&plan,&failure,error,sizeof(error)));
        int reported=!round ? failure.kind==SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT && strstr(error,"queued original first") :
            failure.kind==SH_PACKAGE_SOURCE_FAILURE_SOURCE && failure.scope==1 && strstr(failure.chain,"props/malformed");
        int observed=round<2 ? f->queued_second && f->palette_reads : !f->palette_reads && !f->preparation_calls;
        if(!reported || !observed)
            fprintf(stderr,"after failure round %d: kind %d scope %zu second %d palette %d calls %d: %s\n",round,
                (int)failure.kind,failure.scope,f->queued_second,f->palette_reads,f->preparation_calls,error);
        assert(reported && observed);
    }
    f->prepare_programs=0;
    for(int i=0;i<2;i++)sh_package_owners_free(&files[i].gameplay_owners);
}
/* Every scope's indexed reverse-change decision matched the per-unit check.
 * Checked at exit, after every scenario above has run. */
static void reverse_oracle_checked(void)
{
    size_t checked = 0, mismatches = 0;
    sh_package_source_link_test_reverse(&checked, &mismatches);
    if (!checked || mismatches) {
        fprintf(stderr, "reverse change oracle: %zu checked, %zu mismatched\n", checked, mismatches);
        _exit(3);
    }
    printf("reverse change oracle: %zu decisions checked, 0 mismatched\n", checked);
}
int main(void)
{
    atexit(reverse_oracle_checked);
    char temporary[MAX_PATH], path[4096], error[1024]; fixture f = {0}; setup(&f);
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-reverse-roots-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    catalog_file("base/snap_gameresources.pindex", 0, 1, 0); catalog_file("base/gameresources.pindex", 1, 1, 0);
    create("base/snap_gameresources.resources", ""); create("base/snap_gameresources.patch", "");
    create("base/gameresources.resources", ""); create("base/gameresources.patch", "");
    snprintf(path, sizeof(path), "%s/base", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(path, error, sizeof(error)); assert(catalog);
    reverse_preparation_frontier(&f,catalog);
    reverse_preparation_after_failure(&f,catalog);
    reverse_canonicalizer_scopes(&f,catalog);
    reverse_canonicalizer_fallback(&f,catalog);
    reverse_audio_only(&f,catalog);
    reverse_catalog_alias(&f);
    reverse_seeded_witness(&f,catalog);
    const char *image = "generated/image/textures/rock.bimage";
    package("a", "Red", image, "red-image"); package("b", "Green", image, "green-image");
    package("c", "Red Copy", image, "red-image"); package("d", "Original", image, "stock-image");
    package("e", "Original Copy", image, "stock-image");
    package("material", "Material", "generated/decls/material/generic/rock.decl", "{ tex \"textures/other\" }");
    package("model", "Model", "cooked/model/models/rock.bmodel", "");
    bytes changed_model = model("generic/other");
    binary_file("overrides/model/assets/cooked/model/models/rock.bmodel", changed_model.data, changed_model.length);
    package("redirect_a", "Redirect Red", image, "red-image");
    package("redirect_b", "Redirect Green", image, "green-image");
    create("overrides/redirect_a/assets/generated/decls/material/generic/rock.decl", "{ tex \"textures/other\" }");
    create("overrides/redirect_b/assets/generated/decls/material/generic/rock.decl", "{ tex \"textures/other\" }");
    package("nested", "Nested", image, "yellow-image");
    create("overrides/nested/child/package.json", "{\"id\":\"child\",\"name\":\"Child\"}");
    create("overrides/a/extra.txt", "complete source tree remains authored");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = &f;
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry};
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    sh_package_library library = {0};
    int ok = sh_package_library_compile(sources, &env, catalog, original, &f, registry,
        (uintptr_t)f.reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "reverse roots: %s\n", error); assert(ok);
    assert(library.bindings.semantics == SH_PACKAGE_PROVENANCE_CATALOG && !library.bindings.use_count);
    const char *families[] = {"entitydef", "snapeditorentitydef"};
    const char *originals[] = {"props/rock", "rock"};
    for (size_t i = 0; i < 2; i++) {
        const sh_package_provenance_binding *red = binding(&library, sources, "Red", "", families[i], originals[i]);
        const sh_package_provenance_binding *green = binding(&library, sources, "Green", "", families[i], originals[i]);
        const sh_package_provenance_binding *copy = binding(&library, sources, "Red Copy", "", families[i], originals[i]);
        const sh_package_provenance_binding *mat = binding(&library, sources, "Material", "", families[i], originals[i]);
        const sh_package_provenance_binding *mesh = binding(&library, sources, "Model", "", families[i], originals[i]);
        const sh_package_provenance_binding *ra = binding(&library, sources, "Redirect Red", "", families[i], originals[i]);
        const sh_package_provenance_binding *rb = binding(&library, sources, "Redirect Green", "", families[i], originals[i]);
        const sh_package_provenance_binding *outer = binding(&library, sources, "Nested", "", families[i], originals[i]);
        const sh_package_provenance_binding *child = binding(&library, sources, "Nested", "child", families[i], originals[i]);
        assert(red && green && copy && mat && mesh && ra && rb && outer && child);
        assert(strcmp(red->name, originals[i]) && strcmp(red->name, green->name) && !strcmp(red->name, copy->name));
        assert(!strcmp(mat->name, ra->name) && !strcmp(ra->name, rb->name));
        assert(strcmp(red->name, mat->name) && strcmp(red->name, mesh->name) && !strcmp(outer->name, child->name));
        assert(!binding(&library, sources, "Original", "", families[i], originals[i]));
        assert(resource(&library, families[i], originals[i]));
        if (i) contains(&library, resource(&library, families[i], red->name), "Props");
    }
    /* Nomination does not publish unrelated/campaign roots or create ownership
     * for the unchanged vanilla branch. Production semantics certifies its name. */
    assert(!resource(&library, "snapeditorentitydef", "unrelated"));
    assert(!resource(&library, "snapeditorentitydef", "campaign-only"));
    const sh_compiled_resource *stock = resource(&library, "entitydef", "props/rock");
    contains(&library, stock, "models/rock.lwo");
    assert(resource(&library, "model", "models/rock.lwo"));
    assert(resource(&library, "image", "textures/rock"));
    /* Reconstruct a saved occurrence from only its chosen source and original
     * derived entity identity. The source bundle contains no entity/selector. */
    sh_package_provenance saved = {0};
    const sh_package_provenance_binding *red = binding(&library, sources, "Red", "", "entitydef", "props/rock");
    size_t selected = sh_package_provenance_context_add(&saved, SH_PACKAGE_CONTEXT_ISOLATED,
        (const unsigned char (*)[32])sh_package_source_tree(sources, owner(sources, "Red")), 1, "");
    assert(selected != SIZE_MAX && sh_package_provenance_bind(&saved, "entitydef", "props/rock", red->name, &selected, 1));
    sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/0", "entityDef"};
    assert(sh_package_provenance_use_set(&saved, &site, 0, &selected, 1));
    sh_package_library recorded = {0};
    ok = sh_package_library_compile_contexts(sources, &saved, &env, catalog, original, &f, registry,
        (uintptr_t)f.reflection, &native, &recorded, error, sizeof(error));
    if (!ok) fprintf(stderr, "saved reverse root: %s\n", error); assert(ok);
    const char *replacement = NULL;
    assert(sh_package_provenance_resolve_use(&saved, 0, &recorded.bindings, &replacement) == 1);
    assert(replacement && !strcmp(replacement, red->name));
    for (size_t i = 0; i < recorded.bindings.binding_count; i++)
        assert(strcmp(recorded.bindings.bindings[i].family, "snapeditorentitydef"));
    sh_package_library_free(&recorded); sh_package_provenance_free(&saved);
    /* Historical image-only composition gains no unsolicited selector recipe. */
    sh_package_provenance historical = {0}; historical.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
    assert(sh_package_provenance_context_add(&historical, SH_PACKAGE_CONTEXT_COMPOSED,
        (const unsigned char (*)[32])sh_package_source_tree(sources, owner(sources, "Red")), 1, NULL) != SIZE_MAX);
    ok = sh_package_library_compile_contexts(sources, &historical, &env, catalog, original, &f, registry,
        (uintptr_t)f.reflection, &native, &recorded, error, sizeof(error));
    if (!ok) fprintf(stderr, "historical reverse root: %s\n", error); assert(ok);
    for (size_t i = 0; i < recorded.bindings.binding_count; i++)
        assert(strcmp(recorded.bindings.bindings[i].family, "snapeditorentitydef") &&
            strcmp(recorded.bindings.bindings[i].family, "entitydef"));
    sh_package_library_free(&recorded); sh_package_provenance_free(&historical);
    /* A typed original image and a native explicit-path spelling can serve
     * one exact physical file. Neither spelling supplies another source's data. */
    catalog_file("catalogless/snap_gameresources.pindex", 0, 0, 0);
    catalog_file("catalogless/gameresources.pindex", 1, 0, 0);
    create("catalogless/snap_gameresources.resources", ""); create("catalogless/snap_gameresources.patch", "");
    create("catalogless/gameresources.resources", ""); create("catalogless/gameresources.patch", "");
    snprintf(path, sizeof(path), "%s/catalogless", root);
    sh_resource_catalog *without_images = sh_resource_catalog_open(path, error, sizeof(error)); assert(without_images);
    sh_package_library aliases = {0};
    ok = sh_package_library_compile(sources, &env, without_images, original, &f, registry,
        (uintptr_t)f.reflection, &native, &aliases, error, sizeof(error));
    if (!ok) fprintf(stderr, "original image aliases: %s\n", error); assert(ok);
    const sh_compiled_resource *aliased = sh_package_compilation_find(aliases.compiled, image);
    assert(aliased && sh_package_resource_native_count(aliased) == 2);
    assert(sh_package_resource_native_matches(aliased, "image", "textures/rock"));
    assert(sh_package_resource_native_matches(aliased, "image", image));
    sh_package_changes changes = {0};
    assert(sh_package_compilation_changes(library.compiled, aliases.compiled, &changes, error, sizeof(error)));
    assert(changes.identities_complete);
    for (size_t i = 0; i < changes.count; i++) assert(strcmp(changes.items[i].path, image));
    sh_package_changes_free(&changes);
    sh_package_sources empty_sources = {0}; sh_package_compilation empty = {0}; empty.sources = &empty_sources;
    sh_package_compilation *overlay = sh_package_compilation_overlay(aliases.compiled, &empty, error, sizeof(error));
    assert(overlay && sh_package_resource_native_count(sh_package_compilation_find(overlay, image)) == 2);
    const sh_compiled_resource *input = aliased; size_t emitted_length = 0;
    unsigned char *emitted = sh_package_compilation_read(aliases.compiled, aliased, SIZE_MAX, &emitted_length, error, sizeof(error));
    assert(emitted);
    sh_package_resource_projection projection = {image, &input, 1, emitted, emitted_length,
        aliased->native_type, aliased->native_name};
    sh_package_compilation *projected = sh_package_compilation_project(sources, &projection, 1, NULL, NULL, error, sizeof(error));
    assert(projected && sh_package_resource_native_count(projected->resources) == 2);
    assert(projected->resources[0].native_aliases[0].name != aliased->native_aliases[0].name);
    sh_compiled_resource one = *aliased;
    one.native_type = "image"; one.native_name = "textures/rock"; one.native_aliases = NULL; one.native_alias_count = 0;
    const sh_compiled_resource *one_input = &one;
    sh_package_resource_projection one_projection = {image, &one_input, 1, emitted, emitted_length, "image", "textures/rock"};
    sh_package_compilation *single = sh_package_compilation_project(sources, &one_projection, 1, NULL, NULL, error, sizeof(error));
    assert(single && sh_package_compilation_changes(single, projected, &changes, error, sizeof(error)));
    assert(!changes.count && changes.added_count == 1 && !changes.removed_count && changes.identities_complete);
    assert(!strcmp(changes.added[0].type, "image") && !strcmp(changes.added[0].name, image));
    sh_package_changes_free(&changes);
    sh_package_compilation *combined = sh_package_compilation_overlay(projected, single, error, sizeof(error));
    assert(combined && sh_package_resource_native_count(combined->resources) == 2);
    assert(sh_package_resource_native_matches(combined->resources, "image", image));
    sh_package_compilation_free(combined);
    single->resources[0].body[0] ^= 1;
    combined = sh_package_compilation_overlay(projected, single, error, sizeof(error));
    assert(combined && sh_package_resource_native_count(combined->resources) == 1);
    assert(!sh_package_resource_native_matches(combined->resources, "image", image));
    sh_package_compilation_free(combined); sh_package_compilation_free(single);
    for (size_t i = 0; i < 2; i++) {
        const char *identity = i ? image : "textures/rock";
        char json[512]; snprintf(json, sizeof(json), "{\"targetType\":\"idDeclEntityDef\",\"className\":\"Entity\","
            "\"state\":{\"edit\":{\"image\":\"%s\"}}}", identity);
        sh_package_references references = {0}, paths = {0}; sh_package_source_graph_report report = {0};
        assert(sh_package_source_graph(projected, without_images, original, &f, registry,
            (uintptr_t)f.reflection, json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(!paths.incomplete && !report.gaps && paths.count == 1 && !strcmp(paths.items[0].name, image));
        sh_package_references_free(&paths);
        assert(sh_package_references_add(&references, "image", identity));
        assert(sh_package_map_resources(projected, without_images, "{}", 2, &references, &paths));
        assert(paths.count == 1 && !strcmp(paths.items[0].name, image));
        sh_package_references_free(&paths); sh_package_references_free(&references);
    }
    free(emitted); sh_package_compilation_free(projected); sh_package_compilation_free(overlay);
    /* Distinct selected trees and occurrence sites survive physical coalescing. */
    sh_package_provenance exact = {0}; const char *owners[] = {"Original", "Original Copy"};
    const char *native_names[] = {"textures/rock", image};
    for (size_t i = 0; i < 2; i++) {
        size_t context = sh_package_provenance_context_add(&exact, SH_PACKAGE_CONTEXT_ISOLATED,
            (const unsigned char (*)[32])sh_package_source_tree(sources, owner(sources, owners[i])), 1, "");
        assert(context == i && sh_package_provenance_bind(&exact, "image", native_names[i], native_names[i], &context, 1));
        sh_package_provenance_site occurrence = {SH_PACKAGE_USE_OBJECT, i ? "entity/1" : "entity/0", "edit.image"};
        assert(sh_package_provenance_use_set(&exact, &occurrence, i, &context, 1));
    }
    ok = sh_package_library_compile_contexts(sources, &exact, &env, without_images, original, &f, registry,
        (uintptr_t)f.reflection, &native, &recorded, error, sizeof(error));
    if (!ok) fprintf(stderr, "saved original aliases: %s\n", error); assert(ok);
    assert(recorded.compiled->resource_count == 1 && sh_package_resource_native_count(recorded.compiled->resources) == 2);
    for (size_t i = 0; i < 2; i++) {
        replacement = NULL;
        assert(sh_package_provenance_resolve_use(&exact, i, &recorded.bindings, &replacement) == 1);
        assert(!strcmp(replacement, native_names[i]) && exact.uses[i].contexts[0] == i);
    }
    sh_package_library_free(&recorded); sh_package_provenance_free(&exact);
    sh_package_library_free(&aliases); sh_resource_catalog_close(without_images);
    test_implicit_roots(&f, sources, &env, catalog, registry, &native);
    for (size_t i = 0; i < sources->file_count; i++) if (!sources->files[i].directory)
        assert(sh_package_source_verify(sources->files + i));
    /* A positively absent stock target is a candidate boundary. It cannot
     * invalidate unrelated packages or acquire native admission by observation. */
    f.absent_target = 1;
    ok = sh_package_library_compile(sources, &env, catalog, original, &f, registry,
        (uintptr_t)f.reflection, &native, &recorded, error, sizeof(error));
    if (!ok) fprintf(stderr, "absent unrelated target: %s\n", error); assert(ok);
    assert(!resource(&recorded, "snapeditorentitydef", "unrelated"));
    assert(!resource(&recorded, "entitydef", "props/unrelated"));
    assert(binding(&recorded, sources, "Red", "", "entitydef", "props/rock"));
    sh_package_library_free(&recorded);
    /* A stock declaration beyond the registered readers is accounted where it
     * occurs. Unrelated packages keep their variants; a package changing that
     * stock root's known dependency gets no guessed variant and a diagnosis. */
    f.absent_target = 0; f.reader_gap = 1;
    ok = sh_package_library_compile(sources, &env, catalog, original, &f, registry,
        (uintptr_t)f.reflection, &native, &recorded, error, sizeof(error));
    if (!ok) fprintf(stderr, "stock reader gap: %s\n", error); assert(ok);
    assert(binding(&recorded, sources, "Red", "", "entitydef", "props/rock"));
    if (!recorded.coverage || !strstr(recorded.coverage, "entitydef props/unrelated"))
        fprintf(stderr, "stock reader gap coverage: %s\n", recorded.coverage ? recorded.coverage : "(none)");
    assert(recorded.coverage && strstr(recorded.coverage, "entitydef props/unrelated") &&
        strstr(recorded.coverage, "gui") && !strstr(recorded.coverage, "no toybox variant"));
    sh_package_library_free(&recorded);
    package("gap", "Gap Image", "generated/image/textures/unrelated.bimage", "changed-unrelated");
    sh_package_sources *gap_sources = sh_package_sources_scan(root, error, sizeof(error)); assert(gap_sources);
    ok = sh_package_library_compile(gap_sources, &env, catalog, original, &f, registry,
        (uintptr_t)f.reflection, &native, &recorded, error, sizeof(error));
    if (!ok) fprintf(stderr, "stock reader gap with changed dependency: %s\n", error); assert(ok);
    assert(binding(&recorded, gap_sources, "Red", "", "entitydef", "props/rock"));
    assert(!binding(&recorded, gap_sources, "Gap Image", "", "snapeditorentitydef", "unrelated"));
    assert(!binding(&recorded, gap_sources, "Gap Image", "", "entitydef", "props/unrelated"));
    assert(strstr(recorded.coverage, "Gap Image [same]: no toybox variant for stock snapeditorentitydef unrelated"));
    for (size_t i = 0; i < gap_sources->file_count; i++) if (!gap_sources->files[i].directory)
        assert(sh_package_source_verify(gap_sources->files + i));
    sh_package_library_free(&recorded); sh_package_sources_free(gap_sources); f.reader_gap = 0;
    test_failure_attribution(&f, &env, catalog, registry, &native);
    test_absent_program_boundary(&f, sources, &env, catalog, registry, &native);
    test_player_hands(&f, &env, catalog, registry, &native);
    /* An unreadable baseline edge is a product coverage defect, never a package
     * invalidation or permission to publish a partially discovered library. */
    size_t invalid = 0; env.invalid_package = &invalid; f.missing_unrelated = 1;
    assert(!sh_package_library_compile(sources, &env, catalog, original, &f, registry,
        (uintptr_t)f.reflection, &native, &recorded, error, sizeof(error)));
    assert(!recorded.compiled && invalid == SIZE_MAX);
    assert(strstr(error, "baseline palette reverse dependency coverage is incomplete"));
    sh_package_library_free(&library); sh_package_native_assets_close(assets); sh_package_sources_free(sources);
    sh_resource_catalog_close(catalog); for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
    cleanup(); assert(!failures); puts("package reverse roots: PASS"); return 0;
}
