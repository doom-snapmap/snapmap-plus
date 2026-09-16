/* Candidate source inspection uses native metadata, never loaded object state. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_source_graph.h"
#include "resource_graph.h"

typedef struct region { void *address; size_t length; } region;
typedef struct fixture {
    region regions[160]; size_t count;
    void *registry, *reflection, *root;
    const char *unreadable;
    size_t object_state_reads;
} fixture;
static void *alloc(fixture *f, size_t n)
{ void *p = calloc(1, n); assert(p && f->count < 160); f->regions[f->count++] = (region){p,n}; return p; }
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
    body[at+13]=0; number(body,(int)at+14,carried); at+=23;
    body[at]=2; number(body,(int)at+1,18); number(body,(int)at+5,602);
    body[at+13]=1; number(body,(int)at+14,streamed); at+=23;
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
        "idDeclTypeInfo", "idDeclActorModifier", "idDeclTypeInfoGraph", "idDeclSnapEditorEntity", "idImage"};
    static const char *types[] = {"entityDef", "actorModifier", "snapEditorEntityDef", "image"};
    static const int indices[] = {0, 4, 6, 7};
    void *managers, *container, *records, *objects, *pointers;
    memset(f, 0, sizeof(*f));
    f->registry = alloc(f, 0x18); managers = alloc(f, 32);
    pointer(f->registry, 8, managers); number(f->registry, 0x10, 4);
    for (int i = 0; i < 4; i++) {
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
    f->reflection = alloc(f, 0xb0); container = alloc(f, 0x30); records = alloc(f, 9 * 0x38);
    objects = alloc(f, 9 * 16); pointers = alloc(f, 9 * 16);
    pointer(f->reflection, 0, container); pointer(container, 0x20, records);
    pointer(f->reflection, 0x88, objects); number(f->reflection, 0x90, 9);
    pointer(f->reflection, 0xa0, pointers); number(f->reflection, 0xa8, 9);
    for (int i = 0; i < 8; i++) {
        pointer(records, i * 0x38, string(f, classes[i]));
        pointer(records, i * 0x38 + 8, string(f, i == 4 || i == 5 || i == 6 ? "idDeclTypeInfo" : ""));
    }
    pointer(pointers, 8, (void *)0x10); pointer(pointers, 4 * 16 + 8, (void *)0x10);
    pointer(pointers, 6 * 16 + 8, (void *)0x10); pointer(objects, 2 * 16 + 8, (void *)0x20);
    pointer(objects, 5 * 16 + 8, (void *)0x30);
    pointer(pointers, 7 * 16 + 8, (void *)0x40);
    {
        const char *t[] = {"idDeclEntityDef", "OpaqueStringReader", "idDeclActorModifier", "idDeclSnapEditorEntity"};
        const char *o[] = {"*", "", "*", "*"}, *n[] = {"target", "custom", "modifier", "preview"};
        fields(f, (char *)records + 0x38, 4, t, o, n);
    }
    {
        const char *t[] = {"idDeclEntityDef"}, *o[] = {"*"}, *n[] = {"target"};
        fields(f, (char *)records + 4 * 0x38, 1, t, o, n);
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

static int original(void *context, const char *path, unsigned char **body, size_t *length)
{
    fixture *f = context;
    const char *text = NULL;
    char generated[160];
    unsigned index;
    *body = NULL; *length = 0;
    if (!strcmp(path, "generated/basemodel/md6/boss/boss.bmd6model")) {
        unsigned char cooked[512];
        size_t size = cooked_mesh(cooked, "md6/boss/boss.md6skl", "boss");
        *body = malloc(size);
        if (!*body) return -1;
        memcpy(*body, cooked, size); *length = size; return 2;
    }
    if (f->unreadable && !strcmp(path, f->unreadable)) return -1;
    if (!strcmp(path, "generated/decls/entitydef/base.decl")) text = "{ class = \"Entity\"; edit = { target = \"parent-only\"; } }";
    else if (!strcmp(path, "generated/decls/entitydef/map-child.decl")) text = "{ class = \"Entity\"; edit = { target = \"root\"; } }";
    else if (!strcmp(path, "generated/decls/entitydef/parent-only.decl") ||
             !strcmp(path, "generated/decls/entitydef/modifier-child.decl") ||
             !strcmp(path, "generated/decls/entitydef/local-only.decl")) text = "{ class = \"Entity\"; edit = {} }";
    else if (!strcmp(path, "generated/decls/actormodifier/mod.decl")) text = "{ edit = { target = \"modifier-child\"; } }";
    else if (!strcmp(path, "generated/decls/snapeditorentitydef/preview.decl")) text = "{ edit = { target = \"editor-child\"; } }";
    else if (!strcmp(path, "generated/decls/renderparm/color.decl")) text = "{ Vec 0 }";
    else if (!strcmp(path, "generated/decls/renderparm/tex.decl")) text = "{ Tex2D textures/default/body }";
    else if (!strcmp(path, "generated/decls/renderparm/label.decl")) text = "{ String }";
    else if (!strcmp(path, "generated/decls/renderparm/buffer.decl")) text = "{ StructuredBuffer writable_cs boss_t }";
    else if (!strcmp(path, "generated/decls/renderparm/boss_t.decl")) text = "{ struct { uint offset; } }";
    else if (!strcmp(path, "generated/decls/table/curve.decl")) text = "{ {0,1} }";
    else if (!strcmp(path, "generated/decls/md6def/mesh-only.decl"))
        text = "{ init { mesh \"md6/boss/boss.md6mesh\" } }";
    else if (!strcmp(path, "generated/decls/md6def/boss.decl"))
        text = "{ init { inherit \"demons/base\" mesh \"md6/boss/absent.md6mesh\" calcRefBoundsFromJoints 1 }"
               " aliases { alias { name \"idle\" anim \"md6/boss/motion/idle.md6anim\" }"
               " alias { name \"attack\" anim \"md6/boss/motion/attack.md6anim\" } } }";
    else if (!strcmp(path, "generated/decls/md6def/demons/base.decl"))
        text = "{ init { mesh \"md6/base/base.md6mesh\" } }";
    else if (!strcmp(path, "generated/decls/md6def/broken.decl")) text = "{ init { unknownField 1 } }";
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
static int observed_edge(void *context, const char *pt, const char *pn, const char *type, const char *name)
{ (void)context; (void)pt; (void)pn; (void)type; if (!strcmp(name, "local-only")) observed++; return 1; }
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
    setup(&f);
    sh_decl_registry_source registry = {&f, read_memory, (uintptr_t)f.registry, NULL};
    resource.engine_path = "generated/decls/entitydef/root.decl";
    resource.body = (unsigned char *)"{ inherit = \"base\"; editorVars { target = \"editor-child\"; } edit = { target = \"map-child\"; custom = \"unknown\"; modifier = \"mod\"; preview = \"preview\"; } }";
    resource.body_length = strlen((const char *)resource.body);
    compiled.resources = &resource; compiled.resource_count = 1;
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
    /* A registered resource manager is not necessarily a declaration manager.
     * Never manufacture generated/decls/image for an image root. */
    json = "{\"targetType\":\"idImage\",\"value\":\"image/body\"}";
    assert(sh_package_source_graph(&compiled, NULL, original, &f, registry, (uintptr_t)f.reflection,
        json, strlen(json), &paths, &report, error, sizeof(error)) && !paths.count && report.gaps == 1);
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
    /* Four identities from the child (inherit, mesh, two alias animations) and
     * the inherited definition's own mesh. */
    assert(report.declarations == 2 && report.references == 5 && !f.object_state_reads);
    /* The cooked mesh and animations have no source reader, and that stays
     * visible: their identities are reported as gaps, not as closure. */
    assert(report.gaps == 4 && paths.incomplete);
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
        /* The skeleton has no reader of its own, so it stays an explicit gap. */
        assert(report.gaps && paths.incomplete && !has(&paths, "decls/skeleton"));
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
        assert(sh_package_map_owners(&compiled,NULL,json,strlen(json),&paths,&owners,&complete));
        assert(owners.bits==3); /* A bank-only replacement selects both supplying packages. */
        sh_package_owners_free(&owners);
        banks[0].gameplay_owners.bits=0; banks[2].gameplay_owners.bits=0;
        assert(sh_package_map_owners(&compiled,NULL,json,strlen(json),&paths,&owners,&complete));
        assert(!sh_package_owners_count(&owners)); /* Known unchanged stock bytes have no delivery owner. */
        json="{\"targetType\":\"idSoundShader\",\"value\":\"effects/unused\"}";
        assert(sh_package_source_graph(&compiled,NULL,original,&f,registry,(uintptr_t)f.reflection,
            json,strlen(json),&paths,&report,error,sizeof(error)) && !paths.count);
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
        }
    }
    sh_package_references_free(&paths); sh_resource_graph_test_reset();
    while (f.count) free(f.regions[--f.count].address);
    puts("package_source_graph_test: PASS"); return 0;
}
