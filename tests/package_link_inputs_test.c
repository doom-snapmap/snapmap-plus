/* Synthetic catalog identities and scoped compiler input discovery. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_link_inputs.h"
#include "native_filename.h"
#include "package_fixture.h"

static void be32(unsigned char *out, unsigned n)
{ out[0]=(unsigned char)(n>>24); out[1]=(unsigned char)(n>>16); out[2]=(unsigned char)(n>>8); out[3]=(unsigned char)n; }
static size_t field(unsigned char *out, size_t at, const char *text)
{
    unsigned n = (unsigned)strlen(text);
    out[at]=(unsigned char)n; out[at+1]=(unsigned char)(n>>8); out[at+2]=out[at+3]=0;
    memcpy(out+at+4,text,n); return at+4+n;
}
static void catalog_file(const char *relative)
{
    static const char *rows[][3] = {
        {"renderProg", "shaders/shared", "shader/vertex.bin"},
        {"renderProg", "shaders/shared", "shader/fragment.bin"},
        {"renderProg", "shaders/shared", "shader/fragment.bin"},
        {"renderProg", "shaders/alternate", "shader/vertex.bin"},
        {"model", "models/catalog-special.obj", "cooked/model/models/custom.bmodel"},
        {"image", "images/rock", "generated/image/rock$a00.bimage"},
        {"image", "images/rock", "generated/image/rock$b00.bimage"},
        {"cm", "md6/guard/body.md6", ""},
        {"file", "maps/modules/a/room/lightprobes/light_probe_1_compressed.bimage",
            "maps/modules/a/room/lightprobes/light_probe_1_compressed.bimage"},
        {"snapModule", "maps/modules/a/room.decl", "maps/modules/a/room.decl"},
        {"aas", "maps/modules/a/room/room.aas_monster48", "generated/maps/modules/a/room/room.baas_monster48"}
    };
    unsigned char body[4096] = {0};
    char path[4096]; size_t at = 0x24;
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        be32(body+at,(unsigned)i); at+=4;
        for (size_t j=0;j<3;j++) at=field(body,at,rows[i][j]);
        at+=21; /* Empty synthetic archive slices; no real game data. */
    }
    body[0]=5; memcpy(body+1,"SER",3); be32(body+4,(unsigned)(at-0x20));
    be32(body+0x20,(unsigned)(sizeof(rows)/sizeof(rows[0])));
    create(relative, "");
    snprintf(path,sizeof(path),"%s/%s",root,relative);
    FILE *file = fopen(path,"wb"); assert(file);
    assert(fwrite(body,1,at,file)==at); assert(!fclose(file));
}

