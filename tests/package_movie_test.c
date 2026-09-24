/* UI movie constructor pairs, requests and routing without native calls. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_movie.h"
#include "resource_rebind.h"

static unsigned char movie[16384];
static size_t used;
static void bytes(const void *p, size_t n)
{ assert(n <= sizeof(movie) - used); memcpy(movie + used, p, n); used += n; }
static void zero(size_t n)
{ assert(n <= sizeof(movie) - used); memset(movie + used, 0, n); used += n; }
static void word(unsigned n)
{ unsigned char p[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16),
    (unsigned char)(n >> 8), (unsigned char)n}; bytes(p, 4); }
static void text(const char *s)
{ unsigned n = (unsigned)strlen(s); unsigned char p[4] = {(unsigned char)n,
    (unsigned char)(n >> 8), (unsigned char)(n >> 16), (unsigned char)(n >> 24)};
    bytes(p, 4); bytes(s, n); }
static void sprite(void)
{ zero(2); word(0); word(0); word(0); word(0); }
static void font(const char *name)
{ word(5); text(name); zero(6); word(0); }
/* Dictionary: one external material, one local atlas record, then fonts. */
static void fixture(const char *const *fonts, size_t count, const char *material)
{
    used = 0; word(0x42535717); zero(22); sprite();
    word((unsigned)(count + (material ? 2 : 1)));
    if (material) { word(1); text(material); zero(32); }
    word(1); text(".atlas"); zero(32);
    for (size_t i = 0; i < count; i++) font(fonts[i]);
}

typedef struct seen { char slots[32][300], families[32][16], names[32][256]; size_t count; int absent_from; int fail_at; } seen;
static int visit(void *context, const char *slot, const char *family, const char *name)
{
    seen *s = context;
    assert(s->count < 32);
    snprintf(s->slots[s->count], sizeof(s->slots[0]), "%s", slot);
    snprintf(s->families[s->count], sizeof(s->families[0]), "%s", family);
    snprintf(s->names[s->count], sizeof(s->names[0]), "%s", name);
    s->count++;
    if (s->fail_at && (int)s->count == s->fail_at) return -1;
    return s->absent_from && (int)s->count >= s->absent_from ? 0 : 1;
}

static void names(void)
{
    sh_package_movie_names n;
    assert(sh_package_movie_names_for_path("generated/swf/hud/panel.bswf", &n) == 1);
    assert(!strcmp(n.movie, "generated/swf/hud/panel.bswf") && !strcmp(n.atlas, "generated/swf/hud/panel.bimage"));
    assert(!strcmp(n.consumer, "swf/hud/panel.swf"));
    assert(sh_package_movie_names_for_path("generated/swf/hud/panel.bimage", &n) == 1);
    assert(!strcmp(n.movie, "generated/swf/hud/panel.bswf") && !strcmp(n.consumer, "swf/hud/panel.swf"));
    /* Partitioned native names invert to an equivalent constructor spelling. */
    assert(sh_package_movie_names_for_path("generated/swf/123456789012345678901234567890/1.bswf", &n) == 1);
    assert(!strcmp(n.atlas, "generated/swf/123456789012345678901234567890/1.bimage"));
    /* The first-.bimage rule makes these pairs ambiguous; mixed case is not native. */
    assert(sh_package_movie_names_for_path("generated/swf/a.bimage/panel.bswf", &n) == -1 && !*n.movie);
    assert(sh_package_movie_names_for_path("generated/swf/Panel.bswf", &n) == -1);
    assert(sh_package_movie_names_for_path("generated/image/panel.bimage", &n) == 0);
    assert(sh_package_movie_names_for_path("generated/swf/panel.swf", &n) == 0);
    assert(sh_package_movie_owner("file", "generated/swf/hud/panel.bswf"));
    assert(!sh_package_movie_owner("image", "generated/swf/hud/panel.bswf"));
    assert(!sh_package_movie_owner("file", "generated/swf/hud/panel.bimage"));

    assert(sh_package_movie_names_for_consumer("worldgui/panel.swf", 18, &n) == 1);
    assert(!strcmp(n.movie, "generated/swf/worldgui/panel.bswf") && !strcmp(n.consumer, "worldgui/panel.swf"));
    /* The swf/ prefix test precedes case folding, as in the constructor. */
    assert(sh_package_movie_names_for_consumer("SWF/Panel.SWF", 13, &n) == 1);
    assert(!strcmp(n.movie, "generated/swf/swf/panel.bswf"));
    assert(sh_package_movie_names_for_consumer("swf/a.bimage/panel.swf", 22, &n) == -1);
    assert(sh_package_movie_names_for_consumer("", 0, &n) == 0);
    assert(sh_package_movie_names_for_consumer("swf/a\0b.swf", 11, &n) == -1);

    char native[64];
    assert(sh_package_movie_native_name("\\Fonts\\TT Supermolot", 20, native, sizeof(native)) && !strcmp(native, "fonts/tt supermolot"));
    assert(!sh_package_movie_native_name("", 0, native, sizeof(native)));
}

