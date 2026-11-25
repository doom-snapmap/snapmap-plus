#ifndef SNAPMAP_PLUS_TEST_CACHE_PATHS_H
#define SNAPMAP_PLUS_TEST_CACHE_PATHS_H

#include "mod_paths.h"
#include "heap_out.h"
#include "composition_path.h"

/* Fixtures sometimes use several independent stores below one temporary directory. */
static __inline const char *mt_cache_dir(const char *mods_root)
{
    static __declspec(thread) char path[HO_TEXT];
    if (!HOS(path, sh_mod_cache_dir(mods_root))) abort();
    return path;
}

static __inline const char *mt_composition(const char *mods_root, const char *hash)
{
    static __declspec(thread) char path[HO_TEXT];
    char *base = sh_textf("%s\\content\\served\\%s", mt_cache_dir(mods_root), hash);
    mpj *side = base ? sh_composition_side(base) : NULL;
    int ok = base && HOS(path, sh_composition_dir(base, side));
    mpj_free(side);
    free(base);
    if (!ok) abort();
    return path;
}

#endif
