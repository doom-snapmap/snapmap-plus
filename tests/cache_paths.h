#ifndef SNAPMAP_PLUS_TEST_CACHE_PATHS_H
#define SNAPMAP_PLUS_TEST_CACHE_PATHS_H

#include "mod_paths.h"
#include "heap_out.h"

/* Fixtures sometimes use several independent stores below one temporary directory. */
static __inline const char *mt_cache_dir(const char *mods_root)
{
    static __declspec(thread) char path[HO_TEXT];
    if (!HOS(path, sh_mod_cache_dir(mods_root))) abort();
    return path;
}

#endif