static void cooked_paths(void)
{
    char *atlas_path = sh_package_cooked_path("transsortatlas", "textures/fx/beam.tga");
    assert(atlas_path && !strcmp(atlas_path, "generated/transsortatlas/textures/fx/beam.bimage"));
    const char *atlas_family = NULL; char *atlas_name = NULL;
    assert(sh_package_cooked_identity(atlas_path, &atlas_family, &atlas_name) == 1);
    assert(!strcmp(atlas_family, "transsortatlas") && !strcmp(atlas_name, atlas_path));
    char *atlas_roundtrip = sh_package_cooked_path(atlas_family, atlas_name);
    assert(atlas_roundtrip && !strcmp(atlas_roundtrip, atlas_path));
    free(atlas_path); free(atlas_name); free(atlas_roundtrip);
    static const char *particles[][2] = {
        {"models/particles/splash.lwo", "generated/models/particles/splash.pmodel"},
        {"models.v2/particles/splash.v2.obj", "generated/models.v2/particles/splash.v2.pmodel"},
        {"models/particles/part-name (copy).lwo", "generated/models/particles/part-name (copy).pmodel"},
        {"models/particles/splash.lwo$option", "generated/models/particles/splash.pmodel"},
        {"models/particles/splash.pmodel", "generated/models/particles/splash.pmodel"},
        {"generated/models/particles/splash.pmodel", "generated/generated/models/particles/splash.pmodel"},
        {"dir/1234567890123456789012345678901.obj", "generated/dir/123456789012345678901234567890/1.pmodel"},
        {"dir/123456789012345678901234567890.obj", "generated/dir/123456789012345678901234567890.pmodel"},
        {"dir/123456789012345678901234567890.tail.obj", "generated/dir/123456789012345678901234567890/.pmodel"},
        {"dir/123456789012345678901234567890x.tail.obj", "generated/dir/123456789012345678901234567890/x.pmodel"},
        {"dir/1234567890.2345678901234567890123456789.obj", "generated/dir/1234567890.2345678901234567890/123456789.pmodel"},
        {"models/.lwo", "generated/models/.pmodel"}
    };
    for (size_t i = 0; i < sizeof(particles) / sizeof(*particles); i++) {
        char *path = sh_package_cooked_path("staticparticlemodel", particles[i][0]);
        const char *family = NULL; char *name = NULL;
        if (!path || strcmp(path, particles[i][1])) fprintf(stderr, "particle path %s: %s (expected %s)\n",
            particles[i][0], path ? path : "NULL", particles[i][1]);
        assert(path && !strcmp(path, particles[i][1]));
        assert(sh_package_cooked_identity(path, &family, &name) == 1 && !strcmp(family, "staticparticlemodel"));
        char *roundtrip = sh_package_cooked_path(family, name);
        assert(roundtrip && !strcmp(roundtrip, path)); free(roundtrip); free(path); free(name);
    }
    char short_path[64], oversized[300]; memset(oversized, 'x', sizeof(oversized)); oversized[299] = 0;
    assert(sh_native_generated_filename("generated/", "test.lwo", ".pmodel", short_path, 22));
    assert(!strcmp(short_path, "generated/test.pmodel"));
    assert(!sh_native_generated_filename("generated/", "test.lwo", ".pmodel", short_path, 21) && !*short_path);
    assert(!sh_package_cooked_path("staticparticlemodel", oversized));
    assert(!sh_package_cooked_path("staticparticlemodel", "../models/a.lwo"));
    static const char *cases[][3] = {
        {"model", "models/props/rock.lwo", "cooked/model/models/props/rock.bmodel"},
        {"basemodel", "md6/guard/body.md6mesh", "generated/basemodel/md6/guard/body.bmd6model"},
        {"skeleton", "md6/guard/body.md6skl", "generated/skeleton/md6/guard/body.bmd6skl"},
        {"anim", "md6/guard/walk.md6anim", "cooked/anim/md6/guard/walk.bmd6anim"},
        {"discreteanimation", "models/props/pieces.lwo", "generated/discreteanimation/models/props/pieces.dmodel"}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        char *path=sh_package_cooked_path(cases[i][0],cases[i][1]), *name=NULL;
        const char *family=NULL;
        assert(path && !strcmp(path,cases[i][2]));
        assert(sh_package_cooked_identity(path,&family,&name)==1);
        assert(!strcmp(family,cases[i][0]) &&
            !strcmp(name, !strcmp(family, "discreteanimation") ? path : cases[i][1]));
        free(path); free(name);
    }
    char *path=sh_package_cooked_path("model","MODELS\\Props\\Rock.LWO");
    assert(path && !strcmp(path,cases[0][2])); free(path);
    path=sh_package_cooked_path("model","models/props/rock");
    assert(path && !strcmp(path,cases[0][2])); free(path);
    static const char *skeletons[][2] = {
        {"MD6\\Guard\\Body.MD6SKL", "generated/skeleton/md6/guard/body.bmd6skl"},
        {"md6/guard/body", "generated/skeleton/md6/guard/body.bmd6skl"},
        {"md6/guard/body.other", "generated/skeleton/md6/guard/body.bmd6skl"},
        {"md6/guard/body.md6skl$option", "generated/skeleton/md6/guard/body.md6skl$option.bmd6skl"},
        {"md6/red-body (copy).md6skl", "generated/skeleton/md6/red_body__copy_.bmd6skl"},
        {"custom/body.bmd6skl", "custom/body.bmd6skl"},
        {"generated/skeleton/md6/guard/body.bmd6skl", "generated/skeleton/md6/guard/body.bmd6skl"},
        {"md6/1234567890123456789012345678901234567890.md6skl",
            "generated/skeleton/md6/1234567890123456789012345678901234567890.bmd6skl"}
    };
    for (size_t i = 0; i < sizeof(skeletons) / sizeof(*skeletons); i++) {
        const char *family = NULL; char *name = NULL;
        path = sh_package_cooked_path("skeleton", skeletons[i][0]);
        assert(path && !strcmp(path, skeletons[i][1]));
        assert(sh_package_cooked_identity(path, &family, &name) == 1 && !strcmp(family, "skeleton"));
        char *roundtrip = sh_package_cooked_path(family, name);
        assert(roundtrip && !strcmp(roundtrip, path));
        free(roundtrip); free(name); free(path);
    }
    assert(!sh_package_cooked_path("skeleton", "_default.md6skl"));
    assert(!sh_package_cooked_path("skeleton", "../md6/body.md6skl"));
    assert(!sh_package_cooked_path("skeleton", "md6/.md6skl"));
    static const char *images[][2] = {
        {"textures/rock.tga", "generated/image/textures/rock.bimage"},
        {"textures/rock", "generated/image/textures/rock.bimage"},
        {"TEXTURES\\Rock.PNG", "generated/image/textures/rock.bimage"},
        {"textures.v2/rock", "generated/image/textures.v2/rock.bimage"},
        {"textures/rock.tga$borderclamp$alpha", "generated/image/textures/rock.tga$borderclamp$alpha.bimage"},
        {"textures/rock$bc7", "generated/image/textures/rock$bc7.bimage"},
        {"textures/red-rock (copy).tga", "generated/image/textures/red_rock__copy_.bimage"},
        {"env/sky.bimage", "env/sky.bimage"},
        {"generated/swf/page.bimage", "generated/swf/page.bimage"},
        {"textures/direct-image.bimage", "textures/direct-image.bimage"}
    };
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        path = sh_package_cooked_path("image", images[i][0]);
        assert(path && !strcmp(path, images[i][1])); free(path);
        const char *family = NULL; char *name = NULL;
        assert(sh_package_cooked_identity(images[i][1], &family, &name) == 1);
        assert(!strcmp(family, "image") && !strcmp(name, images[i][1]));
        path = sh_package_cooked_path(family, name);
        assert(path && !strcmp(path, images[i][1])); free(path); free(name);
    }
    /* Navigation keeps its manager kind; a module's collision is its own path
     * (the installed catalog's named aas and cm rows). Grid Room outputs use
     * the same forms under their private module names. */
    static const char *navigation[][2] = {
        {"maps/modules/ind_dlc/room_4x/room_4x.aas_monster48", "generated/maps/modules/ind_dlc/room_4x/room_4x.baas_monster48"},
        {"MAPS\\Modules\\smpgrid\\v1\\m\\1536_1024_512\\1536_1024_512.aas_monster128",
            "generated/maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512.baas_monster128"},
        {"maps/modules/x/x.aas_botplayer", "generated/maps/modules/x/x.baas_botplayer"}
    };
    for (size_t i = 0; i < sizeof(navigation) / sizeof(*navigation); i++) {
        const char *family = NULL; char *name = NULL;
        path = sh_package_cooked_path("aas", navigation[i][0]);
        assert(path && !strcmp(path, navigation[i][1])); free(path);
        assert(sh_package_cooked_identity(navigation[i][1], &family, &name) == 1 && !strcmp(family, "aas"));
        path = sh_package_cooked_path(family, name);
        assert(path && !strcmp(path, navigation[i][1])); free(path); free(name);
    }
    /* The catalog's own long-name split: one name cooks to it, but the path a
     * split produces could also be a 30-character directory, so it names none. */
    path = sh_package_cooked_path("aas", "maps/modules/classic/offest_room/classic_bigger_blank_room_offest.aas_monster128");
    assert(path && !strcmp(path, "generated/maps/modules/classic/offest_room/classic_bigger_blank_room_offe/st.baas_monster128"));
    {
        const char *family = NULL; char *name = NULL;
        assert(sh_package_cooked_identity(path, &family, &name) == 0 && !name);
        free(path);
        /* A long basename the native contract would have split is no native output. */
        assert(sh_package_cooked_identity("generated/maps/x/classic_bigger_blank_room_offest.baas_monster128", &family, &name) == 0 && !name);
        /* A 32-character directory is no split. */
        assert(sh_package_cooked_identity("generated/maps/a123456789012345678901234567890b/x.baas_monster48", &family, &name) == 1);
        assert(!strcmp(name, "maps/a123456789012345678901234567890b/x.aas_monster48")); free(name);
    }
    assert(!sh_package_cooked_path("aas", "maps/modules/x/x.aas_"));
    assert(!sh_package_cooked_path("aas", "maps/modules/x/x.aas_monster48.bak"));
    assert(!sh_package_cooked_path("aas", "maps/modules/x/x"));
    assert(!sh_package_cooked_path("aas", "maps/modules/x/.aas_monster48"));
    {
        const char *family = NULL; char *name = NULL;
        assert(sh_package_cooked_identity("generated/maps/x.baas_", &family, &name) == 0);
        assert(sh_package_cooked_identity("maps/x.baas_monster48", &family, &name) == 0);
    }
    static const char *collision[] = {"maps/modules/ind_dlc/room_4x/room_4x_floor_collision.bcm",
        "smpgrid/v1/m/1536_1024_512/maps/modules/ind_dlc/room_4x/room_4x_floor_collision.bcm"};
    for (size_t i = 0; i < sizeof(collision) / sizeof(*collision); i++) {
        const char *family = NULL; char *name = NULL;
        path = sh_package_cooked_path("cm", collision[i]);
        assert(path && !strcmp(path, collision[i])); free(path);
        assert(sh_package_cooked_identity(collision[i], &family, &name) == 1 && !strcmp(family, "cm") &&
            !strcmp(name, collision[i])); free(name);
    }
    {
        /* A model's collision is sanitized under generated/cm/: no adapter. */
        const char *family = NULL; char *name = NULL;
        assert(sh_package_cooked_identity("generated/cm/models/props/rock.bcm", &family, &name) == 0 && !name);
        assert(!sh_package_cooked_path("cm", "models/props/rock.lwo"));
        assert(!sh_package_cooked_path("cm", "maps/modules/x/.bcm"));
    }
    assert(!sh_package_cooked_path("image", "_white"));
    assert(!sh_package_cooked_path("image", "textures/.tga"));
    assert(!sh_package_cooked_path("image", "../textures/rock.tga"));
    path = sh_package_cooked_path("discreteanimation", "MODELS\\Props\\Part.OBJ");
    assert(path && !strcmp(path, "generated/discreteanimation/models/props/part.dmodel")); free(path);
    path = sh_package_cooked_path("discreteanimation", "models/part.lwo$option");
    assert(path && !strcmp(path, "generated/discreteanimation/models/part.lwo$option.dmodel")); free(path);
    path = sh_package_cooked_path("discreteanimation", "custom/part.dmodel");
    assert(path && !strcmp(path, "custom/part.dmodel")); free(path);
    assert(!sh_package_cooked_path("discreteanimation", "_default"));
    {
        const char *family = NULL; char *name = NULL;
        assert(sh_package_cooked_identity("custom/part.dmodel", &family, &name));
        assert(!strcmp(family, "discreteanimation") && !strcmp(name, "custom/part.dmodel")); free(name);
    }
    assert(!sh_package_cooked_path("unregistered","models/props/rock"));
    assert(!sh_package_cooked_path("model","../models/rock.lwo"));
    assert(!sh_package_cooked_path(NULL,"models/rock.lwo"));
    assert(!sh_package_cooked_path("model",NULL));
    const char *bad[] = {"other/models/rock.bmodel", "cooked/model/rock.bmodel.bak",
        "cooked/model/.bmodel", "cooked/model/../rock.bmodel", "cooked/model/dir/.bmodel",
        "textures/.bimage", "../textures/icon.bimage", "textures/icon.bimage.bak",
        "md6/.bmd6skl", "generated/skeleton/.bmd6skl", "../md6/body.bmd6skl", "md6/body.bmd6skl.bak"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        const char *family=(const char *)1; char *name=(char *)1;
        assert(!sh_package_cooked_identity(bad[i],&family,&name) && !family && !name);
    }
}

