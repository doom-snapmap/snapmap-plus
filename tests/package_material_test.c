#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_material.h"
#include "package_fixture.h"

static const char *material_base = "{ color {1,2,3,4} }";
static const char *color_base = "{ Vec {0,0,0,0} }";
static int unreadable, calls;
static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = NULL; (void)context; *body = NULL; *length = 0;
    if (!strcmp(path, "generated/decls/material/boss.decl")) text = material_base;
    if (!strcmp(path, "generated/decls/renderparm/color.decl")) {
        if (unreadable) return -1;
        text = color_base;
    }
    if (!text) return 0;
    *body = (unsigned char *)_strdup(text); *length = strlen(text); return *body ? 1 : -1;
}
static int custom(void *context, const char *type, sh_decl_source base,
    const sh_decl_source *sources, size_t count, char **body, size_t *length,
    char *error, size_t capacity, sh_decl_conflict *conflict, const sh_package_source_view *view)
{
    (void)context; if (strcmp(type, "material")) return 0;
    calls++;
    *body = sh_package_material_compose(base, sources, count, view, length, error, capacity, conflict);
    return *body ? 1 : -1;
}
static sh_package_compilation *compile(sh_package_sources **sources, char *error,
    const char *product)
{
    sh_package_compile_environment environment = {0};
    sh_package_builtin builtin = {"generated/decls/material/boss.decl", (const unsigned char *)product,
        product ? strlen(product) : 0};
    environment.baseline = baseline; environment.compose_custom = custom;
    if (product) { environment.builtins = &builtin; environment.builtin_count = 1; }
    *sources = sh_package_sources_scan(root, error, 2048); CHECK(*sources);
    return *sources ? sh_package_compile_with(*sources, &environment, error, 2048) : NULL;
}
static void expect(const char *result, const char *diagnostic, const char *product)
{
    char error[2048]; sh_package_sources *sources = NULL;
    sh_package_compilation *compiled = compile(&sources, error, product);
    if (result) {
        if (!compiled) fprintf(stderr, "%s\n", error);
        CHECK(compiled);
        if (compiled) {
            const sh_compiled_resource *r = sh_package_compilation_find(compiled, "generated/decls/material/boss.decl");
            CHECK(r && r->body && strstr((const char *)r->body, result));
            CHECK(r && r->source_count == 2 && r->owners.bits == 3);
        }
    } else { CHECK(!compiled && error[0]); if (diagnostic) CHECK(strstr(error, diagnostic)); }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
}
int main(void)
{
    char temporary[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pma", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    create("overrides/a/support/package.json", "{\"id\":\"support\",\"name\":\"Support\"}");
    create("overrides/a/assets/generated/decls/material/boss.decl", "{ color {9,2,3,4} }");
    create("overrides/b/assets/generated/decls/material/boss.decl", "{ color {1,8,3,4} }");
    create("overrides/a/support/assets/generated/decls/renderparm/color.decl", color_base);
    create("overrides/a/assets/generated/decls/renderparm/color.decl", color_base);
    expect("color { 9, 8, 3, 5 }", NULL, "{ color {1,2,3,5} }");
    CHECK(calls == 1);
    create("overrides/b/assets/generated/decls/material/boss.decl", "{ color {7,2,3,4} }");
    expect(NULL, "value.x", NULL);
    create("overrides/b/assets/generated/decls/material/boss.decl", "{ color {1,8,3,4} }");
    create("overrides/a/assets/generated/decls/renderparm/color.decl", "{ Vec {2,0,0,0} }");
    /* A's changed schema plus B's value authored under the original must not
     * be accepted merely because they change different private tree fields. */
    expect(NULL, "schema", NULL);
    create("overrides/b/assets/generated/decls/renderparm/color.decl", "{ Vec {2,0,0,0} }");
    expect("color { 9, 8, 3, 4 }", NULL, NULL);
    create("overrides/a/support/assets/generated/decls/renderparm/color.decl", "{ Vec {3,0,0,0} }");
    expect(NULL, "incompatible source schemas", NULL);
    create("overrides/a/support/assets/generated/decls/renderparm/color.decl", color_base);
    unreadable = 1; expect(NULL, "unreadable", NULL); unreadable = 0;
    /* A schema supplied only by a third package is a source dependency, and
     * package folder order cannot choose between incompatible peer schemas. */
    material_base = "{}";
    create("overrides/a/assets/generated/decls/material/boss.decl", "{ external 1 }");
    create("overrides/b/assets/generated/decls/material/boss.decl", "{ other 2 }");
    create("overrides/c/package.json", "{\"id\":\"c\",\"name\":\"C\"}");
    create("overrides/c/assets/generated/decls/renderparm/external.decl", "{ Vec 0 }");
    create("overrides/c/assets/generated/decls/renderparm/other.decl", "{ Vec 0 }");
    expect("external { 1, 1, 1, 1 }", NULL, NULL);
    create("overrides/b/assets/generated/decls/renderparm/external.decl", "{ Vec 3 }");
    expect(NULL, "incompatible source schemas", NULL);
    cleanup(); printf("package material composition: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
