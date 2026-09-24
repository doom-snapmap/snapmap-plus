#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_native_names.h"
#include "swf_binary.h"
#include "font_binary.h"

static void release(char *name, char **paths, size_t count)
{ free(name); if (paths) for (size_t i = 0; i < count; i++) free(paths[i]); free(paths); }
static void mapping(const char *family, const char *original, const char *path, const char *prefix, const char *suffix)
{
    sh_compiled_resource resource = {0}; resource.engine_path = (char *)path;
    const sh_compiled_resource *file = &resource;
    sh_package_link_unit unit = {family, original, NULL, &file, 1};
    sh_package_link_inputs inputs = {&unit, NULL, 1};
    unsigned char identity[32] = {17}; char *name, **paths, error[512];
    int assigned = sh_package_native_names_assign(&inputs, 0, identity, 0, &file, 1, &name, &paths, error, sizeof(error));
    if (!assigned) fprintf(stderr, "%s/%s -> %s: %s\n", family, original, path, error);
    assert(assigned);
    assert(!strncmp(name, prefix, strlen(prefix)) && strlen(name) < 128 && strlen(paths[0]) < 160);
    assert(!strcmp(paths[0] + strlen(paths[0]) - strlen(suffix), suffix));
    if (!strcmp(family, "image")) assert(!strcmp(name, paths[0]));
    char *stable_name = _strdup(name), *stable_path = _strdup(paths[0]);
    release(name, paths, 1);
    assert(sh_package_native_names_assign(&inputs, 0, identity, 0, &file, 1, &name, &paths, error, sizeof(error)));
    assert(!strcmp(stable_name, name) && !strcmp(stable_path, paths[0]));
    release(name, paths, 1);
    identity[31] = 1;
    assert(sh_package_native_names_assign(&inputs, 0, identity, 0, &file, 1, &name, &paths, error, sizeof(error)));
    assert(strcmp(stable_name, name) && strcmp(stable_path, paths[0]));
    release(name, paths, 1); free(stable_name); free(stable_path);
    assert(sh_package_native_names_assign(&inputs, 0, identity, 1, &file, 1, &name, &paths, error, sizeof(error)));
    assert(!strcmp(name, original) && !strcmp(paths[0], path)); release(name, paths, 1);
}

