/* A local cvar registry with the supported native layout and an optional signed-in user. */
#include "../src/backend/cvar_unlock.h"
#include "../src/backend/saved_maps.h"

static unsigned long long fixture_account_id;
static unsigned long long fixture_account(void) { return fixture_account_id; }

static void bind_game_paths(const char *savepath, const char *cachepath, unsigned long long account)
{
    static unsigned char system[0x30], save[0x98], cache[0x98];
    static unsigned char *rows[] = { save, cache }, *registry = system;
    *(unsigned char ***)(system + CVARSYS_LIST_ARRAY_OFF) = rows;
    *(unsigned *)(system + CVARSYS_LIST_COUNT_OFF) = 2;
    *(const char **)(save + SH_IDCVAR_NAME) = "fs_savepath";
    *(const char **)(save + 0x10) = savepath;
    *(const char **)(cache + SH_IDCVAR_NAME) = "fs_cachepath";
    *(const char **)(cache + 0x10) = cachepath;
    fixture_account_id = account;
    sh_saved_maps_bind(&registry, fixture_account);
}

static void bind_save_session(const char *path, unsigned long long account)
{
    bind_game_paths(path, "", account);
}
