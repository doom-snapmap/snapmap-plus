// prefab_host_test -- the WebView host's Prefabs tab decisions (prefab_host.h):
// Create's precheck tells an empty selection from a hovered entity that is not
// selected, a prefab's kind is its file's top-level ~type with or without the
// snapmapPlus block, and the live usage is posted again only when it changed.
#include <stdio.h>
#include <string.h>
#include <string>

#include "../src/ui/webview/prefab_host.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static std::string type_of(const std::string &text) { return sh_prefab_host::top_type_of_tail(text.data(), text.size()); }

int main()
{
    using namespace sh_prefab_host;

    const int sel[] = { 61, 68, 69 };
    CHECK(create_precheck(sel, 0, 61, true) == CREATE_EMPTY);
    CHECK(create_precheck(sel, 0, -1, false) == CREATE_EMPTY);
    CHECK(create_precheck(sel, 3, -1, true) == CREATE_NO_HOVER);
    CHECK(create_precheck(sel, 3, 70, true) == CREATE_HOVER_UNSELECTED);   /* P40: the hovered entity is not selected */
    CHECK(create_precheck(sel, 3, 68, true) == CREATE_GO);
    CHECK(create_precheck(sel, 3, -1, false) == CREATE_GO);                /* no hover read: the copy check decides */

    /* An object prefab as the engine writes it: nested "~type"s first, the top-level one second to last. */
    const std::string object =
        "{\"cameraYaw\":0,\"entities\":[{\"entityDef\":{\"~type\":\"idSnapEntity\"}}],\"grabDistance\":128,"
        "\"~type\":\"idSnapEntityPrefab\",\"~version\":0}";
    CHECK(type_of(object) == "idSnapEntityPrefab");
    CHECK(!strcmp(kind_of_type(type_of(object)), "objects"));

    /* A module prefab without a block (P40's mp_basin_module.json) is a module prefab. */
    const std::string module_bare = "{\"instances\":[{\"moduleName\":\"a\"}],\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111}";
    CHECK(type_of(module_bare) == "idSnapMap");
    CHECK(!strcmp(kind_of_type(type_of(module_bare)), "modules"));

    /* With the block after the engine's members: the block is left out, whatever it says. */
    const std::string module_block = module_bare.substr(0, module_bare.size() - 1) +
        ",\n\"snapmapPlus\":{\"format\":1,\"description\":\"\",\"tags\":[],\"mods\":[],\"summary\":{\"kind\":\"modules\",\"~type\":\"x\"}}}";
    CHECK(type_of(module_block) == "idSnapMap");
    const std::string object_block = object.substr(0, object.size() - 1) +
        ",\n\"snapmapPlus\":{\"format\":1,\"description\":\"\",\"tags\":[],\"mods\":[],\"summary\":{\"kind\":\"modules\"}}}";
    CHECK(!strcmp(kind_of_type(type_of(object_block)), "objects"));         /* the file's type wins over a block's kind */

    /* Only a tail was read: the last "~type" before the block is still the top-level one. */
    std::string tail = module_block.substr(module_block.find("\"version\""));
    CHECK(type_of(tail) == "idSnapMap");
    CHECK(type_of("") == "" && type_of("{\"a\":1}") == "");
    CHECK(!strcmp(kind_of_type("idSnapSomething"), ""));

    usage_gate gate;
    CHECK(gate.changed("{\"usage\":[1]}", ",\"held\":null"));                 /* the first value is always posted */
    CHECK(!gate.changed("{\"usage\":[1]}", ",\"held\":null"));                /* nothing changed: nothing posted */
    CHECK(gate.changed("{\"usage\":[2]}", ",\"held\":null"));                 /* the map changed */
    CHECK(gate.changed("{\"usage\":[2]}", ",\"held\":{\"name\":\"Stairs\"}"));  /* picked up */
    CHECK(!gate.changed("{\"usage\":[2]}", ",\"held\":{\"name\":\"Stairs\"}"));
    CHECK(gate.changed("{\"usage\":[2]}", ",\"held\":null"));                 /* Space or Esc */

    if (g_failed) { fprintf(stderr, "prefab_host_test: %d of %d checks failed\n", g_failed, g_checks); return 1; }
    printf("prefab_host_test: %d checks passed\n", g_checks);
    return 0;
}
