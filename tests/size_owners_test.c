/* Copied-size owners (R-321): call-site attribution, per-owner records with their lifetimes, the plan that
 * names fonts to rebuild or owners that make a rebuild wait, the refresh of derived sprite axes, codex
 * widgets and hotBox geometry in either direction, and in-flight records per execution context with their
 * retirement (R-328). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/size_owners.h"
#include "../src/backend/resource_graph.h"
#include "../src/backend/execution_context.h"
#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
#endif

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)

/* Synthetic call sites: one return address per caller class. */
static unsigned char sites[8][16];
static const void *returns[] = {sites[1], sites[2], sites[3], sites[4], sites[5], sites[6], sites[7]};
static const sh_size_site classes[] = {SH_SIZE_SITE_TRANSIENT, SH_SIZE_SITE_SPRITE, SH_SIZE_SITE_FORWARD,
    SH_SIZE_SITE_WIDGET, SH_SIZE_SITE_CREDITS, SH_SIZE_SITE_HOTBOX, SH_SIZE_SITE_FONT};
#define RET(site) ((const void *)sites[site])
enum { AT_TRANSIENT = 1, AT_SPRITE, AT_FORWARD, AT_WIDGET, AT_CREDITS, AT_HOTBOX, AT_FONT, AT_UNKNOWN = 0 };

/* idSWFSpriteInstance fields the module reads and writes: vtable +0, movie +0x20, material +0x60,
 * width +0x68, height +0x6a. */
static unsigned char sprite_vtable[8], other_vtable[8];
typedef struct fake_sprite { unsigned char bytes[0x130]; } fake_sprite;
static fake_sprite sprites[4];
static unsigned char movies[2][0x40], materials[3][0x190];
static void sprite_init(fake_sprite *s, void *movie, void *material, short width, short height)
{
    memset(s, 0, sizeof(*s));
    *(void **)s->bytes = sprite_vtable;
    *(void **)(s->bytes + 0x20) = movie;
    *(void **)(s->bytes + 0x60) = material;
    *(short *)(s->bytes + 0x68) = width;
    *(short *)(s->bytes + 0x6a) = height;
}
static short sprite_width(const fake_sprite *s) { return *(const short *)(s->bytes + 0x68); }
static short sprite_height(const fake_sprite *s) { return *(const short *)(s->bytes + 0x6a); }

/* The measure route: a material's current size and the image it reads. */
static int material_width[3] = {8, 64, 32}, material_height[3] = {8, 64, 16};
static const char *material_image[3] = {"env/placeholder", "gui/icon", "gui/other"};
static int measure_calls, measure_fails;
static int fake_measure(void *material, unsigned axis, int *value, char ***images, size_t *count)
{
    int index = (int)(((unsigned char *)material - materials[0]) / sizeof(materials[0]));
    measure_calls++;
    if (measure_fails) return 0;
    *value = axis == SH_SIZE_AXIS_WIDTH ? material_width[index] : material_height[index];
    *images = malloc(sizeof(char *)); (*images)[0] = _strdup(material_image[index]); *count = 1;
    return 1;
}
/* What the native SetMaterial does with the getter values and the explicit arguments. */
static void set_material(fake_sprite *s, int index, int width, int height, const void *frame)
{
    sh_size_bracket bracket;
    char *image = (char *)material_image[index];
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, s, NULL, frame, width, height);
    *(void **)(s->bytes + 0x60) = materials[index];
    *(short *)(s->bytes + 0x68) = (short)material_width[index];
    CHECK(sh_size_owners_read(RET(AT_SPRITE), NULL, materials[index], SH_SIZE_AXIS_WIDTH, &image, 1));
    *(short *)(s->bytes + 0x6a) = (short)material_height[index];
    CHECK(sh_size_owners_read(RET(AT_SPRITE), NULL, materials[index], SH_SIZE_AXIS_HEIGHT, &image, 1));
    if (width >= 0) *(short *)(s->bytes + 0x68) = (short)width;
    if (height >= 0) *(short *)(s->bytes + 0x6a) = (short)height;
    sh_size_owners_end(&bracket, 1);
}
static int commit_refresh(const char *const *images, char *error)
{
    int ok = sh_size_owners_refresh(images, 1, 0, error, 256);
    sh_size_owners_journal_clear();
    return ok;
}
static void reset(void)
{
    sh_size_owners_test_reset();
    sh_resource_graph_test_reset();
    sh_resource_graph_size_history_begin();
    sh_size_owners_test_sites(returns, classes, sizeof(returns) / sizeof(returns[0]));
    sh_size_owners_enable(fake_measure);
    measure_calls = measure_fails = 0;
    material_width[0] = material_height[0] = 8;
}
static int escaped(const char *image)
{ return (sh_resource_graph_history("image", image) & SH_RESOURCE_GRAPH_HISTORY_ESCAPED) != 0; }
static const char *const placeholder[] = {"env/placeholder"};

/* The real per-renderer tables: every listed site must be a direct call to the getter it names. */
static void table_cases(void)
{
    size_t size = 0x1900000;
    unsigned char *image = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    static const uint32_t vk[] = {0x292c89, 0x292c9a, 0x2ae388, 0x2ae395, 0x2ae8fb, 0x2ae908, 0x4cfc67, 0x4cfc71,
        0x4cfe8f, 0x4cfea9, 0x4d052f, 0x4d0549, 0x4d0901, 0x4d091b, 0x4d0d5d, 0x4d0d77, 0xac78b2, 0xac78bc,
        0xdf800e, 0xdf8019, 0xe477f1, 0xe47801, 0x12e0350, 0x12e035a, 0x1746480, 0x174649e, 0x181a145, 0x181a159};
    static const char vk_getter[] = "hwwhwhwhwhwhwhwhwhwhwhwhwhwh";
    CHECK(image != NULL);
    if (!image) return;
    unsigned char *height = image + 0xd75b40, *width = image + 0xd75d40;
    for (int mode = 0; mode < 3; mode++) {
        sh_size_owners_test_reset();
        memset(image, 0, size);
        for (size_t i = 0; i < sizeof(vk) / sizeof(vk[0]); i++) {
            unsigned char *call = image + vk[i];
            unsigned char *target = vk_getter[i] == 'h' ? height : width;
            /* mode 1: one site calls the other getter; mode 2: one site is not a call. */
            if (mode == 1 && i == 7) target = target == height ? width : height;
            int32_t displacement = (int32_t)(target - (call + 5));
            call[0] = mode == 2 && i == 20 ? 0x90 : 0xe8;
            memcpy(call + 1, &displacement, 4);
        }
        int bound = sh_size_owners_bind_sites(image, 1, height, width, sites, sizeof(sites));
        CHECK(bound == (mode == 0));
        sh_size_owners_enable(fake_measure);
        CHECK(sh_size_owners_enabled() == (mode == 0));
        if (mode) { CHECK(sh_size_owners_site(image + 0x292c89 + 5) == SH_SIZE_SITE_UNKNOWN); continue; }
        CHECK(sh_size_owners_site(image + 0x292c89 + 5) == SH_SIZE_SITE_TRANSIENT);
        CHECK(sh_size_owners_site(image + 0x2ae8fb + 5) == SH_SIZE_SITE_SPRITE);
        CHECK(sh_size_owners_site(image + 0xac78bc + 5) == SH_SIZE_SITE_FORWARD);
        CHECK(sh_size_owners_site(image + 0x12e035a + 5) == SH_SIZE_SITE_WIDGET);
        CHECK(sh_size_owners_site(image + 0xe47801 + 5) == SH_SIZE_SITE_CREDITS);
        CHECK(sh_size_owners_site(image + 0x174649e + 5) == SH_SIZE_SITE_HOTBOX);
        CHECK(sh_size_owners_site(image + 0x181a159 + 5) == SH_SIZE_SITE_FONT);
        /* The call instruction itself is not a return address; this module's own calls only report. */
        CHECK(sh_size_owners_site(image + 0x181a159) == SH_SIZE_SITE_UNKNOWN);
        CHECK(sh_size_owners_site(sites[3] + 1) == SH_SIZE_SITE_TRANSIENT);
        CHECK(sh_size_owners_site(image + 0x1000) == SH_SIZE_SITE_UNKNOWN);
    }
    /* The OpenGL table has its own RVAs: a Vulkan layout does not bind as OpenGL. */
    CHECK(!sh_size_owners_bind_sites(image, 0, height, width, sites, sizeof(sites)));
    VirtualFree(image, 0, MEM_RELEASE);
    sh_size_owners_test_reset();
}

