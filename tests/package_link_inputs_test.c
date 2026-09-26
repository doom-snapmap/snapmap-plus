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
        {"cm", "md6/guard/body.md6", ""}
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
        "textures/.bimage", "../textures/icon.bimage", "textures/icon.bimage.bak"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        const char *family=(const char *)1; char *name=(char *)1;
        assert(!sh_package_cooked_identity(bad[i],&family,&name) && !family && !name);
    }
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
    assert(catalog && sh_resource_catalog_count(catalog)==16);
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
    sh_resource_catalog_close(catalog); cleanup(); assert(!failures);
    puts("package native source input tests passed"); return 0;
}
