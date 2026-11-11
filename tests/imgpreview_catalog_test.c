/* imgpreview_catalog_test.c -- Wwise union, bank mapping, wrapper collapse, and VMTR paging. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <malloc.h>
#include <stdint.h>

#include "../src/backend/imgpreview.c"

static int failures;
static const char *const *vmtr_names;
static int publish_calls;
static unsigned long publish_generation;
static unsigned publish_w, publish_h;
static unsigned char publish_first[4];

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

void backend_log(const char *msg) { (void)msg; }
const wchar_t *sh_host_dir(void) { return L""; }
const char *sh_megapreview_name_at(int index)
{
    return vmtr_names ? vmtr_names[index] : NULL;
}
int sh_preview_publish(unsigned long generation, const unsigned char *rgba, unsigned w, unsigned h)
{
    publish_calls++;
    publish_generation = generation;
    publish_w = w;
    publish_h = h;
    memcpy(publish_first, rgba, sizeof publish_first);
    return SH_PREVIEW_PUBLISHED;
}

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

static int wwise_owns(const char *value)
{
    uintptr_t start = (uintptr_t)g_wwise;
    uintptr_t at = (uintptr_t)value;
    return g_wwise && at >= start && at - start < g_wwiseBytes;
}

static void write_le32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void write_be32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

/* A large image previews from the first mip that still covers the preview size. */
static void check_mip_choice(void)
{
    enum { HEAD = BIM_MIP_HEAD };
    /* Level 0 and level 1 carry a second face, as a cube map's records do. */
    static const unsigned rec[5][3] = { { 0, 0, 960 }, { 0, 1, 960 }, { 1, 0, 480 }, { 1, 1, 480 },
                                        { 2, 0, 240 } };
    unsigned char image[BIM_MIP0 + 5 * (HEAD + 16)] = {0};
    size_t at = BIM_MIP0, third = 0;
    bim_mip m;
    for (int r = 0; r < 5; ++r) {
        if (r == 4) third = at;
        write_be32(image + at, rec[r][0]);
        write_be32(image + at + 4, rec[r][1]);
        write_be32(image + at + 8, rec[r][2]);
        write_be32(image + at + 0x0C, rec[r][2]);
        write_be32(image + at + 0x10, 16);
        at += HEAD + 16;
    }
    CHECK(bim_pick_mip(image, sizeof image, &m) == 1);
    CHECK(m.w == 240 && m.h == 240 && m.size == 16 && m.face == 0);
    CHECK(m.data == third + HEAD);
    /* A record that is not the next level ends the walk at the last good one. */
    write_be32(image + third, 7);
    CHECK(bim_pick_mip(image, sizeof image, &m) == 1);
    CHECK(m.w == 480);
    /* A mip past the end of the payload is refused. */
    CHECK(bim_pick_mip(image, BIM_MIP0 + HEAD + 8, &m) == 0);
}

