#include "../src/backend/direct_actor_iter.h"
#include <stdio.h>
#include <string.h>

static int failed, count, generation[8], living[8], native[8], during, filtered;
static int sequence[24], visible[24], visible_count;
static void *iterator = (void *)1234, *world = (void *)222;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failed++; } } while (0)

static int who(void *p) { return (int)(uintptr_t)p; }

static int identity(void *entity, int require_ai, sh_direct_lifetime *out)
{
    int id = entity == iterator ? 0 : who(entity);
    if (id < 0 || (id == 0 && require_ai) || id > 7 || !generation[id]) return 0;
    *out = (sh_direct_lifetime){entity, (uint32_t)generation[id], id, world};
    return 1;
}

static int current(const sh_direct_lifetime *actor)
{
    return world == actor->world && (uint32_t)generation[actor->slot] == actor->generation;
}
static int live(const sh_direct_lifetime *actor) { return living[actor->slot] && current(actor); }

static int tracked(const sh_direct_lifetime *actor) { return native[actor->slot]; }
static void fire(void *it, unsigned event, void *entity)
{
    int id = who(entity);
    CHECK(it == iterator && event == 0x2b);
    sequence[count++] = id;
    if (id != filtered) visible[visible_count++] = id;
}

static void original(void *it)
{
    sequence[count++] = 0;
    for (int i = 7; i >= 1; i--) if (native[i] > 0 && living[i]) fire(it, 0x2b, (void *)(uintptr_t)i);
    if (during == 1) sh_direct_iter_reset();
    if (during == 2) generation[2]++;
    if (during == 3) living[2] = 0;
    if (during == 4) native[2] = 1;
    if (during == 5) generation[0]++;
    if (during == 6) world = (void *)555;
}

static void run(const char *expected)
{
    char result[32] = "";
    count = visible_count = 0;
    sh_direct_iter_test_run(iterator);
    for (int i = 0; i < count; i++) result[i] = (char)('0' + sequence[i]);
    CHECK(!strcmp(result, expected));
}

static void placed(void)
{
    sh_direct_iter_reset();
    sh_direct_iter_place("room0/new_62", "creatures/new");
    sh_direct_iter_place("room0/new_62", "creatures/new");
    sh_direct_iter_place("room1/new_83", "creatures/new");
    sh_direct_iter_place("room1/other_11", "creatures/other");
    sh_direct_iter_after_spawn((void *)1, "room0/new_62", "creatures/new");
    sh_direct_iter_after_spawn((void *)2, "room1/new_83", "creatures/new");
    sh_direct_iter_after_spawn((void *)4, "room1/other_11", "creatures/unrelated");
    sh_direct_iter_after_spawn((void *)5, "creatures/new", "creatures/new");
}

int main(void)
{
    sh_direct_iter_ops ops = {original, fire, identity, current, live, tracked};
    sh_direct_iter_test_bind(&ops);
    for (int i = 0; i < 8; i++) { generation[i] = i + 20; living[i] = 1; }
    native[7] = native[6] = 1;
    run("076");
    placed();
    filtered = 2;
    run("07612");
    CHECK(visible_count == 3 && visible[0] == 7 && visible[1] == 6 && visible[2] == 1);
    native[1] = 1;
    run("07612");
    native[1] = 0;
    native[2] = -1;
    run("0761");
    native[2] = 0;
    during = 4;
    run("0761");
    during = 0;
    native[2] = 0;
    during = 3;
    run("0761");
    during = 0;
    living[2] = 1;
    during = 2;
    run("0761");
    during = 0;
    sh_direct_iter_after_spawn((void *)2, "room1/new_83", "creatures/new");
    run("0761");
    during = 5;
    run("076");
    during = 6;
    run("076");
    during = 1;
    run("076");
    during = 0;
    run("076");
    placed();
    run("07612");
    sh_direct_iter_reset();
    return failed ? 1 : 0;
}
