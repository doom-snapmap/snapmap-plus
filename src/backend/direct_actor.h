#ifndef SH_DIRECT_ACTOR_H
#define SH_DIRECT_ACTOR_H
#include <string.h>

/* Derived editor rows must also resolve in a stock-only installation. */
#define SH_DIRECT_ACTOR_ID "custom/direct_actors/"
static inline const char *sh_direct_actor_key(const char *request)
{
    if (!request) return NULL;
    if (!_strnicmp(request, "generated/decls/", 16)) request += 16;
    else if (!_strnicmp(request, "decltree/", 9)) request += 9;
    else return NULL;
    if (_strnicmp(request, "snapeditorentitydef/" SH_DIRECT_ACTOR_ID,
                  sizeof("snapeditorentitydef/" SH_DIRECT_ACTOR_ID) - 1)) return NULL;
    return request + sizeof("snapeditorentitydef/") - 1;
}
#endif
