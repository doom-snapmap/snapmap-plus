/* A placed single keeps its actor when several registered demons share a native spawn class. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "encounter_spawn.h"
#include "modserve.h"
#include "patch.h"

typedef void (*members_fn)(void *encounter);
typedef void *(*choose_fn)(void *manager, unsigned type);
static members_fn g_members;
static choose_fn g_choose;
static __declspec(thread) const char *g_encounter_name;

static const char *decl_name(const void *decl)
{
    return decl ? *(const char *const *)((const char *)decl + 8) : NULL;
}

static void spawn_members(void *encounter)
{
    const char *previous = g_encounter_name;
    g_encounter_name = decl_name(*(void **)((char *)encounter + 0x48));
    __try {
        g_members(encounter);
    } __finally {
        g_encounter_name = previous;
    }
}

static void *choose_actor(void *manager, unsigned type)
{
    char *actor = sh_modserve_shared_actor(g_encounter_name);
    unsigned char *rows;
    int n, i;
    void *match = NULL;
    if (!actor) return g_choose(manager, type);
    rows = *(unsigned char **)((char *)manager + 0x5908);
    n = *(int *)((char *)manager + 0x5910);
    for (i = 0; rows && i < n; ++i) {
        unsigned char *row = rows + (size_t)i * 0x30;
        const char *name;
        if (!(*(unsigned *)row & type)) continue;
        name = decl_name(*(void **)(row + 8));
        if (name && !_stricmp(name, actor)) { match = row; break; }
    }
    free(actor);
    return match;
}

#ifndef SH_ENCOUNTER_SPAWN_TEST
int sh_encounter_spawn_install(const sig_result *results, size_t count)
{
    const sig_result *members = sig_lookup(results, count, "SnapEncounterSpawnMembers");
    const sig_result *choose = sig_lookup(results, count, "SnapEncounterChooseActor");
    if (g_members && g_choose) return 1;
    g_members = (members_fn)sh_prepare_detour_sig(members, (void *)spawn_members, 19);
    g_choose = (choose_fn)sh_prepare_detour_sig(choose, (void *)choose_actor, 21);
    if (g_members && g_choose && sh_commit_detour((void *)g_choose) == B2_PATCH_OK &&
        sh_commit_detour((void *)g_members) == B2_PATCH_OK) return 1;
    if (g_members) sh_uninstall_detour((void *)g_members);
    if (g_choose) sh_uninstall_detour((void *)g_choose);
    g_members = NULL;
    g_choose = NULL;
    return 0;
}
#endif
