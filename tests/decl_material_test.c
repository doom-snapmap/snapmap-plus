#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decl_material.h"
static int failures, missing;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
static int equal(sh_decl_source source, const char *text)
{ return source.text && source.length == strlen(text) && !memcmp(source.text, text, source.length); }
static int resolve(void *context, const char *family, sh_decl_source name, int *kind)
{
    (void)context;
    *kind = -1;
    if (!strcmp(family, "renderparm")) {
        if (equal(name, "a") || equal(name, "b") || equal(name, "c") || equal(name, "color") || equal(name, "time")) *kind = 0;
        else if (equal(name, "tex")) *kind = 1;
        else if (equal(name, "sample")) *kind = 10;
        else if (equal(name, "program") || equal(name, "interactionProgram")) *kind = 11;
        else if (equal(name, "label")) *kind = 12;
        else if (equal(name, "buffer")) *kind = 13;
        return *kind >= 0;
    }
    if (missing) return -1;
    if (!strcmp(family, "table")) return equal(name, "curve");
    return !strcmp(family, "image") || !strcmp(family, "renderprog") || !strcmp(family, "sampler");
}
static sh_decl_material read(const char *source)
{
    sh_decl_material out = {0}; char error[256];
    sh_decl_material_schema schema = {NULL, resolve};
    int ok = sh_decl_material_read((sh_decl_source){source, strlen(source)}, &schema, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "%s\n", error);
    CHECK(ok); return out;
}
static void refused(const char *source)
{
    sh_decl_material out = {0}; char error[256];
    sh_decl_material_schema schema = {NULL, resolve};
    CHECK(!sh_decl_material_read((sh_decl_source){source, strlen(source)}, &schema, &out, error, sizeof(error)));
    CHECK(!out.count && !out.writes && !out.references && !out.tokens.items && error[0]);
}
/* The material served for material/smp/5dea5f3e... in the 2026-10-22 Vulkan boot: the installed
 * textures/effects/hud/magic_glass_broken_2 with spare2Map rebound to a private image copy (1247 bytes,
 * SHA-256 e9fd5a7982a7f4a45cec70aaa340cbb93c71236424a09820bbfda9664bbca864). Its stageProgram names hud_magic_glass_off, a render program no
 * shipped renderprog declaration carries, so the native parm-block reader stops there and the engine
 * gives it the default; blendProgram hud_magic_blend_broken is never reached. */
static const char magic_glass_broken_2_rebound[] =
        "{\r\n"
        "\t// DO NOT EDIT TEXT DIRECTLY!\r\n"
        "\t//transmap\tbc7  \t\ttextures/effects/misc/hud_hexagons\r\n"
        "\t//transmap \tbc7\t\tmodels/characters/helmet_interior/helmet_grid\r\n"
        "\ttransmap1\thqcompressNormal\ttextures/effects/hud/hud_broken_glass_3\r\n"
        "\ttransmap2\ttextures/luts/digital_artifact_01\r\n"
        "\t//transmap3\ttextures/effects/hud/hud_broken_glass_mask2\r\n"
        "\ttransmap3 \ttextures/system/constant_color/grey_dk\r\n"
        "\r\n"
        "\tspare1Map\thqcompressNormal\ttextures/effects/hud/hud_broken_glass_3\r\n"
        "\tspare2Map\tgenerated/image/smp/fd7afd8520398873745e9ca43ae016c290a6b934239edb939caec4d40332b274.bimage\r\n"
        "\r\n"
        "\tnoshadows\t\t1\r\n"
        "\tnoselfshadow\t1\r\n"
        "\r\n"
        "\ttiling\t\t{ 5.0, 5.0, 2.0, 2.0 }\r\n"
        "\tramp1\t\t1\r\n"
        "\tramp2\t\t2\r\n"
        "\r\n"
        "\t//ambientProgram\t\thud_magic_glass\r\n"
        "\tstageProgram\t\thud_magic_glass_off\r\n"
        "\tblendProgram\t\thud_magic_blend_broken\r\n"
        "\r\n"
        "\t//addProgram\t\t\t\t\t\t\t\t\t\tglassPrototype_helmet_interior_broken\r\n"
        "\ttransMap\t\t\t\t\t\t\t\t\t\tmodels/characters/helmet_interior/helmet_glass_d\r\n"
        "\tspareBumpMap\t\t/*hqcompressnormal*/ \t\tmodels/characters/helmet_interior/helmet_interior_local\r\n"
        "\tspareSpecularMap\t/*bc7*/\t\t\tmodels/characters/helmet_interior/helmet_interior_s\r\n"
        "\tsparePowerMap\t\t/*bc7*/\t\t\tmodels/characters/helmet_interior/helmet_interior_pm\r\n"
        "\tenvMap\t\t\t\ttextures/system/constant_color/black.tga\r\n"
        "\tglassColorTint\t\t{ 0.07, 0.07, 0.07, 0.25 }\r\n"
        "}";