static void capture(void)
{
    const char *fonts[] = {"Example Font", "tt supermolot", "TT Supermolot"};
    char error[256]; seen s = {0};
    fixture(fonts, 3, "material/paint");
    assert(sh_package_movie_capture(movie, used, visit, &s, error, sizeof(error)));
    /* Two dictionary fonts, four fallback fonts (tt supermolot already listed),
     * then every fixed constructor material. */
    assert(s.count == 2 + 4 + 9);
    assert(!strcmp(s.slots[0], "movie/font/example font") && !strcmp(s.families[0], "font") && !strcmp(s.names[0], "example font"));
    assert(!strcmp(s.slots[1], "movie/font/tt supermolot"));
    assert(!strcmp(s.names[2], "square721 cn tl") && !strcmp(s.names[3], "v7gothic-medium-p"));
    assert(!strcmp(s.names[4], "dflihei-bd") && !strcmp(s.names[5], "vd-logog-medium-p"));
    assert(!strcmp(s.slots[6], "movie/material/guisolid") && !strcmp(s.families[6], "material"));
    assert(!strcmp(s.names[14], "textures/gui/load_image"));
    for (size_t i = 0; i < s.count; i++) assert(strcmp(s.names[i], "material/paint"));

    /* Absence is a native default, not a failure; read failures abort. */
    memset(&s, 0, sizeof(s)); s.absent_from = 1;
    assert(sh_package_movie_capture(movie, used, visit, &s, error, sizeof(error)) && s.count == 15);
    memset(&s, 0, sizeof(s)); s.fail_at = 7;
    assert(!sh_package_movie_capture(movie, used, visit, &s, error, sizeof(error)) && strstr(error, "availability"));

    /* A movie without text never reaches the language resolver. */
    memset(&s, 0, sizeof(s));
    fixture(NULL, 0, NULL);
    assert(sh_package_movie_capture(movie, used, visit, &s, error, sizeof(error)) && s.count == 9);
    assert(!strcmp(s.slots[0], "movie/material/guisolid"));

    /* A malformed envelope is rejected before any request is reported. */
    memset(&s, 0, sizeof(s));
    fixture(fonts, 1, NULL); movie[0] ^= 1;
    assert(!sh_package_movie_capture(movie, used, visit, &s, error, sizeof(error)) && !s.count);
}

typedef struct sites { size_t materials; } sites;
static int site(void *context, const char *family, const char *name, size_t offset, size_t length)
{
    sites *s = context;
    assert(!strcmp(family, "material") && !strcmp(name, "Material/Paint"));
    assert(offset < used && length == strlen(name) + 4);
    s->materials++; return 1;
}
static int rename_material(void *context, const char *family, const char *name, const char **replacement)
{
    (void)context; (void)name;
    assert(!strcmp(family, "material"));
    *replacement = "smp/0123456789abcdef"; return 1;
}
static void relocation(void)
{
    const char *fonts[] = {"Example Font"};
    char error[256]; sites s = {0};
    fixture(fonts, 1, "Material/Paint");
    assert(sh_package_movie_material_sites(NULL, movie, used, site, &s, error, sizeof(error)) && s.materials == 1);
    size_t length = 0, changed = 0;
    unsigned char *out = sh_resource_rebind_with(sh_package_movie_material_sites, NULL, SH_BINDING_IDSTR,
        movie, used, rename_material, NULL, &length, &changed, error, sizeof(error));
    assert(out && changed == 1 && length == used - strlen("Material/Paint") + strlen("smp/0123456789abcdef"));
    /* The font record keeps its original name for the language resolver. */
    const char needle[] = "Example Font";
    int found = 0;
    for (size_t i = 0; i + sizeof(needle) - 1 <= length; i++) if (!memcmp(out + i, needle, sizeof(needle) - 1)) found = 1;
    assert(found);
    free(out);
}

static void routing(void)
{
    sh_package_consumer_target targets[] = {
        {"movie/font/tt supermolot", "font", "smp/0123456789abcdef"},
        {"movie/material/guisolid", "material", "guisolid"},
        {"movie/material/_white", "font", "_white"}
    };
    sh_package_consumer owner = {"file", "generated/swf/hud/panel.bswf", targets, 3};
    char error[128]; const char *replacement = NULL;
    sh_package_consumers *catalog = sh_package_consumers_create(&owner, 1, error, sizeof(error));
    assert(catalog);
    const sh_package_consumer *bound = sh_package_consumers_find(catalog, "file", "generated/swf/hud/panel.bswf");
    assert(bound);
    assert(sh_package_movie_route(bound, "font", "TT Supermolot", &replacement) == 1 &&
        !strcmp(replacement, "smp/0123456789abcdef"));
    assert(sh_package_movie_route(bound, "Font", "tt supermolot", &replacement) == 1);
    assert(sh_package_movie_route(bound, "material", "guiSolid", &replacement) == 0 && !replacement);
    assert(sh_package_movie_route(bound, "material", "unrelated", &replacement) == 0);
    assert(sh_package_movie_route(bound, "image", "guiSolid", &replacement) == 0);
    assert(sh_package_movie_route(bound, "material", "_white", &replacement) == -1);
    sh_package_consumer font_owner = {"font", "tt supermolot", targets, 1};
    assert(sh_package_movie_route(&font_owner, "font", "tt supermolot", &replacement) == 0);
    assert(sh_package_movie_route(NULL, "font", "tt supermolot", &replacement) == 0);
    assert(sh_package_movie_route(bound, NULL, "tt supermolot", &replacement) == -1);
    sh_package_consumers_free(catalog);
}

int main(void)
{
    names(); capture(); relocation(); routing();
    puts("UI movie constructor tests passed");
    return 0;
}