static void check_direct_image_preview(void)
{
    unsigned char image[0x46] = {0};
    image[4] = 0x07;
    memcpy(image + 5, "MIB", 3);
    write_le32(image + 0x20, 10);       /* BC1 */
    image[0x34] = 0; image[0x35] = 4;  /* width */
    image[0x38] = 0; image[0x39] = 4;  /* height */
    write_be32(image + 0x3A, 8);
    image[0x3E] = 0x00; image[0x3F] = 0xF8; /* RGB565 red; remaining BC1 bytes are zero */

    FILE *file = NULL;
    CHECK(tmpfile_s(&file) == 0 && file != NULL);
    if (!file) return;
    CHECK(fwrite(image, 1, sizeof image, file) == sizeof image);
    CHECK(fflush(file) == 0);

    set_records(2);
    if (g_rec) {
        /* Installed data contains names present as both Material and Image records. The Material is
         * deliberately unusable here: an Image-category request must skip it and decode record 1. */
        g_rec[0].name = "textures/direct";
        g_rec[0].kind = SH_ASSET_MATERIAL;
        g_rec[0].box = 0;
        g_rec[0].roff = 0;
        g_rec[0].usz = 1;
        g_rec[0].csz = 1;
        g_rec[1].name = "textures/direct";
        g_rec[1].kind = SH_ASSET_IMAGE;
        g_rec[1].box = 0;
        g_rec[1].roff = 0;
        g_rec[1].usz = (unsigned)sizeof image;
        g_rec[1].csz = (unsigned)sizeof image;
        imgpreview_index_names();
    }
    g_box[0].res = (HANDLE)(intptr_t)_get_osfhandle(_fileno(file));
    publish_calls = 0;
    publish_generation = 0;
    publish_w = publish_h = 0;
    memset(publish_first, 0, sizeof publish_first);

    {
        const char *why = NULL;
        CHECK(sh_imgpreview_produce("textures/direct", 76, &why) == SH_PREVIEW_FAILED);
        CHECK(why != NULL && why[0] != '\0');
        CHECK(sh_imgpreview_produce_image("textures/missing", 75, &why) == SH_PREVIEW_FAILED);
        CHECK(why != NULL && strstr(why, "not in the game's files") != NULL);
    }
    CHECK(publish_calls == 0);
    CHECK(sh_imgpreview_produce_image("textures/direct", 77, NULL) == SH_PREVIEW_PUBLISHED);
    CHECK(publish_calls == 1);
    CHECK(publish_generation == 77);
    CHECK(publish_w == 4 && publish_h == 4);
    CHECK(publish_first[0] == 255 && publish_first[1] == 0 &&
          publish_first[2] == 0 && publish_first[3] == 255);

    {
        char out[64];
        CHECK(g_soundLoaded == 0 && g_vmtrLoaded == 0);
        CHECK(sh_imgpreview_list(SH_ASSET_IMAGE, 0, out, sizeof out) == 1);
        CHECK(strcmp(out, "textures/direct\n") == 0);
        CHECK(g_soundLoaded == 0 && g_vmtrLoaded == 0);
    }

    g_box[0].res = NULL;
    fclose(file);
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

    g_loaded = 1;
    check_direct_image_preview();
    check_mip_choice();

    set_records(1);
    if (g_rec) {
        g_rec[0].name = "play_existing";
        g_rec[0].kind = SH_ASSET_SOUND;
        imgpreview_index_names();
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
    CHECK(g_wwiseBytes > 0 && g_wwiseBytes < xml_len + 1u);
    CHECK(_msize((void *)g_ev) < 1024u * sizeof *g_ev);
    CHECK(_msize((void *)g_sb) < 4096u * sizeof *g_sb);
    for (int i = 0; i < g_evCount; ++i) CHECK(wwise_owns(g_ev[i]));
    for (int i = 0; i < g_sbCount; ++i) {
        CHECK(wwise_owns(g_sb[i].name));
        CHECK(wwise_owns(g_sb[i].bank));
    }
    CHECK(bank_for("play_existing") && _stricmp(bank_for("play_existing"), "doom_initial") == 0);
    CHECK(bank_for("play_multi") && _stricmp(bank_for("play_multi"), "doom_monsters") == 0);
    CHECK(bank_for("play_onlybase") && _stricmp(bank_for("play_onlybase"), "doom_initial") == 0);

    {
        char out[2048];
        CHECK(g_soundLoaded == 0);
        int n = sh_imgpreview_list(SH_ASSET_SOUND, 0, out, sizeof out);
        CHECK(g_soundLoaded == 1);
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
        imgpreview_index_names();
    }
    vmtr_names = atlas;
    CHECK(g_vmtrLoaded == 0);
    {
        char out[64];
        int n = sh_imgpreview_list(SH_ASSET_MATERIAL, 0, out, sizeof out);
        CHECK(g_vmtrLoaded == 1);
        CHECK(g_vtCount == 1);
        CHECK(g_vt && _stricmp(g_vt[0], "materials/vt") == 0);
        CHECK(_msize((void *)g_vt) < 1024u * sizeof *g_vt);
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

    /* A page that cannot fit the next name stops there: a shorter name later in the listing
     * must not be written ahead of it, or the next page would skip one name and repeat another. */
    set_records(2);
    if (g_rec) {
        g_rec[0].name = "aaaaaaaaaaaa";
        g_rec[1].name = "bbbbbbbbbbbbbbbbbbbb";
        g_rec[0].kind = g_rec[1].kind = SH_ASSET_SOUND;
        imgpreview_index_names();
    }
    {
        static const char *const events[] = { "c" };
        char page[24], seen[128] = "";
        unsigned start = 0;
        int n, pages = 0;
        free(g_ev);
        g_ev = (const char **)malloc(sizeof events);
        CHECK(g_ev != NULL);
        if (g_ev) { memcpy((void *)g_ev, events, sizeof events); g_evCount = 1; }
        while ((n = sh_imgpreview_list(SH_ASSET_SOUND, start, page, sizeof page)) > 0 && pages < 8) {
            strcat_s(seen, sizeof seen, page);
            start += (unsigned)n;
            pages++;
        }
        CHECK(strcmp(seen, "aaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbbbb\nc\n") == 0);
        CHECK(pages == 2);
    }

    free(g_vt); g_vt = NULL; g_vtCount = 0;
    free(g_sb); g_sb = NULL; g_sbCount = 0;
    free(g_ev); g_ev = NULL; g_evCount = 0;
    free(g_wwise); g_wwise = NULL; g_wwiseBytes = 0;
    g_wwiseSourceBytes = g_wwiseTagBytes = 0;
    free(g_rec); g_rec = NULL; g_recCount = 0;
    free(g_byName); g_byName = NULL;

    if (failures) {
        fprintf(stderr, "%d catalog test(s) failed\n", failures);
        return 1;
    }
    puts("asset catalog tests passed");
    return 0;
}
