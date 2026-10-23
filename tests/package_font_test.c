#include "package_font.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture { int calls, required, missing, fail; } fixture;
static int capture(void *context, const char *slot, const char *name, int optional)
{
    fixture *f = context; assert(slot && name); f->calls++; f->required += !optional;
    if (f->fail == 1 || (f->fail == 2 && optional)) return -1;
    if (f->fail == 3 && optional) return 2;
    if (optional) return 0;
    assert(strstr(name, "fonts/my_font/") && !strncmp(slot, "font/material/", 14));
    return !f->missing;
}
int main(void)
{
    char error[512]; fixture f = {0};
    const unsigned char glyph[] = {'i','d','f','+',0,0,0,0,0,0,0,0,0,0};
    sh_package_font_member members[] = {{"fonts/my_font/64_df.dat", glyph, sizeof(glyph)},
        {"fonts/my_font/48.dat", glyph, sizeof(glyph)}, {"fonts/my_font/12.dat", glyph, sizeof(glyph)}};
    assert(sh_package_font_capture("my font", members, 3, capture, &f, error, sizeof(error)));
    assert(f.calls == 5 && f.required == 3);
    f = (fixture){0};
    assert(sh_package_font_capture("my font", members + 2, 1, capture, &f, error, sizeof(error)));
    assert(f.required == 1 && f.calls == 3); /* Both selected forms may be absent with 12.dat fallback. */
    f = (fixture){0}; members[0].length = 0;
    assert(sh_package_font_capture("my-font", members, 3, capture, &f, error, sizeof(error)) && f.required == 2);
    assert(!sh_package_font_capture("my font", members, 2, capture, &f, error, sizeof(error)));
    members[0].length = sizeof(glyph); f.missing = 1;
    assert(!sh_package_font_capture("my font", members, 3, capture, &f, error, sizeof(error)));
    f.missing = 0; f.fail = 1;
    assert(!sh_package_font_capture("my font", members, 3, capture, &f, error, sizeof(error)));
    f.fail = 2; assert(!sh_package_font_capture("my font", members, 3, capture, &f, error, sizeof(error)));
    f.fail = 3; assert(!sh_package_font_capture("my font", members, 3, capture, &f, error, sizeof(error)));
    f.fail = 0; members[2].length = 0;
    assert(sh_package_font_capture("my font", members, 3, capture, &f, error, sizeof(error)));
    members[2].length = sizeof(glyph); members[0].length = sizeof(glyph) - 1;
    assert(!sh_package_font_capture("my font", members, 3, capture, &f, error, sizeof(error)));
    members[0] = members[1]; assert(!sh_package_font_capture("my font", members, 3, capture, &f, error, sizeof(error)));
    assert(!sh_package_font_capture("another", members + 1, 1, capture, &f, error, sizeof(error)));
    sh_package_consumer_target targets[] = {{"font/material/48", "material", "smp/red-material"},
        {"font/material/64_df", "material", "smp/red-distance"}, {"font/snapLabel", "material", "smp/red-label"}};
    sh_package_consumer source = {"font", "smp/font-one", targets, 3};
    sh_package_consumers *view = sh_package_consumers_create(&source, 1, error, sizeof(error)); assert(view);
    const sh_package_consumer *owner = sh_package_consumers_find(view, "font", "smp/font-one");
    const char *plan[SH_PACKAGE_FONT_MATERIAL_COUNT];
    assert(sh_package_font_plan(owner, plan));
    assert(!strcmp(plan[0], "smp/red-material") && !strcmp(plan[1], "smp/red-distance"));
    assert(!plan[2] && !plan[3] && !strcmp(plan[4], "smp/red-label"));
    assert(!sh_package_font_plan(NULL, plan));
    for (size_t i = 0; i < SH_PACKAGE_FONT_MATERIAL_COUNT; i++) assert(!plan[i]);
    sh_package_consumer invalid = source;
    invalid.targets = targets + 2; invalid.count = 1;
    assert(!sh_package_font_plan(&invalid, plan)); /* Optional-only is not a usable font. */
    sh_package_consumer_target repeated[] = {targets[0], targets[0]};
    invalid.targets = repeated; invalid.count = 2;
    assert(!sh_package_font_plan(&invalid, plan));
    repeated[1].slot = "unknown";
    assert(!sh_package_font_plan(&invalid, plan));
    for (size_t i = 0; i < SH_PACKAGE_FONT_MATERIAL_COUNT; i++) assert(!plan[i]);
    const char *bound = (void *)1;
    assert(sh_package_font_route(owner, "material", "fonts/smp/font_one/64_df.tga", &bound) == 1 && !strcmp(bound, "smp/red-distance"));
    assert(sh_package_font_route(owner, "material", "snapmap/smp/font_one_snaplabel", &bound) == 1 && !strcmp(bound, "smp/red-label"));
    assert(sh_package_font_route(owner, "MATERIAL", "FONTS/SMP/FONT_ONE/48.TGA", &bound) == 1 && !strcmp(bound, "smp/red-material"));
    assert(sh_package_font_route(owner, "material", "snapmap/smp/font_one_worldText", &bound) == 2 && !bound);
    assert(sh_package_font_route(owner, "material", "fonts/smp/font_one/12.tga", &bound) == -1 && !bound);
    const char *unrelated[] = {"", "f", "fonts/", "fonts/smp/", "fonts/smp/font_on", "fonts/smp/font_one/48.tga-extra",
        "fonts/smp/font_two/48.tga", "snapmap/smp/font_two_snapLabel"};
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(*unrelated); i++)
        assert(sh_package_font_route(owner, "material", unrelated[i], &bound) == 0 && !bound);
    assert(sh_package_font_route(owner, "image", "fonts/smp/font_one/48.tga", &bound) == 0);
    assert(sh_package_font_route(NULL, "material", "fonts/smp/font_one/48.tga", &bound) == 0);
    targets[0].family = "image";
    sh_package_consumers *bad = sh_package_consumers_create(&source, 1, error, sizeof(error)); assert(bad);
    assert(sh_package_font_route(sh_package_consumers_at(bad, 0), "material", "fonts/smp/font_one/48.tga", &bound) == -1);
    sh_package_consumers_free(bad); sh_package_consumers_free(view);
    puts("font constructor capture and request routing: PASS"); return 0;
}
