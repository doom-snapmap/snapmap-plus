/* sh_decl_text_own_state: a placed entity's own decl text from its prepared
 * text and its inherit's. The Entities tab shows and saves this text, so an
 * inherited member must not come back as the entity's own. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/decl_text.h"

static int own(const char *prepared, const char *inherited, char *out, size_t cap)
{
    size_t n = 0;
    int ok = sh_decl_text_own_state((const unsigned char *)prepared, strlen(prepared),
                                    (const unsigned char *)inherited,
                                    inherited ? strlen(inherited) : 0, out, cap, &n);
    if (ok) assert(n == strlen(out));
    else assert(out[0] == '\0');
    return ok;
}

static void expect(const char *prepared, const char *inherited, const char *want)
{
    char out[4096];
    int ok = own(prepared, inherited, out, sizeof out);
    if (!ok || strcmp(out, want) != 0) {
        printf("FAIL\n--- got (%d)\n%s\n--- want\n%s\n", ok, out, want);
        assert(0);
    }
}

/* A mod prop: its entityDef carries the whole edit block, the map only the position. */
static const char *ROCK_PARENT =
    "edit = {\n"
    "\trenderModelInfo = {\n"
    "\t\tmodel = \"mods/local.rocks/models/mapobjects/hell_terrain/boulder_kit_a.lwo\";\n"
    "\t}\n"
    "\tclipModelInfo = {\n"
    "\t\ttype = \"CLIPMODEL_BOX\";\n"
    "\t\tsize = {\n\t\t\tx = 256;\n\t\t\ty = 128;\n\t\t\tz = 128;\n\t\t}\n"
    "\t}\n"
    "\tisSolid = true;\n"
    "\tdynamicObstacle = true;\n"
    "}";
static const char *ROCK_ENTITY =
    "edit = {\n"
    "\trenderModelInfo = {\n"
    "\t\tmodel = \"mods/local.rocks/models/mapobjects/hell_terrain/boulder_kit_a.lwo\";\n"
    "\t}\n"
    "\tclipModelInfo = {\n"
    "\t\ttype = \"CLIPMODEL_BOX\";\n"
    "\t\tsize = {\n\t\t\tx = 256;\n\t\t\ty = 128;\n\t\t\tz = 128;\n\t\t}\n"
    "\t}\n"
    "\tisSolid = true;\n"
    "\tdynamicObstacle = true;\n"
    "\tspawnPosition = {\n\t\tx = 1100;\n\t\ty = 1500;\n\t\tz = 0.25;\n\t}\n"
    "}";

int main(void)
{
    char small[16];

    /* Only the members the entity sets itself are its own. */
    expect(ROCK_ENTITY, ROCK_PARENT,
           "edit = {\n\tspawnPosition = {\n\t\tx = 1100;\n\t\ty = 1500;\n\t\tz = 0.25;\n\t}\n}");

    /* A changed leaf keeps its block path, and only that leaf of it. */
    expect("edit = {\n\tclipModelInfo = {\n\t\ttype = \"CLIPMODEL_BOX\";\n\t\tsize = {\n\t\t\tx = 64;\n\t\t\ty = 128;\n\t\t}\n\t}\n\tisSolid = false;\n}",
           ROCK_PARENT,
           "edit = {\n\tclipModelInfo = {\n\t\tsize = {\n\t\t\tx = 64;\n\t\t}\n\t}\n\tisSolid = false;\n}");

    /* Nothing of its own: the first statement is still written, empty. */
    expect(ROCK_PARENT, ROCK_PARENT, "edit = {\n}");

    /* A stock player start: initial comes from the inherit. */
    expect("edit = {\n\tinitial = true;\n\tspawnPosition = {\n\t\tx = -1397.2294921875;\n\t}\n}",
           "edit = {\n\tinitial = true;\n}",
           "edit = {\n\tspawnPosition = {\n\t\tx = -1397.2294921875;\n\t}\n}");

    /* Equality is by spelling; a different value or a key the inherit lacks is kept. */
    expect("edit = {\n\tisSolid = 1;\n\tnewKey = \"a;b{c}\";\n}",
           "edit = {\n\tisSolid = true;\n}",
           "edit = {\n\tisSolid = 1;\n\tnewKey = \"a;b{c}\";\n}");

    /* A leaf over an inherited block, and a block over an inherited leaf, are kept. */
    expect("edit = {\n\tscale = 2;\n\tcolor = {\n\t\tr = 1;\n\t}\n}",
           "edit = {\n\tscale = {\n\t\tx = 2;\n\t}\n\tcolor = 1;\n}",
           "edit = {\n\tscale = 2;\n\tcolor = {\n\t\tr = 1;\n\t}\n}");

    /* Indexed items compare by their own key. */
    expect("edit = {\n\tattachments = {\n\t\tnum = 2;\n\t\titem[0] = {\n\t\t\ttagName = \"a\";\n\t\t}\n\t\titem[1] = {\n\t\t\ttagName = \"b\";\n\t\t}\n\t}\n}",
           "edit = {\n\tattachments = {\n\t\tnum = 1;\n\t\titem[0] = {\n\t\t\ttagName = \"a\";\n\t\t}\n\t}\n}",
           "edit = {\n\tattachments = {\n\t\tnum = 2;\n\t\titem[1] = {\n\t\t\ttagName = \"b\";\n\t\t}\n\t}\n}");

    /* No inherit text: everything is own, reformatted the engine's way. */
    expect("edit = {\n    a = 1 ;\n    b = ( 1, 1, 1, 1 );\n}", NULL,
           "edit = {\n\ta = 1;\n\tb = ( 1, 1, 1, 1 );\n}");

    /* Truncated or malformed text, and a full output buffer, are refused. */
    assert(!own("edit = {\n\ta = 1;\n", NULL, small, sizeof small));
    assert(!own("edit = {\n\ta = \"x;\n}", NULL, small, sizeof small));
    assert(!own(ROCK_ENTITY, NULL, small, sizeof small));
    assert(!own("edit = {\n}", "edit = {", small, sizeof small));

    printf("decl_text_own_state_test: OK\n");
    return 0;
}