static void sprite_cases(void)
{
    char error[256];
    /* Derived axes recompute after the image changes; an explicit axis keeps the author's value. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    set_material(&sprites[0], 0, -1, 40, NULL);
    CHECK(sprite_width(&sprites[0]) == 8 && sprite_height(&sprites[0]) == 40 && sh_size_owners_count() == 1);
    material_width[0] = 256; material_height[0] = 128;
    CHECK(commit_refresh(placeholder, error));
    CHECK(sprite_width(&sprites[0]) == 256 && sprite_height(&sprites[0]) == 40 && measure_calls == 1);
    /* The return direction recomputes from the record the refresh wrote. */
    material_width[0] = 8;
    CHECK(commit_refresh(placeholder, error) && sprite_width(&sprites[0]) == 8);
    CHECK(!escaped("env/placeholder"));

    /* Both axes explicit, or a material without an image: nothing depends on an image. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    set_material(&sprites[0], 0, 10, 20, NULL);
    CHECK(sh_size_owners_count() == 0);
    material_width[0] = 256;
    CHECK(commit_refresh(placeholder, error) && sprite_width(&sprites[0]) == 10);

    /* A later SetMaterial replaces the provenance; the script setters make an axis explicit. */
    reset();
    sprite_init(&sprites[1], movies[0], NULL, 0, 0);
    set_material(&sprites[1], 0, -1, -1, NULL);
    unsigned char script_object[0xc8] = {0};
    *(void **)(script_object + 0xc0) = &sprites[1];
    *(short *)(sprites[1].bytes + 0x68) = 99;
    sh_size_owners_explicit(script_object, SH_SIZE_AXIS_WIDTH, 1);
    material_width[0] = material_height[0] = 512;
    CHECK(commit_refresh(placeholder, error));
    CHECK(sprite_width(&sprites[1]) == 99 && sprite_height(&sprites[1]) == 512);
    sh_size_owners_explicit(script_object, SH_SIZE_AXIS_HEIGHT, 1);
    CHECK(sh_size_owners_count() == 0);
    /* A setter that raised leaves the provenance unknown: the record is dropped and its reads escape. */
    reset();
    sprite_init(&sprites[1], movies[0], NULL, 0, 0);
    set_material(&sprites[1], 0, -1, -1, NULL);
    sh_size_owners_explicit(script_object, SH_SIZE_AXIS_WIDTH, 0);
    CHECK(sh_size_owners_count() == 0 && escaped("env/placeholder"));

    /* An axis an unseen routine rewrote is kept as written; a changed material is not refreshed. */
    reset();
    sprite_init(&sprites[2], movies[0], NULL, 0, 0);
    set_material(&sprites[2], 0, -1, -1, NULL);
    *(short *)(sprites[2].bytes + 0x6a) = 77;
    material_width[0] = material_height[0] = 300;
    CHECK(commit_refresh(placeholder, error));
    CHECK(sprite_width(&sprites[2]) == 300 && sprite_height(&sprites[2]) == 77);
    *(void **)(sprites[2].bytes + 0x60) = materials[1];
    CHECK(!commit_refresh(placeholder, error) &&
        strstr(error, "material changed outside its recorded writers"));
    /* A failed measurement writes nothing and keeps the record for the rollback. */
    reset();
    sprite_init(&sprites[2], movies[0], NULL, 0, 0);
    set_material(&sprites[2], 0, -1, -1, NULL);
    material_width[0] = 300; measure_fails = 1;
    CHECK(!commit_refresh(placeholder, error) && strstr(error, "could not be measured"));
    CHECK(sprite_width(&sprites[2]) == 8 && sh_size_owners_count() == 1);
    measure_fails = 0;
    CHECK(commit_refresh(placeholder, error) && sprite_width(&sprites[2]) == 300);

    /* Lifetimes: the sprite's own destruction, its movie's, or an unobserved one (another object now). */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    sprite_init(&sprites[1], movies[1], NULL, 0, 0);
    set_material(&sprites[0], 0, -1, -1, NULL);
    set_material(&sprites[1], 0, -1, -1, NULL);
    CHECK(sh_size_owners_count() == 2);
    sh_size_owners_sprite_destroyed(&sprites[0]);
    CHECK(sh_size_owners_count() == 1);
    sh_size_owners_movie_destroyed(movies[1]);
    CHECK(sh_size_owners_count() == 0);
    set_material(&sprites[1], 0, -1, -1, NULL);
    *(void **)sprites[1].bytes = other_vtable;
    material_width[0] = 999;
    CHECK(commit_refresh(placeholder, error) && sprite_width(&sprites[1]) == 8);
    CHECK(sh_size_owners_count() == 0);
    /* An image the record never read is not refreshed. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    set_material(&sprites[0], 1, -1, -1, NULL);
    material_width[1] = 1000;
    CHECK(commit_refresh(placeholder, error) && sprite_width(&sprites[0]) == 64);
    material_width[1] = 64;
}

static void forward_cases(void)
{
    char error[256];
    char *image = (char *)material_image[0];
    unsigned char frame[2][8];
    /* The medal reads both sizes and passes them to SetMaterial from the same frame: derived. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    CHECK(sh_size_owners_read(RET(AT_FORWARD), frame[0], materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    CHECK(sh_size_owners_read(RET(AT_FORWARD), frame[0], materials[0], SH_SIZE_AXIS_HEIGHT, &image, 1));
    set_material(&sprites[0], 0, 8, 8, frame[0]);
    material_width[0] = material_height[0] = 64;
    CHECK(commit_refresh(placeholder, error));
    CHECK(sprite_width(&sprites[0]) == 64 && sprite_height(&sprites[0]) == 64 && !escaped("env/placeholder"));
    /* Reads another frame's SetMaterial does not take cannot be shown to stop at a refreshable copy. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    CHECK(sh_size_owners_read(RET(AT_FORWARD), frame[0], materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    set_material(&sprites[0], 1, 8, 8, frame[1]);
    CHECK(escaped("env/placeholder"));
}

static void owner_cases(void)
{
    char error[512], **fonts = NULL;
    size_t font_count = 0;
    char *image = (char *)material_image[0];
    sh_size_bracket bracket, nested;
    unsigned char script_object[0xc8] = {0};
    /* Transient and unknown callers. */
    reset();
    CHECK(sh_size_owners_read(RET(AT_TRANSIENT), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    CHECK(!sh_size_owners_read(RET(AT_UNKNOWN), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    CHECK(sh_size_owners_count() == 0);
    /* A sprite-writer site outside its bracket, or a font site inside another owner's, is not taken. */
    CHECK(!sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_HOTBOX, &sprites[3], NULL, NULL, -1, -1);
    CHECK(!sh_size_owners_read(RET(AT_FONT), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);

    /* A font that copied the size is named for the rebuild. */
    reset();
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_FONT, NULL, "korataki rg", NULL, -1, -1);
    CHECK(sh_size_owners_read(RET(AT_FONT), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);
    CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)));
    CHECK(font_count == 1 && fonts && !_stricmp(fonts[0], "korataki rg"));
    for (size_t i = 0; i < font_count; i++) free(fonts[i]);
    free(fonts); fonts = NULL;
    static const char *const other[] = {"gui/other"};
    CHECK(sh_size_owners_plan(other, 1, &fonts, &font_count, error, sizeof(error)) && !font_count);
    /* A read that raised merges with what the font copied earlier; a re-read replaces it. */
    char *second = (char *)material_image[2];
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_FONT, NULL, "korataki rg", NULL, -1, -1);
    CHECK(sh_size_owners_read(RET(AT_FONT), NULL, materials[2], SH_SIZE_AXIS_WIDTH, &second, 1));
    sh_size_owners_end(&bracket, 1);
    CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)) && !font_count);

    /* Script reads of a derived axis copy it into the movie's script state: the rebuild waits for the movie. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    set_material(&sprites[0], 0, -1, -1, NULL);
    *(void **)(script_object + 0xc0) = &sprites[0];
    sh_size_owners_script_read(script_object, SH_SIZE_AXIS_HEIGHT);
    CHECK(!sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)) &&
        strstr(error, "movie script") && strstr(error, "env/placeholder"));
    sh_size_owners_movie_destroyed(movies[0]);
    CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)));
    /* A read of an explicit axis copies no image size. */
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    set_material(&sprites[0], 0, 5, -1, NULL);
    sh_size_owners_script_read(script_object, SH_SIZE_AXIS_WIDTH);
    CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)));

    /* A credits item layout has no native refresh: it waits until the item sprite or its movie is gone. The
     * layout's movie is the movie of the sprite its SetMaterial wrote. */
    reset();
    sprite_init(&sprites[1], movies[1], NULL, 0, 0);
    sprite_init(&sprites[2], movies[1], NULL, 0, 0);
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_CREDITS, &sprites[1], NULL, NULL, -1, -1);
    set_material(&sprites[2], 1, -1, 5, NULL);
    CHECK(sh_size_owners_read(RET(AT_CREDITS), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);
    CHECK(!sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)) && strstr(error, "credits item"));
    sh_size_owners_sprite_destroyed(&sprites[1]);
    CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)));
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_CREDITS, &sprites[1], NULL, NULL, -1, -1);
    sh_size_owners_begin(&nested, SH_SIZE_BRACKET_SPRITE, &sprites[2], NULL, NULL, 1, 1);
    sh_size_owners_end(&nested, 1);
    CHECK(sh_size_owners_read(RET(AT_CREDITS), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);
    sh_size_owners_movie_destroyed(movies[1]);
    CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)));
}