/* A product producer's output takes the native kind of what it read only when
 * every input is catalogued under one family named by its own path. */
static void generated_identities(const sh_resource_catalog *catalog)
{
    char *probe_input[] = {"maps/modules/a/room/lightprobes/light_probe_1_compressed.bimage"};
    char *module_input[] = {"maps/modules/a/room.decl"};
    char *nav_input[] = {"generated/maps/modules/a/room/room.baas_monster48"};
    char *mixed_input[] = {"maps/modules/a/room/lightprobes/light_probe_1_compressed.bimage", "maps/modules/a/room.decl"};
    char *absent_input[] = {"maps/modules/a/room/absent.bimage"};
    sh_compiled_resource files[6] = {0};
    sh_package_compilation scope = {0};
    const sh_package_compilation *scopes[] = {&scope};
    sh_package_link_inputs inputs = {0};
    char error[512];
    files[0].engine_path = "maps/modules/smpgrid/v1/m/1_2_3/lightprobes/light_probe_1_compressed.bimage";
    files[0].generated_inputs = probe_input; files[0].generated_input_count = 1;
    files[1].engine_path = "maps/modules/smpgrid/v1/m/1_2_3.decl";
    files[1].generated_inputs = module_input; files[1].generated_input_count = 1;
    files[2].engine_path = "generated/maps/modules/smpgrid/v1/m/1_2_3/1_2_3.baas_monster48";
    files[2].generated_inputs = nav_input; files[2].generated_input_count = 1;
    files[3].engine_path = "maps/modules/smpgrid/v1/m/1_2_3/mixed.bimage";
    files[3].generated_inputs = mixed_input; files[3].generated_input_count = 2;
    files[4].engine_path = "maps/modules/smpgrid/v1/m/1_2_3/absent.bimage";
    files[4].generated_inputs = absent_input; files[4].generated_input_count = 1;
    files[5].engine_path = "maps/modules/smpgrid/v1/m/1_2_3/authored.bimage";
    files[5].generated_inputs = probe_input; files[5].generated_input_count = 1;
    for (size_t i = 0; i < 5; i++) files[i].generated = 1;
    assert(!strcmp(sh_package_generated_family(files, catalog), "file"));
    assert(!strcmp(sh_package_generated_family(files + 1, catalog), "snapmodule"));
    assert(!sh_package_generated_family(files + 2, catalog)); /* the input's name is not its path */
    assert(!sh_package_generated_family(files + 3, catalog)); /* two families */
    assert(!sh_package_generated_family(files + 4, catalog)); /* no catalogued input */
    assert(!sh_package_generated_family(files + 5, catalog)); /* not a producer's output */
    assert(!sh_package_generated_family(files, NULL) && !sh_package_generated_family(NULL, catalog));
    scope.resources = files; scope.resource_count = 6;
    assert(sh_package_link_inputs_open(scopes, 1, catalog, &inputs, error, sizeof(error)) && inputs.count == 6);
    assert(sh_package_link_inputs_find(&inputs, 0, "file", files[0].engine_path) != SIZE_MAX);
    assert(sh_package_link_inputs_find(&inputs, 0, "image", files[0].engine_path) == SIZE_MAX);
    assert(sh_package_link_inputs_find(&inputs, 0, "snapmodule", files[1].engine_path) != SIZE_MAX);
    assert(sh_package_link_inputs_find(&inputs, 0, "aas", "maps/modules/smpgrid/v1/m/1_2_3/1_2_3.aas_monster48") != SIZE_MAX);
    assert(sh_package_link_inputs_find(&inputs, 0, "image", files[3].engine_path) != SIZE_MAX);
    assert(sh_package_link_inputs_find(&inputs, 0, "image", files[4].engine_path) != SIZE_MAX);
    assert(sh_package_link_inputs_find(&inputs, 0, "image", files[5].engine_path) != SIZE_MAX);
    /* A module declaration with no generated family has no identity rule. */
    files[1].generated = 0;
    assert(!sh_package_link_inputs_open(scopes, 1, catalog, &inputs, error, sizeof(error)) &&
        strstr(error, "native resource identity adapter") && strstr(error, files[1].engine_path));
    sh_package_link_inputs_free(&inputs);
}

