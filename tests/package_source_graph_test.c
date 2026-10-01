/* Candidate source inspection uses native metadata, never loaded object state. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_source_graph.h"
#include "package_keys.h"
#include "package_source_link.h"
#include "package_native_names.h"
#include "resource_graph.h"
#include "decl_native_reference.h"
#include "package_library.h"
#include "package_native_assets.h"
#include "package_sound.h"
#include "audio_id_reservations.h"
#include "package_hud.h"
#include "decl_script_schema.h"
#include "model_binary.h"
#include "font_binary.h"
#include "package_movie.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "package_fixture.h"
#include "../src/backend/virtual_material_runtime.c"

void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; assert(0); return NULL; }
sh_patch_status hook_commit(void *tramp) { (void)tramp; assert(0); return B2_PATCH_OK; }
void backend_log(const char *message) { (void)message; }

typedef struct region { void *address; size_t length; } region;
typedef struct fixture {
    region regions[768]; size_t count;
    void *registry, *reflection, *root;
    const char *unreadable;
    size_t object_state_reads;
    uintptr_t script_table;
} fixture;
static void *alloc(fixture *f, size_t n)
{ void *p = calloc(1, n); assert(p && f->count < 768); f->regions[f->count++] = (region){p,n}; return p; }
static void pointer(void *p, size_t at, const void *value)
{ uintptr_t v = (uintptr_t)value; memcpy((char *)p + at, &v, 8); }
static void number(void *p, size_t at, int n) { memcpy((char *)p + at, &n, 4); }
static void audio_bank(unsigned char *body, int bank, int event, int language)
{
    memset(body,0,49); memcpy(body,"BKHD",4); number(body,4,16); number(body,8,113);
    number(body,12,bank); number(body,16,language);
    memcpy(body+24,"HIRC",4); number(body,28,17); number(body,32,1);
    body[36]=4; number(body,37,8); number(body,41,event);
}
/* One event, one media identity resident inside the bank and one it streams. */
static size_t audio_bank_media(unsigned char *body, int bank, int event, int language,
    int carried, int streamed)
{
    size_t at;
    memset(body,0,160); memcpy(body,"BKHD",4); number(body,4,16); number(body,8,113);
    number(body,12,bank); number(body,16,language);
    memcpy(body+24,"DIDX",4); number(body,28,12);
    number(body,32,carried); number(body,36,0); number(body,40,4);
    memcpy(body+44,"DATA",4); number(body,48,4);
    memcpy(body+56,"HIRC",4); number(body,64,3); at=68;
    body[at]=4; number(body,(int)at+1,8); number(body,(int)at+5,event); at+=13;
    body[at]=2; number(body,(int)at+1,18); number(body,(int)at+5,601);
    number(body,(int)at+9,0x20001); body[at+13]=0; number(body,(int)at+14,carried); at+=23;
    body[at]=2; number(body,(int)at+1,18); number(body,(int)at+5,602);
    number(body,(int)at+9,0x20001); body[at+13]=1; number(body,(int)at+14,streamed); at+=23;
    number(body,60,(int)(at-60-4));
    return at;
}
static char *string(fixture *f, const char *s)
{ char *p = alloc(f, strlen(s) + 1); strcpy(p, s); return p; }
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    fixture *f = context;
    if (address >= (uintptr_t)f->root + 0x60 && address < (uintptr_t)f->root + 0x168) {
        f->object_state_reads++; return 0;
    }
    for (size_t i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].address;
        if (address >= start && address - start <= f->regions[i].length &&
            length <= f->regions[i].length - (size_t)(address - start)) {
            memcpy(out, (void *)address, length); return 1;
        }
    }
    return 0;
}
static void fields(fixture *f, void *record, int count, const char *const *types,
    const char *const *ops, const char *const *names)
{
    void *rows = alloc(f, (count + 1) * 0x48); pointer(record, 0x20, rows);
    for (int i = 0; i < count; i++) {
        pointer(rows, i * 0x48, string(f, types[i]));
        pointer(rows, i * 0x48 + 8, string(f, ops[i]));
        pointer(rows, i * 0x48 + 0x10, string(f, names[i])); number(rows, i * 0x48 + 0x1c, 8);
    }
}
static void setup(fixture *f)
{
    static const char *classes[] = {"idDeclEntityDef", "Entity", "OpaqueStringReader",
        "idDeclTypeInfo", "idDeclActorModifier", "idDeclTypeInfoGraph", "idDeclSnapEditorEntity", "idImage", "idRenderModel", "idStaticModel",
        "idStr", "idList < EntityRefs >", "EditorProperty", "EditorSheet", "idList < EditorProperty >",
        "idList < EditorSheet >", "idDeclSnapPropertyInspector_WhiteListEncounterDecl", "idList < Encounters >", "AlternateEntity"};
    static const char *types[] = {"entityDef", "actorModifier", "snapEditorEntityDef", "image",
        "snapPropertyInspector_WhiteListEncounterDecl"};
    static const int indices[] = {0, 4, 6, 7, 16};
    void *managers, *container, *records, *objects, *pointers;
    memset(f, 0, sizeof(*f));
    f->registry = alloc(f, 0x18); managers = alloc(f, 5 * 8);
    pointer(f->registry, 8, managers); number(f->registry, 0x10, 5);
    for (int i = 0; i < 5; i++) {
        void *manager = alloc(f, 0x90); pointer(managers, i * 8, manager);
        pointer(manager, 8, string(f, types[i])); pointer(manager, 0x10, string(f, classes[indices[i]]));
        if (!i) {
            void *entries = alloc(f, 8); f->root = alloc(f, 0x168);
            pointer(entries, 0, f->root); pointer(manager, 0x20, entries); number(manager, 0x28, 1);
            pointer(f->root, 8, string(f, "root")); pointer(f->root, 0x10, manager);
            /* Any attempt to inspect this local prepared state is refused. */
            pointer(f->root, 0x140, string(f, "edit = { target = \"local-only\"; }"));
        }
    }
    f->reflection = alloc(f, 0xb0); container = alloc(f, 0x30); records = alloc(f, 20 * 0x38);
    objects = alloc(f, 20 * 16); pointers = alloc(f, 20 * 16);
    pointer(f->reflection, 0, container); pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, 20);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, 20);
    for (int i = 0; i < 19; i++) {
        pointer(records, i * 0x38, string(f, classes[i]));
        pointer(records, i * 0x38 + 8, string(f, i == 4 || i == 5 || i == 6 || i == 16 ? "idDeclTypeInfo" : ""));
    }
    pointer(pointers, 8, (void *)0x10); pointer(pointers, 4 * 16 + 8, (void *)0x10);
    pointer(pointers, 6 * 16 + 8, (void *)0x10); pointer(objects, 2 * 16 + 8, (void *)0x20);
    pointer(objects, 5 * 16 + 8, (void *)0x30);
    pointer(pointers, 7 * 16 + 8, (void *)0x40);
    pointer(pointers, 8 * 16 + 8, (void *)0x50);
    pointer(pointers, 9 * 16 + 8, (void *)0x60);
    pointer(objects, 10 * 16 + 8, (void *)0x70);
    pointer(pointers, 16 * 16 + 8, (void *)0x10);
    {
        const int lists[] = {11, 14, 15, 17};
        const int elements[] = {0, 12, 13, 0};
        for (size_t i = 0; i < 4; i++) {
            int index = lists[i]; void *element = alloc(f, 0x48);
            pointer(objects, index * 16 + 8, (void *)0x80);
            pointer(records, index * 0x38 + 0x18, element);
            pointer(element, 0, string(f, classes[elements[i]]));
            pointer(element, 8, string(f, i == 1 || i == 2 ? "" : "*"));
            pointer(element, 0x10, string(f, "element")); number(element, 0x1c, 8);
        }
    }
    {
        const char *t[] = {"idDeclEntityDef", "OpaqueStringReader", "idDeclActorModifier", "idDeclSnapEditorEntity", "idRenderModel", "idImage"};
        const char *o[] = {"*", "", "*", "*", "*", "*"}, *n[] = {"target", "custom", "modifier", "preview", "model", "image"};
        fields(f, (char *)records + 0x38, 6, t, o, n);
    }
    {
        const char *t[] = {"idDeclEntityDef"}, *o[] = {"*"}, *n[] = {"target"};
        fields(f, (char *)records + 4 * 0x38, 1, t, o, n);
    }
    {
        const char *t[] = {"idDeclEntityDef", "idDeclEntityDef", "idDeclEntityDef", "idList < EditorSheet >",
            "idList < EntityRefs >", "idList < EntityRefs >"};
        const char *o[] = {"*", "*", "*", "", "", ""}, *n[] = {"target", "first", "second", "propertySheets", "inputs", "outputs"};
        fields(f, (char *)records + 6 * 0x38, 6, t, o, n);
    }
    {
        const char *t[] = {"idStr", "idStr", "idStr", "idDeclSnapPropertyInspector_WhiteListEncounterDecl", "idDeclEntityDef", "idStr"};
        const char *o[] = {"", "", "", "*", "*", ""}, *n[] = {"path", "displayNameTag", "descriptionTag", "inspector", "action", "type"};
        fields(f, (char *)records + 12 * 0x38, 6, t, o, n);
        const char *st[] = {"idList < EditorProperty >"}, *so[] = {""}, *sn[] = {"properties"};
        fields(f, (char *)records + 13 * 0x38, 1, st, so, sn);
        const char *it[] = {"idList < Encounters >", "idStr"}, *io[] = {"", ""}, *in[] = {"validEncounters", "mode"};
        fields(f, (char *)records + 16 * 0x38, 2, it, io, in);
        const char *at[] = {"idDeclEntityDef"}, *ao[] = {"*"}, *an[] = {"alternate"};
        fields(f, (char *)records + 18 * 0x38, 1, at, ao, an);
    }
}
/* A minimal but complete cooked md6mesh: one skeleton, one mesh whose material
 * is a declaration this fixture can serve, no geometry and no material records. */
static size_t cooked_mesh(unsigned char *out, const char *skeleton, const char *material)
{
    static const unsigned char magic[4] = {0x2b, 0x02, 0x4d, 0x4d};
    size_t at = 0;
    #define PUT(bytes, count) do { memcpy(out + at, (bytes), (count)); at += (count); } while (0)
    #define ZERO(count) do { memset(out + at, 0, (count)); at += (count); } while (0)
    #define NAME(text) do { size_t n = strlen(text); out[at]=(unsigned char)n; out[at+1]=(unsigned char)(n>>8); \
        out[at+2]=0; out[at+3]=0; at += 4; memcpy(out + at, (text), n); at += n; } while (0)
    #define BE32(value) do { unsigned v=(value); out[at]=(unsigned char)(v>>24); out[at+1]=(unsigned char)(v>>16); \
        out[at+2]=(unsigned char)(v>>8); out[at+3]=(unsigned char)v; at += 4; } while (0)
    PUT(magic, 4); ZERO(8); NAME(skeleton); ZERO(24); ZERO(1); NAME(""); ZERO(2); ZERO(24);
    BE32(0); ZERO(36);
    BE32(1); NAME("body"); NAME(material); ZERO(1); BE32(1); BE32(0); BE32(0); ZERO(24);
    BE32(0); BE32(0); BE32(0); ZERO(1); BE32(0);
    BE32(0); PUT(magic, 4);
    #undef PUT
    #undef ZERO
    #undef NAME
    #undef BE32
    return at;
}

/* A cooked animation: the md6 header family, then the skeleton it plays on. */
static size_t cooked_anim(unsigned char *out, const char *skeleton)
{
    size_t n = strlen(skeleton);
    memcpy(out, "\x26\x02\x41\x4d", 4);
    memset(out + 4, 0, 8);
    out[12] = (unsigned char)n; out[13] = (unsigned char)(n >> 8); out[14] = 0; out[15] = 0;
    memcpy(out + 16, skeleton, n);
    memset(out + 16 + n, 0, 32);
    return 16 + n + 32;
}

static int original(void *context, const char *path, unsigned char **body, size_t *length)
{
    fixture *f = context;
    const char *text = NULL;
    char generated[160];
    unsigned index;
    *body = NULL; *length = 0;
    if (!strncmp(path, "cooked/anim/md6/boss/motion/", 28) ||
        !strcmp(path, "cooked/anim/md6/boss/boss.md6skl/_default.bmd6anim")) {
        unsigned char cooked[256];
        /* The clip names its skeleton; the per-skeleton default clip does not. */
        size_t size = cooked_anim(cooked, strstr(path, "_default") ? "_defaulted_" : "md6/boss/boss.md6skl");
        *body = malloc(size);
        if (!*body) return -1;
        memcpy(*body, cooked, size); *length = size; return 2;
    }
    if (!strcmp(path, "generated/basemodel/md6/boss/boss.bmd6model")) {
        unsigned char cooked[512];
        size_t size = cooked_mesh(cooked, "md6/boss/boss.md6skl", "boss");
        *body = malloc(size);
        if (!*body) return -1;
        memcpy(*body, cooked, size); *length = size; return 2;
    }
    if (f->unreadable && !strcmp(path, f->unreadable)) return -1;
    if (!strcmp(path, "generated/decls/material/meshbound.decl")) {
        /* The one material here that SnapMap itself ships. */
        *length = 3; *body = (unsigned char *)_strdup("{ }"); return *body ? 1 : -1;
    }
    if (!strcmp(path, "generated/skeleton/md6/present/present.bmd6skl")) {
        *length = 8; *body = (unsigned char *)_strdup("skeleton"); return *body ? 1 : -1;
    }
    if (!strcmp(path, "generated/decls/renderprog/snapprog.decl")) {
        /* The one program here that SnapMap itself ships. */
        *length = 3; *body = (unsigned char *)_strdup("{ }"); return *body ? 1 : -1;
    }
    if (!strcmp(path, "generated/image/image/snap.bimage")) {
        /* The one image here that SnapMap itself ships. */
        *length = 10; *body = (unsigned char *)_strdup("snap image"); return *body ? 1 : -1;
    }
    if (!strcmp(path, "generated/image/image/body.bimage") ||
        !strcmp(path, "generated/image/models/boss/body.bimage") ||
        !strcmp(path, "generated/image/textures/default/body.bimage")) text = "original image payload";
    else if (!strcmp(path, "generated/decls/entitydef/base.decl")) text = "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }";
    else if (!strcmp(path, "generated/decls/entitydef/map-child.decl")) text = "{ class = \"Entity\"; edit = { target = \"root\"; } }";
    else if (!strcmp(path, "generated/decls/entitydef/parent-only.decl") ||
             !strcmp(path, "generated/decls/entitydef/modifier-child.decl") ||
             !strcmp(path, "generated/decls/entitydef/local-only.decl")) text = "{ class = \"Entity\"; edit = {} }";
    else if (!strcmp(path, "generated/decls/actormodifier/mod.decl")) text = "{ edit = { target = \"modifier-child\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/preview.decl")) text = "{ edit = { target = \"editor-child\"; } }";
    else if (!strcmp(path, "generated/decls/renderparm/color.decl")) text = "{ Vec 0 }";
    else if (!strcmp(path, "generated/decls/renderparm/tex.decl")) text = "{ Tex2D textures/default/body }";
    else if (!strcmp(path, "generated/decls/renderparm/label.decl")) text = "{ String }";
    else if (!strcmp(path, "generated/decls/renderparm/transsortmap.decl") ||
             !strcmp(path, "generated/decls/renderparm/transatlasmap.decl")) text = "{ String default/path }";
    else if (!strcmp(path, "generated/transsortatlas/textures/atlas/beam.bimage") ||
             !strcmp(path, "generated/image/textures/atlas/beam.bimage")) {
        text = "atlas fixture bytes";
        *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
    }
    else if (!strcmp(path, "generated/decls/renderparm/stageprogram.decl")) text = "{ Program }";
    else if (!strcmp(path, "generated/decls/renderparm/landpagefile.decl")) text = "{ String }";
    else if (!strcmp(path, "generated/decls/renderparm/buffer.decl")) text = "{ StructuredBuffer writable_cs boss_t }";
    else if (!strcmp(path, "generated/decls/renderparm/boss_t.decl")) text = "{ struct { uint offset; } }";
    else if (!strcmp(path, "generated/decls/table/curve.decl")) text = "{ {0,1} }";
    else if (!strcmp(path, "generated/decls/md6def/mesh-only.decl"))
        text = "{ init { mesh \"md6/boss/boss.md6mesh\" } }";
    else if (!strcmp(path, "generated/decls/md6def/boss.decl"))
        /* The animation event carries one declaration-typed argument, one string
         * and one enum constant: only the first names a resource. */
        text = "{ init { inherit \"demons/base\" mesh \"md6/boss/absent.md6mesh\" calcRefBoundsFromJoints 1 }"
               " events { anim \"md6/boss/motion/idle.md6anim\" { event \"ae_hit\" {\n"
               "frame 1\nrow 0\nlocked 0\nmaterial \"boss\"\nstring \"boss\"\n"
               "fxCondition_t FX_WEAPON_START_FIRE_QUAD\n} } }"
               " aliases { alias { name \"idle\" anim \"md6/boss/motion/idle.md6anim\" }"
               " alias { name \"attack\" anim \"md6/boss/motion/attack.md6anim\" } } }";
    else if (!strcmp(path, "generated/decls/md6def/demons/base.decl"))
        text = "{ init { mesh \"md6/base/base.md6mesh\" } }";
    else if (!strcmp(path, "generated/decls/md6def/broken.decl")) text = "{ init { unknownField 1 } }";
    else if (!strcmp(path, "generated/decls/material/meshcampaign.decl")) text = "{ }";
    else if (!strcmp(path, "generated/decls/md6def/default-clip.decl"))
        text = "{ init { mesh \"md6/absent.md6mesh\" }"
               " aliases { alias { name \"idle\" anim \"md6/boss/boss.md6skl/_default.md6anim\" } } }";
    else if (!strcmp(path, "generated/decls/renderprog/glow.decl"))
        text = "{ hlsl_vp { #include \"includes/vertex.inc\"\n#include \"global.inc\" } }";
    else if (sscanf(path, "generated/decls/entitydef/chain/%u.decl", &index) == 1 && index < 1024) {
        if (index < 1023) snprintf(generated, sizeof(generated), "{ class = \"Entity\"; edit = { target = \"chain/%u\"; } }", index + 1);
        else strcpy(generated, "{ class = \"Entity\"; edit = {} }");
        text = generated;
    }
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 2 : -1;
}
static int has(const sh_package_references *paths, const char *name)
{
    for (size_t i = 0; i < paths->count; i++) if (strstr(paths->items[i].name, name)) return 1;
    return 0;
}
static unsigned observed;
typedef struct atlas_requests { unsigned atlas, image, other; int reject; } atlas_requests;
static int atlas_request(void *context, const char *slot, const char *family, const char *name)
{
    atlas_requests *requests = context;
    assert(!strncmp(slot, "request/", 8));
    if (!strcmp(name, "textures/atlas/beam")) {
        if (!strcmp(family, "transsortatlas")) requests->atlas++;
        else { assert(!strcmp(family, "image")); requests->image++; }
    } else requests->other++;
    return !requests->reject;
}
typedef struct bind_fixture { char output[1024]; size_t count; int replace; } bind_fixture;
static int bind_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    bind_fixture *f = context;
    assert(slot && *slot && family && *family && name && *name); f->count++;
    if (!f->replace) return 0;
    snprintf(f->output, sizeof(f->output), "bound/%s", name); *replacement = f->output; return 1;
}
typedef struct derived_admission { size_t calls; int result, exists; } derived_admission;
static int admit_derived(void *context, const char *family, const char *name, int *exists)
{
    derived_admission *f = context; f->calls++;
    if (strcmp(name, "bound-test-token")) return 0;
    *exists = f->exists && !strcmp(family, "entitydef"); return f->result;
}
static int map_saved_binding(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    (void)context; (void)slot; (void)family;
    if (strncmp(name, "saved/", 6)) return 0;
    *replacement = name + 6; return 1;
}
static void test_bind(fixture *f, sh_package_compilation *compiled, sh_decl_registry_source registry)
{
    const char *entity = "{ inherit = \"base\"; editorVars { label = \"map-child\"; } edit = { target = \"map-child\"; model = \"effects/fire.prt\"; image = \"image/body\"; } }";
    sh_package_source_reader *reader;
    sh_package_source_binding result;
    bind_fixture bindings = {0};
    char error[1024]; unsigned char *out; size_t length;
    reader = sh_package_source_reader_open(compiled, NULL, original, f, registry,
        (uintptr_t)f->reflection, error, sizeof(error)); assert(reader);
    {
        const char *text = "{\n transSortMap textures/absent/old.tga\n"
            " transSortMap \"Textures\\Atlas\\Beam.TGA\"\n label textures/not/a/resource.tga\n }";
        atlas_requests requests = {0};
        assert(sh_package_source_material_requests(reader, (sh_decl_source){text, strlen(text)},
            0, atlas_request, &requests, error, sizeof(error)));
        assert(requests.atlas == 1 && requests.image == 1 && !requests.other);
        text = "{ transAtlasMap textures/absent/beam.tga\n }";
        requests = (atlas_requests){0};
        assert(sh_package_source_material_requests(reader, (sh_decl_source){text, strlen(text)},
            0, atlas_request, &requests, error, sizeof(error)) && !requests.other);
        assert(sh_package_source_material_requests(reader, (sh_decl_source){text, strlen(text)},
            1, atlas_request, &requests, error, sizeof(error)) && requests.other == 2);
        text = "{ label arbitrary path.tga\n }";
        requests = (atlas_requests){0};
        assert(sh_package_source_material_requests(reader, (sh_decl_source){text, strlen(text)},
            1, atlas_request, &requests, error, sizeof(error)) && !requests.other && !requests.atlas && !requests.image);
        text = "{ transAtlasMap textures/atlas/beam.tga\n }";
        requests.reject = 1;
        assert(!sh_package_source_material_requests(reader, (sh_decl_source){text, strlen(text)},
            0, atlas_request, &requests, error, sizeof(error)));
    }
    {
        const char *repeated = "{ class = \"Entity\"; \"Entity\" \"\" edit = {"
            " target = \"map-child\"; target = \"root\"; } }";
        out = sh_package_source_bind(reader, "entitydef", "repeated", (const unsigned char *)repeated,
            strlen(repeated), bind_reference, &bindings, &length, &result, error, sizeof(error));
        if (!out || result.references != 2) fprintf(stderr, "repeated source: refs=%zu error=%s\n", result.references, error);
        assert(out && length == strlen(repeated) && !memcmp(out, repeated, length) && result.references == 2);
        free(out); bindings.replace = 1;
        out = sh_package_source_bind(reader, "entitydef", "repeated", (const unsigned char *)repeated,
            strlen(repeated), bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && strstr((char *)out, "\"Entity\" \"\" edit") && result.changed == 2);
        assert(strstr((char *)out, "\"bound/map-child\"") < strstr((char *)out, "\"bound/root\""));
        free(out); bindings = (bind_fixture){0};
    }
    {
        const char *absent = "{ class = \"Entity\"; edit = { target = \"absent/target\"; } }";
        out = sh_package_source_observe(reader, "entitydef", "observer", (const unsigned char *)absent,
            strlen(absent), bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && length == strlen(absent) && !memcmp(out, absent, length) && result.references == 1);
        free(out);
        /* Strict binding keeps the engine's default for an absent declaration:
         * the authored bytes stay unchanged and no binding edge is requested. */
        size_t requested = bindings.count;
        out = sh_package_source_bind(reader, "entitydef", "observer", (const unsigned char *)absent,
            strlen(absent), bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && length == strlen(absent) && !memcmp(out, absent, length));
        assert(bindings.count == requested && !result.references); free(out);
        bindings = (bind_fixture){0};
    }
    {
        const char *map = "{ \"note\":\"saved/map-child\x97\", \"actor\":{\"targetType\":\"idDeclEntityDef\","
            "\"inherit\":\"saved/base\",\"state\":{\"edit\":{\"target\":\"saved/map-child\","
            "\"model\":\"saved/effects/fire.prt\",\"image\":\"saved/image/body\"}}},"
            "\"editorVars\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"saved/base\"}}";
        const char *expected = "{ \"note\":\"saved/map-child\x97\", \"actor\":{\"targetType\":\"idDeclEntityDef\","
            "\"inherit\":\"base\",\"state\":{\"edit\":{\"target\":\"map-child\","
            "\"model\":\"effects/fire.prt\",\"image\":\"image/body\"}}},"
            "\"editorVars\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"saved/base\"}}";
        char *bound = sh_package_source_bind_map(reader, map, strlen(map), map_saved_binding, NULL,
            &length, &result, error, sizeof(error));
        if (!bound) fprintf(stderr, "map binding: %s\n", error);
        assert(bound && !strcmp(bound, expected) && result.changed == 4 && !result.inspection.gaps); free(bound);
        bound = sh_package_source_bind_map(reader, expected, strlen(expected), map_saved_binding, NULL,
            &length, &result, error, sizeof(error));
        assert(bound && !memcmp(bound, expected, length + 1) && !result.changed); free(bound);
        f->unreadable = "generated/decls/entitydef/base.decl";
        bound = sh_package_source_bind_map(reader, map, strlen(map), map_saved_binding, NULL,
            &length, &result, error, sizeof(error));
        assert(!bound && !length && !result.changed); f->unreadable = NULL;
    }
    {
        /* A map root with no native identity refuses the map and names it. */
        const char *map = "{\"actor\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"absent/boss\"}}";
        char *bound = sh_package_source_bind_map(reader, map, strlen(map), map_saved_binding, NULL,
            &length, &result, error, sizeof(error));
        if (bound || !strstr(error, "map/root")) fprintf(stderr, "absent map root: %s\n", error);
        assert(!bound && result.inspection.gaps && strstr(error, "map/root") &&
            strstr(error, "resolved for \"absent/boss\"") && !strcmp(error + strlen(error) - 13, "\"absent/boss\""));
    }
    out = sh_package_source_bind(reader, "entitydef", "example", (const unsigned char *)entity,
        strlen(entity), bind_reference, &bindings, &length, &result, error, sizeof(error));
    if (!out) fprintf(stderr, "%s\n", error);
    assert(out && length == strlen(entity) && !memcmp(out, entity, length));
    assert(result.references == 4 && !result.changed && bindings.count == 4); free(out);
    bindings.replace = 1; bindings.count = 0;
    out = sh_package_source_bind(reader, "entitydef", "example", (const unsigned char *)entity,
        strlen(entity), bind_reference, &bindings, &length, &result, error, sizeof(error));
    assert(out && result.references == 4 && result.changed == 4 && bindings.count == 4);
    assert(strstr((char *)out, "bound/base") && strstr((char *)out, "bound/map-child") &&
        strstr((char *)out, "bound/effects/fire.prt") && strstr((char *)out, "bound/image/body") &&
        strstr((char *)out, "label = \"map-child\"")); free(out);
    {
        const char *bad = "{ class = \"Entity\"; edit = { target = \"map-child\"; custom = \"late\"; } }";
        assert(!sh_package_source_bind(reader, "entitydef", "example", (const unsigned char *)bad,
            strlen(bad), bind_reference, &bindings, &length, &result, error, sizeof(error)));
        assert(!length && !result.changed && result.inspection.gaps);
    }
    {
        const char *material = "{ color curve[color] tex image/snap label image/body\n}";
        out = sh_package_source_bind(reader, "material", "example", (const unsigned char *)material,
            strlen(material), bind_reference, &bindings, &length, &result, error, sizeof(error));
        /* The expression also names color as its input render parameter. */
        if (!out) fprintf(stderr, "material binding: %s\n", error);
        assert(out && result.references == 6 && result.changed == 6);
        assert(strstr((char *)out, "bound/tex bound/image/snap") && strstr((char *)out, "bound/label image/body")); free(out);
        /* An image only the campaign carries, or none at all, is materialized by
         * name with makeDefault: the parameter keeps it with no binding edge. */
        const char *defaulted[] = {"{ color curve[color] tex image/body label image/body\n}",
            "{ color curve[color] tex image/absent label image/body\n}",
            "{ color curve[color] tex _viewDepth label image/body\n}"};
        const char *kept[] = {"bound/tex image/body", "bound/tex image/absent", "bound/tex _viewDepth"};
        for (size_t i = 0; i < 3; i++) {
            size_t before = bindings.count;
            out = sh_package_source_bind(reader, "material", "example", (const unsigned char *)defaulted[i],
                strlen(defaulted[i]), bind_reference, &bindings, &length, &result, error, sizeof(error));
            if (!out) fprintf(stderr, "defaulted material image: %s\n", error);
            assert(out && result.references == 6 && result.changed == 5 && bindings.count == before + 5);
            assert(strstr((char *)out, kept[i])); free(out);
        }
        /* A parm block finds a program strictly, but an unknown one only ends
         * the block and the engine keeps the ill-formed material. A program
         * neither this scope nor SnapMap has stays by name with no edge. */
        const char *programs[] = {"{ stageProgram snapprog\n}", "{ stageProgram absentprog\n}",
            "{ stageProgram glow\n}"};
        const char *named[] = {"bound/stageprogram bound/snapprog", "bound/stageprogram absentprog",
            "bound/stageprogram glow"};
        for (size_t i = 0; i < 3; i++) {
            size_t before = bindings.count;
            out = sh_package_source_bind(reader, "material", "example", (const unsigned char *)programs[i],
                strlen(programs[i]), bind_reference, &bindings, &length, &result, error, sizeof(error));
            if (!out) fprintf(stderr, "material program: %s\n", error);
            assert(out && result.references == 2 && result.changed == (i ? 1u : 2u) &&
                bindings.count == before + (i ? 1u : 2u));
            assert(strstr((char *)out, named[i])); free(out);
        }
    }
    {
        const char *table = "{ clamp { -1:0, 2:1 } }";
        out = sh_package_source_bind(reader, "table", "example", (const unsigned char *)table,
            strlen(table), bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && !result.references && !result.changed && !result.inspection.gaps &&
            length == strlen(table) && !memcmp(out, table, length)); free(out);
        table = "{ image \"unverified/resource\" }";
        assert(!sh_package_source_bind(reader, "table", "example", (const unsigned char *)table,
            strlen(table), bind_reference, &bindings, &length, &result, error, sizeof(error)));
        assert(!length && !result.changed && error[0]);
    }
    {
        const char *af = "{ settings { model \"robot\" skin \"\" clipMaterial \"body\" } "
            "body \"robot\" { joint \"body\" model custom(\"shapes/body\",(0,0,0),(1,1,1)) } }";
        out = sh_package_source_bind(reader, "articulatedfigure", "example", (const unsigned char *)af,
            strlen(af), bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && result.references == 3 && result.changed == 3 && !result.inspection.gaps);
        assert(strstr((char *)out, "model \"bound/robot\"") &&
            strstr((char *)out, "clipMaterial \"bound/body\"") &&
            strstr((char *)out, "custom(\"bound/shapes/body\"") &&
            strstr((char *)out, "body \"robot\" { joint \"body\"")); free(out);
        af = "{ settings { model \"robot\" skin \"unverified/skin\" } }";
        assert(!sh_package_source_bind(reader, "articulatedfigure", "example", (const unsigned char *)af,
            strlen(af), bind_reference, &bindings, &length, &result, error, sizeof(error)));
        assert(!length && !result.changed && strstr(error, "skin"));
    }
    {
        /* A cloth names its material and the md6 definition its data drapes;
         * joint entries, counts and hit-test groups stay local. */
        const char *cloth = "{\tmass\t\t1.0\tgravity\t-10000.0\tfriction\t1\ttype\tgeneric\tmaterial\tsurfaces/cloth\n"
            "\tclothData { zion/monsters/archvile.md6\t\t40\t\tleftwing1\t{ anchored }\n"
            "\t\tleftwing2\t{ physicsInfluence 0.125\tforward\t2\tright 11\t} }\n\thitTestGroups { group1 { a b } } }";
        out = sh_package_source_bind(reader, "cloth", "example", (const unsigned char *)cloth, strlen(cloth),
            bind_reference, &bindings, &length, &result, error, sizeof(error));
        if (!out) fprintf(stderr, "cloth binding: %s\n", error);
        assert(out && result.references == 2 && result.changed == 2 &&
            strstr((char *)out, "material\tbound/surfaces/cloth") &&
            strstr((char *)out, "clothData { bound/zion/monsters/archvile.md6")); free(out);
        const char *none = "{ type rope clothData { none } }";
        out = sh_package_source_bind(reader, "cloth", "example", (const unsigned char *)none, strlen(none),
            bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && !result.references); free(out);
        const char *unknown = "{ clothDeclFX fx/sparks }";
        assert(!sh_package_source_bind(reader, "cloth", "example", (const unsigned char *)unknown, strlen(unknown),
            bind_reference, &bindings, &length, &result, error, sizeof(error)));
    }
    {
        /* An environment names its base env and a render parameter block read
         * like a material's, including an image materialized by name. */
        const char *env = "{\n\tinherit {\n\t\tsnapbase\n\t}\n\trenderParms {\n\t\ttex\t\t\timage/snap\n"
            "\t\tlabel\t\t\timage/body\n\t\ttex\t\t\timage/absent\n\t}\n\tallowOverride 1\n}";
        size_t before = bindings.count;
        out = sh_package_source_bind(reader, "env", "example", (const unsigned char *)env, strlen(env),
            bind_reference, &bindings, &length, &result, error, sizeof(error));
        if (!out) fprintf(stderr, "environment binding: %s\n", error);
        if (out && (result.references != 6 || result.changed != 5))
            fprintf(stderr, "environment references %zu changed %zu\n", result.references, result.changed);
        assert(out && result.references == 6 && result.changed == 5 && bindings.count == before + 5);
        assert(strstr((char *)out, "bound/snapbase") && strstr((char *)out, "bound/tex\t\t\tbound/image/snap") &&
            strstr((char *)out, "bound/tex\t\t\timage/absent")); free(out);
    }
    {
        /* A clip whose skeleton exists binds it; one whose skeleton no archive
         * holds fails its native skeleton check and loads defaulted, so the
         * name stays with no edge. */
        const char *skeletons[] = {"md6/present/present.md6skl", "md6/absent/absent.md6skl"};
        for (size_t i = 0; i < 2; i++) {
            unsigned char clip[256]; size_t clip_length = cooked_anim(clip, skeletons[i]);
            size_t before = bindings.count;
            out = sh_package_source_bind(reader, "anim", "md6/clip.md6anim", clip, clip_length,
                bind_reference, &bindings, &length, &result, error, sizeof(error));
            if (!out) fprintf(stderr, "clip skeleton %s: %s\n", skeletons[i], error);
            assert(out && result.references == 1 && result.changed == (i ? 0u : 1u) &&
                bindings.count == before + (i ? 0u : 1u)); free(out);
        }
    }
    {
        /* Vector-field grids and transparency atlas entries name no resource:
         * their bytes pass through unchanged with no references. */
        const char *leaves[][2] = {{"fga", "2,2,2,-1,-1,-1,1,1,1,0.5,0,0"}, {"transsortatlas", "cooked atlas bytes"}};
        for (size_t i = 0; i < 2; i++) {
            size_t n = strlen(leaves[i][1]);
            out = sh_package_source_bind(reader, leaves[i][0], "leaf", (const unsigned char *)leaves[i][1], n,
                bind_reference, &bindings, &length, &result, error, sizeof(error));
            if (!out) fprintf(stderr, "%s leaf: %s\n", leaves[i][0], error);
            assert(out && length == n && !memcmp(out, leaves[i][1], n) && !result.references && !result.changed); free(out);
        }
    }
    {
        /* Alias clips only the campaign carries, or that no archive holds,
         * load by name with makeDefault: the md6def keeps them, no edges. */
        /* A declaration-typed event argument no scope or SnapMap source holds is
         * found by ReadEventCallInfo with makeDefault: no edge either. */
        const char *md6 = "{ init { mesh \"md6/boss/boss.md6mesh\" } events {"
            " anim \"md6/boss/motion/idle.md6anim\" { event \"ae_giveItem\" {\nframe 1\nrow 0\nlocked 0\n"
            "inventoryItem \"weapon/absent\"\n} } } aliases {"
            " alias { name \"idle\" anim \"md6/boss/motion/idle.md6anim\" }"
            " alias { name \"gone\" anim \"md6/boss/motion/gone.md6anim\" } } }";
        size_t before = bindings.count;
        out = sh_package_source_bind(reader, "md6def", "example", (const unsigned char *)md6, strlen(md6),
            bind_reference, &bindings, &length, &result, error, sizeof(error));
        if (!out) fprintf(stderr, "defaulted md6def clips: %s\n", error);
        if (out && result.references != 4) fprintf(stderr, "md6def references: %zu\n", result.references);
        assert(out && result.references == 4 && result.changed == 1 && bindings.count == before + 1 &&
            strstr((char *)out, "inventoryItem \"weapon/absent\""));
        assert(strstr((char *)out, "bound/md6/boss/boss.md6mesh") &&
            strstr((char *)out, "anim \"md6/boss/motion/idle.md6anim\"") &&
            strstr((char *)out, "anim \"md6/boss/motion/gone.md6anim\"")); free(out);
    }
    {
        /* A surface material SnapMap ships binds. One only the campaign
         * archive carries, or none at all, is the loader's default object:
         * the mesh keeps its name and only the skeleton reaches the resolver. */
        unsigned char mesh[512]; size_t mesh_length = cooked_mesh(mesh, "md6/body.md6skl", "meshbound");
        out = sh_package_source_bind(reader, "basemodel", "md6/body.md6mesh", mesh, mesh_length,
            bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && result.references == 2 && result.changed == 2 && length == mesh_length + 12); free(out);
        const char *defaulted[] = {"boss", "meshcampaign"};
        for (size_t i = 0; i < 2; i++) {
            size_t before = bindings.count;
            mesh_length = cooked_mesh(mesh, "md6/body.md6skl", defaulted[i]);
            out = sh_package_source_bind(reader, "basemodel", "md6/body.md6mesh", mesh, mesh_length,
                bind_reference, &bindings, &length, &result, error, sizeof(error));
            if (!out) fprintf(stderr, "defaulted mesh material %s: %s\n", defaulted[i], error);
            assert(out && result.references == 2 && result.changed == 1 && length == mesh_length + 6);
            int kept = 0; size_t n = strlen(defaulted[i]);
            for (size_t at = 0; !kept && at + n <= length; at++) kept = !memcmp(out + at, defaulted[i], n);
            assert(bindings.count == before + 1 && kept);
            free(out);
        }
    }
    {
        const char *breakable = "{ model models\\part.lwo pieceNames {1 \"models\\part.lwo\"} "
            "gravity 0 0 -750 explosion {name fx/hit fx fx/hit} trail {particleDie effects/dust} "
            "decal {materials {surfaces/mark} ceilingParticles {effects/drip}} }";
        bindings = (bind_fixture){0};
        out = sh_package_source_observe(reader, "breakable", "part", (const unsigned char *)breakable,
            strlen(breakable), bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && result.references == 5 && !result.changed && !result.inspection.gaps &&
            length == strlen(breakable) && !memcmp(out, breakable, length)); free(out);
        bindings = (bind_fixture){0}; bindings.replace = 1;
        out = sh_package_source_bind(reader, "breakable", "part", (const unsigned char *)breakable,
            strlen(breakable), bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && result.references == 5 && result.changed == 5 && bindings.count == 5);
        assert(strstr((char *)out, "model bound/models/part.lwo"));
        assert(strstr((char *)out, "pieceNames {1 \"models\\part.lwo\"}"));
        assert(strstr((char *)out, "name fx/hit fx bound/fx/hit"));
        assert(strstr((char *)out, "materials {bound/surfaces/mark}")); free(out);
        breakable = "{ model models/part.lwo explosion {foreignLookup hidden/asset} }";
        assert(!sh_package_source_bind(reader, "breakable", "part", (const unsigned char *)breakable,
            strlen(breakable), bind_reference, &bindings, &length, &result, error, sizeof(error)));
        assert(!length && !result.changed && error[0]);
        /* NULL is the stock sentinel for no effect or particle. */
        breakable = "{ model models/part.lwo explosion {fx NULL} trail {particleTrail NULL particleBounce NULL} }";
        bindings = (bind_fixture){0}; bindings.replace = 1;
        out = sh_package_source_bind(reader, "breakable", "part", (const unsigned char *)breakable,
            strlen(breakable), bind_reference, &bindings, &length, &result, error, sizeof(error));
        if (!out) fprintf(stderr, "NULL breakable: %s\n", error);
        assert(out && result.references == 1 && bindings.count == 1); free(out);
    }
    {
        const char *state = "{ edit = { target = \"map-child\"; } }";
        out = sh_package_source_bind(reader, "actormodifier", "example", (const unsigned char *)state,
            strlen(state), bind_reference, &bindings, &length, &result, error, sizeof(error));
        assert(out && result.references == 1 && result.changed == 1 && strstr((char *)out, "bound/map-child")); free(out);
    }
    sh_package_source_reader_close(reader);
}
static int script_lookup(void *context, const char *name, uintptr_t *table)
{
    fixture *f = context;
    *table = !strcmp(name, "Script") ? f->script_table : 0;
    return *table ? 1 : 0;
}
static void test_script_bind(fixture *f, sh_decl_registry_source registry)
{
    void *container = *(void **)f->reflection, *old_records = *(void **)((char *)container + 0x20);
    void *records = alloc(f, 22 * 0x38), *objects = alloc(f, 22 * 16), *pointers = alloc(f, 22 * 16);
    memcpy(records, old_records, 19 * 0x38);
    memcpy(objects, *(void **)((char *)f->reflection + 0x88), 19 * 16);
    memcpy(pointers, *(void **)((char *)f->reflection + 0xa0), 19 * 16);
    pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, 22);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, 22);
    pointer(records, 19 * 0x38, string(f, "idDecl")); pointer(records, 19 * 0x38 + 8, string(f, ""));
    pointer(records, 20 * 0x38, string(f, "idSSObject")); pointer(records, 20 * 0x38 + 8, string(f, ""));
    pointer(records, 8, string(f, "idDecl")); pointer(records, 3 * 0x38 + 8, string(f, "idDecl"));
    pointer(pointers, 20 * 16 + 8, (void *)0x90);
    {
        const char *t[] = {"idDeclEntityDef", "OpaqueStringReader", "idDeclActorModifier", "idDeclSnapEditorEntity", "idRenderModel", "idImage", "idSSObject"};
        const char *o[] = {"*", "", "*", "*", "*", "*", "*"}, *n[] = {"target", "custom", "modifier", "preview", "model", "image", "script"};
        fields(f, (char *)records + 0x38, 7, t, o, n);
    }
    const char *body = "{ class = \"Entity\"; edit = { script = { object = \"Script\";"
        " target = \"map-child\"; note = \"map-child\"; } } }";
    const char *map = "{\"actor\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"base\","
        "\"state\":{\"edit\":{\"script\":{\"object\":\"Script\",\"target\":\"saved/map-child\",\"note\":\"saved/map-child\"}}}}}";
    uintptr_t *names = alloc(f, 16);
    sh_decl_script_schema *scripts;
    sh_package_compilation compiled = {0};
    sh_package_source_reader *reader;
    sh_package_source_binding result;
    bind_fixture bindings = {0};
    char error[512], *bound;
    unsigned char *out;
    size_t length;
    f->script_table = (uintptr_t)alloc(f, 48);
    names[0] = (uintptr_t)string(f, "Script");
    pointer((void *)f->script_table, 0, string(f, "idDeclEntityDef"));
    pointer((void *)f->script_table, 8, string(f, ""));
    pointer((void *)f->script_table, 16, string(f, "target"));
    scripts = sh_decl_script_schema_capture((sh_decl_script_source){f, read_memory,
        (uintptr_t)names, script_lookup}, error, sizeof(error));
    assert(scripts); registry.scripts = scripts;
    reader = sh_package_source_reader_open(&compiled, NULL, original, f, registry,
        (uintptr_t)f->reflection, error, sizeof(error)); assert(reader);
    out = sh_package_source_bind(reader, "entitydef", "scripted", (const unsigned char *)body,
        strlen(body), bind_reference, &bindings, &length, &result, error, sizeof(error));
    if (!out) fprintf(stderr, "script binding: %s\n", error);
    assert(out && length == strlen(body) && !memcmp(out, body, length) && result.references == 1);
    free(out); bindings.replace = 1;
    out = sh_package_source_bind(reader, "entitydef", "scripted", (const unsigned char *)body,
        strlen(body), bind_reference, &bindings, &length, &result, error, sizeof(error));
    assert(out && result.changed == 1 && strstr((char *)out, "target = \"bound/map-child\"") &&
        strstr((char *)out, "note = \"map-child\"") && strstr((char *)out, "object = \"Script\""));
    free(out);
    bound = sh_package_source_bind_map(reader, map, strlen(map), map_saved_binding, NULL,
        &length, &result, error, sizeof(error));
    if (!bound) fprintf(stderr, "script map binding: %s\n", error);
    assert(bound && result.changed == 1 && strstr(bound, "\"target\":\"map-child\"") &&
        strstr(bound, "\"note\":\"saved/map-child\""));
    free(bound); sh_package_source_reader_close(reader); sh_decl_script_schema_free(scripts);
}
static int observed_edge(void *context, const char *pt, const char *pn, const char *type, const char *name)
{ (void)context; (void)pt; (void)pn; (void)type; if (!strcmp(name, "local-only")) observed++; return 1; }
typedef struct source_link_fixture {
    fixture *native;
    const sh_package_link_inputs *inputs;
    size_t reads, admissions;
    int campaign;
    const char *image_path;
    size_t certifications;
    int certificate, different_semantics;
} source_link_fixture;
static int link_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    source_link_fixture *f = context;
    f->reads++;
    if (f->image_path && !strcmp(path, f->image_path)) {
        const char *image = "synthetic image bytes";
        *length = strlen(image); *body = (unsigned char *)_strdup(image);
        return *body ? 1 : -1;
    }
    int status = original(f->native, path, body, length);
    return status == 2 && !f->campaign ? 1 : status;
}
static int link_original_binding(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity);
/* Discovery records each captured file's reference sites so the first
 * identity capture need not walk it again. For every such file they must be
 * exactly what bind_file reports: the same slots, in order, resolved to the
 * same frozen units. */
typedef struct site_record {
    sh_package_source_link *source;
    size_t unit, seen, count;
    const sh_package_link_edge *expected;
    int mismatch;
} site_record;
static int site_reference(void *context, const char *slot, const char *family, const char *name,
    const char **replacement)
{
    site_record *r = context; size_t target = SIZE_MAX; char error[256];
    *replacement = NULL;
    if (!sh_package_source_link_resolve_unit(r->source, r->unit, family, name, &target, error, sizeof(error))) {
        r->mismatch = 1; return -1;
    }
    if (r->seen >= r->count || strcmp(r->expected[r->seen].slot, slot) || r->expected[r->seen].target != target)
        r->mismatch = 1;
    r->seen++; return 0;
}
static void assert_discovered_sites(sh_package_source_link *source)
{
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(source);
    size_t checked = 0;
    for (size_t u = 0; u < inputs->count; u++) for (size_t f = 0; f < inputs->units[u].file_count; f++) {
        const sh_compiled_resource *file = inputs->units[u].files[f];
        const sh_package_link_edge *sites = NULL; size_t count = 0, length = 0, bound_length = 0;
        char error[512];
        if (!sh_package_source_link_sites(source, u, file, &sites, &count)) continue;
        unsigned char *body = sh_package_compilation_read(inputs->units[u].compilation, file, SIZE_MAX,
            &length, error, sizeof(error));
        assert(body);
        site_record r = {source, u, 0, count, sites, 0};
        unsigned char *bound = sh_package_source_link_bind_file(source, u, file, body, length,
            site_reference, &r, &bound_length, error, sizeof(error));
        if (!bound || r.mismatch || r.seen != count)
            fprintf(stderr, "discovered sites differ: %s/%s %s (%zu of %zu) %s\n", inputs->units[u].family,
                inputs->units[u].name, file->engine_path, r.seen, count, bound ? "" : error);
        assert(bound && !r.mismatch && r.seen == count);
        free(body); free(bound); checked++;
    }
    assert(checked);
}
static void test_atlas_sources(fixture *native, sh_decl_registry_source registry)
{
    const char *material = "{ transAtlasMap \"textures/atlas/private.tga\"\n }";
    sh_compiled_resource files[3][2] = {0}; sh_package_compilation compiled[3] = {0};
    const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
    source_link_fixture f = {0}; f.native = native; f.certificate = 1;
    sh_package_source_link_native admission = {0}; admission.context = &f; admission.semantics = link_original_binding;
    sh_package_sources inventory = {0}; sh_package_link_output out = {0}; char error[1024];
    for (size_t i = 0; i < 3; i++) {
        files[i][0].engine_path = "generated/decls/material/atlas-fixture.decl";
        files[i][0].type = "material"; files[i][0].name = "atlas-fixture";
        files[i][0].body = (unsigned char *)material; files[i][0].body_length = strlen(material);
        files[i][1].engine_path = "generated/transsortatlas/textures/atlas/private.bimage";
        files[i][1].body = (unsigned char *)(i == 1 ? "green atlas" : "red atlas");
        files[i][1].body_length = strlen((const char *)files[i][1].body);
        files[i][0].source = files[i][1].source = SIZE_MAX;
        compiled[i].resources = files[i]; compiled[i].resource_count = 2;
    }
    sh_package_source_link *source = sh_package_source_link_open(scopes, 3, NULL,
        link_original, &f, registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    if (!source) fprintf(stderr, "atlas sources: %s\n", error);
    assert(source); f.inputs = sh_package_source_link_inputs(source); assert_discovered_sites(source);
    sh_package_link_adapter names = {(void *)f.inputs, NULL, NULL, sh_package_native_names_assign};
    int ok = sh_package_source_link_compile(source, &inventory, &names, NULL, NULL, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "atlas compile: %s\n", error);
    assert(ok);
    size_t material_class[3], atlas_class[3], image_class[3];
    for (size_t i = 0; i < 3; i++) {
        size_t root = sh_package_link_inputs_find(f.inputs, i, "material", "atlas-fixture");
        size_t atlas = sh_package_link_inputs_find(f.inputs, i, "transsortatlas", "textures/atlas/private");
        size_t image = sh_package_link_inputs_find(f.inputs, i, "image", "generated/transsortatlas/textures/atlas/private.bimage");
        assert(root != SIZE_MAX && atlas != SIZE_MAX && f.inputs->units[root].implicit_count == 2);
        assert(image != SIZE_MAX);
        material_class[i] = out.classes.nodes[root]; atlas_class[i] = out.classes.nodes[atlas];
        image_class[i] = out.classes.nodes[image];
        assert(!memcmp(files[i][0].body, material, strlen(material)));
    }
    assert(material_class[0] == material_class[2] && material_class[0] != material_class[1]);
    assert(atlas_class[0] == atlas_class[2] && atlas_class[0] != atlas_class[1]);
    assert(image_class[0] == image_class[2] && image_class[0] != image_class[1]);
    assert(out.implicit_count == 4); /* Two deduped materials, each atlas plus fallback image. */
    for (size_t i = 0; i < out.compiled->resource_count; i++) {
        const sh_compiled_resource *file = out.compiled->resources + i;
        if (file->type && !strcmp(file->type, "material"))
            assert(file->body_length == strlen(material) && !memcmp(file->body, material, strlen(material)));
    }
    for (size_t i = 0; i < out.implicit_count; i++) {
        const sh_package_link_implicit_binding *request = out.implicit + i;
        assert(!strncmp(request->slot, "request/", 8));
        if (strstr(request->slot, "request/transsortatlas/"))
            assert(request->target == (request->owner == material_class[0] ? atlas_class[0] : atlas_class[1]));
        else {
            assert(!strcmp(request->slot, "request/image/textures/atlas/private"));
            assert(request->target == (request->owner == material_class[0] ? image_class[0] : image_class[1]));
        }
    }
    sh_package_link_output_free(&out); sh_package_source_link_close(source);
}
/* Discovery keeps each path's canonical key per scope canonicalizer for the
 * capture: a scope whose canonicalizer aliases the atlas image selects its own
 * aliased supply, and the other scope its plain one, whichever comes first. */
static char *aliased_atlas(const char *path)
{
    char *key = sh_package_engine_path(path);
    if (key && !strcmp(key, "generated/transsortatlas/textures/atlas/private.bimage")) {
        free(key); key = _strdup("generated/transsortatlas/textures/atlas/aliased.bimage");
    }
    return key;
}
static void test_canonicalizer_scopes(fixture *native, sh_decl_registry_source registry)
{
    const char *material = "{ transAtlasMap \"textures/atlas/private.tga\"\n }";
    for (size_t aliased = 0; aliased < 2; aliased++) {
        sh_compiled_resource files[2][2] = {0}; sh_package_compilation compiled[2] = {0};
        const sh_package_compilation *scopes[] = {compiled, compiled + 1};
        source_link_fixture f = {0}; f.native = native; f.certificate = 1;
        sh_package_source_link_native admission = {0}; admission.context = &f; admission.semantics = link_original_binding;
        char error[1024];
        for (size_t i = 0; i < 2; i++) {
            files[i][0].engine_path = "generated/decls/material/atlas-fixture.decl";
            files[i][0].type = "material"; files[i][0].name = "atlas-fixture";
            files[i][0].body = (unsigned char *)material; files[i][0].body_length = strlen(material);
            files[i][1].engine_path = i == aliased ? "generated/transsortatlas/textures/atlas/aliased.bimage" :
                "generated/transsortatlas/textures/atlas/private.bimage";
            files[i][1].body = (unsigned char *)"red atlas"; files[i][1].body_length = 9;
            files[i][0].source = files[i][1].source = SIZE_MAX;
            compiled[i].resources = files[i]; compiled[i].resource_count = 2;
        }
        compiled[aliased].canonical_path = aliased_atlas;
        sh_package_source_link *source = sh_package_source_link_open(scopes, 2, NULL,
            link_original, &f, registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        if (!source) fprintf(stderr, "canonicalizer scopes: %s\n", error);
        assert(source);
        f.inputs = sh_package_source_link_inputs(source);
        for (size_t i = 0; i < 2; i++) {
            int selected = 0;
            for (size_t u = 0; u < f.inputs->count; u++) if (f.inputs->scopes[u] == i)
                for (size_t k = 0; k < f.inputs->units[u].file_count; k++)
                    selected |= f.inputs->units[u].files[k] == &files[i][1];
            assert(selected);
        }
        sh_package_source_link_close(source);
    }
}
/* Synthetic aliases exercise the production readers/linker, not live native
 * alias admission. Runtime ABI admission requires the engine adapter. */
static int link_assign(void *context, size_t index, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths,
    char *error, size_t capacity)
{
    source_link_fixture *f = context;
    const sh_package_link_unit *unit = f->inputs->units + index;
    char hex[65];
    (void)error; (void)capacity; (void)files; (void)original_binding;
    if (!count) { *name = _strdup(unit->name); return *name != NULL; }
    for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", identity[i]);
    *name = malloc(strlen(unit->name) + 70); assert(*name);
    sprintf(*name, "smp/%s/%s", hex, unit->name);
    *paths = calloc(count, sizeof(**paths)); assert(*paths);
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(unit->family, "image")) {
            (*paths)[i] = sh_package_cooked_path("image", *name);
            assert((*paths)[i]); continue;
        }
        size_t n = strlen(unit->family) + strlen(*name) + 24;
        (*paths)[i] = malloc(n); assert((*paths)[i]);
        snprintf((*paths)[i], n, "generated/decls/%s/%s.decl", unit->family, *name);
    }
    return 1;
}
static int link_admit(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    source_link_fixture *f = context;
    (void)scope; (void)error; (void)capacity;
    f->admissions++;
    if (strcmp(family, "image") || strcmp(name, "synthetic/runtime-image")) return 0;
    memset(semantics, 7, 32); return 1;
}
static int link_original_binding(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    source_link_fixture *f = context;
    (void)name;
    /* Native state alone is insufficient: the driver independently compares
     * source bytes before certifying the complete local original binding. */
    if (f->different_semantics && scope == 2 && !strcmp(family, "material")) semantics[0] = 19;
    f->certifications++;
    if (f->certificate < 0) snprintf(error, capacity, "synthetic native certification failed");
    return f->certificate;
}
static sh_compiled_resource link_decl(const char *name, const char *path, const char *body)
{
    sh_compiled_resource r = {0};
    r.type = "entitydef"; r.name = (char *)name; r.engine_path = (char *)path;
    r.body = (unsigned char *)body; r.body_length = strlen(body); r.source = SIZE_MAX;
    return r;
}
static int af_link_admit(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    (void)context; (void)scope; (void)error; (void)capacity;
    /* This fixture supplies a native skeleton identity instead of real game
     * bytes. Production admission must establish its own backing source. */
    if (strcmp(family, "skeleton") || strcmp(name, "md6/boss/boss.md6skl")) return 0;
    memset(semantics, 11, 32); return 1;
}
static void test_af_link(fixture *native, sh_decl_registry_source registry)
{
    const char *af = "{ settings { model \"mesh-only\" skin \"\" clipMaterial \"body\" } "
        "body \"part\" { joint \"root\" model box((0,0,0),(1,1,1)) } }";
    sh_package_sources inventory = {0};
    sh_compiled_resource files[3][3];
    sh_package_compilation compiled[3] = {0};
    const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
    sh_package_source_link *source;
    sh_package_link_output out = {0};
    source_link_fixture f = {native};
    sh_package_link_adapter adapter = {&f, NULL, NULL, link_assign};
    sh_package_source_link_native admission = {NULL, af_link_admit};
    char error[1024];
    size_t roots[3], materials[3], models[3];
    for (size_t i = 0; i < 3; i++) {
        files[i][0] = link_decl("figure", "generated/decls/articulatedfigure/figure.decl", af);
        files[i][0].type = "articulatedfigure";
        files[i][1] = link_decl("body", "generated/decls/material/body.decl",
            i == 1 ? "{ color {0,1,0} }" : "{ color {1,0,0} }");
        files[i][1].type = "material";
        files[i][2] = link_decl("boss", "generated/decls/material/boss.decl", "{ color {1,1,1} }");
        files[i][2].type = "material";
        compiled[i].resources = files[i]; compiled[i].resource_count = 3; compiled[i].sources = &inventory;
    }
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    if (!source) fprintf(stderr, "AF source link: %s\n", error);
    assert(source); f.inputs = sh_package_source_link_inputs(source); assert_discovered_sites(source);
    for (size_t i = 0; i < 3; i++) {
        roots[i] = sh_package_link_inputs_find(f.inputs, i, "articulatedfigure", "figure");
        materials[i] = sh_package_link_inputs_find(f.inputs, i, "material", "body");
        models[i] = sh_package_link_inputs_find(f.inputs, i, "md6def", "mesh-only");
        assert(roots[i] != SIZE_MAX && materials[i] != SIZE_MAX && models[i] != SIZE_MAX);
    }
    assert(sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error)));
    /* Equal source AND dependency bindings coalesce. A different surface splits
     * the referring figure, while its equal model remains shared across all three. */
    assert(out.classes.nodes[roots[0]] == out.classes.nodes[roots[2]]);
    assert(out.classes.nodes[roots[0]] != out.classes.nodes[roots[1]]);
    assert(out.classes.nodes[materials[0]] == out.classes.nodes[materials[2]]);
    assert(out.classes.nodes[materials[0]] != out.classes.nodes[materials[1]]);
    assert(out.classes.nodes[models[0]] == out.classes.nodes[models[1]] &&
        out.classes.nodes[models[1]] == out.classes.nodes[models[2]]);
    for (size_t i = 0; i < 3; i++) {
        size_t found = 0;
        for (size_t j = 0; j < out.compiled->resource_count; j++) {
            const sh_compiled_resource *r = out.compiled->resources + j;
            if (strcmp(r->type, "articulatedfigure") || strcmp(r->name, out.names[out.classes.nodes[roots[i]]])) continue;
            assert(strstr((const char *)r->body, out.names[out.classes.nodes[materials[i]]]) &&
                strstr((const char *)r->body, out.names[out.classes.nodes[models[i]]]) &&
                strstr((const char *)r->body, "body \"part\" { joint \"root\""));
            found++;
        }
        assert(found == 1 && !strcmp((const char *)files[i][0].body, af));
    }
    sh_package_link_output_free(&out); sh_package_source_link_close(source);
}

static void discrete_u32(unsigned char *body, size_t *at, unsigned value)
{
    body[(*at)++] = (unsigned char)(value >> 24); body[(*at)++] = (unsigned char)(value >> 16);
    body[(*at)++] = (unsigned char)(value >> 8); body[(*at)++] = (unsigned char)value;
}
static void discrete_name(unsigned char *body, size_t *at, const char *value)
{
    size_t length = strlen(value);
    number(body, (int)*at, (int)length); *at += 4;
    memcpy(body + *at, value, length); *at += length;
}
static size_t discrete_model(unsigned char body[1024], const char *material)
{
    size_t at = 0;
    assert(strlen(material) < 256); memset(body, 0, 1024);
    discrete_u32(body, &at, 0x02024d44); at += 32;
    discrete_u32(body, &at, 0); /* Collision hulls. */
    discrete_u32(body, &at, 1); discrete_name(body, &at, material);
    discrete_u32(body, &at, 1); at += 0x30; /* Transform. */
    discrete_u32(body, &at, 1); discrete_name(body, &at, material);
    discrete_u32(body, &at, 0); discrete_u32(body, &at, 1);
    discrete_u32(body, &at, 3); discrete_u32(body, &at, 3);
    memcpy(body + at, "body", 4); at += 3 * 0x30 + 3 * 2; /* Opaque geometry. */
    discrete_u32(body, &at, 0); body[at++] = 0; /* Remaps and base collision. */
    return at;
}
static void test_breakable_link(fixture *native, sh_decl_registry_source registry)
{
    static const char *model_names[] = {"models/part.lwo", "models/part.obj"};
    unsigned char model[1024], before[1024], expected[1024];
    size_t model_length = discrete_model(model, "body");
    memcpy(before, model, model_length);
    for (size_t spelling = 0; spelling < sizeof(model_names) / sizeof(*model_names); spelling++) {
        char root[256], error[1024];
        snprintf(root, sizeof(root), "{ model %s pieceNames {1 body} }", model_names[spelling]);
        char *physical = sh_package_cooked_path("discreteanimation", model_names[spelling]);
        sh_package_sources inventory = {0};
        sh_compiled_resource files[3][3];
        sh_package_compilation compiled[3] = {0};
        const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
        source_link_fixture f = {native};
        sh_package_source_link_native admission = {&f, NULL, link_original_binding};
        sh_package_source_link *source;
        sh_package_link_output out = {0};
        size_t roots[3], models[3], materials[3];
        assert(physical); f.certificate = 1;
        for (size_t i = 0; i < 3; i++) {
            files[i][0] = link_decl("part", "generated/decls/breakable/part.decl", root);
            files[i][0].type = "breakable";
            files[i][1] = (sh_compiled_resource){0};
            files[i][1].engine_path = physical; files[i][1].body = model;
            files[i][1].body_length = model_length; files[i][1].source = SIZE_MAX;
            files[i][2] = link_decl("body", "generated/decls/material/body.decl",
                i == 1 ? "{ color {0,1,0,1} }" : "{ color {1,0,0,1} }");
            files[i][2].type = "material";
            /* Compilations index their files in physical path order. */
            sh_compiled_resource swap = files[i][1]; files[i][1] = files[i][2]; files[i][2] = swap;
            compiled[i].resources = files[i]; compiled[i].resource_count = 3; compiled[i].sources = &inventory;
        }
        source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        if (!source) fprintf(stderr, "breakable source link: %s\n", error);
        assert(source); f.inputs = sh_package_source_link_inputs(source); assert_discovered_sites(source);
        sh_package_link_adapter names = {(void *)f.inputs, NULL, NULL, sh_package_native_names_assign};
        for (size_t i = 0; i < 3; i++) {
            roots[i] = sh_package_link_inputs_find(f.inputs, i, "breakable", "part");
            models[i] = sh_package_link_inputs_find(f.inputs, i, "discreteanimation", model_names[spelling]);
            materials[i] = sh_package_link_inputs_find(f.inputs, i, "material", "body");
            assert(roots[i] != SIZE_MAX && models[i] != SIZE_MAX && materials[i] != SIZE_MAX);
        }
        int ok = sh_package_source_link_compile(source, &inventory, &names, NULL, NULL, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "breakable source compile: %s\n", error);
        if (out.classes.count != 7 || !out.compiled || out.compiled->resource_count != 7)
            fprintf(stderr, "breakable %s: classes=%zu files=%zu\n", model_names[spelling],
                out.classes.count, out.compiled ? out.compiled->resource_count : 0);
        assert(ok && out.classes.count == 7 && out.compiled->resource_count == 7);
        /* The same binary payload binds different material dependencies. It
         * splits its model and referring breakable, but the exact third graph
         * coalesces. No source member is changed by private-name relocation. */
        assert(out.classes.nodes[roots[0]] == out.classes.nodes[roots[2]]);
        assert(out.classes.nodes[roots[0]] != out.classes.nodes[roots[1]]);
        assert(out.classes.nodes[models[0]] == out.classes.nodes[models[2]]);
        assert(out.classes.nodes[models[0]] != out.classes.nodes[models[1]]);
        for (size_t i = 0; i < 3; i++) {
            char *path = sh_package_cooked_path("discreteanimation", out.names[out.classes.nodes[models[i]]]);
            const sh_compiled_resource *bound_model = sh_package_compilation_find(out.compiled, path);
            size_t expected_length = discrete_model(expected, out.names[out.classes.nodes[materials[i]]]);
            assert(bound_model && bound_model->body_length == expected_length &&
                !memcmp(bound_model->body, expected, expected_length)); free(path);
            size_t found = 0;
            for (size_t j = 0; j < out.compiled->resource_count; j++) {
                const sh_compiled_resource *r = out.compiled->resources + j;
                if (!r->type || strcmp(r->type, "breakable") || strcmp(r->name, out.names[out.classes.nodes[roots[i]]])) continue;
                assert(strstr((char *)r->body, out.names[out.classes.nodes[models[i]]]) &&
                    strstr((char *)r->body, "pieceNames {1 body}")); found++;
            }
            assert(found == 1);
        }
        assert(!memcmp(model, before, model_length));
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
        /* Unreferenced supplied binaries remain authorable under their exact
         * cooked identity; discovering consumers must not hide orphan files. */
        compiled[0].resources = files[0] + 1; compiled[0].resource_count = 2;
        source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        assert(source); f.inputs = sh_package_source_link_inputs(source); names.context = (void *)f.inputs; assert_discovered_sites(source);
        assert(sh_package_link_inputs_find(f.inputs, 0, "discreteanimation", physical) != SIZE_MAX);
        assert(sh_package_source_link_compile(source, &inventory, &names, NULL, NULL, &out, error, sizeof(error)));
        assert(out.classes.count == 3 && out.compiled->resource_count == 3);
        sh_package_link_output_free(&out); sh_package_source_link_close(source); free(physical);
    }
}

static void test_particle_link(void)
{
    fixture native; setup(&native);
    void *container = *(void **)native.reflection;
    void *records = alloc(&native, 21 * 0x38), *objects = alloc(&native, 21 * 16), *pointers = alloc(&native, 21 * 16);
    memcpy(records, *(void **)((char *)container + 0x20), 20 * 0x38);
    memcpy(objects, *(void **)((char *)native.reflection + 0x88), 20 * 16);
    memcpy(pointers, *(void **)((char *)native.reflection + 0xa0), 20 * 16);
    pointer(container, 0x20, records);
    pointer(native.reflection, 0x88, objects); number(native.reflection, 0x90, 21);
    pointer(native.reflection, 0xa0, pointers); number(native.reflection, 0xa8, 21);
    pointer(records, 19 * 0x38, string(&native, "idStaticParticleModelData"));
    pointer(records, 19 * 0x38 + 8, string(&native, "")); pointer(pointers, 19 * 16 + 8, (void *)0x90);
    const char *t[] = {"idStaticParticleModelData", "idStr"}, *o[] = {"*", ""}, *n[] = {"staticData", "label"};
    fields(&native, (char *)records + 0x38, 2, t, o, n);
    sh_decl_registry_source registry = {&native, read_memory, (uintptr_t)native.registry, NULL};
    char error[1024];
    {
        sh_decl_native_registry *r = sh_decl_native_registry_open(registry, error, sizeof(error));
        sh_decl_native_schema *s = sh_decl_native_schema_open(
            (sh_decl_native_source){&native, read_memory, (uintptr_t)native.reflection},
            (sh_decl_dependency_observer){0}, error, sizeof(error));
        sh_decl_native_reference reference = {0};
        const char *spelling = "/MODELS\\FX\\PART.OBJ";
        assert(r && s && sh_decl_native_reference_read(s, r,
            (sh_decl_value_type){"idStaticParticleModelData", "*"}, spelling, strlen(spelling), &reference) == 1);
        assert(!strcmp(reference.resource.type, "staticparticlemodel") && !strcmp(reference.resource.name, "models/fx/part.obj"));
        char *bound = sh_decl_native_reference_bind(&reference, "smp/other");
        assert(bound && !strcmp(bound, "smp/other")); free(bound);
        assert(sh_decl_native_reference_read(s, r, (sh_decl_value_type){"idStaticParticleModelData", "*"},
            "", 0, &reference) == 0 && !reference.resource.name);
        sh_decl_native_reference_clear(&reference); sh_decl_native_registry_close(r); sh_decl_native_schema_close(s);
    }
    const char *names[] = {"models/fx/particle.v2.obj", "models/fx/123456789012345678901234567890123456789012345.lwo"};
    for (size_t spelling = 0; spelling < sizeof(names) / sizeof(*names); spelling++)
    for (int mesh_variant = 0; mesh_variant < 2; mesh_variant++) {
        unsigned char geometry[3][204] = {{0}}, before[3][204]; char declarations[3][512];
        sh_package_sources inventory = {0}; sh_compiled_resource files[3][2];
        sh_package_compilation compiled[3] = {0};
        const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
        source_link_fixture f = {&native}; f.certificate = 1;
        sh_package_source_link_native admission = {&f, NULL, link_original_binding};
        sh_package_link_output out = {0}; size_t roots[3], meshes[3];
        char *physical = sh_package_cooked_path("staticparticlemodel", names[spelling]); assert(physical);
        for (size_t i = 0; i < 3; i++) {
            memcpy(geometry[i], "\1LMP", 4); geometry[i][7] = 9; geometry[i][11] = 4;
            geometry[i][12] = mesh_variant && i == 1 ? 2 : 1;
            memcpy(before[i], geometry[i], sizeof(geometry[i]));
            snprintf(declarations[i], sizeof(declarations[i]), "{ class = \"Entity\"; edit = { staticData = \"%s\"; label = \"%s\"; } }",
                names[spelling], !mesh_variant && i == 1 ? "alternate" : "original");
            files[i][0] = link_decl("particle-root", "generated/decls/entitydef/particle-root.decl", declarations[i]);
            files[i][1] = (sh_compiled_resource){0}; files[i][1].engine_path = physical;
            files[i][1].body = geometry[i]; files[i][1].body_length = sizeof(geometry[i]); files[i][1].source = SIZE_MAX;
            compiled[i].resources = files[i]; compiled[i].resource_count = 2; compiled[i].sources = &inventory;
        }
        sh_package_source_link *source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
            registry, (uintptr_t)native.reflection, &admission, error, sizeof(error));
        if (!source) fprintf(stderr, "particle source link: %s\n", error);
        assert(source); f.inputs = sh_package_source_link_inputs(source); assert_discovered_sites(source);
        sh_package_link_adapter adapter = {(void *)f.inputs, NULL, NULL, sh_package_native_names_assign};
        for (size_t i = 0; i < 3; i++) {
            roots[i] = sh_package_link_inputs_find(f.inputs, i, "entitydef", "particle-root");
            meshes[i] = sh_package_link_inputs_find(f.inputs, i, "staticparticlemodel", names[spelling]);
            assert(roots[i] != SIZE_MAX && meshes[i] != SIZE_MAX);
        }
        int ok = sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "particle source compile: %s\n", error);
        assert(ok && out.compiled->resource_count == (mesh_variant ? 4 : 3));
        assert(out.classes.nodes[roots[0]] == out.classes.nodes[roots[2]] && out.classes.nodes[roots[0]] != out.classes.nodes[roots[1]]);
        assert(out.classes.nodes[meshes[0]] == out.classes.nodes[meshes[2]]);
        assert((out.classes.nodes[meshes[0]] != out.classes.nodes[meshes[1]]) == mesh_variant);
        for (size_t i = 0; i < 3; i++) {
            char *path = sh_package_cooked_path("staticparticlemodel", out.names[out.classes.nodes[meshes[i]]]);
            const sh_compiled_resource *file = sh_package_compilation_find(out.compiled, path);
            assert(file && file->body_length == sizeof(geometry[i]) && !memcmp(file->body, before[i], sizeof(geometry[i])));
            free(path); size_t found = 0;
            for (size_t j = 0; j < out.compiled->resource_count; j++) {
                const sh_compiled_resource *root = out.compiled->resources + j;
                if (root->type && !strcmp(root->type, "entitydef") && !strcmp(root->name, out.names[out.classes.nodes[roots[i]]])) {
                    assert(strstr((char *)root->body, out.names[out.classes.nodes[meshes[i]]])); found++;
                }
            }
            assert(found == 1 && !memcmp(geometry[i], before[i], sizeof(geometry[i])));
        }
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
        geometry[0][11] = 5;
        source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
            registry, (uintptr_t)native.reflection, &admission, error, sizeof(error));
        assert(!source && strstr(error, "vertex count")); geometry[0][11] = 4;
        /* An orphan is retained under its reproducible relative cooked name;
         * it must not acquire a fabricated .lwo dependency or disappear. */
        compiled[0].resources = files[0] + 1; compiled[0].resource_count = 1;
        source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
            registry, (uintptr_t)native.reflection, &admission, error, sizeof(error));
        assert(source); f.inputs = sh_package_source_link_inputs(source); adapter.context = (void *)f.inputs; assert_discovered_sites(source);
        assert(sh_package_link_inputs_find(f.inputs, 0, "staticparticlemodel", physical + 10) != SIZE_MAX);
        assert(sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error)));
        assert(out.compiled->resource_count == 1);
        sh_package_link_output_free(&out); sh_package_source_link_close(source); free(physical);
    }
    assert(!native.object_state_reads);
    while (native.count) free(native.regions[--native.count].address);
}

static void test_animation_web_link(fixture *native, sh_decl_registry_source registry)
{
    const char *web = "{props {modelInfos {\"robot\" \"idle\"}} states {state \"idle\" {}} "
        "subWebs {subWeb \"main\" {node \"idle\" {props {} blendTrees {tree {blendEq \"anim0\" "
        "anims {alias {name \"idle\"} alias {name \"_default.md6anim\"} "
        "alias {name \"md6/boss/motion/idle.md6anim\"}}}} "
        "edges {edge {toState \"idle\" blendParms {destAnim \"md6/boss/motion/idle.md6anim\"}}}}}}}";
    const char *model = "{init {mesh \"md6/boss/boss.md6mesh\"} aliases {alias {name \"idle\" "
        "anim \"md6/boss/motion/idle.md6anim\"}}}";
    const char *composite = "{props {composite {\"motion\"}}}";
    for (int animation_variant = 0; animation_variant < 2; animation_variant++) {
        unsigned char animation[3][256], before[3][256]; size_t animation_length[3];
        sh_package_sources inventory = {0}; sh_compiled_resource files[3][5];
        sh_package_compilation compiled[3] = {0};
        const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
        source_link_fixture f = {native}; f.certificate = 1;
        sh_package_source_link_native admission = {&f, af_link_admit, link_original_binding};
        sh_package_source_link *source; sh_package_link_output out = {0}; char error[1024];
        size_t roots[3], webs[3], models[3], meshes[3], animations[3];
        for (size_t i = 0; i < 3; i++) {
            animation_length[i] = cooked_anim(animation[i], "md6/boss/boss.md6skl");
            if (animation_variant && i == 1) animation[i][animation_length[i] - 1] = 1;
            memcpy(before[i], animation[i], animation_length[i]);
            files[i][0] = (sh_compiled_resource){0};
            files[i][0].engine_path = "cooked/anim/md6/boss/motion/idle.bmd6anim";
            files[i][0].body = animation[i]; files[i][0].body_length = animation_length[i];
            files[i][0].source = SIZE_MAX;
            files[i][1] = link_decl("composite", "generated/decls/animweb/composite.decl", composite);
            files[i][1].type = "animweb";
            files[i][2] = link_decl("motion", "generated/decls/animweb/motion.decl", web);
            files[i][2].type = "animweb";
            files[i][3] = link_decl("boss", "generated/decls/material/boss.decl",
                !animation_variant && i == 1 ? "{color {0,1,0,1}}" : "{color {1,0,0,1}}");
            files[i][3].type = "material";
            files[i][4] = link_decl("robot", "generated/decls/md6def/robot.decl", model);
            files[i][4].type = "md6def";
            compiled[i].resources = files[i]; compiled[i].resource_count = 5; compiled[i].sources = &inventory;
        }
        source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        if (!source) fprintf(stderr, "animation web source: %s\n", error);
        assert(source); f.inputs = sh_package_source_link_inputs(source); assert_discovered_sites(source);
        sh_package_link_adapter names = {(void *)f.inputs, NULL, NULL, sh_package_native_names_assign};
        for (size_t i = 0; i < 3; i++) {
            roots[i] = sh_package_link_inputs_find(f.inputs, i, "animweb", "composite");
            webs[i] = sh_package_link_inputs_find(f.inputs, i, "animweb", "motion");
            models[i] = sh_package_link_inputs_find(f.inputs, i, "md6def", "robot");
            meshes[i] = sh_package_link_inputs_find(f.inputs, i, "basemodel", "md6/boss/boss.md6mesh");
            animations[i] = sh_package_link_inputs_find(f.inputs, i, "anim", "md6/boss/motion/idle.md6anim");
            assert(roots[i] != SIZE_MAX && webs[i] != SIZE_MAX && models[i] != SIZE_MAX &&
                meshes[i] != SIZE_MAX && animations[i] != SIZE_MAX);
        }
        int ok = sh_package_source_link_compile(source, &inventory, &names, NULL, NULL, &out, error, sizeof(error));
        if (!ok) fprintf(stderr, "animation web compile: %s\n", error);
        assert(ok);
        assert(out.classes.nodes[roots[0]] == out.classes.nodes[roots[2]] &&
            out.classes.nodes[roots[0]] != out.classes.nodes[roots[1]]);
        assert(out.classes.nodes[webs[0]] == out.classes.nodes[webs[2]] &&
            out.classes.nodes[webs[0]] != out.classes.nodes[webs[1]]);
        assert(out.classes.nodes[models[0]] == out.classes.nodes[models[2]] &&
            out.classes.nodes[models[0]] != out.classes.nodes[models[1]]);
        assert((out.classes.nodes[meshes[0]] == out.classes.nodes[meshes[1]]) == animation_variant);
        assert((out.classes.nodes[animations[0]] != out.classes.nodes[animations[1]]) == animation_variant);
        for (size_t i = 0; i < 3; i++) {
            size_t found = 0;
            for (size_t j = 0; j < out.compiled->resource_count; j++) {
                const sh_compiled_resource *r = out.compiled->resources + j;
                if (!r->type || strcmp(r->type, "animweb")) continue;
                if (!strcmp(r->name, out.names[out.classes.nodes[roots[i]]])) {
                    assert(strstr((char *)r->body, out.names[out.classes.nodes[webs[i]]])); found++;
                } else if (!strcmp(r->name, out.names[out.classes.nodes[webs[i]]])) {
                    assert(strstr((char *)r->body, out.names[out.classes.nodes[models[i]]]) &&
                        strstr((char *)r->body, out.names[out.classes.nodes[animations[i]]]) &&
                        strstr((char *)r->body, "alias {name \"idle\"}") &&
                        strstr((char *)r->body, "alias {name \"_default.md6anim\"}") &&
                        strstr((char *)r->body, "toState \"idle\"")); found++;
                }
            }
            assert(found == 2 && !strcmp((char *)files[i][2].body, web) &&
                !memcmp(before[i], animation[i], animation_length[i]));
        }
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
    }
}

typedef struct implicit_source_fixture { size_t captures; int failure; } implicit_source_fixture;
static int source_implicit(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visitor, char *error, size_t capacity)
{
    implicit_source_fixture *f = context; (void)scope;
    assert(!observing && unit->file_count == 1 && unit->files[0]); f->captures++;
    if (f->failure == 1) { snprintf(error, capacity, "synthetic implicit capture failed"); return -1; }
    if (f->failure == 2) return 0;
    const char *target = !strcmp(unit->name, "z-root") ? "parent-only" :
        !strcmp(unit->name, "parent-only") ? "alpha" : "z-root";
    char slot[] = "constructor/input";
    if (!visit(visitor, f->failure == 3 ? "" : slot, "entitydef", target)) return 0;
    if (!visit(visitor, slot, "entitydef", f->failure == 4 ? unit->name : target)) return 0;
    memset(slot, '?', sizeof(slot) - 1); /* The driver owns the captured slot. */
    return 1;
}
typedef struct font_source_fixture { size_t reads; int status, fail_reads; } font_source_fixture;
static int font_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    font_source_fixture *f = context; f->reads++; *body = NULL; *length = 0;
    if (f->fail_reads) return -1;
    if (!strcmp(path, "generated/decls/renderparm/color.decl")) {
        *body = (unsigned char *)_strdup("{ Vec 0 }"); assert(*body); *length = strlen((char *)*body); return 1;
    }
    if (!strcmp(path, "generated/decls/renderparm/landpagefile.decl")) {
        *body = (unsigned char *)_strdup("{ String }"); assert(*body); *length = strlen((char *)*body); return 1;
    }
    if (strcmp(path, "generated/decls/material/snapmap/title_worldtext.decl")) return 0;
    if (f->status > 0) {
        *body = (unsigned char *)_strdup("{ color {0,0,1} }"); assert(*body); *length = strlen((char *)*body);
    }
    return f->status;
}
static int font_semantics(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    (void)context; (void)scope; (void)name; (void)semantics; (void)error; (void)capacity;
    /* Synthetic fixed parameter ABI, matching the original reader's bytes. */
    return !strcmp(family, "renderparm");
}
static void test_font_source_link(fixture *native, sh_decl_registry_source registry)
{
    /* Equal glyph bytes do not imply equal fonts: constructor materials have
     * their own scoped bindings. An optional peer material is not a baseline. */
    unsigned char glyphs[14] = {'i','d','f','+',0,12,0,0,0,12,0,12,0,0};
    sh_package_sources inventory = {0}; sh_compiled_resource files[3][3] = {0};
    sh_package_compilation compiled[3] = {0};
    const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
    sh_package_link_output out = {0}; font_source_fixture f = {0};
    sh_package_source_link_native admission = {0}; admission.semantics = font_semantics;
    char error[1024]; size_t fonts[3], materials[3];
    for (size_t i = 0; i < 3; i++) {
        files[i][0].engine_path = "fonts/title/64_df.dat";
        files[i][0].body = glyphs; files[i][0].body_length = sizeof(glyphs); files[i][0].source = SIZE_MAX;
        files[i][1] = link_decl("fonts/title/64_df.tga", "generated/decls/material/fonts/title/64_df.tga.decl",
            i == 1 ? "{ color {0,1,0} }" : "{ color {1,0,0} }");
        files[i][1].type = "material";
        files[i][2] = link_decl("snapmap/title_worldtext", "generated/decls/material/snapmap/title_worldtext.decl", "{ color {0,0,1} }");
        files[i][2].type = "material";
        compiled[i].resources = files[i]; compiled[i].resource_count = i == 1 ? 3 : 2; compiled[i].sources = &inventory;
    }
    sh_package_source_link *source = sh_package_source_link_open(scopes, 3, NULL, font_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    if (!source) fprintf(stderr, "font source capture: %s\n", error);
    assert(source);
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(source);
    assert(inputs->count == 10); /* Includes one fixed color parameter per scope. */
    for (size_t i = 0; i < 3; i++) {
        fonts[i] = sh_package_link_inputs_find(inputs, i, "font", "title");
        materials[i] = sh_package_link_inputs_find(inputs, i, "material", "fonts/title/64_df.tga");
        assert(fonts[i] < inputs->count && materials[i] < inputs->count);
        const sh_package_link_unit *font = inputs->units + fonts[i];
        assert(font->file_count == 1 && font->implicit_count == (i == 1 ? 2u : 1u));
        assert(!strcmp(font->implicit[0].slot, "font/material/64_df") && font->implicit[0].target == materials[i]);
        if (i != 1) assert(sh_package_link_inputs_find(inputs, i, "material", "snapmap/title_worldtext") == SIZE_MAX);
    }
    size_t reads = f.reads; f.fail_reads = 1;
    sh_package_link_adapter adapter = {(void *)inputs, NULL, NULL, sh_package_native_names_assign};
    int linked = sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error));
    if (!linked) fprintf(stderr, "font source compile: %s\n", error);
    assert(linked && f.reads == reads && out.classes.count == 6 && out.implicit_count == 3);
    assert(out.classes.nodes[fonts[0]] == out.classes.nodes[fonts[2]] && out.classes.nodes[fonts[0]] != out.classes.nodes[fonts[1]]);
    for (size_t i = 0; i < 3; i++) {
        sh_font_binary_paths paths;
        assert(sh_font_binary_paths_for(out.names[out.classes.nodes[fonts[i]]], SH_FONT_DISTANCE_64, &paths));
        const sh_compiled_resource *file = sh_package_compilation_find(out.compiled, paths.file);
        assert(file && file->body_length == sizeof(glyphs) && !memcmp(file->body, glyphs, sizeof(glyphs)));
        assert(file->bound_dependency_count == (i == 1 ? 2u : 1u));
        sh_font_binary_paths_clear(&paths);
    }
    sh_package_link_output_free(&out); sh_package_source_link_close(source); f.fail_reads = 0;

    /* Existing SnapMap originals are usable; campaign-only optional material
     * is positively unavailable until the author supplies it in this scope. */
    for (f.status = 1; f.status <= 2; f.status++) {
        source = sh_package_source_link_open(scopes, 1, NULL, font_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        assert(source); inputs = sh_package_source_link_inputs(source);
        size_t font = sh_package_link_inputs_find(inputs, 0, "font", "title");
        assert(font < inputs->count && inputs->units[font].implicit_count == (f.status == 1 ? 2u : 1u));
        sh_package_source_link_close(source);
    }
    f.status = -1;
    source = sh_package_source_link_open(scopes, 1, NULL, font_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    assert(!source && strstr(error, "original resource could not be captured")); f.status = 0;

    /* A required glyph material cannot be borrowed from a complete peer. */
    compiled[0].resource_count = 1;
    source = sh_package_source_link_open(scopes, 3, NULL, font_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    assert(!source && strstr(error, "glyph material could not be captured")); compiled[0].resource_count = 2;
    glyphs[3] = '*';
    source = sh_package_source_link_open(scopes, 1, NULL, font_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    assert(!source && strstr(error, "unsupported glyph envelope")); glyphs[3] = '+';

    {
        sh_compiled_resource fallback[3] = {files[0][0], files[0][0], files[0][1]};
        fallback[0].engine_path = "fonts/title/12.dat";
        fallback[1].engine_path = "fonts/title/48.dat"; fallback[1].body_length = 0;
        fallback[2].engine_path = "generated/decls/material/fonts/title/12.tga.decl"; fallback[2].name = "fonts/title/12.tga";
        compiled[0].resources = fallback; compiled[0].resource_count = 3;
        source = sh_package_source_link_open(scopes, 1, NULL, font_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        assert(source); inputs = sh_package_source_link_inputs(source);
        size_t font = sh_package_link_inputs_find(inputs, 0, "font", "title");
        assert(font < inputs->count && inputs->units[font].file_count == 2 && inputs->units[font].implicit_count == 1);
        assert(!strcmp(inputs->units[font].implicit[0].slot, "font/material/12"));
        adapter.context = (void *)inputs;
        assert(sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error)));
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
        fallback[0].body_length = 0;
        source = sh_package_source_link_open(scopes, 1, NULL, font_original, &f,
            registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
        assert(!source && strstr(error, "12.dat fallback"));
        compiled[0].resources = files[0]; compiled[0].resource_count = 2;
    }

    /* A valid source graph is not proof of an installed native consumer. */
    sh_package_library_native production = sh_package_native_assets_adapter(NULL);
    sh_package_link_unit observed_font = {"font", "title"};
    assert(production.source.implicit(production.source.context, 0, 1, &observed_font,
        NULL, NULL, error, sizeof(error)) == 1); /* Observation grants no admission. */
    source = sh_package_source_link_open(scopes, 1, NULL, font_original, &f,
        registry, (uintptr_t)native->reflection, &production.source, error, sizeof(error));
    assert(!source && strstr(error, "font native consumer adapter is not configured"));
}
static int font_atlas(void *context, size_t scope, const sh_package_compilation *compiled,
    const char *material, const char *page_file, sh_package_virtual_atlas *out, char *error, size_t capacity)
{
    (void)context; (void)scope; (void)error; (void)capacity;
    assert(compiled && !strcmp(material, "fonts/title/64_df.tga") && !*page_file);
    /* This fixture has no VT row; retain an explicit original empty provider. */
    out->name = "_vmtr"; out->source = "virtualtextures/_vmtr"; out->original = 1; return 1;
}
static void test_font_library(fixture *native, sh_decl_registry_source registry)
{
    unsigned char glyphs[14] = {'i','d','f','+',0,12,0,0,0,12,0,12,0,0};
    const char *folders[] = {"red", "green", "duplicate"};
    char temporary[MAX_PATH], relative[256], path[4096], error[1024];
    font_source_fixture original_fonts = {0};
    sh_package_compile_environment env = {0};
    sh_package_library library = {0}, restored = {0};
    sh_package_library_native adapter = {0}; adapter.source.semantics = font_semantics; adapter.atlas = font_atlas;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-fonts-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, i == 2 ? "{\"id\":\"font\",\"name\":\"Copy\"}" : "{\"id\":\"font\",\"name\":\"Font\"}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/fonts/title/64_df.dat", folders[i]);
        create(relative, ""); snprintf(path, sizeof(path), "%s/%s", root, relative);
        HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written; assert(handle != INVALID_HANDLE_VALUE);
        assert(WriteFile(handle, glyphs, sizeof(glyphs), &written, NULL) && written == sizeof(glyphs)); CloseHandle(handle);
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/material/fonts/title/64_df.tga.decl", folders[i]);
        create(relative, i == 1 ? "{ color {0,1,0} }" : "{ color {1,0,0} }");
    }
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    env.baseline = font_original; env.baseline_context = &original_fonts;
    int ok = sh_package_library_compile(sources, &env, NULL, font_original, &original_fonts,
        registry, (uintptr_t)native->reflection, &adapter, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "font library: %s\n", error);
    assert(ok && library.compiled);
    size_t fonts = 0, duplicate = 0;
    for (size_t i = 0; i < library.bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = library.bindings.bindings + i;
        if (strcmp(binding->family, "font")) continue;
        fonts++; duplicate += binding->context_count == 2;
        const sh_package_consumer *owner = sh_package_consumers_find(library.consumers, "font", binding->name);
        assert(owner && owner->count == 1);
        assert(sh_package_consumer_find(owner, "font/material/64_df"));
    }
    if (fonts != 2 || duplicate != 1) {
        fprintf(stderr, "font library bindings: %zu fonts, %zu duplicate, %zu total\n", fonts, duplicate, library.bindings.binding_count);
        for (size_t i = 0; i < library.bindings.binding_count; i++)
            fprintf(stderr, "  %s/%s contexts=%zu\n", library.bindings.bindings[i].family,
                library.bindings.bindings[i].original, library.bindings.bindings[i].context_count);
    }
    assert(fonts == 2 && duplicate == 1);
    sh_package_provenance saved = {0}; size_t length;
    unsigned char *encoded = sh_package_provenance_encode(&library.bindings, &length, error, sizeof(error));
    assert(encoded && sh_package_provenance_decode(encoded, length, &saved, error, sizeof(error))); free(encoded);
    ok = sh_package_library_compile_contexts(sources, &saved, &env, NULL, font_original, &original_fonts,
        registry, (uintptr_t)native->reflection, &adapter, &restored, error, sizeof(error));
    if (!ok) fprintf(stderr, "saved font library: %s\n", error);
    assert(ok);
    for (size_t i = 0; i < library.bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = library.bindings.bindings + i;
        if (!strcmp(binding->family, "font"))
            assert(sh_package_consumers_equal(library.consumers, restored.consumers, "font", binding->name));
    }
    sh_package_provenance_free(&saved); sh_package_library_free(&restored); sh_package_library_free(&library);
    sh_package_sources_free(sources); cleanup(); assert(!failures);
}
static void test_implicit_source_link(fixture *native, sh_decl_registry_source registry)
{
    const char *root = "{ class = \"Entity\"; edit = {} }";
    const char *other = "{ class = \"AlternateEntity\"; edit = {} }";
    sh_package_sources inventory = {0}; sh_compiled_resource files[3][2];
    sh_package authored = {0}; inventory.packages = &authored; inventory.package_count = 1;
    sh_package_compilation compiled[3] = {0};
    const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2};
    source_link_fixture f = {native}; implicit_source_fixture implicit = {0};
    sh_package_source_link_native admission = {0}; admission.context = &implicit; admission.implicit = source_implicit;
    sh_package_link_adapter adapter = {&f, NULL, NULL, link_assign};
    sh_package_link_output out = {0}; char error[1024]; size_t roots[3], targets[3], parents[3];
    for (size_t i = 0; i < 3; i++) {
        /* Parent-only is discovered after seeding but sorts between these
         * inputs. Target indices must use the complete final graph. */
        files[i][0] = link_decl("alpha", "generated/decls/entitydef/alpha.decl", i == 1 ? other : root);
        files[i][1] = link_decl("z-root", "generated/decls/entitydef/z-root.decl", root);
        compiled[i].resources = files[i]; compiled[i].resource_count = 2; compiled[i].sources = &inventory;
    }
    files[1][0].gameplay_owners.bits = 1;
    sh_package_source_link *source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    if (!source) fprintf(stderr, "implicit source link (%zu captures): %s\n", implicit.captures, error);
    assert(source && implicit.captures == 9 && f.reads == 1); f.inputs = sh_package_source_link_inputs(source);
    assert(f.inputs->count == 9);
    const unsigned char *shared = sh_package_source_link_shared(source);
    for (size_t i = 0; i < 3; i++) {
        roots[i] = sh_package_link_inputs_find(f.inputs, i, "entitydef", "z-root");
        targets[i] = sh_package_link_inputs_find(f.inputs, i, "entitydef", "alpha");
        parents[i] = sh_package_link_inputs_find(f.inputs, i, "entitydef", "parent-only");
        assert(roots[i] < 9 && targets[i] < 9 && parents[i] < 9);
        const sh_package_link_unit *r = f.inputs->units + roots[i], *t = f.inputs->units + targets[i];
        assert(r->implicit_count == 1 && t->implicit_count == 1);
        assert(!strcmp(r->implicit[0].slot, "constructor/input"));
        assert(r->implicit[0].target == parents[i] && t->implicit[0].target == roots[i]);
        assert(f.inputs->units[parents[i]].implicit[0].target == targets[i]);
        assert(shared[roots[i]] == (i != 1) && shared[targets[i]] == (i != 1) && shared[parents[i]] == (i != 1));
    }
    /* The frozen graph must not repeat discovery or read current native state. */
    implicit.failure = 1;
    int linked = sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error));
    if (!linked) fprintf(stderr, "implicit source compile: %s\n", error);
    assert(linked);
    assert(implicit.captures == 9 && f.reads == 1 && out.classes.count == 6 && out.implicit_count == 6);
    assert(out.classes.nodes[roots[0]] == out.classes.nodes[roots[2]]);
    assert(out.classes.nodes[roots[0]] != out.classes.nodes[roots[1]]);
    for (size_t i = 0; i < out.compiled->resource_count; i++) {
        const sh_compiled_resource *r = out.compiled->resources + i;
        assert(r->bound_dependency_count == 1);
        const sh_compiled_resource *t = sh_package_compilation_find(out.compiled, r->bound_dependencies[0]);
        assert(t && t != r && t->bound_dependency_count == 1);
        const sh_compiled_resource *third = sh_package_compilation_find(out.compiled, t->bound_dependencies[0]);
        assert(third && third != r && third != t && third->bound_dependency_count == 1);
        assert(!strcmp(third->bound_dependencies[0], r->engine_path));
    }
    sh_package_source_link_close(source);
    assert(!strcmp(out.implicit[0].slot, "constructor/input")); /* Output owns its snapshot. */
    sh_package_link_output_free(&out);
    for (implicit.failure = 1; implicit.failure <= 4; implicit.failure++) {
        source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        assert(!source && *error);
        if (implicit.failure == 1) assert(strstr(error, "synthetic implicit capture failed"));
        if (implicit.failure == 3) assert(strstr(error, "semantic slot"));
        if (implicit.failure == 4) assert(strstr(error, "contradictory targets"));
    }
    implicit.failure = 0;
    compiled[0].resources = files[0] + 1; compiled[0].resource_count = 1;
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    assert(!source && strstr(error, "resource has no admitted native source"));
    assert(!strcmp((const char *)files[0][1].body, root));
}

/* ---- Discovery replay ------------------------------------------------------
 * Every scope rediscovers the stock units it reaches. A unit whose files are
 * all unsupplied originals replays the reads of its first valid discovery when
 * its scope supplies none of the paths that discovery looked up. Replay is an
 * optimization only: with it off the same scopes give the same inputs, sites,
 * payload identities, classes, names and outputs; native callbacks run for
 * every unit either way, and each scope keeps its own bindings. */
enum { REPLAY_PLAIN, REPLAY_ROOT, REPLAY_MOD, REPLAY_PARENT, REPLAY_LATE, REPLAY_SCOPES };
typedef struct replay_calls { char entries[256][96]; size_t count; } replay_calls;
static int replay_implicit(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visitor, char *error, size_t capacity)
{
    replay_calls *calls = context;
    (void)observing; (void)visit; (void)visitor; (void)error; (void)capacity;
    assert(calls->count < 256);
    snprintf(calls->entries[calls->count++], sizeof(calls->entries[0]), "%zu %s %s", scope, unit->family, unit->name);
    return 1;
}
/* The child's class, and so which of its fields are references, comes from
 * its parent. Its target and modifier names are checked for a source in the
 * scope (the modifier is absent from stock: the engine defaults it unless the
 * scope supplies one). The beast's md6 parent resolves by name with no source
 * lookup, so a scope changing that parent still replays the beast. */
/* test_source_memo changes stock bytes behind the same reader callback: the
 * child's own declaration, and the label render parameter a material reads. */
static int g_replay_child_changed, g_replay_label_texture;
static const char *replay_stock(const char *path)
{
    if (!strcmp(path, "generated/decls/entitydef/replay/child.decl"))
        return g_replay_child_changed ?
            "{ inherit = \"replay/parent\"; edit = { target = \"parent-only\"; modifier = \"replay/mod\"; "
            "alternate = \"replay/other\"; } }" :
            "{ inherit = \"replay/parent\"; edit = { target = \"root\"; modifier = \"replay/mod\"; "
            "alternate = \"replay/other\"; } }";
    if (g_replay_label_texture && !strcmp(path, "generated/decls/renderparm/label.decl"))
        return "{ Tex2D textures/default/body }";
    if (!strcmp(path, "generated/decls/entitydef/replay/parent.decl") ||
        !strcmp(path, "generated/decls/entitydef/replay/other.decl") ||
        !strcmp(path, "generated/decls/entitydef/root.decl")) return "{ class = \"Entity\"; edit = {} }";
    if (!strcmp(path, "generated/decls/md6def/replay/beast.decl"))
        return "{ init { inherit \"replay/base\" calcRefBoundsFromJoints 1 } }";
    if (!strcmp(path, "generated/decls/md6def/replay/base.decl")) return "{ init { calcRefBoundsFromJoints 1 } }";
    return NULL;
}
static int replay_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = replay_stock(path);
    if (!text) return link_original(context, path, body, length);
    ((source_link_fixture *)context)->reads++;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static int replay_owners_same(const sh_package_owners *a, const sh_package_owners *b)
{
    for (size_t i = 0; i < 256; i++) if (sh_package_owners_contains(a, i) != sh_package_owners_contains(b, i)) return 0;
    return 1;
}
static int replay_path_order(const void *a, const void *b)
{ return strcmp(((const sh_compiled_resource *)a)->engine_path, ((const sh_compiled_resource *)b)->engine_path); }
typedef struct replay_run {
    sh_package_source_link *source;
    sh_package_link_output out;
    replay_calls calls;
    source_link_fixture link;
} replay_run;
static void replay_open_memo(fixture *native, sh_decl_registry_source registry, const sh_package_compilation *const *scopes,
    sh_package_sources *inventory, int replay, sh_package_source_memo *memo, replay_run *run)
{
    char error[1024];
    sh_package_source_link_native natives = {0};
    memset(run, 0, sizeof(*run)); run->link.native = native;
    natives.context = &run->calls; natives.implicit = replay_implicit; natives.memo = memo;
    sh_package_source_link_replay(replay);
    run->source = sh_package_source_link_open(scopes, REPLAY_SCOPES, NULL, replay_original, &run->link,
        registry, (uintptr_t)native->reflection, &natives, error, sizeof(error));
    sh_package_source_link_replay(1);
    if (!run->source) fprintf(stderr, "replay source link (%s): %s\n", replay ? "on" : "off", error);
    assert(run->source); run->link.inputs = sh_package_source_link_inputs(run->source);
    sh_package_link_adapter adapter = {&run->link, NULL, NULL, link_assign};
    int linked = sh_package_source_link_compile(run->source, inventory, &adapter, NULL, NULL, &run->out, error, sizeof(error));
    if (!linked) fprintf(stderr, "replay source compile (%s): %s\n", replay ? "on" : "off", error);
    assert(linked);
}
static void replay_open(fixture *native, sh_decl_registry_source registry, const sh_package_compilation *const *scopes,
    sh_package_sources *inventory, int replay, replay_run *run)
{ replay_open_memo(native, registry, scopes, inventory, replay, NULL, run); }
static void replay_close(replay_run *run)
{
    sh_package_link_output_free(&run->out); sh_package_source_link_close(run->source); free(run);
}
/* Inputs (files, bytes, sites, identities, implicit requests, semantics,
 * ownership), classes, names, outputs, native calls and original reads. */
static void replay_same(const replay_run *a, const replay_run *b)
{
    const sh_package_link_inputs *x = a->link.inputs, *y = b->link.inputs;
    assert(x->count == y->count);
    for (size_t i = 0; i < x->count; i++) {
        const sh_package_link_unit *u = x->units + i, *v = y->units + i;
        assert(x->scopes[i] == y->scopes[i] && !strcmp(u->family, v->family) && !strcmp(u->name, v->name));
        assert(u->file_count == v->file_count && u->original_binding == v->original_binding &&
            !memcmp(u->semantics, v->semantics, 32) && u->witness_count == v->witness_count &&
            u->implicit_count == v->implicit_count && u->stable_name == v->stable_name && u->fixed_name == v->fixed_name);
        assert(replay_owners_same(u->implicit_owners, v->implicit_owners) &&
            replay_owners_same(u->implicit_gameplay_owners, v->implicit_gameplay_owners));
        for (size_t k = 0; k < u->implicit_count; k++)
            assert(!strcmp(u->implicit[k].slot, v->implicit[k].slot) && u->implicit[k].target == v->implicit[k].target);
        for (size_t j = 0; j < u->file_count; j++) {
            const sh_compiled_resource *p = u->files[j], *q = v->files[j];
            const sh_package_link_edge *ps = NULL, *qs = NULL; size_t pc = 0, qc = 0;
            sh_package_file_identity pi, qi;
            assert(!strcmp(p->engine_path, q->engine_path) && p->body_length == q->body_length &&
                (!p->body_length || !memcmp(p->body, q->body, p->body_length)));
            assert(sh_package_source_link_sites(a->source, i, p, &ps, &pc) == sh_package_source_link_sites(b->source, i, q, &qs, &qc));
            assert(pc == qc);
            for (size_t k = 0; k < pc; k++) assert(!strcmp(ps[k].slot, qs[k].slot) && ps[k].target == qs[k].target);
            assert(sh_package_source_link_identity(a->source, i, p, &pi, NULL, 0) &&
                sh_package_source_link_identity(b->source, i, q, &qi, NULL, 0) && !memcmp(&pi, &qi, sizeof(pi)));
        }
    }
    const sh_package_link_output *m = &a->out, *n = &b->out;
    assert(m->classes.count == n->classes.count && m->classes.node_count == n->classes.node_count);
    assert(!memcmp(m->classes.nodes, n->classes.nodes, m->classes.node_count * sizeof(*m->classes.nodes)));
    assert(!m->classes.identities == !n->classes.identities);
    if (m->classes.identities) assert(!memcmp(m->classes.identities, n->classes.identities, m->classes.count * 32));
    for (size_t i = 0; i < m->classes.count; i++) assert(!strcmp(m->names[i], n->names[i]));
    assert(m->compiled->resource_count == n->compiled->resource_count && m->implicit_count == n->implicit_count);
    for (size_t i = 0; i < m->compiled->resource_count; i++) {
        const sh_compiled_resource *p = m->compiled->resources + i, *q = n->compiled->resources + i;
        assert(!strcmp(p->engine_path, q->engine_path) && !strcmp(p->type ? p->type : "", q->type ? q->type : "") &&
            !strcmp(p->name ? p->name : "", q->name ? q->name : ""));
        assert(p->body_length == q->body_length && (!p->body_length || !memcmp(p->body, q->body, p->body_length)));
        assert(p->bound_dependency_count == q->bound_dependency_count);
        for (size_t k = 0; k < p->bound_dependency_count; k++) assert(!strcmp(p->bound_dependencies[k], q->bound_dependencies[k]));
        assert(replay_owners_same(&p->owners, &q->owners) && replay_owners_same(&p->gameplay_owners, &q->gameplay_owners));
        assert(p->native_original == q->native_original && p->baseline_known == q->baseline_known);
    }
    for (size_t i = 0; i < m->implicit_count; i++)
        assert(m->implicit[i].owner == n->implicit[i].owner && m->implicit[i].target == n->implicit[i].target &&
            !strcmp(m->implicit[i].slot, n->implicit[i].slot));
    assert(a->calls.count == b->calls.count);
    for (size_t i = 0; i < a->calls.count; i++) assert(!strcmp(a->calls.entries[i], b->calls.entries[i]));
    assert(a->link.reads == b->link.reads);
}
/* 1 when one of unit's files binds scope's family/name. */
static int replay_edge(const replay_run *run, size_t unit, size_t scope, const char *family, const char *name)
{
    const sh_package_link_unit *u = run->link.inputs->units + unit;
    size_t target = sh_package_link_inputs_find(run->link.inputs, scope, family, name);
    for (size_t j = 0; target != SIZE_MAX && j < u->file_count; j++) {
        const sh_package_link_edge *sites = NULL; size_t count = 0;
        if (!sh_package_source_link_sites(run->source, unit, u->files[j], &sites, &count)) continue;
        for (size_t k = 0; k < count; k++) if (sites[k].target == target) return 1;
    }
    return 0;
}
static void replay_counts(const replay_run *run, size_t *replayed, size_t *recorded, size_t *declined)
{
    char counters[512]; size_t units = 0;
    sh_package_source_link_counters(run->source, counters, sizeof(counters));
    assert(sscanf(counters, "source link %zu units discovered, %zu replayed from another scope (%zu recorded, %zu declined)",
        &units, replayed, recorded, declined) == 4 && units);
}
/* File binds of a capture, replays of an earlier capture's recordings, and
 * what became of the memo ("kept", "renewed", "busy" or "unused"). */
static void memo_counts(const replay_run *run, size_t *binds, size_t *retained, char memo[16])
{
    char counters[1024]; const char *at;
    sh_package_source_link_counters(run->source, counters, sizeof(counters));
    at = strstr(counters, "); ");
    assert(at && sscanf(at, "); %zu file binds", binds) == 1);
    at = strstr(counters, " ms; ");
    assert(at && sscanf(at, " ms; %zu replayed from earlier compiles (memo %15[a-z])", retained, memo) == 2);
}
/* Separate captures sharing one memo (sh_package_source_memo), each checked
 * against a capture of the same inputs without it (replay_same: inputs, bytes,
 * sites, identities, classes, outputs, native calls, original reads). An
 * unchanged capture rebinds nothing. An edited, added or removed supplied file
 * rebinds only the readers that read it. A stock original, or a renderparm
 * declaration a material reads, changing behind the same reader callback and
 * the same (absent) catalog is read and bound afresh. Native reflection
 * changing at the same addresses renews the memo, and so does changing back. */
static void test_source_memo(fixture *native, sh_decl_registry_source registry)
{
    const char *seed = "{ class = \"Entity\"; edit = { target = \"replay/child\"; } }";
    const char *figure = "{ init { inherit \"replay/beast\" } }";
    const char *root = "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }";
    const char *base = "{ init { calcRefBoundsFromJoints 0 } }";
    const char *modifier = "{ edit = { target = \"parent-only\"; } }";
    const char *alternate = "{ class = \"AlternateEntity\"; edit = {} }";
    const char *edited = "{ class = \"Entity\"; edit = {} }";
    const char *skin = "{ tex image/snap label image/body\n}";
    const char *extra = "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }";
    sh_package_sources inventory = {0};
    sh_compiled_resource files[REPLAY_SCOPES][8];
    sh_package_compilation compiled[REPLAY_SCOPES];
    const sh_package_compilation *scopes[REPLAY_SCOPES];
    sh_package_source_memo *memo = sh_package_source_memo_open();
    assert(memo);
    memset(compiled, 0, sizeof(compiled));
    for (size_t i = 0; i < REPLAY_SCOPES; i++) {
        size_t count = 0;
        files[i][count++] = link_decl("replay/seed", "generated/decls/entitydef/replay/seed.decl", seed);
        files[i][count] = link_decl("replay/figure", "generated/decls/md6def/replay/figure.decl", figure);
        files[i][count++].type = "md6def";
        if (i == REPLAY_ROOT) {
            files[i][count++] = link_decl("root", "generated/decls/entitydef/root.decl", root);
            files[i][count] = link_decl("replay/base", "generated/decls/md6def/replay/base.decl", base);
            files[i][count++].type = "md6def";
        }
        if (i == REPLAY_MOD) {
            files[i][count] = link_decl("replay/mod", "generated/decls/actormodifier/replay/mod.decl", modifier);
            files[i][count++].type = "actormodifier";
        }
        if (i == REPLAY_PARENT)
            files[i][count++] = link_decl("replay/parent", "generated/decls/entitydef/replay/parent.decl", alternate);
        if (i == REPLAY_LATE) {
            files[i][count] = link_decl("replay/skin", "generated/decls/material/replay/skin.decl", skin);
            files[i][count++].type = "material";
        }
        qsort(files[i], count, sizeof(files[i][0]), replay_path_order);
        compiled[i].resources = files[i]; compiled[i].resource_count = count; compiled[i].sources = &inventory;
        scopes[i] = compiled + i;
    }
    /* AlternateEntity's alternate field: its reflected type decides whether a
     * child of an alternate parent references replay/other. */
    uintptr_t alternate_row = 0, alternate_type = 0;
    {
        uintptr_t container, records;
        memcpy(&container, native->reflection, 8); memcpy(&records, (char *)container + 0x20, 8);
        memcpy(&alternate_row, (char *)records + 18 * 0x38 + 0x20, 8);
        memcpy(&alternate_type, (const char *)alternate_row, 8);
        assert(!strcmp((const char *)alternate_type, "idDeclEntityDef"));
    }
    uintptr_t retyped = (uintptr_t)string(native, "idDeclActorModifier");
    for (int step = 0; step < 11; step++) {
        size_t binds = 0, retained = 0, unmemo = 0, unretained = 0; char state[16] = "", unstate[16] = "";
        switch (step) {
        case 2: memcpy((void *)alternate_row, &retyped, 8); break; /* Reflection, same addresses. */
        case 3: memcpy((void *)alternate_row, &alternate_type, 8); break;
        case 5: /* Edited: the changed parent's own scope rebinds its readers. */
            for (size_t k = 0; k < compiled[REPLAY_PARENT].resource_count; k++)
                if (!strcmp(files[REPLAY_PARENT][k].engine_path, "generated/decls/entitydef/replay/parent.decl"))
                    files[REPLAY_PARENT][k].body = (unsigned char *)edited, files[REPLAY_PARENT][k].body_length = strlen(edited);
            break;
        case 6: /* Added: a new seed in one scope. */
            files[REPLAY_LATE][compiled[REPLAY_LATE].resource_count++] =
                link_decl("replay/extra", "generated/decls/entitydef/replay/extra.decl", extra);
            qsort(files[REPLAY_LATE], compiled[REPLAY_LATE].resource_count, sizeof(files[0][0]), replay_path_order);
            break;
        case 7: /* Removed: the modifier its scope supplied. */
            for (size_t k = 0; k < compiled[REPLAY_MOD].resource_count; k++)
                if (!strcmp(files[REPLAY_MOD][k].engine_path, "generated/decls/actormodifier/replay/mod.decl")) {
                    files[REPLAY_MOD][k] = files[REPLAY_MOD][--compiled[REPLAY_MOD].resource_count]; break;
                }
            qsort(files[REPLAY_MOD], compiled[REPLAY_MOD].resource_count, sizeof(files[0][0]), replay_path_order);
            break;
        case 8: g_replay_child_changed = 1; break; /* Stock bytes behind the same callback. */
        case 9: g_replay_label_texture = 1; break; /* A render parameter a material reads. */
        default: break;
        }
        replay_run *with = calloc(1, sizeof(*with)), *without = calloc(1, sizeof(*without));
        assert(with && without);
        replay_open_memo(native, registry, scopes, &inventory, 1, memo, with);
        replay_open(native, registry, scopes, &inventory, 1, without);
        replay_same(with, without);
        memo_counts(with, &binds, &retained, state);
        memo_counts(without, &unmemo, &unretained, unstate);
        assert(!unretained && !strcmp(unstate, "unused"));
        size_t child[REPLAY_SCOPES], skin_unit = sh_package_link_inputs_find(with->link.inputs, REPLAY_LATE, "material", "replay/skin");
        for (size_t kind = 0; kind < REPLAY_SCOPES; kind++) {
            child[kind] = sh_package_link_inputs_find(with->link.inputs, kind, "entitydef", "replay/child");
            assert(child[kind] != SIZE_MAX);
        }
        assert(skin_unit != SIZE_MAX);
        int other = replay_edge(with, child[REPLAY_PARENT], REPLAY_PARENT, "entitydef", "replay/other"), expected = 1;
        switch (step) {
        case 0: expected = !retained && binds <= unmemo && !strcmp(state, "kept") && other; break;
        case 1: case 4: case 10: expected = !binds && retained && !strcmp(state, "kept"); break;
        case 2: expected = !retained && binds <= unmemo && !strcmp(state, "renewed") && !other; break;
        case 3: expected = !retained && binds <= unmemo && !strcmp(state, "renewed") && other; break;
        case 5: expected = retained && binds && binds < unmemo && !other &&
            !sh_package_source_link_unit_replayed(with->source, child[REPLAY_PARENT]) &&
            sh_package_source_link_unit_replayed(with->source, child[REPLAY_PLAIN]); break;
        case 6: {
            size_t added = sh_package_link_inputs_find(with->link.inputs, REPLAY_LATE, "entitydef", "replay/extra");
            expected = added != SIZE_MAX && !sh_package_source_link_unit_replayed(with->source, added) &&
                sh_package_source_link_unit_replayed(with->source, child[REPLAY_LATE]) && binds && binds < unmemo;
            break;
        }
        case 7: /* It now reads what a stock child reads, and may replay that recording. */
            expected = !replay_edge(with, child[REPLAY_MOD], REPLAY_MOD, "actormodifier", "replay/mod") && binds < unmemo; break;
        case 8: expected = !sh_package_source_link_unit_replayed(with->source, child[REPLAY_PLAIN]) &&
            replay_edge(with, child[REPLAY_PLAIN], REPLAY_PLAIN, "entitydef", "parent-only"); break;
        case 9: expected = !sh_package_source_link_unit_replayed(with->source, skin_unit) &&
            replay_edge(with, skin_unit, REPLAY_LATE, "image", "image/body"); break;
        }
        if (!expected) fprintf(stderr, "source memo step %d: binds %zu of %zu, %zu from earlier compiles, memo %s, other %d\n",
            step, binds, unmemo, retained, state, other);
        assert(expected);
        replay_close(with); replay_close(without);
    }
    g_replay_child_changed = g_replay_label_texture = 0;
    sh_package_source_memo_close(memo);
}
static void test_source_link_replay(fixture *native, sh_decl_registry_source registry)
{
    const char *seed = "{ class = \"Entity\"; edit = { target = \"replay/child\"; } }";
    const char *figure = "{ init { inherit \"replay/beast\" } }";
    const char *root = "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }";
    const char *base = "{ init { calcRefBoundsFromJoints 0 } }";
    const char *modifier = "{ edit = { target = \"parent-only\"; } }";
    const char *alternate = "{ class = \"AlternateEntity\"; edit = {} }";
    /* Discovery follows scope order: the first scope to reach a unit records
     * it. An unchanged scope first, the reverse order, changed scopes first
     * (a recording that consulted a path its own scope supplies is never kept),
     * and last the scope with the changed md6 parent first. */
    static const size_t orders[4][REPLAY_SCOPES] = {
        {REPLAY_PLAIN, REPLAY_ROOT, REPLAY_MOD, REPLAY_PARENT, REPLAY_LATE},
        {REPLAY_LATE, REPLAY_PARENT, REPLAY_MOD, REPLAY_ROOT, REPLAY_PLAIN},
        {REPLAY_PARENT, REPLAY_MOD, REPLAY_PLAIN, REPLAY_ROOT, REPLAY_LATE},
        {REPLAY_ROOT, REPLAY_PARENT, REPLAY_MOD, REPLAY_PLAIN, REPLAY_LATE},
    };
    /* Whose child replays, by scope kind (PLAIN ROOT MOD PARENT LATE): a
     * scope supplying a path the recording looked up (root, modifier or
     * parent) declines. In the last round the first scope supplies the
     * alternate parent too, so only the last scope keeps a recording. */
    static const int replayed[4][REPLAY_SCOPES] = {
        {0, 0, 0, 0, 1}, {1, 0, 0, 0, 0}, {0, 0, 0, 0, 1}, {0, 0, 0, 0, 0},
    };
    static const size_t declines[4] = {3, 3, 1, 0};
    sh_package_sources inventory = {0};
    sh_compiled_resource files[REPLAY_SCOPES][5], changed_parent;
    sh_package_compilation compiled[REPLAY_SCOPES];
    memset(compiled, 0, sizeof(compiled));
    for (size_t i = 0; i < REPLAY_SCOPES; i++) {
        size_t count = 0;
        files[i][count++] = link_decl("replay/seed", "generated/decls/entitydef/replay/seed.decl", seed);
        files[i][count] = link_decl("replay/figure", "generated/decls/md6def/replay/figure.decl", figure);
        files[i][count++].type = "md6def";
        if (i == REPLAY_ROOT) {
            files[i][count++] = link_decl("root", "generated/decls/entitydef/root.decl", root);
            files[i][count] = link_decl("replay/base", "generated/decls/md6def/replay/base.decl", base);
            files[i][count++].type = "md6def";
        }
        if (i == REPLAY_MOD) {
            files[i][count] = link_decl("replay/mod", "generated/decls/actormodifier/replay/mod.decl", modifier);
            files[i][count++].type = "actormodifier";
        }
        if (i == REPLAY_PARENT)
            files[i][count++] = link_decl("replay/parent", "generated/decls/entitydef/replay/parent.decl", alternate);
        qsort(files[i], count, sizeof(files[i][0]), replay_path_order);
        compiled[i].resources = files[i]; compiled[i].resource_count = count; compiled[i].sources = &inventory;
    }
    changed_parent = link_decl("replay/parent", "generated/decls/entitydef/replay/parent.decl", alternate);
    for (int round = 0; round < 4; round++) {
        const size_t *order = orders[round];
        const sh_package_compilation *scopes[REPLAY_SCOPES];
        size_t scope_of[REPLAY_SCOPES], child[REPLAY_SCOPES], beast[REPLAY_SCOPES];
        if (round == 3) {
            /* A new link keeps no earlier recording: the first scope now also
             * supplies the alternate parent. */
            files[REPLAY_PLAIN][compiled[REPLAY_PLAIN].resource_count++] = changed_parent;
            qsort(files[REPLAY_PLAIN], compiled[REPLAY_PLAIN].resource_count, sizeof(files[REPLAY_PLAIN][0]), replay_path_order);
        }
        for (size_t s = 0; s < REPLAY_SCOPES; s++) { scopes[s] = compiled + order[s]; scope_of[order[s]] = s; }
        replay_run *on = calloc(1, sizeof(*on)), *off = calloc(1, sizeof(*off));
        assert(on && off);
        replay_open(native, registry, scopes, &inventory, 1, on);
        replay_open(native, registry, scopes, &inventory, 0, off);
        replay_same(on, off);
        for (size_t kind = 0; kind < REPLAY_SCOPES; kind++) {
            size_t scope = scope_of[kind], calls = 0;
            int alternate_parent = kind == REPLAY_PARENT || (round == 3 && kind == REPLAY_PLAIN);
            char call[96];
            child[kind] = sh_package_link_inputs_find(on->link.inputs, scope, "entitydef", "replay/child");
            beast[kind] = sh_package_link_inputs_find(on->link.inputs, scope, "md6def", "replay/beast");
            assert(child[kind] != SIZE_MAX && beast[kind] != SIZE_MAX);
            /* Each scope keeps its own bindings: its root, its supplied
             * modifier, the fields its own parent's class makes references and
             * its own md6 parent (stock or changed), replayed or not. */
            assert(replay_edge(on, child[kind], scope, "entitydef", "root") == !alternate_parent);
            assert(replay_edge(on, child[kind], scope, "entitydef", "replay/other") == alternate_parent);
            assert(replay_edge(on, child[kind], scope, "actormodifier", "replay/mod") == (kind == REPLAY_MOD));
            assert(replay_edge(on, beast[kind], scope, "md6def", "replay/base"));
            /* ... and its own native callbacks. */
            snprintf(call, sizeof(call), "%zu entitydef replay/child", scope);
            for (size_t c = 0; c < on->calls.count; c++) calls += !strcmp(on->calls.entries[c], call);
            snprintf(call, sizeof(call), "%zu md6def replay/beast", scope);
            for (size_t c = 0; c < on->calls.count; c++) calls += !strcmp(on->calls.entries[c], call);
            assert(calls == 2);
            assert(!sh_package_source_link_unit_replayed(off->source, child[kind]) &&
                !sh_package_source_link_unit_replayed(off->source, beast[kind]));
            if (sh_package_source_link_unit_replayed(on->source, child[kind]) != replayed[round][kind])
                fprintf(stderr, "replay round %d scope kind %zu: child replayed %d\n", round, kind, !replayed[round][kind]);
            assert(sh_package_source_link_unit_replayed(on->source, child[kind]) == replayed[round][kind]);
            /* The beast consults no scoped path: every later scope replays it. */
            assert(sh_package_source_link_unit_replayed(on->source, beast[kind]) == (scope != 0));
        }
        /* Equal bodies with different bound dependencies stay variants. */
        size_t stock = on->out.classes.nodes[child[REPLAY_LATE]], stock_beast = on->out.classes.nodes[beast[REPLAY_LATE]];
        assert(on->out.classes.nodes[child[REPLAY_ROOT]] != stock && on->out.classes.nodes[child[REPLAY_MOD]] != stock &&
            on->out.classes.nodes[child[REPLAY_PARENT]] != stock);
        assert((on->out.classes.nodes[child[REPLAY_PLAIN]] == stock) == (round < 3));
        assert(on->out.classes.nodes[beast[REPLAY_ROOT]] != stock_beast);
        for (size_t kind = 0; kind < REPLAY_SCOPES; kind++)
            if (kind != REPLAY_ROOT) assert(on->out.classes.nodes[beast[kind]] == stock_beast);
        /* One payload identity per captured original, in every scope. */
        const sh_package_link_unit *a = on->link.inputs->units + beast[REPLAY_ROOT], *b = on->link.inputs->units + beast[REPLAY_LATE];
        sh_package_file_identity x, y, z;
        assert(a->files[0] == b->files[0] && a->files[0]->body);
        assert(sh_package_source_link_identity(on->source, beast[REPLAY_ROOT], a->files[0], &x, NULL, 0) &&
            sh_package_source_link_identity(on->source, beast[REPLAY_LATE], b->files[0], &y, NULL, 0) &&
            sh_package_bytes_identity(a->files[0]->body, a->files[0]->body_length, &z) &&
            !memcmp(&x, &y, sizeof(x)) && !memcmp(&x, &z, sizeof(x)));
        size_t replays, recorded, declined;
        replay_counts(on, &replays, &recorded, &declined);
        if (replays < 4 || recorded < 2 || declined < declines[round])
            fprintf(stderr, "replay round %d: %zu replayed, %zu recorded, %zu declined\n", round, replays, recorded, declined);
        assert(replays >= 4 && recorded >= 2 && declined >= declines[round]);
        replay_counts(off, &replays, &recorded, &declined);
        assert(!replays && !recorded && !declined);
        replay_close(on); replay_close(off);
    }
}

typedef struct unknown_bound { int calls; } unknown_bound;
static int bound_unknown(void *context, const char *family, const char *name, int *exists)
{ (void)family; (void)name; (void)exists; ((unknown_bound *)context)->calls++; return 0; }
static int bound_absent(void *context, const char *family, const char *name, int *exists)
{ (void)family; (void)name; ((unknown_bound *)context)->calls++; *exists = 0; return 1; }
/* A composed body copies a stock-style reference the engine defaults (no
 * source anywhere, absence captured in discovery): it keeps its bytes when the
 * bound outputs do not claim the name, but a name the bound outputs positively
 * lack still refuses the composition. */
static void test_composed_stock_default(fixture *native, sh_decl_registry_source registry)
{
    const char *root = "{ class = \"Entity\"; edit = { target = \"stock/gone\"; } }";
    sh_package_sources inventory = {0};
    sh_compiled_resource files[1];
    sh_package_compilation compiled = {0};
    const sh_package_compilation *scope = &compiled;
    source_link_fixture f = {native};
    char error[1024]; size_t length = 0;
    files[0] = link_decl("root", "generated/decls/entitydef/root.decl", root);
    compiled.resources = files; compiled.resource_count = 1; compiled.sources = &inventory;
    sh_package_source_link *source = sh_package_source_link_open(&scope, 1, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    if (!source) fprintf(stderr, "composed default source: %s\n", error);
    assert(source);
    size_t unit = sh_package_link_inputs_find(sh_package_source_link_inputs(source), 0, "entitydef", "root");
    unknown_bound bound = {0}; bind_fixture capture = {0};
    unsigned char *out = sh_package_source_link_bind_derived(source, unit, (const unsigned char *)root, strlen(root),
        bind_reference, &capture, bound_unknown, &bound, &length, error, sizeof(error));
    if (!out) fprintf(stderr, "composed default: %s\n", error);
    assert(out && bound.calls && !capture.count && length == strlen(root) && !memcmp(out, root, length)); free(out);
    assert(!sh_package_source_link_bind_derived(source, unit, (const unsigned char *)root, strlen(root),
        bind_reference, &capture, bound_absent, &bound, &length, error, sizeof(error)));
    sh_package_source_link_close(source);
}
static void test_source_link(fixture *native, sh_decl_registry_source registry)
{
    const char *root = "{ class = \"Entity\"; edit = { target = \"child\"; } }";
    const char *child = "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }";
    const char *cycle = "{ class = \"Entity\"; edit = { target = \"root\"; } }";
    sh_package_sources inventory = {0};
    sh_compiled_resource files[3][2];
    sh_package_compilation compiled[3] = {0};
    const sh_package_compilation *scopes[3] = {compiled, compiled + 1, compiled + 2};
    sh_package_source_link *source;
    sh_package_link_output out = {0};
    source_link_fixture f = {native};
    sh_package_link_adapter adapter = {&f, NULL, NULL, link_assign};
    sh_package_source_link_native admission = {&f, link_admit};
    char error[1024];
    for (size_t i = 0; i < 3; i++) {
        files[i][0] = link_decl("child", "generated/decls/entitydef/child.decl", i == 1 ? cycle : child);
        files[i][1] = link_decl("root", "generated/decls/entitydef/root.decl", root);
        compiled[i].resources = files[i]; compiled[i].resource_count = 2;
        compiled[i].sources = &inventory;
    }
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    if (!source) fprintf(stderr, "source link: %s\n", error);
    assert(source);
    f.inputs = sh_package_source_link_inputs(source);
    assert(f.inputs->count == 8 && f.reads == 1);
    size_t roots[3];
    for (size_t i = 0; i < 3; i++) roots[i] = sh_package_link_inputs_find(f.inputs, i, "entitydef", "root");
    /* No original can be reread after capture. The link still succeeds using
     * the frozen snapshot, including one dependency shared by two scopes. */
    native->unreadable = "generated/decls/entitydef/parent-only.decl";
    {
        sh_package_link_classes identified = {0}; size_t target = SIZE_MAX;
        assert(sh_package_source_link_identify(source, &identified, error, sizeof(error)));
        assert(identified.count == 5 && identified.node_count == 8 && f.reads == 1);
        assert(identified.nodes[roots[0]] == identified.nodes[roots[2]]);
        assert(identified.nodes[roots[0]] != identified.nodes[roots[1]]);
        for (size_t i = 0; i < 3; i++) {
            assert(sh_package_source_link_resolve_unit(source, roots[i], "entitydef", "child",
                &target, error, sizeof(error)));
            assert(target == sh_package_link_inputs_find(f.inputs, i, "entitydef", "child"));
        }
        assert(!sh_package_source_link_resolve_unit(source, roots[0], "entitydef", "not-captured",
            &target, error, sizeof(error)) && target == SIZE_MAX);
        assert(!sh_package_source_link_resolve_unit(source, SIZE_MAX, "entitydef", "child",
            &target, error, sizeof(error)) && target == SIZE_MAX);
        sh_package_link_classes_free(&identified);
    }
    {
        const unsigned char *derived = (const unsigned char *)"{ class = \"Entity\"; edit = { target = \"bound-test-token\"; } }";
        derived_admission exists = {0, 1, 1}; bind_fixture capture = {0}; size_t length;
        unsigned char *bound = sh_package_source_link_bind_derived(source, roots[0], derived, strlen((char *)derived),
            bind_reference, &capture, admit_derived, &exists, &length, error, sizeof(error));
        assert(bound && exists.calls && capture.count == 1 && !strcmp((char *)bound, (char *)derived)); free(bound);
        size_t calls = exists.calls;
        /* A successful derived call grants nothing to the ordinary reader. Its
         * frozen snapshot never observed this name, so absence is unproven. */
        assert(!sh_package_source_link_bind_unit(source, roots[0], derived, strlen((char *)derived),
            bind_reference, &capture, &length, error, sizeof(error)));
        assert(strstr(error, "uncaptured original"));
        assert(exists.calls == calls);
        exists.result = -1;
        assert(!sh_package_source_link_bind_derived(source, roots[0], derived, strlen((char *)derived),
            bind_reference, &capture, admit_derived, &exists, &length, error, sizeof(error)));
        calls = exists.calls;
        bound = sh_package_source_link_bind_unit(source, roots[0], (const unsigned char *)root, strlen(root),
            bind_reference, &capture, &length, error, sizeof(error));
        assert(bound && exists.calls == calls && !strcmp((char *)bound, root)); free(bound);
        exists.result = 1; exists.exists = 0;
        assert(!sh_package_source_link_bind_derived(source, roots[0], derived, strlen((char *)derived),
            bind_reference, &capture, admit_derived, &exists, &length, error, sizeof(error)));
        assert(f.reads == 1);
    }
    assert(sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 5 && out.compiled->resource_count == 5 && f.reads == 1);
    assert(out.classes.nodes[roots[0]] == out.classes.nodes[roots[2]]);
    assert(out.classes.nodes[roots[0]] != out.classes.nodes[roots[1]]);
    const char *child_alias = out.names[out.classes.nodes[sh_package_link_inputs_find(f.inputs, 0, "entitydef", "child")]];
    int found = 0;
    for (size_t i = 0; i < out.compiled->resource_count; i++) {
        const sh_compiled_resource *r = out.compiled->resources + i;
        if (!strcmp(r->name, out.names[out.classes.nodes[roots[0]]])) {
            assert(strstr((const char *)r->body, child_alias)); found++;
        }
    }
    assert(found == 1);
    const unsigned char *shared = sh_package_source_link_shared(source);
    assert(shared);
    for (size_t i = 0; i < f.inputs->count; i++) assert(shared[i]);
    sh_package_source_link_close(source);
    /* Compiled output owns its copies beyond the reader/original snapshot. */
    assert(out.compiled->resources[0].body && out.compiled->resources[0].native_name);
    sh_package_link_output_free(&out);
    native->unreadable = NULL;
    /* Source ownership seeds the dependency walk; cycles propagate private
     * gameplay only inside that scope. An equal peer stays independently shared. */
    files[1][0].gameplay_owners.bits = 1;
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    assert(source);
    f.inputs = sh_package_source_link_inputs(source); shared = sh_package_source_link_shared(source);
    for (size_t i = 0; i < f.inputs->count; i++) assert(shared[i] == (f.inputs->scopes[i] != 1));
    sh_package_source_link_close(source); files[1][0].gameplay_owners.bits = 0;
    /* A peer's source never fills a hole in this scope, even though it is one
     * of the same driver's other inputs. The absent name keeps the engine's
     * default binding: no edge to the peer's child is captured. */
    compiled[0].resources = files[0] + 1; compiled[0].resource_count = 1;
    source = sh_package_source_link_open(scopes, 2, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    if (!source) fprintf(stderr, "absent peer child: %s\n", error);
    assert(source);
    f.inputs = sh_package_source_link_inputs(source);
    assert(sh_package_link_inputs_find(f.inputs, 0, "entitydef", "child") == SIZE_MAX);
    assert(sh_package_link_inputs_find(f.inputs, 1, "entitydef", "child") != SIZE_MAX);
    assert(sh_package_link_inputs_find(f.inputs, 0, "entitydef", "root") != SIZE_MAX);
    sh_package_source_link_close(source);
    compiled[0].resources = files[0]; compiled[0].resource_count = 2;
    /* A campaign-only dependency cannot silently become shipped bundle data. */
    f.campaign = 1;
    source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    assert(!source && strstr(error, "campaign dependency")); f.campaign = 0;
    native->unreadable = "generated/decls/entitydef/parent-only.decl";
    admission.semantics = link_original_binding; f.certifications = 0;
    source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    assert(!source && strstr(error, "could not be captured") && !f.admissions);
    assert(!f.certifications); /* Implicit native data waits for complete source closure. */
    admission.semantics = NULL;
    native->unreadable = NULL;
    /* Engine-created leaves require explicit native admission and keep their
     * native name. Proven absence and a failed read are different outcomes. */
    files[0][0] = link_decl("root", "generated/decls/entitydef/root.decl",
        "{ class = \"Entity\"; edit = { image = \"synthetic/runtime-image\"; } }");
    compiled[0].resource_count = 1;
    source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    assert(source && f.admissions == 1);
    f.inputs = sh_package_source_link_inputs(source);
    assert(f.inputs->count == 2);
    assert(sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error)));
    size_t image = sh_package_link_inputs_find(f.inputs, 0, "image", "synthetic/runtime-image");
    assert(!strcmp(out.names[out.classes.nodes[image]], "synthetic/runtime-image"));
    sh_package_link_output_free(&out); sh_package_source_link_close(source);
    {
        /* The production native adapter admits verified intrinsic images as
         * external leaves. Equal roots and their shared image dedupe across
         * packages without manufacturing a cooked image file. */
        sh_package_library_native builtins = sh_package_native_assets_adapter(NULL);
        const char *body = "{ class = \"Entity\"; edit = { image = \"_white\"; } }";
        files[0][0] = files[1][0] = link_decl("root", "generated/decls/entitydef/root.decl", body);
        compiled[1].resource_count = 1;
        source = sh_package_source_link_open(scopes, 2, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &builtins.source, error, sizeof(error));
        assert(source); f.inputs = sh_package_source_link_inputs(source); assert_discovered_sites(source);
        image = sh_package_link_inputs_find(f.inputs, 0, "image", "_white");
        size_t peer = sh_package_link_inputs_find(f.inputs, 1, "image", "_white");
        assert(image != SIZE_MAX && peer != SIZE_MAX && !f.inputs->units[image].file_count);
        sh_package_link_adapter names = {(void *)f.inputs, NULL, NULL, sh_package_native_names_assign};
        assert(sh_package_source_link_compile(source, &inventory, &names, NULL, NULL, &out, error, sizeof(error)));
        assert(out.classes.nodes[image] == out.classes.nodes[peer] && out.classes.count == 2);
        assert(!strcmp(out.names[out.classes.nodes[image]], "_white") && out.compiled->resource_count == 1);
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
        /* Zero source files do not make different native objects equivalent.
         * Their identity also distinguishes otherwise identical consumers. */
        files[1][0] = link_decl("root", "generated/decls/entitydef/root.decl",
            "{ class = \"Entity\"; edit = { image = \"_black\"; } }");
        source = sh_package_source_link_open(scopes, 2, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &builtins.source, error, sizeof(error));
        assert(source); f.inputs = sh_package_source_link_inputs(source); names.context = (void *)f.inputs; assert_discovered_sites(source);
        image = sh_package_link_inputs_find(f.inputs, 0, "image", "_white");
        peer = sh_package_link_inputs_find(f.inputs, 1, "image", "_black");
        assert(image != SIZE_MAX && peer != SIZE_MAX);
        assert(sh_package_source_link_compile(source, &inventory, &names, NULL, NULL, &out, error, sizeof(error)));
        assert(out.classes.count == 4 && out.classes.nodes[image] != out.classes.nodes[peer]);
        assert(!strcmp(out.names[out.classes.nodes[image]], "_white"));
        assert(!strcmp(out.names[out.classes.nodes[peer]], "_black") && out.compiled->resource_count == 2);
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
        /* The intrinsic name does not designate a cooked file. An explicit
         * bimage reference is separate even if its basename resembles one. */
        f.image_path = "generated/image/_white.bimage";
        source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &builtins.source, error, sizeof(error));
        assert(source); f.inputs = sh_package_source_link_inputs(source); names.context = (void *)f.inputs; assert_discovered_sites(source);
        image = sh_package_link_inputs_find(f.inputs, 0, "image", "_white");
        assert(!f.inputs->units[image].file_count);
        sh_package_source_link_close(source);
        files[0][0] = link_decl("root", "generated/decls/entitydef/root.decl",
            "{ class = \"Entity\"; edit = { image = \"generated/image/_white.bimage\"; } }");
        source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &builtins.source, error, sizeof(error));
        assert(source); f.inputs = sh_package_source_link_inputs(source); names.context = (void *)f.inputs; assert_discovered_sites(source);
        image = sh_package_link_inputs_find(f.inputs, 0, "image", "generated/image/_white.bimage");
        assert(image != SIZE_MAX && f.inputs->units[image].file_count == 1);
        assert(sh_package_source_link_compile(source, &inventory, &names, NULL, NULL, &out, error, sizeof(error)));
        assert(strcmp(out.names[out.classes.nodes[image]], "_white") && out.compiled->resource_count == 2);
        sh_package_link_output_free(&out); sh_package_source_link_close(source); f.image_path = NULL;
        /* Failed reads are never treated as engine-created source absence. */
        native->unreadable = "generated/image/_white.bimage";
        source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &builtins.source, error, sizeof(error));
        assert(!source && strstr(error, "could not be captured")); native->unreadable = NULL;
        files[0][0] = link_decl("root", "generated/decls/entitydef/root.decl",
            "{ class = \"Entity\"; edit = { image = \"_not_an_intrinsic_image\"; } }");
        source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &builtins.source, error, sizeof(error));
        assert(!source && strstr(error, "no admitted native source"));
    }
    /* Image dependencies without catalog rows use native physical naming,
     * never generated/decls/image. The same captured bytes serve both scopes. */
    static const char *images[][2] = {
        {"textures/example.tga", "generated/image/textures/example.bimage"},
        {"textures/example.tga$borderclamp$alpha", "generated/image/textures/example.tga$borderclamp$alpha.bimage"},
        {"generated/swf/example.bimage", "generated/swf/example.bimage"}
    };
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        char entity[256];
        snprintf(entity, sizeof(entity), "{ class = \"Entity\"; edit = { image = \"%s\"; } }", images[i][0]);
        files[0][0] = files[1][0] = link_decl("root", "generated/decls/entitydef/root.decl", entity);
        compiled[1].resource_count = 1;
        f.image_path = images[i][1]; f.reads = 0;
        source = sh_package_source_link_open(scopes, 2, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        if (!source) fprintf(stderr, "image source link: %s\n", error);
        assert(source && f.reads == 1 && f.admissions == 1);
        f.inputs = sh_package_source_link_inputs(source);
        image = sh_package_link_inputs_find(f.inputs, 0, "image", images[i][0]);
        assert(image != SIZE_MAX && f.inputs->units[image].file_count == 1);
        assert(!strcmp(f.inputs->units[image].files[0]->engine_path, images[i][1]));
        f.image_path = NULL; /* Further external reads cannot supply the file. */
        int linked = sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error));
        if (!linked) fprintf(stderr, "image source compile: %s\n", error);
        assert(linked);
        assert(f.reads == 1 && out.classes.count == 2 && out.compiled->resource_count == 2);
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
    }
    /* A package can supply an image absent from both game catalogs. Discover
     * its typed owner first so the explicit file seed does not create a second
     * copy beside the normal source-name reference. Equal packages dedupe;
     * different image bytes branch their referring entities as well. */
    for (int different = 0; different < 2; different++) {
        const char *image_path = "generated/image/textures/custom/toybox_icon.bimage";
        const char *entity = "{ class = \"Entity\"; edit = { image = \"textures/custom/toybox_icon.tga\"; } }";
        for (size_t i = 0; i < 2; i++) {
            files[i][0] = link_decl("root", "generated/decls/entitydef/root.decl", entity);
            files[i][1] = (sh_compiled_resource){0};
            files[i][1].engine_path = (char *)image_path;
            files[i][1].body = (unsigned char *)(different && i ? "green image bytes" : "red image bytes");
            files[i][1].body_length = strlen((const char *)files[i][1].body);
            files[i][1].source = SIZE_MAX; compiled[i].resource_count = 2;
        }
        source = sh_package_source_link_open(scopes, 2, NULL, link_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        if (!source) fprintf(stderr, "supplied image discovery: %s\n", error);
        assert(source); f.inputs = sh_package_source_link_inputs(source); assert_discovered_sites(source);
        assert(f.inputs->count == 4);
        sh_package_link_adapter names = {(void *)f.inputs, NULL, NULL, sh_package_native_names_assign};
        assert(sh_package_source_link_compile(source, &inventory, &names, NULL, NULL, &out, error, sizeof(error)));
        assert(out.classes.count == (different ? 4 : 2));
        assert(out.compiled->resource_count == out.classes.count);
        size_t first = sh_package_link_inputs_find(f.inputs, 0, "image", "textures/custom/toybox_icon.tga");
        size_t second = sh_package_link_inputs_find(f.inputs, 1, "image", "textures/custom/toybox_icon.tga");
        assert(first != SIZE_MAX && second != SIZE_MAX);
        assert((out.classes.nodes[first] == out.classes.nodes[second]) == !different);
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
    }
    /* A supplied but currently unreferenced image still belongs to the compiled
     * authoring library and can be used by a later authored entity. */
    compiled[0].resources = files[0] + 1; compiled[0].resource_count = 1;
    source = sh_package_source_link_open(scopes, 1, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    assert(source); f.inputs = sh_package_source_link_inputs(source); assert_discovered_sites(source);
    assert(f.inputs->count == 1 && !strcmp(f.inputs->units[0].family, "image"));
    assert(!strcmp(f.inputs->units[0].name, files[0][1].engine_path));
    sh_package_link_output_free(&out); sh_package_source_link_close(source);
    compiled[0].resources = files[0];

    /* Custom grammars participate in the same walk. Reading a parameter to
     * interpret a material and subsequently linking that parameter must use
     * one captured original, not two potentially different schema snapshots. */
    const char *materials[] = {"{ color {1,0,0,1} }", "{ color {0,1,0,1} }", "{ color {1,0,0,1} }"};
    for (size_t i = 0; i < 3; i++) {
        files[i][0] = link_decl("sample", "generated/decls/material/sample.decl", materials[i]);
        files[i][0].type = "material";
        compiled[i].resources = files[i]; compiled[i].resource_count = 1;
    }
    f.reads = 0;
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, NULL, error, sizeof(error));
    if (!source) fprintf(stderr, "material source link: %s\n", error);
    assert(source && f.reads == 1);
    f.inputs = sh_package_source_link_inputs(source);
    assert(f.inputs->count == 6);
    assert(sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 3 && out.compiled->resource_count == 3 && f.reads == 1);
    for (size_t i = 0; i < 3; i++) roots[i] = sh_package_link_inputs_find(f.inputs, i, "material", "sample");
    assert(out.classes.nodes[roots[0]] == out.classes.nodes[roots[2]]);
    assert(out.classes.nodes[roots[0]] != out.classes.nodes[roots[1]]);
    sh_package_link_output_free(&out); sh_package_source_link_close(source);
    /* Production names preserve the shared renderer ABI parameter while two
     * different materials get distinct paths and the third exact copy dedupes.
     * Native VMTR publication remains a separate runtime integration check. */
    admission.semantics = link_original_binding;
    f.certificate = 1; f.certifications = 0; f.reads = 0;
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    assert(source && f.reads == 2 && f.certifications == 6);
    f.inputs = sh_package_source_link_inputs(source);
    sh_package_link_adapter native_names = {(void *)f.inputs, NULL, NULL, sh_package_native_names_assign};
    assert(sh_package_source_link_compile(source, &inventory, &native_names, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 3 && out.compiled->resource_count == 3 && f.reads == 2);
    const sh_compiled_resource *parameter = sh_package_compilation_find(out.compiled, "generated/decls/renderparm/color.decl");
    assert(parameter && !strcmp(parameter->native_name, "color"));
    for (size_t i = 0; i < 3; i++) roots[i] = sh_package_link_inputs_find(f.inputs, i, "material", "sample");
    assert(out.classes.nodes[roots[0]] == out.classes.nodes[roots[2]]);
    assert(strcmp(out.names[out.classes.nodes[roots[0]]], out.names[out.classes.nodes[roots[1]]]));
    for (size_t i = 0; i < 3; i++) assert(!f.inputs->units[roots[i]].original_binding);
    sh_package_link_output_free(&out); sh_package_source_link_close(source);
    f.different_semantics = 1;
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    assert(source); f.inputs = sh_package_source_link_inputs(source); native_names.context = (void *)f.inputs; assert_discovered_sites(source);
    assert(sh_package_source_link_compile(source, &inventory, &native_names, NULL, NULL, &out, error, sizeof(error)));
    assert(out.classes.count == 4 && out.compiled->resource_count == 4);
    sh_package_link_output_free(&out); sh_package_source_link_close(source); f.different_semantics = 0;
    f.certificate = 0;
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    assert(source); f.inputs = sh_package_source_link_inputs(source); native_names.context = (void *)f.inputs; assert_discovered_sites(source);
    assert(!sh_package_source_link_compile(source, &inventory, &native_names, NULL, NULL, &out, error, sizeof(error)));
    assert(!out.compiled && strstr(error, "ABI identity"));
    sh_package_source_link_close(source);
    f.certificate = -1;
    source = sh_package_source_link_open(scopes, 3, NULL, link_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    assert(!source && strstr(error, "certification failed"));
    assert(!native->object_state_reads);
}
typedef struct map_occurrences { unsigned seen; int fail_second; } map_occurrences;
static int bind_occurrence(void *context, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement)
{
    map_occurrences *f = context;
    if (strncmp(name, "saved-", 6)) return 0;
    assert(site && site->owner && site->reference && !strcmp(family, "entitydef"));
    if (site->kind == SH_PACKAGE_USE_OBJECT && !strcmp(site->owner, "entity/0")) {
        if (!strcmp(site->reference, "/k:entityDef/k:inherit")) {
            assert(!strcmp(name, "saved-root")); f->seen |= 1; *replacement = "red-root";
        } else {
            assert(!strcmp(site->reference, "/k:entityDef/k:state/k:edit/k:target") && !strcmp(name, "saved-value"));
            f->seen |= 2; *replacement = "red-value";
        }
    } else if (site->kind == SH_PACKAGE_USE_OBJECT && !strcmp(site->owner, "entity/1")) {
        if (f->fail_second) return -1;
        if (!strcmp(site->reference, "/k:entityDef/k:inherit")) {
            assert(!strcmp(name, "saved-root")); f->seen |= 4; *replacement = "green-root";
        } else {
            assert(!strcmp(site->reference, "/k:entityDef/k:state/k:edit/k:alternate") && !strcmp(name, "saved-value"));
            f->seen |= 8; *replacement = "green-value";
        }
    } else {
        assert(site->kind == SH_PACKAGE_USE_WORLD && !strcmp(site->owner, "world") && !strcmp(name, "saved-value"));
        if (!strcmp(site->reference, "/k:worldA/k:value")) { f->seen |= 16; *replacement = "red-value"; }
        else { assert(!strcmp(site->reference, "/k:worldB/k:value")); f->seen |= 32; *replacement = "green-value"; }
    }
    return 1;
}
static void test_map_occurrences(fixture *f, sh_decl_registry_source registry)
{
    sh_package_sources sources = {0}; sh_package_compilation compiled = {0};
    sh_compiled_resource files[] = {
        link_decl("green-root", "generated/decls/entitydef/green-root.decl", "{ class = \"AlternateEntity\"; edit = {} }"),
        link_decl("green-value", "generated/decls/entitydef/green-value.decl", "{ class = \"Entity\"; edit = {} }"),
        link_decl("red-root", "generated/decls/entitydef/red-root.decl", "{ class = \"Entity\"; edit = {} }"),
        link_decl("red-value", "generated/decls/entitydef/red-value.decl", "{ class = \"Entity\"; edit = {} }")
    };
    compiled.sources = &sources; compiled.resources = files; compiled.resource_count = 4;
    const char *json = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"saved-root\","
        "\"state\":{\"edit\":{\"target\":\"saved-value\"}}}},{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"saved-root\",\"state\":{\"edit\":{\"alternate\":\"saved-value\"}}}}],"
        "\"worldA\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"saved-value\"},\"worldB\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"saved-value\"},"
        "\"note\":\"saved-root\x97\",\"editorVars\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"saved-value\"}}";
    const char *expected = "{\"entities\":[{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"red-root\","
        "\"state\":{\"edit\":{\"target\":\"red-value\"}}}},{\"uniqueId\":42,\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"inherit\":\"green-root\",\"state\":{\"edit\":{\"alternate\":\"green-value\"}}}}],"
        "\"worldA\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"red-value\"},\"worldB\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"green-value\"},"
        "\"note\":\"saved-root\x97\",\"editorVars\":{\"targetType\":\"idDeclEntityDef\",\"value\":\"saved-value\"}}";
    char error[1024]; size_t length; sh_package_source_binding result; map_occurrences calls = {0};
    sh_package_source_reader *reader = sh_package_source_reader_open(&compiled, NULL, original, f,
        registry, (uintptr_t)f->reflection, error, sizeof(error)); assert(reader);
    char *out = sh_package_source_bind_map_sites(reader, json, strlen(json), bind_occurrence, &calls,
        &length, &result, error, sizeof(error));
    if (!out) fprintf(stderr, "map occurrences: %s\n", error);
    assert(out && length == strlen(expected) && !memcmp(out, expected, length + 1) && calls.seen == 63 && result.changed == 6);
    free(out); calls.seen = 0;
    out = sh_package_source_bind_map_sites(reader, expected, strlen(expected), bind_occurrence, &calls,
        &length, &result, error, sizeof(error));
    assert(out && !memcmp(out, expected, length + 1) && !result.changed && !calls.seen); free(out);
    calls.fail_second = 1;
    assert(!sh_package_source_bind_map_sites(reader, json, strlen(json), bind_occurrence, &calls,
        &length, &result, error, sizeof(error)) && !length && !result.changed);
    calls.fail_second = 0;
    out = sh_package_source_bind_map_sites(reader, json, strlen(json), bind_occurrence, &calls,
        &length, &result, error, sizeof(error));
    assert(out && !strcmp(out, expected)); free(out);
    sh_package_source_reader_close(reader);
}

static int library_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    if (strcmp(path, "generated/decls/snapeditorentitydef/controls.decl"))
        return link_original(context, path, body, length);
    *body = (unsigned char *)_strdup("{ edit = {} }"); *length = strlen((char *)*body); return 1;
}

typedef struct rebound_map { const sh_package_provenance *saved, *current; } rebound_map;
static int bind_saved_map(void *context, const char *slot, const char *family, const char *name, const char **replacement)
{
    rebound_map *map = context; (void)slot;
    return sh_package_provenance_resolve(map->saved, map->current, family, name, replacement);
}

static int sound_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context;
    *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/sound/effects/sample.decl") &&
        strcmp(path, "generated/decls/sound/play_sample.decl") &&
        strcmp(path, "generated/decls/sound/stop_sample.decl") &&
        strcmp(path, "generated/decls/sound/default.decl")) return 0;
    const char *text = "{ edit = { } }";
    *body = (unsigned char *)_strdup(text); *length = strlen(text); return *body ? 1 : -1;
}
static void setup_sound(fixture *target)
{
    fixture f; setup(&f);
    void *managers = alloc(&f, 6 * 8);
    memcpy(managers, *(void **)((char *)f.registry + 8), 5 * 8);
    void *manager = alloc(&f, 0x90); pointer(managers, 5 * 8, manager);
    pointer(manager, 8, string(&f, "sound")); pointer(manager, 0x10, string(&f, "idSoundShader"));
    pointer(f.registry, 8, managers); number(f.registry, 0x10, 6);
    void *container = *(void **)f.reflection;
    unsigned char *records = alloc(&f, 21 * 0x38);
    memcpy(records, *(void **)((char *)container + 0x20), 20 * 0x38);
    pointer(container, 0x20, records);
    pointer(records, 19 * 0x38, string(&f, "idSoundShader"));
    pointer(records, 19 * 0x38 + 8, string(&f, "idDeclTypeInfo"));
    void *objects = alloc(&f, 21 * 16), *pointers = alloc(&f, 21 * 16);
    memcpy(objects, *(void **)((char *)f.reflection + 0x88), 20 * 16);
    memcpy(pointers, *(void **)((char *)f.reflection + 0xa0), 20 * 16);
    pointer(f.reflection, 0x88, objects); number(f.reflection, 0x90, 21);
    pointer(f.reflection, 0xa0, pointers); number(f.reflection, 0xa8, 21);
    pointer(pointers, 19 * 16 + 8, (void *)0x10);
    *target = f;
}
static int sound_program_original_id(void *context, const sh_audio_link_graph *graph, size_t node, sh_audio_identity_domain domain, uint32_t authored,
    const unsigned char binding[32], int original, int absent, uint32_t *out, char *error, size_t capacity)
{
    (void)context; (void)domain; (void)binding; (void)absent; (void)error; (void)capacity;
    assert(graph && original); (void)node; *out = authored; return 1;
}
static void test_sound_program_library(void)
{
    fixture f; setup_sound(&f);
    void *container = *(void **)f.reflection;
    unsigned char *records = *(void **)((char *)container + 0x20);
    const char *types[] = {"idStr"}, *ops[] = {""}, *names[] = {"label"};
    fields(&f, records + 19 * 0x38, 1, types, ops, names);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    char temporary[MAX_PATH], relative[512], base[4096], error[2048]; uint32_t event;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-sound-program-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    assert(sh_audio_event_identity("play_sample", &event));
    unsigned char bank[49] = {0};
    memcpy(bank, "BKHD", 4); number(bank, 4, 16); number(bank, 8, 113); number(bank, 12, 84696446);
    memcpy(bank + 24, "HIRC", 4); number(bank, 28, 17); number(bank, 32, 1);
    bank[36] = 4; number(bank, 37, 8); number(bank, 41, event);
    create("base/sound/soundbanks/pc/a.bnk", "");
    snprintf(base, sizeof(base), "%s/base/sound/soundbanks/pc/a.bnk", root);
    FILE *stream = fopen(base, "wb"); assert(stream);
    assert(fwrite(bank, 1, sizeof(bank), stream) == sizeof(bank)); assert(!fclose(stream));
    const char *folders[] = {"a", "b", "copy"};
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, "{\"id\":\"sound\",\"name\":\"Sound\"}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/sound/play_sample.decl", folders[i]);
        create(relative, i == 1 ? "{ edit = { label = \"second\"; } }" : "{ edit = { label = \"first\"; } }");
    }
    snprintf(base, sizeof(base), "%s/base", root);
    sh_audio_originals *audio = sh_audio_originals_open(base, L"English(US)", NULL); assert(audio);
    sh_package_library_native native = sh_package_native_assets_adapter(NULL);
    native.atlas = NULL; native.audio = audio;
    sh_package_compile_environment env = {0}; env.baseline = sound_original;
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_library library = {0};
    int ok = sh_package_library_compile(sources, &env, NULL, sound_original, NULL,
        registry, (uintptr_t)f.reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "sound program library: %s\n", error);
    assert(ok && library.audio && library.compiled->resource_count == 2);
    /* The third package has the exact same authored tree as the first. */
    assert(library.bindings.binding_count == 2 && library.bindings.context_count == 2);
    assert(sh_package_audio_program_bind(library.audio, library.compiled, sound_program_original_id,
        NULL, error, sizeof(error)));
    for (size_t i = 0; i < library.compiled->resource_count; i++)
        assert(library.compiled->resources[i].sound_authored_event == event && library.compiled->resources[i].sound_event == event);
    assert(sh_package_audio_program_scopes(library.audio) >= 3);
    for (size_t i = 0; i < 3; i++) {
        const sh_audio_bank_binding *binding = NULL;
        const sh_audio_graph_bank *captured = sh_package_audio_program_bank(library.audio, i, 0, &binding);
        unsigned char bytes[sizeof(bank)];
        assert(captured && binding && !binding->count && captured->source.length == sizeof(bank));
        assert(captured->source.read(captured->source.context, 0, bytes, sizeof(bytes)) && !memcmp(bytes, bank, sizeof(bank)));
    }
    /* The saved context route must also rebuild and retain its audio program. */
    sh_package_library restored = {0};
    assert(sh_package_library_compile_contexts(sources, &library.bindings, &env, NULL, sound_original, NULL,
        registry, (uintptr_t)f.reflection, &native, &restored, error, sizeof(error)));
    assert(restored.audio && restored.compiled->resource_count == 2 &&
        sh_package_audio_program_bind(restored.audio, restored.compiled, sound_program_original_id,
            NULL, error, sizeof(error)));
    sh_package_library_free(&restored); sh_package_library_free(&library);
    sh_package_sources_free(sources); sh_audio_originals_close(audio); cleanup(); assert(!failures);
    while (f.count) free(f.regions[--f.count].address);
}

static void sound_binary(const char *relative, const void *bytes, size_t length)
{
    char path[4096]; create(relative, ""); snprintf(path, sizeof(path), "%s/%s", root, relative);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(bytes, 1, length, file) == length && !fclose(file));
}
static void sound_catalog(const char *relative, int campaign)
{
    unsigned char bytes[2048] = {5, 'S', 'E', 'R'}; size_t at = 36;
    const char *types[] = {"sound", "entityDef", "snapEditorEntityDef"};
    const char *names[] = {"play_sample", "audio/emitter", "soundcue"};
    const char *paths[] = {"generated/decls/sound/play_sample.decl", "generated/decls/entitydef/audio/emitter.decl",
        "generated/decls/snapeditorentitydef/soundcue.decl"};
    unsigned count = campaign ? 0 : 3; bytes[35] = (unsigned char)count;
    for (unsigned i = 0; i < count; i++) {
        at += 4;
        const char *strings[] = {types[i], names[i], paths[i]};
        for (size_t s = 0; s < 3; s++) {
            size_t length = strlen(strings[s]); number(bytes, at, (int)length); at += 4;
            memcpy(bytes + at, strings[s], length); at += length;
        }
        at += 21;
    }
    uint32_t size = (uint32_t)at - 32;
    bytes[4] = (unsigned char)(size >> 24); bytes[5] = (unsigned char)(size >> 16);
    bytes[6] = (unsigned char)(size >> 8); bytes[7] = (unsigned char)size;
    sound_binary(relative, bytes, at);
}
static int sound_inventory_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    *body = NULL; *length = 0;
    if (sh_package_audio_path_kind(path)) {
        uint64_t size = 0;
        int found = sh_audio_originals_read(context, path, 0, NULL, 0, &size, NULL, 0);
        if (found != 1 || size > SIZE_MAX) return found == 1 ? -1 : found;
        *body = malloc((size_t)size + 1); assert(*body); *length = (size_t)size;
        assert(sh_audio_originals_read(context, path, 0, *body, *length, NULL, NULL, 0) == 1);
        (*body)[*length] = 0; return 1;
    }
    const char *text = NULL;
    if (!strcmp(path, "generated/decls/entitydef/audio/emitter.decl"))
        text = "{ class = \"Entity\"; edit = { sound = \"play_sample\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/soundcue.decl"))
        text = "{ edit = { target = \"audio/emitter\"; } }";
    if (!text) return sound_original(NULL, path, body, length);
    *body = (unsigned char *)_strdup(text); *length = strlen(text); return *body ? 1 : -1;
}
static int sound_inventory_occupied(void *context, sh_audio_identity_domain domain, uint32_t id,
    char *error, size_t capacity)
{
    (void)domain; (void)error; (void)capacity;
    return id == *(uint32_t *)context || id == 84696446 || id == 50 || id == 600 || id == 99;
}
static void sound_inventory_materialize(sh_package_library *library, const char *directory)
{
    char error[2048];
    int ok = sh_package_audio_program_materialize(library->audio, &library->compiled, directory, error, sizeof(error));
    if (!ok) fprintf(stderr, "library audio materialization: %s\n", error); assert(ok);
    size_t banks = 0, media = 0, merged = 0;
    for (size_t i = 0; i < library->compiled->resource_count; i++) {
        const sh_compiled_resource *r = library->compiled->resources + i;
        if (!sh_package_audio_path_kind(r->engine_path)) continue;
        assert(r->cache_file && r->cache_path && r->original_path);
        assert(strcmp(r->original_path, r->engine_path));
        if (strstr(r->engine_path, ".bnk")) {
            banks++; assert(r->bound_dependency_count == 1);
        } else {
            size_t length = 0;
            unsigned char *bytes = sh_package_compilation_read(library->compiled, r, 4, &length, error, sizeof(error));
            assert(bytes && length == 4); media++;
            if (!memcmp(bytes, "aaaa", 4)) { assert(sh_package_owners_count(&r->owners) == 2); merged++; }
            else assert(!memcmp(bytes, "bbbb", 4) && sh_package_owners_count(&r->owners) == 1);
            free(bytes);
        }
    }
    assert(banks == 2 && media == 2 && merged == 1);
}
static int absent_resolver(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    size_t *calls = context; (void)slot; (void)family; (void)name;
    *replacement = NULL; ++*calls; return 0;
}
static int absent_original(void *context, const char *path, unsigned char **body, size_t *length)
{ (void)context; (void)path; *body = NULL; *length = 0; return 0; }
/* The native declaration pointer reader defaults an absent typed name in its
 * own manager. Authored text keeps that reference unchanged with no source
 * edge, like the stock data it was copied from; sound derives an event from the
 * name, so its absence stays a binding failure. */
static void test_absent_declaration_defaults(void)
{
    fixture f; setup_sound(&f);
    void *container = *(void **)f.reflection;
    unsigned char *records = *(void **)((char *)container + 0x20);
    const char *types[] = {"idDeclEntityDef", "idSoundShader"}, *ops[] = {"*", "*"}, *names[] = {"target", "sound"};
    fields(&f, records + 0x38, 2, types, ops, names);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    sh_package_sources sources = {0}; sh_package_compilation empty = {0}; empty.sources = &sources;
    char error[1024]; size_t calls = 0, length = 0;
    sh_package_source_reader *reader = sh_package_source_reader_open(&empty, NULL, absent_original, NULL,
        registry, (uintptr_t)f.reflection, error, sizeof(error));
    if (!reader) fprintf(stderr, "absent declaration reader: %s\n", error);
    assert(reader);
    const char *entity = "{ class = \"Entity\"; edit = { target = \"stock/absent\"; } }";
    unsigned char *out = sh_package_source_bind(reader, "entitydef", "root", (const unsigned char *)entity,
        strlen(entity), absent_resolver, &calls, &length, NULL, error, sizeof(error));
    if (!out) fprintf(stderr, "absent declaration default: %s\n", error);
    assert(out && length == strlen(entity) && !memcmp(out, entity, length) && !calls); free(out);
    const char *sound = "{ class = \"Entity\"; edit = { sound = \"absent_sound\"; } }";
    out = sh_package_source_bind(reader, "entitydef", "root", (const unsigned char *)sound, strlen(sound),
        absent_resolver, &calls, &length, NULL, error, sizeof(error));
    assert(!out && strstr(error, "native source binding could not be resolved") && !calls);
    sh_package_source_reader_close(reader);
    while (f.count) free(f.regions[--f.count].address);
}
static void test_sound_inventory_library(void)
{
    fixture f; setup_sound(&f);
    void *container = *(void **)f.reflection;
    unsigned char *records = *(void **)((char *)container + 0x20);
    const char *types[] = {"idSoundShader"}, *ops[] = {"*"}, *names[] = {"sound"};
    fields(&f, records + 0x38, 1, types, ops, names);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    char temporary[MAX_PATH], relative[512], base[4096], error[2048]; uint32_t event;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-audio-inventory-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL)); assert(sh_audio_event_identity("play_sample", &event));
    /* Complete event/action/streamed-node fixture. */
    unsigned char bank[127] = {0}; audio_bank(bank, 84696446, event, 0);
    number(bank, 28, 95); number(bank, 32, 3); number(bank, 37, 12); number(bank, 45, 1); number(bank, 49, 50);
    bank[53] = 3; number(bank, 54, 18); number(bank, 58, 50); bank[62] = 3; bank[63] = 4;
    number(bank, 64, 600); number(bank, 72, 84696446);
    bank[76] = 2; number(bank, 77, 46); number(bank, 81, 600); number(bank, 85, 0x20001);
    bank[89] = 1; number(bank, 90, 99);
    sound_binary("base/sound/soundbanks/pc/a.bnk", bank, sizeof(bank));
    sound_binary("base/sound/soundbanks/pc/99.wem", "base", 4);
    sound_catalog("base/snap_gameresources.pindex", 0); sound_catalog("base/gameresources.pindex", 1);
    create("base/snap_gameresources.resources", ""); create("base/snap_gameresources.patch", "");
    create("base/gameresources.resources", ""); create("base/gameresources.patch", "");
    const char *folders[] = {"a", "b", "copy", "stock"}, *media[] = {"aaaa", "bbbb", "aaaa", "base"};
    for (size_t i = 0; i < 4; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, i == 2 ? "{\"id\":\"copy\",\"name\":\"Same audio, different package\"}" : "{\"id\":\"audio\",\"name\":\"Audio\"}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/sound/soundbanks/pc/99.wem", folders[i]);
        sound_binary(relative, media[i], 4);
    }
    create("overrides/bank/package.json", "{\"id\":\"bank\",\"name\":\"Original bank\"}");
    sound_binary("overrides/bank/assets/sound/soundbanks/pc/a.bnk", bank, sizeof(bank));
    snprintf(base, sizeof(base), "%s/base", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error)); assert(catalog);
    sh_audio_originals *audio = sh_audio_originals_open(base, L"English(US)", NULL); assert(audio);
    sh_package_library_native native = sh_package_native_assets_adapter(NULL); native.atlas = NULL; native.audio = audio;
    sh_package_compile_environment env = {0}; env.baseline = sound_inventory_original; env.baseline_context = audio;
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_library library = {0}, restored = {0};
    int ok = sh_package_library_compile(sources, &env, catalog, sound_inventory_original, audio,
        registry, (uintptr_t)f.reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "audio inventory library: %s\n", error); assert(ok && library.audio);
    size_t sounds = 0, selectors = 0, entities = 0;
    for (size_t i = 0; i < library.bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = library.bindings.bindings + i;
        if (!strcmp(binding->family, "sound") && !strcmp(binding->original, "play_sample") && strcmp(binding->name, binding->original)) sounds++;
        if (!strcmp(binding->family, "entitydef") && !strcmp(binding->original, "audio/emitter") && strcmp(binding->name, binding->original)) entities++;
        if (!strcmp(binding->family, "snapeditorentitydef") && !strcmp(binding->original, "soundcue") && strcmp(binding->name, binding->original)) selectors++;
    }
    assert(sounds == 2 && entities == 2 && selectors == 2);
    sh_audio_id_pool *reservations = sh_audio_id_pool_open(sound_inventory_occupied, &event); assert(reservations);
    sh_audio_id_lease *local_ids = sh_audio_id_lease_begin(reservations); assert(local_ids);
    assert(sh_package_audio_program_bind(library.audio, library.compiled, sh_audio_id_reserve, local_ids, error, sizeof(error)));
    char cache[4096]; snprintf(cache, sizeof(cache), "%s/package-cache", root); assert(CreateDirectoryA(cache, NULL));
    snprintf(cache, sizeof(cache), "%s/package-cache/resources", root); assert(CreateDirectoryA(cache, NULL));
    sound_inventory_materialize(&library, cache);
    assert(sh_audio_id_lease_ready(local_ids, error, sizeof(error))); sh_audio_id_lease_activate(local_ids);
    /* Save/reopen reconstructs the same private consumers from intact media-
     * only bundles; no declaration has to be added to the authored package. */
    ok = sh_package_library_compile_contexts(sources, &library.bindings, &env, catalog, sound_inventory_original, audio,
        registry, (uintptr_t)f.reflection, &native, &restored, error, sizeof(error));
    if (!ok) fprintf(stderr, "saved audio inventory library: %s\n", error);
    assert(ok && restored.audio && restored.bindings.binding_count == library.bindings.binding_count);
    sh_audio_id_lease *restored_ids = sh_audio_id_lease_begin(reservations); assert(restored_ids);
    assert(sh_package_audio_program_bind(restored.audio, restored.compiled, sh_audio_id_reserve, restored_ids, error, sizeof(error)));
    sound_inventory_materialize(&restored, cache);
    assert(sh_audio_id_lease_ready(restored_ids, error, sizeof(error))); sh_audio_id_lease_activate(restored_ids);
    size_t retained_sounds = 0, retained_audio_files = 0, omitted_originals = 0;
    for (size_t i = 0; i < library.compiled->resource_count; i++) {
        const sh_compiled_resource *r = library.compiled->resources + i;
        if (!r->sound_event && !sh_package_audio_path_kind(r->engine_path)) continue;
        const sh_compiled_resource *same = sh_package_compilation_find(restored.compiled, r->engine_path);
        if (!same) {
            /* Saved contexts omit the unused, ownerless vanilla sound. It is
             * still resolved natively and must not become a carried mod. */
            assert(r->native_original && r->baseline_known == 1 && !sh_package_owners_count(&r->owners));
            omitted_originals++; continue;
        }
        if (!same || same->sound_event != r->sound_event)
            fprintf(stderr, "retained audio identity: %s local=%u restored=%u original=%d owners=%zu\n",
                r->engine_path, r->sound_event, same ? same->sound_event : 0, r->native_original, sh_package_owners_count(&r->owners));
        assert(same && same->sound_event == r->sound_event);
        if (r->cache_path) assert(same->cache_path && !strcmp(same->cache_path, r->cache_path));
        if (r->sound_event) retained_sounds++; else retained_audio_files++;
    }
    assert(retained_sounds == 2 && retained_audio_files == 4 && omitted_originals == 1);
    /* Both providers retain immutable source recipes after local files change. */
    for (size_t i = 0; i < 4; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/assets/sound/soundbanks/pc/99.wem", folders[i]);
        sound_binary(relative, "edit", 4);
    }
    sound_binary("overrides/bank/assets/sound/soundbanks/pc/a.bnk", "edit", 4);
    sound_inventory_materialize(&library, cache);
    sound_inventory_materialize(&restored, cache);
    sh_audio_id_lease_release(restored_ids); sh_audio_id_lease_release(local_ids);
    assert(sh_audio_id_pool_close(reservations));
    sh_package_library_free(&restored); sh_package_library_free(&library); sh_package_sources_free(sources);
    sh_audio_originals_close(audio); sh_resource_catalog_close(catalog); cleanup(); assert(!failures);
    while (f.count) free(f.regions[--f.count].address);
}

/* A package reaching a SnapMap sound whose event no bank implements (the stock
 * "default" sound posts play_default) keeps that sound's native binding: the
 * engine plays nothing for it, so the library compiles without an audio root. */
static void test_bankless_stock_sound(void)
{
    fixture f; setup_sound(&f);
    void *container = *(void **)f.reflection;
    unsigned char *records = *(void **)((char *)container + 0x20);
    const char *types[] = {"idSoundShader"}, *ops[] = {"*"}, *names[] = {"sound"};
    fields(&f, records + 0x38, 1, types, ops, names);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    char temporary[MAX_PATH], base[4096], error[2048]; uint32_t event;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-bankless-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL)); assert(sh_audio_event_identity("play_sample", &event));
    unsigned char bank[127] = {0}; audio_bank(bank, 84696446, event, 0);
    sound_binary("base/sound/soundbanks/pc/a.bnk", bank, sizeof(bank));
    sound_catalog("base/snap_gameresources.pindex", 0); sound_catalog("base/gameresources.pindex", 1);
    create("base/snap_gameresources.resources", ""); create("base/snap_gameresources.patch", "");
    create("base/gameresources.resources", ""); create("base/gameresources.patch", "");
    create("overrides/quiet/package.json", "{\"id\":\"quiet\",\"name\":\"Quiet\"}");
    create("overrides/quiet/assets/generated/decls/entitydef/audio/quiet.decl",
        "{ class = \"Entity\"; edit = { sound = \"default\"; } }");
    snprintf(base, sizeof(base), "%s/base", root);
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error)); assert(catalog);
    sh_audio_originals *audio = sh_audio_originals_open(base, L"English(US)", NULL); assert(audio);
    sh_package_library_native native = sh_package_native_assets_adapter(NULL); native.atlas = NULL; native.audio = audio;
    sh_package_compile_environment env = {0}; env.baseline = sound_inventory_original; env.baseline_context = audio;
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_library library = {0};
    int ok = sh_package_library_compile(sources, &env, catalog, sound_inventory_original, audio,
        registry, (uintptr_t)f.reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "bankless stock sound: %s\n", error);
    assert(ok && library.audio && !sh_package_audio_program_contains(library.audio, 0, "default"));
    sh_package_library_free(&library); sh_package_sources_free(sources);
    sh_audio_originals_close(audio); sh_resource_catalog_close(catalog); cleanup(); assert(!failures);
    while (f.count) free(f.regions[--f.count].address);
}

static int sound_delivery_owners(void *context, size_t scope, const char *family, const char *name,
    const sh_package_owners **owners, const sh_package_owners **gameplay, char *error, size_t capacity)
{
    static const sh_package_owners supplied = {2};
    (void)context; (void)family; (void)name; (void)error; (void)capacity;
    *owners = *gameplay = scope == 1 ? &supplied : NULL; return 1;
}
/* Campaign-only bytes a scope supplies keep their native name, but they are that
 * scope's content: another scope may rely on their absence. They certify as 2,
 * so the linker keeps dependents private. SnapMap bytes certify as 1. */
static void test_campaign_original_binding(fixture *f, sh_decl_registry_source registry)
{
    sh_compiled_resource files[4];
    files[0] = link_decl("base", "generated/decls/entitydef/base.decl",
        "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }");
    files[1] = link_decl("parent-only", "generated/decls/entitydef/parent-only.decl", "{ class = \"Entity\"; edit = {} }");
    files[2] = link_decl("meshbound", "generated/decls/material/meshbound.decl", "{ }"); files[2].type = "material";
    files[3] = link_decl("label", "generated/decls/renderparm/label.decl", "{ String }"); files[3].type = "renderparm";
    sh_package_compilation scope = {0}; scope.resources = files; scope.resource_count = 4;
    const sh_package_compilation *views[] = {&scope};
    sh_package_library_native native = sh_package_native_assets_adapter(NULL);
    char error[1024];
    sh_package_source_link *source = sh_package_source_link_open(views, 1, NULL, original, f,
        registry, (uintptr_t)f->reflection, &native.source, error, sizeof(error));
    if (!source) fprintf(stderr, "campaign originals: %s\n", error);
    assert(source);
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(source);
    const char *family[] = {"entitydef", "entitydef", "material", "renderparm"};
    const char *name[] = {"base", "parent-only", "meshbound", "label"};
    const int expected[] = {2, 2, 1, 2};
    for (size_t i = 0; i < 4; i++) {
        size_t unit = sh_package_link_inputs_find(inputs, 0, family[i], name[i]);
        assert(unit < inputs->count && inputs->units[unit].original_binding == expected[i]);
    }
    sh_package_source_link_close(source);
}
static void test_sound_original_binding(void)
{
    fixture f; setup_sound(&f);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    sh_compiled_resource files[3] = {0}; sh_package_compilation scopes[3] = {0};
    const sh_package_compilation *views[3] = {scopes, scopes + 1, scopes + 2};
    const char *names[] = {"play_sample", "play_sample", "stop_sample"};
    const char *paths[] = {"generated/decls/sound/play_sample.decl", "generated/decls/sound/play_sample.decl", "generated/decls/sound/stop_sample.decl"};
    for (size_t i = 0; i < 3; i++) {
        files[i].engine_path = (char *)paths[i]; files[i].type = "sound"; files[i].name = (char *)names[i];
        files[i].body = (unsigned char *)"{ edit = { } }"; files[i].body_length = strlen((char *)files[i].body);
        files[i].source = SIZE_MAX;
        scopes[i].resources = files + i; scopes[i].resource_count = 1;
    }
    char error[2048]; sh_package_link_classes classes = {0};
    sh_package_library_native native = sh_package_native_assets_adapter(NULL);
    native.source.owners = sound_delivery_owners;
    sh_package_source_link *source = sh_package_source_link_open(views, 3, NULL, sound_original, NULL,
        registry, (uintptr_t)f.reflection, &native.source, error, sizeof(error));
    if (!source) fprintf(stderr, "original sound: %s\n", error);
    assert(source && sh_package_source_link_identify(source, &classes, error, sizeof(error)));
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(source);
    assert(inputs->count == 3 && classes.count == 2);
    const unsigned char *shared = sh_package_source_link_shared(source);
    assert(shared && shared[0] && !shared[1] && shared[2]);
    /* Ownership is not an equality key or an engine original certificate. */
    assert(classes.nodes[0] == classes.nodes[1]);
    for (size_t i = 0; i < inputs->count; i++) {
        const sh_package_link_unit *unit = inputs->units + i;
        assert(unit->original_binding);
        char *name = NULL, **assigned = NULL;
        assert(sh_package_native_names_assign((void *)inputs, i, unit->semantics, unit->original_binding,
            unit->files, unit->file_count, &name, &assigned, error, sizeof(error)));
        assert(!strcmp(name, unit->name) && !strcmp(assigned[0], unit->files[0]->engine_path));
        free(name); free(assigned[0]); free(assigned);
    }
    sh_package_sources inventory = {0}; sh_package_link_output linked = {0};
    native.source.owners = NULL;
    /* This fixture's ad-hoc output inventory has no deliveries. The captured
     * owner sets above are tested independently of that empty output build. */
    sh_package_link_classes_free(&classes); sh_package_source_link_close(source);
    source = sh_package_source_link_open(views, 3, NULL, sound_original, NULL,
        registry, (uintptr_t)f.reflection, &native.source, error, sizeof(error)); assert(source);
    inputs = sh_package_source_link_inputs(source);
    sh_package_link_adapter names_adapter = {(void *)inputs, NULL, NULL, sh_package_native_names_assign};
    uint32_t play, stop, event;
    assert(sh_audio_event_identity("play_sample", &play) && sh_audio_event_identity("stop_sample", &stop));
    assert(sh_package_source_link_compile(source, &inventory, &names_adapter, NULL, NULL,
        &linked, error, sizeof(error)) && sh_package_sound_prepare(linked.compiled, error, sizeof(error)));
    assert(linked.compiled->resource_count == 2 && !sh_package_sound_requires_binding(linked.compiled) &&
        sh_package_sound_event(linked.compiled, "play_sample", &event) && event == play &&
        sh_package_sound_event(linked.compiled, "stop_sample", &event) && event == stop);
    sh_package_link_output_free(&linked);
    sh_package_link_classes_free(&classes); sh_package_source_link_close(source);
    files[1].body = (unsigned char *)"{ edit = { } } /* authored edit */";
    files[1].body_length = strlen((char *)files[1].body);
    source = sh_package_source_link_open(views, 3, NULL, sound_original, NULL,
        registry, (uintptr_t)f.reflection, &native.source, error, sizeof(error));
    assert(source); inputs = sh_package_source_link_inputs(source);
    size_t changed = 0;
    for (size_t i = 0; i < inputs->count; i++) changed += !inputs->units[i].original_binding;
    assert(changed == 1); /* Name semantics alone never certify changed source bytes. */
    names_adapter.context = (void *)inputs;
    assert(sh_package_source_link_compile(source, &inventory, &names_adapter, NULL, NULL,
        &linked, error, sizeof(error)) && sh_package_sound_prepare(linked.compiled, error, sizeof(error)) &&
        sh_package_sound_requires_binding(linked.compiled));
    size_t aliases = 0;
    for (size_t i = 0; i < linked.compiled->resource_count; i++) {
        const sh_compiled_resource *resource = linked.compiled->resources + i;
        if (!resource->original_path) continue;
        aliases++;
        assert(strcmp(resource->name, "play_sample") &&
            sh_package_sound_event(linked.compiled, resource->name, &event) && event == play);
    }
    assert(aliases == 1); sh_package_link_output_free(&linked);
    sh_package_source_link_close(source);
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}

static void test_library_hud(void)
{
    fixture f; setup(&f);
    void *managers = alloc(&f, 6 * 8);
    memcpy(managers, *(void **)((char *)f.registry + 8), 5 * 8);
    void *manager = alloc(&f, 0x90); pointer(managers, 5 * 8, manager);
    pointer(manager, 8, string(&f, "weapon")); pointer(manager, 0x10, string(&f, "idDeclWeapon"));
    pointer(f.registry, 8, managers); number(f.registry, 0x10, 6);
    void *container = *(void **)f.reflection;
    unsigned char *records = alloc(&f, 21 * 0x38);
    memcpy(records, *(void **)((char *)container + 0x20), 20 * 0x38);
    pointer(container, 0x20, records);
    pointer(records, 19 * 0x38, string(&f, "idDeclWeapon"));
    pointer(records, 19 * 0x38 + 8, string(&f, "idDeclTypeInfo"));
    void *object_readers = alloc(&f, 21 * 16), *pointer_readers = alloc(&f, 21 * 16);
    memcpy(object_readers, *(void **)((char *)f.reflection + 0x88), 20 * 16);
    memcpy(pointer_readers, *(void **)((char *)f.reflection + 0xa0), 20 * 16);
    pointer(f.reflection, 0x88, object_readers); number(f.reflection, 0x90, 21);
    pointer(f.reflection, 0xa0, pointer_readers); number(f.reflection, 0xa8, 21);
    const char *types[] = {"idDeclEntityDef"}, *ops[] = {"*"}, *names[] = {"target"};
    fields(&f, records + 19 * 0x38, 1, types, ops, names);
    pointer(*(void **)((char *)f.reflection + 0xa0), 19 * 16 + 8, (void *)0x10);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    source_link_fixture originals = {&f};
    sh_package_compile_environment env = {0}; env.baseline = library_original; env.baseline_context = &originals;
    sh_package_library library = {0};
    char temporary[MAX_PATH], relative[256], descriptor[1024], error[2048];
    const char *folders[] = {"red", "green", "duplicate", "red/child", "native-engine", "native-weapon"};
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-hud-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 6; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        snprintf(descriptor, sizeof(descriptor), "{\"id\":\"same\",\"name\":\"HUD\",\"hud\":{\"weapons\":{\"weapon/%s\":{\"ammo_display\":\"%s\"}}}}",
            i < 4 ? "shared" : "native", i == 0 || i == 5 ? "weapon" : "engine");
        create(relative, descriptor);
        if (i >= 4) continue;
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/weapon/weapon/shared.decl", folders[i]);
        create(relative, i == 1 ? "{ edit = { target = \"modifier-child\"; } }" :
            i == 3 ? "{ edit = { target = \"\"; } }" : "{ edit = { target = \"parent-only\"; } }");
    }
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, library_original, &originals,
        registry, (uintptr_t)f.reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "HUD library: %s\n", error);
    assert(ok && library.compiled->policy.hud_bound);
    /* Diagnostic repeated-work lines: parse census, source capture, identity passes. */
    assert(strstr(library.repeated_work, "parses ") == library.repeated_work);
    assert(strstr(library.repeated_work, "\nsource link ") && strstr(library.repeated_work, " identity passes: "));
    sh_json_object rows = {0}; const char *raw = sh_json_object_get(&library.compiled->policy.hud, "weapons");
    assert(raw && sh_json_parse_object(raw, strlen(raw), 3, &rows) && rows.count == 4);
    assert(!sh_json_object_get(&rows, "weapon/shared"));
    int mode = 0; assert(sh_package_hud_mode(sh_json_object_get(&rows, "weapon/native"), &mode) && mode == 1);
    size_t weapons = 0;
    for (size_t i = 0; i < library.compiled->resource_count; i++) {
        const sh_compiled_resource *resource = library.compiled->resources + i;
        if (!resource->type || strcmp(resource->type, "weapon")) continue;
        weapons++;
        assert(sh_package_hud_mode(sh_json_object_get(&rows, resource->name), &mode));
        size_t body_length = 0;
        unsigned char *body = sh_package_source_read(sources->files + resource->sources[0], SIZE_MAX,
            &body_length, error, sizeof(error)); assert(body);
        assert(mode == (strstr((char *)body, "parent-only") != NULL)); free(body);
    }
    assert(weapons == 3); /* Same bytes dedupe even with a different HUD request. */
    sh_json_object_free(&rows);
    /* Select one source owner after library linking: original raw HUD keys
     * must not replace the retained aliases. */
    sh_package_owners selected = {0}; sh_package_policy policy = {0};
    for (size_t i = 0; i < sources->package_count; i++)
        if (!strcmp(sources->packages[i].name, "green")) assert(sh_package_owners_add(&selected, i));
    assert(sh_package_owners_count(&selected) == 1);
    assert(sh_package_compilation_policy(library.compiled, &selected, &policy, error, sizeof(error)));
    raw = sh_json_object_get(&policy.hud, "weapons");
    assert(raw && !strstr(raw, "weapon/shared") && !strstr(raw, "weapon/native"));
    assert(sh_json_parse_object(raw, strlen(raw), 3, &rows) && rows.count == 1);
    assert(sh_package_hud_mode(rows.members[0].value_json, &mode) && mode == 0);
    sh_json_object_free(&rows); sh_package_policy_free(&policy); sh_package_owners_free(&selected);
    /* Historical composed map contexts retain their alias-bound executable
     * policy when saved metadata is recompiled on top of a newer library. */
    sh_package_provenance historical = {0}; sh_package_library loaded = {0};
    size_t green = SIZE_MAX;
    for (size_t i = 0; i < sources->package_count; i++)
        if (!strcmp(sources->packages[i].name, "green")) green = i;
    assert(green != SIZE_MAX);
    assert(sh_package_provenance_context_add(&historical, SH_PACKAGE_CONTEXT_COMPOSED,
        (const unsigned char (*)[32])sh_package_source_tree(sources, green), 1, NULL) != SIZE_MAX);
    assert(sh_package_library_compile_contexts(sources, &historical, &env, NULL, library_original,
        &originals, registry, (uintptr_t)f.reflection, NULL, &loaded, error, sizeof(error)));
    raw = sh_json_object_get(&loaded.compiled->policy.hud, "weapons");
    assert(raw && !strstr(raw, "weapon/shared") && !strstr(raw, "weapon/native"));
    assert(sh_json_parse_object(raw, strlen(raw), 3, &rows) && rows.count == 1);
    sh_json_object_free(&rows); sh_package_library_free(&loaded); sh_package_provenance_free(&historical);
    sh_package_library_free(&library); sh_package_sources_free(sources); cleanup();
    while (f.count) free(f.regions[--f.count].address);
}

/* Units the library's source capture replayed (its repeated-work line). */
static size_t library_replays(const sh_package_library *library)
{
    const char *line = strstr(library->repeated_work, "units discovered, ");
    size_t replays = SIZE_MAX;
    if (!line || sscanf(line, "units discovered, %zu replayed", &replays) != 1) return SIZE_MAX;
    return replays;
}
/* A library compiled with discovery replay equals the same compile without
 * it: resources, payload identities, bound dependencies, ownership, native
 * names, saved bindings and accounted diagnostics. */
static void library_same(const sh_package_library *a, const sh_package_library *b)
{
    char error[512];
    assert(!a->compiled == !b->compiled);
    if (a->compiled) {
        assert(a->compiled->resource_count == b->compiled->resource_count);
        for (size_t i = 0; i < a->compiled->resource_count; i++) {
            const sh_compiled_resource *p = a->compiled->resources + i, *q = b->compiled->resources + i;
            sh_package_file_identity x, y;
            assert(!strcmp(p->engine_path, q->engine_path));
            assert(!strcmp(p->type ? p->type : "", q->type ? q->type : "") && !strcmp(p->name ? p->name : "", q->name ? q->name : ""));
            assert(!strcmp(p->native_type ? p->native_type : "", q->native_type ? q->native_type : "") &&
                !strcmp(p->native_name ? p->native_name : "", q->native_name ? q->native_name : ""));
            assert(!strcmp(p->original_path ? p->original_path : "", q->original_path ? q->original_path : ""));
            int identified = sh_package_compilation_identity(a->compiled, p, &x, error, sizeof(error));
            assert(identified == sh_package_compilation_identity(b->compiled, q, &y, error, sizeof(error)));
            if (identified) assert(!memcmp(&x, &y, sizeof(x)));
            assert(replay_owners_same(&p->owners, &q->owners) && replay_owners_same(&p->gameplay_owners, &q->gameplay_owners));
            assert(p->source_count == q->source_count && (!p->source_count || !memcmp(p->sources, q->sources, p->source_count * sizeof(*p->sources))));
            assert(p->bound_dependency_count == q->bound_dependency_count);
            for (size_t k = 0; k < p->bound_dependency_count; k++) assert(!strcmp(p->bound_dependencies[k], q->bound_dependencies[k]));
            assert(p->native_original == q->native_original && p->baseline_known == q->baseline_known &&
                p->restored_original == q->restored_original && p->generated == q->generated && p->composed == q->composed);
        }
    }
    size_t x_length = 0, y_length = 0;
    unsigned char *x = sh_package_provenance_encode(&a->bindings, &x_length, error, sizeof(error));
    unsigned char *y = sh_package_provenance_encode(&b->bindings, &y_length, error, sizeof(error));
    assert(x && y && x_length == y_length && !memcmp(x, y, x_length)); free(x); free(y);
    assert(!a->coverage == !b->coverage && (!a->coverage || !strcmp(a->coverage, b->coverage)));
}
/* Compile again with the registration-key site memo off: the same library.
 * Returns how many units the memo left unrewritten in the first. */
static size_t library_key_memo_same(const sh_package_library *library, const sh_package_sources *sources,
    const sh_package_compile_environment *env, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_library_native *native)
{
    sh_package_library plain = {0}; char error[1024];
    size_t scans = 0, reused = 0, kept = 0;
    const char *line = strstr(library->repeated_work, "key sites: ");
    if (line) assert(sscanf(line, "key sites: %zu scans, %zu reused, %zu units kept unbound", &scans, &reused, &kept) == 3);
    sh_package_library_test_key_memo(0);
    int ok = sh_package_library_compile(sources, env, NULL, original, original_context, registry, reflection,
        native, &plain, error, sizeof(error));
    sh_package_library_test_key_memo(1);
    if (!ok) fprintf(stderr, "library without key memo: %s\n", error);
    assert(ok && !strstr(plain.repeated_work, "key sites: "));
    library_same(library, &plain); sh_package_library_free(&plain);
    return kept;
}
/* Compile again with discovery replay off and compare with the replayed
 * compile, which must have replayed at least minimum units. */
static void library_replay_same(const sh_package_library *replayed, size_t minimum, const sh_package_sources *sources,
    const sh_package_compile_environment *env, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_library_native *native)
{
    sh_package_library plain = {0}; char error[1024];
    size_t replays = library_replays(replayed);
    if (replays == SIZE_MAX || replays < minimum) fprintf(stderr, "replayed units: %s\n", replayed->repeated_work);
    assert(replays != SIZE_MAX && replays >= minimum);
    sh_package_source_link_replay(0);
    int ok = sh_package_library_compile(sources, env, NULL, original, original_context, registry, reflection,
        native, &plain, error, sizeof(error));
    sh_package_source_link_replay(1);
    if (!ok) fprintf(stderr, "library without replay: %s\n", error);
    assert(ok && library_replays(&plain) == 0);
    library_same(replayed, &plain); sh_package_library_free(&plain);
}
static void test_library(fixture *f, sh_decl_registry_source registry)
{
    const char *folders[] = {"red", "map-0123456789abcdef/green", "duplicate"};
    char temporary[MAX_PATH], relative[256], error[1024];
    sh_package_sources *sources;
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library library = {0};
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-library-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, i == 2 ? "{\"id\":\"same.id\",\"name\":\"Copy\",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}" :
            i == 1 ? "{\"id\":\"same.id\",\"name\":\"Assets\",\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}}}" :
            "{\"id\":\"same.id\",\"name\":\"Assets\",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/entitydef/root.decl", folders[i]);
        create(relative, i == 1 ? "{ class = \"Entity\"; edit = { target = \"modifier-child\"; } }" :
            "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/snapeditorentitydef/private.decl", folders[i]);
        create(relative, "{ edit = { target = \"root\"; } }");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/snapeditorentitydef/controls.decl", folders[i]);
        create(relative, i == 1 ? "{ edit = { inputs = { num = 1; item[0] = \"modifier-child\"; } } }" :
            "{ edit = { inputs = { num = 1; item[0] = \"parent-only\"; } } }");
    }
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    env.baseline = library_original; env.baseline_context = &originals;
    int ok = sh_package_library_compile(sources, &env, NULL, library_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "library: %s\n", error);
    assert(ok && library.compiled && library.compiled->resource_count == 7);
    /* Three scopes, one beneath an organizer folder, reach the same stock units. */
    library_replay_same(&library, 1, sources, &env, library_original, &originals, registry, (uintptr_t)f->reflection, NULL);
    assert(library.bindings.context_count == 3 && library.bindings.binding_count == 4);
    assert(!sh_package_compilation_find(library.compiled, "generated/decls/entitydef/root.decl"));
    const sh_compiled_resource *controls = sh_package_compilation_find(library.compiled,
        "generated/decls/snapeditorentitydef/controls.decl");
    assert(controls && controls->source_count == 3 && !sh_package_owners_count(&controls->gameplay_owners));
    assert(strstr((char *)controls->body, "\"parent-only\"") && strstr((char *)controls->body, "\"modifier-child\""));
    size_t private_entries = 0, roots = 0, duplicate_bindings = 0;
    for (size_t i = 0; i < library.bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = library.bindings.bindings + i;
        assert(strncmp(binding->name, "smp/", 4) == 0);
        if (!strcmp(binding->family, "entitydef")) roots++;
        else { assert(!strcmp(binding->family, "snapeditorentitydef")); private_entries++; }
        if (binding->context_count == 2) duplicate_bindings++;
    }
    assert(roots == 2 && private_entries == 2 && duplicate_bindings == 2);
    {
        sh_package_provenance saved = {0}, mixed = {0}; sh_package_library restored = {0};
        size_t length; unsigned char *encoded = sh_package_provenance_encode(&library.bindings, &length, error, sizeof(error));
        assert(encoded && sh_package_provenance_decode(encoded, length, &saved, error, sizeof(error))); free(encoded);
        assert(sh_package_library_compile_contexts(sources, &saved, &env, NULL, library_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &restored, error, sizeof(error)));
        /* An imported editor extension cannot replace a live global selector.
         * Its unchanged native dependencies still keep their original names. */
        size_t private_controls = 0;
        assert(!sh_package_compilation_find(restored.compiled,
            "generated/decls/snapeditorentitydef/controls.decl"));
        assert(sh_package_compilation_find(restored.compiled,
            "generated/decls/entitydef/parent-only.decl"));
        for (size_t i = 0; i < restored.compiled->resource_count; i++) {
            const sh_compiled_resource *resource = restored.compiled->resources + i;
            if (resource->original_path && !strcmp(resource->original_path,
                "generated/decls/snapeditorentitydef/controls.decl")) {
                assert(resource->native_name && strcmp(resource->native_name, "controls") &&
                    strstr((const char *)resource->body, "parent-only") &&
                    strstr((const char *)resource->body, "modifier-child"));
                private_controls++;
            }
        }
        assert(private_controls == 1);
        for (size_t i = 0; i < saved.binding_count; i++) {
            const char *name = NULL; const sh_package_provenance_binding *binding = saved.bindings + i;
            assert(sh_package_provenance_resolve(&saved, &restored.bindings, binding->family, binding->name, &name) == 1);
            assert(!strcmp(name, binding->name));
        }
        sh_package_library_free(&restored);
        unsigned char legacy_trees[2][32]; size_t old_count = 0, green = SIZE_MAX;
        for (size_t i = 0; i < sources->package_count; i++) {
            if (strstr(sources->packages[i].name, "green")) green = i;
            else { assert(old_count < 2); memcpy(legacy_trees[old_count++], sources->fingerprints[i], 32); }
        }
        assert(old_count == 2 && green != SIZE_MAX);
        size_t old = sh_package_provenance_context_add(&mixed, SH_PACKAGE_CONTEXT_COMPOSED, legacy_trees, 2, NULL);
        size_t fresh = sh_package_provenance_context_add(&mixed, SH_PACKAGE_CONTEXT_ISOLATED,
            sources->fingerprints + green, 1, "");
        assert(sh_package_provenance_bind(&mixed, "entitydef", "root", "saved-old", &old, 1));
        assert(sh_package_provenance_bind(&mixed, "entitydef", "root", "saved-green", &fresh, 1));
        assert(sh_package_library_compile_contexts(sources, &mixed, &env, NULL, library_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &restored, error, sizeof(error)));
        const char *old_name, *green_name;
        assert(sh_package_provenance_resolve(&mixed, &restored.bindings, "entitydef", "saved-old", &old_name) == 1);
        assert(sh_package_provenance_resolve(&mixed, &restored.bindings, "entitydef", "saved-green", &green_name) == 1);
        assert(strcmp(old_name, green_name));
        char path[512]; snprintf(path, sizeof(path), "generated/decls/entitydef/%s.decl", old_name);
        const sh_compiled_resource *old_resource = sh_package_compilation_find(restored.compiled, path);
        assert(old_resource && strstr((char *)old_resource->body, "parent-only") && !strstr((char *)old_resource->body, "modifier-child"));
        const char *json = "{\"actors\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"saved-old\"},"
            "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"saved-green\"}],\"note\":\"saved-old\x97\"}";
        rebound_map map = {&mixed, &restored.bindings}; sh_package_source_binding binding_result;
        sh_package_source_reader *reader = sh_package_source_reader_open(restored.compiled, NULL,
            library_original, &originals, registry, (uintptr_t)f->reflection, error, sizeof(error)); assert(reader);
        char *rebound = sh_package_source_bind_map(reader, json, strlen(json), bind_saved_map, &map,
            &length, &binding_result, error, sizeof(error));
        assert(rebound && binding_result.changed == 2 && strstr(rebound, old_name) && strstr(rebound, green_name) &&
            strstr(rebound, "\"note\":\"saved-old\x97\"")); free(rebound); sh_package_source_reader_close(reader);
        mixed.contexts[old].trees[0][0] ^= 128;
        assert(!sh_package_library_compile_contexts(sources, &mixed, &env, NULL, library_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &restored, error, sizeof(error)) && !restored.compiled);
        sh_package_library_free(&restored); sh_package_provenance_free(&saved); sh_package_provenance_free(&mixed);
    }
    sh_package_compilation empty = {0}; sh_package_sources no_sources = {0};
    empty.sources = &no_sources;
    sh_package_compilation *view = sh_package_compilation_overlay(library.compiled, &empty, error, sizeof(error));
    assert(view && view->independent_sources && !view->map_independent_sources);
    sh_package_policy selected = {0};
    sh_package_owners all = {0};
    for (size_t i = 0; i < sources->package_count; i++) assert(sh_package_owners_add(&all, i));
    assert(sh_package_compilation_policy(view, &all, &selected, error, sizeof(error)));
    const char *cvars = sh_json_object_get(&selected.requirements, "cvars");
    assert(cvars && strstr(cvars, "g_useResourceBlackList") && strstr(cvars, "g_useImageBlackList"));
    sh_package_policy_free(&selected);
    sh_package_owners_free(&all);
    sh_package_compilation_free(view);
    for (size_t i = 0; i < sources->file_count; i++) if (!sources->files[i].directory)
        assert(sh_package_source_verify(sources->files + i));
    /* An operational reader failure must clear a stale rejection from a prior
     * attempt; it is not permission to discard a healthy source package. */
    size_t invalid = 0;
    env.invalid_package = &invalid;
    sh_decl_registry_source unavailable = registry; unavailable.read = NULL;
    assert(!sh_package_library_compile(sources, &env, NULL, library_original, &originals,
        unavailable, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error)));
    assert(invalid == SIZE_MAX && !library.compiled && !library.bindings.binding_count);
    sh_package_sources_free(sources);
    create("overrides/red/assets/generated/decls/entitydef/root.decl", "{ edit = broken {");
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    assert(!sh_package_library_compile(sources, &env, NULL, library_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error)));
    assert(invalid < sources->package_count && !strcmp(sources->packages[invalid].name, "red"));
    assert(!library.compiled && !library.bindings.binding_count);
    sh_package_library_free(&library); sh_package_sources_free(sources); cleanup(); assert(!failures);
}

static void registration_root(const char *label);
static const char *scoped_entity_name(const sh_package_library *library, const sh_package_sources *sources,
    const char *package, const char *original);
static const sh_compiled_resource *named_resource(const sh_package_compilation *compiled,
    const char *family, const char *name);
static int replay_library_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = replay_stock(path);
    if (!text) return library_original(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* The library level of the discovery replay contract (test_source_link_replay):
 * packages beneath an organizer folder reach one stock child through their
 * own roots, parents and modifiers, in both discovery orders. Replay and the
 * editor composition memo are optimizations only: with either off the library
 * is the same, including a package supplying a byte-identical copy of stock
 * gameplay (the stock child's bound text, but a private editor binding). The
 * final link reuses captured discovery sites, so this comparison, not a
 * rebind, is what would expose a replay that ignored a changed dependency. */
static void test_library_replay(fixture *f, sh_decl_registry_source registry)
{
    /* Packages are discovered in name order: an unchanged package first,
     * the reverse, and changed packages first (their recordings consulted
     * a path they supply and are never kept). */
    static const char *const names[3][5] = {
        {"a-plain", "b-copy", "c-parent", "d-mod", "e-late"},
        {"z-plain", "y-copy", "x-parent", "w-mod", "v-late"},
        {"c-plain", "d-copy", "a-parent", "b-mod", "e-late"},
    };
    source_link_fixture originals = {f};
    char error[1024], path[256], text[256];
    for (int round = 0; round < 3; round++) {
        registration_root(round == 1 ? "library-replay-reversed" : round ? "library-replay-changed" : "library-replay");
        for (size_t i = 0; i < 5; i++) {
            snprintf(path, sizeof(path), "overrides/organizer/%s/package.json", names[round][i]);
            snprintf(text, sizeof(text), "{\"id\":\"replay.%zu\",\"name\":\"%s\"}", i, names[round][i]); create(path, text);
            snprintf(path, sizeof(path), "overrides/organizer/%s/assets/generated/decls/entitydef/replay/seed.decl", names[round][i]);
            create(path, "{ class = \"Entity\"; edit = { target = \"replay/child\"; } }");
        }
        snprintf(path, sizeof(path), "overrides/organizer/%s/assets/generated/decls/entitydef/root.decl", names[round][1]);
        create(path, replay_stock("generated/decls/entitydef/root.decl"));
        snprintf(path, sizeof(path), "overrides/organizer/%s/assets/generated/decls/entitydef/replay/parent.decl", names[round][2]);
        create(path, "{ class = \"AlternateEntity\"; edit = {} }");
        snprintf(path, sizeof(path), "overrides/organizer/%s/assets/generated/decls/actormodifier/replay/mod.decl", names[round][3]);
        create(path, "{ edit = { target = \"parent-only\"; } }");
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        sh_package_compile_environment env = {0};
        env.baseline = replay_library_original; env.baseline_context = &originals;
        sh_package_library library = {0}, fresh = {0};
        int ok = sh_package_library_compile(sources, &env, NULL, replay_library_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "library replay: %s\n", error);
        assert(ok);
        library_replay_same(&library, 2, sources, &env, replay_library_original, &originals,
            registry, (uintptr_t)f->reflection, NULL);
        sh_package_library_test_editor_memo(0);
        ok = sh_package_library_compile(sources, &env, NULL, replay_library_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &fresh, error, sizeof(error));
        sh_package_library_test_editor_memo(1);
        if (!ok) fprintf(stderr, "library without editor memo: %s\n", error);
        assert(ok); library_same(&library, &fresh); sh_package_library_free(&fresh);
        /* Each package keeps its own child, as its own seed binds it: the
         * stock ones share one; a changed parent or a supplied modifier makes
         * another. */
        char child[5][256];
        for (size_t i = 0; i < 5; i++) {
            const char *package = NULL, *value = NULL;
            size_t n = strlen(names[round][i]);
            for (size_t p = 0; p < sources->package_count; p++) {
                const char *name = sources->packages[p].name; size_t m = strlen(name);
                if (m >= n && !strcmp(name + m - n, names[round][i]) && (m == n || name[m - n - 1] == '/' || name[m - n - 1] == '\\'))
                    package = name;
            }
            assert(package);
            const char *seed = scoped_entity_name(&library, sources, package, "replay/seed");
            const sh_compiled_resource *bound = seed ? named_resource(library.compiled, "entitydef", seed) : NULL;
            sh_decl_node *tree = bound ? sh_decl_tree_parse((sh_decl_source){(char *)bound->body, bound->body_length}, NULL, 0) : NULL;
            size_t length = 0;
            assert(tree &&sh_decl_tree_literal(sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "target"), &value, &length) &&
                length < sizeof(child[i]));
            memcpy(child[i], value, length); child[i][length] = 0; sh_decl_tree_free(tree);
        }
        if (strcmp(child[0], child[4]) || !strcmp(child[0], child[2]) || !strcmp(child[0], child[3]))
            fprintf(stderr, "library replay children: %s | %s | %s | %s | %s\n", child[0], child[1], child[2], child[3], child[4]);
        assert(!strcmp(child[0], child[4]) && strcmp(child[0], child[2]) && strcmp(child[0], child[3]));
        sh_package_library_free(&library); sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
}

/* Units the library's source capture replayed from earlier compiles. */
static size_t library_retained(const sh_package_library *library)
{
    const char *line = strstr(library->repeated_work, " ms; ");
    size_t retained = SIZE_MAX;
    if (!line || sscanf(line, " ms; %zu replayed from earlier compiles", &retained) != 1) return SIZE_MAX;
    return retained;
}
/* Separate library compiles sharing one memo (sh_package_source_memo): each
 * equals the same compile without the memo (library_same: resources, payload
 * identities, bound dependencies, ownership, native names, saved bindings,
 * diagnostics), through contributions and the link, while unchanged packages
 * replay their discovery from the earlier compiles: unchanged, an edited
 * package, an added one and a removed one. */
static void test_library_memo(fixture *f, sh_decl_registry_source registry)
{
    static const char *const names[6] = {"a-plain", "b-copy", "c-parent", "d-mod", "e-late", "f-added"};
    source_link_fixture originals = {f};
    char error[1024], path[256], text[256];
    sh_package_source_memo *memo = sh_package_source_memo_open();
    sh_package_library_native native = {0};
    assert(memo); native.source.memo = memo;
    registration_root("library-memo");
    for (size_t i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "overrides/organizer/%s/package.json", names[i]);
        snprintf(text, sizeof(text), "{\"id\":\"memo.%zu\",\"name\":\"%s\"}", i, names[i]); create(path, text);
        snprintf(path, sizeof(path), "overrides/organizer/%s/assets/generated/decls/entitydef/replay/seed.decl", names[i]);
        create(path, "{ class = \"Entity\"; edit = { target = \"replay/child\"; } }");
    }
    create("overrides/organizer/b-copy/assets/generated/decls/entitydef/root.decl", replay_stock("generated/decls/entitydef/root.decl"));
    create("overrides/organizer/c-parent/assets/generated/decls/entitydef/replay/parent.decl", "{ class = \"AlternateEntity\"; edit = {} }");
    create("overrides/organizer/d-mod/assets/generated/decls/actormodifier/replay/mod.decl", "{ edit = { target = \"parent-only\"; } }");
    for (int step = 0; step < 5; step++) {
        if (step == 2) create("overrides/organizer/c-parent/assets/generated/decls/entitydef/replay/parent.decl",
            "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }");
        if (step == 3) {
            create("overrides/organizer/f-added/package.json", "{\"id\":\"memo.5\",\"name\":\"f-added\"}");
            create("overrides/organizer/f-added/assets/generated/decls/entitydef/replay/seed.decl",
                "{ class = \"Entity\"; edit = { target = \"root\"; } }");
        }
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        if (step == 4) for (size_t p = 0; p < sources->package_count; p++) if (strstr(sources->packages[p].name, "d-mod")) {
            assert(sh_package_sources_remove(sources, p)); break;
        }
        sh_package_compile_environment env = {0};
        env.baseline = replay_library_original; env.baseline_context = &originals;
        sh_package_library with = {0}, without = {0};
        int ok = sh_package_library_compile(sources, &env, NULL, replay_library_original, &originals,
            registry, (uintptr_t)f->reflection, &native, &with, error, sizeof(error));
        if (!ok) fprintf(stderr, "library memo step %d: %s\n", step, error);
        assert(ok);
        ok = sh_package_library_compile(sources, &env, NULL, replay_library_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &without, error, sizeof(error));
        assert(ok);
        library_same(&with, &without);
        size_t retained = library_retained(&with);
        if (retained == SIZE_MAX || (step > 0) != (retained > 0) || library_retained(&without) != 0)
            fprintf(stderr, "library memo step %d: %s\n", step, with.repeated_work);
        assert(retained != SIZE_MAX && (step > 0) == (retained > 0) && library_retained(&without) == 0);
        sh_package_library_free(&with); sh_package_library_free(&without); sh_package_sources_free(sources);
    }
    sh_package_source_memo_close(memo);
    cleanup(); assert(!failures);
}
/* A stock parent editor definition with a complete property list, and a child
 * that patches one parent property by index (like demons/archvile_enc). */
static const char *demo_parent_original = "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; "
    "item[0] = { properties = { num = 2; item[0] = { path = \"health\"; type = \"float\"; } "
    "item[1] = { path = \"speed\"; type = \"float\"; } } } } } }";
static const char *demo_child_original = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; "
    "propertySheets = { item[0] = { properties = { item[1] = { descriptionTag = \"#str_child\"; } } } } } }";
static const char *rebound_selector = "{ edit = { propertySheets = { num = 1; item[0] = { properties = { num = 1; "
    "item[0] = { path = \"mode\"; type = \"string\"; inspector = \"behavior\"; } } } } } }";
/* A stock child adding two equal controls without a path (native slots 2 and
 * 3, placed by slot alone). */
static const char *demo_anon_original = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; "
    "propertySheets = { item[0] = { properties = { num = 4; item[2] = { type = \"float\"; } "
    "item[3] = { type = \"float\"; } } } } } }";
/* A stock child binding its parent's second control to another field by a
 * sparse patch of slot 1 (like volume/spawn_inhibit_cylinder). */
static const char *demo_rekey_original = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; "
    "propertySheets = { item[0] = { properties = { item[1] = { path = \"scale\"; } } } } } }";
static int contribution_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = NULL;
    if (!strcmp(path, "generated/decls/snapeditorentitydef/rebound.decl")) {
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){rebound_selector, strlen(rebound_selector)}, NULL, 0);
        assert(tree); *body = (unsigned char *)sh_decl_tree_write(tree, length); sh_decl_tree_free(tree);
        return *body ? 1 : -1;
    }
    if (!strcmp(path, "generated/decls/snapeditorentitydef/mixed.decl"))
        text = "{ edit = { target = \"parent-only\"; propertySheets = { num = 0; } inputs = { num = 0; } } }";
    else if (!strcmp(path, "generated/decls/snappropertyinspector_whitelistencounterdecl/list.decl"))
        text = "{ edit = { mode = \"shared\"; validEncounters = { num = 0; } } }";
    else if (!strcmp(path, "generated/decls/snappropertyinspector_whitelistencounterdecl/behavior.decl"))
        text = "{ edit = { mode = \"stock\"; validEncounters = { num = 0; } } }";
    else if (!strcmp(path, "generated/decls/entitydef/root.decl"))
        text = "{ class = \"Entity\"; edit = { target = \"local-only\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/demo_parent.decl"))
        text = demo_parent_original;
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/demo_child.decl"))
        text = demo_child_original;
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/demo_plain.decl"))
        text = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/demo_anon.decl"))
        text = demo_anon_original;
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/demo_rekey.decl"))
        text = demo_rekey_original;
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/demo_zero.decl"))
        text = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { "
            "properties = { num = 0; } } } } }";
    if (!text) return library_original(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}

static const char *node_value(const sh_decl_node *node)
{
    const char *text; size_t length;
    assert(sh_decl_tree_literal(node, &text, &length));
    /* Tests below compare quoted scalar values through this stable owned copy. */
    static char value[2048]; assert(length < sizeof(value));
    memcpy(value, text, length); value[length] = 0; return value;
}
static const sh_compiled_resource *named_resource(const sh_package_compilation *compiled,
    const char *family, const char *name)
{
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->type && !strcmp(r->type, family) && !strcmp(r->name, name)) return r;
    }
    return NULL;
}
static size_t property_count(const sh_compiled_resource *file, const char *path)
{
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)file->body, file->body_length}, NULL, 0);
    assert(tree); size_t count = 0;
    const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
    const sh_decl_node *sheets = sh_decl_tree_member(edit, "propertySheets");
    for (const sh_decl_node *sheet = sheets ? sheets->children : NULL; sheet; sheet = sheet->next) {
        if (!strcmp(sheet->key, "num")) continue;
        const sh_decl_node *properties = sh_decl_tree_member(sheet, "properties");
        for (const sh_decl_node *p = properties ? properties->children : NULL; p; p = p->next) {
            if (!strcmp(p->key, "num")) continue;
            if (!path || !strcmp(node_value(sh_decl_tree_member(p, "path")), path)) count++;
        }
    }
    sh_decl_tree_free(tree); return count;
}

static void test_contribution_projection(void)
{
    const char *base = "{ class = \"Entity\"; edit = { target = \"base\"; indexed = { num = 3; item[2] = 7; } } }";
    const char *a = "{ class = \"Entity\"; editorVars = { a = 1; } edit = { target = \"red\"; indexed = { num = 3; item[2] = 9; } } }";
    const char *b = "{ class = \"Entity\"; editorVars = { b = 2; } edit = { target = \"green\"; indexed = { num = 3; item[0] = 5; } } }";
    sh_decl_source parts[] = {{a, strlen(a)}, {b, strlen(b)}};
    char error[1024]; size_t length;
    char *out = sh_package_editor_contributions("entitydef", (sh_decl_source){base, strlen(base)},
        parts, 2, parts[0], 1, &length, error, sizeof(error));
    assert(out && strstr(out, "a = 1") && strstr(out, "b = 2") && strstr(out, "\"red\"") &&
        !strstr(out, "\"green\"") && strstr(out, "item[2] = 9") && !strstr(out, "item[0]"));
    /* The entity parser reads editorVars only before edit. */
    assert(strstr(out, "editorVars") < strstr(out, "edit ="));
    free(out);
    {
        const char *late = "{ class = \"Entity\"; edit = { target = \"late\"; } editorVars = { c = 3; } }";
        const char *bare = "{ class = \"Entity\"; edit = { target = \"bare\"; } }";
        const char *placed = "{ class = \"Entity\"; editorVars = { c = 3; } poolCount = 2; edit = { target = \"placed\"; } }";
        sh_decl_source targets[] = {{late, strlen(late)}, {bare, strlen(bare)}, {placed, strlen(placed)}};
        for (size_t t = 0; t < 3; t++) {
            out = sh_package_editor_contributions("entitydef", (sh_decl_source){base, strlen(base)},
                parts, 2, targets[t], 0, &length, error, sizeof(error));
            if (!out) fprintf(stderr, "entity editorVars order %zu: %s\n", t, error);
            assert(out && strstr(out, "editorVars") && strstr(out, "edit =") &&
                strstr(out, "editorVars") < strstr(out, "edit =") && strstr(out, "class") < strstr(out, "editorVars"));
            /* Composed editorVars keep the original's place among the headers. */
            if (t == 2) assert(strstr(out, "editorVars") < strstr(out, "poolCount"));
            free(out);
        }
    }
    {
        /* A selector patching its parent's property by index keeps that sparse
         * inherited edit. Differing parents compose only complete lists. */
        const char *patch = "{ inherit = \"demons/demon_base\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[4] = { descriptionTag = \"#str_patched\"; } } } } } }";
        const char *other = "{ inherit = \"demons/other_base\"; edit = { propertySheets = { item[0] = { properties = { "
            "item[4] = { descriptionTag = \"#str_other\"; } } } } } }";
        const char *full[] = {"{ inherit = \"demons/demon_base\"; edit = { propertySheets = { num = 1; item[0] = { "
            "properties = { num = 1; item[0] = { path = \"health\"; type = \"float\"; } } } } } }",
            "{ inherit = \"demons/other_base\"; edit = { propertySheets = { num = 1; item[0] = { "
            "properties = { num = 1; item[0] = { path = \"speed\"; type = \"float\"; } } } } } }"};
        sh_decl_source one[] = {{patch, strlen(patch)}, {other, strlen(other)}};
        sh_decl_source whole[] = {{full[0], strlen(full[0])}, {full[1], strlen(full[1])}};
        out = sh_package_editor_contributions("snapeditorentitydef", (sh_decl_source){"{}", 2}, one, 1, one[0], 1,
            &length, error, sizeof(error));
        if (!out) fprintf(stderr, "sparse inherited selector: %s\n", error);
        assert(out && strstr(out, "item[4]") && strstr(out, "#str_patched") && !strstr(out, "num") &&
            strstr(out, "inherit = \"demons/demon_base\""));
        free(out);
        assert(!sh_package_editor_contributions("snapeditorentitydef", (sh_decl_source){"{}", 2}, one, 2, one[0], 1,
            &length, error, sizeof(error)));
        out = sh_package_editor_contributions("snapeditorentitydef", (sh_decl_source){"{}", 2}, whole, 2, whole[0], 1,
            &length, error, sizeof(error));
        if (!out) fprintf(stderr, "complete selectors with differing parents: %s\n", error);
        assert(out && strstr(out, "\"health\"") && strstr(out, "\"speed\"") &&
            strstr(out, "inherit = \"demons/demon_base\"") && !strstr(out, "other_base"));
        free(out);
    }
    assert(!sh_package_editor_contribution_phase("snapeditorentitybank"));
    /* Only the block editor composition itself moves may compare unordered. */
    assert(!strcmp(sh_package_editor_reordered_blocks("entitydef"), "editorVars"));
    assert(!strcmp(sh_package_editor_reordered_blocks("snapeditorentitydef"), "edit"));
    assert(!strcmp(sh_package_editor_reordered_blocks("snappropertyinspector_bitflag"), "edit"));
    assert(!sh_package_editor_reordered_blocks("snappropertyinspector_text"));
    assert(!sh_package_editor_reordered_blocks("material"));
    {
        const char *original = "{ class = \"Entity\"; editorVars = { color = (1,1,1,1); flags = { first = 0; second = 0; label = \"base\"; } } edit = { target = \"base\"; } }";
        const char *red = "{ class = \"Entity\"; editorVars = { color = (1,0,0,1); flags = { first = 1; second = 0; label = \"red\"; } } edit = { target = \"red\"; } }";
        const char *green = "{ class = \"Entity\"; editorVars = { color = (0,1,0,1); flags = { first = 0; second = 2; label = \"green\"; } } edit = { target = \"green\"; } }";
        sh_decl_source peer[] = {{red, strlen(red)}, {green, strlen(green)}};
        sh_decl_source baseline = {original, strlen(original)};
        out = sh_package_editor_contributions("entitydef", baseline, peer, 2, peer[0], 1, &length, error, sizeof(error));
        assert(out && strstr(out, "color = (1,0,0,1)") && strstr(out, "label = \"red\"") &&
            strstr(out, "first = 1") && strstr(out, "second = 2") && strstr(out, "target = \"red\""));
        free(out);
        out = sh_package_editor_contributions("entitydef", baseline, peer, 2, peer[1], 1, &length, error, sizeof(error));
        assert(out && strstr(out, "color = (0,1,0,1)") && strstr(out, "label = \"green\"") &&
            strstr(out, "first = 1") && strstr(out, "second = 2") && strstr(out, "target = \"green\""));
        free(out);
        out = sh_package_editor_contributions("entitydef", baseline, peer, 2, baseline, 0, &length, error, sizeof(error));
        assert(out && strstr(out, "target = \"base\""));
        sh_decl_source swapped[] = {peer[1], peer[0]}; size_t other_length;
        char *other = sh_package_editor_contributions("entitydef", baseline, swapped, 2, baseline,
            0, &other_length, error, sizeof(error));
        assert(other && other_length == length && !memcmp(out, other, length)); free(other); free(out);
        out = sh_package_editor_contributions("entitydef", baseline, peer, 2, peer[0], 0, &length, error, sizeof(error));
        other = sh_package_editor_contributions("entitydef", baseline, swapped, 2, peer[1], 0,
            &other_length, error, sizeof(error));
        assert(out && other && strstr(out, "label = \"green\"") && strstr(other, "label = \"green\"") &&
            strstr(out, "first = 1") && strstr(other, "first = 1") &&
            strstr(out, "second = 2") && strstr(other, "second = 2"));
        free(out); free(other);
        /* The internal preference must never resolve gameplay contradictions. */
        out = sh_decl_compose_editor_variant(baseline, peer, 2, peer[0], &length, error, sizeof(error));
        assert(!out && strstr(error, "edit.target"));
    }
}

static void test_bound_contributions(fixture *f, sh_decl_registry_source registry)
{
    const char *folders[] = {"red", "green", "duplicate"};
    char temporary[MAX_PATH], relative[256], text[4096], error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-contributions-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 3; i++) {
        int green = i == 1;
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, "{\"id\":\"same.id\",\"name\":\"Controls\"}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/entitydef/root.decl", folders[i]);
        create(relative, green ? "{ inherit = \"base\"; editorVars = { greenControl = 1; } edit = { target = \"modifier-child\"; } }" :
            "{ inherit = \"base\"; editorVars = { redControl = 1; } edit = { target = \"parent-only\"; } }");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/entitydef/action.decl", folders[i]);
        create(relative, green ? "{ class = \"Entity\"; edit = { target = \"modifier-child\"; } }" :
            "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/snappropertyinspector_whitelistencounterdecl/list.decl", folders[i]);
        snprintf(text, sizeof(text), "{ edit = { mode = \"shared\"; validEncounters = { num = 1; item[0] = \"%s\"; } } }",
            green ? "modifier-child" : "parent-only"); create(relative, text);
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/snappropertyinspector_whitelistencounterdecl/behavior.decl", folders[i]);
        snprintf(text, sizeof(text), "{ edit = { mode = \"%s\"; validEncounters = { num = 0; } } }", green ? "green" : "red"); create(relative, text);
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/snapeditorentitydef/mixed.decl", folders[i]);
        snprintf(text, sizeof(text),
            "{ edit = { target = \"root\"; propertySheets = { num = 2; "
            "item[%d] = { properties = { num = 4; "
            "item[0] = { path = \"value\"; type = \"string\"; inspector = \"list\"; action = \"action\"; displayNameTag = \"%s\"; } "
            "item[1] = { path = \"inspect\"; type = \"string\"; inspector = \"list\"; displayNameTag = \"%s\"; } "
            "item[2] = { path = \"nativeType\"; type = \"%s\"; } "
            "item[3] = { path = \"mode\"; type = \"string\"; inspector = \"behavior\"; } } } "
            "item[%d] = { properties = { num = 0; } } } "
            "inputs = { num = 3; item[0] = \"%s\"; item[1] = \"parent-only\"; item[2] = \"%s\"; } } }",
            green ? 1 : 0, green ? "Green" : "Red", green ? "Other label" : "First label", green ? "string" : "float",
            green ? 0 : 1, green ? "modifier-child" : "action", green ? "action" : "modifier-child");
        create(relative, text);
    }
    {
        size_t length; unsigned char *body;
        assert(contribution_original(&originals, "generated/decls/snapeditorentitydef/rebound.decl", &body, &length) == 1);
        create("overrides/red/assets/generated/decls/snapeditorentitydef/rebound.decl", (char *)body); free(body);
    }
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    env.baseline = contribution_original; env.baseline_context = &originals;
    const char *product = "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"product\"; type = \"float\"; } "
        "item[1] = { path = \"inspect\"; type = \"string\"; inspector = \"list\"; displayNameTag = \"Product label\"; } } } } } }";
    sh_package_builtin builtin = {"generated/decls/snapeditorentitydef/mixed.decl", (const unsigned char *)product, strlen(product)};
    env.builtins = &builtin; env.builtin_count = 1;
    int ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "bound contributions: %s\n", error);
    assert(ok);
    size_t selectors = 0, roots = 0, actions = 0;
    const sh_compiled_resource *inspector = named_resource(library.compiled,
        "snappropertyinspector_whitelistencounterdecl", "list");
    assert(inspector && strstr((char *)inspector->body, "\"parent-only\"") &&
        strstr((char *)inspector->body, "\"modifier-child\""));
    assert(inspector->source_count == 3);
    for (size_t i = 0; i < library.compiled->resource_count; i++) {
        const sh_compiled_resource *file = library.compiled->resources + i;
        const char *authored = file->original_path ? file->original_path : file->engine_path;
        assert(!strstr((char *)file->body, "smp-contribution/"));
        if (!strcmp(authored, "generated/decls/snapeditorentitydef/mixed.decl")) {
            selectors++;
            /* Inspectors with different authored lists became equal in phase 1;
             * labels/pages do not multiply their control in phase 3. */
            assert(property_count(file, "inspect") == 1);
            assert(property_count(file, "value") == 2); /* Same action name, different bound behavior. */
            assert(property_count(file, "nativeType") == 2);
            assert(property_count(file, "mode") == 2); /* Different noncollection inspector behavior. */
            assert(property_count(file, "product") == 1 && property_count(file, NULL) == 8);
            sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)file->body, file->body_length}, NULL, 0);
            const sh_decl_node *edit = sh_decl_tree_member(tree, "edit"), *inputs = sh_decl_tree_member(edit, "inputs");
            assert(!strcmp(node_value(sh_decl_tree_member(inputs, "num")), "4"));
            const char *target = node_value(sh_decl_tree_member(edit, "target"));
            assert(named_resource(library.compiled, "entitydef", target));
            const sh_decl_node *sheets = sh_decl_tree_member(edit, "propertySheets");
            char action_names[2][1024] = {{0}}, behavior_names[2][1024] = {{0}};
            size_t action_count = 0, behavior_count = 0;
            for (const sh_decl_node *sheet = sheets->children; sheet; sheet = sheet->next) {
                const sh_decl_node *properties = sh_decl_tree_member(sheet, "properties");
                for (const sh_decl_node *p = properties ? properties->children : NULL; p; p = p->next) {
                    if (!strcmp(p->key, "num")) continue;
                    const char *path = node_value(sh_decl_tree_member(p, "path"));
                    if (!strcmp(path, "value")) {
                        assert(action_count < 2);
                        snprintf(action_names[action_count++], sizeof(*action_names), "%s", node_value(sh_decl_tree_member(p, "action")));
                    } else if (!strcmp(path, "mode")) {
                        assert(behavior_count < 2);
                        snprintf(behavior_names[behavior_count++], sizeof(*behavior_names), "%s", node_value(sh_decl_tree_member(p, "inspector")));
                    } else if (!strcmp(path, "inspect")) assert(!strcmp(node_value(sh_decl_tree_member(p, "inspector")), "list"));
                }
            }
            assert(action_count == 2 && strcmp(action_names[0], action_names[1]));
            assert(behavior_count == 2 && strcmp(behavior_names[0], behavior_names[1]));
            const sh_compiled_resource *first_action = named_resource(library.compiled, "entitydef", action_names[0]);
            const sh_compiled_resource *second_action = named_resource(library.compiled, "entitydef", action_names[1]);
            assert(first_action && second_action &&
                !!strstr((char *)first_action->body, "modifier-child") != !!strstr((char *)second_action->body, "modifier-child"));
            assert(named_resource(library.compiled, "snappropertyinspector_whitelistencounterdecl", behavior_names[0]) &&
                named_resource(library.compiled, "snappropertyinspector_whitelistencounterdecl", behavior_names[1]));
            sh_decl_tree_free(tree);
        }
        if (!strcmp(authored, "generated/decls/entitydef/root.decl") && strcmp(file->name, "root")) {
            roots++; assert(strstr((char *)file->body, "redControl = 1") && strstr((char *)file->body, "greenControl = 1"));
            assert(!strstr((char *)file->body, "\"local-only\""));
            assert(sh_package_owners_count(&file->gameplay_owners) == (strstr((char *)file->body, "modifier-child") ? 1 : 2));
        }
        if (!strcmp(authored, "generated/decls/entitydef/action.decl")) actions++;
    }
    if (selectors != 3 || roots != 2 || actions != 2) {
        fprintf(stderr, "contribution results: selectors=%zu roots=%zu actions=%zu\n", selectors, roots, actions);
        for (size_t i = 0; i < library.compiled->resource_count; i++)
            fprintf(stderr, "  %s\n", library.compiled->resources[i].engine_path);
    }
    assert(selectors == 3 && roots == 2 && actions == 2);
    assert(named_resource(library.compiled, "snapeditorentitydef", "mixed"));
    assert(named_resource(library.compiled, "entitydef", "root"));
    {
        /* Red bundles a byte-faithful copy of the shipped selector and changes
         * the behavior inspector it shows: that copy is not an edit to the
         * shipped selector (which keeps the shipped inspector), but red's
         * scope binds its own variant of it, showing red's inspector. */
        const sh_compiled_resource *shipped = named_resource(library.compiled, "snapeditorentitydef", "rebound"), *own = NULL;
        for (size_t i = 0; i < library.compiled->resource_count; i++) {
            const sh_compiled_resource *file = library.compiled->resources + i;
            if (file->original_path && !strcmp(file->original_path, "generated/decls/snapeditorentitydef/rebound.decl") &&
                strcmp(file->name, "rebound")) own = file;
        }
        assert(!shipped || strstr((char *)shipped->body, "\"behavior\""));
        assert(own && property_count(own, NULL) == 1);
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)own->body, own->body_length}, NULL, 0);
        const sh_decl_node *item = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
            sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties"), "item[0]"), "inspector");
        const char *name = node_value(item);
        assert(strcmp(name, "behavior"));
        const sh_compiled_resource *bound = named_resource(library.compiled, "snappropertyinspector_whitelistencounterdecl", name);
        assert(bound && strstr((char *)bound->body, "\"red\""));
        sh_decl_tree_free(tree);
    }
    {
        sh_package_provenance recorded = {0}; sh_package_library restored = {0};
        size_t red = SIZE_MAX, green = SIZE_MAX;
        for (size_t i = 0; i < sources->package_count; i++) {
            if (!strcmp(sources->packages[i].name, "red")) red = i;
            if (!strcmp(sources->packages[i].name, "green")) green = i;
        }
        assert(red != SIZE_MAX && green != SIZE_MAX);
        size_t old = sh_package_provenance_context_add(&recorded, SH_PACKAGE_CONTEXT_COMPOSED,
            sources->fingerprints + red, 1, NULL);
        size_t modern = sh_package_provenance_context_add(&recorded, SH_PACKAGE_CONTEXT_ISOLATED,
            sources->fingerprints + green, 1, "");
        assert(old != SIZE_MAX && modern != SIZE_MAX);
        assert(sh_package_provenance_bind(&recorded, "snapeditorentitydef", "mixed", "historical-selector", &old, 1));
        assert(sh_package_provenance_bind(&recorded, "entitydef", "root", "historical-root", &old, 1));
        env.builtins = NULL; env.builtin_count = 0;
        assert(sh_package_library_compile_contexts(sources, &recorded, &env, NULL, contribution_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &restored, error, sizeof(error)));
        const char *selected = NULL;
        assert(sh_package_provenance_resolve(&recorded, &restored.bindings, "snapeditorentitydef", "historical-selector", &selected) == 1);
        const sh_compiled_resource *historical = named_resource(restored.compiled, "snapeditorentitydef", selected);
        assert(historical && property_count(historical, NULL) == 4 && property_count(historical, "value") == 1);
        assert(sh_package_provenance_resolve(&recorded, &restored.bindings, "entitydef", "historical-root", &selected) == 1);
        historical = named_resource(restored.compiled, "entitydef", selected);
        assert(historical && strstr((char *)historical->body, "redControl") && !strstr((char *)historical->body, "greenControl"));
        sh_package_library_free(&restored); sh_package_provenance_free(&recorded);
    }
    for (size_t i = 0; i < sources->file_count; i++) if (!sources->files[i].directory) assert(sh_package_source_verify(sources->files + i));
    sh_package_library_free(&library); sh_package_sources_free(sources); cleanup(); assert(!failures);
}

/* The user's editor package changed demons/demon_base's placement flags and
 * demons/archvile_enc's placeable flag, keeping archvile's stock sparse patch
 * of its parent's property list. Different parent bindings with one parent
 * declaration compose; a different parent declaration cannot, and then only
 * its sole contributing package is left unserved, with the unit named. */
static void test_inherited_editor_parent(fixture *f, sh_decl_registry_source registry)
{
    char temporary[MAX_PATH], error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    size_t unserved = SIZE_MAX;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-editor-parent-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    create("overrides/editor/package.json", "{\"id\":\"local.editor\",\"name\":\"editor\"}");
    create("overrides/editor/assets/generated/decls/snapeditorentitydef/demo_parent.decl",
        "{ edit = { target = \"root\"; propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"health\"; type = \"float\"; } item[1] = { path = \"speed\"; type = \"float\"; } } } } } }");
    create("overrides/editor/assets/generated/decls/snapeditorentitydef/demo_child.decl",
        "{ inherit = \"demo_parent\"; edit = { target = \"root\"; "
        "propertySheets = { item[0] = { properties = { item[1] = { descriptionTag = \"#str_package\"; } } } } } }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    env.baseline = contribution_original; env.baseline_context = &originals;
    env.unserved_package = &unserved;
    int ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "inherited editor parent: %s\n", error);
    assert(ok && unserved == SIZE_MAX);
    size_t children = 0;
    for (size_t i = 0; i < library.compiled->resource_count; i++) {
        const sh_compiled_resource *file = library.compiled->resources + i;
        const char *authored = file->original_path ? file->original_path : file->engine_path;
        if (strcmp(authored, "generated/decls/snapeditorentitydef/demo_child.decl")) continue;
        children++;
        /* The patch keeps addressing the parent's second property by index. */
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)file->body, file->body_length}, NULL, 0);
        const sh_decl_node *properties = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
            sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties");
        assert(properties && !sh_decl_tree_member(properties, "num") &&
            !strcmp(node_value(sh_decl_tree_member(sh_decl_tree_member(properties, "item[1]"), "descriptionTag")), "#str_package"));
        const char *parent = node_value(sh_decl_tree_member(tree, "inherit"));
        const sh_compiled_resource *bound = named_resource(library.compiled, "snapeditorentitydef", parent);
        assert(bound && strstr((char *)bound->body, "\"speed\""));
        sh_decl_tree_free(tree);
    }
    assert(children >= 1);
    sh_package_library_free(&library); sh_package_sources_free(sources);
    /* A different parent declaration is its author's own variant: it keeps
     * its private identity and parent, and the stock child still composes. */
    create("overrides/editor/assets/generated/decls/snapeditorentitydef/other_parent.decl",
        "{ edit = { target = \"root\"; propertySheets = { num = 1; item[0] = { properties = { num = 1; "
        "item[0] = { path = \"armor\"; type = \"float\"; } } } } } }");
    create("overrides/editor/assets/generated/decls/snapeditorentitydef/demo_child.decl",
        "{ inherit = \"other_parent\"; edit = { target = \"root\"; "
        "propertySheets = { item[0] = { properties = { item[0] = { descriptionTag = \"#str_other\"; } } } } } }");
    create("overrides/zzz-peer/package.json", "{\"id\":\"local.peer\",\"name\":\"peer\"}");
    create("overrides/zzz-peer/assets/generated/decls/entitydef/peer_only.decl", "{ class = \"Entity\"; edit = { } }");
    /* The peer changes the stock parent too and extends the stock child: the
     * stock child it reaches in its own scope is not the peer's contribution. */
    create("overrides/zzz-peer/assets/generated/decls/snapeditorentitydef/peer_grandchild.decl",
        "{ inherit = \"demo_child\"; edit = { target = \"root\"; } }");
    create("overrides/zzz-peer/assets/generated/decls/snapeditorentitydef/demo_parent.decl",
        "{ edit = { target = \"modifier-child\"; propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"health\"; type = \"float\"; } item[1] = { path = \"speed\"; type = \"float\"; } } } } } }");
    for (int round = 0; round < 2; round++) {
        size_t invalid = SIZE_MAX, variants = 0, stock = 0;
        if (round) {
            /* Another package keeps the stock parent: it composes with stock. */
            create("overrides/zzz-peer/assets/generated/decls/snapeditorentitydef/demo_child.decl",
                "{ inherit = \"demo_parent\"; edit = { target = \"root\"; "
                "propertySheets = { item[0] = { properties = { item[1] = { descriptionTag = \"#str_peer\"; } } } } } }");
        }
        env.invalid_package = &invalid;
        sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "changed editor parent %d: %s\n", round, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        for (size_t i = 0; i < library.compiled->resource_count; i++) {
            const sh_compiled_resource *file = library.compiled->resources + i;
            const char *authored = file->original_path ? file->original_path : file->engine_path;
            if (strcmp(authored, "generated/decls/snapeditorentitydef/demo_child.decl")) continue;
            if (strstr((char *)file->body, "#str_other")) {
                variants++; assert(!strstr((char *)file->body, "#str_peer") && strcmp(file->name, "demo_child"));
            } else if (!strcmp(file->name, "demo_child")) {
                stock++; assert(!!strstr((char *)file->body, "#str_peer") == round);
            }
        }
        if (variants != 1 || stock != 1) fprintf(stderr, "changed editor parent %d: variants=%zu stock=%zu\n", round, variants, stock);
        assert(variants == 1 && stock == 1);
        env.invalid_package = NULL;
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    cleanup(); assert(!failures);
}

/* Toybox label aliases (package_library.c, strids.h): the same library
 * compiled without and with a native string reservation publishes the same
 * resources, names, bodies and bindings; only the text served for each
 * placeable package row differs, by its displayNameTag alone, to an alias
 * whose mapping keeps the row's effective base label and names its published
 * row, category keys and sources. Exact copies from two packages are one row
 * whose alias names both. */
static const char *label_stock_row = "{ edit = { displayNameTag = \"#str_speaker\"; categoryNameTag = \"#str_cat_audio\"; "
    "metaCategoryNameTag = \"#str_meta_toybox\"; entityDef = \"speaker_def\"; } }";
/* A valid shipped row repeating fields, as fx/ambient_embers repeats fxInfo:
 * the native reader applies them in order, so the last label is effective. */
static const char *label_repeated_row = "{ edit = { displayNameTag = \"#str_first\"; fxInfo = { rate = 1; } "
    "fxInfo = { rate = 2; } displayNameTag = \"#str_embers\"; categoryNameTag = \"#str_cat_audio\"; "
    "metaCategoryNameTag = \"#str_meta_toybox\"; entityDef = \"speaker_def\"; } }";
static int label_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/snapeditorentitydef/audio/2d_speaker.decl") ? label_stock_row :
        !strcmp(path, "generated/decls/snapeditorentitydef/fx/embers.decl") ? label_repeated_row :
        !strcmp(path, "generated/decls/entitydef/speaker_def.decl") ? "{ class = \"Entity\"; edit = { } }" : NULL;
    if (!text) return contribution_original(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static size_t label_reservations;
static int label_reserve(void *context, const char *key, const unsigned char identity[32], const char *mapping)
{ (void)context; (void)identity; (void)mapping; assert(!strncmp(key, "#str_smp_loc_", 13)); label_reservations++; return 1; }
static int label_reserve_global(void *context, const char *key) { (void)context; (void)key; return 1; }
static void label_package(const char *folder, const char *id, const char *name, const char *row, const char *def,
    const char *row_text)
{
    char path[512], text[512];
    snprintf(path, sizeof(path), "overrides/%s/package.json", folder);
    snprintf(text, sizeof(text), "{\"id\":\"%s\",\"name\":\"%s\"}", id, name); create(path, text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/snapeditorentitydef/%s.decl", folder, row);
    create(path, row_text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s.decl", folder, def);
    create(path, "{ inherit = \"speaker_def\"; edit = { } }");
}
/* A row's text without its displayNameTag, and that tag. */
static char *label_without_tag(const unsigned char *text, size_t text_length, char **tag)
{
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(const char *)text, text_length}, NULL, 0);
    size_t length = 0; char *out;
    assert(tree);
    *tag = NULL;
    sh_decl_node *edit = (sh_decl_node *)sh_decl_tree_member(tree, "edit");
    for (sh_decl_node **slot = edit ? &edit->children : NULL; slot && *slot; slot = &(*slot)->next)
        if (!strcmp((*slot)->key, "displayNameTag")) {
            sh_decl_node *gone = *slot; *slot = gone->next; gone->next = NULL;
            *tag = _strdup(node_value(gone)); sh_decl_tree_free(gone); break;
        }
    out = sh_decl_tree_write(tree, &length); sh_decl_tree_free(tree); return out;
}
static void test_toybox_label_aliases(fixture *f, sh_decl_registry_source registry)
{
    char temporary[MAX_PATH], error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library plain = {0}, labelled = {0};
    void *records = *(void **)((char *)*(void **)f->reflection + 0x20);
    {
        const char *t[] = {"idDeclEntityDef", "idDeclEntityDef", "idDeclEntityDef", "idList < EditorSheet >",
            "idList < EntityRefs >", "idList < EntityRefs >", "idDeclEntityDef", "idStr", "idStr", "idStr"};
        const char *o[] = {"*", "*", "*", "", "", "", "*", "", "", ""};
        const char *n[] = {"target", "first", "second", "propertySheets", "inputs", "outputs", "entityDef",
            "displayNameTag", "categoryNameTag", "metaCategoryNameTag"};
        fields(f, (char *)records + 6 * 0x38, 10, t, o, n);
    }
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-labels-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    const char *probe = "{ inherit = \"audio/2d_speaker\"; edit = { entityDef = \"%s\"; } }";
    char text[256];
    snprintf(text, sizeof(text), probe, "probe/p7/def");
    label_package("p7", "acceptance.perf.probe2-7", "Perf probe2 7", "probe/p7/row", "probe/p7/def", text);
    snprintf(text, sizeof(text), probe, "probe/p8/def");
    label_package("p8", "acceptance.perf.probe2-8", "Perf probe2 8", "probe/p8/row", "probe/p8/def", text);
    snprintf(text, sizeof(text), probe, "copy/def");
    label_package("copy-a", "local.copy-a", "Copy A", "copy/row", "copy/def", text);
    label_package("copy-b", "local.copy-b", "Copy B", "copy/row", "copy/def", text);
    label_package("lamp", "local.lamp", "Lamps", "lamp/row", "lamp/def", "{ edit = { displayNameTag = \"Lamp\"; "
        "categoryNameTag = \"#str_cat_light\"; metaCategoryNameTag = \"#str_meta_toybox\"; entityDef = \"lamp/def\"; } }");
    label_package("hidden", "local.hidden", "Hidden", "hidden/row", "hidden/def", "{ edit = { entityDef = \"hidden/def\"; } }");
    label_package("embers", "local.embers", "Embers", "embers/row", "embers/def",
        "{ inherit = \"fx/embers\"; edit = { entityDef = \"embers/def\"; } }");
    /* An explicitly empty label is its author's own value, not the stock
     * row's inherited one: nothing to alias. */
    label_package("clear", "local.clear", "Clear", "clear/row", "clear/def",
        "{ inherit = \"audio/2d_speaker\"; edit = { entityDef = \"clear/def\"; displayNameTag = \"\"; } }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    env.baseline = label_original; env.baseline_context = &originals;
    sh_package_library_native none = {0}, reserving = {0};
    reserving.strings = (sh_package_string_reservations){label_reserve, NULL, 4, label_reserve_global};
    int ok = sh_package_library_compile(sources, &env, NULL, label_original, &originals, registry,
        (uintptr_t)f->reflection, &none, &plain, error, sizeof(error));
    if (!ok) fprintf(stderr, "labels without reservation: %s\n", error);
    assert(ok && !plain.compiled->policy.localized_count);
    label_reservations = 0;
    ok = sh_package_library_compile(sources, &env, NULL, label_original, &originals, registry,
        (uintptr_t)f->reflection, &reserving, &labelled, error, sizeof(error));
    if (!ok) fprintf(stderr, "labels with reservation: %s\n", error);
    assert(ok);
    /* Same resources, names and bodies; only served texts carry aliases. */
    assert(plain.compiled->resource_count == labelled.compiled->resource_count);
    size_t aliased = 0, copies = 0;
    for (size_t i = 0; i < plain.compiled->resource_count; i++) {
        const sh_compiled_resource *a = plain.compiled->resources + i, *b = labelled.compiled->resources + i;
        assert(!strcmp(a->engine_path, b->engine_path));
        assert((!a->name && !b->name) || (a->name && b->name && !strcmp(a->name, b->name)));
        assert(a->body_length == b->body_length && !memcmp(a->body, b->body, a->body_length));
        assert(!a->presented);
        const char *authored = a->original_path ? a->original_path : a->engine_path;
        if (strstr(authored, "hidden/row") || strstr(authored, "clear/row")) assert(!b->presented);
        if (!b->presented) continue;
        assert(b->type && !strcmp(b->type, "snapeditorentitydef"));
        size_t served_length = 0;
        unsigned char *served = sh_package_compilation_read_served(labelled.compiled, b, SIZE_MAX, &served_length,
            error, sizeof(error));
        assert(served && served_length == b->presented_length && !memcmp(served, b->presented, served_length));
        free(served);
        char *before_tag = NULL, *after_tag = NULL;
        char *before = label_without_tag(b->body, b->body_length, &before_tag);
        char *after = label_without_tag(b->presented, b->presented_length, &after_tag);
        assert(before && after && !strcmp(before, after));
        assert(after_tag && !strncmp(after_tag, "#str_smp_loc_", 13));
        {
            aliased++;
            const sh_package_localized_string *entry = NULL;
            for (size_t k = 0; k < labelled.compiled->policy.localized_count; k++)
                if (!strcmp(labelled.compiled->policy.localized[k].key, after_tag)) entry = labelled.compiled->policy.localized + k;
            assert(entry);
            sh_json_object mapping = {0}, label = {0}, list = {0};
            assert(sh_json_parse_object(entry->mapping, strlen(entry->mapping), 8, &mapping));
            const char *raw = sh_json_object_get(&mapping, "label");
            assert(raw && sh_json_parse_object(raw, strlen(raw), 8, &label));
            char row[160], native[64], category[64]; size_t length;
            assert(sh_json_decode_string(sh_json_object_get(&label, "row"), strlen(sh_json_object_get(&label, "row")), row, sizeof(row), &length));
            assert(!strcmp(row, b->name));
            raw = sh_json_object_get(&mapping, "native");
            assert(sh_json_decode_string(raw, strlen(raw), native, sizeof(native), &length));
            raw = sh_json_object_get(&label, "category");
            assert(sh_json_decode_string(raw, strlen(raw), category, sizeof(category), &length));
            raw = sh_json_object_get(&label, "sources");
            assert(raw && sh_json_parse_object(raw, strlen(raw), 8, &list));
            if (strstr(authored, "lamp/row")) assert(!strcmp(native, "Lamp") && !strcmp(category, "#str_cat_light"));
            else if (strstr(authored, "embers/row")) assert(!strcmp(native, "#str_embers") && !strcmp(category, "#str_cat_audio"));
            else assert(!strcmp(native, "#str_speaker") && !strcmp(category, "#str_cat_audio"));
            if (strstr(authored, "copy/row")) {
                copies++;
                assert(list.count == 2 && strstr(raw, "Copy A") && strstr(raw, "Copy B") &&
                    strstr(raw, "local.copy-a") && strstr(raw, "local.copy-b"));
            } else assert(list.count == 1);
            sh_json_object_free(&mapping); sh_json_object_free(&label); sh_json_object_free(&list);
        }
        free(before); free(after); free(before_tag); free(after_tag);
    }
    if (aliased != 5 || copies != 1) fprintf(stderr, "label aliases: aliased=%zu copies=%zu\n", aliased, copies);
    assert(aliased == 5 && copies == 1 && label_reservations == 5);
    /* Saved bindings from either compile name the same published identities. */
    size_t plain_length = 0, labelled_length = 0;
    unsigned char *plain_bindings = sh_package_provenance_encode(&plain.bindings, &plain_length, error, sizeof(error));
    unsigned char *labelled_bindings = sh_package_provenance_encode(&labelled.bindings, &labelled_length, error, sizeof(error));
    assert(plain_bindings && labelled_bindings && plain_length == labelled_length &&
        !memcmp(plain_bindings, labelled_bindings, plain_length));
    free(plain_bindings); free(labelled_bindings);
    sh_package_library_free(&plain); sh_package_library_free(&labelled); sh_package_sources_free(sources);
    {
        const char *t[] = {"idDeclEntityDef", "idDeclEntityDef", "idDeclEntityDef", "idList < EditorSheet >",
            "idList < EntityRefs >", "idList < EntityRefs >"};
        const char *o[] = {"*", "*", "*", "", "", ""}, *n[] = {"target", "first", "second", "propertySheets", "inputs", "outputs"};
        fields(f, (char *)records + 6 * 0x38, 6, t, o, n);
    }
    cleanup(); assert(!failures);
}

/* A package relaying a parent's controls (armor first) leaves each sparse
 * child patch on its intended control, for the stock child and a peer's
 * unchanged stock copy alike; failures keep their cause: one package's own
 * invalid list rejects it, a composition the product cannot perform leaves
 * a contributor unserved, and an unreadable source blames nobody. */
/* Packages that ship an editor declaration exactly as SnapMap does (here in
 * its compact original spelling) contribute nothing: composing the group
 * leaves each copy's tree as it was, so each keeps its exact bytes and native
 * identity instead of a respelled private copy, and nothing that references
 * it turns private with it. */
static void test_unchanged_editor_copies(fixture *f, sh_decl_registry_source registry)
{
    char temporary[MAX_PATH], error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    size_t found = 0;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-unchanged-editor-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    env.baseline = contribution_original; env.baseline_context = &originals;
    create("overrides/one/package.json", "{\"id\":\"local.one\",\"name\":\"one\"}");
    create("overrides/one/assets/generated/decls/snapeditorentitydef/demo_parent.decl", demo_parent_original);
    create("overrides/two/package.json", "{\"id\":\"local.two\",\"name\":\"two\"}");
    create("overrides/two/assets/generated/decls/snapeditorentitydef/demo_parent.decl", demo_parent_original);
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "unchanged editor copies: %s\n", error);
    assert(ok);
    for (size_t i = 0; i < library.compiled->resource_count; i++) {
        const sh_compiled_resource *file = library.compiled->resources + i;
        const char *authored = file->original_path ? file->original_path : file->engine_path;
        if (strcmp(authored, "generated/decls/snapeditorentitydef/demo_parent.decl")) continue;
        found++;
        if (!file->native_original)
            fprintf(stderr, "unchanged editor copy respelled: %.*s\n", (int)file->body_length, (char *)file->body);
        assert(file->native_original && file->body_length == strlen(demo_parent_original) &&
            !memcmp(file->body, demo_parent_original, file->body_length));
    }
    assert(found == 1);
    sh_package_library_free(&library); sh_package_sources_free(sources); cleanup(); assert(!failures);
}

static void test_editor_parent_contracts(fixture *f, sh_decl_registry_source registry)
{
    char temporary[MAX_PATH], error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-editor-contracts-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    env.baseline = contribution_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    create("overrides/layout/package.json", "{\"id\":\"local.layout\",\"name\":\"layout\"}");
    create("overrides/layout/assets/generated/decls/snapeditorentitydef/demo_parent.decl",
        "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; item[0] = { properties = { num = 3; "
        "item[0] = { path = \"armor\"; type = \"float\"; } item[1] = { path = \"health\"; type = \"float\"; } "
        "item[2] = { path = \"speed\"; type = \"float\"; } } } } } }");
    create("overrides/copy/package.json", "{\"id\":\"local.copy\",\"name\":\"copy\"}");
    create("overrides/copy/assets/generated/decls/snapeditorentitydef/demo_child.decl", demo_child_original);
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "relaid editor parent: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    {
        const sh_compiled_resource *child = named_resource(library.compiled, "snapeditorentitydef", "demo_child");
        const sh_compiled_resource *parent = named_resource(library.compiled, "snapeditorentitydef", "demo_parent");
        assert(child && parent && strstr((char *)parent->body, "\"armor\""));
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)child->body, child->body_length}, NULL, 0);
        const sh_decl_node *properties = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
            sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties");
        size_t speed = SIZE_MAX, labelled = 0;
        char slot[32];
        assert(properties && sh_decl_tree_member(properties, "num"));
        for (size_t k = 0; k < 3; k++) {
            snprintf(slot, sizeof(slot), "item[%zu]", k);
            const sh_decl_node *item = sh_decl_tree_member(properties, slot);
            assert(item);
            if (!strcmp(node_value(sh_decl_tree_member(item, "path")), "speed")) speed = k;
            if (sh_decl_tree_member(item, "descriptionTag")) labelled++;
        }
        /* The published parent's slot order is kept; the patch follows speed. */
        assert(speed == 2 && labelled == 1);
        snprintf(slot, sizeof(slot), "item[%zu]", speed);
        assert(!strcmp(node_value(sh_decl_tree_member(sh_decl_tree_member(properties, slot), "descriptionTag")), "#str_child"));
        sh_decl_tree_free(tree);
    }
    sh_package_library_free(&library); sh_package_sources_free(sources);
    /* One package's own input: an unreadable declaration rejects it; a list
     * member no product reader covers leaves it unserved. Peers compile. */
    for (int round = 0; round < 2; round++) {
        size_t *owner = round ? &unserved : &invalid;
        create("overrides/bad/package.json", "{\"id\":\"local.bad\",\"name\":\"bad\"}");
        create("overrides/bad/assets/generated/decls/snapeditorentitydef/demo_child.decl", round ?
            "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { bogus = 1; } } }" :
            "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { num = 1; } }");
        sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        assert(!sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error)));
        if (*owner >= sources->package_count)
            fprintf(stderr, "bad editor input %d: invalid=%zu unserved=%zu %s\n", round, invalid, unserved, error);
        assert(*owner < sources->package_count && !strcmp(sources->packages[*owner].name, "bad"));
        assert((round ? invalid : unserved) == SIZE_MAX);
        assert(sh_package_sources_remove(sources, *owner));
        ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "peers without the bad package %d: %s\n", round, error);
        assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX);
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    create("overrides/bad/assets/generated/decls/snapeditorentitydef/demo_child.decl", demo_child_original);
    /* Environment: an unreadable source blames nothing, not even the package
     * that authored a change to that declaration (layout's relaid parent);
     * the previous usable generation stays with the caller. */
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_library_test_contribution_read_failures("demo_parent", 1);
    assert(!sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error)));
    sh_package_library_test_contribution_read_failures(NULL, 0);
    assert(invalid == SIZE_MAX && unserved == SIZE_MAX && strstr(error, "unreadable") && !library.compiled);
    sh_package_library_free(&library); sh_package_sources_free(sources);
    /* An installation increment compiles each new component alone, as an
     * isolated context: the same inputs keep the same causes there. Round 0
     * is the package's own unreadable declaration, 1 its uncovered member,
     * 2 an unreadable source (environment: nobody). */
    for (int round = 0; round < 3; round++) {
        sh_package_provenance contexts = {0};
        invalid = unserved = SIZE_MAX;
        create("overrides/bad/assets/generated/decls/snapeditorentitydef/demo_child.decl",
            round == 1 ? "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { bogus = 1; } } }" :
            round == 2 ? demo_child_original :
            "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { num = 1; } }");
        sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        for (size_t i = 0; i < sources->component_count; i++) {
            const sh_package_component *component = sources->components + i;
            const unsigned char *tree = sh_package_source_tree(sources, component->owner);
            assert(tree && sh_package_provenance_context_add(&contexts, SH_PACKAGE_CONTEXT_ISOLATED,
                (const unsigned char (*)[32])tree, 1, component->relative) != SIZE_MAX);
        }
        if (round == 2) sh_package_library_test_contribution_read_failures("demo_parent", 1);
        assert(!sh_package_library_compile_contexts(sources, &contexts, &env, NULL, contribution_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error)));
        sh_package_library_test_contribution_read_failures(NULL, 0);
        size_t blamed = round == 1 ? unserved : invalid, other = round == 1 ? invalid : unserved;
        int expected = round < 2 ? blamed < sources->package_count &&
            !strcmp(sources->packages[blamed].name, "bad") && other == SIZE_MAX :
            invalid == SIZE_MAX && unserved == SIZE_MAX;
        if (!expected) fprintf(stderr, "isolated editor input %d: invalid=%zu unserved=%zu %s\n", round, invalid, unserved, error);
        assert(expected && (round != 2 || strstr(error, "unreadable")));
        sh_package_provenance_free(&contexts);
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    invalid = unserved = SIZE_MAX;
    create("overrides/bad/assets/generated/decls/snapeditorentitydef/demo_child.decl", demo_child_original);
    /* Coverage: a parent chain the product cannot follow (circular) leaves
     * its contributing package unserved, not rejected. */
    create("overrides/cycle/package.json", "{\"id\":\"local.cycle\",\"name\":\"cycle\"}");
    create("overrides/cycle/assets/generated/decls/snapeditorentitydef/demo_parent.decl",
        "{ inherit = \"demo_child\"; edit = { target = \"parent-only\"; } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (ok || invalid != SIZE_MAX || unserved >= sources->package_count || strcmp(sources->packages[unserved].name, "cycle"))
        fprintf(stderr, "editor inheritance cycle: ok=%d invalid=%zu unserved=%zu %s\n", ok, invalid, unserved, error);
    assert(!ok && invalid == SIZE_MAX && unserved < sources->package_count &&
        !strcmp(sources->packages[unserved].name, "cycle"));
    assert(sh_package_sources_remove(sources, unserved));
    assert(sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error)) && unserved == SIZE_MAX);
    sh_package_library_free(&library); sh_package_sources_free(sources); cleanup(); assert(!failures);
}

/* The published child's first sheet as "path[=tag],..." in slot order. */
static void child_layout(const sh_package_compilation *compiled, char *out, size_t capacity)
{
    const sh_compiled_resource *child = named_resource(compiled, "snapeditorentitydef", "demo_child");
    size_t used = 0;
    out[0] = 0;
    assert(child);
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)child->body, child->body_length}, NULL, 0);
    const sh_decl_node *properties = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
        sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties");
    assert(properties);
    for (size_t k = 0;; k++) {
        char slot[32]; snprintf(slot, sizeof(slot), "item[%zu]", k);
        const sh_decl_node *item = sh_decl_tree_member(properties, slot), *tag;
        if (!item) break;
        const sh_decl_node *path = sh_decl_tree_member(item, "path");
        used += snprintf(out + used, capacity - used, "%s%s", k ? "," : "", path ? node_value(path) : "?");
        if ((tag = sh_decl_tree_member(item, "descriptionTag")))
            used += snprintf(out + used, capacity - used, "=%s", node_value(tag));
        assert(used < capacity);
    }
    sh_decl_tree_free(tree);
}

/* One package relays the parent (armor first) and also carries demo_child:
 * - an unchanged stock copy was authored against the stock parent, not the
 *   relaid parent its own scope reaches, so its patch stays on speed;
 * - its own child authored against the relaid parent patches armor there and
 *   restates the whole list, so the stock child's label on speed is its own
 *   removal rather than a sparse patch the others cannot align with. */
static void test_editor_rebase_scopes(fixture *f, sh_decl_registry_source registry)
{
    static const char *children[] = {NULL,
        "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { "
        "properties = { item[0] = { descriptionTag = \"#str_armor\"; } } } } } }"};
    static const char *expected[] = {"armor,health,speed=#str_child", "armor=#str_armor,health,speed"};
    char temporary[MAX_PATH], error[1024], layout[512];
    source_link_fixture originals = {f};
    for (int round = 0; round < 2; round++) {
        sh_package_compile_environment env = {0}; sh_package_library library = {0};
        size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
        assert(GetTempPathA(sizeof(temporary), temporary));
        snprintf(root, sizeof(root), "%ssmp-editor-scopes-%d-%lu-%llu", temporary, round, GetCurrentProcessId(), GetTickCount64());
        assert(CreateDirectoryA(root, NULL));
        env.baseline = contribution_original; env.baseline_context = &originals;
        env.unserved_package = &unserved; env.invalid_package = &invalid;
        create("overrides/layout/package.json", "{\"id\":\"local.layout\",\"name\":\"layout\"}");
        create("overrides/layout/assets/generated/decls/snapeditorentitydef/demo_parent.decl",
            "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; item[0] = { properties = { num = 3; "
            "item[0] = { path = \"armor\"; type = \"float\"; } item[1] = { path = \"health\"; type = \"float\"; } "
            "item[2] = { path = \"speed\"; type = \"float\"; } } } } } }");
        create("overrides/layout/assets/generated/decls/snapeditorentitydef/demo_child.decl",
            children[round] ? children[round] : demo_child_original);
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "editor rebase scopes %d: %s\n", round, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        child_layout(library.compiled, layout, sizeof(layout));
        if (strcmp(layout, expected[round])) fprintf(stderr, "editor rebase scopes %d: %s\n", round, layout);
        assert(!strcmp(layout, expected[round]));
        sh_package_library_free(&library); sh_package_sources_free(sources); cleanup();
    }
    assert(!failures);
}


/* A reflected declaration of another family (a package weapon inheriting a
 * stock weapon) is walked in its effective inherited state; its parent and
 * the parent's closure stay dependencies, so the closure is complete. */
static void test_inherited_reflected_state(fixture *f, sh_decl_registry_source registry)
{
    const char *texts[] = {"{ inherit = \"mod\"; edit = { } }", "{ inherit = \"mod\"; edit = { target = \"local-only\"; } }",
        "{ inherit = \"absent-parent\"; edit = { } }"};
    char error[1024];
    for (size_t i = 0; i < 3; i++) {
        sh_compiled_resource file = link_decl("child", "generated/decls/actormodifier/child.decl", texts[i]);
        file.type = "actormodifier";
        sh_package_compilation compiled = {0}; compiled.resources = &file; compiled.resource_count = 1;
        sh_package_references paths = {0}; sh_package_source_graph_report report = {0};
        const char *json = "{\"targetType\":\"idDeclActorModifier\",\"value\":\"child\"}";
        int ok = sh_package_source_graph(&compiled, NULL, original, f, registry, (uintptr_t)f->reflection,
            json, strlen(json), &paths, &report, error, sizeof(error));
        if (i < 2 && (!ok || paths.incomplete || report.gaps))
            fprintf(stderr, "inherited reflected state %zu: ok=%d gaps=%zu %s %s\n", i, ok, report.gaps, report.first_gap, error);
        if (i < 2) {
            assert(ok && !paths.incomplete && !report.gaps);
            assert(has(&paths, "actormodifier/child.decl") && has(&paths, "actormodifier/mod.decl"));
            assert(has(&paths, "entitydef/modifier-child.decl"));
            assert(!!has(&paths, "entitydef/local-only.decl") == (i == 1));
        } else assert(!ok || paths.incomplete);
        sh_package_references_free(&paths);
    }
}

/* A reader gap inside an unchanged SnapMap original cannot hide candidate
 * content (a changed dependency binds a walked private unit), so it is native;
 * the same gap in a campaign-only original or in candidate bytes still leaves
 * the closure incomplete. */
static int native_gap_scope;
static int native_gap_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = NULL;
    if (!strcmp(path, "generated/decls/actormodifier/stockbroken.decl"))
        text = "{ edit = { target = \"modifier-child\"; } } trailing";
    else if (!strcmp(path, "generated/decls/actormodifier/leafmod.decl")) text = "{ edit = { target = \"leaf-entity\"; } }";
    else if (!strcmp(path, "generated/decls/entitydef/stock-host.decl"))
        text = "{ class = \"Entity\"; edit = { modifier = \"campaignbroken\"; } }";
    else if (!strcmp(path, "generated/decls/actormodifier/campaignbroken.decl")) {
        text = "{ edit = { target = \"modifier-child\"; } } trailing";
        *length = strlen(text); *body = (unsigned char *)_strdup(text);
        return *body ? 2 : -1;
    }
    else if (!strcmp(path, "generated/decls/entitydef/leaf-entity.decl"))
        text = "{ class = \"Entity\"; edit = { image = \"absent/pic\"; } }";
    if (!text) return original(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text);
    return *body ? native_gap_scope : -1;
}
static void test_native_original_gaps(fixture *f, sh_decl_registry_source registry)
{
    char error[1024];
    for (int round = 0; round < 3; round++) {
        sh_compiled_resource files[2];
        files[0] = link_decl("stockbroken", "generated/decls/actormodifier/stockbroken.decl",
            "{ edit = { target = \"modifier-child\"; } } trailing");
        files[0].type = "actormodifier";
        files[1] = link_decl("uses-stock", "generated/decls/entitydef/uses-stock.decl",
            "{ class = \"Entity\"; edit = { modifier = \"stockbroken\"; } }");
        sh_package_compilation compiled = {0};
        compiled.resources = round == 2 ? files : files + 1; compiled.resource_count = round == 2 ? 2 : 1;
        native_gap_scope = round == 1 ? 2 : 1;
        sh_package_references paths = {0}; sh_package_source_graph_report report = {0};
        const char *json = "{\"targetType\":\"idDeclEntityDef\",\"value\":\"uses-stock\"}";
        assert(sh_package_source_graph(&compiled, NULL, native_gap_original, f, registry, (uintptr_t)f->reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(has(&paths, "actormodifier/stockbroken.decl"));
        if (!round) assert(!paths.incomplete && !report.gaps && report.native_gaps == 1);
        else assert(paths.incomplete && report.gaps && !report.native_gaps);
        sh_package_references_free(&paths);
    }
    /* A payload only unchanged SnapMap originals name, absent in the game
     * too, is native and not a map requirement; the map naming it as well
     * makes it a real gap and a requirement. */
    native_gap_scope = 1;
    for (int round = 0; round < 2; round++) {
        sh_compiled_resource file = link_decl("uses-leaf", "generated/decls/entitydef/uses-leaf.decl", round ?
            "{ class = \"Entity\"; edit = { modifier = \"leafmod\"; image = \"absent/pic\"; } }" :
            "{ class = \"Entity\"; edit = { modifier = \"leafmod\"; } }");
        sh_package_compilation compiled = {0}; compiled.resources = &file; compiled.resource_count = 1;
        sh_package_references paths = {0}; sh_package_source_graph_report report = {0};
        const char *json = "{\"targetType\":\"idDeclEntityDef\",\"value\":\"uses-leaf\"}";
        assert(sh_package_source_graph(&compiled, NULL, native_gap_original, f, registry, (uintptr_t)f->reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        if (round ? !paths.incomplete : (paths.incomplete || !report.native_gaps))
            fprintf(stderr, "native leaf %d: gaps=%zu native=%zu %s\n", round, report.gaps, report.native_gaps, report.first_gap);
        assert(has(&paths, "entitydef/uses-leaf.decl") && has(&paths, "actormodifier/leafmod.decl"));
        assert(!has(&paths, "entitydef/leaf-entity.decl") && !!has(&paths, "absent/pic") == round);
        if (!round) assert(!paths.incomplete && !report.gaps && report.native_gaps);
        else assert(paths.incomplete && report.gaps && strstr(report.first_gap, "absent/pic"));
        sh_package_references_free(&paths);
    }
    /* A campaign-only declaration with a reader gap that only a SnapMap
     * original names first is walked (depth 2) before a candidate chain names
     * it too (walked at depth 4): its gap waits for the walk's end and then
     * counts; with the original alone naming it, it stays native. */
    for (int round = 0; round < 2; round++) {
        sh_compiled_resource files[5]; /* in engine path order */
        files[0] = link_decl("mid-mod", "generated/decls/actormodifier/mid-mod.decl",
            "{ edit = { target = \"mid-entity2\"; } }");
        files[1] = link_decl("mid-mod2", "generated/decls/actormodifier/mid-mod2.decl",
            "{ edit = { target = \"late-entity\"; } }");
        files[2] = link_decl("late-entity", "generated/decls/entitydef/late-entity.decl", round ?
            "{ class = \"Entity\"; edit = { modifier = \"campaignbroken\"; } }" :
            "{ class = \"Entity\"; edit = { modifier = \"mid-mod\"; } }");
        files[3] = link_decl("mid-entity2", "generated/decls/entitydef/mid-entity2.decl",
            "{ class = \"Entity\"; edit = { modifier = \"mid-mod2\"; } }");
        files[4] = link_decl("uses-late", "generated/decls/entitydef/uses-late.decl",
            "{ class = \"Entity\"; edit = { target = \"stock-host\"; modifier = \"mid-mod\"; } }");
        files[0].type = files[1].type = "actormodifier";
        native_gap_scope = 1;
        sh_package_compilation compiled = {0}; compiled.resources = files; compiled.resource_count = 5;
        sh_package_references paths = {0}; sh_package_source_graph_report report = {0};
        const char *json = "{\"targetType\":\"idDeclEntityDef\",\"value\":\"uses-late\"}";
        assert(sh_package_source_graph(&compiled, NULL, native_gap_original, f, registry, (uintptr_t)f->reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        if (round ? !paths.incomplete : (paths.incomplete || !report.native_gaps))
            fprintf(stderr, "late candidate %d: gaps=%zu native=%zu %s\n", round, report.gaps, report.native_gaps, report.first_gap);
        assert(has(&paths, "entitydef/late-entity.decl") && !!has(&paths, "actormodifier/campaignbroken.decl") == round);
        if (!round) assert(!paths.incomplete && !report.gaps && report.native_gaps);
        else assert(paths.incomplete && report.gaps && strstr(report.first_gap, "campaignbroken"));
        sh_package_references_free(&paths);
    }
}

/* A conductor-shaped entity class: aiTypeList rows of {aiType, entityDef}. */
static void conductor_reflection(fixture *native)
{
    void *container = *(void **)native->reflection;
    void *records = alloc(native, 27 * 0x38), *objects = alloc(native, 27 * 16), *pointers = alloc(native, 27 * 16);
    void *element = alloc(native, 0x48), *mods = alloc(native, 0x48), *choices = alloc(native, 0x48);
    memcpy(records, *(void **)((char *)container + 0x20), 20 * 0x38);
    memcpy(objects, *(void **)((char *)native->reflection + 0x88), 20 * 16);
    memcpy(pointers, *(void **)((char *)native->reflection + 0xa0), 20 * 16);
    pointer(container, 0x20, records);
    pointer(native->reflection, 0x88, objects); number(native->reflection, 0x90, 27);
    pointer(native->reflection, 0xa0, pointers); number(native->reflection, 0xa8, 27);
    /* A bit-flag property inspector declaration: enumType plus values of
     * {enumValue, label}, registered as its own declaration family. */
    pointer(records, 23 * 0x38, string(native, "idDeclSnapPropertyInspector_BitFlag"));
    pointer(records, 23 * 0x38 + 8, string(native, "idDeclTypeInfo"));
    pointer(pointers, 23 * 16 + 8, (void *)0x10);
    pointer(records, 24 * 0x38, string(native, "InspectorChoice")); pointer(records, 24 * 0x38 + 8, string(native, ""));
    pointer(records, 25 * 0x38, string(native, "idList < InspectorChoice >")); pointer(records, 25 * 0x38 + 8, string(native, ""));
    pointer(objects, 25 * 16 + 8, (void *)0x80);
    pointer(records, 25 * 0x38 + 0x18, choices);
    pointer(choices, 0, string(native, "InspectorChoice")); pointer(choices, 8, string(native, ""));
    pointer(choices, 0x10, string(native, "element")); number(choices, 0x1c, 8);
    {
        const char *t[] = {"idStr", "idList < InspectorChoice >"}, *o[] = {"", ""}, *n[] = {"enumType", "values"};
        fields(native, (char *)records + 23 * 0x38, 2, t, o, n);
        const char *ct[] = {"idStr", "idStr"}, *co[] = {"", ""}, *cn[] = {"enumValue", "label"};
        fields(native, (char *)records + 24 * 0x38, 2, ct, co, cn);
    }
    {
        void *managers = alloc(native, 6 * 8), *manager = alloc(native, 0x90);
        memcpy(managers, *(void **)((char *)native->registry + 8), 5 * 8);
        pointer(manager, 8, string(native, "snapPropertyInspector_BitFlag"));
        pointer(manager, 0x10, string(native, "idDeclSnapPropertyInspector_BitFlag"));
        pointer(managers, 5 * 8, manager);
        pointer(native->registry, 8, managers); number(native->registry, 0x10, 6);
    }
    /* globalAIMods rows: {aiType, healthMod} (a string stands in for the float). */
    pointer(records, 21 * 0x38, string(native, "AIMod")); pointer(records, 21 * 0x38 + 8, string(native, ""));
    pointer(records, 22 * 0x38, string(native, "idList < AIMod >")); pointer(records, 22 * 0x38 + 8, string(native, ""));
    pointer(objects, 22 * 16 + 8, (void *)0x80);
    pointer(records, 22 * 0x38 + 0x18, mods);
    pointer(mods, 0, string(native, "AIMod")); pointer(mods, 8, string(native, ""));
    pointer(mods, 0x10, string(native, "element")); number(mods, 0x1c, 8);
    {
        const char *t[] = {"spawnAIType_t", "idStr"}, *o[] = {"", ""}, *n[] = {"aiType", "healthMod"};
        fields(native, (char *)records + 21 * 0x38, 2, t, o, n);
    }
    pointer(records, 19 * 0x38, string(native, "SpawnInfo")); pointer(records, 19 * 0x38 + 8, string(native, ""));
    pointer(records, 20 * 0x38, string(native, "idList < SpawnInfo >")); pointer(records, 20 * 0x38 + 8, string(native, ""));
    pointer(objects, 20 * 16 + 8, (void *)0x80);
    pointer(records, 20 * 0x38 + 0x18, element);
    pointer(element, 0, string(native, "SpawnInfo")); pointer(element, 8, string(native, ""));
    pointer(element, 0x10, string(native, "element")); number(element, 0x1c, 8);
    {
        const char *t[] = {"spawnAIType_t", "idDeclEntityDef"}, *o[] = {"", "*"}, *n[] = {"aiType", "entityDef"};
        fields(native, (char *)records + 19 * 0x38, 2, t, o, n);
    }
    /* The spawn-type enum as reflection registers it: single-bit constants
     * for bits 0-18 (IMP is bit 2, ANY_ZOMBIE bit 17), the "any" bit and the
     * all-bits composite; one record plus terminator. */
    {
        void *enums = alloc(native, 2 * 0x18), *readers = alloc(native, 2 * 16), *members = alloc(native, 22 * 0x10);
        pointer(enums, 0, string(native, "spawnAIType_t")); pointer(enums, 0x10, members);
        for (int i = 0; i < 21; i++) {
            char name[16];
            snprintf(name, sizeof(name), "T%d", i);
            pointer(members, i * 0x10, string(native, i == 2 ? "IMP" : i == 17 ? "ANY_ZOMBIE" : i == 19 ? "ANY" :
                i == 20 ? "ALL" : name));
            number(members, i * 0x10 + 8, i < 19 ? 1 << i : i == 19 ? 1 << 30 : -1);
        }
        pointer(container, 0x10, enums);
        pointer(native->reflection, 0x58, readers); number(native->reflection, 0x60, 2);
    }
    {
        const char *t[] = {"idDeclEntityDef", "OpaqueStringReader", "idDeclActorModifier", "idDeclSnapEditorEntity",
            "idRenderModel", "idImage", "idList < SpawnInfo >", "spawnAIType_t", "idList < AIMod >", "spawnAIType_t"};
        const char *o[] = {"*", "", "*", "*", "*", "*", "", "", "", ""};
        const char *n[] = {"target", "custom", "modifier", "preview", "model", "image", "aiTypeList", "spawnType",
            "globalAIMods", "aiFilter"};
        fields(native, (char *)records + 0x38, 10, t, o, n);
    }
}
static const char *stock_conductor = "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
    "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } } } }";
/* A test's own stock conductor, when it needs more stock rows. */
static const char *registration_conductor;
/* The stock AI filter's inspector: named choices only. */
static const char *stock_choices = "{ edit = { enumType = \"spawnAIType_t\"; values = { num = 3; "
    "item[0] = { enumValue = \"IMP\"; label = \"imp\"; } item[1] = { enumValue = \"T3\"; label = \"t3\"; } "
    "item[2] = { enumValue = \"T4\"; label = \"t4\"; } } } }";
static int registration_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/entitydef/conductor.decl") ?
        (registration_conductor ? registration_conductor : stock_conductor) :
        !strcmp(path, "generated/decls/snappropertyinspector_bitflag/spawnaitypebitflaginspector.decl") ? stock_choices :
        !strcmp(path, "generated/decls/entitydef/imp.decl") || !strcmp(path, "generated/decls/entitydef/t3.decl") ?
        "{ class = \"Entity\"; edit = { } }" : NULL;
    if (!text) return library_original(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static void registration_package(const char *package, const char *key)
{
    char path[512], text[512];
    snprintf(path, sizeof(path), "overrides/%s/package.json", package);
    snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", package, package); create(path, text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", package);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"%s\"; entityDef = \"%s_boss\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = \"%s\"; healthMod = \"%s\"; } } } }",
        key, package, key, package); create(path, text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_boss.decl", package, package);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { target = \"%s\"; } }",
        !strcmp(package, "alpha") ? "parent-only" : !strcmp(package, "beta") ? "modifier-child" : "local-only"); create(path, text);
    /* A spawner naming the key, like an encounter member or spawn target. */
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_spawner.decl", package, package);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { spawnType = \"%lu\"; } }", 4ul | strtoul(key, NULL, 10));
    create(path, text);
    /* An author-facing choice for the key, as the AI filter's inspector offers. */
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/snappropertyinspector_bitflag/%s_types.decl",
        package, package);
    snprintf(text, sizeof(text), "{ edit = { enumType = \"spawnAIType_t\"; values = { num = 2; "
        "item[0] = { enumValue = \"IMP\"; label = \"imp\"; } item[1] = { enumValue = \"%s\"; label = \"%s\"; } } } }",
        key, package); create(path, text);
    /* An AI filter classifying imps and the package's AI. */
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_filter.decl", package, package);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiFilter = \"%lu\"; } }", 4ul | strtoul(key, NULL, 10));
    create(path, text);
}
/* The AI filter mask a package's filter ended with, by authored path. */
static const char *filter_value(const sh_package_compilation *compiled, const char *package)
{
    static char value[64];
    char authored[256];
    snprintf(authored, sizeof(authored), "generated/decls/entitydef/%s_filter.decl", package);
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *file = compiled->resources + i;
        if (strcmp(file->original_path ? file->original_path : file->engine_path, authored)) continue;
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)file->body, file->body_length}, NULL, 0);
        snprintf(value, sizeof(value), "%s", node_value(sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "aiFilter")));
        sh_decl_tree_free(tree); return value;
    }
    return NULL;
}
/* The enumValue a package's inspector offers for its own AI, by authored path. */
static const char *inspector_key(const sh_package_compilation *compiled, const char *package)
{
    static char value[64];
    char authored[256], label[64];
    snprintf(authored, sizeof(authored), "generated/decls/snappropertyinspector_bitflag/%s_types.decl", package);
    snprintf(label, sizeof(label), "label = \"%s\"", package);
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *file = compiled->resources + i;
        if (strcmp(file->original_path ? file->original_path : file->engine_path, authored)) continue;
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)file->body, file->body_length}, NULL, 0);
        const sh_decl_node *values = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "values");
        const char *found = NULL;
        for (const sh_decl_node *item = values ? values->children : NULL; item && !found; item = item->next) {
            if (!item->compound || strcmp(node_value(sh_decl_tree_member(item, "label")), package)) continue;
            snprintf(value, sizeof(value), "%s", node_value(sh_decl_tree_member(item, "enumValue"))); found = value;
        }
        sh_decl_tree_free(tree); return found;
    }
    return NULL;
}
/* The key a package's spawner requests beside IMP, by authored path. */
static uint32_t spawner_request(const sh_package_compilation *compiled, const char *package)
{
    char authored[256];
    snprintf(authored, sizeof(authored), "generated/decls/entitydef/%s_spawner.decl", package);
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *file = compiled->resources + i;
        if (strcmp(file->original_path ? file->original_path : file->engine_path, authored)) continue;
        /* The spawner requests IMP with the key, as one number. */
        const char *at = strstr((const char *)file->body, "spawnType = \"");
        assert(at);
        return (uint32_t)strtoul(at + strlen("spawnType = \""), NULL, 10);
    }
    assert(0); return 0;
}
static const char *spawner_key(const sh_package_compilation *compiled, const char *package)
{
    static char value[64];
    uint32_t requested = spawner_request(compiled, package);
    assert(requested & 4u);
    snprintf(value, sizeof(value), "%u", requested & ~4u); return value;
}
/* One conductor list's row under one key: the named field's value, or NULL
 * (also NULL when two rows carry that key). */
static const char *conductor_list_row(const sh_compiled_resource *conductor, const char *list_name,
    const char *key, const char *field)
{
    static char value[256];
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)conductor->body, conductor->body_length}, NULL, 0);
    const sh_decl_node *list = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), list_name);
    const char *found = NULL;
    int rows = 0;
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next) {
        const sh_decl_node *type = item->compound ? sh_decl_tree_member(item, "aiType") : NULL;
        if (!type || strcmp(node_value(type), key)) continue;
        rows++;
        snprintf(value, sizeof(value), "%s", node_value(sh_decl_tree_member(item, field))); found = value;
    }
    sh_decl_tree_free(tree); return rows == 1 ? found : NULL;
}
static const char *conductor_row(const sh_compiled_resource *conductor, const char *key)
{ return conductor_list_row(conductor, "aiTypeList", key, "entityDef"); }
/* Contexts of the library's key binding (authored bit, bit held here). */
static size_t key_binding(const sh_package_provenance *bindings, const char *original, const char *name)
{
    for (size_t i = 0; i < bindings->binding_count; i++) {
        const sh_package_provenance_binding *binding = bindings->bindings + i;
        if (!strcmp(binding->family, SH_PACKAGE_KEY_FAMILY) && !strcmp(binding->original, original) &&
            !strcmp(binding->name, name)) return binding->context_count;
    }
    return 0;
}
static const char *package_boss(const sh_package_compilation *compiled, const char *package)
{
    char authored[256];
    snprintf(authored, sizeof(authored), "generated/decls/entitydef/%s_boss.decl", package);
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *file = compiled->resources + i;
        if (!strcmp(file->original_path ? file->original_path : file->engine_path, authored)) return file->name;
    }
    return NULL;
}
/* A conductor's aiTypeList is a shared gameplay registration list: each
 * package's rows join the stock-named declaration, each still naming its own
 * package's bound definition, so every package's AI type can spawn. */
static void test_registration_lists(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char temporary[MAX_PATH], error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-registration-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    registration_package("alpha", "524288"); registration_package("beta", "16777216");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "registration lists: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    {
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        const sh_compiled_resource *alpha = NULL, *beta = NULL;
        for (size_t i = 0; i < library.compiled->resource_count; i++) {
            const sh_compiled_resource *file = library.compiled->resources + i;
            const char *authored = file->original_path ? file->original_path : file->engine_path;
            if (!strcmp(authored, "generated/decls/entitydef/alpha_boss.decl")) alpha = file;
            if (!strcmp(authored, "generated/decls/entitydef/beta_boss.decl")) beta = file;
        }
        if (!conductor) for (size_t i = 0; i < library.compiled->resource_count; i++)
            fprintf(stderr, "  %s %s\n", library.compiled->resources[i].engine_path, library.compiled->resources[i].name ? library.compiled->resources[i].name : "");
        assert(conductor && alpha && beta);
        const char *body = (const char *)conductor->body, *first = strstr(body, "\"IMP\"");
        char bound[512];
        assert(strstr(body, "\"524288\"") && strstr(body, "\"16777216\"") && first && !strstr(first + 1, "\"IMP\""));
        snprintf(bound, sizeof(bound), "\"%s\"", alpha->name); assert(strstr(body, bound));
        snprintf(bound, sizeof(bound), "\"%s\"", beta->name); assert(strstr(body, bound));
    }
    sh_package_library_free(&library); sh_package_sources_free(sources);
    /* gamma claims alpha's key for its own definition, and delta registers
     * beta's key for an exactly equal definition. Both distinct AI stay
     * registered: the package first in stable source order keeps 524288, the
     * other moves to the lowest free unnamed bit (bit 20) at its conductor row
     * and spawner alike; delta shares beta's key. Reversing discovery order
     * (renamed folders, same trees) keeps the same choice. */
    registration_package("gamma", "524288"); registration_package("delta", "16777216");
    {
        /* delta's row names beta's boss, of which it ships an exact copy. */
        create("overrides/delta/assets/generated/decls/entitydef/beta_boss.decl",
            "{ class = \"Entity\"; edit = { target = \"modifier-child\"; } }");
        create("overrides/delta/assets/generated/decls/entitydef/conductor.decl",
            "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } "
            "item[1] = { aiType = \"16777216\"; entityDef = \"beta_boss\"; } } "
            "globalAIMods = { num = 1; item[0] = { aiType = \"16777216\"; healthMod = \"beta\"; } } } }");
    }
    char moved[32] = "";
    for (int round = 0; round < 2; round++) {
        if (round) {
            char from[MAX_PATH], to[MAX_PATH];
            snprintf(from, sizeof(from), "%s\\overrides\\alpha", root); snprintf(to, sizeof(to), "%s\\overrides\\zz-alpha", root);
            assert(MoveFileA(from, to));
            snprintf(from, sizeof(from), "%s\\overrides\\gamma", root); snprintf(to, sizeof(to), "%s\\overrides\\aa-gamma", root);
            assert(MoveFileA(from, to));
        }
        sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration collision %d: %s\n", round, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        char alpha[32], gamma[32];
        assert(conductor && spawner_key(library.compiled, "alpha") && spawner_key(library.compiled, "gamma"));
        snprintf(alpha, sizeof(alpha), "%s", spawner_key(library.compiled, "alpha"));
        snprintf(gamma, sizeof(gamma), "%s", spawner_key(library.compiled, "gamma"));
        assert(!strcmp(spawner_key(library.compiled, "beta"), "16777216") &&
            !strcmp(spawner_key(library.compiled, "delta"), "16777216"));
        const char *mover = !strcmp(alpha, "1048576") ? "alpha" : "gamma", *keeper = strcmp(mover, "alpha") ? "alpha" : "gamma";
        if (strcmp(spawner_key(library.compiled, mover), "1048576") || strcmp(spawner_key(library.compiled, keeper), "524288"))
            fprintf(stderr, "registration collision %d: alpha=%s gamma=%s\n", round, alpha, gamma);
        assert(!strcmp(spawner_key(library.compiled, mover), "1048576") && !strcmp(spawner_key(library.compiled, keeper), "524288"));
        const char *row = conductor_row(conductor, "1048576");
        assert(row && !strcmp(row, package_boss(library.compiled, mover)));
        row = conductor_row(conductor, "524288");
        assert(row && !strcmp(row, package_boss(library.compiled, keeper)));
        row = conductor_row(conductor, "16777216");
        const char *beta_boss = package_boss(library.compiled, "beta"), *delta_boss = package_boss(library.compiled, "delta");
        assert(row && ((beta_boss && !strcmp(row, beta_boss)) || (delta_boss && !strcmp(row, delta_boss))));
        /* Each package's AI modifiers follow its (possibly moved) key; the
         * exactly equal registration's equal modifier row appears once. */
        const char *mod = conductor_list_row(conductor, "globalAIMods", "1048576", "healthMod");
        assert(mod && !strcmp(mod, mover));
        mod = conductor_list_row(conductor, "globalAIMods", "524288", "healthMod");
        assert(mod && !strcmp(mod, keeper));
        mod = conductor_list_row(conductor, "globalAIMods", "16777216", "healthMod");
        assert(mod && !strcmp(mod, "beta"));
        /* The moved package's author-facing choice follows its AI. */
        assert(inspector_key(library.compiled, mover) && !strcmp(inspector_key(library.compiled, mover), "1048576"));
        assert(inspector_key(library.compiled, keeper) && !strcmp(inspector_key(library.compiled, keeper), "524288"));
        /* A filter keeps IMP and follows the package's own key. */
        assert(!strcmp(filter_value(library.compiled, mover), "1048580") && !strcmp(filter_value(library.compiled, keeper), "524292"));
        /* Every package key is bound for maps: the moved one to its new bit,
         * the kept one to itself, the shared one in both equal sources. */
        assert(key_binding(&library.bindings, "524288", "1048576") == 1 &&
            key_binding(&library.bindings, "524288", "524288") == 1 &&
            key_binding(&library.bindings, "16777216", "16777216") == 2);
        if (!round) snprintf(moved, sizeof(moved), "%s", mover);
        else assert(!strcmp(moved, mover));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    {
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof(from), "%s\\overrides\\zz-alpha", root); snprintf(to, sizeof(to), "%s\\overrides\\alpha", root);
        assert(MoveFileA(from, to));
        snprintf(from, sizeof(from), "%s\\overrides\\aa-gamma", root); snprintf(to, sizeof(to), "%s\\overrides\\gamma", root);
        assert(MoveFileA(from, to));
    }
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}

static int key_rewrite(void *context, const char *slot, const char *value, size_t length, const char **replacement)
{
    static char owned[256];
    const sh_package_key_move *to = context;
    size_t out_length = 0; int failed = 0;
    char *moved = sh_package_key_relocate(NULL, value, length, to, NULL, &out_length, &failed);
    (void)slot;
    if (failed) return -1;
    if (!moved) return 0;
    assert(out_length < sizeof(owned)); memcpy(owned, moved, out_length + 1); free(moved);
    *replacement = owned; return 1;
}
/* Spawn-type values are flag lists of constant names and decimal numbers; a
 * bit no single-bit constant names is a package's key. Relocation rewrites
 * only decimal tokens, keeps names and spacing, and reaches every reflected
 * spawn-type site of a declaration (both registration lists here). */
/* Rows of one conductor list, in order: "aiType=field" per row. */
static size_t conductor_rows(const sh_compiled_resource *conductor, const char *list_name, const char *field,
    char rows[][96], size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)conductor->body, conductor->body_length}, NULL, 0);
    const sh_decl_node *list = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), list_name);
    size_t count = 0;
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next) {
        if (!item->compound) continue;
        char type[96];
        if (count >= capacity) fprintf(stderr, "conductor rows exceed fixture storage (%zu): %.4096s\n", capacity, conductor->body);
        assert(count < capacity);
        snprintf(type, sizeof(type), "%s", node_value(sh_decl_tree_member(item, "aiType")));
        snprintf(rows[count++], 96, "%s=%s", type, node_value(sh_decl_tree_member(item, field)));
    }
    sh_decl_tree_free(tree); return count;
}
static size_t row_index(char rows[][96], size_t count, const char *row)
{
    for (size_t i = 0; i < count; i++) if (!strcmp(rows[i], row)) return i;
    return SIZE_MAX;
}
/* The spawn-type value of a package spawner that names no custom key. */
static const char *spawner_value(const sh_package_compilation *compiled, const char *package)
{
    static char value[64];
    char authored[256];
    snprintf(authored, sizeof(authored), "generated/decls/entitydef/%s_spawner.decl", package);
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *file = compiled->resources + i;
        if (strcmp(file->original_path ? file->original_path : file->engine_path, authored)) continue;
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)file->body, file->body_length}, NULL, 0);
        snprintf(value, sizeof(value), "%s", node_value(sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "spawnType")));
        sh_decl_tree_free(tree); return value;
    }
    return NULL;
}
static const char *authored_name(const sh_package_compilation *compiled, const char *leaf)
{
    char authored[256];
    snprintf(authored, sizeof(authored), "generated/decls/entitydef/%s.decl", leaf);
    for (size_t i = 0; i < compiled->resource_count; i++) {
        const sh_compiled_resource *file = compiled->resources + i;
        if (!strcmp(file->original_path ? file->original_path : file->engine_path, authored)) return file->name;
    }
    return NULL;
}
static const char *scoped_entity_name(const sh_package_library *library, const sh_package_sources *sources,
    const char *package, const char *original)
{
    for (size_t owner = 0; owner < sources->package_count; owner++) {
        if (strcmp(sources->packages[owner].name, package)) continue;
        const unsigned char *tree = sh_package_source_tree(sources, owner);
        for (size_t i = 0; i < library->bindings.binding_count; i++) {
            const sh_package_provenance_binding *binding = library->bindings.bindings + i;
            if (strcmp(binding->family, "entitydef") || strcmp(binding->original, original)) continue;
            for (size_t j = 0; j < binding->context_count; j++) {
                const sh_package_provenance_context *context = library->bindings.contexts + binding->contexts[j];
                if (context->tree_count == 1 && !memcmp(context->trees[0], tree, 32)) return binding->name;
            }
        }
    }
    return NULL;
}
/* A package whose conductor copy changes the stock IMP row (typed IMP and the
 * any-zombie group bit it shares with T3) to its own AI and IMP's modifier,
 * keeping the stock T3 row; its spawner and inspector name IMP beside a named
 * T3 bit, the group bit and the all-bits composite. */
static void variant_package(const char *package, const char *ai, const char *health)
{
    char path[512], text[768];
    snprintf(path, sizeof(path), "overrides/%s/package.json", package);
    snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", package, package); create(path, text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", package);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = \"IMP ANY_ZOMBIE\"; entityDef = \"%s\"; } "
        "item[1] = { aiType = \"T3 ANY_ZOMBIE\"; entityDef = \"t3\"; } } "
        "globalAIMods = { num = 2; item[0] = { aiType = \"T3\"; healthMod = \"stock-t3\"; } "
        "item[1] = { aiType = \"IMP\"; healthMod = \"%s\"; } } } }", ai, health); create(path, text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s.decl", package, ai);
    create(path, "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_spawner.decl", package, package);
    create(path, "{ class = \"Entity\"; edit = { spawnType = \"IMP T3 ANY_ZOMBIE\"; } }");
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_group_spawner.decl", package, package);
    create(path, "{ class = \"Entity\"; edit = { spawnType = \"ANY_ZOMBIE\"; } }");
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_filter.decl", package, package);
    create(path, "{ class = \"Entity\"; edit = { aiFilter = \"IMP ANY_ZOMBIE\"; } }");
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/snappropertyinspector_bitflag/%s_types.decl",
        package, package);
    snprintf(text, sizeof(text), "{ edit = { enumType = \"spawnAIType_t\"; values = { num = 2; "
        "item[0] = { enumValue = \"IMP\"; label = \"%s\"; } item[1] = { enumValue = \"ALL\"; label = \"all\"; } } } }",
        package); create(path, text);
}
static const char *variant_stock = "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
    "item[0] = { aiType = \"IMP ANY_ZOMBIE\"; entityDef = \"imp\"; } "
    "item[1] = { aiType = \"T3 ANY_ZOMBIE\"; entityDef = \"t3\"; } } "
    "globalAIMods = { num = 2; item[0] = { aiType = \"T3\"; healthMod = \"stock-t3\"; } "
    "item[1] = { aiType = \"IMP\"; healthMod = \"stock\"; } } } }";
static void registration_root(const char *label)
{
    char temporary[MAX_PATH];
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-%s-%lu-%llu", temporary, label, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
}
static void rename_package(const char *from_name, const char *to_name)
{
    char from[MAX_PATH], to[MAX_PATH];
    snprintf(from, sizeof(from), "%s\\overrides\\%s", root, from_name);
    snprintf(to, sizeof(to), "%s\\overrides\\%s", root, to_name);
    assert(MoveFileA(from, to));
}
/* Packages changing one stock row to different AI. fire and ember change IMP
 * to exactly equal AI (ember ships a copy of fire's) and modifier, ice to its
 * own AI with the stock modifier: each distinct change keeps a row typed IMP
 * (and any zombie), now with a private bit, so stock IMP requests pick among
 * the variants and the native type consumers still see IMP; each package's own
 * IMP requests (spawner, inspector choice) move to its private bit alone while
 * the group bit stays shared, and a changed modifier moves there too, ahead of
 * the stock IMP modifier, which stays. solo changes T3 uncontended: its change
 * applies to the stock key as authored. The order follows stable source
 * identity, so reversed discovery gives each package the same key. */
static void test_registration_variants(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024], fire_key[32] = "", ice_key[32] = "";
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    registration_root("registration-variants");
    registration_conductor = variant_stock;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    variant_package("fire", "fire_imp", "fire"); variant_package("ember", "fire_imp", "fire");
    variant_package("ice", "ice_imp", "stock");
    create("overrides/solo/package.json", "{\"id\":\"local.solo\",\"name\":\"solo\"}");
    create("overrides/solo/assets/generated/decls/entitydef/conductor.decl", "{ class = \"Entity\"; edit = { "
        "aiTypeList = { num = 2; item[0] = { aiType = \"IMP ANY_ZOMBIE\"; entityDef = \"imp\"; } "
        "item[1] = { aiType = \"T3 ANY_ZOMBIE\"; entityDef = \"solo_t3\"; } } } }");
    create("overrides/solo/assets/generated/decls/entitydef/solo_t3.decl", "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    create("overrides/solo/assets/generated/decls/entitydef/solo_spawner.decl", "{ class = \"Entity\"; edit = { spawnType = \"T3\"; } }");
    for (int round = 0; round < 2; round++) {
        if (round) { rename_package("fire", "zz-fire"); rename_package("ice", "aa-ice"); }
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration variants %d: %s\n", round, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        library_key_memo_same(&library, sources, &env, registration_original, &originals,
            registry, (uintptr_t)f->reflection, NULL);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        char rows[16][96], mods[16][96], row[160];
        assert(conductor);
        size_t count = conductor_rows(conductor, "aiTypeList", "entityDef", rows, 16);
        size_t mod_count = conductor_rows(conductor, "globalAIMods", "healthMod", mods, 16);
        char fire[64], ember[64], ice[64];
        assert(spawner_value(library.compiled, "fire") && spawner_value(library.compiled, "ember") &&
            spawner_value(library.compiled, "ice"));
        snprintf(fire, sizeof(fire), "%s", spawner_value(library.compiled, "fire"));
        snprintf(ember, sizeof(ember), "%s", spawner_value(library.compiled, "ember"));
        snprintf(ice, sizeof(ice), "%s", spawner_value(library.compiled, "ice"));
        /* The spawner's IMP is the variant's key now, T3 and the group bit
         * as written: one number, as the engine reads unnamed bits. */
        char fire_now[32], ice_now[32];
        snprintf(fire_now, sizeof(fire_now), "%lu", strtoul(fire, NULL, 10) & ~(8ul | 131072ul));
        snprintf(ice_now, sizeof(ice_now), "%lu", strtoul(ice, NULL, 10) & ~(8ul | 131072ul));
        if (round) assert(!strcmp(fire_now, fire_key) && !strcmp(ice_now, ice_key));
        else {
            /* Private bits: the lowest two unnamed ones. */
            snprintf(fire_key, sizeof(fire_key), "%s", fire_now); snprintf(ice_key, sizeof(ice_key), "%s", ice_now);
            assert(strcmp(fire_key, ice_key) && (!strcmp(fire_key, "524288") || !strcmp(fire_key, "1048576")) &&
                (!strcmp(ice_key, "524288") || !strcmp(ice_key, "1048576")));
        }
        snprintf(row, sizeof(row), "%lu", strtoul(fire_key, NULL, 10) | 8ul | 131072ul);
        assert(!strcmp(fire, row) && !strcmp(ember, row));
        snprintf(row, sizeof(row), "%lu", strtoul(ice_key, NULL, 10) | 8ul | 131072ul); assert(!strcmp(ice, row));
        assert(!strcmp(spawner_value(library.compiled, "solo"), "T3"));
        assert(!strcmp(spawner_value(library.compiled, "fire_group"), "ANY_ZOMBIE"));
        /* The stock IMP row is replaced by the two variants (the equal
         * change once), each still typed IMP; solo's T3 change applies. */
        const char *fire_ai = authored_name(library.compiled, "fire_imp");
        const char *ice_ai = authored_name(library.compiled, "ice_imp");
        const char *solo_ai = authored_name(library.compiled, "solo_t3");
        assert(fire_ai && ice_ai && solo_ai && count == 3);
        snprintf(row, sizeof(row), "%lu=%s", 4ul | 131072ul | strtoul(fire_key, NULL, 10), fire_ai);
        assert(row_index(rows, count, row) != SIZE_MAX);
        snprintf(row, sizeof(row), "%lu=%s", 4ul | 131072ul | strtoul(ice_key, NULL, 10), ice_ai);
        assert(row_index(rows, count, row) != SIZE_MAX);
        snprintf(row, sizeof(row), "T3 ANY_ZOMBIE=%s", solo_ai); assert(row_index(rows, count, row) != SIZE_MAX);
        /* fire's modifier precedes the stock IMP modifier, which stays and
         * serves ice's variant; ice's unchanged modifier adds no row. */
        char fire_mod[64];
        snprintf(fire_mod, sizeof(fire_mod), "%s=fire", fire_key);
        size_t stock_mod = row_index(mods, mod_count, "IMP=stock");
        assert(mod_count == 3 && stock_mod != SIZE_MAX && row_index(mods, mod_count, "T3=stock-t3") != SIZE_MAX);
        assert(row_index(mods, mod_count, fire_mod) < stock_mod);
        /* The package's author-facing IMP choice sets AI filters, which
         * classify: it keeps naming every imp, its variant included. */
        const char *choice = inspector_key(library.compiled, "fire");
        assert(choice && !strcmp(choice, "IMP"));
        choice = inspector_key(library.compiled, "ice");
        assert(choice && !strcmp(choice, "IMP"));
        /* So does a filter: it names every imp and zombie as authored. */
        assert(!strcmp(filter_value(library.compiled, "fire"), "IMP ANY_ZOMBIE") &&
            !strcmp(filter_value(library.compiled, "ice"), "IMP ANY_ZOMBIE"));
        /* Maps bind the IMP bit of each changing package to its private bit,
         * or to itself where the change is uncontended. */
        assert(key_binding(&library.bindings, "4", fire_key) == 2 && key_binding(&library.bindings, "4", ice_key) == 1);
        assert(key_binding(&library.bindings, "8", "8") == 1);
        assert(!library.coverage || !strstr(library.coverage, "spawn"));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    rename_package("zz-fire", "fire"); rename_package("aa-ice", "ice");
    /* ice alone: its change applies to the stock key, with no private bit. */
    rename_package("fire", "off-fire"); rename_package("ember", "off-ember");
    {
        char held[MAX_PATH];
        snprintf(held, sizeof(held), "%s\\held", root); assert(CreateDirectoryA(held, NULL));
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof(from), "%s\\overrides\\off-fire", root); snprintf(to, sizeof(to), "%s\\off-fire", held);
        assert(MoveFileA(from, to));
        snprintf(from, sizeof(from), "%s\\overrides\\off-ember", root); snprintf(to, sizeof(to), "%s\\off-ember", held);
        assert(MoveFileA(from, to));
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration variant alone: %s\n", error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        char rows[16][96], mods[16][96], row[160];
        size_t count = conductor_rows(conductor, "aiTypeList", "entityDef", rows, 16);
        size_t mod_count = conductor_rows(conductor, "globalAIMods", "healthMod", mods, 16);
        snprintf(row, sizeof(row), "IMP ANY_ZOMBIE=%s", authored_name(library.compiled, "ice_imp"));
        assert(count == 2 && row_index(rows, count, row) != SIZE_MAX);
        assert(mod_count == 2 && row_index(mods, mod_count, "IMP=stock") != SIZE_MAX);
        assert(!strcmp(spawner_value(library.compiled, "ice"), "IMP T3 ANY_ZOMBIE"));
        assert(key_binding(&library.bindings, "4", "4") == 1);
        sh_package_library_free(&library); sh_package_sources_free(sources);
        assert(MoveFileA(to, from));
        snprintf(from, sizeof(from), "%s\\overrides\\off-fire", root); snprintf(to, sizeof(to), "%s\\off-fire", held);
        assert(MoveFileA(to, from));
        assert(RemoveDirectoryA(held));
    }
    rename_package("off-fire", "fire"); rename_package("off-ember", "ember");
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* Global modifiers apply first-match, one row per mask: two packages that
 * change only the stock IMP modifier keep the first in stable source order,
 * the other's row reverts to stock, both stay served and the library accounts
 * the shadowed change. Reversed discovery keeps the same winner. */
static void test_registration_modifier_precedence(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024], winner[32] = "";
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    registration_root("registration-modifiers");
    registration_conductor = variant_stock;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    const char *names[] = {"tough", "frail"};
    for (size_t i = 0; i < 2; i++) {
        char path[256], text[640];
        snprintf(path, sizeof(path), "overrides/%s/package.json", names[i]);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", names[i], names[i]); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", names[i]);
        snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
            "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"T3\"; entityDef = \"t3\"; } } "
            "globalAIMods = { num = 2; item[0] = { aiType = \"T3\"; healthMod = \"stock-t3\"; } "
            "item[1] = { aiType = \"IMP\"; healthMod = \"%s\"; } } } }", names[i]); create(path, text);
    }
    for (int round = 0; round < 2; round++) {
        if (round) { rename_package("tough", "zz-tough"); rename_package("frail", "aa-frail"); }
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration modifiers %d: %s\n", round, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        const char *mod = conductor_list_row(conductor, "globalAIMods", "IMP", "healthMod");
        assert(mod && (!strcmp(mod, "tough") || !strcmp(mod, "frail")));
        if (!round) snprintf(winner, sizeof(winner), "%s", mod);
        else assert(!strcmp(winner, mod));
        char note[160];
        snprintf(note, sizeof(note), "its global AI modifier for IMP differs from %s's", winner);
        assert(library.coverage && strstr(library.coverage, note) && conductor_row(conductor, "IMP") &&
            !strcmp(conductor_row(conductor, "IMP"), "imp"));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    rename_package("zz-tough", "tough"); rename_package("aa-frail", "frail");
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* The spawn manager picks uniformly among every registration row sharing a
 * bit with a request (VK FUN_141123830), so row order is presentation: two
 * packages that move stock rows in opposing directions still compose, each
 * row once, with or without the private-key contract, and when a frozen
 * legacy context composes both copies whole-file. */
static void test_registration_row_order(void)
{
    static const char *order_stock = "{ class = \"Entity\"; edit = { aiTypeList = { num = 3; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"T3\"; entityDef = \"t3\"; } "
        "item[2] = { aiType = \"T4\"; entityDef = \"imp\"; } } } }";
    static const char *orders[] = {"3i4", "i43"};
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    registration_root("registration-row-order");
    registration_conductor = order_stock;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    for (size_t i = 0; i < 2; i++) {
        char path[256], text[640], items[400] = "";
        const char *name = i ? "second" : "first";
        for (size_t k = 0; k < 3; k++) {
            size_t used = strlen(items);
            const char *row = orders[i][k] == 'i' ? "{ aiType = \"IMP\"; entityDef = \"imp\"; }" :
                orders[i][k] == '3' ? "{ aiType = \"T3\"; entityDef = \"t3\"; }" : "{ aiType = \"T4\"; entityDef = \"imp\"; }";
            snprintf(items + used, sizeof(items) - used, "item[%zu] = %s ", k, row);
        }
        snprintf(path, sizeof(path), "overrides/%s/package.json", name);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", name, name); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", name);
        snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 3; %s} } }", items);
        create(path, text);
    }
    for (int mode = 0; mode < 2; mode++) {
        sh_package_library library = {0}; sh_package_library_native native = {0};
        char rows[8][96];
        native.private_keys = 1;
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, mode ? &native : NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration row order %d: %s\n", mode, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        size_t count = conductor ? conductor_rows(conductor, "aiTypeList", "entityDef", rows, 8) : 0;
        assert(count == 3 && row_index(rows, count, "IMP=imp") != SIZE_MAX &&
            row_index(rows, count, "T3=t3") != SIZE_MAX && row_index(rows, count, "T4=imp") != SIZE_MAX);
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    {
        /* One frozen legacy context holding both packages composes their
         * conductor copies whole-file under the same row-order rule. */
        sh_package_library legacy = {0}; sh_package_provenance contexts = {0};
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error));
        unsigned char trees[2][32];
        char rows[8][96];
        assert(sources && sources->package_count == 2);
        for (size_t i = 0; i < 2; i++) memcpy(trees[i], sh_package_source_tree(sources, i), 32);
        assert(sh_package_provenance_context_add(&contexts, SH_PACKAGE_CONTEXT_COMPOSED,
            (const unsigned char (*)[32])trees, 2, NULL) != SIZE_MAX);
        int ok = sh_package_library_compile_contexts(sources, &contexts, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, NULL, &legacy, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration row order legacy: %s\n", error);
        assert(ok);
        const sh_compiled_resource *conductor = NULL;
        for (size_t i = 0; i < legacy.compiled->resource_count; i++) {
            const sh_compiled_resource *file = legacy.compiled->resources + i;
            if (!strcmp(file->original_path ? file->original_path : file->engine_path,
                    "generated/decls/entitydef/conductor.decl")) { assert(!conductor); conductor = file; }
        }
        size_t count = conductor ? conductor_rows(conductor, "aiTypeList", "entityDef", rows, 8) : 0;
        assert(count == 3 && row_index(rows, count, "IMP=imp") != SIZE_MAX &&
            row_index(rows, count, "T3=t3") != SIZE_MAX && row_index(rows, count, "T4=imp") != SIZE_MAX);
        sh_package_provenance_free(&contexts);
        sh_package_library_free(&legacy); sh_package_sources_free(sources);
    }
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* The installed library's real shape: packages ship the complete stock lists
 * with their own rows last, a package installed from a legacy map ships only
 * IMP and its own rows, and one changes the IMP row. The stock rows a partial
 * list omits are restored in stock order ahead of its own rows, so its
 * first-match modifier row agrees with the same row in a complete list. Under
 * the private-key contract all of them compose: every stock row once, in
 * stock order among the modifiers, and every package row once. */
static void test_registration_mixed_lists(void)
{
    static const char *mixed_stock = "{ class = \"Entity\"; edit = { aiTypeList = { num = 5; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"T3\"; entityDef = \"t3\"; } "
        "item[2] = { aiType = \"T4\"; entityDef = \"imp\"; } item[3] = { aiType = \"T5\"; entityDef = \"t3\"; } "
        "item[4] = { aiType = \"T6\"; entityDef = \"imp\"; } } globalAIMods = { num = 3; "
        "item[0] = { aiType = \"IMP\"; healthMod = \"stock-imp\"; } item[1] = { aiType = \"T3\"; healthMod = \"stock-t3\"; } "
        "item[2] = { aiType = \"T4\"; healthMod = \"stock-t4\"; } } } }";
    static const char *full_rows = "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } "
        "item[1] = { aiType = \"T3\"; entityDef = \"t3\"; } item[2] = { aiType = \"T4\"; entityDef = \"imp\"; } "
        "item[3] = { aiType = \"T5\"; entityDef = \"t3\"; } item[4] = { aiType = \"T6\"; entityDef = \"imp\"; } ";
    static const char *full_mods = "item[0] = { aiType = \"IMP\"; healthMod = \"stock-imp\"; } "
        "item[1] = { aiType = \"T3\"; healthMod = \"stock-t3\"; } item[2] = { aiType = \"T4\"; healthMod = \"stock-t4\"; } ";
    static const char *const packages[][3] = {
        /* name, conductor rows, AI (its target) */
        {"cyber", "full", "cyber_boss"}, {"keys", "full", "keys_boss"},
        {"legacy", "partial", "cyber_boss"}, {"fire", "imp", "fire_imp"}
    };
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    registration_root("registration-mixed-lists");
    registration_conductor = mixed_stock;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    for (size_t i = 0; i < sizeof(packages) / sizeof(*packages); i++) {
        const char *name = packages[i][0], *shape = packages[i][1], *ai = packages[i][2];
        char path[256], text[2048];
        snprintf(path, sizeof(path), "overrides/%s/package.json", name);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", name, name); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", name);
        if (!strcmp(shape, "full"))
            snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 6; %s"
                "item[5] = { aiType = \"524288\"; entityDef = \"%s\"; } } globalAIMods = { num = 4; %s"
                "item[3] = { aiType = \"524288\"; healthMod = \"%s-mod\"; } } } }", full_rows, ai, full_mods, ai);
        else if (!strcmp(shape, "partial"))
            snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
                "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } "
                "item[1] = { aiType = \"524288\"; entityDef = \"%s\"; } } globalAIMods = { num = 2; "
                "item[0] = { aiType = \"IMP\"; healthMod = \"stock-imp\"; } "
                "item[1] = { aiType = \"524288\"; healthMod = \"%s-mod\"; } } } }", ai, ai);
        else
            snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 5; "
                "item[0] = { aiType = \"IMP\"; entityDef = \"%s\"; } %s} } }", ai, strstr(full_rows, "item[1]"));
        create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s.decl", name, ai);
        create(path, !strcmp(ai, "keys_boss") ? "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }" :
            "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    }
    sh_package_library library = {0}; sh_package_library_native native = {0};
    char rows[16][96], row[160];
    native.private_keys = 1;
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "registration mixed lists: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    /* The activation log breaks the contribution phase into its steps. */
    assert(strstr(library.contribution_phases, "keys ") && strstr(library.contribution_phases, "registration ") &&
        strstr(library.contribution_phases, "document "));
    const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
    size_t count = conductor ? conductor_rows(conductor, "aiTypeList", "entityDef", rows, 16) : 0;
    static const char *const stock_rows[] = {"IMP=imp", "T3=t3", "T4=imp", "T5=t3", "T6=imp"};
    for (size_t i = 0; i < 5; i++) {
        size_t at = row_index(rows, count, stock_rows[i]);
        assert(at != SIZE_MAX && row_index(rows + at + 1, count - at - 1, stock_rows[i]) == SIZE_MAX);
    }
    /* Each package AI registers once, whatever key it now holds. */
    for (size_t i = 0; i < sizeof(packages) / sizeof(*packages); i++) {
        const char *ai = authored_name(library.compiled, packages[i][2]);
        size_t seen = 0;
        assert(ai);
        snprintf(row, sizeof(row), "=%s", ai);
        for (size_t r = 0; r < count; r++) {
            const char *tail = strrchr(rows[r], '=');
            if (tail && !strcmp(tail, row)) seen++;
        }
        assert(seen == 1);
    }
    /* Fire replaces imp's first registration with fire_imp. Its remaining
     * imp therefore classifies as T4, not the stock IMP. Preserve those two
     * remaining rows under a separate binding even though the AI file itself
     * is unchanged; the other packages still share the native stock imp. */
    const char *fire_imp = scoped_entity_name(&library, sources, "fire", "imp");
    assert(fire_imp && strcmp(fire_imp, "imp"));
    size_t fire_rows = 0;
    for (size_t i = 0; i < count; i++) {
        const char *equal = strchr(rows[i], '=');
        if (!equal || strcmp(equal + 1, fire_imp)) continue;
        uint32_t mask = (uint32_t)strtoul(rows[i], NULL, 10);
        assert((mask & SH_PACKAGE_KEY_PRIVATE) && (mask & (4u | 16u | 64u)) == (fire_rows ? 64u : 16u));
        fire_rows++;
    }
    assert(count == 5 + 3 + 2 && fire_rows == 2);
    /* Modifiers apply first-match: the stock rows keep their stock order and
     * each package modifier appears once. */
    char mods[16][96];
    size_t mod_count = conductor_rows(conductor, "globalAIMods", "healthMod", mods, 16);
    size_t imp = row_index(mods, mod_count, "IMP=stock-imp"), t3 = row_index(mods, mod_count, "T3=stock-t3"),
        t4 = row_index(mods, mod_count, "T4=stock-t4");
    /* Classification changes retain their native modifier even when that
     * modifier's bytes match stock; it cannot fall through to a peer later. */
    if (mod_count != 7) for (size_t r = 0; r < mod_count; r++) fprintf(stderr, "mixed modifier %s\n", mods[r]);
    assert(mod_count == 7 && imp < t3 && t3 < t4 && t4 != SIZE_MAX);
    for (size_t i = 0; i < 2; i++) {
        size_t seen = 0;
        snprintf(row, sizeof(row), "=%s-mod", i ? "keys_boss" : "cyber_boss");
        for (size_t r = 0; r < mod_count; r++) {
            const char *tail = strrchr(mods[r], '=');
            if (tail && !strcmp(tail, row)) seen++;
        }
        assert(seen == 1);
    }
    sh_package_library_free(&library); sh_package_sources_free(sources);
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* A package whose conductor copy lists, after the stock IMP row, the rows of
 * one list: "aiType=value" pairs separated by '|' (modifiers take the value as
 * healthMod, registrations as entityDef). */
static void first_rows(const char *name, const char *list, const char *rows)
{
    int modifiers = !strcmp(list, "globalAIMods");
    const char *field = modifiers ? "healthMod" : "entityDef", *first = modifiers ? "stock-imp" : "imp";
    char path[256], text[1400], items[1200];
    size_t n = 1;
    snprintf(items, sizeof(items), "item[0] = { aiType = \"IMP\"; %s = \"%s\"; } ", field, first);
    for (const char *at = rows; *at;) {
        const char *bar = strchr(at, '|'), *eq = strchr(at, '=');
        size_t length = bar ? (size_t)(bar - at) : strlen(at), used = strlen(items);
        assert(eq && eq < at + length);
        snprintf(items + used, sizeof(items) - used, "item[%zu] = { aiType = \"%.*s\"; %s = \"%.*s\"; } ",
            n++, (int)(eq - at), at, field, (int)(at + length - eq - 1), eq + 1);
        at += length + (bar ? 1 : 0);
    }
    snprintf(path, sizeof(path), "overrides/%s/package.json", name);
    snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", name, name); create(path, text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", name);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { %s = { num = %zu; %s} } }", list, n, items);
    create(path, text);
}
static size_t first_package(const sh_package_sources *sources, const char *name)
{
    for (size_t i = 0; i < sources->package_count; i++)
        if (!strcmp(sources->packages[i].name, name)) return i;
    return SIZE_MAX;
}
typedef struct key_row { uint32_t mask; char field[96]; } key_row;
static size_t key_rows(fixture *, const sh_compiled_resource *, const char *, const char *, key_row *, size_t);
static const sh_package_key_groups *g_key_groups;
static const char *modifier_for(const key_row *, size_t, uint32_t);
static uint32_t scoped_modifier(fixture *f, const sh_package_library *library, const sh_package_sources *sources,
    const char *package, const char *original, const char *expected)
{
    const char *ai = scoped_entity_name(library, sources, package, original);
    const sh_compiled_resource *conductor = named_resource(library->compiled, "entitydef", "conductor");
    key_row rows[32], mods[32];
    if (!ai || !conductor) {
        fprintf(stderr, "modifier lookup %s/%s: ai=%s conductor=%p\n", package, original, ai ? ai : "(none)", (void *)conductor);
        for (size_t i = 0; i < library->compiled->resource_count; i++) {
            const sh_compiled_resource *r = library->compiled->resources + i;
            if (r->type && !strcmp(r->type, "entitydef")) fprintf(stderr, "%s original=%s\n", r->name,
                r->original_path ? r->original_path : r->engine_path);
        }
    }
    assert(ai && conductor);
    size_t count = key_rows(f, conductor, "aiTypeList", "entityDef", rows, 32);
    size_t mod_count = key_rows(f, conductor, "globalAIMods", "healthMod", mods, 32);
    g_key_groups = library->key_groups;
    for (size_t r = 0; r < count; r++) if (!strcmp(rows[r].field, ai)) {
        const char *actual = modifier_for(mods, mod_count, rows[r].mask);
        if (!actual || strcmp(actual, expected)) fprintf(stderr, "%s %s: expected %s, got %s\n",
            package, original, expected, actual ? actual : "no modifier");
        if (!actual || strcmp(actual, expected)) {
            fprintf(stderr, "selected %s type %u\n", ai, rows[r].mask);
            for (size_t m = 0; m < mod_count; m++) fprintf(stderr, "modifier %u=%s\n", mods[m].mask, mods[m].field);
            fprintf(stderr, "conductor %.*s\n", (int)conductor->body_length, conductor->body);
        }
        assert(actual && !strcmp(actual, expected));
        return rows[r].mask;
    }
    assert(!"scoped AI has no registration"); return 0;
}
/* Opposite modifier orders are equivalent until an AI's first classification
 * meets both rows. Then each package gets its own classification and native
 * first modifier, independently of discovery order. */
static void test_registration_modifier_order(void)
{
    static const char *separate = "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } } globalAIMods = { num = 1; "
        "item[0] = { aiType = \"IMP\"; healthMod = \"stock-imp\"; } } } }";
    static const char *together = "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"T5 T6\"; entityDef = \"t3\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = \"IMP\"; healthMod = \"stock-imp\"; } } } }";
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    char error[1024], kept[2][8][96];
    registration_root("registration-modifier-order");
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    native.private_keys = 1;
    first_rows("order-a", "globalAIMods", "T5=five|T6=six"); first_rows("order-b", "globalAIMods", "T6=six|T5=five");
    for (int round = 0; round < 4; round++) {
        sh_package_library library = {0};
        char mods[8][96];
        if (round == 2) rename_package("order-a", "zz-order-a");
        registration_conductor = round & 1 ? together : separate;
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
            (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "modifier order %d: %s\n", round, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        size_t count = conductor ? conductor_rows(conductor, "globalAIMods", "healthMod", mods, 8) : 0;
        size_t five = row_index(mods, count, "T5=five"), six = row_index(mods, count, "T6=six");
        assert(row_index(mods, count, "IMP=stock-imp") != SIZE_MAX);
        if (!(round & 1)) assert(five != SIZE_MAX && six != SIZE_MAX);
        const char *notes = library.coverage ? library.coverage : "";
        if (round & 1) {
            const char *a_package = round < 2 ? "order-a" : "zz-order-a";
            uint32_t a = scoped_modifier(f, &library, sources, a_package, "t3", "five");
            uint32_t b = scoped_modifier(f, &library, sources, "order-b", "t3", "six");
            assert(count == 5 && a != b);
            assert(strcmp(scoped_entity_name(&library, sources, a_package, "t3"),
                scoped_entity_name(&library, sources, "order-b", "t3")) != 0);
        } else assert(count == 3);
        if (!(round & 1)) assert(!strstr(notes, "global AI modifier"));
        if (round < 2) memcpy(kept[round & 1], mods, sizeof(mods));
        else for (size_t r = 0; r < count; r++) assert(!strcmp(kept[round & 1][r], mods[r]));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    rename_package("zz-order-a", "order-a");
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* Modifier winners beside every other kind of package. The stock conductor
 * registers IMP and T5 AI and has an IMP and a T5 modifier. clash-a and
 * clash-b each list a new row meeting T5 ahead of the stock T5 row, so each
 * changes which row a T5 AI takes: each gets a distinct classification with
 * its own first modifier. Shared native rows still use stable ordering. shade-a and shade-b
 * order two rows meeting T5 oppositely behind the stock T5 row, which wins
 * either way, and calm-a and calm-b order rows no registered type meets: both
 * pairs compose silently. Three packages register private AI. A malformed
 * package is rejected on its own evidence; nothing else is excluded, the
 * candidate is not refused, and discovery order changes nothing. */
static void test_registration_first_match(void)
{
    static const char *first_stock = "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"T5\"; entityDef = \"t3\"; } } "
        "globalAIMods = { num = 2; item[0] = { aiType = \"IMP\"; healthMod = \"stock-imp\"; } "
        "item[1] = { aiType = \"T5\"; healthMod = \"stock-five\"; } } } }";
    static const char *const peers[] = {"peer-1", "peer-2", "peer-3"}, *const targets[] = {"local-only",
        "parent-only", "modifier-child"};
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    char path[256], text[1024], error[1024], kept[16][96];
    size_t kept_count = 0;
    registration_root("registration-first-match");
    registration_conductor = first_stock;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    native.private_keys = 1;
    first_rows("clash-a", "globalAIMods", "T5 T6=six|T5=stock-five");
    first_rows("clash-b", "globalAIMods", "T5 T7=seven|T5=stock-five");
    first_rows("shade-a", "globalAIMods", "T5=stock-five|T5 T9=nine|T5 T10=ten");
    first_rows("shade-b", "globalAIMods", "T5=stock-five|T5 T10=ten|T5 T9=nine");
    first_rows("calm-a", "globalAIMods", "T11=eleven|T12=twelve");
    first_rows("calm-b", "globalAIMods", "T12=twelve|T11=eleven");
    for (size_t i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "overrides/%s/package.json", peers[i]);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", peers[i], peers[i]); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", peers[i]);
        snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
            "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"524288\"; entityDef = \"peer_%zu\"; } } } }",
            i + 1); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/peer_%zu.decl", peers[i], i + 1);
        snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { target = \"%s\"; } }", targets[i]); create(path, text);
    }
    create("overrides/broken/package.json", "{\"id\":\"local.broken\",\"name\":\"broken\"}");
    create("overrides/broken/assets/generated/decls/entitydef/conductor.decl",
        "{ class = \"Entity\"; edit = { globalAIMods = { num = 1; }");
    for (int round = 0; round < 2; round++) {
        sh_package_library library = {0};
        char rows[16][96], mods[16][96], leaf[16], row[160];
        if (round) { rename_package("clash-a", "zz-clash-a"); rename_package("clash-b", "zy-clash-b"); }
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
            (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (ok || invalid != first_package(sources, "broken"))
            fprintf(stderr, "malformed beside first-match packages: ok=%d invalid=%zu %s\n", ok, invalid, error);
        assert(!ok && invalid == first_package(sources, "broken") && invalid != SIZE_MAX && unserved == SIZE_MAX);
        sh_package_library_free(&library);
        assert(sh_package_sources_remove(sources, invalid));
        ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
            (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "first-match packages: %s\n", error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        size_t count = conductor ? conductor_rows(conductor, "aiTypeList", "entityDef", rows, 16) : 0;
        size_t mod_count = conductor ? conductor_rows(conductor, "globalAIMods", "healthMod", mods, 16) : 0;
        for (size_t i = 0; i < 3; i++) {
            snprintf(leaf, sizeof(leaf), "peer_%zu", i + 1);
            const char *ai = authored_name(library.compiled, leaf);
            size_t seen = 0;
            assert(ai);
            snprintf(row, sizeof(row), "=%s", ai);
            for (size_t r = 0; r < count; r++) {
                const char *tail = strrchr(rows[r], '=');
                if (tail && !strcmp(tail, row)) seen++;
            }
            assert(seen == 1);
        }
        const char *notes = library.coverage ? library.coverage : "";
        size_t stock = row_index(mods, mod_count, "T5=stock-five");
        assert(!strstr(notes, "first global AI modifier"));
        assert(!strstr(notes, "shade-") && !strstr(notes, "calm-"));
        uint32_t a_type = scoped_modifier(f, &library, sources, round ? "zz-clash-a" : "clash-a", "t3", "six");
        uint32_t b_type = scoped_modifier(f, &library, sources, round ? "zy-clash-b" : "clash-b", "t3", "seven");
        if (count != 7 || mod_count != 10) for (size_t r = 0; r < mod_count; r++) fprintf(stderr, "first-match row %s\n", mods[r]);
        assert(count == 7 && mod_count == 10 && a_type != b_type && row_index(mods, mod_count, "IMP=stock-imp") != SIZE_MAX);
        key_row native_mods[16];
        size_t native_count = key_rows(f, conductor, "globalAIMods", "healthMod", native_mods, 16);
        assert(!strcmp(modifier_for(native_mods, native_count, 32u), "stock-five"));
        assert(stock < row_index(mods, mod_count, "T5 T9=nine") &&
            stock < row_index(mods, mod_count, "T5 T10=ten") && row_index(mods, mod_count, "T11=eleven") != SIZE_MAX &&
            row_index(mods, mod_count, "T12=twelve") != SIZE_MAX);
        if (!round) { memcpy(kept, mods, sizeof(mods)); kept_count = mod_count; }
        else for (size_t r = 0; r < kept_count; r++) assert(!strcmp(kept[r], mods[r]));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    rename_package("zz-clash-a", "clash-a"); rename_package("zy-clash-b", "clash-b");
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* The fixture enum's bits of a registration row's aiType ("T5", "IMP" or a
 * number, as relocation writes a package's own key beside its named bits). */
static unsigned long first_bits(const char *row)
{
    const char *eq = strrchr(row, '='), *end = eq ? eq : row + strlen(row);
    unsigned long bits = 0;
    for (const char *at = row; at < end;) {
        const char *stop;
        while (at < end && *at == ' ') at++;
        for (stop = at; stop < end && *stop != ' '; stop++) {}
        if (stop > at) bits |= *at >= '0' && *at <= '9' ? strtoul(at, NULL, 10) :
            stop - at == 3 && !strncmp(at, "IMP", 3) ? 4ul : *at == 'T' ? 1ul << atoi(at + 1) : 0ul;
        at = stop;
    }
    return bits;
}
/* A definition's first registration types its AI. The stock conductor
 * registers t3 under T5. reg-a lists t3 under T6 ahead of it, so its t3 AI
 * are typed T6; reg-b adds T7 behind the stock row and changes nothing. Three
 * packages list imp under T8, T9 and T10 behind the stock IMP row in a cyclic
 * presentation order, so the composer drops layout preferences: each
 * definition's first registration must survive that. Then reg-c lists T7 first
 * as well: reg-a and reg-c get distinct t3 definitions, each preserving its
 * own first type. Relocation gives each package's
 * own rows a private key beside their named bits, so types compare by the
 * named bit. */
static void test_registration_first_registration(void)
{
    static const char *types_stock = "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"T5\"; entityDef = \"t3\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = \"IMP\"; healthMod = \"stock-imp\"; } } } }";
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    char error[1024];
    registration_root("registration-first-registration");
    registration_conductor = types_stock;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    native.private_keys = 1;
    first_rows("reg-a", "aiTypeList", "T6=t3|T5=t3"); first_rows("reg-b", "aiTypeList", "T5=t3|T7=t3");
    first_rows("cyc-a", "aiTypeList", "T8=imp|T9=imp"); first_rows("cyc-b", "aiTypeList", "T9=imp|T10=imp");
    first_rows("cyc-c", "aiTypeList", "T10=imp|T8=imp");
    for (int round = 0; round < 2; round++) {
        sh_package_library library = {0};
        char rows[16][96];
        if (round) first_rows("reg-c", "aiTypeList", "T7=t3|T5=t3");
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
            (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "first registration %d: %s\n", round, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        size_t count = conductor ? conductor_rows(conductor, "aiTypeList", "entityDef", rows, 16) : 0;
        size_t first_t3 = SIZE_MAX, first_imp = SIZE_MAX;
        for (size_t r = count; r > 0; r--) {
            const char *tail = strrchr(rows[r - 1], '=');
            if (tail && !strcmp(tail, "=t3")) first_t3 = r - 1;
            if (tail && !strcmp(tail, "=imp")) first_imp = r - 1;
        }
        const char *notes = library.coverage ? library.coverage : "";
        assert(first_imp != SIZE_MAX && !strcmp(rows[first_imp], "IMP=imp") && first_t3 != SIZE_MAX);
        assert((first_bits(rows[first_t3]) & 0x1ff) == 32);
        const char *a = scoped_entity_name(&library, sources, "reg-a", "t3");
        const char *c = round ? scoped_entity_name(&library, sources, "reg-c", "t3") : NULL;
        assert(a && strcmp(a, "t3") && (!round || (c && strcmp(c, "t3") && strcmp(a, c))));
        for (size_t p = 0; p < (round ? 2u : 1u); p++) {
            const char *name = p ? c : a;
            size_t first = SIZE_MAX, seen = 0;
            for (size_t r = 0; r < count; r++) {
                const char *tail = strrchr(rows[r], '=');
                if (tail && !strcmp(tail + 1, name)) { if (!seen) first = r; seen++; }
            }
            assert(seen == 2 && (first_bits(rows[first]) & 0x1ff) == (p ? 128u : 64u));
        }
        assert(!strstr(notes, "first registration"));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* A whitelist is membership, not behavioral order. Opposing orders of the
 * same encounters compose with three peers into one deduplicated union, and
 * every healthy owner remains represented. A malformed peer is rejected on
 * its own evidence; removing that peer leaves the whole healthy union. */
static void test_editor_membership_order(fixture *f, sh_decl_registry_source registry)
{
    static const char *const names[] = {"clash-a", "clash-b", "peer-1", "peer-2", "peer-3"};
    static const char *const lists[] = {"item[0] = \"parent-only\"; item[1] = \"modifier-child\";",
        "item[0] = \"modifier-child\"; item[1] = \"parent-only\";", "item[0] = \"local-only\";",
        "item[0] = \"local-only\";", "item[0] = \"local-only\";"};
    char temporary[MAX_PATH], path[256], text[512], error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library library = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-editor-joint-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    env.baseline = contribution_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    for (size_t i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "overrides/%s/package.json", names[i]);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", names[i], names[i]); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/snappropertyinspector_whitelistencounterdecl/list.decl",
            names[i]);
        snprintf(text, sizeof(text), "{ edit = { mode = \"shared\"; validEncounters = { num = %d; %s } } }",
            i < 2 ? 2 : 1, lists[i]); create(path, text);
    }
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals, registry,
        (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "editor membership union: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    const sh_compiled_resource *list = named_resource(library.compiled,
        "snappropertyinspector_whitelistencounterdecl", "list");
    assert(list && sh_package_owners_count(&list->owners) == 5 &&
        !sh_package_owners_count(&list->gameplay_owners));
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)list->body, list->body_length}, NULL, 0);
    const sh_decl_node *encounters = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "validEncounters");
    assert(encounters && !strcmp(node_value(sh_decl_tree_member(encounters, "num")), "3"));
    assert(strstr((char *)list->body, "parent-only") && strstr((char *)list->body, "modifier-child") &&
        strstr((char *)list->body, "local-only"));
    sh_decl_tree_free(tree);
    sh_package_library_free(&library);
    sh_package_sources_free(sources);
    create("overrides/broken/package.json", "{\"id\":\"local.broken\",\"name\":\"broken\"}");
    create("overrides/broken/assets/generated/decls/snappropertyinspector_whitelistencounterdecl/list.decl",
        "{ edit = { mode = \"shared\"; validEncounters = { num = 1; }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals, registry,
        (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (ok || invalid != first_package(sources, "broken"))
        fprintf(stderr, "malformed beside editor membership union: ok=%d invalid=%zu %s\n", ok, invalid, error);
    assert(!ok && invalid == first_package(sources, "broken") && invalid != SIZE_MAX && unserved == SIZE_MAX);
    sh_package_library_free(&library);
    assert(sh_package_sources_remove(sources, invalid));
    ok = sh_package_library_compile(sources, &env, NULL, contribution_original, &originals, registry,
        (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    list = named_resource(library.compiled,
        "snappropertyinspector_whitelistencounterdecl", "list");
    assert(list && sh_package_owners_count(&list->owners) == 5);
    tree = sh_decl_tree_parse((sh_decl_source){(char *)list->body, list->body_length}, NULL, 0);
    encounters = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "validEncounters");
    assert(encounters && !strcmp(node_value(sh_decl_tree_member(encounters, "num")), "3"));
    sh_decl_tree_free(tree);
    sh_package_library_free(&library); sh_package_sources_free(sources);
    cleanup(); assert(!failures);
}
/* Thirteen packages register distinct AI under one authored key. The engine
 * dispatches 32-bit keys, so eleven unnamed bits give eleven exclusive keys:
 * the first keeps it, ten move, and the last two keep the authored key and
 * share its requests (the spawn manager picks among the rows). All stay
 * served with their AI registered; the shared keys and the modifier rows they
 * shadow are accounted. */
static void test_registration_capacity(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024], names[13][16];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    registration_root("registration-capacity");
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    for (int i = 0; i < 13; i++) { snprintf(names[i], sizeof(names[i]), "cap%02d", i); registration_package(names[i], "524288"); }
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, NULL, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "registration capacity: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
    char rows[32][96], mods[32][96], row[160];
    size_t count = conductor_rows(conductor, "aiTypeList", "entityDef", rows, 32);
    size_t mod_count = conductor_rows(conductor, "globalAIMods", "healthMod", mods, 32);
    uint32_t keys = 0;
    size_t shared = 0;
    for (int i = 0; i < 13; i++) {
        const char *key = spawner_key(library.compiled, names[i]);
        unsigned long value = strtoul(key, NULL, 10);
        assert(value && !(value & (value - 1)) && value >= 524288u && value <= 536870912u);
        keys |= (uint32_t)value;
        if (value == 524288u) shared++;
        /* Every package's AI stays registered under the key it holds. */
        snprintf(row, sizeof(row), "%s=%s", key, package_boss(library.compiled, names[i]));
        assert(row_index(rows, count, row) != SIZE_MAX);
    }
    assert(keys == 0x3ff80000u && shared == 3 && count == 14);
    /* One modifier row per mask; the keeper's holds the shared key. */
    size_t at_shared = 0;
    for (size_t i = 0; i < mod_count; i++) if (!strncmp(mods[i], "524288=", 7)) at_shared++;
    assert(mod_count == 11 && at_shared == 1);
    size_t notes = 0, shadowed = 0;
    for (const char *at = library.coverage; at && (at = strstr(at, "shares its spawn requests")); at++) notes++;
    for (const char *at = library.coverage; at && (at = strstr(at, "global AI modifier for 524288")); at++) shadowed++;
    assert(notes == 2 && shadowed == 2);
    {
        const char *with = strstr(library.coverage, "shares its spawn requests with ");
        char keeper[32], expected[64];
        assert(with); with += strlen("shares its spawn requests with ");
        size_t n = strcspn(with, ";");
        assert(n < sizeof(keeper)); memcpy(keeper, with, n); keeper[n] = 0;
        snprintf(expected, sizeof(expected), "524288=%s", keeper);
        assert(row_index(mods, mod_count, expected) != SIZE_MAX && !strcmp(spawner_key(library.compiled, keeper), "524288"));
    }
    sh_package_library_free(&library); sh_package_sources_free(sources);
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}

/* The spawn-type bits of every row of one conductor list, with its field. */
static size_t key_rows(fixture *f, const sh_compiled_resource *conductor, const char *list_name, const char *field,
    key_row *rows, size_t capacity)
{
    char text[32][96], error[512];
    size_t count = conductor_rows(conductor, list_name, field, text, 32);
    sh_decl_native_schema *schema = sh_decl_native_schema_open(
        (sh_decl_native_source){f, read_memory, (uintptr_t)f->reflection}, (sh_decl_dependency_observer){0},
        error, sizeof(error));
    sh_package_key_space space;
    assert(schema && sh_package_key_space_read(schema, &space) == 1 && count <= capacity);
    for (size_t i = 0; i < count; i++) {
        const char *equals = strchr(text[i], '=');
        assert(equals && sh_package_key_bits(&space, text[i], (size_t)(equals - text[i]), &rows[i].mask));
        snprintf(rows[i].field, sizeof(rows[i].field), "%s", equals + 1);
    }
    sh_decl_native_schema_close(schema);
    return count;
}
/* The group codes of the library under test (package_keys.h). */
/* The AI a request can select under the native private-key contract. */
static size_t selected(const key_row *rows, size_t count, uint32_t request, const char *out[], size_t capacity)
{
    size_t n = 0;
    for (size_t i = 0; i < count; i++)
        if (sh_package_key_request_matches(g_key_groups, request, rows[i].mask)) { assert(n < capacity); out[n++] = rows[i].field; }
    return n;
}
static int holds(const char *const *list, size_t count, const char *value)
{
    for (size_t i = 0; i < count; i++) if (value && !strcmp(list[i], value)) return 1;
    return 0;
}
/* The modifier the native lookup gives an AI typed by its first row. */
static const char *modifier_for(const key_row *mods, size_t count, uint32_t type)
{
    for (size_t i = 0; i < count; i++) if (sh_package_key_modifier_matches(g_key_groups, type, mods[i].mask)) return mods[i].field;
    return NULL;
}
/* Under the native private-key contract every package's changed stock row
 * takes its own key, contended or not: the stock rows stay (restored where a
 * package's copy replaced them), so stock IMP requests select only the stock
 * imp, as native-only maps need; each variant row keeps the stock bits ("IMP
 * ANY_ZOMBIE <key>"), so type consumers still see an imp and a zombie; each
 * package's own requests reach its variant beside the stock rows its other
 * bits name, never another package's; a changed modifier moves to the key
 * ahead of the stock row. Reversed discovery gives each package the same key. */
static void test_registration_private_variants(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024], fire_key[32] = "", ice_key[32] = "";
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    sh_package_library_native native = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    native.private_keys = 1;
    registration_root("registration-private");
    registration_conductor = variant_stock;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    variant_package("fire", "fire_imp", "fire"); variant_package("ember", "fire_imp", "fire");
    variant_package("ice", "ice_imp", "stock");
    create("overrides/solo/package.json", "{\"id\":\"local.solo\",\"name\":\"solo\"}");
    create("overrides/solo/assets/generated/decls/entitydef/conductor.decl", "{ class = \"Entity\"; edit = { "
        "aiTypeList = { num = 2; item[0] = { aiType = \"IMP ANY_ZOMBIE\"; entityDef = \"imp\"; } "
        "item[1] = { aiType = \"T3 ANY_ZOMBIE\"; entityDef = \"solo_t3\"; } } } }");
    create("overrides/solo/assets/generated/decls/entitydef/solo_t3.decl", "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    create("overrides/solo/assets/generated/decls/entitydef/solo_spawner.decl", "{ class = \"Entity\"; edit = { spawnType = \"T3\"; } }");
    for (int round = 0; round < 2; round++) {
        if (round) { rename_package("fire", "zz-fire"); rename_package("ice", "aa-ice"); }
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration private %d: %s\n", round, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        /* Relocation reuses each class's scanned key sites; the same library without. */
        assert(library_key_memo_same(&library, sources, &env, registration_original, &originals,
            registry, (uintptr_t)f->reflection, &native) > 0);
        g_key_groups = library.key_groups;
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        key_row rows[16], mods[16];
        char fire[64], ember[64], ice[64], row[160];
        assert(conductor);
        size_t count = key_rows(f, conductor, "aiTypeList", "entityDef", rows, 16);
        size_t mod_count = key_rows(f, conductor, "globalAIMods", "healthMod", mods, 16);
        snprintf(fire, sizeof(fire), "%s", spawner_value(library.compiled, "fire"));
        snprintf(ember, sizeof(ember), "%s", spawner_value(library.compiled, "ember"));
        snprintf(ice, sizeof(ice), "%s", spawner_value(library.compiled, "ice"));
        char fire_now[32], ice_now[32];
        snprintf(fire_now, sizeof(fire_now), "%lu", strtoul(fire, NULL, 10) & ~(8ul | 131072ul));
        snprintf(ice_now, sizeof(ice_now), "%lu", strtoul(ice, NULL, 10) & ~(8ul | 131072ul));
        if (round) assert(!strcmp(fire_now, fire_key) && !strcmp(ice_now, ice_key));
        else {
            snprintf(fire_key, sizeof(fire_key), "%s", fire_now); snprintf(ice_key, sizeof(ice_key), "%s", ice_now);
            assert(strcmp(fire_key, ice_key) && (!strcmp(fire_key, "524288") || !strcmp(fire_key, "1048576")) &&
                (!strcmp(ice_key, "524288") || !strcmp(ice_key, "1048576")));
        }
        snprintf(row, sizeof(row), "%lu", strtoul(fire_key, NULL, 10) | 8ul | 131072ul);
        assert(!strcmp(fire, row) && !strcmp(ember, row));
        snprintf(row, sizeof(row), "%lu", strtoul(ice_key, NULL, 10) | 8ul | 131072ul); assert(!strcmp(ice, row));
        assert(!strcmp(spawner_value(library.compiled, "solo"), "2097152"));
        /* A package's own group request includes its variant's key. */
        snprintf(row, sizeof(row), "%lu", 131072ul | strtoul(fire_key, NULL, 10));
        assert(!strcmp(spawner_value(library.compiled, "fire_group"), row));
        const char *fire_ai = authored_name(library.compiled, "fire_imp"), *ice_ai = authored_name(library.compiled, "ice_imp");
        const char *solo_ai = authored_name(library.compiled, "solo_t3");
        const char *imp = authored_name(library.compiled, "imp"), *t3 = authored_name(library.compiled, "t3");
        assert(fire_ai && ice_ai && solo_ai && count == 5);
        const char *out[16];
        /* Stock requests: the stock AI alone. */
        size_t n = selected(rows, count, 4u, out, 16);
        assert(n == 1 && (!imp ? !strcmp(out[0], "imp") : !strcmp(out[0], imp)));
        n = selected(rows, count, 131072u, out, 16);
        assert(n == 2 && !holds(out, n, fire_ai) && !holds(out, n, ice_ai) && !holds(out, n, solo_ai));
        n = selected(rows, count, 0xffffffffu, out, 16);
        assert(n == 2);
        (void)t3;
        /* A package's own requests: its variant beside the stock rows its
         * other bits name, never another package's variant. */
        uint32_t fire_request = (uint32_t)strtoul(fire_key, NULL, 10) | 8u | 131072u;
        n = selected(rows, count, fire_request, out, 16);
        assert(n == 3 && holds(out, n, fire_ai) && !holds(out, n, ice_ai) && !holds(out, n, solo_ai));
        n = selected(rows, count, 2097152u, out, 16);
        assert(n == 1 && !strcmp(out[0], solo_ai));
        n = selected(rows, count, 131072u | (uint32_t)strtoul(fire_key, NULL, 10), out, 16);
        assert(n == 3 && holds(out, n, fire_ai) && !holds(out, n, ice_ai));
        /* Type consumers: each variant stays an imp; fire's changed modifier
         * reaches only fire's variant, the stock one every other imp. */
        uint32_t fire_type = 0, ice_type = 0;
        for (size_t i = 0; i < count; i++) {
            if (!strcmp(rows[i].field, fire_ai)) fire_type = rows[i].mask;
            if (!strcmp(rows[i].field, ice_ai)) ice_type = rows[i].mask;
        }
        assert((fire_type & 4u) && (ice_type & 4u) && (fire_type & 131072u));
        assert(mod_count == 5 && !strcmp(modifier_for(mods, mod_count, fire_type), "fire"));
        assert(!strcmp(modifier_for(mods, mod_count, ice_type), "stock") && !strcmp(modifier_for(mods, mod_count, 4u | 131072u), "stock"));
        /* A filter choice classifies: IMP stays every imp, variants included. */
        const char *choice = inspector_key(library.compiled, "fire");
        assert(choice && !strcmp(choice, "IMP") && sh_package_key_filter_matches(g_key_groups, 4u, fire_type));
        assert(!strcmp(filter_value(library.compiled, "fire"), "IMP ANY_ZOMBIE"));
        /* Maps bind each changing package's IMP bit to its key. */
        assert(key_binding(&library.bindings, "4", fire_key) == 2 && key_binding(&library.bindings, "4", ice_key) == 1);
        assert(key_binding(&library.bindings, "8", "2097152") == 1);
        {
            char grouped[32];
            snprintf(grouped, sizeof(grouped), "%u", 131072u | (uint32_t)strtoul(fire_key, NULL, 10));
            assert(key_binding(&library.bindings, "131072", grouped) == 2);
        }
        assert(!library.coverage || !strstr(library.coverage, "spawn"));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    rename_package("zz-fire", "fire"); rename_package("aa-ice", "ice");
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* A package's copy of the stock AI filter inspector: the stock choices in
 * the given order (a permutation of IMP, T3, T4), one for its key and one for
 * the named T5 the stock list lacks, under the package's own label. */
static void stock_inspector_package(const char *package, const char *key, const char *order)
{
    char path[512], text[1024], items[640] = "";
    for (size_t i = 0; i < 3; i++) {
        const char *choice = order[i] == 'i' ? "{ enumValue = \"IMP\"; label = \"imp\"; }" :
            order[i] == '3' ? "{ enumValue = \"T3\"; label = \"t3\"; }" : "{ enumValue = \"T4\"; label = \"t4\"; }";
        size_t used = strlen(items);
        snprintf(items + used, sizeof(items) - used, "item[%zu] = %s ", i, choice);
    }
    snprintf(path, sizeof(path),
        "overrides/%s/assets/generated/decls/snappropertyinspector_bitflag/spawnaitypebitflaginspector.decl", package);
    snprintf(text, sizeof(text), "{ edit = { enumType = \"spawnAIType_t\"; values = { num = 5; %s"
        "item[3] = { enumValue = \"%s\"; label = \"%s\"; } item[4] = { enumValue = \"T5\"; label = \"%s-t5\"; } } } }",
        items, key, package, package); create(path, text);
}
/* The choices a served inspector offers, in order: "label=enumValue". */
static size_t served_choices(const sh_compiled_resource *inspector, char rows[][96], size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)inspector->body, inspector->body_length}, NULL, 0);
    const sh_decl_node *values = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "values");
    size_t count = 0;
    char num[16];
    assert(values);
    for (const sh_decl_node *item = values->children; item; item = item->next) {
        if (!item->compound) continue;
        char label[48];
        assert(count < capacity);
        snprintf(label, sizeof(label), "%s", node_value(sh_decl_tree_member(item, "label")));
        snprintf(rows[count++], 96, "%s=%s", label, node_value(sh_decl_tree_member(item, "enumValue")));
    }
    snprintf(num, sizeof(num), "%zu", count);
    assert(!strcmp(node_value(sh_decl_tree_member(values, "num")), num));
    sh_decl_tree_free(tree); return count;
}
/* Packages add choices to the stock AI filter by shipping its stock-named
 * inspector. The choices are one shared list keyed by value: the stock-named
 * inspector, which the stock filter's property binds, is served with every
 * package's choice at the key its AI holds after relocation (a decimal the
 * native inspector needs key_inspector.c to offer) and each copied stock
 * choice once. A private copy nothing binds would hide them. A value is one
 * choice whatever its labels (both packages add T5), and positions are
 * presentation: opposing moves of a stock choice do not refuse the list. */
static void test_registration_stock_inspector(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    registration_root("registration-stock-inspector");
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    registration_package("alpha", "524288"); registration_package("gamma", "524288");
    stock_inspector_package("alpha", "524288", "3i4"); stock_inspector_package("gamma", "524288", "i43");
    for (int mode = 0; mode < 2; mode++) {
        sh_package_library library = {0}; sh_package_library_native native = {0};
        char rows[8][96], expected[96], alpha[32], gamma[32];
        native.private_keys = 1;
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, mode ? &native : NULL, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "stock inspector %d: %s\n", mode, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        library_key_memo_same(&library, sources, &env, registration_original, &originals,
            registry, (uintptr_t)f->reflection, mode ? &native : NULL);
        const sh_compiled_resource *inspector = named_resource(library.compiled, "snappropertyinspector_bitflag",
            "spawnaitypebitflaginspector");
        size_t served = 0;
        for (size_t i = 0; i < library.compiled->resource_count; i++) {
            const sh_compiled_resource *file = library.compiled->resources + i;
            if (!strcmp(file->original_path ? file->original_path : file->engine_path,
                    "generated/decls/snappropertyinspector_bitflag/spawnaitypebitflaginspector.decl")) served++;
        }
        if (!inspector || served != 1) for (size_t i = 0; i < library.compiled->resource_count; i++)
            fprintf(stderr, "  %s %s\n", library.compiled->resources[i].engine_path,
                library.compiled->resources[i].original_path ? library.compiled->resources[i].original_path : "");
        assert(inspector && served == 1 && !strcmp(inspector->engine_path,
            "generated/decls/snappropertyinspector_bitflag/spawnaitypebitflaginspector.decl"));
        size_t count = served_choices(inspector, rows, 8);
        snprintf(alpha, sizeof(alpha), "%s", inspector_key(library.compiled, "alpha"));
        snprintf(gamma, sizeof(gamma), "%s", inspector_key(library.compiled, "gamma"));
        if (count != 6) for (size_t i = 0; i < count; i++) fprintf(stderr, "  choice %s\n", rows[i]);
        assert(count == 6 && strcmp(alpha, gamma) && row_index(rows, count, "imp=IMP") != SIZE_MAX &&
            row_index(rows, count, "t3=T3") != SIZE_MAX && row_index(rows, count, "t4=T4") != SIZE_MAX &&
            (row_index(rows, count, "alpha-t5=T5") != SIZE_MAX) != (row_index(rows, count, "gamma-t5=T5") != SIZE_MAX));
        /* Each package's choice is the key its own inspector and spawner hold. */
        snprintf(expected, sizeof(expected), "alpha=%s", alpha); assert(row_index(rows, count, expected) != SIZE_MAX);
        snprintf(expected, sizeof(expected), "gamma=%s", gamma); assert(row_index(rows, count, expected) != SIZE_MAX);
        assert(!strcmp(spawner_key(library.compiled, "alpha"), alpha) &&
            !strcmp(spawner_key(library.compiled, "gamma"), gamma));
        if (!mode) assert((!strcmp(alpha, "524288") && !strcmp(gamma, "1048576")) ||
            (!strcmp(alpha, "1048576") && !strcmp(gamma, "524288")));
        else assert(sh_package_key_private_valid((uint32_t)strtoul(alpha, NULL, 10)) &&
            sh_package_key_private_valid((uint32_t)strtoul(gamma, NULL, 10)));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* Under the native private-key contract thirteen distinct registrations of
 * one authored key each get their own: nine single-bit keys, then marked
 * multi-bit keys. Every package's own request selects exactly its AI beside
 * the stock row it names, its modifier row stays its own, and nothing shares. */
static int registration_class_derives(void *context, const char *child, const char *parent)
{ return sh_decl_native_schema_class_derives(context, child, parent); }
static int discovered_registration_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/entitydef/conductor.decl") ?
        "{ inherit = \"conductor-parent\"; edit = { globalAIMods = { item[1] = { damageMod = \"2\"; } } } }" :
        !strcmp(path, "generated/decls/entitydef/conductor-parent.decl") ?
        "{ class = \"Entity\"; edit = { aiTypeList = { num = 3; item[1] = { aiType = \"524288\"; entityDef = \"inherited-ai\"; } } "
        "globalAIMods = { num = 2; item[1] = { aiType = \"524288\"; healthMod = \"stock\"; } } } }" :
        !strcmp(path, "generated/decls/entitydef/inherited-ai.decl") ? "{ class = \"Entity\"; edit = {} }" : NULL;
    if (!text) return registration_original(context, path, body, length);
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* The conductor is found through a dependency; only its parent is supplied.
 * Preparing explicit package files alone misses this inherited native table. */
static void test_registration_discovered_inheritance(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library_native native = {0};
    char error[1024], path[256], text[1024];
    registration_root("registration-discovered-inheritance");
    env.baseline = discovered_registration_original; env.baseline_context = &originals;
    native.private_keys = 1;
    const char *names[] = {"red", "blue"};
    for (size_t i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "overrides/%s/package.json", names[i]);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", names[i], names[i]); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor-parent.decl", names[i]);
        snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 3; "
            "item[1] = { aiType = \"524288\"; entityDef = \"inherited-ai\"; } } "
            "globalAIMods = { num = 2; item[1] = { aiType = \"524288\"; healthMod = \"%s\"; } } } }", names[i]);
        create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/registration-user.decl", names[i]);
        create(path, "{ class = \"Entity\"; edit = { target = \"conductor\"; } }");
    }
    for (int round = 0; round < 2; round++) {
        if (round) rename_package("red", "zz-red");
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        sh_package_library library = {0};
        int ok = sh_package_library_compile(sources, &env, NULL, discovered_registration_original, &originals,
            registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "discovered inherited registrations: %s\n", error);
        assert(ok);
        /* Prepared registrations inherit each package's supplied parent: their
         * discoveries decline replay, in either package order. */
        library_replay_same(&library, 0, sources, &env, discovered_registration_original, &originals,
            registry, (uintptr_t)f->reflection, &native);
        uint32_t masks[2] = {0};
        g_key_groups = library.key_groups;
        for (size_t color = 0; color < 2; color++) {
            const char *package = color ? "blue" : round ? "zz-red" : "red";
            const char *ai = scoped_entity_name(&library, sources, package, "inherited-ai");
            const char *table_name = scoped_entity_name(&library, sources, package, "conductor");
            const sh_compiled_resource *table = table_name ? named_resource(library.compiled, "entitydef", table_name) : NULL;
            key_row rows[32], mods[32], damage[32];
            assert(ai && table);
            size_t count = key_rows(f, table, "aiTypeList", "entityDef", rows, 32);
            size_t mod_count = key_rows(f, table, "globalAIMods", "healthMod", mods, 32);
            size_t damage_count = key_rows(f, table, "globalAIMods", "damageMod", damage, 32);
            for (size_t r = 0; r < count; r++) if (!strcmp(rows[r].field, ai)) masks[color] = rows[r].mask;
            assert(masks[color]);
            const char *health = modifier_for(mods, mod_count, masks[color]);
            const char *multiplier = modifier_for(damage, damage_count, masks[color]);
            if (!health || strcmp(health, names[color])) fprintf(stderr, "discovered %s health=%s damage=%s mask=%u table=%.*s\n",
                package, health ? health : "absent", multiplier ? multiplier : "absent", masks[color], (int)table->body_length, table->body);
            assert(health && !strcmp(health, names[color]));
            assert(multiplier && !strcmp(multiplier, "2"));
        }
        assert(masks[0] != masks[1]);
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    rename_package("zz-red", "red");
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
static void test_registration_inherited_lists(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    char error[1024], path[256], text[1024];
    registration_root("registration-inherited-lists");
    env.baseline = registration_original; env.baseline_context = &originals;
    native.private_keys = 1;
    const char *names[] = {"red", "blue"};
    for (size_t i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "overrides/%s/package.json", names[i]);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", names[i], names[i]); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor-parent.decl", names[i]);
        create(path, "{ class = \"Entity\"; edit = { aiTypeList = { num = 3; "
            "item[1] = { aiType = \"524288\"; entityDef = \"inherited-ai\"; } } "
            "globalAIMods = { num = 2; item[1] = { aiType = \"524288\"; healthMod = \"parent\"; } } } }");
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", names[i]);
        snprintf(text, sizeof(text), "{ inherit = \"conductor-parent\"; edit = { globalAIMods = { "
            "item[1] = { healthMod = \"%s\"; } } } }", names[i]); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/inherited-ai.decl", names[i]);
        create(path, "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    }
    for (int round = 0; round < 2; round++) {
        if (round) rename_package("red", "zz-red");
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        sh_package_library library = {0};
        if (!round) {
            sh_decl_native_schema *schema = sh_decl_native_schema_open(
                (sh_decl_native_source){f, read_memory, (uintptr_t)f->reflection},
                (sh_decl_dependency_observer){0}, error, sizeof(error));
            assert(schema);
            for (int typed = 0; typed < 2; typed++) {
                sh_package_scope scope = {0}; sh_package_compile_environment alone_env = env;
                alone_env.independent_sources = 1;
                alone_env.types = typed ? sh_decl_native_schema_view(schema) : NULL;
                alone_env.class_derives = registration_class_derives; alone_env.type_context = schema;
                assert(sh_package_scope_open(sources, 0, &scope, error, sizeof(error)));
                sh_package_compilation *alone = sh_package_compile_with(&scope.sources, &alone_env, error, sizeof(error));
                if (!alone) fprintf(stderr, "isolated native slots (typed %d): %s\n", typed, error);
                assert(alone);
                const sh_compiled_resource *raw = named_resource(alone, "entitydef", "conductor-parent");
                assert(raw);
                sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)raw->body, raw->body_length}, error, sizeof(error));
                assert(tree);
                const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
                const sh_decl_node *rows = sh_decl_tree_member(edit, "globalAIMods");
                assert(sh_decl_tree_member(rows, "item[1]") && !sh_decl_tree_member(rows, "item[0]"));
                assert(!strcmp(sh_decl_tree_member(rows, "num")->value, "2"));
                sh_decl_tree_free(tree);
                sh_package_compilation_free(alone); sh_package_scope_close(&scope);
            }
            sh_decl_native_schema_close(schema);
        }
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
            (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "inherited registrations: %s\n", error);
        assert(ok);
        uint32_t red = scoped_modifier(f, &library, sources, round ? "zz-red" : "red", "inherited-ai", "red");
        uint32_t blue = scoped_modifier(f, &library, sources, "blue", "inherited-ai", "blue");
        assert(red != blue);
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    rename_package("zz-red", "red");
    create("overrides/broken/package.json", "{\"id\":\"local.broken\",\"name\":\"broken\"}");
    create("overrides/broken/assets/generated/decls/entitydef/broken.decl",
        "{ inherit = \"conductor\"; edit = { aiTypeList = { num = 1; }");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_library library = {0}; size_t invalid = SIZE_MAX, unserved = SIZE_MAX;
    env.invalid_package = &invalid; env.unserved_package = &unserved;
    int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
        (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    if (ok || invalid == SIZE_MAX) fprintf(stderr, "inherited malformed owner: invalid=%zu %s\n", invalid, error);
    assert(!ok && invalid == first_package(sources, "broken") && unserved == SIZE_MAX);
    sh_package_library_free(&library); sh_package_sources_free(sources);
    create("overrides/broken/assets/generated/decls/entitydef/broken.decl",
        "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; item[1] = {} } } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
        (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    assert(!ok && invalid == first_package(sources, "broken") && unserved == SIZE_MAX);
    sh_package_library_free(&library);
    assert(sh_package_sources_remove(sources, invalid));
    ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
        (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX);
    scoped_modifier(f, &library, sources, "red", "inherited-ai", "red");
    scoped_modifier(f, &library, sources, "blue", "inherited-ai", "blue");
    sh_package_library_free(&library); sh_package_sources_free(sources);
    create("overrides/broken/assets/generated/decls/entitydef/broken.decl",
        "{ inherit = \"absent-registration-parent\"; edit = { } }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
        (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    if (ok || unserved == SIZE_MAX) fprintf(stderr, "inherited missing owner: unserved=%zu %s\n", unserved, error);
    assert(!ok && invalid == SIZE_MAX && unserved == first_package(sources, "broken"));
    sh_package_library_free(&library);
    assert(sh_package_sources_remove(sources, unserved));
    ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
        (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    assert(ok && invalid == SIZE_MAX && unserved == SIZE_MAX);
    scoped_modifier(f, &library, sources, "red", "inherited-ai", "red");
    scoped_modifier(f, &library, sources, "blue", "inherited-ai", "blue");
    sh_package_library_free(&library); sh_package_sources_free(sources);
    create("overrides/broken/assets/generated/decls/entitydef/broken.decl",
        "{ inherit = \"parent-only\"; edit = { } }");
    f->unreadable = "generated/decls/entitydef/parent-only.decl";
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals, registry,
        (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    assert(!ok && invalid == SIZE_MAX && unserved == SIZE_MAX);
    f->unreadable = NULL;
    sh_package_library_free(&library); sh_package_sources_free(sources);
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
static void test_registration_named_modifier_scope(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library_native native = {0};
    char error[1024]; size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    registration_root("registration-named-modifier-scope");
    registration_conductor = "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } } globalAIMods = { num = 1; "
        "item[0] = { aiType = \"IMP\"; healthMod = \"stock\"; } } } }";
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid; native.private_keys = 1;
    for (size_t i = 0; i < 2; i++) {
        const char *name = i ? "blue" : "red";
        char path[256], text[1024];
        snprintf(path, sizeof(path), "overrides/%s/package.json", name);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", name, name); create(path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", name);
        snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
            "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } "
            "item[1] = { aiType = \"524292\"; entityDef = \"t3\"; } } globalAIMods = { num = 1; "
            "item[0] = { aiType = \"IMP\"; healthMod = \"%s\"; } } } }", name); create(path, text);
    }
    for (int round = 0; round < 2; round++) {
        if (round) rename_package("red", "zz-red");
        sh_package_library library = {0};
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "named modifier scope: %s\n", error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        uint32_t red = scoped_modifier(f, &library, sources, round ? "zz-red" : "red", "t3", "red");
        uint32_t blue = scoped_modifier(f, &library, sources, "blue", "t3", "blue");
        assert(red != blue);
        const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
        key_row mods[32]; size_t count = key_rows(f, conductor, "globalAIMods", "healthMod", mods, 32);
        assert(!strcmp(modifier_for(mods, count, 4u), "stock"));
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    rename_package("zz-red", "red");
    registration_conductor = NULL; cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
static void test_registration_private_capacity(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024], names[13][16];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    sh_package_library_native native = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    native.private_keys = 1;
    registration_root("registration-private-capacity");
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    for (int i = 0; i < 13; i++) { snprintf(names[i], sizeof(names[i]), "cap%02d", i); registration_package(names[i], "524288"); }
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "registration private capacity: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    g_key_groups = library.key_groups;
    const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
    key_row rows[16], mods[16];
    size_t count = key_rows(f, conductor, "aiTypeList", "entityDef", rows, 16);
    size_t mod_count = key_rows(f, conductor, "globalAIMods", "healthMod", mods, 16);
    uint32_t keys[13];
    size_t marked = 0;
    assert(count == 14 && mod_count == 13);
    for (int i = 0; i < 13; i++) {
        const char *out[16], *boss = package_boss(library.compiled, names[i]);
        keys[i] = (uint32_t)strtoul(spawner_key(library.compiled, names[i]), NULL, 10);
        assert(sh_package_key_private_valid(keys[i]));
        if (keys[i] & SH_PACKAGE_KEY_MARKER) marked++;
        for (int j = 0; j < i; j++) assert(keys[j] != keys[i]);
        size_t n = selected(rows, count, 4u | keys[i], out, 16);
        assert(n == 2 && holds(out, n, boss) && holds(out, n, "imp"));
        n = selected(rows, count, keys[i], out, 16);
        assert(n == 1 && !strcmp(out[0], boss));
        uint32_t type = 0;
        for (size_t r = 0; r < count; r++) if (!strcmp(rows[r].field, boss)) type = rows[r].mask;
        assert(type == keys[i] && !strcmp(modifier_for(mods, mod_count, type), names[i]));
    }
    assert(marked == 4 && (!library.coverage || !strstr(library.coverage, "spawn")));
    sh_package_library_free(&library); sh_package_sources_free(sources);
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}

/* A compile given a peer library's keys (a map compile beside the installed
 * library, or a recompile) keys every scope of the same source as the peer
 * does, so identical sources keep their outputs and served names; a package
 * the peer lacks takes a key no peer scope holds, even where it would
 * otherwise sort first and keep its authored bit. */
static void pinned_compile(fixture *f, const sh_package_provenance *keys, sh_package_sources **sources,
    sh_package_library *library)
{
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    char error[1024];
    native.private_keys = 1; native.keys = keys;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(*sources);
    int ok = sh_package_library_compile(*sources, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native, library, error, sizeof(error));
    if (!ok) fprintf(stderr, "registration pinned: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    library_key_memo_same(library, *sources, &env, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native);
}
static uint32_t variant_key(const sh_package_compilation *compiled, const char *package)
{ return (uint32_t)(strtoul(spawner_value(compiled, package), NULL, 10) & ~(8ul | 131072ul)); }
/* A frozen legacy (composed) context keeps the authored bits of its AI
 * registrations and publishes them as historical: they keep native matching
 * there (stock composites and legacy group masks select them, as they did
 * natively), a modern scope authoring or holding the same bit in the same
 * compile takes another key, and no key or group code of the compile touches a
 * historical bit. Native-only generations have none. */
static void historical_compile(fixture *f, const sh_package_provenance *keys, const char *legacy_name,
    const char *const *modern_names, size_t modern_count, sh_package_sources **sources, sh_package_library *out)
{
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    sh_package_provenance contexts = {0};
    size_t legacy = SIZE_MAX, added = 0;
    char error[1024];
    native.private_keys = 1; native.keys = keys;
    env.baseline = registration_original; env.baseline_context = &originals;
    *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(*sources);
    for (size_t i = 0; i < (*sources)->package_count; i++) {
        if (!strcmp((*sources)->packages[i].name, legacy_name)) legacy = i;
        for (size_t m = 0; m < modern_count; m++) if (!strcmp((*sources)->packages[i].name, modern_names[m])) {
            assert(sh_package_provenance_context_add(&contexts, SH_PACKAGE_CONTEXT_ISOLATED,
                (const unsigned char (*)[32])sh_package_source_tree(*sources, i), 1, "") != SIZE_MAX);
            added++;
        }
    }
    assert(legacy != SIZE_MAX && added == modern_count);
    assert(sh_package_provenance_context_add(&contexts, SH_PACKAGE_CONTEXT_COMPOSED,
        (const unsigned char (*)[32])sh_package_source_tree(*sources, legacy), 1, NULL) != SIZE_MAX);
    int ok = sh_package_library_compile_contexts(*sources, &contexts, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native, out, error, sizeof(error));
    if (!ok) fprintf(stderr, "historical keys: %s\n", error);
    assert(ok);
    sh_package_provenance_free(&contexts);
}
static uint32_t historical_key(const sh_package_library *library, uint32_t original)
{
    char key[16]; snprintf(key, sizeof(key), "%u", original);
    for (size_t i = 0; i < library->bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = library->bindings.bindings + i;
        if (strcmp(binding->family, SH_PACKAGE_KEY_FAMILY) || strcmp(binding->original, key)) continue;
        for (size_t c = 0; c < binding->context_count; c++)
            if (library->bindings.contexts[binding->contexts[c]].mode == SH_PACKAGE_CONTEXT_COMPOSED)
                return (uint32_t)strtoul(binding->name, NULL, 10);
    }
    assert(0); return 0;
}
static void test_registration_historical_keys(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    const uint32_t imp = 4u, all = 0xffffffffu, k19 = 524288u, k20 = 1048576u, m19 = SH_PACKAGE_KEY_MARKER | k19;
    sh_package_key_groups history = {0};
    history.historical = k19 | k20;
    assert(sh_package_key_request_matches(&history, all, k19) && sh_package_key_request_matches(&history, 0xbffdffffu, imp | k19));
    assert(sh_package_key_request_matches(&history, imp, imp | k19) && sh_package_key_request_matches(&history, k19 | k20, k20));
    assert(!sh_package_key_request_matches(&history, all, 1u << 21) && !sh_package_key_request_matches(&history, all, m19));
    assert(!sh_package_key_request_matches(NULL, all, k19) && !sh_package_key_request_matches(NULL, imp, imp | k19));
    assert(sh_package_key_modifier_matches(&history, imp | k19, imp | k20) && !sh_package_key_modifier_matches(NULL, imp | k19, imp | k20));
    assert(sh_package_key_filter_matches(&history, k19 | k20, imp | k19) && !sh_package_key_filter_matches(&history, 1u << 21, imp | k19));
    /* A legacy registration may use bit 28: natively it is a plain bit, not a
     * marked code, for requests and filters alike. */
    sh_package_key_groups marked = {0};
    marked.historical = SH_PACKAGE_KEY_MARKER | k20;
    assert(sh_package_key_request_matches(&marked, SH_PACKAGE_KEY_MARKER, SH_PACKAGE_KEY_MARKER | k20) &&
        !sh_package_key_request_matches(NULL, SH_PACKAGE_KEY_MARKER, SH_PACKAGE_KEY_MARKER | k20));
    assert(sh_package_key_filter_matches(&marked, SH_PACKAGE_KEY_MARKER, SH_PACKAGE_KEY_MARKER | k20) &&
        !sh_package_key_filter_matches(NULL, SH_PACKAGE_KEY_MARKER, SH_PACKAGE_KEY_MARKER | k20));
    /* A mixed context: the legacy package keeps its authored key while the
     * modern one moves off every historical bit. */
    sh_package_library first = {0}, library = {0}, second = {0};
    sh_package_sources *sources = NULL, *more = NULL, *installed = NULL;
    registration_root("registration-historical");
    registration_package("zlegacy", "524288"); registration_package("amodern", "524288");
    const char *const modern[] = {"amodern"};
    historical_compile(f, NULL, "zlegacy", modern, 1, &sources, &first);
    uint32_t kept = historical_key(&first, k19);
    uint32_t moved = (uint32_t)strtoul(spawner_key(first.compiled, "amodern"), NULL, 10);
    assert(sh_package_key_private_valid(kept) && sh_package_key_private_valid(moved) && kept != moved);
    uint32_t request = spawner_request(first.compiled, "zlegacy");
    assert(sh_package_key_request_matches(first.key_groups, request, kept));
    assert(sh_package_key_request_matches(first.key_groups, request, historical_key(&first, imp) | imp));
    assert(!sh_package_key_request_matches(first.key_groups, request, moved));
    assert(!first.key_groups || !first.key_groups->historical);
    /* The installed library, compiled natively, may give the bit to the modern
     * package: a map context holding it historically moves that pin. */
    pinned_compile(f, NULL, &installed, &library);
    assert(strtoul(spawner_key(library.compiled, "amodern"), NULL, 10) == k19);
    assert(!library.key_groups || !library.key_groups->historical);
    historical_compile(f, &library.bindings, "zlegacy", modern, 1, &more, &second);
    kept = historical_key(&second, k19);
    moved = (uint32_t)strtoul(spawner_key(second.compiled, "amodern"), NULL, 10);
    assert(kept != moved && moved == k19 && sh_package_key_private_valid(kept));
    assert(!sh_package_key_request_matches(second.key_groups, spawner_request(second.compiled, "zlegacy"), moved));
    sh_package_library_free(&second); sh_package_sources_free(more);
    sh_package_library_free(&library); sh_package_sources_free(installed);
    sh_package_library_free(&first); sh_package_sources_free(sources);
    cleanup();
    /* Ten modern packages beside the legacy one holding another bit: one
     * keeps its authored bit, seven take the other free single keys, two take
     * marked keys, and none carries the historical bit. */
    {
        char names[10][8]; const char *list[10];
        sh_package_library many = {0};
        sh_package_key_set seen = {{0}};
        registration_root("registration-historical-many");
        registration_package("zlegacy", "1048576");
        for (int i = 0; i < 10; i++) {
            snprintf(names[i], sizeof(names[i]), "m%02d", i); list[i] = names[i]; registration_package(names[i], "524288");
        }
        historical_compile(f, NULL, "zlegacy", list, 10, &sources, &many);
        uint32_t legacy = historical_key(&many, k20);
        int marks = 0;
        for (int i = 0; i < 10; i++) {
            uint32_t key = (uint32_t)strtoul(spawner_key(many.compiled, names[i]), NULL, 10);
            assert(sh_package_key_private_valid(key) && key != legacy && sh_package_key_set_add(&seen, key));
            assert(!sh_package_key_request_matches(many.key_groups, spawner_request(many.compiled, "zlegacy"), key));
            marks += (key & SH_PACKAGE_KEY_MARKER) != 0;
        }
        assert(marks == 1 && sh_package_key_set_count(&seen) == 10);
        assert(!many.key_groups || !many.key_groups->historical);
        sh_package_library_free(&many); sh_package_sources_free(sources);
        cleanup();
    }
    assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
/* A frozen legacy context's AI registrations reach the engine as its
 * whole-file conductor did: the engine finds the conductor by its stock name,
 * so the legacy copy composes against the stock one under that name, keeping
 * its authored keys, and the stock rows it removed stay removed (its requests
 * keep their historical members). A modern context beside it adds its rows
 * under a key clear of the historical bits. */
static void test_registration_historical_conductor(void)
{
    static const char *historical_stock = "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"T3\"; entityDef = \"t3\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = \"IMP\"; healthMod = \"stock\"; } } } }";
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_package_library legacy = {0}, mixed = {0};
    sh_package_sources *sources = NULL, *more = NULL;
    char rows[16][96], mods[16][96], row[160];
    registration_root("registration-historical-conductor");
    registration_conductor = historical_stock;
    registration_package("zlegacy", "524288"); registration_package("amodern", "524288");
    historical_compile(f, NULL, "zlegacy", NULL, 0, &sources, &legacy);
    const sh_compiled_resource *conductor = named_resource(legacy.compiled, "entitydef", "conductor");
    assert(conductor && !strcmp(conductor->engine_path, "generated/decls/entitydef/conductor.decl"));
    size_t count = conductor_rows(conductor, "aiTypeList", "entityDef", rows, 16);
    size_t mod_count = conductor_rows(conductor, "globalAIMods", "healthMod", mods, 16);
    snprintf(row, sizeof(row), "%u=%s", historical_key(&legacy, 524288), authored_name(legacy.compiled, "zlegacy_boss"));
    assert(count == 2 && row_index(rows, count, row) != SIZE_MAX);
    /* This historical table omits the stock IMP modifier, so its imp's
     * classification is private even though the AI file itself is stock. */
    const char *legacy_imp = scoped_entity_name(&legacy, sources, "zlegacy", "imp");
    if (!legacy_imp) for (size_t r = 0; r < count; r++) fprintf(stderr, "historical row %s\n", rows[r]);
    assert(legacy_imp && strcmp(legacy_imp, "imp"));
    snprintf(row, sizeof(row), "%u=%s", historical_key(&legacy, 4) | 4, legacy_imp);
    assert(row_index(rows, count, row) != SIZE_MAX);
    snprintf(row, sizeof(row), "%u=zlegacy", historical_key(&legacy, 524288));
    assert(mod_count == 1 && row_index(mods, mod_count, row) != SIZE_MAX);
    const char *const modern[] = {"amodern"};
    historical_compile(f, NULL, "zlegacy", modern, 1, &more, &mixed);
    conductor = named_resource(mixed.compiled, "entitydef", "conductor");
    assert(conductor);
    count = conductor_rows(conductor, "aiTypeList", "entityDef", rows, 16);
    uint32_t moved = (uint32_t)strtoul(spawner_key(mixed.compiled, "amodern"), NULL, 10);
    assert(moved != historical_key(&mixed, 524288));
    snprintf(row, sizeof(row), "%u=%s", historical_key(&mixed, 524288), authored_name(mixed.compiled, "zlegacy_boss"));
    assert(row_index(rows, count, row) != SIZE_MAX);
    snprintf(row, sizeof(row), "%u=%s", moved, authored_name(mixed.compiled, "amodern_boss"));
    assert(row_index(rows, count, row) != SIZE_MAX);
    assert(!sh_package_key_request_matches(mixed.key_groups, spawner_request(mixed.compiled, "zlegacy"), moved));
    /* A modern map's copy alone stays private: the installed library's
     * conductor serves its rows under the keys the library gave them. */
    {
        sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
        source_link_fixture originals = {f};
        sh_package_compile_environment env = {0};
        sh_package_library_native native = {0};
        sh_package_provenance contexts = {0};
        sh_package_library modern_only = {0};
        char error[1024];
        size_t modern_index = SIZE_MAX;
        native.private_keys = 1;
        env.baseline = registration_original; env.baseline_context = &originals;
        for (size_t i = 0; i < more->package_count; i++) if (!strcmp(more->packages[i].name, "amodern")) modern_index = i;
        assert(modern_index != SIZE_MAX && sh_package_provenance_context_add(&contexts, SH_PACKAGE_CONTEXT_ISOLATED,
            (const unsigned char (*)[32])sh_package_source_tree(more, modern_index), 1, "") != SIZE_MAX);
        int ok = sh_package_library_compile_contexts(more, &contexts, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, &native, &modern_only, error, sizeof(error));
        if (!ok) fprintf(stderr, "modern map conductor: %s\n", error);
        assert(ok && !named_resource(modern_only.compiled, "entitydef", "conductor"));
        sh_package_library_free(&modern_only); sh_package_provenance_free(&contexts);
    }
    sh_package_library_free(&mixed); sh_package_sources_free(more);
    sh_package_library_free(&legacy); sh_package_sources_free(sources);
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}
static void test_registration_pinned_keys(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_package_library first = {0}, second = {0};
    sh_package_sources *sources = NULL, *more = NULL;
    char names[12][16], parked[MAX_PATH], from[MAX_PATH];
    uint32_t held[11];
    int keeper = -1;
    /* Custom keys: eleven holders (two past the single bits), then without
     * the keeper of 524288 a newcomer of the same authored key. */
    registration_root("registration-pinned");
    for (int i = 0; i < 11; i++) { snprintf(names[i], sizeof(names[i]), "pin%02d", i); registration_package(names[i], "524288"); }
    pinned_compile(f, NULL, &sources, &first);
    for (int i = 0; i < 11; i++) {
        held[i] = (uint32_t)strtoul(spawner_key(first.compiled, names[i]), NULL, 10);
        if (held[i] == 524288u) keeper = i;
    }
    assert(keeper >= 0);
    snprintf(from, sizeof(from), "%s\\overrides\\%s", root, names[keeper]);
    snprintf(parked, sizeof(parked), "%s\\parked", root);
    assert(MoveFileA(from, parked));
    snprintf(names[11], sizeof(names[11]), "aaa0"); registration_package(names[11], "524288");
    pinned_compile(f, &first.bindings, &more, &second);
    uint32_t newcomer = (uint32_t)strtoul(spawner_key(second.compiled, names[11]), NULL, 10);
    assert(sh_package_key_private_valid(newcomer));
    for (int i = 0; i < 11; i++) {
        assert(newcomer != held[i]);
        if (i != keeper) assert((uint32_t)strtoul(spawner_key(second.compiled, names[i]), NULL, 10) == held[i]);
    }
    sh_package_library_free(&second); sh_package_sources_free(more);
    sh_package_library_free(&first); sh_package_sources_free(sources);
    assert(MoveFileA(parked, from));
    cleanup();
    /* Named variants: two changes of the stock imp, then a third. */
    registration_root("registration-pinned-named");
    registration_conductor = variant_stock;
    variant_package("fire", "fire_imp", "fire"); variant_package("ice", "ice_imp", "stock");
    pinned_compile(f, NULL, &sources, &first);
    uint32_t fire = variant_key(first.compiled, "fire"), ice = variant_key(first.compiled, "ice");
    variant_package("aaa1", "aaa1_imp", "aaa1");
    pinned_compile(f, &first.bindings, &more, &second);
    uint32_t ash = variant_key(second.compiled, "aaa1");
    assert(variant_key(second.compiled, "fire") == fire && variant_key(second.compiled, "ice") == ice);
    assert(sh_package_key_private_valid(ash) && ash != fire && ash != ice);
    sh_package_library_free(&second); sh_package_sources_free(more);
    sh_package_library_free(&first); sh_package_sources_free(sources);
    registration_conductor = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}

/* A map compile recompiles the packages a saved map uses under that map's
 * contexts. Given the installed library's keys it serves every one of their
 * resources under the installed library's names, so each saved use resolves
 * to exactly one current binding: a spawner's relocated key and a holder
 * referring to that spawner keep their served identities. */
static void test_registration_context_names(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    sh_package_library library = {0}, restored = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX, length = 0, checked = 0;
    char error[1024], path[512], text[256];
    native.private_keys = 1;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    registration_root("registration-context-names");
    for (int i = 0; i < 12; i++) {
        char name[16];
        snprintf(name, sizeof(name), "ctx%02d", i); registration_package(name, "524288");
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_holder.decl", name, name);
        snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { target = \"%s_spawner\"; } }", name);
        create(path, text);
    }
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "registration context names: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    sh_package_provenance saved = {0};
    unsigned char *encoded = sh_package_provenance_encode(&library.bindings, &length, error, sizeof(error));
    assert(encoded && sh_package_provenance_decode(encoded, length, &saved, error, sizeof(error))); free(encoded);
    native.keys = &library.bindings;
    ok = sh_package_library_compile_contexts(sources, &saved, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &restored, error, sizeof(error));
    if (!ok) fprintf(stderr, "registration context names restored: %s\n", error);
    assert(ok);
    for (size_t i = 0; i < saved.binding_count; i++) {
        const sh_package_provenance_binding *binding = saved.bindings + i;
        const char *name = NULL;
        /* The stock-named registration list itself is no map use. */
        if (strcmp(binding->family, "entitydef") || !strcmp(binding->original, "conductor")) continue;
        int resolved = sh_package_provenance_resolve(&saved, &restored.bindings, binding->family, binding->name, &name);
        if (resolved != 1 || strcmp(name, binding->name)) {
            char detail[768];
            sh_package_provenance context_view = {0};
            (void)context_view;
            fprintf(stderr, "context compile renamed %s %s: %s -> %s\n", binding->family, binding->original,
                binding->name, name ? name : "(none)");
            (void)detail;
        }
        assert(resolved == 1 && !strcmp(name, binding->name));
        checked++;
    }
    assert(checked >= 24);
    /* A map uses a few of them: only their contexts are compiled. */
    {
        sh_package_provenance uses = {0};
        sh_package_library map = {0};
        size_t picked = 0;
        for (size_t i = 0; i < saved.binding_count; i++) {
            const sh_package_provenance_binding *binding = saved.bindings + i;
            size_t context;
            if (strcmp(binding->family, "entitydef") || (!strstr(binding->original, "ctx03_holder") &&
                !strstr(binding->original, "ctx09_holder")) || binding->context_count != 1) continue;
            const sh_package_provenance_context *recorded = saved.contexts + binding->contexts[0];
            context = sh_package_provenance_context_add(&uses, recorded->mode, recorded->trees, recorded->tree_count,
                recorded->component);
            assert(context != SIZE_MAX && sh_package_provenance_bind(&uses, binding->family, binding->original,
                binding->name, &context, 1));
            picked++;
        }
        assert(picked == 2);
        ok = sh_package_library_compile_contexts(sources, &uses, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, &native, &map, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration context names map: %s\n", error);
        assert(ok);
        for (size_t i = 0; i < uses.binding_count; i++) {
            const char *name = NULL;
            int resolved = sh_package_provenance_resolve(&uses, &map.bindings, uses.bindings[i].family,
                uses.bindings[i].name, &name);
            if (resolved != 1 || strcmp(name, uses.bindings[i].name))
                fprintf(stderr, "map compile renamed %s: %s -> %s\n", uses.bindings[i].original, uses.bindings[i].name,
                    name ? name : "(none)");
            assert(resolved == 1 && !strcmp(name, uses.bindings[i].name));
        }
        sh_package_library_free(&map); sh_package_provenance_free(&uses);
    }
    sh_package_provenance_free(&saved);
    sh_package_library_free(&restored); sh_package_library_free(&library); sh_package_sources_free(sources);
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}

/* The engine ignores a number after a name, so a row spelled "IMP 524288"
 * registers a new imp and nothing more. Under the contract that new imp takes
 * a private key and the package's own IMP requests gain it, so they still
 * reach its AI beside the stock imp, while stock IMP requests do not. A row
 * written as one number of IMP's bit and a key ("524292") is natively an imp
 * and that key: its package's IMP requests gain the key it holds, so they keep
 * reaching that AI. Bit 29 marks stock composites under the contract, so a key
 * authored there moves;
 * natively both stay as authored. A conductor copy that leaves the stock rows
 * out removes them natively, but under the contract the stock registration
 * stays for everyone else. */
static void test_registration_private_hybrid(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024];
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    sh_package_library_native native = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    registration_root("registration-hybrid");
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    create("overrides/hybrid/package.json", "{\"id\":\"local.hybrid\",\"name\":\"hybrid\"}");
    create("overrides/hybrid/assets/generated/decls/entitydef/conductor.decl", "{ class = \"Entity\"; edit = { "
        "aiTypeList = { num = 2; item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } "
        "item[1] = { aiType = \"IMP 524288\"; entityDef = \"hybrid_imp\"; } } } }");
    create("overrides/hybrid/assets/generated/decls/entitydef/hybrid_imp.decl", "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    create("overrides/hybrid/assets/generated/decls/entitydef/hybrid_spawner.decl", "{ class = \"Entity\"; edit = { spawnType = \"IMP\"; } }");
    registration_package("high", "536870912");
    create("overrides/numeric/package.json", "{\"id\":\"local.numeric\",\"name\":\"numeric\"}");
    create("overrides/numeric/assets/generated/decls/entitydef/conductor.decl", "{ class = \"Entity\"; edit = { "
        "aiTypeList = { num = 1; item[0] = { aiType = \"524292\"; entityDef = \"numeric_imp\"; } } } }");
    create("overrides/numeric/assets/generated/decls/entitydef/numeric_imp.decl", "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    create("overrides/numeric/assets/generated/decls/entitydef/numeric_spawner.decl", "{ class = \"Entity\"; edit = { spawnType = \"IMP\"; } }");
    create("overrides/sparse/package.json", "{\"id\":\"local.sparse\",\"name\":\"sparse\"}");
    create("overrides/sparse/assets/generated/decls/entitydef/conductor.decl", "{ class = \"Entity\"; edit = { "
        "aiTypeList = { num = 1; item[0] = { aiType = \"2097152\"; entityDef = \"sparse_boss\"; } } } }");
    create("overrides/sparse/assets/generated/decls/entitydef/sparse_boss.decl", "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    for (int mode = 0; mode < 2; mode++) {
        native.private_keys = mode;
        sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
        int ok = sh_package_library_compile(sources, &env, NULL, registration_original, &originals,
            registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
        if (!ok) fprintf(stderr, "registration hybrid %d: %s\n", mode, error);
        assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
        g_key_groups = library.key_groups;
        const char *hybrid = spawner_value(library.compiled, "hybrid"), *high = spawner_key(library.compiled, "high");
        assert(hybrid && high);
        if (!mode) {
            assert(!strcmp(hybrid, "IMP") && !strcmp(high, "536870912") &&
                !strcmp(spawner_value(library.compiled, "numeric"), "IMP"));
            assert(!conductor_row(named_resource(library.compiled, "entitydef", "conductor"), "IMP"));
        }
        else {
            const sh_compiled_resource *conductor = named_resource(library.compiled, "entitydef", "conductor");
            key_row rows[16];
            const char *out[16], *ai = authored_name(library.compiled, "hybrid_imp");
            size_t count = key_rows(f, conductor, "aiTypeList", "entityDef", rows, 16);
            uint32_t requested = (uint32_t)strtoul(hybrid, NULL, 10), key = requested & ~4u;
            uint32_t held = (uint32_t)strtoul(high, NULL, 10);
            assert((requested & 4u) && sh_package_key_private_valid(key) && strcmp(high, "536870912") &&
                sh_package_key_private_valid(held) && held != key);
            size_t n = selected(rows, count, 4u, out, 16);
            assert(n == 1 && !strcmp(out[0], "imp"));
            n = selected(rows, count, requested, out, 16);
            assert(n == 2 && holds(out, n, ai) && holds(out, n, "imp"));
            n = selected(rows, count, key, out, 16);
            assert(n == 1 && !strcmp(out[0], ai));
            /* The numeric row's package asks for IMP with the key its row holds. */
            const char *numeric = spawner_value(library.compiled, "numeric"), *numeric_ai = authored_name(library.compiled, "numeric_imp");
            uint32_t asked = (uint32_t)strtoul(numeric, NULL, 10), numeric_key = asked & ~4u;
            assert((asked & 4u) && sh_package_key_private_valid(numeric_key) && numeric_key != key && numeric_key != held);
            n = selected(rows, count, asked, out, 16);
            assert(n == 2 && holds(out, n, numeric_ai) && holds(out, n, "imp"));
        }
        sh_package_library_free(&library); sh_package_sources_free(sources);
    }
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}

/* One package registering two custom AI (authored keys 524288 and 1048576)
 * with a spawner, AI filter and modifier row for both (1572864). */
static void group_package(const char *package)
{
    char path[512], text[768];
    snprintf(path, sizeof(path), "overrides/%s/package.json", package);
    snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", package, package); create(path, text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", package);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 3; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = { aiType = \"524288\"; entityDef = \"%s_one\"; } "
        "item[2] = { aiType = \"1048576\"; entityDef = \"%s_two\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = \"1572864\"; healthMod = \"%s\"; } } } }",
        package, package, package); create(path, text);
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_%s.decl", package, package, i ? "two" : "one");
        snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { target = \"local-only\"; } }"); create(path, text);
    }
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_spawner.decl", package, package);
    create(path, "{ class = \"Entity\"; edit = { spawnType = \"1572868\"; } }");
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_filter.decl", package, package);
    create(path, "{ class = \"Entity\"; edit = { aiFilter = \"1572868\"; } }");
}
static void group_pin(sh_package_provenance *peer, const sh_package_sources *sources, const char *package,
    const char *original, uint32_t key)
{
    char current[16];
    size_t owner = SIZE_MAX, context;
    for (size_t i = 0; i < sources->package_count; i++) if (!strcmp(sources->packages[i].name, package)) owner = i;
    assert(owner != SIZE_MAX);
    context = sh_package_provenance_context_add(peer, SH_PACKAGE_CONTEXT_ISOLATED,
        (const unsigned char (*)[32])sh_package_source_tree(sources, owner), 1, "");
    snprintf(current, sizeof(current), "%u", key);
    assert(context != SIZE_MAX && sh_package_provenance_bind(peer, SH_PACKAGE_KEY_FAMILY, original, current, &context, 1));
}
static void group_compile(fixture *f, const sh_package_provenance *keys, const sh_package_key_groups *groups,
    sh_package_sources **sources, sh_package_library *library)
{
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    size_t unserved = SIZE_MAX, invalid = SIZE_MAX;
    char error[1024];
    native.private_keys = 1; native.keys = keys; native.key_groups = groups;
    env.baseline = registration_original; env.baseline_context = &originals;
    env.unserved_package = &unserved; env.invalid_package = &invalid;
    *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(*sources);
    int ok = sh_package_library_compile(*sources, &env, NULL, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native, library, error, sizeof(error));
    if (!ok) fprintf(stderr, "registration groups: %s\n", error);
    assert(ok && unserved == SIZE_MAX && invalid == SIZE_MAX);
    /* Group codes are defined in the same order with or without the memo. */
    library_key_memo_same(library, *sources, &env, registration_original, &originals,
        registry, (uintptr_t)f->reflection, &native);
    g_key_groups = library->key_groups;
}
static uint32_t group_value(const char *value) { return (uint32_t)strtoul(value, NULL, 10); }
/* The counterexample: A = marker|19|20 and B = marker|19|21 OR to C = 272105472.
 * The pair package's keys are A and B, the third package holds C. The pair's
 * request, filter and modifier for both carry one group code {A, B}: they
 * meet exactly the pair's two AI, never the third row, and C itself selects
 * only the third. A mixed package (a single-bit key and a multi-bit key) gets
 * a mixed group. A compile given this one as peer (a map compile or recompile)
 * keeps both group codes while a new package takes a fresh one. */
static void test_registration_marked_groups(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    const uint32_t a = 270008320u, b = 271056896u, c = 272105472u, k23 = 1u << 23, d = SH_PACKAGE_KEY_MARKER | (1u << 20);
    sh_package_library first = {0}, second = {0};
    sh_package_sources *scan = NULL, *sources = NULL, *more = NULL;
    sh_package_provenance peer = {0};
    char error[1024];
    registration_root("registration-groups");
    group_package("pair"); group_package("mixed"); registration_package("third", "524288");
    scan = sh_package_sources_scan(root, error, sizeof(error)); assert(scan);
    group_pin(&peer, scan, "pair", "524288", a); group_pin(&peer, scan, "pair", "1048576", b);
    group_pin(&peer, scan, "third", "524288", c);
    group_pin(&peer, scan, "mixed", "524288", k23); group_pin(&peer, scan, "mixed", "1048576", d);
    sh_package_sources_free(scan);
    group_compile(f, &peer, NULL, &sources, &first);
    assert(first.key_groups && first.key_groups->count == 2);
    const sh_compiled_resource *conductor = named_resource(first.compiled, "entitydef", "conductor");
    key_row rows[16], mods[16];
    size_t count = key_rows(f, conductor, "aiTypeList", "entityDef", rows, 16);
    size_t mod_count = key_rows(f, conductor, "globalAIMods", "healthMod", mods, 16);
    const char *out[16];
    uint32_t g = group_value(spawner_key(first.compiled, "pair")), h = group_value(spawner_key(first.compiled, "mixed"));
    uint32_t third = group_value(spawner_key(first.compiled, "third"));
    const char *pair_one = NULL, *pair_two = NULL, *mixed_one = NULL, *mixed_two = NULL, *third_boss = NULL;
    uint32_t type_one = 0, type_two = 0, type_third = 0;
    for (size_t r = 0; r < count; r++) {
        if (rows[r].mask == a) { pair_one = rows[r].field; type_one = rows[r].mask; }
        if (rows[r].mask == b) { pair_two = rows[r].field; type_two = rows[r].mask; }
        if (rows[r].mask == k23) mixed_one = rows[r].field;
        if (rows[r].mask == d) mixed_two = rows[r].field;
        if (rows[r].mask == c) { third_boss = rows[r].field; type_third = rows[r].mask; }
    }
    assert(pair_one && pair_two && mixed_one && mixed_two && third_boss && third == c);
    /* The group codes are neither keys nor the OR of their members. */
    assert((g & SH_PACKAGE_KEY_MARKER) && g != a && g != b && g != c && g != (a | b));
    assert((h & SH_PACKAGE_KEY_MARKER) && h != k23 && h != d && h != (k23 | d) && h != g);
    size_t n = selected(rows, count, g, out, 16);
    assert(n == 2 && holds(out, n, pair_one) && holds(out, n, pair_two));
    n = selected(rows, count, 4u | g, out, 16);
    assert(n == 3 && holds(out, n, "imp") && holds(out, n, pair_one) && holds(out, n, pair_two));
    n = selected(rows, count, h, out, 16);
    assert(n == 2 && holds(out, n, mixed_one) && holds(out, n, mixed_two));
    n = selected(rows, count, a | b, out, 16);
    assert(n == 1 && !strcmp(out[0], third_boss));
    /* Filters classify the same two, never the third. */
    uint32_t filter = group_value(filter_value(first.compiled, "pair"));
    assert(filter == (4u | g) && sh_package_key_filter_matches(first.key_groups, filter, type_one) &&
        sh_package_key_filter_matches(first.key_groups, filter, type_two) &&
        !sh_package_key_filter_matches(first.key_groups, filter, type_third) &&
        sh_package_key_filter_matches(first.key_groups, filter, 4u));
    /* The pair's modifier row reaches both its AI and not the third's. */
    assert(!strcmp(modifier_for(mods, mod_count, type_one), "pair") && !strcmp(modifier_for(mods, mod_count, type_two), "pair"));
    assert(!strcmp(modifier_for(mods, mod_count, type_third), "third"));
    (void)mod_count;
    /* A peer-grouped compile (the map beside this library, or a recompile)
     * with a new group user: equal sets keep their codes, the new set takes
     * a code none of them holds. */
    group_package("aaa_new");
    group_compile(f, &first.bindings, first.key_groups, &more, &second);
    assert(group_value(spawner_key(second.compiled, "pair")) == g && group_value(spawner_key(second.compiled, "mixed")) == h);
    /* The newcomer keeps the free authored bits (single-bit keys): their
     * union already names each, so it needs no group code. */
    uint32_t fresh = group_value(spawner_key(second.compiled, "aaa_new"));
    assert(fresh == (524288u | 1048576u) && second.key_groups && second.key_groups->count == 2);
    {
        const sh_compiled_resource *again = named_resource(second.compiled, "entitydef", "conductor");
        key_row more_rows[20];
        size_t more_count = key_rows(f, again, "aiTypeList", "entityDef", more_rows, 20);
        n = selected(more_rows, more_count, fresh, out, 16);
        assert(n == 2);
        for (size_t i = 0; i < n; i++) assert(strstr(out[i], "aaa_new") || strstr(out[i], "smp/"));
        n = selected(more_rows, more_count, g, out, 16);
        assert(n == 2);
    }
    sh_package_library_free(&second); sh_package_sources_free(more);
    sh_package_library_free(&first); sh_package_sources_free(sources); sh_package_provenance_free(&peer);
    g_key_groups = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}

/* A package whose IMP bit names two keys: it changes the stock IMP row to its
 * own AI and registers a custom AI whose row also names IMP (524292); its
 * spawner requests IMP. */
static void named_pair_package(const char *package)
{
    char path[512], text[768];
    snprintf(path, sizeof(path), "overrides/%s/package.json", package);
    snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", package, package); create(path, text);
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/conductor.decl", package);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = \"IMP\"; entityDef = \"%s_one\"; } "
        "item[1] = { aiType = \"524292\"; entityDef = \"%s_two\"; } } } }", package, package); create(path, text);
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_%s.decl", package, package, i ? "two" : "one");
        create(path, i ? "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }" :
            "{ class = \"Entity\"; edit = { target = \"local-only\"; } }");
    }
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s_spawner.decl", package, package);
    create(path, "{ class = \"Entity\"; edit = { spawnType = \"IMP\"; } }");
}
/* The key one scope's authored bit ended with, from the library's provenance. */
static uint32_t bound_key(const sh_package_library *library, const sh_package_sources *sources,
    const char *package, const char *original)
{
    size_t owner = SIZE_MAX;
    for (size_t i = 0; i < sources->package_count; i++) if (!strcmp(sources->packages[i].name, package)) owner = i;
    assert(owner != SIZE_MAX);
    const unsigned char *tree = sh_package_source_tree(sources, owner);
    for (size_t i = 0; i < library->bindings.binding_count; i++) {
        const sh_package_provenance_binding *b = library->bindings.bindings + i;
        if (strcmp(b->family, SH_PACKAGE_KEY_FAMILY) || strcmp(b->original, original)) continue;
        for (size_t c = 0; c < b->context_count; c++)
            if (!memcmp(library->bindings.contexts[b->contexts[c]].trees[0], tree, 32)) return (uint32_t)strtoul(b->name, NULL, 10);
    }
    return 0;
}
/* Past the single-bit keys: thirteen holders of 524288 take the nine single
 * bits and four multi-bit keys. A group allocated afterwards never takes one
 * of those keys; two new IMP rows of one package (both multi-bit) make its
 * IMP bit a group, recorded as that group in provenance; a recompile given
 * this library as peer cannot reuse that group code as either row's key; and
 * a package whose custom bit stays its own key still names it in its ALL. */
static void test_registration_group_capacity(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_package_library first = {0}, second = {0};
    sh_package_sources *sources = NULL, *more = NULL;
    sh_package_provenance peer = {0};
    char names[13][16], error[1024];
    registration_root("registration-group-capacity");
    for (int i = 0; i < 13; i++) { snprintf(names[i], sizeof(names[i]), "cap%02d", i); registration_package(names[i], "524288"); }
    group_package("pair"); named_pair_package("named");
    {
        /* The pair's first key is pinned to the first multi-bit code, so its
         * group is mixed and every later multi-bit code a holder takes is one
         * the group allocation must skip. */
        sh_package_sources *scan = sh_package_sources_scan(root, error, sizeof(error));
        assert(scan);
        group_pin(&peer, scan, "pair", "524288", SH_PACKAGE_KEY_MARKER | 524288u);
        sh_package_sources_free(scan);
    }
    group_compile(f, &peer, NULL, &sources, &first);
    const sh_compiled_resource *conductor = named_resource(first.compiled, "entitydef", "conductor");
    key_row rows[24];
    size_t count = key_rows(f, conductor, "aiTypeList", "entityDef", rows, 24);
    const char *out[24];
    uint32_t holders[13];
    for (int i = 0; i < 13; i++) holders[i] = (uint32_t)strtoul(spawner_key(first.compiled, names[i]), NULL, 10);
    /* The pair's group selects exactly its two AI, never a holder's. */
    uint32_t pair_one = bound_key(&first, sources, "pair", "524288"), pair_two = bound_key(&first, sources, "pair", "1048576");
    uint32_t group = (uint32_t)strtoul(spawner_key(first.compiled, "pair"), NULL, 10);
    assert(pair_one == (SH_PACKAGE_KEY_MARKER | 524288u) && pair_two == 1048576u);
    assert((group & SH_PACKAGE_KEY_MARKER) && first.key_groups && first.key_groups->defined[(group & SH_PACKAGE_KEY_SINGLE) >> 19]);
    for (int i = 0; i < 13; i++) assert(group != holders[i]);
    size_t n = selected(rows, count, group, out, 24);
    assert(n == 2);
    for (size_t i = 0; i < n; i++) {
        uint32_t mask = 0;
        for (size_t r = 0; r < count; r++) if (!strcmp(rows[r].field, out[i])) mask = rows[r].mask;
        assert(mask == pair_one || mask == pair_two);
    }
    /* The changed IMP row's key and the custom row's key: one group for the
     * IMP bit, recorded as that group in provenance. The package's IMP
     * request selects its two AI (its IMP replaced the stock one). */
    uint32_t named_bit = bound_key(&first, sources, "named", "4");
    sh_package_key_set members;
    sh_package_key_decode(first.key_groups, named_bit, &members);
    assert((named_bit & SH_PACKAGE_KEY_MARKER) && sh_package_key_set_count(&members) == 2 &&
        first.key_groups->defined[(named_bit & SH_PACKAGE_KEY_SINGLE) >> 19]);
    uint32_t imp_request = group_value(spawner_value(first.compiled, "named"));
    n = selected(rows, count, imp_request, out, 24);
    assert(n == 2 && !holds(out, n, "imp"));
    n = selected(rows, count, imp_request | 4u, out, 24);
    assert(n == 3 && holds(out, n, "imp"));
    /* A recompile with this peer keeps distinct keys for the two rows. */
    group_compile(f, &first.bindings, first.key_groups, &more, &second);
    uint32_t again = bound_key(&second, more, "named", "4");
    sh_package_key_set now;
    sh_package_key_decode(second.key_groups, again, &now);
    assert(sh_package_key_set_count(&now) == 2);
    for (size_t i = 0; i < SH_PACKAGE_KEY_CODES; i++) {
        uint32_t code = sh_package_key_code(i);
        if (!sh_package_key_set_has(&now, code)) continue;
        assert(!(code & SH_PACKAGE_KEY_MARKER) || !first.key_groups->defined[(code & SH_PACKAGE_KEY_SINGLE) >> 19]);
    }
    sh_package_library_free(&second); sh_package_sources_free(more);
    sh_package_library_free(&first); sh_package_sources_free(sources); sh_package_provenance_free(&peer);
    cleanup();
    /* A kept custom key: the scope's ALL names the stock rows and its AI. */
    registration_root("registration-group-all");
    registration_package("solo", "524288");
    {
        char path[512];
        snprintf(path, sizeof(path), "overrides/solo/assets/generated/decls/entitydef/solo_spawner.decl");
        create(path, "{ class = \"Entity\"; edit = { spawnType = \"ALL\"; } }");
    }
    group_compile(f, NULL, NULL, &sources, &first);
    conductor = named_resource(first.compiled, "entitydef", "conductor");
    count = key_rows(f, conductor, "aiTypeList", "entityDef", rows, 24);
    uint32_t all = group_value(spawner_key(first.compiled, "solo")) | 4u;
    assert(!(all & SH_PACKAGE_KEY_COMPOSITE) && (all & 524288u));
    n = selected(rows, count, all, out, 24);
    assert(n == count);
    sh_package_library_free(&first); sh_package_sources_free(sources);
    g_key_groups = NULL;
    cleanup(); assert(!failures);
    while (f->count) free(f->regions[--f->count].address);
}

/* A test encoder: an equal set keeps its code; a new set takes the next of
 * marker|bit27, marker|bit26, ... ; none when exhausted. */
typedef struct test_group_state { sh_package_key_groups *groups; int calls, exhausted; uint32_t last; } test_group_state;
static int test_group_code(void *context, const sh_package_key_set *keys, uint32_t *code)
{
    test_group_state *state = context;
    uint32_t found = sh_package_key_groups_find(state->groups, keys);
    if (found) { *code = found; return 1; }
    if (state->exhausted) return 0;
    state->calls++;
    state->last = SH_PACKAGE_KEY_MARKER | (1u << (28 - state->calls));
    if (!sh_package_key_groups_define(state->groups, state->last, keys)) return -1;
    *code = state->last; return 1;
}
static void test_registration_keys(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char error[1024];
    sh_decl_native_schema *schema = sh_decl_native_schema_open(
        (sh_decl_native_source){f, read_memory, (uintptr_t)f->reflection}, (sh_decl_dependency_observer){0},
        error, sizeof(error));
    sh_package_key_space space;
    uint32_t bits = 0;
    sh_package_key_moves to;
    memset(to, 0, sizeof(to));
    assert(schema && sh_package_key_space_read(schema, &space) == 1 && sh_package_key_space_private(&space));
    assert(space.named == (0x7ffffu | (1u << 30)) && space.pool == 0x3ff80000u);
    /* The engine reads a value as one number or as a list of names only. */
    assert(sh_package_key_custom_bits(&space, "524292", 6, &bits) && bits == 524288u);
    assert(sh_package_key_custom_bits(&space, "IMP 524288", 10, &bits) && !bits);
    assert(sh_package_key_custom_bits(&space, "1572868", 7, &bits) && bits == (524288u | 1048576u));
    assert(sh_package_key_custom_bits(&space, "ALL ANY", 7, &bits) && !bits);
    assert(sh_package_key_custom_bits(&space, "524288 1048576", 14, &bits) && bits == 524288u);
    assert(sh_package_key_custom_bits(&space, "-1", 2, &bits) && bits == ~space.named);
    /* Names resolve through the enum, composites included. */
    assert(sh_package_key_bits(&space, "IMP ANY_ZOMBIE 524288", 21, &bits) && bits == (4u | 131072u));
    assert(sh_package_key_bits(&space, "ALL", 3, &bits) && bits == 0xffffffffu);
    assert(sh_package_key_bits(&space, "UNKNOWN 8", 9, &bits) && !bits);
    assert(sh_package_key_bits(&space, "9999999999", 10, &bits) && bits == 0x7fffffffu);
    assert(sh_package_key_bits(&space, "IM", 2, &bits) && !bits);
    /* Only decimals and single-bit names are movable, never composites. */
    assert(sh_package_key_movable_bits(&space, "IMP ALL 524288", 14, &bits) && !bits);
    assert(sh_package_key_movable_bits(&space, "IMP T3", 6, &bits) && bits == 12u);
    assert(sh_package_key_movable_bits(&space, "524292", 6, &bits) && bits == 524292u);
    {
        /* A moved single-bit name makes the value one number of all its bits;
         * a composite name covering it keeps the list as written. */
        sh_package_key_moves named;
        size_t length = 0; int failed = 1;
        memset(named, 0, sizeof(named));
        sh_package_key_move_to(named + 2, 0, 1u << 25); sh_package_key_move_to(named + 19, 0, 1u << 20);
        char *moved = sh_package_key_relocate(&space, "IMP T3", 6, named, NULL, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "33554440") && length == strlen(moved));
        free(moved);
        assert(!sh_package_key_relocate(&space, "IMP T3 ALL", 10, named, NULL, &length, &failed) && !failed);
        assert(!sh_package_key_relocate(&space, "T3 UNKNOWN", 10, named, NULL, &length, &failed) && !failed);
        /* A number keeps what follows its first token (the engine ignores it). */
        moved = sh_package_key_relocate(&space, "524292 T3", 9, named, NULL, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "34603008 T3")); free(moved);
        /* A group constant gains a key beside its own bit. */
        sh_package_key_move_add(named + 17, 17, 0, 1u << 25);
        moved = sh_package_key_relocate(&space, "ANY_ZOMBIE", 10, named, NULL, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "33685504")); free(moved);
        moved = sh_package_key_relocate(&space, "IMP T3 ANY_ZOMBIE", 17, named, NULL, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "33685512")); free(moved);
        /* A whole marked key replaces its bits. */
        sh_package_key_moves whole;
        memset(whole, 0, sizeof(whole));
        sh_package_key_move_to(whole + 20, 0, 524288u); whole[28].kind = SH_PACKAGE_KEY_DROP;
        moved = sh_package_key_relocate(&space, "269484036", 9, whole, NULL, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "524292")); free(moved);
        /* Bit 31 is written as the negative number the engine reads. */
        sh_package_key_moves high;
        memset(high, 0, sizeof(high));
        sh_package_key_move_to(high + 30, 0, 0xc0000000u);
        moved = sh_package_key_relocate(&space, "ANY", 3, high, NULL, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "-1073741824")); free(moved);
        const char *inspector = "{ edit = { enumType = \"spawnAIType_t\"; values = { num = 2; "
            "item[0] = { enumValue = \"IMP\"; label = \"a\"; } item[1] = { enumValue = \"ALL\"; label = \"b\"; } } } }";
        moved = sh_package_key_relocate_inspector(&space, inspector, strlen(inspector), named, NULL, &length, &failed);
        assert(!failed && moved && strstr(moved, "\"33554432\"") && strstr(moved, "\"ALL\"") && !strstr(moved, "\"IMP\""));
        free(moved);
        /* Without the enum a name stays a name. */
        assert(!sh_package_key_relocate(NULL, "IMP", 3, named, NULL, &length, &failed) && !failed);
    }
    /* The native contract under the detours. Stock rows answer native
     * intersection; private rows answer only requests naming their key, never
     * stock types or the stock composites (bit 29); marked keys match exactly. */
    {
        const uint32_t imp = 4u, zombie = 131072u, all = 0xffffffffu, k19 = 524288u, k20 = 1048576u;
        const uint32_t m19 = SH_PACKAGE_KEY_MARKER | k19, m1920 = SH_PACKAGE_KEY_MARKER | k19 | k20;
        assert(sh_package_key_request_matches(NULL, imp, imp) && sh_package_key_request_matches(NULL, all, imp));
        assert(!sh_package_key_request_matches(NULL, imp, imp | k19) && !sh_package_key_request_matches(NULL, all, imp | k19));
        /* Stock composites name the widest marked key's bits yet never select it. */
        assert(!sh_package_key_request_matches(NULL, all, SH_PACKAGE_KEY_PRIVATE) &&
            !sh_package_key_request_matches(NULL, 0xbffdffffu, SH_PACKAGE_KEY_PRIVATE) &&
            sh_package_key_request_matches(NULL, SH_PACKAGE_KEY_PRIVATE, SH_PACKAGE_KEY_PRIVATE));
        assert(sh_package_key_request_matches(NULL, k19, imp | k19) && sh_package_key_request_matches(NULL, k19 | k20, k20));
        assert(sh_package_key_request_matches(NULL, imp | k19, imp) && sh_package_key_request_matches(NULL, imp | k19, imp | k19));
        assert(sh_package_key_request_matches(NULL, m19, m19) && !sh_package_key_request_matches(NULL, m19, k19));
        assert(!sh_package_key_request_matches(NULL, m1920, m19) && !sh_package_key_request_matches(NULL, k19, m19));
        assert(!sh_package_key_request_matches(NULL, zombie, zombie | k19) && sh_package_key_request_matches(NULL, zombie | k19, zombie | k19));
        assert(sh_package_key_modifier_matches(NULL, imp, imp) && !sh_package_key_modifier_matches(NULL, imp, k19));
        assert(sh_package_key_modifier_matches(NULL, imp | k19, k19) && sh_package_key_modifier_matches(NULL, imp | k19, imp));
        assert(!sh_package_key_modifier_matches(NULL, imp | k20, k19) && !sh_package_key_modifier_matches(NULL, m19, m1920));
        assert(sh_package_key_modifier_matches(NULL, m1920, m1920) && sh_package_key_modifier_matches(NULL, k19, imp | k19) &&
            !sh_package_key_modifier_matches(NULL, imp, imp | k19));
        assert(sh_package_key_filter_matches(NULL, imp, imp | k19) && sh_package_key_filter_matches(NULL, all, m19));
        assert(sh_package_key_filter_matches(NULL, k19, k19) && !sh_package_key_filter_matches(NULL, k19, imp));
        assert(!sh_package_key_filter_matches(NULL, m1920, m19) && sh_package_key_filter_matches(NULL, k19 | k20, k20));
        /* Keys: single bits 19-27, then marked keys, fewer bits first. */
        assert(sh_package_key_private(0) == k19 && sh_package_key_private(8) == (1u << 27));
        assert(sh_package_key_private(9) == m19 && sh_package_key_private(18) == (SH_PACKAGE_KEY_MARKER | (1u << 19) | (1u << 20)));
        assert(sh_package_key_private(9 + 511) == 0 && sh_package_key_private_valid(m1920) &&
            !sh_package_key_private_valid(SH_PACKAGE_KEY_MARKER) && !sh_package_key_private_valid(k19 | k20));
        /* Code indices cover each private code once, singles first; the
         * allocation order visits every one of them. */
        for (size_t i = 0; i < SH_PACKAGE_KEY_CODES; i++) {
            sh_package_key_set only = {{0}};
            assert(sh_package_key_index(sh_package_key_code(i)) == i && (i >= 9) == !!(sh_package_key_code(i) & SH_PACKAGE_KEY_MARKER));
            assert(sh_package_key_index(sh_package_key_private(i)) != SIZE_MAX);
            assert(sh_package_key_set_add(&only, sh_package_key_private(i)) && sh_package_key_set_count(&only) == 1);
        }
        assert(sh_package_key_index(k19 | k20) == SIZE_MAX && sh_package_key_index(SH_PACKAGE_KEY_MARKER) == SIZE_MAX &&
            sh_package_key_index(imp) == SIZE_MAX && !sh_package_key_code(SH_PACKAGE_KEY_CODES));
    }
    /* Groups: two multi-bit keys OR to a third code. A = marker|19|20 and
     * B = marker|19|21 give 272105472 = C, a key of its own; a request, filter
     * or modifier meant for A and B carries a group code G defined as {A, B}
     * and meets exactly those two. Without the group table G names itself. */
    {
        const uint32_t imp = 4u, k19 = 524288u, a = 270008320u, b = 271056896u, c = 272105472u;
        const uint32_t g = SH_PACKAGE_KEY_MARKER | (1u << 22), h = SH_PACKAGE_KEY_MARKER | (1u << 23);
        sh_package_key_groups groups;
        sh_package_key_set ab = {{0}}, mixed = {{0}}, one = {{0}};
        memset(&groups, 0, sizeof(groups));
        assert((a | b) == c && sh_package_key_set_add(&ab, a) && sh_package_key_set_add(&ab, b));
        assert(sh_package_key_set_add(&mixed, k19) && sh_package_key_set_add(&mixed, a));
        assert(sh_package_key_set_add(&one, a) && !sh_package_key_groups_define(&groups, g, &one));
        assert(sh_package_key_groups_define(&groups, g, &ab) && !sh_package_key_groups_define(&groups, g, &mixed));
        assert(sh_package_key_groups_define(&groups, h, &mixed) && !sh_package_key_groups_define(&groups, k19, &ab));
        assert(sh_package_key_groups_find(&groups, &ab) == g && sh_package_key_groups_find(&groups, &mixed) == h &&
            !sh_package_key_groups_find(&groups, &one) && groups.count == 2);
        assert(sh_package_key_request_matches(&groups, g, a) && sh_package_key_request_matches(&groups, g, b) &&
            !sh_package_key_request_matches(&groups, g, c) && !sh_package_key_request_matches(&groups, c, a));
        assert(!sh_package_key_request_matches(NULL, g, a) && !sh_package_key_request_matches(&groups, g, imp | k19));
        assert(sh_package_key_request_matches(&groups, imp | g, imp) && sh_package_key_request_matches(&groups, imp | g, imp | a));
        assert(sh_package_key_request_matches(&groups, h, k19) && sh_package_key_request_matches(&groups, h, imp | a) &&
            !sh_package_key_request_matches(&groups, h, b) && !sh_package_key_request_matches(&groups, h, c));
        assert(sh_package_key_modifier_matches(&groups, a, g) && sh_package_key_modifier_matches(&groups, imp | b, g) &&
            !sh_package_key_modifier_matches(&groups, c, g) && !sh_package_key_modifier_matches(&groups, imp, g));
        assert(sh_package_key_filter_matches(&groups, g, a) && sh_package_key_filter_matches(&groups, g, b) &&
            !sh_package_key_filter_matches(&groups, g, c) && sh_package_key_filter_matches(&groups, imp | g, imp));
        /* A row registered under a group code (a row naming two keys) answers
         * requests for either key. */
        assert(sh_package_key_request_matches(&groups, a, g) && sh_package_key_request_matches(&groups, b, imp | g) &&
            !sh_package_key_request_matches(&groups, c, g));
    }
    /* Relocation under the contract spells a set of keys as one code: several
     * single-bit keys stay their union, one multi-bit key is itself, and a set
     * holding a multi-bit key takes a group code from the encoder. A package's
     * ALL covers the stock bits it keeps and exactly its own keys. */
    {
        sh_package_key_moves moves;
        sh_package_key_groups groups;
        size_t length = 0; int failed = 1;
        char *moved;
        memset(moves, 0, sizeof(moves)); memset(&groups, 0, sizeof(groups));
        sh_package_key_move_to(moves + 19, 1, 270008320u); sh_package_key_move_to(moves + 20, 1, 271056896u);
        sh_package_key_move_to(moves + 21, 1, 1u << 22);
        test_group_state state = {&groups, 0, 0, 0};
        sh_package_key_encoder encoder = {test_group_code, &state};
        moved = sh_package_key_relocate(&space, "1572864", 7, moves, &encoder, &length, &failed);
        assert(!failed && moved && strtoul(moved, NULL, 10) == state.last && state.calls == 1); free(moved);
        /* The same set reuses its code. */
        moved = sh_package_key_relocate(&space, "1572868", 7, moves, &encoder, &length, &failed);
        assert(!failed && moved && strtoul(moved, NULL, 10) == (state.last | 4u) && state.calls == 1); free(moved);
        moved = sh_package_key_relocate(&space, "524288", 6, moves, &encoder, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "270008320")); free(moved);
        moved = sh_package_key_relocate(&space, "2097152", 7, moves, &encoder, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "4194304")); free(moved);
        /* A single-bit key beside a multi-bit one is a mixed group. */
        moved = sh_package_key_relocate(&space, "2621440", 7, moves, &encoder, &length, &failed);
        assert(!failed && moved && state.calls == 2); free(moved);
        /* ALL keeps the stock bits and names this scope's keys only. */
        moved = sh_package_key_relocate(&space, "ALL", 3, moves, &encoder, &length, &failed);
        uint32_t all = moved ? (uint32_t)strtoul(moved, NULL, 10) : 0;
        sh_package_key_set seen = {{0}};
        sh_package_key_decode(&groups, all, &seen);
        assert(!failed && moved && (all & 0x4007ffffu) == 0x4007ffffu && !(all & SH_PACKAGE_KEY_COMPOSITE) &&
            sh_package_key_set_count(&seen) == 3 && sh_package_key_set_has(&seen, 270008320u) &&
            sh_package_key_set_has(&seen, 271056896u) && sh_package_key_set_has(&seen, 1u << 22)); free(moved);
        /* Without moves a composite is untouched: a stock or unkeyed ALL keeps
         * its native meaning. */
        sh_package_key_moves none;
        memset(none, 0, sizeof(none));
        assert(!sh_package_key_relocate(&space, "ALL", 3, none, &encoder, &length, &failed) && !failed);
        /* An exhausted allocator is reported, never spelled as an OR. */
        state.exhausted = 1;
        sh_package_key_move_to(moves + 22, 1, 272105472u);
        assert(!sh_package_key_relocate(&space, "4718592 5", 9, moves, &encoder, &length, &failed) && failed == 2);
    }
    sh_decl_native_schema_close(schema);
    sh_package_key_move_to(to + 19, 0, 1u << 25);
    {
        size_t length = 0; int failed = 1;
        char *moved = sh_package_key_relocate(NULL, "524288 1048576", 14, to, NULL, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "33554432 1048576") && length == 16); free(moved);
        moved = sh_package_key_relocate(NULL, "1572868", 7, to, NULL, &length, &failed);
        assert(!failed && moved && !strcmp(moved, "34603012")); free(moved);
        assert(!sh_package_key_relocate(NULL, "IMP 1048576", 11, to, NULL, &length, &failed) && !failed);
        const char *inspector = "{ edit = { enumType = \"spawnAIType_t\"; values = { num = 2; "
            "item[0] = { enumValue = \"IMP\"; label = \"a\"; } item[1] = { enumValue = \"524292\"; label = \"b\"; } } } }";
        moved = sh_package_key_relocate_inspector(NULL, inspector, strlen(inspector), to, NULL, &length, &failed);
        assert(!failed && moved && strstr(moved, "\"33554436\"") && strstr(moved, "\"IMP\"") && !strstr(moved, "524292"));
        free(moved);
        const char *other = "{ edit = { enumType = \"teamFilter_t\"; values = { num = 1; "
            "item[0] = { enumValue = \"524288\"; label = \"a\"; } } } }";
        assert(!sh_package_key_relocate_inspector(NULL, other, strlen(other), to, NULL, &length, &failed) && !failed);
    }
    sh_compiled_resource file = link_decl("conductor", "generated/decls/entitydef/conductor.decl",
        "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } "
        "item[1] = { aiType = \"524292\"; entityDef = \"boss\"; } } } }");
    sh_package_compilation compiled = {0}; compiled.resources = &file; compiled.resource_count = 1;
    sh_package_source_reader *reader = sh_package_source_reader_open(&compiled, NULL, registration_original, f,
        registry, (uintptr_t)f->reflection, error, sizeof(error));
    assert(reader);
    size_t length = 0; sh_package_source_binding result = {0};
    unsigned char *out = NULL;
    sh_package_key_space read_space;
    assert(sh_package_source_key_space(reader, &read_space) == 1 && read_space.named == space.named);
    int bound = sh_package_source_bind_keys(reader, "entitydef", "conductor", file.body, file.body_length,
        key_rewrite, to, &out, &length, &result, error, sizeof(error));
    if (bound != 1) fprintf(stderr, "registration key sites: %s\n", error);
    assert(bound == 1 && out && result.references == 2 && result.changed == 1);
    assert(strstr((char *)out, "aiType = \"IMP\";") && strstr((char *)out, "aiType = \"33554436\";") &&
        !strstr((char *)out, "524292") && strstr((char *)out, "entityDef = \"boss\";"));
    free(out); out = NULL;
    /* A family with no reflected state reader has no key site. */
    assert(sh_package_source_bind_keys(reader, "image", "conductor", file.body, file.body_length,
        key_rewrite, to, &out, &length, &result, error, sizeof(error)) == 0 && !out && !*error);
    sh_package_source_reader_close(reader);
    while (f->count) free(f->regions[--f->count].address);
}

static void test_material_path(fixture *f, sh_decl_registry_source registry)
{
    char error[1024]; sh_package_compilation compiled = {0};
    sh_compiled_resource files[2];
    const char *texts[] = {"{}", "{ landPageFile \"virtualtextures/first.pages\"\n}",
        "{ landPageFile first\nlandPageFile \"second\"\n}",
        "{ landPageFile \"\"\n}", "{ landPageFile first second\n}"};
    const char *expected[] = {"", "virtualtextures/first.pages", "second", "", NULL};
    files[0] = link_decl("sample", "generated/decls/material/sample.decl", "{}");
    files[0].type = "material";
    compiled.resources = files; compiled.resource_count = 1;
    for (size_t i = 0; i < 5; i++) {
        files[0].body = (unsigned char *)texts[i]; files[0].body_length = strlen(texts[i]);
        sh_package_source_reader *reader = sh_package_source_reader_open(&compiled, NULL, original, f,
            registry, (uintptr_t)f->reflection, error, sizeof(error)); assert(reader);
        char *path = sh_package_source_material_path(reader, "sample", "landPageFile", error, sizeof(error));
        if (expected[i]) assert(path && !strcmp(path, expected[i]));
        else assert(!path && *error);
        free(path); sh_package_source_reader_close(reader);
    }
    files[0].body = (unsigned char *)"{}"; files[0].body_length = 2;
    files[1] = link_decl("landpagefile", "generated/decls/renderparm/landpagefile.decl", "{ String \"scoped.pages\" }");
    files[1].type = "renderparm"; compiled.resource_count = 2;
    sh_package_source_reader *reader = sh_package_source_reader_open(&compiled, NULL, original, f,
        registry, (uintptr_t)f->reflection, error, sizeof(error)); assert(reader);
    char *path = sh_package_source_material_path(reader, "sample", "landPageFile", error, sizeof(error));
    assert(path && !strcmp(path, "scoped.pages")); free(path); sh_package_source_reader_close(reader);
    compiled.resource_count = 1; f->unreadable = "generated/decls/renderparm/landpagefile.decl";
    reader = sh_package_source_reader_open(&compiled, NULL, original, f,
        registry, (uintptr_t)f->reflection, error, sizeof(error)); assert(reader);
    assert(!sh_package_source_material_path(reader, "sample", "landPageFile", error, sizeof(error)));
    sh_package_source_reader_close(reader); f->unreadable = NULL;
}

typedef struct library_atlas_fixture {
    source_link_fixture originals;
    size_t calls;
    size_t page_reads, parameter_reads;
    int fail, collision;
    sh_package_native_assets *installed;
} library_atlas_fixture;
static int library_atlas_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    library_atlas_fixture *f = context;
    if (f->installed && !strncmp(path, "virtualtextures/", 16))
        return sh_package_native_assets_read(f->installed, path, body, length);
    if (!strncmp(path, "virtualtextures/", 16)) f->page_reads++;
    if (!strcmp(path, "generated/decls/renderparm/landpagefile.decl")) {
        f->parameter_reads++;
        /* A second underlying read is deliberately unavailable. Both passes,
         * material inspection, and the source linker must share one snapshot. */
        if (f->parameter_reads > 1) return -1;
    }
    if (!strcmp(path, "virtualtextures/default.vmtr") || !strcmp(path, "virtualtextures/other.vmtr")) {
        const char *text = "26\n1\n8 4 16 16 0 1 2 \"sample\"\n";
        *body = (unsigned char *)_strdup(text); *length = strlen(text); return *body ? 1 : -1;
    }
    return link_original(&f->originals, path, body, length);
}
static int library_atlas_semantics(void *context, size_t scope, const char *family,
    const char *name, unsigned char semantics[32], char *error, size_t capacity)
{
    library_atlas_fixture *f = context;
    return link_original_binding(&f->originals, scope, family, name, semantics, error, capacity);
}
static int library_atlas(void *context, size_t scope, const sh_package_compilation *compiled,
    const char *material, const char *page_file, sh_package_virtual_atlas *out, char *error, size_t capacity)
{
    library_atlas_fixture *f = context;
    (void)scope; assert(compiled && !strcmp(material, "sample")); f->calls++;
    if (f->fail) { snprintf(error, capacity, "synthetic page source read failed"); return -1; }
    assert(!*page_file || !strcmp(page_file, "other"));
    out->name = *page_file && !f->collision ? page_file : "default";
    out->source = *page_file ? "virtualtextures/other.pages" : "virtualtextures/default.pages";
    out->content[0] = *page_file ? 2 : 1; out->original = 1; return 1;
}
static void test_library_atlas(fixture *f, sh_decl_registry_source registry)
{
    const char *folders[] = {"red", "green", "duplicate"};
    char temporary[MAX_PATH], relative[256], error[1024];
    library_atlas_fixture originals = {{f}}; originals.originals.certificate = 1;
    sh_package_library_native native = {{&originals, NULL, library_atlas_semantics}, library_atlas};
    sh_package_compile_environment env = {0}; sh_package_library library = {0};
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-library-atlas-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, "{\"id\":\"same.id\",\"name\":\"Material\"}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/material/sample.decl", folders[i]);
        create(relative, i == 1 ? "{ color {1,0,0,1}\nlandPageFile other\n}" : "{ color {1,0,0,1}\n}");
    }
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    env.baseline = library_atlas_original; env.baseline_context = &originals;
    int ok = sh_package_library_compile(sources, &env, NULL, library_atlas_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "library atlas: %s\n", error);
    assert(ok && library.virtual_materials && library.virtual_materials->count == 2);
    assert(originals.calls == 3); /* Native scope binding is captured once before composition. */
    assert(originals.page_reads == 12 && originals.parameter_reads == 1);
    assert(library.bindings.binding_count == 2);
    size_t duplicates = 0, defaults = 0, others = 0;
    for (size_t i = 0; i < library.bindings.binding_count; i++) {
        const sh_package_provenance_binding *binding = library.bindings.bindings + i;
        assert(!strcmp(binding->family, "material"));
        const vmr_entry *row = vmr_find(library.virtual_materials, binding->name);
        assert(row && row->record.x == 8);
        defaults += !strcmp(row->atlas, "default"); others += !strcmp(row->atlas, "other");
        /* Entire identical source bundles coalesce contexts as well as data. */
        duplicates += binding->context_count == 1;
    }
    assert(defaults == 1 && others == 1 && duplicates == 2);
    for (size_t i = 0; i < sources->file_count; i++) if (!sources->files[i].directory)
        assert(sh_package_source_verify(sources->files + i));
    originals.collision = 1;
    originals.parameter_reads = 0;
    assert(!sh_package_library_compile(sources, &env, NULL, library_atlas_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error)));
    assert(strstr(error, "different page data") && !library.compiled && !library.virtual_materials);
    originals.collision = 0; originals.fail = 1;
    originals.parameter_reads = 0;
    assert(!sh_package_library_compile(sources, &env, NULL, library_atlas_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error)));
    assert(strstr(error, "page source read failed") && !library.compiled && !library.virtual_materials);
    sh_package_library_free(&library); sh_package_sources_free(sources);

    /* Use the concrete production adapter, including real installed companion
     * capture and its renderer ABI certificate, through the whole library. */
    for (size_t i = 0; i < 3; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/material/sample.decl", folders[i]);
        create(relative, i == 1 ? "{ color {0,1,0,1}\n}" : "{ color {1,0,0,1}\n}");
    }
    create("virtualtextures/_vmtr.vmtr", "26\n1\n8 4 16 16 0 1 2 \"sample\"\n");
    create("virtualtextures/_vmtr_sq1.mega2", "synthetic original page data");
    originals.installed = sh_package_native_assets_open(root); assert(originals.installed);
    native = sh_package_native_assets_adapter(originals.installed);
    originals.parameter_reads = 0;
    sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    ok = sh_package_library_compile(sources, &env, NULL, library_atlas_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "installed atlas library: %s\n", error);
    assert(ok && library.virtual_materials && library.virtual_materials->count == 2);
    assert(library.bindings.binding_count == 2 && originals.parameter_reads == 1);
    for (size_t i = 0; i < library.bindings.binding_count; i++) {
        const vmr_entry *row = vmr_find(library.virtual_materials, library.bindings.bindings[i].name);
        assert(row && row->record.x == 8 && !strcmp(row->atlas, "_vmtr"));
    }
    sh_package_library_free(&library); sh_package_sources_free(sources);
    sh_package_native_assets_close(originals.installed); cleanup(); assert(!failures);
}

/* Localization uses a separate schema projection. Its fixture includes native
 * string values, ordinary strings, inherited fields, collections and a typed
 * polymorphic member; no field is selected by its name or string contents. */
static void setup_strings(fixture *f)
{
    static const char *extra[] = {"idStrId", "idStr", "idAtomicString", "LocalizedBase",
        "LocalizedEntity", "LocalizedNested", "idList < idStrId >",
        "idTypeInfoObjectPtr < LocalizedNested >", "MoreLocalizedNested"};
    void *container, *old_records, *old_objects, *old_pointers, *records, *objects, *pointers;
    setup(f);
    memcpy(&container, f->reflection, 8); memcpy(&old_records, (char *)container + 0x20, 8);
    memcpy(&old_objects, (char *)f->reflection + 0x88, 8);
    memcpy(&old_pointers, (char *)f->reflection + 0xa0, 8);
    records = alloc(f, 20 * 0x38); objects = alloc(f, 20 * 16); pointers = alloc(f, 20 * 16);
    memcpy(records, old_records, 10 * 0x38); memcpy(objects, old_objects, 10 * 16);
    memcpy(pointers, old_pointers, 10 * 16);
    pointer(container, 0x20, records); pointer(f->reflection, 0x88, objects);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0x90, 20); number(f->reflection, 0xa8, 20);
    for (size_t i = 0; i < sizeof(extra) / sizeof(*extra); i++) {
        size_t index = i + 10;
        const char *parent = index == 13 ? "Entity" : index == 14 ? "LocalizedBase" :
            index == 18 ? "LocalizedNested" : "";
        pointer(records, index * 0x38, string(f, extra[i]));
        pointer(records, index * 0x38 + 8, string(f, parent));
    }
    pointer(objects, 10 * 16 + 8, (void *)0x70); /* idStrId reader identity. */
    pointer(objects, 11 * 16 + 8, (void *)0x80); pointer(objects, 12 * 16 + 8, (void *)0x90);
    pointer(objects, 16 * 16 + 8, (void *)0xa0); pointer(objects, 17 * 16 + 8, (void *)0xb0);
    for (size_t i = 16; i <= 17; i++) {
        void *element = alloc(f, 16);
        pointer(element, 0, string(f, i == 16 ? "idStrId" : "LocalizedNested"));
        pointer(element, 8, string(f, "")); pointer(records, i * 0x38 + 0x18, element);
    }
    {
        const char *t[] = {"idStrId", "idStr"}, *o[] = {"", ""};
        const char *n[] = {"displayNameTag", "prose"};
        fields(f, (char *)records + 13 * 0x38, 2, t, o, n);
    }
    {
        const char *t[] = {"idStrId", "idStrId", "idList < idStrId >", "LocalizedNested",
            "idTypeInfoObjectPtr < LocalizedNested >", "idAtomicString", "idStrId"};
        const char *o[] = {"", "[2]", "", "", "", "", "*"};
        const char *n[] = {"displayDescriptionTag", "labels", "localeList", "nested", "poly", "atomic", "pointer"};
        fields(f, (char *)records + 14 * 0x38, 7, t, o, n);
    }
    {
        const char *t[] = {"idStrId"}, *o[] = {""}, *theme[] = {"theme"}, *extra_name[] = {"extra"};
        fields(f, (char *)records + 15 * 0x38, 1, t, o, theme);
        fields(f, (char *)records + 18 * 0x38, 1, t, o, extra_name);
    }
    {
        const char *t[] = {"idStrId", "idDeclEntityDef"}, *o[] = {"", "*"};
        const char *n[] = {"displayNameTag", "target"};
        fields(f, (char *)records + 6 * 0x38, 2, t, o, n);
        fields(f, (char *)records + 4 * 0x38, 2, t, o, n);
        pointer(records, 4 * 0x38 + 8, string(f, "idDeclTypeInfoGraph"));
        pointer(objects, 4 * 16 + 8, (void *)0x30);
    }
}
typedef struct string_sites {
    char slots[16][128], keys[16][128];
    size_t lengths[16], count;
    int replace, fail;
} string_sites;
static int string_site(void *context, const char *slot, const char *key, size_t length, const char **replacement)
{
    string_sites *sites = context;
    assert(sites->count < 16 && length < 128 && strlen(slot) < 128);
    strcpy(sites->slots[sites->count], slot);
    memcpy(sites->keys[sites->count], key, length); sites->keys[sites->count][length] = 0;
    sites->lengths[sites->count++] = length;
    if (sites->fail) return -1;
    if (!sites->replace) return 0;
    *replacement = "#str_private"; return 1;
}
static int string_seen(const string_sites *sites, const char *slot, const char *key)
{
    for (size_t i = 0; i < sites->count; i++)
        if (!strcmp(sites->slots[i], slot) && sites->lengths[i] == strlen(key) && !strcmp(sites->keys[i], key)) return 1;
    return 0;
}
static void test_strings(void)
{
    fixture f; char error[1024]; size_t length;
    sh_package_compilation compiled = {0};
    sh_package_source_reader *reader;
    sh_package_source_binding result;
    string_sites sites = {0};
    bind_fixture resources = {0};
    unsigned char *out;
    const char *body = "{ class = \"LocalizedEntity\"; inherit = \"base\";"
        " editorVars = { displayNameTag = \"#str_metadata\"; } edit = {"
        " displayNameTag = \"#str_same\"; displayDescriptionTag = \"#str_same\";"
        " prose = \"#str_same\xfe\"; atomic = \"#str_same\"; pointer = \"#str_same\";"
        " labels = { labels[0] = \"#str_array\"; labels[1] = \"\"; }"
        " localeList = { num = 2; item[0] = \"#str_list\"; item[1] = \"#str_other\"; }"
        " nested = { theme = \"#str_\xff\"; }"
        " poly = { className = \"MoreLocalizedNested\"; object = { theme = \"#str_poly\"; extra = \"#str_extra\"; } }"
        " target = \"map-child\"; } }";
    const char *editor = "{ edit = { displayNameTag = \"#str_editor\"; target = \"map-child\"; } }";
    const char *graph = "{ inherit = \"mod\"; edit = { object = { displayNameTag = \"#str_graph\"; target = \"map-child\"; } } }";
    setup_strings(&f);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    reader = sh_package_source_reader_open(&compiled, NULL, original, &f, registry,
        (uintptr_t)f.reflection, error, sizeof(error)); assert(reader);
    out = sh_package_source_bind_strings(reader, "entitydef", "localized", (const unsigned char *)body,
        strlen(body), string_site, &sites, &length, &result, error, sizeof(error));
    if (!out) fprintf(stderr, "%s\n", error);
    assert(out && length == strlen(body) && !memcmp(out, body, length)); free(out);
    assert(sites.count == 8 && result.references == 8 && !result.changed && !result.inspection.gaps);
    assert(string_seen(&sites, "edit.displayNameTag", "#str_same"));
    assert(string_seen(&sites, "edit.displayDescriptionTag", "#str_same"));
    assert(string_seen(&sites, "edit.nested.theme", "#str_\xff"));
    assert(string_seen(&sites, "edit.poly.object.extra", "#str_extra"));
    assert(string_seen(&sites, "edit.poly.object.theme", "#str_poly"));
    sites = (string_sites){0}; sites.replace = 1;
    out = sh_package_source_bind_strings(reader, "entitydef", "localized", (const unsigned char *)body,
        strlen(body), string_site, &sites, &length, &result, error, sizeof(error));
    assert(out && result.changed == 8 && result.references == 8 && strstr((char *)out, "#str_private"));
    assert(strstr((char *)out, "#str_same\xfe") && strstr((char *)out, "#str_metadata"));
    assert(strstr((char *)out, "map-child") && strstr((char *)out, "atomic = \"#str_same\""));
    assert(strstr((char *)out, "pointer = \"#str_same\"")); free(out);
    /* The ordinary resource binder still sees exactly inheritance and target.
     * Neither string pass edits its native schema or resource callback route. */
    out = sh_package_source_bind(reader, "entitydef", "localized", (const unsigned char *)body,
        strlen(body), bind_reference, &resources, &length, &result, error, sizeof(error));
    assert(out && resources.count == 2 && result.references == 2 && !result.changed);
    assert(length == strlen(body) && !memcmp(out, body, length)); free(out);
    sites = (string_sites){0}; sites.replace = 1;
    out = sh_package_source_bind_strings(reader, "snapeditorentitydef", "localized", (const unsigned char *)editor,
        strlen(editor), string_site, &sites, &length, &result, error, sizeof(error));
    assert(out && sites.count == 1 && result.changed == 1 && strstr((char *)out, "map-child")); free(out);
    {
        const char *ordered = "{ \"LocalizedEntity\" \"\" edit = {"
            " \"idStrId\" \"\" displayNameTag = \"#str_first\";"
            " displayNameTag = \"#str_second\"; target = \"map-child\"; } }";
        sites = (string_sites){0};
        out = sh_package_source_bind_strings(reader, "snapeditorentitydef", "ordered",
            (const unsigned char *)ordered, strlen(ordered), string_site, &sites,
            &length, &result, error, sizeof(error));
        assert(out && length == strlen(ordered) && !memcmp(out, ordered, length)); free(out);
        assert(sites.count == 2 && result.references == 2 && !result.changed);
        assert(!strcmp(sites.keys[0], "#str_first") && !strcmp(sites.keys[1], "#str_second"));
        sites = (string_sites){0}; sites.replace = 1;
        out = sh_package_source_bind_strings(reader, "snapeditorentitydef", "ordered",
            (const unsigned char *)ordered, strlen(ordered), string_site, &sites,
            &length, &result, error, sizeof(error));
        assert(out && result.changed == 2 && !strstr((char *)out, "#str_first") &&
            !strstr((char *)out, "#str_second") && strstr((char *)out, "target = \"map-child\""));
        assert(strstr((char *)out, "\"idStrId\" \"\" displayNameTag")); free(out);
    }
    sites = (string_sites){0}; sites.replace = 1;
    out = sh_package_source_bind_strings(reader, "actormodifier", "localized", (const unsigned char *)graph,
        strlen(graph), string_site, &sites, &length, &result, error, sizeof(error));
    if (!out) fprintf(stderr, "%s\n", error);
    assert(out && sites.count == 1 && result.changed == 1);
    assert(string_seen(&sites, "edit.object.displayNameTag", "#str_graph"));
    assert(strstr((char *)out, "map-child") && strstr((char *)out, "inherit = \"mod\"")); free(out);
    sites = (string_sites){0}; sites.fail = 1;
    out = sh_package_source_bind_strings(reader, "entitydef", "localized", (const unsigned char *)body,
        strlen(body), string_site, &sites, &length, &result, error, sizeof(error));
    assert(!out && !length && !result.changed && result.inspection.gaps);
    {
        const char *opaque = "{ class = \"LocalizedEntity\"; edit = { custom = \"#str_unknown\"; } }";
        const char *empty = "{ class = \"LocalizedEntity\"; edit = { displayNameTag = \"\"; displayDescriptionTag = NULL; } }";
        sites = (string_sites){0};
        out = sh_package_source_bind_strings(reader, "entitydef", "opaque", (const unsigned char *)opaque,
            strlen(opaque), string_site, &sites, &length, &result, error, sizeof(error));
        assert(!out && !length && !result.changed && result.inspection.gaps && !sites.count);
        out = sh_package_source_bind_strings(reader, "entitydef", "empty", (const unsigned char *)empty,
            strlen(empty), string_site, &sites, &length, &result, error, sizeof(error));
        assert(out && !sites.count && !result.references && length == strlen(empty) && !memcmp(out, empty, length)); free(out);
        out = sh_package_source_bind_strings(reader, "material", "opaque", (const unsigned char *)"{ }", 3,
            string_site, &sites, &length, &result, error, sizeof(error));
        assert(!out && !length && !result.changed && strstr(error, "registered reflected reader"));
    }
    assert(!f.object_state_reads); sh_package_source_reader_close(reader);
    while (f.count) free(f.regions[--f.count].address);
}

static int resource_order(const void *a, const void *b)
{ return strcmp(((const sh_compiled_resource *)a)->engine_path, ((const sh_compiled_resource *)b)->engine_path); }
/* Cooked UI movie fixture: dictionary with one external material, one local
 * atlas record and one font. No shapes, sprites or script bytes are needed. */
typedef struct movie_bytes { unsigned char data[512]; size_t used; } movie_bytes;
static void mv_put(movie_bytes *m, const void *p, size_t n)
{ assert(n <= sizeof(m->data) - m->used); memcpy(m->data + m->used, p, n); m->used += n; }
static void mv_zero(movie_bytes *m, size_t n)
{ assert(n <= sizeof(m->data) - m->used); memset(m->data + m->used, 0, n); m->used += n; }
static void mv_word(movie_bytes *m, unsigned n)
{ unsigned char p[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n}; mv_put(m, p, 4); }
static void mv_text(movie_bytes *m, const char *s)
{ unsigned n = (unsigned)strlen(s); unsigned char p[4] = {(unsigned char)n, (unsigned char)(n >> 8), 0, 0};
  mv_put(m, p, 4); mv_put(m, s, n); }
static void mv_build(movie_bytes *m, const char *material, const char *font)
{
    m->used = 0; mv_word(m, 0x42535717); mv_zero(m, 22);
    mv_zero(m, 2); mv_word(m, 0); mv_word(m, 0); mv_word(m, 0); mv_word(m, 0);
    mv_word(m, 3);
    mv_word(m, 1); mv_text(m, material); mv_zero(m, 32);
    mv_word(m, 1); mv_text(m, ".atlas"); mv_zero(m, 32);
    mv_word(m, 5); mv_text(m, font); mv_zero(m, 6); mv_word(m, 0);
}
typedef struct movie_source_fixture { int stock, fail; movie_bytes stock_movie; } movie_source_fixture;
static int movie_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    movie_source_fixture *f = context; const char *text = NULL; const unsigned char *bytes = NULL;
    static const unsigned char glyphs[14] = {'i','d','f','+',0,12,0,0,0,12,0,12,0,0};
    static const unsigned char atlas[] = "stock atlas pixels";
    size_t n = 0;
    *body = NULL; *length = 0;
    if (f->fail) return -1;
    if (!strcmp(path, "generated/decls/renderparm/color.decl")) text = "{ Vec 0 }";
    else if (!strcmp(path, "generated/decls/renderparm/landpagefile.decl")) text = "{ String }";
    else if (f->stock && !strcmp(path, "generated/swf/stock/panel.bswf")) { bytes = f->stock_movie.data; n = f->stock_movie.used; }
    else if (f->stock && !strcmp(path, "generated/swf/stock/panel.bimage")) { bytes = atlas; n = sizeof(atlas) - 1; }
    else if (f->stock && !strcmp(path, "fonts/title/64_df.dat")) { bytes = glyphs; n = sizeof(glyphs); }
    else if (f->stock && !strcmp(path, "generated/decls/material/fonts/title/64_df.tga.decl")) text = "{ color {1,0,0} }";
    else if (f->stock && !strcmp(path, "generated/decls/material/gui/paint.decl")) text = "{ color {0,0,1} }";
    else if (f->stock && !strcmp(path, "generated/decls/material/guisolid.decl")) text = "{ color {1,1,1} }";
    else return 0;
    if (text) { bytes = (const unsigned char *)text; n = strlen(text); }
    *body = malloc(n + 1); assert(*body);
    memcpy(*body, bytes, n); (*body)[n] = 0; *length = n; return 1;
}
static int movie_semantics(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    (void)context; (void)scope; (void)name; (void)semantics; (void)error; (void)capacity;
    /* Synthetic native state: every family here keeps its name when its exact
     * original bytes and dependencies are retained. */
    return 1;
}
static const sh_package_link_unit *movie_unit(const sh_package_link_inputs *inputs, size_t scope,
    const char *family, const char *name, size_t *index)
{
    *index = sh_package_link_inputs_find(inputs, scope, family, name);
    return *index < inputs->count ? inputs->units + *index : NULL;
}
/* Extend a private reflection fixture: Entity gains an idSWF* field whose
 * registered pointer reader is distinct from every resource reader. */
static void movie_reflection(fixture *native)
{
    {
        void *container = *(void **)native->reflection;
        void *records = alloc(native, 21 * 0x38), *objects = alloc(native, 21 * 16), *pointers = alloc(native, 21 * 16);
        memcpy(records, *(void **)((char *)container + 0x20), 20 * 0x38);
        memcpy(objects, *(void **)((char *)native->reflection + 0x88), 20 * 16);
        memcpy(pointers, *(void **)((char *)native->reflection + 0xa0), 20 * 16);
        pointer(container, 0x20, records);
        pointer(native->reflection, 0x88, objects); number(native->reflection, 0x90, 21);
        pointer(native->reflection, 0xa0, pointers); number(native->reflection, 0xa8, 21);
        pointer(records, 19 * 0x38, string(native, "idSWF")); pointer(records, 19 * 0x38 + 8, string(native, ""));
        pointer(pointers, 19 * 16 + 8, (void *)0xa0);
        const char *t[] = {"idDeclEntityDef", "OpaqueStringReader", "idDeclActorModifier", "idDeclSnapEditorEntity", "idRenderModel", "idImage", "idSWF"};
        const char *o[] = {"*", "", "*", "*", "*", "*", "*"}, *n[] = {"target", "custom", "modifier", "preview", "model", "image", "gui"};
        fields(native, (char *)records + 0x38, 7, t, o, n);
    }
}
static void test_movie_source_link(void)
{
    fixture storage; fixture *native = &storage; setup(native); movie_reflection(native);
    sh_decl_registry_source registry = {native, read_memory, (uintptr_t)native->registry, NULL};
    static const unsigned char glyphs[14] = {'i','d','f','+',0,12,0,0,0,12,0,12,0,0};
    static const char atlas_red[] = "red atlas pixels", atlas_green[] = "green atlas pixels";
    const char *screen = "{ class = \"Entity\"; edit = { gui = \"swf/hud/panel.swf\"; } }";
    movie_bytes m; mv_build(&m, "GUI/Paint", "Title");
    sh_package_sources inventory = {0}; sh_compiled_resource files[4][6];
    sh_package_compilation compiled[4] = {0};
    const sh_package_compilation *scopes[] = {compiled, compiled + 1, compiled + 2, compiled + 3};
    movie_source_fixture f = {0};
    sh_package_source_link_native admission = {0}; admission.semantics = movie_semantics;
    sh_package_link_output out = {0}; char error[1024];
    for (size_t i = 0; i < 4; i++) {
        memset(files[i], 0, sizeof(files[i]));
        files[i][0] = link_decl("screen", "generated/decls/entitydef/screen.decl", screen);
        files[i][1].engine_path = "generated/swf/hud/panel.bswf";
        files[i][1].body = m.data; files[i][1].body_length = m.used; files[i][1].source = SIZE_MAX;
        files[i][2].engine_path = "generated/swf/hud/panel.bimage";
        files[i][2].body = (unsigned char *)(i == 1 ? atlas_green : atlas_red);
        files[i][2].body_length = strlen(i == 1 ? atlas_green : atlas_red); files[i][2].source = SIZE_MAX;
        files[i][3].engine_path = "fonts/title/64_df.dat";
        files[i][3].body = (unsigned char *)glyphs; files[i][3].body_length = sizeof(glyphs); files[i][3].source = SIZE_MAX;
        files[i][4] = link_decl("fonts/title/64_df.tga", "generated/decls/material/fonts/title/64_df.tga.decl",
            i == 3 ? "{ color {0,1,0} }" : "{ color {1,0,0} }");
        files[i][4].type = "material";
        files[i][5] = link_decl("gui/paint", "generated/decls/material/gui/paint.decl", "{ color {0,0,1} }");
        files[i][5].type = "material";
        compiled[i].resources = files[i]; compiled[i].resource_count = 6; compiled[i].sources = &inventory;
        /* Compilation lookup is ordered by engine path. */
        qsort(files[i], 6, sizeof(files[i][0]), resource_order);
    }
    sh_package_source_link *source = sh_package_source_link_open(scopes, 4, NULL, movie_original, &f,
        registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
    if (!source) fprintf(stderr, "movie source capture: %s\n", error);
    assert(source);
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(source);
    size_t movies[4], screens[4], fonts[4], paints[4];
    for (size_t i = 0; i < 4; i++) {
        const sh_package_link_unit *movie = movie_unit(inputs, i, "file", "generated/swf/hud/panel.bswf", movies + i);
        assert(movie && movie->file_count == 2 && movie->file_identities);
        /* Sorted members: the atlas image identity, then the primary movie. */
        assert(!strcmp(movie->files[0]->engine_path, "generated/swf/hud/panel.bimage"));
        assert(!strcmp(movie->file_identities[0].family, "image") &&
            !strcmp(movie->file_identities[0].name, "generated/swf/hud/panel.bimage"));
        assert(!movie->file_identities[1].family && !movie->file_identities[1].name);
        assert(movie_unit(inputs, i, "font", "title", fonts + i) && movie_unit(inputs, i, "material", "gui/paint", paints + i));
        assert(movie->implicit_count == 1 && !strcmp(movie->implicit[0].slot, "movie/font/title") &&
            movie->implicit[0].target == fonts[i]);
        assert(movie_unit(inputs, i, "entitydef", "screen", screens + i));
    }
    /* No fixed constructor material or fallback font exists in these scopes
     * or their originals: native default behavior is retained, not borrowed. */
    assert(sh_package_link_inputs_find(inputs, 0, "material", "guisolid") == SIZE_MAX);
    assert(sh_package_link_inputs_find(inputs, 0, "font", "square721 cn tl") == SIZE_MAX);
    sh_package_link_adapter adapter = {(void *)inputs, NULL, NULL, sh_package_native_names_assign};
    adapter.assign_members = sh_package_native_names_assign_members;
    int linked = sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error));
    if (!linked) fprintf(stderr, "movie source compile: %s\n", error);
    assert(linked);
    /* Equal pairs share one binding; a changed atlas or a changed font
     * material behind identical movie bytes each form their own variant. */
    assert(out.classes.nodes[movies[0]] == out.classes.nodes[movies[2]]);
    assert(out.classes.nodes[movies[0]] != out.classes.nodes[movies[1]]);
    assert(out.classes.nodes[movies[0]] != out.classes.nodes[movies[3]]);
    assert(out.classes.nodes[screens[0]] == out.classes.nodes[screens[2]]);
    assert(out.classes.nodes[screens[1]] != out.classes.nodes[screens[0]] && out.classes.nodes[screens[3]] != out.classes.nodes[screens[0]]);
    size_t movie_rows = 0;
    for (size_t i = 0; i < out.implicit_count; i++)
        if (out.implicit[i].owner == out.classes.nodes[movies[0]] || out.implicit[i].owner == out.classes.nodes[movies[1]]) {
            assert(!strcmp(out.implicit[i].slot, "movie/font/title")); movie_rows++;
        }
    assert(movie_rows == 2);
    for (size_t i = 0; i < 4; i++) {
        const char *movie_name = out.names[out.classes.nodes[movies[i]]];
        sh_package_movie_names names;
        assert(!strncmp(movie_name, "generated/swf/smp/", 18) && sh_package_movie_names_for_path(movie_name, &names) == 1);
        const sh_compiled_resource *cooked = sh_package_compilation_find(out.compiled, names.movie);
        const sh_compiled_resource *pixels = sh_package_compilation_find(out.compiled, names.atlas);
        assert(cooked && pixels && cooked->body && pixels->body);
        assert(pixels->body_length == strlen(i == 1 ? atlas_green : atlas_red));
        /* The dictionary material follows its scoped binding; the font record
         * keeps its original name for the native language resolver. */
        const char *paint = out.names[out.classes.nodes[paints[i]]];
        char needle[128]; int material_found = 0, font_found = 0;
        snprintf(needle, sizeof(needle), "%s", paint);
        for (size_t j = 0; j + strlen(needle) <= cooked->body_length; j++)
            if (!memcmp(cooked->body + j, needle, strlen(needle))) material_found = 1;
        for (size_t j = 0; j + 5 <= cooked->body_length; j++)
            if (!memcmp(cooked->body + j, "Title", 5)) font_found = 1;
        assert(material_found && font_found);
        /* The entity field names a constructor spelling that derives the pair. */
        const char *entity = out.names[out.classes.nodes[screens[i]]];
        char path[512]; snprintf(path, sizeof(path), "generated/decls/entitydef/%s.decl", entity);
        const sh_compiled_resource *decl = sh_package_compilation_find(out.compiled, path);
        assert(decl && decl->body);
        char expected[300]; snprintf(expected, sizeof(expected), "gui = \"%s\";", names.consumer);
        assert(strstr((const char *)decl->body, expected));
    }
    sh_package_link_output_free(&out); sh_package_source_link_close(source);

    /* Exact installed originals keep their native names and field spelling. */
    f.stock = 1; f.stock_movie = m;
    {
        const char *stock = "{ class = \"Entity\"; edit = { gui = \"stock/panel\"; } }";
        sh_compiled_resource only = link_decl("stock-screen", "generated/decls/entitydef/stock-screen.decl", stock);
        compiled[0].resources = &only; compiled[0].resource_count = 1;
        source = sh_package_source_link_open(scopes, 1, NULL, movie_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        if (!source) fprintf(stderr, "stock movie capture: %s\n", error);
        assert(source); inputs = sh_package_source_link_inputs(source);
        size_t stock_movie, fixed;
        const sh_package_link_unit *movie = movie_unit(inputs, 0, "file", "generated/swf/stock/panel.bswf", &stock_movie);
        assert(movie && movie->file_count == 2 && movie->implicit_count == 2);
        assert(movie_unit(inputs, 0, "material", "guisolid", &fixed));
        adapter.context = (void *)inputs;
        assert(sh_package_source_link_compile(source, &inventory, &adapter, NULL, NULL, &out, error, sizeof(error)));
        assert(!strcmp(out.names[out.classes.nodes[stock_movie]], "generated/swf/stock/panel.bswf"));
        size_t screen_index = sh_package_link_inputs_find(inputs, 0, "entitydef", "stock-screen");
        const char *entity = out.names[out.classes.nodes[screen_index]];
        char path[512]; snprintf(path, sizeof(path), "generated/decls/entitydef/%s.decl", entity);
        const sh_compiled_resource *decl = sh_package_compilation_find(out.compiled, path);
        assert(!decl || strstr((const char *)decl->body, "gui = \"stock/panel\";"));
        sh_package_link_output_free(&out); sh_package_source_link_close(source);
        /* A delivered half pair cannot construct. */
        sh_compiled_resource half[2] = {only, files[0][0]};
        half[1].engine_path = "generated/swf/stock/panel.bswf"; half[1].type = NULL; half[1].name = NULL;
        half[1].body = m.data; half[1].body_length = m.used; half[1].source = SIZE_MAX;
        compiled[0].resources = half; compiled[0].resource_count = 2;
        qsort(half, 2, sizeof(half[0]), resource_order);
        f.stock = 0;
        source = sh_package_source_link_open(scopes, 1, NULL, movie_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        assert(!source && strstr(error, "atlas must both be available"));
        /* I/O failure is never treated as positive absence. */
        f.stock = 1; f.fail = 1; compiled[0].resources = &only; compiled[0].resource_count = 1;
        source = sh_package_source_link_open(scopes, 1, NULL, movie_original, &f,
            registry, (uintptr_t)native->reflection, &admission, error, sizeof(error));
        assert(!source && strstr(error, "could not be captured"));
        f.fail = 0;
        /* A valid graph is not an installed constructor route. */
        sh_package_library_native production = sh_package_native_assets_adapter(NULL);
        source = sh_package_source_link_open(scopes, 1, NULL, movie_original, &f,
            registry, (uintptr_t)native->reflection, &production.source, error, sizeof(error));
        assert(!source && strstr(error, "UI movie native consumer adapter is not configured"));
    }
    f.stock = 0;
    {
        /* The shared schema recognizes the registered movie reader. */
        sh_decl_native_registry *r = sh_decl_native_registry_open(registry, error, sizeof(error));
        sh_decl_native_schema *schema = sh_decl_native_schema_open(
            (sh_decl_native_source){native, read_memory, (uintptr_t)native->reflection},
            (sh_decl_dependency_observer){0}, error, sizeof(error));
        sh_decl_native_reference reference = {0}; char *bound;
        assert(r && schema);
        assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idSWF", "*"}) == SH_DECL_READER_MOVIE);
        assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idSWF", ""}) == SH_DECL_READER_UNKNOWN);
        assert(sh_decl_native_reference_read(schema, r, (sh_decl_value_type){"idSWF", "*"}, "Hud\\Panel.swf", 13, &reference) == 1);
        assert(reference.movie && !strcmp(reference.resource.type, "file") &&
            !strcmp(reference.resource.name, "generated/swf/hud/panel.bswf"));
        bound = sh_decl_native_reference_bind(&reference, "generated/swf/smp/0123456789abcdef.bswf");
        assert(bound && !strcmp(bound, "swf/smp/0123456789abcdef.swf")); free(bound);
        assert(!sh_decl_native_reference_bind(&reference, "generated/swf/a.bimage/b.bswf"));
        assert(sh_decl_native_reference_read(schema, r, (sh_decl_value_type){"idSWF", "*"}, "", 0, &reference) == 0 && !reference.resource.name);
        assert(sh_decl_native_reference_read(schema, r, (sh_decl_value_type){"idSWF", "*"}, "swf/a.bimage/b.swf", 18, &reference) == -1);
        sh_decl_native_reference_clear(&reference);
        sh_decl_native_registry_close(r); sh_decl_native_schema_close(schema);
    }
    while (native->count) free(native->regions[--native->count].address);
}
/* Editor contributions compose an entity across scopes through class tokens.
 * A token must read back through its field's reader, and a movie field only
 * names a cooked generated/swf pair: its token keeps that spelling. */
static movie_source_fixture token_movies;
static int movie_token_original(void *context, const char *path, unsigned char **body, size_t *length)
{ (void)context; return movie_original(&token_movies, path, body, length); }
static void test_contribution_movie_token(void)
{
    fixture storage; fixture *native = &storage; setup(native); movie_reflection(native);
    sh_decl_registry_source registry = {native, read_memory, (uintptr_t)native->registry, NULL};
    const char *folders[] = {"red", "green"};
    char temporary[MAX_PATH], relative[256], text[256], error[1024];
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-movie-token-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    for (size_t i = 0; i < 2; i++) {
        snprintf(relative, sizeof(relative), "overrides/%s/package.json", folders[i]);
        create(relative, "{\"id\":\"same.id\",\"name\":\"Screens\"}");
        snprintf(relative, sizeof(relative), "overrides/%s/assets/generated/decls/entitydef/screen.decl", folders[i]);
        snprintf(text, sizeof(text), "{ class = \"Entity\"; editorVars = { %sControl = 1; } "
            "edit = { gui = \"swf/stock/panel.swf\"; } }", folders[i]);
        create(relative, text);
    }
    token_movies = (movie_source_fixture){0}; token_movies.stock = 1;
    mv_build(&token_movies.stock_movie, "GUI/Paint", "Absent");
    sh_package_sources *sources = sh_package_sources_scan(root, error, sizeof(error)); assert(sources);
    sh_package_compile_environment env = {0}; env.baseline = movie_token_original;
    sh_package_library_native certified = {0}; certified.source.semantics = movie_semantics;
    sh_package_library library = {0};
    int ok = sh_package_library_compile(sources, &env, NULL, movie_token_original, NULL, registry,
        (uintptr_t)native->reflection, &certified, &library, error, sizeof(error));
    if (!ok) fprintf(stderr, "contribution movie token: %s\n", error);
    assert(ok);
    size_t screens = 0;
    for (size_t i = 0; i < library.compiled->resource_count; i++) {
        const sh_compiled_resource *file = library.compiled->resources + i;
        const char *authored = file->original_path ? file->original_path : file->engine_path;
        assert(!strstr((char *)file->body, "smp-contribution/"));
        if (strcmp(authored, "generated/decls/entitydef/screen.decl")) continue;
        screens++;
        assert(strstr((char *)file->body, "redControl = 1") && strstr((char *)file->body, "greenControl = 1") &&
            strstr((char *)file->body, "gui = \"swf/stock/panel.swf\""));
    }
    assert(screens);
    sh_package_library_free(&library); sh_package_sources_free(sources); cleanup(); assert(!failures);
    while (native->count) free(native->regions[--native->count].address);
}
/* Effects end in the shared reflected reader: end of file may close only the
 * outermost brace, and quoted control bytes are ordinary string bytes. */
static void test_open_effect_body(void)
{
    fixture storage; fixture *native = &storage; setup(native);
    {
        void *managers = alloc(native, 6 * 8), *manager = alloc(native, 0x90);
        memcpy(managers, *(void **)((char *)native->registry + 8), 5 * 8);
        pointer(managers, 5 * 8, manager); pointer(native->registry, 8, managers); number(native->registry, 0x10, 6);
        pointer(manager, 8, string(native, "fx")); pointer(manager, 0x10, string(native, "idDeclFX"));
        void *container = *(void **)native->reflection;
        void *records = alloc(native, 21 * 0x38), *objects = alloc(native, 21 * 16), *pointers = alloc(native, 21 * 16);
        memcpy(records, *(void **)((char *)container + 0x20), 20 * 0x38);
        memcpy(objects, *(void **)((char *)native->reflection + 0x88), 20 * 16);
        memcpy(pointers, *(void **)((char *)native->reflection + 0xa0), 20 * 16);
        pointer(container, 0x20, records);
        pointer(native->reflection, 0x88, objects); number(native->reflection, 0x90, 21);
        pointer(native->reflection, 0xa0, pointers); number(native->reflection, 0xa8, 21);
        pointer(records, 19 * 0x38, string(native, "idDeclFX"));
        pointer(records, 19 * 0x38 + 8, string(native, "idDeclTypeInfo"));
        pointer(pointers, 19 * 16 + 8, (void *)0x10);
        const char *t[] = {"idDeclEntityDef", "idStr"}, *o[] = {"*", ""}, *n[] = {"target", "name"};
        fields(native, (char *)records + 19 * 0x38, 2, t, o, n);
    }
    sh_decl_registry_source registry = {native, read_memory, (uintptr_t)native->registry, NULL};
    static const char open[] = "{\r\n\tedit = {\r\n\t\ttarget = \"map-child\";\r\n"
        "\t\tname = \"charged\x1f_quad\";\r\n\t}\r\n";
    sh_package_compilation empty = {0};
    sh_package_source_binding result;
    bind_fixture bindings = {0};
    char error[1024]; unsigned char *out; size_t length;
    sh_package_source_reader *reader = sh_package_source_reader_open(&empty, NULL, original, native,
        registry, (uintptr_t)native->reflection, error, sizeof(error));
    assert(reader);
    out = sh_package_source_observe(reader, "fx", "open", (const unsigned char *)open, strlen(open),
        bind_reference, &bindings, &length, &result, error, sizeof(error));
    if (!out) fprintf(stderr, "open effect: %s\n", error);
    assert(out && length == strlen(open) && !memcmp(out, open, length) && result.references == 1); free(out);
    bindings.replace = 1;
    out = sh_package_source_bind(reader, "fx", "open", (const unsigned char *)open, strlen(open),
        bind_reference, &bindings, &length, &result, error, sizeof(error));
    assert(out && result.changed == 1 && strstr((char *)out, "\"bound/map-child\"") &&
        strstr((char *)out, "\"charged\x1f_quad\""));
    {
        /* A rebound body is written complete and still parses strictly. */
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)out, length}, error, sizeof(error));
        assert(tree); sh_decl_tree_free(tree); free(out);
    }
    bindings = (bind_fixture){0};
    /* Only a verified reader tolerates the missing brace; nested blocks and
     * newlines inside strings stay native parse errors. */
    assert(!sh_package_source_bind(reader, "actormodifier", "open", (const unsigned char *)open, strlen(open),
        bind_reference, &bindings, &length, &result, error, sizeof(error)) && strstr(error, "closing brace"));
    {
        const char *nested = "{ edit = { target = \"map-child\";", *broken = "{ edit = { name = \"a\nb\"; } }";
        assert(!sh_package_source_bind(reader, "fx", "nested", (const unsigned char *)nested, strlen(nested),
            bind_reference, &bindings, &length, &result, error, sizeof(error)));
        assert(!sh_package_source_bind(reader, "fx", "broken", (const unsigned char *)broken, strlen(broken),
            bind_reference, &bindings, &length, &result, error, sizeof(error)));
    }
    sh_package_source_reader_close(reader);
    {
        /* Candidate inspection follows the open effect's reference. */
        sh_compiled_resource resources[2] = {0};
        sh_package_compilation compiled = {0};
        sh_package_references paths = {0};
        sh_package_source_graph_report report;
        resources[0] = link_decl("amopen", "generated/decls/actormodifier/amopen.decl", open);
        resources[0].type = "actormodifier";
        resources[1] = link_decl("open", "generated/decls/fx/open.decl", open); resources[1].type = "fx";
        compiled.resources = resources; compiled.resource_count = 2;
        const char *json = "{\"targetType\":\"idDeclFX\",\"value\":\"open\"}";
        assert(sh_package_source_graph(&compiled, NULL, original, native, registry, (uintptr_t)native->reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(!paths.incomplete && !report.gaps && has(&paths, "fx/open.decl") && has(&paths, "map-child.decl"));
        json = "{\"targetType\":\"idDeclActorModifier\",\"value\":\"amopen\"}";
        assert(sh_package_source_graph(&compiled, NULL, original, native, registry, (uintptr_t)native->reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(paths.incomplete && report.gaps && !has(&paths, "map-child.decl"));
        sh_package_references_free(&paths);
    }
    assert(!native->object_state_reads);
    while (native->count) free(native->regions[--native->count].address);
}
static int admit_intrinsic(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    (void)context; (void)scope; (void)error; (void)capacity; memset(semantics, 0, 32);
    return !strcmp(family, "image") && !strcmp(name, "_black");
}
/* One image row registered without a payload path. */
static void fileless_catalog(const char *relative, const char *name)
{
    unsigned char bytes[256] = {5, 'S', 'E', 'R'}; size_t at = 40;
    const char *strings[] = {"image", name, ""};
    bytes[35] = 1;
    for (size_t s = 0; s < 3; s++) {
        size_t length = strlen(strings[s]); number(bytes, at, (int)length); at += 4;
        memcpy(bytes + at, strings[s], length); at += length;
    }
    at += 21;
    uint32_t size = (uint32_t)at - 32;
    bytes[4] = (unsigned char)(size >> 24); bytes[5] = (unsigned char)(size >> 16);
    bytes[6] = (unsigned char)(size >> 8); bytes[7] = (unsigned char)size;
    sound_binary(relative, bytes, at);
}

/* idStrRenderModel_Preview shares idStr's registered reader; the editor passes
 * its text to a model dispatcher, directly or as list elements. */
static void preview_reflection(fixture *native, uintptr_t preview_reader)
{
    void *container = *(void **)native->reflection;
    void *records = alloc(native, 22 * 0x38), *objects = alloc(native, 22 * 16), *pointers = alloc(native, 22 * 16);
    void *element = alloc(native, 0x48);
    memcpy(records, *(void **)((char *)container + 0x20), 20 * 0x38);
    memcpy(objects, *(void **)((char *)native->reflection + 0x88), 20 * 16);
    memcpy(pointers, *(void **)((char *)native->reflection + 0xa0), 20 * 16);
    pointer(container, 0x20, records);
    pointer(native->reflection, 0x88, objects); number(native->reflection, 0x90, 22);
    pointer(native->reflection, 0xa0, pointers); number(native->reflection, 0xa8, 22);
    pointer(records, 19 * 0x38, string(native, "idStrRenderModel_Preview")); pointer(records, 19 * 0x38 + 8, string(native, ""));
    pointer(objects, 19 * 16 + 8, (void *)preview_reader);
    pointer(records, 20 * 0x38, string(native, "idList < idStrRenderModel_Preview >")); pointer(records, 20 * 0x38 + 8, string(native, ""));
    pointer(objects, 20 * 16 + 8, (void *)0x80);
    pointer(records, 20 * 0x38 + 0x18, element);
    pointer(element, 0, string(native, "idStrRenderModel_Preview")); pointer(element, 8, string(native, ""));
    pointer(element, 0x10, string(native, "element")); number(element, 0x1c, 8);
    const char *t[] = {"idDeclEntityDef", "OpaqueStringReader", "idDeclActorModifier", "idDeclSnapEditorEntity",
        "idRenderModel", "idImage", "idStrRenderModel_Preview", "idList < idStrRenderModel_Preview >"};
    const char *o[] = {"*", "", "*", "*", "*", "*", "", ""};
    const char *n[] = {"target", "custom", "modifier", "preview", "model", "image", "previewModel", "previewModels"};
    fields(native, (char *)records + 0x38, 8, t, o, n);
}
static void test_preview_model(void)
{
    for (int variant = 0; variant < 2; variant++) {
        fixture storage; fixture *native = &storage; setup(native);
        /* 0x70 is idStr's registered reader; any other reader is another contract. */
        preview_reflection(native, variant ? 0x71 : 0x70);
        sh_decl_registry_source registry = {native, read_memory, (uintptr_t)native->registry, NULL};
        sh_compiled_resource resource = {0};
        sh_package_compilation compiled = {0};
        sh_package_source_binding result;
        bind_fixture bindings = {0};
        char error[1024]; unsigned char *out; size_t length;
        sh_decl_native_registry *r = sh_decl_native_registry_open(registry, error, sizeof(error));
        sh_decl_native_schema *schema = sh_decl_native_schema_open(
            (sh_decl_native_source){native, read_memory, (uintptr_t)native->reflection},
            (sh_decl_dependency_observer){0}, error, sizeof(error));
        sh_decl_native_reference reference = {0}; char *bound;
        const sh_decl_value_type preview = {"idStrRenderModel_Preview", ""};
        assert(r && schema);
        assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idStr", ""}) == SH_DECL_READER_UNKNOWN);
        assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idStrRenderModel_Preview", "*"}) == SH_DECL_READER_UNKNOWN);
        if (variant) {
            assert(sh_decl_native_schema_reader(schema, preview) == SH_DECL_READER_UNKNOWN);
            assert(sh_decl_native_reference_read(schema, r, preview, "zion/boss.md6", 13, &reference) == -1);
        } else {
            assert(sh_decl_native_schema_reader(schema, preview) == SH_DECL_READER_PREVIEW_MODEL);
            /* MD6 definitions keep the .md6 dispatch suffix in their identity. */
            assert(sh_decl_native_reference_read(schema, r, preview, "Zion\\Boss_SnapEditor.MD6", 24, &reference) == 1);
            assert(!strcmp(reference.resource.type, "md6def") && !strcmp(reference.resource.name, "zion/boss_snapeditor.md6"));
            bound = sh_decl_native_reference_bind(&reference, "smp/0123456789abcdef.md6");
            assert(bound && !strcmp(bound, "smp/0123456789abcdef.md6")); free(bound);
            assert(!sh_decl_native_reference_bind(&reference, "smp/0123456789abcdef"));
            assert(sh_decl_native_reference_read(schema, r, preview, "models/crate.lwo", 16, &reference) == 1);
            assert(!strcmp(reference.resource.type, "model") && !strcmp(reference.resource.name, "models/crate.lwo"));
            assert(sh_decl_native_reference_read(schema, r, preview, "effects/glow.hotspot", 20, &reference) == 1);
            assert(!strcmp(reference.resource.type, "hotspot") && !strcmp(reference.resource.name, "effects/glow"));
            /* The editor builds an _empty model for fx and tests for _empty itself;
             * the renderer's dispatcher asks FindModel for both. */
            assert(sh_decl_native_reference_read(schema, r, preview, "fx/sparks.fx", 12, &reference) == -1 && !reference.resource.name);
            assert(sh_decl_native_reference_read(schema, r, (sh_decl_value_type){"idRenderModel", "*"}, "fx/sparks.fx", 12, &reference) == 1);
            assert(sh_decl_native_reference_read(schema, r, preview, "_EMPTY", 6, &reference) == 0 && !reference.resource.name);
            assert(sh_decl_native_reference_read(schema, r, (sh_decl_value_type){"idRenderModel", "*"}, "_empty", 6, &reference) == 1);
            assert(sh_decl_native_reference_read(schema, r, preview, "", 0, &reference) == 0);
        }
        sh_decl_native_reference_clear(&reference);
        sh_decl_native_registry_close(r); sh_decl_native_schema_close(schema);

        resource.engine_path = "generated/decls/entitydef/root.decl";
        resource.body = (unsigned char *)"{ class = \"Entity\"; }";
        resource.body_length = strlen((const char *)resource.body);
        compiled.resources = &resource; compiled.resource_count = 1;
        sh_package_source_reader *reader = sh_package_source_reader_open(&compiled, NULL, original, native, registry,
            (uintptr_t)native->reflection, error, sizeof(error));
        assert(reader);
        const char *entity = "{ class = \"Entity\"; edit = { previewModel = \"zion/boss.md6\";"
            " previewModels = { num = 3; item[0] = \"models/crate.lwo\"; item[1] = \"_empty\"; item[2] = \"\"; } } }";
        bindings.replace = 1;
        out = sh_package_source_bind(reader, "entitydef", "preview", (const unsigned char *)entity,
            strlen(entity), bind_reference, &bindings, &length, &result, error, sizeof(error));
        if (variant) {
            /* The type name alone does not establish the consumer contract. */
            assert(!out && !length && result.inspection.gaps && !bindings.count);
        } else {
            if (!out) fprintf(stderr, "preview binding: %s\n", error);
            assert(out && result.references == 2 && result.changed == 2 && bindings.count == 2);
            assert(strstr((char *)out, "previewModel = \"bound/zion/boss.md6\"") &&
                strstr((char *)out, "item[0] = \"bound/models/crate.lwo\"") &&
                strstr((char *)out, "item[1] = \"_empty\"") && strstr((char *)out, "item[2] = \"\""));
            free(out);
            /* A saved map keys a counted list's slots, with NULL for an empty one. */
            const char *map = "{\"actor\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"saved/root\","
                "\"state\":{\"edit\":{\"previewModels\":{\"num\":2,\"item[0]\":\"saved/zion/boss.md6\",\"item[1]\":\"NULL\"}}}}}";
            const char *expected = "{\"actor\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\","
                "\"state\":{\"edit\":{\"previewModels\":{\"num\":2,\"item[0]\":\"zion/boss.md6\",\"item[1]\":\"NULL\"}}}}}";
            char *bound_map = sh_package_source_bind_map(reader, map, strlen(map), map_saved_binding, NULL,
                &length, &result, error, sizeof(error));
            if (!bound_map || strcmp(bound_map, expected)) fprintf(stderr, "keyed map list: %s\n", bound_map ? bound_map : error);
            assert(bound_map && !strcmp(bound_map, expected) && result.changed == 2 && !result.inspection.gaps);
            free(bound_map);
            const char *effect = "{ class = \"Entity\"; edit = { previewModel = \"fx/sparks.fx\"; } }";
            out = sh_package_source_bind(reader, "entitydef", "preview", (const unsigned char *)effect,
                strlen(effect), bind_reference, &bindings, &length, &result, error, sizeof(error));
            assert(!out && result.inspection.gaps && strstr(error, "fx/sparks.fx"));
        }
        sh_package_source_reader_close(reader);
        while (native->count) free(native->regions[--native->count].address);
    }
}

static int late_resource_available(void *context, const char *path, char *error, size_t capacity)
{
    (void)context; (void)error; (void)capacity;
    assert(!strstr(path, "editor-only") && !strstr(path, "unused"));
    return strcmp(path, "bound/payload.bin") != 0;
}

/* Native dispatch and companion readers add verified links which are not
 * written as reflected declaration fields. Admission must follow those exact
 * links, including cycles, without requiring unselected or editor-only files. */
static void test_late_required_resources(fixture *f, sh_decl_registry_source registry)
{
    sh_compiled_resource resources[6] = {0};
    sh_package_compilation compiled = {0};
    sh_package_sources sources = {0};
    sh_package_source_file files[2] = {0};
    sh_package_references paths = {0};
    sh_package_source_graph_report report;
    sh_package_missing missing = {0};
    char error[1024];
    size_t inputs[] = {0, 1};
    const char *names[] = {"bound/editor-only.bin", "bound/payload.bin", "bound/program.bin",
        "bound/unused.bin", "generated/decls/entitydef/late-root.decl",
        "generated/decls/snapeditorentitydef/editor-only.decl"};
    char *root_edges[] = {(char *)names[2], (char *)names[5], (char *)names[2]};
    char *program_edges[] = {(char *)names[1]};
    char *payload_edges[] = {(char *)names[2]};
    char *editor_edges[] = {(char *)names[0]};
    const char *body = "{ class = \"Entity\"; edit = {} }";
    const char *json = "{\"targetType\":\"idDeclEntityDef\",\"value\":\"late-root\"}";
    for (size_t i = 0; i < 6; i++) resources[i].engine_path = names[i];
    resources[4].type = "entitydef"; resources[4].name = "late-root";
    resources[4].body = (unsigned char *)body; resources[4].body_length = strlen(body);
    resources[4].bound_dependencies = root_edges; resources[4].bound_dependency_count = 3;
    resources[2].bound_dependencies = program_edges; resources[2].bound_dependency_count = 1;
    resources[1].bound_dependencies = payload_edges; resources[1].bound_dependency_count = 1;
    resources[1].sources = inputs; resources[1].source_count = 2;
    resources[5].type = "snapeditorentitydef";
    resources[5].bound_dependencies = editor_edges; resources[5].bound_dependency_count = 1;
    files[0].owner = 0; files[1].owner = 1;
    sources.files = files; sources.file_count = 2; sources.package_count = 2;
    compiled.resources = resources; compiled.resource_count = 6; compiled.sources = &sources;
    assert(sh_package_source_graph(&compiled, NULL, original, f, registry, (uintptr_t)f->reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(!paths.incomplete && !report.gaps);
    assert(has(&paths, "bound/program.bin") && has(&paths, "bound/payload.bin"));
    assert(!has(&paths, "editor-only") && !has(&paths, "unused"));
    const char **required = calloc(paths.count, sizeof(*required)); assert(required);
    for (size_t i = 0; i < paths.count; i++) required[i] = paths.items[i].name;
    assert(sh_package_compilation_missing(&compiled, required, paths.count, late_resource_available,
        NULL, &missing, error, sizeof(error)));
    assert(missing.count == 1 && !strcmp(missing.paths[0], "bound/payload.bin") &&
        sh_package_owners_count(&missing.packages) == 2);
    free(required); sh_package_missing_free(&missing); sh_package_references_free(&paths);
    /* A vanilla-only map does not acquire another installed root's closure. */
    json = "{}";
    assert(sh_package_source_graph(&compiled, NULL, original, f, registry, (uintptr_t)f->reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(!paths.count && !paths.incomplete); sh_package_references_free(&paths);
}

/* Packages failing on their own editor or registration contributions are
 * found together (sh_package_compile_environment.more_rejections): one failed
 * compile names each with its type and reason, whatever order discovery meets
 * them in, and the compile without them links as if they had never been
 * installed, replaying the earlier compiles' discovery. Stock editor rows and
 * the conductor serve both kinds; stages_original reads either. */
static const char *owners_stock_row = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; "
    "propertySheets = { num = 1; item[0] = { properties = { num = 2; item[0] = { path = \"health\"; type = \"float\"; } "
    "item[1] = { path = \"speed\"; type = \"float\"; } } } } } }";
static const char *owners_stock_standalone = "{ edit = { target = \"parent-only\"; "
    "propertySheets = { num = 1; item[0] = { properties = { num = 2; item[0] = { path = \"health\"; type = \"float\"; } "
    "item[1] = { path = \"speed\"; type = \"float\"; } } } } } }";
/* e_child: a stock child of demo_parent with its own inputs. */
static const char *owners_stock_inputs = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; "
    "inputs = { num = 1; item[0] = \"parent-only\"; } } }";
static int stages_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    /* c_stock: a stock child of demo_parent restating the whole list;
     * d_row: a stock row with no parent. */
    const char *stock = !strcmp(path, "generated/decls/snapeditorentitydef/c_stock.decl") ? owners_stock_row :
        !strcmp(path, "generated/decls/snapeditorentitydef/e_child.decl") ? owners_stock_inputs :
        !strcmp(path, "generated/decls/snapeditorentitydef/d_row.decl") ? owners_stock_standalone : NULL;
    if (stock) { *length = strlen(stock); *body = (unsigned char *)_strdup(stock); return *body ? 1 : -1; }
    return strstr(path, "snapeditorentitydef/demo_") || strstr(path, "whitelistencounterdecl/") ?
        contribution_original(context, path, body, length) : registration_original(context, path, body, length);
}
/* An editor declaration whose edit block never closes. The native reflected
 * reader lets end of file close only the outermost brace; a nested block left
 * open is not a declaration it reads (decl_tree.h,
 * sh_decl_tree_parse_reflected_body), so the owner is invalid natively. */
static void owners_unclosed(const char *folder, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/snapeditorentitydef/%s.decl", folder, name);
    create(path, "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; item[0] = { properties = { num = 2; "
        "item[0] = { path = \"health\"; type = \"float\"; } item[1] = { path = \"speed\"; type = \"float\"; } } } }");
}
/* Whether length bytes at body contain needle. */
static int memchr_text(const unsigned char *body, size_t length, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t at = 0; body && at + n <= length; at++) if (!memcmp(body + at, needle, n)) return 1;
    return 0;
}
static const char *owners_layout = "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; item[0] = { properties = { num = 3; "
    "item[0] = { path = \"armor\"; type = \"float\"; } item[1] = { path = \"health\"; type = \"float\"; } "
    "item[2] = { path = \"speed\"; type = \"float\"; } } } } } }";
/* demo_child describing health instead of speed (its authored patch replaces
 * the stock child's). */
static const char *owners_label = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; "
    "propertySheets = { item[0] = { properties = { item[0] = { descriptionTag = \"#str_health\"; } } } } } }";
static void owners_package(const char *folder)
{
    char path[512], text[256];
    snprintf(path, sizeof(path), "overrides/%s/package.json", folder);
    snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", folder, folder); create(path, text);
}
static void owners_file(const char *folder, const char *relative, const char *text)
{
    char path[512];
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/%s", folder, relative); create(path, text);
}
typedef struct owners_run {
    int ok, own;
    size_t invalid, unserved;
    sh_package_rejections more;
    sh_package_library library;
    char error[1024];
} owners_run;
static void owners_compile(fixture *f, sh_decl_registry_source registry, const sh_package_sources *sources,
    sh_package_source_memo *memo, int collect, owners_run *run)
{
    source_link_fixture originals = {f};
    sh_package_compile_environment env = {0};
    sh_package_library_native native = {0};
    native.private_keys = 1; native.source.memo = memo;
    run->invalid = run->unserved = SIZE_MAX; run->own = 0;
    env.baseline = stages_original; env.baseline_context = &originals;
    env.invalid_package = &run->invalid; env.unserved_package = &run->unserved; env.invalid_own_bytes = &run->own;
    if (collect) env.more_rejections = &run->more;
    run->ok = sh_package_library_compile(sources, &env, NULL, stages_original, &originals,
        registry, (uintptr_t)f->reflection, &native, &run->library, run->error, sizeof(run->error));
}
static void owners_done(owners_run *run)
{ sh_package_library_free(&run->library); sh_package_rejections_free(&run->more); }
/* The typed findings of one failed compile, as "name:kind" in finding order
 * (kind i invalid, u not served), the first from invalid/unserved. */
static void owners_found(const sh_package_sources *sources, const owners_run *run, char *out, size_t capacity)
{
    size_t used = 0; out[0] = 0;
    size_t first = run->invalid != SIZE_MAX ? run->invalid : run->unserved;
    if (first != SIZE_MAX) used += snprintf(out + used, capacity - used, "%s:%c", sources->packages[first].name,
        run->invalid != SIZE_MAX ? 'i' : 'u');
    for (size_t i = 0; i < run->more.count && used < capacity; i++) {
        const sh_package_rejection *r = run->more.items + i;
        used += snprintf(out + used, capacity - used, " %s:%c%s", sources->packages[r->package].name,
            r->invalid ? 'i' : 'u', r->own_bytes ? "!" : "");
    }
}
/* Remove every package named by a failed compile. */
static void owners_leave(sh_package_sources *sources, const owners_run *run)
{
    size_t names[16], count = 0;
    if (run->invalid != SIZE_MAX) names[count++] = run->invalid;
    if (run->unserved != SIZE_MAX) names[count++] = run->unserved;
    for (size_t i = 0; i < run->more.count && count < 16; i++) names[count++] = run->more.items[i].package;
    for (size_t a = 0; a < count; a++) for (size_t b = a + 1; b < count; b++)
        if (names[b] > names[a]) { size_t t = names[a]; names[a] = names[b]; names[b] = t; }
    for (size_t a = 0; a < count; a++) assert(sh_package_sources_remove(sources, names[a]));
}
/* 1 when the findings are exactly the expected "name:kind" tokens, in any
 * order: each expected token is consumed by one finding, so a repeated
 * finding cannot stand in for a missing owner. */
static int owners_same_set(const char *found, const char *const *expected, size_t count)
{
    unsigned char used[16] = {0};
    size_t tokens = 0;
    assert(count <= sizeof(used));
    for (const char *at = found; *at;) {
        while (*at == ' ') at++;
        if (!*at) break;
        const char *end = strchr(at, ' '); size_t length = end ? (size_t)(end - at) : strlen(at), i;
        for (i = 0; i < count; i++)
            if (!used[i] && strlen(expected[i]) == length && !strncmp(expected[i], at, length)) break;
        if (i == count) return 0;
        used[i] = 1; tokens++; at += length;
    }
    return tokens == count;
}
/* Consumes one failed compile's findings against the allowed "name:kind"
 * tokens: each may be found once over all compiles (seen). The number of
 * findings, or -1 for a finding outside them or found again. */
static int owners_consume(const char *found, const char *const *allowed, size_t count, unsigned char *seen)
{
    int consumed = 0;
    for (const char *at = found; *at;) {
        while (*at == ' ') at++;
        if (!*at) break;
        const char *end = strchr(at, ' '); size_t length = end ? (size_t)(end - at) : strlen(at), i;
        for (i = 0; i < count; i++)
            if (!seen[i] && strlen(allowed[i]) == length && !strncmp(allowed[i], at, length)) break;
        if (i == count) return -1;
        seen[i] = 1; consumed++; at += length;
    }
    return consumed;
}
/* Each compiled whitelist named list: every entry either names a compiled
 * entity declaration or is the authored spelling own-enc left as an engine
 * default (a scope without own-enc); suppliers counts the distinct compiled
 * own-enc declarations the lists bind. */
static int stale_whitelists_bound(const sh_package_compilation *compiled, size_t *lists, size_t *suppliers)
{
    const sh_compiled_resource *seen[8];
    size_t seen_count = 0;
    int bound = 1;
    *lists = 0;
    for (size_t r = 0; r < compiled->resource_count; r++) {
        const sh_compiled_resource *file = compiled->resources + r;
        const char *authored = file->original_path ? file->original_path : file->engine_path;
        if (!authored || !strstr(authored, "snappropertyinspector_whitelistencounterdecl/list.decl")) continue;
        (*lists)++;
        sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)file->body, file->body_length}, NULL, 0);
        const sh_decl_node *items = tree ? sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "validEncounters") : NULL;
        if (!items) bound = 0;
        for (const sh_decl_node *item = items ? items->children : NULL; item; item = item->next) {
            if (!strcmp(item->key, "num")) continue;
            const char *value = node_value(item);
            const sh_compiled_resource *target = named_resource(compiled, "entitydef", value);
            if (!target) {
                if (strcmp(value, "own-enc")) { bound = 0; fprintf(stderr, "stale whitelist entry unbound: %s\n", value); }
                continue;
            }
            if (!strstr(target->original_path ? target->original_path : target->engine_path, "own-enc")) continue;
            size_t k;
            for (k = 0; k < seen_count && seen[k] != target; k++) {}
            if (k == seen_count && seen_count < 8) seen[seen_count++] = target;
        }
        sh_decl_tree_free(tree);
    }
    *suppliers = seen_count;
    return bound && *lists;
}
/* The 2026-10-01 OpenGL inventory: three packages ship byte-identical encounter
 * whitelists naming one encounter that only one of them supplies (the variant
 * fixtures had dropped it from their own scopes). In those two scopes the name
 * is an engine default, as authored text may leave it; the shared composition
 * reaches every scope, where the engine reads the entry by name. Nobody is
 * malformed: the library links with all three served, every whitelist entry
 * bound. Before the fix the supplier's scope met the default entry without a
 * captured binding and the whole inventory was refused, unattributed. A
 * provably malformed owner beside them is still found alone, and the compile
 * without it serves the rest. */
static void test_contribution_stale_reference(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    const char *list = "{ edit = { mode = \"shared\"; validEncounters = { num = 1; item[0] = \"own-enc\"; } } }";
    char found[512];
    for (int order = 0; order < 2; order++) {
        const char *supplier = order ? "z-supplier" : "a-supplier";
        const char *one = order ? "a-stale" : "y-stale", *two = order ? "b-stale" : "z-stale";
        const char *broken = order ? "a-broken" : "z-broken";
        const char *expected[1];
        char want[1][32];
        size_t lists = 0;
        registration_root("stale-reference");
        owners_package(supplier);
        owners_file(supplier, "entitydef/own-enc.decl", "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }");
        owners_file(supplier, "snappropertyinspector_whitelistencounterdecl/list.decl", list);
        owners_package(one); owners_file(one, "snappropertyinspector_whitelistencounterdecl/list.decl", list);
        owners_package(two); owners_file(two, "snappropertyinspector_whitelistencounterdecl/list.decl", list);
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0}, isolated = {0}, healthy = {0};
        owners_compile(f, registry, sources, NULL, 1, &run);
        owners_found(sources, &run, found, sizeof(found));
        if (!run.ok || found[0]) fprintf(stderr, "stale reference %d: ok=%d found=%s %s\n", order, run.ok, found, run.error);
        assert(run.ok && run.invalid == SIZE_MAX && run.unserved == SIZE_MAX && !run.more.count);
        assert(sources->package_count == 3);
        size_t suppliers = 0;
        int bound = stale_whitelists_bound(run.library.compiled, &lists, &suppliers);
        if (!bound || suppliers != 1)
            fprintf(stderr, "stale reference %d: %zu whitelist(s), %zu supplier(s) bound\n", order, lists, suppliers);
        assert(bound && suppliers == 1);
        owners_done(&run);
        sh_package_sources_free(sources);
        /* An owner whose own whitelist does not read is found alone. */
        owners_package(broken);
        owners_file(broken, "snappropertyinspector_whitelistencounterdecl/list.decl",
            "{ edit = { mode = \"shared\"; validEncounters = { num = 1; }");
        sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_compile(f, registry, sources, NULL, 1, &isolated);
        owners_found(sources, &isolated, found, sizeof(found));
        snprintf(want[0], sizeof(want[0]), "%s:i", broken); expected[0] = want[0];
        if (isolated.ok || !owners_same_set(found, expected, 1))
            fprintf(stderr, "stale reference %d with a broken owner: ok=%d found=%s %s\n", order, isolated.ok, found, isolated.error);
        assert(!isolated.ok && owners_same_set(found, expected, 1));
        owners_leave(sources, &isolated); owners_done(&isolated);
        owners_compile(f, registry, sources, NULL, 1, &healthy);
        if (!healthy.ok) fprintf(stderr, "stale reference %d without the broken owner: %s\n", order, healthy.error);
        assert(healthy.ok && healthy.invalid == SIZE_MAX && !healthy.more.count && sources->package_count == 3);
        assert(stale_whitelists_bound(healthy.library.compiled, &lists, &suppliers) && suppliers == 1);
        owners_done(&healthy);
        sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    /* Two suppliers ship different own-enc declarations, and a stale owner's
     * whitelist names own-enc too. Contributions run before identities, so the
     * two suppliers' entries share the authored spelling in composed text: the
     * stale owner's default entry must not take either of them. */
    for (int order = 0; order < 2; order++) {
        const char *first = order ? "z-first" : "a-first", *second = order ? "a-second" : "z-second";
        const char *stale = order ? "m-stale" : "b-stale";
        registration_root("stale-overlap");
        owners_package(first);
        owners_file(first, "entitydef/own-enc.decl", "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }");
        owners_file(first, "snappropertyinspector_whitelistencounterdecl/list.decl", list);
        owners_package(second);
        owners_file(second, "entitydef/own-enc.decl", "{ class = \"Entity\"; edit = { target = \"modifier-child\"; } }");
        owners_file(second, "snappropertyinspector_whitelistencounterdecl/list.decl", list);
        owners_package(stale); owners_file(stale, "snappropertyinspector_whitelistencounterdecl/list.decl", list);
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0};
        owners_compile(f, registry, sources, NULL, 1, &run);
        owners_found(sources, &run, found, sizeof(found));
        if (!run.ok || found[0]) fprintf(stderr, "stale overlap %d: ok=%d found=%s %s\n", order, run.ok, found, run.error);
        assert(run.ok && !found[0]);
        /* Each supplier's own-enc is its own declaration. */
        const char *parent_only = NULL, *modifier_child = NULL;
        for (size_t r = 0; r < run.library.compiled->resource_count; r++) {
            const sh_compiled_resource *file = run.library.compiled->resources + r;
            const char *authored = file->original_path ? file->original_path : file->engine_path;
            if (!authored || !strstr(authored, "entitydef/own-enc.decl")) continue;
            if (strstr((const char *)file->body, "parent-only")) parent_only = file->name;
            else if (strstr((const char *)file->body, "modifier-child")) modifier_child = file->name;
        }
        assert(parent_only && modifier_child && strcmp(parent_only, modifier_child));
        /* One whitelist per scope. Each supplier's entry keeps its own
         * declaration everywhere; the stale entry is the supplier's own in each
         * supplier's scope and an engine default in the stale owner's. */
        int shapes[3] = {0, 0, 0}; /* {default,first,second}, {first x2,second}, {first,second x2} */
        size_t lists = 0;
        for (size_t r = 0; r < run.library.compiled->resource_count; r++) {
            const sh_compiled_resource *file = run.library.compiled->resources + r;
            const char *authored = file->original_path ? file->original_path : file->engine_path;
            if (!authored || !strstr(authored, "whitelistencounterdecl/list.decl")) continue;
            lists++;
            sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)file->body, file->body_length}, NULL, 0);
            const sh_decl_node *items = tree ? sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "validEncounters") : NULL;
            int raw = 0, a = 0, z = 0, other = 0;
            for (const sh_decl_node *item = items ? items->children : NULL; item; item = item->next) {
                if (!strcmp(item->key, "num")) continue;
                const char *value = node_value(item);
                if (!strcmp(value, "own-enc")) raw++;
                else if (!strcmp(value, parent_only)) a++;
                else if (!strcmp(value, modifier_child)) z++;
                else other++;
            }
            sh_decl_tree_free(tree);
            if (!other && raw == 1 && a == 1 && z == 1) shapes[0]++;
            else if (!other && !raw && a == 2 && z == 1) shapes[1]++;
            else if (!other && !raw && a == 1 && z == 2) shapes[2]++;
            else fprintf(stderr, "stale overlap %d: whitelist %s has default %d, parent-only %d, modifier-child %d, other %d\n",
                order, file->name, raw, a, z, other);
        }
        assert(lists == 3 && shapes[0] == 1 && shapes[1] == 1 && shapes[2] == 1);
        owners_done(&run);
        sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
}

/* The composed controls of one stock child of demo_parent, as
 * "path=label:type" ("?" for a control without a path), and its body. */
static void anon_layout(const owners_run *run, const char *name, char *out, size_t capacity, char *body, size_t body_capacity)
{
    const sh_compiled_resource *child = named_resource(run->library.compiled, "snapeditorentitydef", name);
    sh_decl_node *tree = child ? sh_decl_tree_parse((sh_decl_source){(char *)child->body, child->body_length}, NULL, 0) : NULL;
    const sh_decl_node *properties = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
        sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties");
    size_t used = 0;
    out[0] = 0; body[0] = 0;
    if (child) snprintf(body, body_capacity, "%.*s", (int)child->body_length, (const char *)child->body);
    for (int k = 0; properties; k++) {
        char slot[32];
        const sh_decl_node *item, *path, *label, *type;
        snprintf(slot, sizeof(slot), "item[%d]", k);
        if (!(item = sh_decl_tree_member(properties, slot))) break;
        char path_text[256] = "?", label_text[256] = "", type_text[256] = "";
        path = sh_decl_tree_member(item, "path"); label = sh_decl_tree_member(item, "descriptionTag");
        type = sh_decl_tree_member(item, "type");
        /* node_value returns one shared buffer: copy each before the next. */
        if (path) snprintf(path_text, sizeof(path_text), "%s", node_value(path));
        if (label) snprintf(label_text, sizeof(label_text), "=%s", node_value(label));
        if (type) snprintf(type_text, sizeof(type_text), ":%s", node_value(type));
        used += snprintf(out + used, capacity - used, "%s%s%s%s", k ? "," : "", path_text, label_text, type_text);
        if (used >= capacity) break;
    }
    sh_decl_tree_free(tree);
}
/* An optional relaid demo_parent (relay; NULL keeps the stock layout) and
 * two packages restating a stock child, admitted in both orders; returns the
 * composed layout and body. */
static void anon_compile(fixture *f, sh_decl_registry_source registry, int order, const char *relay, const char *name,
    const char *first_text, const char *second_text, char *layout, size_t layout_capacity, char *body, size_t body_capacity)
{
    char found[512], relative[128];
    const char *first = order ? "z-first" : "a-first", *second = order ? "a-second" : "z-second";
    registration_root("owners-anonymous");
    snprintf(relative, sizeof(relative), "snapeditorentitydef/%s.decl", name);
    if (relay) { owners_package("m-relay"); owners_file("m-relay", "snapeditorentitydef/demo_parent.decl", relay); }
    owners_package(first); owners_file(first, relative, first_text);
    if (second_text) { owners_package(second); owners_file(second, relative, second_text); }
    sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
    owners_run run = {0};
    owners_compile(f, registry, sources, NULL, 1, &run);
    owners_found(sources, &run, found, sizeof(found));
    if (!run.ok || found[0]) fprintf(stderr, "anonymous %s %d: ok=%d found=%s %s\n", name, order, run.ok, found, run.error);
    assert(run.ok && !found[0] && !run.more.count);
    anon_layout(&run, name, layout, layout_capacity, body, body_capacity);
    owners_done(&run); sh_package_sources_free(sources); cleanup(); assert(!failures);
}
/* Two packages restating demo_parent whole (no parent of its own, so nothing
 * is rebased), admitted in both orders; returns its composed layout and body.
 * A NULL text writes owners_nameless_parent with that type instead. */
static void parent_compile(fixture *f, sh_decl_registry_source registry, int order, const char *first_text,
    const char *second_text, char *layout, size_t layout_capacity, char *body, size_t body_capacity)
{
    char found[512];
    const char *first = order ? "z-first" : "a-first", *second = order ? "a-second" : "z-second";
    registration_root("owners-anonymous-parent");
    owners_package(first); owners_file(first, "snapeditorentitydef/demo_parent.decl", first_text);
    owners_package(second); owners_file(second, "snapeditorentitydef/demo_parent.decl", second_text);
    sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
    owners_run run = {0};
    owners_compile(f, registry, sources, NULL, 1, &run);
    owners_found(sources, &run, found, sizeof(found));
    if (!run.ok || found[0]) fprintf(stderr, "anonymous parent %d: ok=%d found=%s %s\n", order, run.ok, found, run.error);
    assert(run.ok && !found[0] && !run.more.count);
    anon_layout(&run, "demo_parent", layout, layout_capacity, body, body_capacity);
    owners_done(&run); sh_package_sources_free(sources); cleanup(); assert(!failures);
}
/* Controls without a path are placed natively by slot alone. On a relaid
 * parent each package's restatement is rebased into the published frame and
 * composed: different packages' different such controls stay two, an exact
 * duplicate is one control (as equal identities are), one package's repeated
 * equal controls keep their number, the shipped child's own equal controls
 * stay exactly two with each package's edit on its own slot, and the result
 * does not depend on admission order. The namespace those identities use
 * moves past any a package or a parent states, whose values stay intact. */
static void test_contribution_anonymous_controls(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char layout[2][512], body[2][2048];
    const char *plain = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { "
        "properties = { num = %s; %s } } } } }";
    char add_float[512], add_int[512], add_two[512], add_named[512];
    snprintf(add_float, sizeof(add_float), plain, "3", "item[2] = { type = \"float\"; }");
    snprintf(add_int, sizeof(add_int), plain, "3", "item[2] = { type = \"int\"; }");
    snprintf(add_two, sizeof(add_two), plain, "4", "item[2] = { type = \"float\"; } item[3] = { type = \"float\"; }");
    snprintf(add_named, sizeof(add_named), plain, "3", "item[2] = { path = \"#snapmap-plus-slot-0:p0\"; type = \"bool\"; }");
    /* Different packages' different controls: both, in either order. */
    for (int order = 0; order < 2; order++)
        anon_compile(f, registry, order, owners_layout, "demo_plain", add_float, add_int, layout[order], sizeof(layout[0]),
            body[order], sizeof(body[0]));
    if (strcmp(body[0], body[1]) || (strcmp(layout[0], "armor:float,health:float,speed:float,?:float,?:int") &&
            strcmp(layout[0], "armor:float,health:float,speed:float,?:int,?:float")))
        fprintf(stderr, "anonymous different: %s | %s\n", layout[0], layout[1]);
    assert(!strcmp(body[0], body[1]));
    assert(!strcmp(layout[0], "armor:float,health:float,speed:float,?:float,?:int") ||
        !strcmp(layout[0], "armor:float,health:float,speed:float,?:int,?:float"));
    /* An exact duplicate is one control. */
    for (int order = 0; order < 2; order++)
        anon_compile(f, registry, order, owners_layout, "demo_plain", add_float, add_float, layout[order], sizeof(layout[0]),
            body[order], sizeof(body[0]));
    if (strcmp(body[0], body[1]) || strcmp(layout[0], "armor:float,health:float,speed:float,?:float"))
        fprintf(stderr, "anonymous duplicate: %s | %s\n", layout[0], layout[1]);
    assert(!strcmp(body[0], body[1]) && !strcmp(layout[0], "armor:float,health:float,speed:float,?:float"));
    /* Repeated equal controls keep their number; the other package's equal
     * one is the first of them. */
    for (int order = 0; order < 2; order++)
        anon_compile(f, registry, order, owners_layout, "demo_plain", add_two, add_float, layout[order], sizeof(layout[0]),
            body[order], sizeof(body[0]));
    if (strcmp(body[0], body[1]) || strcmp(layout[0], "armor:float,health:float,speed:float,?:float,?:float"))
        fprintf(stderr, "anonymous repeated: %s | %s\n", layout[0], layout[1]);
    assert(!strcmp(body[0], body[1]) && !strcmp(layout[0], "armor:float,health:float,speed:float,?:float,?:float"));
    /* The shipped child's own equal controls: each package's copy restates
     * them at their native slots, so they stay two, and an edit lands on the
     * slot its author edited. */
    for (int order = 0; order < 2; order++)
        anon_compile(f, registry, order, owners_layout, "demo_anon",
            "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { properties = { "
            "num = 4; item[0] = { descriptionTag = \"#str_health\"; } item[2] = { type = \"float\"; } "
            "item[3] = { type = \"float\"; } } } } } }",
            "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { properties = { "
            "num = 4; item[2] = { type = \"float\"; } item[3] = { type = \"float\"; descriptionTag = \"#str_second\"; } "
            "} } } } }", layout[order], sizeof(layout[0]), body[order], sizeof(body[0]));
    if (strcmp(body[0], body[1]) ||
            strcmp(layout[0], "armor:float,health=#str_health:float,speed:float,?:float,?=#str_second:float"))
        fprintf(stderr, "anonymous shipped: %s | %s\n", layout[0], layout[1]);
    assert(!strcmp(body[0], body[1]) &&
        !strcmp(layout[0], "armor:float,health=#str_health:float,speed:float,?:float,?=#str_second:float"));
    /* The parent's layout unchanged: each package appends through a sparse
     * page patch (page zero is inherited; no outer count), and the same
     * identities apply. Different additions stay two, an exact duplicate is
     * one, repeated equal ones keep their number; every package is served
     * with its control, and no page beyond the inherited one is stated. */
    {
        const char *sparse = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { "
            "properties = { num = %s; %s } } } } }";
        char sparse_float[512], sparse_int[512], sparse_two[512];
        snprintf(sparse_float, sizeof(sparse_float), sparse, "3", "item[2] = { type = \"float\"; }");
        snprintf(sparse_int, sizeof(sparse_int), sparse, "3", "item[2] = { type = \"int\"; }");
        snprintf(sparse_two, sizeof(sparse_two), sparse, "4", "item[2] = { type = \"float\"; } item[3] = { type = \"float\"; }");
        for (int order = 0; order < 2; order++)
            anon_compile(f, registry, order, NULL, "demo_plain", sparse_float, sparse_int, layout[order], sizeof(layout[0]),
                body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || (strcmp(layout[0], "health:float,speed:float,?:float,?:int") &&
                strcmp(layout[0], "health:float,speed:float,?:int,?:float")) || !strstr(body[0], "propertySheets = { num = 1;"))
            fprintf(stderr, "anonymous same layout different: %s | %s\n", layout[0], body[0]);
        assert(!strcmp(body[0], body[1]) && (!strcmp(layout[0], "health:float,speed:float,?:float,?:int") ||
            !strcmp(layout[0], "health:float,speed:float,?:int,?:float")) && strstr(body[0], "propertySheets = { num = 1;"));
        for (int order = 0; order < 2; order++)
            anon_compile(f, registry, order, NULL, "demo_plain", sparse_float, sparse_float, layout[order], sizeof(layout[0]),
                body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || strcmp(layout[0], "health:float,speed:float,?:float"))
            fprintf(stderr, "anonymous same layout duplicate: %s | %s\n", layout[0], layout[1]);
        assert(!strcmp(body[0], body[1]) && !strcmp(layout[0], "health:float,speed:float,?:float"));
        for (int order = 0; order < 2; order++)
            anon_compile(f, registry, order, NULL, "demo_plain", sparse_two, sparse_float, layout[order], sizeof(layout[0]),
                body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || strcmp(layout[0], "health:float,speed:float,?:float,?:float"))
            fprintf(stderr, "anonymous same layout repeated: %s | %s\n", layout[0], layout[1]);
        assert(!strcmp(body[0], body[1]) && !strcmp(layout[0], "health:float,speed:float,?:float,?:float"));
        /* The shipped child's own equal controls on the unchanged layout: each
         * edit stays on its own slot, and a further addition is kept. */
        for (int order = 0; order < 2; order++)
            anon_compile(f, registry, order, NULL, "demo_anon",
                "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { properties = { "
                "item[3] = { descriptionTag = \"#str_second\"; } } } } } }",
                "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { properties = { "
                "num = 5; item[4] = { type = \"int\"; } } } } } }", layout[order], sizeof(layout[0]), body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || strcmp(layout[0], "health:float,speed:float,?:float,?=#str_second:float,?:int"))
            fprintf(stderr, "anonymous same layout shipped: %s | %s\n", layout[0], layout[1]);
        assert(!strcmp(body[0], body[1]) && !strcmp(layout[0], "health:float,speed:float,?:float,?=#str_second:float,?:int"));
    }
    /* A package stating a value inside the default namespace: the identities
     * move to another, and that value is composed as authored. */
    for (int order = 0; order < 2; order++)
        anon_compile(f, registry, order, owners_layout, "demo_plain", add_named, add_float, layout[order], sizeof(layout[0]),
            body[order], sizeof(body[0]));
    if (strcmp(body[0], body[1]) || !strstr(layout[0], "#snapmap-plus-slot-0:p0:bool") || !strstr(layout[0], "?:float") ||
            strstr(body[0], "#snapmap-plus-slot-1:"))
        fprintf(stderr, "anonymous namespace: %s | %s\n", layout[0], body[0]);
    assert(!strcmp(body[0], body[1]) && strstr(layout[0], "#snapmap-plus-slot-0:p0:bool") && strstr(layout[0], "?:float") &&
        !strstr(body[0], "#snapmap-plus-slot-1:"));
    /* A parent stating a value inside the default namespace: the rebase meets
     * it in the published parent and starts again under the next. */
    {
        const char *relaid_prefix = "armor:float,health:float,speed:float,#snapmap-plus-slot-0:s0.0:int,";
        const char *relay = "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; item[0] = { properties = { num = 4; "
            "item[0] = { path = \"armor\"; type = \"float\"; } item[1] = { path = \"health\"; type = \"float\"; } "
            "item[2] = { path = \"speed\"; type = \"float\"; } item[3] = { path = \"#snapmap-plus-slot-0:s0.0\"; "
            "type = \"int\"; } } } } } }";
        for (int order = 0; order < 2; order++)
            anon_compile(f, registry, order, relay, "demo_plain", add_float, add_int, layout[order], sizeof(layout[0]),
                body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || strncmp(layout[0], relaid_prefix, strlen(relaid_prefix)) ||
                !strstr(layout[0], "?:float") || !strstr(layout[0], "?:int") || strstr(body[0], "#snapmap-plus-slot-1:"))
            fprintf(stderr, "anonymous parent namespace: %s | %s\n", layout[0], body[0]);
        assert(!strcmp(body[0], body[1]) &&
            !strncmp(layout[0], relaid_prefix, strlen(relaid_prefix)) &&
            strstr(layout[0], "?:float") && strstr(layout[0], "?:int") && !strstr(body[0], "#snapmap-plus-slot-1:"));
    }
    /* A parent restated whole by two packages (no rebase): the shipped
     * controls stay as shipped, different added controls without a path stay
     * two, an exact duplicate is one, repeated equal ones keep their number,
     * and an authored value inside the default namespace is kept. */
    {
        const char *whole = "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; item[0] = { properties = { "
            "num = %s; item[0] = { path = \"health\"; type = \"float\"; } item[1] = { path = \"speed\"; type = \"float\"; } "
            "%s } } } } }";
        char with_float[512], with_int[512], with_two[512], with_named[512];
        snprintf(with_float, sizeof(with_float), whole, "3", "item[2] = { type = \"float\"; }");
        snprintf(with_int, sizeof(with_int), whole, "3", "item[2] = { type = \"int\"; }");
        snprintf(with_two, sizeof(with_two), whole, "4", "item[2] = { type = \"float\"; } item[3] = { type = \"float\"; }");
        snprintf(with_named, sizeof(with_named), whole, "3", "item[2] = { path = \"#snapmap-plus-slot-0:c0#0\"; type = \"bool\"; }");
        for (int order = 0; order < 2; order++)
            parent_compile(f, registry, order, with_float, with_int, layout[order], sizeof(layout[0]), body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || (strcmp(layout[0], "health:float,speed:float,?:float,?:int") &&
                strcmp(layout[0], "health:float,speed:float,?:int,?:float")))
            fprintf(stderr, "anonymous parent different: %s | %s\n", layout[0], layout[1]);
        assert(!strcmp(body[0], body[1]) && (!strcmp(layout[0], "health:float,speed:float,?:float,?:int") ||
            !strcmp(layout[0], "health:float,speed:float,?:int,?:float")));
        for (int order = 0; order < 2; order++)
            parent_compile(f, registry, order, with_float, with_float, layout[order], sizeof(layout[0]), body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || strcmp(layout[0], "health:float,speed:float,?:float"))
            fprintf(stderr, "anonymous parent duplicate: %s | %s\n", layout[0], layout[1]);
        assert(!strcmp(body[0], body[1]) && !strcmp(layout[0], "health:float,speed:float,?:float"));
        for (int order = 0; order < 2; order++)
            parent_compile(f, registry, order, with_two, with_float, layout[order], sizeof(layout[0]), body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || strcmp(layout[0], "health:float,speed:float,?:float,?:float"))
            fprintf(stderr, "anonymous parent repeated: %s | %s\n", layout[0], layout[1]);
        assert(!strcmp(body[0], body[1]) && !strcmp(layout[0], "health:float,speed:float,?:float,?:float"));
        for (int order = 0; order < 2; order++)
            parent_compile(f, registry, order, with_named, with_float, layout[order], sizeof(layout[0]), body[order], sizeof(body[0]));
        if (strcmp(body[0], body[1]) || !strstr(layout[0], "#snapmap-plus-slot-0:c0#0:bool") || !strstr(layout[0], "?:float") ||
                strstr(body[0], "#snapmap-plus-slot-1:"))
            fprintf(stderr, "anonymous parent namespace: %s | %s\n", layout[0], body[0]);
        assert(!strcmp(body[0], body[1]) && strstr(layout[0], "#snapmap-plus-slot-0:c0#0:bool") && strstr(layout[0], "?:float") &&
            !strstr(body[0], "#snapmap-plus-slot-1:"));
    }
    for (size_t i = 0; i < f->count; i++) free(f->regions[i].address);
}
/* Page 0's controls as the native reader leaves them after reading texts in
 * order (the farthest parent first), independent of the compiler (B3
 * evidence F2-F6, F10, resize workers): a statement's first count applies
 * before its items, below 1 only zeroing the count, otherwise keeping the
 * first min(old, new) slots with the rest empty; an item past the count is
 * skipped; an item writes only the scalar fields it names. Each control's
 * fields sorted ("key=value" joined by ','), controls joined by ';'. */
typedef struct native_control { char keys[12][64], values[12][160]; int count; } native_control;
static int native_count_of(const sh_decl_node *list, int *count)
{
    for (const sh_decl_node *child = list->children; child; child = child->next)
        if (!strcmp(child->key, "num")) { *count = atoi(child->value ? child->value : "0"); return 1; }
    return 0;
}
static void native_controls(const char *const *texts, size_t text_count, char *out, size_t capacity)
{
    native_control *controls = calloc(32, sizeof(*controls));
    int pages = 0, count = 0;
    assert(controls);
    for (size_t t = 0; t < text_count; t++) {
        sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){texts[t], strlen(texts[t])}, NULL, 0);
        assert(tree);
        for (const sh_decl_node *edit = tree->children; edit; edit = edit->next) {
            if (strcmp(edit->key, "edit") || !edit->compound) continue;
            for (const sh_decl_node *sheet = edit->children; sheet; sheet = sheet->next) {
                int n, index;
                if (strcmp(sheet->key, "propertySheets") || !sheet->compound) continue;
                if (native_count_of(sheet, &n)) {
                    if (n < 1) pages = 0;
                    else { if (!pages) { memset(controls, 0, 32 * sizeof(*controls)); count = 0; } pages = n; }
                }
                for (const sh_decl_node *page = sheet->children; page; page = page->next) {
                    if (sscanf_s(page->key, "item[%d]", &index) != 1 || index != 0 || index >= pages || !page->compound) continue;
                    for (const sh_decl_node *list = page->children; list; list = list->next) {
                        int slots, j;
                        if (strcmp(list->key, "properties") || !list->compound) continue;
                        if (native_count_of(list, &slots)) {
                            if (slots < 1) count = 0;
                            else {
                                assert(slots <= 32);
                                for (int fresh = count; fresh < slots; fresh++) memset(controls + fresh, 0, sizeof(*controls));
                                count = slots;
                            }
                        }
                        for (const sh_decl_node *item = list->children; item; item = item->next) {
                            if (sscanf_s(item->key, "item[%d]", &j) != 1 || j < 0 || j >= count || !item->compound) continue;
                            for (const sh_decl_node *field = item->children; field; field = field->next) {
                                native_control *c = controls + j;
                                int at = 0;
                                if (field->compound) continue;
                                while (at < c->count && strcmp(c->keys[at], field->key)) at++;
                                if (at == c->count) { assert(c->count < 12); c->count++; }
                                snprintf(c->keys[at], sizeof(c->keys[at]), "%s", field->key);
                                snprintf(c->values[at], sizeof(c->values[at]), "%s", node_value(field));
                            }
                        }
                    }
                }
            }
        }
        sh_decl_tree_free(tree);
    }
    out[0] = 0;
    for (int j = 0; j < count; j++) {
        native_control *c = controls + j;
        for (int a = 0; a < c->count; a++) for (int b = a + 1; b < c->count; b++)
            if (strcmp(c->keys[b], c->keys[a]) < 0) {
                char key[64], value[160];
                memcpy(key, c->keys[a], sizeof(key)); memcpy(c->keys[a], c->keys[b], sizeof(key)); memcpy(c->keys[b], key, sizeof(key));
                memcpy(value, c->values[a], sizeof(value)); memcpy(c->values[a], c->values[b], sizeof(value));
                memcpy(c->values[b], value, sizeof(value));
            }
        if (j) strcat_s(out, capacity, ";");
        for (int a = 0; a < c->count; a++) {
            if (a) strcat_s(out, capacity, ",");
            strcat_s(out, capacity, c->keys[a]); strcat_s(out, capacity, "="); strcat_s(out, capacity, c->values[a]);
        }
    }
    free(controls);
}
static char *served_text(const owners_run *run, const char *name)
{
    const sh_compiled_resource *file = named_resource(run->library.compiled, "snapeditorentitydef", name);
    char *text = file ? malloc(file->body_length + 1) : NULL;
    if (text) { memcpy(text, file->body, file->body_length); text[file->body_length] = 0; }
    return text;
}
/* The live b3-1-vk inherited-child shape: unrelated packages append controls
 * without a path to a parent (one a label, one a description) and edit its
 * second control's description, while a stock child re-keys that control by
 * a sparse patch of slot 1. Admitted in both orders, the served child, read
 * natively over the served parent, is exactly what the child's own sparse
 * text reads there: the edited control keeps its slot and the peer's label,
 * the additions follow it, and no slot keeps a previous occupant's field.
 * A package member stating edit and its list more than once composes as its
 * native form (nobody blamed). A stock child emptying its controls against a
 * peer's addition is an open product gap: reported, nobody excluded. */
static void test_contribution_inherited_rekey(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char found[512], parent_read[2][512], child_read[2][512], stock_read[512];
    const char *whole = "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; item[0] = { properties = { "
        "num = %s; item[0] = { path = \"health\"; type = \"float\"; } item[1] = { path = \"speed\"; type = \"float\";%s } "
        "%s } } } } }";
    char label_only[512], description_only[512], relabel[512];
    snprintf(label_only, sizeof(label_only), whole, "3", "", "item[2] = { displayNameTag = \"#str_size\"; }");
    snprintf(description_only, sizeof(description_only), whole, "3", "", "item[2] = { descriptionTag = \"#str_size_desc\"; }");
    snprintf(relabel, sizeof(relabel), whole, "2", " descriptionTag = \"#str_label\";", "");
    for (int order = 0; order < 2; order++) {
        const char *one = order ? "z-one" : "a-one", *two = order ? "y-two" : "b-two", *label = order ? "x-label" : "c-label";
        registration_root("inherited-rekey");
        owners_package(one); owners_file(one, "snapeditorentitydef/demo_parent.decl", label_only);
        owners_package(two); owners_file(two, "snapeditorentitydef/demo_parent.decl", description_only);
        owners_package(label); owners_file(label, "snapeditorentitydef/demo_parent.decl", relabel);
        owners_package("m-copy"); owners_file("m-copy", "snapeditorentitydef/demo_rekey.decl", demo_rekey_original);
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0};
        owners_compile(f, registry, sources, NULL, 1, &run);
        owners_found(sources, &run, found, sizeof(found));
        if (!run.ok || found[0]) fprintf(stderr, "inherited rekey %d: ok=%d found=%s %s\n", order, run.ok, found, run.error);
        assert(run.ok && !found[0]);
        char *parent = served_text(&run, "demo_parent"), *child = served_text(&run, "demo_rekey");
        assert(parent && child);
        const char *served[2] = {parent, child}, *stock[2] = {parent, demo_rekey_original}, *alone[1] = {parent};
        native_controls(alone, 1, parent_read[order], sizeof(parent_read[0]));
        native_controls(served, 2, child_read[order], sizeof(child_read[0]));
        native_controls(stock, 2, stock_read, sizeof(stock_read));
        if (strcmp(child_read[order], stock_read))
            fprintf(stderr, "inherited rekey %d: served %s\nstock reads %s\nchild %s\n", order, child_read[order], stock_read, child);
        assert(!strcmp(child_read[order], stock_read));
        assert(!strncmp(child_read[order], "path=health,type=float;descriptionTag=#str_label,path=scale,type=float;", 70));
        assert(strstr(child_read[order], ";displayNameTag=#str_size") && strstr(child_read[order], ";descriptionTag=#str_size_desc"));
        free(parent); free(child);
        owners_done(&run); sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    assert(!strcmp(parent_read[0], parent_read[1]) && !strcmp(child_read[0], child_read[1]));
    /* Repeated statements: a package restating demo_child with edit and its
     * list stated twice, over a relaid parent, composes as its native form. */
    {
        const char *repeated = "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { "
            "properties = { item[0] = { descriptionTag = \"#str_h1\"; } } } } } edit = { propertySheets = { item[0] = { "
            "properties = { item[0] = { descriptionTag = \"#str_h2\"; } item[1] = { descriptionTag = \"#str_s\"; } } } } } }";
        registration_root("inherited-repeated");
        owners_package("m-relay"); owners_file("m-relay", "snapeditorentitydef/demo_parent.decl", owners_layout);
        owners_package("r-repeat"); owners_file("r-repeat", "snapeditorentitydef/demo_child.decl", repeated);
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0};
        owners_compile(f, registry, sources, NULL, 1, &run);
        owners_found(sources, &run, found, sizeof(found));
        if (!run.ok || found[0]) fprintf(stderr, "inherited repeated: ok=%d found=%s %s\n", run.ok, found, run.error);
        assert(run.ok && !found[0]);
        char *parent = served_text(&run, "demo_parent"), *child = served_text(&run, "demo_child");
        assert(parent && child);
        const char *served[2] = {parent, child};
        native_controls(served, 2, child_read[0], sizeof(child_read[0]));
        if (strcmp(child_read[0], "path=armor,type=float;descriptionTag=#str_h2,path=health,type=float;"
                "descriptionTag=#str_s,path=speed,type=float"))
            fprintf(stderr, "inherited repeated: %s\n%s\n", child_read[0], child);
        assert(!strcmp(child_read[0], "path=armor,type=float;descriptionTag=#str_h2,path=health,type=float;"
            "descriptionTag=#str_s,path=speed,type=float"));
        free(parent); free(child);
        owners_done(&run); sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    /* A stock child emptying its controls and a peer's addition: the composed
     * child keeps the addition (a removal never erases a peer's addition),
     * which this encoding cannot serve without a stranding count. The
     * candidate reports the gap and excludes nobody. */
    {
        char added[512];
        snprintf(added, sizeof(added), whole, "3", "", "item[2] = { displayNameTag = \"#str_size\"; }");
        registration_root("inherited-zero");
        owners_package("a-add"); owners_file("a-add", "snapeditorentitydef/demo_parent.decl", added);
        owners_package("m-copy"); owners_file("m-copy", "snapeditorentitydef/demo_zero.decl",
            "{ inherit = \"demo_parent\"; edit = { target = \"parent-only\"; propertySheets = { item[0] = { "
            "properties = { num = 0; } } } } }");
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0};
        owners_compile(f, registry, sources, NULL, 1, &run);
        if (run.ok || run.invalid != SIZE_MAX || run.unserved != SIZE_MAX || run.more.count || !strstr(run.error, "proven"))
            fprintf(stderr, "inherited zero: ok=%d invalid=%zu unserved=%zu more=%zu %s\n", run.ok, run.invalid, run.unserved,
                run.more.count, run.error);
        assert(!run.ok && run.invalid == SIZE_MAX && run.unserved == SIZE_MAX && !run.more.count && strstr(run.error, "proven"));
        owners_done(&run); sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    for (size_t i = 0; i < f->count; i++) free(f->regions[i].address);
}
static void test_contribution_owners_together(void)
{
    fixture storage; fixture *f = &storage; setup(f); conductor_reflection(f);
    sh_decl_registry_source registry = {f, read_memory, (uintptr_t)f->registry, NULL};
    char found[512];
    /* 1. One declaration, two packages' independent defects, and valid peers
     * contributing to it and to its child: one compile finds both and blames
     * no peer; without them the library links as a healthy-only compile. By
     * two routes: native-invalid bytes (an unclosed nested block), found while
     * the sources are read, so own-bytes findings with no peer discovery yet to
     * replay; and a defect judged where the declaration's group composes (the
     * contribution-defect seam, over native-valid restatements), after which
     * the unchanged peers replay their discovery. */
    for (int route = 0; route < 2; route++) for (int order = 0; order < 2; order++) {
        const char *one = order ? "z-one" : "a-one", *two = order ? "a-two" : "z-two";
        const char *expected[2];
        char want[2][32];
        sh_package_source_memo *memo = sh_package_source_memo_open(); assert(memo);
        const char *defective[2] = {one, two};
        registration_root("owners-group");
        owners_package(one); owners_package(two);
        if (!route) { owners_unclosed(one, "demo_parent"); owners_unclosed(two, "demo_parent"); }
        else {
            owners_file(one, "snapeditorentitydef/demo_parent.decl", "{ edit = { target = \"parent-only\"; propertySheets = { "
                "num = 1; item[0] = { properties = { num = 3; item[0] = { path = \"health\"; type = \"float\"; } "
                "item[1] = { path = \"speed\"; type = \"float\"; } item[2] = { type = \"float\"; } } } } } }");
            owners_file(two, "snapeditorentitydef/demo_parent.decl", "{ edit = { target = \"parent-only\"; propertySheets = { "
                "num = 1; item[0] = { properties = { num = 3; item[0] = { path = \"health\"; type = \"float\"; } "
                "item[1] = { path = \"speed\"; type = \"float\"; } item[2] = { type = \"int\"; } } } } } }");
            sh_package_library_test_contribution_defects(defective, 2);
        }
        owners_package("m-layout"); owners_file("m-layout", "snapeditorentitydef/demo_parent.decl", owners_layout);
        owners_package("m-label"); owners_file("m-label", "snapeditorentitydef/demo_child.decl", owners_label);
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0}, healthy = {0}, cold = {0};
        owners_compile(f, registry, sources, memo, 1, &run);
        sh_package_library_test_contribution_defects(NULL, 0);
        owners_found(sources, &run, found, sizeof(found));
        /* Malformed bytes are own-bytes findings: the first is run.own and the
         * other is marked "!", in either order. A group's judgement is not. */
        {
            const char *first = order ? two : one, *second = order ? one : two;
            snprintf(want[0], sizeof(want[0]), "%s:i", first);
            snprintf(want[1], sizeof(want[1]), route ? "%s:i" : "%s:i!", second);
        }
        expected[0] = want[0]; expected[1] = want[1];
        if (run.ok || run.own != !route || !owners_same_set(found, expected, 2) || !strstr(run.error, "demo_parent"))
            fprintf(stderr, "owners in one group %d/%d: ok=%d own=%d found=%s %s\n", route, order, run.ok, run.own, found, run.error);
        assert(!run.ok && run.own == !route && owners_same_set(found, expected, 2) && strstr(run.error, "demo_parent"));
        owners_leave(sources, &run); owners_done(&run);
        owners_compile(f, registry, sources, memo, 1, &healthy);
        if (!healthy.ok) fprintf(stderr, "owners in one group %d without them: %s\n", order, healthy.error);
        assert(healthy.ok && healthy.invalid == SIZE_MAX && healthy.unserved == SIZE_MAX && !healthy.more.count);
        owners_compile(f, registry, sources, NULL, 1, &cold);
        assert(cold.ok);
        library_same(&healthy.library, &cold.library);
        /* Malformed bytes are found while the sources are read, before any
         * discovery (a list's out-of-range or unknown element is only a native
         * warning), so that retry has nothing to replay; after a group's
         * judgement the peers' discovery is replayed. */
        size_t retained = library_retained(&healthy.library);
        if (retained == SIZE_MAX || !retained != !route)
            fprintf(stderr, "owners in one group %d/%d replay: %zu %s\n", route, order, retained, healthy.library.repeated_work);
        assert(retained != SIZE_MAX && !retained == !route);
        {
            /* The peers' controls where they placed them: the shared parent
             * leads with m-layout's armor, and m-label's description stays on
             * health at its slot of that layout, under the one parent. */
            const sh_compiled_resource *parent = named_resource(healthy.library.compiled, "snapeditorentitydef", "demo_parent");
            const sh_compiled_resource *child = named_resource(healthy.library.compiled, "snapeditorentitydef", "demo_child");
            char layout[256];
            assert(parent && child && property_count(parent, NULL) == 3 && property_count(parent, "armor") == 1);
            child_layout(healthy.library.compiled, layout, sizeof(layout));
            sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){(char *)child->body, child->body_length}, NULL, 0);
            const sh_decl_node *inherit = tree ? sh_decl_tree_member(tree, "inherit") : NULL;
            int placed = !strcmp(layout, "armor,health=#str_health,speed") && inherit &&
                !strcmp(node_value(inherit), parent->name);
            if (!placed) fprintf(stderr, "owners in one group %d child: %s %.*s\n", order, layout,
                (int)child->body_length, (char *)child->body);
            sh_decl_tree_free(tree);
            assert(placed);
        }
        owners_done(&healthy); owners_done(&cold);
        sh_package_sources_free(sources); sh_package_source_memo_close(memo); cleanup(); assert(!failures);
    }
    /* 2. Contribution stages together: an editor row judged defective where
     * its group composes, a parent-chain gap (not served) and a registration
     * list judged defective in the registration phase are found by one
     * compile, each with its own type (the contribution-defect seam stands in
     * for the editor and registration judgements: no editor list has a
     * native-invalid input of its own, and two modifier rows for one AI are
     * native first-match, a valid list). */
    for (int order = 0; order < 2; order++) {
        const char *rows = order ? "z-rows" : "a-rows", *regs = order ? "a-regs" : "z-regs";
        const char *cycle = order ? "b-cycle" : "y-cycle";
        const char *defective[2] = {rows, regs};
        const char *expected[3];
        char want[3][32];
        registration_root("owners-stages");
        owners_package(rows); owners_file(rows, "snapeditorentitydef/demo_parent.decl", owners_layout);
        owners_package(cycle);
        owners_file(cycle, "snapeditorentitydef/demo_parent.decl", "{ inherit = \"demo_child\"; edit = { target = \"parent-only\"; } }");
        owners_package(regs);
        owners_file(regs, "entitydef/conductor.decl", "{ class = \"Entity\"; edit = { globalAIMods = { num = 1; "
            "item[0] = { aiType = \"T5\"; healthMod = \"a\"; } } } }");
        owners_package("m-label"); owners_file("m-label", "snapeditorentitydef/demo_child.decl", owners_label);
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0}, healthy = {0};
        sh_package_library_test_contribution_defects(defective, 2);
        owners_compile(f, registry, sources, NULL, 1, &run);
        sh_package_library_test_contribution_defects(NULL, 0);
        owners_found(sources, &run, found, sizeof(found));
        snprintf(want[0], sizeof(want[0]), "%s:i", rows); snprintf(want[1], sizeof(want[1]), "%s:u", cycle);
        snprintf(want[2], sizeof(want[2]), "%s:i", regs);
        expected[0] = want[0]; expected[1] = want[1]; expected[2] = want[2];
        if (run.ok || run.own || !owners_same_set(found, expected, 3))
            fprintf(stderr, "owners across stages %d: ok=%d own=%d found=%s %s\n", order, run.ok, run.own, found, run.error);
        assert(!run.ok && !run.own && owners_same_set(found, expected, 3));
        owners_leave(sources, &run); owners_done(&run);
        owners_compile(f, registry, sources, NULL, 1, &healthy);
        if (!healthy.ok) fprintf(stderr, "owners across stages %d without them: %s\n", order, healthy.error);
        assert(healthy.ok && !healthy.more.count);
        owners_done(&healthy);
        sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    /* 3. A group rebasing onto a parent that composed a found package's
     * content is left for the next compile rather than judged against that
     * content. one restates demo_parent with its own layout (armor first),
     * ships c_stock unchanged (a child composing demo_parent early) and a
     * d_row that cannot compose; two gives e_child, a later child of
     * demo_parent, a parent chain that loops back to it (not served); m-peer
     * adds an input to e_child. Scope: two's loop is its own defect whatever
     * the parent, so this pins conservative withholding and its bounds, not a
     * false blame avoided: every finding is one (invalid) or two (not served),
     * each once; one is found by the first compile; m-peer is never blamed;
     * at most one failed compile per bad owner, and the library then serves
     * m-peer's input. */
    for (int order = 0; order < 2; order++) {
        const char *one = order ? "z-one" : "a-one", *two = order ? "a-two" : "z-two";
        char want[2][32]; const char *allowed[2] = {want[0], want[1]};
        unsigned char seen[2] = {0};
        int compiles = 0, linked = 0;
        registration_root("owners-dependent");
        owners_package(one); owners_file(one, "snapeditorentitydef/demo_parent.decl", owners_layout);
        owners_file(one, "snapeditorentitydef/c_stock.decl", owners_stock_row);
        owners_unclosed(one, "d_row");
        owners_package(two);
        owners_file(two, "snapeditorentitydef/e_child.decl", "{ inherit = \"f_loop\"; edit = { target = \"parent-only\"; } }");
        owners_file(two, "snapeditorentitydef/f_loop.decl", "{ inherit = \"e_child\"; edit = { target = \"parent-only\"; } }");
        owners_package("m-peer"); owners_file("m-peer", "snapeditorentitydef/e_child.decl", "{ inherit = \"demo_parent\"; "
            "edit = { target = \"parent-only\"; inputs = { num = 2; item[0] = \"parent-only\"; item[1] = \"modifier-child\"; } } }");
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        snprintf(want[0], sizeof(want[0]), "%s:i", one); snprintf(want[1], sizeof(want[1]), "%s:u", two);
        while (!linked && compiles < 3) {
            owners_run run = {0};
            owners_compile(f, registry, sources, NULL, 1, &run);
            owners_found(sources, &run, found, sizeof(found));
            compiles++;
            int right;
            if (run.ok) {
                /* m-peer's added input is served. */
                const sh_compiled_resource *peer = named_resource(run.library.compiled, "snapeditorentitydef", "e_child");
                sh_decl_node *tree = peer ? sh_decl_tree_parse((sh_decl_source){(char *)peer->body, peer->body_length}, NULL, 0) : NULL;
                const sh_decl_node *inputs = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "inputs");
                right = inputs && sh_decl_tree_member(inputs, "num") && !strcmp(node_value(sh_decl_tree_member(inputs, "num")), "2");
                if (!right && peer) fprintf(stderr, "dependent group %d peer: %.*s\n", order, (int)peer->body_length, (char *)peer->body);
                sh_decl_tree_free(tree);
                linked = 1;
            } else {
                int consumed = owners_consume(found, allowed, 2, seen);
                right = consumed > 0 && (compiles > 1 || seen[0]);
            }
            if (!right) fprintf(stderr, "dependent group %d compile %d: ok=%d found=%s %s\n", order, compiles, run.ok, found, run.error);
            assert(right);
            owners_leave(sources, &run); owners_done(&run);
        }
        assert(linked && seen[0] && seen[1] && compiles <= 3);
        sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    /* 5. Registration input prepared before discovery, scope by scope: two
     * spawn lists with an invalid row (invalid) and an inherited registration
     * whose parent is absent (not served), beside a valid registration, are
     * found in one compile. */
    for (int order = 0; order < 2; order++) {
        const char *first = order ? "z-first" : "a-first", *second = order ? "a-second" : "z-second";
        const char *absent = order ? "b-absent" : "y-absent";
        const char *expected[3];
        char want[3][32];
        registration_root("owners-prepared");
        owners_package(first);
        owners_file(first, "entitydef/conductor.decl", "{ class = \"Entity\"; edit = { aiTypeList = { num = 1; item[1] = {} } } }");
        owners_package(second);
        owners_file(second, "entitydef/conductor.decl", "{ class = \"Entity\"; edit = { aiTypeList = { num = 2; "
            "item[0] = { aiType = \"IMP\"; entityDef = \"imp\"; } item[1] = 5; } } }");
        owners_package(absent);
        owners_file(absent, "entitydef/absent_child.decl", "{ inherit = \"absent-registration-parent\"; edit = { } }");
        registration_package("m-valid", "524288");
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0}, healthy = {0};
        owners_compile(f, registry, sources, NULL, 1, &run);
        owners_found(sources, &run, found, sizeof(found));
        snprintf(want[0], sizeof(want[0]), "%s:i", first); snprintf(want[1], sizeof(want[1]), "%s:i", second);
        snprintf(want[2], sizeof(want[2]), "%s:u", absent);
        expected[0] = want[0]; expected[1] = want[1]; expected[2] = want[2];
        if (run.ok || run.own || !owners_same_set(found, expected, 3))
            fprintf(stderr, "prepared registrations %d: ok=%d own=%d found=%s %s\n", order, run.ok, run.own, found, run.error);
        assert(!run.ok && !run.own && owners_same_set(found, expected, 3));
        owners_leave(sources, &run); owners_done(&run);
        owners_compile(f, registry, sources, NULL, 1, &healthy);
        if (!healthy.ok) fprintf(stderr, "prepared registrations %d without them: %s\n", order, healthy.error);
        assert(healthy.ok && !healthy.more.count && spawner_key(healthy.library.compiled, "m-valid"));
        owners_done(&healthy);
        sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    /* 6. Why a dependent group is withheld rather than judged. one relays
     * demo_parent (armor first) and its d_row is then judged defective where
     * that row's group composes (the contribution-defect seam). m-label's and
     * m-nameless's patches of demo_child (a label on health; a control without
     * a path, placed by slot) were rebased onto one's layout, content of a
     * found package, so their group is withheld instead of judged: the
     * patches are never blamed. three's two modifier rows for one AI are
     * native first-match (R-091), a valid list: three is served with its
     * first row. Without one the library links with both patches on the
     * stock layout. The relaid layout composing with both patches is pinned
     * below. */
    for (int order = 0; order < 2; order++) {
        const char *one = order ? "z-one" : "a-one", *three = order ? "a-three" : "z-three";
        char defect[64];
        const char *defective[1] = {defect};
        const char *expected[1];
        char want[1][32];
        registration_root("owners-withheld-gap");
        owners_package(one); owners_file(one, "snapeditorentitydef/demo_parent.decl", owners_layout);
        owners_file(one, "snapeditorentitydef/c_stock.decl", owners_stock_row);
        owners_file(one, "snapeditorentitydef/d_row.decl", "{ edit = { target = \"parent-only\"; propertySheets = { num = 1; "
            "item[0] = { properties = { num = 2; item[0] = { path = \"health\"; type = \"float\"; descriptionTag = \"#str_row\"; } "
            "item[1] = { path = \"speed\"; type = \"float\"; } } } } } }");
        owners_package("m-label"); owners_file("m-label", "snapeditorentitydef/demo_child.decl", owners_label);
        owners_package("m-nameless"); owners_file("m-nameless", "snapeditorentitydef/demo_child.decl", "{ inherit = \"demo_parent\"; "
            "edit = { target = \"parent-only\"; propertySheets = { item[0] = { properties = { num = 3; item[2] = { type = \"float\"; } } } } } }");
        owners_package(three);
        owners_file(three, "entitydef/conductor.decl", "{ class = \"Entity\"; edit = { globalAIMods = { num = 2; "
            "item[0] = { aiType = \"T5\"; healthMod = \"a\"; } item[1] = { aiType = \"T5\"; healthMod = \"b\"; } } } }");
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0}, healthy = {0};
        snprintf(defect, sizeof(defect), "%s:d_row", one);
        sh_package_library_test_contribution_defects(defective, 1);
        owners_compile(f, registry, sources, NULL, 1, &run);
        sh_package_library_test_contribution_defects(NULL, 0);
        owners_found(sources, &run, found, sizeof(found));
        snprintf(want[0], sizeof(want[0]), "%s:i", one); expected[0] = want[0];
        if (run.ok || run.own || !owners_same_set(found, expected, 1))
            fprintf(stderr, "withheld gap %d: ok=%d own=%d found=%s %s\n", order, run.ok, run.own, found, run.error);
        assert(!run.ok && !run.own && owners_same_set(found, expected, 1));
        owners_leave(sources, &run); owners_done(&run);
        owners_compile(f, registry, sources, NULL, 1, &healthy);
        if (!healthy.ok) fprintf(stderr, "withheld gap %d without them: %s\n", order, healthy.error);
        assert(healthy.ok && !healthy.more.count);
        {
            /* Both sparse patches, by slot of the stock layout: m-label's
             * description on health (slot 0), m-nameless's control at slot 2. */
            const sh_compiled_resource *child = named_resource(healthy.library.compiled, "snapeditorentitydef", "demo_child");
            sh_decl_node *tree = child ? sh_decl_tree_parse((sh_decl_source){(char *)child->body, child->body_length}, NULL, 0) : NULL;
            const sh_decl_node *properties = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
                sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties");
            const sh_decl_node *health = sh_decl_tree_member(properties, "item[0]");
            const sh_decl_node *added = sh_decl_tree_member(properties, "item[2]");
            const sh_decl_node *tag = sh_decl_tree_member(health, "descriptionTag");
            int placed = tag && !strcmp(node_value(tag), "#str_health") && added && !sh_decl_tree_member(added, "path") &&
                sh_decl_tree_member(added, "type") && !strcmp(node_value(sh_decl_tree_member(added, "type")), "float");
            if (!placed && child) fprintf(stderr, "withheld gap %d child: %.*s\n", order, (int)child->body_length, (char *)child->body);
            sh_decl_tree_free(tree);
            assert(placed);
            /* three's conductor keeps its first T5 modifier, as the native
             * first-match reads it; the unreachable second row is dropped. */
            const sh_compiled_resource *conductor = named_resource(healthy.library.compiled, "entitydef", "conductor");
            int first_match = conductor && memchr_text(conductor->body, conductor->body_length, "healthMod = \"a\"") &&
                !memchr_text(conductor->body, conductor->body_length, "healthMod = \"b\"");
            if (!first_match && conductor) fprintf(stderr, "withheld gap %d conductor: %.*s\n", order,
                (int)conductor->body_length, (char *)conductor->body);
            assert(first_match);
        }
        owners_done(&healthy);
        /* The relaid parent with nothing found: one's layout (armor first)
         * and both patches compose. Each is rebased onto the published parent
         * by control identity, m-label's label following health to slot 1;
         * m-nameless's control has no identity field, which the native reader
         * allows (it places it by slot), so its slot in the published frame is
         * its identity and it follows the published controls. The slot never
         * reaches the composed text. */
        sh_package_sources_free(sources);
        owners_file(one, "snapeditorentitydef/d_row.decl", owners_stock_standalone);
        sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        assert(sh_package_sources_remove(sources, first_package(sources, three)));
        {
            owners_run gap = {0};
            owners_compile(f, registry, sources, NULL, 1, &gap);
            owners_found(sources, &gap, found, sizeof(found));
            if (!gap.ok || found[0] || gap.more.count)
                fprintf(stderr, "relaid gap %d: ok=%d found=%s %s\n", order, gap.ok, found, gap.error);
            assert(gap.ok && !found[0] && !gap.more.count);
            const sh_compiled_resource *child = named_resource(gap.library.compiled, "snapeditorentitydef", "demo_child");
            sh_decl_node *tree = child ? sh_decl_tree_parse((sh_decl_source){(char *)child->body, child->body_length}, NULL, 0) : NULL;
            const sh_decl_node *properties = sh_decl_tree_member(sh_decl_tree_member(sh_decl_tree_member(
                sh_decl_tree_member(tree, "edit"), "propertySheets"), "item[0]"), "properties");
            const sh_decl_node *armor = sh_decl_tree_member(properties, "item[0]");
            const sh_decl_node *health = sh_decl_tree_member(properties, "item[1]");
            const sh_decl_node *speed = sh_decl_tree_member(properties, "item[2]");
            const sh_decl_node *added = sh_decl_tree_member(properties, "item[3]");
            const sh_decl_node *tag = sh_decl_tree_member(health, "descriptionTag");
            int placed = properties && !strcmp(node_value(sh_decl_tree_member(properties, "num")), "4") &&
                armor && !strcmp(node_value(sh_decl_tree_member(armor, "path")), "armor") &&
                health && !strcmp(node_value(sh_decl_tree_member(health, "path")), "health") &&
                tag && !strcmp(node_value(tag), "#str_health") &&
                speed && !strcmp(node_value(sh_decl_tree_member(speed, "path")), "speed") &&
                added && !sh_decl_tree_member(added, "path") && sh_decl_tree_member(added, "type") &&
                !strcmp(node_value(sh_decl_tree_member(added, "type")), "float") &&
                !memchr_text(child->body, child->body_length, "#snapmap-plus-slot:");
            if (!placed && child) fprintf(stderr, "relaid gap %d child: %.*s\n", order, (int)child->body_length, (char *)child->body);
            sh_decl_tree_free(tree);
            assert(placed);
            owners_done(&gap);
        }
        sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    /* 4. The environment failing after a package was found blames nobody:
     * the second judgement of a group that found one fails as an allocation
     * would, a later group's source is unreadable, or a later step fails
     * without any judgement. No package is reported. A caller without
     * more_rejections still gets exactly the first finding. The findings are
     * judged where the declaration's group composes (the contribution-defect
     * seam over native-valid restatements; a declaration malformed in its own
     * bytes is found earlier, while the sources are read, before any of these
     * steps). */
    for (int fault = 0; fault < 4; fault++) {
        static const char *const defective[2] = {"a-one", "z-two"};
        registration_root("owners-environment");
        owners_package("a-one"); owners_file("a-one", "snapeditorentitydef/demo_parent.decl", owners_layout);
        owners_package("z-two"); owners_file("z-two", "snapeditorentitydef/demo_parent.decl", owners_stock_standalone);
        owners_package("m-label"); owners_file("m-label", "snapeditorentitydef/demo_child.decl", owners_label);
        sh_package_sources *sources = sh_package_sources_scan(root, found, sizeof(found)); assert(sources);
        owners_run run = {0};
        if (fault == 0) sh_package_library_test_rejudge_failures(1);
        if (fault == 1) sh_package_library_test_contribution_read_failures("demo_child", 1);
        if (fault == 2) sh_package_library_test_token_failures(1);
        sh_package_library_test_contribution_defects(defective, 2);
        owners_compile(f, registry, sources, NULL, fault != 3, &run);
        sh_package_library_test_contribution_defects(NULL, 0);
        sh_package_library_test_rejudge_failures(0); sh_package_library_test_contribution_read_failures(NULL, 0);
        sh_package_library_test_token_failures(0);
        owners_found(sources, &run, found, sizeof(found));
        int expected = fault == 3 ? !run.ok && run.invalid == first_package(sources, "a-one") && run.unserved == SIZE_MAX :
            !run.ok && run.invalid == SIZE_MAX && run.unserved == SIZE_MAX && !run.more.count && !run.own;
        if (!expected) fprintf(stderr, "owners then environment %d: ok=%d found=%s %s\n", fault, run.ok, found, run.error);
        assert(expected);
        owners_done(&run);
        sh_package_sources_free(sources); cleanup(); assert(!failures);
    }
    for (size_t i = 0; i < f->count; i++) free(f->regions[i].address);
}
int main(void)
{
    fixture f;
    sh_compiled_resource resource = {0};
    sh_package_compilation compiled = {0};
    sh_package_references paths = {0};
    sh_package_source_graph_report report;
    sh_resource_graph_frame frame;
    char error[1024];
    const char *json = "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\",\"editorVars\":{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"editor-child\"}}";
    test_strings();
    test_particle_link();
    {
        fixture scripts; setup(&scripts);
        test_script_bind(&scripts, (sh_decl_registry_source){&scripts, read_memory, (uintptr_t)scripts.registry, NULL});
        while (scripts.count) free(scripts.regions[--scripts.count].address);
    }
    setup(&f);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    test_late_required_resources(&f, registry);
    test_source_link(&f, registry);
    test_atlas_sources(&f, registry);
    test_canonicalizer_scopes(&f, registry);
    test_composed_stock_default(&f, registry);
    test_campaign_original_binding(&f, registry);
    test_implicit_source_link(&f, registry);
    test_library_replay(&f, registry);
    test_source_link_replay(&f, registry);
    test_source_memo(&f, registry);
    test_library_memo(&f, registry);
    test_font_source_link(&f, registry);
    test_font_library(&f, registry);
    test_movie_source_link();
    test_preview_model();
    test_registration_lists();
    test_registration_variants();
    test_registration_modifier_precedence();
    test_registration_row_order();
    test_registration_mixed_lists();
    test_registration_modifier_order();
    test_registration_named_modifier_scope();
    test_registration_inherited_lists();
    test_registration_discovered_inheritance();
    test_registration_first_match();
    test_registration_first_registration();
    test_registration_capacity();
    test_registration_private_variants();
    test_registration_stock_inspector();
    test_registration_private_capacity();
    test_registration_private_hybrid();
    test_registration_marked_groups();
    test_registration_group_capacity();
    test_registration_pinned_keys();
    test_registration_historical_keys();
    test_registration_historical_conductor();
    test_registration_context_names();
    test_registration_keys();
    test_contribution_movie_token();
    test_open_effect_body();
    test_map_occurrences(&f, registry);
    test_library(&f, registry);
    test_library_hud();
    test_sound_original_binding();
    test_sound_program_library();
    test_sound_inventory_library();
    test_bankless_stock_sound();
    test_absent_declaration_defaults();
    test_contribution_projection();
    test_bound_contributions(&f, registry);
    test_inherited_editor_parent(&f, registry);
    test_inherited_reflected_state(&f, registry);
    test_editor_parent_contracts(&f, registry);
    test_editor_membership_order(&f, registry);
    test_unchanged_editor_copies(&f, registry);
    test_editor_rebase_scopes(&f, registry);
    test_contribution_owners_together();
    test_contribution_anonymous_controls();
    test_contribution_inherited_rekey();
    test_contribution_stale_reference();
    test_native_original_gaps(&f, registry);
    test_material_path(&f, registry);
    test_library_atlas(&f, registry);
    test_toybox_label_aliases(&f, registry);
    {
        sh_decl_native_registry *r = sh_decl_native_registry_open(registry, error, sizeof(error));
        sh_decl_native_schema *s = sh_decl_native_schema_open(
            (sh_decl_native_source){&f, read_memory, (uintptr_t)f.reflection},
            (sh_decl_dependency_observer){0}, error, sizeof(error));
        sh_decl_native_reference reference = {0}; char *bound;
        assert(r && s);
        assert(sh_decl_native_reference_read(s, r, (sh_decl_value_type){"idImage", "*"},
            "/Images\\BODY", 12, &reference) == 1);
        assert(!strcmp(reference.resource.type, "image") && !strcmp(reference.resource.name, "images/body"));
        bound = sh_decl_native_reference_bind(&reference, "images/scoped/body");
        assert(bound && !strcmp(bound, "images/scoped/body")); free(bound);
        assert(sh_decl_native_reference_read(s, r, (sh_decl_value_type){"idRenderModel", "*"},
            "effects/fire.prt", 16, &reference) == 1);
        assert(!strcmp(reference.resource.type, "particle") && !strcmp(reference.resource.name, "effects/fire"));
        bound = sh_decl_native_reference_bind(&reference, "scoped/fire");
        assert(bound && !strcmp(bound, "scoped/fire.prt")); free(bound);
        assert(sh_decl_native_reference_read(s, r, (sh_decl_value_type){"idRenderModel", "*"},
            "_world", 6, &reference) == 0 && !reference.resource.name);
        assert(sh_decl_native_reference_read(s, r, (sh_decl_value_type){"idStaticModel", "*"},
            "effects/fire.prt", 16, &reference) == 1);
        assert(!strcmp(reference.resource.type, "model") && !strcmp(reference.resource.name, "effects/fire.prt"));
        bound = sh_decl_native_reference_bind(&reference, "scoped/model.lwo");
        assert(bound && !strcmp(bound, "scoped/model.lwo")); free(bound);
        assert(sh_decl_native_reference_read(s, r, (sh_decl_value_type){"idStaticModel", "*"},
            "", 0, &reference) == 0 && !reference.resource.name);
        assert(sh_decl_native_reference_read(s, r, (sh_decl_value_type){"Entity", "*"},
            "unknown", 7, &reference) == -1 && !reference.resource.name);
        sh_decl_native_reference_clear(&reference);
        sh_decl_native_registry_close(r); sh_decl_native_schema_close(s);
    }
    resource.engine_path = "generated/decls/entitydef/root.decl";
    resource.body = (unsigned char *)"{ inherit = \"base\"; editorVars { target = \"editor-child\"; } edit = { target = \"map-child\"; custom = \"unknown\"; modifier = \"mod\"; preview = \"preview\"; } }";
    resource.body_length = strlen((const char *)resource.body);
    compiled.resources = &resource; compiled.resource_count = 1;
    test_bind(&f, &compiled, registry);
    test_af_link(&f, registry);
    test_breakable_link(&f, registry);
    test_animation_web_link(&f, registry);
    sh_resource_graph_begin(&frame, "entitydef", "root");
    sh_resource_graph_reference("entitydef", "local-only"); sh_resource_graph_end(&frame, 1);
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(paths.incomplete && report.declarations == 4 && report.gaps >= 1);
    assert(has(&paths, "map-child.decl") && has(&paths, "modifier-child.decl") && has(&paths, "base.decl"));
    assert(!has(&paths, "local-only") && !has(&paths, "editor-child") && !has(&paths, "preview.decl"));
    assert(!has(&paths, "parent-only")); /* Child target replaces this inherited reference. */
    assert(!f.object_state_reads);
    sh_resource_graph_walk("entitydef", "root", observed_edge, NULL); assert(observed == 1);
    /* Inspection follows a second compiled snapshot, independent of the native
     * object's unchanged local prepared state and the previous walk. */
    resource.body = (unsigned char *)"{ class = \"Entity\"; edit = { target = \"local-only\"; } }";
    resource.body_length = strlen((const char *)resource.body);
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(paths.count == 2 && has(&paths, "local-only") && !has(&paths, "map-child"));
    /* Missing inline class is resolved from candidate source, never native state. */
    json = "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"root\",\"state\":{\"edit\":{\"target\":\"map-child\"}}}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)) && has(&paths, "map-child"));
    assert(!f.object_state_reads);
    f.unreadable = "generated/decls/entitydef/local-only.decl";
    assert(!sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)) && !paths.count);
    f.unreadable = NULL;
    json = "{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"chain/0\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)) && paths.count == 1024 && report.declarations == 1024);
    assert(!sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        "{", 1, &paths, &report, error, sizeof(error)) && !paths.count);
    /* A leaf still needs its resource path. Use the verified generated image
     * spelling instead of manufacturing a declaration path or omitting it. */
    json = "{\"targetType\":\"idImage\",\"value\":\"image/body\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)) && paths.count == 1 && !report.gaps);
    assert(has(&paths, "generated/image/image/body.bimage") && !has(&paths, "decls/image"));
    assert(!paths.incomplete);
    json = "{\"targetType\":\"idImage\",\"value\":\"image/absent\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(paths.incomplete && has(&paths, "generated/image/image/absent.bimage"));
    assert(strstr(report.first_gap, "candidate and original resource sources are absent"));
    /* With no catalogued or supplied file, map coverage admits what the library
     * link admits: a verified engine-created image and a SnapMap row registered
     * without data. An unlisted name or a campaign-only row keeps its gap. */
    {
        sh_package_source_admission admission = {NULL, admit_intrinsic};
        char temporary[MAX_PATH], base[MAX_PATH + 32];
        json = "{\"targetType\":\"idImage\",\"value\":\"_black\"}";
        assert(sh_package_source_graph_with_contracts(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
            NULL, NULL, &admission, json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(!paths.count && !report.gaps && !paths.incomplete);
        assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)) && paths.incomplete);
        json = "{\"targetType\":\"idImage\",\"value\":\"_unlisted\"}";
        assert(sh_package_source_graph_with_contracts(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
            NULL, NULL, &admission, json, strlen(json), &paths, &report, error, sizeof(error)) && paths.incomplete);
        assert(GetTempPathA(sizeof(temporary), temporary));
        snprintf(root, sizeof(root), "%ssmp-fileless-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
        assert(CreateDirectoryA(root, NULL));
        fileless_catalog("base/snap_gameresources.pindex", "textures/registered");
        fileless_catalog("base/gameresources.pindex", "textures/campaign");
        create("base/snap_gameresources.resources", ""); create("base/snap_gameresources.patch", "");
        create("base/gameresources.resources", ""); create("base/gameresources.patch", "");
        snprintf(base, sizeof(base), "%s/base", root);
        sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error)); assert(catalog);
        json = "{\"targetType\":\"idImage\",\"value\":\"textures/registered\"}";
        assert(sh_package_source_graph(&compiled, catalog, original, &f, registry, (uintptr_t)f.reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(!paths.count && !report.gaps && !paths.incomplete);
        json = "{\"targetType\":\"idImage\",\"value\":\"textures/campaign\"}";
        assert(sh_package_source_graph(&compiled, catalog, original, &f, registry, (uintptr_t)f.reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)) && paths.incomplete);
        sh_resource_catalog_close(catalog); cleanup();
    }
    json = "{\"targetType\":\"idImage\",\"value\":\"image/body\"}";
    /* Private binary identities retain all compiler-assigned companions. An
     * unrelated payload in the same complete compilation is not required. */
    {
        sh_compiled_resource images[3] = {0};
        images[0].engine_path = "generated/image/smp/body.bimage";
        images[1].engine_path = "generated/image/smp/body_mip.bimage";
        images[2].engine_path = "generated/image/smp/unused.bimage";
        for (size_t i = 0; i < 3; i++) {
            images[i].native_type = "image";
            images[i].native_name = i == 2 ? "unused" : "image/body";
        }
        compiled.resources = images; compiled.resource_count = 3;
        assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(paths.count == 2 && !report.gaps && !paths.incomplete);
        assert(has(&paths, "smp/body.bimage") && has(&paths, "smp/body_mip.bimage") && !has(&paths, "unused"));
        compiled.resources = &resource; compiled.resource_count = 1;
    }
    /* Reflected render-model fields use the native extension dispatcher. This
     * reaches MD6 dependencies instead of inventing a static bmodel path. */
    json = "{\"targetType\":\"idDeclEntityDef\",\"className\":\"Entity\",\"state\":{\"edit\":{\"model\":\"boss.decl\",\"image\":\"image/body\"}}}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(has(&paths, "md6def/boss.decl.decl") && !has(&paths, "model/boss"));
    json = "{\"targetType\":\"idDeclEntityDef\",\"className\":\"Entity\",\"state\":{\"edit\":{\"model\":\"_world\"}}}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)) && !paths.count && !report.gaps);
    /* Custom material source retains parameter schemas and table sources.
     * Typed image names never manufacture a generated/decls/image path, and
     * String content never creates an image edge just from its spelling. */
    json = "{\"targetType\":\"idMaterial\",\"value\":\"boss\"}";
    resource.engine_path = "generated/decls/material/boss.decl";
    resource.body = (unsigned char *)"{ color curve[color] tex models/boss/body\n label models/imaginary-image\n}";
    resource.body_length = strlen((const char *)resource.body);
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(has(&paths, "material/boss.decl") && has(&paths, "renderparm/color.decl"));
    assert(has(&paths, "renderparm/tex.decl") && has(&paths, "table/curve.decl"));
    assert(has(&paths, "renderparm/label.decl") && !has(&paths, "imaginary-image"));
    /* Seven references include the parameter's default image: its declaration
     * loads that default even when the material later supplies another image. */
    assert(!has(&paths, "decls/image/") && report.references == 7 && !f.object_state_reads);
    /* Source schema reads prefer the map's prospective resource over baseline. */
    {
        sh_compiled_resource pair[2] = {resource, {0}};
        pair[0].body = (unsigned char *)"{ color models/boss/body }";
        pair[0].body_length = strlen((const char *)pair[0].body);
        pair[1].engine_path = "generated/decls/renderparm/color.decl";
        pair[1].body = (unsigned char *)"{ Tex2D }";
        pair[1].body_length = strlen((const char *)pair[1].body);
        compiled.resources = pair; compiled.resource_count = 2;
        assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(report.references == 2 && has(&paths, "renderparm/color.decl") && !has(&paths, "table/curve.decl"));
    }
    compiled.resources = &resource; compiled.resource_count = 1;
    /* A cooked animation names its skeleton. That leaf still needs a catalog
     * or compiler payload identity before the closure can be complete. */
    json = "{\"targetType\":\"idDeclMD6\",\"value\":\"boss\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(has(&paths, "cooked/anim/md6/boss/motion/idle.bmd6anim"));
    assert(!has(&paths, "decls/skeleton") && !has(&paths, "decls/anim"));
    /* A per-skeleton default clip carries a placeholder, so the skeleton comes
     * from its identity instead. Both routes are covered. */
    json = "{\"targetType\":\"idDeclMD6\",\"value\":\"default-clip\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(has(&paths, "cooked/anim/md6/boss/boss.md6skl/_default.bmd6anim"));
    /* The legacy graph call has no renderer/ABI contract. It reports coverage
     * explicitly; complete binary-derived includes are tested by shader_link. */
    json = "{\"targetType\":\"idDeclRenderProg\",\"value\":\"glow\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(paths.incomplete && report.gaps);
    assert(strstr(report.first_gap, "renderer contract"));
    json = "{\"targetType\":\"idDeclRenderParm\",\"value\":\"buffer\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(has(&paths, "renderparm/buffer.decl") && has(&paths, "renderparm/boss_t.decl") && report.references == 1);
    assert(report.declarations == 2 && !report.gaps && !f.object_state_reads);
    /* Nothing was left to an adapter here, so this closure is not incomplete. */
    assert(!paths.incomplete);
    /* An MD6 definition names its inherited definition, its mesh and one
     * animation per alias. The cooked binaries are resolved from the catalog,
     * so they contribute their own known paths, not a manufactured decl path. */
    json = "{\"targetType\":\"idDeclMD6\",\"value\":\"boss\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(has(&paths, "md6def/boss.decl") && has(&paths, "md6def/demons/base.decl"));
    assert(!has(&paths, "decls/basemodel") && !has(&paths, "decls/anim"));
    /* Four identities from the child (inherit, mesh, two alias animations), the
     * inherited definition's own mesh, the skeleton each cooked clip names, and
     * the material one animation event argument resolves, which brings its own
     * render parameter and table with it. The event's string and enum arguments
     * name no resource. */
    assert(has(&paths, "material/boss.decl") && has(&paths, "renderparm/color.decl"));
    assert(!has(&paths, "decls/string") && !has(&paths, "fxCondition"));
    assert(report.declarations == 7 && report.references == 15 && !f.object_state_reads);
    /* Both meshes are absent and the skeleton lacks a verified path. The
     * numeric table is inspected completely without creating a false gap. */
    assert(report.gaps == 3 && paths.incomplete);
    json = "{\"targetType\":\"idDeclMD6\",\"value\":\"broken\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)));
    assert(has(&paths, "md6def/broken.decl") && report.gaps == 1 && paths.incomplete);
    /* A mesh the provider supplies is read through the same cooked reader: its
     * skeleton and material identities become references of their own, and the
     * material resolves to a real declaration source. */
    {
        sh_compiled_resource material = {0};
        material.engine_path = "generated/decls/material/boss.decl";
        material.body = (unsigned char *)"{ color curve[color] }";
        material.body_length = strlen((const char *)material.body);
        compiled.resources = &material; compiled.resource_count = 1;
        json = "{\"targetType\":\"idDeclMD6\",\"value\":\"mesh-only\"}";
        assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
            json, strlen(json), &paths, &report, error, sizeof(error)));
        assert(has(&paths, "basemodel/md6/boss/boss.bmd6model"));
        assert(has(&paths, "material/boss.decl") && has(&paths, "renderparm/color.decl"));
        /* A skeleton is a leaf, but the fixture supplies no payload identity. */
        assert(report.gaps == 1 && paths.incomplete && !has(&paths, "decls/skeleton"));
        assert(strstr(report.first_gap, "skeleton") && strstr(report.first_gap, "payload path"));
        {
            sh_compiled_resource pair[2] = {{0}, material};
            pair[0].engine_path = "fixture/registered-skeleton.bytes";
            pair[0].native_type = "skeleton"; pair[0].native_name = "md6/boss/boss.md6skl";
            compiled.resources = pair; compiled.resource_count = 2;
            assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
                json, strlen(json), &paths, &report, error, sizeof(error)));
            assert(!report.gaps && !paths.incomplete && has(&paths, "fixture/registered-skeleton.bytes"));
        }
        compiled.resources = &resource; compiled.resource_count = 1;
    }
    json = "{\"targetType\":\"idDeclRenderParm\",\"value\":\"buffer\"}";
    json = "{\"targetType\":\"idMaterial\",\"value\":\"boss\"}";
    f.unreadable = "generated/decls/renderparm/color.decl";
    assert(!sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)) && !paths.count);
    f.unreadable = NULL;
    {
        unsigned char bank[49], unrelated[49], localized[49];
        sh_compiled_resource banks[3] = {0};
        sh_package_owners owners = {0};
        int complete;
        audio_bank(bank,84696446,1811189196,0);
        audio_bank(unrelated,84696445,13,0);
        audio_bank(localized,84696446,1811189196,12);
        banks[0].engine_path="sound/soundbanks/pc/a.bnk"; banks[0].body=bank;
        banks[1].engine_path="sound/soundbanks/pc/b.bnk"; banks[1].body=unrelated;
        banks[2].engine_path="sound/soundbanks/pc/english(us)/a.bnk"; banks[2].body=localized;
        for(size_t i=0;i<3;i++) banks[i].body_length=49;
        banks[0].gameplay_owners.bits=3; /* Two intact contributors to the same file. */
        banks[1].gameplay_owners.bits=4; /* Installed but unused by this map. */
        banks[2].gameplay_owners.bits=1;
        compiled.resources=banks; compiled.resource_count=3;
        json="{\"targetType\":\"idSoundShader\",\"value\":\"effects/sample\"}";
        assert(sh_package_source_graph(&compiled,NULL,original,&f,registry,(uintptr_t)f.reflection,
            json,strlen(json),&paths,&report,error,sizeof(error)));
        assert(has(&paths,"pc/a.bnk") && has(&paths,"english(us)/a.bnk") && !has(&paths,"pc/b.bnk"));
        assert(report.references==2 && !f.object_state_reads);
        assert(paths.incomplete && strstr(report.first_gap, "complete audio event dependency coverage"));
        {
            char temporary[MAX_PATH],empty[MAX_PATH];
            assert(GetTempPathA(sizeof(temporary),temporary) && GetTempFileNameA(temporary,"asg",0,empty));
            assert(DeleteFileA(empty) && CreateDirectoryA(empty,NULL));
            sh_audio_originals *originals=sh_audio_originals_open(empty,L"English(US)",NULL);assert(originals);
            sh_package_source_audio audio={originals};
            fixture sound_fixture; setup_sound(&sound_fixture);
            sh_decl_registry_source sound_registry={&sound_fixture,read_memory,(uintptr_t)sound_fixture.registry,NULL};
            assert(sh_package_source_graph_with_contracts(&compiled,NULL,sound_original,&sound_fixture,
                sound_registry,(uintptr_t)sound_fixture.reflection,
                NULL,&audio,NULL,json,strlen(json),&paths,&report,error,sizeof(error)));
            if(paths.incomplete || report.gaps || report.references!=1)
                fprintf(stderr,"audio source closure: references=%zu gaps=%zu %s\n",report.references,report.gaps,report.first_gap);
            assert(!paths.incomplete && !report.gaps && report.references==1);
            assert(has(&paths,"english(us)/a.bnk") && !has(&paths,"pc/a.bnk") && !has(&paths,"pc/b.bnk"));
            {
                /* A SnapMap sound no bank implements (the stock "default") keeps
                 * its native binding: nothing plays and nothing is required. The
                 * same event authored by a package declaration needs its bank. */
                const char *bankless="{\"targetType\":\"idSoundShader\",\"value\":\"default\"}";
                assert(sh_package_source_graph_with_contracts(&compiled,NULL,sound_original,&sound_fixture,
                    sound_registry,(uintptr_t)sound_fixture.reflection,
                    NULL,&audio,NULL,bankless,strlen(bankless),&paths,&report,error,sizeof(error)));
                assert(!paths.incomplete && !report.gaps && !report.references);
                assert(paths.count==1 && has(&paths,"sound/default.decl"));
                sh_compiled_resource authored[4]={0};
                authored[0].engine_path="generated/decls/sound/default.decl";
                authored[0].body=(unsigned char *)"{ }"; authored[0].body_length=3;
                authored[0].type=authored[0].native_type="sound";
                authored[0].name=authored[0].native_name="default";
                assert(sh_audio_event_identity("default",&authored[0].sound_authored_event));
                authored[0].sound_event=authored[0].sound_authored_event;
                for(size_t i=0;i<3;i++) authored[i+1]=banks[i];
                compiled.resources=authored; compiled.resource_count=4;
                assert(!sh_package_source_graph_with_contracts(&compiled,NULL,sound_original,&sound_fixture,
                    sound_registry,(uintptr_t)sound_fixture.reflection,
                    NULL,&audio,NULL,bankless,strlen(bankless),&paths,&report,error,sizeof(error)));
                assert(strstr(error,"has no bank in its source scope"));
                compiled.resources=banks; compiled.resource_count=3;
            }
            sh_audio_originals_close(originals);assert(RemoveDirectoryA(empty));
            for(size_t i=0;i<sound_fixture.count;i++) free(sound_fixture.regions[i].address);
            /* Preserve the legacy API's explicit incomplete inventory for its
             * following ownership assertions; only the configured route claims closure. */
            assert(sh_package_source_graph(&compiled,NULL,original,&f,registry,(uintptr_t)f.reflection,
                json,strlen(json),&paths,&report,error,sizeof(error)));
        }
        assert(sh_package_map_owners(&compiled,NULL,json,strlen(json),&paths,&owners,&complete));
        assert(owners.bits==3); /* A bank-only replacement selects both supplying packages. */
        sh_package_owners_free(&owners);
        banks[0].gameplay_owners.bits=0; banks[2].gameplay_owners.bits=0;
        assert(sh_package_map_owners(&compiled,NULL,json,strlen(json),&paths,&owners,&complete));
        assert(!sh_package_owners_count(&owners)); /* Known unchanged stock bytes have no delivery owner. */
        json="{\"targetType\":\"idSoundShader\",\"value\":\"effects/unused\"}";
        assert(sh_package_source_graph(&compiled,NULL,original,&f,registry,(uintptr_t)f.reflection,
            json,strlen(json),&paths,&report,error,sizeof(error)) && !paths.count);
        assert(paths.incomplete && strstr(report.first_gap, "event bank membership"));
        json="{\"editorVars\":{\"targetType\":\"idSoundShader\",\"value\":\"effects/sample\"}}";
        assert(sh_package_source_graph(&compiled,NULL,original,&f,registry,(uintptr_t)f.reflection,
            json,strlen(json),&paths,&report,error,sizeof(error)) && !paths.count);
        sh_package_owners_free(&owners);
        {
            /* A bank streams media from its own files. Those the package supplies
             * travel with it; a payload resident inside the bank does not. */
            unsigned char streaming[160], media[4] = {1,2,3,4};
            sh_compiled_resource with_media[3] = {0};
            size_t size = audio_bank_media(streaming,84696446,1811189196,0,900,901);
            with_media[0].engine_path="sound/soundbanks/pc/a.bnk";
            with_media[0].body=streaming; with_media[0].body_length=size;
            with_media[0].gameplay_owners.bits=1;
            with_media[1].engine_path="sound/soundbanks/pc/900.wem";
            with_media[1].body=media; with_media[1].body_length=sizeof(media);
            with_media[2].engine_path="sound/soundbanks/pc/901.wem";
            with_media[2].body=media; with_media[2].body_length=sizeof(media);
            compiled.resources=with_media; compiled.resource_count=3;
            json="{\"targetType\":\"idSoundShader\",\"value\":\"effects/sample\"}";
            assert(sh_package_source_graph(&compiled,NULL,original,&f,registry,(uintptr_t)f.reflection,
                json,strlen(json),&paths,&report,error,sizeof(error)));
            assert(has(&paths,"pc/a.bnk") && has(&paths,"pc/901.wem") && !has(&paths,"pc/900.wem"));
            assert(report.references==2);
            compiled.resource_count=2; /* No supplied path for the streamed 901. */
            assert(sh_package_source_graph(&compiled,NULL,original,&f,registry,(uintptr_t)f.reflection,
                json,strlen(json),&paths,&report,error,sizeof(error)));
            assert(paths.incomplete && strstr(report.first_gap, "streamed media"));
        }
    }
    sh_package_references_free(&paths); sh_resource_graph_test_reset();
    while (f.count) free(f.regions[--f.count].address);
    puts("package_source_graph_test: PASS"); return 0;
}