/* hotBox: the refresh returns the saved scale to a state the native update cannot match. */
static unsigned char hotbox_vtable[8];
static unsigned char hotbox[0x1000];
static void hotbox_cases(void)
{
    char error[256], *image = (char *)material_image[0];
    sh_size_bracket bracket;
    float saved[3] = {1.0f, 1.0f, 1.0f};
    reset();
    sh_size_owners_hotbox_layout(0xfc0);
    memset(hotbox, 0, sizeof(hotbox)); *(void **)hotbox = hotbox_vtable;
    memcpy(hotbox + 0xfc0, saved, sizeof(saved));
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_HOTBOX, hotbox, NULL, NULL, -1, -1);
    CHECK(sh_size_owners_read(RET(AT_HOTBOX), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);
    CHECK(commit_refresh(placeholder, error));
    for (int i = 0; i < 3; i++) {
        float value; memcpy(&value, hotbox + 0xfc0 + 4 * i, 4);
        CHECK(value != value); /* NaN: never equal to the current scale */
    }
    /* Destroyed, or no longer a hotBox: nothing is written. */
    memcpy(hotbox + 0xfc0, saved, sizeof(saved));
    sh_size_owners_hotbox_destroyed(hotbox);
    CHECK(commit_refresh(placeholder, error) && !memcmp(hotbox + 0xfc0, saved, sizeof(saved)));
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_HOTBOX, hotbox, NULL, NULL, -1, -1);
    CHECK(sh_size_owners_read(RET(AT_HOTBOX), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);
    *(void **)hotbox = other_vtable;
    CHECK(commit_refresh(placeholder, error) && !memcmp(hotbox + 0xfc0, saved, sizeof(saved)));
    CHECK(sh_size_owners_count() == 0);
    /* Without the decoded layout the refresh cannot return the geometry: it fails rather than skip it. */
    reset();
    *(void **)hotbox = hotbox_vtable;
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_HOTBOX, hotbox, NULL, NULL, -1, -1);
    CHECK(sh_size_owners_read(RET(AT_HOTBOX), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);
    CHECK(!commit_refresh(placeholder, error) && strstr(error, "hotBox"));
}

/* Codex widgets: the refresh re-runs the widget's own update (vtable slot 3), which lays out again. */
static int widget_updates, widget_reads;
static unsigned char widget[0x40];
static void widget_update(void *self)
{
    sh_size_bracket bracket;
    char *image = (char *)material_image[0];
    CHECK(self == widget);
    widget_updates++;
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_WIDGET, self, NULL, NULL, -1, -1);
    if (widget_reads) CHECK(sh_size_owners_read(RET(AT_WIDGET), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);
}
static void (*widget_table[8])(void *) = {NULL, NULL, NULL, widget_update};
static void widget_cases(void)
{
    char error[256], **fonts = NULL;
    size_t font_count = 0;
    reset();
    *(void **)widget = widget_table;
    widget_reads = 1; widget_updates = 0;
    widget_update(widget);
    CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)) && !font_count);
    CHECK(commit_refresh(placeholder, error) && widget_updates == 2);
    /* An update that lays nothing out (a hidden widget) keeps the earlier copy's owner. */
    widget_reads = 0;
    CHECK(commit_refresh(placeholder, error) && widget_updates == 3);
    CHECK(commit_refresh(placeholder, error) && widget_updates == 4);
    /* Destroyed, or another object at the address: not called. */
    sh_size_owners_widget_destroyed(widget);
    CHECK(commit_refresh(placeholder, error) && widget_updates == 4);
    widget_reads = 1; widget_update(widget);
    *(void **)widget = other_vtable;
    CHECK(commit_refresh(placeholder, error) && widget_updates == 5);
    CHECK(sh_size_owners_count() == 0);
}