static int image_catalog(const char *base)
{
    char error[1024]; size_t checked = 0, mismatches = 0, catalog_only = 0;
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error));
    if (!catalog) { fprintf(stderr, "%s\n", error); return 1; }
    for (size_t i = 0; i < sh_resource_catalog_count(catalog); i++) {
        const sh_resource_catalog_entry *entry = sh_resource_catalog_at(catalog, i);
        if (strcmp(entry->type, "image") || !*entry->path) continue;
        size_t length = strlen(entry->path);
        /* Explicit catalog paths need not follow the generated-file contract.
         * The linker selects them before using its missing-row fallback. */
        if (strncmp(entry->path, "generated/image/", 16) &&
            (length < 7 || strcmp(entry->path + length - 7, ".bimage"))) {
            catalog_only++; continue;
        }
        char *path = sh_package_cooked_path("image", entry->name);
        checked++;
        if (!path || strcmp(path, entry->path)) {
            if (mismatches++ < 8) fprintf(stderr, "%s: expected %s, resolved %s\n",
                entry->name, entry->path, path ? path : "(no physical path)");
        }
        free(path);
    }
    sh_resource_catalog_close(catalog);
    printf("image catalog paths: %zu checked, %zu mismatches, %zu explicit catalog-only paths\n",
        checked, mismatches, catalog_only);
    return !checked || mismatches ? 1 : 0;
}