static void consumer_bounds(void)
{
    sh_compiled_resource resource = {0};
    resource.engine_path = "generated/decls/material/generic/baked/surface.decl";
    const sh_compiled_resource *file = &resource;
    sh_package_link_unit unit = {"material", "generic/baked/surface", NULL, &file, 1};
    sh_package_link_inputs inputs = {&unit, NULL, 1};
    unsigned char token[32] = {17};
    char *name, **paths, error[256];
    assert(sh_package_native_names_assign_bounded(&inputs, 0, token, 0, &file, 1,
        64, 90, &name, &paths, error, sizeof(error)));
    assert(strlen(name) < 64 && strlen(paths[0]) < 90 && !strncmp(name, "generic/baked/smp/", 18));
    assert(!strcmp(paths[0] + strlen(paths[0]) - 5, ".decl"));
    char *first = _strdup(name); assert(first); release(name, paths, 1);
    token[0] = 18;
    assert(sh_package_native_names_assign_bounded(&inputs, 0, token, 0, &file, 1,
        64, 90, &name, &paths, error, sizeof(error)) && strcmp(first, name));
    free(first); release(name, paths, 1);
    assert(!sh_package_native_names_assign_bounded(&inputs, 0, token, 0, &file, 1,
        30, 90, &name, &paths, error, sizeof(error)) && strstr(error, "capacity"));
    release(name, paths, 1);
    assert(!sh_package_native_names_assign_bounded(&inputs, 0, token, 0, &file, 1,
        34, 90, &name, &paths, error, sizeof(error)) && strstr(error, "capacity"));
    release(name, paths, 1);
    assert(sh_package_native_names_assign_bounded(&inputs, 0, token, 0, &file, 1,
        35, 90, &name, &paths, error, sizeof(error)) && strlen(name) == 34);
    release(name, paths, 1);
    assert(!sh_package_native_names_assign_bounded(&inputs, 0, token, 1, &file, 1,
        8, 90, &name, &paths, error, sizeof(error)) && strstr(error, "original native name"));
    release(name, paths, 1);
    resource.engine_path = "generated/image/textures/example.bimage";
    unit.family = "image"; unit.name = "textures/example.tga";
    assert(sh_package_native_names_assign_bounded(&inputs, 0, token, 0, &file, 1,
        64, 64, &name, &paths, error, sizeof(error)));
    assert(strlen(name) < 64 && !strcmp(name, paths[0]) && !strcmp(name + strlen(name) - 7, ".bimage"));
    release(name, paths, 1);
}
static void movie_names(void)
{
    sh_compiled_resource resources[2] = {0};
    resources[0].engine_path = "generated/swf/worldgui/panel.bswf";
    resources[1].engine_path = "generated/swf/worldgui/panel.bimage";
    const sh_compiled_resource *files[] = {resources, resources + 1};
    const sh_compiled_resource *sorted[] = {resources + 1, resources};
    sh_package_link_file_identity members[2] = {{0}, {"image", resources[1].engine_path}};
    sh_package_link_unit unit = {"file", resources[0].engine_path, NULL, files, 2}; unit.file_identities = members;
    sh_package_link_inputs inputs = {&unit, NULL, 1};
    unsigned char token[32] = {42}; char *name, **paths, **names, error[512];
    for (int original = 0; original < 2; original++) {
        assert(sh_package_native_names_assign(&inputs, 0, token, original, sorted, 2, &name, &paths, error, sizeof(error)));
        assert(!strcmp(name, paths[1]));
        assert(sh_package_native_names_assign_members(&inputs, 0, name, sorted,
            (const char *const *)paths, 2, &names, error, sizeof(error)));
        assert(!strcmp(names[0], paths[0]) && !strcmp(names[1], paths[1]));
        if (original) assert(!strcmp(name, unit.name) && !strcmp(paths[0], resources[1].engine_path));
        else {
            char source[256]; sh_swf_binary_paths pair;
            assert(!strncmp(name, "generated/swf/smp/", 18));
            assert(strlen(name) < sizeof(source) && strlen(name) > 10);
            snprintf(source, sizeof(source), "%s", name + 10);
            memcpy(source + strlen(source) - 5, ".swf", 5);
            assert(sh_swf_binary_paths_for(source, &pair));
            assert(!strcmp(name, pair.movie) && !strcmp(paths[0], pair.atlas));
        }
        release(NULL, names, 2); release(name, paths, 2);
    }
    assert(sh_package_native_names_assign_bounded(&inputs, 0, token, 0, sorted, 2,
        50, 50, &name, &paths, error, sizeof(error)));
    assert(strlen(name) < 50 && strlen(paths[0]) < 50); release(name, paths, 2);
    assert(!sh_package_native_names_assign_bounded(&inputs, 0, token, 0, sorted, 2,
        20, 50, &name, &paths, error, sizeof(error)) && strstr(error, "capacity")); release(name, paths, 2);
    assert(!sh_package_native_names_assign_bounded(&inputs, 0, token, 1, sorted, 2,
        strlen(unit.name) + 1, 256, &name, &paths, error, sizeof(error)) && strstr(error, "member name"));
    release(name, paths, 2);
    /* The engine replaces the first .bimage substring, even in a directory. */
    unit.name = resources[0].engine_path = "generated/swf/a.bswf";
    resources[1].engine_path = "generated/swf/a.bimage/panel.bimage"; members[1].name = resources[1].engine_path;
    assert(sh_package_native_names_assign(&inputs, 0, token, 1, sorted, 2, &name, &paths, error, sizeof(error)));
    assert(sh_package_native_names_assign_members(&inputs, 0, name, sorted,
        (const char *const *)paths, 2, &names, error, sizeof(error)));
    release(NULL, names, 2); release(name, paths, 2);
    assert(sh_package_native_names_assign(&inputs, 0, token, 0, sorted, 2, &name, &paths, error, sizeof(error)));
    free(paths[0]); paths[0] = _strdup("generated/swf/unrelated.bimage");
    assert(!sh_package_native_names_assign_members(&inputs, 0, name, sorted,
        (const char *const *)paths, 2, &names, error, sizeof(error)) && !names);
    release(name, paths, 2);
    members[1].name = "generated/swf/unrelated.bimage";
    assert(!sh_package_native_names_assign(&inputs, 0, token, 1, sorted, 2, &name, &paths, error, sizeof(error)));
    assert(strstr(error, "paired files")); release(name, paths, 2);
    members[1].name = resources[1].engine_path; members[1].family = "material";
    assert(!sh_package_native_names_assign(&inputs, 0, token, 0, sorted, 2, &name, &paths, error, sizeof(error)));
    release(name, paths, 2);
}