/* Many sprite records: each is found, retired and refreshed through the index, with deletions in an order
 * that moves records and shifts probe chains. */
static fake_sprite many[600];
static void index_cases(void)
{
    char error[256];
    reset();
    for (int i = 0; i < 600; i++) {
        sprite_init(&many[i], movies[i % 2], NULL, 0, 0);
        set_material(&many[i], 0, -1, -1, NULL);
    }
    CHECK(sh_size_owners_count() == 600);
    for (int i = 0; i < 600; i += 3) sh_size_owners_sprite_destroyed(&many[i]);
    CHECK(sh_size_owners_count() == 400);
    for (int i = 0; i < 600; i += 3) sh_size_owners_sprite_destroyed(&many[i]); /* already gone */
    CHECK(sh_size_owners_count() == 400);
    material_width[0] = 77;
    CHECK(commit_refresh(placeholder, error));
    int refreshed = 0, kept = 0;
    for (int i = 0; i < 600; i++) {
        if (i % 3 == 0) kept += sprite_width(&many[i]) == 8;
        else refreshed += sprite_width(&many[i]) == 77;
    }
    CHECK(refreshed == 400 && kept == 200);
    sh_size_owners_movie_destroyed(movies[0]);
    CHECK(sh_size_owners_count() == 200);
    for (int i = 1; i < 600; i += 2) if (i % 3) sh_size_owners_sprite_destroyed(&many[i]);
    CHECK(sh_size_owners_count() == 0);
    material_width[0] = 8;
}

/* Rollback (root-journal-recovery.md). A restoring pass writes back exactly what the failed forward pass
 * changed, without measuring or allocating. A write that fails keeps its obligation and fails the restoration,
 * so a retry can finish it; a value another writer changed meanwhile is preserved, not overwritten; a destroyed
 * sprite's obligation is obsolete. A forward pass refuses while a failed restoration still owns values. */
static fake_sprite *page_sprite(void)
{
    return (fake_sprite *)VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
static void journal_cases(void)
{
    char error[256];
    DWORD old;
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    sprite_init(&sprites[1], movies[0], NULL, 0, 0);
    set_material(&sprites[0], 0, -1, -1, NULL);
    set_material(&sprites[1], 0, -1, 30, NULL);
    material_width[0] = 256; material_height[0] = 128;
    CHECK(sh_size_owners_refresh(placeholder, 1, 0, error, sizeof(error)));
    CHECK(sprite_width(&sprites[0]) == 256 && sprite_height(&sprites[0]) == 128 && sprite_width(&sprites[1]) == 256);
    CHECK(sh_size_owners_journal_count() == 3 && sh_size_owners_journal_pending() == 3); /* sprite 1's explicit height was not written */
    material_width[0] = material_height[0] = 8; measure_fails = 1;
    int before = measure_calls;
    CHECK(sh_size_owners_refresh(placeholder, 1, 1, error, sizeof(error)));
    CHECK(measure_calls == before && sh_size_owners_journal_pending() == 0 && sh_size_owners_journal_count() == 3);
    CHECK(sprite_width(&sprites[0]) == 8 && sprite_height(&sprites[0]) == 8 && sprite_width(&sprites[1]) == 8 &&
        sprite_height(&sprites[1]) == 30);
    measure_fails = 0;
    sh_size_owners_journal_clear(); /* the restoration succeeded */
    /* The records took the restored values: a later forward pass refreshes them again. */
    material_width[0] = 64;
    CHECK(sh_size_owners_refresh(placeholder, 1, 0, error, sizeof(error)) && sprite_width(&sprites[0]) == 64);
    /* A value rewritten after the forward pass is preserved, distinct from a failed write: the restoration
     * succeeds and leaves it. */
    *(short *)(sprites[1].bytes + 0x68) = 5;
    CHECK(sh_size_owners_refresh(placeholder, 1, 1, error, sizeof(error)) && !error[0]);
    CHECK(sprite_width(&sprites[1]) == 5 && sprite_width(&sprites[0]) == 8 && !sh_size_owners_journal_pending());
    sh_size_owners_journal_clear();
    material_width[0] = 8;

    /* A failed write keeps its obligation and fails the restoration; a forward pass refuses meanwhile; the
     * retry finishes it. */
    reset();
    fake_sprite *paged = page_sprite();
    CHECK(paged != NULL);
    if (!paged) return;
    sprite_init(paged, movies[0], NULL, 0, 0);
    set_material(paged, 0, -1, -1, NULL);
    material_width[0] = 256; material_height[0] = 128;
    CHECK(sh_size_owners_refresh(placeholder, 1, 0, error, sizeof(error)) && sprite_width(paged) == 256);
    CHECK(VirtualProtect(paged, 0x1000, PAGE_READONLY, &old));
    material_width[0] = material_height[0] = 8; measure_fails = 1;
    CHECK(!sh_size_owners_refresh(placeholder, 1, 1, error, sizeof(error)) && strstr(error, "stay pending"));
    CHECK(sh_size_owners_journal_pending() == 2 && sprite_width(paged) == 256 && sprite_height(paged) == 128);
    CHECK(!sh_size_owners_refresh(placeholder, 1, 0, error, sizeof(error)) && strstr(error, "still owns 2"));
    CHECK(VirtualProtect(paged, 0x1000, PAGE_READWRITE, &old));
    CHECK(sh_size_owners_refresh(placeholder, 1, 1, error, sizeof(error)) && !sh_size_owners_journal_pending());
    CHECK(sprite_width(paged) == 8 && sprite_height(paged) == 128 - 120);
    sh_size_owners_journal_clear();
    measure_fails = 0;
    /* A destroyed sprite's obligation is obsolete: nothing is written to its memory. */
    sprite_init(paged, movies[0], NULL, 0, 0);
    set_material(paged, 0, -1, -1, NULL);
    material_width[0] = 256;
    CHECK(sh_size_owners_refresh(placeholder, 1, 0, error, sizeof(error)) && sprite_width(paged) == 256);
    sh_size_owners_sprite_destroyed(paged);
    CHECK(!sh_size_owners_journal_pending());
    CHECK(sh_size_owners_refresh(placeholder, 1, 1, error, sizeof(error)) && sprite_width(paged) == 256);
    sh_size_owners_journal_clear();
    VirtualFree(paged, 0, MEM_RELEASE);
    material_width[0] = material_height[0] = 8;
}

/* Names compare as the resource graph folds them (the resident pass passes folded names). */
static void fold_cases(void)
{
    char error[256], **fonts = NULL, *image = "ENV\\Placeholder";
    size_t count = 0;
    sh_size_bracket bracket;
    reset();
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_FONT, NULL, "Fold RG", NULL, -1, -1);
    CHECK(sh_size_owners_read(RET(AT_FONT), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&bracket, 1);
    CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &count, error, sizeof(error)) && count == 1);
    for (size_t i = 0; i < count; i++) free(fonts[i]);
    free(fonts);
}

