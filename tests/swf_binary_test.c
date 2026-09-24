#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "swf_binary.h"

static unsigned char movie[32768];
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
static const unsigned char opaque[] = {0x96, 0xff, 0, 0x80, 'x', 0, 0xf8};
static void sprite(void)
{
    zero(2); word(2); word(0); word(1);
    word(1); word(0); text("material/not_a_dependency");
    word(1); word(12); word(sizeof(opaque)); bytes(opaque, sizeof(opaque));
    word(1); word(sizeof(opaque)); bytes(opaque, sizeof(opaque));
}
static void vertices(void) { word(2); zero(16); }
static void indices(void) { word(2); zero(4); }
static size_t gradient_at, count_at;
static void shape(void)
{
    zero(32); word(1); zero(58); gradient_at = used;
    const unsigned char n = 2; bytes(&n, 1); zero(20); zero(6);
    vertices(); vertices(); indices();
    word(1); zero(12); vertices(); vertices(); indices();
}
static void fixture(const char *material, const char *font)
{
    used = 0; word(0x42535717); zero(22); sprite(); count_at = used; word(9);
    word(0);
    word(1); text(material); zero(32);
    word(1); text("."); zero(32);
    word(2); shape();
    word(3); shape();
    word(4); sprite();
    word(5); text(font); zero(6); word(1); zero(4); vertices(); indices();
    word(6); zero(40); word(1); zero(15); word(1); zero(8);
    word(7); zero(42); text("local variable"); text("raw \xff\x80 text");
}
typedef struct observed { size_t references, commands; int refuse; } observed;
static int reference(void *context, const char *family, const char *name,
    size_t offset, size_t length)
{
    observed *s = context;
    assert((!strcmp(family, "material") && !strcmp(name, "material/paint")) ||
        (!strcmp(family, "font") && !strcmp(name, "Example Font")));
    assert(length == strlen(name) + 4 && offset < used);
    s->references++;
    return !s->refuse;
}
static int command(void *context, size_t dictionary, int initialization,
    uint32_t tag, const unsigned char *p, size_t offset, size_t length)
{
    observed *s = context;
    assert(dictionary == SIZE_MAX || dictionary == 5);
    assert(tag == (initialization ? 0u : 12u));
    assert(length == sizeof(opaque) && !memcmp(p, opaque, length));
    assert(offset < used);
    s->commands++;
    return !s->refuse;
}
static int resolver(void *context, const char *family, const char *name, const char **replacement)
{
    int mode = *(int *)context;
    (void)name;
    if (mode <= 0) return mode;
    *replacement = !strcmp(family, "material") ? "material/private/longer/paint" : "Short";
    return 1;
}
static void naming(void)
{
    static const struct { const char *name, *movie, *atlas; } cases[] = {
        {"worldgui/panel.swf", "generated/swf/worldgui/panel.bswf", "generated/swf/worldgui/panel.bimage"},
        {"swf/worldgui/panel", "generated/swf/worldgui/panel.bswf", "generated/swf/worldgui/panel.bimage"},
        {"SWF/Panel.SWF", "generated/swf/swf/panel.bswf", "generated/swf/swf/panel.bimage"},
        {"swf\\Panel.swf", "generated/swf/swf/panel.bswf", "generated/swf/swf/panel.bimage"},
        {"swf/1234567890123456789012345678901.swf",
            "generated/swf/123456789012345678901234567890/1.bswf",
            "generated/swf/123456789012345678901234567890/1.bimage"},
        {"swf/a.bimage/panel.swf", "generated/swf/a.bswf", "generated/swf/a.bimage/panel.bimage"},
        {"swf/panel.v2.swf", "generated/swf/panel.v2.bswf", "generated/swf/panel.v2.bimage"}
    };
    sh_swf_binary_paths paths;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        assert(sh_swf_binary_paths_for(cases[i].name, &paths));
        assert(!strcmp(paths.movie, cases[i].movie) && !strcmp(paths.atlas, cases[i].atlas));
    }
    char oversized[1024]; memset(oversized, 'a', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = 0;
    assert(!sh_swf_binary_paths_for(oversized, &paths) && !*paths.movie && !*paths.atlas);
    assert(!sh_swf_binary_paths_for(NULL, &paths) && !*paths.movie && !*paths.atlas);
    assert(!sh_swf_binary_paths_for("", &paths) && !*paths.movie && !*paths.atlas);
}
int main(void)
{
    naming();
    char error[256];
    observed seen = {0};
    sh_swf_binary_observer observer = {&seen, reference, command};
    fixture("material/paint", "Example Font");
    size_t n = used, gradient = gradient_at, count = count_at;
    unsigned char original[sizeof(movie)]; memcpy(original, movie, used);
    assert(sh_swf_binary_read(movie, n, &observer, error, sizeof(error)));
    assert(!error[0] && seen.references == 2 && seen.commands == 4);
    for (size_t i = 0; i < n; i++) {
        assert(!sh_swf_binary_read(movie, i, NULL, error, sizeof(error)));
        assert(error[0]);
    }
    zero(1); assert(!sh_swf_binary_read(movie, used, NULL, error, sizeof(error)));
    movie[3]++; assert(!sh_swf_binary_read(movie, n, NULL, error, sizeof(error))); movie[3]--;
    movie[gradient] = 17;
    assert(!sh_swf_binary_read(movie, n, NULL, error, sizeof(error)));
    assert(strstr(error, "gradient"));
    memcpy(movie, original, n); movie[count] = 0xff;
    assert(!sh_swf_binary_read(movie, n, NULL, error, sizeof(error)));
    memcpy(movie, original, n); movie[count + 7] = 8;
    assert(!sh_swf_binary_read(movie, n, NULL, error, sizeof(error)));
    assert(strstr(error, "dictionary"));
    memcpy(movie, original, n);
    seen.refuse = 1;
    assert(!sh_swf_binary_read(movie, n, &observer, error, sizeof(error)));
    assert(strstr(error, "visitor"));
    assert(!sh_swf_binary_read(NULL, 0, NULL, error, sizeof(error)));
    /* The relocation transaction cannot publish a partially validated movie. */
    for (int mode = -1; mode <= 1; mode++) {
        size_t output_length = 99, changed = 99;
        unsigned char *out = sh_resource_rebind_with(sh_swf_binary_reference_sites, NULL,
            SH_BINDING_IDSTR, original, n, resolver, &mode, &output_length, &changed, error, sizeof(error));
        if (mode < 0) assert(!out && !output_length && !changed);
        else if (!mode) assert(out && output_length == n && !changed && !memcmp(out, original, n));
        else {
            fixture("material/private/longer/paint", "Short");
            assert(out && output_length == used && changed == 2 && !memcmp(out, movie, used));
            assert(sh_swf_binary_read(out, output_length, NULL, error, sizeof(error)));
        }
        free(out);
    }
    int mode = 1; size_t output_length = 99, changed = 99;
    assert(!sh_resource_rebind_with(sh_swf_binary_reference_sites, NULL, SH_BINDING_IDSTR,
        original, n - 1, resolver, &mode, &output_length, &changed, error, sizeof(error)));
    assert(!output_length && !changed);
    puts("UI movie binary tests passed"); return 0;
}
