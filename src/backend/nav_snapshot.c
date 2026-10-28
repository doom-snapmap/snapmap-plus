/* Choose the map object a navigation snapshot describes. See nav_snapshot.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nav_snapshot.h"

#define NAVS_PREFIX  "maps/modules/"
#define NAVS_SUFFIX  ".decl"

int sh_nav_snapshot_module_key(const char *decl, char *out, size_t cap)
{
    size_t plen = sizeof NAVS_PREFIX - 1, slen = sizeof NAVS_SUFFIX - 1, n;
    const char *body;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!decl || strncmp(decl, NAVS_PREFIX, plen) != 0) return 0;
    body = decl + plen;
    n = strlen(body);
    if (n > slen && strcmp(body + n - slen, NAVS_SUFFIX) == 0) n -= slen;
    if (n == 0 || n >= cap) return 0;
    memcpy(out, body, n);
    out[n] = 0;
    return 1;
}

int sh_nav_snapshot_modules_match(const sh_nav_map *map, const char *names, size_t name_cap,
                                  int count, char *why, size_t why_cap)
{
    int i;
    char key[SH_NAVR_MODULE_CAP];
    if (why && why_cap) why[0] = 0;
    if (!map || count < 0 || (count && (!names || !name_cap))) return 0;
    if (map->instance_count != count) {
        if (why) _snprintf_s(why, why_cap, _TRUNCATE,
                             "it places %d module(s) and the conversion places %d",
                             map->instance_count, count);
        return 0;
    }
    for (i = 0; i < count; i++) {
        const char *name = names + (size_t)i * name_cap;
        if (!map->instances[i].module[0] ||
            !sh_nav_snapshot_module_key(name, key, sizeof key) ||
            strcmp(key, map->instances[i].module) != 0) {
            if (why) _snprintf_s(why, why_cap, _TRUNCATE,
                                 "copy %d is '%s' there and '%s' in the conversion", i,
                                 map->instances[i].module[0] ? map->instances[i].module : "?",
                                 name[0] ? name : "?");
            return 0;
        }
    }
    return 1;
}

const char *sh_nav_snapshot_source_name(sh_nav_snapshot_source source)
{
    switch (source) {
    case SH_NAV_SNAPSHOT_EDITOR:          return "the editor's map";
    case SH_NAV_SNAPSHOT_CONVERTED:       return "the map being played";
    case SH_NAV_SNAPSHOT_EDITOR_SAME:     return "the editor's map, which is the map being played";
    case SH_NAV_SNAPSHOT_EDITOR_FALLBACK: return "the editor's map, standing in for the map being played";
    default:                              return "nothing";
    }
}

/* Does `edit_map` place exactly what the conversion places? */
static int navs_matches(const sh_nav_snapshot_sources *src, const sh_nav_map *map,
                        char *why, size_t why_cap)
{
    char *names;
    int count, ok;
    if (!src->modules) {
        _snprintf_s(why, why_cap, _TRUNCATE, "the conversion's modules cannot be listed");
        return 0;
    }
    names = (char *)calloc(SH_NAVR_MAX_INSTANCES, SH_NAVR_MODULE_CAP);
    if (!names) { _snprintf_s(why, why_cap, _TRUNCATE, "out of memory"); return 0; }
    count = src->modules(src->build, names, SH_NAVR_MODULE_CAP, SH_NAVR_MAX_INSTANCES, src->ctx);
    if (count < 0) {
        _snprintf_s(why, why_cap, _TRUNCATE, "the conversion's modules could not be read");
        ok = 0;
    } else ok = sh_nav_snapshot_modules_match(map, names, SH_NAVR_MODULE_CAP, count, why, why_cap);
    free(names);
    return ok;
}

/* The inverse of nav_bake.c's bake_world_point: world.xy = origin.xy +
 * M[o] * local.xy, world.z = origin.z + local.z. Every M[o] is orthogonal, so
 * its inverse is its transpose. */