/* The getter's read collection: each call's finds and editor image, attributed through the caller's owner. */
static void collect_cases(void)
{
    sh_size_bracket owner, call, nested, font;
    char **names = NULL;
    size_t count = 0;
    reset();
    CHECK(!sh_size_owners_collecting());
    sh_size_owners_note("env/placeholder"); /* outside a collection: nothing */
    /* Hand-over to the measurement, deduplicated. */
    sh_size_owners_collect_begin(&call, materials[0], SH_SIZE_AXIS_WIDTH);
    CHECK(sh_size_owners_collecting());
    sh_size_owners_note("env/placeholder"); sh_size_owners_note("ENV/placeholder"); sh_size_owners_note("gui/icon");
    CHECK(sh_size_owners_collect_end(&call, 1, NULL, NULL, &names, &count) && count == 2);
    CHECK(!sh_size_owners_collecting() && sh_size_owners_test_contexts() == 0);
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    /* An unidentified read, or a getter that raised, fails the call: its reads stay with each image. */
    for (int mode = 0; mode < 2; mode++) {
        reset();
        sh_size_owners_collect_begin(&call, materials[0], SH_SIZE_AXIS_WIDTH);
        sh_size_owners_note("env/placeholder");
        if (!mode) sh_size_owners_note(NULL);
        CHECK(!sh_size_owners_collect_end(&call, mode == 0, RET(AT_FONT), NULL, NULL, NULL));
        CHECK(escaped("env/placeholder"));
        sh_size_owners_collect_begin(&call, materials[0], SH_SIZE_AXIS_WIDTH);
        sh_size_owners_note("gui/icon");
        if (!mode) sh_size_owners_note(NULL);
        CHECK(!sh_size_owners_collect_end(&call, mode == 0, NULL, NULL, &names, &count) && !names && !count);
    }
    /* Attribution through the caller's owner: a sprite writer's getter call inside SetMaterial. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    sh_size_owners_begin(&owner, SH_SIZE_BRACKET_SPRITE, &sprites[0], NULL, NULL, -1, -1);
    *(void **)(sprites[0].bytes + 0x60) = materials[0];
    sh_size_owners_collect_begin(&call, materials[0], SH_SIZE_AXIS_WIDTH);
    sh_size_owners_note("env/placeholder");
    /* A font loaded inside the getter keeps its own reads; they are not the outer getter's. */
    sh_size_owners_begin(&font, SH_SIZE_BRACKET_FONT, NULL, "nested rg", NULL, -1, -1);
    sh_size_owners_note("gui/other");
    sh_size_owners_collect_begin(&nested, materials[2], SH_SIZE_AXIS_WIDTH);
    sh_size_owners_note("gui/other");
    CHECK(sh_size_owners_collect_end(&nested, 1, RET(AT_FONT), NULL, NULL, NULL));
    sh_size_owners_end(&font, 1);
    CHECK(sh_size_owners_collect_end(&call, 1, RET(AT_SPRITE), NULL, NULL, NULL));
    sh_size_owners_end(&owner, 1);
    CHECK(!escaped("env/placeholder") && !escaped("gui/other") && sh_size_owners_count() == 2);
    {
        char error[256], **fonts = NULL;
        size_t font_count = 0;
        static const char *const other[] = {"gui/other"};
        CHECK(sh_size_owners_plan(other, 1, &fonts, &font_count, error, sizeof(error)) && font_count == 1);
        for (size_t i = 0; i < font_count; i++) free(fonts[i]);
        free(fonts); fonts = NULL;
        CHECK(sh_size_owners_plan(placeholder, 1, &fonts, &font_count, error, sizeof(error)) && !font_count);
        /* The nested font's reads are not the outer getter's: the sprite does not refresh on them. */
        int before = measure_calls;
        CHECK(commit_refresh(other, error) && measure_calls == before);
        material_width[0] = 128;
        CHECK(commit_refresh(placeholder, error) && sprite_width(&sprites[0]) == 128);
    }
    /* No owner of the caller's kind on this stack: the reads stay with each image. */
    reset();
    sh_size_owners_collect_begin(&call, materials[0], SH_SIZE_AXIS_WIDTH);
    sh_size_owners_note("env/placeholder");
    CHECK(!sh_size_owners_collect_end(&call, 1, RET(AT_SPRITE), NULL, NULL, NULL) && escaped("env/placeholder"));
}

typedef struct stack_range { const void *low, *high; } stack_range;
static stack_range stack_now(void)
{
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    stack_range range = {tib->StackLimit, tib->StackBase};
    return range;
}
/* A fiber deleted, or a thread ended, inside its frames leaves their stack poisoning for AddressSanitizer; a later
 * stack at the same addresses would report it. */
static void stack_forget(stack_range range)
{
#ifdef __SANITIZE_ADDRESS__
    if (range.high > range.low)
        __asan_unpoison_memory_region((void *)range.low, (size_t)((const char *)range.high - (const char *)range.low));
#else
    (void)range;
#endif
}
/* Execution context (root's control, m130/root-size-owner-execution-context.md). Brackets, collections and
 * forward reads belong to the execution stack. Fibers A and B interleave on one thread out of LIFO order:
 * A begins, B begins, A resumes, reads and ends, then B resumes and ends. A thread-keyed chain would hand A's
 * read to B and leave B's parent pointing at A's returned frame. */
