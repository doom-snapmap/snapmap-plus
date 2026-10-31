// rawmap_picker_test -- the file types the File menu's rawmap dialogs offer:
// Load Rawmap lists .json and .zip together, Save Rawmap As puts .zip first
// (and proposes rawmap.zip) when the open map uses a mod, .json first when it
// does not, and the mod count is read from the backend's status object.
#include <stdio.h>
#include <wchar.h>

#include "../src/ui/webview/rawmap_picker.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static bool offers(const sh_rawmap_picker::choice &c, const wchar_t *pattern)
{
    for (unsigned i = 0; i < c.count; ++i)
        if (!wcscmp(c.types[i].pattern, pattern)) return true;
    return false;
}

int main()
{
    using namespace sh_rawmap_picker;

    choice load = for_dialog(false, 0);
    CHECK(load.count == 4 && !wcscmp(load.types[0].pattern, L"*.json;*.zip"));
    CHECK(offers(load, L"*.zip") && offers(load, L"*.json") && offers(load, L"*.*"));
    CHECK(load.file_name == nullptr);
    /* The open map's mods do not change what can be loaded. */
    choice load_mods = for_dialog(false, 3);
    CHECK(load_mods.count == load.count && !wcscmp(load_mods.types[0].pattern, load.types[0].pattern));

    choice plain = for_dialog(true, 0);
    CHECK(!wcscmp(plain.types[0].pattern, L"*.json") && offers(plain, L"*.zip"));
    CHECK(!wcscmp(plain.extension, L"json") && !wcscmp(plain.file_name, L"rawmap.json"));

    choice modded = for_dialog(true, 2);
    CHECK(!wcscmp(modded.types[0].pattern, L"*.zip") && offers(modded, L"*.json"));
    CHECK(!wcscmp(modded.extension, L"zip") && !wcscmp(modded.file_name, L"rawmap.zip"));
    CHECK(wcsstr(modded.types[0].label, L"with its mods") != nullptr);
    CHECK(wcsstr(modded.types[1].label, L"without its mods") != nullptr);

    /* A prefab's export is always a ZIP; import also takes the .json of an extracted one. */
    choice pexport = for_prefab_dialog(true);
    CHECK(!wcscmp(pexport.types[0].pattern, L"*.zip") && !wcscmp(pexport.types[0].label, L"Prefab export (*.zip)"));
    CHECK(!wcscmp(pexport.extension, L"zip") && pexport.file_name == nullptr);   /* the caller proposes "<name>.zip" */
    choice pimport = for_prefab_dialog(false);
    CHECK(!wcscmp(pimport.types[0].pattern, L"*.zip;*.json") && !wcscmp(pimport.types[0].label, L"Prefab exports (*.zip, *.json)"));
    CHECK(offers(pimport, L"*.*") && pimport.file_name == nullptr);

    CHECK(map_mods_from_status("{\"load\":\"a\",\"mapMods\":2}") == 2);
    CHECK(map_mods_from_status("{\"load\":\"a\",\"mapMods\":0}") == 0);
    CHECK(map_mods_from_status("{\"load\":\"a\"}") == 0);     /* an older backend */
    CHECK(map_mods_from_status(nullptr) == 0);

    printf("rawmap_picker_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
