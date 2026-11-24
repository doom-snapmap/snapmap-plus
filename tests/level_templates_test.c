/* level_templates_test.c -- the rule half of level_templates (src/backend/level_templates_rule.c): the template walk's
 * list keeps its count and every entry except those the scope hides, which are cleared; a null entry or an unreadable
 * name stays. Also checks the E8 call decoding the install uses to find the list copy. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "level_templates.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

typedef struct fake_res { void *vtable; const char *name; } fake_res;

static const char *name_of(const void *item) { return ((const fake_res *)item)->name; }
static int hides(const char *name) { return !strncmp(name, "mods/undecl00/", 14); }

int main(void)
{
    fake_res stock = { NULL, "ai/demon/imp" }, mod1 = { NULL, "mods/undecl00/ai/demon/broken" },
             own = { NULL, "mods/declar00/ai/demon/tyrant" }, mod2 = { NULL, "mods/undecl00/snapmaps/enc" },
             unnamed = { NULL, NULL };
    void *items[6];
    int hidden;
    const char *first = NULL;

    /* Hidden entries are cleared in place; the list keeps its count (the walk compares it with the manager's). */
    items[0] = &stock; items[1] = &mod1; items[2] = NULL; items[3] = &own; items[4] = &mod2; items[5] = &unnamed;
    hidden = sh_level_templates_filter(items, 6, name_of, hides, &first);
    CHECK(hidden == 2 && first == mod1.name);
    CHECK(items[0] == &stock && items[1] == NULL && items[2] == NULL && items[3] == &own && items[4] == NULL &&
          items[5] == &unnamed);

    /* nothing hidden: the list and *first unchanged */
    first = "unset";
    CHECK(sh_level_templates_filter(items, 6, name_of, hides, &first) == 0 && !strcmp(first, "unset"));
    CHECK(items[0] == &stock && items[3] == &own && items[5] == &unnamed);
    CHECK(sh_level_templates_filter(NULL, 3, name_of, hides, NULL) == 0);
    CHECK(sh_level_templates_filter(items, -1, name_of, hides, NULL) == 0);

    /* E8 rel32 decoding, forward and backward */
    {
        uint8_t code[16] = { 0x90, 0xE8, 0x10, 0x00, 0x00, 0x00, 0x90 };
        CHECK(sh_level_templates_call_target(code + 1) == (uintptr_t)(code + 6 + 0x10));
        code[2] = 0xF0; code[3] = 0xFF; code[4] = 0xFF; code[5] = 0xFF;
        CHECK(sh_level_templates_call_target(code + 1) == (uintptr_t)(code + 6 - 0x10));
        CHECK(sh_level_templates_call_target(code) == 0 && sh_level_templates_call_target(NULL) == 0);
    }

    printf(g_fail ? "level_templates_test: FAIL\n" : "level_templates_test: all checks passed\n");
    return g_fail;
}