/* A module's name-derived members and the same members of another module. */
static void module_members(void)
{
    static const char *const members[][3] = {
        {"generated/decls/snapmoduleinfo/ind_dlc/ind_totally_blank_room_4x.decl", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "card"},
        {"maps/modules/palettes/mega_blessed/ind_dlc/ind_totally_blank_room_4x.bmodel", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "render"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/_combo/world.bcm", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "collision"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_floor_collision.bcm", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "collision"},
        {"generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.baas_monster48", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "navigation"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.sbsp", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "sound"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.ambientsh", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "ambient"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/_combo/_world.tome", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "occlusion"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/lightprobes/light_probe_1_compressed.bimage", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "probe"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_single_surface_walls.bmodel", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "blueprint"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_walls_floor.bmodel", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "blueprint"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_wall_12.bmodel", "maps/modules/ind_dlc/ind_totally_blank_room_4x", "blueprint"},
        {"maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512.sbsp", "maps/modules/smpgrid/v1/m/1536_1024_512", "sound"},
        {"maps/modules/classic/classic_90/classic_90_aas_monster128.bmodel", "maps/modules/classic/classic_90", "named"}};
    static const char *const others[] = {
        "maps/modules/ind_dlc/ind_totally_blank_room_4x.decl", "maps/modules/classic/classic_90.decl", "maps/modules/ind_dlc/ind_totally_blank_room_4x/other.sbsp",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/volume_flight_2.bcm", "maps/modules/x.sbsp",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x", "maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4xb.bmodel",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x/lightprobes/sub/x.bimage", "maps/modules/lightprobes/x.bimage",
        "maps/modules/ind_dlc/../x/x.sbsp", "maps/modules/palettes/mega_blessed.bmodel", "maps/modules/palettes//x.bmodel",
        "generated/decls/snapmoduleinfo/.decl", "generated/maps/modules/ind_dlc/room/other.baas_monster48",
        "maps/game/room/room.sbsp", "models/maps/modules/room/room.sbsp", "maps/modules/_combo/world.bcm", ""};
    char module[256]; const char *role;
    for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++) {
        role = NULL; module[0] = 0;
        assert(sh_package_module_member(members[i][0], module, sizeof(module), &role) == 1);
        assert(!strcmp(module, members[i][1]) && role && !strcmp(role, members[i][2]));
        assert(!sh_package_module_member(members[i][0], module, 10, &role));
    }
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++)
        assert(!sh_package_module_member(others[i], module, sizeof(module), &role));
    assert(!sh_package_module_member(NULL, module, sizeof(module), &role));
    static const char *const renames[][4] = {
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_floor_collision.bcm",
         "maps/modules/ind_dlc/ind_totally_blank_room_4x", "maps/modules/smpgrid/v1/m/1536_1024_512",
         "maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512_floor_collision.bcm"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/_combo/world.bcm", "maps/modules/ind_dlc/ind_totally_blank_room_4x",
         "maps/modules/smp/0123abcd/ind_dlc/ind_totally_blank_room_4x",
         "maps/modules/smp/0123abcd/ind_dlc/ind_totally_blank_room_4x/_combo/world.bcm"},
        {"generated/maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x.baas_monster96",
         "maps/modules/ind_dlc/ind_totally_blank_room_4x", "maps/modules/smpgrid/v1/m/1536_1024_512",
         "generated/maps/modules/smpgrid/v1/m/1536_1024_512/1536_1024_512.baas_monster96"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/lightprobes/light_probe_1_compressed.bimage",
         "maps/modules/ind_dlc/ind_totally_blank_room_4x", "maps/modules/smpgrid/v1/m/1536_1024_512",
         "maps/modules/smpgrid/v1/m/1536_1024_512/lightprobes/light_probe_1_compressed.bimage"},
        {"maps/modules/palettes/mega_blessed/ind_dlc/ind_totally_blank_room_4x.bmodel",
         "maps/modules/ind_dlc/ind_totally_blank_room_4x", "maps/modules/smp/0123abcd/ind_dlc/ind_totally_blank_room_4x",
         "maps/modules/palettes/mega_blessed/smp/0123abcd/ind_dlc/ind_totally_blank_room_4x.bmodel"},
        {"generated/decls/snapmoduleinfo/ind_dlc/ind_totally_blank_room_4x.decl", "maps/modules/ind_dlc/ind_totally_blank_room_4x",
         "maps/modules/smp/0123abcd/ind_dlc/ind_totally_blank_room_4x",
         "generated/decls/snapmoduleinfo/smp/0123abcd/ind_dlc/ind_totally_blank_room_4x.decl"},
        {"maps/modules/ind_dlc/ind_totally_blank_room_4x/ind_totally_blank_room_4x_wall_3.bmodel",
         "maps/modules/ind_dlc/ind_totally_blank_room_4x", "maps/modules/smpgrid/v1/c/512_768_384",
         "maps/modules/smpgrid/v1/c/512_768_384/512_768_384_wall_3.bmodel"},
        {"maps/modules/classic/classic_90/classic_90_aas_monster128.bmodel", "maps/modules/classic/classic_90",
         "maps/modules/smp/0123abcd/classic/classic_90", "maps/modules/smp/0123abcd/classic/classic_90/classic_90_aas_monster128.bmodel"}};
    for (size_t i = 0; i < sizeof(renames) / sizeof(renames[0]); i++) {
        char *out = sh_package_module_member_rename(renames[i][0], renames[i][1], renames[i][2]);
        assert(out && !strcmp(out, renames[i][3]));
        free(out);
    }
    assert(!sh_package_module_member_rename("maps/modules/ind_dlc/ind_totally_blank_room_4x/_combo/world.bcm",
        "maps/modules/ind_dlc/other", "maps/modules/smp/0123abcd/ind_dlc/other"));
    assert(!sh_package_module_member_rename("maps/modules/ind_dlc/ind_totally_blank_room_4x.decl",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x", "maps/modules/smp/0123abcd/ind_dlc/ind_totally_blank_room_4x"));
    assert(!sh_package_module_member_rename("maps/modules/ind_dlc/ind_totally_blank_room_4x/_combo/world.bcm",
        "maps/modules/ind_dlc/ind_totally_blank_room_4x", "maps/modules/"));
}

int main(int argc, char **argv)
{
    char temporary[MAX_PATH], base[4096], error[1024];
    sh_compiled_resource files[9] = {0}, duplicate[2];
    sh_package_compilation scope = {0}, malformed = {0};
    const sh_package_compilation *scopes[] = {&scope, &scope};
    sh_package_link_inputs inputs = {0};
    const char *paths[] = {"generated/decls/entitydef/props/rock.decl", "shader/vertex.bin",
        "shader/fragment.bin", "cooked/model/models/custom.bmodel", "generated/image/rock$a00.bimage",
        "generated/image/rock$b00.bimage", "generated/basemodel/md6/guard/body.bmd6model",
        "cooked/anim/md6/guard/walk.bmd6anim", "md6/guard/body.md6"};
    if (argc == 2) return image_catalog(argv[1]);
    cooked_paths();
    assert(GetTempPathA(sizeof(temporary),temporary));
    snprintf(root,sizeof(root),"%ssmp-link-inputs-%lu-%llu",temporary,GetCurrentProcessId(),GetTickCount64());
    assert(CreateDirectoryA(root,NULL));
    catalog_file("base/snap_gameresources.pindex"); catalog_file("base/gameresources.pindex");
    create("base/snap_gameresources.resources",""); create("base/snap_gameresources.patch","");
    create("base/gameresources.resources",""); create("base/gameresources.patch","");
    snprintf(base,sizeof(base),"%s/base",root);
    sh_resource_catalog *catalog=sh_resource_catalog_open(base,error,sizeof(error));
    assert(catalog && sh_resource_catalog_count(catalog)==22);
    generated_identities(catalog);
    scope.resources=files; scope.resource_count=9;
    for(size_t i=0;i<9;i++) files[i].engine_path=paths[i];
    files[0].type="entitydef"; files[0].name="props/rock";
    assert(sh_package_link_inputs_open(scopes,2,catalog,&inputs,error,sizeof(error)) && inputs.count==16);
    for(size_t s=0;s<2;s++) {
        size_t shader=sh_package_link_inputs_find(&inputs,s,"renderprog","shaders/shared");
        size_t alternate=sh_package_link_inputs_find(&inputs,s,"renderprog","shaders/alternate");
        size_t image=sh_package_link_inputs_find(&inputs,s,"image","images/rock");
        assert(shader!=SIZE_MAX && alternate!=SIZE_MAX && image!=SIZE_MAX);
        assert(inputs.units[shader].file_count==2 && inputs.units[alternate].file_count==1);
        assert(!strcmp(inputs.units[shader].files[0]->engine_path,"shader/fragment.bin"));
        assert(!strcmp(inputs.units[shader].files[1]->engine_path,"shader/vertex.bin"));
        assert(inputs.units[image].file_count==2);
        assert(sh_package_link_inputs_find(&inputs,s,"model","models/catalog-special.obj")!=SIZE_MAX);
        assert(sh_package_link_inputs_find(&inputs,s,"model","models/custom.lwo")==SIZE_MAX);
        assert(sh_package_link_inputs_find(&inputs,s,"basemodel","md6/guard/body.md6mesh")!=SIZE_MAX);
        assert(sh_package_link_inputs_find(&inputs,s,"anim","md6/guard/walk.md6anim")!=SIZE_MAX);
        assert(sh_package_link_inputs_find(&inputs,s,"entitydef","props/rock")!=SIZE_MAX);
        /* An unnamed index row is supplied under its resource name, as a
         * converted legacy manifest imports zero-byte collision placeholders. */
        size_t placeholder=sh_package_link_inputs_find(&inputs,s,"cm","md6/guard/body.md6");
        assert(placeholder!=SIZE_MAX && inputs.units[placeholder].file_count==1);
        assert(inputs.units[placeholder].files[0]==files+8);
    }
    assert(sh_package_link_inputs_find(&inputs,2,"entitydef","props/rock")==SIZE_MAX);
    assert(sh_package_link_inputs_find(&inputs,0,"image","missing")==SIZE_MAX);
    /* Custom images do not need an installed catalog row. Their explicit
     * native path remains accounted for in every authored scope. */
    files[7].engine_path="generated/image/textures/custom/icon.bimage";
    assert(sh_package_link_inputs_open(scopes,2,catalog,&inputs,error,sizeof(error)));
    for (size_t s=0;s<2;s++) {
        size_t image=sh_package_link_inputs_find(&inputs,s,"image",files[7].engine_path);
        assert(image!=SIZE_MAX && inputs.units[image].file_count==1);
        assert(inputs.units[image].files[0]==files+7);
    }
    /* A native file is never quietly classified as a leaf or dropped. */
    files[7].engine_path="unknown/physical-resource.bin";
    assert(!sh_package_link_inputs_open(scopes,2,catalog,&inputs,error,sizeof(error)));
    assert(strstr(error,files[7].engine_path) && !inputs.count && !inputs.units && !inputs.scopes);
    /* A named row's resource name is not a provider path. */
    files[7].engine_path="images/rock";
    assert(!sh_package_link_inputs_open(scopes,2,catalog,&inputs,error,sizeof(error)));
    assert(strstr(error,"native resource identity adapter") && strstr(error,files[7].engine_path));
    files[7].engine_path=paths[7];
    files[0].name=NULL;
    assert(!sh_package_link_inputs_open(scopes,2,catalog,&inputs,error,sizeof(error)) && strstr(error,"declaration identity"));
    files[0].name="props/rock";
    duplicate[0]=duplicate[1]=files[0]; malformed.resources=duplicate; malformed.resource_count=2;
    scopes[0]=&malformed;
    assert(!sh_package_link_inputs_open(scopes,1,catalog,&inputs,error,sizeof(error)) && strstr(error,"repeats a provider path"));
    scopes[0]=&scope;
    assert(!sh_package_link_inputs_open(NULL,1,catalog,&inputs,error,sizeof(error)));
    assert(!sh_package_link_inputs_open(scopes,SIZE_MAX,catalog,&inputs,error,sizeof(error)));
    assert(sh_package_link_inputs_open(NULL,0,NULL,&inputs,error,sizeof(error)) && !inputs.count);
    sh_package_link_inputs_free(&inputs); sh_package_link_inputs_free(&inputs);
    module_members();
    sh_resource_catalog_close(catalog); cleanup(); assert(!failures);
    puts("package native source input tests passed"); return 0;
}