static void font_names(void)
{
    sh_compiled_resource resources[3] = {0};
    resources[0].engine_path = "fonts/example_font_bold/48.dat";
    resources[1].engine_path = "fonts/example_font_bold/64_df.dat";
    resources[2].engine_path = "fonts/example_font_bold/12.dat";
    const sh_compiled_resource *files[] = {resources, resources + 1, resources + 2};
    const sh_compiled_resource *sorted[] = {resources + 2, resources, resources + 1};
    unsigned variants[] = {SH_FONT_BITMAP_12, SH_FONT_BITMAP_48, SH_FONT_DISTANCE_64};
    sh_package_link_unit unit = {"font", "example font-bold", NULL, files, 3};
    sh_package_link_inputs inputs = {&unit, NULL, 1};
    unsigned char token[32] = {42}; char *name, **paths, error[512];
    char *first = NULL;
    for (int original = 0; original < 2; original++) {
        assert(sh_package_native_names_assign(&inputs, 0, token, original, sorted, 3,
            &name, &paths, error, sizeof(error)));
        if (original) assert(!strcmp(name, unit.name));
        else { assert(!strncmp(name, "smp/", 4)); first = _strdup(name); assert(first); }
        for (size_t i = 0; i < 3; i++) {
            sh_font_binary_paths expected;
            assert(sh_font_binary_paths_for(name, (sh_font_binary_variant)variants[i], &expected));
            assert(!strcmp(paths[i], expected.file));
            if (original) assert(!strcmp(paths[i], sorted[i]->engine_path));
            sh_font_binary_paths_clear(&expected);
        }
        release(name, paths, 3);
    }
    assert(sh_package_native_names_assign(&inputs, 0, token, 0, files, 3, &name, &paths, error, sizeof(error)));
    assert(!strcmp(name, first)); release(name, paths, 3);
    token[31] = 1;
    assert(sh_package_native_names_assign(&inputs, 0, token, 0, files, 3, &name, &paths, error, sizeof(error)));
    assert(strcmp(name, first)); release(name, paths, 3); free(first);
    assert(sh_package_native_names_assign_bounded(&inputs, 0, token, 0, files, 3,
        30, 45, &name, &paths, error, sizeof(error)));
    assert(strlen(name) < 30);
    for (size_t i = 0; i < 3; i++) assert(strlen(paths[i]) < 45);
    release(name, paths, 3);
    assert(!sh_package_native_names_assign_bounded(&inputs, 0, token, 0, files, 3,
        20, 45, &name, &paths, error, sizeof(error)) && strstr(error, "capacity"));
    release(name, paths, 3);
    assert(!sh_package_native_names_assign_bounded(&inputs, 0, token, 0, files, 3,
        128, 30, &name, &paths, error, sizeof(error)) && strstr(error, "capacity"));
    release(name, paths, 3);
    resources[1].engine_path = "fonts/other/64_df.dat";
    assert(!sh_package_native_names_assign(&inputs, 0, token, 0, files, 3, &name, &paths, error, sizeof(error)));
    assert(strstr(error, "font files")); release(name, paths, 3);
    resources[1].engine_path = resources[0].engine_path;
    assert(!sh_package_native_names_assign(&inputs, 0, token, 1, files, 3, &name, &paths, error, sizeof(error)));
    release(name, paths, 3);
    unit.file_count = 1;
    assert(sh_package_native_names_assign(&inputs, 0, token, 0, files, 1, &name, &paths, error, sizeof(error)));
    assert(strstr(paths[0], "/48.dat")); release(name, paths, 1);
}

