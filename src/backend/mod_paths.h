/* The shared cache beside a profile's mods folder. */
#ifndef SNAPMAP_PLUS_MOD_PATHS_H
#define SNAPMAP_PLUS_MOD_PATHS_H

#include "modbundle.h"
#include "../common/wide_path.h"
#include <stdlib.h>

static __inline char *sh_mod_cache_dir(const char *mods_root)
{
    wchar_t *path = sh_mb_wpath(mods_root), *name, *end;
    char *cache;
    size_t capacity, used;
    int standard;
    if (!path) return NULL;
    end = path + wcslen(path);
    while (end > path && end[-1] == L'\\') *--end = 0;
    name = wcsrchr(path, L'\\');
    if (!name || !name[1] || end[-1] == L':') { free(path); return NULL; }
    standard = !_wcsicmp(name + 1, L"mods");
    if (standard) *name = 0;
    capacity = wcslen(path) * 3 + sizeof "\\cache";
    cache = (char *)malloc(capacity);
    used = cache ? sh_wpath_utf8(path, cache, capacity) : 0;
    free(path);
    if (!used) { free(cache); return NULL; }
    /* An alternate tool or test store has its own sibling cache even when it shares a parent with another store. */
    strcpy_s(cache + used, capacity - used, standard ? "\\cache" : ".cache");
    return cache;
}

#endif
