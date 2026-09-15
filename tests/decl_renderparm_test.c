#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decl_material.h"
static int failures, missing;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
static int equal(sh_decl_source source, const char *text)
{ return source.length == strlen(text) && (!source.length || !memcmp(source.text, text, source.length)); }
static int resolve(void *context, const char *family, sh_decl_source name, int *kind)
{
    (void)context;
    CHECK(name.text && name.length);
    if (!strcmp(family, "renderparm")) {
        if (missing) return -1;
        if (equal(name, "boss_t")) { *kind = 14; return 1; }
        if (equal(name, "not_struct")) { *kind = 0; return 1; }
        return 0;
    }
    CHECK(!strcmp(family, "image") || !strcmp(family, "renderprog") || !strcmp(family, "sampler"));
    return missing ? -1 : 1;
}
static sh_decl_renderparm read(const char *source)
{
    sh_decl_renderparm out = {0}; char error[512]; sh_decl_material_schema schema = {NULL, resolve};
    int ok = sh_decl_renderparm_read((sh_decl_source){source, strlen(source)}, &schema, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "%s\n", error); CHECK(ok); return out;
}
static void refused(const char *source)
{
    sh_decl_renderparm out = {0}; char error[512]; sh_decl_material_schema schema = {NULL, resolve};
    CHECK(!sh_decl_renderparm_read((sh_decl_source){source, strlen(source)}, &schema, &out, error, sizeof(error)));
    CHECK(!out.tokens.items && !out.references && !out.reference_count && !out.value.text && error[0]);
    sh_decl_renderparm_free(&out);
}
int main(void)
{
    sh_decl_renderparm p = read("{ Vec {-1,2,3,4} Range 0, 1 }");
    CHECK(p.kind == 0 && equal(p.value, "{-1,2,3,4}") && equal(p.edit_specifiers, "Range 0, 1") && !p.reference_count);
    sh_decl_renderparm_free(&p);
    p = read("{ Tex2D clamp bc7 textures/boss/body.tga }");
    CHECK(p.kind == 1 && p.reference_count == 1 && equal(p.references[0].name, "textures/boss/body.tga"));
    CHECK(equal(p.value, "clamp bc7 textures/boss/body.tga") && !p.edit_specifiers.length);
    sh_decl_renderparm_free(&p);
    p = read("{ TexCube \"env\\black_px\" }");
    CHECK(p.kind == 3 && p.reference_count == 1 && equal(p.references[0].name, "env\\black_px"));
    sh_decl_renderparm_free(&p);
    p = read("{ Program boss/lighting }");
    CHECK(p.kind == 11 && p.reference_count == 1 && !strcmp(p.references[0].family, "renderprog"));
    sh_decl_renderparm_free(&p);
    p = read("{ Sampler mipMapSampler0 }");
    CHECK(p.kind == 10 && p.reference_count == 1 && !strcmp(p.references[0].family, "sampler"));
    sh_decl_renderparm_free(&p);
    p = read("{ Program 0 }"); CHECK(p.kind == 11 && !p.reference_count); sh_decl_renderparm_free(&p);
    p = read("{ Sampler \"0\" }"); CHECK(p.kind == 10 && !p.reference_count); sh_decl_renderparm_free(&p);
    /* Default Strings consume to the terminator, with no guessed image edge. */
    p = read("{ String textures/default\n more tokens }");
    CHECK(p.kind == 12 && equal(p.value, "textures/default\n more tokens") && !p.reference_count && !p.edit_specifiers.length);
    sh_decl_renderparm_free(&p);
    p = read("{ String }"); CHECK(p.kind == 12 && !p.value.length && !p.reference_count); sh_decl_renderparm_free(&p);
    refused("{ Vec otherParameter }"); refused("{ Vec }"); refused("{ Tex2D clamp }");
    refused("{ Program }"); refused("{ String \"}\" trailing }");
    p = read("{ StructuredBuffer writable_cs coherent boss_t }");
    CHECK(p.kind == 13 && p.reference_count == 1 && !strcmp(p.references[0].family, "renderparm"));
    CHECK(equal(p.references[0].name, "boss_t") && p.type_name); sh_decl_renderparm_free(&p);
    p = read("{ StructuredBuffer unsigned int }");
    CHECK(!p.reference_count && p.type_name && !strcmp(p.type_name, "unsignedint")); sh_decl_renderparm_free(&p);
    p = read("{ struct { uint alpha[2]; float4 color; } }");
    CHECK(p.kind == 14 && !p.reference_count); sh_decl_renderparm_free(&p);
    p = read("{ UniformBuffer 4096 dynamicOffset }");
    CHECK(p.kind == 15 && !p.reference_count && equal(p.value, "4096")); sh_decl_renderparm_free(&p);
    refused("{ StructuredBuffer not_struct }"); refused("{ StructuredBuffer writable }");
    p = read("{ StructuredBuffer WRITABLE boss_t }");
    CHECK(!p.reference_count && p.type_name && !strcmp(p.type_name, "WRITABLEboss_t")); sh_decl_renderparm_free(&p);
    refused("{ struct { nested { int x; } } }");
    refused("{ UniformBuffer missing }");
    p = read("{ imageStoreBuffer2D EARLY_FRAGMENT_TESTS r11f_g11f_b10f dynamicOffset }");
    CHECK(p.kind == 16 && !p.reference_count && equal(p.value, "EARLY_FRAGMENT_TESTS r11f_g11f_b10f"));
    CHECK(equal(p.edit_specifiers, "dynamicOffset")); sh_decl_renderparm_free(&p);
    p = read("{ imageStoreBuffer3D r8ui }"); CHECK(p.kind == 18 && !p.reference_count); sh_decl_renderparm_free(&p);
    p = read("{ imageStoreBuffer2DArray }"); CHECK(p.kind == 17 && !p.value.length && !p.reference_count); sh_decl_renderparm_free(&p);
    refused("{ Vec 1 } extra");
    missing = 1; refused("{ Tex2D missing/image }");
    refused("{ StructuredBuffer boss_t }");
    p = read("{ Program 0 }"); CHECK(!p.reference_count); sh_decl_renderparm_free(&p);
    printf("renderparm defaults: %s\n", failures ? "FAIL" : "PASS"); return failures ? 1 : 0;
}
