#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model_reference.h"

int main(void)
{
    static const struct { const char *request, *family, *name, *bound; } cases[] = {
        {"/FX\\Smoke.PRT", "particle", "fx/smoke", "scope/fx/smoke.prt"},
        {"Props/Cape.Cloth", "cloth", "props/cape", "scope/props/cape.cloth"},
        {"actors/robot.MD6", "md6def", "actors/robot.md6", "scope/actors/robot.md6"},
        {"actors/robot.decl", "md6def", "actors/robot.decl", "scope/actors/robot.decl"},
        {"props/door.break", "breakable", "props/door", "scope/props/door.break"},
        {"props/door.breakabledecl", "breakable", "props/door.decl", "scope/props/door.breakabledecl"},
        {"effects/glow.hotspot", "hotspot", "effects/glow", "scope/effects/glow.hotspot"},
        {"models/rock.lwo", "model", "models/rock.lwo", "scope/models/rock.lwo"},
        {"folder.md6/rock", "model", "folder.md6/rock", "scope/folder.md6/rock"},
        {"_world/rock.lwo", "model", "_world/rock.lwo", "scope/_world/rock.lwo"}
    };
    sh_model_reference ref = {0};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char replacement[128], *bound;
        assert(sh_model_reference_read(cases[i].request, &ref) == 1);
        assert(!strcmp(ref.family, cases[i].family) && !strcmp(ref.name, cases[i].name));
        snprintf(replacement, sizeof(replacement), "scope/%s", ref.name);
        bound = sh_model_reference_bind(&ref, replacement);
        assert(bound && !strcmp(bound, cases[i].bound)); free(bound);
    }
    assert(sh_model_reference_read("_WORLD", &ref) == 0 && !ref.name);
    assert(sh_model_reference_read("_decal", &ref) == 0);
    assert(sh_model_reference_read(NULL, &ref) == 0);
    assert(sh_model_reference_read("", &ref) == 0);
    assert(sh_model_reference_read("test.automap", &ref) == -1 && !ref.name);
    assert(sh_model_reference_read("test.watersurf", &ref) == -1);
    assert(sh_model_reference_read("\xe9/model", &ref) == -1);
    assert(sh_model_reference_read("actors/robot.md6", &ref) == 1);
    assert(!sh_model_reference_bind(&ref, "scope/lost-extension"));
    sh_model_reference_clear(&ref); puts("model reference tests passed"); return 0;
}