static void navs_point_to_local(const sh_nav_instance *in, float p[3], int vector)
{
    static const int xy[8][4] = {{1,0,0,1},{0,-1,1,0},{-1,0,0,-1},{0,1,-1,0},
                                 {-1,0,0,1},{0,1,1,0},{1,0,0,-1},{0,-1,-1,0}};
    int o = in->orientation;
    float dx = p[0], dy = p[1];
    if (o < 0 || o > 7) o = 0;
    if (!vector) { dx -= in->origin[0]; dy -= in->origin[1]; p[2] -= in->origin[2]; }
    p[0] = xy[o][0] * dx + xy[o][2] * dy;
    p[1] = xy[o][1] * dx + xy[o][3] * dy;
}

static void navs_regions_to_local(const sh_nav_map *m, sh_nav_region *r, int count)
{
    int i, k;
    if (!r) return;
    for (i = 0; i < count; i++) {
        const sh_nav_instance *in;
        if (r[i].instance < 0 || r[i].instance >= m->instance_count) continue;
        in = &m->instances[r[i].instance];
        if (in->unusable) continue;
        for (k = 0; k < 4; k++) navs_point_to_local(in, r[i].c[k], 0);
        navs_point_to_local(in, r[i].n, 1);
    }
}

void sh_nav_snapshot_map_to_local(sh_nav_map *m)
{
    if (!m || !m->instances) return;
    navs_regions_to_local(m, m->regions, m->region_count);
    navs_regions_to_local(m, m->obstacles, m->obstacle_count);
}

int sh_nav_snapshot_take(const sh_nav_snapshot_sources *src, sh_nav_map *out,
                         sh_nav_snapshot_result *result)
{
    sh_nav_snapshot_result local;
    char why[128];
    if (!result) result = &local;
    result->source = SH_NAV_SNAPSHOT_NONE;
    result->why[0] = 0;
    if (!src || !src->read || !out) return 0;

    /* No conversion: the editor preview and Build Navmesh read the edit map. */
    if (!src->build) {
        if (src->edit && src->read(src->edit, 0, out, src->ctx)) {
            result->source = SH_NAV_SNAPSHOT_EDITOR;
            return 1;
        }
        _snprintf_s(result->why, sizeof result->why, _TRUNCATE,
                    src->edit ? "the editor's map could not be read" : "no editor map is open");
        return 0;
    }

    if (src->read(src->build, 1, out, src->ctx)) {
        if (navs_matches(src, out, why, sizeof why)) {
            result->source = src->edit == src->build ? SH_NAV_SNAPSHOT_EDITOR_SAME
                                                     : SH_NAV_SNAPSHOT_CONVERTED;
            /* The pre-build pass has attached every entity to its module, so
             * the converted copy carries world positions (live, both launch
             * routes); the reader's tables are module-local. */
            if (result->source == SH_NAV_SNAPSHOT_CONVERTED) sh_nav_snapshot_map_to_local(out);
            return 1;
        }
        _snprintf_s(result->why, sizeof result->why, _TRUNCATE,
                    "the map being played does not match its own conversion: %s", why);
    } else _snprintf_s(result->why, sizeof result->why, _TRUNCATE,
                       "the map being played could not be read");

    /* The same object was just refused; reading it again cannot help. */
    if (!src->edit || src->edit == src->build) return 0;
    /* The map tables are heap-backed and a read never frees what `out`
     * held: release the refused conversion's tables before reading again. */
    sh_nav_map_free(out);
    if (!src->read(src->edit, 1, out, src->ctx)) {
        _snprintf_s(result->why + strlen(result->why), sizeof result->why - strlen(result->why),
                    _TRUNCATE, ", nor could the editor's map");
        return 0;
    }
    if (!navs_matches(src, out, why, sizeof why)) {
        /* An earlier editor session's map: its boxes belong to another map. */
        _snprintf_s(result->why + strlen(result->why), sizeof result->why - strlen(result->why),
                    _TRUNCATE, ", and the editor's map is a different map (%s)", why);
        sh_nav_map_free(out);
        return 0;
    }
    result->source = SH_NAV_SNAPSHOT_EDITOR_FALLBACK;
    return 1;
}
