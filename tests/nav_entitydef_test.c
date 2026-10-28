/* The collision size a Blocking Box inherits from its entityDef: the built-in
 * shipped value, an inherit walk through a decl reader, and the cache. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nav_entitydef.h"

void backend_log(const char *message) { (void)message; }

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); } } while (0)

static int near_f(float a, float b) { return a - b < 0.001f && b - a < 0.001f; }

static int g_reads;
static const struct { const char *name; const char *text; } DECLS[] = {
    { "test/child",  "{ inherit = \"test/middle\"; edit = { clipModelInfo = { size = { x = 40; } } } }" },
    { "test/middle", "{ inherit = \"test/base\"; edit = { clipModelInfo = { size = { z = 30; } } } }" },
    { "test/base",   "{ edit = { clipModelInfo = { type = \"CLIPMODEL_BOX\"; size = { x = 1; y = 20; z = 3; } } } }" },
    { "test/round",  "{ inherit = \"test/cyl\"; }" },
    { "test/cyl",    "{ edit = { clipModelInfo = { type = \"CLIPMODEL_CYLINDER\"; size = { x = 1; y = 2; z = 3; } } } }" },
    { "test/short",  "{ edit = { clipModelInfo = { size = { x = 1; y = 2; } } } }" },
    { "test/loop",   "{ inherit = \"test/loop2\"; edit = { clipModelInfo = { size = { x = 1; } } } }" },
    { "test/loop2",  "{ inherit = \"test/loop\"; }" },
};

static int reader(const char *name, unsigned char **out, size_t *len)
{
    size_t i;
    g_reads++;
    for (i = 0; i < sizeof DECLS / sizeof DECLS[0]; i++) if (!strcmp(name, DECLS[i].name)) {
        *len = strlen(DECLS[i].text);
        *out = (unsigned char *)malloc(*len);
        memcpy(*out, DECLS[i].text, *len);
        return 1;
    }
    return 0;
}

int main(void)
{
    float size[3] = { 0, 0, 0 };

    /* The shipped Blocking Box answers with no reader and no I/O. */
    CHECK(sh_nav_entitydef_box_size("snapmaps/volume/blocking", size));
    CHECK(near_f(size[0], 16) && near_f(size[1], 128) && near_f(size[2], 64));
    CHECK(sh_nav_entitydef_default_size("snapmaps/volume/blocking", size, NULL));
    CHECK(!sh_nav_entitydef_box_size("test/child", size));
    CHECK(!sh_nav_entitydef_box_size(NULL, size) && !sh_nav_entitydef_box_size("", size));

    sh_nav_entitydef_set_reader(reader);
    g_reads = 0;
    CHECK(sh_nav_entitydef_box_size("snapmaps/volume/blocking", size) && g_reads == 0);
    /* The nearest level that spells an axis wins. */
    CHECK(sh_nav_entitydef_box_size("test/child", size));
    CHECK(near_f(size[0], 40) && near_f(size[1], 20) && near_f(size[2], 30));
    CHECK(g_reads == 3);
    /* Cached: no second walk. */
    CHECK(sh_nav_entitydef_box_size("test/child", size) && g_reads == 3);
    /* Not a box, a missing axis, a cycle and an unknown name all answer 0,
     * and the refusal is cached too. */
    CHECK(!sh_nav_entitydef_box_size("test/round", size));
    CHECK(!sh_nav_entitydef_box_size("test/short", size));
    CHECK(!sh_nav_entitydef_box_size("test/loop", size));
    CHECK(!sh_nav_entitydef_box_size("test/none", size));
    g_reads = 0;
    CHECK(!sh_nav_entitydef_box_size("test/round", size) && g_reads == 0);
    sh_nav_entitydef_reset();
    CHECK(sh_nav_entitydef_box_size("test/child", size) && g_reads == 3);
    sh_nav_entitydef_set_reader(NULL);

    if (g_failed) { fprintf(stderr, "nav_entitydef_test: %d of %d failed\n", g_failed, g_checks); return 1; }
    printf("nav_entitydef_test: %d checks passed\n", g_checks);
    return 0;
}
