/* Downstream-only packages expose affected native editor roots in their scope. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_library.h"
#include "decl_block_compose.h"
#include "package_native_assets.h"
#include "package_audio_program.h"
#include "audio_banks.h"
#include "audio_originals.h"
#include "package_usage.h"
/* Every created directory level counts until the final cleanup; overflowing
 * the table silently leaves later fixtures' files uncreated. */
#define PACKAGE_FIXTURE_ENTRIES 2048u
#include "package_fixture.h"
#include "image_exclusion.h"
#include "../src/backend/virtual_material_runtime.c"
#include "library_output_dump.h"

void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; assert(0); return B2_PATCH_OK; }
int hook_unpatch(void *trampoline) { (void)trampoline; assert(0); return 0; }
void backend_log(const char *message) { (void)message; }

typedef struct region { void *address; size_t length; } region;
typedef struct fixture {
    region regions[192]; size_t count;
    void *registry, *reflection;
    int missing_unrelated, absent_target, implicit_root, reader_gap, absent_program, hands;
    int absent_material; /* the stock material generic/rock names image textures/absent, which no reader supplies (R-342) */
    int prepare_programs, preparation_calls, program_read_failure;
    int queued_second; /* preparations requested for program "second" */
    int palette_reads; /* stock palette entries read (reverse-root discovery) */
    int empty_parent; /* props/rock: 1 inherit = "", 2 a declaration-pointer target = "" */
    /* props/rock also names image 1 textures/partial (a campaign-only catalog
     * row before a SnapMap one) or 2 textures/campaign_image (campaign-only). */
    int partial_image;
    int labelled; /* the shipped rock row states a displayNameTag (reverse_label_aliases) */
    /* The installed textures/rock binary is an 8 x 8 bimage header whose first dword is all ones,
     * which a binary read could leave in the exclusion branch's state (image_exclusion_isolation). */
    int ambiguous_rock;
    /* props/unrelated and props/cyc_b target each other (a stock reference cycle, R-348). */
    int cycle;
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
        "idDeclRenderProg", "idDeclAnimWeb", "idDeclWeapon", "idUnmodelledPointer"};
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
    void *container = allocate(f, 0x30), *records = allocate(f, 14 * 0x38);
    void *objects = allocate(f, 14 * 16), *pointers = allocate(f, 14 * 16);
    pointer(f->reflection, 0, container); pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, 14);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, 14);
    pointer(pointers, 10 * 16 + 8, (void *)0x10); pointer(pointers, 11 * 16 + 8, (void *)0x10);
    for (size_t i = 0; i < 13; i++) {
        pointer(records, i * 0x38, text(f, classes[i]));
        pointer(records, i * 0x38 + 8, text(f, i == 3 || i == 8 || i == 11 ? "idDeclTypeInfo" : ""));
    }
    pointer(pointers, 8, (void *)0x10); pointer(pointers, 3 * 16 + 8, (void *)0x10);
    pointer(pointers, 4 * 16 + 8, (void *)0x40); pointer(pointers, 5 * 16 + 8, (void *)0x50);
    pointer(pointers, 6 * 16 + 8, (void *)0x60); pointer(objects, 7 * 16 + 8, (void *)0x70);
    pointer(objects, 8 * 16 + 8, (void *)0x30); pointer(pointers, 9 * 16 + 8, (void *)0x10);
    /* A pointer to a name that is neither a class nor an enum is skipped by
     * the native dispatch, so it is no gap. gui instead names a class whose
     * registered pointer reader (0x90) has no modelled body: a stock coverage gap. */
    pointer(pointers, 12 * 16 + 8, (void *)0x90);
    const char *et[] = {"idRenderModel", "idImage", "idDeclEntityDef", "idUnmodelledPointer", "idDeclRenderProg", "idDeclAnimWeb"};
    const char *eo[] = {"*", "*", "*", "*", "*", "*"};
    const char *en[] = {"model", "image", "target", "gui", "program", "handsAnimWeb"};
    fields(f, (char *)records + 0x38, 6, et, eo, en);
    const char *st[] = {"idDeclEntityDef", "idStr", "idStr", "idImage"};
    const char *so[] = {"*", "", "", "*"}, *sn[] = {"entityDef", "category", "displayName", "previewImage"};
    fields(f, (char *)records + 3 * 0x38, 4, st, so, sn);
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
        value = f->labelled ? "{ inherit = \"rock_base\"; edit = { entityDef = \"props/rock\"; categoryNameTag = \"#str_props\"; "
            "metaCategoryNameTag = \"#str_objects\"; displayNameTag = \"#str_rock\"; } }" :
            "{ edit = { entityDef = \"props/rock\"; category = \"Props\"; displayName = \"Rock\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/rock_base.decl"))
        value = "{ edit = { previewImage = \"textures/rock_icon\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/unrelated.decl")) {
        if (f->missing_unrelated) return -1;
        value = "{ edit = { entityDef = \"props/unrelated\"; category = \"Unrelated\"; } }";
    } else if (!strcmp(path, "generated/decls/entitydef/props/rock.decl"))
        value = f->empty_parent == 1 ? "{ inherit = \"\"; class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }" :
            f->empty_parent == 2 ? "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; target = \"\"; } }" :
            f->prepare_programs ? "{ class = \"Entity\"; edit = { program = \"first\"; } }" :
            f->implicit_root ? "{ class = \"Entity\"; edit = {} }" : f->partial_image == 1 ?
            "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; image = \"textures/partial\"; } }" :
            f->partial_image == 2 ?
            "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; image = \"textures/campaign_image\"; } }" :
            f->absent_program == 2 ?
            "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; program = \"campaignprog\"; } }" :
            f->absent_program ?
            "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; program = \"absentprog\"; } }" :
            "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }";
    else if (f->cycle && !strcmp(path, "generated/decls/entitydef/props/unrelated.decl"))
        value = "{ class = \"Entity\"; edit = { image = \"textures/unrelated\"; target = \"props/cyc_b\"; } }";
    else if (f->cycle && !strcmp(path, "generated/decls/entitydef/props/cyc_b.decl"))
        value = "{ class = \"Entity\"; edit = { target = \"props/unrelated\"; } }";
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
    else if (!strcmp(path, "generated/decls/material/generic/rock.decl"))
        value = f->absent_material ? "{ tex \"textures/absent\" }" : "{ tex \"textures/rock\" }";
    else if (!strcmp(path, "generated/decls/material/generic/other.decl")) value = "{ tex \"textures/other\" }";
    else if (!strcmp(path, "generated/decls/renderparm/tex.decl")) value = "{ Tex2D _white }";
    /* A stock renderparm whose own value is textures/rock, as renderparm/userenvmap names
     * env/default_hdr_prefiltered.bimage (R-309 r7). */
    else if (!strcmp(path, "generated/decls/renderparm/rockdefault.decl")) value = "{ TexCube textures/rock }";
    else if (!strcmp(path, "generated/decls/renderparm/landpagefile.decl")) value = "{ String }";
    else if (!strcmp(path, "generated/image/textures/rock.bimage") && f->ambiguous_rock) {
        static const unsigned char ones[24] = {0xff,0xff,0xff,0xff, 0x07,'M','I','B', 0,0,0,0, 0,0,0,8, 0,0,0,8};
        *length = sizeof(ones); *body = malloc(*length + 1); assert(*body);
        memcpy(*body, ones, *length); (*body)[*length] = 0; return 1;
    }
    else if (!strcmp(path, "generated/image/textures/rock.bimage")) value = "stock-image";
    else if (!strcmp(path, "generated/image/textures/other.bimage")) value = "other-image";
    else if (!strcmp(path, "generated/image/textures/rock_icon.bimage")) value = "stock-icon";
    else if (!strcmp(path, "generated/image/textures/unrelated.bimage")) value = "unrelated-image";
    else if (!strcmp(path, "generated/transsortatlas/textures/fx/glow.bimage")) value = "stock-atlas";
    else if (!strcmp(path, "generated/decls/md6def/gore/chunk.md6.decl")) value = "{ init { } }";
    else if (!strcmp(path, "generated/transsortatlas/textures/fx/campaignglow.bimage")) {
        *length = 14; *body = malloc(15); assert(*body); memcpy(*body, "campaign-atlas", 15); return 2;
    }
    else if (!strcmp(path, "generated/image/textures/partial_a.bimage") ||
        !strcmp(path, "generated/image/textures/campaign_image.bimage")) {
        *length = 14; *body = malloc(15); assert(*body); memcpy(*body, "campaign-image", 15); return 2;
    }
    else if (!strcmp(path, "generated/image/textures/partial_b.bimage")) value = "partial-image";
    else if (!strcmp(path, "generated/decls/renderprog/campaignprog.decl")) {
        /* A program only the campaign archive carries. */
        *length = 3; *body = malloc(4); assert(*body); memcpy(*body, "{ }", 4); return 2;
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
/* When set, catalog_file also lists the partial_image units: textures/partial
 * has a campaign-only row (partial_a) and a SnapMap row (partial_b), which
 * sort in that order; textures/campaign_image has only a campaign-only row. */
static int g_partial_rows;
static void catalog_file(const char *relative, int campaign, int images, int alias)
{
    bytes b = {0}; unsigned char head[0x20] = {5, 'S', 'E', 'R'}, tail[21] = {0};
    unsigned base = (campaign ? 2u : images ? 6u : 2u) + (alias ? 1u : 0u);
    unsigned count = base + (g_partial_rows ? (campaign ? 2u : 1u) : 0u);
    raw(&b, head, sizeof(head)); be(&b, count);
    for (unsigned i = 0; i < count; i++) {
        if (i >= base) {
            const char *paths[] = {"generated/image/textures/partial_b.bimage",
                "generated/image/textures/partial_a.bimage", "generated/image/textures/campaign_image.bimage"};
            unsigned row = campaign ? 1u + (i - base) : 0u;
            be(&b, i); string(&b, "image"); string(&b, row == 2 ? "textures/campaign_image" : "textures/partial");
            string(&b, paths[row]); raw(&b, tail, sizeof(tail)); continue;
        }
        if (alias && i == base - 1) {
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
    sh_package_library_free(&out);
    /* A program only the campaign archive carries, named by that stock root,
     * still refuses: a shader companion has no stock-route identity (sl_path). */
    f->absent_program = 2;
    ok = sh_package_library_compile(sources, env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (ok || !strstr(error, "campaign dependency is absent from its authored scope: generated/decls/renderprog/campaignprog.decl"))
        fprintf(stderr, "campaign program: %s\n", ok ? "served" : error);
    assert(!ok && strstr(error, "campaign dependency is absent from its authored scope: generated/decls/renderprog/campaignprog.decl"));
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

/* A stock root's own typed reference to an image only the campaign archive
 * carries is kept as an identity: the package whose change nominated that
 * root is served, and the root's text names the image literally. An image
 * SnapMap carries only in part stays a campaign dependency, although its
 * campaign-only row is listed before its SnapMap row. */
static void test_stock_route_campaign_boundary(fixture *f, const sh_package_compile_environment *base,
    sh_decl_registry_source registry, const sh_package_library_native *native)
{
    char path[4096], error[2048]; sh_package_library out = {0};
    size_t invalid = SIZE_MAX, unserved = SIZE_MAX;
    sh_package_compile_environment env = *base; env.invalid_package = &invalid; env.unserved_package = &unserved;
    g_partial_rows = 1;
    catalog_file("partial-catalog/snap_gameresources.pindex", 0, 1, 0);
    catalog_file("partial-catalog/gameresources.pindex", 1, 1, 0);
    g_partial_rows = 0;
    create("partial-catalog/snap_gameresources.resources", ""); create("partial-catalog/snap_gameresources.patch", "");
    create("partial-catalog/gameresources.resources", ""); create("partial-catalog/gameresources.patch", "");
    snprintf(path, sizeof(path), "%s/partial-catalog", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(path, error, sizeof(error)); assert(catalog);
    create("partial/overrides/red/package.json", "{\"id\":\"red\",\"name\":\"Red\"}");
    create("partial/overrides/red/assets/generated/image/textures/rock.bimage", "red-image");
    snprintf(path, sizeof(path), "%s/partial", root);
    sh_package_sources *sources = sh_package_sources_scan(path, error, sizeof(error)); assert(sources);
    f->partial_image = 2;
    int ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "stock route campaign boundary: %s\n", error);
    assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX);
    const sh_package_provenance_binding *red = binding(&out, sources, "Red", "", "entitydef", "props/rock");
    assert(red);
    contains(&out, resource(&out, "entitydef", red->name), "image = \"textures/campaign_image\"");
    sh_package_library_free(&out);
    f->partial_image = 1;
    ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, native, &out, error, sizeof(error));
    if (ok || !strstr(error, "campaign dependency is absent from its authored scope: textures/partial"))
        fprintf(stderr, "partial campaign image: %s (invalid %zu, unserved %zu)\n", ok ? "served" : error, invalid, unserved);
    assert(!ok && !out.compiled && strstr(error, "campaign dependency is absent from its authored scope: textures/partial"));
    sh_package_library_free(&out); f->partial_image = 0;
    /* Two independent packages whose changes nominate that root fail together,
     * in either scope order. Each copy of the root fails in its own scope, and
     * the root's baseline copy then fails as no package's defect: the failed
     * compile still judges the other package and reports both, one as the
     * failure and one among the further rejections, each as not served here
     * (coverage, not malformed bytes), instead of one per compile. The healthy
     * peer is then served with its own text. */
    for (int order = 0; order < 2; order++) {
        sh_package_rejections more = {0};
        char folder[160];
        const char *first = order ? "blue" : "red", *second = order ? "red" : "blue";
        snprintf(folder, sizeof(folder), "partial-batch-%d/overrides/a-%s/package.json", order, first);
        create(folder, order ? "{\"id\":\"blue\",\"name\":\"Blue\"}" : "{\"id\":\"red\",\"name\":\"Red\"}");
        snprintf(folder, sizeof(folder), "partial-batch-%d/overrides/a-%s/assets/generated/image/textures/rock.bimage", order, first);
        create(folder, order ? "blue-image" : "red-image");
        snprintf(folder, sizeof(folder), "partial-batch-%d/overrides/m-healthy/package.json", order);
        create(folder, "{\"id\":\"healthy\",\"name\":\"Healthy\"}");
        snprintf(folder, sizeof(folder), "partial-batch-%d/overrides/m-healthy/assets/generated/decls/entitydef/props/healthy.decl", order);
        create(folder, "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
        snprintf(folder, sizeof(folder), "partial-batch-%d/overrides/z-%s/package.json", order, second);
        create(folder, order ? "{\"id\":\"red\",\"name\":\"Red\"}" : "{\"id\":\"blue\",\"name\":\"Blue\"}");
        snprintf(folder, sizeof(folder), "partial-batch-%d/overrides/z-%s/assets/generated/image/textures/rock.bimage", order, second);
        create(folder, order ? "red-image" : "blue-image");
        snprintf(path, sizeof(path), "%s/partial-batch-%d", root, order);
        sh_package_sources *batch = sh_package_sources_scan(path, error, sizeof(error)); assert(batch);
        size_t red = owner(batch, "Red"), blue = owner(batch, "Blue"), healthy = owner(batch, "Healthy");
        env.more_rejections = &more; f->partial_image = 1;
        ok = sh_package_library_compile(batch, &env, catalog, original, f, registry,
            (uintptr_t)f->reflection, native, &out, error, sizeof(error));
        int together = !ok && !out.compiled && invalid == SIZE_MAX && more.count == 1 && !more.items[0].invalid &&
            !more.items[0].own_bytes &&
            ((unserved == red && more.items[0].package == blue) || (unserved == blue && more.items[0].package == red));
        if (!together)
            fprintf(stderr, "independent stock-route owners, order %d: %s (unserved %zu, %zu more, first %zd; red %zu blue %zu healthy %zu)\n",
                order, ok ? "served" : error, unserved, more.count, more.count ? (ptrdiff_t)more.items[0].package : (ptrdiff_t)-1,
                red, blue, healthy);
        assert(together && unserved != healthy);
        sh_package_rejections_free(&more); sh_package_library_free(&out);
        env.more_rejections = NULL;
        assert(sh_package_sources_remove(batch, red > blue ? red : blue));
        assert(sh_package_sources_remove(batch, red > blue ? blue : red));
        ok = sh_package_library_compile(batch, &env, catalog, original, f, registry,
            (uintptr_t)f->reflection, native, &out, error, sizeof(error));
        const sh_package_provenance_binding *served = ok ? binding(&out, batch, "Healthy", "", "entitydef", "props/healthy") : NULL;
        const sh_compiled_resource *text = served ? resource(&out, "entitydef", served->name) : NULL;
        if (!ok || invalid != SIZE_MAX || unserved != SIZE_MAX || !text)
            fprintf(stderr, "healthy peer after both, order %d: %s (invalid %zu, unserved %zu, binding %d, resource %d)\n",
                order, ok ? "served" : error, invalid, unserved, served != NULL, text != NULL);
        assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX && text);
        contains(&out, text, "model = \"models/rock.lwo\"");
        sh_package_library_free(&out); sh_package_sources_free(batch); f->partial_image = 0;
    }
    sh_package_sources_free(sources); sh_resource_catalog_close(catalog);
}

/* The runtime's registered-reader route for block-grammar families
 * (package_runtime.c pr_compose_custom): the player hands web composes by
 * native records and refuses two different changes to one record. */
static int block_compose(void *context, const char *type, sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, char **body, size_t *length,
    char *error, size_t capacity, sh_decl_conflict *conflict, const sh_package_source_view *view)
{
    (void)context; (void)view;
    *body = NULL; *length = 0;
    if (!sh_decl_block_family(type)) return 0;
    *body = sh_decl_block_compose(type, baseline, sources, count, length, error, capacity, conflict);
    return *body ? 1 : -1;
}
static char *read_text(const char *relative, size_t *length)
{
    char path[4096]; snprintf(path, sizeof(path), "%s/%s", root, relative);
    FILE *file = fopen(path, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size >= 0); rewind(file);
    char *text = malloc((size_t)size + 1); assert(text);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size); assert(!fclose(file));
    text[size] = 0; *length = (size_t)size; return text;
}
/* An alignment moving one package's subweb to two names, as a failing
 * sh_decl_animweb_prepare caller would see it: every copy is rewritten. */
static int prepare_twice(sh_decl_source baseline, sh_decl_source *sources, size_t count, char **owned,
    struct sh_decl_animweb_move **moves, size_t *move_count, char *error, size_t capacity)
{
    (void)baseline; (void)error; (void)capacity;
    for (size_t k = 0; k < count; k++) {
        assert((owned[k] = _strdup("{ rewritten }")));
        sources[k] = (sh_decl_source){owned[k], strlen(owned[k])};
    }
    sh_decl_animweb_move *twice = calloc(2, sizeof(*twice)); assert(twice);
    twice[0] = (sh_decl_animweb_move){0, _strdup("sp_pistol"), _strdup("sp_pistol_a")};
    twice[1] = (sh_decl_animweb_move){0, _strdup("sp_pistol"), _strdup("sp_pistol_b")};
    assert(twice[0].from && twice[0].to && twice[1].from && twice[1].to);
    *moves = twice; *move_count = 2; return 1;
}
/* The quoted value of a weapon field. */
static void field_value(const char *text, const char *field, char *out, size_t capacity)
{
    char key[128]; snprintf(key, sizeof(key), "%s = \"", field);
    const char *at = strstr(text, key); assert(at); at += strlen(key);
    size_t n = strcspn(at, "\""); assert(n < capacity); memcpy(out, at, n); out[n] = 0;
}
/* 1 when the web has subweb `name` and its block holds needle. */
static int subweb_holds(const char *web, const char *name, const char *needle)
{
    char block[160]; snprintf(block, sizeof(block), "subWeb \"%s\" {", name);
    const char *mine = strstr(web, block);
    if (!mine) return 0;
    const char *end = strstr(mine + 1, "subWeb \""), *found = strstr(mine, needle);
    return found && (!end || found < end);
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
        "node \"own\" { props { } blendTrees { } edges { } } } subWeb \"sp_pistol\" { node \"idle\" { props { } "
        "blendTrees { tree { modelIndex 0 blendEq \"none\" anims { alias { name \"first_idle\" } } } } edges { } } } } }");
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
        "subWeb \"sp_pistol\" { node \"idle\" { props { } blendTrees { tree { modelIndex 0 blendEq \"none\" anims { alias { name \"third_idle\" } } } } edges { } } "
        "node \"third\" { props { } blendTrees { } edges { } } } } }");
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
    /* A map's historical context composing the contesting packages in one
     * scope keeps every one of them too. The scope composes their copies by
     * the runtime's custom block route, so each package's copy aligns against
     * the stock web there, and each package's weapons then name the subweb
     * copies carrying that package's own records, in any discovery or context
     * order. Each weapon's package comes from its provenance, never from the
     * generated subweb names, and no source byte changes. A fourth package
     * carrying an exact copy of the third's web shares its subweb. */
    {
        {
            size_t length = 0;
            char *copy = read_text("overrides/weapon3/assets/generated/decls/animweb/hands.decl", &length);
            package("weapon4", "Weapon Four", "generated/decls/animweb/hands.decl", copy); free(copy);
            create("overrides/weapon4/assets/generated/decls/weapon/fourth.decl",
                "{ edit = { subweb_normal = \"sp_pistol\"; handsHitReactionSubweb = \"sp_pistol\"; } }");
        }
        static const char *const inputs[] = {
            "overrides/weapon/assets/generated/decls/animweb/hands.decl",
            "overrides/weapon2/assets/generated/decls/animweb/hands.decl",
            "overrides/weapon3/assets/generated/decls/animweb/hands.decl",
            "overrides/weapon/assets/generated/decls/weapon/first.decl",
            "overrides/weapon3/assets/generated/decls/weapon/third.decl",
            "overrides/weapon4/assets/generated/decls/animweb/hands.decl",
            "overrides/weapon4/assets/generated/decls/weapon/fourth.decl"};
        enum { INPUTS = sizeof(inputs) / sizeof(inputs[0]) };
        char *before[INPUTS]; size_t before_length[INPUTS];
        for (size_t i = 0; i < INPUTS; i++) before[i] = read_text(inputs[i], before_length + i);
        /* Each contesting copy alone is valid for the production composer;
         * together they are the native same-record collision. */
        {
            unsigned char *stock = NULL; size_t stock_length = 0;
            assert(original(f, "generated/decls/animweb/hands.decl", &stock, &stock_length) > 0 && stock);
            sh_decl_source stock_web = {(const char *)stock, stock_length};
            sh_decl_source both[2] = {{before[0], before_length[0]}, {before[2], before_length[2]}};
            sh_decl_conflict conflict = {SIZE_MAX, SIZE_MAX, 0, 0};
            size_t length = 0;
            for (size_t i = 0; i < 2; i++) {
                char *alone = sh_decl_block_compose("animweb", stock_web, both + i, 1, &length, error, sizeof(error), &conflict);
                if (!alone) fprintf(stderr, "player hands copy %zu alone: %s\n", i, error);
                assert(alone); free(alone);
            }
            char *together = sh_decl_block_compose("animweb", stock_web, both, 2, &length, error, sizeof(error), &conflict);
            fprintf(stderr, "player hands copies together: %s\n", together ? "composed" : error);
            assert(!together && conflict.first == 0 && conflict.second == 1);
            free(stock);
        }
        /* Opening a map without recorded contexts first composes all of its
         * carried packages as one scope (package_runtime.c historical carrier).
         * A plain whole compile meets the native collision; the composed
         * library entry aligns each package's copy, and each package's weapons
         * name the copy holding that package's own nodes. */
        {
            sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
            /* The map carries just the four weapon packages. */
            size_t carried[4] = {owner(sources, "Weapon"), owner(sources, "Weapon Two"),
                owner(sources, "Weapon Three"), owner(sources, "Weapon Four")};
            for (size_t i = sources->package_count; i; i--)
                if (i - 1 != carried[0] && i - 1 != carried[1] && i - 1 != carried[2] && i - 1 != carried[3])
                    assert(sh_package_sources_remove(sources, i - 1));
            assert(sources->package_count == 4);
            sh_package_compile_environment whole = env;
            whole.compose_custom = block_compose; whole.baseline = original; whole.baseline_context = f;
            sh_package_compilation *plain = sh_package_compile_with(sources, &whole, error, sizeof(error));
            fprintf(stderr, "player hands whole map, plain compile: %s\n", plain ? "composed" : error);
            assert(!plain && strstr(error, "incompatible edits"));
            sh_package_compilation *aligned = sh_package_library_compile_composed(sources, &whole, error, sizeof(error));
            if (!aligned) fprintf(stderr, "player hands whole map, composed entry: %s\n", error);
            assert(aligned);
            const sh_compiled_resource *whole_web = NULL;
            for (size_t i = 0; i < aligned->resource_count; i++)
                if (aligned->resources[i].engine_path && !strcmp(aligned->resources[i].engine_path, "generated/decls/animweb/hands.decl"))
                    whole_web = aligned->resources + i;
            assert(whole_web);
            size_t web_length = 0;
            unsigned char *raw = sh_package_compilation_read(aligned, whole_web, SIZE_MAX, &web_length, error, sizeof(error));
            assert(raw); char *served = realloc(raw, web_length + 1); assert(served); served[web_length] = 0;
            size_t first_owner = owner(sources, "Weapon"), third_owner = owner(sources, "Weapon Three"),
                fourth_owner = owner(sources, "Weapon Four"), weapons = 0;
            char third_normal[128] = "";
            for (size_t i = 0; i < aligned->resource_count; i++) {
                const sh_compiled_resource *r = aligned->resources + i;
                if (!r->type || strcmp(r->type, "weapon")) continue;
                size_t source = r->source != SIZE_MAX ? r->source : r->source_count ? r->sources[0] : SIZE_MAX;
                assert(source < aligned->sources->file_count);
                size_t who = aligned->sources->files[source].owner;
                char text[512], normal[128]; size_t length = 0;
                unsigned char *body = sh_package_compilation_read(aligned, r, SIZE_MAX, &length, error, sizeof(error));
                assert(body && length < sizeof(text)); memcpy(text, body, length); text[length] = 0; free(body);
                field_value(text, "subweb_normal", normal, sizeof(normal));
                assert(subweb_holds(served, normal, "subWeb"));
                if (who == first_owner)
                    assert(subweb_holds(served, normal, "\"first_idle\"") && !subweb_holds(served, normal, "\"third_idle\""));
                else {
                    assert(who == third_owner || who == fourth_owner);
                    assert(subweb_holds(served, normal, "\"third_idle\"") && !subweb_holds(served, normal, "\"first_idle\""));
                    /* The exact copies of two packages share one subweb. */
                    if (third_normal[0]) assert(!strcmp(third_normal, normal));
                    else snprintf(third_normal, sizeof(third_normal), "%s", normal);
                }
                weapons++;
            }
            fprintf(stderr, "player hands whole map, composed entry: %zu weapons on their own copies\n", weapons);
            assert(weapons == 3 && third_normal[0]);
            free(served); sh_package_compilation_free(aligned); sh_package_sources_free(sources);
        }
        int kept_by_third = -1;
        char *first_web = NULL; size_t first_web_length = 0;
        for (int round = 0; round < 3; round++) {
            /* Round 1 reverses discovery (renamed folders, same trees); round 2
             * also reverses the context's tree order. */
            if (round == 1) {
                char from[MAX_PATH], to[MAX_PATH];
                snprintf(from, sizeof(from), "%s\\overrides\\weapon", root); snprintf(to, sizeof(to), "%s\\overrides\\zz-weapon", root);
                assert(MoveFileA(from, to));
                snprintf(from, sizeof(from), "%s\\overrides\\weapon3", root); snprintf(to, sizeof(to), "%s\\overrides\\aa-weapon3", root);
                assert(MoveFileA(from, to));
            }
            sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
            const char *names[4] = {"Weapon", "Weapon Two", "Weapon Three", "Weapon Four"};
            size_t owners[4];
            unsigned char trees[4][32];
            for (size_t i = 0; i < 4; i++) {
                owners[i] = owner(sources, names[i]);
                memcpy(trees[round == 2 ? 3 - i : i], sh_package_source_tree(sources, owners[i]), 32);
            }
            sh_package_provenance historical = {0}; historical.semantics = SH_PACKAGE_PROVENANCE_HISTORICAL;
            assert(sh_package_provenance_context_add(&historical, SH_PACKAGE_CONTEXT_COMPOSED,
                (const unsigned char (*)[32])trees, 4, NULL) != SIZE_MAX);
            sh_package_compile_environment composed_env = env;
            composed_env.compose_custom = block_compose;
            ok = sh_package_library_compile_contexts(sources, &historical, &composed_env, catalog, original, f, registry,
                (uintptr_t)f->reflection, native, &library, error, sizeof(error));
            if (!ok) fprintf(stderr, "player hands composed context %d: %s\n", round, error);
            assert(ok);
            web = resource(&library, "animweb", "hands");
            assert(web);
            size_t length_of_web = 0;
            unsigned char *raw = sh_package_compilation_read(library.compiled, web, SIZE_MAX, &length_of_web, error, sizeof(error));
            assert(raw); char *served = realloc(raw, length_of_web + 1); assert(served); served[length_of_web] = 0;
            /* The stock pistol stays for everyone and the disjoint addition joins. */
            assert(strstr(served, "subWeb \"plasma\" {"));
            assert(strstr(served, "subWeb \"pistol\" {") && !subweb_holds(served, "pistol", "node \"own\""));
            size_t weapons = 0, seen[4] = {0};
            char shared[2][128] = {"", ""};
            for (size_t i = 0; i < library.compiled->resource_count; i++) {
                const sh_compiled_resource *r = library.compiled->resources + i;
                if (!r->native_type || strcmp(r->native_type, "weapon")) continue;
                const sh_package_sources *supplied = library.compiled->sources;
                size_t source = r->source != SIZE_MAX ? r->source : r->source_count ? r->sources[0] : SIZE_MAX;
                assert(supplied && source < supplied->file_count);
                size_t who = 4;
                for (size_t k = 0; k < 4; k++) if (supplied->files[source].owner == owners[k]) who = k;
                if (who == 4) fprintf(stderr, "weapon %s supplied by package %zu\n", r->engine_path, supplied->files[source].owner);
                assert(who == 0 || who == 2 || who == 3); seen[who]++;
                char text[512], normal[128], hit[128]; size_t length = 0;
                unsigned char *body = sh_package_compilation_read(library.compiled, r, SIZE_MAX, &length, error, sizeof(error));
                assert(body && length < sizeof(text)); memcpy(text, body, length); text[length] = 0; free(body);
                field_value(text, "subweb_normal", normal, sizeof(normal));
                field_value(text, "handsHitReactionSubweb", hit, sizeof(hit));
                /* Each weapon names subwebs the served web has, holding its own
                 * package's nodes and none of the other's. */
                assert(subweb_holds(served, normal, "subWeb") && subweb_holds(served, hit, "subWeb"));
                if (who >= 2) {
                    assert(subweb_holds(served, normal, "node \"third\"") && subweb_holds(served, normal, "\"third_idle\"") &&
                        !subweb_holds(served, normal, "\"first_idle\"") && !strcmp(hit, normal));
                    snprintf(shared[who - 2], sizeof(shared[0]), "%s", normal);
                } else {
                    assert(subweb_holds(served, normal, "\"first_idle\"") && !subweb_holds(served, normal, "\"third_idle\"") &&
                        !subweb_holds(served, normal, "node \"third\"") && strcmp(normal, hit));
                    assert(subweb_holds(served, hit, "node \"own\""));
                }
                if (!strcmp(normal, "sp_pistol")) {
                    if (kept_by_third < 0) kept_by_third = who >= 2;
                    else assert(kept_by_third == (who >= 2));
                }
                weapons++;
            }
            assert(weapons == 3 && seen[0] == 1 && seen[2] == 1 && seen[3] == 1 && kept_by_third >= 0);
            assert(!strcmp(shared[0], shared[1]));
            /* The same packages compose the same web in every order. */
            if (!first_web) { first_web = served; first_web_length = length_of_web; served = NULL; }
            else assert(length_of_web == first_web_length && !memcmp(served, first_web, first_web_length));
            free(served); sh_package_library_free(&library); sh_package_provenance_free(&historical);
            sh_package_sources_free(sources);
        }
        free(first_web);
        {
            char from[MAX_PATH], to[MAX_PATH];
            snprintf(from, sizeof(from), "%s\\overrides\\zz-weapon", root); snprintf(to, sizeof(to), "%s\\overrides\\weapon", root);
            assert(MoveFileA(from, to));
            snprintf(from, sizeof(from), "%s\\overrides\\aa-weapon3", root); snprintf(to, sizeof(to), "%s\\overrides\\weapon3", root);
            assert(MoveFileA(from, to));
        }
        /* A failed alignment refuses the web with its packages named and
         * blames no package's own bytes; nothing it rewrote reaches the
         * conflict path (the copies are published only after every move is
         * recorded). */
        {
            static const char *const webs[] = {"hands"};
            sh_package_hands_adapter twice = {webs, 1, prepare_twice, sh_decl_animweb_moves_free,
                sh_decl_weapon_subweb_rename};
            sh_package_compile_environment direct = env;
            size_t invalid = 0;
            direct.compose_custom = block_compose; direct.player_hands = &twice; direct.independent_sources = 0;
            direct.invalid_package = &invalid;
            sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
            sh_package_compilation *compiled = sh_package_compile_with(sources, &direct, error, sizeof(error));
            fprintf(stderr, "player hands failed alignment: %s\n", compiled ? "compiled" : error);
            assert(!compiled && invalid == SIZE_MAX && strstr(error, "hands.decl") &&
                strstr(error, "two copies of subweb sp_pistol"));
            sh_package_sources_free(sources);
        }
        for (size_t i = 0; i < INPUTS; i++) {
            size_t length = 0; char *after = read_text(inputs[i], &length);
            assert(length == before_length[i] && !memcmp(after, before[i], length));
            free(after); free(before[i]);
        }
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
/* Captured originals are borrowed from the compile's snapshot (and by the reverse base from its parent capture),
 * spellings are interned once per capture, a prepared stand-in exists only for a prepared unit, and the link output
 * shares the snapshot's bytes of the captured originals it serves (R-341; before, it took those served at their own
 * paths, M130 e031-e035). Every output is read after the compile returned and closed its captures and snapshot. The
 * same compile with owned copies (the path of a link given no borrowing reader), without sharing, with discovery
 * lists in one-item arena blocks (e055), and a repeat agree with it field for field. A refused interned spelling,
 * snapshot body, shared projection resource, arena block or shared body, at any reached position, and a link refused
 * after its projection, fail the compile with a reason or leave the outputs equal; each frees every body it made,
 * and nothing they left behind changes the next clean compile. */
static int capture_compile(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *native,
    sh_package_library *out, char *error, size_t capacity)
{
    memset(out, 0, sizeof(*out));
    return sh_package_library_compile(sources, env, catalog, original, f, registry, (uintptr_t)f->reflection,
        native, out, error, capacity);
}
/* kind 0 refuses the nth interned spelling, 1 the nth snapshot body, 2 a projection's nth resource that shares a
 * body, 3 the nth discovery arena or frozen site block, 4 the nth shared body, 5 the nth optional unit record, 6 the
 * nth kept copy of a lent unit table's row. */
static void capture_refusals(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *native,
    const sh_package_library *expected, int kind, size_t *reached, size_t *failed)
{
    static const char *const labels[] = {"intern refusal", "snapshot refusal", "projection refusal", "block refusal",
        "body refusal", "extra refusal", "keep refusal"};
    char error[1024];
    long previous = 0, nth = 1, live = sh_package_body_test_live();
    for (;;) {
        sh_package_library out;
        long left = 0, ignored = 0;
        *error = 0;
        if (kind == 1) sh_package_library_test_original_refusal(nth);
        else if (kind == 2) sh_package_compilation_test_project_refusal(nth);
        else if (kind == 3) sh_package_source_link_test_blocks(0, nth);
        else if (kind == 4) sh_package_body_test_refusal(nth);
        else if (kind == 5) sh_package_source_link_test_extra_refusal(nth);
        else if (kind == 6) sh_package_source_link_test_preserve_refusal(nth);
        else sh_package_source_link_test_refuse(nth, 0);
        int ok = capture_compile(f, sources, env, catalog, registry, native, &out, error, sizeof(error));
        if (kind == 1) left = sh_package_library_test_original_refusal_left();
        else if (kind == 2) left = sh_package_compilation_test_project_refusal_left();
        else if (kind == 3) left = sh_package_source_link_test_blocks_left();
        else if (kind == 4) left = sh_package_body_test_refusal_left();
        else if (kind == 5) left = sh_package_source_link_test_extra_refusal_left();
        else if (kind == 6) left = sh_package_source_link_test_preserve_refusal_left();
        else sh_package_source_link_test_refusals_left(&left, &ignored);
        sh_package_library_test_original_refusal(0); sh_package_source_link_test_refuse(0, 0);
        sh_package_compilation_test_project_refusal(0); sh_package_source_link_test_blocks(0, 0);
        sh_package_body_test_refusal(0); sh_package_source_link_test_extra_refusal(0);
        sh_package_source_link_test_preserve_refusal(0);
        if (left) {
            /* Past the last allocation: an ordinary compile. */
            assert(ok && lod_same(expected, &out, "capture refusal past the end"));
            sh_package_library_free(&out); assert(sh_package_body_test_live() == live); break;
        }
        (*reached)++;
        if (ok) assert(lod_same(expected, &out, labels[kind]));
        else { assert(*error); (*failed)++; }
        sh_package_library_free(&out);
        assert(sh_package_body_test_live() == live);
        long next = nth + previous; previous = nth; nth = next;
    }
}
static void test_capture_ownership(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *native,
    const sh_package_library *borrowed)
{
    char error[1024];
    sh_package_library copied, again, unhanded;
    sh_package_source_link_test_borrow(0);
    int ok = capture_compile(f, sources, env, catalog, registry, native, &copied, error, sizeof(error));
    sh_package_source_link_test_borrow(1);
    if (!ok) fprintf(stderr, "capture ownership, copies: %s\n", error); assert(ok);
    assert(lod_same(borrowed, &copied, "copied originals"));
    sh_package_library_free(&copied);
    /* The borrowing compile's output serves captured originals with the snapshot's own bytes, which stay valid after
     * the compile released its snapshot and are freed with the output; without sharing it copies them, the same
     * result. */
    long live = sh_package_body_test_live();
    if (!sh_package_library_test_snapshot_twins(error, sizeof(error)) ||
        !sh_package_library_test_stage_pool(error, sizeof(error))) { fprintf(stderr, "%s\n", error); assert(0); }
    assert(sh_package_body_test_live() == live);
    ok = capture_compile(f, sources, env, catalog, registry, native, &again, error, sizeof(error));
    size_t handed = sh_package_library_test_shared(), views = sh_package_library_test_views_shared();
    if (!ok) fprintf(stderr, "capture ownership, shared: %s\n", error); assert(ok && handed && views);
    assert(sh_package_body_test_live() > live && lod_same(borrowed, &again, "shared bodies"));
    sh_package_library_free(&again);
    assert(sh_package_body_test_live() == live);
    sh_package_library_test_share(0);
    ok = capture_compile(f, sources, env, catalog, registry, native, &unhanded, error, sizeof(error));
    sh_package_library_test_share(1);
    if (!ok) fprintf(stderr, "capture ownership, no sharing: %s\n", error);
    assert(ok && !sh_package_library_test_shared() && !sh_package_library_test_views_shared() &&
        lod_same(borrowed, &unhanded, "copied link output"));
    sh_package_library_free(&unhanded);
    assert(sh_package_body_test_live() == live);
    /* R-344 stage A, shared stock (the compiles above share it by default): without sharing (the oracle), deciding
     * without applying (the strict materialization oracle: every would-be shared unit ends unedited with its
     * representative's class and name), with later contributions free to meet shared rows (the library's boundary
     * falls back to the scopes' own copies), and without streamed originals: the same outputs. */
    {
        static const char *const variants[] = {"shared stock off", "shared stock decided only",
            "shared stock without conservative non-sharing", "originals not streamed"};
        const char *line = strstr(borrowed->repeated_work, "source link shared stock");
        fprintf(stderr, "%.*s\n", line ? (int)strcspn(line, "\n") : 0, line ? line : "");
        for (int variant = 0; variant < 4; variant++) {
            if (variant == 0) sh_package_source_link_share(0);
            else if (variant == 1) sh_package_source_link_share(2);
            else if (variant == 2) sh_package_source_link_test_share_conservative(0);
            else sh_package_source_link_stream(0);
            ok = capture_compile(f, sources, env, catalog, registry, native, &unhanded, error, sizeof(error));
            sh_package_source_link_test_share_defaults();
            if (!ok) fprintf(stderr, "capture ownership, %s: %s\n", variants[variant], error);
            line = strstr(unhanded.repeated_work, "source link shared stock");
            fprintf(stderr, "%s: %.*s\n", variants[variant], line ? (int)strcspn(line, "\n") : 0, line ? line : "");
            line = strstr(unhanded.repeated_work, "shared stock fallback");
            if (line) fprintf(stderr, "%s: %.*s\n", variants[variant], (int)strcspn(line, "\n"), line);
            if (ok && !lod_same(borrowed, &unhanded, variants[variant]) && getenv("SMP_SHARE_DUMP")) {
                /* Diagnostics: both canonical dumps, for a line diff. */
                char *x = lod_dump(borrowed), *y = lod_dump(&unhanded), name[512];
                for (int side = 0; side < 2; side++) {
                    snprintf(name, sizeof(name), "%s.%d.%s.txt", getenv("SMP_SHARE_DUMP"), variant, side ? "variant" : "shared");
                    FILE *dump = fopen(name, "wb");
                    if (dump) { fputs(side ? (y ? y : "") : (x ? x : ""), dump); fclose(dump); }
                }
                free(x); free(y);
            }
            assert(ok && lod_same(borrowed, &unhanded, variants[variant]));
            sh_package_library_free(&unhanded);
            assert(sh_package_body_test_live() == live);
        }
        size_t checked = 0, violations = 0;
        sh_package_library_test_share_checks(&checked, &violations);
        fprintf(stderr, "shared stock strict oracle: %zu would-be shared units checked, %zu differed\n", checked, violations);
        assert(!violations);
    }
    /* The final link releases its retained identity order after its pass; keeping it is the same result. */
    sh_package_library_test_release_order(0);
    ok = capture_compile(f, sources, env, catalog, registry, native, &unhanded, error, sizeof(error));
    sh_package_library_test_release_order(1);
    if (!ok) fprintf(stderr, "capture ownership, order kept: %s\n", error);
    assert(ok && lod_same(borrowed, &unhanded, "retained identity order"));
    sh_package_library_free(&unhanded);
    /* Discovery lists in one-item arena blocks move as they grow, in the base capture too: the same outputs. */
    sh_package_source_link_test_blocks(1, 0);
    ok = capture_compile(f, sources, env, catalog, registry, native, &unhanded, error, sizeof(error));
    sh_package_source_link_test_blocks(0, 0);
    if (!ok) fprintf(stderr, "capture ownership, one-item blocks: %s\n", error);
    assert(ok && lod_same(borrowed, &unhanded, "one-item discovery blocks"));
    sh_package_library_free(&unhanded);
    size_t reached[7] = {0}, failed[7] = {0};
    for (int kind = 0; kind < 7; kind++)
        capture_refusals(f, sources, env, catalog, registry, native, borrowed, kind, reached + kind, failed + kind);
    /* Refused right after its projection, the link discards an output that shares bodies. */
    sh_package_link_test_after_projection_refusal(1);
    *error = 0;
    ok = capture_compile(f, sources, env, catalog, registry, native, &again, error, sizeof(error));
    sh_package_link_test_after_projection_refusal(0);
    assert(!ok && *error);
    sh_package_library_free(&again);
    assert(sh_package_body_test_live() == live);
    fprintf(stderr, "capture ownership: %zu resources share snapshot bodies, %zu views the inventory's file table; "
        "interned refusals %zu reached %zu failed; snapshot refusals %zu reached %zu failed; projection refusals %zu "
        "reached %zu failed; block refusals %zu reached %zu failed; body refusals %zu reached %zu failed; extra refusals %zu "
        "reached %zu failed; keep refusals %zu reached %zu failed\n", handed, views,
        reached[0], failed[0], reached[1], failed[1], reached[2], failed[2], reached[3], failed[3], reached[4], failed[4],
        reached[5], failed[5], reached[6], failed[6]);
    assert(reached[0] && failed[0] && reached[1] && failed[1] && reached[2] && failed[2] && reached[3] && failed[3] &&
        reached[4] && reached[5] && reached[6] && failed[6] == reached[6]);
    /* The fixture's optional records are diagnostic notes (refused, the outputs stay equal); a refused kept copy always
     * fails the edit that needed it. */
    ok = capture_compile(f, sources, env, catalog, registry, native, &again, error, sizeof(error));
    if (!ok) fprintf(stderr, "capture ownership, again: %s\n", error); assert(ok);
    assert(lod_same(borrowed, &again, "repeated compile"));
    /* R-343: every library compile above edited the lent unit table's captured rows only after keeping the capture's
     * copy; refused copies (kind 6) failed their edits as environment failures. */
    size_t rows_checked, rows_changed, rows_kept;
    sh_package_source_link_test_rows(&rows_checked, &rows_changed, &rows_kept);
    fprintf(stderr, "lent unit tables: %zu rows checked, %zu changed without a kept copy, %zu copies kept\n", rows_checked,
        rows_changed, rows_kept);
    assert(rows_checked && !rows_changed && rows_kept && rows_kept * 4 < rows_checked); /* copies only for edited rows */
    sh_package_library_free(&again);
}
/* R-342: the shared body's backing. A backing must declare exactly the body's length, and a second one is refused;
 * either is released when refused. The bytes go only when every holder is detached and a backing exists, also when
 * the last attached holder releases; attach and read return them exactly; a failed or short read is recorded on the
 * body and leaves the holder detached; drop releases attached and detached holders alike; the last release frees
 * the backing once. */
typedef struct memory_backing { const char *text; size_t length; int released; long reads; } memory_backing;
static unsigned char *memory_backing_read(void *context, size_t *length)
{
    memory_backing *m = context;
    unsigned char *out = malloc(m->length + 1);
    if (!out) return NULL;
    memcpy(out, m->text, m->length); out[m->length] = 0; *length = m->length; m->reads++; return out;
}
static void memory_backing_release(void *context) { ((memory_backing *)context)->released++; }
static sh_package_backing memory_backing_of(memory_backing *m, size_t declared)
{ return (sh_package_backing){memory_backing_read, memory_backing_release, m, declared}; }
static void test_body_backing_protocol(void)
{
    long live = sh_package_body_test_live(), released = sh_package_body_test_released();
    memory_backing good = {"exact bytes", 11}, extra = {"exact bytes", 11}, shorter = {"exact", 5}, liar = {"exact", 5};
    size_t length = 0;
    unsigned char *read;
    sh_package_body *body = sh_package_body_adopt((unsigned char *)_strdup("exact bytes"), 11), *liar_body;
    assert(body);
    assert(!sh_package_body_back(body, memory_backing_of(&shorter, 5)) && shorter.released == 1 && !sh_package_body_backed(body));
    /* Without a backing, a body whose every holder is detached keeps its bytes. */
    sh_package_body_detach(body);
    assert(sh_package_body_bytes(body) && sh_package_body_test_released() == released);
    assert(sh_package_body_attach(body) == sh_package_body_bytes(body));
    assert(sh_package_body_back(body, memory_backing_of(&good, 11)) && sh_package_body_backed(body));
    assert(!sh_package_body_back(body, memory_backing_of(&extra, 11)) && extra.released == 1);
    /* One attached holder keeps the bytes; the last one detaching lets them go. */
    sh_package_body *copy = sh_package_body_retain_detached(body);
    assert(copy == body && sh_package_body_bytes(body));
    sh_package_body_detach(body);
    assert(!sh_package_body_bytes(body) && sh_package_body_test_released() == released + 1);
    read = sh_package_body_read(body, &length);
    assert(read && length == 11 && !memcmp(read, "exact bytes", 12) && good.reads == 1 && !sh_package_body_bytes(body));
    free(read);
    const unsigned char *held = sh_package_body_attach(body);
    assert(held && !memcmp(held, "exact bytes", 12) && good.reads == 2 && sh_package_body_test_released() == released);
    /* A read that fails is recorded; the holder stays detached and a later read succeeds. */
    sh_package_body_detach(body);
    assert(!sh_package_body_bytes(body) && !sh_package_body_failed(body));
    sh_package_body_test_backing_failure(1);
    assert(!sh_package_body_attach(body) && sh_package_body_failed(body) && !sh_package_body_bytes(body));
    assert(!sh_package_body_test_backing_failure_left());
    read = sh_package_body_read(body, &length); assert(read && length == 11); free(read);
    /* The last attached holder releasing lets the bytes go too. */
    assert(sh_package_body_attach(body) && sh_package_body_bytes(body));
    sh_package_body_drop(copy, NULL);
    assert(sh_package_body_bytes(body));
    copy = sh_package_body_retain_detached(body);
    sh_package_body_drop(body, sh_package_body_bytes(body));
    assert(!sh_package_body_bytes(copy) && sh_package_body_test_released() == released + 1 && !good.released);
    sh_package_body_drop(copy, NULL);
    assert(good.released == 1 && sh_package_body_test_live() == live && sh_package_body_test_released() == released);
    /* A backing that reads fewer bytes than it declared fails exactly like an unreadable one. */
    liar_body = sh_package_body_adopt((unsigned char *)_strdup("exact bytes"), 11);
    assert(liar_body && sh_package_body_back(liar_body, memory_backing_of(&liar, 11)));
    sh_package_body_detach(liar_body);
    assert(!sh_package_body_read(liar_body, &length) && !length && sh_package_body_failed(liar_body) && liar.reads == 1);
    assert(!sh_package_body_attach(liar_body) && liar.reads == 2);
    sh_package_body_drop(liar_body, NULL);
    assert(liar.released == 1 && sh_package_body_test_live() == live && sh_package_body_test_released() == released);
    fprintf(stderr, "body backings: release, read again, failure and holder accounting checked\n");
}
/* R-347: two bodies backed by one stored row hold equal bytes without either being read. Comparing two publications
 * whose images let their bytes go reads nothing when their backings name the same row; it reads both exactly when
 * they name different rows (equal or changed bytes), and fails with the image named when such a read fails. A
 * backing that cannot name its row never compares the same. */
typedef struct row_backing { memory_backing bytes; int row; } row_backing;
static unsigned char *row_backing_read(void *context, size_t *length)
{ return memory_backing_read(&((row_backing *)context)->bytes, length); }
static void row_backing_release(void *context) { memory_backing_release(&((row_backing *)context)->bytes); }
static int row_backing_same(const void *context, const void *other)
{ return ((const row_backing *)context)->row == ((const row_backing *)other)->row; }
static sh_package_body *row_body(row_backing *row)
{
    sh_package_body *body = sh_package_body_adopt((unsigned char *)_strdup(row->bytes.text), row->bytes.length);
    assert(body && sh_package_body_back(body, (sh_package_backing){row_backing_read, row_backing_release, row,
        row->bytes.length, row_backing_same}));
    sh_package_body_detach(body); /* every holder reads through the body: the bytes go */
    assert(!sh_package_body_bytes(body));
    return body;
}
static void test_same_backing_changes(void)
{
    long live = sh_package_body_test_live();
    row_backing a = {{"atlas pixels", 12}, 1}, b = {{"atlas pixels", 12}, 1}, c = {{"atlas pixels", 12}, 2},
        d = {{"atlas pixelz", 12}, 3};
    memory_backing plain = {"atlas pixels", 12};
    sh_package_body *x = row_body(&a), *y = row_body(&b), *z = row_body(&c), *w = row_body(&d);
    sh_package_body *p = sh_package_body_adopt((unsigned char *)_strdup("atlas pixels"), 12);
    assert(p && sh_package_body_back(p, memory_backing_of(&plain, 12)));
    sh_package_body_detach(p);
    assert(sh_package_body_same_backing(x, y) && sh_package_body_same_backing(x, x) && !sh_package_body_same_backing(x, z));
    assert(!sh_package_body_same_backing(x, p) && !sh_package_body_same_backing(p, p));
    assert(!sh_package_body_same_backing(x, NULL) && !sh_package_body_same_backing(NULL, x));
    sh_compiled_resource before_row = {0}, after_row = {0};
    before_row.engine_path = after_row.engine_path = "generated/image/stock/atlas.bimage";
    before_row.body_owner = x; before_row.body_length = 12;
    after_row.body_owner = y; after_row.body_length = 12;
    sh_package_compilation before = {0}, after = {0};
    before.resources = &before_row; before.resource_count = 1;
    after.resources = &after_row; after.resource_count = 1;
    sh_package_changes changes = {0};
    char error[512] = "";
    assert(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
    assert(!a.bytes.reads && !b.bytes.reads);
    after_row.body_owner = z;
    assert(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count);
    assert(a.bytes.reads == 1 && c.bytes.reads == 1);
    after_row.body_owner = w;
    assert(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && changes.count == 1 &&
        changes.items[0].kind == SH_PACKAGE_RESOURCE_REPLACED);
    sh_package_changes_free(&changes);
    after_row.body_owner = p;
    assert(sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count &&
        plain.reads == 1);
    after_row.body_owner = z;
    sh_package_body_test_backing_failure(1);
    assert(!sh_package_compilation_changes(&before, &after, &changes, error, sizeof(error)) && !changes.count &&
        strstr(error, "generated/image/stock/atlas.bimage"));
    assert(!sh_package_body_test_backing_failure_left());
    sh_package_body_drop(x, NULL); sh_package_body_drop(y, NULL); sh_package_body_drop(z, NULL);
    sh_package_body_drop(w, NULL); sh_package_body_drop(p, NULL);
    assert(a.bytes.released == 1 && b.bytes.released == 1 && c.bytes.released == 1 && d.bytes.released == 1 &&
        plain.released == 1 && sh_package_body_test_live() == live);
    fprintf(stderr, "same-row bodies: compared without reads; other rows read exactly; failures typed\n");
}
/* R-342: a reader that names where its bytes can be read again (a memory copy here, standing in for an installed
 * archive row the catalog pins) lets the snapshot and the capture release every non-declaration original at the
 * capture's release points; later readers read it again through its body. The output, once attached as a
 * publication attaches it, equals the resident compile field for field; no declaration is ever read again; every
 * body and backing is freed with the output. A read that cannot reproduce the bytes, at any position, fails the
 * compile with the original named and blames no package; one past the compile's reads fails only the attach.
 * Without shared owners nothing is released (the capture borrows the snapshot's bytes with no reference). */
typedef struct backing_copy { char *path; unsigned char *bytes; size_t length; } backing_copy;
static long g_backings, g_backing_reads;
static unsigned char *backing_copy_read(void *context, size_t *length)
{
    backing_copy *copy = context;
    size_t n = strlen(copy->path);
    if (n >= 5 && !_stricmp(copy->path + n - 5, ".decl")) {
        fprintf(stderr, "declaration read again: %s\n", copy->path); _exit(3);
    }
    unsigned char *out = malloc(copy->length + 1);
    if (!out) return NULL;
    memcpy(out, copy->bytes, copy->length); out[copy->length] = 0;
    *length = copy->length; g_backing_reads++; return out;
}
static void backing_copy_release(void *context)
{
    backing_copy *copy = context;
    free(copy->path); free(copy->bytes); free(copy); g_backings--;
}
static int backed_original(void *context, const char *path, unsigned char **body, size_t *length,
    sh_package_backing *backing)
{
    int status = original(context, path, body, length);
    memset(backing, 0, sizeof(*backing));
    if (status <= 0 || !*body) return status;
    backing_copy *copy = calloc(1, sizeof(*copy));
    if (copy && (copy->path = _strdup(path)) != NULL && (copy->bytes = malloc(*length + 1)) != NULL) {
        memcpy(copy->bytes, *body, *length); copy->length = *length; g_backings++;
        *backing = (sh_package_backing){backing_copy_read, backing_copy_release, copy, *length};
    } else if (copy) { free(copy->path); free(copy); }
    return status;
}
static int backed_compile(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *base,
    sh_package_library *out, char *error, size_t capacity)
{
    sh_package_library_native native = *base;
    native.baseline_backed = backed_original; native.original_backed = backed_original;
    return capture_compile(f, sources, env, catalog, registry, &native, out, error, capacity);
}
static void test_capture_backings(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *native,
    const sh_package_library *expected)
{
    char error[1024];
    sh_package_library out;
    size_t invalid = SIZE_MAX, unserved = SIZE_MAX;
    sh_package_rejections more = {0};
    sh_package_compile_environment blamed = *env;
    blamed.invalid_package = &invalid; blamed.unserved_package = &unserved; blamed.more_rejections = &more;
    long live = sh_package_body_test_live(), releases = sh_package_body_test_releases();
    g_backing_reads = 0;
    int ok = backed_compile(f, sources, &blamed, catalog, registry, native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "capture backings: %s\n", error);
    long released = sh_package_body_test_releases() - releases, compile_reads = g_backing_reads;
    size_t detached = 0;
    for (size_t i = 0; ok && i < out.compiled->resource_count; i++)
        detached += !out.compiled->resources[i].body && out.compiled->resources[i].body_owner;
    assert(ok && released > 0 && g_backings > 0);
    ok = sh_package_compilation_attach(out.compiled, error, sizeof(error));
    long attach_reads = g_backing_reads - compile_reads;
    assert(ok && lod_same(expected, &out, "backed originals") && !sh_package_body_test_released());
    sh_package_library_free(&out);
    assert(sh_package_body_test_live() == live && !g_backings);
    size_t reached = 0, failed = 0;
    for (long nth = 1;; nth++) {
        long left;
        *error = 0; invalid = unserved = SIZE_MAX;
        sh_package_body_test_backing_failure(nth);
        ok = backed_compile(f, sources, &blamed, catalog, registry, native, &out, error, sizeof(error));
        int attached = ok && sh_package_compilation_attach(out.compiled, error, sizeof(error));
        left = sh_package_body_test_backing_failure_left();
        sh_package_body_test_backing_failure(0);
        if (left) {
            /* Past every read: the compile and its attach as before. */
            assert(attached && lod_same(expected, &out, "backing failure past the end"));
            sh_package_library_free(&out); break;
        }
        reached++;
        if (nth <= compile_reads) {
            if (ok || !strstr(error, "could not be read again") || invalid != SIZE_MAX || unserved != SIZE_MAX || more.count)
                fprintf(stderr, "backing failure %ld: ok %d invalid %zu unserved %zu more %zu: %s\n", nth, ok, invalid,
                    unserved, more.count, error);
            assert(!ok && strstr(error, "could not be read again") && invalid == SIZE_MAX && unserved == SIZE_MAX &&
                !more.count);
        } else assert(ok && !attached && strstr(error, "could not be read again"));
        failed++;
        sh_package_library_free(&out);
        assert(sh_package_body_test_live() == live && !g_backings);
    }
    assert(reached == (size_t)(compile_reads + attach_reads) && failed == reached);
    /* Without shared owners the capture borrows the snapshot's bytes: nothing is released. */
    releases = sh_package_body_test_releases();
    sh_package_library_test_share(0);
    ok = backed_compile(f, sources, env, catalog, registry, native, &out, error, sizeof(error));
    sh_package_library_test_share(1);
    assert(ok && sh_package_body_test_releases() == releases && sh_package_compilation_attach(out.compiled, error, sizeof(error)) &&
        lod_same(expected, &out, "backed originals without sharing"));
    sh_package_library_free(&out);
    assert(sh_package_body_test_live() == live && !g_backings);
    /* Readers that copy an original only to learn its status, as before, bind exactly the same. */
    size_t queries = sh_package_source_link_test_status_queries();
    sh_package_source_link_test_status(0);
    ok = capture_compile(f, sources, env, catalog, registry, native, &out, error, sizeof(error));
    sh_package_source_link_test_status(1);
    assert(ok && queries && sh_package_source_link_test_status_queries() == queries &&
        lod_same(expected, &out, "status read by copying"));
    sh_package_library_free(&out);
    /* A stock material naming an image nothing supplies: the status-only answer (absent) defaults that reference
     * exactly as reading the original does, also with the backing released. */
    sh_package_library absent_status, absent_copied;
    f->absent_material = 1;
    size_t before_absent = sh_package_source_link_test_status_queries();
    int status_ok = backed_compile(f, sources, env, catalog, registry, native, &absent_status, error, sizeof(error));
    if (!status_ok) fprintf(stderr, "absent material, status: %s\n", error);
    size_t absent_queries = sh_package_source_link_test_status_queries() - before_absent;
    sh_package_source_link_test_status(0);
    int copied_ok = backed_compile(f, sources, env, catalog, registry, native, &absent_copied, error, sizeof(error));
    sh_package_source_link_test_status(1);
    if (!copied_ok) fprintf(stderr, "absent material, copied: %s\n", error);
    f->absent_material = 0;
    assert(status_ok && copied_ok && absent_queries &&
        sh_package_compilation_attach(absent_status.compiled, error, sizeof(error)) &&
        sh_package_compilation_attach(absent_copied.compiled, error, sizeof(error)) &&
        lod_same(&absent_copied, &absent_status, "absent material by status"));
    sh_package_library_free(&absent_status); sh_package_library_free(&absent_copied);
    assert(sh_package_body_test_live() == live && !g_backings);
    sh_package_rejections_free(&more);
    fprintf(stderr, "capture backings: %ld bodies released, %zu detached in the output, %ld reads in the compile and %ld "
        "in the attach; backing failures %zu reached %zu failed; %zu status-only reads\n", released, detached,
        compile_reads, attach_reads, reached, failed, queries);
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
    /* e055: every frozen file's site range against its discovery sites, and the arena under interleaved growth. */
    size_t ranges = 0, differing = 0;
    char arena[256] = "";
    sh_package_source_link_test_site_ranges(&ranges, &differing);
    if (!ranges || differing || !sh_package_source_link_test_arena(arena, sizeof(arena))) {
        fprintf(stderr, "frozen site ranges: %zu compared, %zu differ; arena: %s\n", ranges, differing, arena);
        _exit(3);
    }
    printf("frozen site ranges: %zu compared with their discovery sites, 0 differ\n", ranges);
    /* R-341: every freeze released each unit's discovery state and its three arenas. */
    size_t released = 0, kept = 0;
    sh_package_source_link_test_discovery(&released, &kept);
    if (!released || kept) {
        fprintf(stderr, "discovery release: %zu units released, %zu freezes kept state\n", released, kept);
        _exit(3);
    }
    printf("discovery release: %zu units released at their freeze, 0 kept\n", released);
}
/* entityDef Parse and its merge look a parent up with makeDefault 0, which
 * returns NULL for an empty name: the stock info/cloud_shot's inherit = ""
 * names no parent even where a package supplies entitydef/_emptyName, so that
 * package gains no variant of it. A declaration-pointer field's "" is the
 * reader's makeDefault lookup of _emptyName and binds the package's own,
 * while another scope keeps its stock spelling. */
static void test_empty_parent(fixture *f, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *native)
{
    char error[1024], from[MAX_PATH], to[MAX_PATH];
    /* The weapon webs need test_player_hands' player fixture: set them aside. */
    static const char *const held[] = {"weapon", "weapon2", "weapon3", "weapon4"};
    int moved[4] = {0};
    for (int i = 0; i < 4; i++) {
        snprintf(from, sizeof(from), "%s\\overrides\\%s", root, held[i]); snprintf(to, sizeof(to), "%s\\%s-held", root, held[i]);
        moved[i] = GetFileAttributesA(from) != INVALID_FILE_ATTRIBUTES && MoveFileA(from, to);
    }
    package("emptyparent", "Empty Parent", "generated/decls/entitydef/_emptyName.decl", "{ class = \"Entity\"; edit = { } }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    for (int shape = 1; shape <= 2; shape++) {
        sh_package_library library = {0};
        f->empty_parent = shape;
        int ok = sh_package_library_compile(sources, env, catalog, original, f, registry,
            (uintptr_t)f->reflection, native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "empty parent %d: %s\n", shape, error);
        assert(ok);
        const sh_package_provenance_binding *rock = binding(&library, sources, "Empty Parent", "", "entitydef", "props/rock");
        const sh_package_provenance_binding *red = binding(&library, sources, "Red", "", "entitydef", "props/rock");
        size_t rebound = published(&library, "entitydef", "inherit = \"smp/");
        if (shape == 1 ? rock || rebound || !red || !holds(&library, resource(&library, "entitydef", red->name), "inherit = \"\"") :
            !rock || !red || !holds(&library, resource(&library, "entitydef", rock->name), "target = \"smp/") ||
            !holds(&library, resource(&library, "entitydef", red->name), "target = \"\"")) {
            fprintf(stderr, "empty parent %d: own variant %s, rebound parents %zu\n", shape, rock ? rock->name : "(none)", rebound);
            for (size_t i = 0; i < library.compiled->resource_count; i++)
                fprintf(stderr, "  %s %s\n", library.compiled->resources[i].engine_path,
                    library.compiled->resources[i].native_name ? library.compiled->resources[i].native_name : "");
            assert(0);
        }
        sh_package_library_free(&library);
    }
    f->empty_parent = 0; sh_package_sources_free(sources);
    for (int i = 0; i < 4; i++) if (moved[i]) {
        snprintf(from, sizeof(from), "%s\\overrides\\%s", root, held[i]); snprintf(to, sizeof(to), "%s\\%s-held", root, held[i]);
        assert(MoveFileA(to, from));
    }
}

/* B2: a downstream-only variant (the shipped rock tile republished in a scope
 * whose image, material or model changed) is told apart from the shipped tile
 * by a display-only label alias naming that scope's package; exact copies and
 * the variants that deduplicate with them share one alias naming every
 * package, the shipped row keeps its own label, and identities, bindings and
 * bodies are those of the same compile without string reservations. Icon's
 * variant changes only the preview image its parent row names, and so places
 * the shipped entityDef, whose lifetime edge to the rows describing it (the
 * shipped one included) is not a dependency of the shipped row: Icon is named
 * too, for its image the variant reaches through its own private parent. */
static size_t reverse_reservations;
static int reverse_reserve(void *context, const char *key, const unsigned char identity[32], const char *mapping)
{
    (void)context; (void)identity; (void)mapping;
    if (strncmp(key, "#str_smp_loc_", 13)) return 0;
    reverse_reservations++; return 1;
}
static int reverse_reserve_global(void *context, const char *key) { (void)context; (void)key; return 1; }
/* The label mapping a row's presented text selects, or NULL. */
static const char *reverse_alias(const sh_package_library *library, const sh_compiled_resource *row)
{
    const char *at = row && row->presented ? strstr((const char *)row->presented, "displayNameTag = \"#str_smp_loc_") : NULL;
    char key[160] = "";
    if (!at || sscanf(at + 18, "%159[^\"]", key) != 1) return NULL;
    for (size_t i = 0; i < library->compiled->policy.localized_count; i++)
        if (!strcmp(library->compiled->policy.localized[i].key, key)) return library->compiled->policy.localized[i].mapping;
    return NULL;
}
static void reverse_label_aliases(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *base)
{
    struct { const char *package, *also[3]; } variants[] = {
        {"Red", {"Red Copy"}}, {"Green", {0}}, {"Material", {"Redirect Red", "Redirect Green"}}, {"Model", {0}},
        {"Nested", {0}}, {"Icon", {0}}};
    sh_package_library_native native = *base;
    sh_package_library labelled = {0}, unlabelled = {0};
    char error[1024];
    native.strings.reserve = reverse_reserve; native.strings.max_attempts = 4;
    native.strings.reserve_global = reverse_reserve_global;
    f->labelled = 1;
    int ok = sh_package_library_compile(sources, env, catalog, original, f, registry, (uintptr_t)f->reflection,
        base, &unlabelled, error, sizeof(error));
    if (!ok) fprintf(stderr, "unreserved labelled reverse roots: %s\n", error); assert(ok);
    ok = sh_package_library_compile(sources, env, catalog, original, f, registry, (uintptr_t)f->reflection,
        &native, &labelled, error, sizeof(error));
    f->labelled = 0;
    if (!ok) fprintf(stderr, "labelled reverse roots: %s\n", error); assert(ok);
    fprintf(stderr, "reverse labels: %zu reservations\n", reverse_reservations);
    for (size_t v = 0; v < sizeof(variants) / sizeof(*variants); v++) {
        const sh_package_provenance_binding *named = binding(&labelled, sources, variants[v].package, "", "snapeditorentitydef", "rock");
        const sh_package_provenance_binding *bare = binding(&unlabelled, sources, variants[v].package, "", "snapeditorentitydef", "rock");
        assert(named && bare && !strcmp(named->name, bare->name));
        const sh_compiled_resource *row = resource(&labelled, "snapeditorentitydef", named->name);
        const sh_compiled_resource *same = resource(&unlabelled, "snapeditorentitydef", named->name);
        const char *mapping = reverse_alias(&labelled, row);
        fprintf(stderr, "reverse label %s: row %s, %s\n", variants[v].package, named->name, mapping ? mapping : "(no alias)");
        /* Display only: the served text gains the alias; the body is the unreserved compile's. */
        assert(row && same && row->body && same->body && row->body_length == same->body_length &&
            !memcmp(row->body, same->body, row->body_length) && !strstr((const char *)row->body, "#str_smp_loc_"));
        assert(!same->presented && mapping);
        char name[96];
        snprintf(name, sizeof(name), "\"name\": \"%s\"", variants[v].package);
        assert(strstr(mapping, name) && strstr(mapping, "\"category\": \"#str_props\"") &&
            strstr(mapping, "\"meta\": \"#str_objects\"") && strstr(mapping, "\"row\": \"rock\""));
        for (size_t a = 0; a < 3 && variants[v].also[a]; a++) {
            snprintf(name, sizeof(name), "\"name\": \"%s\"", variants[v].also[a]);
            assert(strstr(mapping, name));
        }
        if (!strcmp(variants[v].package, "Nested")) assert(strstr(mapping, "\"component\": \"child\""));
    }
    /* Icon's variant binds the shipped entityDef; it has no entityDef of its own, only its parent. */
    const sh_package_provenance_binding *iconic = binding(&labelled, sources, "Icon", "", "snapeditorentitydef", "rock");
    const sh_compiled_resource *icon_row = iconic ? resource(&labelled, "snapeditorentitydef", iconic->name) : NULL;
    assert(icon_row && icon_row->body && strstr((const char *)icon_row->body, "inherit = \"smp/") &&
        strstr((const char *)icon_row->body, "entityDef = \"props/rock\""));
    assert(!binding(&labelled, sources, "Icon", "", "entitydef", "props/rock"));
    assert(binding(&labelled, sources, "Red", "", "entitydef", "props/rock"));
    /* The shipped row, and packages whose image bytes are the shipped ones, show the shipped label. */
    const sh_compiled_resource *shipped = resource(&labelled, "snapeditorentitydef", "rock");
    assert(!shipped || !shipped->presented);
    assert(!binding(&labelled, sources, "Original", "", "snapeditorentitydef", "rock"));
    sh_package_library_free(&labelled); sh_package_library_free(&unlabelled);
}
/* A scope whose own package declares g_useImageBlackList 0 serves a private stock copy of a stock
 * image whose registered native object is an exclusion placeholder, through the real
 * image_exclusion query and the real library compile (R-309). A scope without the requirement, a
 * usable registered image, an installed header that leaves the state ambiguous, and every unknown
 * answer (no binding, another thread, a refused native lock) never certify: unknown fails the
 * compile instead of serving the stock name. */
typedef struct ie_list { unsigned char bytes[0x40]; void *items[2]; } ie_list;
static ie_list g_ie_images;
static unsigned char g_ie_rock[0x100];
static void *g_ie_head, *g_ie_live;
static int g_ie_lock_object, g_ie_refuse;
static void *ie_lock(void) { return &g_ie_lock_object; }
static unsigned ie_acquire(void *lock, char blocking)
{ assert(lock == &g_ie_lock_object && blocking == 1); if (g_ie_refuse) return 0; g_ie_head = g_ie_live; return 1; }
static void ie_release(void *lock) { assert(lock == &g_ie_lock_object); g_ie_head = NULL; }
static int ie_query(void *context, const char *name, const unsigned char *installed, size_t length)
{ (void)context; return sh_image_exclusion_query(name, installed, length); }
/* As idImage leaves textures/rock: the exclusion branch, or a read of its binary. */
static void ie_rock(int excluded)
{
    memset(g_ie_rock, 0, sizeof(g_ie_rock));
    *(const char **)(g_ie_rock + 8) = "textures/rock";
    g_ie_rock[0x24] = 0x17; g_ie_rock[0x2c] = 0x10;
    uint32_t low = excluded ? 0xffffffffu : 0x12345678u, high = excluded ? 0xffffffffu : 0;
    memcpy(g_ie_rock + 0x38, &low, 4); memcpy(g_ie_rock + 0x3c, &high, 4);
    *(int *)(g_ie_rock + 0x58) = excluded ? 3 : 5;
    *(int *)(g_ie_rock + 0x60) = *(int *)(g_ie_rock + 0x64) = excluded ? 8 : 64;
    *(int *)(g_ie_rock + 0x90) = 5; *(uint32_t *)(g_ie_rock + 0xb0) = 0x41500000u;
}
static void ie_bind(int main_thread)
{
    sh_image_exclusion_test_native native = {(void *const *)&g_ie_head, ie_lock, ie_acquire, ie_release, main_thread};
    sh_image_exclusion_test_bind(&native);
}
static void test_image_exclusion_isolation(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[1024];
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-exclusion", saved); assert(CreateDirectoryA(root, NULL));
    create("overrides/gated/package.json",
        "{\"id\":\"gated\",\"name\":\"Gated\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}}}");
    create("overrides/gated/assets/generated/decls/material/generic/gated.decl", "{ tex \"textures/rock\" }");
    create("overrides/plain/package.json", "{\"id\":\"plain\",\"name\":\"Plain\"}");
    create("overrides/plain/assets/generated/decls/material/generic/gated.decl", "{ tex \"textures/rock\" }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry};
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    native.image_excluded = ie_query;
    memset(&g_ie_images, 0, sizeof(g_ie_images));
    *(const char **)(g_ie_images.bytes + 0x08) = "image";
    *(void ***)(g_ie_images.bytes + 0x20) = g_ie_images.items;
    *(int *)(g_ie_images.bytes + 0x28) = 1;
    g_ie_images.items[0] = g_ie_rock; g_ie_live = &g_ie_images;
    sh_package_library out = {0};

    /* The registered textures/rock is the exclusion branch's object. */
    ie_bind(1); ie_rock(1);
    int ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "image exclusion isolation: %s\n", error); assert(ok);
    const sh_package_provenance_binding *gated = binding(&out, sources, "Gated", "", "image", "textures/rock");
    assert(gated && !strncmp(gated->name, "generated/image/smp/", 20));
    const sh_compiled_resource *copy = resource(&out, "image", gated->name);
    assert(copy); contains(&out, copy, "stock-image");               /* the installed bytes, exact */
    const sh_package_provenance_binding *gated_material = binding(&out, sources, "Gated", "", "material", "generic/gated");
    const sh_package_provenance_binding *plain_material = binding(&out, sources, "Plain", "", "material", "generic/gated");
    assert(gated_material && plain_material);
    contains(&out, resource(&out, "material", gated_material->name), gated->name);
    /* The scope without the requirement isolates the image too, because its own material names it (R-389): the same
     * private copy, so the two equal materials are one. */
    const sh_package_provenance_binding *plain = binding(&out, sources, "Plain", "", "image", "textures/rock");
    assert(plain && !strcmp(plain->name, gated->name) && !strcmp(gated_material->name, plain_material->name));
    sh_package_library_free(&out);

    /* A usable registered image: no copy, both scopes share one material. */
    ie_rock(0);
    ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "usable image: %s\n", error); assert(ok);
    assert(!binding(&out, sources, "Gated", "", "image", "textures/rock"));
    gated_material = binding(&out, sources, "Gated", "", "material", "generic/gated");
    plain_material = binding(&out, sources, "Plain", "", "material", "generic/gated");
    assert(gated_material && plain_material && !strcmp(gated_material->name, plain_material->name));
    contains(&out, resource(&out, "material", gated_material->name), "textures/rock");
    sh_package_library_free(&out);

    /* Unknown answers fail the compile, leaving no publication to serve: an installed 8 x 8 header
     * whose first dword is all ones, no binding, another thread, a refused native lock. */
    for (int unknown = 0; unknown < 4; unknown++) {
        ie_rock(1); ie_bind(1);
        if (unknown == 0) f->ambiguous_rock = 1;
        if (unknown == 1) sh_image_exclusion_test_bind(NULL);
        if (unknown == 2) ie_bind(0);
        if (unknown == 3) g_ie_refuse = 1;
        ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
            (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
        if (ok || !strstr(error, "native image availability could not be read: textures/rock"))
            fprintf(stderr, "unknown image availability %d: %s\n", unknown, ok ? "served" : error);
        assert(!ok && strstr(error, "native image availability could not be read: textures/rock"));
        assert(!out.compiled && !out.bindings.binding_count);
        sh_package_library_free(&out);
        f->ambiguous_rock = 0; g_ie_refuse = 0;
    }
    /* The same ambiguous header with a registered image a binary read produced (its source's
     * timestamp at +0x3c) is usable, proven: no copy, and the compile succeeds. */
    f->ambiguous_rock = 1; ie_bind(1); ie_rock(1);
    { uint32_t stamp = 0; memcpy(g_ie_rock + 0x3c, &stamp, 4); }
    ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "read ambiguous image: %s\n", error); assert(ok);
    assert(!binding(&out, sources, "Gated", "", "image", "textures/rock"));
    sh_package_library_free(&out); f->ambiguous_rock = 0;

    sh_image_exclusion_test_bind(NULL);
    sh_package_native_assets_close(assets);
    sh_package_sources_free(sources);
    strcpy_s(root, sizeof(root), saved);
}

/* R-389: a scope that does not open the image gate isolates an exclusion placeholder its own state names (a supplied
 * material naming the image, as package zz-e2-movie-a's font material names fonts/korataki_rg/64_df.tga), and only
 * that: a scope reaching the image only through stock state keeps the stock image, material and model; the gate stays
 * closed and nothing is recovered. A usable image, and an unknown answer, keep the stock binding with no failure. The
 * same results hold without sharing, with sharing and when deciding without sharing. */
static void test_image_exclusion_authored(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[1024];
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-exclusion-authored", saved); assert(CreateDirectoryA(root, NULL));
    create("overrides/plain/package.json", "{\"id\":\"plain\",\"name\":\"Plain\"}");
    create("overrides/plain/assets/generated/decls/material/generic/plain.decl", "{ tex \"textures/rock\" }");
    create("overrides/stock/package.json", "{\"id\":\"stock\",\"name\":\"Stock\"}");
    create("overrides/stock/assets/generated/decls/entitydef/stock/thing.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    /* Both: a supplied entity reaching the stock material, and a supplied material naming the image. */
    create("overrides/both/package.json", "{\"id\":\"both\",\"name\":\"Both\"}");
    create("overrides/both/assets/generated/decls/entitydef/both/thing.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/both/assets/generated/decls/material/generic/both.decl", "{ tex \"textures/rock\" }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry};
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    native.image_excluded = ie_query;
    memset(&g_ie_images, 0, sizeof(g_ie_images));
    *(const char **)(g_ie_images.bytes + 0x08) = "image";
    *(void ***)(g_ie_images.bytes + 0x20) = g_ie_images.items;
    *(int *)(g_ie_images.bytes + 0x28) = 1;
    g_ie_images.items[0] = g_ie_rock; g_ie_live = &g_ie_images;
    sh_package_library out = {0};
    for (int mode = 0; mode <= 2; mode++) {
        sh_package_source_link_test_share_defaults(); sh_package_source_link_share(mode);
        ie_bind(1); ie_rock(1);
        int ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
            (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "authored exclusion (share %d): %s\n", mode, error); assert(ok);
        assert(!out.image_gate && !out.recovered_image_count);
        const sh_package_provenance_binding *image = binding(&out, sources, "Plain", "", "image", "textures/rock");
        assert(image && !strncmp(image->name, "generated/image/smp/", 20));
        const sh_compiled_resource *copy = resource(&out, "image", image->name);
        assert(copy); contains(&out, copy, "stock-image");               /* the installed bytes, exact */
        const sh_package_provenance_binding *material = binding(&out, sources, "Plain", "", "material", "generic/plain");
        assert(material); contains(&out, resource(&out, "material", material->name), image->name);
        /* Stock state alone: the stock image, material and model. */
        assert(!binding(&out, sources, "Stock", "", "image", "textures/rock"));
        assert(!binding(&out, sources, "Stock", "", "material", "generic/rock"));
        assert(!binding(&out, sources, "Stock", "", "model", "models/rock.lwo"));
        /* Its own material names the image, so the scope's stock material reaching it follows the copy too. */
        const sh_package_provenance_binding *both = binding(&out, sources, "Both", "", "image", "textures/rock");
        assert(both && !strcmp(both->name, image->name));
        const sh_package_provenance_binding *both_material = binding(&out, sources, "Both", "", "material", "generic/both");
        assert(both_material); contains(&out, resource(&out, "material", both_material->name), image->name);
        const sh_package_provenance_binding *both_rock = binding(&out, sources, "Both", "", "material", "generic/rock");
        assert(both_rock); contains(&out, resource(&out, "material", both_rock->name), image->name);
        sh_package_library_free(&out);

        /* A usable image: nothing is isolated. */
        ie_rock(0);
        ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
            (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "authored usable (share %d): %s\n", mode, error); assert(ok);
        assert(!binding(&out, sources, "Plain", "", "image", "textures/rock"));
        assert(!binding(&out, sources, "Both", "", "image", "textures/rock"));
        sh_package_library_free(&out);

        /* Unknown (no binding, another thread): the stock binding, as before the question; the compile succeeds. */
        for (int unknown = 0; unknown < 2; unknown++) {
            ie_rock(1);
            if (unknown == 0) sh_image_exclusion_test_bind(NULL); else ie_bind(0);
            ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
                (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
            if (!ok) fprintf(stderr, "authored unknown %d (share %d): %s\n", unknown, mode, error); assert(ok);
            assert(!binding(&out, sources, "Plain", "", "image", "textures/rock"));
            material = binding(&out, sources, "Plain", "", "material", "generic/plain");
            assert(material); contains(&out, resource(&out, "material", material->name), "textures/rock");
            sh_package_library_free(&out);
        }
    }
    sh_package_source_link_test_share_defaults();
    sh_image_exclusion_test_bind(NULL);
    sh_package_native_assets_close(assets);
    sh_package_sources_free(sources);
    strcpy_s(root, sizeof(root), saved);
}

/* A stock material a gate-opening scope reaches only through a stock model names the isolated
 * image. The scope serves a private copy of it whose bytes are the installed original's with that
 * image's name replaced, certified as a rebound installed original (package_compiler.h
 * rebound_original 1; R-321, r9's magic_glass_broken_2). The model, renamed through the material it
 * names, is rebound through a non-image name (2). Authored declarations are never certified, and the
 * scope without the requirement serves no copy. */
static void test_rebound_original_certificate(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[1024];
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-rebound", saved); assert(CreateDirectoryA(root, NULL));
    create("overrides/gated/package.json",
        "{\"id\":\"gated\",\"name\":\"Gated\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}}}");
    create("overrides/gated/assets/generated/decls/entitydef/gated/thing.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/gated/assets/generated/decls/material/generic/gated.decl", "{ tex \"textures/rock\" }");
    create("overrides/plain/package.json", "{\"id\":\"plain\",\"name\":\"Plain\"}");
    create("overrides/plain/assets/generated/decls/entitydef/plain/thing.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry};
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    native.image_excluded = ie_query;
    memset(&g_ie_images, 0, sizeof(g_ie_images));
    *(const char **)(g_ie_images.bytes + 0x08) = "image";
    *(void ***)(g_ie_images.bytes + 0x20) = g_ie_images.items;
    *(int *)(g_ie_images.bytes + 0x28) = 1;
    g_ie_images.items[0] = g_ie_rock; g_ie_live = &g_ie_images;
    sh_package_library out = {0};
    ie_bind(1); ie_rock(1);
    int ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "rebound original: %s\n", error); assert(ok);
    const sh_package_provenance_binding *image = binding(&out, sources, "Gated", "", "image", "textures/rock");
    assert(image && !strncmp(image->name, "generated/image/smp/", 20));
    /* The stock material is certified, with its original's path, and names the private image. */
    const sh_package_provenance_binding *material = binding(&out, sources, "Gated", "", "material", "generic/rock");
    assert(material && strcmp(material->name, "generic/rock"));
    const sh_compiled_resource *copy = resource(&out, "material", material->name);
    assert(copy && copy->rebound_original == 1 && !copy->native_original && !copy->source_count);
    assert(copy->original_path && !strcmp(copy->original_path, "generated/decls/material/generic/rock.decl"));
    contains(&out, copy, image->name);
    /* The model is renamed through the material it names: a non-image rebinding. */
    const sh_package_provenance_binding *model = binding(&out, sources, "Gated", "", "model", "models/rock.lwo");
    assert(model && strcmp(model->name, "models/rock.lwo"));
    const sh_compiled_resource *model_copy = resource(&out, "model", model->name);
    assert(model_copy && model_copy->rebound_original == 2);
    /* Authored declarations are never certified, whatever they name. */
    const sh_package_provenance_binding *authored = binding(&out, sources, "Gated", "", "material", "generic/gated");
    assert(authored && resource(&out, "material", authored->name) &&
        resource(&out, "material", authored->name)->rebound_original == 0);
    const sh_package_provenance_binding *thing = binding(&out, sources, "Gated", "", "entitydef", "gated/thing");
    assert(thing && resource(&out, "entitydef", thing->name)->rebound_original == 0);
    /* The scope without the requirement keeps the stock material and model: no copy, no certificate. */
    assert(!binding(&out, sources, "Plain", "", "material", "generic/rock"));
    assert(!binding(&out, sources, "Plain", "", "model", "models/rock.lwo"));
    for (size_t i = 0; i < out.compiled->resource_count; i++) {
        const sh_compiled_resource *r = out.compiled->resources + i;
        assert(!r->rebound_original || r == copy || r == model_copy);
    }
    sh_package_library_free(&out);
    /* A usable registered image: nothing is renamed, so nothing is certified. */
    ie_rock(0);
    ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "rebound original, usable image: %s\n", error); assert(ok);
    assert(!binding(&out, sources, "Gated", "", "material", "generic/rock"));
    for (size_t i = 0; i < out.compiled->resource_count; i++) assert(!out.compiled->resources[i].rebound_original);
    sh_package_library_free(&out);
    sh_image_exclusion_test_bind(NULL);
    sh_package_native_assets_close(assets);
    sh_package_sources_free(sources);
    strcpy_s(root, sizeof(root), saved);
}

/* A placeholder a fixed native ABI unit reaches (R-309 r7): the renderparm keeps its original
 * binding, so the image keeps its name in every scope and the library lists it for in-place
 * recovery; no scope serves a private copy of it, even one that also names it through a material.
 * A placeholder only relocatable consumers reach is still isolated in its gate-opening scope. */
static void test_image_recovery_fixed_binding(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[1024];
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-recovery", saved); assert(CreateDirectoryA(root, NULL));
    create("overrides/gated/package.json",
        "{\"id\":\"gated\",\"name\":\"Gated\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}}}");
    /* Writes the parm with another image; the renderparm is reached for its kind, and its own
     * value is the placeholder (env/uac_picus_a -> renderparm/userenvmap in r7). */
    create("overrides/gated/assets/generated/decls/material/generic/envuser.decl", "{ rockdefault \"textures/other\" }");
    create("overrides/plain/package.json", "{\"id\":\"plain\",\"name\":\"Plain\"}");
    create("overrides/plain/assets/generated/decls/material/generic/envuser.decl", "{ rockdefault \"textures/other\" }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry};
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    native.image_excluded = ie_query;
    memset(&g_ie_images, 0, sizeof(g_ie_images));
    *(const char **)(g_ie_images.bytes + 0x08) = "image";
    *(void ***)(g_ie_images.bytes + 0x20) = g_ie_images.items;
    *(int *)(g_ie_images.bytes + 0x28) = 1;
    g_ie_images.items[0] = g_ie_rock; g_ie_live = &g_ie_images;
    sh_package_library out = {0};
    ie_bind(1); ie_rock(1);
    int ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "fixed-binding recovery: %s\n", error); assert(ok);
    assert(out.image_gate == 1);
    assert(out.recovered_image_count == 1 && !strcmp(out.recovered_images[0], "textures/rock"));
    /* No scope relocates the image or the renderparm. */
    assert(!binding(&out, sources, "Gated", "", "image", "textures/rock"));
    assert(!binding(&out, sources, "Gated", "", "renderparm", "rockdefault"));
    assert(!binding(&out, sources, "Plain", "", "image", "textures/rock"));
    sh_package_library_free(&out);

    /* A gated material that names the placeholder directly no longer gets a private copy: the
     * native object it binds is the one the publication recovers. */
    create("overrides/gated/assets/generated/decls/material/generic/direct.decl", "{ tex \"textures/rock\" }");
    sh_package_sources_free(sources);
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "fixed-binding recovery with a direct consumer: %s\n", error); assert(ok);
    assert(out.recovered_image_count == 1 && !strcmp(out.recovered_images[0], "textures/rock"));
    assert(!binding(&out, sources, "Gated", "", "image", "textures/rock"));
    const sh_package_provenance_binding *direct = binding(&out, sources, "Gated", "", "material", "generic/direct");
    assert(direct); contains(&out, resource(&out, "material", direct->name), "textures/rock");
    sh_package_library_free(&out);

    /* Without a gate-opening scope nothing is recovered, and the material naming the image keeps it: the renderparm, a
     * fixed native ABI unit of the same scope, reaches it, so it keeps its stock binding (R-389). */
    create("overrides/gated/package.json", "{\"id\":\"gated\",\"name\":\"Gated\"}");
    sh_package_sources_free(sources);
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, catalog, original, f, registry,
        (uintptr_t)f->reflection, &native, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "no gate: %s\n", error); assert(ok);
    assert(!out.image_gate && !out.recovered_image_count && !out.recovered_images);
    assert(!binding(&out, sources, "Gated", "", "image", "textures/rock"));
    direct = binding(&out, sources, "Gated", "", "material", "generic/direct");
    assert(direct); contains(&out, resource(&out, "material", direct->name), "textures/rock");
    sh_package_library_free(&out);

    sh_image_exclusion_test_bind(NULL);
    sh_package_native_assets_close(assets);
    sh_package_sources_free(sources);
    strcpy_s(root, sizeof(root), saved);
}
/* One library compile with shared stock mode (0 off, 1 on) and a mutation control set for it alone. */
static int share_compile(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, const sh_package_library_native *native, int mode, int mutation,
    sh_package_library *out, char **log, char *error, size_t capacity)
{
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry};
    sh_package_source_link_test_share_defaults();
    sh_package_source_link_share(mode); sh_package_source_link_test_share_mutation(mutation);
    sh_package_source_link_test_shared_log(1);
    memset(out, 0, sizeof(*out));
    int ok = sh_package_library_compile(sources, env, catalog, original, f, registry, (uintptr_t)f->reflection, native,
        out, error, capacity);
    if (log) *log = _strdup(sh_package_source_link_test_shared_logged());
    sh_package_source_link_test_shared_log(0); sh_package_source_link_test_share_defaults();
    return ok;
}
/* R-344 stage A, first-copy placement: package Mine, the earlier scope, reaches the stock rock image unchanged
 * through its own entity and binds it as shared stock; package Same, the later scope, supplies a byte-identical copy.
 * One class holds both copies and its first member decides what the projection publishes: the stock copy, as without
 * sharing, because a shared unit sorts at the earliest scope that binds it (sl_unit_place). Without that placement
 * (mutation control 5) the supplied copy leads and the outputs differ. */
static void test_shared_stock_placement(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[1024];
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-placement", saved); assert(CreateDirectoryA(root, NULL));
    create("overrides/a_mine/package.json", "{\"id\":\"mine\",\"name\":\"Mine\"}");
    create("overrides/a_mine/assets/generated/decls/entitydef/props/mine.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/b_same/package.json", "{\"id\":\"same\",\"name\":\"Same\"}");
    create("overrides/b_same/assets/generated/image/textures/rock.bimage", "stock-image");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    sh_package_library off, on, unplaced;
    char *log = NULL;
    int ok = share_compile(f, sources, &env, catalog, &native, 0, 0, &off, NULL, error, sizeof(error));
    if (!ok) fprintf(stderr, "placement, without sharing: %s\n", error); assert(ok);
    ok = share_compile(f, sources, &env, catalog, &native, 1, 0, &on, &log, error, sizeof(error));
    if (!ok) fprintf(stderr, "placement, shared: %s\n", error);
    if (!log || !strstr(log, "Mine\timage\ttextures/rock\n")) fprintf(stderr, "placement decisions:\n%s", log ? log : "");
    assert(ok && log && strstr(log, "Mine\timage\ttextures/rock\n") && !strstr(log, "Same\timage\ttextures/rock\n"));
    assert(lod_same(&off, &on, "first-copy placement"));
    ok = share_compile(f, sources, &env, catalog, &native, 1, 5, &unplaced, NULL, error, sizeof(error));
    assert(ok && !lod_same(&off, &unplaced, "first-copy placement mutation control (expected to differ)"));
    sh_package_library_free(&off); sh_package_library_free(&on); sh_package_library_free(&unplaced); free(log);
    sh_package_native_assets_close(assets);
    sh_package_sources_free(sources);
    strcpy_s(root, sizeof(root), saved);
}
/* Remove the package named name (folder below overrides) from sources. */
static void remove_named(sh_package_sources *sources, const char *name)
{
    for (size_t i = 0; i < sources->package_count; i++)
        if (!strcmp(sources->packages[i].name, name)) { assert(sh_package_sources_remove(sources, i)); return; }
    assert(0);
}
static int name_order(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
/* R-344 stage A, failure convergence: package a_malformed supplies text the native grammar cannot read (its own
 * bytes), b_broken's own entity reaches a model SnapMap does not ship (coverage), c_red changes the stock rock image
 * and d_mine reaches it unchanged through its own entity (shared stock). Every rejection names its own package, and
 * leaving the rejected packages out converges, with and without shared stock, through the same left-out sets to the
 * same healthy publication (a report that lists one failure first and the others as further rejections may order
 * them by discovery; the sets compare). A product built-in reaching the same campaign gap stops the capture with no
 * package to blame: the compile with shared stock, which discovered scope by scope, is made again without it and
 * reports exactly the same failure, still blaming no one. */
static void test_shared_stock_convergence(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[2048], sequence[2][512] = {"", ""}, first[2][512] = {"", ""};
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-convergence", saved); assert(CreateDirectoryA(root, NULL));
    create("overrides/a_malformed/package.json", "{\"id\":\"malformed\",\"name\":\"Malformed\"}");
    create("overrides/a_malformed/assets/generated/decls/entitydef/props/malformed.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"\r\n target = \"props/rock\"; } }");
    create("overrides/b_broken/package.json", "{\"id\":\"broken\",\"name\":\"Broken\"}");
    create("overrides/b_broken/assets/generated/decls/entitydef/props/broken.decl",
        "{ class = \"Entity\"; edit = { model = \"models/campaign.lwo\"; } }");
    create("overrides/c_red/package.json", "{\"id\":\"red\",\"name\":\"Red\"}");
    create("overrides/c_red/assets/generated/image/textures/rock.bimage", "red-image");
    create("overrides/d_mine/package.json", "{\"id\":\"mine\",\"name\":\"Mine\"}");
    create("overrides/d_mine/assets/generated/decls/entitydef/props/mine.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    sh_package_library healthy[2];
    sh_package_sources *kept[2];
    for (int mode = 0; mode < 2; mode++) {
        size_t invalid, unserved;
        sh_package_rejections more = {0};
        sh_package_compile_environment blamed = env;
        blamed.invalid_package = &invalid; blamed.unserved_package = &unserved; blamed.more_rejections = &more;
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        for (int round = 0;; round++) {
            invalid = unserved = SIZE_MAX; *error = 0;
            int ok = share_compile(f, sources, &blamed, catalog, &native, mode, 0, healthy + mode, NULL, error,
                sizeof(error));
            if (ok) break;
            if (round >= 3 || (invalid == SIZE_MAX) == (unserved == SIZE_MAX) || healthy[mode].compiled)
                fprintf(stderr, "failure convergence (mode %d, round %d): invalid %zu unserved %zu more %zu: %s\n", mode,
                    round, invalid, unserved, more.count, error);
            assert(round < 3 && (invalid == SIZE_MAX) != (unserved == SIZE_MAX) && !healthy[mode].compiled);
            /* The reported package, then the further rejections: the round's left-out set, by name. */
            size_t owner = invalid != SIZE_MAX ? invalid : unserved, count = 0;
            char names[8][96];
            const char *sorted[8];
            snprintf(names[count++], sizeof(names[0]), "%s %s", invalid != SIZE_MAX ? "invalid" : "unserved",
                sources->packages[owner].name);
            for (size_t k = 0; k < more.count && count < 8; k++)
                snprintf(names[count++], sizeof(names[0]), "%s %s", more.items[k].invalid ? "invalid" : "unserved",
                    sources->packages[more.items[k].package].name);
            size_t used = strlen(first[mode]);
            snprintf(first[mode] + used, sizeof(first[mode]) - used, "%s; ", names[0]);
            for (size_t k = 0; k < count; k++) sorted[k] = names[k];
            qsort(sorted, count, sizeof(*sorted), name_order);
            used = strlen(sequence[mode]);
            for (size_t k = 0; k < count; k++, used = strlen(sequence[mode]))
                snprintf(sequence[mode] + used, sizeof(sequence[mode]) - used, "%s%s", sorted[k], k + 1 < count ? ", " : "; ");
            sh_package_library_free(healthy + mode);
            for (size_t k = 0; k < count; k++) remove_named(sources, strchr(names[k], ' ') + 1);
            sh_package_rejections_free(&more); memset(&more, 0, sizeof(more));
        }
        sh_package_rejections_free(&more);
        kept[mode] = sources;
    }
    fprintf(stderr, "shared stock failure convergence: left out without sharing %s(first %s), shared %s(first %s)\n",
        sequence[0], first[0], sequence[1], first[1]);
    assert(!strcmp(sequence[0], sequence[1]) && strstr(sequence[0], "invalid a_malformed") &&
        strstr(sequence[0], "unserved b_broken"));
    assert(lod_same(healthy, healthy + 1, "failure convergence"));
    sh_package_library_free(healthy); sh_package_library_free(healthy + 1);
    /* The product stop: the same failure in both modes, nobody blamed, the shared compile made again without sharing. */
    sh_package_builtin builtin = {"generated/decls/entitydef/props/builtin.decl",
        (const unsigned char *)"{ class = \"Entity\"; edit = { model = \"models/campaign.lwo\"; } }", 0};
    builtin.length = strlen((const char *)builtin.body);
    char stops[2][2048];
    for (int mode = 0; mode < 2; mode++) {
        size_t invalid = SIZE_MAX, unserved = SIZE_MAX;
        sh_package_rejections more = {0};
        sh_package_compile_environment blamed = env;
        blamed.invalid_package = &invalid; blamed.unserved_package = &unserved; blamed.more_rejections = &more;
        blamed.builtins = &builtin; blamed.builtin_count = 1;
        sh_package_library stopped;
        *stops[mode] = 0;
        assert(!share_compile(f, kept[mode], &blamed, catalog, &native, mode, 0, &stopped, NULL, stops[mode],
            sizeof(stops[mode])));
        if (invalid != SIZE_MAX || unserved != SIZE_MAX || more.count || stopped.compiled ||
            !strstr(stops[mode], "reached only from product and stock content") ||
            (mode && !strstr(stopped.repeated_work, "shared stock fallback rounds: 1 (the last without sharing)")))
            fprintf(stderr, "product stop (mode %d): invalid %zu unserved %zu more %zu: %s\nrepeated work: %s\n", mode,
                invalid, unserved, more.count, stops[mode], stopped.repeated_work);
        assert(invalid == SIZE_MAX && unserved == SIZE_MAX && !more.count && !stopped.compiled &&
            strstr(stops[mode], "reached only from product and stock content"));
        assert(!mode || strstr(stopped.repeated_work, "shared stock fallback rounds: 1 (the last without sharing)"));
        sh_package_library_free(&stopped); sh_package_rejections_free(&more);
    }
    assert(!strcmp(stops[0], stops[1]));
    sh_package_sources_free(kept[0]); sh_package_sources_free(kept[1]);
    sh_package_native_assets_close(assets);
    strcpy_s(root, sizeof(root), saved);
}
/* R-344 stage A: a refused shared-stock allocation (pending lists, fold decisions, stock facts, privatization copies)
 * fails the compile as an environment failure: no package is blamed, nothing is published and every body is freed;
 * past the last such allocation the compile is the ordinary one. */
static void test_share_refusals(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, sh_decl_registry_source registry, const sh_package_library_native *native,
    const sh_package_library *expected)
{
    char error[1024];
    size_t invalid = SIZE_MAX, unserved = SIZE_MAX, reached = 0, failed = 0;
    sh_package_rejections more = {0};
    sh_package_compile_environment blamed = *env;
    blamed.invalid_package = &invalid; blamed.unserved_package = &unserved; blamed.more_rejections = &more;
    long live = sh_package_body_test_live(), previous = 0, nth = 1;
    for (;;) {
        sh_package_library out;
        *error = 0; invalid = unserved = SIZE_MAX;
        sh_package_source_link_test_share_defaults(); sh_package_source_link_share(1);
        sh_package_source_link_test_share_mutation(0); sh_package_source_link_test_share_refusal(nth);
        int ok = capture_compile(f, sources, &blamed, catalog, registry, native, &out, error, sizeof(error));
        long left = sh_package_source_link_test_share_refusal_left();
        sh_package_source_link_test_share_refusal(0); sh_package_source_link_test_share_defaults();
        if (left) {
            assert(ok && lod_same(expected, &out, "share refusal past the end"));
            sh_package_library_free(&out); break;
        }
        reached++;
        if (ok) assert(lod_same(expected, &out, "share refusal"));
        else {
            if (!*error || invalid != SIZE_MAX || unserved != SIZE_MAX || more.count || out.compiled)
                fprintf(stderr, "share refusal %ld: invalid %zu unserved %zu more %zu: %s\n", nth, invalid, unserved,
                    more.count, error);
            assert(*error && invalid == SIZE_MAX && unserved == SIZE_MAX && !more.count && !out.compiled);
            failed++;
        }
        sh_package_library_free(&out);
        assert(sh_package_body_test_live() == live);
        /* Every early allocation, then a widening sample of the later ones. */
        long next = nth < 64 ? nth + 1 : nth + previous; previous = nth; nth = next;
    }
    sh_package_rejections_free(&more);
    fprintf(stderr, "shared stock refusals: %zu reached, %zu failed\n", reached, failed);
    assert(reached && failed);
}

/* ---- Unchanged-scope reuse (R-348) ----
 * A capture records what each package scope bound as shared stock; a later capture of a changed library restores the
 * scopes whose content is unchanged from that record. Each case compiles the changed library fresh and with the record
 * offered, and requires the same frozen capture (every unit, file, edge, certificate and flag, and every scope's share
 * map) and the same library outputs; the counters show the work the restore avoided. Each case also runs the restore
 * with each validation dropped (mutation controls): every dropped validation must make some case differ. */
typedef struct reuse_counts {
    size_t keyed, offered, matched, restored, bindings, valid, bound, copies, certified, refused[8], recorded;
    /* Settled entity-phase editor groups (package_library.c, pl_group_settled_key), and the editor phase's. */
    size_t settled_keyed, settled_hits, settled_recorded, settled_unkeyed;
    size_t editor_keyed, editor_hits, editor_recorded, editor_unkeyed;
    size_t restaged; /* members staged again from the record, all phases */
    /* Identity templates the record offered, and those the identity passes took and refused (package_linker.c). */
    size_t templates_offered, templates_taken, templates_refused;
} reuse_counts;
static int reuse_counts_read(const sh_package_library *library, reuse_counts *c)
{
    memset(c, 0, sizeof(*c));
    const char *line = strstr(library->repeated_work, "source link scope reuse: ");
    if (!line) return 0;
    unsigned long long ignored = 0;
    int n = sscanf(line, "source link scope reuse: %zu scopes keyed in %llu ms, %zu offered, %zu matched, %zu restored; "
        "%zu bindings offered, %zu valid, %zu bound (%zu admission copies replaced, %zu stock units certified here), "
        "refused: own %zu, absent %zu, structure %zu, family %zu, changed %zu, inputs %zu, closure %zu, certificate %zu",
        &c->keyed, &ignored, &c->offered, &c->matched, &c->restored, &c->bindings, &c->valid, &c->bound, &c->copies,
        &c->certified, c->refused, c->refused + 1, c->refused + 2, c->refused + 3, c->refused + 4, c->refused + 5,
        c->refused + 6, c->refused + 7);
    const char *recorded = strstr(line, "; recorded ");
    if (recorded) sscanf(recorded, "; recorded %zu scopes", &c->recorded);
    const char *settled = strstr(library->repeated_work, "editor groups settled:");
    const char *settled_line = settled;
    settled = settled ? strstr(settled, " 2: ") : NULL;
    size_t members = 0, again = 0;
    if (!settled || sscanf(settled, " 2: %zu keyed, %zu settled from the record (%zu members, %zu staged again), %zu "
        "recorded, %zu not keyed;", &c->settled_keyed, &c->settled_hits, &members, &again, &c->settled_recorded,
        &c->settled_unkeyed) != 6) return 0;
    c->restaged = again;
    settled = strstr(settled, " 3: ");
    if (!settled || sscanf(settled, " 3: %zu keyed, %zu settled from the record (%zu members, %zu staged again), %zu "
        "recorded, %zu not keyed;", &c->editor_keyed, &c->editor_hits, &members, &again, &c->editor_recorded,
        &c->editor_unkeyed) != 6) return 0;
    c->restaged += again;
    const char *inspectors = strstr(settled_line, " 1: ");
    if (inspectors && sscanf(inspectors, " 1: %*zu keyed, %*zu settled from the record (%*zu members, %zu staged again)",
        &again) == 1) c->restaged += again;
    const char *templates = strstr(library->repeated_work, "identity templates from the record: offered, ");
    if (templates && sscanf(templates, "identity templates from the record: offered, %zu offered (%*llu KB, indexed in "
        "%*llu ms), %zu taken, %zu refused", &c->templates_offered, &c->templates_taken, &c->templates_refused) != 3) return 0;
    return n == 18;
}
static int reuse_compile(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, const sh_package_library_native *base, const sh_package_source_reuse *in,
    sh_package_source_reuse *record, char **dump, sh_package_library *out, char *error, size_t capacity)
{
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry};
    sh_package_library_native native = *base;
    native.source.reuse_in = in; native.source.reuse_out = record;
    sh_package_source_link_test_capture_dump(1);
    memset(out, 0, sizeof(*out));
    int ok = sh_package_library_compile(sources, env, catalog, original, f, registry, (uintptr_t)f->reflection, &native,
        out, error, capacity);
    if (dump) *dump = _strdup(sh_package_source_link_test_capture_dumped());
    sh_package_source_link_test_capture_dump(0);
    return ok;
}
/* The last capture of a dump (a compile that fell back dumps its fallback's capture last). */
static const char *reuse_last_capture(const char *dump)
{
    const char *last = dump;
    for (const char *at = dump; at && (at = strstr(at, "capture ")) != NULL; at++) if (at == dump || at[-1] == '\n') last = at;
    return last;
}
static int reuse_text_same(const char *a, const char *b, const char *label, int quiet)
{
    if (a && b && !strcmp(a, b)) return 1;
    if (!quiet && a && b) {
        size_t at = 0, line = 1;
        while (a[at] && a[at] == b[at]) { if (a[at] == '\n') line++; at++; }
        size_t from = at; while (from && a[from - 1] != '\n') from--;
        fprintf(stderr, "%s: captures differ at line %zu:\n  fresh    %.400s\n  restored %.400s\n", label, line, a + from, b + from);
    }
    return 0;
}
static int reuse_outputs_same(const sh_package_library *a, const sh_package_library *b)
{
    char *x = lod_dump(a), *y = lod_dump(b);
    int same = x && y && !strcmp(x, y);
    free(x); free(y); return same;
}
static const unsigned reuse_mutations[] = {1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 6, (1u << 1) | (1u << 2), 1u << 7};
#define REUSE_MUTATIONS (sizeof(reuse_mutations) / sizeof(*reuse_mutations))
static unsigned g_reuse_caught[REUSE_MUTATIONS];
/* Settled editor groups' mutation controls: the record trusted (1), the key without the members' content (2), a
 * recorded group settled without staging what it staged (3). */
static unsigned g_settled_caught[4];
/* One library state: fresh, then with record offered (and next recording), compared exactly; then each mutation. */
static void reuse_case(fixture *f, const sh_package_sources *sources, const sh_package_compile_environment *env,
    const sh_resource_catalog *catalog, const sh_package_library_native *native, const sh_package_source_reuse *record,
    sh_package_source_reuse *next, const char *label, reuse_counts *counts)
{
    char error[2048];
    char *fresh_dump = NULL, *restored_dump = NULL;
    sh_package_library fresh, restored;
    int ok = reuse_compile(f, sources, env, catalog, native, NULL, NULL, &fresh_dump, &fresh, error, sizeof(error));
    if (!ok) fprintf(stderr, "%s, fresh: %s\n", label, error);
    assert(ok);
    ok = reuse_compile(f, sources, env, catalog, native, record, next, &restored_dump, &restored, error, sizeof(error));
    if (!ok) fprintf(stderr, "%s, restored: %s\n", label, error);
    assert(ok);
    {
        char *debug = NULL; size_t length = 0;
        if (!_dupenv_s(&debug, &length, "SMP_REUSE_DEBUG") && debug) {
            char *text = sh_package_source_reuse_test_text(record);
            fprintf(stderr, "scope reuse, %s: offered record\n%s", label, text ? text : "(none)\n");
            free(text);
            fprintf(stderr, "scope reuse, %s: fresh capture\n%s", label, fresh_dump);
        }
        free(debug);
    }
    int same_capture = reuse_text_same(fresh_dump, restored_dump, label, 0);
    int same_outputs = lod_same(&fresh, &restored, label);
    assert(reuse_counts_read(&restored, counts));
    fprintf(stderr, "scope reuse, %s: %zu scopes keyed, %zu matched; %zu bindings offered, %zu valid, %zu bound, %zu "
        "copies replaced, %zu certified here; refused own %zu absent %zu structure %zu family %zu changed %zu inputs %zu "
        "closure %zu certificate %zu; capture %s, outputs %s\n", label, counts->keyed, counts->matched, counts->bindings,
        counts->valid, counts->bound, counts->copies, counts->certified, counts->refused[0], counts->refused[1],
        counts->refused[2], counts->refused[3], counts->refused[4], counts->refused[5], counts->refused[6],
        counts->refused[7], same_capture ? "equal" : "DIFFERENT", same_outputs ? "equal" : "DIFFERENT");
    assert(same_capture && same_outputs && restored.reuse_bindings == counts->bound && !restored.reuse_fallback);
    /* Every record here was kept by a compile, with its identity templates: the restored compile takes the unchanged
     * files' templates from it, and the outputs above are the fresh compile's. */
    fprintf(stderr, "scope reuse, %s: identity templates %zu offered, %zu taken, %zu refused\n", label,
        counts->templates_offered, counts->templates_taken, counts->templates_refused);
    assert(restored.reuse_templates == counts->templates_taken && (!record || (counts->templates_offered &&
        counts->templates_taken)));
    sh_package_library_free(&restored); free(restored_dump);
    for (size_t m = 0; record && m < REUSE_MUTATIONS; m++) {
        char *dump = NULL;
        sh_package_source_link_test_reuse_mutation((int)reuse_mutations[m]);
        ok = reuse_compile(f, sources, env, catalog, native, record, NULL, &dump, &restored, error, sizeof(error));
        sh_package_source_link_test_reuse_mutation(0);
        int differs = !ok || !reuse_text_same(fresh_dump, dump, label, 1) || !reuse_outputs_same(&fresh, &restored) ||
            restored.reuse_fallback || strstr(dump, "capture ") != reuse_last_capture(dump);
        if (differs) {
            g_reuse_caught[m]++;
            fprintf(stderr, "scope reuse, %s: mutation %#x caught (%s)\n", label, reuse_mutations[m],
                !ok ? "compile failed" : strstr(dump, "capture ") != reuse_last_capture(dump) ? "fell back" : "differs");
        }
        sh_package_library_free(&restored); free(dump);
    }
    /* The record trusted (settled mutation 1) and its staging ignored (3); the content-blind key (2) needs a record kept
     * under it, below. */
    for (int m = 1; record && m <= 3; m++) {
        if (m == 2) continue;
        char *dump = NULL;
        sh_package_library_test_settled_mutation(m);
        ok = reuse_compile(f, sources, env, catalog, native, record, NULL, &dump, &restored, error, sizeof(error));
        sh_package_library_test_settled_mutation(0);
        if (!ok || !reuse_outputs_same(&fresh, &restored) || restored.reuse_fallback) {
            g_settled_caught[m]++;
            fprintf(stderr, "scope reuse, %s: settled mutation %d caught (%s)\n", label, m, !ok ? "compile failed" :
                restored.reuse_fallback ? "fell back" : "differs");
        } else if (ok) {
            const char *settled = strstr(restored.repeated_work, "editor groups settled:");
            const char *end = settled ? strchr(settled, '\n') : NULL;
            fprintf(stderr, "scope reuse, %s: settled mutation %d not caught; %.*s\n", label, m,
                settled ? (int)(end ? end - settled : (ptrdiff_t)strlen(settled)) : 0, settled ? settled : "");
        }
        sh_package_library_free(&restored); free(dump);
    }
    sh_package_library_free(&fresh); free(fresh_dump);
}
static void reuse_rescan(sh_package_sources **sources)
{
    char error[1024];
    sh_package_sources_free(*sources);
    *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(*sources);
}
static void test_scope_reuse(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[2048];
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-reuse", saved); assert(CreateDirectoryA(root, NULL));
    f->cycle = 1;
    sh_package_source_link_test_share_defaults(); sh_package_source_link_share(1);
    /* a_editor relays the stock rock entry, so the baseline seeds it and holds its closure (props/rock, the rock model,
     * its material and image); b_mine reaches that model, c_aim that entity, d_other an image no baseline unit reaches
     * (it becomes stock); f_unrelated relays the unrelated entry, whose entity and props/cyc_b target each other, and
     * g_cycle reaches that cycle. */
    create("overrides/a_editor/package.json", "{\"id\":\"editor\",\"name\":\"Editor\"}");
    create("overrides/a_editor/assets/generated/decls/snapeditorentitydef/rock.decl",
        "{ edit = { entityDef = \"props/rock\"; category = \"Props\"; displayName = \"Rock A\"; } }");
    create("overrides/b_mine/package.json", "{\"id\":\"mine\",\"name\":\"Mine\"}");
    create("overrides/b_mine/assets/generated/decls/entitydef/props/mine.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/c_aim/package.json", "{\"id\":\"aim\",\"name\":\"Aim\"}");
    create("overrides/c_aim/assets/generated/decls/entitydef/props/aim.decl",
        "{ class = \"Entity\"; edit = { target = \"props/rock\"; } }");
    create("overrides/d_other/package.json", "{\"id\":\"other\",\"name\":\"Other\"}");
    create("overrides/d_other/assets/generated/decls/entitydef/props/other.decl",
        "{ class = \"Entity\"; edit = { image = \"textures/other\"; } }");
    create("overrides/f_unrelated/package.json", "{\"id\":\"unrelated\",\"name\":\"Unrelated\"}");
    create("overrides/f_unrelated/assets/generated/decls/snapeditorentitydef/unrelated.decl",
        "{ edit = { entityDef = \"props/unrelated\"; category = \"Elsewhere\"; } }");
    create("overrides/g_cycle/package.json", "{\"id\":\"cycle\",\"name\":\"Cycle\"}");
    create("overrides/g_cycle/assets/generated/decls/entitydef/props/loop.decl",
        "{ class = \"Entity\"; edit = { target = \"props/cyc_b\"; } }");
    /* h_red supplies the stock rock image itself: its own unit of that name. */
    create("overrides/h_red/package.json", "{\"id\":\"red\",\"name\":\"Red\"}");
    create("overrides/h_red/assets/generated/image/textures/rock.bimage", "red-image");
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    reuse_counts counts;
    sh_package_source_reuse *first = sh_package_source_reuse_open(), *next = sh_package_source_reuse_open();
    assert(first && next);
    /* Recording changes nothing: the recording compile's capture and outputs are the fresh compile's. */
    reuse_case(f, sources, &env, catalog, &native, NULL, first, "recording", &counts);
    assert(!counts.matched && !counts.bound && counts.recorded == 7 && sh_package_source_reuse_scope_count(first) == 7);
    size_t recorded = sh_package_source_reuse_binding_count(first);
    assert(recorded);
    /* The same library: every scope restored, its bindings bound instead of copies discovered, folded, certified. */
    reuse_case(f, sources, &env, catalog, &native, first, next, "unchanged library", &counts);
    assert(counts.matched == 7 && counts.bound && counts.copies && counts.refused[1]); /* d_other's stock: absent */
    assert(sh_package_source_reuse_binding_count(next) == recorded);
    sh_package_source_reuse_move(first, next);
    /* A contradictory record: h_red is offered b_mine's binding of the stock rock image, which it supplies itself. */
    {
        sh_package_source_reuse *forged = sh_package_source_reuse_open(); assert(forged);
        sh_package_library ignored;
        assert(reuse_compile(f, sources, &env, catalog, &native, NULL, forged, NULL, &ignored, error, sizeof(error)));
        sh_package_library_free(&ignored);
        assert(sh_package_source_reuse_test_copy_binding(forged, 1, "image", "textures/rock", 6, 0));
        reuse_case(f, sources, &env, catalog, &native, forged, next, "contradictory record", &counts);
        assert(counts.refused[0]);
        sh_package_source_reuse_close(forged);
    }
    /* b_mine changed: it is discovered; the others are restored. */
    create("overrides/b_mine/assets/generated/decls/entitydef/props/mine.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; image = \"textures/rock\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "changed package", &counts);
    assert(counts.keyed == 7 && counts.matched == 6 && counts.bound);
    sh_package_source_reuse_move(first, next);
    /* A package added first shifts every scope and the contribution order. */
    create("overrides/aa_first/package.json", "{\"id\":\"first\",\"name\":\"First\"}");
    create("overrides/aa_first/assets/generated/decls/entitydef/props/first.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; target = \"props/aim\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "added package", &counts);
    assert(counts.keyed == 8 && counts.matched == 7 && counts.bound);
    sh_package_source_reuse_move(first, next);
    /* Equal content moved to another folder keeps its key. */
    {
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof(from), "%s/overrides/d_other", root); snprintf(to, sizeof(to), "%s/overrides/z_moved", root);
        assert(MoveFileA(from, to));
    }
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "moved package", &counts);
    assert(counts.keyed == 8 && counts.matched == 8);
    sh_package_source_reuse_move(first, next);
    /* The rock relay removed: the baseline holds the rock closure no more; what the scopes bound there is absent,
     * and their copies are discovered and become stock in the first scope that reaches them, as fresh. */
    remove_named(sources, "a_editor");
    reuse_case(f, sources, &env, catalog, &native, first, next, "removed relay", &counts);
    assert(counts.matched == 7 && counts.refused[1]);
    sh_package_source_reuse_move(first, next);
    /* A baseline seed newly on a bound unit's path: e_rockdef changes props/rock, so the baseline binds its stock bytes
     * as a seed; the scopes that bound the stock props/rock have their own copies again. */
    create("overrides/e_rockdef/package.json", "{\"id\":\"rockdef\",\"name\":\"Rockdef\"}");
    create("overrides/e_rockdef/assets/generated/decls/entitydef/props/rock.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; image = \"textures/other\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "baseline seed on a bound path", &counts);
    assert(counts.bound);
    sh_package_source_reuse_move(first, next);
    /* A unit inside the bound stock cycle changes: e_cyc supplies its own props/cyc_b, so the baseline seeds the stock
     * bytes on that path and g_cycle's recorded bindings into the cycle are judged against the pool as it is now (the
     * unit the cycle closes through, and everything reaching it, stay g_cycle's own where they no longer hold). */
    create("overrides/e_cyc/package.json", "{\"id\":\"cyc\",\"name\":\"Cyc\"}");
    create("overrides/e_cyc/assets/generated/decls/entitydef/props/cyc_b.decl",
        "{ class = \"Entity\"; edit = { target = \"props/unrelated\"; image = \"textures/other\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "changed unit in a bound cycle", &counts);
    assert(counts.matched && counts.bound);
    sh_package_source_reuse_move(first, next);
    /* Settled editor groups: i_faith ships the stock props/unrelated entity unchanged (it joins the stock unit), so the
     * entity's group composes with nothing staged and settles in the next compile; then i_faith's own copies (below). */
    create("overrides/i_faith/package.json", "{\"id\":\"faith\",\"name\":\"Faith\"}");
    create("overrides/i_faith/assets/generated/decls/entitydef/props/unrelated.decl",
        "{ class = \"Entity\"; edit = { image = \"textures/unrelated\"; target = \"props/cyc_b\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "faithful copy", &counts);
    assert(counts.settled_keyed && counts.settled_recorded);
    sh_package_source_reuse_move(first, next);
    reuse_counts recorded_state = counts;
    reuse_case(f, sources, &env, catalog, &native, first, next, "faithful copy, unchanged", &counts);
    /* Every group recorded settled settles again, and is recorded again. */
    assert(counts.settled_hits && counts.settled_hits == recorded_state.settled_recorded &&
        counts.settled_recorded == recorded_state.settled_recorded);
    sh_package_source_reuse_move(first, next);
    reuse_counts unchanged_state = counts;
    /* i_faith's own copy: a changed edit field keeps it the package's own member of the group, which still composes with
     * nothing staged (an entity's edit fields are each member's own) and settles. */
    create("overrides/i_faith/assets/generated/decls/entitydef/props/unrelated.decl",
        "{ class = \"Entity\"; edit = { image = \"textures/other\"; target = \"props/cyc_b\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "own copy", &counts);
    assert(counts.settled_hits < unchanged_state.settled_hits && counts.settled_recorded);
    sh_package_source_reuse_move(first, next);
    reuse_counts own_state = counts;
    /* The content of the key (settled mutation 2): a record kept with keys blind to the members' content. */
    sh_package_source_reuse *blind = sh_package_source_reuse_open(); assert(blind);
    {
        sh_package_library recorded;
        sh_package_library_test_settled_mutation(2);
        assert(reuse_compile(f, sources, &env, catalog, &native, NULL, blind, NULL, &recorded, error, sizeof(error)));
        sh_package_library_test_settled_mutation(0);
        sh_package_library_free(&recorded);
        assert(sh_package_source_reuse_settled_count(blind));
    }
    /* The same copy stating editor variables: they are the entity group's shared surface, so the baseline member stages
     * their union, and the group's key (its members' content) differs. */
    create("overrides/i_faith/assets/generated/decls/entitydef/props/unrelated.decl",
        "{ class = \"Entity\"; editorVars = { faith = 1; } edit = { image = \"textures/other\"; target = \"props/cyc_b\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "own copy with editor variables", &counts);
    assert(counts.settled_hits < own_state.settled_recorded);
    sh_package_source_reuse_move(first, next);
    /* With the blind record and key the group settles as it was, and the library differs from the fresh one. */
    {
        sh_package_library fresh, blinded;
        assert(reuse_compile(f, sources, &env, catalog, &native, NULL, NULL, NULL, &fresh, error, sizeof(error)));
        sh_package_library_test_settled_mutation(2);
        int ok = reuse_compile(f, sources, &env, catalog, &native, blind, NULL, NULL, &blinded, error, sizeof(error));
        sh_package_library_test_settled_mutation(0);
        int differs = !ok || !reuse_outputs_same(&fresh, &blinded) || blinded.reuse_fallback;
        if (differs) g_settled_caught[2]++;
        fprintf(stderr, "scope reuse, own copy with editor variables: settled mutation 2 with a blind record %s\n",
            differs ? "caught (differs)" : "NOT caught");
        sh_package_library_free(&fresh); if (ok) sh_package_library_free(&blinded);
    }
    sh_package_source_reuse_close(blind);
    /* Inherited editor collections: j_menu's editor entry inherits j_menu's own base entry. Unchanged, both groups
     * settle; when the base's collection changes, the base's group and the entry's (the base is on its chain) compose
     * again, exactly as fresh. */
    create("overrides/j_menu/package.json", "{\"id\":\"menu\",\"name\":\"Menu\"}");
    create("overrides/j_menu/assets/generated/decls/snapeditorentitydef/j_base.decl",
        "{ edit = { category = \"Menu\"; previewImage = \"textures/rock_icon\"; } }");
    create("overrides/j_menu/assets/generated/decls/snapeditorentitydef/j_child.decl",
        "{ inherit = \"j_base\"; edit = { entityDef = \"props/rock\"; displayName = \"Child\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "inherited entry", &counts);
    sh_package_source_reuse_move(first, next);
    reuse_counts entry_state = counts;
    reuse_case(f, sources, &env, catalog, &native, first, next, "inherited entry, unchanged", &counts);
    reuse_counts inherited_state = counts;
    /* Every editor group recorded settles again, those that staged members by staging them again. */
    fprintf(stderr, "scope reuse, inherited entry, unchanged: editor groups keyed %zu, settled %zu (recorded before %zu), "
        "members staged again %zu\n", counts.editor_keyed, counts.editor_hits, entry_state.editor_recorded, counts.restaged);
    assert(counts.editor_keyed && counts.editor_hits == entry_state.editor_recorded && counts.restaged);
    sh_package_source_reuse_move(first, next);
    create("overrides/j_menu/assets/generated/decls/snapeditorentitydef/j_base.decl",
        "{ edit = { category = \"Menu B\"; previewImage = \"textures/rock_icon\"; } }");
    reuse_rescan(&sources);
    reuse_case(f, sources, &env, catalog, &native, first, next, "inherited base changed", &counts);
    /* The j_menu groups that settled before miss now: the base changed, and it is on the entry's chain. */
    fprintf(stderr, "scope reuse, inherited base changed: editor groups settled %zu, before %zu\n", counts.editor_hits,
        inherited_state.editor_hits);
    assert(counts.editor_hits <= inherited_state.editor_hits);
    sh_package_source_reuse_move(first, next);
    /* The record's settled groups survive encoding, by key, each once, with what they staged; a record from before them
     * (SMPSR1) still reads, with none, and a settled section out of order is refused. */
    {
        unsigned char *bytes = NULL, *again = NULL;
        size_t length = 0, again_length = 0;
        sh_package_source_reuse *decoded = NULL, *legacy = NULL, *refused = NULL;
        assert(sh_package_source_reuse_settled_count(first));
        assert(sh_package_source_reuse_encode(first, &bytes, &length) && length > 6 && !memcmp(bytes, "SMPSR2", 6));
        assert(sh_package_source_reuse_decode(bytes, length, &decoded) == 1 && decoded);
        assert(sh_package_source_reuse_encode(decoded, &again, &again_length) && again_length == length &&
            !memcmp(again, bytes, length));
        size_t settled = sh_package_source_reuse_settled_count(decoded), section = 8, staged = 0, first_size = 0, second_size = 0;
        assert(settled);
        for (size_t i = 0; i < settled; i++) {
            unsigned char key[32];
            size_t outputs = 0, found = 0;
            const unsigned char *kept = NULL;
            assert(sh_package_source_reuse_test_settled_entry(decoded, i, key, &outputs));
            assert(sh_package_source_reuse_settled(decoded, key, &kept, &found) && found == outputs);
            assert(sh_package_source_reuse_settled(first, key, NULL, &found) && found == outputs);
            section += 40 + outputs; staged += outputs != 0;
            if (i == 0) first_size = 40 + outputs; else if (i == 1) second_size = 40 + outputs;
        }
        fprintf(stderr, "scope reuse record: %zu settled groups, %zu with staged members, %zu bytes of %zu\n", settled, staged,
            section, length);
        assert(length > section);
        unsigned char *changed = malloc(length); assert(changed);
        memcpy(changed, bytes, length);
        if (settled > 1) {
            /* The first two entries swapped. */
            size_t at = length - section + 8;
            memcpy(changed + at, bytes + at + first_size, second_size);
            memcpy(changed + at + second_size, bytes + at, first_size);
            assert(sh_package_source_reuse_decode(changed, length, &refused) == 0 && !refused);
            memcpy(changed, bytes, length);
        }
        memcpy(changed, "SMPSR1", 6);
        assert(sh_package_source_reuse_decode(changed, length - section, &legacy) == 1 && legacy &&
            !sh_package_source_reuse_settled_count(legacy) &&
            sh_package_source_reuse_binding_count(legacy) == sh_package_source_reuse_binding_count(decoded));
        free(changed); free(bytes); free(again);
        sh_package_source_reuse_close(decoded); sh_package_source_reuse_close(legacy);
    }
    /* Failure after bindings were admitted: the compile fails over to a fresh one that equals the fresh compile. */
    {
        char *fresh_dump = NULL, *dump = NULL;
        sh_package_library fresh, restored;
        int ok = reuse_compile(f, sources, &env, catalog, &native, NULL, NULL, &fresh_dump, &fresh, error, sizeof(error));
        assert(ok);
        size_t failed_over = 0;
        for (long nth = 1;; nth++) {
            sh_package_source_link_test_reuse_refusal(nth);
            ok = reuse_compile(f, sources, &env, catalog, &native, first, NULL, &dump, &restored, error, sizeof(error));
            long left = sh_package_source_link_test_reuse_refusal_left();
            sh_package_source_link_test_reuse_refusal(0);
            if (!ok) fprintf(stderr, "reuse refusal %ld: %s\n", nth, error);
            assert(ok);
            failed_over += left <= 0;
            if (!reuse_text_same(fresh_dump, reuse_last_capture(dump), "reuse refusal fallback", 0) ||
                !lod_same(&fresh, &restored, "reuse refusal")) assert(0);
            sh_package_library_free(&restored); free(dump); dump = NULL;
            if (left > 0) break; /* past the last refusal point: an ordinary restore */
            assert(nth < 4096);
        }
        fprintf(stderr, "scope reuse: %zu refusal points failed over to a fresh compile\n", failed_over);
        assert(failed_over > 1);
        /* Outside fallback: the library reports the failure for its caller to compile again. */
        sh_package_library_native outside = native; outside.reuse_fallback_outside = 1;
        sh_package_source_link_test_reuse_refusal(3);
        ok = reuse_compile(f, sources, &env, catalog, &outside, first, NULL, NULL, &restored, error, sizeof(error));
        sh_package_source_link_test_reuse_refusal(0);
        assert(!ok && restored.reuse_fallback && !restored.compiled);
        sh_package_library_free(&restored); sh_package_library_free(&fresh); free(fresh_dump);
    }
    sh_package_source_reuse_close(first); sh_package_source_reuse_close(next);
    sh_package_sources_free(sources);
    sh_package_native_assets_close(assets);
    sh_package_source_link_test_share_defaults();
    f->cycle = 0;
    {
        /* The fixture deletes what it created by path. */
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof(from), "%s/overrides/z_moved", root); snprintf(to, sizeof(to), "%s/overrides/d_other", root);
        assert(MoveFileA(from, to));
    }
    strcpy_s(root, sizeof(root), saved);
}
/* Native answers a restored scope's bindings depend on are asked again, for that scope, as the fold asks them:
 * - image availability (R-309): recorded while the registered rock image is usable, restored once it is the exclusion
 *   branch's placeholder, the gate-opening scope must serve its private copy again, and everything that reaches it;
 * - the native implicit adapter, which may answer per scope: recorded without a request, restored once it requests
 *   one for one scope's copy of the rock model, that scope keeps its own model. */
static size_t g_reuse_request_scope = SIZE_MAX;
static sh_package_source_implicit g_reuse_request_inner;
static int reuse_request_implicit(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visitor, char *error, size_t capacity)
{
    if (!observing && scope == g_reuse_request_scope && !strcmp(unit->family, "model") && !strcmp(unit->name, "models/rock.lwo") &&
        visit(visitor, "constructor/extra", "image", "textures/other") != 1) return 0;
    return g_reuse_request_inner ? g_reuse_request_inner(context, scope, observing, unit, visit, visitor, error, capacity) : 1;
}
static void test_scope_reuse_native_answers(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[2048];
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-reuse-native", saved); assert(CreateDirectoryA(root, NULL));
    sh_package_source_link_test_share_defaults(); sh_package_source_link_share(1);
    create("overrides/a_relay/package.json", "{\"id\":\"relay\",\"name\":\"Relay\"}");
    create("overrides/a_relay/assets/generated/decls/snapeditorentitydef/rock.decl",
        "{ edit = { entityDef = \"props/rock\"; category = \"Props\"; displayName = \"Rock R\"; } }");
    create("overrides/b_gated/package.json",
        "{\"id\":\"gated\",\"name\":\"Gated\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}}}");
    create("overrides/b_gated/assets/generated/decls/entitydef/props/gated.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/c_plain/package.json", "{\"id\":\"plain\",\"name\":\"Plain\"}");
    create("overrides/c_plain/assets/generated/decls/entitydef/props/plain.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    /* d_red changes the stock other image and reaches the rock model too. */
    create("overrides/d_red/package.json", "{\"id\":\"red\",\"name\":\"Red\"}");
    create("overrides/d_red/assets/generated/decls/entitydef/props/red.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/d_red/assets/generated/image/textures/other.bimage", "red-other-image");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    native.image_excluded = ie_query;
    memset(&g_ie_images, 0, sizeof(g_ie_images));
    *(const char **)(g_ie_images.bytes + 0x08) = "image";
    *(void ***)(g_ie_images.bytes + 0x20) = g_ie_images.items;
    *(int *)(g_ie_images.bytes + 0x28) = 1;
    g_ie_images.items[0] = g_ie_rock; g_ie_live = &g_ie_images;
    ie_bind(1);
    reuse_counts counts;
    sh_package_source_reuse *usable = sh_package_source_reuse_open(), *next = sh_package_source_reuse_open();
    assert(usable && next);
    /* Recorded with a usable rock image: every scope shares the rock closure. */
    ie_rock(0);
    reuse_case(f, sources, &env, catalog, &native, NULL, usable, "usable image, recording", &counts);
    reuse_case(f, sources, &env, catalog, &native, usable, next, "usable image, restored", &counts);
    assert(counts.matched == 4 && counts.bound && !counts.refused[7]);
    /* Now the exclusion placeholder: the gated scope's copy of the image is private, and so is what reaches it. */
    ie_rock(1);
    reuse_case(f, sources, &env, catalog, &native, usable, next, "image now excluded", &counts);
    assert(counts.refused[7] && counts.refused[6]); /* the image's certificate, then the closure reaching it */
    /* A contradictory record: the gated scope keeps its own rock material now (it reaches the private image), and
     * is offered c_plain's binding of the stock one in a later pass, which only the own-unit check refuses. */
    {
        sh_package_source_reuse *forged = sh_package_source_reuse_open(); assert(forged);
        sh_package_library ignored;
        assert(reuse_compile(f, sources, &env, catalog, &native, NULL, forged, NULL, &ignored, error, sizeof(error)));
        sh_package_library_free(&ignored);
        assert(sh_package_source_reuse_test_copy_binding(forged, 2, "material", "generic/rock", 1, 2));
        reuse_case(f, sources, &env, catalog, &native, forged, next, "own unit, contradictory record", &counts);
        assert(counts.refused[0]);
        sh_package_source_reuse_close(forged);
    }
    /* A per-scope native request for d_red's copy of the rock model (scope 3): it names d_red's own changed image, so
     * that copy binds differently from the stock model and stays d_red's own. */
    ie_rock(0);
    sh_package_library_native requests = native;
    g_reuse_request_inner = native.source.implicit; requests.source.implicit = reuse_request_implicit;
    g_reuse_request_scope = 3;
    reuse_case(f, sources, &env, catalog, &requests, usable, next, "per-scope native request", &counts);
    assert(counts.refused[7]);
    g_reuse_request_scope = SIZE_MAX; g_reuse_request_inner = NULL;
    sh_package_source_reuse_close(usable); sh_package_source_reuse_close(next);
    sh_image_exclusion_test_bind(NULL);
    sh_package_native_assets_close(assets);
    sh_package_sources_free(sources);
    sh_package_source_link_test_share_defaults();
    strcpy_s(root, sizeof(root), saved);
}
/* R-389 under unchanged-scope reuse: a record kept while the rock image was usable binds Plain's and Both's rock image
 * as shared stock. Restored after the image became an exclusion placeholder, those scopes isolate it exactly as a fresh
 * compile does (the restored capture and outputs equal the fresh ones), whether the shared image is refused by its
 * certificate or taken back by the scope's authored request. */
static void test_image_exclusion_authored_reuse(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[1024];
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-exclusion-authored-reuse", saved); assert(CreateDirectoryA(root, NULL));
    sh_package_source_link_test_share_defaults(); sh_package_source_link_share(1);
    create("overrides/a_stock/package.json", "{\"id\":\"stock\",\"name\":\"Stock\"}");
    create("overrides/a_stock/assets/generated/decls/entitydef/stock/thing.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/b_plain/package.json", "{\"id\":\"plain\",\"name\":\"Plain\"}");
    create("overrides/b_plain/assets/generated/decls/material/generic/plain.decl", "{ tex \"textures/rock\" }");
    create("overrides/c_both/package.json", "{\"id\":\"both\",\"name\":\"Both\"}");
    create("overrides/c_both/assets/generated/decls/entitydef/both/thing.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/c_both/assets/generated/decls/material/generic/both.decl", "{ tex \"textures/rock\" }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    native.image_excluded = ie_query;
    memset(&g_ie_images, 0, sizeof(g_ie_images));
    *(const char **)(g_ie_images.bytes + 0x08) = "image";
    *(void ***)(g_ie_images.bytes + 0x20) = g_ie_images.items;
    *(int *)(g_ie_images.bytes + 0x28) = 1;
    g_ie_images.items[0] = g_ie_rock; g_ie_live = &g_ie_images;
    ie_bind(1);
    reuse_counts counts;
    sh_package_source_reuse *usable = sh_package_source_reuse_open(), *next = sh_package_source_reuse_open();
    assert(usable && next);
    ie_rock(0);
    reuse_case(f, sources, &env, catalog, &native, NULL, usable, "authored image usable, recording", &counts);
    ie_rock(1);
    {
        /* The restored compile cannot keep the record's shared image: it is made again without reuse, and equals a
         * fresh compile. */
        sh_package_library fresh, out; char *fresh_dump = NULL, *dump = NULL;
        assert(reuse_compile(f, sources, &env, catalog, &native, NULL, NULL, &fresh_dump, &fresh, error, sizeof(error)));
        int ok = reuse_compile(f, sources, &env, catalog, &native, usable, next, &dump, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "authored image now excluded, restored: %s\n", error); assert(ok);
        assert(reuse_text_same(fresh_dump, reuse_last_capture(dump), "authored image now excluded", 0));
        assert(lod_same(&fresh, &out, "authored image now excluded"));
        assert(!out.reuse_bindings && !out.reuse_fallback);
        const sh_package_provenance_binding *plain = binding(&out, sources, "Plain", "", "image", "textures/rock");
        const sh_package_provenance_binding *both = binding(&out, sources, "Both", "", "image", "textures/rock");
        assert(plain && both && !strcmp(plain->name, both->name) && !strncmp(plain->name, "generated/image/smp/", 20));
        assert(!binding(&out, sources, "Stock", "", "image", "textures/rock"));
        assert(!binding(&out, sources, "Stock", "", "material", "generic/rock"));
        sh_package_library_free(&fresh); sh_package_library_free(&out); free(fresh_dump); free(dump);
    }
    sh_package_source_reuse_close(usable); sh_package_source_reuse_close(next);
    sh_image_exclusion_test_bind(NULL);
    sh_package_native_assets_close(assets);
    sh_package_sources_free(sources);
    sh_package_source_link_test_share_defaults();
    strcpy_s(root, sizeof(root), saved);
}
/* The mutation controls across both: dropping the closure, the certificate, the native requests or every validation
 * (the record trusted) must make some case differ. The digest, the consulted inputs and the own-unit check are
 * reported, not required: where a record and a capture disagree in these cases another validation refuses the same
 * binding (the structure and inputs of the stock unit's own recording, the closure reaching what the scope keeps),
 * and the contradictory records above show the own-unit check refusing first. */
/* Retained reverse graph (R-386): with the caller's assertion that the stock originals are the catalog's, the first
 * compile keeps the shape of the stock reverse base in its memo and the next compile of the same catalog, readers and
 * renderer rebuilds the base from it, with the oracle's fresh discovery encoding the same bytes and the capture and
 * outputs equal to the fresh compile's. The shape survives export and import (a damaged or truncated record is
 * refused); another catalog is another key; without the assertion nothing is kept. */
static void test_reverse_graph(fixture *f, const sh_resource_catalog *catalog)
{
    char saved[MAX_PATH], error[2048], path[4096];
    char *dump_a = NULL, *dump_b = NULL, *dump_c = NULL, *dump_d = NULL, *dump_e = NULL;
    size_t captured0 = 0, reused0 = 0, captured = 0, reused = 0, mismatches = 0, length = 0, units = 0;
    unsigned char *bytes = NULL, key[32], other_key[32];
    sh_package_library a = {0}, b = {0}, c = {0}, d = {0}, e = {0};
    strcpy_s(saved, sizeof(saved), root);
    snprintf(root, sizeof(root), "%s-reverse-graph", saved); assert(CreateDirectoryA(root, NULL));
    f->cycle = 0;
    sh_package_source_link_test_share_defaults(); sh_package_source_link_share(1);
    create("overrides/a_editor/package.json", "{\"id\":\"editor\",\"name\":\"Editor\"}");
    create("overrides/a_editor/assets/generated/decls/snapeditorentitydef/rock.decl",
        "{ edit = { entityDef = \"props/rock\"; category = \"Props\"; displayName = \"Rock A\"; } }");
    create("overrides/b_mine/package.json", "{\"id\":\"mine\",\"name\":\"Mine\"}");
    create("overrides/b_mine/assets/generated/decls/entitydef/props/mine.decl",
        "{ class = \"Entity\"; edit = { model = \"models/rock.lwo\"; } }");
    create("overrides/c_aim/package.json", "{\"id\":\"aim\",\"name\":\"Aim\"}");
    create("overrides/c_aim/assets/generated/decls/entitydef/props/aim.decl",
        "{ class = \"Entity\"; edit = { target = \"props/rock\"; } }");
    sh_package_compile_environment env = {0}; env.baseline = original; env.baseline_context = f;
    sh_package_native_assets *assets = sh_package_native_assets_open(root); assert(assets);
    sh_package_library_native native = sh_package_native_assets_adapter(assets);
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_source_memo *memo = sh_package_source_memo_open(), *other = sh_package_source_memo_open();
    assert(memo && other);
    native.source.memo = memo; native.source.reverse_retain = 1;
    sh_package_source_link_test_reverse_graph(&captured0, &reused0, &mismatches);
    assert(!mismatches && !sh_package_source_memo_reverse_key(memo, key, &units));
    /* Fresh: the stock reverse base is discovered and its shape kept. */
    assert(reuse_compile(f, sources, &env, catalog, &native, NULL, NULL, &dump_a, &a, error, sizeof(error)));
    sh_package_source_link_test_reverse_graph(&captured, &reused, &mismatches);
    assert(captured == captured0 + 1 && reused == reused0 && !mismatches);
    assert(strstr(a.repeated_work, "reverse base ") && !strstr(a.repeated_work, "rebuilt from the retained reverse graph"));
    assert(sh_package_source_memo_reverse_key(memo, key, &units) && units);
    /* The same inputs: the base is rebuilt from the shape; the oracle's fresh discovery encodes the same bytes. */
    assert(reuse_compile(f, sources, &env, catalog, &native, NULL, NULL, &dump_b, &b, error, sizeof(error)));
    sh_package_source_link_test_reverse_graph(&captured, &reused, &mismatches);
    assert(captured == captured0 + 1 && reused == reused0 + 1 && !mismatches);
    assert(strstr(b.repeated_work, "rebuilt from the retained reverse graph"));
    assert(reuse_text_same(dump_a, dump_b, "reverse graph", 0) && lod_same(&a, &b, "reverse graph"));
    /* The record: exported, refused when truncated or altered, imported whole into another memo and reused there. */
    assert(sh_package_source_memo_reverse_export(memo, &bytes, &length) && bytes && length > 100);
    assert(!sh_package_source_memo_reverse_import(other, bytes, length - 1));
    assert(!sh_package_source_memo_reverse_import(other, bytes + 1, length - 1));
    bytes[length / 2] ^= 1;
    assert(!sh_package_source_memo_reverse_import(other, bytes, length));
    bytes[length / 2] ^= 1;
    assert(!sh_package_source_memo_reverse_key(other, other_key, NULL));
    assert(sh_package_source_memo_reverse_import(other, bytes, length));
    assert(sh_package_source_memo_reverse_key(other, other_key, NULL) && !memcmp(key, other_key, 32));
    {
        unsigned char *again = NULL; size_t again_length = 0;
        assert(sh_package_source_memo_reverse_export(other, &again, &again_length) && again_length == length &&
            !memcmp(again, bytes, length));
        free(again);
    }
    native.source.memo = other;
    assert(reuse_compile(f, sources, &env, catalog, &native, NULL, NULL, &dump_c, &c, error, sizeof(error)));
    sh_package_source_link_test_reverse_graph(&captured, &reused, &mismatches);
    assert(captured == captured0 + 1 && reused == reused0 + 2 && !mismatches);
    assert(reuse_text_same(dump_a, dump_c, "imported reverse graph", 0) && lod_same(&a, &c, "imported reverse graph"));
    /* Another catalog (the partial image rows) is another key: discovered afresh, and the memo then keeps that
     * shape instead. */
    g_partial_rows = 1;
    catalog_file("base/snap_gameresources.pindex", 0, 1, 0); catalog_file("base/gameresources.pindex", 1, 1, 0);
    g_partial_rows = 0;
    create("base/snap_gameresources.resources", ""); create("base/snap_gameresources.patch", "");
    create("base/gameresources.resources", ""); create("base/gameresources.patch", "");
    snprintf(path, sizeof(path), "%s/base", root);
    sh_resource_catalog *aliased = sh_resource_catalog_open(path, error, sizeof(error)); assert(aliased);
    native.source.memo = memo;
    if (!reuse_compile(f, sources, &env, aliased, &native, NULL, NULL, &dump_d, &d, error, sizeof(error))) {
        fprintf(stderr, "reverse graph, another catalog: %s\n", error); assert(0);
    }
    sh_package_source_link_test_reverse_graph(&captured, &reused, &mismatches);
    assert(captured == captured0 + 2 && reused == reused0 + 2 && !mismatches);
    assert(!strstr(d.repeated_work, "rebuilt from the retained reverse graph"));
    assert(sh_package_source_memo_reverse_key(memo, other_key, NULL) && memcmp(key, other_key, 32));
    /* Without the assertion nothing is kept or rebuilt. */
    sh_package_source_memo_reverse_clear(memo);
    assert(!sh_package_source_memo_reverse_key(memo, other_key, NULL));
    native.source.reverse_retain = 0;
    assert(reuse_compile(f, sources, &env, catalog, &native, NULL, NULL, &dump_e, &e, error, sizeof(error)));
    sh_package_source_link_test_reverse_graph(&captured, &reused, &mismatches);
    assert(captured == captured0 + 2 && reused == reused0 + 2 && !mismatches);
    assert(!sh_package_source_memo_reverse_key(memo, other_key, NULL) && lod_same(&a, &e, "reverse graph off"));
    fprintf(stderr, "reverse graph: %zu stock units kept, %zu bytes exported, rebuilt twice with the oracle agreeing\n",
        units, length);
    sh_package_library_free(&a); sh_package_library_free(&b); sh_package_library_free(&c);
    sh_package_library_free(&d); sh_package_library_free(&e);
    free(dump_a); free(dump_b); free(dump_c); free(dump_d); free(dump_e); free(bytes);
    sh_package_source_memo_close(memo); sh_package_source_memo_close(other);
    sh_resource_catalog_close(aliased);
    sh_package_sources_free(sources); sh_package_native_assets_close(assets);
    sh_package_source_link_test_share_defaults();
    strcpy_s(root, sizeof(root), saved);
}
static void reuse_mutations_report(void)
{
    for (size_t m = 0; m < REUSE_MUTATIONS; m++) {
        int required = reuse_mutations[m] == (1u << 3) || reuse_mutations[m] == (1u << 4) ||
            reuse_mutations[m] == (1u << 6) || reuse_mutations[m] == (1u << 7);
        fprintf(stderr, "scope reuse mutation %#x: caught in %u cases%s\n", reuse_mutations[m], g_reuse_caught[m],
            required ? "" : " (backed by other validations)");
        assert(!required || g_reuse_caught[m]);
    }
    for (int m = 1; m <= 3; m++) {
        fprintf(stderr, "settled editor group mutation %d: caught in %u cases\n", m, g_settled_caught[m]);
        assert(g_settled_caught[m]);
    }
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
    /* Only the labelled rock row's parent names this image, as its preview icon. */
    package("icon", "Icon", "generated/image/textures/rock_icon.bimage", "icon-image");
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
    test_capture_ownership(&f, sources, &env, catalog, registry, &native, &library);
    test_body_backing_protocol();
    test_same_backing_changes();
    test_capture_backings(&f, sources, &env, catalog, registry, &native, &library);
    reverse_label_aliases(&f, sources, &env, catalog, registry, &native);
    test_image_exclusion_isolation(&f, catalog);
    test_image_exclusion_authored(&f, catalog);
    test_rebound_original_certificate(&f, catalog);
    test_image_recovery_fixed_binding(&f, catalog);
    test_shared_stock_placement(&f, catalog);
    test_shared_stock_convergence(&f, catalog);
    test_scope_reuse(&f, catalog);
    test_scope_reuse_native_answers(&f, catalog);
    test_image_exclusion_authored_reuse(&f, catalog);
    test_reverse_graph(&f, catalog);
    reuse_mutations_report();
    test_share_refusals(&f, sources, &env, catalog, registry, &native, &library);
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
    test_stock_route_campaign_boundary(&f, &env, registry, &native);
    test_absent_program_boundary(&f, sources, &env, catalog, registry, &native);
    test_player_hands(&f, &env, catalog, registry, &native);
    test_empty_parent(&f, &env, catalog, registry, &native);
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
