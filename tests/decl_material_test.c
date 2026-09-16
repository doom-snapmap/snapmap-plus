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
    printf("typed material reader: %s\n", failures ? "FAIL" : "PASS"); return failures ? 1 : 0;
}