static void *main_fiber, *fiber_a, *fiber_b, *thread_fiber;
static int fiber_mode;
static int fiber_ok_a, fiber_ok_b;
static unsigned char shared_frame[8];
static stack_range a_stack, b_stack;
static void fiber_switch(void *to) { SwitchToFiber(to); }
static void CALLBACK fiber_a_body(void *unused)
{
    sh_size_bracket bracket, call;
    char *image = (char *)material_image[0];
    (void)unused;
    a_stack = stack_now();
    if (fiber_mode == 0) {
        /* Sprite writers: A's getter read must land in A's bracket. */
        sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, &sprites[0], NULL, NULL, -1, -1);
        *(void **)(sprites[0].bytes + 0x60) = materials[0];
        fiber_switch(fiber_b);
        fiber_ok_a = sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1);
        sh_size_owners_end(&bracket, 1);
        fiber_switch(fiber_b);
    } else if (fiber_mode == 1) {
        /* Collections inside fonts: each fiber's notes stay in its own collection. */
        sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_FONT, NULL, "font a", NULL, -1, -1);
        sh_size_owners_collect_begin(&call, materials[0], SH_SIZE_AXIS_WIDTH);
        fiber_switch(fiber_b);
        sh_size_owners_note(material_image[0]);
        fiber_ok_a = sh_size_owners_collect_end(&call, 1, RET(AT_FONT), NULL, NULL, NULL);
        sh_size_owners_end(&bracket, 1);
        fiber_switch(fiber_b);
    } else if (fiber_mode == 2) {
        /* Forward reads wait in A's context: B's SetMaterial from an equal frame value does not take them. */
        fiber_ok_a = sh_size_owners_read(RET(AT_FORWARD), shared_frame, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1) &&
            sh_size_owners_read(RET(AT_FORWARD), shared_frame, materials[0], SH_SIZE_AXIS_HEIGHT, &image, 1);
        fiber_switch(fiber_b);
        set_material(&sprites[0], 0, 8, 8, shared_frame);
        fiber_switch(fiber_b);
    } else {
        /* Resumed on another thread: the bracket follows the fiber's stack. */
        sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, &sprites[0], NULL, NULL, -1, -1);
        *(void **)(sprites[0].bytes + 0x60) = materials[0];
        fiber_switch(main_fiber);
        fiber_ok_a = sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1);
        sh_size_owners_end(&bracket, 1);
        fiber_switch(thread_fiber);
    }
    for (;;) fiber_switch(main_fiber);
}
static void CALLBACK fiber_b_body(void *unused)
{
    sh_size_bracket bracket, call;
    char *image = (char *)material_image[1];
    (void)unused;
    b_stack = stack_now();
    if (fiber_mode == 0) {
        sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, &sprites[1], NULL, NULL, -1, -1);
        *(void **)(sprites[1].bytes + 0x60) = materials[1];
        fiber_switch(fiber_a);
        fiber_ok_b = sh_size_owners_read(RET(AT_SPRITE), NULL, materials[1], SH_SIZE_AXIS_HEIGHT, &image, 1);
        sh_size_owners_end(&bracket, 1);
    } else if (fiber_mode == 1) {
        sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_FONT, NULL, "font b", NULL, -1, -1);
        sh_size_owners_collect_begin(&call, materials[1], SH_SIZE_AXIS_WIDTH);
        sh_size_owners_note(material_image[1]);
        fiber_switch(fiber_a);
        fiber_ok_b = sh_size_owners_collect_end(&call, 1, RET(AT_FONT), NULL, NULL, NULL);
        sh_size_owners_end(&bracket, 1);
    } else if (fiber_mode == 2) {
        set_material(&sprites[1], 1, 8, 8, shared_frame);
        fiber_ok_b = 1;
        fiber_switch(fiber_a);
    }
    for (;;) fiber_switch(main_fiber);
}
static DWORD WINAPI resume_thread(void *unused)
{
    (void)unused;
    thread_fiber = ConvertThreadToFiber(NULL);
    if (!thread_fiber) return 1;
    SwitchToFiber(fiber_a);
    ConvertFiberToThread();
    return 0;
}
static int font_planned(const char *image, const char *font)
{
    char error[256], **fonts = NULL;
    size_t count = 0;
    const char *const one[] = {image};
    int found = 0;
    if (!sh_size_owners_plan(one, 1, &fonts, &count, error, sizeof(error))) return -1;
    for (size_t i = 0; i < count; i++) { found += !_stricmp(fonts[i], font); free(fonts[i]); }
    free(fonts);
    return (int)count == found ? found : -1;
}
static void fiber_cases(void)
{
    char error[256];
    main_fiber = ConvertThreadToFiber(NULL);
    CHECK(main_fiber != NULL);
    if (!main_fiber) return;
    for (fiber_mode = 0; fiber_mode < 4; fiber_mode++) {
        reset();
        sprite_init(&sprites[0], movies[0], NULL, 8, 8);
        sprite_init(&sprites[1], movies[1], NULL, 64, 64);
        fiber_ok_a = fiber_ok_b = 0;
        a_stack.low = a_stack.high = NULL; b_stack = a_stack;
        fiber_a = CreateFiber(0, fiber_a_body, NULL);
        fiber_b = CreateFiber(0, fiber_b_body, NULL);
        CHECK(fiber_a && fiber_b);
        if (!fiber_a || !fiber_b) break;
        if (fiber_mode < 3) {
            SwitchToFiber(fiber_a);
            /* Both fibers have finished their routines and wait at the main fiber. */
        } else {
            SwitchToFiber(fiber_a);          /* A begins its bracket and comes back here */
            HANDLE thread = CreateThread(NULL, 0, resume_thread, NULL, 0, NULL);
            CHECK(thread != NULL);
            if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
            fiber_ok_b = 1;
        }
        CHECK(fiber_ok_a && fiber_ok_b);
        CHECK(sh_size_owners_test_contexts() == 0);
        CHECK(!escaped("env/placeholder") && !escaped("gui/icon"));
        if (fiber_mode == 0 || fiber_mode == 3) {
            /* A's sprite owns its width read; B's (mode 0) its height read: each refreshes on its own image. */
            material_width[0] = 256;
            CHECK(commit_refresh(placeholder, error));
            CHECK(sprite_width(&sprites[0]) == 256);
            if (fiber_mode == 0) {
                static const char *const icon[] = {"gui/icon"};
                material_height[1] = 128;
                CHECK(commit_refresh(icon, error) && sprite_height(&sprites[1]) == 128);
                CHECK(sprite_width(&sprites[1]) == 64);
                material_height[1] = 64;
            }
        } else if (fiber_mode == 1) {
            CHECK(font_planned("env/placeholder", "font a") == 1);
            CHECK(font_planned("gui/icon", "font b") == 1);
        } else {
            /* A's SetMaterial took A's forward reads (derived); B's explicit axes stayed explicit. */
            material_width[0] = material_height[0] = 300;
            CHECK(commit_refresh(placeholder, error));
            CHECK(sprite_width(&sprites[0]) == 300 && sprite_height(&sprites[0]) == 300);
            CHECK(sprite_width(&sprites[1]) == 8 && sprite_height(&sprites[1]) == 8);
        }
        DeleteFiber(fiber_a); DeleteFiber(fiber_b);
        /* Deleted while suspended: their frames' stack poisoning goes, as later fibers reuse the addresses. */
        stack_forget(a_stack); stack_forget(b_stack);
        fiber_a = fiber_b = NULL;
    }
    ConvertFiberToThread();
    main_fiber = NULL;
}
/* A bracket whose frame never ended (its handle lost or overwritten) stays the innermost record: reads of its kind go
 * to it, never to an outer owner, and the outer end drops it as a routine that raised, so every read either took
 * escapes. A handle never begun, a copy of one already ended, or another context's call ends nothing. */
