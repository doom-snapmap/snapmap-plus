/* The collision size a Blocking Box inherits from its entityDef: the built-in
 * shipped value, and no answer for anything navigation does not read. */
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

int main(void)
{
    float size[3] = { 0, 0, 0 };

    CHECK(sh_nav_entitydef_box_size("snapmaps/volume/blocking", size));
    CHECK(near_f(size[0], 16) && near_f(size[1], 128) && near_f(size[2], 64));
    CHECK(sh_nav_entitydef_box_size("SnapMaps/Volume/Blocking", size));
    CHECK(sh_nav_entitydef_default_size("snapmaps/volume/blocking", size, NULL));
    CHECK(!sh_nav_entitydef_box_size("test/child", size));
    CHECK(!sh_nav_entitydef_box_size(NULL, size) && !sh_nav_entitydef_box_size("", size));

    if (g_failed) { fprintf(stderr, "nav_entitydef_test: %d of %d failed\n", g_failed, g_checks); return 1; }
    printf("nav_entitydef_test: %d checks passed\n", g_checks);
    return 0;
}