int main(void)
{
    consumer_bounds();
    movie_names();
    font_names();
    mapping("font", "example font-bold", "fonts/example_font_bold/64_df.dat", "smp/", "/64_df.dat");
    mapping("font", "_authored-font", "fonts/_authored_font/64_df.dat", "smp/", "/64_df.dat");
    mapping("material", "_authored", "generated/decls/material/_authored.decl", "smp/", ".decl");
    mapping("model", "_authored.lwo", "cooked/model/_authored.bmodel", "smp/", ".bmodel");
    mapping("entitydef", "props/stone", "generated/decls/entitydef/props/stone.decl", "smp/", ".decl");
    mapping("material", "generic/surface", "generated/decls/material/generic/surface.decl", "generic/smp/", ".decl");
    mapping("material", "generic/baked/surface", "generated/decls/material/generic/baked/surface.decl", "generic/baked/smp/", ".decl");
    mapping("material", "world/baked/surface", "generated/decls/material/world/baked/surface.decl", "baked/smp/", ".decl");
    mapping("material", "world/surface", "generated/decls/material/world/surface.decl", "smp/", ".decl");
    mapping("model", "models/prop.lwo", "cooked/model/models/prop.bmodel", "smp/", ".bmodel");
    mapping("discreteanimation", "models/prop.lwo", "generated/discreteanimation/models/prop.dmodel", "smp/", ".dmodel");
    mapping("staticparticlemodel", "models/particle.lwo", "generated/models/particle.pmodel", "smp/", ".pmodel");
    mapping("basemodel", "md6/prop.md6mesh", "generated/basemodel/md6/prop.bmd6model", "smp/", ".bmd6model");
    mapping("anim", "md6/motion.md6anim", "cooked/anim/md6/motion.bmd6anim", "smp/", ".bmd6anim");
    mapping("image", "textures/surface.tga$clamp$bc7", "generated/image/textures/surface.tga$clamp$bc7.bimage", "generated/image/smp/", ".bimage");
    mapping("image", "generated/swf/icon.bimage", "generated/swf/icon.bimage", "generated/image/smp/", ".bimage");
    sh_compiled_resource resources[2] = {0};
    resources[0].engine_path = "shader/vertex.bin"; resources[1].engine_path = "shader/fragment.bin";
    const sh_compiled_resource *files[] = {resources, resources + 1};
    sh_package_link_unit unit = {"renderprog", "geometry", NULL, files, 2};
    sh_package_link_inputs inputs = {&unit, NULL, 1};
    unsigned char identity[32] = {1}; char *name, **paths, error[512];
    assert(sh_package_native_names_assign(&inputs, 0, identity, 1, files, 2, &name, &paths, error, sizeof(error)));
    assert(!strcmp(name, "geometry") && !strcmp(paths[0], resources[0].engine_path) && !strcmp(paths[1], resources[1].engine_path));
    release(name, paths, 2);
    assert(!sh_package_native_names_assign(&inputs, 0, identity, 0, files, 2, &name, &paths, error, sizeof(error)) && strstr(error, "ABI"));
    release(name, paths, 2);
    unit.family = "model"; unit.name = "models/prop.lwo"; unit.file_count = 1;
    assert(!sh_package_native_names_assign(&inputs, 0, identity, 0, files, 1, &name, &paths, error, sizeof(error)) && strstr(error, "relocation"));
    release(name, paths, 1);
    unit.family = "image"; unit.name = "_white"; unit.file_count = 0;
    assert(sh_package_native_names_assign(&inputs, 0, identity, 1, NULL, 0, &name, &paths, error, sizeof(error)) && !strcmp(name, "_white"));
    release(name, paths, 0);
    assert(!sh_package_native_names_assign(&inputs, 0, identity, 0, NULL, 0, &name, &paths, error, sizeof(error)) && strstr(error, "ABI"));
    release(name, paths, 0);
    /* Image's native generated-file routine has its own underscore rule.
     * Removing a generic name heuristic must not bypass that family contract. */
    unit.family = "image"; unit.name = "_authored.tga"; unit.file_count = 1;
    resources[0].engine_path = "generated/image/_authored.bimage";
    assert(!sh_package_native_names_assign(&inputs, 0, identity, 0, files, 1, &name, &paths, error, sizeof(error)));
    assert(strstr(error, "relocation rule")); release(name, paths, 1);
    unit.family = "material"; unit.name = "../invalid"; unit.file_count = 1;
    assert(!sh_package_native_names_assign(&inputs, 0, identity, 1, files, 1, &name, &paths, error, sizeof(error)));
    release(name, paths, 1);
    puts("package_native_names_test: PASS"); return 0;
}