static int glass_resolve(void *context, const char *family, sh_decl_source name, int *kind)
{
    (void)context;
    *kind = -1;
    if (!strcmp(family, "renderparm")) {
        static const char *const images[] = {"transmap1", "transmap2", "transmap3", "spare1Map", "spare2Map",
            "transMap", "spareBumpMap", "spareSpecularMap", "sparePowerMap", "envMap"};
        static const char *const vectors[] = {"noshadows", "noselfshadow", "tiling", "ramp1", "ramp2", "glassColorTint"};
        for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) if (equal(name, images[i])) *kind = 1;
        for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) if (equal(name, vectors[i])) *kind = 0;
        if (equal(name, "stageProgram") || equal(name, "blendProgram") || equal(name, "interactionProgram")) *kind = 11;
        return *kind >= 0;
    }
    return !strcmp(family, "image") || !strcmp(family, "renderprog") || !strcmp(family, "sampler");
}
/* The inventory: every program except the two the magic-glass materials alone name. */
static int glass_known(void *context, const char *family, const char *name)
{
    int *answer = context;
    if (answer && *answer < 0) return -1;
    if (strcmp(family, "renderprog")) return 1;
    return strcmp(name, "hud_magic_glass_off") && strcmp(name, "hud_magic_blend_broken");
}
static void test_native_default(void)
{
    sh_decl_material_schema schema = {NULL, glass_resolve};
    sh_decl_source text = {magic_glass_broken_2_rebound, sizeof(magic_glass_broken_2_rebound) - 1};
    char program[128] = "x", error[256] = "";
    int unknown_answer = -1;
    CHECK(sizeof(magic_glass_broken_2_rebound) - 1 == 1247);
    /* The product's grammar reads the served text; the native parse stops at stageProgram. */
    CHECK(sh_decl_material_native_default(text, &schema, glass_known, NULL, program, sizeof(program), error, sizeof(error)) == 1);
    CHECK(!strcmp(program, "hud_magic_glass_off"));
    /* With every program declared nothing defaults. */
    {
        const char *fine = "{ transmap1 textures/a stageProgram hud_known blendProgram hud_known_2 interactionProgram hud_unknown_ok }";
        sh_decl_source source = {fine, strlen(fine)};
        CHECK(sh_decl_material_native_default(source, &schema, glass_known, NULL, program, sizeof(program), error, sizeof(error)) == 0);
        CHECK(program[0] == 0);
    }
    /* interactionProgram is consumed and ignored natively, so an undeclared one does not default. */
    {
        const char *ignored = "{ interactionProgram hud_magic_glass_off transmap1 textures/a }";
        sh_decl_source source = {ignored, strlen(ignored)};
        CHECK(sh_decl_material_native_default(source, &schema, glass_known, NULL, program, sizeof(program), error, sizeof(error)) == 0);
    }
    /* An inventory that cannot answer, an unreadable text and a missing destination are unknown, never a verdict. */
    CHECK(sh_decl_material_native_default(text, &schema, glass_known, &unknown_answer, program, sizeof(program), error, sizeof(error)) == -1);
    {
        const char *broken = "{ stageProgram }";
        sh_decl_source source = {broken, strlen(broken)};
        CHECK(sh_decl_material_native_default(source, &schema, glass_known, NULL, program, sizeof(program), error, sizeof(error)) == -1);
        CHECK(error[0] != 0);
    }
    CHECK(sh_decl_material_native_default(text, &schema, glass_known, NULL, NULL, 0, error, sizeof(error)) == -1);
    CHECK(sh_decl_material_native_default(text, &schema, glass_known, NULL, program, 4, error, sizeof(error)) == -1);
}
int main(void)
{
    int kind = -1; char error[256];
    const char *parameter = "{ /* prefix */ TeXCube }";
    CHECK(sh_decl_material_parameter_kind((sh_decl_source){parameter, strlen(parameter)}, &kind, error, sizeof(error)) && kind == 3);
    parameter = "{ Vec 0 } extra";
    CHECK(!sh_decl_material_parameter_kind((sh_decl_source){parameter, strlen(parameter)}, &kind, error, sizeof(error)) && kind == -1 && error[0]);
    parameter = "{ Unknown 0 }";
    CHECK(!sh_decl_material_parameter_kind((sh_decl_source){parameter, strlen(parameter)}, &kind, error, sizeof(error)) && kind == -1);
    sh_decl_material m = read("{ a 1 b = {2,3}; c -.5e+2 color {1,2,3,4} }");
    CHECK(m.count == 4 && m.reference_count == 4);
    if (m.count == 4) {
        CHECK(m.writes[0].constant && equal(m.writes[0].components[3], "1"));
        CHECK(m.writes[1].constant && equal(m.writes[1].components[1], "3"));
        CHECK(equal(m.writes[1].components[2], "0") && equal(m.writes[1].components[3], "0"));
        CHECK(equal(m.writes[2].components[0], "-.5e+2"));
        CHECK(equal(m.writes[3].components[3], "4"));
    }
    sh_decl_material_free(&m);
    m = read("{ a (time + 2) * curve[time / 2] b a.zyx c {3} color.xy {1,2} color.z 4 }");
    CHECK(m.count == 5);
    if (m.count == 5) {
        CHECK(!m.writes[0].constant && m.writes[0].reference_count == 4);
        CHECK(equal(m.writes[0].value, "(time + 2) * curve[time / 2]"));
        CHECK(!m.writes[1].constant && m.writes[1].reference_count == 2);
        CHECK(equal(m.writes[2].components[3], "3"));
        CHECK(m.writes[3].mask == 3 && m.writes[4].mask == 4);
        CHECK(equal(m.writes[3].name, "color") && equal(m.writes[4].name, "color"));
    }
    sh_decl_material_free(&m);
    m = read("{\n label \"models/boss/body\\\" extra tokens // comment\n a 1\n tex clamp linear bc7 models/boss/body.tga\n program effect sample nearestSample\n interactionProgram ignored\n}");
    CHECK(m.count == 6);
    if (m.count == 6) {
        CHECK(equal(m.writes[0].value, "\"models/boss/body\\\" extra tokens"));
        CHECK(m.writes[0].reference_count == 1); /* String content has no guessed image edge. */
        CHECK(m.writes[2].reference_count == 2 && m.writes[2].kind == 1);
        CHECK(m.writes[3].kind == 11 && m.writes[4].kind == 10);
        CHECK(m.writes[5].reference_count == 1);
    }
    sh_decl_material_free(&m);
    m = read("{ a\n (time +\n 2)\n b 3 } "); CHECK(m.count == 2); sh_decl_material_free(&m);
    m = read("{}"); CHECK(!m.count); sh_decl_material_free(&m);
    refused("{ unknown 1 }"); refused("{ color.yx 1 }"); refused("{ color.xx 1 }");
    refused("{ color. 1 }"); refused("{ color.X 1 }"); refused("{ a {1,2,3,4,5} }");
    refused("{ a {1,} }"); refused("{ a -time }"); refused("{ a time + }");
    refused("{ a () }"); refused("{ a (time time) }"); refused("{ a curve[] }");
    refused("{ a other[1] }"); refused("{ a tex }"); refused("{ a time.xyzww }");
    refused("{ label value }"); refused("{ label\nvalue\n}"); refused("{ buffer something }");
    /* The native parm-block reader sends every kind outside image, sampler,
     * render program and String through the same expression path a vector write
     * uses, so a buffer write binds through the parameters it names. */
    m = read("{ buffer time buffer.x (a + b) }");
    CHECK(m.count == 2);
    if (m.count == 2) {
        CHECK(m.writes[0].kind == 13 && m.writes[0].reference_count == 2 && !m.writes[0].constant);
        CHECK(m.writes[1].kind == 13 && m.writes[1].mask == 1 && m.writes[1].reference_count == 3);
    }
    sh_decl_material_free(&m);
    m = read("{ a dot3(time, {1,2,3}) b dot4(a, dot3(time, a)) }");
    CHECK(m.count == 2 && m.writes[0].reference_count == 2 && m.writes[1].reference_count == 4);
    sh_decl_material_free(&m);
    refused("{ a dot3(a) }"); refused("{ a dot4(a,b,c) }"); refused("{ a dot3(,b) }");
    refused("{ state {} }");
    missing = 1; refused("{ tex image }"); missing = 0;
    {
        char source[800] = "{";
        for (size_t i = 0; i < 128; i++) strcat_s(source, sizeof(source), " a 1");
        strcat_s(source, sizeof(source), " }");
        m = read(source); CHECK(m.count == 128); sh_decl_material_free(&m);
        source[strlen(source) - 1] = 0; strcat_s(source, sizeof(source), " a 1 }");
        refused(source);
    }
    {
        size_t depth = 8192; char *source = malloc(depth * 2 + 10);
        CHECK(source);
        if (source) {
            memcpy(source, "{ a ", 4); memset(source + 4, '(', depth);
            source[depth + 4] = '1'; memset(source + depth + 5, ')', depth);
            memcpy(source + depth * 2 + 5, " }", 3);
            m = read(source); CHECK(m.count == 1); sh_decl_material_free(&m); free(source);
        }
    }
    test_native_default();
    printf("typed material reader: %s\n", failures ? "FAIL" : "PASS"); return failures ? 1 : 0;
}
