/* The real encounter wrapper and chooser, with native row and declaration fixtures. */
#define SH_ENCOUNTER_SPAWN_TEST
#include "../src/backend/encounter_spawn.c"
#include <stdio.h>

static unsigned char rows[0x30 * 3], manager[0x5920];
static void *actual;
static unsigned requested;
static int draws, failed;
static const char * const names[] = { "ai/stock", "mods/pack/a", "mods/pack/b" };
static struct { void *vtable; const char *name; } actors[3], decls[5];
static struct { char pad[0x48]; void *decl; } encounters[5];

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%u: %s\n", __LINE__, #x); failed++; } } while (0)

char *sh_modserve_shared_actor(const char *name)
{
    if (!name || strncmp(name, "mods/pack/single_", 17)) return NULL;
    return _strdup(name[17] == 'a' ? names[1] : name[17] == 'b' ? names[2] : "absent");
}

static void *draw(void *table, unsigned type)
{
    CHECK(table == manager && type == requested);
    draws++;
    return rows;
}

static void issue(void *encounter)
{
    if (encounter == &encounters[4]) {
        spawn_members(&encounters[1]);
        CHECK(actual == rows + 0x60);
    }
    actual = choose_actor(manager, requested);
}

int main(void)
{
    const unsigned shared = 1u << 19;
    const char *const singles[] = { "mods/pack/single_a", "mods/pack/single_b", "mods/pack/single_x",
                                    "single_stock", "mods/pack/mixed" };
    int i;
    g_choose = draw;
    g_members = issue;
    *(void **)(&manager[0x5908]) = rows;
    *(int *)(&manager[0x5910]) = 3;
    for (i = 0; i < 3; ++i) {
        actors[i].name = names[i];
        *(unsigned *)(rows + i * 0x30) = i ? shared : 4;
        *(void **)(rows + i * 0x30 + 8) = &actors[i];
    }
    for (i = 0; i < 5; ++i) { decls[i].name = singles[i]; encounters[i].decl = &decls[i]; }
    requested = shared;
    spawn_members(&encounters[0]); CHECK(actual == rows + 0x30);
    spawn_members(&encounters[1]); CHECK(actual == rows + 0x60);
    spawn_members(&encounters[2]); CHECK(!actual);
    CHECK(!draws && !g_encounter_name);
    requested = 4; spawn_members(&encounters[3]); CHECK(actual == rows && draws == 1);
    requested = shared; spawn_members(&encounters[4]); CHECK(actual == rows && draws == 2 && !g_encounter_name);
    requested = 4; spawn_members(&encounters[1]); CHECK(!actual && draws == 2);
    printf("encounter_spawn_test: %s\n", failed ? "FAIL" : "PASS");
    return failed != 0;
}
