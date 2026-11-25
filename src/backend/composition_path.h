/* A row's atomic sidecar selects its immutable set of files. Older caches put those files at the hash itself. */
#ifndef SNAPMAP_PLUS_COMPOSITION_PATH_H
#define SNAPMAP_PLUS_COMPOSITION_PATH_H

#include "modbundle.h"
#include "mp_json.h"
#include "../common/text.h"
#include <stdlib.h>
#include <string.h>

static __inline mpj *sh_composition_side(const char *base)
{
    char *path = sh_textf("%s.json", base);
    size_t n = 0;
    unsigned char *data = path ? sh_mb_read_file(path, 1u << 24, &n) : NULL;
    mpj *side = data ? mpj_parse((char *)data, n) : NULL;
    free(data);
    free(path);
    return side;
}

static __inline char *sh_composition_dir(const char *base, const mpj *side)
{
    const mpj *g = side ? mpj_get(side, "generation") : NULL;
    if (!g) return sh_text(base);
    if (g->type != MPJ_STR || strlen(g->text) != 32 || strspn(g->text, "0123456789abcdef") != 32) return NULL;
    return sh_textf("%s.g-%s", base, g->text);
}
#endif
