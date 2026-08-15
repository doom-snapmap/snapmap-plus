/* imgpreview_catalog_test.c -- Wwise union, bank mapping, wrapper collapse, and VMTR paging. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/imgpreview.c"

static int failures;
static const char *const *vmtr_names;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

void backend_log(const char *msg) { (void)msg; }
const char *sh_megapreview_name_at(int index)
{
    return vmtr_names ? vmtr_names[index] : NULL;
}
int sh_preview_publish(unsigned long generation, const unsigned char *rgba, unsigned w, unsigned h)
{ (void)generation; (void)rgba; (void)w; (void)h; return SH_PREVIEW_PUBLISHED; }

static void set_records(int count)
{
    free(g_rec);
    g_rec = (rec_t *)calloc((size_t)count, sizeof *g_rec);
    g_recCount = count;
    CHECK(count == 0 || g_rec != NULL);
}

static const char *bank_for(const char *name)
{
    for (int i = 0; i < g_sbCount; ++i)
        if (_stricmp(g_sb[i].name, name) == 0) return g_sb[i].bank;
    return NULL;
}

static int event_present(const char *name)
{
    for (int i = 0; i < g_evCount; ++i)
        if (_stricmp(g_ev[i], name) == 0) return 1;
    return 0;
}

int main(void)
{
    static const char xml[] =
        "<Root>"
        "<SoundBank Id=\"1\"><ShortName>doom_initial</ShortName><IncludedEvents>"
        "<Event Id=\"1\" Name=\"Play_Existing\"/>"
        "<Event Id=\"2\" Name=\"Play_Multi\"/>"
        "<Event Id=\"3\" Name=\"Play_OnlyBase\"/>"
        "<Event Id=\"4\" Name=\"Play_Foo\"/>"
        "</IncludedEvents></SoundBank>"
        "<SoundBank Id=\"2\"><ShortName>doom_monsters</ShortName><IncludedEvents>"
        "<Event Id=\"5\" Name=\"play_multi\"/>"
        "<Event Id=\"6\" Name=\"Play_Specific\"/>"
        "</IncludedEvents></SoundBank>"
        "</Root>";
    static const char *const atlas[] = {
        "materials/existing", "Materials/Existing", "materials/vt", "MATERIALS/VT", NULL
    };

    InitializeCriticalSection(&g_lock);
    g_loaded = 1;

    set_records(1);
    if (g_rec) {
        g_rec[0].name = "play_existing";
        g_rec[0].kind = SH_ASSET_SOUND;
    }
    size_t xml_len = sizeof xml - 1;
    unsigned char *manifest = (unsigned char *)malloc(xml_len + 1u);
    CHECK(manifest != NULL);
    if (manifest) {
        memcpy(manifest, xml, xml_len + 1u);
        imgpreview_parse_wwise_buffer(manifest, xml_len);
    }

    CHECK(g_evCount == 4);
    CHECK(!event_present("play_existing"));
    CHECK(event_present("play_multi"));
    CHECK(event_present("play_onlybase"));
    CHECK(event_present("play_foo"));
    CHECK(event_present("play_specific"));
    CHECK(g_sbCount == 5);
    CHECK(bank_for("play_existing") && _stricmp(bank_for("play_existing"), "doom_initial") == 0);
    CHECK(bank_for("play_multi") && _stricmp(bank_for("play_multi"), "doom_monsters") == 0);
    CHECK(bank_for("play_onlybase") && _stricmp(bank_for("play_onlybase"), "doom_initial") == 0);

    {
        char out[2048];
        int n = sh_imgpreview_list(SH_ASSET_SOUND, 0, out, sizeof out);
        CHECK(n == 5);
        CHECK(strstr(out, "play_existing\n") != NULL);
        CHECK(strstr(out, "Play_Specific\n") != NULL);
        CHECK(sh_imgpreview_has(SH_ASSET_SOUND, "PLAY_SPECIFIC") == 1);
        CHECK(sh_imgpreview_has(SH_ASSET_SOUND, "missing") == 0);

        n = sh_imgpreview_list(SH_ASSET_SNDBANK, 0, out, sizeof out);
        CHECK(n == 5);
        CHECK(strstr(out, "Play_Existing|doom_initial\n") != NULL);
        CHECK(strstr(out, "play_multi|doom_monsters\n") != NULL ||
              strstr(out, "Play_Multi|doom_monsters\n") != NULL);
    }

    set_records(4);
    if (g_rec) {
        g_rec[0].name = "effects/foo";
        g_rec[1].name = "play_bar";
        g_rec[2].name = "effects/bar";
        g_rec[3].name = "effects/unique";
        for (int i = 0; i < 4; ++i) g_rec[i].kind = SH_ASSET_SOUND;
        CHECK(imgpreview_hide_wrapped_sounds() == 2);
        CHECK(g_rec[0].hidden == 1);
        CHECK(g_rec[1].hidden == 0);
        CHECK(g_rec[2].hidden == 1);
        CHECK(g_rec[3].hidden == 0);
    }

    set_records(1);
    if (g_rec) {
        g_rec[0].name = "materials/existing";
        g_rec[0].kind = SH_ASSET_MATERIAL;
    }
    vmtr_names = atlas;
    imgpreview_load_vmtr();
    CHECK(g_vtCount == 1);
    CHECK(g_vt && _stricmp(g_vt[0], "materials/vt") == 0);
    {
        char out[64];
        int n = sh_imgpreview_list(SH_ASSET_MATERIAL, 0, out, sizeof out);
        CHECK(n == 2);
        CHECK(strcmp(out, "materials/existing\nmaterials/vt\n") == 0 ||
              strcmp(out, "materials/existing\nMATERIALS/VT\n") == 0);

        char one[20];
        n = sh_imgpreview_list(SH_ASSET_MATERIAL, 0, one, sizeof one);
        CHECK(n == 1);
        CHECK(strcmp(one, "materials/existing\n") == 0);
        n = sh_imgpreview_list(SH_ASSET_MATERIAL, 1, one, sizeof one);
        CHECK(n == 1);
        CHECK(_stricmp(one, "materials/vt\n") == 0);
        CHECK(sh_imgpreview_list(SH_ASSET_MATERIAL, 2, one, sizeof one) == 0);
        CHECK(sh_imgpreview_list(SH_ASSET_VTONLY, 0, out, sizeof out) == 1);
    }

    free(g_vt); g_vt = NULL; g_vtCount = 0;
    free(g_sb); g_sb = NULL; g_sbCount = 0;
    free(g_ev); g_ev = NULL; g_evCount = 0;
    free(g_wwise); g_wwise = NULL;
    free(g_rec); g_rec = NULL; g_recCount = 0;
    DeleteCriticalSection(&g_lock);

    if (failures) {
        fprintf(stderr, "%d catalog test(s) failed\n", failures);
        return 1;
    }
    puts("image catalog tests passed");
    return 0;
}