static sh_size_bracket *foreign;
static DWORD WINAPI end_foreign(void *unused)
{
    (void)unused;
    sh_size_owners_end(foreign, 1);
    return 0;
}
static void chain_cases(void)
{
    sh_size_bracket outer, inner, copy, never = {0};
    char *image = (char *)material_image[0];
    HANDLE thread;
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    sprite_init(&sprites[1], movies[0], NULL, 0, 0);
    sh_size_owners_begin(&outer, SH_SIZE_BRACKET_SPRITE, &sprites[0], NULL, NULL, -1, -1);
    *(void **)(sprites[0].bytes + 0x60) = materials[0];
    CHECK(sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_begin(&inner, SH_SIZE_BRACKET_SPRITE, &sprites[1], NULL, NULL, -1, -1);
    *(void **)(sprites[1].bytes + 0x60) = materials[0];
    memset(&inner, 0, sizeof(inner));
    /* The lost bracket takes the read of its kind: it does not reach the outer sprite. */
    CHECK(sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&inner, 1);
    CHECK(sh_size_owners_test_records() == 2);
    /* The outer end drops it and reports out of order: both reads escape and neither sprite keeps a record. */
    sh_size_owners_end(&outer, 1);
    CHECK(sh_size_owners_count() == 0 && sh_size_owners_test_contexts() == 0 && sh_size_owners_test_records() == 0);
    CHECK(escaped("env/placeholder"));
    /* Never begun, ended by another context, or ended twice through a copy: only the bracket's own end counts. */
    reset();
    sh_size_owners_end(&never, 1);
    sh_size_owners_begin(&outer, SH_SIZE_BRACKET_WIDGET, widget, NULL, NULL, -1, -1);
    copy = outer;
    foreign = &outer;
    thread = CreateThread(NULL, 0, end_foreign, NULL, 0, NULL);
    CHECK(thread != NULL);
    if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    CHECK(sh_size_owners_test_records() == 1);
    CHECK(sh_size_owners_read(RET(AT_WIDGET), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&outer, 1);
    sh_size_owners_end(&copy, 1);
    CHECK(sh_size_owners_count() == 1 && sh_size_owners_test_records() == 0 && !escaped("env/placeholder"));
    /* Ordinary nesting on one context still attributes to the innermost owner of the right kind. */
    reset();
    sh_size_owners_begin(&outer, SH_SIZE_BRACKET_WIDGET, widget, NULL, NULL, -1, -1);
    sh_size_owners_begin(&inner, SH_SIZE_BRACKET_SPRITE, &sprites[0], NULL, NULL, -1, -1);
    *(void **)(sprites[0].bytes + 0x60) = materials[0];
    CHECK(sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    CHECK(!sh_size_owners_read(RET(AT_WIDGET), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&inner, 1);
    CHECK(sh_size_owners_read(RET(AT_WIDGET), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_end(&outer, 1);
    CHECK(sh_size_owners_count() == 2 && sh_size_owners_test_contexts() == 0);
}

/* Retirement (R-328). The registry this replaces kept a context by its TIB StackBase: a fiber deleted inside its
 * brackets left the entry, its chain pointing into the freed stack, its pending reads never escaping and its
 * collection still counted. Records are now this module's, per context owned through fiber local storage, whose
 * callback queues the context on deletion, exit or index release; the next lookup by a live context retires its
 * records: open brackets end as routines that raised (a font keeps
 * what it copied, as a raised read does; other reads escape), pending reads escape and collections close, without
 * the stack. Identifier reuse is synthetic here: production identifiers are never reused. */
static void test_retire(uint64_t context) { (void)context; }
static sh_execution_owner test_owner = {test_retire};
static stack_range held_stack, probe_stack, exit_stack;
static sh_size_bracket held_font, held_call;
static uint64_t held_context;
static int probe_ok;
static void CALLBACK held_fiber(void *unused)
{
    sh_size_bracket font, call;
    char *forward = (char *)material_image[0], *image = (char *)material_image[1];
    (void)unused;
    /* A forward read waits for its frame's next SetMaterial, a font reads a size, and a getter call inside the font
     * collects; the fiber then stays suspended inside all of it. */
    CHECK(sh_size_owners_read(RET(AT_FORWARD), shared_frame, materials[0], SH_SIZE_AXIS_WIDTH, &forward, 1));
    sh_size_owners_begin(&font, SH_SIZE_BRACKET_FONT, NULL, "held rg", NULL, -1, -1);
    CHECK(sh_size_owners_read(RET(AT_FONT), NULL, materials[1], SH_SIZE_AXIS_WIDTH, &image, 1));
    sh_size_owners_collect_begin(&call, materials[2], SH_SIZE_AXIS_HEIGHT);
    sh_size_owners_note(material_image[2]);
    held_font = font; held_call = call; held_context = font.context; held_stack = stack_now();
    for (;;) fiber_switch(main_fiber);
}
/* Under the retired identifier, handed out again, nothing of the old context is found and its handles end nothing. */
static void CALLBACK reuse_fiber(void *unused)
{
    sh_size_bracket font = held_font, call = held_call;
    char *image = (char *)material_image[1], **names = NULL;
    size_t count = 0;
    (void)unused;
    probe_stack = stack_now();
    probe_ok = sh_execution_context(&test_owner, 1) == held_context && !sh_size_owners_collecting();
    probe_ok = probe_ok && !sh_size_owners_read(RET(AT_FONT), NULL, materials[1], SH_SIZE_AXIS_WIDTH, &image, 1);
    /* A SetMaterial from the old pending read's frame takes no forward read: its explicit axes stay explicit. */
    set_material(&sprites[1], 1, 8, 8, shared_frame);
    probe_ok = probe_ok && sh_size_owners_count() == 1;
    sh_size_owners_end(&font, 1);
    probe_ok = probe_ok && !sh_size_owners_collect_end(&call, 1, NULL, NULL, &names, &count) && !names && !count;
    probe_ok = probe_ok && sh_size_owners_count() == 1 && font_planned("gui/icon", "held rg") == 1;
    probe_ok = probe_ok && sh_size_owners_test_contexts() == 0 && sh_size_owners_test_records() == 0;
    for (;;) fiber_switch(main_fiber);
}
static DWORD WINAPI exit_in_bracket(void *unused)
{
    sh_size_bracket bracket;
    char *image = (char *)material_image[0];
    (void)unused;
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, &sprites[2], NULL, NULL, -1, -1);
    *(void **)(sprites[2].bytes + 0x60) = materials[0];
    if (!sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1)) return 1;
    exit_stack = stack_now();
    ExitThread(0);   /* inside SetMaterial: its frame never returns */
}
/* Suspended with a forward read pending while the index is freed; afterwards its SetMaterial takes none. */
static void CALLBACK pending_fiber(void *unused)
{
    char *image = (char *)material_image[1];
    (void)unused;
    probe_stack = stack_now();
    probe_ok = sh_size_owners_read(RET(AT_FORWARD), shared_frame, materials[1], SH_SIZE_AXIS_WIDTH, &image, 1);
    fiber_switch(main_fiber);
    set_material(&sprites[1], 1, 8, 8, shared_frame);
    probe_ok = probe_ok && sh_size_owners_count() == 0;
    for (;;) fiber_switch(main_fiber);
}
static void retirement_cases(void)
{
    unsigned long on = 0;
    long retired;
    uint64_t resume;
    HANDLE thread;
    DWORD thread_id = 0, code = 1;
    sh_size_bracket bracket;
    char *image = (char *)material_image[0];
    reset();
    sprite_init(&sprites[1], movies[1], NULL, 64, 64);
    main_fiber = ConvertThreadToFiber(NULL);
    CHECK(main_fiber != NULL);
    if (!main_fiber) return;
    /* Deleted while suspended inside its brackets: the callback runs on the deleting thread and only queues the
     * context; the next lookup by a live context retires it. */
    fiber_a = CreateFiber(0, held_fiber, NULL);
    CHECK(fiber_a != NULL);
    if (!fiber_a) return;
    fiber_switch(fiber_a);
    CHECK(sh_size_owners_test_contexts() == 1 && sh_size_owners_test_records() == 2 && sh_size_owners_collecting());
    CHECK(!escaped("env/placeholder") && !escaped("gui/other"));
    retired = sh_execution_context_test_retirements(NULL);
    DeleteFiber(fiber_a);
    fiber_a = NULL;
    stack_forget(held_stack);
    CHECK(sh_execution_context_test_retirements(&on) == retired + 1 && on == GetCurrentThreadId());
    CHECK(sh_execution_context_test_queued() == 1 && sh_size_owners_test_contexts() == 1);
    sh_execution_context(NULL, 0);
    CHECK(!sh_execution_context_test_queued());
    CHECK(sh_size_owners_test_contexts() == 0 && sh_size_owners_test_records() == 0 && !sh_size_owners_collecting());
    CHECK(escaped("env/placeholder") && escaped("gui/other") && !escaped("gui/icon"));
    CHECK(font_planned("gui/icon", "held rg") == 1 && sh_size_owners_count() == 1);
    /* Its identifier handed out again. */
    resume = sh_execution_context_test_next(held_context);
    probe_ok = 0;
    fiber_b = CreateFiber(0, reuse_fiber, NULL);
    CHECK(fiber_b != NULL);
    if (fiber_b) {
        fiber_switch(fiber_b);
        DeleteFiber(fiber_b);
        fiber_b = NULL;
        stack_forget(probe_stack);
    }
    sh_execution_context_test_next(resume);
    CHECK(probe_ok);
    /* A thread that exits inside a SetMaterial: the callback runs on that thread and queues the context; once it is
     * retired the read has escaped and the sprite keeps no record. */
    reset();
    sprite_init(&sprites[2], movies[0], NULL, 0, 0);
    retired = sh_execution_context_test_retirements(NULL);
    thread = CreateThread(NULL, 0, exit_in_bracket, NULL, 0, &thread_id);
    CHECK(thread != NULL);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CHECK(GetExitCodeThread(thread, &code) && code == 0);
        CloseHandle(thread);
    }
    stack_forget(exit_stack);
    CHECK(sh_execution_context_test_retirements(&on) == retired + 1 && on == thread_id);
    CHECK(sh_execution_context_test_queued() == 1 && sh_size_owners_test_records() == 1);
    sh_execution_context(NULL, 0);
    CHECK(sh_size_owners_test_contexts() == 0 && sh_size_owners_test_records() == 0);
    CHECK(sh_size_owners_count() == 0 && escaped("env/placeholder"));
    /* Freeing the index queues every context holding a value: this context's open SetMaterial and a suspended
     * fiber's pending read go at the next lookup, and each context starts again under a new identifier. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    sprite_init(&sprites[1], movies[1], NULL, 64, 64);
    probe_ok = 0;
    fiber_a = CreateFiber(0, pending_fiber, NULL);
    CHECK(fiber_a != NULL);
    if (fiber_a) {
        fiber_switch(fiber_a);
        sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, &sprites[0], NULL, NULL, -1, -1);
        *(void **)(sprites[0].bytes + 0x60) = materials[0];
        CHECK(sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
        CHECK(sh_size_owners_test_contexts() == 2 && sh_size_owners_test_records() == 1);
        retired = sh_execution_context_test_retirements(NULL);
        sh_execution_context_test_shutdown();
        CHECK(sh_execution_context_test_retirements(NULL) >= retired + 2 && sh_execution_context_test_queued() >= 2);
        sh_execution_context(NULL, 0);
        CHECK(sh_size_owners_test_contexts() == 0 && sh_size_owners_test_records() == 0);
        CHECK(escaped("env/placeholder") && escaped("gui/icon"));
        /* Its context retired: its end attributes nothing and the sprite keeps no record. */
        sh_size_owners_end(&bracket, 1);
        CHECK(sh_size_owners_count() == 0);
        fiber_switch(fiber_a);
        CHECK(probe_ok);
        DeleteFiber(fiber_a);
        fiber_a = NULL;
        stack_forget(probe_stack);
    }
    set_material(&sprites[0], 0, -1, -1, NULL);
    CHECK(sh_size_owners_count() == 1 && sh_size_owners_test_contexts() == 0);
    /* No FLS index left: nothing is recorded and reads stay unattributed, without disabling attribution: once an
     * index is free again, the next bracket records, with no reset in between. */
    reset();
    sprite_init(&sprites[0], movies[0], NULL, 0, 0);
    {
        enum { HOG = 16384 };
        DWORD *hog = (DWORD *)malloc(HOG * sizeof(*hog));
        size_t hogged = 0;
        CHECK(hog != NULL);
        sh_execution_context_test_shutdown();
        while (hog && hogged < HOG && (hog[hogged] = FlsAlloc(NULL)) != FLS_OUT_OF_INDEXES) hogged++;
        if (hog && hogged && hogged < HOG) {
            sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, &sprites[0], NULL, NULL, -1, -1);
            CHECK(!bracket.serial);
            CHECK(!sh_size_owners_read(RET(AT_SPRITE), NULL, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
            CHECK(!sh_size_owners_read(RET(AT_FORWARD), shared_frame, materials[0], SH_SIZE_AXIS_WIDTH, &image, 1));
            sh_size_owners_end(&bracket, 1);
            CHECK(sh_size_owners_count() == 0 && sh_size_owners_test_contexts() == 0 && sh_size_owners_test_records() == 0);
            FlsFree(hog[--hogged]);
            set_material(&sprites[0], 0, -1, -1, NULL);
            CHECK(sh_size_owners_count() == 1);
        } else printf("FLS indexes were not exhausted: the exhaustion control did not run\n");
        while (hogged) FlsFree(hog[--hogged]);
        free(hog);
    }
    set_material(&sprites[0], 0, -1, -1, NULL);
    CHECK(sh_size_owners_count() == 1);
    ConvertFiberToThread();
    main_fiber = NULL;
}

int main(void)
{
    table_cases();
    sprite_cases();
    forward_cases();
    owner_cases();
    hotbox_cases();
    widget_cases();
    collect_cases();
    index_cases();
    journal_cases();
    fold_cases();
    fiber_cases();
    chain_cases();
    retirement_cases();
    sh_size_owners_test_reset();
    sh_resource_graph_test_reset();
    printf("size owners: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
